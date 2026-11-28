// SPDX-License-Identifier: Apache-2.0
#include "swifty_paseo_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace swifty {
namespace paseo {
namespace {

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out) {
        c = char(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string basename(const std::string &path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string str(JsonVariantConst v)
{
    return v.is<const char *>() ? std::string(v.as<const char *>()) : std::string();
}

// Words that say nothing about which agent is meant.
bool filler(const std::string &w)
{
    static const char *const WORDS[] = {"the", "a", "an", "my", "agent", "agents", "chat", "session", "paseo",
                                        "to", "on", "in", "for", "of", "one", "please"
                                       };
    for (const char *f : WORDS)
        if (w == f) {
            return true;
        }
    return false;
}

std::vector<std::string> words(const std::string &text)
{
    std::vector<std::string> out;
    std::string w;
    for (char c : lower(text) + " ") {
        if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80) || c == '-' || c == '_' || c == '.') {
            w += c;
        } else if (!w.empty()) {
            if (!filler(w)) {
                out.push_back(w);
            }
            w.clear();
        }
    }
    return out;
}

struct Key {
    std::string id, short_id, name, haystack;
};

// Exact id or short id; then an exact name; then every word of want in the
// name (and the folder). A unique fit wins.
int match(const std::vector<Key> &keys, const std::string &want_raw, std::string *candidates)
{
    if (candidates) {
        candidates->clear();
    }
    std::string want = want_raw;
    while (!want.empty() && isspace(static_cast<unsigned char>(want.back()))) {
        want.pop_back();
    }
    want.erase(0, want.find_first_not_of(" \t\n"));
    if (want.empty()) {
        return -1;
    }
    const std::string w = lower(want);
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string id = lower(keys[i].id);
        if (id == w || (!keys[i].short_id.empty() && lower(keys[i].short_id) == w) ||
                (w.size() >= 6 && id.compare(0, w.size(), w) == 0)) {
            return int(i);
        }
    }
    for (size_t i = 0; i < keys.size(); ++i)
        if (lower(keys[i].name) == w) {
            return int(i);
        }
    const std::vector<std::string> need = words(want);
    if (need.empty()) {
        return -1;
    }
    std::vector<int> fits;
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string hay = lower(keys[i].name + " " + keys[i].haystack);
        bool all = true;
        for (const std::string &n : need) {
            all = all && hay.find(n) != std::string::npos;
        }
        if (all) {
            fits.push_back(int(i));
        }
    }
    if (fits.size() == 1) {
        return fits[0];
    }
    if (candidates) {
        for (size_t k = 0; k < fits.size() && k < 6; ++k) {
            const Key &key = keys[size_t(fits[k])];
            *candidates += (k ? "; " : "") + (key.name.empty() ? key.id : key.name) +
                           (key.short_id.empty() ? "" : " [" + key.short_id + "]");
        }
    }
    return -1;
}

int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

constexpr time_t CLOCK_SET = 1700000000;  // before this the board has no time yet

std::string span(time_t s)
{
    char text[32];
    if (s < 3600) {
        snprintf(text, sizeof(text), "%d min", int(s / 60 > 0 ? s / 60 : 1));
    } else if (s < 2 * 86400) {
        snprintf(text, sizeof(text), "%d h", int(s / 3600));
    } else {
        snprintf(text, sizeof(text), "%d days", int(s / 86400));
    }
    return text;
}

const char *status_words(const std::string &status)
{
    if (status == "running") {
        return "working";
    }
    if (status == "initializing") {
        return "starting";
    }
    if (status == "error") {
        return "failed";
    }
    if (status == "closed") {
        return "closed (a message wakes it)";
    }
    return status.empty() ? "unknown" : status.c_str();
}

const char *attention_words(const std::string &reason)
{
    if (reason == "permission") {
        return "waiting for your permission";
    }
    if (reason == "error") {
        return "stopped with an error";
    }
    if (reason == "finished") {
        return "finished, not read yet";
    }
    return "needs you";
}

std::string cadence_text(JsonVariantConst c)
{
    const std::string type = str(c["type"]);
    if (type == "every") {
        const int64_t ms = c["everyMs"] | int64_t(0);
        if (ms <= 0) {
            return "";
        }
        if (ms % 86400000 == 0) {
            return ms == 86400000 ? "every day" : "every " + std::to_string(ms / 86400000) + " days";
        }
        if (ms % 3600000 == 0) {
            return ms == 3600000 ? "every hour" : "every " + std::to_string(ms / 3600000) + " h";
        }
        return "every " + std::to_string(ms / 60000 > 0 ? ms / 60000 : 1) + " min";
    }
    if (type == "cron") {
        std::string out = "cron " + str(c["expression"]);
        const std::string tz = str(c["timezone"]);
        return tz.empty() ? out : out + " (" + tz + ")";
    }
    return "";
}

