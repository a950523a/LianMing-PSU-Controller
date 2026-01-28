#ifndef PORT_DEF_H
#define PORT_DEF_H

#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>

// --- CAN Bus (TWAI) ---
#define PIN_CAN_TX      GPIO_NUM_5
#define PIN_CAN_RX      GPIO_NUM_4

// --- User Buttons (Active Low) ---
#define PIN_BTN_SEL     GPIO_NUM_12
#define PIN_BTN_UP      GPIO_NUM_13
#define PIN_BTN_DOWN    GPIO_NUM_14

// --- UART (Serial Communication) ---
#define UART_PORT_NUM   UART_NUM_0
#define PIN_UART_TX     UART_PIN_NO_CHANGE 
#define PIN_UART_RX     UART_PIN_NO_CHANGE
#define UART_BAUD_RATE  115200

#define CMD_UART_PORT   UART_NUM_2      
#define CMD_UART_TX     GPIO_NUM_17
#define CMD_UART_RX     GPIO_NUM_16
#define CMD_UART_BAUD   115200

// --- I2C (OLED Display) ---
#define I2C_PORT        I2C_NUM_0
#define PIN_SDA         GPIO_NUM_22
#define PIN_SCL         GPIO_NUM_21
#define I2C_SPEED_HZ       400000
#define OLED_ADDR       0x3C

#endif // PORT_DEF_H