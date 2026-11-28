// SPDX-License-Identifier: Apache-2.0
// Settings > Wi-Fi (see wifi_settings.h): pure LVGL over board_wifi.h, so the
// desktop preview shows it with demo networks.
#include "wifi_settings.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "board_wifi.h"
#include "mui.h"

namespace {
using namespace mui;

// Clears typed passwords from memory (a plain memset may be optimised away).
void scrub(std::string &s)
{
    volatile char *p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; p && i < s.size(); ++i) {
        p[i] = 0;
    }
}

enum class Page : uint8_t { List, Password, Name };

struct {
    lv_obj_t *root = nullptr, *page = nullptr;
    lv_timer_t *timer = nullptr;
    // List
    lv_obj_t *dot = nullptr, *name = nullptr, *detail = nullptr, *forget = nullptr, *list = nullptr,
              *scanning = nullptr, *rescan = nullptr;
    bool forget_armed = false, results_shown = false;
    std::string shown_key;  // what the status card shows, to rebuild it only on a change
    // Password and name pages
    lv_obj_t *area = nullptr, *show = nullptr;
    std::string target;    // the network a password is for
    std::string note;      // a line under the field (why the last try failed)
    std::vector<std::string> rows;  // the list rows' names (their user data points here)
    Page go = Page::List;
} W;

void show_page(Page page);

// Pages change after the tap that asked (the tapped object is on the old page).
void go(Page page)
{
    W.go = page;
    lv_async_call([](void *) {
        if (W.root) {
            show_page(W.go);
        }
    }, nullptr);
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int size, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent, x, y, size, size, size / 2);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    const int g = size / 2;
    icon(b, glyph, (size - g) / 2, (size - g) / 2, g, TEXT, 2.2f);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

