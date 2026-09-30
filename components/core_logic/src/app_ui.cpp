#include "app_ui.h"
#include "config_common.h"
#include <stdio.h>
#include <string.h>

AppUI::AppUI(IHardwareHAL* hal, PowerProtocol* psu)
    : _hal(hal), _psu(psu), _mode(MODE_MONITOR) {
    lastSel = false;
    lastUp = false;
    lastDown = false;
    lastDebounce = 0;
    _displayDirty = true;
    _lastDrawnMode = MODE_MONITOR;
    memset(&_cachedStatus, 0, sizeof(_cachedStatus));
    memset(&_lastDrawnStatus, 0, sizeof(_lastDrawnStatus));
    _cachedTransport = -1;
    _cachedPaired    = false;
    _cachedPairing   = false;
    _lastDrawnTransport = -1;
    _lastDrawnPaired    = false;
    _lastDrawnPairing   = false;
    _pair          = HalPairInfo();
    _lastDrawnPair = HalPairInfo();
    _pairScreen    = false;
}

// 配對中，或剛結束（完成顯示 3 s、失敗 5 s）
bool AppUI::pairScreenWanted(const HalPairInfo& p) {
    if (p.state >= 1 && p.state <= 3) return true;
    if (p.state == 4 && p.ageMs < 3000) return true;
    if (p.state == 5 && p.ageMs < 5000) return true;
    return _hal->isPairingActive();
}

// SEL = 兩邊配對碼相同；UP / DOWN = 取消。配對中這兩顆鍵不能照平常
// 開關輸出 —— 使用者看著配對碼按鍵，結果是 PSU 開始輸出。
void AppUI::handlePairButtons() {
    if (_hal->getTickCount() - lastDebounce < 150) return;
    bool s = _hal->readButton(BTN_SELECT);
    bool u = _hal->readButton(BTN_UP);
    bool d = _hal->readButton(BTN_DOWN);
    if (s && !lastSel && _pair.state == 3 && !_pair.localOk) {
        lastDebounce = _hal->getTickCount();
        _hal->pairAnswer(true);
    }
    if ((u && !lastUp) || (d && !lastDown)) {
        lastDebounce = _hal->getTickCount();
        if (_pair.state >= 1 && _pair.state <= 3) _hal->pairAnswer(false);
    }
    lastSel = s; lastUp = u; lastDown = d;
}

void AppUI::drawPairScreen() {
    char buf[32];
    _hal->displayClear();
    switch (_pair.state) {
    case 3:
        _hal->displayDrawString(0, 0, "PAIR WITH TES", 0);
        snprintf(buf, sizeof(buf), "%03lu %03lu",
                 (unsigned long)((_pair.code / 1000u) % 1000u), (unsigned long)(_pair.code % 1000u));
        _hal->displayDrawString(30, 18, buf, 1);
        if (_pair.localOk) {
            _hal->displayDrawString(0, 42, "Waiting for TES...", 0);
            _hal->displayDrawString(0, 54, "UP/DOWN = cancel", 0);
        } else {
            _hal->displayDrawString(0, 42, "Same on TES? SEL=yes", 0);
            _hal->displayDrawString(0, 54, "UP/DOWN = no", 0);
        }
        break;
    case 4:
        _hal->displayDrawString(0, 0, "PAIRED", 1);
        _hal->displayDrawString(0, 26, "TES linked securely", 0);
        break;
    case 5: {
        const char* why = "Error";
        switch (_pair.fail) {
        case 1: why = "Timed out";         break;
        case 2: why = "Cancelled";         break;
        case 3: why = "Cancelled on TES";  break;
        case 4: case 5: why = "Security check fail"; break;
        }
        _hal->displayDrawString(0, 0, "PAIRING FAILED", 0);
        _hal->displayDrawString(0, 18, why, 0);
        _hal->displayDrawString(0, 32, "Nothing was saved", 0);
        break;
    }
    default:
        _hal->displayDrawString(0, 0, "PAIRING...", 1);
        _hal->displayDrawString(0, 24, _pair.state == 2 ? "TES found..." : "Waiting for TES", 0);
        _hal->displayDrawString(0, 36, "Start pairing on TES", 0);
        _hal->displayDrawString(0, 54, "UP/DOWN = cancel", 0);
        break;
    }
    _hal->displayShow();
}

void AppUI::begin() {
    // HAL handles setup
}

