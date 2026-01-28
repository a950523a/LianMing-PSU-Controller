#include "hal_interface.h"
#include "port_def.h"

#include <driver/gpio.h>
#include <driver/twai.h>
#include <driver/uart.h>
#include <driver/i2c.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <u8g2.h> // 引入 U8g2

// --- U8g2 Callback Functions (C Style) ---

// 1. GPIO & Delay Callback
uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            // I2C init is done in HAL class, nothing to do here for I2C OLED
            break;
        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;
        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(arg_int * 10);
            break;
        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1); // Min delay
            break;
        // I2C HW doesn't need manual GPIO toggling
        default:
            return 0;
    }
    return 1;
}

// 2. I2C Byte Callback (Hardware I2C)
uint8_t u8x8_byte_esp32_hw_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static i2c_cmd_handle_t handle_i2c;

    switch (msg) {
        case U8X8_MSG_BYTE_SEND: {
            uint8_t *data = (uint8_t *)arg_ptr;
            while (arg_int > 0) {
                i2c_master_write_byte(handle_i2c, *data, true);
                data++;
                arg_int--;
            }
            break;
        }
        case U8X8_MSG_BYTE_INIT:
            // I2C init is done in HAL class
            break;
        case U8X8_MSG_BYTE_SET_DC:
            break;
        case U8X8_MSG_BYTE_START_TRANSFER: {
            uint8_t i2c_address = u8x8_GetI2CAddress(u8x8);
            handle_i2c = i2c_cmd_link_create();
            i2c_master_start(handle_i2c);
            i2c_master_write_byte(handle_i2c, (i2c_address << 1) | I2C_MASTER_WRITE, true);
            break;
        }
        case U8X8_MSG_BYTE_END_TRANSFER:
            i2c_master_stop(handle_i2c);
            i2c_master_cmd_begin(I2C_PORT, handle_i2c, pdMS_TO_TICKS(100));
            i2c_cmd_link_delete(handle_i2c);
            break;
        default:
            return 0;
    }
    return 1;
}

// --- HAL Implementation ---

class Esp32HAL : public IHardwareHAL {
public:
    Esp32HAL() {
        initGpio();
        initCan();
        initUart();
        initI2cAndU8g2(); // 初始化 I2C 和 U8g2
    }

    // ... (System, GPIO, CAN, UART 實作保持不變，省略以節省篇幅) ...
    // 請複製之前的 getTickCount, delayMs, readButton, canSend, canReceive, uart* 函數

    uint32_t getTickCount() override { return (uint32_t)(esp_timer_get_time() / 1000); }
    void delayMs(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
    
    bool readButton(HalButton btn) override {
        gpio_num_t pin;
        switch(btn) {
            case BTN_SELECT: pin = PIN_BTN_SEL; break;
            case BTN_UP:     pin = PIN_BTN_UP; break;
            case BTN_DOWN:   pin = PIN_BTN_DOWN; break;
            default: return false;
        }
        return gpio_get_level(pin) == 0;
    }

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

    void uartSend(const char* str) override { uart_write_bytes(UART_PORT_NUM, str, strlen(str)); }
    int uartRead() override {
        uint8_t data;
        if (uart_read_bytes(UART_PORT_NUM, &data, 1, 0) > 0) return data;
        return -1;
    }
    int uartAvailable() override {
        size_t size;
        uart_get_buffered_data_len(UART_PORT_NUM, &size);
        return (int)size;
    }

    // --- Display Implementation using U8g2 ---

    void displayClear() override {
        u8g2_ClearBuffer(&_u8g2);
    }

    void displayDrawString(int x, int y, const char* str, int fontSize) override {
        // 根據 fontSize 選擇字型
        if (fontSize == 0) {
            u8g2_SetFont(&_u8g2, u8g2_font_6x10_tf); // 小字體
        } else {
            u8g2_SetFont(&_u8g2, u8g2_font_profont17_tf); // 大字體
        }
        // U8g2 的 y 座標通常是 Baseline，但為了相容之前的邏輯，我們可能需要微調
        // 這裡直接畫
        u8g2_DrawStr(&_u8g2, x, y + 8, str); // +8 是因為 u8g2 座標在左下角，而通常我們習慣左上角
    }

    void displayShow() override {
        u8g2_SendBuffer(&_u8g2);
    }

private:
    u8g2_t _u8g2; // U8g2 物件

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
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_driver_install(UART_PORT_NUM, 1024, 0, 0, NULL, 0);
        uart_param_config(UART_PORT_NUM, &uart_config);
        uart_set_pin(UART_PORT_NUM, PIN_UART_TX, PIN_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    void initI2cAndU8g2() {
        // 1. ESP32 I2C Driver Init
        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = PIN_SDA;
        conf.scl_io_num = PIN_SCL;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = I2C_SPEED;
        i2c_param_config(I2C_PORT, &conf);
        i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);

        // 2. U8g2 Init
        // 使用 SSD1306 128x64 Noname F (Full buffer)
        // 這裡綁定我們上面寫的兩個 Callback
        u8g2_Setup_ssd1306_i2c_128x64_noname_f(
            &_u8g2,
            U8G2_R0,
            u8x8_byte_esp32_hw_i2c,
            u8x8_gpio_and_delay_esp32
        );

        // 設定 I2C 地址 (U8g2 預設 0x78 = 0x3C << 1)
        u8x8_SetI2CAddress(&_u8g2.u8x8, OLED_ADDR << 1);
        
        u8g2_InitDisplay(&_u8g2);
        u8g2_SetPowerSave(&_u8g2, 0); // Wake up
    }
};

Esp32HAL g_hal;
IHardwareHAL* getHal() { return &g_hal; }