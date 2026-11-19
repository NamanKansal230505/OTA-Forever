#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "button.h"

static volatile bool s_requested;

#if CONFIG_OTAF_BUTTON_GPIO >= 0
static void button_task(void *arg)
{
    int held = 0;
    for (;;) {
        if (gpio_get_level(CONFIG_OTAF_BUTTON_GPIO) == 0) {
            if (++held == 20) {  // 20 x 50 ms
                s_requested = true;
                ESP_LOGW("button", "setup requested");
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void otaf_button_start(void)
{
#if CONFIG_OTAF_BUTTON_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_OTAF_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "otaf_btn", 2048, NULL, 5, NULL);
#endif
}

bool otaf_button_requested(void)
{
    return s_requested;
}

void otaf_button_clear(void)
{
    s_requested = false;
}
