#include "module_impl.hh"
#include "library/soapysdr.hh"

#include <algorithm>
#include <exception>
#include <new>

#include <jetstream/memory/axis.hh>
#include <jetstream/scheduler_context.hh>
#include <jetstream/tools/numeric.hh>

namespace Jetstream::Modules {

SoapyImpl::~SoapyImpl() {
    stopReceiver();
}

std::string SoapyImpl::OutputName(const U64 channel) {
    return channel == 0 ? "signal" : jst::fmt::format("signal{}", channel + 1);
}

Result SoapyImpl::validate() {
    const auto& config = *candidate();
    validatedOutputSizeBytes = 0;
    validatedInternalElements = 0;
    validatedInternalSizeBytes = 0;

    if (config.channels == 0 || config.channels > MaxChannels) {
        JST_ERROR("[MODULE_SOAPY] Channels must be between 1 and {}.", MaxChannels);
        return Result::ERROR;
    }

    if (!std::isfinite(config.frequency)) {
        JST_ERROR("[MODULE_SOAPY] Frequency must be finite.");
        return Result::ERROR;
    }

    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0f) {
        JST_ERROR("[MODULE_SOAPY] Sample rate must be finite and positive.");
        return Result::ERROR;
    }

    if (!std::isfinite(config.manualGain)) {
        JST_ERROR("[MODULE_SOAPY] Manual gain must be finite.");
        return Result::ERROR;
    }

    if (!std::isfinite(config.referenceFrequency) || config.referenceFrequency < 0.0f) {
        JST_ERROR("[MODULE_SOAPY] Reference frequency must be finite and not negative.");
        return Result::ERROR;
    }

    if (config.numberOfBatches == 0) {
        JST_ERROR("[MODULE_SOAPY] Number of batches cannot be zero.");
        return Result::ERROR;
    }

    if (config.numberOfTimeSamples == 0) {
        JST_ERROR("[MODULE_SOAPY] Number of time samples cannot be zero.");
        return Result::ERROR;
    }

    if (config.bufferMultiplier == 0) {
        JST_ERROR("[MODULE_SOAPY] Buffer multiplier cannot be zero.");
        return Result::ERROR;
    }

    U64 outputElements = 0;
    if (!detail::CheckedMultiply(config.numberOfBatches,
                                 config.numberOfTimeSamples,
                                 outputElements)) {
        JST_ERROR("[MODULE_SOAPY] Output buffer dimensions are too large.");
        return Result::ERROR;
    }

    U64 outputSizeBytes = 0;
    if (!detail::CheckedMultiply(outputElements,
                                 static_cast<U64>(sizeof(CF32)),
                                 outputSizeBytes)) {
        JST_ERROR("[MODULE_SOAPY] Output buffer layout is too large.");
        return Result::ERROR;
    }

    U64 internalElements = 0;
    U64 internalSizeBytes = 0;
    if (!detail::CheckedMultiply(outputElements,
                                 config.bufferMultiplier,
                                 internalElements) ||
        !detail::CheckedMultiply(internalElements,
                                 static_cast<U64>(sizeof(CF32)),
                                 internalSizeBytes)) {
        JST_ERROR("[MODULE_SOAPY] Internal buffer layout is too large.");
        return Result::ERROR;
    }

    validatedOutputSizeBytes = outputSizeBytes;
    validatedInternalElements = internalElements;
    validatedInternalSizeBytes = internalSizeBytes;

    return Result::SUCCESS;
}

Result SoapyImpl::define() {
    for (U64 channel = 0; channel < channels; ++channel) {
        JST_CHECK(defineInterfaceOutput(OutputName(channel)));
    }

    return Result::SUCCESS;
}

