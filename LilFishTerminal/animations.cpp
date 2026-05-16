#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdlib.h>
#include "animations.h"
#include "chart_screen.h"  // SCR_W, SCR_H

// ── Swipe / gesture state ─────────────────────────────────────────────────────
static volatile int8_t anim_swipe = 0;

static void anim_gesture_cb(lv_event_t*) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if      (dir == LV_DIR_LEFT)  anim_swipe =  1;
    else if (dir == LV_DIR_RIGHT) anim_swipe = -1;
}

int anim_get_swipe() {
    int d = (int)anim_swipe;
    anim_swipe = 0;
    return d;
}

// ── Shared state ──────────────────────────────────────────────────────────────
static lv_obj_t*   anim_scr    = nullptr;
static lv_timer_t* anim_timer  = nullptr;
static lv_color_t* anim_buf    = nullptr;
static lv_obj_t*   anim_canvas = nullptr;
static int         cur_type    = -1;
static uint32_t    countdown   = 0;

// ══════════════════════════════════════════════════════════════════════════════
// STARFIELD
// ══════════════════════════════════════════════════════════════════════════════
#define STAR_COUNT 120
typedef struct { int16_t x, y; uint8_t bri; } Star;
static Star stars[STAR_COUNT];

static void star_init() {
    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].x   = random(0, SCR_W);
        stars[i].y   = random(0, SCR_H);
        stars[i].bri = random(40, 255);
    }
}

static void star_draw() {
    lv_canvas_fill_bg(anim_canvas, lv_color_hex(0x020408), LV_OPA_COVER);
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0; rdsc.border_width = 0;

    for (int i = 0; i < STAR_COUNT; i++) {
        uint8_t b = stars[i].bri;
        rdsc.bg_color = lv_color_make(b, b, b);
        int sz = (b > 200) ? 2 : 1;
        lv_canvas_draw_rect(anim_canvas, stars[i].x, stars[i].y, sz, sz, &rdsc);
    }
}

static void star_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;
    for (int i = 0; i < STAR_COUNT / 8; i++) {
        int idx = random(0, STAR_COUNT);
        int nb  = (int)stars[idx].bri + random(-30, 31);
        if (nb < 20)  nb = 20;
        if (nb > 255) nb = 255;
        stars[idx].bri = (uint8_t)nb;
    }
    star_draw();
    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// PIXEL AQUARIUM
// ══════════════════════════════════════════════════════════════════════════════
#define FISH_COUNT   6
#define BUBBLE_COUNT 12

typedef struct { float x, y, vx; bool right; int col_idx; } Fish;
typedef struct { int16_t x; float y; } Bubble;

static Fish   fish_arr[FISH_COUNT];
static Bubble bubbles[BUBBLE_COUNT];

static const uint32_t FISH_HEX[6] = {
    0xFF6B35, 0x4ECDC4, 0xFFE66D, 0xFF6B9D, 0x95E1D3, 0xF38181
};

static void aqua_init() {
    for (int i = 0; i < FISH_COUNT; i++) {
        fish_arr[i].x       = random(20, SCR_W - 20);
        fish_arr[i].y       = random(50, SCR_H - 40);
        float spd = 0.5f + random(0, 10) * 0.15f;
        fish_arr[i].right   = (bool)random(0, 2);
        fish_arr[i].vx      = fish_arr[i].right ? spd : -spd;
        fish_arr[i].col_idx = i;
    }
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        bubbles[i].x = random(0, SCR_W);
        bubbles[i].y = random(0, SCR_H);
    }
}

static void draw_fish(int x, int y, lv_color_t col, bool right) {
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0; rdsc.border_width = 0;

    rdsc.bg_color = col;
    lv_canvas_draw_rect(anim_canvas, x, y, 10, 5, &rdsc);

    if (right) {
        lv_canvas_draw_rect(anim_canvas, x - 4, y,     3, 2, &rdsc);
        lv_canvas_draw_rect(anim_canvas, x - 4, y + 3, 3, 2, &rdsc);
    } else {
        lv_canvas_draw_rect(anim_canvas, x + 10, y,     3, 2, &rdsc);
        lv_canvas_draw_rect(anim_canvas, x + 10, y + 3, 3, 2, &rdsc);
    }
    rdsc.bg_color = lv_color_hex(0x111111);
    int ex = right ? (x + 7) : (x + 2);
    lv_canvas_draw_rect(anim_canvas, ex, y + 1, 2, 2, &rdsc);
}

