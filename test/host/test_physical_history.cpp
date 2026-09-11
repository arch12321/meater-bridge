#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "physical_history.h"

static void put16(std::vector<uint8_t>& b, size_t p, int16_t v) {
    b[p] = static_cast<uint8_t>(v);
    b[p + 1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8U);
}
static void put32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(v >> (8U * i));
}

int main() {
    std::vector<uint8_t> converted(484, 0);
    put16(converted, 0, 15);
    put16(converted, 2, 2);
    put16(converted, 4, 640);  // 40 C in 1/16 C
    put16(converted, 6, 1280); // 80 C
    put16(converted, 8, 656);
    put16(converted, 10, 1296);
    mb::PhysicalHistory h;
    assert(mb::parsePhysicalHistory(converted.data(), converted.size(), false, h));
    assert(h.intervalSeconds == 15 && h.count == 2 && h.elapsedSeconds == 30);
    assert(h.internalRaw32[0] == 1280 && h.ambientRaw32[0] == 2560);

    std::vector<uint8_t> extended(499, 0);
    put16(extended, 0, 25);
    put16(extended, 2, 1);
    put16(extended, 4, 700);
    put16(extended, 6, 900);
    put32(extended, 485, 2700); // already-running 45 minute cook
    assert(mb::parsePhysicalHistory(extended.data(), extended.size(), false, h));
    assert(h.elapsedSeconds == 2700);

    std::vector<uint8_t> bad(484, 0);
    put16(bad, 0, 7);
    assert(!mb::parsePhysicalHistory(bad.data(), bad.size(), false, h));
    std::cout << "physical history host tests passed\n";
}