Result SoapyImpl::create() {
    if (deviceString.empty()) {
        JST_ERROR("[MODULE_SOAPY] No device selected.");
        return Result::INCOMPLETE;
    }

    if (!SoapyRuntime::Acquire()) {
        JST_ERROR("[MODULE_SOAPY] {}", SoapyRuntime::Availability().message);
        return Result::INCOMPLETE;
    }
    JST_CHECK(SoapyDiscovery::LoadDriverLibrary(modulePath));

    errored = false;
    streaming = false;
    bufferFill.publish(0.0f);
    deviceOverflows.publish(0);
    throughput.publish({0.0f, 0.0f});
    referenceLock.publish(std::nullopt);
    ppsStatus.publish({});
    ppsMonitor = {};

    JST_CHECK(receiverDevice.open(deviceString));
    JST_CHECK(receiverDevice.validateSettings(sampleRate, frequency));
    JST_CHECK(allocateBuffers());
    JST_CHECK(configureDevice());

    for (U64 channel = 0; channel < channels; ++channel) {
        const auto output = OutputName(channel);
        outputs()[output].produced(name(), output, buffers[channel]);
    }

    generation = 0;
    resetStream(frequency, sampleRate);
    publishedMetadata = {};
    JST_CHECK(publishMetadata(streams[0].metadata()));

    JST_CHECK(startReceiver());

    return Result::SUCCESS;
}

