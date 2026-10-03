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
- 2432x1376 H.264 at 60 fps, 10000 kbps CBR, GOP 60
- JPEG, video1, motion detect, records, and crond disabled by default

These are first-boot defaults, scoped to the C120 board overlay. Installing or
updating the recovery plugin does not alter video settings. The 60 fps setting
is a requested encoder rate; the checked-in sensor mode advertises a maximum
of 30 fps, and exposure can reduce the measured sensor rate further.

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
- C120 preview page additions

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

Five-second RTSP checks decoded H.264 and Opus from both cameras at roughly
30 video frames per second. One requested size, 2067x1170, was reported by the
encoder as 2064x1168; the other remained 2432x1376. No encoder configuration was
changed during this update. Both cameras passed an AP/setup-page/station-mode
cycle without rebooting. A physical button press and a long-duration stream
soak remain separate hardware checks.
