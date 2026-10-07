# C120 Verification History

Dated observations and historical image hashes. These do not describe the bytes
of a freshly rebuilt image or guarantee long-term stability. Current installation
and source defaults are in [README.md](README.md).

## Audio Verification, 2026-10-05

Both live cameras received this pin hook and saved speaker configuration on
2026-10-05. At level 80, their microphones captured the generated
660/880/660 Hz sequence, with zero clipped samples, GPIO43 high during
playback and low afterward. The owner confirmed `.126` was loud enough and
clear; `.196` was not nearby for a listening check. Initial quiet-room
microphone measurements were about -56 dBFS RMS on both, with negligible DC
offset and no clipping. Native capture was 48 kHz with no discarded frames;
microphone gain and rate were not changed. These are short checks, not a
claim of full-duplex echo cancellation or calibrated acoustic levels. This
SigmaStar Lite schema exposes no noise reduction, AGC or high-pass controls,
so none were added as unsupported settings.

## Motion And AI Actions, 2026-10-05/06

On 2026-10-05 this plugin was installed on both live cameras, with the feature
disabled on `.126` and enabled on `.196`. Both retain a three-second motion
delay and 30-second duration. Before adding the qualification delay, the owner
confirmed physical motion/light-on/timed-off tests on both; recorded GPIO12/13
stayed high throughout, while only GPIO14 switched. The added delay and
out-of-region rejection passed simulated native-metric/GPIO tests; these are
not a second physical movement test. Both current region lists are empty and
were preserved. Final UI checks showed the saved off/on preferences and 3/30
values at desktop and 390-pixel mobile width without form overflow or console
errors. Complete native settings, Majestic PIDs, and boot IDs remained unchanged.
Both sensors reported 30 fps at their saved 2560x1440/6000 kbps settings.
The existing single-thread helper used 80/88 KiB RSS in the final sample.

On 2026-10-06 both cameras received the AI action integration. `.126` keeps
automatic floodlighting off; `.196` now selects person-only AI, with the same
three-second delay, 30-second tail and night-only gate. Home Assistant webhooks
send Pixel notifications with private stills captured on receipt. Independent
AI recording controls are installed but remain off until mounted storage is
configured. Stock cats and dogs share the `pet` category. See the AI plugin
README for payloads, storage guards, native-recorder priority and limitations.

## Native Helper Memory, 2026-10-04

On the two cameras checked on 2026-10-04, helper RSS fell from 928/964 KiB to
60-64 KiB, and proportional memory from 198/229 KiB to 56-60 KiB. Shared BusyBox pages
mean the RSS difference is not equivalent to total RAM reclaimed. Short samples
showed about 4.8% of one CPU core for the old shell plus its children versus
about 0.1% for the native helper. Resolution, requested FPS, bitrate, JPEG and
motion settings were preserved.

## QHD And Upstream Update, 2026-10-04

The complete OpenIPC firmware tree was merged through upstream
`a71fc5dd55b032eced1d882549b29c48fbd781c2`, rather than selectively backported.
The latest rolling Majestic and Web UI were fetched on 2026-10-04. Supporting
packages include Mbed TLS 3.6.4, curl 8.15.0, and OpenIPC libevent at
`694decef35717d8955aa34ba4d2baaaf61c9e4a9`.

In the initial software-only stage, both cameras received the new Majestic,
Web UI, libraries, CLI, updater,
clock helpers, and compatible C120 light plugin. The old QHD hook is no longer
installed or loaded. Wi-Fi credentials, root passwords, bitrates, kernel,
bootloader, partition layout, and media-heap settings were preserved. Existing
JPEG/motion settings were retained; full-resolution motion visualization remains
off. GOP is now 2 seconds, not 40 seconds.

Backups of replaced software and configuration are stored privately on the
deployment PC, including a verified pre-update archive for each camera. Do not
commit these archives: they contain private configuration. Streamed staging on
persistent storage avoids decompressing large archives into these cameras'
small RAM-backed `/tmp`.

