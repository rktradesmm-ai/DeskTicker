# Lil Fish Terminal — Claude Context

## Required Toolchain

**ESP32 Arduino core: 3.0.7** (Boards Manager → "esp32 by Espressif" → 3.0.7)

- Do **NOT** upgrade to 3.1.0 or later — introduces a display deadlock regression
  that hangs the aquarium/beach/starfield animations in ~40 min.
- Do **NOT** downgrade to 3.0.2 — HTTP client bug causes `YF: IncompleteInput`
  errors on Yahoo Finance responses.
- 3.0.7 is the bisect-tested sweet spot: hang window extends to 2–9 h (vs 30–45 min
  on 3.3.8), and HTTP works correctly.
- See `BISECT_LOG.md` for the full bisect history and test results.

## Project Overview

ESP32-S3 desk trading terminal. Fetches live OHLCV candle data from Yahoo Finance and renders
a candlestick chart on a 3.5" 480×320 IPS touchscreen. No cloud backend, no API key.

**Hardware:** JC3248W535C_I_Y board — ESP32-S3, AXS15231B QSPI display driver, capacitive touch.
**Framework:** Arduino + LVGL v8.3.9 + FreeRTOS.

---

## File Map

| File | Responsibility |
|------|---------------|
| `LilFishTerminal.ino` | Main state machine, WiFi/NTP lifecycle, loop orchestration |
| `assets.h` | `AssetDef` / `AssetData` / `Candle` structs; `ASSETS[]` table (24 assets); `asset_find()` |
| `settings.h / .cpp` | `Settings` struct; NVS load/save via Arduino `Preferences` |
| `api_client.h / .cpp` | Yahoo Finance v8 chart API fetch; 4H aggregation; host fallback |
| `chart_screen.h / .cpp` | LVGL chart screen: header, candle canvas, y-axis canvas, footer |
| `wifi_manager.h / .cpp` | AP captive-portal setup UI (LVGL screen + HTTP web form); asset picker grouped into Crypto / Stocks & ETFs / Commodities / Forex sections |
| `animations.h / .cpp` | After-hours fullscreen animations: aquarium, beach, starfield, countdown |
| `settings_screen.h / .cpp` | On-device settings menu: assets, TF, TZ, candle colour, anim, cycling, brightness, diagnostics |
| `tz_options.h / .cpp` | Shared 34-entry timezone table consumed by both `wifi_manager` and `settings_screen` |

Board support files (`esp_bsp`, `lv_port`, `esp_lcd_*`, `display.h`, `lv_conf.h`) are copied
from the JC3248W535EN demo. **Do not edit them except for the two deliberate changes below.**

**Deliberate change — `lv_port.c` line ~242:** `disp_drv.full_refresh = 0` (was `1`).
Enables partial refresh so LVGL only flushes the changed dirty region each frame, reducing
sustained QSPI DMA load that causes the silent-deadlock hang.

**Do not change the semaphore waits.** Both `trans_done_sem` wait (`lv_port.c`) and
`te_v_sync_sem` wait (`esp_bsp.c`) are at stock `portMAX_DELAY`. An earlier experiment
replaced these with bounded `pdMS_TO_TICKS(100/250)` values; that was reverted because the
bounded timeouts broke QSPI/DMA serialization, allowing a new transfer to be queued before
the previous one completed, silently wedging the SPI driver's internal queue.

---

## State Machine (`LilFishTerminal.ino`)

```
S_INIT → S_WIFI_SETUP (first boot, no saved WiFi)
       → S_CONNECTING (saved WiFi exists)

S_CONNECTING → S_NTP_SYNC (connected)
             → S_WIFI_SETUP (retries exhausted, no cached data)
             → S_CHART / S_AFTER_HOURS (retries exhausted, has cached data)

S_NTP_SYNC → S_FETCH

S_FETCH → S_CHART (market open)
        → S_AFTER_HOURS (market closed)
        → S_RECONNECT (HTTP -1: TCP failure on both YF hosts)

S_CHART → S_FETCH (refresh interval, swipe, auto-cycle, market close)
        → S_RECONNECT (WiFi lost)
        → S_SETTINGS (triple-tap)

S_AFTER_HOURS → S_FETCH (re-check every 5 min, swipe, auto-cycle)
              → S_RECONNECT (WiFi lost)
              → S_SETTINGS (triple-tap)

S_SETTINGS → S_FETCH (cancel — chart_displaced flag restores the chart)
           → reboot (save)

S_RECONNECT → S_FETCH (soft reconnect succeeded, chart was visible — chart stays on screen)
            → S_NTP_SYNC (soft reconnect succeeded but NTP was never done)
            → S_CONNECTING (soft window exhausted, or no chart visible — full screen)
```

