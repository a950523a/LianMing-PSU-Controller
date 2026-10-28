#ifndef HAL_INTERFACE_H
#define HAL_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

// 定義 CAN 訊息結構
struct HalCanFrame {
    uint32_t id;
    uint8_t data[8];
    uint8_t len;
    bool ext; // true for extended frame
};

// 定義按鍵索引
enum HalButton {
    BTN_SELECT = 0,
    BTN_UP,
    BTN_DOWN
};

// ESP-NOW 配對進度（數字比對，見 psu_link/psu_pair.h），給 OLED 與網頁顯示
struct HalPairInfo {
    uint8_t  state;        // psu_pair_state_t：0 閒置、1 搜尋、2 交換金鑰、3 比對配對碼、4 完成、5 失敗
    uint8_t  fail;         // psu_pair_fail_t
    uint32_t code;         // state 3 / 4 時有效，顯示成 6 位數
    bool     localOk;      // 這邊已按確認
    bool     peerOk;       // TES 那邊已按確認
    uint32_t ageMs;        // 進入目前狀態多久
    bool     linkAuth;     // 跟配對的 TES 已完成握手，訊息驗證中
    bool     needsRepair;  // 只有舊版配對（MAC、沒有金鑰）—— 要重新配對
};

// HAL 介面定義
class IHardwareHAL {
public:
    virtual ~IHardwareHAL() {}

    virtual void init() = 0; 

    // System
    virtual uint32_t getTickCount() = 0; // 回傳毫秒 (ms)
    virtual void delayMs(uint32_t ms) = 0;

    // GPIO
    virtual bool readButton(HalButton btn) = 0; // 回傳 true 表示按下 (處理 Active Low)

    // CAN Bus
    virtual bool canSend(const HalCanFrame& frame) = 0;
    virtual bool canReceive(HalCanFrame& frame) = 0;

    // UART (Serial)
    virtual void uartSend(const char* str) = 0;
    virtual int uartRead() = 0; // 回傳 -1 表示無資料
    virtual int uartAvailable() = 0;

    // Display (OLED) - 簡化版介面
    virtual void displayClear() = 0;
    virtual void displayDrawString(int x, int y, const char* str, int fontSize) = 0; // fontSize: 0=Small, 1=Large
    virtual void displayShow() = 0;

    // Transport (UART=0 / ESP-NOW=1) — no-op defaults keep core_logic portable
    virtual void triggerPairing() {}
    virtual bool isPairingActive() { return false; }
    virtual void setTransport(int mode) { (void)mode; }
    virtual int  getTransport() { return 0; }
    virtual bool isPairedEspNow() { return false; }
    virtual void getEspNowPeerMac(char* buf, size_t len) { if (len > 0) buf[0] = '\0'; }

    // 無線傳輸的收尾工作（配對、握手、驗證收到的訊息）。主迴圈每輪呼叫一次，
    // 在 SerialCmd::loop() 之前 —— uartRead() 只會讀到驗證通過的行。
    virtual void transportPoll() {}
    virtual void getPairInfo(HalPairInfo& info) { info = HalPairInfo(); }
    // 使用者對配對碼的回應：accept = 兩邊數字相同；配對碼出來前傳 false = 取消
    virtual void pairAnswer(bool accept) { (void)accept; }

    // TES 用 $CHN 告訴我們它的 WiFi 頻道（ESP-NOW 用，2026-10-18）：不在那個頻道就切過去
    virtual void followTesChannel(uint8_t ch) { (void)ch; }
    // 本機識別碼（$INF 的 <uid>，2026-10-28）：寫進 out、回傳位元組數；0 = 沒有（就不送 INF）
    virtual size_t getUniqueId(uint8_t* out, size_t max) { (void)out; (void)max; return 0; }

    // TES 用 $NET 告訴我們它在哪個網路（BSSID＋SSID，2026-10-26）：記下來，下次找 TES 時先掃描這個網路的頻道
    virtual void rememberTesNetwork(const uint8_t* bssid, const uint8_t* ssid, uint8_t ssid_len) {
        (void)bssid; (void)ssid; (void)ssid_len;
    }
    // 目前頻道；TES 要我們去、但切不過去的頻道（手機連著熱點時切不了），0 = 沒有
    virtual uint8_t getLinkChannel() { return 0; }
    virtual uint8_t getBlockedChannel() { return 0; }

    // 存在掉電後還留著的地方（ESP32：NVS）的小設定。沒有就回 false，呼叫端用預設值
    virtual bool settingGetU16(const char* key, uint16_t* out) { (void)key; (void)out; return false; }
    virtual void settingSetU16(const char* key, uint16_t value) { (void)key; (void)value; }
};

#endif // HAL_INTERFACE_H