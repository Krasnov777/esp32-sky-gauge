// LVGL-backed display module — boots the GC9A01, owns LVGL, drives backlight.
#pragma once

#include <Arduino.h>

namespace display {

// One-time init: creates SPI bus, panel, backlight, LVGL display, draw buffers.
// Must be called before any lv_* calls.
void begin();

// Pump LVGL — call frequently from loop().
void tick();

// Backlight: 0..255 (PWM duty).
void set_brightness(uint8_t value);
uint8_t brightness();

}  // namespace display
