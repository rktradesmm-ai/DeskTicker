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
