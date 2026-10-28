// SPDX-License-Identifier: GPL-3.0-only
// Ecowitt "Customized server" upload — the GW1000/WH2650 HTTP protocol that Home
// Assistant's `ecowitt` integration (via aioecowitt) consumes. Sends decoded sensor
// readings to a user-configured server as an HTTP GET with query params.
//
// Field names are the authoritative aioecowitt keys (aioecowitt), so the
// receiver parses every family. The Ecowitt CLOUD upload is intentionally NOT implemented
// — custom-server only, matching the GW1100 "Customized" slot.
//
// ENTITY-CONTINUITY (critical): HA's ecowitt integration keys every entity as
//   unique_id = "<PASSKEY>-<fieldkey>"
// The stock GW1100 sends PASSKEY = MD5(WiFi.macAddress()) UPPERCASED and METRIC field
// names. To take over an existing station's entities (and their history) without forking
// a new set, this firmware must send the SAME PASSKEY and SAME metric keys. default_passkey()
// recomputes that MD5 from the MAC at runtime; the YAML `passkey` overrides it only if set.
#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <Arduino.h>
// ESP32-C3 build: the Arduino WiFi library is NOT linked (ESPHome drives WiFi via its own
// component). Use ESPHome's wifi global + lwIP BSD sockets + mbedTLS instead of WiFiClient.
#include "esphome/components/wifi/wifi_component.h"
#include <esp_mac.h>
#include <mbedtls/md5.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "fineoffset.h"

