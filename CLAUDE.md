# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based CAN Bus controller for LianMing (LM) high-frequency switching rectifier PSU modules (e.g., LM48-6000AL, LM100-6000AL). Built with ESP-IDF v5.5.5 (not Arduino). Licensed CC BY-NC-SA 4.0 (non-commercial).

Current version: **v1.3.1** (linked to git tag via `CMakeLists.txt` — do not hardcode version strings anywhere in source).

## Current Status (2026-09-24)

**Hardware is still a perfboard prototype** on a NodeMCU-32S — no custom PCB yet.

**Link protocol v2 — built, not hardware-tested.** `SerialCmd` speaks the PSU-Link
protocol (`components/psu_link` submodule, see **CAN / UART Protocol** below). Both this
firmware and the TES controller's build under IDF 5.5.5, but no frame has crossed a real
wire yet. The TES side is on its `dev` branch only, waiting for the same test. What to
check: `$CAP` arrives at the TES side on boot / on `$HELO`; `$ST` every 1 s idle and
100 ms while outputting, V/I matching the OLED; `$SET` answered by `$ACK,<seq>,0` and the
output really changing; all of it again over ESP-NOW.

**First flash after 2026-09-24 needs a full erase.** Builds now use the OTA partition
table (`sdkconfig.defaults`, see **Development Environment**); a board that was flashed
with the old single-app table must get `idf.py erase-flash` once. That also clears NVS,
so the transport setting and ESP-NOW pairing have to be redone — do it together with the
protocol test above.

**Flash size is unknown.** The config assumes 2 MB, which is safe either way: a 2 MB
layout runs on a 4 MB chip, not the other way round. NodeMCU-32S modules normally carry
4 MB, but that is not verified — `python -m esptool flash_id` with the board on USB
prints "Detected flash size". If it is 4 MB, raise `CONFIG_ESPTOOLPY_FLASHSIZE_*` in
`sdkconfig.defaults` and grow `ota_0` / `ota_1` to `0x1E0000` each; OTA headroom is only
about 7 % today (app 0xDFC00 / slot 0xF0000).

**Possible future role, not decided:** the protocol also covers a *measurement-only*
node — for power supplies with no digital interface — which reports V/I and accepts no
setpoints. If that node is built on the same ESP32 board, this firmware could gain a
second mode rather than a second repository. See the TES controller's CLAUDE.md,
**Current Status**, for why that node is wanted.

**Fixed 2026-09-24:** `core_logic/` and `port_esp32/` had `CmakeLists.txt` (wrong
case). ESP-IDF looks for `CMakeLists.txt`; it only built because Windows file names are
case-insensitive, and would have broken on Linux (e.g. CI). If you ever rename one of
these on Windows, go through a temporary name — a case-only `git mv` is a no-op there.

**Known, not fixed:**
- `hal_impl.cpp:435` warns about a missing `allow_pd` initializer in the `uart_config_t`
  built at line 426 (`-Wmissing-field-initializers`). Harmless; name the field or
  zero-initialise the struct to silence it.

## Build Commands

Requires ESP-IDF **v5.5.5** (`idf.py` on PATH) — the same version as the TES charging controller, on purpose: both ends of the link are built and debugged together, so bump both or neither. Local checkout: `C:\Users\user\esp\v5.5.5\esp-idf`. After switching IDF versions delete `build/` — the CMake cache pins the old toolchain path.

```bash
# Configure target (first time only)
idf.py set-target esp32

# Build firmware
idf.py build

# Flash and open serial monitor (replace COMx with actual port)
idf.py -p COMx flash monitor

# Force CMake re-configure (e.g. after tagging a new version)
idf.py reconfigure

# Clean build
idf.py fullclean
```

There is no test suite — validation is hardware-in-the-loop via serial monitor and CAN Bus inspection.

## OTA Update Workflow

1. Build firmware: `idf.py build`
2. **First-time OTA setup** (partition table changed from single-app → dual OTA):
   ```bash
   idf.py erase-flash   # required once to clear old partition table
   idf.py flash
   ```
3. Subsequent OTA: connect to WiFi `PSU-Controller` (pw: `psu12345`), open `http://192.168.4.1`, upload `build/LianMing-PSU-Controller.bin` via the OTA section.
4. Device reboots and runs new firmware. The previous slot is kept as rollback (future).

