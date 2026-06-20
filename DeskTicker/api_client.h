#pragma once
#include "assets.h"
#include "settings.h"

// Fetch candle data for assets[idx] and populate out.
// Returns true on success.
bool api_fetch(const Settings* s, int idx, AssetData* out);

// Returns the auto-refresh interval in ms based on timeframe.
unsigned long api_refresh_interval(int timeframe);

// Returns true if this asset type is always open (crypto).
bool asset_always_open(const AssetDef* a);

// ── Custom-ticker validation / auto-classification ────────────────────────────
// Result of probing a raw user-typed symbol against Yahoo Finance.
typedef struct {
    bool     ok;        // true if the symbol is valid and `def` is populated
    AssetDef def;       // fully-filled definition (symbol/name/yahoo/market/decimals/continuous)
    char     err[48];   // human-readable reason when ok == false
} AssetProbeResult;

// Look up a single symbol on Yahoo Finance and auto-classify it: reads
// meta.instrumentType (→ market type + 24/7 flag), shortName/longName (→ display
// name) and priceHint (→ decimals). Requires an active WiFi (STA) connection and
// must be called from the main loop (NOT an LVGL callback) wrapped in the same
// lvgl_flush_suspended window as a normal fetch. Returns true on success.
bool api_probe_symbol(const char* symbol, AssetProbeResult* out);