namespace esphome {
namespace ecowitt_rf {

struct UploadConfig {
    std::string host;
    uint16_t port = 8123;
    std::string path = "/api/webhook/";   // webhook id appended by the YAML path value
    std::string passkey;                  // empty -> use default_passkey() (MD5 of MAC)
    std::string stationtype = "GW1100A";
    std::string freq = "868M";
    float rel_offset_hpa = 0;             // REL = ABS + this (console setting step 9)
};

// Upload transport protocol for a target server.
enum class Protocol : uint8_t { ECOWITT = 0, WUNDERGROUND = 1, MQTT = 2 };

// One upload target. Ecowitt uses host/port/path + key=PASSKEY; Wunderground uses
// host/port/path + key=station ID. MQTT is a global toggle (broker from ESPHome's mqtt).
struct Server {
    Protocol proto = Protocol::ECOWITT;
    std::string host;
    uint16_t port = 80;
    std::string path;
    std::string key;
    bool enabled = false;
};

// Generic raw-socket HTTP over lwIP. use_body=true -> POST the query as a form body
// (aioecowitt); use_body=false -> GET with the query in the URL (Wunderground).
// Returns the HTTP status code, or -2 (not configured/net down), -1 (connect failed), -3 (no response).
inline int http_request(bool use_body, const std::string &host, uint16_t port,
                        const std::string &path, const std::string &query) {
    if (host.empty() || wifi::global_wifi_component == nullptr ||
        !wifi::global_wifi_component->is_connected())
        return -2;
    // Resolve the host (IP literal or DNS name, e.g. the Ecowitt cloud or Wunderground).
    // getaddrinfo() has no timeout of its own, and a stalled lookup on every 16 s upload
    // blocked the main loop; cache each name's address for an hour and re-resolve only
    // after it expires or a connect fails.
    struct DnsEntry { std::string host; uint32_t addr = 0; uint32_t at_ms = 0; };
    static DnsEntry dns_cache[4];
    uint32_t now = millis();
    DnsEntry *entry = nullptr;
    for (auto &e : dns_cache)
        if (e.addr != 0 && e.host == host && (now - e.at_ms) < 3600000UL) { entry = &e; break; }
    if (entry == nullptr) {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
            return -1;
        uint32_t addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        DnsEntry *slot = &dns_cache[0];
        for (auto &e : dns_cache) {
            if (e.host == host) { slot = &e; break; }
            if (e.at_ms < slot->at_ms) slot = &e;   // replace the oldest
        }
        slot->host = host; slot->addr = addr; slot->at_ms = now;
        entry = slot;
    }
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = entry->addr;
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    // Bound send/recv so an unreachable/slow server can't stall the loop task (the
    // original Arduino client.setTimeout(3000) did this; the lwIP rewrite must too —
    // otherwise a blackholed server freezes the fast RF RX poll). 1200 ms, down from
    // 3000: one slow target at 3 s could blow the component's loop-time budget on its
    // own. The receivers here are on the LAN.
    struct timeval tv = {1, 200000};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // The socket timeouts do not cover connect(): to a host that never answers it waits out
    // the TCP SYN retries (many seconds). Connect non-blocking and wait at most 1.2 s.
    int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int r = ::connect(sock, (struct sockaddr *) &sa, sizeof(sa));
    if (r != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval ctv = {1, 200000};
        r = -1;
        if (::select(sock + 1, nullptr, &wfds, nullptr, &ctv) == 1) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                r = 0;
        }
    }
    ::fcntl(sock, F_SETFL, flags);
    if (r != 0) {
        ::close(sock);
        entry->addr = 0;   // the address may have moved: re-resolve next time
        return -1;
    }
    std::string req = std::string(use_body ? "POST " : "GET ") + path +
                      (use_body ? std::string(" HTTP/1.0") : ("?" + query + " HTTP/1.0"));
    req += "\r\nHost: " + host + "\r\n";
    if (use_body) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(query.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (use_body) req += query;
    ::send(sock, req.c_str(), req.size(), 0);
    std::string status;
    char c;
    while (status.size() < 64 && ::recv(sock, &c, 1, 0) == 1) {
        if (c == '\n') break;
        status += c;
    }
    ::close(sock);
    int sp = status.find(' ');
    return sp < 0 ? -3 : atoi(status.substr(sp + 1, 3).c_str());
}

// ---- Background HTTP worker ---------------------------------------------------------
// Every upload used to run inline on the main loop. On the console's weak WiFi (-86 dBm)
// a single post could take seconds (logged up to 2.8 s), several ran back to back, and the
// loop regularly exceeded the 5 s task watchdog: the console rebooted ~15 times overnight
// with reset reason "task watchdog", losing RF packets and window-handle state each time.
// Posts now go to a queue served by one low-priority task; the loop never waits on the
// network. The result lands in *code_out when the request finishes (-4 = queue full,
// dropped; -7 = queued, not finished yet).
struct HttpJob {
    bool use_body;
    std::string host;
    uint16_t port;
    std::string path;
    std::string query;
    volatile int *code_out;
};

inline std::atomic<uint32_t> &http_success_count() {
    static std::atomic<uint32_t> count{0};
    return count;
}
inline void record_http_result(int code) {
    if (code >= 200 && code < 300) http_success_count().fetch_add(1,std::memory_order_relaxed);
}

inline void http_worker(void *arg) {
    QueueHandle_t q = (QueueHandle_t) arg;
    for (;;) {
        HttpJob *job = nullptr;
        if (xQueueReceive(q, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
            int code = http_request(job->use_body, job->host, job->port, job->path, job->query);
            record_http_result(code);
            if (job->code_out != nullptr)
                *job->code_out = code;
            delete job;
        }
    }
}

inline void http_request_async(bool use_body, const std::string &host, uint16_t port,
                               const std::string &path, const std::string &query,
                               volatile int *code_out) {
    static QueueHandle_t queue = nullptr;
    if (queue == nullptr) {
        queue = xQueueCreate(6, sizeof(HttpJob *));
        if (queue == nullptr ||
            xTaskCreate(http_worker, "ecowitt_http", 6144, queue, 1, nullptr) != pdPASS) {
            if (code_out != nullptr) *code_out = -4;
            return;
        }
    }
    if (host.empty()) {
        if (code_out != nullptr) *code_out = -2;
        return;
    }
    HttpJob *job = new HttpJob{use_body, host, port, path, query, code_out};
    if (xQueueSend(queue, &job, 0) != pdTRUE) {   // never block the caller
        delete job;
        if (code_out != nullptr) *code_out = -4;
        return;
    }
    if (code_out != nullptr && *code_out == 0) *code_out = -7;
}

// PASSKEY the stock GW1100/WS3900A uses: uppercase MD5 of the MAC (colon-separated,
// uppercase), e.g. MD5("AA:BB:CC:DD:EE:FF"). Recomputed from the WiFi STA MAC at runtime.
inline std::string default_passkey() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macstr[18];
    snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint8_t md5[16];
    mbedtls_md5((const uint8_t *) macstr, strlen(macstr), md5);
    char out[33];
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02X", md5[i]);
    return std::string(out);
}

inline void add_field(std::string &url, const char *name, float value) {
    if (!std::isfinite(value)) return;
    if (!url.empty()) url += '&';
    url += name; url += '=';
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    url += buf;
}
inline void add_field_prec(std::string &url, const char *name, float value, int prec) {
    if (!std::isfinite(value)) return;
    if (!url.empty()) url += '&';
    url += name; url += '=';
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, value);
    url += buf;
}
inline void add_field_i(std::string &url, const char *name, int value) {
    if (!url.empty()) url += '&';
    url += name; url += '=';
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    url += buf;
}
inline void add_field(std::string &url, const char *name, const std::string &value) {
    if (!url.empty()) url += '&';
    url += name; url += '=';
    url += value;
}

// Vapour pressure deficit, in inHg (the aioecowitt `vpd` key is VPD_INHG). Magnus formula.
inline float vpd_inhg(float temp_c, int humidity) {
    if (temp_c <= -50.0f || humidity <= 0 || humidity > 100)
        return 0.0f;
    float es = 0.6108f * expf(17.27f * temp_c / (temp_c + 237.3f));  // kPa
    float vpd = es * (1.0f - humidity / 100.0f);
    return (vpd < 0 ? 0.0f : vpd) * 0.2953f;  // kPa -> inHg
}

// Percent-encode a query value (RFC 3986 unreserved characters pass through).
inline std::string url_encode(const std::string &v) {
    static const char *const HEXD = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : v) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%'; o += HEXD[c >> 4]; o += HEXD[c & 0x0F];
        }
    }
    return o;
}

