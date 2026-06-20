# Firmware binaries — v1.0.0

Drop the four files the web installer needs into **this folder**. They come from
the Arduino IDE "Export Compiled Binary" step (see `../../PUBLISHING.md`):

| File | Source | Flash offset |
|------|--------|--------------|
| `DeskTicker.ino.bootloader.bin` | Export Compiled Binary output | `0x0` (0) |
| `DeskTicker.ino.partitions.bin` | Export Compiled Binary output | `0x8000` (32768) |
| `boot_app0.bin` | ESP32 core `tools/partitions/boot_app0.bin` | `0xe000` (57344) |
| `DeskTicker.ino.bin` | Export Compiled Binary output (the app) | `0x10000` (65536) |

The offsets above are also written into `../../manifest.json`. **Verify them
against the verbose Arduino build/flash command** before each release — if the
partition scheme ever changes, these move.

> NVS (saved settings/Wi-Fi at `0x9000`) is deliberately **not** listed, so a
> normal update keeps the customer's settings. Ticking "Erase device" in the
> installer wipes everything for a clean install.

This folder is currently a placeholder — the actual `.bin` files are not yet
committed. Add them, then commit.