| Camera suffix | Resolution | Frames in 60 s | Preserved bitrate |
| --- | --- | --- | --- |
| .126 | 2560x1440 | 1199 | 10000 kbps |
| .196 | 2560x1440 | 1199 | 6000 kbps |

Both passed a normal Majestic restart and a software-triggered AP/setup-page/
station cycle. Opus audio remained working. The new light page was checked in
a browser at desktop and 390-pixel mobile widths without horizontal overflow.
Both also passed a software reboot: subsequent 15-second checks counted 299
and 298 QHD frames respectively, with working audio, the new binary, no old
hook, and the native recovery helper running. The clock restored correctly.
The H.264 header still advertises 30 fps; counted frames over stream timestamps
confirm the actual rate is approximately 20 fps. Physical button testing and a
multi-hour stream soak remain separate checks.

## Verified Full Image

The complete latest firmware built successfully from `84d98762`. C120-specific
256 KiB SquashFS blocks and XZ ARM/ARM-Thumb compression let it fit the existing
partitions without removing the Web UI or changing image quality:

| Image | Bytes | Partition limit |
| --- | --- | --- |
| `uImage.ssc377` | 2039272 | 2097152 |
| `rootfs.squashfs.ssc377` | 5140480 | 5242880 |

The image identifies itself as `ssc377_lite_tp-link-tapo-c120-v1`, not generic
SSC377 lite. Do not substitute a generic SSC377 image: it lacks the C120 sensor,
Wi-Fi power, and GPIO defaults. Recovery/light controls remain installable extras,
not files baked into the base image.

SHA256:

```text
711cfac32f177f6c7afac8c12f457be628b473539a2c79becb14f1b27d51382b  uImage.ssc377
18cf14b012bf72c7dd17a9e1ee7a750fa144d8623b2fb8ebd7a115bfaacfc318  rootfs.squashfs.ssc377
fb6b203fde1fee82a2e13020abc82324680f568f972bb2ef0fe087ed0b8abf59  openipc.ssc377-nor-lite.tgz
663debf35b66ccf5d3e8c5a36d2b9d00c1cafce9d07c1dbb02216cb01bf71447  usr/bin/majestic
```

The owner subsequently authorized a full flash over Wi-Fi, with UART recovery
available if necessary. Both cameras were upgraded successfully on 2026-10-04;
no UART recovery was needed. They now boot the October 4 kernel 5.10.61 #10
and root filesystem `84d98762`, displaying firmware version `2.6.10.04`.
Fresh installation still retains the upstream password-claim and Majestic
EULA flow for the human owner; existing claimed credentials were preserved.

Full flash backups and a logical overlay archive were stored privately off
each camera before writing. With no SD device exposed, Majestic was stopped
and the two raw image files were staged separately in `/tmp`, not an archive
plus unpacked copies. About 6.5 MiB remained available before the RAM pivot.
Upstream sysupgrade v1.0.69 was used with run-specific guards to abort if that
pivot failed and leave U-Boot environment keys untouched. Bootloader and
environment partition hashes remained identical, and independent SHA256
readbacks of both written images matched the published hashes above.

Retained overlay software was compared with ROM. The historical first-camera
sensor/load-script override, firmware version stamp, and comment-only helper
copies were reconciled with the new image. The separately installed AP/light
plugin, Wi-Fi configuration, passwords, SSH identity, video settings and
disabled crond were retained. No partition or media-heap change was needed.

Each camera passed AP/setup-page/station restoration and an additional reboot.
Final 60-second checks counted 1198 QHD H.264 frames on each, with 3001/3000
Opus frames respectively. Bitrates stayed at 10000/6000 kbps; JPEG and motion
remain off on the first camera and on on the second. Both helpers remained
running, and available memory was approximately 9908/9568 KiB. Physical button
presses and a multi-hour stream soak remain unverified in this rollout.

