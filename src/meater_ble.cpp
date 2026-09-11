#include "meater_ble.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

#include "bridge_config.h"
#include "meater_probe_gatt.h"
#include "protobuf_wire.h"
#include "physical_history.h"

namespace {

MeaterBle* gMeaterBleInstance = nullptr;

constexpr const char* kTemperatureServices[] = {
    "a75cc7fc-c956-488f-ac2a-2dbc08b63a04",  // Original / MEATER+
    "49141a23-307f-4e25-ad82-0a3f00d8b90b",  // MEATER SE
    "c9e2746c-59f1-4e54-a0dd-e1e54555cf8b",  // MEATER+ V2
    "dcbb67ca-64fb-41a3-99d1-5d9fd8cf33ca",  // G2 temperature service
};
constexpr bool kServiceUsesG2[] = {false, false, false, true};
constexpr const char* kTemperatureCharacteristic = "7edda774-045e-4bbf-909b-45d1991a2876";
constexpr const char* kBatteryCharacteristic = "2adb4877-68d8-4884-bd3c-d83853bf27b8";
constexpr const char* kPlusBatteryCharacteristic = "22db81c4-d125-4e8f-99a4-3609e4c9a017";
constexpr const char* kProbeRssiCharacteristic = "370aabe7-4837-4bee-aadc-cd1836dbce53";
constexpr const char* kBaseTemperatureCharacteristic = "bb9b2404-fcfb-4b73-8acd-b1b08da3749d";
constexpr const char* kProbeInfoCharacteristic = "1cbff55e-9a06-4721-a178-1e2d84246dd1";
constexpr const char* kProbeStateCharacteristic = "e03c6ccc-2aa7-40a4-8a66-c98b599b737a";
constexpr const char* kTemperatureLogModeCharacteristic = "575d3bf1-2757-45ad-94d9-875c2f6120d3";
constexpr const char* kDeviceInfoService = "180a";
constexpr const char* kFirmwareCharacteristic = "2a26";

int32_t signed12(uint8_t low, uint8_t high) {
    int32_t value = static_cast<int32_t>(low) | (static_cast<int32_t>(high) << 8);
    if (value >= 2048) {
        value |= ~0x0fff;
    }
    return value;
}

int16_t littleInt16(const uint8_t* data) {
    return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                (static_cast<uint16_t>(data[1]) << 8));
}

uint64_t littleUint64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (8U * i);
    }
    return value;
}

void temperatureNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleTemperature(data, length);
    }
}

void batteryNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleBattery(data, length);
    }
}

void baseBatteryNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleBaseBattery(data, length);
    }
}

void probeRssiNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleProbeRssi(data, length);
    }
}

void baseTemperatureNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleBaseTemperature(data, length);
    }
}

void probeInfoNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleProbeInfo(data, length);
    }
}

void probeStateNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (gMeaterBleInstance != nullptr) {
        gMeaterBleInstance->handleProbeState(data, length);
    }
}

}  // namespace

class MeaterScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit MeaterScanCallbacks(MeaterBle* owner) : owner_(owner) {}

    void onResult(const NimBLEAdvertisedDevice* device) override { owner_->considerDevice(device); }

    void onScanEnd(const NimBLEScanResults&, int) override {
        const mb::ConnState state = owner_->supervisor_.state();
        if (state != mb::ConnState::Scanning) return;
        if (owner_->pendingDevice_ != nullptr) {
            owner_->supervisor_.onEvent(mb::ConnEvent::DeviceFound, millis());
            return;
        }
        if (owner_->client_ == nullptr || !owner_->client_->isConnected()) {
            owner_->applyAction(owner_->supervisor_.onEvent(mb::ConnEvent::ScanTimeout,
                                                            millis()));
        }
    }

private:
    MeaterBle* owner_;
};

class MeaterClientCallbacks : public NimBLEClientCallbacks {
public:
    explicit MeaterClientCallbacks(MeaterBle* owner) : owner_(owner) {}

    void onConnect(NimBLEClient*) override { Serial.println("MEATER BLE connected"); }

    void onDisconnect(NimBLEClient*, int reason) override { owner_->handleDisconnect(reason); }

private:
    MeaterBle* owner_;
};

