/*
 * NomadCast — Key Button Test (event printing only)
 *
 *   Button | GPIO | 功能
 *   -------|------|------
 *   Vol-   |   6  | 音量减
 *   Vol+   |  17  | 音量加
 *   Stop   |   7  | 停止
 *   OnOff  |   5  | 开关
 *   HP-DET |  18  | 耳机插入检测
 *
 * 仅响应按键事件并打印，不做真实音频 / 音量 / 播放控制。
 *
 * === Leisound V1 Pins ===
 *   EN_PWR   = GPIO46   全板外设电源
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "t_key";

/* ========================================================================
 * Pin Definitions
 * ======================================================================== */

#define BTN_VOL_DOWN    GPIO_NUM_6
#define BTN_VOL_UP      GPIO_NUM_17
#define BTN_STOP        GPIO_NUM_7
#define BTN_ONOFF       GPIO_NUM_5
#define PIN_HP_DET      GPIO_NUM_18   /* 耳机插入检测 (AMP_EN) */
#define PIN_EN_POWER    GPIO_NUM_46

/* ========================================================================
 * Button Handling
 * ======================================================================== */

#define DEBOUNCE_MS     50

static QueueHandle_t gpio_evt_queue = NULL;
static int s_last_hp_level = -1;   /* 上一次稳定的耳机电平，-1=未初始化 */

/* 防抖读取：要求电平稳定 DEBOUNCE_MS 才返回，过滤机械触点抖动 */
static int debounced_level(gpio_num_t pin)
{
    int level = gpio_get_level(pin);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        int l = gpio_get_level(pin);
        if (l == level) {
            return level;   /* 稳定 DEBOUNCE_MS，返回 */
        }
        level = l;           /* 还在抖，重新计时 */
    }
}

static const char *btn_name(uint32_t gpio_num)
{
    switch (gpio_num) {
        case BTN_VOL_DOWN: return "Vol-";
        case BTN_VOL_UP:   return "Vol+";
        case BTN_STOP:     return "Stop";
        case BTN_ONOFF:    return "OnOff";
        default:           return "Unknown";
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void btn_task(void *arg)
{
    (void)arg;
    uint32_t gpio_num;

    /* 上电等待耳机检测电路稳定（~700ms 启动抖动），以稳定电平为基线，
     * 并丢弃启动期间积压的抖动事件 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_last_hp_level = debounced_level(PIN_HP_DET);
    while (xQueueReceive(gpio_evt_queue, &gpio_num, 0) == pdTRUE) { /* discard */ }
    ESP_LOGI(TAG, "HP baseline: %s", s_last_hp_level ? "INSERTED" : "REMOVED");

    while (1) {
        if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
            /* 耳机插入检测：双边沿 + 防抖 + 只在电平变化时打印 */
            if (gpio_num == PIN_HP_DET) {
                int level = debounced_level(PIN_HP_DET);
                if (level != s_last_hp_level) {
                    s_last_hp_level = level;
                    ESP_LOGI(TAG, "[HP] headphone %s (GPIO18=%d)",
                             level ? "INSERTED" : "REMOVED", level);
                }
                continue;
            }

            /* 按键：防抖 + 只认高电平（按下） */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(gpio_num) != 1) continue;

            ESP_LOGI(TAG, "[BTN] %s pressed (GPIO%d)", btn_name(gpio_num), (int)gpio_num);
        }
    }
}

/* ========================================================================
 * Hardware Init
 * ======================================================================== */

static void hw_init(void)
{
    gpio_config_t pwr = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(PIN_EN_POWER) };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "HW: EN_POWER(46)=HIGH");
}

/* ========================================================================
 * Button GPIO Init
 * ======================================================================== */

static void btn_init(void)
{
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    ESP_ERROR_CHECK(gpio_evt_queue == NULL ? ESP_FAIL : ESP_OK);

    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_POSEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_VOL_DOWN) |
                        (1ULL << BTN_VOL_UP)   |
                        (1ULL << BTN_STOP)     |
                        (1ULL << BTN_ONOFF),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* 耳机插入检测 (AMP_EN=GPIO18)：双边沿 + 悬空（板子自带上下拉，ESP32 别抢） */
    gpio_config_t hp_conf = {
        .intr_type    = GPIO_INTR_ANYEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_HP_DET),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&hp_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_VOL_DOWN, gpio_isr_handler, (void *)BTN_VOL_DOWN));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_VOL_UP,   gpio_isr_handler, (void *)BTN_VOL_UP));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_STOP,     gpio_isr_handler, (void *)BTN_STOP));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_ONOFF,    gpio_isr_handler, (void *)BTN_ONOFF));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_HP_DET,   gpio_isr_handler, (void *)PIN_HP_DET));

    xTaskCreate(btn_task, "btn_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Buttons ready — Vol-:IO6, Vol+:IO17, Stop:IO7, OnOff:IO5, HP:IO18");
}

/* ========================================================================
 * Main
 * ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_key: Button Event Test ===");

    hw_init();
    btn_init();

    ESP_LOGI(TAG, "Press any button — event will be printed");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
