// Fetch nearby aircraft from airplanes.live (then adsb.fi, then adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): the body is bulk-read into a PSRAM buffer
// (no full-body String in internal RAM), parsed with an ArduinoJson field filter so only
// the ~12 fields we need are kept, and the number of aircraft is hard-capped
// (ADSB_MAX_AIRCRAFT). The radar then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"   // tcp_active_pcbs: lwIP has no public way to list connections

// A provider that accepts the TCP connection and then says nothing (a TLS-inspecting
// firewall, a captive portal, a lossy link) leaves something behind on every failed
// request. HTTPClient closes the socket, lwIP sends a FIN with the unacknowledged
// ClientHello still queued, and that FIN is never answered either. The socket is gone,
// but lwIP keeps the connection in FIN_WAIT_1, retransmitting with exponential backoff
// until TCP_MAXRTX gives up: about 39 minutes, holding its send buffers all along.
// A ~3-minute stall at one try per 10 s fills lwIP's whole connection pool (16), and on
// boards without lwIP buffers in PSRAM (the LCD 2.1) that is ~2 KB of internal RAM each,
// at the point where TLS needs internal RAM to recover.
//
// Nothing can answer those FINs any more, so after a failed request reset every outgoing
// HTTPS connection (remote port 443: feeds, route and photo lookups) that has been closed
// by its owner (callback_arg cleared by the socket layer) and is still waiting for its FIN
// to be acknowledged. We only close those once we are done with them, so nothing is lost.
// The web server's own connections are left alone: a page's last bytes may still be in
// flight when its socket closes.
// SO_LINGER can't do this: HTTPClient closes inside connect() when TLS fails.
static unsigned freeStalledConnections() {
    struct tcp_pcb* stale[MEMP_NUM_TCP_PCB];
    unsigned n = 0;
    LOCK_TCPIP_CORE();
    for (struct tcp_pcb* p = tcp_active_pcbs; p && n < MEMP_NUM_TCP_PCB; p = p->next) {
        const bool closing = p->state == FIN_WAIT_1 || p->state == FIN_WAIT_2 ||
                             p->state == CLOSING || p->state == LAST_ACK;
        if (closing && p->callback_arg == NULL && p->remote_port == 443) stale[n++] = p;
    }
    for (unsigned i = 0; i < n; ++i) tcp_abort(stale[i]);   // unlinks; don't walk the list here
    UNLOCK_TCPIP_CORE();
    return n;
}

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
//
// UNUSED, and kept deliberately. This is upstream's fix for the IncompleteInput bug; this
// fork fixes the same bug by bulk-reading the body into PSRAM and parsing from there (see
// fetchFrom below). Both work -- tests/adsb_json_stream_test.cpp proves this one does -- but
// parsing through a Stream costs ArduinoJson one readBytes() call PER BYTE, which measured as
// a ~10% render frame-rate drop on the device (7.79 -> 7.02 fps). Retained so the class stays
// in sync with upstream and the test keeps compiling; do not wire it back in without
// re-running that measurement.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        // timedRead() calls this in a loop with no yield until a byte arrives. If the server
        // pauses mid-body for more than 5 s, that busy loop starves IDLE0 and the task
        // watchdog restarts the radar. A 1 ms wait lets the idle task run; the stream
        // timeout still ends a stalled read.
        else delay(1);
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

