#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mb {

template <size_t Capacity>
class DeviceRotationT {
public:
    struct Candidate {
        bool inUse{false};
        bool isBase{false};
        uint64_t endpointId{0};
        int16_t rssi{-127};
        uint32_t lastSeenMs{0};
        uint32_t lastServedMs{0};
    };

    void observe(uint64_t endpointId, bool isBase, int16_t rssi, uint32_t nowMs) {
        if (endpointId == 0) return;
        Candidate* slot = nullptr;
        for (auto& candidate : candidates_) {
            if (candidate.inUse && candidate.endpointId == endpointId) {
                slot = &candidate;
                break;
            }
            if (!candidate.inUse && slot == nullptr) slot = &candidate;
        }
        if (slot == nullptr) return;
        if (!slot->inUse) {
            *slot = Candidate{};
            slot->inUse = true;
            slot->endpointId = endpointId;
        }
        slot->isBase = isBase;
        slot->rssi = rssi;
        slot->lastSeenMs = nowMs;
    }

    const Candidate* select(bool preferBase, uint32_t nowMs,
                            uint32_t maxUnseenMs = 600000U) const {
        bool haveBase = false;
        for (const auto& c : candidates_) {
            if (recent(c, nowMs, maxUnseenMs) && c.isBase) haveBase = true;
        }
        const Candidate* best = nullptr;
        for (const auto& c : candidates_) {
            if (!recent(c, nowMs, maxUnseenMs)) continue;
            if (preferBase && haveBase && !c.isBase) continue;
            if (best == nullptr || c.lastServedMs < best->lastServedMs ||
                (c.lastServedMs == best->lastServedMs && c.rssi > best->rssi)) {
                best = &c;
            }
        }
        return best;
    }

    void markServed(uint64_t endpointId, uint32_t nowMs) {
        for (auto& c : candidates_) {
            if (c.inUse && c.endpointId == endpointId) {
                c.lastServedMs = nowMs == 0 ? 1 : nowMs;
                return;
            }
        }
    }

    size_t eligibleCount(bool preferBase, uint32_t nowMs,
                         uint32_t maxUnseenMs = 600000U) const {
        bool haveBase = false;
        for (const auto& c : candidates_) {
            if (recent(c, nowMs, maxUnseenMs) && c.isBase) haveBase = true;
        }
        size_t count = 0;
        for (const auto& c : candidates_) {
            if (!recent(c, nowMs, maxUnseenMs)) continue;
            if (preferBase && haveBase && !c.isBase) continue;
            ++count;
        }
        return count;
    }

    bool shouldRotate(bool preferBase, uint32_t connectedSinceMs, uint32_t nowMs,
                      uint32_t dwellMs) const {
        return connectedSinceMs != 0 && nowMs - connectedSinceMs >= dwellMs &&
               eligibleCount(preferBase, nowMs) > 1;
    }

private:
    static bool recent(const Candidate& c, uint32_t nowMs, uint32_t maxUnseenMs) {
        return c.inUse && nowMs - c.lastSeenMs <= maxUnseenMs;
    }

    std::array<Candidate, Capacity> candidates_{};
};

}  // namespace mb
