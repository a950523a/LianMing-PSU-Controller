#include "serial_cmd.h"
#include "config_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SerialCmd::SerialCmd(IHardwareHAL* hal, PowerProtocol* psu)
    : _hal(hal), _psu(psu), _bufIndex(0), _lastReportTime(0), _lastHbTime(0) {
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

    // 2. Telemetry: V/I @ 200ms when outputting, HB @ 1000ms when standby
    PowerStatus st = _psu->getStatus();
    uint32_t now   = _hal->getTickCount();
    bool outputting = (st.voltageOut >= 1.0f);

    if (outputting) {
        if (now - _lastReportTime >= 100) {
            char buf[32];
            snprintf(buf, sizeof(buf), "V=%.1f,I=%.1f\n", st.voltageOut, st.currentOut);
            _hal->uartSend(buf);
            _lastReportTime = now;
        }
    } else {
        if (now - _lastHbTime >= 1000) {
            _hal->uartSend("HB\n");
            _lastHbTime = now;
        }
    }

    if (_psu->getStatus().newInputVoltage) {
        char buf[32];
        snprintf(buf, sizeof(buf), "AC=%.1f\r\n", _psu->getStatus().inputVoltage);
        _hal->uartSend(buf);
        _psu->clearInputFlag();
    }
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
        char* end;
        float v = strtof(cmd + 6, &end);
        if (end == cmd + 6 || v < 0.0f || v > MAX_TARGET_VOLTAGE) {
            _hal->uartSend("ERR:V_OUT_OF_RANGE\r\n");
            return;
        }
        _psu->setOutput(v, _psu->getStatus().currentSet);
        char buf[32];
        snprintf(buf, sizeof(buf), "CMD_ACK:SET_V:%.1f\r\n", v);
        _hal->uartSend(buf);
    } else if (strncmp(cmd, "SET:I=", 6) == 0) {
        char* end;
        float i = strtof(cmd + 6, &end);
        if (end == cmd + 6 || i < 0.0f || i > MAX_TARGET_CURRENT) {
            _hal->uartSend("ERR:I_OUT_OF_RANGE\r\n");
            return;
        }
        _psu->setOutput(_psu->getStatus().voltageSet, i);
        char buf[32];
        snprintf(buf, sizeof(buf), "CMD_ACK:SET_I:%.1f\r\n", i);
        _hal->uartSend(buf);
    } else if (strcmp(cmd, "GET:AC") == 0) {
        _psu->queryInputVoltage();
        _hal->uartSend("CMD_ACK:QUERY_AC\r\n");
    } else if (strcmp(cmd, "EQ:ON") == 0) {
        _psu->setEqualization(true);
        _hal->uartSend("CMD_ACK:EQ_ON\r\n");
    } else if (strcmp(cmd, "EQ:OFF") == 0) {
        _psu->setEqualization(false);
        _hal->uartSend("CMD_ACK:EQ_OFF\r\n");
    } else if (strcmp(cmd, "PAIR") == 0) {
        _hal->triggerPairing();
        _hal->uartSend("CMD_ACK:PAIR\r\n");
    } else if (strncmp(cmd, "SET:TRANSPORT=", 14) == 0) {
        int mode = atoi(cmd + 14);
        _hal->setTransport(mode);
        _hal->uartSend("CMD_ACK:SET_TRANSPORT\r\n");
    }
}