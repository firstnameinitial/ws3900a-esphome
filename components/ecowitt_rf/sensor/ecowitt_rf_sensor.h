// SPDX-License-Identifier: GPL-3.0-only
// ESPHome custom sensor platform: WS3900A RF receiver + device-config control plane.
//
// Ties together cmt2300a.h (radio init) + fineoffset.h (decode) + ecowitt_upload.h
// (custom-server HTTP upload). Polls the CMT2300A RX FIFO, decodes a Fine Offset
// packet, applies calibration, gates by family-enable, publishes the values as
// ESPHome sensors, and uploads to the custom server.
//
// CONFIG GUI: runtime-editable settings are exposed in the YAML as `template`
// text/number/select/switch entities whose `on_value`/`on_turn_on`/`on_turn_off`
// automations call the setters below. Each setter persists to ESPHome preferences
// (ESP32 NVS-backed) and applies
// immediately, so config survives power loss and is re-applied on boot. Two
// output-only text_sensors (sensor roster + device info) are owned here directly.
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/sun/sun.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/network/util.h"
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#include <cstdint>
#include <string>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <cmath>
#include <Arduino.h>
// ESP32-C3: the Arduino WiFi library is NOT linked; use ESPHome's wifi global + IDF MAC.
#include "esphome/components/wifi/wifi_component.h"
#include <esp_mac.h>
// ESP32-C3: the Arduino ESPAsyncWebServer lib is NOT linked. web_server_base.h (included
// above) pulls in the IDF web server, which exposes an API-compatible AsyncWebServer /
// AsyncWebServerRequest, so the on()/send()/getParam()/arg() calls below keep working.
#include <ctime>

#include "cmt2300a.h"
#include "fineoffset.h"
#include "rain_checkpoint.h"
#include "bounded_json.h"
#include "status_request.h"
#include "status_response.h"
#include <memory>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "ecowitt_upload.h"
#include "indoor_sensor.h"
#include "dashboard_html.h"
#include "gpl811x.h"
#include "display.h"
#include "display_history.h"
#include "display_state.h"
#include "panel.h"

namespace esphome {
namespace ecowitt_rf {

// Component log tag (ESPHome convention): the name used by the per-tag logger filter and
// the prefix on every log line from this component. Must match the logger key in
// ws3900a.yaml if it is ever overridden there.
static const char *const TAG = "ecowitt_rf";

// Preference type tags (unique within this component), stored in ESP32 NVS.
// Keep existing keys and layouts compatible with older firmware for rollback.
enum : uint32_t {
    PREF_RAIN_WINDOWS = 0xE621, // versioned primary rolling-rain checkpoint (ESP32 NVS)
    PREF_DISPLAY_HISTORY = 0xE622, // extended display records; legacy blob retained
    PREF_CFG = 0xE601,       // consolidated DeviceConfig blob
    PREF_RAIN = 0xE612,      // compact rain-aggregate state (first bucket)
    PREF_DISABLED = 0xE614,  // per-sensor disable list
    PREF_SERVERS = 0xE613,   // upload-server list blob
    PREF_PANEL = 0xE616,     // console-local UI settings (units, backlight, clock alarm)
    PREF_MAXMIN = 0xE617,    // all-time MAX/MIN records (outdoor T/H, gust, rain rate)
    PREF_RAIN_EVT = 0xE618,  // unix time of the last rain (event-rain 24 h dry reset)
    PREF_SRV_KEYS = 0xE619,  // full-length server keys (PersistedServer.key is 15 chars)
    PREF_PANEL_REL = 0xE620, // console REL-pressure offset (float hPa, setting step 9)
};

// Runtime device config. Defaults match the YAML (metric, 868 MHz, HA webhook path
// is supplied by YAML as `upload.path`; empty here means "not yet overridden").
struct DeviceConfig {
    char host[24] = {0};          // IP/hostname of the primary Ecowitt server (fits IPv4)
    char path[48] = {0};          // webhook path (HA /api/webhook/<32-hex> = 45 chars)
    char passkey[32] = {0};
    uint16_t port = 8123;
    uint16_t interval_s = 16;
    char timezone[24] = {0};      // empty -> keep the compile-time YAML timezone (IANA <=19)
    uint8_t band = 1;             // device_type: 0=433,1=868,2=915,3=920
    bool upload_enabled = true;
    bool cloud_enabled = false;   // Ecowitt cloud (OEM app) upload — OFF by default
    uint16_t family_enable = 0xFFFF;  // one bit per family group, all on by default

    float cal_temp = 0.0f;        // additive offsets / multiplicative gains
    float cal_hum = 0.0f;
    float cal_press = 0.0f;
    float cal_rain = 1.0f;
    float cal_wind = 1.0f;
    float cal_solar = 1.0f;
    float cal_uv = 1.0f;
};

// Family-enable bit indices (grouped families share one switch).
enum FamilyGroup : uint8_t {
    GRP_INDOOR = 0,       // WH25/WH32
    GRP_OUTDOOR_TH = 1,   // WH31
    GRP_SOIL_MOIST = 2,   // WH51
    GRP_SOIL_TEMP = 3,    // WN34/WN38
    GRP_SOIL_EC = 4,      // WH52
    GRP_ARRAY = 5,        // WH24/WH65/WS69
    GRP_RAIN = 6,         // WH40/WN20
    GRP_ULTRASONIC = 7,   // WS68/WS80/WS85/WS90
    GRP_AIRQ = 8,         // WH0290/WH43/WH45/WH46
    GRP_LIGHTNING = 9,    // WH57
    GRP_LEAK = 10,        // WH55/WH54
    GRP_LEAF = 11,        // WH35
};

// ESPHome's runtime set_timezone() accepts POSIX TZ strings, not IANA names. The YAML
// timezone select offers IANA names (human-readable); map them to POSIX here.
inline const char *iana_to_posix(const char *tz) {
    if (tz == nullptr || *tz == '\0') return nullptr;
    if (strcmp(tz, "Europe/London") == 0) return "GMT0BST,M3.5.0/1,M10.5.0";
    if (strcmp(tz, "Europe/Berlin") == 0 || strcmp(tz, "Europe/Paris") == 0 ||
        strcmp(tz, "Europe/Amsterdam") == 0 || strcmp(tz, "Europe/Madrid") == 0 ||
        strcmp(tz, "Europe/Rome") == 0) return "CET-1CEST,M3.5.0,M10.5.0/3";
    if (strcmp(tz, "UTC") == 0) return "UTC0";
    if (strcmp(tz, "America/New_York") == 0) return "EST5EDT,M3.2.0,M11.1.0";
    if (strcmp(tz, "America/Los_Angeles") == 0) return "PST8PDT,M3.2.0,M11.1.0";
    if (strcmp(tz, "Australia/Sydney") == 0) return "AEST-10AEDT,M10.1.0,M4.1.0/3";
    if (strcmp(tz, "Asia/Tokyo") == 0) return "JST-9";
    return "UTC0";
}

inline uint8_t family_group(Family f) {
    switch (f) {
        case Family::WH25: return GRP_INDOOR;
        case Family::WH31: return GRP_OUTDOOR_TH;
        case Family::WH51: return GRP_SOIL_MOIST;
        case Family::WN34:
        case Family::WN38: return GRP_SOIL_TEMP;
        case Family::WH52: return GRP_SOIL_EC;
        case Family::WH24:
        case Family::WH65:
        case Family::WS69: return GRP_ARRAY;
        case Family::WH40:
        case Family::WN20: return GRP_RAIN;
        case Family::WS68:
        case Family::WS80:
        case Family::WS85:
        case Family::WS90: return GRP_ULTRASONIC;
        case Family::WH0290:
        case Family::WH43:
        case Family::WH45:
        case Family::WH46: return GRP_AIRQ;
        case Family::WH57: return GRP_LIGHTNING;
        case Family::WH55:
        case Family::WH54: return GRP_LEAK;
        case Family::WH35: return GRP_LEAF;
        default: return GRP_ARRAY;  // unknown -> gated under array (never matches a decode)
    }
}

// Short canonical family tag (used as the dashboard roster `fam` + fam_en keys).
inline const char *family_tag(Family f) {
    switch (f) {
        case Family::WH25:   return "WH25";
        case Family::WH31:   return "WH31";
        case Family::WH51:   return "WH51";
        case Family::WH24:   return "WH24";
        case Family::WH65:   return "WH65";
        case Family::WH0290: return "WH41";
        case Family::WS69:   return "WS69";
        case Family::WH40:   return "WH40";
        case Family::WN20:   return "WN20";
        case Family::WS68:   return "WS68";
        case Family::WH43:   return "WH43";
        case Family::WH45:   return "WH45";
        case Family::WH46:   return "WH46";
        case Family::WH52:   return "WH52";
        case Family::WH55:   return "WH55";
        case Family::WH54:   return "WH54";
        case Family::WH57:   return "WH57";
        case Family::WN34:   return "WN34";
        case Family::WN38:   return "WN38";
        case Family::WS80:   return "WS80";
        case Family::WS85:   return "WS85";
        case Family::WS90:   return "WS90";
        case Family::WH26:   return "WH26";
        case Family::WH35:   return "WH35";
        case Family::Builtin: return "Indoor";
        default:             return "??";
    }
}

// Map a family tag back to its enable group (for the /config fam_en POST). -1 = unknown.
inline int8_t tag_to_group(const char *t) {
    if (t == nullptr) return -1;
    if (strcmp(t, "WH25") == 0) return GRP_INDOOR;
    if (strcmp(t, "WH31") == 0) return GRP_OUTDOOR_TH;
    if (strcmp(t, "WH51") == 0) return GRP_SOIL_MOIST;
    if (strcmp(t, "WN34") == 0 || strcmp(t, "WN38") == 0) return GRP_SOIL_TEMP;
    if (strcmp(t, "WH52") == 0) return GRP_SOIL_EC;
    if (strcmp(t, "WH24") == 0 || strcmp(t, "WH65") == 0 || strcmp(t, "WS69") == 0) return GRP_ARRAY;
    if (strcmp(t, "WH40") == 0 || strcmp(t, "WN20") == 0) return GRP_RAIN;
    if (strcmp(t, "WS68") == 0 || strcmp(t, "WS80") == 0 || strcmp(t, "WS85") == 0 || strcmp(t, "WS90") == 0) return GRP_ULTRASONIC;
    if (strcmp(t, "WH41") == 0 || strcmp(t, "WH43") == 0 || strcmp(t, "WH45") == 0 || strcmp(t, "WH46") == 0) return GRP_AIRQ;
    if (strcmp(t, "WH57") == 0) return GRP_LIGHTNING;
    if (strcmp(t, "WH55") == 0 || strcmp(t, "WH54") == 0) return GRP_LEAK;
    if (strcmp(t, "WH35") == 0) return GRP_LEAF;
    return -1;
}

// Map a family tag back to the Family enum (reverse of family_tag). Unknown = Unknown.
inline Family tag_to_family(const char *t) {
    if (t == nullptr) return Family::Unknown;
    if (strcmp(t, "WH25") == 0) return Family::WH25;
    if (strcmp(t, "WH31") == 0) return Family::WH31;
    if (strcmp(t, "WH51") == 0) return Family::WH51;
    if (strcmp(t, "WH24") == 0) return Family::WH24;
    if (strcmp(t, "WH65") == 0) return Family::WH65;
    if (strcmp(t, "WH41") == 0) return Family::WH0290;
    if (strcmp(t, "WS69") == 0) return Family::WS69;
    if (strcmp(t, "WH40") == 0) return Family::WH40;
    if (strcmp(t, "WN20") == 0) return Family::WN20;
    if (strcmp(t, "WS68") == 0) return Family::WS68;
    if (strcmp(t, "WH43") == 0) return Family::WH43;
    if (strcmp(t, "WH45") == 0) return Family::WH45;
    if (strcmp(t, "WH46") == 0) return Family::WH46;
    if (strcmp(t, "WH52") == 0) return Family::WH52;
    if (strcmp(t, "WH55") == 0) return Family::WH55;
    if (strcmp(t, "WH54") == 0) return Family::WH54;
    if (strcmp(t, "WH57") == 0) return Family::WH57;
    if (strcmp(t, "WN34") == 0) return Family::WN34;
    if (strcmp(t, "WN38") == 0) return Family::WN38;
    if (strcmp(t, "WS80") == 0) return Family::WS80;
    if (strcmp(t, "WS85") == 0) return Family::WS85;
    if (strcmp(t, "WS90") == 0) return Family::WS90;
    if (strcmp(t, "WH26") == 0) return Family::WH26;
    if (strcmp(t, "WH35") == 0) return Family::WH35;
    return Family::Unknown;
}

// Persistent per-sensor config (flash): one slot per learned sensor, keyed by (family, id).
struct SensorSlot {
    uint8_t family;         // Family enum value
    uint8_t id0, id1, id2;  // 24-bit sensor ID, big-endian
    uint8_t channel;        // user-assigned channel (0-15)
    uint8_t enabled;        // 1 = enabled
    char name[16];          // user-assigned friendly name (empty = auto family name)
};
static constexpr uint8_t SENSOR_MAX = 24;

// Fixed-size mirror of a Server — std::string can't be memcpy'd, so the upload-server list
// is serialized to/from this POD. Sized so 3 servers + the config blob + the 13 template
// switches' restore_mode all fit the 512 B flash preference sector. Wunderground's host/path
// are hardcoded constants, so only Ecowitt IPs, ecowitt.net/MQTT paths and WU station IDs are
// stored (an extra-Ecowitt webhook path >24 chars would truncate — rare, the primary is HA).
static constexpr uint8_t SERVER_MAX = 3;
struct PersistedServer {
    uint8_t proto;       // Protocol enum value
    uint8_t enabled;
    uint16_t port;
    char host[16];
    char path[24];
    char key[16];
};
static constexpr size_t SERVERS_BLOB = 1 + SERVER_MAX * sizeof(PersistedServer);  // 1 + 3*60 = 181 B
// Full-length server keys, stored under their own preference key so the PersistedServer
// layout above stays load-compatible. A Wunderground key is "STATIONID:STATIONKEY", which
// does not fit key[16]; PersistedServer.key keeps the (truncated) first 15 chars.
static constexpr size_t SERVER_KEY_MAX = 64;
struct ServerKeys {
    char key[SERVER_MAX][SERVER_KEY_MAX];
};

// Compact persisted rain-aggregate state (the RainAggregator's first bucket, minus the
// short-lived hour_mm / last_rain_ms / last_sample_ms which reset naturally). Enough to keep
// the monthly/yearly totals correct across a reboot — the OEM's parity gap.
struct RainPersist {
    uint32_t key = 0;
    float last_counter = -1.0f;
    float event_mm = 0, day_mm = 0, week_mm = 0, month_mm = 0, year_mm = 0;
    uint16_t mark_doy = 0;
    uint8_t mark_month = 0;
    uint16_t mark_year = 0;
};

// Persisted per-sensor disable list. Only the exceptions are stored (the roster re-learns
// every sensor on boot and defaults to enabled), so this is just the (family, id) pairs the
// user has switched off. name/channel are not persisted — name is no longer exposed in the
// UI, and channel is re-derived from the decoder each time.
static constexpr uint8_t DISABLED_MAX = 8;
struct DisabledList {
    uint8_t count = 0;
    struct { uint8_t family; uint8_t id0, id1, id2; } s[DISABLED_MAX];
};

// Minimal URL handler for the ESP32 IDF web server (which uses addHandler() instead of
// ESPAsyncWebServer's on()). Matches a URI + method and dispatches to callbacks.
class UrlHandler : public AsyncWebHandler {
 public:
    using ReqFn = std::function<void(AsyncWebServerRequest *)>;
    using BodyFn = std::function<void(AsyncWebServerRequest *, uint8_t *, size_t, size_t, size_t)>;

    UrlHandler(const char *url, http_method method, ReqFn on_req, BodyFn on_body = nullptr)
        : url_(url), method_(method), on_req_(std::move(on_req)), on_body_(std::move(on_body)) {}

    bool canHandle(AsyncWebServerRequest *request) const override {
        return request->url() == url_ && request->method() == method_;
    }
    void handleRequest(AsyncWebServerRequest *request) override {
        if (on_req_)
            on_req_(request);
    }
    void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                    size_t total) override {
        if (on_body_)
            on_body_(request, data, len, index, total);
    }
    bool isRequestHandlerTrivial() const override { return method_ != HTTP_POST; }

 protected:
    std::string url_;
    http_method method_;
    ReqFn on_req_;
    BodyFn on_body_;
};

class EcowittRFSensor : public PollingComponent {
 public:
    // -- config (set by the Python schema / YAML) --
    uint8_t device_type = 1;         // 0=433, 1=868, 2=915, 3=920 MHz (compile default)
    UploadConfig upload;
    Server extra_servers_[SERVER_MAX];
    size_t extra_count_ = 0;
    volatile int extra_code_[SERVER_MAX] = {0, 0, 0};   // written by the HTTP worker task
    i2c::I2CBus *i2c_bus_{nullptr};
    IndoorSensor indoor_;
    RainAggregator rain_;
    DisabledList disabled_;

    // The 13 config switches (1 upload + 12 family). The component owns their state in
    // cfg_ (upload_enabled / family_enable) and pushes it back on boot, so the templates
    // use restore_mode: DISABLED and no longer each burn a flash preference.
    switch_::Switch *upload_switch_{nullptr};
    switch_::Switch *family_switches_[12] = {nullptr};
    switch_::Switch *segment_walk_switch_{nullptr};

    // Wind time-window state (RAM-only; feeds maxdailygust / winddir_avg10m).
    // Rolling rain is held per sensor by RainAggregator.
    float daily_max_gust_ = 0;     // m/s, max gust since midnight
    uint16_t gust_day_ = 0;        // local day-of-year at last reset
    struct DirSample { uint16_t deg; uint32_t ms; };
    DirSample dir_buf_[72];        // ~10 min of ~8.5 s wind readings
    uint8_t dir_head_ = 0, dir_count_ = 0;
    int wind_dir_avg_ = 0;         // last 10-min average of the primary array

    // -- exposed sensors (optional; one slot per family) --
    // temp/humidity/pressure are the BUILT-IN I2C sensor's, written only by update().
    sensor::Sensor *temp = nullptr;
    sensor::Sensor *humidity = nullptr;
    sensor::Sensor *pressure = nullptr;
    // ...and these are the primary outdoor array's, written only by publish(). Keeping them
    // apart is what stops the indoor entity alternating with the outdoor value.
    sensor::Sensor *outdoor_temp = nullptr;
    sensor::Sensor *outdoor_humidity = nullptr;
    sensor::Sensor *wind_speed = nullptr;
    sensor::Sensor *wind_dir = nullptr;
    sensor::Sensor *gust = nullptr;
    sensor::Sensor *rain = nullptr;
    sensor::Sensor *rain_rate = nullptr;
    sensor::Sensor *uv = nullptr;
    sensor::Sensor *light = nullptr;
    sensor::Sensor *soil_moisture = nullptr;
    sensor::Sensor *soil_temp = nullptr;
    sensor::Sensor *soil_ec = nullptr;
    sensor::Sensor *pm25 = nullptr;
    sensor::Sensor *pm10 = nullptr;
    sensor::Sensor *co2 = nullptr;
    sensor::Sensor *leaf_wetness = nullptr;
    sensor::Sensor *lightning_strikes = nullptr;
    sensor::Sensor *lightning_distance = nullptr;
    sensor::Sensor *leak = nullptr;

    // -- output-only text sensors --
    text_sensor::TextSensor *roster = nullptr;
    text_sensor::TextSensor *device_info = nullptr;
    text_sensor::TextSensor *panel_mode = nullptr;  // current LCD mode/page (HA-visible)

    // -- time clock (for runtime timezone apply) --
    time::RealTimeClock *time_clock = nullptr;
    sun::Sun *sun_ = nullptr;

    // -- web dashboard state --
    UiState ui_;
    bool up_ok_ = false;
    uint32_t up_last_ = 0;      // unix seconds of last upload attempt
    volatile int up_code_ = 0;   // written by the HTTP worker task
    volatile int cloud_code_ = 0;  // Ecowitt cloud (OEM) last HTTP status (worker task)
    bool web_registered_ = false;
    std::string body_buf_;          // /config raw body (web-server task only)
    bool body_overflow_ = false;    // current request's body exceeded CONFIG_BODY_MAX
    static constexpr size_t CONFIG_BODY_MAX = 4096;
    // POST /config bodies are validated in the web-server task but APPLIED in loop(): the
    // handler used to mutate upload/extra_servers_ and drive radio + LCD bus writes from the
    // httpd task, concurrently with loop(). web_mutex_ guards this queue and the status-request mailbox.
    // Status serialization and config application both run in loop().
    static constexpr uint8_t CONFIG_Q_MAX = 4;
    std::string config_q_[CONFIG_Q_MAX];
    uint8_t config_q_head_ = 0, config_q_len_ = 0;
    Mutex web_mutex_;
    std::shared_ptr<StatusRequest> status_pending_;

    // The station's primary outdoor array / rain sensor: the enabled, repeat-seen sensor whose
    // readings drive daily max gust, 10-min wind average, all-time MAX/MIN, and the displayed
    // and persisted rain bucket. Latched; another sensor may take over only after the current
    // one has not been accepted for PRIMARY_STALE_MS (a boot grants the restored rain key the
    // same grace). Neighbours' and disabled arrays never qualify.
    static constexpr uint32_t PRIMARY_STALE_MS = 3600000UL;
    uint32_t primary_array_key_ = 0, primary_array_ms_ = 0;
    uint32_t primary_rain_key_ = 0, primary_rain_ms_ = 0;
    // Rain persistence is throttled: boundary rolls save at once, rain increments at most
    // once per RAIN_SAVE_MS (a wet day used to write flash on every ~9 s packet).
    static constexpr uint32_t RAIN_SAVE_MS = 60000UL;
    RainCheckpoint rain_checkpoint_;
    bool rain_restore_pending_ = false, rain_restored_ = false;
    bool rain_checkpoint_saved_ = false;
    bool rain_save_pending_ = false;
    uint32_t rain_save_ms_ = 0;

    // -- RF diagnostics (exposed on /state for live RX debugging) --
    int8_t rssi_dbm_ = 0;      // CMT2300A reg 0x70 - 128 (live RSSI, dBm)
    int8_t last_rssi_ = 0;     // RSSI latched at the most recent PKT_OK (reg 0x70 - 128)
    // Frames arriving below this are on the receiver's noise floor and are dropped before
    // their decode is acted on. A real sensor at a sane install is >= -100 dBm; the phantoms
    // the gw1100 accumulated were all -109..-114 dBm.
    static constexpr int8_t RSSI_FLOOR_DBM = -105;
    uint16_t rejected_rssi_ = 0;   // noise-floor frames dropped (diagnostic, /state)
    uint32_t gpio0_hits_ = 0;      // DEBUG: data-ready (GPIO0) assertions seen in loop()
    uint8_t reg01_ = 0;        // reg 0x01 part number (0x66 = CMT2300A, 0xB3 = CMT2219A)
    uint8_t reg02_ = 0;        // reg 0x02 part number low (0xB376 → 0x76)
    uint8_t reg03_ = 0;        // reg 0x03 PLL/freq (868 table = 0x15)
    uint8_t reg04_ = 0;        // reg 0x04 PLL/freq (868 table = 0x0D)
    uint8_t reg17_ = 0;        // reg 0x17 sync word (868 table = 0xD4)
    uint8_t reg18_ = 0;        // reg 0x18 freq (868 band table = 0x46)
    uint8_t reg6d_ = 0;        // reg 0x6D CUS_INT_FLAG (PKT_DONE etc.)
    uint8_t reg6e_ = 0;        // reg 0x6E CUS_FIFO_FLAG (RX_FIFO_NMTY etc.)
    uint8_t reg6f_ = 0;        // reg 0x6F CUS_RSSI_CODE (raw RSSI — nonzero = RF energy present)
    uint8_t reg61_ = 0;        // reg 0x61 CUS_MODE_STA (low nibble 0x05 = RX)
    uint8_t reg3f_ = 0;        // reg 0x3F INT_EN (init writes 0xFF)
    uint8_t reg3f_test_ = 0;   // write-read self-test: write 0x5A to 0x3F, read back
    uint8_t reg43_ = 0;        // reg 0x43 INT flag (bit0 = PKT_DONE — did a packet arrive?)
    uint8_t reg47_ = 0;        // reg 0x47 CMT2219A mode/status (read-back = OP_STATUS)
    uint8_t reg45_ = 0;        // reg 0x45 CMT2219A RSSI_VALUE (the REAL RSSI on 2219A)
    uint8_t rssi_min_ = 0, rssi_max_ = 0, rssi_changes_ = 0;  // RSSI fluctuation over 1.2s
    std::string last_hex_;     // hex of the most recent FIFO drain (first 24 bytes)
    std::string regdump_;      // full config register dump 0x00..0x5F (hex, for diff vs init)

