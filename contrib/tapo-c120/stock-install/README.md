# Stock C120 to OpenIPC

## Next Camera

Download or clone this repository, pair the camera in Tapo on 2.4 GHz Wi-Fi,
and insert a camera-formatted SD card with at least 64 MiB free. From the
repository folder on Windows:

```powershell
.\contrib\tapo-c120\stock-install\c120-install.cmd CAMERA_IP
```

On Linux or macOS:

```sh
sh contrib/tapo-c120/stock-install/c120-install CAMERA_IP
```

The sensor-specific firmware is already in `kit/`; there is nothing to build or copy to
the card by hand. Enter your account and Wi-Fi details when prompted, confirm
the verified backup, then complete OpenIPC's setup page yourself.

One-time requirements: Python 3.11+ with `venv` and `pip`, plus internet access
to install the pinned Python dependencies. Normal installation needs **no WSL,
root/administrator access, compiler, SquashFS tools or distro package manager**.
Use `python` on Windows or `python3` on Linux/macOS. The POSIX launcher also
accepts `PYTHON=/path/to/python3`. A missing Python/venv must be supplied by the
owner using their platform's normal installation method.
Run the command with `--check-kit` instead of `CAMERA_IP` to check the PC and
firmware without contacting a camera. A failed check stops before login.

Keep power and the SD card connected until the camera has booted. This is a
verified single-bank flash, not a guarantee against hardware or power failure;
UART recovery may still be needed if something goes wrong.

## Supported Devices

Both variants report C120 hardware v1.0; that label alone does not identify
the sensor. The installer selects it automatically:

| Sensor | Exact stock version | Qualification |
| --- | --- | --- |
| SC438HAI | `1.4.4 Build 260106 Rel.62350n` | Complete UART-free migration tested |
| SC430AI (older C120 V1) | `1.4.1 Build 250910 Rel.58576n` | Dump-qualified; live migration pending |

The older variant requires `--allow-experimental-sc430ai`, including for
`--check-only`. Without that acknowledgement it stops before preparation.
It uses its own SC430AI kernel/rootfs, driver, IQ and 2560x1440/30 fps defaults.
This is not a claim that every C120 V1 or every stock release is supported.
In particular, an older sensor with a different stock build is not silently
accepted. See [COMPATIBILITY.md](COMPATIBILITY.md) for the offline evidence.

The installer checks the exact stock application, running sensor module, MAC address and flash layout.
Other versions stop before flashing. Existing OpenIPC cameras are not targets.

Hardware validation on 2026-10-07: a previously stock unit at `.152` completed
the full migration without UART. Its 16 MiB physical backup was verified on
the PC, the complete written image passed physical read-back, and OpenIPC
booted and rejoined Wi-Fi. The owner completed setup. A second software reboot
retained the password, MAC, sensor selection and byte-identical video settings.
Both settled stream tests decoded 899 H.264 frames over 30 seconds at
2560x1440/30 fps with Opus 48 kHz audio. An earlier post-reboot sample decoded
620 frames during concurrent browser reconnections; the repeat was clean.
This is bounded testing, not a long-term stability claim or a new cold-power-cycle test.

The refreshed SC438HAI image uses the source-built driver tested in linear
QHD30 mode on cam4, not the incompatible stock module. Both images pass executable, interpreter, symlink
and sensor checks during packaging. The installer pins the entire kit manifest
and verifies every file plus image headers, CRCs and layout before login.
Changing a file and its manifest checksum together does not bypass that pin.
The portable launcher and descriptor-discovering watchdog helper were tested
offline after the original hardware trial; the revised installer has not been
used to reflash an existing OpenIPC camera.

## Install

1. Pair the new camera normally in Tapo and connect it to 2.4 GHz Wi-Fi.
2. Insert a working, camera-formatted SD card with at least 64 MiB free.
   Keep the camera powered and the card inserted throughout installation.
3. Open a terminal in the repository folder and run the launcher above.
   For an older SC430AI camera on the exact supported stock build:

   ```powershell
   .\contrib\tapo-c120\stock-install\c120-install.cmd CAMERA_IP --allow-experimental-sc430ai
   ```

4. Enter the Tapo account credentials, then the Wi-Fi name and password to
   use after installation. Review the camera MAC and type the exact `FLASH`
   confirmation only when the verified backup and preflight are complete.
5. Wait for the OpenIPC setup URL. Review the licence and set your root
   password yourself. The installer does not accept the licence for you.
6. Verify picture, audio and network access before moving the camera.

The first launcher run creates a private Python environment and installs
the pinned cryptography, Paramiko and tftpy dependencies. Subsequent runs
check those requirements without upgrading them. The camera must reach this
PC over TFTP (**UDP 1069** and the negotiated transfer port).
`--tftp-port PORT` selects another unprivileged port (1024-65535).
`--bind PC_IP` selects the reachable PC address
when automatic route selection is unsuitable. Use only a trusted local network.

