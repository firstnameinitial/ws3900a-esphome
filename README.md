# WS3900A ESPHome firmware

**Beta. Initial installation requires opening the case and serial flashing;
this can damage or brick your device. No warranty.**
Independent project, not affiliated with or endorsed by Ecowitt or Fine Offset.

Replacement firmware for the Ecowitt WS3900A console (ESP32-C3). Receives multiple
Ecowitt / Fine Offset sensors and sends readings to Home Assistant through ESPHome
or the Ecowitt integration. Includes the console display and buttons, an on-device
web interface for sensor and upload settings, and optional Ecowitt cloud uploads.

The main reason for this firmware is local Home Assistant access to multiple
sensors with their own identities and settings. Ecowitt uploads still have the
protocol's fixed channel limits.

## Build

Use ESPHome 2026.6.2. Copy `secrets.example.yaml` to `secrets.yaml`, enter your own
settings and location, then run `esphome compile ws3900a.yaml`. After initial
installation, update with `esphome upload ws3900a.yaml --device <device-ip>`.

## Serial connections

Use a **3.3 V UART adapter** and common ground:

| Connection | ESP32-C3 signal |
|---|---|
| Adapter TX → device RX | GPIO20 / UART0 RX |
| Adapter RX ← device TX | GPIO21 / UART0 TX |
| Ground | GND |
| Boot strap | GPIO9; ground during reset, then release |

GPIO9 also serves the indoor sensor's I²C SDA. GPIO18/19 are used
by the display/radio, so this configuration disables USB Serial/JTAG.
Back up the entire 4 MB flash before writing; keep that backup private.

## Notes

Keep your own stock backup and a serial recovery route.
The web interface has no authentication: use it only on a trusted local network.
Optional cloud uploads use Ecowitt's service and are subject to its terms.

Source is licensed under GPL-3.0-only, with compatible third-party material listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Run `tests/run.sh` for host checks.

Console modes and record behaviour: [Display and buttons](DISPLAY.md).

## Rain and status recovery

The WS90 `rain_state` signal is a **wet flag**, not proof of measured rain. Use
measured rain rate for rain notifications; a damp sensor may remain flagged in sunshine.

The primary rain sensor's rolling 24-hour, one-hour and ten-minute windows are saved
with their matching raw/calibrated baseline and calendar totals in a separate,
versioned NVS checkpoint. Recovery waits for a valid clock and ages out rainfall
through downtime. Until then, rolling totals/rate are omitted from HTTP uploads and
the dashboard, and native rate is unknown. Existing config and calendar preference
layouts are retained for rollback. A checkpoint that disagrees with newer legacy
calendar data (for example after running an older firmware) is discarded rather than
replacing that newer history. Other rain sensors retain independent RAM windows.

Saves keep the existing one-minute rain-write limit, save immediately on calendar
rolls, and flush on orderly shutdown. Sudden power loss can lose the unsaved tail;
recovery is from the last valid checkpoint, not a complete historical archive.
The first upgrade cannot reconstruct rolling rain that earlier firmware never saved.
Counter increments first observed after an outage have no measurement timestamp and
are attributed to receipt, as in ordinary RF gaps.

`/state` uses a bounded JSON buffer built on the main loop; `/health` returns a smaller
snapshot including reset reason, upload status and recovery flags. Busy/low-memory
requests return HTTP 503 for retry. Uploads keep their separate worker task, and
configuration changes remain queued for the main loop. Dashboard saves use the IDF
server's supported `j` form transport and synchronise the native switches.

Run `tests/run.sh` and `node tests/dashboard_test.js`. Source/host/build verification
does not replace an on-console reboot, power-cycle and LCD/RF/upload acceptance check.

The complete button/segment comparison, intentional sparse pages and qualification
limits are in [the panel capability audit](docs/panel-capability-audit.md).
System → Console configures daily records, the lighting schedule, rain calendar
and traditional/piezo preference. New checkpoints preserve the original preferences.
The forecast starts partly cloudy and learns from real timestamped pressure readings;
its trend baseline progressively fills over thirty observed days.

Dashboard checks also include `node tests/dashboard_panel_test.js`.

Per-sensor API name/channel settings now survive restarts; see
[Sensor settings](docs/sensor-settings.md) for partial updates and automatic-channel reset.