    // -- setters (called by the Python schema / YAML template automations) --
    void set_device_type(uint8_t v) { device_type = v; }
    void set_upload_host(const std::string &v) { upload.host = v; }
    void set_upload_path(const std::string &v) { upload.path = v; }
    void set_upload_port(uint16_t v) { upload.port = v; }
    void set_upload_passkey(const std::string &v) { upload.passkey = v; }
    void set_upload_stationtype(const std::string &v) { upload.stationtype = v; }
    void set_upload_freq(const std::string &v) { upload.freq = v; }
    void set_time_clock(time::RealTimeClock *t) { time_clock = t; }
    void set_sun(sun::Sun *s) { sun_ = s; }
    void set_i2c_bus(i2c::I2CBus *bus) { i2c_bus_ = bus; }
    void set_upload_switch(switch_::Switch *s) { upload_switch_ = s; }
    void set_family_switch(uint8_t group, switch_::Switch *s) {
        if (group < 12) family_switches_[group] = s;
    }
    void set_segment_walk_switch(switch_::Switch *s) { segment_walk_switch_ = s; }
    void set_roster(text_sensor::TextSensor *s) { roster = s; }
    void set_device_info(text_sensor::TextSensor *s) { device_info = s; }
    void set_panel_mode(text_sensor::TextSensor *s) { panel_mode = s; }

    void set_temp(sensor::Sensor *s) { temp = s; }
    void set_humidity(sensor::Sensor *s) { humidity = s; }
    void set_pressure(sensor::Sensor *s) { pressure = s; }
    void set_outdoor_temp(sensor::Sensor *s) { outdoor_temp = s; }
    void set_outdoor_humidity(sensor::Sensor *s) { outdoor_humidity = s; }
    void set_wind_speed(sensor::Sensor *s) { wind_speed = s; }
    void set_wind_dir(sensor::Sensor *s) { wind_dir = s; }
    void set_gust(sensor::Sensor *s) { gust = s; }
    void set_rain(sensor::Sensor *s) { rain = s; }
    void set_rain_rate(sensor::Sensor *s) { rain_rate = s; }
    void set_uv(sensor::Sensor *s) { uv = s; }
    void set_light(sensor::Sensor *s) { light = s; }
    void set_soil_moisture(sensor::Sensor *s) { soil_moisture = s; }
    void set_soil_temp(sensor::Sensor *s) { soil_temp = s; }
    void set_soil_ec(sensor::Sensor *s) { soil_ec = s; }
    void set_pm25(sensor::Sensor *s) { pm25 = s; }
    void set_pm10(sensor::Sensor *s) { pm10 = s; }
    void set_co2(sensor::Sensor *s) { co2 = s; }
    void set_leaf_wetness(sensor::Sensor *s) { leaf_wetness = s; }
    void set_lightning_strikes(sensor::Sensor *s) { lightning_strikes = s; }
    void set_lightning_distance(sensor::Sensor *s) { lightning_distance = s; }
    void set_leak(sensor::Sensor *s) { leak = s; }

    // -- runtime config setters (persist + apply) --
    void set_cfg_host(const std::string &v) {
        strncpy(cfg_.host, v.c_str(), sizeof(cfg_.host) - 1);
        save_cfg();
        apply_cfg();
    }
    void set_cfg_path(const std::string &v) {
        strncpy(cfg_.path, v.c_str(), sizeof(cfg_.path) - 1);
        save_cfg();
        apply_cfg();
    }
    void set_cfg_passkey(const std::string &v) {
        strncpy(cfg_.passkey, v.c_str(), sizeof(cfg_.passkey) - 1);
        save_cfg();
        apply_cfg();
    }
    void set_cfg_port(uint16_t v) {
        cfg_.port = v;
        save_cfg();
        apply_cfg();
    }
    void set_cfg_interval(uint16_t v) {
        cfg_.interval_s = (v < 5) ? 5 : v;
        save_cfg();
        apply_cfg();
    }
    void set_cfg_timezone(const std::string &v) {
        strncpy(cfg_.timezone, v.c_str(), sizeof(cfg_.timezone) - 1);
        save_cfg();
        apply_cfg();
    }
    void set_cfg_band(uint8_t v) {   // applied on next boot (radio re-init)
        cfg_.band = (v > 3) ? 1 : v;
        save_cfg();
    }
    void set_cfg_upload_enabled(bool v) {
        cfg_.upload_enabled = v;
        if (upload_switch_) upload_switch_->publish_state(v);
        save_cfg();
        apply_cfg();
    }
    void set_cfg_cloud(bool v) {
        cfg_.cloud_enabled = v;
        save_cfg();
    }
    void set_family_enabled(uint8_t group, bool v) {
        if (group > 15)
            return;
        if (v)
            cfg_.family_enable |= (1u << group);
        else
            cfg_.family_enable &= ~(1u << group);
        if (group < 12 && family_switches_[group]) family_switches_[group]->publish_state(v);
        save_cfg();
    }
    // ---- HA-injectable keys ----------------------------------------------------------
    // Queue a synthetic key event. Injected events are handed to the SAME
    // handle_panel_event() the GPL811x key scan feeds (panel.h `event()` -> component
    // `handle_panel_event()` -> `panel_.handle()`), so a button pressed in HA is
    // indistinguishable from a finger on the glass: alarm-silence, beep, mode changes,
    // hold semantics and the settings-persist side effects all run identically.
    //
    // btn: 0=SET 1=TEMP 2=RAIN1 3=RAIN2 4=WIND 5=+ 6=- 7=LIGHT   (Btn enum, panel.h)
    // kind: 1=SHORT 2=LONG(2 s) 3=XLONG(5 s) 4=REPEAT             (EvKind enum)
    // The queue is drained one event per loop() pass so a burst of HA presses is stepped
    // through the state machine in order rather than collapsing into one redraw.
    void inject_key(uint8_t btn, uint8_t kind = 1) {
        if (btn > 7 || kind == 0 || kind > 4) {
            ESP_LOGW(TAG, "inject_key: bad btn=%u kind=%u ignored", btn, kind);
            return;
        }
        if (key_q_len_ >= KEY_Q_MAX) {
            ESP_LOGW(TAG, "inject_key: queue full, dropping btn=%u", btn);
            return;
        }
        uint8_t slot = (uint8_t)((key_q_head_ + key_q_len_) % KEY_Q_MAX);
        key_q_[slot].btn = (Btn)btn;
        key_q_[slot].kind = (EvKind)kind;
        key_q_len_++;
        ESP_LOGI(TAG, "inject_key: queued btn=%u kind=%u (depth %u)", btn, kind, key_q_len_);
    }

    std::string panel_mode_text() const {
        char buf[128];
        switch (panel_.mode) {
            case DMode::NORMAL: {
                static const char *TS[3] = {"Temp", "FeelsLike", "DewPoint"};
                char ts[16];
                if (panel_.temp_auto) {
                    std::snprintf(ts, sizeof(ts), "Auto");
                } else if (panel_.temp_sel >= Panel::TEMP_MAIN_N) {
                    const uint8_t idx = panel_.temp_sel - Panel::TEMP_MAIN_N;
                    std::snprintf(ts, sizeof(ts), "CH%u", idx < ext_chan_n_ ? ext_chans_[idx] : 0);
                } else {
                    std::snprintf(ts, sizeof(ts), "%s", TS[panel_.temp_sel]);
                }
                static const char *WS[4] = {"Speed", "Gust", "Dir", "10minAvg"};
                static const char *RS[8] = {"?", "?", "?", "Daily", "Weekly", "Monthly",
                                            "Yearly", "Total"};
                // The rain field is two rows: rate/event above, the accumulation range
                // below, so both are named.
                std::snprintf(buf, sizeof(buf), "Normal / %s / Wind %s / Rain %s+%s / %s / %s",
                              ts,
                              WS[panel_.wind_sel & 3],
                              panel_.rain_event ? "Event" : "Rate",
                              RS[panel_.rain_range & 7],
                              panel_.date_sel ? "Weekday" : "Date",
                              panel_.rot_sun ? "SunRiseSet" : "UVI+Solar");
                return std::string(buf);
            }
            case DMode::SETTING:
                std::snprintf(buf, sizeof(buf), "Setting / step %u", panel_.setting_step);
                return std::string(buf);
            case DMode::MAX:
                std::snprintf(buf, sizeof(buf), "MAX / %s / %s",
                              panel_.max_sel == 1 ? "FeelsLike" : panel_.max_sel == 2 ? "DewPoint" : "Temp",
                              panel_.mm_gust ? "Gust" : "Wind");
                return std::string(buf);
            case DMode::MIN:
                std::snprintf(buf, sizeof(buf), "MIN / %s",
                              panel_.min_sel == 1 ? "FeelsLike" : panel_.min_sel == 2 ? "DewPoint" : "Temp");
                return std::string(buf);
            case DMode::ALARM:
                std::snprintf(buf, sizeof(buf), "Alarm / %s%s",
                              panel_.alarm_sub == 0 ? "hour" : "minute",
                              panel_.alarm_edit ? " (edit)" : "");
                return std::string(buf);
            case DMode::INFO:
                return std::string("Info");
        }
        return std::string("?");
    }

    void publish_panel_mode() {
        if (panel_mode == nullptr) return;
        std::string t = walk_active_ ? std::string("Segment walk")
                       : test_fb_on_ ? std::string("Test framebuffer")
                                     : panel_mode_text();
        if (t != panel_mode_last_) {
            panel_mode_last_ = t;
            panel_mode->publish_state(t);
        }
    }

    // Segment-walk diagnostic (driver in walk_tick): light every one of the 512 segments
    // in framebuffer order so a single video maps segment id -> physical glass position.
    // ON starts the walk (2 s all-on + 2 s all-off sync, then 512 × 500 ms steps); OFF
    // aborts it. The component flips the switch back OFF when the walk finishes.
    void set_segment_walk(bool on) {
        if (!gpl_ok_)
            return;
        if (on) {
            if (walk_active_)
                return;
            walk_active_ = true;
            walk_base_ms_ = millis();
            walk_shown_ = -3;  // nothing pushed yet -> walk_tick shows the all-on marker
            ESP_LOGI(TAG, "WALK START");
        } else if (walk_active_) {
            walk_active_ = false;
            memset(fb_, 0, sizeof(fb_));
            ecowitt_rf::gpl_push(fb_);
            last_render_ms_ = 0;  // force the next loop to render the normal page
            ESP_LOGI(TAG, "WALK ABORT");
        }
    }
    // 128 hex chars -> the 64-byte framebuffer; anything shorter releases the override.
    void set_test_fb_hex(const std::string &hex) {
        if (hex.size() < 128) {
            test_fb_on_ = false;
            last_render_ms_ = 0;
            ESP_LOGI(TAG, "TESTFB off");
            return;
        }
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        uint8_t buf[64];
        for (uint8_t i = 0; i < 64; i++) {
            int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                ESP_LOGW(TAG, "TESTFB: bad hex at %u", i * 2);
                return;
            }
            buf[i] = (uint8_t)((hi << 4) | lo);
        }
        memcpy(test_fb_, buf, sizeof(test_fb_));
        test_fb_on_ = true;
        last_render_ms_ = 0;
        ESP_LOGI(TAG, "TESTFB on (hex)");
    }

    // Comma-separated segment ids ("0x2b04,0x2302,..."). An empty list releases.
    void set_test_fb_segs(const std::string &list) {
        if (list.empty()) {
            test_fb_on_ = false;
            last_render_ms_ = 0;
            ESP_LOGI(TAG, "TESTFB off");
            return;
        }
        memset(test_fb_, 0, sizeof(test_fb_));
        const char *p = list.c_str();
        uint16_t n = 0;
        while (*p) {
            char *end = nullptr;
            unsigned long v = strtoul(p, &end, 0);
            if (end == p) break;
            uint8_t byte = (uint8_t)(v >> 8), bit = (uint8_t)(v & 0xff);
            if (byte < 64 && bit) {
                test_fb_[byte] |= bit;
                n++;
            }
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        test_fb_on_ = true;
        last_render_ms_ = 0;
        ESP_LOGI(TAG, "TESTFB on: %u segments", n);
    }

    void set_cal_temp(float v) { cfg_.cal_temp = v; save_cfg(); }
    void set_cal_hum(float v) { cfg_.cal_hum = v; save_cfg(); }
    void set_cal_press(float v) { cfg_.cal_press = v; save_cfg(); }
    void set_cal_rain(float v) {
        cfg_.cal_rain = v;
        save_cfg();
        // The aggregator scales DELTAS by the gain (see RainAggregator::set_gain), so a new
        // gain adds no rain; persist the re-labelled lifetime counter with the new gain so a
        // reboot recovers the same raw baseline.
        rain_.set_gain(v);
        rain_.take_rolled();
        if (rain_.take_dirty())
            save_rain();
    }
    void set_cal_wind(float v) { cfg_.cal_wind = v; save_cfg(); }
    void set_cal_solar(float v) { cfg_.cal_solar = v; save_cfg(); }
    void set_cal_uv(float v) { cfg_.cal_uv = v; save_cfg(); }

    // ---- dashboard: map a decoded reading into a UiState (family-aware keys) ----
    // The shared weather readout follows the selected primary sources; each extra
    // sensor retains its independent values in the roster and TEMP channel page.
    void update_ui_state(const Reading &r, bool rain_primary, bool array_primary) {
        update_shared_reading(r, ui_, rain_primary, array_primary);
    }

    // Latch `slot` to `key` if it is free or its holder has not been accepted for
    // PRIMARY_STALE_MS; refresh it when `key` already holds it. True if `key` is primary.
    static bool claim_primary(uint32_t &slot, uint32_t &slot_ms, uint32_t key, uint32_t now_ms) {
        if (slot != key && slot != 0 && (now_ms - slot_ms) <= PRIMARY_STALE_MS)
            return false;
        if (slot != key)
            ESP_LOGI(TAG, "primary sensor -> %s id=0x%06X", family_name((Family)(key >> 24)),
                     (unsigned)(key & 0xFFFFFF));
        slot = key;
        slot_ms = now_ms;
        return true;
    }

    // ---- dashboard HTTP routes ----
    void register_web() {
        if (web_registered_)
            return;
        auto *base = web_server_base::global_web_server_base;
        if (base == nullptr)
            return;
        if (base->get_server() == nullptr) {
            // lwIP is only up once WiFi setup() has run; begin() does socket()/bind(),
            // which asserts on a null netconn TLS semaphore (xQueueSemaphoreTake) before
            // that. loop() keeps retrying, so defer until the network is connected.
            if (!network::is_connected())
                return;
            base->init();  // begin the shared server (captive_portal only inits it in AP mode)
        }
        AsyncWebServer *srv = base->get_server();
        if (srv == nullptr)
            return;
        srv->addHandler(new UrlHandler("/", HTTP_GET,
                                       [this](AsyncWebServerRequest *req) { handle_index(req); }));
        srv->addHandler(new UrlHandler("/health", HTTP_GET,
                                       [this](AsyncWebServerRequest *req) { handle_state(req, true); }));
        srv->addHandler(new UrlHandler("/state", HTTP_GET,
                                       [this](AsyncWebServerRequest *req) { handle_state(req); }));
        srv->addHandler(new UrlHandler(
            "/config", HTTP_POST,
            [this](AsyncWebServerRequest *req) {
                // Two ways in, because ESPHome's ESP-IDF web-server shim does not deliver
                // raw request bodies: request_post_handler() only buffers the body for
                // Content-Type: application/x-www-form-urlencoded (and multipart). An
                // application/json POST is logged "Unsupported content type", falls back
                // to the GET handler, and the body is never read at all — so handleBody()
                // never fires and body_buf_ stays empty. The supported form is therefore
                // the primary path: POST the JSON as a single urlencoded field `j`.
                //   curl -X POST .../config --data-urlencode 'j={"hm":{"monitor":true}}'
                // The body_buf_ path is kept for stacks that do deliver bodies.
                // Always CONSUME the buffered body here (every path), so a body left behind
                // by an aborted request can never be applied as part of the next one.
                std::string body;
                body.swap(body_buf_);
                bool overflow = body_overflow_;
                body_overflow_ = false;
                if (overflow) {
                    send_json_response(*req, "413 Content Too Large", "{\"ok\":false,\"err\":\"body too large\"}");
                    return;
                }
                if (body.empty()) {
                    auto *p = req->getParam("j");
                    if (p != nullptr && !p->value().empty())
                        body = p->value().c_str();
                }
                if (body.empty() || body.size() > CONFIG_BODY_MAX) {
                    send_json_response(*req, "400 Bad Request",
                              "{\"ok\":false,\"err\":\"empty body (send j=<urlencoded json>)\"}");
                    return;
                }
                handle_config(req, body);
            },
            [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
                if (index == 0) {   // first chunk of a new request: drop any stale body
                    body_buf_.clear();
                    body_overflow_ = false;
                }
                if (body_overflow_ || body_buf_.size() + len > CONFIG_BODY_MAX) {
                    body_buf_.clear();
                    body_overflow_ = true;
                    return;
                }
                body_buf_.append(reinterpret_cast<char *>(data), len);
            }));
        web_registered_ = true;
    }

    void handle_index(AsyncWebServerRequest *req) {
        req->send(200, "text/html; charset=utf-8", DASHBOARD_HTML);
    }

    // The IDF HTTP task only queues and sends. All mutable component data is read
    // by loop(), into one bounded buffer, without holding a lock across network I/O.
    void handle_state(AsyncWebServerRequest *req, bool compact = false) {
        if (ESP.getFreeHeap() < 20000) {
            send_json_response(*req, "503 Service Unavailable", "{\"err\":\"busy, retry\"}");
            return;
        }
        auto job = std::make_shared<StatusRequest>(compact);
        bool queued = false;
        {
            LockGuard guard(web_mutex_);
            if (!status_pending_) { status_pending_ = job; queued = true; }
        }
        if (!queued) {
            send_json_response(*req, "503 Service Unavailable", "{\"err\":\"busy, retry\"}");
            return;
        }
        uint32_t began = millis();
        while (job->status() == StatusRequest::PENDING && millis() - began < 500)
            vTaskDelay(1);
        job->cancel();  // no-op if loop() already published READY/FAILED
        if (job->status() != StatusRequest::READY) {
            send_json_response(*req, "503 Service Unavailable", "{\"err\":\"busy, retry\"}");
            return;
        }
        // IDF sends synchronously; job owns the buffer until this call returns.
        send_json_response(*req, "200 OK", job->body.get(), job->length);
    }

    void drain_status_request() {
        std::shared_ptr<StatusRequest> job;
        {
            LockGuard guard(web_mutex_);
            job.swap(status_pending_);
        }
        if (!job || job->status() != StatusRequest::PENDING) return;
        BoundedJson count;
        serialize_state(count, job->compact);
        const size_t capacity = count.size() + 32;
        if (!state_response_fits(count.size(), ESP.getFreeHeap(),
                                 heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))) {
            job->complete(nullptr, 0); return;
        }
        char *body = static_cast<char *>(std::malloc(capacity));
        if (!body) { job->complete(nullptr, 0); return; }
        BoundedJson out(body, capacity);
        serialize_state(out, job->compact);
        if (!out.fits()) { std::free(body); job->complete(nullptr, 0); return; }
        job->complete(body, out.size());
    }

    void serialize_state(BoundedJson &j, bool compact) {
        j.object();
        j.object("dev");
        j.field("name", "WS3900A");
        {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char macbuf[18];
            snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            j.field("mac", macbuf);
        }
        if (wifi::global_wifi_component != nullptr) {
            auto ips = wifi::global_wifi_component->wifi_sta_ip_addresses();
            std::string ipstr = ips.empty() ? std::string() : ips[0].str();
            j.field("ip", ipstr.c_str());
            j.field("ssid", wifi::global_wifi_component->get_sta().get_ssid().c_str());
            j.field("wifi_rssi", wifi::global_wifi_component->wifi_rssi());
        } else {
            j.field("ip", "");
            j.field("ssid", "");
            j.field("wifi_rssi", 0);
        }
        j.field("heap", (uint32_t)ESP.getFreeHeap());
        j.field("uptime", (uint32_t)(millis() / 1000));
        j.field("fw", ESPHOME_VERSION);
        j.field("build", "display-fixes-20261028a");
        j.field("ts", (uint32_t)::time(nullptr));

        j.field("reset_reason", (int)esp_reset_reason());
        j.end_object();
        j.object("recovery");
        j.field("pending", rain_restore_pending_);
        j.field("restored", rain_restored_);
        j.field("saved", rain_checkpoint_saved_);
        j.end_object();
        j.object("up");
        j.field("ok", up_code_ == 200); j.field("last", up_last_);
        j.field("code", (int)up_code_); j.field("cloud", (int)cloud_code_);
        j.end_object();
        j.object("r"); serialize_values(j, ui_); j.end_object();
        if (!compact) {
            j.object("diag");
            j.field("rssi", rssi_dbm_);
            j.field("reg01", reg01_);
            j.field("reg02", reg02_);
            j.field("reg03", reg03_);
            j.field("reg04", reg04_);
            j.field("reg17", reg17_);
            j.field("reg18", reg18_);
            j.field("reg6d", reg6d_);
            j.field("reg6e", reg6e_);
            j.field("reg6f", reg6f_);
            j.field("reg61", reg61_);
            j.field("regdump", regdump_.c_str());
            j.field("rej_rssi", rejected_rssi_);   // frames dropped at the noise floor
            j.field("gpio0_hits", gpio0_hits_);    // DEBUG: GPIO0 data-ready assertions
            j.field("gpl_ok", gpl_ok_);            // DEBUG: GPL811x ready-check result
            j.field("gpl_scratch", gpl_scratch_);  // 0x5a = chip answered
            j.field("keys_raw", panel_.raw);       // front-panel key bits (learn pass)
            j.field("keys_stable", panel_.stable);
            j.field("panel_mode", (uint8_t)panel_.mode);  // 0 normal,1 setting,2 max,3 min,7 alarm,10 info
            j.field("gpl_reg03", gpl_reg03_);      // cached key regs
            j.field("gpl_reg13", gpl_reg13_);
            j.field("reg3f", reg3f_);
            j.field("reg3f_test", reg3f_test_);
            j.field("reg43", reg43_);
            j.field("reg47", reg47_);
            j.field("reg45", reg45_);
            j.field("rssi_min", rssi_min_);
            j.field("rssi_max", rssi_max_);
            j.field("rssi_changes", rssi_changes_);
            j.field("last_hex", last_hex_.c_str());
            // Shadow framebuffer, 64 bytes as hex. Read-only diagnostic: lets a host
            // decode every lit segment id ((byte << 8) | bit) against the display.h
            // tables and verify each page-0 field exactly, instead of squinting at a
            // camera frame. Nothing in the render path reads this back.
            {
                static const char HEXD[] = "0123456789abcdef";
                char fbhex[129];
                for (uint8_t i = 0; i < 64; i++) {
                    fbhex[i * 2] = HEXD[(fb_[i] >> 4) & 0xF];
                    fbhex[i * 2 + 1] = HEXD[fb_[i] & 0xF];
                }
                fbhex[128] = '\0';
                j.field("fb", fbhex);
            }
            // Derived page-0 fields that have no sensor behind them, so they can be read
            // back and checked against the glass without decoding the framebuffer by hand.
            j.field("fc", fc_state_);              // forecast state 0..6, 255 = warming up
            j.field("fc_ovr", fc_override_);
            j.field("fc_fill", fc_fill_);          // hourly pressure samples collected
            j.field("moon", moon_phase());         // 0 new .. 4 full .. 7 waning crescent
            j.field("date_sel", panel_.date_sel);  // bottom-left row: 0 month, 1 weekday
            j.field("rot_sun", panel_.rot_sun);    // mid-right zone: false UVI+SOLAR, true sun
            {
                uint8_t h = 0, m = 0;
                char b[8];
                if (sun_event(true, h, m)) std::snprintf(b, sizeof(b), "%02u:%02u", h, m);
                else std::snprintf(b, sizeof(b), "--:--");
                j.field("sunrise", b);
                if (sun_event(false, h, m)) std::snprintf(b, sizeof(b), "%02u:%02u", h, m);
                else std::snprintf(b, sizeof(b), "--:--");
                j.field("sunset", b);
            }

            j.end_object();
            serialize_roster(j);
            serialize_config(j);
        }
        j.end_object();
    }

