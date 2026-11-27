// SPDX-License-Identifier: Apache-2.0
// The home screen's arrangement: pages of item keys (apps and shortcut tiles)
// and the hidden ones. Plain data, no LVGL: the launcher edits a Layout and
// saves its text in NVS.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace launcher_layout {

constexpr int COLS = 4;
constexpr int FIRST_ROWS = 2;  // the first page has the two widgets above its icons
constexpr int ROWS = 3;
constexpr int MAX_PAGES = 8;
constexpr size_t MAX_KEY = 23;
inline int capacity(int page)
{
    return COLS * (page == 0 ? FIRST_ROWS : ROWS);
}

using Page = std::vector<std::string>;
struct Layout {
    std::vector<Page> pages;          // the first is the widgets' page
    std::vector<std::string> hidden;  // in the order they were hidden
};

// What a key stands for now. A shortcut tile whose engine has not registered
// it yet (after a restart) keeps its place but takes no cell meanwhile.
enum class Kind { Shown, Pending, Gone };
using Classify = std::function<Kind(const std::string &key)>;

// Text form, as saved: pages split by '|', the hidden keys after '#', keys by
// ','. "glimdock,home|webcam#gallery". Malformed keys are dropped.
Layout parse(const std::string &text);
std::string serialize(const Layout &layout);

// The keys of a page that take a cell, in order.
std::vector<std::string> shown(const Page &page, const Classify &kind);

// Makes the layout whole: every key of `order` (the apps, then the registered
// shortcuts) exactly once, on a page or hidden, new ones at the end of the
// last page; gone keys and duplicates dropped; `keep` keys never hidden; no
// page over its capacity (the last ones move on to the next page); empty
// pages after the first removed, but one empty page kept at the end when
// `tail` (for dragging onto a new page while editing).
void normalize(Layout &layout, const std::vector<std::string> &order, const Classify &kind,
               const std::function<bool(const std::string &)> &keep, bool tail);

// Puts key on page before the item now at shown index (at the end past the
// last). A full page passes its last item on to the next.
void move(Layout &layout, const std::string &key, int page, int index, const Classify &kind);
void hide(Layout &layout, const std::string &key);
// Back from hidden onto the first page from `page` on with room (a new page
// at the end otherwise).
void unhide(Layout &layout, const std::string &key, int page, const Classify &kind);
void remove(Layout &layout, const std::string &key);
// Where key shows: page and shown index. False when hidden, pending or absent.
bool find(const Layout &layout, const std::string &key, const Classify &kind, int *page, int *index);
bool is_hidden(const Layout &layout, const std::string &key);

}  // namespace launcher_layout
