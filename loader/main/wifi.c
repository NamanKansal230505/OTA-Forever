#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define MAX_RETRIES 4

static EventGroupHandle_t s_ev;
static bool s_started;
static bool s_sta_active;
static int s_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        if (s_sta_active) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        if (!s_sta_active) {
            return;
        }
        if (s_retries++ < MAX_RETRIES) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
    s_retries = 0;
    xEventGroupSetBits(s_ev, BIT_GOT_IP);
}

void otaf_wifi_init(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Power save (the IDF default) sleeps the radio between beacons and cuts
    // download speed to ~10 KB/s. The loader only runs briefly, so keep it awake.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
}

void otaf_wifi_stop(void)
{
    s_sta_active = false;
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
}

bool otaf_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    otaf_wifi_stop();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    s_retries = 0;
    s_sta_active = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    ESP_LOGI(TAG, "connecting to \"%s\"...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
        return true;
    }
    ESP_LOGW(TAG, "could not connect to \"%s\"", ssid);
    return false;
}

void otaf_wifi_start_ap(const char *ssid)
{
    otaf_wifi_stop();

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA so the setup page can scan for networks
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
}

void otaf_json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (ch < 0x20) {
            o += snprintf(out + o, cap - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

void otaf_wifi_scan_json(char *out, size_t cap)
{
    strlcpy(out, "[]", cap);
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return;
    }
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        return;
    }

    size_t o = 0;
    out[o++] = '[';
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;  // records are sorted by signal; keep the strongest
        for (int j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)recs[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        char esc[100];
        otaf_json_escape(ssid, esc, sizeof(esc));
        int w = snprintf(out + o, cap - o, "%s\"%s\"", o > 1 ? "," : "", esc);
        if (w < 0 || (size_t)w >= cap - o - 2) {
            break;
        }
        o += w;
    }
    out[o++] = ']';
    out[o] = '\0';
    free(recs);
}
