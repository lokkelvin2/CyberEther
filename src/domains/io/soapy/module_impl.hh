#ifndef JETSTREAM_DOMAINS_IO_SOAPY_MODULE_IMPL_HH
#define JETSTREAM_DOMAINS_IO_SOAPY_MODULE_IMPL_HH

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <jetstream/domains/io/soapy/module.hh>
#include <jetstream/detail/module_impl.hh>
#include <jetstream/tools/snapshot.hh>

#include "domains/io/sdr/tools/stream_access.hh"
#include "domains/io/sdr/tools/stream_buffer.hh"
#include "library/soapysdr.hh"

namespace Jetstream::Modules {

struct JETSTREAM_API SoapyImpl : public Module::Impl, public DynamicConfig<Soapy> {
 public:
    static constexpr U64 MaxChannels = SoapyReceiver::MaxChannels;

    struct PpsStatus {
        bool monitored = false;
        bool present = false;
        I64 lastTimeNs = 0;
    };

    ~SoapyImpl() override;

    Result validate() override;
    Result define() override;
    Result create() override;
    Result destroy() override;
    Result reconfigure() override;

    F32 getBufferFill() const;
    F64 getBufferLoss() const;
    U64 getDeviceOverflows() const;
    std::pair<F32, F32> getThroughput() const;
    std::vector<std::string> listAntennas(U64 channel = 0) const;
    std::vector<std::string> listClockSources() const;
    std::vector<std::string> listTimeSources() const;
    bool supportsBiasTee() const;
    std::optional<SoapyAbi::Range> getGainRange() const;
    // Empty while the reference is not monitored (no external clock source or no lock sensor).
    std::optional<bool> getReferenceLock() const;
    PpsStatus getPpsStatus() const;

    Result setBiasTee(bool enabled);

    static std::string OutputName(U64 channel);

 protected:
    // One output and ring buffer per channel. Both rings get the same pushes and pops under
    // channelMutex, so their samples stay aligned.
    std::array<Tensor, MaxChannels> buffers;
    std::array<Radio::StreamBuffer, MaxChannels> streams;
    std::mutex channelMutex;

    U64 validatedOutputSizeBytes = 0;
    U64 validatedInternalElements = 0;
    U64 validatedInternalSizeBytes = 0;

    std::thread producer;
    std::atomic<bool> errored{false};
    std::atomic<bool> streaming{false};

    Radio::StreamBuffer::Metadata publishedMetadata;
    U64 generation = 0;

    Tools::Snapshot<F32> bufferFill{0.0f};
    Tools::Snapshot<U64> deviceOverflows{0};
    Tools::Snapshot<std::pair<F32, F32>> throughput{{0.0f, 0.0f}};
    Tools::Snapshot<std::optional<SoapyAbi::Range>> gainRange;
    Tools::Snapshot<std::optional<bool>> referenceLock;
    Tools::Snapshot<PpsStatus> ppsStatus;

    Result soapyThreadLoop();
    Result emitFrame();

 private:
    struct Tuning {
        F32 frequency = 0.0f;
        F32 sampleRate = 0.0f;
        bool automaticGain = true;
        F32 gain = 0.0f;
    };

    SoapyReceiver receiverDevice;
    Radio::StreamAccess access;
    Tuning tuned;

    struct PpsMonitor {
        std::optional<I64> lastTimeNs;
        std::chrono::steady_clock::time_point lastChange{};
    };

    PpsMonitor ppsMonitor;

    Result allocateBuffers();
    Result configureDevice();
    bool monitorsReference() const;
    bool monitorsPps() const;
    void pollTiming();
    Result applyTuning(const Tuning& desired, bool force = false);
    void resetStream(F32 appliedFrequency, F32 appliedSampleRate);
    Result publishMetadata(const Radio::StreamBuffer::Metadata& metadata);
    Result startReceiver();
    void stopReceiver();
};

}  // namespace Jetstream::Modules

#endif  // JETSTREAM_DOMAINS_IO_SOAPY_MODULE_IMPL_HH
