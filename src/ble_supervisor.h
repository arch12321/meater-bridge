#pragma once

// BLE ownership / handoff supervisor [item 1].
//
// The interface phase left ble_conn_state.h as a pure state-machine skeleton
// (ConnState/ConnEvent/BackoffPolicy/ConnStateMachine) with no timing driver.
// This header adds the missing reliability behaviour on top of that skeleton,
// still WITHOUT touching NimBLE, so it stays host-testable:
//
//   * finite scans then bounded exponential backoff + jitter;
//   * a connect watchdog (abort a stuck connect) and a link/no-temperature
//     watchdog (abort a silent link so it cannot masquerade as connected);
//   * half-open teardown accounting (a disconnect must fully release before the
//     next attempt, so a stale half-open connection never wedges the owner);
//   * base-preference with anti-thrashing when Android initially owns the base:
//     a base that keeps dropping us (single-central contention) is deprioritised
//     for a cooldown so we neither fight the phone nor thrash the radio;
//   * same-probe-ID handoff continuity: the identity we were serving is
//     remembered across a reconnect so downstream keeps one logical device.
//
// The supervisor is a decision engine: feed it observations (millis(), events,
// candidate sightings) and it returns a SupervisorAction telling the on-target
// driver what to do next (start a scan, connect a candidate, arm a teardown,
// wait). meater_ble.cpp performs the NimBLE side effects; all policy lives here
// and is exercised by host tests.

#include <cstdint>

#include "ble_conn_state.h"

namespace mb {

// A candidate seen during a scan, distilled to what the owner policy needs.
struct BleCandidate {
    bool valid{false};
    bool isBase{false};       // MEATER+/SE charger/repeater (preferred path).
    uint64_t deviceId{0};     // Stable identity (probe id, or base id).
    int16_t rssi{-127};
};

// What the driver should do this tick. Exactly one directive plus optional data.
enum class SupervisorDirective : uint8_t {
    Wait = 0,           // Nothing to do yet (in a scan, mid-backoff, connected).
    StartScan = 1,      // Begin a bounded scan pass.
    Connect = 2,        // Connect `target` (returned alongside).
    Teardown = 3,       // Force-release the current/half-open link.
    EnterBackoff = 4,   // Begin waiting `backoffMs` before the next attempt.
};

struct SupervisorAction {
    SupervisorDirective directive{SupervisorDirective::Wait};
    BleCandidate target{};   // Valid when directive == Connect.
    uint32_t backoffMs{0};   // Valid when directive == EnterBackoff.

    // Explicit factories: some embedded toolchains (older GCC on the ESP32)
    // reject a nested empty-brace aggregate init like {Directive, {}, 0}, so
    // build actions through these helpers instead of brace-init lists.
    static SupervisorAction wait() { return SupervisorAction{}; }
    static SupervisorAction scan() {
        SupervisorAction a;
        a.directive = SupervisorDirective::StartScan;
        return a;
    }
    static SupervisorAction connect(const BleCandidate& c) {
        SupervisorAction a;
        a.directive = SupervisorDirective::Connect;
        a.target = c;
        return a;
    }
    static SupervisorAction teardown() {
        SupervisorAction a;
        a.directive = SupervisorDirective::Teardown;
        return a;
    }
    static SupervisorAction enterBackoff(uint32_t ms) {
        SupervisorAction a;
        a.directive = SupervisorDirective::EnterBackoff;
        a.backoffMs = ms;
        return a;
    }
};

// Base-preference + anti-thrash bookkeeping. When Android owns the base, our
// connects to it fail or drop almost immediately; counting those rapid drops
// lets us back off the base path for a cooldown instead of thrashing.
struct OwnerPolicyState {
    uint32_t baseRapidDrops{0};       // Consecutive fast drops from a base.
    uint32_t baseCooldownUntilMs{0};  // Skip base candidates until this time.
};

// Timing knobs specific to the supervisor, layered on ConnTiming.
struct SupervisorTiming {
    // A connection that drops sooner than this after connecting counts as a
    // "rapid drop" -- the fingerprint of losing a single-central race.
    uint32_t rapidDropMs{4000};
    // After this many consecutive rapid drops from a base, deprioritise the
    // base path for baseCooldownMs so Android can keep it without a fight.
    uint32_t rapidDropsForCooldown{3};
    uint32_t baseCooldownMs{30000};
    // No-temperature watchdog: a connected link that produces no temperature
    // within this budget is torn down (never present a silent link as live).
    uint32_t noTemperatureMs{45000};
    // How long we let a scan keep running before we call it a timeout.
    uint32_t scanWindowMs{6000};
    // Max scans that find nothing before we back off (finite scans).
    uint32_t maxScansBeforeBackoff{3};
    // Connect attempt budget before we abort and tear down.
    uint32_t connectWatchdogMs{10000};
};

// The supervisor. Owns the ConnStateMachine skeleton and the reliability timing
// the skeleton deliberately left out. Pure decision logic -- no NimBLE, no
// millis() of its own; the caller passes `now` every tick.
class BleSupervisor {
public:
    BleSupervisor()
        : backoff_(timing_) {}

