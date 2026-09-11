#include "runtime_config.h"

#include <WiFi.h>
#include <esp_system.h>

#include "bridge_config.h"
#include "meater_probe_gatt.h"
#include "runtime_config_validation.h"

void RuntimeConfig::begin() {
    pinMode(MB_CONFIG_BUTTON_PIN, INPUT_PULLUP);
    prefs_.begin("meater", false);
    if (!prefs_.getBool("seeded", false)) seedDefaults();
    load();
}

void RuntimeConfig::seedDefaults() {
    prefs_.putString("wifi_ssid", MB_WIFI_SSID);
    prefs_.putString("wifi_pass", MB_WIFI_PASSWORD);
    prefs_.putString("mqtt_host", MB_MQTT_HOST);
    prefs_.putUShort("mqtt_port", MB_MQTT_PORT);
    prefs_.putString("mqtt_user", MB_MQTT_USER);
    prefs_.putString("mqtt_pass", MB_MQTT_PASSWORD);
    prefs_.putBool("mqtt_tls", MB_MQTT_TLS != 0);
    prefs_.putString("mqtt_ca", MB_MQTT_CA_CERT);
    prefs_.putString("ota_pass", MB_OTA_PASSWORD);
    prefs_.putString("target_mac", MB_MEATER_MAC);
    prefs_.putString("node_id", MB_NODE_ID);
    prefs_.putBool("prefer_base", MB_PREFER_BASE != 0);
    prefs_.putBool("probe_write", MB_PROBE_WRITE_RUNTIME != 0);
    prefs_.putBool("seeded", true);
}

void RuntimeConfig::load() {
    wifiSsid_ = prefs_.getString("wifi_ssid", "");
    wifiPassword_ = prefs_.getString("wifi_pass", "");
    mqttHost_ = prefs_.getString("mqtt_host", "");
    mqttPort_ = prefs_.getUShort("mqtt_port", 1883);
    mqttUser_ = prefs_.getString("mqtt_user", "");
    mqttPassword_ = prefs_.getString("mqtt_pass", "");
    mqttTls_ = prefs_.getBool("mqtt_tls", false);
    mqttCaCert_ = prefs_.getString("mqtt_ca", "");
    otaPassword_ = prefs_.getString("ota_pass", "");
    targetMac_ = prefs_.getString("target_mac", "");
    nodeId_ = prefs_.getString("node_id", "meater_bridge");
    preferBase_ = prefs_.getBool("prefer_base", true);
    probeWritesEnabled_ = prefs_.getBool("probe_write", false) &&
                          meater_gatt::writesCompiledIn();
}

bool RuntimeConfig::shouldStartPortal() const {
    return wifiSsid_.isEmpty() || digitalRead(MB_CONFIG_BUTTON_PIN) == LOW;
}

String RuntimeConfig::htmlEscape(const String& value) const {
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const char ch = value[i];
        if (ch == '&') out += F("&amp;");
        else if (ch == '<') out += F("&lt;");
        else if (ch == '>') out += F("&gt;");
        else if (ch == '"') out += F("&quot;");
        else out += ch;
    }
    return out;
}

void RuntimeConfig::configureRoutes() {
    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/save", HTTP_POST, [this]() { handleSave(); });
    server_.on("/reset", HTTP_POST, [this]() { handleReset(); });
    server_.onNotFound([this]() { handleRoot(); });
}

void RuntimeConfig::startPortal() {
    if (portalActive_) return;
    const uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac());
    char apName[32];
    snprintf(apName, sizeof(apName), "MEATER-Bridge-%06lx",
             static_cast<unsigned long>(chip & 0xffffff));
    portalPassword_ = "Meater1234";
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName, portalPassword_.c_str());
    dns_.start(53, "*", WiFi.softAPIP());
    configureRoutes();
    server_.begin();
    portalStartedMs_ = millis();
    portalActive_ = true;
    log_i("Configuration AP %s at %s; user admin; password %s", apName,
          WiFi.softAPIP().toString().c_str(), portalPassword_.c_str());
}

bool RuntimeConfig::ensureAuthenticated() {
    if (server_.authenticate("admin", portalPassword_.c_str())) return true;
    server_.requestAuthentication(BASIC_AUTH, "MEATER Bridge Setup");
    return false;
}

void RuntimeConfig::stopPortal() {
    if (!portalActive_) return;
    server_.stop();
    dns_.stop();
    WiFi.softAPdisconnect(true);
    portalActive_ = false;
    portalPassword_ = "";
    log_i("Configuration portal closed");
}

