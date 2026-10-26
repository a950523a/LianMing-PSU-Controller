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
#include <stdlib.h>
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

// ── 頻道（2026-10-14）────────────────────────────────────────────────────────
// ESP-NOW 只收得到同一個頻道上的封包。TES 連著家裡路由器時只能待在路由器的頻道，
// 這裡的 AP 卻寫死在 1 —— 頻道不同就永遠配不起來、也連不上（實測兩邊都「配對逾時」）。
// 所以由節點去找 TES：
//   配對搜尋中：每 PAIR_HOP_MS 換一個頻道（1→13 輪流），TES 一回應（離開 SEARCHING）就停
//   配對成功：把當時的頻道存進 NVS（tes_ch），開機就用它
//   連線中：送給 TES 的單播連續 LINK_FAIL_LIMIT 次沒 ACK（路由器換頻道、TES 重開到別的頻道）
//           → 重新握手；握手 LINK_SCAN_AFTER_MS 內沒成功就每 LINK_HOP_MS 換頻道找
// 換頻道會讓連在這台 AP 上的手機短暫斷線 —— 只在配對或找不到 TES 時才會換。
#define CH_MIN              1
#define CH_MAX              13
#define PAIR_HOP_MS         600u    // > PSU_PAIR_RESEND_MS（500）：每個頻道至少廣播一次
#define LINK_HOP_MS         1200u   // 慢掃：> PSU_SESS_RESEND_MS（1000），舊版 TES 每秒才送一次 SH1
#define LINK_HOP_FAST_MS     250u   // 快掃（2026-10-18）：一跳過去就喊 SHR，新版 TES 立刻回 SH1，往返不到 50 ms
#define LINK_SCAN_AFTER_MS  1500u   // 握手沒成功多久開始找（原本 5 s）
#define LINK_FAIL_LIMIT     5u
// 用 TES 的網路找頻道（$NET，2026-10-26）：記得 TES 在哪個網路的話，找 TES 時先掃描一次 WiFi，
// 看那台基地台在哪個頻道就直接切過去（約 2 s），不必一個頻道一個頻道試。掃不到（TES 換了網路、
// 不在範圍內）才照舊跳頻道找。常見的 ESP-NOW＋WiFi 教學也是這樣找頻道的
#define LINK_RESCAN_MS     30000u   // 兩次掃描最少間隔（中間照舊跳頻道）
#define LINK_SCAN_HOLD_MS   3000u   // 掃到之後在那個頻道等握手多久，才回去跳頻道
#define LINK_SCAN_MAX_MS    6000u   // 掃描最久等多久（正常約 2 s）
static psu_msg_network_t s_net;                   // TES 上次說的網路
static bool              s_net_valid      = false;
static volatile bool     s_scan_done      = false; // WIFI_EVENT_SCAN_DONE（事件任務設、主任務讀）
static bool              s_scan_running   = false;
static uint32_t          s_scan_start_ms  = 0;
static uint32_t          s_scan_last_ms   = 0;
static bool              s_scan_ever      = false;
static uint32_t          s_scan_hold_until = 0;
static uint8_t           s_chan           = 1;    // 目前的頻道
static uint8_t           s_chan_blocked   = 0;    // TES 說它在這個頻道、我們卻切不過去（手機連著熱點）
// 握手完成後用「多久沒收到 TES 的驗證訊息」判斷掉線，不只看 MAC ACK（TES 每 5 s 送 $CHN）
static uint32_t          s_last_tes_rx_ms = 0;
static bool              s_was_ready      = false;   // 上一圈已經握手完成（抓「剛完成」那一刻）
#define LINK_RX_TIMEOUT_MS  12000u   // TES 每 5 s 送一次 $CHN：12 s 都沒收到就當作掉線
static uint32_t          s_chan_blocked_log_ms = 0;
static uint8_t           s_chan_saved     = 0;    // NVS 裡的頻道（0 = 沒存過）
static uint32_t          s_hop_ms         = 0;
static uint8_t           s_link_hops      = 0;    // 這一輪找頻道跳了幾次：快掃、慢掃一輪輪交替
static uint32_t          s_link_ok_ms     = 0;    // 最後一次確認連線正常
static volatile uint32_t s_tx_fail_streak = 0;    // 連續沒有 ACK 的單播（send 回呼更新）

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
    uint8_t ch = 0;
    if (nvs_get_u8(h, "tes_ch", &ch) == ESP_OK && ch >= CH_MIN && ch <= CH_MAX) {
        s_chan_saved = ch;
        s_chan       = ch;
    }
    size_t nlen = sizeof s_net;
    s_net_valid = nvs_get_blob(h, "tes_net", &s_net, &nlen) == ESP_OK && nlen == sizeof s_net &&
                  s_net.ssid_len >= 1 && s_net.ssid_len <= PSU_SSID_MAX;
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

