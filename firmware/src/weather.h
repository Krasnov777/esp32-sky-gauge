// Current-conditions fetcher for Weather mode.
//
// Primary source: buienradar.nl — actual measurements from the nearest
// KNMI/Buienradar station (NL/BE coverage), with authentic Dutch condition
// text. Falls back to Open-Meteo (global, model-based) when no station is
// within range. Both free, no API key. Mirrors the radar module's pattern:
// a mode-gated FreeRTOS task on core 0, mutex-guarded snapshot, Status enum.
// Refreshes every 10 minutes; uses the location from settings radar config.
#pragma once

#include <Arduino.h>

namespace weather {

// Icon classes the UI can draw (LVGL-primitive icon groups, in this order).
enum class Icon : uint8_t { Sun = 0, Partly, Cloud, Rain, Snow, Thunder, Fog };
constexpr uint8_t ICON_COUNT = 7;

struct Current {
    float   temp_c;
    float   feels_c;
    uint8_t humidity;       // %
    float   wind_kmh;
    int16_t wind_dir_deg;   // meteorological, 0 = from north
    Icon    icon;
    char    desc[28];       // condition text (Dutch from buienradar, English from fallback)
    bool    valid;
};

enum class Status : uint8_t {
    NoLocation,   // radar.lat/lon not configured
    NoWifi,
    Fetching,     // no successful fetch yet
    Ok,
    Error,        // last fetch failed (will retry)
};

void begin();
Current  get();
Status   status();
uint32_t data_age_ms();   // ms since last successful fetch (UINT32_MAX if never)

}  // namespace weather
