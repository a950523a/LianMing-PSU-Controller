#include "hal_interface.h"
#include "psu_protocol.h"
#include "app_ui.h"
#include "serial_cmd.h"
#include "web_ctrl.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern IHardwareHAL* getHal();

extern "C" void app_main(void) {
    IHardwareHAL* hal = getHal();
    hal->init();

    hal->uartSend("System Starting...\r\n");

    PowerProtocol psu(hal);
    AppUI         ui(hal, &psu);
    SerialCmd     serial(hal, &psu);
    WebCtrl       web(&psu, hal);

    serial.begin();
    ui.begin();
    web.begin();   // HTTP server starts here (WiFi AP already up from hal->init())

    hal->delayMs(3000);
    psu.init(PSU_ADDRESS);
    hal->uartSend("PSU Initialized.\r\n");

    while (1) {
        psu.loop();
        ui.loop();
        serial.loop();
        web.processCommands();  // drain commands queued by web UI handlers

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}