void RuntimeConfig::handleRoot() {
    if (!ensureAuthenticated()) return;
    String page;
    page.reserve(5000);
    page = F("<!doctype html><meta name=viewport content='width=device-width'><title>MEATER Bridge</title><style>body{font-family:sans-serif;max-width:720px;margin:2em auto;padding:0 1em}label{display:block;margin-top:1em}input,textarea{width:100%;padding:.6em;box-sizing:border-box}button{margin-top:1.2em;padding:.8em}</style><h1>MEATER Bridge</h1><form method=post action=/save>");
    auto input = [&](const char* label, const char* name, const String& value, const char* type="text") {
        page += "<label>"; page += label; page += "<input type="; page += type;
        page += " name="; page += name; page += " value=\""; page += htmlEscape(value); page += "\"></label>";
    };
    input("Wi-Fi SSID", "wifi_ssid", wifiSsid_);
    input("Wi-Fi password (blank keeps saved value)", "wifi_pass", "", "password");
    input("MQTT host", "mqtt_host", mqttHost_);
    input("MQTT port", "mqtt_port", String(mqttPort_), "number");
    input("MQTT user", "mqtt_user", mqttUser_);
    input("MQTT password (blank keeps saved value)", "mqtt_pass", "", "password");
    input("Node ID", "node_id", nodeId_);
    input("Preferred MEATER MAC (optional)", "target_mac", targetMac_);
    input("OTA password (blank keeps saved value)", "ota_pass", "", "password");
    page += "<label><input style='width:auto' type=checkbox name=mqtt_tls value=1";
    if (mqttTls_) page += " checked";
    page += "> MQTT TLS</label><label>MQTT CA certificate<textarea name=mqtt_ca rows=8>";
    page += htmlEscape(mqttCaCert_); page += "</textarea></label>";
    page += "<label><input style='width:auto' type=checkbox name=prefer_base value=1";
    if (preferBase_) page += " checked";
    page += "> Prefer charger/base</label>";
#if MB_PROBE_WRITE_ENABLED
    page += "<label><input style='width:auto' type=checkbox name=probe_write value=1";
    if (probeWritesEnabled_) page += " checked";
    page += "> Enable physical probe cook writes (experimental; compile gate enabled)</label>";
#else
    page += "<p>Physical probe cook writes are compiled out (safe default).</p>";
#endif
    page += "<button type=submit>Save and reboot</button></form><form method=post action=/reset><button>Factory-reset configuration</button></form>";
    server_.send(200, "text/html", page);
}

void RuntimeConfig::handleSave() {
    if (!ensureAuthenticated()) return;
    const String wifiSsid = server_.arg("wifi_ssid");
    const String mqttHost = server_.arg("mqtt_host");
    const String nodeId = server_.arg("node_id");
    const String targetMac = server_.arg("target_mac");
    const String otaPassword = server_.arg("ota_pass");
    const String caCertificate = server_.arg("mqtt_ca");
    const long mqttPort = server_.arg("mqtt_port").toInt();
    auto view = [](const String& value) {
        return mb::TextView(value.c_str(), value.length());
    };
    if (wifiSsid.isEmpty() || wifiSsid.length() > 32 ||
        !mb::validMqttPort(mqttPort) || !mb::validNodeId(view(nodeId)) ||
        !mb::validMacOrEmpty(view(targetMac)) || mqttHost.length() > 253 ||
        server_.arg("wifi_pass").length() > 128 ||
        server_.arg("mqtt_pass").length() > 128 || caCertificate.length() > 8192) {
        server_.send(400, "text/plain", "Invalid configuration value");
        return;
    }
    if (!otaPassword.isEmpty() && !mb::strongOtaPassword(view(otaPassword))) {
        server_.send(400, "text/plain",
                     "OTA password must be 12-128 printable characters with letters and digits");
        return;
    }
    if (server_.hasArg("mqtt_tls") && !mb::looksLikePemCertificate(view(caCertificate))) {
        server_.send(400, "text/plain", "MQTT TLS requires a PEM CA certificate");
        return;
    }

    prefs_.putString("wifi_ssid", wifiSsid);
    if (!server_.arg("wifi_pass").isEmpty()) prefs_.putString("wifi_pass", server_.arg("wifi_pass"));
    prefs_.putString("mqtt_host", mqttHost);
    prefs_.putUShort("mqtt_port", static_cast<uint16_t>(mqttPort));
    prefs_.putString("mqtt_user", server_.arg("mqtt_user"));
    if (!server_.arg("mqtt_pass").isEmpty()) prefs_.putString("mqtt_pass", server_.arg("mqtt_pass"));
    prefs_.putBool("mqtt_tls", server_.hasArg("mqtt_tls"));
    prefs_.putString("mqtt_ca", caCertificate);
    prefs_.putString("node_id", nodeId);
    prefs_.putString("target_mac", targetMac);
    if (!otaPassword.isEmpty()) prefs_.putString("ota_pass", otaPassword);
    prefs_.putBool("prefer_base", server_.hasArg("prefer_base"));
    prefs_.putBool("probe_write", server_.hasArg("probe_write") &&
                                    meater_gatt::writesCompiledIn());
    server_.send(200, "text/html", "Saved. Rebooting...");
    delay(500);
    ESP.restart();
}

void RuntimeConfig::handleReset() {
    if (!ensureAuthenticated()) return;
    prefs_.clear();
    server_.send(200, "text/html", "Configuration cleared. Rebooting...");
    delay(500);
    ESP.restart();
}

void RuntimeConfig::loop() {
    if (!portalActive_) return;
    dns_.processNextRequest();
    server_.handleClient();
    if (!wifiSsid_.isEmpty() && millis() - portalStartedMs_ >= 600000U) stopPortal();
}
