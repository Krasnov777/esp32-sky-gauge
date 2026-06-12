// Current-conditions fetcher for Weather mode.
//
// Mirrors the radar module's pattern: a FreeRTOS task on core 0 polls
// Open-Meteo (free, no API key, ~1 KB responses) while Weather mode is
// active, using the location from settings::state().radar. Refreshes every
// 10 minutes; the UI thread reads a copy via get().
#pragma once

#include <Arduino.h>

namespace weather {

struct Current {
    float   temp_c;
    float   feels_c;
    uint8_t humidity;       // %
    float   wind_kmh;
    int16_t wind_dir_deg;   // meteorological, 0 = from north
    uint8_t code;           // WMO weather code
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