`S_RECONNECT` tries up to `RECONNECT_SOFT_TRIES` × `RECONNECT_SOFT_MS` silent
reconnects while the chart stays on screen ("Reconnecting…" in header). Only
falls back to the full `S_CONNECTING` screen if the network stays down for the
whole soft window, or if no chart is currently visible (e.g. first-boot,
after-hours).

---

## Critical Patterns

### LVGL Lock
Every LVGL call must be made while holding the display mutex:
```cpp
if (LV_LOCK()) {
    // LVGL calls here
    LV_UNLOCK();
}
```
`LV_LOCK()` = `bsp_display_lock(2000)`, `LV_UNLOCK()` = `bsp_display_unlock()`.
The timeout is **2000 ms, not 0**. `bsp_display_lock(0)` maps to `portMAX_DELAY` (infinite wait). The vendor flush callback (`bsp_display_sync_cb`) holds `lvgl_mux` while waiting for the panel's tearing-effect interrupt with `portMAX_DELAY`; if a TE interrupt is missed, the LVGL task is stuck forever holding the mutex and an infinite-wait `LV_LOCK()` in the main loop never returns → total freeze requiring power cycle. A 2 s timeout lets the main loop skip the update and retry, converting a permanent freeze into a brief self-healing stall.
Forgetting the lock causes silent corruption or crashes.

### Screen Lifecycle (pending_del_scr)
LVGL crashes if you `lv_obj_del()` the currently active screen.
The pattern used here:
1. `queue_scr_for_delete(old_scr)` — stores the screen to be deleted
2. Load the new screen with `lv_scr_load()`
3. `cleanup_pending_scr()` — now safely deletes the queued screen

Always call `cleanup_pending_scr()` immediately after `lv_scr_load()` / `chart_screen_show()` / `anim_start()`.

**Critical for chart → after-hours transitions:** `chart_screen_destroy()` must be called **after** `anim_start()` loads the animation screen, not before. When swiping from a crypto asset (which always shows a chart) to a closed-market commodity, the chart is the active screen; destroying it first leaves `act_scr` dangling and `anim_start`'s own `lv_scr_load` dereferences it → `LoadProhibited` crash. The fix: compare `lv_scr_act()` before and after `anim_start()`, then destroy the chart only if the active screen changed.

### Background Refresh (bg_refresh flag)
When `chart_created == true`, the chart is already visible. Avoid displacing it with a loading screen:
```cpp
bool bg_refresh = chart_created;
if (!bg_refresh) show_connecting("Fetching...");
else { /* one locked set_status("Fetching…") — see below */ }
// ... fetch ...
hide_connecting();
// on success, inside the final chart_screen_update() lock:
//   chart_screen_set_status("");  chart_screen_update(...);
```
On fetch failure during bg_refresh, write the error to `chart_screen_set_status()` (NOT `show_error()`) — `show_error()` creates a persistent label on `chart_scr` with no cleanup path.

**Exactly ONE locked `chart_screen_set_status` write before the fetch; the clear is folded into the final `chart_screen_update()` lock (not a separate lock block).** Each standalone locked label write is a separate LVGL flush. The original code had `set_status("Fetching…")` + `set_status("")` as two separate lock blocks → two extra flushes on top of the gesture flush and the full-redraw flush → 4 back-to-back flushes maximized the probability of hitting the vendor TE-sync race in `bsp_display_sync_cb` / `bsp_display_sync_task` → device freeze. Rule: one set (before fetch), zero-cost clear (inside the redraw lock).

### Gesture Flags
Swipe flags are `volatile int8_t` set by the LVGL task and read+cleared by the main loop.
Read with the dedicated accessors (they reset the flag atomically):
```cpp
int chart_screen_get_swipe();       // ±1 horizontal (assets)
int chart_screen_get_swipe_vert();  // ±1 vertical (timeframes)
int anim_get_swipe();               // ±1 horizontal (assets, after-hours screen)
```
Swipe directions (with LV_DISP_ROT_90): LEFT=+1 (next asset), RIGHT=-1 (prev asset),
TOP=+1 (next timeframe), BOTTOM=-1 (prev timeframe).