// Lines are added while they fit; the rest are counted.
struct Lines {
    std::string out;
    size_t max;
    int dropped = 0;
    void add(const std::string &line)
    {
        if (dropped || out.size() + line.size() + 1 > max) {
            ++dropped;
            return;
        }
        out += line + "\n";
    }
    std::string done()
    {
        if (dropped) {
            out += "(and " + std::to_string(dropped) + " more)\n";
        }
        return out;
    }
};

}  // namespace

std::string clip(const std::string &text, size_t max)
{
    if (text.size() <= max) {
        return text;
    }
    if (max < 4) {
        return text.substr(0, max);
    }
    size_t cut = max - 3;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "\xE2\x80\xA6";
}

std::string endpoint(const std::string &base)
{
    std::string url = base;
    while (!url.empty() && (url.back() == '/' || isspace(static_cast<unsigned char>(url.back())))) {
        url.pop_back();
    }
    static const char SUFFIX[] = "/mcp/agents";
    const size_t n = sizeof(SUFFIX) - 1;
    if (url.size() >= n && url.compare(url.size() - n, n, SUFFIX) == 0) {
        return url;
    }
    return url + SUFFIX;
}

std::string call_body(int id, const char *tool, JsonVariantConst arguments)
{
    JsonDocument doc;
    doc["jsonrpc"] = "2.0";
    doc["id"] = id;
    doc["method"] = "tools/call";
    doc["params"]["name"] = tool;
    if (arguments.is<JsonObjectConst>()) {
        doc["params"]["arguments"] = arguments;
    } else {
        doc["params"]["arguments"].to<JsonObject>();
    }
    std::string body;
    serializeJson(doc, body);
    return body;
}

bool parse_reply(const std::string &body, int id, JsonDocument &structured, std::string &text, std::string &error)
{
    structured.clear();
    text.clear();
    error.clear();
    // The JSON messages in the body: one, or each event's data lines joined.
    std::vector<std::string> messages;
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && body[first] == '{') {
        messages.push_back(body.substr(first));
    } else {
        std::string data;
        size_t at = 0;
        while (at <= body.size()) {
            size_t eol = body.find('\n', at);
            if (eol == std::string::npos) {
                eol = body.size();
            }
            std::string line = body.substr(at, eol - at);
            at = eol + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                if (!data.empty()) {
                    messages.push_back(data);
                }
                data.clear();
            } else if (line.compare(0, 5, "data:") == 0) {
                size_t from = 5;
                if (line.size() > 5 && line[5] == ' ') {
                    from = 6;
                }
                data += (data.empty() ? "" : "\n") + line.substr(from);
            }
            if (eol == body.size()) {
                break;
            }
        }
        if (!data.empty()) {
            messages.push_back(data);
        }
    }
    JsonDocument filter;
    filter["id"] = true;
    filter["error"]["message"] = true;
    filter["result"]["isError"] = true;
    filter["result"]["structuredContent"] = true;
    for (const std::string &m : messages) {
        JsonDocument doc;
        if (deserializeJson(doc, m, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
            continue;
        }
        const bool ours = doc["id"].isNull() || (doc["id"] | -1) == id;
        if (!ours) {
            continue;
        }
        if (!doc["error"].isNull()) {
            const std::string message = str(doc["error"]["message"]);
            error = "Paseo: " + (message.empty() ? std::string("the call failed.") : message);
            return false;
        }
        if (doc["result"].isNull()) {
            continue;
        }
        const bool failed = doc["result"]["isError"] | false;
        if (failed || doc["result"]["structuredContent"].isNull()) {
            JsonDocument content_filter;
            content_filter["result"]["content"][0]["text"] = true;
            JsonDocument full;
            if (deserializeJson(full, m, DeserializationOption::Filter(content_filter)) == DeserializationError::Ok) {
                text = str(full["result"]["content"][0]["text"]);
            }
        }
        if (failed) {
            error = "Paseo: " + (text.empty() ? std::string("the call failed.") : clip(text, 300));
            text.clear();
            return false;
        }
        structured.set(doc["result"]["structuredContent"]);
        return true;
    }
    error = "Paseo sent a reply Swifty cannot read.";
    return false;
}

