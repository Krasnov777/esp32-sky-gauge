// LVGL screens for each display mode, plus an internal boot/status screen.
// All public functions assume LVGL has been initialised (display::begin() first).
#pragma once

#include "settings.h"

namespace ui {

void begin();

// Switch active mode (Radar / Weather).
void set_mode(settings::Mode m);

// Re-apply current settings: rebuilds all screens (theme / range / labels)
// and loads the screen for settings::state().mode. Call on the LVGL thread.
void apply_settings();

// Boot/status screen — shown during startup so network messages are readable.
void show_status();
void update_status(const char* line1, const char* line2);

}  // namespace ui
