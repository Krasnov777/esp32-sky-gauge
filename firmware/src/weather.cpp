#include "weather.h"
#include "settings.h"
#include "net_fetch.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <math.h>

namespace weather {
namespace {

constexpr uint32_t REFRESH_MS = 10 * 60 * 1000;
constexpr uint32_t RETRY_MS   = 20 * 1000;
constexpr float    DEG2RAD    = 0.0174532925f;
constexpr float    KM_PER_DEG = 111.19f;
// Beyond this the nearest buienradar station isn't representative → fallback.
constexpr float    MAX_STATION_KM = 100.0f;

SemaphoreHandle_t mutex = nullptr;
Current  cur = {};
volatile Status   st = Status::Fetching;
volatile uint32_t last_ok_ms  = 0;
volatile bool     have_data   = false;
uint32_t last_try_ms = 0;
bool     tried_once  = false;

// Publish station conditions, preserving the rain nowcast (separate cadence).
void publish(Current n) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    memcpy(n.rain, cur.rain, sizeof(n.rain));
    n.rain_n = cur.rain_n;
    strlcpy(n.rain_start, cur.rain_start, sizeof(n.rain_start));
    cur = n;
    xSemaphoreGive(mutex);
    last_ok_ms = millis();
    have_data  = true;
}

void set_desc(Current& n, const char* s) {
    strlcpy(n.desc, s, sizeof(n.desc));
    if (strlen(s) >= sizeof(n.desc))            // truncated — show it honestly
        strcpy(n.desc + sizeof(n.desc) - 4, "...");
}

// Case-insensitive substring (Dutch descriptions are plain ASCII).
bool has(const char* haystack, const char* needle) {
    char low[64];
    strlcpy(low, haystack, sizeof(low));
    for (char* p = low; *p; p++) *p = tolower(*p);
    return strstr(low, needle) != nullptr;
}

// Classify a buienradar Dutch weatherdescription into an icon.
Icon icon_from_dutch(const char* d) {
    if (has(d, "onweer"))                        return Icon::Thunder;
    if (has(d, "sneeuw") || has(d, "hagel"))     return Icon::Snow;
    if (has(d, "regen") || has(d, "buien") ||
        has(d, "motregen"))                      return Icon::Rain;
    if (has(d, "mist") || has(d, "nevel"))       return Icon::Fog;
    if (has(d, "zwaar bewolkt") ||
        has(d, "geheel bewolkt"))                return Icon::Cloud;
    if (has(d, "half bewolkt") ||
        has(d, "opklaring") ||
        has(d, "wolkenvelden") ||
        has(d, "bewolking"))                     return Icon::Partly;
    if (has(d, "zonnig") || has(d, "onbewolkt") ||
        has(d, "helder"))                        return Icon::Sun;
    if (has(d, "bewolkt"))                       return Icon::Cloud;
    return Icon::Cloud;
}

// WMO weather code → icon + English description (Open-Meteo fallback).
struct WmoKind { Icon icon; const char* desc; };

WmoKind wmo_kind(uint8_t code) {
    switch (code) {
        case 0:  return {Icon::Sun,     "Clear sky"};
        case 1:  return {Icon::Partly,  "Mainly clear"};
        case 2:  return {Icon::Partly,  "Partly cloudy"};
        case 3:  return {Icon::Cloud,   "Overcast"};
        case 45: case 48: return {Icon::Fog, "Fog"};
        case 51: case 53: case 55: return {Icon::Rain, "Drizzle"};
        case 56: case 57: return {Icon::Rain, "Freezing drizzle"};
        case 61: case 63: case 65: return {Icon::Rain, "Rain"};
        case 66: case 67: return {Icon::Rain, "Freezing rain"};
        case 71: case 73: case 75: case 77: return {Icon::Snow, "Snow"};
        case 80: case 81: case 82: return {Icon::Rain, "Rain showers"};
        case 85: case 86: return {Icon::Snow, "Snow showers"};
        case 95: return {Icon::Thunder, "Thunderstorm"};
        case 96: case 99: return {Icon::Thunder, "Storm with hail"};
        default: return {Icon::Cloud, "Clouds"};
    }
}

// ── buienradar.nl (primary) ──────────────────────────────────────────────────
// One feed for the whole country (~36 KB). The feed no longer carries station
// coordinates (Latitude/Longitude are all 0 as of 2026-06), so we match
// StationId against a baked-in table of KNMI station positions
// (StationId = 6000 + KNMI STN; coordinates from daggegevens.knmi.nl).
struct StationPos { uint16_t id; float lat, lon; };

constexpr StationPos STATIONS[] = {
    {6215, 52.141f, 4.437f},  // Voorschoten
    {6225, 52.463f, 4.555f},  // IJmuiden
    {6229, 52.996f, 4.721f},  // Texelhors
    {6235, 52.928f, 4.781f},  // Den Helder (De Kooy)
    {6239, 54.854f, 4.696f},  // Zeeplatform F-3
    {6240, 52.318f, 4.790f},  // Schiphol
    {6242, 53.241f, 4.921f},  // Vlieland
    {6248, 52.634f, 5.174f},  // Wijdenes
    {6249, 52.644f, 4.979f},  // Berkhout
    {6251, 53.392f, 5.346f},  // Hoorn Terschelling
    {6257, 52.506f, 4.603f},  // Wijk aan Zee
    {6258, 52.649f, 5.401f},  // Houtribdijk
    {6260, 52.100f, 5.180f},  // De Bilt
    {6267, 52.898f, 5.384f},  // Stavoren
    {6269, 52.458f, 5.520f},  // Lelystad
    {6270, 53.224f, 5.752f},  // Leeuwarden
    {6273, 52.703f, 5.888f},  // Marknesse
    {6275, 52.056f, 5.873f},  // Arnhem (Deelen)
    {6277, 53.413f, 6.200f},  // Lauwersoog
    {6278, 52.435f, 6.259f},  // Heino
    {6279, 52.750f, 6.574f},  // Hoogeveen
    {6280, 53.125f, 6.585f},  // Groningen (Eelde)
    {6283, 52.069f, 6.657f},  // Groenlo-Hupsel
    {6286, 53.196f, 7.150f},  // Nieuw Beerta
    {6290, 52.274f, 6.891f},  // Twente
    {6310, 51.442f, 3.596f},  // Vlissingen
    {6319, 51.226f, 3.861f},  // Westdorpe
    {6323, 51.527f, 3.884f},  // Goes (Wilhelminadorp)
    {6330, 51.992f, 4.122f},  // Hoek van Holland
    {6340, 51.449f, 4.342f},  // Woensdrecht
    {6343, 51.893f, 4.313f},  // Rotterdam Geulhaven
    {6344, 51.962f, 4.447f},  // Rotterdam
    {6348, 51.970f, 4.926f},  // Lopik-Cabauw
    {6350, 51.566f, 4.936f},  // Gilze Rijen
    {6356, 51.859f, 5.146f},  // Herwijnen
    {6370, 51.451f, 5.377f},  // Eindhoven
    {6375, 51.659f, 5.707f},  // Volkel
    {6377, 51.198f, 5.763f},  // Ell
    {6380, 50.906f, 5.762f},  // Maastricht (Beek)
    {6392, 51.487f, 6.056f},  // Horst
};

const StationPos* station_pos(uint16_t id) {
    for (const auto& s : STATIONS)
        if (s.id == id) return &s;
    return nullptr;
}

bool fetch_buienradar(float lat, float lon) {
    JsonDocument filter;
    JsonObject f = filter["Actual"]["WeatherStationMeasurements"].add<JsonObject>();
    f["StationId"]            = true;
    f["StationName"]          = true;
    f["Temperature"]          = true;
    f["FeelTemperature"]      = true;
    f["Humidity"]             = true;
    f["Windspeed"]            = true;
    f["WindDirectionDegrees"] = true;
    f["WeatherDescription"]   = true;

    JsonDocument doc(net::psram_allocator());
    if (!net::http_get_json("https://data.buienradar.nl/2.0/feed/json", doc, filter))
        return false;

    float cos_lat = cosf(lat * DEG2RAD);
    float best_km = 1e9f;
    JsonObjectConst best;
    for (JsonObjectConst ms : doc["Actual"]["WeatherStationMeasurements"].as<JsonArrayConst>()) {
        if (!ms["Temperature"].is<float>()) continue;   // some stations are rain-only
        const StationPos* pos = station_pos(ms["StationId"] | 0);
        if (!pos) continue;
        float dlat = pos->lat - lat;
        float dlon = (pos->lon - lon) * cos_lat;
        float d = KM_PER_DEG * sqrtf(dlat * dlat + dlon * dlon);
        if (d < best_km) { best_km = d; best = ms; }
    }
    if (best.isNull() || best_km > MAX_STATION_KM) {
        log_i("[weather] no buienradar station within %.0f km", MAX_STATION_KM);
        return false;
    }

    Current n = {};
    n.temp_c       = best["Temperature"]     | 0.0f;
    n.feels_c      = best["FeelTemperature"] | n.temp_c;
    n.humidity     = (uint8_t)(best["Humidity"] | 0);
    n.wind_kmh     = (best["Windspeed"] | 0.0f) * 3.6f;   // feed is m/s
    n.wind_dir_deg = best["WindDirectionDegrees"] | 0;
    const char* d  = best["WeatherDescription"] | "";
    set_desc(n, d);
    n.icon  = icon_from_dutch(d);
    n.valid = true;
    publish(n);
    log_i("[weather] buienradar '%s' (%.0f km): %.1f°C '%s'",
          best["StationName"] | "?", best_km, n.temp_c, n.desc);
    return true;
}

// ── Open-Meteo (fallback outside NL/BE coverage) ─────────────────────────────
bool fetch_openmeteo(float lat, float lon) {
    char url[224];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
             "weather_code,wind_speed_10m,wind_direction_10m",
             lat, lon);

