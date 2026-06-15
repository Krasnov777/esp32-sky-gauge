// Home Assistant entity poller for Home mode.
//
// Mirrors the radar/weather modules: a mode-gated FreeRTOS task on core 0
// fetches each configured tile's entity state from HA's REST API
// (GET {url}/api/states/{entity} with a long-lived Bearer token) and
// publishes a mutex-guarded snapshot the UI reads. The integration mirrors
// the LilyGo T5 "Frame" project's Home mode.
#pragma once

#include <Arduino.h>
#include "settings.h"

namespace homeassistant {

struct Tile {
    bool   ok;            // primary fetch succeeded
    float  value;         // numeric state (0 if non-numeric)
    char   raw[20];       // raw state string (for non-numeric entities / "custom")
    bool   sec_ok;        // secondary fetch succeeded
    float  sec_value;     // secondary numeric state
    bool   configured;    // entity[i] is non-empty
};

struct Snapshot {
    Tile tiles[settings::HOME_TILES];
    bool any;             // at least one tile configured
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

// Preset metadata for a tile `type` key (unit, decimals, secondary flag).
struct TypeInfo { const char* unit; int decimals; bool secondary; };
TypeInfo type_info(const char* key);

}  // namespace homeassistant