### PSRAM Canvas Buffers
Both chart canvases are allocated in OPI PSRAM with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
Free them in `chart_screen_destroy()` before the parent object is deleted.
Current sizes:
- `chart_buf`: `CANVAS_W × CANVAS_H × 2` bytes ≈ 150 KB
- `yaxis_buf`: `YAXIS_W × CHART_H × 2` bytes ≈ 35 KB
- `anim_buf` (animations): `SCR_W × SCR_H × 2` bytes ≈ 300 KB

---

## Layout Constants (`chart_screen.h`)

| Constant | Value | Meaning |
|----------|-------|---------|
| `SCR_W / SCR_H` | 480 / 320 | Logical screen size after 90° rotation |
| `HDR_H / FTR_H` | 48 / 48 | Header and footer height |
| `CHART_H` | 224 | Chart area height (SCR_H − HDR_H − FTR_H) |
| `LEFT_MARGIN` | 8 | Chart canvas left offset (matches header 8px left inset) |
| `RIGHT_MARGIN` | 8 | Gap between y-axis canvas right edge and screen edge |
| `YAXIS_W` | 80 | Y-axis canvas width (must fit 9-digit prices at font_14) |
| `CANVAS_W` | 384 | Chart canvas width (SCR_W − YAXIS_W − LEFT_MARGIN − RIGHT_MARGIN) |
| `CANVAS_H` | 196 | Chart canvas height (CHART_H − 28 px for x-axis labels) |
| `XAXIS_H` | 28 | X-axis label strip below candle canvas (CHART_H − CANVAS_H) |
| `CHART_BOX_MARGIN` | 6 | Vertical gap between header/footer and the chart box border |
| `CANDLE_W / CANDLE_G` | 7 / 2 | Candle body width and gap |
| `CANDLE_STEP` | 9 | Pixels per candle slot |
| `CHART_RIGHT_PAD` | 4 | Empty candle slots right of newest candle |
| `MAX_VIS_CANDLES` | ~38 | Visible candles = CANVAS_W/CANDLE_STEP − CHART_RIGHT_PAD |

Y-axis canvas sits at `x = LEFT_MARGIN + CANVAS_W = 392`, width 80, right edge at 472.
The rightmost 8px (472–480) is bare `COL_BG` background (right margin).

---

## Header Layout

Flex row (left-aligned, vertically centered) inside `hdr`:
- `lbl_ticker` (font_22, `pad_right=6`) → 1×18px vertical separator bar → `lbl_price` (font_22, `pad_left=6`) → `lbl_change` (font_16, `pad_left=6`)
- `lbl_status` (font_10, right-anchored at −65px) — shows "Fetching …" or error during bg refresh
- `lbl_tf` (font_16, right-anchored at −8px) — active timeframe label

Footer: `lbl_ohlc` + `lbl_vol` (left, font_12) | `dot_wifi` 10×10 circle (right, −8px).

---

## Y-Axis Drawing

Drawn entirely with `lv_canvas_draw_*` (not LVGL widgets):
- **Grid labels** (font_14, `LV_TEXT_ALIGN_LEFT`, x=LABEL_PAD=4): 4 evenly spaced price levels
- **Current price pill** (blue rect `COL_CUR_PX`, height 17px): width = `text_width + PILL_PAD(16px)`, right-aligned to yaxis canvas right edge; font_12 text is explicitly centered inside the pill

Grid labels use `lv_txt_get_size` to measure the text pixel width, then draw at `x = YAXIS_W - text_width` with `max_w = text_width + 1`. This right-aligns every label flush with the yaxis canvas right edge (= screen right edge minus RIGHT_MARGIN), matching the blue current-price pill. The gap between the chart gridline and the first digit adjusts automatically: short prices (e.g. "9.42") get a wider gap, long prices (e.g. "78959.74") get a narrower gap. Using a bounding box exactly as wide as the text also defeats the LVGL canvas centering quirk that caused inconsistent alignment in earlier versions.

If YAXIS_W is reduced, verify that a 9-digit price ("103245.12") still fits at font_14 (~74px).

---

## API Client (`api_client.cpp`)

