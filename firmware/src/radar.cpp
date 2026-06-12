#include "radar.h"
#include "settings.h"
#include "net_lock.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

namespace radar {
namespace {

constexpr float DEG2RAD = 0.0174532925f;
constexpr float KM_PER_DEG = 111.19f;
constexpr float KM_PER_NM = 1.852f;

// ── Shared snapshot (poller writes, UI reads) ────────────────────────────────
SemaphoreHandle_t mutex = nullptr;
Aircraft aircraft[MAX_AIRCRAFT];
size_t   aircraft_n = 0;
volatile Status   cur_status = Status::Scanning;
volatile uint32_t last_ok_ms = 0;
volatile bool     have_data  = false;

// ── Route cache (callsign → "JFK>LHR") ───────────────────────────────────────
// Small ring: ~24 concurrent callsigns is plenty for a 100 km scope. Unknown
// callsigns are cached with an empty route so we don't hammer adsbdb retrying.
struct RouteEntry {
    char callsign[12] = "";
    char route[16]    = "";
};
constexpr size_t ROUTE_CACHE_N = 24;
RouteEntry route_cache[ROUTE_CACHE_N];
size_t     route_cache_next = 0;

const RouteEntry* find_route(const char* cs) {
    for (const auto& e : route_cache)
        if (e.callsign[0] && strcmp(e.callsign, cs) == 0) return &e;
    return nullptr;
}

void cache_route(const char* cs, const char* route) {
    RouteEntry& e = route_cache[route_cache_next];
    route_cache_next = (route_cache_next + 1) % ROUTE_CACHE_N;
    strlcpy(e.callsign, cs, sizeof(e.callsign));
    strlcpy(e.route, route, sizeof(e.route));
}

// ── HTTP helpers ─────────────────────────────────────────────────────────────
// The adsb.lol response can be a few hundred KB in busy airspace. Stream-
// parsing it byte-by-byte over TLS is slow enough that the server drops the
// connection mid-body, so we slurp the whole response into PSRAM (2 MB,
// otherwise unused) and parse from memory. The JsonDocument lives in PSRAM
// too, keeping internal DRAM free for the TLS handshake.

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override {
        void* p = ps_malloc(n);
        return p ? p : malloc(n);
    }
    void deallocate(void* p) override { free(p); }
    void* reallocate(void* p, size_t n) override {
        void* q = ps_realloc(p, n);
        return q ? q : realloc(p, n);
    }
};
PsramAllocator psram_alloc;

constexpr size_t BODY_CAP = 700 * 1024;

// Response buffer, allocated once in PSRAM (heap fallback) and reused across
// polls — re-allocating ~700 KB every cycle is pointless churn.
char*  body_buf = nullptr;
size_t body_cap = 0;

// Read the full body of an in-progress GET into the shared buffer.
char* read_body(HTTPClient& http, size_t& out_len) {
    if (!body_buf) {
        body_buf = (char*)ps_malloc(BODY_CAP);
        body_cap = BODY_CAP;
        if (!body_buf) {                        // no PSRAM? small heap fallback
            body_buf = (char*)malloc(48 * 1024);
            body_cap = 48 * 1024;
        }
        if (!body_buf) return nullptr;
    }

    WiFiClient* stream = http.getStreamPtr();
    int   total = http.getSize();               // -1 when no Content-Length
    size_t len = 0;
    uint32_t last_data = millis();

    while (stream->connected() || stream->available()) {
        size_t avail = stream->available();
        if (avail) {
            size_t want = min(avail, body_cap - 1 - len);
            if (want == 0) break;               // cap reached
            len += stream->readBytes(body_buf + len, want);
            last_data = millis();
            if (total > 0 && (int)len >= total) break;
        } else {
            if (millis() - last_data > 8000) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    body_buf[len] = '\0';
    out_len = len;
    return body_buf;
}

bool http_get_json(const char* url, JsonDocument& doc, JsonDocument& filter) {
    netlock::Guard one_tls_at_a_time;
    WiFiClientSecure client;
    client.setInsecure();   // public read-only data; CA validation not worth the RAM
    HTTPClient http;
    http.useHTTP10(true);   // no chunked transfer encoding
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        log_w("[radar] GET %s -> %d", url, code);
        http.end();
        return false;
    }

    size_t len = 0;
    char* body = read_body(http, len);
    http.end();
    if (!body) {
        log_w("[radar] body alloc failed");
        return false;
    }

    DeserializationError err =
        deserializeJson(doc, body, len, DeserializationOption::Filter(filter));
    if (err) {
        log_w("[radar] JSON parse (%u bytes): %s", (unsigned)len, err.c_str());
        return false;
    }
    return true;
}

// ── adsb.lol poll ────────────────────────────────────────────────────────────
void poll_positions(float home_lat, float home_lon, uint16_t range_km) {
    float range_nm = range_km / KM_PER_NM;
    range_nm = constrain(range_nm, 5.0f, 250.0f);

    char url[128];
    // ceil, not round: a 10 km range is 5.4 nm — rounding down to 5 nm would
    // silently drop aircraft in the outer ~7% of the scope.
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%u",
             home_lat, home_lon, (unsigned)ceilf(range_nm));

    JsonDocument filter;
    JsonObject f = filter["ac"].add<JsonObject>();
    f["flight"]    = true;
    f["lat"]       = true;
    f["lon"]       = true;
    f["alt_baro"]  = true;
    f["gs"]        = true;
    f["track"]     = true;
    f["baro_rate"] = true;
    f["squawk"]    = true;

    JsonDocument doc(&psram_alloc);
    if (!http_get_json(url, doc, filter)) {
        cur_status = have_data ? Status::Ok : Status::Error;
        return;
    }

    // Parse all returned aircraft, keep the nearest MAX_AIRCRAFT (insertion
    // sort into a fixed buffer — the response can list hundreds).
    Aircraft local[MAX_AIRCRAFT];
    size_t n = 0;
    float cos_lat = cosf(home_lat * DEG2RAD);

    for (JsonObjectConst ac : doc["ac"].as<JsonArrayConst>()) {
        if (!ac["lat"].is<float>() || !ac["lon"].is<float>()) continue;

        Aircraft a = {};
        a.x_km = KM_PER_DEG * (ac["lon"].as<float>() - home_lon) * cos_lat;
        a.y_km = KM_PER_DEG * (ac["lat"].as<float>() - home_lat);
        a.dist_km = sqrtf(a.x_km * a.x_km + a.y_km * a.y_km);
        if (a.dist_km > (float)range_km) continue;

        strlcpy(a.callsign, ac["flight"] | "", sizeof(a.callsign));
        for (int i = strlen(a.callsign) - 1; i >= 0 && a.callsign[i] == ' '; i--)
            a.callsign[i] = '\0';

        JsonVariantConst alt = ac["alt_baro"];
        if (alt.is<const char*>()) {           // "ground"
            a.on_ground = true;
        } else {
            a.alt_ft = alt | 0;
        }
        a.gs_kt     = (int16_t)lroundf(ac["gs"] | 0.0f);
        a.track_deg = (int16_t)lroundf(ac["track"] | 0.0f);
        a.baro_rate = (int16_t)constrain((int)(ac["baro_rate"] | 0), -32000, 32000);
        const char* sq = ac["squawk"] | "";
        a.emergency = strcmp(sq, "7500") == 0 || strcmp(sq, "7600") == 0 ||
                      strcmp(sq, "7700") == 0;

        if (const RouteEntry* r = find_route(a.callsign))
            strlcpy(a.route, r->route, sizeof(a.route));

        // insert sorted by distance
        size_t pos = n;
        while (pos > 0 && local[pos - 1].dist_km > a.dist_km) pos--;
        if (pos >= MAX_AIRCRAFT) continue;
        size_t last = (n < MAX_AIRCRAFT) ? n : MAX_AIRCRAFT - 1;
        for (size_t i = last; i > pos; i--) local[i] = local[i - 1];
        local[pos] = a;
        if (n < MAX_AIRCRAFT) n++;
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    memcpy(aircraft, local, sizeof(local));
    aircraft_n = n;
    xSemaphoreGive(mutex);

    last_ok_ms = millis();
    have_data  = true;
    cur_status = Status::Ok;
    log_i("[radar] %u aircraft within %u km", (unsigned)n, range_km);
}

// ── adsbdb route lookups ─────────────────────────────────────────────────────
// Resolve up to `budget` not-yet-cached callsigns per poll cycle, nearest first.
void resolve_routes(int budget) {
    Aircraft local[MAX_AIRCRAFT];
    size_t n = get_aircraft(local, MAX_AIRCRAFT);

    for (size_t i = 0; i < n && budget > 0; i++) {
        const char* cs = local[i].callsign;
        if (!cs[0] || find_route(cs)) continue;
        budget--;

        char url[96];
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);

        JsonDocument filter;
        filter["response"]["flightroute"]["origin"]["iata_code"]      = true;
        filter["response"]["flightroute"]["destination"]["iata_code"] = true;

        JsonDocument doc;
        if (!http_get_json(url, doc, filter)) {
            // 404 (= unknown callsign) lands here too — cache empty so we
            // don't retry it every cycle; transient network errors pay the
            // same price but a cache slot recycles quickly.
            cache_route(cs, "");
            continue;
        }
        const char* from = doc["response"]["flightroute"]["origin"]["iata_code"] | "";
        const char* to   = doc["response"]["flightroute"]["destination"]["iata_code"] | "";

        char route[16] = "";
        if (from[0] && to[0]) snprintf(route, sizeof(route), "%s>%s", from, to);
        cache_route(cs, route);
        log_i("[radar] route %s = %s", cs, route[0] ? route : "(unknown)");

        // patch the shared list so the UI shows it without waiting a cycle
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (size_t k = 0; k < aircraft_n; k++)
            if (strcmp(aircraft[k].callsign, cs) == 0)
                strlcpy(aircraft[k].route, route, sizeof(aircraft[k].route));
        xSemaphoreGive(mutex);
    }
}

// ── Poller task ──────────────────────────────────────────────────────────────
void task_fn(void*) {
    for (;;) {
        const auto& cfg = settings::state();

        if (cfg.mode != settings::Mode::Radar) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (cfg.radar.lat == 0.0f && cfg.radar.lon == 0.0f) {
            cur_status = Status::NoLocation;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            cur_status = Status::NoWifi;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!have_data) cur_status = Status::Scanning;

        poll_positions(cfg.radar.lat, cfg.radar.lon, cfg.radar.range_km);
        resolve_routes(2);

        // Sleep the poll interval in slices so mode/location changes react fast.
        uint32_t waited = 0, wait_ms = max<uint16_t>(cfg.radar.poll_s, 5) * 1000u;
        while (waited < wait_ms && settings::state().mode == settings::Mode::Radar) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
    }
}

}  // namespace

void begin() {
    if (mutex) return;
    mutex = xSemaphoreCreateMutex();
    // 10 KB stack: TLS handshake + ArduinoJson parsing both run here.
    xTaskCreatePinnedToCore(task_fn, "radar", 10240, nullptr, 1, nullptr, 0);
    log_i("[radar] poller task started on core 0");
}

size_t get_aircraft(Aircraft* out, size_t cap) {
    if (!mutex) return 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    size_t n = min(aircraft_n, cap);
    memcpy(out, aircraft, n * sizeof(Aircraft));
    xSemaphoreGive(mutex);
    return n;
}

Status status() { return cur_status; }

uint32_t data_age_ms() {
    return have_data ? millis() - last_ok_ms : UINT32_MAX;
}

}  // namespace radar
