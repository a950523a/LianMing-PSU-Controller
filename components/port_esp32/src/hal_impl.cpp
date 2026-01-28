#include "hal_interface.h"
#include "port_def.h"

#include <driver/gpio.h>
#include <driver/twai.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <esp_rom_sys.h> // ESP-IDF v5.x 延遲函數
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <u8g2.h>

// --- Global Handles for New I2C Driver ---
static i2c_master_bus_handle_t  g_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t  g_oled_handle = NULL;

// --- U8g2 Callback Functions (C Style) ---

// 1. GPIO & Delay Callback
uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            // I2C 初始化已在 HAL 類別中完成，這裡不做事
            break;
        case U8X8_MSG_DELAY_MILLI:
            // 這裡呼叫 vTaskDelay，必須確保 FreeRTOS 已經啟動
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;
        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(arg_int * 10);
            break;
        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1);
            break;
        // I2C 硬體驅動不需要手動控制 GPIO (DC/RESET/CS)
        default:
            return 0;
    }
    return 1;
}

// 2. I2C Byte Callback (Adapted for ESP-IDF v5.x I2C Master)
// U8g2 的傳輸模式是: Start -> Send Bytes... -> End
// 新版 ESP-IDF 需要一次性傳輸，所以我們需要一個 Buffer
#define I2C_BUFFER_SIZE 256
static uint8_t i2c_buffer[I2C_BUFFER_SIZE];
static size_t i2c_buffer_len = 0;

uint8_t u8x8_byte_esp32_hw_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_BYTE_SEND: {
            // 將資料暫存到 Buffer
            uint8_t *data = (uint8_t *)arg_ptr;
            if (i2c_buffer_len + arg_int < I2C_BUFFER_SIZE) {
                memcpy(&i2c_buffer[i2c_buffer_len], data, arg_int);
                i2c_buffer_len += arg_int;
            }
            break;
        }
        case U8X8_MSG_BYTE_START_TRANSFER:
            // 開始傳輸前，清空 Buffer
            i2c_buffer_len = 0;
            break;
            
        case U8X8_MSG_BYTE_END_TRANSFER:
            // 結束傳輸時，一次性發送 Buffer 內容
            if (g_oled_handle && i2c_buffer_len > 0) {
                // 使用新版 API 發送
                i2c_master_transmit(g_oled_handle, i2c_buffer, i2c_buffer_len, -1);
            }
            break;
            
        case U8X8_MSG_BYTE_INIT:
        case U8X8_MSG_BYTE_SET_DC:
            break;
        default:
            return 0;
    }
    return 1;
}

// --- HAL Implementation ---

class Esp32HAL : public IHardwareHAL {
public:
    // [重要] 建構子留空，不要在這裡做硬體初始化
    Esp32HAL() {}

    // [重要] 新增 init 實作，由 app_main 呼叫
    void init() override {
        printf("HAL: Init GPIO...\n");
        initGpio();
        
        printf("HAL: Init CAN...\n");
        initCan();
        
        printf("HAL: Init UART...\n");
        initUart();
        
        printf("HAL: Init I2C & OLED...\n");
        initI2cAndU8g2();
        
        printf("HAL: Init Done!\n");
    }

