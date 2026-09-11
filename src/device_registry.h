#pragma once

// Multi-device model [item 7].
//
// Fixed-capacity runtime registry for several probes represented as independent
// MQTT/Home Assistant devices. The BLE owner time-slices one physical link and
// this registry preserves stable logical identity/topic derivation between
// visits.
//
// Backward compatibility contract:
//   * The FIRST device registered (slot 0) is the "primary" device and MUST
//     reuse the historical node id (MB_NODE_ID) verbatim so that existing
//     Home Assistant unique_ids ("<node>_<object>") and topics
//     ("<state_prefix>/<node>/state") are preserved byte-for-byte.
//   * Additional devices derive a distinct, stable suffix from their BLE
//     identity so multiple devices never collide.
//
// The registry is fixed-capacity and allocation-free so it is safe on the
// ESP32-C3. Nothing here starts scans, connects, or publishes.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bridge_types.h"

namespace mb {

// Logical role of a tracked BLE endpoint.
enum class DeviceRole : uint8_t {
    Unknown = 0,
    Probe = 1,  // A MEATER probe (direct connection or reported via a base).
    Base = 2,   // A MEATER+/SE charger base acting as a repeater.
};

// A single tracked device. `nodeId` is the HA/MQTT identity fragment; for the
// primary device it equals the configured MB_NODE_ID for backward compat.
struct DeviceIdentity {
    bool inUse{false};
    bool primary{false};
    DeviceRole role{DeviceRole::Unknown};
    uint64_t bleDeviceId{0};   // Stable identity extracted from advertisement.
    uint8_t probeNumber{0};    // MEATER probe/base type byte.
    char nodeId[40]{};         // e.g. "meater_bridge" or "meater_bridge_a1b2c3".
    char bleAddress[18]{};     // "AA:BB:CC:DD:EE:FF", optional.

    bool matches(uint64_t id) const { return inUse && id != 0 && bleDeviceId == id; }
};

// Fixed-capacity registry. Capacity is generous for a home setup while staying
// allocation-free. Slot 0 is reserved for the primary (single-device) case so
// defaults are preserved when only one device is ever seen.
template <size_t Capacity = 6>
class DeviceRegistryT {
public:
    static constexpr size_t capacity() { return Capacity; }

    void begin(const char* primaryNodeId) {
        devices_ = {};
        count_ = 0;
        // Reserve slot 0 as the primary device using the historical node id so
        // the first real device keeps the legacy unique_id/topic scheme.
        DeviceIdentity& primary = devices_[0];
        primary.inUse = true;
        primary.primary = true;
        primary.role = DeviceRole::Probe;
        copyNodeId(primary.nodeId, primaryNodeId);
        count_ = 1;
    }

    size_t size() const { return count_; }

    const DeviceIdentity& primary() const { return devices_[0]; }

    const DeviceIdentity* at(size_t index) const {
        return index < count_ ? &devices_[index] : nullptr;
    }

    size_t indexOf(uint64_t bleDeviceId) const {
        for (size_t i = 0; i < count_; ++i) {
            if (devices_[i].matches(bleDeviceId)) return i;
        }
        return Capacity;
    }

    // Returns the slot for `bleDeviceId`, creating one if absent. The first
    // device bound (slot 0) keeps the primary node id; later devices get a
    // deterministic suffix derived from their identity. Returns nullptr only
    // when the registry is full.
    DeviceIdentity* bind(uint64_t bleDeviceId, DeviceRole role, uint8_t probeNumber,
                         const char* bleAddress) {
        if (bleDeviceId == 0) {
            return nullptr;
        }
        for (size_t i = 0; i < count_; ++i) {
            if (devices_[i].matches(bleDeviceId)) {
                annotate(devices_[i], role, probeNumber, bleAddress);
                return &devices_[i];
            }
        }
        // Prefer filling the reserved primary slot first so single-device
        // deployments keep their legacy identity.
        if (devices_[0].bleDeviceId == 0) {
            devices_[0].bleDeviceId = bleDeviceId;
            annotate(devices_[0], role, probeNumber, bleAddress);
            return &devices_[0];
        }
        if (count_ >= Capacity) {
            return nullptr;
        }
        DeviceIdentity& slot = devices_[count_];
        slot.inUse = true;
        slot.primary = false;
        slot.bleDeviceId = bleDeviceId;
        deriveNodeId(slot.nodeId, devices_[0].nodeId, bleDeviceId);
        annotate(slot, role, probeNumber, bleAddress);
        ++count_;
        return &slot;
    }

