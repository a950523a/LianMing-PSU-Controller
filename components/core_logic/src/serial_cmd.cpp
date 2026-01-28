#include "serial_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SerialCmd::SerialCmd(IHardwareHAL* hal, PowerProtocol* psu) 
    : _hal(hal), _psu(psu), _bufIndex(0), _lastReportTime(0) {
    memset(_inputBuffer, 0, BUF_SIZE);
}

void SerialCmd::begin() {
    // UART init is handled by HAL
}

void SerialCmd::loop() {
    // 1. Receive Char
    while (_hal->uartAvailable()) {
        int c = _hal->uartRead();
        if (c != -1) {
            if (c == '\n') {
                _inputBuffer[_bufIndex] = '\0';
                // Remove \r if present
                if (_bufIndex > 0 && _inputBuffer[_bufIndex-1] == '\r') {
                    _inputBuffer[_bufIndex-1] = '\0';
                }
                processCommand(_inputBuffer);
                _bufIndex = 0;
            } else if (_bufIndex < BUF_SIZE - 1) {
                _inputBuffer[_bufIndex++] = (char)c;
            }
        }
    }

    // 2. Periodic Report
    if (_hal->getTickCount() - _lastReportTime >= 100) {
        sendPeriodicReport();
        _lastReportTime = _hal->getTickCount();
    }

    if (_psu->getStatus().newInputVoltage) {
        char buf[32];
        snprintf(buf, sizeof(buf), "AC=%.1f\r\n", _psu->getStatus().inputVoltage);
        _hal->uartSend(buf);
        _psu->clearInputFlag();
    }
}

void SerialCmd::sendPeriodicReport() {
    PowerStatus st = _psu->getStatus();
    char buf[64];
    snprintf(buf, sizeof(buf), "V=%.1f,I=%.1f\r\n", st.voltageOut, st.currentOut);
    _hal->uartSend(buf);
}

void SerialCmd::processCommand(char* cmd) {
    // Simple parser
    if (strcmp(cmd, "ON") == 0) {
        _psu->setPower(true);
        _hal->uartSend("CMD_ACK:ON\r\n");
    } else if (strcmp(cmd, "OFF") == 0) {
        _psu->setPower(false);
        _hal->uartSend("CMD_ACK:OFF\r\n");
    } else if (strncmp(cmd, "SET:V=", 6) == 0) {
        float v = strtof(cmd + 6, NULL);
        _psu->setOutput(v, _psu->getStatus().currentSet);
        char buf[32];
        snprintf(buf, sizeof(buf), "CMD_ACK:SET_V:%.1f\r\n", v);
        _hal->uartSend(buf);
    } else if (strncmp(cmd, "SET:I=", 6) == 0) {
        float i = strtof(cmd + 6, NULL);
        _psu->setOutput(_psu->getStatus().voltageSet, i);
        char buf[32];
        snprintf(buf, sizeof(buf), "CMD_ACK:SET_I:%.1f\r\n", i);
        _hal->uartSend(buf);
    } else if (strcmp(cmd, "GET:AC") == 0) {
        _psu->queryInputVoltage();
        _hal->uartSend("CMD_ACK:QUERY_AC\r\n");
    }
}