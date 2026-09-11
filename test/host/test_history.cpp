#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "meater_link_history.h"

// Round-trips a TemperatureHistory sub-message through encode + parse and checks
// the recovered fields. Also verifies the request-message body and the sint32
// (zig-zag) encoding of negative temperatures, matching the proven wire format.
int main() {
    using namespace meater_link;

    // Request body: required fixed64 deviceID = tag 1.
    pb::Writer req;
    encodeHistoryRequest(req, UINT64_C(0x1122334455667788));
    // tag 1, wiretype 1 (fixed64) => key byte 0x09, then 8 LE bytes.
    const std::vector<uint8_t> expectedReq{
        0x09, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    assert(req.bytes() == expectedReq);

    // History with two recordings (one negative internal) and one peak.
    TemperatureHistory h;
    h.startTime = 1710000000u;
    h.interval = 5u;
    h.values.push_back(TemperatureRecording{32 * 20, 32 * 18});   // 20C, 18C in 1/32C
    h.values.push_back(TemperatureRecording{-32 * 3, 32 * 200});  // -3C, 200C
    h.peaks.push_back(32 * 205);

    pb::Writer hw;
    encodeHistory(hw, h);

    TemperatureHistory parsed;
    assert(parseHistory(hw.bytes().data(), hw.bytes().size(), parsed));
    assert(parsed.startTime == 1710000000u);
    assert(parsed.interval == 5u);
    assert(parsed.values.size() == 2);
    assert(parsed.values[0].internalRaw32 == 32 * 20);
    assert(parsed.values[0].ambientRaw32 == 32 * 18);
    assert(parsed.values[1].internalRaw32 == -32 * 3);   // zig-zag negative survives
    assert(parsed.values[1].ambientRaw32 == 32 * 200);
    assert(parsed.peaks.size() == 1);
    assert(parsed.peaks[0] == 32 * 205);

    // Full response body wraps history + two fixed64 ids.
    TemperatureHistoryResponse resp;
    resp.deviceId = UINT64_C(0xAABBCCDDEEFF0011);
    resp.cookId = UINT64_C(0x0011223344556677);
    resp.history = h;
    pb::Writer rw;
    encodeHistoryResponse(rw, resp);

    pb::Reader top(rw.bytes().data(), rw.bytes().size());
    pb::Field field;
    bool sawDevice = false, sawHistory = false, sawCook = false;
    while (top.next(field)) {
        if (field.number == kHistRespFieldDeviceId) {
            assert(field.type == pb::WireType::Fixed64);
            assert(field.fixed64Value == resp.deviceId);
            sawDevice = true;
        } else if (field.number == kHistRespFieldHistory) {
            assert(field.type == pb::WireType::LengthDelimited);
            TemperatureHistory inner;
            assert(parseHistory(field.data, field.length, inner));
            assert(inner.values.size() == 2);
            sawHistory = true;
        } else if (field.number == kHistRespFieldCookId) {
            assert(field.fixed64Value == resp.cookId);
            sawCook = true;
        }
    }
    assert(top.valid());
    assert(sawDevice && sawHistory && sawCook);

    TemperatureHistoryResponse worst;
    worst.deviceId = 1;
    worst.cookId = 2;
    worst.history.startTime = 1;
    worst.history.interval = 5;
    for (unsigned i = 0; i < 120; ++i) {
        worst.history.values.push_back(
            TemperatureRecording{static_cast<int32_t>(0x80000000u), 0x7fffffff});
    }
    pb::Writer bounded;
    const size_t retained = encodeHistoryResponseBounded(bounded, worst, 1400);
    assert(bounded.bytes().size() <= 1400);
    assert(retained > 0 && retained < 120);

    std::cout << "temperature-history host tests passed\n";
    return 0;
}
