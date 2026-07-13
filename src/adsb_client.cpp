// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Prefer the primary host, and give it a quick second try before touching the fallback:
    // the primary is reliable in practice, while the fallback can be slow to time out from
    // some networks (turning one transient primary blip into a long no-data gap + amber HUD).
    // A short delay between attempts gives the TLS/socket teardown from the previous try
    // time to actually release its buffers -- back-to-back retries with no gap were seen to
    // starve themselves of contiguous internal RAM, causing IncompleteInput parse failures.
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;
    delay(300);
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;   // transient blip -> retry the healthy host
    delay(300);
    return fetchFrom(ADSB_FALLBACK_HOST, out);            // last resort
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif
    client.setTimeout(15000);        // the Stream-level read timeout (default 1000ms) governs
                                      // how long a single low-level read waits for more bytes —
                                      // HTTPClient::setTimeout() below does NOT reliably propagate
                                      // to it, so a normal mid-transfer WiFi stall could cut a
                                      // read short well before our intended 15s budget.

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);     // fail reasonably fast: a slow host must not block the
    http.setTimeout(15000);          // task (and the user's photo lookups) for too long — but a
                                      // busy/Class-B area's JSON can be large, so give it room
                                      // to actually finish rather than cutting the read short.
    if (!http.begin(client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) { Serial.printf("[adsb] HTTP %d (%s)\n", code, host); http.end(); return false; }
    const int declaredLen = http.getSize();   // Content-Length, or -1 if chunked/unknown
    Serial.printf("[adsb] HTTP 200 (%s), content-length=%d\n", host, declaredLen);

    // Read the WHOLE body into a PSRAM buffer first, then parse from that buffer -- rather than
    // parsing straight off the stream, where a slow/bursty delivery mid-parse previously showed
    // up as an opaque "IncompleteInput" with no way to tell how much we'd actually received.
    const size_t cap = (declaredLen > 0) ? (size_t)declaredLen : (size_t)(512 * 1024);
    char *body = (char *)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!body) {
        Serial.printf("[adsb] body buffer alloc failed (%s, %u bytes)\n", host, (unsigned)cap);
        http.end();
        return false;
    }
    // Manually pump the stream rather than trust a single readBytes() call: WiFiClientSecure
    // appears to hand over only its first internal TLS-decrypt buffer's worth of data and stop,
    // so we loop on available()/read(), only giving up after a genuine multi-second stall with
    // no new bytes at all (not just "no bytes this instant" -- that's normal between TCP packets).
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t lastData = millis();
    while (got < cap) {
        const int avail = stream->available();
        if (avail > 0) {
            const size_t want = cap - got;
            const int n = stream->read((uint8_t *)(body + got), (size_t)avail < want ? (size_t)avail : want);
            if (n > 0) { got += (size_t)n; lastData = millis(); continue; }
        }
        if (!http.connected() && stream->available() == 0) break;   // server closed, nothing left to read
        if (millis() - lastData > 15000) break;                     // true stall -> give up
        delay(5);
    }
    http.end();
    if (declaredLen > 0 && got != (size_t)declaredLen) {
        Serial.printf("[adsb] short read (%s): got %u of %u declared bytes\n",
                      host, (unsigned)got, (unsigned)declaredLen);
        heap_caps_free(body);
        return false;
    }
    body[got] = 0;

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, body, got, DeserializationOption::Filter(filter));
    heap_caps_free(body);
    if (err) { Serial.printf("[adsb] parse error (%s): %s (got %u bytes)\n", host, err.c_str(), (unsigned)got); return false; }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) { Serial.printf("[adsb] no ac/aircraft array (%s)\n", host); return false; }

    // Keep the ADSB_MAX_AIRCRAFT *nearest* aircraft (not just the first ones the feed happens to
    // list), so busy areas still show the traffic closest to you. We gate by distance BEFORE
    // parsing the strings, so the hundreds of far-away aircraft never allocate anything.
    std::vector<Aircraft> tmp;
    std::vector<float>     dist;             // parallel array: km from home for each kept aircraft
    tmp.reserve(ADSB_MAX_AIRCRAFT);
    dist.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if (a["lat"].isNull() || a["lon"].isNull()) continue;   // need a position
        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();

        // alt_baro is the string "ground" for aircraft on the ground; skip them if hide-ground is on.
        const bool  onGround = a["alt_baro"].is<const char*>();
        const float altFt    = onGround ? 0.0f : (a["alt_baro"] | 0.0f);
        if (_hideGround && onGround) continue;
        // optional filters (applied before the cap, so slots only go to matching aircraft)
        if (_minAltFt > 0.0f && (onGround || altFt < _minAltFt)) continue;
        if (_milOnly && (((a["dbFlags"] | 0u) & 0x1) == 0)) continue;

        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        // nearest-N gate: if the buffer is full and this one isn't closer than the farthest kept,
        // drop it now — before any string allocation.
        int farIdx = -1;
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        ac.hex = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat = lat; ac.lon = lon;
        ac.onGround = onGround;
        ac.altBaro  = altFt;
        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }   // replace the farthest kept
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
