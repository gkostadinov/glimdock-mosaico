// SPDX-License-Identifier: Apache-2.0
// Paseo's agent MCP endpoint (/mcp/agents on the daemon's HTTP port), as plain
// data: the JSON-RPC calls Swifty sends, the replies it reads and the short
// texts it gives the model. No I/O here, so the host test
// (components/swifty_app/test/paseo_proto_test.cpp) runs it against saved
// replies.
//
// The endpoint is MCP's Streamable HTTP in stateless mode: every POST of
// {"jsonrpc":"2.0","method":"tools/call",...} stands alone (no initialize, no
// session), with "Accept: application/json, text/event-stream" and the daemon
// password as "Authorization: Bearer". The reply is one server-sent event
// ("data: {...}") or plain JSON. A tool's answer is in result.structuredContent;
// a failure in result.isError with the reason in result.content[0].text, or in
// a JSON-RPC error.
#pragma once

#include <time.h>

#include <string>
#include <vector>

#include "ArduinoJson.h"

namespace swifty {
namespace paseo {

struct Agent {
    std::string id, short_id, title, provider, status, cwd, updated, attention;
    bool needs_you = false;
};

struct Workspace {
    std::string id, title, cwd, kind;
};

struct Schedule {
    std::string id, name, status, prompt, cadence, next_run, last_run;
};

// The endpoint for a configured daemon address: http://host:6767 and
// http://host:6767/ both give http://host:6767/mcp/agents; a URL that already
// names the endpoint is kept.
std::string endpoint(const std::string &base);
// A tools/call request body.
std::string call_body(int id, const char *tool, JsonVariantConst arguments);

// Reads a reply body (an event stream or JSON). On success structured holds
// result.structuredContent and text result.content[0].text (one may be empty);
// otherwise error says why, in words for the person.
bool parse_reply(const std::string &body, int id, JsonDocument &structured, std::string &text, std::string &error);
// What an HTTP failure means (401, 403, 404, ...).
std::string http_error(int status, const std::string &body);

std::vector<Agent> agents_from(JsonVariantConst structured);
std::vector<Workspace> workspaces_from(JsonVariantConst structured);
std::vector<Schedule> schedules_from(JsonVariantConst structured);

// Finds an agent (or workspace, schedule) by id, short id, or name (exact,
// then every word of want in it). Returns its index, or -1 with candidates
// set to the names that also fit ("" when none does).
int match_agent(const std::vector<Agent> &agents, const std::string &want, std::string *candidates);
int match_workspace(const std::vector<Workspace> &workspaces, const std::string &want, std::string *candidates);
int match_schedule(const std::vector<Schedule> &schedules, const std::string &want, std::string *candidates);

// Short texts for the model (and through it, the round screen): an agent's
// name (its title, or short id), its state ("working, active 5 min ago, in
// glimdock (codex)") and both in one line.
std::string agent_name(const Agent &a);
std::string agent_state(const Agent &a, time_t now);
std::string agent_line(const Agent &a, time_t now);
std::string describe_agents(const std::vector<Agent> &agents, time_t now, size_t max_bytes);
std::string describe_workspaces(const std::vector<Workspace> &workspaces, size_t max_bytes);
std::string describe_schedules(const std::vector<Schedule> &schedules, time_t now, size_t max_bytes);
// The end of an agent's activity (get_agent_activity's content): its newest
// lines, whole, within max_bytes.
std::string activity_tail(const std::string &content, size_t max_bytes);

// ISO 8601 UTC ("2026-11-27T18:48:56.980Z") to a time_t (0 when unreadable),
// and "5 min ago" from it (empty while the board's clock is unset).
time_t parse_time(const std::string &iso);
std::string ago(const std::string &iso, time_t now);
std::string until(const std::string &iso, time_t now);

// text cut to at most max bytes at a character boundary (with an ellipsis
// when cut).
std::string clip(const std::string &text, size_t max);

}  // namespace paseo
}  // namespace swifty
