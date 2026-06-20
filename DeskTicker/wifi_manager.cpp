#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <lvgl.h>
#include "wifi_manager.h"
#include "settings.h"
#include "assets.h"
#include "esp_bsp.h"
#include "tz_options.h"

// ── Captive portal objects ────────────────────────────────────────────────────
static WebServer server(80);
static DNSServer dns;
static bool      setup_done   = false;
static Settings* g_settings   = nullptr;

// ── Setup-mode LVGL screen ────────────────────────────────────────────────────
static lv_obj_t* setup_scr    = nullptr;
static lv_obj_t* status_lbl   = nullptr;

static void build_setup_screen() {
    setup_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(setup_scr, lv_color_hex(0x0E1117), LV_PART_MAIN);

    // "DeskTicker Setup" — D in bull-green, T in bear-red, rest pearl.
    lv_obj_t* title = lv_spangroup_create(setup_scr);
    lv_obj_set_style_bg_opa(title, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(title, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(title, 0, LV_PART_MAIN);
    lv_obj_set_width(title,  LV_SIZE_CONTENT);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    {
        const lv_font_t* f = &lv_font_montserrat_28;
        lv_span_t* sp;
        sp = lv_spangroup_new_span(title); lv_span_set_text_static(sp, "D");
        lv_style_set_text_color(&sp->style, lv_color_hex(0x22C55E));
        lv_style_set_text_font(&sp->style, f);
        sp = lv_spangroup_new_span(title); lv_span_set_text_static(sp, "esk");
        lv_style_set_text_color(&sp->style, lv_color_hex(0xE6E9EF));
        lv_style_set_text_font(&sp->style, f);
        sp = lv_spangroup_new_span(title); lv_span_set_text_static(sp, "T");
        lv_style_set_text_color(&sp->style, lv_color_hex(0xEF4444));
        lv_style_set_text_font(&sp->style, f);
        sp = lv_spangroup_new_span(title); lv_span_set_text_static(sp, "icker Setup");
        lv_style_set_text_color(&sp->style, lv_color_hex(0xE6E9EF));
        lv_style_set_text_font(&sp->style, f);
        lv_spangroup_refr_mode(title);  // compute size before alignment
    }
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // QR code encodes http://192.168.4.1 — user can scan instead of typing
    lv_obj_t* qr = lv_qrcode_create(setup_scr, 120,
                                     lv_color_hex(0x0E1117),   // dark modules (Deep Space)
                                     lv_color_hex(0xE6E9EF));  // light bg (Pearl)
    lv_qrcode_update(qr, "http://192.168.4.1", 18);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 68);

    lv_obj_t* inst1 = lv_label_create(setup_scr);
    lv_obj_set_style_text_font(inst1, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(inst1, lv_color_hex(0xCDD9E5), LV_PART_MAIN);
    lv_label_set_text(inst1, "1. Connect to WiFi:  DeskTicker-Setup");
    lv_obj_align(inst1, LV_ALIGN_CENTER, 0, 50);

    lv_obj_t* inst2 = lv_label_create(setup_scr);
    lv_obj_set_style_text_font(inst2, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(inst2, lv_color_hex(0xCDD9E5), LV_PART_MAIN);
    lv_label_set_text(inst2, "2. Scan QR  -or-  open 192.168.4.1");
    lv_obj_align(inst2, LV_ALIGN_CENTER, 0, 75);

    status_lbl = lv_label_create(setup_scr);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x22C55E), LV_PART_MAIN);
    lv_label_set_text(status_lbl, "Waiting for connection...");
    lv_obj_align(status_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_scr_load(setup_scr);
}

static void set_status(const char* msg) {
    if (!status_lbl) return;
    if (bsp_display_lock(0)) {
        lv_label_set_text(status_lbl, msg);
        bsp_display_unlock();
    }
}

// ── HTML generation ───────────────────────────────────────────────────────────
static String build_html(Settings* s) {
    int n = WiFi.scanNetworks();
    bool is_manual = (s->cycle_secs == 0);
    int  show_cs   = is_manual ? 30 : s->cycle_secs;

    // All inputs live inside one <form> so no JavaScript is needed to submit.
    // JS is used only for UX enhancements (max-6 validation, show/hide).
    String html = F(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>DeskTicker Setup</title>"
        "<style>"
        "body{font-family:sans-serif;background:#0d1117;color:#cdd9e5;margin:0;padding:16px}"
        "h1{color:#22c55e;text-align:center;margin-bottom:4px}"
        "h2{color:#22c55e;border-bottom:1px solid #21262d;padding-bottom:6px;font-size:1rem}"
        ".card{background:#161b22;border:1px solid #21262d;border-radius:8px;padding:14px;margin-bottom:14px}"
        "input[type=text],input[type=password],select{"
        "background:#0d1117;color:#cdd9e5;border:1px solid #30363d;"
        "border-radius:6px;padding:8px;width:100%;box-sizing:border-box;margin-top:4px}"
        ".asset-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px}"
        ".asset-item{background:#21262d;border-radius:6px;padding:6px;cursor:pointer}"
        ".asset-item input{width:auto;margin:0 6px 0 0}"
        ".tf-row,.anim-row,.mode-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:6px}"
        ".tf-btn,.anim-btn,.mode-btn{flex:1;min-width:60px;padding:8px;text-align:center;"
        "background:#21262d;border:2px solid #30363d;border-radius:6px;cursor:pointer;color:#cdd9e5}"
        "input[type=radio]:checked+label.anim-btn,"
        "input[type=radio]:checked+label.mode-btn{border-color:#22c55e;background:#0a2a10}"
        "input.tf-cb:checked+label.tf-btn{border-color:#22c55e;background:#0a2a10}"
        "input[type=radio],input.tf-cb{display:none}"
        ".theme-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:6px}"
        ".theme-opt{flex:1;min-width:80px;padding:10px;text-align:center;"
        "background:#21262d;border:2px solid #30363d;border-radius:6px;cursor:pointer}"
        "input[name=theme]:checked+label.theme-opt{border-color:#22c55e;background:#0a2a10}"
        ".color-row{display:flex;gap:12px;margin-top:8px;align-items:center}"
        ".btn{background:#22c55e;color:#0e1117;border:none;border-radius:8px;"
        "padding:14px;font-size:1.1rem;width:100%;cursor:pointer;margin-top:8px}"
        ".btn:active{background:#16a34a}"
        "#custom_colors{display:none}"
        ".note{color:#8b949e;font-size:.8rem}"
        ".asset-group-label{color:#8b949e;font-size:.75rem;text-transform:uppercase;"
        "letter-spacing:.06em;margin:10px 0 4px}"
        "</style>"
        "<script>"
        "function checkMax(cb){"
        "var b=document.querySelectorAll('.asset-cb:checked');"
        "if(b.length>6){cb.checked=false;alert('Max 6 assets!');}}"
        "function checkTF(cb){"
        "var b=document.querySelectorAll('.tf-cb:checked');"
        "if(b.length==0){cb.checked=true;alert('Select at least one timeframe!');}}"
        "function toggleCustom(v){"
        "document.getElementById('custom_colors').style.display=(v==3)?'block':'none';}"
        "function updateCycle(){"
        "var m=document.getElementById('cm_m').checked;"
        "document.getElementById('cslider').style.display=m?'none':'block';}"
        "</script>"
        "</head><body>"
        "<h1>DeskTicker Setup</h1>"
        "<form method='POST' action='/save'>"
    );

    // WiFi
    html += F("<div class='card'><h2>WiFi Network</h2>"
              "<label>Network</label><select name='ssid'>");
    for (int i = 0; i < n; i++) {
        html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) +
                " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    html += F("</select>"
              "<label style='margin-top:10px;display:block'>Password</label>"
              "<input type='password' name='pass' placeholder='Leave blank if open'>"
              "</div>");

    // Assets — rendered in four labelled groups so the user can find them easily
    struct { MarketType market; const char* label; } groups[] = {
        { MARKET_CRYPTO,    "Crypto"        },
        { MARKET_STOCK,     "Stocks & ETFs" },
        { MARKET_COMMODITY, "Commodities"   },
        { MARKET_FOREX,     "Forex"         },
    };
    html += F("<div class='card'><h2>Assets <span class='note'>(pick up to 6)</span></h2>");
    for (int g = 0; g < 4; g++) {
        html += "<p class='asset-group-label'>";
        html += groups[g].label;
        html += "</p><div class='asset-grid'>";
        for (int i = 0; i < TOTAL_ASSETS; i++) {
            if (ASSETS[i].market != groups[g].market) continue;
            bool checked = false;
            for (int j = 0; j < s->asset_count; j++) {
                if (strcmp(s->assets[j], ASSETS[i].symbol) == 0) { checked = true; break; }
            }
            html += "<label class='asset-item'><input class='asset-cb' type='checkbox' name='asset' value='";
            html += ASSETS[i].symbol;
            html += "'";
            if (checked) html += " checked";
            html += " onchange='checkMax(this)'> ";
            html += ASSETS[i].symbol;
            html += "<br><span class='note'>";
            html += ASSETS[i].name;
            html += "</span></label>";
        }
        html += F("</div>");
    }
    // Custom ticker — a free-text Yahoo symbol. The setup AP has no internet, so it
    // can't be validated here; the device checks/classifies it on first connect.
    html += F("<p class='asset-group-label'>Custom</p>"
              "<input type='text' name='custom' maxlength='9' "
              "placeholder='Add any Yahoo symbol, e.g. PLTR' "
              "oninput='this.value=this.value.toUpperCase()'>"
              "<p class='note'>Checked automatically the first time it loads.</p>");
    html += F("</div>");

    // Timeframe — checkboxes, at least one required, swipe up/down to cycle on device
    const char* tfs[]      = {"15","60","240","1440"};
    const int   tf_vals[]  = {TF_15M, TF_1H, TF_4H, TF_1D};
    const char* tf_names[] = {"15m","1h","4h","1D"};
    html += F("<div class='card'><h2>Timeframe <span class='note'>(pick 1 or more; swipe up/down to cycle)</span></h2><div class='tf-row'>");
    for (int i = 0; i < 4; i++) {
        bool checked = false;
        for (int j = 0; j < s->timeframe_count; j++) {
            if (s->timeframes[j] == tf_vals[i]) { checked = true; break; }
        }
        html += "<input class='tf-cb' type='checkbox' name='tf' id='tf";
        html += tfs[i]; html += "' value='"; html += tfs[i];
        html += "'"; if (checked) html += " checked";
        html += " onchange='checkTF(this)'><label class='tf-btn' for='tf";
        html += tfs[i]; html += "'>"; html += tf_names[i]; html += "</label>";
    }
    html += F("</div></div>");

    // Asset cycling mode
    html += F("<div class='card'><h2>Asset Cycling</h2><div class='mode-row'>"
              "<input type='radio' name='auto_cycle' id='cm_a' value='1'");
    if (!is_manual) html += F(" checked");
    html += F(" onchange='updateCycle()'>"
              "<label class='mode-btn' for='cm_a'>Auto</label>"
              "<input type='radio' name='auto_cycle' id='cm_m' value='0'");
    if (is_manual) html += F(" checked");
    html += F(" onchange='updateCycle()'>"
              "<label class='mode-btn' for='cm_m'>Manual (swipe)</label>"
              "</div>"
              "<div id='cslider' style='margin-top:10px");
    if (is_manual) html += F(";display:none");
    html += F("'><label>Switch every: <b id='cv'>");
    html += String(show_cs);
    html += F("s</b></label>"
              "<input type='range' name='cycle' min='5' max='120' step='5' value='");
    html += String(show_cs);
    html += F("' oninput=\"document.getElementById('cv').textContent=this.value+'s'\">"
              "</div></div>");

    // Timezone — options come from the shared TZ_OPTS table in tz_options.h
    html += F("<div class='card'><h2>Timezone</h2>"
              "<select name='tz'>");
    for (int i = 0; i < TZ_OPTS_N; i++) {
        html += "<option value='"; html += String(TZ_OPTS[i].minutes); html += "'";
        if (s->tz_offset == TZ_OPTS[i].minutes) html += " selected";
        html += ">"; html += TZ_OPTS[i].label; html += "</option>";
    }
    html += F("</select></div>");

    // Color theme
    const char* themes[]      = {"0","1","2","3"};
    const char* theme_names[] = {"Classic","Color Shift","Neon Pulse","Custom"};
    html += F("<div class='card'><h2>Candle Colour</h2><div class='theme-row'>");
    for (int i = 0; i < 4; i++) {
        bool sel = (s->theme == i);
        html += "<input type='radio' name='theme' id='th"; html += themes[i];
        html += "' value='"; html += themes[i];
        html += "' onchange='toggleCustom("; html += themes[i]; html += ")'";
        if (sel) html += " checked";
        html += "><label class='theme-opt' for='th"; html += themes[i];
        html += "'>"; html += theme_names[i]; html += "</label>";
    }
    html += F("</div>");
    char bull_hex[8], bear_hex[8];
    snprintf(bull_hex, 8, "#%06x", (unsigned)s->bull_rgb);
    snprintf(bear_hex, 8, "#%06x", (unsigned)s->bear_rgb);
    html += "<div id='custom_colors'";
    if (s->theme != THEME_CUSTOM) html += " style='display:none'";
    html += F("><div class='color-row'>"
              "<div><label>Bull candle</label>"
              "<input type='color' name='bull' value='");
    html += bull_hex;
    html += F("'></div><div><label>Bear candle</label>"
              "<input type='color' name='bear' value='");
    html += bear_hex;
    html += F("'></div></div></div></div>");

    // After-hours animation
    const char* anims[]      = {"0","1","2","3","4","5"};
    const char* anim_names[] = {"Tidepool","Coral Reef","Starfield","Countdown",
                                 "Pixel Beach","Grassland"};
    html += F("<div class='card'><h2>After-Hours Animation</h2><div class='anim-row'>");
    for (int i = 0; i < 6; i++) {
        bool sel = (s->after_anim == i);
        html += "<input type='radio' name='anim' id='an"; html += anims[i];
        html += "' value='"; html += anims[i];
        if (sel) html += "' checked"; else html += "'";
        html += "><label class='anim-btn' for='an"; html += anims[i];
        html += "'>"; html += anim_names[i]; html += "</label>";
    }
    html += F("</div></div>");

    html += F("<button class='btn' type='submit'>Save &amp; Start</button>"
              "</form></body></html>");
    return html;
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static void handle_root() {
    set_status("Device connected!");
    String html = build_html(g_settings);
    server.send(200, "text/html", html);
}

static void handle_save() {
    Settings* s = g_settings;

    // WiFi
    if (server.hasArg("ssid")) {
        String v = server.arg("ssid");
        strncpy(s->wifi_ssid, v.c_str(), sizeof(s->wifi_ssid) - 1);
    }
    if (server.hasArg("pass")) {
        String v = server.arg("pass");
        strncpy(s->wifi_pass, v.c_str(), sizeof(s->wifi_pass) - 1);
    }

    // Assets
    s->asset_count = 0;
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) == "asset" && s->asset_count < MAX_ASSETS) {
            strncpy(s->assets[s->asset_count], server.arg(i).c_str(), ASSET_SYM_LEN - 1);
            s->asset_count++;
        }
    }
    if (s->asset_count == 0) {
        strncpy(s->assets[0], "SPY", ASSET_SYM_LEN - 1);
        s->asset_count = 1;
    }

    // Custom ticker (optional). No internet in setup AP mode, so we can't validate
    // against Yahoo here — store it provisionally as a generic 24/7 stock so the
    // fetch window stays small, and let the device auto-classify it on first connect.
    if (server.hasArg("custom")) {
        char sym[ASSET_SYM_LEN];
        strncpy(sym, server.arg("custom").c_str(), sizeof(sym) - 1);
        sym[sizeof(sym) - 1] = '\0';
        symbol_normalize(sym);
        if (sym[0] && !symbol_is_builtin(sym) &&
            custom_index(sym) < 0 && !custom_is_full()) {
            AssetDef d;
            memset(&d, 0, sizeof(d));
            strncpy(d.symbol, sym, sizeof(d.symbol) - 1);
            strncpy(d.yahoo,  sym, sizeof(d.yahoo)  - 1);
            strncpy(d.name,   sym, sizeof(d.name)   - 1);
            d.market     = MARKET_STOCK;
            d.decimals   = 2;
            d.continuous = 1;   // 24/7-sized fetch window until classified
            if (custom_add(&d, true)) {
                custom_save_to_nvs();
                if (s->asset_count < MAX_ASSETS) {
                    strncpy(s->assets[s->asset_count], sym, ASSET_SYM_LEN - 1);
                    s->assets[s->asset_count][ASSET_SYM_LEN - 1] = '\0';
                    s->asset_count++;
                }
            }
        }
    }

    // Timeframe — collect all checked values (checkboxes)
    s->timeframe_count = 0;
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) == "tf" && s->timeframe_count < 4) {
            int v = server.arg(i).toInt();
            if (v == TF_15M || v == TF_1H || v == TF_4H || v == TF_1D) {
                s->timeframes[s->timeframe_count++] = v;
            }
        }
    }
    if (s->timeframe_count == 0) {
        s->timeframes[0] = TF_1H;
        s->timeframe_count = 1;
    }
    // Sort ascending so swipe cycles shortest→longest
    for (int i = 0; i < s->timeframe_count - 1; i++)
        for (int j = i + 1; j < s->timeframe_count; j++)
            if (s->timeframes[j] < s->timeframes[i]) {
                int tmp = s->timeframes[i]; s->timeframes[i] = s->timeframes[j]; s->timeframes[j] = tmp;
            }
    s->timeframe = s->timeframes[0];  // start on shortest selected TF

    // Timezone
    if (server.hasArg("tz")) s->tz_offset = server.arg("tz").toInt();
    // Theme
    if (server.hasArg("theme")) s->theme = server.arg("theme").toInt();
    // Colors
    if (server.hasArg("bull")) {
        String hex = server.arg("bull");
        if (hex.startsWith("#")) hex = hex.substring(1);
        s->bull_rgb = strtoul(hex.c_str(), nullptr, 16);
    }
    if (server.hasArg("bear")) {
        String hex = server.arg("bear");
        if (hex.startsWith("#")) hex = hex.substring(1);
        s->bear_rgb = strtoul(hex.c_str(), nullptr, 16);
    }
    // Cycle: auto_cycle=0 → manual swipe mode (sentinel: cycle_secs=0)
    if (server.hasArg("auto_cycle") && server.arg("auto_cycle") == "0") {
        s->cycle_secs = 0;
    } else if (server.hasArg("cycle")) {
        int cs = server.arg("cycle").toInt();
        s->cycle_secs = (cs >= 5 && cs <= 120) ? cs : 30;
    }
    // Animation
    if (server.hasArg("anim")) s->after_anim = server.arg("anim").toInt();

    s->wifi_ok = true;
    settings_save(s);

    server.send(200, "text/html",
        "<!DOCTYPE html><html><body style='background:#0e1117;color:#22c55e;"
        "font-family:sans-serif;text-align:center;padding:40px'>"
        "<h1>&#10003; Saved!</h1><p>Restarting DeskTicker...</p></body></html>");

    delay(1500);
    setup_done = true;
}

static void handle_redirect() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}

// ── Public API ────────────────────────────────────────────────────────────────
bool wifi_setup_run(Settings* s) {
    g_settings = s;
    setup_done = false;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("DeskTicker-Setup");
    // Note: scanNetworks() is called inside build_html() when the page is requested.

    dns.start(53, "*", IPAddress(192, 168, 4, 1));

    server.on("/",       HTTP_GET,  handle_root);
    server.on("/save",   HTTP_POST, handle_save);
    server.onNotFound(handle_redirect);
    server.begin();

    // Show setup screen on display
    if (bsp_display_lock(0)) {
        build_setup_screen();
        bsp_display_unlock();
    }

    while (!setup_done) {
        dns.processNextRequest();
        server.handleClient();
        delay(2);
    }

    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);

    if (bsp_display_lock(0)) {
        if (setup_scr) lv_obj_del(setup_scr);
        setup_scr  = nullptr;
        status_lbl = nullptr;
        bsp_display_unlock();
    }

    return true;
}

bool wifi_connect(const char* ssid, const char* pass, int timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((int)(millis() - t) > timeout_ms) return false;
        delay(250);
    }
    return true;
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}
