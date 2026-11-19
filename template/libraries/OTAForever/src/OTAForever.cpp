#include "OTAForever.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

OTAForeverClass OTAForever;

// Must match loader/main/config.h
static const char *kPartition = "ota_cfg";
static const char *kNamespace = "otaf";

static String readStr(nvs_handle_t h, const char *key) {
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return String();
  char *buf = (char *)malloc(len);
  if (!buf) return String();
  String out;
  if (nvs_get_str(h, key, buf, &len) == ESP_OK) out = buf;
  free(buf);
  return out;
}

// Value of a top-level integer field, e.g. "build": 42
static long jsonInt(const String &json, const char *key) {
  String needle = String("\"") + key + "\"";
  int i = json.indexOf(needle);
  if (i < 0) return -1;
  i = json.indexOf(':', i + needle.length());
  if (i < 0) return -1;
  return strtol(json.c_str() + i + 1, nullptr, 10);
}

// True if the JSON contains the key "name": (used for the per-chip target entry)
static bool jsonHasKey(const String &json, const char *key) {
  String needle = String("\"") + key + "\"";
  for (int i = json.indexOf(needle); i >= 0; i = json.indexOf(needle, i + 1)) {
    int j = i + needle.length();
    while (j < (int)json.length() && isspace((unsigned char)json[j])) j++;
    if (j < (int)json.length() && json[j] == ':') return true;
  }
  return false;
}

bool OTAForeverClass::load() {
  if (_loaded) return true;
  if (!esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr) ||
      nvs_flash_init_partition(kPartition) != ESP_OK) {
    log_w("OTAForever: loader not found (flash it from the OTA-Forever installer page)");
    return false;
  }
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  _ssid = readStr(h, "ssid");
  _pass = readStr(h, "pass");
  _repo = readStr(h, "repo");
  _channel = readStr(h, "channel");
  if (_channel.length() == 0) _channel = "main";
  uint32_t b = 0, failed = 0;
  nvs_get_u32(h, "build", &b);
  nvs_get_u32(h, "failed", &failed);
  _build = b;
  _failedBuild = failed;
  nvs_close(h);
  _loaded = _repo.length() > 0;
  return _loaded;
}

bool OTAForeverClass::connectWiFi(uint32_t timeoutMs) {
  if (!load() || _ssid.length() == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(_ssid.c_str(), _pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

bool OTAForeverClass::updateAvailable() {
  if (!load() || WiFi.status() != WL_CONNECTED) return false;

  String url = "https://github.com/" + _repo + "/releases/download/ota-" + _channel + "/manifest.json";
  WiFiClientSecure client;
  // Only the build number is read here. The loader verifies the signature and
  // SHA-256 of the firmware itself before installing anything.
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  String body = code == 200 ? http.getString() : String();
  http.end();
  if (code != 200) return false;

  long build = jsonInt(body, "build");
  if (build > 0 && (uint32_t)build == _failedBuild) {
    // The loader just failed to install this build (e.g. bad connection).
    // Don't loop; it retries on the next reset, or when a newer build appears.
    return false;
  }
  return build > (long)_build && jsonHasKey(body, CONFIG_IDF_TARGET);
}

void OTAForeverClass::rebootToLoader() {
  const esp_partition_t *loader =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!loader) {
    log_e("OTAForever: no loader partition");
    return;
  }
  // Selecting the factory app clears otadata; the next boot runs the loader,
  // which then restarts this sketch (or the update).
  if (esp_ota_set_boot_partition(loader) != ESP_OK) {
    log_e("OTAForever: could not select loader");
    return;
  }
  delay(100);
  esp_restart();
}

void OTAForeverClass::taskFn(void *arg) {
  OTAForeverClass *self = static_cast<OTAForeverClass *>(arg);
  vTaskDelay(pdMS_TO_TICKS(20000));  // let the sketch settle first
  for (;;) {
    if (self->updateAvailable()) {
      log_i("OTAForever: new build published, restarting into loader");
      self->rebootToLoader();
    }
    vTaskDelay(pdMS_TO_TICKS(self->_intervalMs));
  }
}

bool OTAForeverClass::begin(uint32_t intervalMinutes) {
  if (!load()) return false;
  _intervalMs = (intervalMinutes ? intervalMinutes : 1) * 60UL * 1000UL;
  if (!_task) xTaskCreate(taskFn, "otaforever", 10240, this, 1, &_task);
  return true;
}
