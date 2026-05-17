#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include "hal_interface.h"
#include "psu_protocol.h"

class SerialCmd {
public:
    SerialCmd(IHardwareHAL* hal, PowerProtocol* psu);
    void begin();
    void loop();

private:
    IHardwareHAL* _hal;
    PowerProtocol* _psu;
    
    static const int BUF_SIZE = 64;
    char _inputBuffer[BUF_SIZE];
    int _bufIndex;
    uint32_t _lastReportTime;  // last V/I telemetry sent (200ms interval)
    uint32_t _lastHbTime;      // last HB heartbeat sent  (1000ms interval)

    void processCommand(char* cmd);
};

#endif