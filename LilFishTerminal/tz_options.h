#pragma once

// Shared timezone option table — used by wifi_manager.cpp (web form) and
// settings_screen.cpp (on-device settings menu).
typedef struct {
    const char* label;
    int         minutes;  // UTC offset in minutes (e.g. -300 = UTC-5)
} TzOption;

#define TZ_OPTS_N 34
extern const TzOption TZ_OPTS[TZ_OPTS_N];
