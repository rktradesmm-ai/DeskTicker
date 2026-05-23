# Lil Fish Trading Terminal

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
> - See `LilFishTerminal/BISECT_LOG.md` for the full version-bisect history.

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

1. Open `LilFishTerminal/LilFishTerminal.ino` in Arduino IDE.
2. All board support files (`esp_bsp.c`, `lv_port.c`, etc.) are already in the sketch folder.
3. Select your COM port and click Upload.

---

## First-Time Setup

1. Power on the device. It shows the **Lil Fish Setup** screen.
2. On your phone or laptop, connect to WiFi: **`LilFish-Setup`**
3. Open a browser and go to **`192.168.4.1`** (captive portal may open automatically).
4. Configure:
   - **WiFi** — your network name and password
   - **Assets** — pick up to 6, grouped by class (Crypto / Stocks & ETFs / Commodities / Forex)
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
| Aquarium | Fish of mixed sizes drifting over a deep-blue ocean floor with gravel |
| Sunset Beach | Animated beach sunset with a soft horizon and gentle waves |
| Starry Sky | Softly twinkling stars on a dark sky with an occasional shooting star |
| Countdown | Digital clock counting down to the next NYSE/NASDAQ open (9:30 AM ET) |

---

## On-Device Settings

Triple-tap the chart or after-hours screen to open the **Settings** menu without rebooting.

| Section | What you can change |
|---------|---------------------|
| Assets | Which tickers are shown (1–6); grouped by Crypto / Stocks & ETFs / Commodities / Forex |
| Timeframes | Which intervals are active (15m / 1h / 4h / 1D); swipe cycles through them |
| Timezone | UTC offset for your location |
| Candle Colour | Classic, Color Shift, Neon Pulse, or Custom (swatches shown for each option) |
| After-Hours Animation | Aquarium / Sunset Beach / Starry Sky / Countdown |
| Asset Cycling | Enable auto-cycle and set the interval (5–120 s); disable for manual swipe only |
| Brightness | Display brightness 10–100% (adjusts live as you drag) |
| About / Diagnostics | Free heap, PSRAM, WiFi SSID/RSSI/IP, NTP status, uptime |
| Re-do WiFi Setup | Wipes credentials and returns to the captive-portal setup screen |

**Save & Restart** — writes all changes to NVS and reboots.  
**Cancel** — discards changes and returns to the chart with no reboot.

---

## Reset / Return to Setup

**Hold the BOOT button for 3 seconds.**

The **BOOT** button is the small button labeled BOOT on the ESP32-S3 board (GPIO 0) —
not the RST/Reset button. All saved settings are wiped and the device restarts into setup mode.

---

## Folder Structure

```
LilFishTerminal/
├── LilFishTerminal.ino    # Main sketch & state machine
├── assets.h               # Ticker definitions, candle & data structs
├── settings.h / .cpp      # NVS preferences (WiFi, assets, timeframes, timezone, theme)
├── wifi_manager.h / .cpp  # Captive-portal setup UI + web form handler
├── api_client.h / .cpp    # Yahoo Finance v8 chart API, 4H aggregation, host fallback
├── chart_screen.h / .cpp  # LVGL candlestick chart, header, y-axis, footer
├── animations.h / .cpp    # After-hours animations (aquarium, beach, starfield, countdown)
├── settings_screen.h / .cpp  # On-device settings menu (triple-tap to open)
├── tz_options.h / .cpp    # Shared 34-entry timezone table (used by setup portal + settings menu)
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
