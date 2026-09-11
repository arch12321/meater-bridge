#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

class RuntimeConfig {
public:
    void begin();
    bool shouldStartPortal() const;
    void startPortal();
    void loop();
    bool portalActive() const { return portalActive_; }

    const String& wifiSsid() const { return wifiSsid_; }
    const String& wifiPassword() const { return wifiPassword_; }
    const String& mqttHost() const { return mqttHost_; }
    uint16_t mqttPort() const { return mqttPort_; }
    const String& mqttUser() const { return mqttUser_; }
    const String& mqttPassword() const { return mqttPassword_; }
    bool mqttTls() const { return mqttTls_; }
    const String& mqttCaCert() const { return mqttCaCert_; }
    const String& otaPassword() const { return otaPassword_; }
    const String& targetMac() const { return targetMac_; }
    const String& nodeId() const { return nodeId_; }
    bool preferBase() const { return preferBase_; }
    bool probeWritesEnabled() const { return probeWritesEnabled_; }

private:
    void load();
    void seedDefaults();
    void configureRoutes();
    void handleRoot();
    void handleSave();
    void handleReset();
    bool ensureAuthenticated();
    void stopPortal();
    String htmlEscape(const String& value) const;

    Preferences prefs_;
    DNSServer dns_;
    WebServer server_{80};
    bool portalActive_{false};
    uint32_t portalStartedMs_{0};
    String portalPassword_;

    String wifiSsid_;
    String wifiPassword_;
    String mqttHost_;
    uint16_t mqttPort_{1883};
    String mqttUser_;
    String mqttPassword_;
    bool mqttTls_{false};
    String mqttCaCert_;
    String otaPassword_;
    String targetMac_;
    String nodeId_{"meater_bridge"};
    bool preferBase_{true};
    bool probeWritesEnabled_{false};
};
