#include "CAN_Driver.h"

CAN_Driver CanBus;

bool CAN_Driver::begin() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    // 協議要求 125kbps
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS(); 
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) return false;
    if (twai_start() != ESP_OK) return false;
    return true;
}

bool CAN_Driver::send(uint32_t id, uint8_t* data, uint8_t len) {
    twai_message_t message;
    message.identifier = id;
    message.extd = 1; // 29-bit 擴展幀
    message.data_length_code = len;
    for (int i = 0; i < len; i++) message.data[i] = data[i];
    return twai_transmit(&message, pdMS_TO_TICKS(10)) == ESP_OK;
}

bool CAN_Driver::receive(uint32_t &id, uint8_t* data, uint8_t &len) {
    twai_message_t message;
    if (twai_receive(&message, 0) == ESP_OK) {
        id = message.identifier;
        len = message.data_length_code;
        for (int i = 0; i < len; i++) data[i] = message.data[i];
        return true;
    }
    return false;
}