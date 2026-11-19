#pragma once
#include <stdbool.h>

/* Watches the setup button in the background (held 1 s = request setup portal). */
void otaf_button_start(void);
bool otaf_button_requested(void);
void otaf_button_clear(void);
