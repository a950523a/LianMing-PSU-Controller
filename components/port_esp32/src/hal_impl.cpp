#include "hal_interface.h"
#include "port_def.h"
#include "config_common.h"

#include <driver/gpio.h>
#include <driver/twai.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>
#include <u8g2.h>
#include "psu_link/psu_pair.h"
#include "psu_link/psu_sess.h"
#include "psu_link/psu_crypto_mbedtls.h"

// ──────────────────────────────────────────────────────────────
// I2C / U8g2 globals (unchanged)
// ──────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t  g_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t  g_oled_handle     = NULL;

uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT: break;
        case U8X8_MSG_DELAY_MILLI:         vTaskDelay(pdMS_TO_TICKS(arg_int)); break;
        case U8X8_MSG_DELAY_10MICRO:       esp_rom_delay_us(arg_int * 10); break;
        case U8X8_MSG_DELAY_100NANO:       esp_rom_delay_us(1); break;
        default: return 0;
    }
    return 1;
}

#define I2C_BUFFER_SIZE 256
static uint8_t i2c_buffer[I2C_BUFFER_SIZE];
static size_t  i2c_buffer_len = 0;

uint8_t u8x8_byte_esp32_hw_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_BYTE_SEND: {
            uint8_t *data = (uint8_t *)arg_ptr;
            if (i2c_buffer_len + arg_int < I2C_BUFFER_SIZE) {
                memcpy(&i2c_buffer[i2c_buffer_len], data, arg_int);
                i2c_buffer_len += arg_int;
            }
            break;
        }
        case U8X8_MSG_BYTE_START_TRANSFER: i2c_buffer_len = 0; break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (g_oled_handle && i2c_buffer_len > 0) {
                i2c_master_transmit(g_oled_handle, i2c_buffer, i2c_buffer_len, -1);
            }
            break;
        case U8X8_MSG_BYTE_INIT:
        case U8X8_MSG_BYTE_SET_DC: break;
        default: return 0;
    }
    return 1;
}

// ──────────────────────────────────────────────────────────────
// WiFi AP credentials (web control interface)
// ──────────────────────────────────────────────────────────────

#define AP_SSID "PSU-Controller"
#define AP_PASS "psu12345"

// ──────────────────────────────────────────────────────────────
// Transport / ESP-NOW state
// ──────────────────────────────────────────────────────────────

static int     s_transport      = TRANSPORT_UART;
static uint8_t s_tes_mac[6]     = {0};
static bool    s_tes_mac_valid  = false;   // 有配對、而且有金鑰
static bool    s_needs_repair   = false;   // NVS 只有舊版配對的 MAC
static volatile bool s_espnow_send_ok = true;

// ── ESP-NOW：配對與逐封包驗證（psu_link 子模組，與 TES 控制板共用）────────────
//
// 無線上任何人都能送封包。舊版把收到的每個位元組都交給 SerialCmd —— 不看來源、
// 也不驗證，所以附近任何一台 ESP32 都能送 ON / OFF / SET:V=… 或 $SET 改輸出。
// 現在收到的封包先進佇列，transportPoll() 在主迴圈裡處理：
//   配對的 TES 送來、驗證碼正確的行 → 放進 RX 緩衝給 SerialCmd
//   配對與握手訊息（本身不帶驗證碼）→ psu_pair / psu_sess
//   其餘全部丟掉
// 為什麼不用 ESP-NOW 自己的加密：它擋不住偽造（未加密的封包照收），見 psu_sess.h。
//
// 除了 recv 回呼（只把封包丟進佇列），這一切都在主任務裡跑：transportPoll、
// SerialCmd 的收送、網頁指令（WebCtrl::processCommands 也在主迴圈）—— 不需要鎖。

typedef struct {
    char    data[PSU_LINK_MAX_AUTH_LINE];
    int     len;
    uint8_t src[6];
} espnow_rx_item_t;

static QueueHandle_t s_rx_q = NULL;