    // Derives the HA object's unique_id for a device+object pair. Matches the
    // legacy "<node>_<object>" scheme exactly for the primary device.
    static void uniqueId(char* out, size_t outLen, const DeviceIdentity& device,
                         const char* object) {
        snprintf(out, outLen, "%s_%s", device.nodeId, object);
    }

    // Derives the MQTT state topic for a device: "<statePrefix>/<node>/state".
    // For the primary device nodeId == MB_NODE_ID so this reproduces the
    // legacy single-device topic byte-for-byte; additional devices get a
    // distinct topic under their derived node id.
    static void stateTopic(char* out, size_t outLen, const char* statePrefix,
                           const DeviceIdentity& device) {
        snprintf(out, outLen, "%s/%s/state", statePrefix, device.nodeId);
    }

    // Derives the MQTT availability topic for a device. Backward compatible for
    // the primary device.
    static void availabilityTopic(char* out, size_t outLen, const char* statePrefix,
                                  const DeviceIdentity& device) {
        snprintf(out, outLen, "%s/%s/availability", statePrefix, device.nodeId);
    }

    // How many additional (non-primary) devices may still be admitted before
    // the registry is full. Selection behaviour: the primary slot is always
    // filled first (single-device backward compat); once capacity is reached
    // further distinct devices are REJECTED by bind() returning nullptr, and
    // the caller keeps serving the already-admitted devices rather than
    // evicting one. This is deterministic and avoids identity churn under the
    // ESP32-C3 connection/memory ceiling.
    size_t remainingCapacity() const { return Capacity - count_; }
    bool full() const { return count_ >= Capacity; }

private:
    static void copyNodeId(char* dst, const char* src) {
        if (src == nullptr) {
            dst[0] = '\0';
            return;
        }
        std::strncpy(dst, src, 39);
        dst[39] = '\0';
    }

    static void deriveNodeId(char* dst, const char* primaryNodeId, uint64_t bleDeviceId) {
        // Stable, collision-resistant suffix from the low 24 bits of identity.
        snprintf(dst, 40, "%s_%06lx", primaryNodeId,
                 static_cast<unsigned long>(bleDeviceId & 0xFFFFFFUL));
    }

    static void annotate(DeviceIdentity& slot, DeviceRole role, uint8_t probeNumber,
                         const char* bleAddress) {
        if (role != DeviceRole::Unknown) {
            slot.role = role;
        }
        slot.probeNumber = probeNumber;
        if (bleAddress != nullptr && bleAddress[0] != '\0') {
            std::strncpy(slot.bleAddress, bleAddress, 17);
            slot.bleAddress[17] = '\0';
        }
    }

    std::array<DeviceIdentity, Capacity> devices_{};
    size_t count_{0};
};

// Configured maximum number of independently-tracked devices. This is the
// single source of truth for the multi-device ceiling and is intentionally
// small: each admitted device costs an MQTT discovery set plus retained state,
// and the ESP32-C3 has a tight connection/memory budget. Override at compile
// time via MB_MAX_DEVICES in include/config.h; it is clamped to a sane ceiling
// so a mis-set value cannot exhaust RAM.
#ifndef MB_MAX_DEVICES
#define MB_MAX_DEVICES 4
#endif

static_assert(MB_MAX_DEVICES >= 1, "MB_MAX_DEVICES must be at least 1");
static_assert(MB_MAX_DEVICES <= 8, "MB_MAX_DEVICES exceeds the ESP32-C3 ceiling");

using DeviceRegistry = DeviceRegistryT<MB_MAX_DEVICES>;

}  // namespace mb
