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

The SC430AI IQ/config blob used by the working cameras is included at:

```text
general/package/sigmastar-osdrv-infinity6c/files/sensor/configs/sc430ai.bin
```

The package also tolerates building without that file, but the working cameras
were validated with this blob present as `/etc/sensors/sc430ai.bin`.

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

`c120-eventd.c` is the draft native daemon source kept for future RAM reduction.
The currently deployed/tested helper is still the shell version in the overlay.