static void nvs_save_channel(uint8_t ch) {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "tes_ch", ch);
    nvs_commit(h);
    nvs_close(h);
    s_chan_saved = ch;
    printf("ESP-NOW: TES is on channel %u (saved)\n", (unsigned)ch);
}

// 連線斷掉後重新握手：從 NVS 重新讀長期金鑰（RAM 裡不另外留一份）
static bool sess_restart() {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t ltk[32];
    size_t klen = sizeof ltk;
    bool ok = nvs_get_blob(h, "tes_ltk", ltk, &klen) == ESP_OK && klen == sizeof ltk;
    nvs_close(h);
    if (ok) psu_sess_init(&s_sess, PSU_ROLE_NODE, ltk, psu_crypto_mbedtls(), ms_now());
    memset(ltk, 0, sizeof ltk);
    return ok;
}

// TES 說它在哪個網路：變了才寫 NVS
static void remember_tes_network(const uint8_t* bssid, const uint8_t* ssid, uint8_t len) {
    if (s_transport != TRANSPORT_ESPNOW || len < 1 || len > PSU_SSID_MAX) return;
    psu_msg_network_t n = {};
    memcpy(n.bssid, bssid, 6);
    memcpy(n.ssid, ssid, len);
    n.ssid_len = len;
    if (s_net_valid && memcmp(&n, &s_net, sizeof n) == 0) return;
    s_net       = n;
    s_net_valid = true;
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tes_net", &s_net, sizeof s_net);
    nvs_commit(h);
    nvs_close(h);
    printf("ESP-NOW: TES is on network %.*s (" MACSTR ") - saved\n", (int)len, (const char*)ssid, MAC2STR(bssid));
}

static void on_scan_done(void*, esp_event_base_t, int32_t, void*) { s_scan_done = true; }

// 掃描結果裡 TES 的網路在哪個頻道：先比 BSSID（mesh 同名的基地台可能在不同頻道），再比 SSID（取最強）。
// 找不到回 0。讀完就清掉掃描結果
static uint8_t scan_find_tes_channel() {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) { esp_wifi_clear_ap_list(); return 0; }
    wifi_ap_record_t* recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_clear_ap_list(); return 0; }
    esp_wifi_scan_get_ap_records(&n, recs);
    uint8_t by_bssid = 0, by_ssid = 0;
    int8_t  best = -127;
    for (uint16_t i = 0; i < n; i++) {
        const wifi_ap_record_t& r = recs[i];
        if (memcmp(r.bssid, s_net.bssid, 6) == 0) { by_bssid = r.primary; break; }
        if (strnlen((const char*)r.ssid, sizeof r.ssid) == s_net.ssid_len &&
            memcmp(r.ssid, s_net.ssid, s_net.ssid_len) == 0 && r.rssi > best) {
            best = r.rssi;
            by_ssid = r.primary;
        }
    }
    free(recs);
    uint8_t ch = by_bssid ? by_bssid : by_ssid;
    return (ch >= CH_MIN && ch <= CH_MAX) ? ch : 0;
}

static void set_channel(uint8_t ch) {
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) s_chan = ch;
}

