#include "PowerProtocol.h"

PowerProtocol PSU;

void PowerProtocol::init(uint8_t addr) {
    _addr = addr;
    memset(&_status, 0, sizeof(_status));
    
    _targetVolts = DEFAULT_TARGET_VOLTAGE; 
    _targetAmps  = DEFAULT_TARGET_CURRENT;    

    _status.voltageSet = _targetVolts;
    _status.currentSet = _targetAmps;

    _startupCheckDone = false; 
}

void PowerProtocol::loop() {
    uint32_t id;
    uint8_t data[8];
    uint8_t len;
    
    // 1. 處理 CAN 接收
    while (CanBus.receive(id, data, len)) {
        parseFrame(id, data, len);
    }

    uint32_t now = millis();

    // 2. 軟啟動邏輯 (智慧偵測版)
    if (_status.isOn && _softStartActive) {
        
        // 關鍵修改：只有偵測到實際電流大於 1A (代表接觸器已吸合)，才開始爬升
        // 如果電流還沒出來，就一直維持在初始的 10A 等待
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
            // 電流還沒出來 (接觸器斷開)，保持計時器更新，避免一吸合就瞬間跳過好幾階
            _lastRampTime = now;
        }
    }

    // 3. 定時查詢狀態 (100ms)
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

    // 如果已經穩定運行（非軟啟動中），立即發送
    if (_status.isOn && !_softStartActive) {
        sendSetCommand(_targetVolts, _targetAmps);
    } 
}

void PowerProtocol::sendSetCommand(float voltageV, float currentA) {
    uint8_t data[8];
    uint32_t iVal = (uint32_t)(currentA * 1000);
    uint32_t vVal = (uint32_t)(voltageV * 1000);

    data[0] = 0x00; // CMD=0
    data[1] = (iVal >> 16) & 0xFF;
    data[2] = (iVal >> 8) & 0xFF;
    data[3] = iVal & 0xFF;
    data[4] = (vVal >> 24) & 0xFF;
    data[5] = (vVal >> 16) & 0xFF;
    data[6] = (vVal >> 8) & 0xFF;
    data[7] = vVal & 0xFF;

    CanBus.send(ID_CMD_SET + _addr, data, 8);
}

void PowerProtocol::setPower(bool on) {
    uint8_t data[8] = {0};
    data[0] = 0x02; // CMD=2
    data[7] = on ? 0x55 : 0xAA;
    CanBus.send(ID_CMD_SET + _addr, data, 8);

    _status.isOn = on;

    if (on) {
        // --- 觸發軟啟動 ---
        _softStartActive = true;
        _status.isSoftStarting = true;
        
        // 設定初始電流：10A (等待接觸器吸合)
        // 如果目標電流本身就小於 10A，那也沒關係，邏輯會直接設定到位
        if (_targetAmps <= 0.1f) {
             _rampingAmps = 0.0f; 
        } else {
             _rampingAmps = (_targetAmps > SOFT_START_INITIAL_CURRENT) ? SOFT_START_INITIAL_CURRENT : _targetAmps;
        }
        
        _lastRampTime = millis();
        
        // 立即發送這個 "安全電流" (10A)
        sendSetCommand(_targetVolts, _rampingAmps);
    } else {
        _softStartActive = false;
        _status.isSoftStarting = false;
    }
}

void PowerProtocol::queryStatus() {
    uint8_t data[8] = {0};
    data[0] = 0x01; // CMD=1
    CanBus.send(ID_CMD_QUERY + _addr, data, 8);
}

void PowerProtocol::queryInputVoltage() {
    uint8_t data[8] = {0};
    data[0] = 0x31; // 固定命令字 0x31 (PDF Page 4)
    // 傳送查詢指令 ID: 1907A080 + addr
    CanBus.send(ID_CMD_QUERY_IN + _addr, data, 8);
}

void PowerProtocol::parseFrame(uint32_t id, uint8_t* data, uint8_t len) {
    if (id == (ID_RESP_STATUS + _addr)) {
        uint8_t cmdType = data[0];

        if (cmdType == 0x01) { 
            // --- CMD=1 ACK ---
            uint16_t rawI = (data[2] << 8) | data[3];
            uint16_t rawV = (data[4] << 8) | data[5];

            _status.currentOut = rawI / 10.0f;
            _status.voltageOut = rawV / 10.0f;
            
            bool hwIsOff = (data[7] & 0x01);
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
            
            _status.lastUpdate = millis();

        } else if (cmdType == 0x02) {
            _status.powerCmdSuccess = (data[1] != 0);
        } else {
            _status.setCmdSuccess = (data[0] != 0);
        }
    }
    else if (id == (ID_RESP_INPUT + _addr)) {
        // PDF: Byte0=0x31, Byte2-3=Vab
        if (data[0] == 0x31) {
            uint16_t rawInput = (data[2] << 8) | data[3];
            // 公式: Value / 32
            _status.inputVoltage = rawInput / 32.0f;
            _status.newInputVoltage = true; // 舉旗通知 Serial
        }
    }
}