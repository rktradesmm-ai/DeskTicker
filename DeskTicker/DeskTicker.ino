/*
 * DeskTicker
 * ──────────────────────────────────────────────────────────────────────────────
 * Hardware: JC3248W535C_I_Y (ESP32-S3, 3.5" 320×480 IPS QSPI, AXS15231B)
 *
 * Required Arduino libraries (install via Library Manager):
 *   - ArduinoJson  v6.x  (by Benoit Blanchon)
 *
 * Board files already in this folder (copied from DEMO_LVGL):
 *   esp_bsp.h/c, esp_lcd_axs15231b.h/c, esp_lcd_touch.h/c,
 *   lv_port.h/c, display.h, lv_conf.h, bsp_err_check.h
 *
 * Arduino IDE settings:
 *   Board:       ESP32S3 Dev Module
 *   PSRAM:       OPI PSRAM
 *   Flash Size:  16MB
 *   Partition:   Huge APP (3MB No OTA / 1MB SPIFFS)
 *   CPU Speed:   240 MHz
 *
 * Build note: copy the lvgl library folder from
 *   JC3248W535EN/1-Demo/Demo_Arduino/libraries/lvgl
 *   into your Arduino libraries directory.
 * ──────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <lvgl.h>
#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

#include "assets.h"
#include "settings.h"
#include "wifi_manager.h"
#include "api_client.h"
#include "chart_screen.h"
#include "animations.h"
#include "settings_screen.h"

// ── Config ────────────────────────────────────────────────────────────────────
#define LVGL_ROTATION     LV_DISP_ROT_90
#define WIFI_RETRY             3
#define RECONNECT_SOFT_TRIES   3     // quick reconnect attempts before showing full screen
#define RECONNECT_SOFT_MS   5000     // per-attempt timeout ms (~15 s total soft window)
#define NTP_SERVER        "pool.ntp.org"
#define RESET_BTN_GPIO    0   // BOOT button on ESP32-S3 — hold 3 s to re-run setup

// ── State machine ─────────────────────────────────────────────────────────────
enum State {
    S_INIT,
    S_WIFI_SETUP,
    S_CONNECTING,
    S_NTP_SYNC,
    S_FETCH,
    S_CHART,
    S_AFTER_HOURS,
    S_RECONNECT,
    S_SETTINGS
};

// ── Reset button state ────────────────────────────────────────────────────────
static unsigned long btn_held_since = 0;

// Generate a no-DST POSIX TZ string from UTC offset in minutes.
// POSIX sign convention is opposite to human convention:
//   human UTC+5:30 → offset=330 → "UTC-5:30"
//   human UTC-5    → offset=-300 → "UTC+5"
static void make_tz_string(int offset_min, char* buf, size_t n) {
    int posix = -offset_min;
    int h = posix / 60;
    int m = abs(posix % 60);
    if (m == 0) snprintf(buf, n, "UTC%+d", h);
    else        snprintf(buf, n, "UTC%+d:%02d", h, m);
}

static State       state            = S_INIT;
static Settings    cfg;
static AssetData   asset_data[MAX_ASSETS];
static int         cur_idx          = 0;
static unsigned long last_cycle_ms  = 0;
static unsigned long last_fetch_ms  = 0;
static int         wifi_retries     = 0;
static bool        chart_created    = false;
static bool        anim_running     = false;
// True when conn_scr or the after-hours animation actually covered the chart
// screen while it was built. Only then do we need to call chart_screen_show()
// to bring the chart back. A plain background refresh never displaces the chart,
// so this flag stays false and we skip the lv_scr_load that caused the flash.
static bool        chart_displaced  = false;
static bool        ntp_synced       = false;

// ── LVGL helper macros ────────────────────────────────────────────────────────
#define LV_LOCK()   bsp_display_lock(2000)  // 2 s timeout — prevents permanent freeze if vendor TE flush stalls
#define LV_UNLOCK() bsp_display_unlock()

// ── Safe screen deletion ──────────────────────────────────────────────────────
// Rule: never call lv_obj_del() on the ACTIVE screen — LVGL leaves act_scr dangling.
// Instead, queue the screen here and call cleanup_pending_scr() AFTER a new screen loads.
static lv_obj_t* pending_del_scr = nullptr;

static void queue_scr_for_delete(lv_obj_t* s) {
    // Called while holding LV_LOCK. If there's already a pending one, delete it now
    // (it must be inactive since we're about to replace it with another queued screen).
    if (pending_del_scr && pending_del_scr != s) {
        lv_obj_del(pending_del_scr);
    }
    pending_del_scr = s;
}

// Call this AFTER lv_scr_load() has switched to a new screen.
static void cleanup_pending_scr() {
    if (pending_del_scr) {
        lv_obj_del(pending_del_scr);
        pending_del_scr = nullptr;
    }
}

// ── Splash screen ─────────────────────────────────────────────────────────────

// Frees the PSRAM canvas buffer when the splash screen object is deleted.
static void splash_buf_free_cb(lv_event_t* e) {
    void* buf = lv_event_get_user_data(e);
    if (buf) heap_caps_free(buf);
}

// Helper: adds the colored "DeskTicker" spangroup and tagline to a screen.
static void splash_add_wordmark(lv_obj_t* scr, int y_ofs_wm, int y_ofs_tag) {
    lv_obj_t* sg = lv_spangroup_create(scr);
    lv_obj_set_style_bg_opa(sg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sg, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sg, 0, LV_PART_MAIN);
    lv_obj_set_width(sg,  LV_SIZE_CONTENT);
    lv_obj_set_height(sg, LV_SIZE_CONTENT);

    const lv_font_t* f = &lv_font_montserrat_40;
    lv_span_t* sp;
    sp = lv_spangroup_new_span(sg); lv_span_set_text_static(sp, "D");
    lv_style_set_text_color(&sp->style, lv_color_hex(0x22C55E));
    lv_style_set_text_font(&sp->style, f);
    sp = lv_spangroup_new_span(sg); lv_span_set_text_static(sp, "esk");
    lv_style_set_text_color(&sp->style, lv_color_hex(0xE6E9EF));
    lv_style_set_text_font(&sp->style, f);
    sp = lv_spangroup_new_span(sg); lv_span_set_text_static(sp, "T");
    lv_style_set_text_color(&sp->style, lv_color_hex(0xEF4444));
    lv_style_set_text_font(&sp->style, f);
    sp = lv_spangroup_new_span(sg); lv_span_set_text_static(sp, "icker");
    lv_style_set_text_color(&sp->style, lv_color_hex(0xE6E9EF));
    lv_style_set_text_font(&sp->style, f);
    lv_spangroup_refr_mode(sg);  // compute size before alignment
    lv_obj_align(sg, LV_ALIGN_CENTER, 0, y_ofs_wm);

    lv_obj_t* sub = lv_label_create(scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7A8290), LV_PART_MAIN);
    lv_label_set_text(sub, "The market, on your desk.");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, y_ofs_tag);
}

static void show_splash() {
    // PSRAM canvas for the crab pixel art. Text uses LVGL widgets (no extra flush).
    lv_color_t* splash_buf = (lv_color_t*)heap_caps_malloc(
        (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);

    if (!LV_LOCK()) { if (splash_buf) heap_caps_free(splash_buf); return; }

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1117), LV_PART_MAIN);

    if (splash_buf) {
        // Attach delete callback so PSRAM buffer is freed when screen is deleted.
        lv_obj_add_event_cb(scr, splash_buf_free_cb, LV_EVENT_DELETE, splash_buf);
        lv_obj_t* cvs = lv_canvas_create(scr);
        lv_canvas_set_buffer(cvs, splash_buf, SCR_W, SCR_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(cvs, 0, 0);
        lv_canvas_fill_bg(cvs, lv_color_hex(0x0E1117), LV_OPA_COVER);

        // "The Pinch" lockup: crab perched directly above the wordmark,
        // claws hanging down naturally toward the D (left/green) and T (right/red).
        // Wordmark center at (SCR_W/2, SCR_H/2 + 18) = (240, 178), ~40 px tall.
        // Crab cy=142 places its lowest legs ~6 px above the wordmark top edge.
        anim_draw_crab(cvs, SCR_W / 2, 128,
                       cfg.bull_rgb, cfg.bear_rgb,
                       /*blink=*/false, /*walk_frame=*/0, /*claws_raised=*/false);
    } else {
        // PSRAM unavailable: wordmark-only fallback.
        splash_add_wordmark(scr, 0, 38);
    }

    // Wordmark and tagline are LVGL widgets — they don't trigger an extra flush.
    if (splash_buf) splash_add_wordmark(scr, 10, 48);

    lv_scr_load(scr);           // single full-screen flush, no separate invalidate
    queue_scr_for_delete(scr);
    LV_UNLOCK();

    delay(3000);  // static hold; WiFi connecting in background
}

