# C120 Stock AI Bring-Up

Experimental diagnostic, **not an installed AI feature or a daemon**. Tested on
one SSC377/SC430AI C120 running the 2026-10-04 firmware. No boot scripts, Majestic
configuration, global libraries, or flash partitions are changed by the probe.

This is a historical bring-up report. The installed feature is now the separate
[AI Detection plugin](../ai-plugin/README.md), whose status supersedes the original
integration plan below. Stop that service and verify it has exited before using
the probe; both use the same IPU and SCL resources.

## Verified on 2026-10-05

- The stock object model runs on the existing IPU kernel driver. No kernel
  replacement or stock `main` process is needed.
- Live camera inference works using unused SCL device 0/channel 0/port 2,
  scaled to 800x448 NV12. Ports 0 (main), 1 (JPEG), and 3 (motion) stay intact.
- 100 live inferences: mean 50.491 ms, maximum 54.032 ms, with a 200 ms pause
  between samples. This is an initial trial, not a long-term stability test.
- Known reference images produced people at 0.85/0.78/0.72 confidence, a bus at
  0.66, and a dog at 0.69. These few images do not establish general accuracy.
- The sound model loads and executes on the IPU, producing seven scores from a
  synthetic input tensor in 1-9 ms. **Actual sound classification is not verified.**
- Main video settings remained H.264 2560x1440, 30 fps, 6000 kbps. Main SCL output
  reported 30 fps after the live test. Majestic PID, boot ID and complete settings
  were unchanged. No RTSP packet-loss/latency soak test has been completed.
- The probe's process RSS was about 704 KiB. The IPU and AI image buffers also
  consume roughly 7.5 MiB of the separate 32 MiB media-memory pool; RSS alone is
  **not** the total memory cost. About 5.5 MiB media memory remained in the trial.
- Linux MemAvailable was about 4.2 MiB during the trial, including the 4.29 MiB
  model staged in `/tmp`, and about 8.6 MiB after cleanup. A flash-backed model
  avoids keeping that extra temporary copy, but still needs a memory guard.

At the time of this probe, the owner's second camera was inspected only and no
AI service had been installed. Later deployments are documented in the plugin guide.

## Runtime Compatibility

The OpenIPC 2022-09-07 IPU library and the stock 2023-02-08 IPU library use an older
CreateDevice ioctl payload (24 bytes). The installed 2024-06-18 `mi.ko` expects the
newer 12-byte payload. The old library fails with error 5 and misleading allocation
errors. Do not diagnose this as insufficient RAM or replace the entire media stack.

Working userspace IPU library:

- Repository: <https://github.com/johnchia/sigmastar-lib>
- Commit: `cd3c621fdb9e60e7f567ec6146af44ebeddc2630`
- Path: `infinity6c/lib/1230/uclibc/9.1.0/libmi_ipu.so`
- Build: 2024-12-30, project `8e24a5b`, SDK `9e4d64f`, MHAL `da23dd9`.
- Size: 38,448 bytes.
- SHA256: `e78f2d9ad9000ce6adedef7686eb1d30625b2259bd3c86d0c2ab2a77ab1a47e3`.

This is a vendor binary, not rebuilt open source. Review its upstream provenance
and redistribution terms before distribution. The model binaries below are also
proprietary stock assets: the diagnostic source does not redistribute them.

Only the probe's private library directory uses this IPU library. The other small
vendor dependencies come from the matching OpenIPC Infinity6C package. The tool
links the SDK uClibc compatibility archive and library. Its two backtrace stubs
disable an unavailable optional crash trace; they do not hook another process.

The optional-model-extension warning is printed even for the working models.
Judge success from CreateDevice, CreateCHN, Invoke and cleanup return codes.

## Stock Assets and ABI

From stock `/etc/plugins`:

| File | Bytes | SHA256 |
| --- | ---: | --- |
| `obj_detection.sim_sgsimg.img` | 4501504 | `07c8ffa24e9d028bf4c5bc1f5656f5a4baa958f0d5063963917e0b3f714cfb7d` |
| `sed.sim_sgsimg8k.img` | 480640 | `72bdb31f95b892155b84c212c61e4774b85ccc314403b797dafea67d3850e8aa` |
| `pcls.tflite_sgsimg.img` | 413696 | `92682555e80945b7e50a58de125800bc9995aa809c4dd0533a66ddff2216e7e9` |