    explicit BleSupervisor(const ConnTiming& connTiming, const SupervisorTiming& supTiming)
        : timing_(connTiming), sup_(supTiming), backoff_(timing_) {}

    ConnState state() const { return sm_.state(); }
    uint32_t attempt() const { return sm_.attemptCount(); }
    const OwnerPolicyState& ownerPolicy() const { return owner_; }

    void setPreference(OwnerPreference pref) { sm_.setPreference(pref); }
    OwnerPreference preference() const { return sm_.preference(); }

    // Identity we are (or were last) serving. Kept across reconnects so the
    // downstream logical device is stable through a handoff.
    uint64_t servingId() const { return servingId_; }

    void reset(uint32_t now) {
        sm_.reset();
        owner_ = OwnerPolicyState{};
        servingId_ = 0;
        connectedAtMs_ = 0;
        lastTemperatureMs_ = 0;
        scanStartedMs_ = 0;
        connectStartedMs_ = 0;
        backoffUntilMs_ = 0;
        haveCandidate_ = false;
        candidate_ = BleCandidate{};
        finiteScans_ = 0;
        (void)now;
    }

    // Record a candidate seen during scanning. Applies base-preference and the
    // anti-thrash cooldown: a base under cooldown is ignored; otherwise a base
    // outranks a direct probe when preferBase is set. The best candidate so far
    // is retained until we act on it.
    void onCandidate(const BleCandidate& c, uint32_t now) {
        if (!c.valid || state() != ConnState::Scanning) {
            return;
        }
        if (c.isBase && now < owner_.baseCooldownUntilMs) {
            // Base is in anti-thrash cooldown; ignore it this pass so we do not
            // keep fighting Android for the single-central slot.
            return;
        }
        if (!haveCandidate_) {
            candidate_ = c;
            haveCandidate_ = true;
            return;
        }
        // Prefer a base over a direct probe when configured; otherwise the
        // stronger RSSI wins so we pick the most reliable link.
        const bool preferBase = preference() == OwnerPreference::Base;
        if (preferBase && c.isBase && !candidate_.isBase) {
            candidate_ = c;
        } else if (preferBase && candidate_.isBase && !c.isBase) {
            // keep existing base
        } else if (c.rssi > candidate_.rssi) {
            candidate_ = c;
        }
    }

    // Commit the exact candidate selected by an external full-window scheduler.
    void selectCandidate(const BleCandidate& candidate) {
        if (state() != ConnState::Scanning || !candidate.valid) return;
        candidate_ = candidate;
        haveCandidate_ = true;
    }