void MeaterBle::begin(const char* targetMac, bool preferBase, bool probeWritesEnabled) {
    targetMac_ = targetMac == nullptr ? "" : targetMac;
    preferBase_ = preferBase;
    runtimeProbeWrites_ = probeWritesEnabled && meater_gatt::writesCompiledIn();
    sampleMutex_ = xSemaphoreCreateMutex();
    gMeaterBleInstance = this;
    NimBLEDevice::init("MEATER-Bridge");
    NimBLEDevice::setPower(3);

    // [item 1] Configure the reliability supervisor from compile-time tunables
    // and mirror MB_PREFER_BASE into its owner preference.
    mb::ConnTiming ct;
    ct.scanWindowMs = MB_BLE_SCAN_WINDOW_MS;
    ct.maxScansBeforeBackoff = MB_BLE_MAX_SCANS_BEFORE_BACKOFF;
    ct.connectWatchdogMs = MB_BLE_CONNECT_WATCHDOG_MS;
    ct.linkWatchdogMs = MB_BLE_NO_TEMP_WATCHDOG_MS;
    ct.backoffBaseMs = MB_BLE_BACKOFF_BASE_MS;
    ct.backoffMaxMs = MB_BLE_BACKOFF_MAX_MS;
    ct.jitterMs = MB_BLE_BACKOFF_JITTER_MS;
    mb::SupervisorTiming st;
    st.scanWindowMs = MB_BLE_SCAN_WINDOW_MS;
    st.maxScansBeforeBackoff = MB_BLE_MAX_SCANS_BEFORE_BACKOFF;
    st.connectWatchdogMs = MB_BLE_CONNECT_WATCHDOG_MS;
    st.noTemperatureMs = MB_BLE_NO_TEMP_WATCHDOG_MS;
    st.rapidDropMs = MB_BLE_RAPID_DROP_MS;
    st.rapidDropsForCooldown = MB_BLE_RAPID_DROPS_FOR_COOLDOWN;
    st.baseCooldownMs = MB_BLE_BASE_COOLDOWN_MS;
    supervisor_ = mb::BleSupervisor(ct, st);
    supervisor_.setPreference(preferBase_ ? mb::OwnerPreference::Base
                                           : mb::OwnerPreference::DirectProbe);
    supervisor_.reset(millis());

    scanCallbacks_ = new MeaterScanCallbacks(this);
    clientCallbacks_ = new MeaterClientCallbacks(this);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(scanCallbacks_, false);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->setActiveScan(true);
    // The supervisor drives the first scan from loop()'s Idle handling; kick it
    // off now so behaviour is unchanged if loop() has not yet run.
    supervisor_.onEvent(mb::ConnEvent::StartScan, millis());
    startScan();
}

void MeaterBle::startScan() {
    if (NimBLEDevice::getScan()->isScanning()) {
        return;
    }
    Serial.println("Scanning for MEATER BLE services");
    NimBLEDevice::getScan()->start(MB_BLE_SCAN_WINDOW_MS, false, true);
}

bool MeaterBle::isBaseType(uint8_t probeNumber) const {
    switch (probeNumber) {
        case 80:   // Second-generation Thermomix Plus
        case 112:  // Second-generation Plus
        case 113:  // Second-generation Plus Pro
        case 128:  // MEATER+
        case 129:  // MEATER SE charger
        case 144:  // Second-generation Traeger Plus
            return true;
        default:
            return false;
    }
}

bool MeaterBle::matchesConfiguredMac(const NimBLEAdvertisedDevice* device) const {
    const String& configured = targetMac_;
    if (configured.length() == 0) {
        return true;
    }
    return configured.equalsIgnoreCase(device->getAddress().toString().c_str());
}

bool MeaterBle::extractIdentity(const NimBLEAdvertisedDevice* device, uint64_t& deviceId,
                                uint8_t& probeNumber) const {
    std::string manufacturer = device->getManufacturerData();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(manufacturer.data());
    size_t length = manufacturer.size();

    // NimBLE normally includes the two-byte company ID; Android's API strips it.
    if (length >= 2 && bytes[0] == 0x7b && bytes[1] == 0x03) {
        bytes += 2;
        length -= 2;
    }

    if (length >= 9) {
        probeNumber = bytes[0];
        deviceId = littleUint64(bytes + 1);
    } else if (length >= 8) {
        probeNumber = 0;
        deviceId = littleUint64(bytes);
    } else {
        deviceId = 0;
    }

    if (deviceId == 0) {
        // Stable fallback derived from the BLE address.
        const std::string address = device->getAddress().toString();
        uint64_t fallback = 0;
        unsigned count = 0;
        for (char ch : address) {
            if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                continue;
            }
            fallback = (fallback << 4U) | static_cast<uint64_t>(std::isdigit(ch) ? ch - '0' :
                std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10);
            if (++count == 12) {
                break;
            }
        }
        deviceId = fallback;
    }
    return deviceId != 0;
}