// Independent providers, tried in order. Same readsb payload, different URL shapes.
// Order matters: airplanes.live stays first so an approved key is used when available
// (it parks itself in seconds if not), then adsb.fi, whose 1 req/s limit is documented
// and fixed, then adsb.lol, whose limits are dynamic and throttle hardest.
struct AdsbProvider {
    const char* host;
    const char* pathFmt;   // lat, lon, radius-in-nm
    bool        http10;    // ask in HTTP/1.0 (see fetchFrom for why, and why not adsb.lol)
};
static const AdsbProvider kProviders[ADSB_PROVIDER_COUNT] = {
    { ADSB_PRIMARY_HOST,  "/v2/point/%.4f/%.4f/%.0f",            true  },
    { ADSB_OPENDATA_HOST, "/api/v3/lat/%.4f/lon/%.4f/dist/%.0f", true  },
    { ADSB_FALLBACK_HOST, "/v2/point/%.4f/%.4f/%.0f",            false },
};

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Try each independent provider once, skipping any that is parked or still inside its
    // spacing window. Retrying a hard-failing provider every poll achieves nothing and just
    // pushes the surviving ones over their own limits.
    bool askedSomeone = false;
    for (int i = 0; i < ADSB_PROVIDER_COUNT; ++i) {
        if (cooling(i)) continue;
        askedSomeone = true;
        if (fetchFrom(i, out)) { _lastPollSkipped = false; return true; }
    }
    _lastPollSkipped = !askedSomeone;          // nothing was asked; not a failure
    return false;
}