`--check-kit` only validates local artifacts. `--check-only` stages temporary,
key-only recovery SSH and checks the camera, but neither freezes services nor
writes flash. `--resume PRIVATE_RUN_FOLDER` can continue that preparation on
the same camera without another transfer. It refuses a different camera and
any run which has already started the watchdog, paused stock services, made a
physical backup or attempted flashing. Otherwise reboot before a fresh run;
the recovery folder intentionally cannot be reused accidentally.

## What Is Preserved

Each run gets a new ignored folder under `stock-install/private-runs/` and a fresh
SD staging directory. The full **physical** stock flash backup is copied to
this PC and its SHA-256 verified before flashing. Keep that folder: it contains
camera credentials, unique factory data and the temporary recovery key.
Never publish it or use one camera's backup to restore another.

Normal stock MTD reads substitute part of the rootfs header, so they are not
accepted as a physical backup. Recovery uses the vendor physical-read ioctl.
Stock services are paused, their flash filesystem remounted read-only, and an
independent watchdog feeder is observed for two watchdog periods first.
The feeder discovers and duplicates the stock monitor's existing watchdog descriptor using
the kernel's [pidfd_getfd API](https://man7.org/linux/man-pages/man2/pidfd_getfd.2.html).
It does not depend on the command shell inheriting that descriptor, nor try
to reopen an exclusively owned watchdog device.

The replacement is streamed from SD using a RAM-only recovery shell. It writes
and verifies the body before the bootloader, verifies the complete physical
image, then reboots. A write/verification failure attempts a full stock restore.
If restore verification fails it does **not** reboot. Do not disconnect power
or run a second installer when the outcome is uncertain.

This is not a signed Tapo update, nor power-loss-safe dual-bank flashing.
It replaces the bootloader. A power failure, defective flash or an unforeseen
boot problem can still require UART recovery.

## Rebuild And Test

Normal installations use the included kit, not a download of whatever firmware
happens to be latest. Both images were rebuilt on 2026-10-07 against firmware
`5650e029`, with Majestic `0ea3123` and WebUI `a6c7cf9`. Both retain QHD/30 fps
and the current speaker and automatic day/night defaults. The refreshed
SC438HAI image passed an OpenIPC-to-OpenIPC upgrade, two boots and two settled
30-second stream checks on cam4. The original stock-to-OpenIPC trial above
used the preceding image; the refreshed kit has not repeated that migration.
The SC430AI image passed offline checks only and remains experimental.
See the [refresh evidence](../HISTORY.md#upstream-refresh-2026-10-07).
Optional Lights, Wi-Fi recovery and AI plugins are installed separately; see
the [C120 guide](../README.md#installable-extras).

Maintainers can package newly validated artifacts with
`python c120_build_install_kit.py --source INPUT_KIT --output NEW_DIRECTORY`.
This **maintainer build step**, unlike installation, needs Linux, `unsquashfs`
and an OpenIPC ARM cross-compiler selected with `--cc`. The input uses the
same layout as `kit/`, including `sc430ai/` kernel/rootfs images. The builder
sanitizes Wi-Fi/MAC values, compiles both recovery helpers from source, validates
both root filesystems and prints the new manifest hash. Review the artifacts
and tests before updating `MANIFEST_SHA256`; packaging alone is not hardware
validation. Private stock dumps are not included in the repository.

Run these checks from this `stock-install` directory:

```powershell
python test_c120_install.py
python test_c120_prepare_raw.py
python test_tapo_client.py
python test_c120_tftp.py
.\c120-install.cmd --check-kit
```

On Linux also run `python3 test_c120_stock_recovery_flash.py` and
`python3 test_c120_rescue_guard.py`. The portable CI matrix covers Ubuntu,
Windows and macOS; the whole runtime suite also validates the boot files.
Local verification was performed on Windows and Linux, not macOS.

Tests cover both sensor profiles, kit pinning, malformed identities, command
lengths, AES compatibility, factory-mode cleanup, read-only TFTP, binary read-back,
truncated backups, cancellation, preflight failure, flash order and rollback.
The previous broken rootfs is rejected by the startup checker. A successful
test run alone is not proof that a physical camera boots and streams.

## Troubleshooting

- No TFTP progress: check PC-to-camera and camera-to-PC routing and Windows
  firewall rules for the installer Python process. Try `--bind PC_IP` on a
  multi-network PC. Do not disable the firewall globally.
- Login rejected: verify the account in Tapo and wait out any reported lockout.
  The installer deliberately does not retry passwords.
- Unsupported sensor or version: stop; do not bypass the identity checks or
  substitute another camera's backup.
- Connection lost after flashing started: keep power and the SD card connected.
  Do not start another installer or reboot blindly. Keep the private run folder
  and SD `physical-flash.log`; check DHCP for the camera's printed MAC address.
- Setup completed: verify live video/audio, reboot once through OpenIPC, and
  check again. For a later physical power cycle, leave power off at least five
  seconds. Keep the original stock backup somewhere private and durable.
