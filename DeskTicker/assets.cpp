// Custom-ticker library + symbol lookup for DeskTicker.
//
// asset_find() lives here (not inline in the header) because custom tickers must
// be held in a single writable table that persists for the whole program lifetime
// and is shared by every translation unit. The built-in ASSETS[] table in
// assets.h stays as-is; this file searches it first, then the user's custom list.

#include "assets.h"
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// ── Writable custom-ticker registry (loaded from NVS at boot) ──────────────────
static AssetDef g_custom[MAX_CUSTOM];          // full definitions of user-added tickers
static bool     g_custom_prov[MAX_CUSTOM];     // true = portal-added, not yet Yahoo-classified
static int      g_custom_n = 0;                // how many slots are in use

// Resolve a symbol to its definition: built-ins win, then customs. nullptr if unknown.
const AssetDef* asset_find(const char* symbol) {
    if (!symbol || !symbol[0]) return nullptr;
    for (int i = 0; i < TOTAL_ASSETS; i++) {
        if (strcmp(ASSETS[i].symbol, symbol) == 0) return &ASSETS[i];
    }
    for (int i = 0; i < g_custom_n; i++) {
        if (strcmp(g_custom[i].symbol, symbol) == 0) return &g_custom[i];
    }
    return nullptr;
}

// Number of custom tickers currently stored.
int custom_count() { return g_custom_n; }

// Definition at index i, or nullptr if out of range.
const AssetDef* custom_get(int i) {
    return (i >= 0 && i < g_custom_n) ? &g_custom[i] : nullptr;
}

// True when no more customs can be added.
bool custom_is_full() { return g_custom_n >= MAX_CUSTOM; }

// Index of a custom by symbol, or -1.
int custom_index(const char* symbol) {
    if (!symbol) return -1;
    for (int i = 0; i < g_custom_n; i++) {
        if (strcmp(g_custom[i].symbol, symbol) == 0) return i;
    }
    return -1;
}

// True if the symbol is one of the built-in tickers.
bool symbol_is_builtin(const char* symbol) {
    if (!symbol) return false;
    for (int i = 0; i < TOTAL_ASSETS; i++) {
        if (strcmp(ASSETS[i].symbol, symbol) == 0) return true;
    }
    return false;
}

// Trim leading/trailing whitespace and uppercase the string in place.
void symbol_normalize(char* s) {
    if (!s) return;
    // Drop leading spaces/tabs.
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    // Drop trailing spaces/tabs.
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    // Uppercase (Yahoo symbols and suffixes like -USD, =X, =F are all uppercase).
    for (int i = 0; s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]);
}

// Add a custom ticker. Rejects empty/over-long symbols, built-in clashes,
// duplicates, and a full library. Does not persist; caller saves afterwards.
bool custom_add(const AssetDef* d, bool provisional) {
    if (!d || !d->symbol[0]) return false;
    if (g_custom_n >= MAX_CUSTOM) return false;
    if (symbol_is_builtin(d->symbol)) return false;
    if (custom_index(d->symbol) >= 0) return false;
    g_custom[g_custom_n]      = *d;
    g_custom_prov[g_custom_n] = provisional;
    g_custom_n++;
    return true;
}

// Remove a custom by symbol, compacting the array. Returns true if removed.
bool custom_remove(const char* symbol) {
    int idx = custom_index(symbol);
    if (idx < 0) return false;
    for (int i = idx; i < g_custom_n - 1; i++) {
        g_custom[i]      = g_custom[i + 1];
        g_custom_prov[i] = g_custom_prov[i + 1];
    }
    g_custom_n--;
    return true;
}

// True if this custom still carries the provisional (un-classified) flag.
bool custom_is_provisional(const char* symbol) {
    int i = custom_index(symbol);
    return (i >= 0) && g_custom_prov[i];
}

// Replace an existing custom's definition and clear its provisional flag.
bool custom_update(const AssetDef* d) {
    if (!d) return false;
    int i = custom_index(d->symbol);
    if (i < 0) return false;
    g_custom[i]      = *d;
    g_custom_prov[i] = false;
    return true;
}

// ── NVS persistence (shares the "lilfish" namespace with settings) ─────────────
// Key scheme: "c_n" (count) + per slot "c{i}_sym", "c{i}_yh", "c{i}_nm",
// "c{i}_m" (market), "c{i}_d" (decimals), "c{i}_c" (continuous), "c{i}_p"
// (provisional). Old NVS without these keys loads as an empty library.

void custom_load_from_nvs() {
    Preferences p;
    p.begin("lilfish", true);  // read-only
    int n = p.getInt("c_n", 0);
    if (n < 0) n = 0;
    if (n > MAX_CUSTOM) n = MAX_CUSTOM;

    g_custom_n = 0;
    for (int i = 0; i < n; i++) {
        char key[12];
        AssetDef d;
        memset(&d, 0, sizeof(d));

        snprintf(key, sizeof(key), "c%d_sym", i);
        p.getString(key, d.symbol, sizeof(d.symbol));
        if (!d.symbol[0]) continue;  // skip a corrupt/empty slot

        snprintf(key, sizeof(key), "c%d_yh", i);
        p.getString(key, d.yahoo, sizeof(d.yahoo));
        snprintf(key, sizeof(key), "c%d_nm", i);
        p.getString(key, d.name, sizeof(d.name));
        if (!d.yahoo[0]) strncpy(d.yahoo, d.symbol, sizeof(d.yahoo) - 1);
        if (!d.name[0])  strncpy(d.name,  d.symbol, sizeof(d.name)  - 1);

        snprintf(key, sizeof(key), "c%d_m", i);
        d.market = (MarketType)p.getUChar(key, MARKET_STOCK);
        snprintf(key, sizeof(key), "c%d_d", i);
        d.decimals = p.getUChar(key, 2);
        snprintf(key, sizeof(key), "c%d_c", i);
        d.continuous = p.getUChar(key, 1);
        snprintf(key, sizeof(key), "c%d_p", i);
        bool prov = p.getUChar(key, 0) != 0;

        g_custom[g_custom_n]      = d;
        g_custom_prov[g_custom_n] = prov;
        g_custom_n++;
    }
    p.end();
}

void custom_save_to_nvs() {
    Preferences p;
    p.begin("lilfish", false);  // read-write
    p.putInt("c_n", g_custom_n);
    for (int i = 0; i < g_custom_n; i++) {
        char key[12];
        snprintf(key, sizeof(key), "c%d_sym", i); p.putString(key, g_custom[i].symbol);
        snprintf(key, sizeof(key), "c%d_yh",  i); p.putString(key, g_custom[i].yahoo);
        snprintf(key, sizeof(key), "c%d_nm",  i); p.putString(key, g_custom[i].name);
        snprintf(key, sizeof(key), "c%d_m",   i); p.putUChar(key, (uint8_t)g_custom[i].market);
        snprintf(key, sizeof(key), "c%d_d",   i); p.putUChar(key, g_custom[i].decimals);
        snprintf(key, sizeof(key), "c%d_c",   i); p.putUChar(key, g_custom[i].continuous);
        snprintf(key, sizeof(key), "c%d_p",   i); p.putUChar(key, g_custom_prov[i] ? 1 : 0);
    }
    p.end();
}
