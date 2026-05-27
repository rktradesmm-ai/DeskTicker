// On-device settings menu for DeskTicker.
// Opened by holding the touchscreen for 3+ seconds from the chart or
// after-hours screen. Edits a working copy of Settings; only writes to
// NVS on Save (followed by ESP.restart()).

#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <time.h>
#include "settings_screen.h"
#include "settings.h"
#include "assets.h"
#include "tz_options.h"
#include "chart_screen.h"   // SCR_W, SCR_H
#include "display.h"        // bsp_display_brightness_set

// ── Color constants (match chart_screen dark theme) ───────────────────────────
#define SS_BG       lv_color_hex(0x0E1117)   // Deep Space
#define SS_HDR      lv_color_hex(0x1E222D)
#define SS_TEXT     lv_color_hex(0xE6E9EF)   // Pearl
#define SS_SUBTEXT  lv_color_hex(0x7A8290)   // Tide Gray
#define SS_DIVIDER  lv_color_hex(0x2A2E39)
#define SS_GREEN    lv_color_hex(0x22C55E)   // Bull Green
#define SS_RED      lv_color_hex(0xEF4444)   // Bear Red
#define SS_AMBER    lv_color_hex(0xF59E0B)   // Sand Amber
#define SS_BLUE     lv_color_hex(0x22C55E)   // Bull Green (replaces blue as active accent)
#define SS_ROW_SEL  lv_color_hex(0x1E2533)   // pressed/selected row highlight

// ── Layout ────────────────────────────────────────────────────────────────────
#define HDR_H   44
#define CONT_H  (SCR_H - HDR_H)   // 276
#define ROW_H   46                 // touch-target height for list rows
#define BTN_H   44                 // action button height

// ── Working copy and result flag ──────────────────────────────────────────────
static Settings          ss_work;
static volatile int8_t   ss_result = 0;   // 0 editing / 1 save / -1 cancel

// ── Screen and navigation ─────────────────────────────────────────────────────
static lv_obj_t* ss_scr       = nullptr;
static lv_obj_t* ss_hdr       = nullptr;
static lv_obj_t* hdr_title    = nullptr;
static lv_obj_t* btn_back     = nullptr;
static lv_obj_t* cur_page     = nullptr;

// Pages (only one visible at a time inside ss_scr)
static lv_obj_t* page_main   = nullptr;
static lv_obj_t* page_assets = nullptr;
static lv_obj_t* page_tf     = nullptr;
static lv_obj_t* page_tz     = nullptr;
static lv_obj_t* page_theme  = nullptr;
static lv_obj_t* page_anim   = nullptr;
static lv_obj_t* page_cycle  = nullptr;
static lv_obj_t* page_bri    = nullptr;
static lv_obj_t* page_diag   = nullptr;

// Widget handles needed across callbacks
static lv_obj_t* asset_count_lbl  = nullptr;
static lv_obj_t* asset_cbs[TOTAL_ASSETS];

static lv_obj_t* tf_cbs[4]        = {nullptr};

