# MP3 Tools

Optional LAME 3.100 command-line encoder and `libmp3lame.so.0` for the musl
ARM hard-float C120 firmware. Nothing runs until invoked; no audio/video
settings, startup services or flash partitions are changed. The package uses
the writable overlay and survives a normal settings-preserving firmware update.

This does **not** add `/audio.mp3` or `audio.codec: mp3` to Lite Majestic.
Ultimate Majestic is not published for Infinity6C. Keep the microphone on
Opus and use the encoder for an existing WAV/PCM file on the SD card. Do not
use `/audio.pcm`: the tested Majestic build can crash on that capture route.

It also does not add MP3 upload support to `/play_audio`. That endpoint accepts
raw signed 16-bit little-endian PCM or Ogg Opus via HTTP POST, not a browser
page. Convert MP3 on the sending computer, then upload the converted file:

```sh
ffmpeg -i sound.mp3 -ac 1 -ar 48000 -f s16le sound.pcm
curl -u root -H 'Content-Type: application/octet-stream;rate=48000;channels=1' \
    --data-binary @sound.pcm http://CAMERA_IP/play_audio
```

`curl` prompts for the camera password. For an existing Ogg Opus file, send it
with `Content-Type: audio/ogg` instead. See the upstream
[speaker playback documentation](https://github.com/OpenIPC/wiki/blob/master/en/majestic-streamer.md#how-to-play-audio-file-on-cameras-speaker-over-network).

## Build and Install

Use an already built C120 firmware tree with its matching musl toolchain:

```sh
make BOARD=ssc377_lite_tp-link-tapo-c120-v1 br-lame
sh contrib/tapo-c120/mp3-tools/package.sh output /path/to/new-package
```

For a separate Buildroot output, pass its path to `package.sh`. This uses
Buildroot's standard `lame` package (which includes the command-line frontend),
not OpenIPC's library-only `lame-openipc` package. Buildroot verifies the
3.100 source archive SHA-256:
`ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`.

Extract `openipc-mp3-tools.tgz` to a directory on the camera's SD card and run
`sh install.sh` there as root. The installer checks hashes, compatibility and
flash headroom, and refuses to replace different existing MP3 tools. This
keeps the existing 5 MiB rootfs partition and optional plugin settings intact.

Encode a WAV file already on SD, using your card's actual mount point:

```sh
nice -n 10 lame --silent -m m -b 64 /mnt/mmcblk0p1/input.wav /mnt/mmcblk0p1/output.mp3
```

Use SD for real audio files, not `/tmp` or flash. Encoding consumes CPU and RAM
while running; real-time encoding alongside all AI/recording features is not
guaranteed. Synthetic on-device encoding is verified without recording room
audio. Native recording continues to use the camera's configured Opus audio.

LAME is LGPL-2.0-or-later; `COPYING` is installed with the library. Its
[corresponding upstream source](https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz)
is the archive verified by Buildroot above. The shared library remains separate
and replaceable; the packager strips symbols, not source-code changes.
