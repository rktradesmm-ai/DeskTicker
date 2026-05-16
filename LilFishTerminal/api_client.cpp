#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "api_client.h"

// ══════════════════════════════════════════════════════════════════════════════
// YAHOO FINANCE  (stocks, ETFs, commodities, forex, crypto — all assets)
//
// Root cause of IncompleteInput / EmptyInput on ESP32:
//   - range=60d with interval=60m returns 300–400 candles → 50–80 KB raw JSON.
//     The ESP32 heap can't buffer it during streaming → truncation mid-parse.
// Fix:
//   1. Replace "range" with period1/period2 timestamps so we only fetch the
//      bars we actually need (< 20 KB for all timeframes).
//   2. Use http.getString() to read the complete body into a String first,
//      then parse — avoids every streaming / chunked / TLS timeout edge case.
//   3. useHTTP10(true) + Accept-Encoding:identity to prevent gzip/chunked.
// ══════════════════════════════════════════════════════════════════════════════

#define YF_HOST_PRIMARY  "query1.finance.yahoo.com"
#define YF_HOST_FALLBACK "query2.finance.yahoo.com"

// Route ArduinoJson document memory to PSRAM (same as the LVGL canvas buffers)
// so the large parse buffer never competes with the internal heap / TLS buffers.
// Using the internal heap (DynamicJsonDocument default) caused a fragmentation
// reset when swiping from a crypto asset (double-TLS on 1D) to a commodity asset.
struct SpiRamAllocator {
    void* allocate(size_t sz)            { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p)            { heap_caps_free(p); }
    void* reallocate(void* p, size_t sz) { return heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM); }
};
using PsramJsonDocument = BasicJsonDocument<SpiRamAllocator>;

// ── Per-asset cache: stores the true 00:00-UTC open for each crypto asset ──
// Fetched once per UTC calendar day (utc_day = unix_time / 86400) so the 1D
// timeframe uses the exact same daily-open anchor as 15m / 1h / 4h.
struct CryptoDayOpen { char ticker[ASSET_SYM_LEN]; uint32_t utc_day; float open; };
static CryptoDayOpen g_day_open[8];  // slots for up to 6 assets (+ 2 spare)

// Return the cached open for this ticker on utc_day, or 0 if not cached.
static float cache_lookup(const char* ticker, uint32_t utc_day) {
    for (int i = 0; i < 8; i++) {
        if (g_day_open[i].open > 0.0f &&
            g_day_open[i].utc_day == utc_day &&
            strncmp(g_day_open[i].ticker, ticker, ASSET_SYM_LEN) == 0) {
            return g_day_open[i].open;
        }
    }
    return 0.0f;
}

// Store the open for this ticker on utc_day.
// Overwrites the existing slot for this ticker, or the first empty/zero slot.
static void cache_store(const char* ticker, uint32_t utc_day, float open) {
    // Look for an existing slot for this ticker first.
    for (int i = 0; i < 8; i++) {
        if (strncmp(g_day_open[i].ticker, ticker, ASSET_SYM_LEN) == 0) {
            g_day_open[i].utc_day = utc_day;
            g_day_open[i].open    = open;
            return;
        }
    }
    // No existing slot — find the first empty one.
    for (int i = 0; i < 8; i++) {
        if (g_day_open[i].open <= 0.0f) {
            strncpy(g_day_open[i].ticker, ticker, ASSET_SYM_LEN - 1);
            g_day_open[i].ticker[ASSET_SYM_LEN - 1] = '\0';
            g_day_open[i].utc_day = utc_day;
            g_day_open[i].open    = open;
            return;
        }
    }
    // All slots full (more than 8 crypto assets — shouldn't happen with 6 max).
    // Overwrite slot 0 as a safety fallback.
    strncpy(g_day_open[0].ticker, ticker, ASSET_SYM_LEN - 1);
    g_day_open[0].ticker[ASSET_SYM_LEN - 1] = '\0';
    g_day_open[0].utc_day = utc_day;
    g_day_open[0].open    = open;
}

static void get_yf_interval(int tf, char* interval, size_t ilen) {
    switch (tf) {
        case TF_15M: strncpy(interval, "15m", ilen); break;
        case TF_1H:  strncpy(interval, "60m", ilen); break;
        case TF_4H:  strncpy(interval, "60m", ilen); break;  // aggregated after fetch
        default:     strncpy(interval, "1d",  ilen); break;
    }
}

