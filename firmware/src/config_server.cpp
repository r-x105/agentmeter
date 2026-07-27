#include "config_server.h"
#include "poller.h"
#include "providers/provider.h"
#include "providers/secrets.h"
#include "ui.h"
#include "captive_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_random.h>

// Shares port 80 with the captive portal's WebServer, but never concurrently:
// the portal runs in AP mode (no STA link), this server only once STA-connected.

static WebServer s_server(80);
static bool      s_running = false;

// ---- Rotating device code (see config_server.h) ----
// Alphabet drops the lookalikes 0/O, 1/I/L so the code is easy to copy from
// the panel. 31^8 ≈ 8·10^11 — rotation makes brute force moot.
static const char CODE_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
#define CODE_ROTATE_MS 600000UL   // 10 min
static char     s_code[9] = "--------";   // placeholder until the server starts
static uint32_t s_code_ts = 0;

static void rotate_code(void) {
    for (int i = 0; i < 8; i++)
        s_code[i] = CODE_ALPHABET[esp_random() % (sizeof(CODE_ALPHABET) - 1)];
    s_code[8] = '\0';
    s_code_ts = millis();
    Serial.println("config: device code rotated");
    ui_update_wifi_creds(captive_portal_is_active());   // refresh the Code row
}

const char* config_server_code(void)      { return s_code; }
void        config_server_rotate_code(void) { rotate_code(); }

// ---- Sessions ----------------------------------------------------------
// Entering the device code once unlocks the page for a while, rather than
// re-typing it per save. Sessions are deliberately independent of the code's
// rotation: a save rotates the code (so a shoulder-surfed code is spent) but
// does not sign you out.
#define SESSION_COUNT   4            // a couple of browsers/phones at once
#define SESSION_TTL_MS  1800000UL    // 30 min, sliding on each authed request
#define AUTH_MAX_FAILS  5
#define AUTH_LOCK_MS    60000UL      // brute-force backoff after MAX_FAILS

struct Session {
    char     token[33];   // 128 bits, hex
    uint32_t last_seen;
    bool     used;
};
static Session  s_sessions[SESSION_COUNT];
static uint8_t  s_auth_fails = 0;
static uint32_t s_lock_until = 0;

// Compare without leaking the match length through timing.
static bool ct_equal(const char* a, const char* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void mint_token(char* out33) {
    for (int i = 0; i < 4; i++)
        snprintf(out33 + i * 8, 9, "%08lx", (unsigned long)esp_random());
    out33[32] = '\0';
}

static const char* session_create(void) {
    int slot = 0;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (!s_sessions[i].used) { slot = i; break; }
        if (s_sessions[i].last_seen < oldest) { oldest = s_sessions[i].last_seen; slot = i; }
    }
    mint_token(s_sessions[slot].token);
    s_sessions[slot].last_seen = millis();
    s_sessions[slot].used = true;
    return s_sessions[slot].token;
}

static void session_drop(const char* token) {
    for (int i = 0; i < SESSION_COUNT; i++)
        if (s_sessions[i].used && ct_equal(s_sessions[i].token, token, 32))
            s_sessions[i].used = false;
}

// Pull am_session out of the Cookie header. Returns "" when absent.
static String cookie_token(void) {
    String c = s_server.header("Cookie");
    int at = c.indexOf("am_session=");
    if (at < 0) return String("");
    at += 11;
    int end = c.indexOf(';', at);
    return end < 0 ? c.substring(at) : c.substring(at, end);
}

// True when the request carries a live session; refreshes its expiry.
static bool request_authed(void) {
    String t = cookie_token();
    if (t.length() != 32) return false;
    uint32_t now = millis();
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (!s_sessions[i].used) continue;
        if (now - s_sessions[i].last_seen > SESSION_TTL_MS) { s_sessions[i].used = false; continue; }
        if (ct_equal(s_sessions[i].token, t.c_str(), 32)) {
            s_sessions[i].last_seen = now;
            return true;
        }
    }
    return false;
}

// ---- Page chrome -------------------------------------------------------
// Sent with sendContent_P so the markup streams straight from flash. Building
// the whole page as one String would put several KB on the internal heap, and
// internal heap is the scarcest thing on this device (see docs/hardware-notes.md).