Never substitute a generic SSC377 image or use validation-bypass flags for
these upgrades. Preserve the settings partition unless deliberately doing a
fresh installation, which returns the camera to the human claim/EULA flow.

## Automatic Day/Night And Settings Audit

The post-flash audit on 2026-10-04 found `lightMonitor` disabled on the first
camera and enabled on the second. The default exposure-exhaustion trigger did
not switch the first camera even with its lens covered: SigmaStar reported
128x analog gain and zero scene luminance, but `isp_exposureismax` remained 0.
The C120 profile therefore uses Majestic's native `autoNightGain: 16` override,
not an extra polling daemon or legacy raw-gain thresholds. The observed
uncovered daytime gains were below 10x. This is a tested starting point, not
a claim that every installation has identical lighting.

The source profile now enables the monitor, keeps both actuators in `auto`,
and explicitly sets gain thresholds of 16x/2x and night/day delays of 15/60
seconds. It retains the inverted single-pin IR-cut on GPIO81 and active-high
lamp leader GPIO12. With the separately installed light plugin configured for
`12 13`, both IR illuminators follow the automatic night state. No daylight
sensor pin or legacy `minThreshold`/`maxThreshold` is configured.

Both live cameras passed owner-assisted lens-cover/uncover tests. Metrics
recorded one automatic night and one automatic day transition on each, source
4, grayscale in night, GPIO81 changing 1 -> 0 -> 1, and GPIO12/GPIO13 changing
0 -> 1 -> 0. Majestic's lamp-down check confirmed day; its anti-flapping penalty
remained 1. These short tests do not replace a dawn/dusk or overnight soak.

Live changes were limited to `nightMode.lightMonitor` and `autoNightGain`.
Effective configuration comparisons confirmed that all other Majestic settings
were preserved, including image orientation, audio rates, JPEG size, motion
ROIs, resolution, requested frame rate and bitrate. Both Majestic processes
remained running without rebuilding their media pipelines. Subsequent
30-second RTSP checks counted 598/599 QHD H.264 frames and 1501/1500 Opus frames
on the first/second cameras. Sensor selection, Wi-Fi power/driver, reset-button
plugin, memory layout, and watchdog were already configured correctly.

The first camera's clock was about 84 seconds fast with public NTP pools. It
was switched to a verified local time server and then agreed with the PC to
within the one-second clock-read precision; the second camera's working NTP
settings and both timezones were preserved. The local server address is a
deployment setting, not a firmware default. The known SDK/IQ minor-version
warning remains; this audit did not substitute an untested sensor tuning blob.

The verified image from `84d98762` above predates this source-default change;
its hashes have not changed. Existing installations preserve their overlay and
do not rerun the first-boot customizer. On the tested October Majestic, enable
the same native policy without reflashing:

```sh
cli -s .nightMode.lightMonitor true
cli -s .nightMode.autoNightGain 16
```

