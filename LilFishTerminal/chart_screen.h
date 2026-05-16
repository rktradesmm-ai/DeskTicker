#pragma once
#include <lvgl.h>
#include "assets.h"
#include "settings.h"

// Screen dimensions in landscape (after 90° rotation)
#define SCR_W   480
#define SCR_H   320

// Layout zones
#define HDR_H   48
#define FTR_H   48
#define CHART_H (SCR_H - HDR_H - FTR_H)   // 224
#define CHART_W SCR_W                       // 480

// Canvas drawing area inside chart
#define YAXIS_W       80   // wide enough for 9-digit prices at font_14 without wrapping
#define LEFT_MARGIN    8   // matches header content left offset
#define RIGHT_MARGIN   8
#define CANVAS_W  (CHART_W - YAXIS_W - LEFT_MARGIN - RIGHT_MARGIN)  // 394
#define CANVAS_H  (CHART_H - 28)                                     // ~196

#define XAXIS_H   (CHART_H - CANVAS_H)   // 28 — x-axis label strip below candles

// Gap in pixels between the header/footer and the chart box border.
// Both the top gap (header→box top) and the bottom gap (time labels→footer)
// use this same value, making the spacing exactly equal by construction.
#define CHART_BOX_MARGIN 6

#define CANDLE_W  7
#define CANDLE_G  2   // gap between candles
#define CANDLE_STEP (CANDLE_W + CANDLE_G)
#define CHART_RIGHT_PAD  4
#define MAX_VIS_CANDLES  ((CANVAS_W / CANDLE_STEP) - CHART_RIGHT_PAD)

// Create and show the chart screen.
void chart_screen_create(const Settings* s);

// Re-show the chart screen without rebuilding it (use after conn_scr displaces it).
void chart_screen_show();

// Show a small status message in the header (pass "" to clear).
// Call from within LVGL lock.
void chart_screen_set_status(const char* msg);

// Update chart content (call while holding LVGL lock).
void chart_screen_update(const AssetData* d, const Settings* s, bool wifi_ok);

// Returns +1 (swipe left = next asset), -1 (swipe right = prev), or 0.
// Resets the flag on read — call once per loop iteration.
int chart_screen_get_swipe();

// Returns +1 (swipe up = next timeframe), -1 (swipe down = prev), or 0.
// Resets the flag on read — call once per loop iteration.
int chart_screen_get_swipe_vert();

// Cleanup.
void chart_screen_destroy();
