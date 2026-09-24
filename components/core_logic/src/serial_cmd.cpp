#include "serial_cmd.h"
#include "config_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 狀態回報週期：輸出中 100 ms，待機 1 s（待機的 $ST 同時是心跳）
static const uint32_t STATUS_PERIOD_ON_MS  = 100;
static const uint32_t STATUS_PERIOD_OFF_MS = 1000;

// 輸出電壓低於這個值視為「沒在輸出」，本筆電壓／電流標為無效。
// TES 控制板看到無效就改用自己的 ADC —— 沿用舊協定「≥ 1 V 才送 V=，否則送 HB」的界線。
static const float OUTPUT_ACTIVE_V = 1.0f;

// "v1.3.1-5-gabc123" → 131。解不出來回傳 0（TES 端當作「未知」）。
static uint16_t fwVersionCode(const char* s)
{
    if (*s == 'v' || *s == 'V') s++;
    unsigned part[3] = {0, 0, 0};
    int n = 0;
    while (n < 3 && *s >= '0' && *s <= '9') {
        while (*s >= '0' && *s <= '9') part[n] = part[n] * 10u + (unsigned)(*s++ - '0');
        n++;
        if (*s != '.') break;
        s++;
    }
    if (n == 0) return 0;
    unsigned code = part[0] * 100u + (part[1] > 9u ? 9u : part[1]) * 10u + (part[2] > 9u ? 9u : part[2]);
    return (uint16_t)(code > 65535u ? 65535u : code);
}

static uint16_t toCentiU(float x)
{
    if (!(x > 0.0f)) return 0;           // 負值與 NaN 一律當 0
    if (x >= 655.35f) return 65535u;
    return (uint16_t)(x * 100.0f + 0.5f);
}

static int16_t toCentiS(float x)
{
    if (x != x) return 0;                // NaN
    if (x >= 327.67f)  return 32767;
    if (x <= -327.68f) return -32768;
    return (int16_t)(x * 100.0f + (x >= 0.0f ? 0.5f : -0.5f));
}

SerialCmd::SerialCmd(IHardwareHAL* hal, PowerProtocol* psu)
    : _hal(hal), _psu(psu), _bufIndex(0), _discarding(false),
      _lastStatusTime(0), _statusSeq(0) {
    memset(_inputBuffer, 0, BUF_SIZE);
}

void SerialCmd::begin() {
    // UART init is handled by HAL。開機先宣告自己是什麼；控制板若比我們晚開機
    // 錯過了這一則，它收到 $ST 之後會送 $HELO 來問。
    sendCap();
}

void SerialCmd::loop() {
    // 1. Receive Char
    while (_hal->uartAvailable()) {
        int c = _hal->uartRead();
        if (c == -1) break;
        if (c == '\n') {
            if (!_discarding) {
                _inputBuffer[_bufIndex] = '\0';
                processLine(_inputBuffer, _bufIndex);
            }
            _bufIndex   = 0;
            _discarding = false;
        } else if (_discarding) {
            // 丟棄中
        } else if (_bufIndex < BUF_SIZE - 1) {
            _inputBuffer[_bufIndex++] = (char)c;
        } else {
            _bufIndex   = 0;
            _discarding = true;
        }
    }

    // 2. Telemetry：$ST 輸出中 100 ms 一次、待機 1 s 一次
    PowerStatus st  = _psu->getStatus();
    uint32_t now    = _hal->getTickCount();
    uint32_t period = (st.voltageOut >= OUTPUT_ACTIVE_V) ? STATUS_PERIOD_ON_MS : STATUS_PERIOD_OFF_MS;
    if (now - _lastStatusTime >= period) {
        sendStatus(st);
        _lastStatusTime = now;
    }

    // 3. AC 查詢結果（人下的 GET:AC 指令的非同步回應，維持文字格式）
    if (st.newInputVoltage) {
        char buf[32];
        snprintf(buf, sizeof(buf), "AC=%.1f\r\n", st.inputVoltage);
        _hal->uartSend(buf);
        _psu->clearInputFlag();
    }
}

