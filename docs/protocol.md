# MEATER Link v17.8 notes

These notes come from static analysis of the authenticated MEATER Android 5.1.0 APK (version code 515). They describe only the fields implemented by this bridge.

## Transport

- UDP port: `7878`
- Protocol identifier: `21578`
- Major version: `17`
- Minor version advertised by 5.1.0: `8`
- Maximum Android receive buffer: 1,500 bytes
- Discovery: Android broadcasts a `SubscriptionMessage` to `255.255.255.255:7878`.
- Renewal: approximately 3 seconds unicast / 6 seconds broadcast; subscriber timeout 30 seconds.
- State: a peer master returns `MasterMessage` packets by unicast and may broadcast state for discovery.

## Envelope

```proto
message MeaterLinkHeader {
  required uint32  meaterLinkIdentifier = 1;
  required uint32  versionMajor         = 2;
  required uint32  versionMinor         = 3;
  required uint32  messageNumber        = 4;
  required fixed64 deviceID             = 5;
}

message MeaterLinkMessage {
  required MeaterLinkHeader header               = 1;
  optional SubscriptionMessage subscription      = 2;
  optional MasterMessage master                  = 3;
  optional SetupMessage setup                    = 4;
  optional TemperatureHistoryRequestMessage historyReq = 5;
  optional TemperatureHistoryMessage history           = 6;
  optional MasterStatusRequestMessage statusReq  = 10;
  optional MasterStatusMessage status            = 11;
}
```

The Android Wire-generated class enforces one payload field at a time
(`MeaterLinkMessage`'s constructor calls `Internal.countNonNull(...) > 1` over
all payload arms). The payload field *names* above (`subscription`, `master`,
`setup`, `statusReq`, `status`) are documentation aliases; the actual Wire class
fields are `subscriptionMessage`, `masterMessage`, `setupMessage`,
`masterStatusRequestMessage`, `masterStatusMessage`. On the wire only the tag
numbers matter, and every tag above is verified against
`v3protobuf/MeaterLinkMessage.java`. `MeaterLinkMessage` also carries payload
arms this bridge does not implement — tag 7 `blockFirmwareUpdateMessage`, 8
`networkSettingsRequestMessage`, 9 `networkSettingsMessage`, 12
`ongoingRecipeDiscoveryMessage`, 13 `ongoingRecipeSubscriptionMessage`, 14
`ongoingRecipeMessage`, 15 `pingPongMessage` — omitted here deliberately.

## State packet subset

```proto
message MasterMessage {
  required MasterType masterType = 1;                       // Android peer = 2 (MasterType.MASTER_TYPE_ANDROID(2))
  required CloudConnectionState cloudConnectionState = 2;   // disabled = 0 (CloudConnectionState.CLOUD_CONNECTION_STATE_DISABLED(0))
  repeated MLDevice devices = 3;
}

message MLDevice {
  optional MLProbe probe = 1;                         // oneof { probe(1), plus(2), block(3), amber(4) } — bridge uses probe
  required fixed64 identifier = 5;
  required uint32 probeNumber = 6;
  required ChargeState chargeState = 7;
  optional string firmwareRevision = 8;
  required ConnectionState connectionState = 9;       // connected = 1
  required DeviceConnectionType connectionType = 10;  // BLE = 0
  optional sint32 bleSignalLevel = 11;
}

message MLProbe {
  required fixed64 parentIdentifier = 1;
  required CookSetup setup = 3;
  required CookStatus status = 4;
}

message CookStatus {
  required sint32 internalTemperature = 1;  // 1/32 °C
  required sint32 ambientTemperature = 2;   // 1/32 °C
  required sint32 peakTemperature = 3;
  required sint32 remainingCookTime = 4;
  required uint32 elapsedTime = 5;
  required sint32 totalRemainingTime = 6;
  repeated sint32 internalTemperatures = 7; // unpacked
}

message ChargeState {
  required ChargingStatus chargingStatus = 1;
  required uint32 batteryLevelPercent = 2;
  required uint32 batteryHoursRemaining = 3;
}
```

`CookSetup` minimally requires tags 1 (`sequenceNumber`), 2 (`state`), 3 (`targetInternalTemperature`) and 99 (`lastItem`, current default 96).

