// Host tests for the BLE ownership/handoff supervisor [item 1] in
// ble_supervisor.h. Pure decision logic, no NimBLE -- every path is driven by
// feeding events/candidates with explicit timestamps and asserting the returned
// SupervisorAction and resulting ConnState.

#include <cassert>
#include <cstdint>
#include <iostream>

#include "ble_supervisor.h"

using namespace mb;

static BleCandidate base(uint64_t id, int16_t rssi) {
    return BleCandidate{true, true, id, rssi};
}
static BleCandidate probe(uint64_t id, int16_t rssi) {
    return BleCandidate{true, false, id, rssi};
}

// Base preference: a base outranks a stronger-RSSI direct probe when preferBase.
static void testBasePreferenceOverProbe() {
    BleSupervisor sup;
    sup.setPreference(OwnerPreference::Base);
    uint32_t t = 1000;
    auto a = sup.poll(t);  // Idle -> StartScan
    assert(a.directive == SupervisorDirective::StartScan);
    assert(sup.state() == ConnState::Scanning);

    // Direct probe seen first with strong signal, then a weaker base.
    sup.onCandidate(probe(0xAAAA, -50), t);
    sup.onCandidate(base(0xBBBB, -80), t);

    // Next poll drives the connect and must pick the BASE despite weaker RSSI.
    a = sup.poll(t);
    assert(a.directive == SupervisorDirective::Connect);
    assert(a.target.isBase);
    assert(a.target.deviceId == 0xBBBB);
    assert(sup.state() == ConnState::Connecting);
}

// Direct-probe preference: strongest RSSI wins, base is not special.
static void testDirectProbePreference() {
    BleSupervisor sup;
    sup.setPreference(OwnerPreference::DirectProbe);
    uint32_t t = 1000;
    sup.poll(t);
    sup.onCandidate(base(0xBBBB, -80), t);
    sup.onCandidate(probe(0xAAAA, -50), t);
    auto a = sup.poll(t);
    assert(a.directive == SupervisorDirective::Connect);
    assert(a.target.deviceId == 0xAAAA);  // stronger RSSI
}

// Finite scans then bounded backoff: empty scans count and eventually back off.
static void testFiniteScansThenBackoff() {
    ConnTiming ct;
    SupervisorTiming st;
    st.maxScansBeforeBackoff = 3;
    st.scanWindowMs = 1000;
    BleSupervisor sup(ct, st);
    uint32_t t = 0;
    sup.poll(t);  // Idle -> Scanning (scan 1 running)
    assert(sup.state() == ConnState::Scanning);

    // Scan 1 times out (no candidate) -> another scan (finite not exhausted).
    t += 1000;
    auto a = sup.poll(t);
    assert(a.directive == SupervisorDirective::StartScan);
    // Scan 2 times out -> another scan.
    t += 1000;
    a = sup.poll(t);
    assert(a.directive == SupervisorDirective::StartScan);
    // Scan 3 times out -> now finite scans exhausted -> backoff.
    t += 1000;
    a = sup.poll(t);
    assert(a.directive == SupervisorDirective::EnterBackoff);
    assert(a.backoffMs > 0);
    assert(sup.state() == ConnState::Backoff);

    // Backoff expires -> a fresh scan resumes.
    t += a.backoffMs;
    a = sup.poll(t);
    assert(a.directive == SupervisorDirective::StartScan);
    assert(sup.state() == ConnState::Scanning);
}

// Half-open teardown: a disconnect must go through HalfOpen -> Teardown ->
// (driver reports) TeardownComplete -> Backoff, never straight back to connect.
static void testHalfOpenTeardown() {
    BleSupervisor sup;
    uint32_t t = 0;
    sup.poll(t);                       // Scanning
    sup.onCandidate(base(0x1234, -60), t);
    sup.poll(t);                       // Connecting
    sup.onEvent(ConnEvent::Connected, t);
    assert(sup.state() == ConnState::Connected);
    assert(sup.attempt() == 0);
    assert(sup.servingId() == 0x1234); // handoff identity remembered

    t += 20000;                        // healthy link for a while
    auto a = sup.onEvent(ConnEvent::Disconnected, t);
    assert(a.directive == SupervisorDirective::Teardown);
    assert(sup.state() == ConnState::HalfOpen);

    a = sup.onEvent(ConnEvent::TeardownComplete, t);
    assert(a.directive == SupervisorDirective::EnterBackoff);
    assert(sup.state() == ConnState::Backoff);

    // Identity survives the reconnect (same-probe-ID handoff continuity).
    assert(sup.servingId() == 0x1234);
}

// No-temperature watchdog: a connected link that produces no temperature within
// the budget is torn down (never presented as live).
static void testNoTemperatureWatchdog() {
    ConnTiming ct;
    SupervisorTiming st;
    st.noTemperatureMs = 5000;
    BleSupervisor sup(ct, st);
    uint32_t t = 0;
    sup.poll(t);
    sup.onCandidate(base(0x1, -60), t);
    sup.poll(t);
    sup.onEvent(ConnEvent::Connected, t);
    assert(sup.state() == ConnState::Connected);

    // A reading at +3s keeps it alive.
    t += 3000;
    sup.noteTemperature(t);
    auto a = sup.poll(t + 1000);
    assert(a.directive == SupervisorDirective::Wait);
    assert(sup.state() == ConnState::Connected);

    // Now go silent past the budget -> watchdog tears it down.
    t += 3000 + 5001;
    a = sup.poll(t);
    assert(a.directive == SupervisorDirective::Teardown);
    assert(sup.state() == ConnState::HalfOpen);
}

