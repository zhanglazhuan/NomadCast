/*
 * Touch Panel BSP — GT911 Driver (I2C)
 *
 * C++ class wrapper for the GT911 capacitive touch controller
 * used on the Leisound V1 board.
 *
 * Reference: debug/t_display_touch/ (working GT911 implementation)
 */

#ifndef TOUCH_BSP_H
#define TOUCH_BSP_H

#include "i2c_bsp.h"
#include "driver/gpio.h"

class LcdTouchPanel
{
private:
    I2cMasterBus& i2cbus_;
    i2c_master_dev_handle_t touch_dev_handle_;
    int touch_rst_pin_;
    int touch_int_pin_;
    uint8_t dev_addr_;

    bool probe_gt911(void);
    bool i2c_read_reg16(uint16_t reg, uint8_t *data, size_t len);
    bool i2c_write_reg16(uint16_t reg, const uint8_t *data, size_t len);

public:
    LcdTouchPanel(I2cMasterBus& i2cbus, int dev_addr = 0x5D,
                  int touch_rst_pin = GPIO_NUM_NC,
                  int touch_int_pin = GPIO_NUM_NC);
    ~LcdTouchPanel();

    uint8_t GetCoords(uint16_t *x, uint16_t *y);
    void ResetTouch(void);
};

#endif
