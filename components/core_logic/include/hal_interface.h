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
};

#endif // HAL_INTERFACE_H