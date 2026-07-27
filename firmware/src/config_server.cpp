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

static const char PAGE_HEAD[] = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Agentmeter</title>
<style>
body{font-family:sans-serif;max-width:520px;margin:40px auto;padding:0 20px;
     background:#1a1a1a;color:#e8e0d4}
h1{margin-bottom:6px}
.sub{color:#888;font-size:.9em;margin:0 0 28px}
fieldset{border:1px solid #333;border-radius:8px;margin-top:22px;padding:14px 16px 18px}
legend{padding:0 8px;font-weight:600}
label{display:block;margin-top:12px;font-size:.85em;color:#888;margin-bottom:4px}
input[type=text],input[type=password]{width:100%;box-sizing:border-box;
     padding:10px 12px;border:1px solid #444;border-radius:6px;background:#252525;
     color:#e8e0d4;font-size:1em}
.row{margin-top:12px;font-size:.9em;color:#aaa}
.row input{margin-right:8px}
button{margin-top:28px;width:100%;padding:13px;background:#d97757;border:none;
       border-radius:6px;color:#fff;font-size:1.05em;cursor:pointer;font-weight:600}
.note{margin-top:24px;font-size:.8em;color:#555;line-height:1.5}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}
</style></head>
<body>
<h1>Agentmeter</h1>
<p class="sub">Provider configuration &#8212; saves apply immediately, no reboot.</p>
<form method="POST" action="/save">
)html";

static const char PAGE_TAIL[] = R"html(
<fieldset><legend>Device code</legend>
<label>The 8-character code on the device's Wi-Fi screen (rotates every 10 min)</label>
<input type="text" name="code" autocomplete="off" required maxlength="8"
       style="text-transform:uppercase;letter-spacing:.2em">
</fieldset>
<button type="submit">Save</button>
</form>
<p class="note">
Secret fields are write-only: leave one empty to keep the stored value.
Saving requires the code shown on the device, so this page can be viewed
&#8212; but not changed &#8212; by others on the network.
</p>
</body></html>)html";

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

static void handle_root() {
    int prim = provider_primary_index();
    String html = PAGE_HEAD;
    for (int i = 0; i < provider_count(); i++) {
        ProviderSlot s;
        provider_snapshot(i, &s);
        const ProviderDef* d = s.def;
        char color[8];
        snprintf(color, sizeof(color), "#%06lx", (unsigned long)d->color);

        html += "<fieldset><legend><span class=\"dot\" style=\"background:";
        html += color;
        html += "\"></span>";
        html += d->name;
        html += "</legend>";
        for (int c = 0; c < d->cred_count; c++) {
            html += "<label>";
            html += d->creds[c].label;
            html += "</label><input type=\"";
            html += d->creds[c].secret ? "password" : "text";
            html += "\" autocomplete=\"off\" name=\"";
            html += d->id; html += "_"; html += d->creds[c].key;
            html += "\" placeholder=\"";
            html += masked_hint(d->id, d->creds[c].key);
            html += "\">";
        }
        html += "<div class=\"row\"><label style=\"display:inline\">"
                "<input type=\"checkbox\" name=\"";
        html += d->id;
        html += "_en\" value=\"1\"";
        if (s.enabled) html += " checked";
        html += ">Enabled</label>&nbsp;&nbsp;&nbsp;"
                "<label style=\"display:inline\">"
                "<input type=\"radio\" name=\"primary\" value=\"";
        html += d->id;
        html += "\"";
        if (i == prim) html += " checked";
        html += ">Primary (drives chime &amp; status)</label></div>";
        html += "</fieldset>";
    }
    html += PAGE_TAIL;
    s_server.send(200, "text/html", html);
}

static void handle_save() {
    String code = s_server.hasArg("code") ? s_server.arg("code") : "";
    code.trim();
    code.toUpperCase();
    if (code != s_code) {
        s_server.send(403, "text/html",
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<style>body{font-family:sans-serif;max-width:480px;margin:60px auto;"
            "padding:0 20px;background:#1a1a1a;color:#e8e0d4}h1{color:#d97757}"
            "p{color:#aaa}a{display:block;margin-top:24px;color:#d97757}</style>"
            "</head><body><h1>Wrong device code</h1>"
            "<p>The code is on the device's Wi-Fi screen and rotates every "
            "10 minutes. Nothing was saved.</p>"
            "<a href='/'>&#8592; Back</a></body></html>");
        Serial.println("config: save rejected (bad device code)");
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

    rotate_code();   // a used code is spent — the next save needs a fresh one

    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
    Serial.println("config: saved via LAN page");
}

// Plain-JSON state dump — handy for scripting and debugging.
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
        s_server.on("/", HTTP_GET, handle_root);
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
