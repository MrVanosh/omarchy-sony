#include "MDRProtocol.hpp"
#include <algorithm>
#include <sstream>
#include <chrono>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// HeadphoneState Implementation
// ---------------------------------------------------------------------------

std::string HeadphoneState::toJson() const {
    if (!connected) {
        return "{\"schema_version\":1,\"connected\":false}";
    }

    std::ostringstream ss;
    ss << "{"
       << "\"schema_version\":" << schema_version << ","
       << "\"connected\":true,"
       << "\"device_name\":\"" << device_name << "\","
       << "\"battery_level\":" << battery_level << ","
       << "\"charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"battery_charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"noise_mode\":\"" << noise_mode << "\","
       << "\"ambient_level\":" << ambient_sound_level << ","
       << "\"ambient_sound_level\":" << ambient_sound_level << ","
       << "\"voice_passthrough\":" << (voice_passthrough ? "true" : "false") << ","
       << "\"ult_mode\":" << ult_mode << ","
       << "\"eq_preset\":\"" << eq_preset << "\","
       << "\"eq_bands\":["
       << eq_custom_bands[0] << "," << eq_custom_bands[1] << "," << eq_custom_bands[2] << ","
       << eq_custom_bands[3] << "," << eq_custom_bands[4] << "],"
       << "\"eq_custom_bands\":["
       << eq_custom_bands[0] << "," << eq_custom_bands[1] << "," << eq_custom_bands[2] << ","
       << eq_custom_bands[3] << "," << eq_custom_bands[4] << "],"
       << "\"clear_bass\":" << clear_bass << ","
       << "\"dsee\":" << (dsee_extreme ? "true" : "false") << ","
       << "\"dsee_extreme\":" << (dsee_extreme ? "true" : "false") << ","
       << "\"codec\":\"" << codec << "\","
       << "\"last_updated\":" << last_updated
       << "}";
    return ss.str();
}

HeadphoneState HeadphoneState::makeDisconnected() {
    HeadphoneState s;
    s.schema_version = 1;
    s.connected = false;
    return s;
}

// ---------------------------------------------------------------------------
// Low-Level Framing, Escaping & Checksums
// ---------------------------------------------------------------------------

uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept {
    uint8_t sum = 0;
    for (uint8_t b : data) {
        sum += b;
    }
    return sum;
}

std::vector<uint8_t> escapeBytes(std::span<const uint8_t> unescaped) {
    std::vector<uint8_t> out;
    out.reserve(unescaped.size() * 2);
    for (uint8_t b : unescaped) {
        switch (b) {
            case kEndMarker:    out.push_back(kEscapeSentry); out.push_back(kEscaped3C); break;
            case kEscapeSentry: out.push_back(kEscapeSentry); out.push_back(kEscaped3D); break;
            case kStartMarker:  out.push_back(kEscapeSentry); out.push_back(kEscaped3E); break;
            default:            out.push_back(b); break;
        }
    }
    return out;
}

std::vector<uint8_t> unescapeBytes(std::span<const uint8_t> escaped) {
    std::vector<uint8_t> out;
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        uint8_t b = escaped[i];
        if (b == kEscapeSentry) {
            if (i + 1 >= escaped.size()) return {}; // Incomplete escape at EOF
            uint8_t next = escaped[++i];
            switch (next) {
                case kEscaped3C: out.push_back(kEndMarker); break;
                case kEscaped3D: out.push_back(kEscapeSentry); break;
                case kEscaped3E: out.push_back(kStartMarker); break;
                default: return {}; // Invalid escape sequence
            }
        } else {
            out.push_back(b);
        }
    }
    return out;
}

std::vector<uint8_t> packFrame(PacketType type, uint8_t seq, std::span<const uint8_t> payload) {
    std::vector<uint8_t> unescaped;
    unescaped.reserve(6 + payload.size() + 1);

    unescaped.push_back(static_cast<uint8_t>(type));
    unescaped.push_back(seq);

    // 4-byte Big-Endian Length
    uint32_t len = static_cast<uint32_t>(payload.size());
    unescaped.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>(len & 0xFF));

    // Payload
    unescaped.insert(unescaped.end(), payload.begin(), payload.end());

    // 8-bit additive Checksum (modulo 256 sum over all unescaped bytes before checksum)
    uint8_t csum = calculateChecksum(unescaped);
    unescaped.push_back(csum);

    // Escape and encapsulate with start/end markers
    std::vector<uint8_t> frame;
    frame.reserve(unescaped.size() * 2 + 2);
    frame.push_back(kStartMarker);
    std::vector<uint8_t> escaped = escapeBytes(unescaped);
    frame.insert(frame.end(), escaped.begin(), escaped.end());
    frame.push_back(kEndMarker);

    return frame;
}

