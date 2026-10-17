#ifndef PSU_PROTOCOL_H
#define PSU_PROTOCOL_H

#include "hal_interface.h"
#include "config_common.h"
#include <string.h> // for memset

struct PowerStatus {
    float voltageOut;
    float currentOut;
    float voltageSet;
    float currentSet;
    float inputVoltage;
    bool isOn;
    bool hwRunning;
    bool isSoftStarting;
    bool setCmdSuccess;
    bool powerCmdSuccess;
    bool newInputVoltage;       // 人下的 GET:AC／網頁按鈕的結果到了（定期自動讀的不算）
    uint32_t lastUpdate;
    // 可用功率（power_limit.h）：TES 經 $PWR 拿到同一組數字
    uint16_t powerLimitW;       // 現在能給的最大 DC 功率；0 = AC 還沒讀到、不限
    uint16_t outletLimitDa;     // 插座電流上限（0.1 A）；0 = 不限
    uint8_t  acSource;          // psu_ac_src_t（現在只有插座；之後接 J1772／Type-2 會有 AC 樁）
    float    currentEff;        // 實際送給模組的電流設定（被功率上限收過的）
};

class PowerProtocol {
public:
    PowerProtocol(IHardwareHAL* hal);
    void init(uint8_t addr);
    void loop();
    
    void setOutput(float voltageV, float currentA);
    void setPower(bool on);
    void setEqualization(bool enable);
    void queryInputVoltage();   // 人下的查詢：結果會設 newInputVoltage
    // 插座電流上限（0.1 A；0 = 不限，否則 OUTLET_LIMIT_MIN_DA–MAX_DA）。存起來，馬上重算功率上限
    bool setOutletLimit(uint16_t da);
    void clearInputFlag() { _status.newInputVoltage = false; }
    
    PowerStatus getStatus() const { return _status; }

private:
    IHardwareHAL* _hal;
    uint8_t _addr;
    PowerStatus _status;
    
    bool _startupCheckDone;
    uint32_t _lastQueryTime;
    uint32_t _lastAcQuery;
    bool     _userAcQuery;
    uint32_t _lastPowerCheck;
    float    _lastSentAmps;
    
    // Soft Start
    bool _softStartActive;
    float _targetVolts;
    float _targetAmps;
    float _rampingAmps;
    uint32_t _lastRampTime;

    // CAN IDs
    static const uint32_t ID_CMD_SET       = 0x1907C080;
    static const uint32_t ID_CMD_QUERY     = 0x1907C080;
    static const uint32_t ID_RESP_STATUS   = 0x1807C080;
    static const uint32_t ID_CMD_QUERY_IN  = 0x1907A080;
    static const uint32_t ID_RESP_INPUT    = 0x1807A080;
    static const uint32_t ID_CMD_QUERY_ISET  = 0x19010880;
    static const uint32_t ID_RESP_ISET       = 0x18010880;
    static const uint32_t ID_CMD_EQUALIZE    = 0x19C21880;

    void queryStatus();
    void queryCurrentSetpoint();
    void sendSetCommand(float voltage, float current);
    void sendQueryInput();
    void recomputeLimit();
    float limitVolts() const;   // 算功率上限用的電壓：輸出中用量到的，否則用設定值
    void parseFrame(const HalCanFrame& frame);
};

#endif