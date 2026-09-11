#include "meater_link.h"

#include <WiFi.h>

#include <algorithm>
#include <ctime>
#include <cstring>
#include <string>

#include "bridge_config.h"
#include "meater_link_history.h"
#include "protobuf_wire.h"

namespace {

void addHeader(pb::Writer& top, uint32_t number, uint64_t bridgeId) {
    pb::Writer header;
    header.uint32Field(1, 21578);
    header.uint32Field(2, 17);
    header.uint32Field(3, 8);
    header.uint32Field(4, number);
    header.fixed64Field(5, bridgeId);
    top.messageField(1, header);
}

}  // namespace

void MeaterLinkBridge::begin(uint64_t bridgeDeviceId) {
    bridgeDeviceId_ = bridgeDeviceId;
#if MB_MEATER_LINK_ENABLED
    if (udp_.begin(kPort)) {
        started_ = true;
        Serial.printf("MEATER Link listening on UDP %u (protocol %u.%u)\n", kPort,
                      kProtocolMajor, kProtocolMinor);
    } else {
        Serial.println("MEATER Link failed to bind UDP 7878");
    }
#endif
}

bool MeaterLinkBridge::validateHeader(const uint8_t* data, size_t length) const {
    pb::Reader reader(data, length);
    pb::Field field;
    uint64_t identifier = 0;
    uint64_t major = 0;
    while (reader.next(field)) {
        if (field.type != pb::WireType::Varint) {
            continue;
        }
        if (field.number == 1) {
            identifier = field.varintValue;
        } else if (field.number == 2) {
            major = field.varintValue;
        }
    }
    return reader.valid() && identifier == kProtocolId && major == kProtocolMajor;
}

MeaterLinkBridge::IncomingPacket MeaterLinkBridge::inspectPacket(const uint8_t* data,
                                                                  size_t length) const {
    IncomingPacket result;
    pb::Reader reader(data, length);
    pb::Field field;
    while (reader.next(field)) {
        if (field.type != pb::WireType::LengthDelimited) {
            continue;
        }
        if (field.number == 1) {
            result.validHeader = validateHeader(field.data, field.length);
        } else if (field.number == 2) {
            result.subscription = true;
        } else if (field.number == 4) {
            result.setupData = field.data;
            result.setupLength = field.length;
        } else if (field.number == 5) {
            result.historyRequest = true;
            result.historyRequestData = field.data;
            result.historyRequestLength = field.length;
        } else if (field.number == 10) {
            result.statusRequest = true;
        }
    }
    if (!reader.valid()) {
        result = IncomingPacket{};
    }
    return result;
}

void MeaterLinkBridge::addSubscriber(const IPAddress& address, uint16_t port) {
    const uint32_t now = millis();
    Subscriber* freeSlot = nullptr;
    Subscriber* oldest = &subscribers_[0];

    for (auto& subscriber : subscribers_) {
        if (subscriber.active && subscriber.address == address && subscriber.port == port) {
            subscriber.lastSeenMs = now;
            return;
        }
        if (!subscriber.active && freeSlot == nullptr) {
            freeSlot = &subscriber;
        }
        if (subscriber.lastSeenMs < oldest->lastSeenMs) {
            oldest = &subscriber;
        }
    }

    Subscriber* slot = freeSlot != nullptr ? freeSlot : oldest;
    slot->address = address;
    slot->port = port;
    slot->lastSeenMs = now;
    slot->active = true;
    Serial.printf("MEATER Link subscriber %s:%u\n", address.toString().c_str(), port);
}

void MeaterLinkBridge::expireSubscribers(uint32_t now) {
    for (auto& subscriber : subscribers_) {
        if (subscriber.active && now - subscriber.lastSeenMs > kSubscriberTimeoutMs) {
            subscriber.active = false;
        }
    }
}