std::optional<UnpackedFrame> unpackFrame(std::span<const uint8_t> frameBytes) {
    if (frameBytes.size() < 9) return std::nullopt;
    if (frameBytes.front() != kStartMarker || frameBytes.back() != kEndMarker) return std::nullopt;

    // Strip markers
    std::span<const uint8_t> inner = frameBytes.subspan(1, frameBytes.size() - 2);
    std::vector<uint8_t> unescaped = unescapeBytes(inner);
    if (unescaped.size() < 7) return std::nullopt; // Type(1) + Seq(1) + Len(4) + Csum(1)

    // Verify Checksum
    uint8_t receivedCsum = unescaped.back();
    std::span<const uint8_t> checkSpan(unescaped.data(), unescaped.size() - 1);
    if (calculateChecksum(checkSpan) != receivedCsum) return std::nullopt;

    PacketType type = static_cast<PacketType>(unescaped[0]);
    uint8_t seq = unescaped[1];
    uint32_t len = (static_cast<uint32_t>(unescaped[2]) << 24) |
                   (static_cast<uint32_t>(unescaped[3]) << 16) |
                   (static_cast<uint32_t>(unescaped[4]) << 8)  |
                   static_cast<uint32_t>(unescaped[5]);

    if (unescaped.size() - 7 != len) return std::nullopt;

    std::vector<uint8_t> payload(unescaped.begin() + 6, unescaped.begin() + 6 + len);
    return UnpackedFrame{type, seq, std::move(payload)};
}

// ---------------------------------------------------------------------------
// StreamFramer Implementation
// ---------------------------------------------------------------------------

void StreamFramer::append(std::span<const uint8_t> incoming) {
    constexpr size_t kMaxBufferSize = 64 * 1024; // 64 KB
    if (buffer_.size() + incoming.size() > kMaxBufferSize) {
        buffer_.clear(); // Discard corrupted unclosed stream data to prevent unbounded growth
    }
    buffer_.insert(buffer_.end(), incoming.begin(), incoming.end());
}

std::optional<std::vector<uint8_t>> StreamFramer::nextFrame() {
    auto startIt = std::find(buffer_.begin(), buffer_.end(), kStartMarker);
    if (startIt == buffer_.end()) {
        buffer_.clear();
        return std::nullopt;
    }
    if (startIt != buffer_.begin()) {
        buffer_.erase(buffer_.begin(), startIt);
        startIt = buffer_.begin();
    }

    auto endIt = std::find(startIt + 1, buffer_.end(), kEndMarker);
    if (endIt == buffer_.end()) {
        return std::nullopt; // Incomplete frame
    }

    std::vector<uint8_t> frame(startIt, endIt + 1);
    buffer_.erase(buffer_.begin(), endIt + 1);
    return frame;
}

void StreamFramer::reset() {
    buffer_.clear();
}

// ---------------------------------------------------------------------------
// Command Serializers (Host -> ULT WEAR / Sony MDR v2)
// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeACK(uint8_t rx_seq) {
    return packFrame(PacketType::ACK, static_cast<uint8_t>(1 - rx_seq), {});
}

