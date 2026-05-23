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
// PIXEL AQUARIUM
// ══════════════════════════════════════════════════════════════════════════════
#define FISH_COUNT   8
#define BUBBLE_COUNT 14

typedef struct { float x, y, vx, scale; bool right; int col_idx; } Fish;
typedef struct { int16_t x; float y; } Bubble;

static Fish   fish_arr[FISH_COUNT];
static Bubble bubbles[BUBBLE_COUNT];

static const uint32_t FISH_HEX[6] = {
    0xFF6B35, 0x4ECDC4, 0xFFE66D, 0xFF6B9D, 0x95E1D3, 0xF38181
};

static void aqua_init() {
    for (int i = 0; i < FISH_COUNT; i++) {
        float sc = 0.6f + random(0, 110) * 0.01f;
        fish_arr[i].scale   = sc;
        fish_arr[i].x       = random(20, SCR_W - 20);
        fish_arr[i].y       = random(40, SCR_H - 50);
        float spd = (0.35f + random(0, 8) * 0.12f) * (0.6f + sc * 0.5f);
        fish_arr[i].right   = (bool)random(0, 2);
        fish_arr[i].vx      = fish_arr[i].right ? spd : -spd;
        fish_arr[i].col_idx = i;
    }
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        bubbles[i].x = random(0, SCR_W);
        bubbles[i].y = random(0, SCR_H);
    }
}

// Returns the bounding box that fully covers a fish (body + tail + fin + margin).
static void fish_bbox(int fx, int fy, float scale, bool right,
                      int* x1, int* y1, int* x2, int* y2) {
    int bw = (int)(18 * scale); if (bw < 6) bw = 6;
    int bh = (int)(9  * scale); if (bh < 4) bh = 4;
    int tl = (int)(9  * scale); if (tl < 3) tl = 3;
    int fh = (int)(5  * scale); if (fh < 2) fh = 2;
    *x1 = (right ? fx - tl : fx      ) - 2;
    *x2 = (right ? fx + bw : fx + bw + tl) + 2;
    *y1 = fy - fh - 2;
    *y2 = fy + bh + 2;
}

// Smooth fish: rounded stadium body, forked-rect tail, rect fin, eye.
// Uses only lv_canvas_draw_rect (no polygon) — the polygon mask allocation
// per call was causing heap churn that contributed to the display deadlock.
static void draw_fish(int x, int y, lv_color_t col, bool right, float scale) {
    int bw = (int)(18 * scale); if (bw < 6) bw = 6;
    int bh = (int)(9  * scale); if (bh < 4) bh = 4;
    int tl = (int)(9  * scale); if (tl < 3) tl = 3;
    int fh = (int)(5  * scale); if (fh < 2) fh = 2;

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.border_width = 0;

    lv_color_t dark = lv_color_mix(lv_color_black(), col, 70);
    rdsc.bg_color = dark;

    // Forked tail: two rect prongs at the rear of the body.
    int prong_h = (bh - 2) / 3;
    if (prong_h < 1) prong_h = 1;
    if (right) {
        lv_canvas_draw_rect(anim_canvas, x - tl, y,              tl, prong_h,   &rdsc);
        lv_canvas_draw_rect(anim_canvas, x - tl, y + bh - prong_h, tl, prong_h, &rdsc);
    } else {
        lv_canvas_draw_rect(anim_canvas, x + bw, y,              tl, prong_h,   &rdsc);
        lv_canvas_draw_rect(anim_canvas, x + bw, y + bh - prong_h, tl, prong_h, &rdsc);
    }

    // Dorsal fin: small rect stub on top of body center.
    int fw = (tl > 4) ? tl / 2 : 2;
    lv_canvas_draw_rect(anim_canvas, x + bw / 2 - fw / 2, y - fh, fw, fh, &rdsc);

    // Rounded stadium body.
    rdsc.radius   = LV_RADIUS_CIRCLE;
    rdsc.bg_color = col;
    lv_canvas_draw_rect(anim_canvas, x, y, bw, bh, &rdsc);
    rdsc.radius = 0;

    // Eye.
    int es = (scale >= 1.0f) ? 3 : 2;
    int ex = right ? (x + bw - (int)(5 * scale)) : (x + (int)(3 * scale));
    rdsc.bg_color = lv_color_hex(0x0A0A0A);
    lv_canvas_draw_rect(anim_canvas, ex, y + bh / 2 - es / 2, es, es, &rdsc);
}

