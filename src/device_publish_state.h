#pragma once

// Per-device publish state [item 7 + item 8].
//
// The ESP32-C3 holds a single active BLE central link at a time, so at any
// instant only one MEATER probe/base is actively feeding a live MeaterSample.
// The DeviceRegistry (device_registry.h) however remembers every distinct
// probe/base identity seen this boot, up to MB_MAX_DEVICES. This module holds
// the *publish-side* state that must exist PER discovered device rather than
// once globally:
//
//   * Independent RSSI trackers [item 8] for the two links that device exposes
//     -- ESP32<->base link RSSI and base<->probe RSSI -- each keeping raw + EMA
//     + band, and NEVER overwritten by an error sentinel on a dropout.
//   * A per-device last-good MeaterSample snapshot, so a device that is not the
//     currently-connected one still publishes its most recent values (behind
//     its own freshness gate) instead of being blanked by the active device.
//   * A per-device FreshnessTracker feeding that device's HA availability
//     [item 2 reuse], so ageing one device out never affects another.
//
// Slot 0 mirrors the primary device: HaMqtt keeps publishing the primary from
// its existing legacy-named entities/topics, and this array's slot 0 simply
// carries the primary's RSSI/freshness so the per-device and legacy code share
// one source of truth. Slots 1..N-1 are the additional devices that get their
// own derived node id.
//
// Pure state + arithmetic: no Arduino, no NimBLE, no MQTT. Time is passed in
// explicitly so the whole module is host-testable. Fixed-capacity and
// allocation-free for the C3.

#include <array>
#include <cstdint>

#include "bridge_config.h"
#include "bridge_types.h"
#include "device_registry.h"
#include "freshness.h"
#include "rssi_quality.h"

namespace mb {

// The publish-side state for one discovered device.
struct DevicePublishState {
    bool inUse{false};
    uint64_t bleDeviceId{0};

    // Last-good snapshot for THIS device. Retained across the active link
    // moving to another device; freshness (below) decides whether its
    // temperatures are surfaced as current. Never presented as live once stale.
    MeaterSample lastSample{};
    bool haveSample{false};

    FreshnessTracker freshness{};
    uint32_t lastPublishedMs{0};
    uint32_t lastPublishedSampleMs{0};
    uint32_t lastObservedSampleMs{0};

    // [item 8] The two RSSI links for this device, each raw + EMA + band.
    RssiTracker baseLinkRssi{};   // ESP32 <-> base link RSSI (NimBLE readRssi).
    RssiTracker probeLinkRssi{};  // base  <-> probe RSSI (MLDevice.bleSignalLevel /
                                  // MEATERPlusProbeRSSI notify).

    void reset() { *this = DevicePublishState{}; }
};

// Fixed-capacity collection of per-device publish state, indexed 1:1 with the
// DeviceRegistry slots. Capacity is the same MB_MAX_DEVICES ceiling so the two
// structures never disagree on how many devices exist.
template <size_t Capacity>
class DevicePublishTableT {
public:
    static constexpr size_t capacity() { return Capacity; }

    void begin(uint16_t alphaNum, uint16_t alphaDen) {
        states_ = {};
        for (auto& s : states_) {
            s.baseLinkRssi.setAlpha(alphaNum, alphaDen);
            s.probeLinkRssi.setAlpha(alphaNum, alphaDen);
            s.freshness.policy().maxAgeMs[static_cast<size_t>(FreshnessSource::Base)] =
                MB_MULTI_DEVICE_STALE_MS;
            s.freshness.policy().maxAgeMs[static_cast<size_t>(FreshnessSource::Probe)] =
                MB_MULTI_DEVICE_STALE_MS;
        }
    }

    DevicePublishState& slot(size_t index) { return states_[index]; }
    const DevicePublishState& slot(size_t index) const { return states_[index]; }

    // Bind a registry slot index to a BLE identity so the two stay aligned.
    void attach(size_t index, uint64_t bleDeviceId) {
        if (index >= Capacity) {
            return;
        }
        DevicePublishState& s = states_[index];
        if (!s.inUse || s.bleDeviceId != bleDeviceId) {
            // New device landing in this slot: clear any prior device's state
            // so we never mix two devices' readings.
            s.reset();
            s.inUse = true;
            s.bleDeviceId = bleDeviceId;
            s.baseLinkRssi.setAlpha(MB_RSSI_EMA_ALPHA_NUM, MB_RSSI_EMA_ALPHA_DEN);
            s.probeLinkRssi.setAlpha(MB_RSSI_EMA_ALPHA_NUM, MB_RSSI_EMA_ALPHA_DEN);
            s.freshness.policy().maxAgeMs[static_cast<size_t>(FreshnessSource::Base)] =
                MB_MULTI_DEVICE_STALE_MS;
            s.freshness.policy().maxAgeMs[static_cast<size_t>(FreshnessSource::Probe)] =
                MB_MULTI_DEVICE_STALE_MS;
        }
    }

    // Record the currently-connected device's live sample against its slot,
    // updating its RSSI trackers and its last-good snapshot. `nowMs` drives the
    // EMA/freshness clocks. Only VALID readings advance state; an invalid read
    // leaves retained values untouched (the trackers enforce this).
    void observe(size_t index, const MeaterSample& sample, uint32_t nowMs) {
        if (index >= Capacity) {
            return;
        }
        DevicePublishState& s = states_[index];
        s.inUse = true;

        s.baseLinkRssi.update(sample.rssi != 0, sample.rssi, nowMs);
        s.probeLinkRssi.update(sample.probeRssiValid, sample.probeRssi, nowMs);

        const bool newReading = sample.valid && sample.updatedMs != s.lastObservedSampleMs;
        if (sample.valid) {
            s.lastSample = sample;
            s.haveSample = true;
            if (newReading && sample.bleConnected) {
                s.freshness.mark(FreshnessSource::Base, nowMs);
                if (sample.probeConnected()) {
                    s.freshness.mark(FreshnessSource::Probe, nowMs);
                }
            }
            s.lastObservedSampleMs = sample.updatedMs;
        }
    }

private:
    std::array<DevicePublishState, Capacity> states_{};
};

using DevicePublishTable = DevicePublishTableT<MB_MAX_DEVICES>;

}  // namespace mb
