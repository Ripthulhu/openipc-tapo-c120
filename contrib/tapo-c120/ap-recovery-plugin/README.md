# OpenIPC C120 AP Recovery Plugin

Post-flash recovery add-on for the TP-Link Tapo C120 OpenIPC build.

This deliberately lives outside the base image. The flashable OpenIPC build can
stay small and simple, then this bundle can be installed after flashing on any
C120 that should have physical reset-button Wi-Fi recovery.

## What It Adds

- Single resident GPIO event helper for reset-button AP toggle and light-pin
  mirroring.
- Reset button toggle on GPIO9, active-low.
- Open setup AP named `C120-Setup`.
- Static setup address `192.168.4.1`.
- DHCP leases `192.168.4.20` through `192.168.4.200`.
- Web Wi-Fi setup page at `http://192.168.4.1/cgi-bin/c120-wifi-setup.cgi`.
- SSH availability on `root@192.168.4.1` while the AP is active.
- Multi-network station fallback from U-Boot env:
  `wlanssid/wlanpass`, `wlanssid2/wlanpass2`, and `wlanssid3/wlanpass3`.

AP mode is button controlled only. Hold reset for about one second to enter AP
mode; hold it again for about one second to return to normal Wi-Fi station mode.
There is no timer.

To keep AP mode stable on the 32 MB Linux memory budget, the AP helper stops
Majestic, NTP, and cron while setup mode is active. Dropbear and `c120-eventd`
stay running, so the button can still leave AP mode. Stopped services are
restarted when station mode returns.

## Install

Copy the packaged tarball to the camera, extract it, and run the installer:

```sh
scp openipc-c120-ap-recovery-plugin-20260521.tgz root@<CAMERA_IP>:/tmp/
ssh root@<CAMERA_IP>
cd /tmp
gzip -dc openipc-c120-ap-recovery-plugin-20260521.tgz | tar -xf -
cd openipc-c120-ap-recovery-plugin
sh ./install.sh
```

The installer is idempotent. It backs up the previous `/etc/network/interfaces.d/wlan0`
to `/root/c120-ap-recovery-backups/` and removes old `wlan0.*` backup files from
`interfaces.d` so they are not parsed as extra interfaces.

Check the install state:

```sh
sh /tmp/openipc-c120-ap-recovery-plugin/install.sh status
```

## Configure Rescue Networks

Set credentials in U-Boot env. Do not put secrets in this repository or bundle.

```sh
fw_setenv wlanssid '<PRIMARY_SSID>'
fw_setenv wlanpass '<PRIMARY_PSK>'
fw_setenv wlanssid2 '<RESCUE_SSID>'
fw_setenv wlanpass2 '<RESCUE_PSK>'
```

If the primary network is wrong later, bring up the rescue SSID on a phone or
travel router, wait for DHCP, SSH in, and fix `wlanssid` / `wlanpass`.

## Runtime Files

```text
/usr/bin/c120-setup-ap
/usr/bin/c120-eventd
/usr/bin/c120-button-apd
/usr/bin/c120-wifi-conf
/etc/init.d/S45c120-ap-button
/etc/network/interfaces.d/wlan0
/var/www/cgi-bin/c120-wifi-setup.cgi
/usr/sbin/hostapd
/usr/bin/hostapd_cli
/usr/lib/libnl-3.so.200
/usr/lib/libnl-genl-3.so.200
```

Hostapd overlay SHA256:

```text
a856e3ed876757580e24a3e7bd748c2d8e5da0176fae040191e64dd634b16d52  hostapd-overlay.tgz
```
