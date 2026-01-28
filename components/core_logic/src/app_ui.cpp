#include "app_ui.h"
#include <stdio.h>

AppUI::AppUI(IHardwareHAL* hal, PowerProtocol* psu) 
    : _hal(hal), _psu(psu), _mode(MODE_MONITOR) {
    lastSel = false; // Initial state assuming not pressed
    lastUp = false;
    lastDown = false;
    lastDebounce = 0;
}

void AppUI::begin() {
    // HAL handles setup
}

void AppUI::loop() {
    handleButtons();
    drawScreen();
}

void AppUI::handleButtons() {
    if (_hal->getTickCount() - lastDebounce < 150) return;

    bool s = _hal->readButton(BTN_SELECT);
    bool u = _hal->readButton(BTN_UP);
    bool d = _hal->readButton(BTN_DOWN);

    PowerStatus st = _psu->getStatus();
    float tmpV = st.voltageSet;
    float tmpI = st.currentSet;
    bool changed = false;

    // Detect Rising Edge (Press)
    if (s && !lastSel) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_MONITOR) _mode = MODE_SET_VOLTAGE;
        else if (_mode == MODE_SET_VOLTAGE) _mode = MODE_SET_CURRENT;
        else _mode = MODE_MONITOR;
    }

    if (u && !lastUp) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_SET_VOLTAGE) { tmpV += 1.0f; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI += 1.0f; changed = true; }
        if (_mode == MODE_MONITOR) { _psu->setPower(true); }
    }

    if (d && !lastDown) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_SET_VOLTAGE) { tmpV -= 1.0f; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI -= 1.0f; changed = true; }
        if (_mode == MODE_MONITOR) { _psu->setPower(false); }
    }

    if (tmpV < 0) tmpV = 0;
    if (tmpI < 0) tmpI = 0;

    if (changed) {
        _psu->setOutput(tmpV, tmpI);
    }

    lastSel = s; lastUp = u; lastDown = d;
}

void AppUI::drawScreen() {
    _hal->displayClear();
    PowerStatus st = _psu->getStatus();
    char buf[32];

    // Header
    snprintf(buf, sizeof(buf), "Addr:%d %s", PSU_ADDRESS, 
             st.isSoftStarting ? "SOFT" : (st.isOn ? "ON" : "OFF"));
    _hal->displayDrawString(0, 0, buf, 0); // Small Font
    
    if(st.setCmdSuccess) _hal->displayDrawString(50, 0, "ACK", 0);

    // Values
    snprintf(buf, sizeof(buf), "V: %5.1f V", st.voltageOut);
    _hal->displayDrawString(0, 20, buf, 1); // Large Font

    snprintf(buf, sizeof(buf), "I: %5.1f A", st.currentOut);
    _hal->displayDrawString(0, 40, buf, 1);

    // Footer
    if (_mode == MODE_MONITOR) {
        snprintf(buf, sizeof(buf), "Set: %.0fV %.1fA", st.voltageSet, st.currentSet);
    } else if (_mode == MODE_SET_VOLTAGE) {
        snprintf(buf, sizeof(buf), ">> Set Volt: %.1f", st.voltageSet);
    } else if (_mode == MODE_SET_CURRENT) {
        snprintf(buf, sizeof(buf), ">> Set Curr: %.1f", st.currentSet);
    }
    _hal->displayDrawString(0, 56, buf, 0);

    _hal->displayShow();
}