    static void serialize_values(BoundedJson &j, const UiState &u) {
        static const char *const KEYS[UiState::N] = {
            "t_in", "h_in", "p", "t_out", "h_out", "leaf_wet", "wind_dir", "wind_spd",
            "wind_gust", "rain", "rain_rate", "uv", "solar", "soil_moist", "soil_t",
            "soil_ec", "pm25", "pm10", "co2", "light_cnt", "light_dist", "leak",
            "batt_v", "cap_v", "max_gust", "wind_avg", "rain_24h", "rain_state"};
        for (uint8_t i = 0; i < UiState::N; i++)
            if (u.has(i) && std::isfinite(u.v[i]))
                j.field(KEYS[i], u.v[i]);
    }

    void serialize_roster(BoundedJson &j) {
        j.array("roster");
        uint32_t now_unix = (uint32_t)::time(nullptr);
        for (size_t i = 0; i < roster_count_; i++) {
            j.object();
            j.field("fam", family_tag(roster_[i].family));
            char id[8];
            std::snprintf(id, sizeof(id), "%06X", roster_[i].id & 0xFFFFFF);
            j.field("id", id);
            int16_t idx = find_sensor((uint8_t)roster_[i].family, roster_[i].id);
            j.field("ch", (idx >= 0) ? sensor_slots_[idx].channel : roster_[i].channel);
            j.field("en", (idx >= 0) ? (sensor_slots_[idx].enabled != 0) : true);
            j.field("name", (idx >= 0) ? sensor_slots_[idx].name : "");
            j.field("rssi", roster_[i].rssi);
            uint32_t age_s = (millis() - roster_[i].last_ms) / 1000;
            j.field("last", (now_unix >= age_s) ? (now_unix - age_s) : 0);
            j.field("batt", roster_[i].battery_low);
            j.object("v"); serialize_values(j, roster_[i].v); j.end_object();
            j.end_object();
        }
        j.end_array();
    }

    // "****" + the last 4 characters (just "****" for a key of 4 or fewer); "" stays "".
    static std::string mask_secret(const std::string &s) {
        if (s.empty()) return std::string();
        if (s.size() <= 4) return std::string("****");
        return std::string("****") + s.substr(s.size() - 4);
    }
    // A submitted secret that must NOT overwrite the stored one: empty, or exactly the mask
    // /state handed out for it.
    static bool keeps_secret(const char *submitted, const std::string &stored) {
        return submitted == nullptr || *submitted == '\0' || mask_secret(stored) == submitted;
    }

