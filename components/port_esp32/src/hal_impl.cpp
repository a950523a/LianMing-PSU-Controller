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
#include <string.h>
#include <stdio.h>
#include <u8g2.h>

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
static bool    s_tes_mac_valid  = false;

// RX ring buffer (single-producer ESP-NOW task, single-consumer main task)
#define ESPNOW_RX_BUF_SIZE 256
static uint8_t           s_rx_buf[ESPNOW_RX_BUF_SIZE];
static int               s_rx_head  = 0;
static int               s_rx_tail  = 0;
static SemaphoreHandle_t s_rx_mutex = NULL;

// Pairing
static volatile bool     s_pairing_active   = false;
static uint32_t          s_pairing_start_ms = 0;
static esp_timer_handle_t s_pairing_timer   = NULL;

// ──────────────────────────────────────────────────────────────
// NVS helpers
// ──────────────────────────────────────────────────────────────

static void nvs_load_config() {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READONLY, &h) != ESP_OK) return;

    int32_t t = 0;
    if (nvs_get_i32(h, "transport", &t) == ESP_OK) s_transport = (int)t;

    size_t len = 6;
    if (nvs_get_blob(h, "tes_mac", s_tes_mac, &len) == ESP_OK) {
        for (int i = 0; i < 6; i++) {
            if (s_tes_mac[i]) { s_tes_mac_valid = true; break; }
        }
    }
    nvs_close(h);
}

static void nvs_save_transport(int mode) {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "transport", (int32_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_tes_mac(const uint8_t *mac) {
    nvs_handle_t h;
    if (nvs_open("psu_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tes_mac", mac, 6);
    nvs_commit(h);
    nvs_close(h);
}

// ──────────────────────────────────────────────────────────────
// ESP-NOW ring buffer ops (call with mutex held)
// ──────────────────────────────────────────────────────────────

static void rx_push_locked(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        int next = (s_rx_head + 1) % ESPNOW_RX_BUF_SIZE;
        if (next != s_rx_tail) {          // drop if full
            s_rx_buf[s_rx_head] = data[i];
            s_rx_head = next;
        }
    }
}

static int rx_available_locked() {
    return (s_rx_head - s_rx_tail + ESPNOW_RX_BUF_SIZE) % ESPNOW_RX_BUF_SIZE;
}

static int rx_pop_locked() {
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

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len) {
    // 配對視窗中收到第一個封包 → TES 已記錄我方 MAC
    if (s_pairing_active) {
        const uint8_t *src = info->src_addr;
        memcpy(s_tes_mac, src, 6);
        s_tes_mac_valid = true;
        nvs_save_tes_mac(src);
        espnow_add_peer(src);
        s_pairing_active = false;
        if (s_pairing_timer) esp_timer_stop(s_pairing_timer);
        printf("ESP-NOW: Paired! TES=" MACSTR "\n", MAC2STR(src));
    }

    // 正常 transport 資料 → 放入 RX buffer（供 SerialCmd 解析）
    if (s_rx_mutex) {
        xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
        rx_push_locked(data, len);
        xSemaphoreGive(s_rx_mutex);
    }
}

// 每 500 ms 廣播 PSU_HELLO，超時自動停止
static void pairing_timer_cb(void *) {
    if (!s_pairing_active) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_pairing_start_ms >= 10000) {
        s_pairing_active = false;
        esp_timer_stop(s_pairing_timer);
        printf("ESP-NOW: Pairing timed out\n");
        return;
    }

    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const char    hello[]      = "PSU_HELLO\n";
    esp_now_send(broadcast, (const uint8_t *)hello, sizeof(hello) - 1);
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

    void uartSend(const char *str) override {
        if (s_transport == TRANSPORT_ESPNOW) {
            if (s_tes_mac_valid) {
                esp_now_send(s_tes_mac, (const uint8_t *)str, strlen(str));
            }
            return;
        }
        uart_write_bytes(CMD_UART_PORT, str, strlen(str));
    }

    int uartRead() override {
        if (s_transport == TRANSPORT_ESPNOW) {
            if (!s_rx_mutex) return -1;
            xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
            int c = rx_pop_locked();
            xSemaphoreGive(s_rx_mutex);
            return c;
        }
        uint8_t data;
        int len = uart_read_bytes(CMD_UART_PORT, &data, 1, 0);
        return (len > 0) ? data : -1;
    }

    int uartAvailable() override {
        if (s_transport == TRANSPORT_ESPNOW) {
            if (!s_rx_mutex) return 0;
            xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
            int avail = rx_available_locked();
            xSemaphoreGive(s_rx_mutex);
            return avail;
        }
        size_t size;
        uart_get_buffered_data_len(CMD_UART_PORT, &size);
        return (int)size;
    }

    // ── Transport config ─────────────────────────────────────

    void triggerPairing() override {
        if (!s_pairing_timer) return;
        esp_timer_stop(s_pairing_timer);  // 若已在跑則重置
        s_pairing_active   = true;
        s_pairing_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_timer_start_periodic(s_pairing_timer, 500 * 1000ULL);
        printf("ESP-NOW: Pairing started (10s window)\n");
    }

    bool isPairingActive() override {
        return s_pairing_active;
    }

    void setTransport(int mode) override {
        s_transport = mode;
        nvs_save_transport(mode);
        printf("HAL: Transport -> %d\n", mode);
    }

    int getTransport() override {
        return s_transport;
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
            .flags = { .backup_before_sleep = 0 },
        };
        uart_driver_install(CMD_UART_PORT, 1024, 0, 0, NULL, 0);
        uart_param_config(CMD_UART_PORT, &uart_config);
        uart_set_pin(CMD_UART_PORT, CMD_UART_TX, CMD_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    void initWifiAndEspNow() {
        s_rx_mutex = xSemaphoreCreateMutex();

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

        // 廣播 peer（配對 hello 需要）
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        espnow_add_peer(bcast);

        // 若已有配對 MAC，預先加入
        if (s_tes_mac_valid) {
            espnow_add_peer(s_tes_mac);
        }

        // 建立配對 timer（尚未啟動）
        esp_timer_create_args_t ta = {
            .callback             = pairing_timer_cb,
            .arg                  = NULL,
            .dispatch_method      = ESP_TIMER_TASK,
            .name                 = "pair_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&ta, &s_pairing_timer));
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
