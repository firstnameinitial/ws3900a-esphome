# WS3900A panel capability audit

Audit date: 2026-10-28. This document separates the observed baseline from the replacement implementation and from device qualification. An implemented or host-tested item is not automatically a device-tested item.

## Evidence and interpretation

The primary functional reference is the [official WS3900/WS3910 manual](https://oss.ecowitt.net/uploads/20240924/WS3900%26WS3910%20Manual.pdf), especially the glass legend and icon explanations (pp.23–36), button table (pp.38–40), settings/MAX/MIN/alarm pages (pp.41–44), and optional sensor list. The manual identifies the regional A suffix; the eight-button panel and glass match this console.

The [supplied all-electrodes reference image](https://i5.walmartimages.com/asr/2ff6ee9b-a91b-478a-a481-b83b29441704.9efb28e139977e5891f01678d48b452e.jpeg) establishes the available captions, numeric cells, units and icons. It shows mutually exclusive states simultaneously. It is not a target normal page: an empty rain/UV histogram at zero, a dark low-battery icon with healthy batteries, and absent live weather on the MAX/MIN/alarm/MAC pages can all be correct.

The owner-supplied button-cycle recording was reviewed against beep/action boundaries. Times below are seconds from the start of that recording, not wall-clock times. The recording shows the baseline firmware, not proof that the subsequent replacement works on the device. Stock-firmware analysis supplements the manual where it establishes actual electrode ownership or resolves an unspecified calculation; discrepancies remain explicit.

## Observed button cycle

| Relative time | Observation | Interpretation and resolution |
|---|---|---|
| 0–2s | Outdoor10.4°C/89%; forecast blank | Live weather page. Forecast startup must show partly cloudy while learning. |
| 3s | FEELSLIKE10.4°C | Derived-temperature caption works; this value equals outdoor temperature in the applicable range. |
| 4s | DEW8.7°C | Dew-point page and caption. |
| 5s | CH1,15.1°C/70%; OUTDOOR dark | Extra sensor page. The glass has a channel number, not an arbitrary attic-name field. |
| 6–11s | Auto, outdoor and derived pages | TEMP selection and five-second auto rotation must remain distinct. |
| 12–18s | Rain rate0, event1.5; bar changes with upper row | Empty bar at0rate is correct even when daily rain is positive. |
| 19–27s | Daily1.5, weekly18.3, monthly68.7, yearly503.6, total59.8 | RAIN2 cycles the lower row. Legacy TOTAL has incomplete provenance; preserve its migration anchor and add future console-observed increments. Do not invent missing earlier totals. |
| 29–38s | Wind speed/gust/direction/averaged direction | Four WIND views; direction views require degrees and the corresponding compass mode. |
| 39–44s | + changes date presentation | Baseline defect: month/day and weekday/day should alternate automatically; + selects available AQ fields. |
| 45–49s | REL/ABS pressure changes | Supported quick toggle. |
| 50–57s | LIGHT brightness cycle runs in the wrong direction | Correct DC sequence: Max→High→Medium→Low→Off. |
| 59–60s |31.3°C on MAX; malformed leading M | A stored maximum, not a31°C live reading. Fix the physical M strokes and retain honest historical provenance. |
| 61–62s | Minimum−0.1°C | Stored minimum page; unrelated live fields are intentionally absent. |
| 63–65s | Alarm fields, then MAC | Alarm hour/minute editing and identification page, not failed weather displays. |
| 66–74s | Normal→MAX→MIN→alarm hour/minute→MAC→Normal | Confirms the repeated SET navigation sequence. |

## Controls and side effects

| Control | Required behavior | Replacement implementation / qualification |
|---|---|---|
| SET short | Normal→MAX→MIN→alarm hour→alarm minute→MAC→Normal | State machine and isolated page renderers implemented; final physical walkthrough pending. |
| SET hold2s | Enter settings; SET advances; +/− edit; LIGHT/30s returns Normal | Manual order restored; clock edits change the actual local calendar. Shared console validity supports2000–2099, including years ESPHome otherwise treats as unsynchronized. |
| TEMP short | Outdoor→Feels-like→Dew→available CH1…CH8→Auto | Sparse available channels, explicit CH caption, OUTDOOR suppressed on CH; Auto rotates measured outdoor/channels every5s and skips derived values. |
| TEMP hold5s | Re-register current outdoor source, current CH, or outdoor+CH in Auto; clear corresponding records | Deliberate component effect clears matching accepted registration/history state; continuous receive reacquires it. First sightings must not be displayed as accepted channels. Physical rescan qualification pending. |
| RAIN1 | Upper Rate/Event and synchronized histogram | Implemented. No extra undocumented long-press action. |
| RAIN2 | Lower Daily/Weekly/Monthly/Yearly/Total | Implemented with persisted period/console-total policy described below. |
| WIND | Speed/Gust/Direction/10-minute direction | Implemented. In MAX toggles recorded speed/gust; in alarm arms/disarms. |
| +, Normal | Date/available CO2/PM1/PM2.5/PM10/AQI | Available-field selector implemented; no AQ source leaves + inert. DATE alternates month/day and weekday/day automatically every5s. |
| − short / hold | REL/ABS; UV+solar versus sunrise+sunset | Implemented. Missing sun configuration, invalid date, or no horizon crossing yields missing data, not invented times. |
| LIGHT, DC | Max→High→Medium→Low→Off | Correct descending selection and active-low GPL brightness mapping. |
| Any key, battery | Wake Medium; inactivity15s→Off; LIGHT Medium/Off | Supply detection and monotonic policy implemented. Raw key activity wakes promptly; full press/hold/release behavior requires device qualification. |
| DC connect/remove | Set Medium on transition; battery timeout after removal | Uses GPL register03 bit1. Schedule/manual override behavior is separately tested. |
| LIGHT in other pages | Return Normal | Implemented. |
| LIGHT while ringing | Snooze10minutes; show snooze icon | Pure alarm engine uses monotonic snooze timing; re-ring no longer requires the original alarm HH:MM. |
| Other key while ringing | Stop alarm | Shared physical/injected event path; physical press consumption and synthetic LIGHT context are handled separately where needed. |
| Any key held2s while snoozing | Cancel snooze | Detailed manual prose specifies any button; Table5 highlights LIGHT. Raw-held timing handles TEMP at2s and consumes the press through release, independently of its5s registration action. |
| TEMP+− hold2s | Open configuration AP; fast WiFi flash;5-minute/client-extended window | Native IDF APSTA helper uses compiled AP settings, preserves STA/netif, and never rewrites STA credentials. Existing fallback is borrowed unchanged. See ownership limits below. |
| SET+LIGHT hold5s | Application defaults and reboot | Explicit application-preference allowlist; global ESPHome NVS and compiled WiFi/API/OTA identity retained. No indiscriminate NVS erase. Live factory reset is not part of routine qualification. |

Held-SET order is: beep;12/24h;hour;minute;year;month;day;pressure unit;relative pressure700–1100hPa;temperature unit;wind unit;rain unit;solar unit;hemisphere;daily high/low reset. Built-in CO2 calibration is WS3910-only and skipped on this WS3900A.

## Glass and data coverage

| Region/capability | Implemented behavior | Evidence and limits |
|---|---|---|
| Outdoor / derived / channels | Exclusive OUTDOOR/FEELSLIKE/DEW/CH states, units, auto icon, selected-source RF/battery | Manual pp.23,29,33–34; recording3–11s. An installed CH sensor cannot be renamed on fixed glass. |
| MAX/MIN records | Primary and channel T/H, derived values, indoor T/H and pressure; MAX wind/gust/rain rate/UV/solar | Manual p.42 deliberately omits forecast, live clock/moon and accumulation. MIN omits wind/rain/UV/solar. Missing records remain dashed. |
| MAX/MIN caption | Correct physical M and dedicated A/X/I/N strokes | Electrode mapping tested separately from text string selection; recording59–62s exposed the baseline defect. |
| Daily high/low | Persisted local-date rollover resets records when enabled | Resets primary and channel histories; clock regressions must not repeatedly erase records. |
| Rain upper row/bar | Rate from last10minutes×6; Event upper row; numeric units and corresponding histogram |0rate means no filled bar; bar is not a daily-total gauge. |
| Rain periods | Configurable day hour, Sunday/Monday week start, year-season month | Settings are available in the console web UI; policy changes preserve observed totals and rebase unavailable history. |
| Rain event | Ends only with a complete/proven24h window below1mm and no rain in the last hour | Partial observations, pre-NTP gaps and counter resets do not prove a whole dry day. |
| Console TOTAL | Separate persisted migration anchor plus calibrated future increments | Never decreases on sensor reset/provider change/gain change. Exposed migration/incomplete-history flags prevent a false reconstruction of earlier rainfall. |
| UVI | Numeric0–15; filled bands at>0/>3/>6/>9/>12 | Manual p.31. Night/zero has an empty histogram; this is not missing capability. |
| Solar/sun | Solar W/m²/kfc/klux; sunrise/sunset with time captions in alternate view | Units and values are mutually exclusive with sun times; UV graph is dark in time view. |
| Forecast | Sunny; partly cloudy with both clouds; cloudy; rain; snow; storm; storm-snow | Default partly cloudy during warm-up. Corrected groups light the whole cloud/precipitation shapes. |
| Storm animation | Clouds stay visible while precipitation blinks | Follows manual pp.26–27. Stock renderer blanks the whole storm pictograph during its off phase; this replacement intentionally follows the documented presentation. |
| Forecast learning / pressure trend | Persisted timestamped progressive history; pressure pointer compares with prior30-day context | Training/clock-wait diagnostics remain visible; host tests cannot substitute for elapsed real-world learning. |
| Moon | Eight phases, hemisphere-dependent orientation; plain arc at new moon | Uses calendar; no extra moon sensor. This image is compiled for868MHz and defaults North. This audit does not claim a generic433MHz image automatically defaults South. |
| Initial search display | DATE and sunrise/sunset for20s, then available AQ and UV/solar; explicit user choices take precedence | Manual p.35–36; startup state policy and integration implemented. |
| AQ row | Correct shared DATE/CO2/PM/AQI cells, captions/units, selected external-source link/battery | WH46 PM1 and unavailable sentinels preserved. No optional source means these fields correctly stay unused. |
| AQI calculation | Explicit historical stock US EPA2012 PM2.5 convention, including exact tenths | Stock routine establishes its older breakpoints; it is not silently presented as a current regulator's scale. |
| Indoor / console battery | Built-in T/H/P or selected WN32P replacement; independent calibrated console battery icon | Known zero/missing readings remain distinct. Console battery-low/no-battery shares the same stock icon; healthy battery leaves it dark. |
| Source ownership | Independent outdoor, wind, rain, UV/solar and indoor priorities, accepted/fresh/enabled gating | LCD, native publication and station upload fields must follow the same owners; per-channel data remains independent. |
| WiFi | Fast AP/unconnected blink; slow connected/no-successful-upload blink; steady signal after successful upload | Link state alone does not prove successful weather upload. Actual AP diagnostic remains distinct from the manual AP request window. |

## Persistence and AP ownership

The existing14-byte panel-settings layout is unchanged. New history, schedule/rain policy and console-total data use separate preferences/checkpoints; existing rain-window and calendar layouts remain compatible. Rollback must not depend on preserving an arbitrary C++ structure layout after fields are reordered.

The AP helper never stops the WiFi driver, disables STA, writes STA configuration, or destroys the shared AP netif. A manual request lasts five minutes and refreshes while clients are connected. Expiry closes only AP/portal resources started by that request. A pre-existing fallback remains active. If STA is disconnected or client enumeration fails at expiry, ownership is handed back to ESPHome recovery rather than removing an uncertain recovery path; a later successful STA transition can close it. Failed partial starts roll back to STA; failed stop/rollback retries are limited to once per second. An AP probe should verify connected STA continuity and actual AP mode, without joining or changing credentials.

Factory-reset scope is deliberately narrower than erasing all ESPHome NVS: application settings, upload configuration and histories reset; transport/provisioning identity remains recoverable. Save/flush failure cancels reboot. This is a non-atomic sequence of application-preference writes; no atomic reset or interrupted-write rollback guarantee is claimed. Shutdown flushes are suppressed after a successful reset so old records cannot overwrite defaults.

## Verification tiers and remaining qualification

- **Source/evidence:** official manual, all-electrodes inventory, recording action boundaries and independent stock-electrode/calculation review.
- **Host:** C++ state/render/history/rain/source/AQ tests under ASan/UBSan and warnings-as-errors; native-IDF adapter mocks assert AP-only configuration, STA retention, bounded retries and cleanup. JavaScript tests exercise actual form payloads, validation boundaries, older-config compatibility and failed saves. These prove deterministic logic, not actual glass, supply voltages or RF hardware.
- **Build:** the complete integrated ESP-IDF firmware compiles. Host tests also cover startup selection and the shared console-clock validity range. Module compilation alone is not the build claim.
- **Device:** physical page/button sweep; actual DC/battery transitions; clock edits and year/day boundaries; alarm/snooze/held-key consumption; safe APSTA start/expiry; actual electrode photographs. The replacement image is on the device and qualification is underway. Final physical results must be recorded separately from implementation claims. Alarm, buzzer and power timers continue during the segment-walk diagnostic.

No built-in CO2 calibration can be demonstrated on WS3900A without the WS3910 hardware. Optional AQ sensors, WN32P substitution and additional RF families require those transmitters for device qualification. The recording does not establish long-hold, clock edit, supply transition, new AQ, forecast-learning, factory-reset or AP-expiry behavior. No invented readings or unrelated illuminated segments are used to make those areas look populated.

## Integrated qualification receipt —28 October2026

Code004807c passed29 sanitizer C++ programs, both dashboard JavaScript suites,
and complete pinned ESPHome2026.6.2 Mac/Ubuntu builds. The final image uses68.8%
of the application slot. The console was upgraded with its private configuration;
its encrypted native API retained64 entities/55 state publications and no BidCoS.

Remote injected buttons through the normal component handler and the live LCD
framebuffer verified normal/derived/CH1/Auto, both rain selectors/histogram, all wind
pages, REL/ABS, sunrise/set, the complete SET sequence and all15 settings steps.
The physical raw-key scanner/long holds were covered by host tests and the baseline
owner recording; a post-change finger-operated sweep is a separate observation.

All seven forced forecast compositions passed, including both phases of each storm.
The exact0x0137 M glyph passed. The31.3°C/−0.1°C records, independent CH1 record,
503.6mm YEARLY and59.8mm console TOTAL survived the final orderly OTA restart.
Configuration matched after normal RF rediscovery. Immediate partly-cloudy forecast
continued; elapsed thirty-day pressure learning cannot be proved by this short trial.

The actual APSTA probe opened while STA, RF and HTTP uploads remained available,
then closed at the five-minute lease boundary without a reboot; both custom and
cloud uploads reported200 afterwards. Current requests can still fail transiently
on weak WiFi, as before this change; this trial is not a long-term delivery guarantee.

Factory reset/re-registration were implemented and scope/failure paths tested on
host, without deleting the owner's records as a qualification exercise. Actual
battery/DC transitions, physical cold power loss/delayed NTP, live alarm/snooze
cadence and optional AQ/WN32P hardware remain hardware qualification items. Calendar,
AQ packets/sentinels/AQI, power transitions and alarm timing are covered by deterministic
host fixtures. The source matrix distinguishes those tests from installed hardware.

## Persistence follow-up —29 October2026

The API name/channel override save path identified during1.0 review is corrected
in b1f95b5, with a separate validated preference and automatic-channel reset.
[Sensor settings and device qualification](sensor-settings.md).
Thirty host programs and both dashboard suites pass; Mac/Linux builds and actual
restart/rollback/reinstall retention pass. Physical acceptance limits above remain.

## Version 1.0 acceptance —29 October2026

An actual DC unplug/replug produced reset reason POWERON, with restored sensor
preferences, rain TOTAL and24hourly/1daily pressure observations by the first read
at9s uptime. The clock was already valid; this does not test deliberately delayed NTP.

The cold-start path originally forced Medium brightness. Version 1.0 removes that
stock-behaviour override and restores the saved DC brightness, including Off, on
cold and warm starts. All five saved levels are regression-tested; live supply
transitions retain their existing Medium/battery-wake policy. See the final release
notes for the installed brightness-retest result.

The remaining physical button/alarm, fresh-rain and optional-hardware checks are
explicitly deferred. Automated coverage and the earlier observations remain as
listed; version 1.0 does not certify those deferred physical outcomes. Startup
brightness restoration intentionally follows the retained user preference rather
than the stock default-Medium rule.