void MeaterBle::considerDevice(const NimBLEAdvertisedDevice* device) {
    if ((client_ != nullptr && client_->isConnected()) || !matchesConfiguredMac(device)) {
        return;
    }

    bool supported = false;
    for (const char* service : kTemperatureServices) {
        if (device->isAdvertisingService(NimBLEUUID(service))) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        return;
    }

    uint64_t deviceId = 0;
    uint8_t probeNumber = 0;
    if (!extractIdentity(device, deviceId, probeNumber)) {
        return;
    }

    const bool throughBase = isBaseType(probeNumber);
    const bool explicitTarget = !targetMac_.isEmpty();

    // [item 1] Anti-thrash: when the base path is in cooldown (Android has been
    // holding the single central slot and dropping us), ignore base candidates
    // for the cooldown window and let a direct probe be chosen instead. An
    // explicit configured MAC always overrides the cooldown.
    if (throughBase && !explicitTarget &&
        millis() < supervisor_.ownerPolicy().baseCooldownUntilMs) {
        return;
    }

    rotation_.observe(deviceId, throughBase, device->getRSSI(), millis());

    if (!explicitTarget) {
        const auto* desired = rotation_.select(preferBase_, millis(),
                                                MB_BLE_SCAN_WINDOW_MS);
        if (desired == nullptr || desired->endpointId != deviceId) return;
    }

    mb::BleCandidate cand;
    cand.valid = true;
    cand.isBase = throughBase;
    cand.deviceId = deviceId;
    cand.rssi = device->getRSSI();
    supervisor_.selectCandidate(cand);

    pendingDevice_ = device;
    pendingThroughBase_ = throughBase;
    pendingEndpointId_ = deviceId;
    pendingProbeNumber_ = probeNumber;
    pendingRssi_ = static_cast<int16_t>(device->getRSSI());
    const std::string address = device->getAddress().toString();
    std::strncpy(pendingAddress_, address.c_str(), sizeof(pendingAddress_) - 1);
    pendingAddress_[sizeof(pendingAddress_) - 1] = '\0';
    log_i("MEATER candidate: type=%u path=%s RSSI=%d dBm", probeNumber,
          throughBase ? "charger/base" : "direct probe", device->getRSSI());

    if (explicitTarget) {
        supervisor_.onEvent(mb::ConnEvent::DeviceFound, millis());
        NimBLEDevice::getScan()->stop();
    }
}

