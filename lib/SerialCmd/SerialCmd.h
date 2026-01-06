#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include <Arduino.h>
#include "PowerProtocol.h"

class SerialCmd {
public:
    void begin();
    void loop();
private:
    String inputBuffer;
    uint32_t lastReportTime = 0; // 回報計時器

    void processCommand(String cmd);
    void sendPeriodicReport();   // 自動回報函數
};

extern SerialCmd SerialCom;

#endif