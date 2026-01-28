#ifndef APP_UI_H
#define APP_UI_H

#include "hal_interface.h"
#include "psu_protocol.h"

enum UIMode {
    MODE_MONITOR,
    MODE_SET_VOLTAGE,
    MODE_SET_CURRENT
};

class AppUI {
public:
    AppUI(IHardwareHAL* hal, PowerProtocol* psu);
    void begin();
    void loop();

private:
    IHardwareHAL* _hal;
    PowerProtocol* _psu;
    UIMode _mode;

    bool lastSel, lastUp, lastDown;
    uint32_t lastDebounce;

    void handleButtons();
    void drawScreen();
};

#endif