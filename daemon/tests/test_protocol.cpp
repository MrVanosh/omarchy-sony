#include "MDRProtocol.hpp"
#include "BluetoothManager.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace omarchy::sony;
using namespace omarchy::sony::protocol;

static int gFailedTests = 0;
static int gTotalTests = 0;

#define TEST_CASE(name) \
    std::cout << "[ RUN      ] " << name << std::endl; \
    gTotalTests++;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[  FAILED  ] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(actual, expected, msg) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "[  FAILED  ] " << msg << " | Expected: " << (expected) \
                      << ", Actual: " << (actual) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_PASS(name) \
    std::cout << "[       OK ] " << name << std::endl;

// ---------------------------------------------------------------------------
// 1. Checksum Verification Tests
// ---------------------------------------------------------------------------
void testChecksumCalculation() {
    TEST_CASE("ChecksumCalculation");

    // Standard vector matching WH-1000XM3 physical capture
    std::vector<uint8_t> data = {0x0C, 0x01, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x03, 0x00, 0x30, 0x18, 0x00, 0x00};
    uint8_t csum = calculateChecksum(data);
    // 12 + 1 + 8 + 1 + 3 + 48 + 24 = 97 = 0x61
    TEST_ASSERT_EQ(csum, 0x61, "Checksum must match physical capture modulo 256 sum");

    // Test modulo 256 overflow
    std::vector<uint8_t> overflowData = {0xFF, 0x02}; // 255 + 2 = 257 = 1 (mod 256)
    TEST_ASSERT_EQ(calculateChecksum(overflowData), 0x01, "Checksum must wrap at 256");

    // Empty vector
    std::vector<uint8_t> emptyData;
    TEST_ASSERT_EQ(calculateChecksum(emptyData), 0x00, "Empty checksum is 0");

    TEST_PASS("ChecksumCalculation");
}

// ---------------------------------------------------------------------------
// 2. Escaping & Delimiter Sentry Tests
// ---------------------------------------------------------------------------
void testEscapingAndUnescaping() {
    TEST_CASE("EscapingAndUnescaping");

    // Bytes containing delimiters 0x3C, 0x3D, 0x3E
    std::vector<uint8_t> raw = {0x10, 0x3C, 0x20, 0x3D, 0x30, 0x3E, 0x40};
    auto escaped = escapeBytes(raw);

    // 0x3C -> 0x3D 0x2C
    // 0x3D -> 0x3D 0x2D
    // 0x3E -> 0x3D 0x2E
    // Original 7 bytes with 3 escaped bytes should result in 7 + 3 = 10 bytes
    TEST_ASSERT_EQ(escaped.size(), static_cast<size_t>(10), "Escaped length should account for sentry expansions");

    auto unescaped = unescapeBytes(escaped);
    TEST_ASSERT(unescaped == raw, "Roundtrip escape and unescape must reproduce original bytes");

    // Edge case: Corrupt trailing escape sentry 0x3D at end of buffer
    std::vector<uint8_t> corruptTrailing = {0x01, 0x3D};
    auto corruptUnescape = unescapeBytes(corruptTrailing);
    TEST_ASSERT(corruptUnescape.empty(), "Unescape must reject trailing dangling escape sentry");

    // Edge case: Invalid escape sequence (0x3D followed by unexpected byte)
    std::vector<uint8_t> corruptInvalid = {0x01, 0x3D, 0xAA, 0x02};
    auto corruptInvalidUnescape = unescapeBytes(corruptInvalid);
    TEST_ASSERT(corruptInvalidUnescape.empty(), "Unescape must reject invalid escape sequence");

    TEST_PASS("EscapingAndUnescaping");
}

// ---------------------------------------------------------------------------
// 3. Packet Packing, Unpacking & Integrity Tests
// ---------------------------------------------------------------------------
void testPacketFraming() {
    TEST_CASE("PacketFraming");

    std::vector<uint8_t> payload = {0x68, 0x17, 0x01, 0x01, 0x00, 0x00, 0x00};
    auto frame = packFrame(PacketType::DATA_MDR, 1, payload);

    TEST_ASSERT(frame.size() >= 9, "Frame must be at least 9 bytes");
    TEST_ASSERT_EQ(frame.front(), kStartMarker, "Frame must start with 0x3E");
    TEST_ASSERT_EQ(frame.back(), kEndMarker, "Frame must end with 0x3C");

    // Unpack valid frame
    auto unpacked = unpackFrame(frame);
    TEST_ASSERT(unpacked.has_value(), "Valid frame must unpack successfully");
    TEST_ASSERT(unpacked->type == PacketType::DATA_MDR, "Packet type must match DATA_MDR");
    TEST_ASSERT_EQ(unpacked->seq, 1, "Sequence number must match");
    TEST_ASSERT(unpacked->payload == payload, "Unpacked payload must match original");

    // Corrupted checksum test: tamper with one byte in the frame
    std::vector<uint8_t> tampered = frame;
    tampered[4] ^= 0xFF;
    auto tamperedUnpack = unpackFrame(tampered);
    TEST_ASSERT(!tamperedUnpack.has_value(), "Tampered frame must fail checksum verification");

    // Missing start marker
    std::vector<uint8_t> noStart = frame;
    noStart[0] = 0x00;
    TEST_ASSERT(!unpackFrame(noStart).has_value(), "Missing start marker must be rejected");

    // Missing end marker
    std::vector<uint8_t> noEnd = frame;
    noEnd.back() = 0x00;
    TEST_ASSERT(!unpackFrame(noEnd).has_value(), "Missing end marker must be rejected");

    // Too short frame (< 9 bytes)
    std::vector<uint8_t> tooShort = {0x3E, 0x0C, 0x3C};
    TEST_ASSERT(!unpackFrame(tooShort).has_value(), "Short frame must be rejected");

    TEST_PASS("PacketFraming");
}

