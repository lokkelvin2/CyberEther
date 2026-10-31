#include <jetstream/domains/io/soapy/block.hh>
#include <jetstream/detail/block_impl.hh>

#include <jetstream/domains/io/soapy/module.hh>
#include "module_impl.hh"
#include "library/soapysdr.hh"

#include <cmath>
#include <limits>

namespace Jetstream::Blocks {

struct SoapyImpl : public Block::Impl, public DynamicConfig<Blocks::Soapy> {
    Result validate() override;
    Result configure() override;
    Result define() override;
    Result create() override;
    Result reconfigure() override;

 protected:
    std::shared_ptr<Modules::Soapy> moduleConfig = std::make_shared<Modules::Soapy>();
    Modules::SoapyImpl* moduleImpl = nullptr;

 private:
    Result updateOptionFormats();
    Result updateManualGainFormat();
    std::string referenceLabel() const;
    std::string ppsLabel() const;
    static bool ShowsReferenceFrequency(const std::string& clockSource);
    static Parser::Map optionsFormat(const std::string& selected,
                                     const std::vector<std::string>& values = {});
    Parser::Map manualGainFormat(const std::optional<Modules::SoapyAbi::Range>& range = std::nullopt);
};

Result SoapyImpl::validate() {
    const auto& config = *candidate();

    if (!std::isfinite(config.frequencyStep) || config.frequencyStep <= 0.0f) {
        JST_ERROR("[BLOCK_SOAPY] Frequency step must be finite and positive.");
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyImpl::configure() {
    if (Modules::SoapyRuntime::Acquire()) {
        JST_CHECK(Modules::SoapyDiscovery::LoadDriverLibrary(modulePath));
    }

    moduleConfig->modulePath = modulePath;
    moduleConfig->deviceString = deviceString;
    moduleConfig->streamString = streamString;
    moduleConfig->channels = channels;
    moduleConfig->antenna = antenna;
    moduleConfig->secondAntenna = secondAntenna;
    moduleConfig->frequency = frequency;
    moduleConfig->sampleRate = sampleRate;
    moduleConfig->automaticGain = automaticGain;
    moduleConfig->manualGain = manualGain;
    moduleConfig->biasTee = biasTee;
    moduleConfig->clockSource = clockSource;
    moduleConfig->referenceFrequency = referenceFrequency;
    moduleConfig->timeSource = timeSource;
    moduleConfig->ppsSync = ppsSync;
    moduleConfig->numberOfBatches = numberOfBatches;
    moduleConfig->numberOfTimeSamples = numberOfTimeSamples;
    moduleConfig->bufferMultiplier = bufferMultiplier;

    return Result::SUCCESS;
}

Result SoapyImpl::define() {
    const auto& config = *candidate();

    JST_CHECK(defineInterfaceOutput("signal",
                                    "Output",
                                    "The output buffer containing samples from the SDR device."));

    if (config.channels > 1) {
        JST_CHECK(defineInterfaceOutput(Modules::SoapyImpl::OutputName(1),
                                        "Output 2",
                                        "Samples from the second channel, taken at the same instants as Output."));
    }

    Parser::Sequence deviceOptions{Parser::Map{{"label", "None"}, {"value", ""}}};
    bool selectionListed = config.deviceString.empty();
    for (const auto& [label, device] : Modules::SoapyDiscovery::List()) {
        auto args = device;
        args.erase("label");
        const auto value = Modules::SoapyDiscovery::FormatArguments(args);
        selectionListed = selectionListed || value == config.deviceString;
        deviceOptions.emplace_back(Parser::Map{{"label", label}, {"value", value}});
    }
    if (!selectionListed) {
        deviceOptions.emplace_back(Parser::Map{{"label", "Configured device"}, {"value", config.deviceString}});
    }

    JST_CHECK(defineInterfaceConfig("deviceString",
                                    "Device",
                                    "Select a device to receive samples. Choose None to disconnect.",
                                    {{"type", "dropdown"}, {"options", std::move(deviceOptions)}}));

    JST_CHECK(defineInterfaceConfig("channels",
                                    "Channels",
                                    "Receive channels streamed together. 2 adds Output 2. Changes restart the receiver.",
                                    {{"type", "uint"}, {"unit", "channels"}}));

    JST_CHECK(defineInterfaceConfig("antenna",
                                    config.channels > 1 ? "Antenna 1" : "Antenna",
                                    "Receive antenna port of the first channel. Changes restart the receiver.",
                                    optionsFormat(config.antenna)));

    if (config.channels > 1) {
        JST_CHECK(defineInterfaceConfig("secondAntenna",
                                        "Antenna 2",
                                        "Receive antenna port of the second channel. Changes restart the receiver.",
                                        optionsFormat(config.secondAntenna)));
    }

    JST_CHECK(defineInterfaceConfig("frequency",
                                    "Frequency",
                                    "Tuner frequency.",
                                    {{"type", "float"}, {"unit", "MHz"}, {"scale", 1.0e6f},
                                     {"precision", 3}, {"step_config",
                                        std::isfinite(config.frequencyStep) && config.frequencyStep > 0.0f
                                            ? std::string("frequencyStep") : std::string{}}}));

    JST_CHECK(defineInterfaceConfig("sampleRate",
                                    "Sample Rate",
                                    "Sampling rate.",
                                    {{"type", "float"}, {"unit", "MHz"}, {"scale", 1.0e6f}, {"precision", 3}}));

    JST_CHECK(defineInterfaceConfig("automaticGain",
                                    "Automatic Gain",
                                    "Enable automatic gain control.",
                                    {{"type", "bool"}}));

    if (!config.automaticGain) {
        JST_CHECK(defineInterfaceConfig("manualGain",
                                        "Manual Gain",
                                        "Receive gain applied while automatic gain is off.",
                                        manualGainFormat()));
    }

    const auto module = moduleHandle("soapy");
    const bool knownWithoutBiasTee = module && module->state() == Module::State::CREATED &&
                                     config.deviceString == deviceString &&
                                     !module->getImpl<Modules::SoapyImpl>()->supportsBiasTee();
    if (!knownWithoutBiasTee) {
        JST_CHECK(defineInterfaceConfig("biasTee",
                                        "Bias-T",
                                        "Enable antenna power through the antenna cable.",
                                        {{"type", "bool"}}));
    }

    JST_CHECK(defineInterfaceConfig("clockSource",
                                    "Clock Source",
                                    "Frequency reference, such as the external REF input. Changes restart the receiver.",
                                    optionsFormat(config.clockSource)));

    if (ShowsReferenceFrequency(config.clockSource)) {
        JST_CHECK(defineInterfaceConfig("referenceFrequency",
                                        "Reference Frequency",
                                        "External reference frequency. 0 keeps the driver's default. "
                                        "Ettus B2xx radios need 10 MHz.",
                                        {{"type", "float"}, {"unit", "MHz"}, {"scale", 1.0e6f}, {"precision", 3}}));
    }

    JST_CHECK(defineInterfaceConfig("timeSource",
                                    "Time Source",
                                    "Source of the PPS that marks whole seconds, such as the external PPS input. "
                                    "Changes restart the receiver.",
                                    optionsFormat(config.timeSource)));

    if (!config.timeSource.empty()) {
        JST_CHECK(defineInterfaceConfig("ppsSync",
                                        "PPS Sync",
                                        "Set the radio time to UTC at a PPS edge and start streaming on the next edge.",
                                        {{"type", "bool"}}));
    }

    JST_CHECK(defineInterfaceConfig("numberOfBatches",
                                    "Batches",
                                    "Number of batches in output buffer.",
                                    {{"type", "uint"}, {"unit", "batches"}}));

    JST_CHECK(defineInterfaceConfig("numberOfTimeSamples",
                                    "Samples",
                                    "Samples per batch and the FFT size downstream.",
                                    {{"type", "uint"}, {"unit", "samples"}}));

    JST_CHECK(defineInterfaceConfig("bufferMultiplier",
                                    "Buffer Multiplier",
                                    "Internal buffer size multiplier.",
                                    {{"type", "uint"}, {"unit", "x"}}));

    JST_CHECK(defineInterfaceMetric("bufferFill",
                                    "Buffer Fill",
                                    "How full the internal buffer is. Near 100% means the chain is falling behind.",
                                    {{"type", "progressbar"}, {"warning_above", 0.5f}, {"error_above", 0.75f}},
        [this]() -> std::any {
            if (!moduleImpl) {
                return std::pair<std::string, F32>{"0.0%", 0.0f};
            }
            const F32 bufferFill = moduleImpl->getBufferFill();
            return std::pair<std::string, F32>{jst::fmt::format("{:.1f}%", bufferFill * 100.0f),
                                               bufferFill};
        }));

    JST_CHECK(defineInterfaceMetric("bufferLoss",
                                    "Buffer Loss",
                                    "Share of samples overwritten before the chain read them since the stream started. "
                                    "Overflows the radio reported (OVF) show next to it.",
                                    {{"type", "progressbar"}, {"warning_above", 0.0f}, {"error_above", 0.01f}},
        [this]() -> std::any {
            const F64 loss = moduleImpl ? moduleImpl->getBufferLoss() : 0.0;
            const U64 deviceOverflows = moduleImpl ? moduleImpl->getDeviceOverflows() : 0;
            auto label = loss > 0.0 && loss < 0.0001
                ? std::string("<0.01%") : jst::fmt::format("{:.2f}%", loss * 100.0);
            if (deviceOverflows > 0) {
                label = jst::fmt::format("{} ({} OVF)", label, deviceOverflows);
            }
            return std::pair<std::string, F32>{label, static_cast<F32>(loss)};
        }));

    JST_CHECK(defineInterfaceMetric("throughput",
                                    "Throughput",
                                    "Rate the chain reads next to the rate the radio sends.",
                                    {{"type", "progressbar"}, {"warning_below", 0.95f}, {"error_below", 0.8f}},
        [this]() -> std::any {
            const auto [actual, expected] = moduleImpl ? moduleImpl->getThroughput()
                                                       : std::pair<F32, F32>{0.0f, 0.0f};
            const F32 fraction = actual > 0.0f && expected > 0.0f ? actual / expected
                                                                  : std::numeric_limits<F32>::quiet_NaN();
            return std::pair<std::string, F32>{
                jst::fmt::format("{:.1f} / {:.1f} MB/s", actual, expected), fraction};
        }));

    JST_CHECK(defineInterfaceMetric("reference",
                                    "Reference",
                                    "Lock to the external frequency reference.",
                                    {{"type", "label"}},
        [this]() -> std::any { return referenceLabel(); }));

    JST_CHECK(defineInterfaceMetric("pps",
                                    "PPS",
                                    "PPS edges seen on the time source, with the radio time of the last one.",
                                    {{"type", "label"}},
        [this]() -> std::any { return ppsLabel(); }));

    return Result::SUCCESS;
}

Result SoapyImpl::create() {
    JST_CHECK(moduleCreate("soapy", moduleConfig, {}));
    for (U64 channel = 0; channel < channels; ++channel) {
        const auto output = Modules::SoapyImpl::OutputName(channel);
        JST_CHECK(moduleExposeOutput(output, {"soapy", output}));
    }

    moduleImpl = moduleHandle("soapy")->getImpl<Modules::SoapyImpl>();
    JST_CHECK(updateOptionFormats());
    JST_CHECK(updateManualGainFormat());

    return Result::SUCCESS;
}

Result SoapyImpl::reconfigure() {
    JST_CHECK(updateOptionFormats());
    JST_CHECK(updateManualGainFormat());

    return Result::SUCCESS;
}

Result SoapyImpl::updateOptionFormats() {
    JST_CHECK(updateInterfaceConfigFormat("antenna", optionsFormat(antenna, moduleImpl->listAntennas(0))));
    if (channels > 1) {
        JST_CHECK(updateInterfaceConfigFormat("secondAntenna",
                                              optionsFormat(secondAntenna, moduleImpl->listAntennas(1))));
    }
    JST_CHECK(updateInterfaceConfigFormat("clockSource",
                                          optionsFormat(clockSource, moduleImpl->listClockSources())));
    JST_CHECK(updateInterfaceConfigFormat("timeSource",
                                          optionsFormat(timeSource, moduleImpl->listTimeSources())));

    return Result::SUCCESS;
}

Result SoapyImpl::updateManualGainFormat() {
    if (moduleImpl->getGainRange()) {
        manualGain = moduleImpl->manualGain;
        moduleConfig->manualGain = manualGain;
    }

    if (!automaticGain) {
        JST_CHECK(updateInterfaceConfigFormat("manualGain",
                                              manualGainFormat(moduleImpl->getGainRange())));
    }

    return Result::SUCCESS;
}

std::string SoapyImpl::referenceLabel() const {
    if (clockSource.empty()) {
        return "Default";
    }
    if (!ShowsReferenceFrequency(clockSource)) {
        return "Internal";
    }
    const auto locked = moduleImpl ? moduleImpl->getReferenceLock() : std::nullopt;
    if (!locked) {
        return "N/A";
    }
    return *locked ? "Locked" : "Not locked";
}

std::string SoapyImpl::ppsLabel() const {
    if (timeSource.empty()) {
        return "Not used";
    }
    const auto status = moduleImpl ? moduleImpl->getPpsStatus() : Modules::SoapyImpl::PpsStatus{};
    if (!status.monitored) {
        return "N/A";
    }
    if (!status.present) {
        return "No pulse";
    }
    const I64 seconds = status.lastTimeNs / 1'000'000'000;
    if (ppsSync) {
        // Synced radios count UTC seconds, so show the time of day.
        const I64 secondOfDay = ((seconds % 86400) + 86400) % 86400;
        return jst::fmt::format("Present, {:02}:{:02}:{:02} UTC",
                                secondOfDay / 3600, (secondOfDay / 60) % 60, secondOfDay % 60);
    }
    return jst::fmt::format("Present, t = {} s", seconds);
}

bool SoapyImpl::ShowsReferenceFrequency(const std::string& clockSource) {
    return !clockSource.empty() && clockSource != "internal";
}

Parser::Map SoapyImpl::optionsFormat(const std::string& selected,
                                     const std::vector<std::string>& values) {
    Parser::Sequence options{Parser::Map{{"label", "Default"}, {"value", ""}}};
    bool selectionListed = selected.empty();
    for (const auto& value : values) {
        selectionListed = selectionListed || value == selected;
        options.emplace_back(Parser::Map{{"label", value}, {"value", value}});
    }
    if (!selectionListed) {
        options.emplace_back(Parser::Map{{"label", selected}, {"value", selected}});
    }
    return {{"type", "dropdown"}, {"options", std::move(options)}};
}

Parser::Map SoapyImpl::manualGainFormat(const std::optional<Modules::SoapyAbi::Range>& range) {
    const F32 minimum = range ? static_cast<F32>(range->minimum) : 0.0f;
    const F32 maximum = range ? static_cast<F32>(range->maximum) : 60.0f;
    return {{"type", "range"}, {"min", minimum}, {"max", maximum}, {"unit", "dB"}};
}

JST_REGISTER_BLOCK(SoapyImpl, {"soapy"});

}  // namespace Jetstream::Blocks
