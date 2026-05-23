#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "animations.h"
#include "chart_screen.h"  // SCR_W, SCR_H
#include "esp_timer.h"

// ── Animation watchdog ────────────────────────────────────────────────────────
// Independent hardware timer (esp_timer, NOT an LVGL timer) that reboots the
// device if no animation frame fires for 30 seconds. Because it runs via the
// esp_timer task — not the LVGL render task — it fires even when LVGL is
// frozen in a QSPI DMA semaphore wait (the silent-deadlock pattern seen during
// long animation runs). Each timer callback feeds it; anim_start arms it;
// anim_stop disarms it before tearing down LVGL objects.
static esp_timer_handle_t s_wdt = nullptr;

static void wdt_fire(void*) {
    // No animation frame in 30 s — LVGL render task is deadlocked. Reboot.
    Serial.println("[WDT] Animation watchdog: no frame in 30s, rebooting");
    Serial.flush();
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
        esp_timer_start_once(s_wdt, 30ULL * 1000 * 1000); // 30 s in µs
}

static void wdt_stop() {
    if (!s_wdt) return;
    esp_timer_stop(s_wdt);    // safe even if already fired
    esp_timer_delete(s_wdt);
    s_wdt = nullptr;
}

static void wdt_feed() {
    if (!s_wdt) return;
    // Reset the 30-second countdown — we are alive and rendering.
    esp_timer_restart(s_wdt, 30ULL * 1000 * 1000);
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
// before anim_start(). Defaults match the classic teal/red candle theme.
static uint32_t s_anim_bull = 0x26A69A;
static uint32_t s_anim_bear = 0xEF5350;

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

static void star_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();

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
#define TP_HORIZON      100   // y where sky meets water
#define TP_BUBBLE_COUNT   8   // rising bubbles in the water zone

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
} Crab;

typedef struct { int16_t x; float y; } Bubble;

static Crab   crab_state;
static Bubble bubbles[TP_BUBBLE_COUNT];

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

// One-time: dusk sky + tidal water + rocky shore + baked stars + pebbles.
// All baked into anim_bg with ordered dithering.
static void aqua_bg_render() {
    if (!anim_bg) return;

    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int   ft = tp_floor_y(x);
            float r, g, b;
            if (y < TP_HORIZON) {
                // Sky: deep navy at top (#0E1117) → dark teal-navy at horizon (#1A2F4A)
                float f = (float)y / TP_HORIZON;
                r = 14.0f + f * 12.0f;
                g = 17.0f + f * 30.0f;
                b = 23.0f + f * 51.0f;
            } else if (y < ft) {
                // Water: tidal teal, darker toward shore
                float denom = (float)(ft - TP_HORIZON);
                float f = (denom > 0.0f) ? (float)(y - TP_HORIZON) / denom : 1.0f;
                r = 30.0f  - f * 20.0f;
                g = 111.0f - f * 74.0f;
                b = 140.0f - f * 87.0f;
            } else {
                // Shore: dark rocky sand, deepening downward
                float denom = (float)(SCR_H - ft);
                float f = (denom > 0.0f) ? (float)(y - ft) / denom : 1.0f;
                r = 42.0f + f * 32.0f;
                g = 34.0f + f * 26.0f;
                b = 24.0f + f * 10.0f;
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // Horizon shimmer line at sky/water boundary
    for (int x = 0; x < SCR_W; x++) {
        float sh = 0.5f + 0.5f * sinf(x * 0.08f);
        put_dith(anim_bg, x, TP_HORIZON,
                 40.0f * sh, 90.0f * sh, 115.0f * sh);
        if (TP_HORIZON + 1 < SCR_H)
            put_dith(anim_bg, x, TP_HORIZON + 1,
                     25.0f * sh, 65.0f * sh, 90.0f * sh);
    }

    // Bake 38 stars into sky zone
    for (int i = 0; i < 38; i++) {
        int sx = random(0, SCR_W);
        int sy = random(2, TP_HORIZON - 6);
        uint8_t bv = (uint8_t)random(55, 210);
        put_dith(anim_bg, sx, sy, (float)bv, (float)bv, (float)bv);
        // Slightly larger bright stars get a dim halo pixel
        if (bv > 130 && sx + 1 < SCR_W)
            put_dith(anim_bg, sx + 1, sy, bv * 0.55f, bv * 0.55f, bv * 0.55f);
    }

    // Shore pebbles and boulders
    static const uint32_t TP_PEBBLE[5] = {
        0x3A3028, 0x4A3C2C, 0x5A4C38, 0x2A2420, 0x6A5840
    };
    for (int p = 0; p < 80; p++) {
        int px  = random(2, SCR_W - 2);
        int ft  = tp_floor_y(px);
        int py  = ft + random(2, SCR_H - ft - 1);
        int rad = random(1, 4);
        uint32_t c = TP_PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (float)((c >> 16) & 0xFF),
                  (float)((c >>  8) & 0xFF),
                  (float)( c        & 0xFF));
    }
    for (int p = 0; p < 5; p++) {
        int px  = 50 + random(0, SCR_W - 100);
        int ft  = tp_floor_y(px);
        int rad = random(8, 14);
        int py  = ft + rad - 2;
        uint32_t c = TP_PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (float)((c >> 16) & 0xFF),
                  (float)((c >>  8) & 0xFF),
                  (float)( c        & 0xFF));
    }
}

