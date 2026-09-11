#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "protobuf_wire.h"

int main() {
    pb::Writer header;
    header.uint32Field(1, 21578);
    header.uint32Field(2, 17);
    header.uint32Field(3, 8);
    header.uint32Field(4, 1);
    header.fixed64Field(5, UINT64_C(0x0102030405060708));

    const std::vector<uint8_t> expected{
        0x08, 0xca, 0xa8, 0x01, 0x10, 0x11, 0x18, 0x08, 0x20, 0x01,
        0x29, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    assert(header.bytes() == expected);

    pb::Writer subscription;
    subscription.enumField(2, 2);
    pb::Writer packet;
    packet.messageField(1, header);
    packet.messageField(2, subscription);

    pb::Reader top(packet.bytes().data(), packet.bytes().size());
    pb::Field field;
    bool foundHeader = false;
    bool foundSubscription = false;
    while (top.next(field)) {
        if (field.number == 1) {
            assert(field.type == pb::WireType::LengthDelimited);
            pb::Reader nested(field.data, field.length);
            pb::Field nestedField;
            uint64_t protocol = 0;
            uint64_t major = 0;
            while (nested.next(nestedField)) {
                if (nestedField.number == 1) protocol = nestedField.varintValue;
                if (nestedField.number == 2) major = nestedField.varintValue;
            }
            assert(nested.valid());
            assert(protocol == 21578);
            assert(major == 17);
            foundHeader = true;
        } else if (field.number == 2) {
            foundSubscription = true;
        }
    }
    assert(top.valid());
    assert(foundHeader && foundSubscription);

    pb::Writer signedValues;
    signedValues.sint32Field(1, -62);
    pb::Reader signedReader(signedValues.bytes().data(), signedValues.bytes().size());
    assert(signedReader.next(field));
    assert(pb::decodeZigZag32(field.varintValue) == -62);
    assert(signedReader.valid());

    std::cout << "protobuf host tests passed\n";
    return 0;
}
