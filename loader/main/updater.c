#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "launcher.h"
#include "updater.h"

static const char *TAG = "update";

#define MANIFEST_MAX 8192
#define CHUNK        4096

/* Opens url (from byte `offset` on, if > 0), following GitHub's redirects to its download CDN. */
static esp_http_client_handle_t open_url_at(const char *url, uint32_t offset, int *status)
{
    *status = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,     // redirect URLs from GitHub are long
        .buffer_size_tx = 4096,
        .user_agent = "ota-forever-loader",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    if (offset > 0) {
        char range[32];
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)offset);
        esp_http_client_set_header(c, "Range", range);  // kept across redirects
    }
    for (int hop = 0; hop < 6; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
            break;
        }
        if (esp_http_client_fetch_headers(c) < 0 && esp_http_client_get_status_code(c) == 0) {
            ESP_LOGE(TAG, "no response");
            break;
        }
        int st = esp_http_client_get_status_code(c);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            esp_http_client_flush_response(c, NULL);
            esp_http_client_close(c);
            if (esp_http_client_set_redirection(c) != ESP_OK) {
                break;
            }
            continue;
        }
        *status = st;
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

static esp_http_client_handle_t open_url(const char *url, int *status)
{
    return open_url_at(url, 0, status);
}

static void close_url(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static char *fetch_text(const char *url, int *status)
{
    esp_http_client_handle_t c = open_url(url, status);
    if (!c) {
        return NULL;
    }
    char *buf = NULL;
    if (*status == 200 && (buf = malloc(MANIFEST_MAX + 1)) != NULL) {
        int total = 0, n = 0;
        while (total < MANIFEST_MAX && (n = esp_http_client_read(c, buf + total, MANIFEST_MAX - total)) > 0) {
            total += n;
        }
        if (n < 0) {
            free(buf);
            buf = NULL;
        } else {
            buf[total] = '\0';
        }
    }
    close_url(c);
    return buf;
}

static bool parse_sha256_hex(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
        if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) {
            return false;
        }
        out[i] = (uint8_t)strtol(b, NULL, 16);
    }
    return true;
}

/* ECDSA P-256 / SHA-256 over msg, DER signature, PEM public key. */
static bool signature_ok(const char *pem, const char *msg, const char *sig_b64)
{
    uint8_t sig[128];
    size_t sig_len = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len, (const unsigned char *)sig_b64, strlen(sig_b64)) != 0) {
        return false;
    }
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)msg, strlen(msg), hash, 0);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, strlen(pem) + 1);
    if (ret == 0 && !mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)) {
        ret = -1;
    }
    if (ret == 0) {
        ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    }
    mbedtls_pk_free(&pk);
    return ret == 0;
}

#define DOWNLOAD_ATTEMPTS 5

