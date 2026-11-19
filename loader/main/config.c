#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "config";

static esp_err_t open_rw(nvs_handle_t *h)
{
    return nvs_open_from_partition(OTAF_NVS_PARTITION, OTAF_NVS_NAMESPACE, NVS_READWRITE, h);
}

esp_err_t otaf_config_init(void)
{
    esp_err_t err = nvs_flash_init_partition(OTAF_NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "formatting config partition");
        ESP_ERROR_CHECK(nvs_flash_erase_partition(OTAF_NVS_PARTITION));
        err = nvs_flash_init_partition(OTAF_NVS_PARTITION);
    }
    return err;
}

static void read_str(nvs_handle_t h, const char *key, char *out, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, def, cap);
    }
}

void otaf_config_load(otaf_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->channel, "main", sizeof(c->channel));

    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) {
        return;
    }
    read_str(h, "ssid", c->wifi_ssid, sizeof(c->wifi_ssid), "");
    read_str(h, "pass", c->wifi_pass, sizeof(c->wifi_pass), "");
    read_str(h, "repo", c->repo, sizeof(c->repo), "");
    read_str(h, "channel", c->channel, sizeof(c->channel), "main");
    read_str(h, "pubkey", c->pubkey, sizeof(c->pubkey), "");
    nvs_get_u32(h, "build", &c->installed_build);
    nvs_close(h);
}

esp_err_t otaf_config_save(const otaf_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", c->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", c->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "repo", c->repo);
    if (err == ESP_OK) err = nvs_set_str(h, "channel", c->channel);
    if (err == ESP_OK) err = nvs_set_str(h, "pubkey", c->pubkey);
    if (err == ESP_OK) err = nvs_set_u32(h, "build", c->installed_build);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool otaf_config_is_provisioned(const otaf_config_t *c)
{
    return c->wifi_ssid[0] != '\0' && c->repo[0] != '\0';
}

static const char *slot_key(int slot)
{
    return slot == 0 ? "slot0" : "slot1";
}

bool otaf_config_get_slot_sha(int slot, uint8_t sha[32])
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) {
        return false;
    }
    size_t len = 32;
    esp_err_t err = nvs_get_blob(h, slot_key(slot), sha, &len);
    nvs_close(h);
    return err == ESP_OK && len == 32;
}

int otaf_config_get_active_slot(void)
{
    nvs_handle_t h;
    uint8_t v = 0xFF;
    if (open_rw(&h) == ESP_OK) {
        nvs_get_u8(h, "active", &v);
        nvs_close(h);
    }
    return v == 0xFF ? -1 : v;
}

void otaf_config_set_failed_build(uint32_t build)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) {
        return;
    }
    uint32_t cur = 0;
    if (nvs_get_u32(h, "failed", &cur) != ESP_OK || cur != build) {
        nvs_set_u32(h, "failed", build);
        nvs_commit(h);
    }
    nvs_close(h);
}

void otaf_config_set_active_slot(int slot, const uint8_t sha[32])
{
    // Only write when something changed: this runs on every boot.
    uint8_t known[32];
    bool sha_same = otaf_config_get_slot_sha(slot, known) && memcmp(known, sha, 32) == 0;
    bool active_same = otaf_config_get_active_slot() == slot;
    if (sha_same && active_same) {
        return;
    }
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) {
        return;
    }
    if (!sha_same) nvs_set_blob(h, slot_key(slot), sha, 32);
    if (!active_same) nvs_set_u8(h, "active", (uint8_t)slot);
    nvs_commit(h);
    nvs_close(h);
}
