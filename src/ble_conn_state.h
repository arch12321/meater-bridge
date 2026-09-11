#pragma once

// BLE ownership / handoff + connection state machine skeleton [item 1].
//
// This is a SKELETON: it defines the state set, the transition surface, and the
// timing/backoff parameters that later phases will drive. It deliberately does
// NOT contain the full connection logic yet -- there is no NimBLE call here and
// no scanning. The intent is to give later phases a stable interface (states,
// events, backoff-with-jitter, watchdog, half-open teardown, base preference)
// to build against while keeping the current MeaterBle flow untouched.
//
// Ownership/handoff: exactly one owner may hold the active BLE connection at a
// time. `preferBase` biases the owner toward a MEATER+/SE base repeater over a
// direct probe, matching the existing MB_PREFER_BASE behaviour.

#include <cstdint>

namespace mb {

// Connection lifecycle. Half-open is the teardown-in-progress state entered
// when a disconnect is detected but resources have not yet been released.
enum class ConnState : uint8_t {
    Idle = 0,        // Not connected, not scanning.
    Scanning = 1,    // Bounded scan in progress.
    Connecting = 2,  // Connect attempt in flight.
    Connected = 3,   // Link up, characteristics being used.
    HalfOpen = 4,    // Disconnect seen; tearing down before returning to Idle.
    Backoff = 5,     // Waiting out a backoff interval before the next attempt.
};

// Events that drive transitions. Later phases feed these from NimBLE callbacks
// and timers; the skeleton only records them and computes the next state.
enum class ConnEvent : uint8_t {
    StartScan = 0,
    DeviceFound = 1,
    ConnectRequested = 2,
    Connected = 3,
    Disconnected = 4,
    TeardownComplete = 5,
    ScanTimeout = 6,
    Watchdog = 7,       // Watchdog fired (no progress within budget).
    BackoffExpired = 8,
};

// Preferred owner when both a base repeater and a direct probe are candidates.
enum class OwnerPreference : uint8_t {
    DirectProbe = 0,
    Base = 1,  // Default, mirrors MB_PREFER_BASE.
};

// Tunable timing for scans, watchdogs, and backoff-with-jitter. Values are
// placeholders that later phases may override; they are not yet enforced by any
// running loop.
struct ConnTiming {
    uint32_t scanWindowMs{6000};        // One bounded scan pass.
    uint32_t maxScansBeforeBackoff{3};  // Finite scans, then back off.
    uint32_t connectWatchdogMs{10000};  // Abort a stuck connect.
    uint32_t linkWatchdogMs{45000};     // Abort a silent link.
    uint32_t backoffBaseMs{2000};       // First backoff interval.
    uint32_t backoffMaxMs{60000};       // Ceiling.
    uint32_t jitterMs{750};             // +/- jitter added to each backoff.
};

// Backoff-with-jitter calculator. Deterministic given (attempt, jitterSeed) so
// it is host-testable. Doubles the base per attempt, clamps at the ceiling, and
// applies a bounded pseudo-random jitter.
class BackoffPolicy {
public:
    explicit BackoffPolicy(const ConnTiming& timing) : timing_(timing) {}

    uint32_t delayMs(uint32_t attempt, uint32_t jitterSeed) const {
        uint64_t base = timing_.backoffBaseMs;
        for (uint32_t i = 0; i < attempt && base < timing_.backoffMaxMs; ++i) {
            base <<= 1;
        }
        if (base > timing_.backoffMaxMs) {
            base = timing_.backoffMaxMs;
        }
        if (timing_.jitterMs == 0) {
            return static_cast<uint32_t>(base);
        }
        // Bounded symmetric jitter in [-jitterMs, +jitterMs].
        const uint32_t span = timing_.jitterMs * 2U + 1U;
        const int32_t jitter =
            static_cast<int32_t>(mix(attempt, jitterSeed) % span) -
            static_cast<int32_t>(timing_.jitterMs);
        int64_t result = static_cast<int64_t>(base) + jitter;
        if (result < 0) {
            result = 0;
        }
        return static_cast<uint32_t>(result);
    }

private:
    static uint32_t mix(uint32_t a, uint32_t b) {
        uint32_t h = a * 2654435761U ^ (b + 0x9E3779B9U + (a << 6) + (a >> 2));
        h ^= h >> 15;
        h *= 0x85EBCA6BU;
        h ^= h >> 13;
        return h;
    }

    ConnTiming timing_;
};

// The state machine skeleton. next() is a pure transition function so it can be
// unit-tested on the host; it does not perform any BLE I/O. Later phases wire
// the returned ConnState to actual scan/connect/teardown side effects.
class ConnStateMachine {
public:
    ConnStateMachine() = default;

    ConnState state() const { return state_; }
    uint32_t scanCount() const { return scanCount_; }
    uint32_t attemptCount() const { return attemptCount_; }

    void reset() {
        state_ = ConnState::Idle;
        scanCount_ = 0;
        attemptCount_ = 0;
    }

    void setPreference(OwnerPreference preference) { preference_ = preference; }
    OwnerPreference preference() const { return preference_; }

    // Pure transition: given the current state and an event, compute the next
    // state and update bounded counters. No I/O, no full logic -- this is the
    // skeleton later phases attach behaviour to.
    ConnState next(ConnEvent event) {
        switch (state_) {
            case ConnState::Idle:
                if (event == ConnEvent::StartScan) {
                    scanCount_ = 0;
                    state_ = ConnState::Scanning;
                }
                break;
            case ConnState::Scanning:
                if (event == ConnEvent::DeviceFound ||
                    event == ConnEvent::ConnectRequested) {
                    ++attemptCount_;
                    state_ = ConnState::Connecting;
                } else if (event == ConnEvent::ScanTimeout) {
                    ++scanCount_;
                    state_ = ConnState::Backoff;  // Finite scans -> backoff.
                }
                break;
            case ConnState::Connecting:
                if (event == ConnEvent::Connected) {
                    state_ = ConnState::Connected;
                    attemptCount_ = 0;
                } else if (event == ConnEvent::Disconnected ||
                           event == ConnEvent::Watchdog) {
                    state_ = ConnState::HalfOpen;
                }
                break;
            case ConnState::Connected:
                if (event == ConnEvent::Disconnected ||
                    event == ConnEvent::Watchdog) {
                    state_ = ConnState::HalfOpen;
                }
                break;
            case ConnState::HalfOpen:
                if (event == ConnEvent::TeardownComplete) {
                    state_ = ConnState::Backoff;
                }
                break;
            case ConnState::Backoff:
                if (event == ConnEvent::BackoffExpired) {
                    state_ = ConnState::Idle;
                }
                break;
        }
        return state_;
    }

private:
    ConnState state_{ConnState::Idle};
    OwnerPreference preference_{OwnerPreference::Base};
    uint32_t scanCount_{0};
    uint32_t attemptCount_{0};
};

}  // namespace mb
