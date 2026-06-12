// On-demand screen capture, served by the web server as /shot.bmp.
//
// LVGL is single-threaded (core 1), but HTTP handlers run on the AsyncTCP
// task — so a handler only *requests* a capture; loop_tick() (called from
// the main loop, same thread as LVGL) renders the active screen into a
// PSRAM buffer via lv_snapshot and encodes it as a 24-bit BMP.
#pragma once

#include <Arduino.h>

namespace shot {

// Ask for a capture (safe from any thread). Returns false if buffers
// couldn't be allocated.
bool request();

// Perform a pending capture. Call from the LVGL thread.
void loop_tick();

// Last capture as a complete BMP file; nullptr if none taken yet.
const uint8_t* bmp(size_t& len_out);

uint32_t age_ms();   // ms since last capture (UINT32_MAX if never)

}  // namespace shot