// Wunderground "PWS upload" field names (imperial, per the WU protocol). WU authenticates
// with the station ID *and* the station key, but a Server has one key field, so it holds
// "STATIONID:STATIONKEY" (split at the first ':'). A key without ':' is sent as the ID with
// an empty PASSWORD (WU will reject it; the old hardcoded "password" never worked either).
inline bool upload_station_outdoor(const Reading &r) {
    switch (r.family) {
        case Family::WH24: case Family::WH65: case Family::WS69:
        case Family::WS68: case Family::WS80: case Family::WS85: case Family::WS90: return true;
        case Family::WH25: return r.model == 0xD && !r.has(F_PRESS);
        default: return false;
    }
}
inline std::string wunderground_query(const std::string &key, const Reading &r) {
    std::string q;
    size_t colon = key.find(':');
    std::string id = (colon == std::string::npos) ? key : key.substr(0, colon);
    std::string pw = (colon == std::string::npos) ? std::string() : key.substr(colon + 1);
    add_field(q, "ID", url_encode(id));
    add_field(q, "PASSWORD", url_encode(pw));
    add_field(q, "dateutc", "now");
    add_field(q, "action", "updateraw");
    constexpr float IN = 1.0f / 25.4f;
    if (upload_station_outdoor(r) && r.has(F_TEMP)) add_field(q, "tempf", r.temp_c * 9.0f / 5.0f + 32.0f);
    if (upload_station_outdoor(r) && r.has(F_HUM)) add_field_i(q, "humidity", r.humidity);
    if (r.has(F_WDIR)) add_field_i(q, "winddir", r.wind_dir_deg);
    if (r.has(F_WIND)) add_field(q, "windspeedmph", r.wind_mps * 2.23694f);
    if (r.has(F_GUST)) add_field(q, "windgustmph", r.gust_mps * 2.23694f);
    if (is_rain_family(r.family) && r.allow_rain_upload) {
        // WU: rainin = rain over the PAST HOUR, dailyrainin = since local midnight. The
        // lifetime counter is not a WU field (it read as an hourly downpour of the total).
        add_field_prec(q, "rainin", r.last1h_rain_mm * IN, 2);
        add_field_prec(q, "dailyrainin", r.rain_day_mm * IN, 2);
        add_field_prec(q, "rainratein", r.rain_rate_mmh * IN, 2);
    }
    if (r.has(F_LIGHT)) add_field(q, "solarradiation", r.light_wm2);
    if (r.has(F_UV)) add_field(q, "UV", (float)r.uv);
    if (r.has(F_PRESS)) add_field_prec(q, "baromin", r.pressure_hpa * 0.02953f, 3);
    return q;
}

