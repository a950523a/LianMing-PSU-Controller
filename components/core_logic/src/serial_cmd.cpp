#include "serial_cmd.h"
#include "config_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 狀態回報週期：輸出中 100 ms，待機 1 s（待機的 $ST 同時是心跳）
static const uint32_t STATUS_PERIOD_ON_MS  = 100;
static const uint32_t STATUS_PERIOD_OFF_MS = 1000;
// $PWR：有變化馬上送；沒變化也每 30 s 送一次（ESP-NOW 偶爾掉一則，TES 不會一直拿著舊的）
static const uint32_t POWER_PERIOD_MS = 30000;

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
      _lastStatusTime(0), _statusSeq(0), _lastPowerTime(0) {
    memset(_inputBuffer, 0, BUF_SIZE);
    memset(&_lastPower, 0, sizeof(_lastPower));
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

    // 3. 可用功率：AC 電壓差 2 V 以上、或功率上限、插座上限變了就送
    {
        uint16_t vac = (uint16_t)(st.inputVoltage * 10.0f + 0.5f);
        int dv = (int)vac - (int)_lastPower.vac_dv;
        bool changed = dv > 20 || dv < -20 || st.powerLimitW != _lastPower.pmax_w ||
                       st.outletLimitDa != _lastPower.ilim_da || st.acSource != _lastPower.src;
        if ((changed && st.inputVoltage > 0.0f) || now - _lastPowerTime >= POWER_PERIOD_MS) sendPower(st);
    }

    // 4. AC 查詢結果（人下的 GET:AC 指令的非同步回應，維持文字格式）
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
    m.u.cap.caps      = PSU_CAP_REPORT_V | PSU_CAP_REPORT_I | PSU_CAP_SET_V | PSU_CAP_SET_I |
                        PSU_CAP_REPORT_PWR;
    m.u.cap.v_max_cv  = toCentiU(MAX_TARGET_VOLTAGE);
    m.u.cap.i_max_ca  = toCentiU(MAX_TARGET_CURRENT);
    m.u.cap.fw_ver    = fwVersionCode(FIRMWARE_VERSION);
    sendMsg(m);
    sendInfo();
}

// 節點身分：緊接在 CAP 之後（開機與回 HELO 時）。版本字串只留 PSU-Link 允許的字元、截到上限
void SerialCmd::sendInfo() {
    psu_msg_t m = {};
    m.type = PSU_MSG_INFO;
    m.u.info.hw_rev = NODE_HW_REV;
    size_t n = 0;
    for (const char* p = FIRMWARE_VERSION; *p && n < PSU_VER_STR_MAX; p++) {
        char c = *p;
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  c == '.' || c == '_' || c == '+' || c == '-';
        if (ok) m.u.info.fw[n++] = c;
    }
    m.u.info.fw[n]   = '\0';
    m.u.info.boot[0] = '\0';                    // ESP-IDF 的開機程式不另外更新，不回報
    m.u.info.uid_len = (uint8_t)_hal->getUniqueId(m.u.info.uid, PSU_UID_MAX);
    if (m.u.info.uid_len > 0) sendMsg(m);
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

void SerialCmd::sendPower(const PowerStatus& st) {
    psu_msg_t m = {};
    m.type = PSU_MSG_POWER;
    float vac = st.inputVoltage;
    m.u.power.vac_dv  = (vac > 0.0f && vac < 6553.0f) ? (uint16_t)(vac * 10.0f + 0.5f) : 0;
    m.u.power.pmax_w  = st.powerLimitW;
    m.u.power.ilim_da = st.outletLimitDa;
    m.u.power.src     = st.acSource;
    sendMsg(m);
    _lastPower     = m.u.power;
    _lastPowerTime = _hal->getTickCount();
}

void SerialCmd::applySet(const psu_msg_set_t& set) {
    PowerStatus st = _psu->getStatus();
    float v = set.has_v ? (float)set.v_cv / 100.0f : st.voltageSet;
    float i = set.has_i ? (float)set.i_ca / 100.0f : st.currentSet;

    psu_msg_t ack = {};
    ack.type = PSU_MSG_ACK;
    ack.u.ack.seq = set.seq;

    // 電流超過模組上限：夾到上限照樣套用（TES 的最大電流設定可能比 66 A 高；整筆拒絕的話
    // 連電壓都不會變）。電壓超過就整筆不套用 —— 那是設定錯了，不要只套一半
    if (i > MAX_TARGET_CURRENT) i = MAX_TARGET_CURRENT;
    if (set.has_v && v > MAX_TARGET_VOLTAGE) {
        ack.u.ack.result = PSU_ACK_RANGE;
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
    case PSU_MSG_HELLO: sendCap(); sendPower(_psu->getStatus()); break;
    case PSU_MSG_SET:   applySet(m.u.set);   break;
    case PSU_MSG_CHANNEL: _hal->followTesChannel(m.u.channel.channel); break;
    case PSU_MSG_NETWORK:
        _hal->rememberTesNetwork(m.u.network.bssid, m.u.network.ssid, m.u.network.ssid_len);
        break;
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
    } else if (strncmp(cmd, "SET:OUTLET=", 11) == 0) {
        // 插座電流上限（A），0 = 不限
        char* end;
        float a = strtof(cmd + 11, &end);
        if (end == cmd + 11 || *end || !_psu->setOutletLimit((uint16_t)(a * 10.0f + 0.5f))) {
            _hal->uartSend("ERR:OUTLET_OUT_OF_RANGE\r\n");
            return;
        }
        char buf[40];
        snprintf(buf, sizeof(buf), "OUTLET=%.1f\r\n", a);
        _hal->uartSend(buf);
    } else if (strcmp(cmd, "GET:PWR") == 0) {
        PowerStatus st = _psu->getStatus();
        char buf[64];
        snprintf(buf, sizeof(buf), "AC=%.1f PMAX=%u OUTLET=%.1f\r\n",
                 st.inputVoltage, (unsigned)st.powerLimitW, st.outletLimitDa / 10.0f);
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
