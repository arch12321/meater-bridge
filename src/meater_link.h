#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <array>
#include <cstdint>
#include <vector>

#include "bridge_types.h"

class MeaterLinkBridge {
public:
    void begin(uint64_t bridgeDeviceId);
    void loop(const MeaterSample& sample);
    const CookControl& cookControl() const { return cook_; }
    uint32_t setupRevision() const { return setupRevision_; }

private:
    struct Subscriber {
        IPAddress address;
        uint16_t port{7878};
        uint32_t lastSeenMs{0};
        bool active{false};
    };

    struct IncomingPacket {
        bool validHeader{false};
        bool subscription{false};
        bool statusRequest{false};
        bool historyRequest{false};
        const uint8_t* historyRequestData{nullptr};
        size_t historyRequestLength{0};
        const uint8_t* setupData{nullptr};
        size_t setupLength{0};
    };

    static constexpr uint16_t kPort = 7878;
    static constexpr uint32_t kProtocolId = 21578;
    static constexpr uint32_t kProtocolMajor = 17;
    static constexpr uint32_t kProtocolMinor = 8;
    static constexpr uint32_t kSubscriberTimeoutMs = 30000;

    IncomingPacket inspectPacket(const uint8_t* data, size_t length) const;
    bool validateHeader(const uint8_t* data, size_t length) const;
    void handlePacket(const uint8_t* data, size_t length, const IPAddress& address, uint16_t port,
                      const MeaterSample& sample);
    void applySetup(const uint8_t* data, size_t length, const MeaterSample& sample);
    void addSubscriber(const IPAddress& address, uint16_t port);
    void expireSubscribers(uint32_t now);

    std::vector<uint8_t> buildMasterPacket(const MeaterSample& sample);
    std::vector<uint8_t> buildStatusPacket();
    std::vector<uint8_t> buildHistoryPacket(const MeaterSample& sample);
    void sendPacket(const std::vector<uint8_t>& packet, const IPAddress& address, uint16_t port);
    void sendMaster(const MeaterSample& sample, bool includeBroadcast);
    void sendStatus(const IPAddress& address, uint16_t port);

    WiFiUDP udp_;
    std::array<Subscriber, 6> subscribers_{};
    CookControl cook_{};
    uint64_t bridgeDeviceId_{0};
    uint32_t messageNumber_{1};
    uint32_t lastMasterMs_{0};
    uint32_t lastBroadcastMs_{0};
    uint32_t lastSampleUpdate_{0};
    bool started_{false};
    uint32_t setupRevision_{0};
};