// Returns the bounding box that fully covers the crab sprite at (cx, cy).
static void crab_bbox_at(int cx, int cy, int* x1, int* y1, int* x2, int* y2) {
    *x1 = cx - 42;  *y1 = cy - 18;
    *x2 = cx + 42;  *y2 = cy + 17;
}

// Draws a pixel-art crab centered at (cx, cy) onto the given canvas.
// Left claw = bull_col, right claw = bear_col (user's candle theme colors).
// blink: draw eyes closed. walk_frame 0/1: alternates leg angles for walking.
static void draw_crab(lv_obj_t* canvas, int cx, int cy,
                      lv_color_t bull_col, lv_color_t bear_col,
                      bool blink, uint8_t walk_frame) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.border_width = 0;
    rd.radius       = 0;

    lv_color_t shell_dk  = lv_color_hex(0x7A2518);
    lv_color_t shell_mid = lv_color_hex(0xC04030);
    lv_color_t shell_hi  = lv_color_hex(0xE06050);

    int sw = 28, sh = 14;   // shell width / height
    int sx = cx - sw / 2;   // shell left x
    int sy = cy - sh / 2;   // shell top y

    // Legs: 3 per side; walk_frame alternates odd/even leg y-offsets.
    for (int i = 0; i < 3; i++) {
        int base_y = sy + 2 + i * 4;
        int woff   = walk_frame ? (i % 2 == 0 ? -2 : 2) : 0;
        rd.bg_color = shell_dk;
        lv_canvas_draw_rect(canvas, sx - 10, base_y + woff, 10, 2, &rd);
        lv_canvas_draw_rect(canvas, sx + sw,  base_y - woff, 10, 2, &rd);
    }

    // Arms extending from shell sides (drawn before shell so shell overlaps root)
    int arm_y = sy + sh / 3;
    rd.bg_color = shell_dk;
    lv_canvas_draw_rect(canvas, sx - 18, arm_y, 18, 3, &rd);
    lv_canvas_draw_rect(canvas, sx + sw,  arm_y, 18, 3, &rd);

    // Shell body (covers leg/arm roots)
    rd.radius   = LV_RADIUS_CIRCLE;
    rd.bg_color = shell_mid;
    lv_canvas_draw_rect(canvas, sx, sy, sw, sh, &rd);
    rd.radius   = 0;
    rd.bg_color = shell_hi;
    lv_canvas_draw_rect(canvas, sx + 5, sy + 2, sw - 10, 4, &rd);

    // Claws at arm tips: left = bull_col, right = bear_col (V-pincer).
    rd.bg_color = bull_col;
    lv_canvas_draw_rect(canvas, sx - 25, arm_y - 3, 7, 3, &rd);
    lv_canvas_draw_rect(canvas, sx - 25, arm_y + 3, 7, 3, &rd);
    rd.bg_color = bear_col;
    lv_canvas_draw_rect(canvas, sx + sw + 18, arm_y - 3, 7, 3, &rd);
    lv_canvas_draw_rect(canvas, sx + sw + 18, arm_y + 3, 7, 3, &rd);

    // Eye stalks
    rd.bg_color = shell_dk;
    lv_canvas_draw_rect(canvas, cx - 6, sy - 6, 2, 6, &rd);
    lv_canvas_draw_rect(canvas, cx + 4, sy - 6, 2, 6, &rd);

    // Eyes open or blinking
    if (!blink) {
        rd.radius   = LV_RADIUS_CIRCLE;
        rd.bg_color = lv_color_hex(0x0A0A0A);
        lv_canvas_draw_rect(canvas, cx - 8, sy - 8, 5, 5, &rd);
        lv_canvas_draw_rect(canvas, cx + 3, sy - 8, 5, 5, &rd);
        rd.radius   = 0;
        rd.bg_color = lv_color_hex(0xF0F0F0);
        lv_canvas_draw_rect(canvas, cx - 7, sy - 7, 2, 2, &rd);
        lv_canvas_draw_rect(canvas, cx + 4, sy - 7, 2, 2, &rd);
    } else {
        rd.bg_color = shell_dk;
        lv_canvas_draw_rect(canvas, cx - 8, sy - 6, 5, 1, &rd);
        lv_canvas_draw_rect(canvas, cx + 3, sy - 6, 5, 1, &rd);
    }
}

