// SPDX-License-Identifier: Apache-2.0
#include "launcher_layout.h"

#include <algorithm>
#include <set>

namespace launcher_layout {
namespace {
const char HEADER[] = "@2";

bool valid_key(const std::string &k)
{
    return !k.empty() && k.size() <= MAX_KEY && k.find_first_of(",|# \n\r\t") == std::string::npos;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return out;
        }
        start = end + 1;
    }
}

// Takes key out of wherever it is.
void take(Layout &l, const std::string &key)
{
    for (Page &p : l.pages) {
        p.erase(std::remove(p.begin(), p.end(), key), p.end());
    }
    l.hidden.erase(std::remove(l.hidden.begin(), l.hidden.end(), key), l.hidden.end());
}

bool page_fits(const Page &page, const Classify &kind)
{
    return fits(shown(page, kind));
}

// Every page's items fitting: the last shown item moves to the front of the
// next page until they do.
void overflow(Layout &l, const Classify &kind)
{
    for (size_t p = 0; p < l.pages.size(); ++p) {
        while (!page_fits(l.pages[p], kind)) {
            Page &page = l.pages[p];
            auto last = std::find_if(page.rbegin(), page.rend(), [&](const std::string & k) {
                return kind(k) == Kind::Shown;
            });
            const std::string key = *last;
            page.erase(std::next(last).base());
            if (p + 1 == l.pages.size()) {
                l.pages.emplace_back();
            }
            l.pages[p + 1].insert(l.pages[p + 1].begin(), key);
        }
    }
}
}  // namespace

int width(const std::string &key)
{
    if (key.size() > 3 && key[0] == 'w' && key[2] == ':') {
        if (key[1] == '2') {
            return 2;
        }
        if (key[1] == '4') {
            return 4;
        }
    }
    return 1;
}

Grid pack(const std::vector<int> &widths)
{
    Grid g;
    bool used[ROWS][COLS] = {};
    int talls = 0;
    for (int w : widths) {
        w = w >= COLS ? COLS : w >= 2 ? 2 : 1;
        Spot s;
        s.width = w;
        s.fits = false;
        const int step = w == 1 ? 1 : w;  // widgets on a half (or across)
        for (int r = 0; r < ROWS && !s.fits; ++r) {
            if (w > 1 && !g.tall[r] && talls >= MAX_TALL) {
                continue;  // a third tall row would not fit on the page
            }
            for (int c = 0; c + w <= COLS; c += step) {
                bool free = true;
                for (int i = c; i < c + w; ++i) {
                    free = free && !used[r][i];
                }
                if (!free) {
                    continue;
                }
                for (int i = c; i < c + w; ++i) {
                    used[r][i] = true;
                }
                if (w > 1 && !g.tall[r]) {
                    g.tall[r] = true;
                    ++talls;
                }
                s.row = r;
                s.col = c;
                s.fits = true;
                g.rows = std::max(g.rows, r + 1);
                break;
            }
        }
        if (!s.fits) {
            s.row = ROWS;  // past the page
        }
        g.spots.push_back(s);
    }
    return g;
}

Grid pack(const std::vector<std::string> &keys)
{
    std::vector<int> widths;
    widths.reserve(keys.size());
    for (const std::string &k : keys) {
        widths.push_back(width(k));
    }
    return pack(widths);
}

bool fits(const std::vector<std::string> &keys)
{
    for (const Spot &s : pack(keys).spots)
        if (!s.fits) {
            return false;
        }
    return true;
}

Layout parse(const std::string &input)
{
    Layout l;
    std::string text = input;
    const size_t h = sizeof(HEADER) - 1;
    if (text.compare(0, h, HEADER) == 0 && (text.size() == h || text.find_first_of(",|#", h) == h)) {
        text.erase(0, text.size() > h && text[h] == ',' ? h + 1 : h);
        l.version = VERSION;
    } else {
        l.version = 1;  // older text (or none: a fresh board), still to migrate
    }
    if (text.empty()) {
        return l;
    }
    const size_t hash = text.find('#');
    const std::string pages = text.substr(0, hash);
    for (const std::string &p : split(pages, '|')) {
        Page page;
        if (!p.empty()) {
            for (const std::string &k : split(p, ','))
                if (valid_key(k)) {
                    page.push_back(k);
                }
        }
        l.pages.push_back(page);
    }
    if (hash != std::string::npos && hash + 1 < text.size()) {
        for (const std::string &k : split(text.substr(hash + 1), ','))
            if (valid_key(k)) {
                l.hidden.push_back(k);
            }
    }
    return l;
}

std::string serialize(const Layout &l)
{
    std::string out = HEADER;
    if (!l.pages.empty() && !l.pages[0].empty()) {
        out += ',';
    }
    for (size_t p = 0; p < l.pages.size(); ++p) {
        if (p) {
            out += '|';
        }
        for (size_t i = 0; i < l.pages[p].size(); ++i) {
            if (i) {
                out += ',';
            }
            out += l.pages[p][i];
        }
    }
    if (!l.hidden.empty()) {
        out += '#';
        for (size_t i = 0; i < l.hidden.size(); ++i) {
            if (i) {
                out += ',';
            }
            out += l.hidden[i];
        }
    }
    return out;
}

