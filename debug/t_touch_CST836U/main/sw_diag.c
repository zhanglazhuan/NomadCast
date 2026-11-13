/* Independent open-drain I2C diagnostic. No hardware controller is active. */
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nomadcast.h"
#define SDA NOMADCAST_PIN_I2C_SDA
#define SCL NOMADCAST_PIN_I2C_SCL
static const char *TAG = "CST836U_SW";
static void pause_bit(void) { esp_rom_delay_us(50); }
static bool high_clock(void)
{
    gpio_set_level(SCL, 1);
    int64_t until = esp_timer_get_time() + 2000;
    while (!gpio_get_level(SCL)) {
        if (esp_timer_get_time() >= until) return false;
    }
    pause_bit();
    return true;
}
static bool stop_bus(void)
{
    gpio_set_level(SCL, 0);
    gpio_set_level(SDA, 0); pause_bit();
    bool ok = high_clock();
    gpio_set_level(SDA, 1); pause_bit();
    return ok && gpio_get_level(SDA);
}
static bool start_bus(void)
{
    gpio_set_level(SDA, 1); pause_bit();
    if (!high_clock() || !gpio_get_level(SDA)) return false;
    gpio_set_level(SDA, 0); pause_bit();
    gpio_set_level(SCL, 0); pause_bit();
    return true;
}
/* 1=ACK, 0=NACK, -1=clock timeout. */
static int send_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; --i) {
        gpio_set_level(SDA, (byte >> i) & 1); pause_bit();
        if (!high_clock()) return -1;
        gpio_set_level(SCL, 0); pause_bit();
    }
    gpio_set_level(SDA, 1); pause_bit();
    if (!high_clock()) return -1;
    int ack = !gpio_get_level(SDA);
    gpio_set_level(SCL, 0); pause_bit();
    return ack;
}
static bool receive_byte(uint8_t *byte, bool ack)
{
    *byte = 0;
    gpio_set_level(SDA, 1);
    for (int i = 0; i < 8; ++i) {
        pause_bit();
        if (!high_clock()) return false;
        *byte = (*byte << 1) | gpio_get_level(SDA);
        gpio_set_level(SCL, 0); pause_bit();
    }
    gpio_set_level(SDA, !ack); pause_bit();
    if (!high_clock()) return false;
    gpio_set_level(SCL, 0); pause_bit();
    gpio_set_level(SDA, 1);
    return true;
}
static void address_check(uint8_t address)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool started = start_bus();
        int ack = started ? send_byte(address << 1) : -2;
        bool stopped = stop_bus();
        ESP_LOGI(TAG, "addr=0x%02x attempt=%d ACK=%d STOP=%d SDA=%d SCL=%d (1=ACK,0=NACK,-1=clock timeout,-2=bus not idle)",
                 address, attempt + 1, ack, stopped, gpio_get_level(SDA), gpio_get_level(SCL));
    }
}
void cst836u_software_diagnostic(void)
{
    gpio_config_t pins = {
        .pin_bit_mask = BIT64(SDA) | BIT64(SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pins));
    gpio_set_level(SDA, 1); gpio_set_level(SCL, 1); pause_bit();
    ESP_LOGI(TAG, "Before recovery: SDA=%d SCL=%d", gpio_get_level(SDA), gpio_get_level(SCL));
    for (int i = 0; i < 9; ++i) {
        gpio_set_level(SCL, 0); pause_bit();
        if (!high_clock()) break;
    }
    ESP_LOGI(TAG, "Recovery STOP=%d", stop_bus());
    address_check(0x15);
    address_check(0x08); /* Previously observed address, not an identity assertion. */
    bool started = start_bus();
    int address_ack = started ? send_byte(0x15 << 1) : -2;
    int register_ack = address_ack == 1 ? send_byte(0x02) : -2;
    int read_ack = -2;
    uint8_t raw[5] = {0};
    bool complete = false;
    if (register_ack == 1 && start_bus()) {
        read_ack = send_byte((0x15 << 1) | 1);
        if (read_ack == 1) {
            complete = true;
            for (int i = 0; i < 5; ++i) {
                if (!receive_byte(&raw[i], i < 4)) { complete = false; break; }
            }
        }
    }
    bool stopped = stop_bus();
    ESP_LOGI(TAG, "reg02/read5: addr_ACK=%d reg_ACK=%d read_ACK=%d complete=%d STOP=%d",
             address_ack, register_ack, read_ack, complete, stopped);
    if (complete) ESP_LOG_BUFFER_HEX_LEVEL(TAG, raw, sizeof(raw), ESP_LOG_INFO);
    ESP_ERROR_CHECK(gpio_reset_pin(SDA));
    ESP_ERROR_CHECK(gpio_reset_pin(SCL));
}
