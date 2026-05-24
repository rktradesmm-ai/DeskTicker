#include <Preferences.h>
#include <string.h>
#include "settings.h"

static Preferences prefs;

void settings_defaults(Settings* s) {
    memset(s, 0, sizeof(Settings));
    strncpy(s->assets[0], "SPY",  ASSET_SYM_LEN - 1);
    strncpy(s->assets[1], "BTC",  ASSET_SYM_LEN - 1);
    strncpy(s->assets[2], "NVDA", ASSET_SYM_LEN - 1);
    s->asset_count      = 3;
    s->timeframe        = TF_1H;
    s->timeframes[0]    = TF_1H;
    s->timeframe_count  = 1;
    s->theme            = THEME_CLASSIC;
    s->bull_rgb         = 0x26A69A;   // Classic teal
    s->bear_rgb         = 0xEF5350;   // Classic red
    s->cycle_secs       = 30;
    s->after_anim       = ANIM_AQUARIUM;
    s->tz_offset        = 0;
    s->brightness       = 100;
    s->wifi_ok          = false;
}

void settings_load(Settings* s) {
    settings_defaults(s);
    prefs.begin("lilfish", true);
    if (!prefs.getBool("init", false)) {
        prefs.end();
        return;
    }
    prefs.getString("ssid",  s->wifi_ssid, sizeof(s->wifi_ssid));
    prefs.getString("pass",  s->wifi_pass, sizeof(s->wifi_pass));
    s->wifi_ok     = prefs.getBool("wifi_ok", false);
    s->asset_count = prefs.getInt("n_assets", 3);
    if (s->asset_count < 1)  s->asset_count = 1;
    if (s->asset_count > MAX_ASSETS) s->asset_count = MAX_ASSETS;
    for (int i = 0; i < s->asset_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        prefs.getString(key, s->assets[i], ASSET_SYM_LEN);
    }
    s->timeframe       = prefs.getInt("tf",     TF_1H);
    s->timeframe_count = prefs.getInt("tf_n",   0);
    if (s->timeframe_count < 1 || s->timeframe_count > 4) {
        // Migrate from old firmware: derive single-TF list from saved timeframe
        s->timeframes[0]   = s->timeframe;
        s->timeframe_count = 1;
    } else {
        for (int i = 0; i < s->timeframe_count; i++) {
            char key[8];
            snprintf(key, sizeof(key), "tf%d", i);
            s->timeframes[i] = prefs.getInt(key, TF_1H);
        }
        // Ensure active timeframe is in the enabled list
        bool found = false;
        for (int i = 0; i < s->timeframe_count; i++) {
            if (s->timeframes[i] == s->timeframe) { found = true; break; }
        }
        if (!found) s->timeframe = s->timeframes[0];
    }
    s->theme      = prefs.getInt("theme",    THEME_CLASSIC);
    s->bull_rgb   = prefs.getUInt("bull",    0x22C55E);   // Brand-kit Bull Green
    s->bear_rgb   = prefs.getUInt("bear",    0xEF4444);   // Brand-kit Bear Red
    s->cycle_secs = prefs.getInt("cycle",    30);
    s->after_anim  = prefs.getInt("anim",       ANIM_AQUARIUM);
    s->tz_offset   = prefs.getInt("tz",         0);
    s->brightness  = prefs.getInt("brightness", 100);
    if (s->brightness < 10)  s->brightness = 10;
    if (s->brightness > 100) s->brightness = 100;
    prefs.end();
}

void settings_save(const Settings* s) {
    prefs.begin("lilfish", false);
    prefs.putBool("init",    true);
    prefs.putString("ssid",  s->wifi_ssid);
    prefs.putString("pass",  s->wifi_pass);
    prefs.putBool("wifi_ok", s->wifi_ok);
    prefs.putInt("n_assets", s->asset_count);
    for (int i = 0; i < s->asset_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        prefs.putString(key, s->assets[i]);
    }
    prefs.putInt("tf",     s->timeframe);
    prefs.putInt("tf_n",   s->timeframe_count);
    for (int i = 0; i < s->timeframe_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "tf%d", i);
        prefs.putInt(key, s->timeframes[i]);
    }
    prefs.putInt("theme",  s->theme);
    prefs.putUInt("bull",    s->bull_rgb);
    prefs.putUInt("bear",    s->bear_rgb);
    prefs.putInt("cycle",  s->cycle_secs);
    prefs.putInt("anim",       s->after_anim);
    prefs.putInt("tz",         s->tz_offset);
    prefs.putInt("brightness", s->brightness);
    prefs.end();
}

void settings_clear() {
    prefs.begin("lilfish", false);
    prefs.clear();
    prefs.end();
}