// Calendar-day window that gives ~MAX_CANDLES bars (with buffer for weekends/holidays).
// Crypto trades 24/7; stocks ~6.5 h/day Mon–Fri.
// continuous=1 for 24/7-traded assets (crypto tokens, some futures) so the
// window is sized correctly and the JSON body stays small enough for the ESP32 heap.
static time_t yf_period1(int tf, MarketType market, bool continuous) {
    time_t now = time(nullptr);
    if (now < 1704067200L) now = 1704067200L;  // fallback: Jan 1 2024

    if (market == MARKET_CRYPTO || continuous) {
        switch (tf) {
            case TF_15M: return now -  3L * 86400;  //  3 cal days → ~288 15m bars
            case TF_1H:  return now -  5L * 86400;  //  5 cal days → ~120 1h bars
            case TF_4H:  return now - 12L * 86400;  // 12 cal days → ~288 1h → ~72 4h bars
            default:     return now - 90L * 86400;
        }
    } else {
        // NYSE: ~6.5 h/day, 5 days/week
        switch (tf) {
            case TF_15M: return now -  5L * 86400;  //  5 cal days → ~130 15m bars
            case TF_1H:  return now - 14L * 86400;  // 14 cal days → ~91 1h bars
            case TF_4H:  return now - 55L * 86400;  // 55 cal days → ~240 1h → ~60 4h bars
            default:     return now - 90L * 86400;  // 90 cal days → ~63 1d bars
        }
    }
}


// Find the close price of the last completed calendar day before the newest
// candle's day, using UTC day boundaries (Yahoo crypto candles are UTC-based).
// This is the "previous daily close" that Yahoo's quote page shows as the
// reference for its daily % change — works for all timeframes:
//   - Daily series:   the previous candle is yesterday's bar → exact match.
//   - Intraday:       scans back to the last bar of the prior UTC day → very
//                     close to the daily close for 24/7 crypto assets.
// Returns 0.0f if there are fewer than 2 candles or no prior day is found.
static float prev_day_close(const AssetData* d) {
    if (d->candle_count < 2) return 0.0f;
    time_t newest_ts = (time_t)d->candles[d->candle_count - 1].ts;
    struct tm newest_day;
    gmtime_r(&newest_ts, &newest_day);
    int today_yday  = newest_day.tm_yday;
    int today_year  = newest_day.tm_year;
    for (int i = d->candle_count - 2; i >= 0; i--) {
        struct tm bar_day;
        time_t bar_ts = (time_t)d->candles[i].ts;
        gmtime_r(&bar_ts, &bar_day);
        if (bar_day.tm_yday != today_yday || bar_day.tm_year != today_year) {
            return d->candles[i].close;  // last bar of the previous UTC day
        }
    }
    return 0.0f;  // all candles fall on the same calendar day
}

// Find the open price of the first candle that belongs to today's UTC calendar
// day — this is the "Binance-style fixed-point daily open" (Binance daily
// candles also reset at 00:00 UTC). The % change using this as the reference
// shows how much the price has moved since the start of today, which matches
// what Binance's 1D chart candle shows as its "change since open."
// For a daily-interval series the newest candle IS today's bar, so we just
// walk backward until we reach the first bar still on the same UTC day.
// Returns 0.0f if no candles are available.
static float today_open_utc(const AssetData* d) {
    if (d->candle_count < 1) return 0.0f;
    time_t newest_ts = (time_t)d->candles[d->candle_count - 1].ts;
    struct tm newest_day;
    gmtime_r(&newest_ts, &newest_day);
    int today_yday = newest_day.tm_yday;
    int today_year = newest_day.tm_year;
    // Walk backward; keep going while bars still belong to today.
    // The last one we accept (smallest index still on today) is the day's first bar.
    int first_today = d->candle_count - 1;
    for (int i = d->candle_count - 2; i >= 0; i--) {
        struct tm bar_day;
        time_t bar_ts = (time_t)d->candles[i].ts;
        gmtime_r(&bar_ts, &bar_day);
        if (bar_day.tm_yday == today_yday && bar_day.tm_year == today_year) {
            first_today = i;  // still today — keep scanning earlier
        } else {
            break;  // crossed into the previous UTC day
        }
    }
    return d->candles[first_today].open;
}