void migrate(Layout &l, const std::vector<std::string> &widgets)
{
    if (l.version >= VERSION) {
        return;
    }
    if (l.pages.empty()) {
        l.pages.emplace_back();
    }
    Page &first = l.pages[0];
    first.insert(first.begin(), widgets.begin(), widgets.end());
    l.version = VERSION;
}

std::vector<std::string> shown(const Page &page, const Classify &kind)
{
    std::vector<std::string> out;
    for (const std::string &k : page)
        if (kind(k) == Kind::Shown) {
            out.push_back(k);
        }
    return out;
}

void normalize(Layout &l, const std::vector<std::string> &order, const Classify &kind,
               const std::function<bool(const std::string &)> &keep, bool tail)
{
    // Duplicates and gone keys out; keep keys back onto the last page.
    std::set<std::string> seen;
    for (Page &p : l.pages) {
        Page kept;
        for (const std::string &k : p)
            if (kind(k) != Kind::Gone && seen.insert(k).second) {
                kept.push_back(k);
            }
        p.swap(kept);
    }
    if (l.pages.empty()) {
        l.pages.emplace_back();
    }
    std::vector<std::string> hidden;
    for (const std::string &k : l.hidden) {
        if (kind(k) == Kind::Gone || !seen.insert(k).second) {
            continue;
        }
        if ((keep && keep(k)) || is_widget(k)) {  // a widget is removed, never hidden
            l.pages.back().push_back(k);
        } else {
            hidden.push_back(k);
        }
    }
    l.hidden.swap(hidden);
    // New items at the end of the last page that shows something.
    size_t last = 0;
    for (size_t p = 0; p < l.pages.size(); ++p)
        if (!shown(l.pages[p], kind).empty()) {
            last = p;
        }
    for (const std::string &k : order)
        if (!seen.count(k) && kind(k) == Kind::Shown) {
            seen.insert(k);
            l.pages[last].push_back(k);
        }
    // A page left with only pending items hands them to the page before (they
    // take no cell until shown), then empty pages after the first go.
    for (size_t p = l.pages.size(); p-- > 1;) {
        if (shown(l.pages[p], kind).empty()) {
            Page &prev = l.pages[p - 1];
            prev.insert(prev.end(), l.pages[p].begin(), l.pages[p].end());
            l.pages.erase(l.pages.begin() + long(p));
        }
    }
    overflow(l, kind);
    if (tail) {
        l.pages.emplace_back();
    }
}

void move(Layout &l, const std::string &key, int page, int index, const Classify &kind)
{
    take(l, key);
    if (page < 0) {
        page = 0;
    }
    while (int(l.pages.size()) <= page) {
        l.pages.emplace_back();
    }
    Page &p = l.pages[size_t(page)];
    int seen = 0;
    auto at = p.end();
    for (auto it = p.begin(); it != p.end(); ++it)
        if (kind(*it) == Kind::Shown && seen++ == index) {
            at = it;
            break;
        }
    p.insert(at, key);
    overflow(l, kind);
}

void hide(Layout &l, const std::string &key)
{
    take(l, key);
    l.hidden.push_back(key);
}

int unhide(Layout &l, const std::string &key, int page, const Classify &kind)
{
    take(l, key);
    page = std::clamp(page, 0, std::max(0, int(l.pages.size()) - 1));
    for (size_t p = size_t(page); p < l.pages.size(); ++p) {
        std::vector<std::string> keys = shown(l.pages[p], kind);
        keys.push_back(key);
        if (fits(keys)) {
            l.pages[p].push_back(key);
            return int(p);
        }
    }
    // A page left empty at the end (while editing) takes it, else a new one.
    if (l.pages.empty() || !shown(l.pages.back(), kind).empty()) {
        l.pages.emplace_back();
    }
    l.pages.back().push_back(key);
    return int(l.pages.size()) - 1;
}

void remove(Layout &l, const std::string &key)
{
    take(l, key);
}

void replace(Layout &l, const std::string &key, const std::string &with, const Classify &kind)
{
    if (key == with) {
        return;
    }
    take(l, with);
    for (Page &p : l.pages)
        for (std::string &k : p)
            if (k == key) {
                k = with;
                overflow(l, kind);
                return;
            }
}

bool find(const Layout &l, const std::string &key, const Classify &kind, int *page, int *index)
{
    for (size_t p = 0; p < l.pages.size(); ++p) {
        int i = 0;
        for (const std::string &k : l.pages[p]) {
            if (kind(k) != Kind::Shown) {
                continue;
            }
            if (k == key) {
                *page = int(p);
                *index = i;
                return true;
            }
            ++i;
        }
    }
    return false;
}

bool is_hidden(const Layout &l, const std::string &key)
{
    return std::find(l.hidden.begin(), l.hidden.end(), key) != l.hidden.end();
}

bool contains(const Layout &l, const std::string &key)
{
    for (const Page &p : l.pages)
        if (std::find(p.begin(), p.end(), key) != p.end()) {
            return true;
        }
    return is_hidden(l, key);
}

}  // namespace launcher_layout