lv_obj_t *frame(const char *title, bool back)
{
    if (W.page) {
        lv_obj_delete(W.page);
    }
    W.page = box(W.root, 0, 0, PANEL, PANEL);
    W.dot = W.name = W.detail = W.forget = W.list = W.scanning = W.rescan = W.area = W.show = nullptr;
    W.shown_key.clear();
    W.results_shown = false;
    lv_obj_t *t = label(W.page, title, 48, 44, 300, TEXT, 26, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    round_button(W.page, PANEL - 96, 36, 48, back ? "back" : "x", back ? +[](lv_event_t *) {
        go(Page::List);
    } : +[](lv_event_t *) {
        lv_async_call([](void *) {
            wifi_settings_close();
        }, nullptr);
    });
    return W.page;
}

// Signal strength as four rising bars.
void bars(lv_obj_t *parent, int x, int y, int rssi, uint32_t on_color)
{
    const int lit = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
        const int h = 6 + 4 * i;
        lv_obj_t *b = box(parent, x + 7 * i, y + 18 - h, 4, h, 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < lit ? on_color : OFF), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
}

// ---- the list --------------------------------------------------------------------------
void update_status()
{
    if (!W.name) {
        return;
    }
    const BoardWifiStatus s = board_wifi_status();
    std::string joining;
    const BoardWifiJoin join = board_wifi_join_state(&joining);
    std::string title, sub;
    uint32_t color = OFF;
    bool can_forget = false, trouble = false;
    if (join == BoardWifiJoin::Joining) {
        title = joining;
        sub = "Joining\xE2\x80\xA6";
        color = WARN;
    } else if (s.ssid.empty()) {
        title = "Not connected";
        sub = "Choose a network below";
        color = RED;
    } else {
        title = s.ssid;
        color = s.connected ? OK : WARN;
        sub = s.connected ? "Connected \xC2\xB7 " + s.ip : "Connecting\xE2\x80\xA6";
        if (s.fallback) {
            sub += s.connected ? " \xC2\xB7 your choice is out of range" : "";
        } else if (s.vibe) {
            sub += " \xC2\xB7 from Vibe Mode";
        }
        can_forget = s.saved;
    }
    // The last join's result, until the next one.
    if (join == BoardWifiJoin::Joined) {
        sub = "Joined \xC2\xB7 " + (s.connected ? s.ip : std::string("getting an address"));
    } else if (join == BoardWifiJoin::NotFound) {
        sub = "Couldn't find \xE2\x80\x9C" + joining + "\xE2\x80\x9D";
        trouble = true;
    } else if (join == BoardWifiJoin::Failed) {
        sub = "\xE2\x80\x9C" + joining + "\xE2\x80\x9D didn't let the board in";
        trouble = true;
    }
    const std::string key = title + "|" + sub + "|" + std::to_string(color) + (can_forget ? "|f" : "") + (trouble ? "|t" : "");
    if (key == W.shown_key) {
        return;
    }
    W.shown_key = key;
    set_bg_color(W.dot, lv_color_hex(color));
    set_text(W.name, title.c_str());
    set_text(W.detail, sub.c_str());
    set_text_color(W.detail, lv_color_hex(trouble ? WARN : TEXT2));
    lv_obj_set_width(W.detail, can_forget ? 270 : PANEL - 80 - 40);
    set_hidden(W.forget, !can_forget);
    if (!can_forget) {
        W.forget_armed = false;
    }
}

void row_tapped(lv_event_t *e)
{
    const std::string *ssid = static_cast<const std::string *>(lv_event_get_user_data(e));
    if (!ssid || board_wifi_join_state() == BoardWifiJoin::Joining) {
        return;
    }
    if (ssid->empty()) {  // Other network...
        W.target.clear();
        W.note.clear();
        go(Page::Name);
        return;
    }
    const BoardWifiStatus s = board_wifi_status();
    if (s.connected && s.ssid == *ssid) {
        return;  // already on it
    }
    for (const BoardWifiNetwork &n : board_wifi_results()) {
        if (n.ssid != *ssid) {
            continue;
        }
        if (n.saved || !n.secure) {
            board_wifi_join(n.ssid.c_str(), nullptr);  // the saved password, or none
            return;
        }
    }
    W.target = *ssid;
    W.note.clear();
    go(Page::Password);
}

lv_obj_t *network_row(int y, const std::string *ssid, const char *title, const char *sub, int rssi, bool secure,
                      bool current)
{
    const int w = PANEL - 80, h = 58;
    lv_obj_t *r = glass(W.list, 0, y, w, h, 20, current ? GREEN : CYAN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    press_fx(r);
    if (rssi) {
        bars(r, 18, 20, rssi, current ? OK : TEXT);
    } else {
        icon(r, "pencil", 16, 17, 24, TEXT2, 2.0f);
    }
    const bool two = sub && *sub;
    lv_obj_t *t = label(r, title, 60, two ? 9 : 17, w - 120, current ? OK : TEXT, 17, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (two) {
        lv_obj_t *d = label(r, sub, 60, 33, w - 120, TEXT3, 13);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    }
    if (secure) {
        icon(r, "lock", w - 44, 19, 20, TEXT3, 1.8f);
    }
    lv_obj_add_event_cb(r, row_tapped, LV_EVENT_CLICKED, const_cast<std::string *>(ssid));
    return r;
}

void fill_list()
{
    if (!W.list) {
        return;
    }
    lv_obj_clean(W.list);
    const std::vector<BoardWifiNetwork> found = board_wifi_results();
    const BoardWifiStatus s = board_wifi_status();
    W.rows.clear();
    W.rows.reserve(found.size() + 1);  // the rows keep pointers into it
    int y = 0;
    // The board's network first, then the ones it knows, then the rest by strength.
    std::vector<const BoardWifiNetwork *> order;
    for (int pass = 0; pass < 3; ++pass)
        for (const BoardWifiNetwork &n : found) {
            const int rank = n.ssid == s.ssid ? 0 : n.saved ? 1 : 2;
            if (rank == pass) {
                order.push_back(&n);
            }
        }
    for (const BoardWifiNetwork *n : order) {
        W.rows.push_back(n->ssid);
        const bool current = n->ssid == s.ssid;
        const char *sub = current ? (s.connected ? "Connected" : "Joining\xE2\x80\xA6")
                          : n->vibe ? "Saved in Vibe Mode"
                          : n->saved ? "Saved"
                          : !n->secure ? "Open network" : "";
        network_row(y, &W.rows.back(), n->ssid.c_str(), sub, n->rssi, n->secure, current);
        y += 64;
    }
    if (order.empty()) {
        lv_obj_t *n = label(W.list, board_wifi_scanning() ? "Looking for networks\xE2\x80\xA6" : "No networks found. Scan again, or add one by its name.",
                            8, 4, PANEL - 96, TEXT2, 15);
        wrap_label(n);
        y += 56;
    }
    W.rows.push_back(std::string());
    network_row(y, &W.rows.back(), "Other network\xE2\x80\xA6", "A hidden one, by its name", 0, false, false);
    box(W.list, 0, y + 64, 1, 40);  // room to scroll the last row clear of the rounded corner
    W.results_shown = !board_wifi_scanning();
}

void list_page()
{
    lv_obj_t *p = frame("Wi-Fi", false);
    lv_obj_t *card = glass(p, 40, 100, PANEL - 80, 74, 24, GREEN);
    W.dot = dot(card, 20, 21, 10, OFF, false);
    W.name = label(card, "", 40, 12, 230, TEXT, 18, true);
    lv_label_set_long_mode(W.name, LV_LABEL_LONG_MODE_DOTS);
    W.detail = label(card, "", 20, 42, 270, TEXT2, 14);
    lv_label_set_long_mode(W.detail, LV_LABEL_LONG_MODE_DOTS);
    W.forget = field_button(card, PANEL - 80 - 104, 19, 90, 36, "Forget", 15, RED);
    W.forget_armed = false;
    lv_obj_add_event_cb(W.forget, [](lv_event_t *e) {
        lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        if (!W.forget_armed) {
            W.forget_armed = true;  // a second tap forgets
            if (lv_obj_t *text = lv_obj_get_child(button, 0)) {
                set_text(text, "Sure?");
            }
            return;
        }
        const BoardWifiStatus s = board_wifi_status();
        board_wifi_forget(s.ssid.c_str());
        go(Page::List);
    }, LV_EVENT_CLICKED, nullptr);

    label(p, "Networks", 48, 190, 200, TEXT3, 14, true);
    W.scanning = label(p, "Scanning\xE2\x80\xA6", 200, 190, 170, TEXT3, 14, false, LV_TEXT_ALIGN_RIGHT);
    W.rescan = round_button(p, PANEL - 84, 180, 36, "search", [](lv_event_t *) {
        if (board_wifi_scan()) {
            W.results_shown = false;
            set_hidden(W.scanning, false);
        }
    });
    W.list = box(p, 40, 218, PANEL - 80, PANEL - 218 - 24);
    lv_obj_add_flag(W.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(W.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(W.list, LV_SCROLLBAR_MODE_OFF);
    update_status();
    fill_list();
    set_hidden(W.scanning, !board_wifi_scanning());
    stagger(W.list, 0, 20);
}

// ---- typing a password or a name -------------------------------------------------------
void typed(lv_event_t *e);

lv_obj_t *field(lv_obj_t *p, const char *text, uint32_t max, bool secret)
{
    lv_obj_t *ta = lv_textarea_create(p);
    lv_obj_set_pos(ta, 24, 92);
    lv_obj_set_size(ta, secret ? 340 : 432, 58);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max);
    lv_textarea_set_text(ta, text);
    lv_textarea_set_password_mode(ta, secret);
    lv_obj_set_style_bg_color(ta, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(GREEN.glow), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_pad_ver(ta, 15, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(ta, font(20, false), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(GREEN.glow), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(ta, 2, LV_PART_CURSOR);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, typed, LV_EVENT_READY, nullptr);
    W.area = ta;
    return ta;
}

void keyboard_for(lv_obj_t *p, lv_obj_t *ta, const char *action)
{
    KeyboardConfig kc;
    kc.action = action;
    kc.accent = &GREEN;
    kc.auto_caps = false;
    keyboard(p, ta, 0, 236, PANEL, 234, kc);
}

void password_page()
{
    lv_obj_t *p = frame(W.target.c_str(), true);
    field(p, "", 64, true);
    W.show = field_button(p, 376, 96, 80, 50, "Show", 15, TEXT2);
    lv_obj_add_event_cb(W.show, [](lv_event_t *) {
        const bool hidden = lv_textarea_get_password_mode(W.area);
        lv_textarea_set_password_mode(W.area, !hidden);
        if (lv_obj_t *text = lv_obj_get_child(W.show, 0)) {
            set_text(text, hidden ? "Hide" : "Show");
        }
    }, LV_EVENT_CLICKED, nullptr);
    const bool failed = !W.note.empty();
    label(p, failed ? W.note.c_str() : "The network's password", 40, 160, 400, failed ? WARN : TEXT3, 14, false,
          LV_TEXT_ALIGN_CENTER);
    keyboard_for(p, W.area, "Join");
}

void name_page()
{
    lv_obj_t *p = frame("Other network", true);
    field(p, W.target.c_str(), 32, false);
    label(p, W.note.empty() ? "Its name, exactly as it is written" : W.note.c_str(), 40, 160, 400,
          W.note.empty() ? TEXT3 : WARN, 14, false, LV_TEXT_ALIGN_CENTER);
    keyboard_for(p, W.area, "Next");
}

void typed(lv_event_t *)
{
    if (!W.area) {
        return;
    }
    std::string text = lv_textarea_get_text(W.area);
    const bool password = W.show != nullptr;  // the password page has Show
    if (!password) {  // the name of a hidden network: then its password
        if (text.empty() || text.size() > 32) {
            W.note = "A name of 1 to 32 characters";
            return go(Page::Name);
        }
        W.target = text;
        W.note = "Leave it empty for an open network";
        return go(Page::Password);
    }
    if (!text.empty() && (text.size() < 8 || text.size() > 64)) {
        W.note = "A Wi-Fi password has 8 to 63 characters";
        scrub(text);
        lv_textarea_set_text(W.area, "");
        return go(Page::Password);
    }
    board_wifi_join(W.target.c_str(), text.c_str());
    scrub(text);
    lv_textarea_set_text(W.area, "");
    go(Page::List);
}

void show_page(Page page)
{
    W.go = page;  // the page in view (Back reads it)
    switch (page) {
    case Page::List:
        list_page();
        break;
    case Page::Password:
        password_page();
        break;
    case Page::Name:
        name_page();
        break;
    }
}

void tick(lv_timer_t *)
{
    if (!W.root) {
        return;
    }
    std::string joining;
    const BoardWifiJoin join = board_wifi_join_state(&joining);
    if (join == BoardWifiJoin::WrongPassword) {
        board_wifi_join_seen();
        W.target = joining;
        W.note = "That password didn't work. Try again.";
        show_page(Page::Password);
        return;
    }
    if (!W.name) {
        return;  // typing
    }
    update_status();
    const bool scanning = board_wifi_scanning();
    set_hidden(W.scanning, !scanning);
    // New results, or a join that changed which network is the board's.
    static std::string listed_for;
    const BoardWifiStatus s = board_wifi_status();
    const std::string now_for = s.ssid + (s.connected ? "+" : "-") + std::to_string(int(join));
    if ((!scanning && !W.results_shown) || now_for != listed_for) {
        listed_for = now_for;
        fill_list();
    }
}
}  // namespace

void wifi_settings_open()
{
    if (W.root) {
        return;
    }
    W.root = box(lv_layer_top(), 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(W.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(W.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(W.root, LV_OBJ_FLAG_CLICKABLE);
    board_wifi_join_seen();  // an old result is not news
    board_wifi_scan();
    show_page(Page::List);
    W.timer = lv_timer_create(tick, 250, nullptr);
}

void wifi_settings_close()
{
    if (W.timer) {
        lv_timer_delete(W.timer);
        W.timer = nullptr;
    }
    if (W.root) {
        lv_obj_delete(W.root);
    }
    W.root = W.page = nullptr;
    W.dot = W.name = W.detail = W.forget = W.list = W.scanning = W.rescan = W.area = W.show = nullptr;
    W.rows.clear();
    W.target.clear();
}

bool wifi_settings_showing()
{
    return W.root != nullptr;
}

bool wifi_settings_back()
{
    if (!W.root) {
        return false;
    }
    if (W.go != Page::List) {
        show_page(Page::List);  // the password or name page: back to the list
    } else {
        wifi_settings_close();
    }
    return true;
}