static lv_obj_t* tz_roller        = nullptr;
static lv_obj_t* theme_rows[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* theme_checks[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* anim_roller      = nullptr;

static lv_obj_t* cycle_sw         = nullptr;
static lv_obj_t* cycle_slider     = nullptr;
static lv_obj_t* cycle_val_lbl    = nullptr;

static lv_obj_t* bri_slider       = nullptr;
static lv_obj_t* bri_val_lbl      = nullptr;

// Main-page value labels (updated when sub-pages are modified)
static lv_obj_t* main_val_assets  = nullptr;
static lv_obj_t* main_val_tf      = nullptr;
static lv_obj_t* main_val_tz      = nullptr;
static lv_obj_t* main_val_theme   = nullptr;
static lv_obj_t* main_val_anim    = nullptr;
static lv_obj_t* main_val_cycle   = nullptr;
static lv_obj_t* main_val_bri     = nullptr;

// Diagnostics labels
static lv_obj_t* diag_heap_lbl    = nullptr;
static lv_obj_t* diag_psram_lbl   = nullptr;
static lv_obj_t* diag_wifi_lbl    = nullptr;
static lv_obj_t* diag_uptime_lbl  = nullptr;
static lv_obj_t* diag_fw_lbl      = nullptr;

// ── Style helpers ─────────────────────────────────────────────────────────────

// Apply dark background to any container-like object.
static void style_container(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj,    SS_BG,       LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj,      LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0,           LV_PART_MAIN);
    lv_obj_set_style_radius(obj,       0,           LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj,      0,           LV_PART_MAIN);
}

// Standard clickable row: dark bg with a 1-px bottom divider.
static void style_row(lv_obj_t* obj) {
    style_container(obj);
    lv_obj_set_style_border_side(obj,  LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, SS_DIVIDER,             LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1,                      LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj,     SS_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(obj,     12,         LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj,    12,         LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj,   LV_OBJ_FLAG_CLICKABLE);
}

// Create a scrollable page container positioned below the header.
static lv_obj_t* make_page(bool hidden) {
    lv_obj_t* p = lv_obj_create(ss_scr);
    lv_obj_set_size(p, SCR_W, CONT_H);
    lv_obj_set_pos(p, 0, HDR_H);
    style_container(p);
    lv_obj_set_style_pad_left(p,   10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(p,  10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(p,    10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(p, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    if (hidden) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

// Saved scroll position of the main page — restored when back is pressed.
static lv_coord_t s_main_scroll_y = 0;

// Navigation: show a page and update the header.
static void show_page(lv_obj_t* page, const char* title) {
    if (cur_page) {
        // Save main page position before navigating away from it.
        if (cur_page == page_main)
            s_main_scroll_y = lv_obj_get_scroll_y(page_main);
        lv_obj_add_flag(cur_page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
    // Returning to main: restore where the user was. Sub-pages always start at top.
    if (page == page_main)
        lv_obj_scroll_to_y(page, s_main_scroll_y, LV_ANIM_OFF);
    else
        lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    cur_page = page;
    lv_label_set_text(hdr_title, title);
    if (page == page_main) {
        lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    }
}

// Create a standard navigation row: label on left, value + arrow on right.
// Returns the row object. value may be nullptr for no right-side label.
static lv_obj_t* make_nav_row(lv_obj_t* parent, const char* label,
                               lv_obj_t** val_label_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    style_row(row);

    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, SS_TEXT,                LV_PART_MAIN);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* arrow = lv_label_create(row);
    lv_obj_set_style_text_color(arrow, SS_SUBTEXT, LV_PART_MAIN);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

    if (val_label_out) {
        lv_obj_t* val = lv_label_create(row);
        lv_obj_set_style_text_font(val,  &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(val, SS_SUBTEXT,             LV_PART_MAIN);
        lv_label_set_text(val, "");
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -18, 0);
        *val_label_out = val;
    }

    return row;
}

// Create a full-width colored action button.
static lv_obj_t* make_action_btn(lv_obj_t* parent, const char* text,
                                  lv_color_t color) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), BTN_H);
    lv_obj_set_style_bg_color(btn, color,     LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn,  LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn,  0,            LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0,       LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0,       LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_darken(color, LV_OPA_20),
                               LV_STATE_PRESSED);
    lv_obj_set_style_pad_top(btn, 0,    LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 0, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, SS_TEXT,                LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

// Create a spacer row of given height (non-clickable).
static void make_spacer(lv_obj_t* parent, int h) {
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_set_size(s, LV_PCT(100), h);
    style_container(s);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
}

// Create a section header label (non-clickable, smaller, subtext color).
static void make_section_label(lv_obj_t* parent, const char* text) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 28);
    style_container(row);
    lv_obj_set_style_bg_color(row, SS_HDR, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

// ── Helpers: update main-page value summary labels ────────────────────────────

static void update_main_assets_label() {
    if (!main_val_assets) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/6", ss_work.asset_count);
    lv_label_set_text(main_val_assets, buf);
}

static void update_main_tf_label() {
    if (!main_val_tf) return;
    char buf[20] = {0};
    for (int i = 0; i < ss_work.timeframe_count; i++) {
        if (i) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
        switch (ss_work.timeframes[i]) {
            case 15:   strncat(buf, "15m", sizeof(buf) - strlen(buf) - 1); break;
            case 60:   strncat(buf, "1h",  sizeof(buf) - strlen(buf) - 1); break;
            case 240:  strncat(buf, "4h",  sizeof(buf) - strlen(buf) - 1); break;
            case 1440: strncat(buf, "1D",  sizeof(buf) - strlen(buf) - 1); break;
        }
    }
    lv_label_set_text(main_val_tf, buf);
}

static void update_main_tz_label() {
    if (!main_val_tz) return;
    for (int i = 0; i < TZ_OPTS_N; i++) {
        if (TZ_OPTS[i].minutes == ss_work.tz_offset) {
            // Show only up to "UTC+XX:XX" portion (first word)
            char buf[16];
            const char* sp = strchr(TZ_OPTS[i].label, ' ');
            if (sp) {
                int n = sp - TZ_OPTS[i].label;
                if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
                strncpy(buf, TZ_OPTS[i].label, n);
                buf[n] = '\0';
            } else {
                strncpy(buf, TZ_OPTS[i].label, sizeof(buf) - 1);
                buf[sizeof(buf)-1] = '\0';
            }
            lv_label_set_text(main_val_tz, buf);
            return;
        }
    }
    lv_label_set_text(main_val_tz, "UTC+0");
}

static void update_main_theme_label() {
    if (!main_val_theme) return;
    const char* names[] = {"Classic", "ColorShift", "NeonPulse", "Custom"};
    int t = ss_work.theme;
    if (t < 0 || t > 3) t = 0;
    lv_label_set_text(main_val_theme, names[t]);
}

static void update_main_anim_label() {
    if (!main_val_anim) return;
    const char* names[] = {"Tidepool", "Coral Reef", "Starfield", "Countdown",
                           "Pixel Beach", "Market Pit"};
    int a = ss_work.after_anim;
    if (a < 0 || a > 5) a = 0;
    lv_label_set_text(main_val_anim, names[a]);
}

static void update_main_cycle_label() {
    if (!main_val_cycle) return;
    if (ss_work.cycle_secs == 0) {
        lv_label_set_text(main_val_cycle, "Manual");
    } else {
        char buf[12];
        snprintf(buf, sizeof(buf), "Auto %ds", ss_work.cycle_secs);
        lv_label_set_text(main_val_cycle, buf);
    }
}

static void update_main_bri_label() {
    if (!main_val_bri) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", ss_work.brightness);
    lv_label_set_text(main_val_bri, buf);
}

// ── Asset count label update ──────────────────────────────────────────────────

static void refresh_asset_count_lbl() {
    if (!asset_count_lbl) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "Select up to 6  (%d/6)", ss_work.asset_count);
    lv_label_set_text(asset_count_lbl, buf);
}

// Rebuild ss_work.assets[] from the current checkbox state.
static void sync_assets_from_cbs() {
    ss_work.asset_count = 0;
    for (int i = 0; i < TOTAL_ASSETS; i++) {
        if (!asset_cbs[i]) continue;
        if (lv_obj_has_state(asset_cbs[i], LV_STATE_CHECKED)) {
            strncpy(ss_work.assets[ss_work.asset_count],
                    ASSETS[i].symbol, ASSET_SYM_LEN - 1);
            ss_work.assets[ss_work.asset_count][ASSET_SYM_LEN - 1] = '\0';
            ss_work.asset_count++;
        }
    }
}

// Rebuild ss_work.timeframes[] from TF checkboxes and keep active timeframe
// inside the enabled set.
static void sync_tf_from_cbs() {
    static const int TF_VALS[4] = {15, 60, 240, 1440};
    ss_work.timeframe_count = 0;
    for (int i = 0; i < 4; i++) {
        if (!tf_cbs[i]) continue;
        if (lv_obj_has_state(tf_cbs[i], LV_STATE_CHECKED)) {
            ss_work.timeframes[ss_work.timeframe_count++] = TF_VALS[i];
        }
    }
    // Ensure active timeframe is within the enabled set
    bool found = false;
    for (int i = 0; i < ss_work.timeframe_count; i++) {
        if (ss_work.timeframes[i] == ss_work.timeframe) { found = true; break; }
    }
    if (!found && ss_work.timeframe_count > 0)
        ss_work.timeframe = ss_work.timeframes[0];
}

// ── Asset page callbacks ──────────────────────────────────────────────────────

static void asset_cb_handler(lv_event_t* e) {
    lv_obj_t* cb  = lv_event_get_target(e);
    bool checked  = lv_obj_has_state(cb, LV_STATE_CHECKED);

    // Count currently checked boxes (including this change)
    int count = 0;
    for (int i = 0; i < TOTAL_ASSETS; i++) {
        if (asset_cbs[i] && lv_obj_has_state(asset_cbs[i], LV_STATE_CHECKED))
            count++;
    }

    // Enforce maximum of MAX_ASSETS (6) — uncheck this one if over limit
    if (checked && count > MAX_ASSETS) {
        lv_obj_clear_state(cb, LV_STATE_CHECKED);
    }

    sync_assets_from_cbs();
    refresh_asset_count_lbl();
    update_main_assets_label();
}

// ── Timeframe page callbacks ──────────────────────────────────────────────────

static void tf_cb_handler(lv_event_t* e) {
    lv_obj_t* cb = lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);

    if (!checked) {
        // Count remaining checked boxes
        int count = 0;
        for (int i = 0; i < 4; i++) {
            if (tf_cbs[i] && lv_obj_has_state(tf_cbs[i], LV_STATE_CHECKED))
                count++;
        }
        // Enforce minimum of 1 — prevent unchecking the last box
        if (count == 0) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
            return;
        }
    }

    sync_tf_from_cbs();
    update_main_tf_label();
}

// ── Cycle page callbacks ──────────────────────────────────────────────────────

static void cycle_sw_cb(lv_event_t*) {
    bool on = lv_obj_has_state(cycle_sw, LV_STATE_CHECKED);
    if (on) {
        lv_obj_clear_state(cycle_slider, LV_STATE_DISABLED);
        int val = lv_slider_get_value(cycle_slider);
        ss_work.cycle_secs = val;
        char buf[12];
        snprintf(buf, sizeof(buf), "%ds", val);
        lv_label_set_text(cycle_val_lbl, buf);
    } else {
        lv_obj_add_state(cycle_slider, LV_STATE_DISABLED);
        ss_work.cycle_secs = 0;
        lv_label_set_text(cycle_val_lbl, "Manual");
    }
    update_main_cycle_label();
}

static void cycle_slider_cb(lv_event_t*) {
    int val = lv_slider_get_value(cycle_slider);
    // Snap to multiples of 5
    val = ((val + 2) / 5) * 5;
    if (val < 5)   val = 5;
    if (val > 120) val = 120;
    lv_slider_set_value(cycle_slider, val, LV_ANIM_OFF);
    ss_work.cycle_secs = val;
    char buf[12];
    snprintf(buf, sizeof(buf), "%ds", val);
    lv_label_set_text(cycle_val_lbl, buf);
    update_main_cycle_label();
}

// ── Brightness page callback ──────────────────────────────────────────────────

static void bri_slider_cb(lv_event_t*) {
    int val = lv_slider_get_value(bri_slider);
    if (val < 10)  val = 10;
    if (val > 100) val = 100;
    ss_work.brightness = val;
    bsp_display_brightness_set(val);  // live preview
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(bri_val_lbl, buf);
    update_main_bri_label();
}

// ── Navigation callbacks ──────────────────────────────────────────────────────

static void back_btn_cb(lv_event_t*) {
    show_page(page_main, "Settings");
}

static void go_assets_cb(lv_event_t*)  { show_page(page_assets, "Assets");          }
static void go_tf_cb(lv_event_t*)      { show_page(page_tf,     "Timeframes");       }
static void go_tz_cb(lv_event_t*) {
    // Commit roller selection before navigating away (roller shows live selection)
    show_page(page_tz,  "Timezone");
}
static void go_theme_cb(lv_event_t*)   { show_page(page_theme,  "Candle Colour");    }
static void go_anim_cb(lv_event_t*)    { show_page(page_anim,   "After-Hours Animation"); }
static void go_cycle_cb(lv_event_t*)   { show_page(page_cycle,  "Asset Cycling");    }
static void go_bri_cb(lv_event_t*)     { show_page(page_bri,    "Brightness");       }
static void go_diag_cb(lv_event_t*) {
    // Refresh diagnostics labels on every open
    if (diag_heap_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Heap free:  %lu B", (unsigned long)ESP.getFreeHeap());
        lv_label_set_text(diag_heap_lbl, buf);
        snprintf(buf, sizeof(buf), "PSRAM free: %lu B",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        lv_label_set_text(diag_psram_lbl, buf);
        if (WiFi.status() == WL_CONNECTED) {
            snprintf(buf, sizeof(buf), "WiFi: %s  RSSI %d",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            snprintf(buf, sizeof(buf), "WiFi: disconnected");
        }
        lv_label_set_text(diag_wifi_lbl, buf);
        unsigned long up = millis() / 1000;
        snprintf(buf, sizeof(buf), "Uptime: %luh %02lum %02lus",
                 up / 3600, (up % 3600) / 60, up % 60);
        lv_label_set_text(diag_uptime_lbl, buf);
    }
    show_page(page_diag, "About / Diagnostics");
}

// ── Save callback ─────────────────────────────────────────────────────────────

static void save_cb(lv_event_t*) {
    // Commit roller selections into ss_work
    if (tz_roller) {
        int idx = (int)lv_roller_get_selected(tz_roller);
        if (idx >= 0 && idx < TZ_OPTS_N)
            ss_work.tz_offset = TZ_OPTS[idx].minutes;
    }
    // ss_work.theme is written immediately on tap in the theme page callback
    if (anim_roller) {
        ss_work.after_anim = (int)lv_roller_get_selected(anim_roller);
    }

    // Guard: at least one asset must be selected
    if (ss_work.asset_count < 1) return;

    // Show a reboot notice on the screen before triggering the restart
    lv_obj_t* overlay = lv_obj_create(ss_scr);
    lv_obj_set_size(overlay, SCR_W, SCR_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x0E1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay,   0,    LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = lv_label_create(overlay);
    lv_obj_set_style_text_font(msg,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE6E9EF), LV_PART_MAIN);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER,   LV_PART_MAIN);
    lv_label_set_text(msg, "Saving settings...\nDevice will restart.");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    ss_result = 1;  // main loop will call settings_screen_get() then restart
}

// ── Cancel callback ───────────────────────────────────────────────────────────

static void cancel_cb(lv_event_t*) {
    ss_result = -1;
}

// ── WiFi redo callback ────────────────────────────────────────────────────────

static void wifi_redo_cb(lv_event_t*) {
    // Same action as holding the BOOT button 3 seconds
    settings_clear();
    delay(200);
    ESP.restart();
}

// ── Page builders ─────────────────────────────────────────────────────────────

// Main menu (list of navigation rows + action buttons).
static void build_main_page() {
    page_main = make_page(false);  // visible initially

    lv_obj_t* r;

    r = make_nav_row(page_main, "Assets", &main_val_assets);
    lv_obj_add_event_cb(r, go_assets_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "Timeframes", &main_val_tf);
    lv_obj_add_event_cb(r, go_tf_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "Timezone", &main_val_tz);
    lv_obj_add_event_cb(r, go_tz_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "Candle Colour", &main_val_theme);
    lv_obj_add_event_cb(r, go_theme_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "After-Hours Animation", &main_val_anim);
    lv_obj_add_event_cb(r, go_anim_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "Asset Cycling", &main_val_cycle);
    lv_obj_add_event_cb(r, go_cycle_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "Brightness", &main_val_bri);
    lv_obj_add_event_cb(r, go_bri_cb, LV_EVENT_CLICKED, NULL);

    r = make_nav_row(page_main, "About / Diagnostics", nullptr);
    lv_obj_add_event_cb(r, go_diag_cb, LV_EVENT_CLICKED, NULL);

    make_spacer(page_main, 12);

    lv_obj_t* btn;
    btn = make_action_btn(page_main, LV_SYMBOL_WARNING "  Re-do WiFi Setup",
                          lv_color_hex(0xD97706));  // amber-600, darker than SS_AMBER
    lv_obj_add_event_cb(btn, wifi_redo_cb, LV_EVENT_CLICKED, NULL);

    make_spacer(page_main, 4);

    btn = make_action_btn(page_main, "Save & Restart",
                          lv_color_hex(0x16A34A));  // green-700, darker than SS_GREEN
    lv_obj_add_event_cb(btn, save_cb, LV_EVENT_CLICKED, NULL);

    make_spacer(page_main, 4);

    btn = make_action_btn(page_main, "Cancel", SS_DIVIDER);
    lv_obj_add_event_cb(btn, cancel_cb, LV_EVENT_CLICKED, NULL);

    make_spacer(page_main, 8);

    // Initialise summary labels from ss_work
    update_main_assets_label();
    update_main_tf_label();
    update_main_tz_label();
    update_main_theme_label();
    update_main_anim_label();
    update_main_cycle_label();
    update_main_bri_label();
}

// Assets page: 24 checkboxes grouped by market type.
static void build_assets_page() {
    page_assets = make_page(true);

    // Count label at the top
    lv_obj_t* count_row = lv_obj_create(page_assets);
    lv_obj_set_size(count_row, LV_PCT(100), 32);
    style_container(count_row);
    lv_obj_set_style_bg_color(count_row, SS_HDR, LV_PART_MAIN);
    lv_obj_set_style_pad_left(count_row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(count_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    asset_count_lbl = lv_label_create(count_row);
    lv_obj_set_style_text_font(asset_count_lbl,  &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(asset_count_lbl, SS_SUBTEXT,             LV_PART_MAIN);
    lv_obj_align(asset_count_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // Build grouped checkboxes. Iterate ASSETS[] and emit section headers on
    // first asset of each market group.
    static const MarketType group_order[] = {
        MARKET_CRYPTO, MARKET_STOCK, MARKET_COMMODITY, MARKET_FOREX
    };
    static const char* group_names[] = {
        "CRYPTO", "STOCKS & ETFs", "COMMODITIES", "FOREX"
    };

    for (int g = 0; g < 4; g++) {
        MarketType mt = group_order[g];
        bool first_in_group = true;

        for (int i = 0; i < TOTAL_ASSETS; i++) {
            if (ASSETS[i].market != mt) continue;

            if (first_in_group) {
                make_section_label(page_assets, group_names[g]);
                first_in_group = false;
            }

            lv_obj_t* row = lv_obj_create(page_assets);
            lv_obj_set_size(row, LV_PCT(100), ROW_H);
            style_row(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* cb = lv_checkbox_create(row);
            // Checkbox label = "SYM  Full Name"
            char lbl_buf[32];
            snprintf(lbl_buf, sizeof(lbl_buf), "%-6s %s",
                     ASSETS[i].symbol, ASSETS[i].name);
            lv_checkbox_set_text(cb, lbl_buf);
            lv_obj_set_style_text_font(cb,  &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(cb, SS_TEXT,                LV_PART_MAIN);
            // Checkbox indicator style
            lv_obj_set_style_bg_color(cb,     SS_BG,      LV_PART_INDICATOR);
            lv_obj_set_style_border_color(cb, SS_SUBTEXT, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(cb,     SS_GREEN,   LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_border_color(cb, SS_GREEN,   LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_add_flag(cb, LV_OBJ_FLAG_CLICKABLE);

            // Pre-check if this asset is in the current selection
            for (int j = 0; j < ss_work.asset_count; j++) {
                if (strcmp(ss_work.assets[j], ASSETS[i].symbol) == 0) {
                    lv_obj_add_state(cb, LV_STATE_CHECKED);
                    break;
                }
            }

            lv_obj_add_event_cb(cb, asset_cb_handler, LV_EVENT_VALUE_CHANGED, NULL);
            asset_cbs[i] = cb;
        }
    }

    refresh_asset_count_lbl();
    make_spacer(page_assets, 8);
}

// Timeframes page: 4 checkboxes, min 1 must remain checked.
static void build_tf_page() {
    page_tf = make_page(true);

    make_section_label(page_tf, "At least one must be selected");

    static const int   TF_VALS[4]  = {15,    60,   240,   1440};
    static const char* TF_NAMES[4] = {"15m", "1h", "4h",  "1D"};

    for (int i = 0; i < 4; i++) {
        lv_obj_t* row = lv_obj_create(page_tf);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        style_row(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* cb = lv_checkbox_create(row);
        lv_checkbox_set_text(cb, TF_NAMES[i]);
        lv_obj_set_style_text_font(cb,  &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(cb, SS_TEXT,                LV_PART_MAIN);
        lv_obj_set_style_bg_color(cb,   SS_BG,    LV_PART_INDICATOR);
        lv_obj_set_style_border_color(cb, SS_SUBTEXT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(cb,   SS_GREEN,  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(cb, SS_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(cb, LV_OBJ_FLAG_CLICKABLE);

        // Pre-check if this TF is in the enabled set
        for (int j = 0; j < ss_work.timeframe_count; j++) {
            if (ss_work.timeframes[j] == TF_VALS[i]) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
                break;
            }
        }

        lv_obj_add_event_cb(cb, tf_cb_handler, LV_EVENT_VALUE_CHANGED, NULL);
        tf_cbs[i] = cb;
    }

    make_spacer(page_tf, 8);
}

// Timezone page: lv_roller with 34 options.
static void build_tz_page() {
    page_tz = make_page(true);

    // Build the roller options string (newline-separated)
    // Each label is up to ~40 chars; 34 entries * 41 = ~1400 chars
    static char tz_opts_str[1600];
    tz_opts_str[0] = '\0';
    for (int i = 0; i < TZ_OPTS_N; i++) {
        if (i) strncat(tz_opts_str, "\n", sizeof(tz_opts_str) - strlen(tz_opts_str) - 1);
        strncat(tz_opts_str, TZ_OPTS[i].label, sizeof(tz_opts_str) - strlen(tz_opts_str) - 1);
    }

    tz_roller = lv_roller_create(page_tz);
    lv_roller_set_options(tz_roller, tz_opts_str, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(tz_roller, 5);
    lv_obj_set_size(tz_roller, LV_PCT(100), CONT_H - 20);
    lv_obj_align(tz_roller, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(tz_roller,     SS_BG,     LV_PART_MAIN);
    lv_obj_set_style_text_color(tz_roller,   SS_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(tz_roller,    &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tz_roller,     SS_HDR,    LV_PART_SELECTED);
    lv_obj_set_style_text_color(tz_roller,   SS_TEXT,   LV_PART_SELECTED);
    lv_obj_set_style_border_color(tz_roller, SS_DIVIDER, LV_PART_MAIN);

    // Pre-select current timezone
    for (int i = 0; i < TZ_OPTS_N; i++) {
        if (TZ_OPTS[i].minutes == ss_work.tz_offset) {
            lv_roller_set_selected(tz_roller, (uint16_t)i, LV_ANIM_OFF);
            break;
        }
    }

    // Commit selection to ss_work whenever it changes
    lv_obj_add_event_cb(tz_roller, [](lv_event_t*) {
        int idx = (int)lv_roller_get_selected(tz_roller);
        if (idx >= 0 && idx < TZ_OPTS_N) {
            ss_work.tz_offset = TZ_OPTS[idx].minutes;
            update_main_tz_label();
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);
}

// Theme page: 4 selectable rows, each showing the theme name and up/down candle color swatches.
static void build_theme_page() {
    page_theme = make_page(true);

    static const char* names[4] = { "Classic", "Color Shift", "Neon Pulse", "Custom" };
    lv_color_t bull_cols[4] = {
        lv_color_hex(0x26A69A),
        lv_color_hsv_to_rgb(85,  80, 90),   // representative ColorShift snapshot
        lv_color_hsv_to_rgb(150, 90, 90),   // NeonPulse peak green
        lv_color_hex(ss_work.bull_rgb)
    };
    lv_color_t bear_cols[4] = {
        lv_color_hex(0xEF5350),
        lv_color_hsv_to_rgb(213, 85, 85),   // representative ColorShift snapshot
        lv_color_hsv_to_rgb(300, 90, 90),   // NeonPulse peak magenta
        lv_color_hex(ss_work.bear_rgb)
    };

    int t = ss_work.theme;
    if (t < 0 || t > 3) t = 0;

    make_spacer(page_theme, 8);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* row = lv_obj_create(page_theme);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        style_row(row);
        theme_rows[i] = row;

        // Theme name (left)
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, SS_TEXT, LV_PART_MAIN);
        lv_label_set_text(lbl, names[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        // Selection checkmark (rightmost, hidden unless this row is active)
        lv_obj_t* chk = lv_label_create(row);
        lv_obj_set_style_text_color(chk, SS_TEXT, LV_PART_MAIN);
        lv_label_set_text(chk, LV_SYMBOL_OK);
        lv_obj_align(chk, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_clear_flag(chk, LV_OBJ_FLAG_CLICKABLE);
        if (i != t) lv_obj_add_flag(chk, LV_OBJ_FLAG_HIDDEN);
        theme_checks[i] = chk;

        // Down (bear) candle color swatch — left of checkmark
        lv_obj_t* bear_sw = lv_obj_create(row);
        lv_obj_set_size(bear_sw, 22, 22);
        lv_obj_set_style_bg_color(bear_sw, bear_cols[i], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bear_sw, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bear_sw, 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(bear_sw, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bear_sw, 0, LV_PART_MAIN);
        lv_obj_align(bear_sw, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_clear_flag(bear_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bear_sw, LV_OBJ_FLAG_SCROLLABLE);

        // Up (bull) candle color swatch — left of bear swatch
        lv_obj_t* bull_sw = lv_obj_create(row);
        lv_obj_set_size(bull_sw, 22, 22);
        lv_obj_set_style_bg_color(bull_sw, bull_cols[i], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bull_sw, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bull_sw, 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(bull_sw, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bull_sw, 0, LV_PART_MAIN);
        lv_obj_align(bull_sw, LV_ALIGN_RIGHT_MID, -50, 0);
        lv_obj_clear_flag(bull_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bull_sw, LV_OBJ_FLAG_SCROLLABLE);

        // Tap selects this theme
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            ss_work.theme = idx;
            for (int j = 0; j < 4; j++) {
                if (!theme_checks[j]) continue;
                if (j == idx) lv_obj_clear_flag(theme_checks[j], LV_OBJ_FLAG_HIDDEN);
                else          lv_obj_add_flag(theme_checks[j],   LV_OBJ_FLAG_HIDDEN);
            }
            update_main_theme_label();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    make_spacer(page_theme, 8);
}

// Animation page: lv_roller with 4 options.
static void build_anim_page() {
    page_anim = make_page(true);

    anim_roller = lv_roller_create(page_anim);
    lv_roller_set_options(anim_roller,
        "Tidepool\nCoral Reef\nStarfield\nCountdown\nPixel Beach\nMarket Pit",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(anim_roller, 6);
    lv_obj_set_size(anim_roller, LV_PCT(100), CONT_H - 20);
    lv_obj_align(anim_roller, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(anim_roller,     SS_BG,     LV_PART_MAIN);
    lv_obj_set_style_text_color(anim_roller,   SS_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(anim_roller,    &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(anim_roller,     SS_HDR,    LV_PART_SELECTED);
    lv_obj_set_style_text_color(anim_roller,   SS_TEXT,   LV_PART_SELECTED);
    lv_obj_set_style_border_color(anim_roller, SS_DIVIDER, LV_PART_MAIN);

    int a = ss_work.after_anim;
    if (a < 0 || a > 5) a = 0;
    lv_roller_set_selected(anim_roller, (uint16_t)a, LV_ANIM_OFF);

    lv_obj_add_event_cb(anim_roller, [](lv_event_t*) {
        ss_work.after_anim = (int)lv_roller_get_selected(anim_roller);
        update_main_anim_label();
    }, LV_EVENT_VALUE_CHANGED, NULL);
}

// Cycling page: switch (auto/manual) + slider (5-120 s).
static void build_cycle_page() {
    page_cycle = make_page(true);

    make_spacer(page_cycle, 16);

    // Switch row
    lv_obj_t* sw_row = lv_obj_create(page_cycle);
    lv_obj_set_size(sw_row, LV_PCT(100), ROW_H);
    style_row(sw_row);
    lv_obj_clear_flag(sw_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* sw_lbl = lv_label_create(sw_row);
    lv_obj_set_style_text_font(sw_lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sw_lbl, SS_TEXT,                LV_PART_MAIN);
    lv_label_set_text(sw_lbl, "Auto-cycle assets");
    lv_obj_align(sw_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    cycle_sw = lv_switch_create(sw_row);
    lv_obj_set_style_bg_color(cycle_sw, SS_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(cycle_sw, LV_ALIGN_RIGHT_MID, 0, 0);

    make_spacer(page_cycle, 12);

    // Interval label row
    lv_obj_t* int_row = lv_obj_create(page_cycle);
    lv_obj_set_size(int_row, LV_PCT(100), 28);
    style_container(int_row);
    lv_obj_set_style_pad_left(int_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(int_row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(int_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* int_lbl = lv_label_create(int_row);
    lv_obj_set_style_text_font(int_lbl,  &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(int_lbl, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(int_lbl, "Interval:");
    lv_obj_align(int_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    cycle_val_lbl = lv_label_create(int_row);
    lv_obj_set_style_text_font(cycle_val_lbl,  &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cycle_val_lbl, SS_TEXT,                LV_PART_MAIN);
    lv_obj_align(cycle_val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    make_spacer(page_cycle, 6);

    // Slider row
    lv_obj_t* sl_row = lv_obj_create(page_cycle);
    lv_obj_set_size(sl_row, LV_PCT(100), 44);
    style_container(sl_row);
    lv_obj_set_style_pad_left(sl_row,  16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(sl_row, 16, LV_PART_MAIN);
    lv_obj_clear_flag(sl_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    cycle_slider = lv_slider_create(sl_row);
    lv_obj_set_width(cycle_slider, LV_PCT(100));
    lv_slider_set_range(cycle_slider, 5, 120);
    lv_obj_set_style_bg_color(cycle_slider, SS_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cycle_slider, SS_BLUE,    LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cycle_slider, SS_BLUE,    LV_PART_KNOB);
    lv_obj_align(cycle_slider, LV_ALIGN_CENTER, 0, 0);

    // Range labels row (5s … 120s)
    lv_obj_t* rng_row = lv_obj_create(page_cycle);
    lv_obj_set_size(rng_row, LV_PCT(100), 20);
    style_container(rng_row);
    lv_obj_set_style_pad_left(rng_row,  16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(rng_row, 16, LV_PART_MAIN);
    lv_obj_clear_flag(rng_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* min_lbl = lv_label_create(rng_row);
    lv_obj_set_style_text_font(min_lbl,  &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(min_lbl, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(min_lbl, "5s");
    lv_obj_align(min_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* max_lbl = lv_label_create(rng_row);
    lv_obj_set_style_text_font(max_lbl,  &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(max_lbl, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(max_lbl, "120s");
    lv_obj_align(max_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(cycle_sw,     cycle_sw_cb,     LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(cycle_slider, cycle_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Initialise from ss_work
    if (ss_work.cycle_secs > 0) {
        lv_obj_add_state(cycle_sw, LV_STATE_CHECKED);
        lv_slider_set_value(cycle_slider, ss_work.cycle_secs, LV_ANIM_OFF);
        char buf[12];
        snprintf(buf, sizeof(buf), "%ds", ss_work.cycle_secs);
        lv_label_set_text(cycle_val_lbl, buf);
    } else {
        lv_obj_clear_state(cycle_sw, LV_STATE_CHECKED);
        lv_obj_add_state(cycle_slider, LV_STATE_DISABLED);
        lv_slider_set_value(cycle_slider, 30, LV_ANIM_OFF);
        lv_label_set_text(cycle_val_lbl, "Manual");
    }

    make_spacer(page_cycle, 8);
}

// Brightness page: slider 10-100 % with live preview.
static void build_bri_page() {
    page_bri = make_page(true);
    // Content easily fits — disable scroll so the parent doesn't intercept
    // horizontal drag events meant for the slider.
    lv_obj_set_scroll_dir(page_bri, LV_DIR_NONE);

    make_spacer(page_bri, 24);

    // Value label
    lv_obj_t* val_row = lv_obj_create(page_bri);
    lv_obj_set_size(val_row, LV_PCT(100), 40);
    style_container(val_row);
    lv_obj_clear_flag(val_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    bri_val_lbl = lv_label_create(val_row);
    lv_obj_set_style_text_font(bri_val_lbl,  &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(bri_val_lbl, SS_TEXT,                LV_PART_MAIN);
    lv_obj_align(bri_val_lbl, LV_ALIGN_CENTER, 0, 0);

    // Slider row — slightly taller than the track to give ext_click_area room
    lv_obj_t* sl_row = lv_obj_create(page_bri);
    lv_obj_set_size(sl_row, LV_PCT(100), 48);
    style_container(sl_row);
    lv_obj_set_style_pad_left(sl_row,  24, LV_PART_MAIN);
    lv_obj_set_style_pad_right(sl_row, 24, LV_PART_MAIN);
    lv_obj_clear_flag(sl_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    bri_slider = lv_slider_create(sl_row);
    lv_obj_set_width(bri_slider, LV_PCT(100));
    lv_slider_set_range(bri_slider, 10, 100);
    lv_slider_set_value(bri_slider, ss_work.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bri_slider, SS_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bri_slider, SS_AMBER,   LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bri_slider, SS_AMBER,   LV_PART_KNOB);
    // Extend the hit-test zone 16 px in every direction beyond the visual track,
    // so first-touch anywhere in the row reliably lands on the slider.
    lv_obj_set_ext_click_area(bri_slider, 16);
    lv_obj_align(bri_slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(bri_slider, bri_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Range labels
    lv_obj_t* rng_row = lv_obj_create(page_bri);
    lv_obj_set_size(rng_row, LV_PCT(100), 20);
    style_container(rng_row);
    lv_obj_set_style_pad_left(rng_row,  24, LV_PART_MAIN);
    lv_obj_set_style_pad_right(rng_row, 24, LV_PART_MAIN);
    lv_obj_clear_flag(rng_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lo = lv_label_create(rng_row);
    lv_obj_set_style_text_font(lo,  &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lo, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(lo, "10%");
    lv_obj_align(lo, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* hi = lv_label_create(rng_row);
    lv_obj_set_style_text_font(hi,  &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(hi, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(hi, "100%");
    lv_obj_align(hi, LV_ALIGN_RIGHT_MID, 0, 0);

    // Set initial label
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", ss_work.brightness);
    lv_label_set_text(bri_val_lbl, buf);

    make_spacer(page_bri, 8);
}

// Diagnostics page: read-only labels refreshed on open.
static void build_diag_page() {
    page_diag = make_page(true);

    make_spacer(page_diag, 8);

    // Helper to make a read-only label row
    auto make_diag_row = [](lv_obj_t* parent, lv_obj_t** lbl_out) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        style_container(row);
        lv_obj_set_style_border_side(row,  LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, SS_DIVIDER,             LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1,                      LV_PART_MAIN);
        lv_obj_set_style_pad_left(row,  12, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, SS_TEXT,                LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        *lbl_out = lbl;
    };

    make_diag_row(page_diag, &diag_fw_lbl);
    lv_label_set_text(diag_fw_lbl, "Build: " __DATE__ " " __TIME__);

    make_diag_row(page_diag, &diag_heap_lbl);
    make_diag_row(page_diag, &diag_psram_lbl);
    make_diag_row(page_diag, &diag_wifi_lbl);
    make_diag_row(page_diag, &diag_uptime_lbl);

    make_spacer(page_diag, 8);
}

// ── Public API ────────────────────────────────────────────────────────────────

void settings_screen_create(const Settings* initial) {
    ss_work  = *initial;
    ss_result = 0;

    // Screen
    ss_scr = lv_obj_create(NULL);
    style_container(ss_scr);

    // Header bar
    ss_hdr = lv_obj_create(ss_scr);
    lv_obj_set_size(ss_hdr, SCR_W, HDR_H);
    lv_obj_set_pos(ss_hdr, 0, 0);
    lv_obj_set_style_bg_color(ss_hdr,     SS_HDR,       LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ss_hdr,       LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(ss_hdr,  LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(ss_hdr, SS_DIVIDER,   LV_PART_MAIN);
    lv_obj_set_style_border_width(ss_hdr, 1,            LV_PART_MAIN);
    lv_obj_set_style_radius(ss_hdr,       0,            LV_PART_MAIN);
    lv_obj_set_style_pad_all(ss_hdr,      0,            LV_PART_MAIN);
    lv_obj_clear_flag(ss_hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button (hidden on the main page)
    btn_back = lv_btn_create(ss_hdr);
    lv_obj_set_size(btn_back, 40, HDR_H);
    lv_obj_set_style_bg_opa(btn_back,     LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_back, 0,           LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_back, 0,           LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back,     0,             LV_PART_MAIN);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);  // hidden until sub-page

    lv_obj_t* back_lbl = lv_label_create(btn_back);
    lv_obj_set_style_text_font(back_lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_lbl, SS_BLUE,                LV_PART_MAIN);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    // Title
    hdr_title = lv_label_create(ss_hdr);
    lv_obj_set_style_text_font(hdr_title,  &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr_title, SS_TEXT,                LV_PART_MAIN);
    lv_label_set_text(hdr_title, "Settings");
    lv_obj_align(hdr_title, LV_ALIGN_CENTER, 0, 0);

    // Gear icon on the right of header
    lv_obj_t* gear = lv_label_create(ss_hdr);
    lv_obj_set_style_text_font(gear,  &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(gear, SS_SUBTEXT,             LV_PART_MAIN);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -10, 0);

    // Build all pages (only page_main starts visible)
    build_main_page();
    build_assets_page();
    build_tf_page();
    build_tz_page();
    build_theme_page();
    build_anim_page();
    build_cycle_page();
    build_bri_page();
    build_diag_page();

    cur_page = page_main;

    lv_scr_load(ss_scr);
}

int settings_screen_poll() {
    int r = (int)ss_result;
    // Don't clear ss_result — the main loop reads it once then stops polling.
    return r;
}

void settings_screen_get(Settings* out) {
    *out = ss_work;
}

lv_obj_t* settings_screen_detach() {
    lv_obj_t* scr = ss_scr;
    // Null all handles so no dangling pointer writes happen if any LVGL
    // timer fires after detach (none should, but be safe).
    ss_scr       = nullptr;
    ss_hdr       = nullptr;
    hdr_title    = nullptr;
    btn_back     = nullptr;
    cur_page     = nullptr;
    page_main    = nullptr;
    page_assets  = nullptr;
    page_tf      = nullptr;
    page_tz      = nullptr;
    page_theme   = nullptr;
    page_anim    = nullptr;
    page_cycle   = nullptr;
    page_bri     = nullptr;
    page_diag    = nullptr;
    asset_count_lbl = nullptr;
    for (int i = 0; i < TOTAL_ASSETS; i++) asset_cbs[i] = nullptr;
    for (int i = 0; i < 4; i++) tf_cbs[i] = nullptr;
    tz_roller     = nullptr;
    for (int i = 0; i < 4; i++) { theme_rows[i] = nullptr; theme_checks[i] = nullptr; }
    anim_roller   = nullptr;
    cycle_sw      = nullptr;
    cycle_slider  = nullptr;
    cycle_val_lbl = nullptr;
    bri_slider    = nullptr;
    bri_val_lbl   = nullptr;
    main_val_assets = nullptr;
    main_val_tf     = nullptr;
    main_val_tz     = nullptr;
    main_val_theme  = nullptr;
    main_val_anim   = nullptr;
    main_val_cycle  = nullptr;
    main_val_bri    = nullptr;
    diag_heap_lbl   = nullptr;
    diag_psram_lbl  = nullptr;
    diag_wifi_lbl   = nullptr;
    diag_uptime_lbl = nullptr;
    diag_fw_lbl     = nullptr;
    ss_result       = 0;
    return scr;
}
