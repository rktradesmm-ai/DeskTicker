#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include "chart_screen.h"
#include "settings.h"
#include "assets.h"

// ── Color helpers ─────────────────────────────────────────────────────────────
static lv_color_t rgb32_to_lv(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static lv_color_t colorshift_bull() {
    uint8_t h = (uint8_t)((millis() / 50) & 0xFF);
    return lv_color_hsv_to_rgb(h, 80, 90);
}
static lv_color_t colorshift_bear() {
    uint8_t h = (uint8_t)(((millis() / 50) + 128) & 0xFF);
    return lv_color_hsv_to_rgb(h, 85, 85);
}

static lv_color_t neon_bull() {
    uint8_t v = (uint8_t)(75 + 25 * sinf(millis() * 0.005f));
    return lv_color_hsv_to_rgb(150, 90, v);
}
static lv_color_t neon_bear() {
    uint8_t v = (uint8_t)(75 + 25 * sinf(millis() * 0.005f + 1.5f));
    return lv_color_hsv_to_rgb(300, 90, v);
}

static void get_candle_colors(const Settings* s, lv_color_t* bull, lv_color_t* bear) {
    switch (s->theme) {
        case THEME_COLORSHIFT: *bull = colorshift_bull(); *bear = colorshift_bear(); break;
        case THEME_NEONPULSE:  *bull = neon_bull();       *bear = neon_bear();       break;
        case THEME_CUSTOM:     *bull = rgb32_to_lv(s->bull_rgb); *bear = rgb32_to_lv(s->bear_rgb); break;
        default:               *bull = lv_color_hex(0x26A69A); *bear = lv_color_hex(0xEF5350); break;
    }
}

// ── Theme colors ──────────────────────────────────────────────────────────────
#define COL_BG      lv_color_hex(0x131722)
#define COL_HDR     lv_color_hex(0x1E222D)
#define COL_FTR     lv_color_hex(0x1E222D)
#define COL_TEXT    lv_color_hex(0xD1D4DC)
#define COL_SUBTEXT lv_color_hex(0x787B86)
#define COL_GRID    lv_color_hex(0x2A2E39)
#define COL_WICK    lv_color_hex(0x5D6073)
#define COL_POS     lv_color_hex(0x26A69A)
#define COL_NEG     lv_color_hex(0xEF5350)
#define COL_WIFI_OK lv_color_hex(0x3FB950)
#define COL_WIFI_NO lv_color_hex(0xEF5350)
#define COL_CUR_PX  lv_color_hex(0x58A6FF)

// ── Swipe / gesture state ─────────────────────────────────────────────────────
static volatile int8_t chart_swipe      = 0;  // horizontal: set by LVGL task
static volatile int8_t chart_swipe_vert = 0;  // vertical: set by LVGL task

static void chart_gesture_cb(lv_event_t*) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if      (dir == LV_DIR_LEFT)   chart_swipe      =  1;  // next asset
    else if (dir == LV_DIR_RIGHT)  chart_swipe      = -1;  // prev asset
    else if (dir == LV_DIR_TOP)    chart_swipe_vert =  1;  // next timeframe
    else if (dir == LV_DIR_BOTTOM) chart_swipe_vert = -1;  // prev timeframe
}

int chart_screen_get_swipe() {
    int d = (int)chart_swipe;
    chart_swipe = 0;
    return d;
}

int chart_screen_get_swipe_vert() {
    int d = (int)chart_swipe_vert;
    chart_swipe_vert = 0;
    return d;
}

// ── Widget handles ────────────────────────────────────────────────────────────
static lv_obj_t*    chart_scr    = nullptr;
static lv_obj_t*    lbl_ticker   = nullptr;
static lv_obj_t*    lbl_price    = nullptr;
static lv_obj_t*    lbl_change   = nullptr;
static lv_obj_t*    lbl_tf       = nullptr;
static lv_obj_t*    lbl_date_time = nullptr;
static lv_obj_t*    lbl_status   = nullptr;
static lv_obj_t*    dot_wifi     = nullptr;
static lv_obj_t*    chart_canvas = nullptr;
static lv_obj_t*    yaxis_canvas = nullptr;
static lv_obj_t*    xaxis_canvas = nullptr;
static lv_obj_t*    lbl_ohlc     = nullptr;
static lv_color_t*  chart_buf    = nullptr;
static lv_color_t*  yaxis_buf    = nullptr;
static lv_color_t*  xaxis_buf    = nullptr;

