#ifndef WEB_CTRL_H
#define WEB_CTRL_H

#include "psu_protocol.h"
#include "hal_interface.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Command issued by the web UI; drained by processCommands() in the main loop.
struct WebCmd {
    enum Type : uint8_t {
        NONE, ON, OFF, SET_VI, EQ_ON, EQ_OFF, QUERY_AC, PAIR, SET_TRANSPORT
    } type;
    float v, i;
    int   transport_mode;
};

class WebCtrl {
public:
    WebCtrl(PowerProtocol* psu, IHardwareHAL* hal);
    void begin();
    // Call once per main-loop tick to execute any pending web commands.
    void processCommands();

    // Public so static C-style HTTP handlers (registered via user_ctx) can reach them.
    PowerProtocol*  _psu;
    IHardwareHAL*   _hal;
    QueueHandle_t   _cmdQueue;
};

#endif // WEB_CTRL_H