    // System
    uint32_t getTickCount() override {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void delayMs(uint32_t ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    // GPIO
    bool readButton(HalButton btn) override {
        gpio_num_t pin;
        switch(btn) {
            case BTN_SELECT: pin = PIN_BTN_SEL; break;
            case BTN_UP:     pin = PIN_BTN_UP; break;
            case BTN_DOWN:   pin = PIN_BTN_DOWN; break;
            default: return false;
        }
        // Active Low
        return gpio_get_level(pin) == 0;
    }

    // CAN
    bool canSend(const HalCanFrame& frame) override {
        twai_message_t msg;
        msg.identifier = frame.id;
        msg.extd = frame.ext;
        msg.data_length_code = frame.len;
        memcpy(msg.data, frame.data, frame.len);
        return twai_transmit(&msg, 0) == ESP_OK;
    }

    bool canReceive(HalCanFrame& frame) override {
        twai_message_t msg;
        if (twai_receive(&msg, 0) == ESP_OK) {
            frame.id = msg.identifier;
            frame.ext = msg.extd;
            frame.len = msg.data_length_code;
            memcpy(frame.data, msg.data, msg.data_length_code);
            return true;
        }
        return false;
    }

    // UART
    void uartSend(const char* str) override {
        uart_write_bytes(CMD_UART_PORT, str, strlen(str));
    }

    int uartRead() override {
        uint8_t data;
        int len = uart_read_bytes(CMD_UART_PORT, &data, 1, 0);
        if (len > 0) return data;
        return -1;
    }
    
    int uartAvailable() override {
        size_t size;
        uart_get_buffered_data_len(CMD_UART_PORT, &size);
        return (int)size;
    }

    // Display (U8g2)
    void displayClear() override {
        u8g2_ClearBuffer(&_u8g2);
    }

    void displayDrawString(int x, int y, const char* str, int fontSize) override {
        if (fontSize == 0) {
            u8g2_SetFont(&_u8g2, u8g2_font_6x10_tf);
        } else {
            u8g2_SetFont(&_u8g2, u8g2_font_profont17_tf);
        }
        // U8g2 座標系是 Baseline，這裡 +8 讓它行為接近左上角座標系
        u8g2_DrawStr(&_u8g2, x, y + 8, str);
    }

    void displayShow() override {
        u8g2_SendBuffer(&_u8g2);
    }

private:
    u8g2_t _u8g2;

    void initGpio() {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL<<PIN_BTN_SEL) | (1ULL<<PIN_BTN_UP) | (1ULL<<PIN_BTN_DOWN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }

    void initCan() {
        twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(PIN_CAN_TX, PIN_CAN_RX, TWAI_MODE_NORMAL);
        twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
        twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        twai_driver_install(&g_config, &t_config, &f_config);
        twai_start();
    }

    void initUart() {
        // 初始化 Command UART (UART2)
        uart_config_t uart_config = {
            .baud_rate = CMD_UART_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 122,
            .source_clk = UART_SCLK_DEFAULT,
            // [修正] 完整初始化 flags 結構
            .flags = {
                .backup_before_sleep = 0, // 補上這個
            }
        };
        
        uart_driver_install(CMD_UART_PORT, 1024, 0, 0, NULL, 0);
        uart_param_config(CMD_UART_PORT, &uart_config);
        uart_set_pin(CMD_UART_PORT, CMD_UART_TX, CMD_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    void initI2cAndU8g2() {
        // 1. New I2C Master Bus Init
        i2c_master_bus_config_t i2c_mst_config = {
            .i2c_port = I2C_PORT,
            .sda_io_num = PIN_SDA,
            .scl_io_num = PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            // [修正] 完整初始化 flags 結構
            .flags = {
                .enable_internal_pullup = 1,
                .allow_pd = 0, // 補上這個 (Allow Power Down)
            }
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &g_i2c_bus_handle));

        // 2. Add OLED Device
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_ADDR,
            .scl_speed_hz = I2C_SPEED_HZ, // 這裡對應 port_def.h 的定義
            .scl_wait_us = 0,             // 建議補上這個初始化
            .flags = { .disable_ack_check = 0 }, // 建議補上這個初始化
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg, &g_oled_handle));

        // 3. U8g2 Init
        u8g2_Setup_ssd1306_i2c_128x64_noname_f(
            &_u8g2,
            U8G2_R0,
            u8x8_byte_esp32_hw_i2c,
            u8x8_gpio_and_delay_esp32
        );

        u8x8_SetI2CAddress(&_u8g2.u8x8, OLED_ADDR << 1);
        u8g2_InitDisplay(&_u8g2);
        u8g2_SetPowerSave(&_u8g2, 0);
        
        // 測試畫面
        u8g2_ClearBuffer(&_u8g2);
        u8g2_SetFont(&_u8g2, u8g2_font_6x10_tf);
        u8g2_DrawStr(&_u8g2, 0, 10, "System Ready");
        u8g2_SendBuffer(&_u8g2);
    }
};

// Global HAL Instance
Esp32HAL g_hal;
IHardwareHAL* getHal() { return &g_hal; }