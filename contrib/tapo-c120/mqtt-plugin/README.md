# OpenIPC MQTT Plugin

Optional native MQTT 5 helper for Majestic and the AI plugin. No extra encoder,
shell-command service, database or Python runtime. Video resolution, frame rate
and bitrate are untouched. The stripped ARM helper is approximately 52 KiB.

## Install

```sh
sh build.sh /path/to/openipc-build /path/to/package
```

The build hash-checks official Mosquitto 2.0.22 client source and statically links
its single-threaded, non-TLS client. JSON-C and curl come from the firmware.
Extract the verified `openipc-mqtt-plugin.tgz` on persistent storage and run its
`install.sh BACKUP_DIRECTORY` as root. Existing settings are preserved and
replaced files backed up. FAT staging is supported. No firmware flash is needed.

Configure `/etc/openipc-mqtt.json` (root-owned, mode `0600`) and restart
`/etc/init.d/S98openipc-mqtt`. All eight fields are required:

```json
{
  "enabled": true,
  "host": "192.168.1.1",
  "port": 1883,
  "id": "001122334455",
  "name": "Camera 1",
  "username": "camera_001122334455",
  "password": "a-dedicated-broker-password",
  "url": "https://camera.example"
}
```

Use a unique, stable ID, preferably `fw_printenv -n ethaddr` without colons.
Do not change it after discovery. `url` is the local HTTPS configuration link;
an empty string is permitted. Credentials never enter discovery/state messages
or command arguments. The example shipped configuration is disabled.

**Trusted LAN/VPN only:** authenticated TCP port 1883 is supported, not TLS.
Credentials/detections are unencrypted on that network. Never forward these
ports to the Internet. Give each camera its own broker account and topic ACL:

```text
user camera_001122334455
topic read homeassistant/status
topic read openipc/001122334455/set/+
topic write openipc/001122334455/#
topic write homeassistant/device/openipc_001122334455/config
```

HA/automation clients need read access to state/events and write access to
commands. Use MQTT 5 and HA's default discovery prefix `homeassistant`. One
device announces 27 entities, reannounces on reconnect/HA birth, and has a
retained online/offline last will. The existing setup AP helper stops/restarts
MQTT to preserve low-memory recovery. Missing/stale services make affected
entities unavailable. Install the current AI plugin for AI/sound and recording.

## API

Topics below are relative to `openipc/<id>/`:

| Topic | Payload / purpose |
| --- | --- |
| `state` | Retained JSON: detection booleans, settings, recording/status and latest clip ID |
| `availability` | Retained `online` / `offline` |
| `event/detection` | Non-retained AI event: `event_type`, label, confidence, per-boot ID and time |
| `event/recording` | Non-retained completed clip metadata, `event_type: complete`, media/still URLs |
| `result` | Non-retained `{command, accepted, error?}` |
| `set/motion_enabled`, `set/ai_enabled`, `set/ai_regions`, `set/sound_enabled`, `set/recording_enabled` | Exactly `ON` / `OFF` |
| `set/motion_sensitivity` | Integer 0-8 |
| `set/ai_confidence` | Integer percent 5-95 |
| `set/sound_sensitivity` | Integer 0-2 |
| `set/record_seconds` | Integer 1-600 |
| `set/record_categories` | `People`, `Pets`, `People and pets`, `Vehicles`, `Birds`, `People, pets and vehicles`, `All detections` |
| `set/record_clip` | Exactly `PRESS` |
| `set/night`, `set/floodlight` | Exactly `ON` / `OFF` |

Commands must be **non-retained**. Stored/live retained commands, malformed or
oversized payloads, unknown settings and shell commands are rejected. Identical
settings do not rewrite flash. The HA category selector displays `Custom` for
other combinations; configure those using the existing AI page/API.

Presence sensors: person, pet, vehicle, bird, bark, meow, cry, glass. Motion uses
the native region-filtered counter with a three-second hold. AI uses accepted
objects and configured motion regions. Sound has no spatial region. Stock AI
reports **pet**, not cat/dog species. Bird detection needs the selected bird model.

Night commands do not disable automatic day/night monitoring, which may change
mode again. Floodlight commands retain the existing manual override semantics.
Configure automatic light timers on the Lights page.

Automatic recording lasts until `record_seconds` after the last matching
detection. `record_clip` requests that duration without enabling automatic
recording. Retriggering extends the existing recorder's deadline. Native
recording takes precedence; writable SD/USB storage and a healthy MP4 stream
are required. There is no RAM/flash fallback. Watch status and completion, not
just `accepted: true`. Existing oldest-first closed-clip retention is used.
For older C120 settings, use the AI plugin's documented 0.5-second keyframe
interval before recording; a two-second GOP can overflow the MP4 muxer. On
low-memory cameras, simultaneous live viewing and recording may exhaust the
shared live buffer; shortened/failed clips must not be treated as a full-duration
success. Monitor recording status and the returned duration, not just completion.

New manually started clips have source `manual`; an already-active AI clip
keeps source `ai`. No classification is invented. The AI API also accepts a
CSRF-authenticated POST with `recordClipSeconds` (1-600) and returns 202.

MQTT live events are best-effort QoS 0: no historical replay after reconnect.
The runtime recording signal holds the latest completion, so rapid closes can
coalesce. Use the durable [recording API](../ai-plugin/RECORDINGS-API.md) and its
cursor to catch up or retrieve every clip. Video/still URLs use authenticated
HTTP, not MQTT; footage/room audio is never buffered into broker messages.

## Home / Away

`home-assistant-home-away.yaml` is an optional HA package for a persistent mode
helper and MQTT selector. First use defaults to Home; subsequent restarts
restore the last mode. Keep an existing UI-created helper rather than defining
a duplicate YAML helper. Add this condition to phone notification automations,
including existing webhook-based ones:

```yaml
conditions:
  - condition: state
    entity_id: input_select.camera_notification_mode
    state: Away
```

**Home silences gated phone notifications; Away allows them.** Apply the
condition only to cameras that should follow the mode. For a camera that must
notify in both modes, omit that condition and keep its event filters/cooldowns.
Detection, recording,
event publication and automatic lights continue. Occupancy is not inferred
automatically; an HA person/zone automation can set the helper if desired.
The shared MQTT selector uses `openipc/notifications/mode/set` (`Home` / `Away`,
never retained) and retained state `openipc/notifications/mode/state`. Camera
accounts do not need access to those topics.

## Tests / Disable

`python3 test_mqtt.py` uses a real loopback-only test broker and synthetic camera
APIs. Requires gcc, JSON-C/curl/Mosquitto headers and broker/client tools. No
camera or credentials are used. It is included in the C120 regression suite.

To disable, stop `S98openipc-mqtt` and set `enabled` false. To remove HA discovery,
publish an empty retained payload to `homeassistant/device/openipc_<id>/config`
using an authorized account. Revoke the camera broker account when uninstalling.
