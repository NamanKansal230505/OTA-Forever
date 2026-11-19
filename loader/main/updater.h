#pragma once
#include "config.h"

typedef enum {
    OTAF_UPDATE_NONE,       // already up to date / nothing published for this chip
    OTAF_UPDATE_INSTALLED,  // new firmware written and selected for boot
    OTAF_UPDATE_ERROR,
} otaf_update_result_t;

/*
 * Reads https://github.com/<repo>/releases/download/ota-<channel>/manifest.json,
 * and if it has a newer build for this chip: checks its signature, downloads it
 * into the inactive slot, verifies the SHA-256 and selects it for boot.
 */
otaf_update_result_t otaf_check_and_update(otaf_config_t *cfg);
