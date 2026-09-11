#pragma once

// Physical MEATER probe GATT: cook-setup write layout, plus RSSI / device
// identity / handoff-relevant characteristics.
//
// Reconstructed in this project's own style from static analysis of the
// authenticated MEATER Android 5.1.0 APK (version code 515). Decompiled sources
// are used ONLY as interoperability evidence; no application code is copied.
//
// !!! UNVERIFIED HARDWARE PATH !!!
// The GATT UUIDs and the cook-setup characteristic write LAYOUT below are
// STATICALLY PROVEN from string constants and the write routine in the APK.
// What is NOT proven without a real probe is that a write in this exact shape
// is *accepted* by current firmware and takes effect safely. Therefore every
// physical WRITE routed through this header is compiled out unless the operator
// explicitly opts in at BOTH compile time (MB_PROBE_WRITE_ENABLED) and run time
// (a config flag). The echo-only safe default performs NO probe writes.
//
// Reading (RSSI, identity, temperatures) is non-destructive and stays on the
// normal path; only cook-setup WRITES are gated.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bridge_config.h"
#include "bridge_types.h"
#include "protobuf_wire.h"

// Compile-time master switch for physical probe writes. Defaults OFF. Even when
// defined to 1 at build time, a run-time opt-in must also be true before any
// byte is written to the cook-setup characteristic (see RuntimeConfig's
// probe-write opt-in and MeaterBle::applyCookSetup). This double gate is the "writes default OFF"
// guarantee.
#ifndef MB_PROBE_WRITE_ENABLED
#define MB_PROBE_WRITE_ENABLED 0
#endif