Object model: variable memory 1,687,552 bytes; input NV12 800x448, 537,600 bytes.
Four float32 outputs already contain boxes (50x4), classes (50), scores (50), and
count (1). The stock wrapper swaps y/x into x/y: native box order is
`ymin,xmin,ymax,xmax`, normalized. Clamp coordinates before drawing.

Stock descriptor: YOLOV6, confidence 0.25, NMS 0.45,
`class_labels=[0,2,3,4,7,8]`, `class_ids=[0,1,1,9,1,2]`.
Do not substitute COCO labels. Reference images verified output 0 for people,
2 for a bus and 4 for a dog. The complete class taxonomy and descriptor's
application-ID mapping still need confirmation; do not publish guessed labels.

Sound model: variable memory 81,920 bytes; int16 input `[1,64,101]`, 12,928 bytes,
quantization scale 0.0030518509 and zero point 0. Output is seven float32 scores.
Stock configuration uses 8 kHz mono, hop 160, window 512, 64 features, 101 frames,
step 12. The preprocessing, normalization and seven-label ordering must be
reproduced from stock before any bark/meow/cry/glass/alarm event is trustworthy.
The exported `libAED_LINUX` functions are a separate AED/LSD interface, not a
ready-made replacement for the DLA feature pipeline.

The optional 192x192 PCLS model was located, but has not been runtime-tested.

## Build and Run

Build on Linux using an already-built Infinity6C firmware tree:

```sh
sh contrib/tapo-c120/ai-probe/build.sh FIRMWARE_BUILD LIBMI_IPU_1230_PATH OUTPUT
```

Copy the output and **one** stock model to a private test directory. Check Linux
MemAvailable before staging: leave at least 4 MiB after the model copy, plus
space for the small probe libraries. Check media-pool headroom separately.
Do not stage the whole stock firmware or all models in `/tmp`.

In the private directory on the camera:

```sh
LD_LIBRARY_PATH="$PWD/lib" ./ipu-probe model.img
LD_LIBRARY_PATH="$PWD/lib" ./ipu-probe model.img --load
LD_LIBRARY_PATH="$PWD/lib" ./ipu-probe model.img --invoke
LD_LIBRARY_PATH="$PWD/lib" ./ipu-probe model.img --input < input.tensor
```

`--invoke` uses a zero-filled synthetic tensor, not microphone or video data.
`--input` accepts exactly the model's aligned input size on stdin. Test images:
<https://github.com/ultralytics/assets/blob/main/im/bus.jpg> and
<https://github.com/pytorch/hub/blob/master/images/dog.jpg>, converted with FFmpeg
`-vf scale=800:448 -pix_fmt nv12 -f rawvideo`.

For the object model only, **first verify port 2 is disabled and unbound** in
`/proc/mi_modules/mi_scl/mi_scl0`, and ensure only one probe runs:

```sh
LD_LIBRARY_PATH="$PWD/lib" ./ipu-probe model.img --live 100
```

The caller must enforce this port ownership check. The probe is not suitable for
unattended operation: its 40-second emergency alarm terminates a hung vendor call
without guaranteed port cleanup. Normal completion releases every tensor,
disables the AI port and destroys the IPU channel/device. When the port previously
had no valid dimensions, its new dimensions remain inert while it is disabled;
restoring a zero-size configuration is rejected by the driver.

Delete only the staged model after testing. Verify the extra port is disabled,
MMA memory is released, main output remains 30 fps, and Majestic has not restarted.

## Original Integration Plan

1. A separate opt-in C120 plugin with bounded inference rate and no global library
   replacement. Prefer flash-backed model reads; budget compressed flash usage
   plus update/recovery headroom. The object model alone gzip-compresses to 2,919,581
   bytes; JFFS2 usage is not identical to gzip size.
2. Port ownership, restart handling, signal cleanup and memory safeguards. Stop AI
   during AP recovery, firmware updates or conflicting media reconfiguration.
3. Validated class labels, confidence filtering, motion-region geometry and short
   event persistence, exposed through authenticated JSON/metrics and WebUI settings.
4. Exact stock audio feature extraction and labelled audio-fixture tests. Retain
   the existing 48 kHz microphone stream and resample a private analysis branch.
5. Combined object/audio memory tests, RTSP and talkback regression checks, and a
   longer thermal/reboot/stream stability test before enabling it on both cameras.

No Python, ONNX or generic neural-network framework is needed on the camera.
