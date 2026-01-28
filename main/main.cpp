#include "hal_interface.h"
#include "psu_protocol.h"
#include "app_ui.h"
#include "serial_cmd.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 宣告在 port_esp32 中實作的 HAL 取得函數
extern IHardwareHAL* getHal();

extern "C" void app_main(void) {
    // 1. 取得硬體抽象層實體
    IHardwareHAL* hal = getHal();

    hal->init();
    
    hal->uartSend("System Starting...\r\n");

    // 2. 初始化核心邏輯模組
    // Dependency Injection: 將 HAL 注入到應用層
    PowerProtocol psu(hal);
    AppUI ui(hal, &psu);
    SerialCmd serial(hal, &psu);

    // 3. 模組初始化
    serial.begin();
    ui.begin();
    
    hal->delayMs(1500);
    psu.init(PSU_ADDRESS);
    hal->uartSend("PSU Initialized.\r\n");

    // 4. 主迴圈
    while (1) {
        psu.loop();
        ui.loop();
        serial.loop();
        
        // 釋放一點 CPU 給 FreeRTOS 的 IDLE task
        // 如果 loop 執行很快，加這個可以避免 Watchdog 觸發
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}