// ---------------------------------------------------------------------------
// 4. StreamFramer Chunk Fragmentation & Coalescing Tests
// ---------------------------------------------------------------------------
void testStreamFramer() {
    TEST_CASE("StreamFramer");

    StreamFramer framer;
    std::vector<uint8_t> payload1 = {0xAA, 0xBB, 0xCC};
    std::vector<uint8_t> payload2 = {0x11, 0x22, 0x33, 0x44};

    auto frame1 = packFrame(PacketType::DATA_MDR, 0, payload1);
    auto frame2 = packFrame(PacketType::DATA_MDR, 1, payload2);

    // Test 1: Fragmented feed - 1 byte at a time
    for (size_t i = 0; i < frame1.size() - 1; ++i) {
        uint8_t b = frame1[i];
        framer.append(std::span<const uint8_t>(&b, 1));
        auto pending = framer.nextFrame();
        TEST_ASSERT(!pending.has_value(), "Frame must not complete before final delimiter");
    }

    // Feed the last byte
    uint8_t lastByte = frame1.back();
    framer.append(std::span<const uint8_t>(&lastByte, 1));
    auto extracted1 = framer.nextFrame();
    TEST_ASSERT(extracted1.has_value(), "Frame 1 must complete on final byte");
    TEST_ASSERT(*extracted1 == frame1, "Extracted frame must match frame 1 exactly");

    // Test 2: Coalesced packets + leading garbage
    std::vector<uint8_t> combined;
    combined.push_back(0xFF); // Garbage preceding start
    combined.push_back(0xDE);
    combined.insert(combined.end(), frame1.begin(), frame1.end());
    combined.insert(combined.end(), frame2.begin(), frame2.end());

    framer.append(combined);
    auto res1 = framer.nextFrame();
    TEST_ASSERT(res1.has_value(), "First coalesced frame must be extracted");
    TEST_ASSERT(*res1 == frame1, "First frame content must match");

    auto res2 = framer.nextFrame();
    TEST_ASSERT(res2.has_value(), "Second coalesced frame must be extracted");
    TEST_ASSERT(*res2 == frame2, "Second frame content must match");

    auto res3 = framer.nextFrame();
    TEST_ASSERT(!res3.has_value(), "Framer must be empty after all frames extracted");

    // Reset test
    framer.append(frame1);
    framer.reset();
    TEST_ASSERT_EQ(framer.bufferedBytes(), static_cast<size_t>(0), "Framer buffer must be empty after reset");

    TEST_PASS("StreamFramer");
}