static void aqua_init() {
    // Crab starts left-of-center, walking right
    crab_state.x          = SCR_W * 0.32f;
    crab_state.vx         = 1.3f;
    crab_state.right      = true;
    crab_state.walk_frame = 0;
    crab_state.walk_tick  = 0;
    crab_state.blink_cd   = (uint16_t)random(100, 220);
    crab_state.blinking   = false;

    // Bubbles spread across the water zone
    for (int i = 0; i < TP_BUBBLE_COUNT; i++) {
        bubbles[i].x = random(0, SCR_W);
        int ft = tp_floor_y(bubbles[i].x);
        bubbles[i].y = (float)random(TP_HORIZON, ft - 5);
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
              crab_state.blinking, crab_state.walk_frame);

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
#define REEF_FISH_COUNT   3
#define REEF_BUBBLE_COUNT 8

typedef struct { float x, y, vx, scale; bool right; int col_idx; } ReefFish;

static ReefFish reef_fish[REEF_FISH_COUNT];
static Bubble   reef_bubbles[REEF_BUBBLE_COUNT];

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
    int bw = (int)(16 * scale); if (bw < 5) bw = 5;
    int bh = (int)(8  * scale); if (bh < 3) bh = 3;
    int tl = (int)(8  * scale); if (tl < 2) tl = 2;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.border_width = 0;

    lv_color_t dark = lv_color_mix(lv_color_black(), col, 65);
    rd.bg_color     = dark;
    int prong_h     = (bh - 1) / 3; if (prong_h < 1) prong_h = 1;
    if (right) {
        lv_canvas_draw_rect(anim_canvas, x - tl, y,                tl, prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x - tl, y + bh - prong_h, tl, prong_h, &rd);
    } else {
        lv_canvas_draw_rect(anim_canvas, x + bw, y,                tl, prong_h, &rd);
        lv_canvas_draw_rect(anim_canvas, x + bw, y + bh - prong_h, tl, prong_h, &rd);
    }
    rd.radius   = LV_RADIUS_CIRCLE;
    rd.bg_color = col;
    lv_canvas_draw_rect(anim_canvas, x, y, bw, bh, &rd);
    rd.radius   = 0;
    int ex      = right ? (x + bw - (int)(4 * scale)) : (x + (int)(2 * scale));
    rd.bg_color = lv_color_hex(0x080808);
    lv_canvas_draw_rect(anim_canvas, ex, y + bh / 2 - 1, 2, 2, &rd);
}

static void reef_fish_bbox(int fx, int fy, float sc, bool right,
                           int* x1, int* y1, int* x2, int* y2) {
    int bw = (int)(16 * sc); if (bw < 5) bw = 5;
    int bh = (int)(8  * sc); if (bh < 3) bh = 3;
    int tl = (int)(8  * sc); if (tl < 2) tl = 2;
    *x1 = (right ? fx - tl : fx) - 2;
    *x2 = (right ? fx + bw : fx + bw + tl) + 2;
    *y1 = fy - 2;
    *y2 = fy + bh + 2;
}

