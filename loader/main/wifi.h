#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void otaf_wifi_init(void);
bool otaf_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms);
void otaf_wifi_start_ap(const char *ssid);
void otaf_wifi_stop(void);
/* Scans and writes a JSON array of SSID strings into out. */
void otaf_wifi_scan_json(char *out, size_t cap);
/* JSON-escapes in into out (always NUL-terminated). */
void otaf_json_escape(const char *in, char *out, size_t cap);