static const char CSS[] = R"css(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>Agentmeter</title>
<style>
/* The page is already dark. Without this, browsers with auto-dark-mode on
   (Chrome's "Auto Dark Mode for Web Contents") darken it a second time and
   mute the accent — the button fill measured rgb(142,59,32) instead of
   #d97757 before this was declared. */
:root{color-scheme:dark}
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
 max-width:560px;margin:0 auto;padding:32px 20px 64px;background:#0a0a0a;
 color:#faf9f5;-webkit-font-smoothing:antialiased}
header{display:flex;align-items:baseline;gap:12px;margin-bottom:4px}
h1{font-size:1.5rem;margin:0;letter-spacing:-.01em}
.sub{color:#8a8880;font-size:.875rem;margin:0 0 28px}
.pill{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;padding:3px 9px;
 border-radius:99px;background:#1f1f1e;color:#b0aea5;white-space:nowrap}
.pill.ok{background:#1e2a17;color:#9cb37a}
.pill.warn{background:#2e2318;color:#d99a57}
.pill.err{background:#2e1a17;color:#e07a63}
.card{background:#141413;border:1px solid #1f1f1e;border-radius:12px;
 padding:18px 18px 20px;margin-bottom:14px}
.card h2{font-size:1rem;margin:0;display:flex;align-items:center;gap:9px}
.dot{width:10px;height:10px;border-radius:50%;flex:none}
.chead{display:flex;align-items:center;justify-content:space-between;gap:12px;
 margin-bottom:14px}
/* Deliberately NOT uppercased: credential labels carry case-sensitive text
   like ~/.codex/auth.json and token prefixes, and transforming them
   misrepresents what the user is supposed to paste. */
label{display:block;margin-top:14px;font-size:.78rem;color:#8a8880;
 margin-bottom:6px}
input[type=text],input[type=password]{width:100%;padding:11px 13px;
 border:1px solid #2a2a28;border-radius:8px;background:#0a0a0a;color:#faf9f5;
 font-size:.95rem;font-family:inherit}
input:focus{outline:none;border-color:#d97757}
input::placeholder{color:#55534d}
.opts{display:flex;flex-wrap:wrap;gap:18px;margin-top:18px;padding-top:16px;
 border-top:1px solid #1f1f1e}
.opt{display:flex;align-items:center;gap:7px;font-size:.85rem;color:#b0aea5;margin:0}
.opt input{accent-color:#d97757;width:16px;height:16px;margin:0}
button{width:100%;padding:14px;background:#d97757;border:none;border-radius:8px;
 color:#fff;font-size:1rem;font-weight:600;cursor:pointer;font-family:inherit}
button:hover{background:#c26647}
.note{margin-top:22px;font-size:.78rem;color:#55534d;line-height:1.6}
a{color:#d97757}
.lock{margin-top:48px;text-align:center}
.lock .card{padding:32px 24px}
#code{text-align:center;font-size:1.6rem;letter-spacing:.34em;padding:16px 10px;
 text-transform:uppercase;font-weight:600}
.err-box{background:#2e1a17;border:1px solid #46231c;color:#e07a63;padding:11px 14px;
 border-radius:8px;font-size:.85rem;margin-bottom:18px}
.ok-box{background:#1e2a17;border:1px solid #2c3d20;color:#9cb37a;padding:11px 14px;
 border-radius:8px;font-size:.85rem;margin-bottom:18px}
.bar{height:6px;background:#1f1f1e;border-radius:3px;overflow:hidden}
.bar i{display:block;height:100%;border-radius:3px}
.metric{margin-bottom:12px}
.mrow{display:flex;justify-content:space-between;gap:12px;font-size:.78rem;
 color:#8a8880;margin-bottom:6px}
.mrow span:first-child{color:#b0aea5}
.foot{display:flex;justify-content:space-between;align-items:center;margin-top:26px;
 font-size:.8rem;color:#55534d}
.foot form{margin:0}
.foot button{width:auto;padding:7px 14px;background:none;border:1px solid #2a2a28;
 color:#b0aea5;font-size:.8rem;font-weight:400}
.foot button:hover{background:#1f1f1e}
</style></head><body>
)css";

static const char LOCK_FORM[] = R"html(
<div class="lock">
<header style="justify-content:center"><h1>Agentmeter</h1></header>
<p class="sub">Enter the device code to make changes</p>
<div class="card">
<form method="POST" action="/auth">
<input id="code" name="code" type="text" autocomplete="off" autofocus required
 maxlength="8" inputmode="latin" spellcheck="false" placeholder="XXXXXXXX">
<label style="margin-top:16px;text-align:left">
Shown on the device's Wi-Fi screen. Rotates every 10 minutes.</label>
<button type="submit">Unlock</button>
</form>
</div>
<p class="note">Anyone on this network can view the device's status, but only
someone who can see the panel can change its settings.</p>
</div>
<script>
var c=document.getElementById('code');
c.addEventListener('input',function(){this.value=this.value.toUpperCase()});
</script>
</body></html>
)html";

static const char FORM_HEAD[] = R"html(
<header><h1>Agentmeter</h1><span class="pill ok">Unlocked</span></header>
<p class="sub">Changes apply immediately &#8212; no reboot.</p>
<form method="POST" action="/save">
)html";

static const char FORM_TAIL[] = R"html(
<button type="submit">Save changes</button>
</form>
<div class="foot">
<span>Secret fields are write-only &#8212; leave blank to keep the stored value.</span>
<form method="POST" action="/logout"><button type="submit">Lock</button></form>
</div>
</body></html>
)html";

// "sk-ant-o&#8230; (set)" — enough to recognize a credential, never the value.
static String masked_hint(const char* id, const char* sfx) {
    String v = secrets_get(id, sfx);
    if (!v.length()) return String("not set");
    String head = v.substring(0, 8);
    head.replace("\"", "");   // defang for the attribute context
    head.replace("&", "");
    head.replace("<", "");
    return head + "&#8230; (set)";
}

// "resets in 4h 42m" — empty when the metric has no reset window.
static void fmt_reset(int mins, char* buf, size_t n) {
    if (mins < 0)         { buf[0] = '\0'; return; }
    if (mins < 60)          snprintf(buf, n, "resets in %dm", mins);
    else if (mins < 1440)   snprintf(buf, n, "resets in %dh %dm", mins / 60, mins % 60);
    else                    snprintf(buf, n, "resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
}

// One usage window, rendered the way the panel renders it: label, reading,
// countdown, and a bar in the provider's color that goes amber then red.
static void send_metric(const Metric& m, const char* color) {
    if (!m.present) return;

    int pct = 0;
    char value[24];
    if (m.kind == METRIC_PCT) {
        pct = (int)(m.value + 0.5f);
        snprintf(value, sizeof(value), "%d%%", pct);
    } else if (m.kind == METRIC_MONEY) {
        if (m.limit > 0.0f) pct = (int)(m.value / m.limit * 100.0f + 0.5f);
        snprintf(value, sizeof(value), "$%d.%02d",
                 (int)m.value, (int)(m.value * 100) % 100);
    } else {
        if (m.limit > 0.0f) pct = (int)(m.value / m.limit * 100.0f + 0.5f);
        snprintf(value, sizeof(value), "%d", (int)(m.value + 0.5f));
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    char reset[40];
    fmt_reset(m.reset_mins, reset, sizeof(reset));

    String h = "<div class=\"metric\"><div class=\"mrow\"><span>";
    h += m.label;
    h += "</span><span>";
    h += value;
    if (m.kind != METRIC_PCT && m.limit > 0.0f) {
        h += " of ";
        if (m.kind == METRIC_MONEY) {
            char cap[24];
            snprintf(cap, sizeof(cap), "$%d.%02d",
                     (int)m.limit, (int)(m.limit * 100) % 100);
            h += cap;
        } else {
            h += String((int)m.limit);
        }
    }
    if (reset[0]) { h += " &#183; "; h += reset; }
    h += "</span></div><div class=\"bar\"><i style=\"width:";
    h += String(pct);
    h += "%;background:";
    h += (pct >= 80 ? "#c0392b" : pct >= 50 ? "#d97757" : color);
    h += "\"></i></div></div>";
    s_server.sendContent(h);
}

// Status pill for a provider: what the device currently thinks of it.
static void status_pill(const ProviderSlot& s, const char** cls, const char** txt) {
    if (!s.configured)  { *cls = "";     *txt = "No credentials"; return; }
    if (!s.enabled)     { *cls = "";     *txt = "Disabled";       return; }
    switch (s.state) {
        case PROV_OK:           *cls = "ok";   *txt = "Live";           return;
        case PROV_AUTH_NEEDED:  *cls = "err";  *txt = "Re-auth needed"; return;
        case PROV_LIMITED:      *cls = "err";  *txt = "Limit reached";  return;
        case PROV_DOWN:         *cls = "warn"; *txt = "API down";       return;
        case PROV_ERROR:        *cls = "warn"; *txt = "Error";          return;
        default:                *cls = "";     *txt = "Waiting";        return;
    }
}

static void send_lock_page(const char* banner_cls, const char* banner_txt) {
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "text/html", "");
    s_server.sendContent_P(CSS);
    if (banner_txt) {
        String b = "<div class=\"lock\"><div class=\"";
        b += banner_cls; b += "\">"; b += banner_txt; b += "</div></div>";
        s_server.sendContent(b);
    }
    s_server.sendContent_P(LOCK_FORM);
    s_server.sendContent("");
}

static void handle_root() {
    if (!request_authed()) { send_lock_page(nullptr, nullptr); return; }

    const int prim = provider_primary_index();
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "text/html", "");
    s_server.sendContent_P(CSS);
    if (s_server.hasArg("saved"))
        s_server.sendContent(F("<div class=\"ok-box\">Saved. The device is using "
                               "the new settings.</div>"));
    s_server.sendContent_P(FORM_HEAD);

    for (int i = 0; i < provider_count(); i++) {
        ProviderSlot s;
        provider_snapshot(i, &s);
        const ProviderDef* d = s.def;

        char color[8];
        snprintf(color, sizeof(color), "#%06lx", (unsigned long)d->color);
        const char *pcls, *ptxt;
        status_pill(s, &pcls, &ptxt);

        String h = "<div class=\"card\"><div class=\"chead\"><h2>"
                   "<span class=\"dot\" style=\"background:";
        h += color; h += "\"></span>"; h += d->name;
        h += "</h2><span class=\"pill "; h += pcls; h += "\">"; h += ptxt;
        h += "</span></div>";
        s_server.sendContent(h);

        // Every usage window the provider reports, so the page says exactly
        // what the panel says.
        if (s.valid) {
            send_metric(s.primary, color);
            send_metric(s.secondary, color);
        }

        for (int c = 0; c < d->cred_count; c++) {
            String f = "<label>";
            f += d->creds[c].label;
            f += "</label><input type=\"";
            f += d->creds[c].secret ? "password" : "text";
            f += "\" autocomplete=\"off\" name=\"";
            f += d->id; f += "_"; f += d->creds[c].key;
            f += "\" placeholder=\"";
            f += masked_hint(d->id, d->creds[c].key);
            f += "\">";
            s_server.sendContent(f);
        }

        String o = "<div class=\"opts\"><label class=\"opt\">"
                   "<input type=\"checkbox\" name=\"";
        o += d->id; o += "_en\" value=\"1\"";
        if (s.enabled) o += " checked";
        o += ">Enabled</label><label class=\"opt\">"
             "<input type=\"radio\" name=\"primary\" value=\"";
        o += d->id; o += "\"";
        if (i == prim) o += " checked";
        o += ">Primary</label></div></div>";
        s_server.sendContent(o);
    }

    s_server.sendContent_P(FORM_TAIL);
    s_server.sendContent("");
}

static void handle_auth() {
    uint32_t now = millis();
    if (s_lock_until && (int32_t)(now - s_lock_until) < 0) {
        send_lock_page("err-box", "Too many attempts. Wait a minute and try again.");
        Serial.println("config: auth attempt while locked out");
        return;
    }
    s_lock_until = 0;

    String code = s_server.hasArg("code") ? s_server.arg("code") : "";
    code.trim();
    code.toUpperCase();

    if (code.length() != 8 || !ct_equal(code.c_str(), s_code, 8)) {
        if (++s_auth_fails >= AUTH_MAX_FAILS) {
            s_lock_until = now + AUTH_LOCK_MS;
            s_auth_fails = 0;
            Serial.println("config: auth locked out after repeated failures");
        }
        send_lock_page("err-box", "That code isn't right. Check the device's Wi-Fi screen.");
        Serial.println("config: auth rejected (bad device code)");
        return;
    }

    s_auth_fails = 0;
    String cookie = "am_session=";
    cookie += session_create();
    cookie += "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800";
    s_server.sendHeader("Set-Cookie", cookie);
    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
    Serial.println("config: unlocked");
}

static void handle_logout() {
    String t = cookie_token();
    if (t.length() == 32) session_drop(t.c_str());
    s_server.sendHeader("Set-Cookie", "am_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
}

static void handle_save() {
    if (!request_authed()) {
        send_lock_page("err-box", "Your session expired. Enter the code again &#8212; "
                                  "nothing was saved.");
        Serial.println("config: save rejected (no session)");
        return;
    }

    for (int i = 0; i < provider_count(); i++) {
        ProviderSlot s;
        provider_snapshot(i, &s);
        const ProviderDef* d = s.def;

        for (int c = 0; c < d->cred_count; c++) {
            String field = String(d->id) + "_" + d->creds[c].key;
            if (s_server.hasArg(field) && s_server.arg(field).length() > 0) {
                secrets_set(d->id, d->creds[c].key, s_server.arg(field).c_str());
            }
        }
        secrets_set_enabled(d->id, s_server.hasArg(String(d->id) + "_en"));
        provider_refresh_config(i);
    }
    if (s_server.hasArg("primary")) {
        provider_set_primary(s_server.arg("primary").c_str());
    }

    // Same task as the LVGL loop (handleClient is polled from loop()), so the
    // rebuild is safe to run inline.
    ui_rebuild_provider_tiles();

    // The code that unlocked this session is spent, but the session survives —
    // no one wants to re-read the panel between two edits.
    rotate_code();

    s_server.sendHeader("Location", "/?saved=1");
    s_server.send(303, "text/plain", "");
    Serial.println("config: saved via LAN page");
}

// Plain-JSON state dump — handy for scripting and debugging. Read-only and
// carries no secrets, so it stays open: viewing is not changing.
static void handle_status() {
    String j = "{\"providers\":[";
    for (int i = 0; i < provider_count(); i++) {
        ProviderSlot s;
        provider_snapshot(i, &s);
        if (i) j += ",";
        j += "{\"id\":\"";        j += s.def->id;
        j += "\",\"enabled\":";   j += s.enabled ? "true" : "false";
        j += ",\"configured\":";  j += s.configured ? "true" : "false";
        j += ",\"state\":";       j += String((int)s.state);
        j += ",\"valid\":";       j += s.valid ? "true" : "false";
        j += ",\"primary\":";     j += String(s.primary.value, 2);
        j += ",\"secondary\":";   j += String(s.secondary.value, 2);
        j += ",\"http\":";        j += String(s.last_http_code);
        j += "}";
    }
    j += "]}";
    s_server.send(200, "application/json", j);
}

void config_server_tick(void) {
    if (!s_running) {
        if (!poller_is_connected()) return;
        if (!MDNS.begin("agentmeter")) {
            Serial.println("config: mDNS start failed (page still on device IP)");
        } else {
            MDNS.addService("http", "tcp", 80);
        }
        const char* headers[] = { "Cookie" };
        s_server.collectHeaders(headers, 1);
        s_server.on("/", HTTP_GET, handle_root);
        s_server.on("/auth", HTTP_POST, handle_auth);
        s_server.on("/logout", HTTP_POST, handle_logout);
        s_server.on("/save", HTTP_POST, handle_save);
        s_server.on("/status", HTTP_GET, handle_status);
        s_server.begin();
        s_running = true;
        rotate_code();
        Serial.printf("config: http://agentmeter.local (http://%s)\n",
            WiFi.localIP().toString().c_str());
        return;
    }
    if (millis() - s_code_ts >= CODE_ROTATE_MS) rotate_code();
    s_server.handleClient();
}

bool config_server_running(void) { return s_running; }