Leave the sensor-pin and legacy threshold fields empty, keep the IR-cut and
camera light in automatic mode, and retain the 2x/15s/60s defaults. The current
Web UI exposes these controls under **Settings > Day / Night**. See
[upstream day/night documentation](https://github.com/OpenIPC/wiki/blob/master/en/majestic-streamer.md#auto-daynight-detection)
for the native policy and supported tuning keys.

A separate full image was rebuilt from `251e59a5` with these new first-boot
defaults. Kernel/rootfs sizes are 2039332/5140480 bytes, still within the same
partitions. Packed-image checks confirmed the policy and unchanged Majestic
binary. This newer package has not been flashed or boot-tested; the two live
cameras remain on the verified `84d98762` base with the tested settings saved
persistently. An additional flash was not needed for this configuration change.

The full ARM build, native helper/form tests, QHD defaults/startup/pruning tests,
CI selector self-test, workflow syntax checks, and upstream shell tests passed.

## QHD 30 FPS Trial, 2026-10-04

Both cameras now retain an FPS-only change to 30 at 2560x1440 on the tested
October Majestic. Their existing 10000/6000 kbps bitrates, 33 ms exposure cap,
two-second GOP, audio, orientation, JPEG/motion settings and automatic day/night
policy were preserved by complete effective-configuration comparisons. The
separately installed AP/light helper remains running; both IR lamp pins still
follow the automatic night state. No firmware flash, preload, sensor override,
media-heap change or feature removal was needed.

Changing `video0.fps` through the live API alone accepted 30 but still encoded
about 20 fps on the first camera. A normal Majestic restart recreated the
native Ring SCL-to-VENC binding at 30/1 -> 30/1 instead of 30/1 -> 20/1. For
this tested build, restart after changing the saved frame rate:

```sh
cli -s .video0.fps 30
/etc/init.d/S95majestic restart
```

Each camera passed a 15-second RTSP check followed by three 60-second checks,
with working Opus audio, stable streamer PID and unchanged boot ID:

| Camera suffix | Decoded frames per 60 s probe | Native encoded fps | Available RAM at end |
| --- | --- | --- | --- |
| .126 | 1771 / 1799 / 1786 | 29.960 / 29.981 / 29.967 | 10000 KiB |
| .196 | 1799 / 1799 / 1799 | 29.992 / 29.993 / 29.994 | 9300 KiB |

Native counter deltas are measured independently of the nominal H.264 header,
which already advertised 30 even at the previous 20 fps setting. CPU usage
during the longer probes was approximately 30-33% / 26-32%, and temperatures
were 70/72 C. These are short night-mode trials, not an overnight stability or
latency soak. Following the owner's approval, source first-boot defaults now
use 30 fps and both cameras retain it persistently. The owner power-cycled both
after the trial; these were intentional, not observed crashes. Subsequent
15-second checks counted 449 QHD frames on each, with working audio and saved
30 fps. To fall back to 20 fps, change `video0.fps` and perform the same normal streamer
restart. Previously packed images retain their historical defaults; rebuild
the C120 profile to include this source change. Existing camera overlays keep
their saved video settings on upgrade.

A separate package was rebuilt from `251e59a5` plus the FPS-default change.
Kernel/rootfs sizes are 2039300/5140480 bytes, within the existing partitions.
Packed-image inspection confirmed the 30 fps customizer, and the QHD defaults,
pruning and streamer lifecycle tests passed. This new package has not been
flashed or boot-tested; live cameras keep their verified base image and saved
30 fps configuration.

```text
0719396c7af5386037d2551825de21f942cfc3980888b8c7c7fd57cf38be1648  uImage.ssc377
9663b8f81db8b5086afc603d8a8bef9a281a8d55c18b4cd0175c61dd0f0b0fcb  rootfs.squashfs.ssc377
740ca10fb746bfe83d034831e239fbdbc2a8e96f9fd849d9d74008b6e184a8a1  openipc.ssc377-nor-lite.tgz
```

## WireGuard Live Investigation, 2026-10-06

Read-only checks found both cameras healthy at saved 2560x1440 / 30 fps. The
local-only HTTPS proxy already supplied WebSocket upgrades and disabled buffering;
the router already had a narrowly scoped VPN-to-camera rule. The phone capture
showed large TLS streams for both camera hosts through the proxy, no direct UDP
to either camera in the filtered interval, and no observed TCP retransmissions
or zero-window stalls on those proxy streams.

Earlier camera logs showed phone WebRTC offers without a completed media session.
Local browser tests played both WebRTC and plain HTTPS/MSE at full resolution
and frame rate. A controlled browser that could not complete WebRTC negotiation
fell back to WebSocket MSE after approximately 33 seconds. This modeled a failed
negotiation; it did not reproduce the phone's network environment.

The owner subsequently reported a visible picture, but moving video and the
selected transport were not yet confirmed. No production camera, proxy, VPN or
phone settings were changed. Do not treat this investigation as a confirmed
remote WebRTC fix or weaken network isolation based on it.

## Lingering-Pet Notifications, 2026-10-06

Cam2's history showed repeated pet arrival events after brief confidence gaps,
with notifications permitted every 60 seconds. The notification helper now
refreshes visual presence on every accepted, ROI-filtered object. A category
that has already notified re-arms only after its full configured cooldown with
no accepted detection. Detector events, recording and floodlight signals are
unchanged. This is per-category presence, not individual animal tracking.

Fake-clock tests covered several minutes of intermittent pet detections, exact
60-second re-arm, independent person arrivals, settings reload and unchanged
sound cooldown. The full host suite, ARM musl build, QEMU and on-camera self-tests
passed. The executable SHA256 is
`17d1f790289ad181012acdf72a51bc7e065cf4ac3043a1efeae25febc5a35ef2`.

Only `.196` received the executable update, with an off-device rollback backup.
All saved camera, AI and light settings were preserved; Majestic PID and boot ID
were unchanged. A 93-second metadata-only check counted 174 analysis frames with
both detectors running and at least 7728 KiB available RAM. No pet was detected
in that interval, so it is health evidence, not a new live lingering-cat test.
No fabricated notification, room image or audio recording was produced.

## SD Model Library And Bird Decoder, 2026-10-06

Both cameras received the model-library API/WebUI and matching native helpers.
Stock remains selected: neither camera currently has a mounted SD card. The
library streams bounded uploads directly to SD, checks profile/size/SHA-256,
commits complete directories atomically and protects selected/active models.
Unavailable media falls back to Stock while preserving the requested selection.
No full model is staged in RAM or written to root flash.

The optional `yolov5n-coco-bird-v1` decoder implements the photo-calibrated
320x320 RGB S16 input and three raw YOLOv5 heads, with 80-class argmax before
bird filtering, confidence/NMS and inverse letterboxing. Its C preprocessing
matched Pillow byte-for-byte on five synthetic camera-size/asymmetric RGB
inputs. Quantized inputs and decoded outputs matched all 20 saved native-model
fixtures. This validates those offline fixtures, not live bird recognition.
NV12 camera conversion, actual model loading, memory/speed, live accuracy and
night performance await an SD card. Multi-crop inference is not implemented.

Bird events use the existing ROI filter, notification quiet-period gate,
recording filters and floodlight qualification/timer. Unsupported action filters
are retained across model selection. Existing Home Assistant receivers need
`bird` in their category allowlist before enabling bird alerts. The current
person/pet/vehicle actions and camera-specific floodlight preferences were kept.

Host checks covered absent/late/read-only/full storage, incomplete/corrupt files,
safe paths, symlinks, oversized offsets, protected deletion, tensor contracts,
thresholds and best-class policy. ARM compilation, QEMU and camera self-tests
passed. Both cameras passed hot confidence reload without recreating inference;
cam1 also passed missing-model fallback, repeated retry and AI-service restart
fallback. Original AI/light settings were restored, and complete Majestic
configuration, process IDs and device boot IDs were unchanged at 2560x1440 /
30 fps / 6000 kbit/s. Both also passed final authenticated API/CSRF/validation
checks and 90 seconds of uninterrupted visual/sound inference without a new
audio reconnect. These are bounded tests, not long-term stability evidence.

Installed executable SHA-256:

```text
b27a5bea2ce9abd0938f60ed67aefb193acb4b2154a7e6e05cbd99db2e873ffd  c120-aid
166647c6da99bc959b6f9b4cd91bf4b1af62893a91aa260133e6b0ee4063c6a0  c120-eventd
```

Rollback copies and private runtime evidence stay off-device under `analysis/`.
The bird model binary is private; only its profile and decoder source are in the
repository. The three-second model warm-up suppresses notification arrivals on
switching, but does not track identities or prove a subject left during a pause.
