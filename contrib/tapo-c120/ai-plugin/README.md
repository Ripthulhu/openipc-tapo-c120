# AI Detection Plugin

Native, on-camera person / pet / vehicle and sound detection for the SSC377 Tapo C120.
Uses the stock object and sound models on the IPU. No cloud service or full firmware flash.
This is an experimental board-specific extension, not a general OpenIPC package.

Packages can be staged on FAT-formatted SD cards: the installer creates the
private libc compatibility symlink on flash, not in the archive. Model storage
checks whole mount options, so `errors=remount-ro` does not incorrectly mark a
writable card read-only on the camera's libc. Installation assigns Unix modes
explicitly and does not copy FAT directory permissions onto system directories.

## Status

- **Sound input now uses `/audio.opus`, never `/audio.pcm`.** The October 6
  investigation reproduced a fatal Majestic `master+8dbe2e7` `SIGSEGV` while an
  independent client read PCM with the AI service stopped. The fault maps to
  `evbuffer_expand_singlechain+0x6` in libevent core. The exact buffer-lifetime
  or concurrency cause remains unresolved. This plugin avoids that endpoint;
  it does not repair Majestic for other clients that still request raw PCM.
- Object model: person verified live; bus, cat and dog verified with reference images.
- The stock detector has no established bird class. An optional, bird-only
  YOLOv5n COCO profile has a tested host decoder and SD model library.
  Twenty saved native-inference fixtures matched the reference decoder. On
  October 7, the photo-calibrated model loaded from FAT SD on an SC438HAI C120
  and processed live NV12 frames alongside sound detection and QHD/30 video.
  Bird recognition accuracy, night performance and long-term stability still
  need field validation. It identifies generic birds, not species.
- Sound model: experimental barking, meowing, baby crying and glass breaking.
  Barking and glass-breaking classes performed better than crying/meowing in the
  reference clips. Some meows were mistaken for cries; some cries were missed.
  Stock confidence thresholds intentionally reject many uncertain predictions.
  Before the Opus-input change, barking was confirmed through the live microphone
  at 72-79% confidence with Normal sensitivity and +12 dB analysis gain. A live
  meowing clip produced three confirmed events at 66-70% with those same settings.
  The first Opus-input barking playback peaked at 70.2%, below the unchanged 72%
  Normal threshold, and produced no event. A repeat playback also produced no
  confirmed event. This is not evidence of equivalent recognition accuracy across
  inputs; playback level and distance were uncontrolled. Live Opus-input recognition
  still needs calibration and confirmation, independently of connection stability.
  Do not use this work as a baby monitor or smoke/security alarm.
- One unexplained camera reboot occurred during development. A subsequent bounded
  ten-minute visual trial completed without a restart or growing memory use.
  This is not a claim of long-term stability.
- During sound development Majestic stopped on both cameras without device
  reboots or logged out-of-memory kills. HTTP/1.1 PCM also produced malformed
  chunks with an independent curl client. Requesting unchunked HTTP/1.0 avoided
  that framing error but did not resolve the service crashes described above.
- The earlier HTTP/1.0 PCM version passed five-minute tests before failing later.
  Short tests alone are not evidence of long-term stability.

## Runtime

Open **Camera > AI Detection**, or `/cgi-bin/c120-ai.cgi`.
The page provides enable, confidence and analysis interval settings, boxes over a
JPEG preview, and the last twelve detection events. The preview and inference are
separate samples, so boxes can lag a moving subject.

Analysis defaults to one 800x448 frame every 500 ms. It does not change the main
stream's configured resolution, frame rate or bitrate, JPEG settings, audio,
lighting, or existing motion detection. Notification, AI recording and AI
floodlight actions are opt-in.
Both detectors are off for a fresh installation; existing settings are preserved
on upgrade. The four-field visual-only configuration migrates with sound disabled.

When "Use motion regions" is enabled, the **center** of an object's box must lie
inside at least one configured Majestic motion region. An empty list means the
whole image. Invalid regions pause AI rather than broadening the watched area.

