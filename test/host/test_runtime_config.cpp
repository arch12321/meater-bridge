#include <cassert>
#include <iostream>

#include "runtime_config_validation.h"

int main() {
    assert(mb::validNodeId(mb::textView("meater_bridge-2")));
    assert(!mb::validNodeId(mb::textView("meater/bridge")));
    assert(!mb::validNodeId(mb::textView("")));
    assert(mb::validMqttPort(8883));
    assert(!mb::validMqttPort(0));
    assert(!mb::validMqttPort(70000));
    assert(mb::validMacOrEmpty(mb::textView("")));
    assert(mb::validMacOrEmpty(mb::textView("AA:bb:01:23:45:67")));
    assert(!mb::validMacOrEmpty(mb::textView("AA-BB-CC-DD-EE-FF")));
    assert(mb::strongOtaPassword(mb::textView("BridgeSecure2026")));
    assert(!mb::strongOtaPassword(mb::textView("short1")));
    assert(!mb::strongOtaPassword(mb::textView("letters-only")));
    assert(mb::looksLikePemCertificate(mb::textView(
        "-----BEGIN CERTIFICATE-----\nAA==\n-----END CERTIFICATE-----\n")));
    assert(!mb::looksLikePemCertificate(mb::textView("not a certificate")));
    std::cout << "runtime configuration host tests passed\n";
}
