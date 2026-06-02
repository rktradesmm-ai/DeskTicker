#include "sdlog.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <stdarg.h>

#include "lv_port.h"   // lvgl_flush_suspended — never write to SD during a fetch

// ── SD bus pins (from JC3248W535C schematic "SD_Car" block, SPI mode) ─────────
#define SD_PIN_CS    10   // TF_CS  (DAT3)
#define SD_PIN_MOSI  11   // MCU_MOSI (CMD)
#define SD_PIN_SCK   12   // TF_CLK (CLK)
#define SD_PIN_MISO  13   // MCU_MISO (DAT0)

#define SDLOG_PATH      "/deskticker.log"
#define SDLOG_PATH_OLD  "/deskticker.old"
#define SDLOG_MAX_BYTES (10UL * 1024 * 1024)   // rotate at 10 MB
#define SDLOG_REMOUNT_MS 3000                  // retry mount this often while card is out

// PSRAM-backed FIFO of pending text. 64 KB comfortably covers the gap between flushes
// even with verbose [health]/[YF] bursts; oldest bytes are dropped if it ever fills.
#define FIFO_CAP (64 * 1024)
static char*    s_fifo      = nullptr;   // ring buffer in PSRAM
static volatile uint32_t s_head = 0;     // write index
static volatile uint32_t s_tail = 0;     // read index (next byte to write to SD)
static volatile bool s_overflow = false; // set if we dropped bytes (logged once)

// SD card uses its own SPI bus so it can never collide with the QSPI display (SPI2).
static SPIClass s_sdspi(HSPI);
static bool     s_ready    = false;      // card mounted + file usable
static uint32_t s_file_sz  = 0;          // current size of deskticker.log
static bool     s_bus_started = false;   // s_sdspi.begin() done once
static unsigned long s_last_mount_try = 0; // throttle remount attempts while card is out

// ── helpers ───────────────────────────────────────────────────────────────────

// Number of bytes currently queued in the FIFO.
static inline uint32_t fifo_used() {
    return (s_head - s_tail + FIFO_CAP) % FIFO_CAP;
}

// Append one byte to the ring buffer (drops oldest on overflow).
static inline void fifo_push(char c) {
    uint32_t next = (s_head + 1) % FIFO_CAP;
    if (next == s_tail) {
        // Full — advance tail (drop oldest byte) and flag it.
        s_tail = (s_tail + 1) % FIFO_CAP;
        s_overflow = true;
    }
    s_fifo[s_head] = c;
    s_head = next;
}

static void fifo_push_str(const char* s) {
    if (!s_fifo) return;
    while (*s) fifo_push(*s++);
}

// Build "[YYYY-MM-DD HH:MM:SS] " when NTP is plausibly synced, else "[boot+NNNNNNNNms] ".
static void make_timestamp(char* buf, size_t n) {
    time_t now = time(nullptr);
    if (now > 1700000000) {   // ~2023-11; NTP has run
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(buf, n, "[%Y-%m-%d %H:%M:%S] ", &lt);
    } else {
        snprintf(buf, n, "[boot+%08lums] ", (unsigned long)millis());
    }
}

const char* reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";    // cold power-up
        case ESP_RST_SW:        return "SW";         // our esp_restart() (watchdog reboot)
        case ESP_RST_PANIC:     return "PANIC";      // crash / exception
        case ESP_RST_INT_WDT:   return "INT_WDT";    // interrupt watchdog
        case ESP_RST_TASK_WDT:  return "TASK_WDT";   // task watchdog
        case ESP_RST_WDT:       return "OTHER_WDT";  // other watchdog
        case ESP_RST_BROWNOUT:  return "BROWNOUT";   // supply voltage dip
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_EXT:       return "EXT";         // external reset pin
        default:                return "UNKNOWN";
    }
}

// ── public API ────────────────────────────────────────────────────────────────

// (Re)mount the card and open the log so rotation/size tracking is current. Returns true
// on success. Safe to call repeatedly — used both at init and to recover after the card is
// pulled and reinserted while the device keeps running. The SPI bus is brought up only
// once (it stays valid); only the SD/FS layer is torn down and re-init'd on a remount.
static bool sdlog_try_mount() {
    if (!s_bus_started) {
        // 20 MHz is conservative and reliable for logging; the card is on its own bus.
        s_sdspi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
        s_bus_started = true;
    } else {
        SD.end();   // drop stale driver state from a previously-removed card
    }

    if (!SD.begin(SD_PIN_CS, s_sdspi, 20000000)) {
        return false;
    }

    // Record the current size of the log so rotation works across mounts/reboots.
    File f = SD.open(SDLOG_PATH, FILE_APPEND);
    if (!f) {
        return false;
    }
    s_file_sz = f.size();
    f.close();
    return true;
}

