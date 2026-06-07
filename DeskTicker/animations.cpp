#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "animations.h"
#include "chart_screen.h"  // SCR_W, SCR_H
#include "esp_timer.h"
#include <time.h>          // time() for the watchdog reboot timestamp
#include "lv_port.h"       // lvgl_render_phase / lvgl_render_chunk (phase locator)
#include "sdlog.h"         // mirror [WDT] reboot line to SD before esp_restart()

// ── Render-task liveness watchdog (always-on) ─────────────────────────────────
// Independent hardware timer (esp_timer, NOT an LVGL timer) that reboots the
// device if the LVGL render task stops producing frames for 5 seconds. Because it
// runs via the esp_timer task — not the LVGL render task — it fires even when LVGL
// is frozen in a QSPI DMA / tearing-effect semaphore wait (the silent display
// deadlock documented in BISECT_LOG.md).
//
// It is ALWAYS ON for the whole runtime: render_wdt_init() arms it once and starts
// a global feed lv_timer (render_feed_cb) that runs inside lv_timer_handler(), i.e.
// in the render task. So it protects EVERY screen — live chart, connecting, settings
// AND after-hours animation — not just the animation as before. If the render task
// deadlocks, lv_timer_handler() stops cycling, the feed stops, and the device
// reboots. Main-loop blocking (e.g. a 30 s HTTP fetch) never false-fires it, because
// the render task keeps feeding independently. The animation timer callbacks also
// call wdt_feed() — now harmless extra feeds of the same global watchdog.
static esp_timer_handle_t s_wdt        = nullptr;
static lv_timer_t*        s_feed_timer = nullptr;
static volatile uint32_t  s_heartbeat  = 0;     // ++ each render feed; flat => render stalled
static volatile uint8_t   s_last_state = 0xFF;  // last main-loop State, for crash logging

// Survives a software reset (esp_restart) so the NEXT boot can report that the
// previous boot ended in a render-watchdog reboot, and what the device was doing
// when it hung. MUST be RTC_NOINIT_ATTR, not RTC_DATA_ATTR: RTC_DATA_ATTR has an
// initializer that the bootloader reloads on every software reset, so it would be
// wiped back to zero by esp_restart() (the magic check below would then always
// fail and the resume-last-view logic would never run). RTC_NOINIT_ATTR is left
// untouched across a software/hardware reset and is only garbage on a cold
// power-on — which the WDT_MAGIC guard in render_wdt_consume_last_reboot() rejects.
#define WDT_MAGIC 0xDEADBE01u
RTC_NOINIT_ATTR static WdtReboot s_reboot_mark;

static void wdt_fire(void*) {
    // No render frame in 5 s — the LVGL render task is deadlocked. Record what the
    // device was doing so the NEXT boot can report it (RTC RAM survives esp_restart),
    // then reboot.
    s_reboot_mark.magic        = WDT_MAGIC;
    s_reboot_mark.last_state   = s_last_state;
    s_reboot_mark.free_heap    = (uint32_t)ESP.getFreeHeap();
    s_reboot_mark.free_psram   = (uint32_t)ESP.getFreePsram();
    s_reboot_mark.reboot_epoch = (uint32_t)time(nullptr);
    // Capture the render-phase locator (0=idle/cb, 2=TE sync, 3=DMA-done wait,
    // 4=esp_lcd draw — most likely stuck point, 5=flush done, 6=mutex wait).
    s_reboot_mark.phase        = lvgl_render_phase;
    s_reboot_mark.chunk        = lvgl_render_chunk;
    sdlog_printf("[WDT] render watchdog: no frame in 5s, rebooting "
                 "(state=%u phase=%u chunk=%u heap=%u psram=%u hb=%u)\n",
                 s_last_state, s_reboot_mark.phase, s_reboot_mark.chunk,
                 s_reboot_mark.free_heap, s_reboot_mark.free_psram, s_heartbeat);
    Serial.flush();
    // Persist the reboot-cause line to the SD card before we reset, so an unattended
    // overnight reboot leaves a record even though the next boot's RTC line also reports it.
    sdlog_flush_blocking();
    esp_restart();
}

static void wdt_start() {
    if (s_wdt) return;
    const esp_timer_create_args_t args = {
        .callback        = wdt_fire,
        .arg             = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "anim_wdt",
        .skip_unhandled_events = false
    };
    if (esp_timer_create(&args, &s_wdt) == ESP_OK)
        esp_timer_start_once(s_wdt, 5ULL * 1000 * 1000);  // 5 s in µs
}

static void wdt_feed() {
    if (!s_wdt) return;
    // Reset the 5-second countdown — we are alive and rendering.
    esp_timer_restart(s_wdt, 5ULL * 1000 * 1000);
}

// ── Swipe / gesture state ─────────────────────────────────────────────────────
static volatile int8_t anim_swipe        = 0;
static volatile int8_t anim_settings_req = 0;

// Triple-tap tracking: 3 clicks within 1.2 s opens Settings.
static int           anim_tap_count   = 0;
static unsigned long anim_last_tap_ms = 0;

static void anim_gesture_cb(lv_event_t*) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if      (dir == LV_DIR_LEFT)  anim_swipe =  1;
    else if (dir == LV_DIR_RIGHT) anim_swipe = -1;
    // A swipe cancels any in-progress tap sequence.
    anim_tap_count = 0;
}

// Counts clean taps; fires settings_req after 3 within 1.2 s.
static void anim_tap_cb(lv_event_t*) {
    unsigned long now = millis();
    if (now - anim_last_tap_ms > 1200UL) anim_tap_count = 0;
    anim_last_tap_ms = now;
    if (++anim_tap_count >= 3) {
        anim_tap_count    = 0;
        anim_settings_req = 1;
    }
}

int anim_get_swipe() {
    int d = (int)anim_swipe;
    anim_swipe = 0;
    return d;
}

int anim_get_settings_req() {
    int d = (int)anim_settings_req;
    anim_settings_req = 0;
    return d;
}

// ── Shared state ──────────────────────────────────────────────────────────────
static lv_obj_t*   anim_scr    = nullptr;
static lv_timer_t* anim_timer  = nullptr;
static lv_color_t* anim_buf    = nullptr;
static lv_obj_t*   anim_canvas = nullptr;
static int         cur_type    = -1;
static uint32_t    countdown   = 0;

// Pre-rendered background (water gradient / sky+sand). Rendered once at init
// with ordered dithering. anim_buf is primed with a full copy of anim_bg at
// startup; each frame restores only the small rectangles that sprites occupy
// (not the whole buffer) to erase last frame's ghosts. ~300 KB PSRAM, freed
// in anim_stop(). Not used for ANIM_STARFIELD (star_draw() is cheap enough).
static lv_color_t* anim_bg = nullptr;

// Candle theme colors for the crab's claws. Set by anim_set_candle_colors()
// before anim_start(). Defaults match the brand-kit Bull Green / Bear Red.
static uint32_t s_anim_bull = 0x22C55E;
static uint32_t s_anim_bear = 0xEF4444;

// ── Ordered-dither helper ─────────────────────────────────────────────────────
// RGB565 has only 32 levels per R/B channel, so smooth gradients band into
// hard horizontal stripes. An 8x8 Bayer matrix spreads the quantization error
// across pixels so the eye sees a smooth gradient instead.
static const uint8_t BAYER8[8][8] = {
    {  0, 48, 12, 60,  3, 51, 15, 63 },
    { 32, 16, 44, 28, 35, 19, 47, 31 },
    {  8, 56,  4, 52, 11, 59,  7, 55 },
    { 40, 24, 36, 20, 43, 27, 39, 23 },
    {  2, 50, 14, 62,  1, 49, 13, 61 },
    { 34, 18, 46, 30, 33, 17, 45, 29 },
    { 10, 58,  6, 54,  9, 57,  5, 53 },
    { 42, 26, 38, 22, 41, 25, 37, 21 }
};

// Writes one dithered pixel (float r,g,b in 0..255) directly into a buffer.
static inline void put_dith(lv_color_t* buf, int x, int y,
                            float r, float g, float b) {
    float d = (BAYER8[y & 7][x & 7] / 64.0f - 0.5f) * 8.0f;
    int R = (int)(r + d), G = (int)(g + d), B = (int)(b + d);
    if (R < 0) R = 0; else if (R > 255) R = 255;
    if (G < 0) G = 0; else if (G > 255) G = 255;
    if (B < 0) B = 0; else if (B > 255) B = 255;
    buf[y * SCR_W + x] = lv_color_make((uint8_t)R, (uint8_t)G, (uint8_t)B);
}

// Copies a rectangle from anim_bg back into anim_buf (erases a sprite ghost).
// Clamped to screen bounds; a fully off-screen rect is a silent no-op.
static void bg_restore(int x1, int y1, int x2, int y2) {
    if (!anim_bg || !anim_buf) return;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= SCR_W) x2 = SCR_W - 1; if (y2 >= SCR_H) y2 = SCR_H - 1;
    if (x1 > x2 || y1 > y2) return;
    int w = (x2 - x1 + 1) * (int)sizeof(lv_color_t);
    for (int y = y1; y <= y2; y++)
        memcpy(anim_buf + y * SCR_W + x1, anim_bg + y * SCR_W + x1, w);
}

// ══════════════════════════════════════════════════════════════════════════════
// STARFIELD
// ══════════════════════════════════════════════════════════════════════════════
#define STAR_COUNT 120
// kind: 0 = white, 1 = blue-white (matches shooting star), 2 = warm.
typedef struct { int16_t x, y; uint8_t bri, kind, big; } Star;
static Star stars[STAR_COUNT];

// Centered real-time clock overlay (labels render on top of the star canvas).
// Created in the ANIM_STARFIELD dispatch, updated once per second in
// star_timer_cb, and freed with anim_scr in anim_stop.
static lv_obj_t* sf_lbl_clock = nullptr;
static lv_obj_t* sf_lbl_date  = nullptr;
static int       sf_last_sec  = -1;

// Shooting star: a bright streak with a fading tail that crosses the sky
// every 20–90 s.
static bool          shoot_active  = false;
static float         shoot_x, shoot_y, shoot_vx, shoot_vy;
static unsigned long shoot_next_ms = 0;

static void shoot_schedule() {
    shoot_next_ms = millis() + (unsigned long)random(20000, 90001);
}

static void shoot_spawn() {
    shoot_x  = (float)random(-40, SCR_W / 2);
    shoot_y  = (float)random(0, SCR_H / 3);
    float sp = 7.0f + random(0, 40) * 0.1f;
    float ang = 0.35f + random(0, 50) * 0.005f;
    shoot_vx = sp * cosf(ang);
    shoot_vy = sp * sinf(ang);
    shoot_active = true;
}

static void star_init() {
    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].x    = random(0, SCR_W);
        stars[i].y    = random(0, SCR_H);
        stars[i].bri  = random(40, 255);
        int r = random(0, 100);
        stars[i].kind = (r < 55) ? 0 : (r < 88) ? 1 : 2;
        stars[i].big  = (random(0, 7) == 0) ? 1 : 0;
    }
    shoot_active = false;
    shoot_schedule();
}

// Tints a star's brightness by its kind.
static inline lv_color_t star_color(uint8_t b, uint8_t kind) {
    if (kind == 1) return lv_color_make((uint8_t)(b * 0.78f),
                                        (uint8_t)(b * 0.88f), b);
    if (kind == 2) return lv_color_make(b, (uint8_t)(b * 0.84f),
                                        (uint8_t)(b * 0.60f));
    return lv_color_make(b, b, b);
}

// Draws the full star field into the canvas (sky fill + all 120 stars).
static void star_draw() {
    lv_canvas_fill_bg(anim_canvas, lv_color_hex(0x05070F), LV_OPA_COVER);
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = LV_RADIUS_CIRCLE; rdsc.border_width = 0;

    for (int i = 0; i < STAR_COUNT; i++) {
        uint8_t    b  = stars[i].bri;
        lv_color_t c  = star_color(b, stars[i].kind);
        int        x  = stars[i].x;
        int        y  = stars[i].y;

        if (b > 150 || stars[i].big) {
            rdsc.bg_color = c;
            rdsc.bg_opa   = (b > 210) ? LV_OPA_30 : LV_OPA_20;
            int g = stars[i].big ? 6 : 4;
            lv_canvas_draw_rect(anim_canvas, x - g / 2, y - g / 2, g, g, &rdsc);
        }
        rdsc.bg_color = c;
        rdsc.bg_opa   = LV_OPA_COVER;
        int sz = stars[i].big ? 3 : (b > 190 ? 2 : 1);
        lv_canvas_draw_rect(anim_canvas, x, y, sz, sz, &rdsc);
    }
    rdsc.radius = 0;
}

