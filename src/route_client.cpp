// Route lookup via adsbdb.com (free, no API key): GET /v0/callsign/{callsign}.
// Returns origin/destination city names (English). Device-only.
#include "route_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>

#define ROUTE_CACHE_MAX 200   // wrap the cache before it can crowd NVS

// strip spaces -> a valid NVS key (callsigns are <= 8 chars)
static void route_key(const char *callsign, char *out, size_t on) {
    size_t j = 0;
    for (const char *p = callsign; *p && j < on - 1; ++p)
        if (*p != ' ') out[j++] = *p;
    out[j] = 0;
}

bool route_cache_get(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0]) return false;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return false;
    Preferences p;
    if (!p.begin("routes", true)) return false;
    String v = p.getString(key, "");
    p.end();
    if (v.length() == 0) return false;
    const int bar = v.indexOf('|');
    if (bar < 0) return false;
    snprintf(from, fn, "%s", v.substring(0, bar).c_str());
    snprintf(to, tn, "%s", v.substring(bar + 1).c_str());
    return true;
}

void route_cache_put(const char *callsign, const char *from, const char *to) {
    if (!callsign || !callsign[0]) return;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_CACHE_MAX) { p.clear(); n = 0; }   // wrap to bound NVS usage
    String v = String(from ? from : "") + "|" + String(to ? to : "");
    if (p.putString(key, v) > 0) p.putInt("__n", n + 1);
    p.end();
}

bool route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0] || WiFi.status() != WL_CONNECTED) return false;

    // strip spaces from the callsign
    char cs[12];
    size_t j = 0;
    for (const char *p = callsign; *p && j < sizeof(cs) - 1; ++p)
        if (*p != ' ') cs[j++] = *p;
    cs[j] = 0;
    if (j == 0) return false;

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);

    const int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument filter;
    filter["response"]["flightroute"]["origin"]["municipality"] = true;
    filter["response"]["flightroute"]["origin"]["iata_code"] = true;
    filter["response"]["flightroute"]["destination"]["municipality"] = true;
    filter["response"]["flightroute"]["destination"]["iata_code"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonObjectConst fr = doc["response"]["flightroute"].as<JsonObjectConst>();
    if (fr.isNull()) return false;   // "unknown callsign" etc.

    const char *oCity = fr["origin"]["municipality"] | "";
    const char *oIata = fr["origin"]["iata_code"] | "";
    const char *dCity = fr["destination"]["municipality"] | "";
    const char *dIata = fr["destination"]["iata_code"] | "";
    snprintf(from, fn, "%s", oCity[0] ? oCity : oIata);
    snprintf(to, tn, "%s", dCity[0] ? dCity : dIata);
    return (from[0] || to[0]);
}
