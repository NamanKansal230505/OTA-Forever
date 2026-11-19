#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/*
 * Starts the "OTA-Setup-XXXX" access point with a captive setup page.
 * Blocks until settings are saved (returns true, cfg updated) or until
 * timeout_ms passes (0 = wait forever; returns false).
 */
bool otaf_portal_run(otaf_config_t *cfg, uint32_t timeout_ms);
