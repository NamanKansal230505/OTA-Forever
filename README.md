# OTA-Forever

Write Arduino code, push it to GitHub, and every linked ESP32 installs it over WiFi.
You don't need a server or a database, and hosting is free. The updater is a separate
firmware in its own flash partition, so **no sketch you upload can remove it**.

```
 GitHub repo (your sketch + ota.yml)
        │  git push
        ▼
 GitHub Actions ── builds each board in ota.yml (optionally signs them),
        │          publishes them to the release "ota-<branch>"
        ▼
 https://github.com/<you>/<repo>/releases/download/ota-main/manifest.json
        ▲
        │  on every power-on / reset (and optionally every N minutes)
 ESP32: loader ── newer build? → download → check SHA-256 (+ signature) → start your sketch
```

## What's in this repo

| Path | What it is |
|---|---|
| `loader/` | ESP-IDF project for the permanent loader and its custom bootloader |
| `installer/` | Web page that flashes the loader from the browser (ESP Web Tools), served by GitHub Pages |
| `template/` | Starting point for a user project: sketch, `ota.yml`, GitHub Actions workflow, signing scripts, `OTAForever` library |
| `.github/workflows/loader.yml` | Builds the loader for all chips and deploys `installer/` to GitHub Pages |

## How it stays "forever"

Flash layout (4 MB and larger):

```
0x009000  nvs       20 KB   user sketch's NVS (Preferences etc.)
0x00E000  otadata    8 KB   which app slot is selected
0x010000  app0     1.4 MB   user firmware, slot A   ← Arduino USB uploads land here
0x170000  app1     1.4 MB   user firmware, slot B
0x2D0000  loader   1.1 MB   OTA-Forever loader (factory partition)
0x3F0000  ota_cfg   64 KB   loader settings (WiFi, repo, key), separate from the sketch's NVS
```

- **Updates only write app0/app1.** The loader downloads into the slot that isn't
  running, so a failed or interrupted download never breaks the firmware you have now.
- **A custom 2nd-stage bootloader** chooses what to run:

  | Reset type | Boots |
  |---|---|
  | Power-on, reset button, brown-out, watchdog | **loader**, which checks for an update and then starts the sketch |
  | `ESP.restart()` / `esp_restart()`, wake from deep sleep | the sketch directly, with no delay |

  This gives the loader control on every real reset **without any OTA code in the sketch**.
- **The loader adds about 2–4 seconds at power-on** (WiFi connect plus the manifest check).
  If WiFi is unavailable, it gives up after 15 seconds and starts the sketch anyway.
- **USB uploads still work.** The template ships `partitions.csv` next to the `.ino`, so
  the Arduino IDE and arduino-cli use this layout and write the sketch into app0. The
  loader notices the new content and starts it.

## Security

- **Default (no setup):** devices trust builds because the manifest comes over HTTPS from
  `github.com/<owner>/<repo>`, checked against the ESP-IDF certificate bundle. Each
  firmware file's SHA-256 is checked against that manifest. Trust is therefore "whoever
  controls the GitHub repo".
- **Optional signed builds:** add an `OTA_SIGNING_KEY` secret (ECDSA P-256), and GitHub
  Actions signs `otaf1|<chip>|<build>|<sha256>`. A device pins the key from its first signed
  build, or you can paste the key on the setup page. After that it refuses unsigned or
  differently signed builds. Because the build number is part of the signed text, an old
  binary can't be replayed as a new build.
- The loader never needs a GitHub token, because it only reads public release downloads.
- The setup access point is open, and it only appears when the device has no settings,
  when you hold BOOT at power-on, or when no firmware is installed yet.

## Getting started

### For hobbyists: 3 clicks, then just edit code
1. On the installer page, click **Create my project**. GitHub creates their own repo from
   the template.
2. Click **Install on my board**. The browser flashes the loader over USB.
3. On a phone, join `OTA-Setup-XXXX` and enter their WiFi network and repo link.

After that, they edit `firmware/firmware.ino` on github.com and commit. No local tools, keys
or board settings are needed (`boards: auto` builds every chip).

### For you (platform owner, once)
Publish two public repos:

| Repo | Contents | Setting |
|---|---|---|
| `OTA-Forever` | this folder (`loader/`, `installer/`, `template/`, `.github/`) | Settings → Pages → Source: **GitHub Actions** |
| `ota-forever-template` | contents of `template/` | Settings → **Template repository** ✓ |

The **Loader + installer** workflow builds the loader for all five chips and publishes the
installer at `https://<you>.github.io/OTA-Forever/`. The **Create my project** button
points at `https://github.com/<you>/ota-forever-template/generate`, with the owner read
from the page address.

To build the loader locally (ESP-IDF v5.4, or Docker):
```
cd loader
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.4 bash build_all.sh
```

## Branches = channels

Each branch publishes to its own release (`ota-main`, `ota-dev`, and so on). Each device
follows the branch set on its setup page, so you can test on a few boards using `dev`
before merging to `main`. Slashes in branch names become dashes (`feature/x` → `ota-feature-x`).

## Limitations and next steps

- **ESP32 family only for now** (esp32, s2, s3, c3, c6). ESP8266, RP2040 and boards without
  WiFi need their own loaders or an ESP32 acting as a programmer; see the roadmap below.
- **Uploading over USB with the stock Arduino bootloader** replaces the custom bootloader.
  OTA still works if the sketch uses `OTAForever.begin()`. Otherwise, reinstall the loader
  from the installer page to get updates on every reset again.
- **Sketch crashes that cause a software reset** (for example a panic) go straight back to
  the sketch. Power-cycling always runs the loader, so pushing a fix and power-cycling recovers.
- **Public repos only.** For private source code, have the workflow publish releases to a
  separate public "firmware" repo.

### Roadmap
- Loader self-update (update the loader from GitHub too)
- Web dashboard: devices check in, show online status, version and logs
- Instant "update now" over MQTT
- Click-to-configure web page that writes `ota.yml` for you
- More boards: RP2040 / Pico W (resident bootloader), ESP8266, and AVR/STM32 through an ESP32 gateway
