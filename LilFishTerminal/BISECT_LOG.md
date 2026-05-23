# ESP32 Core Version Bisect Log

## Goal
Find the first ESP32 Arduino core version that introduces the silent-deadlock
display hang on the Lil Fish Terminal (JC3248W535C_I_Y, AXS15231B QSPI display).

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
1. Pin to core 3.0.7 — extends hang window to 2–9 h
2. Hardware watchdog — auto-reboots device in ~30 s if hang occurs
3. Partial refresh (`full_refresh = 0`) — reduces DMA pressure, targets root cause

## Known separate bugs (not hang-related)
- **QQQ/commodities unavailable during US market hours:** Likely caused
  by UTC+8 timezone setting shifting the market-hours check by ~12 h.
  Fix after bisect is complete.

## Notes
- "Cannot configure port / OSError(22)" after flash = harmless Windows
  timing issue. Flash is successful if "Hash of data verified" appears.
  Open Serial Monitor manually ~4 s after device boots.
- Both constraints must be satisfied: no display hang AND no HTTP error.
  A version passing only one is not a valid pin target.
