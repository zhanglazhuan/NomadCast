/*
 * Software I2C (bit-banging) master — implementation.
 *
 * Ported from debug/t_touch_bitbanging/main/gt911.c (working bit-bang driver),
 * generalized to support arbitrary register widths so both the GT911 touch
 * (16-bit register addresses) and the ES8156 codec (8-bit register addresses)
 * share the same bus.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "sw_i2c.h"

static const char *TAG = "sw_i2c";

static gpio_num_t s_scl = GPIO_NUM_NC;
static gpio_num_t s_sda = GPIO_NUM_NC;
static SemaphoreHandle_t s_mutex = NULL;

/* ~100 kHz half-bit delay */
static inline void sw_i2c_delay(void)
{
    esp_rom_delay_us(5);
}

/* ---- Low-level bus primitives (caller must hold s_mutex) ---- */

static void sda_out(uint32_t level)
{
    gpio_set_level(s_sda, level);
}

static void scl_out(uint32_t level)
{
    gpio_set_level(s_scl, level);
}

static void sw_i2c_start(void)
{
    sda_out(1);
    scl_out(1);
    sw_i2c_delay();
    sda_out(0);
    sw_i2c_delay();
    scl_out(0);
    sw_i2c_delay();
}

static void sw_i2c_stop(void)
{
    sda_out(0);
    scl_out(1);
    sw_i2c_delay();
    sda_out(1);
    sw_i2c_delay();
}

/* Write one byte MSB-first; returns true on ACK. */
static bool sw_i2c_write_byte(uint8_t data)
{
    for (int i = 0; i < 8; i++) {
        sda_out((data & 0x80) ? 1 : 0);
        data <<= 1;
        sw_i2c_delay();
        scl_out(1);
        sw_i2c_delay();
        scl_out(0);
    }
    /* Read ACK */
    sda_out(1);  /* release SDA */
    sw_i2c_delay();
    scl_out(1);
    sw_i2c_delay();
    bool ack = (gpio_get_level(s_sda) == 0);
    scl_out(0);
    sw_i2c_delay();
    return ack;
}

/* Read one byte MSB-first; ack=true ACKs the byte, false NACKs. */
static uint8_t sw_i2c_read_byte(bool ack)
{
    uint8_t data = 0;
    sda_out(1);  /* release SDA for slave output */
    for (int i = 0; i < 8; i++) {
        data <<= 1;
        sw_i2c_delay();
        scl_out(1);
        sw_i2c_delay();
        if (gpio_get_level(s_sda)) {
            data |= 0x01;
        }
        scl_out(0);
    }
    /* Send ACK/NACK */
    sda_out(ack ? 0 : 1);
    sw_i2c_delay();
    scl_out(1);
    sw_i2c_delay();
    scl_out(0);
    sw_i2c_delay();
    return data;
}

/* ---- Public API ---- */

esp_err_t sw_i2c_master_init(gpio_num_t scl_pin, gpio_num_t sda_pin)
{
    /* Create the mutex once. */
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    /* Reconfigure only if the pins actually changed. */
    if (s_scl == scl_pin && s_sda == sda_pin && s_scl != GPIO_NUM_NC) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scl = scl_pin;
    s_sda = sda_pin;

    gpio_config_t scl_conf = {
        .pin_bit_mask = BIT64(s_scl),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&scl_conf);

    gpio_config_t sda_conf = {
        .pin_bit_mask = BIT64(s_sda),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sda_conf);

    scl_out(1);
    sda_out(1);
    sw_i2c_delay();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "software I2C on SDA=GPIO%d SCL=GPIO%d", (int)sda_pin, (int)scl_pin);
    return ESP_OK;
}

bool sw_i2c_probe(uint8_t addr7)
{
    if (!s_mutex || s_scl == GPIO_NUM_NC) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sw_i2c_start();
    bool ack = sw_i2c_write_byte((uint8_t)(addr7 << 1));
    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
    return ack;
}

esp_err_t sw_i2c_mem_write(uint8_t addr7, uint16_t reg, uint8_t reg_bytes,
                           const uint8_t *data, size_t len)
{
    if (!s_mutex || s_scl == GPIO_NUM_NC) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    sw_i2c_start();
    if (!sw_i2c_write_byte((uint8_t)(addr7 << 1))) goto nack;

    for (int i = reg_bytes - 1; i >= 0; i--) {
        if (!sw_i2c_write_byte((uint8_t)((reg >> (8 * i)) & 0xFF))) goto nack;
    }
    for (size_t i = 0; i < len; i++) {
        if (!sw_i2c_write_byte(data[i])) goto nack;
    }

    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
    return ESP_OK;

nack:
    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
    return ESP_FAIL;
}

esp_err_t sw_i2c_mem_read(uint8_t addr7, uint16_t reg, uint8_t reg_bytes,
                          uint8_t *data, size_t len)
{
    if (!s_mutex || s_scl == GPIO_NUM_NC) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    sw_i2c_start();
    if (!sw_i2c_write_byte((uint8_t)(addr7 << 1))) goto nack;

    for (int i = reg_bytes - 1; i >= 0; i--) {
        if (!sw_i2c_write_byte((uint8_t)((reg >> (8 * i)) & 0xFF))) goto nack;
    }

    /* Repeated start, then read address. */
    sw_i2c_start();
    if (!sw_i2c_write_byte((uint8_t)((addr7 << 1) | 1))) goto nack;

    for (size_t i = 0; i < len; i++) {
        data[i] = sw_i2c_read_byte(i < (len - 1));  /* ACK all but last */
    }

    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
    return ESP_OK;

nack:
    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
    return ESP_FAIL;
}

void sw_i2c_reset(void)
{
    if (!s_mutex || s_scl == GPIO_NUM_NC) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sda_out(1);
    for (int i = 0; i < 9; i++) {
        scl_out(1);
        sw_i2c_delay();
        scl_out(0);
        sw_i2c_delay();
    }
    sw_i2c_start();
    sw_i2c_stop();
    xSemaphoreGive(s_mutex);
}
