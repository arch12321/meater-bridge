#include <cassert>
#include <cstdint>
#include <iostream>

#include "device_rotation.h"

int main() {
    mb::DeviceRotationT<4> rotation;
    rotation.observe(1, false, -40, 1000);
    rotation.observe(2, true, -70, 1000);
    rotation.observe(3, true, -60, 1000);

    const auto* selected = rotation.select(true, 1000);
    assert(selected != nullptr && selected->endpointId == 3); // base preferred, stronger tie
    rotation.markServed(3, 1000);
    selected = rotation.select(true, 2000);
    assert(selected != nullptr && selected->endpointId == 2); // unserved base next
    rotation.markServed(2, 2000);
    selected = rotation.select(true, 3000);
    assert(selected != nullptr && selected->endpointId == 3); // oldest served base

    assert(!rotation.shouldRotate(true, 1000, 30999, 30000));
    assert(rotation.shouldRotate(true, 1000, 31000, 30000));
    assert(rotation.eligibleCount(true, 700001, 600000) == 0); // sightings aged out

    mb::DeviceRotationT<2> directOnly;
    directOnly.observe(10, false, -80, 5);
    directOnly.observe(11, false, -50, 5);
    assert(directOnly.select(true, 5)->endpointId == 11);
    directOnly.markServed(11, 5);
    assert(directOnly.select(true, 6)->endpointId == 10);
    std::cout << "device rotation host tests passed\n";
}