// Fetch today's true 00:00-UTC opening price for a crypto ticker using a small
// 15-minute-bar request that covers only today's UTC calendar day.
// This is the exact same anchor that the 15m timeframe uses — so calling this
// for the 1D timeframe makes all four timeframes show identical % change.
// Returns 0.0f on any HTTP or parse failure (caller uses existing value instead).
static float fetch_crypto_today_open(const char* host, const char* ticker) {
    time_t now = time(nullptr);
    if (now < 1704067200L) return 0.0f;

    // period1 = midnight 00:00 UTC today; interval = 15m → returns only today's bars.
    time_t today_midnight = (now / 86400) * 86400;
    char url[256];
    snprintf(url, sizeof(url),
        "https://%s/v8/finance/chart/%s"
        "?interval=15m&period1=%ld&period2=%ld&includePrePost=false",
        host, ticker, (long)today_midnight, (long)now);
    Serial.printf("[YF] anchor GET %s\n", url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(10000);
    http.setTimeout(20000);
    if (!http.begin(client, url)) { return 0.0f; }
    http.addHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept",          "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Referer",         "https://finance.yahoo.com/");

    int code = http.GET();
    if (code != 200) { http.end(); return 0.0f; }

    String resp = http.getString();
    http.end();
    if (resp.length() < 20) return 0.0f;

    // Small filter — only need the open array; ignore everything else.
    StaticJsonDocument<128> filter;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["open"] = true;

    PsramJsonDocument doc(8192);
    if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) return 0.0f;
    resp = String();

    JsonArray opens = doc["chart"]["result"][0]["indicators"]["quote"][0]["open"];
    if (opens.isNull()) return 0.0f;

    // Return the first finite, non-zero open — that bar is the 00:00-UTC bar.
    for (JsonVariant v : opens) {
        float o = v | 0.0f;
        if (isfinite(o) && o > 0.0001f) {
            Serial.printf("[YF] anchor %s open=%.4f\n", ticker, (double)o);
            return o;
        }
    }
    return 0.0f;
}

