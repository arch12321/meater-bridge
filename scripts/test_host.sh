#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${KIROCREW_SCRATCH:-${TMPDIR:-/tmp}}/meater-bridge-host-tests"
mkdir -p "$OUT"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_protobuf.cpp" -o "$OUT/test_protobuf"
"$OUT/test_protobuf"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_history.cpp" -o "$OUT/test_history"
"$OUT/test_history"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_interfaces.cpp" -o "$OUT/test_interfaces"
"$OUT/test_interfaces"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" -I"$ROOT/include" \
  "$ROOT/test/host/test_rssi.cpp" -o "$OUT/test_rssi"
"$OUT/test_rssi"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_ble_supervisor.cpp" -o "$OUT/test_ble_supervisor"
"$OUT/test_ble_supervisor"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_availability.cpp" -o "$OUT/test_availability"
"$OUT/test_availability"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_physical_history.cpp" -o "$OUT/test_physical_history"
"$OUT/test_physical_history"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" -I"$ROOT/include" \
  "$ROOT/test/host/test_cook_write.cpp" -o "$OUT/test_cook_write"
"$OUT/test_cook_write"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_device_rotation.cpp" -o "$OUT/test_device_rotation"
"$OUT/test_device_rotation"
c++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/test/host/test_runtime_config.cpp" -o "$OUT/test_runtime_config"
"$OUT/test_runtime_config"
