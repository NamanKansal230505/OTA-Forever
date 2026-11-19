#pragma once
/*
 * Loader settings, stored in the dedicated "ota_cfg" NVS partition so that
 * user sketches (Preferences, nvs_flash_erase, WiFi storage...) can never wipe them.
 *
 * NVS layout, namespace "otaf" (also read by the OTAForever Arduino library):
 *   ssid, pass, repo, channel, pubkey : strings
 *   build                             : u32, build number currently installed
 *   slot0, slot1                      : blob[32], ELF SHA-256 of the app the loader last saw in each slot
 *   active                            : u8, slot index the loader last launched
 *   failed                            : u32, build whose last install attempt failed (0 = none)
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OTAF_NVS_PARTITION "ota_cfg"
#define OTAF_NVS_NAMESPACE "otaf"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char repo[101];      // "owner/name"
    char channel[41];    // branch name (sanitized); release tag is "ota-<channel>"
    char pubkey[400];    // PEM (P-256). Empty = trust the key in the first manifest seen.
    uint32_t installed_build;
} otaf_config_t;

esp_err_t otaf_config_init(void);
void otaf_config_load(otaf_config_t *cfg);
esp_err_t otaf_config_save(const otaf_config_t *cfg);
bool otaf_config_is_provisioned(const otaf_config_t *cfg);

bool otaf_config_get_slot_sha(int slot, uint8_t sha[32]);
int otaf_config_get_active_slot(void);
void otaf_config_set_active_slot(int slot, const uint8_t sha[32]);
void otaf_config_set_failed_build(uint32_t build);
