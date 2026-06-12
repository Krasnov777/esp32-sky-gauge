// HTTP + WebSocket server (AsyncWebServer/AsyncWebSocket on LittleFS).
#pragma once

namespace web {

void begin();        // start server on port 80 (call after WiFi is up)
void loop_tick();    // cleanup dead WS clients, flush debounced saves,
                     // push radar broadcasts

// Broadcast a fresh state snapshot to all subscribers (called after settings
// change so the web UI reflects what's on the device).
void broadcast_state();

}  // namespace web
