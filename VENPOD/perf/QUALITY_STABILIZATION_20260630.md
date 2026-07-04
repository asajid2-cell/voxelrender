# VENPOD Quality Stabilization Report - 2026-06-30

## Objective

Stabilize default `.\rebrun.ps1 -NoBuild` quality mode toward stable 100+ FPS:

- after warmup: p50 <= 10 ms, p95 <= 12 ms, p99 <= 16.7 ms
- no recurring >33 ms frames
- no visual correctness regression: `visibleMissing=0`, `residentMissingSurface=0`, `unsafe/miss=0`
- scenarios: idle, walk, yaw, edit/brush

The exact target is not met at the current 1080p full-resolution quality configuration. The best safe improvement landed in this pass is the height-pump admission cap, which removes the main CPU streaming cascade without changing quality. The p50/median side of the target is hard-blocked by the full-resolution far raymarch floor. The p95/p99 and no-hitch side still has additional present-pacing and CPU-tail work after the GPU floor is lowered, so it should not be described as purely raymarch-blocked.

## Fresh Baseline

Capture folder:

`build/captures/stabilize_20260630_0712_quality`

Command shape:

```powershell
.\scripts\run_interactive_capture_task.ps1 -Scenario <idle|walk|yaw|edit> -Frames 900 -OutputDir .\build\captures\stabilize_20260630_0712_quality -Label <scenario>_quality -TimeoutSeconds 1200
.\scripts\analyze_stabilize_quality.ps1 -InputDir .\build\captures\stabilize_20260630_0712_quality -WarmupFrame 300
```

Warmup frame: 300.

| Scenario | p50 | p95 | p99 | max | >33 | GPU p50 | Ray p50 | Correctness |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| idle | 23.24 | 33.00 | 48.65 | 57.25 | 30 | 23.28 | 22.48 | unsafe/resident/visible = 0 |
| walk | 31.54 | 125.05 | 327.08 | 1029.10 | 283 | 8.33 | 4.09 | unsafe/resident/visible = 0 |
| yaw | 20.12 | 56.55 | 119.61 | 362.37 | 85 | 5.41 | 0.36 | unsafe/resident/visible = 0 |
| edit | 24.06 | 61.66 | 147.92 | 259.04 | 151 | 4.97 | 3.17 | unsafe/resident/visible = 0; transient miss only |

Important frame-level findings:

- idle was already GPU-bound by full-resolution raymarch: raw p50 ~= GPU p50 ~= ray p50.
- walk had a severe height-pump outlier: frame 488 `pumpHeight=187.94`, `genHeight=10`, `budgetMid=24`, `rawMs=339.65`.
- walk/yaw/edit tails also had recenter, interest, and surface-extraction work, but the height pump seeded a larger cascade.

## Implemented Improvement

File:

`src/Simulation/SparseClipmap.cpp`

Change:

- Reuse the existing `voxelPumpHardBudgetMs` value as a hard height-pump admission budget.
- Compute `effectiveMaxHeightTiles` from the previous frame's height-pump ms/tile.
- Apply the cap before `AllocateSlot()` in both parallel and serial height-pump paths.
- Do not break a parallel batch after admission, because `AllocateSlot()` can erase an evicted coord before the new tile is generated and committed.
- Add only a serial post-commit time check, where the just-generated tile is already published and the remainder is still queued.

This preserves spill/correctness: unadmitted coords remain in `m_generationQueue` and are not dropped.

Claude independently reviewed this patch shape and converged on the safety rule: bound admission, not mid-generation.

## Measurement Support Changes

Files:

- `src/Core/Window.cpp`
- `src/Core/Window.h`
- `scripts/run_interactive_capture_task.ps1`
- `scripts/stabilize_quality_capture.ps1`
- `scripts/analyze_stabilize_quality.ps1`

The window change is not a performance lever. It was added while diagnosing why non-interactive
agent runs could not create a DXGI swapchain: failed swapchain creation now reports the HRESULT,
flags, and size, and the engine retries without tearing only if the tearing-enabled creation fails.
The validated quality captures still created the swapchain with `tearing=enabled` and emitted no
fallback warning, so this did not change the measured default-quality present path.