// Connect watchdog: a stuck Connecting state aborts after the budget.
static void testConnectWatchdog() {
    ConnTiming ct;
    SupervisorTiming st;
    st.connectWatchdogMs = 4000;
    BleSupervisor sup(ct, st);
    uint32_t t = 0;
    sup.poll(t);
    sup.onCandidate(base(0x1, -60), t);
    sup.poll(t);  // Connecting at t
    assert(sup.state() == ConnState::Connecting);

    // Still connecting after the budget -> watchdog -> teardown.
    t += 4001;
    auto a = sup.poll(t);
    assert(a.directive == SupervisorDirective::Teardown);
    assert(sup.state() == ConnState::HalfOpen);
}

// Anti-thrashing: repeated rapid drops from a base (Android owns the single
// central slot) push the base path into a cooldown, during which base
// candidates are ignored so we stop fighting the phone.
static void testAntiThrashBaseCooldown() {
    ConnTiming ct;
    SupervisorTiming st;
    st.rapidDropMs = 4000;
    st.rapidDropsForCooldown = 3;
    st.baseCooldownMs = 30000;
    BleSupervisor sup(ct, st);
    uint32_t t = 0;

    auto connectBaseThenRapidDrop = [&]() {
        sup.onEvent(ConnEvent::StartScan, t);
        sup.onCandidate(base(0xB0, -60), t);
        sup.poll(t);                              // Connecting
        sup.onEvent(ConnEvent::Connected, t);     // Connected
        t += 500;                                 // held < rapidDropMs
        sup.onEvent(ConnEvent::Disconnected, t);  // rapid drop -> HalfOpen
        sup.onEvent(ConnEvent::TeardownComplete, t);
        sup.onEvent(ConnEvent::BackoffExpired, t);  // back to Scanning
    };

    connectBaseThenRapidDrop();
    assert(sup.ownerPolicy().baseCooldownUntilMs == 0);  // 1 drop
    connectBaseThenRapidDrop();
    assert(sup.ownerPolicy().baseCooldownUntilMs == 0);  // 2 drops
    const uint32_t before = t;
    connectBaseThenRapidDrop();                          // 3rd -> cooldown
    assert(sup.ownerPolicy().baseCooldownUntilMs > before);

    // During cooldown, a base candidate is ignored; a direct probe still works.
    assert(sup.state() == ConnState::Scanning);
    sup.onCandidate(base(0xB0, -55), t);      // ignored (cooldown)
    sup.onCandidate(probe(0xC0DE, -70), t);
    auto a = sup.poll(t);
    assert(a.directive == SupervisorDirective::Connect);
    assert(!a.target.isBase);                 // fell back to the direct probe
}

// A healthy long-lived link resets the anti-thrash counter so one bad streak
// long ago does not permanently penalise the base.
static void testHealthyLinkResetsAntiThrash() {
    ConnTiming ct;
    SupervisorTiming st;
    st.rapidDropMs = 4000;
    st.rapidDropsForCooldown = 3;
    BleSupervisor sup(ct, st);
    uint32_t t = 0;

    // Two rapid drops.
    for (int i = 0; i < 2; ++i) {
        sup.onEvent(ConnEvent::StartScan, t);
        sup.onCandidate(base(0xB0, -60), t);
        sup.poll(t);
        sup.onEvent(ConnEvent::Connected, t);
        t += 500;
        sup.onEvent(ConnEvent::Disconnected, t);
        sup.onEvent(ConnEvent::TeardownComplete, t);
        sup.onEvent(ConnEvent::BackoffExpired, t);
    }
    assert(sup.ownerPolicy().baseRapidDrops == 2);

    // A healthy link held well past rapidDropMs resets the counter.
    sup.onEvent(ConnEvent::StartScan, t);
    sup.onCandidate(base(0xB0, -60), t);
    sup.poll(t);
    sup.onEvent(ConnEvent::Connected, t);
    t += 60000;  // long, healthy
    sup.onEvent(ConnEvent::Disconnected, t);
    assert(sup.ownerPolicy().baseRapidDrops == 0);
}

static void testFailedConnectsTriggerBaseCooldown() {
    ConnTiming ct;
    SupervisorTiming st;
    st.rapidDropsForCooldown = 3;
    st.baseCooldownMs = 30000;
    BleSupervisor sup(ct, st);
    uint32_t t = 100;
    for (int i = 0; i < 3; ++i) {
        sup.onEvent(ConnEvent::StartScan, t);
        sup.onCandidate(base(0xB0, -80), t);
        sup.poll(t);
        sup.onEvent(ConnEvent::Watchdog, t + 1);
        auto backoff = sup.onEvent(ConnEvent::TeardownComplete, t + 1);
        t += backoff.backoffMs + 2;
        sup.poll(t);
    }
    assert(sup.ownerPolicy().baseCooldownUntilMs > t - 2);
}

int main() {
    testBasePreferenceOverProbe();
    testDirectProbePreference();
    testFiniteScansThenBackoff();
    testHalfOpenTeardown();
    testNoTemperatureWatchdog();
    testConnectWatchdog();
    testAntiThrashBaseCooldown();
    testHealthyLinkResetsAntiThrash();
    testFailedConnectsTriggerBaseCooldown();
    std::cout << "ble supervisor host tests passed\n";
    return 0;
}
