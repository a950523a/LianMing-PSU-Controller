#include "SerialCmd.h"

SerialCmd SerialCom;

void SerialCmd::begin() {
    // 初始化 Serial，使用 GPIO 16 (RX), 17 (TX)
    // 鮑率建議設高一點，例如 115200，確保 100ms 傳輸順暢
    Serial2.begin(115200, SERIAL_8N1, 16, 17); 
}

void SerialCmd::loop() {
    // 1. 處理接收
    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n') {
            processCommand(inputBuffer);
            inputBuffer = "";
        } else if (c != '\r') {
            inputBuffer += c;
        }
    }

    // 2. 自動回報 (100ms)
    // 格式 V=xx.x,I=xx.x
    if (millis() - lastReportTime >= 100) {
        sendPeriodicReport();
        lastReportTime = millis();
    }

    if (PSU.getStatus().newInputVoltage) {
        // 有新資料！透過 Serial 回報
        Serial2.printf("AC=%.1f\r\n", PSU.getStatus().inputVoltage);
        // 清除旗標，避免重複印
        PSU.clearInputFlag();
    }
}

void SerialCmd::sendPeriodicReport() {
    PowerStatus st = PSU.getStatus();
    // 僅回報數值，方便後端模組解析
    Serial2.printf("V=%.1f,I=%.1f\r\n", 
        st.voltageOut, 
        st.currentOut
    );
}

void SerialCmd::processCommand(String cmd) {
    cmd.toUpperCase();
    cmd.trim(); 

    if (cmd == "ON") {
        PSU.setPower(true);
        Serial2.println("CMD_ACK:ON");
    } else if (cmd == "OFF") {
        PSU.setPower(false);
        Serial2.println("CMD_ACK:OFF");
    } else if (cmd.startsWith("SET:V=")) {
        float v = cmd.substring(6).toFloat();
        // 保持電流不變，僅改電壓
        PSU.setOutput(v, PSU.getStatus().currentSet);
        Serial2.printf("CMD_ACK:SET_V:%.1f\n", v);
    } else if (cmd.startsWith("SET:I=")) {
        float i = cmd.substring(6).toFloat();
        // 保持電壓不變，僅改電流
        PSU.setOutput(PSU.getStatus().voltageSet, i);
        Serial2.printf("CMD_ACK:SET_I:%.1f\n", i);
    }
    else if (cmd == "GET:AC") {
        // 觸發查詢
        PSU.queryInputVoltage();
        // 這裡不直接回傳數值，因為資料還沒回來
        // 等 CAN 回來後，loop() 裡的第3步驟會自動回傳 "AC=xxx.x"
        // 我們這裡只回個 ACK
        Serial2.println("CMD_ACK:QUERY_AC");
    }
}