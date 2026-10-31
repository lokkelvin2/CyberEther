#ifndef JETSTREAM_TESTS_SUPPORT_SCRIPTED_SOAPY_HH
#define JETSTREAM_TESTS_SUPPORT_SCRIPTED_SOAPY_HH

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "domains/io/soapy/library/soapysdr.hh"

namespace TestSoapy {

using namespace Jetstream;
using namespace Jetstream::Modules;

struct Settings {
    F64 frequency = 100.0e6;
    F64 sampleRate = 2.0e6;
    bool automaticGain = false;
    F64 gain = 30.0;
    std::array<std::string, 2> antennas;
    std::string clockSource;
    std::string timeSource;
    F64 referenceRate = 0.0;
};

struct Script {
    std::mutex mutex;
    Settings applied;
    std::string failure;
    bool automaticGainSupported = true;
    std::atomic<U64> gainModeWrites{0};
    std::atomic<unsigned> activeReads{0};
    std::atomic<U64> concurrentChanges{0};

    std::size_t deviceChannels = 2;
    std::vector<SoapyAbi::Range> referenceRates;
    std::vector<std::size_t> streamChannels;
    U64 activations = 0;
    int activationFlags = 0;
    long long activationTimeNs = 0;
    // UHD can change the master clock in setupStream, which invalidates a time set earlier.
    bool timeSetAfterSetup = false;

    // Sample delivery: channel c gets (n, c) for running sample number n, so aligned
    // channels hold equal real parts.
    bool deliverSamples = false;
    U64 delivered = 0;

    // PPS: every second read of the PPS time sees a new edge, which takes a pending time
    // set through setHardwareTime or else advances by one second.
    bool ppsPresent = true;
    bool referenceLocked = true;
    U64 ppsReads = 0;
    long long ppsTimeNs = 0;
    std::optional<long long> pendingPpsTimeNs;

    void reset() {
        std::lock_guard lock(mutex);
        applied = {};
        failure.clear();
        automaticGainSupported = true;
        gainModeWrites = 0;
        activeReads = 0;
        concurrentChanges = 0;
        deviceChannels = 2;
        referenceRates.clear();
        streamChannels.clear();
        activations = 0;
        activationFlags = 0;
        activationTimeNs = 0;
        timeSetAfterSetup = false;
        deliverSamples = false;
        delivered = 0;
        ppsPresent = true;
        referenceLocked = true;
        ppsReads = 0;
        ppsTimeNs = 0;
        pendingPpsTimeNs.reset();
    }

    Settings settings() {
        std::lock_guard lock(mutex);
        return applied;
    }

    void fail(const std::string& call) {
        std::lock_guard lock(mutex);
        failure = call;
    }

    template<typename T>
    int set(const char* name, T Settings::* field, const T value) {
        std::lock_guard lock(mutex);
        if (activeReads > 0) {
            ++concurrentChanges;
        }
        if (failure == name) {
            failure.clear();
            return -1;
        }
        applied.*field = value;
        return 0;
    }

