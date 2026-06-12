#include "web_server.h"
#include "settings.h"
#include "ui.h"
#include "display.h"
#include "radar.h"
#include "screenshot.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>

namespace web {
namespace {

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// NVS writes are debounced: a brightness-slider drag arrives as dozens of
// config patches per second, and each save() walks every key. Settings apply
// live immediately; the flash write happens after the burst settles.
bool     save_pending = false;
uint32_t save_request_ms = 0;
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;

void flush_save() {
    if (save_pending) {
        save_pending = false;
        settings::save();
    }
}

// LVGL is single-threaded and lives on core 1 (main loop). Config patches
// arrive on the AsyncTCP task, so touching ui::* from the handlers races the
// radar screen's 33 ms lv_timer — screen loads get clobbered and mode
// switches silently fail. Handlers only set this flag; loop_tick() (same
// thread as lv_timer_handler) applies the UI change.
volatile bool ui_refresh_pending = false;

void send_state_to(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"] = "state";
    JsonObject data = doc["data"].to<JsonObject>();
    settings::to_json(data, false);

    String out;
    serializeJson(doc, out);
    client->text(out);
}

void broadcast_all_state_inline() {
    JsonDocument doc;
    doc["type"] = "state";
    JsonObject data = doc["data"].to<JsonObject>();
    settings::to_json(data, false);

    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

// Apply a config patch. Brightness-only patches (slider drags) skip the
// screen rebuild — they'd reset animation state 30×/second for no visual
// difference, since brightness is pure backlight PWM.
void apply_config_patch(JsonVariantConst patch) {
    if (!settings::apply_json(patch)) return;

    save_pending = true;
    save_request_ms = millis();
    display::set_brightness(settings::state().brightness);

    JsonObjectConst o = patch.as<JsonObjectConst>();
    bool only_brightness = o.size() == 1 && !o["brightness"].isNull();
    if (!only_brightness) {
        ui_refresh_pending = true;   // applied on the LVGL thread in loop_tick()
    }
    broadcast_all_state_inline();
}

void handle_ws_message(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        log_w("[ws] bad JSON: %s", err.c_str());
        return;
    }

    const char* type = doc["type"] | "";

    if (strcmp(type, "config") == 0) {
        apply_config_patch(doc["patch"]);
        return;
    }

    if (strcmp(type, "hello") == 0) {
        send_state_to(client);
        return;
    }

    if (strcmp(type, "command") == 0) {
        const char* cmd = doc["cmd"] | "";
        if (strcmp(cmd, "reboot") == 0) {
            log_i("[ws] reboot requested");
            flush_save();   // don't lose a debounced config write
            delay(100);
            ESP.restart();
        } else if (strcmp(cmd, "factory_reset") == 0) {
            settings::reset_to_defaults();
            delay(100);
            ESP.restart();
        }
        return;
    }
}

void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                 void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            log_i("[ws] client #%u connected from %s", client->id(),
                  client->remoteIP().toString().c_str());
            send_state_to(client);
            break;
        case WS_EVT_DISCONNECT:
            log_i("[ws] client #%u disconnected", client->id());
            break;
        case WS_EVT_DATA: {
            auto* info = static_cast<AwsFrameInfo*>(arg);
            if (info->final && info->index == 0 && info->len == len &&
                info->opcode == WS_TEXT) {
                handle_ws_message(client, data, len);
            }
            break;
        }
        default:
            break;
    }
}

void register_routes() {
    // Static files from LittleFS — index.html, app.js, style.css.
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html")
          .setCacheControl("max-age=600");

    // REST endpoints for the web UI (HTTP fallback to WS-driven flow).
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        settings::to_json(root, false);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    auto* patchHandler = new AsyncCallbackJsonWebHandler(
        "/api/state",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            apply_config_patch(json);
            JsonDocument doc;
            JsonObject root = doc.to<JsonObject>();
            settings::to_json(root, false);
            String out;
            serializeJson(doc, out);
            req->send(200, "application/json", out);
        });
    server.addHandler(patchHandler);

    // Screen capture: POST /api/shot requests one (taken on the LVGL thread
    // a few ms later); GET /shot.bmp serves the most recent capture.
    server.on("/api/shot", HTTP_POST, [](AsyncWebServerRequest* req) {
        bool ok = shot::request();
        req->send(ok ? 200 : 507, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    server.on("/shot.bmp", HTTP_GET, [](AsyncWebServerRequest* req) {
        size_t len = 0;
        const uint8_t* data = shot::bmp(len);
        if (!data) {
            req->send(404, "text/plain",
                      "No capture yet - POST /api/shot first, then retry.");
            return;
        }
        AsyncWebServerResponse* res =
            req->beginResponse(200, "image/bmp", data, len);
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        flush_save();
        delay(100);
        ESP.restart();
    });

    server.on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        settings::reset_to_defaults();
        req->send(200, "application/json", "{\"ok\":true}");
        delay(100);
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) { req->send(200); return; }
        req->send(404, "text/plain", "Not found");
    });
}

}  // namespace

void begin() {
    if (!LittleFS.begin(false)) {
        log_e("LittleFS mount failed — did you upload the filesystem image?");
    }

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);
    register_routes();

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server.begin();
    log_i("HTTP server started on :80");
}

void loop_tick() {
    ws.cleanupClients();

    if (ui_refresh_pending) {
        ui_refresh_pending = false;
        ui::apply_settings();   // rebuilds + loads the screen for the new mode
    }

    shot::loop_tick();          // pending screen captures (LVGL thread)

    if (save_pending && millis() - save_request_ms >= SAVE_DEBOUNCE_MS) {
        flush_save();
    }

    // In Radar/Auto mode, push the aircraft list to web clients every 2 s so
    // the browser can render its own scope (it dead-reckons between pushes).
    static uint32_t last_radar_ms = 0;
    bool radar_active = settings::state().mode == settings::Mode::Radar ||
                        settings::state().mode == settings::Mode::Auto;
    if (radar_active && ws.count() > 0) {
        uint32_t now = millis();
        if (now - last_radar_ms >= 2000) {
            last_radar_ms = now;
            radar::Aircraft ac[radar::MAX_AIRCRAFT];
            size_t n = radar::get_aircraft(ac, radar::MAX_AIRCRAFT);

            JsonDocument doc;
            doc["type"]  = "radar";
            doc["age"]   = radar::data_age_ms();
            doc["range"] = settings::state().radar.range_km;
            JsonArray arr = doc["ac"].to<JsonArray>();
            for (size_t i = 0; i < n; i++) {
                JsonObject o = arr.add<JsonObject>();
                o["cs"]  = ac[i].callsign;
                o["rt"]  = ac[i].route;
                o["x"]   = ac[i].x_km;
                o["y"]   = ac[i].y_km;
                o["alt"] = ac[i].alt_ft;
                o["gs"]  = ac[i].gs_kt;
                o["trk"] = ac[i].track_deg;
                o["vr"]  = ac[i].baro_rate;
                o["gnd"] = ac[i].on_ground;
                o["emg"] = ac[i].emergency;
            }
            String out;
            serializeJson(doc, out);
            ws.textAll(out);
        }
    }
}

void broadcast_state() { broadcast_all_state_inline(); }

}  // namespace web