// 驗證通過的位元組 → SerialCmd（主任務自己寫、自己讀）
#define ESPNOW_RX_BUF_SIZE 512
static uint8_t s_rx_buf[ESPNOW_RX_BUF_SIZE];
static int     s_rx_head = 0;
static int     s_rx_tail = 0;

static psu_sess_t s_sess;
static bool       s_sess_on        = false;
static psu_pair_t s_pair;
static bool       s_pair_on        = false;
static bool       s_pair_start_req = false;
static uint8_t    s_pair_last_state = 0;
static uint32_t   s_pair_state_ms   = 0;
static uint32_t   s_auth_rejects    = 0;
#define PAIR_LINGER_MS 3000u   // 配對結束後再處理 3 s：TES 可能還在等最後一則確認

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint32_t ms_now() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ──────────────────────────────────────────────────────────────
// NVS helpers
// ──────────────────────────────────────────────────────────────

static void nvs_load_config() {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READONLY, &h) != ESP_OK) return;

    int32_t t = 0;
    if (nvs_get_i32(h, "transport", &t) == ESP_OK) s_transport = (int)t;

    size_t len = 6;
    bool have_mac = false;
    if (nvs_get_blob(h, "tes_mac", s_tes_mac, &len) == ESP_OK) {
        for (int i = 0; i < 6; i++) {
            if (s_tes_mac[i]) { have_mac = true; break; }
        }
    }
    uint8_t ltk[32];
    size_t klen = sizeof ltk;
    bool have_ltk = nvs_get_blob(h, "tes_ltk", ltk, &klen) == ESP_OK && klen == sizeof ltk;
    nvs_close(h);

    if (have_mac && have_ltk) {
        psu_sess_init(&s_sess, PSU_ROLE_NODE, ltk, psu_crypto_mbedtls(), ms_now());
        s_sess_on       = true;
        s_tes_mac_valid = true;
    } else if (have_mac) {
        // 舊版配對只存了 MAC：沒有金鑰就驗證不了任何訊息，只能當成沒配對
        s_needs_repair = true;
        printf("ESP-NOW: stored pairing predates link authentication - pair again\n");
    }
    memset(ltk, 0, sizeof ltk);
}

static void nvs_save_transport(int mode) {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "transport", (int32_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

// 先寫金鑰再寫 MAC：中途斷電最多留下「有金鑰沒 MAC」（視同未配對），
// 不會變成「新 MAC 配舊金鑰」。
static void nvs_save_pairing(const uint8_t *mac, const uint8_t *ltk) {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tes_ltk", ltk, 32);
    nvs_commit(h);
    nvs_set_blob(h, "tes_mac", mac, 6);
    nvs_commit(h);
    nvs_close(h);
}

// ──────────────────────────────────────────────────────────────
// 驗證通過的位元組 → SerialCmd（只在主任務裡用）
// ──────────────────────────────────────────────────────────────

static void rx_push(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int next = (s_rx_head + 1) % ESPNOW_RX_BUF_SIZE;
        if (next == s_rx_tail) return;    // 滿了就丟
        s_rx_buf[s_rx_head] = (uint8_t)data[i];
        s_rx_head = next;
    }
}

static int rx_available() {
    return (s_rx_head - s_rx_tail + ESPNOW_RX_BUF_SIZE) % ESPNOW_RX_BUF_SIZE;
}

static int rx_pop() {
    if (s_rx_head == s_rx_tail) return -1;
    int c = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % ESPNOW_RX_BUF_SIZE;
    return c;
}

// ──────────────────────────────────────────────────────────────
// ESP-NOW peer management
// ──────────────────────────────────────────────────────────────

static void espnow_add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;       // 跟隨當前頻道
    p.encrypt = false;
    esp_now_add_peer(&p);
}

// ──────────────────────────────────────────────────────────────
// ESP-NOW callbacks
// ──────────────────────────────────────────────────────────────

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    s_espnow_send_ok = (status == ESP_NOW_SEND_SUCCESS);
    if (!s_espnow_send_ok) {
        printf("ESP-NOW: TX to " MACSTR " failed\n", MAC2STR(tx_info->des_addr));
    }
}