Result SoapyImpl::allocateBuffers() {
    try {
        for (U64 channel = 0; channel < channels; ++channel) {
            JST_CHECK(buffers[channel].create(device(), DataType::CF32, {numberOfBatches, numberOfTimeSamples}));
            JST_CHECK(SetSignalAxes(buffers[channel], {
                .sample = Index{1},
                .batch = Index{0},
            }));
            JST_CHECK(streams[channel].resize(validatedInternalElements));
        }
    } catch (const std::bad_array_new_length&) {
        JST_ERROR("[MODULE_SOAPY] Internal buffer dimensions are too large.");
        return Result::ERROR;
    } catch (const std::bad_alloc&) {
        JST_ERROR("[MODULE_SOAPY] Failed to allocate the internal buffer.");
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyImpl::configureDevice() {
    const Tuning desired{
        .frequency = frequency,
        .sampleRate = sampleRate,
        .automaticGain = automaticGain,
        .gain = manualGain,
    };
    JST_CHECK(receiverDevice.setChannels(channels));
    // References and time sources settle before tuning, as UHD recommends.
    JST_CHECK(receiverDevice.setClockSource(clockSource));
    JST_CHECK(receiverDevice.setReferenceClockRate(referenceFrequency));
    JST_CHECK(receiverDevice.setTimeSource(timeSource));
    JST_CHECK(receiverDevice.setAntenna(0, antenna));
    if (channels > 1) {
        JST_CHECK(receiverDevice.setAntenna(1, secondAntenna));
    }
    JST_CHECK(applyTuning(desired, true));
    manualGain = tuned.gain;
    JST_CHECK(receiverDevice.setBiasTee(biasTee));

    // Set up the stream before syncing: it can change the master clock and with it the device time.
    JST_CHECK(receiverDevice.setupStream(streamString));

    std::optional<I64> startTimeNs;
    // PPS Sync is only offered with a Time Source, so a hidden saved value stays inactive.
    if (ppsSync && !timeSource.empty()) {
        I64 edgeTimeNs = 0;
        JST_CHECK(receiverDevice.syncTimeToPps(edgeTimeNs));
        // Start on the next PPS edge, so radios synced to the same PPS start on the same edge.
        startTimeNs = edgeTimeNs + 1'000'000'000;
        JST_INFO("[MODULE_SOAPY] Device time synced to PPS. Streaming starts at {} s.",
                 *startTimeNs / 1'000'000'000);
    } else if (channels > 1) {
        // UHD refuses to start several channels "now" because they would not start on the same
        // sample, so start them together shortly after the current device time.
        if (const auto now = receiverDevice.readHardwareTime()) {
            startTimeNs = *now + 100'000'000;
        }
    }
    JST_CHECK(receiverDevice.activateStream(startTimeNs));
    return Result::SUCCESS;
}

Result SoapyImpl::startReceiver() {
    streaming = true;
    try {
        producer = std::thread([this] {
            Result result;
            try {
                result = soapyThreadLoop();
            } catch (...) {
                result = Result::ERROR;
            }
            if (result != Result::SUCCESS && result != Result::RELOAD) {
                errored = true;
                JST_FATAL("[MODULE_SOAPY] Device thread crashed.");
            }
            Scheduler::Context::NotifyPendingCompute();
        });
    } catch (const std::exception& e) {
        streaming = false;
        JST_ERROR("[MODULE_SOAPY] Failed to start device thread: {}", e.what());
        return Result::ERROR;
    } catch (...) {
        streaming = false;
        JST_ERROR("[MODULE_SOAPY] Failed to start device thread.");
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

void SoapyImpl::stopReceiver() {
    streaming = false;

    if (producer.joinable()) {
        producer.join();
    }
}

Result SoapyImpl::destroy() {
    stopReceiver();
    receiverDevice.reset();

    bufferFill.publish(0.0f);
    throughput.publish({0.0f, 0.0f});
    referenceLock.publish(std::nullopt);
    ppsStatus.publish({});

    return Result::SUCCESS;
}

Result SoapyImpl::reconfigure() {
    const auto& newConfig = *candidate();

    if (errored || newConfig.modulePath != modulePath ||
        newConfig.deviceString != deviceString ||
        newConfig.streamString != streamString ||
        newConfig.channels != channels ||
        newConfig.antenna != antenna ||
        newConfig.secondAntenna != secondAntenna ||
        newConfig.biasTee != biasTee ||
        newConfig.clockSource != clockSource ||
        newConfig.referenceFrequency != referenceFrequency ||
        newConfig.timeSource != timeSource ||
        newConfig.ppsSync != ppsSync ||
        newConfig.numberOfBatches != numberOfBatches ||
        newConfig.numberOfTimeSamples != numberOfTimeSamples ||
        newConfig.bufferMultiplier != bufferMultiplier) {
        return Result::RECREATE;
    }

    if ((newConfig.frequency != frequency || newConfig.sampleRate != sampleRate) &&
        receiverDevice.validateSettings(newConfig.sampleRate, newConfig.frequency) != Result::SUCCESS) {
        return Result::WARNING;
    }

    return access.change([&] {
        const Tuning desired{
            .frequency = newConfig.frequency,
            .sampleRate = newConfig.sampleRate,
            .automaticGain = newConfig.automaticGain,
            .gain = newConfig.manualGain,
        };
        const auto previous = tuned;
        const auto result = applyTuning(desired);
        if (result != Result::SUCCESS) {
            if (applyTuning(previous, true) != Result::SUCCESS) {
                errored = true;
                JST_ERROR("[MODULE_SOAPY] Failed to restore the previous receiver settings.");
            }
            return result;
        }

        frequency = newConfig.frequency;
        sampleRate = newConfig.sampleRate;
        automaticGain = newConfig.automaticGain;
        manualGain = tuned.gain;
        return Result::SUCCESS;
    });
}

void SoapyImpl::resetStream(const F32 appliedFrequency, const F32 appliedSampleRate) {
    std::lock_guard lock(channelMutex);
    ++generation;
    for (U64 channel = 0; channel < channels; ++channel) {
        streams[channel].reset({
            .frequency = appliedFrequency,
            .sampleRate = appliedSampleRate,
            .generation = generation,
        });
    }
}

Result SoapyImpl::publishMetadata(const Radio::StreamBuffer::Metadata& metadata) {
    if (metadata == publishedMetadata) {
        return Result::SUCCESS;
    }
    for (U64 channel = 0; channel < channels; ++channel) {
        JST_CHECK(buffers[channel].setAttribute("frequency", static_cast<F32>(metadata.frequency)));
        JST_CHECK(buffers[channel].setAttribute("sampleRate", static_cast<F32>(metadata.sampleRate)));
    }
    publishedMetadata = metadata;
    return Result::SUCCESS;
}

Result SoapyImpl::emitFrame() {
    std::lock_guard lock(channelMutex);
    Radio::StreamBuffer::Metadata metadata;
    for (U64 channel = 0; channel < channels; ++channel) {
        Radio::StreamBuffer::Metadata channelMetadata;
        const auto result = streams[channel].pop(buffers[channel].data<CF32>(), buffers[channel].size(),
                                                 channelMetadata);
        // The rings only differ right after a reset, and then the next frame is aligned again.
        if (result == Result::INCOMPLETE) {
            return Result::YIELD;
        }
        JST_CHECK(result);
        if (channel == 0) {
            metadata = channelMetadata;
        }
    }
    return publishMetadata(metadata);
}

bool SoapyImpl::monitorsReference() const {
    return !clockSource.empty() && clockSource != "internal" && receiverDevice.supportsReferenceLock();
}

bool SoapyImpl::monitorsPps() const {
    return !timeSource.empty() && receiverDevice.supportsPpsTime();
}

void SoapyImpl::pollTiming() {
    if (monitorsReference()) {
        const auto locked = receiverDevice.readReferenceLock();
        if (locked && locked != referenceLock.get()) {
            // Debug level: a missing reference makes the lock flip every second.
            JST_DEBUG("[MODULE_SOAPY] Reference on '{}' is {}.", clockSource, *locked ? "locked" : "not locked");
        }
        referenceLock.publish(locked);
    }

    if (monitorsPps()) {
        const auto now = std::chrono::steady_clock::now();
        const auto time = receiverDevice.readLastPpsTime();
        if (time && time != ppsMonitor.lastTimeNs) {
            // The first reading has no earlier one to compare with, so it does not count as a pulse.
            if (ppsMonitor.lastTimeNs) {
                ppsMonitor.lastChange = now;
            }
            ppsMonitor.lastTimeNs = time;
        }
        const bool present = ppsMonitor.lastChange != std::chrono::steady_clock::time_point{} &&
                             now - ppsMonitor.lastChange < std::chrono::milliseconds(2500);
        if (present != ppsStatus.get().present) {
            JST_DEBUG("[MODULE_SOAPY] PPS on '{}' is {}.", timeSource, present ? "present" : "missing");
        }
        ppsStatus.publish({
            .monitored = true,
            .present = present,
            .lastTimeNs = ppsMonitor.lastTimeNs.value_or(0),
        });
    }
}

Result SoapyImpl::soapyThreadLoop() {
    constexpr std::size_t temporaryBufferSize = 8192;
    constexpr auto timingPollPeriod = std::chrono::seconds(1);
    std::array<std::vector<CF32>, MaxChannels> tmp;
    std::array<CF32*, MaxChannels> readBuffers{};
    for (U64 channel = 0; channel < channels; ++channel) {
        tmp[channel].resize(temporaryBufferSize);
        readBuffers[channel] = tmp[channel].data();
    }
    const auto readSize = std::min<std::size_t>(temporaryBufferSize, streams[0].capacity());
    const auto readSpan = std::span<CF32* const>{readBuffers.data(), static_cast<std::size_t>(channels)};
    SoapyReceiveStatus receiveStatus;
    auto nextTimingPoll = std::chrono::steady_clock::now();

    while (streaming) {
        const auto lock = access.lock();
        if (!streaming || errored) {
            break;
        }

        if (std::chrono::steady_clock::now() >= nextTimingPoll) {
            pollTiming();
            nextTimingPoll = std::chrono::steady_clock::now() + timingPollPeriod;
        }

        const U64 readGeneration = streams[0].metadata().generation;
        const auto received = receiverDevice.read(readSpan, readSize);
        if (!streaming || errored) {
            break;
        }

        const auto action = receiveStatus.handle(received.status);
        if (received.status == SoapyReceiver::ReadStatus::Overflow) {
            deviceOverflows.publish(receiveStatus.deviceOverflows);
        }
        if (action == SoapyReceiveStatus::Action::Fail) {
            JST_ERROR("[MODULE_SOAPY] Failed to read stream on '{}': {}. Stopping reception.",
                      name(), received.error);
            errored = true;
            break;
        } else if (action == SoapyReceiveStatus::Action::Samples) {
            {
                // Push the later channels first: readers wait on channel 0.
                std::lock_guard channelLock(channelMutex);
                for (U64 channel = channels; channel-- > 0;) {
                    streams[channel].push(readGeneration, tmp[channel].data(), received.sampleCount);
                }
            }
            Scheduler::Context::NotifyPendingCompute();
            const U64 capacity = streams[0].capacity();
            if (capacity > 0) {
                const F32 newFill = static_cast<F32>(streams[0].size()) /
                                    static_cast<F32>(capacity);
                const F32 smoothedFill = bufferFill.get() * 0.99f + newFill * 0.01f;
                bufferFill.publish(smoothedFill);
            }
            const auto bytesPerSample = static_cast<F32>(sizeof(CF32) * channels);
            const F32 actualMB = static_cast<F32>(streams[0].throughput()) * bytesPerSample / 1e6f;
            const F32 expectedMB = static_cast<F32>(streams[0].metadata().sampleRate) * bytesPerSample / 1e6f;
            throughput.publish({actualMB, expectedMB});
        }
    }

    return Result::SUCCESS;
}

F32 SoapyImpl::getBufferFill() const {
    return bufferFill.get();
}

F64 SoapyImpl::getBufferLoss() const {
    const auto stats = streams[0].statistics();
    return stats.pushedSamples > 0
        ? static_cast<F64>(stats.overwrittenSamples) / static_cast<F64>(stats.pushedSamples)
        : 0.0;
}

U64 SoapyImpl::getDeviceOverflows() const {
    return deviceOverflows.get();
}

std::pair<F32, F32> SoapyImpl::getThroughput() const {
    return throughput.get();
}

std::vector<std::string> SoapyImpl::listAntennas(const U64 channel) const {
    return receiverDevice.listAntennas(channel);
}

std::vector<std::string> SoapyImpl::listClockSources() const {
    return receiverDevice.listClockSources();
}

std::vector<std::string> SoapyImpl::listTimeSources() const {
    return receiverDevice.listTimeSources();
}

bool SoapyImpl::supportsBiasTee() const {
    return receiverDevice.supportsBiasTee();
}

std::optional<SoapyAbi::Range> SoapyImpl::getGainRange() const {
    return gainRange.get();
}

std::optional<bool> SoapyImpl::getReferenceLock() const {
    return referenceLock.get();
}

SoapyImpl::PpsStatus SoapyImpl::getPpsStatus() const {
    return ppsStatus.get();
}

Result SoapyImpl::applyTuning(const Tuning& desired, const bool force) {
    if (force || desired.sampleRate != tuned.sampleRate) {
        JST_CHECK(receiverDevice.setSampleRate(desired.sampleRate));
        tuned.sampleRate = desired.sampleRate;
        resetStream(tuned.frequency, tuned.sampleRate);
    }

    const bool retuned = force || desired.frequency != tuned.frequency;
    if (retuned) {
        JST_CHECK(receiverDevice.setTunerFrequency(desired.frequency));
        tuned.frequency = desired.frequency;
        resetStream(tuned.frequency, tuned.sampleRate);
        gainRange.publish(receiverDevice.getGainRange());
    }

    const bool modeChanged = force || desired.automaticGain != tuned.automaticGain;
    if (modeChanged) {
        JST_CHECK(receiverDevice.setAutomaticGain(desired.automaticGain));
        tuned.automaticGain = desired.automaticGain;
    }

    const F32 applied = receiverDevice.clampGain(desired.gain);
    if (!tuned.automaticGain && (applied != tuned.gain || retuned || modeChanged)) {
        JST_CHECK(receiverDevice.setGain(applied));
    }
    tuned.gain = applied;

    return Result::SUCCESS;
}

Result SoapyImpl::setBiasTee(const bool enabled) {
    return access.change([&] { return receiverDevice.setBiasTee(enabled); });
}

}  // namespace Jetstream::Modules