The capture scripts provide repeatable interactive scheduled-task runs, explicit exit-status
handling, default low-overhead summary logging, optional render-scale proof runs, and an analyzer
that maps `PERF_FRAME_END`, `PERF_GPU`, `PERF_RENDER_COMPOSITION`, ownership/readiness,
`MIDMESH_SELFTIME`, `PERF_SPARSE_CLIPMAP`, and `PERF_UNTRACKED` streams into `summary.csv` and
`frame_map.csv`.

## Post-Patch Validation

Primary post-patch performance capture folder:

`build/captures/stabilize_20260630_1120_height_budget_perf`

Warmup frame: 300. Capture settings: low-overhead summary interval 30, per-frame `PERF_FRAME_END`.

| Scenario | p50 | p95 | p99 | max | >33 | GPU p50 | Ray p50 | Correctness |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| idle | 23.62 | 36.28 | 52.26 | 73.08 | 48 | 23.60 | 22.78 | unsafe=0, residentMissingSurface=0 |
| walk | 12.87 | 27.58 | 37.30 | 102.64 | 13 | 4.50 | 3.37 | unsafe=0, residentMissingSurface=0, visibleMissing=0 |
| yaw | 21.47 | 27.76 | 30.48 | 33.26 | 2 | 21.48 | 19.91 | unsafe=0, residentMissingSurface=0 |
| edit | 18.65 | 36.49 | 51.16 | 65.39 | 49 | 4.20 | 2.62 | unsafe=0, residentMissingSurface=0, visibleMissing=0; transient miss only |

Measured improvement:

- walk p50: 31.54 -> 12.87 ms
- walk p95: 125.05 -> 27.58 ms
- walk p99: 327.08 -> 37.30 ms
- walk max: 1029.10 -> 102.64 ms
- walk frames >33 ms: 283 -> 13
- edit p95: 61.66 -> 36.49 ms
- edit p99: 147.92 -> 51.16 ms
- yaw p95: 56.55 -> 27.76 ms
- yaw p99: 119.61 -> 30.48 ms

The patch is a real cascade break, not just a single-frame fix. It keeps the visible-safety gates clean: `unsafeNearMiss=0`, `residentMissingSurface=0`, and emitted `visibleMissing=0`.

Edit caveat: ownership `miss` is still nonzero during edit/brush, as it was in the baseline. Warmed max miss changed from 488 to 803 in the post-patch capture, concentrated at ownership frames 300 and 330; frames 360, 390, 420, 450, and 480 match the baseline exactly. `unsafeNearMiss`, `residentMissingSurface`, and `visibleMissing` remain zero. This is not a visible-hole regression in the current logs, but strict `miss=0` is not achieved and should remain a correctness gate for the next stage.

## Remaining Blocker

The exact p50/median target is physically blocked at the current default quality configuration:

- `renderScale=1.000`
- output `1920x1080`
- background pass inactive
- full-resolution near and far raymarch

The median blocker is the far raymarch floor in normal horizon-facing views:

- idle: raw p50 23.62 ~= GPU p50 23.60 ~= ray p50 22.78
- yaw: raw p50 21.47 ~= GPU p50 21.48 ~= ray p50 19.91
- composition is not an artificial sky-only pose: the raymarched far/background portion is a normal visible fraction, not the whole screen.

No CPU streaming patch can make idle/yaw reach p50 <= 10 ms while the GPU raymarch alone costs about 20-23 ms.

This blocker should not be over-scoped. The p95/p99 and recurring-hitch side is not proven to be
purely raymarch-bound. In the `RenderScale 0.4` proof, idle reaches `gpuP95=10.10` and `rayP95=9.29`
but still has `rawP95=15.17` with `gapPrevP95=6.53`. Walk/edit tails also still show present/surface
work after the height cascade is bounded. Those tails are real next-stage work, but they are
secondary to the 1080p raymarch wall because the default full-resolution median already cannot
reach 100+ FPS.

## Render-Scale Proofs

These are idle-only proofs, not default-quality patches. `rebrun.ps1` documents lower render scale/background pass as softer, and quality mode intentionally disables that path.

| Proof | Output | p50 | p95 | p99 | GPU p50 | Ray p50 | >33 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| quality default | 1920x1080 | 23.62 | 36.28 | 52.26 | 23.60 | 22.78 | 48 |
| quality `-RenderScale 0.5` | 960x540 | 11.00 | 16.58 | 17.09 | 11.07 | 10.04 | 0 |
| quality `-RenderScale 0.4` | 768x432 | 9.64 | 15.17 | 16.44 | 9.67 | 8.90 | 0 |