static esp_err_t download_to(const esp_partition_t *part, const char *url, uint32_t size,
                             const uint8_t expect_sha[32])
{
    uint32_t got = 0;
    int last_pct = -10;
    esp_ota_handle_t ota = 0;
    esp_http_client_handle_t c = NULL;
    uint8_t hash[32];
    esp_err_t err = ESP_FAIL;
    char *buf = malloc(CHUNK);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    if (!buf) {
        goto done;
    }
    if (size > part->size) {
        ESP_LOGE(TAG, "firmware is %lu bytes, slot holds %lu", (unsigned long)size, (unsigned long)part->size);
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ota = 0;
        goto done;
    }

    // If the connection stalls, reconnect and continue from where we stopped.
    for (int attempt = 1; attempt <= DOWNLOAD_ATTEMPTS && got < size; attempt++) {
        int status = 0;
        if (attempt > 1) {
            ESP_LOGW(TAG, "connection dropped at %lu bytes, resuming (attempt %d/%d)",
                     (unsigned long)got, attempt, DOWNLOAD_ATTEMPTS);
        }
        c = open_url_at(url, got, &status);
        if (!c) {
            continue;
        }
        if (status != (got ? 206 : 200)) {
            ESP_LOGE(TAG, "firmware download: HTTP %d", status);
            close_url(c);
            c = NULL;
            if (status == 404 || (got && status == 200)) {
                break;  // gone, or server can't resume
            }
            continue;
        }
        while (got < size) {
            int n = esp_http_client_read(c, buf, CHUNK);
            if (n <= 0 || got + (uint32_t)n > size) {
                break;
            }
            mbedtls_sha256_update(&sha, (const unsigned char *)buf, n);
            err = esp_ota_write(ota, buf, n);
            if (err != ESP_OK) {
                goto done;
            }
            got += n;
            int pct = (int)((uint64_t)got * 100 / size);
            if (pct >= last_pct + 10) {
                ESP_LOGI(TAG, "downloading... %d%%", pct);
                last_pct = pct;
            }
        }
        close_url(c);
        c = NULL;
    }
    if (got != size) {
        ESP_LOGE(TAG, "download incomplete (%lu of %lu bytes)", (unsigned long)got, (unsigned long)size);
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    mbedtls_sha256_finish(&sha, hash);
    if (memcmp(hash, expect_sha, 32) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch, discarding download");
        err = ESP_ERR_INVALID_CRC;
        goto done;
    }
    err = esp_ota_end(ota);  // also validates the image (chip type, checksums)
    ota = 0;

done:
    if (ota) esp_ota_abort(ota);
    if (c) close_url(c);
    mbedtls_sha256_free(&sha);
    free(buf);
    return err;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

otaf_update_result_t otaf_check_and_update(otaf_config_t *cfg)
{
    char url[256];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/ota-%s/manifest.json",
             cfg->repo, cfg->channel);
    ESP_LOGI(TAG, "checking %s", url);

    int status = 0;
    char *text = fetch_text(url, &status);
    if (!text) {
        if (status == 404) {
            ESP_LOGW(TAG, "nothing published yet for %s (channel %s). Push code to the repo, "
                          "and make sure it is public.", cfg->repo, cfg->channel);
            return OTAF_UPDATE_NONE;
        }
        ESP_LOGE(TAG, "manifest download failed (HTTP %d)", status);
        return OTAF_UPDATE_ERROR;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGE(TAG, "manifest is not valid JSON");
        return OTAF_UPDATE_ERROR;
    }

    otaf_update_result_t result = OTAF_UPDATE_ERROR;
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "targets"), CONFIG_IDF_TARGET);
    double build_d = json_num(root, "build");
    const char *version = json_str(root, "version");
    bool have_app = otaf_find_user_app() != NULL;
    uint32_t installed = have_app ? cfg->installed_build : 0;

    if (json_num(root, "schema") != 1 || build_d < 1) {
        ESP_LOGE(TAG, "unsupported manifest");
        goto out;
    }
    uint32_t build = (uint32_t)build_d;
    if (build <= installed) {
        ESP_LOGI(TAG, "up to date (build %lu)", (unsigned long)installed);
        result = OTAF_UPDATE_NONE;
        goto out;
    }
    if (!target) {
        ESP_LOGW(TAG, "build %lu has no firmware for %s; add this board to ota.yml",
                 (unsigned long)build, CONFIG_IDF_TARGET);
        result = OTAF_UPDATE_NONE;
        goto out;
    }

    const char *bin_url = json_str(target, "url");
    const char *sha_hex = json_str(target, "sha256");
    const char *sig = json_str(target, "sig");
    double size = json_num(target, "size");
    uint8_t sha[32];
    if (!bin_url || size <= 0 || !parse_sha256_hex(sha_hex, sha)) {
        ESP_LOGE(TAG, "manifest entry for %s is incomplete", CONFIG_IDF_TARGET);
        goto out;
    }

    /*
     * Trust:
     *  - device has a pinned key (entered at setup, or learned from the first
     *    signed build): the build MUST carry a valid signature from that key.
     *  - no pinned key + signed build: verify with the manifest's key, then pin it.
     *  - no pinned key + unsigned build: trust GitHub over HTTPS (the manifest came
     *    from github.com/<repo> with certificate checks; the binary's SHA-256 is
     *    checked against it below).
     */
    bool pinned = cfg->pubkey[0] != '\0';
    const char *manifest_key = json_str(root, "pubkey");
    const char *key = pinned ? cfg->pubkey : (sig ? manifest_key : NULL);
    if (pinned && !sig) {
        ESP_LOGE(TAG, "build %lu is unsigned, but this device only accepts builds signed with its "
                      "pinned key (clear the key on the setup page to allow unsigned builds)",
                 (unsigned long)build);
        goto out;
    }
    if (key) {
        char msg[160];
        snprintf(msg, sizeof(msg), "otaf1|%s|%lu|%s", CONFIG_IDF_TARGET, (unsigned long)build, sha_hex);
        if (!signature_ok(key, msg, sig)) {
            ESP_LOGE(TAG, "SIGNATURE INVALID for build %lu, refusing to install", (unsigned long)build);
            goto out;
        }
        ESP_LOGI(TAG, "signature OK");
    } else {
        ESP_LOGI(TAG, "unsigned build, trusted via HTTPS from github.com/%s", cfg->repo);
    }

    const esp_partition_t *slot = otaf_pick_update_slot();
    ESP_LOGI(TAG, "installing build %lu (%s), %lu bytes -> %s", (unsigned long)build,
             version ? version : "?", (unsigned long)size, slot->label);
    if (download_to(slot, bin_url, (uint32_t)size, sha) != ESP_OK ||
        esp_ota_set_boot_partition(slot) != ESP_OK) {
        // Tells the OTAForever library not to hand over again for this build;
        // the loader still retries it on the next reset.
        otaf_config_set_failed_build(build);
        goto out;
    }
    otaf_config_set_failed_build(0);
    cfg->installed_build = build;
    if (!pinned && key) {
        strlcpy(cfg->pubkey, key, sizeof(cfg->pubkey));
        ESP_LOGI(TAG, "pinned signing key from %s", cfg->repo);
    }
    otaf_config_save(cfg);
    ESP_LOGI(TAG, "build %lu installed", (unsigned long)build);
    result = OTAF_UPDATE_INSTALLED;

out:
    cJSON_Delete(root);
    return result;
}
