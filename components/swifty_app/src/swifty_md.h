// SPDX-License-Identifier: Apache-2.0
// Markdown for Swifty's replies, as LVGL objects: headings, paragraphs with
// bold, italic, strikethrough, inline code and links, bullet and numbered
// lists, quotes, code blocks, rules, simple tables, and images on their own
// line (handed to a loader, since they are fetched).
#pragma once

#include <functional>
#include <string>

#include "lvgl.h"

namespace swifty {

using ImageLoader = std::function<void(const std::string &url, const std::string &alt, lv_obj_t *holder)>;

// Replaces parent's children (parent should be a flex column of the given
// width) with the rendered text.
void md_render(lv_obj_t *parent, const std::string &text, int width, const ImageLoader &images);

}  // namespace swifty