// Send one HTTP POST to the custom server. Raw-socket HTTP (no Arduino HTTPClient) to
// keep the dependency surface small. HA's aioecowitt webhook accepts POST (returns 200);
// it rejects GET with 405, so we POST the query as an x-www-form-urlencoded body.
// Returns the HTTP status code, or a negative diagnostic:
//   -2 = not configured / no network, -1 = connect failed, -3 = no response line.
inline int upload(const UploadConfig &cfg, const std::string &query) {
    // POST the query as an x-www-form-urlencoded body to the custom server.
    return http_request(true, cfg.host, cfg.port, cfg.path, query);
}

// Common station fields (auth + identity), METRIC.
inline std::string station_fields(const UploadConfig &cfg) {
    std::string q;
    std::string pk = cfg.passkey.empty() ? default_passkey() : cfg.passkey;
    add_field(q, "PASSKEY", pk);
    add_field(q, "stationtype", cfg.stationtype);
    add_field(q, "freq", cfg.freq);
    add_field(q, "model", cfg.stationtype);
    return q;
}

// Extra fields for the Ecowitt CLOUD post only. The cloud reads the stock gateway's
// imperial keys (inches); the metric *_piezomm / *rainmm keys that HA's aioecowitt takes
// are ignored there, which is why the Ecowitt app showed no rain while temperature and
// wind (already sent in both units) appeared. Kept out of reading_query() so HA does not
// grow a second, inch-denominated set of rain entities.
inline void add_cloud_rain_fields(std::string &q, const Reading &r) {
    if (!r.allow_rain_upload) return;
    constexpr float IN = 1.0f / 25.4f;
    switch (r.family) {
        case Family::WS85:
        case Family::WS90:
            add_field_prec(q, "rrain_piezo", r.rain_rate_mmh * IN, 3);
            add_field_prec(q, "erain_piezo", r.rain_event_mm * IN, 3);
            add_field_prec(q, "hrain_piezo", r.rain_hour_mm * IN, 3);
            add_field_prec(q, "drain_piezo", r.rain_day_mm * IN, 3);
            add_field_prec(q, "wrain_piezo", r.rain_week_mm * IN, 3);
            add_field_prec(q, "mrain_piezo", r.rain_month_mm * IN, 3);
            add_field_prec(q, "yrain_piezo", r.rain_year_mm * IN, 3);
            break;
        case Family::WH24:
        case Family::WH65:
        case Family::WS69:
        case Family::WH40:
        case Family::WN20:
            add_field_prec(q, "rainratein", r.rain_rate_mmh * IN, 3);
            add_field_prec(q, "eventrainin", r.rain_event_mm * IN, 3);
            add_field_prec(q, "hourlyrainin", r.rain_hour_mm * IN, 3);
            add_field_prec(q, "dailyrainin", r.rain_day_mm * IN, 3);
            add_field_prec(q, "weeklyrainin", r.rain_week_mm * IN, 3);
            add_field_prec(q, "monthlyrainin", r.rain_month_mm * IN, 3);
            add_field_prec(q, "yearlyrainin", r.rain_year_mm * IN, 3);
            add_field_prec(q, "totalrainin", r.rain_mm * IN, 3);
            break;
        default:
            break;
    }
}

// ---- Battery field encodings (aioecowitt SENSOR_MAP types) --------------------------
// Each *batt key has its own encoding, and a healthy sensor must upload the "OK" value of
// THAT encoding. The old code sent 1-for-OK everywhere, which is backwards for the binary
// keys: a healthy WH31 posted batt1=1 and HA showed it LOW for days, even after fresh
// batteries (2026-09-27).
//   BATTERY_BINARY     batt1..8, wh65batt, wh25/26batt   0 = OK, 1 = low
//   BATTERY_PERCENTAGE pm25battN, leakbattN, wh57batt,   level 0..5 (HA shows level x 20 %)
//                      co2_batt
//   BATTERY_VOLTAGE    soilbattN, tf_battN, soil_ec_battN, wh40/wn20/wh68/80/85/90batt
inline int batt_binary(const Reading &r) { return r.battery_low ? 1 : 0; }
inline int batt_level(const Reading &r) { return r.battery_low ? 1 : 5; }
// Decoded voltage when the family carries one, else a nominal single-cell figure either
// side of the ~1.2 V low threshold so HA's voltage entity still reflects the low flag.
inline float batt_volts(const Reading &r) {
    if (r.battery_v > 0.0f) return r.battery_v;
    return r.battery_low ? 1.1f : 1.5f;
}