// ── Previous-draw state (for incremental canvas update) ───────────────────────
// Track what the chart last drew so we only do a full repaint when something
// structurally changed (new candle, different asset, different timeframe).
// Between structural changes only the last candle body + dashed price line need
// updating — no reason to clear and repaint all 38 candles every 30 seconds.
static int     prev_candle_count = -1;
static uint32_t prev_last_ts     = 0;
static char     prev_symbol[ASSET_SYM_LEN] = {0};
static int      prev_timeframe   = -1;

// ── Format helpers ────────────────────────────────────────────────────────────
static void fmt_price(char* buf, size_t n, float v, int decimals) {
    if (v < 1.0f) {
        snprintf(buf, n, "%.*f", decimals + 2, (double)v);
    } else {
        snprintf(buf, n, "%.*f", decimals, (double)v);
    }
}


static const char* tf_label(int tf) {
    switch (tf) {
        case TF_15M: return "15m";
        case TF_1H:  return "1h";
        case TF_4H:  return "4h";
        default:     return "1D";
    }
}

// ── Price range from visible candles ─────────────────────────────────────────
static void price_range(const AssetData* d, float* out_lo, float* out_hi) {
    int vis   = (d->candle_count < MAX_VIS_CANDLES) ? d->candle_count : MAX_VIS_CANDLES;
    int start = d->candle_count - vis;

    float lo = d->candles[start].low;
    float hi = d->candles[start].high;
    for (int i = start + 1; i < d->candle_count; i++) {
        if (d->candles[i].low  < lo) lo = d->candles[i].low;
        if (d->candles[i].high > hi) hi = d->candles[i].high;
    }
    float pad = (hi - lo) * 0.03f;
    *out_lo = lo - pad;
    *out_hi = hi + pad;
}