// ---------------------------------------------------------------------------
// 5. Command Serializers Tests
// ---------------------------------------------------------------------------
void testCommandSerializers() {
    TEST_CASE("CommandSerializers");

    // Noise Mode: ANC
    // ULT WEAR AmbientSoundControl2: 68 17 01 <enabled> <ambient> 02 <voice> 00
    auto ancFrame = serializeNoiseMode(NoiseMode::ANC, 0, false, 0);
    auto unpAnc = unpackFrame(ancFrame);
    TEST_ASSERT(unpAnc.has_value(), "ANC frame must be valid");
    TEST_ASSERT_EQ(unpAnc->payload[0], 0x68, "Opcode must be NCASM_SET_PARAM (0x68)");
    TEST_ASSERT_EQ(unpAnc->payload[1], 0x17, "Type must be AmbientSoundControl2 (0x17)");
    TEST_ASSERT_EQ(unpAnc->payload[3], 0x01, "ANC must enable noise control");
    TEST_ASSERT_EQ(unpAnc->payload[4], 0x00, "ANC must disable ambient mode");
    TEST_ASSERT_EQ(unpAnc->payload[7], 0x00, "ANC carries ambient level 0");

    // Noise Mode: Ambient Level 15 with voice focus
    auto ambFrame = serializeAmbientLevel(15, true, 1);
    auto unpAmb = unpackFrame(ambFrame);
    TEST_ASSERT(unpAmb.has_value(), "Ambient frame must be valid");
    TEST_ASSERT_EQ(unpAmb->payload[3], 0x01, "Ambient must enable noise control");
    TEST_ASSERT_EQ(unpAmb->payload[4], 0x01, "Ambient mode byte must be ON");
    TEST_ASSERT_EQ(unpAmb->payload[6], 0x01, "Voice Focus must be ON");
    TEST_ASSERT_EQ(unpAmb->payload[7], 0x00, "ULT WEAR ignores graduated ambient levels");

    // ULT WEAR ignores the level field.
    auto clampedAmb = serializeAmbientLevel(50, false, 0);
    auto unpClampedAmb = unpackFrame(clampedAmb);
    TEST_ASSERT(unpClampedAmb.has_value(), "Clamped ambient frame valid");
    TEST_ASSERT_EQ(unpClampedAmb->payload[7], 0, "Ambient level must stay zero");

    // Ambient remains binary at level 0.
    auto zeroAmb = serializeAmbientLevel(0, false, 0);
    auto unpZeroAmb = unpackFrame(zeroAmb);
    TEST_ASSERT(unpZeroAmb.has_value(), "Zero-level ambient frame must be valid");
    TEST_ASSERT_EQ(unpZeroAmb->payload[4], 0x01, "Ambient mode must stay enabled");
    TEST_ASSERT_EQ(unpZeroAmb->payload[7], 0x00, "ambient level must be 0");

    // Noise Mode: Off
    auto offFrame = serializeNoiseMode(NoiseMode::OFF, 0, false, 0);
    auto unpOff = unpackFrame(offFrame);
    TEST_ASSERT(unpOff.has_value(), "Off frame must be valid");
    TEST_ASSERT_EQ(unpOff->payload[3], 0x00, "Noise control must be OFF");

    // EQ Preset: Vocal
    auto eqVocal = serializeEqPreset(EqPreset::VOCAL, 0);
    auto unpVocal = unpackFrame(eqVocal);
    TEST_ASSERT(unpVocal.has_value(), "EQ Preset frame must be valid");
    TEST_ASSERT_EQ(unpVocal->payload[0], 0x58, "Opcode must be EQEBB_SET_PARAM (0x58)");
    TEST_ASSERT_EQ(unpVocal->payload[1], 0x00, "Legacy preset EQ inquired type must be 0x00");
    TEST_ASSERT_EQ(unpVocal->payload[2], static_cast<uint8_t>(EqPreset::VOCAL), "Preset must match Vocal (0x14)");

    // Custom EQ: [-10, 0, 10, -5, 5] and clear bass 3
    std::array<int, 5> bands = {-10, 0, 10, -5, 5};
    auto eqCustom = serializeCustomEq(bands, 3, 1);
    auto unpCustom = unpackFrame(eqCustom);
    TEST_ASSERT(unpCustom.has_value(), "Custom EQ frame must be valid");
    TEST_ASSERT_EQ(unpCustom->payload[2], static_cast<uint8_t>(EqPreset::CUSTOM), "Preset must be CUSTOM (0xA0)");
    TEST_ASSERT_EQ(unpCustom->payload[3], 0x06, "6 steps must follow");
    TEST_ASSERT_EQ(unpCustom->payload[4], 13, "Clear Bass 3 + 10 = 13");
    TEST_ASSERT_EQ(unpCustom->payload[5], 0, "Band 0: -10 + 10 = 0");
    TEST_ASSERT_EQ(unpCustom->payload[6], 10, "Band 1: 0 + 10 = 10");
    TEST_ASSERT_EQ(unpCustom->payload[7], 20, "Band 2: 10 + 10 = 20");
    TEST_ASSERT_EQ(unpCustom->payload[8], 5, "Band 3: -5 + 10 = 5");
    TEST_ASSERT_EQ(unpCustom->payload[9], 15, "Band 4: 5 + 10 = 15");

    // Custom EQ: Clamping test (out of bounds [-15, 15])
    std::array<int, 5> outOfBoundsBands = {-15, -10, 0, 10, 15};
    auto eqClamped = serializeCustomEq(outOfBoundsBands, 20, 0);
    auto unpEqClamped = unpackFrame(eqClamped);
    TEST_ASSERT(unpEqClamped.has_value(), "Clamped EQ frame valid");
    TEST_ASSERT_EQ(unpEqClamped->payload[4], 20, "Clear Bass clamped to +10 -> 20");
    TEST_ASSERT_EQ(unpEqClamped->payload[5], 0, "Band 0 clamped to -10 -> 0");
    TEST_ASSERT_EQ(unpEqClamped->payload[9], 20, "Band 4 clamped to +10 -> 20");

    // Feature Toggles
    auto dseeOn = serializeDsee(true, 0);
    auto unpDsee = unpackFrame(dseeOn);
    TEST_ASSERT(unpDsee.has_value(), "DSEE frame valid");
    TEST_ASSERT_EQ(unpDsee->payload[0], 0xE8, "Opcode must be AUDIO_SET_PARAM (0xE8)");
    TEST_ASSERT_EQ(unpDsee->payload[1], 0x01, "Type must be UPSCALING (0x01)");
    TEST_ASSERT_EQ(unpDsee->payload[2], 0x01, "DSEE ON is wire byte 0x01");

    auto dseeOff = serializeDsee(false, 0);
    auto unpDseeOff = unpackFrame(dseeOff);
    TEST_ASSERT(unpDseeOff.has_value(), "DSEE off frame valid");
    TEST_ASSERT_EQ(unpDseeOff->payload[2], 0x00, "DSEE OFF is wire byte 0x00");

    auto ult2 = unpackFrame(serializeUltMode(UltMode::ULT2, 1));
    TEST_ASSERT(ult2.has_value(), "ULT mode frame valid");
    TEST_ASSERT_EQ(ult2->payload[0], 0x58, "ULT uses EQEBB_SET_PARAM");
    TEST_ASSERT_EQ(ult2->payload[1], 0x03, "ULT inquired type is 0x03");
    TEST_ASSERT_EQ(ult2->payload[3], 0x02, "ULT 2 mode byte");

    // ACK Packet
    auto ackFrame = serializeACK(1);
    auto unpAck = unpackFrame(ackFrame);
    TEST_ASSERT(unpAck.has_value(), "ACK frame valid");
    TEST_ASSERT(unpAck->type == PacketType::ACK, "Packet type must be ACK");
    TEST_ASSERT_EQ(unpAck->seq, 0, "ACK sequence should toggle 1 - rx_seq");

    TEST_PASS("CommandSerializers");
}

