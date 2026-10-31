#ifndef JETSTREAM_DOMAINS_IO_SOAPY_LIBRARY_SOAPYSDR_HH
#define JETSTREAM_DOMAINS_IO_SOAPY_LIBRARY_SOAPYSDR_HH

#include <chrono>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <jetstream/types.hh>

#include "soapy_runtime.hh"

namespace Jetstream::Modules {

inline bool SoapyRangeContains(const std::vector<SoapyAbi::Range>& ranges, const F32 value) {
    for (const auto& range : ranges) {
        const F32 minimum = static_cast<F32>(range.minimum);
        const F32 maximum = static_cast<F32>(range.maximum);
        if (value < minimum || value > maximum) {
            continue;
        }

        const double step = range.step;
        if (!std::isfinite(step) || step <= 0.0) {
            return true;
        }

        const double stepCount = std::round((static_cast<double>(value) - range.minimum) / step);
        const F32 closest = static_cast<F32>(range.minimum + stepCount * step);
        if (value == closest) {
            return true;
        }
    }
    return false;
}

struct JETSTREAM_API SoapyDiscovery {
    using Clock = std::chrono::steady_clock;
    using DeviceEntry = SoapyKwargs;
    using DeviceList = std::map<std::string, DeviceEntry>;

    static DeviceList List(const std::string& filter = "", bool refresh = false,
                           std::optional<Clock::time_point> now = std::nullopt);
    static void ClearCache();
    static Result LoadDriverLibrary(const std::string& path);
    static DeviceList BuildDeviceList(const SoapyKwargsList& entries);
    static std::string FormatArguments(const SoapyKwargs& args);
};

class JETSTREAM_API SoapyReceiver {
 public:
    static constexpr U64 MaxChannels = 2;

    enum class ReadStatus {
        Samples,
        Timeout,
        Overflow,
        Error,
    };

    struct ReadResult {
        ReadStatus status = ReadStatus::Error;
        std::size_t sampleCount = 0;
        std::optional<I64> timestampNs;
        std::string error;
    };

    SoapyReceiver() = default;
    ~SoapyReceiver();
    SoapyReceiver(const SoapyReceiver&) = delete;
    SoapyReceiver& operator=(const SoapyReceiver&) = delete;

    Result open(const std::string& deviceString);
    Result openArgs(const SoapyKwargs& args);
    Result validateSettings(F32 sampleRate, F32 frequency) const;
    const std::vector<std::string>& listAntennas(U64 channel = 0) const;
    const std::vector<std::string>& listClockSources() const;
    const std::vector<std::string>& listTimeSources() const;
    const std::optional<SoapyAbi::Range>& getGainRange() const;
    bool supportsBiasTee() const;
    bool supportsReferenceLock() const;
    bool supportsPpsTime() const;
    F32 clampGain(F32 gain) const;
    // Channels 0 to count - 1 are tuned together and streamed. Call before tuning.
    Result setChannels(U64 count);
    Result setAntenna(U64 channel, const std::string& antenna);
    Result setClockSource(const std::string& source);
    Result setReferenceClockRate(F32 rate);
    Result setTimeSource(const std::string& source);
    // Waits for a PPS edge, sets the device time to the next whole host UTC second at the
    // following edge, and waits for that edge. Returns the device time of that edge.
    Result syncTimeToPps(I64& edgeTimeNs);
    std::optional<bool> readReferenceLock() const;
    std::optional<I64> readLastPpsTime() const;
    // Current device time, or empty when the driver has no hardware time.
    std::optional<I64> readHardwareTime() const;
    Result setTunerFrequency(F32 frequency);
    Result setSampleRate(F32 sampleRate);
    Result setAutomaticGain(bool automaticGain);
    Result setGain(F32 gain);
    Result setBiasTee(bool enabled);
    Result startStream(const std::string& streamString);
    Result startStreamArgs(const SoapyKwargs& args);
    // Split form of startStream for timed starts. Setting up the stream can change the master
    // clock (UHD does for two channels), which invalidates the device time, so sync the time
    // between the two calls. A start time starts the stream at that device time.
    Result setupStream(const std::string& streamString);
    Result setupStreamArgs(const SoapyKwargs& args);
    Result activateStream(std::optional<I64> startTimeNs = std::nullopt);
    void reset();

    // One buffer per streamed channel, each with room for sampleCount samples.
    ReadResult read(std::span<CF32* const> buffers, std::size_t sampleCount);

 private:
    enum class State {
        Closed,
        Open,
        StreamReady,
        Streaming,
    };

    Result queryCapabilities();
    void queryGainRange();
    std::vector<std::string> readStrings(char** (*function)(const SoapyAbi::Device*, std::size_t*),
                                         const char* what) const;
    std::string failure() const;

    State state = State::Closed;
    std::shared_ptr<SoapyLibrary> library;
    SoapyAbi::Device* device = nullptr;
    SoapyAbi::Stream* stream = nullptr;
    U64 availableChannels = 1;
    U64 channels = 1;
    std::vector<SoapyAbi::Range> sampleRateRanges;
    std::vector<SoapyAbi::Range> frequencyRanges;
    std::vector<SoapyAbi::Range> referenceClockRates;
    std::vector<std::vector<std::string>> antennas;
    std::vector<std::string> clockSources;
    std::vector<std::string> timeSources;
    std::optional<SoapyAbi::Range> gainRange;
    bool biasTeeSupported = false;
    bool biasTeeNeedsCleanup = false;
    bool referenceLockSupported = false;
    bool ppsTimeSupported = false;
    bool hardwareTimeSupported = false;
};

struct JETSTREAM_API SoapyReceiveStatus {
    enum class Action {
        Samples,
        Retry,
        Fail,
    };

    Action handle(SoapyReceiver::ReadStatus status);

    U64 deviceOverflows = 0;
};

}  // namespace Jetstream::Modules

#endif  // JETSTREAM_DOMAINS_IO_SOAPY_LIBRARY_SOAPYSDR_HH
