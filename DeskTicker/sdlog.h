#pragma once
#include <stdint.h>

// ── SD-card serial logger ─────────────────────────────────────────────────────
// Mirrors everything that goes to Serial onto the on-board micro-SD card so the
// device can be tested unattended (no PC required to capture the serial monitor).
//
// Design notes:
//  - The SD card is wired for SPI mode on its own bus (SPI3/FSPI), pins CS=10,
//    SCK=12, MOSI=11, MISO=13 — fully separate from the QSPI display (SPI2). So SD
//    writes never touch the display bus.
//  - Log lines are queued into a PSRAM RAM FIFO immediately (cheap, any context) and
//    only written to the card from sdlog_flush(), which the main loop calls when it is
//    safe. Crucially, sdlog_flush() does NOT write while lvgl_flush_suspended is true
//    (the api_fetch window we protect from WiFi/QSPI DMA contention).
//  - Every line is timestamped (wall clock once NTP is synced, else boot millis).
//  - The log rotates at 10 MB: deskticker.log -> deskticker.old (previous .old deleted),
//    a fresh deskticker.log is started. Up to ~20 MB total on the card.

// Mount the SD card and open the log file. Call once, early in setup() (after
// Serial.begin). Safe to call even if no card is present — logging silently falls
// back to Serial-only. Prints the mount result to Serial.
void sdlog_init();

// printf-style log: formats once, writes to Serial immediately (unchanged on-screen
// behaviour) AND queues the same timestamped bytes for the SD card. Use everywhere we
// previously called Serial.printf.
void sdlog_printf(const char* fmt, ...);

// println equivalent (mirrors Serial.println): writes s + newline to Serial and SD.
// Drop-in replacement for Serial.println(const char*).
void sdlog_println(const char* s);

// Flush any queued lines to the SD card IF it is currently safe (card mounted and
// lvgl_flush_suspended == false). Call once per main loop() iteration. Handles the
// 10 MB rotation. No-op when nothing is pending or a fetch is in progress.
void sdlog_flush();

// Force a synchronous write of all pending lines right now (used just before
// esp_restart() so the final [WDT] reboot-cause line is not lost). Still avoids running
// concurrently with an in-flight display flush.
void sdlog_flush_blocking();

// Hand the SD card off to another owner (the USB Mass-Storage "Share SD over USB"
// feature). Flushes any pending lines, closes the filesystem (SD.end()), frees the
// SPI bus, and SUSPENDS the logger so sdlog_flush() will not re-mount the card while
// it is shared. Logging falls back to Serial-only after this. The device reboots when
// the user leaves share mode, so the logger re-mounts the card cleanly on the next boot.
void sdlog_release();

// Human-readable name for esp_reset_reason() (POWERON / SW / PANIC / BROWNOUT / ...).
// Logged at every boot so an unattended reboot's cause is captured on the card.
const char* reset_reason_str();

// Thin wrappers so existing Serial.print*-style call sites convert cleanly.
#define LOGF(...)  sdlog_printf(__VA_ARGS__)
#define LOGLN(s)   sdlog_printf("%s\n", (s))
