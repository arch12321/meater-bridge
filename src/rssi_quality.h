#pragma once

// RSSI smoothing + connection-quality bands [item 8].
//
// Provides an allocation-free exponential-moving-average (EMA) smoother and a
// dBm -> quality-band classifier for the two RSSI links the bridge observes:
//   * ESP32 <-> MEATER+/SE base   (the NimBLE link RSSI, read via readRssi)
//   * base  <-> probe             (the MEATERPlusProbeRSSI notification)
//
// Design contract (matches the task constraints):
//   * Publish BOTH the raw reading AND the EMA-smoothed value.
//   * NEVER replace a previously-good value with an error sentinel. When a
//     fresh reading is unavailable/invalid, the tracker keeps and reports its
//     last good raw and smoothed values and flips a validity flag instead of
//     overwriting them with -128/0. Freshness (freshness.h) is what decides
//     whether those retained values are surfaced or the source is marked
//     unavailable -- this module never fabricates a "current" reading.
//
// Time is passed in explicitly so the EMA and staleness are host-testable.
//
// This header is pure state + arithmetic; it performs no I/O and has no
// dependency on Arduino, so it compiles both on the ESP32-C3 and on the host.

#include <cstdint>

namespace mb {

// Connection-quality bands derived from a (smoothed) RSSI in dBm. Ordered from
// best to worst so callers can compare/threshold if needed. `Unknown` is used
// only when no good reading has ever been observed -- it is NOT an error
// sentinel substituted for a real value.
enum class RssiBand : uint8_t {
    Unknown = 0,
    Excellent = 1,  // >= -60 dBm
    Good = 2,       // >= -70 dBm
    Fair = 3,       // >= -80 dBm
    Poor = 4,       // >= -90 dBm
    VeryPoor = 5,   // < -90 dBm
};

// Human-readable band name for HA text/attributes.
inline const char* rssiBandName(RssiBand band) {
    switch (band) {
        case RssiBand::Excellent: return "excellent";
        case RssiBand::Good: return "good";
        case RssiBand::Fair: return "fair";
        case RssiBand::Poor: return "poor";
        case RssiBand::VeryPoor: return "very poor";
        case RssiBand::Unknown:
        default: return "unknown";
    }
}

// Band thresholds in dBm. A reading is classified into the best band whose
// floor it meets or exceeds. Values below the poorFloor are VeryPoor.
struct RssiBandThresholds {
    int16_t excellentFloor{-60};
    int16_t goodFloor{-70};
    int16_t fairFloor{-80};
    int16_t poorFloor{-90};

    RssiBand classify(int16_t dbm) const {
        if (dbm >= excellentFloor) return RssiBand::Excellent;
        if (dbm >= goodFloor) return RssiBand::Good;
        if (dbm >= fairFloor) return RssiBand::Fair;
        if (dbm >= poorFloor) return RssiBand::Poor;
        return RssiBand::VeryPoor;
    }
};

// A single RSSI link tracker: keeps the last good raw reading, an EMA-smoothed
// value (fixed-point internally to avoid float drift), and the derived band.
//
// The EMA uses integer arithmetic in 1/100 dBm units. alphaNum/alphaDen give
// the smoothing factor (default 3/10 = 0.3): higher alpha tracks faster, lower
// alpha is smoother.
class RssiTracker {
public:
    // A reading is "valid" per the caller's own rule (e.g. probeRssiValid, or
    // link rssi != 0). Only valid readings advance the raw/EMA state; an
    // invalid reading leaves the retained good values untouched and only marks
    // the source not-currently-valid so downstream can consult freshness.
    void update(bool valid, int16_t dbm, uint32_t nowMs) {
        lastUpdateAttemptMs_ = nowMs;
        if (!valid) {
            currentValid_ = false;
            return;
        }
        currentValid_ = true;
        rawDbm_ = dbm;
        lastGoodMs_ = nowMs;
        const int32_t sample100 = static_cast<int32_t>(dbm) * 100;
        if (!everSeen_) {
            ema100_ = sample100;
            everSeen_ = true;
        } else {
            // ema += alpha * (sample - ema), all in 1/100 dBm fixed point.
            ema100_ += (alphaNum_ * (sample100 - ema100_)) / alphaDen_;
        }
    }

    // True once at least one good reading has been observed. Distinct from
    // "currently valid": after a dropout everSeen stays true and the retained
    // values remain available, but currentValid() is false.
    bool everSeen() const { return everSeen_; }
    bool currentValid() const { return currentValid_; }

    // Last good raw reading in dBm. Undefined until everSeen() is true.
    int16_t rawDbm() const { return rawDbm_; }

    // EMA-smoothed reading in dBm (rounded). Undefined until everSeen().
    int16_t emaDbm() const {
        // Round half away from zero.
        const int32_t v = ema100_;
        return static_cast<int16_t>((v >= 0 ? v + 50 : v - 50) / 100);
    }

    // EMA in 1/100 dBm for callers that want sub-dBm precision.
    int32_t emaCentiDbm() const { return ema100_; }

    // Band derived from the SMOOTHED value (stable classification). Returns
    // Unknown only when nothing was ever seen.
    RssiBand band(const RssiBandThresholds& t) const {
        if (!everSeen_) return RssiBand::Unknown;
        return t.classify(emaDbm());
    }

    uint32_t lastGoodMs() const { return lastGoodMs_; }

    void setAlpha(uint16_t num, uint16_t den) {
        if (den == 0 || num == 0 || num > den) return;
        alphaNum_ = num;
        alphaDen_ = den;
    }

    void reset() { *this = RssiTracker{}; }

private:
    bool everSeen_{false};
    bool currentValid_{false};
    int16_t rawDbm_{0};
    int32_t ema100_{0};          // 1/100 dBm.
    uint16_t alphaNum_{3};       // 0.3 default.
    uint16_t alphaDen_{10};
    uint32_t lastGoodMs_{0};
    uint32_t lastUpdateAttemptMs_{0};
};

}  // namespace mb