std::string http_error(int status, const std::string &body)
{
    if (status == 401) {
        return "Paseo refused the password (paseo_token in Swifty's settings).";
    }
    if (status == 403 && body.find("Invalid Host") != std::string::npos) {
        return "Paseo refused the name the board uses for it: put the daemon's IP address in paseo_url, or add the "
               "name to the daemon's daemon.hostnames.";
    }
    if (status == 404) {
        return "There is no Paseo agent endpoint at paseo_url (/mcp/agents): check the address and that the daemon's "
               "MCP server is on (daemon.mcp.enabled).";
    }
    if (status >= 500) {
        return "Paseo failed (HTTP " + std::to_string(status) + ").";
    }
    return "Paseo answered HTTP " + std::to_string(status) + ".";
}

std::vector<Agent> agents_from(JsonVariantConst structured)
{
    std::vector<Agent> out;
    for (JsonVariantConst a : structured["agents"].as<JsonArrayConst>()) {
        Agent agent;
        agent.id = str(a["id"]);
        agent.short_id = str(a["shortId"]);
        if (agent.short_id.empty()) {
            agent.short_id = agent.id.substr(0, 7);
        }
        agent.title = str(a["title"]);
        agent.provider = str(a["provider"]);
        agent.status = str(a["status"]);
        agent.cwd = str(a["cwd"]);
        agent.updated = str(a["updatedAt"]);
        agent.needs_you = a["requiresAttention"] | false;
        agent.attention = str(a["attentionReason"]);
        if (!agent.id.empty()) {
            out.push_back(agent);
        }
    }
    return out;
}

std::vector<Workspace> workspaces_from(JsonVariantConst structured)
{
    std::vector<Workspace> out;
    for (JsonVariantConst w : structured["workspaces"].as<JsonArrayConst>()) {
        Workspace ws;
        ws.id = str(w["workspaceId"]);
        ws.title = str(w["title"]);
        ws.cwd = str(w["cwd"]);
        ws.kind = str(w["kind"]);
        if (!ws.id.empty()) {
            out.push_back(ws);
        }
    }
    return out;
}

std::vector<Schedule> schedules_from(JsonVariantConst structured)
{
    std::vector<Schedule> out;
    for (JsonVariantConst s : structured["schedules"].as<JsonArrayConst>()) {
        Schedule sc;
        sc.id = str(s["id"]);
        sc.name = str(s["name"]);
        sc.status = str(s["status"]);
        sc.prompt = str(s["prompt"]);
        sc.cadence = cadence_text(s["cadence"]);
        sc.next_run = str(s["nextRunAt"]);
        sc.last_run = str(s["lastRunAt"]);
        if (!sc.id.empty()) {
            out.push_back(sc);
        }
    }
    return out;
}

int match_agent(const std::vector<Agent> &agents, const std::string &want, std::string *candidates)
{
    std::vector<Key> keys;
    for (const Agent &a : agents) {
        keys.push_back({a.id, a.short_id, a.title, basename(a.cwd)});
    }
    return match(keys, want, candidates);
}

int match_workspace(const std::vector<Workspace> &workspaces, const std::string &want, std::string *candidates)
{
    std::vector<Key> keys;
    for (const Workspace &w : workspaces) {
        keys.push_back({w.id, "", w.title.empty() ? basename(w.cwd) : w.title, basename(w.cwd)});
    }
    return match(keys, want, candidates);
}

int match_schedule(const std::vector<Schedule> &schedules, const std::string &want, std::string *candidates)
{
    std::vector<Key> keys;
    for (const Schedule &s : schedules) {
        keys.push_back({s.id, "", s.name.empty() ? clip(s.prompt, 40) : s.name, s.prompt});
    }
    return match(keys, want, candidates);
}

std::string agent_name(const Agent &a)
{
    return a.title.empty() ? a.short_id : clip(a.title, 70);
}

std::string agent_state(const Agent &a, time_t now)
{
    std::string line = status_words(a.status);
    if (a.needs_you) {
        line += std::string(", ") + attention_words(a.attention);
    }
    const std::string when = ago(a.updated, now);
    if (!when.empty()) {
        line += ", active " + when;
    }
    const std::string folder = basename(a.cwd);
    if (!folder.empty()) {
        line += ", in " + folder;
    }
    if (!a.provider.empty()) {
        line += " (" + a.provider + ")";
    }
    return line;
}