`partitions.csv` defines the dual-OTA layout (two × 960KB slots, fits 2MB flash). If hardware has 4MB flash, enlarge `ota_0`/`ota_1` to `0x1E0000` each for more headroom.

## Versioning

`FIRMWARE_VERSION` is injected at build time by `CMakeLists.txt` via `git describe --tags --always --dirty`. CMake auto-reconfigures when `.git/HEAD` changes (new commit or branch switch).

- Tag a release: `git tag -a v1.x.y -m "..."` then `git push origin v1.x.y`
- Between tags the version will be `vX.x.y-N-gHASH`; with uncommitted changes it appends `-dirty`
- `config_common.h` only defines a `#ifndef FIRMWARE_VERSION` fallback — never set it there directly

## Architecture

The project uses a **Hardware Abstraction Layer (HAL) pattern** with dependency injection, cleanly separating portable business logic from ESP32-specific drivers.

```
main/main.cpp
    └── Instantiates HalImpl (port_esp32) → injects into:
            ├── PowerProtocol (psu_protocol)   — CAN Bus messaging, soft-start FSM
            ├── AppUI (app_ui)                 — OLED display, 3-mode UI state machine
            ├── SerialCmd (serial_cmd)         — UART/ESP-NOW command parser
            └── WebCtrl (web_ctrl)             — HTTP server, REST API, OTA upload
```

### Components

**`components/web_ctrl/`** — ESP32-specific HTTP server and OTA update service.
- `include/web_ctrl.h` / `src/web_ctrl.cpp` — `WebCtrl(PowerProtocol*, IHardwareHAL*)` class; `begin()` starts the HTTP server; `processCommands()` drains the web command queue (must be called from main loop)
- REST API: `GET /api/status` → JSON, `POST /api/cmd` → JSON command, `POST /api/ota` → raw `.bin` OTA upload
- WiFi AP: SSID `PSU-Controller`, password `psu12345`, URL `http://192.168.4.1`
- OTA: uploads to the next OTA partition, sets it as boot, then restarts. Requires `partitions.csv` (dual OTA layout)
- WebCmd types: `ON`, `OFF`, `SET_VI`, `EQ_ON`, `EQ_OFF`, `QUERY_AC`, `PAIR`, `SET_TRANSPORT`

**`components/core_logic/`** — Pure C++ business logic, **zero ESP-IDF dependencies**. Portable and hardware-agnostic.
- `include/hal_interface.h` — `IHardwareHAL` abstract base class; all hardware access goes through this interface
- `include/config_common.h` — Hardware-agnostic constants (PSU address, soft-start parameters, voltage/current limits)
- `psu_protocol.{h,cpp}` — CAN Bus protocol, soft-start FSM (10A initial, +10A steps), periodic status queries, equalization control
- `app_ui.{h,cpp}` — Button-driven UI with 3 modes: Monitor, Set Voltage, Set Current; dirty-flag OLED rendering; caches transport/pairing state for display
- `serial_cmd.{h,cpp}` — UART/ESP-NOW command parser with input validation

**`components/port_esp32/`** — Concrete `IHardwareHAL` implementation using ESP-IDF drivers. WiFi runs in `WIFI_MODE_APSTA`: AP interface (192.168.4.1) serves the web UI; STA interface is used by ESP-NOW.
- `include/port_def.h` — Pin assignments and hardware constants
- `src/hal_impl.cpp` — Drives CAN (TWAI), UART2, I2C, GPIO, timers, OLED via U8g2, and ESP-NOW

**`components/u8g2/`** — U8g2 OLED graphics library (git submodule, SSD1306 support).

### Key Hardware Pins (ESP32 NodeMCU-32S)

| Function | GPIO |
|---|---|
| CAN TX | 5 |
| CAN RX | 4 |
| OLED SDA | 22 |
| OLED SCL | 21 |
| UART2 TX | 17 |
| UART2 RX | 16 |
| Button Select | 12 |
| Button Up/ON | 13 |
| Button Down/OFF | 14 |

Buttons are active-low. OLED I2C address is 0x3C. Debug output is on UART0 (USB). Control commands arrive on UART2 at 115200 baud.

### CAN / UART Protocol

The command port (UART2 at 115200, or ESP-NOW — same bytes either way) carries two
things, told apart by the first character of each line:

