#include "StateEngine.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>

namespace omarchy::sony {

// ---------------------------------------------------------------------------
// Path Resolution
// ---------------------------------------------------------------------------

std::filesystem::path StateEngine::resolveStateFilePath(const std::filesystem::path& customStatePath) {
    if (!customStatePath.empty()) {
        if (customStatePath.filename() == "status.json") {
            return customStatePath;
        }
        if (customStatePath.filename() == "sony-headphones") {
            return customStatePath / "status.json";
        }
        return customStatePath / "sony-headphones" / "status.json";
    }

    // 1. $XDG_STATE_HOME
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    if (xdgState && *xdgState != '\0') {
        std::filesystem::path p(xdgState);
        if (p.filename() == "sony-headphones") {
            return p / "status.json";
        }
        return p / "sony-headphones" / "status.json";
    }

    // 2. Fallback: $HOME/.local/state
    const char* home = std::getenv("HOME");
    if (home && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "state" / "sony-headphones" / "status.json";
    }

    // 3. Fallback: getpwuid(getuid())
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *(pw->pw_dir) != '\0') {
        return std::filesystem::path(pw->pw_dir) / ".local" / "state" / "sony-headphones" / "status.json";
    }

    // 4. Absolute fallback
    return std::filesystem::path("/tmp/sony-headphones/status.json");
}

bool StateEngine::ensureStateDirectory(const std::filesystem::path& dirPath) {
    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        std::cerr << "[StateEngine] Failed to create directories " << dirPath << ": " << ec.message() << std::endl;
        return false;
    }

    // Strictly enforce 0700 mode (S_IRWXU)
    if (::chmod(dirPath.c_str(), S_IRWXU) != 0) {
        std::cerr << "[StateEngine] Failed to chmod 0700 on " << dirPath << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Construction & Lifecycle
// ---------------------------------------------------------------------------

StateEngine::StateEngine(const std::filesystem::path& customStatePath)
    : stateFilePath_(resolveStateFilePath(customStatePath)),
      stateDir_(stateFilePath_.parent_path()) {
    // Default initial ULT WEAR state
    state_.schema_version = 1;
    state_.connected = true;
    state_.device_name = "ULT WEAR";
    state_.battery_level = 85;
    state_.battery_charging = false;
    state_.noise_mode = "anc";
    state_.ambient_sound_level = 0;
    state_.voice_passthrough = false;
    state_.ult_mode = 0;
    state_.eq_preset = "off";
    state_.eq_custom_bands = {0, 0, 0, 0, 0};
    state_.clear_bass = 0;
    state_.dsee_extreme = true;
    state_.codec = "LDAC";
    state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

StateEngine::~StateEngine() {
    cleanup();
}

bool StateEngine::initialize(bool initialConnected) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanedUp_ = false;
    state_.connected = initialConnected;
    state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (!ensureStateDirectory(stateDir_)) {
        return false;
    }

    return writeAtomic(serializeStateLocked());
}

void StateEngine::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cleanedUp_) {
        return;
    }
    cleanedUp_ = true;

    if (!stateFilePath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(stateFilePath_, ec);

        std::string tmpPath = stateFilePath_.string() + ".tmp." + std::to_string(::getpid());
        std::filesystem::remove(tmpPath, ec);
    }
}

// ---------------------------------------------------------------------------
// Atomic Persistence
// ---------------------------------------------------------------------------

