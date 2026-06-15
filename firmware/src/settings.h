// Persistent configuration backed by NVS (Preferences).
// All write operations are debounced + flushed by save().
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace settings {

enum class Mode : uint8_t {
    Radar = 0,    // live flight radar (adsb.lol) around the home location
    Weather = 1,  // current conditions (buienradar/Open-Meteo) at the same location
    Auto = 2,     // weather by default; switches to radar while traffic is
                  // within radar.auto_km, back 30 s after the sky clears
    Home = 3,     // Home Assistant entity tiles (REST /api/states + token)
};

constexpr int HOME_TILES = 5;   // one HA entity per page; 5 pages on the round screen

struct HomeConfig {
    // Home Assistant REST integration. Token is a long-lived access token —
    // write-only: never returned by to_json() (a `token_set` bool is exposed
    // instead). url is e.g. "http://homeassistant.local:8123" (use the IP if
    // the .local name doesn't resolve from the ESP).
    char url[96]     = "";
    char token[224]  = "";       // HA long-lived tokens are ~180-char JWTs
    uint16_t poll_s  = 15;       // entity refresh interval

    // Per-tile config. `type` is a preset key bundling unit + decimals +
    // whether a secondary (humidity-style) value is shown. `icon` selects a
    // drawn glyph ("auto" = derive from type). entity2 is optional.
    char type[HOME_TILES][16]    = {"temperature", "temperature", "humidity", "power", "battery"};
    char icon[HOME_TILES][16]    = {"auto", "auto", "auto", "auto", "auto"};
    char label[HOME_TILES][20]   = {"Living", "Bedroom", "Humidity", "Power", "Battery"};
    char entity[HOME_TILES][48]  = {"", "", "", "", ""};
    char entity2[HOME_TILES][48] = {"", "", "", "", ""};
};

struct RadarConfig {
    // lat == 0 && lon == 0 is the "not configured" sentinel (it's in the
    // Gulf of Guinea — if you live there, nudge a coordinate by 0.0001).
    // The location is shared by both modes: radar scope and weather.
    float    lat        = 0.0f;
    float    lon        = 0.0f;
    uint16_t range_km   = 100;    // scope radius
    uint16_t poll_s     = 10;     // adsb.lol poll interval
    bool     show_tags  = true;   // callsign label next to each blip
    uint8_t  theme      = 0;      // 0 = green phosphor, 1 = amber
    uint16_t alert_km   = 3;      // pin focus + pulse when traffic this close; 0 = off
    uint16_t auto_km    = 5;      // Auto mode: show radar while traffic this close; 0 = off
    uint8_t  auto_base  = 0;      // Auto resting screen: 0 = Weather, 1 = Home
};

struct Snapshot {
    // network
    char     wifi_ssid[33]    = "";
    char     wifi_password[65]= "";
    char     hostname[33]     = "esp-gauge";

    // display
    Mode     mode             = Mode::Radar;
    uint8_t  brightness       = 255;

    RadarConfig radar;
    HomeConfig  home;
};

// Lifecycle
void begin();
void save();          // flush in-memory state to NVS
void reset_to_defaults();

// Access — returned reference is mutable; call save() to persist.
Snapshot& state();

// Apply a JSON patch onto the snapshot. Returns true if anything changed.
// Recognized keys: mode, brightness,
//                  radar.{lat,lon,range_km,poll_s,show_tags,theme,alert_km,auto_km},
//                  home.{url,token,poll_s,tiles:[{type,icon,label,entity,entity2}]},
//                  wifi.{ssid,password,hostname}.
bool apply_json(JsonVariantConst patch);

// Serialize current state into the given JSON object (passwords redacted by
// default). The object may be the root of a document or a nested key.
void to_json(JsonObject out, bool include_secrets = false);

}  // namespace settings
