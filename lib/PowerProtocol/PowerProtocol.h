#ifndef POWER_PROTOCOL_H
#define POWER_PROTOCOL_H

#include <Arduino.h>
#include "CAN_Driver.h"

// 軟啟動參數
#define SOFT_START_INITIAL_CURRENT 10.0f 
#define SOFT_START_STEP_CURRENT    10.0f 
#define DEFAULT_TARGET_VOLTAGE     100.0f 
#define DEFAULT_TARGET_CURRENT     6.0f   

struct PowerStatus {
    float voltageOut; // V (實際讀回)
    float currentOut; // A (實際讀回)
    float voltageSet; // V (設定目標)
    float currentSet; // A (設定目標)

    float inputVoltage; // AC 輸入電壓
    
    bool isOn;        // [軟體控制狀態] 我們希望它開還是關
    bool hwRunning;   // [硬體回授狀態] 電源實際上有沒有在跑 (僅供顯示用)
    
    bool isSoftStarting; 
    bool setCmdSuccess;   
    bool powerCmdSuccess; 

    bool newInputVoltage;
    
    uint32_t lastUpdate;
};

class PowerProtocol {
public:
    void init(uint8_t addr);
    void loop(); 

    void setOutput(float voltageV, float currentA); 
    void setPower(bool on);                   
    
    void queryInputVoltage();
    void clearInputFlag() { _status.newInputVoltage = false ; }
    
    PowerStatus getStatus() { return _status; }

private:
    uint8_t _addr;
    PowerStatus _status;
    
    bool _startupCheckDone = false; 
    uint32_t _lastQueryTime = 0;
    
    // 軟啟動變數
    bool _softStartActive = false;
    float _targetVolts = 0.0;     
    float _targetAmps = 0.0;      
    float _rampingAmps = 0.0;     
    uint32_t _lastRampTime = 0;   
    
    const uint32_t ID_CMD_SET     = 0x1907C080; 
    const uint32_t ID_CMD_QUERY   = 0x1907C080; 
    const uint32_t ID_RESP_STATUS   = 0x1807C080;

    const uint32_t ID_CMD_QUERY_IN  = 0x1907A080; 
    const uint32_t ID_RESP_INPUT    = 0x1807A080; 

    void queryStatus();
    void sendSetCommand(float voltage, float current); 
    void parseFrame(uint32_t id, uint8_t* data, uint8_t len);
};

extern PowerProtocol PSU;

#endif