bool MeaterBle::connectPending() {
    if (pendingDevice_ == nullptr) {
        return false;
    }

    if (client_ == nullptr) {
        client_ = NimBLEDevice::createClient();
        client_->setClientCallbacks(clientCallbacks_, false);
        client_->setConnectionParams(24, 40, 0, 400);
        client_->setConnectTimeout(10000);
    }

    const NimBLEAdvertisedDevice* target = pendingDevice_;
    const uint64_t targetEndpointId = pendingEndpointId_;
    const uint8_t targetProbeNumber = pendingProbeNumber_;
    const int16_t targetRssi = pendingRssi_;
    char targetAddress[18];
    std::strncpy(targetAddress, pendingAddress_, sizeof(targetAddress) - 1);
    targetAddress[sizeof(targetAddress) - 1] = '\0';
    throughBase_ = pendingThroughBase_;
    pendingDevice_ = nullptr;
    pendingThroughBase_ = false;
    pendingEndpointId_ = 0;
    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_ = MeaterSample{};
        sample_.rssi = targetRssi;
        if (throughBase_) {
            sample_.baseDeviceId = targetEndpointId;
            sample_.baseType = targetProbeNumber;
            std::strncpy(sample_.baseAddress, targetAddress, sizeof(sample_.baseAddress) - 1);
        } else {
            sample_.deviceId = targetEndpointId;
            sample_.probeNumber = targetProbeNumber;
        }
        xSemaphoreGive(sampleMutex_);
    }
    if (!client_->connect(target)) {
        Serial.println("MEATER BLE connection failed");
        // [item 1] A failed connect is a drop: let the supervisor account for
        // it (anti-thrash) and schedule backoff instead of re-scanning
        // immediately, which would thrash the radio.
        supervisor_.onEvent(mb::ConnEvent::Watchdog, millis());
        releaseLink();
        return false;
    }

    NimBLERemoteService* temperatureService = nullptr;
    g2Payload_ = false;
    constexpr size_t serviceCount = sizeof(kTemperatureServices) / sizeof(kTemperatureServices[0]);
    for (size_t i = 0; i < serviceCount; ++i) {
        temperatureService = client_->getService(kTemperatureServices[i]);
        if (temperatureService != nullptr) {
            g2Payload_ = kServiceUsesG2[i];
            Serial.printf("Using MEATER temperature service %s (%s decoder)\n",
                          kTemperatureServices[i], g2Payload_ ? "G2" : "G1");
            break;
        }
    }
    if (temperatureService == nullptr) {
        Serial.println("No supported MEATER temperature service found");
        client_->disconnect();
        return false;
    }

    probeInfoCharacteristic_ = nullptr;
    probeStateCharacteristic_ = nullptr;
    baseBatteryCharacteristic_ = nullptr;
    probeRssiCharacteristic_ = nullptr;
    baseTemperatureCharacteristic_ = nullptr;
    temperatureLogCharacteristic_ = nullptr;
    temperatureLogModeCharacteristic_ = nullptr;
    cookSetupCharacteristic_ = nullptr;
    if (throughBase_) {
        probeStateCharacteristic_ = temperatureService->getCharacteristic(kProbeStateCharacteristic);
        probeInfoCharacteristic_ = temperatureService->getCharacteristic(kProbeInfoCharacteristic);
        if (probeStateCharacteristic_ == nullptr || probeInfoCharacteristic_ == nullptr) {
            log_e("MEATER charger/base is missing state or child-probe info characteristics");
            client_->disconnect();
            return false;
        }
        if (probeStateCharacteristic_->canNotify()) {
            probeStateCharacteristic_->subscribe(true, probeStateNotify);
        }
        if (probeStateCharacteristic_->canRead()) {
            const NimBLEAttValue value = probeStateCharacteristic_->readValue();
            handleProbeState(value.data(), value.size());
        }
        if (probeInfoCharacteristic_->canNotify()) {
            probeInfoCharacteristic_->subscribe(true, probeInfoNotify);
        }
        if (probeInfoCharacteristic_->canRead()) {
            const NimBLEAttValue value = probeInfoCharacteristic_->readValue();
            handleProbeInfo(value.data(), value.size());
        }

        baseBatteryCharacteristic_ = temperatureService->getCharacteristic(kPlusBatteryCharacteristic);
        if (baseBatteryCharacteristic_ != nullptr) {
            if (baseBatteryCharacteristic_->canNotify()) {
                baseBatteryCharacteristic_->subscribe(true, baseBatteryNotify);
            }
            if (baseBatteryCharacteristic_->canRead()) {
                const NimBLEAttValue value = baseBatteryCharacteristic_->readValue();
                handleBaseBattery(value.data(), value.size());
            }
        }

        probeRssiCharacteristic_ = temperatureService->getCharacteristic(kProbeRssiCharacteristic);
        if (probeRssiCharacteristic_ != nullptr) {
            if (probeRssiCharacteristic_->canNotify()) {
                probeRssiCharacteristic_->subscribe(true, probeRssiNotify);
            }
            if (probeRssiCharacteristic_->canRead()) {
                const NimBLEAttValue value = probeRssiCharacteristic_->readValue();
                handleProbeRssi(value.data(), value.size());
            }
        }

        baseTemperatureCharacteristic_ =
            temperatureService->getCharacteristic(kBaseTemperatureCharacteristic);
        if (baseTemperatureCharacteristic_ != nullptr) {
            if (baseTemperatureCharacteristic_->canNotify()) {
                baseTemperatureCharacteristic_->subscribe(true, baseTemperatureNotify);
            }
            if (baseTemperatureCharacteristic_->canRead()) {
                const NimBLEAttValue value = baseTemperatureCharacteristic_->readValue();
                handleBaseTemperature(value.data(), value.size());
            }
        }
    }

    temperatureCharacteristic_ =
        temperatureService->getCharacteristic(kTemperatureCharacteristic);
    if (temperatureCharacteristic_ == nullptr) {
        Serial.println("MEATER temperature characteristic not found");
        client_->disconnect();
        return false;
    }

    temperatureLogCharacteristic_ =
        temperatureService->getCharacteristic(meater_gatt::kChrTemperatureLog);
    temperatureLogModeCharacteristic_ =
        temperatureService->getCharacteristic(meater_gatt::kChrTemperatureLogMode);
    cookSetupCharacteristic_ =
        temperatureService->getCharacteristic(meater_gatt::kChrCookSetup);
    if (temperatureLogCharacteristic_ != nullptr && temperatureLogCharacteristic_->canRead()) {
        const NimBLEAttValue history = temperatureLogCharacteristic_->readValue();
        handleTemperatureLog(history.data(), history.size());
        refreshHistory_ = false;
    }

    batteryCharacteristic_ = temperatureService->getCharacteristic(kBatteryCharacteristic);
    if (batteryCharacteristic_ == nullptr && !throughBase_) {
        batteryCharacteristic_ = temperatureService->getCharacteristic(kPlusBatteryCharacteristic);
    }

    if (temperatureCharacteristic_->canNotify() &&
        !temperatureCharacteristic_->subscribe(true, temperatureNotify)) {
        Serial.println("Failed to subscribe to MEATER temperature notifications");
        client_->disconnect();
        return false;
    }

    if (temperatureLogModeCharacteristic_ != nullptr &&
        temperatureLogModeCharacteristic_->canWrite()) {
        const uint8_t fastInterval = 0xc1;
        temperatureLogModeCharacteristic_->writeValue(&fastInterval, 1, true);
    }

    if (temperatureCharacteristic_->canRead()) {
        const NimBLEAttValue value = temperatureCharacteristic_->readValue();
        handleTemperature(value.data(), value.size());
    }

    if (batteryCharacteristic_ != nullptr) {
        if (batteryCharacteristic_->canNotify()) {
            batteryCharacteristic_->subscribe(true, batteryNotify);
        }
        if (batteryCharacteristic_->canRead()) {
            const NimBLEAttValue value = batteryCharacteristic_->readValue();
            handleBattery(value.data(), value.size());
        }
    }

    NimBLERemoteService* infoService = client_->getService(kDeviceInfoService);
    if (infoService != nullptr) {
        NimBLERemoteCharacteristic* firmware =
            infoService->getCharacteristic(kFirmwareCharacteristic);
        if (firmware != nullptr && firmware->canRead()) {
            const NimBLEAttValue value = firmware->readValue();
            if (sampleMutex_ != nullptr &&
                xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
                char* destination = throughBase_ ? sample_.baseFirmware : sample_.firmware;
                const size_t capacity = throughBase_ ? sizeof(sample_.baseFirmware)
                                                     : sizeof(sample_.firmware);
                const size_t copy =
                    std::min<size_t>(static_cast<size_t>(value.size()), capacity - 1);
                std::memcpy(destination, value.data(), copy);
                destination[copy] = '\0';
                xSemaphoreGive(sampleMutex_);
            }
        }
    }

    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.bleConnected = true;
        sample_.rssi = client_->getRssi();
        xSemaphoreGive(sampleMutex_);
    }
    // [item 1] Link is up: advance the supervisor to Connected, which arms the
    // no-temperature watchdog and remembers the identity we are serving so the
    // downstream logical device is stable across a reconnect (handoff).
    supervisor_.onEvent(mb::ConnEvent::Connected, millis());
    supervisorConnected_ = true;
    currentEndpointId_ = targetEndpointId;
    connectedSinceMs_ = millis();
    rotationRequested_ = false;
    rotation_.markServed(targetEndpointId, connectedSinceMs_);
    lastTemperatureMs_ = connectedSinceMs_;
    supervisor_.noteTemperature(lastTemperatureMs_);
    return true;
}

