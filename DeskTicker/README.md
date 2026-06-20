# DeskTicker

Live candlestick chart terminal for your desk. WiFi-connected, no subscriptions, no API key.

## Hardware

| Component | Detail |
|-----------|--------|
| Board SKU | JC3248W535C_I_Y |
| MCU | ESP32-S3 |
| Display | 3.5" IPS TFT, 320×480, AXS15231B driver (QSPI) |
| Touch | Capacitive I2C |

## What it shows

- Real-time candlestick charts — swipe **up/down** to cycle between your selected timeframes
- Up to 6 assets — swipe **left/right** to switch assets (or set auto-cycle)
- Stocks & ETFs, Index Futures (ES/NQ), Crypto, Commodities, Forex
- After-hours animations when markets are closed

---

## Build Instructions

### 1. Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| PSRAM | OPI PSRAM |
| Flash Size | 16MB |
| Partition Scheme | Huge APP (3MB No OTA / 1MB SPIFFS) |
| CPU Frequency | 240 MHz |
| Upload Speed | 921600 |
| **USB Mode** | **USB-OTG (TinyUSB)** |
| **USB CDC On Boot** | **Enabled** |

> **USB Mode must be "USB-OTG (TinyUSB)"** for the [SD Card Access over USB](#sd-card-access-over-usb)
> feature to work — the default "Hardware CDC and JTAG" cannot present a USB drive. Keep **USB CDC
> On Boot = Enabled** so the Serial Monitor keeps working over the same cable. This mode changes how
> the board flashes — see [Flashing in USB-OTG mode](#flashing-in-usb-otg-mode) below.

#### Flashing in USB-OTG mode

The **BOOT** and **RST** (reset) buttons used below are on the back of the board:

![Back of the board showing the BOOT and RST (reset) buttons](../Device-reset-boot-buttons.jpg)

In USB-OTG (TinyUSB) mode the board shows up as **two different COM ports**, because the running
firmware and the bootloader are two separate USB devices that are never present at the same time:

- **COM5 (example)** — the *running firmware's* serial port (TinyUSB CDC). This is the one the
  **Serial Monitor** uses, and the only port visible while the device is running normally.
- **COM3 (example)** — the *bootloader's* serial port. It only appears while the chip is in
  **download mode**, and it's the one esptool actually **uploads** through.

(The exact numbers depend on your PC — yours may differ. What matters is "the running one" vs "the
download one.") You upload through the download port and watch logs on the running port. Pick
**either** way to flash:

**Option A — let the failed attempt flip the board (no buttons):**
1. Select the running port (e.g. **COM5**) and click **Upload**.
2. It will error out with something like
   `ClearCommError failed (PermissionError(13, 'The device does not recognize the command.'))`.
   **This is expected, not a fault** — the IDE just rebooted the chip into download mode, so the
   port it was talking to (COM5) vanished and the download port (**COM3**) appeared in its place.
3. Now select the download port (**COM3**) and click **Upload** again. This one flashes for real.

**Option B — go straight to download mode (one upload, no error):**
1. Put the board into download mode yourself: hold **BOOT**, tap **RST**, release **BOOT**.
   The download port (**COM3**) appears immediately.
2. Select **COM3** and click **Upload** once. No error, no second attempt.

**At the end of every upload:** when esptool prints `Leaving... Hard resetting via RTS pin...` and
the **screen is blank**, the board often does not restart on its own in this USB mode — **press the
RST (reset) button once** to reboot into the new firmware. (Just **RST** on its own here — *not* the
BOOT + RST combo, which is only for entering download mode at the *start* of an upload.)

### 2. Install Board Support

In Arduino IDE → Preferences → Additional Boards Manager URLs, add:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Install **esp32 by Espressif Systems v3.0.7**.

> **Important — use exactly v3.0.7.**
> - v3.1.0 and later introduce a display deadlock that hangs the after-hours animations in ~40 minutes.
> - v3.0.2 and earlier have an HTTP client bug that truncates Yahoo Finance responses.
> - v3.0.7 is the tested stable version: longest hang-free window and correct HTTP behaviour.
> - See `DeskTicker/BISECT_LOG.md` for the full version-bisect history.

> **Note:** The JC3248W535EN "Must see for use.txt" board-file swap applies only to the vendor MJPEG demo and is **not** needed for this project — use the stock **ESP32S3 Dev Module** board as listed in the table above.

### 3. Install Libraries

**Via Arduino Library Manager:**
- **ArduinoJson** by Benoit Blanchon — v6.x (do NOT install v7)

**Manually (copy to Arduino/libraries):**
Copy the entire `lvgl` folder from:
```
JC3248W535EN/1-Demo/Demo_Arduino/libraries/lvgl
```
into your Arduino `libraries` directory.

### 4. Open and Flash

1. Open `DeskTicker/DeskTicker.ino` in Arduino IDE.
2. All board support files (`esp_bsp.c`, `lv_port.c`, etc.) are already in the sketch folder.
3. Select your COM port and click Upload.

---

## First-Time Setup

1. Power on the device. It shows the **DeskTicker Setup** screen.
2. On your phone or laptop, connect to WiFi: **`DeskTicker-Setup`**
3. Open a browser and go to **`192.168.4.1`** (captive portal may open automatically).
4. Configure:
   - **WiFi** — your network name and password
   - **Assets** — pick up to 6, grouped by class (Crypto / Stocks & ETFs / Commodities / Forex). You can also type a **Custom** Yahoo Finance symbol — see [Custom Tickers](#custom-tickers)
   - **Timeframes** — select one or more (15m / 1h / 4h / 1D); swipe up/down on the chart to cycle between them
   - **Timezone** — UTC offset for your location
   - **Candle Colour** — Classic, Color Shift, Neon Pulse, or Custom
   - **After-Hours Animation** — shown when markets are closed
   - **Asset cycling** — auto-cycle interval, or manual swipe-only
5. Tap **Save & Start** — the device restarts and connects automatically.

---

## Available Assets

| Category | Tickers |
|----------|---------|
| Crypto | BTC, ETH, LINK, XRP, DOGE |
| Stocks & ETFs | SPY, QQQ, DIA, IWM, AAPL, MSFT, AMZN, GOOGL, TSLA, NVDA, META |
| Index Futures† | ES (S&P 500 Fut), NQ (Nasdaq 100 Fut) |
| Commodities* | GOLD, SILVER, OIL |
| Forex | USD/JPY, EUR/USD, GBP/USD |

*Commodities use Yahoo's 24/7 crypto-style tokens: Tether Gold (`XAUT-USD`), a silver derivative (`XAG39343-USD`), and a crude oil token (`CL-USD`). Because these instruments trade around the clock, the firmware fetches them with a 24/7-sized window — prices are near-real-time at all times, not delayed like standard futures or ETF quotes.

†Index futures use real CME Globex futures contracts (`ES=F`, `NQ=F`). Near-real-time during the ~23-hour CME session (closed weekends and one 1-hour daily maintenance break).

For details on Yahoo Finance data providers, see:
https://help.yahoo.com/kb/finance-for-web/exchanges-data-providers-yahoo-finance-sln2310.html

---

## Custom Tickers

Beyond the built-in list above, you can add **any symbol that exists on Yahoo Finance**
(e.g. `PLTR`, `SOL-USD`, `EURGBP=X`, `CL=F`). The device looks the symbol up on Yahoo and
**auto-detects** everything it needs:

- **Asset class** (Stock / Crypto / Forex / Futures) — from Yahoo's `instrumentType`
- **Display name** — from Yahoo's short/long name
- **Decimal places** — from Yahoo's price hint

Custom tickers are saved in a small **library of up to 6** on the device and appear in the
asset picker right alongside the built-ins. Use the symbol exactly as Yahoo Finance lists it
(open finance.yahoo.com and check the symbol in the quote header / URL).

### Add one on the device (recommended)

1. Triple-tap to open **Settings → Assets**.
2. Tap **+ Add custom ticker**.
3. Type the symbol on the on-screen keyboard and tap **Add** (or the keyboard's ✓).
4. The device checks Yahoo and, on success, shows the detected class and name — e.g.
   `Added & selected: PLTR - Stock / Palantir Technologies (2 dp)`. The new ticker is
   selected automatically if you have a free slot among the 6.
5. Tap **Save & Restart**.

A brief (~1–3 s) screen freeze during the check is normal — it's the same pause as a regular
data fetch.

### Add one during first-time setup

The setup web form has a **Custom** field in the Assets card. Because the setup hotspot has no
internet, the symbol can't be checked at that moment — it is saved and **classified
automatically the first time it loads** after the device joins your WiFi. Until then its row
shows **`[Pending]`**.

### Confirming the detected class

Each custom ticker's row in **Settings → Assets** shows its class — e.g.
`PLTR  [Stock] Palantir` or `SOL-USD  [Crypto] Solana`. If a class looks wrong, tap the **🗑**
next to it to remove it, then re-add it.

### Notes & limits

- The **library holds 6 customs**; the on-screen selection limit is still **6 tickers total**
  (built-ins + customs combined).
- **Built-ins win** — a symbol matching a built-in (e.g. `AAPL`) is rejected as "already a
  built-in ticker"; duplicates are rejected too.
- An invalid symbol (typo) is rejected with a "not found" message and nothing is saved.
- Removing a custom that is currently selected also removes it from your active list.

---

## Touchscreen Gestures

| Gesture | Action |
|---------|--------|
| Swipe left | Next asset |
| Swipe right | Previous asset |
| Swipe up | Next timeframe (cycles through your selected timeframes) |
| Swipe down | Previous timeframe |
| **Triple-tap** | Open on-device Settings menu |

Triple-tap works from both the chart screen and after-hours animation screens.
Tap three times within ~1.2 seconds anywhere on the screen.

---

## Data Source

Market data is fetched from Yahoo Finance's public chart API (HTTPS, no account required).

| Timeframe | Refresh interval |
|-----------|-----------------|
| 15m | Every 30 seconds |
| 1h | Every 30 seconds |
| 4h | Every 30 seconds |
| 1D | Every 10 minutes |

The 4H timeframe is synthesised on-device by aggregating four consecutive 1H bars.

If Yahoo Finance is unreachable on both primary and fallback hosts (HTTP −1), the device
reconnects WiFi automatically and retries. On a brief WiFi drop the chart stays on screen
with a small "Reconnecting…" note in the header; only after ~15 s of failed retries does
it fall back to the full "Connecting to WiFi…" screen.

---

## Candle Colour Themes

| Theme | Up candle | Down candle |
|-------|-----------|-------------|
| Classic | Teal `#26A69A` | Red `#EF5350` |
| Color Shift | Cycles through the rainbow (animated) | Opposite hue |
| Neon Pulse | Neon green (pulsing) | Neon magenta (pulsing) |
| Custom | Your choice | Your choice |

The **Candle Colour** page in Settings shows a live color swatch for each option so you can see the up/down colors before selecting.

---

## After-Hours Animations

Shown when the selected ticker's market is closed (stocks, ETFs, commodities, forex).
Crypto is always open and never shows an after-hours screen.

| Animation | Description |
|-----------|-------------|
| Tidepool | Dusk shore at sunset: a setting sun reflecting on the tide, distant headlands, rocky tide-pools, beach detritus (starfish, shells, driftwood), twinkling stars, a gentle tide-foam line, drifting gulls, and a pixel crab among rising bubbles. Claw colors match your bull/bear candle theme. |
| Coral Reef | Underwater reef with shimmering caustic light shafts, a sandy rippled seabed, varied coral plus anemones, a sea urchin and a starfish, finned tropical fish swimming at three depths, a slow drifting jellyfish, rising bubbles, and a walking pixel crab. Claw colors match your bull/bear theme. |
| Starfield | Slow starfield drift with occasional shooting stars, and a large centered real-time clock with the date. |
| Countdown | Digital clock counting down to the next NYSE/NASDAQ open (9:30 AM ET). Crab walks along the bottom; digit color shifts from white to amber to green as open time approaches. |
| Pixel Beach | Night beach with a boardwalk lamp post casting a warm cone of light onto the sand, a wooden bench in the lit sand, twinkling stars, a crescent moon, shimmering water glints, starfish and seashells, and a walking pixel crab that leaves footprints. An occasional shooting star streaks across the sky. Claw colors match your bull/bear theme. |
| Grassland | Calm dawn meadow: layered rolling hills under a soft sunrise sky, a warm rising sun, a lone tree, drifting clouds, swaying grass with tiny wildflowers, fluttering butterflies, a couple of distant birds, and a walking pixel crab in the foreground. Claw colors match your bull/bear theme. |

---

## On-Device Settings

Triple-tap the chart or after-hours screen to open the **Settings** menu without rebooting.

| Section | What you can change |
|---------|---------------------|
| Assets | Which tickers are shown (1–6); grouped by Crypto / Stocks & ETFs / Commodities / Forex. **+ Add custom ticker** adds any Yahoo Finance symbol — see [Custom Tickers](#custom-tickers) |
| Timeframes | Which intervals are active (15m / 1h / 4h / 1D); swipe cycles through them |
| Timezone | UTC offset for your location |
| Candle Colour | Classic, Color Shift, Neon Pulse, or Custom (swatches shown for each option) |
| After-Hours Animation | Tidepool / Coral Reef / Starfield / Countdown / Pixel Beach / Grassland |
| Asset Cycling | Enable auto-cycle and set the interval (5–120 s); disable for manual swipe only |
| Brightness | Display brightness 10–100% (adjusts live as you drag) |
| About / Diagnostics | Free heap, PSRAM, WiFi SSID/RSSI/IP, NTP status, uptime |
| Share SD over USB | Exposes the micro-SD card to your PC as a USB drive — see [SD Card Access over USB](#sd-card-access-over-usb) |
| Re-do WiFi Setup | Wipes credentials and returns to the captive-portal setup screen |

**Save & Restart** — writes all changes to NVS and reboots.  
**Cancel** — discards changes and returns to the chart with no reboot.

---

## SD Card Access over USB

The device has a micro-SD card slot (a 512 MB card is included). The firmware writes a running
diagnostic log to the card (`deskticker.log`) so it can be tested unattended. **Share SD over USB**
lets you read and write that card directly from your computer over the USB-C cable — no need to
pop the card out and use a card reader.

> Requires the firmware to be built with **USB Mode = "USB-OTG (TinyUSB)"** (see
> [Build Instructions](#1-arduino-ide-settings)). The board's USB-C is wired to the ESP32-S3's
> native USB, so it appears to Windows as both a serial port **and** a removable drive.

### How to use it

1. Connect the device to your PC with the USB-C cable.
2. Triple-tap the screen to open **Settings**, then tap **Share SD over USB**.
3. The screen shows **"SD Shared with PC."** The card now appears as a **removable drive** in
   Windows File Explorer (and as a disk on macOS/Linux). Copy files off, add files, or delete
   them — it is a normal read/write drive.
4. When you're finished, **⚠️ Safely Eject the drive first** (see below), then **tap the screen**.
   The device restarts and resumes the ticker.

### ⚠️ Always "Safely Eject" before tapping to exit

Because you have full read/write access, Windows may still be finishing writes in the background.
**Before** you tap the screen to leave share mode:

- **Windows:** click the **"Safely Remove Hardware / Eject"** icon in the system tray (or
  right-click the drive in File Explorer → **Eject**) and wait for the "safe to remove" message.
- **macOS:** drag the drive to the Trash / click the **eject** ⏏ icon next to it in Finder.

Skipping this can leave the last files you copied incomplete or corrupt the card's file table.
Once the drive is ejected, tap the screen — the device reboots and the ticker comes back.

### Good to know

- **One at a time.** While the card is shared with the PC, the ticker is paused and the firmware
  stops logging to the card — this is deliberate, so the PC is the only thing writing it and the
  card can never be corrupted by both sides at once.
- **It will not freeze the device.** USB is a separate system from the display, and nothing new
  runs while the card is shared.
- **Returning is a quick reboot.** Tapping to exit restarts the device (a few seconds) and it
  reconnects automatically.
- **When *not* sharing,** Windows may show the drive letter as "insert a disk" — that's normal;
  the card is in use by the ticker until you choose **Share SD over USB**.

---

## Reset / Return to Setup

**Hold the BOOT button for 3 seconds.**

The **BOOT** button is the small button labeled BOOT on the ESP32-S3 board (GPIO 0) —
not the RST/Reset button. All saved settings are wiped and the device restarts into setup mode.

---

## Folder Structure

```
DeskTicker/
├── DeskTicker.ino         # Main sketch & state machine
├── assets.h / .cpp        # Ticker definitions & structs; custom-ticker library + symbol lookup
├── settings.h / .cpp      # NVS preferences (WiFi, assets, timeframes, timezone, theme)
├── wifi_manager.h / .cpp  # Captive-portal setup UI + web form handler
├── api_client.h / .cpp    # Yahoo Finance v8 chart API, 4H aggregation, host fallback
├── chart_screen.h / .cpp  # LVGL candlestick chart, header, y-axis, footer
├── animations.h / .cpp    # After-hours animations (tidepool, reef, starfield, countdown, pixel beach, grassland)
├── settings_screen.h / .cpp  # On-device settings menu (triple-tap to open)
├── tz_options.h / .cpp    # Shared 34-entry timezone table (used by setup portal + settings menu)
├── sdlog.h / .cpp         # SD-card diagnostic logger (deskticker.log)
├── usb_msc.h / .cpp       # Share SD over USB — expose the SD card to a PC as a USB drive
├── CLAUDE.md              # Architecture notes for AI-assisted development
│
│  ── Board support files (from JC3248W535EN DEMO_LVGL) ──
├── esp_bsp.h / .c
├── esp_lcd_axs15231b.h / .c
├── esp_lcd_touch.h / .c
├── lv_port.h / .c
├── display.h
├── lv_conf.h
└── bsp_err_check.h
```