    JsonDocument filter;
    JsonObject f = filter["current"].to<JsonObject>();
    f["temperature_2m"]       = true;
    f["apparent_temperature"] = true;
    f["relative_humidity_2m"] = true;
    f["weather_code"]         = true;
    f["wind_speed_10m"]       = true;
    f["wind_direction_10m"]   = true;

    JsonDocument doc;
    if (!net::http_get_json(url, doc, filter)) return false;
    JsonObjectConst c = doc["current"];
    if (c.isNull()) return false;

    Current n = {};
    n.temp_c       = c["temperature_2m"]       | 0.0f;
    n.feels_c      = c["apparent_temperature"] | 0.0f;
    n.humidity     = c["relative_humidity_2m"] | 0;
    n.wind_kmh     = c["wind_speed_10m"]       | 0.0f;
    n.wind_dir_deg = c["wind_direction_10m"]   | 0;
    WmoKind k = wmo_kind(c["weather_code"] | 0);
    n.icon  = k.icon;
    set_desc(n, k.desc);
    n.valid = true;
    publish(n);
    log_i("[weather] open-meteo: %.1f°C '%s'", n.temp_c, n.desc);
    return true;
}

// ── rain nowcast (buienradar raintext) ───────────────────────────────────────
// Plain text, one "value|HH:MM" line per 5-minute step for the next 2 hours.
bool fetch_rain(float lat, float lon) {
    char url[128];
    snprintf(url, sizeof(url),
             "https://gpsgadget.buienradar.nl/data/raintext?lat=%.2f&lon=%.2f",
             lat, lon);
    char body[768];
    if (!net::http_get_text(url, body, sizeof(body))) return false;

    uint8_t levels[24] = {};
    uint8_t n = 0;
    char    start[6] = "";
    char*   save = nullptr;
    for (char* line = strtok_r(body, "\r\n", &save); line && n < 24;
         line = strtok_r(nullptr, "\r\n", &save)) {
        char* bar = strchr(line, '|');
        if (!bar) continue;
        if (n == 0) strlcpy(start, bar + 1, sizeof(start));
        levels[n++] = (uint8_t)constrain(atoi(line), 0, 255);
    }
    if (n == 0) return false;

    xSemaphoreTake(mutex, portMAX_DELAY);
    memcpy(cur.rain, levels, sizeof(cur.rain));
    cur.rain_n = n;
    strlcpy(cur.rain_start, start, sizeof(cur.rain_start));
    xSemaphoreGive(mutex);

    uint8_t peak = 0;
    for (uint8_t i = 0; i < n; i++) peak = max(peak, levels[i]);
    log_i("[weather] rain nowcast: %u steps from %s, peak %u", n, start, peak);
    return true;
}

void task_fn(void*) {
    uint32_t last_rain_try_ms = 0;
    bool     rain_tried       = false;
    bool     on_buienradar    = false;   // raintext only covers NL/BE

    for (;;) {
        const auto& cfg = settings::state();

        // Active in Weather mode, and in Auto mode when Weather is the
        // resting screen (auto_base == 0).
        bool active = cfg.mode == settings::Mode::Weather ||
                      (cfg.mode == settings::Mode::Auto && cfg.radar.auto_base == 0);
        if (!active) {
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
            on_buienradar = fetch_buienradar(cfg.radar.lat, cfg.radar.lon);
            bool ok = on_buienradar || fetch_openmeteo(cfg.radar.lat, cfg.radar.lon);
            st = ok || have_data ? Status::Ok : Status::Error;
        }

        // Rain nowcast on its own 5-minute cadence (the radar images behind
        // it update every 5 minutes; failures just retry next cycle).
        // Only meaningful inside buienradar coverage (NL/BE).
        if (on_buienradar &&
            (!rain_tried || now - last_rain_try_ms >= 5 * 60 * 1000UL)) {
            rain_tried       = true;
            last_rain_try_ms = now;
            fetch_rain(cfg.radar.lat, cfg.radar.lon);
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
