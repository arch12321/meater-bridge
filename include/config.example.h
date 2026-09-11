#pragma once

// Copy this file to include/config.h and edit the values below.

#define MB_WIFI_SSID ""
#define MB_WIFI_PASSWORD ""

// Leave MB_MQTT_HOST empty to disable MQTT/Home Assistant output.
#define MB_MQTT_HOST ""
#define MB_MQTT_PORT 1883
#define MB_MQTT_USER ""
#define MB_MQTT_PASSWORD ""

// Optional. Leave empty to connect to the first supported MEATER found.
// Example: "B8:1F:5E:12:34:56"
#define MB_MEATER_MAC ""

#define MB_NODE_ID "meater_bridge"
#define MB_FRIENDLY_NAME "MEATER Bridge"
#define MB_MQTT_DISCOVERY_PREFIX "homeassistant"
#define MB_MQTT_STATE_PREFIX "meater_bridge"

// Prefer a MEATER+/SE charger repeater when both it and the direct probe advertise.
#define MB_PREFER_BASE 1

#define MB_MEATER_LINK_ENABLED 1

// Runtime provisioning/security defaults; captive portal values override these in NVS.
#define MB_MQTT_TLS 0
#define MB_MQTT_CA_CERT ""
#define MB_OTA_PASSWORD ""
#define MB_PROBE_WRITE_RUNTIME 0
#define MB_PROBE_WRITE_ENABLED 0
#define MB_CONFIG_BUTTON_PIN 9
#define MB_MQTT_ENABLED 1
#define MB_LOG_RAW_BLE 0

// Maximum independently-tracked MEATER devices (each gets its own HA identity
// and MQTT topics). Slot 0 always reuses MB_NODE_ID for single-device backward
// compatibility. Range 1..8; kept small for the ESP32-C3 memory budget.
#define MB_MAX_DEVICES 4

// RSSI EMA smoothing factor alpha = num/den (default 0.3). Lower = smoother.
#define MB_RSSI_EMA_ALPHA_NUM 3
#define MB_RSSI_EMA_ALPHA_DEN 10

// One-active-link multi-device scheduling. With a fixed MB_MEATER_MAC, rotation
// is disabled. Otherwise eligible devices receive this dwell time and retained
// values become unavailable after the stale window.
#define MB_MULTI_DEVICE_ENABLED 1
#define MB_MULTI_DEVICE_DWELL_MS 30000
#define MB_MULTI_DEVICE_STALE_MS 180000