The daemon is low priority and owns only an otherwise unused SCL output port 2.
An incomplete scaler dump or failed settings query leaves ownership unknown,
not lost: cleanup retains its claim and retries before releasing the IPU device.
It never disables a bound port or one whose owner/settings have changed.
Periodic camera-configuration checks have a 50 ms deadline so they do not
block draining the audio and recording streams. Only an HTTP timeout can
reuse a validated configuration for at most ten seconds, while the same
Majestic process and initialized device remain active. Explicit invalid
settings, malformed replies, HTTP errors, shutdown and AP mode do not use
this grace period. The normal memory and analysis-port ownership guards
still apply. This improves recovery under load; it does not guarantee
simultaneous recording, recognition and browser viewing at native resolution.
It pauses and releases the model/feed when the substream is enabled, memory is
low, Majestic is unavailable, or setup AP mode is active. The AP recovery hook
stops it before Majestic and restores the service afterward. No SDK libraries are
replaced globally, no firmware memory is patched, and no LD_PRELOAD is used.

Measured on the development camera: approximately 1.2 MiB process RSS, 7.5 MiB
media-heap use including the model/feed, and roughly 50-57 ms per inference.
Media memory and Linux memory are separate pools; RSS alone omits most AI memory.
The compressed model is read from flash directly, never unpacked into `/tmp`.

Sound recognition uses a second channel on the same IPU device, not another
process. A visual analysis-port conflict does not prevent that sound channel
from starting; shared camera/readiness and memory guards still apply.
It consumes Majestic's local `/audio.opus` stream with libcurl over
HTTP/1.0. The firmware's libogg and libopus decode an analysis-only copy to
16 kHz mono directly, then the stock frontend decimates to 8 kHz. Ogg timing
remains in 48 kHz units, including pre-skip and end trimming. No separate
resampler is linked. The user's microphone sample rate, codec and gain are
not changed. The mel bank stores only its 501 nonzero coefficients, saving
59.5 KiB of resident DSP state and avoiding zero-weight multiplications.
Host tests validate the frontend and Opus timing, but recognition accuracy with
libopus's native-rate decoding still needs controlled, labelled playback tests.
Audio is analyzed in overlapping 2.064-second windows every 240 ms. Two qualifying
windows in the last ten confirm an event. Repeats need a three-second absence and
a ten-second per-category cooldown. Motion regions apply only to visual objects.
Sound categories and stock Low/Normal/High sensitivity are independently selectable.
Analysis gain (0-24 dB, initially +12 dB) compensates for lower capture levels
without altering the recorded/listened-to microphone stream. Reduce it if loud
audio clips or background noise causes false detections. It is not a volume control.
Recognition does not mute or exclude audio played through the camera's speaker.

An October 6 ten-minute combined visual/sound trial on both cameras completed
without a reboot, Majestic restart or audio reconnect. Video averaged 29.999 fps
with all Majestic settings unchanged. AI process RSS was 1.52-1.63 MiB, one thread,
and available Linux RAM stayed above 8.0 MiB. Sound inference was roughly 1 ms
after startup. These are bounded measurements, not long-term guarantees.
The connection reconnects on failure and clears decoding/detection history.
Decoded backlogs longer than one second are discarded. Ogg buffering and packet
assembly are each capped at 64 KiB. CRC errors, missing pages, unsupported channel
layouts and chained streams cause a reconnect, never a raw-PCM fallback. This is
a bounded reader for Majestic's mono live stream, not a general media player.
Analysis audio is never written to disk. An explicitly enabled AI video recording
uses the normal MP4 endpoint and may include the camera's configured audio.

`/etc/c120-ai.json` is persistent. State, request tokens and the twelve-event
history live under `/run`. Analysis saves no pictures or audio. Configured webhook
notifications send event metadata only; recording writes only to mounted storage.

## Detection Actions

