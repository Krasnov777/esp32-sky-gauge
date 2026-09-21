// Scheduled backlight blanking, and the single authority on what the backlight
// is actually set to.
//
// This panel is a GC9A01 IPS LCD, not an OLED: none of this is about burn-in,
// which an LCD does not suffer. The window exists so the gauge stops lighting
// the room at 3 a.m. and stops spending backlight power — by far the largest
// term in this board's draw — on an audience of nobody.
//
// The device has no buttons, so the only manual way back in is wake(), wired
// to the web UI. Inside the window one thing is allowed to light the panel on
// its own: a fresh emergency squawk on the radar.
#pragma once

#include <Arduino.h>

namespace nightmode {

// Starts SNTP with the configured POSIX TZ. Call after WiFi is up; the first
// sync lands a few seconds later and in_window() stays false until it does.
void begin();

// Cheap — re-evaluates about once a second. Call from loop().
void tick();

// Re-apply the effective backlight level. Every path that wants to change
// brightness goes through here or wake(), never display::set_brightness()
// directly, or a slider drag would light the panel mid-window.
void refresh();

// Light the panel for WAKE_MS at a level that reads in the dark.
void wake();

bool time_valid();          // SNTP has produced a plausible clock
bool in_window();           // inside the configured hours (false without a clock)
bool blanked();             // window active and the backlight currently held down

// A fresh emergency squawk (7500/7600/7700) inside the window. Lights the
// panel, and tells the Auto supervisor to show the scope rather than a resting
// screen — otherwise the wake would have nothing to show.
bool emergency_override();

}  // namespace nightmode