- **`$…` — the link protocol with the TES charging controller.** Defined in the
  `components/psu_link` submodule ([PSU-Link](https://github.com/a950523a/PSU-Link),
  `psu_link/psu_link.h`) — the TES firmware uses the same submodule, so never change the
  wire format here; change it in PSU-Link and bump the submodule pointer in both repos.
  Fresh clone: `git submodule update --init`. `SerialCmd` answers `$HELO`
  with `$CAP`, applies `$SET` and answers `$ACK`, and sends `$ST` every 100 ms while
  outputting / every 1 s idle (that is also the heartbeat). Every line ends in a CRC-16;
  bad lines are dropped.
- **Anything else — human text commands** (table below). Replies are text, which the TES
  controller ignores, so a terminal and the TES board can share the port.

Two deliberate choices in `serial_cmd.cpp`: below 1 V output the `$ST` voltage/current
are flagged invalid (the TES side then uses its own ADC — same boundary as the old
`V=`/`HB` split), and the node does **not** declare `PSU_CAP_REPORT_MODE`, because the
LianMing CAN protocol does not report CV/CC and guessing it from setpoints would feed the
controller wrong information.

| Command | Description |
|---|---|
| `SET:V=100.0` | Set output voltage (0 – MAX_TARGET_VOLTAGE) |
| `SET:I=50.0` | Set current limit (0 – MAX_TARGET_CURRENT) |
| `ON` / `OFF` | Power on/off with soft-start |
| `GET:AC` | Query AC input voltage (replies `AC=xxx.x`) |
| `EQ:ON` / `EQ:OFF` | Enable/disable multi-module CAN equalization (broadcast) |
| `PAIR` | Open ESP-NOW pairing window (10 s) |
| `SET:TRANSPORT=0` | Switch to UART mode (persisted in NVS) |
| `SET:TRANSPORT=1` | Switch to ESP-NOW mode (persisted in NVS) |
| `STATUS:TRANSPORT` | Query current transport mode → `TRANSPORT=0` or `TRANSPORT=1` |
| `STATUS:PAIR` | Query pairing state → `PAIRED=1,PAIRING=0` |

`SET:V=` / `SET:I=` remain for manual testing from a terminal; the TES controller uses `$SET`.  
Invalid range or non-numeric input replies: `ERR:V_OUT_OF_RANGE` / `ERR:I_OUT_OF_RANGE`

CAN Bus follows the LianMing rectifier module protocol (proprietary framing in `psu_protocol.cpp`).  
Official protocol spec: `docs/联明电源数字电源模块CAN通讯协议V2.0 (1).pdf` — cross-reference this when modifying CAN frame parsing.

### ESP-NOW Transport

The controller supports switching between UART and ESP-NOW as the command transport. The ESP-NOW payload format is **identical to UART** — plain ASCII strings terminated with `\n`. Transport mode and paired TES MAC are persisted in NVS and restored on boot.

**Pairing flow** (broadcast handshake):
1. Trigger with `PAIR\n` (UART) or web UI button.
2. PSU broadcasts `PSU_HELLO\n` to `FF:FF:FF:FF:FF:FF` every 500 ms for 10 s.
3. TES receives the broadcast, records PSU MAC, and sends back any packet.
4. PSU receives the reply, records TES MAC, saves to NVS — pairing complete.

**OLED first-line transport indicators** (right-side bracket):
- `[UA]` — UART mode
- `[EN]` — ESP-NOW, paired
- `[E?]` — ESP-NOW, no TES MAC yet
- `[PR]` — pairing window open

**ESP-NOW send callback** (`espnow_send_cb`): uses the ESP-IDF v5.5 signature `(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)` — destination MAC is in `tx_info->des_addr`. This is a breaking API change from older ESP-IDF versions.

### HAL Interface — Transport Methods

Defined in `hal_interface.h` with no-op defaults so `core_logic/` stays portable:

| Method | Description |
|---|---|
| `triggerPairing()` | Open 10 s pairing window |
| `isPairingActive()` | True while pairing window is open |
| `setTransport(int)` | 0 = UART, 1 = ESP-NOW (saves to NVS) |
| `getTransport()` | Returns current transport mode |
| `isPairedEspNow()` | True if a TES MAC is stored |
| `getEspNowPeerMac(char*, size_t)` | Write formatted MAC string (needs ≥ 18 bytes); empty string if unpaired |

### Web API

`GET /api/status` JSON fields (relevant to transport):

| Field | Type | Description |
|---|---|---|
| `transport` | int | 0 = UART, 1 = ESP-NOW |
| `paired` | bool | TES MAC is known |
| `pairing` | bool | Pairing window is open |
| `peer_mac` | string | TES MAC address (`"--"` if unpaired) |

`POST /api/cmd` accepts `{"cmd":"PAIR"}` and `{"cmd":"SET_TRANSPORT","mode":0}`.

### Safety Limits

Defined in `config_common.h` — adjust here for different PSU models:
- `MAX_TARGET_VOLTAGE 120.0f`
- `MAX_TARGET_CURRENT 100.0f`

Both serial commands and UI button increments are clamped to these limits.

## Development Environment

Two supported setups:
1. **Dev Container** — `.devcontainer/devcontainer.json` provisions a Docker image with ESP-IDF v5.x at `/opt/esp/idf`. Requires Docker + VS Code Dev Containers extension.
2. **Local** — Install ESP-IDF v5.5.5 manually; VS Code with ESP-IDF extension recommended. IntelliSense is configured via `.clangd` and `.vscode/c_cpp_properties.json`.

`sdkconfig` is **not** committed — it is gitignored and regenerated per machine. Anything that is not an ESP-IDF default must go in **`sdkconfig.defaults`** (committed), or a fresh clone silently builds with defaults. That is how the partition table went wrong until 2026-09-24: `partitions.csv` was committed, but "custom partition table" only ever lived in a local `sdkconfig`, so every build used the single-app table and web OTA answered "No OTA partition".

To change a setting: `idf.py menuconfig`, then `idf.py save-defconfig` to see the non-default list, and merge it into `sdkconfig.defaults` by hand (that command overwrites the file and drops its comments). Existing local `sdkconfig` values win over `sdkconfig.defaults` — delete the local `sdkconfig` after editing the defaults to regenerate it.

**OTA headroom is tight:** the app is 0xDFC00 bytes against a 0xF0000 slot — about 7 % free on 2026-09-24. When it runs out, the fix is a bigger slot (needs a board with more than 2 MB flash; see the partition note above) or `-Os` instead of the default `-Og`, not dropping features.

## Design Conventions

- All hardware access **must** go through `IHardwareHAL` — never call ESP-IDF drivers directly from `core_logic/`.
- `core_logic/` must remain free of ESP-IDF includes so it stays portable.
- New hardware peripherals: add virtual methods to `hal_interface.h` with no-op defaults, implement in `hal_impl.cpp`, update `port_def.h` for pin assignments.
- New UI modes: extend the state machine in `app_ui.cpp` and update `AppUIMode` enum.
- `AppUI::loop()` caches `PowerStatus` once per tick in `_cachedStatus`, and transport/pairing state in `_cachedTransport` / `_cachedPaired` / `_cachedPairing`; use these in `handleButtons()` and `drawScreen()` — do not call HAL methods directly inside those methods.
- OLED is only redrawn when `_displayDirty` is true; set this flag on any state or value change, including transport/pairing state changes.
- `WebCtrl` takes both `PowerProtocol*` and `IHardwareHAL*` — required for transport/pairing commands and status reporting.

## Portability Status

`core_logic/` has been verified to have **zero ESP-IDF dependencies** — all includes are standard C/C++ (`<stdint.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`). The HAL interface (`hal_interface.h`) uses only custom types (`HalCanFrame`, `HalButton`), no platform-specific types.

`main/main.cpp` uses FreeRTOS (`vTaskDelay`) and `app_main()` — this is expected as the platform entry point. FreeRTOS API is identical on STM32, so porting only requires a new `components/port_stm32/hal_impl.cpp`.

## Protocol Notes (Verified against docs/)

- `ID_CMD_SET == ID_CMD_QUERY == 0x1907C080` is **intentional** — CMD=0/1/2 share the same CAN ID, distinguished by `data[0]`. Confirmed against PDF examples.
- All CAN frame byte offsets in `parseFrame()` verified correct against spec examples.
- Multi-module equalization: broadcast ID `0x19C21880`, 6-byte frame, `data[3] = 0xAA` (on) / `0x55` (off). No reply expected.
- Startup sequence: first status response auto-detects PSU state; if already ON, queries current setpoint via `0x19010880` to sync `_targetAmps`.