    void serialize_config(BoundedJson &j) {
        j.object("cfg");
        j.object("up");
        j.field("en", cfg_.upload_enabled);
        j.field("cloud", cfg_.cloud_enabled);
        j.field("host", cfg_.host);
        j.field("path", cfg_.path);
        j.field("port", cfg_.port);
        // /state is unauthenticated: never echo secrets. The dashboard posts the mask back
        // unchanged, which apply_config() treats as "keep the stored value".
        j.field("key", mask_secret(cfg_.passkey).c_str());
        j.field("int", cfg_.interval_s);

        j.end_object();
        j.array("servers");
        for (size_t i = 0; i < extra_count_; ++i) {
            j.object();
            j.field("proto", (extra_servers_[i].proto == Protocol::WUNDERGROUND) ? "wunderground"
                       : (extra_servers_[i].proto == Protocol::MQTT) ? "mqtt" : "ecowitt");
            j.field("host", extra_servers_[i].host.c_str());
            j.field("port", extra_servers_[i].port);
            j.field("path", extra_servers_[i].path.c_str());
            j.field("key", mask_secret(extra_servers_[i].key).c_str());
            j.field("en", extra_servers_[i].enabled);
            j.field("code", (int) extra_code_[i]);
            j.end_object();
        }
        j.end_array();
        // Report the zone actually in effect: the compile-time default is Europe/Berlin
        // (ws3900a.yaml). Reporting London made any dashboard save switch the clock to UK time.
        j.field("tz", cfg_.timezone[0] ? cfg_.timezone : "Europe/Berlin");
        j.field("rf", (cfg_.band == 0 ? 433 : cfg_.band == 1 ? 868 : cfg_.band == 2 ? 915 : 920));

        j.object("cal");
        j.field("t_off", cfg_.cal_temp);
        j.field("h_off", cfg_.cal_hum);
        j.field("p_off", cfg_.cal_press);
        j.field("rain_g", cfg_.cal_rain);
        j.field("wind_g", cfg_.cal_wind);
        j.field("solar_g", cfg_.cal_solar);
        j.field("uv_g", cfg_.cal_uv);

        j.end_object();
        j.object("fam_en");
        static const char *const TAGS[] = {
            "WH25", "WH31", "WH51", "WN34", "WN38", "WH52", "WH24", "WH65", "WS69", "WH40",
            "WN20", "WS68", "WS80", "WS85", "WS90", "WH41", "WH43", "WH45", "WH46", "WH57",
            "WH55", "WH54", "WH35"};
        for (const char *t : TAGS) {
            int8_t g = tag_to_group(t);
            if (g >= 0)
                j.field(t, (cfg_.family_enable & (1u << g)) != 0);
        }

        j.end_object();
        j.array("sensors");
        for (uint8_t i = 0; i < sensor_count_; i++) {
            j.object();
            j.field("fam", family_tag((Family)sensor_slots_[i].family));
            uint32_t sid = ((uint32_t)sensor_slots_[i].id0 << 16) | ((uint32_t)sensor_slots_[i].id1 << 8) | sensor_slots_[i].id2;
            char id[8];
            std::snprintf(id, sizeof(id), "%06X", sid);
            j.field("id", id);
            j.field("ch", sensor_slots_[i].channel);
            j.field("en", sensor_slots_[i].enabled != 0);
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }

    // Web-server task: validate JSON and queue it for loop(). Nothing here touches
    // the radio, the LCD bus,
    // upload/extra_servers_ or flash.
    void handle_config(AsyncWebServerRequest *req, const std::string &body) {
        bool ok = json::parse_json(body, [](JsonObject) -> bool {
            return true;
        });
        if (ok) {
            {
                LockGuard guard(web_mutex_);
                if (config_q_len_ >= CONFIG_Q_MAX) {
                    ok = false;
                } else {
                    config_q_[(config_q_head_ + config_q_len_) % CONFIG_Q_MAX] = body;
                    config_q_len_++;
                }
            } // release the queue lock before a potentially slow network send
            if (!ok) {
                send_json_response(*req, "503 Service Unavailable", "{\"ok\":false,\"err\":\"busy, retry\"}");
                return;
            }
        }
        std::string resp = json::build_json([ok](JsonObject o) { o["ok"] = ok; });
        send_json_response(*req, ok ? "200 OK" : "400 Bad Request", resp.c_str());
    }

    // loop(): apply at most one queued /config body per pass.
    void drain_config_queue() {
        std::string body;
        {
            LockGuard guard(web_mutex_);
            if (config_q_len_ == 0)
                return;
            body.swap(config_q_[config_q_head_]);
            config_q_head_ = (uint8_t)((config_q_head_ + 1) % CONFIG_Q_MAX);
            config_q_len_--;
        }
        apply_config(body);
    }

    // Main-loop context only (drain_config_queue). Same JSON schema as always.
    void apply_config(const std::string &body) {
        json::parse_json(body, [this](JsonObject root) -> bool {
            if (JsonObject up = root["up"].as<JsonObject>()) {
                if (!up["en"].isNull()) set_cfg_upload_enabled(up["en"].as<bool>());
                if (!up["cloud"].isNull()) set_cfg_cloud(up["cloud"].as<bool>());
                if (!up["host"].isNull()) set_cfg_host(up["host"].as<const char *>());
                if (!up["path"].isNull()) set_cfg_path(up["path"].as<const char *>());
                // Empty or the /state mask = keep the stored passkey.
                if (!up["key"].isNull() && !keeps_secret(up["key"].as<const char *>(), cfg_.passkey))
                    set_cfg_passkey(up["key"].as<const char *>());
                if (!up["port"].isNull()) set_cfg_port((uint16_t)up["port"].as<int>());
                if (!up["int"].isNull()) set_cfg_interval((uint16_t)up["int"].as<int>());
            }
            if (!root["tz"].isNull()) set_cfg_timezone(root["tz"].as<const char *>());
            if (!root["rf"].isNull()) {
                int rf = root["rf"].as<int>();
                set_cfg_band((uint8_t)(rf == 433 ? 0 : rf == 868 ? 1 : rf == 915 ? 2 : 3));
            }
            if (JsonObject cal = root["cal"].as<JsonObject>()) {
                if (!cal["t_off"].isNull()) set_cal_temp(cal["t_off"].as<float>());
                if (!cal["h_off"].isNull()) set_cal_hum(cal["h_off"].as<float>());
                if (!cal["p_off"].isNull()) set_cal_press(cal["p_off"].as<float>());
                if (!cal["rain_g"].isNull()) set_cal_rain(cal["rain_g"].as<float>());
                if (!cal["wind_g"].isNull()) set_cal_wind(cal["wind_g"].as<float>());
                if (!cal["solar_g"].isNull()) set_cal_solar(cal["solar_g"].as<float>());
                if (!cal["uv_g"].isNull()) set_cal_uv(cal["uv_g"].as<float>());
            }
            if (JsonObject fe = root["fam_en"].as<JsonObject>()) {
                for (auto kv : fe) {
                    int8_t g = tag_to_group(kv.key().c_str());
                    if (g >= 0)
                        set_family_enabled((uint8_t)g, kv.value().as<bool>());
                }
            }
            if (JsonArray sa = root["sensors"].as<JsonArray>()) {
                for (JsonVariant v : sa) {
                    JsonObject s = v.as<JsonObject>();
                    Family f = tag_to_family(s["fam"].as<const char *>());
                    if (f == Family::Unknown)
                        continue;
                    const char *idstr = s["id"].as<const char *>();
                    if (idstr == nullptr)
                        continue;
                    uint32_t id = (uint32_t)strtoul(idstr, nullptr, 16);
                    uint8_t ch = s["ch"].isNull() ? 0 : s["ch"].as<uint8_t>();
                    bool en = s["en"].isNull() ? true : s["en"].as<bool>();
                    // Enable-only toggle (no "ch" sent) must preserve the learned channel.
                    if (s["ch"].isNull()) {
                        int16_t ex = find_sensor((uint8_t)f, id);
                        ch = (ex >= 0) ? sensor_slots_[ex].channel : 0;
                    }
                    set_sensor((uint8_t)f, id, ch, en);
                    const char *nm = s["name"].as<const char *>();
                    if (nm)
                        set_sensor_name((uint8_t)f, id, nm);
                }
            }
            if (JsonArray srv = root["servers"].as<JsonArray>()) {
                Server next[SERVER_MAX];
                size_t n = 0;
                for (JsonVariant v : srv) {
                    if (n >= SERVER_MAX)
                        break;
                    JsonObject s = v.as<JsonObject>();
                    Server &d = next[n];
                    const char *p = s["proto"].as<const char *>();
                    d.proto = (p && strcmp(p, "wunderground") == 0) ? Protocol::WUNDERGROUND
                            : (p && strcmp(p, "mqtt") == 0) ? Protocol::MQTT : Protocol::ECOWITT;
                    d.host = s["host"].as<const char *>() ? s["host"].as<const char *>() : "";
                    d.port = s["port"].isNull() ? 80 : (uint16_t)s["port"].as<int>();
                    d.path = s["path"].as<const char *>() ? s["path"].as<const char *>() : "";
                    const char *k = s["key"].as<const char *>();
                    d.key = k ? k : "";
                    // Masked (from /state) or empty key = keep the stored one. Match by the
                    // exact mask on a server of the same protocol + host, so a reordered or
                    // deleted row cannot inherit another server's key; an empty key keeps
                    // the key of the same row if it is still the same server.
                    if (k == nullptr || *k == '\0') {
                        if (n < extra_count_ && extra_servers_[n].proto == d.proto &&
                            extra_servers_[n].host == d.host)
                            d.key = extra_servers_[n].key;
                    } else {
                        for (size_t o = 0; o < extra_count_; o++) {
                            if (extra_servers_[o].proto == d.proto && extra_servers_[o].host == d.host &&
                                !extra_servers_[o].key.empty() && mask_secret(extra_servers_[o].key) == k) {
                                d.key = extra_servers_[o].key;
                                break;
                            }
                        }
                    }
                    d.enabled = s["en"].isNull() ? false : s["en"].as<bool>();
                    n++;
                }
                {
                    LockGuard guard(web_mutex_);
                    for (size_t i = 0; i < SERVER_MAX; i++)
                        extra_servers_[i] = (i < n) ? next[i] : Server{};
                    extra_count_ = n;
                }
                save_servers();
            }
            // Panel/diagnostic control. The ESPHome native API is the normal control
            // surface for this (the template buttons + the Segment Walk switch), but that
            // needs the device adopted in HA; this gives the same three things over the
            // device's own web server so a bench session never depends on HA:
            //   {"panel":{"key":0..7,"kind":1|2}}  inject a front-panel key press
            //   {"panel":{"walk":true|false}}      start/abort the segment-walk diagnostic
            //   {"panel":{"fc":0..6|255}}          pin the forecast icon state (255 = auto)
            //   {"panel":{"moon":0..7|255}}        pin the moon phase (255 = auto)
            // The two overrides exist because the forecast needs hours of pressure history
            // and the moon needs weeks of calendar before every icon combination can be
            // seen once; they are not persisted and reset to auto on reboot.
            if (JsonObject pn = root["panel"].as<JsonObject>()) {
                if (!pn["key"].isNull())
                    inject_key((uint8_t)pn["key"].as<int>(),
                               pn["kind"].isNull() ? 1 : (uint8_t)pn["kind"].as<int>());
                if (!pn["walk"].isNull())
                    set_segment_walk(pn["walk"].as<bool>());
                if (!pn["fc"].isNull()) {
                    int v = pn["fc"].as<int>();
                    fc_override_ = (v >= 0 && v <= 6) ? (uint8_t)v : FC_NONE;
                    panel_.fx_redraw = true;
                }
                if (!pn["moon"].isNull()) {
                    int v = pn["moon"].as<int>();
                    moon_override_ = (v >= 0 && v <= 7) ? (uint8_t)v : 0xff;
                    panel_.fx_redraw = true;
                }
                if (!pn["testfb"].isNull()) {
                    const char *h = pn["testfb"].as<const char *>();
                    set_test_fb_hex(h ? h : "");
                }
                if (!pn["testseg"].isNull()) {
                    const char *l = pn["testseg"].as<const char *>();
                    set_test_fb_segs(l ? l : "");
                }
            }
            return true;
        });
    }

    // -- GPL811x display + keys (WS3900A front panel) --
    bool gpl_ok_{false};
    uint8_t gpl_scratch_{0};
    uint8_t gpl_reg03_{0};
    uint8_t gpl_reg13_{0};
    uint8_t fb_[64] = {0};
    Panel panel_;
    PanelSettings pset_;         // console-local units/backlight/alarm (PREF_PANEL)
    float indoor_temp_ = NAN;
    uint32_t last_render_ms_ = 0;

    uint16_t wdir_hist_[5] = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    uint8_t wdir_hist_i_ = 0;
    uint32_t wdir_hist_ms_ = 0;
    bool wdir_hist_seeded_ = false;

    uint16_t wdir_hist_mask() {
        if (ui_.has(UiState::WDIR)) {
            uint32_t now = millis();
            if (!wdir_hist_seeded_ || now - wdir_hist_ms_ >= 120000) {
                wdir_hist_seeded_ = true;
                wdir_hist_ms_ = now;
                int d = (int)lroundf(ui_.v[UiState::WDIR]);
                wdir_hist_[wdir_hist_i_] = (d < 0 || d > 719) ? 0xffff : (uint16_t)d;
                wdir_hist_i_ = (uint8_t)((wdir_hist_i_ + 1) % 5);
            }
        }
        uint16_t mask = 0;
        for (uint8_t i = 0; i < 5; i++)
            if (wdir_hist_[i] != 0xffff)
                mask |= (uint16_t)(1u << rose_sector(wdir_hist_[i]));
        return mask;
    }

    // -- segment-walk diagnostic state --
    // Timeline (ms since walk_base_ms_): 0..2 s all-on, 2..4 s all-off (video sync), then
    // step i = (elapsed-4000)/500 lights segment id (byte i>>3, bit 1<<(i&7)) for 500 ms.
    // Fixed cadence derived from the base timestamp, so a video frame counter maps
    // arithmetically to the step index.
    bool walk_active_ = false;
    uint32_t walk_base_ms_ = 0;
    int walk_shown_ = -3;   // -2 all-on, -1 all-off, 0..511 = the step currently on screen

    struct MaxMin {
        float t_max = -1000, t_min = 1000;   // outdoor temp, °C
        float h_max = -1, h_min = 101;       // outdoor humidity, %
        float gust_max = -1;                 // m/s
        float rrate_max = -1;                // mm/h
        uint8_t valid = 0;                   // bit0 temp, 1 hum, 2 gust, 3 rain-rate
    } mm_;
    bool mm_dirty_ = false;
    DisplayHistory history_;
    bool history_dirty_ = false;
    uint32_t history_save_ms_ = 0;

    // HA-injected key queue (see inject_key). Small: HA presses arrive one at a time.
    static constexpr uint8_t KEY_Q_MAX = 8;
    PanelEvent key_q_[KEY_Q_MAX];
    uint8_t key_q_head_ = 0, key_q_len_ = 0;
    std::string panel_mode_last_;

    // Pressure-trend reference ring (see ptrend_tick / draw_pressure_trend).
    float ptrend_ring_[4] = {0, 0, 0, 0};
    float ptrend_ref_ = 0;
    uint32_t ptrend_last_ms_ = 0;
    uint8_t ptrend_head_ = 0, ptrend_fill_ = 0;
    bool ptrend_valid_ = false;
    uint32_t mm_save_ms_ = 0;

    // Clock alarm (stock mode 7): 120 s ring, LIGHT snoozes 10 min, any key silences.
    bool alarm_ringing_ = false;
    uint32_t alarm_ring_ms_ = 0;
    uint32_t alarm_snooze_until_ = 0;
    bool buzzer_on_ = false;
    uint32_t beep_off_ms_ = 0;               // key-beep pulse end
    uint32_t last_beep_ms_ = 0;              // alarm cadence timer

    float disp_temp(float c) const { return pset_.unit_temp ? c * 9.0f / 5.0f + 32.0f : c; }
    float disp_wind(float ms) const {
        switch (pset_.unit_wind) {
            case 1: return ms * 3.6f;      // km/h
            case 2: return ms * 2.23694f;  // mph
            case 3: return ms * 1.94384f;  // knots
            case 5: return (float)beaufort_x10((uint32_t)lroundf(ms * 10.0f));  // Beaufort
            default: return ms;            // m/s
        }
    }
    float disp_rain(float mm) const { return pset_.unit_rain ? mm / 25.4f : mm; }
    float disp_press(float hpa) const {
        switch (pset_.unit_press) {
            case 1: return hpa * 0.02953f;   // inHg
            case 2: return hpa * 0.75006f;   // mmHg
            default: return hpa;
        }
    }

    // Link age follows the same primary source as the displayed weather values.
    // An attic packet must not keep a missing weather array's RF bars alive.
    uint32_t source_age_s(uint32_t key) const {
        const uint32_t now = millis();
        for (size_t i = 0; i < roster_count_; i++) {
            const auto &e = roster_[i];
            const uint32_t ekey = ((uint32_t)e.family << 24) | (e.id & 0xffffff);
            if (ekey == key && e.last_ms && family_enabled(e.family) &&
                !is_disabled((uint8_t)e.family, e.id)) return (now - e.last_ms) / 1000;
        }
        return 0xffffffffu;
    }
    uint32_t outdoor_age_s() const { return source_age_s(primary_array_key_); }
    // ---- extra T/H channels (WH31) ---------------------------------------------------
    // The OEM outdoor box can show any of eight WH31 channels instead of the main array;
    // the "8 CH" digit under the big readout says which. Ours lists the channels actually
    // in the roster, sorted, so TEMP walks outdoor/feels/dew, each real channel,
    // then auto-scroll. Missing, disabled and expired channels are skipped.
    static constexpr uint8_t EXT_CHAN_MAX = 8;
    uint8_t ext_chans_[EXT_CHAN_MAX] = {0};
    uint8_t ext_chan_n_ = 0;

    void refresh_ext_chans() {
        uint8_t n = 0;
        for (uint8_t ch = 1; ch <= EXT_CHAN_MAX && n < EXT_CHAN_MAX; ch++)
            if (ext_chan_slot(ch) >= 0)
                ext_chans_[n++] = ch;
        ext_chan_n_ = n;
        panel_.ext_chan_count = n;
    }
    // Roster index of the WH31 on channel `ch`, or -1. Only WH31 (and its E/B variants,
    // which decode to the same family) carries a channel number here.
    int16_t ext_chan_slot(uint8_t ch) const {
        for (size_t i = 0; i < roster_count_; i++)
            if (roster_[i].family == Family::WH31 && roster_[i].channel == ch &&
                roster_[i].last_ms != 0 && family_enabled(Family::WH31) &&
                !is_disabled((uint8_t)Family::WH31, roster_[i].id) &&
                millis() - roster_[i].last_ms <= ROSTER_SEEN_WINDOW_MS)
                return (int16_t)i;
        return -1;
    }

    bool outdoor_batt_low() const {
        for (size_t i = 0; i < roster_count_; i++) {
            if (roster_[i].family == Family::Builtin || roster_[i].last_ms == 0) continue;
            if (roster_[i].battery_low) return true;
        }
        return false;
    }

    void ptrend_tick() {
        if (!ui_.has(UiState::P)) return;
        uint32_t now = millis();
        if (ptrend_last_ms_ != 0 && now - ptrend_last_ms_ < 3600000UL) return;
        ptrend_last_ms_ = now;
        ptrend_ring_[ptrend_head_] = ui_.v[UiState::P];
        ptrend_head_ = (uint8_t)((ptrend_head_ + 1) % 4);
        if (ptrend_fill_ < 4) ptrend_fill_++;
        if (ptrend_fill_ >= 4) {           // oldest slot is 3 samples (~3 h) back
            ptrend_ref_ = ptrend_ring_[ptrend_head_];
            ptrend_valid_ = true;
        }
    }

    static constexpr uint16_t FC_HOURS = 72;   // hourly ring depth (3 days)
    static constexpr uint8_t FC_WARM = 3;      // hours of history before the first verdict
    static constexpr uint8_t FC_SHORT = 12;    // hours in the short-term mean
    static constexpr uint8_t FC_NONE = 0xff;   // stock's "no change" / invalid sentinel

    int16_t fc_hourly_[FC_HOURS] = {0};   // pressure in 0.1 hPa, [0] = newest
    uint16_t fc_fill_ = 0;          // valid entries in fc_hourly_
    uint32_t fc_hour_ms_ = 0;
    int16_t fc_ten_[6] = {0, 0, 0, 0, 0, 0};   // 10-minute ring for the storm test
    uint8_t fc_ten_fill_ = 0;
    uint32_t fc_ten_ms_ = 0;
    uint8_t fc_state_ = FC_NONE;    // 0..6, or FC_NONE while warming up
    uint8_t fc_override_ = FC_NONE; // bench override via POST /config {"panel":{"fc":n}}

    static int16_t mean16(const int16_t *v, uint16_t n) {
        int32_t sum = 0;
        for (uint16_t i = 0; i < n; i++) sum += v[i];
        return (int16_t)(sum / (int32_t)n);
    }

    // One 10-minute and one hourly pressure sample, then re-run the classifier. Called
    // from update() (16 s tick), so the intervals are checked, not assumed.
    void forecast_tick() {
        if (!ui_.has(UiState::P)) return;
        uint32_t now = millis();
        int16_t p10 = (int16_t)lroundf(ui_.v[UiState::P] * 10.0f);

        if (fc_ten_ms_ == 0 || now - fc_ten_ms_ >= 600000UL) {
            fc_ten_ms_ = now;
            for (uint8_t i = 5; i > 0; i--) fc_ten_[i] = fc_ten_[i - 1];
            fc_ten_[0] = p10;
            if (fc_ten_fill_ < 6) fc_ten_fill_++;
        }
        if (fc_hour_ms_ != 0 && now - fc_hour_ms_ < 3600000UL) return;
        fc_hour_ms_ = now;
        for (uint16_t i = FC_HOURS - 1; i > 0; i--) fc_hourly_[i] = fc_hourly_[i - 1];
        fc_hourly_[0] = p10;
        if (fc_fill_ < FC_HOURS) fc_fill_++;
        if (fc_fill_ < FC_WARM) return;

        if (fc_ten_fill_ == 6 && fc_fill_ >= FC_SHORT) {
            int16_t lo_recent = fc_ten_[0], lo_long = fc_hourly_[0];
            for (uint8_t i = 1; i < 6; i++)
                if (fc_ten_[i] < lo_recent) lo_recent = fc_ten_[i];
            for (uint16_t i = 1; i < fc_fill_; i++)
                if (fc_hourly_[i] < lo_long) lo_long = fc_hourly_[i];
            if ((int32_t)lo_recent - (int32_t)lo_long < -39) {
                fc_state_ = 5;
                fc_snow_promote();
                return;
            }
        }

        uint16_t n_short = fc_fill_ < FC_SHORT ? fc_fill_ : FC_SHORT;
        int32_t d = (int32_t)mean16(fc_hourly_, n_short) - (int32_t)mean16(fc_hourly_, fc_fill_);
        uint8_t st = FC_NONE;
        if (d > 49) st = 0;
        else if (d > 45) st = FC_NONE;
        else if (d > 4) st = 1;
        else if (d >= 0) st = FC_NONE;
        else if (d >= -30) st = 2;
        else if (d >= -34) st = FC_NONE;
        else st = 3;
        if (st != FC_NONE) fc_state_ = st;
        fc_snow_promote();
    }

    void fc_snow_promote() {
        if (fc_state_ != 3 && fc_state_ != 5) return;
        if (ui_.has(UiState::T_OUT) && ui_.v[UiState::T_OUT] <= 0.0f) fc_state_++;
    }

    uint8_t moon_phase() const {
        if (moon_override_ < 8) return moon_override_;
        if (time_clock == nullptr) return 0xff;
        ESPTime t = time_clock->utcnow();
        if (!t.is_valid()) return 0xff;
        double days = ((double)t.timestamp - 947182440.0) / 86400.0;   // since that new moon
        double frac = days / 29.530588853;
        frac -= floor(frac);
        return (uint8_t)((uint32_t)lround(frac * 8.0) & 7u);
    }
    uint8_t moon_override_ = 0xff;

    // ---- sunrise / sunset ------------------------------------------------------------
    // esphome's sun component does the astronomy; it returns the event in UTC, so convert
    // back to local for the glass. Returns false when there is no sun_id, no valid clock,
    // or the sun never crosses the horizon (polar day/night) — the caller then renders the
    // stock's "--:--".
    bool sun_event(bool rising, uint8_t &hh, uint8_t &mm) const {
        if (sun_ == nullptr || time_clock == nullptr) return false;
        ESPTime today = time_clock->utcnow();
        if (!today.is_valid()) return false;
        today.hour = today.minute = today.second = 0;
        today.recalc_timestamp_utc();
        optional<ESPTime> e = rising ? sun_->sunrise(today, -0.83333) : sun_->sunset(today, -0.83333);
        if (!e.has_value()) return false;
        ESPTime l = ESPTime::from_epoch_local(e->timestamp);
        hh = l.hour;
        mm = l.minute;
        return true;
    }

    // ---- derived temperatures (manual §3.3.12 + standard formulas) ----
    static float dew_point_c(float t, float rh) {  // Magnus
        if (rh <= 0.0f) return NAN;
        const float a = 17.27f, b = 237.7f;
        float al = (a * t) / (b + t) + logf(rh / 100.0f);
        return b * al / (a - al);
    }
    static float heat_index_c(float t, float rh) {  // Rothfusz regression (F-domain)
        float tf = t * 9.0f / 5.0f + 32.0f;
        float hi = -42.379f + 2.04901523f * tf + 10.14333127f * rh - 0.22475541f * tf * rh -
                   0.00683783f * tf * tf - 0.05481717f * rh * rh + 0.00122874f * tf * tf * rh +
                   0.00085282f * tf * rh * rh - 0.00000199f * tf * tf * rh * rh;
        return (hi - 32.0f) * 5.0f / 9.0f;
    }
    static float wind_chill_c(float t, float ms) {  // Environment Canada (km/h)
        float v = ms * 3.6f;
        if (v < 4.8f) return t;
        float p = powf(v, 0.16f);
        return 13.12f + 0.6215f * t - 11.37f * p + 0.3965f * t * p;
    }
    // Feels Like: <10 °C wind chill, 10..26.7 °C actual, >26.7 °C heat index (manual).
    float feels_like_c(float t, float rh, float ms) const {
        if (t < 10.0f) return wind_chill_c(t, ms);
        if (t > 26.7f) return heat_index_c(t, rh);
        return t;
    }

    // ---- MAX/MIN tracking (called from the FO decode path, calibrated values) ----
    // T/H/gust only from the primary array, rain rate only from the primary rain sensor:
    // a neighbour's (or a disabled) array used to write the persisted all-time records.
    void track_maxmin(const Reading &r, bool array_primary, bool rain_primary) {
        if (array_primary && r.has(F_TEMP)) {
            if (!(mm_.valid & 1)) { mm_.t_max = mm_.t_min = r.temp_c; mm_.valid |= 1; mm_dirty_ = true; }
            else if (r.temp_c > mm_.t_max) { mm_.t_max = r.temp_c; mm_dirty_ = true; }
            else if (r.temp_c < mm_.t_min) { mm_.t_min = r.temp_c; mm_dirty_ = true; }
        }
        if (array_primary && r.has(F_HUM)) {
            if (!(mm_.valid & 2)) { mm_.h_max = mm_.h_min = r.humidity; mm_.valid |= 2; mm_dirty_ = true; }
            else if (r.humidity > mm_.h_max) { mm_.h_max = r.humidity; mm_dirty_ = true; }
            else if (r.humidity < mm_.h_min) { mm_.h_min = r.humidity; mm_dirty_ = true; }
        }
        if (array_primary && r.has(F_GUST) && (!(mm_.valid & 4) || r.gust_mps > mm_.gust_max)) {
            mm_.gust_max = r.gust_mps; mm_.valid |= 4; mm_dirty_ = true;
        }
        if (rain_primary && r.rain_rate_mmh > 0 && (!(mm_.valid & 8) || r.rain_rate_mmh > mm_.rrate_max)) {
            mm_.rrate_max = r.rain_rate_mmh; mm_.valid |= 8; mm_dirty_ = true;
        }
        // Throttle flash writes to one per minute at most.
        if (mm_dirty_ && millis() - mm_save_ms_ > 60000) {
            mm_save_ms_ = millis();
            mm_dirty_ = false;
            save_maxmin();
        }
    }

    // Feed only accepted primary-array/rain readings, or the console's own I2C data.
    // Derived extrema use simultaneous T/H/wind, never independent extrema combined later.
    void track_display_history(const Reading &r, bool array, bool rain, bool indoor = false) {
        using H = DisplayHistory;
        auto add = [this](H::Field f, float v) { history_dirty_ |= history_.sample(f, v); };
        if (indoor) {
            if (r.has(F_TEMP)) add(H::IN_TEMP, r.temp_c);
            if (r.has(F_HUM)) add(H::IN_HUM, r.humidity);
            if (r.has(F_PRESS)) add(H::PRESS, r.pressure_hpa);
        }
        if (array) {
            if (r.has(F_TEMP)) add(H::TEMP, r.temp_c);
            if (r.has(F_HUM)) add(H::HUM, r.humidity);
            if (r.has(F_TEMP) && r.has(F_HUM)) add(H::DEW, dew_point_c(r.temp_c, r.humidity));
            if (r.has(F_TEMP)) {
                const bool humidity_ok = r.temp_c <= 26.7f || r.has(F_HUM);
                const bool wind_ok = r.temp_c >= 10.0f || r.has(F_WIND);
                if (humidity_ok && wind_ok) add(H::FEELS, feels_like_c(r.temp_c, r.humidity, r.wind_mps));
            }
            if (r.has(F_WIND)) add(H::WIND, r.wind_mps);
            if (r.has(F_GUST)) add(H::GUST, r.gust_mps);
            if (r.has(F_UV)) add(H::UV, r.uv);
            if (r.has(F_LIGHT)) add(H::SOLAR, r.light_wm2);
        }
        if (rain) add(H::RAIN_RATE, r.rain_rate_mmh); // finite zero is a real record
        if (history_dirty_ && millis() - history_save_ms_ >= 60000) save_display_history();
    }

    void save_display_history() {
        if (!history_dirty_) return;
        // Keep the signature and its legacy blob coherent across a power loss.
        if (mm_dirty_) {
            save_maxmin();
            mm_dirty_ = false;
            mm_save_ms_ = millis();
        }
        const float legacy[6] = {mm_.t_max, mm_.t_min, mm_.h_max, mm_.h_min, mm_.gust_max, mm_.rrate_max};
        history_.legacy_signature = DisplayHistory::signature(legacy, mm_.valid);
        history_pref_.save(&history_);
        global_preferences->sync();
        history_dirty_ = false;
        history_save_ms_ = millis();
    }

    void reset_maxmin(bool max_side) {
        history_.reset(max_side);
        history_dirty_ = true;
        // Re-seed the reset side from the live reading (sentinel if none seen yet) so
        // the opposite side's records survive — temp/hum share one valid bit per pair.
        float t = ui_.has(UiState::T_OUT) ? ui_.v[UiState::T_OUT] : NAN;
        float h = ui_.has(UiState::H_OUT) ? ui_.v[UiState::H_OUT] : NAN;
        if (max_side) {
            mm_.t_max = std::isnan(t) ? -1000 : t;
            mm_.h_max = std::isnan(h) ? -1 : h;
            mm_.gust_max = -1;
            mm_.rrate_max = -1;
            mm_.valid &= (uint8_t)~0x0C;  // gust + rain-rate re-seed from scratch
            if (!std::isnan(t)) mm_.valid |= 1;
            if (!std::isnan(h)) mm_.valid |= 2;
        } else {
            mm_.t_min = std::isnan(t) ? 1000 : t;
            mm_.h_min = std::isnan(h) ? 101 : h;
            if (!std::isnan(t)) mm_.valid |= 1;
            if (!std::isnan(h)) mm_.valid |= 2;
        }
        save_maxmin();
        save_display_history();
    }

    static void fmt_x10(float v, int32_t &out) { out = (int32_t)lroundf(v * 10.0f); }

    // Outdoor temperature in display-unit tenths, including the separate minus stroke.
    void draw_outdoor_temp(int32_t v10, bool missing) {
        uint8_t b[6];
        seg(fb_, SEG_OUT_DP, true);
        seg(fb_, SEG_OUT_C, pset_.unit_temp == 0);
        seg(fb_, SEG_OUT_F, pset_.unit_temp == 1);
        if (missing) {
            fmt_dashes(b);
        } else {
            int32_t c = v10 < -400 ? -400 : (v10 > 1999 ? 1999 : v10);
            fmt_decimal(c, b);
            if (v10 >= -9 && v10 <= 9) {
                b[1] = 0;
                if (v10 < 0) b[2] = SM_MINUS;
            }
        }
        seg(fb_, SEG_OUT_ONE, b[3] == 1);
        seg(fb_, SEG_OUT_MINUS, b[3] == SM_MINUS);
        draw_small_glyph(fb_, SM_DIGITS[3], b[2]);
        draw_small_glyph(fb_, OT_TENS, b[1]);
        draw_small_glyph(fb_, OT_UNITS, b[0]);
    }

    // Outdoor humidity with leading-zero blanking.
    void draw_outdoor_hum(int32_t rh, bool missing) {
        uint8_t b[6];
        seg(fb_, SEG_OUTH_PCT, true);
        if (missing) {
            fmt_dashes(b);
        } else {
            fmt_pair((uint32_t)(rh < 0 ? 0 : (rh > 99 ? 99 : rh)), b);
            if (b[1] == 0) b[1] = SM_BLANK;
        }
        draw_small_glyph(fb_, OH_TENS, b[1]);
        draw_small_glyph(fb_, OH_UNITS, b[0]);
    }

    // Indoor temperature in display-unit tenths; the glass has no hundreds position.
    void draw_indoor_temp(int32_t v10, bool missing) {
        uint8_t b[6];
        if (missing) {
            fmt_dashes(b);
            b[3] = 0;
            b[0] = SM_BLANK;   // stock blanks the units digit on "no sensor"
        } else {
            int32_t c = v10 < -99 ? -99 : v10;
            fmt_decimal(c, b);
            if (c >= -9 && c <= 9) {
                b[1] = 0;
                if (c < 0) b[2] = SM_MINUS;
            }
        }
        seg(fb_, SEG_IN_DP, true);
        seg(fb_, SEG_IN_C, pset_.unit_temp == 0);
        seg(fb_, SEG_IN_F, pset_.unit_temp == 1);
        seg(fb_, SEG_IN_ONE, b[3] == 1);
        draw_small_glyph(fb_, SM_DIGITS[0], b[2]);   // SM0 = D300
        draw_small_glyph(fb_, SM_DIGITS[1], b[1]);   // SM1 = D310
        draw_small_glyph(fb_, D320, b[0]);
    }

    void draw_indoor_hum(int32_t rh, bool missing) {
        uint8_t b[6];
        if (missing) {
            fmt_dashes(b);
        } else {
            fmt_pair((uint32_t)(rh < 0 ? 0 : (rh > 99 ? 99 : rh)), b);
            if (b[1] == 0) b[1] = SM_BLANK;
        }
        seg(fb_, SEG_INH_PCT, true);
        draw_small_glyph(fb_, D340, b[1]);
        draw_small_glyph(fb_, D350, b[0]);
    }

    // All fields share a 500 ms blink phase, sampled once per repaint.
    bool blink_on_ = true;
    // An actively adjusted field stays visible briefly between repeated key events.
    bool blink_vis() const { return blink_on_ || (millis() - panel_.last_adj_ms) < 700; }

    // Relative pressure = measured absolute pressure + persisted user offset.
    // Use a separate preference to preserve the fourteen-byte PanelSettings layout.
    float rel_off_hpa_ = 0.0f;
    float press_abs_hpa() const { return ui_.v[UiState::P]; }
    float press_rel_hpa() const { return ui_.v[UiState::P] + rel_off_hpa_; }

    // Apply one displayed least-significant unit per key step; clamp REL to 700..1200 hPa.
    void apply_rel_steps() {
        if (panel_.rel_steps == 0) return;
        float lsb = pset_.unit_press == 1 ? 0.01f * 33.8639f
                  : pset_.unit_press == 2 ? 0.1f * 1.333224f : 0.1f;
        float ab = ui_.has(UiState::P) ? press_abs_hpa() : 1013.25f;
        float rel = ab + rel_off_hpa_ + (float)panel_.rel_steps * lsb;
        rel = std::clamp(rel, 700.0f, 1200.0f);
        rel_off_hpa_ = rel - ab;
        upload.rel_offset_hpa = rel_off_hpa_;
        panel_.rel_steps = 0;
        rel_pref_.save(&rel_off_hpa_);   // flushed by the preferences' own write interval
    }

    // Pressure uses AL0..AL3, which overlap SM6..SM9: do not draw both.
    // Scaling is hPa x10, inHg x100, mmHg x10; decimal points remain visible for missing data.
    void draw_pressure(float hpa, bool missing, bool rel, bool units_vis = true,
                       bool digits_vis = true) {
        uint8_t b[6];
        seg(fb_, SEG_PR_LABEL, true);
        seg(fb_, SEG_PR_ABS, !rel);
        seg(fb_, SEG_PR_REL, rel);
        seg(fb_, SEG_PR_HPA, units_vis && pset_.unit_press == 0);
        seg(fb_, SEG_PR_INHG, units_vis && pset_.unit_press == 1);
        seg(fb_, SEG_PR_MMHG, units_vis && pset_.unit_press == 2);
        seg(fb_, SEG_PR_DP_INHG, pset_.unit_press == 1);
        seg(fb_, SEG_PR_DP_HPA, pset_.unit_press != 1);
        if (!digits_vis) {
            fmt_blank(b);
        } else if (missing) {
            fmt_dashes(b);
        } else {
            float d = disp_press(hpa);
            int32_t v = (int32_t)lroundf(d * (pset_.unit_press == 1 ? 100.0f : 10.0f));
            if (v < 0) v = 0;
            if (v > 19999) v = 19999;
            fmt_decimal(v, b);
        }
        seg(fb_, SEG_PR_ONE, b[4] == 1);
        draw_small_glyph(fb_, AL_DIGITS[0], b[3]);
        draw_small_glyph(fb_, AL_DIGITS[1], b[2]);
        draw_small_glyph(fb_, AL_DIGITS[2], b[1]);
        draw_small_glyph(fb_, AL_DIGITS[3], b[0]);
    }

    // Hour-tens uses three individual electrodes. Preserve minute leading zeroes;
    // hide selected fields during blink-off and respect the 12/24-hour setting.
    void draw_clock_digits(uint8_t h24, uint8_t mm, bool show_h, bool show_m) {
        uint8_t hb[6], mb[6];
        if (show_h) {
            fmt_pair(disp_hour(h24), hb);
        } else {
            fmt_blank(hb);
        }
        seg(fb_, SEG_TIME_PM, show_h && pset_.h12 && h24 >= 12);
        seg(fb_, SEG_TIME_T2, hb[1] == 2);
        seg(fb_, SEG_TIME_T12, hb[1] == 1 || hb[1] == 2);
        seg(fb_, SEG_TIME_T1, hb[1] == 1);
        draw_small_glyph(fb_, TA, hb[0]);
        if (show_m) {
            fmt_pair(mm, mb);
        } else {
            fmt_blank(mb);
        }
        draw_small_glyph(fb_, TB, mb[1]);
        draw_small_glyph(fb_, TC, mb[0]);
    }

    void draw_clock_year(uint16_t year, bool show) {
        uint8_t b[6];
        if (show) {
            fmt_decimal(year, b);
        } else {
            fmt_blank(b);
        }
        seg(fb_, SEG_TIME_T2, b[3] == 2);
        seg(fb_, SEG_TIME_T12, b[3] == 1 || b[3] == 2);
        seg(fb_, SEG_TIME_T1, b[3] == 1);
        draw_small_glyph(fb_, TA, b[2]);
        draw_small_glyph(fb_, TB, b[1]);
        draw_small_glyph(fb_, TC, b[0]);
    }

    void draw_time(bool blink) {
        seg(fb_, SEG_TIME_LABEL, true);     // the "TIME" caption; stock lights it every page
        seg(fb_, SEG_TIME_COLON, blink);
        bool valid = false, dst = false;
        uint8_t hh = 0, mm = 0;
        if (time_clock != nullptr) {
            ESPTime t = time_clock->now();
            if (t.is_valid()) {
                valid = true;
                mm = t.minute;
                hh = t.hour;
                dst = t.is_dst;
            }
        }
        seg(fb_, SEG_TIME_DST, valid && dst);
        seg(fb_, SEG_TIME_BELL, pset_.alarm_on != 0 && (blink || !alarm_ringing_));
        seg(fb_, SEG_TIME_SNOOZE, alarm_snooze_until_ > millis());
        if (!valid) {
            seg(fb_, SEG_TIME_PM, false);
            seg(fb_, SEG_TIME_T12, false);
            seg(fb_, SEG_TIME_T1, false);
            seg(fb_, SEG_TIME_T2, false);
            draw_small_glyph(fb_, TA, SM_MINUS);
            draw_small_glyph(fb_, TB, SM_MINUS);
            draw_small_glyph(fb_, TC, SM_MINUS);
            return;
        }
        draw_clock_digits(hh, mm, true, true);
    }

    int32_t wind_disp_x10(float ms) const {
        int32_t v = (int32_t)lroundf(ms * 10.0f);
        if (v < 0) v = 0;
        switch (pset_.unit_wind) {
            case 1: return (v * 36 + 5) / 10;             // km/h
            case 2: return (v * 22369 + 5000) / 10000;    // mph
            case 3: return (v * 19438 + 5000) / 10000;    // knots
            case 5: return beaufort_x10((uint32_t)v);     // Beaufort, no decimal
            default: return v;                            // m/s
        }
    }

    // Show one decimal below 100.0 and whole units above; Beaufort has no decimal.
    void wind_digits(float ms, bool missing) {
        uint8_t b[6];
        bool dp = false;
        if (missing) {
            fmt_dashes(b);
        } else {
            int32_t v = wind_disp_x10(ms);
            bool bft = pset_.unit_wind == 5;
            if (!bft && v <= 999) dp = true;
            else if (!bft) v = (v + 5) / 10;
            if (v > 999) v = 999;
            fmt_decimal(v, b);
            if (dp && v <= 9) b[1] = 0;
        }
        seg(fb_, SEG_WIND_DP, dp);
        draw_small_glyph(fb_, WS_HUND, b[2]);
        draw_small_glyph(fb_, WS_TENS, b[1]);
        draw_small_glyph(fb_, WS_UNITS, b[0]);
    }

    void wind_labels(uint8_t sel, bool units_vis) {
        bool on = units_vis && sel < 2;
        seg(fb_, SEG_WIND_UNITS, true);
        seg(fb_, SEG_WIND_GUST, sel == 1);
        seg(fb_, SEG_WIND_AVG, sel == 3);
        seg(fb_, SEG_WIND_MS, on && pset_.unit_wind == 0);
        seg(fb_, SEG_WIND_KMH, on && pset_.unit_wind == 1);
        seg(fb_, SEG_WIND_MPH, on && pset_.unit_wind == 2);
        seg(fb_, SEG_WIND_KNOTS, on && pset_.unit_wind == 3);
        seg(fb_, SEG_WIND_BFT, on && pset_.unit_wind == 5);
    }

    void draw_wind(uint8_t sel, bool units_vis = true) {
        bool dir_mode = sel >= 2;
        wind_labels(sel, units_vis);
        seg(fb_, SEG_WIND_DEG_A, dir_mode);
        seg(fb_, SEG_WIND_DEG_B, dir_mode);
        seg(fb_, SEG_WIND_BATT, field_batt_low(UiState::WSPD, UiState::WGUST));

        if (dir_mode) {
            uint8_t b[6];
            // Mode 3 falls back to the current direction when there is no average yet.
            uint8_t di = (sel == 3 && ui_.has(UiState::WIND_AVG)) ? UiState::WIND_AVG
                                                                  : UiState::WDIR;
            if (!ui_.has(di)) {
                fmt_dashes(b);
            } else {
                int32_t deg = (int32_t)lroundf(ui_.v[di]);
                fmt_decimal(((deg % 360) + 360) % 360, b);
            }
            seg(fb_, SEG_WIND_DP, false);
            draw_small_glyph(fb_, WS_HUND, b[2]);
            draw_small_glyph(fb_, WS_TENS, b[1]);
            draw_small_glyph(fb_, WS_UNITS, b[0]);
        } else {
            uint8_t idx = (sel == 1) ? UiState::WGUST : UiState::WSPD;
            wind_digits(ui_.has(idx) ? ui_.v[idx] : 0.0f, !ui_.has(idx));
        }

        rose(fb_, ui_.has(UiState::WDIR) ? (int)ui_.v[UiState::WDIR] : -1, 0);
        rose_mask(fb_, wdir_hist_mask(), 1);
    }

    // Upper rain row has three digits plus leading-one; lower row has four digits.
    // Use different decimal thresholds to fit their respective ranges.
    uint8_t rain_scale(float mm, bool four_digit, int32_t &out) const {
        bool inch = pset_.unit_rain != 0;
        int32_t v = (int32_t)lroundf(disp_rain(mm) * (inch ? 100.0f : 10.0f));
        if (v < 0) v = 0;
        if (!four_digit) {                          // rate row
            if (v > 19990) v = 19990;
            if (!inch) {
                if (v <= 999) { out = v; return 1; }
                out = (v + 5) / 10;
                return 0;
            }
            if (v > 9999) { out = (v + 5) / 100; return 0; }
            if (v <= 999) { out = v; return 2; }
            out = (v + 5) / 10;
            return 1;
        }
        if (v > 99998) { out = 9999; return 0; }    // accumulation row
        if (v <= 9999) { out = v; return inch ? 2 : 1; }
        out = (v + 5) / 10;
        return inch ? 1 : 0;
    }

    void draw_rain_rate(bool event, float mm, bool missing, bool units_vis = true) {
        seg(fb_, SEG_RAIN_LABEL, true);
        seg(fb_, SEG_R1_RATE_A, !event);
        seg(fb_, SEG_R1_RATE_B, !event);
        seg(fb_, SEG_R1_EVENT, event);

        uint8_t b[6], dec = 0;
        if (missing) {
            fmt_dashes(b);
        } else {
            int32_t v;
            dec = rain_scale(mm, false, v);        // 3 digits + the leading "1"
            fmt_decimal(v, b);
            if (dec == 2 && v <= 99) b[2] = 0;
            if (dec != 0 && v <= 9) b[1] = 0;
        }
        seg(fb_, SEG_R1_UNIT_IN, units_vis && pset_.unit_rain != 0);
        seg(fb_, SEG_R1_UNIT_MM, units_vis && pset_.unit_rain == 0);
        seg(fb_, SEG_R1_DP2, dec == 2);
        seg(fb_, SEG_R1_DP1, dec == 1);
        seg(fb_, SEG_R1_ONE, b[3] == 1);
        draw_small_glyph(fb_, R1_HUND, b[2]);
        draw_small_glyph(fb_, R1_TENS, b[1]);
        draw_small_glyph(fb_, R1_UNITS, b[0]);
        draw_rain_bar(mm, missing);
    }

    // The live rate/event figure for page 0 (RAIN1 toggles panel_.rain_event).
    void rain_live(bool event, float &mm, bool &missing) {
        mm = 0;
        missing = true;
        if (!event) {
            if (ui_.has(UiState::RAIN_RATE)) { mm = ui_.v[UiState::RAIN_RATE]; missing = false; }
        } else if (rain_.count() && rain_.buckets()[0].last_counter >= 0.0f) {
            mm = rain_.buckets()[0].event_mm;
            missing = false;
        }
    }

    void draw_rain_accum(uint8_t r) {
        seg(fb_, SEG_R2_DAILY, r == 3);
        seg(fb_, SEG_R2_WEEKLY, r == 4);
        seg(fb_, SEG_R2_MONTHLY, r == 5);
        seg(fb_, SEG_R2_YEARLY, r == 6);
        seg(fb_, SEG_R2_TOTAL, r >= 7);

        float mm = 0;
        bool missing = true;
        if (rain_.count() && rain_.buckets()[0].last_counter >= 0.0f) {
            const RainAggregator::Bucket &bk = rain_.buckets()[0];
            mm = r == 3 ? bk.day_mm : r == 4 ? bk.week_mm : r == 5 ? bk.month_mm
                 : r == 6 ? bk.year_mm : bk.last_counter;
            missing = false;
        }
        uint8_t b[6], dec = 0;
        if (missing) {
            fmt_dashes(b);
        } else {
            int32_t v;
            dec = rain_scale(mm, true, v);         // 4 digits
            fmt_decimal(v, b);
            if (dec == 2 && v <= 99) b[2] = 0;
            if (dec != 0 && v <= 9) b[1] = 0;
        }
        seg(fb_, SEG_R2_DP2, dec == 2);
        seg(fb_, SEG_R2_DP1, dec == 1);
        draw_small_glyph(fb_, R2_THOU, b[3]);
        draw_small_glyph(fb_, R2_HUND, b[2]);
        draw_small_glyph(fb_, R2_TENS, b[1]);
        draw_small_glyph(fb_, R2_UNITS, b[0]);
    }

    void draw_rain_bar(float mm, bool missing) {
        seg(fb_, SEG_RAIN_BAR_FRAME, true);
        bool inch = pset_.unit_rain != 0;
        seg(fb_, SEG_RAIN_UNIT_IN, inch);
        seg(fb_, SEG_RAIN_UNIT_MM, !inch);
        uint8_t n = 0;
        if (!missing) {
            int32_t v = (int32_t)lroundf(disp_rain(mm) * (inch ? 100.0f : 10.0f));
            if (v < 0) v = 0;
            if (inch)
                n = v > 390 ? 6 : v > 200 ? 5 : v > 100 ? 4 : v > 40 ? 3 : v > 20 ? 2 : v ? 1 : 0;
            else {
                int32_t t = v * 10;
                n = t > 9998 ? 6 : t > 5000 ? 5 : t > 2500 ? 4 : t > 1000 ? 3 : t > 500 ? 2
                                                                                       : (t ? 1 : 0);
            }
        }
        for (uint8_t i = 0; i < 6; i++) seg(fb_, RAIN_BAR[i], i < n);
    }

    // Eleven-position pointer from the current pressure relative to its moving baseline.
    void draw_pressure_trend() {
        seg(fb_, SEG_TREND_ZONE, true);
        seg(fb_, SEG_TREND_AXIS, true);
        int lvl = -1;   // -1 = reference not established yet -> all 11 dark, like stock
        if (ui_.has(UiState::P) && ptrend_valid_) {
            // Ladder edges are stock's, in 0.1 hPa, so compare in hPa regardless of the
            // display unit the user picked.
            int32_t d = (int32_t)lroundf((ui_.v[UiState::P] - ptrend_ref_) * 10.0f);
            lvl = d > 49 ? 0 : d > 39 ? 1 : d > 29 ? 2 : d > 19 ? 3 : d > 9 ? 4
                  : d >= -9 ? 5 : d < -49 ? 10 : d < -39 ? 9 : d < -29 ? 8 : d < -19 ? 7 : 6;
        }
        for (uint8_t i = 0; i < PTREND_N; i++) seg(fb_, BAR22[i], (int)i == lvl);
    }

    void draw_batt_channel() {
        for (uint8_t i = 0; i < 6; i++) seg(fb_, CH_MARK[i], i == 0);
        seg(fb_, SEG_AQ_BATT, false);
        seg(fb_, SEG_ROW_DATE, true);
        seg(fb_, SEG_ROW_AQ, false);
        seg(fb_, SEG_ROW_AQU, false);
        draw_date_row();
    }

    // Three fourteen-segment letters, then two day-of-month digits; no leading-zero blanking.
    void draw_date_row() {
        bool ok = false;
        ESPTime t{};
        if (time_clock != nullptr) {
            t = time_clock->now();
            ok = t.is_valid();
        }
        if (!ok) {
            big_text(fb_, "---");
            draw_small_glyph(fb_, Z0, SM_MINUS);
            draw_small_glyph(fb_, Z1, SM_MINUS);
            return;
        }
        if (panel_.date_sel == 0) {
            uint8_t mi = (uint8_t)((t.month >= 1 && t.month <= 12) ? t.month - 1 : 0);
            big_text(fb_, MONTH3[mi]);
        } else {
            // ESPTime::day_of_week is 1..7 with 1 = Sunday; the stock table is 0-based.
            uint8_t wi = (uint8_t)((t.day_of_week >= 1 && t.day_of_week <= 7) ? t.day_of_week - 1 : 0);
            big_text(fb_, WDAY3[wi]);
        }
        uint8_t b[6];
        fmt_pair(t.day_of_month, b);
        draw_small_glyph(fb_, Z0, b[1]);
        draw_small_glyph(fb_, Z1, b[0]);
    }

    // Hemisphere selects crescent orientation; an invalid clock leaves the moon dark.
    void draw_moon(bool visible = true) {
        uint8_t ph = moon_phase();
        uint8_t mask = (ph < 8 && visible) ? MOON_MASK[ph] : 0;
        const uint16_t *tab = moon_seg(pset_.hemisphere != 0);
        for (uint8_t i = 0; i < 4; i++) seg(fb_, tab[i], ((mask >> i) & 1) != 0);
    }

    // Forecast states 0..4 compose fixed weather icons. States 5/6 flash rain/snow;
    // unknown states, including warm-up, leave the box dark.
    void draw_forecast(bool blink) {
        uint8_t st = (fc_override_ != FC_NONE) ? fc_override_ : fc_state_;
        if (st == 5 || st == 6) st = blink ? (uint8_t)(st - 2) : FC_NONE;
        seg(fb_, SEG_FC_SUN, st < 2);
        seg(fb_, SEG_FC_CLOUD, st >= 1 && st <= 4);
        seg(fb_, SEG_FC_RAIN, st == 3);
        seg(fb_, SEG_FC_SNOW, st == 4);
    }

    void draw_wifi_icon() {
        uint8_t lvl = 0;
        if (wifi::global_wifi_component != nullptr &&
            wifi::global_wifi_component->is_connected()) {
            int r = wifi::global_wifi_component->wifi_rssi();
            lvl = r >= -55 ? 4 : r >= -67 ? 3 : r >= -78 ? 2 : 1;
        }
        for (uint8_t i = 0; i < 4; i++) seg(fb_, WIFI_BARS[i], i < lvl);
    }

    // Reception bars reflect packet age, a freshness proxy rather than an RSSI measurement.
    void draw_rf_strip(int16_t ext_slot = -1) {
        uint32_t age = ext_slot >= 0
                           ? ((roster_[ext_slot].last_ms == 0)
                                  ? 0xffffffffu
                                  : (millis() - roster_[ext_slot].last_ms) / 1000)
                           : outdoor_age_s();
        uint8_t lvl = age == 0xffffffffu ? 0
                      : age < 90 ? 4 : age < 300 ? 3 : age < 900 ? 2 : age < 3600 ? 1 : 0;
        seg(fb_, SEG_RF_ANT, age != 0xffffffffu);
        for (uint8_t i = 0; i < 4; i++) seg(fb_, RF_BARS[i], i < lvl);
    }

    void draw_rf_bars(uint16_t ant, const uint16_t *bars, uint32_t age) {
        uint8_t lvl = age == 0xffffffffu ? 0
                      : age < 90 ? 4 : age < 300 ? 3 : age < 900 ? 2 : age < 3600 ? 1 : 0;
        seg(fb_, ant, age != 0xffffffffu);
        for (uint8_t i = 0; i < 4; i++) seg(fb_, bars[i], i < lvl);
    }

    void draw_rot_zone() {
        bool sun_mode = panel_.rot_sun;
        seg(fb_, SEG_ROT_DATE_A, sun_mode);
        seg(fb_, SEG_ROT_DATE_B, sun_mode);
        seg(fb_, SEG_ROT_UV_A, !sun_mode);
        seg(fb_, SEG_ROT_UV_B, !sun_mode);
        seg(fb_, SEG_ROT_UV_C, !sun_mode);
        seg(fb_, SEG_SOL_DP1, false);
        seg(fb_, SEG_SOL_DP2, false);
        if (sun_mode) {
            for (uint16_t sgm : UVI_BARS) seg(fb_, sgm, false);
            draw_sun_times();
        } else {
            draw_uvi_solar();
        }
    }

    // Sunrise and sunset use the UVI/solar rows as HH:MM, with partial hour-tens electrodes.
    void draw_sun_times() {
        uint8_t hh = 0, mm = 0, b[6];
        // --- sunrise -> UVI row (electrodes + Y0 : Y1 Y2) ---
        if (!sun_event(true, hh, mm)) {
            seg(fb_, SEG_SR_T1, false);
            seg(fb_, SEG_SR_T2, false);
            seg(fb_, SEG_SR_T12, false);
            draw_small_glyph(fb_, Y0, SM_MINUS);
            draw_small_glyph(fb_, Y1, SM_MINUS);
            draw_small_glyph(fb_, Y2, SM_MINUS);
        } else {
            fmt_pair(disp_hour(hh), b);
            seg(fb_, SEG_SR_T2, b[1] == 2);
            seg(fb_, SEG_SR_T12, b[1] == 1 || b[1] == 2);
            seg(fb_, SEG_SR_T1, b[1] == 1);
            draw_small_glyph(fb_, Y0, b[0]);
            fmt_pair(mm, b);
            draw_small_glyph(fb_, Y1, b[1]);
            draw_small_glyph(fb_, Y2, b[0]);
        }
        // --- sunset -> SOLAR row (SEG_SOL_T* + SO_HUND : SO_TENS SO_UNITS) ---
        seg(fb_, SEG_SOL_KLUX, false);
        seg(fb_, SEG_SOL_KFC, false);
        seg(fb_, SEG_SOL_WM2, false);
        if (!sun_event(false, hh, mm)) {
            seg(fb_, SEG_SOL_T1, false);
            seg(fb_, SEG_SOL_T2, false);
            seg(fb_, SEG_SOL_T12, false);
            draw_small_glyph(fb_, SO_HUND, SM_MINUS);
            draw_small_glyph(fb_, SO_TENS, SM_MINUS);
            draw_small_glyph(fb_, SO_UNITS, SM_MINUS);
            return;
        }
        fmt_pair(disp_hour(hh), b);
        seg(fb_, SEG_SOL_T2, b[1] == 2);
        seg(fb_, SEG_SOL_T12, b[1] == 1 || b[1] == 2);
        seg(fb_, SEG_SOL_T1, b[1] == 1);
        draw_small_glyph(fb_, SO_HUND, b[0]);
        fmt_pair(mm, b);
        draw_small_glyph(fb_, SO_TENS, b[1]);
        draw_small_glyph(fb_, SO_UNITS, b[0]);
    }

    uint8_t disp_hour(uint8_t h) const {
        if (!pset_.h12) return h;
        uint8_t r = (uint8_t)(h % 12);
        return r == 0 ? 12 : r;
    }

    // Clear sunrise hour-tens electrodes before drawing UVI. Missing data shows dashes.
    void draw_uvi(int32_t uv, bool missing) {
        seg(fb_, SEG_SR_T1, false);
        seg(fb_, SEG_SR_T2, false);
        seg(fb_, SEG_SR_T12, false);
        uint8_t b[6];
        if (missing) {
            fmt_dashes(b);
        } else {
            fmt_decimal(uv < 0 ? 0 : (uv > 99 ? 99 : uv), b);
        }
        draw_small_glyph(fb_, Y0, b[1]);
        draw_small_glyph(fb_, Y1, b[0]);
        draw_small_glyph(fb_, Y2, SM_BLANK);
        int32_t u = missing ? 0 : uv;
        uint8_t lvl = uvi_level(u);
        for (uint8_t i = 0; i < 5; i++) seg(fb_, UVI_BARS[i], i < lvl);
    }

    // Solar units: 0=kLux, 1=kfc, 2=W/m2. Fit two decimals below 30,
    // one below 1000, then whole units capped at 2999.
    void draw_solar(float wm2, bool missing, bool units_vis = true) {
        seg(fb_, SEG_SOL_KLUX, units_vis && pset_.unit_solar == 0);
        seg(fb_, SEG_SOL_KFC, units_vis && pset_.unit_solar == 1);
        seg(fb_, SEG_SOL_WM2, units_vis && pset_.unit_solar == 2);
        uint8_t s[6], dec = 0;
        if (missing) {
            fmt_dashes(s);
        } else {
            float shown = pset_.unit_solar == 2 ? wm2
                          : pset_.unit_solar == 1 ? wm2 * 0.1267f * 0.0929f   // kfc
                                                  : wm2 * 0.1267f;            // kLux
            int32_t v = (int32_t)lroundf(shown * 100.0f);
            if (v < 0) v = 0;
            if (v <= 2999) {
                dec = 2;
            } else if (v <= 9999) {
                v = (v + 5) / 10;
                dec = 1;
            } else {
                v = (v + 5) / 100;
                if (v > 2999) v = 2999;
                dec = 0;
            }
            fmt_decimal(v, s);
        }
        seg(fb_, SEG_SOL_DP1, dec == 1);
        seg(fb_, SEG_SOL_DP2, dec == 2);
        seg(fb_, SEG_SOL_T2, s[3] == 2);
        seg(fb_, SEG_SOL_T12, s[3] == 1 || s[3] == 2);
        seg(fb_, SEG_SOL_T1, s[3] == 1);
        draw_small_glyph(fb_, SO_HUND, s[2]);
        draw_small_glyph(fb_, SO_TENS, s[1]);
        draw_small_glyph(fb_, SO_UNITS, s[0]);
    }

    void draw_uvi_solar() {
        draw_uvi(ui_.has(UiState::UV) ? (int32_t)lroundf(ui_.v[UiState::UV]) : 0,
                 !ui_.has(UiState::UV));
        draw_solar(ui_.has(UiState::SOLAR) ? ui_.v[UiState::SOLAR] : 0.0f,
                   !ui_.has(UiState::SOLAR));
    }

    // Low-battery state of whichever outdoor sensor supplies `f1` (or `f2`): any recently
    // heard, non-built-in roster entry carrying that field with its battery_low flag set.
    // Stock reads one fixed sensor slot per zone; the roster search is our equivalent.
    bool field_batt_low(uint8_t f1, uint8_t f2 = 0xff) const {
        const bool rain = f1 == UiState::RAIN_RATE || f1 == UiState::RAIN;
        const uint32_t key = rain ? primary_rain_key_ : primary_array_key_;
        const uint32_t now = millis();
        for (size_t i = 0; i < roster_count_; i++) {
            const auto &e = roster_[i];
            const uint32_t ekey = ((uint32_t)e.family << 24) | (e.id & 0xffffff);
            if (ekey != key || !e.last_ms || now - e.last_ms > ROSTER_SEEN_WINDOW_MS ||
                !family_enabled(e.family) || is_disabled((uint8_t)e.family, e.id)) continue;
            if (e.v.has(f1) || (f2 != 0xff && e.v.has(f2))) return e.battery_low;
        }
        return false;
    }

    void render_normal(bool blink) {
        refresh_ext_chans();
        uint8_t sel = panel_.temp_sel;
        if (sel >= Panel::TEMP_MAIN_N + ext_chan_n_) sel = 0;   // roster shrank under us
        bool on_ext = sel >= Panel::TEMP_MAIN_N;
        int16_t ext = -1;
        uint8_t ext_ch = 0;
        if (on_ext) {
            ext_ch = ext_chans_[sel - Panel::TEMP_MAIN_N];
            ext = ext_chan_slot(ext_ch);
            if (ext < 0) { on_ext = false; sel = 0; }
        }
        seg(fb_, SEG_OUT_ROTATE, panel_.temp_auto);
        seg(fb_, SEG_OUT_LABEL, !on_ext);             // "OUTDOOR" is the main array only
        seg(fb_, SEG_OUT_FEELS, !on_ext && sel == 1);
        seg(fb_, SEG_OUT_DEW, !on_ext && sel == 2);
        seg(fb_, SEG_OUT_FLAG, on_ext);
        draw_small_glyph(fb_, OT_CHAN, on_ext ? ext_ch : SM_BLANK);
        seg(fb_, SEG_OUT_BATT, on_ext ? roster_[ext].battery_low
                                      : field_batt_low(UiState::T_OUT, UiState::H_OUT));
        {
            const UiState &src = on_ext ? roster_[ext].v : ui_;
            bool have_t = src.has(UiState::T_OUT) || (on_ext && src.has(UiState::T_IN));
            bool have_h = src.has(UiState::H_OUT) || src.has(UiState::H_IN);
            // A WH31 reports through the outdoor T/H slots; fall back to T_IN for decoders
            // that file a channel sensor as an indoor pair.
            float t = src.has(UiState::T_OUT) ? src.v[UiState::T_OUT]
                    : src.has(UiState::T_IN) ? src.v[UiState::T_IN] : 0;
            float rh = src.has(UiState::H_OUT) ? src.v[UiState::H_OUT]
                     : src.has(UiState::H_IN) ? src.v[UiState::H_IN] : 0;
            float w = ui_.has(UiState::WSPD) ? ui_.v[UiState::WSPD] : 0;
            float v = t;
            bool need_h = false;   // the variant is undefined without a humidity
            if (!on_ext) {
                switch (sel) {
                    case 1: v = feels_like_c(t, rh, w); need_h = t > 26.7f; break;
                    case 2: v = dew_point_c(t, rh); need_h = true; break;
                    default: break;
                }
            }
            // No humidity -> dashes rather than a dew point / heat index invented from an
            // assumed 50 %.
            bool missing = !have_t || (need_h && !have_h) || std::isnan(v);
            int32_t v10 = 0;
            if (!missing) fmt_x10(disp_temp(v), v10);
            draw_outdoor_temp(v10, missing);
            draw_outdoor_hum((int32_t)lroundf(rh), !have_h);
        }

        seg(fb_, SEG_CHAN_TICK, true);
        seg(fb_, SEG_CHAN_ARROW, false);   // existing CH label/number are sufficient; physical mapping verified

        {
            int32_t v10 = 0;
            bool missing = !ui_.has(UiState::T_IN);
            if (!missing) fmt_x10(disp_temp(ui_.v[UiState::T_IN]), v10);
            draw_indoor_temp(v10, missing);
        }
        draw_indoor_hum((int32_t)lroundf(ui_.v[UiState::H_IN]), !ui_.has(UiState::H_IN));

        {
            bool rel = pset_.press_abs == 0;
            draw_pressure(rel ? press_rel_hpa() : press_abs_hpa(), !ui_.has(UiState::P), rel);
        }
        draw_time(blink);
        draw_batt_channel();   // + the bottom-left DATE row (stock tail-calls it)
        draw_moon();
        draw_pressure_trend();
        draw_forecast(blink);  // stock draws it from the same routine as the trend pointer
        draw_rot_zone();       // UVI + SOLAR, or sunrise + sunset
        draw_wind(panel_.wind_sel);
        draw_rf_strip(ext);   // stock shows the SELECTED sensor's link, not the array's
        draw_rf_bars(SEG_RF_WIND_ANT, RF_WIND_BARS, outdoor_age_s());   // wind header
        draw_rf_bars(SEG_RF_RAIN_ANT, RF_RAIN_BARS, source_age_s(primary_rain_key_));   // rain header
        seg(fb_, SEG_RAIN_BATT, field_batt_low(UiState::RAIN_RATE, UiState::RAIN));
        draw_wifi_icon();
        {
            float mm;
            bool missing;
            rain_live(panel_.rain_event, mm, missing);
            draw_rain_rate(panel_.rain_event, mm, missing);   // + the rain bar (tail call)
        }
        draw_rain_accum(panel_.rain_range);
    }

    // MAX/MIN show observed records; a field remains dashed until its first sample.
    void render_maxmin(bool maximum) {
        using H = DisplayHistory;
        big_text(fb_, maximum ? ROWTXT_MAX : ROWTXT_MIN);
        const uint8_t sel = maximum ? panel_.max_sel : panel_.min_sel;
        const H::Field tf = sel == 1 ? H::FEELS : sel == 2 ? H::DEW : H::TEMP;
        const auto have = [this, maximum](H::Field f) { return history_.has(f, maximum); };
        const auto value = [this, maximum](H::Field f) { return history_.get(f, maximum); };
        seg(fb_, SEG_OUT_LABEL, true);
        seg(fb_, SEG_OUT_FEELS, sel == 1);
        seg(fb_, SEG_OUT_DEW, sel == 2);
        draw_outdoor_temp(have(tf) ? (int32_t)lroundf(disp_temp(value(tf)) * 10) : 0, !have(tf));
        draw_outdoor_hum(have(H::HUM) ? (int32_t)lroundf(value(H::HUM)) : 0, !have(H::HUM));
        seg(fb_, SEG_CHAN_TICK, true);
        draw_indoor_temp(have(H::IN_TEMP) ? (int32_t)lroundf(disp_temp(value(H::IN_TEMP)) * 10) : 0,
                         !have(H::IN_TEMP));
        draw_indoor_hum(have(H::IN_HUM) ? (int32_t)lroundf(value(H::IN_HUM)) : 0, !have(H::IN_HUM));
        const bool rel = pset_.press_abs == 0;
        draw_pressure(value(H::PRESS) + (rel ? rel_off_hpa_ : 0.0f), !have(H::PRESS), rel);
        if (!maximum) return;
        const H::Field wf = panel_.mm_gust ? H::GUST : H::WIND;
        seg(fb_, SEG_WIND_UNITS, true);
        seg(fb_, SEG_WIND_GUST, panel_.mm_gust);
        seg(fb_, SEG_WIND_MS, pset_.unit_wind == 0);
        seg(fb_, SEG_WIND_KMH, pset_.unit_wind == 1);
        seg(fb_, SEG_WIND_MPH, pset_.unit_wind == 2);
        seg(fb_, SEG_WIND_KNOTS, pset_.unit_wind == 3);
        seg(fb_, SEG_WIND_BFT, pset_.unit_wind == 5);
        wind_digits(value(wf), !have(wf));
        draw_rain_rate(false, value(H::RAIN_RATE), !have(H::RAIN_RATE));
        seg(fb_, SEG_ROT_UV_A, true);
        seg(fb_, SEG_ROT_UV_B, true);
        seg(fb_, SEG_ROT_UV_C, true);
        draw_uvi((int32_t)lroundf(value(H::UV)), !have(H::UV));
        draw_solar(value(H::SOLAR), !have(H::SOLAR));
    }

    // Render alarm time on the clock digits; blink the selected hour/minute field.
    void render_alarm() {
        big_text(fb_, ROWTXT_ALM);
        bool vis = blink_vis();
        draw_clock_digits(pset_.alarm_h, pset_.alarm_m, panel_.alarm_sub != 0 || vis,
                          panel_.alarm_sub != 1 || vis);
        seg(fb_, SEG_TIME_COLON, true);
        seg(fb_, SEG_TIME_BELL, pset_.alarm_on != 0);
    }

    // MAC address nibbles occupy non-overlapping outdoor, indoor and pressure digits.
    void render_info() {
        big_text(fb_, ROWTXT_MAC);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        const uint16_t *const POS[12] = {SM_DIGITS[3], OT_TENS, OH_TENS, OH_UNITS,
                                         SM_DIGITS[0], SM_DIGITS[1], D340, D350,
                                         AL_DIGITS[0], AL_DIGITS[1], AL_DIGITS[2], AL_DIGITS[3]};
        for (uint8_t i = 0; i < 12; i++)
            draw_small_glyph(fb_, POS[i], (i & 1) ? (mac[i / 2] & 0x0F) : (mac[i / 2] >> 4));
    }

    void draw_on_off(bool on, bool vis) {
        uint8_t a = SM_BLANK, b = SM_BLANK, c = SM_BLANK;
        if (vis) {
            if (on) { b = SF_O; c = SF_n; } else { a = SF_O; b = SF_F; c = SF_F; }
        }
        draw_small_glyph(fb_, TA, a);
        draw_small_glyph(fb_, TB, b);
        draw_small_glyph(fb_, TC, c);
    }

    // Four small-font characters on the pressure digits AL0..AL3 (setting captions).
    void draw_al_text(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {
        draw_small_glyph(fb_, AL_DIGITS[0], c0);
        draw_small_glyph(fb_, AL_DIGITS[1], c1);
        draw_small_glyph(fb_, AL_DIGITS[2], c2);
        draw_small_glyph(fb_, AL_DIGITS[3], c3);
    }

    void draw_date_fields(const ESPTime &t, bool show_month, bool show_day) {
        seg(fb_, SEG_ROW_DATE, true);   // the steps OR 0x0502 into the framebuffer
        uint8_t mi = (uint8_t)((t.month >= 1 && t.month <= 12) ? t.month - 1 : 0);
        big_text(fb_, show_month ? MONTH3[mi] : "   ");
        uint8_t b[6];
        if (show_day) {
            fmt_pair(t.day_of_month, b);
        } else {
            fmt_blank(b);
        }
        draw_small_glyph(fb_, Z0, b[1]);
        draw_small_glyph(fb_, Z1, b[0]);
    }

    // Each settings step highlights its corresponding display zone.
    // Clock/date values are read-only here because SNTP owns time.
    void render_setting() {
        uint8_t st = panel_.setting_step;
        bool vis = blink_vis();
        switch (st) {
            case 0:
                draw_al_text(SF_b, SF_E, SF_E, SF_P);
                draw_on_off(pset_.beep != 0, vis);
                break;
            case 1:
                big_text(fb_, ROWTXT_RST);
                draw_al_text(SF_H, SF_I, SF_L, SF_o);
                draw_on_off(pset_.rst_hilo != 0, vis);
                break;
            case 2: {
                uint8_t b[6];
                if (vis) {
                    fmt_pair(pset_.h12 ? 12 : 24, b);
                } else {
                    fmt_blank(b);
                }
                seg(fb_, SEG_TIME_T2, b[1] == 2);
                seg(fb_, SEG_TIME_T12, b[1] == 1 || b[1] == 2);
                seg(fb_, SEG_TIME_T1, b[1] == 1);
                draw_small_glyph(fb_, TA, b[0]);
                draw_small_glyph(fb_, TB, SM_BLANK);
                draw_small_glyph(fb_, TC, SF_H);
                break;
            }
            case 3: case 4: case 5: case 6: case 7: {
                ESPTime t{};
                bool ok = time_clock != nullptr && (t = time_clock->now()).is_valid();
                if (!ok) {
                    draw_small_glyph(fb_, TA, SM_MINUS);
                    draw_small_glyph(fb_, TB, SM_MINUS);
                    draw_small_glyph(fb_, TC, SM_MINUS);
                    big_text(fb_, "---");
                    break;
                }
                seg(fb_, SEG_TIME_DST, t.is_dst);
                if (st <= 4) {
                    seg(fb_, SEG_TIME_COLON, blink_on_);
                    draw_clock_digits(t.hour, t.minute, st != 3 || vis, st != 4 || vis);
                } else {
                    draw_clock_year(t.year, st != 5 || vis);
                }
                draw_date_fields(t, st != 6 || vis, st != 7 || vis);
                break;
            }
            case 8:
                draw_pressure(press_rel_hpa(), !ui_.has(UiState::P), true, vis, true);
                break;
            case 9:
                draw_pressure(press_rel_hpa(), !ui_.has(UiState::P), true, true, vis);
                break;
            case 10: {
                seg(fb_, SEG_CHAN_TICK, true);
                seg(fb_, SEG_OUT_LABEL, true);
                uint8_t u = pset_.unit_temp ? SF_F : SF_C;
                uint8_t d = vis ? SF_DEG : SM_BLANK, c = vis ? u : SM_BLANK;
                draw_small_glyph(fb_, SM_DIGITS[3], d);
                draw_small_glyph(fb_, OT_TENS, c);
                draw_small_glyph(fb_, SM_DIGITS[0], d);
                draw_small_glyph(fb_, SM_DIGITS[1], c);
                break;
            }
            case 11:
                draw_wind(0, vis);
                break;
            case 12: {
                float mm;
                bool missing;
                rain_live(true, mm, missing);
                draw_rain_rate(true, mm, missing, vis);
                draw_rain_accum(3);
                break;
            }
            case 13:
                seg(fb_, SEG_ROT_UV_B, true);
                draw_solar(ui_.has(UiState::SOLAR) ? ui_.v[UiState::SOLAR] : 0.0f,
                           !ui_.has(UiState::SOLAR), vis);
                break;
            case 14:
                big_text(fb_, pset_.hemisphere ? ROWTXT_NTH : ROWTXT_STH);
                draw_moon(vis);
                break;
            default:
                big_text(fb_, ROWTXT_CAL);
                seg(fb_, SEG_ROW_CO2, true);
                draw_small_glyph(fb_, AL_DIGITS[0], SM_MINUS);
                draw_small_glyph(fb_, AL_DIGITS[1], SM_MINUS);
                draw_small_glyph(fb_, AL_DIGITS[2], SM_MINUS);
                break;
        }
    }

    // Bench diagnostic: pin the framebuffer to an explicit pattern so a single zone can be
    // lit on its own and photographed. The segment walk answers the same question but takes
    // 4.5 min and one segment at a time, which is below the camera's noise floor in the
    // washed-out top half of the glass; lighting a whole digit group at once is ~20x the
    // signal. Set through POST /config {"panel":{"testseg":"0x2b04,..."}} or
    // {"panel":{"testfb":"<128 hex>"}}; an empty string releases it. Never persisted.
    uint8_t test_fb_[64] = {0};
    bool test_fb_on_ = false;

    void render_panel() {
        render_phase_ = millis() / 500;
        blink_on_ = (render_phase_ & 1) == 0;
        if (!gpl_ok_)
            return;
        if (test_fb_on_) {
            memcpy(fb_, test_fb_, sizeof(fb_));
            last_render_ms_ = millis();
            ecowitt_rf::gpl_push(fb_);
            return;
        }
        memset(fb_, 0, sizeof(fb_));
        last_render_ms_ = millis();
        switch (panel_.mode) {
            case DMode::NORMAL:  render_normal(blink_on_); break;
            case DMode::MAX:     render_maxmin(true); break;
            case DMode::MIN:     render_maxmin(false); break;
            case DMode::ALARM:   render_alarm(); break;
            case DMode::INFO:    render_info(); break;
            case DMode::SETTING: render_setting(); break;
            default: break;
        }
        ecowitt_rf::gpl_push(fb_);
    }
    uint32_t render_phase_ = 0;
    // True once the 0.5 s blink phase has moved on since the last paint: the colon, the
    // storm forecast, a ringing bell and every setting/alarm field blink on it. A paint is
    // a 64-byte I2C burst, so 2 Hz costs nothing.
    bool render_due() const { return last_render_ms_ == 0 || millis() / 500 != render_phase_; }

    // ---- segment-walk diagnostic (owns the panel while walk_active_) ----
    // One segment lit at a time, in framebuffer order: step i = byte i>>3, bit 1<<(i&7),
    // i.e. the note's segment id (byte<<8)|bit. Log one line per step so the video frame
    // counter can be cross-checked against the log.
    void walk_show(int step) {
        uint8_t byte = (uint8_t)(step >> 3);
        uint8_t bit = (uint8_t)(1u << (step & 7));
        memset(fb_, 0, sizeof(fb_));
        fb_[byte] = bit;
        ecowitt_rf::gpl_push(fb_);
        ESP_LOGI(TAG, "WALK i=%d seg=%02x.%02x", step, byte, bit);
    }

    void walk_tick() {
        uint32_t el = millis() - walk_base_ms_;
        int want;
        if (el < 2000)
            want = -2;  // all-on sync marker
        else if (el < 4000)
            want = -1;  // all-off sync marker
        else
            want = (int)((el - 4000) / 500);   // step 0..511 (>=512 -> finished)

        if (want >= 512) {
            walk_active_ = false;
            memset(fb_, 0, sizeof(fb_));
            ecowitt_rf::gpl_push(fb_);
            if (segment_walk_switch_)
                segment_walk_switch_->publish_state(false);
            last_render_ms_ = 0;  // force the next loop to render the normal page
            ESP_LOGI(TAG, "WALK DONE");
            return;
        }
        if (want != walk_shown_) {
            walk_shown_ = want;
            if (want < 0) {
                memset(fb_, want == -2 ? 0xff : 0x00, sizeof(fb_));
                ecowitt_rf::gpl_push(fb_);
            } else {
                walk_show(want);
            }
        }
    }

    // ---- panel event side effects (called from loop) ----
    void handle_panel_event(const PanelEvent &ev) {
        if (alarm_ringing_) {
            alarm_ringing_ = false;
            gpl_buzzer(false);
            buzzer_on_ = false;
            if (ev.btn == Btn::LIGHT && ev.kind == EvKind::SHORT) {
                alarm_snooze_until_ = millis() + 600000UL;
                ESP_LOGI(TAG, "alarm snoozed 10 min");
                return;  // LIGHT consumed by snooze
            }
            ESP_LOGI(TAG, "alarm silenced by key");
            return;  // Stock: key consumed by silencing alarm (no mode jump)
        } else if (ev.kind == EvKind::LONG && alarm_snooze_until_ > millis()) {
            alarm_snooze_until_ = 0;  // hold any button 2 s while snoozed -> exit snooze
            ESP_LOGI(TAG, "alarm snooze cancelled");
            return;  // Consumed by snooze cancel
        }
        panel_.handle(ev);
        if (panel_.fx_beep && pset_.beep) {
            gpl_buzzer(true);
            buzzer_on_ = true;
            beep_off_ms_ = millis() + 60;
        }
    }

    void apply_panel_fx() {
        if (panel_.fx_reset_max) reset_maxmin(true);
        if (panel_.fx_reset_min) reset_maxmin(false);
        if (panel_.fx_backlight) gpl_backlight_level(pset_.backlight);
        if (panel_.fx_settings) save_panel();
        if (panel_.fx_rel) apply_rel_steps();
        if (panel_.fx_reregister) {
            ESP_LOGW(TAG, "TEMP hold (2s/5s): RF sensor search/re-register requested (roster auto-learns; log-only stub)");
        }
        if (panel_.fx_wifi_hotspot) {
            ESP_LOGW(TAG, "TEMP+- chord (2s): Wi-Fi hotspot / config mode requested");
#ifdef USE_CAPTIVE_PORTAL
            if (captive_portal::global_captive_portal != nullptr) {
                ESP_LOGI(TAG, "Starting captive portal AP for Wi-Fi config");
                captive_portal::global_captive_portal->start();
            }
#endif
        }
        if (panel_.fx_factory_reset) {
            // Guardrail: the SET+LIGHT 5 s factory-reset chord MUST stay a LOG-ONLY STUB.
            // DO NOT wipe panel settings, records, or flash preferences. On custom ESPHome
            // firmware a wipe of settings or NVS could sever Wi-Fi/HA connectivity or lose
            // calibration, so this is logged only.
            ESP_LOGW(TAG, "SET+LIGHT 5s chord: Factory reset requested (log-only stub per safety guardrails; no config wipe)");
        }
    }

    // Buzzer housekeeping: end the 60 ms key beep; alarm cadence (2 Hz toggle, 120 s).
    void buzzer_tick() {
        uint32_t now = millis();
        if (buzzer_on_ && !alarm_ringing_ && (int32_t)(now - beep_off_ms_) >= 0) {
            gpl_buzzer(false);
            buzzer_on_ = false;
        }
        if (alarm_ringing_) {
            if (now - alarm_ring_ms_ > 120000UL) {  // 120 s ring timeout (stock)
                alarm_ringing_ = false;
                gpl_buzzer(false);
                buzzer_on_ = false;
                return;
            }
            if (now - last_beep_ms_ >= 250) {  // 2 Hz; stock escalates 1->2->4->cont.
                last_beep_ms_ = now;
                buzzer_on_ = !buzzer_on_;
                gpl_buzzer(buzzer_on_);
            }
        }
    }

    // Clock alarm trigger: armed && HH:MM match && not snoozed && not already ringing.
    void alarm_tick() {
        if (!pset_.alarm_on || alarm_ringing_ || time_clock == nullptr)
            return;
        uint32_t now = millis();
        if (now < alarm_snooze_until_)
            return;
        ESPTime t = time_clock->now();
        if (!t.is_valid() || t.hour != pset_.alarm_h || t.minute != pset_.alarm_m)
            return;
        if (now - alarm_ring_ms_ < 90000UL)  // once per minute at most
            return;
        alarm_ringing_ = true;
        alarm_ring_ms_ = now;
        last_beep_ms_ = 0;
        ESP_LOGI(TAG, "clock alarm ringing (%02d:%02d)", pset_.alarm_h, pset_.alarm_m);
    }

    // Console-local UI settings (units/backlight/clock alarm) + MAX/MIN records.
    void load_panel() {
        PanelSettings ps;
        if (panel_pref_.load(&ps))
            pset_ = ps;
        // Byte 9 used to be an unused rain-season month (default 1); it is now the moon
        // hemisphere, 1 = north. Fold any old month value onto north.
        if (pset_.hemisphere > 1) pset_.hemisphere = 1;
        if (pset_.unit_wind == 4 || pset_.unit_wind > 5) pset_.unit_wind = 0;
        float off = 0.0f;
        if (rel_pref_.load(&off) && std::isfinite(off) && off > -500.0f && off < 500.0f)
            rel_off_hpa_ = off;
            upload.rel_offset_hpa = off;
        panel_.cfg = &pset_;
    }
    void save_panel() {
        panel_pref_.save(&pset_);
        global_preferences->sync();
    }
    void load_maxmin() {
        MaxMin m;
        if (maxmin_pref_.load(&m)) mm_ = m;
        DisplayHistory h;
        if (history_pref_.load(&h) && h.valid()) history_ = h;
        const float legacy[6] = {mm_.t_max, mm_.t_min, mm_.h_max, mm_.h_min, mm_.gust_max, mm_.rrate_max};
        if (history_.legacy_signature != DisplayHistory::signature(legacy, mm_.valid)) {
            history_.import_legacy(legacy, mm_.valid);
            history_dirty_ = true;
        }
    }
    void save_maxmin() {
        maxmin_pref_.save(&mm_);
        global_preferences->sync();
    }

    void setup() override {
        pinMode(ecowitt_rf::PIN_DOUT, INPUT);
        setup_prefs();
        load_cfg();
        load_servers();
        rain_.set_gain(cfg_.cal_rain);   // BEFORE load_rain: restored counters use this gain
        load_rain();
        if (rain_.count() > 0) {
            // The restored bucket's sensor stays primary for PRIMARY_STALE_MS after boot, so
            // another rain sensor heard first cannot take over the persisted totals.
            primary_rain_key_ = rain_.buckets()[0].key;
            primary_rain_ms_ = millis();
        }
        load_disabled();
        load_panel();
        load_maxmin();
        apply_cfg();
        // Push the persisted config back to the 13 template switches (they use
        // restore_mode: DISABLED, so this component is their source of truth).
        if (upload_switch_)
            upload_switch_->publish_state(cfg_.upload_enabled);
        for (uint8_t g = 0; g < 12; g++)
            if (family_switches_[g])
                family_switches_[g]->publish_state((cfg_.family_enable & (1u << g)) != 0);
        ecowitt_rf::init(device_type);
        indoor_.set_bus(i2c_bus_);
        indoor_.probe();
        register_web();
        // GPL811x display init (blank the panel; the stock's LCD bus shares SCLK/DIN
        // with the RF chip, so both CS lines stay high when idle and never overlap here).
        gpl_ok_ = ecowitt_rf::gpl_init();
        gpl_scratch_ = ecowitt_rf::gpl_dbg_scratch();
        ESP_LOGW(TAG, "GPL811x ready-check: %s (scratch readback 0x%02x, want 0x5a)",
                 gpl_ok_ ? "OK" : "FAIL", gpl_scratch_);
        if (gpl_ok_) {
            ecowitt_rf::gpl_render(fb_, "888");  // all digit segments lit — display smoke test
            ecowitt_rf::gpl_push(fb_);
            ecowitt_rf::gpl_backlight_init();
            ecowitt_rf::gpl_backlight_level(pset_.backlight);  // stock levels 0..4 (duty 8/4/2/1/0)
        }
    }

    void on_shutdown() override { save_rain(); save_display_history(); }

    void loop() override {
        register_web();  // retry until the shared HTTP server (captive_portal) is up
        drain_status_request();
        drain_config_queue();  // apply /config POSTs here, never in the web-server task

        // GPL811x key scan + panel state machine (stock key_lcd task, lcd_type 4). Keys
        // are read from the LCD chip, not GPIOs — see gpl811x.h. Events: short on
        // release, long at 2 s, extra-long at 5 s, 8.3 Hz +/- repeat (panel.h).
        if (gpl_ok_) {
            if (walk_active_) {
                // Segment-walk diagnostic owns the panel: fixed cadence, no keys/render.
                walk_tick();
            } else {
                panel_.clear_fx();
                PanelEvent ev;
                while (panel_.event(ev))
                    handle_panel_event(ev);
                // One HA-injected key per pass, through the identical handler, so a burst
                // walks the state machine step by step instead of collapsing into one.
                if (key_q_len_ > 0) {
                    PanelEvent iev = key_q_[key_q_head_];
                    key_q_head_ = (uint8_t)((key_q_head_ + 1) % KEY_Q_MAX);
                    key_q_len_--;
                    handle_panel_event(iev);
                }
                panel_.tick();
                apply_panel_fx();
                buzzer_tick();
                alarm_tick();
                // Immediate refresh on any panel activity; otherwise repaint on every 0.5 s
                // blink-phase change (colon, alarm bell, storm icon, setting fields).
                if (panel_.fx_redraw || render_due())
                    render_panel();
                if (panel_.fx_redraw)
                    publish_panel_mode();
            }
        }

        // Fast RX poll — mirrors the stock's ~1 ms GPIO15 loop. The CMT2219A data-ready is
        // a per-byte FIFO_WBYTE pulse, so the old 16 s update() poll could never catch it.
        // Drain immediately on data-ready, then decode/publish/upload.
        if (ecowitt_rf::rx_ready())
            gpio0_hits_++;  // DEBUG: count data-ready assertions (diag → /state)
        uint8_t packet[69];
        size_t n = receive_packet(packet, sizeof(packet));
        if (n == 0)
            return;

        char hx[3];
        last_hex_.clear();
        for (size_t i = 0; i < n && i < 24; ++i) {
            std::snprintf(hx, sizeof(hx), "%02X", packet[i]);
            last_hex_ += hx;
        }

        ecowitt_rf::Reading r;
        bool dec_ok = ecowitt_rf::decode(packet, n, r);
        if (!dec_ok) {
            if (last_rssi_ >= RSSI_FLOOR_DBM) {
                ESP_LOGI(TAG, "FO RX DROP [%d dBm] type=0x%02X hex=%s",
                         (int)last_rssi_, packet[0], last_hex_.c_str());
            }
            return;
        }
        r.rssi_dbm = last_rssi_;

        // ★ RSSI FLOOR — the cheapest and most effective anti-phantom defence. On the
        // gw1100 the real sensors sat around -77 dBm while all 21 phantom "WH25/WH32"
        // units that noise minted over 2.2 days sat at -109..-114 dBm, i.e. on the
        // receiver's noise floor. Rejecting sub-threshold frames HERE, before
        // resolve_sensor(), also stops them consuming a persistent sensor slot.
        if (r.rssi_dbm != 0 && r.rssi_dbm < RSSI_FLOOR_DBM) {
            rejected_rssi_++;
            return;
        }

        ESP_LOGI(TAG, "FO RX OK [%d dBm] fam=%s id=0x%06X ch=%u temp=%.1f hum=%d p=%.1f hex=%s",
                 (int)r.rssi_dbm, ecowitt_rf::family_name(r.family),
                 (unsigned)r.id, (unsigned)r.channel,
                 r.has(F_TEMP) ? r.temp_c : -999.0f,
                 r.has(F_HUM) ? r.humidity : -1,
                 r.has(F_PRESS) ? r.pressure_hpa : -999.0f,
                 last_hex_.c_str());

        bool sen_enabled = resolve_sensor(r);   // per-sensor enable + assigned channel
        // The rain aggregator takes the RAW counter and applies the gain to each delta;
        // apply_calibration() then scales r.rain_mm (the reported lifetime total) only.
        const float raw_rain_mm = r.rain_mm;
        apply_calibration(r);

        bool was_seen = false;
        int16_t ridx = record_roster(r, was_seen);
        const uint32_t now_ms = millis();
        const uint32_t key = ((uint32_t)r.family << 24) | (r.id & 0xFFFFFF);
        // Accepted = enabled (family + sensor) and a repeat sighting. Only accepted readings
        // may claim/drive the primary array and rain state below.
        const bool accepted = family_enabled(r.family) && sen_enabled && was_seen;
        const bool array_primary = accepted && is_primary_array(r.family) &&
                                   claim_primary(primary_array_key_, primary_array_ms_, key, now_ms);
        const bool rain_primary = accepted && is_rain_family(r.family) &&
                                  claim_primary(primary_rain_key_, primary_rain_ms_, key, now_ms);

        // Fold rain-capable counters into the running aggregate (sampled for every heard
        // sensor so the delta baseline is always current, even for a disabled one). Only the
        // primary rain sensor's bucket is persisted and shown on the LCD (bucket 0).
        if (is_rain_family(r.family)) {
            RainAggregate agg;
            uint16_t doy = 0; uint8_t hour = 0, week = 0, month = 0; uint16_t year = 0;
            uint32_t now_unix = 0;
            if (time_clock) {
                ESPTime t = time_clock->now();
                if (t.is_valid()) {
                    doy = t.day_of_year; hour = t.hour; month = t.month; year = t.year;
                    week = ecowitt_rf::rain_week_key(year, doy);
                    now_unix = (uint32_t)t.timestamp;
                }
            }
            // A genuinely new primary after the stale grace may replace the saved source.
            if (rain_primary && rain_restore_pending_ && key != rain_checkpoint_.key)
                rain_restore_pending_ = false;
            bool waiting = rain_restore_pending_ && key == rain_checkpoint_.key;
            if (waiting && now_unix >= rain_checkpoint_.unix_time) {
                rain_restored_ = rain_checkpoint_.restore(rain_.buckets()[0], now_unix, now_ms);
                if (rain_restored_) { rain_restore_pending_ = false; waiting = false; }
            }
            if (waiting) {
                // Do not advance saved baselines or publish a false dry/rate reading before NTP.
                const auto &b = rain_.buckets()[0];
                agg.event_mm = b.event_mm; agg.hour_mm = b.hour_mm; agg.day_mm = b.day_mm;
                agg.week_mm = b.week_mm; agg.month_mm = b.month_mm; agg.year_mm = b.year_mm;
                agg.last24h_mm = agg.last1h_mm = agg.rate_mmh = NAN;
            } else {
                rain_.sample((uint8_t)r.family, r.id, raw_rain_mm, doy, hour, week, month, year,
                             now_ms, agg, now_unix);
            }
            r.rain_event_mm = agg.event_mm; r.rain_hour_mm = agg.hour_mm; r.rain_day_mm = agg.day_mm;
            r.rain_week_mm = agg.week_mm; r.rain_month_mm = agg.month_mm; r.rain_year_mm = agg.year_mm;
            r.rain_rate_mmh = agg.rate_mmh;
            r.last24h_rain_mm = agg.last24h_mm;
            r.last1h_rain_mm = agg.last1h_mm;
            bool dirty = rain_.take_dirty();
            bool rolled = rain_.take_rolled();
            if (rain_primary) {
                bool promoted = rain_.promote(key);
                if (promoted || rolled || (dirty && now_ms - rain_save_ms_ >= RAIN_SAVE_MS))
                    save_rain();
                else if (dirty)
                    rain_save_pending_ = true;   // flushed from update() once RAIN_SAVE_MS passes
            }
        }

        apply_windows(r, array_primary);
        track_maxmin(r, array_primary, rain_primary);   // all-time MAX/MIN (panel modes 2/3)
        if (ridx >= 0)
            map_reading(r, roster_[ridx].v);   // after windows so the card carries max_gust/wind_avg

        if (!family_enabled(r.family) || !sen_enabled)
            return;  // discovered but disabled — still shown in the roster
        if (!was_seen)
            return;  // first sighting — don't upload until it repeats (phantom-entity guard)

        update_ui_state(r, rain_primary, array_primary);
        track_display_history(r, array_primary, rain_primary);
        publish(r, rain_primary);   // native API + MQTT: cheap and non-blocking, so every packet goes out

        // Upload sockets stay on the worker task. Honour "Upload Interval" per sensor
        // to bound queue pressure without starving the fast RF poll.
        // The limit is PER SENSOR (roster slot): one global timestamp let whichever family
        // happened to transmit first after each interval win, starving the others.
        // Remote WH25 is NOT uploaded via the RF loop — the built-in sensor owns the indoor
        // slot in update() (GW1100 rule).
        if (r.family != Family::WH25 && ridx >= 0) {
            uint32_t &last_up = roster_[ridx].last_up_ms;
            if (last_up == 0 || (now_ms - last_up) >= (uint32_t)cfg_.interval_s * 1000UL) {
                last_up = now_ms ? now_ms : 1;
                upload_reading(r);
            }
        }
    }

    void update() override {
        // Periodic (16 s): device info + register diagnostics + roster text.
        ptrend_tick();          // hourly pressure sample for the trend pointer
        if (rain_save_pending_ && millis() - rain_save_ms_ >= RAIN_SAVE_MS)
            save_rain();        // throttled rain increments (see RAIN_SAVE_MS)
        forecast_tick();        // 10 min + hourly samples for the forecast icons
        publish_panel_mode();   // push the panel mode/page text sensor on change
        float it = 0, ih = 0, ip = 0;
        bool th_ok = indoor_.read_th(it, ih);
        bool p_ok = indoor_.read_pressure(ip);

        // Build the built-in reading FIRST, then calibrate it, then publish the CALIBRATED
        // values. This path used to skip apply_calibration() altogether, so cal_temp /
        // cal_hum / cal_press silently did nothing for the console's own I2C sensor even
        // though the config UI advertises them as "applied before publish + upload". It
        // also meant the built-in barometer and the RF sensors were offset from each other
        // by exactly cal_press. family=WH25 drives the reading_query field map (tempinc /
        // humidityin / baromrelhpa+baromabshpa); it is not an RF packet.
        Reading r;
        r.family = Family::WH25;
        if (th_ok) {
            r.temp_c = it; r.mark(F_TEMP);
            r.humidity = (int)(ih + 0.5f); r.mark(F_HUM);
        }
        if (p_ok) { r.pressure_hpa = ip; r.mark(F_PRESS); }
        apply_calibration(r);

        if (th_ok) {
            if (temp) temp->publish_state(r.temp_c);
            if (humidity) humidity->publish_state(r.humidity);
            ui_.set(UiState::T_IN, r.temp_c);   // built-in indoor sensor -> dashboard
            ui_.set(UiState::H_IN, r.humidity);
        }
        if (p_ok) {
            if (pressure) pressure->publish_state(r.pressure_hpa);
            ui_.set(UiState::P, r.pressure_hpa);
        }
        track_display_history(r, false, false, true);
        // GPL811x display: store the indoor temperature and refresh the panel.
        if (th_ok) {
            indoor_temp_ = r.temp_c;
            if (gpl_ok_)
                render_panel();
        }
        if (th_ok || p_ok) {
            record_builtin(r.temp_c, r.humidity, r.pressure_hpa);  // built-in roster card
            upload_reading(r);
        }
        publish_device_info();
        rssi_dbm_ = (int8_t)((int)read_reg(0x70) - 128);
        reg01_ = read_reg(0x01);
        reg02_ = read_reg(0x02);
        reg03_ = read_reg(0x03);
        reg04_ = read_reg(0x04);
        reg17_ = read_reg(0x17);
        reg18_ = read_reg(0x18);
        reg6d_ = read_reg(0x6D);
        reg6e_ = read_reg(0x6E);
        reg6f_ = read_reg(0x6F);
        char hx2[3];
        regdump_.clear();
        for (uint8_t i = 0; i < 0x60; ++i) {
            std::snprintf(hx2, sizeof(hx2), "%02X", read_reg(i));
            regdump_ += hx2;
        }
        reg61_ = read_reg(0x61);        // CUS_MODE_STA (low nibble 0x05 = RX)
        // NOTE: no 0x3F write canary — 0x3F is SYNC_VALUE<23:16>; writing it corrupts sync.
        reg43_ = read_reg(0x43);
        reg45_ = read_reg(0x45);
        reg47_ = read_reg(0x47);
        if (gpl_ok_) {
            gpl_reg03_ = ecowitt_rf::gpl_read_reg(0x0003);
            gpl_reg13_ = ecowitt_rf::gpl_read_reg(0x0013);
        }

        // Single-sample RSSI. The old 2 s blocking scan starved the fast RX poll in
        // loop(), which is what actually drains packets from the FIFO — a WH90 bursts
        // for only ~25 ms every ~8.8 s, so a 2 s freeze every 16 s was missing packets.
        uint8_t rnow = read_reg(0x70);
        rssi_min_ = rssi_max_ = rnow;
        rssi_changes_ = 0;

        publish_roster();
    }

 protected:
    DeviceConfig cfg_;
    // Preference objects are allocated once in setup_prefs() and reused for load/save.
    ESPPreferenceObject cfg_pref_;
    ESPPreferenceObject servers_pref_;
    ESPPreferenceObject rain_windows_pref_;
    ESPPreferenceObject rain_pref_;
    ESPPreferenceObject rain_evt_pref_;  // separate so RainPersist keeps its flash layout
    ESPPreferenceObject disabled_pref_;
    ESPPreferenceObject panel_pref_;
    ESPPreferenceObject maxmin_pref_;
    ESPPreferenceObject history_pref_;
    ESPPreferenceObject srv_keys_pref_;
    ESPPreferenceObject rel_pref_;

    bool family_enabled(Family f) const {
        uint8_t g = family_group(f);
        return (cfg_.family_enable & (1u << g)) != 0;
    }

    // Gated on Reading::has() rather than on magic zeros, so a legitimate 0 °C / calm wind /
    // night-time UV is calibrated like any other reading instead of being treated as absent.
    // The humidity clamp is now only what it says it is — keeping an OFFSET inside 0..100.
    // It used to double as the last line of defence against un-sentinel-checked garbage,
    // which is why bad frames surfaced in HA as a suspiciously exact 100 %.
    void apply_calibration(Reading &r) const {
        if (r.has(F_TEMP)) r.temp_c += cfg_.cal_temp;
        if (r.has(F_HUM)) r.humidity = (int)std::clamp(r.humidity + (int)cfg_.cal_hum, 0, 100);
        if (r.has(F_PRESS)) r.pressure_hpa += cfg_.cal_press;
        // Reported lifetime total only; the aggregator applies the same gain to deltas.
        if (r.rain_mm > 0) r.rain_mm *= rain_.gain();
        if (r.has(F_WIND)) r.wind_mps *= cfg_.cal_wind;
        if (r.has(F_GUST)) r.gust_mps *= cfg_.cal_wind;
        if (r.has(F_LIGHT)) r.light_wm2 *= cfg_.cal_solar;
        if (r.has(F_UV)) r.uv = (int)std::lroundf(r.uv * cfg_.cal_uv);
    }

    // The families that ARE the station's primary outdoor array. Only these drive the shared
    // outdoor_temperature / outdoor_humidity sensors: multi-channel families (WH31) and
    // remote indoor units (WH25) publish through their own per-channel upload fields and
    // must not fight over a single entity.
    static bool is_primary_array(Family f) {
        switch (f) {
            case Family::WH24: case Family::WH65: case Family::WS69:
            case Family::WS68: case Family::WS80: case Family::WS85: case Family::WS90:
                return true;
            default:
                return false;
        }
    }

    // Derive the time-window fields: max daily gust, 10-min circular wind-direction
    // average. All RAM-only; rainfall windows live in the per-sensor aggregator.
    // Only the primary array updates the windows; any other reading just carries the
    // primary's current values (a neighbour's gust must not become our daily max).
    void apply_windows(Reading &r, bool primary) {
        if (!primary) {
            r.daily_gust_mps = daily_max_gust_;
            if (r.has(F_WDIR)) r.wind_dir_avg_deg = wind_dir_avg_;
            return;
        }
        uint16_t doy = 1;
        if (time_clock) {
            ESPTime t = time_clock->now();
            if (t.is_valid()) doy = t.day_of_year;
        }
        uint32_t now = millis();

        // Max gust since local midnight.
        if (doy != gust_day_) { gust_day_ = doy; daily_max_gust_ = 0; }
        if (r.has(F_GUST) && r.gust_mps > daily_max_gust_) daily_max_gust_ = r.gust_mps;
        r.daily_gust_mps = daily_max_gust_;

        // 10-min circular-average wind direction (vector sum over a sliding window).
        // Gated on has() — due north is 0°, which the old `> 0` test silently discarded.
        if (r.has(F_WDIR)) {
            while (dir_count_ && now - dir_buf_[dir_head_].ms > 600000UL) {
                dir_head_ = (uint8_t)((dir_head_ + 1) % 72); dir_count_--;
            }
            uint8_t tail = (uint8_t)((dir_head_ + dir_count_) % 72);
            dir_buf_[tail].deg = (uint16_t)r.wind_dir_deg;
            dir_buf_[tail].ms = now;
            if (dir_count_ < 72) dir_count_++;
            else dir_head_ = (uint8_t)((dir_head_ + 1) % 72);
            float s = 0, c = 0;
            for (uint8_t i = 0; i < dir_count_; i++) {
                float a = dir_buf_[(uint8_t)((dir_head_ + i) % 72)].deg * 0.0174532925f;
                s += sinf(a); c += cosf(a);
            }
            float avg = atan2f(s, c) * 57.295779513f;
            if (avg < 0) avg += 360.0f;
            r.wind_dir_avg_deg = (int)(avg + 0.5f) % 360;
            wind_dir_avg_ = r.wind_dir_avg_deg;
        }

    }

    // Poll GPIO15 (data-ready), burst-read the RX FIFO, re-arm. The decode dispatches on
    // buf[0] (sensor-type). Returns 0 if nothing was ready.
    size_t receive_packet(uint8_t *buf, size_t max) {
        if (!ecowitt_rf::rx_ready())
            return 0;
        // 0x70 is latched at PKT_OK; read it now (before re-arm clears the latch) so the
        // packet's actual signal strength is captured, not the stale 16 s diag value.
        last_rssi_ = (int8_t)((int)read_reg(0x70) - 128);
        size_t plen = ecowitt_rf::fifo_packet_len();  // 69 (CMT2300A) or 32 (CMT2219A)
        size_t n = (max < plen) ? max : plen;
        ecowitt_rf::fifo_read(buf, n);
        ecowitt_rf::rearm_rx();
        return n;
    }

    void publish(const ecowitt_rf::Reading &r, bool rain_primary) {
        // ★ The indoor temp/humidity/pressure sensors belong to the BUILT-IN I2C sensor and
        // are published from update(). An RF sensor must never write them. Until this split,
        // every RF packet wrote the same three Sensor objects the I2C sensor wrote, so the
        // "Indoor Humidity" entity was a 50/50 interleave of the console and the outdoor
        // array. Outdoor T/H now has its own pair of sensors, driven only by the primary
        // array.
        if (is_primary_array(r.family)) {
            if (outdoor_temp && r.has(F_TEMP))        outdoor_temp->publish_state(r.temp_c);
            if (outdoor_humidity && r.has(F_HUM))     outdoor_humidity->publish_state(r.humidity);
        }
        // Publish only the fields this family actually carried (see Reading::has()).
        if (wind_speed && r.has(F_WIND))              wind_speed->publish_state(r.wind_mps);
        if (wind_dir && r.has(F_WDIR))                wind_dir->publish_state(r.wind_dir_deg);
        if (gust && r.has(F_GUST))                    gust->publish_state(r.gust_mps);
        if (rain_primary) {   // one rain sensor drives the HA rain entities
            if (rain) rain->publish_state(r.rain_mm);
            if (rain_rate) rain_rate->publish_state(r.rain_rate_mmh);
        }
        if (uv && r.has(F_UV))                        uv->publish_state(r.uv);
        if (light && r.has(F_LIGHT))                  light->publish_state(r.light_wm2);
        if (soil_moisture && r.moisture_pct)          soil_moisture->publish_state(r.moisture_pct);
        if (soil_temp && r.soil_temp_c != 0)          soil_temp->publish_state(r.soil_temp_c);
        if (soil_ec && r.soil_ec_uscm > 0)            soil_ec->publish_state(r.soil_ec_uscm);
        if (pm25 && r.pm25_ugm3)                      pm25->publish_state(r.pm25_ugm3);
        if (pm10 && r.pm10_ugm3)                      pm10->publish_state(r.pm10_ugm3);
        if (co2 && r.co2_ppm)                         co2->publish_state(r.co2_ppm);
        if (leaf_wetness && r.leaf_wetness)           leaf_wetness->publish_state(r.leaf_wetness);
        if (lightning_strikes && r.strikes)           lightning_strikes->publish_state(r.strikes);
        if (lightning_distance && r.lightning_dist_km > 0) lightning_distance->publish_state(r.lightning_dist_km);
        if (leak)                                  leak->publish_state(r.leak ? 1.0f : 0.0f);
    }

    void upload_reading(const ecowitt_rf::Reading &r) {
        if (!cfg_.upload_enabled)
            return;
        // Primary Ecowitt server (YAML / config entity, backward compatible).
        if (!upload.host.empty()) {
            std::string q = ecowitt_rf::reading_query(upload, r);
            ecowitt_rf::http_request_async(true, upload.host, upload.port, upload.path, q, &up_code_);
            up_last_ = (uint32_t)::time(nullptr);
        }
        // Ecowitt CLOUD (the OEM upload path) — the stock GW1100 posts here so the WS View /
        // Ecowitt app sees live data. Same PASSKEY + field set as the primary, plus dateutc
        // (the cloud rejects a stale timestamp). Fixed endpoint, gated by cfg_.cloud_enabled.
        // Skipped until the clock is set: before SNTP, dateutc would read 1970 and the
        // cloud rejects it.
        time_t cloud_now = ::time(nullptr);
        if (cfg_.cloud_enabled && cloud_now > 1700000000) {
            std::string q = ecowitt_rf::reading_query(upload, r);
            ecowitt_rf::add_cloud_rain_fields(q, r);
            time_t now = cloud_now;
            struct tm *t = ::gmtime(&now);
            if (t) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "dateutc=%04d-%02d-%02d+%02d:%02d:%02d",
                              t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
                if (!q.empty()) q += '&';
                q += buf;
            }
            ecowitt_rf::http_request_async(true, "cdnrtpdate.ecowitt.net", 80, "/data/report/", q, &cloud_code_);
        }
        // Additional servers (any protocol, any count up to 4).
        for (size_t i = 0; i < extra_count_; ++i) {
            Server &s = extra_servers_[i];
            if (!s.enabled)
                continue;
            if (s.proto == Protocol::MQTT) {
                mqtt_upload_reading(s, r, i);
                continue;
            }
            if (s.host.empty())
                continue;
            if (s.proto == Protocol::WUNDERGROUND) {
                // Wunderground's host/path are fixed constants (raw-socket HTTP, so the legacy
                // rtupdate endpoint, not the HTTPS one); only the station ID (key) varies.
                std::string q = ecowitt_rf::wunderground_query(s.key, r);
                ecowitt_rf::http_request_async(false, "rtupdate.wunderground.com", 80,
                                               "/weatherstation/updateweatherstation.php", q,
                                               &extra_code_[i]);
            } else {  // ECOWITT (or unknown) — POST like the primary
                UploadConfig tmp;
                tmp.host = s.host; tmp.port = s.port; tmp.path = s.path; tmp.passkey = s.key;
                std::string q = ecowitt_rf::reading_query(tmp, r);
                ecowitt_rf::http_request_async(true, s.host, s.port, s.path, q, &extra_code_[i]);
            }
        }
    }

    // Publish one decoded reading to MQTT (the "MQTT" upload protocol). The broker is the
    // global ESPHome mqtt: component (see YAML), so the Server's host/port are ignored;
    // `path` is the topic prefix (default "gw1100"), and the full topic is
    // "<prefix>/<FAM>[/chN]". Payload is a flat JSON object with identity + every
    // non-zero measurement this family carries. extra_code_[slot]:
    //   1 = published, -4 = publish failed, -5 = broker not connected, -6 = MQTT not built.
    void mqtt_upload_reading(const Server &s, const ecowitt_rf::Reading &r, size_t slot) {
#ifdef USE_MQTT
        if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
            extra_code_[slot] = -5;
            return;
        }
        std::string topic = s.path.empty() ? std::string("gw1100") : std::string(s.path);
        topic += "/";
        topic += family_tag(r.family);
        if (r.channel > 0) {
            char cbuf[10];
            std::snprintf(cbuf, sizeof(cbuf), "/ch%d", r.channel);
            topic += cbuf;
        }
        bool ok = mqtt::global_mqtt_client->publish_json(topic, [&r](JsonObject o) {
            o["fam"] = family_tag(r.family);
            char id[8];
            std::snprintf(id, sizeof(id), "%06X", r.id & 0xFFFFFF);
            o["id"] = id;
            o["ch"] = r.channel;
            o["rssi"] = r.rssi_dbm;
            o["batt"] = r.battery_low ? 0 : 1;
            o["ts"] = (uint32_t)::time(nullptr);
            if (r.has(F_TEMP)) o["temp_c"] = r.temp_c;
            if (r.has(F_HUM)) o["humidity"] = r.humidity;
            if (r.has(F_PRESS)) o["pressure_hpa"] = r.pressure_hpa;
            if (r.has(F_WIND)) o["wind_mps"] = r.wind_mps;
            if (r.has(F_GUST)) o["gust_mps"] = r.gust_mps;
            if (r.has(F_WDIR)) o["wind_dir_deg"] = r.wind_dir_deg;
            if (is_rain_family(r.family)) {
                o["rain_mm"] = r.rain_mm;
                o["rain_rate_mmh"] = r.rain_rate_mmh;
                o["rain_event_mm"] = r.rain_event_mm;
                o["rain_hour_mm"] = r.rain_hour_mm;
                o["rain_day_mm"] = r.rain_day_mm;
                o["rain_week_mm"] = r.rain_week_mm;
                o["rain_month_mm"] = r.rain_month_mm;
                o["rain_year_mm"] = r.rain_year_mm;
                o["last24h_rain_mm"] = r.last24h_rain_mm;
            }
            if (r.has(F_UV)) o["uv"] = r.uv;
            if (r.has(F_LIGHT)) o["solar_wm2"] = r.light_wm2;
            if (r.moisture_pct) o["soil_moist_pct"] = r.moisture_pct;
            if (r.soil_temp_c != 0) o["soil_temp_c"] = r.soil_temp_c;
            if (r.soil_ec_uscm > 0) o["soil_ec_uscm"] = r.soil_ec_uscm;
            if (r.pm25_ugm3) o["pm25_ugm3"] = r.pm25_ugm3;
            if (r.pm10_ugm3) o["pm10_ugm3"] = r.pm10_ugm3;
            if (r.co2_ppm) o["co2_ppm"] = r.co2_ppm;
            if (r.leaf_wetness) o["leaf_wetness"] = r.leaf_wetness;
            if (r.strikes) o["strikes"] = r.strikes;
            if (r.lightning_dist_km > 0) o["lightning_dist_km"] = r.lightning_dist_km;
            if (r.leak) o["leak"] = 1;
        });
        extra_code_[slot] = ok ? 1 : -4;
#else
        (void)s; (void)r;
        extra_code_[slot] = -6;
#endif
    }

