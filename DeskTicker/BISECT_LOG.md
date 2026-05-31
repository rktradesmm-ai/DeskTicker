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
  **SUPERSEDED 2026-05-30** — the watchdog is now always-on and covers every screen,
  including the live chart. See "Chart-screen watchdog gap fixed" below.

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

## Chart-screen watchdog gap fixed — 2026-05-30 (branch `fix/chart-hang-watchdog`)

**Finding:** the live chart had NO watchdog. The 30 s `esp_timer` watchdog was armed
only by `anim_start()` and torn down by `anim_stop()`, so it ran **only during the
after-hours animation**. When the chart screen hit the silent display deadlock, nothing
rebooted the device — it stayed frozen indefinitely (which is why the chart hang window
was never measurable: no reboot signalled it). The 16 h chart soak (2026-05-24) passed
only because the hang is rare on core 3.0.7, not because anything would have recovered it.

**Fix:** the watchdog is now a single **always-on render-task-liveness watchdog**:
- `render_wdt_init()` (called once in `setup()` after `bsp_display_start()`) arms the
  30 s timer and creates ONE global feed `lv_timer` (`render_feed_cb`, 1 s period).
- The feed timer runs inside `lv_timer_handler()` (the LVGL render task), so it keeps
  feeding on EVERY screen — chart, connecting, settings AND animation. If the render
  task deadlocks (the documented QSPI/TE freeze), `lv_timer_handler()` stops cycling,
  the feed stops, and the device reboots in ≤30 s.
- It is immune to main-loop blocking: a 30 s HTTP fetch never false-fires it, because
  the render task feeds independently of the main loop.
- `anim_start()`/`anim_stop()` no longer arm/disarm the watchdog. The per-animation
  `wdt_feed()` calls remain as harmless redundant feeds. `wdt_stop()` was deleted (the
  watchdog must never be stopped now).
- **This also closes the 2026-05-23 soak caveat** (the after-hours watchdog used to
  reset every 5 min via `anim_stop()`/`anim_start()` and "was never under pressure to
  fire"). The always-on watchdog now runs continuously across those poll cycles.

**Diagnostics added (so the next hang is captured with evidence):**
- An `RTC_DATA_ATTR` marker is written in `wdt_fire()` before reboot (last state, free
  heap/PSRAM, reboot epoch). On the next boot `setup()` reads it via
  `render_wdt_consume_last_reboot()` and prints
  `[WDT] previous boot ended in a render-watchdog reboot: state=… freeHeap=… atEpoch=…`.
  This is how the chart hang window is now measured: note boot time, watch for this
  line, `atEpoch` tells you when it hung.
- A `[health]` line every 60 s in `loop()`: free heap, free PSRAM, largest free PSRAM
  block, current state, and the render heartbeat (`renderHB (+N/min)`). A flat
  heartbeat = the render task is stalling; a falling heap/PSRAM trend = a slow leak.

**Not changed (per decision):** the vendor `portMAX_DELAY` semaphore waits in
`lv_port.c`/`esp_bsp.c` were left at stock — recovery is by reboot, not self-heal. Core
stays 3.0.7; `full_refresh` stays 1. The state-machine `State` enum order is what the
`state=` numbers in the logs refer to (S_INIT=0, S_WIFI_SETUP=1, S_CONNECTING=2,
S_NTP_SYNC=3, S_FETCH=4, S_CHART=5, S_AFTER_HOURS=6, S_RECONNECT=7, S_SETTINGS=8).

**Still needs:** on-device verification on core 3.0.7 — confirm the boot banner
`[WDT] render watchdog armed (always-on, 30s)`, the 60 s `[health]` line with a rising
heartbeat, and (simulated) that forcing a render stall triggers a reboot whose next boot
prints the "previous boot…" line.

## Root cause located AND fixed at source — 2026-05-31 (branch `fix/chart-hang-watchdog`)

After the watchdog work, the vendor port layer was read line-by-line and the silent
deadlock was pinned to **two unbounded `portMAX_DELAY` waits** that run inside the LVGL
render task while it holds the display lock (`lvgl_mux`). Either one blocking forever =
whole UI frozen:

1. **`lv_port.c` `lvgl_port_flush_callback()`** — `xSemaphoreTake(trans_done_sem,
   portMAX_DELAY)` waited forever for each QSPI DMA chunk's "done" signal (given by the
   `on_color_trans_done` ISR `lvgl_port_flush_ready_callback`). If that completion
   signal is lost under QSPI/DMA pressure, it hangs forever.
