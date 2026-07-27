#include "captive_portal.h"
#include "provisioning.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Arduino.h>

#define PORTAL_SSID "Agentmeter"

static WebServer  s_server(80);
static DNSServer  s_dns;
static bool       s_active = false;

static const char PORTAL_HTML[] = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Agentmeter Setup</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 20px;
     background:#1a1a1a;color:#e8e0d4}
h1{margin-bottom:6px}
.sub{color:#888;font-size:.9em;margin:0 0 28px}
label{display:block;margin-top:18px;font-size:.85em;color:#888;margin-bottom:4px}
input{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #444;
      border-radius:6px;background:#252525;color:#e8e0d4;font-size:1em}
button{margin-top:28px;width:100%;padding:13px;background:#d97757;border:none;
       border-radius:6px;color:#fff;font-size:1.05em;cursor:pointer;font-weight:600}
.note{margin-top:24px;font-size:.8em;color:#555;line-height:1.5}
</style></head>
<body>
<h1>Agentmeter Setup</h1>
<p class="sub">Connect the device to your Wi-Fi network.</p>
<form method="POST" action="/save">
  <label>Wi-Fi Network (SSID)</label>
  <input name="ssid" type="text" autocomplete="off" required>
  <label>Wi-Fi Password</label>
  <input name="pass" type="password" autocomplete="off" required>
  <button type="submit">Save &amp; Connect</button>
</form>
<p class="note">
  Provider tokens are configured afterwards from your normal network at
  <code>http://agentmeter.local</code>.<br>
  Device restarts automatically after saving.
</p>
</body></html>)html";

static const char SAVED_HTML[] = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><title>Saved</title>
<style>body{font-family:sans-serif;max-width:480px;margin:60px auto;padding:0 20px;
     background:#1a1a1a;color:#e8e0d4}h1{color:#7abf8e}p{color:#888}</style>
</head><body>
<h1>Saved &#10003;</h1>
<p>ClawdMeter is restarting and will connect to your network.</p>
<p>You can close this page.</p>
</body></html>)html";

static void send_form_error(const char* msg) {
    String html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Error</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:60px auto;"
        "padding:0 20px;background:#1a1a1a;color:#e8e0d4}"
        "h1{color:#d97757}p{color:#aaa}"
        "a{display:block;margin-top:24px;color:#d97757}</style>"
        "</head><body>"
        "<h1>Setup error</h1><p>";
    html += msg;
    html += "</p><a href='/'>&#8592; Back</a></body></html>";
    s_server.send(400, "text/html", html);
}

static void handle_root() {
    s_server.send(200, "text/html", PORTAL_HTML);
}

static void handle_save() {
    String ssid  = s_server.hasArg("ssid")  ? s_server.arg("ssid")  : "";
    String pass  = s_server.hasArg("pass")  ? s_server.arg("pass")  : "";

    if (ssid.length() == 0 || pass.length() == 0) {
        send_form_error("Wi-Fi network name and password are required.");
        return;
    }

    // Both keys in one NVS transaction — avoids partial credential state if
    // power is lost between writes. Provider tokens live in the LAN config
    // page (config_server.cpp), not here.
    provisioning_save_wifi(ssid.c_str(), pass.c_str(), nullptr);

    s_server.send(200, "text/html", SAVED_HTML);
    delay(1500);
    ESP.restart();
}

void captive_portal_start(void) {
    if (s_active) return;
    s_active = true;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(PORTAL_SSID);
    Serial.printf("portal: AP \"%s\" up, IP=%s\n",
                  PORTAL_SSID, WiFi.softAPIP().toString().c_str());

    s_dns.start(53, "*", WiFi.softAPIP());

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/save", HTTP_POST, handle_save);
    // Captive portal detection endpoints — Android and iOS check these URLs
    // when joining an AP; by serving our form (or a redirect), the OS shows
    // the "Sign into network" notification that opens the browser for the user.
    s_server.on("/generate_204", HTTP_GET, []() {
        s_server.sendHeader("Location", "http://192.168.4.1/");
        s_server.send(302, "text/plain", "");
    });
    s_server.on("/hotspot-detect.html", HTTP_GET, handle_root);
    s_server.onNotFound(handle_root);
    s_server.begin();
}

void captive_portal_init(void) {
    if (!provisioning_has_wifi()) captive_portal_start();
}

void captive_portal_tick(void) {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_server.handleClient();
}

bool captive_portal_is_active(void) { return s_active; }