void MeaterBle::handleProbeState(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 1) {
        return;
    }
    const uint8_t state = data[0];
    const char* label = "unknown";
    switch (state) {
        case 0: label = "connected"; break;
        case 1: label = "disconnected"; break;
        case 2: label = "not paired"; break;
        case 3: label = "fetching cook setup"; break;
        case 4: label = "clearing history"; break;
        case 7: label = "probe battery shutdown"; break;
        case 8: label = "probe in charger"; break;
        case 9: label = "probe upside down"; break;
        case 10: label = "probe sleep (no probe)"; break;
        case 11: label = "probe sleep (no cook)"; break;
        default: break;
    }
    log_i("MEATER charger child state: %s (%u)", label, state);
    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.childState = state;
        xSemaphoreGive(sampleMutex_);
    }
    if (state == 0 || state == 3 || state == 4 || state == 11) {
        refreshProbeInfo_ = true;
        refreshHistory_ = true;
    }
}

void MeaterBle::handleProbeInfo(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 24 || sampleMutex_ == nullptr) {
        log_e("Invalid MEATER charger child-probe info (length=%u)",
              static_cast<unsigned>(length));
        return;
    }

    const uint8_t probeNumber = data[0];
    uint64_t probeId = 0;
    for (size_t i = 1; i <= 8; ++i) {
        probeId = (probeId << 8U) | data[i];
    }
    if (probeId == 0) {
        log_i("MEATER charger has no active child probe yet");
        return;
    }

    char firmware[16]{};
    std::memcpy(firmware, data + 9, 15);
    for (int i = 14; i >= 0; --i) {
        if (firmware[i] == '\0' || std::isspace(static_cast<unsigned char>(firmware[i]))) {
            firmware[i] = '\0';
        } else {
            break;
        }
    }

    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.deviceId = probeId;
        sample_.probeNumber = probeNumber;
        std::strncpy(sample_.firmware, firmware, sizeof(sample_.firmware) - 1);
        sample_.firmware[sizeof(sample_.firmware) - 1] = '\0';
        xSemaphoreGive(sampleMutex_);
    }
    log_i("MEATER charger child probe ready: id=%016llx type=%u firmware=%s",
          static_cast<unsigned long long>(probeId), probeNumber, firmware);
}

void MeaterBle::handleTemperature(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 6 || sampleMutex_ == nullptr) {
        return;
    }
