#include "psu_protocol.h"

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
}

void PowerProtocol::setOutput(float voltageV, float currentA) {
    _targetVolts = voltageV;
    _targetAmps = currentA;
    _status.voltageSet = voltageV;
    _status.currentSet = currentA;

    if (_status.isOn && !_softStartActive) {
        sendSetCommand(_targetVolts, _targetAmps);
    } 
}

void PowerProtocol::sendSetCommand(float voltageV, float currentA) {
    HalCanFrame frame;
    frame.id = ID_CMD_SET + _addr;
    frame.len = 8;
    frame.ext = true;

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
                }
                _startupCheckDone = true; 
            } 
            _status.lastUpdate = _hal->getTickCount();
        } else if (cmdType == 0x02) {
            _status.powerCmdSuccess = (frame.data[1] != 0);
        } else {
            _status.setCmdSuccess = (frame.data[0] != 0);
        }
    }
    else if (frame.id == (ID_RESP_INPUT + _addr)) {
        if (frame.data[0] == 0x31) {
            uint16_t rawInput = (frame.data[2] << 8) | frame.data[3];
            _status.inputVoltage = rawInput / 32.0f;
            _status.newInputVoltage = true;
        }
    }
}