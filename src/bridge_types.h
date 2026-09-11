#pragma once

#include <array>
#include <cstdint>
#include <cstring>

constexpr int32_t MB_INVALID_TEMP_RAW32 = -1024;

struct MeaterSample {
    bool valid{false};
    bool bleConnected{false};
    uint64_t deviceId{0};
    uint64_t baseDeviceId{0};
    uint8_t probeNumber{0};
    uint8_t baseType{0};
    int32_t internalRaw32{MB_INVALID_TEMP_RAW32};
    int32_t ambientRaw32{MB_INVALID_TEMP_RAW32};
    int32_t peakRaw32{MB_INVALID_TEMP_RAW32};
    int32_t baseTemperatureRaw32{MB_INVALID_TEMP_RAW32};
    std::array<int32_t, 5> internalsRaw32{
        MB_INVALID_TEMP_RAW32, MB_INVALID_TEMP_RAW32, MB_INVALID_TEMP_RAW32,
        MB_INVALID_TEMP_RAW32, MB_INVALID_TEMP_RAW32};
    std::array<int32_t, 120> historyInternalRaw32{};
    std::array<int32_t, 120> historyAmbientRaw32{};
    uint16_t historyCount{0};
    uint16_t historyIntervalSeconds{0};
    uint32_t historyElapsedSeconds{0};
    uint32_t historyCapturedMs{0};
    uint8_t internalCount{0};
    uint8_t batteryPercent{0};
    uint8_t baseBatteryPercent{0};
    bool baseBatteryValid{false};
    int16_t rssi{-127};
    int16_t probeRssi{-127};
    bool probeRssiValid{false};
    int16_t childState{-1};
    uint32_t updatedMs{0};
    char firmware[24]{};
    char baseFirmware[24]{};
    char baseAddress[18]{};

    float internalC() const { return internalRaw32 / 32.0f; }
    float ambientC() const { return ambientRaw32 / 32.0f; }
    float peakC() const { return peakRaw32 / 32.0f; }
    float baseTemperatureC() const { return baseTemperatureRaw32 / 32.0f; }
    bool probeConnected() const {
        return childState < 0 ? bleConnected
                              : childState == 0 || childState == 3 || childState == 4 ||
                                    childState == 11;
    }
};

struct CookControl {
    uint32_t sequenceNumber{0};
    uint32_t state{0};
    int32_t targetInternalRaw32{0};
    uint64_t cookId{0};
    uint32_t startedMs{0};
    char name[33]{};
};
