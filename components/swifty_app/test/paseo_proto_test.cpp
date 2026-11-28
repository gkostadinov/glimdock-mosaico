// SPDX-License-Identifier: Apache-2.0
// Host test of Swifty's Paseo requests and replies (swifty_paseo_proto.cpp),
// against synthetic daemon replies. From the project root:
//
//   c++ -std=c++17 -Wall -Wextra -I components/swifty_app/src \
//       -I managed_components/bblanchon__arduinojson/src \
//       components/swifty_app/test/paseo_proto_test.cpp components/swifty_app/src/swifty_paseo_proto.cpp \
//       -o build-host/paseo_proto_test && build-host/paseo_proto_test
//
// Against a real daemon, read-only: "--body list_agents" prints the request
// the board sends, and "--agents FILE" reads a saved reply (an event stream)
// and prints only counts, never names:
//
//   build-host/paseo_proto_test --body list_agents | curl -s -X POST http://127.0.0.1:6767/mcp/agents \
//       -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' -d @- > reply.txt
//   build-host/paseo_proto_test --agents reply.txt
//
// "--activity FILE" does the same for a saved get_agent_activity reply.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "swifty_paseo_proto.h"

using namespace swifty::paseo;

namespace {
int failures = 0, checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        ++checks;                                                                                         \
        const std::string va = (a), vb = (b);                                                             \
        if (va != vb) {                                                                                   \
            ++failures;                                                                                   \
            fprintf(stderr, "%s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, va.c_str(), vb.c_str()); \
        }                                                                                                 \
    } while (0)

// 2026-11-28T12:00:00Z
const time_t NOW = parse_time("2026-11-28T12:00:00.000Z");

const char *AGENTS_SSE =
    "event: message\n"
    "data: {\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"agents_count=4\\n\\n{...}\"}],"
    "\"structuredContent\":{\"agents\":["
    "{\"id\":\"11111111-aaaa-4bbb-8ccc-000000000001\",\"shortId\":\"1111111\",\"title\":\"Homelab audit\","
    "\"provider\":\"codex\",\"model\":\"gpt-5.4\",\"status\":\"running\",\"cwd\":\"/home/dev/homelab\","
    "\"createdAt\":\"2026-11-28T10:00:00.000Z\",\"updatedAt\":\"2026-11-28T11:55:00.000Z\",\"archivedAt\":null,"
    "\"requiresAttention\":false,\"attentionReason\":null,\"labels\":{}},"
    "{\"id\":\"22222222-aaaa-4bbb-8ccc-000000000002\",\"shortId\":\"2222222\",\"title\":\"Fix the board's Wi-Fi\","
    "\"provider\":\"claude\",\"status\":\"idle\",\"cwd\":\"/home/dev/glimdock\",\"updatedAt\":\"2026-11-28T09:00:00.000Z\","
    "\"requiresAttention\":true,\"attentionReason\":\"permission\"},"
    "{\"id\":\"33333333-aaaa-4bbb-8ccc-000000000003\",\"shortId\":\"3333333\",\"title\":\"Glimdock release notes\","
    "\"provider\":\"omp\",\"status\":\"closed\",\"cwd\":\"/home/dev/glimdock\",\"updatedAt\":\"2026-11-25T12:00:00.000Z\","
    "\"requiresAttention\":false},"
    "{\"id\":\"44444444-aaaa-4bbb-8ccc-000000000004\",\"shortId\":\"4444444\",\"title\":null,"
    "\"provider\":\"codex\",\"status\":\"error\",\"cwd\":\"/srv/site\",\"updatedAt\":\"2026-11-27T11:00:00.000Z\","
    "\"requiresAttention\":true,\"attentionReason\":\"error\"}"
    "]}},\"jsonrpc\":\"2.0\",\"id\":7}\n"
    "\n";

void test_requests()
{
    CHECK_EQ(endpoint("http://192.0.2.10:6767"), "http://192.0.2.10:6767/mcp/agents");
    CHECK_EQ(endpoint("http://192.0.2.10:6767/"), "http://192.0.2.10:6767/mcp/agents");
    CHECK_EQ(endpoint("https://paseo.example/mcp/agents/ "), "https://paseo.example/mcp/agents");
    CHECK_EQ(endpoint("https://proxy.example/paseo"), "https://proxy.example/paseo/mcp/agents");

    JsonDocument args;
    args["limit"] = 30;
    args["sinceHours"] = 336;
    CHECK_EQ(call_body(7, "list_agents", args),
             R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"list_agents","arguments":{"limit":30,"sinceHours":336}}})");
    CHECK_EQ(call_body(8, "list_workspaces", JsonVariantConst()),
             R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"list_workspaces","arguments":{}}})");
    JsonDocument send;
    send["agentId"] = "x";
    send["prompt"] = "Line one\n\"quoted\"";
    send["activeTurnBehavior"] = "steer";
    CHECK_EQ(call_body(9, "send_agent_prompt", send),
             R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"send_agent_prompt","arguments":{"agentId":"x","prompt":"Line one\n\"quoted\"","activeTurnBehavior":"steer"}}})");
}

