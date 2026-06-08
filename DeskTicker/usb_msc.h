#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── USB Mass-Storage "Share SD over USB" feature ──────────────────────────────
// Lets the on-board micro-SD card be read/written directly from a PC over the
// USB-C cable, by presenting it as a USB drive (Mass Storage Class, MSC).
//
// HARD RULE — one owner at a time:
//   USB MSC gives the PC raw block-level access to the card. The firmware's SD
//   logger ALSO writes the card. Two filesystem owners writing the same medium =
//   corruption. So sharing is a distinct mode: the firmware first releases the
//   card (sdlog_release()), then hands it to the PC. The device reboots when the
//   user leaves share mode, which cleanly returns the card to the logger.
//
// Build requirement (Arduino IDE → Tools):
//   USB Mode        = "USB-OTG (TinyUSB)"   ← REQUIRED (default is Hardware CDC/JTAG)
//   USB CDC On Boot = "Enabled"             (keeps the serial monitor working)
//   If USB Mode is left on Hardware CDC/JTAG, MSC is unavailable and these calls
//   are harmless no-ops (the share screen reports "unavailable").

// Register the USB device as a composite CDC(serial) + MSC(removable disk) and
// start the USB stack. Call ONCE in setup(), AFTER sdlog_init() (so the card's
// sector geometry can be read from the already-mounted SD object) and before the
// display init. The disk starts with NO media present — the card stays owned by
// the logger until the user explicitly shares it. Safe with no card inserted.
void usb_msc_init();

// Enter share mode: flush + release the SD card from the logger, mount it at the
// raw-sector level, and present it to the PC as a read/write USB drive.
// Returns true if media is now visible to the PC; false if no card / mount failed
// (the caller should show an error instead of the "shared" screen).
bool usb_msc_begin_share();

// Leave share mode: stop presenting media to the PC. The caller reboots right
// after, which fully resets the SD/SPI bus and hands the card back to the logger.
void usb_msc_end_share();

// True while the card is actively shared with the PC (media present).
bool usb_msc_is_sharing();
