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
#include "sdlog.h"           // SD-card mirror of the serial log (overnight testing)
#include "usb_msc.h"         // "Share SD over USB" — expose the SD card to a PC as a USB drive

// ── Config ────────────────────────────────────────────────────────────────────
#define LVGL_ROTATION     LV_DISP_ROT_90
#define WIFI_RETRY             3
#define RECONNECT_SOFT_TRIES   3     // quick reconnect attempts before showing full screen
#define RECONNECT_SOFT_MS   5000     // per-attempt timeout ms (~15 s total soft window)
#define NTP_SERVER        "pool.ntp.org"
#define RESET_BTN_GPIO    0   // BOOT button on ESP32-S3 — hold 3 s to re-run setup
// Forex/futures use local session math, but local time can't know about bank holidays.
// On a holiday the market's last trade (regularMarketTime) goes stale. If the local
// session says "open" but the last trade is older than this, treat it as closed
// (holiday) → show the after-hours animation. 2 h is well above the ~15 min normal
// staleness and the 1 h futures maintenance halt, and far below a ≥24 h holiday gap.
#define HOLIDAY_STALE_SECS  (2 * 3600)

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
    S_SETTINGS,
    S_USB_SHARE        // SD card shared with PC over USB (ticker paused; reboot to exit)
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

// Persist last-viewed asset and TF across watchdog reboots so the device resumes
// the same view instead of starting from asset 0 (= BTC) / NVS-saved TF. MUST be
// RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the latter is reloaded from its initializer
// by the bootloader on every esp_restart(), so it would be wiped and the resume
// would never happen (the device would always fall back to asset 0). RTC_NOINIT_ATTR
// survives a software reset and is only garbage on a cold power-on — these are only
// read when render_wdt_consume_last_reboot() confirms a watchdog reboot (its magic
// guard rejects power-on garbage), and asset_idx/tf are range-checked before use.
RTC_NOINIT_ATTR static int  rtc_resume_asset_idx = 0;
RTC_NOINIT_ATTR static int  rtc_resume_tf         = 0;
RTC_NOINIT_ATTR static bool rtc_resume_valid       = false;

// Set just before a *deliberate* restart that should still resume the last view —
// ending "Share SD over USB" mode, or "Save & Restart" from the settings menu. Those
// are clean esp_restart()s, NOT watchdog reboots, so was_wdt_reboot is false and the
// resume block above would otherwise be skipped — landing on asset 0 instead of the
// ticker the user was viewing. This magic (checked + cleared once on boot) tells the
// boot path to honour rtc_resume_* for that one intentional reboot. Its own magic guard
// rejects cold-power-on garbage, exactly like render_wdt_consume_last_reboot().
#define RESUME_MAGIC 0x05B5E51DUL
RTC_NOINIT_ATTR static uint32_t rtc_resume_magic = 0;

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
    // Paint the connecting screen synchronously, while we hold the lock, so it is on
    // the display before the caller suspends DMA (lvgl_flush_suspended) for the HTTP
    // fetch. A plain set + unlock relies on the async render task (Core 1), which never
    // gets to flush before suspension — that is why "Fetching..." vanished when swiping
    // from the after-hours animation. WiFi is idle here, so this flush is race-free.
    lv_refr_now(NULL);
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

// ── USB "Share SD over USB" screen ────────────────────────────────────────────
// Full-screen notice shown while the micro-SD card is exposed to a PC as a USB
// drive. A single tap anywhere requests exit (the main loop then reboots, which
// hands the SD card cleanly back to the logger).
static lv_obj_t*     usb_scr      = nullptr;
static volatile bool usb_exit_req = false;

static void usb_scr_tap_cb(lv_event_t*) { usb_exit_req = true; }

