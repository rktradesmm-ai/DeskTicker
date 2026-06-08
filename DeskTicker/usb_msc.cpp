// USB Mass-Storage ("Share SD over USB") implementation.
// See usb_msc.h for the design rules. The whole feature is compiled only when the
// board is built in USB-OTG (TinyUSB) mode; in Hardware-CDC/JTAG mode the USB.h /
// USBMSC.h classes do not exist, so we fall back to no-op stubs and the sketch
// still builds (the share screen then reports the feature as unavailable).

#include <Arduino.h>
#include "usb_msc.h"

#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 0

#include "USB.h"
#include "USBMSC.h"
#include <SD.h>          // global SD object the logger mounted — used to read geometry
#include "sdlog.h"       // sdlog_release() — hand the card off cleanly before sharing

// ESP-IDF low-level SD-over-SPI driver. We access the card at the raw sector level
// (NOT through a filesystem) so the PC is the only thing interpreting the FAT.
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

// ── SD bus pins — identical to the logger (JC3248W535C "SD_Car" block, SPI mode) ──
// On the ESP32-S3 the Arduino "HSPI" bus the logger uses maps to SPI3_HOST, so we
// claim the same host here after the logger has freed it.
#define SD_PIN_CS     10
#define SD_PIN_MOSI   11
#define SD_PIN_SCK    12
#define SD_PIN_MISO   13
#define SD_SPI_HOST   SPI3_HOST
#define SD_BLOCK_SIZE 512u
#define SD_FREQ_KHZ   20000          // 20 MHz — same conservative rate as the logger

// 512 MB fallback geometry, used only if no card is present at boot (so the MSC
// interface still has a valid size to advertise). 512 MB / 512 B = 1,048,576 sectors.
#define SD_NOMINAL_SECTORS (1024u * 1024u)

static USBMSC       s_msc;                 // the Mass-Storage interface
static sdmmc_card_t s_card;                // IDF card handle (valid while mounted)
static sdspi_dev_handle_t s_dev = 0;       // IDF sdspi device handle
static bool         s_card_mounted = false;// raw card initialised on the SD bus
static volatile bool s_sharing     = false;// media presented to the PC right now
static uint32_t     s_block_count  = 0;    // sectors advertised over MSC

// ── Raw-sector mount / unmount (IDF sdspi, no filesystem) ─────────────────────

// Bring up the SD card at the raw block level on its SPI bus. The logger MUST have
// released the bus (sdlog_release) before this is called. Returns true on success.
static bool raw_sd_mount() {
    if (s_card_mounted) return true;

    // Initialise the SPI bus on the SD pins.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = SD_PIN_MOSI;
    buscfg.miso_io_num     = SD_PIN_MISO;
    buscfg.sclk_io_num     = SD_PIN_SCK;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = 4096;         // one MSC buffer's worth of sectors
    if (spi_bus_initialize(SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    // Configure the sdspi host + device (chip-select pin, host id).
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t devcfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    devcfg.gpio_cs = (gpio_num_t)SD_PIN_CS;
    devcfg.host_id = SD_SPI_HOST;

    if (sdspi_host_init() != ESP_OK) {
        spi_bus_free(SD_SPI_HOST);
        return false;
    }
    if (sdspi_host_init_device(&devcfg, &s_dev) != ESP_OK) {
        sdspi_host_deinit();
        spi_bus_free(SD_SPI_HOST);
        return false;
    }
    // The device handle becomes host.slot for the card-init transactions.
    host.slot = s_dev;

    if (sdmmc_card_init(&host, &s_card) != ESP_OK) {
        sdspi_host_remove_device(s_dev);
        sdspi_host_deinit();
        spi_bus_free(SD_SPI_HOST);
        s_dev = 0;
        return false;
    }

    s_block_count  = (uint32_t)s_card.csd.capacity;   // real sector count of this card
    s_card_mounted = true;
    return true;
}

// ── MSC SCSI callbacks (called by TinyUSB on the USB task) ────────────────────
// TinyUSB requests whole 512-byte blocks (offset is the byte offset inside the
// first block and is 0 for normal block transfers). bufsize is always a multiple
// of the block size, so count = bufsize / 512.

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)offset;
    if (!s_sharing || !s_card_mounted) return -1;
    uint32_t count = bufsize / SD_BLOCK_SIZE;
    if (count == 0) return 0;
    if (sdmmc_read_sectors(&s_card, buffer, lba, count) != ESP_OK) return -1;
    return (int32_t)(count * SD_BLOCK_SIZE);
}

static int32_t msc_on_write(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)offset;
    if (!s_sharing || !s_card_mounted) return -1;
    uint32_t count = bufsize / SD_BLOCK_SIZE;
    if (count == 0) return 0;
    if (sdmmc_write_sectors(&s_card, buffer, lba, count) != ESP_OK) return -1;
    return (int32_t)(count * SD_BLOCK_SIZE);
}

// Host pressed "eject"/"safely remove" → stop presenting media. We don't physically
// remove the card; we just hide it so the PC releases its handle.
static bool msc_on_startstop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject && !start) {
        s_sharing = false;
        s_msc.mediaPresent(false);
    }
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

void usb_msc_init() {
    // Read the real card geometry from the SD object the logger already mounted.
    // If no card is present, advertise a nominal 512 MB so the interface still has a
    // valid capacity (real geometry is re-read when the user actually shares).
    uint32_t sectors = (uint32_t)SD.numSectors();
    s_block_count = (sectors > 0) ? sectors : SD_NOMINAL_SECTORS;

    s_msc.vendorID("DeskTick");          // <= 8 chars
    s_msc.productID("SD Card");          // <= 16 chars
    s_msc.productRevision("1.0");        // <= 4 chars
    s_msc.onRead(msc_on_read);
    s_msc.onWrite(msc_on_write);
    s_msc.onStartStop(msc_on_startstop);
    // Read & write: registering onWrite() makes the drive writable (the PC may copy,
    // delete, and format). The MSC layer defaults to writable when a write handler is set.
    s_msc.mediaPresent(false);          // no media until the user shares the card
    s_msc.begin(s_block_count, SD_BLOCK_SIZE);

    // Start (or, with CDC-on-boot, finalise) the USB stack. The actual TinyUSB init
    // is deferred to the USB task after setup() returns, so registering MSC here —
    // alongside the auto-registered CDC serial — composes correctly into one device.
    USB.begin();
}

bool usb_msc_begin_share() {
    if (s_sharing) return true;

    // Hand the card off: flush remaining log lines, close the FS, free the SPI bus.
    sdlog_release();
    delay(50);                           // let the bus settle before re-init

    if (!raw_sd_mount()) {
        return false;                    // no card / mount failed
    }

    s_sharing = true;
    s_msc.mediaPresent(true);            // the drive now appears on the PC
    return true;
}

void usb_msc_end_share() {
    s_sharing = false;
    s_msc.mediaPresent(false);
    // The caller reboots immediately after this. A reboot fully resets the sdspi
    // host and SPI bus, so the logger re-mounts the card cleanly on the next boot —
    // no in-place hand-back needed.
}

bool usb_msc_is_sharing() {
    return s_sharing;
}

#else  // ARDUINO_USB_MODE != 0 (Hardware CDC/JTAG) — MSC unavailable

void usb_msc_init() {
    Serial.println("[usb] MSC unavailable — set Tools > USB Mode = \"USB-OTG (TinyUSB)\"");
}
bool usb_msc_begin_share() { return false; }
void usb_msc_end_share()   {}
bool usb_msc_is_sharing()  { return false; }

#endif