// Build the query string for one decoded reading (METRIC field names), matching the
// aioecowitt key set so HA maps onto the existing station's entities.
inline std::string reading_query(const UploadConfig &cfg, const Reading &r) {
    std::string q = station_fields(cfg);

    switch (r.family) {
        case Family::WH25:   // WN32 outdoor is modelD; WN32P/built-in use indoor keys.
            if (r.model == 0xD && !r.has(F_PRESS)) {
                if (r.has(F_TEMP)) {
                    add_field(q, "tempc", r.temp_c);
                    add_field(q, "tempf", r.temp_c * 9.0f / 5.0f + 32.0f);
                }
                if (r.has(F_HUM)) add_field_i(q, "humidity", r.humidity);
                if (r.has(F_TEMP) && r.has(F_HUM)) add_field(q, "vpd", vpd_inhg(r.temp_c,r.humidity));
                break;
            }
            if (r.has(F_TEMP)) {
                add_field(q, "tempinc", r.temp_c);
                add_field(q, "tempinf", r.temp_c * 9.0f / 5.0f + 32.0f);  // the Ecowitt app reads the imperial field for indoor
            }
            if (r.has(F_HUM)) add_field_i(q, "humidityin", r.humidity);
            if (r.has(F_PRESS)) {
                const float rel_hpa = r.pressure_hpa + cfg.rel_offset_hpa;
                add_field(q, "baromrelhpa", rel_hpa);
                add_field(q, "baromabshpa", r.pressure_hpa);
                // aioecowitt's calc.py OVERWRITES baromrelhpa with
                // round(baromrelin * 33.86, 1). Sending baromrelin at the OEM-style "%.1f"
                // (0.1 inHg = 3.4 hPa) therefore quantised the pressure to 3.4 hPa steps and
                // froze the HA entity (e.g. 1016.5 -> 30.0 inHg -> 1015.8 for hours). Send it
                // at the exact inverse of the library's 33.86 factor and 0.001 inHg precision
                // so the round-trip is lossless and the entity tracks the metric value.
                add_field_prec(q, "baromrelin", rel_hpa / 33.86f, 3);
                add_field_prec(q, "baromabsin", r.pressure_hpa / 33.86f, 3);
            }
            if (r.allow_battery_upload) {
                if (r.model == 0xE) {
                    // RF indoor replacement has its own battery; it is not the console.
                    add_field_i(q, "wh25batt", batt_binary(r));
                } else {
                    add_field_i(q, "wh26batt", batt_binary(r));
                    add_field(q, "console_batt", r.battery_v);
                }
            }
            break;

        case Family::WH31: { // outdoor multi-channel T/H (8 ch)
            char k[16];
            if (r.has(F_TEMP)) {
                std::snprintf(k, sizeof(k), "temp%dc", r.channel);
                add_field(q, k, r.temp_c);
                std::snprintf(k, sizeof(k), "temp%df", r.channel);
                add_field(q, k, r.temp_c * 9.0f / 5.0f + 32.0f);
            }
            if (r.has(F_HUM)) {
                std::snprintf(k, sizeof(k), "humidity%d", r.channel);
                add_field_i(q, k, r.humidity);
            }
            std::snprintf(k, sizeof(k), "batt%d", r.channel);
            if (r.allow_battery_upload) add_field_i(q, k, batt_binary(r));
            break;
        }

        // WH51 / WN34 / WN38 / WH52 carry no channel on the wire; the GW1100 assigns one at
        // registration and here it is the user-assigned slot channel from resolve_sensor()
        // (0 = never assigned). Channel 0 is not a valid aioecowitt key (soilmoisture0,
        // tf_ch0, ...), and guessing channel 1 would let a neighbour's default-enabled
        // sensor fight ours for the same entity, so unassigned sensors are not uploaded.
        case Family::WH51: { // soil moisture (16 ch)
            if (r.channel == 0) break;
            char k[16];
            std::snprintf(k, sizeof(k), "soilmoisture%d", r.channel);
            add_field_i(q, k, r.moisture_pct);
            std::snprintf(k, sizeof(k), "soilbatt%d", r.channel);
            if (r.allow_battery_upload) add_field(q, k, batt_volts(r));
            break;
        }

        case Family::WH24:
        case Family::WH65:
        case Family::WS69: { // outdoor weather array (tipping-bucket rain)
            if (r.has(F_WDIR)) add_field_i(q, "winddir", r.wind_dir_deg);
            if (r.has(F_WIND)) {
                add_field(q, "windspeedkmh", r.wind_mps * 3.6f);
                add_field(q, "windspeedmph", r.wind_mps * 2.23694f);   // imperial — calc.py derives windchill/feels-like from these
            }
            if (r.has(F_GUST)) {
                add_field(q, "windgustkmh", r.gust_mps * 3.6f);
                add_field(q, "windgustmph", r.gust_mps * 2.23694f);
            }
            if (r.has(F_GUST)) add_field(q, "maxdailygust", r.daily_gust_mps * 2.23694f);
            if (r.has(F_WDIR)) add_field_i(q, "winddir_avg10m", r.wind_dir_avg_deg);
            if (r.allow_rain_upload) {
            add_field(q, "rainratemm", r.rain_rate_mmh);
            add_field(q, "eventrainmm", r.rain_event_mm);
            add_field(q, "hourlyrainmm", r.rain_hour_mm);
            add_field(q, "dailyrainmm", r.rain_day_mm);
            add_field(q, "weeklyrainmm", r.rain_week_mm);
            add_field(q, "monthlyrainmm", r.rain_month_mm);
            add_field(q, "yearlyrainmm", r.rain_year_mm);
            add_field(q, "totalrainmm", r.rain_mm);
            }
            if (r.has(F_LIGHT)) add_field(q, "solarradiation", r.light_wm2);
            if (r.has(F_UV)) add_field_i(q, "uv", r.uv);
            if (r.has(F_TEMP)) {
                add_field(q, "tempc", r.temp_c);
                add_field(q, "tempf", r.temp_c * 9.0f / 5.0f + 32.0f);  // aioecowitt metric-temp bug workaround
            }
            if (r.has(F_HUM)) add_field_i(q, "humidity", r.humidity);
            if (r.has(F_TEMP) && r.has(F_HUM)) add_field(q, "vpd", vpd_inhg(r.temp_c, r.humidity));
            // NOTE: the WS69 pressure extension is deliberately NOT uploaded — see the
            // barometer-ownership comment in the WS68/80/85/90 case below.
            if (r.allow_battery_upload) add_field_i(q, "wh65batt", batt_binary(r));
            break;
        }

        case Family::WH0290: // PM2.5/PM10 (WH41 family)
            if (r.has(F_PM25)) add_field(q, "pm25_ch1", std::isfinite(r.pm25_exact_ugm3) ? r.pm25_exact_ugm3 : r.pm25_ugm3);
            if (r.allow_battery_upload) add_field_i(q, "pm25batt1", batt_level(r));
            break;

        case Family::WH43:
            if (r.has(F_PM25)) add_field(q, "pm25_ch1", std::isfinite(r.pm25_exact_ugm3) ? r.pm25_exact_ugm3 : r.pm25_ugm3);
            if (r.allow_battery_upload) add_field_i(q, "pm25batt1", batt_level(r));
            break;

        case Family::WH45:   // CO2 + PM combo: its own *_co2 keys, not the WH41 pm25_ch1 slot
        case Family::WH46:
            if (r.has(F_PM25)) add_field(q, "pm25_co2", std::isfinite(r.pm25_exact_ugm3) ? r.pm25_exact_ugm3 : r.pm25_ugm3);
            if (r.has(F_PM1)) add_field(q, "pm1_co2", r.pm1_ugm3);
            if (r.has(F_PM10)) add_field_i(q, "pm10_co2", r.pm10_ugm3);
            if (r.has(F_TEMP)) add_field(q, "tf_co2", r.temp_c * 9.0f / 5.0f + 32.0f);  // F key; calc.py derives tf_co2c
            if (r.has(F_HUM)) add_field_i(q, "humi_co2", r.humidity);
            if (r.has(F_CO2)) add_field_i(q, "co2", r.co2_ppm);
            if (r.allow_battery_upload) add_field_i(q, "co2_batt", batt_level(r));
            break;

        case Family::WN34:   // soil temp (8 ch)
        case Family::WN38: {
            if (r.channel == 0) break;   // unassigned: see WH51
            char k[16];
            std::snprintf(k, sizeof(k), "tf_ch%d", r.channel);   // F key; calc.py derives tf_chNc
            add_field(q, k, r.soil_temp_c * 9.0f / 5.0f + 32.0f);
            std::snprintf(k, sizeof(k), "tf_batt%d", r.channel);
            if (r.allow_battery_upload) add_field(q, k, batt_volts(r));
            break;
        }

        case Family::WH40:   // rain gauge
        case Family::WN20:
            if (r.allow_rain_upload) {
            add_field(q, "rainratemm", r.rain_rate_mmh);
            add_field(q, "eventrainmm", r.rain_event_mm);
            add_field(q, "hourlyrainmm", r.rain_hour_mm);
            add_field(q, "dailyrainmm", r.rain_day_mm);
            add_field(q, "weeklyrainmm", r.rain_week_mm);
            add_field(q, "monthlyrainmm", r.rain_month_mm);
            add_field(q, "yearlyrainmm", r.rain_year_mm);
            add_field(q, "totalrainmm", r.rain_mm);
            }
            if (r.allow_battery_upload) add_field(q, (r.family == Family::WN20) ? "wn20batt" : "wh40batt", r.battery_v);  // battery V
            break;

        case Family::WH55: { // leak (4 ch)
            char k[16];
            std::snprintf(k, sizeof(k), "leak_ch%d", r.channel);
            add_field_i(q, k, r.leak ? 1 : 0);
            std::snprintf(k, sizeof(k), "leakbatt%d", r.channel);
            if (r.allow_battery_upload) add_field_i(q, k, batt_level(r));
            break;
        }

        case Family::WH57:   // lightning
            // `lightning` is the strike DISTANCE in km (aioecowitt LIGHTNING_DISTANCE_KM; calc.py
            // derives lightning_mi from it). Only sent when the sensor reports a distance.
            if (r.lightning_dist_km > 0) {
                add_field_i(q, "lightning", (int)lroundf(r.lightning_dist_km));
                add_field(q, "lightning_mi", r.lightning_dist_km * 0.621371f);
            }
            add_field_i(q, "lightning_num", (int)r.strikes);
            if (r.allow_battery_upload) add_field_i(q, "wh57batt", batt_level(r));
            break;

        case Family::WS68:
        case Family::WS80:
        case Family::WS85:
        case Family::WS90: { // ultrasonic arrays (WH90 = piezo rain)
            if (r.has(F_WDIR)) add_field_i(q, "winddir", r.wind_dir_deg);
            if (r.has(F_WIND)) {
                add_field(q, "windspeedkmh", r.wind_mps * 3.6f);
                add_field(q, "windspeedmph", r.wind_mps * 2.23694f);   // imperial — calc.py derives windchill/feels-like from these
            }
            if (r.has(F_GUST)) {
                add_field(q, "windgustkmh", r.gust_mps * 3.6f);
                add_field(q, "windgustmph", r.gust_mps * 2.23694f);
            }
            if (r.has(F_GUST)) add_field(q, "maxdailygust", r.daily_gust_mps * 2.23694f);
            if (r.has(F_WDIR)) add_field_i(q, "winddir_avg10m", r.wind_dir_avg_deg);
            if (r.has(F_LIGHT)) add_field(q, "solarradiation", r.light_wm2);
            if (r.has(F_UV)) add_field_i(q, "uv", r.uv);
            // ★ BAROMETER OWNERSHIP. The WS90 carries its own barometer, but so does the
            // console (uploaded every update() as the WH25 slot) — and both used to write
            // baromrelhpa under the SAME PASSKEY, so HA's single relative-pressure entity
            // was driven alternately by two sensors at two cadences reading a few hPa
            // apart. The console barometer is the station barometer, as on the OEM
            // firmware, so the array's pressure is intentionally NOT uploaded.
            if (r.has(F_TEMP)) {
                add_field(q, "tempc", r.temp_c);
                add_field(q, "tempf", r.temp_c * 9.0f / 5.0f + 32.0f);  // aioecowitt metric-temp bug workaround
            }
            if (r.has(F_HUM)) add_field_i(q, "humidity", r.humidity);
            if (r.has(F_TEMP) && r.has(F_HUM)) add_field(q, "vpd", vpd_inhg(r.temp_c, r.humidity));
            // piezo rain (WS85/WS90 only; WS68/WS80 have no rain gauge): running
            // hourly/daily/weekly/monthly/yearly totals + event rain + rate (all derived by
            // RainAggregator from the cumulative counter).
            if (r.allow_rain_upload && (r.family == Family::WS85 || r.family == Family::WS90)) {
            add_field(q, "erain_piezomm", r.rain_event_mm);
            add_field(q, "hrain_piezomm", r.rain_hour_mm);
            add_field(q, "drain_piezomm", r.rain_day_mm);
            add_field(q, "wrain_piezomm", r.rain_week_mm);
            add_field(q, "mrain_piezomm", r.rain_month_mm);
            add_field(q, "yrain_piezomm", r.rain_year_mm);
            add_field(q, "last24hrain_piezomm", r.last24h_rain_mm);
            add_field(q, "rrain_piezomm", r.rain_rate_mmh);
            add_field(q, "totalrainmm", r.rain_mm);   // lifetime counter — the WS90's one historic value
            add_field_i(q, "srain_piezo", r.rain_state ? 1 : 0);  // rain-start flag (RAIN_STATE)
            }
            // battery + supercap are VOLTAGES (aioecowitt BATTERY_VOLTAGE / VOLTAGE), not 0/1.
            if (r.family == Family::WS68) {
                if (r.allow_battery_upload) add_field(q, "wh68batt", r.battery_v);   // BATTERY_VOLTAGE, like wh80batt
            } else if (r.family == Family::WS80) {
                if (r.allow_battery_upload) add_field(q, "wh80batt", r.battery_v);
            } else if (r.family == Family::WS85) {
                if (r.allow_battery_upload) add_field(q, "wh85batt", r.battery_v);
                if (r.allow_battery_upload && r.has(F_CAPV)) add_field(q, "ws85cap_volt", r.cap_v);
            } else {  // WS90
                if (r.allow_battery_upload) add_field(q, "wh90batt", r.battery_v);
                if (r.allow_battery_upload && r.has(F_CAPV)) add_field(q, "ws90cap_volt", r.cap_v);
            }
            break;
        }

        case Family::WH52: { // soil moisture/temp/EC: the soil_ec_* key family
            if (r.channel == 0) break;   // unassigned: see WH51
            char k[20];
            // soil_ec_humN, not soilmoistureN: that is the WH51 slot of the same channel.
            std::snprintf(k, sizeof(k), "soil_ec_hum%d", r.channel);
            add_field_i(q, k, r.moisture_pct);
            std::snprintf(k, sizeof(k), "soil_ec_temp%d", r.channel);   // F key; calc.py derives soil_ec_tempNc
            add_field(q, k, r.soil_temp_c * 9.0f / 5.0f + 32.0f);
            std::snprintf(k, sizeof(k), "soil_ec%d", r.channel);
            add_field(q, k, r.soil_ec_uscm);
            std::snprintf(k, sizeof(k), "soil_ec_batt%d", r.channel);
            if (r.allow_battery_upload) add_field(q, k, batt_volts(r));
            break;
        }

        default:
            break;
    }
    return q;
}

// Convenience: WH25/WH32 indoor reading (kept for backward compatibility).
inline std::string wh25_query(const UploadConfig &cfg, const Reading &r) {
    return reading_query(cfg, r);
}

}  // namespace ecowitt_rf
}  // namespace esphome