void MeaterLinkBridge::applySetup(const uint8_t* data, size_t length,
                                  const MeaterSample& sample) {
    pb::Reader setupMessage(data, length);
    pb::Field field;
    uint64_t deviceId = 0;
    const uint8_t* cookData = nullptr;
    size_t cookLength = 0;

    while (setupMessage.next(field)) {
        if (field.number == 1 && field.type == pb::WireType::Fixed64) {
            deviceId = field.fixed64Value;
        } else if (field.number == 2 && field.type == pb::WireType::LengthDelimited) {
            cookData = field.data;
            cookLength = field.length;
        }
    }
    if (!setupMessage.valid() || deviceId != sample.deviceId || cookData == nullptr) {
        return;
    }

    CookControl next = cook_;
    pb::Reader cookReader(cookData, cookLength);
    while (cookReader.next(field)) {
        if (field.number == 1 && field.type == pb::WireType::Varint) {
            next.sequenceNumber = static_cast<uint32_t>(field.varintValue);
        } else if (field.number == 2 && field.type == pb::WireType::Varint) {
            next.state = static_cast<uint32_t>(field.varintValue);
        } else if (field.number == 3 && field.type == pb::WireType::Varint) {
            next.targetInternalRaw32 = pb::decodeZigZag32(field.varintValue);
        } else if (field.number == 9 && field.type == pb::WireType::LengthDelimited) {
            const size_t copy = std::min(field.length, sizeof(next.name) - 1);
            std::memcpy(next.name, field.data, copy);
            next.name[copy] = '\0';
        } else if (field.number == 12 && field.type == pb::WireType::Fixed64) {
            next.cookId = field.fixed64Value;
        }
    }
    if (!cookReader.valid()) {
        return;
    }

    if (next.state == 2 && cook_.state != 2) {
        next.startedMs = millis();
    } else if (next.state == 0) {
        next.startedMs = 0;
    }
    cook_ = next;
    ++setupRevision_;
    Serial.printf("MEATER app cook setup: state=%u target=%.2f C name=%s\n", cook_.state,
                  cook_.targetInternalRaw32 / 32.0f, cook_.name);
}

void MeaterLinkBridge::handlePacket(const uint8_t* data, size_t length,
                                    const IPAddress& address, uint16_t port,
                                    const MeaterSample& sample) {
    const IncomingPacket incoming = inspectPacket(data, length);
    if (!incoming.validHeader) {
        return;
    }
    if (incoming.subscription) {
        addSubscriber(address, port);
        if (sample.valid) {
            sendPacket(buildMasterPacket(sample), address, port);
        }
    }
    if (incoming.statusRequest) {
        sendStatus(address, port);
    }
    if (incoming.historyRequest && sample.valid && sample.historyCount > 0) {
        uint64_t requestedId = 0;
        pb::Reader reader(incoming.historyRequestData, incoming.historyRequestLength);
        pb::Field field;
        while (reader.next(field)) {
            if (field.number == 1 && field.type == pb::WireType::Fixed64) {
                requestedId = field.fixed64Value;
            }
        }
        if (reader.valid() && (requestedId == 0 || requestedId == sample.deviceId)) {
            sendPacket(buildHistoryPacket(sample), address, port);
        }
    }
    if (incoming.setupData != nullptr && sample.valid) {
        applySetup(incoming.setupData, incoming.setupLength, sample);
        sendPacket(buildMasterPacket(sample), address, port);
    }
}

std::vector<uint8_t> MeaterLinkBridge::buildMasterPacket(const MeaterSample& sample) {
    pb::Writer cookSetup;
    cookSetup.uint32Field(1, cook_.sequenceNumber);
    cookSetup.enumField(2, cook_.state);
    cookSetup.sint32Field(3, cook_.targetInternalRaw32);
    if (cook_.name[0] != '\0') {
        cookSetup.stringField(9, cook_.name);
    }
    if (cook_.cookId != 0) {
        cookSetup.fixed64Field(12, cook_.cookId);
    }
    cookSetup.uint32Field(99, 96);

    const uint32_t localElapsed = cook_.startedMs == 0 ? 0 :
        (millis() - cook_.startedMs) / 1000U;
    const uint32_t historyElapsed = sample.historyElapsedSeconds == 0 ? 0 :
        sample.historyElapsedSeconds + (millis() - sample.historyCapturedMs) / 1000U;
    const uint32_t elapsed = std::max(localElapsed, historyElapsed);
    pb::Writer status;
    status.sint32Field(1, sample.internalRaw32);
    status.sint32Field(2, sample.ambientRaw32);
    status.sint32Field(3, sample.peakRaw32);
    status.sint32Field(4, 0);
    status.uint32Field(5, elapsed);
    status.sint32Field(6, 0);
    for (uint8_t i = 0; i < sample.internalCount; ++i) {
        status.sint32Field(7, sample.internalsRaw32[i]);
    }

    pb::Writer probe;
    probe.fixed64Field(1, 0);
    probe.messageField(3, cookSetup);
    probe.messageField(4, status);

    pb::Writer charge;
    charge.enumField(1, 1);  // NOT_SUPPORTED, matching the Android peer-master builder.
    charge.uint32Field(2, sample.batteryPercent);
    charge.uint32Field(3, 0);

    pb::Writer device;
    device.messageField(1, probe);
    device.fixed64Field(5, sample.deviceId);
    device.uint32Field(6, sample.probeNumber);
    device.messageField(7, charge);
    if (sample.firmware[0] != '\0') {
        device.stringField(8, sample.firmware);
    }
    device.enumField(9, sample.bleConnected ? 1 : 0);
    device.enumField(10, 0);  // BLE
    device.sint32Field(11, sample.rssi);

    pb::Writer master;
    master.enumField(1, 2);  // MASTER_TYPE_ANDROID: peer MEATER Link master.
    master.enumField(2, 0);  // Cloud disabled.
    master.messageField(3, device);

    pb::Writer packet;
    addHeader(packet, messageNumber_++, bridgeDeviceId_);
    packet.messageField(3, master);
    return packet.bytes();
}

