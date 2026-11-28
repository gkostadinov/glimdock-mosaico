// SPDX-License-Identifier: Apache-2.0
// Every hub action Home sends, built in one place so each body can be checked
// against the hub: the action ids, parameter names and ranges are those of
// swifty-hub's catalog (crates/swifty-hub/src/catalog/actions.rs), which
// refuses an unknown parameter, a missing one or a value out of range
// (validate_params in catalog/mod.rs). The host test (host_test/) sends each
// of these to a demo hub.
//
// A paired display acts as a family member (displays.rs), so the hub lists in
// its catalog, and runs, only what policy.rs FAMILY_ACTIONS names: lights,
// rooms, scenes, the speaker, the TV and the pets. Switches (plugs), the air
// conditioner and the printer are the owner's unless the hub shares them; the
// UI offers a control only when hub.can() says the catalog has it.
#pragma once

#include <algorithm>
#include <string>

#include "ArduinoJson.h"

namespace home {

struct Command {
    std::string action, target;
    JsonDocument params;  // always an object, {} when the action takes none
};

namespace cmd {
inline Command make(const char *action, const std::string &target)
{
    Command c{action, target, JsonDocument()};
    c.params.to<JsonObject>();
    return c;
}
template <typename T> Command make(const char *action, const std::string &target, const char *key, T value)
{
    Command c = make(action, target);
    c.params[key] = value;
    return c;
}

// The fixed targets the hub names (catalog/actions.rs Targets::Fixed).
constexpr const char *ALL_LIGHTS = "all_bulbs";
constexpr const char *MOVIE = "movie";
// Ranges the catalog takes; a device's capabilities may narrow them.
constexpr int PERCENT_MIN = 1, PERCENT_MAX = 100;  // P_PERCENT
constexpr int AC_MIN = 17, AC_MAX = 30;            // ac.temperature celsius
constexpr int GRAMS_MIN = 10, GRAMS_MAX = 50;      // pets.feed grams

// Lights: {on: bool}, {percent: 1..100}, {hue: 0..359}.
inline Command light_power(const std::string &t, bool on)
{
    return make("light.power", t, "on", on);
}
inline Command light_brightness(const std::string &t, int pct)
{
    return make("light.brightness", t, "percent", std::clamp(pct, PERCENT_MIN, PERCENT_MAX));
}
inline Command light_hue(const std::string &t, int hue)
{
    return make("light.hue", t, "hue", ((hue % 360) + 360) % 360);
}
inline Command lights_power(bool on)
{
    return make("lights.power", ALL_LIGHTS, "on", on);
}
inline Command lights_brightness(int pct)
{
    return make("lights.brightness", ALL_LIGHTS, "percent", std::clamp(pct, PERCENT_MIN, PERCENT_MAX));
}
// A room by its id: {on}, {percent: 1..100}, {scene: a name saved for the room}.
inline Command room_power(const std::string &id, bool on)
{
    return make("room.power", id, "on", on);
}
inline Command room_brightness(const std::string &id, int pct)
{
    return make("room.brightness", id, "percent", std::clamp(pct, PERCENT_MIN, PERCENT_MAX));
}
inline Command room_scene(const std::string &id, const std::string &name)
{
    return make("room.scene", id, "scene", name);
}
// The house's scenes: evening, focus or all_off; Movie asks first.
inline Command scene_apply(const std::string &id)
{
    return make("scene.apply", id);
}
inline Command scene_movie()
{
    return make("scene.movie", MOVIE);
}
// Speaker: {level: 0..100}, {muted: bool}, {on: bool}, play and pause take none.
inline Command speaker_volume(const std::string &t, int level)
{
    return make("speaker.volume", t, "level", std::clamp(level, 0, 100));
}
inline Command speaker_mute(const std::string &t, bool muted)
{
    return make("speaker.mute", t, "muted", muted);
}
inline Command speaker_night(const std::string &t, bool on)
{
    return make("speaker.night", t, "on", on);
}
inline Command speaker_play(const std::string &t)
{
    return make("speaker.play", t);
}
inline Command speaker_pause(const std::string &t)
{
    return make("speaker.pause", t);
}
// TV: {on} (asks first), {input: one of TV_INPUTS}, {key: one of TV_KEYS}.
inline Command tv_power(const std::string &t, bool on)
{
    return make("tv.power", t, "on", on);
}
inline Command tv_input(const std::string &t, const std::string &input)
{
    return make("tv.input", t, "input", input);
}
inline Command tv_key(const std::string &t, const std::string &key)
{
    return make("tv.key", t, "key", key);
}
// A switch: the hub's class "plug" (a socket, or a USB port that switches on its own).
inline Command plug_power(const std::string &t, bool on)
{
    return make("plug.power", t, "on", on);
}
// Air conditioner: {on}, {mode: AC_MODES}, {celsius: 17..30}, {fan: AC_FANS}.
inline Command ac_power(const std::string &t, bool on)
{
    return make("ac.power", t, "on", on);
}
inline Command ac_mode(const std::string &t, const std::string &mode)
{
    return make("ac.mode", t, "mode", mode);
}
inline Command ac_temperature(const std::string &t, int celsius)
{
    return make("ac.temperature", t, "celsius", std::clamp(celsius, AC_MIN, AC_MAX));
}
inline Command ac_fan(const std::string &t, const std::string &fan)
{
    return make("ac.fan", t, "fan", fan);
}
// Pets, both asking first: {grams: 10..50}, and a clean cycle with none.
inline Command pets_feed(const std::string &t, int grams)
{
    return make("pets.feed", t, "grams", std::clamp(grams, GRAMS_MIN, GRAMS_MAX));
}
inline Command pets_clean(const std::string &t)
{
    return make("pets.clean", t);
}
// Printer: pause, resume and start ask first; the emergency stop runs at once
// (tier S: it only removes risk).
inline Command print_pause(const std::string &t)
{
    return make("print.pause", t);
}
inline Command print_resume(const std::string &t)
{
    return make("print.resume", t);
}
inline Command print_start(const std::string &t)
{
    return make("print.start", t);
}
inline Command print_estop(const std::string &t)
{
    return make("print.estop", t);
}
}  // namespace cmd
}  // namespace home