// One-time: underwater water gradient + reef floor + coral formations
// + kelp strands + floor pebbles. All baked into anim_bg.
static void reef_bg_render() {
    if (!anim_bg) return;

    // Water column: bright teal (#1B6FA8) at surface → deep navy (#071520) at floor
    for (int y = 0; y < SCR_H; y++) {
        float f = (float)y / (SCR_H - 1);
        for (int x = 0; x < SCR_W; x++) {
            int   ft = reef_floor_y(x);
            float r, g, b;
            if (y >= ft) {
                // Rocky reef floor
                float fd = (float)(y - ft) / (float)(SCR_H - ft);
                r = 26.0f + fd * 14.0f;
                g = 32.0f + fd * 10.0f;
                b = 42.0f + fd *  8.0f;
            } else {
                r = 27.0f  - f * 20.0f;
                g = 111.0f - f * 90.0f;
                b = 168.0f - f * 140.0f;
                // Caustic shimmer near surface
                if (y < 40) {
                    float caus = (40.0f - y) / 40.0f * 10.0f
                                 * (0.5f + 0.5f * sinf(x * 0.12f + y * 0.3f));
                    g += caus; if (g > 255.0f) g = 255.0f;
                    b += caus; if (b > 255.0f) b = 255.0f;
                }
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }

    // Coral formations: 5 clusters, blobs baked near the floor
    struct { int cx; int sz; uint32_t col; } coral[5] = {
        {  60, 22, 0xC84058 },
        { 155, 18, 0xD86838 },
        { 250, 24, 0xB83060 },
        { 355, 20, 0xD04858 },
        { 440, 16, 0xC86048 },
    };
    for (int c = 0; c < 5; c++) {
        int ft      = reef_floor_y(coral[c].cx);
        int branches = 2 + coral[c].sz / 8;
        for (int bi = 0; bi < branches; bi++) {
            int bx   = coral[c].cx + random(-6, 7);
            int brad = coral[c].sz / 2 - bi * 2;
            if (brad < 4) brad = 4;
            aqua_blot(bx, ft - brad + 3, brad,
                      (float)((coral[c].col >> 16) & 0xFF),
                      (float)((coral[c].col >>  8) & 0xFF),
                      (float)( coral[c].col        & 0xFF));
        }
    }

    // Floor pebbles
    static const uint32_t REEF_PEB[4] = { 0x1E2830, 0x283038, 0x1A2A38, 0x243040 };
    for (int p = 0; p < 70; p++) {
        int px  = random(2, SCR_W - 2);
        int ft  = reef_floor_y(px);
        int py  = ft + random(2, SCR_H - ft - 1);
        int rad = random(1, 4);
        uint32_t c = REEF_PEB[random(0, 4)];
        aqua_blot(px, py, rad,
                  (float)((c >> 16) & 0xFF),
                  (float)((c >>  8) & 0xFF),
                  (float)( c        & 0xFF));
    }

    // Kelp strands (4 tall strands baked in with natural curve)
    static const uint32_t KELP[2] = { 0x2E8B57, 0x1F6B3F };
    for (int k = 0; k < 4; k++) {
        int   kx   = 90 + k * 100 + random(-12, 13);
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
    // Fish at 3 depths: back=small+slow, front=larger+faster (depth illusion)
    for (int i = 0; i < REEF_FISH_COUNT; i++) {
        float sc  = 0.55f + i * 0.15f;
        float spd = 0.40f + i * 0.25f;
        reef_fish[i].scale   = sc;
        reef_fish[i].y       = 30.0f + i * 65.0f;
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
    // Crab walks the reef floor (reuses crab_state — never active with tidepool)
    crab_state.x          = SCR_W * 0.45f;
    crab_state.vx         = 1.2f;
    crab_state.right      = true;
    crab_state.walk_frame = 0;
    crab_state.walk_tick  = 0;
    crab_state.blink_cd   = (uint16_t)random(100, 220);
    crab_state.blinking   = false;
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
// COUNTDOWN CLOCK
// ══════════════════════════════════════════════════════════════════════════════
#define CD_CRAB_H 50   // pixel height of the crab canvas strip at screen bottom

static lv_obj_t*   cd_scr         = nullptr;
static lv_obj_t*   cd_lbl_time    = nullptr;
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

    // Fixed y within the 50px canvas: shell center sits 20px from bottom
    const int cy = CD_CRAB_H - 20;

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

    // Erase ghost and redraw
    cd_crab_erase(
        (ox1 < nx1 ? ox1 : nx1), (oy1 < ny1 ? oy1 : ny1),
        (ox2 > nx2 ? ox2 : nx2), (oy2 > ny2 ? oy2 : ny2));
    draw_crab(cd_crab_canvas, ncx, cy,
              lv_color_hex(s_anim_bull), lv_color_hex(s_anim_bear),
              crab_state.blinking, crab_state.walk_frame);
    lv_obj_invalidate(cd_crab_canvas);
}

static void countdown_build() {
    cd_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cd_scr, lv_color_hex(0x0E1117), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x7A8290), LV_PART_MAIN);
    lv_label_set_text(title, "Market opens in");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -64);

    cd_lbl_time = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(cd_lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(cd_lbl_time, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
    lv_label_set_text(cd_lbl_time, "--:--:--");
    lv_obj_align(cd_lbl_time, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* sub = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x22C55E), LV_PART_MAIN);
    lv_label_set_text(sub, "NYSE / NASDAQ   9:30 AM ET");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 64);

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
        crab_state.x          = SCR_W * 0.35f;
        crab_state.vx         = 1.1f;
        crab_state.right      = true;
        crab_state.walk_frame = 0;
        crab_state.walk_tick  = 0;
        crab_state.blink_cd   = (uint16_t)random(80, 200);
        crab_state.blinking   = false;
    }

    lv_scr_load(cd_scr);
}