static bool yf_try_host(const char* host, const char* ticker, const char* interval,
                        time_t period1, int tf, MarketType market, AssetData* out) {
    time_t period2 = time(nullptr);
    if (period2 < 1704067200L) period2 = 1704067200L;

    char url[256];
    snprintf(url, sizeof(url),
        "https://%s/v8/finance/chart/%s"
        "?interval=%s&period1=%ld&period2=%ld&includePrePost=false",
        host, ticker, interval, (long)period1, (long)period2);
    Serial.printf("[YF] GET %s\n", url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);           // no chunked transfer-encoding
    http.setConnectTimeout(10000);
    http.setTimeout(30000);         // 30 s to fully receive body

    if (!http.begin(client, url)) {
        strncpy(out->err, "YF: begin failed", sizeof(out->err));
        out->valid = false;
        return false;
    }
    http.addHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept",          "application/json");
    http.addHeader("Accept-Encoding", "identity");  // no gzip
    http.addHeader("Referer",         "https://finance.yahoo.com/");

    int code = http.GET();
    Serial.printf("[YF] %s HTTP %d\n", ticker, code);
    if (code != 200) {
        snprintf(out->err, sizeof(out->err), "YF HTTP %d", code);
        http.end();
        out->valid = false;
        return false;
    }

    // Read the complete body before parsing — eliminates all streaming edge cases
    // (IncompleteInput / EmptyInput from TLS fragmentation or connection close timing).
    String resp = http.getString();
    http.end();

    if (resp.length() < 20) {
        strncpy(out->err, "YF: empty body", sizeof(out->err));
        out->valid = false;
        return false;
    }
    Serial.printf("[YF] body %u bytes\n", (unsigned)resp.length());

    StaticJsonDocument<512> filter;
    filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
    filter["chart"]["result"][0]["meta"]["previousClose"]      = true;
    filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
    filter["chart"]["result"][0]["meta"]["marketState"]        = true;
    filter["chart"]["result"][0]["timestamp"]                  = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["open"]   = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["high"]   = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["low"]    = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["close"]  = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["volume"] = true;
    filter["chart"]["error"] = true;

    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 56 * 1024) {
        strncpy(out->err, "YF: low PSRAM", sizeof(out->err));
        out->valid = false;
        return false;
    }
    PsramJsonDocument doc(49152);  // 48 KB in PSRAM: worst case ~300 1h bars × 6 arrays × 16 B/slot ≈ 29 KB + overhead
    DeserializationError derr = deserializeJson(doc, resp,
                                    DeserializationOption::Filter(filter));
    resp = String();  // free String memory immediately after parsing

    if (derr) {
        snprintf(out->err, sizeof(out->err), "YF JSON: %s", derr.c_str());
        out->valid = false;
        return false;
    }

    if (!doc["chart"]["error"].isNull()) {
        const char* ec = doc["chart"]["error"]["code"]        | "?";
        const char* ed = doc["chart"]["error"]["description"] | "";
        Serial.printf("[YF] API error: %s — %s\n", ec, ed);
    }

    JsonArray results = doc["chart"]["result"];
    if (results.isNull() || results.size() == 0) {
        strncpy(out->err, "YF: no result", sizeof(out->err));
        out->valid = false;
        return false;
    }

    JsonObject result = results[0];
    JsonObject meta   = result["meta"];

    out->price = meta["regularMarketPrice"] | 0.0f;

    // Read the two "previous close" fields separately so we know which is present.
    // previousClose   = prior official session close (stocks/ETFs — exact daily reference).
    // chartPreviousClose = close of the bar before this chart's range window; for
    //   multi-month requests this is months ago, NOT yesterday → wrong for crypto.
    // change_pct is computed below, after candles are filled, so it can derive
    // a better previous-close from the candle series when previousClose is absent.
    JsonVariant meta_prev = meta["previousClose"];
    bool has_meta_prev    = !meta_prev.isNull();
    float meta_prev_val   = has_meta_prev ? meta_prev.as<float>() : 0.0f;
    float chart_prev_val  = meta["chartPreviousClose"] | 0.0f;
    // Store the best available meta value in prev_close for now; refined below.
    out->prev_close = has_meta_prev ? meta_prev_val : chart_prev_val;

    const char* ms = meta["marketState"] | "CLOSED";
    if      (strcmp(ms, "REGULAR") == 0) out->market_state = MSTATE_OPEN;
    else if (strcmp(ms, "PRE")     == 0) out->market_state = MSTATE_PRE;
    else if (strcmp(ms, "POST")    == 0) out->market_state = MSTATE_POST;
    else                                 out->market_state = MSTATE_CLOSED;

    JsonArray ts    = result["timestamp"];
    JsonObject q    = result["indicators"]["quote"][0];
    JsonArray opens = q["open"],  highs = q["high"],
              lows  = q["low"],   closes = q["close"],
              vols  = q["volume"];

    int raw = (int)ts.size();
    out->candle_count = 0;

    if (tf == TF_4H) {
        // Group 1-hour bars into 4-hour candles aligned to UTC clock boundaries.
        // Each bar is assigned to a "bucket" using integer division: bucket = ts / 14400.
        // Because 14400 seconds = 4 hours and the Unix epoch starts at 00:00 UTC,
        // bucket boundaries fall exactly at 00:00, 04:00, 08:00, 12:00, 16:00, 20:00 UTC —
        // the same boundaries Binance uses for its 4H candles on BTCUSDT.
        // This ensures the first 4H candle of today always opens at 00:00 UTC, so that
        // today_open_utc() finds the correct Binance-style daily-open anchor on the 4H chart.
        float grp_o = 0, grp_h = 0, grp_l = 0, grp_c = 0;
        long grp_vol = 0; int grp_cnt = 0;
        uint32_t cur_bucket = 0;  // UTC 4h bucket id for the group currently being built
        for (int i = 0; i < raw; i++) {
            float o = opens[i]  | 0.0f;
            float h = highs[i]  | 0.0f;
            float l = lows[i]   | 0.0f;
            float c = closes[i] | 0.0f;
            if (o < 0.0001f && c < 0.0001f) continue;
            if (!isfinite(o) || !isfinite(h) || !isfinite(l) || !isfinite(c)) continue;
            uint32_t bar_ts  = ts[i] | 0;
            uint32_t bucket  = bar_ts / 14400;  // which 4h UTC slot does this bar belong to?
            if (grp_cnt == 0) {
                // Start a new group with this bar.
                cur_bucket = bucket;
                grp_o = o; grp_h = h; grp_l = l; grp_c = c;
                grp_vol = (long)(vols[i] | 0.0f); grp_cnt = 1;
            } else if (bucket == cur_bucket) {
                // Same 4h bucket — fold this bar into the current group.
                if (h > grp_h) grp_h = h;
                if (l < grp_l) grp_l = l;
                grp_c = c; grp_vol += (long)(vols[i] | 0.0f); grp_cnt++;
            } else {
                // New bucket — emit the completed group, then start a fresh one.
                if (out->candle_count == MAX_CANDLES) {
                    memmove(out->candles, out->candles + 1, (MAX_CANDLES - 1) * sizeof(Candle));
                    out->candle_count--;
                }
                int ci = out->candle_count;
                out->candles[ci].open   = grp_o;
                out->candles[ci].high   = grp_h;
                out->candles[ci].low    = grp_l;
                out->candles[ci].close  = grp_c;
                out->candles[ci].ts     = cur_bucket * 14400;  // stamp at UTC bucket start
                out->candles[ci].volume = grp_vol;
                out->candle_count++;
                cur_bucket = bucket;
                grp_o = o; grp_h = h; grp_l = l; grp_c = c;
                grp_vol = (long)(vols[i] | 0.0f); grp_cnt = 1;
            }
        }
        // Emit any partial (still-forming) group at the end — this is the current 4H candle.
        // It must be included so today_open_utc() can find the 00:00 UTC bucket open price.
        if (grp_cnt > 0) {
            if (out->candle_count == MAX_CANDLES) {
                memmove(out->candles, out->candles + 1, (MAX_CANDLES - 1) * sizeof(Candle));
                out->candle_count--;
            }
            int ci = out->candle_count;
            out->candles[ci].open   = grp_o;
            out->candles[ci].high   = grp_h;
            out->candles[ci].low    = grp_l;
            out->candles[ci].close  = grp_c;
            out->candles[ci].ts     = cur_bucket * 14400;
            out->candles[ci].volume = grp_vol;
            out->candle_count++;
        }
    } else {
        int start = (raw > MAX_CANDLES) ? raw - MAX_CANDLES : 0;
        for (int i = start; i < raw && out->candle_count < MAX_CANDLES; i++) {
            float o = opens[i]  | 0.0f;
            float h = highs[i]  | 0.0f;
            float l = lows[i]   | 0.0f;
            float c = closes[i] | 0.0f;
            if (o < 0.0001f && c < 0.0001f) continue;
            if (!isfinite(o) || !isfinite(h) || !isfinite(l) || !isfinite(c)) continue;
            int ci = out->candle_count;
            out->candles[ci].open   = o;
            out->candles[ci].high   = h;
            out->candles[ci].low    = l;
            out->candles[ci].close  = c;
            out->candles[ci].ts     = ts[i] | 0;
            out->candles[ci].volume = (long)(vols[i] | 0.0f);
            out->candle_count++;
        }
    }

    // Compute the daily % change now that the candle array is filled.
    // The reference price differs by asset type:
    //   Crypto: Binance-style fixed-point daily change — "since 00:00 UTC today."
    //           Use today_open_utc() which finds the open of the first bar of the
    //           current UTC day (matches Binance's 1D candle "change since open").
    //           All crypto timeframes include today's 00:00 UTC boundary, so this
    //           gives one consistent daily % on 15m / 1h / 4h / 1D alike.
    //           Fallback to chartPreviousClose if candles are unavailable.
    //   Stocks / other: Yahoo-style prior-session reference — use meta.previousClose
    //           (exact official close, provided by Yahoo for stocks/ETFs), else
    //           prev_day_close() from candles, else chartPreviousClose.
    float prev_close_for_pct;
    if (market == MARKET_CRYPTO) {
        float day_open = today_open_utc(out);        // Binance fixed-point anchor
        prev_close_for_pct = (day_open > 0.001f) ? day_open : chart_prev_val;
    } else if (has_meta_prev && meta_prev_val > 0.001f) {
        prev_close_for_pct = meta_prev_val;          // stocks: Yahoo exact prior close
    } else {
        float derived = prev_day_close(out);         // stocks fallback from candles
        prev_close_for_pct = (derived > 0.001f) ? derived : chart_prev_val;
    }
    out->prev_close = prev_close_for_pct;
    out->change_pct = (prev_close_for_pct > 0.001f)
                    ? (out->price - prev_close_for_pct) / prev_close_for_pct * 100.0f
                    : 0.0f;

    Serial.printf("[YF] %s OK price=%.4f prev=%.4f chg=%.2f%% candles=%d state=%s\n",
                  ticker, (double)out->price, (double)prev_close_for_pct,
                  (double)out->change_pct, out->candle_count, ms);

    out->last_fetch = (uint32_t)(millis() / 1000);
    out->valid      = (out->candle_count > 0);
    if (!out->valid) strncpy(out->err, "YF: 0 candles", sizeof(out->err));
    return out->valid;
}

