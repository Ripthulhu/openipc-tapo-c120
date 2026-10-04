# TP-Link Tapo C120 v1 Notes

This fork carries the working OpenIPC bring-up pieces for the TP-Link Tapo C120
v1 on SigmaStar SSC377 / Infinity6C with an SC430AI sensor.

## Firmware Board

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
- 2560x1440 H.264 at 20 fps, 10000 kbps CBR, GOP 2 seconds (40 frames)
- maximum exposure 33 ms for stable frame delivery
- JPEG, video1, motion detect, records, and crond disabled by default

These are first-boot defaults, scoped to the C120 board overlay. Installing or
updating the recovery plugin does not alter video settings. Existing cameras
retain their settings on upgrade, so also set the resolution/rate explicitly
when migrating them to QHD. The sensor mode advertises 30 fps, but the verified
QHD output target is 20 fps. The exposure cap can reduce low-light brightness;
raising it may reduce the actual frame rate.

The current upstream Majestic handles QHD natively through its Ring SCL-to-VENC
binding. The old `c120-qhd` package and preload hook have been removed. On an
existing camera, remove the old `/etc/default/majestic` and
`/usr/lib/libc120-qhd.so` only when upgrading to the tested new streamer and
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

## Installable Extras

`ap-recovery-plugin/` contains the tested after-flash recovery plugin:

- reset-button toggled open setup AP on `192.168.4.1`
- Web Wi-Fi setup CGI
- merged resident `c120-eventd` for reset-button handling
- no timer; AP/station mode is controlled by the button

`runtime-overlay/` contains the tested runtime helper files for the live cameras:

- `c120-lamps` for off, white, 850 nm, 940 nm, and both-IR modes
- `c120-light-pins.cgi` for configuring multiple camera-light GPIO pins
- `c120-eventd` merged reset-button and light-pin mirror daemon
- `c120-lights.cgi`, using the current Web UI's controls and navigation

Install the AP recovery plugin with its own `install.sh` first. Then install the
light controls from this directory:

```sh
sh contrib/tapo-c120/install-runtime.sh
```

The light installer requires the current OpenIPC Web UI and an inactive setup
AP. It preserves the light-pin configuration and backs up the navigation file.
Open **Camera > C120 Lights** for the five lamp modes and multi-pin settings.
Re-run this installer after a standalone upstream Web UI update to restore the
menu entry. It does not replace upstream's live/preview page.

`c120-eventd.c` builds the native helper shipped in both payloads. It replaces
the shell polling loop, enforces a single running instance, validates settings,
and only changes follower GPIO outputs when necessary. Debug logging is capped
at approximately 64 KiB. AP startup failure restores station mode and services;
concurrent AP transitions are locked, and repeated start/stop calls are harmless.

## Build And Test The Plugin

Use the OpenIPC Infinity6C musl toolchain on Linux:

```sh
CC=/path/to/sdk/bin/arm-openipc-linux-musleabihf-gcc sh contrib/tapo-c120/build-plugin.sh
python3 contrib/tapo-c120/test_runtime.py
python3 contrib/tapo-c120/test_qhd.py
```

The helper is statically linked so both existing C120 firmware versions can run
the same binary. The build updates both payload copies and `eventd.sha256`.
Tests use simulated GPIOs and services; they never touch a camera. They cover
button toggling, exclusive IR modes, reload/stop, duplicate processes, AP
rollback, board selection, and sensor-source placement.

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

The full ARM build, native helper/form tests, QHD defaults/startup/pruning tests,
CI selector self-test, workflow syntax checks, and upstream shell tests passed.
