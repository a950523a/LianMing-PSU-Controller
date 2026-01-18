# LianMing-PSU-Controller

**An open-source ESP32-based CAN Bus controller for LianMing (LM) Power Supply Units.**  
這是一個基於 ESP32 的開源控制器，專為深圳市聯明電源 (LianMing Power) 的整流模塊設計，透過 CAN Bus 協議實現遠端監控、電壓電流設定及軟啟動保護。

![License](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)
![Platform](https://img.shields.io/badge/Platform-ESP32%20%7C%20Arduino-blue)

## 📖 專案簡介 (Overview)

本專案旨在解決 LM 系列高頻開關整流模塊（如 `LM48-6000AL`, `LM100-6000AL` 等）缺乏便捷控制介面的問題。透過 ESP32 與 CAN Transceiver，本控制器提供了 OLED 本地顯示與 UART 遠端控制功能，並特別針對**電池充電**應用加入了軟啟動邏輯與接觸器保護機制。

### ✨ 主要功能 (Features)

*   **⚡ 智慧軟啟動 (Smart Soft-Start)**: 
    *   開機時限制電流為 10A，等待後端接觸器吸合（偵測到負載電流）後，才平滑爬升至目標電流，保護繼電器與電池。
*   **🖥️ OLED 狀態顯示**: 
    *   即時顯示輸出電壓、電流、開關機狀態及軟啟動進度。
    *   顯示 CAN 通訊 ACK 確認狀態。
*   **🎛️ 本地按鍵控制**: 
    *   透過 3 顆實體按鍵調整目標電壓與電流。
*   **📡 UART 遠端控制**: 
    *   支援外部模組透過 Serial 指令 (`SET:V=...`, `GET:AC`) 進行自動化控制。
    *   支援 AC 輸入電壓查詢。
*   **🔌 廣泛相容**: 
    *   支援聯明電源 V2.0 CAN 通訊協議，理論上適用於該品牌絕大多數數位電源模塊。

## 🛠️ 硬體架構 (Hardware)

*   **MCU**: ESP32 NodeMCU-32S
*   **CAN Transceiver**: TJA1051T/3 或 SN65HVD230 或 VP230 (3.3V Logic)
*   **Display**: 0.96" OLED (SSD1306/SH1106 I2C)
*   **Power Supply**: LM Power Module (LM100-6000AL tested)

### 接線定義 (Pinout)

| ESP32 Pin | Function | Description |
| :--- | :--- | :--- |
| **GPIO 5** | CAN TX | Connect to Transceiver TX |
| **GPIO 4** | CAN RX | Connect to Transceiver RX |
| **GPIO 21** | I2C SDA | OLED SDA |
| **GPIO 22** | I2C SCL | OLED SCL |
| **GPIO 16** | UART RX2 | Remote Control RX |
| **GPIO 17** | UART TX2 | Remote Control TX |
| **GPIO 12** | Button | Select / Mode |
| **GPIO 13** | Button | Up / ON |
| **GPIO 14** | Button | Down / OFF |

> ⚠️ **注意**: 請務必確保 ESP32 的 GND 與電源模塊的 CAN GND (Pin 5) 共地。電源模塊的硬體開關 (Pin 4) 需短接至 GND 才能接受控制。

## 💻 軟體安裝 (Installation)

本專案使用 **PlatformIO** 進行開發。

1.  Clone 本專案：
    ```bash
    git clone https://github.com/YourUsername/LMPower-CAN-Controller.git
    ```
2.  使用 VS Code 開啟資料夾。
3.  等待 PlatformIO 自動下載依賴函式庫 (`U8g2`)。
4.  在 `include/Config.h` 中確認您的硬體設定。
5.  上傳程式碼至 ESP32。

## 📡 通訊協議 (UART Protocol)

控制器預設使用 `Serial2` (Baud 115200) 進行外部通訊。

*   **設定電壓**: `SET:V=100.0` (設定為 100V)
*   **設定電流**: `SET:I=50.0` (設定為 50A)
*   **開機**: `ON`
*   **關機**: `OFF`
*   **查詢 AC 電壓**: `GET:AC` (回傳 `AC=220.5`)
*   **自動回報**: 每 100ms 自動回傳 `V=xx.x,I=xx.x`

## ⚠️ 免責聲明 (Disclaimer)

*   本專案涉及高壓直流電 (HVDC) 與高功率設備，操作不當可能導致觸電、火災或設備損壞。
*   **請務必做好絕緣防護**，特別是在裸機測試時。
*   作者不對因使用本專案造成的任何硬體損壞或人身傷害負責。

## 📜 授權 (License)

本專案採用 **CC BY-NC-SA 4.0** (姓名標示-非商業性-相同方式分享) 授權。
This project is licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

---
**Developed for Portable EV Charging Solutions.**
