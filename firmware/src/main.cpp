// Entry point — orchestrates settings, display, UI, WiFi, web server and the
// two data pollers (flight radar + weather).
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "settings.h"
#include "display.h"
#include "ui.h"
#include "web_server.h"
#include "radar.h"
#include "weather.h"
#include "homeassistant.h"
#include "net_lock.h"

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
    // Disable WiFi modem-sleep (power save). It's on by default and is a
    // notorious cause of "associated but unresponsive / dropping connections"
    // flakiness — which this unit suffered repeatedly. This is a mains/USB
    // powered desk device, so the extra draw is irrelevant.
    WiFi.setSleep(false);
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
        ui::update_status("Connecting WiFi...", s.wifi_ssid);
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

// OTA firmware + LittleFS updates over WiFi (no USB cable needed):
//   pio run -t upload   --upload-protocol espota --upload-port esp-gauge.local
//   pio run -t uploadfs --upload-protocol espota --upload-port esp-gauge.local
// The net lock is held for the whole transfer so the pollers' TLS traffic
// can't compete with the update for heap/bandwidth.
bool ota_holds_netlock = false;

void setup_ota() {
    ArduinoOTA.setHostname(settings::state().hostname);
    ArduinoOTA.onStart([]() {
        ota_holds_netlock =
            xSemaphoreTake(netlock::handle(), pdMS_TO_TICKS(15000)) == pdTRUE;
        ui::show_status();
        ui::update_status("OTA update", "starting...");
        display::tick();
        log_i("OTA started (%s)",
              ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t last_pct = 255;
        uint8_t pct = done * 100 / total;
        if (pct == last_pct) return;
        last_pct = pct;
        char buf[12];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        ui::update_status("OTA update", buf);
        display::tick();
    });
    ArduinoOTA.onEnd([]() {
        ui::update_status("OTA done", "rebooting...");
        display::tick();
        if (ota_holds_netlock) xSemaphoreGive(netlock::handle());
    });
    ArduinoOTA.onError([](ota_error_t err) {
        log_e("OTA error %u", err);
        if (ota_holds_netlock) xSemaphoreGive(netlock::handle());
        ota_holds_netlock = false;
        ui::update_status("OTA failed", "");
        display::tick();
    });
    ArduinoOTA.begin();
    log_i("OTA ready on port 3232");
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
    ui::update_status("Booting...", "");
    display::tick();

    connect_or_ap();
    web::begin();
    setup_ota();
    radar::begin();     // pollers idle until their mode is active
    weather::begin();
    homeassistant::begin();

    // Let the user see the IP for a moment before switching to their mode.
    uint32_t t0 = millis();
    while (millis() - t0 < 2500) display::tick();

    display::set_brightness(settings::state().brightness);
    ui::set_mode(settings::state().mode);
}

void loop() {
    display::tick();
    web::loop_tick();
    ArduinoOTA.handle();

    // STA association can drop and stay down silently (observed in the wild:
    // device alive, WiFi gone, nothing reconnecting). Nudge it every 30 s.
    static uint32_t last_wifi_ok_ms = 0;
    uint32_t now = millis();
    if (WiFi.getMode() == WIFI_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            last_wifi_ok_ms = now;
        } else if (now - last_wifi_ok_ms > 30000) {
            last_wifi_ok_ms = now;   // rate-limit attempts
            log_w("WiFi down >30 s — reconnecting");
            WiFi.reconnect();
        }
    }
    // Yield briefly instead of busy-spinning core 1 — LVGL timers are 33 ms+
    // granular, so a 1 ms sleep costs nothing and lets the idle task run.
    delay(1);
}
