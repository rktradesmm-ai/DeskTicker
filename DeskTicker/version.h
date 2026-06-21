#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Firmware version shown on the on-device About / Diagnostics screen.
// Keep this in sync with the web updater:
//   - docs/manifest.json  ("version")
//   - the "Latest firmware" line in docs/index.html
//   - the docs/firmware/<ver>/ folder name
// Bump FW_VERSION + FW_DATE together every time you publish a new release so a
// customer can compare what's on their device to what the updater page offers.
// ─────────────────────────────────────────────────────────────────────────────
#define FW_VERSION "1.0.0"
#define FW_DATE    "2026-06-21"