void AppUI::loop() {
    _hal->getPairInfo(_pair);
    if (pairScreenWanted(_pair)) {
        handlePairButtons();
        _hal->getPairInfo(_pair);
        if (!_pairScreen || _pair.state != _lastDrawnPair.state || _pair.code != _lastDrawnPair.code ||
            _pair.localOk != _lastDrawnPair.localOk) {
            drawPairScreen();
            _lastDrawnPair = _pair;
        }
        _pairScreen = true;
        return;
    }
    if (_pairScreen) { _pairScreen = false; _displayDirty = true; }   // 回到一般畫面

    _cachedStatus    = _psu->getStatus();
    _cachedTransport = _hal->getTransport();
    _cachedPaired    = _hal->isPairedEspNow();
    _cachedPairing   = _hal->isPairingActive();

    if (_cachedStatus.voltageOut      != _lastDrawnStatus.voltageOut     ||
        _cachedStatus.currentOut      != _lastDrawnStatus.currentOut     ||
        _cachedStatus.voltageSet      != _lastDrawnStatus.voltageSet     ||
        _cachedStatus.currentSet      != _lastDrawnStatus.currentSet     ||
        _cachedStatus.isOn            != _lastDrawnStatus.isOn           ||
        _cachedStatus.isSoftStarting  != _lastDrawnStatus.isSoftStarting ||
        _cachedStatus.setCmdSuccess   != _lastDrawnStatus.setCmdSuccess  ||
        _cachedTransport != _lastDrawnTransport                          ||
        _cachedPaired    != _lastDrawnPaired                             ||
        _cachedPairing   != _lastDrawnPairing) {
        _displayDirty = true;
    }

    handleButtons();

    if (_displayDirty) {
        drawScreen();
        _lastDrawnStatus    = _cachedStatus;
        _lastDrawnMode      = _mode;
        _lastDrawnTransport = _cachedTransport;
        _lastDrawnPaired    = _cachedPaired;
        _lastDrawnPairing   = _cachedPairing;
        _displayDirty = false;
    }
}

void AppUI::handleButtons() {
    if (_hal->getTickCount() - lastDebounce < 150) return;

    bool s = _hal->readButton(BTN_SELECT);
    bool u = _hal->readButton(BTN_UP);
    bool d = _hal->readButton(BTN_DOWN);

    float tmpV = _cachedStatus.voltageSet;
    float tmpI = _cachedStatus.currentSet;
    bool changed = false;

    if (s && !lastSel) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_MONITOR) _mode = MODE_SET_VOLTAGE;
        else if (_mode == MODE_SET_VOLTAGE) _mode = MODE_SET_CURRENT;
        else _mode = MODE_MONITOR;
        _displayDirty = true;
    }

    if (u && !lastUp) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_SET_VOLTAGE) { tmpV += 1.0f; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI += 1.0f; changed = true; }
        if (_mode == MODE_MONITOR) { _psu->setPower(true); _displayDirty = true; }
    }

    if (d && !lastDown) {
        lastDebounce = _hal->getTickCount();
        if (_mode == MODE_SET_VOLTAGE) { tmpV -= 1.0f; changed = true; }
        if (_mode == MODE_SET_CURRENT) { tmpI -= 1.0f; changed = true; }
        if (_mode == MODE_MONITOR) { _psu->setPower(false); _displayDirty = true; }
    }

    if (tmpV < 0.0f) tmpV = 0.0f;
    if (tmpV > MAX_TARGET_VOLTAGE) tmpV = MAX_TARGET_VOLTAGE;
    if (tmpI < 0.0f) tmpI = 0.0f;
    if (tmpI > MAX_TARGET_CURRENT) tmpI = MAX_TARGET_CURRENT;

    if (changed) {
        _psu->setOutput(tmpV, tmpI);
        _displayDirty = true;
    }

    lastSel = s; lastUp = u; lastDown = d;
}

void AppUI::drawScreen() {
    _hal->displayClear();
    char buf[32];

    // Transport indicator: UA=UART, EN=ESP-NOW paired, E?=ESP-NOW unpaired, PR=pairing
    const char* tr;
    if (_cachedPairing)          tr = "PR";
    else if (_cachedTransport == 1) tr = (_cachedPaired ? "EN" : "E?");
    else                         tr = "UA";

    const char* pwr = _cachedStatus.isSoftStarting ? "SS"
                    : (_cachedStatus.isOn           ? "ON" : "--");

    snprintf(buf, sizeof(buf), "A%d %s%s [%s]",
             PSU_ADDRESS, pwr,
             _cachedStatus.setCmdSuccess ? "*" : " ",
             tr);
    _hal->displayDrawString(0, 0, buf, 0);

    snprintf(buf, sizeof(buf), "V: %5.1f V", _cachedStatus.voltageOut);
    _hal->displayDrawString(0, 20, buf, 1);

    snprintf(buf, sizeof(buf), "I: %5.1f A", _cachedStatus.currentOut);
    _hal->displayDrawString(0, 40, buf, 1);

    if (_mode == MODE_MONITOR) {
        snprintf(buf, sizeof(buf), "Set: %.0fV %.1fA", _cachedStatus.voltageSet, _cachedStatus.currentSet);
    } else if (_mode == MODE_SET_VOLTAGE) {
        snprintf(buf, sizeof(buf), ">> Set Volt: %.1f", _cachedStatus.voltageSet);
    } else if (_mode == MODE_SET_CURRENT) {
        snprintf(buf, sizeof(buf), ">> Set Curr: %.1f", _cachedStatus.currentSet);
    }
    _hal->displayDrawString(0, 56, buf, 0);

    _hal->displayShow();
}