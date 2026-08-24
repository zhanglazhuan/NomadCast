/*
 * Software I2C (bit-banging) master — shared by GT911 touch and ES8156 codec
 *
 * Motivation (Leisound/NomadCast V1.1 board): the touch/codec SCL is routed to
 * GPIO45, a VDD_SPI strapping pin whose default pull-down fights the ESP-IDF
 * hardware I2C driver's weak internal pull-up, producing probe-timeout / no-ACK.
 * Bit-banging drives SCL as push-pull (hard 3.3 V) and SDA as open-drain with an
 * explicit weak pull-up, overriding the strap.
 *
 * This is a single shared bus (one instance). Device drivers call
 * sw_i2c_master_init() (idempotent) then mem_read/mem_write with their own
 * 7-bit address and register width.
 */

#ifndef SW_I2C_H
#define SW_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the software I2C bus on the given pins.
 *
 * Idempotent — safe to call from every device driver sharing the bus.
 * SCL = push-pull output; SDA = open-drain I/O with weak internal pull-up.
 *
 * @param scl_pin  SCL GPIO.
 * @param sda_pin  SDA GPIO.
 * @return ESP_OK.
 */
esp_err_t sw_i2c_master_init(gpio_num_t scl_pin, gpio_num_t sda_pin);

/**
 * @brief Probe a 7-bit address (START + addr|W + STOP), returns ACK.
 */
bool sw_i2c_probe(uint8_t addr7);

/**
 * @brief Write bytes to a register of an I2C device.
 *
 * @param addr7      7-bit device address.
 * @param reg        Register address (big-endian when reg_bytes == 2).
 * @param reg_bytes  Register width in bytes (1 or 2).
 * @param data       Payload to write.
 * @param len        Payload length.
 * @return ESP_OK on success, ESP_FAIL on any NACK.
 */
esp_err_t sw_i2c_mem_write(uint8_t addr7, uint16_t reg, uint8_t reg_bytes,
                           const uint8_t *data, size_t len);

/**
 * @brief Read bytes from a register of an I2C device.
 *
 * @param addr7      7-bit device address.
 * @param reg        Register address (big-endian when reg_bytes == 2).
 * @param reg_bytes  Register width in bytes (1 or 2).
 * @param data       Output buffer.
 * @param len        Number of bytes to read.
 * @return ESP_OK on success, ESP_FAIL on any NACK.
 */
esp_err_t sw_i2c_mem_read(uint8_t addr7, uint16_t reg, uint8_t reg_bytes,
                          uint8_t *data, size_t len);

/**
 * @brief Recover a stuck bus: pulse SCL 9x with SDA released, then STOP.
 */
void sw_i2c_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SW_I2C_H */