static void countdown_tick_cb(lv_timer_t*) {
    if (!cd_lbl_time) return;
    wdt_feed();
    if (countdown > 0) countdown--;
    uint32_t s  = countdown;
    uint32_t h  = s / 3600;
    uint32_t m  = (s % 3600) / 60;
    uint32_t sc = s % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, sc);
    lv_label_set_text(cd_lbl_time, buf);

    // Digit color shifts from Pearl → Sand Amber → Bull Green as open approaches
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
        cd_crab_timer   = lv_timer_create(countdown_crab_cb,  120,  nullptr);
        wdt_start();
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
        case ANIM_STARFIELD:
            // No anim_bg needed: star_draw() fills sky + all stars each frame,
            // which is cheap and naturally erases the previous shooting star tail.
            star_init();
            star_draw();
            anim_timer = lv_timer_create(star_timer_cb, 120, nullptr);
            break;

        case ANIM_AQUARIUM:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            aqua_bg_render();
            // Prime the live canvas with the background.
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            aqua_init();
            anim_timer = lv_timer_create(aqua_timer_cb, 120, nullptr);
            break;

        case ANIM_BEACH:
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            reef_bg_render();
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            reef_init();
            anim_timer = lv_timer_create(reef_timer_cb, 120, nullptr);
            break;

        default:
            star_init();
            star_draw();
            anim_timer = lv_timer_create(star_timer_cb, 120, nullptr);
            break;
    }

    lv_obj_add_event_cb(anim_scr, anim_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(anim_scr, anim_tap_cb,    LV_EVENT_CLICKED, NULL);
    anim_settings_req = 0;
    anim_tap_count    = 0;
    anim_last_tap_ms  = 0;
    wdt_start();
    lv_scr_load(anim_scr);
}

void anim_stop() {
    wdt_stop(); // disarm before any LVGL teardown — watchdog must not fire mid-cleanup
    anim_swipe        = 0;
    anim_settings_req = 0;
    anim_tap_count    = 0;
    if (anim_timer)    { lv_timer_del(anim_timer);    anim_timer    = nullptr; }
    if (cd_crab_timer) { lv_timer_del(cd_crab_timer); cd_crab_timer = nullptr; }

    if (cur_type == ANIM_COUNTDOWN) {
        if (cd_scr) { lv_obj_del(cd_scr); cd_scr = nullptr; }
        cd_lbl_time    = nullptr;
        cd_crab_canvas = nullptr;   // child of cd_scr, deleted above
        if (cd_crab_buf) { heap_caps_free(cd_crab_buf); cd_crab_buf = nullptr; }
    } else {
        if (anim_scr) { lv_obj_del(anim_scr); anim_scr = nullptr; }
        if (anim_buf) { heap_caps_free(anim_buf); anim_buf = nullptr; }
        if (anim_bg)  { heap_caps_free(anim_bg);  anim_bg  = nullptr; }
        anim_canvas = nullptr;
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