## Implemented inbound messages

- `SubscriptionMessage`: records/renews the sender as a subscriber and triggers immediate state.
- `MasterStatusRequestMessage`: returns bridge identity/version.
- `SetupMessage`: stores sequence, state, target, name and cook ID, then echoes them in subsequent state.
- `TemperatureHistoryRequestMessage`: validates the requested child probe ID and returns a bounded physical-log history response.

## TemperatureHistory request/response (MEATER Link v17.8)

Reconstructed from the Wire-generated protobuf classes in the MEATER Android
5.1.0 APK. Field numbers are read from each field's `@WireField(tag = N)`
annotation; the annotation's `adapter = "...#FIXED64/#UINT32/#SINT32"` fixes the
wire encoding, and `label = WireField.Label.REQUIRED/REPEATED` (plus the
`build()` null-check) fixes the required/repeated flag. Implementation:
`src/meater_link_history.h`. This path is **statically proven** and rides the
existing UDP transport, so it is enabled by default (no hardware needed to
exercise it).

Evidence-citation note: citations point at the class file and the
`@WireField(tag = N)` annotation on the named field. They deliberately do NOT
quote decompiler line numbers — the decompiled output orders fields
alphabetically and the annotation/line offsets are not stable, so a line number
would be false precision. Verify a field by opening the cited class and reading
its `@WireField` annotation.

Top-level routing (`MeaterLinkMessage`), verified against
`v3protobuf/MeaterLinkMessage.java` (`@WireField` tags + the `decode()` switch):

| tag | field | evidence |
|----:|-------|----------|
| 5 | `temperatureHistoryRequestMessage` | `v3protobuf/MeaterLinkMessage.java` `@WireField(tag = 5)` + `decode()` case 5 |
| 6 | `temperatureHistoryMessage` | `v3protobuf/MeaterLinkMessage.java` `@WireField(tag = 6)` + `decode()` case 6 |

```proto
message TemperatureHistoryRequestMessage {   // v3protobuf/TemperatureHistoryRequestMessage.java
  required fixed64 deviceID = 1;              // @WireField(adapter=#FIXED64, label=REQUIRED, tag=1)
}

message TemperatureHistoryMessage {           // v3protobuf/TemperatureHistoryMessage.java
  required fixed64          deviceID = 1;      // @WireField(#FIXED64, REQUIRED, tag=1)
  required TemperatureHistory history = 2;     // @WireField(TemperatureHistory#ADAPTER, REQUIRED, tag=2)
  required fixed64          cookID   = 3;      // @WireField(#FIXED64, REQUIRED, tag=3)
}

message TemperatureHistory {                   // v3protobuf/TemperatureHistory.java
  required uint32               startTime = 1; // @WireField(#UINT32, REQUIRED, tag=1) — epoch seconds, cook start
  required uint32               interval  = 2; // @WireField(#UINT32, REQUIRED, tag=2) — seconds between samples
  repeated TemperatureRecording values    = 3; // @WireField(TemperatureRecording#ADAPTER, REPEATED, tag=3)
  repeated sint32               peaks     = 4; // @WireField(#SINT32, REPEATED, tag=4)
}

message TemperatureRecording {                 // v3protobuf/TemperatureRecording.java
  required sint32 internal = 1;                // @WireField(#SINT32, REQUIRED, tag=1)
  required sint32 ambient  = 2;                // @WireField(#SINT32, REQUIRED, tag=2)
}
```

Units: `internal`/`ambient`/`peaks` reuse the app's normalized signed **1/32 °C**
convention (same as `CookStatus`). That the *history* samples share the 1/32 °C
scaling is inferred from the shared `CookStatus` convention, not stated in
`TemperatureRecording.java` — treat the exact scale as consistent-but-not-locally-proven.

### Physical temperature-log source

The bridge reads `b3e02c20-85be-4d1e-8da8-30cd88aaf0d4` before changing live log mode and again when the base reports a connected/ready child state.

| payload length | layout used by bridge |
|---:|---|
| 484 | LE `int16 interval`, LE `int16 count`, then up to 120 internal/ambient `int16` pairs |
| 499 | 484-byte converted body padded to 120 pairs, state byte at 484, LE `uint32 elapsedSeconds` at 485 |
| 512 | raw variant with four-byte prefix and eight-byte pre-sample header; parsed defensively |