// ---------------------------------------------------------------------------
// 6. Query Serializers Tests
// ---------------------------------------------------------------------------
void testQuerySerializers() {
    TEST_CASE("QuerySerializers");

    // Handshake (CONNECT_GET_PROTOCOL_INFO): must open every session
    auto qHs = serializeHandshake(0);
    auto unpHs = unpackFrame(qHs);
    TEST_ASSERT(unpHs.has_value(), "Handshake valid");
    TEST_ASSERT_EQ(unpHs->payload[0], 0x00, "CONNECT_GET_PROTOCOL_INFO opcode");
    TEST_ASSERT_EQ(unpHs->payload[1], 0x00, "FIXED_VALUE type");

    // Query Device Name
    auto qName = serializeQueryDeviceName(0);
    auto unpName = unpackFrame(qName);
    TEST_ASSERT(unpName.has_value(), "Device name query valid");
    TEST_ASSERT_EQ(unpName->payload[0], 0x04, "CONNECT_GET_DEVICE_INFO opcode");
    TEST_ASSERT_EQ(unpName->payload[1], 0x01, "MODEL_NAME type");

    // Query Battery
    auto qBat = serializeQueryBattery(0);
    auto unpBat = unpackFrame(qBat);
    TEST_ASSERT(unpBat.has_value(), "Battery query valid");
    TEST_ASSERT_EQ(unpBat->payload[0], 0x22, "POWER_GET_STATUS opcode (v2)");
    TEST_ASSERT_EQ(unpBat->payload[1], 0x00, "BATTERY type");

    // Query Noise Mode
    auto qNc = serializeQueryNoiseMode(0);
    auto unpNc = unpackFrame(qNc);
    TEST_ASSERT(unpNc.has_value(), "Noise query valid");
    TEST_ASSERT_EQ(unpNc->payload[0], 0x66, "NCASM_GET_PARAM opcode");
    TEST_ASSERT_EQ(unpNc->payload[1], 0x17, "AmbientSoundControl2 type");

    // Query EQ
    auto qEq = serializeQueryEq(0);
    auto unpEq = unpackFrame(qEq);
    TEST_ASSERT(unpEq.has_value(), "EQ query valid");
    TEST_ASSERT_EQ(unpEq->payload[0], 0x56, "EQEBB_GET_PARAM opcode");
    TEST_ASSERT_EQ(unpEq->payload[1], 0x00, "Preset EQ inquired type 0x00");

    auto qUlt = unpackFrame(serializeQueryUltMode(0));
    TEST_ASSERT(qUlt.has_value(), "ULT query valid");
    TEST_ASSERT_EQ(qUlt->payload[0], 0x56, "ULT uses EQEBB_GET_PARAM");
    TEST_ASSERT_EQ(qUlt->payload[1], 0x03, "ULT query inquired type 0x03");

    // Query DSEE
    auto qDsee = serializeQueryDsee(0);
    auto unpDseeQ = unpackFrame(qDsee);
    TEST_ASSERT(unpDseeQ.has_value(), "DSEE query valid");
    TEST_ASSERT_EQ(unpDseeQ->payload[0], 0xE6, "AUDIO_GET_PARAM opcode");
    TEST_ASSERT_EQ(unpDseeQ->payload[1], 0x01, "UPSCALING type");

    TEST_PASS("QuerySerializers");
}

