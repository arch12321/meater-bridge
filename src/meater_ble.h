#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ble_supervisor.h"
#include "bridge_config.h"
#include "bridge_types.h"
#include "device_rotation.h"

class MeaterScanCallbacks;
class MeaterClientCallbacks;

class MeaterBle {
public:
    void begin(const char* targetMac, bool preferBase, bool probeWritesEnabled);
    void loop();
    MeaterSample sample() const;
    bool applyCookSetup(const CookControl& cook);

    // Called by NimBLE's function-pointer notification callbacks.
    void handleTemperature(const uint8_t* data, size_t length);
    void handleBattery(const uint8_t* data, size_t length);
    void handleBaseBattery(const uint8_t* data, size_t length);
    void handleProbeRssi(const uint8_t* data, size_t length);
    void handleBaseTemperature(const uint8_t* data, size_t length);
    void handleTemperatureLog(const uint8_t* data, size_t length);
    void handleProbeInfo(const uint8_t* data, size_t length);
    void handleProbeState(const uint8_t* data, size_t length);

private:
    friend class MeaterScanCallbacks;
    friend class MeaterClientCallbacks;

    void considerDevice(const NimBLEAdvertisedDevice* device);
    bool connectPending();
    void startScan();
    void handleDisconnect(int reason);
    void updateRssi(int rssi);
    bool isBaseType(uint8_t probeNumber) const;
    bool matchesConfiguredMac(const NimBLEAdvertisedDevice* device) const;
    bool extractIdentity(const NimBLEAdvertisedDevice* device, uint64_t& deviceId,
                         uint8_t& probeNumber) const;

    // [item 1] Reliability supervisor helpers. serviceSupervisor() runs the
    // watchdogs/backoff each loop; releaseLink() performs the half-open
    // teardown the supervisor asks for; applyAction() maps a SupervisorAction
    // onto NimBLE side effects.
    void serviceSupervisor();
    void releaseLink();
    void applyAction(const mb::SupervisorAction& action);
    void stopScan();

    mutable SemaphoreHandle_t sampleMutex_{nullptr};
    MeaterSample sample_{};
    const NimBLEAdvertisedDevice* pendingDevice_{nullptr};
    NimBLEClient* client_{nullptr};
    NimBLERemoteCharacteristic* temperatureCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* batteryCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* baseBatteryCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* probeRssiCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* baseTemperatureCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* temperatureLogCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* temperatureLogModeCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* cookSetupCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* probeInfoCharacteristic_{nullptr};
    NimBLERemoteCharacteristic* probeStateCharacteristic_{nullptr};
    MeaterScanCallbacks* scanCallbacks_{nullptr};
    MeaterClientCallbacks* clientCallbacks_{nullptr};
    volatile bool refreshProbeInfo_{false};
    volatile bool refreshHistory_{false};
    bool pendingThroughBase_{false};
    bool throughBase_{false};
    bool g2Payload_{false};
    uint32_t lastRssiMs_{0};

    // [item 1] The ownership/handoff supervisor plus the observations it needs.
    mb::BleSupervisor supervisor_{};
    volatile bool disconnectPending_{false};  // Set from the NimBLE callback.
    int lastDisconnectReason_{0};
    bool supervisorConnected_{false};         // Mirrors the supervisor's view.
    uint32_t lastTemperatureMs_{0};
    String targetMac_;
    bool preferBase_{true};
    bool runtimeProbeWrites_{false};

    mb::DeviceRotationT<MB_MAX_DEVICES * 2> rotation_{};
    uint64_t pendingEndpointId_{0};
    uint64_t currentEndpointId_{0};
    uint8_t pendingProbeNumber_{0};
    int16_t pendingRssi_{-127};
    char pendingAddress_[18]{};
    uint32_t connectedSinceMs_{0};
    bool rotationRequested_{false};
};
