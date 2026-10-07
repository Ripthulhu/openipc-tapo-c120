# C120 Verification History

Dated observations and historical image hashes. These do not describe the bytes
of a freshly rebuilt image or guarantee long-term stability. Current installation
and source defaults are in [README.md](README.md).

## Fleet Software Alignment, 2026-10-08

All four cameras now use the tested Majestic `master+d8a0721` binary and
AI helper `df826cf2cdf6092845814ee7140c32567386d7550449ce44ffa39c4be686cf73`
described below. Their static Web UI files were aligned with the verified
`master+a6c7cf9` UI plus our runtime extensions, including WebSocket MSE
and raw SDK luminance. Cam4 had been missing the luminance correction;
its readings are no longer presented as an 8-bit value out of 255.

The base remains the October 7 firmware with SC430AI components on cam1/2
and SC438HAI components on cam3/4. Sensor binaries and individual settings
are intentionally not interchangeable. Existing video, AI actions, MQTT and
Wi-Fi configuration were preserved, with no camera reboot or partition writes.
Cam4 also received the previously missing Lights/AP-recovery and MQTT
plugins; automatic floodlighting and MQTT remain disabled there by default.
The host suite passed against the exact first-commit source, independently
of the subsequent optimization work. This alignment does not resolve the
concurrent-recording limitation documented below.

## Cam3 Recording and Live Load, 2026-10-08

Cam3's native-resolution freeze was reproduced by reading `/video.mp4` with
AI running, even when discarding the stream without writing to SD. HTTP
configuration requests then timed out while the OS remained reachable.
The collected logs do not establish an OOM kill or kernel crash.

Only cam3 received upstream Majestic `master+d8a0721` (2026-10-07 17:31),
SHA256 `6ab7e4ff8e9d077f3f46fd7a81d28e2a4f08caefc1ba5e8267dc97712a94d788`.
The updated AI helper bounds periodic configuration waits at 50 ms, retaining
the last verified settings for at most ten seconds only on a timeout with
the same active camera owner. Twenty-four host cases cover post-load retries
and runtime timeout, invalid-response, shutdown and ownership guards. The
full `check.sh` suite passed. Final deployed helper SHA256:
`df826cf2cdf6092845814ee7140c32567386d7550449ce44ffa39c4be686cf73`.

The Live runtime add-on now uses upstream's `MJ_FEED = "websocket"` for MSE;
WebRTC remains available for Talk. Two bounded 80-second measurements,
excluding the first 15 seconds, showed camera CPU averaging 70.5% with the
previous MSE data-channel feed and 53.8% with WebSocket. Majestic's main
thread fell from 29.5% to 7.3%. Live stayed open in both trials; Dashboard
was paused for much of the first and open throughout the second. These
are sustained-load observations, not identical-workload peak benchmarks.

Recording remains unresolved under concurrent viewing. One earlier saved
clip decoded only 330 video frames over 12.77 seconds despite a 15-second
request. The final trial with Live and Dashboard open produced an HTTP
timeout and a stalled retry, with no completed clip published. Majestic
reported 2524 KiB free memory below its 3467 KiB floor and shed the MP4
client. Video and both detectors recovered without a camera reboot or
Majestic/AI/MQTT daemon restart. This is recovery evidence, not proof of
reliable or lossless native-resolution recording.

Live video stayed at 2688x1520, 30 fps, 6000 kbit/s CBR and GOP 0.5 seconds.
Motion detection, saved AI actions and notification settings were preserved.
The final stream buffer is 512 KiB; experimental VM dirty limits were
restored to their original values. No other camera was updated in this trial.

## Native Resolution and MP3 Tools, 2026-10-07

Both SC430AI and SC438HAI drivers expose 2688x1520 at 30 fps. All four cameras
now encode that native size, retaining their live 6000 kbit/s CBR, 0.5-second
GOP and other Majestic/AI settings. The previous QHD output was scaled from
the full sensor, not an explicit crop; this adds output pixels, not field of
view. New first-boot defaults use the native size. The previously published
stock installer kit remains pinned to its earlier QHD images.

Independent 30-second RTSP checks decoded 857-898 H.264 frames and matching
Opus audio without errors on each camera. Cam1's later reboot was confirmed
as a manual power cycle, and its repeat stream check passed. Cam2's HTTP
configuration endpoint stalled after its initial MP3 test, services restarted,
and the camera rebooted unexpectedly around 22:56 local. Neither the owner
nor the coordinated bird-model chat caused that reboot. Available post-boot
logs do not establish its cause; it must not be attributed to MP3 encoding
or declared fixed without further evidence. Cam2 recovered, retained its
settings and passed the repeat MP3 and stream checks.