// ── Draw candlestick canvas ───────────────────────────────────────────────────
static void draw_chart_canvas(const AssetData* d, const Settings* s) {
    if (!chart_canvas) return;

    int vis   = (d->candle_count < MAX_VIS_CANDLES) ? d->candle_count : MAX_VIS_CANDLES;
    int start = d->candle_count - vis;

    float p_lo, p_hi;
    price_range(d, &p_lo, &p_hi);
    float range = p_hi - p_lo;
    if (range < 1e-6f) range = 1.0f;

    int cw = CANVAS_W;

    // The plot region is inset CHART_BOX_MARGIN px from the top so there is
    // breathing room between the header and the box top line.  The same margin
    // is applied at the bottom (in draw_xaxis), making the gaps exactly equal.
    const int PLOT_TOP = CHART_BOX_MARGIN;   // first usable y row for candles
    const int PLOT_H   = CANVAS_H - PLOT_TOP; // height of the actual plot area

    lv_canvas_fill_bg(chart_canvas, COL_BG, LV_OPA_COVER);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius       = 0;
    rdsc.border_width = 0;

    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_COVER;

    // Box top line and left border (same grey as grid lines)
    ldsc.color = COL_GRID;
    lv_point_t box_top[2]  = {{0, PLOT_TOP}, {cw, PLOT_TOP}};
    lv_point_t box_left[2] = {{0, PLOT_TOP}, {0, CANVAS_H - 1}};
    lv_canvas_draw_line(chart_canvas, box_top,  2, &ldsc);
    lv_canvas_draw_line(chart_canvas, box_left, 2, &ldsc);

    // Horizontal grid lines — evenly spaced within the inset plot region
    const int GRID_N = 4;
    for (int g = 1; g <= GRID_N; g++) {
        int gy = PLOT_TOP + PLOT_H * g / (GRID_N + 1);
        ldsc.color = COL_GRID;
        lv_point_t pts[2] = {{0, gy}, {cw, gy}};
        lv_canvas_draw_line(chart_canvas, pts, 2, &ldsc);
    }

    lv_color_t bull_col, bear_col;
    get_candle_colors(s, &bull_col, &bear_col);

    // Draw candles right-aligned, mapped into the inset plot region
    for (int i = d->candle_count - 1; i >= start; i--) {
        const Candle* c = &d->candles[i];
        int offset = d->candle_count - 1 - i;
        int xc = cw - 2 - (CHART_RIGHT_PAD + offset) * CANDLE_STEP;  // right edge of body
        if (xc < 0) break;

        bool bull = c->close >= c->open;
        lv_color_t col = bull ? bull_col : bear_col;

        int y_open  = PLOT_TOP + (int)((p_hi - c->open)  / range * PLOT_H);
        int y_close = PLOT_TOP + (int)((p_hi - c->close) / range * PLOT_H);
        int y_high  = PLOT_TOP + (int)((p_hi - c->high)  / range * PLOT_H);
        int y_low   = PLOT_TOP + (int)((p_hi - c->low)   / range * PLOT_H);

        // Clamp to the inset plot region
        if (y_high  < PLOT_TOP)    y_high  = PLOT_TOP;
        if (y_low   >= CANVAS_H)   y_low   = CANVAS_H - 1;
        if (y_open  < PLOT_TOP)    y_open  = PLOT_TOP;
        if (y_close < PLOT_TOP)    y_close = PLOT_TOP;
        if (y_open  >= CANVAS_H)   y_open  = CANVAS_H - 1;
        if (y_close >= CANVAS_H)   y_close = CANVAS_H - 1;

        int body_top = bull ? y_close : y_open;
        int body_bot = bull ? y_open  : y_close;
        if (body_bot <= body_top) body_bot = body_top + 1;

        int left  = xc - CANDLE_W + 1;
        int mid_x = left + CANDLE_W / 2;

        // Upper wick
        if (y_high < body_top) {
            ldsc.color = COL_WICK;
            lv_point_t wpts[2] = {{mid_x, y_high}, {mid_x, body_top}};
            lv_canvas_draw_line(chart_canvas, wpts, 2, &ldsc);
        }
        // Lower wick
        if (y_low > body_bot) {
            ldsc.color = COL_WICK;
            lv_point_t wpts[2] = {{mid_x, body_bot}, {mid_x, y_low}};
            lv_canvas_draw_line(chart_canvas, wpts, 2, &ldsc);
        }
        // Body
        rdsc.bg_color = col;
        lv_canvas_draw_rect(chart_canvas, left, body_top, CANDLE_W, body_bot - body_top, &rdsc);
    }

    // Dashed current-price line: rightmost candle right-edge → y-axis
    if (d->price >= p_lo && d->price <= p_hi) {
        int gy = PLOT_TOP + (int)((p_hi - d->price) / range * PLOT_H);
        int x_start = cw - 2 - CHART_RIGHT_PAD * CANDLE_STEP + 1;
        if (x_start < 0) x_start = 0;
        ldsc.color = COL_CUR_PX;
        for (int x = x_start; x < cw; x += 9) {
            int x2 = x + 4;
            if (x2 >= cw) x2 = cw - 1;
            lv_point_t pts[2] = {{x, gy}, {x2, gy}};
            lv_canvas_draw_line(chart_canvas, pts, 2, &ldsc);
        }
    }
}

