# TP-Link Tapo C120 v1

This fork carries the working OpenIPC bring-up pieces for the TP-Link Tapo C120
v1 on SigmaStar SSC377 / Infinity6C with SC430AI or SC438HAI sensors. This guide describes
current source defaults and installation; dated image hashes and camera trials
are preserved in [HISTORY.md](HISTORY.md), not presented as current build artifacts.

For a new stock camera, use the [guided installer](stock-install/README.md).
It includes the validated firmware, verifies the sensor/version, and preserves
the camera's own full physical backup before asking for flash confirmation.
SC438HAI / stock 1.4.4 has completed a UART-free migration. The older
SC430AI / stock 1.4.1 path is available with explicit experimental opt-in,
qualified against saved dumps but not yet through a complete live migration.
Both use the same Python installer on Windows, Linux and macOS; no WSL is
needed for normal installation. See the guide for exact supported builds.

## Layout

| Path | Purpose |
| --- | --- |
| `br-ext-chip-sigmastar/board/tapo-c120/` | Base firmware defaults and speaker GPIO hook |
| `ap-recovery-plugin/` | After-flash reset-button Wi-Fi recovery |
| `runtime-overlay/`, `install-runtime.sh` | Lights, motion/AI floodlight, Live speaker controls |
| `ai-plugin/` | Optional AI Detection service, API, notifications and recording |
| `ai-probe/` | Standalone bring-up diagnostic; not installed on cameras |
| `check.sh`, `test_*` | Host-only regression checks |

## Firmware Board

The newer **SC438HAI** variant now has a source-built driver at
`sigmastar/infinity6c/sensor_sc438hai_mipi.c`, compiled against OpenIPC's SDK
headers. Use `BOARD=ssc377_lite_tp-link-tapo-c120-v1-sc438hai` for that sensor.
The original SC430AI profile below is unchanged; do not interchange them.

SC438HAI hardware verification on 2026-10-06: native 2688x1520 linear sensor
mode, H.264 output at 2560x1440/30 fps, stock day IQ loaded, and automatic
startup after a cold boot with at least five seconds of power-off. An
independent RTSP decode read 900 frames at the configured resolution/rate.
Sensor/ISP counters measured 29.98-30.00 fps with no CRC or hardware-drop
errors; the ISP channel's 44 startup drops did not increase during the check.
This is a bounded bring-up test, not long-term stability certification.

The extracted stock `sc438hai_2lane.ko` is **not** shipped or loaded. Its
sensor-handle layout overwrites OpenIPC's SPI pointer and causes a kernel
fault at address `0x9`; matching kernel version strings did not prove ABI
compatibility. The source driver replaces it, and the temporary vendor-startup
quarantine has been removed. The SC438HAI-only `/etc/default/majestic` exports
`SENSOR=sc438hai` because automatic identification does not yet recognize it.
Keep that file: it selects the correct IQ profile without a preload hook.

The stock IQ file produces a minor SDK-version warning, also seen with the
working SC430AI tuning. Linear video works. An experimental stock-derived HDR
driver port now registers long/short planes and passes host timing/gain tests
and an ARM module build. It also passed a linear QHD30 regression on `.101`.
HDR frames are NOT validated: the current Majestic binary explicitly selects
linear plane mode; four-lane PCB routing and suitable HDR ISP tuning are still
unverified. HDR integration/testing was stopped at the user's request and the
camera retains its proven linear-only module. The older SC430AI stock driver
also contains HDR paths; our deployed SC430AI source remains linear-only.
The stock alternate night IQ profiles are not validated. Automatic day/night is
configured, but a physical lens-cover transition test is still outstanding
on this sensor. Before migrating another stock camera, independently identify
its sensor and preserve its own complete physical flash backup and credentials.

Build the C120 profile with:

```sh
make BOARD=ssc377_lite_tp-link-tapo-c120-v1
```

The board profile sets:

- sensor `sc430ai`
- `srcfg` value `0 1 1 0 1 1`
- RTL8188FU USB Wi-Fi power enable on GPIO42
- IR-cut GPIO81 with inverted single-coil polarity
- camera light leader GPIO12
- native automatic day/night: 16x gain for night, 2x for day, 15/60-second delays
- 2560x1440 H.264 at 30 fps, 10000 kbps CBR, GOP 2 seconds (60 frames)
- maximum exposure 33 ms for stable frame delivery
- 48 kHz mono Opus microphone at level 50; speaker enabled at level 80 on GPIO43
- JPEG, video1, motion detect, records, and crond disabled by default