// ── Connecting screen ─────────────────────────────────────────────────────────
static lv_obj_t* conn_scr  = nullptr;
static lv_obj_t* conn_lbl  = nullptr;

static void show_connecting(const char* msg) {
    if (!LV_LOCK()) return;
    if (!conn_scr) {
        conn_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(conn_scr, lv_color_hex(0x0E1117), LV_PART_MAIN);
        conn_lbl = lv_label_create(conn_scr);
        lv_obj_set_style_text_font(conn_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(conn_lbl, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
        lv_obj_align(conn_lbl, LV_ALIGN_CENTER, 0, 0);
        // If the chart was built and visible, loading conn_scr over it displaces
        // it — remember this so we can call chart_screen_show() after the fetch.
        if (chart_created) chart_displaced = true;
        lv_scr_load(conn_scr);
        // The screen we just replaced is now safe to delete.
        cleanup_pending_scr();
    }
    lv_label_set_text(conn_lbl, msg);
    LV_UNLOCK();
}

// Queue conn_scr for deletion. The caller must load a new screen promptly.
static void hide_connecting() {
    if (!LV_LOCK()) return;
    if (conn_scr) {
        queue_scr_for_delete(conn_scr);
        conn_scr = nullptr;
        conn_lbl = nullptr;
    }
    LV_UNLOCK();
}

// ── US Eastern time helpers ───────────────────────────────────────────────────
// NYSE open/close is always defined in ET. These helpers let is_after_hours()
// and secs_to_market_open() work correctly regardless of the user's tz_offset.

// Returns true when UTC time t is in US EDT (UTC-4) rather than EST (UTC-5).
// Post-2007 rules: 2nd Sunday of March 2am EST → 1st Sunday of November 2am EDT.
static bool is_us_dst(time_t t) {
    struct tm u; gmtime_r(&t, &u);
    int month = u.tm_mon + 1;
    if (month < 3 || month > 11) return false;
    if (month > 3 && month < 11) return true;
    // Find day-of-month of the relevant Sunday cutover.
    struct tm probe = {};
    probe.tm_year = u.tm_year;
    probe.tm_mon  = u.tm_mon;
    probe.tm_mday = 1;
    mktime(&probe);                              // populates tm_wday for the 1st
    int first_sun     = 1 + ((7 - probe.tm_wday) % 7);
    int cutover_day   = (month == 3) ? first_sun + 7 : first_sun;
    int cutover_utc_h = (month == 3) ? 7 : 6;   // 2am EST=7 UTC, 2am EDT=6 UTC
    if (u.tm_mday < cutover_day) return (month == 11);
    if (u.tm_mday > cutover_day) return (month == 3);
    return (month == 3) ? (u.tm_hour >= cutover_utc_h)
                        : (u.tm_hour <  cutover_utc_h);
}

// Fill *out with the US Eastern wall clock for UTC time t.
static void et_localtime(time_t t, struct tm* out) {
    time_t et = t - (time_t)(is_us_dst(t) ? 4 : 5) * 3600;
    gmtime_r(&et, out);
}

// Convert a struct tm treated as a wall-clock ET time to a UTC time_t.
// (ESP32 Arduino core has no timegm/mkgmtime, so we compute it directly.)
static time_t et_to_utc(const struct tm* et_tm, bool dst) {
    // Accumulate days since 1970-01-01 UTC.
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = et_tm->tm_year + 1900;
    int days = (y - 1970) * 365 + (y - 1969) / 4
             - (y - 1901) / 100 + (y - 1601) / 400
             + mdays[et_tm->tm_mon] + (et_tm->tm_mday - 1);
    bool leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    if (leap && et_tm->tm_mon >= 2) days++;
    time_t utc_naive = (time_t)days * 86400
                     + et_tm->tm_hour * 3600 + et_tm->tm_min * 60 + et_tm->tm_sec;
    return utc_naive + (time_t)(dst ? 4 : 5) * 3600;
}

// ── After-hours detection ─────────────────────────────────────────────────────
static bool is_after_hours(int idx) {
    if (idx < 0 || idx >= cfg.asset_count) return false;

    const AssetDef* def = asset_find(cfg.assets[idx]);
    if (!def) return false;
    if (def->market == MARKET_CRYPTO || def->market == MARKET_COMMODITY) return false;  // 24/7 markets

    // Compute US Eastern time (NYSE is always ET regardless of user's tz_offset).
    int wday = -1, mins = -1;
    if (ntp_synced) {
        time_t now_t = time(nullptr);
        struct tm et;
        et_localtime(now_t, &et);
        wday = et.tm_wday;
        mins = et.tm_hour * 60 + et.tm_min;

        // During regular NYSE hours (Mon–Fri 09:30–16:00 ET) always show the chart.
        // This overrides any incorrect "CLOSED" the YF API sometimes returns for
        // stocks/ETFs during an active session (a known Yahoo Finance quirk).
        if (wday >= 1 && wday <= 5 && mins >= 9*60+30 && mins < 16*60)
            return false;
    }

    AssetData* d = &asset_data[idx];

    // Outside regular hours: trust the API-reported state (handles pre/post-market,
    // holidays, and early closes that local time alone can't detect).
    if (d->valid) {
        return (d->market_state == MSTATE_CLOSED ||
                d->market_state == MSTATE_POST   ||
                d->market_state == MSTATE_PRE);
    }

    // No valid API data: fall back to ET time
    if (wday < 0) return false;             // no NTP — assume open
    if (wday == 0 || wday == 6)          return true;   // weekend in ET
    if (mins < 9*60+30 || mins >= 16*60) return true;   // outside ET session
    return false;
}

// ── Next market open countdown (seconds from now to 09:30 ET next weekday) ────
static uint32_t secs_to_market_open() {
    if (!ntp_synced) return 3600;  // fallback 1h

    time_t now = time(nullptr);
    struct tm et_now;
    et_localtime(now, &et_now);

    // Target: 9:30 AM ET on today's ET date.
    struct tm target = et_now;
    target.tm_hour = 9;
    target.tm_min  = 30;
    target.tm_sec  = 0;
    time_t t_open  = et_to_utc(&target, is_us_dst(now));

    if (t_open <= now) {
        // Already past 9:30 ET today — move to the next calendar day in ET.
        target.tm_mday += 1;
        // Advance by one ET day (use tomorrow's UTC midnight to check DST).
        time_t tomorrow = t_open + 86400;
        t_open = et_to_utc(&target, is_us_dst(tomorrow));
    }

    // Skip weekends in ET.
    struct tm tgt_et;
    et_localtime(t_open, &tgt_et);
    while (tgt_et.tm_wday == 0 || tgt_et.tm_wday == 6) {
        t_open += 86400;
        et_localtime(t_open, &tgt_et);
    }

    int diff = (int)(t_open - now);
    return diff > 0 ? (uint32_t)diff : 0;
}

// ── Error display ─────────────────────────────────────────────────────────────
static void show_error(const char* msg) {
    Serial.printf("[ERROR] %s\n", msg);
    if (!LV_LOCK()) return;
    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xEF4444), LV_PART_MAIN);
    lv_label_set_text(lbl, msg);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
    LV_UNLOCK();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("DeskTicker booting...");

    // Init display
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate        = LVGL_ROTATION,
    };
    bsp_display_start_with_config(&disp_cfg);
    Serial.println("[setup] display init OK");
    bsp_display_backlight_on();
    Serial.println("[setup] backlight on");

    pinMode(RESET_BTN_GPIO, INPUT_PULLUP);

    // Load settings from NVS
    settings_load(&cfg);
    Serial.printf("[setup] settings loaded: wifi_ok=%d assets=%d tf=%d brightness=%d\n",
                  cfg.wifi_ok, cfg.asset_count, cfg.timeframe, cfg.brightness);
    bsp_display_brightness_set(cfg.brightness);

    // Kick off WiFi before the splash so the 3 s hold isn't dead time.
    if (cfg.wifi_ok) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
        Serial.println("[setup] WiFi.begin issued (background)");
    }

    show_splash();
    Serial.println("[setup] splash shown");

    if (!cfg.wifi_ok) {
        state = S_WIFI_SETUP;
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[setup] WiFi up during splash: %s\n",
                      WiFi.localIP().toString().c_str());
        state = S_NTP_SYNC;   // skip "Connecting to WiFi..." screen
    } else {
        state = S_CONNECTING;
    }
}

