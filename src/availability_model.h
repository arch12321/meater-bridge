#pragma once

// Availability & staleness gating [item 2].
//
// Builds on freshness.h. FreshnessTracker records, per source, the timestamp of
// the last good reading and derives its age. This header turns that raw state
// into the availability model the rest of the bridge publishes:
//
//   * Independent availability for bridge / base / probe / Wi-Fi / MQTT.
//   * A per-source last-reading timestamp and derived age (ms and s).
//   * A single hard rule -- temperatureGated(): a temperature reading is
//     surfaced as "current" ONLY when its source is fresh. When the source is
//     stale the temperature is reported unavailable (null), NEVER the last
//     value presented as if live.
//
// Pure state + policy. No I/O, no NimBLE, no MQTT. Time is passed in explicitly
// (millis()) so the whole thing is host-testable. The ESP32 driver and the MQTT
// layer consume this; they do not re-implement the staleness rule.

#include <array>
#include <cstdint>

#include "freshness.h"

namespace mb {

// Availability of a single source, resolved against the freshness policy at a
// given `now`. `Unknown` is distinct from `Offline`: Unknown means the source
// has never produced a reading this boot (so we must not claim it is online),
// Offline means it was seen but has since gone stale.
enum class Availability : uint8_t {
    Unknown = 0,   // Never seen -> treat as not-current, never as fresh.
    Online = 1,    // Seen and within its freshness threshold.
    Offline = 2,   // Seen once, but now older than its threshold (stale).
};

inline const char* availabilityName(Availability a) {
    switch (a) {
        case Availability::Online:
            return "online";
        case Availability::Offline:
            return "offline";
        case Availability::Unknown:
        default:
            return "unknown";
    }
}

// A source is considered "available" for presenting data only when Online.
inline bool isAvailable(Availability a) { return a == Availability::Online; }

// Snapshot of one source's availability at a point in time.
struct SourceStatus {
    Availability availability{Availability::Unknown};
    bool everSeen{false};
    uint32_t lastReadingMs{0};  // millis() of last accepted reading (0 if never).
    uint32_t ageMs{UINT32_MAX}; // Age at the evaluation `now`.

    uint32_t ageSeconds() const {
        return ageMs == UINT32_MAX ? UINT32_MAX : ageMs / 1000U;
    }
};

// Resolves FreshnessTracker state into per-source availability and answers the
// temperature-gating question. Holds no state of its own beyond a reference to
// the tracker; every query takes `now` so it is deterministic and testable.
class AvailabilityModel {
public:
    explicit AvailabilityModel(const FreshnessTracker& tracker) : tracker_(tracker) {}

    Availability availability(FreshnessSource source, uint32_t now) const {
        const SourceFreshness& s = tracker_.state(source);
        if (!s.everSeen) {
            return Availability::Unknown;
        }
        return tracker_.isFresh(source, now) ? Availability::Online
                                             : Availability::Offline;
    }

    SourceStatus status(FreshnessSource source, uint32_t now) const {
        const SourceFreshness& s = tracker_.state(source);
        SourceStatus out;
        out.everSeen = s.everSeen;
        out.lastReadingMs = s.lastReadingMs;
        out.ageMs = s.ageMs(now);
        out.availability = availability(source, now);
        return out;
    }

    // THE core rule for item 2. A temperature value sourced from `source` may be
    // presented as current ONLY when that source is Online right now. Callers
    // publish the value when this returns true, and publish null / unavailable
    // otherwise -- they must NOT fall back to the last cached temperature.
    bool temperatureGated(FreshnessSource source, uint32_t now) const {
        return availability(source, now) == Availability::Online;
    }

    const FreshnessTracker& tracker() const { return tracker_; }

private:
    const FreshnessTracker& tracker_;
};

}  // namespace mb
