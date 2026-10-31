#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <limits>
#include <thread>

#include "environment.hh"
#include "jetstream/domains/io/soapy/module.hh"
#include "jetstream/module_context.hh"
#include "jetstream/module_interface.hh"
#include "jetstream/registry.hh"
#include "jetstream/runtime_context_native_cpu.hh"
#include "jetstream/scheduler_context.hh"
#include "library/soapysdr.hh"
#include "module_impl.hh"
#include "scripted_soapy.hh"

using namespace Jetstream;

namespace {

struct RuntimeReset {
    RuntimeReset() {
        Modules::SoapyRuntime::Reset();
        Modules::SoapyDiscovery::ClearCache();
    }

    ~RuntimeReset() {
        Modules::SoapyRuntime::Reset();
        Modules::SoapyDiscovery::ClearCache();
    }
};

std::shared_ptr<Module> BuildSoapyModule() {
    std::shared_ptr<Module> module;
    REQUIRE(Registry::BuildModule("soapy", DeviceType::CPU, RuntimeType::NATIVE,
                                  "generic", module) == Result::SUCCESS);
    return module;
}

struct ModuleCleanup {
    std::shared_ptr<Module> module;
    ~ModuleCleanup() { static_cast<void>(module->destroy()); }
};

template<typename Predicate>
bool WaitFor(Predicate predicate, const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

}  // namespace

TEST_CASE("Soapy validates configuration before accessing the runtime",
          "[modules][soapy][validation]") {
    const TestSupport::ScopedEnvironment environment("CYBERETHER_SOAPYSDR_LIBRARY");
    REQUIRE(environment.set("/__cyberether_missing_soapy__/library"));
    const RuntimeReset reset;
    Modules::Soapy config;
    config.deviceString = "driver=saved,serial=123";
    SECTION("invalid sample rate") {
        config.sampleRate = 0.0f;
    }
    SECTION("non-finite frequency") {
        config.frequency = std::numeric_limits<F32>::quiet_NaN();
    }
    SECTION("non-finite manual gain") {
        config.manualGain = std::numeric_limits<F32>::infinity();
    }
    SECTION("overflowing buffer dimensions") {
        config.numberOfBatches = std::numeric_limits<U64>::max();
    }
    SECTION("no channels") {
        config.channels = 0;
    }
    SECTION("more channels than supported") {
        config.channels = Modules::SoapyReceiver::MaxChannels + 1;
    }
    SECTION("negative reference frequency") {
        config.referenceFrequency = -10.0e6f;
    }

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::ERROR);
    REQUIRE(module->state() == Module::State::ERRORED);
    REQUIRE(module->interface()->outputs().empty());
    REQUIRE(module->outputs().empty());
    REQUIRE(static_cast<const Modules::Soapy&>(module->config()).deviceString.empty());
    REQUIRE(module->destroy() == Result::SUCCESS);
}

TEST_CASE("Soapy missing runtime retains saved module configuration",
          "[modules][soapy][runtime]") {
    const TestSupport::ScopedEnvironment environment("CYBERETHER_SOAPYSDR_LIBRARY");
    REQUIRE(environment.set("/__cyberether_missing_soapy__/library"));
    const RuntimeReset reset;
    Modules::Soapy config;
    config.modulePath = "saved-device-module";
    config.deviceString = "driver=saved,serial=123";
    config.streamString = "bufflen=8192";
    Parser::Map saved;
    REQUIRE(config.serialize(saved) == Result::SUCCESS);

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::INCOMPLETE);
    REQUIRE(module->state() == Module::State::INCOMPLETE);
    REQUIRE(module->interface()->outputs() == std::vector<std::string>{"signal"});
    REQUIRE(module->outputs().empty());
    Parser::Map retained;
    REQUIRE(module->config(retained) == Result::SUCCESS);
    REQUIRE(retained == saved);
    REQUIRE_FALSE(Modules::SoapyRuntime::Acquire());
    REQUIRE_FALSE(Modules::SoapyRuntime::Availability().message.empty());
    REQUIRE(module->destroy() == Result::SUCCESS);
}