These are first-boot defaults, scoped to the C120 board overlay. Installing or
updating the recovery plugin does not alter video settings. Existing cameras
retain their settings on upgrade, so also set the resolution/rate explicitly
when migrating them to QHD. The sensor mode advertises 30 fps; the first-boot
QHD target is now 30 fps following the [native trial](HISTORY.md#qhd-30-fps-trial-2026-10-04). The exposure
cap can reduce low-light brightness; raising it may reduce the actual frame
rate.

The current upstream Majestic handles QHD natively through its Ring SCL-to-VENC
binding. The old `c120-qhd` package and preload hook have been removed. On an
existing camera, remove the old preload setting from `/etc/default/majestic` and
remove `/usr/lib/libc120-qhd.so` only when upgrading to the tested new streamer and
supporting libraries. Do not remove the hook while keeping the older streamer.

At QHD, leave `motionDetect.visualize` disabled: full-resolution motion boxes
use extra media memory. Motion detection and JPEG can remain enabled, as tested
on the second camera, although the lean first-boot defaults leave both off.

The SC430AI IQ/config blob used by the working cameras is included at:

```text
general/package/sigmastar-osdrv-infinity6c/files/sensor/configs/sc430ai.bin
```

The C120 CI artifact check requires the sensor module and this blob to be
present. Other Infinity6C sensor profiles retain their existing behavior.

## Audio

The speaker amplifier is active-high GPIO43. The C120-only `muxes.sh` exports
it as an output, initially low, on every boot through the existing
`S30customizer` hook. Majestic's SigmaStar GPIO writer needs the pin exported
before playback; configuring `speakerPin` alone left `set_gpio(43, 0) error`
in the log. A repeated hook run preserves an already active output. Native
Majestic controls power afterward, with a two-second hold after playback;
no resident helper or additional audio service is needed.

First-boot defaults enable microphone and speaker with shared 48 kHz mono
audio, Opus capture, microphone level 50 and speaker level 80. The older
8 kHz / speaker-off defaults predated playback verification. Existing cameras
retain saved settings on upgrade: install the C120 `muxes.sh` and use the
native Audio settings to enable output. On SigmaStar, changing the speaker
level needs **Apply** to rebuild the pipeline; this briefly interrupts the
stream but does not change video settings.

Authenticated `POST /play_audio` accepts raw signed 16-bit little-endian mono
PCM or Ogg Opus. For PCM, declare the source rate, for example
`Content-Type: application/octet-stream;rate=48000`. Do not send a WAV header
as PCM. Playback streams through the native server rather than staging a
large file in the camera's RAM. See the
[upstream audio documentation](https://github.com/OpenIPC/wiki/blob/master/en/majestic-streamer.md#enabling-the-speaker)
for clip playback and two-way protocols. Browser microphone access needs
HTTPS or a local secure origin; enabling the amplifier does not remove that
browser restriction on ordinary LAN HTTP.

The runtime installer adds a speaker-volume slider to **Live**, using the
native toolbar styling. This changes only `audio.outputVolume`; the existing
listen slider still controls playback in the browser. Changes are saved on
release and applied through the native Majestic reload endpoint. Zero mutes
the speaker. Save/apply failures remain visible rather than silently showing
an unapplied level.

Two-way audio reuses upstream's **Talk** toggle on the **WebRTC** transport.
It requests the browser microphone only when Talk is switched on and releases
it when switched off or the player closes. No custom audio transport or extra
camera daemon is added. On plain HTTP, the add-on's Talk link opens the secure
Live address saved in `/etc/c120-live-audio.json`, for example:

```json
{"secureLiveUrl":"https://camera.example.com/cgi-bin/live.cgi"}
```

Without a proxy address or configured native HTTPS certificate, the HTTP Talk
link is disabled. For a reverse proxy, preserve Host and authentication,
enable WebSocket upgrades with HTTP/1.1, disable request/response buffering,
and allow long-lived streams. Use a trusted TLS certificate and restrict the
proxy to trusted local clients; local DNS alone is not an access restriction.
The owner verified browser-to-speaker talkback on 2026-10-05. Echo cancellation
is not provided by the camera, so avoid putting the browser's speaker and the
camera microphone next to each other during a call.

For microphone streaming, use `/audio.opus`. The tested Majestic build can
crash on `/audio.pcm` even with AI stopped; this is independent of speaker
PCM uploads to `/play_audio`. See the [AI plugin status](ai-plugin/README.md#status).

## Remote Live Viewing

The native Live page offers WebRTC and MSE. WebRTC signaling uses the reverse
proxy, but media normally uses a direct ICE/UDP connection to the camera.
A proxy that serves the page successfully does not prove that path is reachable
from a VPN client. Check VPN routes and narrowly scoped firewall permissions;
keep the cameras and proxy private rather than opening them to the internet.

If WebRTC stays at connecting, select **MSE** in the Live controls. The preference
is saved per browser and camera origin, not in the camera's firmware settings.
MSE uses `/ws/video` through the same HTTPS proxy and does not lower resolution,
frame rate or bitrate. Listening remains available; Talk requires WebRTC.
Native MSE can first try a data channel before falling back to WebSocket.

A controlled failed-negotiation test took about 33 seconds to reach automatic
WebSocket fallback. Separate proxy tests played at 2560x1440 / 30 fps. These are
browser-test observations, not proof that every remote client's ICE path works.
The [WireGuard investigation](HISTORY.md#wireguard-live-investigation-2026-10-06)
records what was checked without claiming a confirmed phone-side fix.

## Installable Extras

`ap-recovery-plugin/` contains the tested after-flash recovery plugin:

- reset-button toggled open setup AP on `192.168.4.1`
- Web Wi-Fi setup CGI
- merged resident `c120-eventd` for reset-button handling
- no timer; AP/station mode is controlled by the button

`runtime-overlay/` contains the tested runtime helper files for the live cameras:

- `c120-lamps` for off, white, 850 nm, 940 nm, and both-IR modes
- `c120-light-pins.cgi` for configuring multiple camera-light GPIO pins
- `c120-eventd` merged reset-button, light-pin mirror, and motion floodlight daemon
- `c120-lights.cgi`, using the current Web UI's controls and navigation
- a Live-page speaker-volume control and secure Talk link, without replacing the native player

Run `build-plugin.sh` with the target compiler to generate both packages in
`build/` (see [Build And Test](#build-and-test)). Copy the two tarballs to the
camera and install AP recovery first, then the light controls:

```sh
cd /tmp
gzip -dc openipc-c120-ap-recovery-plugin.tgz | tar -xf -
sh openipc-c120-ap-recovery-plugin/install.sh
gzip -dc openipc-c120-runtime.tgz | tar -xf -
sh openipc-c120-runtime/install-runtime.sh
```

The light installer requires the current OpenIPC Web UI and an inactive setup
AP. It preserves the light-pin configuration and backs up the navigation file.
Open **Camera > Lights** for the five lamp modes and multi-pin settings.
Re-run this installer after a standalone upstream Web UI update to restore the
menu entry. It does not replace upstream's live/preview page.

The runtime installer also applies `dashboard-luminance.sed` to the dashboard:
`isp_avelum` stays in raw SDK units, the scene-luminance graph auto-scales, and
the incorrect universal `/ 255` label and 8-bit low-luminance band are removed.
This is a display-only fix; camera exposure and day/night settings do not change.
Re-running the installer is idempotent and backs up the old dashboard script.

### Floodlight URL

`http://CAMERA/cgi-bin/c120-floodlight.cgi` toggles white GPIO14 independently
of the IR lights and day/night state. It uses the same authentication as other
Majestic URLs and is listed under **Night** on **Stream URLs**. Majestic owns
`/night/*` internally, so this runtime extension uses a CGI endpoint instead.
Append `?action=on`, `?action=off`, or `?action=status` for idempotent control or
a read-only check; `?action=toggle` is the default. GET and form-encoded POST
are supported, while HEAD and invalid actions never change the lamp.

The response is JSON with `white`, `manual`, and `available` booleans. Manual
on cancels any active motion timer and stays on until turned off or another
lamp mode is selected. Manual off releases ownership so fresh night-time
motion can trigger the timer again. Motion preferences are not changed. The
helper serializes these commands with motion/manual lamp control and rejects
a floodlight GPIO assigned to the IR mirror or reset button. No extra server,
resident process or stream restart is needed.

### Night Motion Or AI Floodlight

**Camera > Lights > Motion floodlight** provides a night-only enable switch
and a duration of 1-600 seconds. Motion delay is adjustable from 0-600 seconds,
defaulting to 3: the ROI-filtered counter must grow in consecutive one-second
polls for that long before the light turns on. A quiet poll resets the wait;
zero enables immediate triggering. Once lit, any further detection extends the
off deadline without another qualification delay. The initial duration is 30 seconds; the feature
is disabled on a new plugin install until enabled explicitly. Native Majestic
motion detection must already be enabled and working; it is not enabled or
retuned by this plugin. It uses the existing motion regions: detections rejected
by Majestic's ROI filter do not start or extend the floodlight timer. With no
regions configured, the native detector watches the whole image. Camera quality
settings are never changed.

With the [AI Detection plugin](ai-plugin/README.md) installed, this section also
offers **AI objects** as the trigger, with separate Person / Pet / Vehicle / Bird
filters. Qualification and duration work the same way. AI presence must be
fresh, pass confidence and motion regions, and survive the qualification delay;
raw motion alone cannot trigger an AI-only light. The existing GPIO helper reads
the AI signal without a second daemon. New installs still default to motion,
with automatic lighting disabled.

The existing native helper reads `night_enabled` and the ROI-filtered native
motion counter once per second. A new detection while night mode is active
switches only white GPIO14 on; further motion extends the deadline. Expiry,
day mode, detector/API failure, AP startup, disable/reload, and normal shutdown
turn off a lamp owned by this timer. A three-second settling period after lamp
or night transitions ignores exposure changes caused by the light itself.

Manual lamp commands take ownership and cancel the current automatic timer.
Manual white remains on until changed manually; the timer does not shut it off.
Other manual modes can be followed by a fresh automatic trigger. GPIO12/13 and
the IR-cut policy are not changed by the motion timer. Do not add GPIO14 to the
IR mirror list: the motion feature refuses that conflicting assignment.

Settings are stored separately in `/etc/c120-motion-light.conf` and survive
reboots and plugin updates. The UI uses validated, same-origin POST requests.
The small HTTP reader is bounded to 200 ms per loopback request; no shell polling
process, extra resident daemon, dynamic library, or firmware reflash is needed.
Status is available from `c120-eventd motion-status` as JSON.

`c120-eventd.c` builds one native helper, inserted into both generated packages.
The source tree stores neither binary copies nor duplicate init scripts. It replaces
the shell polling loop, enforces a single running instance, validates settings,
and only changes follower GPIO outputs when necessary. Debug logging is capped
at approximately 64 KiB. AP startup failure restores station mode and services;
concurrent AP transitions are locked, and repeated start/stop calls are harmless.

## AI Detection

The optional [AI Detection plugin](ai-plugin/README.md) provides person / pet /
vehicle recognition, experimental sound categories, webhook notifications,
AI-triggered recording and a signal for the existing floodlight helper. It
preserves native motion regions and video settings. Both detectors and actions
are opt-in on a fresh installation.

The WebUI also manages compiled models on a mounted SD card, with upload,
selection, removal and reported fallback to Stock. An optional bird-only YOLOv5n
profile has passed offline decoder checks; live camera validation is pending an
SD card. Stock remains the default and does not expose a verified bird class.

Model binaries and the compatible IPU library are private, owner-provided build
inputs; they are not committed or downloaded by the plugin build. Build/install,
provenance, authenticated API and Home Assistant notification examples are in
the plugin guide. Stop AI before running the standalone probe: both use IPU and
SCL resources that must not be shared concurrently.

## Build And Test

Build from a Linux checkout with real symbolic links and LF line endings. A
Windows checkout copied verbatim to Linux can produce an unbootable image:
`#!/bin/sh\r` cannot execute, and flattened links break helpers and DNS. The
post-build startup check rejects these defects before packing the rootfs.
Run its regression tests with:

```sh
python3 general/scripts/tests/test_rootfs_startup.py
```

Build the helper with the OpenIPC Infinity6C musl toolchain on Linux. For the
host checks on Debian/Ubuntu, install the test dependencies first:

```sh
sudo apt-get install gcc make haserl nodejs python3-numpy pkg-config ffmpeg curl \
    libcurl4-openssl-dev libjson-c-dev libogg-dev libopus-dev zlib1g-dev
```

From the repository root:

```sh
CC=/path/to/sdk/bin/arm-openipc-linux-musleabihf-gcc sh contrib/tapo-c120/build-plugin.sh
sh contrib/tapo-c120/check.sh
```

The helper is statically linked so both existing C120 firmware versions can run
the same binary. The build writes `build/c120-eventd`,
`build/openipc-c120-ap-recovery-plugin.tgz`, and `build/openipc-c120-runtime.tgz`.
Each package contains its own `eventd.sha256`; source files are never overwritten.
Pass an output directory as the build script's first argument to build elsewhere.
`check.sh` is also the C120 CI entry point. It checks generated packages and
helper consistency, native helper/QHD behavior, Live audio controls, AI validation,
notification and recording actions, synthetic Opus under ASan/UBSan, and the DSP
reference. DSP sources are fetched from pinned upstream commits; stock models,
camera credentials and private samples are not needed. Live `ai-plugin/test_api.py`
is deliberately separate because it connects to a real camera.

Tests use simulated GPIOs and services; they never touch a camera. They cover
button toggling, exclusive IR modes, reload/stop, duplicate processes, AP
rollback, board selection, and sensor-source placement. Motion tests cover
ROI-filtered versus unfiltered events, continuous-motion qualification and quiet-gap reset, night gating,
expiry/retrigger, scene-change settling, manual ownership,
unavailable/invalid/stalled native metrics, counter resets, stale-marker cleanup,
and POST validation. Real GPIO, native motion events, and the Web UI still need
on-camera checks after deployment; simulated tests are not hardware evidence.
