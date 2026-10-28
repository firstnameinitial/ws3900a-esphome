# Display and buttons

| Button | Normal display | Other modes |
|---|---|---|
| SET | MAX → MIN → alarm hour → alarm minute → MAC → Normal | Hold from Normal: settings; SET advances; LIGHT exits |
| TEMP | Outdoor → Feels-like → Dew → available CH1–CH8 → Auto → Outdoor | MAX/MIN: temperature / feels-like / dew |
| RAIN1 | Rate / Event; right-hand bar follows this row | — |
| RAIN2 | Daily / Weekly / Monthly / Yearly / Total | — |
| WIND | Speed / Gust / Direction / 10-minute direction | MAX: speed/gust; Alarm: arm/disarm |
| + | Month/day / weekday/day | Settings/alarm: increase |
| − | Relative / absolute pressure | Settings/alarm: decrease |
| Hold − | UV/solar / sunrise/sunset | — |
| LIGHT | Backlight level | Return to Normal; alarm snooze |

Held-SET menu order: Beep → RST HILO (stored only) → 12/24h → hour → minute →
year → month → day → pressure unit → relative-pressure offset → temperature unit →
wind unit → rain unit → solar unit → hemisphere → CO₂ calibration (display only).
Hour/date fields are read-only because SNTP owns the clock. Sensor re-registration
and factory-reset actions are currently log-only stubs.

Auto scrolls measured outdoor and enabled, recently received channels every five
seconds. The channel digit and CH marker identify extra sensors; derived views
use FEELSLIKE or DEW. The fixed segment glass has no arbitrary sensor-name field.

The rain bar follows the upper Rate/Event row. Daily accumulation is independent;
a dry rate can have an empty bar alongside a positive daily total. UV0 has an empty
bar; successive filled levels end at UV3, 6, 9 and 12.

MAX/MIN record outdoor/indoor temperature and humidity, feels-like, dew and absolute
pressure. MAX additionally records wind speed, gust, rain rate, UV and solar.
Relative pressure applies the current offset to the stored absolute-pressure
record. Long SET in MAX/MIN clears that side. Unknown records show dashes. These
are all-time records; the stored daily-high/low-reset setting is not implemented.

Existing outdoor T/H, gust and rain-rate records migrate. Additional fields start
with newly observed data. A separate versioned preference retains the extended
records without changing legacy configuration/history layouts. After running
older firmware, original fields reconcile from its legacy history; older firmware
cannot record the additional fields. Saves are throttled to one minute and flushed
on orderly shutdown; abrupt power loss can lose the unsaved tail.

Reference: [Ecowitt WS3900/WS3910 manual](https://oss.ecowitt.net/uploads/20240924/WS3900%26WS3910%20Manual.pdf), sections 3.3–3.5.
