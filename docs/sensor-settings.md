# Sensor settings API

POST `/config` as a form field `j` containing JSON. Sensor identity is `fam` plus
hexadecimal `id`. Only supplied fields change.

```json
{"sensors":[{"fam":"WH31","id":"001234","name":"Attic","ch":2}]}
```

| Field | Meaning |
|---|---|
| `name` | Persisted friendly name, up to15 UTF-8 bytes; `""` restores the default |
| `ch` | Persisted channel override: WH31 uses1–8; other families allow0–16 |
| `channel_auto` | `true` clears the channel override and follows RF channel data |
| `en` | Enable/disable; independent of name/channel settings |

`channel_auto:true` takes precedence over an effective `ch` echoed from `/state`.
Omitting `en` preserves its current value; enable-only changes preserve overrides.
Names are metadata; the fixed LCD still identifies extra sensors by CH number.

```json
{"sensors":[{"fam":"WH31","id":"001234","name":"","channel_auto":true}]}
```

The separate checksummed/versioned override preference holds24 identities. It is
loaded before RF processing and included in application reset. RF discovery does
not write flash. Missing, corrupt or unknown-version data falls back to physical
channels and default names; the existing disable list remains authoritative.
Failed writes retain the previous runtime settings and restore pending cache data.

`cfg.sensors` reports effective channel, name, enable and automatic-channel status.
Diagnostics report override count, boot restoration and last-save success. Overrides
can appear before the transmitter is heard; this does not establish live reception.
Older firmware ignores this new preference; its transient name/channel edits do not
update it. Reinstalling this firmware restores the last overrides it saved.
