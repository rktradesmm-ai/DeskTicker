#pragma once
#include <stdint.h>
#include "assets.h"

#define TF_15M   15
#define TF_1H    60
#define TF_4H    240
#define TF_1D    1440

#define THEME_CLASSIC     0
#define THEME_COLORSHIFT  1
#define THEME_NEONPULSE   2
#define THEME_CUSTOM      3

#define ANIM_AQUARIUM   0
#define ANIM_BEACH      1
#define ANIM_STARFIELD  2
#define ANIM_COUNTDOWN  3
#define ANIM_PIXELBEACH 4
#define ANIM_GRASSLAND  5

typedef struct {
    char     wifi_ssid[64];
    char     wifi_pass[64];
    bool     wifi_ok;
    char     assets[MAX_ASSETS][ASSET_SYM_LEN];
    int      asset_count;
    int      timeframe;            // TF_* — currently active timeframe
    int      timeframes[4];        // enabled TF_* values (user-selected)
    int      timeframe_count;      // how many entries in timeframes[]
    int      theme;                // THEME_*
    uint32_t bull_rgb;             // 0xRRGGBB
    uint32_t bear_rgb;             // 0xRRGGBB
    int      cycle_secs;           // 5-120, 0 = manual swipe only
    int      after_anim;           // ANIM_*
    int      tz_offset;            // UTC offset in minutes (e.g. -300 = UTC-5, +330 = UTC+5:30)
    int      brightness;           // backlight level 10-100 %
} Settings;

void settings_defaults(Settings* s);
void settings_load(Settings* s);
void settings_save(const Settings* s);
void settings_clear();
