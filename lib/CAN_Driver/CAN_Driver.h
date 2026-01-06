#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include <driver/twai.h>
#include "Config.h"

class CAN_Driver {
public:
    bool begin();
    bool send(uint32_t id, uint8_t* data, uint8_t len);
    bool receive(uint32_t &id, uint8_t* data, uint8_t &len);
};

extern CAN_Driver CanBus;

#endif