void test_replies()
{
    JsonDocument s;
    std::string text, error;
    CHECK(parse_reply(AGENTS_SSE, 7, s, text, error));
    CHECK_EQ(error, "");
    std::vector<Agent> agents = agents_from(s);
    CHECK(agents.size() == 4);
    CHECK_EQ(agents[0].title, "Homelab audit");
    CHECK_EQ(agents[1].attention, "permission");
    CHECK(agents[1].needs_you);
    CHECK_EQ(agents[3].title, "");

    // CRLF event stream, a message for another request first.
    std::string crlf = "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"structuredContent\":{\"agents\":[]}}}\r\n\r\n";
    crlf += "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"structuredContent\":{\"workspaces\":["
            "{\"workspaceId\":\"ws1\",\"projectId\":\"p\",\"cwd\":\"/home/dev/glimdock\",\"isolation\":\"local\","
            "\"kind\":\"local_checkout\",\"title\":null}]}}}\r\n\r\n";
    CHECK(parse_reply(crlf, 4, s, text, error));
    CHECK(workspaces_from(s).size() == 1);

    // Plain JSON (a server that answers application/json).
    CHECK(parse_reply(R"({"jsonrpc":"2.0","id":5,"result":{"structuredContent":{"schedules":[]}}})", 5, s, text, error));
    CHECK(s["schedules"].is<JsonArray>());

    // A tool that failed.
    CHECK(!parse_reply("event: message\ndata: {\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Agent abc not found\"}],"
                       "\"isError\":true},\"jsonrpc\":\"2.0\",\"id\":6}\n\n",
                       6, s, text, error));
    CHECK_EQ(error, "Paseo: Agent abc not found");

    // A JSON-RPC error (no id).
    CHECK(!parse_reply(R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"Method not allowed"},"id":null})", 1, s, text, error));
    CHECK_EQ(error, "Paseo: Method not allowed");

    // Nothing readable.
    CHECK(!parse_reply("<html>proxy error</html>", 1, s, text, error));
    CHECK_EQ(error, "Paseo sent a reply Swifty cannot read.");
    CHECK(!parse_reply("", 1, s, text, error));

    CHECK(http_error(401, "").find("password") != std::string::npos);
    CHECK(http_error(403, "Invalid Host header").find("daemon.hostnames") != std::string::npos);
    CHECK(http_error(404, "").find("/mcp/agents") != std::string::npos);
}