// ---------------------------------------------------------------------------
// 7. Inbound State Parser & Real Capture Tests
// ---------------------------------------------------------------------------
void testInboundStateParser() {
    TEST_CASE("InboundStateParser");

    HeadphoneState state;

    // 1. Battery status notification: 85% charging (v2 POWER_NTFY_STATUS)
    std::vector<uint8_t> batteryPayload = {0x25, 0x00, 85, 0x01};
    TEST_ASSERT(parseInboundPayload(batteryPayload, state), "Battery payload must be parsed");
    TEST_ASSERT_EQ(state.battery_level, 85, "Battery level must be 85");
    TEST_ASSERT(state.battery_charging, "Battery charging must be true");
    TEST_ASSERT(state.connected, "State must mark connected true");

    // 2. Model name
    std::vector<uint8_t> namePayload = {0x05, 0x01, 0x08, 'U','L','T',' ','W','E','A','R'};
    TEST_ASSERT(parseInboundPayload(namePayload, state), "Model name payload must be parsed");
    TEST_ASSERT_EQ(state.device_name, std::string("ULT WEAR"), "Device name must be ULT WEAR");

    // 3. ULT WEAR: everything off
    std::vector<uint8_t> ncPayload = {0x67, 0x17, 0x01, 0x00, 0x00, 0x01, 0x00};
    TEST_ASSERT(parseInboundPayload(ncPayload, state), "NC payload must be parsed");
    TEST_ASSERT_EQ(state.noise_mode, std::string("off"), "Noise mode must be off");

    // 4. ULT WEAR ANC
    std::vector<uint8_t> ancPayload = {0x69, 0x17, 0x01, 0x01, 0x00, 0x01, 0x00};
    TEST_ASSERT(parseInboundPayload(ancPayload, state), "ANC payload must be parsed");
    TEST_ASSERT_EQ(state.noise_mode, std::string("anc"), "Noise mode must be anc");

    // 5. ULT WEAR ambient with Focus on Voice
    std::vector<uint8_t> ambPayload = {0x69, 0x17, 0x01, 0x01, 0x01, 0x01, 0x01};
    TEST_ASSERT(parseInboundPayload(ambPayload, state), "Ambient payload must be parsed");
    TEST_ASSERT_EQ(state.noise_mode, std::string("ambient"), "Noise mode must be ambient");
    TEST_ASSERT_EQ(state.ambient_sound_level, 0, "ULT WEAR has no ambient level");
    TEST_ASSERT(state.voice_passthrough, "Voice passthrough must be true");

    // 6. ULT 2 status
    std::vector<uint8_t> ultPayload = {0x57, 0x03, 0x00, 0x02, 0x06, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A};
    TEST_ASSERT(parseInboundPayload(ultPayload, state), "ULT payload must be parsed");
    TEST_ASSERT_EQ(state.ult_mode, 2, "ULT mode must be 2");

    // 7. DSEE on -> e7 01 01
    std::vector<uint8_t> dseePayload = {0xE7, 0x01, 0x01};
    TEST_ASSERT(parseInboundPayload(dseePayload, state), "DSEE payload must be parsed");
    TEST_ASSERT(state.dsee_extreme, "DSEE must be true");

    std::vector<uint8_t> dseeOffPayload = {0xE9, 0x01, 0x00};
    TEST_ASSERT(parseInboundPayload(dseeOffPayload, state), "DSEE off payload must be parsed");
    TEST_ASSERT(!state.dsee_extreme, "DSEE must be false");

    // Alternative compact packet format (0x10, 0x11, 0x12, 0x13, 0x14)
    std::vector<uint8_t> compactBat = {0x10, 92, 0x00};
    TEST_ASSERT(parseInboundPayload(compactBat, state), "Compact battery parsed");
    TEST_ASSERT_EQ(state.battery_level, 92, "Compact battery level");
    TEST_ASSERT(!state.battery_charging, "Compact battery not charging");

    // Verify JSON Output conforms to contract
    std::string json = state.toJson();
    TEST_ASSERT(json.find("\"schema_version\":1") != std::string::npos, "JSON must contain schema_version: 1");
    TEST_ASSERT(json.find("\"connected\":true") != std::string::npos, "JSON must contain connected: true");
    TEST_ASSERT(json.find("\"battery_level\":92") != std::string::npos, "JSON must contain battery_level");
    TEST_ASSERT(json.find("\"noise_mode\":\"ambient\"") != std::string::npos, "JSON must contain noise_mode");
    TEST_ASSERT(json.find("speak_to_chat") == std::string::npos, "JSON must not advertise XM5-only speak_to_chat");
    TEST_ASSERT(json.find("multipoint") == std::string::npos, "JSON must not advertise XM5-only multipoint");
    TEST_ASSERT(json.find("\"ult_mode\":2") != std::string::npos, "JSON must contain ULT mode");

    // Verify disconnected JSON
    HeadphoneState disc = HeadphoneState::makeDisconnected();
    TEST_ASSERT_EQ(disc.toJson(), std::string("{\"schema_version\":1,\"connected\":false}"), "Disconnected JSON format");

    TEST_PASS("InboundStateParser");
}

// ---------------------------------------------------------------------------
// 8. String & Enum Helpers Tests
// ---------------------------------------------------------------------------
void testStringAndEnumHelpers() {
    TEST_CASE("StringAndEnumHelpers");

    // Noise Mode string mapping
    TEST_ASSERT_EQ(noiseModeToString(NoiseMode::OFF), std::string("off"), "Off mode string");
    TEST_ASSERT_EQ(noiseModeToString(NoiseMode::ANC), std::string("anc"), "ANC mode string");
    TEST_ASSERT_EQ(noiseModeToString(NoiseMode::AMBIENT), std::string("ambient"), "Ambient mode string");

    TEST_ASSERT(stringToNoiseMode("anc") == NoiseMode::ANC, "StringToNoiseMode anc");
    TEST_ASSERT(stringToNoiseMode("ambient") == NoiseMode::AMBIENT, "StringToNoiseMode ambient");
    TEST_ASSERT(stringToNoiseMode("wind") == NoiseMode::ANC, "Wind is unsupported on the XM3, falls back to anc");
    TEST_ASSERT(stringToNoiseMode("off") == NoiseMode::OFF, "StringToNoiseMode off");
    TEST_ASSERT(stringToNoiseMode("unknown") == NoiseMode::ANC, "StringToNoiseMode fallback to anc");

    // EQ Preset string mapping
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::OFF), std::string("off"), "EQ off");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::BRIGHT), std::string("bright"), "EQ bright");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::EXCITED), std::string("excited"), "EQ excited");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::MELLOW), std::string("mellow"), "EQ mellow");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::RELAXED), std::string("relaxed"), "EQ relaxed");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::VOCAL), std::string("vocal"), "EQ vocal");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::TREBLE), std::string("treble"), "EQ treble");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::BASS), std::string("bass"), "EQ bass");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::SPEECH), std::string("speech"), "EQ speech");
    TEST_ASSERT_EQ(eqPresetToString(EqPreset::CUSTOM), std::string("custom"), "EQ custom");

    TEST_ASSERT(stringToEqPreset("vocal") == EqPreset::VOCAL, "StringToEqPreset vocal");
    TEST_ASSERT(stringToEqPreset("custom") == EqPreset::CUSTOM, "StringToEqPreset custom");
    TEST_ASSERT(stringToEqPreset("unknown") == EqPreset::OFF, "StringToEqPreset fallback to off");

    TEST_PASS("StringAndEnumHelpers");
}

