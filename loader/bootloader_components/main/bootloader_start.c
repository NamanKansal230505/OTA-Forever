/*
 * OTA-Forever second-stage bootloader.
 *
 * Identical to the stock ESP-IDF bootloader except for one decision: which app
 * to start.
 *
 *   power-on, reset button, brown-out, watchdog  ->  loader (factory partition)
 *   esp_restart() / ESP.restart(), deep-sleep wake ->  app selected in otadata
 *
 * So the loader gets control on every real reset (and can install updates
 * from GitHub) without the user's sketch having to contain any OTA code, while
 * a sketch's own restarts and deep-sleep wake-ups go straight back to it.
 */
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

static const char *TAG = "otaf_boot";

static int select_partition_number(bootloader_state_t *bs);

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // Deep-sleep wake: fast path back into the app that was running.
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // Falls back to the other slots / factory if the chosen image is invalid.
    bootloader_utility_load_boot_image(&bs, boot_index);
}

static bool reset_returns_to_user_app(soc_reset_reason_t reason)
{
    switch (reason) {
    case RESET_REASON_CORE_SW:          // software restart (most chips)
    case RESET_REASON_CPU0_SW:          // software restart (ESP32 and others)
    case RESET_REASON_CORE_DEEP_SLEEP:  // wake-up from deep sleep
        return true;
    default:
        return false;
    }
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    int selected = bootloader_utility_get_selected_boot_partition(bs);

    if (bs->factory.size == 0) {
        // No loader in this partition table: behave exactly like the stock bootloader.
        return selected;
    }

    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    if (selected >= 0 && reset_returns_to_user_app(reason)) {
        ESP_LOGI(TAG, "reset %d -> user app (ota_%d)", (int)reason, selected);
        return selected;
    }

    ESP_LOGI(TAG, "reset %d -> OTA loader", (int)reason);
    return FACTORY_INDEX;
}

// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
