#include "settings.h"

#include <type_traits>
#include <ArduinoJson.h>
#include <Preferences.h>

namespace settings {
namespace {

constexpr const char* NS = "esp-gauge";

Preferences prefs;
Snapshot    snap;

template <typename T>
void load_scalar(const char* key, T& out) {
    if (!prefs.isKey(key)) return;
    if constexpr (std::is_same_v<T, float>)   out = prefs.getFloat(key, out);
    else if constexpr (std::is_same_v<T, uint8_t>)  out = prefs.getUChar(key, out);
    else if constexpr (std::is_same_v<T, uint16_t>) out = prefs.getUShort(key, out);
    else if constexpr (std::is_same_v<T, int16_t>)  out = prefs.getShort(key, out);
    else if constexpr (std::is_same_v<T, bool>)     out = prefs.getBool(key, out);
}

void load_string(const char* key, char* dst, size_t cap) {
    if (!prefs.isKey(key)) return;
    String s = prefs.getString(key, dst);
    strlcpy(dst, s.c_str(), cap);
}

}  // namespace

void begin() {
    prefs.begin(NS, false);

    load_string("ssid", snap.wifi_ssid, sizeof(snap.wifi_ssid));
    load_string("pwd",  snap.wifi_password, sizeof(snap.wifi_password));
    load_string("host", snap.hostname, sizeof(snap.hostname));

    uint8_t mode_u8 = static_cast<uint8_t>(snap.mode);
    load_scalar("mode", mode_u8);
    // Values from the retired audio-gauge firmware (or junk) fall back to Radar.
    snap.mode = mode_u8 <= static_cast<uint8_t>(Mode::Weather)
                    ? static_cast<Mode>(mode_u8) : Mode::Radar;
    load_scalar("bri", snap.brightness);

    load_scalar("r_lat",  snap.radar.lat);
    load_scalar("r_lon",  snap.radar.lon);
    load_scalar("r_rng",  snap.radar.range_km);
    load_scalar("r_poll", snap.radar.poll_s);
    load_scalar("r_tags", snap.radar.show_tags);
    load_scalar("r_thm",  snap.radar.theme);
    load_scalar("r_alrt", snap.radar.alert_km);
}

void save() {
    prefs.putString("ssid", snap.wifi_ssid);
    prefs.putString("pwd",  snap.wifi_password);
    prefs.putString("host", snap.hostname);

    prefs.putUChar("mode", static_cast<uint8_t>(snap.mode));
    prefs.putUChar("bri",  snap.brightness);

    prefs.putFloat("r_lat",  snap.radar.lat);
    prefs.putFloat("r_lon",  snap.radar.lon);
    prefs.putUShort("r_rng",  snap.radar.range_km);
    prefs.putUShort("r_poll", snap.radar.poll_s);
    prefs.putBool("r_tags",  snap.radar.show_tags);
    prefs.putUChar("r_thm",  snap.radar.theme);
    prefs.putUShort("r_alrt", snap.radar.alert_km);
}

void reset_to_defaults() {
    prefs.clear();
    snap = Snapshot{};
    save();
}

Snapshot& state() { return snap; }

namespace {
template <typename T>
bool maybe_set(JsonVariantConst v, T& field) {
    if (v.isNull()) return false;
    T newv = v.as<T>();
    if (newv == field) return false;
    field = newv;
    return true;
}

bool maybe_set_str(JsonVariantConst v, char* dst, size_t cap) {
    if (v.isNull()) return false;
    const char* s = v.as<const char*>();
    if (!s) return false;
    if (strncmp(dst, s, cap) == 0) return false;
    strlcpy(dst, s, cap);
    return true;
}
}  // namespace

bool apply_json(JsonVariantConst patch) {
    bool changed = false;

    if (patch["mode"].is<uint8_t>() || patch["mode"].is<int>()) {
        uint8_t m = patch["mode"].as<uint8_t>();
        if (m <= static_cast<uint8_t>(Mode::Weather) &&
            static_cast<uint8_t>(snap.mode) != m) {
            snap.mode = static_cast<Mode>(m);
            changed = true;
        }
    }

    changed |= maybe_set<uint8_t>(patch["brightness"], snap.brightness);

    JsonVariantConst r = patch["radar"];
    if (!r.isNull()) {
        changed |= maybe_set<float>(r["lat"], snap.radar.lat);
        changed |= maybe_set<float>(r["lon"], snap.radar.lon);
        changed |= maybe_set<uint16_t>(r["range_km"], snap.radar.range_km);
        changed |= maybe_set<uint16_t>(r["poll_s"],   snap.radar.poll_s);
        changed |= maybe_set<bool>(r["show_tags"],    snap.radar.show_tags);
        changed |= maybe_set<uint8_t>(r["theme"],     snap.radar.theme);
        changed |= maybe_set<uint16_t>(r["alert_km"], snap.radar.alert_km);
        snap.radar.range_km = constrain(snap.radar.range_km, 10, 400);
        snap.radar.poll_s   = constrain(snap.radar.poll_s, 5, 120);
        snap.radar.theme    = constrain(snap.radar.theme, 0, 1);
        snap.radar.alert_km = constrain(snap.radar.alert_km, 0, 50);
    }

    JsonVariantConst w = patch["wifi"];
    if (!w.isNull()) {
        changed |= maybe_set_str(w["ssid"],     snap.wifi_ssid,     sizeof(snap.wifi_ssid));
        changed |= maybe_set_str(w["password"], snap.wifi_password, sizeof(snap.wifi_password));
        changed |= maybe_set_str(w["hostname"], snap.hostname,      sizeof(snap.hostname));
    }

    return changed;
}

void to_json(JsonObject out, bool include_secrets) {
    out["mode"]       = static_cast<uint8_t>(snap.mode);
    out["brightness"] = snap.brightness;

    JsonObject r = out["radar"].to<JsonObject>();
    r["lat"]       = snap.radar.lat;
    r["lon"]       = snap.radar.lon;
    r["range_km"]  = snap.radar.range_km;
    r["poll_s"]    = snap.radar.poll_s;
    r["show_tags"] = snap.radar.show_tags;
    r["theme"]     = snap.radar.theme;
    r["alert_km"]  = snap.radar.alert_km;

    JsonObject w = out["wifi"].to<JsonObject>();
    w["ssid"]     = snap.wifi_ssid;
    w["password"] = include_secrets ? snap.wifi_password : (strlen(snap.wifi_password) ? "********" : "");
    w["hostname"] = snap.hostname;
}

}  // namespace settings
