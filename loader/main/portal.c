#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "dns.h"
#include "portal.h"
#include "wifi.h"

extern const char portal_html_start[] asm("_binary_portal_html_start");

static const char *TAG = "portal";
static SemaphoreHandle_t s_saved;
static otaf_config_t *s_cfg;

/* ---------- helpers ---------- */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static void form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    url_decode(out);
}

static void trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
}

/* Accepts "owner/name", "github.com/owner/name", "https://github.com/owner/name(.git)". */
static bool normalize_repo(char *r)
{
    trim(r);
    static const char *prefixes[] = {"https://github.com/", "http://github.com/", "github.com/"};
    for (int i = 0; i < 3; i++) {
        size_t pl = strlen(prefixes[i]);
        if (strncasecmp(r, prefixes[i], pl) == 0) {
            memmove(r, r + pl, strlen(r + pl) + 1);
            break;
        }
    }
    size_t n = strlen(r);
    while (n && r[n - 1] == '/') r[--n] = '\0';
    if (n > 4 && strcmp(r + n - 4, ".git") == 0) r[n -= 4] = '\0';

    int slashes = 0;
    for (size_t i = 0; i < n; i++) {
        if (r[i] == '/') slashes++;
        else if (!name_char(r[i])) return false;
    }
    return n >= 3 && slashes == 1 && r[0] != '/' && r[n - 1] != '/';
}

static bool normalize_channel(char *c)
{
    trim(c);
    if (!c[0]) strcpy(c, "main");
    for (char *p = c; *p; p++) {
        if (*p == '/') *p = '-';  // same rule as the GitHub workflow
        else if (!name_char(*p)) return false;
    }
    return true;
}

static void strip_cr(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s != '\r') *o++ = *s;
    }
    *o = '\0';
}

static esp_err_t send_json_error(httpd_req_t *req, const char *msg)
{
    char esc[160], out[200];
    otaf_json_escape(msg, esc, sizeof(esc));
    snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", esc);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

/* ---------- handlers ---------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, portal_html_start);
}

static esp_err_t config_get(httpd_req_t *req)
{
    char *out = malloc(2048);
    char *e1 = malloc(1024);
    if (!out || !e1) {
        free(out);
        free(e1);
        return httpd_resp_send_500(req);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int o = snprintf(out, 2048, "{\"chip\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"build\":%lu,\"has_pass\":%s",
                     CONFIG_IDF_TARGET, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     (unsigned long)s_cfg->installed_build, s_cfg->wifi_pass[0] ? "true" : "false");
    const char *keys[] = {"ssid", "repo", "channel", "pubkey"};
    const char *vals[] = {s_cfg->wifi_ssid, s_cfg->repo, s_cfg->channel, s_cfg->pubkey};
    for (int i = 0; i < 4 && o < 2000; i++) {
        otaf_json_escape(vals[i], e1, 1024);
        o += snprintf(out + o, 2048 - o, ",\"%s\":\"%s\"", keys[i], e1);
    }
    if (o < 2047) {
        out[o++] = '}';
        out[o] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    free(e1);
    return r;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char *out = malloc(2048);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    otaf_wifi_scan_json(out, 2048);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

static esp_err_t save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        return send_json_error(req, "Request too large");
    }
    char *body = malloc(total + 1);
    char *v = malloc(1400);
    if (!body || !v) {
        free(body);
        free(v);
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) {
            free(body);
            free(v);
            return ESP_FAIL;
        }
        got += n;
    }
    body[total] = '\0';

    otaf_config_t c = *s_cfg;
    const char *error = NULL;

    form_get(body, "ssid", v, 1400);
    trim(v);
    if (!v[0] || strlen(v) >= sizeof(c.wifi_ssid)) {
        error = "Enter your WiFi network name";
    } else {
        bool same_network = strcmp(v, s_cfg->wifi_ssid) == 0;
        strlcpy(c.wifi_ssid, v, sizeof(c.wifi_ssid));
        form_get(body, "pass", v, 1400);
        if (strlen(v) >= sizeof(c.wifi_pass)) {
            error = "WiFi password is too long";
        } else if (v[0] || !same_network) {
            strlcpy(c.wifi_pass, v, sizeof(c.wifi_pass));  // empty + same network = keep old one
        }
    }

    if (!error) {
        form_get(body, "repo", v, 1400);
        if (strlen(v) >= sizeof(c.repo) || !normalize_repo(v)) {
            error = "GitHub repo must look like owner/name";
        } else {
            strlcpy(c.repo, v, sizeof(c.repo));
        }
    }
    if (!error) {
        form_get(body, "channel", v, 1400);
        if (strlen(v) >= sizeof(c.channel) || !normalize_channel(v)) {
            error = "Branch name has unsupported characters";
        } else {
            strlcpy(c.channel, v, sizeof(c.channel));
        }
    }
    if (!error) {
        form_get(body, "pubkey", v, 1400);
        strip_cr(v);
        trim(v);
        // The page pre-fills the current key, so an empty field means "remove it".
        if (v[0] && (strlen(v) >= sizeof(c.pubkey) || !strstr(v, "-----BEGIN PUBLIC KEY-----"))) {
            error = "Public key must be a PEM block (-----BEGIN PUBLIC KEY-----)";
        } else {
            strlcpy(c.pubkey, v, sizeof(c.pubkey));
        }
    }

    if (!error) {
        bool new_source = strcmp(c.repo, s_cfg->repo) != 0 || strcmp(c.channel, s_cfg->channel) != 0;
        if (new_source) {
            c.installed_build = 0;  // install whatever the new source has
            if (strcmp(c.repo, s_cfg->repo) != 0 && strcmp(c.pubkey, s_cfg->pubkey) == 0) {
                c.pubkey[0] = '\0';  // different repo: re-pin its key on first update
            }
        }
        if (otaf_config_save(&c) != ESP_OK) {
            error = "Could not save settings";
        }
    }
    free(body);
    free(v);

    if (error) {
        return send_json_error(req, error);
    }
    *s_cfg = c;
    ESP_LOGI(TAG, "saved: WiFi \"%s\", repo %s, channel %s", c.wifi_ssid, c.repo, c.channel);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xSemaphoreGive(s_saved);
    return ESP_OK;
}

/* Any unknown URL (phone/OS captive-portal probes) -> setup page */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" OTAF_PORTAL_IP_STR "/");
    return httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
}

/* ---------- public ---------- */

bool otaf_portal_run(otaf_config_t *cfg, uint32_t timeout_ms)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_name[32];
    snprintf(ap_name, sizeof(ap_name), "OTA-Setup-%02X%02X", mac[4], mac[5]);

    s_cfg = cfg;
    s_saved = xSemaphoreCreateBinary();
    otaf_wifi_start_ap(ap_name);
    otaf_dns_start();

    httpd_handle_t server = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &hc));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);

    ESP_LOGW(TAG, "SETUP: join WiFi \"%s\" and open http://%s", ap_name, OTAF_PORTAL_IP_STR);
    TickType_t wait = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    bool saved = xSemaphoreTake(s_saved, wait) == pdTRUE;
    if (saved) {
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the browser receive the reply
    }

    httpd_stop(server);
    otaf_dns_stop();
    otaf_wifi_stop();
    vSemaphoreDelete(s_saved);
    s_saved = NULL;
    return saved;
}