2. **`esp_bsp.c` `bsp_display_sync_cb()`** (the `draw_wait_cb`, called once per flush) —
   `xSemaphoreTake(te_v_sync_sem, portMAX_DELAY)` waited forever for the display's
   tearing-effect (TE) GPIO interrupt. A missed TE pulse → hangs forever.

This confirms the long-standing "DMA pressure + TE sync" hypothesis at the code level.

**Fix — bound both waits + recover (no reboot needed):**
- `lv_port.c`: wait `LVGL_PORT_FLUSH_TIMEOUT_MS` (100 ms). On timeout, increment
  `lvgl_port_flush_timeouts` and `break` out of the chunk loop. `lv_disp_flush_ready()`
  still runs after the loop, so LVGL is released and (`full_refresh=1`) the whole screen
  repaints next frame.
- `esp_bsp.c`: wait `BSP_TE_SYNC_TIMEOUT_MS` (100 ms). On timeout, increment
  `bsp_te_sync_timeouts` and draw the frame anyway (worst case: one torn frame).
- 100 ms is far above normal latency (~16 ms frame, sub-ms per DMA chunk) and far below
  the 30 s render watchdog, so only a genuine stall trips it.
- Counters are `extern` (`lv_port.h` / `esp_bsp.h`) and printed in the 60 s `[health]`
  line as `flushTO=` / `teTO=`. **Climbing counters with no freeze = the fix is catching
  real misses that previously WOULD have frozen the device.**

**Layering:** this removes the deadlock itself; the always-on render watchdog (previous
section) remains as the final backstop if anything else ever wedges the render task.

**Why NOT upgrade LVGL to fix this:** the bug is in board glue code *below* LVGL
(`lv_port.c` / `esp_bsp.c`), not in LVGL. Upgrading LVGL (e.g. to 9.x) would not touch
these waits, would force a full rewrite of `lv_port.c` against LVGL 9's new display API
(`lv_display_t`, new flush-cb signature) plus all screen code (still v8 API), and would
re-introduce the same two waits. Ruled out. Core stays 3.0.7; LVGL stays v8.3.x;
`full_refresh` stays 1.

**Recovery trade-off (accepted):** a timed-out flush breaks before all chunks draw; a
late DMA-done ISR may leave `trans_done_sem` pre-signalled so the next frame's first
chunk skips its wait → at most one torn/partial frame, self-corrected by the next full
refresh. Strictly better than a permanent freeze.

**Still needs:** on-device soak on core 3.0.7. Success = no freeze, no watchdog reboot;
non-zero `flushTO`/`teTO` are the recovered events. To prove recovery quickly, temporarily
drop the two timeouts to ~5 ms, confirm the UI keeps running while the counters climb,
then restore 100 ms.

## Phase=4 confirmed — watchdog to 5s + sub-phase locator — 2026-05-31

### Soak 1 (`freezeTest1.txt`): bounded waits did NOT catch the freeze

Flashed `493c2c9` (bounded `flushTO`/`teTO` waits). Chart hung, watchdog rebooted.
Key from the log: `flushTO=0 teTO=0` at reboot. Neither of the two bounded waits
we added ever fired. The render task froze somewhere else, with all phase=3 and
teTO=2 paths clear. The bounded waits are correct defensive code but are not on
the critical path for this hang.

→ Added render-phase locator (`lvgl_render_phase`, `lvgl_render_chunk`) and bounded
render-loop mutex acquire (`lockTO`) — commit `830d7fb`.

### Soak 2 (`freezeTest2.txt`): **`phase=4 chunk=3` — root cause nailed**

```
[health] state=5 ... flushTO=0 teTO=0 lockTO=0 phase=4 chunk=3
[WDT] render watchdog: no frame in 30s, rebooting (state=5 phase=4 chunk=3 ...)
```

**All suspects but one are now ruled out:**
- `flushTO=0` → our bounded `trans_done_sem` wait (phase 3) never triggered
- `teTO=0`   → our bounded TE sync wait (phase 2) never triggered
- `lockTO=0` → our bounded render-loop mutex acquire (phase 6) never triggered
- **`phase=4 chunk=3` → render task was stuck inside `esp_lcd_panel_draw_bitmap()`
  at `lv_port.c:580`, specifically in chunk 3's call, for the full 30 s watchdog window**

**What happens inside `esp_lcd_panel_draw_bitmap()` (`esp_lcd_axs15231b.c:287`):**

```c
// A: CASET command send — precompiled esp_lcd_panel_io_tx_param (portMAX_DELAY inside)
tx_param(axs15231b, io, LCD_CMD_CASET, ...);
// B: pixel DMA transfer — precompiled esp_lcd_panel_io_tx_color (portMAX_DELAY inside)
tx_color(axs15231b, io, LCD_CMD_RAMWR/RAMWRC, color_data, len);
```