// WiFi 任務：只把封包丟進佇列，處理全部在主任務的 transportPoll()
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len) {
    if (!s_rx_q || len <= 0 || len > (int)PSU_LINK_MAX_AUTH_LINE - 1) return;
    espnow_rx_item_t item;
    memcpy(item.data, data, len);
    item.data[len] = '\0';
    item.len = len;
    memcpy(item.src, info->src_addr, 6);
    xQueueSend(s_rx_q, &item, 0);
}

// 配對與握手訊息：不帶驗證碼（它們本身就是建立驗證的過程）
static void send_plain(const uint8_t *dest, const psu_msg_t *m) {
    char buf[PSU_LINK_MAX_LINE];
    size_t n = psu_link_encode(m, buf, sizeof buf);
    if (n == 0) return;
    espnow_add_peer(dest);
    esp_now_send(dest, (const uint8_t *)buf, n);
}

static bool from_tes(const uint8_t *src) {
    return s_tes_mac_valid && memcmp(src, s_tes_mac, 6) == 0;
}

static void handle_line(const char *line, size_t len, const uint8_t *src) {
    size_t inner = 0;
    psu_sess_result_t r = (s_sess_on && from_tes(src))
                        ? psu_sess_unwrap(&s_sess, line, len, &inner, ms_now())
                        : PSU_SESS_PLAIN;
    if (r == PSU_SESS_OK) {
        if (s_transport == TRANSPORT_ESPNOW) {
            rx_push(line, inner);
            rx_push("\n", 1);
        }
        return;
    }
    if (r != PSU_SESS_PLAIN) { s_auth_rejects++; return; }   // 驗不過、重送、還沒握手

    psu_msg_t m;
    if (psu_link_decode(line, len, &m) != PSU_LINK_OK) {
        if (from_tes(src)) s_auth_rejects++;   // 沒驗證碼的文字指令（ON、SET:V=…）：不收
        return;
    }
    switch (m.type) {
    case PSU_MSG_SESS_INIT: case PSU_MSG_SESS_REPLY:
    case PSU_MSG_SESS_FINISH: case PSU_MSG_SESS_REQUEST:
        if (s_sess_on && from_tes(src)) psu_sess_rx(&s_sess, &m, ms_now());
        return;
    case PSU_MSG_PAIR_KEY: case PSU_MSG_PAIR_COMMIT: case PSU_MSG_PAIR_NONCE:
    case PSU_MSG_PAIR_CONFIRM: case PSU_MSG_PAIR_REJECT:
        if (s_pair_on) psu_pair_rx(&s_pair, &m, src, ms_now());
        return;
    default:
        if (from_tes(src)) s_auth_rejects++;   // 沒驗證碼的 $SET / $HELO：偽造
        return;
    }
}

static void pair_apply() {
    if (s_tes_mac_valid && memcmp(s_tes_mac, s_pair.peer_mac, 6) != 0) esp_now_del_peer(s_tes_mac);
    memcpy(s_tes_mac, s_pair.peer_mac, 6);
    s_tes_mac_valid = true;
    s_needs_repair  = false;
    espnow_add_peer(s_tes_mac);
    psu_sess_init(&s_sess, PSU_ROLE_NODE, s_pair.ltk, psu_crypto_mbedtls(), ms_now());
    s_sess_on = true;
    nvs_save_pairing(s_tes_mac, s_pair.ltk);
    printf("ESP-NOW: paired (code %06lu), TES=" MACSTR "\n",
           (unsigned long)s_pair.code, MAC2STR(s_tes_mac));
}