std::string agent_line(const Agent &a, time_t now)
{
    return agent_name(a) + " [" + a.short_id + "]: " + agent_state(a, now);
}

std::string describe_agents(const std::vector<Agent> &agents, time_t now, size_t max_bytes)
{
    if (agents.empty()) {
        return "No recent Paseo agents.";
    }
    int working = 0, waiting = 0;
    for (const Agent &a : agents) {
        working += a.status == "running" || a.status == "initializing";
        waiting += a.needs_you;
    }
    Lines lines{std::to_string(agents.size()) + " recent Paseo agents (" + std::to_string(working) + " working, " +
                std::to_string(waiting) + " need you), newest first:\n", max_bytes};
    for (const Agent &a : agents) {
        lines.add("- " + agent_line(a, now));
    }
    return lines.done();
}

std::string describe_workspaces(const std::vector<Workspace> &workspaces, size_t max_bytes)
{
    if (workspaces.empty()) {
        return "No Paseo workspaces.";
    }
    Lines lines{std::to_string(workspaces.size()) + " Paseo workspaces:\n", max_bytes};
    for (const Workspace &w : workspaces) {
        const std::string name = w.title.empty() ? basename(w.cwd) : w.title;
        lines.add("- " + clip(name, 60) + (w.kind == "worktree" ? " (worktree)" : "") + ": " + clip(w.cwd, 90));
    }
    return lines.done();
}

std::string describe_schedules(const std::vector<Schedule> &schedules, time_t now, size_t max_bytes)
{
    if (schedules.empty()) {
        return "No Paseo schedules.";
    }
    Lines lines{std::to_string(schedules.size()) + " Paseo schedules:\n", max_bytes};
    for (const Schedule &s : schedules) {
        std::string line = "- " + (s.name.empty() ? std::string("(unnamed)") : clip(s.name, 60)) + " [" + s.id + "]: " + s.status;
        if (!s.cadence.empty()) {
            line += ", " + s.cadence;
        }
        const std::string next = until(s.next_run, now), last = ago(s.last_run, now);
        if (s.status == "active" && !next.empty()) {
            line += ", next " + next;
        }
        if (!last.empty()) {
            line += ", last ran " + last;
        }
        line += ". Task: " + clip(s.prompt, 100);
        lines.add(line);
    }
    return lines.done();
}

std::string activity_tail(const std::string &content, size_t max_bytes)
{
    // "Showing 4 of 136 activities (limited to 4)\n\n" then one entry a line.
    std::string head, rest = content;
    const size_t gap = content.find("\n\n");
    if (gap != std::string::npos && gap < 120) {
        head = content.substr(0, gap);
        rest = content.substr(gap + 2);
    }
    // Blank runs closed up.
    std::string tidy;
    int blanks = 0;
    for (char c : rest) {
        blanks = c == '\n' ? blanks + 1 : 0;
        if (blanks <= 2) {
            tidy += c;
        }
    }
    while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' ')) {
        tidy.pop_back();
    }
    const size_t budget = max_bytes > head.size() + 8 ? max_bytes - head.size() - 8 : max_bytes / 2;
    if (tidy.size() > budget) {
        size_t start = tidy.size() - budget;
        const size_t eol = tidy.find('\n', start);
        start = eol != std::string::npos && eol + 1 < tidy.size() && eol - start < budget / 2 ? eol + 1 : start;
        while (start < tidy.size() && (static_cast<unsigned char>(tidy[start]) & 0xC0) == 0x80) {
            ++start;
        }
        tidy = "\xE2\x80\xA6\n" + tidy.substr(start);
    }
    return head.empty() ? tidy : head + ":\n" + tidy;
}

time_t parse_time(const std::string &iso)
{
    int y, mo, d, h, mi, s;
    if (sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return 0;
    }
    return time_t(days_from_civil(y, unsigned(mo), unsigned(d)) * 86400 + h * 3600 + mi * 60 + s);
}

std::string ago(const std::string &iso, time_t now)
{
    const time_t t = parse_time(iso);
    if (!t || now < CLOCK_SET) {
        return "";
    }
    const time_t s = now - t;
    if (s < 60) {
        return "just now";
    }
    if (s >= 86400 && s < 2 * 86400) {
        return "yesterday";
    }
    return span(s) + " ago";
}

std::string until(const std::string &iso, time_t now)
{
    const time_t t = parse_time(iso);
    if (!t || now < CLOCK_SET) {
        return "";
    }
    return t <= now + 30 ? "now" : "in " + span(t - now);
}

}  // namespace paseo
}  // namespace swifty