    // ---- persistence (ESPHome flash preferences, 512 B sector) ----
    // Config is stored as ONE blob (16 individual prefs would each carry a checksum word and
    // blow the sector). The upload-server list is a second compact blob. Per-sensor overrides
    // (name/channel/enable) are RAM-only and re-learned on boot — no flash room left.
    //
    // The preference objects are allocated ONCE (setup_prefs) and reused: ESP8266's
    // make_preference() allocates a NEW flash offset on every call (no hash cache), so calling
    // it in each load/save would read/write different offsets and silently drop data.
    void setup_prefs() {
        cfg_pref_ = global_preferences->make_preference<uint8_t[sizeof(DeviceConfig)]>(PREF_CFG, true);
        servers_pref_ = global_preferences->make_preference<uint8_t[SERVERS_BLOB]>(PREF_SERVERS, true);
        rain_pref_ = global_preferences->make_preference<uint8_t[sizeof(RainPersist)]>(PREF_RAIN, true);
        rain_windows_pref_ = global_preferences->make_preference<RainCheckpoint>(PREF_RAIN_WINDOWS, true);
        rain_evt_pref_ = global_preferences->make_preference<uint32_t>(PREF_RAIN_EVT, true);
        disabled_pref_ = global_preferences->make_preference<uint8_t[sizeof(DisabledList)]>(PREF_DISABLED, true);
        panel_pref_ = global_preferences->make_preference<uint8_t[sizeof(PanelSettings)]>(PREF_PANEL, true);
        maxmin_pref_ = global_preferences->make_preference<uint8_t[sizeof(MaxMin)]>(PREF_MAXMIN, true);
        history_pref_ = global_preferences->make_preference<DisplayHistory>(PREF_DISPLAY_HISTORY, true);
        srv_keys_pref_ = global_preferences->make_preference<ServerKeys>(PREF_SRV_KEYS, true);
        rel_pref_ = global_preferences->make_preference<float>(PREF_PANEL_REL, true);
    }