Intervals must be positive multiples of five seconds and counts are capped at 120. G1 samples are converted from 1/16 °C to 1/32 °C. The 499-byte elapsed field is authoritative when plausible; otherwise elapsed falls back to `count × interval`. History responses retain newest samples and are trimmed only if pathological varint widths would exceed the 1,500-byte UDP receive limit.

## Physical probe cook-setup GATT write — UNVERIFIED HARDWARE PATH

Implementation: `src/meater_probe_gatt.h` and `MeaterBle::applyCookSetup`. **Physical writes are compiled out
unless `MB_PROBE_WRITE_ENABLED=1` AND a run-time opt-in is set; the safe default
is echo-only, no probe writes.** The layout below is statically proven from the
APK, but that current firmware accepts a write of this exact shape is not
verified without a real probe.

GATT constants (evidence: `data/Config.java` string constants, wrapped in
`p732x5/MEATERBLEUUID.java`):

| purpose | UUID | Config.java constant |
|---------|------|----------------------|
| temperature service | `a75cc7fc-c956-488f-ac2a-2dbc08b63a04` | `MEATERBLETemperatureServiceUUID` |
| live temperatures (notify) | `7edda774-045e-4bbf-909b-45d1991a2876` | `MEATERTemperatureBLECharacteristicUUID` |
| battery | `2adb4877-68d8-4884-bd3c-d83853bf27b8` | `MEATERBatteryBLECharacteristicUUID` |
| temp-log mode (write) | `575d3bf1-2757-45ad-94d9-875c2f6120d3` | `MEATERTemperatureLogModeBLECharacteristicUUID` |
| temp log (read) | `b3e02c20-85be-4d1e-8da8-30cd88aaf0d4` | `MEATERTemperatureLogBLECharactertisticUUID` |
| **cook setup (write)** | `caf28e64-3b17-4cb4-bb0a-2eaa33c47af7` | `MEATERCookSetupBLECharacteristicUUID` |

**Cook-setup write payload** (evidence: `p732x5/MEATERProbeBLEConnection.java` —
the method that builds the cook-setup write value; cited by its code facts,
since the obfuscated method name is not stable across decompiles):

```
value = [ cookID : uint64 little-endian, 8 bytes ] ++ [ CookSetup protobuf bytes ]
```

- The cookID prefix is 8 bytes from `ByteBuffer.allocate(8)`,
  `.order(ByteOrder.LITTLE_ENDIAN)`, `.putLong(cookID)` — **little-endian,
  proven** (`p309U6/BinUtils.java`, the `allocate(8)` + `order(LITTLE_ENDIAN)` +
  `putLong` helper).
- The body is the *same* `CookSetup` message, `CookSetup.encode()`d — **raw
  protobuf, not a packed struct** (proven: the routine calls `.encode()` then
  `System.arraycopy` concatenates `[prefix][encoded body]`).
- Write type `WRITE_TYPE_DEFAULT` / `setWriteType(2)` (write-with-response), proven
  (`bluetoothGattService.getCharacteristic(uuid).setWriteType(2)`).

`CookSetup` field tags (evidence: `v3protobuf/CookSetup.java`; symbolic tag
constants resolved from `p193Ma/NetworkRequestMetric.java` and
`data/Temperature.java`):

| tag | field | type | notes |
|----:|-------|------|-------|
| 1 | `sequenceNumber` | uint32 | **required** |
| 2 | `state` | `DeviceCookState` enum | **required** |
| 3 | `targetInternalTemperature` | sint32 | **required**, 1/32 °C |
| 4 | `targetAmbientTemperature` | `TemperatureRange` | |
| 5 | `cutID` | uint32 | |
| 6 | `presetID` | uint32 | |
| 7 | `ongoingRecipeID` | fixed64 | |
| 8 | `estimatorConfig` | `EstimatorConfig` | |
| 9 | `name` | string | |
| 10 | `alarms` | repeated `Alarm` | |
| 11 | `clipNumber` | uint32 | |
| 12 | `cookID` | fixed64 | |
| 13 | `recipeID` | uint32 | |
| 14 | `recipeStepID` | uint32 | |
| 15 | `flareUpAlert` | `AlarmState` | |
| 16 | `cookingAppliance` | uint32 | |
| 99 | `lastItem` | uint32 | **required**, default 96 |

