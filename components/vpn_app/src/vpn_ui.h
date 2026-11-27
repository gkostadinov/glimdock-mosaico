// SPDX-License-Identifier: Apache-2.0
// The VPN's screen (vpn_ui.cpp): a big switch, the connection's state and
// numbers, and the start-up option. It reads vpn_app_summary() only, so the
// host preview draws it with made-up data.
#pragma once

#include "vpn_app.h"

namespace vpn {
void ui_set_hooks(const vpn_hooks_t *hooks);
}  // namespace vpn