static void aqua_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0; rdsc.border_width = 0;

    // Deep blue gradient sky
    for (int y = 0; y < SCR_H; y++) {
        uint8_t b = (uint8_t)(20 + (uint8_t)(50 * y / SCR_H));
        rdsc.bg_color = lv_color_make(0, b / 4, b);
        lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &rdsc);
    }

    // Seaweed
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.width = 3; ldsc.color = lv_color_hex(0x1A7A1A);
    float phase = millis() * 0.002f;
    for (int sw = 0; sw < 5; sw++) {
        int sx = 30 + sw * 90;
        for (int k = 0; k < 8; k++) {
            int ox = (int)(6 * sinf((float)k * 0.8f + phase));
            int y0 = SCR_H - k * 14;
            int y1 = SCR_H - (k + 1) * 14;
            lv_point_t pts[2] = {{sx + ox, y0}, {sx + ox + 2, y1}};
            lv_canvas_draw_line(anim_canvas, pts, 2, &ldsc);
        }
    }

    // Fish
    for (int i = 0; i < FISH_COUNT; i++) {
        fish_arr[i].x += fish_arr[i].vx;
        if (fish_arr[i].x < -16)       { fish_arr[i].x = SCR_W + 5;  fish_arr[i].right = false; fish_arr[i].vx = -fabsf(fish_arr[i].vx); }
        if (fish_arr[i].x > SCR_W + 5) { fish_arr[i].x = -5;         fish_arr[i].right = true;  fish_arr[i].vx =  fabsf(fish_arr[i].vx); }
        draw_fish((int)fish_arr[i].x, (int)fish_arr[i].y,
                  lv_color_hex(FISH_HEX[fish_arr[i].col_idx % 6]),
                  fish_arr[i].right);
    }

    // Bubbles
    rdsc.radius    = LV_RADIUS_CIRCLE;
    rdsc.bg_opa    = LV_OPA_40;
    rdsc.bg_color  = lv_color_hex(0xADD8E6);
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        bubbles[i].y -= 0.4f;
        if (bubbles[i].y < 0) {
            bubbles[i].y = (float)(SCR_H + random(0, 20));
            bubbles[i].x = random(0, SCR_W);
        }
        lv_canvas_draw_rect(anim_canvas, bubbles[i].x, (int)bubbles[i].y, 4, 4, &rdsc);
    }

    lv_obj_invalidate(anim_canvas);
}

