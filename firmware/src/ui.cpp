#include "ui.h"
#include "board_config.h"
#include "radar.h"
#include "weather.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

namespace ui {
namespace {

// ── Geometry (centered 240×240) ──────────────────────────────────────────────
constexpr int CX = board::LCD_WIDTH / 2;
constexpr int CY = board::LCD_HEIGHT / 2;

// ── Palette ──────────────────────────────────────────────────────────────────
constexpr lv_color_t COL_BG        = LV_COLOR_MAKE(0,   0,   0);
constexpr lv_color_t COL_GREEN     = LV_COLOR_MAKE(0,   255, 70);
constexpr lv_color_t COL_DIM_GREEN = LV_COLOR_MAKE(0,   95,  32);
constexpr lv_color_t COL_AMBER     = LV_COLOR_MAKE(255, 176, 0);
constexpr lv_color_t COL_DIM_AMBER = LV_COLOR_MAKE(120, 78,  0);
constexpr lv_color_t COL_RED       = LV_COLOR_MAKE(255, 35,  25);
constexpr lv_color_t COL_TEXT      = LV_COLOR_MAKE(255, 255, 255);
constexpr lv_color_t COL_DIM_TEXT  = LV_COLOR_MAKE(150, 150, 150);
// Weather icon colors
constexpr lv_color_t COL_SUN       = LV_COLOR_MAKE(255, 205, 40);
constexpr lv_color_t COL_CLOUD     = LV_COLOR_MAKE(176, 188, 200);
constexpr lv_color_t COL_RAIN      = LV_COLOR_MAKE(79,  195, 247);
constexpr lv_color_t COL_SNOW      = LV_COLOR_MAKE(235, 245, 255);

// ── Shared state ─────────────────────────────────────────────────────────────
settings::Mode current = settings::Mode::Radar;
bool status_active = false;   // boot/status screen is on display

lv_obj_t* screen_status = nullptr;
lv_obj_t* screen_radar  = nullptr;
lv_obj_t* screen_wx     = nullptr;

// Which screen is (being) displayed. In Auto mode this differs from what the
// settings mode alone implies — the supervisor flips between weather and
// radar — so animation timers gate on this, not on `current`.
lv_obj_t* shown = nullptr;

void load_screen(lv_obj_t* target, bool anim = true) {
    if (shown == target) return;
    shown = target;
    if (anim) lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_FADE_IN, 220, 0, false);
    else      lv_screen_load(target);
}

// ── Helpers ──────────────────────────────────────────────────────────────────
// Skip identical text updates: lv_label_set_text always reallocates and
// invalidates, and the radar/weather screens refresh on a timer.
void set_text_if_changed(lv_obj_t* l, const char* t) {
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

// Scope phosphor color per theme (shared by radar scope furniture).
lv_color_t scope_col() {
    return settings::state().radar.theme == 1 ? COL_AMBER : COL_GREEN;
}
lv_color_t scope_dim() {
    return settings::state().radar.theme == 1 ? COL_DIM_AMBER : COL_DIM_GREEN;
}

// ── Boot / status screen ─────────────────────────────────────────────────────
lv_obj_t* st_title = nullptr;
lv_obj_t* st_line1 = nullptr;
lv_obj_t* st_line2 = nullptr;

void build_status_screen() {
    if (screen_status) {
        lv_obj_clean(screen_status);
    } else {
        screen_status = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen_status);
        lv_obj_set_style_bg_color(screen_status, COL_BG, 0);
        lv_obj_set_style_bg_opa(screen_status, LV_OPA_COVER, 0);
    }

    st_title = lv_label_create(screen_status);
    lv_obj_set_style_text_font(st_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(st_title, scope_col(), 0);
    lv_label_set_text(st_title, "SKY GAUGE");
    lv_obj_align(st_title, LV_ALIGN_CENTER, 0, -34);

    st_line1 = lv_label_create(screen_status);
    lv_obj_set_style_text_font(st_line1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st_line1, COL_TEXT, 0);
    lv_label_set_text(st_line1, "");
    lv_obj_align(st_line1, LV_ALIGN_CENTER, 0, 8);

    st_line2 = lv_label_create(screen_status);
    lv_obj_set_style_text_font(st_line2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st_line2, COL_DIM_TEXT, 0);
    lv_label_set_text(st_line2, "");
    lv_obj_align(st_line2, LV_ALIGN_CENTER, 0, 32);
}

// ── Flight radar screen ──────────────────────────────────────────────────────
// Retro PPI scope: dim range rings + crosshair, a bright sweep line with a
// dense fading trail rotating 8 s/rev, and one blip per aircraft. Blips flare
// when the sweep passes over them and decay back ("afterglow"). Positions are
// dead-reckoned between polls. Nearest-aircraft details cycle through the
// bottom readout unless the overhead alert pins one.

constexpr int    RADAR_R       = 108;          // outermost range ring radius (px)
// The afterglow wedge is built from contiguous filled pie sectors (lv_arc
// with arc_width == radius) instead of fanned lines — lines leave radial
// gaps that widen toward the rim, sectors tile perfectly by angle.
// Translucent full-radius arcs are expensive to composite, so the count is
// kept low and the wedge re-anchors in WEDGE_STEP_DEG increments; 12 sectors
// at 33 ms starved the whole device. There is no separate lead line — the
// brightest sector is the beam.
constexpr int    SWEEP_SECTORS = 6;
constexpr float  SECTOR_DEG    = 7.0f;         // wedge spans ~42°
constexpr float  WEDGE_STEP_DEG = 1.5f;        // ≈ one re-anchor per tick at 8 s/rev
constexpr int    WEDGE_R       = RADAR_R - 2;  // stay inside the outer ring
constexpr uint32_t SWEEP_PERIOD_MS = 8000;     // one revolution

lv_obj_t* rd_wedge[SWEEP_SECTORS] = {};        // afterglow sectors
lv_obj_t* rd_blip[radar::MAX_AIRCRAFT]  = {};
lv_obj_t* rd_tag[radar::MAX_AIRCRAFT]   = {};
lv_obj_t* rd_vec[radar::MAX_AIRCRAFT]   = {};  // heading vector per blip
lv_point_precise_t rd_vec_pts[radar::MAX_AIRCRAFT][2];
uint8_t   rd_glow[radar::MAX_AIRCRAFT]  = {};  // afterglow opacity per blip
float     rd_bearing[radar::MAX_AIRCRAFT] = {};// extrapolated, for afterglow
lv_obj_t* rd_ring_outer = nullptr;             // pulsed during overhead alert
lv_obj_t* rd_count  = nullptr;                 // "5 TRK" header
lv_obj_t* rd_focus1 = nullptr;                 // "BAW172  JFK>LHR"
lv_obj_t* rd_focus2 = nullptr;                 // "36000ft 480kt 23km"
lv_obj_t* rd_status = nullptr;                 // big center message when empty
lv_timer_t* rd_timer = nullptr;
float     rd_sweep_deg = 0.0f;
radar::Aircraft rd_ac[radar::MAX_AIRCRAFT];
size_t    rd_ac_n     = 0;
size_t    rd_focus_ix = 0;
bool      rd_alert    = false;                 // traffic inside alert_km right now

lv_obj_t* make_radar_ring(int radius) {
    lv_obj_t* ring = lv_obj_create(screen_radar);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, radius * 2, radius * 2);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, scope_dim(), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    return ring;
}

void build_radar_screen() {
    if (screen_radar) {
        lv_obj_clean(screen_radar);
    } else {
        screen_radar = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen_radar);
        lv_obj_set_style_bg_color(screen_radar, COL_BG, 0);
        lv_obj_set_style_bg_opa(screen_radar, LV_OPA_COVER, 0);
    }