// 熱點上有手機連著時 esp_wifi_set_channel() 會失敗（ESP-IDF：STA+AP 模式下 AP 有連線就不能呼叫），
// 所以「找 TES」與「跟著 TES」在手機連著熱點時都動不了 —— 2026-10-18 就是這樣卡住的。
static void follow_tes_channel(uint8_t ch) {
    if (s_transport != TRANSPORT_ESPNOW || ch < CH_MIN || ch > CH_MAX) return;
    if (ch == s_chan) {
        // TES 確認我們在對的頻道：這時才存 —— 握手成功的頻道可能只是 TES 掃描時路過的
        s_chan_blocked = 0;
        if (s_chan != s_chan_saved) nvs_save_channel(s_chan);
        return;
    }
    uint8_t from = s_chan;
    set_channel(ch);
    if (s_chan == ch) {
        s_chan_blocked = 0;
        printf("ESP-NOW: TES says it is on channel %u - moved from %u\n", (unsigned)ch, (unsigned)from);
        nvs_save_channel(ch);
        return;
    }
    s_chan_blocked = ch;
    uint32_t now = ms_now();
    if (now - s_chan_blocked_log_ms >= 60000u || s_chan_blocked_log_ms == 0) {
        s_chan_blocked_log_ms = now;
        printf("ESP-NOW: TES is on channel %u but we cannot leave %u - a phone on the hotspot blocks it\n",
               (unsigned)ch, (unsigned)s_chan);
    }
}

static void hop_channel() {
    set_channel(s_chan >= CH_MAX ? (uint8_t)CH_MIN : (uint8_t)(s_chan + 1));
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
    // 廣播沒有 ACK，只算單播
    static const uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (memcmp(tx_info->des_addr, bc, 6) != 0)
        s_tx_fail_streak = s_espnow_send_ok ? 0 : s_tx_fail_streak + 1;
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
        s_last_tes_rx_ms = ms_now();
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
    if (s_chan != s_chan_saved) nvs_save_channel(s_chan);
    s_link_ok_ms = ms_now();
    printf("ESP-NOW: paired (code %06lu), TES=" MACSTR " on channel %u\n",
           (unsigned long)s_pair.code, MAC2STR(s_tes_mac), (unsigned)s_chan);
}