static void transport_poll() {
    espnow_rx_item_t item;
    while (s_rx_q && xQueueReceive(s_rx_q, &item, 0) == pdTRUE) {
        int start = 0;
        for (int i = 0; i <= item.len; i++) {
            if (i == item.len || item.data[i] == '\n') {
                if (i > start) handle_line(item.data + start, (size_t)(i - start), item.src);
                start = i + 1;
            }
        }
    }

    uint32_t now = ms_now();
    if (s_pair_start_req) {
        s_pair_start_req = false;
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);   // ESP-NOW 從 STA 介面送出，TES 看到的就是這個
        psu_pair_start(&s_pair, PSU_ROLE_NODE, mac, psu_crypto_mbedtls(), now);
        s_pair_on = true;
        printf("ESP-NOW: pairing started - broadcasting key\n");
    }
    if (s_pair_on) {
        psu_msg_t m;
        bool bcast;
        while (psu_pair_poll(&s_pair, now, &m, &bcast)) send_plain(bcast ? BCAST : s_pair.peer_mac, &m);
        if (s_pair.state != s_pair_last_state) {
            s_pair_last_state = s_pair.state;
            s_pair_state_ms   = now;
            if (s_pair.state == PSU_PAIR_CONFIRM)
                printf("ESP-NOW: pairing code %06lu - confirm on both devices\n", (unsigned long)s_pair.code);
            else if (s_pair.state == PSU_PAIR_DONE)
                pair_apply();
            else if (s_pair.state == PSU_PAIR_FAILED)
                printf("ESP-NOW: pairing failed (%u)\n", (unsigned)s_pair.fail);
        }
        if (!psu_pair_busy(&s_pair) && now - s_pair_state_ms >= PAIR_LINGER_MS) {
            s_pair_on = false;
            memset(s_pair.ltk, 0, sizeof s_pair.ltk);
        }
    }
    if (s_sess_on) {
        psu_msg_t m;
        while (psu_sess_poll(&s_sess, now, &m)) send_plain(s_tes_mac, &m);
    }
}

// ──────────────────────────────────────────────────────────────
// HAL Implementation
// ──────────────────────────────────────────────────────────────

class Esp32HAL : public IHardwareHAL {
public:
    Esp32HAL() {}

    void init() override {
        printf("HAL: Init NVS...\n");
        initNvs();

        printf("HAL: Init GPIO...\n");
        initGpio();

        printf("HAL: Init CAN...\n");
        initCan();

        printf("HAL: Init UART...\n");
        initUart();

        printf("HAL: Init WiFi & ESP-NOW...\n");
        initWifiAndEspNow();

        printf("HAL: Init I2C & OLED...\n");
        initI2cAndU8g2();

        printf("HAL: Init Done! transport=%d, TES MAC %s\n",
               s_transport, s_tes_mac_valid ? "loaded" : "unknown");
    }

    // ── System ──────────────────────────────────────────────

    uint32_t getTickCount() override {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void delayMs(uint32_t ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    // ── GPIO ────────────────────────────────────────────────

    bool readButton(HalButton btn) override {
        gpio_num_t pin;
        switch (btn) {
            case BTN_SELECT: pin = PIN_BTN_SEL; break;
            case BTN_UP:     pin = PIN_BTN_UP;  break;
            case BTN_DOWN:   pin = PIN_BTN_DOWN; break;
            default: return false;
        }
        return gpio_get_level(pin) == 0;  // Active Low
    }

    // ── CAN ─────────────────────────────────────────────────

    bool canSend(const HalCanFrame &frame) override {
        twai_message_t msg;
        msg.identifier      = frame.id;
        msg.extd            = frame.ext;
        msg.data_length_code = frame.len;
        memcpy(msg.data, frame.data, frame.len);
        return twai_transmit(&msg, 0) == ESP_OK;
    }

    bool canReceive(HalCanFrame &frame) override {
        twai_message_t msg;
        if (twai_receive(&msg, 0) == ESP_OK) {
            frame.id  = msg.identifier;
            frame.ext = msg.extd;
            frame.len = msg.data_length_code;
            memcpy(frame.data, msg.data, msg.data_length_code);
            return true;
        }
        return false;
    }

    // ── UART / ESP-NOW transport (路由在此) ─────────────────

    // ESP-NOW：每一行都加驗證尾碼。還沒跟 TES 握手完成就不送 —— TES 反正會丟掉
    // 沒驗證的訊息（開機時的 $CAP 因此會錯過，TES 連上後會送 $HELO 來問）。
    void uartSend(const char *str) override {
        if (s_transport == TRANSPORT_ESPNOW) {
            if (!s_sess_on || !psu_sess_ready(&s_sess)) return;
            const char *p = str;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p + 1) : strlen(p);
                char out[PSU_LINK_MAX_AUTH_LINE];
                size_t w = psu_sess_wrap(&s_sess, p, n, out, sizeof out);
                if (w > 0) esp_now_send(s_tes_mac, (const uint8_t *)out, w);
                p += n;
            }
            return;
        }
        uart_write_bytes(CMD_UART_PORT, str, strlen(str));
    }

