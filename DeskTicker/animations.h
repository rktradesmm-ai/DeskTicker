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

// ── Render-task liveness watchdog (always-on) ─────────────────────────────────
// Reboots the device if the LVGL render task stops producing frames for 30 s (the
// silent QSPI / tearing-effect display deadlock — see BISECT_LOG.md). Armed once in
// setup() and fed by a global lv_timer, so it protects EVERY screen: live chart,
// connecting, settings AND after-hours animation. Implemented in animations.cpp.

// Details of a watchdog reboot, stashed in RTC RAM so the next boot can report it.
typedef struct {
    uint32_t magic;        // set when the previous boot was a watchdog reboot
    uint8_t  last_state;   // main-loop State at the hang
    uint32_t free_heap;    // bytes free at reboot
    uint32_t free_psram;   // bytes free at reboot
    uint32_t reboot_epoch; // time(nullptr) at reboot (0 if NTP not synced)
    // Render-phase locator: pinpoints the exact step the render task was stuck in.
    // 0=idle/timer-cb  2=TE sync wait (→teTO)  3=chunk DMA-done wait (→flushTO)
    // 4=tx_color RAMWR/RAMWRC pixel DMA  5=flush done  6=render-loop mutex (→lockTO)
    // 7=tx_param CASET command send  (freezeTest2.txt: phase=4 chunk=3)
    uint8_t  phase;        // lvgl_render_phase at the moment of reboot
    uint8_t  chunk;        // lvgl_render_chunk at the moment of reboot
} WdtReboot;

// Arm the watchdog + start its feed timer. Call once in setup(), under LV_LOCK,
// after bsp_display_start() (the render task must already exist).
void render_wdt_init();

// Record the current main-loop State so a watchdog reboot can log where it hung.
void render_wdt_set_context(uint8_t state_code);

// Render-task heartbeat (increments ~1/s while the render task is alive).
uint32_t render_wdt_heartbeat();

// If the previous boot ended in a watchdog reboot, copy details to *out, clear the
// marker, and return true. Call once early in setup().
bool render_wdt_consume_last_reboot(WdtReboot* out);
