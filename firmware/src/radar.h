// Flight data fetcher for Radar mode.
//
// Runs an HTTPS poller in its own FreeRTOS task (core 0) so TLS handshakes
// never block the LVGL render loop on core 1:
//   - aircraft positions:  https://api.adsb.lol/v2/point/{lat}/{lon}/{nm}
//   - callsign → route:    https://api.adsbdb.com/v0/callsign/{cs}  (cached)
//
// The UI thread pulls a consistent snapshot via get_aircraft(); nothing in
// here touches LVGL.
#pragma once

#include <Arduino.h>

namespace radar {

constexpr size_t MAX_AIRCRAFT = 12;   // nearest N kept, sorted by distance

struct Aircraft {
    char    callsign[12];   // trimmed, e.g. "BAW172"
    char    route[16];      // "JFK>LHR", "" while unresolved/unknown
    // Position is cartesian relative to home so the UI can dead-reckon
    // between polls: pos(t) = (x_km, y_km) + v * data_age.
    float   x_km;           // east of home
    float   y_km;           // north of home
    float   dist_km;        // at fetch time (sorting key)
    int32_t alt_ft;         // barometric altitude
    int16_t gs_kt;          // ground speed
    int16_t track_deg;      // course over ground, true, 0 = north
    int16_t baro_rate;      // ft/min, + climbing
    bool    on_ground;
    bool    emergency;      // squawk 7500 / 7600 / 7700
};

enum class Status : uint8_t {
    NoLocation,   // radar.lat/lon not configured
    NoWifi,       // not connected as STA
    Scanning,     // configured, no successful fetch yet
    Ok,           // data is fresh
    Error,        // last fetch failed (will retry)
};

void begin();   // start the poller task; safe to call once from setup()

// Copy up to `cap` aircraft into `out`; returns the count.
size_t get_aircraft(Aircraft* out, size_t cap);

Status   status();
uint32_t data_age_ms();   // ms since last successful fetch (UINT32_MAX if never)

}  // namespace radar
