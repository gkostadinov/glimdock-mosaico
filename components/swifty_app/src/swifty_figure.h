// SPDX-License-Identifier: Apache-2.0
// Swifty, the bird of Swifty Home, in 3D: a port of web/app/swifty/figure.js
// (ball, feather and wing surfaces, one lighting model, spring-driven pose and
// moods) to a small software rasterizer. No ESP-IDF or LVGL here, so the host
// preview renders the same frames.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>

namespace swifty {

enum class Mood : uint8_t { Idle, Listen, Think, Talk };
enum class Shot : uint8_t { Hello, Send, Pleased, Sorry, Confused, Poke };

class Figure {
public:
    // size: output square in pixels; ss: supersampling factor (1 or 2).
    Figure(int size, int ss = 1);
    ~Figure();
    Figure(const Figure &) = delete;
    Figure &operator=(const Figure &) = delete;

    void set_mood(Mood mood);
    void shoot(Shot shot);
    // A reply arrived: its mood (or Sorry when it failed), and it "talks" for a
    // while in proportion to its length.
    void react(Shot mood, bool failed, int text_length);
    void set_glow(bool glow);  // the soft halo behind the bird (on by default)
    void set_level(float level);  // microphone loudness 0..1 while listening
    void step(float dt);          // seconds
    // Doing something on purpose (listening, thinking, talking, a shot, the
    // mic's level): worth a higher frame rate than the idle sway.
    bool busy() const;
    // Where render time went since the last call (per frame): for tuning.
    std::string stats(bool reset);
    // Draws into a size x size BGRA buffer (LVGL ARGB8888, straight alpha).
    void render(uint8_t *bgra);
    // The same, premultiplied (LVGL ARGB8888_PREMULTIPLIED): no division per
    // pixel, for the animation (render() costs ~4 ms more a frame on the board).
    void render_premultiplied(uint8_t *bgra);
    int size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace swifty
