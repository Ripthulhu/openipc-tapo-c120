# Older SC430AI C120 V1

Offline qualification, 2026-10-07. No stock SC430AI device was available, and
no existing OpenIPC camera was changed. This path requires experimental opt-in
until an owner completes a full migration, setup and reboot on hardware.

## Saved Stock Evidence

The 2026-05 dumps identify C120 hardware 1.0, stock
`1.4.1 Build 250910 Rel.58576n`, Linux 5.10.61 and the running module
`drv_ms_cus_sc430ai_MIPI_tp_ww`.

| Input | SHA-256 |
| --- | --- |
| Stock `/bin/main` | `6ea2fd02fa952dd998e433405c797a4b7686714f9eafabcd898e91dfa38adbef` |
| Stock `/bin/monitor` | `3aca2a2bc48c0b23f2d329c3ed8d26e23686020f7bd29c6e77a36d5880a87ac0` |
| Decompressed stock kernel | `820d75618836c5593b1053ba9541d2d1d04f461bcdc1942c09a2f2876a9977cf` |

In that main binary, the `set_info` registration references Thumb handler
`0x443ac`. Its factory-mode guard precedes the type/data parsing; type 14 at
`0x4445c` passes the data to the command helper at `0x440e0`. The helper
formats a bounded 256-byte command and executes it through `0x4408c`.
An empty command result returns the same -40101 response expected by the
installer. This permits the existing short-lived factory batch without
writing the device name or changing region codes. This is static evidence,
not a new live confirmation of that batch on 1.4.1.

The old kernel's physical-read dispatcher at `0xc019f7e2` copies a 16-byte
request with offset, userspace buffer and length at offsets 0, 4 and 8.
Its `0x8001df00` table entry selects `0xc019f960`, limits reads to 64 KiB
and copies the result to userspace. That read branch is byte-identical to the
1.4.4 kernel branch shifted by 0x240 bytes. The saved device listing exposes
`/dev/slp_flash_chrdev`; the installer uses fresh physical reads, not the
header-substituted MTD export.

The saved partition map matches the writer's required physical layout:

```text
mtd0:  00030000 00001000 "factory_boot"
mtd1:  00010000 00001000 "factory_info"
mtd8:  00080000 00010000 "user_record"
mtd15: 00fc0000 00001000 "af"
```

The old runtime mounted mtdblock8 at `/tmp/usr_def_audio`, with the same
`/bin/main` and `/bin/monitor` services. Watchdog descriptor discovery now
matches the actual device instead of assuming stock always uses descriptor 6.
The independent feeder still normalizes its own descriptor to 6, and the
existing freeze, read-only mounts, memory checks and watchdog observation stay.

## Checks And Limits

Both dumped BusyBox versions transferred a byte-exact 16 KiB test file using
a high UDP port under QEMU. This exercises the actual stock TFTP clients,
against loopback only; it does not emulate Wi-Fi, SD hardware or flash.
Reproduce with an extracted stock rootfs preserving its symlinks:

```sh
C120_STOCK_ROOT=/path/to/rootfs python3 test_c120_tftp.py
```

The SC430AI image is the 2026-10-04 QHD/30 build, with its own source driver
and IQ. Packaging checks the actual SquashFS links, executable interpreters
and first-boot sensor/video settings. File hashes are in `kit/manifest.json`.
The exact image has not been boot-tested, although SC430AI operation and QHD/30
were previously tested on the older cameras.

Before enabling a new stock build, qualify its command handler, sensor module,
physical-read ABI, mounts and partition map. Never broaden the version allowlist
solely because the casing says C120 V1. A full live trial must also verify the
private physical backup, completed write/read-back, owner setup, video/audio
and a second boot. Saved dumps cannot prove those steps or power-loss safety.
