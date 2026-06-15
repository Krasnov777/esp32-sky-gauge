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

// GET {url}/api/states/{entity} → state string + unit_of_measurement.
bool get_state(const char* base, const char* token, const char* entity,
               char* val_out, size_t val_cap, char* unit_out, size_t unit_cap) {
    if (!entity[0]) return false;

    char url[224];
    // trim trailing slash on base
    int n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    snprintf(url, sizeof(url), "%.*s/api/states/%s", n, base, entity);

    char body[2048];
    if (!net::http_get_text(url, body, sizeof(body), token)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    const char* state = doc["state"] | "";
    if (!state[0] || !strcmp(state, "unavailable") || !strcmp(state, "unknown"))
        return false;

    // HA reports floats at full precision (e.g. "21.7000007629395"). Round a
    // purely-numeric state to 1 decimal for a glanceable display, and strip a
    // trailing ".0"; keep non-numeric states ("on", "home", …) verbatim.
    char* end = nullptr;
    double num = strtod(state, &end);
    if (end != state && *end == '\0') {
        snprintf(val_out, val_cap, "%.1f", num);
        char* p = val_out + strlen(val_out) - 1;
        while (p > val_out && *p == '0') *p-- = '\0';
        if (*p == '.') *p = '\0';
    } else {
        strlcpy(val_out, state, val_cap);
    }
    strlcpy(unit_out, doc["attributes"]["unit_of_measurement"] | "", unit_cap);
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
                         t.value, sizeof(t.value), t.unit, sizeof(t.unit));
        if (!t.ok) all_ok = false;
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
        // Active in Home mode, and in Auto mode when Home is among the resting
        // screens (auto_base bit1).
        bool active = s.mode == settings::Mode::Home ||
                      (s.mode == settings::Mode::Auto && (s.radar.auto_base & 2));
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

}  // namespace homeassistant