    void load_cfg() {
        uint8_t blob[sizeof(DeviceConfig)];
        if (cfg_pref_.load(&blob))
            memcpy(&cfg_, blob, sizeof(DeviceConfig));
    }

    void save_cfg() {
        uint8_t blob[sizeof(DeviceConfig)];
        memcpy(blob, &cfg_, sizeof(DeviceConfig));
        cfg_pref_.save(&blob);
        global_preferences->sync();
    }

    void load_servers() {
        uint8_t blob[SERVERS_BLOB];
        if (servers_pref_.load(&blob)) {
            extra_count_ = (blob[0] <= SERVER_MAX) ? blob[0] : 0;
            for (uint8_t i = 0; i < extra_count_; i++) {
                PersistedServer ps;
                memcpy(&ps, blob + 1 + (size_t)i * sizeof(PersistedServer), sizeof(ps));
                Server &s = extra_servers_[i];
                s.proto = (ps.proto == (uint8_t)Protocol::WUNDERGROUND) ? Protocol::WUNDERGROUND
                         : (ps.proto == (uint8_t)Protocol::MQTT) ? Protocol::MQTT : Protocol::ECOWITT;
                s.enabled = ps.enabled != 0;
                s.port = ps.port;
                s.host.assign(ps.host);
                s.path.assign(ps.path);
                s.key.assign(ps.key);
            }
            // Full-length keys (new preference). Only trusted when it still agrees with the
            // 15-char copy in the blob, i.e. both were written by the same save.
            ServerKeys sk;
            if (srv_keys_pref_.load(&sk)) {
                for (uint8_t i = 0; i < extra_count_; i++) {
                    sk.key[i][SERVER_KEY_MAX - 1] = '\0';
                    const std::string &short_key = extra_servers_[i].key;
                    if (strlen(sk.key[i]) > short_key.size() &&
                        strncmp(sk.key[i], short_key.c_str(), short_key.size()) == 0)
                        extra_servers_[i].key.assign(sk.key[i]);
                }
            }
        }
    }