**Notifications:** the AI Detection page accepts a Home Assistant/webhook URL or
an ntfy topic URL, optional Bearer token, category filter and per-category cooldown
(60 seconds by default). Confirmed appearances are sent, not every inference frame.
Visual alerts are also re-armed only after that category has been undetected for
the full cooldown: a cat staying in view does not cause repeat alerts just because
confidence briefly falls below the threshold. Every accepted, ROI-filtered object
refreshes presence, including frames that create no arrival event. Person, pet and
vehicle and bird have independent presence gates; cats and dogs share one pet episode.
Another pet entering while a pet is already present is not a separately tracked
arrival. These gates affect notifications only, not event history, recordings or
floodlight presence. Detector pauses are not proof that an animal actually left;
this policy measures time since the last accepted detection, not tracked identities.
Cats and dogs share the stock `pet` category. Sound notifications are opt-in and
retain the experimental recognition limitations above. Delivery uses nonblocking
libcurl, one request at a time, an eight-event queue, a three-second timeout, no
redirects or retries, and discards queued events older than 30 seconds. Failures
are counted in the API without stopping AI. Config changes cancel pending sends.
The **Send Test** button sends a clearly marked test without triggering lights,
recording, or a normal detection event. It uses the saved destination.

Webhook payload:

```json
{"schemaVersion":1,"source":"ai","camera":"hostname","event":{"label":"pet","type":"object","confidence":0.9,"time":1791288000,"id":12345,"bootId":"kernel-boot-id"}}
```

Use a separate random webhook ID per camera to identify cameras even if their
hostnames match. Home Assistant webhooks should be POST-only and `local_only: true`.
The sample below emits `openipc_ai_detection` for further automations and sends an
Android notification with a still. First add a **Generic Camera** in Home Assistant
using `http://CAMERA_IP/image.jpg`, Basic authentication and the camera credentials;
leave the stream source empty. Create `/media/openipc` on Home Assistant. Its
existing media directory must be in `allowlist_external_dirs` for `camera.snapshot`.
Replace the webhook ID, camera entity and exact device notification service:

```yaml
alias: Camera AI notification
triggers:
  - trigger: webhook
    webhook_id: REPLACE_WITH_RANDOM_SECRET
    allowed_methods: [POST]
    local_only: true
conditions:
  - condition: template
    value_template: "{{ trigger.json is mapping and trigger.json.get('schemaVersion') == 1 and trigger.json.get('source') == 'ai' and trigger.json.get('event') is mapping and trigger.json.event.get('label') in ['person', 'pet', 'vehicle', 'bird', 'bark', 'meow', 'cry', 'glass', 'test'] and trigger.json.event.get('type') in ['object', 'sound', 'test'] and trigger.json.event.get('id') is number and trigger.json.event.get('bootId') is string and trigger.json.event.get('confidence') is number and 0 <= trigger.json.event.confidence <= 1 }}"
actions:
  - action: camera.snapshot
    continue_on_error: true
    target:
      entity_id: camera.YOUR_CAMERA
    data:
      filename: "/media/openipc/cam1-{{ trigger.json.event.label }}.jpg"
  - event: "{{ 'openipc_ai_notification_test' if trigger.json.event.type == 'test' else 'openipc_ai_detection' }}"
    event_data:
      camera: cam1
      label: "{{ trigger.json.event.label }}"
      confidence: "{{ trigger.json.event.confidence }}"
  - action: notify.mobile_app_YOUR_PHONE
    data:
      title: Camera AI
      message: "{{ trigger.json.event.label }} detected"
      data:
        image: "/media/local/openipc/cam1-{{ trigger.json.event.label }}.jpg?event={{ trigger.json.event.bootId | urlencode }}-{{ trigger.json.event.id | int }}"
        tag: "openipc-cam1-{{ trigger.json.event.label }}"
        group: OpenIPC cameras
mode: queued
max: 10
```

