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

    PowerStatus _cachedStatus;
    PowerStatus _lastDrawnStatus;
    UIMode _lastDrawnMode;
    bool _displayDirty;

    int  _cachedTransport;
    bool _cachedPaired;
    bool _cachedPairing;
    int  _lastDrawnTransport;
    bool _lastDrawnPaired;
    bool _lastDrawnPairing;

    // ESP-NOW 配對畫面（數字比對）：配對中蓋過一般畫面，按鈕改成確認／取消
    HalPairInfo _pair;
    HalPairInfo _lastDrawnPair;
    bool        _pairScreen;

    void handleButtons();
    void drawScreen();
    bool pairScreenWanted(const HalPairInfo& p);
    void handlePairButtons();
    void drawPairScreen();
};

#endif