#if MB_LOG_RAW_BLE
    Serial.print("MEATER temp raw:");
    for (size_t i = 0; i < length; ++i) {
        Serial.printf(" %02x", data[i]);
    }
    Serial.println();
#endif

    MeaterSample next = sample();
    if (g2Payload_ || length >= 12) {
        if (length < 12) {
            return;
        }
        next.internalCount = 5;
        int32_t lowest = INT32_MAX;
        for (uint8_t i = 0; i < 5; ++i) {
            next.internalsRaw32[i] = littleInt16(data + i * 2);
            lowest = std::min(lowest, next.internalsRaw32[i]);
        }
        const int32_t rawAmbient = littleInt16(data + 10);
        const int32_t reference = next.internalsRaw32[4];
        next.ambientRaw32 = static_cast<int32_t>(reference + (rawAmbient - reference) * 1.2f);
        next.internalRaw32 = lowest;
    } else {
        const int32_t internal16 = signed12(data[0], data[1]);
        const int32_t ambient16 = signed12(data[2], data[3]);
        const int32_t ambientOffset16 = signed12(data[4], data[5]);
        const int32_t correction = std::max<int32_t>(
            0, static_cast<int32_t>(((ambient16 - std::min<int32_t>(48, ambientOffset16)) *
                                      9424.0f) /
                                     1487.0f));
        next.internalCount = 1;
        next.internalRaw32 = internal16 * 2;
        next.internalsRaw32[0] = next.internalRaw32;
        next.ambientRaw32 = std::max<int32_t>(0, internal16 + correction) * 2;
    }

    next.valid = true;
    next.bleConnected = true;
    next.updatedMs = millis();
    if (next.peakRaw32 == MB_INVALID_TEMP_RAW32 || next.internalRaw32 > next.peakRaw32) {
        next.peakRaw32 = next.internalRaw32;
    }

    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_ = next;
        xSemaphoreGive(sampleMutex_);
    }
    // [item 1] A real temperature arrived: pet the no-temperature watchdog so a
    // live link is not torn down. Recorded here (not on any notify) so a silent
    // link that only emits battery/RSSI still trips the watchdog.
    lastTemperatureMs_ = millis();
    supervisor_.noteTemperature(lastTemperatureMs_);
    Serial.printf("MEATER %.2f C / ambient %.2f C / battery %u%%\n", next.internalC(),
                  next.ambientC(), next.batteryPercent);
}

void MeaterBle::handleBattery(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0 || sampleMutex_ == nullptr) {
        return;
    }
    int battery = 0;
    if (length == 3 || length == 5 || length == 1) {
        battery = data[0];
    } else if (length >= 2) {
        battery = static_cast<int>(data[1]) * 256 + static_cast<int>(data[0]) * 10;
    }
    battery = std::max(0, std::min(battery, 100));
    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.batteryPercent = static_cast<uint8_t>(battery);
        xSemaphoreGive(sampleMutex_);
    }
}

void MeaterBle::handleBaseBattery(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 1 || sampleMutex_ == nullptr) {
        return;
    }
    const uint8_t battery = std::min<uint8_t>(data[0], 100);
    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.baseBatteryPercent = battery;
        sample_.baseBatteryValid = true;
        xSemaphoreGive(sampleMutex_);
    }
}

void MeaterBle::handleProbeRssi(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 1 || sampleMutex_ == nullptr) {
        return;
    }
    int8_t rssi = static_cast<int8_t>(data[0]);
    if (rssi == 127) {
        rssi = -128;
    }
    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.probeRssi = rssi;
        sample_.probeRssiValid = rssi != -128;
        xSemaphoreGive(sampleMutex_);
    }
}

void MeaterBle::handleBaseTemperature(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 2 || sampleMutex_ == nullptr) {
        return;
    }
    int32_t raw32 = littleInt16(data);
    if (!g2Payload_) {
        raw32 *= 2;
    }
    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.baseTemperatureRaw32 = raw32;
        xSemaphoreGive(sampleMutex_);
    }
}

void MeaterBle::handleTemperatureLog(const uint8_t* data, size_t length) {
    if (sampleMutex_ == nullptr) return;
    mb::PhysicalHistory parsed;
    if (!mb::parsePhysicalHistory(data, length, g2Payload_, parsed)) return;

    MeaterSample next = sample();
    next.historyCount = parsed.count;
    next.historyIntervalSeconds = parsed.intervalSeconds;
    next.historyElapsedSeconds = parsed.elapsedSeconds;
    next.historyCapturedMs = millis();
    for (uint16_t i = 0; i < parsed.count; ++i) {
        next.historyInternalRaw32[i] = parsed.internalRaw32[i];
        next.historyAmbientRaw32[i] = parsed.ambientRaw32[i];
        if (next.peakRaw32 == MB_INVALID_TEMP_RAW32 ||
            parsed.internalRaw32[i] > next.peakRaw32) {
            next.peakRaw32 = parsed.internalRaw32[i];
        }
    }
    if (xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_ = next;
        xSemaphoreGive(sampleMutex_);
    }
    log_i("MEATER history: %u readings x %us; recovered elapsed=%us", parsed.count,
          parsed.intervalSeconds, parsed.elapsedSeconds);
}

