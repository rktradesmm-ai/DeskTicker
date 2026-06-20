# DeskTicker — Publishing a firmware update (maintainer runbook)

How to cut a new firmware version that customers can flash from the web updater
at `docs/index.html` (served via GitHub Pages). End users never need the Arduino
IDE — only **you** (the maintainer) do, to produce the binaries.

---

## 1. Build the firmware in Arduino IDE

Open `DeskTicker/DeskTicker.ino` and set **Tools** exactly as the product ships:

- **Board:** ESP32S3 Dev Module (JC3248W535C)
- **ESP32 Arduino core:** **3.0.7** (do not change — see `DeskTicker/CLAUDE.md`)
- **Flash Size:** 16MB
- **Partition Scheme:** **Huge APP (3MB No OTA / 1MB SPIFFS)**
- **PSRAM:** OPI PSRAM
- **USB Mode:** **USB-OTG (TinyUSB)**
- **USB CDC On Boot:** Enabled

Then **Sketch → Export Compiled Binary**. The binaries land in the sketch's
`build/<board>/` folder (or a `build/` subfolder shown in the output).

You need these three from that folder:
- `DeskTicker.ino.bootloader.bin`
- `DeskTicker.ino.partitions.bin`
- `DeskTicker.ino.bin`  *(the app)*

Plus this fixed file from the core (same for every build):
- `boot_app0.bin` →
  `C:\Users\squal\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.0.7\tools\partitions\boot_app0.bin`

> **Sanity check:** `DeskTicker.ino.bin` must be **< 3 MB** to fit the Huge-APP
> slot. (It runs today, so it does — just confirm after big additions.)

---

## 2. Confirm the flash offsets (do this once, and again if the scheme changes)

The offsets the web installer uses must match the real build. Turn on
**File → Preferences → "Show verbose output during: upload"**, do a normal
Upload once, and read the `esptool ... write_flash` line. For the current
scheme they are:

| Part | Offset (hex) | Offset (decimal, used in manifest) |
|------|--------------|-------------------------------------|
| `DeskTicker.ino.bootloader.bin` | `0x0`     | `0`     |
| `DeskTicker.ino.partitions.bin` | `0x8000`  | `32768` |
| `boot_app0.bin`                 | `0xe000`  | `57344` |
| `DeskTicker.ino.bin`            | `0x10000` | `65536` |

(ESP32-S3 puts the bootloader at `0x0`, unlike the classic ESP32 at `0x1000`.)

NVS (saved settings/Wi-Fi) lives at `0x9000` and is **intentionally not flashed**,
so updates keep the customer's settings.

---

## 3. Add the binaries to the site

1. Make a new folder `docs/firmware/<version>/` (e.g. `docs/firmware/1.0.1/`).
2. Copy the four files into it:
   - `DeskTicker.ino.bootloader.bin`
   - `DeskTicker.ino.partitions.bin`
   - `boot_app0.bin`
   - `DeskTicker.ino.bin`
3. Update `docs/manifest.json`:
   - bump `"version"` to the new version,
   - change every `"path"` to point at `firmware/<version>/...`,
   - re-check the `"offset"` values against step 2.

---

## 4. Publish

```sh
git add docs/
git commit -m "Firmware v<version>: web updater binaries"
git push
```

GitHub Pages then serves the updated page at
`https://<user>.github.io/DeskTicker/`. (One-time setup: repo **Settings → Pages
→ Source = main / docs**.)

Also cut a **GitHub Release** tagged `v<version>` and attach the same four `.bin`
files plus a `DeskTicker.ino.merged.bin` (full-image, for advanced/recovery
flashing) and a short changelog. The Release is the canonical versioned record;
the Pages site always serves from `docs/firmware/<version>/`.

---

## 5. Smoke-test before announcing

1. Open the Pages URL (or run `npx serve docs` and use `http://localhost:3000`)
   in Chrome/Edge.
2. On a real unit: enter update mode (**hold BOOT, tap RST, release BOOT**),
   click **Install**, pick the port, finish, press **RST**.
3. **Keep-settings:** with "Erase device" *unchecked*, confirm Wi-Fi/settings
   survive. **Clean:** with it *checked*, confirm first-run setup appears.

---

## Notes / gotchas

- Keep manifest `path`s **same-origin** (under `docs/`). Do **not** point them at
  GitHub Release download URLs — cross-origin `fetch` can be blocked (CORS).
- `chipFamily` must stay `"ESP32-S3"`.
- WebSerial needs **https or localhost** and **Chrome/Edge desktop**.
- The device has no USB-serial bridge, so auto-reset into the bootloader does not
  work — the manual BOOT/RST step is required every time (documented on the page).
