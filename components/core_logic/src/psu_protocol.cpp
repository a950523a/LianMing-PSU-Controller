#include "psu_protocol.h"
#include "power_limit.h"
#include "psu_link/psu_link.h"

PowerProtocol::PowerProtocol(IHardwareHAL* hal) : _hal(hal) {}

void PowerProtocol::init(uint8_t addr) {
    _addr = addr;
    memset(&_status, 0, sizeof(_status));
    
    _targetVolts = DEFAULT_TARGET_VOLTAGE; 
    _targetAmps  = DEFAULT_TARGET_CURRENT;    

    _status.voltageSet = _targetVolts;
    _status.currentSet = _targetAmps;

    _startupCheckDone = false;
    _softStartActive = false;
    _lastQueryTime = 0;
    _lastRampTime = 0;

    // 插座上限：存過就用存的，否則 15 A
    uint16_t da;
    _status.outletLimitDa = OUTLET_LIMIT_DEFAULT_DA;
    if (_hal->settingGetU16("outlet_da", &da) &&
        (da == 0 || (da >= OUTLET_LIMIT_MIN_DA && da <= OUTLET_LIMIT_MAX_DA)))
        _status.outletLimitDa = da;
    _status.acSource = PSU_AC_SRC_OUTLET;
    _status.currentEff = _targetAmps;
    _userAcQuery = false;
    _lastPowerCheck = 0;
    _lastSentAmps = 0.0f;
    // 第一次 AC 查詢在 2 秒後（等模組回第一筆狀態），之後每 AC_QUERY_PERIOD_MS 一次
    _lastAcQuery = _hal->getTickCount() - AC_QUERY_PERIOD_MS + 2000;
}

void PowerProtocol::loop() {
    HalCanFrame frame;
    
    // 1. CAN Receive
    while (_hal->canReceive(frame)) {
        parseFrame(frame);
    }

    uint32_t now = _hal->getTickCount();

    // 2. Soft Start
    if (_status.isOn && _softStartActive) {
        if (_status.currentOut > 1.0f) {
            if (now - _lastRampTime >= 100) {
                _lastRampTime = now;
                _rampingAmps += SOFT_START_STEP_CURRENT;

                if (_rampingAmps >= _targetAmps) {
                    _rampingAmps = _targetAmps;
                    _softStartActive = false; 
                    _status.isSoftStarting = false;
                }
                sendSetCommand(_targetVolts, _rampingAmps);
            }
        } else {
            _lastRampTime = now;
        }
    }

    // 3. Periodic Query (100ms)
    if (now - _lastQueryTime >= 100) {
        queryStatus();
        _lastQueryTime = now;
    }

    // 4. 功率上限跟著輸出電壓走：電池電壓上升，能給的電流就變少（反之亦然）
    if (now - _lastPowerCheck >= POWER_RECHECK_MS) {
        _lastPowerCheck = now;
        float a = pl_clamp_current(_targetAmps, limitVolts(), _status.powerLimitW);
        _status.currentEff = a;
        float d = a - _lastSentAmps;
        if (_status.isOn && !_softStartActive && (d >= POWER_RESEND_DELTA_A || d <= -POWER_RESEND_DELTA_A))
            sendSetCommand(_targetVolts, _targetAmps);
    }

    // 5. AC 輸入電壓：自動定期讀，算出可用功率（插到別的插座要重開機，但 AC 會掉電壓，照樣跟著走）
    if (now - _lastAcQuery >= AC_QUERY_PERIOD_MS) {
        _lastAcQuery = now;
        sendQueryInput();
    }
}

float PowerProtocol::limitVolts() const {
    return (_status.isOn && _status.voltageOut >= POWER_LIVE_V_MIN) ? _status.voltageOut : _targetVolts;
}

// 依 AC 電壓與插座上限重算；上限變了而且正在輸出，馬上把電流設定重送一次
void PowerProtocol::recomputeLimit() {
    uint16_t p = pl_power_limit_w(_status.inputVoltage, _status.outletLimitDa / 10.0f);
    bool changed = p != _status.powerLimitW;
    _status.powerLimitW = p;
    _status.currentEff  = pl_clamp_current(_targetAmps, limitVolts(), p);
    if (changed && _status.isOn && !_softStartActive) sendSetCommand(_targetVolts, _targetAmps);
}

bool PowerProtocol::setOutletLimit(uint16_t da) {
    if (da != 0 && (da < OUTLET_LIMIT_MIN_DA || da > OUTLET_LIMIT_MAX_DA)) return false;
    _status.outletLimitDa = da;
    _hal->settingSetU16("outlet_da", da);
    recomputeLimit();
    return true;
}

void PowerProtocol::setOutput(float voltageV, float currentA) {
    _targetVolts = voltageV;
    _targetAmps = currentA;
    _status.voltageSet = voltageV;
    _status.currentSet = currentA;
    _status.currentEff = pl_clamp_current(currentA, limitVolts(), _status.powerLimitW);

    if (_status.isOn && !_softStartActive) {
        sendSetCommand(_targetVolts, _targetAmps);
    } 
}

