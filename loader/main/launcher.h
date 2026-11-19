#pragma once
#include "esp_partition.h"

/* The user app that should run (verified image), or NULL if none is installed. */
const esp_partition_t *otaf_find_user_app(void);

/* Slot to download the next update into (never the app currently in use). */
const esp_partition_t *otaf_pick_update_slot(void);

/* Records the slot as the active one and reboots into it. Does not return. */
void otaf_launch(const esp_partition_t *app) __attribute__((noreturn));
