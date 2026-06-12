// Entry point — orchestrates settings, display, UI, WiFi, web server and the
// two data pollers (flight radar + weather).
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "settings.h"
#include "display.h"
#include "ui.h"
#include "web_server.h"
#include "radar.h"
#include "weather.h"

namespace {

constexpr uint32_t WIFI_TIMEOUT_MS = 20000;

// AP fallback identity ───────────────────────────────────────────────────────
String ap_ssid() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "ESP-Gauge-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

bool try_sta(const char* ssid, const char* pwd) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(settings::state().hostname);
    WiFi.begin(ssid, pwd);
    log_i("WiFi STA → SSID='%s'", ssid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        return false;
    }
    log_i("WiFi connected, IP = %s", WiFi.localIP().toString().c_str());
    return true;
}

void start_ap_mode() {
    WiFi.mode(WIFI_AP);
    String ssid = ap_ssid();
    WiFi.softAP(ssid.c_str(), nullptr);
    log_i("AP mode '%s' on %s", ssid.c_str(), WiFi.softAPIP().toString().c_str());
    char l1[40];
    snprintf(l1, sizeof(l1), "AP %s", ssid.c_str());
    ui::update_status(l1, WiFi.softAPIP().toString().c_str());
}

void connect_or_ap() {
    const auto& s = settings::state();
    bool connected = false;
    if (strlen(s.wifi_ssid) > 0) {
        ui::update_status("Connecting WiFi…", s.wifi_ssid);
        connected = try_sta(s.wifi_ssid, s.wifi_password);
    }
    if (!connected) {
        start_ap_mode();
    } else {
        ui::update_status("WiFi connected", WiFi.localIP().toString().c_str());
    }

    // mDNS works in either mode — but skip if AP, just in case.
    if (connected && MDNS.begin(s.hostname)) {
        MDNS.addService("http", "tcp", 80);
        log_i("mDNS as %s.local", s.hostname);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(150);
    log_i("Booting ESP32-S3 sky gauge");

    settings::begin();
    display::begin();
    ui::begin();

    // Show the status screen during boot so the user can read network messages,
    // then switch to their saved mode once the network is up.
    ui::show_status();
    ui::update_status("Booting…", "");
    display::tick();

    connect_or_ap();
    web::begin();
    radar::begin();     // pollers idle until their mode is active
    weather::begin();

    // Let the user see the IP for a moment before switching to their mode.
    uint32_t t0 = millis();
    while (millis() - t0 < 2500) display::tick();

    display::set_brightness(settings::state().brightness);
    ui::set_mode(settings::state().mode);
}

void loop() {
    display::tick();
    web::loop_tick();
    // Yield briefly instead of busy-spinning core 1 — LVGL timers are 33 ms+
    // granular, so a 1 ms sleep costs nothing and lets the idle task run.
    delay(1);
}
