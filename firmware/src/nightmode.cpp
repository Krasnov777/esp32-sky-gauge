#include "nightmode.h"

#include "display.h"
#include "radar.h"
#include "settings.h"

#include <time.h>

namespace nightmode {
namespace {

constexpr uint32_t WAKE_MS        = 20000;   // how long a web-UI wake lasts
constexpr uint8_t  WAKE_BRIGHT    = 40;      // readable in a dark room
constexpr uint32_t TICK_MS        = 1000;    // the hour does not move fast
constexpr uint32_t EMERG_FRESH_MS = 60000;   // a stale contact must not hold the panel on
const char* const  NTP_SERVER     = "pool.ntp.org";

uint32_t last_wake_ms = 0;
uint32_t last_tick_ms = 0;
bool     is_blanked   = false;

// getLocalTime() with a 0 ms budget runs its body exactly once instead of
// blocking for five seconds — this is called from the LVGL thread.
bool local_now(struct tm& t) { return getLocalTime(&t, 0); }

}  // namespace

bool time_valid() {
    struct tm t;
    return local_now(t);
}

bool in_window() {
    const auto& s = settings::state();
    if (s.night_from == s.night_to) return false;      // from == to disables
    struct tm t;
    if (!local_now(t)) return false;                   // no clock, no blanking
    // A window that wraps midnight (23 → 7) is the normal case, so the
    // comparison has to handle from > to.
    return (s.night_from < s.night_to)
               ? (t.tm_hour >= s.night_from && t.tm_hour < s.night_to)
               : (t.tm_hour >= s.night_from || t.tm_hour < s.night_to);
}

bool emergency_override() {
    if (!in_window()) return false;
    // The flag is not cleared by a failed poll, so pair it with the data age:
    // an emergency seen ten minutes ago must not pin the backlight on.
    return radar::emergency() && radar::data_age_ms() < EMERG_FRESH_MS;
}

void refresh() {
    const auto& s = settings::state();
    bool night = in_window();
    bool awake = (millis() - last_wake_ms) < WAKE_MS;

    uint8_t want;
    if (!night) {
        want = s.brightness;
    } else if (emergency_override()) {
        want = s.brightness;            // full: something is actually wrong up there
    } else if (awake) {
        // Never brighter than the configured day level — if the user runs the
        // gauge at 20, a "wake" at 40 would be a step up, not down.
        want = s.brightness < WAKE_BRIGHT ? s.brightness : WAKE_BRIGHT;
    } else {
        want = s.night_bright;          // 0 = backlight off
    }

    is_blanked = night && want == 0;
    display::set_brightness(want);
}

void wake() {
    last_wake_ms = millis();
    refresh();
}

void tick() {
    uint32_t now = millis();
    if (now - last_tick_ms < TICK_MS) return;
    last_tick_ms = now;
    refresh();
}

void begin() {
    // Sets TZ and starts SNTP in one call.
    configTzTime(settings::state().tz, NTP_SERVER);
    last_wake_ms = millis();    // the boot screen stays readable for WAKE_MS
    refresh();
}

bool blanked() { return is_blanked; }

}  // namespace nightmode