static inline int aqua_floor_top(int x) {
    return (int)(SCR_H - 40
                 + 6.0f * sinf(x * 0.020f)
                 + 3.0f * sinf(x * 0.061f + 2.0f));
}

// Bakes a soft round pebble into anim_bg at (cx,cy).
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

// One-time: dithered water gradient + light shafts + ocean floor with
// gravel and stones + right-side seaweed cluster. All baked into anim_bg.
static void aqua_bg_render() {
    if (!anim_bg) return;
    for (int y = 0; y < SCR_H; y++) {
        float f = (float)y / (SCR_H - 1);
        float r = 14.0f - f * 12.0f;
        float g = 78.0f - f * 64.0f;
        float b = 108.0f - f * 70.0f;
        for (int x = 0; x < SCR_W; x++) {
            int ft = aqua_floor_top(x);
            if (y >= ft) {
                float fd = (float)(y - ft) / (float)(SCR_H - ft);
                put_dith(anim_bg, x, y,
                         70.0f - fd * 36.0f,
                         78.0f - fd * 38.0f,
                         66.0f - fd * 30.0f);
                continue;
            }
            float sh = 0.0f;
            for (int s = 0; s < 3; s++) {
                int cx = 90 + s * 150 + (int)(y * 0.5f);
                float d = (float)(x - cx);
                if (d > -26 && d < 26)
                    sh += (1.0f - fabsf(d) / 26.0f) * 16.0f * (1.0f - f);
            }
            put_dith(anim_bg, x, y, r + sh * 0.4f, g + sh, b + sh);
        }
    }

    static const uint32_t PEBBLE[5] = {
        0x6E7A66, 0x847C66, 0x5A6258, 0x9A8E70, 0x4E544C
    };
    for (int p = 0; p < 90; p++) {
        int px  = random(2, SCR_W - 2);
        int ft  = aqua_floor_top(px);
        int py  = ft + random(3, SCR_H - ft - 1);
        int rad = random(1, 4);
        uint32_t c = PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    for (int p = 0; p < 6; p++) {
        int px  = 40 + random(0, SCR_W - 80);
        int ft  = aqua_floor_top(px);
        int rad = random(7, 13);
        int py  = ft + rad - 2;
        uint32_t c = PEBBLE[random(0, 5)];
        aqua_blot(px, py, rad,
                  (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }

    static const uint32_t WEED[4] = { 0x1F7A33, 0x2A9D45, 0x176B2A, 0x33B355 };
    for (int wd = 0; wd < 8; wd++) {
        int   bx    = SCR_W - 104 + wd * 13 + (int)random(-4, 5);
        if (bx < 6) bx = 6; else if (bx > SCR_W - 6) bx = SCR_W - 6;
        int   ftb   = aqua_floor_top(bx) + 2;
        int   hgt   = random(64, 150);
        float amp   = 7.0f + random(0, 14);
        float freq  = 0.020f + random(0, 18) * 0.001f;
        float ph    = random(0, 628) * 0.01f;
        float hwB   = 2.5f + random(0, 25) * 0.1f;
        uint32_t bc = WEED[wd & 3];
        float br = (bc >> 16) & 0xFF, bg = (bc >> 8) & 0xFF, bb = bc & 0xFF;
        for (int yy = ftb; yy > ftb - hgt; yy--) {
            if (yy < 0 || yy >= SCR_H) continue;
            float tt = (float)(ftb - yy) / (float)hgt;
            int   cx = bx + (int)(amp * tt * sinf(yy * freq + ph));
            int   hw = (int)(hwB * (1.0f - tt * 0.85f) + 0.5f);
            if (hw < 1) hw = 1;
            for (int xx = cx - hw; xx <= cx + hw; xx++) {
                if (xx < 0 || xx >= SCR_W) continue;
                float edge = 1.0f - 0.45f * (fabsf((float)(xx - cx)) / (hw + 0.5f));
                put_dith(anim_bg, xx, yy, br * edge, bg * edge, bb * edge);
            }
        }
    }
}

static void aqua_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();

    // Fallback path: if anim_bg wasn't allocated, paint a simple gradient.
    if (!anim_bg) {
        lv_draw_rect_dsc_t fb; lv_draw_rect_dsc_init(&fb);
        fb.radius = 0; fb.border_width = 0;
        for (int y = 0; y < SCR_H; y++) {
            uint8_t b = (uint8_t)(20 + (uint8_t)(50 * y / SCR_H));
            fb.bg_color = lv_color_make(0, b / 4, b);
            lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &fb);
        }
        lv_obj_invalidate(anim_canvas);
        return;
    }

    // ── FISH ─────────────────────────────────────────────────────────────────
    // Compute the previous bbox BEFORE advancing (= where the fish is right now),
    // advance the fish, compute the new bbox, restore the union of old+new from
    // anim_bg (erases last frame's sprite and cleans background for the new draw),
    // then draw the fish at its new position. No full-buffer memcpy needed.
    for (int i = 0; i < FISH_COUNT; i++) {
        int px1, py1, px2, py2;
        fish_bbox((int)fish_arr[i].x, (int)fish_arr[i].y,
                  fish_arr[i].scale, fish_arr[i].right,
                  &px1, &py1, &px2, &py2);

        fish_arr[i].x += fish_arr[i].vx;
        if (fish_arr[i].x < -28) {
            fish_arr[i].x     = SCR_W + 8;
            fish_arr[i].right = false;
            fish_arr[i].vx    = -fabsf(fish_arr[i].vx);
        }
        if (fish_arr[i].x > SCR_W + 8) {
            fish_arr[i].x     = -8;
            fish_arr[i].right = true;
            fish_arr[i].vx    =  fabsf(fish_arr[i].vx);
        }

        int nx1, ny1, nx2, ny2;
        fish_bbox((int)fish_arr[i].x, (int)fish_arr[i].y,
                  fish_arr[i].scale, fish_arr[i].right,
                  &nx1, &ny1, &nx2, &ny2);

        bg_restore(
            (px1 < nx1 ? px1 : nx1), (py1 < ny1 ? py1 : ny1),
            (px2 > nx2 ? px2 : nx2), (py2 > ny2 ? py2 : ny2));

        draw_fish((int)fish_arr[i].x, (int)fish_arr[i].y,
                  lv_color_hex(FISH_HEX[fish_arr[i].col_idx % 6]),
                  fish_arr[i].right, fish_arr[i].scale);
    }

    // ── BUBBLES ───────────────────────────────────────────────────────────────
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius       = LV_RADIUS_CIRCLE;
    rdsc.bg_opa       = LV_OPA_40;
    rdsc.bg_color     = lv_color_hex(0xADD8E6);
    rdsc.border_width = 0;

    for (int i = 0; i < BUBBLE_COUNT; i++) {
        int old_by = (int)bubbles[i].y;
        bubbles[i].y -= 0.4f;

        if (bubbles[i].y < 0) {
            // Erase the last visible position, then wrap bubble to the bottom.
            if (old_by >= 0 && old_by < SCR_H)
                bg_restore(bubbles[i].x - 1, old_by - 1, bubbles[i].x + 5, old_by + 5);
            bubbles[i].y = (float)(SCR_H + random(0, 20));
            bubbles[i].x = random(0, SCR_W);
            continue;
        }

        int new_by = (int)bubbles[i].y;
        // Restore the union of old and new y-band (same x column).
        int fy1 = (old_by < new_by ? old_by : new_by) - 1;
        int fy2 = (old_by > new_by ? old_by : new_by) + 5;
        bg_restore(bubbles[i].x - 1, fy1, bubbles[i].x + 5, fy2);

        if (new_by >= 0 && new_by < SCR_H)
            lv_canvas_draw_rect(anim_canvas, bubbles[i].x, new_by, 4, 4, &rdsc);
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// BEACH SUNSET
// ══════════════════════════════════════════════════════════════════════════════
static int beach_hz = 0;
// Sun center from the previous frame; -1 means first frame (nothing to erase).
static int sun_prev_x = -1, sun_prev_y = 0;

// Irregular natural shoreline: y of the sand top edge at column x.
static inline int beach_sand_top(int x) {
    return (int)(SCR_H - 26
                 + 7.0f * sinf(x * 0.016f)
                 + 3.0f * sinf(x * 0.052f + 1.3f));
}

// One-time: dithered sky → soft horizon blend → ocean → irregular sand.
static void beach_bg_render() {
    if (!anim_bg) return;
    int hz = beach_hz;
    for (int x = 0; x < SCR_W; x++) {
        int st = beach_sand_top(x);
        for (int y = 0; y < SCR_H; y++) {
            float r, g, b;
            if (y < st) {
                float fs = (float)y / hz; if (fs > 1.0f) fs = 1.0f;
                float sr = 220.0f - fs * 150.0f;
                float sg = 100.0f - fs * 80.0f;
                float sb = 30.0f  + fs * 100.0f;
                int   den = (st - hz); if (den < 1) den = 1;
                float fo = (float)(y - hz) / den;
                if (fo < 0.0f) fo = 0.0f; else if (fo > 1.0f) fo = 1.0f;
                float orr = 20.0f  + fo * 10.0f;
                float og  = 70.0f  + fo * 55.0f;
                float ob  = 120.0f + fo * 35.0f;
                if (y <= hz - 7) { r = sr; g = sg; b = sb; }
                else if (y >= hz + 7) { r = orr; g = og; b = ob; }
                else {
                    float m = (float)(y - (hz - 7)) / 14.0f;
                    r = sr + (orr - sr) * m;
                    g = sg + (og  - sg) * m;
                    b = sb + (ob  - sb) * m;
                }
            } else {
                int depth = y - st;
                if (depth < 3) {
                    r = 225.0f; g = 230.0f; b = 225.0f;
                } else {
                    float fd = (float)(depth - 3) / 20.0f;
                    if (fd > 1.0f) fd = 1.0f;
                    r = 150.0f + fd * 44.0f;
                    g = 120.0f + fd * 40.0f;
                    b = 78.0f  + fd * 13.0f;
                }
            }
            put_dith(anim_bg, x, y, r, g, b);
        }
    }
}

static void beach_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    wdt_feed();

    float t  = millis() * 0.001f;
    int   hz = beach_hz;

    // Fallback path if bg not allocated.
    if (!anim_bg) {
        lv_draw_rect_dsc_t fb; lv_draw_rect_dsc_init(&fb);
        fb.radius = 0; fb.border_width = 0;
        for (int y = 0; y < hz; y++) {
            float f = (float)y / hz;
            fb.bg_color = lv_color_make((uint8_t)(220 - f * 150),
                                        (uint8_t)(100 - f * 80),
                                        (uint8_t)(30  + f * 100));
            lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &fb);
        }
        for (int y = hz; y < SCR_H; y++) {
            float f = (float)(y - hz) / (SCR_H - hz);
            fb.bg_color = lv_color_make(0, (uint8_t)(20 + f * 40),
                                           (uint8_t)(60 + f * 40));
            lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &fb);
        }
        fb.bg_color = lv_color_hex(0xC2A05B);
        lv_canvas_draw_rect(anim_canvas, 0, SCR_H - 18, SCR_W, 18, &fb);
        lv_obj_invalidate(anim_canvas);
        return;
    }

    // ── SUN ──────────────────────────────────────────────────────────────────
    // Restore the sun glow region: previous position (erase last frame's sun)
    // and current position (clean background to draw this frame's sun on).
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.border_width = 0;

    int sx = SCR_W / 2 + (int)(30 * sinf(t * 0.1f));
    int sy = (int)(hz * 0.38f);
    if (sun_prev_x >= 0)
        bg_restore(sun_prev_x - 33, sun_prev_y - 33, sun_prev_x + 33, sun_prev_y + 33);
    bg_restore(sx - 33, sy - 33, sx + 33, sy + 33);
    sun_prev_x = sx; sun_prev_y = sy;

    rdsc.radius   = LV_RADIUS_CIRCLE;
    rdsc.bg_color = lv_color_hex(0xFFD700);
    rdsc.bg_opa   = LV_OPA_COVER;
    lv_canvas_draw_rect(anim_canvas, sx - 22, sy - 22, 44, 44, &rdsc);
    rdsc.bg_opa = LV_OPA_20;
    lv_canvas_draw_rect(anim_canvas, sx - 32, sy - 32, 64, 64, &rdsc);
    rdsc.radius = 0; rdsc.bg_opa = LV_OPA_COVER;

    // ── WAVES ─────────────────────────────────────────────────────────────────
    // Restore the fixed ripple band each frame (hz+3 to hz+70, full width).
    bg_restore(0, hz + 3, SCR_W - 1, hz + 70);

    // Draw animated ripple lines on the ocean.
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    struct { int base; float amp, fx, ft, phase; uint32_t col; lv_opa_t opa; int w; }
    ripple[3] = {
        { hz + 14, 3.5f, 0.045f, 1.4f, 0.0f, 0x6FA8C8, LV_OPA_30, 2 },
        { hz + 34, 4.0f, 0.060f, 1.8f, 2.1f, 0x5C9BBE, LV_OPA_30, 2 },
        { hz + 58, 4.5f, 0.075f, 2.2f, 4.0f, 0x4E8FB4, LV_OPA_40, 3 },
    };
    for (int rr = 0; rr < 3; rr++) {
        ldsc.width = ripple[rr].w;
        ldsc.color = lv_color_hex(ripple[rr].col);
        ldsc.opa   = ripple[rr].opa;
        for (int x = 0; x < SCR_W - 1; x++) {
            int y0 = ripple[rr].base + (int)(ripple[rr].amp *
                     sinf(x * ripple[rr].fx + t * ripple[rr].ft + ripple[rr].phase));
            int y1 = ripple[rr].base + (int)(ripple[rr].amp *
                     sinf((x + 1) * ripple[rr].fx + t * ripple[rr].ft + ripple[rr].phase));
            lv_point_t pts[2] = {{x, y0}, {x + 1, y1}};
            lv_canvas_draw_line(anim_canvas, pts, 2, &ldsc);
        }
    }
    // Faint foam crest on the nearest wave only.
    ldsc.width = 1;
    ldsc.color = lv_color_hex(0xBFE0EE);
    ldsc.opa   = LV_OPA_30;
    for (int x = 0; x < SCR_W - 1; x++) {
        int y0 = ripple[0].base - 2 + (int)(ripple[0].amp *
                 sinf(x * ripple[0].fx + t * ripple[0].ft));
        int y1 = ripple[0].base - 2 + (int)(ripple[0].amp *
                 sinf((x + 1) * ripple[0].fx + t * ripple[0].ft));
        lv_point_t pts[2] = {{x, y0}, {x + 1, y1}};
        lv_canvas_draw_line(anim_canvas, pts, 2, &ldsc);
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// COUNTDOWN CLOCK
// ══════════════════════════════════════════════════════════════════════════════
static lv_obj_t* cd_scr      = nullptr;
static lv_obj_t* cd_lbl_time = nullptr;

static void countdown_build() {
    cd_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cd_scr, lv_color_hex(0x0D1117), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x787B86), LV_PART_MAIN);
    lv_label_set_text(title, "Market opens in");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -64);

    cd_lbl_time = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(cd_lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(cd_lbl_time, lv_color_hex(0x58A6FF), LV_PART_MAIN);
    lv_label_set_text(cd_lbl_time, "--:--:--");
    lv_obj_align(cd_lbl_time, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* sub = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x3FB950), LV_PART_MAIN);
    lv_label_set_text(sub, "NYSE / NASDAQ   9:30 AM ET");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 64);

    lv_obj_t* fish = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(fish, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(fish, lv_color_hex(0x2D4A8A), LV_PART_MAIN);
    lv_label_set_text(fish, "><>   ><>   ><>");
    lv_obj_align(fish, LV_ALIGN_BOTTOM_MID, 0, -24);

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
        anim_timer = lv_timer_create(countdown_tick_cb, 1000, nullptr);
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
            beach_hz  = (int)(SCR_H * 0.52f);
            sun_prev_x = -1;
            anim_bg = (lv_color_t*)heap_caps_malloc(
                (size_t)SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
            beach_bg_render();
            if (anim_bg)
                memcpy(anim_buf, anim_bg, (size_t)SCR_W * SCR_H * sizeof(lv_color_t));
            anim_timer = lv_timer_create(beach_timer_cb, 150, nullptr);
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
    if (anim_timer) { lv_timer_del(anim_timer); anim_timer = nullptr; }

    if (cur_type == ANIM_COUNTDOWN) {
        if (cd_scr) { lv_obj_del(cd_scr); cd_scr = nullptr; }
        cd_lbl_time = nullptr;
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