// ── Loop / State machine ──────────────────────────────────────────────────────
void loop() {
    // 3-second hold on BOOT button → wipe all settings and return to setup
    if (digitalRead(RESET_BTN_GPIO) == LOW) {
        if (btn_held_since == 0) btn_held_since = millis();
        else if (millis() - btn_held_since >= 3000) {
            Serial.println("[reset] button held 3 s — clearing settings and rebooting");
            settings_clear();
            delay(200);
            ESP.restart();
        }
    } else {
        btn_held_since = 0;
    }

    switch (state) {

    // ── First-time WiFi setup ─────────────────────────────────────────────
    case S_WIFI_SETUP:
        Serial.println("Entering WiFi setup mode");
        wifi_setup_run(&cfg);   // blocks until saved & restarts flag set
        Serial.println("Setup done, rebooting");
        ESP.restart();
        break;

    // ── Connect to saved WiFi ─────────────────────────────────────────────
    case S_CONNECTING:
        show_connecting("Connecting to WiFi...");
        if (wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, 15000)) {
            Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
            wifi_retries = 0;
            hide_connecting();
            state = S_NTP_SYNC;
        } else {
            wifi_retries++;
            Serial.printf("WiFi failed (attempt %d)\n", wifi_retries);
            if (wifi_retries >= WIFI_RETRY) {
                // Still show chart if we have cached data, else setup
                hide_connecting();
                bool have_data = asset_data[cur_idx].valid;
                if (have_data) {
                    state = is_after_hours(cur_idx) ? S_AFTER_HOURS : S_CHART;
                } else {
                    state = S_WIFI_SETUP;
                }
            }
        }
        break;

    // ── NTP time sync ─────────────────────────────────────────────────────
    case S_NTP_SYNC:
        show_connecting("Syncing time...");
        {
            char tz_buf[24];
            make_tz_string(cfg.tz_offset, tz_buf, sizeof(tz_buf));
            configTzTime(tz_buf, NTP_SERVER);
        }
        {
            struct tm t;
            unsigned long deadline = millis() + 8000;
            while (!getLocalTime(&t, 100) && millis() < deadline) delay(100);
            ntp_synced = getLocalTime(&t, 100);
        }
        Serial.printf("NTP %s\n", ntp_synced ? "OK" : "failed (using device time)");
        hide_connecting();
        state = S_FETCH;
        break;

    // ── Fetch market data ─────────────────────────────────────────────────
    case S_FETCH: {
        if (!wifi_is_connected()) {
            state = S_RECONNECT;
            break;
        }
        bool bg_refresh = chart_created;
        if (!bg_refresh) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Fetching %s...", cfg.assets[cur_idx]);
            show_connecting(msg);
        } else {
            // Chart stays visible — show a loading hint in the header status label.
            // Exactly ONE locked write here; the clear is folded into the final
            // chart_screen_update() lock below (no standalone clear = no extra flush
            // in the vendor TE-sync race window that previously froze the device).
            char stsmsg[28];
            snprintf(stsmsg, sizeof(stsmsg), "Fetching %s...", cfg.assets[cur_idx]);
            if (LV_LOCK()) { chart_screen_set_status(stsmsg); LV_UNLOCK(); }
        }

        bool ok = api_fetch(&cfg, cur_idx, &asset_data[cur_idx]);
        Serial.printf("[fetch] %s: %s\n", cfg.assets[cur_idx],
                      ok ? "OK" : asset_data[cur_idx].err);
        // Always call hide_connecting — no-op when conn_scr is null, but correctly
        // queues conn_scr for deletion if WiFi reconnect had displaced the chart.
        hide_connecting();

        if (!ok) {
            // When the chart is already visible (bg_refresh), show the error in the
            // header status label — it is cleared automatically on the next successful
            // fetch. Avoid creating a floating label on chart_scr (no cleanup path).
            if (bg_refresh) {
                if (LV_LOCK()) { chart_screen_set_status(asset_data[cur_idx].err); LV_UNLOCK(); }
            } else {
                show_error(asset_data[cur_idx].err);
            }
            if (strncmp(asset_data[cur_idx].err, "YF HTTP -1", 10) == 0) {
                // Both primary and fallback failed at TCP level — WiFi stack is stale
                delay(1500);
                state = S_RECONNECT;
                break;
            }
            delay(3000);
            // A failed fetch gives no information about market state. Never trigger
            // S_AFTER_HOURS from a fetch failure — if market_state were stale or
            // the tz fallback wrong, we'd incorrectly show the animation (e.g. QQQ
            // during market hours). Stay on the chart if one exists, else retry.
            last_fetch_ms = millis();
            last_cycle_ms = millis();
            state = chart_created ? S_CHART : S_FETCH;
            break;
        }

        last_fetch_ms = millis();
        last_cycle_ms = millis();

        bool ah = is_after_hours(cur_idx);

        if (ah) {
            // After-hours: stop any running animation, start the new one, then
            // tear down the chart screen AFTER the anim screen is active.
            // Order matters: chart_screen_destroy() calls lv_obj_del(chart_scr).
            // If chart_scr is the active screen when deleted, LVGL leaves act_scr
            // dangling -> subsequent lv_scr_load in anim_start dereferences it
            // -> LoadProhibited crash. Loading the anim screen first makes
            // chart_scr inactive, so the destroy is safe.
            if (anim_running) {
                if (LV_LOCK()) { anim_stop(); LV_UNLOCK(); }
                anim_running = false;
            }
            uint32_t cd = secs_to_market_open();
            if (LV_LOCK()) {
                lv_obj_t* act_before = lv_scr_act();
                anim_set_candle_colors(cfg.bull_rgb, cfg.bear_rgb);
                anim_start(cfg.after_anim, cd);   // loads anim/countdown screen -> now active
                // Destroy the chart only once it is no longer the active screen.
                // Guard against the rare case where anim_start failed to load
                // (e.g. PSRAM exhausted) so we never delete the active screen.
                if (chart_created && lv_scr_act() != act_before) {
                    chart_screen_destroy();
                    chart_created = false;
                }
                cleanup_pending_scr();  // free any queued conn_scr
                LV_UNLOCK();
            }
            anim_running = true;
            state = S_AFTER_HOURS;
        } else {
            // Market open: stop animation if running, show chart.
            if (anim_running) {
                if (LV_LOCK()) { anim_stop(); LV_UNLOCK(); }
                anim_running = false;
            }
            if (!chart_created) {
                if (LV_LOCK()) {
                    chart_screen_create(&cfg);
                    cleanup_pending_scr();  // chart screen now active; safe to free conn_scr
                    LV_UNLOCK();
                }
                chart_created = true;
            } else if (chart_displaced) {
                // Chart already built but conn_scr or anim_scr covered it.
                // Bring the chart back and free the covering screen.
                if (LV_LOCK()) {
                    chart_screen_show();
                    cleanup_pending_scr();  // chart screen now active; safe to free conn_scr
                    LV_UNLOCK();
                }
                chart_displaced = false;
            }
            // When chart_displaced is false (plain background refresh), the chart
            // screen was never replaced — skip lv_scr_load to avoid the flash.
            if (LV_LOCK()) {
                chart_screen_set_status("");  // clear "Fetching…" hint — same lock, no extra flush
                chart_screen_update(&asset_data[cur_idx], &cfg, wifi_is_connected());
                LV_UNLOCK();
            }
            state = S_CHART;
        }
        break;
    }

    // ── Chart display loop ────────────────────────────────────────────────
    case S_CHART: {
        if (!wifi_is_connected()) {
            state = S_RECONNECT;
            break;
        }

        // Update WiFi/price in header every second
        if (LV_LOCK()) {
            chart_screen_update(&asset_data[cur_idx], &cfg, true);
            LV_UNLOCK();
        }

        // Check if market closed → go after-hours
        if (is_after_hours(cur_idx)) {
            state = S_FETCH;
            break;
        }

        // Vertical swipe: cycle through enabled timeframes
        int swipe_tf = chart_screen_get_swipe_vert();
        if (swipe_tf != 0 && cfg.timeframe_count > 1) {
            int ci = 0;
            for (int i = 0; i < cfg.timeframe_count; i++) {
                if (cfg.timeframes[i] == cfg.timeframe) { ci = i; break; }
            }
            ci = (ci + swipe_tf + cfg.timeframe_count) % cfg.timeframe_count;
            cfg.timeframe = cfg.timeframes[ci];
            last_fetch_ms = 0;  // force immediate re-fetch with new timeframe
            state = S_FETCH;
            break;
        }

        // Horizontal swipe: cycle assets
        int swipe = chart_screen_get_swipe();
        if (swipe != 0 && cfg.asset_count > 1) {
            cur_idx = (cur_idx + swipe + cfg.asset_count) % cfg.asset_count;
            last_cycle_ms = millis();
            state = S_FETCH;
            break;
        }

        // 3-second hold: open on-device settings menu
        if (chart_screen_get_settings_req()) {
            state = S_SETTINGS;
            break;
        }

        // Auto cycle (disabled when cycle_secs == 0)
        bool do_cycle = (cfg.cycle_secs > 0) && (cfg.asset_count > 1) &&
                        ((millis() - last_cycle_ms) >= (unsigned long)cfg.cycle_secs * 1000UL);
        if (do_cycle) {
            cur_idx = (cur_idx + 1) % cfg.asset_count;
            state   = S_FETCH;
            break;
        }

        // Refresh data at timeframe interval
        unsigned long refresh = api_refresh_interval(cfg.timeframe);
        if ((millis() - last_fetch_ms) >= refresh) {
            state = S_FETCH;
            break;
        }

        delay(1000);
        break;
    }

    // ── After-hours animation ─────────────────────────────────────────────
    case S_AFTER_HOURS: {
        if (!wifi_is_connected()) {
            state = S_RECONNECT;
            break;
        }

        // Update countdown every second
        static unsigned long last_cd_ms = 0;
        if (cfg.after_anim == ANIM_COUNTDOWN && (millis() - last_cd_ms) >= 1000) {
            last_cd_ms = millis();
            uint32_t cd = secs_to_market_open();
            if (LV_LOCK()) {
                anim_set_countdown(cd);
                LV_UNLOCK();
            }
        }

        // Check market re-open every 5 min
        if ((millis() - last_fetch_ms) >= 300000UL) {
            state = S_FETCH;
            break;
        }

        // Manual swipe
        int swipe_ah = anim_get_swipe();
        if (swipe_ah != 0 && cfg.asset_count > 1) {
            cur_idx = (cur_idx + swipe_ah + cfg.asset_count) % cfg.asset_count;
            last_cycle_ms = millis();
            state = S_FETCH;
            break;
        }

        // 3-second hold: open on-device settings menu
        if (anim_get_settings_req()) {
            state = S_SETTINGS;
            break;
        }

        // Auto cycle (disabled when cycle_secs == 0)
        bool do_cycle = (cfg.cycle_secs > 0) && (cfg.asset_count > 1) &&
                        ((millis() - last_cycle_ms) >= (unsigned long)cfg.cycle_secs * 1000UL);
        if (do_cycle) {
            cur_idx = (cur_idx + 1) % cfg.asset_count;
            state   = S_FETCH;
            break;
        }

        delay(100);
        break;
    }

    // ── WiFi lost, try to reconnect ───────────────────────────────────────
    case S_RECONNECT: {
        Serial.println("WiFi lost, reconnecting...");
        // If the chart is already on screen, attempt a quick silent reconnect
        // without displacing it — just show a small header note. Only fall back
        // to the full "Connecting…" screen if the soft window is exhausted.
        bool keep_visible = chart_created && !chart_displaced;
        if (keep_visible) {
            if (LV_LOCK()) { chart_screen_set_status("Reconnecting..."); LV_UNLOCK(); }
            for (int i = 0; i < RECONNECT_SOFT_TRIES; i++) {
                if (wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, RECONNECT_SOFT_MS)) {
                    Serial.println("WiFi reconnected (chart kept visible)");
                    wifi_retries  = 0;
                    last_fetch_ms = 0;  // force immediate refresh
                    // System time survives a brief drop — skip NTP screen if synced.
                    state = ntp_synced ? S_FETCH : S_NTP_SYNC;
                    break;
                }
            }
            if (state == S_RECONNECT) {  // soft window exhausted — fall back
                wifi_retries = 0;
                state = S_CONNECTING;
            }
        } else {
            wifi_retries = 0;
            state = S_CONNECTING;
            delay(2000);
        }
        break;
    }

    // ── On-device settings menu ───────────────────────────────────────────
    case S_SETTINGS: {
        static bool   settings_entered = false;
        static Settings settings_work;

        if (!settings_entered) {
            settings_work = cfg;
            if (LV_LOCK()) {
                // Load the settings screen on top of whatever is active.
                // The displaced screen (chart or anim) stays in memory so
                // Cancel can restore it without a full rebuild.
                settings_screen_create(&settings_work);
                LV_UNLOCK();
            }
            settings_entered = true;
        }

        int r = settings_screen_poll();

        if (r == 1) {
            // User pressed Save — retrieve edited settings, write NVS, restart.
            // settings_screen_create showed a "Restarting…" overlay before
            // setting the result, so the user has a visual cue already.
            settings_screen_get(&settings_work);
            settings_save(&settings_work);
            delay(1500);   // let the on-screen message render
            ESP.restart();

        } else if (r == -1) {
            // User pressed Cancel — restore the screen that was active before
            // settings opened, and discard all edits (NVS is untouched).
            if (LV_LOCK()) {
                lv_obj_t* ss = settings_screen_detach();
                queue_scr_for_delete(ss);
                if (chart_created) {
                    // Chart screen still exists in memory; bring it back and
                    // immediately free the queued settings screen.
                    chart_screen_show();
                    cleanup_pending_scr();
                }
                // If chart_created == false (came from S_AFTER_HOURS), the
                // settings screen stays queued and will be freed by
                // cleanup_pending_scr() inside show_connecting() in S_FETCH.
                LV_UNLOCK();
            }
            settings_entered = false;
            last_fetch_ms   = 0;
            chart_displaced = false;
            state = S_FETCH;
        } else {
            delay(30);   // yield to the LVGL task while user navigates
        }
        break;
    }

    default:
        state = S_CONNECTING;
        break;
    }
}
