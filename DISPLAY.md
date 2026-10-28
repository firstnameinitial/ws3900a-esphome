# Display and buttons

| Button | Normal display | Other modes |
|---|---|---|
| SET | MAX → MIN → alarm hour → alarm minute → MAC → Normal | Hold2s: settings; SET advances; LIGHT/30s exits |
| TEMP | Outdoor → Feels-like → Dew → available CH1–CH8 → Auto → Outdoor | MAX/MIN: primary/derived/channel records; hold5s re-registers selected source |
| RAIN1 | Rate / Event; histogram follows this upper row | — |
| RAIN2 | Daily / Weekly / Monthly / Yearly / Total | — |
| WIND | Speed / Gust / Direction /10-minute direction | MAX: speed/gust; Alarm: arm/disarm |
| + | DATE / available CO2 / PM1 / PM2.5 / PM10 / AQI | Settings/alarm: increase |
| − | Relative / absolute pressure | Settings/alarm: decrease |
| Hold − | UV/solar / sunrise/sunset | — |
| LIGHT | DC: Max→High→Medium→Low→Off; battery: Medium/Off | Return Normal; ringing alarm:10-minute snooze |
| TEMP+− hold2s | Five-minute configuration AP, extended while connected clients are present | STA credentials retained |
| SET+LIGHT hold5s | Reset application settings/history and reboot | Compiled WiFi/API/OTA identity retained |

Held-SET menu: Beep →12/24h →hour →minute →year →month →day →pressure unit →
relative pressure700–1100hPa →temperature unit →wind unit →rain unit →solar unit →
hemisphere →daily high/low reset. Calendar fields are editable; network time may
later synchronise them. Built-in CO2 calibration belongs to WS3910 and is skipped.

DATE automatically alternates month/day and weekday/day every5s. With no AQ
transmitter, + stays on DATE. At startup the display searches for20s on DATE and
sunrise/set, then selects available AQ and UV/solar. Explicit controls take priority.
Auto scrolls measured outdoor and accepted, enabled/recent channels every5s.
CH identifies the selected sensor; fixed glass cannot show arbitrary sensor names.
Missing dependencies remain dashed, including wind chill without a wind reading.

The rain bar follows Rate/Event; a dry rate correctly gives an empty bar beside a
positive DAILY total. UV0 has an empty bar; filled bands end at3/6/9/12.
System → Console configures the rain day/week/season, traditional/piezo preference,
automatic lighting schedule and daily records. Console TOTAL retains its migration
anchor and adds future calibrated rain; it cannot reconstruct missing older history.

MAX/MIN show recorded outdoor/channel/indoor T/H, derived temperature and pressure.
MAX also shows recorded wind/gust/rain rate/UV/solar. Forecast, clock/moon and lower
rain accumulation are deliberately blank; MIN additionally omits those MAX-only
measurements. Long SET clears records on that side. Unknown records show dashes.
Daily reset clears primary/channel records on the next local day when enabled.

The forecast starts partly cloudy while timestamped pressure history builds. Storm
precipitation blinks over steady clouds, following the manual diagram. Trend uses
progressively observed30-day pressure context; training status is diagnostic data.
WiFi flashes fast while pairing/unjoined, slowly until a successful HTTP upload,
then shows steady RSSI. RF bars indicate freshness, independently for each source.
A separate icon shows calibrated console low/no-battery status, including on DC.

On battery, any key wakes Medium for15s. DC transitions select Medium. Alarm runs
up to2min with accelerating cadence; LIGHT snoozes10min, another key stops it, and
holding any key2s cancels snooze without invoking its normal hold action.

Legacy configuration/history layouts remain unchanged. New checkpoints retain
extended/channel records, forecast history and console TOTAL. Saves are throttled
and flushed on orderly shutdown; abrupt power loss can lose the unsaved tail.
Reset save/flush errors cancel reboot; the multi-key reset is not an atomic transaction.

[Complete evidence, capability matrix and qualification limits](docs/panel-capability-audit.md).
Reference: [Ecowitt manual](https://oss.ecowitt.net/uploads/20240924/WS3900%26WS3910%20Manual.pdf), sections3.3–3.5.
