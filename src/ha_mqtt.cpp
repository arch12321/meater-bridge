#include "ha_mqtt.h"

#include <WiFi.h>

#include <algorithm>

#include "bridge_config.h"
#include "runtime_config_validation.h"

namespace {

const char* childStateName(int state) {
    switch (state) {
        case 0: return "connected";
        case 1: return "disconnected";
        case 2: return "not paired";
        case 3: return "fetching cook setup";
        case 4: return "clearing history";
        case 7: return "probe battery shutdown";
        case 8: return "probe in charger";
        case 9: return "probe upside down";
        case 10: return "sleeping: no probe";
        case 11: return "sleeping: no active cook";
        default: return "unknown";
    }
}

const char* cookStateName(uint32_t state) {
    static const char* names[] = {"not started", "configured", "cooking", "ready for resting",
                                  "resting", "slightly underdone", "finished",
                                  "slightly overdone", "overcooked"};
    return state < (sizeof(names) / sizeof(names[0])) ? names[state] : "unknown";
}

String jsonEscape(const char* input) {
    String output;
    if (input == nullptr) return output;
    while (*input != '\0') {
        const char ch = *input++;
        if (ch == '\\' || ch == '"') output += '\\';
        if (static_cast<uint8_t>(ch) >= 0x20) output += ch;
    }
    return output;
}

}  // namespace

HaMqtt::HaMqtt() : client_(network_) {}

void HaMqtt::begin(const RuntimeConfig& config) {
    config_ = &config;
    nodeId_ = config.nodeId().isEmpty() ? String(MB_NODE_ID) : config.nodeId();
    baseLinkRssi_.setAlpha(MB_RSSI_EMA_ALPHA_NUM, MB_RSSI_EMA_ALPHA_DEN);
    probeLinkRssi_.setAlpha(MB_RSSI_EMA_ALPHA_NUM, MB_RSSI_EMA_ALPHA_DEN);
    devicePublish_.begin(MB_RSSI_EMA_ALPHA_NUM, MB_RSSI_EMA_ALPHA_DEN);
#if MB_MQTT_ENABLED
    enabled_ = !config.mqttHost().isEmpty();
    if (!enabled_) {
        log_i("MQTT disabled (runtime host is empty)");
        return;
    }
    if (config.mqttTls()) {
        const mb::TextView ca(config.mqttCaCert().c_str(), config.mqttCaCert().length());
        if (!mb::looksLikePemCertificate(ca)) {
            log_e("MQTT TLS requested without a valid PEM CA certificate; MQTT disabled");
            enabled_ = false;
            return;
        }
        secureNetwork_.setCACert(config.mqttCaCert().c_str());
        client_.setClient(secureNetwork_);
    } else {
        client_.setClient(network_);
        log_w("MQTT plaintext compatibility mode enabled");
    }
    client_.setServer(config.mqttHost().c_str(), config.mqttPort());
    client_.setBufferSize(3072);
    client_.setKeepAlive(30);
#endif
}

String HaMqtt::stateTopic() const {
    return String(MB_MQTT_STATE_PREFIX) + "/" + nodeId_ + "/state";
}

String HaMqtt::availabilityTopic() const {
    return String(MB_MQTT_STATE_PREFIX) + "/" + nodeId_ + "/availability";
}

