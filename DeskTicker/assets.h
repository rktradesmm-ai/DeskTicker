#pragma once
#include <stdint.h>

#define MAX_ASSETS      6
#define MAX_CANDLES     60
#define ASSET_SYM_LEN   10
#define ASSET_NAME_LEN  20
#define ASSET_YAHOO_LEN 16

// Maximum number of user-added "custom" tickers kept in the saved library
// (in addition to the built-in ASSETS[] table). Persisted in NVS.
#define MAX_CUSTOM      6

typedef enum {
    MARKET_STOCK     = 0,
    MARKET_CRYPTO    = 1,
    MARKET_COMMODITY = 2,
    MARKET_FOREX     = 3
} MarketType;

typedef enum {
    MSTATE_OPEN   = 0,
    MSTATE_PRE    = 1,
    MSTATE_POST   = 2,
    MSTATE_CLOSED = 3
} MarketState;

typedef struct {
    char     symbol[ASSET_SYM_LEN];
    char     name[ASSET_NAME_LEN];
    char     yahoo[ASSET_YAHOO_LEN];
    MarketType market;
    uint8_t  decimals;   // price display decimals
    uint8_t  continuous; // 1 = trades ~24h; use crypto-sized fetch windows to keep JSON small
} AssetDef;

typedef struct {
    float    open, high, low, close;
    uint32_t ts;
    long     volume;
} Candle;

typedef struct {
    char        symbol[ASSET_SYM_LEN];
    float       price;
    float       prev_close;
    float       change_pct;
    Candle      candles[MAX_CANDLES];
    int         candle_count;
    MarketState market_state;
    uint32_t    last_fetch;
    // Market-session metadata parsed from Yahoo's chart `meta` object. Used by
    // is_after_hours() to decide live chart vs after-hours animation per asset class.
    uint32_t    reg_start;     // currentTradingPeriod.regular.start (epoch s); 0 if absent
    uint32_t    reg_end;       // currentTradingPeriod.regular.end   (epoch s); 0 if absent
    uint32_t    reg_mkt_time;  // regularMarketTime (last-trade epoch s); 0 if absent
    bool        valid;
    char        err[48];
} AssetData;

#define TOTAL_ASSETS 25

static const AssetDef ASSETS[TOTAL_ASSETS] = {
    // Stocks & ETFs (regular exchange hours, not continuous)
    {"SPY",    "S&P 500 ETF",   "SPY",       MARKET_STOCK,     2, 0},
    {"QQQ",    "Nasdaq 100",    "QQQ",       MARKET_STOCK,     2, 0},
    {"DIA",    "Dow Jones",     "DIA",       MARKET_STOCK,     2, 0},
    {"IWM",    "Russell 2000",  "IWM",       MARKET_STOCK,     2, 0},
    {"AAPL",   "Apple",         "AAPL",      MARKET_STOCK,     2, 0},
    {"MSFT",   "Microsoft",     "MSFT",      MARKET_STOCK,     2, 0},
    {"AMZN",   "Amazon",        "AMZN",      MARKET_STOCK,     2, 0},
    {"GOOGL",  "Alphabet",      "GOOGL",     MARKET_STOCK,     2, 0},
    {"TSLA",   "Tesla",         "TSLA",      MARKET_STOCK,     2, 0},
    {"NVDA",   "NVIDIA",        "NVDA",      MARKET_STOCK,     2, 0},
    {"META",   "Meta",          "META",      MARKET_STOCK,     2, 0},
    // Index futures — CME Globex ~23h/day; continuous=1 keeps fetch window small
    {"ES",     "S&P 500 Fut",   "ES=F",      MARKET_STOCK,     2, 1},
    {"NQ",     "Nasdaq 100 Fut","NQ=F",      MARKET_STOCK,     2, 1},
    // Crypto (continuous handled by MARKET_CRYPTO in yf_period1)
    {"BTC",    "Bitcoin",       "BTC-USD",   MARKET_CRYPTO,    2, 0},
    {"ETH",    "Ethereum",      "ETH-USD",   MARKET_CRYPTO,    2, 0},
    {"LINK",   "Chainlink",     "LINK-USD",  MARKET_CRYPTO,    2, 0},
    {"XRP",    "XRP",           "XRP-USD",   MARKET_CRYPTO,    2, 0},
    {"DOGE",   "Dogecoin",      "DOGE-USD",  MARKET_CRYPTO,    5, 0},
    // Commodities — these trade 24/7 as crypto-style tokens; continuous=1 fixes the reset crash
    {"GOLD",   "Gold",          "XAUT-USD",      MARKET_COMMODITY, 2, 1},
    {"SILVER", "Silver",        "XAG39343-USD",  MARKET_COMMODITY, 3, 1},
    {"OIL",    "Crude Oil",     "CL-USD",        MARKET_COMMODITY, 2, 1},
    // Forex (standard ~24/5 FX session)
    {"USD/JPY","USD/JPY",       "JPY=X",     MARKET_FOREX,     3, 0},
    {"EUR/USD","EUR/USD",       "EURUSD=X",  MARKET_FOREX,     4, 0},
    {"GBP/USD","GBP/USD",       "GBPUSD=X",  MARKET_FOREX,     4, 0},
    {"DXY",    "US Dollar Idx", "DX-Y.NYB",  MARKET_FOREX,     3, 0},
};

// Resolve a user-facing symbol (e.g. "BTC", "PLTR") to its full definition.
// Searches the built-in ASSETS[] table first, then the user's custom library.
// Returns nullptr if the symbol is unknown. Defined in assets.cpp.
const AssetDef* asset_find(const char* symbol);

// ── Custom-ticker library ─────────────────────────────────────────────────────
// User-added Yahoo Finance tickers, stored as full AssetDefs in a small writable
// registry that lives in RAM for the program's lifetime and is persisted to NVS.
// Built-in tickers always take priority in asset_find(); a custom can never shadow
// one. Implemented in assets.cpp.

// How many custom tickers are currently in the library (0..MAX_CUSTOM).
int custom_count();
// Get the custom AssetDef at index i (0..custom_count()-1), or nullptr if out of range.
const AssetDef* custom_get(int i);
// True if the custom library is at capacity (cannot add more).
bool custom_is_full();
// Index of a custom by symbol, or -1 if not present.
int custom_index(const char* symbol);
// True if `symbol` matches one of the built-in ASSETS[] entries.
bool symbol_is_builtin(const char* symbol);
// Trim surrounding spaces and uppercase `s` in place (Yahoo symbols are uppercase).
void symbol_normalize(char* s);
// Add a custom ticker. Rejects duplicates, built-in clashes, or a full library.
// `provisional` marks a portal-added ticker whose type still needs Yahoo classification.
// Returns true on success. Does NOT persist — call custom_save_to_nvs() after.
bool custom_add(const AssetDef* d, bool provisional);
// Remove a custom ticker by symbol. Returns true if it was present.
bool custom_remove(const char* symbol);
// True if the named custom is still provisional (added via the setup portal,
// not yet classified against Yahoo).
bool custom_is_provisional(const char* symbol);
// Replace an existing custom's definition (matched by symbol) and clear its
// provisional flag. Returns false if no custom with that symbol exists.
bool custom_update(const AssetDef* d);
// Load / save the custom library from / to NVS (namespace "lilfish").
void custom_load_from_nvs();
void custom_save_to_nvs();
