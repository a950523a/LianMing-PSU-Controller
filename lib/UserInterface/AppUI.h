#ifndef APP_UI_H
#define APP_UI_H

#include <U8g2lib.h>
#include "Config.h"
#include "PowerProtocol.h"

enum UIMode {
    MODE_MONITOR,
    MODE_SET_VOLTAGE,
    MODE_SET_CURRENT
};

class AppUI {
public:
    AppUI();
    void begin();
    void loop();

private:
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
    
    UIMode _mode = MODE_MONITOR;
    
    // 按鍵狀態
    bool lastSel = HIGH, lastUp = HIGH, lastDown = HIGH;
    unsigned long lastDebounce = 0;

    void handleButtons();
    void drawScreen();
};

extern AppUI UI;

#endif