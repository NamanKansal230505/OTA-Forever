# My ESP32 project (OTA-Forever)

Change the code in this repo, and your ESP32 boards update themselves over WiFi.

## Changing your code

1. Open [`firmware/firmware.ino`](firmware/firmware.ino) and click the ✏️ pencil
   (or press `.` to open the full editor in your browser).
2. Make your change and click **Commit changes**.
3. Open the **Actions** tab to watch the build. A green check means it's published.
4. Your boards install it the next time they restart, or within 5 minutes while they're
   running (the example sketch checks for updates in the background).

If a build fails, click it in the **Actions** tab to see the compiler error. Your boards
keep running the last good version.

## First-time setup of a board

Use the OTA-Forever installer page: click **Install on my board** (Chrome or Edge, USB).
On your phone, join the WiFi network `OTA-Setup-XXXX`, choose your WiFi network and paste
this repo's link. To change those settings later, hold **BOOT** for 1 second right after
power-on.

## Settings (`ota.yml`)

The defaults work, so you don't have to change anything here.

| Key | Meaning |
|---|---|
| `boards` | `auto` builds every supported chip. Listing only your chips makes builds faster. |
| `libraries` | Libraries from the Arduino Library Manager, e.g. `["ArduinoJson"]` |
| `branches` | Branches that publish updates. A board follows the branch set on its setup page (default `main`). |
| `version` | A label shown in board logs |

## Good to know

- **Keep the repo public.** Boards download updates without logging in to GitHub.
- **Keep `firmware/partitions.csv`.** It makes USB uploads from the Arduino IDE keep the
  updater intact.
- **Size limit:** firmware must fit in 1.4 MB per board.
- **Arduino IDE on your computer:** add the `libraries/` folder to your sketchbook so
  `#include <OTAForever.h>` resolves.

## Optional: signed builds

By default, boards trust builds because they come over HTTPS from this repo, and every
file's SHA-256 is checked. For extra protection, for example if someone could take over
your GitHub account, you can sign builds:

```
pip install cryptography
python .github/ota/keygen.py
gh secret set OTA_SIGNING_KEY < ota_signing_key.pem
```

After the first signed build, each board remembers the key and refuses unsigned or
differently signed builds. Keep `ota_signing_key.pem` safe and never commit it.
