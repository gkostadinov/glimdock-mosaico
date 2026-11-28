// SPDX-License-Identifier: Apache-2.0
// Swifty and Paseo: Swifty reads and drives the person's Paseo agents through
// the daemon's agent MCP endpoint (http(s)://daemon:port/mcp/agents, the
// daemon password as a bearer token). The address, password, an optional CA
// and the provider for new agents are Swifty's paseo_* settings
// (tools/mosaico-config.py, Web settings), never compiled in.
//
// Swifty's tools: paseo_list_agents, paseo_read (an agent's latest reply or
// progress), paseo_send (a message to an agent, or a new agent in a
// workspace), paseo_workspaces and paseo_schedules (list, or run one now).
// Anything that makes an agent act shows a card on the screen first and goes
// ahead only when the person taps it.
//
// Service 0x4744 method 64 (CONTRIBUTING.md): a read-only call for testing
// ("agents", "workspaces", "schedules" or "read NAME"), then its result.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Whether this board's profile lets Swifty use Paseo. Every paseo_* tool asks
// it first. The profiles component wires it to profiles_can(Capability::paseo);
// until then it is always true.
bool swifty_paseo_allowed(void);

// Registers the paseo_* tools and RPC 64; swifty_app_init calls it.
void swifty_paseo_start(void);

#ifdef __cplusplus
}
#endif