// ── Draw Y-axis labels ────────────────────────────────────────────────────────
static void draw_yaxis(const AssetData* d, const Settings* s) {
    if (!yaxis_canvas) return;

    float p_lo, p_hi;
    price_range(d, &p_lo, &p_hi);
    float range = p_hi - p_lo;
    if (range < 1e-6f) range = 1.0f;

    const AssetDef* def = asset_find(d->symbol);
    int dec = def ? def->decimals : 2;

    lv_canvas_fill_bg(yaxis_canvas, COL_BG, LV_OPA_COVER);

    lv_draw_label_dsc_t tdsc;
    lv_draw_label_dsc_init(&tdsc);
    tdsc.font  = &lv_font_montserrat_14;
    tdsc.color = lv_color_hex(0xB2B5BE);
    tdsc.align = LV_TEXT_ALIGN_LEFT;

    // Gridline extension descriptor — same style as chart canvas gridlines
    lv_draw_line_dsc_t gldsc;
    lv_draw_line_dsc_init(&gldsc);
    gldsc.color = COL_GRID;
    gldsc.width = 1;
    gldsc.opa   = LV_OPA_COVER;

    const int GRID_N = 4;
    const int GRID_LABEL_GAP = 16;  // ~2 digit-char widths gap before the label text

    // Match the same inset plot region used in draw_chart_canvas so that
    // gridline labels and the current-price pill align with the drawn candles.
    const int PLOT_TOP = CHART_BOX_MARGIN;
    const int PLOT_H   = CANVAS_H - PLOT_TOP;

    // Pass 1: find the widest price label so all lines end at the same x.
    int max_txt_w = 0;
    for (int g = 1; g <= GRID_N; g++) {
        float gp = p_lo + range * g / (GRID_N + 1);
        char buf[20];
        fmt_price(buf, sizeof(buf), gp, dec);
        lv_point_t sz;
        lv_txt_get_size(&sz, buf, &lv_font_montserrat_14, 0, 0, YAXIS_W, LV_TEXT_FLAG_NONE);
        if (sz.x > max_txt_w) max_txt_w = sz.x;
    }
    // Common right end for every line — gap before the widest label.
    // Clamped to a minimum of 2px so the box right border always draws even for
    // long prices like BTC ("78987.06") where the label nearly fills YAXIS_W.
    int line_end_x = YAXIS_W - max_txt_w - GRID_LABEL_GAP;
    const int MIN_LINE_END = 2;
    if (line_end_x < MIN_LINE_END) line_end_x = MIN_LINE_END;

    // Box top-line continuation and right border vertical in yaxis_canvas.
    // Together with the chart_canvas top/left lines and the xaxis bottom line
    // this closes the rectangular box around the candle plot area.
    if (line_end_x > 0) {
        lv_point_t top_ext[2]   = {{0, PLOT_TOP}, {line_end_x, PLOT_TOP}};
        lv_point_t right_bdr[2] = {{line_end_x, PLOT_TOP}, {line_end_x, CANVAS_H}};
        lv_canvas_draw_line(yaxis_canvas, top_ext,   2, &gldsc);
        lv_canvas_draw_line(yaxis_canvas, right_bdr, 2, &gldsc);
    }

    // Pass 2: draw each gridline extension to the common end, then the label.
    for (int g = 1; g <= GRID_N; g++) {
        float gp = p_lo + range * g / (GRID_N + 1);
        // Use the same integer pixel formula as draw_chart_canvas() so the two
        // canvas segments of each grid line meet exactly at the seam.
        // gp ascends with g (low price first) so its pixel row counts from the
        // bottom: row = GRID_N+1-g from the top, matching draw_chart_canvas g.
        int   gy = PLOT_TOP + PLOT_H * (GRID_N + 1 - g) / (GRID_N + 1);

        char buf[20];
        fmt_price(buf, sizeof(buf), gp, dec);

        lv_point_t sz;
        lv_txt_get_size(&sz, buf, &lv_font_montserrat_14, 0, 0, YAXIS_W, LV_TEXT_FLAG_NONE);
        int draw_x = YAXIS_W - sz.x;
        if (draw_x < 0) draw_x = 0;

        if (line_end_x > 0) {
            lv_point_t pts[2] = {{0, gy}, {line_end_x, gy}};
            lv_canvas_draw_line(yaxis_canvas, pts, 2, &gldsc);
        }

        lv_canvas_draw_text(yaxis_canvas, draw_x, gy - 7, sz.x + 1, &tdsc, buf);
    }

    // Bottom x-axis continuation at y=CANVAS_H — same row as the x-axis line in
    // xaxis_canvas, within the taller yaxis_canvas (CANVAS_H 196 < CHART_H 224).
    if (line_end_x > 0) {
        lv_point_t xext[2] = {{0, CANVAS_H}, {line_end_x, CANVAS_H}};
        lv_canvas_draw_line(yaxis_canvas, xext, 2, &gldsc);
    }

    // Current price pill — right-aligned to yaxis edge, width = text + ~2 char padding
    float cur = d->price;
    if (cur >= p_lo && cur <= p_hi) {
        int gy = PLOT_TOP + (int)((p_hi - cur) / range * PLOT_H);

        char buf[20];
        fmt_price(buf, sizeof(buf), cur, dec);

        // Measure text at pill font then size the pill to text_width + 2-char padding
        // so it sits flush with the right edge (matching grid labels) and scales with
        // price string length across different assets.
        lv_point_t sz;
        lv_txt_get_size(&sz, buf, &lv_font_montserrat_12, 0, 0, YAXIS_W, LV_TEXT_FLAG_NONE);
        const int PILL_PAD = 16;  // ~2 digit-char widths total: 8px breathing room each side
        int pill_w = sz.x + PILL_PAD;
        if (pill_w > YAXIS_W) pill_w = YAXIS_W;
        int pill_x = YAXIS_W - pill_w;  // right edge flush with yaxis canvas right edge

        const int LBL_H = 17;
        int lbl_y = gy - LBL_H / 2;
        if (lbl_y < PLOT_TOP)           lbl_y = PLOT_TOP;
        if (lbl_y + LBL_H >= CANVAS_H)  lbl_y = CANVAS_H - LBL_H - 1;

        // Extend dashed line through the yaxis canvas gap up to the pill left edge
        if (pill_x > 0) {
            lv_draw_line_dsc_t ldsc;
            lv_draw_line_dsc_init(&ldsc);
            ldsc.color = COL_CUR_PX;
            ldsc.width = 1;
            ldsc.opa   = LV_OPA_COVER;
            for (int x = 0; x < pill_x; x += 9) {
                int x2 = x + 4;
                if (x2 >= pill_x) x2 = pill_x - 1;
                lv_point_t pts[2] = {{x, gy}, {x2, gy}};
                lv_canvas_draw_line(yaxis_canvas, pts, 2, &ldsc);
            }
        }

        lv_draw_rect_dsc_t rdsc;
        lv_draw_rect_dsc_init(&rdsc);
        rdsc.radius       = 0;
        rdsc.border_width = 0;
        rdsc.bg_color     = COL_CUR_PX;
        lv_canvas_draw_rect(yaxis_canvas, pill_x, lbl_y, pill_w, LBL_H, &rdsc);

        lv_draw_label_dsc_t pdsc;
        lv_draw_label_dsc_init(&pdsc);
        pdsc.font  = &lv_font_montserrat_12;
        pdsc.color = COL_BG;
        pdsc.align = LV_TEXT_ALIGN_LEFT;
        // Explicitly center text within the pill (avoids LVGL canvas alignment quirks)
        int text_x = pill_x + (pill_w - sz.x) / 2;
        lv_canvas_draw_text(yaxis_canvas, text_x, lbl_y + 2, sz.x + 1, &pdsc, buf);
    }
}