namespace meater_gatt {

// ---------------------------------------------------------------------------
// GATT UUIDs.
//   Evidence: com/apptionlabs/meater_app/data/Config.java (string constants)
//   and p732x5/MEATERBLEUUID.java (UUID.fromString wrappers).
// All 128-bit. Kept as strings so NimBLE (NimBLEUUID) can consume them directly.
// ---------------------------------------------------------------------------

// --- Direct-to-probe (MEATER / MEATER+ first-generation temperature service) ---
// PROVEN: Config.java MEATERBLETemperatureServiceUUID
constexpr const char* kSvcMeaterTemperature = "a75cc7fc-c956-488f-ac2a-2dbc08b63a04";
// PROVEN: Config.java MEATERTemperatureBLECharacteristicUUID (notify: live temps)
constexpr const char* kChrTemperature = "7edda774-045e-4bbf-909b-45d1991a2876";
// PROVEN: Config.java MEATERBatteryBLECharacteristicUUID
constexpr const char* kChrBattery = "2adb4877-68d8-4884-bd3c-d83853bf27b8";
// PROVEN: Config.java MEATERTemperatureLogModeBLECharacteristicUUID (write: log mode/reset)
constexpr const char* kChrTemperatureLogMode = "575d3bf1-2757-45ad-94d9-875c2f6120d3";
// PROVEN: Config.java MEATERTemperatureLogBLECharactertisticUUID (read: stored log)
constexpr const char* kChrTemperatureLog = "b3e02c20-85be-4d1e-8da8-30cd88aaf0d4";
// PROVEN: Config.java MEATERCookSetupBLECharacteristicUUID (write: cook setup) <-- (b)
constexpr const char* kChrCookSetup = "caf28e64-3b17-4cb4-bb0a-2eaa33c47af7";

// --- Device Information (standard SIG service) ---
// PROVEN: Config.java DeviceInformationServiceUUID (0x180A)
constexpr const char* kSvcDeviceInformation = "0000180a-0000-1000-8000-00805f9b34fb";
// PROVEN: Config.java FirmwareRevisionCharacteristicUUID (0x2A26)
constexpr const char* kChrFirmwareRevision = "00002a26-0000-1000-8000-00805f9b34fb";
// PROVEN: Config.java BLESoftwareRevisionCharacteristicUUID (0x2A28)
constexpr const char* kChrSoftwareRevision = "00002a28-0000-1000-8000-00805f9b34fb";
// PROVEN: Config.java BLECharacteristicConfigUUID (0x2902 CCCD)
constexpr const char* kDescClientConfig = "00002902-0000-1000-8000-00805f9b34fb";

// --- MEATER+ base / charger relayed-probe characteristics (handoff-relevant) (c) ---
// PROVEN: Config.java MEATERPlusProbeRSSIUUID (notify: probe->base RSSI)
constexpr const char* kChrPlusProbeRssi = "370AABE7-4837-4BEE-AADC-CD1836DBCE53";
// PROVEN: Config.java MEATERPlusBatteryLevelUUID
constexpr const char* kChrPlusBatteryLevel = "22DB81C4-D125-4E8F-99A4-3609E4C9A017";
// PROVEN: Config.java MEATERPlusProbeInfoUUID (read/notify: identity + firmware)
constexpr const char* kChrPlusProbeInfo = "1CBFF55E-9A06-4721-A178-1E2D84246DD1";
// PROVEN: Config.java MEATERPlusProbeConnectionStateUUID (== Block connection state)
constexpr const char* kChrPlusProbeConnectionState = "E03C6CCC-2AA7-40A4-8A66-C98B599B737A";
// PROVEN: Config.java MEATERPlusChipTemperatureUUID
constexpr const char* kChrPlusChipTemperature = "BB9B2404-FCFB-4B73-8ACD-B1B08DA3749D";
// PROVEN: Config.java MEATERPlusV2TemperatureServiceUUID / SE service
constexpr const char* kSvcPlusV2Temperature = "C9E2746C-59F1-4E54-A0DD-E1E54555CF8B";
constexpr const char* kSvcPlusSeTemperature = "49141A23-307F-4E25-AD82-0A3F00D8B90B";

// PROVEN: Config.java BLECompanyIDApptionLabs = 891 (0x037B).
// Used to match MEATER advertisements by manufacturer-data company id.
constexpr uint16_t kCompanyIdApptionLabs = 891;

// ---------------------------------------------------------------------------
// (b) Cook-setup characteristic write layout.
//   Evidence: p732x5/MEATERProbeBLEConnection.java (the method that builds the
//   cook-setup write value; verified by its CookSetup.encode() call followed by
//   two System.arraycopy calls that concatenate the prefix and the encoded body,
//   then setWriteType(2)). Obfuscated method names drift between decompiles, so
//   this cites the code facts rather than a symbol name.
//
//   The value written to kChrCookSetup is:
//     [ 8-byte cookID (little-endian) ] ++ [ CookSetup protobuf bytes ]
//
//   - cookID prefix: 8 bytes via ByteBuffer.allocate(8).order(ByteOrder
//     .LITTLE_ENDIAN).putLong(cookID). PROVEN little-endian: p309U6/BinUtils.java
//     (ByteBuffer allocate(8) + order(ByteOrder.LITTLE_ENDIAN) + putLong).
//   - CookSetup body: the SAME CookSetup message defined for MEATER Link, encoded
//     with its ProtoAdapter (CookSetup.encode()). PROVEN it is raw protobuf, not
//     a hand-packed struct: the routine calls cookSetup.encode() then
//     System.arraycopy concatenates [prefix][encoded body].
//   - Write type: WRITE_TYPE_DEFAULT (setWriteType(2)), i.e. write-with-response.
//     PROVEN: bluetoothGattService.getCharacteristic(uuid).setWriteType(2).
//
//   CookSetup field tags (Evidence: v3protobuf/CookSetup.java @WireField
//   annotations + build() null-check for the REQUIRED set; symbolic tag
//   constants resolved from NetworkRequestMetric.java and Temperature.java):
//     required uint32          sequenceNumber            = 1;   // UINT32
//     required DeviceCookState state                     = 2;   // enum
//     required sint32          targetInternalTemperature = 3;   // SINT32 (1/32 C)
//     optional TemperatureRange targetAmbientTemperature = 4;   // message
//     optional uint32          cutID                     = 5;
//     optional uint32          presetID                  = 6;
//     optional fixed64         ongoingRecipeID           = 7;
//     optional EstimatorConfig estimatorConfig           = 8;
//     optional string          name                      = 9;
//     repeated Alarm           alarms                    = 10;  // TIME_TO_RESPONSE_COMPLETED_US_FIELD_NUMBER
//     optional uint32          clipNumber                = 11;  // NETWORK_CLIENT_ERROR_REASON_FIELD_NUMBER
//     optional fixed64         cookID                    = 12;  // CUSTOM_ATTRIBUTES_FIELD_NUMBER
//     optional uint32          recipeID                  = 13;  // PERF_SESSIONS_FIELD_NUMBER
//     optional uint32          recipeStepID              = 14;
//     optional AlarmState      flareUpAlert              = 15;
//     optional uint32          cookingAppliance          = 16;
//     required uint32          lastItem                  = 99;  // Temperature.MAX_INTERNAL_PROBE; DEFAULT 96
//
//   NOTE: the same CookSetup structure is already reconstructed for the UDP
//   MEATER Link path in this project; the physical write reuses it. Only tags
//   1, 2, 3 and 99 are `required` and must always be present.
// ---------------------------------------------------------------------------
constexpr size_t kCookIdPrefixBytes = 8;  // little-endian uint64 cookID, PROVEN

// ---------------------------------------------------------------------------
// (b) Write SEQUENCE.
//   Evidence: p732x5/MEATERProbeBLEConnection.java (state enum
//   MEATERProbeBLEConnectionState + the log-mode write helper). Verified: the
//   states RESETTING_LOG_THEN_WRITE_COOK_SETUP and
//   WRITING_COOK_SETUP_THEN_READ_TEMP_LOG exist, and the log-mode write helper
//   issues a SINGLE-byte write new byte[]{ TemperatureLogState.<value>() } to
//   MEATERBLEUUID for MEATERTemperatureLogModeBLECharacteristicUUID.
//   The app models cook-setup application as an ordered state machine:
//     RESETTING_LOG_THEN_WRITE_COOK_SETUP
//       -> write kChrTemperatureLogMode = single byte TemperatureLogState value
//          -- PROVEN it is one byte (new byte[]{ ... }, length 1).
//     WRITING_COOK_SETUP_THEN_READ_TEMP_LOG
//       -> write kChrCookSetup = [cookID LE64][CookSetup protobuf]
//       -> then read kChrTemperatureLog back.
//   TemperatureLogState.RESET is the proven single-byte value 0.
// ---------------------------------------------------------------------------
enum class CookSetupWritePhase : uint8_t {
    kResetLog = 0,       // write 1-byte log-mode reset first
    kWriteCookSetup = 1, // then write [cookID][CookSetup]
    kReadBackTempLog = 2 // then read the temperature-log characteristic
};

// ---------------------------------------------------------------------------
// (c) Device identity in the MEATER Link protobuf (for handoff/dedup).
//   Evidence: v3protobuf/MLDevice.java and MeaterLinkHeader.java.
//     MeaterLinkHeader.deviceID            fixed64 tag 5  (the bridge's own id)
//     MLDevice.identifier                  fixed64 tag 5  (per-device 64-bit id)
//     MLDevice.probeNumber                 uint32  tag 6
//     MLDevice.chargeState                 message tag 7
//     MLDevice.firmwareRevision            string  tag 8
//     MLDevice.connectionState             enum    tag 9  (connected == 1)
//     MLDevice.connectionType              enum    tag 10 (BLE == 0)
//     MLDevice.bleSignalLevel              sint32  tag 11 (RSSI, dBm)  <-- (c)
//     MLDevice.wifiSignalLevel             sint32  tag 12
//   These tags matter for handoff: bleSignalLevel is SINT32 (zig-zag), not the
//   plain int the older notes implied, so a negative RSSI must be zig-zag
//   encoded. This project's Writer::sint32Field already does that.
// ---------------------------------------------------------------------------
constexpr uint32_t kMlDeviceFieldProbe = 1;
constexpr uint32_t kMlDeviceFieldIdentifier = 5;
constexpr uint32_t kMlDeviceFieldProbeNumber = 6;
constexpr uint32_t kMlDeviceFieldChargeState = 7;
constexpr uint32_t kMlDeviceFieldFirmwareRevision = 8;
constexpr uint32_t kMlDeviceFieldConnectionState = 9;
constexpr uint32_t kMlDeviceFieldConnectionType = 10;
constexpr uint32_t kMlDeviceFieldBleSignalLevel = 11;   // SINT32, RSSI dBm
constexpr uint32_t kMlDeviceFieldWifiSignalLevel = 12;  // SINT32

// ---------------------------------------------------------------------------
// (c) BLE-side RSSI / identity reads (handoff between direct-probe and base).
//   - Live link RSSI to whatever the ESP32 is connected to: read from the
//     NimBLE client (readRssi), not a characteristic. NON-DESTRUCTIVE.
//   - When connected THROUGH a MEATER+ base, the probe's own RSSI is delivered
//     via notify on kChrPlusProbeRssi. Evidence: p732x5/MEATERPlusBLEConnection
//     .java mo64703x() enables notify on kChrPlusProbeRssi and reads
//     kChrPlusProbeInfo for identity.
//   - Hardware-confirmed child-info layout: byte 0 probe type, bytes 1..8
//     big-endian probe ID, bytes 9..23 firmware text.
//   - Probe RSSI is signed byte 0; 127 is the unavailable sentinel.
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> buildCookSetupPayload(const CookControl& cook) {
    pb::Writer setup;
    setup.uint32Field(1, cook.sequenceNumber);
    setup.enumField(2, cook.state);
    setup.sint32Field(3, cook.targetInternalRaw32);
    if (cook.name[0] != '\0') setup.stringField(9, cook.name);
    if (cook.cookId != 0) setup.fixed64Field(12, cook.cookId);
    setup.uint32Field(99, 96);
    std::vector<uint8_t> payload;
    payload.reserve(kCookIdPrefixBytes + setup.bytes().size());
    uint64_t id = cook.cookId;
    for (size_t i = 0; i < kCookIdPrefixBytes; ++i) {
        payload.push_back(static_cast<uint8_t>(id & 0xffU));
        id >>= 8U;
    }
    payload.insert(payload.end(), setup.bytes().begin(), setup.bytes().end());
    return payload;
}

// Is a physical cook-setup write permitted right now? Compile-time gate; the
// caller must AND this with the run-time config opt-in before writing.
constexpr bool writesCompiledIn() { return MB_PROBE_WRITE_ENABLED != 0; }

}  // namespace meater_gatt