- **Endpoint:** `https://query1.finance.yahoo.com/v8/finance/chart/{ticker}?interval=…&period1=…&period2=…`
- **Fallback host:** `query2.finance.yahoo.com` — tried on any YF error string
- **HTTP -1** (TCP failure on both hosts) triggers `S_RECONNECT` in the main loop
- **Streaming avoided:** `http.useHTTP10(true)` + `http.getString()` reads the full body before parsing
- **JSON filter:** `StaticJsonDocument<512>` filter + `PsramJsonDocument(49152)` — genuinely allocated in PSRAM via `SpiRamAllocator` (see top of `api_client.cpp`); the small anchor doc uses `PsramJsonDocument(8192)`
- **4H timeframe:** fetched as 1H bars, aggregated 4:1 on-device in `yf_try_host()`; the partial (still-forming) 4H group at the tail **is emitted** (not discarded) so `today_open_utc()` can find the 00:00-UTC bucket open price
- **Candle cap:** non-4H path uses `int start = (raw > MAX_CANDLES) ? raw - MAX_CANDLES : 0` to prevent buffer overflow
- **Fetch window (`yf_period1`):** signature is `yf_period1(tf, market, continuous)`. Assets where `market == MARKET_CRYPTO` **or** `continuous == 1` use the small 24/7-sized window (e.g. 4H = 12 cal days of 1h bars ≈ 288 bars). All other assets use the stock window (4H = 55 cal days ≈ 240 1h bars). **Root cause of the GOLD/OIL/SILVER reset crash:** those 24/7 crypto-style tokens classified as `MARKET_COMMODITY` previously fell into the stock window, pulling ~1300 hourly bars → ~150-250 KB JSON → ESP32 heap exhaustion → panic/reset. Fixed by setting `continuous = 1` on those entries in `ASSETS[]`.

Refresh intervals: 15m=30s, 1H=30s, 4H=30s, 1D=600s.

---

## Settings (`settings.h / .cpp`)

Stored in NVS namespace `"lilfish"` via `Preferences`. Key names:

| Key | Type | Meaning |
|-----|------|---------|
| `init` | bool | True once settings have been saved at least once |
| `ssid` / `pass` | string | WiFi credentials |
| `wifi_ok` | bool | True after first successful save |
| `n_assets` | int | Asset count (1–6) |
| `a0`…`a5` | string | Asset symbols |
| `tf` | int | Active timeframe (TF_* value) |
| `tf_n` | int | Number of enabled timeframes |
| `tf0`…`tf3` | int | Enabled timeframe values |
| `theme` | int | THEME_* |
| `bull` / `bear` | uint | 0xRRGGBB candle colors |
| `cycle` | int | Auto-cycle seconds (0 = manual) |
| `anim` | int | ANIM_* |
| `tz` | int | UTC offset in minutes (e.g. −300 = UTC−5) |

On load, if `tf_n` is missing (old firmware), the single saved `tf` is wrapped into `timeframes[0]` for backward compatibility.

---

## Timezone

`cfg.tz_offset` is UTC offset in minutes. Converted to a POSIX TZ string by `make_tz_string()`:
- POSIX sign is opposite to human convention: UTC+5:30 → offset=+330 → `"UTC-5:30"`
- Applied with `configTzTime(tz_buf, NTP_SERVER)` during `S_NTP_SYNC`
- No DST — user sets their current local offset manually
- `is_after_hours()` local-time fallback compares against 9:30–16:00 ET regardless of `tz_offset`; this is only used when the API `market_state` field is absent

---

## BOOT Button Reset

GPIO 0 (labeled **BOOT** on the board, NOT RST). Held LOW for ≥ 3 seconds in `loop()` calls
`settings_clear()` + `ESP.restart()` → returns to `S_WIFI_SETUP`.

---

## After-Hours Animations

All share `anim_scr` + `anim_canvas` (PSRAM) driven by `lv_timer_create()`.
Countdown (`ANIM_COUNTDOWN`) is LVGL-widget based (`cd_scr`, no canvas), uses a 1 s timer.
`anim_stop()` deletes the timer first, then the screen / frees PSRAM.

---

## On-Device Settings Menu

### Triple-Tap Trigger

The AXS15231B touch IC pulses its INT line only on touch *transitions* (down / up), never
continuously while a finger rests. This means stationary holds produce exactly one PRESSED
and one RELEASED event — LVGL never sees repeated `LV_EVENT_PRESSING` ticks, so a hold
timer cannot work. **Two-finger hold is also impossible** — the driver is single-touch only.

