#include "AppUI.h"
#include <Wire.h>

AppUI UI;

AppUI::AppUI() : u8g2(U8G2_R0, U8X8_PIN_NONE) {} 

void AppUI::begin() {
    Wire.begin(OLED_SDA, OLED_SCL); 
    u8g2.begin();
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
    pinMode(BTN_UP_PIN, INPUT_PULLUP);
    pinMode(BTN_DOWN_PIN, INPUT_PULLUP);
}

void AppUI::loop() {
    handleButtons();
    drawScreen();
}

void AppUI::handleButtons() {
    if (millis() - lastDebounce < 150) return;

    bool s = digitalRead(BTN_SELECT_PIN);
    bool u = digitalRead(BTN_UP_PIN);
    bool d = digitalRead(BTN_DOWN_PIN);

    PowerStatus st = PSU.getStatus();
    // 取得當前設定值作為操作基礎
    float tmpV = st.voltageSet;
    float tmpI = st.currentSet;
    bool changed = false;

    // SELECT 鍵: 切換模式
    if (s == LOW && lastSel == HIGH) {
        lastDebounce = millis();
        if (_mode == MODE_MONITOR) _mode = MODE_SET_VOLTAGE;
        else if (_mode == MODE_SET_VOLTAGE) _mode = MODE_SET_CURRENT;
        else _mode = MODE_MONITOR;
    }
    
    // UP 鍵
    if (u == LOW && lastUp == HIGH) {
        lastDebounce = millis();
        if (_mode == MODE_SET_VOLTAGE) { tmpV += 1.0; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI += 1.0; changed = true; } // 步進 1A
        if (_mode == MODE_MONITOR) { PSU.setPower(true); } // 手動開機
    }

    // DOWN 鍵
    if (d == LOW && lastDown == HIGH) {
        lastDebounce = millis();
        if (_mode == MODE_SET_VOLTAGE) { tmpV -= 1.0; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI -= 1.0; changed = true; }
        if (_mode == MODE_MONITOR) { PSU.setPower(false); } // 手動關機
    }

    // 數值邊界檢查
    if (tmpV < 0) tmpV = 0;
    if (tmpI < 0) tmpI = 0;

    if (changed) {
        PSU.setOutput(tmpV, tmpI);
    }

    lastSel = s; lastUp = u; lastDown = d;
}

void AppUI::drawScreen() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    PowerStatus st = PSU.getStatus();

    // --- Header ---
    u8g2.setCursor(0, 10);
    u8g2.print("Addr:"); u8g2.print(PSU_ADDRESS);
    
    // 顯示 ACK 狀態 (ACK 表示設定成功)
    u8g2.setCursor(50, 10);
    if(st.setCmdSuccess) u8g2.print("ACK"); 
    
    u8g2.setCursor(90, 10);
    if (st.isSoftStarting) u8g2.print("SOFT");
    else u8g2.print(st.isOn ? "ON" : "OFF");

    // --- Main Values ---
    u8g2.setFont(u8g2_font_profont17_tf);
    
    char buf[20];
    sprintf(buf, "V: %5.1f V", st.voltageOut);
    u8g2.drawStr(0, 30, buf);
    
    sprintf(buf, "I: %5.1f A", st.currentOut);
    u8g2.drawStr(0, 50, buf);

    // --- Footer Mode ---
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawHLine(0, 54, 128);
    u8g2.setCursor(0, 62);
    
    if (_mode == MODE_MONITOR) {
        u8g2.print("Set: "); u8g2.print(st.voltageSet, 0); u8g2.print("V ");
        u8g2.print(st.currentSet, 1); u8g2.print("A");
    } else if (_mode == MODE_SET_VOLTAGE) {
        u8g2.print(">> Set Volt: "); u8g2.print(st.voltageSet, 1);
    } else if (_mode == MODE_SET_CURRENT) {
        u8g2.print(">> Set Curr: "); u8g2.print(st.currentSet, 1);
    }

    u8g2.sendBuffer();
}