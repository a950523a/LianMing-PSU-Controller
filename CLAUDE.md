# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based CAN Bus controller for LianMing (LM) high-frequency switching rectifier PSU modules (e.g., LM48-6000AL, LM100-6000AL). Built with ESP-IDF v5.5.1+ (not Arduino). Licensed CC BY-NC-SA 4.0 (non-commercial).

## Build Commands

Requires ESP-IDF v5.5.1+ toolchain (`idf.py` on PATH).

```bash
# Configure target (first time only)
idf.py set-target esp32

# Build firmware
idf.py build

# Flash and open serial monitor (replace COMx with actual port)
idf.py -p COMx flash monitor

# Build only, no flash
idf.py build

# Clean build
idf.py fullclean
```

There is no test suite — validation is hardware-in-the-loop via serial monitor and CAN Bus inspection.

## Architecture

The project uses a **Hardware Abstraction Layer (HAL) pattern** with dependency injection, cleanly separating portable business logic from ESP32-specific drivers.

```
main/main.cpp
    └── Instantiates HalImpl (port_esp32) → injects into:
            ├── PowerProtocol (psu_protocol)   — CAN Bus messaging, soft-start FSM
            ├── AppUI (app_ui)                 — OLED display, 3-mode UI state machine
            └── SerialCmd (serial_cmd)         — UART command parser
```

### Components

**`components/core_logic/`** — Pure C++ business logic, **zero ESP-IDF dependencies**. Portable and hardware-agnostic.
- `include/hal_interface.h` — `IHardwareHAL` abstract base class; all hardware access goes through this interface
- `include/config_common.h` — Hardware-agnostic constants (PSU address, soft-start parameters, voltage/current defaults)
- `psu_protocol.{h,cpp}` — CAN Bus protocol, soft-start (initial 10A, +10A steps), periodic status queries
- `app_ui.{h,cpp}` — Button-driven UI with 3 modes: Monitor, Set Voltage, Set Current
- `serial_cmd.{h,cpp}` — UART command parser (`SET:V=`, `SET:I=`, `ON`, `OFF`, `GET:AC`)

**`components/port_esp32/`** — Concrete `IHardwareHAL` implementation using ESP-IDF drivers.
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

UART2 commands (115200 baud): `SET:V=100.0`, `SET:I=50.0`, `ON`, `OFF`, `GET:AC`  
Auto-report every 100ms: `V=xx.x,I=xx.x`

CAN Bus follows the LianMing rectifier module protocol (proprietary framing in `psu_protocol.cpp`).  
Official protocol spec: `docs/联明电源数字电源模块CAN通讯协议V2.0 (1).pdf` — cross-reference this when modifying CAN frame parsing.

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

## Portability Status

`core_logic/` has been verified to have **zero ESP-IDF dependencies** — all includes are standard C/C++ (`<stdint.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`). The HAL interface (`hal_interface.h`) uses only custom types (`HalCanFrame`, `HalButton`), no platform-specific types.

`main/main.cpp` uses FreeRTOS (`vTaskDelay`) and `app_main()` — this is expected as the platform entry point. FreeRTOS API is identical on STM32, so porting only requires a new `components/port_stm32/hal_impl.cpp`.

## Protocol Notes (Verified against docs/)

- `ID_CMD_SET == ID_CMD_QUERY == 0x1907C080` is **intentional** — CMD=0/1/2 share the same CAN ID, distinguished by `data[0]`. Confirmed against PDF examples.
- All CAN frame byte offsets in `parseFrame()` verified correct against spec examples.
- Voltage/current safety limits defined in `config_common.h` as `MAX_TARGET_VOLTAGE` / `MAX_TARGET_CURRENT`.
