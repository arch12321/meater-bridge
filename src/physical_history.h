#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mb {

struct PhysicalHistory {
    std::array<int32_t, 120> internalRaw32{};
    std::array<int32_t, 120> ambientRaw32{};
    uint16_t count{0};
    uint16_t intervalSeconds{0};
    uint32_t elapsedSeconds{0};
};

inline int16_t historyLe16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8U));
}

inline bool parsePhysicalHistory(const uint8_t* data, size_t length, bool g2,
                                 PhysicalHistory& out) {
    out = PhysicalHistory{};
    if (data == nullptr || (length != 484 && length != 499 && length != 512)) return false;
    size_t offset = length == 512 ? 4 : 0;
    if (offset + 4 > length) return false;
    const int16_t interval = historyLe16(data + offset);
    const int16_t count = historyLe16(data + offset + 2);
    if (interval <= 0 || interval % 5 != 0 || count < 0 || count > 120) return false;
    offset += length == 512 ? 8 : 4;
    if (offset + static_cast<size_t>(count) * 4 > length) return false;
    out.intervalSeconds = static_cast<uint16_t>(interval);
    out.count = static_cast<uint16_t>(count);
    for (uint16_t i = 0; i < out.count; ++i) {
        int32_t internal = historyLe16(data + offset + i * 4);
        int32_t ambient = historyLe16(data + offset + i * 4 + 2);
        if (!g2) { internal *= 2; ambient *= 2; }
        out.internalRaw32[i] = internal;
        out.ambientRaw32[i] = ambient;
    }
    out.elapsedSeconds = static_cast<uint32_t>(out.count) * out.intervalSeconds;
    if (length == 499) {
        constexpr size_t p = 485;
        const uint32_t reported = static_cast<uint32_t>(data[p]) |
            (static_cast<uint32_t>(data[p + 1]) << 8U) |
            (static_cast<uint32_t>(data[p + 2]) << 16U) |
            (static_cast<uint32_t>(data[p + 3]) << 24U);
        if (reported > out.elapsedSeconds && reported < 7U * 24U * 3600U) {
            out.elapsedSeconds = reported;
        }
    }
    return true;
}

}  // namespace mb