    int uartRead() override {
        if (s_transport == TRANSPORT_ESPNOW) return rx_pop();
        uint8_t data;
        int len = uart_read_bytes(CMD_UART_PORT, &data, 1, 0);
        return (len > 0) ? data : -1;
    }

    int uartAvailable() override {
        if (s_transport == TRANSPORT_ESPNOW) return rx_available();
        size_t size;
        uart_get_buffered_data_len(CMD_UART_PORT, &size);
        return (int)size;
    }

    // ── Transport config ─────────────────────────────────────

    void triggerPairing() override {
        s_pair_start_req = true;   // transportPoll() 裡才開始（主任務）
    }

    bool isPairingActive() override {
        return s_pair_start_req || (s_pair_on && psu_pair_busy(&s_pair));
    }

    void transportPoll() override { transport_poll(); }

    void getPairInfo(HalPairInfo& i) override {
        i = HalPairInfo();
        i.state       = s_pair.state;
        i.fail        = s_pair.fail;
        i.code        = s_pair.code;
        i.localOk     = s_pair.local_ok;
        i.peerOk      = s_pair.peer_ok;
        i.ageMs       = ms_now() - s_pair_state_ms;
        i.linkAuth    = s_sess_on && psu_sess_ready(&s_sess);
        i.needsRepair = s_needs_repair;
    }

    void pairAnswer(bool accept) override {
        if (s_pair_on) psu_pair_user(&s_pair, accept, ms_now());
    }

    void setTransport(int mode) override {
        s_transport = mode;
        nvs_save_transport(mode);
        printf("HAL: Transport -> %d\n", mode);
    }

    int getTransport() override {
        return s_transport;
    }

    bool isPairedEspNow() override {
        return s_tes_mac_valid;
    }

    void getEspNowPeerMac(char* buf, size_t len) override {
        if (!s_tes_mac_valid || len < 18) {
            if (len > 0) buf[0] = '\0';
            return;
        }
        snprintf(buf, len, MACSTR, MAC2STR(s_tes_mac));
    }

    // ── Display (U8g2) ──────────────────────────────────────

    void displayClear() override {
        u8g2_ClearBuffer(&_u8g2);
    }

    void displayDrawString(int x, int y, const char *str, int fontSize) override {
        if (fontSize == 0) {
            u8g2_SetFont(&_u8g2, u8g2_font_6x10_tf);
        } else {
            u8g2_SetFont(&_u8g2, u8g2_font_profont17_tf);
        }
        u8g2_DrawStr(&_u8g2, x, y + 8, str);
    }

    void displayShow() override {
        u8g2_SendBuffer(&_u8g2);
    }

private:
    u8g2_t _u8g2;

    // ── init helpers ────────────────────────────────────────

