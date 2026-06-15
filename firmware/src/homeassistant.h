// Home Assistant entity poller for Home Integration mode.
//
// Mirrors the radar/weather modules: a mode-gated FreeRTOS task on core 0
// fetches each configured page's entity from HA's REST API
// (GET {url}/api/states/{entity} with a long-lived Bearer token) and
// publishes a mutex-guarded snapshot the UI reads. One entity per page; the
// value's unit comes from the entity's unit_of_measurement attribute.
#pragma once

#include <Arduino.h>
#include "settings.h"

namespace homeassistant {

struct Tile {
    bool   ok;            // fetch succeeded
    char   value[20];     // raw state string (e.g. "21.5", "on")
    char   unit[12];      // unit_of_measurement (e.g. "°C", "%", "W"); "" if none
    bool   configured;    // entity[i] is non-empty
};

struct Snapshot {
    Tile tiles[settings::HOME_TILES];
    bool any;             // at least one page configured
};

enum class Status : uint8_t {
    NoConfig,   // url/token/entities not set
    NoWifi,
    Fetching,   // no successful fetch yet
    Ok,
    Error,      // last cycle had failures
};

void begin();
Snapshot get();
Status   status();
uint32_t data_age_ms();   // ms since last successful cycle (UINT32_MAX if never)

}  // namespace homeassistant
