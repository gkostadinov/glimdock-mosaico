// SPDX-License-Identifier: Apache-2.0
// The home screen's arrangement: pages of item keys (apps, shortcut tiles and
// widgets) and the hidden ones. Plain data, no LVGL: the launcher edits a
// Layout and saves its text in NVS.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace launcher_layout {

// A page is a grid of COLS columns and at most ROWS rows. An icon takes one
// cell; a widget a whole row's height and 2 or 4 columns. A row with a widget
// in it is taller (the widget's card is), so a page has room for at most
// MAX_TALL of them: two widget rows and one of icons fill a page.
constexpr int COLS = 4;
constexpr int ROWS = 3;
constexpr int MAX_TALL = 2;
constexpr int MAX_PAGES = 8;
constexpr size_t MAX_KEY = 23;
constexpr int VERSION = 2;

// A widget's key: "w2:" (2 x 1 cells) or "w4:" (4 x 1) then its kind, and "~n"
// for another of the same: "w2:weather", "w4:clock~1". Everything else is an
// icon (an app or a shortcut tile).
int width(const std::string &key);  // 1, 2 or 4
inline bool is_widget(const std::string &key)
{
    return width(key) > 1;
}

// Where the items of a page go, in order: first fit, reading order, a 2-wide
// widget in the left or right half, a 4-wide one across. An icon fills a cell
// a widget's alignment left free. Items that would need a fourth row, or a
// third tall one, don't fit (they go on to the next page).
struct Spot {
    int row = 0, col = 0, width = 1;
    bool fits = true;
};
struct Grid {
    std::vector<Spot> spots;  // one per item
    int rows = 0;             // rows in use
    bool tall[ROWS] = {};     // rows with a widget
};
Grid pack(const std::vector<int> &widths);
Grid pack(const std::vector<std::string> &keys);
bool fits(const std::vector<std::string> &keys);

using Page = std::vector<std::string>;
struct Layout {
    std::vector<Page> pages;
    std::vector<std::string> hidden;  // in the order they were hidden
    // The text's format. 1: before widgets were items (two fixed widgets
    // above the first page's two rows of icons); migrate() makes it 2.
    int version = VERSION;
};

// What a key stands for now. A shortcut tile whose engine has not registered
// it yet (after a restart), or an app (and its widgets) a filter hides, keeps
// its place but takes no cell meanwhile.
enum class Kind { Shown, Pending, Gone };
using Classify = std::function<Kind(const std::string &key)>;

// Text form, as saved: "@2" first, pages split by '|', the hidden keys after
// '#', keys by ','. "@2,w2:clock,glimdock,home|webcam#gallery". Version 1
// had no "@2" (and so parses as version 1); firmware before widgets reads "@2"
// and the widget keys as gone and drops them. Malformed keys are dropped.
Layout parse(const std::string &text);
std::string serialize(const Layout &layout);
// Version 1 to 2: the two fixed widgets (their keys, left then right) become
// the first page's first items, where they were: the home screen looks the same.
void migrate(Layout &layout, const std::vector<std::string> &widgets);

// The keys of a page that take a cell, in order.
std::vector<std::string> shown(const Page &page, const Classify &kind);

// Makes the layout whole: every key of `order` (the apps, then the registered
// shortcuts) exactly once, on a page or hidden, new ones at the end of the
// last page; gone keys and duplicates dropped; `keep` keys never hidden; every
// page's items fitting (the last ones move on to the next page); empty pages
// after the first removed, but one empty page kept at the end when `tail` (for
// dragging onto a new page while editing). Widgets are never added: only
// placed where they are.
void normalize(Layout &layout, const std::vector<std::string> &order, const Classify &kind,
               const std::function<bool(const std::string &)> &keep, bool tail);

// Puts key on page before the item now at shown index (at the end past the
// last). Items that no longer fit move on to the next page.
void move(Layout &layout, const std::string &key, int page, int index, const Classify &kind);
void hide(Layout &layout, const std::string &key);
// Back from hidden (or new: a widget) onto the first page from `page` on with
// room for it (a new page at the end otherwise). Returns that page.
int unhide(Layout &layout, const std::string &key, int page, const Classify &kind);
void remove(Layout &layout, const std::string &key);
// Puts `with` where `key` is (a widget changing kind or size).
void replace(Layout &layout, const std::string &key, const std::string &with, const Classify &kind);
// Where key shows: page and shown index. False when hidden, pending or absent.
bool find(const Layout &layout, const std::string &key, const Classify &kind, int *page, int *index);
bool is_hidden(const Layout &layout, const std::string &key);
bool contains(const Layout &layout, const std::string &key);

}  // namespace launcher_layout