void sdlog_init() {
    // Allocate the FIFO in PSRAM (falls back to internal RAM if PSRAM is absent).
    s_fifo = (char*)heap_caps_malloc(FIFO_CAP, MALLOC_CAP_SPIRAM);
    if (!s_fifo) s_fifo = (char*)malloc(FIFO_CAP);
    if (!s_fifo) {
        Serial.println("[sdlog] FIFO alloc failed — Serial-only logging");
        return;
    }
    s_head = s_tail = 0;

    if (sdlog_try_mount()) {
        s_ready = true;
        Serial.printf("[sdlog] SD ready, %s = %lu bytes\n", SDLOG_PATH, (unsigned long)s_file_sz);
    } else {
        Serial.println("[sdlog] SD mount failed — Serial-only logging (will retry when inserted)");
        s_ready = false;
    }
}

void sdlog_printf(const char* fmt, ...) {
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    // Always mirror to Serial immediately (unchanged on-screen behaviour).
    Serial.print(body);

    // Queue a timestamped copy for the SD card. Prefix once per call (each call is a
    // full line in this codebase).
    if (s_fifo) {
        char ts[40];
        make_timestamp(ts, sizeof(ts));
        fifo_push_str(ts);
        fifo_push_str(body);
    }
}

void sdlog_println(const char* s) {
    sdlog_printf("%s\n", s);
}

// Drain the FIFO to the open log file. Caller guarantees it is safe to do SD I/O.
static void drain_to_sd() {
    if (!s_ready || fifo_used() == 0) return;

    File f = SD.open(SDLOG_PATH, FILE_APPEND);
    if (!f) { s_ready = false; return; }

    // Note a prior overflow once, so gaps in the log are visible.
    if (s_overflow) {
        const char* warn = "[sdlog] (buffer overflow — some lines dropped)\n";
        f.print(warn);
        s_file_sz += strlen(warn);
        s_overflow = false;
    }

    // Write the queued bytes in up to two contiguous spans (ring may wrap). Advance the
    // tail only by the bytes actually written, so if the card is pulled mid-write the
    // unwritten lines stay buffered and are retried after the card is re-mounted.
    uint32_t tail = s_tail, head = s_head;
    bool short_write = false;
    if (tail != head) {
        if (tail < head) {
            uint32_t want = head - tail;
            size_t   wrote = f.write((const uint8_t*)(s_fifo + tail), want);
            s_file_sz += wrote;
            s_tail = (tail + wrote) % FIFO_CAP;
            short_write = (wrote != want);
        } else {
            uint32_t want1 = FIFO_CAP - tail;
            size_t   wrote1 = f.write((const uint8_t*)(s_fifo + tail), want1);
            s_file_sz += wrote1;
            s_tail = (tail + wrote1) % FIFO_CAP;
            if (wrote1 == want1) {
                size_t wrote2 = f.write((const uint8_t*)s_fifo, head);
                s_file_sz += wrote2;
                s_tail = wrote2;
                short_write = (wrote2 != head);
            } else {
                short_write = true;
            }
        }
    }
    f.close();

    // A short write means the card was likely yanked mid-flush — mark not-ready so
    // sdlog_flush() starts trying to re-mount; the unwritten bytes remain in the FIFO.
    if (short_write) {
        s_ready = false;
        return;
    }

    // Rotate once the active log passes the cap.
    if (s_file_sz >= SDLOG_MAX_BYTES) {
        SD.remove(SDLOG_PATH_OLD);          // discard the previous .old
        SD.rename(SDLOG_PATH, SDLOG_PATH_OLD);
        s_file_sz = 0;                      // fresh deskticker.log on next write
    }
}

void sdlog_flush() {
    // Never touch the SD bus while a fetch has the display suspended — that is the exact
    // window we are protecting from bus contention. The FIFO holds the lines until later.
    if (!s_fifo || lvgl_flush_suspended) return;

    // Card not mounted (never inserted, or pulled while running): retry the mount every
    // few seconds so logging auto-resumes when the card is (re)inserted — no reboot needed.
    if (!s_ready) {
        if (millis() - s_last_mount_try >= SDLOG_REMOUNT_MS) {
            s_last_mount_try = millis();
            if (sdlog_try_mount()) {
                s_ready = true;
                Serial.println("[sdlog] SD re-mounted, resuming logging");
                drain_to_sd();   // write everything buffered while the card was out
            }
        }
        return;
    }
    drain_to_sd();
}

void sdlog_flush_blocking() {
    // Used right before esp_restart(): respect the fetch suspend flag (a reboot during a
    // fetch is rare). If the card dropped, attempt one immediate re-mount so the final
    // reboot-cause line can still be persisted when the card is present.
    if (!s_fifo || lvgl_flush_suspended) return;
    if (!s_ready) {
        if (!sdlog_try_mount()) return;
        s_ready = true;
    }
    drain_to_sd();
}
