#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>

#include "bridge_config.h"
#include "device_registry.h"
#include "freshness.h"
#include "ha_mqtt.h"
#include "meater_ble.h"
#include "meater_link.h"
#include "runtime_config.h"
#include "runtime_config_validation.h"

namespace {

MeaterBle gBle;
MeaterLinkBridge gMeaterLink;
HaMqtt gHaMqtt;
RuntimeConfig gConfig;

// Shared runtime state for independent device identities and freshness gates.
mb::DeviceRegistry gDevices;
mb::FreshnessTracker gFreshness;

bool gLinkStarted = false;
bool gOtaStarted = false;
uint32_t gAppliedSetupRevision = 0;
uint64_t gPrimaryDeviceId = 0;
MeaterSample gPrimarySample{};
uint32_t gLastWifiAttemptMs = 0;
// [item 2] Timestamp of the last sample whose reading we counted toward Base/
// Probe freshness, so a stale (unchanging) sample cannot keep a source "fresh".
uint32_t gLastFreshUpdateMs_ = 0;

const char* authModeName(uint8_t mode) {
    static const char* names[] = {
        "open", "WEP", "WPA-PSK", "WPA2-PSK", "WPA/WPA2-PSK",
        "WPA2-Enterprise", "WPA3-PSK", "WPA2/WPA3-PSK", "WAPI-PSK"};
    return mode < (sizeof(names) / sizeof(names[0])) ? names[mode] : "unknown";
}

void diagnoseConfiguredNetwork() {
    log_i("Scanning 2.4 GHz for the configured SSID");
    const int count = WiFi.scanNetworks(false, true);
    int matches = 0;
    for (int i = 0; i < count; ++i) {
        if (WiFi.SSID(i) != gConfig.wifiSsid()) {
            continue;
        }
        ++matches;
        const uint8_t auth = static_cast<uint8_t>(WiFi.encryptionType(i));
        log_i("Configured AP visible: channel=%d RSSI=%d dBm security=%s (%u)",
              WiFi.channel(i), WiFi.RSSI(i), authModeName(auth), auth);
    }
    if (matches == 0) {
        log_e("Configured SSID is not visible to the ESP32-C3 on 2.4 GHz");
    }
    WiFi.scanDelete();
}

void connectWifi() {
    if (gConfig.wifiSsid().isEmpty()) {
        log_e("Wi-Fi is not configured; use the captive portal");
        gConfig.startPortal();
        return;
    }

    WiFi.mode(WIFI_STA);
    // ESP32-C3 requires modem sleep when Wi-Fi and BLE coexist; disabling it
    // makes esp_bt_controller_enable() abort inside coex_core_enable().
    WiFi.setSleep(true);
    diagnoseConfiguredNetwork();
    log_i("Connecting to the configured Wi-Fi SSID");
    WiFi.begin(gConfig.wifiSsid().c_str(), gConfig.wifiPassword().c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
        delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
        log_i("Wi-Fi connected: channel=%d RSSI=%d dBm IP=%s", WiFi.channel(), WiFi.RSSI(),
              WiFi.localIP().toString().c_str());
    } else {
        log_e("Wi-Fi connection timed out; background retry enabled");
        gConfig.startPortal();
    }
}

void serviceWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!gLinkStarted) {
            gMeaterLink.begin(ESP.getEfuseMac());
            gLinkStarted = true;
        }
        return;
    }
    if (gConfig.wifiSsid().isEmpty() || millis() - gLastWifiAttemptMs < 10000) {
        return;
    }
    gLastWifiAttemptMs = millis();
    Serial.println("Retrying Wi-Fi");
    WiFi.disconnect();
    WiFi.begin(gConfig.wifiSsid().c_str(), gConfig.wifiPassword().c_str());
}

