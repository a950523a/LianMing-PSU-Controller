#include <Arduino.h>
#include "Config.h"
#include "CAN_Driver.h"
#include "PowerProtocol.h"
#include "AppUI.h"
#include "SerialCmd.h"

void setup() {
    // 1. 串口初始化
    Serial.begin(115200);
    Serial.println("Debug Port Started");

    // 2. CAN 初始化
    if (!CanBus.begin()) {
        Serial.println("Error: CAN Init Failed!");
        // 若 CAN 失敗，停滯在此或顯示錯誤
    } else {
        Serial.println("CAN Bus Initialized");
    }

    // 3. UI 初始化
    UI.begin();

    // 4. 串口命令模組初始化
    SerialCom.begin();

    // 5. 協議層初始化 (100V, 0A)
    delay(1500);
    PSU.init(PSU_ADDRESS);
}

void loop() {
    // 任務調度
    PSU.loop();       // 協議處理 (CAN 接收/軟啟動/查詢)
    UI.loop();        // 介面處理 (OLED/按鍵)
    SerialCom.loop(); // UART 處理 (接收指令/回報 V,I)

}