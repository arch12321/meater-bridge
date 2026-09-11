# MEATER ESP32 Bridge

[![Flash Firmware](https://img.shields.io/badge/Flash-Web%20Installer-orange?style=for-the-badge)](https://arch12321.github.io/meater-bridge/flash/)
[![GitHub Release](https://img.shields.io/github/v/release/arch12321/meater-bridge?style=for-the-badge)](https://github.com/arch12321/meater-bridge/releases/latest)
[![License](https://img.shields.io/github/license/arch12321/meater-bridge?style=for-the-badge)](LICENSE)

> **Built with AI.** This entire project — firmware, protocol reverse-engineering documentation, test suite, and README — was created using [Kiro](https://kiro.dev) with GPT Sol 5.6 in under 2 hours across 14 prompts. An additional 12 prompts expanded functionality (multi-device support, RSSI quality bands, freshness gating) and improved reusability. The code has been validated on real hardware; the AI origin doesn't diminish its quality — it demonstrates what's now possible.

**[⚡ Flash Now](https://arch12321.github.io/meater-bridge/flash/)** · **[📦 Download Binaries](https://github.com/arch12321/meater-bridge/releases/latest)** · **[📖 Documentation](#what-this-is)**

---

Local bridge that owns the MEATER probe's single BLE path and publishes each reading in parallel to:

1. the official MEATER Android app over same-LAN MEATER Link UDP; and
2. MQTT with Home Assistant discovery.

The MEATER Link implementation is based on the field tags and runtime flow recovered from the authenticated MEATER Android 5.1.0 APK (version code 515): protocol identifier `21578`, version `17.8`, UDP port `7878`.

## What this is

Bluetooth barely reaches, and that is the whole problem. A MEATER probe is a tiny battery-powered radio buried inside a joint of meat inside a metal box, so you have to keep your phone within a few metres of the cooker for the entire cook or the readings stop.

This firmware turns a cheap ESP32 into a mains-powered stand-in that sits by the cooker so you don't have to. It holds the short Bluetooth hop permanently and forwards every reading over your home Wi-Fi, which reaches the whole house. Two things fall out of that.

**The official MEATER app keeps working, from anywhere on your Wi-Fi.** MEATER already built a way for the app to read a probe over the network instead of over Bluetooth, because that is how the app talks to a MEATER Block. The bridge speaks that same protocol and reports your probe's real ID, so the app shows the probe you already paired. Nothing to install, nothing to re-pair, no **Add Device**.

**Home Assistant gets everything, in parallel.** 44 entities appear on their own over MQTT discovery: temperatures, cook target and elapsed time, both batteries, per-hop signal strength with quality bands, and connection and staleness diagnostics. From there it is ordinary Home Assistant data, so you can graph a cook, keep its history, or have your phone buzz when the meat hits temperature.

The strongest version of this uses a powered MEATER+ charger as a relay, which is the topology below. The charger is already a Bluetooth repeater designed to sit near the cooker, so the probe makes one short hop to the charger and the charger makes another short hop to the ESP32. Both hops are fixed and close, which is exactly what Bluetooth is good at. The bridge prefers that path automatically and falls back to connecting straight to the probe if no charger turns up within its six-second scan.

Nothing here touches the internet. There is no account, no subscription, and no MEATER Cloud, so your cook data stays on your network. The trade-off is that the app only sees the bridge while the phone is on the same Wi-Fi; for watching a cook from the pub, use Home Assistant's own remote access or a VPN.

Start with [Quick flash](#quick-flash-no-build-required) if you just want to get running, or see the [Hardware guide](#hardware-guide) and [Build and flash](#build-and-flash) for source builds.

## Proven topology

```text
MEATER probe -- BLE --> powered MEATER+ charger/repeater -- BLE --> ESP32
                                                                       |-- UDP 7878 --> MEATER app
                                                                       `-- MQTT -----> Home Assistant
```

The firmware prefers a supported powered charger/repeater. If none appears during the six-second scan window, it falls back to a direct probe connection.

## Features

- Prefers a powered MEATER+/SE charger/repeater and falls back to a direct probe.
- Uses finite BLE scans, connect/no-temperature watchdogs, half-open teardown, bounded backoff and base anti-thrash cooldown.
- Reads the real child probe ID, firmware, batteries, temperatures, state, raw RSSI and physical temperature history.
- Recovers an already-running cook's elapsed duration from the physical log instead of restarting at bridge handoff.
- Serves MEATER Link v17.8 live state, cook setup echo and bounded temperature-history responses on UDP `7878`.
- Publishes retained MQTT state and Home Assistant discovery with freshness-gated temperatures, source ages and raw/EMA RSSI bands.
- Tracks up to `MB_MAX_DEVICES` logical probes; one BLE link is time-sliced and each additional probe gets independent retained MQTT/HA topics.
- Stores Wi-Fi, MQTT, TLS, OTA and BLE preferences in NVS through an authenticated captive portal.
- Supports CA-validated MQTT TLS with no insecure TLS fallback; plaintext MQTT remains an explicit compatibility mode.
- Supports password-authenticated LAN OTA with strong-password validation.
- Keeps physical cook writes compiled out by default and requires both compile-time and NVS runtime opt-in.
- Runs strict host tests for protobuf, history, physical logs, cook writes, BLE supervision, freshness, RSSI, rotation and runtime validation.

## Boundaries

- **Same LAN only:** the bridge implements MEATER Link, not MEATER Cloud.
- Away-from-home monitoring should use secure Home Assistant remote access.
- Pair the real probe with the MEATER app normally at least once before using the bridge.
- The bridge reuses the real child probe ID so the app recognises the existing paired device.
- A MEATER+ Bluetooth charger/repeater must remain powered while used in the preferred topology.
- An actual Wi-Fi MEATER Block/Pro XL already provides its own Wi-Fi bridge and follows a different path.
- Original MEATER, MEATER+ and MEATER SE use the proven G1 decoder.
- MEATER Pro/2 Plus support is best-effort through the recovered G2 decoder and still requires hardware validation.
- The official app receives only the first resolved probe; additional probes are MQTT/Home Assistant devices and temporarily take the one BLE link during rotation.
- With base preference enabled, rotation stays among discovered bases when any base is present; set a fixed MAC to disable rotation entirely.
- Physical cook setup writes are implemented but compile/runtime gated off and remain live-hardware experimental.
- ArduinoOTA is strong-password authenticated for a trusted LAN; it is not a substitute for ESP32 Secure Boot or signed firmware.
- Runtime secrets are hidden from the portal and logs but ordinary NVS is not encrypted against an attacker with physical flash access.

## Hardware guide

### What to buy

| Item | Needed? | Notes |
|---|---|---|
| **ESP32 board with BLE, 4 MB flash minimum** | Yes | Any ESP32, ESP32-C3, ESP32-S3 or ESP32-C6 board works. The `esp32-c3-devkitm-1` is the default and lists at about $8 from [Mouser](https://www.mouser.com/ProductDetail/Espressif-Systems/ESP32-C3-DevKitM-1?qs=pUKx8fyJudB1sOWbbEnGFw%3D%3D). Generic boards cost less and work the same. 4 MB flash is a hard floor: the build uses 63.8% of each 1.875 MiB OTA slot under `min_spiffs.csv`. See [Supported boards](#supported-boards) for the full list. |
| **MEATER probe** | Yes | Original, MEATER+ or MEATER SE are on the proven G1 decoder. See [Boundaries](#boundaries) for Pro/2 Plus. |
| **Powered MEATER+ charger/base** | Strongly recommended | The relay in the proven topology. You likely already own one. It must stay powered, with its battery in. |
| **USB cable that carries data** | Yes | For the first flash. Charge-only cables fail confusingly; later updates can go over OTA. |
| **5 V USB power supply** | Yes | Any phone charger. This is the point of the exercise: the radio stays powered and stationary. |
| **MQTT broker** | Only for Home Assistant | Usually Mosquitto alongside Home Assistant. The bridge runs MEATER Link fine without it. |

You also need a 2.4 GHz Wi-Fi network. The ESP32 has no 5 GHz radio, so a 5 GHz-only SSID will never associate.

No soldering, no extra components, and no enclosure needed to get a first reading.

### Supported boards

The firmware supports any ESP32 variant with Bluetooth Low Energy (BLE) and at least 4 MB flash. Pre-configured environments exist for popular boards; other boards can use a similar environment or the generic `esp32dev`.

| Chip | Environment name | Notes |
|---|---|---|
| **ESP32-C3** | `esp32-c3-devkitm-1` (default) | RISC-V single-core, BLE 5.0, compact and power-efficient |
| | `esp32-c3-devkitc-02` | |
| | `seeed_xiao_esp32c3` | Ultra-compact Seeed XIAO form factor |
| | `lolin_c3_mini` | |
| **ESP32-S3** | `esp32-s3-devkitc-1` | Xtensa dual-core, BLE 5.0, more RAM |
| | `esp32-s3-devkitm-1` | |
| | `lolin_s3` | |
| | `seeed_xiao_esp32s3` | |
| **ESP32 (original)** | `esp32dev` | Xtensa dual-core, BLE 4.2, most common |
| | `nodemcu-32s` | |
| | `lolin32` | |
| | `esp-wrover-kit` | |
| **ESP32-C6** | `esp32-c6-devkitc-1` | RISC-V single-core, BLE 5.0, Wi-Fi 6 |
| | `esp32-c6-devkitm-1` | |

**Not compatible:** ESP32-S2 (no Bluetooth), ESP8266 (no BLE, insufficient resources).

### Changing the target board

There are three ways to build for a different board:

**Option 1: Edit the default environment (permanent)**

Edit `platformio.ini` and change the `default_envs` line:

```ini
[platformio]
default_envs = esp32dev    ; was esp32-c3-devkitm-1
```

Then build normally with `pio run`.

**Option 2: Use the `-e` flag (per-build)**

Build for a specific environment without changing the config file:

```bash
pio run -e esp32dev
pio run -e esp32-s3-devkitc-1
pio run -e nodemcu-32s
```

**Option 3: Add a custom board**

For boards not listed, add a new environment to `platformio.ini`:

```ini
[env:my_custom_board]
extends = common
board = my_board_name
```

Find your board name with `pio boards | grep -i esp32` or check the [PlatformIO board list](https://docs.platformio.org/en/latest/boards/index.html#espressif-32).

### Why the charger is worth using

Skipping the charger works, but it spends your range budget badly. Connecting the ESP32 straight to the probe means the one Bluetooth hop has to cross the cooker wall and whatever is in the way, from a transmitter that is inside the food.

With the charger in the path, the probe makes a short hop to a repeater built for the job, and the ESP32 talks to the charger instead. You also get more data: the charger reports both batteries, its own chip temperature, the base-to-probe connection state, and the child probe's identity and firmware, which is what produces the separate base and probe entity sets in Home Assistant.

### Where to put it

- **Put the ESP32 near the base, not near the probe.** Aim for a base RSSI better than `-75 dBm`. Below roughly `-85 dBm` the link gets unreliable, and the `rssi_band` entity will tell you which side of that you are on without reading raw numbers.
- **Put the base where it can see the probe.** It is the half of the path that has to cross the cooker.
- **Keep both on the same subnet as your phone.** MEATER Link discovery relies on a UDP broadcast to `255.255.255.255:7878`. A guest VLAN, a separate SSID, or AP client isolation will stop the app finding the bridge even when the bridge is perfectly healthy.
- **Keep the board out of the heat and the weather.** A covered patio is fine; the shelf next to the firebox is not. Outdoors permanently means a sealed box with the antenna end clear.

The ESP32 shares one 2.4 GHz radio between Wi-Fi and Bluetooth, so a marginal Wi-Fi link can degrade BLE and vice versa. If readings are intermittent, check `wifi_rssi` before assuming the Bluetooth side is at fault.

### Software prerequisites

- PlatformIO Core, or the PlatformIO IDE extension for VS Code.

The project pins:

- PlatformIO Espressif32 `7.1.2`
- NimBLE-Arduino `2.5.1`
- PubSubClient `2.8`

## Configure

Runtime configuration in NVS is authoritative. `include/config.h` only seeds an empty NVS partition on first boot and remains gitignored.

### Authenticated captive portal

1. Connect USB serial at `115200` baud and boot the ESP32.
2. On an unconfigured board, or while holding the BOOT pin low during boot, the portal opens automatically.
3. Join the `MEATER-Bridge-xxxxxx` Wi-Fi network and open `http://192.168.4.1`.
4. Authenticate as user `admin` with password `Meater1234`.
5. Enter Wi-Fi, MQTT, TLS, OTA and BLE preferences, then select **Save and reboot**.
6. For an already configured bridge, finish within ten minutes because the reconfiguration AP then closes automatically.

Passwords are never prefilled in the page or printed in normal logs. A blank password field preserves its saved value. MQTT TLS requires a PEM CA certificate and never falls back to insecure TLS. OTA requires 12–128 printable characters containing letters and digits.

**Note:** The BOOT pin varies by board. On ESP32-C3, it's GPIO 9. On original ESP32 and ESP32-S3, it's GPIO 0. Check your board's pinout diagram.

| Runtime setting | Purpose |
|---|---|
| Wi-Fi SSID/password | Required 2.4 GHz network |
| MQTT host/port/user/password | Empty host disables MQTT |
| MQTT TLS + CA | CA-validated TLS, normally port `8883` |
| Node ID | Stable MQTT/HA identifier using letters, digits, `_` or `-` |
| Preferred MEATER MAC | Optional fixed base/probe MAC; also disables rotation |
| Prefer charger/base | Keeps the powered repeater path preferred |
| OTA password | Enables authenticated LAN OTA when strong enough |
| Physical cook writes | Visible only in a build whose compile-time gate is enabled |

### Compile-time defaults

1. Copy `include/config.example.h` to `include/config.h` only if you want first-boot defaults instead of portal-only provisioning.
2. Leave `MB_PROBE_WRITE_ENABLED` at `0` unless deliberately testing physical writes on expendable hardware.
3. To test physical writes, set `MB_PROBE_WRITE_ENABLED=1`, rebuild, then separately enable the runtime checkbox in the portal.

`MB_MAX_DEVICES`, `MB_MULTI_DEVICE_DWELL_MS`, `MB_MULTI_DEVICE_STALE_MS` and RSSI EMA constants remain compile-time resource/timing bounds.

## Quick flash (no build required)

If you just want to get running quickly, use a pre-built binary. No PlatformIO, no compilation, no toolchain setup.

### Option 1: Web-based flash (easiest)

Visit **[meater-bridge.github.io/flash](https://meater-bridge.github.io/flash)** in Chrome or Edge, connect your ESP32 via USB, and click **Install**. That's it.

> **Note:** Web Serial requires a Chromium-based browser (Chrome, Edge, Opera). Firefox and Safari are not supported.

### Option 2: Command-line flash with esptool

1. **Download the firmware** from the [latest release](https://github.com/arch12321/meater-bridge/releases/latest):
   - `meater-bridge-esp32c3.bin` for ESP32-C3 boards (default)
   - `meater-bridge-esp32.bin` for original ESP32 boards
   - `meater-bridge-esp32s3.bin` for ESP32-S3 boards

2. **Install esptool** (if you don't have it):
   ```bash
   pip install esptool
   ```

3. **Find your serial port**:
   ```bash
   # macOS
   ls /dev/cu.usb*

   # Linux
   ls /dev/ttyUSB* /dev/ttyACM*

   # Windows: check Device Manager for COM port
   ```

4. **Flash the firmware** (adjust the port and binary name):

   **ESP32-C3:**
   ```bash
   esptool.py --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 460800 \
     write_flash 0x0 meater-bridge-esp32c3.bin
   ```

   **Original ESP32:**
   ```bash
   esptool.py --chip esp32 --port /dev/cu.usbserial-0001 --baud 460800 \
     write_flash 0x10000 meater-bridge-esp32.bin
   ```

   **ESP32-S3:**
   ```bash
   esptool.py --chip esp32s3 --port /dev/cu.usbmodem2101 --baud 460800 \
     write_flash 0x0 meater-bridge-esp32s3.bin
   ```

5. **Reset the board** and continue to [Configure](#configure).

> **Troubleshooting:** If the flash fails, hold the **BOOT** button while pressing **RESET**, then try again. Release BOOT after the flash starts. On some boards you may need to hold BOOT throughout the entire flash process.

### Option 3: Build from source

For customization, multiple boards, or contributing changes, see [Build and flash](#build-and-flash) below.

---

## Build and flash

Flash first, then provision over the portal described in [Configure](#configure). A blank board has no credentials, so it comes up as its own access point rather than joining your network.

### 1. Install PlatformIO Core

```bash
pipx install platformio     # or: python3 -m pip install --user platformio
pio --version
```

### 2. Run the host tests

```bash
bash scripts/test_host.sh
```

Ten host binaries build with `-Wall -Wextra -Werror` and run off-target. They are quick and they fail fast, so run them before burning a flash cycle.

### 3. Build

```bash
pio run                    # builds the default environment
pio run -e esp32dev        # builds for a specific board
```

To change the default board permanently, edit the `default_envs` line in `platformio.ini`. See [Changing the target board](#changing-the-target-board) for details.

The first build pulls the toolchain and libraries and takes a few minutes. Partitioning is already handled: `board_build.partitions = min_spiffs.csv` in `platformio.ini` is what yields the two 1.875 MiB OTA slots, so there is no manual partition step. Expect roughly 60,164 bytes RAM (18.4%) and 1,254,920 bytes flash (63.8% of one slot).

### 4. Find the port and upload

Connect the board over USB and **close any running serial monitor first** — a monitor holding the port is the most common upload failure.

```bash
pio device list
pio run --target upload --upload-port /dev/cu.usbmodem2101
```

Substitute whatever `pio device list` reports. Boards with a native USB-JTAG interface usually appear as `/dev/cu.usbmodem*`; boards with a USB-to-UART bridge appear as `/dev/cu.usbserial-*`.

Most boards reset into the bootloader on their own. If yours doesn't, hold **BOOT**, tap **RESET**/**EN**, release **BOOT**, and upload again. The BOOT pin location varies by board (GPIO 9 on C3, GPIO 0 on original ESP32/S3).

### 5. Watch it boot

```bash
pio device monitor --port /dev/cu.usbmodem2101 --baud 115200
```

On an unconfigured board this is where the portal's AP name is printed, so keep the monitor open and continue at [Configure](#configure). A healthy run afterwards looks like [Expected startup sequence](#expected-startup-sequence). `Ctrl+C` exits.

Note that the BOOT pin does double duty: it is the pin for download mode, and holding it low during a normal boot is also what forces an already-configured bridge back into the configuration portal. The BOOT pin is GPIO 9 on ESP32-C3/C6, and GPIO 0 on original ESP32/S3.

### 6. Later updates

Once the board is provisioned and an OTA password is set, subsequent flashes can go over the LAN instead of USB, which matters when the bridge is living outside next to the cooker. OTA is password-authenticated for a trusted LAN and is not equivalent to Secure Boot or signed firmware.

## First-time setup with a MEATER+ charger/repeater

1. Use the official app normally first and confirm the probe is already paired through the charger/base.
2. End any active cook, but do not remove or unpair the probe in the app.
3. On Android, hold the MEATER app icon, open **App info**, then select **Force stop**.
4. Leave Android connected to the same Wi-Fi as the ESP32 and temporarily disable any VPN.
5. Leave the MEATER+ charger/base powered; do not remove its battery.
6. Place the ESP32 near the base, preferably with BLE RSSI better than `-75 dBm`.
7. If the probe is already out and the base is disconnected, dock it for 10 seconds.
8. Remove the probe from the powered base and place it near/in the cooker.
9. Wait for the base state to change to `connected (0)` and for the child probe ID to be logged.
10. Reopen the MEATER app only after the ESP32 has acquired the child probe.
11. Do not choose **Add Device** or pair again; MEATER Link discovery is automatic.
12. Wait 10–15 seconds for the existing paired probe to appear over Wi-Fi.

## Normal use

1. Power the MEATER+ charger/base and ESP32.
2. Keep the phone and ESP32 on the same LAN.
3. Force-stop the MEATER app if it currently owns the base's BLE connection.
4. Remove the probe from the powered base.
5. Wait for the ESP32 to connect to the base and resolve the child probe ID.
6. Open the MEATER app and use the existing paired probe.
7. Home Assistant receives the same readings independently through MQTT.

## Connection details

### Probe and charger/base BLE

A MEATER+ base advertises as device type `128`. The ESP32 connects to the base and subscribes to:

- relayed temperature notifications;
- probe/base battery values;
- base-to-probe connection state; and
- `MEATERPlusProbeInfo`, containing child probe number, 64-bit probe ID and firmware.

The child probe ID—not the base ID—is published to the MEATER app. This is what lets the app match the Wi-Fi data to the probe it already knows.

Typical base states include:

| Value | Meaning |
|---:|---|
| `0` | Probe connected |
| `1` | Probe disconnected |
| `2` | Base not paired with a probe |
| `3` | Fetching cook setup |
| `8` | Probe in charger |
| `9` | Probe upside down |
| `10` | Probe sleeping: no probe |
| `11` | Probe sleeping: no active cook |

When the state changes to connected/ready, the firmware automatically refreshes child identity and physical history; no ESP32 restart is required. The initial history read happens before changing live log mode so an existing cook's elapsed time is retained.

The ESP32 owns one BLE link at a time. When multiple eligible bases are seen and no fixed MAC is configured, it rotates after `MB_MULTI_DEVICE_DWELL_MS`, retains each device's last sample under an independent freshness timer, and revisits the least-recently-served endpoint. The first resolved child probe remains the official-app identity; later probes are MQTT/HA-only.

### ESP32 to MEATER app

- Transport: UDP broadcast/unicast on port `7878`.
- Protocol: protobuf-compatible MEATER Link `17.8`.
- The app broadcasts `SubscriptionMessage`; the bridge records the sender for 30 seconds.
- The bridge replies with `MasterMessage` state every reading and at least every 2.5 seconds.
- The bridge broadcasts state every five seconds to aid discovery.
- Incoming app `SetupMessage` state is echoed in later bridge messages.
- Tag `5` temperature-history requests receive bounded tag `6` responses sourced from the probe's physical log.
- `CookStatus.elapsedTime` uses recovered physical elapsed time, so a cook started before bridge handoff does not restart at zero.
- Android Bluetooth may remain enabled, but the ESP32 must acquire the base connection before the app opens.
- There is no manual Wi-Fi pairing page in the MEATER app.

Full recovered field details are in `docs/protocol.md`.

### ESP32 to Home Assistant

With default prefixes, the bridge publishes:

- state: `meater_bridge/meater_bridge/state`
- availability: `meater_bridge/meater_bridge/availability`
- discovery: `homeassistant/{component}/meater_bridge/{entity}/config`

Discovered entities include:

- Probe: tip, ambient, peak, target, battery, connection state, RSSI, ID, type and firmware.
- Base: battery, chip temperature, BLE connection, child state/code, RSSI, ID, type, BLE address and firmware.
- G2 diagnostics: individual internal sensors 1–5 and internal-sensor count.
- Cook: state, name, elapsed time, sequence and target.
- Bridge: Wi-Fi RSSI/channel/IP, MQTT connection, uptime, free heap and firmware.

The retained state JSON also includes raw 1/32 °C values and cook/base identifiers for downstream consumers. Additional probes use stable `meater_bridge_<id>` topic/device suffixes, independent RSSI EMA trackers and independent availability; stale temperatures become `null` rather than remaining current.

## Expected startup sequence

```text
Configured AP visible: channel=... RSSI=... security=...
Wi-Fi connected: channel=... RSSI=... IP=...
NimBLE Started!
MEATER candidate: type=128 path=charger/base RSSI=...
MEATER charger child state: connected (0)
MEATER charger child probe ready: id=... type=0 firmware=...
```

A repeated child-info line is harmless: setup and a state-triggered refresh can read the same identity twice.

## Troubleshooting

| Symptom | Action |
|---|---|
| `203 - ASSOC_FAIL`, then Wi-Fi connects | Transient association against one mesh AP; no action required |
| Configured SSID not visible | Confirm the same SSID exists on 2.4 GHz |
| Base not discovered | Force-stop the app, move ESP32 closer and confirm base power/battery |
| Base RSSI below about `-85 dBm` | Move ESP32 closer to the base; aim for better than `-75 dBm` |
| Child state `disconnected (1)` | Move base closer to probe/cooker; dock probe for 10 seconds then remove it |
| Child state `probe in charger (8)` | Remove the probe from the powered base |
| Base reports no child probe | Wait for state `connected (0)`; firmware then rereads probe info |
| App remains offline | Same LAN, VPN off, force-stop/reopen app, do not re-pair, disable AP client isolation |
| ESP32 reboots in `coex_core_enable()` | Use current firmware; Wi-Fi modem sleep must remain enabled for BLE coexistence |
| Upload says serial port busy | Stop `pio device monitor` with Ctrl+C before flashing |
| Cook elapsed remains `0` | Confirm serial logs a `MEATER history` line; reconnect after the base reports child state `connected (0)` |
| Portal asks for credentials | Log in as user `admin` with password `Meater1234` |
| MQTT TLS stays disconnected | Install the broker's issuing CA PEM, verify device time/NTP and use the TLS port |
| HA entities absent | Check MQTT host/credentials, broker availability and HA MQTT discovery |

## Validation status

Validated on target hardware:

- ESP32-C3 and ESP32 (original) PlatformIO release build with no compiler/linker warnings.
- Ten host test binaries compile with `-Wall -Wextra -Werror` and cover protobuf, MEATER Link history, physical 484/499-byte logs, cook-write payloads/gates, BLE supervision, freshness, RSSI, rotation and runtime validation.
- Wi-Fi association on a WPA2/WPA3 2.4 GHz mesh BSSID.
- MEATER+ charger/base discovery and GATT connection.
- Base-to-probe state `connected (0)`.
- Child probe identity `type=0`, firmware `v1.3.17_0`.
- Official Android app discovery and live connection over same-LAN Wi-Fi.
- MQTT retained state and Home Assistant discovery entities.

Still awaiting explicit validation:

- MEATER Pro/2 Plus G2 hardware.
- Temperature history and physical probe cook-setup writes.

Current build footprint: 60,164 bytes RAM (18.4%) and 1,254,920 bytes flash (63.8% of each 1.875 MiB OTA slot).

The firmware does not copy MEATER application code. It implements only the independently reconstructed interoperable wire fields needed to exchange device state.

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

This means you are free to:

- **Use** the software for any purpose
- **Study** how the software works and modify it
- **Distribute** copies of the original software
- **Distribute** copies of your modified versions

Under the following conditions:

- **Disclose source:** If you distribute the software or any derivative works, you must make the source code available under the same GPL-3.0 license.
- **Same license:** Any modifications or derivative works must also be licensed under GPL-3.0.
- **State changes:** If you modify the software, you must indicate that changes were made.
- **No additional restrictions:** You may not impose any further restrictions on the recipients' exercise of the rights granted.

The full license text is available in the [LICENSE](LICENSE) file.

For more information about GPL-3.0, see the [GNU GPL v3.0 page](https://www.gnu.org/licenses/gpl-3.0.html).
