# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based CAN Bus controller for LianMing (LM) high-frequency switching rectifier PSU modules (e.g., LM48-6000AL, LM100-6000AL). Built with ESP-IDF v5.5.1+ (not Arduino). Licensed CC BY-NC-SA 4.0 (non-commercial).

Current version: **v1.3.0** (linked to git tag via `CMakeLists.txt` — do not hardcode version strings anywhere in source).

## Build Commands

Requires ESP-IDF v5.5.1+ toolchain (`idf.py` on PATH).

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

- Tag a release: `git tag -a v1.x.0 -m "..."` then `git push origin vX.x.0`
- Between tags the version will be `vX.x.0-N-gHASH`; with uncommitted changes it appends `-dirty`
- `config_common.h` only defines a `#ifndef FIRMWARE_VERSION` fallback — never set it there directly

## Architecture

The project uses a **Hardware Abstraction Layer (HAL) pattern** with dependency injection, cleanly separating portable business logic from ESP32-specific drivers.

```
main/main.cpp
    └── Instantiates HalImpl (port_esp32) → injects into:
            ├── PowerProtocol (psu_protocol)   — CAN Bus messaging, soft-start FSM
            ├── AppUI (app_ui)                 — OLED display, 3-mode UI state machine
            ├── SerialCmd (serial_cmd)         — UART command parser
            └── WebCtrl (web_ctrl)             — HTTP server, REST API, OTA upload
```

### Components

**`components/web_ctrl/`** — ESP32-specific HTTP server and OTA update service.
- `include/web_ctrl.h` / `src/web_ctrl.cpp` — `WebCtrl` class; `begin()` starts the HTTP server; `processCommands()` drains the web command queue (must be called from main loop)
- REST API: `GET /api/status` → JSON, `POST /api/cmd` → JSON command, `POST /api/ota` → raw `.bin` OTA upload
- WiFi AP: SSID `PSU-Controller`, password `psu12345`, URL `http://192.168.4.1`
- OTA: uploads to the next OTA partition, sets it as boot, then restarts. Requires `partitions.csv` (dual OTA layout)

**`components/core_logic/`** — Pure C++ business logic, **zero ESP-IDF dependencies**. Portable and hardware-agnostic.
- `include/hal_interface.h` — `IHardwareHAL` abstract base class; all hardware access goes through this interface
- `include/config_common.h` — Hardware-agnostic constants (PSU address, soft-start parameters, voltage/current limits)
- `psu_protocol.{h,cpp}` — CAN Bus protocol, soft-start FSM (10A initial, +10A steps), periodic status queries, equalization control
- `app_ui.{h,cpp}` — Button-driven UI with 3 modes: Monitor, Set Voltage, Set Current; dirty-flag OLED rendering
- `serial_cmd.{h,cpp}` — UART command parser with input validation

**`components/port_esp32/`** — Concrete `IHardwareHAL` implementation using ESP-IDF drivers. WiFi runs in `WIFI_MODE_APSTA`: AP interface (192.168.4.1) serves the web UI; STA interface is used by ESP-NOW.
- `include/port_def.h` — Pin assignments and hardware constants
- `src/hal_impl.cpp` — Drives CAN (TWAI), UART2, I2C, GPIO, timers, and OLED via U8g2

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

UART2 commands (115200 baud):

| Command | Description |
|---|---|
| `SET:V=100.0` | Set output voltage (0 – MAX_TARGET_VOLTAGE) |
| `SET:I=50.0` | Set current limit (0 – MAX_TARGET_CURRENT) |
| `ON` / `OFF` | Power on/off with soft-start |
| `GET:AC` | Query AC input voltage (replies `AC=xxx.x`) |
| `EQ:ON` / `EQ:OFF` | Enable/disable multi-module CAN equalization (broadcast) |

Auto-report every 100ms: `V=xx.x,I=xx.x`  
Invalid range or non-numeric input replies: `ERR:V_OUT_OF_RANGE` / `ERR:I_OUT_OF_RANGE`

CAN Bus follows the LianMing rectifier module protocol (proprietary framing in `psu_protocol.cpp`).  
Official protocol spec: `docs/联明电源数字电源模块CAN通讯协议V2.0 (1).pdf` — cross-reference this when modifying CAN frame parsing.

### Safety Limits

Defined in `config_common.h` — adjust here for different PSU models:
- `MAX_TARGET_VOLTAGE 120.0f`
- `MAX_TARGET_CURRENT 100.0f`

Both serial commands and UI button increments are clamped to these limits.

## Development Environment

Two supported setups:
1. **Dev Container** — `.devcontainer/devcontainer.json` provisions a Docker image with ESP-IDF v5.x at `/opt/esp/idf`. Requires Docker + VS Code Dev Containers extension.
2. **Local** — Install ESP-IDF v5.5.1+ manually; VS Code with ESP-IDF extension recommended. IntelliSense is configured via `.clangd` and `.vscode/c_cpp_properties.json`.

`sdkconfig` is committed and tracks the ESP-IDF SDK configuration. Run `idf.py menuconfig` to change it interactively; avoid hand-editing.

## Design Conventions

- All hardware access **must** go through `IHardwareHAL` — never call ESP-IDF drivers directly from `core_logic/`.
- `core_logic/` must remain free of ESP-IDF includes so it stays portable.
- New hardware peripherals: add virtual methods to `hal_interface.h`, implement in `hal_impl.cpp`, update `port_def.h` for pin assignments.
- New UI modes: extend the state machine in `app_ui.cpp` and update `AppUIMode` enum.
- `AppUI::loop()` caches `PowerStatus` once per tick in `_cachedStatus`; use this in `handleButtons()` and `drawScreen()` — do not call `_psu->getStatus()` directly inside those methods.
- OLED is only redrawn when `_displayDirty` is true; set this flag on any state or value change.

## Portability Status

`core_logic/` has been verified to have **zero ESP-IDF dependencies** — all includes are standard C/C++ (`<stdint.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`). The HAL interface (`hal_interface.h`) uses only custom types (`HalCanFrame`, `HalButton`), no platform-specific types.

`main/main.cpp` uses FreeRTOS (`vTaskDelay`) and `app_main()` — this is expected as the platform entry point. FreeRTOS API is identical on STM32, so porting only requires a new `components/port_stm32/hal_impl.cpp`.

## Protocol Notes (Verified against docs/)

- `ID_CMD_SET == ID_CMD_QUERY == 0x1907C080` is **intentional** — CMD=0/1/2 share the same CAN ID, distinguished by `data[0]`. Confirmed against PDF examples.
- All CAN frame byte offsets in `parseFrame()` verified correct against spec examples.
- Multi-module equalization: broadcast ID `0x19C21880`, 6-byte frame, `data[3] = 0xAA` (on) / `0x55` (off). No reply expected.
- Startup sequence: first status response auto-detects PSU state; if already ON, queries current setpoint via `0x19010880` to sync `_targetAmps`.
