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

GPIO9 also serves the indoor sensor's I²C SDA. Physical test-pad locations are not
yet documented; confirm connections on the PCB before wiring. GPIO18/19 are used
by the display/radio, so this configuration disables USB Serial/JTAG.
Back up the entire 4 MB flash before writing; keep that backup private.

## Notes

This source revision has passed host tests and builds but has not been flashed
or verified on physical hardware.

Keep your own stock backup and a serial recovery route.
The web interface has no authentication: use it only on a trusted local network.
Optional cloud uploads use Ecowitt's service and are subject to its terms.

Source is licensed under GPL-3.0-only, with compatible third-party material listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Run `tests/run.sh` for host checks.