std::vector<uint8_t> MeaterLinkBridge::buildStatusPacket() {
    pb::Writer status;
    status.fixed64Field(1, bridgeDeviceId_);
    status.enumField(2, 2);  // Android peer master.
    status.enumField(3, 1);  // Phone subtype.
    status.uint32Field(4, 100);
    status.stringField(6, std::string("meater-bridge/") + MB_FIRMWARE_VERSION);

    pb::Writer statusMessage;
    statusMessage.messageField(1, status);

    pb::Writer packet;
    addHeader(packet, messageNumber_++, bridgeDeviceId_);
    packet.messageField(11, statusMessage);
    return packet.bytes();
}

std::vector<uint8_t> MeaterLinkBridge::buildHistoryPacket(const MeaterSample& sample) {
    meater_link::TemperatureHistoryResponse response;
    response.deviceId = sample.deviceId;
    response.cookId = cook_.cookId;
    response.history.interval = sample.historyIntervalSeconds;
    const uint32_t elapsed = sample.historyElapsedSeconds +
        (sample.historyCapturedMs == 0 ? 0 : (millis() - sample.historyCapturedMs) / 1000U);
    const time_t now = time(nullptr);
    response.history.startTime = now > 1000000000 ? static_cast<uint32_t>(now - elapsed) : 0;
    response.history.values.reserve(sample.historyCount);
    for (uint16_t i = 0; i < sample.historyCount && i < 120; ++i) {
        meater_link::TemperatureRecording rec;
        rec.internalRaw32 = sample.historyInternalRaw32[i];
        rec.ambientRaw32 = sample.historyAmbientRaw32[i];
        response.history.values.push_back(rec);
    }
    if (sample.peakRaw32 != MB_INVALID_TEMP_RAW32) {
        response.history.peaks.push_back(sample.peakRaw32);
    }
    pb::Writer body;
    const size_t encodedCount =
        meater_link::encodeHistoryResponseBounded(body, response, 1400);
    if (encodedCount < response.history.values.size()) {
        log_w("MEATER history response trimmed from %u to %u samples for UDP",
              static_cast<unsigned>(response.history.values.size()),
              static_cast<unsigned>(encodedCount));
    }
    pb::Writer packet;
    addHeader(packet, messageNumber_++, bridgeDeviceId_);
    packet.messageField(meater_link::kMlFieldTemperatureHistoryResponse, body);
    return packet.bytes();
}

void MeaterLinkBridge::sendPacket(const std::vector<uint8_t>& packet,
                                  const IPAddress& address, uint16_t port) {
    if (packet.empty() || packet.size() > 1500) {
        return;
    }
    udp_.beginPacket(address, port);
    udp_.write(packet.data(), packet.size());
    udp_.endPacket();
}

void MeaterLinkBridge::sendStatus(const IPAddress& address, uint16_t port) {
    sendPacket(buildStatusPacket(), address, port);
}

void MeaterLinkBridge::sendMaster(const MeaterSample& sample, bool includeBroadcast) {
    const std::vector<uint8_t> packet = buildMasterPacket(sample);
    for (const auto& subscriber : subscribers_) {
        if (subscriber.active) {
            sendPacket(packet, subscriber.address, subscriber.port);
        }
    }
    if (includeBroadcast) {
        sendPacket(packet, IPAddress(255, 255, 255, 255), kPort);
    }
}

void MeaterLinkBridge::loop(const MeaterSample& sample) {
#if MB_MEATER_LINK_ENABLED
    if (!started_ || WiFi.status() != WL_CONNECTED) {
        return;
    }

    int packetLength = udp_.parsePacket();
    while (packetLength > 0) {
        std::array<uint8_t, 1500> buffer{};
        const int readLength = udp_.read(buffer.data(), std::min(packetLength, 1500));
        if (readLength > 0) {
            handlePacket(buffer.data(), static_cast<size_t>(readLength), udp_.remoteIP(),
                         udp_.remotePort(), sample);
        }
        packetLength = udp_.parsePacket();
    }

    const uint32_t now = millis();
    expireSubscribers(now);
    if (!sample.valid) {
        return;
    }

    const bool newReading = sample.updatedMs != lastSampleUpdate_;
    const bool periodic = now - lastMasterMs_ >= 2500;
    const bool broadcast = now - lastBroadcastMs_ >= 5000;
    if (newReading || periodic || broadcast) {
        sendMaster(sample, broadcast);
        lastMasterMs_ = now;
        lastSampleUpdate_ = sample.updatedMs;
        if (broadcast) {
            lastBroadcastMs_ = now;
        }
    }
#endif
}
