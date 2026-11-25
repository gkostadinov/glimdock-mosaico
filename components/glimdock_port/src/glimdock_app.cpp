// SPDX-License-Identifier: Apache-2.0
// Glimdock as one application of the ESP-Mosaico shell: the Mosaico UI
// (glim_ui.cpp) over the desk firmware's data layer (network.cpp).
#include "glimdock_app.h"

#include <string.h>

#include "Preferences.h"
#include "glim_ui.h"
#include "pairing_policy.h"
#include "mbedtls/platform_util.h"

void glimdock_summary_mark_started();

namespace {
char platform_ssid[33];
char platform_password[65];
bool started = false;
}  // namespace

const char *homelabPlatformSsid() { return platform_ssid; }
const char *homelabPlatformPassword() { return platform_password; }

extern "C" void glimdock_app_set_platform_wifi(const char *ssid, const char *password)
{
    strlcpy(platform_ssid, ssid ? ssid : "", sizeof(platform_ssid));
    mbedtls_platform_zeroize(platform_password, sizeof(platform_password));
    strlcpy(platform_password, password ? password : "", sizeof(platform_password));
}

extern "C" void glimdock_app_start(lv_obj_t *screen)
{
    if (started) {
        return;
    }
    started = true;
    glim_ui_start(screen);
    glimdock_summary_mark_started();
}

extern "C" bool glimdock_app_started(void) { return started; }

extern "C" void glimdock_app_open(lv_obj_t *screen)
{
    glimdock_app_start(screen);
    glim_ui_set_active(true);
}

extern "C" void glimdock_app_set_active(bool value) { glim_ui_set_active(value && started); }

extern "C" void glimdock_app_note_touch(bool) {}

namespace {
String saved(Preferences &p, const char *key) { return p.isKey(key) ? p.getString(key, "") : String(); }
void wipe(String &s)
{
    if (s.length()) mbedtls_platform_zeroize(const_cast<char *>(s.c_str()), s.length());
    s = "";
}
}  // namespace

extern "C" const char *glimdock_app_store_connection(const glimdock_connection_t *c)
{
    if (!c) return "no settings";
    Preferences p;
    if (!p.begin("homelab", false)) return "storage unavailable";
    String ssid = saved(p, "ssid"), password = saved(p, "password"), endpoint = saved(p, "endpoint"), token = saved(p, "token"),
           setup = saved(p, "setup"), ca = saved(p, "ca");
    const String old_ssid = ssid;
    if (c->ssid) ssid = c->ssid;
    if (ssid.isEmpty()) ssid = platform_ssid;  // Vibe Mode's network
    if (c->password) password = c->password;
    else if (ssid != old_ssid) password = ssid == platform_ssid ? String(platform_password) : String();
    if (c->endpoint) endpoint = c->endpoint;
    if (c->token) token = c->token;
    if (c->setup_token) setup = c->setup_token;
    if (c->ca) ca = c->ca;
    ssid.trim(), endpoint.trim(), token.trim(), setup.trim();
    const glimdock::PairingSettings check{ssid.c_str(), password.c_str(), endpoint.c_str(), token.c_str(), setup.c_str(), ca.c_str()};
    const char *error = nullptr;
    if (ssid.isEmpty()) error = "no Wi-Fi network: set glimdock.ssid or Wi-Fi in Vibe Mode";
    else if (!glimdock::collectorEndpointValid(endpoint.c_str())) error = "glimdock.endpoint must be an http:// or https:// collector URL";
    else if (token.isEmpty()) error = "glimdock.token (the display token) is required";
    else if (endpoint.startsWith("https://") && ca.indexOf("-----BEGIN CERTIFICATE-----") < 0) error = "an https:// endpoint needs glimdock.ca";
    else if (!glimdock::pairingSettingsValid(check)) error = "the Glimdock settings are out of range (lengths or line breaks)";
    if (!error) {
        // As saveSettings: everything else first, the network last, so a
        // half-written set is never loaded.
        p.putString("password", password);
        p.putString("endpoint", endpoint);
        p.putString("token", token);
        p.putString("setup", setup);
        p.putString("ca", ca);
        p.putString("ssid", ssid);
        if (p.getString("ssid", "") != ssid || p.getString("token", "") != token || p.getString("endpoint", "") != endpoint)
            error = "storage write failed";
    }
    p.end();
    wipe(password), wipe(token), wipe(setup);
    return error;
}

extern "C" void glimdock_app_connection_info(glimdock_connection_info_t *out)
{
    memset(out, 0, sizeof(*out));
    Preferences p;
    if (!p.begin("homelab", true)) return;
    strlcpy(out->ssid, saved(p, "ssid").c_str(), sizeof(out->ssid));
    strlcpy(out->endpoint, saved(p, "endpoint").c_str(), sizeof(out->endpoint));
    out->password = p.isKey("password") && !p.getString("password", "").isEmpty();
    out->token = p.isKey("token") && !p.getString("token", "").isEmpty();
    out->setup_token = p.isKey("setup") && !p.getString("setup", "").isEmpty();
    out->ca = p.isKey("ca") && !p.getString("ca", "").isEmpty();
    p.end();
}