// ok == true  → card mounted and shared; ok == false → no card found / share failed.
static void show_usb_share_screen(bool ok) {
    if (!LV_LOCK()) return;
    usb_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(usb_scr, lv_color_hex(0x0E1117), LV_PART_MAIN);
    lv_obj_clear_flag(usb_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(usb_scr);
    lv_obj_set_style_text_font(title,  &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, ok ? lv_color_hex(0x22C55E)
                                          : lv_color_hex(0xF59E0B), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(title, ok ? LV_SYMBOL_USB "  SD Shared with PC"
                                : LV_SYMBOL_WARNING "  No SD Card");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -56);

    lv_obj_t* body = lv_label_create(usb_scr);
    lv_obj_set_style_text_font(body,  &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, SCR_W - 48);
    if (ok) {
        lv_label_set_text(body,
            "The micro-SD card is now a USB drive on your computer.\n\n"
            "When finished, use \"Safely Remove / Eject\" on the PC,\n"
            "then tap the screen to resume the ticker (device restarts).");
    } else {
        lv_label_set_text(body,
            "No card was detected, so nothing was shared.\n\n"
            "Tap the screen to return (device restarts).");
    }
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 28);

    // Labels are non-clickable by default, so taps fall through to the screen.
    lv_obj_add_flag(usb_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(usb_scr, usb_scr_tap_cb, LV_EVENT_CLICKED, NULL);

    if (chart_created) chart_displaced = true;
    lv_scr_load(usb_scr);
    cleanup_pending_scr();   // free any screen queued before we got here (e.g. settings)
    lv_refr_now(NULL);       // paint synchronously while WiFi is idle (race-free)
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

// ── Per-asset-class local session helpers ─────────────────────────────────────
// All helpers take US Eastern wall-clock wday (0=Sun..6=Sat) and mins (hour*60+min).
// Used by is_after_hours() to apply the correct session floor for each class,
// independently of Yahoo's marketState (which is unreliable for FX/futures).

// FX (~24/5): live Sun 17:00 ET → Fri 17:00 ET.
// Closed: Saturday all day, Sunday before 17:00, Friday at/after 17:00.
static bool fx_session_open(int wday, int mins) {
    if (wday == 6) return false;                // Saturday — always closed
    if (wday == 0) return (mins >= 17 * 60);    // Sunday — opens at 17:00 ET
    if (wday == 5) return (mins < 17 * 60);     // Friday — closes at 17:00 ET
    return true;                                // Mon–Thu always in session
}

// CME Globex index futures (~23h/weekday): Sun 18:00 ET → Fri 17:00 ET,
// with a daily maintenance halt Mon–Thu 17:00–18:00 ET.
// Closed: Saturday, Sunday before 18:00, Friday at/after 17:00, and the daily halt.
static bool futures_session_open(int wday, int mins) {
    if (wday == 6) return false;                // Saturday — always closed
    if (wday == 0) return (mins >= 18 * 60);    // Sunday — opens at 18:00 ET
    if (wday == 5) return (mins < 17 * 60);     // Friday — closes at 17:00 ET
    // Mon–Thu: open except the 17:00–18:00 daily maintenance halt
    return !(mins >= 17 * 60 && mins < 18 * 60);
}

// Returns true if Yahoo's last-trade time for this asset is stale enough to mean the
// market isn't actually trading right now (i.e. a bank holiday). Used for forex/futures,
// whose local session math can't know about holidays. Safe only when d->valid.
static bool last_trade_stale(const AssetData* d) {
    if (!d->valid || d->reg_mkt_time == 0) return false;  // no data → can't tell
    uint32_t now_s = (uint32_t)time(nullptr);
    return (now_s > d->reg_mkt_time) &&
           (now_s - d->reg_mkt_time > HOLIDAY_STALE_SECS);
}

// ── After-hours detection ─────────────────────────────────────────────────────
// Returns true when the chart should be replaced by the after-hours animation.
//
// Yahoo's v8/finance/chart API no longer sends `marketState`, so detection is:
//   Crypto / Commodity : always live (24/7) — short-circuit.
//   Forex              : local FX session (Sun 17:00 → Fri 17:00 ET). Stays live
//                        through US bank holidays (FX still trades globally); a stale
//                        last-trade time means FX itself is closed → animation.
//   Futures            : local Globex session (Sun 18:00 → Fri 17:00 ET, minus the
//                        daily 17:00–18:00 ET halt). A stale last-trade time during
//                        what the schedule thinks is an open session = bank holiday
//                        → animation.
//   Stocks / ETFs &    : Yahoo's currentTradingPeriod.regular window (epoch). It is
//   any foreign ticker   precise and per-exchange (NYSE, Bursa, LSE, TSE, …) and
//                        calendar-aware, so holidays/early-closes are handled
//                        automatically. Failsafe to NYSE 09:30–16:00 ET when the
//                        window is absent/degenerate or there's no fresh data.
//   Unknown ticker     : assume live (safe fallback).
//
// The user's tz_offset is never used here — every session is anchored to its own
// exchange (ET helpers for FX/futures, Yahoo epoch windows for equities).
// This function is intentionally side-effect free (no LVGL calls).
static bool is_after_hours(int idx) {
    if (idx < 0 || idx >= cfg.asset_count) return false;

    const AssetDef* def = asset_find(cfg.assets[idx]);
    if (!def) return false;

    // 24/7 markets — never show the after-hours animation.
    if (def->market == MARKET_CRYPTO || def->market == MARKET_COMMODITY) return false;

    AssetData* d = &asset_data[idx];

    // Compute US Eastern wall-clock time for local session math.
    // NYSE/CME/FX sessions are all anchored to ET, independent of the user's tz_offset.
    int wday = -1, mins = -1;
    if (ntp_synced) {
        time_t now_t = time(nullptr);
        struct tm et;
        et_localtime(now_t, &et);
        wday = et.tm_wday;
        mins = et.tm_hour * 60 + et.tm_min;
    }

    // ── Forex ─────────────────────────────────────────────────────────────────
    // Local FX session (Yahoo's window is too coarse for CCY pairs). A stale
    // last-trade time means FX itself is closed (e.g. a global holiday).
    if (def->market == MARKET_FOREX) {
        if (wday < 0) return false;       // no NTP — assume open (safe failsafe)
        return !fx_session_open(wday, mins) || last_trade_stale(d);
    }

    // ── Futures (MARKET_STOCK + continuous=1, e.g. ES=F, NQ=F) ───────────────
    // Local Globex session + stale-trade holiday detection (Yahoo's window for
    // futures is a coarse 00:00–23:59 that can't express the real schedule).
    if (def->market == MARKET_STOCK && def->continuous) {
        if (wday < 0) return false;
        return !futures_session_open(wday, mins) || last_trade_stale(d);
    }

    // ── Regular stocks / ETFs (MARKET_STOCK + continuous=0) & foreign tickers ──
    // Trust Yahoo's per-exchange, calendar-aware regular trading window when present.
    // live  ⇔  reg_start ≤ now < reg_end. This auto-adapts to NYSE/Bursa/LSE/TSE and
    // handles holidays/early-closes with no hardcoding and no dependence on tz_offset.
    if (d->valid && d->reg_end > d->reg_start) {
        uint32_t now_s = (uint32_t)time(nullptr);
        return !(now_s >= d->reg_start && now_s < d->reg_end);
    }

    // Failsafe (no fresh data, or window absent/degenerate): pure NYSE ET schedule.
    if (wday < 0) return false;                       // no NTP — assume open
    if (wday == 0 || wday == 6)          return true; // weekend in ET
    if (mins < 9*60+30 || mins >= 16*60) return true; // outside ET session
    return false;
}

// ── Next session open countdown (seconds from now to the next open for this asset) ──
// Targets the correct opening time per asset class:
//   Stocks / ETFs : next weekday 09:30 ET (NYSE open).
//   Forex         : next Sunday 17:00 ET (FX session reopens after weekend).
//   Futures       : next valid open — today 18:00 ET if in the daily halt
//                   (Mon–Thu 17:00–18:00 ET), otherwise next Sunday 18:00 ET
//                   (Globex reopens after the weekend gap).
//   Crypto/Commod : never called (always live), but returns 0 defensively.
static uint32_t secs_to_market_open(int idx) {
    if (!ntp_synced) return 3600;  // fallback 1h when NTP not yet synced

    // Determine asset class so we target the right open time.
    bool is_fx  = false;
    bool is_fut = false;
    if (idx >= 0 && idx < cfg.asset_count) {
        const AssetDef* def = asset_find(cfg.assets[idx]);
        if (def) {
            is_fx  = (def->market == MARKET_FOREX);
            is_fut = (def->market == MARKET_STOCK && def->continuous);
        }
    }

    time_t now = time(nullptr);
    struct tm et_now;
    et_localtime(now, &et_now);

    // Choose the target open hour/minute in ET for this class.
    int open_h, open_m;
    if (is_fx)       { open_h = 17; open_m = 0; }   // FX reopens Sun 17:00 ET
    else if (is_fut) { open_h = 18; open_m = 0; }   // Globex reopens Sun/daily 18:00 ET
    else             { open_h = 9;  open_m = 30; }  // NYSE opens weekdays 09:30 ET

    // Build candidate: today's ET date at the open time.
    struct tm target = et_now;
    target.tm_hour = open_h;
    target.tm_min  = open_m;
    target.tm_sec  = 0;
    time_t t_open  = et_to_utc(&target, is_us_dst(now));

    // If candidate is already in the past, advance one day.
    if (t_open <= now) {
        t_open += 86400;
    }

    // Skip days on which this market does not open.
    // FX    : opens only on Sunday (wday=0). Skip all other days.
    // Futures: opens Sunday (weekend gap) and Mon–Thu (after daily halt).
    //          Does NOT reopen on Friday evenings or Saturdays — skip those.
    // Stocks : opens weekdays (Mon–Fri). Skip weekends.
    for (int guard = 0; guard < 8; guard++) {
        struct tm tgt_et;
        et_localtime(t_open, &tgt_et);
        bool day_ok;
        if (is_fx) {
            day_ok = (tgt_et.tm_wday == 0);           // Sunday only
        } else if (is_fut) {
            // Valid: Sunday and Mon–Thu. Not valid: Friday (wday=5) or Saturday (wday=6).
            day_ok = (tgt_et.tm_wday != 5 && tgt_et.tm_wday != 6);
        } else {
            day_ok = (tgt_et.tm_wday >= 1 && tgt_et.tm_wday <= 5);  // Mon–Fri
        }
        if (day_ok) break;
        t_open += 86400;
    }

    int diff = (int)(t_open - now);
    return diff > 0 ? (uint32_t)diff : 0;
}

// ── Error display ─────────────────────────────────────────────────────────────
static void show_error(const char* msg) {
    sdlog_printf("[ERROR] %s\n", msg);
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
    // Mount the SD card first so the boot banner and reset reason are captured on the
    // card for unattended/overnight testing. Falls back to Serial-only if no card.
    sdlog_init();
    sdlog_println("DeskTicker booting...");
    // Record WHY the device booted (POWERON / SW=our watchdog / PANIC / BROWNOUT / ...).
    // A wake on asset[0] after-hours with reset_reason != SW means a non-watchdog reset
    // wiped the RTC resume state — this line tells us which.
    sdlog_printf("[boot] reset_reason=%s\n", reset_reason_str());

    // Register the composite USB device (CDC serial + Mass-Storage disk) now that the
    // SD card geometry is known. The disk starts with NO media — the card stays owned by
    // the logger until the user picks "Share SD over USB" in settings. No-op (with a
    // serial note) unless Tools > USB Mode = "USB-OTG (TinyUSB)".
    usb_msc_init();

    // Init display
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate        = LVGL_ROTATION,
    };
    bsp_display_start_with_config(&disp_cfg);
    sdlog_println("[setup] display init OK");
    bsp_display_backlight_on();
    sdlog_println("[setup] backlight on");

    // Arm the always-on render-task liveness watchdog now that the display (and its
    // render task) is up. It protects every screen — including the live chart, which
    // previously had no watchdog at all. Create its feed lv_timer under the LVGL lock.
    if (LV_LOCK()) { render_wdt_init(); LV_UNLOCK(); }
    // If the previous boot was a watchdog reboot, report it so a chart hang is visible
    // and its timing measurable on the serial monitor.
    WdtReboot wr;
    bool was_wdt_reboot = render_wdt_consume_last_reboot(&wr);
    if (was_wdt_reboot) {
        // phase: 0=idle  2=TE wait  3=DMA-done wait  4=tx_color pixel DMA (freezeTest2)
        //        5=flush done  6=mutex wait  7=tx_param CASET cmd (new sub-phase)
        sdlog_printf("[WDT] previous boot ended in a render-watchdog reboot: "
                      "state=%u phase=%u chunk=%u freeHeap=%u freePSRAM=%u atEpoch=%lu\n",
                      wr.last_state, wr.phase, wr.chunk,
                      wr.free_heap, wr.free_psram,
                      (unsigned long)wr.reboot_epoch);
    }

    pinMode(RESET_BTN_GPIO, INPUT_PULLUP);

    // Load settings from NVS
    settings_load(&cfg);
    sdlog_printf("[setup] settings loaded: wifi_ok=%d assets=%d tf=%d brightness=%d\n",
                  cfg.wifi_ok, cfg.asset_count, cfg.timeframe, cfg.brightness);
    bsp_display_brightness_set(cfg.brightness);

    // After settings are loaded: restore the last-viewed asset and timeframe so the
    // reboot is invisible to the user. This fires for two kinds of reboot:
    //   1. a watchdog reboot (was_wdt_reboot), and
    //   2. a deliberate restart that asked to resume — ending "Share SD over USB" mode
    //      or "Save & Restart" from settings (RESUME_MAGIC flag below).
    // Consume the magic exactly once so a later cold power-on starts at asset 0.
    bool deliberate_resume = (rtc_resume_magic == RESUME_MAGIC);
    rtc_resume_magic = 0;
    if ((was_wdt_reboot || deliberate_resume) && rtc_resume_valid) {
        if (rtc_resume_asset_idx < cfg.asset_count) {
            cur_idx = rtc_resume_asset_idx;
        }
        for (int i = 0; i < cfg.timeframe_count; i++) {
            if (cfg.timeframes[i] == rtc_resume_tf) {
                cfg.timeframe = rtc_resume_tf;
                break;
            }
        }
        sdlog_printf("[%s] resuming last view: asset_idx=%d tf=%d\n",
                      deliberate_resume ? "resume" : "WDT", cur_idx, cfg.timeframe);
    }

    // Kick off WiFi before the splash so the 3 s hold isn't dead time.
    if (cfg.wifi_ok) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
        sdlog_println("[setup] WiFi.begin issued (background)");
    }

    show_splash();
    sdlog_println("[setup] splash shown");

    if (!cfg.wifi_ok) {
        state = S_WIFI_SETUP;
    } else if (WiFi.status() == WL_CONNECTED) {
        sdlog_printf("[setup] WiFi up during splash: %s\n",
                      WiFi.localIP().toString().c_str());
        state = S_NTP_SYNC;   // skip "Connecting to WiFi..." screen
    } else {
        state = S_CONNECTING;
    }
}