// ---------------------------------------------------------------------------
// 9. Mock Transport & BluetoothManager Lifecycle Tests
// ---------------------------------------------------------------------------
void testMockTransportAndBluetoothManager() {
    TEST_CASE("MockTransportAndBluetoothManager");

    int peerFd = -1;
    BluetoothConfig config;
    config.initialBackoffMs = 50;
    config.maxBackoffMs = 200;
    auto manager = BluetoothManager::createMock(config, &peerFd);
    TEST_ASSERT(manager != nullptr, "BluetoothManager must be created");
    TEST_ASSERT(peerFd >= 0, "Mock peer socketpair fd must be valid");

    bool connectedCalled = false;
    bool disconnectedCalled = false;
    std::vector<uint8_t> receivedBytes;

    BluetoothCallbacks callbacks;
    callbacks.onConnected = [&]() { connectedCalled = true; };
    callbacks.onDisconnected = [&](const std::string&) { disconnectedCalled = true; };
    callbacks.onDataReceived = [&](const uint8_t* data, size_t len) {
        receivedBytes.insert(receivedBytes.end(), data, data + len);
    };
    manager->setCallbacks(callbacks);

    // Initial state is DISCONNECTED
    TEST_ASSERT(manager->getState() == ConnectionState::DISCONNECTED, "Initial state DISCONNECTED");

    // Start -> discovery -> sdp -> connect
    manager->start();
    TEST_ASSERT(manager->getState() == ConnectionState::CONNECTED, "State must transition to CONNECTED in mock mode");
    TEST_ASSERT(connectedCalled, "onConnected callback must be triggered");

    // Verify polling fd
    int pollFd = manager->getPollFd();
    TEST_ASSERT(pollFd >= 0, "Poll fd must be valid when connected");
    short pollEvents = manager->getPollEvents();
    TEST_ASSERT(pollEvents & POLLIN, "Poll events must include POLLIN");

    // Test sending packet from manager to peerFd
    std::vector<uint8_t> testPacket = {0x3E, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x01, 0x55, 0x63, 0x3C};
    bool sendOk = manager->sendPacket(testPacket);
    TEST_ASSERT(sendOk, "sendPacket must succeed");

    // Read from peerFd and verify
    uint8_t readBuf[128];
    ssize_t nRead = ::recv(peerFd, readBuf, sizeof(readBuf), 0);
    TEST_ASSERT_EQ(nRead, static_cast<ssize_t>(testPacket.size()), "Peer must receive exact packet bytes");
    TEST_ASSERT(std::memcmp(readBuf, testPacket.data(), testPacket.size()) == 0, "Received bytes match sent packet");

    // Test sending data from peerFd to manager
    std::vector<uint8_t> inboundPacket = {0x3E, 0x0C, 0x00, 0x00, 0x00, 0x01, 0xAA, 0xB7, 0x3C};
    ssize_t nSent = ::send(peerFd, inboundPacket.data(), inboundPacket.size(), 0);
    TEST_ASSERT_EQ(nSent, static_cast<ssize_t>(inboundPacket.size()), "Peer sent bytes");

    // Trigger handleSocketEvent with POLLIN
    manager->handleSocketEvent(POLLIN);
    TEST_ASSERT(receivedBytes == inboundPacket, "Manager onDataReceived must receive inbound bytes");

    // Test remote disconnect simulation
    // Closing peerFd causes POLLHUP/EOF on manager side
    ::close(peerFd);
    peerFd = -1;

    manager->handleSocketEvent(POLLHUP);
    TEST_ASSERT(manager->getState() == ConnectionState::RECONNECT_BACKOFF, "State must transition to RECONNECT_BACKOFF on hangup");
    TEST_ASSERT(disconnectedCalled, "onDisconnected callback must be triggered");

    // Test backoff tick retry
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    manager->tick();
    // After backoff timer expires, tick() should transition through discovery and reconnect
    TEST_ASSERT(manager->getState() == ConnectionState::CONNECTED, "After backoff timer, tick must trigger reconnect to mock");

    TEST_PASS("MockTransportAndBluetoothManager");
}

