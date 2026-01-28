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
    bool newInputVoltage;
    uint32_t lastUpdate;
};

class PowerProtocol {
public:
    PowerProtocol(IHardwareHAL* hal);
    void init(uint8_t addr);
    void loop();
    
    void setOutput(float voltageV, float currentA);
    void setPower(bool on);
    void queryInputVoltage();
    void clearInputFlag() { _status.newInputVoltage = false; }
    
    PowerStatus getStatus() const { return _status; }

private:
    IHardwareHAL* _hal;
    uint8_t _addr;
    PowerStatus _status;
    
    bool _startupCheckDone;
    uint32_t _lastQueryTime;
    
    // Soft Start
    bool _softStartActive;
    float _targetVolts;
    float _targetAmps;
    float _rampingAmps;
    uint32_t _lastRampTime;

    // CAN IDs
    static const uint32_t ID_CMD_SET     = 0x1907C080;
    static const uint32_t ID_CMD_QUERY   = 0x1907C080;
    static const uint32_t ID_RESP_STATUS = 0x1807C080;
    static const uint32_t ID_CMD_QUERY_IN  = 0x1907A080;
    static const uint32_t ID_RESP_INPUT    = 0x1807A080;

    void queryStatus();
    void sendSetCommand(float voltage, float current);
    void parseFrame(const HalCanFrame& frame);
};

#endif