    void save_servers() {
        uint8_t blob[SERVERS_BLOB];
        memset(blob, 0, sizeof(blob));
        blob[0] = extra_count_;
        for (uint8_t i = 0; i < extra_count_; i++) {
            PersistedServer ps;
            memset(&ps, 0, sizeof(ps));
            ps.proto = (uint8_t)extra_servers_[i].proto;
            ps.enabled = extra_servers_[i].enabled ? 1 : 0;
            ps.port = extra_servers_[i].port;
            strncpy(ps.host, extra_servers_[i].host.c_str(), sizeof(ps.host) - 1);
            strncpy(ps.path, extra_servers_[i].path.c_str(), sizeof(ps.path) - 1);
            strncpy(ps.key, extra_servers_[i].key.c_str(), sizeof(ps.key) - 1);
            memcpy(blob + 1 + (size_t)i * sizeof(PersistedServer), &ps, sizeof(ps));
        }
        ServerKeys sk;
        memset(&sk, 0, sizeof(sk));
        for (uint8_t i = 0; i < extra_count_; i++)
            strncpy(sk.key[i], extra_servers_[i].key.c_str(), SERVER_KEY_MAX - 1);
        srv_keys_pref_.save(&sk);
        servers_pref_.save(&blob);
        global_preferences->sync();
    }

