#include "soapysdr.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <jetstream/logger.hh>

namespace Jetstream::Modules {

namespace {

bool SampleRateInRanges(const std::vector<SoapyAbi::Range>& ranges, const F32 rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) {
        return false;
    }

    return std::any_of(ranges.begin(), ranges.end(), [rate](const auto& range) {
        return rate >= static_cast<F32>(range.minimum) &&
               rate <= static_cast<F32>(range.maximum);
    });
}

using DiscoveryClock = SoapyDiscovery::Clock;
constexpr auto DiscoveryLifetime = std::chrono::seconds(1);

struct DiscoveryEntry {
    std::shared_future<SoapyKwargsList> result;
    DiscoveryClock::time_point expiresAt{};
    bool ready = false;
};

struct DiscoveryCache {
    std::mutex mutex;
    std::map<SoapyKwargs, std::shared_ptr<DiscoveryEntry>> entries;
};

DiscoveryCache& GetDiscoveryCache() {
    static DiscoveryCache cache;
    return cache;
}

Result DiscoverDevices(const SoapyLibrary& library, const SoapyKwargs& args, SoapyKwargsList& devices,
                       const bool refresh = false,
                       const std::optional<DiscoveryClock::time_point> now = std::nullopt) {
    auto& cache = GetDiscoveryCache();
    std::shared_ptr<DiscoveryEntry> entry;
    std::optional<std::promise<SoapyKwargsList>> pending;
    {
        std::lock_guard lock(cache.mutex);
        const auto currentTime = now ? *now : DiscoveryClock::now();
        std::erase_if(cache.entries, [&](const auto& item) {
            return item.second->ready && currentTime >= item.second->expiresAt;
        });

        if (const auto it = cache.entries.find(args); it != cache.entries.end()) {
            if (!refresh || !it->second->ready) {
                entry = it->second;
            } else {
                cache.entries.erase(it);
            }
        }
        if (!entry) {
            pending.emplace();
            entry = std::make_shared<DiscoveryEntry>();
            entry->result = pending->get_future().share();
            cache.entries.emplace(args, entry);
        }
    }

    if (pending) {
        SoapyKwargsList found;
        const auto result = library.enumerate(args, found);
        pending->set_value(found);

        std::lock_guard lock(cache.mutex);
        if (result == Result::SUCCESS) {
            entry->expiresAt = (now ? *now : DiscoveryClock::now()) + DiscoveryLifetime;
            entry->ready = true;
        } else if (const auto it = cache.entries.find(args);
                   it != cache.entries.end() && it->second == entry) {
            cache.entries.erase(it);
        }
        if (result != Result::SUCCESS) {
            return result;
        }
    }

    devices = entry->result.get();
    return Result::SUCCESS;
}

}  // namespace

SoapyDiscovery::DeviceList SoapyDiscovery::List(const std::string& filter, const bool refresh,
                                                const std::optional<Clock::time_point> now) {
    const auto library = SoapyRuntime::Acquire();
    if (!library) {
        return {};
    }
    SoapyKwargs args;
    SoapyKwargsList devices;
    if (library->kwargsFromString(filter, args) != Result::SUCCESS ||
        DiscoverDevices(*library, args, devices, refresh, now) != Result::SUCCESS) {
        return {};
    }
    return BuildDeviceList(devices);
}

void SoapyDiscovery::ClearCache() {
    auto& cache = GetDiscoveryCache();
    std::lock_guard lock(cache.mutex);
    cache.entries.clear();
}

Result SoapyDiscovery::LoadDriverLibrary(const std::string& path) {
    if (path.empty()) {
        return Result::SUCCESS;
    }

    const auto library = SoapyRuntime::Acquire();
    if (!library) {
        JST_ERROR("[MODULE_SOAPY] Cannot load module '{}': {}", path,
                  SoapyRuntime::Availability().message);
        return Result::ERROR;
    }
    JST_CHECK(library->loadModule(path));
    ClearCache();
    return Result::SUCCESS;
}