void MeaterBle::updateRssi(int rssi) {
    if (rssi == 0) {
        return;
    }
    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.rssi = static_cast<int16_t>(rssi);
        xSemaphoreGive(sampleMutex_);
    }
}

void MeaterBle::handleDisconnect(int reason) {
    Serial.printf("MEATER BLE disconnected, reason=%d\n", reason);
    temperatureCharacteristic_ = nullptr;
    batteryCharacteristic_ = nullptr;
    baseBatteryCharacteristic_ = nullptr;
    probeRssiCharacteristic_ = nullptr;
    baseTemperatureCharacteristic_ = nullptr;
    temperatureLogCharacteristic_ = nullptr;
    temperatureLogModeCharacteristic_ = nullptr;
    cookSetupCharacteristic_ = nullptr;
    probeInfoCharacteristic_ = nullptr;
    probeStateCharacteristic_ = nullptr;
    refreshProbeInfo_ = false;
    refreshHistory_ = false;
    throughBase_ = false;
    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        sample_.bleConnected = false;
        sample_.probeRssiValid = false;
        sample_.childState = 1;
        sample_.updatedMs = millis();
        xSemaphoreGive(sampleMutex_);
    }
    // [item 1] Do NOT rescan from the NimBLE callback context. Flag the drop and
    // let loop() feed the supervisor, run the half-open teardown, and apply
    // bounded backoff + jitter before the next scan. This is what replaces the
    // old immediate delay(250)+startScan() thrash.
    lastDisconnectReason_ = reason;
    disconnectPending_ = true;
}

void MeaterBle::stopScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan != nullptr && scan->isScanning()) {
        scan->stop();
    }
}

// [item 1] Half-open teardown: fully release the NimBLE client so a stale
// half-open connection cannot wedge the owner. Idempotent. After the client is
// released we report TeardownComplete to the supervisor, which moves it into
// Backoff and hands back the computed delay.
void MeaterBle::releaseLink() {
    stopScan();
    pendingDevice_ = nullptr;
    pendingThroughBase_ = false;
    pendingEndpointId_ = 0;
    pendingProbeNumber_ = 0;
    pendingRssi_ = -127;
    pendingAddress_[0] = '\0';
    currentEndpointId_ = 0;
    connectedSinceMs_ = 0;
    rotationRequested_ = false;
    temperatureCharacteristic_ = nullptr;
    batteryCharacteristic_ = nullptr;
    baseBatteryCharacteristic_ = nullptr;
    probeRssiCharacteristic_ = nullptr;
    baseTemperatureCharacteristic_ = nullptr;
    temperatureLogCharacteristic_ = nullptr;
    temperatureLogModeCharacteristic_ = nullptr;
    cookSetupCharacteristic_ = nullptr;
    probeInfoCharacteristic_ = nullptr;
    probeStateCharacteristic_ = nullptr;
    refreshProbeInfo_ = false;
    refreshHistory_ = false;
    throughBase_ = false;
    supervisorConnected_ = false;
    if (client_ != nullptr) {
        if (client_->isConnected()) {
            client_->disconnect();
        }
        NimBLEDevice::deleteClient(client_);
        client_ = nullptr;
    }
    // Teardown is complete -> supervisor computes backoff and enters Backoff.
    const mb::SupervisorAction action = supervisor_.onEvent(mb::ConnEvent::TeardownComplete,
                                                            millis());
    applyAction(action);
}

// [item 1] Map a SupervisorAction onto NimBLE side effects. Connect/Teardown
// are driven inline elsewhere; here we handle StartScan (begin a scan pass) and
// EnterBackoff (log the wait; loop() gates the next scan on the timer).
void MeaterBle::applyAction(const mb::SupervisorAction& action) {
    switch (action.directive) {
        case mb::SupervisorDirective::StartScan:
            startScan();
            break;
        case mb::SupervisorDirective::EnterBackoff:
            log_i("BLE backoff %lu ms before next scan (attempt=%lu)",
                  static_cast<unsigned long>(action.backoffMs),
                  static_cast<unsigned long>(supervisor_.attempt()));
            break;
        case mb::SupervisorDirective::Teardown:
            releaseLink();
            break;
        case mb::SupervisorDirective::Connect:
        case mb::SupervisorDirective::Wait:
        default:
            break;
    }
}