The still is fetched when Home Assistant receives the event, not taken from the
exact IPU inference frame. No image upload, additional stream, or image storage
is added to the camera. Use a different filename prefix for each camera. The
latest still per category is overwritten, matching each notification's tag;
at most nine files per camera are kept, including tests. The event query prevents
reuse of a previously cached attachment. The phone downloads the attachment on
receipt, so these files are not a historical image archive: a delayed/offline
phone may receive the latest image for that category instead of an older event.
Home Assistant's Generic Camera may return its last cached still on a camera
fetch failure; its normal error log identifies that condition.

Use authenticated `/media/local/...` URLs, **not** the publicly served `/local/...`
directory. The Companion app supplies authentication for media attachments; see
the [official attachment documentation](https://companion.home-assistant.io/docs/notifications/notification-attachments/).
Home Assistant must be reachable through the phone's configured server URL.
Snapshot failures do not prevent the text notification. Isolated IoT networks
need narrowly scoped camera-to-HA webhook access and HA-to-camera TCP 80 access,
not internet exposure. Keep camera passwords and webhook IDs out of firmware
packages, repositories and logs.

**Floodlight:** Lights offers Motion or AI objects as the trigger, with independent
Person / Pet / Vehicle / Bird selection. Unsupported categories are disabled for
the active model. Existing sustained-detection delay, night gate,
time after the last detection and manual override remain in effect. The GPIO
helper reads a versioned, timestamped `/run/c120-ai-light.signal`; stopped/stale AI
cannot keep driving the lamp. Presence signals already obey confidence and motion
regions. These controls require the matching `c120-eventd` and Lights CGI updates.

**AI recording:** independently selects object/sound categories and continues for
30 seconds after the last qualifying detection by default. It writes the existing
main-stream `/video.mp4` directly to the native `records.path`, producing `AI-*.mp4`
files visible in Recordings. No second encoder or shell/FFmpeg process is used.
Object triggers obey motion regions; sound triggers have no spatial regions.
It uses `records.split` and `records.maxUsage`, requires writable SD/USB or mounted
network storage, and refuses root flash and RAM. Existing native recording takes
priority: AI recording pauses if `records.enabled` is true, rather than stopping
or duplicating continuous/motion recordings. Enabling this setting does not alter
Majestic configuration. Configure storage on the Recording page, with native
recording off when AI is the desired trigger.

This external trigger bridge starts at the next MP4 fragment/keyframe and has
**no pre-roll**. Unlike native motion recording, it allocates no video history in
RAM. At the storage limit, the catalogue worker deletes the oldest closed,
catalogued recordings under `records.path` until space is available again.
Native recording keeps its own Majestic retention policy. Open files, partial
clips, models and unrelated files are never eviction candidates. Use a dedicated
recording directory: all imported MP4 recordings below it participate in retention.

Majestic `0ea3123` limits the HTTP MP4 muxer to a 1 MiB GOP. At 10,000 kbit/s,
use a 0.5-second keyframe interval (`cli -s .video0.gopSize 0.5`); the old C120
two-second interval overflows that limit and drops video. This is now the C120
first-boot default, but an existing configuration is preserved by upgrades and
the plugin does not change it automatically. Resolution, frame rate and bitrate
stay unchanged. Increasing `system.buffer` or reducing `records.fragmentMs`
did not fix the HTTP path in cam4 tests. Recheck GOP size if bitrate is raised.
Normal stops trim the last incomplete MP4 fragment before publishing the clip.
Failed streams are discarded; files left by a power failure keep their `.partial`
suffix and are not advertised as completed recordings.
Changing settings or entering recovery AP mode closes the current AI clip.
Camera streaming quality settings are never modified.

## Recording Automation API

See [RECORDINGS-API.md](RECORDINGS-API.md) for polling, durable completion webhooks,
AI summaries, still images, authentication, retention, and a Python download client.
The machine-readable contract is [recordings-openapi.yaml](recordings-openapi.yaml).
The AI plugin can be installed independently of the optional Lights/Wi-Fi plugins;
it integrates with their recovery AP lifecycle when they are present.

## SD Model Library

**AI Detection > Model Library** lists the protected built-in Stock detector and
compatible models on a mounted SD card. Upload the small profile JSON and compiled
`.img` together, then select the detector and save. Selection uses the profile's
confidence and NMS defaults; both are adjustable. Stock supports person/pet/vehicle;
the bird profile exposes only bird. Existing action filters are retained when
unsupported, not silently erased. Add `bird` to your webhook receiver's category
allowlist before enabling bird notifications.

The library uses `<SD mount>/c120-ai/models/<id>/profile.json` and `model.img`.
Only a real `/dev/mmcblk*` mount below `/mnt/` is accepted, on vfat, exfat,
ext2/3/4 or f2fs. No root-flash or RAM fallback exists. You may copy complete model
directories onto the card yourself. IDs are immutable: use a new ID for a new
revision. The library allows 16 compatible models, each at most 8 MiB; uploads
reserve another 1 MiB of free storage. Incomplete uploads can be cancelled.
The selected, active and rollback models cannot be removed through the API.

Bird preprocessing keeps 16 horizontally resized rows instead of a full
intermediate image. At the 800x450 camera feed this saves approximately
407 KiB for 320 input or 488 KiB for 384 input, with unchanged two-pass
rounding. Synthetic RGB/NV12 fixtures are byte-identical to the earlier
full-image implementation, including padded rows and portrait/upsampled inputs.

The tested one-class bird profiles are
[320x320](bird-presence-profile.json) and
[384x384](bird-presence-384-profile.json). Both use RGB NHWC S16 input and three
raw heads with 18 logical channels (three anchors times six values). Their fixed
decoder contracts are `yolov5n-bird-presence-v1` and
`yolov5n-bird-presence-384-v1`; profiles default to confidence 0.70 and NMS 0.45.
Bird replaces person/pet/vehicle analysis, not an additional resident model.
Stock remains the default. The model images must be supplied separately on SD;
profiles alone are not executable models. Camera recognition accuracy remains
unmeasured, despite successful loading/inference and offline decoding checks.

Media admission uses the queried tensor sizes after channel creation, preserving
space for the feed and a safety reserve. Post-load configuration reads retry
only HTTP timeouts, at most three 2.5-second attempts with 0.2-second gaps;
invalid regions, an enabled substream, camera ownership changes, setup AP and
shutdown still reject the load. This is not a general retry for invalid models.

Upload chunks are written directly to the card, at most 3072 decoded bytes per
request. Length and SHA-256 are checked before an atomic directory rename. Paths,
symlinks, profile keys and tensor descriptors are validated. Only upload trusted,
target-compatible compiled models: a matching hash proves integrity, not that
an arbitrary vendor model is safe or accurate.

The bundled [bird-coco-profile.json](bird-coco-profile.json) identifies the current
private photo-calibrated model, 4456832 bytes, SHA-256
`9dc16e8480a099b5ab355491fea9f86ebd132e6b12d37e9d40e2b5d1283323ab`:

```json
{"version":1,"id":"bird-coco-realcal","name":"Bird - YOLOv5n COCO (photo calibrated)","target":"ssc377-ipu-v6","decoder":"yolov5n-coco-bird-v1","modelBytes":4456832,"sha256":"9dc16e8480a099b5ab355491fea9f86ebd132e6b12d37e9d40e2b5d1283323ab","confidence":0.25,"nms":0.45}
```

`decoder` names a fixed, checked contract, not a generic model importer. This
profile requires RGB NHWC S16 320x320 input and three raw YOLOv5 heads at
40x40x255, 20x20x255 and 10x10x255. It uses standard YOLOv5 anchors/strides,
80-class argmax followed by bird class 14 filtering, sigmoid objectness times
class confidence, bird-only NMS, and inverse letterboxing. Input uses padding 114,
antialiased bilinear resizing, float32/255 and truncating S16 quantization.
Actual camera NV12 frames use BT.601 limited-range RGB conversion; this path
runs on the SC438HAI camera, but recognition accuracy still needs live validation.
Arbitrary architectures and other COCO classes are
not supported by this profile. Cropped multi-pass inference is deferred until
single-frame resource use and camera accuracy are measured.

Switches release the old visual channel before loading the new one; they never
keep two object models resident. Sound remains on its existing shared IPU device.
If loading fails, the previous model or Stock resumes and the API/UI reports the
requested model separately from the actual active one. A missing card retains
the selection and can retry when the card reappears; removed active media falls
back to Stock. Read-only media can be listed/loaded but not uploaded or removed.
There is no device reboot or video-quality change. A three-second model warm-up
suppresses new notification arrivals during switching. This is not identity
tracking and cannot prove that a subject left during a detector pause.

Changing confidence, interval or ROI policy reloads without recreating the AI
pipeline. Sound/enable changes may recreate it. The October 7 camera was restored
to Stock after its tests, with the bird model left available on SD.

### SC438HAI SD Trial (2026-10-07)

The native upload start/finish API validated the profile, length and SHA-256;
the image bytes were streamed directly over SSH to its staging file on SD.
This tests native commit validation, not the browser chunk-upload transport.
Bird inference was typically 31-39 ms, with roughly 6 MiB Linux memory and
4-4.6 MiB media memory available. A bounded two-minute run advanced both visual
and sound counters without a service restart or audio reconnect. An independent
RTSP probe read 120 H.264 frames at 2560x1440, reporting 30 fps. All Majestic
configuration remained unchanged; these observations are not a long-term soak.

Earlier tests exposed intermittent configuration-read failures during model
loading, one abandoned analysis feed, and one Majestic exit without a retained
crash signature. The ownership check now ignores ABI padding, and the model-load
configuration check allows 2.5 seconds instead of the regular 700 ms polling
deadline. Failure messages identify the failed load stage. The final bird/stock
switch sequence passed without a Majestic restart or device reboot. Some audio
reconnects occurred in other trials. The earlier Majestic exit's cause is not
established; do not interpret the bounded final checks as proof it is fixed.

## API

`GET /cgi-bin/c120-ai-api.cgi?view=status` returns the same live status and
CSRF token without `config` or `models`. It avoids reading settings and
enumerating the SD model library. Authentication, recovery-AP exclusion and
stale-daemon checks are unchanged. The UI uses this for its one-second polls;
full data is fetched on load/save, model operations, model changes, return
to a visible tab and every minute. Unsaved form edits are preserved.

`GET /cgi-bin/c120-ai-api.cgi` returns schema version 2, current `objects`, recent `events`, `status`,
`running`, `frames`, resource use, `config` and a per-boot `csrf` token. It uses the
same authentication as Majestic's WebUI. Objects contain a normalized `[x,y,w,h]`
box, category, model class ID and confidence. Events are debounced category
appearances, not tracked identities. Polling does not provide a durable event queue.
Sound fields are `sounds`, `soundStatus`, `soundRunning`, `soundFrames` and
`soundInferenceMs`; `soundReconnects` counts unexpected microphone reconnects.
`soundSource: "opus"` identifies the safe-input version. `soundRunning` becomes
true only after audio has actually been decoded, not merely after connecting.
Confirmed events share the twelve-entry history and include
`type: "sound"` or `type: "object"`; sound labels are `bark`, `meow`, `cry`, `glass`.
They also include `bootId` and an ID unique within that device boot, including
across AI daemon restarts. `notifications` and `recording` report action state and
delivery/clip/error counters.
`notifications.suppressed` counts arrival events skipped by cooldown or ongoing
visual presence; it is not a failed delivery. `config.notifications` and
`config.recording` hold the persistent action settings; old eight-field clients
preserve them on save.
POST `{"csrf":"TOKEN_FROM_GET","testNotification":true}` requests a test on an
enabled destination. Repeated test requests are rate-limited to one per ten seconds.

POST the complete configuration plus the returned token as JSON to the same URL:

```json
{"enabled":true,"confidence":0.6,"intervalMs":500,"motionRegions":true,"soundEnabled":true,"soundSensitivity":1,"soundGainDb":12,"soundClasses":["bark","meow","cry","glass"],"csrf":"TOKEN_FROM_GET"}
```

Only authenticated POST changes settings. No side-effecting GETs or token in URLs.
The API is unavailable in the open recovery AP. API errors use HTTP 400/403/405/503;
settings are validated before an atomic write. Inactive/stale processes do not
report live detections. The API does not accept shell commands or arbitrary paths.

`config.model` and `config.nms` persist selection and the bird overlap threshold.
`activeModel`, `activeModelName`, `requestedModel`, `previousModel`, `modelFallback`
and `modelError` distinguish successful activation from fallback. `models` reports
the mounted storage, compatible items and unfinished uploads. Older ten-field
clients preserve model settings on save. The API validates an available model's
hash before accepting a new selection; it does not claim activation succeeded
until the daemon reports it.

Model requests use the same authenticated POST, CSRF token and model `id`. `modelAction` is
`start` (with `profile`), `chunk` (with `id`, `offset`, base64 `data`), `finish`,
`cancel` or `remove` (with `id`). These do not enable a detector or change the
selected model; use the normal configuration save after a successful upload.

## Build And Install

Requires the matching OpenIPC musl cross-toolchain, its json-c/libcurl/libogg/libopus/zlib, the
verified Infinity6C IPU library, and the owner's stock model dump:

```sh
sh build.sh FIRMWARE_BUILD IPU_LIBRARY STOCK_MODELS OUTPUT
```

The result is `OUTPUT/../c120-ai-plugin.tgz`. The build validates the three critical
proprietary input hashes; provenance is below. No model is fetched automatically.
Do not stage this package in camera RAM. Copy/extract it on a **mounted SD card**,
then run `sh install.sh /mnt/mmc/ai-backups` from the package after creating the
backup directory. `install.sh` requires an SD/USB filesystem, backs up replaced
files there, and conservatively reserves full-package size plus 512 KiB of flash.
For low-space upgrades, use an off-device SSH deployment that backs up existing
files and streams only changed files directly to their final locations.

Service: `/etc/init.d/S97c120-ai {start|stop|restart|reload}`. Graceful stop must
finish before updating the executable or using the standalone IPU probe.
`c120-ai --trial 60` runs bounded inference without enabling it persistently;
stop the service first. `c120-ai --self-test` checks validation, regions and labels.
`c120-ai --trial-sound 60` enables both detectors only for the bounded trial.

`sh uninstall.sh` stops AI, removes only plugin-owned files and menu/AP hooks,
and retains `/etc/c120-ai.json`. It does not revert other camera changes.
An interrupted update can be recovered using the off-device/SD backup.

### Host Checks

For all host-only checks, run `sh contrib/tapo-c120/check.sh` from the repository
root. It compiles the daemon's validation self-test without loading vendor
libraries or models, and includes the actions, Opus and DSP checks described
below. The dependency list is in the [C120 guide](../README.md#build-and-test).
These host checks do not connect to cameras or change configuration.
`python3 test_models.py` additionally tests library storage/security boundaries
and the raw bird decoder. `--golden DIRECTORY` checks the private native-inference
arrays and Pillow preprocessing reference; it requires Pillow and NumPy. Those
private fixtures and model binaries are not included in the repository.

### Camera Checks

The following integration checks are separate and connect to the specified camera.
Provide the password in `C120_SSH_PASSWORD`.

`--exercise` deliberately enables sound. The test first requires `soundSource`
to identify this Opus-input version before exercising any controls.

```sh
python test_api.py https://camera.example --exercise
python test_api.py https://camera.example --soak-seconds 300
```

This briefly disables/re-enables AI, restores the original settings, tests auth,
CSRF and invalid settings, and checks that Majestic configuration is unchanged.
The optional soak requires uninterrupted inference and no new audio reconnects.
`python3 test_actions.py` checks notification filtering, cooldown, slow/failed
destinations, bounded queues, recording guards and a timed synthetic playable MP4
on Linux with gcc, libcurl/json-c development headers and FFmpeg. Recording tests
use a test-only filesystem shim; that exception is not compiled into firmware.
`python3 test_sound.py` builds and compares the native audio frontend to a NumPy
reference on the host, including streaming cadence at five sample rates. It needs
gcc, curl and NumPy and uses synthetic signals only. `soundScores` in the API are
the latest seven model outputs in stock order; `soundLevelDbfs` is the current
analysis level after gain. These are diagnostics, not confirmed events.

The Opus-input regression uses synthetic audio, native libogg/libopus, and checks
fragmented input, gain, pre-skip/end trimming, CRC/sequence failures, buffer caps
and fresh decoder state on reconnect. Run with the development headers installed:

```sh
gcc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    test_opus.c opus-input.c -logg -lopus -lm -o /tmp/test-opus
/tmp/test-opus
```

The October 6 version passed this regression under host ASan/UBSan and ARM QEMU
using the target firmware libraries, plus the DSP reference test. Both cameras
passed API control tests and ten short-lived additional Opus clients each,
alongside the AI reader, without restarting Majestic or reconnecting AI. No camera audio was
saved during the live tests.

## Provenance

These proprietary artifacts are owner-provided/runtime dependencies, not claimed
as open-source or proposed for inclusion in upstream OpenIPC. Redistribution rights
must be checked separately. They remain private to `/usr/lib/c120-ai` at runtime.

| Input | Origin / SHA-256 |
| --- | --- |
| `obj_detection.sim_sgsimg.img` | Owner's C120 stock 1.4.1 dump; `07c8ffa24e9d028bf4c5bc1f5656f5a4baa958f0d5063963917e0b3f714cfb7d` |
| `sed.sim_sgsimg8k.img` | Owner's C120 stock 1.4.1 dump; `72bdb31f95b892155b84c212c61e4774b85ccc314403b797dafea67d3850e8aa` |
| `libmi_ipu.so` | `johnchia/sigmastar-lib`, commit `cd3c621fdb9e60e7f567ec6146af44ebeddc2630`, `infinity6c/lib/1230/uclibc/9.1.0/libmi_ipu.so`; `e78f2d9ad9000ce6adedef7686eb1d30625b2259bd3c86d0c2ab2a77ab1a47e3` |
| Other MI libraries | The matching firmware's `sigmastar-osdrv-infinity6c/files/lib` |
| uClibc compatibility | The matching toolchain's SDK compatibility library |
| Frame ABI | See `../ai-probe/frame-abi.h` and its source/license attribution |
| KissFFT | `mborgerding/kissfft` commit `8f47a67f595a6641c566087bf5277034be64f24d`, BSD-3-Clause |

The stock descriptor groups object classes 2/3/7 as vehicles; 0 is person;
cat and dog fixtures both returned class 4. Class 8 is deliberately not named or
exposed without verification. This is not a COCO label table.

Stock sound labels were traced in the owner's main executable at `0xc9d28`:
0 bark, 1 meow, 2 baby cry, 3 glass breaking. The remaining outputs are not the
other three user-facing visual categories. The feature path uses 8 kHz,
512-sample periodic Hann windows, 160-sample hops, 64 HTK mel bands, 101 frames,
log power and int16 quantization. DC/Nyquist use magnitude, matching the stock
frontend's endpoint quirk. KissFFT replaces the original FFT implementation.
The native streaming frontend matched the independent NumPy reference within
one quantization unit for silence, tone and seeded noise. Fifteen ESC-50 reference
clips were tested using 195 overlapping windows on the IPU. Test clips are not
distributed with the plugin and retain their original dataset/source licenses.
