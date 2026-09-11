#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <array>

#include "runtime_config.h"

#include "bridge_types.h"
#include "device_registry.h"
#include "device_publish_state.h"
#include "availability_model.h"
#include "freshness.h"
#include "rssi_quality.h"

class HaMqtt {
public:
    HaMqtt();
    void begin(const RuntimeConfig& config);

    // Primary loop. `devices` and `freshness` are optional shared state: when
    // provided, per-device availability is driven from freshness ages [item 2/
    // 8] and additional discovered devices publish independent identities/
    // topics [item 7]. Passing only the sample preserves the legacy
    // single-device behaviour.
    void loop(const MeaterSample& sample, const CookControl& cook);
    void loop(const MeaterSample& sample, const CookControl& cook,
              mb::DeviceRegistry& devices, const mb::FreshnessTracker& freshness,
              uint32_t nowMs);

    // [item 2] Live MQTT broker connection state, so the caller can drive the
    // independent Mqtt freshness/availability source. Reports the transport
    // state directly; it does not itself gate temperatures.
    bool connected() { return enabled_ && client_.connected(); }

private:
    bool connect();
    void publishDiscovery();
    void publishState(const MeaterSample& sample, const CookControl& cook);

    // [item 7] Publish independent discovery + state + availability for the
    // additional (non-primary) devices in the registry. The primary device
    // (slot 0) keeps its legacy entity IDs/topics via the methods above; these
    // helpers only ever touch slots >= 1 so single-device deployments are
    // unaffected. Each additional device gets a compact diagnostic entity set
    // (temperatures + RSSI raw/EMA/band + freshness), scoped to its derived
    // node id, within the ESP32-C3 memory budget.
    void publishAdditionalDiscovery(const mb::DeviceIdentity& device);
    void publishAdditionalState(const mb::DeviceIdentity& device,
                                const mb::DevicePublishState& pub, uint32_t nowMs);
    void publishAdditionalAvailability(const mb::DeviceIdentity& device,
                                       bool online);
    void serviceAdditionalDevices(mb::DeviceRegistry& devices,
                                   const mb::FreshnessTracker& freshness,
                                   uint32_t nowMs);

    // Availability computed from freshness: a source that has aged past its
    // policy threshold is reported "offline" so HA marks entities unavailable
    // rather than showing a stale value as current.
    void publishAvailability(const mb::FreshnessTracker& freshness, uint32_t nowMs);

    String stateTopic() const;
    String availabilityTopic() const;
    String discoveryTopic(const char* component, const char* object) const;
    String deviceJson() const;

    // RSSI smoothing/quality state [item 8]. Retained across dropouts; never
    // overwritten with error sentinels.
    void updateRssiTrackers(const MeaterSample& sample, uint32_t nowMs);

    WiFiClient network_;
    WiFiClientSecure secureNetwork_;
    PubSubClient client_;
    const RuntimeConfig* config_{nullptr};
    String nodeId_;
    uint32_t lastConnectAttemptMs_{0};
    uint32_t lastPublishMs_{0};
    uint32_t lastSampleUpdate_{0};
    bool enabled_{false};

    mb::RssiTracker baseLinkRssi_{};   // ESP32 <-> base link RSSI.
    mb::RssiTracker probeLinkRssi_{};  // base  <-> probe RSSI.
    mb::RssiBandThresholds rssiThresholds_{};

    // [item 7/8] Per-device publish state for the ADDITIONAL devices (slots
    // >= 1). Slot 0 mirrors the primary but the primary is published via the
    // legacy methods above using baseLinkRssi_/probeLinkRssi_; this table drives
    // the extra devices' own RSSI raw/EMA/band and retained snapshots.
    mb::DevicePublishTable devicePublish_{};
    // Discovery is published once per additional device (on first admission).
    // A bitmask of registry slots whose discovery config has been emitted.
    uint32_t additionalDiscoveryDone_{0};
    // Per-additional-device last availability, so we only publish transitions.
    std::array<bool, MB_MAX_DEVICES> additionalOnline_{};
    std::array<bool, MB_MAX_DEVICES> additionalOnlineInit_{};
    bool lastAvailabilityOnline_{true};
    bool availabilityInit_{false};

    // Latest per-source freshness ages (seconds), cached from the freshness
    // tracker so publishState can emit them. UINT32_MAX-derived "never seen"
    // ages are published as -1 => HA treats them as unknown, not a live 0.
    int32_t baseAgeS_{-1};
    int32_t probeAgeS_{-1};

    // [item 2] Temperature-gating flags cached from the freshness-aware loop.
    // When a source is stale its temperatures are published as null instead of
    // the last cached value, so a stale reading is NEVER presented as current.
    // Default true so the legacy (freshness-less) loop overload is unchanged.
    bool probeTempFresh_{true};  // Gates probe tip/ambient/peak/internal_*.
    bool baseTempFresh_{true};   // Gates base_temperature.
    bool freshnessGating_{false};// True once the freshness-aware loop has run.
};