// ── Draw X-axis date/time labels ──────────────────────────────────────────────
static void draw_xaxis(const AssetData* d, const Settings* s) {
    if (!xaxis_canvas) return;

    lv_canvas_fill_bg(xaxis_canvas, COL_BG, LV_OPA_COVER);

    int vis   = (d->candle_count < MAX_VIS_CANDLES) ? d->candle_count : MAX_VIS_CANDLES;
    int start = d->candle_count - vis;

    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = COL_GRID;
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_COVER;

    // Horizontal axis line across the full width at the top of the label strip
    lv_point_t axis[2] = {{0, 0}, {CANVAS_W, 0}};
    lv_canvas_draw_line(xaxis_canvas, axis, 2, &ldsc);

    lv_draw_label_dsc_t tdsc;
    lv_draw_label_dsc_init(&tdsc);
    tdsc.font  = &lv_font_montserrat_10;
    tdsc.color = COL_SUBTEXT;
    tdsc.align = LV_TEXT_ALIGN_LEFT;

    // Label every 6th candle from right so the newest candle always anchors the first label
    const int label_every = 6;

    for (int i = d->candle_count - 1; i >= start; i -= label_every) {
        int offset = d->candle_count - 1 - i;
        int xc = CANVAS_W - 2 - (CHART_RIGHT_PAD + offset) * CANDLE_STEP;
        if (xc < 0 || xc >= CANVAS_W) continue;

        // Format: HH:MM for intraday timeframes, M/D for daily and above
        char buf[10];
        time_t ts = (time_t)d->candles[i].ts;
        struct tm lt;
        localtime_r(&ts, &lt);
        if (s->timeframe == TF_15M || s->timeframe == TF_1H) {
            snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
        } else {
            snprintf(buf, sizeof(buf), "%d/%d", lt.tm_mon + 1, lt.tm_mday);
        }

        // Short tick mark
        lv_point_t tick[2] = {{xc, 0}, {xc, 4}};
        lv_canvas_draw_line(xaxis_canvas, tick, 2, &ldsc);

        // Label centered on xc.
        // Position vertically so the gap between the label bottom and the footer
        // equals CHART_BOX_MARGIN — matching the top gap above the box top line.
        lv_point_t sz;
        lv_txt_get_size(&sz, buf, &lv_font_montserrat_10, 0, 0, 60, LV_TEXT_FLAG_NONE);
        int lx = xc - sz.x / 2;
        if (lx < 0) lx = 0;
        if (lx + sz.x >= CANVAS_W) lx = CANVAS_W - sz.x - 1;
        int ly = XAXIS_H - CHART_BOX_MARGIN - sz.y;
        if (ly < 6) ly = 6;  // keep below the tick mark (4px) with a small gap
        lv_canvas_draw_text(xaxis_canvas, lx, ly, sz.x + 1, &tdsc, buf);
    }
}