    // Outer ring sits at the full range; inner rings at round distances.
    // Pick the largest step that yields 2-3 inner rings, falling back to the
    // largest that yields one (e.g. range 10 km → ring at 5 + rim at 10).
    rd_ring_outer = make_radar_ring(RADAR_R);
    const uint16_t range = settings::state().radar.range_km;
    int ring_step = 0, ring_fallback = 0;
    for (int s : {200, 100, 50, 25, 10, 5, 2, 1}) {
        int c = (range - 1) / s;   // multiples strictly below range
        if (c >= 2 && c <= 3) { ring_step = s; break; }
        if (c == 1 && !ring_fallback) ring_fallback = s;
    }
    if (!ring_step) ring_step = ring_fallback ? ring_fallback : range;
    for (int d = ring_step; d < range; d += ring_step)
        make_radar_ring((int)lroundf((float)RADAR_R * d / range));

    // Crosshair
    static lv_point_precise_t hpts[2], vpts[2];
    hpts[0] = {(lv_value_precise_t)(CX - RADAR_R), (lv_value_precise_t)CY};
    hpts[1] = {(lv_value_precise_t)(CX + RADAR_R), (lv_value_precise_t)CY};
    vpts[0] = {(lv_value_precise_t)CX, (lv_value_precise_t)(CY - RADAR_R)};
    vpts[1] = {(lv_value_precise_t)CX, (lv_value_precise_t)(CY + RADAR_R)};
    for (auto* pts : {hpts, vpts}) {
        lv_obj_t* ln = lv_line_create(screen_radar);
        lv_line_set_points(ln, pts, 2);
        lv_obj_set_style_line_color(ln, scope_dim(), 0);
        lv_obj_set_style_line_width(ln, 1, 0);
        lv_obj_set_style_line_opa(ln, LV_OPA_50, 0);
    }

    // Compass: "N" letter marks north (up = true north); ticks only at
    // E/S/W — a north tick would run right through the letter.
    static lv_point_precise_t tick_pts[4][2];
    constexpr int TICK_LEN = 8;
    for (int c = 1; c < 4; c++) {
        float rad = c * 90.0f * (float)M_PI / 180.0f - (float)M_PI / 2.0f;
        tick_pts[c][0] = {(lv_value_precise_t)(CX + (RADAR_R - TICK_LEN) * cosf(rad)),
                          (lv_value_precise_t)(CY + (RADAR_R - TICK_LEN) * sinf(rad))};
        tick_pts[c][1] = {(lv_value_precise_t)(CX + RADAR_R * cosf(rad)),
                          (lv_value_precise_t)(CY + RADAR_R * sinf(rad))};
        lv_obj_t* tk = lv_line_create(screen_radar);
        lv_line_set_points(tk, tick_pts[c], 2);
        lv_obj_set_style_line_color(tk, scope_col(), 0);
        lv_obj_set_style_line_width(tk, 2, 0);
        lv_obj_set_style_line_opa(tk, LV_OPA_70, 0);
    }
    lv_obj_t* north = lv_label_create(screen_radar);
    lv_obj_set_style_text_font(north, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(north, scope_col(), 0);
    lv_label_set_text(north, "N");
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 12);

