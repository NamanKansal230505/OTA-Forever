#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_system.h"
#include "config.h"
#include "launcher.h"

static const char *TAG = "launcher";
#define SLOTS 2

static const esp_partition_t *slot(int i)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i), NULL);
}

static int slot_index(const esp_partition_t *p)
{
    for (int i = 0; p && i < SLOTS; i++) {
        const esp_partition_t *s = slot(i);
        if (s && s->address == p->address) {
            return i;
        }
    }
    return -1;
}

static bool image_ok(const esp_partition_t *p, esp_app_desc_t *desc)
{
    esp_image_metadata_t md;
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) {
        return false;
    }
    return desc == NULL || esp_ota_get_partition_description(p, desc) == ESP_OK;
}

const esp_partition_t *otaf_find_user_app(void)
{
    // 1. otadata points at a slot (set by us, or by the sketch's own ArduinoOTA)
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (slot_index(boot) >= 0 && image_ok(boot, NULL)) {
        return boot;
    }

    // 2. otadata is blank: either a fresh USB upload, or the sketch asked to
    //    come back to the loader. A slot whose content we've never seen was
    //    written by someone else (USB upload) and is the newest; otherwise
    //    return to the slot we launched last.
    int foreign = -1, first_valid = -1;
    bool valid[SLOTS] = {false};
    for (int i = 0; i < SLOTS; i++) {
        const esp_partition_t *p = slot(i);
        esp_app_desc_t d;
        if (!p || !image_ok(p, &d)) {
            continue;
        }
        valid[i] = true;
        if (first_valid < 0) first_valid = i;
        uint8_t known[32];
        if (foreign < 0 && (!otaf_config_get_slot_sha(i, known) ||
                            memcmp(known, d.app_elf_sha256, 32) != 0)) {
            foreign = i;
        }
    }
    int active = otaf_config_get_active_slot();
    int pick = foreign >= 0                            ? foreign
             : (active >= 0 && active < SLOTS && valid[active]) ? active
                                                       : first_valid;
    return pick >= 0 ? slot(pick) : NULL;
}

const esp_partition_t *otaf_pick_update_slot(void)
{
    const esp_partition_t *current = otaf_find_user_app();
    int other = (slot_index(current) == 0) ? 1 : 0;
    const esp_partition_t *p = slot(current ? other : 0);
    return p ? p : slot(0);
}

void otaf_launch(const esp_partition_t *app)
{
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(app, &d) == ESP_OK) {
        otaf_config_set_active_slot(slot_index(app), d.app_elf_sha256);
        ESP_LOGI(TAG, "starting user firmware \"%s\" %s from %s", d.project_name, d.version, app->label);
    }
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!boot || boot->address != app->address) {
        ESP_ERROR_CHECK(esp_ota_set_boot_partition(app));
    }
    esp_restart();
}
