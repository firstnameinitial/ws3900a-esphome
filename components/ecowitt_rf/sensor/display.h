// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// WS3900A segment wiring and rendering. The physical electrode addresses were learned
// through device interoperability work; they are not a font or an executable image.
// Fonts and numeric formatting have been replaced. See THIRD_PARTY_NOTICES.md.

#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <driver/ledc.h>

#include "gpl811x.h"

namespace esphome {
namespace ecowitt_rf {

constexpr uint8_t SM_MINUS = 0x19, SM_BLANK = 0x1a;

// Small clock/date digits: each row maps segments a..g to electrode coordinates.
static const uint16_t SM_DIGITS[10][7] = {
    {0x3e04, 0x3e02, 0x2e02, 0x2604, 0x2e04, 0x3604, 0x3602},  // SM0
    {0x3e01, 0x3d80, 0x2d80, 0x2601, 0x2e01, 0x3601, 0x3580},  // SM1
    {0x3d10, 0x3d08, 0x2d08, 0x2510, 0x2d10, 0x3510, 0x3508},  // SM2
    {0x3e40, 0x3680, 0x2680, 0x2640, 0x2e40, 0x3640, 0x2e80},  // SM3
    {0x3d04, 0x3d02, 0x2d02, 0x2504, 0x2d04, 0x3504, 0x3502},  // SM4
    {0x3d01, 0x3c80, 0x2c80, 0x2501, 0x2d01, 0x3501, 0x3480},  // SM5
    {0x3c10, 0x3c08, 0x2c08, 0x2410, 0x2c10, 0x3410, 0x3408},  // SM6
    {0x3c04, 0x3c02, 0x2c02, 0x2404, 0x2c04, 0x3404, 0x3402},  // SM7
    {0x3c01, 0x3b80, 0x2b80, 0x2401, 0x2c01, 0x3401, 0x3380},  // SM8
    {0x3b40, 0x3b20, 0x2b20, 0x2340, 0x2b40, 0x3340, 0x3320},  // SM9
};

// Alarm/pressure digits. AL0 overlaps SM6; never draw both on the same page.
static const uint16_t AL_DIGITS[4][7] = {
    {0x3c20, 0x3410, 0x2410, 0x2420, 0x2c20, 0x3420, 0x2c10},  // AL0
    {0x3c08, 0x3c04, 0x2c04, 0x2408, 0x2c08, 0x3408, 0x3404},  // AL1
    {0x3c02, 0x3c01, 0x2c01, 0x2402, 0x2c02, 0x3402, 0x3401},  // AL2
    {0x3b80, 0x3340, 0x2340, 0x2380, 0x2b80, 0x3380, 0x2b40},  // AL3
};

// Indoor temperature/humidity and clock digits. D320 overlaps SM2; D340/D350
// overlap SM4/SM5. TA is hour-units, TB/TC are minute tens/units.
static const uint16_t D320[7] = {0x3d20, 0x3d10, 0x2d10, 0x2520, 0x2d20, 0x3520, 0x3510};
static const uint16_t D340[7] = {0x3d08, 0x3d04, 0x2d04, 0x2508, 0x2d08, 0x3508, 0x3504};
static const uint16_t D350[7] = {0x3d02, 0x3d01, 0x2d01, 0x2502, 0x2d02, 0x3502, 0x3501};
static const uint16_t TA[7]   = {0x1c20, 0x1c10, 0x0c10, 0x0420, 0x0c20, 0x1420, 0x1410};
static const uint16_t TB[7]   = {0x1c08, 0x1c04, 0x0c04, 0x0408, 0x0c08, 0x1408, 0x1404};
static const uint16_t TC[7]   = {0x1c02, 0x1401, 0x0401, 0x0402, 0x0c02, 0x1402, 0x0c01};
static const uint16_t Y0[7]   = {0x3a04, 0x3202, 0x2202, 0x2204, 0x2a04, 0x3204, 0x2a02};
static const uint16_t Y1[7]   = {0x3a01, 0x3980, 0x2980, 0x2201, 0x2a01, 0x3201, 0x3180};
static const uint16_t Y2[7]   = {0x3940, 0x3920, 0x2920, 0x2140, 0x2940, 0x3140, 0x3120};

// Outdoor temperature, channel and humidity digits in the top-left zone.
static const uint16_t OT_TENS[7]  = {0x3f01, 0x3708, 0x2708, 0x2701, 0x2f01, 0x3701, 0x2f08};
static const uint16_t OT_UNITS[7] = {0x3702, 0x3610, 0x2610, 0x1e10, 0x2702, 0x2f02, 0x2e10};
static const uint16_t OT_CHAN[7]  = {0x1e20, 0x1e40, 0x0e40, 0x0620, 0x0e20, 0x1620, 0x1640};
static const uint16_t OH_TENS[7]  = {0x1e80, 0x1f01, 0x0f01, 0x0680, 0x0e80, 0x1680, 0x1701};
static const uint16_t OH_UNITS[7] = {0x1f08, 0x1f04, 0x0f04, 0x0708, 0x0f08, 0x1708, 0x1704};
// Wind speed digits inside the compass rose.
static const uint16_t WS_HUND[7]  = {0x2b04, 0x2302, 0x1302, 0x1304, 0x1b04, 0x2304, 0x1b02};
static const uint16_t WS_TENS[7]  = {0x2b01, 0x2280, 0x1280, 0x1301, 0x1b01, 0x2301, 0x1a80};
static const uint16_t WS_UNITS[7] = {0x2a40, 0x2220, 0x1220, 0x1240, 0x1a40, 0x2240, 0x1a20};
// Upper rain row: three digits plus a separate leading-one electrode.
static const uint16_t R1_HUND[7]  = {0x3908, 0x3904, 0x2904, 0x2108, 0x2908, 0x3108, 0x3104};
static const uint16_t R1_TENS[7]  = {0x3902, 0x3901, 0x2901, 0x2102, 0x2902, 0x3102, 0x3101};
static const uint16_t R1_UNITS[7] = {0x3880, 0x3840, 0x2840, 0x2080, 0x2880, 0x3080, 0x3040};
// Lower rain row: four accumulation digits.
static const uint16_t R2_THOU[7]  = {0x1910, 0x1108, 0x0108, 0x0110, 0x0910, 0x1110, 0x0908};
static const uint16_t R2_HUND[7]  = {0x1902, 0x1102, 0x0902, 0x0102, 0x0104, 0x1104, 0x0904};
static const uint16_t R2_TENS[7]  = {0x1880, 0x1080, 0x0880, 0x0080, 0x0901, 0x1901, 0x1101};
static const uint16_t R2_UNITS[7] = {0x1820, 0x1020, 0x0820, 0x0020, 0x0840, 0x1840, 0x1040};
// Solar digits in the lower-right zone.
static const uint16_t SO_HUND[7]  = {0x1a04, 0x1202, 0x0202, 0x0204, 0x0a04, 0x1204, 0x0a02};
static const uint16_t SO_TENS[7]  = {0x1a01, 0x1980, 0x0980, 0x0201, 0x0a01, 0x1201, 0x1180};
static const uint16_t SO_UNITS[7] = {0x1940, 0x1920, 0x0920, 0x0140, 0x0940, 0x1140, 0x1120};
// Day-of-month digits beside the three-character date row.
static const uint16_t Z0[7]       = {0x1d01, 0x1480, 0x0480, 0x0501, 0x0d01, 0x1501, 0x0c80};
static const uint16_t Z1[7]       = {0x1f02, 0x1610, 0x0210, 0x0610, 0x0f02, 0x1702, 0x0e10};

// Two sixteen-sector rings: current direction, then recent direction spread.
static const uint16_t WIND_ROSE[32] = {
    0x3304, 0x3302, 0x3220, 0x2a10, 0x2210, 0x0a20, 0x0a40, 0x0301,  // ring A W0..7
    0x0302, 0x0308, 0x0b08, 0x1308, 0x1b08, 0x2308, 0x2b08, 0x3308,  // ring A W8..15
    0x3b04, 0x3b02, 0x3a20, 0x3210, 0x1a10, 0x0220, 0x0240, 0x0280,  // ring B W16..23
    0x0304, 0x0310, 0x0b10, 0x1310, 0x1b10, 0x2310, 0x2b10, 0x3b08,  // ring B W24..31
};

// Auxiliary history/settings bar; use RAIN_BAR for the main rain display.
static const uint16_t BAR7[7] = {0x0301, 0x0280, 0x0240, 0x0220, 0x0208, 0x0204, 0x0a02};
// The first eleven entries are this glass variant's pressure-trend pointer.
// The remaining entries describe an alternate glass variant.
static const uint16_t BAR22[22] = {
    0x0002, 0x0802, 0x1002, 0x1802, 0x2002, 0x2802, 0x2801, 0x2001, 0x1801, 0x1001,
    0x0801, 0x3b02, 0x3b01, 0x3302, 0x3301, 0x2b02, 0x2b01, 0x2302, 0x2301, 0x2304,
    0x2b04, 0x3304,
};

// Rain-level bar electrodes in fill order, from first lit to last.
static const uint16_t RAIN_BAR[6] = {0x3808, 0x3008, 0x2808, 0x2008, 0x1808, 0x1008};
constexpr uint16_t SEG_RAIN_BAR_FRAME = 0x0808;
constexpr uint16_t SEG_RAIN_UNIT_IN = 0x0004;
constexpr uint16_t SEG_RAIN_UNIT_MM = 0x0008;

// Pressure trend: exactly one position, from fastest rise (0) through steady (5)
// to fastest fall (10). Uses the first eleven BAR22 entries.
constexpr uint8_t PTREND_N = 11;
constexpr uint16_t SEG_TREND_ZONE = 0x1210;
constexpr uint16_t SEG_TREND_AXIS = 0x0001;

// Air-quality battery icon beside the indoor heading, distinct from outdoor battery.
constexpr uint16_t SEG_AQ_BATT = 0x3480;
static const uint16_t CH_MARK[6] = {
    0x2608,
    0x1e08,
    0x2602,
    0x2580,
    0x2510,
    0x1c80,
};

// Wi-Fi bars in fill order; a level of zero leaves every bar dark.
static const uint16_t WIFI_BARS[4] = {0x0340, 0x0b40, 0x1340, 0x1b40};

// Outdoor reception antenna and bars, followed by the wind/rain equivalents.
constexpr uint16_t SEG_RF_ANT = 0x3f08;
static const uint16_t RF_BARS[4] = {0x3f04, 0x3f02, 0x3e10, 0x3e08};
constexpr uint16_t SEG_RF_WIND_ANT = 0x3b01;
static const uint16_t RF_WIND_BARS[4] = {0x3301, 0x3a80, 0x3280, 0x3a40};
constexpr uint16_t SEG_RF_RAIN_ANT = 0x2810;
static const uint16_t RF_RAIN_BARS[4] = {0x2010, 0x1810, 0x1010, 0x0810};
// Upper rain-row unit marks; separate from the rain-bar unit marks.
constexpr uint16_t SEG_R1_UNIT_IN = 0x3810;
constexpr uint16_t SEG_R1_UNIT_MM = 0x3010;
// Pressure decimal points: inHg uses two decimal places, hPa/mmHg one.
constexpr uint16_t SEG_PR_DP_INHG = 0x2404;
constexpr uint16_t SEG_PR_DP_HPA = 0x2401;
// UVI bar in increasing fill order.
static const uint16_t UVI_BARS[5] = {0x1804, 0x2004, 0x2804, 0x3004, 0x3804};
constexpr uint16_t SEG_WIND_DEG_A = 0x3240;
constexpr uint16_t SEG_WIND_DEG_B = 0x2a20;

// Forecast composition: sun, sun+cloud, cloud, cloud+rain, or cloud+snow.
constexpr uint16_t SEG_FC_SUN = 0x3801;
constexpr uint16_t SEG_FC_CLOUD = 0x3802;
constexpr uint16_t SEG_FC_RAIN = 0x3002;
constexpr uint16_t SEG_FC_SNOW = 0x3001;

// Moon crescents in southern orientation; MOON_SEG_B reverses them for north.
static const uint16_t MOON_SEG_A[4] = {0x1320, 0x0b20, 0x0320, 0x0380};
static const uint16_t MOON_SEG_B[4] = {0x0380, 0x0320, 0x0b20, 0x1320};
static const uint8_t MOON_MASK[8] = {0x00, 0x08, 0x0c, 0x0e, 0x0f, 0x07, 0x03, 0x01};
// Hardware checks established that northern waxing phases light the right limb.
inline const uint16_t *moon_seg(bool north) { return north ? MOON_SEG_B : MOON_SEG_A; }

// The bottom-left row shares date and air-quality labels.
constexpr uint16_t SEG_ROW_DATE = 0x0502;
constexpr uint16_t SEG_ROW_AQ = 0x0404;
constexpr uint16_t SEG_ROW_AQU = 0x0440;

// Clock status icons: snooze, daylight-saving time and armed alarm.
constexpr uint16_t SEG_TIME_SNOOZE = 0x0b80;
constexpr uint16_t SEG_TIME_DST = 0x1b80;
constexpr uint16_t SEG_TIME_BELL = 0x1380;

// Sunrise hour-tens electrodes; the remaining positions show hour/minute digits.
constexpr uint16_t SEG_SR_T2 = 0x2a08;
constexpr uint16_t SEG_SR_T12 = 0x3208;
constexpr uint16_t SEG_SR_T1 = 0x2208;

// 3-letter month / weekday names for the bottom-left row, in English (month-1 and Sunday-based weekday indexing).
static const char MONTH3[12][4] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static const char WDAY3[7][4] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// Wind-sensor low-battery icon. Voltage-type readings use a 2.42 V threshold.
constexpr uint16_t SEG_WIND_BATT = 0x3a10;
// Outdoor-sensor low-battery icon; interpret voltage or flag by sensor family.
constexpr uint16_t SEG_OUT_BATT = 0x3b20;
// Rain-sensor low-battery icon; the component decodes each sensor family.
constexpr uint16_t SEG_RAIN_BATT = 0x0010;

constexpr uint16_t SEG_OUT_ROTATE = 0x0701;
constexpr uint16_t SEG_OUT_LABEL = 0x3620;
constexpr uint16_t SEG_OUT_DEW = 0x3e80;
constexpr uint16_t SEG_OUT_FEELS = 0x3e20;
constexpr uint16_t SEG_OUT_FLAG = 0x0640;
constexpr uint16_t SEG_OUT_DP = 0x3704;
constexpr uint16_t SEG_OUT_C = 0x2704;
constexpr uint16_t SEG_OUT_F = 0x2f04;
constexpr uint16_t SEG_OUT_ONE = 0x2e20;
constexpr uint16_t SEG_OUT_MINUS = 0x2620;
constexpr uint16_t SEG_OUTH_PCT = 0x0704;
constexpr uint16_t SEG_CHAN_TICK = 0x3608;
constexpr uint16_t SEG_CHAN_ARROW = 0x3d40;

constexpr uint16_t SEG_IN_DP = 0x3540;
constexpr uint16_t SEG_IN_C = 0x2540;
constexpr uint16_t SEG_IN_F = 0x2d40;
constexpr uint16_t SEG_IN_ONE = 0x2e08;
constexpr uint16_t SEG_INH_PCT = 0x3c80;

constexpr uint16_t SEG_PR_LABEL = 0x3c40;
// Relative pressure is the measured absolute pressure plus the configured offset.
constexpr uint16_t SEG_PR_REL = 0x3c10;
constexpr uint16_t SEG_PR_ABS = 0x3440;
constexpr uint16_t SEG_PR_HPA = 0x2b20;
constexpr uint16_t SEG_PR_INHG = 0x2320;
constexpr uint16_t SEG_PR_MMHG = 0x1b20;
constexpr uint16_t SEG_PR_ONE = 0x2c40;

// The TIME caption and colon are separate electrodes; only the colon blinks.
constexpr uint16_t SEG_TIME_LABEL = 0x2440;
constexpr uint16_t SEG_TIME_COLON = 0x0410;
constexpr uint16_t SEG_TIME_PM = 0x1c01;
constexpr uint16_t SEG_TIME_T12 = 0x1c40;
constexpr uint16_t SEG_TIME_T1 = 0x0c40;
constexpr uint16_t SEG_TIME_T2 = 0x1440;

constexpr uint16_t SEG_WIND_UNITS = 0x3b10;
constexpr uint16_t SEG_WIND_GUST = 0x3310;
constexpr uint16_t SEG_WIND_AVG = 0x3320;
constexpr uint16_t SEG_WIND_DP = 0x0a80;
constexpr uint16_t SEG_WIND_MS = 0x0b02;
constexpr uint16_t SEG_WIND_KMH = 0x2b02;
constexpr uint16_t SEG_WIND_MPH = 0x2a80;
constexpr uint16_t SEG_WIND_KNOTS = 0x0b04;
constexpr uint16_t SEG_WIND_BFT = 0x0b01;

constexpr uint16_t SEG_RAIN_LABEL = 0x3910;
constexpr uint16_t SEG_R1_RATE_A = 0x2910;
constexpr uint16_t SEG_R1_RATE_B = 0x2110;
constexpr uint16_t SEG_R1_EVENT = 0x3020;
constexpr uint16_t SEG_R1_ONE = 0x3110;
constexpr uint16_t SEG_R1_DP2 = 0x2104;
constexpr uint16_t SEG_R1_DP1 = 0x2101;
constexpr uint16_t SEG_R2_DAILY = 0x1908;
constexpr uint16_t SEG_R2_WEEKLY = 0x1904;
constexpr uint16_t SEG_R2_MONTHLY = 0x2040;
constexpr uint16_t SEG_R2_YEARLY = 0x2020;
constexpr uint16_t SEG_R2_TOTAL = 0x2820;
constexpr uint16_t SEG_R2_DP2 = 0x0101;
constexpr uint16_t SEG_R2_DP1 = 0x0040;

constexpr uint16_t SEG_ROT_DATE_A = 0x3a02;
constexpr uint16_t SEG_ROT_DATE_B = 0x1a02;
constexpr uint16_t SEG_ROT_UV_A = 0x3a08;
constexpr uint16_t SEG_ROT_UV_B = 0x1a08;
constexpr uint16_t SEG_ROT_UV_C = 0x1004;
constexpr uint16_t SEG_SOL_KLUX = 0x2180;
constexpr uint16_t SEG_SOL_WM2 = 0x2120;
constexpr uint16_t SEG_SOL_KFC = 0x0804;
constexpr uint16_t SEG_SOL_DP1 = 0x0120;
constexpr uint16_t SEG_SOL_DP2 = 0x0180;
constexpr uint16_t SEG_SOL_T2 = 0x0a08;
constexpr uint16_t SEG_SOL_T12 = 0x1208;
constexpr uint16_t SEG_SOL_T1 = 0x0208;

// Bottom-left CO2 label, unrelated to the armed-alarm bell.
constexpr uint16_t SEG_ROW_CO2 = 0x1e08;

// Short labels for the three-character row.
static const char ROWTXT_MAX[4] = "MAX";
static const char ROWTXT_MIN[4] = "MIN";
static const char ROWTXT_ALM[4] = "ALM";
static const char ROWTXT_RST[4] = "RST";
static const char ROWTXT_MAC[4] = "MAC";
static const char ROWTXT_STH[4] = "STH";   // 42 -- setting step 14, hemisphere byte == 0
static const char ROWTXT_NTH[4] = "NTH";   // 43 -- setting step 14, hemisphere byte != 0
static const char ROWTXT_CAL[4] = "CAL";   // 44 -- CO2 calibration (setting step 15)

// Small-font letter codes used by the setting pages.
constexpr uint8_t SF_b = 0x0b, SF_C = 0x0c, SF_E = 0x0e, SF_F = 0x0f, SF_H = 0x10, SF_I = 0x11,
                  SF_L = 0x12, SF_n = 0x13, SF_O = 0x14, SF_P = 0x15, SF_DEG = 0x17,
                  SF_o = 0x18;

// Beaufort lower limits, expressed in tenths of a metre per second.
inline uint8_t beaufort_x10(uint32_t speed) {
    constexpr uint16_t lower[] = {3,16,34,55,80,108,139,172,208,245,285,327};
    uint8_t force = 0;
    while (force < 12 && speed >= lower[force]) ++force;
    return force;
}

// ---- primitive render helpers ------------------------------------------------------
inline void seg(uint8_t *fb, uint16_t id, bool on) { gpl_set_seg(fb, id, on); }

// One small 7-seg character (font code 0..31) at SM/AL-style table `tab`.
inline void draw_small_glyph(uint8_t *fb, const uint16_t tab[7], uint8_t code) {
    uint8_t pat = small_glyph(code);
    for (int i = 0; i < 7; i++)
        gpl_set_seg(fb, tab[i], (pat >> i) & 1);
}
inline void draw_small_at(uint8_t *fb, uint8_t pos, uint8_t code) { draw_small_glyph(fb, SM_DIGITS[pos % 10], code); }
inline void al_char_at(uint8_t *fb, uint8_t pos, uint8_t code) { draw_small_glyph(fb, AL_DIGITS[pos % 4], code); }

// Decimal formatting, units first. A minus occupies the next free place.
// Six cells support five magnitude digits and a sign; overflow displays dashes.
inline void fmt_dashes(uint8_t *buf) { memset(buf, SM_MINUS, 6); }
inline void fmt_blank(uint8_t *buf) { memset(buf, SM_BLANK, 6); }
inline void fmt_decimal(int32_t value, uint8_t *buf) {
    fmt_blank(buf);
    uint32_t magnitude = value < 0 ? uint32_t(-int64_t(value)) : uint32_t(value);
    if (magnitude > 99999 || (value < 0 && magnitude > 9999)) {
        fmt_dashes(buf);
        return;
    }
    unsigned count = 0;
    do { buf[count++] = magnitude % 10; magnitude /= 10; } while (magnitude);
    if (value < 0) buf[count] = SM_MINUS;
}
inline void fmt_pair(uint32_t value, uint8_t *buf) {
    buf[0] = value % 10;
    buf[1] = (value / 10) % 10;
}
// Fixed-width, most-significant cell first; report overflow to the caller.
inline bool fmt_codes(int32_t value, uint8_t *out, uint8_t count) {
    if (!count || count > 6) return false;
    uint32_t magnitude = value < 0 ? uint32_t(-int64_t(value)) : uint32_t(value);
    memset(out, SM_BLANK, count);
    uint8_t remaining = count;
    do {
        if (!remaining) return false;
        out[--remaining] = magnitude % 10;
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) {
        if (!remaining) return false;
        out[remaining - 1] = SM_MINUS;
    }
    return true;
}

// Render an integer right-aligned into SM positions [pos, pos+count), MSD at pos.
// Stock semantics: leading-zero blank, minus before MSD, units never blank.
// Overflow or `missing` renders all dashes ("---" for absent sensors).
inline void sm_number(uint8_t *fb, uint8_t pos, uint8_t count, int32_t v, bool missing = false) {
    uint8_t codes[6];
    if (missing || count > 6 || !fmt_codes(v, codes, count)) {
        for (uint8_t i = 0; i < count; i++)
            draw_small_at(fb, pos + i, SM_MINUS);
        return;
    }
    for (uint8_t i = 0; i < count; i++)
        draw_small_at(fb, pos + i, codes[i]);
}

// Same into the alarm-digit positions (alarm page).
inline void al_number(uint8_t *fb, uint8_t pos, uint8_t count, int32_t v, bool missing = false) {
    uint8_t codes[6];
    if (missing || count > 6 || !fmt_codes(v, codes, count)) {
        for (uint8_t i = 0; i < count; i++)
            al_char_at(fb, pos + i, SM_MINUS);
        return;
    }
    for (uint8_t i = 0; i < count; i++)
        al_char_at(fb, pos + i, codes[i]);
}

// Render a 2-digit value into two explicit 7-seg tables (MSD into t0, LSD into t1) —
// for the stock positions that are not part of the SM0..9 run (indoor humidity D340/D350,
// secondary Y0/Y1). Stock leading-zero-blank / minus semantics as sm_number().
inline void sm_pair(uint8_t *fb, const uint16_t t0[7], const uint16_t t1[7], int32_t v,
                    bool missing = false) {
    uint8_t codes[2];
    if (missing || !fmt_codes(v, codes, 2)) {
        draw_small_glyph(fb, t0, SM_MINUS);
        draw_small_glyph(fb, t1, SM_MINUS);
        return;
    }
    draw_small_glyph(fb, t0, codes[0]);
    draw_small_glyph(fb, t1, codes[1]);
}

// Write all fourteen segments, including clear bits, so a glyph can be overwritten.
// Position zero is the leftmost character.
inline void big_char(uint8_t *fb, uint8_t pos, char c) {
    const uint16_t *tab = (pos == 0) ? SEG_POS0 : (pos == 1) ? SEG_POS1 : SEG_POS2;
    uint16_t mask = console_glyph(c);
    for (uint8_t i = 0; i < 14; i++)
        gpl_set_seg(fb, tab[i], ((mask >> i) & 1) != 0);
}

// Three BIG characters, left to right; short strings are space-padded (= cleared).
inline void big_text(uint8_t *fb, const char *s) {
    bool ended = false;
    for (uint8_t i = 0; i < 3; ++i) {
        if (!ended && *s == '\0') ended = true;
        big_char(fb, i, ended ? ' ' : *s++);
    }
}

// Big 3-digit group. Renders the absolute value right-aligned, leading zeros blank, minus
// in the next free slot left of the MSD, units always shown. `missing` -> "---".
// Overflow saturates to dashes.
inline void big3(uint8_t *fb, int32_t v, bool missing = false) {
    uint8_t codes[3];
    if (missing || !fmt_codes(v, codes, 3)) {
        big_text(fb, "---");
        return;
    }
    char out[4] = {' ', ' ', ' ', 0};
    for (uint8_t i = 0; i < 3; i++)   // codes[0] = MSD -> left position 0
        out[i] = (codes[i] == SM_BLANK) ? ' ' : (codes[i] == SM_MINUS) ? '-'
                                                                       : (char)('0' + codes[i]);
    big_text(fb, out);
}

// Sixteen equal 22.5-degree sectors, centred on N, NNE, ... .
inline uint8_t rose_sector(int degrees) {
    int wrapped = ((degrees % 360) + 360) % 360;
    return uint8_t(((wrapped * 4 + 45) / 90) % 16);
}

// Ring zero shows current direction; ring one shows recent direction spread.
inline void rose_mask(uint8_t *fb, uint16_t mask, uint8_t ring) {
    const uint16_t *tab = WIND_ROSE + (ring ? 16 : 0);
    for (uint8_t i = 0; i < 16; i++)
        gpl_set_seg(fb, tab[i], ((mask >> i) & 1) != 0);
}

// Convenience: light exactly one sector. dir_deg < 0 clears the ring (stock: 0xFFFF).
inline void rose(uint8_t *fb, int dir_deg, uint8_t ring) {
    rose_mask(fb, (dir_deg < 0 || dir_deg > 719) ? 0
                                                 : (uint16_t)(1u << rose_sector(dir_deg)),
              ring);
}

// Fill-style bar graph: light the first `level` of `count` entries (clears the rest).
inline void bar(uint8_t *fb, const uint16_t *tab, uint8_t count, uint8_t level) {
    for (uint8_t i = 0; i < count; i++)
        gpl_set_seg(fb, tab[i], i < level);
}

// Active-low backlight: levels 0..4 map to duties 8,4,2,1,0.
// Duty 8 must remain unmasked so level zero really switches the light off.
inline void gpl_backlight_level(uint8_t level) {
    static const uint8_t DUTY[5] = {8, 4, 2, 1, 0};
    if (level > 4)
        level = 4;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, DUTY[level]);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Buzzer gate is register 0x16; drive is register 0x22 bit 7. Idle value is 0x43.
inline void gpl_buzzer(bool on) {
    gpl_write_reg(0x16, on ? 0x01 : 0x00);
    gpl_write_reg(0x22, on ? 0xC3 : 0x43);
}

}  // namespace ecowitt_rf
}  // namespace esphome