Fix: **triple-tap** (3 taps within 1.2 s). Each discrete tap fires a clean down+up interrupt
pair, registering reliably. A swipe emits `LV_EVENT_GESTURE` (never `LV_EVENT_CLICKED`),
so swipes can never be mistaken for taps. State tracked in `chart_tap_count` /
`chart_last_tap_ms` (chart screen) and `anim_tap_count` / `anim_last_tap_ms` (anim screens).

### Click Routing on the Chart Screen

`LV_EVENT_CLICKED` is delivered only to the object the press lands on (`act_obj`) — it does
**not** bubble. The chart screen is fully covered by container objects (`hdr`, `left_cont`,
`lbl_dot`, `chart_cont`, `ftr`, `dot_wifi`), which would swallow every tap. Fix: clear
`LV_OBJ_FLAG_CLICKABLE` on all six containers so hit-testing falls through to `chart_scr`
where `chart_tap_cb` is registered. Gesture bubbling (`LV_OBJ_FLAG_GESTURE_BUBBLE`) is on by
default and is unaffected — swipes still reach `chart_scr`.

### S_SETTINGS State

- **Entry:** copy `Settings work = cfg;` then call `settings_screen_create(&work)` under
  `LV_LOCK()`; use `cleanup_pending_scr()` to delete the prior screen.
- **Poll loop:** call `settings_screen_poll()` each iteration →
  `0` = still editing / `1` = save / `-1` = cancel.
- **Save (1):** `settings_screen_get(&out); settings_save(&out);` display a brief reboot
  warning, then `ESP.restart()`.
- **Cancel (-1):** destroy the settings screen, set `last_fetch_ms = 0`,
  `state = S_FETCH`. The existing `chart_displaced` path at the top of `S_FETCH` rebuilds
  the chart without a loading screen if it was previously visible.

### Settings Screen API (`settings_screen.h`)

```cpp
void settings_screen_create(const Settings* work); // builds UI + lv_scr_load; under LV_LOCK
int  settings_screen_poll();                        // 0 = editing / 1 = save / -1 = cancel
void settings_screen_get(Settings* out);            // copy edited working copy out
void settings_screen_destroy();                     // teardown + queue delete; under LV_LOCK
```

### Candle Colour Page

The theme picker uses **4 custom selectable rows** (not `lv_roller`) so each row can display
up/down candle color swatches. ColorShift and NeonPulse are animated at runtime but the
swatches show a fixed representative sample. The `theme_roller` static pointer was removed;
`ss_work.theme` is written immediately in the tap callback, not on save. Arrays:
`theme_rows[4]` and `theme_checks[4]` (checkmark labels, hidden/shown on selection).

---

## Common Gotchas

- **Never call `lv_obj_del()` on `lv_scr_act()`** — always use `queue_scr_for_delete` + `cleanup_pending_scr`.
- **`show_error()` creates an unmanaged label** on the active screen. Only safe to use when `bg_refresh == false` (i.e., conn_scr is active and will be deleted shortly). When `bg_refresh == true`, use `chart_screen_set_status()` instead.
- **`lv_font_montserrat_N`** must be enabled in `lv_conf.h`. Currently enabled: 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 40, 48.
- **Text alignment in canvas**: set `dsc.align` before calling `lv_canvas_draw_text()`. For left-aligned text, start x at 4 for a consistent left margin; width is the bounding box.
- **4H aggregation** emits the partial (still-forming) tail group — this is intentional so the 00:00-UTC bucket open is always present for `today_open_utc()` to anchor the % change correctly.
- **`continuous = 1` is required for any 24/7 asset** (crypto-style tokens, CME futures, anything that trades outside stock-market hours). Omitting it causes the 4H fetch window to request ~1300 hourly bars → huge JSON → ESP32 heap exhaustion → reset.
- **ArduinoJson documents must use `PsramJsonDocument`, not `DynamicJsonDocument`**. `DynamicJsonDocument` allocates from the internal heap (only ~200 KB), which mbedTLS / WiFiClientSecure fragmentation can exhaust, causing a directional reset (e.g. crypto-then-commodity swipe on 1D). `PsramJsonDocument` uses the 8 MB PSRAM and is immune to this.
- **`CANVAS_X` define is unused** — canvas position is set explicitly with `lv_obj_set_pos`.
