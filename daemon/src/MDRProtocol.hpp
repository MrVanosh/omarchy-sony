#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>
#include <array>
#include <optional>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// Wire Framing Constants
// ---------------------------------------------------------------------------
inline constexpr uint8_t kStartMarker     = 0x3E; // '>' (Start delimiter)
inline constexpr uint8_t kEndMarker       = 0x3C; // '<' (End delimiter)
inline constexpr uint8_t kEscapeSentry    = 0x3D; // '=' (Escape sentry)
inline constexpr uint8_t kEscaped3C       = 0x2C; // 0x3C is escaped as 0x3D 0x2C
inline constexpr uint8_t kEscaped3D       = 0x2D; // 0x3D is escaped as 0x3D 0x2D
inline constexpr uint8_t kEscaped3E       = 0x2E; // 0x3E is escaped as 0x3D 0x2E

// ---------------------------------------------------------------------------
// Protocol Enumerations
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    ACK          = 0x01,
    DATA_MDR     = 0x0C,
    DATA_MDR_NO2 = 0x0E,
    UNKNOWN      = 0xFF
};

enum class NoiseMode : uint8_t {
    OFF         = 0,
    ANC         = 1,
    AMBIENT     = 2
};

enum class UltMode : uint8_t {
    OFF = 0,
    ULT1 = 1,
    ULT2 = 2
};

inline constexpr uint8_t kEqebbInquiredType = 0x00;

enum class EqPreset : uint8_t {
    OFF         = 0x00,
    BRIGHT      = 0x10,
    EXCITED     = 0x11,
    MELLOW      = 0x12,
    RELAXED     = 0x13,
    VOCAL       = 0x14,
    TREBLE      = 0x15,
    BASS        = 0x16,
    SPEECH      = 0x17,
    CUSTOM      = 0xA0,
    UNKNOWN     = 0xFF
};

// ---------------------------------------------------------------------------
// Data Structures
// ---------------------------------------------------------------------------
struct UnpackedFrame {
    PacketType type{PacketType::UNKNOWN};
    uint8_t seq{0};
    std::vector<uint8_t> payload;
};

struct HeadphoneState {
    int schema_version = 1;
    bool connected = false;
    std::string device_name = "ULT WEAR";
    int battery_level = -1;             // 0-100, or -1 if unknown
    bool battery_charging = false;
    std::string noise_mode = "anc";     // "anc", "ambient", "off"
    int ambient_sound_level = 0;        // Kept for status compatibility; ULT WEAR is binary
    bool voice_passthrough = false;
    int ult_mode = 0;                   // 0 = off, 1 = ULT 1, 2 = ULT 2
    std::string eq_preset = "off";
    std::array<int, 5> eq_custom_bands = {0, 0, 0, 0, 0}; // [-10, 10]
    int clear_bass = 0;                 // [-10, 10]
    bool dsee_extreme = true;
    std::string codec = "LDAC";
    int64_t last_updated = 0;

    [[nodiscard]] std::string toJson() const;
    static HeadphoneState makeDisconnected();
};

// ---------------------------------------------------------------------------
// Low-Level Framing & Checksum
// ---------------------------------------------------------------------------
uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept;
std::vector<uint8_t> escapeBytes(std::span<const uint8_t> unescaped);
std::vector<uint8_t> unescapeBytes(std::span<const uint8_t> escaped);

// Packs a complete frame ready for transmission over RFCOMM (delimited with 0x3E and 0x3C)
std::vector<uint8_t> packFrame(PacketType type, uint8_t seq, std::span<const uint8_t> payload);

// Unpacks a single complete frame enclosed by [0x3E ... 0x3C]
std::optional<UnpackedFrame> unpackFrame(std::span<const uint8_t> frameBytes);

// Stream Framer: extracts complete [0x3E ... 0x3C] frames from streaming socket input
class StreamFramer {
public:
    void append(std::span<const uint8_t> incoming);
    std::optional<std::vector<uint8_t>> nextFrame();
    void reset();
    [[nodiscard]] size_t bufferedBytes() const noexcept { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
};

// ---------------------------------------------------------------------------
// Command Serializers (Host -> ULT WEAR / Sony MDR v2)
// ---------------------------------------------------------------------------
std::vector<uint8_t> serializeACK(uint8_t rx_seq);
std::vector<uint8_t> serializeNoiseMode(NoiseMode mode, uint8_t ambientLevel = 0, bool voiceFocus = false, uint8_t seq = 0);
std::vector<uint8_t> serializeAmbientLevel(uint8_t level, bool voiceFocus = false, uint8_t seq = 0);
std::vector<uint8_t> serializeEqPreset(EqPreset preset, uint8_t seq = 0);
std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq = 0);
std::vector<uint8_t> serializeDsee(bool enabled, uint8_t seq = 0);
std::vector<uint8_t> serializeUltMode(UltMode mode, uint8_t seq = 0);

// ---------------------------------------------------------------------------
// Query Serializers (Host -> ULT WEAR initialization)
// ---------------------------------------------------------------------------
// CONNECT_GET_PROTOCOL_INFO. The XM3 stays mute on every other command until
// this handshake has been sent, so it must open each RFCOMM session.
std::vector<uint8_t> serializeHandshake(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryDeviceName(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryBattery(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryNoiseMode(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryEq(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryDsee(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryUltMode(uint8_t seq = 0);

// ---------------------------------------------------------------------------
// Inbound State Deserializer (ULT WEAR -> Host)
// ---------------------------------------------------------------------------
// Parses an unpacked payload into HeadphoneState. Returns true if state was updated.
bool parseInboundPayload(std::span<const uint8_t> payload, HeadphoneState& state);

// ---------------------------------------------------------------------------
// Enum <-> String Helpers
// ---------------------------------------------------------------------------
std::string eqPresetToString(EqPreset preset);
EqPreset stringToEqPreset(const std::string& str);
std::string noiseModeToString(NoiseMode mode);
NoiseMode stringToNoiseMode(const std::string& str);

} // namespace omarchy::sony::protocol

// Provide convenient alias for consumer code
namespace sony = omarchy::sony;