TEST_CASE("Soapy can load an installed system runtime without opening a radio",
          "[modules][soapy][runtime][system]") {
    const TestSupport::ScopedEnvironment environment("CYBERETHER_SOAPYSDR_LIBRARY");
    REQUIRE(environment.set(std::nullopt));
    const RuntimeReset reset;
    const auto library = Modules::SoapyRuntime::Acquire();
    if (!library) {
        SKIP(Modules::SoapyRuntime::Availability().message);
    }
    REQUIRE(Modules::SoapyRuntime::Availability().available);
    REQUIRE(std::string(library->api().getABIVersion()) == Modules::SoapyAbi::Version);
    REQUIRE_FALSE(library->path().empty());
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy gain control respects device capabilities during startup and reconfiguration",
                 "[modules][soapy][devices][agc]") {
    const bool supported = GENERATE(true, false);
    const bool enabled = GENERATE(true, false);
    CAPTURE(supported, enabled);
    TestSoapy::State().automaticGainSupported = supported;

    const auto module = BuildSoapyModule();
    struct Cleanup {
        std::shared_ptr<Module> module;
        ~Cleanup() { static_cast<void>(module->destroy()); }
    } cleanup{module};
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.automaticGain = enabled;
    config.manualGain = 30.0f;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);
    const auto outputId = module->outputs().at("signal").tensor.id();
    U64 expectedWrites = supported ? 1 : 0;
    REQUIRE(TestSoapy::State().gainModeWrites == expectedWrites);
    REQUIRE(TestSoapy::State().settings().automaticGain == (supported && enabled));

    for (const bool gain : {!enabled, enabled}) {
        REQUIRE(module->reconfigure({{"automaticGain", gain}}) == Result::SUCCESS);
        REQUIRE(module->state() == Module::State::CREATED);
        REQUIRE(module->outputs().at("signal").tensor.id() == outputId);
        REQUIRE(static_cast<const Modules::Soapy&>(module->config()).automaticGain == gain);
        if (supported) {
            ++expectedWrites;
        }
        REQUIRE(TestSoapy::State().gainModeWrites == expectedWrites);
        REQUIRE(TestSoapy::State().settings().automaticGain == (supported && gain));
    }
    REQUIRE(TestSoapy::State().concurrentChanges == 0);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy reports gain control failures on devices advertising AGC",
                 "[modules][soapy][devices][agc]") {
    TestSoapy::State().fail("automaticGain");
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::ERROR);
    REQUIRE(TestSoapy::State().gainModeWrites == 1);
    REQUIRE(module->state() == Module::State::DESTROYED);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy live edits finish synchronously and failed transactions remain retryable",
                 "[modules][soapy][reconfigure][control]") {
    const auto module = BuildSoapyModule();
    struct Cleanup {
        std::shared_ptr<Module> module;
        ~Cleanup() { static_cast<void>(module->destroy()); }
    } cleanup{module};
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.frequency = 100.0e6f;
    config.sampleRate = 2.0e6f;
    config.automaticGain = false;
    config.manualGain = 30.0f;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);

    Parser::Map edit;
    std::string failure;
    SECTION("frequency") {
        edit = {{"frequency", 300.0e6f}};
        failure = "frequency";
    }
    SECTION("sample rate") {
        edit = {{"sampleRate", 4.0e6f}};
        failure = "sampleRate";
    }
    SECTION("automatic gain") {
        edit = {{"automaticGain", true}};
        failure = "automaticGain";
    }
    SECTION("manual gain after changing the rate and frequency") {
        edit = {{"sampleRate", 4.0e6f}, {"frequency", 300.0e6f}, {"manualGain", 40.0f}};
        failure = "manualGain";
    }
    const auto checkApplied = [&] {
        const auto applied = TestSoapy::State().settings();
        REQUIRE(applied.frequency == config.frequency);
        REQUIRE(applied.sampleRate == config.sampleRate);
        REQUIRE(applied.automaticGain == config.automaticGain);
        REQUIRE(applied.gain == config.manualGain);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (TestSoapy::State().activeReads == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(TestSoapy::State().activeReads > 0);
    TestSoapy::State().fail(failure);
    REQUIRE(module->reconfigure(edit) == Result::ERROR);
    checkApplied();
    Parser::Map before;
    Parser::Map retained;
    REQUIRE(config.serialize(before) == Result::SUCCESS);
    REQUIRE(module->config(retained) == Result::SUCCESS);
    REQUIRE(retained == before);

    REQUIRE(module->reconfigure(edit) == Result::SUCCESS);
    REQUIRE(config.deserialize(edit) == Result::SUCCESS);
    checkApplied();
    REQUIRE(TestSoapy::State().concurrentChanges == 0);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy streams two channels with their own antennas and aligned samples",
                 "[modules][soapy][channels]") {
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().deliverSamples = true;
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.channels = 2;
    config.antenna = "RX2";
    config.secondAntenna = "TX/RX";
    config.numberOfBatches = 2;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    const ModuleCleanup cleanup{module};
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);
    REQUIRE(module->interface()->outputs() == std::vector<std::string>{"signal", "signal2"});
    REQUIRE(TestSoapy::State().streamChannels == std::vector<std::size_t>{0, 1});
    REQUIRE(TestSoapy::State().settings().antennas == std::array<std::string, 2>{"RX2", "TX/RX"});
    // UHD rejects an untimed start on several channels, so they start together shortly after now.
    REQUIRE((TestSoapy::State().activationFlags & Modules::SoapyAbi::HasTime) != 0);
    REQUIRE(TestSoapy::State().activationTimeNs == TestSoapy::NowNs + 100'000'000);

    const auto runtime = std::dynamic_pointer_cast<NativeCpuRuntimeContext>(module->context()->runtime());
    REQUIRE(runtime != nullptr);
    const auto& first = module->outputs().at("signal").tensor;
    const auto& second = module->outputs().at("signal2").tensor;
    REQUIRE(first.size() == 16);
    REQUIRE(second.size() == 16);

    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(WaitFor([&] { return module->context()->scheduler()->hasPendingCompute() == Result::SUCCESS; }));
        REQUIRE(runtime->computeSubmit() == Result::SUCCESS);
        for (U64 i = 0; i < first.size(); ++i) {
            CAPTURE(frame, i);
            REQUIRE(first.data<CF32>()[i].imag() == 0.0f);
            REQUIRE(second.data<CF32>()[i].imag() == 1.0f);
            REQUIRE(first.data<CF32>()[i].real() == second.data<CF32>()[i].real());
        }
    }
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy refuses more channels than the device has",
                 "[modules][soapy][channels]") {
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().deviceChannels = 1;
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.channels = 2;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::ERROR);
    REQUIRE(TestSoapy::State().streamChannels.empty());
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy applies the clock source and reference frequency and reports reference lock",
                 "[modules][soapy][clock]") {
    const bool adjustable = GENERATE(true, false);
    CAPTURE(adjustable);
    if (adjustable) {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().referenceRates = {{10.0e6, 10.0e6, 0.0}, {30.72e6, 30.72e6, 0.0}};
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.clockSource = "external";
    config.referenceFrequency = 10.0e6f;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    const ModuleCleanup cleanup{module};
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);
    const auto applied = TestSoapy::State().settings();
    REQUIRE(applied.clockSource == "external");
    // Without reported rates the driver cannot change the reference, so it is left alone.
    REQUIRE(applied.referenceRate == (adjustable ? 10.0e6 : 0.0));
    REQUIRE(applied.timeSource.empty());

    auto* soapy = module->getImpl<Modules::SoapyImpl>();
    REQUIRE(WaitFor([&] { return soapy->getReferenceLock() == std::optional<bool>{true}; }));
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().referenceLocked = false;
    }
    REQUIRE(WaitFor([&] { return soapy->getReferenceLock() == std::optional<bool>{false}; }));
    REQUIRE_FALSE(soapy->getPpsStatus().monitored);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy rejects reference frequencies the driver does not list",
                 "[modules][soapy][clock]") {
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().referenceRates = {{10.0e6, 10.0e6, 0.0}};
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.clockSource = "external";
    config.referenceFrequency = 20.0e6f;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::ERROR);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy PPS sync sets the device time at a PPS edge and starts on the next one",
                 "[modules][soapy][pps]") {
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.timeSource = "external";
    config.ppsSync = true;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    const ModuleCleanup cleanup{module};
    const auto before = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);
    const auto after = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    REQUIRE(TestSoapy::State().settings().timeSource == "external");
    REQUIRE(TestSoapy::State().timeSetAfterSetup);
    REQUIRE((TestSoapy::State().activationFlags & Modules::SoapyAbi::HasTime) != 0);
    const long long start = TestSoapy::State().activationTimeNs;
    REQUIRE(start % 1'000'000'000 == 0);
    // The device time was set to the host second after the edge, and streaming starts one edge later.
    REQUIRE(start / 1'000'000'000 >= before + 2);
    REQUIRE(start / 1'000'000'000 <= after + 2);

    auto* soapy = module->getImpl<Modules::SoapyImpl>();
    REQUIRE(WaitFor([&] {
        const auto status = soapy->getPpsStatus();
        return status.monitored && status.present;
    }));
    REQUIRE(soapy->getPpsStatus().lastTimeNs >= start - 1'000'000'000);
    REQUIRE_FALSE(soapy->getReferenceLock());
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy PPS sync fails when no PPS edge arrives",
                 "[modules][soapy][pps]") {
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().ppsPresent = false;
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.timeSource = "external";
    config.ppsSync = true;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    REQUIRE(module->create("test", config, {}) == Result::ERROR);
    REQUIRE(TestSoapy::State().activations == 0);
}

TEST_CASE_METHOD(TestSoapy::Runtime,
                 "Soapy ignores a saved PPS sync without a time source",
                 "[modules][soapy][pps]") {
    {
        std::lock_guard lock(TestSoapy::State().mutex);
        TestSoapy::State().ppsPresent = false;
    }
    Modules::Soapy config;
    config.deviceString = "driver=scripted";
    config.ppsSync = true;
    config.numberOfBatches = 1;
    config.numberOfTimeSamples = 8;

    const auto module = BuildSoapyModule();
    const ModuleCleanup cleanup{module};
    REQUIRE(module->create("test", config, {}) == Result::SUCCESS);
    REQUIRE((TestSoapy::State().activationFlags & Modules::SoapyAbi::HasTime) == 0);
    REQUIRE_FALSE(module->getImpl<Modules::SoapyImpl>()->getPpsStatus().monitored);
}