std::vector<uint8_t> serializeNoiseMode(NoiseMode mode, uint8_t ambientLevel, bool voiceFocus, uint8_t seq) {
    // AmbientSoundControl2 (0x17). ULT WEAR accepts the common v2 level byte
    // but ignores it: ANC and Ambient are binary modes on this model.
    const uint8_t enabled = mode == NoiseMode::OFF ? 0x00 : 0x01;
    const uint8_t ambient = mode == NoiseMode::AMBIENT ? 0x01 : 0x00;
    const uint8_t voice = (mode == NoiseMode::AMBIENT && voiceFocus) ? 0x01 : 0x00;
    std::vector<uint8_t> payload = {
        0x68, 0x17, 0x01, enabled, ambient, 0x02, voice, 0x00
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeAmbientLevel(uint8_t level, bool voiceFocus, uint8_t seq) {
    (void)level;
    return serializeNoiseMode(NoiseMode::AMBIENT, level, voiceFocus, seq);
}

std::vector<uint8_t> serializeEqPreset(EqPreset preset, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0x58,                           // EQEBB_SET_PARAM
        kEqebbInquiredType,             // PRESET_EQ (0x01 on the XM3)
        static_cast<uint8_t>(preset),
        0x00                            // 0 band steps follow (preset selection only)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq) {
    auto clampVal = [](int v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, -10, 10) + 10);
    };

    std::vector<uint8_t> payload = {
        0x58,
        kEqebbInquiredType,
        static_cast<uint8_t>(EqPreset::CUSTOM), // 0xA0
        0x06,                                   // 6 steps follow
        clampVal(clearBass),
        clampVal(bands[0]),
        clampVal(bands[1]),
        clampVal(bands[2]),
        clampVal(bands[3]),
        clampVal(bands[4])
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeDsee(bool enabled, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0xE8, 0x01, static_cast<uint8_t>(enabled ? 0x01 : 0x00)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeUltMode(UltMode mode, uint8_t seq) {
    // ULT bass is multiplexed into EQEBB inquired type 0x03. Preserve a flat
    // six-band payload, as captured from the WH-ULT900N.
    std::vector<uint8_t> payload = {
        0x58, 0x03, 0x00, static_cast<uint8_t>(mode), 0x06,
        0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

// ---------------------------------------------------------------------------
// Query Serializers
// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeHandshake(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x00, 0x00 }; // CONNECT_GET_PROTOCOL_INFO
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryDeviceName(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x04, 0x01 }; // CONNECT_GET_DEVICE_INFO, MODEL_NAME
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryBattery(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x22, 0x00 }; // POWER_GET_STATUS, BATTERY (v2)
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryNoiseMode(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x66, 0x17 }; // AmbientSoundControl2
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryEq(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x56, 0x00 }; // Legacy preset-EQ query
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryDsee(uint8_t seq) {
    std::vector<uint8_t> payload = { 0xE6, 0x01 }; // AUDIO_GET_PARAM, UPSCALING
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryUltMode(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x56, 0x03 };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

// ---------------------------------------------------------------------------
// Inbound State Deserializer (ULT WEAR -> Host)
// ---------------------------------------------------------------------------

bool parseInboundPayload(std::span<const uint8_t> payload, HeadphoneState& state) {
    if (payload.empty()) return false;
    uint8_t cmd = payload[0];
    bool updated = false;

    // 1. Model name (CONNECT_RET_DEVICE_INFO): 05 01 <len> <ascii...>
    if (cmd == 0x05 && payload.size() >= 3 && payload[1] == 0x01) {
        size_t len = payload[2];
        if (len > 0 && payload.size() >= 3 + len) {
            state.device_name.assign(reinterpret_cast<const char*>(payload.data() + 3), len);
            updated = true;
        }
    }
    // 2. Battery (POWER_RET_STATUS / POWER_NTFY_STATUS): 23|25 00 <level> <charging>
    else if ((cmd == 0x23 || cmd == 0x25) && payload.size() >= 4 && payload[1] == 0x00) {
        state.battery_level = payload[2];
        state.battery_charging = (payload[3] == 0x01);
        updated = true;
    }
    // 3. AmbientSoundControl2: 67|69 17 01 <enabled> <ambient> <reserved> <voice>
    else if ((cmd == 0x67 || cmd == 0x69) && payload.size() >= 7 && payload[1] == 0x17) {
        if (payload[3] == 0x00) {
            state.noise_mode = "off";
        } else if (payload[4] == 0x01) {
            state.noise_mode = "ambient";
        } else {
            state.noise_mode = "anc";
        }
        state.ambient_sound_level = 0;
        state.voice_passthrough = payload[6] == 0x01;
        updated = true;
    }
    // 4. ULT mode: 57|59 03 <eqPreset> <ultMode> 06 <six bands>
    else if ((cmd == 0x57 || cmd == 0x59) && payload.size() >= 5 && payload[1] == 0x03) {
        state.ult_mode = std::clamp(static_cast<int>(payload[3]), 0, 2);
        updated = true;
    }
    // 4. Equalizer (EQEBB_RET_PARAM / EQEBB_NTFY_PARAM):
    //    57|59 01 <preset> 06 <clearBass> <band0..band4>
    else if ((cmd == 0x57 || cmd == 0x59) && payload.size() >= 3 && payload[1] == kEqebbInquiredType) {
        state.eq_preset = eqPresetToString(static_cast<EqPreset>(payload[2]));
        if (payload.size() >= 10 && payload[3] == 0x06) {
            state.clear_bass = static_cast<int>(payload[4]) - 10;
            for (size_t i = 0; i < 5; ++i) {
                state.eq_custom_bands[i] = static_cast<int>(payload[5 + i]) - 10;
            }
        }
        updated = true;
    }
    // 6. DSEE (AUDIO_RET_PARAM / AUDIO_NTFY_PARAM): e7|e9 01 <value>
    else if ((cmd == 0xE7 || cmd == 0xE9) && payload.size() >= 3 && payload[1] == 0x01) {
        state.dsee_extreme = (payload[2] == 0x01);
        updated = true;
    }
    // Alternative single-byte dispatch (compact representations used by mocks)
    else if (cmd == 0x10 && payload.size() >= 3) { // Battery: [0x10, level, charging]
        state.battery_level = payload[1];
        state.battery_charging = (payload[2] != 0);
        updated = true;
    } else if (cmd == 0x11 && payload.size() >= 4) { // NC/ASM: [0x11, mode, level, voiceFocus]
        state.noise_mode = noiseModeToString(static_cast<NoiseMode>(payload[1]));
        state.ambient_sound_level = payload[2];
        state.voice_passthrough = (payload[3] != 0);
        updated = true;
    } else if (cmd == 0x12 && payload.size() >= 2) { // EQ Preset: [0x12, preset]
        state.eq_preset = eqPresetToString(static_cast<EqPreset>(payload[1]));
        updated = true;
    } else if (cmd == 0x13 && payload.size() >= 7) { // Custom EQ: [0x13, b0..b4, clearBass]
        for (size_t i = 0; i < 5; ++i) {
            state.eq_custom_bands[i] = static_cast<int>(payload[1 + i]) - 10;
        }
        state.clear_bass = static_cast<int>(payload[6]) - 10;
        updated = true;
    } else if (cmd == 0x14 && payload.size() >= 2) { // Toggles: [0x14, dsee]
        state.dsee_extreme = (payload[1] != 0);
        updated = true;
    }

    if (updated) {
        state.connected = true;
        state.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    return updated;
}

// ---------------------------------------------------------------------------
// Enum <-> String Helpers
// ---------------------------------------------------------------------------

std::string eqPresetToString(EqPreset preset) {
    switch (preset) {
        case EqPreset::OFF:     return "off";
        case EqPreset::BRIGHT:  return "bright";
        case EqPreset::EXCITED: return "excited";
        case EqPreset::MELLOW:  return "mellow";
        case EqPreset::RELAXED: return "relaxed";
        case EqPreset::VOCAL:   return "vocal";
        case EqPreset::TREBLE:  return "treble";
        case EqPreset::BASS:    return "bass";
        case EqPreset::SPEECH:  return "speech";
        case EqPreset::CUSTOM:  return "custom";
        default:                return "off";
    }
}

EqPreset stringToEqPreset(const std::string& str) {
    if (str == "off")     return EqPreset::OFF;
    if (str == "bright")  return EqPreset::BRIGHT;
    if (str == "excited") return EqPreset::EXCITED;
    if (str == "mellow")  return EqPreset::MELLOW;
    if (str == "relaxed") return EqPreset::RELAXED;
    if (str == "vocal")   return EqPreset::VOCAL;
    if (str == "treble")  return EqPreset::TREBLE;
    if (str == "bass")    return EqPreset::BASS;
    if (str == "speech")  return EqPreset::SPEECH;
    if (str == "custom")  return EqPreset::CUSTOM;
    return EqPreset::OFF;
}

std::string noiseModeToString(NoiseMode mode) {
    switch (mode) {
        case NoiseMode::OFF:     return "off";
        case NoiseMode::ANC:     return "anc";
        case NoiseMode::AMBIENT: return "ambient";
        default:                 return "off";
    }
}

NoiseMode stringToNoiseMode(const std::string& str) {
    if (str == "anc")     return NoiseMode::ANC;
    if (str == "ambient") return NoiseMode::AMBIENT;
    if (str == "off")     return NoiseMode::OFF;
    return NoiseMode::ANC;
}

} // namespace omarchy::sony::protocol
