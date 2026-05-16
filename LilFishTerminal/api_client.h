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