    // Feed a lifecycle event with a timestamp. Returns the next action for the
    // driver. This is the heart of item 1: it enforces watchdogs, half-open
    // teardown, finite scans, backoff, and the anti-thrash cooldown.
    SupervisorAction onEvent(ConnEvent event, uint32_t now) {
        switch (event) {
            case ConnEvent::StartScan:
                sm_.next(ConnEvent::StartScan);
                scanStartedMs_ = now;
                finiteScans_ = 0;  // Fresh scan cycle.
                haveCandidate_ = false;
                candidate_ = BleCandidate{};
                return SupervisorAction::scan();

            case ConnEvent::DeviceFound:
                if (state() == ConnState::Scanning && haveCandidate_) {
                    sm_.next(ConnEvent::DeviceFound);
                    connectStartedMs_ = now;
                    return SupervisorAction::connect(candidate_);
                }
                return SupervisorAction::wait();

            case ConnEvent::ConnectRequested:
                // Explicit connect request (e.g. a configured MAC): treated like
                // DeviceFound -- advance to Connecting if we have a candidate.
                if (state() == ConnState::Scanning && haveCandidate_) {
                    sm_.next(ConnEvent::ConnectRequested);
                    connectStartedMs_ = now;
                    return SupervisorAction::connect(candidate_);
                }
                return SupervisorAction::wait();

            case ConnEvent::Connected:
                if (state() == ConnState::Connecting) {
                    sm_.next(ConnEvent::Connected);
                    connectedAtMs_ = now;
                    lastTemperatureMs_ = now;  // grace: expect a reading soon
                    finiteScans_ = 0;
                    if (haveCandidate_) {
                        servingId_ = candidate_.deviceId;
                        servingIsBase_ = candidate_.isBase;
                    }
                }
                return SupervisorAction::wait();

            case ConnEvent::Disconnected:
                return handleDrop(now, /*fromWatchdog=*/false);

            case ConnEvent::Watchdog:
                return handleDrop(now, /*fromWatchdog=*/true);

            case ConnEvent::TeardownComplete: {
                sm_.next(ConnEvent::TeardownComplete);  // -> Backoff
                const uint32_t delay = backoff_.delayMs(sm_.attemptCount(), servingId_ + now);
                backoffUntilMs_ = now + delay;
                return SupervisorAction::enterBackoff(delay);
            }

            case ConnEvent::ScanTimeout:
                // Count this empty scan pass in the supervisor's own counter so
                // the skeleton SM's scanCount (which its StartScan handler
                // zeroes) does not fight us. Finite scans: retry until the
                // budget is spent, then back off.
                ++finiteScans_;
                if (finiteScans_ >= sup_.maxScansBeforeBackoff) {
                    sm_.next(ConnEvent::ScanTimeout);  // -> Backoff
                    const uint32_t delay =
                        backoff_.delayMs(finiteScans_, servingId_ + now);
                    backoffUntilMs_ = now + delay;
                    return SupervisorAction::enterBackoff(delay);
                }
                // Budget not spent: keep scanning (SM stays in Scanning).
                scanStartedMs_ = now;
                haveCandidate_ = false;
                candidate_ = BleCandidate{};
                return SupervisorAction::scan();

            case ConnEvent::BackoffExpired:
                sm_.next(ConnEvent::BackoffExpired);  // -> Idle
                return startAnotherScan(now);
        }
        return SupervisorAction::wait();
    }

    // Record that a temperature reading arrived (feeds the no-temperature
    // watchdog). Only meaningful while connected.
    void noteTemperature(uint32_t now) { lastTemperatureMs_ = now; }

    // Poll the time-based watchdogs. Returns an action if a watchdog fired.
    // Call every tick with the current millis().
    SupervisorAction poll(uint32_t now) {
        switch (state()) {
            case ConnState::Scanning:
                if (haveCandidate_) {
                    // A candidate is waiting: drive the connect.
                    return onEvent(ConnEvent::DeviceFound, now);
                }
                if (elapsed(scanStartedMs_, now) >= sup_.scanWindowMs) {
                    return onEvent(ConnEvent::ScanTimeout, now);
                }
                return SupervisorAction::wait();

            case ConnState::Connecting:
                if (elapsed(connectStartedMs_, now) >= sup_.connectWatchdogMs) {
                    // Stuck connect: fire the watchdog -> half-open teardown.
                    return onEvent(ConnEvent::Watchdog, now);
                }
                return SupervisorAction::wait();

            case ConnState::Connected:
                if (elapsed(lastTemperatureMs_, now) >= sup_.noTemperatureMs) {
                    // Silent link: never present it as live. Tear it down.
                    return onEvent(ConnEvent::Watchdog, now);
                }
                return SupervisorAction::wait();

            case ConnState::Backoff:
                if (now >= backoffUntilMs_) {
                    return onEvent(ConnEvent::BackoffExpired, now);
                }
                return SupervisorAction::wait();

            case ConnState::HalfOpen:
                // Waiting for the driver to report TeardownComplete. If it never
                // does, treat the teardown as complete after the connect budget
                // so a stuck half-open cannot wedge the owner forever.
                if (elapsed(halfOpenSinceMs_, now) >= sup_.connectWatchdogMs) {
                    return onEvent(ConnEvent::TeardownComplete, now);
                }
                return SupervisorAction::wait();

            case ConnState::Idle:
                // Kick off the first scan.
                return onEvent(ConnEvent::StartScan, now);
        }
        return SupervisorAction::wait();
    }

private:
    static uint32_t elapsed(uint32_t since, uint32_t now) {
        // Unsigned subtraction; callers set `since` on entering the relevant
        // state before polling, so there is no "unset" sentinel to special-case
        // (a t==0 start is legitimate and must measure real elapsed time).
        return now - since;
    }

