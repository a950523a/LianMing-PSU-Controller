#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include "hal_interface.h"
#include "psu_protocol.h"
#include "psu_link/psu_link.h"

// 指令埠（UART2 或 ESP-NOW，由 HAL 決定）上的兩種內容：
//   - '$' 開頭：與 TES 控制板之間的連線協定（PSU-Link 子模組，psu_link/psu_link.h）
//       收：$HELO、$SET        送：$CAP、$ST、$ACK
//   - 其他行：人下的文字指令（ON、OFF、PAIR、SET:TRANSPORT=…），回應也是文字
//     TES 控制板會忽略不是 '$' 開頭的行，所以兩者可以共用同一個埠。
class SerialCmd {
public:
    SerialCmd(IHardwareHAL* hal, PowerProtocol* psu);
    void begin();
    void loop();

private:
    IHardwareHAL* _hal;
    PowerProtocol* _psu;

    static const int BUF_SIZE = PSU_LINK_MAX_LINE;
    char _inputBuffer[BUF_SIZE];
    int _bufIndex;
    bool _discarding;          // 行太長：丟到下一個 '\n' 為止
    uint32_t _lastStatusTime;  // 最後一次送出 $ST
    uint16_t _statusSeq;

    void processLine(char* line, int len);
    void processFrame(const char* line, int len);
    void processCommand(char* cmd);

    void sendMsg(const psu_msg_t& msg);
    void sendCap();
    void sendStatus(const PowerStatus& st);
    void applySet(const psu_msg_set_t& set);
};

#endif
