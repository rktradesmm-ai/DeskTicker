# ESP32 Core Version Bisect Log

## Goal
Find the first ESP32 Arduino core version that introduces the silent-deadlock
display hang on DeskTicker (JC3248W535C_I_Y, AXS15231B QSPI display).

## Test setup
- **Code:** always the baseline worktree at `C:\Users\squal\FishBaseline`
  (commit `48269e2`, tag `v1.0-core-3.0.2-known-good`) — simple pixelated
  fish, no visual improvements, no settings menu.
- **Only variable:** ESP32 core version in Arduino IDE Boards Manager.
- **Pass criteria:** aquarium animation survives 60+ min with no serial
  silence AND chart/HTTP loads without IncompleteInput error.

## Results

| Version | Display hang? | HTTP/Chart ok? | Notes |
|---------|--------------|----------------|-------|
| 3.0.2 | GOOD — no hang | BAD — "YF: Empty body / IncompleteInput" | 3.0.2 HTTP client bug (not Yahoo Finance); can't enter aquarium due to failed fetch |
| 3.1.0 | BAD — hangs ~40 min | GOOD — no IncompleteInput; crypto chart ran >17 h no hang | HTTP bug fixed in 3.1.0; display regression also introduced |
| 3.2.1 | BAD — hangs | unknown | Chart freeze within 30 min |
| 3.3.8 | BAD — hangs ~30–45 min | GOOD | Current project core version |
| 3.0.7 | BAD — hangs 2–9 hours | GOOD — no IncompleteInput | **Best available.** Longest hang window; HTTP works. Chosen as pin target. |

## Bisect conclusion
The hang exists on **every tested core version** — it is not a clean good/bad
boundary. Newer cores trigger it faster. There is no "safe" core to pin to that
eliminates the hang by itself.

**3.0.7 is the best available core:** longest hang window (2–9 h) AND no HTTP
client bug. Bisect is complete.

| Version | Hang time | HTTP |
|---------|-----------|------|
| 3.0.2 | unknown (never long-tested) | BAD |
| 3.0.7 | 2–9 hours | GOOD |
| 3.1.0 | ~40 min | GOOD |
| 3.2.1 | shorter | unknown |
| 3.3.8 | 30–45 min | GOOD |

## Next steps (firmware fixes on top of core 3.0.7 pin)
1. ~~Pin to core 3.0.7 — extends hang window to 2–9 h~~ **DONE**
2. ~~Hardware watchdog — auto-reboots device in ~30 s if hang occurs~~ **DONE** (see below)
3. ~~Partial refresh (`full_refresh = 0`) — reduces DMA pressure, targets root cause~~ **REVERTED — permanent incompatibility** (see below)

---

## Post-bisect firmware changes

### Hardware watchdog — DONE (tag `v2.x`)
Added an `esp_timer`-based hardware watchdog with a 30-second timeout.
- `wdt_feed()` is called every 120 ms from every animation timer callback.
- `wdt_start()` is called in `anim_start()`; `wdt_stop()` is called in `anim_stop()`.
- If an animation freezes and stops calling `wdt_feed()`, the device auto-reboots within 30 s.
- Watchdog does not run during chart / connecting / settings screens (only after-hours).

### Partial refresh — REVERTED — permanent incompatibility (tag `v2.x`)
Tested `full_refresh = 0` in `lv_port.c`. Result: immediate, severe garbled output —
diagonal colour stripes on the settings page and double/overlapping frames on the chart.

Root cause: the QSPI flush callback performs a **software 90° coordinate rotation** that
was written for full-screen writes only. Partial-area `(x1,y1)→(x2,y2)` coordinates are
mis-mapped through the rotation, landing in the wrong physical display region.

**`full_refresh` must stay `1` permanently.** Do not attempt partial refresh again without
first rewriting the flush callback's rotation to handle sub-screen areas.

---

## Soak test — 2026-05-23 (Coral Reef animation, core 3.0.7 + watchdog)

**Result: 6 hours, no hang.**

Device ran the Coral Reef after-hours animation on ESP32 core 3.0.7 with the hardware
watchdog enabled. No freeze, no reboot, no serial silence for 6 hours.

### Important caveat — this is NOT a true continuous soak test

The after-hours state in `DeskTicker.ino` re-checks market hours every **5 minutes**
(`300000UL` ms). Each re-check calls `anim_stop()` → fetches QQQ → `anim_start()`.

- `anim_stop()` calls `wdt_stop()` — the watchdog timer is cancelled.
- `anim_start()` calls `wdt_start()` — a fresh 30-second watchdog starts.

