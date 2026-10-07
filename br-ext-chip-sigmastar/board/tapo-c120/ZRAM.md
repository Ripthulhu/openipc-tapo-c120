# Tapo C120 zram

Both C120 sensor variants enable one RAM-only zram swap device at boot. It has a
16 MiB logical size, an 8 MiB zram memory limit, and LZ4 compression. Creating
the device does not reserve 16 MiB of RAM; pages consume memory as they are
swapped. To opt out on the next boot, create `/etc/zram.disabled` in the writable
overlay.

One SC430AI camera reported 27,784 KiB `MemTotal`, 8,960 KiB `MemAvailable`,
2,404 KiB `AnonPages`, and no swap on 2026-10-07. These are a single read-only
snapshot, not a measured zram benefit. Zram may preserve more file cache or avert
an out-of-memory event when anonymous memory grows, but compression consumes CPU
on the single Cortex-A35 and can add latency. It cannot recover the video/ISP
memory reserved outside Linux, or prevent allocation failures in those pools.

Both sensor variants passed a hardware smoke test on 2026-10-07: flash readback,
active LZ4 zram swap with the intended sizes, preserved settings, and a 30-second
2560x1440 stream with video and audio. Long-running stability and memory benefit
remain unmeasured. With the same 2560x1440, 30 fps, 10,000 kb/s stream and the
same workload, compare `/proc/meminfo`, `/proc/vmstat` (`pswpin`, `pswpout`, major faults),
`/sys/block/zram0/mm_stat` (original bytes versus actual memory used), CPU load,
frame drops, and stream latency before and after. A benefit requires swapped
original bytes to exceed zram memory used without unacceptable CPU or stream
latency cost. The option does not configure writeback or any SD/flash swap.

See the [Linux 5.10 zram guide](https://www.kernel.org/doc/html/v5.10/admin-guide/blockdev/zram.html)
for the sysfs settings and memory accounting fields.