String HaMqtt::discoveryTopic(const char* component, const char* object) const {
    return String(MB_MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + nodeId_ + "/" +
           object + "/config";
}

String HaMqtt::deviceJson() const {
    return String("\"dev\":{\"ids\":[\"") + nodeId_ + "\"],\"name\":\"" +
           MB_FRIENDLY_NAME + "\",\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 MEATER Bridge\","
           "\"sw\":\"" + MB_FIRMWARE_VERSION + "\"}";
}

bool HaMqtt::connect() {
    if (!enabled_ || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    const String clientId = nodeId_ + "-" +
                            String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
    const String availability = availabilityTopic();
    bool connected = false;
    if (config_ != nullptr && !config_->mqttUser().isEmpty()) {
        connected = client_.connect(clientId.c_str(), config_->mqttUser().c_str(),
                                    config_->mqttPassword().c_str(), availability.c_str(), 0,
                                    true, "offline");
    } else {
        connected = client_.connect(clientId.c_str(), availability.c_str(), 0, true,
                                    "offline");
    }

    if (!connected) {
        Serial.printf("MQTT connection failed, state=%d\n", client_.state());
        return false;
    }

    client_.publish(availability.c_str(), "online", true);
    publishDiscovery();
    additionalDiscoveryDone_ = 0;
    additionalOnlineInit_.fill(false);
    Serial.println("MQTT connected; Home Assistant discovery published");
    return true;
}

void HaMqtt::publishDiscovery() {
    const String state = stateTopic();
    const String availability = availabilityTopic();
    const String device = deviceJson();

    auto publishSensor = [&](const char* object, const char* name, const char* key,
                             const char* unit, const char* deviceClass,
                             const char* entityCategory = nullptr) {
        String payload = String("{\"name\":\"") + name + "\",\"uniq_id\":\"" +
                         nodeId_ + "_" + object + "\",\"stat_t\":\"" + state +
                         "\",\"val_tpl\":\"{{ value_json." + key +
                         " }}\",\"avty_t\":\"" + availability + "\"," + device;
        if (unit != nullptr && unit[0] != '\0') {
            payload += String(",\"unit_of_meas\":\"") + unit + "\"";
        }
        if (deviceClass != nullptr && deviceClass[0] != '\0') {
            payload += String(",\"dev_cla\":\"") + deviceClass + "\"";
        }
        if (entityCategory != nullptr) {
            payload += String(",\"ent_cat\":\"") + entityCategory + "\"";
        } else {
            payload += ",\"stat_cla\":\"measurement\"";
        }
        payload += "}";
        const String topic = discoveryTopic("sensor", object);
        client_.publish(topic.c_str(), payload.c_str(), true);
    };

    auto publishText = [&](const char* object, const char* name, const char* key,
                           const char* entityCategory = "diagnostic") {
        String payload = String("{\"name\":\"") + name + "\",\"uniq_id\":\"" +
                         nodeId_ + "_" + object + "\",\"stat_t\":\"" + state +
                         "\",\"val_tpl\":\"{{ value_json." + key +
                         " }}\",\"avty_t\":\"" + availability + "\"," + device;
        if (entityCategory != nullptr) {
            payload += String(",\"ent_cat\":\"") + entityCategory + "\"";
        }
        payload += "}";
        const String topic = discoveryTopic("sensor", object);
        client_.publish(topic.c_str(), payload.c_str(), true);
    };

    auto publishBinary = [&](const char* object, const char* name, const char* key) {
        String payload = String("{\"name\":\"") + name + "\",\"uniq_id\":\"" +
                         nodeId_ + "_" + object + "\",\"stat_t\":\"" + state +
                         "\",\"val_tpl\":\"{{ value_json." + key +
                         " }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                         "\"avty_t\":\"" + availability +
                         "\",\"ent_cat\":\"diagnostic\"," + device + "}";
        const String topic = discoveryTopic("binary_sensor", object);
        client_.publish(topic.c_str(), payload.c_str(), true);
    };

    publishSensor("tip", "MEATER Tip", "tip", "°C", "temperature");
    publishSensor("ambient", "MEATER Ambient", "ambient", "°C", "temperature");
    publishSensor("peak", "MEATER Peak", "peak", "°C", "temperature");
    publishSensor("target", "MEATER Target", "target", "°C", "temperature");
    publishSensor("cook_elapsed", "MEATER Cook Elapsed", "cook_elapsed", "s", "duration");

    publishSensor("battery", "MEATER Probe Battery", "battery", "%", "battery", "diagnostic");
    publishSensor("base_battery", "MEATER Base Battery", "base_battery", "%", "battery",
                  "diagnostic");
    publishSensor("rssi", "MEATER Base RSSI", "rssi", "dBm", "signal_strength", "diagnostic");
    publishSensor("probe_rssi", "MEATER Probe RSSI", "probe_rssi", "dBm",
                  "signal_strength", "diagnostic");
    // [item 8] EMA-smoothed RSSI alongside the raw values, plus quality bands.
    publishSensor("rssi_ema", "MEATER Base RSSI (smoothed)", "rssi_ema", "dBm",
                  "signal_strength", "diagnostic");
    publishSensor("probe_rssi_ema", "MEATER Probe RSSI (smoothed)", "probe_rssi_ema",
                  "dBm", "signal_strength", "diagnostic");
    publishText("rssi_band", "MEATER Base Signal Quality", "rssi_band");
    publishText("probe_rssi_band", "MEATER Probe Signal Quality", "probe_rssi_band");
    publishSensor("base_temperature", "MEATER Base Temperature", "base_temperature", "°C",
                  "temperature", "diagnostic");

    for (unsigned i = 1; i <= 5; ++i) {
        const String object = String("internal_") + i;
        const String name = String("MEATER Internal Sensor ") + i;
        publishSensor(object.c_str(), name.c_str(), object.c_str(), "°C", "temperature",
                      "diagnostic");
    }

    publishSensor("wifi_rssi", "Bridge Wi-Fi RSSI", "wifi_rssi", "dBm", "signal_strength",
                  "diagnostic");
    publishSensor("wifi_channel", "Bridge Wi-Fi Channel", "wifi_channel", "", "",
                  "diagnostic");
    publishSensor("uptime", "Bridge Uptime", "uptime", "s", "duration", "diagnostic");
    // [item 2/8] Freshness ages feed HA availability; also expose them so a
    // stale source is visible even before availability flips.
    publishSensor("base_age", "MEATER Base Data Age", "base_age", "s", "duration",
                  "diagnostic");
    publishSensor("probe_age", "MEATER Probe Data Age", "probe_age", "s", "duration",
                  "diagnostic");
    publishSensor("free_heap", "Bridge Free Heap", "free_heap", "B", "data_size",
                  "diagnostic");
    publishSensor("probe_number", "MEATER Probe Type", "probe_number", "", "", "diagnostic");
    publishSensor("base_type", "MEATER Base Type", "base_type", "", "", "diagnostic");
    publishSensor("child_state_code", "MEATER Child State Code", "child_state_code", "", "",
                  "diagnostic");

    publishBinary("connected", "MEATER Base BLE Connected", "connected");
    publishBinary("probe_connected", "MEATER Probe Connected", "probe_connected");
    publishBinary("mqtt_connected", "Bridge MQTT Connected", "mqtt_connected");
    // [item 2] Independent freshness/availability of the temperature sources.
    // problem device_class so HA shows ON == a problem (source stale/absent).
    publishBinary("probe_available", "MEATER Probe Data Available", "probe_available");
    publishBinary("base_available", "MEATER Base Data Available", "base_available");
    publishBinary("stale", "MEATER Reading Stale", "stale");

    publishText("probe_state", "MEATER Probe State", "probe_state");
    publishText("cook_state", "MEATER Cook State", "cook_state", nullptr);
    publishText("cook_name", "MEATER Cook Name", "cook_name", nullptr);
    publishText("probe_firmware", "MEATER Probe Firmware", "firmware");
    publishText("base_firmware", "MEATER Base Firmware", "base_firmware");
    publishText("probe_id", "MEATER Probe ID", "device_id");
    publishText("base_id", "MEATER Base ID", "base_id");
    publishText("base_address", "MEATER Base BLE Address", "base_address");
    publishText("ip", "Bridge IP Address", "ip");
    publishText("bridge_firmware", "Bridge Firmware", "bridge_firmware");
}

void HaMqtt::publishState(const MeaterSample& sample, const CookControl& cook) {
    String payload;
    payload.reserve(1800);
    payload = "{";
    bool first = true;

    auto key = [&](const char* name) {
        if (!first) payload += ',';
        first = false;
        payload += '"';
        payload += name;
        payload += "\":";
    };
    auto addInt = [&](const char* name, int64_t value) {
        key(name);
        payload += String(static_cast<long long>(value));
    };
    auto addNullableInt = [&](const char* name, bool valid, int64_t value) {
        key(name);
        payload += valid ? String(static_cast<long long>(value)) : String("null");
    };
    auto addTemp = [&](const char* name, int32_t raw32) {
        key(name);
        payload += raw32 == MB_INVALID_TEMP_RAW32 ? String("null")
                                                   : String(raw32 / 32.0f, 2);
    };
    // [item 2] Gated temperature: publish null when the source is stale so a
    // stale reading is NEVER surfaced as current. `fresh` comes from the
    // freshness-aware loop; the legacy loop leaves gating flags true.
    auto addTempGated = [&](const char* name, int32_t raw32, bool fresh) {
        key(name);
        if (!fresh || raw32 == MB_INVALID_TEMP_RAW32) {
            payload += String("null");
        } else {
            payload += String(raw32 / 32.0f, 2);
        }
    };
    auto addString = [&](const char* name, const char* value) {
        key(name);
        payload += '"';
        payload += jsonEscape(value);
        payload += '"';
    };

    addTempGated("tip", sample.internalRaw32, probeTempFresh_);
    addTempGated("ambient", sample.ambientRaw32, probeTempFresh_);
    addTempGated("peak", sample.peakRaw32, probeTempFresh_);
    addTemp("target", cook.targetInternalRaw32 == 0 ? MB_INVALID_TEMP_RAW32
                                                     : cook.targetInternalRaw32);
    addTempGated("base_temperature", sample.baseTemperatureRaw32, baseTempFresh_);
    for (unsigned i = 0; i < 5; ++i) {
        const String name = String("internal_") + (i + 1);
        addTempGated(name.c_str(),
                     i < sample.internalCount ? sample.internalsRaw32[i]
                                              : MB_INVALID_TEMP_RAW32,
                     probeTempFresh_);
    }

    addInt("battery", sample.batteryPercent);
    addNullableInt("base_battery", sample.baseBatteryValid, sample.baseBatteryPercent);
    addNullableInt("rssi", sample.rssi != 0, sample.rssi);
    addNullableInt("probe_rssi", sample.probeRssiValid, sample.probeRssi);
    // [item 8] EMA-smoothed RSSI + quality bands. These read from the retained
    // trackers: after a dropout everSeen() stays true so the last good smoothed
    // value is still reported (never replaced by a sentinel); only a
    // never-seen link emits null / "unknown".
    addNullableInt("rssi_ema", baseLinkRssi_.everSeen(), baseLinkRssi_.emaDbm());
    addNullableInt("probe_rssi_ema", probeLinkRssi_.everSeen(),
                   probeLinkRssi_.emaDbm());
    addString("rssi_band", mb::rssiBandName(baseLinkRssi_.band(rssiThresholds_)));
    addString("probe_rssi_band",
              mb::rssiBandName(probeLinkRssi_.band(rssiThresholds_)));
    addNullableInt("child_state_code", sample.childState >= 0, sample.childState);
    addInt("probe_number", sample.probeNumber);
    addInt("base_type", sample.baseType);
    addInt("internal_count", sample.internalCount);
    addInt("tip_raw32", sample.internalRaw32);
    addInt("ambient_raw32", sample.ambientRaw32);
    addInt("cook_sequence", cook.sequenceNumber);
    addInt("cook_target_raw32", cook.targetInternalRaw32);
    const uint32_t localElapsed = cook.startedMs == 0 ? 0 : (millis() - cook.startedMs) / 1000U;
    const uint32_t historyElapsed = sample.historyElapsedSeconds == 0 ? 0 :
        sample.historyElapsedSeconds + (millis() - sample.historyCapturedMs) / 1000U;
    addInt("cook_elapsed", std::max(localElapsed, historyElapsed));

    addString("connected", sample.bleConnected ? "ON" : "OFF");
    addString("probe_connected", sample.probeConnected() ? "ON" : "OFF");
    addString("mqtt_connected", client_.connected() ? "ON" : "OFF");
    addString("probe_state", childStateName(sample.childState));
    addString("cook_state", cookStateName(cook.state));
    addString("cook_name", cook.name);
    addString("firmware", sample.firmware);
    addString("base_firmware", sample.baseFirmware);
    addString("base_address", sample.baseAddress);
    addString("ip", WiFi.localIP().toString().c_str());
    addString("bridge_firmware", MB_FIRMWARE_VERSION);

    char identifier[17];
    snprintf(identifier, sizeof(identifier), "%016llx",
             static_cast<unsigned long long>(sample.deviceId));
    addString("device_id", identifier);
    snprintf(identifier, sizeof(identifier), "%016llx",
             static_cast<unsigned long long>(sample.baseDeviceId));
    addString("base_id", identifier);
    snprintf(identifier, sizeof(identifier), "%016llx",
             static_cast<unsigned long long>(cook.cookId));
    addString("cook_id", identifier);

    addInt("wifi_rssi", WiFi.RSSI());
    addInt("wifi_channel", WiFi.channel());
    addInt("uptime", millis() / 1000U);
    addInt("free_heap", ESP.getFreeHeap());
    // [item 2/8] Freshness ages (seconds); -1 => never seen (HA "unknown").
    addNullableInt("base_age", baseAgeS_ >= 0, baseAgeS_);
    addNullableInt("probe_age", probeAgeS_ >= 0, probeAgeS_);
    // [item 2] Explicit availability of the temperature sources. "stale" is ON
    // whenever a temperature source is not fresh, so HA can flag/alert on it
    // even though the temperature fields themselves are already null-gated.
    addString("probe_available", probeTempFresh_ ? "ON" : "OFF");
    addString("base_available", baseTempFresh_ ? "ON" : "OFF");
    addString("stale", (!probeTempFresh_ || !baseTempFresh_) ? "ON" : "OFF");

    payload += '}';
    const String topic = stateTopic();
    client_.publish(topic.c_str(), payload.c_str(), true);
}

void HaMqtt::updateRssiTrackers(const MeaterSample& sample, uint32_t nowMs) {
    // Base link RSSI is valid when non-zero (0 == "not read yet" from NimBLE).
    baseLinkRssi_.update(sample.rssi != 0, sample.rssi, nowMs);
    // Probe RSSI validity is decided upstream (probeRssiValid); -128 sentinel
    // is treated as invalid there so it never enters the EMA.
    probeLinkRssi_.update(sample.probeRssiValid, sample.probeRssi, nowMs);
}

void HaMqtt::publishAvailability(const mb::FreshnessTracker& freshness, uint32_t nowMs) {
    const bool fresh = freshness.isFresh(mb::FreshnessSource::Base, nowMs) &&
                       WiFi.status() == WL_CONNECTED;
    if (availabilityInit_ && fresh == lastAvailabilityOnline_) return;
    availabilityInit_ = true;
    lastAvailabilityOnline_ = fresh;
    const String availability = availabilityTopic();
    client_.publish(availability.c_str(), fresh ? "online" : "offline", true);
}

void HaMqtt::publishAdditionalDiscovery(const mb::DeviceIdentity& device) {
    char state[96];
    char availability[96];
    mb::DeviceRegistry::stateTopic(state, sizeof(state), MB_MQTT_STATE_PREFIX, device);
    mb::DeviceRegistry::availabilityTopic(availability, sizeof(availability),
                                          MB_MQTT_STATE_PREFIX, device);
    const String deviceJson = String("\"dev\":{\"ids\":[\"") + device.nodeId +
        "\"],\"name\":\"MEATER " + device.nodeId +
        "\",\"mf\":\"Apption Labs\",\"mdl\":\"MEATER via ESP32-C3\",\"via_device\":\"" +
        nodeId_ + "\"}";

    auto sensor = [&](const char* object, const char* name, const char* key,
                      const char* unit, const char* deviceClass, bool diagnostic) {
        String payload = String("{\"name\":\"") + name + "\",\"uniq_id\":\"" +
            device.nodeId + "_" + object + "\",\"stat_t\":\"" + state +
            "\",\"val_tpl\":\"{{ value_json." + key + " }}\",\"avty_t\":\"" +
            availability + "\"," + deviceJson;
        if (unit != nullptr && unit[0] != '\0') payload += String(",\"unit_of_meas\":\"") + unit + "\"";
        if (deviceClass != nullptr && deviceClass[0] != '\0') payload += String(",\"dev_cla\":\"") + deviceClass + "\"";
        if (diagnostic) payload += ",\"ent_cat\":\"diagnostic\"";
        else payload += ",\"stat_cla\":\"measurement\"";
        payload += "}";
        const String topic = String(MB_MQTT_DISCOVERY_PREFIX) + "/sensor/" +
            device.nodeId + "/" + object + "/config";
        client_.publish(topic.c_str(), payload.c_str(), true);
    };
    auto text = [&](const char* object, const char* name, const char* key) {
        sensor(object, name, key, "", "", true);
    };

    sensor("tip", "MEATER Tip", "tip", "°C", "temperature", false);
    sensor("ambient", "MEATER Ambient", "ambient", "°C", "temperature", false);
    sensor("peak", "MEATER Peak", "peak", "°C", "temperature", false);
    sensor("battery", "MEATER Probe Battery", "battery", "%", "battery", true);
    sensor("base_battery", "MEATER Base Battery", "base_battery", "%", "battery", true);
    sensor("rssi", "MEATER Base RSSI", "rssi", "dBm", "signal_strength", true);
    sensor("rssi_ema", "MEATER Base RSSI (smoothed)", "rssi_ema", "dBm", "signal_strength", true);
    sensor("probe_rssi", "MEATER Probe RSSI", "probe_rssi", "dBm", "signal_strength", true);
    sensor("probe_rssi_ema", "MEATER Probe RSSI (smoothed)", "probe_rssi_ema", "dBm", "signal_strength", true);
    sensor("probe_age", "MEATER Probe Data Age", "probe_age", "s", "duration", true);
    sensor("base_age", "MEATER Base Data Age", "base_age", "s", "duration", true);
    text("rssi_band", "MEATER Base Signal Quality", "rssi_band");
    text("probe_rssi_band", "MEATER Probe Signal Quality", "probe_rssi_band");
    text("probe_state", "MEATER Probe State", "probe_state");
    text("probe_firmware", "MEATER Probe Firmware", "firmware");
    text("probe_id", "MEATER Probe ID", "device_id");
    text("base_id", "MEATER Base ID", "base_id");
}

void HaMqtt::publishAdditionalAvailability(const mb::DeviceIdentity& device, bool online) {
    char topic[96];
    mb::DeviceRegistry::availabilityTopic(topic, sizeof(topic), MB_MQTT_STATE_PREFIX, device);
    client_.publish(topic, online ? "online" : "offline", true);
}

void HaMqtt::publishAdditionalState(const mb::DeviceIdentity& device,
                                    const mb::DevicePublishState& pub, uint32_t nowMs) {
    const MeaterSample& sample = pub.lastSample;
    const mb::AvailabilityModel availability(pub.freshness);
    const bool probeFresh = availability.temperatureGated(mb::FreshnessSource::Probe, nowMs);
    String payload;
    payload.reserve(900);
    payload = "{";
    bool first = true;
    auto key = [&](const char* name) { if (!first) payload += ','; first = false; payload += '"'; payload += name; payload += "\":"; };
    auto integer = [&](const char* name, int64_t value) { key(name); payload += String(static_cast<long long>(value)); };
    auto nullable = [&](const char* name, bool valid, int64_t value) { key(name); payload += valid ? String(static_cast<long long>(value)) : String("null"); };
    auto temperature = [&](const char* name, int32_t raw32, bool fresh) { key(name); payload += (!fresh || raw32 == MB_INVALID_TEMP_RAW32) ? String("null") : String(raw32 / 32.0f, 2); };
    auto text = [&](const char* name, const char* value) { key(name); payload += '"'; payload += jsonEscape(value); payload += '"'; };

    temperature("tip", sample.internalRaw32, probeFresh);
    temperature("ambient", sample.ambientRaw32, probeFresh);
    temperature("peak", sample.peakRaw32, probeFresh);
    integer("battery", sample.batteryPercent);
    nullable("base_battery", sample.baseBatteryValid, sample.baseBatteryPercent);
    nullable("rssi", pub.baseLinkRssi.everSeen(), pub.baseLinkRssi.rawDbm());
    nullable("rssi_ema", pub.baseLinkRssi.everSeen(), pub.baseLinkRssi.emaDbm());
    nullable("probe_rssi", pub.probeLinkRssi.everSeen(), pub.probeLinkRssi.rawDbm());
    nullable("probe_rssi_ema", pub.probeLinkRssi.everSeen(), pub.probeLinkRssi.emaDbm());
    text("rssi_band", mb::rssiBandName(pub.baseLinkRssi.band(rssiThresholds_)));
    text("probe_rssi_band", mb::rssiBandName(pub.probeLinkRssi.band(rssiThresholds_)));
    text("probe_state", childStateName(sample.childState));
    text("firmware", sample.firmware);
    nullable("base_age",
             pub.freshness.state(mb::FreshnessSource::Base).everSeen,
             pub.freshness.ageMs(mb::FreshnessSource::Base, nowMs) / 1000U);
    nullable("probe_age",
             pub.freshness.state(mb::FreshnessSource::Probe).everSeen,
             pub.freshness.ageMs(mb::FreshnessSource::Probe, nowMs) / 1000U);
    char identifier[17];
    snprintf(identifier, sizeof(identifier), "%016llx",
             static_cast<unsigned long long>(sample.deviceId));
    text("device_id", identifier);
    snprintf(identifier, sizeof(identifier), "%016llx",
             static_cast<unsigned long long>(sample.baseDeviceId));
    text("base_id", identifier);
    payload += "}";
    char topic[96];
    mb::DeviceRegistry::stateTopic(topic, sizeof(topic), MB_MQTT_STATE_PREFIX, device);
    client_.publish(topic, payload.c_str(), true);
}

void HaMqtt::serviceAdditionalDevices(mb::DeviceRegistry& devices,
                                      const mb::FreshnessTracker&, uint32_t nowMs) {
    for (size_t i = 1; i < devices.size() && i < MB_MAX_DEVICES; ++i) {
        const mb::DeviceIdentity* device = devices.at(i);
        mb::DevicePublishState& pub = devicePublish_.slot(i);
        if (device == nullptr || !device->inUse || !pub.inUse || !pub.haveSample) continue;
        const uint32_t bit = 1UL << i;
        if ((additionalDiscoveryDone_ & bit) == 0) {
            publishAdditionalDiscovery(*device);
            additionalDiscoveryDone_ |= bit;
        }
        const bool online = pub.freshness.isFresh(mb::FreshnessSource::Probe, nowMs);
        if (!additionalOnlineInit_[i] || additionalOnline_[i] != online) {
            publishAdditionalAvailability(*device, online);
            additionalOnlineInit_[i] = true;
            additionalOnline_[i] = online;
        }
        if (pub.lastPublishedSampleMs != pub.lastSample.updatedMs ||
            nowMs - pub.lastPublishedMs >= 10000U) {
            publishAdditionalState(*device, pub, nowMs);
            pub.lastPublishedSampleMs = pub.lastSample.updatedMs;
            pub.lastPublishedMs = nowMs;
        }
    }
}

void HaMqtt::loop(const MeaterSample& sample, const CookControl& cook) {
#if MB_MQTT_ENABLED
    if (!enabled_) {
        return;
    }
    updateRssiTrackers(sample, millis());
    if (!client_.connected()) {
        const uint32_t now = millis();
        if (now - lastConnectAttemptMs_ >= 5000) {
            lastConnectAttemptMs_ = now;
            connect();
        }
        return;
    }

    client_.loop();
    const uint32_t now = millis();
    if (sample.valid &&
        (sample.updatedMs != lastSampleUpdate_ || now - lastPublishMs_ >= 10000)) {
        publishState(sample, cook);
        lastSampleUpdate_ = sample.updatedMs;
        lastPublishMs_ = now;
    }
#endif
}

void HaMqtt::loop(const MeaterSample& sample, const CookControl& cook,
                  mb::DeviceRegistry& devices, const mb::FreshnessTracker& freshness,
                  uint32_t nowMs) {
#if MB_MQTT_ENABLED
    if (!enabled_) return;

    size_t activeIndex = mb::DeviceRegistry::capacity();
    if (sample.valid && sample.deviceId != 0) {
        activeIndex = devices.indexOf(sample.deviceId);
        if (activeIndex < mb::DeviceRegistry::capacity()) {
            devicePublish_.attach(activeIndex, sample.deviceId);
            devicePublish_.observe(activeIndex, sample, nowMs);
            if (activeIndex == 0) updateRssiTrackers(sample, nowMs);
        }
    }

    const mb::DevicePublishState& primary = devicePublish_.slot(0);
    const MeaterSample* primarySample = primary.haveSample ? &primary.lastSample : &sample;
    const mb::FreshnessTracker* primaryFreshness = primary.haveSample ? &primary.freshness : &freshness;
    const uint32_t baseAge = primaryFreshness->ageMs(mb::FreshnessSource::Base, nowMs);
    const uint32_t probeAge = primaryFreshness->ageMs(mb::FreshnessSource::Probe, nowMs);
    baseAgeS_ = baseAge == UINT32_MAX ? -1 : static_cast<int32_t>(baseAge / 1000U);
    probeAgeS_ = probeAge == UINT32_MAX ? -1 : static_cast<int32_t>(probeAge / 1000U);
    const mb::AvailabilityModel availability(*primaryFreshness);
    probeTempFresh_ = availability.temperatureGated(mb::FreshnessSource::Probe, nowMs);
    baseTempFresh_ = availability.temperatureGated(mb::FreshnessSource::Base, nowMs);
    freshnessGating_ = true;

    if (!client_.connected()) {
        if (nowMs - lastConnectAttemptMs_ >= 5000) {
            lastConnectAttemptMs_ = nowMs;
            connect();
        }
        return;
    }

    client_.loop();
    publishAvailability(*primaryFreshness, nowMs);
    if (primarySample->valid &&
        (primarySample->updatedMs != lastSampleUpdate_ || nowMs - lastPublishMs_ >= 10000U)) {
        publishState(*primarySample, cook);
        lastSampleUpdate_ = primarySample->updatedMs;
        lastPublishMs_ = nowMs;
    }
    serviceAdditionalDevices(devices, freshness, nowMs);
#endif
}