static bool api_fetch_yahoo(const Settings* s, const AssetDef* def, AssetData* out) {
    char interval[8];
    get_yf_interval(s->timeframe, interval, sizeof(interval));
    time_t p1 = yf_period1(s->timeframe, def->market, (bool)def->continuous);

    strncpy(out->symbol, def->symbol, sizeof(out->symbol) - 1);

    bool ok = yf_try_host(YF_HOST_PRIMARY, def->yahoo, interval, p1, s->timeframe, def->market, out);

    if (!ok) {
        // Retry on any failure — query2 may be on a different server/IP so connection
        // errors (HTTP -1) are worth retrying, as are parse/data errors.
        if (strncmp(out->err, "YF HTTP",        7) == 0 ||
            strncmp(out->err, "YF: no result", 13) == 0 ||
            strncmp(out->err, "YF JSON:",       8) == 0 ||
            strncmp(out->err, "YF: empty",      9) == 0 ||
            strncmp(out->err, "YF: 0 candles", 13) == 0) {
            Serial.printf("[YF] %s: primary failed (%s), trying fallback\n",
                          def->yahoo, out->err);
            ok = yf_try_host(YF_HOST_FALLBACK, def->yahoo, interval, p1, s->timeframe, def->market, out);
        }
    }

    if (!ok) return false;

    // For crypto on the 1D timeframe: replace the % anchor with the true 00:00-UTC
    // open fetched via a small 15m request (cached once per UTC calendar day).
    // This makes the 1D % identical to the 15m/1h/4h % — all anchored to Binance's
    // fixed daily-open point. The main 1D fetch only has Yahoo daily candles (stamped
    // 12:00 UTC, slightly different open), so today_open_utc() alone isn't exact here.
    if (def->market == MARKET_CRYPTO && s->timeframe == TF_1D) {
        uint32_t today = (uint32_t)(time(nullptr) / 86400);
        float ref = cache_lookup(def->yahoo, today);
        if (ref <= 0.001f) {
            // Not cached yet for today — fetch the anchor from the primary host.
            ref = fetch_crypto_today_open(YF_HOST_PRIMARY, def->yahoo);
            if (ref <= 0.001f) ref = fetch_crypto_today_open(YF_HOST_FALLBACK, def->yahoo);
            if (ref > 0.001f) cache_store(def->yahoo, today, ref);
        }
        if (ref > 0.001f) {
            // Override the change_pct computed inside yf_try_host with the exact anchor.
            out->prev_close = ref;
            out->change_pct = (out->price - ref) / ref * 100.0f;
            Serial.printf("[YF] %s 1D anchor override prev=%.4f chg=%.2f%%\n",
                          def->yahoo, (double)ref, (double)out->change_pct);
        }
        // If fetch failed, leave the existing values as-is (graceful fallback).
    }

    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════
bool api_fetch(const Settings* s, int idx, AssetData* out) {
    if (idx < 0 || idx >= s->asset_count) return false;

    const AssetDef* def = asset_find(s->assets[idx]);
    if (!def) {
        snprintf(out->err, sizeof(out->err), "Unknown: %s", s->assets[idx]);
        out->valid = false;
        return false;
    }

    return api_fetch_yahoo(s, def, out);
}

unsigned long api_refresh_interval(int timeframe) {
    switch (timeframe) {
        case TF_15M: return  30UL * 1000;
        case TF_1H:  return  30UL * 1000;
        case TF_4H:  return  30UL * 1000;
        default:     return 600UL * 1000;
    }
}

bool asset_always_open(const AssetDef* a) {
    return a && a->market == MARKET_CRYPTO;
}