bool AdsbClient::fetchFrom(int slot, std::vector<Aircraft>& out) {
    const char* host = kProviders[slot].host;
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char path[96];
    snprintf(path, sizeof(path), kProviders[slot].pathFmt, _lat, _lon, nm);
    char url[160];
    snprintf(url, sizeof(url), "https://%s%s", host, path);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif
    // The core's default TLS handshake timeout is 120 s. If a server accepts the TCP
    // connection but never answers the handshake, that blocks this task for two minutes
    // per provider: the radar goes stale for that long, and two in a row pass the feed
    // watchdog's 180 s mark (feed_watchdog.h), which reboots when the heap is fragmented.
    // A handshake takes well under a second when things work, so give up early and let
    // the next poll retry.
    client.setHandshakeTimeout(TLS_HANDSHAKE_S);

    _pacer.onAttempt(slot, millis());

    HTTPClient http;
    http.setReuse(false);
    // Ask in HTTP/1.0, which has no chunked transfer encoding.
    //
    // This matters because the bulk read below pulls straight off http.getStreamPtr(), and
    // that is the RAW socket: Arduino's HTTPClient only de-chunks inside
    // writeToStream()/getString(). Against a chunked server (adsb.fi is one; adsb.lol sends
    // Content-Length) the buffer would start with the hex chunk-size line, which ArduinoJson
    // parses as a perfectly good JSON number -- a 200 that silently yields no aircraft.
    // HTTP/1.0 makes the body either Content-Length- or close-delimited, both of which the
    // bulk read handles.
    //
    // Except adsb.lol: it now sits behind an Envoy proxy, which refuses HTTP/1.0 outright
    // (426 "Upgrade Required" on every request; Envoy's accept_http_10 is off by default),
    // so that fallback was never answering. It replies with Content-Length rather than
    // chunked, so HTTP/1.1 reads fine there. Should it ever start chunking, the 200 parses
    // with no "ac" key and takes the same backoff as any unusable reply.
    http.useHTTP10(kProviders[slot].http10);
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's photo lookups) for too long
    if (!http.begin(client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); return false; }
    // MUST be setUserAgent(): addHeader() silently drops User-Agent (it is on
    // HTTPClient's "handled by code" list), leaving the default "ESP32HTTPClient".
    http.setUserAgent(ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");
    const char* wanted[] = { "Retry-After" };
    http.collectHeaders(wanted, 1);

    const int code = http.GET();
    if (code > 0) _lastResponseMs = millis();   // the server answered: TLS and heap are healthy
    if (code != 200) {
        // Upstream's diagnostics: the TLS error + heap picture explain most non-200s here
        // (handshake failures show up as a negative code with an empty body).
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        // Log a bounded slice of the error body. Providers explain themselves here
        // ("contact us for access", "rate limited", ...) and throwing it away turns an
        // actionable message into a bare status code. Bounded + time-capped so a hostile
        // or hanging response can never stall the poll task.
        char body[161] = "";
        if (code > 0) {
            NetworkClient& es = http.getStream();
            size_t n = 0;
            const uint32_t t0 = millis();
            while (n < sizeof(body) - 1 && (millis() - t0) < 500) {
                if (!es.available()) {
                    if (!es.connected()) break;
                    delay(5);
                    continue;
                }
                const int c = es.read();
                if (c < 0) break;
                body[n++] = (c == '\r' || c == '\n') ? ' ' : (char)c;
            }
            body[n] = '\0';
        }
        Serial.printf("[adsb] HTTP %d (%s) tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      code, host, tlsCode, tls,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        if (body[0]) Serial.printf("[adsb]   body: %s\n", body);

        if (code == 403) {
            // Policy refusal: park it. Announce once, not on every poll.
            if (_pacer.onRefused(slot, millis()))
                Serial.printf("[adsb] %s parked for %us after HTTP 403\n",
                              host, (unsigned)(ADSB_COOLDOWN_403_MS / 1000));
        } else if (code == 429) {
            // Back off multiplicatively; the limit is dynamic, so probe for what sticks.
            const long ra = http.header("Retry-After").toInt();
            const uint32_t sp = _pacer.onRateLimited(slot, millis(), ra);
            if (sp)
                Serial.printf("[adsb] %s rate-limited; spacing requests %us apart\n",
                              host, (unsigned)(sp / 1000));
        } else if (code <= 0) {
            // No answer at all (connect refused, TLS, read timeout). Until this branch
            // existed the pacer ignored such failures, so a silent provider was re-asked
            // every poll at its full connect+TLS timeout price and the radar only updated
            // as fast as the surviving providers answered.
            // Log the gap that actually applies: while every provider is silent the pacer
            // caps it at ADSB_SILENCE_OUTAGE_MAX_MS, below the stored one.
            const uint32_t sp = _pacer.onTransportFail(slot);
            if (sp) {
                const uint32_t gap = _pacer.gapMs(slot, millis());
                if (gap < sp)
                    Serial.printf("[adsb] %s silent (HTTP %d); spacing requests %us apart "
                                  "while every provider is silent, %us once one answers\n",
                                  host, code, (unsigned)(gap / 1000), (unsigned)(sp / 1000));
                else
                    Serial.printf("[adsb] %s silent (HTTP %d); spacing requests %us apart\n",
                                  host, code, (unsigned)(gap / 1000));
            }
        }
        http.end();
        if (code <= 0) {
            const unsigned freed = freeStalledConnections();
            if (freed) Serial.printf("[adsb] reset %u stalled connection%s\n", freed, freed == 1 ? "" : "s");
        }
        return false;
    }
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
    // Parse from the fully-buffered body (read above), not from the live stream: the buffered
    // read already guarantees we have the whole document, so a slow/bursty delivery can't turn
    // into an opaque IncompleteInput mid-parse. (Upstream solves the same bug with the
    // ReliableJsonStream wrapper above, which is why that class is currently unused here.)
    DeserializationError err = deserializeJson(doc, body, got, DeserializationOption::Filter(filter));
    heap_caps_free(body);
    if (err) { Serial.printf("[adsb] parse error (%s): %s (got %u bytes)\n", host, err.c_str(), (unsigned)got); return false; }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) {
        // Was silent, which made a provider that answers 200 with an unexpected shape
        // indistinguishable from one that is never tried at all. It is also not a success:
        // pace it like a 429, or a provider that changes its payload shape gets re-asked
        // every poll forever — exactly the loop the chunked-response bug produced.
        Serial.printf("[adsb] %s: 200 but no aircraft array (expected=%d read=%u)\n",
                      host, declaredLen, (unsigned)got);
        const uint32_t sp = _pacer.onUnusable(slot, millis());
        if (sp)
            Serial.printf("[adsb] %s unusable; spacing requests %us apart\n",
                          host, (unsigned)(sp / 1000));
        return false;
    }

    // Only now is the response genuinely usable, so only now does it count as a success
    // for pacing purposes.
    _lastHost = host;                   // so the caller can say who served the data
    if (_pacer.onOk(slot, millis()))
        Serial.printf("[adsb] %s steady; spacing eased to %us\n",
                      host, (unsigned)(_pacer.spacingMs(slot) / 1000));

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
        if (_maxAltFt > 0.0f && !onGround && altFt > _maxAltFt) continue;  // low-traffic/heli spotting
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