// 已配對、用 ESP-NOW：確認 TES 還在這個頻道，不在就找
static void link_channel_poll(uint32_t now) {
    if (s_transport != TRANSPORT_ESPNOW || !s_sess_on || s_pair_on) return;
    if (psu_sess_ready(&s_sess)) {
        if (s_tx_fail_streak >= LINK_FAIL_LIMIT) {
            printf("ESP-NOW: %u sends to TES unanswered - re-handshaking\n", (unsigned)s_tx_fail_streak);
            s_tx_fail_streak = 0;
            sess_restart();
            s_link_ok_ms = now;
            return;
        }
        // TES 每 5 s 送 $CHN：太久沒收到 TES 的驗證訊息就重新握手、找頻道。
        // 只看 MAC ACK 不夠：停在隔壁頻道時偶爾會送成功，計數一直被歸零（2026-10-18 卡過）。
        // 以前要先收到過一則 $CHN 才啟用這個檢查（相容不送 $CHN 的舊版 TES）—— 但開機後第一次握手
        // 就停在隔壁頻道的話，$CHN 一則都收不到，檢查永遠不啟用，只能重開 LianMing（2026-10-22 拿掉）。
        // 計時從握手完成那一刻起算
        if (!s_was_ready) {
            s_was_ready      = true;
            s_last_tes_rx_ms = now;
        }
        if (now - s_last_tes_rx_ms >= LINK_RX_TIMEOUT_MS) {
            printf("ESP-NOW: nothing from TES for %lu ms - re-handshaking\n", (unsigned long)(now - s_last_tes_rx_ms));
            sess_restart();
            s_last_tes_rx_ms = now;
            s_link_ok_ms = now;
            return;
        }
        s_link_ok_ms = now;
        s_link_hops  = 0;
        // 不在這裡存頻道：握手成功的頻道可能只是 TES 開機掃描時路過的。等 TES 用 $CHN 確認才存
        return;
    }
    s_was_ready = false;
    // 先用 TES 的網路找頻道：掃描中就等結果；掃到了就切過去、在那裡等一下握手
    if (s_scan_running) {
        if (!s_scan_done) {
            if (now - s_scan_start_ms >= LINK_SCAN_MAX_MS) {
                esp_wifi_scan_stop();
                s_scan_running = false;
                printf("ESP-NOW: WiFi scan timed out\n");
            }
            return;
        }
        s_scan_done    = false;
        s_scan_running = false;
        uint8_t ch = scan_find_tes_channel();
        if (ch) {
            uint8_t from = s_chan;
            set_channel(ch);
            printf("ESP-NOW: TES's network %.*s is on channel %u (from %u)%s\n", (int)s_net.ssid_len,
                   (const char*)s_net.ssid, (unsigned)ch, (unsigned)from,
                   s_chan == ch ? "" : " - cannot move, a phone on the hotspot blocks it");
            s_scan_hold_until = now + LINK_SCAN_HOLD_MS;
            s_hop_ms = now;
            psu_sess_kick(&s_sess, now);   // 到了就喊一聲
        } else {
            printf("ESP-NOW: TES's network %.*s not found - searching channel by channel\n",
                   (int)s_net.ssid_len, (const char*)s_net.ssid);
        }
        return;
    }
    if ((int32_t)(s_scan_hold_until - now) > 0) {
        if (psu_sess_handshaking(&s_sess, now)) return;
        if (now - s_hop_ms >= LINK_HOP_MS) { s_hop_ms = now; psu_sess_kick(&s_sess, now); }
        return;
    }
    if (s_net_valid && now - s_link_ok_ms >= LINK_SCAN_AFTER_MS && !psu_sess_handshaking(&s_sess, now) &&
        (!s_scan_ever || now - s_scan_last_ms >= LINK_RESCAN_MS)) {
        wifi_scan_config_t sc = {};
        sc.show_hidden = true;   // 隱藏 SSID 的路由器：信標裡沒名字，但 BSSID 對得上
        if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
            s_scan_done     = false;
            s_scan_running  = true;
            s_scan_start_ms = now;
            s_scan_last_ms  = now;
            s_scan_ever     = true;
            return;
        }
    }
    // 握手還沒成功：先給目前頻道 LINK_SCAN_AFTER_MS，之後開始找。
    // 快掃一輪（每頻道 250 ms，跳過去就喊 SHR）找不到，下一輪改慢掃 —— 舊版 TES 不會馬上回 SH1。
    // 握手做到一半（收到 SH1、等 SH3）不跳
    if (psu_sess_handshaking(&s_sess, now)) return;
    bool fast = (s_link_hops / CH_MAX) % 2 == 0;
    if (now - s_link_ok_ms >= LINK_SCAN_AFTER_MS && now - s_hop_ms >= (fast ? LINK_HOP_FAST_MS : LINK_HOP_MS)) {
        s_hop_ms = now;
        s_link_hops++;
        hop_channel();
        psu_sess_kick(&s_sess, now);   // 一到新頻道就喊一聲
    }
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
        s_hop_ms  = now;
        printf("ESP-NOW: pairing started - broadcasting key on channels %d-%d\n", CH_MIN, CH_MAX);
    }
    if (s_pair_on) {
        // 還在找 TES：換頻道。TES 回應之後（EXCHANGING 起）就停在這個頻道
        if (s_pair.state == PSU_PAIR_SEARCHING && now - s_hop_ms >= PAIR_HOP_MS) {
            s_hop_ms = now;
            hop_channel();
        }
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
    link_channel_poll(now);
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

    void followTesChannel(uint8_t ch) override { follow_tes_channel(ch); }
    void rememberTesNetwork(const uint8_t* bssid, const uint8_t* ssid, uint8_t len) override {
        remember_tes_network(bssid, ssid, len);
    }
    uint8_t getLinkChannel() override { return s_chan; }
    uint8_t getBlockedChannel() override { return s_chan_blocked; }

    bool settingGetU16(const char* key, uint16_t* out) override {
        nvs_handle_t h;
        if (nvs_open("psu_cfg", NVS_READONLY, &h) != ESP_OK) return false;
        bool ok = nvs_get_u16(h, key, out) == ESP_OK;
        nvs_close(h);
        return ok;
    }

    void settingSetU16(const char* key, uint16_t value) override {
        nvs_handle_t h;
        if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u16(h, key, value);
        nvs_commit(h);
        nvs_close(h);
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
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_scan_done, nullptr));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

        // 設定 SoftAP：SSID/密碼、最多 3 台客戶端
        wifi_config_t ap_cfg = {};
        strncpy((char*)ap_cfg.ap.ssid,     AP_SSID, sizeof(ap_cfg.ap.ssid));
        strncpy((char*)ap_cfg.ap.password, AP_PASS,  sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
        ap_cfg.ap.max_connection = 3;
        ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.channel        = s_chan;   // 配對時存下的 TES 頻道；沒存過就是 1
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