// [item 1] Run the supervisor's time-based watchdogs and backoff each loop, and
// process a deferred disconnect. This is the single place NimBLE state is
// reconciled with the supervisor's decisions.
void MeaterBle::serviceSupervisor() {
    const uint32_t now = millis();

    // Deferred disconnect from the NimBLE callback: feed the drop, then tear
    // down and enter backoff.
    if (disconnectPending_) {
        disconnectPending_ = false;
        supervisor_.onEvent(mb::ConnEvent::Disconnected, now);
        releaseLink();
        return;
    }

    // A scan-end callback owns candidate commitment and empty-scan accounting.
    // Polling while NimBLE is still scanning would commit the first sighting and
    // prevent the rotation selector from considering the full scan window.
    if (supervisor_.state() == mb::ConnState::Scanning &&
        NimBLEDevice::getScan()->isScanning()) {
        return;
    }

    // Poll watchdogs/backoff. The supervisor tells us what to do next.
    const mb::SupervisorAction action = supervisor_.poll(now);
    switch (action.directive) {
        case mb::SupervisorDirective::Teardown:
            // A watchdog fired (stuck connect or silent link). Release now.
            releaseLink();
            break;
        case mb::SupervisorDirective::StartScan:
            if (client_ == nullptr || !client_->isConnected()) {
                startScan();
            }
            break;
        case mb::SupervisorDirective::Connect:
        case mb::SupervisorDirective::EnterBackoff:
        case mb::SupervisorDirective::Wait:
        default:
            break;
    }
}

bool MeaterBle::applyCookSetup(const CookControl& cook) {
#if MB_PROBE_WRITE_ENABLED
    if (!runtimeProbeWrites_ || client_ == nullptr || !client_->isConnected() ||
        cookSetupCharacteristic_ == nullptr || temperatureLogModeCharacteristic_ == nullptr ||
        !cookSetupCharacteristic_->canWrite() || !temperatureLogModeCharacteristic_->canWrite()) {
        return false;
    }
    const std::vector<uint8_t> payload = meater_gatt::buildCookSetupPayload(cook);

    const uint8_t resetLog = 0;  // TemperatureLogState.RESET, proven enum value.
    if (!temperatureLogModeCharacteristic_->writeValue(&resetLog, 1, true)) return false;
    if (!cookSetupCharacteristic_->writeValue(payload.data(), payload.size(), true)) return false;
    refreshHistory_ = true;
    log_w("Experimental physical cook setup written (sequence=%u)", cook.sequenceNumber);
    return true;
#else
    (void)cook;
    return false;
#endif
}

MeaterSample MeaterBle::sample() const {
    MeaterSample copy;
    if (sampleMutex_ != nullptr && xSemaphoreTake(sampleMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = sample_;
        xSemaphoreGive(sampleMutex_);
    }
    return copy;
}

void MeaterBle::loop() {
    if (pendingDevice_ != nullptr && !NimBLEDevice::getScan()->isScanning()) {
        connectPending();
    }
    if (refreshProbeInfo_ && client_ != nullptr && client_->isConnected() &&
        probeInfoCharacteristic_ != nullptr && probeInfoCharacteristic_->canRead()) {
        refreshProbeInfo_ = false;
        const NimBLEAttValue value = probeInfoCharacteristic_->readValue();
        handleProbeInfo(value.data(), value.size());
    }
    if (refreshHistory_ && client_ != nullptr && client_->isConnected() &&
        temperatureLogCharacteristic_ != nullptr && temperatureLogCharacteristic_->canRead()) {
        refreshHistory_ = false;
        const NimBLEAttValue value = temperatureLogCharacteristic_->readValue();
        handleTemperatureLog(value.data(), value.size());
    }
    if (client_ != nullptr && client_->isConnected() && millis() - lastRssiMs_ > 5000) {
        updateRssi(client_->getRssi());
        lastRssiMs_ = millis();
    }
#if MB_MULTI_DEVICE_ENABLED
    const uint32_t now = millis();
    if (!rotationRequested_ && targetMac_.isEmpty() && client_ != nullptr &&
        client_->isConnected() &&
        rotation_.shouldRotate(preferBase_, connectedSinceMs_, now,
                               MB_MULTI_DEVICE_DWELL_MS)) {
        rotationRequested_ = true;
        log_i("Rotating BLE ownership after %lu ms from endpoint %016llx",
              static_cast<unsigned long>(now - connectedSinceMs_),
              static_cast<unsigned long long>(currentEndpointId_));
        client_->disconnect();
    }
#endif
    // [item 1] Reconcile NimBLE state with the supervisor: run watchdogs, apply
    // bounded backoff before re-scanning, and process a deferred disconnect.
    // This replaces the old unconditional startScan() fallback, which would
    // rescan immediately and thrash instead of honouring backoff.
    serviceSupervisor();
}