// ── Loop / State machine ──────────────────────────────────────────────────────
void loop() {
    // Flush any queued log lines to the SD card. No-op during a fetch (lvgl_flush_suspended)
    // so SD I/O never adds bus pressure in the WiFi/QSPI-DMA-sensitive window.
    sdlog_flush();

    // 3-second hold on BOOT button → wipe all settings and return to setup
    if (digitalRead(RESET_BTN_GPIO) == LOW) {
        if (btn_held_since == 0) btn_held_since = millis();
        else if (millis() - btn_held_since >= 3000) {
            sdlog_println("[reset] button held 3 s — clearing settings and rebooting");
            sdlog_flush_blocking();
            settings_clear();
            delay(200);
            ESP.restart();
        }
    } else {
        btn_held_since = 0;
    }

    // Tell the render watchdog which state we're in (logged if it has to reboot), and
    // emit a periodic health line so a hang or a slow memory leak is visible on serial.
    render_wdt_set_context((uint8_t)state);
    static unsigned long last_health_ms = 0;
    static uint32_t      last_hb        = 0;
    if (millis() - last_health_ms >= 60000) {
        last_health_ms = millis();
        uint32_t hb = render_wdt_heartbeat();
        // phase: 0=idle  2=TE wait  3=DMA-done wait  4=tx_color pixel DMA  5=done
        //        6=mutex wait  7=tx_param CASET cmd
        sdlog_printf("[health] state=%d freeHeap=%u freePSRAM=%u largestPSRAM=%u "
                      "renderHB=%u (+%u/min) flushTO=%u teTO=%u lockTO=%u phase=%u chunk=%u\n",
                      (int)state, (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                      (unsigned)hb, (unsigned)(hb - last_hb),
                      (unsigned)lvgl_port_flush_timeouts,
                      (unsigned)bsp_te_sync_timeouts,
                      (unsigned)lvgl_lock_timeouts,
                      (unsigned)lvgl_render_phase,
                      (unsigned)lvgl_render_chunk);
        last_hb = hb;
    }

    switch (state) {

    // ── First-time WiFi setup ─────────────────────────────────────────────
    case S_WIFI_SETUP:
        sdlog_println("Entering WiFi setup mode");
        wifi_setup_run(&cfg);   // blocks until saved & restarts flag set
        sdlog_println("Setup done, rebooting");
        sdlog_flush_blocking();
        ESP.restart();
        break;

    // ── Connect to saved WiFi ─────────────────────────────────────────────
    case S_CONNECTING:
        show_connecting("Connecting to WiFi...");
        if (wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, 15000)) {
            sdlog_printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
            wifi_retries = 0;
            hide_connecting();
            state = S_NTP_SYNC;
        } else {
            wifi_retries++;
            sdlog_printf("WiFi failed (attempt %d)\n", wifi_retries);
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
        sdlog_printf("NTP %s\n", ntp_synced ? "OK" : "failed (using device time)");
        hide_connecting();
        state = S_FETCH;
        break;

    // ── Fetch market data ─────────────────────────────────────────────────
    case S_FETCH: {
        // Record the intended asset+TF so a watchdog reboot can resume here.
        rtc_resume_asset_idx = cur_idx;
        rtc_resume_tf        = cfg.timeframe;
        rtc_resume_valid     = true;

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
            // Chart stays visible — show a loading hint in the status label, then
            // synchronously paint it with lv_refr_now() while we still hold the lock.
            // The render task is blocked on the same recursive mutex, so we have
            // exclusive display access and the flush callback runs inline here. This
            // MUST happen before lvgl_flush_suspended freezes the display below — a plain
            // set + unlock is not enough, because the async render task may never paint
            // the label before suspension (the screen would jump straight from the old
            // frame to the new chart, and "Fetching..." would never appear). WiFi is idle
            // at this point, so this one flush cannot hit the WiFi/QSPI DMA race.
            char stsmsg[28];
            snprintf(stsmsg, sizeof(stsmsg), "Fetching %s...", cfg.assets[cur_idx]);
            if (LV_LOCK()) {
                chart_screen_set_status(stsmsg);
                lv_refr_now(NULL);
                LV_UNLOCK();
            }
        }

        // Suppress QSPI DMA during the HTTP fetch (prevents WiFi receive DMA from racing
        // with QSPI display DMA on the shared AHB bus). Unlike lvgl_port_stop(), this
        // flag leaves lv_timer_handler() running: the render watchdog feed timer keeps
        // firing every 1s → no false watchdog reboots on slow or retried fetches.
        lvgl_flush_suspended = true;
        bool ok = api_fetch(&cfg, cur_idx, &asset_data[cur_idx]);
        lvgl_flush_suspended = false;
        sdlog_printf("[fetch] %s: %s\n", cfg.assets[cur_idx],
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
            uint32_t cd = secs_to_market_open(cur_idx);  // uses per-class session open time
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
            uint32_t cd = secs_to_market_open(cur_idx);  // per-class open time
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
        sdlog_println("WiFi lost, reconnecting...");
        // If the chart is already on screen, attempt a quick silent reconnect
        // without displacing it — just show a small header note. Only fall back
        // to the full "Connecting…" screen if the soft window is exhausted.
        bool keep_visible = chart_created && !chart_displaced;
        if (keep_visible) {
            if (LV_LOCK()) { chart_screen_set_status("Reconnecting..."); LV_UNLOCK(); }
            for (int i = 0; i < RECONNECT_SOFT_TRIES; i++) {
                if (wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, RECONNECT_SOFT_MS)) {
                    sdlog_println("WiFi reconnected (chart kept visible)");
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
            // Resume the ticker the user was viewing before opening settings, instead of
            // restarting at asset 0. rtc_resume_* hold that view from the last S_FETCH; the
            // boot path range-checks the asset index and only restores the timeframe if it
            // is still enabled, so it stays sensible even if the saved settings changed it.
            rtc_resume_magic = RESUME_MAGIC;
            sdlog_flush_blocking();
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

        } else if (r == 2) {
            // User tapped "Share SD over USB" — tear down the settings screen and enter
            // USB drive mode. The settings screen is queued for deletion and freed by
            // cleanup_pending_scr() once the share screen loads.
            if (LV_LOCK()) {
                lv_obj_t* ss = settings_screen_detach();
                queue_scr_for_delete(ss);
                LV_UNLOCK();
            }
            settings_entered = false;
            state = S_USB_SHARE;

        } else {
            delay(30);   // yield to the LVGL task while user navigates
        }
        break;
    }

    // ── USB drive mode: SD card shared with a connected PC ────────────────────
    case S_USB_SHARE: {
        static bool usb_entered = false;
        if (!usb_entered) {
            // Hand the card to the PC. begin_share() releases it from the logger and
            // mounts it for raw USB access; false means no card was found.
            bool ok = usb_msc_begin_share();
            sdlog_printf("[usb] share %s\n", ok ? "started" : "failed (no card)");
            show_usb_share_screen(ok);
            usb_exit_req = false;
            usb_entered  = true;
        }
        // A screen tap ends the session. Reboot is the simplest safe way to return the
        // SD bus to the logger and rebuild the normal UI.
        if (usb_exit_req) {
            usb_msc_end_share();
            // Mark this as a deliberate restart so the boot path restores the ticker the
            // user was viewing before sharing (rtc_resume_* still hold it from S_FETCH),
            // instead of falling back to asset 0.
            rtc_resume_magic = RESUME_MAGIC;
            sdlog_println("[usb] share ended — restarting");
            delay(300);          // let the PC notice the media disappear
            ESP.restart();
        }
        delay(50);
        break;
    }

    default:
        state = S_CONNECTING;
        break;
    }
}
