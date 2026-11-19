#pragma once
#include <Arduino.h>

/*
 * Optional companion for the OTA-Forever loader.
 *
 * Without this library, devices still update on every power-on/reset.
 * With it, a running sketch also notices new builds and hands over to the
 * loader, which downloads, verifies and installs them.
 */
class OTAForeverClass {
 public:
  // Start a background task that checks GitHub every `intervalMinutes`
  // (needs WiFi connected) and restarts into the loader when a newer build exists.
  bool begin(uint32_t intervalMinutes = 10);

  // Connect using the WiFi network saved on the loader's setup page.
  bool connectWiFi(uint32_t timeoutMs = 15000);

  // One-off check (blocking, needs WiFi). True if a newer build exists for this chip.
  bool updateAvailable();

  // Hand control to the loader right now (it installs any update, then restarts this sketch).
  void rebootToLoader();

  // True when running under the OTA-Forever loader and it has been set up.
  bool isManaged() { return load(); }

  uint32_t installedBuild() { load(); return _build; }
  String repo() { load(); return _repo; }
  String channel() { load(); return _channel; }
  String wifiSSID() { load(); return _ssid; }
  String wifiPassword() { load(); return _pass; }

 private:
  bool load();
  static void taskFn(void *arg);

  bool _loaded = false;
  String _ssid, _pass, _repo, _channel;
  uint32_t _build = 0;
  uint32_t _failedBuild = 0;
  uint32_t _intervalMs = 10 * 60 * 1000UL;
  TaskHandle_t _task = nullptr;
};

extern OTAForeverClass OTAForever;
