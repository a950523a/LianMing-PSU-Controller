# LianMing-PSU-Controller (ESP-IDF Version)

**An open-source ESP32-based CAN Bus controller for LianMing (LM) Power Supply Units.**  
這是一個基於 ESP32 (ESP-IDF) 的開源控制器，專為深圳市聯明電源 (LianMing Power) 的整流模塊設計，透過 CAN Bus 協議實現遠端監控、電壓電流設定及軟啟動保護。

![Version](https://img.shields.io/badge/Version-v1-green)
![Framework](https://img.shields.io/badge/Framework-ESP--IDF%20v5.5.1-blue)
![License](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)

## 📖 專案簡介 (Overview)

本專案旨在解決 LM 系列高頻開關整流模塊（如 `LM48-6000AL`, `LM100-6000AL` 等）缺乏便捷控制介面的問題。

**v1 重大更新：**
本版本已從 Arduino 框架完全遷移至 **ESP-IDF (v5.x)** 原生開發環境，採用專業的 **HAL (Hardware Abstraction Layer)** 分層架構，將核心邏輯與硬體驅動完全分離，大幅提升穩定性與可移植性。

### ✨ 主要功能 (Features)

*   **⚡ 智慧軟啟動 (Smart Soft-Start)**: 
    *   開機時限制電流為 10A，等待後端接觸器吸合（偵測到負載電流）後，才平滑爬升至目標電流，保護繼電器與電池。
*   **🖥️ OLED 狀態顯示**: 
    *   使用 U8g2 函式庫驅動 SSD1306 OLED。
    *   即時顯示輸出電壓、電流、開關機狀態及軟啟動進度。
*   **🎛️ 本地按鍵控制**: 
    *   透過 3 顆實體按鍵調整目標電壓與電流。
*   **📡 雙 UART 架構**: 
    *   **Debug Port (UART0)**: 透過 USB 輸出系統日誌 (Log)。
    *   **Command Port (UART2)**: 專用指令介面，支援外部模組自動化控制 (`SET:V=...`, `GET:AC`)。
*   **🔌 廣泛相容**: 
    *   支援聯明電源 V2.0 CAN 通訊協議。

## 🛠️ 硬體架構 (Hardware)

*   **MCU**: ESP32 NodeMCU-32S (or compatible)
*   **CAN Transceiver**: TJA1051T/3, SN65HVD230, or VP230 (3.3V Logic)
*   **Display**: 0.96" OLED (SSD1306 I2C Address `0x3C`)
*   **Power Supply**: LM Power Module (LM100-6000AL tested)

### 接線定義 (Pinout)

| ESP32 Pin | Function | Description |
| :--- | :--- | :--- |
| **GPIO 5** | CAN TX | Connect to Transceiver TX |
| **GPIO 4** | CAN RX | Connect to Transceiver RX |
| **GPIO 21** | I2C SDA | OLED SDA |
| **GPIO 22** | I2C SCL | OLED SCL |
| **GPIO 16** | UART RX2 | **Command Port** (Connect to External Controller TX) |
| **GPIO 17** | UART TX2 | **Command Port** (Connect to External Controller RX) |
| **GPIO 1** | UART TX0 | **Debug Port** (USB Serial Monitor) |
| **GPIO 3** | UART RX0 | **Debug Port** (USB Serial Monitor) |
| **GPIO 12** | Button | Select / Mode (Active Low) |
| **GPIO 13** | Button | Up / ON (Active Low) |
| **GPIO 14** | Button | Down / OFF (Active Low) |

> ⚠️ **注意**: 請務必確保 ESP32 的 GND 與電源模塊的 CAN GND (Pin 5) 共地。電源模塊的硬體開關 (Pin 4) 需短接至 GND 才能接受控制。

## 💻 軟體安裝 (Installation)

本專案使用 **Espressif IoT Development Framework (ESP-IDF)** 進行開發。

### 前置需求
*   ESP-IDF v5.5.1 或更高版本 
*   VS Code + ESP-IDF Extension

### 編譯步驟

1.  Clone 本專案：
    ```bash
    git clone https://github.com/YourUsername/LianMing-PSU-Controller.git
    cd LianMing-PSU-Controller
    ```
2.  設定目標晶片：
    ```bash
    idf.py set-target esp32
    ```
3.  編譯專案：
    ```bash
    idf.py build
    ```
4.  燒錄並監控：
    ```bash
    idf.py -p COMx flash monitor
    ```

### 專案結構
*   `components/core_logic`: 純 C++ 業務邏輯 (無硬體依賴)。
*   `components/port_esp32`: ESP32 硬體驅動實作 (HAL Implementation)。
*   `components/u8g2`: 圖形函式庫。
*   `main`: 程式入口點。

## 📡 通訊協議 (UART Command Port)

控制器使用 **UART2** (GPIO 16/17, Baud 115200) 進行外部通訊。

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
