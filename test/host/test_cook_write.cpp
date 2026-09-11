#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "meater_probe_gatt.h"

int main() {
    CookControl cook;
    cook.sequenceNumber = 7;
    cook.state = 2;
    cook.targetInternalRaw32 = 2144;
    cook.cookId = 0x1122334455667788ULL;
    std::strcpy(cook.name, "Brisket");

    const std::vector<uint8_t> payload = meater_gatt::buildCookSetupPayload(cook);
    const uint8_t prefix[] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    assert(payload.size() > sizeof(prefix));
    assert(std::memcmp(payload.data(), prefix, sizeof(prefix)) == 0);

    pb::Reader reader(payload.data() + sizeof(prefix), payload.size() - sizeof(prefix));
    pb::Field field;
    bool sequence = false, state = false, target = false, name = false, id = false, last = false;
    while (reader.next(field)) {
        if (field.number == 1) { assert(field.varintValue == 7); sequence = true; }
        else if (field.number == 2) { assert(field.varintValue == 2); state = true; }
        else if (field.number == 3) { assert(pb::decodeZigZag32(field.varintValue) == 2144); target = true; }
        else if (field.number == 9) { assert(std::string(reinterpret_cast<const char*>(field.data), field.length) == "Brisket"); name = true; }
        else if (field.number == 12) { assert(field.fixed64Value == cook.cookId); id = true; }
        else if (field.number == 99) { assert(field.varintValue == 96); last = true; }
    }
    assert(reader.finished());
    assert(sequence && state && target && name && id && last);
#if MB_PROBE_WRITE_ENABLED
    assert(meater_gatt::writesCompiledIn());
#else
    assert(!meater_gatt::writesCompiledIn());
#endif
    std::cout << "physical cook-write host tests passed\n";
}
