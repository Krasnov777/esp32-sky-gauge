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
                  // within radar.alert_km, back 30 s after the sky clears
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
};

// Lifecycle
void begin();
void save();          // flush in-memory state to NVS
void reset_to_defaults();

// Access — returned reference is mutable; call save() to persist.
Snapshot& state();

// Apply a JSON patch onto the snapshot. Returns true if anything changed.
// Recognized keys: mode, brightness,
//                  radar.{lat,lon,range_km,poll_s,show_tags,theme,alert_km},
//                  wifi.{ssid,password,hostname}.
bool apply_json(JsonVariantConst patch);

// Serialize current state into the given JSON object (passwords redacted by
// default). The object may be the root of a document or a nested key.
void to_json(JsonObject out, bool include_secrets = false);

}  // namespace settings