// ══════════════════════════════════════════════════════════════════════════════
// BEACH SUNSET
// ══════════════════════════════════════════════════════════════════════════════
static void beach_timer_cb(lv_timer_t*) {
    if (!anim_canvas) return;

    float t = millis() * 0.001f;

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0; rdsc.border_width = 0;

    int hz = (int)(SCR_H * 0.52f);

    // Sky gradient orange→purple
    for (int y = 0; y < hz; y++) {
        float f  = (float)y / hz;
        uint8_t r = (uint8_t)(220 - f * 150);
        uint8_t g = (uint8_t)(100 - f * 80);
        uint8_t b = (uint8_t)(30  + f * 100);
        rdsc.bg_color = lv_color_make(r, g, b);
        lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &rdsc);
    }

    // Sun
    rdsc.radius   = LV_RADIUS_CIRCLE;
    rdsc.bg_color = lv_color_hex(0xFFD700);
    rdsc.bg_opa   = LV_OPA_COVER;
    int sx = SCR_W / 2 + (int)(30 * sinf(t * 0.1f));
    int sy = (int)(hz * 0.38f);
    lv_canvas_draw_rect(anim_canvas, sx - 22, sy - 22, 44, 44, &rdsc);
    rdsc.bg_opa = LV_OPA_20;
    lv_canvas_draw_rect(anim_canvas, sx - 32, sy - 32, 64, 64, &rdsc);
    rdsc.radius = 0; rdsc.bg_opa = LV_OPA_COVER;

    // Ocean
    for (int y = hz; y < SCR_H - 18; y++) {
        float f = (float)(y - hz) / (SCR_H - 18 - hz);
        rdsc.bg_color = lv_color_make(0, (uint8_t)(20 + f * 40), (uint8_t)(60 + f * 40));
        lv_canvas_draw_rect(anim_canvas, 0, y, SCR_W, 1, &rdsc);
    }

    // Waves
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.width = 3; ldsc.color = lv_color_hex(0x99CCFF);
    for (int x = 0; x < SCR_W - 1; x++) {
        int y0 = hz + 5 + (int)(4 * sinf(x * 0.06f + t * 2.0f));
        int y1 = hz + 5 + (int)(4 * sinf((x+1) * 0.06f + t * 2.0f));
        lv_point_t pts[2] = {{x, y0}, {x+1, y1}};
        lv_canvas_draw_line(anim_canvas, pts, 2, &ldsc);
    }
    ldsc.width = 2; ldsc.color = lv_color_hex(0x66AADD);
    for (int x = 0; x < SCR_W - 1; x++) {
        int y0 = hz + 16 + (int)(3 * sinf(x * 0.08f + t * 1.5f));
        int y1 = hz + 16 + (int)(3 * sinf((x+1) * 0.08f + t * 1.5f));
        lv_point_t pts[2] = {{x, y0}, {x+1, y1}};
        lv_canvas_draw_line(anim_canvas, pts, 2, &ldsc);
    }

    // Sand
    rdsc.bg_color = lv_color_hex(0xC2A05B);
    lv_canvas_draw_rect(anim_canvas, 0, SCR_H - 18, SCR_W, 18, &rdsc);

    // Stars in upper sky
    rdsc.radius   = LV_RADIUS_CIRCLE;
    for (int s = 0; s < 30; s++) {
        int stx = (s * 137 + 11) % SCR_W;
        int sty = (s * 73 + 7)   % (hz / 2);
        uint8_t bri = (uint8_t)(100 + 80 * sinf(t * 0.5f + s));
        rdsc.bg_color = lv_color_make(bri, bri, (uint8_t)(bri > 20 ? bri - 20 : 0));
        lv_canvas_draw_rect(anim_canvas, stx, sty, 2, 2, &rdsc);
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

    // Decorative fish
    lv_obj_t* fish = lv_label_create(cd_scr);
    lv_obj_set_style_text_font(fish, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(fish, lv_color_hex(0x2D4A8A), LV_PART_MAIN);
    lv_label_set_text(fish, "><>   ><>   ><>");
    lv_obj_align(fish, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_scr_load(cd_scr);
}

static void countdown_tick_cb(lv_timer_t*) {
    if (!cd_lbl_time) return;
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
        anim_timer = lv_timer_create(countdown_tick_cb, 1000, nullptr);
        return;
    }

    // Full-screen canvas for other animations
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
            star_init();
            star_draw();
            anim_timer = lv_timer_create(star_timer_cb, 80, nullptr);
            break;
        case ANIM_AQUARIUM:
            aqua_init();
            anim_timer = lv_timer_create(aqua_timer_cb, 50, nullptr);
            break;
        case ANIM_BEACH:
            anim_timer = lv_timer_create(beach_timer_cb, 33, nullptr);
            break;
        default:
            star_init();
            star_draw();
            anim_timer = lv_timer_create(star_timer_cb, 80, nullptr);
            break;
    }

    lv_obj_add_event_cb(anim_scr, anim_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_scr_load(anim_scr);
}

void anim_stop() {
    anim_swipe = 0;
    if (anim_timer) { lv_timer_del(anim_timer); anim_timer = nullptr; }

    if (cur_type == ANIM_COUNTDOWN) {
        if (cd_scr) { lv_obj_del(cd_scr); cd_scr = nullptr; }
        cd_lbl_time = nullptr;
    } else {
        if (anim_scr) { lv_obj_del(anim_scr); anim_scr = nullptr; }
        if (anim_buf) { heap_caps_free(anim_buf); anim_buf = nullptr; }
        anim_canvas = nullptr;
    }
    cur_type = -1;
}

void anim_set_countdown(uint32_t secs) {
    countdown = secs;
}