void PowerProtocol::sendSetCommand(float voltageV, float currentA) {
    HalCanFrame frame;
    frame.id = ID_CMD_SET + _addr;
    frame.len = 8;
    frame.ext = true;

    // 不管誰下的設定（TES、網頁、按鍵），都不超過這台現在能給的功率。
    // 用量到的輸出電壓：車子電壓低（例如 60 V 的車接在 84 V 的電源上）就能多給電流
    currentA = pl_clamp_current(currentA, limitVolts(), _status.powerLimitW);
    _lastSentAmps = currentA;
    uint32_t iVal = (uint32_t)(currentA * 1000);
    uint32_t vVal = (uint32_t)(voltageV * 1000);

    frame.data[0] = 0x00;
    frame.data[1] = (iVal >> 16) & 0xFF;
    frame.data[2] = (iVal >> 8) & 0xFF;
    frame.data[3] = iVal & 0xFF;
    frame.data[4] = (vVal >> 24) & 0xFF;
    frame.data[5] = (vVal >> 16) & 0xFF;
    frame.data[6] = (vVal >> 8) & 0xFF;
    frame.data[7] = vVal & 0xFF;

    _hal->canSend(frame);
}

void PowerProtocol::setPower(bool on) {
    HalCanFrame frame;
    frame.id = ID_CMD_SET + _addr;
    frame.len = 8;
    frame.ext = true;
    memset(frame.data, 0, 8);
    
    frame.data[0] = 0x02; // CMD=2
    frame.data[7] = on ? 0x55 : 0xAA;
    _hal->canSend(frame);

    _status.isOn = on;

    if (on) {
        _softStartActive = true;
        _status.isSoftStarting = true;
        if (_targetAmps <= 0.1f) {
             _rampingAmps = 0.0f; 
        } else {
             _rampingAmps = (_targetAmps > SOFT_START_INITIAL_CURRENT) ? SOFT_START_INITIAL_CURRENT : _targetAmps;
        }
        _lastRampTime = _hal->getTickCount();
        sendSetCommand(_targetVolts, _rampingAmps);
    } else {
        _softStartActive = false;
        _status.isSoftStarting = false;
    }
}

void PowerProtocol::queryStatus() {
    HalCanFrame frame;
    frame.id = ID_CMD_QUERY + _addr;
    frame.len = 8;
    frame.ext = true;
    memset(frame.data, 0, 8);
    frame.data[0] = 0x01;
    _hal->canSend(frame);
}

void PowerProtocol::queryInputVoltage() {
    _userAcQuery = true;
    sendQueryInput();
}

void PowerProtocol::sendQueryInput() {
    HalCanFrame frame;
    frame.id = ID_CMD_QUERY_IN + _addr;
    frame.len = 8;
    frame.ext = true;
    memset(frame.data, 0, 8);
    frame.data[0] = 0x31;
    _hal->canSend(frame);
}

void PowerProtocol::parseFrame(const HalCanFrame& frame) {
    if (frame.id == (ID_RESP_STATUS + _addr)) {
        uint8_t cmdType = frame.data[0];

        if (cmdType == 0x01) { 
            uint16_t rawI = (frame.data[2] << 8) | frame.data[3];
            uint16_t rawV = (frame.data[4] << 8) | frame.data[5];

            _status.currentOut = rawI / 10.0f;
            _status.voltageOut = rawV / 10.0f;
            
            bool hwIsOff = (frame.data[7] & 0x01);
            _status.hwRunning = !hwIsOff;

            if (!_startupCheckDone) {
                if (hwIsOff) {
                    setPower(true);
                } else {
                    _status.isOn = true;
                    _status.hwRunning = true;
                    _softStartActive = false;
                    _targetVolts = _status.voltageOut;
                    queryCurrentSetpoint();
                }
                _startupCheckDone = true;
            } 
            _status.lastUpdate = _hal->getTickCount();
        } else if (cmdType == 0x02) {
            _status.powerCmdSuccess = (frame.data[1] != 0);
        } else {
            _status.setCmdSuccess = (frame.data[1] != 0);
        }
    }
    else if (frame.id == (ID_RESP_INPUT + _addr)) {
        if (frame.data[0] == 0x31) {
            uint16_t rawInput = (frame.data[2] << 8) | frame.data[3];
            _status.inputVoltage = rawInput / 32.0f;
            if (_userAcQuery) {          // 定期自動讀的不回報，不然 UART 每 10 秒冒一行 AC=
                _status.newInputVoltage = true;
                _userAcQuery = false;
            }
            recomputeLimit();
        }
    }
    else if (frame.id == (ID_RESP_ISET + _addr)) {
        uint16_t rawI = (frame.data[0] << 8) | frame.data[1];
        _targetAmps = rawI / 10.0f;
        _status.currentSet = _targetAmps;
        _status.currentEff = pl_clamp_current(_targetAmps, limitVolts(), _status.powerLimitW);
    }
}

void PowerProtocol::queryCurrentSetpoint() {
    HalCanFrame frame;
    frame.id = ID_CMD_QUERY_ISET + _addr;
    frame.len = 0;
    frame.ext = true;
    memset(frame.data, 0, 8);
    _hal->canSend(frame);
}

void PowerProtocol::setEqualization(bool enable) {
    HalCanFrame frame;
    frame.id = ID_CMD_EQUALIZE;
    frame.len = 6;
    frame.ext = true;
    memset(frame.data, 0, 8);
    frame.data[3] = enable ? 0xAA : 0x55;
    _hal->canSend(frame);
}