```proto
message TemperatureRange {   // v3protobuf/TemperatureRange.java
  required sint32 low    = 1; // @WireField(#SINT32, REQUIRED, tag=1)  (1/32 C)
  required sint32 offset = 2; // @WireField(#SINT32, REQUIRED, tag=2)
}
```

**Write sequence** (evidence: `MEATERProbeBLEConnection.java` — the
`MEATERProbeBLEConnectionState` enum plus the log-mode write helper; the
`RESETTING_LOG_THEN_WRITE_COOK_SETUP` and `WRITING_COOK_SETUP_THEN_READ_TEMP_LOG`
state names and the single-byte log-mode write are proven):

1. `RESETTING_LOG_THEN_WRITE_COOK_SETUP` — write the single byte `0x00` (`TemperatureLogState.RESET`, proven enum value) to the temp-log mode characteristic.
2. `WRITING_COOK_SETUP_THEN_READ_TEMP_LOG` — write `[cookID LE64][CookSetup]` to the cook-setup characteristic with response.
3. Schedule a temperature-log readback.

The bridge performs this sequence only when `MB_PROBE_WRITE_ENABLED=1`, the NVS runtime opt-in is also enabled, the primary probe is the active BLE owner and both characteristics report write capability.

## RSSI / device identity / handoff characteristics

**In the MEATER Link protobuf** (evidence: `v3protobuf/MLDevice.java` and
`v3protobuf/MeaterLinkHeader.java`; each row verified against that field's
`@WireField` annotation and the class `decode()`/`encode()` methods):

| tag | `MLDevice` field | type | note |
|----:|------------------|------|------|
| 5 | `identifier` | fixed64 | 64-bit device id (REQUIRED) |
| 6 | `probeNumber` | uint32 | REQUIRED |
| 8 | `firmwareRevision` | string | optional |
| 9 | `connectionState` | enum | REQUIRED; connected == 1 (`ConnectionState.CONNECTION_STATE_CONNECTED(1)`) |
| 10 | `connectionType` | enum | REQUIRED; BLE == 0 (`DeviceConnectionType.BLE(0)`) |
| 11 | `bleSignalLevel` | **sint32** | RSSI dBm — zig-zag, not plain int (optional) |
| 12 | `wifiSignalLevel` | **sint32** | optional |

`MeaterLinkHeader.deviceID` is `@WireField(#FIXED64, REQUIRED, tag = 5)`. Note
`bleSignalLevel` is `SINT32` (zig-zag): a negative RSSI must be zig-zag encoded,
which this project's `Writer::sint32Field` already does. Also note that in
`MLDevice`, tag 1 (`probe`) is one arm of a `oneof { probe(1), plus(2),
block(3), amber(4) }` (the class constructor enforces at most one non-null); the
bridge only constructs the `probe` arm, which is why the state-packet subset
above lists only `probe = 1`.

**On BLE** (evidence: `data/Config.java`, `p732x5/MEATERPlusBLEConnection.java`):

- Live link RSSI is read from the NimBLE client (`readRssi`), not a characteristic
  — non-destructive.
- `MEATERPlusProbeInfo` is parsed as byte 0 probe type, bytes 1–8 big-endian child probe ID and bytes 9–23 firmware text; this layout was confirmed on the powered MEATER+ base used for validation.
- `MEATERPlusProbeRSSI` uses signed byte 0 in dBm; `127` maps to unavailable (`-128`) and never enters the EMA.
- Company id for advertisement matching: `BLECompanyIDApptionLabs = 891` (0x037B), proven in `Config.java`.

## Not implemented yet

- MEATER Cloud authentication/upload
- Full Block/Pro XL network-settings protocol

## BLE temperature units

The app normalizes all temperatures to signed 1/32 °C integers before placing them in `CookStatus`.

- G1 payload: 1/16 °C values; bridge multiplies by two.
- G2 payload: five internal and one ambient signed 1/32 °C values.
