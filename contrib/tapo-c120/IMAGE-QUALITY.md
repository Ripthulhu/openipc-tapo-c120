# C120 image quality and ISP tuning

The C120's native Majestic `isp.wdr` control changes ISP tone mapping while
the sensor stays in linear mode. On cam4 with the SC438HAI, the stock IQ
profile preserved more shadow detail than the manual strengths tested on
2026-10-07. Keep automatic stock tuning unless a comparison in the installed
scene demonstrates an improvement. A missing `wdr` value in the current
configuration does not mean WDR is unavailable or disabled.

## Supported controls

Use `/api/v1/config.schema.json` to discover controls on the running build;
`/api/v1/config.json` contains current values and omits optional unset keys.
All four audited Infinity6c cameras advertise `isp.wdr`, `isp.antiFlicker`,
`isp.exposure`, and `isp.aGain`. The native Settings page already exposes
these controls; an additional tuning service is unnecessary.

| Control | Guidance |
| --- | --- |
| WDR | ISP tone mapping, independent of sensor HDR. The bundled ISP XML describes strength values from 0 to 255, but the native schema does not publish bounds. Manual values replace the profile's behavior; do not assume a small value improves shadows. |
| Anti-flicker | `50` matches Dutch mains lighting; `60` is for 60 Hz lighting. `disabled` leaves the ISP unconstrained by this setting. Check under the actual lights because LED PWM and monitor refresh can produce different artifacts. |
| Exposure | Retain the tested 33 ms maximum for QHD30. Raising it can reduce actual frame delivery. |
| Analog gain | The schema offers a maximum gain multiple. Leave it unset unless a low-light comparison justifies a cap; a cap can darken the image and interfere with the 16x automatic-night threshold. |
| Contrast, luminance, saturation, hue | Retain neutral 50 values as the starting point. Contrast or brightness adjustments cannot recover information clipped at the sensor. |
| Sharpening and noise reduction | These builds expose no direct `isp.sharpen` or `isp.nr` keys. Preserve the matching stock IQ profile; do not copy another vendor's controls or switch sensor profiles to obtain a slider. |
| Automatic day/night | Retain the native 16x night / 2x day policy, 15 / 60 second delays, automatic IR-cut and illuminators, and grayscale night mode. Scene-specific dawn/dusk tests remain useful. |

The schema also lists `nightMode.overrideDrc`. Its presence alone does not
establish a working DRC backend on these cameras; this audit did not validate
that override. The daytime `isp.drc` control is absent from their schemas.

## WDR compared with sensor HDR

ISP WDR redistributes the brightness already captured in a linear frame. It
cannot recover clipped sensor samples or create a second exposure.
SC430AI's deployed source remains linear-only. The SC438HAI source contains
experimental HDR planes, but Majestic selects the linear capture pipeline;
HDR wiring, runtime integration and suitable HDR IQ remain unvalidated.
True sensor HDR integration was stopped at the user's request. The WDR
setting does not enable it.

## Verified camera state

This section records the image-quality audit before the later native-resolution
rollout. See [verification history](HISTORY.md) for subsequent fleet changes.

All four cameras retained 2560x1440 H.264 output at 30 fps. The audit found
6000 kbit/s on cam1, cam2 and cam3, and 10000 kbit/s on cam4. Those existing
bitrates were preserved. Cam1/2 use SC430AI; cam3/4 use SC438HAI.

Cam1, cam2 and cam3 received no setting writes from this audit. Cam4 retained
only `isp.antiFlicker: '50'` instead of `disabled`. Exposure, gain limits,
colour, orientation, day/night, audio and every other effective setting were
compared and preserved. Stock automatic WDR was restored after the trial.
No firmware, libraries, model, MQTT, recording or dashboard files changed.

## Cam4 comparison and validation

The comparison used the same JPEG endpoint and quality, with exposure near
29.981 ms and reported ISP frame rate 30 throughout. These figures describe
the particular indoor scene; they are not a universal image-quality score.

| WDR state | Mean grayscale value | 5th percentile grayscale value |
| --- | ---: | ---: |
| Original stock profile | 109.21 | 28 |
| Manual 0 | 74.30 | 13 |
| Manual 32 | 84.89 | 17 |
| Manual 96 | 92.52 | 13 |
| Manual 128 | 94.98 | 13 |
| Manual 224 | 102.81 | 16 |
| Restored stock profile with 50 Hz anti-flicker | 109.56 | 28 |

Manual strengths visibly changed the tone curve without restarting Majestic.
The stock profile retained brighter shadows. The test did not demonstrate
a measurable reduction in rolling bands from anti-flicker; it established
that the setting persisted and the stream remained stable under those lights.

After restoring the stock IQ, an independent 30-second RTSP decode read
898 H.264 frames at 2560x1440 and a declared 30/1 rate, plus 1500 Opus frames
at 48 kHz. The AI service was running. The existing memory dashboard script
and CGI remained unchanged and responded after the restart. This bounded
check does not replace overnight or moving-subject testing.

## Safe changes and rollback

Back up `/etc/majestic.yaml` privately and record the effective configuration
before a trial. Change one image control at a time and compare exposure,
gain, clipping, shadow detail and a stream decode before keeping it.

These commands run locally on the camera through an authenticated shell:

```sh
# Example manual ISP tone-mapping trial, not a recommended universal value.
curl -sf -H 'Content-Type: application/json' \
  --data '{"isp":{"wdr":96}}' http://127.0.0.1/api/v1/config

# Remove the saved override to return to the stock profile at initialization.
curl -sf -H 'Content-Type: application/json' \
  --data '{"isp":{"wdr":null}}' http://127.0.0.1/api/v1/config
```

On the tested cam4 build, removing the saved key left the last manual tone
curve active. One controlled `/etc/init.d/S95majestic restart` reloaded the
stock IQ and restored the original shadows. Coordinate this brief stream
interruption with other camera work. Do not restore an entire old YAML over
unrelated changes; restore only the keys tested. A previously saved manual
WDR value should be restored to that value rather than deleted.

To undo cam4's retained anti-flicker change, use the native Settings page or:

```sh
curl -sf -H 'Content-Type: application/json' \
  --data '{"isp":{"antiFlicker":"disabled"}}' http://127.0.0.1/api/v1/config
```

The [upstream exposure and anti-flicker documentation](https://github.com/OpenIPC/wiki/blob/master/en/majestic-streamer.md#anti-flicker-matching-the-shutter-to-mains-lighting)
explains shutter constraints and the metric units. Always prefer the camera's
own schema over examples for other SoC families.