    // Rain-aggregate state of the PRIMARY rain sensor, kept in bucket 0 (loop() promotes it
    // there). Persisted so monthly/yearly totals survive reboot — the OEM's parity gap.
    // Loaded in setup(); saved on boundary rolls at once, on rain at most every RAIN_SAVE_MS.
    // RainPersist.last_counter is the CALIBRATED counter (raw * cal_rain at save time).
    void load_rain() {
        RainPersist rp;
        const bool legacy_loaded = rain_pref_.load(&rp) && std::isfinite(rp.last_counter) && rp.last_counter >= 0;
        if (rain_windows_pref_.load(&rain_checkpoint_) && rain_checkpoint_.valid() &&
            (!legacy_loaded || rain_checkpoint_.matches_calendar(rp))) {
            rain_.set_count(1);
            rain_checkpoint_.seed(rain_.buckets()[0], rain_.gain());
            rain_restore_pending_ = true;
            rain_checkpoint_saved_ = true;
            return;
        }
        if (legacy_loaded) {
            rain_.set_count(1);
            RainAggregator::Bucket &b = rain_.buckets()[0];
            b.key = rp.key;
            b.last_counter = rp.last_counter;
            b.event_mm = rp.event_mm; b.day_mm = rp.day_mm; b.week_mm = rp.week_mm;
            b.month_mm = rp.month_mm; b.year_mm = rp.year_mm;
            b.restore_calendar(rp.mark_doy, rp.mark_month, rp.mark_year);
            uint32_t last_rain_unix = 0;
            if (rain_evt_pref_.load(&last_rain_unix))
                b.last_rain_unix = last_rain_unix;
        }
    }

    void save_rain() {
        if (rain_restore_pending_) return; // protect the saved baseline until NTP

        rain_save_pending_ = false;
        rain_save_ms_ = millis();
        if (rain_.count() == 0)
            return;
        RainAggregator::Bucket &b = rain_.buckets()[0];
        // Bucket 0 is the primary rain sensor's (promoted in loop()); never persist another
        // sensor's bucket over it (e.g. before the primary has been heard, or a full table).
        if (primary_rain_key_ != 0 && b.key != primary_rain_key_)
            return;
        RainPersist rp;
        rp.key = b.key;
        rp.last_counter = b.last_counter;
        rp.event_mm = b.event_mm; rp.day_mm = b.day_mm; rp.week_mm = b.week_mm;
        rp.month_mm = b.month_mm; rp.year_mm = b.year_mm;
        rp.mark_doy = b.mark_doy; rp.mark_month = b.mark_month; rp.mark_year = b.mark_year;
        rain_pref_.save(&rp);
        uint32_t last_rain_unix = b.last_rain_unix;
        rain_evt_pref_.save(&last_rain_unix);
        if (time_clock && time_clock->now().is_valid()) {
            auto snapshot = RainCheckpoint::capture(b, (uint32_t)time_clock->now().timestamp, millis());
            if (snapshot.valid()) rain_checkpoint_saved_ = rain_windows_pref_.save(&snapshot);
        }
        if (!global_preferences->sync()) rain_checkpoint_saved_ = false;
    }

    // Per-sensor disable list (the only user-editable override that must survive reboot).
    void load_disabled() {
        DisabledList dl;
        if (disabled_pref_.load(&dl) && dl.count <= DISABLED_MAX)
            disabled_ = dl;
    }

    bool is_disabled(uint8_t family, uint32_t id) const {
        for (uint8_t i = 0; i < disabled_.count; i++) {
            uint32_t sid = ((uint32_t)disabled_.s[i].id0 << 16) | ((uint32_t)disabled_.s[i].id1 << 8) | disabled_.s[i].id2;
            if (disabled_.s[i].family == family && sid == id)
                return true;
        }
        return false;
    }

    void set_disabled(uint8_t family, uint32_t id, bool dis) {
        // Remove any existing entry, shifting left.
        uint8_t w = 0;
        for (uint8_t i = 0; i < disabled_.count; i++) {
            uint32_t sid = ((uint32_t)disabled_.s[i].id0 << 16) | ((uint32_t)disabled_.s[i].id1 << 8) | disabled_.s[i].id2;
            if (!(disabled_.s[i].family == family && sid == id))
                disabled_.s[w++] = disabled_.s[i];
        }
        disabled_.count = w;
        // Add back if disabling.
        if (dis && disabled_.count < DISABLED_MAX) {
            disabled_.s[disabled_.count].family = family;
            disabled_.s[disabled_.count].id0 = (id >> 16) & 0xFF;
            disabled_.s[disabled_.count].id1 = (id >> 8) & 0xFF;
            disabled_.s[disabled_.count].id2 = id & 0xFF;
            disabled_.count++;
        }
        disabled_pref_.save(&disabled_);
        global_preferences->sync();
    }

    void save_sensors() {}  // kept for the setter call sites; enable is handled by set_disabled

    // Push persisted config into the live upload/poll state.
    void apply_cfg() {
        if (cfg_.host[0]) upload.host = cfg_.host;
        if (cfg_.path[0]) upload.path = cfg_.path;
        upload.port = cfg_.port;
        if (cfg_.passkey[0]) upload.passkey = cfg_.passkey;
        device_type = cfg_.band;   // 0 (433 MHz) is a valid band — don't skip it as falsy
        // PollingComponent registers its interval (start_poller) BEFORE setup() runs, and
        // set_update_interval() only stores the value — so neither the persisted interval
        // nor a runtime change ever reached the scheduler. Re-register when it changes.
        uint32_t interval_ms = (uint32_t)cfg_.interval_s * 1000;
        if (interval_ms != get_update_interval()) {
            set_update_interval(interval_ms);
            stop_poller();
            start_poller();
        }
        if (time_clock && cfg_.timezone[0]) {
            const char *posix = iana_to_posix(cfg_.timezone);
            if (posix)
                time_clock->set_timezone(posix);
        }
    }

    // ---- per-sensor config (flash-persisted: enable + channel per sensor id) ----
    SensorSlot sensor_slots_[SENSOR_MAX];
    uint8_t sensor_count_ = 0;

    int16_t find_sensor(uint8_t family, uint32_t id) const {
        for (uint8_t i = 0; i < sensor_count_; i++) {
            uint32_t sid = ((uint32_t)sensor_slots_[i].id0 << 16) | ((uint32_t)sensor_slots_[i].id1 << 8) | sensor_slots_[i].id2;
            if (sensor_slots_[i].family == family && sid == id)
                return i;
        }
        return -1;
    }

    // Ensure the sensor has a slot; apply its persisted channel; return whether enabled.
    bool resolve_sensor(Reading &r) {
        int16_t idx = find_sensor((uint8_t)r.family, r.id);
        if (idx >= 0) {
            r.channel = sensor_slots_[idx].channel;
            return sensor_slots_[idx].enabled != 0;
        }
        if (sensor_count_ < SENSOR_MAX) {
            SensorSlot &s = sensor_slots_[sensor_count_++];
            s.family = (uint8_t)r.family;
            s.id0 = (r.id >> 16) & 0xFF;
            s.id1 = (r.id >> 8) & 0xFF;
            s.id2 = r.id & 0xFF;
            s.channel = r.channel;
            s.enabled = is_disabled((uint8_t)r.family, r.id) ? 0 : 1;  // apply persisted disable
            save_sensors();
        }
        return true;
    }

    void set_sensor(uint8_t family, uint32_t id, uint8_t channel, bool enabled) {
        int16_t idx = find_sensor(family, id);
        if (idx < 0) {
            if (sensor_count_ >= SENSOR_MAX)
                return;
            idx = sensor_count_++;
            sensor_slots_[idx].family = family;
            sensor_slots_[idx].id0 = (id >> 16) & 0xFF;
            sensor_slots_[idx].id1 = (id >> 8) & 0xFF;
            sensor_slots_[idx].id2 = id & 0xFF;
        }
        sensor_slots_[idx].channel = channel;
        sensor_slots_[idx].enabled = enabled ? 1 : 0;
        set_disabled(family, id, !enabled);
        save_sensors();
    }
    void set_sensor_name(uint8_t family, uint32_t id, const char *name) {
        int16_t idx = find_sensor(family, id);
        if (idx < 0)
            return;
        strncpy(sensor_slots_[idx].name, name, sizeof(sensor_slots_[idx].name) - 1);
        sensor_slots_[idx].name[sizeof(sensor_slots_[idx].name) - 1] = 0;
        save_sensors();
    }

    // ---- learned-sensor roster (RAM-only live state, not persisted) ----
    struct RosterEntry {
        Family family = Family::Unknown;
        uint32_t id = 0;
        uint8_t channel = 0;
        int8_t rssi = 0;
        uint32_t last_ms = 0;
        bool battery_low = false;
        uint32_t last_up_ms = 0;  // last HTTP upload of this sensor (per-sensor rate limit)
        UiState v;             // this sensor's own latest readings (per-sensor, not shared)
    };
    RosterEntry roster_[32];
    size_t roster_count_ = 0;

    // Returns true if the sensor was already in the roster AND was heard RECENTLY. A
    // first-sighting false is used to skip the upload, so a one-off stray decode (noise that
    // passes the checksum) can't mint a phantom entity in HA.
    //
    // ★ The recency window matters. The WH25 ID is only 8 bits wide, so with ~20 stale
    // phantom IDs resident a fresh noise frame had roughly a 1-in-12 chance of colliding
    // with one of them and being waved through as "seen before" — which is exactly how
    // garbage temp/humidity/pressure reached HA about once a day on the gw1100. A genuine
    // sensor reports at most ~70 s apart, so anything that has been silent for an hour is
    // not a repeat sighting; it is a new frame that happens to share an ID.
    static constexpr uint32_t ROSTER_SEEN_WINDOW_MS = 3600000UL;  // 1 h

    // Returns the roster index (or -1) and sets `was_seen`. The caller maps the reading into
    // roster_[idx].v once the derived window fields are filled in.
    //
    // When all slots are taken, the entry silent for longest is evicted — but only if it has
    // been silent for more than ROSTER_SEEN_WINDOW_MS. Without eviction a roster filled by
    // noise decodes / passing sensors meant a new real sensor was never seen or uploaded.
    int16_t record_roster(const Reading &r, bool &was_seen) {
        was_seen = false;
        uint32_t now = millis();
        for (size_t i = 0; i < roster_count_; i++) {
            if (roster_[i].family == r.family && roster_[i].id == r.id) {
                uint32_t prev_ms = roster_[i].last_ms;
                roster_[i].channel = r.channel;
                roster_[i].rssi = r.rssi_dbm;
                roster_[i].last_ms = now;
                roster_[i].battery_low = r.battery_low;
                was_seen = (now - prev_ms) <= ROSTER_SEEN_WINDOW_MS;
                return (int16_t)i;
            }
        }
        size_t slot = roster_count_;
        if (slot >= sizeof(roster_) / sizeof(roster_[0])) {
            uint32_t oldest_age = ROSTER_SEEN_WINDOW_MS;
            slot = SIZE_MAX;
            for (size_t i = 0; i < roster_count_; i++) {
                if (roster_[i].family == Family::Builtin)
                    continue;
                uint32_t age = now - roster_[i].last_ms;
                if (age > oldest_age) {
                    oldest_age = age;
                    slot = i;
                }
            }
            if (slot == SIZE_MAX)
                return -1;   // full of sensors heard within the hour
            ESP_LOGI(TAG, "roster full: evicting %s id=0x%06X (silent %us)",
                     family_name(roster_[slot].family), (unsigned)roster_[slot].id,
                     (unsigned)(oldest_age / 1000));
        } else {
            roster_count_++;
        }
        RosterEntry &e = roster_[slot];
        e = RosterEntry{};
        e.family = r.family; e.id = r.id; e.channel = r.channel;
        e.rssi = r.rssi_dbm; e.last_ms = now; e.battery_low = r.battery_low;
        return (int16_t)slot;
    }

    // The gateway's own built-in indoor T/H/P (I2C, not RF) gets its own roster card so it
    // appears alongside the RF sensors. family=Builtin, id=0 (no RF id).
    void record_builtin(float temp_c, float humidity, float pressure_hpa) {
        for (size_t i = 0; i < roster_count_; i++) {
            if (roster_[i].family == Family::Builtin) {
                roster_[i].last_ms = millis();
                if (temp_c != 0) roster_[i].v.set(UiState::T_IN, temp_c);
                if (humidity) roster_[i].v.set(UiState::H_IN, humidity);
                if (pressure_hpa > 0) roster_[i].v.set(UiState::P, pressure_hpa);
                return;
            }
        }
        if (roster_count_ < sizeof(roster_) / sizeof(roster_[0])) {
            RosterEntry &e = roster_[roster_count_++];
            e.family = Family::Builtin; e.id = 0; e.channel = 0;
            e.last_ms = millis();
            if (temp_c != 0) e.v.set(UiState::T_IN, temp_c);
            if (humidity) e.v.set(UiState::H_IN, humidity);
            if (pressure_hpa > 0) e.v.set(UiState::P, pressure_hpa);
        }
    }

    void publish_roster() {
        if (!roster)
            return;
        std::string s;
        char line[96];
        uint32_t now = millis();
        for (size_t i = 0; i < roster_count_; i++) {
            uint32_t ago_s = (now - roster_[i].last_ms) / 1000;
            std::snprintf(line, sizeof(line), "%s id=0x%06X ch=%u rssi=%ddBm %us ago\n",
                          family_name(roster_[i].family), roster_[i].id,
                          roster_[i].channel, roster_[i].rssi, (unsigned)ago_s);
            s += line;
        }
        if (roster_count_ == 0)
            s = "no sensors heard yet";
        roster->publish_state(s);
    }

    void publish_device_info() {
        if (!device_info)
            return;
        char buf[160];
        char macbuf[18] = "?";
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
            snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        const char *ipstr = "";
        const char *ssidstr = "";
        int8_t rssi = 0;
        if (wifi::global_wifi_component != nullptr) {
            auto ips = wifi::global_wifi_component->wifi_sta_ip_addresses();
            static std::string ipbuf, ssidbuf;
            ipbuf = ips.empty() ? std::string() : ips[0].str();
            ipstr = ipbuf.c_str();
            ssidbuf = wifi::global_wifi_component->get_sta().get_ssid().str();  // copy before the temporary WiFiAP dies
            ssidstr = ssidbuf.c_str();
            rssi = wifi::global_wifi_component->wifi_rssi();
        }
        std::snprintf(buf, sizeof(buf), "MAC=%s IP=%s SSID=%s RSSI=%ddBm heap=%uk",
                      macbuf, ipstr, ssidstr, rssi, (unsigned)(ESP.getFreeHeap() / 1024));
        device_info->publish_state(buf);
    }
};

} // namespace ecowitt_rf
} // namespace esphome