// ---------------------------------------------------------------------------
// 10. StateEngine Persistence & Permissions Tests
// ---------------------------------------------------------------------------
void testStateEngine() {
    TEST_CASE("StateEngine");

    char tmpl[] = "/tmp/test_omasony_state_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp must succeed");
    std::filesystem::path sandboxPath(sandbox);

    {
        StateEngine engine(sandboxPath);
        bool ok = engine.initialize(true);
        TEST_ASSERT(ok, "StateEngine initialize must succeed");

        struct stat st{};
        int rc = ::stat(engine.getStateDirectory().c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "State directory must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0700, "State directory mode must be 0700");

        rc = ::stat(engine.getStateFilePath().c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "status.json must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0600, "status.json mode must be 0600");

        // Verify JSON contents
        std::ifstream ifs(engine.getStateFilePath());
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_ASSERT(content.find("\"schema_version\": 1") != std::string::npos, "Schema version must be 1");
        TEST_ASSERT(content.find("\"connected\": true") != std::string::npos, "Connected must be true");
        TEST_ASSERT(content.find("\"device_name\": \"ULT WEAR\"") != std::string::npos, "Device name must match");
        TEST_ASSERT(content.find("\"ult_mode\": 0") != std::string::npos, "Must contain ULT mode");
        TEST_ASSERT(content.find("\"ambient_sound_level\"") != std::string::npos, "Must contain ambient_sound_level");
        TEST_ASSERT(content.find("\"ambient_level\"") != std::string::npos, "Must contain ambient_level");
        TEST_ASSERT(content.find("\"battery_charging\"") != std::string::npos, "Must contain battery_charging");
        TEST_ASSERT(content.find("\"charging\"") != std::string::npos, "Must contain charging");

        // Test state mutation
        engine.setNoiseMode("ambient");
        engine.setAmbientLevel(14);
        engine.setEqPreset("vocal");

        std::string jsonNow = engine.getStatusJson();
        TEST_ASSERT(jsonNow.find("\"noise_mode\":\"ambient\"") != std::string::npos, "Noise mode updated");
        TEST_ASSERT(jsonNow.find("\"ambient_sound_level\":14") != std::string::npos, "Ambient level updated");
        TEST_ASSERT(jsonNow.find("\"eq_preset\":\"vocal\"") != std::string::npos, "EQ preset updated");
        TEST_ASSERT(jsonNow.find('\n') == std::string::npos, "getStatusJson must not contain newlines");

        // Test concurrent readers (50 iterations)
        std::atomic<bool> writerDone{false};
        std::atomic<int> tornReads{0};

        std::thread reader([&]() {
            while (!writerDone.load()) {
                std::ifstream rfs(engine.getStateFilePath());
                std::string s((std::istreambuf_iterator<char>(rfs)), std::istreambuf_iterator<char>());
                if (!s.empty()) {
                    if (s.front() != '{' || s.back() != '\n') {
                        tornReads++;
                    }
                }
            }
        });

        for (int i = 0; i < 50; ++i) {
            engine.setAmbientLevel(i % 21);
        }
        writerDone = true;
        reader.join();
        TEST_ASSERT_EQ(tornReads.load(), 0, "No torn reads under concurrent atomic writes");

        // Test disconnect
        engine.setConnected(false);
        std::ifstream dis_fs(engine.getStateFilePath());
        std::string disContent((std::istreambuf_iterator<char>(dis_fs)), std::istreambuf_iterator<char>());
        TEST_ASSERT(disContent.find("\"connected\": false") != std::string::npos, "Disconnected payload persisted");
        TEST_ASSERT(std::filesystem::exists(engine.getStateFilePath()), "File remains on disconnect");

        // Test cleanup on shutdown
        engine.cleanup();
        TEST_ASSERT(!std::filesystem::exists(engine.getStateFilePath()), "File unlinked on clean shutdown");
    }

    std::error_code ec;
    std::filesystem::remove_all(sandboxPath, ec);

    TEST_PASS("StateEngine");
}

// ---------------------------------------------------------------------------
// 11. IpcServer Wire Protocol & Socket Tests
// ---------------------------------------------------------------------------
void testIpcServer() {
    TEST_CASE("IpcServer");

    IpcServer server;

    // Direct command execution checks
    TEST_ASSERT_EQ(server.handleCommandLine("noise anc"), "OK\n", "noise anc valid");
    TEST_ASSERT_EQ(server.handleCommandLine("noise ambient"), "OK\n", "noise ambient valid");
    TEST_ASSERT(server.handleCommandLine("noise wind").rfind("ERR invalid mode", 0) == 0, "wind rejected: unsupported on the XM3");
    TEST_ASSERT(server.handleCommandLine("noise invalid").rfind("ERR invalid mode", 0) == 0, "invalid noise rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 16"), "OK\n", "ambient-level 16 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 0"), "OK\n", "ambient-level 0 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 20"), "OK\n", "ambient-level 20 valid");
    TEST_ASSERT(server.handleCommandLine("ambient-level 25").rfind("ERR ambient level out of range", 0) == 0, "out of range ambient level rejected");
    TEST_ASSERT(server.handleCommandLine("ambient-level abc").rfind("ERR invalid level", 0) == 0, "non-integer ambient level rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("eq vocal"), "OK\n", "eq vocal valid");
    TEST_ASSERT(server.handleCommandLine("eq invalid").rfind("ERR unknown eq preset", 0) == 0, "unknown eq preset rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("eq custom 1 2 3 4 5 6"), "OK\n", "eq custom valid");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3").rfind("ERR custom eq requires", 0) == 0, "short custom eq rejected");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3 4 15 0").rfind("ERR custom eq band out of range", 0) == 0, "out of range custom eq band rejected");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3 4 5 15").rfind("ERR clear bass out of range", 0) == 0, "out of range clear bass rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("dsee off"), "OK\n", "dsee off valid");
    TEST_ASSERT_EQ(server.handleCommandLine("dsee on"), "OK\n", "dsee on valid");
    TEST_ASSERT_EQ(server.handleCommandLine("voice-focus on"), "OK\n", "voice focus on valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ult off"), "OK\n", "ULT off valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ult 1"), "OK\n", "ULT 1 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ult ult2"), "OK\n", "ULT 2 alias valid");
    TEST_ASSERT(server.handleCommandLine("ult 3").rfind("ERR expected off|1|2", 0) == 0, "invalid ULT mode rejected");
    // XM5-only verbs must not silently look like they worked
    TEST_ASSERT(server.handleCommandLine("speak-to-chat on").rfind("ERR unknown command", 0) == 0, "speak-to-chat rejected");
    TEST_ASSERT(server.handleCommandLine("multipoint on").rfind("ERR unknown command", 0) == 0, "multipoint rejected");
    TEST_ASSERT(server.handleCommandLine("ear-detect off").rfind("ERR unknown command", 0) == 0, "ear-detect rejected");
    TEST_ASSERT(server.handleCommandLine("").rfind("ERR empty command", 0) == 0, "empty command rejected");
    TEST_ASSERT(server.handleCommandLine("unknown_verb").rfind("ERR unknown command", 0) == 0, "unknown verb rejected");

    // Socket Functional Test
    char tmpl[] = "/tmp/test_omasony_ipc_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp for socket test must succeed");
    std::string sockPath = std::string(sandbox) + "/test.sock";

    {
        IpcServer sockServer(sockPath);
        bool started = sockServer.start();
        TEST_ASSERT(started, "IpcServer start must succeed");

        struct stat st{};
        int rc = ::stat(sockPath.c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "Socket file must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0700, "Socket file mode must be 0700");

        // Connect client
        int clientFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd >= 0, "Client socket creation must succeed");

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

        rc = ::connect(clientFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect to socket must succeed");

        // Server accepts client
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 connected client");

        // Test 1: Full command
        std::string cmd1 = "noise ambient\n";
        ::send(clientFd, cmd1.data(), cmd1.size(), 0);
        sockServer.pollOnce(50);

        char buf[256]{};
        ssize_t n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 3L, "Response must be 3 bytes ('OK\\n')");
        TEST_ASSERT_EQ(std::string(buf), "OK\n", "Response string must be 'OK\\n'");

        // Test 2: Chunked partial delivery ("ambient-" then "level 10\n")
        std::string chunk1 = "ambient-";
        ::send(clientFd, chunk1.data(), chunk1.size(), 0);
        sockServer.pollOnce(10);

        std::string chunk2 = "level 10\n";
        ::send(clientFd, chunk2.data(), chunk2.size(), 0);
        sockServer.pollOnce(50);

        std::memset(buf, 0, sizeof(buf));
        n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 3L, "Response to chunked command must be 3 bytes");
        TEST_ASSERT_EQ(std::string(buf), "OK\n", "Response to chunked command must be 'OK\\n'");

        // Test 3: Pipelined multiple commands in single write
        std::string pipelined = "dsee on\ndsee off\n";
        ::send(clientFd, pipelined.data(), pipelined.size(), 0);
        sockServer.pollOnce(50);

        std::memset(buf, 0, sizeof(buf));
        n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 6L, "Response to pipelined commands must be 6 bytes ('OK\\nOK\\n')");
        TEST_ASSERT_EQ(std::string(buf), "OK\nOK\n", "Pipelined responses must be 'OK\\nOK\\n'");

        ::close(clientFd);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Client count must be 0 after disconnect");

        // Set callbacks for status queries
        IpcCallbacks cbs;
        protocol::HeadphoneState testState;
        testState.connected = true;
        testState.device_name = "ULT WEAR";
        testState.noise_mode = "anc";
        testState.battery_level = 90;
        cbs.getStatusJson = [&testState]() {
            return testState.toJson();
        };
        sockServer.setCallbacks(cbs);

        // Test 4: Compact single-line status output over socket
        int clientFd2 = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd2 >= 0, "Client 2 socket creation must succeed");
        rc = ::connect(clientFd2, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect client 2 must succeed");
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 client connected");

        std::string statusCmd = "status\n";
        ::send(clientFd2, statusCmd.data(), statusCmd.size(), 0);
        sockServer.pollOnce(50);

        char respBuf[4096]{};
        n = ::recv(clientFd2, respBuf, sizeof(respBuf) - 1, 0);
        TEST_ASSERT(n > 0, "Status response received from socket");
        std::string statusResp(respBuf, static_cast<size_t>(n));
        TEST_ASSERT(statusResp.back() == '\n', "Status response must terminate in newline");
        TEST_ASSERT_EQ(statusResp.find('\n'), statusResp.size() - 1, "Status response must have no embedded newlines before the final newline");
        TEST_ASSERT(statusResp.find("\"device_name\":\"ULT WEAR\"") != std::string::npos, "Status response contains compact device_name");
        TEST_ASSERT(statusResp.find("\"noise_mode\":\"anc\"") != std::string::npos, "Status response contains compact noise_mode");

        ::close(clientFd2);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Client count must be 0 after disconnect");

        // Test 5: Pipelined commands under abrupt client disconnect (UAF prevention check)
        int clientFd3 = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd3 >= 0, "Client 3 socket creation must succeed");
        rc = ::connect(clientFd3, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect client 3 must succeed");
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 client connected");

        std::string pipeBurst = "status\nnoise anc\nstatus\nambient-level 5\nstatus\n";
        ::send(clientFd3, pipeBurst.data(), pipeBurst.size(), 0);
        ::shutdown(clientFd3, SHUT_RD);
        ::close(clientFd3);

        // Server processes pipelined read event on abruptly closed socket: must not crash or UAF
        sockServer.pollOnce(50);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Server must cleanly handle abrupt disconnect during pipelining without crash");

        sockServer.stop();
        rc = ::stat(sockPath.c_str(), &st);
        TEST_ASSERT_EQ(rc, -1, "Socket file must be unlinked after stop()");
    }

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    TEST_PASS("IpcServer");
}

// ---------------------------------------------------------------------------
// Main Runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================\n";
    std::cout << "  omarchy-sony: Protocol & Mock Tests   \n";
    std::cout << "========================================\n";

    testChecksumCalculation();
    testEscapingAndUnescaping();
    testPacketFraming();
    testStreamFramer();
    testCommandSerializers();
    testQuerySerializers();
    testInboundStateParser();
    testStringAndEnumHelpers();
    testMockTransportAndBluetoothManager();
    testStateEngine();
    testIpcServer();

    std::cout << "========================================\n";
    std::cout << "Summary: " << (gTotalTests - gFailedTests) << "/" << gTotalTests
              << " test cases passed (" << gFailedTests << " failed)\n";
    std::cout << "========================================\n";

    return (gFailedTests == 0) ? 0 : 1;
}