// ── Create chart screen ───────────────────────────────────────────────────────
void chart_screen_create(const Settings* s) {
    chart_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(chart_scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart_scr, 0, LV_PART_MAIN);

    // ── Header ──────────────────────────────────────────────────────────────
    lv_obj_t* hdr = lv_obj_create(chart_scr);
    lv_obj_set_size(hdr, SCR_W, HDR_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_HDR, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Left flex row: ticker · price  change%  (auto-sizes with ticker width)
    lv_obj_t* left_cont = lv_obj_create(hdr);
    lv_obj_remove_style_all(left_cont);
    lv_obj_set_size(left_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(left_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cont, 2, 0);
    lv_obj_align(left_cont, LV_ALIGN_LEFT_MID, 8, 0);

    lbl_ticker = lv_label_create(left_cont);
    lv_obj_set_style_text_font(lbl_ticker, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_ticker, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(lbl_ticker, 6, 0);  // ticker→| gap: 2 (pad_column) + 6 = 8px
    lv_label_set_text(lbl_ticker, "--");

    lv_obj_t* lbl_dot = lv_obj_create(left_cont);
    lv_obj_remove_style_all(lbl_dot);
    lv_obj_set_size(lbl_dot, 2, 18);
    lv_obj_set_style_radius(lbl_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lbl_dot, COL_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lbl_dot, LV_OPA_COVER, LV_PART_MAIN);

    lbl_price = lv_label_create(left_cont);
    lv_obj_set_style_text_font(lbl_price, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_price, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_left(lbl_price, 6, 0);  // ticker→| gap: 2 (pad_column) + 6 = 8px
    lv_label_set_text(lbl_price, "--");

    lbl_change = lv_label_create(left_cont);
    lv_obj_set_style_text_font(lbl_change, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_pad_left(lbl_change, 6, 0);  // single-space gap before change%
    lv_label_set_text(lbl_change, "+0.00%");

    // Date + time combined — single label left of the timeframe label.
    // Format: "16 May | 15:51" (day with leading zero, 3-letter month, 24h).
    lbl_date_time = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_date_time, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_date_time, COL_SUBTEXT, LV_PART_MAIN);
    lv_label_set_text(lbl_date_time, "-- --- | --:--");
    lv_obj_align(lbl_date_time, LV_ALIGN_RIGHT_MID, -60, 0);

    // Timeframe — right edge, same font size as change%
    lbl_tf = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_tf, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_tf, COL_SUBTEXT, LV_PART_MAIN);
    lv_label_set_text(lbl_tf, tf_label(s->timeframe));
    lv_obj_align(lbl_tf, LV_ALIGN_RIGHT_MID, -8, 0);

    // ── Chart area ───────────────────────────────────────────────────────────
    lv_obj_t* chart_cont = lv_obj_create(chart_scr);
    lv_obj_set_size(chart_cont, SCR_W, CHART_H);
    lv_obj_align(chart_cont, LV_ALIGN_TOP_LEFT, 0, HDR_H);
    lv_obj_set_style_bg_color(chart_cont, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart_cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chart_cont, LV_OBJ_FLAG_SCROLLABLE);

    // Main chart canvas (left side, full width minus y-axis)
    chart_buf = (lv_color_t*)heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (chart_buf) {
        chart_canvas = lv_canvas_create(chart_cont);
        lv_canvas_set_buffer(chart_canvas, chart_buf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(chart_canvas, LEFT_MARGIN, 0);
        lv_canvas_fill_bg(chart_canvas, COL_BG, LV_OPA_COVER);
    }

    // Y-axis canvas (right strip)
    yaxis_buf = (lv_color_t*)heap_caps_malloc(YAXIS_W * CHART_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (yaxis_buf) {
        yaxis_canvas = lv_canvas_create(chart_cont);
        lv_canvas_set_buffer(yaxis_canvas, yaxis_buf, YAXIS_W, CHART_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(yaxis_canvas, LEFT_MARGIN + CANVAS_W, 0);
        lv_canvas_fill_bg(yaxis_canvas, COL_BG, LV_OPA_COVER);
    }

    // X-axis label strip (28 px below the chart canvas, same x-range)
    xaxis_buf = (lv_color_t*)heap_caps_malloc(CANVAS_W * XAXIS_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (xaxis_buf) {
        xaxis_canvas = lv_canvas_create(chart_cont);
        lv_canvas_set_buffer(xaxis_canvas, xaxis_buf, CANVAS_W, XAXIS_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(xaxis_canvas, LEFT_MARGIN, CANVAS_H);
        lv_canvas_fill_bg(xaxis_canvas, COL_BG, LV_OPA_COVER);
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    lv_obj_t* ftr = lv_obj_create(chart_scr);
    lv_obj_set_size(ftr, SCR_W, FTR_H);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ftr, COL_FTR, LV_PART_MAIN);
    lv_obj_set_style_border_width(ftr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ftr, 4, LV_PART_MAIN);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);

    lbl_ohlc = lv_label_create(ftr);
    lv_obj_set_style_text_font(lbl_ohlc, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_ohlc, COL_SUBTEXT, LV_PART_MAIN);
    lv_label_set_text(lbl_ohlc, "O:-- H:-- L:-- C:--");
    lv_obj_align(lbl_ohlc, LV_ALIGN_LEFT_MID, 4, 0);

    // Fetch/error status — shown left of wifi dot during background refresh
    lbl_status = lv_label_create(ftr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status, COL_SUBTEXT, LV_PART_MAIN);
    lv_label_set_text(lbl_status, "");
    lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -24, 0);

    // WiFi status dot (right side of footer)
    dot_wifi = lv_obj_create(ftr);
    lv_obj_set_size(dot_wifi, 10, 10);
    lv_obj_set_style_radius(dot_wifi, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot_wifi, COL_WIFI_NO, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot_wifi, 0, LV_PART_MAIN);
    lv_obj_align(dot_wifi, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_add_event_cb(chart_scr, chart_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_scr_load(chart_scr);
}

// ── Re-activate chart screen (after conn_scr displaced it) ───────────────────
void chart_screen_show() {
    if (chart_scr) lv_scr_load(chart_scr);
}

// ── Background fetch status in header ────────────────────────────────────────
void chart_screen_set_status(const char* msg) {
    if (lbl_status) lv_label_set_text(lbl_status, msg ? msg : "");
}

// ── Update chart content ──────────────────────────────────────────────────────
void chart_screen_update(const AssetData* d, const Settings* s, bool wifi_ok) {
    if (!chart_scr) return;

    lv_obj_set_style_bg_color(dot_wifi, wifi_ok ? COL_WIFI_OK : COL_WIFI_NO, LV_PART_MAIN);

    // Update header date + time
    if (lbl_date_time) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        static const char* MON[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        char buf[20];
        snprintf(buf, sizeof(buf), "%02d %s | %02d:%02d",
                 lt.tm_mday, MON[lt.tm_mon], lt.tm_hour, lt.tm_min);
        lv_label_set_text(lbl_date_time, buf);
    }

    if (!d || !d->valid) {
        lv_label_set_text(lbl_ticker, d ? d->symbol : "...");
        lv_label_set_text(lbl_price,  "No data");
        lv_label_set_text(lbl_change, "");
        return;
    }

    const AssetDef* def = asset_find(d->symbol);
    int dec = def ? def->decimals : 2;

    lv_label_set_text(lbl_ticker, d->symbol);
    lv_label_set_text(lbl_tf, tf_label(s->timeframe));

    // Price
    char pbuf[20];
    fmt_price(pbuf, sizeof(pbuf), d->price, dec);
    char fprice[26];
    if (def && (def->market == MARKET_STOCK || def->market == MARKET_COMMODITY)) {
        snprintf(fprice, sizeof(fprice), "$%s", pbuf);
    } else {
        strncpy(fprice, pbuf, sizeof(fprice));
    }
    lv_label_set_text(lbl_price, fprice);

    // Change %
    char cbuf[18];
    snprintf(cbuf, sizeof(cbuf), "%+.2f%%", (double)d->change_pct);
    lv_label_set_text(lbl_change, cbuf);
    lv_obj_set_style_text_color(lbl_change, d->change_pct >= 0 ? COL_POS : COL_NEG, LV_PART_MAIN);

    // OHLC from last candle
    if (d->candle_count > 0) {
        const Candle* last = &d->candles[d->candle_count - 1];
        char ob[14], hb[14], lb[14], cb[14];
        fmt_price(ob, sizeof(ob), last->open,  dec);
        fmt_price(hb, sizeof(hb), last->high,  dec);
        fmt_price(lb, sizeof(lb), last->low,   dec);
        fmt_price(cb, sizeof(cb), last->close, dec);

        char ohlc[72];
        snprintf(ohlc, sizeof(ohlc), "O:%s  H:%s  L:%s  C:%s", ob, hb, lb, cb);
        lv_label_set_text(lbl_ohlc, ohlc);
    }

    // Decide whether to do a full redraw or just update the last candle + price.
    // A full redraw is needed when the candle set changed (new bar arrived),
    // when the asset or timeframe switched, or on the very first draw.
    // Otherwise — same bar, only the live price or last candle OHLC changed —
    // skip the x-axis (time labels are unchanged) and only repaint the candle
    // canvas and y-axis so the last candle body + dashed price line move
    // smoothly without flashing the whole chart.
    if (d->candle_count > 1) {
        uint32_t cur_last_ts = d->candles[d->candle_count - 1].ts;
        bool full_redraw = (d->candle_count != prev_candle_count) ||
                           (cur_last_ts != prev_last_ts) ||
                           (strncmp(d->symbol, prev_symbol, ASSET_SYM_LEN) != 0) ||
                           (s->timeframe != prev_timeframe);

        // Store current state for next comparison
        prev_candle_count = d->candle_count;
        prev_last_ts      = cur_last_ts;
        strncpy(prev_symbol, d->symbol, ASSET_SYM_LEN - 1);
        prev_symbol[ASSET_SYM_LEN - 1] = '\0';
        prev_timeframe    = s->timeframe;

        draw_chart_canvas(d, s);
        draw_yaxis(d, s);
        if (full_redraw) draw_xaxis(d, s);
    }
}

// ── Cleanup ───────────────────────────────────────────────────────────────────
void chart_screen_destroy() {
    chart_swipe      = 0;
    chart_swipe_vert = 0;
    // Reset prev-draw state so the next chart creation forces a full repaint.
    prev_candle_count = -1;
    prev_last_ts      = 0;
    prev_symbol[0]    = '\0';
    prev_timeframe    = -1;
    if (chart_scr)  { lv_obj_del(chart_scr); chart_scr = nullptr; }
    if (chart_buf)  { heap_caps_free(chart_buf);  chart_buf  = nullptr; }
    if (yaxis_buf)  { heap_caps_free(yaxis_buf);  yaxis_buf  = nullptr; }
    if (xaxis_buf)  { heap_caps_free(xaxis_buf);  xaxis_buf  = nullptr; }
    chart_canvas = yaxis_canvas = xaxis_canvas = nullptr;
    lbl_ticker = lbl_price = lbl_change = lbl_tf = lbl_date_time = lbl_status = dot_wifi = nullptr;
    lbl_ohlc = nullptr;
}