SoapyDiscovery::DeviceList SoapyDiscovery::BuildDeviceList(const SoapyKwargsList& entries) {
    DeviceList devices;
    for (const auto& entry : entries) {
        const auto labelIt = entry.find("label");
        const auto driverIt = entry.find("driver");
        std::string label = "SoapySDR Device";
        if (labelIt != entry.end() && !labelIt->second.empty()) {
            label = labelIt->second;
        } else if (driverIt != entry.end() && !driverIt->second.empty()) {
            label = driverIt->second;
        }

        std::string uniqueLabel = label;
        if (devices.contains(uniqueLabel)) {
            const auto serialIt = entry.find("serial");
            if (serialIt != entry.end() && !serialIt->second.empty() &&
                label.find(serialIt->second) == std::string::npos) {
                uniqueLabel = label + " [" + serialIt->second + "]";
            }

            const std::string uniqueLabelBase = uniqueLabel;
            U64 suffix = 2;
            while (devices.contains(uniqueLabel)) {
                uniqueLabel = uniqueLabelBase + " #" + std::to_string(suffix++);
            }
        }

        devices.emplace(std::move(uniqueLabel), entry);
    }
    return devices;
}

std::string SoapyDiscovery::FormatArguments(const SoapyKwargs& args) {
    const auto library = SoapyRuntime::Acquire();
    return library ? library->kwargsToString(args) : std::string{};
}

SoapyReceiver::~SoapyReceiver() {
    reset();
}

std::string SoapyReceiver::failure() const {
    const auto error = library ? library->lastError() : std::string{};
    return error.empty() ? std::string("Unknown driver error.") : error;
}

Result SoapyReceiver::open(const std::string& deviceString) {
    if (state != State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Device is already open.");
        return Result::ERROR;
    }
    const auto runtime = SoapyRuntime::Acquire();
    if (!runtime) {
        JST_ERROR("[MODULE_SOAPY] {}", SoapyRuntime::Availability().message);
        return Result::INCOMPLETE;
    }
    SoapyKwargs args;
    JST_CHECK(runtime->kwargsFromString(deviceString, args));
    return openArgs(args);
}

Result SoapyReceiver::openArgs(const SoapyKwargs& args) {
    if (state != State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Device is already open.");
        return Result::ERROR;
    }

    library = SoapyRuntime::Acquire();
    if (!library) {
        JST_ERROR("[MODULE_SOAPY] {}", SoapyRuntime::Availability().message);
        return Result::INCOMPLETE;
    }
    const auto& api = library->api();

    SoapyKwargs deviceArgs;
    {
        SoapyKwargsList devices;
        if (DiscoverDevices(*library, args, devices) != Result::SUCCESS) {
            SoapyDiscovery::ClearCache();
            library.reset();
            return Result::ERROR;
        }
        if (devices.empty()) {
            JST_ERROR("[MODULE_SOAPY] No SoapySDR devices found.");
            library.reset();
            return Result::INCOMPLETE;
        }
        if (devices.size() != 1) {
            JST_ERROR("[MODULE_SOAPY] Device selection is ambiguous.");
            library.reset();
            return Result::INCOMPLETE;
        }
        deviceArgs = devices.front();
        for (const auto& [key, value] : args) {
            deviceArgs.insert_or_assign(key, value);
        }
    }

    const SoapyKwargsView view(deviceArgs);
    device = api.make(view.get());
    if (device == nullptr) {
        const auto openError = failure();
        SoapyDiscovery::ClearCache();
        SoapyKwargsList devices;
        if (DiscoverDevices(*library, args, devices, true) == Result::SUCCESS) {
            if (devices.empty()) {
                JST_ERROR("[MODULE_SOAPY] Selected device is no longer available.");
                library.reset();
                return Result::INCOMPLETE;
            }
            if (devices.size() != 1) {
                JST_ERROR("[MODULE_SOAPY] Device selection is ambiguous.");
                library.reset();
                return Result::INCOMPLETE;
            }
        }
        JST_ERROR("[MODULE_SOAPY] Failed to open device: {}", openError);
        library.reset();
        return Result::ERROR;
    }

    state = State::Open;
    return queryCapabilities();
}

