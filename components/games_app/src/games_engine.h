// SPDX-License-Identifier: Apache-2.0
// The Games app's three retro 3D games, drawn in software into a 240 x 240
// RGB565 frame that the app shows doubled on the 480 x 480 panel. No ESP-IDF
// or LVGL here, so the host preview renders the same frames.
#pragma once

#include <stdint.h>

#include <memory>

namespace games {

constexpr int W = 240, H = 240;  // the game's frame
constexpr int GAME_COUNT = 6;    // 0 Ember Crypt, 1 Starbore, 2 Skyglide, 3 Jellystorm, 4 Lunar Flip, 5 Ridge Rally

// What happened during a step, for sounds and haptics (take_events()).
enum : uint32_t {
    EV_PICKUP = 1u << 0,  // a shard, an orb, a ring
    EV_HIT = 1u << 1,     // damage
    EV_LEVEL = 1u << 2,   // a level or a lap done
    EV_OVER = 1u << 3,    // the game ended
    EV_POWER = 1u << 4,   // the game's action (a flare), a refill
    EV_WARN = 1u << 5,    // running low (oil, time, height)
};

// Steering in -1..1 (x right, y forward or up the screen) from a finger or a
// tilt (the board's top tilted back towards the player is up, in every game),
// a turn of the board itself (the gyroscope: radians this step, + right) and
// the game's one action (a tap, a shake, a sub-board button) for this step.
// absolute: x and y come from the board's tilt, a position the player holds
// (level is the middle), rather than a drag's push. left, right: held down
// (a finger on that half of the screen, or the Interaction module's button:
// flippers); shake: the board was shaken (a nudge).
struct Input {
    float x = 0, y = 0, turn = 0;
    bool action = false, absolute = false, left = false, right = false, shake = false;
};

// What the app shows over the frame.
struct Hud {
    int score = 0;
    float meter = 1;               // 0..1
    uint32_t meter_color = 0xFFFFFF;
    const char *meter_name = "";   // "Oil", "Shield", "Time"
    // A second meter under the first (Ridge Rally's car), shown when named.
    float meter2 = 1;
    uint32_t meter2_color = 0xFFFFFF;
    const char *meter2_name = "";
    int stage = 0;                 // how far into its difficulty a run is (0: the game has no stages)
    char line[64] = "";            // a short status: "Depth 2 · 3 of 6 shards"
};

class Game {
public:
    virtual ~Game() = default;
    // A new run; demo: it plays itself, for the title screen (and never ends).
    virtual void reset(uint32_t seed, bool demo) = 0;
    virtual void step(float dt, const Input &in) = 0;  // dt in seconds
    virtual void render(uint16_t *frame) = 0;          // W x H, RGB565 in native byte order
    virtual void hud(Hud *out) const = 0;
    // Why the game ended, for the game-over card.
    virtual const char *ending() const = 0;
    // The frame is drawn column-major (x * H + y): see games_common.h.
    virtual bool transposed() const
    {
        return false;
    }
    // Where drawing time went since the last call, when a clock is set (tuning).
    virtual void profile(char *out, int capacity)
    {
        if (capacity > 0) {
            out[0] = 0;
        }
    }
    bool over() const
    {
        return over_;
    }
    int score() const
    {
        return score_;
    }
    uint32_t take_events()
    {
        const uint32_t e = events_;
        events_ = 0;
        return e;
    }
    // Tests and tuning: the title screen's pilot plays a real run, one that
    // ends as a played run does (a demo never ends). Call before reset().
    // Skyglide and Ridge Rally have one; the other games ignore it.
    void set_bot(bool on)
    {
        bot_ = on;
    }

protected:
    bool over_ = false, demo_ = false, bot_ = false;
    int score_ = 0;
    uint32_t events_ = 0;
};

std::unique_ptr<Game> make_game(int index);

// A size x size thumbnail: the frame's centre (2 * size square) box-filtered 2:1.
void thumbnail(const uint16_t *frame, bool transposed, uint16_t *out, int size);
// A microsecond clock for profile() (the app sets it; none: no profiling).
extern int64_t (*clock_us)();

}  // namespace games