void setupOta() {
    if (gOtaStarted || WiFi.status() != WL_CONNECTED || gConfig.otaPassword().isEmpty()) return;
    const mb::TextView password(gConfig.otaPassword().c_str(),
                                gConfig.otaPassword().length());
    if (!mb::strongOtaPassword(password)) {
        log_e("OTA disabled: configure a 12-128 character password with letters and digits");
        return;
    }
    ArduinoOTA.setHostname(gConfig.nodeId().c_str());
    ArduinoOTA.setPassword(gConfig.otaPassword().c_str());
    ArduinoOTA.onStart([]() { log_i("OTA update started"); });
    ArduinoOTA.onEnd([]() { log_i("OTA update complete"); });
    ArduinoOTA.onProgress([](unsigned progress, unsigned total) {
        log_i("OTA progress %u%%", total == 0 ? 0 : (progress * 100U) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) { log_e("OTA error %u", error); });
    ArduinoOTA.begin();
    gOtaStarted = true;
    log_i("Authenticated OTA enabled for %s", gConfig.nodeId().c_str());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\nMEATER Bridge %s starting on ESP32-C3\n", MB_FIRMWARE_VERSION);
    gConfig.begin();
    if (gConfig.shouldStartPortal()) gConfig.startPortal();
    // Reserve the primary device using the runtime node id so existing defaults
    // remain compatible while portal changes take effect after reboot.
    gDevices.begin(gConfig.nodeId().c_str());
    gFreshness.reset();
    connectWifi();
    serviceWifi();
    if (WiFi.status() == WL_CONNECTED) configTime(0, 0, "pool.ntp.org", "time.google.com");
    setupOta();
    gHaMqtt.begin(gConfig);
    gBle.begin(gConfig.targetMac().c_str(), gConfig.preferBase(),
               gConfig.probeWritesEnabled());
}

void loop() {
    gConfig.loop();
    serviceWifi();
    setupOta();
    if (gOtaStarted) ArduinoOTA.handle();
    gBle.loop();
    const MeaterSample sample = gBle.sample();

    // [item 2] Record per-source freshness. This is what gates output: a source
    // that stops refreshing ages past its threshold and its temperatures are
    // published null, so a stale reading is NEVER presented as current.
    //
    // Freshness must key off ACTUAL new readings, not the persistently-valid
    // sample struct. `sample.valid` latches true on the first reading and is
    // never cleared on disconnect (only `bleConnected` clears), so marking Base
    // on `sample.valid` alone would keep the source "fresh" forever after the
    // link drops and defeat the whole staleness gate. Mark Base/Probe only when
    // the link is live AND a new reading has actually landed (updatedMs moved).
    const uint32_t now = millis();
    gFreshness.mark(mb::FreshnessSource::Bridge, now);
    if (WiFi.status() == WL_CONNECTED) {
        gFreshness.mark(mb::FreshnessSource::WiFi, now);
    }
    if (gHaMqtt.connected()) {
        gFreshness.mark(mb::FreshnessSource::Mqtt, now);
    }
    const bool newReading = sample.valid && sample.updatedMs != gLastFreshUpdateMs_;
    if (sample.valid && sample.bleConnected && newReading) {
        // A fresh reading arrived over a live link: the base relay is current.
        gFreshness.mark(mb::FreshnessSource::Base, now);
        if (sample.probeConnected()) {
            // ...and the child probe is actively reporting through it.
            gFreshness.mark(mb::FreshnessSource::Probe, now);
        }
    }
    if (sample.valid) {
        gLastFreshUpdateMs_ = sample.updatedMs;
    }

    if (sample.valid && sample.deviceId != 0) {
        if (gPrimaryDeviceId == 0) gPrimaryDeviceId = sample.deviceId;
        if (sample.deviceId == gPrimaryDeviceId) gPrimarySample = sample;
    }
    MeaterSample linkSample = gPrimarySample.valid ? gPrimarySample : sample;
    if (sample.deviceId != gPrimaryDeviceId) {
        linkSample.bleConnected = false;
        linkSample.childState = 1;
    }
    gMeaterLink.loop(linkSample);
    const uint32_t setupRevision = gMeaterLink.setupRevision();
    if (setupRevision != gAppliedSetupRevision) {
        if (!gConfig.probeWritesEnabled()) {
            gAppliedSetupRevision = setupRevision;
        } else if (sample.deviceId == gPrimaryDeviceId && sample.bleConnected &&
                   gBle.applyCookSetup(gMeaterLink.cookControl())) {
            gAppliedSetupRevision = setupRevision;
        }
    }

    // [item 7] Bind discovered probe/base identities into the registry so
    // multi-device publishing has stable per-device identities. The FIRST
    // bound identity keeps slot 0 (MB_NODE_ID) so single-device HA entity IDs
    // are preserved. Additional distinct devices get their own node id, up to
    // MB_MAX_DEVICES; beyond that they are rejected (selection: keep the
    // already-admitted set, no eviction) which is safe under the C3 budget.
    if (sample.valid && sample.deviceId != 0) {
        gDevices.bind(sample.deviceId, mb::DeviceRole::Probe, sample.probeNumber,
                      sample.baseAddress);
    }

    // [item 2/8] Feed freshness ages into HA availability and publish raw+EMA
    // RSSI with quality bands via the freshness-aware loop overload.
    gHaMqtt.loop(sample, gMeaterLink.cookControl(), gDevices, gFreshness, now);
    delay(5);
}
