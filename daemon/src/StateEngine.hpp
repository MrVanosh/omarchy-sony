#pragma once

#include "MDRProtocol.hpp"
#include <string>
#include <filesystem>
#include <mutex>
#include <functional>
#include <optional>
#include <vector>
#include <array>
#include <span>

namespace omarchy::sony {

// State modification listener callback
using StateListener = std::function<void(const protocol::HeadphoneState&)>;

class StateEngine {
public:
    // Constructor accepts an optional custom state directory or file path.
    // If empty, standard XDG path resolution is performed.
    explicit StateEngine(const std::filesystem::path& customStatePath = "");
    ~StateEngine();

    // Non-copyable, non-movable
    StateEngine(const StateEngine&) = delete;
    StateEngine& operator=(const StateEngine&) = delete;
    StateEngine(StateEngine&&) = delete;
    StateEngine& operator=(StateEngine&&) = delete;

    // -----------------------------------------------------------------------
    // Path Resolution & Filesystem Lifecycle
    // -----------------------------------------------------------------------
    static std::filesystem::path resolveStateFilePath(const std::filesystem::path& customStatePath = "");
    static bool ensureStateDirectory(const std::filesystem::path& dirPath);

    // Initializer: creates directories and commits initial status.json
    bool initialize(bool initialConnected = true);
    bool init() { return initialize(true); }

    // Shutdown cleanup: unlinks status.json and any stale temporary files
    void cleanup();

    // -----------------------------------------------------------------------
    // Atomic Persistence Engine
    // -----------------------------------------------------------------------
    // Writes JSON content to <status.json>.tmp.<pid> with mode 0600,
    // calls fsync(), closes, and renames atomically to <status.json>.
    bool writeAtomic(const std::string& jsonContent);

    // Formats the current state and writes to disk atomically
    bool commit();
    bool save() { return commit(); }

    // -----------------------------------------------------------------------
    // State Accessors (Thread-Safe)
    // -----------------------------------------------------------------------
    [[nodiscard]] protocol::HeadphoneState getState() const;
    [[nodiscard]] protocol::HeadphoneState& getStateUnsafe() noexcept { return state_; }
    [[nodiscard]] std::string getStatusJson() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] const std::filesystem::path& getStateFilePath() const noexcept { return stateFilePath_; }
    [[nodiscard]] std::string getStatusFilePath() const { return stateFilePath_.string(); }
    [[nodiscard]] const std::filesystem::path& getStateDirectory() const noexcept { return stateDir_; }

    // -----------------------------------------------------------------------
    // Inbound Protocol Integration
    // -----------------------------------------------------------------------
    bool updateFromInbound(std::span<const uint8_t> payload);

    // -----------------------------------------------------------------------
    // State Mutators
    // -----------------------------------------------------------------------
    void setConnected(bool connected, const std::string& deviceName = "ULT WEAR");
    void setDeviceName(const std::string& name);
    void setBatteryLevel(int level);
    void setCharging(bool charging);
    void setBattery(int level, bool charging);

    bool setNoiseMode(const std::string& mode);
    bool updateNoiseMode(const std::string& mode, int ambientLevel = 0, bool voiceFocus = false);

    bool setAmbientLevel(int level);
    bool updateAmbientLevel(int level) { return setAmbientLevel(level); }

    bool setVoicePassthrough(bool passthrough);

    bool setUltMode(int mode);
    bool updateUltMode(int mode) { return setUltMode(mode); }

    bool setEqPreset(const std::string& preset);
    bool updateEqPreset(const std::string& preset) { return setEqPreset(preset); }

    bool setCustomEq(const std::array<int, 5>& bands, int clearBass);
    bool updateCustomEq(const std::array<int, 5>& bands, int clearBass) { return setCustomEq(bands, clearBass); }

    void setDsee(bool enabled);
    void updateDsee(bool enabled) { setDsee(enabled); }

    void setCodec(const std::string& codec);

    // Transactional mutation
    void modifyState(const std::function<void(protocol::HeadphoneState&)>& mutator);
    void setState(const protocol::HeadphoneState& newState);

    // -----------------------------------------------------------------------
    // Listeners & Notifications
    // -----------------------------------------------------------------------
    void addListener(StateListener listener);

private:
    std::string serializeStateLocked() const;
    void notifyListenersLocked();

    mutable std::mutex mutex_;
    std::filesystem::path stateFilePath_;
    std::filesystem::path stateDir_;
    protocol::HeadphoneState state_;
    std::vector<StateListener> listeners_;
    bool cleanedUp_{false};
};

} // namespace omarchy::sony

namespace omarchy::sony::daemon {
    using StateEngine = omarchy::sony::StateEngine;
    using StateListener = omarchy::sony::StateListener;
}
