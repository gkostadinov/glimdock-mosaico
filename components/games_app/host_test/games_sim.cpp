// SPDX-License-Identifier: Apache-2.0
// Skyglide and Ridge Rally get harder as a run goes on, and runs end: a
// simulation at 30 steps a second with fixed seeds (CMakeLists.txt says how
// to build it). Each game's own pilot, the title screen's playing a real
// run, stands in for a good player: it should get through the gentle first
// stage every time, climb several stages and still lose within minutes.
// Scripted input checks that each way to lose ends a run. Exits non-zero
// when a check fails; -v prints every stage of the first runs.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "games_engine.h"

namespace {

using games::Hud;
using games::Input;

constexpr float DT = 1.0f / 30;
constexpr int SKYGLIDE = 2, RALLY = 5;
int failures = 0;
bool verbose = false;

void check(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct Run {
    float seconds;
    int stage, score;
    std::string ending;
    bool steady;  // the stage only ever went up, one at a time
};

// One run until it ends or the limit: input from the script, or the game's
// own pilot without one.
using Script = std::function<Input(float seconds, const Hud &hud)>;
Run play(int game, uint32_t seed, float limit, const Script &script, bool talk = false)
{
    auto g = games::make_game(game);
    g->set_bot(!script);
    g->reset(seed, false);
    Hud h;
    g->hud(&h);
    float t = 0;
    int stage = h.stage;
    bool steady = stage == 1;
    while (t < limit && !g->over()) {
        g->step(DT, script ? script(t, h) : Input{});
        g->take_events();
        t += DT;
        h = Hud{};
        g->hud(&h);
        if (talk && h.stage != stage) {
            printf("      %5.1f s  stage %2d  score %7d  %s %.2f  %s %.2f\n", t, h.stage, h.score, h.meter_name, h.meter,
                   h.meter2_name[0] ? h.meter2_name : "-", h.meter2_name[0] ? h.meter2 : 0.0f);
        }
        steady = steady && (h.stage == stage || h.stage == stage + 1);
        stage = h.stage;
    }
    return {t, h.stage, g->score(), g->over() ? g->ending() : "", steady};
}

// The pilot on many seeds: everyone through the gentle start, the middle
// run over within a few minutes (not seconds, not for ever), several stages
// reached, and stages that only go up.
void pilots(int game, const char *name, int runs, float first, float least, float most, int stages)
{
    printf("%s, its pilot on %d seeds:\n", name, runs);
    std::vector<Run> all;
    for (int i = 0; i < runs; ++i) {
        const uint32_t seed = 1000 + uint32_t(i) * 7919;
        all.push_back(play(game, seed, 900, nullptr, verbose && i < 2));
        const Run &r = all.back();
        printf("    seed %5u  %6.1f s  stage %2d  score %7d  %s\n", unsigned(seed), double(r.seconds), r.stage, r.score,
               r.ending.empty() ? "(still flying at the limit)" : r.ending.c_str());
    }
    std::vector<float> times;
    int ended = 0, best_stage = 0, by_stage[32] = {};
    bool steady = true;
    for (const Run &r : all) {
        times.push_back(r.seconds);
        ended += r.ending.empty() ? 0 : 1;
        best_stage = std::max(best_stage, r.stage);
        by_stage[std::min(r.stage, 31)] += r.ending.empty() ? 0 : 1;
        steady = steady && r.steady;
    }
    std::sort(times.begin(), times.end());
    const float median = times[times.size() / 2];
    printf("    shortest %.0f s, median %.0f s, longest %.0f s; runs ended by stage:", double(times.front()), double(median),
           double(times.back()));
    for (int s = 1; s < 32; ++s) {
        if (by_stage[s]) {
            printf(" %d: %d", s, by_stage[s]);
        }
    }
    printf("\n");
    char what[96];
    snprintf(what, sizeof(what), "every run lasts the first %.0f s", double(first));
    check(times.front() >= first, what);
    snprintf(what, sizeof(what), "the median run lasts %.0f to %.0f s", double(least), double(most));
    check(median >= least && median <= most, what);
    check(steady, "stages go up one at a time, and never down");
    check(ended >= runs * 9 / 10, "nine runs in ten end before 15 minutes");
    snprintf(what, sizeof(what), "a run reaches stage %d", stages);
    check(best_stage >= stages, what);
}

void scripted(int game, const char *what, float limit, const Script &script, const char *ending)
{
    const Run r = play(game, 4242, limit, script);
    printf("    %s: %.1f s, stage %d, \"%s\"\n", what, double(r.seconds), r.stage, r.ending.c_str());
    char line[128];
    snprintf(line, sizeof(line), "%s ends the run within %.0f s (\"%s\")", what, double(limit), ending);
    check(!r.ending.empty() && strstr(r.ending.c_str(), ending), line);
}

}  // namespace

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    pilots(SKYGLIDE, "Skyglide", 24, 30, 90, 330, 6);
    printf("Skyglide, scripted:\n");
    scripted(SKYGLIDE, "diving", 20, [](float, const Hud &) {
        Input in;
        in.y = -1;
        return in;
    }, "Down in the hills");
    scripted(SKYGLIDE, "circling", 90, [](float, const Hud &) {
        Input in;
        in.x = 1, in.y = 0.2f;
        return in;
    }, "Missed three hoops");
    pilots(RALLY, "Ridge Rally", 24, 30, 90, 330, 7);
    printf("Ridge Rally, scripted:\n");
    scripted(RALLY, "standing still", 60, [](float, const Hud &) {
        Input in;
        in.y = -1;
        return in;
    }, "Out of time");
    scripted(RALLY, "steering into the trees", 90, [](float, const Hud &) {
        Input in;
        in.x = 1, in.y = 1;
        return in;
    }, "Wrecked");
    printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
