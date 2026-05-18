# LianMing-PSU-Controller

**An open-source ESP32-based CAN Bus controller for LianMing (LM) Power Supply Units.**  
這是一個基於 ESP32 (ESP-IDF) 的開源控制器，專為深圳市聯明電源 (LianMing Power) 的整流模塊設計，透過 CAN Bus 協議實現遠端監控、電壓電流設定及軟啟動保護。

![Version](https://img.shields.io/badge/Version-v1.3.0-green)
![Framework](https://img.shields.io/badge/Framework-ESP--IDF%20v5.5.1-blue)
![License](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)

---

## 📖 專案簡介 (Overview)

本專案旨在解決 LM 系列高頻開關整流模塊（如 `LM48-6000AL`, `LM100-6000AL` 等）缺乏便捷控制介面的問題。  
採用 **ESP-IDF (v5.x)** 原生開發環境，以 **HAL (Hardware Abstraction Layer)** 分層架構將核心邏輯與硬體驅動完全分離。

### ✨ 主要功能 (Features)

- **⚡ 智慧軟啟動 (Smart Soft-Start)** — 開機限制 10A，偵測負載後平滑爬升至目標電流，保護繼電器與電池。
- **🖥️ OLED 狀態顯示** — 即時顯示電壓、電流、開關機狀態、傳輸模式及配對狀態。
- **🎛️ 本地按鍵控制** — 3 顆實體按鍵調整目標電壓與電流、開關機。
- **🌐 WiFi AP 網頁控制介面** — 連接 SSID `PSU-Controller` 後可透過瀏覽器即時監控與操控。
- **📦 OTA 韌體更新** — 透過網頁直接上傳 `.bin` 檔進行無線更新，不須拆機。
- **📡 雙傳輸模式** — 支援 **UART** 有線控制與 **ESP-NOW** 無線控制，可熱切換並持久化儲存。
- **🔌 廣泛相容** — 支援聯明電源 V2.0 CAN 通訊協議。

---

## 🛠️ 硬體架構 (Hardware)

- **MCU**: ESP32 NodeMCU-32S (or compatible)
- **CAN Transceiver**: TJA1051T/3, SN65HVD230, or VP230 (3.3V Logic)
- **Display**: 0.96" OLED (SSD1306, I2C Address `0x3C`)
- **Power Supply Module**: LM Power Module (LM100-6000AL tested)

### 接線定義 (Pinout)

| ESP32 Pin | 功能 | 說明 |
| :--- | :--- | :--- |
| **GPIO 5** | CAN TX | 接 CAN 收發器 TX |
| **GPIO 4** | CAN RX | 接 CAN 收發器 RX |
| **GPIO 21** | I2C SDA | OLED SDA |
| **GPIO 22** | I2C SCL | OLED SCL |
| **GPIO 16** | UART2 RX | Command Port（接外部控制器 TX） |
| **GPIO 17** | UART2 TX | Command Port（接外部控制器 RX） |
| **GPIO 1**  | UART0 TX | Debug Port（USB 串列監控） |
| **GPIO 3**  | UART0 RX | Debug Port（USB 串列監控） |
| **GPIO 12** | Button   | Select / Mode（Active Low） |
| **GPIO 13** | Button   | Up / ON（Active Low） |
| **GPIO 14** | Button   | Down / OFF（Active Low） |

> ⚠️ **注意**：ESP32 的 GND 必須與電源模塊 CAN GND (Pin 5) 共地。電源模塊硬體開關 (Pin 4) 需短接至 GND 才能接受控制。

---

## 💻 軟體安裝 (Installation)

### 前置需求
- ESP-IDF v5.5.1 或更高版本
- VS Code + ESP-IDF Extension（建議）

### 編譯與燒錄

```bash
git clone https://github.com/a950523a/LianMing-PSU-Controller.git
cd LianMing-PSU-Controller

idf.py set-target esp32   # 首次設定
idf.py build
idf.py -p COMx flash monitor
```

### OTA 無線更新（初次需清除 Flash）

```bash
# 首次使用 OTA 分割表，需清除一次 Flash
idf.py erase-flash
idf.py flash
```

後續更新：連接 WiFi `PSU-Controller`（密碼 `psu12345`），開啟 `http://192.168.4.1`，於 OTA 區塊上傳 `build/LianMing-PSU-Controller.bin`。

### 專案結構

```
main/                   程式入口 (app_main)
components/
  core_logic/           純 C++ 業務邏輯（零 ESP-IDF 依賴，可移植）
  port_esp32/           ESP32 HAL 實作（CAN/UART/WiFi/ESP-NOW/OLED）
  web_ctrl/             WiFi AP HTTP Server、OTA、REST API
  u8g2/                 OLED 圖形函式庫
```

---

## 📡 通訊協議 (Communication)

控制器支援三種控制介面，使用相同的指令語法：**UART**、**ESP-NOW**、**Web API**。

---

### 1. UART 指令介面（Command Port）

**硬體**：UART2，GPIO 16 (RX) / 17 (TX)，Baud Rate **115200**，8N1

#### 控制指令

| 指令 | 說明 | 回應 |
| :--- | :--- | :--- |
| `ON` | 開啟輸出（含軟啟動） | `CMD_ACK:ON` |
| `OFF` | 關閉輸出 | `CMD_ACK:OFF` |
| `SET:V=<value>` | 設定目標電壓（`0 ~ 120.0` V） | `CMD_ACK:SET_V:<value>` |
| `SET:I=<value>` | 設定目標電流（`0 ~ 100.0` A） | `CMD_ACK:SET_I:<value>` |
| `GET:AC` | 查詢 AC 輸入電壓 | `CMD_ACK:QUERY_AC`，非同步回傳 `AC=xxx.x` |
| `EQ:ON` | 啟用多模組 CAN 均流（廣播） | `CMD_ACK:EQ_ON` |
| `EQ:OFF` | 停用多模組 CAN 均流（廣播） | `CMD_ACK:EQ_OFF` |

#### 自動回報 (Telemetry)

| 條件 | 格式 | 週期 |
| :--- | :--- | :--- |
| 輸出電壓 ≥ 1V（運作中） | `V=xx.x,I=xx.x` | 每 100 ms |
| 輸出電壓 < 1V（待機） | `HB` | 每 1000 ms |
| AC 查詢結果（非同步） | `AC=xxx.x` | 收到 CAN 回應時 |

#### 錯誤回應

| 錯誤代碼 | 說明 |
| :--- | :--- |
| `ERR:V_OUT_OF_RANGE` | 電壓超出範圍或非數值 |
| `ERR:I_OUT_OF_RANGE` | 電流超出範圍或非數值 |

#### ESP-NOW 傳輸管理指令

| 指令 | 說明 | 回應 |
| :--- | :--- | :--- |
| `PAIR` | 開啟配對視窗（10 秒） | `CMD_ACK:PAIR` |
| `SET:TRANSPORT=0` | 切換至 UART 模式 | `CMD_ACK:SET_TRANSPORT` |
| `SET:TRANSPORT=1` | 切換至 ESP-NOW 模式 | `CMD_ACK:SET_TRANSPORT` |
| `STATUS:TRANSPORT` | 查詢目前傳輸模式 | `TRANSPORT=0` 或 `TRANSPORT=1` |
| `STATUS:PAIR` | 查詢配對與配對中狀態 | `PAIRED=1,PAIRING=0` |

---

### 2. ESP-NOW 無線傳輸

ESP-NOW 是 Espressif 的點對點 2.4GHz 無線協議，**不需要 Router**，延遲低於 1ms。啟用後，上述所有 UART 控制指令均可透過 ESP-NOW 傳送，TES（遠端控制端）也會以相同格式收到 Telemetry 回報。

#### 資料幀格式

ESP-NOW payload 為純 ASCII 字串（與 UART 指令完全相同格式）：

```
[指令字串]\n
```

**範例：**

| 方向 | Payload（ASCII） | 說明 |
| :--- | :--- | :--- |
| TES → PSU | `ON\n` | 開啟輸出 |
| TES → PSU | `SET:V=48.0\n` | 設定電壓 48V |
| TES → PSU | `SET:I=60.0\n` | 設定電流 60A |
| TES → PSU | `OFF\n` | 關閉輸出 |
| PSU → TES | `V=48.2,I=59.8\n` | 每 100ms 自動回報 |
| PSU → TES | `HB\n` | 待機心跳（每 1s） |
| PSU → TES | `AC=220.3\n` | AC 電壓查詢結果 |
| PSU → TES | `CMD_ACK:ON\n` | 指令確認 |

> **頻道**：PSU Controller 的 WiFi AP 固定在 **Channel 1**，TES 裝置需在同一頻道。

#### 配對流程 (Pairing Procedure)

ESP-NOW 需要預先知道對方 MAC Address 才能傳送。本專案使用「廣播握手」機制，**配對資訊存入 NVS，重開機後自動恢復**。

```
PSU Controller                         TES 裝置
      │                                    │
      │  觸發配對 (PAIR指令 / 網頁按鈕)      │
      │                                    │
      │──── ESP-NOW 廣播 ──────────────────▶│
      │    Payload: "PSU_HELLO\n"           │ ← 每 500ms 廣播一次
      │    Dst MAC:  FF:FF:FF:FF:FF:FF      │   持續 10 秒
      │                                    │
      │                          TES 收到廣播後
      │                          記下 PSU MAC
      │                          回傳任意封包
      │                                    │
      │◀─── ESP-NOW 單播 ──────────────────│
      │    Src MAC: TES MAC                 │
      │                                    │
      │  PSU 記下 TES MAC → 寫入 NVS       │
      │  配對完成！                         │
```

**觸發配對的三種方式：**

1. UART 指令：`PAIR\n`
2. 網頁：連接 `http://192.168.4.1` → ESP-NOW 傳輸卡片 → 點「開始配對 (10s 視窗)」
3. 配對完成後，OLED 第一行由 `[PR]` 變為 `[EN]`

**配對後切換到 ESP-NOW 模式：**

```
SET:TRANSPORT=1\n   ← UART 送出（配對完成後）
```

或從網頁點「切換 ESP-NOW」按鈕。

#### OLED 傳輸狀態指示

OLED 第一行右側以方括號顯示目前傳輸狀態：

| 顯示 | 說明 |
| :--- | :--- |
| `[UA]` | UART 模式 |
| `[EN]` | ESP-NOW 模式，已配對 |
| `[E?]` | ESP-NOW 模式，未配對（無 TES MAC） |
| `[PR]` | 配對視窗開啟中（倒數 10 秒） |

完整第一行格式：
```
A1 ON  [EN]   ← 輸出中，ESP-NOW 已配對
A1 SS* [UA]   ← 軟啟動中，收到 ACK，UART 模式
A1 --  [PR]   ← 待機，配對進行中
```

---

### 3. Web API

WiFi AP：SSID `PSU-Controller`，密碼 `psu12345`，IP `192.168.4.1`

#### GET /api/status

回傳 JSON 即時狀態：

```json
{
  "v":         48.2,
  "i":         59.8,
  "vset":      48.0,
  "iset":      60.0,
  "ac":        220.3,
  "on":        true,
  "softstart": false,
  "eq":        false,
  "version":   "v1.3.0",
  "transport": 1,
  "paired":    true,
  "pairing":   false,
  "peer_mac":  "aa:bb:cc:dd:ee:ff"
}
```

| 欄位 | 型別 | 說明 |
| :--- | :--- | :--- |
| `v` / `i` | float | 實際輸出電壓 / 電流 |
| `vset` / `iset` | float | 設定電壓 / 電流 |
| `ac` | float | AC 輸入電壓（0 = 未查詢） |
| `on` | bool | 輸出開啟狀態 |
| `softstart` | bool | 軟啟動進行中 |
| `transport` | int | `0` = UART，`1` = ESP-NOW |
| `paired` | bool | 是否已有已配對的 TES MAC |
| `pairing` | bool | 配對視窗是否開啟中 |
| `peer_mac` | string | TES MAC 位址（未配對時為 `"--"`） |

#### POST /api/cmd

Request body（JSON）：

| 指令 | Body | 說明 |
| :--- | :--- | :--- |
| 開啟輸出 | `{"cmd":"ON"}` | |
| 關閉輸出 | `{"cmd":"OFF"}` | |
| 設定 V/I | `{"cmd":"SET","v":48.0,"i":60.0}` | |
| 均流 ON/OFF | `{"cmd":"EQ_ON"}` / `{"cmd":"EQ_OFF"}` | |
| 查詢 AC | `{"cmd":"QUERY_AC"}` | |
| 開始配對 | `{"cmd":"PAIR"}` | |
| 切換傳輸 | `{"cmd":"SET_TRANSPORT","mode":1}` | `0`=UART, `1`=ESP-NOW |

#### POST /api/ota

Body：raw binary（`.bin` 韌體檔）。上傳完成後裝置自動重啟。

---

## 🏗️ 軟體架構 (Architecture)

```
main/main.cpp
    └── Esp32HAL (port_esp32) → 注入至：
            ├── PowerProtocol (core_logic) — CAN Bus、軟啟動 FSM
            ├── AppUI         (core_logic) — OLED 顯示、按鍵狀態機
            ├── SerialCmd     (core_logic) — UART/ESP-NOW 指令解析
            └── WebCtrl       (web_ctrl)   — HTTP Server、REST API、OTA
```

`core_logic/` 零 ESP-IDF 依賴，可直接移植至 STM32 等平台（僅需實作新的 `hal_impl.cpp`）。

---

## ⚠️ 免責聲明 (Disclaimer)

本專案涉及高壓直流電 (HVDC) 與高功率設備，操作不當可能導致觸電、火災或設備損壞。**請務必做好絕緣防護**，特別是在裸機測試時。作者不對因使用本專案造成的任何硬體損壞或人身傷害負責。

## 📜 授權 (License)

本專案採用 **CC BY-NC-SA 4.0**（姓名標示-非商業性-相同方式分享）授權。  
This project is licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

---
*Developed for Portable EV Charging Solutions.*