bool StateEngine::writeAtomic(const std::string& jsonContent) {
    if (stateFilePath_.empty()) {
        return false;
    }

    if (!ensureStateDirectory(stateDir_)) {
        return false;
    }

    std::string tmpPath = stateFilePath_.string() + ".tmp." + std::to_string(::getpid());

    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::cerr << "[StateEngine] Failed to open tmp file " << tmpPath << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    // Guarantee strictly 0600 mode regardless of umask
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        std::cerr << "[StateEngine] Failed to fchmod 0600 on " << tmpPath << ": " << std::strerror(errno) << std::endl;
        ::close(fd);
        ::unlink(tmpPath.c_str());
        return false;
    }

    const char* buf = jsonContent.data();
    size_t remaining = jsonContent.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, buf, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[StateEngine] Write error: " << std::strerror(errno) << std::endl;
            ::close(fd);
            ::unlink(tmpPath.c_str());
            return false;
        }
        buf += written;
        remaining -= static_cast<size_t>(written);
    }

    if (!jsonContent.empty() && jsonContent.back() != '\n') {
        char nl = '\n';
        while (::write(fd, &nl, 1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    if (::fsync(fd) != 0) {
        std::cerr << "[StateEngine] fsync failed: " << std::strerror(errno) << std::endl;
        ::close(fd);
        ::unlink(tmpPath.c_str());
        return false;
    }

    if (::close(fd) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }

    if (::rename(tmpPath.c_str(), stateFilePath_.c_str()) != 0) {
        std::cerr << "[StateEngine] rename failed (" << tmpPath << " -> " << stateFilePath_ << "): "
                  << std::strerror(errno) << std::endl;
        ::unlink(tmpPath.c_str());
        return false;
    }

    return true;
}

bool StateEngine::commit() {
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payload = serializeStateLocked();
        notifyListenersLocked();
    }
    return writeAtomic(payload);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string StateEngine::serializeStateLocked() const {
    if (!state_.connected) {
        return "{\n  \"schema_version\": 1,\n  \"connected\": false\n}\n";
    }

    std::ostringstream ss;
    ss << "{\n"
       << "  \"schema_version\": " << state_.schema_version << ",\n"
       << "  \"connected\": true,\n"
       << "  \"device_name\": \"" << state_.device_name << "\",\n"
       << "  \"battery_level\": " << state_.battery_level << ",\n"
       << "  \"charging\": " << (state_.battery_charging ? "true" : "false") << ",\n"
       << "  \"battery_charging\": " << (state_.battery_charging ? "true" : "false") << ",\n"
       << "  \"noise_mode\": \"" << state_.noise_mode << "\",\n"
       << "  \"ambient_level\": " << state_.ambient_sound_level << ",\n"
       << "  \"ambient_sound_level\": " << state_.ambient_sound_level << ",\n"
       << "  \"voice_passthrough\": " << (state_.voice_passthrough ? "true" : "false") << ",\n"
       << "  \"ult_mode\": " << state_.ult_mode << ",\n"
       << "  \"eq_preset\": \"" << state_.eq_preset << "\",\n"
       << "  \"eq_bands\": ["
       << state_.eq_custom_bands[0] << ", " << state_.eq_custom_bands[1] << ", "
       << state_.eq_custom_bands[2] << ", " << state_.eq_custom_bands[3] << ", "
       << state_.eq_custom_bands[4] << "],\n"
       << "  \"eq_custom_bands\": ["
       << state_.eq_custom_bands[0] << ", " << state_.eq_custom_bands[1] << ", "
       << state_.eq_custom_bands[2] << ", " << state_.eq_custom_bands[3] << ", "
       << state_.eq_custom_bands[4] << "],\n"
       << "  \"clear_bass\": " << state_.clear_bass << ",\n"
       << "  \"dsee\": " << (state_.dsee_extreme ? "true" : "false") << ",\n"
       << "  \"dsee_extreme\": " << (state_.dsee_extreme ? "true" : "false") << ",\n"
       << "  \"codec\": \"" << state_.codec << "\",\n"
       << "  \"last_updated\": " << state_.last_updated << "\n"
       << "}\n";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

protocol::HeadphoneState StateEngine::getState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string StateEngine::getStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.toJson();
}

bool StateEngine::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.connected;
}

// ---------------------------------------------------------------------------
// Inbound Parser Integration
// ---------------------------------------------------------------------------

bool StateEngine::updateFromInbound(std::span<const uint8_t> payload) {
    std::string payloadJson;
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        updated = protocol::parseInboundPayload(payload, state_);
        if (updated) {
            state_.connected = true;
            state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            payloadJson = serializeStateLocked();
            notifyListenersLocked();
        }
    }
    if (updated) {
        writeAtomic(payloadJson);
    }
    return updated;
}

// ---------------------------------------------------------------------------
// State Mutators
// ---------------------------------------------------------------------------

void StateEngine::setConnected(bool connected, const std::string& deviceName) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.connected = connected;
        if (connected) {
            state_.device_name = deviceName;
        }
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setDeviceName(const std::string& name) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.device_name = name;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setBatteryLevel(int level) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.battery_level = level;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setCharging(bool charging) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.battery_charging = charging;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setBattery(int level, bool charging) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.battery_level = level;
        state_.battery_charging = charging;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

bool StateEngine::setNoiseMode(const std::string& mode) {
    if (mode != "anc" && mode != "ambient" && mode != "off") {
        return false;
    }
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.noise_mode = mode;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::updateNoiseMode(const std::string& mode, int ambientLevel, bool voiceFocus) {
    if (mode != "anc" && mode != "ambient" && mode != "off") {
        return false;
    }
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.noise_mode = mode;
        state_.ambient_sound_level = ambientLevel;
        state_.voice_passthrough = voiceFocus;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::setAmbientLevel(int level) {
    if (level < 0 || level > 20) {
        return false;
    }
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.ambient_sound_level = level;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::setVoicePassthrough(bool passthrough) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.voice_passthrough = passthrough;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::setUltMode(int mode) {
    if (mode < 0 || mode > 2) {
        return false;
    }
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.ult_mode = mode;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::setEqPreset(const std::string& preset) {
    static const std::vector<std::string> kPresets = {
        "off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech", "custom"
    };
    bool valid = false;
    for (const auto& p : kPresets) {
        if (p == preset) { valid = true; break; }
    }
    if (!valid) return false;

    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.eq_preset = preset;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

bool StateEngine::setCustomEq(const std::array<int, 5>& bands, int clearBass) {
    for (int b : bands) {
        if (b < -10 || b > 10) return false;
    }
    if (clearBass < -10 || clearBass > 10) return false;

    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.eq_preset = "custom";
        state_.eq_custom_bands = bands;
        state_.clear_bass = clearBass;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
    return true;
}

void StateEngine::setDsee(bool enabled) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.dsee_extreme = enabled;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setCodec(const std::string& codec) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.codec = codec;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::modifyState(const std::function<void(protocol::HeadphoneState&)>& mutator) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mutator(state_);
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

void StateEngine::setState(const protocol::HeadphoneState& newState) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = newState;
        state_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        payloadJson = serializeStateLocked();
        notifyListenersLocked();
    }
    writeAtomic(payloadJson);
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------

void StateEngine::addListener(StateListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}

void StateEngine::notifyListenersLocked() {
    for (const auto& listener : listeners_) {
        if (listener) {
            listener(state_);
        }
    }
}

} // namespace omarchy::sony