    void initNvs() {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ESP_ERROR_CHECK(nvs_flash_init());
        }
        nvs_load_config();
    }

    void initGpio() {
        gpio_config_t io_conf = {};
        io_conf.intr_type    = GPIO_INTR_DISABLE;
        io_conf.mode         = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL<<PIN_BTN_SEL) | (1ULL<<PIN_BTN_UP) | (1ULL<<PIN_BTN_DOWN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }

    void initCan() {
        twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(PIN_CAN_TX, PIN_CAN_RX, TWAI_MODE_NORMAL);
        twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_125KBITS();
        twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        twai_driver_install(&g_config, &t_config, &f_config);
        twai_start();
    }

    void initUart() {
        uart_config_t uart_config = {
            .baud_rate  = CMD_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 122,
            .source_clk = UART_SCLK_DEFAULT,
            // allow_pd = 0：睡眠時 UART 的電源域要保持供電。backup_before_sleep 是它已棄用的別名，
            // 只指定別名會讓 C++ 對「沒初始化到 allow_pd」發出警告；空的 {} 兩個都歸零，意思不變。
            .flags = {},
        };
        uart_driver_install(CMD_UART_PORT, 1024, 0, 0, NULL, 0);
        uart_param_config(CMD_UART_PORT, &uart_config);
        uart_set_pin(CMD_UART_PORT, CMD_UART_TX, CMD_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    void initWifiAndEspNow() {
        s_rx_q = xQueueCreate(8, sizeof(espnow_rx_item_t));

        ESP_ERROR_CHECK(esp_netif_init());
        // event loop 可能已由其他元件建立，忽略 INVALID_STATE
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }

        // AP netif 提供 DHCP server 給 web 控制頁面的客戶端
        esp_netif_create_default_wifi_ap();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

        // 設定 SoftAP：SSID/密碼、最多 3 台客戶端
        wifi_config_t ap_cfg = {};
        strncpy((char*)ap_cfg.ap.ssid,     AP_SSID, sizeof(ap_cfg.ap.ssid));
        strncpy((char*)ap_cfg.ap.password, AP_PASS,  sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
        ap_cfg.ap.max_connection = 3;
        ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.channel        = 1;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

        ESP_ERROR_CHECK(esp_wifi_start());
        printf("WiFi AP started: SSID=%s  Password=%s  IP=192.168.4.1\n", AP_SSID, AP_PASS);

        ESP_ERROR_CHECK(esp_now_init());
        ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
        ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

        // 廣播 peer（配對 hello 需要）
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        espnow_add_peer(bcast);

        // 若已有配對 MAC，預先加入
        if (s_tes_mac_valid) {
            espnow_add_peer(s_tes_mac);
        }
    }

    void initI2cAndU8g2() {
        i2c_master_bus_config_t i2c_mst_config = {
            .i2c_port        = I2C_PORT,
            .sda_io_num      = PIN_SDA,
            .scl_io_num      = PIN_SCL,
            .clk_source      = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority   = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1, .allow_pd = 0 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &g_i2c_bus_handle));

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = OLED_ADDR,
            .scl_speed_hz    = I2C_SPEED_HZ,
            .scl_wait_us     = 0,
            .flags = { .disable_ack_check = 0 },
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg, &g_oled_handle));

        u8g2_Setup_ssd1306_i2c_128x64_noname_f(
            &_u8g2, U8G2_R0,
            u8x8_byte_esp32_hw_i2c,
            u8x8_gpio_and_delay_esp32);
        u8x8_SetI2CAddress(&_u8g2.u8x8, OLED_ADDR << 1);
        u8g2_InitDisplay(&_u8g2);
        u8g2_SetPowerSave(&_u8g2, 0);

        u8g2_ClearBuffer(&_u8g2);
        u8g2_SetFont(&_u8g2, u8g2_font_6x10_tf);
        u8g2_DrawStr(&_u8g2, 0, 10, "System Ready");
        u8g2_SendBuffer(&_u8g2);
    }
};

// ──────────────────────────────────────────────────────────────
// Global HAL instance
// ──────────────────────────────────────────────────────────────

Esp32HAL g_hal;
IHardwareHAL* getHal() { return &g_hal; }
