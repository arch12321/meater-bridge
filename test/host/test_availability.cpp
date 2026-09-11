// Host tests for the availability / staleness-gating layer [item 2] in
// availability.h, layered on freshness.h. Verifies independent per-source
// availability, last-reading timestamps + ages, and the hard rule that a stale
// temperature source is NEVER gated "current".

#include <cassert>
#include <cstdint>
#include <iostream>

#include "availability_model.h"
#include "freshness.h"

using namespace mb;

// Each source is Unknown until seen, Online while fresh, Offline once stale,
// and they are tracked independently of one another.
static void testIndependentAvailability() {
    FreshnessTracker tracker;
    AvailabilityModel model(tracker);

    // Nothing seen yet: every source Unknown, and Unknown is NOT available.
    assert(model.availability(FreshnessSource::Bridge, 0) == Availability::Unknown);
    assert(model.availability(FreshnessSource::Probe, 0) == Availability::Unknown);
    assert(!isAvailable(model.availability(FreshnessSource::Probe, 0)));

    // Bridge + Wi-Fi seen at t=1000; Probe never seen.
    tracker.mark(FreshnessSource::Bridge, 1000);
    tracker.mark(FreshnessSource::WiFi, 1000);
    assert(model.availability(FreshnessSource::Bridge, 1000) == Availability::Online);
    assert(model.availability(FreshnessSource::WiFi, 1000) == Availability::Online);
    assert(model.availability(FreshnessSource::Probe, 1000) == Availability::Unknown);

    // Wi-Fi default threshold is 20s: still Online at +20s, Offline at +21s.
    assert(model.availability(FreshnessSource::WiFi, 21000) == Availability::Online);
    assert(model.availability(FreshnessSource::WiFi, 21001) == Availability::Offline);
    // Bridge threshold is 15s and independent: Offline at +16s.
    assert(model.availability(FreshnessSource::Bridge, 17000) == Availability::Offline);
}

// Last-reading timestamp and derived age (ms + seconds) are reported per source.
static void testLastReadingAndAge() {
    FreshnessTracker tracker;
    AvailabilityModel model(tracker);

    tracker.mark(FreshnessSource::Probe, 5000);
    SourceStatus s = model.status(FreshnessSource::Probe, 12000);
    assert(s.everSeen);
    assert(s.lastReadingMs == 5000);
    assert(s.ageMs == 7000);
    assert(s.ageSeconds() == 7);
    // 7s < probe 30s threshold -> Online.
    assert(s.availability == Availability::Online);

    // Never-seen source: age is UINT32_MAX and seconds saturate too.
    SourceStatus unseen = model.status(FreshnessSource::Mqtt, 12000);
    assert(!unseen.everSeen);
    assert(unseen.ageMs == UINT32_MAX);
    assert(unseen.ageSeconds() == UINT32_MAX);
    assert(unseen.availability == Availability::Unknown);
}

// THE item-2 rule: a temperature is gated "current" only while its source is
// Online. A stale source gates false so callers publish null, never the last
// value. An unseen source also gates false.
static void testTemperatureGatingNeverStale() {
    FreshnessTracker tracker;
    AvailabilityModel model(tracker);

    // Unseen probe: not gated (must not present anything as current).
    assert(!model.temperatureGated(FreshnessSource::Probe, 1000));

    // Fresh reading: gated true.
    tracker.mark(FreshnessSource::Probe, 1000);
    assert(model.temperatureGated(FreshnessSource::Probe, 1000));
    assert(model.temperatureGated(FreshnessSource::Probe, 20000));  // within 30s

    // Past threshold: gated false -> the last temperature must NOT be surfaced.
    assert(!model.temperatureGated(FreshnessSource::Probe, 31001));

    // A fresh reading restores gating.
    tracker.mark(FreshnessSource::Probe, 40000);
    assert(model.temperatureGated(FreshnessSource::Probe, 40000));
}

// The base and probe are independent: the base link can be Online (relaying)
// while the probe itself has gone stale, and the probe temperature must then be
// gated false even though the base is up.
static void testBaseOnlineProbeStale() {
    FreshnessTracker tracker;
    AvailabilityModel model(tracker);

    tracker.mark(FreshnessSource::Base, 0);
    tracker.mark(FreshnessSource::Probe, 0);
    assert(model.temperatureGated(FreshnessSource::Probe, 0));

    // Base keeps refreshing (relay alive) but the probe stops reporting.
    tracker.mark(FreshnessSource::Base, 25000);
    assert(model.availability(FreshnessSource::Base, 25000) == Availability::Online);
    // Probe last seen at 0, threshold 30s -> stale at 31s.
    assert(!model.temperatureGated(FreshnessSource::Probe, 31000));
    assert(model.availability(FreshnessSource::Base, 31000) == Availability::Online);
}

int main() {
    testIndependentAvailability();
    testLastReadingAndAge();
    testTemperatureGatingNeverStale();
    testBaseOnlineProbeStale();
    std::cout << "availability host tests passed\n";
    return 0;
}
