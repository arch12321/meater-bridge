#pragma once

// Freshness / availability state [item 2].
//
// Shared interface carrying, per data source, the timestamp of the last good
// reading and the derived age. Later phases use this to gate what is published
// so that stale temperatures are NEVER presented as current: an aged source is
// reported unavailable rather than emitting its last value as if live.
//
// This header is pure state + policy helpers. It does not publish anything and
// has no side effects. Time is passed in explicitly (millis()) so the same code
// is testable on the host.

#include <array>
#include <cstdint>

#include "bridge_types.h"

namespace mb {

// Data sources whose freshness is tracked independently.
enum class FreshnessSource : uint8_t {
    Bridge = 0,  // The bridge process itself (loop heartbeat).
    Base = 1,    // MEATER+/SE base link.
    Probe = 2,   // MEATER probe readings.
    WiFi = 3,    // Wi-Fi association.
    Mqtt = 4,    // MQTT broker connection.
    Count = 5,
};

// Per-source freshness record.
struct SourceFreshness {
    bool everSeen{false};
    uint32_t lastReadingMs{0};  // millis() of the last accepted reading.

    // Age in ms relative to `now`. Returns UINT32_MAX when never seen so an
    // "unknown" source is always treated as stale, never as fresh.
    uint32_t ageMs(uint32_t now) const {
        if (!everSeen) {
            return UINT32_MAX;
        }
        return now - lastReadingMs;
    }
};

// Staleness thresholds per source, in milliseconds. Defaults are conservative;
// a source older than its threshold is considered NOT fresh. Chosen so a normal
// notification cadence keeps a source fresh, but a dropped link goes stale
// within a couple of missed updates.
struct FreshnessPolicy {
    std::array<uint32_t, static_cast<size_t>(FreshnessSource::Count)> maxAgeMs{
        /*Bridge*/ 15000,
        /*Base*/ 30000,
        /*Probe*/ 30000,
        /*WiFi*/ 20000,
        /*Mqtt*/ 20000,
    };

    uint32_t threshold(FreshnessSource source) const {
        return maxAgeMs[static_cast<size_t>(source)];
    }
};

// Tracks freshness for every source and answers "is this source fresh right
// now?" against the policy. Callers mark a source on each accepted reading.
class FreshnessTracker {
public:
    void reset() {
        sources_ = {};
    }

    // Record that `source` produced a good reading at `nowMs`.
    void mark(FreshnessSource source, uint32_t nowMs) {
        SourceFreshness& s = sources_[static_cast<size_t>(source)];
        s.everSeen = true;
        s.lastReadingMs = nowMs;
    }

    const SourceFreshness& state(FreshnessSource source) const {
        return sources_[static_cast<size_t>(source)];
    }

    uint32_t ageMs(FreshnessSource source, uint32_t nowMs) const {
        return sources_[static_cast<size_t>(source)].ageMs(nowMs);
    }

    // Core policy: a source is fresh iff it has been seen and its age is within
    // the configured threshold. Never returns true for an unseen source, which
    // is what prevents stale readings being surfaced as current.
    bool isFresh(FreshnessSource source, uint32_t nowMs) const {
        return sources_[static_cast<size_t>(source)].ageMs(nowMs) <=
               policy_.threshold(source);
    }

    FreshnessPolicy& policy() { return policy_; }
    const FreshnessPolicy& policy() const { return policy_; }

private:
    std::array<SourceFreshness, static_cast<size_t>(FreshnessSource::Count)> sources_{};
    FreshnessPolicy policy_{};
};

}  // namespace mb
