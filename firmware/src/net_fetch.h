// Shared HTTPS JSON fetcher for the data pollers (radar + weather).
//
// - Serializes TLS connections via net_lock (heap can't fit two handshakes).
// - Buffers the whole response body into a reused PSRAM buffer before
//   parsing: large bodies (adsb.lol near a hub, the buienradar full feed)
//   parsed byte-wise off the TLS stream are slow enough that servers drop
//   the connection mid-body.
// - Parses with an ArduinoJson filter so only the requested fields survive.
#pragma once

#include <ArduinoJson.h>

namespace net {

// GET `url`, parse the body into `doc` keeping only fields set in `filter`.
// Returns false on connection/HTTP/parse failure (already logged).
bool http_get_json(const char* url, JsonDocument& doc, JsonDocument& filter);

// GET `url` and copy the (small, plain-text) body into `out`. Handles both
// http and https by scheme. Pass `bearer` to send an Authorization: Bearer
// header (for Home Assistant's REST API). Returns false on connection/HTTP failure.
bool http_get_text(const char* url, char* out, size_t cap, const char* bearer = nullptr);

// Allocator that prefers PSRAM — pass to JsonDocument for large documents.
ArduinoJson::Allocator* psram_allocator();

}  // namespace net