void SerialCmd::processLine(char* line, int len) {
    // Remove \r if present
    if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
    if (len == 0) return;

    if (line[0] == '$') processFrame(line, len);
    else                processCommand(line);
}

// ─── 連線協定（與 TES 控制板）──────────────────────────────────────────────

void SerialCmd::sendMsg(const psu_msg_t& msg) {
    char buf[PSU_LINK_MAX_LINE];
    if (psu_link_encode(&msg, buf, sizeof(buf)) > 0) _hal->uartSend(buf);
}

void SerialCmd::sendCap() {
    psu_msg_t m = {};
    m.type = PSU_MSG_CAP;
    m.u.cap.proto_ver = PSU_LINK_PROTO_VER;
    m.u.cap.node_type = PSU_NODE_RECTIFIER;
    // 聯明的 CAN 協定沒有回報 CV/CC 狀態，所以不宣告 PSU_CAP_REPORT_MODE ——
    // 用設定值與量測值去猜，猜錯時控制板會據此做出錯誤的判斷。
    m.u.cap.caps      = PSU_CAP_REPORT_V | PSU_CAP_REPORT_I | PSU_CAP_SET_V | PSU_CAP_SET_I;
    m.u.cap.v_max_cv  = toCentiU(MAX_TARGET_VOLTAGE);
    m.u.cap.i_max_ca  = toCentiU(MAX_TARGET_CURRENT);
    m.u.cap.fw_ver    = fwVersionCode(FIRMWARE_VERSION);
    sendMsg(m);
}

void SerialCmd::sendStatus(const PowerStatus& st) {
    bool active = (st.voltageOut >= OUTPUT_ACTIVE_V);
    uint8_t flags = 0;
    if (active)            flags |= PSU_ST_V_VALID | PSU_ST_I_VALID;
    if (st.isOn)           flags |= PSU_ST_OUTPUT_ON;
    if (st.isSoftStarting) flags |= PSU_ST_SOFT_START;

    psu_msg_t m = {};
    m.type = PSU_MSG_STATUS;
    m.u.status.seq   = _statusSeq++;
    m.u.status.v_cv  = toCentiU(st.voltageOut);
    m.u.status.i_ca  = toCentiS(st.currentOut);
    m.u.status.mode  = st.isOn ? PSU_MODE_UNKNOWN : PSU_MODE_OFF;
    m.u.status.flags = flags;
    sendMsg(m);
}

void SerialCmd::applySet(const psu_msg_set_t& set) {
    PowerStatus st = _psu->getStatus();
    float v = set.has_v ? (float)set.v_cv / 100.0f : st.voltageSet;
    float i = set.has_i ? (float)set.i_ca / 100.0f : st.currentSet;

    psu_msg_t ack = {};
    ack.type = PSU_MSG_ACK;
    ack.u.ack.seq = set.seq;

    if ((set.has_v && v > MAX_TARGET_VOLTAGE) || (set.has_i && i > MAX_TARGET_CURRENT)) {
        ack.u.ack.result = PSU_ACK_RANGE;       // 整筆不套用：不要只套一半
    } else {
        _psu->setOutput(v, i);
        ack.u.ack.result = PSU_ACK_OK;
    }
    sendMsg(ack);
}

void SerialCmd::processFrame(const char* line, int len) {
    psu_msg_t m;
    if (psu_link_decode(line, (size_t)len, &m) != PSU_LINK_OK) return;   // CRC 錯或看不懂：丟掉

    switch (m.type) {
    case PSU_MSG_HELLO: sendCap();           break;
    case PSU_MSG_SET:   applySet(m.u.set);   break;
    default:            break;   // CAP / ST / ACK 是我們送出的方向
    }
}

// ─── 人下的文字指令 ─────────────────────────────────────────────────────────

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
    } else if (strcmp(cmd, "STATUS:TRANSPORT") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "TRANSPORT=%d\r\n", _hal->getTransport());
        _hal->uartSend(buf);
    } else if (strcmp(cmd, "STATUS:PAIR") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PAIRED=%d,PAIRING=%d\r\n",
                 _hal->isPairedEspNow() ? 1 : 0,
                 _hal->isPairingActive() ? 1 : 0);
        _hal->uartSend(buf);
    }
}
