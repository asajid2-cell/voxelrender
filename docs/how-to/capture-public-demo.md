# Capture A Public Demo

Use the public demo capture wrapper to produce review media from VENPOD's own
DX12 backbuffer readback path.

```powershell
cd VENPOD
.\public_demo_capture.ps1 -Config Release
```

By default, artifacts are written to `VENPOD/build/captures/public_demo/`:

- `sparse-public-demo.mp4`
- `contact_sheet.png`
- `image_stats.csv`
- `venpod_runtime.log`

The script first runs `engine_capture_smoke.ps1`, so the frames are validated for
nonblank terrain coverage and sparse runtime contracts before the MP4 is encoded.
The full sparse regression gate additionally parses the generated runtime log
and requires post-ready terrain ownership, positive mid/far ownership, positive
visible far-SVO pixels, surface fragments, and zero `miss` / `unsafeNearMiss`
pixels for the public demo source frames.
Invalid capture windows are rejected before the engine launches or output
folders are created.
The engine capture layer clears stale frame/stat/log artifacts in the dedicated
output folder before launch and restricts capture output to dedicated
subfolders under `VENPOD/build/captures/` or `VENPOD/build/logs/`.
Local sparse physics is enabled by default to match the normal public sandbox;
use `-DisablePhysics` only when isolating render output. Use `-SkipVideo` when
`ffmpeg` is not available and only the validated contact sheet/stats are needed.
When video is enabled, the script probes or decodes the generated MP4 before
reporting success.
The output directory is cleaned of prior capture artifacts before each run. The
wrapper rejects broad directories such as the VENPOD root, repository root, or
build root, as well as source/runtime trees such as `VENPOD/build/bin/`; use a
dedicated capture or log folder when passing `-OutputDir`. Frame and playback
parameters are validated before cleanup starts, and output-directory safety is
checked before the folder is created.

Useful variants:

```powershell
.\public_demo_capture.ps1 -Config Release -StressCamera
.\public_demo_capture.ps1 -Config Release -BoundaryTest
.\public_demo_capture.ps1 -Config Release -DisablePhysics
.\public_demo_capture.ps1 -Config Release -CaptureFrames 150 -PlaybackFps 30
.\public_demo_capture.ps1 -Config Release -ReviewReel -CaptureFrames 24 -PlaybackFps 12
```

`-ReviewReel` creates a public-review reel from three validated segments:
normal public view, high-flight, and waterline/submerged traversal. It writes
per-segment contact sheets/logs, `review_reel_manifest.csv`,
`PUBLIC_DEMO_REVIEW_REEL.md`, and `sparse-public-review-reel.mp4` unless
`-SkipVideo` is used. Passing generation means the capture smoke gates passed;
it is still review evidence, not final visual acceptance.

For public-readiness visual review across the scenarios called out in the
completion ledger, use the visual review wrapper:

```powershell
.\visual_review_capture.ps1 -Config Release
```

It writes validated contact sheets, stats, runtime logs,
`VISUAL_REVIEW_CHECKLIST.md`, and `VISUAL_REVIEW_SUMMARY.csv` under
`VENPOD/build/logs/visual_review_capture/`. The scenarios are normal view,
scripted walk, long scripted walk, fast flight, long fast flight,
fast water-transition, long fast-water transition, waterline/submerged
movement, and long waterline/submerged traversal. Passing this
wrapper means the artifacts were generated and basic capture gates passed; it
does not by itself mean the visuals are accepted for release. The checklist must
be reviewed, while the CSV records per-scenario gate thresholds, top-band stats,
ownership maxima, and miss/unsafe-near-miss counts for audit. Any remaining
proxy, water, or LOD artifacts must stay tracked in
`docs/COMPLETION_LEDGER.md`.

The generated MP4 is intentionally not tracked in git. Regenerate it for release
notes, repository previews, or review attachments.
