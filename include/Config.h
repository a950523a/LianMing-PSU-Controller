#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// CAN Bus Pins (TWAI)
// 請確認您的 CAN 收發器接腳
#define CAN_TX_PIN      GPIO_NUM_5
#define CAN_RX_PIN      GPIO_NUM_4

// I2C OLED Pins
#define OLED_SDA        22
#define OLED_SCL        21

// Buttons (Active Low, 使用內建上拉電阻)
// Select: 切換模式, Up/Down: 調整數值
#define BTN_SELECT_PIN  12
#define BTN_UP_PIN      13
#define BTN_DOWN_PIN    14

// 電源模組地址 (1~60)
#define PSU_ADDRESS     1

#endif