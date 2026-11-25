// SPDX-License-Identifier: Apache-2.0
// The preview has no Interaction module: Tools' Interaction tab shows a note.
#include "../components/tools_app/src/interact.h"

namespace interact {
void start() {}

void build(lv_obj_t *pane, const mui::Accent &)
{
    lv_obj_t *note = lv_label_create(pane);
    lv_label_set_text(note, "Interaction module (not in the preview)");
    lv_obj_set_style_text_color(note, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 120);
}

void update() {}
void set_visible(bool) {}
}  // namespace interact