void test_texts()
{
    JsonDocument s;
    std::string text, error;
    parse_reply(AGENTS_SSE, 7, s, text, error);
    const std::vector<Agent> agents = agents_from(s);
    CHECK_EQ(agent_line(agents[0], NOW), "Homelab audit [1111111]: working, active 5 min ago, in homelab (codex)");
    CHECK_EQ(agent_line(agents[1], NOW),
             "Fix the board's Wi-Fi [2222222]: idle, waiting for your permission, active 3 h ago, in glimdock (claude)");
    CHECK_EQ(agent_line(agents[2], NOW), "Glimdock release notes [3333333]: closed (a message wakes it), active 3 days ago, in glimdock (omp)");
    CHECK_EQ(agent_line(agents[3], NOW), "4444444 [4444444]: failed, stopped with an error, active yesterday, in site (codex)");
    const std::string all = describe_agents(agents, NOW, 4000);
    CHECK(all.compare(0, 52, "4 recent Paseo agents (1 working, 2 need you), newes") == 0);
    const std::string small = describe_agents(agents, NOW, 200);
    CHECK(small.find("(and ") != std::string::npos);
    CHECK(small.size() < 260);
    CHECK_EQ(describe_agents({}, NOW, 100), "No recent Paseo agents.");

    // No clock yet: no times.
    CHECK_EQ(agent_line(agents[0], 0), "Homelab audit [1111111]: working, in homelab (codex)");

    JsonDocument sc;
    deserializeJson(sc, R"({"schedules":[
        {"id":"sch1","name":"Morning report","prompt":"Summarise the homelab alerts","status":"active",
         "cadence":{"type":"cron","expression":"0 8 * * *","timezone":"Europe/Berlin"},
         "nextRunAt":"2026-11-29T06:00:00.000Z","lastRunAt":"2026-11-28T06:00:00.000Z"},
        {"id":"sch2","name":null,"prompt":"Check the backups","status":"paused","cadence":{"type":"every","everyMs":3600000},
         "nextRunAt":null,"lastRunAt":null}]})");
    const std::vector<Schedule> schedules = schedules_from(sc);
    CHECK(schedules.size() == 2);
    CHECK_EQ(schedules[1].cadence, "every hour");
    CHECK_EQ(describe_schedules(schedules, NOW, 4000),
             "2 Paseo schedules:\n"
             "- Morning report [sch1]: active, cron 0 8 * * * (Europe/Berlin), next in 18 h, last ran 6 h ago. Task: Summarise the homelab alerts\n"
             "- (unnamed) [sch2]: paused, every hour. Task: Check the backups\n");
    std::string fits;
    CHECK(match_schedule(schedules, "morning report", &fits) == 0);
    CHECK(match_schedule(schedules, "backups", &fits) == 1);
    CHECK(match_schedule(schedules, "sch2", &fits) == 1);
}

void test_matching()
{
    JsonDocument s;
    std::string text, error, fits;
    parse_reply(AGENTS_SSE, 7, s, text, error);
    const std::vector<Agent> agents = agents_from(s);
    CHECK(match_agent(agents, "2222222", &fits) == 1);
    CHECK(match_agent(agents, "33333333-AAAA", &fits) == 2);
    CHECK(match_agent(agents, "homelab audit", &fits) == 0);
    CHECK(match_agent(agents, "the homelab agent", &fits) == 0);
    CHECK(match_agent(agents, "wi-fi", &fits) == 1);
    CHECK(match_agent(agents, "release notes", &fits) == 2);
    CHECK(match_agent(agents, "site", &fits) == 3);  // its folder
    // Two agents work in glimdock: ask.
    CHECK(match_agent(agents, "glimdock", &fits) == -1);
    CHECK_EQ(fits, "Fix the board's Wi-Fi [2222222]; Glimdock release notes [3333333]");
    CHECK(match_agent(agents, "printer", &fits) == -1);
    CHECK_EQ(fits, "");
    CHECK(match_agent(agents, "the agent", &fits) == -1);
    CHECK(match_agent(agents, "  ", &fits) == -1);

    JsonDocument w;
    deserializeJson(w, R"({"workspaces":[
        {"workspaceId":"ws-a","cwd":"/home/dev/glimdock","kind":"local_checkout","title":"Glimdock main"},
        {"workspaceId":"ws-b","cwd":"/home/dev/.paseo/worktrees/glimdock-paseo","kind":"worktree","title":"Paseo for Swifty"}]})");
    const std::vector<Workspace> spaces = workspaces_from(w);
    CHECK(match_workspace(spaces, "Paseo for Swifty", &fits) == 1);
    CHECK(match_workspace(spaces, "ws-a", &fits) == 0);
    CHECK(match_workspace(spaces, "glimdock", &fits) == -1);  // both
    CHECK_EQ(fits, "Glimdock main; Paseo for Swifty");
    CHECK(match_workspace(spaces, "glimdock main", &fits) == 0);
    CHECK_EQ(describe_workspaces(spaces, 1000),
             "2 Paseo workspaces:\n- Glimdock main: /home/dev/glimdock\n- Paseo for Swifty (worktree): /home/dev/.paseo/worktrees/glimdock-paseo\n");
}