// Updates the centered clock + date labels once per second (cheap; same time
// source as the Countdown screen). Shows --:--:-- until NTP has synced.
static void sf_update_clock() {
    if (!sf_lbl_clock) return;
    time_t now_t = time(nullptr);
    struct tm lt;
    localtime_r(&now_t, &lt);
    if (lt.tm_sec == sf_last_sec) return;   // only repaint when the second changes
    sf_last_sec = lt.tm_sec;

    if (lt.tm_year > 100) {   // NTP synced: year > 2000
        char clock_buf[12];
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d:%02d",
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
        lv_label_set_text(sf_lbl_clock, clock_buf);
        if (sf_lbl_date) {
            static const char* const WDAY[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            static const char* const MON[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
            char date_buf[20];
            snprintf(date_buf, sizeof(date_buf), "%s, %s %d",
                     WDAY[lt.tm_wday], MON[lt.tm_mon], lt.tm_mday);
            lv_label_set_text(sf_lbl_date, date_buf);
        }
    } else {
        lv_label_set_text(sf_lbl_clock, "--:--:--");
        if (sf_lbl_date) lv_label_set_text(sf_lbl_date, "---");
    }
}

static void star_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();
    sf_update_clock();

    // Twinkle ~15 random stars per frame.
    for (int i = 0; i < STAR_COUNT / 8; i++) {
        int idx = random(0, STAR_COUNT);
        int nb  = (int)stars[idx].bri + random(-30, 31);
        if (nb < 20)  nb = 20;
        if (nb > 255) nb = 255;
        stars[idx].bri = (uint8_t)nb;
    }

    // Redraw full star field at current brightnesses (this also erases the
    // previous frame's shooting star tail by filling the sky fresh).
    star_draw();

    // Shooting star: advance position and draw 5-segment fading tail.
    if (!shoot_active && millis() >= shoot_next_ms) shoot_spawn();

    if (shoot_active) {
        shoot_x += shoot_vx;
        shoot_y += shoot_vy;

        if (shoot_x > SCR_W + 30 || shoot_y > SCR_H + 30) {
            shoot_active = false;
            shoot_schedule();
        } else {
            lv_draw_line_dsc_t sd;
            lv_draw_line_dsc_init(&sd);
            for (int s = 0; s < 5; s++) {
                float f0 = (float)s;
                float f1 = (float)(s + 1);
                lv_point_t pts[2];
                pts[0].x = (lv_coord_t)(shoot_x - shoot_vx * f0);
                pts[0].y = (lv_coord_t)(shoot_y - shoot_vy * f0);
                pts[1].x = (lv_coord_t)(shoot_x - shoot_vx * f1);
                pts[1].y = (lv_coord_t)(shoot_y - shoot_vy * f1);
                uint8_t v = (uint8_t)(230 - s * 45);
                sd.color = lv_color_make(v, v, 255);
                sd.width = (s < 2) ? 3 : 2;
                sd.opa   = (lv_opa_t)(255 - s * 45);
                lv_canvas_draw_line(anim_canvas, pts, 2, &sd);
            }
        }
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// TIDEPOOL AT DUSK
// ══════════════════════════════════════════════════════════════════════════════
#define TP_HORIZON      122   // y where the dusk sky meets the water
#define TP_BUBBLE_COUNT   8   // rising bubbles in the water zone
#define TP_SUN_X        150   // setting-sun center x (left of center)
#define TP_SUN_Y        (TP_HORIZON - 3)  // sun sits just above the waterline
#define TP_SUN_R         15   // sun disc radius

// Crab mascot state: one crab walks the rocky shore, blinks, and holds its
// claws in the user's bull/bear candle colors.
typedef struct {
    float    x;           // shell center x
    float    vx;          // walk speed (positive = moving right)
    bool     right;       // facing direction
    uint8_t  walk_frame;  // 0 or 1 (alternating leg pose)
    uint8_t  walk_tick;   // ticks until next walk-frame flip
    uint16_t blink_cd;    // countdown frames to next blink (or blink duration)
    bool     blinking;    // currently in blink pose?
    uint16_t celebrate_cd;  // countdown to next claws-raised burst
    uint8_t  celebrate_fr;  // frames remaining in the claws-raised burst
} Crab;

typedef struct { int16_t x; float y; } Bubble;

static Crab   crab_state;
static Bubble bubbles[TP_BUBBLE_COUNT];

// ── Per-frame ambient layers: twinkle stars, sun glints, tide foam, gulls ────
#define TP_TWINKLE_COUNT 10
static struct { int16_t x, y; uint8_t bri, big; } tp_twk[TP_TWINKLE_COUNT];
#define TP_GLINT_COUNT 14
static struct { int16_t x, y; uint8_t phase; } tp_glint[TP_GLINT_COUNT];
static uint8_t tp_glint_tick = 0;
static uint8_t tp_foam_phase = 0;
#define TP_GULL_COUNT 2
static struct { float x; int16_t y; float vx; } tp_gull[TP_GULL_COUNT];

// Rocky shoreline: y of land surface at column x.
static inline int tp_floor_y(int x) {
    return (int)(SCR_H - 55
                 + 5.0f * sinf(x * 0.018f)
                 + 3.0f * sinf(x * 0.055f + 1.8f));
}

// Bakes a soft round pebble or boulder into anim_bg at (cx, cy).
static void aqua_blot(int cx, int cy, int rad, float pr, float pg, float pb) {
    for (int yy = cy - rad; yy <= cy + rad; yy++) {
        if (yy < 0 || yy >= SCR_H) continue;
        for (int xx = cx - rad; xx <= cx + rad; xx++) {
            if (xx < 0 || xx >= SCR_W) continue;
            float dx = (float)(xx - cx), dy = (float)(yy - cy);
            float dd = (dx * dx) / (float)(rad * rad)
                     + (dy * dy) / (float)(rad * rad);
            if (dd > 1.0f) continue;
            float sh = 1.0f - dd * 0.35f - (dy / (rad + 1)) * 0.18f;
            put_dith(anim_bg, xx, yy, pr * sh, pg * sh, pb * sh);
        }
    }
}

// One-time: sunset dusk sky + setting sun + headlands + tidal water with a sun
// reflection + rocky shore with tide-pools + beach detritus + stars. Baked into
// anim_bg with ordered dithering.
static void aqua_bg_render() {
    if (!anim_bg) return;

    // ── SKY / WATER / SHORE gradient ─────────────────────────────────────────
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int   ft = tp_floor_y(x);
            float r, g, b;
            if (y < TP_HORIZON) {
                // Dusk sunset: deep indigo top → violet midband → warm orange horizon
                float f    = (float)y / TP_HORIZON;        // 0 top .. 1 horizon
                float warm = f * f;                        // warmth ramps toward horizon
                r = 24.0f + warm * 196.0f;                 // 24 → 220
                g = 22.0f + warm * 70.0f + f * 16.0f;      // → ~108
                b = 58.0f - f * 18.0f + sinf(f * 3.14159f) * 34.0f;  // violet midband
            } else if (y < ft) {
                // Water: warm near horizon (sunset reflection), teal toward shore
                float denom = (float)(ft - TP_HORIZON);
                float f = (denom > 0.0f) ? (float)(y - TP_HORIZON) / denom : 1.0f;
                r = 60.0f * (1.0f - f) + 12.0f + f * 6.0f;
                g = 70.0f - f * 38.0f;
                b = 96.0f - f * 60.0f;
            } else {
                // Shore: warm dusk sand, deepening downward
                float denom = (float)(SCR_H - ft);
                float f = (denom > 0.0f) ? (float)(y - ft) / denom : 1.0f;
                r = 78.0f + f * 78.0f;
                g = 58.0f + f * 56.0f;
                b = 44.0f + f * 40.0f;
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // ── DISTANT HEADLANDS (two silhouette humps sitting on the horizon) ──────
    struct { int cx, w, h; } hill[2] = { { 60, 130, 26 }, { 410, 150, 20 } };
    for (int hh = 0; hh < 2; hh++) {
        int x0 = hill[hh].cx - hill[hh].w / 2;
        for (int x = x0; x <= hill[hh].cx + hill[hh].w / 2; x++) {
            if (x < 0 || x >= SCR_W) continue;
            float t   = (float)(x - x0) / (float)hill[hh].w;        // 0..1
            int   top = TP_HORIZON - (int)(hill[hh].h * sinf(t * 3.14159f));
            for (int y = top; y < TP_HORIZON; y++)
                put_dith(anim_bg, x, y, 46.0f, 32.0f, 54.0f);       // dark violet land
        }
    }

    // ── SETTING SUN + glow halo (over the sky) ───────────────────────────────
    aqua_blot(TP_SUN_X, TP_SUN_Y, TP_SUN_R + 16, 150.0f,  70.0f, 50.0f);  // outer glow
    aqua_blot(TP_SUN_X, TP_SUN_Y, TP_SUN_R + 7,  210.0f, 110.0f, 60.0f);  // inner glow
    aqua_blot(TP_SUN_X, TP_SUN_Y, TP_SUN_R,      255.0f, 196.0f, 120.0f); // disc

    // ── SUN REFLECTION COLUMN on the water (baked dappled warm band) ─────────
    {
        int ft = tp_floor_y(TP_SUN_X);
        for (int y = TP_HORIZON + 1; y < ft; y++) {
            int   half = 14 + (y - TP_HORIZON) * 2 / 5;            // fans to ~30% of screen width near the foreground
            float fade = 1.0f - (float)(y - TP_HORIZON) / (float)(ft - TP_HORIZON);
            for (int dx = -half; dx <= half; dx++) {
                int x = TP_SUN_X + dx;
                if (x < 0 || x >= SCR_W) continue;
                if (((y + dx) % 3) != 0) continue;                 // sparse → soft, subtle shimmer
                put_dith(anim_bg, x, y,
                         35.0f + 55.0f * fade, 34.0f + 28.0f * fade, 34.0f + 10.0f * fade);
            }
        }
    }

    // ── HORIZON SHIMMER line ─────────────────────────────────────────────────
    for (int x = 0; x < SCR_W; x++) {
        float sh = 0.5f + 0.5f * sinf(x * 0.08f);
        put_dith(anim_bg, x, TP_HORIZON, 120.0f * sh, 70.0f * sh, 60.0f * sh);
    }

    // ── WATER RIPPLE highlight lines (a few cool horizontal streaks) ─────────
    for (int ry = TP_HORIZON + 12; ry < SCR_H - 70; ry += 14) {
        for (int x = 0; x < SCR_W; x++) {
            int ft = tp_floor_y(x);
            if (ry >= ft) continue;
            float sh = 0.5f + 0.5f * sinf(x * 0.05f + ry * 0.3f);
            if (sh < 0.6f) continue;
            put_dith(anim_bg, x, ry, 40.0f + 25.0f * sh, 60.0f + 20.0f * sh, 80.0f + 20.0f * sh);
        }
    }

    // ── STARS in the upper (darker) sky band ─────────────────────────────────
    for (int i = 0; i < 30; i++) {
        int sx = random(0, SCR_W);
        int sy = random(2, TP_HORIZON - 40);
        uint8_t bv = (uint8_t)random(55, 200);
        put_dith(anim_bg, sx, sy, (float)bv, (float)bv, (float)bv);
        if (bv > 140 && sx + 1 < SCR_W)
            put_dith(anim_bg, sx + 1, sy, bv * 0.5f, bv * 0.5f, bv * 0.5f);
    }

    // ── SHORE pebbles and boulders ───────────────────────────────────────────
    static const uint32_t TP_PEBBLE[5] = {
        0x4A3A2C, 0x5A4A34, 0x6A5840, 0x342A20, 0x7A6848
    };
    for (int p = 0; p < 80; p++) {
        int px  = random(2, SCR_W - 2);
        int ft  = tp_floor_y(px);
        int py  = ft + random(2, SCR_H - ft - 1);
        int rad = random(1, 4);
        uint32_t c = TP_PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (float)((c >> 16) & 0xFF), (float)((c >> 8) & 0xFF), (float)(c & 0xFF));
    }
    for (int p = 0; p < 6; p++) {
        int px  = 40 + random(0, SCR_W - 80);
        int ft  = tp_floor_y(px);
        int rad = random(8, 15);
        int py  = ft + rad - 2;
        uint32_t c = TP_PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (float)((c >> 16) & 0xFF), (float)((c >> 8) & 0xFF), (float)(c & 0xFF));
        aqua_blot(px - rad / 3, py - rad / 3, rad / 3 + 1, 150.0f, 110.0f, 80.0f); // sunlit edge
    }

    // ── TIDE-POOL puddles in the rock (dark reflective patches + warm glint) ─
    static const int TP_POOL[2] = { 250, 360 };
    for (int p = 0; p < 2; p++) {
        int px = TP_POOL[p];
        int py = tp_floor_y(px) + 18;
        for (int yy = py - 4; yy <= py + 4; yy++)
            for (int xx = px - 12; xx <= px + 12; xx++) {
                if (xx < 0 || xx >= SCR_W || yy < 0 || yy >= SCR_H) continue;
                float ex = (float)(xx - px) / 12.0f, ey = (float)(yy - py) / 4.0f;
                if (ex * ex + ey * ey > 1.0f) continue;
                put_dith(anim_bg, xx, yy, 30.0f, 44.0f, 58.0f);    // reflective water
            }
        put_dith(anim_bg, px - 4, py - 1, 150.0f, 90.0f, 70.0f);   // warm sky glint
        put_dith(anim_bg, px - 3, py - 1, 120.0f, 80.0f, 64.0f);
    }

    // ── DETRITUS: starfish, shells, driftwood, seaweed ───────────────────────
    {
        int fx = 90, fy = tp_floor_y(fx) + 26;                     // starfish
        aqua_blot(fx, fy, 3, 196.0f, 110.0f, 70.0f);
        put_dith(anim_bg, fx,     fy - 5, 217, 140, 90);
        put_dith(anim_bg, fx - 5, fy - 1, 217, 140, 90);
        put_dith(anim_bg, fx + 5, fy - 1, 217, 140, 90);
        put_dith(anim_bg, fx - 3, fy + 5, 217, 140, 90);
        put_dith(anim_bg, fx + 3, fy + 5, 217, 140, 90);

        aqua_blot(300, tp_floor_y(300) + 30, 2, 210.0f, 196.0f, 172.0f);  // shells
        aqua_blot(430, tp_floor_y(430) + 22, 2, 224.0f, 206.0f, 182.0f);

        int wx = 200, wy = tp_floor_y(wx) + 40;                    // driftwood sliver
        for (int x = wx - 14; x <= wx + 14; x++)
            if (x >= 0 && x < SCR_W) {
                put_dith(anim_bg, x, wy,     92.0f, 70.0f, 50.0f);
                put_dith(anim_bg, x, wy + 1, 70.0f, 52.0f, 36.0f);
            }

        int kx = 360, kbase = tp_floor_y(kx) + 8;                  // seaweed clump
        for (int blade = -2; blade <= 2; blade++)
            for (int h = 0; h <= 8 - (blade < 0 ? -blade : blade) * 2; h++) {
                int xx = kx + blade * 2 + (int)(h * 0.3f * (blade < 0 ? -1 : 1));
                int yy = kbase - h;
                if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                    put_dith(anim_bg, xx, yy, 46.0f, 78.0f, 46.0f);
            }
    }
}

// Returns the bounding box that fully covers the crab sprite at (cx, cy).
// Covers the worst case (claws_raised adds 6px upward).
static void crab_bbox_at(int cx, int cy, int* x1, int* y1, int* x2, int* y2) {
    *x1 = cx - 25;  *y1 = cy - 25;
    *x2 = cx + 25;  *y2 = cy + 13;
}

// Draws a pixel-art crab centered at (cx, cy) onto the given canvas.
// Pixel layout follows the 48×48 brand-kit spec (deskticker_crab.svg <g id="crab48">).
// SVG anchor point: shell center at SVG (24, 27) maps to canvas (cx, cy).
// Left claw = bull_col, right claw = bear_col (user candle theme, set at runtime).
// blink: replace eyes with thin white slit.
// walk_frame 0/1: alternates leg y-offsets for walking motion.
// claws_raised: lifts claw clusters and arms 6 px upward (celebratory pose).
static void draw_crab(lv_obj_t* canvas, int cx, int cy,
                      lv_color_t bull_col, lv_color_t bear_col,
                      bool blink, uint8_t walk_frame,
                      bool claws_raised = false) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.border_width = 0;
    rd.radius       = 0;

    // All SVG coordinates offset by the anchor to get canvas coords.
    // cl: vertical lift applied to claw clusters and arms only.
    int cl = claws_raised ? -6 : 0;
    // Walk-frame leg offsets: alternates front/back leg pairs.
    int w0 = walk_frame ? -2 : 0;
    int w1 = walk_frame ?  2 : 0;

    // Inline helpers: R draws a rect at SVG coords with a hex color;
    //                 RC draws with a pre-formed lv_color_t (for user theme colors).
    auto R = [&](uint32_t col, int sx, int sy, int sw, int sh) {
        rd.bg_color = lv_color_hex(col);
        lv_canvas_draw_rect(canvas, cx + sx - 24, cy + sy - 27, sw, sh, &rd);
    };
    auto RC = [&](lv_color_t col, int sx, int sy, int sw, int sh) {
        rd.bg_color = col;
        lv_canvas_draw_rect(canvas, cx + sx - 24, cy + sy - 27, sw, sh, &rd);
    };

    // ── LEGS (drawn first so shell covers their roots) ────────────────────────
    R(0xC0432F, 12, 30+w0, 6, 2);  R(0xC0432F,  9, 32+w0, 4, 2);  R(0xC0432F,  7, 34+w0, 2, 3);
    R(0xC0432F, 13, 34+w1, 6, 2);  R(0xC0432F, 10, 36+w1, 4, 2);
    R(0xC0432F, 30, 30+w0, 6, 2);  R(0xC0432F, 35, 32+w0, 4, 2);  R(0xC0432F, 39, 34+w0, 2, 3);
    R(0xC0432F, 29, 34+w1, 6, 2);  R(0xC0432F, 34, 36+w1, 4, 2);

    // ── ARMS (shell will overlap their shell-side roots) ──────────────────────
    R(0x241016,  6, 28+cl, 8, 3);  R(0xD94F3D,  7, 29+cl, 6, 1);
    R(0x241016, 34, 28+cl, 8, 3);  R(0xD94F3D, 35, 29+cl, 6, 1);

    // ── CLAW CLUSTERS ─────────────────────────────────────────────────────────
    // Three candles per side: outline then body.
    // Left (bull): outer at SVG x=2 (top y=9), middle at x=6 (top y=13, swing-notch),
    //              inner at x=10 (top y=9).
    R(0x0F3A1C,  2,  9+cl, 4, 14); RC(bull_col,  3, 10+cl, 2, 12);
    R(0x0F3A1C,  6, 13+cl, 4, 14); RC(bull_col,  7, 14+cl, 2, 12);
    R(0x0F3A1C, 10,  9+cl, 4, 14); RC(bull_col, 11, 10+cl, 2, 12);
    // Right (bear): inner at x=34 (top y=9), middle at x=38 (top y=13),
    //               outer at x=42 (top y=9).
    R(0x5A0F12, 34,  9+cl, 4, 14); RC(bear_col, 35, 10+cl, 2, 12);
    R(0x5A0F12, 38, 13+cl, 4, 14); RC(bear_col, 39, 14+cl, 2, 12);
    R(0x5A0F12, 42,  9+cl, 4, 14); RC(bear_col, 43, 10+cl, 2, 12);

    // ── SHELL ─────────────────────────────────────────────────────────────────
    // Outline first, then fills layered on top.
    R(0x241016, 14, 20, 20,  2);  // top edge
    R(0x241016, 11, 22, 26,  2);  // second tier
    R(0x241016, 10, 24, 28,  9);  // main body outline
    R(0x241016, 13, 33, 22,  2);  // underbelly
    R(0xE85D49, 15, 22, 18,  2);  // base fill top
    R(0xE85D49, 12, 24, 24,  9);  // base fill main
    R(0xF8836B, 14, 24, 20,  3);  // highlight (top of shell)
    R(0xD94F3D, 12, 30, 24,  3);  // mid band (bottom of shell)

    // ── EYE STALKS ────────────────────────────────────────────────────────────
    R(0x241016, 18, 14, 4, 7);  R(0xE85D49, 19, 16, 2, 5);
    R(0x241016, 26, 14, 4, 7);  R(0xE85D49, 27, 16, 2, 5);

    // ── EYES ──────────────────────────────────────────────────────────────────
    R(0x241016, 16, 10, 6, 6);  // left eye socket
    R(0x241016, 26, 10, 6, 6);  // right eye socket
    if (!blink) {
        R(0xF4F6FA, 17, 11, 4, 4);  R(0x11151C, 18, 12, 2, 3);  R(0xFFFFFF, 17, 11, 1, 1);
        R(0xF4F6FA, 27, 11, 4, 4);  R(0x11151C, 28, 12, 2, 3);  R(0xFFFFFF, 27, 11, 1, 1);
    } else {
        // Blink: thin white slit across the dark eye socket.
        R(0xF4F6FA, 17, 13, 4, 1);
        R(0xF4F6FA, 27, 13, 4, 1);
    }
}

static void aqua_init() {
    // Crab starts left-of-center, walking right
    crab_state.x            = SCR_W * 0.32f;
    crab_state.vx           = 1.3f;
    crab_state.right        = true;
    crab_state.walk_frame   = 0;
    crab_state.walk_tick    = 0;
    crab_state.blink_cd     = (uint16_t)random(100, 220);
    crab_state.blinking     = false;
    crab_state.celebrate_cd = (uint16_t)random(160, 240);  // ~32-48 s first burst
    crab_state.celebrate_fr = 0;

    // Bubbles spread across the water zone
    for (int i = 0; i < TP_BUBBLE_COUNT; i++) {
        bubbles[i].x = random(0, SCR_W);
        int ft = tp_floor_y(bubbles[i].x);
        bubbles[i].y = (float)random(TP_HORIZON, ft - 5);
    }

    // Twinkling stars in the upper (darker) sky band
    for (int i = 0; i < TP_TWINKLE_COUNT; i++) {
        tp_twk[i].x   = (int16_t)random(4, SCR_W - 4);
        tp_twk[i].y   = (int16_t)random(3, TP_HORIZON - 42);
        tp_twk[i].bri = (uint8_t)random(60, 190);
        tp_twk[i].big = (random(0, 5) == 0) ? 1 : 0;
    }
    // Sun-reflection shimmer glints clustered on the water under the sun
    tp_glint_tick = 0;
    for (int i = 0; i < TP_GLINT_COUNT; i++) {
        tp_glint[i].x     = (int16_t)(TP_SUN_X + random(-70, 71));
        tp_glint[i].y     = (int16_t)random(TP_HORIZON + 6, TP_HORIZON + 128);
        tp_glint[i].phase = (uint8_t)random(0, 255);
    }
    tp_foam_phase = 0;
    // Gulls drift slowly across the sky
    for (int i = 0; i < TP_GULL_COUNT; i++) {
        tp_gull[i].x  = (float)random(0, SCR_W);
        tp_gull[i].y  = (int16_t)random(24, TP_HORIZON - 50);
        tp_gull[i].vx = (random(0, 2) ? 1 : -1) * (0.25f + random(0, 20) * 0.01f);
    }
}

static void aqua_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();

    // Fallback if anim_bg was not allocated
    if (!anim_bg) {
        lv_draw_rect_dsc_t fb; lv_draw_rect_dsc_init(&fb);
        fb.radius = 0; fb.border_width = 0;
        for (int y = 0; y < SCR_H; y++) {
            float f = (float)y / SCR_H;
            fb.bg_color = lv_color_make((uint8_t)(14 + f * 16),
                                        (uint8_t)(17 + f * 20),
                                        (uint8_t)(23 + f * 30));
            lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &fb);
        }
        lv_obj_invalidate(anim_canvas);
        return;
    }

    // ── AMBIENT LAYERS (drawn first so the crab/bubbles render on top) ────────
    // Twinkling stars in the upper sky
    {
        lv_draw_rect_dsc_t sd; lv_draw_rect_dsc_init(&sd);
        sd.radius = 0; sd.border_width = 0;
        for (int i = 0; i < TP_TWINKLE_COUNT; i++) {
            int sx = tp_twk[i].x, sy = tp_twk[i].y, sz = tp_twk[i].big ? 2 : 1;
            bg_restore(sx - 1, sy - 1, sx + sz, sy + sz);
            int nb = (int)tp_twk[i].bri + (int)random(-40, 41);
            if (nb < 40) nb = 40; if (nb > 255) nb = 255;
            tp_twk[i].bri = (uint8_t)nb;
            sd.bg_color = lv_color_make((uint8_t)nb, (uint8_t)nb, (uint8_t)(nb * 0.95f));
            sd.bg_opa   = LV_OPA_COVER;
            lv_canvas_draw_rect(anim_canvas, sx, sy, sz, sz, &sd);
        }
    }
    // Sun-reflection shimmer glints on the water
    tp_glint_tick++;
    {
        lv_draw_rect_dsc_t gd; lv_draw_rect_dsc_init(&gd);
        gd.radius = 0; gd.border_width = 0;
        for (int i = 0; i < TP_GLINT_COUNT; i++) {
            int gx = tp_glint[i].x, gy = tp_glint[i].y;
            bg_restore(gx - 1, gy, gx + 2, gy + 1);
            float sh = 0.5f + 0.5f * sinf((tp_glint_tick + tp_glint[i].phase) * 0.11f);
            uint8_t v = (uint8_t)(40.0f + 45.0f * sh);
            gd.bg_color = lv_color_make(v, (uint8_t)(v * 0.7f), (uint8_t)(v * 0.4f)); // warm
            gd.bg_opa   = (lv_opa_t)(22 + (int)(40.0f * sh));
            lv_canvas_draw_rect(anim_canvas, gx, gy, 2, 1, &gd);
        }
    }
    // Tide-foam line tracking the rock/water edge (warm-tinted, slides left)
    tp_foam_phase++;
    bg_restore(0, 252, SCR_W - 1, 276);
    {
        lv_draw_rect_dsc_t fd; lv_draw_rect_dsc_init(&fd);
        fd.radius = 0; fd.border_width = 0;
        fd.bg_color = lv_color_hex(0xE8C8A0); fd.bg_opa = LV_OPA_50;
        for (int x = 0; x < SCR_W; x++) {
            int ft = tp_floor_y(x);
            int wy = ft - 1 + (int)(2.0f * sinf((x + tp_foam_phase) * 0.09f));
            if (wy >= 0 && wy < SCR_H)
                lv_canvas_draw_rect(anim_canvas, x, wy, 1, 1, &fd);
        }
    }
    // Gulls drifting across the sky (little "v" silhouettes)
    {
        lv_draw_rect_dsc_t wd; lv_draw_rect_dsc_init(&wd);
        wd.radius = 0; wd.border_width = 0; wd.bg_color = lv_color_hex(0xD8D0E0);
        for (int i = 0; i < TP_GULL_COUNT; i++) {
            int ogx = (int)tp_gull[i].x, gy = tp_gull[i].y;
            bg_restore(ogx - 6, gy - 1, ogx + 6, gy + 3);
            tp_gull[i].x += tp_gull[i].vx;
            if (tp_gull[i].x < -8)        tp_gull[i].x = SCR_W + 8;
            if (tp_gull[i].x > SCR_W + 8) tp_gull[i].x = -8;
            int gx = (int)tp_gull[i].x;
            for (int k = 0; k < 3; k++) {
                int yy = gy + k;
                if (gx - 1 - k >= 0)    lv_canvas_draw_rect(anim_canvas, gx - 1 - k, yy, 2, 1, &wd);
                if (gx + k < SCR_W)     lv_canvas_draw_rect(anim_canvas, gx + k,     yy, 2, 1, &wd);
            }
        }
    }

    // ── CRAB ─────────────────────────────────────────────────────────────────

    // Advance walk-frame ticker (~8 ticks per frame flip = ~1 s at 120ms)
    crab_state.walk_tick++;
    if (crab_state.walk_tick >= 8) {
        crab_state.walk_tick  = 0;
        crab_state.walk_frame ^= 1;
    }

    // Blink state machine
    if (crab_state.blinking) {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        if (crab_state.blink_cd == 0) {
            crab_state.blinking = false;
            crab_state.blink_cd = (uint16_t)random(100, 220);
        }
    } else {
        if (crab_state.blink_cd > 0) {
            crab_state.blink_cd--;
        } else {
            crab_state.blinking = true;
            crab_state.blink_cd = 3;  // stay closed ~360ms then reopen
        }
    }

    // Celebrate (claws-raised) beat: fires every ~25 s for ~3 s (24 frames at 120ms)
    bool celebrating = false;
    if (crab_state.celebrate_fr > 0) {
        crab_state.celebrate_fr--;
        celebrating = true;
    } else if (crab_state.celebrate_cd > 0) {
        crab_state.celebrate_cd--;
    } else {
        crab_state.celebrate_fr = 24;
        crab_state.celebrate_cd = (uint16_t)random(160, 240);
    }

    // Old bbox (current position before move, used to erase ghost)
    int ocx = (int)crab_state.x;
    int ocy = tp_floor_y(ocx) - 8;
    int ox1, oy1, ox2, oy2;
    crab_bbox_at(ocx, ocy, &ox1, &oy1, &ox2, &oy2);

    // Advance position and bounce at screen edges
    crab_state.x += crab_state.vx;
    if (crab_state.x < 48.0f) {
        crab_state.vx    =  fabsf(crab_state.vx);
        crab_state.right = true;
    }
    if (crab_state.x > (float)(SCR_W - 48)) {
        crab_state.vx    = -fabsf(crab_state.vx);
        crab_state.right = false;
    }

    // New bbox
    int ncx = (int)crab_state.x;
    int ncy = tp_floor_y(ncx) - 8;
    int nx1, ny1, nx2, ny2;
    crab_bbox_at(ncx, ncy, &nx1, &ny1, &nx2, &ny2);

    // Erase last frame's ghost by restoring the union of old + new bboxes from bg
    bg_restore(
        (ox1 < nx1 ? ox1 : nx1), (oy1 < ny1 ? oy1 : ny1),
        (ox2 > nx2 ? ox2 : nx2), (oy2 > ny2 ? oy2 : ny2));

    // Draw crab with current candle-color claws
    draw_crab(anim_canvas, ncx, ncy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame, celebrating);

    // ── BUBBLES (tidal water zone) ────────────────────────────────────────────
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius       = LV_RADIUS_CIRCLE;
    rdsc.bg_opa       = LV_OPA_30;
    rdsc.bg_color     = lv_color_hex(0x7AB8C8);
    rdsc.border_width = 0;

    for (int i = 0; i < TP_BUBBLE_COUNT; i++) {
        int old_by = (int)bubbles[i].y;
        bubbles[i].y -= 0.35f;

        if (bubbles[i].y < TP_HORIZON) {
            // Erase and respawn near the floor
            if (old_by >= 0 && old_by < SCR_H)
                bg_restore(bubbles[i].x - 1, old_by - 1,
                           bubbles[i].x + 5, old_by + 5);
            bubbles[i].x = random(0, SCR_W);
            int ft = tp_floor_y(bubbles[i].x);
            bubbles[i].y = (float)(ft - random(10, 45));
            continue;
        }

        int new_by = (int)bubbles[i].y;
        int fy1 = (old_by < new_by ? old_by : new_by) - 1;
        int fy2 = (old_by > new_by ? old_by : new_by) + 5;
        bg_restore(bubbles[i].x - 1, fy1, bubbles[i].x + 5, fy2);

        if (new_by >= TP_HORIZON && new_by < SCR_H)
            lv_canvas_draw_rect(anim_canvas, bubbles[i].x, new_by, 4, 4, &rdsc);
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// CORAL REEF
// ══════════════════════════════════════════════════════════════════════════════
#define REEF_FISH_COUNT   6
#define REEF_BUBBLE_COUNT 10

typedef struct { float x, y, vx, scale; bool right; int col_idx; } ReefFish;

static ReefFish reef_fish[REEF_FISH_COUNT];
static Bubble   reef_bubbles[REEF_BUBBLE_COUNT];

// One slow drifting jellyfish + animated caustic light shafts (per-frame layers).
static struct { float x, y, vx; uint8_t phase; } reef_jelly;
#define REEF_RAY_COUNT 5
static struct { int16_t x; uint8_t phase; } reef_ray[REEF_RAY_COUNT];
static uint8_t reef_ray_tick = 0;

static const uint32_t REEF_FISH_COL[5] = {
    0xFFD700, 0xFF7420, 0x4FC3D8, 0xB0E860, 0xFF8CB0
};

// Wavy reef floor: y of rocky surface at column x.
static inline int reef_floor_y(int x) {
    return (int)(SCR_H - 50
                 + 4.0f * sinf(x * 0.015f)
                 + 2.0f * sinf(x * 0.048f + 1.2f));
}

// Simple reef fish: rounded body + forked tail + eye.
// Draws onto anim_canvas (only used from reef_timer_cb).
static void draw_reef_fish(int x, int y, lv_color_t col, bool right, float scale) {
    int bw = (int)(18 * scale); if (bw < 6) bw = 6;
    int bh = (int)(9  * scale); if (bh < 3) bh = 3;
    int tl = (int)(8  * scale); if (tl < 2) tl = 2;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.border_width = 0;

    lv_color_t dark = lv_color_mix(lv_color_black(), col, 60);
    lv_color_t lite = lv_color_mix(lv_color_white(), col, 35);

    // Forked tail (behind the body), with a filled notch
    rd.bg_color = dark;
    int prong_h = (bh - 1) / 3; if (prong_h < 1) prong_h = 1;
    if (right) {
        lv_canvas_draw_rect(anim_canvas, x - tl,     y,                tl,     prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x - tl,     y + bh - prong_h, tl,     prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x - tl / 2, y + bh / 2 - 1,   tl / 2 + 1, 2,   &rd);
    } else {
        lv_canvas_draw_rect(anim_canvas, x + bw,     y,                tl,     prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x + bw,     y + bh - prong_h, tl,     prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x + bw,     y + bh / 2 - 1,   tl / 2 + 1, 2,   &rd);
    }

    // Dorsal fin on top
    rd.bg_color = lite;
    lv_canvas_draw_rect(anim_canvas, x + bw / 4, y - 2, bw / 2, 2, &rd);

    // Rounded body
    rd.radius   = LV_RADIUS_CIRCLE;
    rd.bg_color = col;
    lv_canvas_draw_rect(anim_canvas, x, y, bw, bh, &rd);
    rd.radius   = 0;

    // Vertical stripe across the body
    rd.bg_color = dark;
    rd.bg_opa   = LV_OPA_50;
    int stripx = right ? (x + bw / 2) : (x + bw / 3);
    lv_canvas_draw_rect(anim_canvas, stripx, y, 2, bh, &rd);
    rd.bg_opa   = LV_OPA_COVER;

    // Eye (white + dark pupil)
    int ex = right ? (x + bw - (int)(4 * scale)) : (x + (int)(2 * scale));
    rd.bg_color = lv_color_hex(0xF4F6FA);
    lv_canvas_draw_rect(anim_canvas, ex, y + bh / 2 - 1, 2, 2, &rd);
    rd.bg_color = lv_color_hex(0x080808);
    lv_canvas_draw_rect(anim_canvas, ex + (right ? 1 : 0), y + bh / 2 - 1, 1, 2, &rd);
}

static void reef_fish_bbox(int fx, int fy, float sc, bool right,
                           int* x1, int* y1, int* x2, int* y2) {
    int bw = (int)(18 * sc); if (bw < 6) bw = 6;
    int bh = (int)(9  * sc); if (bh < 3) bh = 3;
    int tl = (int)(8  * sc); if (tl < 2) tl = 2;
    *x1 = (right ? fx - tl : fx) - 2;
    *x2 = (right ? fx + bw : fx + bw + tl) + 2;
    *y1 = fy - 4;                 // include the dorsal fin
    *y2 = fy + bh + 2;
}

// One-time: underwater gradient + caustics + sandy rippled seabed + varied coral,
// anemones, urchin, starfish + kelp. All baked into anim_bg.
static void reef_bg_render() {
    if (!anim_bg) return;

    // Water column: bright teal at surface → deep navy near the floor
    for (int y = 0; y < SCR_H; y++) {
        float f = (float)y / (SCR_H - 1);
        for (int x = 0; x < SCR_W; x++) {
            int   ft = reef_floor_y(x);
            float r, g, b;
            if (y >= ft) {
                // Sandy seabed (warm tan, darkening with depth)
                float fd = (float)(y - ft) / (float)(SCR_H - ft);
                r = 86.0f - fd * 30.0f;
                g = 74.0f - fd * 28.0f;
                b = 52.0f - fd * 22.0f;
            } else {
                r = 27.0f  - f * 20.0f;
                g = 111.0f - f * 90.0f;
                b = 168.0f - f * 140.0f;
                if (y < 44) {   // caustic shimmer near the surface
                    float caus = (44.0f - y) / 44.0f * 12.0f
                                 * (0.5f + 0.5f * sinf(x * 0.12f + y * 0.3f));
                    g += caus; if (g > 255.0f) g = 255.0f;
                    b += caus; if (b > 255.0f) b = 255.0f;
                }
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // Seabed ripple lines (darker dapples following the floor curve)
    for (int x = 0; x < SCR_W; x++) {
        int ft = reef_floor_y(x);
        for (int ry = ft + 8; ry < SCR_H; ry += 9) {
            float sh = 0.5f + 0.5f * sinf(x * 0.06f + ry * 0.5f);
            if (sh < 0.5f) continue;
            put_dith(anim_bg, x, ry, 60.0f, 50.0f, 34.0f);
        }
    }

    // Varied coral: kind 0 = brain (round ridged), 1 = branching, 2 = tube sponges
    struct { int cx; int sz; uint32_t col; uint8_t kind; } coral[9] = {
        {  40, 22, 0xFF7A7A, 0 }, {  95, 16, 0xFFB060, 1 }, { 150, 20, 0xFF6A8A, 2 },
        { 210, 24, 0xE76AC8, 0 }, { 265, 16, 0xFF9A6A, 1 }, { 320, 22, 0x9A7AFF, 2 },
        { 375, 18, 0x6AC8FF, 0 }, { 430, 22, 0xFF7A8A, 1 }, { 465, 14, 0xFFC050, 2 },
    };
    for (int c = 0; c < 9; c++) {
        int   ft = reef_floor_y(coral[c].cx);
        float cr = (coral[c].col >> 16) & 0xFF;
        float cg = (coral[c].col >>  8) & 0xFF;
        float cb =  coral[c].col        & 0xFF;
        if (coral[c].kind == 0) {                 // brain coral: stacked blobs
            int n = 2 + coral[c].sz / 8;
            for (int bi = 0; bi < n; bi++) {
                int bx   = coral[c].cx + random(-6, 7);
                int brad = coral[c].sz / 2 - bi * 2; if (brad < 4) brad = 4;
                aqua_blot(bx, ft - brad + 3, brad, cr, cg, cb);
            }
        } else if (coral[c].kind == 1) {          // branching coral: thin fingers
            for (int b = -2; b <= 2; b++) {
                int h = coral[c].sz - (b < 0 ? -b : b) * 3;
                for (int k = 0; k <= h; k++) {
                    int xx = coral[c].cx + b * 3 + (int)(k * 0.25f * b);
                    int yy = ft - k;
                    if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                        put_dith(anim_bg, xx, yy, cr, cg, cb);
                }
            }
        } else {                                  // tube sponges: vertical cylinders
            for (int t = 0; t < 3; t++) {
                int tx = coral[c].cx + (t - 1) * 4;
                int th = coral[c].sz - t * 3; if (th < 6) th = 6;
                for (int yy = ft; yy > ft - th; yy--)
                    for (int xx = tx - 1; xx <= tx + 1; xx++)
                        if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                            put_dith(anim_bg, xx, yy, cr, cg, cb);
                if (tx >= 0 && tx < SCR_W)        // dark mouth at the top
                    put_dith(anim_bg, tx, ft - th, cr * 0.3f, cg * 0.3f, cb * 0.3f);
            }
        }
    }

    // Anemones: base blob + radiating tentacle pixels
    static const int ANEM[2] = { 120, 350 };
    for (int a = 0; a < 2; a++) {
        int ax = ANEM[a]; int ay = reef_floor_y(ax) - 2;
        aqua_blot(ax, ay, 4, 220.0f, 120.0f, 150.0f);
        for (int t = -4; t <= 4; t++) {
            int len = 6 - (t < 0 ? -t : t);
            for (int k = 0; k <= len; k++) {
                int xx = ax + t + (int)(k * 0.2f * t);
                int yy = ay - 2 - k;
                if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                    put_dith(anim_bg, xx, yy, 230.0f, 150.0f, 180.0f);
            }
        }
    }

    // Sea urchin: dark spiky blob
    {
        int ux = 290, uy = reef_floor_y(ux) + 4;
        aqua_blot(ux, uy, 4, 40.0f, 24.0f, 50.0f);
        for (int s = 0; s < 8; s++) {
            float ang = s * 0.785f;
            int xx = ux + (int)(7 * cosf(ang));
            int yy = uy + (int)(7 * sinf(ang));
            if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                put_dith(anim_bg, xx, yy, 30.0f, 18.0f, 40.0f);
        }
    }

    // Starfish on the sand
    {
        int fx = 200, fy = reef_floor_y(fx) + 12;
        aqua_blot(fx, fy, 3, 230.0f, 120.0f, 70.0f);
        put_dith(anim_bg, fx,     fy - 5, 240, 150, 90);
        put_dith(anim_bg, fx - 5, fy - 1, 240, 150, 90);
        put_dith(anim_bg, fx + 5, fy - 1, 240, 150, 90);
        put_dith(anim_bg, fx - 3, fy + 5, 240, 150, 90);
        put_dith(anim_bg, fx + 3, fy + 5, 240, 150, 90);
    }

    // Kelp strands (6 tall strands with natural curve)
    static const uint32_t KELP[2] = { 0x2E8B57, 0x1F6B3F };
    for (int k = 0; k < 6; k++) {
        int   kx   = 50 + k * 75 + random(-12, 13);
        int   ft   = reef_floor_y(kx) + 2;
        int   hgt  = random(70, 130);
        float amp  = 5.0f + k * 1.5f;
        float freq = 0.020f + k * 0.003f;
        float ph   = k * 1.4f;
        uint32_t kc = KELP[k & 1];
        float kl_r  = (kc >> 16) & 0xFF;
        float kl_g  = (kc >>  8) & 0xFF;
        float kl_b  = kc & 0xFF;
        for (int yy = ft; yy > ft - hgt; yy--) {
            if (yy < 0 || yy >= SCR_H) continue;
            float tt  = (float)(ft - yy) / (float)hgt;
            int   kxx = kx + (int)(amp * tt * sinf(yy * freq + ph));
            int   hw  = (int)(2.5f * (1.0f - tt * 0.7f) + 0.5f);
            if (hw < 1) hw = 1;
            for (int xx = kxx - hw; xx <= kxx + hw; xx++) {
                if (xx < 0 || xx >= SCR_W) continue;
                float edge = 1.0f - 0.4f * fabsf((float)(xx - kxx)) / (hw + 0.5f);
                put_dith(anim_bg, xx, yy, kl_r * edge, kl_g * edge, kl_b * edge);
            }
        }
    }
}

static void reef_init() {
    // Fish across 3 depth bands (2 each): back = small+slow, front = large+fast
    for (int i = 0; i < REEF_FISH_COUNT; i++) {
        int   band = i % 3;
        float sc   = 0.55f + band * 0.22f;
        float spd  = 0.40f + band * 0.30f;
        reef_fish[i].scale   = sc;
        reef_fish[i].y       = 26.0f + band * 56.0f + random(-8, 9);
        reef_fish[i].x       = random(20, SCR_W - 20);
        reef_fish[i].right   = (bool)random(0, 2);
        reef_fish[i].vx      = reef_fish[i].right ? spd : -spd;
        reef_fish[i].col_idx = i;
    }
    for (int i = 0; i < REEF_BUBBLE_COUNT; i++) {
        reef_bubbles[i].x = random(0, SCR_W);
        int ft = reef_floor_y(reef_bubbles[i].x);
        reef_bubbles[i].y = (float)random(10, ft - 10);
    }
    // Slow drifting jellyfish through the upper-mid water
    reef_jelly.x     = (float)random(40, SCR_W - 40);
    reef_jelly.y     = (float)random(40, 120);
    reef_jelly.vx    = (random(0, 2) ? 1 : -1) * 0.25f;
    reef_jelly.phase = 0;
    // Caustic light shafts spread across the width
    reef_ray_tick = 0;
    for (int i = 0; i < REEF_RAY_COUNT; i++) {
        reef_ray[i].x     = (int16_t)(30 + i * (SCR_W - 60) / (REEF_RAY_COUNT - 1)
                                       + random(-12, 13));
        reef_ray[i].phase = (uint8_t)random(0, 255);
    }
    // Crab walks the reef floor (reuses crab_state — never active with tidepool)
    crab_state.x            = SCR_W * 0.45f;
    crab_state.vx           = 1.2f;
    crab_state.right        = true;
    crab_state.walk_frame   = 0;
    crab_state.walk_tick    = 0;
    crab_state.blink_cd     = (uint16_t)random(100, 220);
    crab_state.blinking     = false;
    crab_state.celebrate_cd = 0;
    crab_state.celebrate_fr = 0;
}

static void reef_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();

    if (!anim_bg) {
        // Fallback: solid blue gradient
        lv_draw_rect_dsc_t fb; lv_draw_rect_dsc_init(&fb);
        fb.radius = 0; fb.border_width = 0;
        for (int y = 0; y < SCR_H; y++) {
            float f = (float)y / SCR_H;
            fb.bg_color = lv_color_make((uint8_t)(27 - f * 20),
                                        (uint8_t)(111 - f * 90),
                                        (uint8_t)(168 - f * 140));
            lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &fb);
        }
        lv_obj_invalidate(anim_canvas);
        return;
    }

    // ── CAUSTIC LIGHT SHAFTS (pulsing translucent beams, drawn behind sprites) ─
    reef_ray_tick++;
    {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.radius = 0; rd.border_width = 0; rd.bg_color = lv_color_hex(0xBFF0FF);
        for (int i = 0; i < REEF_RAY_COUNT; i++) {
            int rx = reef_ray[i].x;
            bg_restore(rx - 3, 0, rx + 12, 150);
            float pulse = 0.4f + 0.6f * (0.5f + 0.5f *
                          sinf((reef_ray_tick + reef_ray[i].phase) * 0.04f));
            for (int seg = 0; seg < 6; seg++) {
                int   yy   = seg * 25;
                float fade = 1.0f - (float)yy / 150.0f;     // brightest at the surface
                int   op   = (int)(40.0f * fade * pulse);
                if (op < 3) continue;
                rd.bg_opa = (lv_opa_t)op;
                int xoff = (int)(yy * 0.06f);               // slight diagonal lean
                lv_canvas_draw_rect(anim_canvas, rx + xoff - 2, yy, 5, 25, &rd);
            }
        }
    }

    // ── JELLYFISH (slow translucent drifter) ──────────────────────────────────
    reef_jelly.phase++;
    {
        int ojx = (int)reef_jelly.x, ojy = (int)reef_jelly.y;
        bg_restore(ojx - 10, ojy - 8, ojx + 10, ojy + 22);
        reef_jelly.x += reef_jelly.vx;
        if (reef_jelly.x < -12)        reef_jelly.x = SCR_W + 12;
        if (reef_jelly.x > SCR_W + 12) reef_jelly.x = -12;
        reef_jelly.y += 0.15f * sinf(reef_jelly.phase * 0.08f);   // gentle bob
        int jx = (int)reef_jelly.x, jy = (int)reef_jelly.y;
        lv_draw_rect_dsc_t jd; lv_draw_rect_dsc_init(&jd);
        jd.radius = LV_RADIUS_CIRCLE; jd.border_width = 0;
        jd.bg_color = lv_color_hex(0xC8A0FF); jd.bg_opa = LV_OPA_50;
        lv_canvas_draw_rect(anim_canvas, jx - 8, jy - 6, 16, 12, &jd);   // bell
        jd.bg_opa = LV_OPA_30;
        lv_canvas_draw_rect(anim_canvas, jx - 6, jy - 8, 12, 6, &jd);    // dome cap
        jd.radius = 0; jd.bg_opa = LV_OPA_40; jd.bg_color = lv_color_hex(0xD8C0FF);
        for (int t = -3; t <= 3; t += 2)                                 // tentacles
            for (int k = 0; k < 14; k++) {
                int xx = jx + t + (int)(2.0f * sinf((k * 0.5f) + reef_jelly.phase * 0.15f + t));
                int yy = jy + 5 + k;
                if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                    lv_canvas_draw_rect(anim_canvas, xx, yy, 1, 1, &jd);
            }
    }

    // ── CRAB ─────────────────────────────────────────────────────────────────
    crab_state.walk_tick++;
    if (crab_state.walk_tick >= 8) { crab_state.walk_tick = 0; crab_state.walk_frame ^= 1; }

    if (crab_state.blinking) {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        if (crab_state.blink_cd == 0) {
            crab_state.blinking = false;
            crab_state.blink_cd = (uint16_t)random(100, 220);
        }
    } else {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        else { crab_state.blinking = true; crab_state.blink_cd = 3; }
    }

    int ocx = (int)crab_state.x;
    int ocy = reef_floor_y(ocx) - 8;
    int ox1, oy1, ox2, oy2;
    crab_bbox_at(ocx, ocy, &ox1, &oy1, &ox2, &oy2);

    crab_state.x += crab_state.vx;
    if (crab_state.x < 48.0f)           { crab_state.vx =  fabsf(crab_state.vx); crab_state.right = true;  }
    if (crab_state.x > SCR_W - 48.0f)   { crab_state.vx = -fabsf(crab_state.vx); crab_state.right = false; }

    int ncx = (int)crab_state.x;
    int ncy = reef_floor_y(ncx) - 8;
    int nx1, ny1, nx2, ny2;
    crab_bbox_at(ncx, ncy, &nx1, &ny1, &nx2, &ny2);
    bg_restore((ox1<nx1?ox1:nx1),(oy1<ny1?oy1:ny1),(ox2>nx2?ox2:nx2),(oy2>ny2?oy2:ny2));
    draw_crab(anim_canvas, ncx, ncy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame);

    // ── FISH ─────────────────────────────────────────────────────────────────
    for (int i = 0; i < REEF_FISH_COUNT; i++) {
        int px1, py1, px2, py2;
        reef_fish_bbox((int)reef_fish[i].x, (int)reef_fish[i].y,
                       reef_fish[i].scale, reef_fish[i].right,
                       &px1, &py1, &px2, &py2);

        reef_fish[i].x += reef_fish[i].vx;
        if (reef_fish[i].x < -25) {
            reef_fish[i].x     = SCR_W + 8;
            reef_fish[i].right = false;
            reef_fish[i].vx    = -fabsf(reef_fish[i].vx);
        }
        if (reef_fish[i].x > SCR_W + 8) {
            reef_fish[i].x     = -8;
            reef_fish[i].right = true;
            reef_fish[i].vx    =  fabsf(reef_fish[i].vx);
        }

        int nx1f, ny1f, nx2f, ny2f;
        reef_fish_bbox((int)reef_fish[i].x, (int)reef_fish[i].y,
                       reef_fish[i].scale, reef_fish[i].right,
                       &nx1f, &ny1f, &nx2f, &ny2f);
        bg_restore((px1<nx1f?px1:nx1f),(py1<ny1f?py1:ny1f),
                   (px2>nx2f?px2:nx2f),(py2>ny2f?py2:ny2f));
        draw_reef_fish((int)reef_fish[i].x, (int)reef_fish[i].y,
                       lv_color_hex(REEF_FISH_COL[reef_fish[i].col_idx % 5]),
                       reef_fish[i].right, reef_fish[i].scale);
    }

    // ── BUBBLES ───────────────────────────────────────────────────────────────
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius       = LV_RADIUS_CIRCLE;
    rdsc.bg_opa       = LV_OPA_30;
    rdsc.bg_color     = lv_color_hex(0x80C8E0);
    rdsc.border_width = 0;

    for (int i = 0; i < REEF_BUBBLE_COUNT; i++) {
        int old_by = (int)reef_bubbles[i].y;
        reef_bubbles[i].y -= 0.35f;

        if (reef_bubbles[i].y < 0) {
            if (old_by >= 0 && old_by < SCR_H)
                bg_restore(reef_bubbles[i].x - 1, old_by - 1,
                           reef_bubbles[i].x + 5, old_by + 5);
            reef_bubbles[i].x = random(0, SCR_W);
            int ft = reef_floor_y(reef_bubbles[i].x);
            reef_bubbles[i].y = (float)(ft - random(5, 30));
            continue;
        }

        int new_by = (int)reef_bubbles[i].y;
        int fy1    = (old_by < new_by ? old_by : new_by) - 1;
        int fy2    = (old_by > new_by ? old_by : new_by) + 5;
        bg_restore(reef_bubbles[i].x - 1, fy1, reef_bubbles[i].x + 5, fy2);
        if (new_by >= 0 && new_by < SCR_H)
            lv_canvas_draw_rect(anim_canvas, reef_bubbles[i].x, new_by, 4, 4, &rdsc);
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// PIXEL BEACH (NIGHT)
// ══════════════════════════════════════════════════════════════════════════════
#define PB_HORIZON    210   // y where water meets sand
#define PB_SAND_TOP   265   // y where dry sand starts

// Boardwalk lamp-post geometry (replaces the old lighthouse). The post is a
// tall weathered-wood column near the right side; a glowing lantern head sits
// just below the finial, and a long warm cone of light fans down to the sand.
#define PB_LAMP_X      400   // post center x (pulled inboard so the cone fits)
#define PB_LAMP_BASE   288   // y of the post foot (planted in the DRY sand, below the surf)
#define PB_LAMP_TOP    151   // y of the finial at the top of the post
#define PB_LANTERN_Y   165   // y of the lantern head (glowing box) center
#define PB_LANTERN_HW    5   // lantern head half-width  (box = 2*HW+1 wide)
#define PB_LANTERN_HH    7   // lantern head half-height (box = 2*HH+1 tall)

// Footprint ring buffer (crab leaves prints in wet sand)
#define PB_PRINT_MAX  12
typedef struct { int16_t x, y; } Footprint;
static Footprint pb_prints[PB_PRINT_MAX];
static int    pb_print_head  = 0;
static int    pb_print_count = 0;
static uint8_t pb_print_tick  = 0;

// Wave foam phase counter
static uint8_t pb_wave_phase = 0;

// ── Twinkling stars (a handful animate; the 38 baked stars stay static) ──────
#define PB_TWINKLE_COUNT 12
typedef struct { int16_t x, y; uint8_t bri, big; } PbStar;
static PbStar pb_twinkle[PB_TWINKLE_COUNT];

// ── Water glints (moon + lamp reflections shimmering on the sea) ─────────────
#define PB_GLINT_COUNT 8
typedef struct { int16_t x, y; uint8_t phase; } PbGlint;
static PbGlint pb_glints[PB_GLINT_COUNT];
static uint8_t pb_glint_tick = 0;

// ── Lantern glow flicker phase ───────────────────────────────────────────────
static uint8_t pb_lamp_phase = 0;

// ── Occasional shooting star — erased each frame via bg_restore of its last
//    drawn bounding box (Pixel Beach has no full-sky refill like Starfield) ────
static bool          pb_shoot_active = false;
static float         pb_shoot_x, pb_shoot_y, pb_shoot_vx, pb_shoot_vy;
static unsigned long pb_shoot_next_ms = 0;
static int           pb_shoot_x1, pb_shoot_y1, pb_shoot_x2, pb_shoot_y2;

// Flat sand floor for this scene
static inline int pixbeach_floor_y(int x) {
    (void)x;
    return PB_SAND_TOP;
}

// Computes the baked base sky/water/sand color at row y — IDENTICAL gradient to
// the fill loop below (the fill depends only on y). The light cone uses this to
// add warm light on top of the exact base color, so there is no visible seam.
static inline void pixbeach_base_rgb(int y, float* r, float* g, float* b) {
    if (y < PB_HORIZON) {
        float f = (float)y / PB_HORIZON;
        *r = 6.0f  + f * 20.0f;
        *g = 7.0f  + f * 11.0f;
        *b = 13.0f + f * 37.0f;
    } else if (y < PB_SAND_TOP) {
        float f = (float)(y - PB_HORIZON) / (PB_SAND_TOP - PB_HORIZON);
        *r = 15.0f - f *  5.0f;
        *g = 42.0f + f * 10.0f;
        *b = 56.0f - f * 10.0f;
    } else {
        float f = (float)(y - PB_SAND_TOP) / (SCR_H - PB_SAND_TOP);
        *r = 58.0f + f * 32.0f;
        *g = 46.0f + f * 20.0f;
        *b = 30.0f + f * 12.0f;
    }
}

// Bake the static night-beach background into anim_bg.
static void pixbeach_bg_render() {
    if (!anim_bg) return;

    // Sky + sea + sand fill
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            float r, g, b;
            if (y < PB_HORIZON) {
                // Night sky: deep navy at top → purple-navy at horizon
                float f = (float)y / PB_HORIZON;
                r = 6.0f  + f * 20.0f;
                g = 7.0f  + f * 11.0f;
                b = 13.0f + f * 37.0f;
            } else if (y < PB_SAND_TOP) {
                // Dark teal water
                float f = (float)(y - PB_HORIZON) / (PB_SAND_TOP - PB_HORIZON);
                r = 15.0f - f *  5.0f;
                g = 42.0f + f * 10.0f;
                b = 56.0f - f * 10.0f;
            } else {
                // Dim warm sand
                float f = (float)(y - PB_SAND_TOP) / (SCR_H - PB_SAND_TOP);
                r = 58.0f + f * 32.0f;
                g = 46.0f + f * 20.0f;
                b = 30.0f + f * 12.0f;
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // Horizon shimmer
    for (int x = 0; x < SCR_W; x++) {
        float sh = 0.5f + 0.5f * sinf(x * 0.06f);
        put_dith(anim_bg, x, PB_HORIZON,
                 20.0f * sh, 55.0f * sh, 70.0f * sh);
    }

    // Crescent moon: white disc + deep-space overlay to carve crescent
    aqua_blot(SCR_W - 90, 40, 14, 200.0f, 200.0f, 185.0f);   // full disc
    aqua_blot(SCR_W - 82, 36, 13, 6.0f, 7.0f, 13.0f);         // carve crescent

    // Baked stars
    for (int i = 0; i < 38; i++) {
        int sx = random(0, SCR_W - 20);
        int sy = random(2, PB_HORIZON - 30);
        uint8_t bv = (uint8_t)random(55, 190);
        put_dith(anim_bg, sx, sy, (float)bv, (float)bv, (float)bv * 0.9f);
        if (bv > 130 && sx + 1 < SCR_W)
            put_dith(anim_bg, sx + 1, sy, bv * 0.45f, bv * 0.45f, bv * 0.45f);
    }

    // Sand grain pebbles
    static const uint32_t PB_PEB[4] = { 0x2A1E14, 0x3A2A1C, 0x1E1610, 0x4A3828 };
    for (int p = 0; p < 80; p++) {
        int px  = random(2, SCR_W - 2);
        int py  = PB_SAND_TOP + random(1, SCR_H - PB_SAND_TOP - 1);
        int rad = random(0, 2);  // 0 = 1px, 1 = 2px
        uint32_t c = PB_PEB[random(0, 4)];
        if (rad == 0) {
            put_dith(anim_bg, px, py,
                     (float)((c >> 16) & 0xFF),
                     (float)((c >>  8) & 0xFF),
                     (float)( c        & 0xFF));
        } else {
            aqua_blot(px, py, rad,
                      (float)((c >> 16) & 0xFF),
                      (float)((c >>  8) & 0xFF),
                      (float)( c        & 0xFF));
        }
    }

    // ── LIGHT CONE (baked, steady warm glow from the lantern down to the sand) ─
    // Baked first so the solid post & bench (drawn next) occlude the beam, and
    // so the crab walking through it is lit/erased correctly by bg_restore().
    // Half-width grows with depth; intensity fades with distance + toward edges.
    int cone_top = PB_LANTERN_Y + PB_LANTERN_HH;   // just below the lantern glass
    int cone_bot = PB_LAMP_BASE + 4;               // pools at the post foot on the sand
    for (int cy = cone_top; cy <= cone_bot; cy++) {
        float depth = (float)(cy - cone_top) / (float)(cone_bot - cone_top); // 0..1
        int   half  = 3 + (int)(depth * 34.0f);    // fan from ~3 px to ~37 px
        float fall  = (1.0f - depth) * (1.0f - depth);  // brighter near the lamp
        for (int dx = -half; dx <= half; dx++) {
            int cx = PB_LAMP_X + dx;
            if (cx < 0 || cx >= SCR_W || cy < 0 || cy >= SCR_H) continue;
            float edge = 1.0f - (float)(dx < 0 ? -dx : dx) / (float)(half + 1);
            float add  = 72.0f * fall * edge;       // warm additive intensity
            float br, bg, bb;
            pixbeach_base_rgb(cy, &br, &bg, &bb);
            put_dith(anim_bg, cx, cy, br + add, bg + add * 0.78f, bb + add * 0.35f);
        }
    }

    // ── BOARDWALK LAMP POST (solid, baked on top of the cone) ─────────────────
    // Foot / base plate planted in the sand
    for (int ly = PB_LAMP_BASE - 1; ly <= PB_LAMP_BASE + 3; ly++)
        for (int lx = PB_LAMP_X - 5; lx <= PB_LAMP_X + 5; lx++)
            if (lx >= 0 && lx < SCR_W && ly >= 0 && ly < SCR_H)
                put_dith(anim_bg, lx, ly, 30.0f, 24.0f, 18.0f);
    // Vertical post: ~5 px weathered wood; lit side (toward lantern) a bit warmer
    for (int ly = PB_LAMP_TOP + 4; ly < PB_LAMP_BASE; ly++) {
        for (int lx = PB_LAMP_X - 2; lx <= PB_LAMP_X + 2; lx++) {
            if (lx < 0 || lx >= SCR_W || ly < 0 || ly >= SCR_H) continue;
            float lit = (lx >= PB_LAMP_X) ? 1.15f : 0.80f;
            put_dith(anim_bg, lx, ly, 74.0f * lit, 53.0f * lit, 38.0f * lit);
        }
    }
    // Finial knob at the very top
    for (int ly = PB_LAMP_TOP; ly <= PB_LAMP_TOP + 3; ly++)
        for (int lx = PB_LAMP_X - 1; lx <= PB_LAMP_X + 1; lx++)
            if (lx >= 0 && lx < SCR_W) put_dith(anim_bg, lx, ly, 70.0f, 52.0f, 36.0f);
    // Lantern head: dark frame box with a warm glowing glass core
    for (int ly = PB_LANTERN_Y - PB_LANTERN_HH; ly <= PB_LANTERN_Y + PB_LANTERN_HH; ly++) {
        for (int lx = PB_LAMP_X - PB_LANTERN_HW; lx <= PB_LAMP_X + PB_LANTERN_HW; lx++) {
            if (lx < 0 || lx >= SCR_W || ly < 0 || ly >= SCR_H) continue;
            bool frame = (ly == PB_LANTERN_Y - PB_LANTERN_HH ||
                          ly == PB_LANTERN_Y + PB_LANTERN_HH ||
                          lx == PB_LAMP_X - PB_LANTERN_HW ||
                          lx == PB_LAMP_X + PB_LANTERN_HW);
            if (frame) put_dith(anim_bg, lx, ly, 42.0f, 42.0f, 48.0f);     // 0x2A2A30
            else       put_dith(anim_bg, lx, ly, 245.0f, 232.0f, 160.0f);  // 0xF5E8A0
        }
    }

    // ── WOODEN BENCH (in the lit sand, just left of the post foot) ────────────
    {
        int bx = PB_LAMP_X - 22;   // bench center, inside the left of the cone
        int sy = 280;              // seat top y (on the dry sand, below the surf)
        // Backrest slats (vertical)
        for (int s = -14; s <= 14; s += 7)
            for (int by = sy - 12; by < sy; by++)
                if (bx + s >= 0 && bx + s < SCR_W)
                    put_dith(anim_bg, bx + s, by, 106.0f, 74.0f, 48.0f);
        // Top rail of the backrest (lit highlight)
        for (int bxx = bx - 15; bxx <= bx + 15; bxx++)
            if (bxx >= 0 && bxx < SCR_W)
                put_dith(anim_bg, bxx, sy - 12, 138.0f, 106.0f, 68.0f);
        // Seat plank (lit top edge, darker body)
        for (int by = sy; by < sy + 4; by++)
            for (int bxx = bx - 16; bxx <= bx + 16; bxx++)
                if (bxx >= 0 && bxx < SCR_W)
                    put_dith(anim_bg, bxx, by,
                             (by == sy ? 138.0f : 106.0f),
                             (by == sy ? 106.0f :  74.0f),
                             (by == sy ?  68.0f :  48.0f));
        // Two legs down to the sand
        for (int by = sy + 4; by <= 292; by++)
            for (int legi = 0; legi < 2; legi++) {
                int bxx = bx + (legi ? 12 : -13);
                if (bxx >= 0 && bxx < SCR_W)     put_dith(anim_bg, bxx,     by, 74.0f, 50.0f, 30.0f);
                if (bxx + 1 >= 0 && bxx + 1 < SCR_W) put_dith(anim_bg, bxx + 1, by, 74.0f, 50.0f, 30.0f);
            }
    }

    // ── STARFISH & SEASHELLS scattered on the dry sand ────────────────────────
    static const int PB_SF[][2] = { { 70, 300 }, { 205, 290 } };   // starfish
    for (int s = 0; s < 2; s++) {
        int fx = PB_SF[s][0], fy = PB_SF[s][1];
        aqua_blot(fx, fy, 2, 196.0f, 120.0f, 78.0f);   // body 0xC4784E
        put_dith(anim_bg, fx,     fy - 4, 217.0f, 140.0f, 90.0f);  // 5 arms 0xD98C5A
        put_dith(anim_bg, fx - 4, fy - 1, 217.0f, 140.0f, 90.0f);
        put_dith(anim_bg, fx + 4, fy - 1, 217.0f, 140.0f, 90.0f);
        put_dith(anim_bg, fx - 3, fy + 4, 217.0f, 140.0f, 90.0f);
        put_dith(anim_bg, fx + 3, fy + 4, 217.0f, 140.0f, 90.0f);
    }
    static const int PB_SH[][2] = { { 120, 308 }, { 300, 298 }, { 440, 312 } }; // shells
    for (int s = 0; s < 3; s++) {
        int hx = PB_SH[s][0], hy = PB_SH[s][1];
        aqua_blot(hx, hy, 2, 200.0f, 184.0f, 160.0f);  // fan body
        put_dith(anim_bg, hx,     hy - 2, 232.0f, 216.0f, 192.0f);  // ridge 0xE8D8C0
        put_dith(anim_bg, hx - 2, hy,     210.0f, 194.0f, 170.0f);
        put_dith(anim_bg, hx + 2, hy,     210.0f, 194.0f, 170.0f);
    }

    // ── DUNE GRASS tufts near the lamp / bench ────────────────────────────────
    static const int PB_GRASS[3] = { PB_LAMP_X + 20, PB_LAMP_X - 50, 44 };
    for (int t = 0; t < 3; t++) {
        int gx    = PB_GRASS[t];
        int gbase = PB_SAND_TOP + 4 + t * 2;
        for (int blade = -2; blade <= 2; blade++) {
            int top = 9 - (blade < 0 ? -blade : blade) * 2;   // outer blades shorter
            for (int h = 0; h <= top; h++) {
                int yy = gbase - h;
                int xx = gx + blade * 2 + (int)(h * 0.3f * (blade < 0 ? -1 : 1));
                if (xx >= 0 && xx < SCR_W && yy >= 0 && yy < SCR_H)
                    put_dith(anim_bg, xx, yy, 58.0f, 90.0f, 44.0f);  // 0x3A5A2C
            }
        }
    }
}

static void pixbeach_init() {
    pb_print_head  = 0;
    pb_print_count = 0;
    pb_print_tick  = 0;
    pb_wave_phase  = 0;
    pb_lamp_phase  = 0;
    pb_glint_tick  = 0;

    // Twinkling stars scattered in the sky (separate from the 38 baked stars)
    for (int i = 0; i < PB_TWINKLE_COUNT; i++) {
        pb_twinkle[i].x   = (int16_t)random(4, SCR_W - 24);
        pb_twinkle[i].y   = (int16_t)random(3, PB_HORIZON - 34);
        pb_twinkle[i].bri = (uint8_t)random(60, 200);
        pb_twinkle[i].big = (random(0, 5) == 0) ? 1 : 0;
    }

    // Water glints: half cluster under the moon (right), half under the lamp post
    for (int i = 0; i < PB_GLINT_COUNT; i++) {
        bool moonside = (i < PB_GLINT_COUNT / 2);
        int  base_x   = moonside ? (SCR_W - 90) : PB_LAMP_X;
        pb_glints[i].x     = (int16_t)(base_x + random(-14, 15));
        pb_glints[i].y     = (int16_t)random(PB_HORIZON + 4, PB_SAND_TOP - 3);
        pb_glints[i].phase = (uint8_t)random(0, 255);
    }

    // Shooting star: idle, with an empty last-bbox so the first erase is a no-op
    pb_shoot_active  = false;
    pb_shoot_x1 = 0; pb_shoot_y1 = 0; pb_shoot_x2 = -1; pb_shoot_y2 = -1;
    pb_shoot_next_ms = millis() + (unsigned long)random(15000, 45001);

    crab_state.x            = SCR_W * 0.35f;
    crab_state.vx           = 1.2f;
    crab_state.right        = true;
    crab_state.walk_frame   = 0;
    crab_state.walk_tick    = 0;
    crab_state.blink_cd     = (uint16_t)random(100, 220);
    crab_state.blinking     = false;
    crab_state.celebrate_cd = (uint16_t)random(160, 240);
    crab_state.celebrate_fr = 0;
}

static void pixbeach_timer_cb(lv_timer_t*) {
    if (!anim_canvas || !anim_bg) return;
    wdt_feed();

    // ── WAVE FOAM LINE ────────────────────────────────────────────────────────
    // Erase previous foam strip from the full-width band at the sand edge
    bg_restore(0, PB_SAND_TOP - 3, SCR_W - 1, PB_SAND_TOP + 2);

    // Draw new foam: 1 px sinusoid sliding left
    lv_draw_rect_dsc_t wd;
    lv_draw_rect_dsc_init(&wd);
    wd.radius = 0; wd.border_width = 0;
    wd.bg_color = lv_color_hex(0xB0C8D0);
    wd.bg_opa   = LV_OPA_60;
    pb_wave_phase++;
    for (int x = 0; x < SCR_W; x++) {
        // The lamp post stands on the beach in front of the distant surf — don't
        // paint foam over the part of the pole that crosses the waterline.
        if (x >= PB_LAMP_X - 3 && x <= PB_LAMP_X + 3) continue;
        int wy = PB_SAND_TOP - 1 + (int)(2.0f * sinf((x + pb_wave_phase) * 0.08f));
        if (wy >= 0 && wy < SCR_H)
            lv_canvas_draw_rect(anim_canvas, x, wy, 1, 1, &wd);
    }

    // ── SHOOTING STAR: ERASE previous streak first ────────────────────────────
    // Done before the twinkle/glint draws so it can't wipe this frame's sprites.
    if (pb_shoot_x2 >= pb_shoot_x1 && pb_shoot_y2 >= pb_shoot_y1)
        bg_restore(pb_shoot_x1, pb_shoot_y1, pb_shoot_x2, pb_shoot_y2);
    pb_shoot_x2 = -1; pb_shoot_y2 = -1;   // mark empty until (re)drawn below

    // ── TWINKLING STARS ───────────────────────────────────────────────────────
    {
        lv_draw_rect_dsc_t sd;
        lv_draw_rect_dsc_init(&sd);
        sd.radius = 0; sd.border_width = 0;
        for (int i = 0; i < PB_TWINKLE_COUNT; i++) {
            int sx = pb_twinkle[i].x, sy = pb_twinkle[i].y;
            int sz = pb_twinkle[i].big ? 2 : 1;
            bg_restore(sx - 1, sy - 1, sx + sz, sy + sz);   // erase from baked sky
            int nb = (int)pb_twinkle[i].bri + (int)random(-40, 41);
            if (nb < 40)  nb = 40;
            if (nb > 255) nb = 255;
            pb_twinkle[i].bri = (uint8_t)nb;
            sd.bg_color = lv_color_make((uint8_t)nb, (uint8_t)nb, (uint8_t)(nb * 0.92f));
            if (nb > 170) {                                 // soft halo behind the core
                sd.bg_opa = LV_OPA_20;
                lv_canvas_draw_rect(anim_canvas, sx - 1, sy - 1, sz + 2, sz + 2, &sd);
            }
            sd.bg_opa = LV_OPA_COVER;                       // crisp bright core on top
            lv_canvas_draw_rect(anim_canvas, sx, sy, sz, sz, &sd);
        }
    }

    // ── WATER GLINTS (moon + lamp reflections shimmering on the sea) ──────────
    pb_glint_tick++;
    {
        lv_draw_rect_dsc_t gd;
        lv_draw_rect_dsc_init(&gd);
        gd.radius = 0; gd.border_width = 0;
        for (int i = 0; i < PB_GLINT_COUNT; i++) {
            int gx = pb_glints[i].x, gy = pb_glints[i].y;
            bg_restore(gx - 1, gy, gx + 2, gy + 1);
            float sh = 0.5f + 0.5f * sinf((pb_glint_tick + pb_glints[i].phase) * 0.10f);
            uint8_t v = (uint8_t)(60.0f + 120.0f * sh);
            bool moonside = (i < PB_GLINT_COUNT / 2);
            gd.bg_color = moonside
                ? lv_color_make(v, v, (uint8_t)(v * 0.95f))                 // cool moon
                : lv_color_make(v, (uint8_t)(v * 0.82f), (uint8_t)(v * 0.45f)); // warm lamp
            gd.bg_opa = (lv_opa_t)(60 + (int)(120.0f * sh));
            lv_canvas_draw_rect(anim_canvas, gx, gy, 2, 1, &gd);
        }
    }

    // ── LANTERN GLOW FLICKER (tiny breathing halo over the baked lantern) ─────
    pb_lamp_phase++;
    bg_restore(PB_LAMP_X - PB_LANTERN_HW - 2, PB_LANTERN_Y - PB_LANTERN_HH - 2,
               PB_LAMP_X + PB_LANTERN_HW + 2, PB_LANTERN_Y + PB_LANTERN_HH + 2);
    {
        lv_draw_rect_dsc_t hd;
        lv_draw_rect_dsc_init(&hd);
        hd.radius = LV_RADIUS_CIRCLE; hd.border_width = 0;
        float fl = 0.78f + 0.22f * sinf(pb_lamp_phase * 0.18f);
        hd.bg_color = lv_color_hex(0xF5E8A0);
        hd.bg_opa   = (lv_opa_t)(70.0f * fl);
        lv_canvas_draw_rect(anim_canvas, PB_LAMP_X - PB_LANTERN_HW - 1,
                            PB_LANTERN_Y - PB_LANTERN_HH - 1,
                            (PB_LANTERN_HW + 1) * 2 + 1, (PB_LANTERN_HH + 1) * 2 + 1, &hd);
    }

    // ── SHOOTING STAR: advance + draw (rare; 5-segment fading tail) ───────────
    if (!pb_shoot_active && millis() >= pb_shoot_next_ms) {
        pb_shoot_x  = (float)random(-30, SCR_W / 2);
        pb_shoot_y  = (float)random(4, PB_HORIZON / 2);
        float sp    = 6.0f + random(0, 30) * 0.1f;
        float ang   = 0.35f + random(0, 40) * 0.005f;
        pb_shoot_vx = sp * cosf(ang);
        pb_shoot_vy = sp * sinf(ang);
        pb_shoot_active = true;
    }
    if (pb_shoot_active) {
        pb_shoot_x += pb_shoot_vx;
        pb_shoot_y += pb_shoot_vy;
        if (pb_shoot_x > SCR_W + 24 || pb_shoot_y > PB_HORIZON) {
            pb_shoot_active  = false;
            pb_shoot_next_ms = millis() + (unsigned long)random(20000, 90001);
        } else {
            lv_draw_line_dsc_t ld;
            lv_draw_line_dsc_init(&ld);
            int minx = SCR_W, miny = SCR_H, maxx = 0, maxy = 0;
            for (int s = 0; s < 5; s++) {
                lv_point_t pts[2];
                pts[0].x = (lv_coord_t)(pb_shoot_x - pb_shoot_vx * s);
                pts[0].y = (lv_coord_t)(pb_shoot_y - pb_shoot_vy * s);
                pts[1].x = (lv_coord_t)(pb_shoot_x - pb_shoot_vx * (s + 1));
                pts[1].y = (lv_coord_t)(pb_shoot_y - pb_shoot_vy * (s + 1));
                uint8_t v = (uint8_t)(230 - s * 45);
                ld.color = lv_color_make(v, v, 255);
                ld.width = (s < 2) ? 2 : 1;
                ld.opa   = (lv_opa_t)(255 - s * 45);
                lv_canvas_draw_line(anim_canvas, pts, 2, &ld);
                for (int k = 0; k < 2; k++) {
                    if (pts[k].x < minx) minx = pts[k].x;
                    if (pts[k].y < miny) miny = pts[k].y;
                    if (pts[k].x > maxx) maxx = pts[k].x;
                    if (pts[k].y > maxy) maxy = pts[k].y;
                }
            }
            pb_shoot_x1 = minx - 2; pb_shoot_y1 = miny - 2;   // padded bbox for erase
            pb_shoot_x2 = maxx + 2; pb_shoot_y2 = maxy + 2;
        }
    }

    // ── CRAB + FOOTPRINTS ─────────────────────────────────────────────────────
    crab_state.walk_tick++;
    if (crab_state.walk_tick >= 8) {
        crab_state.walk_tick  = 0;
        crab_state.walk_frame ^= 1;
    }

    // Blink
    if (crab_state.blinking) {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        if (crab_state.blink_cd == 0) {
            crab_state.blinking = false;
            crab_state.blink_cd = (uint16_t)random(100, 220);
        }
    } else {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        else { crab_state.blinking = true; crab_state.blink_cd = 3; }
    }

    // Celebrate
    bool celebrating = false;
    if (crab_state.celebrate_fr > 0) {
        crab_state.celebrate_fr--;
        celebrating = true;
    } else if (crab_state.celebrate_cd > 0) {
        crab_state.celebrate_cd--;
    } else {
        crab_state.celebrate_fr = 24;
        crab_state.celebrate_cd = (uint16_t)random(160, 240);
    }

    int ocx = (int)crab_state.x;
    int ocy = pixbeach_floor_y(ocx) - 8;
    int ox1, oy1, ox2, oy2;
    crab_bbox_at(ocx, ocy, &ox1, &oy1, &ox2, &oy2);

    crab_state.x += crab_state.vx;
    if (crab_state.x < 48.0f) {
        crab_state.vx = fabsf(crab_state.vx); crab_state.right = true;
    }
    if (crab_state.x > (float)(SCR_W - 48)) {
        crab_state.vx = -fabsf(crab_state.vx); crab_state.right = false;
    }

    int ncx = (int)crab_state.x;
    int ncy = pixbeach_floor_y(ncx) - 8;
    int nx1, ny1, nx2, ny2;
    crab_bbox_at(ncx, ncy, &nx1, &ny1, &nx2, &ny2);

    bg_restore((ox1 < nx1 ? ox1 : nx1), (oy1 < ny1 ? oy1 : ny1),
               (ox2 > nx2 ? ox2 : nx2), (oy2 > ny2 ? oy2 : ny2));

    // Drop a footprint every 8 frames in the sand zone
    pb_print_tick++;
    if (pb_print_tick >= 8) {
        pb_print_tick = 0;
        int py = ncy + 14;   // bottom of crab sprite
        if (py >= PB_SAND_TOP && py < SCR_H - 4) {
            // Draw print into live canvas buffer (not anim_bg — wave overwrites)
            lv_draw_rect_dsc_t pd;
            lv_draw_rect_dsc_init(&pd);
            pd.radius = 0; pd.border_width = 0;
            pd.bg_color = lv_color_hex(0x2A1E14);
            pd.bg_opa   = LV_OPA_70;
            lv_canvas_draw_rect(anim_canvas, ncx - 2, py, 2, 2, &pd);
            lv_canvas_draw_rect(anim_canvas, ncx + 2, py, 2, 2, &pd);
            pb_prints[pb_print_head].x = (int16_t)ncx;
            pb_prints[pb_print_head].y = (int16_t)py;
            pb_print_head = (pb_print_head + 1) % PB_PRINT_MAX;
            if (pb_print_count < PB_PRINT_MAX) pb_print_count++;
        }
    }

    draw_crab(anim_canvas, ncx, ncy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame, celebrating);

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// ROLLING GRASSLAND (dawn meadow)
// ══════════════════════════════════════════════════════════════════════════════
// A calm sunrise meadow: layered rolling hills under a soft dawn sky, a lone tree,
// a warm rising sun, drifting clouds, swaying grass, fluttering butterflies and a
// couple of distant birds. The pixel crab walks the foreground. Built on the same
// bake-once (anim_bg) + per-frame-sprite (bg_restore) engine as the other scenes.
#define GR_HILL_TOP    138   // y where the farthest hills meet the sky
#define GR_SUN_X       360   // rising-sun center x (right of center)
#define GR_SUN_Y        66   // sun center y (low in the dawn sky)
#define GR_SUN_R        18   // sun disc radius

// Foreground meadow surface where the crab walks (gentle roll).
static inline int grass_floor_y(int x) {
    return (int)(SCR_H - 40
                 + 4.0f * sinf(x * 0.016f)
                 + 2.0f * sinf(x * 0.06f + 1.1f));
}

// Drifting clouds (slow, wrap around). w = size variant 0..2.
#define GR_CLOUD_COUNT 3
static struct { float x; int16_t y; float vx; uint8_t w; } gr_cloud[GR_CLOUD_COUNT];

// Swaying grass tufts along the very foreground (below the crab).
#define GR_TUFT_COUNT 14
static struct { int16_t x, base; uint8_t phase; } gr_tuft[GR_TUFT_COUNT];
static uint8_t gr_anim_tick = 0;   // shared tick for sway / flutter

// Fluttering butterflies wandering over the meadow.
#define GR_BFLY_COUNT 3
static struct { float x, y; uint8_t phase, wing; } gr_bfly[GR_BFLY_COUNT];

// A couple of distant birds drifting across the sky.
#define GR_BIRD_COUNT 2
static struct { float x; int16_t y; float vx; } gr_bird[GR_BIRD_COUNT];

// Hill bands (back→front). Front band fills down to the bottom of the screen.
// Shared by grass_bg_render() and the lone-tree placement so the tree sits on the
// mid hill's surface exactly.
static const struct { int base, amp; float fr, ph, r, g, b; } GR_BAND[3] = {
    { 150, 12, 0.013f, 0.0f, 132.0f, 168.0f, 96.0f },  // far hazy green
    { 188, 18, 0.011f, 2.1f,  92.0f, 144.0f, 70.0f },  // mid green
    { 224, 16, 0.009f, 4.3f,  60.0f, 112.0f, 48.0f },  // near green (to bottom)
};

// Surface y of hill band b at column x (where trees/bushes sit on that hill).
static inline int gr_band_surface(int b, int x) {
    return GR_BAND[b].base - (int)(GR_BAND[b].amp * sinf(x * GR_BAND[b].fr + GR_BAND[b].ph));
}

// Bakes a simple pixel tree (trunk + rounded canopy) into anim_bg. Trunk foot sits
// at (tx, base_y); cr = canopy radius (smaller = more distant). Canopy uses
// overlapping aqua_blot blobs (green-on-green blends cleanly, no banding).
static void grass_bake_tree(int tx, int base_y, int cr) {
    int th = cr + 10;                                  // trunk height grows with canopy
    int tw = (cr >= 12) ? 2 : 1;                       // trunk half-width
    for (int ty = base_y - th; ty <= base_y; ty++)
        for (int txx = tx - tw; txx <= tx + tw - 1; txx++)
            if (txx >= 0 && txx < SCR_W && ty >= 0 && ty < SCR_H)
                put_dith(anim_bg, txx, ty, 74.0f, 52.0f, 32.0f);
    int cy = base_y - th + 2;                           // canopy center
    aqua_blot(tx,                 cy,                cr,             48.0f, 104.0f, 44.0f);
    aqua_blot(tx - (cr * 7) / 10, cy + cr / 3,       (cr * 7) / 10,  44.0f,  96.0f, 40.0f);
    aqua_blot(tx + (cr * 7) / 10, cy + cr / 3,       (cr * 7) / 10,  44.0f,  96.0f, 40.0f);
    aqua_blot(tx - cr / 3,        cy - (cr * 4) / 10, (cr * 11) / 20, 70.0f, 132.0f, 58.0f); // sunlit top
}

// Bake the static meadow background into anim_bg.
static void grass_bg_render() {
    if (!anim_bg) return;

    // ── SKY + base meadow fill ────────────────────────────────────────────────
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            float r, g, b;
            if (y < GR_HILL_TOP) {
                // Dawn sky: soft blue at top → warm cream near the hilltops.
                float f = (float)y / GR_HILL_TOP;            // 0 (top) .. 1 (horizon)
                r = 116.0f + f * 140.0f;
                g = 158.0f + f *  74.0f;
                b = 206.0f - f *  44.0f;
            } else {
                // Base meadow green (the hill bands paint over this next).
                r = 70.0f; g = 120.0f; b = 56.0f;
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // ── RISING SUN: clean solid orange disc with a soft warm glow ─────────────
    // A simple, crisp orange sun (no layered haze rings — the old stacked blots
    // banded into a muddy "ball of yarn"). The glow fades smoothly into the dawn
    // sky by blending toward the exact sky color at each row, so there is no ring.
    {
        int sr   = GR_SUN_R;          // solid disc radius
        int gr_o = GR_SUN_R + 14;     // outer glow radius
        for (int dy = -gr_o; dy <= gr_o; dy++) {
            int y = GR_SUN_Y + dy;
            if (y < 0 || y >= SCR_H) continue;
            // Sky base color at this row (same formula as the sky fill above).
            float f   = (float)y / GR_HILL_TOP;
            float sky_r = 116.0f + f * 140.0f;
            float sky_g = 158.0f + f *  74.0f;
            float sky_b = 206.0f - f *  44.0f;
            for (int dx = -gr_o; dx <= gr_o; dx++) {
                int x = GR_SUN_X + dx;
                if (x < 0 || x >= SCR_W) continue;
                float d = sqrtf((float)(dx * dx + dy * dy));
                if (d <= sr) {
                    // Solid orange disc, a touch brighter toward the center.
                    float c = 1.0f - d / (float)(sr + 1);   // 1 center .. 0 rim
                    put_dith(anim_bg, x, y,
                             250.0f + 5.0f * c, 140.0f + 40.0f * c, 40.0f + 30.0f * c);
                } else if (d <= gr_o) {
                    // Smooth glow: fade a warm orange tint into the sky.
                    float t = (d - sr) / (float)(gr_o - sr);    // 0 rim .. 1 out
                    float k = (1.0f - t) * (1.0f - t) * 0.85f;  // glow strength
                    put_dith(anim_bg, x, y,
                             sky_r + (255.0f - sky_r) * k,
                             sky_g + (170.0f - sky_g) * k,
                             sky_b + ( 60.0f - sky_b) * k);
                }
            }
        }
    }

    // ── ROLLING HILLS (3 layered sine bands, back→front) ──────────────────────
    for (int hb = 0; hb < 3; hb++) {
        for (int x = 0; x < SCR_W; x++) {
            int top = GR_BAND[hb].base
                      - (int)(GR_BAND[hb].amp * sinf(x * GR_BAND[hb].fr + GR_BAND[hb].ph));
            for (int y = top; y < SCR_H; y++)
                if (y >= 0) put_dith(anim_bg, x, y, GR_BAND[hb].r, GR_BAND[hb].g, GR_BAND[hb].b);
        }
    }

    // ── TREES (a few, varied sizes, each sitting on its hill surface) ─────────
    grass_bake_tree( 96, gr_band_surface(1,  96) - 1, 14);   // mid (original spot)
    grass_bake_tree(178, gr_band_surface(2, 178) - 1, 18);   // near, larger foreground
    grass_bake_tree(300, gr_band_surface(1, 300) - 1, 12);   // mid
    grass_bake_tree(248, gr_band_surface(0, 248) - 1,  8);   // far, small + hazy
    grass_bake_tree(432, gr_band_surface(0, 432) - 1,  7);   // far, small

    // ── BUSHES / SHRUBS (low rounded blobs nestled on the hills) ──────────────
    static const int GR_BUSH[][3] = {   // x, band, radius
        { 140, 2, 6 }, { 352, 2, 7 }, { 60, 1, 5 }, { 402, 1, 5 }, { 212, 1, 4 }
    };
    for (int i = 0; i < 5; i++) {
        int bx = GR_BUSH[i][0], bb = GR_BUSH[i][1], br = GR_BUSH[i][2];
        int by = gr_band_surface(bb, bx) - br / 2;
        aqua_blot(bx,      by, br,             46.0f, 98.0f, 42.0f);
        aqua_blot(bx - br, by, (br * 2) / 3,   42.0f, 90.0f, 38.0f);
        aqua_blot(bx + br, by, (br * 2) / 3,   42.0f, 90.0f, 38.0f);
    }

    // ── ROCKS / BOULDERS (grey blots on the near meadow) ──────────────────────
    static const int GR_ROCK[][2] = { { 120, 250 }, { 330, 272 }, { 268, 300 } };
    for (int i = 0; i < 3; i++) {
        int rx = GR_ROCK[i][0], ry = GR_ROCK[i][1];
        aqua_blot(rx,     ry,     3, 120.0f, 120.0f, 116.0f);          // body
        aqua_blot(rx - 2, ry + 1, 2, 100.0f, 100.0f,  96.0f);          // shadow side
        put_dith(anim_bg, rx - 1, ry - 2, 156.0f, 156.0f, 150.0f);     // lit top edge
    }

    // ── MEADOW TEXTURE: grass flecks + a few tiny flowers (front band only) ───
    for (int p = 0; p < 120; p++) {
        int px = random(0, SCR_W);
        int py = random(GR_BAND[2].base - 8, SCR_H);
        if (py < 0 || py >= SCR_H) continue;
        if (random(0, 10) == 0) {
            // tiny flower: white or yellow speck
            uint32_t fc = random(0, 2) ? 0xF4F4E0 : 0xF2D24A;
            put_dith(anim_bg, px, py,
                     (float)((fc >> 16) & 0xFF), (float)((fc >> 8) & 0xFF), (float)(fc & 0xFF));
        } else {
            float v = random(0, 2) ? 1.18f : 0.82f;       // lighter / darker green fleck
            put_dith(anim_bg, px, py, 60.0f * v, 112.0f * v, 48.0f * v);
        }
    }

    // ── FLOWER CLUSTERS (a few stemmed blooms dotted across the foreground) ───
    static const int GR_FLOWER[][3] = {   // x, y, color (0 = pink, 1 = yellow)
        { 78, 300, 0 }, { 250, 286, 1 }, { 360, 306, 0 }, { 150, 312, 1 }, { 428, 296, 0 }
    };
    for (int i = 0; i < 5; i++) {
        int fx = GR_FLOWER[i][0], fy = GR_FLOWER[i][1];
        uint32_t pc = GR_FLOWER[i][2] ? 0xF2D24A : 0xF06A8A;   // yellow or pink petals
        float pr = (float)((pc >> 16) & 0xFF), pg = (float)((pc >> 8) & 0xFF), pb = (float)(pc & 0xFF);
        // short green stem
        for (int s = 1; s <= 4; s++)
            if (fy + s < SCR_H) put_dith(anim_bg, fx, fy + s, 52.0f, 100.0f, 44.0f);
        // 4-petal bloom + sunny center
        put_dith(anim_bg, fx,     fy - 1, pr, pg, pb);
        put_dith(anim_bg, fx - 1, fy,     pr, pg, pb);
        put_dith(anim_bg, fx + 1, fy,     pr, pg, pb);
        put_dith(anim_bg, fx,     fy + 1, pr, pg, pb);
        put_dith(anim_bg, fx,     fy,     250.0f, 230.0f, 90.0f);
    }

    // ── GLOBAL DIM: knock the whole baked scene back ~15% for a softer mood ────
    // One uniform pass over the finished background so sky, sun, hills, trees,
    // bushes, rocks and flowers all darken together (the crab and other live
    // sprites keep their normal colors). 0.85 = 15% darker.
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        lv_color32_t c;
        c.full = lv_color_to32(anim_bg[i]);
        anim_bg[i] = lv_color_make((uint8_t)(c.ch.red   * 0.85f),
                                   (uint8_t)(c.ch.green * 0.85f),
                                   (uint8_t)(c.ch.blue  * 0.85f));
    }
}

static void grass_init() {
    gr_anim_tick = 0;

    // Clouds spread across the sky, slow rightward drift.
    for (int i = 0; i < GR_CLOUD_COUNT; i++) {
        gr_cloud[i].x  = (float)random(0, SCR_W);
        gr_cloud[i].y  = (int16_t)random(18, GR_HILL_TOP - 40);
        gr_cloud[i].vx = 0.20f + random(0, 25) * 0.01f;   // 0.20..0.45 px/frame
        gr_cloud[i].w  = (uint8_t)random(0, 3);
    }

    // Grass tufts spaced across the foreground, each with its own sway phase.
    for (int i = 0; i < GR_TUFT_COUNT; i++) {
        int gx = 16 + i * ((SCR_W - 32) / (GR_TUFT_COUNT - 1));
        gr_tuft[i].x     = (int16_t)gx;
        gr_tuft[i].base  = (int16_t)(SCR_H - 4 - random(0, 6));   // very foreground
        gr_tuft[i].phase = (uint8_t)random(0, 255);
    }

    // Butterflies wandering over the meadow.
    for (int i = 0; i < GR_BFLY_COUNT; i++) {
        gr_bfly[i].x     = (float)random(40, SCR_W - 40);
        gr_bfly[i].y     = (float)random(GR_HILL_TOP - 6, SCR_H - 70);
        gr_bfly[i].phase = (uint8_t)random(0, 255);
        gr_bfly[i].wing  = 0;
    }

    // Distant birds drifting across the sky.
    for (int i = 0; i < GR_BIRD_COUNT; i++) {
        gr_bird[i].x  = (float)random(0, SCR_W);
        gr_bird[i].y  = (int16_t)random(24, 70);
        gr_bird[i].vx = 0.5f + random(0, 8) * 0.1f;
    }

    crab_state.x            = SCR_W * 0.4f;
    crab_state.vx           = 1.2f;
    crab_state.right        = true;
    crab_state.walk_frame   = 0;
    crab_state.walk_tick    = 0;
    crab_state.blink_cd     = (uint16_t)random(100, 220);
    crab_state.blinking     = false;
    crab_state.celebrate_cd = (uint16_t)random(160, 240);
    crab_state.celebrate_fr = 0;
}

// Draws a soft pixel cloud (a body puff + two side lobes) at (cx,cy) onto the canvas.
static void grass_draw_cloud(int cx, int cy, int wv) {
    lv_draw_rect_dsc_t cd;
    lv_draw_rect_dsc_init(&cd);
    cd.radius = LV_RADIUS_CIRCLE; cd.border_width = 0;
    int w = 16 + wv * 5;
    cd.bg_color = lv_color_hex(0xF4F6FA);
    cd.bg_opa   = LV_OPA_70;
    lv_canvas_draw_rect(anim_canvas, cx - w / 2,     cy - 4, w,  10, &cd);  // body
    cd.bg_opa = LV_OPA_60;
    lv_canvas_draw_rect(anim_canvas, cx - w / 2 - 6, cy - 1, 12,  8, &cd);  // left lobe
    lv_canvas_draw_rect(anim_canvas, cx + w / 2 - 6, cy - 6, 14, 12, &cd);  // right lobe
}

// Draws a swaying grass tuft (5 blades; tips lean with the shared sway tick).
static void grass_draw_tuft(int gx, int base, uint8_t phase, uint8_t tick) {
    float s = sinf((tick + phase) * 0.12f);     // -1..1 lean
    lv_draw_line_dsc_t bd;
    lv_draw_line_dsc_init(&bd);
    bd.width = 1;
    bd.color = lv_color_make(61, 112, 48);   // ~15% dimmed to match the darkened meadow
    for (int blade = -2; blade <= 2; blade++) {
        int absb  = blade < 0 ? -blade : blade;
        int h     = 10 - absb * 2;                       // center blade tallest
        int tipdx = (int)(s * (3 + absb));               // outer blades lean more
        lv_point_t pts[2];
        pts[0].x = (lv_coord_t)(gx + blade * 2);          pts[0].y = (lv_coord_t)base;
        pts[1].x = (lv_coord_t)(gx + blade * 2 + tipdx);  pts[1].y = (lv_coord_t)(base - h);
        lv_canvas_draw_line(anim_canvas, pts, 2, &bd);
    }
}

// Draws a tiny butterfly (dark body + two warm wings whose spread flaps with `wing`).
static void grass_draw_bfly(int cx, int cy, uint8_t wing) {
    lv_draw_rect_dsc_t wd;
    lv_draw_rect_dsc_init(&wd);
    wd.radius = 0; wd.border_width = 0;
    wd.bg_color = lv_color_hex(0x2A1A0E);                 // body
    lv_canvas_draw_rect(anim_canvas, cx, cy, 1, 3, &wd);
    wd.bg_color = lv_color_hex(0xF2A23A);                 // warm orange wings
    int sp = wing ? 1 : 2;                                // flap spread
    lv_canvas_draw_rect(anim_canvas, cx - sp - 1, cy, sp + 1, 2, &wd);  // left
    lv_canvas_draw_rect(anim_canvas, cx + 1,      cy, sp + 1, 2, &wd);  // right
}

static void grass_timer_cb(lv_timer_t*) {
    if (!anim_canvas || !anim_bg) return;
    wdt_feed();
    gr_anim_tick++;

    // ── CLOUDS (drift right, wrap) ────────────────────────────────────────────
    for (int i = 0; i < GR_CLOUD_COUNT; i++) {
        int w  = 16 + gr_cloud[i].w * 5;
        int cx = (int)gr_cloud[i].x, cy = gr_cloud[i].y;
        bg_restore(cx - w / 2 - 8, cy - 8, cx + w / 2 + 9, cy + 8);   // erase old
        gr_cloud[i].x += gr_cloud[i].vx;
        if (gr_cloud[i].x > SCR_W + w) gr_cloud[i].x = (float)(-w);
        grass_draw_cloud((int)gr_cloud[i].x, cy, gr_cloud[i].w);
    }

    // ── DISTANT BIRDS (slow v-shapes) ─────────────────────────────────────────
    {
        lv_draw_line_dsc_t bd;
        lv_draw_line_dsc_init(&bd);
        bd.width = 1; bd.color = lv_color_hex(0x3A3A46);
        for (int i = 0; i < GR_BIRD_COUNT; i++) {
            int bx = (int)gr_bird[i].x, by = gr_bird[i].y;
            bg_restore(bx - 5, by - 3, bx + 5, by + 3);
            gr_bird[i].x += gr_bird[i].vx;
            if (gr_bird[i].x > SCR_W + 6) { gr_bird[i].x = -6.0f; gr_bird[i].y = (int16_t)random(24, 70); }
            int nbx = (int)gr_bird[i].x;
            lv_point_t p1[2] = { { (lv_coord_t)(nbx - 4), (lv_coord_t)(by + 2) },
                                 { (lv_coord_t)nbx,       (lv_coord_t)(by - 1) } };
            lv_point_t p2[2] = { { (lv_coord_t)nbx,       (lv_coord_t)(by - 1) },
                                 { (lv_coord_t)(nbx + 4), (lv_coord_t)(by + 2) } };
            lv_canvas_draw_line(anim_canvas, p1, 2, &bd);
            lv_canvas_draw_line(anim_canvas, p2, 2, &bd);
        }
    }

    // ── BUTTERFLIES (flutter + bob) ───────────────────────────────────────────
    for (int i = 0; i < GR_BFLY_COUNT; i++) {
        int ox = (int)gr_bfly[i].x, oy = (int)gr_bfly[i].y;
        bg_restore(ox - 5, oy - 2, ox + 5, oy + 5);
        gr_bfly[i].x += 0.6f * sinf((gr_anim_tick + gr_bfly[i].phase) * 0.05f) + 0.3f;
        gr_bfly[i].y += 0.8f * sinf((gr_anim_tick * 2 + gr_bfly[i].phase) * 0.09f);
        if (gr_bfly[i].x > SCR_W - 6)          gr_bfly[i].x = 6.0f;
        if (gr_bfly[i].y < GR_HILL_TOP - 10)   gr_bfly[i].y = (float)(GR_HILL_TOP - 10);
        if (gr_bfly[i].y > SCR_H - 60)         gr_bfly[i].y = (float)(SCR_H - 60);
        gr_bfly[i].wing ^= 1;
        grass_draw_bfly((int)gr_bfly[i].x, (int)gr_bfly[i].y, gr_bfly[i].wing);
    }

    // ── SWAYING GRASS TUFTS (very foreground, below the crab) ─────────────────
    for (int i = 0; i < GR_TUFT_COUNT; i++) {
        int gx = gr_tuft[i].x, gb = gr_tuft[i].base;
        bg_restore(gx - 10, gb - 13, gx + 10, gb + 1);
        grass_draw_tuft(gx, gb, gr_tuft[i].phase, gr_anim_tick);
    }

    // ── CRAB (walks the foreground; drawn last so it stays in front) ──────────
    crab_state.walk_tick++;
    if (crab_state.walk_tick >= 8) { crab_state.walk_tick = 0; crab_state.walk_frame ^= 1; }

    if (crab_state.blinking) {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        if (crab_state.blink_cd == 0) {
            crab_state.blinking = false; crab_state.blink_cd = (uint16_t)random(100, 220);
        }
    } else {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        else { crab_state.blinking = true; crab_state.blink_cd = 3; }
    }

    bool celebrating = false;
    if (crab_state.celebrate_fr > 0)      { crab_state.celebrate_fr--; celebrating = true; }
    else if (crab_state.celebrate_cd > 0) { crab_state.celebrate_cd--; }
    else { crab_state.celebrate_fr = 24; crab_state.celebrate_cd = (uint16_t)random(160, 240); }

    int ocx = (int)crab_state.x;
    int ocy = grass_floor_y(ocx) - 8;
    int ox1, oy1, ox2, oy2;
    crab_bbox_at(ocx, ocy, &ox1, &oy1, &ox2, &oy2);

    crab_state.x += crab_state.vx;
    if (crab_state.x < 48.0f)               { crab_state.vx =  fabsf(crab_state.vx); crab_state.right = true;  }
    if (crab_state.x > (float)(SCR_W - 48)) { crab_state.vx = -fabsf(crab_state.vx); crab_state.right = false; }

    int ncx = (int)crab_state.x;
    int ncy = grass_floor_y(ncx) - 8;
    int nx1, ny1, nx2, ny2;
    crab_bbox_at(ncx, ncy, &nx1, &ny1, &nx2, &ny2);

    bg_restore((ox1 < nx1 ? ox1 : nx1), (oy1 < ny1 ? oy1 : ny1),
               (ox2 > nx2 ? ox2 : nx2), (oy2 > ny2 ? oy2 : ny2));

    draw_crab(anim_canvas, ncx, ncy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame, celebrating);

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// COUNTDOWN CLOCK
// ══════════════════════════════════════════════════════════════════════════════
#define CD_CRAB_H 50   // pixel height of the crab canvas strip at screen bottom

static lv_obj_t*   cd_scr         = nullptr;
static lv_obj_t*   cd_lbl_clock   = nullptr;   // current local time (big)
static lv_obj_t*   cd_lbl_date    = nullptr;   // date line
static lv_obj_t*   cd_lbl_time    = nullptr;   // countdown digits
static lv_obj_t*   cd_crab_canvas = nullptr;
static lv_color_t* cd_crab_buf    = nullptr;
static lv_timer_t* cd_crab_timer  = nullptr;

// Erases the crab ghost on the countdown canvas by filling the bbox with
// the Deep Space background color. Much cheaper than a full canvas fill.
static void cd_crab_erase(int x1, int y1, int x2, int y2) {
    if (!cd_crab_canvas) return;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= SCR_W)    x2 = SCR_W - 1;
    if (y2 >= CD_CRAB_H) y2 = CD_CRAB_H - 1;
    if (x1 > x2 || y1 > y2) return;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.border_width = 0;
    rd.radius       = 0;
    rd.bg_color     = lv_color_hex(0x0E1117);
    rd.bg_opa       = LV_OPA_COVER;
    lv_canvas_draw_rect(cd_crab_canvas, x1, y1, x2 - x1 + 1, y2 - y1 + 1, &rd);
}

// Crab walk animation for the countdown screen (fires every 120ms).
// Uses the shared crab_state struct — countdown and tidepool never run together.
static void countdown_crab_cb(lv_timer_t*) {
    if (!cd_crab_canvas) return;
    wdt_feed();

    // Walk-frame tick
    crab_state.walk_tick++;
    if (crab_state.walk_tick >= 8) {
        crab_state.walk_tick  = 0;
        crab_state.walk_frame ^= 1;
    }

    // Blink state machine
    if (crab_state.blinking) {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        if (crab_state.blink_cd == 0) {
            crab_state.blinking = false;
            crab_state.blink_cd = (uint16_t)random(80, 200);
        }
    } else {
        if (crab_state.blink_cd > 0) crab_state.blink_cd--;
        else { crab_state.blinking = true; crab_state.blink_cd = 3; }
    }

    // Fixed y within the 50px canvas: shell center sits 23px from bottom
    const int cy = CD_CRAB_H - 23;

    // Old bbox (before move)
    int ocx = (int)crab_state.x;
    int ox1, oy1, ox2, oy2;
    crab_bbox_at(ocx, cy, &ox1, &oy1, &ox2, &oy2);

    // Advance and bounce
    crab_state.x += crab_state.vx;
    if (crab_state.x < 48.0f) {
        crab_state.vx    =  fabsf(crab_state.vx);
        crab_state.right = true;
    }
    if (crab_state.x > (float)(SCR_W - 48)) {
        crab_state.vx    = -fabsf(crab_state.vx);
        crab_state.right = false;
    }

    // New bbox
    int ncx = (int)crab_state.x;
    int nx1, ny1, nx2, ny2;
    crab_bbox_at(ncx, cy, &nx1, &ny1, &nx2, &ny2);

    // Erase ghost and redraw; raise claws in the final 30 s before open
    bool cd_celebrate = (countdown <= 30);
    cd_crab_erase(
        (ox1 < nx1 ? ox1 : nx1), (oy1 < ny1 ? oy1 : ny1),
        (ox2 > nx2 ? ox2 : nx2), (oy2 > ny2 ? oy2 : ny2));
    draw_crab(cd_crab_canvas, ncx, cy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame, cd_celebrate);
    lv_obj_invalidate(cd_crab_canvas);
}

static void countdown_build() {
    cd_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cd_scr, lv_color_hex(0x0E1117), LV_PART_MAIN);

    // Large current-time clock (dominant element, top section)
    cd_lbl_clock = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(cd_lbl_clock, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(cd_lbl_clock, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
    lv_label_set_text(cd_lbl_clock, "--:--:--");
    lv_obj_align(cd_lbl_clock, LV_ALIGN_TOP_MID, 0, 45);

    // Date line below the clock
    cd_lbl_date = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(cd_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cd_lbl_date, lv_color_hex(0x7A8290), LV_PART_MAIN);
    lv_label_set_text(cd_lbl_date, "---");
    lv_obj_align(cd_lbl_date, LV_ALIGN_TOP_MID, 0, 109);

    // "Market opens in" subtitle
    lv_obj_t* sub = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7A8290), LV_PART_MAIN);
    lv_label_set_text(sub, "Market opens in");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 143);

    // Countdown digits — smaller than the clock, shifts color as open approaches
    cd_lbl_time = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(cd_lbl_time, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(cd_lbl_time, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
    lv_label_set_text(cd_lbl_time, "--:--:--");
    lv_obj_align(cd_lbl_time, LV_ALIGN_TOP_MID, 0, 173);

    // Market label below countdown
    lv_obj_t* mkt = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(mkt, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(mkt, lv_color_hex(0x22C55E), LV_PART_MAIN);
    lv_label_set_text(mkt, "NYSE / NASDAQ  9:30 AM ET");
    lv_obj_align(mkt, LV_ALIGN_TOP_MID, 0, 221);

    // Crab walk canvas strip at the bottom of the screen
    cd_crab_buf = (lv_color_t*)heap_caps_malloc(
        (size_t)SCR_W * CD_CRAB_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (cd_crab_buf) {
        cd_crab_canvas = lv_canvas_create(cd_scr);
        lv_canvas_set_buffer(cd_crab_canvas, cd_crab_buf,
                             SCR_W, CD_CRAB_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(cd_crab_canvas, 0, SCR_H - CD_CRAB_H);
        lv_canvas_fill_bg(cd_crab_canvas, lv_color_hex(0x0E1117), LV_OPA_COVER);

        // Init crab state (shared struct — tidepool never runs at same time)
        crab_state.x            = SCR_W * 0.35f;
        crab_state.vx           = 1.1f;
        crab_state.right        = true;
        crab_state.walk_frame   = 0;
        crab_state.walk_tick    = 0;
        crab_state.blink_cd     = (uint16_t)random(80, 200);
        crab_state.blinking     = false;
        crab_state.celebrate_cd = 0;
        crab_state.celebrate_fr = 0;
    }

    lv_scr_load(cd_scr);
}

static void countdown_tick_cb(lv_timer_t*) {
    if (!cd_lbl_time) return;
    wdt_feed();

    // ── Current clock + date (user's timezone via configTzTime already applied)
    time_t now_t = time(nullptr);
    struct tm lt;
    localtime_r(&now_t, &lt);

    if (lt.tm_year > 100) {   // NTP synced: year > 2000
        char clock_buf[12];
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d:%02d",
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
        if (cd_lbl_clock) lv_label_set_text(cd_lbl_clock, clock_buf);

        if (cd_lbl_date) {
            char date_buf[20];
            // strftime would need locale; use manual day/month arrays instead
            static const char* const WDAY[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            static const char* const MON[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
            snprintf(date_buf, sizeof(date_buf), "%s, %s %d",
                     WDAY[lt.tm_wday], MON[lt.tm_mon], lt.tm_mday);
            lv_label_set_text(cd_lbl_date, date_buf);
        }
    } else {
        if (cd_lbl_clock) lv_label_set_text(cd_lbl_clock, "--:--:--");
        if (cd_lbl_date)  lv_label_set_text(cd_lbl_date,  "---");
    }

    // ── Countdown (decrement every second)
    if (countdown > 0) countdown--;
    uint32_t s  = countdown;
    uint32_t h  = s / 3600;
    uint32_t m  = (s % 3600) / 60;
    uint32_t sc = s % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, sc);
    lv_label_set_text(cd_lbl_time, buf);

    // Digit color shifts Pearl → Sand Amber → Bull Green as open approaches
    lv_color_t digit_col;
    if (s > 1800)      digit_col = lv_color_hex(0xE6E9EF);  // Pearl: >30 min
    else if (s > 300)  digit_col = lv_color_hex(0xF59E0B);  // Sand Amber: 5-30 min
    else               digit_col = lv_color_hex(0x22C55E);  // Bull Green: <5 min
    lv_obj_set_style_text_color(cd_lbl_time, digit_col, LV_PART_MAIN);
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════
void anim_start(int type, uint32_t secs_to_open) {
    cur_type  = type;
    countdown = secs_to_open;

    if (type == ANIM_COUNTDOWN) {
        countdown_build();
        lv_obj_add_event_cb(cd_scr, anim_gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(cd_scr, anim_tap_cb,    LV_EVENT_CLICKED, NULL);
        anim_settings_req = 0;
        anim_tap_count    = 0;
        anim_last_tap_ms  = 0;
        anim_timer      = lv_timer_create(countdown_tick_cb,  1000, nullptr);
        // 160 ms (~6 fps): with full_refresh=1 every frame is a full-screen QSPI
        // flush, so a slower tick lowers sustained DMA pressure and the rate of the
        // phase=7 panel hang (see BISECT_LOG.md). 120 ms rebooted ~hourly.
        cd_crab_timer   = lv_timer_create(countdown_crab_cb,  160,  nullptr);
        return;
    }

    anim_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(anim_scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(anim_scr, 0, LV_PART_MAIN);

    anim_buf = (lv_color_t*)heap_caps_malloc(SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!anim_buf) { lv_obj_del(anim_scr); anim_scr = nullptr; return; }

    anim_canvas = lv_canvas_create(anim_scr);
    lv_canvas_set_buffer(anim_canvas, anim_buf, SCR_W, SCR_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(anim_canvas, 0, 0);

    switch (type) {
        case ANIM_STARFIELD: {
            // No anim_bg needed: star_draw() fills sky + all stars each frame,
            // which is cheap and naturally erases the previous shooting star tail.
            star_init();
            star_draw();
            // Centered real-time clock + date overlay. Labels are children of
            // anim_scr, composited on top of the star canvas by LVGL, and freed
            // when anim_scr is deleted in anim_stop().
            sf_last_sec  = -1;
            sf_lbl_clock = lv_label_create(anim_scr);
            lv_obj_set_style_text_font(sf_lbl_clock, &lv_font_montserrat_48, LV_PART_MAIN);
            lv_obj_set_style_text_color(sf_lbl_clock, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
            lv_obj_set_style_bg_color(sf_lbl_clock, lv_color_hex(0x05070F), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sf_lbl_clock, LV_OPA_40, LV_PART_MAIN);  // faint backdrop
            lv_obj_set_style_radius(sf_lbl_clock, 8, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sf_lbl_clock, 8, LV_PART_MAIN);
            lv_label_set_text(sf_lbl_clock, "--:--:--");
            lv_obj_align(sf_lbl_clock, LV_ALIGN_CENTER, 0, -10);

            sf_lbl_date = lv_label_create(anim_scr);
            lv_obj_set_style_text_font(sf_lbl_date, &lv_font_montserrat_20, LV_PART_MAIN);
            lv_obj_set_style_text_color(sf_lbl_date, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
            lv_label_set_text(sf_lbl_date, "---");
            lv_obj_align_to(sf_lbl_date, sf_lbl_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

            anim_timer = lv_timer_create(star_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;
        }

        case ANIM_AQUARIUM:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            aqua_bg_render();
            // Prime the live canvas with the background.
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            aqua_init();
            anim_timer = lv_timer_create(aqua_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;

        case ANIM_BEACH:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            reef_bg_render();
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            reef_init();
            anim_timer = lv_timer_create(reef_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;

        case ANIM_PIXELBEACH:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            pixbeach_bg_render();
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            pixbeach_init();
            anim_timer = lv_timer_create(pixbeach_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;

        case ANIM_GRASSLAND:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            grass_bg_render();
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            grass_init();
            anim_timer = lv_timer_create(grass_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;

        default:
            star_init();
            star_draw();
            anim_timer = lv_timer_create(star_timer_cb, 160, nullptr);  // 160 ms: see BISECT_LOG phase=7 note
            break;
    }

    lv_obj_add_event_cb(anim_scr, anim_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(anim_scr, anim_tap_cb,    LV_EVENT_CLICKED, NULL);
    anim_settings_req = 0;
    anim_tap_count    = 0;
    anim_last_tap_ms  = 0;
    lv_scr_load(anim_scr);
}

void anim_stop() {
    // The watchdog is now always-on (armed once in render_wdt_init); never stop it
    // here — it must keep protecting whatever screen comes next, including the chart.
    anim_swipe        = 0;
    anim_settings_req = 0;
    anim_tap_count    = 0;
    if (anim_timer)    { lv_timer_del(anim_timer);    anim_timer    = nullptr; }
    if (cd_crab_timer) { lv_timer_del(cd_crab_timer); cd_crab_timer = nullptr; }

    if (cur_type == ANIM_COUNTDOWN) {
        if (cd_scr) { lv_obj_del(cd_scr); cd_scr = nullptr; }
        cd_lbl_clock   = nullptr;
        cd_lbl_date    = nullptr;
        cd_lbl_time    = nullptr;
        cd_crab_canvas = nullptr;   // child of cd_scr, deleted above
        if (cd_crab_buf) { heap_caps_free(cd_crab_buf); cd_crab_buf = nullptr; }
    } else {
        if (anim_scr) { lv_obj_del(anim_scr); anim_scr = nullptr; }
        if (anim_buf) { heap_caps_free(anim_buf); anim_buf = nullptr; }
        if (anim_bg)  { heap_caps_free(anim_bg);  anim_bg  = nullptr; }
        anim_canvas  = nullptr;
        sf_lbl_clock = nullptr;   // children of anim_scr, already deleted above
        sf_lbl_date  = nullptr;
        sf_last_sec  = -1;
    }
    cur_type = -1;
}

void anim_set_countdown(uint32_t secs) {
    countdown = secs;
}

// Updates the crab's claw colors to match the user's candle theme.
// Call this before anim_start() whenever the bull/bear colors change.
void anim_set_candle_colors(uint32_t bull, uint32_t bear) {
    s_anim_bull = bull;
    s_anim_bear = bear;
}

// Public wrapper: draws the brand-spec crab onto any canvas under LV_LOCK.
// Used by boot splash and any external screen that needs the mascot.
void anim_draw_crab(lv_obj_t* canvas, int cx, int cy,
                    uint32_t bull_rgb, uint32_t bear_rgb,
                    bool blink, uint8_t walk_frame, bool claws_raised) {
    draw_crab(canvas, cx, cy,
              lv_color_hex(bull_rgb), lv_color_hex(bear_rgb),
              blink, walk_frame, claws_raised);
}

// ── Render-task liveness watchdog — public API ────────────────────────────────

// LVGL timer callback. Runs inside lv_timer_handler() (the render task), so each
// call proves that task is alive and producing frames. Bumps the heartbeat counter
// and feeds the hardware watchdog. If the render task deadlocks, this stops firing
// and the watchdog reboots the device ~30 s later.
static void render_feed_cb(lv_timer_t*) {
    s_heartbeat++;
    wdt_feed();
}

// Public feed: resets the watchdog countdown immediately without waiting for the 1 s
// lv_timer. Use before lvgl_port_stop() so the watchdog won't false-fire while LVGL
// rendering (and therefore the lv_timer feed) is paused during an HTTP fetch.
void render_wdt_keepalive() {
    wdt_feed();
}

// Arm the always-on watchdog and start its 1 s feed timer. Call once from setup(),
// under LV_LOCK, after bsp_display_start() (the render task must already exist).
void render_wdt_init() {
    wdt_start();                                   // arm the 30 s esp_timer (idempotent)
    if (!s_feed_timer)
        s_feed_timer = lv_timer_create(render_feed_cb, 1000, nullptr);  // feed every 1 s
    sdlog_println("[WDT] render watchdog armed (always-on, 5s)");
}

// Record the current main-loop State so a watchdog reboot can log where it hung.
void render_wdt_set_context(uint8_t state_code) {
    s_last_state = state_code;
}

// Render-task heartbeat (increments ~1/s while the render task is alive). A flat
// value across a health interval means the render task is stalling.
uint32_t render_wdt_heartbeat() {
    return s_heartbeat;
}

// If the previous boot ended in a watchdog reboot, copy the recorded details into
// *out, clear the marker, and return true. Call once early in setup().
bool render_wdt_consume_last_reboot(WdtReboot* out) {
    if (s_reboot_mark.magic != WDT_MAGIC) return false;
    if (out) *out = s_reboot_mark;
    s_reboot_mark.magic = 0;
    return true;
}