    // Range marks where each ring crosses the east crosshair arm
    auto add_range_mark = [&](int dist_km, bool rim) {
        char t[16];
        snprintf(t, sizeof(t), rim ? "%dkm" : "%d", dist_km);
        lv_obj_t* l = lv_label_create(screen_radar);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, scope_col(), 0);
        lv_obj_set_style_text_opa(l, 150, 0);
        lv_label_set_text(l, t);
        int r_px = (int)lroundf((float)RADAR_R * dist_km / range);
        lv_obj_align(l, LV_ALIGN_CENTER, r_px - (rim ? 24 : 10), -10);
    };
    for (int d = ring_step; d < range; d += ring_step) add_range_mark(d, false);
    add_range_mark(range, true);

    // Sweep afterglow: contiguous translucent pie sectors with quadratically
    // decaying opacity (drawn first = under the blips). The brightest sector
    // IS the sweep's leading edge — no separate line, the gradient is the beam.
    for (int i = SWEEP_SECTORS - 1; i >= 0; i--) {
        lv_obj_t* a = lv_arc_create(screen_radar);
        lv_obj_set_size(a, WEDGE_R * 2, WEDGE_R * 2);
        lv_obj_center(a);
        lv_arc_set_rotation(a, 0);
        lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(a, WEDGE_R, LV_PART_MAIN);   // ring → full pie
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, scope_col(), LV_PART_MAIN);
        float t = 1.0f - (float)i / SWEEP_SECTORS;
        lv_obj_set_style_arc_opa(a, (lv_opa_t)(190.0f * t * t), LV_PART_MAIN);
        rd_wedge[i] = a;
    }

    // Center hub: a round cap over the origin point — the flat-ended sweep
    // lines otherwise overlap into a jagged square blob at the center.
    lv_obj_t* hub = lv_obj_create(screen_radar);
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, 13, 13);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, scope_col(), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);

    // Blip pool: heading vector (under) + dot + callsign tag
    for (size_t i = 0; i < radar::MAX_AIRCRAFT; i++) {
        rd_vec_pts[i][0] = {(lv_value_precise_t)CX, (lv_value_precise_t)CY};
        rd_vec_pts[i][1] = {(lv_value_precise_t)CX, (lv_value_precise_t)CY};
        rd_vec[i] = lv_line_create(screen_radar);
        lv_line_set_points(rd_vec[i], rd_vec_pts[i], 2);
        lv_obj_set_style_line_color(rd_vec[i], scope_col(), 0);
        lv_obj_set_style_line_width(rd_vec[i], 2, 0);
        lv_obj_add_flag(rd_vec[i], LV_OBJ_FLAG_HIDDEN);

        rd_blip[i] = lv_obj_create(screen_radar);
        lv_obj_remove_style_all(rd_blip[i]);
        lv_obj_set_size(rd_blip[i], 7, 7);
        lv_obj_set_style_radius(rd_blip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(rd_blip[i], scope_col(), 0);
        lv_obj_set_style_bg_opa(rd_blip[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(rd_blip[i], LV_OBJ_FLAG_HIDDEN);

        rd_tag[i] = lv_label_create(screen_radar);
        lv_obj_set_style_text_font(rd_tag[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rd_tag[i], scope_col(), 0);
        lv_obj_add_flag(rd_tag[i], LV_OBJ_FLAG_HIDDEN);
        rd_glow[i] = 110;
    }

    rd_count = lv_label_create(screen_radar);
    lv_obj_set_style_text_font(rd_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rd_count, COL_DIM_TEXT, 0);
    lv_label_set_text(rd_count, "");
    lv_obj_align(rd_count, LV_ALIGN_TOP_MID, 0, 30);

    rd_focus1 = lv_label_create(screen_radar);
    lv_obj_set_style_text_font(rd_focus1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rd_focus1, scope_col(), 0);
    lv_label_set_text(rd_focus1, "");
    lv_obj_align(rd_focus1, LV_ALIGN_BOTTOM_MID, 0, -38);

    rd_focus2 = lv_label_create(screen_radar);
    lv_obj_set_style_text_font(rd_focus2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rd_focus2, COL_DIM_TEXT, 0);
    lv_label_set_text(rd_focus2, "");
    lv_obj_align(rd_focus2, LV_ALIGN_BOTTOM_MID, 0, -20);

    rd_status = lv_label_create(screen_radar);
    lv_obj_set_style_text_font(rd_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rd_status, COL_DIM_TEXT, 0);
    lv_obj_set_style_text_align(rd_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(rd_status, "");
    lv_obj_align(rd_status, LV_ALIGN_CENTER, 0, -40);
}

// Re-aim the wedge sectors at the current sweep angle.
void radar_place_sweep() {
    // LVGL arc angles: 0° at 3 o'clock, clockwise. Our 0° is north/up.
    float lead = rd_sweep_deg - 90.0f;

    // Anchor in fixed steps so the arc-compositing cost stays bounded.
    static float last_anchor = -1000.0f;
    float anchor = floorf(lead / WEDGE_STEP_DEG) * WEDGE_STEP_DEG;
    if (anchor == last_anchor) return;
    last_anchor = anchor;
    for (int i = 0; i < SWEEP_SECTORS; i++) {
        float e = anchor - i * SECTOR_DEG;
        float s = e - SECTOR_DEG;
        s = fmodf(s + 720.0f, 360.0f);
        e = fmodf(e + 720.0f, 360.0f);
        lv_arc_set_bg_angles(rd_wedge[i], (lv_value_precise_t)s,
                             (lv_value_precise_t)e);   // wraps past 0 fine
    }
}

// Re-place blips, heading vectors and labels. Positions are dead-reckoned:
// last fetched position + ground-speed vector × data age, so blips glide
// between polls instead of jumping every poll_s seconds.
void radar_place_blips() {
    const auto& cfg = settings::state().radar;
    if (rd_focus_ix >= rd_ac_n) rd_focus_ix = 0;

    // Extrapolation time. Freeze stale data rather than flying blips blindly.
    uint32_t age = radar::data_age_ms();
    if (age > 60000) age = (age == UINT32_MAX) ? 0 : 60000;
    float dt_h = age / 3600000.0f;   // hours

    // Overhead alert: nearest airborne aircraft inside alert_km pins focus.
    rd_alert = false;
    if (cfg.alert_km > 0) {
        float best = 1e9f;
        for (size_t i = 0; i < rd_ac_n; i++) {
            if (rd_ac[i].on_ground) continue;
            if (rd_ac[i].dist_km < best && rd_ac[i].dist_km <= (float)cfg.alert_km) {
                best = rd_ac[i].dist_km;
                rd_focus_ix = i;
                rd_alert = true;
            }
        }
    }

    float live_dist[radar::MAX_AIRCRAFT] = {};

    for (size_t i = 0; i < radar::MAX_AIRCRAFT; i++) {
        if (i >= rd_ac_n) {
            lv_obj_add_flag(rd_blip[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(rd_tag[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(rd_vec[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const auto& a = rd_ac[i];

        // Dead-reckoned position (km east/north of home)
        float trk = a.track_deg * (float)M_PI / 180.0f;
        float v_kmh = (a.on_ground ? 0 : a.gs_kt) * 1.852f;
        float ex = a.x_km + v_kmh * dt_h * sinf(trk);
        float ey = a.y_km + v_kmh * dt_h * cosf(trk);
        live_dist[i] = sqrtf(ex * ex + ey * ey);
        rd_bearing[i] = atan2f(ex, ey) * 180.0f / (float)M_PI;
        if (rd_bearing[i] < 0) rd_bearing[i] += 360.0f;

        float scale = RADAR_R / (float)cfg.range_km;
        float px = ex * scale, py = -ey * scale;          // screen y grows down
        float r = sqrtf(px * px + py * py);
        if (r > RADAR_R) { px *= RADAR_R / r; py *= RADAR_R / r; }
        int x = CX + (int)lroundf(px);
        int y = CY + (int)lroundf(py);

        lv_color_t col = a.emergency ? COL_RED
                       : (i == rd_focus_ix ? COL_TEXT : scope_col());

        lv_obj_set_pos(rd_blip[i], x - 3, y - 3);
        lv_obj_set_style_bg_color(rd_blip[i], col, 0);
        lv_obj_remove_flag(rd_blip[i], LV_OBJ_FLAG_HIDDEN);

        // Heading vector: where it's going, length scaled by speed
        if (!a.on_ground && a.gs_kt > 30) {
            float len = 7.0f + min((int)a.gs_kt, 480) / 40.0f;   // 8..19 px
            rd_vec_pts[i][0] = {(lv_value_precise_t)x, (lv_value_precise_t)y};
            rd_vec_pts[i][1] = {(lv_value_precise_t)(x + len * sinf(trk)),
                                (lv_value_precise_t)(y - len * cosf(trk))};
            lv_line_set_points(rd_vec[i], rd_vec_pts[i], 2);
            lv_obj_set_style_line_color(rd_vec[i], col, 0);
            lv_obj_remove_flag(rd_vec[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(rd_vec[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (cfg.show_tags && a.callsign[0]) {
            set_text_if_changed(rd_tag[i], a.callsign);
            lv_obj_set_style_text_color(rd_tag[i], a.emergency ? COL_RED : scope_col(), 0);
            // keep the tag on screen: flip it to the left of the blip on the right half
            lv_obj_set_pos(rd_tag[i], x > CX ? x - 8 - 7 * (int)strlen(a.callsign) : x + 6,
                           y - 14);
            lv_obj_remove_flag(rd_tag[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(rd_tag[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%u TRK", (unsigned)rd_ac_n);
    set_text_if_changed(rd_count, buf);

    // Bottom readout: focused aircraft
    if (rd_ac_n == 0) {
        set_text_if_changed(rd_focus1, "");
        set_text_if_changed(rd_focus2, "");
        const char* msg = "";
        switch (radar::status()) {
            case radar::Status::NoLocation: msg = "SET LOCATION\nin web UI"; break;
            case radar::Status::NoWifi:     msg = "NO WIFI";     break;
            case radar::Status::Scanning:   msg = "SCANNING...."; break;
            case radar::Status::Error:      msg = "DATA ERROR";  break;
            case radar::Status::Ok:         msg = "NO TRAFFIC";  break;
        }
        set_text_if_changed(rd_status, msg);
        return;
    }
    set_text_if_changed(rd_status, "");

    const auto& f = rd_ac[rd_focus_ix];
    char l1[40];
    if (f.emergency)     snprintf(l1, sizeof(l1), LV_SYMBOL_WARNING " %s SQUAWK", f.callsign);
    else if (f.route[0]) snprintf(l1, sizeof(l1), "%s  %s", f.callsign, f.route);
    else                 snprintf(l1, sizeof(l1), "%s", f.callsign[0] ? f.callsign : "(no callsign)");
    set_text_if_changed(rd_focus1, l1);
    lv_obj_set_style_text_color(rd_focus1, f.emergency ? COL_RED : scope_col(), 0);

    // Climb/descend arrow from baro rate (±300 fpm deadband)
    const char* vsym = "";
    if (f.baro_rate > 300)       vsym = LV_SYMBOL_UP;
    else if (f.baro_rate < -300) vsym = LV_SYMBOL_DOWN;

    char l2[48];
    if (f.on_ground) snprintf(l2, sizeof(l2), "GROUND · %.0fkm", live_dist[rd_focus_ix]);
    else snprintf(l2, sizeof(l2), "%ldft%s  %dkt  %.1fkm",
                  (long)f.alt_ft, vsym, (int)f.gs_kt, live_dist[rd_focus_ix]);
    set_text_if_changed(rd_focus2, l2);
}

// Fast tick: rotate sweep, flare/decay blips, glide positions, pulse alert.
void radar_timer_cb(lv_timer_t*) {
    if (shown != screen_radar || !screen_radar) return;

    uint32_t now = millis();

    // Derive the sweep angle from the wall clock rather than counting ticks:
    // timer callbacks arrive unevenly under render/network load, and a fixed
    // per-tick step turns that jitter into visible stutter.
    float prev = rd_sweep_deg;
    rd_sweep_deg = (now % SWEEP_PERIOD_MS) * (360.0f / SWEEP_PERIOD_MS);
    radar_place_sweep();

    // Pull fresh data once a second; re-place (dead-reckon) twice a second;
    // cycle the focused aircraft every 4 s unless the overhead alert pins it.
    static uint32_t last_pull_ms = 0, last_place_ms = 0, last_focus_ms = 0;
    bool dirty = false;
    if (now - last_pull_ms >= 1000) {
        last_pull_ms = now;
        rd_ac_n = radar::get_aircraft(rd_ac, radar::MAX_AIRCRAFT);
        dirty = true;
    }
    if (!rd_alert && rd_ac_n > 1 && now - last_focus_ms >= 4000) {
        last_focus_ms = now;
        rd_focus_ix = (rd_focus_ix + 1) % rd_ac_n;
        dirty = true;
    }
    if (dirty || now - last_place_ms >= 500) {
        last_place_ms = now;
        radar_place_blips();
    }

    // Afterglow: flare a blip when the sweep crosses its bearing, then decay.
    for (size_t i = 0; i < rd_ac_n; i++) {
        float b = rd_bearing[i];
        bool crossed = (prev <= b && b < rd_sweep_deg) ||
                       (prev > rd_sweep_deg && (b >= prev || b < rd_sweep_deg));  // wrap
        if (crossed)             rd_glow[i] = 255;
        else if (rd_glow[i] > 110) rd_glow[i] -= 5;   // ~1 s fade at 33 ms ticks
        lv_obj_set_style_bg_opa(rd_blip[i], rd_glow[i], 0);
        lv_obj_set_style_text_opa(rd_tag[i], rd_glow[i], 0);
        lv_obj_set_style_line_opa(rd_vec[i], rd_glow[i], 0);
    }

    // Overhead alert: pulse the outer ring
    if (rd_ring_outer) {
        if (rd_alert) {
            lv_opa_t opa = (lv_opa_t)(140 + 115 * sinf(now * 0.012f));
            lv_obj_set_style_border_color(rd_ring_outer, scope_col(), 0);
            lv_obj_set_style_border_width(rd_ring_outer, 3, 0);
            lv_obj_set_style_border_opa(rd_ring_outer, opa, 0);
        } else {
            lv_obj_set_style_border_color(rd_ring_outer, scope_dim(), 0);
            lv_obj_set_style_border_width(rd_ring_outer, 1, 0);
            lv_obj_set_style_border_opa(rd_ring_outer, LV_OPA_COVER, 0);
        }
    }
}

// ── Weather screen ───────────────────────────────────────────────────────────
// Icons are composed from LVGL primitives (circles, lines) — no image assets,
// no extra fonts. One hidden group per icon kind; the update timer shows the
// one matching the current WMO weather code.

// Icon classification lives in weather.h; groups are built in enum order.
constexpr uint8_t WX_SUN     = (uint8_t)weather::Icon::Sun;
constexpr uint8_t WX_PARTLY  = (uint8_t)weather::Icon::Partly;
constexpr uint8_t WX_CLOUD   = (uint8_t)weather::Icon::Cloud;
constexpr uint8_t WX_RAIN    = (uint8_t)weather::Icon::Rain;
constexpr uint8_t WX_SNOW    = (uint8_t)weather::Icon::Snow;
constexpr uint8_t WX_THUNDER = (uint8_t)weather::Icon::Thunder;
constexpr uint8_t WX_FOG     = (uint8_t)weather::Icon::Fog;
constexpr uint8_t WX_ICON_COUNT = weather::ICON_COUNT;

lv_obj_t* wx_icon[WX_ICON_COUNT] = {};
lv_obj_t* wx_temp   = nullptr;
lv_obj_t* wx_desc   = nullptr;
lv_obj_t* wx_line1  = nullptr;
lv_obj_t* wx_line2  = nullptr;
lv_obj_t* wx_status = nullptr;
lv_timer_t* wx_timer = nullptr;

// Rain nowcast mini-graph: one bar per 5-minute step, 2 h total, at the very
// bottom of the round screen. Hidden entirely when the next 2 h are dry.
constexpr int RAIN_BARS      = 24;
constexpr int RAIN_BAR_W     = 3;
constexpr int RAIN_BAR_PITCH = 4;
constexpr int RAIN_BASE_Y    = 228;   // bar bottoms; chord half-width here ≈ 48 px
constexpr int RAIN_MAX_H     = 16;
lv_obj_t* wx_rain[RAIN_BARS] = {};

const char* wind_compass(int deg) {
    static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return dirs[((deg % 360) + 382) / 45 % 8];   // +22 rounding, normalized
}

// primitive helpers (positions are local to the icon group)
lv_obj_t* wx_circle(lv_obj_t* p, int d, int x, int y, lv_color_t c) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* wx_line(lv_obj_t* p, lv_point_precise_t* pts, int n,
                  lv_color_t c, int w) {
    lv_obj_t* o = lv_line_create(p);
    lv_line_set_points(o, pts, n);
    lv_obj_set_style_line_color(o, c, 0);
    lv_obj_set_style_line_width(o, w, 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    return o;
}

// cloud puffs + base bar, offset by (ox, oy) within the group
void wx_cloud(lv_obj_t* p, int ox, int oy, lv_color_t c) {
    wx_circle(p, 26, ox,      oy + 12, c);
    wx_circle(p, 36, ox + 16, oy,      c);
    wx_circle(p, 28, ox + 40, oy + 10, c);
    lv_obj_t* bar = lv_obj_create(p);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 64, 18);
    lv_obj_set_pos(bar, ox + 2, oy + 20);
    lv_obj_set_style_radius(bar, 9, 0);
    lv_obj_set_style_bg_color(bar, c, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
}

void build_weather_screen() {
    if (screen_wx) {
        lv_obj_clean(screen_wx);
    } else {
        screen_wx = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen_wx);
        lv_obj_set_style_bg_color(screen_wx, COL_BG, 0);
        lv_obj_set_style_bg_opa(screen_wx, LV_OPA_COVER, 0);
    }

    auto group = [&]() {
        lv_obj_t* g = lv_obj_create(screen_wx);
        lv_obj_remove_style_all(g);
        lv_obj_set_size(g, 100, 84);
        lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
        return g;
    };

    // sun: disc + 8 rays
    {
        lv_obj_t* g = wx_icon[WX_SUN] = group();
        static lv_point_precise_t rays[8][2];
        for (int i = 0; i < 8; i++) {
            float a = i * 45.0f * (float)M_PI / 180.0f;
            rays[i][0] = {(lv_value_precise_t)(50 + 24 * cosf(a)),
                          (lv_value_precise_t)(40 + 24 * sinf(a))};
            rays[i][1] = {(lv_value_precise_t)(50 + 34 * cosf(a)),
                          (lv_value_precise_t)(40 + 34 * sinf(a))};
            wx_line(g, rays[i], 2, COL_SUN, 3);
        }
        wx_circle(g, 36, 32, 22, COL_SUN);
    }
    // partly cloudy: small sun peeking from behind a cloud
    {
        lv_obj_t* g = wx_icon[WX_PARTLY] = group();
        static lv_point_precise_t rays[4][2];
        for (int i = 0; i < 4; i++) {
            float a = (i * 90.0f - 45.0f) * (float)M_PI / 180.0f;
            rays[i][0] = {(lv_value_precise_t)(30 + 16 * cosf(a)),
                          (lv_value_precise_t)(22 + 16 * sinf(a))};
            rays[i][1] = {(lv_value_precise_t)(30 + 24 * cosf(a)),
                          (lv_value_precise_t)(22 + 24 * sinf(a))};
            wx_line(g, rays[i], 2, COL_SUN, 3);
        }
        wx_circle(g, 26, 17, 9, COL_SUN);
        wx_cloud(g, 22, 26, COL_CLOUD);
    }
    // overcast
    {
        lv_obj_t* g = wx_icon[WX_CLOUD] = group();
        wx_cloud(g, 18, 16, COL_CLOUD);
    }
    // rain: cloud + 3 slanted drops
    {
        lv_obj_t* g = wx_icon[WX_RAIN] = group();
        wx_cloud(g, 18, 8, COL_CLOUD);
        static lv_point_precise_t drops[3][2];
        for (int i = 0; i < 3; i++) {
            drops[i][0] = {(lv_value_precise_t)(32 + i * 16), 56};
            drops[i][1] = {(lv_value_precise_t)(28 + i * 16), 72};
            wx_line(g, drops[i], 2, COL_RAIN, 3);
        }
    }
    // snow: cloud + 3 flakes
    {
        lv_obj_t* g = wx_icon[WX_SNOW] = group();
        wx_cloud(g, 18, 8, COL_CLOUD);
        wx_circle(g, 8, 28, 58, COL_SNOW);
        wx_circle(g, 8, 46, 66, COL_SNOW);
        wx_circle(g, 8, 64, 58, COL_SNOW);
    }
    // thunder: cloud + bolt
    {
        lv_obj_t* g = wx_icon[WX_THUNDER] = group();
        wx_cloud(g, 18, 6, COL_CLOUD);
        static lv_point_precise_t bolt[4] = {{56, 44}, {44, 62}, {53, 62}, {42, 82}};
        wx_line(g, bolt, 4, COL_SUN, 4);
    }
    // fog: stacked bars
    {
        lv_obj_t* g = wx_icon[WX_FOG] = group();
        static lv_point_precise_t bars[3][2];
        for (int i = 0; i < 3; i++) {
            bars[i][0] = {(lv_value_precise_t)(20 + (i == 1 ? 8 : 0)),
                          (lv_value_precise_t)(28 + i * 16)};
            bars[i][1] = {(lv_value_precise_t)(80 + (i == 1 ? 8 : 0)),
                          (lv_value_precise_t)(28 + i * 16)};
            wx_line(g, bars[i], 2, COL_CLOUD, 5);
        }
    }

    wx_temp = lv_label_create(screen_wx);
    lv_obj_set_style_text_font(wx_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(wx_temp, COL_TEXT, 0);
    lv_label_set_text(wx_temp, "");
    lv_obj_align(wx_temp, LV_ALIGN_CENTER, 0, 10);

    wx_desc = lv_label_create(screen_wx);
    lv_obj_set_style_text_font(wx_desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wx_desc, COL_DIM_TEXT, 0);
    lv_label_set_text(wx_desc, "");
    lv_obj_align(wx_desc, LV_ALIGN_CENTER, 0, 44);

    wx_line1 = lv_label_create(screen_wx);
    lv_obj_set_style_text_font(wx_line1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wx_line1, COL_TEXT, 0);
    lv_label_set_text(wx_line1, "");
    lv_obj_align(wx_line1, LV_ALIGN_BOTTOM_MID, 0, -50);

    wx_line2 = lv_label_create(screen_wx);
    lv_obj_set_style_text_font(wx_line2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wx_line2, COL_DIM_TEXT, 0);
    lv_label_set_text(wx_line2, "");
    lv_obj_align(wx_line2, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Rain nowcast bars (left = now, right = +2 h)
    int x0 = CX - (RAIN_BARS * RAIN_BAR_PITCH - (RAIN_BAR_PITCH - RAIN_BAR_W)) / 2;
    for (int i = 0; i < RAIN_BARS; i++) {
        wx_rain[i] = lv_obj_create(screen_wx);
        lv_obj_remove_style_all(wx_rain[i]);
        lv_obj_set_size(wx_rain[i], RAIN_BAR_W, 2);
        lv_obj_set_pos(wx_rain[i], x0 + i * RAIN_BAR_PITCH, RAIN_BASE_Y - 2);
        lv_obj_set_style_radius(wx_rain[i], 1, 0);
        lv_obj_set_style_bg_color(wx_rain[i], COL_RAIN, 0);
        lv_obj_set_style_bg_opa(wx_rain[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(wx_rain[i], LV_OBJ_FLAG_HIDDEN);
    }

    wx_status = lv_label_create(screen_wx);
    lv_obj_set_style_text_font(wx_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wx_status, COL_DIM_TEXT, 0);
    lv_obj_set_style_text_align(wx_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wx_status, "");
    lv_obj_align(wx_status, LV_ALIGN_CENTER, 0, 0);
}

void weather_timer_cb(lv_timer_t*) {
    if (shown != screen_wx || !screen_wx) return;

    auto w = weather::get();

    if (!w.valid) {
        for (auto* g : wx_icon) lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
        for (auto* b : wx_rain) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        set_text_if_changed(wx_temp, "");
        set_text_if_changed(wx_desc, "");
        set_text_if_changed(wx_line1, "");
        set_text_if_changed(wx_line2, "");
        const char* msg = "";
        switch (weather::status()) {
            case weather::Status::NoLocation: msg = "SET LOCATION\nin web UI"; break;
            case weather::Status::NoWifi:     msg = "NO WIFI";      break;
            case weather::Status::Fetching:   msg = "FETCHING...."; break;
            case weather::Status::Error:      msg = "DATA ERROR";   break;
            case weather::Status::Ok:         msg = "";             break;
        }
        set_text_if_changed(wx_status, msg);
        return;
    }
    set_text_if_changed(wx_status, "");

    uint8_t icon_ix = (uint8_t)w.icon;
    for (uint8_t i = 0; i < WX_ICON_COUNT; i++) {
        if (i == icon_ix) lv_obj_remove_flag(wx_icon[i], LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(wx_icon[i], LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%.0f°", w.temp_c);
    set_text_if_changed(wx_temp, buf);
    set_text_if_changed(wx_desc, w.desc);

    snprintf(buf, sizeof(buf), "feels %.0f°  •  %u%%", w.feels_c, w.humidity);
    set_text_if_changed(wx_line1, buf);

    snprintf(buf, sizeof(buf), "wind %.0f km/h %s",
             w.wind_kmh, wind_compass(w.wind_dir_deg));
    set_text_if_changed(wx_line2, buf);

    // Rain nowcast graph — only shown when rain is actually coming.
    // raintext raw values: 0 = dry, ~77 ≈ 0.1 mm/h, ~130 ≈ 1 mm/h.
    uint8_t peak = 0;
    for (uint8_t i = 0; i < w.rain_n; i++) peak = max(peak, w.rain[i]);
    bool raining_soon = w.rain_n > 0 && peak >= 70;

    for (int i = 0; i < RAIN_BARS; i++) {
        if (!raining_soon) {
            lv_obj_add_flag(wx_rain[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        uint8_t raw = (i < w.rain_n) ? w.rain[i] : 0;
        float rel = (raw - 60) / 130.0f;
        rel = rel < 0 ? 0 : (rel > 1 ? 1 : rel);
        int h = 2 + (int)(rel * RAIN_MAX_H);
        lv_obj_set_size(wx_rain[i], RAIN_BAR_W, h);
        lv_obj_set_y(wx_rain[i], RAIN_BASE_Y - h);
        lv_obj_remove_flag(wx_rain[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Auto mode supervisor ─────────────────────────────────────────────────────
// Weather is the resting screen; when an airborne aircraft comes within
// radar.auto_km the scope takes over, and stays for 30 s after the last
// contact so brief gaps between polls don't cause flapping. auto_km == 0
// disables switching (Auto then just shows weather). Distinct from alert_km
// (the on-scope pulse/pin), which typically sits tighter than the switch.
constexpr uint32_t AUTO_HOLD_MS = 30000;

void auto_timer_cb(lv_timer_t*) {
    if (status_active || settings::state().mode != settings::Mode::Auto) return;

    const auto& cfg = settings::state().radar;
    static uint32_t last_traffic_ms = 0;

    bool traffic = false;
    if (cfg.auto_km > 0) {
        radar::Aircraft ac[radar::MAX_AIRCRAFT];
        size_t n = radar::get_aircraft(ac, radar::MAX_AIRCRAFT);
        for (size_t i = 0; i < n; i++) {
            if (!ac[i].on_ground && ac[i].dist_km <= (float)cfg.auto_km) {
                traffic = true;
                break;
            }
        }
    }

    uint32_t now = millis();
    if (traffic) last_traffic_ms = now;
    bool hold = last_traffic_ms != 0 && now - last_traffic_ms < AUTO_HOLD_MS;
    load_screen(traffic || hold ? screen_radar : screen_wx);
}

// Screen the current settings mode wants displayed right now.
lv_obj_t* target_for_mode() {
    switch (settings::state().mode) {
        case settings::Mode::Radar:   return screen_radar;
        case settings::Mode::Weather: return screen_wx;
        // Auto: keep whatever the supervisor chose; default to weather.
        default: return shown == screen_radar ? screen_radar : screen_wx;
    }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────
void begin() {
    build_status_screen();
    build_radar_screen();
    build_weather_screen();
    // 33 ms: the wall-clock-derived sweep angle stays smooth at any tick
    // rate, and the smaller per-frame redraw keeps flush seams invisible.
    rd_timer = lv_timer_create(radar_timer_cb, 33, nullptr);
    wx_timer = lv_timer_create(weather_timer_cb, 1000, nullptr);
    lv_timer_create(auto_timer_cb, 500, nullptr);
    set_mode(settings::state().mode);
}

void set_mode(settings::Mode m) {
    current = m;
    status_active = false;
    shown = nullptr;            // force the load even if the target matches
    load_screen(target_for_mode());
}

void apply_settings() {
    // Rebuild all screens so theme / range / labels update, then reload the
    // right screen. One load only — a separate set_mode() first would start
    // a fade that the rebuild's lv_obj_clean() then guts mid-animation.
    build_status_screen();
    build_radar_screen();
    build_weather_screen();
    current = settings::state().mode;
    lv_obj_t* target = status_active ? screen_status : target_for_mode();
    shown = nullptr;
    load_screen(target, !status_active);
}

void show_status() {
    status_active = true;
    shown = screen_status;
    lv_screen_load(screen_status);
}

void update_status(const char* line1, const char* line2) {
    if (st_line1 && line1) lv_label_set_text(st_line1, line1);
    if (st_line2 && line2) lv_label_set_text(st_line2, line2);
}

}  // namespace ui
