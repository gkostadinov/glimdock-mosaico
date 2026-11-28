// SPDX-License-Identifier: Apache-2.0
// Host tests for the home screen's layout (launcher_layout.h): the text form,
// the grid's packing and the migration from the layout before widgets were
// items. Built with the preview (host/CMakeLists.txt: launcher-layout-test),
// or on its own from the repository root:
//   c++ -std=c++17 -Icomponents/mosaico_shell/src host/launcher_layout_test.cpp \
//       components/mosaico_shell/src/launcher_layout.cpp -o /tmp/layout-test && /tmp/layout-test
#include <cstdio>
#include <string>
#include <vector>

#include "launcher_layout.h"

namespace L = launcher_layout;

namespace {
int failures = 0, checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(cond)) {                                                              \
            ++failures;                                                             \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++checks;                                                                                   \
        const std::string x_ = (a), y_ = (b);                                                       \
        if (x_ != y_) {                                                                             \
            ++failures;                                                                             \
            std::printf("%s:%d: %s\n   got: %s\n  want: %s\n", __FILE__, __LINE__, #a, x_.c_str(), y_.c_str()); \
        }                                                                                           \
    } while (0)

// The firmware's apps in launcher_apps.cpp's order (settings can't be hidden).
const std::vector<std::string> APPS = {"glimdock", "home", "swifty", "camera", "gallery", "files", "tools", "settings",
    "webcam", "interaction", "calculator", "vpn", "games"
};
const std::vector<std::string> OLD_WIDGETS = {"w2:glimdock", "w2:lights"};

bool is_app(const std::string &k)
{
    for (const std::string &a : APPS)
        if (a == k) {
            return true;
        }
    return false;
}

// As the launcher classifies keys: apps and known widgets show, unregistered
// shortcut tiles wait, anything else is gone.
L::Kind kind(const std::string &k)
{
    if (is_app(k)) {
        return L::Kind::Shown;
    }
    if (L::is_widget(k)) {
        const std::string name = k.substr(3, k.find('~') == std::string::npos ? std::string::npos : k.find('~') - 3);
        return name == "glimdock" || name == "lights" || name == "weather" || name == "clock" || name == "music"
               ? L::Kind::Shown : L::Kind::Gone;
    }
    return k.compare(0, 3, "sc:") == 0 ? L::Kind::Pending : L::Kind::Gone;
}

bool keep(const std::string &k)
{
    return k == "settings";
}

// Parse, migrate with the two old widget kinds, normalize: what the launcher
// does with the text NVS holds at start.
std::string load(const std::string &text, const std::vector<std::string> &widgets = OLD_WIDGETS)
{
    L::Layout l = L::parse(text);
    L::migrate(l, widgets);
    L::normalize(l, APPS, kind, keep, false);
    return L::serialize(l);
}

std::string spots(const std::vector<int> &widths)
{
    std::string out;
    for (const L::Spot &s : L::pack(widths).spots) {
        char b[24];
        std::snprintf(b, sizeof(b), "%s%d%d%s", out.empty() ? "" : " ", s.row, s.col, s.fits ? "" : "!");
        out += b;
    }
    return out;
}

void test_widths()
{
    CHECK(L::width("w2:weather") == 2);
    CHECK(L::width("w4:clock~1") == 4);
    CHECK(L::width("glimdock") == 1);
    CHECK(L::width("sc:movie") == 1);
    CHECK(L::width("w3:x") == 1);
    CHECK(L::width("w2:") == 1);
}

void test_pack()
{
    // The old first page: two widgets in a tall row, two rows of icons.
    CHECK_EQ(spots({2, 2, 1, 1, 1, 1, 1, 1, 1, 1}), "00 02 10 11 12 13 20 21 22 23");
    L::Grid g = L::pack(std::vector<int> {2, 2, 1, 1, 1, 1, 1, 1, 1, 1});
    CHECK(g.rows == 3 && g.tall[0] && !g.tall[1] && !g.tall[2]);
    // A ninth icon needs a fourth row.
    CHECK_EQ(spots({2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1}), "00 02 10 11 12 13 20 21 22 23 30!");
    // A widget after an icon takes the right half; the next icon fills the gap.
    CHECK_EQ(spots({1, 2, 1}), "00 02 01");
    CHECK_EQ(spots({1, 1, 1, 2, 1}), "00 01 02 10 03");
    // Wide widgets take a row each; a third tall row doesn't fit.
    CHECK_EQ(spots({4, 4, 4}), "00 10 30!");
    CHECK_EQ(spots({4, 1, 1, 1, 1, 4}), "00 10 11 12 13 20");
    CHECK_EQ(spots({2, 2, 2, 2, 1, 1, 1, 1}), "00 02 10 12 20 21 22 23");
    CHECK_EQ(spots({2, 1, 1, 1, 1, 1, 1, 2, 2}), "00 02 03 10 11 12 13 20 22");
    CHECK_EQ(spots({2, 1, 1, 2, 2, 2}), "00 02 03 10 12 30!");
    // Three rows of icons: the page holds twelve.
    CHECK(L::fits(std::vector<std::string>(12, "x")));
    CHECK(!L::fits(std::vector<std::string>(13, "x")));
}

void test_text()
{
    CHECK(L::parse("").version == 1);
    CHECK(L::parse("glimdock,home|webcam#gallery").version == 1);
    CHECK(L::parse("@2").version == 2);
    CHECK(L::parse("@2,glimdock").version == 2);
    CHECK(L::parse("@2|webcam").version == 2);
    CHECK(L::parse("@2#gallery").version == 2);
    CHECK(L::parse("@2x,glimdock").version == 1);  // not the header: a (gone) key
    const std::string v2 = "@2,w2:clock,glimdock,home|w4:weather,webcam#gallery";
    CHECK_EQ(L::serialize(L::parse(v2)), v2);
    CHECK_EQ(L::serialize(L::parse("@2|webcam")), "@2|webcam");
    L::Layout l = L::parse("@2,a,,b|c#d,e");
    CHECK(l.pages.size() == 2 && l.pages[0].size() == 2 && l.hidden.size() == 2);
}

void test_migration()
{
    // A fresh board (nothing saved), with the old defaults (Glimdock, Lights):
    // the widgets on top, eight apps under them, the rest on page 2.
    CHECK_EQ(load(""), "@2,w2:glimdock,w2:lights,glimdock,home,swifty,camera,gallery,files,tools,settings"
             "|webcam,interaction,calculator,vpn,games");
    // The text firmware 1.2 saved, rearranged, with hidden apps, a shortcut and
    // other widget kinds: every page and the hidden apps stay as they were.
    const std::string old = "calculator,glimdock,home,swifty,camera,files,tools,settings"
                            "|webcam,sc:movie,interaction,vpn,games#gallery";
    CHECK_EQ(load(old, {"w2:clock", "w2:music"}),
             "@2,w2:clock,w2:music,calculator,glimdock,home,swifty,camera,files,tools,settings"
             "|webcam,sc:movie,interaction,vpn,games#gallery");
    // A first page that had fewer icons keeps its gap.
    CHECK_EQ(load("glimdock,home|swifty,camera,gallery,files,tools,settings,webcam,interaction,calculator,vpn,games"),
             "@2,w2:glimdock,w2:lights,glimdock,home"
             "|swifty,camera,gallery,files,tools,settings,webcam,interaction,calculator,vpn,games");
    // Migrated once only: a version 2 text keeps its own widgets.
    const std::string mine = "@2,glimdock,home,swifty,camera|gallery,files,tools,settings,webcam,interaction,calculator,"
                             "vpn,games";
    CHECK_EQ(load(mine), mine);
}

void test_edits()
{
    L::Layout l = L::parse(load(""));
    // A widget onto page 2's start: page 2's icons make room.
    L::move(l, "w2:lights", 1, 0, kind);
    L::normalize(l, APPS, kind, keep, false);
    CHECK_EQ(L::serialize(l), "@2,w2:glimdock,glimdock,home,swifty,camera,gallery,files,tools,settings"
             "|w2:lights,webcam,interaction,calculator,vpn,games");
    // A wide one at the top of page 1: the last two apps move on.
    CHECK(L::unhide(l, "w4:weather", 0, kind) == 1);  // page 1 is full, page 2 has room
    L::move(l, "w4:weather", 0, 0, kind);
    L::normalize(l, APPS, kind, keep, false);
    CHECK_EQ(L::serialize(l), "@2,w4:weather,w2:glimdock,glimdock,home,swifty,camera,gallery,files"
             "|tools,settings,w2:lights,webcam,interaction,calculator,vpn,games");
    // A widget changing size where it is.
    L::replace(l, "w2:glimdock", "w4:glimdock", kind);
    L::normalize(l, APPS, kind, keep, false);
    CHECK_EQ(L::serialize(l), "@2,w4:weather,w4:glimdock,glimdock,home,swifty,camera"
             "|gallery,files,tools,settings,w2:lights,webcam,interaction,calculator,vpn,games");
    // Removed, and a widget saved as hidden comes back onto a page.
    L::remove(l, "w4:glimdock");
    L::Layout h = L::parse("@2,glimdock#w2:clock,gallery");
    L::normalize(h, APPS, kind, keep, false);
    CHECK(!L::is_hidden(h, "w2:clock") && L::is_hidden(h, "gallery"));
    // Gone widgets (an unknown kind, from newer firmware) are dropped.
    L::Layout g = L::parse("@2,w2:radar,glimdock");
    L::normalize(g, APPS, kind, keep, false);
    CHECK(!L::contains(g, "w2:radar"));
    // A page full of widgets: the next one moves on.
    L::Layout w = L::parse("@2,w4:clock,w4:weather,w4:music,glimdock");
    L::normalize(w, APPS, kind, keep, false);
    CHECK(w.pages.size() >= 2 && w.pages[1][0] == "w4:music");
    // Pending tiles take no cell: twelve apps and a pending tile fit a page.
    L::Layout p = L::parse("@2,glimdock,home,swifty,camera,gallery,files,tools,settings,sc:x,webcam,interaction,"
                           "calculator,vpn");
    L::normalize(p, APPS, kind, keep, false);
    CHECK(p.pages[0].size() == 13);
    int page = -1, index = -1;
    CHECK(L::find(p, "webcam", kind, &page, &index) && page == 0 && index == 8);
}
}  // namespace

int main()
{
    test_widths();
    test_pack();
    test_text();
    test_migration();
    test_edits();
    std::printf("launcher_layout: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