Both sub-calls go into precompiled `esp_lcd_panel_io_tx_*` functions that contain their
own `spi_device_queue_trans(portMAX_DELAY)` / `spi_device_get_trans_result(portMAX_DELAY)`
waits. If the SPI DMA interrupt is lost under bus pressure (WiFi reconnect + 21 KB JSON
parse), either blocks forever — our `trans_done_sem` ISR path is not involved, so
`flushTO` stays 0.

**Phase=4 covered the whole function.** We do not yet know whether sub-call A (CASET
command) or B (RAMWR/RAMWRC pixel DMA) is the specific blocking point.

### Actions taken — 2026-05-31

1. **Watchdog reduced from 30s → 5s** (`animations.cpp` `wdt_start()`/`wdt_feed()`).
   The render task feeds every 1s; 5s gives 5× headroom with no false-fire risk.
   Effect: freeze duration drops from 30s to ≤5s.

2. **Sub-phase marker added in `panel_axs15231b_draw_bitmap`:**
   - `phase=7` set before `tx_param` (CASET command send)
   - `phase=4` set before `tx_color` (RAMWR/RAMWRC pixel DMA)
   Next freeze will say `phase=7` (CASET is stuck) or `phase=4` (pixel DMA is stuck).
   This is the final remaining unknown.

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
4. ~~**`anim_start()` must call `wdt_start()`. `anim_stop()` must call `wdt_stop()`.**~~
   **OBSOLETE since 2026-05-30.** The watchdog is now always-on (armed once in
   `render_wdt_init()` from `setup()`) and is fed by a global render-task `lv_timer`,
   not by per-animation arming. New animations need do nothing for watchdog coverage —
   they are protected automatically. Do NOT call `wdt_start()`/`wdt_stop()` from an
   animation (the latter no longer exists). Keeping `wdt_feed()` in a timer callback is
   fine but redundant.

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

---

## Bounded-wait fix did NOT prevent the hang — 2026-05-31 soak data

**Test:** flashed `fix/chart-hang-watchdog` (commit `493c2c9`) with both bounded
waits (`LVGL_PORT_FLUSH_TIMEOUT_MS = 100 ms`, `BSP_TE_SYNC_TIMEOUT_MS = 100 ms`)
and `flushTO`/`teTO` counters in the `[health]` log. Ran the live chart on core 3.0.7.

**Result (from `freezeTest1.txt`):**
```
[health] state=5 ... renderHB=8607 (+89/min) flushTO=0 teTO=0
...
[WDT] render watchdog: no frame in 30s, rebooting (state=5 heap=186340 psram=7853060 hb=8740)
```

- Hung in **state=5 (S_CHART)**, right after a WiFi reconnect + a heavy BTC fetch
  (21756 bytes body) — peak bus/CPU pressure.
- **`flushTO=0` and `teTO=0`** at reboot — NEITHER bounded wait we added ever fired.
- `renderHB` climbed 8607→8725→8740 then stopped; 30 s later watchdog rebooted.

**Conclusion:** Our two bounded waits are correct but did NOT address the right spot.
The render task froze somewhere else — the two waits we bounded are not on the critical
path during this type of hang. Leading suspect: the precompiled
`esp_lcd_panel_io_tx_color()` call buried inside `esp_lcd_panel_draw_bitmap()` (called
from `lvgl_port_flush_callback():580`), which has its own internal descriptor/bus
semaphore wait that we cannot see or edit.

**Action taken — 2026-05-31:** Added a render-phase locator that stashes the exact
step into RTC RAM before each watchdog reboot, so the NEXT freeze's `[WDT] previous
boot` line will say `phase=4 chunk=N` (or whichever step is actually stuck) instead of
requiring us to guess:
- **Phase values:** 0=idle/timer-cb, 2=TE sync wait (bounded `teTO`), 3=chunk
  DMA-done wait (bounded `flushTO`), **4=inside precompiled `esp_lcd_panel_draw_bitmap`
  (the suspected stuck point)**, 5=flush done, 6=render-loop mutex wait (bounded `lockTO`)
- Also added `lockTO` (bounded the render-loop `lvgl_mux` acquire, was `portMAX_DELAY`)
  and surfaced `lockTO=/phase=/chunk=` in the 60 s `[health]` log.
- After the next freeze, read the `[WDT]` boot line: `phase=4` confirms the draw path;
  `phase=6` means mutex starvation; `phase=0` means a chart timer callback is blocking.
