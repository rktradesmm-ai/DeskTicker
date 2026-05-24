#pragma once
#include <lvgl.h>
#include "settings.h"

// Start an after-hours animation scene.
// Call from within LVGL lock.
void anim_start(int anim_type, uint32_t mkt_open_epoch);

// Stop and clean up the current animation.
// Call from within LVGL lock.
void anim_stop();

// Update countdown time (call periodically from loop).
void anim_set_countdown(uint32_t seconds_remaining);

// Returns +1 (swipe left = next), -1 (swipe right = prev), or 0. Resets on read.
int anim_get_swipe();

// Returns 1 if the user held the after-hours screen for >3 s. Resets on read.
int anim_get_settings_req();

// Sets the crab claw colors to match the user's bull/bear candle theme.
// Call before anim_start() whenever the theme changes.
void anim_set_candle_colors(uint32_t bull_rgb, uint32_t bear_rgb);

// Draws the brand-spec 48×48 crab mascot onto a canvas at (cx, cy).
// Call from within LV_LOCK. bull_rgb / bear_rgb are 0xRRGGBB.
// claws_raised=true lifts the claw clusters for the celebratory pose.
void anim_draw_crab(lv_obj_t* canvas, int cx, int cy,
                    uint32_t bull_rgb, uint32_t bear_rgb,
                    bool blink, uint8_t walk_frame, bool claws_raised);