Result SoapyReceiver::queryCapabilities() {
    const auto& api = library->api();

    const auto readRanges = [&](auto function, std::vector<SoapyAbi::Range>& ranges) {
        std::size_t length = 0;
        auto* list = function(device, SoapyAbi::RX, 0, &length);
        const bool ok = api.lastStatus() == 0;
        if (ok && list) {
            ranges.assign(list, list + length);
        }
        api.free(list);
        return ok;
    };
    if (!readRanges(api.getSampleRateRange, sampleRateRanges) ||
        !readRanges(api.getFrequencyRange, frequencyRanges)) {
        JST_ERROR("[MODULE_SOAPY] Failed to get device ranges: {}", failure());
        return Result::ERROR;
    }

    std::size_t settingCount = 0;
    auto* settings = api.getSettingInfo(device, &settingCount);
    if (api.lastStatus() == 0) {
        for (std::size_t i = 0; i < settingCount; ++i) {
            biasTeeSupported |= settings[i].key && std::string(settings[i].key) == "biastee";
        }
    } else {
        JST_WARN("[MODULE_SOAPY] Failed to query optional device settings: {}", failure());
    }
    api.argInfoListClear(settings, settingCount);

    const std::size_t channelCount = api.getNumChannels(device, SoapyAbi::RX);
    if (api.lastStatus() == 0) {
        availableChannels = std::max<U64>(channelCount, 1);
    } else {
        JST_WARN("[MODULE_SOAPY] Failed to query receive channels: {}", failure());
        availableChannels = 1;
    }

    antennas.assign(std::min(availableChannels, MaxChannels), {});
    for (U64 channel = 0; channel < antennas.size(); ++channel) {
        std::size_t antennaCount = 0;
        char** names = api.listAntennas(device, SoapyAbi::RX, channel, &antennaCount);
        if (api.lastStatus() == 0) {
            for (std::size_t i = 0; i < antennaCount; ++i) {
                antennas[channel].emplace_back(names[i] ? names[i] : "");
            }
        } else {
            JST_WARN("[MODULE_SOAPY] Failed to query receive antennas of channel {}: {}", channel, failure());
        }
        api.stringsClear(&names, antennaCount);
    }

    clockSources = readStrings(api.listClockSources, "clock sources");
    timeSources = readStrings(api.listTimeSources, "time sources");
    const auto sensors = readStrings(api.listSensors, "sensors");
    referenceLockSupported = std::find(sensors.begin(), sensors.end(), "ref_locked") != sensors.end();

    std::size_t rateCount = 0;
    auto* rates = api.getReferenceClockRates(device, &rateCount);
    if (api.lastStatus() == 0 && rates) {
        referenceClockRates.assign(rates, rates + rateCount);
    }
    api.free(rates);

    ppsTimeSupported = api.hasHardwareTime(device, "PPS") && api.lastStatus() == 0;

    queryGainRange();

    return Result::SUCCESS;
}

std::vector<std::string> SoapyReceiver::readStrings(char** (*function)(const SoapyAbi::Device*, std::size_t*),
                                                    const char* what) const {
    const auto& api = library->api();
    std::vector<std::string> strings;
    std::size_t count = 0;
    char** list = function(device, &count);
    if (api.lastStatus() == 0) {
        for (std::size_t i = 0; i < count; ++i) {
            strings.emplace_back(list[i] ? list[i] : "");
        }
    } else {
        JST_WARN("[MODULE_SOAPY] Failed to query {}: {}", what, failure());
    }
    api.stringsClear(&list, count);
    return strings;
}

void SoapyReceiver::queryGainRange() {
    gainRange.reset();

    const auto range = library->api().getGainRange(device, SoapyAbi::RX, 0);
    if (library->api().lastStatus() != 0) {
        JST_WARN("[MODULE_SOAPY] Failed to query receive gain range: {}", failure());
        return;
    }
    if (std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
        range.minimum <= range.maximum) {
        gainRange = range;
    }
}