The optional [MP3 tools package](mp3-tools/README.md) installs Buildroot's
LAME 3.100 frontend and shared library on all four cameras, without a startup
service, firmware flash or partition change. A synthetic one-second mono
48 kHz tone encoded at 64 kbit/s on each camera and independently decoded
to 48,000 samples with the expected level. Settings and running process IDs
were unchanged in successful install checks. It adds neither `/audio.mp3`
capture nor MP3 input to `/play_audio`; playback still needs PCM or Ogg Opus.

After recovery, a low-overhead 60-second check preserved every boot ID and
Majestic/AI/MQTT process ID. Cam1-3 each processed 117 visual frames and
249-250 sound frames, with at least 4880 KiB Linux and 4066 KiB media memory
free across those cameras. Cam4's detectors remained deliberately disabled.
These are bounded checks, not a long-term stability or simultaneous
recording/recognition guarantee at the new resolution.

## One-Class Bird Models, 2026-10-07

Cam4 (.152) qualified one-class 320 and 384 bird models with the combined
analysis-port lifecycle fix and bounded post-load HTTP-timeout retries. Live
model switching passed with normal HTTP status polling; inference took
28-32 ms and 42-51 ms respectively at a 500 ms interval. During the 384 trial,
30-second RTSP decoding returned 893 QHD H.264 frames plus 1501 Opus frames
with no decoder errors. Scene recognition accuracy was not measured.

The runtime rejected the larger COCO model at the queried tensor/feed memory
guard. Its bytes remain in SD reference storage outside the selectable library.
Cam4's exact original configuration was restored: Stock selected, visual and
sound detection disabled, no automatic actions enabled, unchanged video quality.
Final free media memory returned to 10,527 KiB. Native combined helper hash:
`5b3363be1089c17db4498ef9ed6693be5587ad16a3b2720bba17c712a5adabbd`.
These are bounded hardware checks, not a long-term stability or accuracy claim.

The tested decoder/profile/memory/retry changes are integrated into shared
source for future builds. They were not deployed to cam1, cam2 or cam3, and
neither experimental model becomes a fresh-install default.

## AI Analysis Port Recovery, 2026-10-07

Cam3 (.101) stopped both detectors with "analysis port is in use". Its old
helper had forgotten ownership of an enabled, unbound SCL output 2, although
the saved 800x448 settings and Majestic PID still matched the live port.
The "invalid motion regions" status was also misleading: the saved region
list was empty and valid. The exact initial failed ownership query was not
captured, so the observations do not prove whether it was a driver error or
an incomplete proc dump.

Ownership now distinguishes unknown from known-other. Unknown queries and
failed cleanup retain the claim for retry; proc reports must contain complete
binding and output sections. A visual-port conflict no longer blocks sound
startup. Successful camera validation clears the stale region warning.
An orderly Majestic restart cleared the existing orphan; only the AI helper
was replaced, with all saved settings and QHD/30/6000 video preserved.

Host regressions passed. On-device detector enable/disable combinations
worked. Repeated CGI polling caused temporary configuration timeouts during
the strict soak; both detectors recovered automatically without a service
restart or another orphan. A separate low-overhead 60-second status check
completed uninterrupted, with 115 visual and 246 sound frames, at least
4672 KiB Linux and 5193 KiB media memory free. The final cam3 helper hash is
`4c6eef5085b802f4f29f9c5c321ca788d589749e8fcc601ca7191532d15e0fa9`.
This verifies bounded recovery, not the absence of all HTTP-load pauses or
long-term faults.

## MQTT, SD and Overlay Cleanup, 2026-10-07

The optional [MQTT plugin](mqtt-plugin/README.md) is deployed on cam1 (.126),
cam2 (.196) and cam3 (.101). Each publishes 27 Home Assistant entities through
a dedicated broker account restricted to its own topics. Real HA setting
commands, discovery, reconnect state and negative cross-camera ACL checks
passed. Commands never alter video resolution, FPS or bitrate; retained
commands are rejected. Completion events are best-effort, so the durable
recordings API remains the way to catch up after disconnection.

Home/Away is a persistent HA notification mode. Cam1 and cam2 phone alerts
require Away; cam3 deliberately notifies in both modes. Detection, recording
and light actions continue in Home. The existing notification filters,
cooldowns and still attachments were preserved. Cam3 retains automatic
person/pet recording; cam1 and cam2 automatic recording remain disabled.

