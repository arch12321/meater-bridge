#pragma once

// MEATER Link v17.8 TemperatureHistory request/response wire format.
//
// Reconstructed in this project's own style from static analysis of the
// authenticated MEATER Android 5.1.0 APK (version code 515). The decompiled
// Wire-generated protobuf classes are used ONLY as interoperability evidence;
// no application code is copied. Every field below cites the class it was
// recovered from. Field numbers were read from each field's @WireField(tag = N)
// annotation and the annotation's adapter (ProtoAdapter#FIXED64 / #UINT32 /
// #SINT32) fixes the wire encoding exactly. Citations name the class + the
// field's @WireField annotation, NOT decompiler line numbers (the decompiler
// alphabetises fields, so line offsets are not stable evidence).
//
// This whole path is STATICALLY PROVEN from the .proto-equivalent field tables
// and needs no hardware to exercise (it rides the existing UDP transport in
// meater_link.cpp), so it is enabled by default. The physical-probe write path
// that would source real history from a probe is separate and gated OFF; see
// meater_probe_gatt.h.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protobuf_wire.h"

namespace meater_link {

// ---------------------------------------------------------------------------
// Top-level MeaterLinkMessage oneof tags carrying temperature history.
//   Evidence: com/apptionlabs/meater_app/v3protobuf/MeaterLinkMessage.java
//     @WireField(tag = 5) temperatureHistoryRequestMessage (decode() case 5)
//     @WireField(tag = 6) temperatureHistoryMessage          (decode() case 6)
// ---------------------------------------------------------------------------
constexpr uint32_t kMlFieldTemperatureHistoryRequest = 5;  // MeaterLinkMessage.java @WireField(tag=5)
constexpr uint32_t kMlFieldTemperatureHistoryResponse = 6;  // MeaterLinkMessage.java @WireField(tag=6)

// ---------------------------------------------------------------------------
// message TemperatureHistoryRequestMessage
//   Evidence: v3protobuf/TemperatureHistoryRequestMessage.java
//     required fixed64 deviceID = 1;   // @WireField(adapter=#FIXED64, label=REQUIRED, tag=1)
// ---------------------------------------------------------------------------
constexpr uint32_t kHistReqFieldDeviceId = 1;

// message TemperatureHistoryMessage
//   Evidence: v3protobuf/TemperatureHistoryMessage.java
//     required fixed64          deviceID = 1;  // @WireField(#FIXED64, REQUIRED, tag=1)
//     required TemperatureHistory history = 2; // @WireField(TemperatureHistory#ADAPTER, REQUIRED, tag=2)
//     required fixed64          cookID   = 3;  // @WireField(#FIXED64, REQUIRED, tag=3)
constexpr uint32_t kHistRespFieldDeviceId = 1;
constexpr uint32_t kHistRespFieldHistory = 2;
constexpr uint32_t kHistRespFieldCookId = 3;

// message TemperatureHistory
//   Evidence: v3protobuf/TemperatureHistory.java
//     required uint32                    startTime = 1;  // @WireField(#UINT32, REQUIRED, tag=1) epoch seconds, cook start
//     required uint32                    interval  = 2;  // @WireField(#UINT32, REQUIRED, tag=2) seconds between samples
//     repeated TemperatureRecording      values    = 3;  // @WireField(TemperatureRecording#ADAPTER, REPEATED, tag=3)
//     repeated sint32                    peaks     = 4;  // @WireField(#SINT32, REPEATED, tag=4) (1/32 C, see note)
constexpr uint32_t kHistFieldStartTime = 1;
constexpr uint32_t kHistFieldInterval = 2;
constexpr uint32_t kHistFieldValues = 3;
constexpr uint32_t kHistFieldPeaks = 4;

// message TemperatureRecording
//   Evidence: v3protobuf/TemperatureRecording.java
//     required sint32 internal = 1;  // @WireField(#SINT32, REQUIRED, tag=1)
//     required sint32 ambient  = 2;  // @WireField(#SINT32, REQUIRED, tag=2)
// Temperature units: the app normalizes CookStatus internal/ambient to signed
// 1/32 C (see docs/protocol.md "BLE temperature units"). TemperatureRecording
// reuses the same sint32 units. The 1/32 C scaling of these history samples is
// consistent with the live CookStatus fields but is NOT independently proven by
// a comment in TemperatureRecording.java itself.
constexpr uint32_t kRecFieldInternal = 1;
constexpr uint32_t kRecFieldAmbient = 2;

struct TemperatureRecording {
    int32_t internalRaw32{0};  // 1/32 C (see units note above)
    int32_t ambientRaw32{0};   // 1/32 C
};

struct TemperatureHistory {
    uint32_t startTime{0};  // epoch seconds at cook start
    uint32_t interval{0};   // seconds between successive `values`
    std::vector<TemperatureRecording> values;
    std::vector<int32_t> peaks;  // 1/32 C peak markers
};

struct TemperatureHistoryResponse {
    uint64_t deviceId{0};
    uint64_t cookId{0};
    TemperatureHistory history;
};

// Encode a TemperatureHistoryRequestMessage body (the inner message only; the
// caller wraps it at MeaterLinkMessage tag 5). Proven wire format.
inline void encodeHistoryRequest(pb::Writer& out, uint64_t deviceId) {
    out.fixed64Field(kHistReqFieldDeviceId, deviceId);
}

// Encode a TemperatureHistory sub-message. Field order matches the app's
// ProtoWriter (startTime, interval, values, peaks) though protobuf is order-
// independent on decode.
inline void encodeHistory(pb::Writer& out, const TemperatureHistory& h) {
    out.uint32Field(kHistFieldStartTime, h.startTime);
    out.uint32Field(kHistFieldInterval, h.interval);
    for (const auto& rec : h.values) {
        pb::Writer nested;
        nested.sint32Field(kRecFieldInternal, rec.internalRaw32);
        nested.sint32Field(kRecFieldAmbient, rec.ambientRaw32);
        out.messageField(kHistFieldValues, nested);
    }
    for (const int32_t peak : h.peaks) {
        out.sint32Field(kHistFieldPeaks, peak);
    }
}

// Encode a full TemperatureHistoryMessage body (inner message; caller wraps at
// MeaterLinkMessage tag 6).
inline void encodeHistoryResponse(pb::Writer& out, const TemperatureHistoryResponse& r) {
    out.fixed64Field(kHistRespFieldDeviceId, r.deviceId);
    pb::Writer history;
    encodeHistory(history, r.history);
    out.messageField(kHistRespFieldHistory, history);
    out.fixed64Field(kHistRespFieldCookId, r.cookId);
}

// Encode a response under a caller-supplied body budget. If extreme varint
// widths would exceed the UDP packet ceiling, drop the oldest recordings and
// preserve the newest cook history instead of silently dropping the datagram.
inline size_t encodeHistoryResponseBounded(pb::Writer& out,
                                           TemperatureHistoryResponse response,
                                           size_t maxBytes) {
    for (;;) {
        pb::Writer candidate;
        encodeHistoryResponse(candidate, response);
        if (candidate.bytes().size() <= maxBytes || response.history.values.empty()) {
            out = candidate;
            return response.history.values.size();
        }
        response.history.values.erase(response.history.values.begin());
    }
}

// Parse a TemperatureHistory sub-message. Returns false on malformed input.
inline bool parseHistory(const uint8_t* data, size_t length, TemperatureHistory& out) {
    pb::Reader reader(data, length);
    pb::Field field;
    out = TemperatureHistory{};
    while (reader.next(field)) {
        switch (field.number) {
            case kHistFieldStartTime:
                out.startTime = static_cast<uint32_t>(field.varintValue);
                break;
            case kHistFieldInterval:
                out.interval = static_cast<uint32_t>(field.varintValue);
                break;
            case kHistFieldValues: {
                if (field.type != pb::WireType::LengthDelimited) break;
                pb::Reader nested(field.data, field.length);
                pb::Field inner;
                TemperatureRecording rec{};
                while (nested.next(inner)) {
                    if (inner.number == kRecFieldInternal) {
                        rec.internalRaw32 = pb::decodeZigZag32(inner.varintValue);
                    } else if (inner.number == kRecFieldAmbient) {
                        rec.ambientRaw32 = pb::decodeZigZag32(inner.varintValue);
                    }
                }
                if (!nested.valid()) return false;
                out.values.push_back(rec);
                break;
            }
            case kHistFieldPeaks:
                out.peaks.push_back(pb::decodeZigZag32(field.varintValue));
                break;
            default:
                break;
        }
    }
    return reader.valid();
}

}  // namespace meater_link