Result SoapyReceiver::validateSettings(const F32 sampleRate, const F32 frequency) const {
    if (!SampleRateInRanges(sampleRateRanges, sampleRate)) {
        JST_ERROR("[MODULE_SOAPY] Sample rate ({:.3f} MHz) not supported.", sampleRate / 1e6);
        return Result::ERROR;
    }

    if (!SoapyRangeContains(frequencyRanges, frequency)) {
        JST_ERROR("[MODULE_SOAPY] Frequency ({:.3f} MHz) not supported.", frequency / 1e6);
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

const std::vector<std::string>& SoapyReceiver::listAntennas(const U64 channel) const {
    static const std::vector<std::string> none;
    return channel < antennas.size() ? antennas[channel] : none;
}

const std::vector<std::string>& SoapyReceiver::listClockSources() const {
    return clockSources;
}

const std::vector<std::string>& SoapyReceiver::listTimeSources() const {
    return timeSources;
}

bool SoapyReceiver::supportsBiasTee() const {
    return biasTeeSupported;
}

bool SoapyReceiver::supportsReferenceLock() const {
    return referenceLockSupported;
}

bool SoapyReceiver::supportsPpsTime() const {
    return ppsTimeSupported;
}

const std::optional<SoapyAbi::Range>& SoapyReceiver::getGainRange() const {
    return gainRange;
}

F32 SoapyReceiver::clampGain(const F32 gain) const {
    if (!gainRange) {
        return gain;
    }

    const F32 minimum = static_cast<F32>(gainRange->minimum);
    const F32 maximum = static_cast<F32>(gainRange->maximum);
    const F32 applied = std::clamp(gain, minimum, maximum);
    if (applied != gain) {
        JST_WARN("[MODULE_SOAPY] Gain ({:.1f} dB) outside device range ({:.1f} to {:.1f} dB). Using {:.1f} dB.",
                 gain, minimum, maximum, applied);
    }
    return applied;
}

Result SoapyReceiver::setChannels(const U64 count) {
    if (state != State::Open) {
        JST_ERROR("[MODULE_SOAPY] Cannot change channels in the current device state.");
        return Result::ERROR;
    }
    if (count == 0 || count > MaxChannels) {
        JST_ERROR("[MODULE_SOAPY] Channels must be between 1 and {}.", MaxChannels);
        return Result::ERROR;
    }
    if (count > availableChannels) {
        JST_ERROR("[MODULE_SOAPY] The selected device has {} receive channel(s), so it cannot stream {}.",
                  availableChannels, count);
        return Result::ERROR;
    }

    channels = count;
    return Result::SUCCESS;
}

Result SoapyReceiver::setAntenna(const U64 channel, const std::string& antenna) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set antenna without an active device.");
        return Result::ERROR;
    }
    if (antenna.empty()) {
        return Result::SUCCESS;
    }
    const auto& available = listAntennas(channel);
    if (std::find(available.begin(), available.end(), antenna) == available.end()) {
        JST_ERROR("[MODULE_SOAPY] Receive antenna '{}' is not supported on channel {} of the selected device.",
                  antenna, channel);
        return Result::ERROR;
    }

    if (library->api().setAntenna(device, SoapyAbi::RX, channel, antenna.c_str()) != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to set receive antenna '{}' on channel {}: {}", antenna, channel, failure());
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setClockSource(const std::string& source) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set clock source without an active device.");
        return Result::ERROR;
    }
    if (source.empty()) {
        return Result::SUCCESS;
    }
    if (std::find(clockSources.begin(), clockSources.end(), source) == clockSources.end()) {
        JST_ERROR("[MODULE_SOAPY] Clock source '{}' is not supported by the selected device.", source);
        return Result::ERROR;
    }

    if (library->api().setClockSource(device, source.c_str()) != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to set clock source '{}': {}", source, failure());
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setReferenceClockRate(const F32 rate) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set reference frequency without an active device.");
        return Result::ERROR;
    }
    if (rate <= 0.0f) {
        return Result::SUCCESS;
    }
    if (referenceClockRates.empty()) {
        // SoapySDR's default implementation ignores the rate, so say so instead of failing quietly.
        JST_WARN("[MODULE_SOAPY] The driver cannot change the reference frequency ({:.3f} MHz requested). "
                 "The radio expects its fixed reference input frequency (10 MHz on Ettus B2xx).", rate / 1e6);
        return Result::SUCCESS;
    }
    if (!SoapyRangeContains(referenceClockRates, rate)) {
        JST_ERROR("[MODULE_SOAPY] Reference frequency ({:.3f} MHz) not supported.", rate / 1e6);
        return Result::ERROR;
    }

    if (library->api().setReferenceClockRate(device, rate) != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to set reference frequency: {}", failure());
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setTimeSource(const std::string& source) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set time source without an active device.");
        return Result::ERROR;
    }
    if (source.empty()) {
        return Result::SUCCESS;
    }
    if (std::find(timeSources.begin(), timeSources.end(), source) == timeSources.end()) {
        JST_ERROR("[MODULE_SOAPY] Time source '{}' is not supported by the selected device.", source);
        return Result::ERROR;
    }

    if (library->api().setTimeSource(device, source.c_str()) != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to set time source '{}': {}", source, failure());
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::syncTimeToPps(I64& edgeTimeNs) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot sync time without an active device.");
        return Result::ERROR;
    }
    if (!ppsTimeSupported) {
        JST_ERROR("[MODULE_SOAPY] The selected device cannot set its time from a PPS input.");
        return Result::ERROR;
    }

    // A PPS edge arrives once per second, so allow a bit more than one period.
    constexpr auto PpsTimeout = std::chrono::milliseconds(1500);
    constexpr I64 NanosecondsPerSecond = 1'000'000'000;
    const auto waitForEdge = [&](const I64 previous, I64& edge) {
        const auto deadline = std::chrono::steady_clock::now() + PpsTimeout;
        while (true) {
            const auto time = readLastPpsTime();
            if (!time) {
                JST_ERROR("[MODULE_SOAPY] Failed to read the PPS time: {}", failure());
                return Result::ERROR;
            }
            if (*time != previous) {
                edge = *time;
                return Result::SUCCESS;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                JST_ERROR("[MODULE_SOAPY] No PPS edge within {} ms. Check the 1PPS input and the Time Source.",
                          PpsTimeout.count());
                return Result::ERROR;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    // Set the time just after an edge, so the next edge is about a second away.
    const auto initial = readLastPpsTime();
    if (!initial) {
        JST_ERROR("[MODULE_SOAPY] Failed to read the PPS time: {}", failure());
        return Result::ERROR;
    }
    I64 edge = 0;
    JST_CHECK(waitForEdge(*initial, edge));

    // The edge just seen began the current host second, so the next one begins the following second.
    const auto hostSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    edgeTimeNs = (static_cast<I64>(hostSeconds) + 1) * NanosecondsPerSecond;
    if (library->api().setHardwareTime(device, edgeTimeNs, "PPS") != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to set the device time at the next PPS edge: {}", failure());
        return Result::ERROR;
    }

    I64 applied = 0;
    JST_CHECK(waitForEdge(edge, applied));
    if (applied != edgeTimeNs) {
        JST_ERROR("[MODULE_SOAPY] PPS time sync failed: the device time at the PPS edge is {} ns, not {} ns.",
                  applied, edgeTimeNs);
        return Result::ERROR;
    }

    return Result::SUCCESS;
}

std::optional<bool> SoapyReceiver::readReferenceLock() const {
    if (state == State::Closed || !referenceLockSupported) {
        return std::nullopt;
    }
    const auto& api = library->api();
    char* value = api.readSensor(device, "ref_locked");
    const bool ok = api.lastStatus() == 0 && value;
    const std::string text = value ? value : "";
    api.free(value);
    if (!ok) {
        return std::nullopt;
    }
    return text == "true" || text == "1";
}

std::optional<I64> SoapyReceiver::readLastPpsTime() const {
    if (state == State::Closed || !ppsTimeSupported) {
        return std::nullopt;
    }
    const auto& api = library->api();
    const long long time = api.getHardwareTime(device, "PPS");
    if (api.lastStatus() != 0) {
        return std::nullopt;
    }
    return static_cast<I64>(time);
}

Result SoapyReceiver::setTunerFrequency(const F32 frequency) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set frequency without an active device.");
        return Result::ERROR;
    }

    if (!SoapyRangeContains(frequencyRanges, frequency)) {
        JST_WARN("[MODULE_SOAPY] Frequency ({:.3f} MHz) not supported.", frequency / 1e6);
        return Result::WARNING;
    }

    const SoapyKwargsView args({});
    for (U64 channel = 0; channel < channels; ++channel) {
        if (library->api().setFrequency(device, SoapyAbi::RX, channel, frequency, args.get()) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to set frequency: {}", failure());
            return Result::ERROR;
        }
    }

    queryGainRange();

    return Result::SUCCESS;
}

Result SoapyReceiver::setSampleRate(const F32 sampleRate) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set sample rate without an active device.");
        return Result::ERROR;
    }

    if (!SampleRateInRanges(sampleRateRanges, sampleRate)) {
        JST_WARN("[MODULE_SOAPY] Sample rate ({:.3f} MHz) not supported.", sampleRate / 1e6);
        return Result::WARNING;
    }

    for (U64 channel = 0; channel < channels; ++channel) {
        if (library->api().setSampleRate(device, SoapyAbi::RX, channel, sampleRate) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to set sample rate: {}", failure());
            return Result::ERROR;
        }
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setAutomaticGain(const bool automaticGain) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set gain mode without an active device.");
        return Result::ERROR;
    }

    const auto& api = library->api();
    const bool supported = api.hasGainMode(device, SoapyAbi::RX, 0);
    if (api.lastStatus() != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to query gain mode support: {}", failure());
        return Result::ERROR;
    }
    if (!supported) {
        if (automaticGain) {
            JST_WARN("[MODULE_SOAPY] Automatic gain control is not supported by the selected device.");
        }
        return Result::SUCCESS;
    }

    for (U64 channel = 0; channel < channels; ++channel) {
        if (api.setGainMode(device, SoapyAbi::RX, channel, automaticGain) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to set gain mode: {}", failure());
            return Result::ERROR;
        }
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setGain(const F32 gain) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set gain without an active device.");
        return Result::ERROR;
    }

    for (U64 channel = 0; channel < channels; ++channel) {
        if (library->api().setGain(device, SoapyAbi::RX, channel, gain) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to set gain: {}", failure());
            return Result::ERROR;
        }
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::setBiasTee(const bool enabled) {
    if (state == State::Closed) {
        JST_ERROR("[MODULE_SOAPY] Cannot set Bias-T without an active device.");
        return Result::ERROR;
    }

    if (biasTeeSupported) {
        biasTeeNeedsCleanup = true;
        if (library->api().writeSetting(device, "biastee", enabled ? "true" : "false") != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to set Bias-T: {}", failure());
            return Result::ERROR;
        }
        biasTeeNeedsCleanup = enabled;
    } else if (enabled) {
        JST_WARN("[MODULE_SOAPY] Bias-T is not supported by the selected device.");
    }

    return Result::SUCCESS;
}

Result SoapyReceiver::startStream(const std::string& streamString) {
    JST_CHECK(setupStream(streamString));
    return activateStream();
}

Result SoapyReceiver::startStreamArgs(const SoapyKwargs& args) {
    JST_CHECK(setupStreamArgs(args));
    return activateStream();
}

Result SoapyReceiver::setupStream(const std::string& streamString) {
    if (!library) {
        JST_ERROR("[MODULE_SOAPY] Cannot setup stream without an active device.");
        return Result::ERROR;
    }
    SoapyKwargs args;
    JST_CHECK(library->kwargsFromString(streamString, args));
    return setupStreamArgs(args);
}

Result SoapyReceiver::setupStreamArgs(const SoapyKwargs& args) {
    if (state != State::Open) {
        JST_ERROR("[MODULE_SOAPY] Cannot setup stream in the current device state.");
        return Result::ERROR;
    }
    const auto& api = library->api();

    std::vector<std::size_t> streamChannels(channels);
    for (U64 channel = 0; channel < channels; ++channel) {
        streamChannels[channel] = channel;
    }
    const SoapyKwargsView view(args);
    stream = api.setupStream(device, SoapyAbi::RX, SoapyAbi::FormatCF32, streamChannels.data(),
                             streamChannels.size(), view.get());
    if (stream == nullptr) {
        JST_ERROR("[MODULE_SOAPY] Failed to setup stream: {}", failure());
        return Result::ERROR;
    }
    state = State::StreamReady;

    return Result::SUCCESS;
}

Result SoapyReceiver::activateStream(const std::optional<I64> startTimeNs) {
    if (state != State::StreamReady) {
        JST_ERROR("[MODULE_SOAPY] Cannot activate stream in the current device state.");
        return Result::ERROR;
    }

    const int activationFlags = startTimeNs ? SoapyAbi::HasTime : 0;
    const int activationResult = library->api().activateStream(device, stream, activationFlags,
                                                               startTimeNs.value_or(0), 0);
    if (activationResult != 0) {
        JST_ERROR("[MODULE_SOAPY] Failed to configure device: activateStream returned status {}",
                  activationResult);
        return Result::ERROR;
    }
    state = State::Streaming;

    return Result::SUCCESS;
}

SoapyReceiver::ReadResult SoapyReceiver::read(const std::span<CF32* const> buffers,
                                              const std::size_t sampleCount) {
    if (state != State::Streaming) {
        return {.error = "Cannot read without an active stream"};
    }
    if (sampleCount == 0) {
        return {.error = "Receive buffer cannot be empty"};
    }
    if (buffers.size() != channels) {
        return {.error = "One receive buffer per channel is required"};
    }

    std::array<void*, MaxChannels> channelBuffers{};
    std::copy(buffers.begin(), buffers.end(), channelBuffers.begin());
    int flags = 0;
    long long timeNs = 0;
    const int result = library->api().readStream(device, stream, channelBuffers.data(), sampleCount,
                                                 &flags, &timeNs, 100000);

    if (result > 0) {
        if (static_cast<std::size_t>(result) > sampleCount) {
            return {.error = "Device returned more samples than requested"};
        }
        ReadResult received{
            .status = ReadStatus::Samples,
            .sampleCount = static_cast<std::size_t>(result),
        };
        if (flags & SoapyAbi::HasTime) {
            received.timestampNs = timeNs;
        }
        return received;
    }
    if (result == 0 || result == SoapyAbi::Timeout) {
        return {.status = ReadStatus::Timeout};
    }
    if (result == SoapyAbi::Overflow) {
        return {.status = ReadStatus::Overflow};
    }

    auto error = jst::fmt::format("{} ({})", library->errorString(result), result);
    if (const auto detail = library->lastError(); !detail.empty()) {
        error = jst::fmt::format("{}: {}", error, detail);
    }
    return {.error = error};
}

void SoapyReceiver::reset() {
    if (device && stream) {
        const auto& api = library->api();
        if (state == State::Streaming && api.deactivateStream(device, stream, 0, 0) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to deactivate stream: {}", failure());
        }
        if (api.closeStream(device, stream) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to close stream: {}", failure());
        }
        stream = nullptr;
        state = State::Open;
    }

    if (biasTeeNeedsCleanup) {
        static_cast<void>(setBiasTee(false));
    }

    if (device) {
        if (library->api().unmake(device) != 0) {
            JST_ERROR("[MODULE_SOAPY] Failed to unmake device: {}", failure());
        }
        device = nullptr;
        state = State::Closed;
    }

    availableChannels = 1;
    channels = 1;
    sampleRateRanges.clear();
    frequencyRanges.clear();
    referenceClockRates.clear();
    antennas.clear();
    clockSources.clear();
    timeSources.clear();
    gainRange.reset();
    biasTeeSupported = false;
    biasTeeNeedsCleanup = false;
    referenceLockSupported = false;
    ppsTimeSupported = false;
    library.reset();
}

SoapyReceiveStatus::Action SoapyReceiveStatus::handle(const SoapyReceiver::ReadStatus status) {
    using Status = SoapyReceiver::ReadStatus;
    if (status == Status::Samples) {
        return Action::Samples;
    }
    if (status == Status::Timeout) {
        return Action::Retry;
    }
    if (status != Status::Overflow) {
        return Action::Fail;
    }

    ++deviceOverflows;
    return Action::Retry;
}

}  // namespace Jetstream::Modules