    long long readPpsTime() {
        std::lock_guard lock(mutex);
        if (ppsPresent && ++ppsReads % 2 == 0) {
            ppsTimeNs = pendingPpsTimeNs.value_or(ppsTimeNs + 1'000'000'000);
            pendingPpsTimeNs.reset();
        }
        return ppsTimeNs;
    }
};

// Device time reported by getHardwareTime("").
constexpr long long NowNs = 7'000'000'000;

inline Script& State() {
    static Script script;
    return script;
}

inline char** Strings(std::initializer_list<const char*> values, std::size_t* count) {
    static thread_local std::vector<char*> list;
    list.clear();
    for (const char* value : values) {
        list.push_back(const_cast<char*>(value));
    }
    *count = list.size();
    return list.data();
}

inline SoapyApi Api() {
    SoapyApi api;
    api.lastError = [] { return "injected failure"; };
    api.lastStatus = [] { return 0; };
    api.free = [](void*) {};
    api.stringsClear = [](char***, std::size_t) {};
    api.kwargsFromString = [](const char*) { return SoapyAbi::Kwargs{}; };
    api.kwargsClear = [](SoapyAbi::Kwargs*) {};
    api.kwargsListClear = [](SoapyAbi::Kwargs*, std::size_t) {};
    api.argInfoListClear = [](SoapyAbi::ArgInfo*, std::size_t) {};
    api.enumerate = [](const SoapyAbi::Kwargs*, std::size_t* count) {
        static SoapyAbi::Kwargs entry{};
        *count = 1;
        return &entry;
    };
    api.make = [](const SoapyAbi::Kwargs*) {
        return reinterpret_cast<SoapyAbi::Device*>(&State());
    };
    api.unmake = [](SoapyAbi::Device*) { return 0; };
    api.getNumChannels = [](const SoapyAbi::Device*, int) {
        std::lock_guard lock(State().mutex);
        return State().deviceChannels;
    };
    api.getSampleRateRange = [](const SoapyAbi::Device*, int, std::size_t, std::size_t* count) {
        static SoapyAbi::Range range{1.0e6, 10.0e6, 0.0};
        *count = 1;
        return &range;
    };
    api.getFrequencyRange = [](const SoapyAbi::Device*, int, std::size_t, std::size_t* count) {
        static SoapyAbi::Range range{10.0e6, 2000.0e6, 0.0};
        *count = 1;
        return &range;
    };
    api.getSettingInfo = [](const SoapyAbi::Device*, std::size_t* count) -> SoapyAbi::ArgInfo* {
        *count = 0;
        return nullptr;
    };
    api.listAntennas = [](const SoapyAbi::Device*, int, std::size_t, std::size_t* count) {
        return Strings({"TX/RX", "RX2"}, count);
    };
    api.setAntenna = [](SoapyAbi::Device*, int, std::size_t channel, const char* name) {
        std::lock_guard lock(State().mutex);
        State().applied.antennas.at(channel) = name;
        return 0;
    };
    api.getGainRange = [](const SoapyAbi::Device*, int, std::size_t) {
        return SoapyAbi::Range{0.0, 60.0, 0.0};
    };
    api.setFrequency = [](SoapyAbi::Device*, int, std::size_t, double value, const SoapyAbi::Kwargs*) {
        return State().set("frequency", &Settings::frequency, value);
    };
    api.setSampleRate = [](SoapyAbi::Device*, int, std::size_t, double value) {
        return State().set("sampleRate", &Settings::sampleRate, value);
    };
    api.hasGainMode = [](const SoapyAbi::Device*, int, std::size_t) {
        return State().automaticGainSupported;
    };
    api.setGainMode = [](SoapyAbi::Device*, int, std::size_t, bool value) {
        ++State().gainModeWrites;
        if (!State().automaticGainSupported) {
            return -1;
        }
        return State().set("automaticGain", &Settings::automaticGain, value);
    };
    api.setGain = [](SoapyAbi::Device*, int, std::size_t, double value) {
        return State().set("manualGain", &Settings::gain, value);
    };
    api.getReferenceClockRates = [](const SoapyAbi::Device*, std::size_t* count) -> SoapyAbi::Range* {
        static thread_local std::vector<SoapyAbi::Range> rates;
        std::lock_guard lock(State().mutex);
        rates = State().referenceRates;
        *count = rates.size();
        return rates.empty() ? nullptr : rates.data();
    };
    api.setReferenceClockRate = [](SoapyAbi::Device*, double rate) {
        return State().set("referenceRate", &Settings::referenceRate, rate);
    };
    api.listClockSources = [](const SoapyAbi::Device*, std::size_t* count) {
        return Strings({"internal", "external"}, count);
    };
    api.setClockSource = [](SoapyAbi::Device*, const char* source) {
        return State().set("clockSource", &Settings::clockSource, std::string(source));
    };
    api.listTimeSources = [](const SoapyAbi::Device*, std::size_t* count) {
        return Strings({"internal", "external"}, count);
    };
    api.setTimeSource = [](SoapyAbi::Device*, const char* source) {
        return State().set("timeSource", &Settings::timeSource, std::string(source));
    };
    api.hasHardwareTime = [](const SoapyAbi::Device*, const char* what) {
        return std::strcmp(what, "PPS") == 0 || std::strcmp(what, "") == 0;
    };
    api.getHardwareTime = [](const SoapyAbi::Device*, const char* what) -> long long {
        return std::strcmp(what, "PPS") == 0 ? State().readPpsTime() : NowNs;
    };
    api.setHardwareTime = [](SoapyAbi::Device*, long long timeNs, const char*) {
        std::lock_guard lock(State().mutex);
        State().pendingPpsTimeNs = timeNs;
        State().timeSetAfterSetup = !State().streamChannels.empty();
        return 0;
    };
    api.listSensors = [](const SoapyAbi::Device*, std::size_t* count) {
        return Strings({"ref_locked"}, count);
    };
    api.readSensor = [](const SoapyAbi::Device*, const char*) {
        std::lock_guard lock(State().mutex);
        return const_cast<char*>(State().referenceLocked ? "true" : "false");
    };
    api.setupStream = [](SoapyAbi::Device*, int, const char*, const std::size_t* channels,
                          std::size_t count, const SoapyAbi::Kwargs*) {
        std::lock_guard lock(State().mutex);
        State().streamChannels.assign(channels, channels + count);
        return reinterpret_cast<SoapyAbi::Stream*>(&State());
    };
    api.activateStream = [](SoapyAbi::Device*, SoapyAbi::Stream*, int flags, long long timeNs, std::size_t) {
        std::lock_guard lock(State().mutex);
        ++State().activations;
        State().activationFlags = flags;
        State().activationTimeNs = timeNs;
        return 0;
    };
    api.deactivateStream = [](SoapyAbi::Device*, SoapyAbi::Stream*, int, long long) { return 0; };
    api.closeStream = [](SoapyAbi::Device*, SoapyAbi::Stream*) { return 0; };
    api.readStream = [](SoapyAbi::Device*, SoapyAbi::Stream*, void* const* buffers, std::size_t count, int*,
                         long long*, long) {
        ++State().activeReads;
        std::this_thread::sleep_for(std::chrono::milliseconds(State().deliverSamples ? 1 : 10));
        int result = SoapyAbi::Timeout;
        {
            std::lock_guard lock(State().mutex);
            if (State().deliverSamples) {
                for (std::size_t channel = 0; channel < State().streamChannels.size(); ++channel) {
                    auto* samples = static_cast<CF32*>(buffers[channel]);
                    for (std::size_t i = 0; i < count; ++i) {
                        samples[i] = CF32(static_cast<F32>(State().delivered + i), static_cast<F32>(channel));
                    }
                }
                State().delivered += count;
                result = static_cast<int>(count);
            }
        }
        --State().activeReads;
        return result;
    };
    return api;
}

struct Runtime {
    Runtime() {
        State().reset();
        Modules::SoapyRuntime::Override(Api());
        Modules::SoapyDiscovery::ClearCache();
    }

    ~Runtime() {
        Modules::SoapyRuntime::Reset();
        Modules::SoapyDiscovery::ClearCache();
    }
};

}

#endif
