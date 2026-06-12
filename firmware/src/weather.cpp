#include "weather.h"
#include "settings.h"
#include "net_lock.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace weather {
namespace {

constexpr uint32_t REFRESH_MS = 10 * 60 * 1000;   // Open-Meteo updates ~15-min
constexpr uint32_t RETRY_MS   = 20 * 1000;

SemaphoreHandle_t mutex = nullptr;
Current  cur = {};
volatile Status   st = Status::Fetching;
volatile uint32_t last_ok_ms  = 0;
volatile bool     have_data   = false;
uint32_t last_try_ms = 0;
bool     tried_once  = false;

bool fetch(float lat, float lon) {
    char url[224];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
             "weather_code,wind_speed_10m,wind_direction_10m",
             lat, lon);

    netlock::Guard one_tls_at_a_time;
    WiFiClientSecure client;
    client.setInsecure();   // public read-only data; CA validation not worth the RAM
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        log_w("[weather] GET -> %d", code);
        http.end();
        return false;
    }
    String body = http.getString();   // ~1 KB — fine on the heap
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        log_w("[weather] JSON parse failed");
        return false;
    }
    JsonObjectConst c = doc["current"];
    if (c.isNull()) return false;

    Current n = {};
    n.temp_c       = c["temperature_2m"]       | 0.0f;
    n.feels_c      = c["apparent_temperature"] | 0.0f;
    n.humidity     = c["relative_humidity_2m"] | 0;
    n.wind_kmh     = c["wind_speed_10m"]       | 0.0f;
    n.wind_dir_deg = c["wind_direction_10m"]   | 0;
    n.code         = c["weather_code"]         | 0;
    n.valid        = true;

    xSemaphoreTake(mutex, portMAX_DELAY);
    cur = n;
    xSemaphoreGive(mutex);
    last_ok_ms = millis();
    have_data  = true;
    log_i("[weather] %.1f°C code %u wind %.0f km/h", n.temp_c, n.code, n.wind_kmh);
    return true;
}

void task_fn(void*) {
    for (;;) {
        const auto& cfg = settings::state();

        if (cfg.mode != settings::Mode::Weather) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (cfg.radar.lat == 0.0f && cfg.radar.lon == 0.0f) {
            st = Status::NoLocation;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            st = Status::NoWifi;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        uint32_t now = millis();
        bool stale = !have_data || now - last_ok_ms >= REFRESH_MS;
        if (stale && (!tried_once || now - last_try_ms >= RETRY_MS)) {
            tried_once  = true;
            last_try_ms = now;
            if (!have_data) st = Status::Fetching;
            bool ok = fetch(cfg.radar.lat, cfg.radar.lon);
            st = ok || have_data ? Status::Ok : Status::Error;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

void begin() {
    if (mutex) return;
    mutex = xSemaphoreCreateMutex();
    // 10 KB stack: TLS handshake runs here.
    xTaskCreatePinnedToCore(task_fn, "weather", 10240, nullptr, 1, nullptr, 0);
    log_i("[weather] poller task started on core 0");
}

Current get() {
    Current c = {};
    if (!mutex) return c;
    xSemaphoreTake(mutex, portMAX_DELAY);
    c = cur;
    xSemaphoreGive(mutex);
    return c;
}

Status status() { return st; }

uint32_t data_age_ms() {
    return have_data ? millis() - last_ok_ms : UINT32_MAX;
}

}  // namespace weather
