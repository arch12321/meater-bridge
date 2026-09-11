#pragma once

// Config fallback: PlatformIO adds include/ to the search path. The user's
// config.h takes priority; if it doesn't exist, we try config.example.h which
// provides safe defaults for CI/demo builds (empty SSID/MQTT = captive portal).
//
// Note: __has_include with quotes searches include paths set by PlatformIO.
// Both config.h and config.example.h live in include/, so both forms work.

#if defined(MB_SKIP_CONFIG_H)
// Build system explicitly says skip config.h (e.g., host tests)
#elif __has_include("config.h")
#include "config.h"
#elif __has_include("config.example.h")
#include "config.example.h"
#endif

// Provide defaults for anything not defined by config.h or config.example.h.
// This ensures builds work even with an empty or partial config file.

#ifndef MB_WIFI_SSID
#define MB_WIFI_SSID ""
#endif
#ifndef MB_WIFI_PASSWORD
#define MB_WIFI_PASSWORD ""
#endif
#ifndef MB_MQTT_HOST
#define MB_MQTT_HOST ""
#endif
#ifndef MB_MQTT_PORT
#define MB_MQTT_PORT 1883
#endif
#ifndef MB_MQTT_USER
#define MB_MQTT_USER ""
#endif
#ifndef MB_MQTT_PASSWORD
#define MB_MQTT_PASSWORD ""
#endif
#ifndef MB_MEATER_MAC
#define MB_MEATER_MAC ""
#endif
#ifndef MB_NODE_ID
#define MB_NODE_ID "meater_bridge"
#endif
#ifndef MB_FRIENDLY_NAME
#define MB_FRIENDLY_NAME "MEATER Bridge"
#endif
#ifndef MB_MQTT_DISCOVERY_PREFIX
#define MB_MQTT_DISCOVERY_PREFIX "homeassistant"
#endif
#ifndef MB_MQTT_STATE_PREFIX
#define MB_MQTT_STATE_PREFIX "meater_bridge"
#endif
#ifndef MB_PROBE_WRITE_ENABLED
#define MB_PROBE_WRITE_ENABLED 0
#endif

#ifndef MB_FIRMWARE_VERSION
#define MB_FIRMWARE_VERSION "dev"
#endif

#ifndef MB_MEATER_LINK_ENABLED
#define MB_MEATER_LINK_ENABLED 1
#endif

#ifndef MB_MQTT_ENABLED
#define MB_MQTT_ENABLED 1
#endif

#ifndef MB_PREFER_BASE
#define MB_PREFER_BASE 1
#endif

#ifndef MB_LOG_RAW_BLE
#define MB_LOG_RAW_BLE 0
#endif

// [item 7] Maximum number of independently-tracked MEATER devices (probes/
// bases) that get their own MQTT/HA identity and topics. Slot 0 is always the
// primary device and reuses MB_NODE_ID for backward compatibility. Kept small
// for the ESP32-C3 connection/memory budget; see device_registry.h.
#ifndef MB_MAX_DEVICES
#define MB_MAX_DEVICES 4
#endif
#ifndef MB_MULTI_DEVICE_ENABLED
#define MB_MULTI_DEVICE_ENABLED 1
#endif
#ifndef MB_MULTI_DEVICE_DWELL_MS
#define MB_MULTI_DEVICE_DWELL_MS 30000
#endif
#ifndef MB_MULTI_DEVICE_STALE_MS
#define MB_MULTI_DEVICE_STALE_MS 180000
#endif

// [item 8] RSSI EMA smoothing factor as an integer fraction (alpha =
// num/den). Default 0.3 = responsive but noticeably smoothed. Applied to both
// the ESP32<->base link RSSI and the base<->probe RSSI.
#ifndef MB_RSSI_EMA_ALPHA_NUM
#define MB_RSSI_EMA_ALPHA_NUM 3
#endif
#ifndef MB_RSSI_EMA_ALPHA_DEN
#define MB_RSSI_EMA_ALPHA_DEN 10
#endif

// [item 1] BLE ownership/handoff reliability tunables. Drive the BleSupervisor
// (ble_supervisor.h). All are milliseconds unless noted. Defaults chosen for a
// powered MEATER+ base under normal same-room RSSI; tighten only if the base is
// very close and Wi-Fi coexistence headroom is comfortable.
#ifndef MB_BLE_SCAN_WINDOW_MS
#define MB_BLE_SCAN_WINDOW_MS 6000
#endif
#ifndef MB_BLE_MAX_SCANS_BEFORE_BACKOFF
#define MB_BLE_MAX_SCANS_BEFORE_BACKOFF 3
#endif
#ifndef MB_BLE_CONNECT_WATCHDOG_MS
#define MB_BLE_CONNECT_WATCHDOG_MS 10000
#endif
// No-temperature watchdog: a connected link that yields no temperature within
// this budget is torn down so a silent link is never presented as live.
#ifndef MB_BLE_NO_TEMP_WATCHDOG_MS
#define MB_BLE_NO_TEMP_WATCHDOG_MS 45000
#endif
#ifndef MB_BLE_BACKOFF_BASE_MS
#define MB_BLE_BACKOFF_BASE_MS 2000
#endif
#ifndef MB_BLE_BACKOFF_MAX_MS
#define MB_BLE_BACKOFF_MAX_MS 60000
#endif
#ifndef MB_BLE_BACKOFF_JITTER_MS
#define MB_BLE_BACKOFF_JITTER_MS 750
#endif
// Anti-thrash: a link dropping sooner than this after connecting is a "rapid
// drop" (fingerprint of Android holding the base's single central slot).
#ifndef MB_BLE_RAPID_DROP_MS
#define MB_BLE_RAPID_DROP_MS 4000
#endif
#ifndef MB_BLE_RAPID_DROPS_FOR_COOLDOWN
#define MB_BLE_RAPID_DROPS_FOR_COOLDOWN 3
#endif
// After the threshold of rapid drops, deprioritise the base path for this long
// so the bridge stops fighting the phone for the single-central slot.
#ifndef MB_BLE_BASE_COOLDOWN_MS
#define MB_BLE_BASE_COOLDOWN_MS 30000
#endif

#ifndef MB_MQTT_TLS
#define MB_MQTT_TLS 0
#endif
#ifndef MB_MQTT_CA_CERT
#define MB_MQTT_CA_CERT ""
#endif
#ifndef MB_OTA_PASSWORD
#define MB_OTA_PASSWORD ""
#endif
#ifndef MB_PROBE_WRITE_RUNTIME
#define MB_PROBE_WRITE_RUNTIME 0
#endif
#ifndef MB_CONFIG_BUTTON_PIN
#define MB_CONFIG_BUTTON_PIN 9
#endif