    SupervisorAction startAnotherScan(uint32_t now) {
        // The skeleton SM routes a ScanTimeout to Backoff. For a finite-scan
        // retry (scans not yet exhausted) we want to resume Scanning without a
        // backoff wait, so walk the SM back through Backoff->Idle->Scanning.
        if (sm_.state() == ConnState::Backoff) {
            sm_.next(ConnEvent::BackoffExpired);  // -> Idle
        }
        if (sm_.state() == ConnState::Idle) {
            sm_.next(ConnEvent::StartScan);       // -> Scanning
        }
        scanStartedMs_ = now;
        haveCandidate_ = false;
        candidate_ = BleCandidate{};
        finiteScans_ = 0;
        return SupervisorAction::scan();
    }

    // Common drop path for both a real disconnect and a watchdog abort. Applies
    // anti-thrash accounting (rapid drops from a base -> cooldown), then enters
    // the half-open teardown state; the driver reports TeardownComplete when the
    // NimBLE client is fully released.
    SupervisorAction handleDrop(uint32_t now, bool fromWatchdog) {
        const bool wasConnected = state() == ConnState::Connected;
        const uint32_t heldMs = wasConnected ? elapsed(connectedAtMs_, now) : 0;

        // Anti-thrash: a base link that drops fast (or a base connect that had
        // to be watchdog-aborted) is the signature of Android holding the single
        // central slot. Count it and, past the threshold, cool the base path.
        const bool baseInvolved = servingIsBase_ || (haveCandidate_ && candidate_.isBase);
        const bool rapid = fromWatchdog || (wasConnected && heldMs < sup_.rapidDropMs);
        if (baseInvolved && rapid) {
            if (++owner_.baseRapidDrops >= sup_.rapidDropsForCooldown) {
                owner_.baseCooldownUntilMs = now + sup_.baseCooldownMs;
                owner_.baseRapidDrops = 0;
            }
        } else if (wasConnected && heldMs >= sup_.rapidDropMs) {
            // A healthy long-lived link resets the anti-thrash counter.
            owner_.baseRapidDrops = 0;
        }

        sm_.next(fromWatchdog ? ConnEvent::Watchdog : ConnEvent::Disconnected);  // -> HalfOpen
        halfOpenSinceMs_ = now;
        return SupervisorAction::teardown();
    }

    ConnTiming timing_{};
    SupervisorTiming sup_{};
    ConnStateMachine sm_{};
    BackoffPolicy backoff_;
    OwnerPolicyState owner_{};

    BleCandidate candidate_{};
    bool haveCandidate_{false};
    uint64_t servingId_{0};
    bool servingIsBase_{false};

    uint32_t connectedAtMs_{0};
    uint32_t lastTemperatureMs_{0};
    uint32_t scanStartedMs_{0};
    uint32_t connectStartedMs_{0};
    uint32_t halfOpenSinceMs_{0};
    uint32_t backoffUntilMs_{0};
    uint32_t finiteScans_{0};  // Empty scan passes since the last connect/cycle.
};

}  // namespace mb