So the animation **never runs for more than ~5 minutes at a stretch**. The 6-hour "soak
test" was actually ~72 consecutive 5-minute runs. The watchdog is reset every cycle —
it is never under pressure to fire.

A genuine soak test requires temporarily extending the poll interval to prevent
`anim_stop()` from firing. Example: change `300000UL` (5 min) to `36000000UL` (10 hours)
in `DeskTicker.ino`, flash, and let the animation run uninterrupted overnight.
The watchdog fix has **not yet been stress-tested** under truly continuous animation.

---

## Soak test — 2026-05-24 (live chart screen, core 3.0.7)

**Result: 16+ hours, no hang.**

Device ran the live chart screen continuously on ESP32 core 3.0.7. No freeze, no reboot,
no serial silence over 16+ hours.

### Scope of this test

- **Screen under test:** chart screen (market hours path), NOT after-hours animation.
- **Watchdog status:** not active. `wdt_start()` is only called from `anim_start()`,
  which only fires during after-hours. The chart screen never feeds the watchdog and
  never has it running.
- **What this validates:** the chart/HTTP path on core 3.0.7 is stable for at least
  16 h uninterrupted.
- **What this does NOT validate:** the after-hours animation path under truly continuous
  load (still gated by the 5 min poll caveat above — `anim_stop()`/`anim_start()` resets
  the watchdog every cycle).

Combined picture so far on core 3.0.7:
- Chart path: 16+ h continuous, no hang. ✓
- Animation path: 6 h in 5-min slices, no hang. Watchdog never had to fire. ⚠ caveat.

---

## Known separate bugs (not hang-related)
- **QQQ/commodities unavailable during US market hours:** Likely caused
  by UTC+8 timezone setting shifting the market-hours check by ~12 h.
  Deferred.

## Notes
- "Cannot configure port / OSError(22)" after flash = harmless Windows
  timing issue. Flash is successful if "Hash of data verified" appears.
  Open Serial Monitor manually ~4 s after device boots.
- Both constraints must be satisfied: no display hang AND no HTTP error.
  A version passing only one is not a valid pin target.

---

## Guidance for adding or redesigning after-hours animations

Read this before writing new animation code. The hang behaviour documented above is
present on every tested ESP32 core. Core 3.0.7 widens the window but does not eliminate
it. The hardware watchdog is the safety net. New animations must not break the safety
net.

### Hard constraints (do not violate)

1. **ESP32 core stays pinned to 3.0.7.** Do not bump the core version to get a newer
   LVGL or new Arduino API. The hang window collapses on 3.1.0+ (~40 min) and 3.3.8
   (~30–45 min).
2. **`full_refresh = 1` in `lv_port.c` stays.** Partial refresh garbles the display
   because the QSPI flush callback's software 90° rotation only handles full-screen
   writes. See the "Partial refresh — REVERTED" section above.
3. **Every animation timer callback must call `wdt_feed()` at least every ~120 ms.**
   This is what keeps the 30 s hardware watchdog alive. If a new animation has a
   callback that runs less often than that, add a separate `lv_timer` whose only job
   is to feed the watchdog.
4. **`anim_start()` must call `wdt_start()`. `anim_stop()` must call `wdt_stop()`.**
   The existing helpers handle this. New animations should be plumbed into the same
   start/stop path, not bypass it.

### If a new animation reintroduces a hang

Before assuming it is the same core-level bug, check in this order:

1. Is `wdt_feed()` being called from the new animation's timer callback? If the
   watchdog never fires after a freeze, the callback stopped running but the watchdog
   was also never armed — OR the freeze is in the LVGL render path (which still feeds
   the watchdog from other timers).
2. Does the new animation do any blocking work in a timer callback (HTTP fetch, large
   `delay()`, file I/O)? Timer callbacks must return quickly. Move blocking work to
   the main loop or a FreeRTOS task.
3. Does the new animation allocate large DMA buffers or trigger many full-screen
   redraws per second? DMA pressure on the QSPI bus is the suspected root cause of
   the underlying hang.
4. If none of the above explains it, the new animation has hit the same core-level
   bug as the original. The fix is not in this codebase — it is in the ESP32 Arduino
   core. Rely on the watchdog reboot as mitigation.

### How to run a true continuous soak test

The 6 h after-hours test is NOT a continuous test (see caveat above). To actually
stress-test the watchdog and a new animation under continuous load:

1. In `DeskTicker.ino`, temporarily change the after-hours poll interval from
   `300000UL` (5 min) to `36000000UL` (10 h).
2. Flash and let the device sit on the after-hours animation overnight.
3. If the watchdog fires during the test, the serial monitor will show a reboot and
   the watchdog reset reason. That is the signal that the new animation hung and the
   watchdog saved it.
4. Revert the poll interval before committing.
