/*
 * OTA-Forever loader.
 *
 * Lives in the "factory" partition and is never overwritten by firmware
 * updates or normal USB uploads. On every power-on/reset it:
 *   1. opens the setup portal if it isn't configured (or the BOOT button is held),
 *   2. connects to WiFi and installs a newer signed build from GitHub, if any,
 *   3. starts the user firmware.
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "button.h"
#include "config.h"
#include "launcher.h"
#include "portal.h"
#include "updater.h"
#include "wifi.h"

static const char *TAG = "loader";

static void wait_for_grace_period(void)
{
    while (esp_timer_get_time() / 1000 < CONFIG_OTAF_BOOT_GRACE_MS && !otaf_button_requested()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "OTA-Forever loader %s on %s (reset reason %d)",
             esp_app_get_description()->version, CONFIG_IDF_TARGET, (int)esp_reset_reason());

    ESP_ERROR_CHECK(otaf_config_init());
    otaf_config_t cfg;
    otaf_config_load(&cfg);
    otaf_button_start();
    otaf_wifi_init();

    for (;;) {
        if (!otaf_config_is_provisioned(&cfg) || otaf_button_requested()) {
            otaf_button_clear();
            otaf_portal_run(&cfg, 0);
        }

        if (otaf_wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, CONFIG_OTAF_WIFI_TIMEOUT_MS)) {
            otaf_check_and_update(&cfg);
        }
        otaf_wifi_stop();

        wait_for_grace_period();
        if (otaf_button_requested()) {
            continue;
        }

        const esp_partition_t *app = otaf_find_user_app();
        if (app) {
            otaf_launch(app);  // does not return
        }

        ESP_LOGW(TAG, "no user firmware installed yet; push code to github.com/%s (branch %s). "
                      "Setup portal open for 3 minutes, then retrying.", cfg.repo, cfg.channel);
        otaf_portal_run(&cfg, 3 * 60 * 1000);
    }
}