void test_activity()
{
    const std::string content =
        "Showing 4 of 136 activities (limited to 4)\n\n"
        "[User] Check the disks on every node\n"
        "[Shell] smartctl -a /dev/sda\n"
        "\n\n\n"
        "All four disks are healthy. The NAS disk 3 has 12 reallocated sectors; watch it.";
    CHECK_EQ(activity_tail(content, 1000),
             "Showing 4 of 136 activities (limited to 4):\n[User] Check the disks on every node\n[Shell] smartctl -a /dev/sda\n\n"
             "All four disks are healthy. The NAS disk 3 has 12 reallocated sectors; watch it.");
    // Cut from the front at a line, and never inside a character.
    std::string long_text = "Showing all 3 activities\n\n";
    for (int i = 0; i < 200; ++i) {
        long_text += "Step " + std::to_string(i) + ": caf\xC3\xA9 done\n";
    }
    long_text += "Final answer: ready.";
    const std::string tail = activity_tail(long_text, 300);
    CHECK(tail.size() <= 300);
    CHECK(tail.compare(0, 24, "Showing all 3 activities") == 0);
    CHECK(tail.find("\xE2\x80\xA6\nStep ") != std::string::npos);
    CHECK(tail.size() >= 20 && tail.compare(tail.size() - 20, 20, "Final answer: ready.") == 0);
    CHECK_EQ(activity_tail("No activity to display.", 100), "No activity to display.");

    CHECK_EQ(clip("caf\xC3\xA9s!!", 7), "caf\xE2\x80\xA6");  // never half an é
    CHECK_EQ(clip("caf\xC3\xA9s!", 5), "ca\xE2\x80\xA6");
    CHECK_EQ(clip("short", 10), "short");
}

void test_time()
{
    CHECK(parse_time("1970-01-01T00:00:00Z") == 0);
    CHECK(parse_time("2026-11-28T12:00:00.000Z") == 1795867200);
    CHECK(parse_time("not a time") == 0);
    CHECK_EQ(ago("2026-11-28T11:59:30.000Z", NOW), "just now");
    CHECK_EQ(ago("2026-11-28T11:00:00.000Z", NOW), "1 h ago");
    CHECK_EQ(ago("2026-11-21T12:00:00.000Z", NOW), "7 days ago");
    CHECK_EQ(until("2026-11-28T12:20:00.000Z", NOW), "in 20 min");
    CHECK_EQ(until("2026-11-28T11:00:00.000Z", NOW), "now");
    CHECK_EQ(until("", NOW), "");
}

std::string read_file(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--body")) {
        JsonDocument args;
        if (!strcmp(argv[2], "list_agents")) {
            args["limit"] = 30;
            args["sinceHours"] = 24 * 14;
        }
        printf("%s", call_body(1, argv[2], args).c_str());
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--agents")) {
        JsonDocument s;
        std::string text, error;
        if (!parse_reply(read_file(argv[2]), 1, s, text, error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const std::vector<Agent> agents = agents_from(s);
        int working = 0, waiting = 0;
        for (const Agent &a : agents) {
            working += a.status == "running";
            waiting += a.needs_you;
        }
        const std::string text_out = describe_agents(agents, time(nullptr), 3800);
        printf("%zu agents, %d working, %d need the person; the tool's text is %zu bytes\n", agents.size(), working, waiting,
               text_out.size());
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--activity")) {
        JsonDocument s;
        std::string text, error;
        if (!parse_reply(read_file(argv[2]), 1, s, text, error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const std::string content = s["content"] | "";
        const std::string tail = activity_tail(content, 3500);
        printf("activity of %zu bytes; the tool keeps its last %zu bytes, %d lines\n", content.size(), tail.size(),
               int(std::count(tail.begin(), tail.end(), '\n')) + 1);
        return 0;
    }
    test_requests();
    test_replies();
    test_texts();
    test_matching();
    test_activity();
    test_time();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
