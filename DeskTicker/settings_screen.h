#pragma once
#include <lvgl.h>
#include "settings.h"

// Build and load the on-device settings menu. Call while holding LV_LOCK.
// initial: current settings used to pre-populate all widgets.
void settings_screen_create(const Settings* initial);

// Call from the main loop (no lock needed).
// Returns: 0 = user still editing, 1 = user pressed Save, -1 = user pressed Cancel,
//          2 = user tapped "Share SD over USB" (enter USB drive mode).
int settings_screen_poll();

// Copy the user's edited settings out. Call after poll() returns 1.
void settings_screen_get(Settings* out);

// Detach the raw screen object from this module's tracking and return it.
// The caller is responsible for queuing it with queue_scr_for_delete() and
// calling cleanup_pending_scr() once a new screen is loaded.
// Internal widget handles are nulled — call under LV_LOCK.
lv_obj_t* settings_screen_detach();
