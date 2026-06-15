#include "homeassistant.h"
#include "net_fetch.h"

#include <WiFi.h>
#include <ArduinoJson.h>

namespace homeassistant {
namespace {

constexpr uint32_t RETRY_MS = 10 * 1000;

SemaphoreHandle_t mutex = nullptr;
Snapshot snap = {};
volatile Status   st = Status::Fetching;
volatile uint32_t last_ok_ms = 0;
volatile bool     have_data  = false;
uint32_t last_try_ms = 0;
bool     tried_once  = false;

// type preset catalog — keep keys in sync with the web UI dropdown.
struct Preset { const char* key; const char* unit; int decimals; bool secondary; };
const Preset PRESETS[] = {
    {"temperature", "\xC2\xB0", 1, false},   // ° (UTF-8)
    {"climate",     "\xC2\xB0", 1, true},    // temp + humidity secondary
    {"humidity",    "%",   0, false},
    {"power",       " W",  0, false},
    {"battery",     "%",   0, false},
    {"co2",         " ppm",0, false},
    {"pressure",    " hPa",0, false},
    {"voltage",     " V",  1, false},
    {"custom",      "",    1, false},        // shows raw string
};
const Preset& preset_for(const char* key) {
    for (auto& p : PRESETS) if (strcmp(key, p.key) == 0) return p;
    return PRESETS[sizeof(PRESETS) / sizeof(PRESETS[0]) - 1];   // custom
}

// GET {url}/api/states/{entity} → numeric value + raw string.
bool get_state(const char* base, const char* token, const char* entity,
               float& num_out, char* raw_out, size_t raw_cap) {
    if (!entity[0]) return false;

    char url[224];
    // trim trailing slash on base
    int n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    snprintf(url, sizeof(url), "%.*s/api/states/%s", n, base, entity);

    char body[512];
    if (!net::http_get_text(url, body, sizeof(body), token)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    const char* state = doc["state"] | "";
    if (!state[0] || !strcmp(state, "unavailable") || !strcmp(state, "unknown"))
        return false;
    if (raw_out) strlcpy(raw_out, state, raw_cap);
    num_out = atof(state);
    return true;
}

void poll_all(const settings::HomeConfig& cfg) {
    Snapshot local = {};
    bool any = false, all_ok = true;

    for (int i = 0; i < settings::HOME_TILES; i++) {
        Tile& t = local.tiles[i];
        t.configured = cfg.entity[i][0] != '\0';
        if (!t.configured) continue;
        any = true;

        t.ok = get_state(cfg.url, cfg.token, cfg.entity[i],
                         t.value, t.raw, sizeof(t.raw));
        if (!t.ok) all_ok = false;

        if (preset_for(cfg.type[i]).secondary && cfg.entity2[i][0]) {
            char dummy[20];
            t.sec_ok = get_state(cfg.url, cfg.token, cfg.entity2[i],
                                 t.sec_value, dummy, sizeof(dummy));
        }
    }
    local.any = any;

    xSemaphoreTake(mutex, portMAX_DELAY);
    snap = local;
    xSemaphoreGive(mutex);

    if (any && all_ok) { last_ok_ms = millis(); have_data = true; }
    st = !any ? Status::NoConfig
       : all_ok ? Status::Ok
       : (have_data ? Status::Ok : Status::Error);
}

void task_fn(void*) {
    for (;;) {
        const auto& s = settings::state();
        // Active in Home mode, and in Auto mode when Home is the resting
        // screen (auto_base == 1).
        bool active = s.mode == settings::Mode::Home ||
                      (s.mode == settings::Mode::Auto && s.radar.auto_base == 1);
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s.home.url[0] == '\0' || s.home.token[0] == '\0') {
            st = Status::NoConfig;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            st = Status::NoWifi;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        uint32_t now = millis();
        uint32_t period = max<uint16_t>(s.home.poll_s, 5) * 1000u;
        bool due = !tried_once || now - last_try_ms >= period ||
                   (!have_data && now - last_try_ms >= RETRY_MS);
        if (due) {
            tried_once  = true;
            last_try_ms = now;
            if (!have_data) st = Status::Fetching;
            poll_all(s.home);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void begin() {
    if (mutex) return;
    mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(task_fn, "ha", 10240, nullptr, 1, nullptr, 0);
    log_i("[ha] poller task started on core 0");
}

Snapshot get() {
    Snapshot s = {};
    if (!mutex) return s;
    xSemaphoreTake(mutex, portMAX_DELAY);
    s = snap;
    xSemaphoreGive(mutex);
    return s;
}

Status status() { return st; }

uint32_t data_age_ms() {
    return have_data ? millis() - last_ok_ms : UINT32_MAX;
}

TypeInfo type_info(const char* key) {
    const Preset& p = preset_for(key);
    return {p.unit, p.decimals, p.secondary};
}

}  // namespace homeassistant