Cam1's replacement 32 GB card was explicitly formatted FAT32 with 32 KiB
clusters after an old FAT error had remounted it read-only. Filesystem checking,
writes and plugin-package restoration passed. HA's Record Clip button reached
the native recorder and produced a catalogued H.264 2560x1440/Opus clip plus an
MQTT/HA completion event. The final 30-second request contained 29.800 seconds
of parseable media, 855 video packets and matching audio. This is not a promise
of lossless capture under every simultaneous viewing/analysis load: an earlier
request contained only 20.3 seconds despite a 30-second wall-clock timer.
GOP remains 0.5 seconds; cam1's live buffer is 512 KiB. QHD/30 and 6000 kbit/s
were preserved. Recording errors now distinguish storage/stream failures and
refuse a read-only filesystem before creating a clip.

Private complete overlay backups preceded cam1/cam2 cleanup. Stale historical
backups and verified redundant upper files were removed. The old October 4
Majestic override (`663debf3...`) was masking the newer October 6 version
already in their verified zram firmware (`0d61f1b7...`). Its matching core
library set was tested with reversible bind mounts before retiring the old
overrides and rebooting. Thirty-second QHD H.264/Opus checks passed, and
persistent settings, AI models/helpers, MQTT and memory-dashboard files stayed
intact. Cam1 free overlay space rose from 1080 to 4236 KiB (88% to 52% used);
cam2 from 1368 to 4280 KiB (85% to 52%). No kernel, rootfs or bootloader flash
write was needed. Future upgrades should check for stale core overrides rather
than assuming an updated immutable image is the active runtime.

The full C120 regression suite passed, including MQTT broker/command tests,
read-only/error handling, retention, recording catalogue and memory dashboard.
Long-term recording stability and a fresh physical motion/talkback check on
both older sensors remain separate from these stream/configuration checks.

## HTTP MP4 Stall, 2026-10-07

Cam4 (.152), Majestic `0ea3123`: the HTTP MP4 path drops video when a GOP
exceeds its fixed 1 MiB muxer limit. This reproduced with plain `curl`, without
the AI recorder. A 10-second request at GOP=2 timed out after 18 seconds with
only 198 video packets and mismatched audio/video durations. Setting
`records.fragmentMs=250`, `fragmentBytes=262144`, and `queueBytes=524288` did not
fix that HTTP path; all three were restored.

`video0.gopSize="0.5"` keeps groups small enough without enlarging buffers or
changing 2560x1440, 30 fps, 10,000 kbit/s CBR, exposure or audio. Cam4 now uses
that interval, and both C120 variants inherit it as the first-boot default.
Existing configurations are not silently migrated by plugin installation.
This is a configuration workaround for the current proprietary muxer, not a
patch to Majestic itself; more frequent keyframes trade some coding efficiency
for bounded groups and quicker starts.

Hardware checks:
- HTTP `duration=10`: 300 video packets over 10.000022 seconds, matching Opus.
- HTTP `duration=30`: 900 video packets over 30.133354 seconds; audio ended
  within 0.1 ms of video, and the complete file decoded without errors.
- The real AI recorder with an explicitly synthetic `integration-test` trigger
  and a five-second timer saved 135 QHD frames over 4.500025 seconds, Opus audio,
  JPEG and catalogue metadata, with one completed clip and zero errors. The
  next-keyframe start and whole-fragment close account for the half-second gap.
- A separate 30-second RTSP check decoded 899 QHD frames and 1,501 Opus frames.
- No reboot or reflash; boot ID stayed unchanged. The QHD/defaults and AI action
  host tests passed. No other camera was changed.

Full visual-recognition-to-recording testing still needs media-memory headroom.
The SD also remains about 99% full of stock Tapo files, which were not deleted;
the 30-second AI test correctly refused to start at its test storage limit.
Automatic recording and outbound webhooks stay disabled on cam4. The current
GOP change resolves the demonstrated stream stall, not those separate guards.

## Recording Catalogue, 2026-10-07

The optional AI plugin now supplies the version-1 recordings catalogue, stable
storage-generation cursors, native close-hook integration, AI summary/still
metadata, and persistent `recording.ready` webhook attempts. The API and a Python
standard-library client are documented in [ai-plugin/RECORDINGS-API.md](ai-plugin/RECORDINGS-API.md).
AI retention now evicts oldest closed catalogued clips using `records.maxUsage`;
native recording keeps Majestic's own oldest-first purge. Test coverage includes
crash/index repair, filters, cursor expiry, deleted files, path/symlink confinement,
webhook retry/cancellation/expiry, full storage, open-file protection, and streams
that must not be published as successfully completed clips.

Cam4 (.152) validated a real native close event, stable IDs through service restarts,
root authentication and HTTP Range downloads. A synthetic trigger through the real
AI recorder produced a QHD H.264/Opus file plus a JPEG and correctly labelled
`integration-test` summary. This does **not** claim a real person was recognized.
The other cameras were not changed.