The proofs confirm the idle median raymarch cost is resolution-bound. They also show a config flip is insufficient and visually regressive: even at `RenderScale 0.4`, idle p95 is still 15.17 ms because a `gapPrev`/present-pacing tail becomes visible after the GPU floor is lowered.

## Validation Limits

- The post-patch captures are single-run scenario captures, not a three-run A/B suite. The large
  walk cascade improvement is much larger than observed control variance, but p95/p99/tail counts
  should still be treated as one-run measurements.
- Idle is a useful control because the height-pump patch cannot materially affect it. Its tail
  variance worsened from baseline to post-patch (`p99 48.65 -> 52.26`, `>33 30 -> 48`), which is
  evidence that tail counts need multi-run confirmation before fine-grained claims.
- `visibleMissing=0` is directly emitted in the post-patch walk/edit captures. Idle/yaw emitted
  `unsafeNearMiss=0` and `residentMissingSurface=0`, but `visibleMissing` was not present in their
  low-overhead summary rows, so the idle/yaw visible-hole claim is inferred from the stronger
  resident/unsafe gates and normal far fallback behavior rather than directly logged.
- The edit ownership `miss` increase near the warmup boundary is unresolved. It did not produce
  `unsafeNearMiss`, `residentMissingSurface`, or `visibleMissing` failures in the current logs, but
  strict edit `miss=0` remains an open correctness/perf gate.
- The first-frame stale-predictor case can still overshoot once if the parallel height path admits a
  burst before a prior ms/tile estimate exists. The patch prevents the repeated cascade observed in
  the baseline rather than proving every possible future burst is impossible.

## Rejected Next Patches

- Do not ship `RenderScale < 1.0` or background pass in quality mode as a silent fix. That violates the current quality contract.
- Do not lower height/streaming budgets further. The measured height cascade is already bounded; further budget cuts would trade coverage for numbers.
- Do not chase `gapPrev` or edit `postWait` before the raymarch wall is accepted. Those tails matter, but they cannot make the default full-resolution idle/yaw medians hit p50 <= 10 ms while far raymarch costs 20+ ms.
- Do not move warmup/capture windows to make percentiles pass.

## Next Architecture Plan

The no-quality-loss path to the target is not another small CPU patch. It is a far-field rendering architecture change that reduces full-resolution raymarch cost without simply lowering final image resolution.

Recommended next stage:

1. Add a temporal far-field reprojection/amortization path:
   - render far/background raymarch over multiple frames
   - reproject with camera motion
   - refresh disocclusions and high-error regions first
   - keep near/surface raster full resolution
2. Add confidence/error masks:
   - reject reprojection for large camera deltas, water transitions, and uncovered silhouettes
   - fall back to full-resolution raymarch only where needed
3. Measure against the same gates:
   - idle, walk, yaw, edit/brush
   - p50/p95/p99 and >33
   - `visibleMissing=0`, `residentMissingSurface=0`, `unsafe/miss=0`
   - visual review for ghosting and horizon artifacts
4. After the GPU wall is below about 9 ms, return to secondary CPU tail work:
   - edit brush `postWait`/surface extraction
   - present pacing or `gapPrev` if still reproducible in low-overhead captures
   - residual surface apply/upload tails

## Current Artifacts

- Baseline captures: `build/captures/stabilize_20260630_0712_quality`
- Post-patch perf captures: `build/captures/stabilize_20260630_1120_height_budget_perf`
- Dense diagnostic captures: `build/captures/stabilize_20260630_1110_height_budget_all`
- Render-scale proof 0.5: `build/captures/stabilize_20260630_1935_render_scale_proof`
- Render-scale proof 0.4: `build/captures/stabilize_20260630_1940_render_scale_04_proof`
- Analyzer: `scripts/analyze_stabilize_quality.ps1`
- Interactive capture wrapper: `scripts/run_interactive_capture_task.ps1`
- Capture runner: `scripts/stabilize_quality_capture.ps1`
- Tandem ledger: `z:/328/CMPUT328-A2/codexworks/301/tandem/tandems/venpod-stabilize-quality/TANDEM.md`
