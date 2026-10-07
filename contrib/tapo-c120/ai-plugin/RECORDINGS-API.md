# Recording Automation API

**Validation note (2026-10-07):** cam4's HTTP MP4 stall is avoided with
`video0.gopSize: "0.5"`, now the C120 first-boot default. QHD, 30 fps and
10,000 kbit/s remain unchanged. A 30-second HTTP clip and a synthetic AI-triggered
SD clip passed; the actual visual detector is still behind its media-memory
guard, so end-to-end recognition-to-recording there is not yet validated.
See [verification history](../HISTORY.md#http-mp4-stall-2026-10-07).

`GET /cgi-bin/c120-recordings-api.cgi` is a read-only, versioned catalogue for
automation clients. Install the AI plugin; no additional server, database daemon,
encoder, account or video-quality change is required. Recording and outbound
webhooks remain off until configured. Existing recording and AI APIs are unchanged.

## Existing APIs

| Endpoint | Purpose |
| --- | --- |
| `/api/v1/config.json` | Majestic configuration, including `records` |
| `/api/v1/config` (POST) | Partial Majestic configuration update |
| `/cgi-bin/j/recordings.cgi?days=1` | Web UI's day/file browser, not a durable event feed |
| `/api/v1/records` (POST) | Native recording pause control, not a clip list |
| `/api/v1/analytics/day?d=...` | Native motion/face analytics, not plugin AI categories |
| `/cgi-bin/c120-ai-api.cgi` | AI settings/live detections; POST requires its CSRF token |
| `/cgi-bin/c120-recordings-api.cgi` | Completed clips, stable IDs, metadata and media URLs |

Use the existing **root** login over local HTTPS, or a trusted LAN/VPN. The API
does not create an unauthenticated endpoint or separate read-only account. The
camera's ordinary media-only users cannot use this administrative CGI. Do not
expose it directly to the Internet. Recovery AP mode returns 503.

```sh
curl --anyauth --user root https://cam.example/cgi-bin/c120-recordings-api.cgi
curl --anyauth --user root 'https://cam.example/cgi-bin/c120-recordings-api.cgi?category=pet&limit=20'
curl --anyauth --user root 'https://cam.example/cgi-bin/c120-recordings-api.cgi?id=0123456789abcdef-00000000000000000001'
```

## Polling and Downloads

The list response contains `schemaVersion: 1`, `cameraId`, `storageGeneration`,
`recordings`, `nextCursor` and `hasMore`. It is ordered by catalogue completion
sequence, oldest first. Store `nextCursor` only after processing the entire page;
request it as `?cursor=...`. Continue immediately while `hasMore` is true, then
poll again later. The default page is 50 clips; `limit` is 1 through 200.

Filters are `category` (person, pet, vehicle, bird, bark, meow, cry or glass),
`source` (ai, manual, native or imported), and `after`/`before` (exclusive UTC ISO-8601
completion times, e.g. `2026-10-07T12:00:00Z`). Keep filters fixed while using a
cursor. Start a new scan to change filters. `id` selects one clip and cannot be
combined with other parameters. Unknown/duplicate parameters are rejected.

IDs and cursors survive reboots. Another card or a newly created catalogue gets a
new generation: old cursors return **410**, requiring an explicit fresh scan.
Deletion does not reset the sequence. Polling cannot retrieve media already
deleted by retention. A disconnected/read-only/missing or damaged catalogue
returns **503**, invalid requests **400**, and a missing individual clip **404**.

Every clip includes `id`, `cameraId`, `source`, `state: complete`, `bytes`,
`startedAt`, `endedAt`, `completedAt`, `durationSeconds`, `durationAccuracy`,
`classification`, `detections`, `videoUrl`, `snapshotUrl` and
`snapshotCapturedAt`. UTC timestamps have one-second precision. Native hook
durations and AI wall-clock durations are estimates, not exact sample durations.
For imports, start and duration are null and end is the file's modification time.

The URLs are percent-encoded, origin-relative URLs served by Majestic's existing
file server. Use the same login/origin, including through the reverse proxy.
HTTP Range/seek is supported by that server; videos are not buffered by the CGI.
The storage must be reachable via the normal Recordings UI's file-serving path.

[recordings-client.py](recordings-client.py) is a standard-library-only example
which checkpoints its cursor and downloads each clip into an ID-named directory:

```sh
python3 recordings-client.py https://cam.example ./camera-recordings
```

It prompts for the root password (or reads `CAMERA_PASSWORD`). Run repeatedly from
your scheduler. Clip IDs provide deduplication when a page is replayed after a
client interruption. The client stops on 410; it never silently discards a cursor.

## AI Summaries and Stills

New AI clips aggregate accepted detections over the clip: category, type,
active model ID, maximum confidence, and first/last seen timestamps. Visual
detections already passed the configured motion-region policy. The selected
recording categories control triggering; other accepted categories seen during
an active clip can appear in its summary. Sound has no spatial region.

Stock visual AI reports **pet**, not separate cat/dog species. Bark and meow are
sound categories. The optional bird model reports bird. Do not infer a species
from a stock pet result.

One `/image.jpg` request is streamed directly to storage when an AI clip starts.
`snapshotCapturedAt` is the camera's request-time estimate, not a frame-aligned
timestamp from the MP4. The still may differ slightly from the first video frame.
If JPEG is disabled, unavailable, oversized or times out, recording still succeeds
and both still fields are null. This feature does not change JPEG/video settings.
Native and historical clips remain unclassified with no invented still or labels.

## Completion Webhooks

These are **separate from existing detection notifications**, which still report
arrivals immediately. To enable completion delivery, GET `/cgi-bin/c120-ai-api.cgi`,
retain its full `config`, set the following field, and POST that config plus the
returned `csrf` token back to the same endpoint:

```json
"recordingWebhook": {
  "enabled": true,
  "url": "https://automation.example/api/webhook/camera-recording",
  "token": ""
}
```

`token`, when nonempty, is sent as a Bearer credential. Older UI clients preserve
this setting when saving. Enter the destination on each camera; do not bake
private URLs or tokens into a shared firmware build.

```json
{
  "schemaVersion": 1,
  "type": "recording.ready",
  "cameraId": "camera-0123456789abcdef",
  "recordingId": "0123456789abcdef-00000000000000000001",
  "recording": { "id": "0123456789abcdef-00000000000000000001", "source": "ai" }
}
```

`recording` contains the full detail object described above, abbreviated here.
Return 2xx only after accepting the event. Requests time out after three seconds,
do not follow redirects, and use one short-lived worker at a time. Failed requests
retry with exponential backoff up to five minutes, for at most 24 hours after
completion. Delivery state is on recording storage, not RAM or root flash.
Attempts are **at least once**: deduplicate by `(cameraId, recordingId)`, since a
receiver may accept a request just before a timeout or power loss. A destination
or token change cancels old pending events rather than sending history elsewhere.
Expired/deleted clips are not retried. Polling is the authoritative catch-up path.
No webhook is sent for historical imports, avoiding an alert storm on installation.

## Native Hooks, Recovery and Retention

The installer sets `records.onClose` to `/usr/bin/c120-recording-closed` only when
empty or using OpenIPC's standard `/usr/sbin/record.sh`. It preserves and chains
the standard dispatcher. Uninstall restores the previous value only if the
plugin still owns the hook. A custom hook is never replaced: add
`/usr/bin/c120-ai recording-closed "$1" "$2" "$3"` to it for completion events.
Without a hook, reconciliation still imports closed native clips as unclassified
historical entries, without notifications. No recording is enabled automatically.

The native close hook performs local metadata work, not network delivery. A
background pass imports up to 64 previously unindexed clips per minute and repairs
interrupted metadata/path-reference updates. Open files, symlinks, incomplete/invalid MP4s
and `.partial` files are excluded. Up to eight directory levels are scanned.

The catalogue lives in `.c120-recordings` below the non-date prefix of
`records.path`. Use a dedicated directory such as `/mnt/mmcblk0/recordings/%F`.
Never stage firmware, models or backups inside the recordings directory.

Majestic already deletes its oldest clips using `records.maxUsage` and
`records.purgeSeconds`. With native recording off and AI recording on, the plugin
checks space every ten seconds and deletes up to 32 oldest closed catalogued MP4s
per pass. It aims for two percentage points below maxUsage and at least 32 MiB
free, starting cleanup at the limit or below 16 MiB free. The writer pauses below
8 MiB/at maxUsage and retries after cleanup. It never deletes active/open files,
partials, unrelated files, or files outside the recording root. If non-recording
data fills the card, recording safely waits rather than deleting that data.

Deleted clips' plugin stills, metadata and pending deliveries are cleaned up
without changing IDs or cursors. Native/manual
deletions also disappear from the API. Retention is not a backup: download clips
promptly if you need permanent storage. Remove orphaned `.partial` files manually
after recovering anything needed from an interrupted recording.

Storage safety and auth tests use synthetic files/streams. Full-card tests do not
fill or erase a real user's SD card. Streaming quality remains 2560x1440 at 30 fps
and the configured bitrate.
