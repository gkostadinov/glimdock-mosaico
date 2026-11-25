// SPDX-License-Identifier: Apache-2.0
// Host platform for the Home preview: answers the hub API from saved demo-hub
// fixtures (FIXTURE_DIR/<endpoint>.json). No network, no credentials.
#include <fstream>
#include <sstream>

#include "home_hub.h"
#include "home_prefs.h"

uint32_t millis();
std::string home_fixture_dir;

namespace home {
uint32_t now_ms() { return millis(); }

HttpResponse http_perform(const HttpRequest &request)
{
    HttpResponse out;
    const size_t at = request.url.find("/api/v1/");
    std::string endpoint = at == std::string::npos ? "" : request.url.substr(at + 8);
    std::ifstream file(home_fixture_dir + "/" + endpoint + ".json");
    if (!file) {
        out.code = 404;
        out.body = "{\"error\":{\"message\":\"No fixture\"}}";
        return out;
    }
    std::stringstream body;
    body << file.rdbuf();
    out.code = 200;
    out.body = body.str();
    return out;
}

void prefs_load(std::string &hub, std::string &token, std::string &name, int &theme, std::string &room)
{
    hub = "http://hub.test";
    token = "preview-only";  // not a credential: the fixture transport ignores it
    name = "Mosaico";
    theme = 0;
    room.clear();
}
void prefs_save(const std::string &, const std::string &, const std::string &, int, const std::string &) {}
int prefs_clock_zone() { return 0; }
}  // namespace home
