// Host tests for [item 8] RSSI smoothing/quality bands and [item 7]
// multi-device topic derivation + selection behaviour.
//
// rssi_quality.h and device_registry.h are Arduino-free and host-compilable
// directly. Temporary build output goes to $KIROCREW_SCRATCH / $TMPDIR (see
// scripts/test_host.sh).

#include <cassert>
#include <cstring>
#include <iostream>

#include "device_registry.h"
#include "device_publish_state.h"
#include "rssi_quality.h"

static void testRssiEmaTracksAndSmooths() {
    mb::RssiTracker t;
    assert(!t.everSeen());
    assert(t.band(mb::RssiBandThresholds{}) == mb::RssiBand::Unknown);

    // First good reading seeds the EMA exactly.
    t.update(true, -55, 1000);
    assert(t.everSeen());
    assert(t.currentValid());
    assert(t.rawDbm() == -55);
    assert(t.emaDbm() == -55);  // Seeded, no lag on first sample.

    // Subsequent readings are smoothed: EMA moves toward the new sample but
    // lags it (alpha 0.3 default). raw follows the sample exactly.
    t.update(true, -75, 2000);
    assert(t.rawDbm() == -75);
    // ema = -55 + 0.3*(-75 - -55) = -55 + 0.3*-20 = -61.
    assert(t.emaDbm() == -61);
    // Smoothed value must lie strictly between the old EMA and the new raw.
    assert(t.emaDbm() > -75 && t.emaDbm() < -55);
}

static void testRssiNeverReplacedBySentinel() {
    mb::RssiTracker t;
    t.update(true, -62, 1000);
    const int16_t goodRaw = t.rawDbm();
    const int16_t goodEma = t.emaDbm();
    assert(goodRaw == -62);

    // An invalid reading (dropout) must NOT overwrite the retained good values
    // with a sentinel; it only flips currentValid() off.
    t.update(false, -128, 2000);
    assert(!t.currentValid());
    assert(t.everSeen());               // Still have history.
    assert(t.rawDbm() == goodRaw);      // Retained, not clobbered.
    assert(t.emaDbm() == goodEma);      // Retained, not clobbered.
    assert(t.band(mb::RssiBandThresholds{}) != mb::RssiBand::Unknown);

    // A later valid reading resumes smoothing from the retained EMA.
    t.update(true, -62, 3000);
    assert(t.currentValid());
    assert(t.emaDbm() == goodEma);  // -62 -> ema stays -62.
}

static void testRssiBands() {
    mb::RssiBandThresholds th;  // -60/-70/-80/-90 defaults.
    mb::RssiTracker t;
    t.update(true, -55, 0);
    assert(t.band(th) == mb::RssiBand::Excellent);
    t.reset();
    t.update(true, -65, 0);
    assert(t.band(th) == mb::RssiBand::Good);
    t.reset();
    t.update(true, -75, 0);
    assert(t.band(th) == mb::RssiBand::Fair);
    t.reset();
    t.update(true, -85, 0);
    assert(t.band(th) == mb::RssiBand::Poor);
    t.reset();
    t.update(true, -95, 0);
    assert(t.band(th) == mb::RssiBand::VeryPoor);

    assert(std::strcmp(mb::rssiBandName(mb::RssiBand::Excellent), "excellent") == 0);
    assert(std::strcmp(mb::rssiBandName(mb::RssiBand::Unknown), "unknown") == 0);
}

static void testDeviceTopicsPreserveLegacy() {
    mb::DeviceRegistry reg;
    reg.begin("meater_bridge");
    mb::DeviceIdentity* primary =
        reg.bind(UINT64_C(0xAABBCCDDEEFF), mb::DeviceRole::Probe, 2, "");
    assert(primary != nullptr);

    char topic[96];
    mb::DeviceRegistry::stateTopic(topic, sizeof(topic), "meater_bridge", *primary);
    assert(std::strcmp(topic, "meater_bridge/meater_bridge/state") == 0);  // Legacy.
    mb::DeviceRegistry::availabilityTopic(topic, sizeof(topic), "meater_bridge", *primary);
    assert(std::strcmp(topic, "meater_bridge/meater_bridge/availability") == 0);

    // A second device gets a distinct topic namespace.
    mb::DeviceIdentity* second =
        reg.bind(UINT64_C(0x112233445566), mb::DeviceRole::Base, 32, "");
    assert(second != nullptr);
    mb::DeviceRegistry::stateTopic(topic, sizeof(topic), "meater_bridge", *second);
    assert(std::strncmp(topic, "meater_bridge/meater_bridge_", 28) == 0);
    assert(std::strcmp(topic, "meater_bridge/meater_bridge/state") != 0);
}

static void testDeviceSelectionRespectsConfiguredMax() {
    // Capacity is MB_MAX_DEVICES (default 4). Fill it, then confirm a further
    // distinct device is rejected (no eviction) and re-binding an admitted one
    // still succeeds.
    mb::DeviceRegistry reg;
    reg.begin("meater_bridge");
    const size_t cap = mb::DeviceRegistry::capacity();
    for (size_t i = 0; i < cap; ++i) {
        mb::DeviceIdentity* d =
            reg.bind(UINT64_C(0x100) + i, mb::DeviceRole::Probe, 2, "");
        assert(d != nullptr);
    }
    assert(reg.full());
    assert(reg.remainingCapacity() == 0);

    // Over capacity: rejected, existing set preserved.
    mb::DeviceIdentity* overflow = reg.bind(UINT64_C(0x9999), mb::DeviceRole::Probe, 2, "");
    assert(overflow == nullptr);
    assert(reg.size() == cap);

    // Already-admitted device still binds idempotently.
    mb::DeviceIdentity* again = reg.bind(UINT64_C(0x100), mb::DeviceRole::Probe, 2, "");
    assert(again != nullptr);
    assert(reg.size() == cap);
}

static void testPerDeviceFreshnessDoesNotRefreshUnchangedSamples();

int main() {
    testRssiEmaTracksAndSmooths();
    testRssiNeverReplacedBySentinel();
    testRssiBands();
    testDeviceTopicsPreserveLegacy();
    testDeviceSelectionRespectsConfiguredMax();
    testPerDeviceFreshnessDoesNotRefreshUnchangedSamples();
    std::cout << "rssi + multidevice host tests passed\n";
    return 0;
}

static void testPerDeviceFreshnessDoesNotRefreshUnchangedSamples() {
    mb::DevicePublishTable table;
    table.begin(3, 10);
    table.attach(0, 0x10);
    table.attach(1, 0x11);
    MeaterSample sample;
    sample.valid = true;
    sample.bleConnected = true;
    sample.deviceId = 0x10;
    sample.updatedMs = 100;
    sample.rssi = -60;
    table.observe(0, sample, 1000);
    assert(table.slot(0).freshness.isFresh(mb::FreshnessSource::Probe, 1000));
    table.observe(0, sample, 200000); // Same reading must not refresh its timestamp.
    assert(!table.slot(0).freshness.isFresh(mb::FreshnessSource::Probe, 200000));
    assert(!table.slot(1).haveSample);
    sample.updatedMs = 101;
    table.observe(0, sample, 200001);
    assert(table.slot(0).freshness.isFresh(mb::FreshnessSource::Probe, 200001));
}
