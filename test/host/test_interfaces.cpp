// Host tests for the shared interfaces introduced for later phases:
//   * device_registry.h  -- multi-device model [item 7]
//   * freshness.h        -- freshness/availability state [item 2]
//   * ble_conn_state.h   -- BLE connection state machine skeleton [item 1]
//
// These headers use only <cstdint>/<array> and are host-compilable directly.

#include <cassert>
#include <cstring>
#include <iostream>

#include "ble_conn_state.h"
#include "device_registry.h"
#include "freshness.h"

static void testDeviceRegistryPreservesPrimary() {
    mb::DeviceRegistry reg;
    reg.begin("meater_bridge");
    assert(reg.size() == 1);
    assert(reg.primary().primary);
    assert(std::strcmp(reg.primary().nodeId, "meater_bridge") == 0);

    // First bound device reuses the primary (legacy) node id -> BC unique_ids.
    mb::DeviceIdentity* first =
        reg.bind(UINT64_C(0xAABBCCDDEEFF), mb::DeviceRole::Probe, 2, "AA:BB:CC:DD:EE:FF");
    assert(first != nullptr);
    assert(std::strcmp(first->nodeId, "meater_bridge") == 0);
    assert(reg.size() == 1);

    char uid[64];
    mb::DeviceRegistry::uniqueId(uid, sizeof(uid), *first, "tip");
    assert(std::strcmp(uid, "meater_bridge_tip") == 0);  // Legacy scheme intact.

    // A second, different device gets a distinct deterministic node id.
    mb::DeviceIdentity* second =
        reg.bind(UINT64_C(0x112233445566), mb::DeviceRole::Base, 32, "11:22:33:44:55:66");
    assert(second != nullptr);
    assert(reg.size() == 2);
    assert(std::strcmp(second->nodeId, "meater_bridge") != 0);
    assert(std::strncmp(second->nodeId, "meater_bridge_", 14) == 0);

    // Re-binding the same identity is idempotent (no new slot).
    mb::DeviceIdentity* again = reg.bind(UINT64_C(0x112233445566), mb::DeviceRole::Base, 32, "");
    assert(again == second);
    assert(reg.size() == 2);
}

static void testFreshnessNeverPresentsStaleAsCurrent() {
    mb::FreshnessTracker tracker;
    // Unseen source is stale, never fresh.
    assert(!tracker.isFresh(mb::FreshnessSource::Probe, 1000));
    assert(tracker.ageMs(mb::FreshnessSource::Probe, 1000) == UINT32_MAX);

    tracker.mark(mb::FreshnessSource::Probe, 1000);
    assert(tracker.isFresh(mb::FreshnessSource::Probe, 1000));
    assert(tracker.ageMs(mb::FreshnessSource::Probe, 6000) == 5000);

    // Probe default threshold is 30s; at +25s fresh, at +31s stale.
    assert(tracker.isFresh(mb::FreshnessSource::Probe, 26000));
    assert(!tracker.isFresh(mb::FreshnessSource::Probe, 31001));

    // Independent per-source tracking.
    tracker.mark(mb::FreshnessSource::Mqtt, 5000);
    assert(tracker.isFresh(mb::FreshnessSource::Mqtt, 6000));
    assert(!tracker.isFresh(mb::FreshnessSource::Probe, 40000));
}

static void testConnStateMachineSkeleton() {
    mb::ConnStateMachine sm;
    assert(sm.state() == mb::ConnState::Idle);
    assert(sm.preference() == mb::OwnerPreference::Base);  // Mirrors MB_PREFER_BASE.

    assert(sm.next(mb::ConnEvent::StartScan) == mb::ConnState::Scanning);
    assert(sm.next(mb::ConnEvent::DeviceFound) == mb::ConnState::Connecting);
    assert(sm.next(mb::ConnEvent::Connected) == mb::ConnState::Connected);

    // Half-open teardown path on disconnect.
    assert(sm.next(mb::ConnEvent::Disconnected) == mb::ConnState::HalfOpen);
    assert(sm.next(mb::ConnEvent::TeardownComplete) == mb::ConnState::Backoff);
    assert(sm.next(mb::ConnEvent::BackoffExpired) == mb::ConnState::Idle);

    // Finite scans: a scan timeout counts a scan and backs off.
    sm.reset();
    sm.next(mb::ConnEvent::StartScan);
    assert(sm.next(mb::ConnEvent::ScanTimeout) == mb::ConnState::Backoff);
    assert(sm.scanCount() == 1);
}

static void testBackoffWithJitter() {
    mb::ConnTiming timing;
    mb::BackoffPolicy backoff(timing);

    // Monotonic non-decreasing base growth, clamped at ceiling (allow jitter).
    const uint32_t d0 = backoff.delayMs(0, 1);
    const uint32_t d3 = backoff.delayMs(3, 1);
    assert(d3 + timing.jitterMs >= d0);

    // Clamp: a large attempt never exceeds ceiling + jitter.
    const uint32_t big = backoff.delayMs(30, 42);
    assert(big <= timing.backoffMaxMs + timing.jitterMs);

    // Determinism: same inputs -> same output.
    assert(backoff.delayMs(5, 7) == backoff.delayMs(5, 7));
}

int main() {
    testDeviceRegistryPreservesPrimary();
    testFreshnessNeverPresentsStaleAsCurrent();
    testConnStateMachineSkeleton();
    testBackoffWithJitter();
    std::cout << "interface host tests passed\n";
    return 0;
}