At the initial catalogue test, the stock AI engine stayed paused because
reported free media memory was about 10,527 KiB, below its 11,264 KiB start
guard. The HTTP MP4 GOP stall was resolved later as described above. Experimental
buffer and fragment settings were restored; automatic recording and webhooks
remain disabled on cam4. Full live AI-triggered recording on this Majestic build
has not been validated on other cameras.

The nearly full cam4 SD still contains stock Tapo preallocated files. These were
not erased. Retention only manages recordings inside its configured recording
root, not arbitrary stock files or firmware backups. Cam4's final recording path
is `/mnt/mmcblk0/recordings/%F`, correcting the nonexistent `mmcblk0p1` mount;
native/AI recording and both notification channels remain disabled. The example
client downloaded the test clips, summaries and available stills, then replayed
its checkpoint without duplicate downloads. The final 30-second RTSP check
decoded 898 QHD H.264 frames and 1,501 Opus frames at 48 kHz. The boot ID remained
unchanged, and final available Linux memory was about 9 MiB. The full C120 host
suite passed, including both pinned stock-installer images.

## Upstream Refresh, 2026-10-07

Merged firmware `5650e0297260dc312375de6eef4491d3b0ad1d81` onto fork base
`02130b6ffaa28f96c555c3688c01e091e5036d50`. This includes the RAM/CMA-aware
sysupgrade checks and sparse Majestic configuration. C120's first-boot CLI
writes were checked with a missing config, including the ARM yaml-cli under
QEMU. CI retains both C120 runtime and upstream build-request coverage; the
rootfs pruning test fixtures now satisfy the existing startup validator.

Both C120 boards completed full builds. Majestic is `0ea3123` (2026-10-06),
binary SHA-256 `0d61f1b725ea9b4ce3db217bdacdaeb29509a3c7389ab114c1e32b15c91ea2cd`.
WebUI is `master+a6c7cf9, 2026-10-06 21:19`, including its ICE restart and
configuration-reset updates. Packed-image tests enforce matching components
and the navigation, Live audio and floodlight-list integration points used by
the optional plugins. This is not a live AI/plugin rollout: cam4 has the base
firmware, and the other cameras were left untouched.

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| SC438HAI kernel | 2038396 | `de8e5a3836e83304571ba63b5a389ff6b46b3516b0fcf76af8da048925f46c55` |
| SC438HAI rootfs | 5033984 | `3827b26d23d3910fa31e61d6fd3beae4981e8494a8f04e4da4f27a8cdfc853f6` |
| SC430AI kernel | 2040308 | `d30205fa681e51f8c0c952c653de52363865694f8acc4bd05a04e175a36fa8c4` |
| SC430AI rootfs | 5181440 | `7d37a0492944ce6e59c03b2aa8f2a12309473f9beeb9d504af3ec70d019aefb3` |

All fit the 2 MiB kernel / 5 MiB rootfs partitions. Rootfs headroom is 204 KiB
for SC438HAI and 60 KiB for SC430AI. The public kit manifest is pinned to
`4cc5cb9186177f55c4b9278464ea6373f81e35974b88fde0b28893849ca85d1f`.
Images were built from the merged source tree (verification snapshot
`ac2ef8895f19e0e6b450bbf59f8f72045168d416`) and stamped
`BUILD_SHA=upstream-5650e029`, not with the eventual merge commit ID.

Only cam4 (`.152`, SC438HAI) was upgraded. Its old kernel/rootfs and private
configuration/overlay were backed up off-device. Both new flash partitions
matched the full image hashes above on physical MTD read-back. After the
upgrade and a second software reboot, each settled RTSP test decoded 899
H.264 frames over 30 seconds at 2560x1440/30 fps, plus 1501 Opus frames at
48 kHz. WebRTC and MSE both played at full resolution in the browser.
Effective configuration, saved YAML and password hash remained unchanged;
resolution, frame rate, bitrate, exposure and day/night settings were not tuned.
Available RAM after the second boot was 9516 KiB of 27784 KiB.

The update staged files on SD, bind-mounted below `/tmp`, so sysupgrade's
RAM-root pivot retained access without copying the images into scarce RAM.
A plain absolute SD path is not preserved by that pivot; this test did not
change generic sysupgrade path handling. The existing overlay was preserved;
no bootloader rewrite, reset, EULA acceptance or password change was performed.

This is bounded linear-mode validation, not a new cold-power test, HDR test,
physical day/night transition test or stock-to-OpenIPC migration. SC430AI's
refreshed image was checked offline only; its experimental opt-in remains.

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
