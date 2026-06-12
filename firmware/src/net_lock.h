// Shared TLS serialization lock.
//
// A TLS handshake needs ~45 KB of free internal heap. The radar and weather
// pollers are mode-gated so they rarely overlap — but during a mode switch
// one task can still be mid-fetch while the other starts, and two concurrent
// handshakes exhaust the heap (mbedtls -32512). Holding this lock around any
// HTTPS request keeps at most one TLS connection alive at a time.
#pragma once

#include <Arduino.h>

namespace netlock {

inline SemaphoreHandle_t handle() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

struct Guard {
    Guard()  { xSemaphoreTake(handle(), portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(handle()); }
};

}  // namespace netlock
