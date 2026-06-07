# VENPOD Rendering Debug Handoff

Canonical current handoff: `debug-handoff.md`. Update that file first for session memory; this file is retained as historical long-form debug context.

Generated: 2026-06-02  
Source chat: `019e2878-79a1-7092-8931-4c3a2ff5f019`  
Rollout file: `C:\Users\Ahmed\.codex\sessions\2026\05\14\rollout-2026-05-14T15-50-44-019e2878-79a1-7092-8931-4c3a2ff5f019.jsonl`  
Project root: `Z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD`

This document is meant to pull the technical working history out of the chat so it can be shared with researchers or other engineers. It summarizes the project, the active task, the experiments and tool calls that mattered, what was fixed, what was disproven, and what is still blocking the renderer.

## Clean-Harness / Parallel Mid Pump Validation - 2026-06-05

The perf harness was accidentally forcing `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`, which made low-alt walk rows run projected missing-brick work and priority tagging even when the prepump branch was inactive. This polluted the accepted-stack measurements.

Retained cleanup:

- `perf_noncapture_smoke.ps1` now exposes `-MidClipmapVisibleCriticalPrepump` and clears `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP` by default.
- `src/main_launcher.cpp` runs projected missing-brick scan/priority tagging only when:
  - high-alt prepump can affect the budget,
  - visible priority/lane guard is enabled,
  - or missing-sample feedback is explicitly enabled.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Clean control and candidate rows:

| Stack | Scenario | Raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| clean control | walk realtime f600 | `63.51` | `55.81` | `12.94` | `16.70` | `26.15` | `14.31` | `11.01` | `12.22` | true clean blocker |
| parallel mid pump | fixed f384 | `23.80` | `22.31` | `4.50` | `17.00` | `0.80` | `0.00` | `20.87` | `6.04` | partial |
| parallel mid pump | walk realtime f600 | `59.51` | `35.60` | `15.40` | `14.67` | `5.51` | `4.01` | `11.32` | `12.95` | strongest current clean walk row |
| parallel mid pump | high-alt f398 | `51.66` | `26.21` | `4.94` | `5.18` | `16.09` | `9.74` | `3.78` | `10.25` | high-alt still blocked |

Rejected in the clean cycle:

- `MidInterestInterval=2`: worsened walk to raw/CPU `91.39/125.58`.
- terrain-critical parallel generation on top of parallel mid: activated but worsened walk to `63.32/48.43`.
- worker-local mid column cache on top of parallel mid: slightly lower CPU, worse raw/window than plain parallel mid.
- high-alt projected visible-critical prepump/cache defer: activated and proved broad cache debt, but high-alt window regressed.

Visual:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605/contact_sheet.png`
- Walk capture smoke passed for frames `560/580/600`; no obvious white holes/sky leaks, but terrain remains coarse/blocky.

Decision:

- Parallel mid pump is a valid default-off partial, not a playable stack.
- The renderer remains far from 60 FPS: fixed about `24 ms`, walk about `60 ms`, high-alt about `52 ms`.
- The next implementation must stop isolating single queues and build ownership-lane state across request, exact generation, upload/apply, surface extraction, and publish.

## Goal Charter / Window Validation Anchor - 2026-06-05

Active goal remains `streaming_playability_real_fix_campaign_20260604`; do not replace it with a narrow diagnostic goal.

New default-neutral validation infrastructure:

- `VENPOD_PERF_FRAME_END_LOG_INTERVAL`, unset by default.
- `perf_noncapture_smoke.ps1 -FrameEndLogInterval`.
- `perf_noncapture_smoke.ps1` now writes `window_summary.csv` and `window_table.md`.

Accepted-stack validation artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_framewindow_20260605`

Target rows:

- fixed f384: raw `19.58`, CPU `17.89`, request/gen/clip `3.82/13.13/0.93`, surface extract `20.60`, GPU `5.67`
- walk realtime f600: raw `58.94`, CPU `48.30`, request/gen/clip `12.55/12.87/22.86`, surface extract `9.78`, GPU `17.85`
- high-alt f360: raw `48.21`, CPU `30.41`, request/gen/clip `8.96/4.78/16.66`, surface extract `8.13`, GPU `5.99`

Decision: next implementation branch must use this accepted-stack table, not `frameend_interval_validation_20260605`, which used the mixed persistent-mid/interest-2 probe stack.

## Campaign Resume - Visible Cache And Inline Surface Rejections - 2026-06-05

Latest durable handoff:

- `handoff.md`, section `Campaign Resume - Visible Cache And Inline Surface Rejections - 2026-06-05`

Build/test status before and after the removed inline-surface prototype:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Fresh baseline artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/inline_generated_surface_baseline_vsync0_20260605`

Fresh baseline:

- fixed frame `384`: raw `21.97`, CPU `21.93`, request/gen/clip `4.28/16.56/1.08`, surface extract/stage `20.10/2.24`, GPU `6.78`
- walk realtime frame `600`: raw `88.21`, CPU `49.84`, request/gen/clip `12.79/13.04/23.99`, pump `21.59`, surface extract/stage `9.42/2.82`, GPU `17.98`, missing/sample `563/378`
- high-alt frame `360`: raw `52.57`, CPU `32.26`, request/gen/clip `7.57/6.91/17.77`, pump `5.63`, surface extract/stage `4.05/2.21`, GPU `10.84`, missing/sample `2231/0`

Rejected/removed:

- Inline generated surface extraction. It reduced queued surface extraction but moved work into generation/clip/CPU: walk realtime became `96.91/112.71 raw/CPU`. The temporary flag and code were removed.

Rejected/default-off diagnostics:

- Full visible-lane/cache-only deferral. It deferred `3586` bricks indefinitely and left broad mid coverage at `61`, so it is a fake win.
- High-alt-only projected cache deferral. It reduced high-alt pump from `5.63` to `1.82 ms`, but raw high-alt stayed about `52 ms` and walk regressed.
- Current-stack parallel mid pump. It activated in walk, but CPU regressed to `60.51 ms`.

Decision:

- Current representative walk debt is not scroll churn: frame `600` had reused interest and old sampled-visible debt.
- Walk/realtime missing debt remains fallback-invalid/unknown and cannot be deferred safely.
- High-alt unsampled cache debt is real, but the safe deferral slice is not enough by itself.
- The next meaningful slice is ownership-aware exact/surface request-generation-publish lanes with separate budgets and apply/upload accounting.

## Campaign Continuation - Surface Lane Budget Rejected - 2026-06-05

Latest durable handoff:

- `handoff.md`, section `Campaign Continuation - Surface Lane Budget Rejected - 2026-06-05`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_lane_budget_rejected_20260605.md`

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Two surface-budget branches were attempted and removed:

- hidden post-open pre-publish surface cap:
  - fixed `22.37/21.65 raw/CPU`, walk `74.88/52.92`, high-alt `49.68/34.30`
  - rejected because general surface extraction refilled the frame and walk regressed
- combined hidden post-open cap plus low-lane surface extraction budget:
  - fixed `23.86/18.56 raw/CPU`, walk `56.01/69.32`, high-alt `183.41/36.64`
  - rejected because it shifted work into catch-up/gap debt and regressed representative rows

Decision:

- Do not retry hidden-only pre-publish caps, prefetch surface caps, or low-lane surface caps.
- The remaining blocker is not surface-budget tuning; it requires reducing real visible/sampled work or adding ownership-proved async/apply architecture without treating unknown fallback as safe.

## Campaign Continuation - Request Slice Rejected / Critical Generation Batch - 2026-06-04

Latest durable handoff:

- `handoff.md`, section `Campaign Continuation - Request Slice Rejected / Critical Generation Batch - 2026-06-04`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Results:

- default-off resident-touch cache was implemented and measured, then removed:
  - no-heavy accepted stack walk raw/CPU/request `65.89/45.21/15.44`
  - resident-touch cache walk raw/CPU/request `70.79/44.71/16.16`
  - decision: rejected, not a request fix
- retained default-off same-frame terrain-critical parallel generation:
  - `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
  - `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`
  - walk raw improved to `51.34 ms`, with CPU still `46.57 ms`
  - no ownership deferral; requested critical bricks still complete same-frame
- combined V2 guard + parallel generation was rejected as a global stack because walk raw worsened to `71.84 ms`.

Current blocker:

- no 60 FPS candidate exists.
- remaining walk cost is distributed across request, generation, clip/pump, surface extraction, and GPU.
- the next real fix needs ownership-aware lanes through exact generation, surface extraction, upload/apply, and publication, not another resident-touch cache.

## Campaign Continuation - Visible-Critical V2 / Interval-2 Cycle - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed, `ninja: no work to do`.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- `git diff --check` on touched code/harness found no whitespace errors, only existing LF-to-CRLF warnings.

Artifacts:

- `build/captures/visible_coverage_guard_v2_walk_candidate_20260604/`
- `build/captures/visible_coverage_guard_v2_highalt_candidate_20260604/`
- `build/captures/visible_coverage_guard_v2_fixed_candidate_20260604/`
- `build/captures/mid_interest_interval2_walk_candidate_20260604/`
- `build/captures/mid_interval2_pressure_guard_walk_candidate_20260604/`
- `build/captures/mid_interval2_parallel_surface_walk_candidate_20260604/`
- `build/captures/mid_interval2_parallel_exact_walk_candidate_20260604/`
- `build/captures/mid_interval2_direct_footprint_walk_candidate_20260604/`
- `build/captures/mid_interval2_async_exact_walk_candidate_20260604/`
- `build/captures/accepted_stack_interval2_v2_all_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_v2_interval2_cycle_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_v2_interval2_cycle_table.md`

New default-off infrastructure:

- `SparseStreamingLane { Cache, Prefetch, Visible, PublicCritical }`
- resident lane touch/promotion helpers and generation/upload/surface queue lane counts
- `VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS=0`
- `PERF_SPARSE_STREAMING_LANES`
- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2=0`

Results:

- V2 high-alt row was the meaningful partial win:
  - frame `385`, raw `48.84`, CPU `26.00`, clip/pump `15.81/6.06`
  - missing `1865`, sampled/unsampled `1/1864`
  - cache-only defer `1864`, visible-critical coverage `99`
  - `miss/unsafe=0/0`
- V2 walk row correctly refused to defer sampled-visible debt:
  - frame `600`, raw `74.15`, CPU `34.44`, clip/pump `9.65/7.17`
  - missing `494`, sampled/unsampled `298/196`, defer `0`
  - `miss/unsafe=0/0`
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2` gave a partial walk clip-interest win:
  - frame `600`, raw `67.99`, CPU `40.70`, request/gen/clip/pump `15.63/16.71/8.34/6.13`
  - missing `429`, sampled/unsampled `239/190`, coverage `95`
  - `miss/unsafe=0/0`

Rejected:

- pressure-trim free-page guard: raw `136.15`, CPU `56.97`, surface/GPU/raw spike
- parallel surface extraction: raw `106.14`, CPU `59.94`, regressed surface/clip/GPU
- parallel exact generation: raw `73.96`, CPU `44.57`, missing sampled `410`, frame regression
- direct footprint columns: raw `101.25`, CPU `67.27`, request/gen/GPU regression
- async exact generation: raw `119.41`, CPU `54.01`, async enqueued/applied `0/0`

Accepted-stack retest with V2 and interval-2 did not produce a playable candidate:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | GPU | Missing sampled/unsampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `379` | `30.06` | `18.51` | `11.86` | `5.77` | `0.89` | `0.00` | `9.08` | `0/0` | `0/0` |
| walk realtime | `600` | `114.88` | `49.00` | `12.05` | `15.62` | `21.30` | `18.79` | `23.11` | `295/188` | `0/0` |
| high-alt | `400` | `112.25` | `53.42` | `19.69` | `19.11` | `14.61` | `7.49` | `23.11` | `4/1873` | `0/0` |

Decision:

- No validated 60 FPS candidate exists.
- V2 and interval-2 are safe default-off partials, not a full stack.
- Representative walk remains sampled-visible; deferring it would require treating unknown/invalid fallback as safe, which is not allowed.
- Remaining work is the ownership-aware streaming/publication state split across request, generation, surface extraction, upload/apply, and publication. Local caps/threading variants tested here just moved cost or regressed.

## Campaign Continuation - Exact ViewCone and Cache Defer Closure - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after removing the exact ViewCone no-op prototype.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/exact_viewcone_prefetch_async_candidate_20260604/`
- `build/captures/exact_viewcone_prefetch_async_highalt_candidate_20260604/`
- `build/captures/accepted_stack_vsync_off_probe_20260604/`
- `build/captures/mid_cache_only_defer_highalt_candidate_20260604/`
- `build/captures/mid_cache_only_defer_walk_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Exact ViewCone speculative demotion / exact async:

- Measured as a no-op and removed from code/harness.
- walk realtime frame `600`: raw `71.90`, CPU `43.56`, request/gen/clip/pump `14.86/15.52/13.17/10.94`, GPU `14.43`, missing `438`, sampled `240`, coverage `94`, miss/unsafe `0/0`.
- `PERF_SPARSE_EXACT_PUBLIC_LANES` showed `viewDemoted=0`, `viewPrefetchDemoted=0`.
- `PERF_SPARSE_EXACT_ASYNC` showed enqueued/completed/applied all `0`.
- high-alt frame `393`: raw `48.94`, CPU `25.67`, clip/pump `14.83/4.29`, missing `1486`, sampled `0`, exact async enqueued/applied `0/0`, miss/unsafe `0/0`.

VSync-off probe:

- walk realtime frame `600`: raw `74.62`, body `69.82`, CPU `45.68`, GPU `6.04`, `vsync=0`, miss/unsafe `0/0`.
- This ruled out a simple VSync/present-only explanation for the current noncapture frame time.

Mid-clipmap cache-only defer:

- Measured and rejected as a candidate.
- high-alt frame `404`: raw `62.14`, CPU `33.82`, clip/pump `22.22/9.37`, missing `1501`, sampled `1101`, unsampled `400`, visible-critical coverage `87`, budget reason `2`, defer count `0`, miss/unsafe `0/0`.
- walk realtime frame `600`: raw `66.53`, CPU `51.54`, request/gen/clip/pump `11.91/13.41/26.20/13.26`, missing `488`, sampled `289`, unsampled `199`, budget reason `2`, defer count `0`, miss/unsafe `0/0`.
- The branch did not activate under safety guards and regressed representative movement.

Current decision:

- No validated 60 FPS candidate exists.
- The strongest default-off stack remains the worker-local mid column cache stack: fixed raw about `23.77`, walk realtime raw about `49.61`, high-alt raw about `50.85`.
- The next implementation is the ownership-aware streaming/publication state split, not another exact request demotion, cache-only defer, VSync tweak, queue cap, or ring heuristic.
- Required shape: separate public-critical/sampled-visible work from cache/prefetch/maintenance before request, generation, surface extraction, upload/apply, and page publication. Unknown sampled fallback remains critical. Cache/prefetch work can be budgeted or async-generated only after classification removes it from the public owner path.

## Campaign Continuation - Public-Critical Surface Gate Split Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the rejected probe was removed.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- Known `rayDir` shadow warnings remain.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_gate_public_critical_only_candidate_20260604/`

Temporary probe:

- `VENPOD_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL_ONLY`
- harness switch `-SurfaceGatePublicCriticalOnly`
- `PERF_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL`

The probe tried to keep terrain-critical, hidden-exact tracked/critical, collision, and edited publishes under the exact-surface-ready page-table gate, while allowing non-public-critical visible publishes through before exact surface extraction was known. Surface extraction remained queued; this was not a deletion of surface work.

Results:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe | Gate result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `22.30` | `21.79` | `4.35` | `16.09` | `1.35` | `0.00` | `18.90/1.90` | `6.25` | `0` | `100` | `0/0` | `latePublishes=0` |
| walk realtime | `600` | `92.00` | `60.95` | `10.41` | `11.81` | `38.70` | `36.83` | `8.86/2.64` | `17.36` | `421` | `94` | `0/0` | `latePublishes=0`, `criticalDefers=6` |
| high-alt | `360` | `49.84` | `30.70` | `7.16` | `5.80` | `17.74` | `5.81` | `3.59/1.81` | `10.87` | `0` | `99` | `0/0` | `latePublishes=0` |

Decision:

- Rejected and removed.
- At the target frames there was no noncritical late-publish opportunity, so this was not the missing public-critical/cache split.
- Walk regressed into clip/pump catch-up and became much worse than the accepted stack.
- Do not retry a page-table surface gate bypass. The real slice must add explicit streaming lanes earlier than publication: public-critical, sampled-visible, cache/prefetch, and maintenance.

## Campaign Continuation - Surface-Ready Publish Pressure Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after adding default-off surface-ready publish pressure accounting.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_queue_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_pressure_candidate_20260604/`

Patch addition:

- `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=0`
- harness switch `-SurfaceReadyPublishPressure`
- `PERF_SPARSE_SURFACE_READY_PUBLISH pressure=0/1`
- When enabled with `VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE=1`, surface-ready pending publishes are counted as protected visible publish pressure for sparse runtime budget evaluation. Defaults remain unchanged.

Results:

| Branch | Walk frame 600 | Decision |
|---|---|---|
| strongest accepted stack | `raw=49.61`, `CPU=24.04`, `request/gen/clip=8.83/7.82/7.38`, `pump=5.82`, `surface=8.57/2.92`, `GPU=12.72`, `miss/unsafe=0/0` | baseline |
| surface-ready queue only | `raw=90.29`, `CPU=61.98`, `request/gen/clip=10.45/13.83/37.69`, `pump=35.59`, `surface=8.52/2.93`, `GPU=16.19`, `miss/unsafe=0/0` | rejected; side queue hid pressure and caused clip catch-up |
| surface-ready queue + pressure | `raw=62.23`, `CPU=43.31`, `request/gen/clip=12.61/14.94/15.75`, `pump=4.02`, `surface=8.59/2.77`, `GPU=17.22`, `miss/unsafe=0/0` | rejected; still worse than accepted stack |

Interpretation:

- Surface-ready publish pressure is safe to keep default-off as a diagnostic/prototype, but is not part of the playable candidate stack.
- The queue-only probe showed the scheduler cannot ignore side-queue surface-gated visible publishes.
- The pressure probe showed that simply feeding that queue back into protected pressure still regresses the accepted stack.
- The next real code slice must split public-critical publication from cache/prefetch publication, then budget generation, surface extraction, upload/apply, and page-table publish separately.

## Campaign Continuation - Time-Budgeted Surface Parallel Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the default-off time-budgeted surface parallel batch cap.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- `git diff --check` on touched code reported no code whitespace errors, only existing LF/CRLF warnings.
- Known `rayDir` shadow warnings remain.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_surface_time_budgeted_batch32_candidate_20260604/`

New default-off prototype knobs:

- `VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH=32`

Result:

| Branch | Walk realtime frame 600 | Decision |
|---|---|---|
| strongest accepted stack | `raw=49.61`, `CPU=24.04`, `request/gen/clip=8.83/7.82/7.38`, `surface=8.57/2.92`, `GPU=12.72`, `miss/unsafe=0/0` | baseline to beat |
| time-budgeted parallel surface batch `32` | `raw=76.24`, `CPU=36.50`, `request/gen/clip=13.67/16.20/6.62`, `surface=11.75/2.97`, `GPU=14.38`, `miss/unsafe=0/0` | rejected |

Interpretation:

- The accepted walk row had `qsurf=0/3265/0/0`, so the surface queue was visible-class rather than speculative/cache.
- Async surface extraction for speculative/cache work would be a no-op in that row until VENPOD has a real public-critical/cache publication split.
- Time-budgeted parallel surface extraction preserved `miss/unsafe=0/0`, but it regressed representative walk and shifted debt into request/generation/publication.
- This closes the local surface-parallel branch as a playable path.

## Campaign Continuation - Request/Surface Micro-Branches - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the surface parallel extraction candidate.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- `git diff --check` on touched code reported no code whitespace errors, only existing LF/CRLF warnings.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_fast_resident_touch_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_extraction_max1_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_surface_extraction_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_surface_extraction_cached_candidate_20260604/`

New default-off/instrumentation knobs:

- `VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS=4`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS=4`
- `PERF_SPARSE_CPU_DETAIL` reports `surfaceParallel=active/bricks/workers/wallMs`
- `perf_noncapture_smoke.ps1` exposes `-RequestFastResidentTouch`, `-SurfaceExtractionMaxMs`, `-ParallelSurfaceExtraction`, and `-ParallelSurfaceExtractionMaxWorkers`

Results:

| Branch | Walk realtime frame 600 | Decision |
|---|---|---|
| strongest accepted stack | `raw=49.61`, `CPU=24.04`, `request/gen/clip=8.83/7.82/7.38`, `surface=8.57/2.92`, `GPU=12.72`, `miss/unsafe=0/0` | baseline to beat |
| fast resident touch | `raw=59.01`, `CPU=38.22`, `request/gen/clip=14.70/15.75/7.70`, `surface=12.25/3.06`, `GPU=5.67`, `miss/unsafe=0/0` | rejected; safe but slower |
| global surface extraction max `1` | `raw=63.83`, `CPU=34.30`, `request/gen/clip=13.22/15.13/5.93`, `surface=9.89/2.46`, `GPU=11.95`, `miss/unsafe=0/0` | rejected globally; high-alt direction only, walk worsened |
| parallel surface extraction, uncached | `raw=64.81`, `CPU=36.94`, `request/gen/clip=14.68/16.41/5.84`, `surface=16.11/2.91`, `GPU=12.70`, `miss/unsafe=0/0` | rejected; worker helper recomputed columns |
| parallel surface extraction, worker cache | `raw=72.39`, `CPU=41.06`, `request/gen/clip=14.28/19.01/7.75`, `surface=12.78/2.89`, `GPU=9.87`, `miss/unsafe=0/0` | rejected; worker batch overran the time-budgeted surface lane and shifted debt |

Interpretation:

- No validated 60 FPS/noncapture playable candidate exists.
- The strongest validated default-off stack remains the worker-local mid column cache stack.
- Fast resident touch, global surface cap, and parallel surface extraction should not be included in the candidate stack.
- The next real implementation direction is an ownership-aware streaming/publication state split:
  - visible-critical pages stay synchronous/guarded until exact surface/page-table publication is safe;
  - cache/prefetch pages can generate/extract surface asynchronously because they are not required public owners this frame;
  - surface extraction results apply on the main thread at a frame boundary with upload/apply budgets;
  - request prep, generation, surface extraction, page publish, and GPU upload need separate lane/backlog age accounting.

## Campaign Continuation - Worker-Local Mid Column Cache - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the worker-local mid column cache and surface-general-budget instrumentation.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_worker_column_cache_all_20260604/`

Patch additions:

- Added default-off worker-local mid-clipmap column cache:
  - `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE=0`
  - harness switch `-ParallelMidWorkerColumnCache`
  - `PERF_SPARSE_MID_CLIPMAP_BACKLOG` now reports `workerColumnCache=active/entries/hHit/hMiss/rHit/rMiss`
- Added default-off surface general strict-budget instrumentation:
  - `VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET=0`
  - `VENPOD_SPARSE_SURFACE_GENERAL_MIN_BUDGET_MS=4`
  - `PERF_SPARSE_SURFACE_GENERAL_BUDGET`

Results:

| Candidate | Walk frame 600 | Decision |
|---|---|---|
| pre-continuation strongest | `raw=46.49`, `cpu=31.05`, `request/gen/clip=8.15/7.00/15.87`, `surface=9.00/3.31`, `gpu=14.60`, `miss/unsafe=0/0` | best older stack |
| worker-local mid column cache | `raw=49.61`, `cpu=24.04`, `request/gen/clip=8.83/7.82/7.38`, `surface=8.57/2.92`, `gpu=12.72`, `miss/unsafe=0/0` | accepted default-off partial; strongest continuation stack despite raw not reaching older best |
| strict surface time budget | `raw=102.71`, `cpu=40.79` | rejected |
| shared mid column cache | `raw=59.41`, `cpu=44.74`, `clip=28.15` | rejected; disables parallel |
| surface sort cache | `raw=86.56`, `cpu=45.95`, sort hits `0` | rejected |
| surface partial sort | `raw=70.84`, `cpu=34.35` | rejected |
| surface general strict budget | `raw=67.23`, `cpu=31.92`, `budgetReason=2` | rejected as a fix |
| mid pump 8 workers | `raw=52.19`, `cpu=36.38` | rejected |
| parallel exact + worker cache | `raw=66.03`, `cpu=30.65` | rejected/no win |
| async exact + worker cache | `raw=49.23`, `cpu=34.31`, async queued/applied `0` | no-op/rejected |

Interpretation:

- This continuation reached Completion B, not a validated 60 FPS candidate.
- The useful code step is the worker-local mid column cache: it keeps visible-critical generation synchronous while making the parallel pump less wasteful.
- The remaining representative walk cost is not one isolated bucket. Request, exact/mid generation, clip/pump, surface publication, and residual GPU cost are all material.
- The next fix should be an ownership-aware streaming/publication state split, not another queue-order, sort, scan, or worker-count tweak.
- All defaults remain unchanged; background split, clean throttle, backlog-aware pump, fallback diagnostics, async, and bounded repair remain default-off.

## Streaming Playability Campaign Surface/Scan Continuation - 2026-06-04

### Persistent Surface Workers Rejected - 2026-06-05

A default-off persistent surface extraction worker pool was implemented behind the existing parallel surface extraction path and removed after validation.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch16_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch8_candidate_20260605`

Summary:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| baseline persistent exact 2w | `21.81/17.17` | `67.23/32.40` | `51.21/28.80` | current partial |
| surface persistent 4w | `18.90/18.22` | `67.16/42.00` | `50.49/29.60` | rejected |
| surface persistent 4w batch16 | `21.84/19.62` | `60.95/44.43` | `47.96/30.80` | rejected |
| surface persistent 4w batch8 | `22.92/25.86` | `64.86/52.48` | `51.09/27.99` | rejected |

Decision:

- no persistent surface worker code or env flag remains.
- raw improvements were not stable and came with representative CPU/clip regressions.
- surface extraction should not be revisited as a thread-backend tweak; it needs lane-aware publish/backlog architecture if tackled again.

### Diagnostic-Off / Bounded64 Checks - 2026-06-05

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_noheavydiag_20260605`: diagnostics-off does not make the stack playable; walk remains about `63.72 ms` raw and high-alt about `52.95 ms` raw.
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/bounded64_noheavydiag_candidate_20260605`: bounded64 remains default-off/comparison-only; it is not the CPU fix.
- current remaining blocker is a missing ownership-aware streaming state machine across request, generation, upload/apply, surface extraction, and publish.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after hidden-tracked scan budgeting
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- `.\build.ps1 -Config Release`: passed after surface sort cache / partial-sort changes
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_hidden_tracked_scan_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_hidden_tracked_scan128_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_surface_sort_cache_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_surface_partial_sort_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/critical_reuse_surface_max1_direct_footprint_candidate_20260604/`

Patch additions:

- Added default-off hidden-exact tracked scan budgeting:
  - `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=0`
  - `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET=512`
  - `PERF_SPARSE_HIDDEN_EXACT_TRACKED_SCAN`
- Added default-off surface queue ordering candidates:
  - `VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE=0`
  - `VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT=0`
  - `PERF_SPARSE_CPU_DETAIL surface=.../sort/sortHit`

Results:

| Candidate | Walk frame 600 | Decision |
|---|---|---|
| strongest stack | `raw=46.49`, `cpu=31.05`, `request/gen/clip=8.15/7.00/15.87`, `surface=9.00/3.31`, `gpu=14.60`, `miss/unsafe=0/0` | current strongest default-off stack, not 60 FPS |
| hidden tracked scan 512 | `raw=56.03`, `cpu=32.09`, `surface=8.94/2.91` | rejected; scans dropped but surface bucket remained material |
| hidden tracked scan 128 | `raw=53.88`, `cpu=31.02`, `surface=9.16/2.87` | rejected; lower scan budget did not fix surface cost |
| surface sort cache | `raw=48.71`, `cpu=34.87`, `surface=9.07/3.27`, sort hits `0` | rejected; no useful cache reuse |
| surface partial sort | `raw=49.23`, `cpu=35.14`, `surface=8.48/3.00` | rejected as accepted stack; small local reduction only |
| direct footprint | `raw=47.85`, `cpu=33.96`, `missing=485`, coverage `94`, budget reason `2` | rejected; increased catch-up pressure |

Interpretation:

- This continuation did not stop after one partial result: tracked scan budgeting, tighter tracked budget, surface sort cache, partial sort, and direct footprint were all tested.
- None beat the strongest representative walk row.
- No validated 60 FPS candidate exists.
- The remaining blocker is architectural: public-critical/cache/prefetch ownership state, critical generation, and surface publication are still coupled enough that local scan/sort/queue tweaks only move debt between buckets.
- The next implementation slice should split streaming/publication state instead of treating unknown fallback as safe or adding another queue-order heuristic.

## Budget-Compatible Parallel Mid Pump - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the budget-compatible parallel mid-pump code change
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_candidate_20260604/`
- updated master ledger: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Patch:

- The existing default-off `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0` candidate can now run when backlog-aware pump budgeting is active.
- Under the budgeted path, the worker batch is capped by `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS` and reports budget hit when queued work remains.
- `perf_noncapture_smoke.ps1` has `-NoBacklogAwarePump` for diagnostics only.

Results:

| Candidate | Walk realtime frame 600 | High-alt | Decision |
|---|---|---|---|
| gen24 + surface max4 | `rawMs=50.51`, `cpuUpdateMs=44.10`, `clipMs=18.21`, `gpuRayMs=18.58`, `missing=446`, `sampled=376` | `rawMs=54.55`, `cpuUpdateMs=31.25` | partial only |
| no-backlog parallel | `rawMs=79.09`, `cpuUpdateMs=41.27`, `clipMs=8.08`, `missing=137`, `sampled=109` | `rawMs=60.43`, `cpuUpdateMs=30.13` | diagnostic only |
| no-backlog parallel + trim8k | `rawMs=65.62`, `cpuUpdateMs=41.29`, `missing=93`, `sampled=70` | high-alt `coverage=86`, sampled debt `865` | rejected |
| budget-compatible parallel mid pump | `rawMs=70.54`, `cpuUpdateMs=35.90`, `request/gen/clip=17.17/11.93/6.78`, `surface=11.97/3.28`, `gpuRayMs=13.72`, `missing=391`, `sampled=324` | `rawMs=60.77`, `cpuUpdateMs=27.59`, `coverage=100` | accepted default-off partial |
| budgeted parallel + gen16 | `rawMs=69.95`, `cpuUpdateMs=40.43`, `clipMs=12.57`, `coverage=94` | high-alt `cpuUpdateMs=27.15` | rejected |
| budgeted parallel + surface max1 | `rawMs=88.13`, `cpuUpdateMs=40.32`, `gpuRayMs=18.84` | high-alt `cpuUpdateMs=29.08` | rejected |

Interpretation:

- The mid-pump worker path is now a real partial candidate when used with backlog-aware safety; it is not defaulted.
- Representative walk is still not playable. The remaining measured buckets are request admission, exact generation, surface extraction/staging, and residual GPU/post-wait around sampled unknown mid debt.
- Async remains blocked for sampled unknown/invalid debt because no fallback-valid subset exists.
- The next real slice is exact request/generation/surface publication state separation: public-critical work versus repair/cache work with separate budgets and publish readiness.

## Request Pressure Trim Cycle - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after pressure-trim code changes
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_phase_split_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_free_guard_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/incremental_pressure_trim_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/incremental_pressure_trim_16k_candidate_20260604/`
- updated master ledger: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Patch:

- `PERF_SPARSE_REQUEST_DETAIL` now splits request pressure trim and pre-hierarchy phases.
- Added default-off `VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL=0` with `VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET=32768`.
- Added default-off `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=0`, then rejected it after validation.

Results:

| Candidate | Walk realtime frame 600 | High-alt frame 400 | Decision |
|---|---|---|---|
| pressure-trim free-page guard | `rawMs=164.69`, `cpuUpdateMs=141.35`, `clipMs=72.25`, `missingVoxel=524` | `rawMs=115.77`, `cpuUpdateMs=156.88` | rejected |
| incremental pressure trim 32k | `rawMs=108.50`, `cpuUpdateMs=68.71`, `request/gen/clip=29.77/29.80/9.10` | `rawMs=94.96`, `cpuUpdateMs=55.33` | partial only |
| incremental pressure trim 16k | `rawMs=117.43`, `cpuUpdateMs=69.71`, `request/gen/clip=32.85/30.15/6.69`, `surfaceExtractMs=15.56` | `rawMs=75.70`, `cpuUpdateMs=40.75` | high-alt partial, not a walk fix |

Interpretation:

- Request pressure trim/full-cache scanning is a real request-phase cost.
- Skipping pressure trim based on free pages is unsafe because it lets streaming debt accumulate into catch-up work.
- Incremental pressure trim is safer and useful as a default-off partial candidate, especially in high-alt, but it does not solve representative walk.
- The current walk blocker is visible exact request/generation plus surface extraction around sampled unknown terrain debt. Exact async/parallel, water-lane throttling, blunt caps, ring-only visible-critical heuristics, age-priority ordering, and bounded64-as-performance-fix remain rejected.

## Same-Frame Parallel Generation Cycle - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Default-off knobs added:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0`
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=0`

Findings:

- The same-frame mid-clipmap parallel pump path did not activate in the latest rows, so it is not credited as an accepted improvement.
- The same-frame exact parallel generation path activated but regressed walk/high-alt badly. Walk realtime reached `rawMs=209.53`, `cpuUpdateMs=146.40`, `clipMs=81.87`, `missingVoxel=527`; high-alt reached `rawMs=176.07`, `cpuUpdateMs=77.50`.
- Miss/unsafe stayed `0/0`, but the branch is rejected because frame time and streaming debt regressed.
- Do not retry direct exact generation as the next branch. The remaining blocker is cached exact generation/request/surface pipeline architecture around visible sampled terrain debt.

## Streaming Campaign Rejected Local Branches - 2026-06-04

Continuation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_generation_cached_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/async_exact_generation_cached_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_throttle_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/bounded64_current_stack_candidate_20260604/`

Patch:

- exact generation now has a reusable cached-column helper so worker paths can use per-worker local terrain-column caches instead of the full direct terrain generator
- default behavior remains unchanged

Results:

| Candidate | Walk realtime frame 600 | High-alt frame 400 | Decision |
|---|---|---|---|
| cached exact parallel | `rawMs=207.85`, `cpuUpdateMs=140.79`, `clipMs=77.03` | `rawMs=207.86`, `cpuUpdateMs=236.39` | rejected |
| cached async exact | `rawMs=144.58`, `cpuUpdateMs=90.44`, async enqueued `0` | `rawMs=126.38`, `cpuUpdateMs=60.48`, async enqueued `0` | rejected/no-op |
| hidden-exact water throttle | `rawMs=93.13`, `cpuUpdateMs=117.13`, `clipMs=77.13` | `rawMs=167.26`, `cpuUpdateMs=99.95` | rejected |
| bounded64 current stack | `rawMs=188.26`, `cpuUpdateMs=131.54`, `missingVoxel=736` | `rawMs=131.61`, `cpuUpdateMs=54.15` | rejected for perf; comparison-only |

Interpretation:

- Parallelizing exact generation is not enough and can make the streaming debt worse, even when the cached-column algorithm is preserved locally.
- Existing exact async does not help because the current queue is visible-class; moving visible exact work async would weaken the public-frame contract unless an owner/fallback proof exists.
- Hidden-exact water repair is a material request source, but throttling it as a blunt performance lever sends the frame into mid-clipmap catch-up and is rejected.
- Bounded64 remains the leading correctness candidate, but it is not the current performance fix and remains default-off.

Current least-bad stack row:

- `parallel_mid_pump_candidate` walk realtime frame `600`: `rawMs=75.97`, `cpuUpdateMs=40.80`, `requestMs=18.90`, `genMs=15.79`, `clipMs=6.09`, `surfaceExtractMs=11.16`, `gpuRayMs=13.45`, `missingVoxel=439`, `sampledMissingApprox=371`, `miss/unsafe=0/0`.
- This is not an accepted optimization because `parallelPumpActive=0`; treat it as the current scoreboard, not a credited patch.

Next safe branch:

- target the exact request/generation/surface-publish pipeline with ownership-aware state:
  - split hidden-exact accepted requests from terrain-critical requests,
  - distinguish public-critical exact work from repair/cache work,
  - budget surface extraction/publish by criticality,
  - keep visible sampled unknown/invalid work guarded.
- Do not retry direct/cached exact parallel, existing visible exact async, water throttle, bounded64 as a performance fix, blunt pump caps, age-priority, or ring-only visible-critical heuristics.

## Chat Rollout Scope

The referenced rollout is very large: about 5.19 GB, 170,387 JSONL records, 660 task turns, from `2026-05-14T21:51:44Z` through `2026-06-02T17:44:08Z`.

High-level event counts from the streamed extraction:

| Event kind | Count |
|---|---:|
| `response_item` | 97,875 |
| `event_msg` | 71,192 |
| `turn_context` | 986 |
| `compacted` | 333 |
| user messages | 690 |
| assistant messages | 8,970 |
| shell calls | 28,269 |
| tool/function calls total | 32,296 |

The extraction artifacts created for this handoff are under:

`tmp\chat019_extract`

Important extracted streams:

- `tmp\chat019_extract\user_messages.md`
- `tmp\chat019_extract\assistant_key_messages.md`
- `tmp\chat019_extract\final_answers.md`
- `tmp\chat019_extract\compactions.md`
- `tmp\chat019_extract\tool_calls_key.md`
- `tmp\chat019_extract\tool_outputs_key.md`
- `tmp\chat019_extract\recent_june2_raw_key.md`

The extraction was filtered around terms such as `VENPOD`, `water`, `terrain`, `SVO`, `raymarch`, `basin`, `artifact`, `TryResolve`, `PS_Raymarch`, `current_goal`, `hlsl`, and `csv`.

## Project Context

VENPOD is a voxel/terrain renderer with multiple terrain ownership layers:

- exact sparse surface / sparse pages for near editable terrain
- mid voxel clipmap for lower-resolution terrain coverage
- Far-SVO for distant voxel terrain
- deterministic far terrain height / continuity fallback
- deterministic far water / sea-plane handling
- sky/miss fallback

The active branch is:

`perf-runtime-budget`

Relevant files:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
- `assets/shaders/Common/SharedTypes.hlsli`
- `src/main_launcher.cpp`
- `src/Graphics/FarVoxelOctree.h`
- `src/Graphics/SparseVoxelGpuResources.cpp`
- `src/Graphics/Renderer.cpp`
- `engine_capture_smoke.ps1`
- `basin_water_artifact_audit.ps1`
- `water_shoreline_audit.ps1`
- `raymarch_shader_iter.ps1`
- `handoff.md`

The worktree is very dirty. Do not assume all changes are part of one clean patch. Many files across shader, renderer, sparse world, docs, and scripts are modified. The current debug/handoff work should not revert anything unless explicitly requested.

## User-Facing Problem

The renderer still looks broken in public/playable views.

Observed symptoms across the session:

- Terrain appears slowly and incoherently after startup.
- Shoreline/basin areas can show water, gaps, wrong coarse geometry, or terrain that later fills in.
- Broad/high views previously showed wrong terrain caused by analytic far-height fallback.
- Passing generic smoke gates did not mean the public renderer was coherent.
- The user correctly pushed back on "tests pass" style conclusions because screenshots and live behavior still showed partial internal streaming states.

The active goal is not "make captures pass." The active goal is:

> VENPOD must have a public-frame rendering contract: every visible pixel must have a legitimate owner now, not eventually after streaming catches up.

Legitimate owners can be exact surface, mid voxel, Far-SVO, water, or sky, but the owner must match deterministic terrain/water truth and must not expose an arbitrary half-ready streaming state.

## Main Conceptual Diagnosis

The session converged on a renderer-contract problem, not a simple material, water color, or terrain generation problem.

Earlier in the chat, the core suspicion was that the shader used the same `startDist` for two different meanings:

1. where background terrain is allowed to render
2. where diagnostics begin checking whether deterministic terrain should exist

That made telemetry misleading. If `startDist` was inflated by exact-near distance, protected bands, or dense render-box exit logic, the ray could pass through deterministic terrain before diagnostics began. Then a pixel could be classified as legitimate sky/miss even though terrain existed earlier on the ray.

The bounded diagnostic task became:

> Build a terrain-sky reason diagnostic, not a rendering fix.

That led to splitting diagnostic probing from rendering handoff distance, adding reason buckets, and using owner/material/face/debug capture modes to classify bad pixels before patching.

## Important Debug Modes

The current shader includes a number of background debug modes. The late-session evidence used these heavily:

- `54`: material debug
- `56`: owner + material
- `58`: owner layer
- `59/60/63/65/66`: LOD/face/diagnostic modes
- `61`: water
- `68`: mid diagnostic
- `70`: waterline resolver reason

Current shader state in `PS_Raymarch.hlsl`:

`BackgroundDebugLayerMode()` currently includes modes `8, 9, 49, 50, 58, 59, 60, 61, 62, 63, 65, 66, 67`.

Modes `68` and `70` exist later in the shader, but they are not included in the background debug predicate in the current file. A quick attempt to include them made the shader run very slowly or hang and was reverted. Treat mode `70` as diagnostic tooling debt until the foreground handoff is bucketed.

## Major Attempts And Findings

### 1. Terrain-Sky Diagnostic Split

The chat first focused on separating diagnostic start distance from render handoff. The reason was that background start distance could hide terrain from diagnostics.

Important functions involved:

- `SurfaceAuthoritativeBackgroundStartForRay`
- `SparseMissingPageBackgroundStartForRay`
- `RaymarchBackgroundField`
- `DebugBackgroundMissHit`
- `DiagnosticFarTerrainWouldHit`
- `TerrainDiagnosticStartDistance`

Current shader has:

```hlsl
float TerrainDiagnosticStartDistance() {
    const float midStart = MidVoxelStartDistance();
    return max(160.0f, min(midStart, ExactNearDistance() + 8.0f));
}
```

Outcome:

- Diagnostics became more useful.
- This did not itself solve rendering.
- It helped reveal that several "clean" stats were not proving public-frame coherence.

### 2. Far-Height / Far-SVO Domain Bug

A real bug was found and fixed: broad/high views could leave the Far-SVO domain even though the engine considered Far-SVO "ready."

Before fix:

- Far-SVO page radius was too small.
- Public rays left the configured Far-SVO grid.
- Deterministic terrain had no resident mid/Far-SVO owner.
- The renderer fell back to analytic far-height continuity.
- This looked like wrong detached terrain / bad far terrain.

Evidence:

- `build\captures\current_goal_v3t_mid_feedback_stored_count_20260602`
- frame 90 had `farHeight=13379`
- `farHeightReason=13379/13379/0/0/0/13379/13379/0/0`
- Meaning interpreted in chat: all far-height pixels were continuity fallback, outside mid diagnostic range and outside Far-SVO page grid.

Fix:

- `src/Graphics/FarVoxelOctree.h`
- default `FarVoxelOctreeConfig::pageRadius` changed from `6` to `12`
- radius 12 gives 25x25 pages at page size 1024, or 25,600 world units of coverage.

Validation:

- `build\captures\current_goal_v3u_farsvo_radius12_control_20260602`
- cache: 625 pages, 19,747,106 nodes, about 301.33 MB
- cold build around 25 seconds
- GPU upload around 74 ms
- far-height dropped to zero through tested frames

Default validation after code change:

- `build\captures\current_goal_v3v_default_farsvo_radius12_20260602`
- passed strict stress smoke with max height proxy screen percentage zero
- `miss=0`
- terrain-critical `postNonReady=0`
- Far-SVO cache loaded in about 295 ms at startup

Conclusion:

- This closed one real failure class.
- It did not solve the renderer.
- Do not regress to analytic far-height fallback as an acceptable public owner.

### 3. Mid-Closure / Far-Height Owner Investigation

After the Far-SVO radius fix, broad/high views still had public `farHeight` continuity fallback in some stress cases.

Tried:

- `TryBuildResidentMidVoxelClosureHit()` was relaxed for high-altitude diagnostic terrain before `midStart` / `exactNear`.
- It sampled resident ring0 mid voxels and added small post-crossing probes to tolerate voxel quantization.

Artifact:

- `build\captures\current_goal_v3m_stress_midclosure_steep_patch_20260602`

Result:

- Did not clear `farHeight`.
- frame 90 `farHeightScreenPct=0.6452`
- frame 120 `farHeightScreenPct=0.429`
- `miss=0`

Conclusion:

- Remaining `farHeight` was not simply caused by mid-closure distance/angle gating.

Also tried:

- A resident Far-SVO continuity helper that traversed resident Far-SVO page around the diagnostic terrain crossing before falling back to analytic far-height.

Artifact:

- `build\captures\current_goal_v3n_stress_far_svo_continuity_patch_20260602`

Result:

- Rejected.
- It destabilized runtime/readback.
- Errors included `GPUBuffer::Map failed: 0x-7785FFFB`.
- Backend ownership/readback went bad (`own=0` after issue).
- Frame body rose to around 130 ms.

Conclusion:

- Do not reapply broad per-pixel Far-SVO traversal in the miss path.
- If Far-SVO recovery is needed, it needs a cheaper precomputed or budgeted mechanism.

### 4. Startup Public-Render Contract

The user-visible problem was not only post-open artifacts; public rendering could open while exact-contract terrain was still known dirty.

Finding:

- Earlier work had effectively accepted `unsafe=0` or coarse coverage as "safe" even while exact-contract non-ready terrain was hidden by lower LOD.
- That was too weak for public rendering.

Patch:

- `src/main_launcher.cpp`
- `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` default changed to `1`.
- Comment updated to say same-camera unsafe feedback is part of the public rendering contract.

Current relevant source:

```cpp
const bool sparseStartupShaderUnsafeBlocksPublicRender =
    enableSparseStartupPublicRenderGate &&
    enableSparseSurfaceRaster &&
    // Same-camera unsafe feedback is part of the public rendering contract:
    // do not reveal the world while lower LOD is still hiding exact-surface
    // terrain that is known to be non-ready for the current view.
    ReadUIntEnv("VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS", 1u) != 0u;
```

Validation:

- `build\captures\current_goal_v4c_shader_unsafe_default_on_20260602`

Observed result from chat:

- Default startup waited through about frame 100 until shader-unsafe contract samples were render-ready.
- After opening, frame 120 still had about 19% `midVoxel` ownership and about 1% far-water.

Conclusion:

- The patch fixed an early exposure class.
- It did not solve post-open LOD/water/shoreline coherence.

### 5. Post-Open Owner/Material/Face/Water Debug Set

After the startup contract patch, the next question became:

> Once startup contract is clean, are remaining visible breakages legitimate mid/far terrain, water winning incorrectly, or exact surface still absent/draw-order wrong?

Diagnostic capture set:

- `build\captures\current_goal_v4d_post_open_debug_20260602`

Captured aligned modes/directories:

- `owner`
- `material`
- `face`
- `water`
- `water_reason`

The wrapper failed generic dark-image heuristics for some debug-color frames, but BMP/log artifacts existed. This was treated as harness/debug-tooling false positive, not as a renderer result.

### 6. Basin / Shoreline Audit

The most important current audit:

- `build\captures\current_goal_v4e_post_open_basin_audit_20260602`
- requested/camera frame: 121
- sample count: 512

Files:

- `basin_water_artifact_overlay.bmp`
- `basin_water_artifact_pixels.csv`
- `basin_water_artifact_summary.csv`

Summary:

| Bucket | Count |
|---|---:|
| generated land above sea, not missing water | 411 |
| water should draw before terrain | 47 |
| visible water | 44 |
| coarse layer basin terrain | 9 |
| mixed/unclassified | 1 |

Owner totals:

| Owner | Count |
|---|---:|
| `mid_voxel` | 278 |
| `far_svo` | 128 |
| `exact_sparse_surface_extended` | 105 |
| mixed/unclassified | 1 |

Top owner/material/face combinations:

| Combination | Count |
|---|---:|
| `far_svo|stone|mixed` | 125 |
| `mid_voxel|stone|side` | 93 |
| `mid_voxel|dirt|mixed` | 93 |
| `mid_voxel|water|top` | 44 |
| `exact_sparse_surface_extended|sand|side` | 39 |
| `exact_sparse_surface_extended|sand|top` | 35 |
| `exact_sparse_surface_extended|dirt|side` | 18 |

Representative `water_should_draw_before_terrain` rows:

| pixel | owner/material/face | rayTerrainT | waterPlaneT | terrainHeightAtWaterPlane |
|---|---|---:|---:|---:|
| 738,560 | `far_svo|stone|mixed` | 1496 | 1494.312 | -51.813 |
| 744,560 | `far_svo|stone|mixed` | 1494 | 1492.191 | -55.210 |
| 750,560 | `far_svo|stone|mixed` | 1492 | 1490.125 | -63.951 |
| 756,560 | `far_svo|stone|mixed` | 1490 | 1488.115 | -68.901 |
| 762,560 | `far_svo|stone|mixed` | 1488 | 1486.161 | -70.807 |
| 768,560 | `far_svo|stone|mixed` | 1486 | 1484.262 | -71.229 |

Interpretation:

- Most sampled bad-looking basin/shoreline pixels are generated land above sea level, not missing water.
- The suspicious water conflicts are real but tight: water-plane T is only about 1-2 world units before CPU terrain T at distances roughly 1058-1496.
- The suspicious rows are almost all `far_svo|stone|mixed`.
- This looks less like "all water is incorrectly winning" and more like shoreline ordering/parity at a water/terrain boundary plus coarse Far-SVO mixed cells.

## Current Shader Water Logic

Important functions:

- `LowerLodWaterlineTolerance`
- `TryResolveDeterministicWaterBeforeBackground`
- `DebugWaterlineResolverReasonHit`
- `ResolveBackgroundOcclusionAndWater`

Current waterline tolerance:

```hlsl
float LowerLodWaterlineTolerance(float backgroundDistance) {
    const float baseTolerance = max(24.0f, min(96.0f, backgroundDistance * 0.015f));
    const float coarseTolerance = max(128.0f, min(384.0f, backgroundDistance * 0.12f));
    return max(baseTolerance, coarseTolerance);
}
```

Current deterministic water-before-background path rejects if:

- no layer or already far water
- camera below water or ray angle outside accepted range
- water T outside `[32, 10400]`
- CPU terrain height at water plane is at or above sea level
- background position is too high above water
- water is too far behind background hit beyond tolerance
- exact-near ownership disallows the water hit

The current unresolved question from the chat:

> Why does `TryResolveDeterministicWaterBeforeBackground` not replace the `far_svo|stone|mixed` hits when CPU-side audit says the water plane is before terrain?

Possible explanations to test:

- Shader-side terrain height at the water plane differs from CPU audit terrain truth.
- The exact-near/background ownership guard rejects the water hit.
- The background hit is considered high/dry by shader-side `backgroundPos.y > FAR_WATER_SURFACE_Y + 96`.
- Layer/angle/T gate rejects the row.
- Debug mode 70 exists, but it is not currently included in `BackgroundDebugLayerMode()`; a quick inclusion attempt was reverted after a slow/hung run.

## Latest Startup Repair Takeover

After the waterline/basin work, the debugging moved back to the foreground handoff because the latest public-frame captures still showed the exact raster band stuck at near range only.

The critical promotion gate is in `src/main_launcher.cpp` around `hiddenExactRuntimeCleanForWideRaster`. Wide sparse-surface promotion requires hidden-exact runtime clean frames to reach `VENPOD_SPARSE_HIDDEN_EXACT_MISS_CLEAN_IDLE_FRAMES`, currently `8` in the wrapper path. If the hidden-exact scanner keeps finding accepted/missing candidates, clean rays reset and promotion stays blocked.

Important evidence:

- `build/captures/root_probe_startup_hiddenexact_block_20260602`
  - Startup hidden-exact proof could report clean, but regular hidden-exact feedback continued after that proof.
  - The first public view still had `surfaceRasterMax=1024`, `surfacePromoted=0`, and mid voxel around `19.16%`.
  - Conclusion: the startup proof footprint was too narrow to be treated as a full foreground contract.

- `build/captures/root_probe_repair_clean8_env_20260602`
  - The old repair convergence shortcut could report `hiddenExactRepairClean=8/8` while `hiddenExactRepairCleanRays=0`.
  - Conclusion: that shortcut was not a real proof and was removed.

- `build/captures/root_patch_startup_repair_scan_default_20260602`
  - Turning strict startup repair blocking on by default inherited a 16-phase full proof.
  - By frame 420 it was still at `startupProofScan=45153/150672`.
  - Public render did not open.

- `build/captures/root_patch_repair_scan_phase1_default_20260602`
  - Repair-only startup proof was changed to one phase.
  - The full sweep completed, but the scanner stayed in startup-proof mode and never accumulated warmup repair clean rays.
  - Code fix kept: repair proof mode now stops once the startup full sweep has completed.

- `build/captures/root_patch_repair_warmup_default_20260602`
  - Warmup repair scanning became active.
  - It still saw repeated hidden-exact feedback around frames 354-379, often `5-35` accepted candidates per logged frame.
  - Strict repair blocking still did not release by frame 380.

- `build/captures/root_patch_repair_code_default_off_postopen_20260602`
  - Strict repair blocking was turned back off by default in `src/main_launcher.cpp`, `engine_capture_smoke.ps1`, and `rebrun.ps1`.
  - Build passed.
  - Post-open smoke passed when captures started at frame 160.
  - Frames 160/200/240/280 still showed the stuck state:

| Frame | `surfaceRasterMax` | `surfacePromoted` | `hiddenExactMissing` | `hiddenExactAccepted` | `midVoxelScreenPct` |
|---:|---:|---:|---:|---:|---:|
| 160 | 1024 | 0 | `1/0` | 1 | 19.2018 |
| 200 | 1024 | 0 | `13/0` | 13 | 19.2009 |
| 240 | 1024 | 0 | `0/0` | 0 | 19.1995 |
| 280 | 1024 | 0 | `0/0` | 0 | 19.1995 |

At frame 240, hidden-exact feedback was zero but `clean=0/8` and `cleanRays=758/9417`, so the wide promotion gate still had not been proven clean.

- `build/captures/root_probe_promotion_gate_postopen_20260602`
  - Added `PERF_SPARSE_SURFACE_PROMOTION_GATE` logging around the wide-promotion decision.
  - Build passed and the post-open smoke passed.
  - The gate was blocked on every logged public-open sample from frames 120 through 280.
  - Common blocker pattern was `blockers=shaderMissing/runtimeClean/hiddenMiss/shaderNonReady:1/1/0/0`.
  - When hidden-exact feedback appeared again, the pattern became `1/1/1/0`.
  - `hiddenRuntimeClean=0`, `hiddenClean=0/8` throughout.
  - Clean ray progress was far too slow for startup/public promotion: examples include `50/9417` at frame 180, `1922/9417` at frame 240, `4699/9417` at frame 260, then reset to `0/9417` when feedback appeared at frame 280.
  - `shaderFeedbackRecent=0` at the 20-frame probe samples because shader unsafe feedback owner-frame age was about `10`, while `VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE` defaults to `5`.

Interpretation of the promotion-gate probe:

- Primary blocker: hidden-exact runtime clean requires full clean sweeps and is reset by later feedback, so the default proof can take hundreds or thousands of frames.
- Secondary blocker: shader unsafe feedback recency is stricter than the current feedback owner-frame cadence.
- This confirms that the current public handoff is over-constrained for a startup/open gate. A strict zero-feedback global-ish proof is not the right default unless the scanner budget/cadence changes radically.

- `build/captures/root_probe_bounded_policy_default_off_20260602`
  - Confirmed the new bounded-promotion flag does not change default behavior.
  - Post-open smoke passed.

- `build/captures/root_probe_bounded_promotion_postopen_20260602`
  - Env:

```powershell
$env:VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN='1'
$env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE='12'
```

  - Post-open smoke passed.
  - Wide promotion activated at the public-open samples:

| Frame | `surfaceRasterMax` | `surfacePromoted` | `surfaceScreenPct` | `midVoxelScreenPct` | `missScreenPct` | `unsafeNearMissScreenPct` |
|---:|---:|---:|---:|---:|---:|---:|
| 160 | 2560 | 1 | 55.3193 | 12.5235 | 0 | 0 |
| 200 | 2560 | 1 | 56.5030 | 11.5899 | 0 | 0 |
| 240 | 2560 | 1 | 57.4280 | 10.8032 | 0 | 0 |
| 280 | 2560 | 1 | 58.9799 | 9.3913 | 0 | 0 |

  - Compared with the default stuck state, surface ownership rose from about `47.7%` to `59.0%` by frame 280, and mid voxel dropped from about `19.2%` to `9.4%`.
  - This is promising but not final. The bounded policy kept promotion active while hidden-exact feedback still appeared (`hiddenExactAccepted=2` at frame 160 and `32` at frame 280), using the existing demotion bound of `128`.

## Bounded Promotion Validation Pass - 2026-06-02

The next debugging step was to stop broad root-cause hunting and validate the bounded foreground contract across longer fixed-camera, movement/stress, and basin-audit captures.

Common env:

```powershell
$env:VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN='1'
$env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE='12'
```

Validation captures:

- `build/captures/bounded_long_fixed_20260602`
  - Same fixed camera, longer run, frames `160..720`.
  - Smoke passed.
  - `surfaceRasterMax=2560`, `surfacePromoted=1`, `surfaceClean=1/1` at every sampled frame.
  - `midVoxelScreenPct` dropped from `12.6196%` at frame 160 to `4.1305%` at frame 720.
  - `surfaceScreenPct` rose from `55.2018%` to `64.1976%`.
  - Sampled `hiddenExactAccepted` bursts were bounded and drained: `0, 0, 8, 0, 21, 0, 2, 0`.
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
  - Sampled frame body times were still heavy, roughly `46-77 ms`.

- `build/captures/bounded_stress_camera_20260602`
  - High-altitude stress smoke passed safety gates.
  - This did not validate bounded wide promotion because the high-altitude path kept `surfacePromoted=0` and `surfaceRasterMax=1024`.
  - Shader unsafe contract non-ready stayed high in this high-altitude path, so suppression appears intentional for this mode.
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
  - Sampled frame body times were roughly `104-107 ms`.

- `build/captures/bounded_walk_test_20260602`
  - Low-altitude movement/walk smoke passed.
  - In this older pre-contract opt-in run, promotion was delayed while hidden-exact feedback and shader-feedback recency were not clean enough, then activated at frame 480 and stayed active through frame 720.
  - The later named-policy bound-64 walk probe superseded this timing by promoting at frame 140 after a bounded repair burst drained under the bound.
  - `midVoxelScreenPct` went from `17.9144%` at frame 160 to `0.5201%` at frame 720.
  - Sampled `hiddenExactAccepted` stayed below the current demotion bound: max `43` in sampled frames.
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
  - Sampled frame body times were still expensive, roughly `90-122 ms`.

- `build/captures/bounded_basin_audit_frame160_20260602`
  - Compared old baseline frame-121 basin audit to bounded fixed-camera frame 160. This is not an exact same-frame comparison because the frame-121 bounded one-shot did not open public render.
  - Exact sparse surface ownership increased from `105/512` to `265/512`.
  - Mid voxel ownership decreased from `278/512` to `177/512`.
  - Far-SVO ownership decreased from `128/512` to `70/512`.
  - `water_should_draw_before_terrain` decreased from `47/512` to `0/512`.
  - Visible water decreased from `44/512` to `0/512`.
  - Coarse fallback basin-terrain reason decreased from `9/512` to `0/512`.
  - All `512/512` sampled rows classified as generated land above sea level in the bounded frame-160 audit.

Interpretation:

- The bounded policy is now the strongest candidate foreground contract.
- The fixed-camera and low-altitude movement validations did not show miss/unsafe regressions.
- The basin audit materially improved: exact ownership rose and mid/Far-SVO ownership dropped.
- The high-altitude stress run is a safety check, not a promotion proof.
- The pre-contract policy was still not ready to default because frame time was high, the walk run promoted late, visual build-in/flicker had not been inspected, residual mid/Far-SVO basin ownership remained, and the `128` demotion bound was arbitrary. The later guarded-contract pass improved the promotion timing and narrowed the candidate bound to `64`, but still needs longer movement, basin, and cost validation.

## Guarded Contract Patch - 2026-06-02

The next patch turned the bounded experiment into an explicit named policy while keeping it default-off.

Code changes in `src/main_launcher.cpp`:

- Added `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY`.
  - Default is `strict`.
  - Candidate policy is `bounded_repair`.
  - The old `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN=1` path still enables the bounded behavior for compatibility.
- Added bound aliases for sweep work:
  - `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND`
  - `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND`
  - `VENPOD_SPARSE_SURFACE_PROMOTION_SHADER_NONREADY_DEMOTE_BOUND`
- Added `PERF_SPARSE_SURFACE_PROMOTION_POLICY`, which logs:
  - policy, enabled, promoted, reason
  - shader unsafe clean status and feedback age
  - hidden-exact accepted/missing and current bound
  - hidden-exact demotion bound
  - repair active, bounded repair active
  - miss/unsafe cleanliness
  - high-alt exclusion
  - demotion and demotion reason
- Added explicit public ownership cleanliness to the promotion/retention guard.
- Kept high-altitude views explicitly excluded from wide promotion.

Build:

- `.\build.ps1 -Config Release`
- Passed.
- Warnings were unchanged local `rayDir` shadow warnings in `src/main_launcher.cpp`; no compile errors.

Default-off control:

- `build/captures/contract_policy_default_off_20260602`
- `policy=strict enabled=0`
- `surfaceRasterMax=1024`, `surfacePromoted=0`
- frames 160/200/240/280 had `midVoxelScreenPct` about `19.18-19.21%`
- `missScreenPct=0`, `unsafeNearMissScreenPct=0`

Named bounded policy at bound 64:

```powershell
$env:VENPOD_SPARSE_SURFACE_PROMOTION_POLICY='bounded_repair'
$env:VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE='12'
$env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND='64'
$env:VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND='64'
```

- `build/captures/contract_policy_bounded64_fixed_final_20260602`
- Promoted by frame 140 with `reason=bounded_repair_contract`.
- Stayed promoted through frame 280.
- Frame 160: `surfaceRasterMax=2560`, `surfacePromoted=1`, hidden accepted/missing `17/17`.
- Frame 280: hidden accepted/missing `14/14`.
- `surfaceScreenPct` rose `55.459% -> 59.7338%`.
- `midVoxelScreenPct` dropped `12.5476% -> 8.647%`.
- `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
- Frame 240 body/raw: `55.24/60.38 ms`.

Bound sweep:

| Bound | Result |
|---:|---|
| 128 | Promoted by frame 140, stayed promoted, frame-280 mid voxel `8.7147%`. |
| 64 | Promoted by frame 140, stayed promoted, frame-280 mid voxel `8.647%`; best current candidate. |
| 32 | Promoted and stayed promoted in short fixed probe, frame-280 mid voxel `8.3256%`; needs longer walk/basin validation. |
| 16 | Too strict; demoted at frame 240 when hidden accepted/missing hit `32 > 16`, `surfaceRasterMax` collapsed to `1024`, mid voxel returned to `19.1792%`, and visual temporal jump was visible. |

Movement validation:

- `build/captures/contract_policy_bounded64_walk_20260602`
- At frame 120, bound `64` correctly blocked/demoted a burst of `113` hidden-exact accepted/missing.
- Promoted at frame 140 once the burst drained under the bound.
- Stayed promoted through frame 600.
- Captured frames 360/400/440/480/520/560 had `midVoxelScreenPct` `1.7945% -> 0.441%`.
- `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
- Frame 480 body/raw: `124.36/83.19 ms`.
- Visual inspection of the contact sheet did not show an obvious promotion snap in the sampled 360-560 window; most temporal change is normal camera motion. Promotion happened before that sampled window.

High-altitude validation:

- `build/captures/contract_policy_bounded64_highalt_20260602`
- Logged `reason=high_alt_excluded`.
- `surfaceRasterMax=1024`, `surfacePromoted=0`.
- `missScreenPct=0`, `unsafeNearMissScreenPct=0`.
- Frame 240 body/raw: `93.07/106.02 ms`.

Current guarded-contract interpretation:

- Root cause remains accepted: strict hidden-exact zero-feedback proof is too slow/reset-prone for public open.
- Candidate fix is now implemented as a named, default-off contract policy.
- Bound `64` is the best current candidate from this pass.
- Bound `32` is plausible but needs longer movement and basin validation.
- Bound `16` is too strict for observed fixed-camera repair bursts.
- Do not make the policy default yet; next evidence should be longer movement, matched-frame basin audit, and cost attribution.

Current code state from this takeover:

- `VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS` default is back to `0`.
- The wrappers also default repair blocking to `0`.
- The bogus zero-ray repair convergence path is removed.
- Repair-only startup proof defaults to one phase unless explicit hidden-exact proof blocking is enabled.
- Repair proof mode now exits after the startup full sweep so warmup repair scanning can run.
- If strict repair convergence is explicitly enabled and does converge, it seeds runtime hidden-exact clean frames.
- `PERF_SPARSE_SURFACE_PROMOTION_GATE` logs the current promotion blockers every 20 public-open frames.
- `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN` is available as an opt-in bounded-promotion experiment and defaults to `0`.
- `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair` is now the preferred named opt-in for the guarded bounded contract.
- `PERF_SPARSE_SURFACE_PROMOTION_POLICY` logs the policy decision and demotion reason every 20 public-open frames.

Interpretation:

Strict startup hidden-exact repair blocking is not a viable default right now. It is a useful diagnostic mode, but the candidate stream is persistent enough that a zero-feedback public gate can stall. The next fix should be a foreground ownership contract, not another unconditional startup wait.

## What Is Fixed

These are real fixes or useful deltas, not final completion:

1. Far-SVO radius `6 -> 12`
   - Fixed one broad-view owner-contract bug.
   - Removed tested analytic far-height fallback caused by leaving the Far-SVO domain.

2. Startup shader-unsafe block default `0 -> 1`
   - Prevents public render from opening while same-camera exact-contract feedback still says non-ready terrain is hidden by lower LOD.
   - Verified to delay public opening until a cleaner proof in default capture.

3. Waterline reason tooling located, but not reliable yet
   - Modes `68` and `70` exist in `PS_Raymarch.hlsl`.
   - They are not currently included in `BackgroundDebugLayerMode()`.
   - A quick inclusion attempt was reverted after a slow/hung shader run, so this remains tooling debt rather than a fix.

4. Hidden-exact repair accounting made honest
   - Removed the path that could mark startup repair converged with `cleanRays=0`.
   - Fixed repair proof mode so it can leave full-sweep proof and enter warmup scanning.
   - Kept strict repair blocking opt-in because default strict blocking stalled public open.

5. Bounded wide-promotion experiment added, default off
   - Preferred env flag: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair`.
   - Legacy env flag: `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN`.
   - Default remains off.
   - Broader validation shows it can promote the wide surface band in fixed-camera and low-altitude movement runs, reduce mid voxel materially, and improve basin ownership without miss/unsafe regressions.
   - The new guarded-contract pass adds explicit policy logging, demotion reasons, public miss/unsafe cleanliness, hidden-exact bounds, and high-alt exclusion.
   - It remains experimental because frame time, matched-frame basin ownership, longer movement behavior, and final bound choice are unresolved.

## What Was Disproven Or Deprioritized

- This is not primarily a material/color tuning problem.
- This is not solved by saying `miss=0`.
- This is not solved by saying `farHeight=0`.
- This is not solved by passing one stress capture.
- The rejected per-pixel Far-SVO traversal helper should not be reapplied.
- Generic wrapper failures on dark debug-color frames are not proof of renderer failure when BMP/log artifacts were produced before the heuristic failed.
- Water is not the dominant bug in all views. In the old frame-121 basin audit, most suspicious-looking patches were generated land above sea level and the water-before-terrain class was smaller; in the bounded frame-160 audit, that water class dropped to `0/512`.
- Strict startup hidden-exact repair blocking is not a shippable default yet. It correctly exposes that the candidate stream is not clean, but it can hold public render indefinitely instead of producing a better first public frame.

## Current Blocker

Default rendering is still exposing coarse/incomplete ownership after public open.

Current evidence points to two active classes:

1. Lower-LOD ownership in near-ish shoreline/basin views
   - Frame 121 after startup contract is clean still has substantial `mid_voxel` and `far_svo` ownership in a user-visible shoreline/basin area.
   - Many pixels are side/mixed faces, which visually read as coarse terrain patches or gaps.
   - Latest post-open validation still has `surfaceRasterMax=1024`, `surfacePromoted=0`, and `midVoxelScreenPct=19.20%` at frames 160-280.
   - The wide promotion gate is blocked because hidden-exact runtime clean has not reached `8/8`.
   - Promotion-gate probe confirms this is not a surface backlog or shader non-ready problem in the tested post-open frames.
   - The opt-in bounded promotion tests reduced this class materially but did not fully eliminate it.
   - Latest bounded basin comparison: exact ownership `105/512 -> 265/512`, mid voxel `278/512 -> 177/512`, Far-SVO `128/512 -> 70/512`.

2. Tight water/terrain boundary ordering
   - 47/512 sampled audit pixels say water plane should draw before terrain.
   - Those rows are mostly `far_svo|stone|mixed`.
   - Water T is only 1-2 units before terrain T, so this may be coarse-cell parity/tolerance rather than large-scale water theft.
   - In the bounded frame-160 basin audit this class was `0/512`, so do not return to mode 70 before deciding the foreground contract unless a new bounded audit shows water rows dominate again.

The current highest-value mechanism question:

> Can the bounded foreground contract replace the too-slow zero-feedback hidden-exact proof, and what guardrails make that safe enough to default?

## Recommended Next Work

1. Keep the bounded policy default-off until the foreground contract has explicit guardrails.

2. Continue validating the named bounded contract, not broad root-cause discovery:
   - current candidate: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair`
   - current best bound: `64`
   - compare with bound `32` on longer movement and basin captures

3. Validate the visual part that CSVs cannot prove:
   - inspect longer fixed-camera and walk captures for visible build-in/flicker
   - determine whether residual `177/512` mid voxel and `70/512` Far-SVO basin rows are visually acceptable or need a stricter foreground band

4. Profile promoted frame cost:
   - final fixed bound-64 frame 240 body/raw: `55.24/60.38 ms`
   - bound-64 walk frame 480 body/raw: `124.36/83.19 ms`
   - bound-64 high-alt frame 240 body/raw: `93.07/106.02 ms`

5. Re-run a longer movement capture:
   - bound `64` no longer waits until frame 480 in the latest walk probe
   - it blocked/demoted at frame 120 because hidden accepted/missing was `113 > 64`
   - it promoted at frame 140 once the burst drained and stayed promoted through frame 600

6. Re-run a matched-frame basin audit if possible:
   - the old baseline was frame 121
   - the bounded basin audit used frame 160 because the frame-121 one-shot did not open public render
   - a same-frame comparison would remove that caveat

7. Only after the foreground contract is accepted or disproven, return to the smaller waterline class:
   - make mode `70` reliable without the broad slow/hung `BackgroundDebugLayerMode()` change
   - re-run the same post-open debug set
   - join rows where `reasonBucket == water_should_draw_before_terrain` with sampled mode-70 color/reason

Bounded foreground failure buckets:

| Class | Meaning |
|---|---|
| clean sweep too slow | no feedback now, but clean rays need too many frames |
| persistent hidden-exact feedback | accepted/missing candidates continue after public open |
| shader unsafe sample missing/stale | promotion requires public shader feedback that is absent or too old |
| non-ready exact remains bounded | exact misses are small enough for an explicit bounded repair policy |
| lower LOD needs equivalence proof | mid/Far-SVO can be allowed only after a deterministic bound |

Waterline failure buckets, only after foreground contract is bucketed:

| Class | Meaning |
|---|---|
| shader terrain says dry | CPU/shader terrain truth mismatch |
| water too far behind background | tolerance/order issue |
| exact-near rejects water | public ownership guard issue |
| layer/angle/T gate rejects | deterministic water resolver gate issue |
| resolver never reached | call ordering/debug path issue |
| accepted in debug but not normal | normal/debug path divergence |

Likely patch directions depending on evidence:

- If clean sweep is too slow but feedback is already zero: make the clean proof camera-footprint bounded and explicitly seeded from startup/private proof.
- If feedback persists: classify those candidates by owner/material/face before relaxing the gate.
- If lower LOD is meant to carry the view: add a deterministic equivalence/bounded-error contract for mid/Far-SVO ownership instead of requiring exact zero feedback.
- If shader terrain truth differs from CPU audit: align audit terrain function or shader seed/height function before changing rendering.
- If exact-near rejects water incorrectly: inspect `BackgroundHitAllowedByExactNear` for far water in the exact public band.
- If tolerance rejects rows where water is only 1-2 units ahead: this may be a boundary/tolerance bug around coarse Far-SVO mixed cells.
- If resolver is bypassed: inspect `ResolveBackgroundOcclusionAndWater` call ordering and layer values for `BACKGROUND_LAYER_FAR_SVO`.
- If mode 70 still fails: treat it as diagnostic tooling debt, then continue from owner/material/face evidence rather than tuning visuals blindly.

## Commands And Tooling That Mattered

Common build/capture/debug commands used in the rollout:

```powershell
.\rebrun.ps1 ...
.\engine_capture_smoke.ps1 ...
.\basin_water_artifact_audit.ps1 ...
Select-String -Path .\assets\shaders\Graphics\PS_Raymarch.hlsl -Pattern ...
Import-Csv build\captures\...\basin_water_artifact_pixels.csv | Group-Object ...
```

The latest relevant capture directories are:

```text
build\captures\current_goal_v4e_post_open_basin_audit_20260602
build\captures\current_goal_v4d_post_open_debug_20260602
build\captures\current_goal_v4c_shader_unsafe_default_on_20260602
build\captures\current_goal_v4b_shader_unsafe_block_control_20260602
build\captures\current_goal_v4a_default_frame121_owner_20260602
build\captures\current_goal_v3v_default_farsvo_radius12_20260602
build\captures\current_goal_v3u_farsvo_radius12_control_20260602
build\captures\current_goal_v3t_mid_feedback_stored_count_20260602
build\captures\root_patch_repair_code_default_off_postopen_20260602
build\captures\root_patch_repair_warmup_default_20260602
build\captures\root_patch_repair_scan_phase1_default_20260602
build\captures\root_patch_startup_repair_scan_default_20260602
build\captures\root_probe_repair_clean8_env_20260602
build\captures\root_probe_startup_hiddenexact_block_20260602
build\captures\root_probe_promotion_gate_postopen_20260602
build\captures\root_probe_bounded_policy_default_off_20260602
build\captures\root_probe_bounded_promotion_postopen_20260602
build\captures\bounded_long_fixed_20260602
build\captures\bounded_stress_camera_20260602
build\captures\bounded_walk_test_20260602
build\captures\bounded_basin_frame160_owner_20260602
build\captures\bounded_basin_frame160_material_20260602
build\captures\bounded_basin_frame160_face_20260602
build\captures\bounded_basin_audit_frame160_20260602
build\captures\contract_policy_default_off_20260602
build\captures\contract_policy_bounded128_fixed_20260602
build\captures\contract_policy_bounded64_fixed_20260602
build\captures\contract_policy_bounded32_fixed_20260602
build\captures\contract_policy_bounded16_fixed_20260602
build\captures\contract_policy_bounded64_fixed_final_20260602
build\captures\contract_policy_bounded64_walk_20260602
build\captures\contract_policy_bounded64_highalt_20260602
```

Useful commands to resume:

```powershell
git status --short
Select-String -Path .\assets\shaders\Graphics\PS_Raymarch.hlsl -Pattern 'BackgroundDebugLayerMode|TryResolveDeterministicWaterBeforeBackground|DebugWaterlineResolverReasonHit|LowerLodWaterlineTolerance' -Context 3,10
Get-Content build\captures\current_goal_v4e_post_open_basin_audit_20260602\basin_water_artifact_summary.csv
Import-Csv build\captures\current_goal_v4e_post_open_basin_audit_20260602\basin_water_artifact_pixels.csv | Where-Object {$_.reasonBucket -eq 'water_should_draw_before_terrain'} | Select-Object -First 20
rg -n "PERF_CAMERA_EXPOSURE frame=(160|200|240|280)|PERF_SPARSE_HIDDEN_EXACT_MISS frame=(160|180|200|210|240|280)" build\captures\root_patch_repair_code_default_off_postopen_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_GATE|PERF_CAMERA_EXPOSURE frame=(160|200|240|280)" build\captures\root_probe_promotion_gate_postopen_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_GATE|PERF_CAMERA_EXPOSURE frame=(160|200|240|280)" build\captures\root_probe_bounded_promotion_postopen_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_GATE|PERF_CAMERA_EXPOSURE frame=(160|240|320|400|480|560|640|720)|PERF_FRAME_END frame=(240|480|720)" build\captures\bounded_long_fixed_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_GATE|PERF_CAMERA_EXPOSURE frame=(160|240|320|400|480|560|640|720)|PERF_FRAME_END frame=(240|480|720)" build\captures\bounded_walk_test_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_POLICY|PERF_CAMERA_EXPOSURE frame=(160|200|240|280)|PERF_FRAME_END frame=240" build\captures\contract_policy_bounded64_fixed_final_20260602\venpod_runtime.log
rg -n "PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=(120|140|360|400|440|480|520|560)|PERF_CAMERA_EXPOSURE frame=(360|400|440|480|520|560)|PERF_FRAME_END frame=480" build\captures\contract_policy_bounded64_walk_20260602\venpod_runtime.log
Import-Csv build\captures\root_patch_repair_code_default_off_postopen_20260602\layer_screen_timeline.csv | Where-Object { [int]$_.frame -in 160,200,240,280 } | Select-Object frame,surfaceScreenPct,backgroundScreenPct,midVoxelScreenPct,farSvoScreenPct,farWaterScreenPct,waterContextScreenPct,skyScreenPct,missScreenPct,unsafeNearMissScreenPct
Import-Csv build\captures\bounded_basin_audit_frame160_20260602\basin_water_artifact_summary.csv
```

## Performance Root-Cause Pass

Date: 2026-06-02

Current pivot:

- Stop broad visual correctness work for now.
- Keep `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict` as the default.
- Keep `bounded_repair` default-off.
- Keep bound `64` as a correctness candidate only.
- Do not return to water/mode 70 in this pass.

New/confirmed instrumentation:

- Existing timing logs are sufficient for the frame waterfall:
  - `PERF`
  - `PERF_FRAME_END`
  - `PERF_SPARSE`
  - `PERF_SPARSE_CLIPMAP`
  - `PERF_SPARSE_SURFACE`
  - `PERF_SPARSE_HIDDEN_EXACT_MISS`
  - `PERF_SPARSE_EDIT_PUBLISH`
  - `PERF_CAMERA_EXPOSURE`
  - `PERF_SPARSE_SURFACE_PROMOTION_POLICY`
- Added `PERF_SPARSE_EDIT_LATENCY` in `src/main_launcher.cpp` for brush/edit queue and GPU-apply latency.
- Added `perf_root_cause_audit.ps1`.

Generated artifacts:

```text
build\captures\perf_root_cause_20260602\perf_cost_table.csv
build\captures\perf_root_cause_20260602\perf_cost_table.md
build\captures\perf_root_cause_20260602\edit_latency_events.txt
build\captures\perf_non_capture_default_strict_nophys_20260602
build\captures\perf_edit_brush_paint_20260602
```

Cost table:

| Scenario | Frame | Promoted | Raster | Mid % | Body ms | Raw ms | GPU ray ms | CPU update ms | Hidden repair ms | Surface extract ms | Page gen ms | Upload ms | Readback ms | Present/wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| default strict fixed capture | 240 | 0 | 1024 | 19.18 | 113.99 | 87.08 | 61.55 | 55.91 | 4.51 | 10.78 | 9.51 | 0.52 | 0.01 | 57.92 |
| bounded64 fixed capture | 240 | 1 | 2560 | 9.20 | 55.24 | 60.38 | 41.65 | 43.25 | 4.04 | 3.05 | 5.72 | 0.33 | 0.01 | 11.90 |
| bounded64 walk capture | 480 | 1 | 2560 | 0.38 | 124.36 | 83.19 | 25.73 | 92.59 | 2.46 | 5.82 | 9.58 | 1.69 | 0.03 | 31.69 |
| high-alt excluded capture | 240 | 0 | 1024 | 0.00 | 93.07 | 106.02 | 64.29 | 55.40 | 2.74 | 4.35 | 5.25 | 1.64 | 0.01 | 37.58 |
| edit brush paint capture | 240 | 0 | 1024 | 19.19 | 62.77 | 67.40 | 60.12 | 45.81 | 4.04 | 4.92 | 7.40 | 0.46 | 0.01 | 16.88 |
| non-capture strict, physics disabled | 240 | 0 | 1024 | 19.18 | 62.31 | 82.02 | 60.09 | 50.15 | 4.10 | 3.21 | 6.05 | 0.33 | 0.10 | 12.07 |

Interpretation:

- Capture/debug is not the primary blocker. It can inflate frame-end body/gap timings, but the non-capture no-physics run still has `gpuRayMs=60.09` and `cpuUpdateMs=50.15` at frame 240.
- The GPU raymarch floor is the first hard blocker: fixed/default/high-alt/edit/non-capture samples sit around `60-64 ms` GPU ray, already far above a 16.67 ms frame budget.
- Movement has a separate CPU sparse streaming/update bottleneck. The bound64 walk frame has `cpuUpdateMs=92.59`, with `37.19 ms` sparse request, `37.15 ms` clipmap, `9.58 ms` generation, and `6.97 ms` trim.
- Hidden-exact repair is not the dominant sampled cost (`2.46-4.51 ms`).
- Upload/readback are not dominant in the sampled rows.
- Surface extraction/staging is a secondary cost, not the first blocker: extraction is `3.05-10.78 ms`; walk staging reached `11.51 ms`.
- Bounded promotion is not currently the performance culprit. The bounded64 fixed capture is faster than strict fixed in the sampled frame, and high-alt remains slow while promotion is explicitly excluded.

Edit/build latency probe:

- The edit smoke ran and produced useful latency data, but failed its own final coverage gate:
  - `frames=120/60`
  - `queued=9/3`
  - `applied=52/1`
  - `fallback=0`
  - `missingResident=0`
  - `overflow=0`
  - `deltaMismatch=0`
  - `cases=3/4`
  - `caseQueued=3/3/3/0`
  - `pathCells=0`
- Treat this as a partial edit-latency probe, not an edit correctness pass.
- Successful queue/apply events:
  - frame `120` queued, frame `123` GPU-applied, `3` frames delayed, `16` records, `16` applied, `4` pages touched, `4` edited publishes completed
  - frame `165` queued, frame `168` GPU-applied, `3` frames delayed, `29` records, `29` applied, `1` page touched
  - frame `210` queued, frame `213` GPU-applied, `3` frames delayed, `7` records, `7` applied, `2` pages touched, `2` edited publishes completed
- The successful edit path did not hit CPU fallback, missing-resident hints, or overflow.
- Post-edit frame spikes are not dominated by the brush code. The worst post-edit raw frames around `105-127 ms` report `brush=0` and are dominated by sparse request/generation plus the persistent `~61 ms` GPU raymarch.
- First visible correct edit frame is not yet proven by an automated material-diff check. The latency log records `firstVisibleFrame=0` until that validator exists.

Current performance root:

1. GPU raymarch is too expensive in ordinary views.
2. Sparse CPU request/clipmap/generation work becomes too expensive during movement.
3. Edit latency has a fixed GPU-feedback delay of about `3` frames, but edit/build lag is currently masked by the renderer's baseline frame-time cost.

Recommended next optimization patch:

Target the GPU raymarch floor first. Add a narrow raymarch-cost reduction pass for the fullscreen background ray path, then measure before/after using `gpuRayMs`, `rayScale`, `rayBudget`, ownership percentages, `missScreenPct`, and `unsafeNearMissScreenPct`. After that, separately optimize movement CPU sparse request/clipmap work.

## GPU Raymarch Floor Ablation - 2026-06-02

Audit name:

```text
gpu_raymarch_floor_ablation_20260602
```

Generated artifacts:

```text
build\captures\gpu_raymarch_floor_ablation_20260602\raymarch_floor_summary.csv
build\captures\gpu_raymarch_floor_ablation_20260602\raymarch_floor_table.md
```

New default-neutral knobs added:

- `VENPOD_RAYMARCH_RENDER_SCALE=1.0` by default
- `VENPOD_RENDER_QUALITY=playable`, opt-in alias for render scale `0.5`
- `VENPOD_RAYMARCH_MAX_STEPS_SCALE=1.0` by default
- `VENPOD_RAYMARCH_MAX_DISTANCE_SCALE=1.0` by default

Existing knobs used for attribution:

- `VENPOD_RAYMARCH_MAX_DISTANCE`
- `VENPOD_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_DISTANCE`
- `VENPOD_SPARSE_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT`
- `VENPOD_ENABLE_FAR_SVO`
- `VENPOD_SPARSE_MID_VOXEL_RENDER`
- `VENPOD_SPARSE_MID_CLIPMAP`

Resolution sweep, strict fixed camera, frame `240`:

| Scale | Output | GPU ray ms | Speedup | CPU update ms | Body/raw ms | Miss/unsafe |
|---:|---:|---:|---:|---:|---:|---:|
| `1.00` | `1920x1080` | `61.92` | `1.00x` | `44.65` | `59.18/65.32` | `0/0` |
| `0.75` | `1440x810` | `41.58` | `1.49x` | `63.37` | `83.85/94.73` | `0/0` |
| `0.50` | `960x540` | `28.38` | `2.18x` | `65.78` | `84.56/90.39` | `0/0` |
| `0.33` | `634x356` | `28.23` | `2.19x` | `68.84` | `89.59/98.28` | `0/0` |

This shows real pixel-count sensitivity, but not pixel-linear scaling. Half resolution removes about `33.5 ms` of GPU ray time; further reduction to `0.33` does not materially improve beyond the `~28 ms` residual floor.

Max-step sweep, strict fixed camera, native resolution:

| Step scale | Ray budget steps | GPU ray ms | Speedup | Miss/unsafe |
|---:|---:|---:|---:|---:|
| `1.00` | `73` | `61.19` | `1.00x` | `0/0` |
| `0.75` | `55` | `60.64` | `1.01x` | `0/0` |
| `0.50` | `36` | `61.09` | `1.00x` | `0/0` |
| `0.25` | `18` | `60.00` | `1.02x` | `0/0` |

The exposed max-step budget is not the primary cause of the native GPU floor.

Feature ablation, strict fixed camera, native resolution:

| Diagnostic row | GPU ray ms | Delta vs strict | Notes |
|---|---:|---:|---|
| strict baseline | `61.19` | `0.00` | baseline |
| Far-SVO off | `29.41` | `-31.78` | dominant measured GPU contributor |
| mid voxel render off | `52.26` | `-8.93` | secondary |
| runtime quality ceiling 50 | `60.27` | `-0.92` | current quality scalar does not address the floor |

Far-SVO final ownership is only about `0.22%` in the fixed strict frame, but disabling the Far-SVO path still cuts GPU ray time roughly in half. The likely cost is broad per-pixel Far-SVO/background traversal/check overhead, not just the pixels that retire as Far-SVO.

High-altitude rows:

| Row | Output | GPU ray ms | CPU update ms | Notes |
|---|---:|---:|---:|---|
| high-alt native | `1920x1080` | `79.86` | `260.99` | promotion excluded |
| high-alt half res | `960x540` | `40.66` | `324.02` | GPU improves `1.96x`; CPU remains dominant |
| high-alt half steps | `1920x1080` | `78.15` | `351.81` | max-step reduction does not fix high-alt |

Bounded promotion comparison:

- strict native fixed: `gpuRayMs=61.92`, `surfacePromoted=0`, `surfaceRasterMax=1024`
- bounded64 native fixed: `gpuRayMs=42.70`, `surfacePromoted=1`, `surfaceRasterMax=2560`
- bounded64 half-res fixed: `gpuRayMs=24.01`

Conclusion:

The native `~60 ms` GPU ray floor is primarily a Far-SVO/background fullscreen raymarch cost with pixel-count sensitivity and a residual `~28 ms` floor. It is not caused by the bounded promotion policy and is not materially improved by the exposed max-step budget.

Implemented first playability lever:

- `VENPOD_RAYMARCH_RENDER_SCALE=0.5`
- or `VENPOD_RENDER_QUALITY=playable`

Measured effect in strict fixed camera:

- `gpuRayMs`: `61.92 -> 28.38`
- speedup: `2.18x`
- `missScreenPct=0`
- `unsafeNearMissScreenPct=0`

This lever is default-off and is not a complete 60 fps fix. It exposes the next two blockers: residual `~28 ms` GPU ray cost and high CPU sparse update cost.

Recommended next patch:

Do not disable Far-SVO as the correctness fix. Make the Far-SVO/background ray path cheaper: lower-res far/background pass, tile/region early reject, or split the far/background pass from exact foreground ownership so full-resolution pixels do not all pay the Far-SVO traversal/check cost. Continue measuring `gpuRayMs`, ownership percentages, `missScreenPct`, and `unsafeNearMissScreenPct`.

## Far-SVO/background GPU reduction pass

Audit:

```text
far_svo_background_gpu_reduction_20260602
```

Artifacts:

```text
build\captures\far_svo_background_gpu_reduction_20260602\far_svo_gpu_summary.csv
build\captures\far_svo_background_gpu_reduction_20260602\far_svo_gpu_table.md
```

Default-off knob added:

- `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE=1.0` by default
- tested candidate: `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE=0.5`

This is a C++-side budget lever. It scales `camera.farFieldQuality` before renderer constants are uploaded, so the existing shader Far-SVO quality/page-step schedule does the work. It does not set `VENPOD_ENABLE_FAR_SVO=0`, does not default bounded promotion, and does not change water/mode70.

Shader instrumentation note:

- Per-attempt Far-SVO/background atomic stats were attempted and removed because they created multi-minute runtime DXC compiles.
- A shader-side `defer elevated Far-SVO` branch was also removed; shader source churn around `PS_Raymarch.hlsl` is currently expensive enough to be tooling debt.
- The audit used existing final ownership counters plus GPU timing. The local shader cache was seeded with the prior known-good `7386940` byte PS_Raymarch compiled shader to keep the capture matrix runnable after source-hash churn.

Main result:

| Scenario | Baseline GPU ray | Quality 0.5 GPU ray | Delta | Far-SVO baseline -> candidate | Miss/unsafe |
|---|---:|---:|---:|---:|---:|
| strict native fixed | `59.96 ms` | `34.20 ms` | `-25.76 ms` | `0.22% -> 0.00%` | `0/0` |
| strict half fixed | `35.86 ms` | `28.91 ms` | `-6.95 ms` | `0.22% -> 0.00%` | `0/0` |
| bounded64 native fixed | `46.05 ms` | `29.61 ms` | `-16.44 ms` | `0.09% -> 0.00%` | `0/0` |
| high-alt native | `78.99 ms` | `74.82 ms` | `-4.17 ms` | `0.92% -> 0.93%` | `0/0` |
| high-alt half | `67.23 ms` | `71.97 ms` | `+4.74 ms` | `1.04% -> 1.05%` | `0/0` |
| walk native | `0.02 ms` | `2.98 ms` | `+2.96 ms` | `0.00% -> 0.00%` | `0/0` |

Decision:

The quality-scale knob is safe to keep default-off as a diagnostic/perf-budget lever, but it is not a playable-mode default candidate yet. It gives a strong strict fixed-camera GPU win, but in low-alt fixed rows it effectively removes final Far-SVO ownership, and it does not consistently help high-alt or walk rows.

Next recommendation:

Continue the GPU background pass with a real pass architecture change: lower-res far/background pass, tile/region reject, or pass split. Do not depend on suppressing Far-SVO ownership as the fix.

## Background pass split / mask prototype - 2026-06-02

Audit:

```text
background_pass_split_or_mask_20260602
```

Artifacts:

```text
build\captures\background_pass_split_or_mask_20260602\background_split_summary.csv
build\captures\background_pass_split_or_mask_20260602\background_split_table.md
build\captures\background_pass_split_or_mask_20260602\background_split_contact_sheet.png
```

Prototype chosen:

- lower-resolution background pass split
- exact sparse surface raster stays full-resolution on the swapchain target
- `PS_Raymarch` renders to a lower-resolution offscreen color/depth target
- a tiny `PS_BackgroundComposite` shader samples that target and composites only where full-resolution stencil is zero
- no `PS_Raymarch.hlsl` edit was needed

New default-off/default-neutral knobs:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0`
- tested candidate: enable `1`, scale `0.375`
- audit-only fixed-camera helper: `VENPOD_RAYMARCH_FIXED_CAMERA=0`

Main timing result:

| Scenario | Baseline GPU ray | Candidate GPU ray | Notes |
|---|---:|---:|---|
| strict native fixed | `61.33 ms` | `32.37 ms` | `1.89x`, strict GPU target hit |
| strict playable fixed | n/a | `24.39 ms` | global half-res plus split, diagnostic only |
| bounded64 native fixed | n/a | `28.68 ms` | comparison only; bounded remains default-off |
| high-alt native | `79.97 ms` | `35.73 ms` | directional only; stress camera was not matched |
| walk native | n/a | `21.01 ms` | CPU still dominant at `110.78 ms` |

Safety/ownership result:

- sampled split rows kept `missScreenPct=0`
- sampled split rows kept `unsafeNearMissScreenPct=0`
- Far-SVO sample ownership did not collapse to zero (`642` strict low-res samples, `3141` high-alt low-res samples)
- split-row ownership percentages are low-resolution raymarch sample counters, not true full-resolution final composite ownership counters

Visual result:

- The prototype failed visual validation.
- Contact sheet shows white sampled background/sky in unstenciled regions for split rows.
- A temporary constant-color composite probe confirmed the full-resolution stencil composite executes.
- Therefore the bad output is in the lower-resolution background target content path or SRV contents, not the stencil gate itself.

Decision:

Keep the code default-off. The architecture is still promising because it cuts strict native GPU ray time from `61.33 ms` to `32.37 ms`, but it is not correctness-preserving and is not a playable-mode candidate until the background target content path is fixed, true composite counters are added, and matched high-alt captures are rerun.

## Background Split Content Fix - 2026-06-02

Audit:

```text
background_split_content_fix_20260602
```

Artifacts:

```text
build\captures\background_split_content_fix_20260602\background_split_content_summary.csv
build\captures\background_split_content_fix_20260602\background_split_content_table.md
build\captures\background_split_content_fix_20260602\contact_sheet.png
```

Split rows also include lower-res background target readbacks named `background_pass_frame_*.bmp` and final composites named `engine_frame_*.bmp`.

Root cause:

- `BackgroundPassColor` was valid and the constant-color composite draw path worked.
- The background pass SRV had been copied into shader-visible descriptor index `0`.
- The ImGui backend also uses descriptor index `0` for the font texture.
- ImGui overwrote descriptor `0`, so `PS_BackgroundComposite` sampled the ImGui font/white texture instead of the lower-res background target.
- `Renderer` now reserves shader-visible descriptor `0` for ImGui immediately after heap initialization; renderer-owned shader-visible SRVs now start at index `1`.

Debug probes:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_FORCE_COLOR=1` now produces a green lower-res readback and a green full-res composite.
- Normal split readbacks now contain plausible terrain/sky/water content.
- `lowResWhiteOrClearPct=0` in the sampled split rows.
- Full-resolution foreground/composite counters come from `PERF_RENDER_COMPOSITION` (`surfaceOwnedPixels`, `backgroundPixels`, `screen`), not low-res ownership percentages.

Matched validation:

| Scenario | Frame | Split | Background res | Foreground stencil % | Composite % | GPU ray ms | CPU update ms | Far-SVO % | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| strict native fixed baseline | `380` | `0` | `1920x1080` | `47.778` | `52.222` | `60.38` | `41.89` | `0.219` | `0/0` |
| strict native fixed split | `380` | `1` | `720x405` | `83.813` | `16.187` | `17.88` | `39.33` | `0.030` | `0/0` |
| strict playable fixed split | `380` | `1` | `360x203` | `83.763` | `16.237` | `10.62` | `39.20` | `0.031` | `0/0` |
| bounded64 native fixed split | `380` | `1` | `720x405` | `85.938` | `14.063` | `18.75` | `40.71` | `0.023` | `0/0` |
| high-alt native baseline matched | `240` | `0` | `1920x1080` | `36.442` | `63.558` | `73.76` | `164.84` | `0.293` | `0/0` |
| high-alt native split matched | `240` | `1` | `720x405` | `84.404` | `15.596` | `29.86` | `64.08` | `0.043` | `0/0` |
| walk native split | `480` | `1` | `720x405` | `84.593` | `15.407` | `17.86` | `81.12` | `0.127` | `0/0` |

Decision:

- The lower-res background split is now visually plausible in the sampled contact sheet.
- It preserves `missScreenPct=0` and `unsafeNearMissScreenPct=0` in the sampled rows.
- It meets the strict native fixed GPU target: `60.38 -> 17.88 ms`.
- Matched high-alt improves materially: `73.76 -> 29.86 ms`.
- Playable fixed split reaches `10.62 ms` GPU ray time, but total frame time is still not a 60 fps claim because CPU sparse update is still about `39 ms`.
- Bounded promotion remains not the performance culprit; bounded64 split is comparable to strict split and remains default-off.
- Keep `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0` and `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0` by default.
- The next performance pass should target movement sparse CPU request/clipmap/generation/trim cost unless the split needs longer visual validation first.

## Movement Sparse CPU Update Reduction - 2026-06-03

Audit:

```text
movement_sparse_cpu_update_reduction_20260602
```

Artifacts:

```text
build\captures\movement_sparse_cpu_update_reduction_20260602\cpu_sparse_summary.csv
build\captures\movement_sparse_cpu_update_reduction_20260602\cpu_sparse_table.md
build\captures\movement_sparse_cpu_update_reduction_20260602\contact_sheet.png
```

Added CPU-side detail logging:

- `VENPOD_SPARSE_CPU_DETAIL=1`
- `PERF_SPARSE_CPU_DETAIL`
- request attempts/unique/duplicates/resident/nonresident/allocated and request skip reasons
- trim and replacement scan calls/records/candidates/evictions
- mid-clipmap interest/reuse/pump/generation/missing counts
- surface extract/stage timings and extraction backlog

Implemented one default-off optimization knob:

- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS=0` default
- nonzero values cap the live mid-voxel clipmap pump only after startup/public readiness has opened
- the cap is now inactive for the high-alt LOD-carrying path because unguarded high-alt cap probes caused white-terrain visual regressions

Also added equivalent top-K eviction selection:

- `VENPOD_SPARSE_EVICTION_PARTIAL_SORT=1` default
- `0` uses the old full sort for comparison

Classification:

- The movement/high-alt CPU spike is primarily mid-voxel clipmap pump/generation.
- Request prep and surface staging remain secondary but material.
- Trim/replacement scans are visible during movement; they were not the fixed-camera blocker.

Key rows, all with background split enabled at `0.375`:

| Scenario | Frame | Cap | CPU update ms | Request | Gen | Clip | Clip pump | Missing voxel | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict | `380` | `0` | `38.34` | `31.82` | `5.64` | `0.86` | `0.20` | `0` | `16.24` | `0/0` | baseline |
| fixed strict | `380` | `8` | `37.30` | `30.95` | `5.45` | `0.88` | `0.20` | `0` | `16.00` | `0/0` | no fixed regression |
| walk strict | `480` | `0` | `63.02` | `24.83` | `11.32` | `23.39` | `12.74` | `103` | `20.65` | `0/0` | deterministic walk baseline |
| walk strict | `480` | `8` | `41.72` | `23.36` | `9.75` | `5.51` | `4.54` | `2716` | `21.97` | `0/0` | movement candidate; backlog debt grew |
| walk bounded64 | `480` | `0` | `60.80` | `30.56` | `12.85` | `13.92` | `12.97` | `48` | `14.26` | `0/0` | bounded comparison |
| walk bounded64 | `480` | `8` | `66.51` | `33.83` | `10.03` | `19.09` | `8.19` | `2390` | `14.48` | `0/0` | bounded comparison not improved |
| high-alt strict | `380` | `0` | `100.60` | `18.97` | `10.90` | `70.73` | `64.86` | `297` | `25.03` | `0/0` | baseline |
| high-alt strict | `380` | `8 guarded` | `96.66` | `14.54` | `14.30` | `67.80` | `58.93` | `278` | `20.36` | `0/0` | cap requested but inactive; visual-safe |

Visual result:

- fixed and strict-walk cap rows looked plausible in the contact sheet
- unguarded high-alt cap `8` and `16` showed obvious white terrain/shore regions and were rejected
- guarded high-alt cap `8` matched the baseline visual instead of the rejected white-terrain rows

Decision:

- Keep `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS=0` by default.
- Cap `8` is a movement-only default-off candidate: strict deterministic walk CPU update improved `63.02 -> 41.72 ms` with `miss/unsafe=0/0`.
- Do not make it a playable-mode default yet; it increased missing interested voxel bricks `103 -> 2716`.
- High-alt remains a separate CPU bottleneck and needs a different backlog-aware/incremental solution.
- Bounded promotion remains default-off and remains not the performance culprit.

## Movement Sparse CPU Update Follow-Up

Latest audit artifacts:

- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_summary.csv`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_table.md`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/contact_sheet.png`

The cap result above has been superseded. It remains useful diagnostic evidence, but it is not the accepted optimization because the backlog/coverage debt is too large.

Accepted default-off optimization:

- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0` by default.
- When enabled, broad terrain-surface prefetch is skipped only after public ownership is already clean.
- It does not disable screen-critical repair, hidden-exact repair, mid-clipmap work, Far-SVO, or promotion policy.
- `PERF_SPARSE_CPU_DETAIL` now logs `terrainPrefetch=ms/rays/budget/seen/new/cleanThrottle`.

Key rows with background split enabled at `0.375`:

| Scenario | Frame | Clean throttle | CPU update ms | Request | Gen | Clip | Terrain prefetch | Clip pump | Missing voxel | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict | `240` | `0` | `42.94` | `35.60` | `6.45` | `0.87` | n/a | `0.20` | `0` | `16.72` | `0/0` | baseline |
| fixed strict | `240` | `1` | `21.46` | `8.92` | `10.67` | `1.86` | `0.00 ms / 0 rays` | `0.30` | `0` | `11.48` | `0/0` | fixed CPU roughly halves |
| walk strict | `480` | `0` | `70.24` | `33.72` | `14.01` | `18.71` | `13.19 ms / 627 rays` | `17.85` | `86` | `13.73` | `0/0` | matched walk baseline |
| walk strict | `480` | `1` | `58.49` | `22.10` | `13.55` | `19.28` | `0.00 ms / 0 rays` | `18.10` | `115` | `13.01` | `0/0` | useful, still above target |
| walk bounded64 | `480` | `1` | `51.83` | `21.13` | `20.89` | `6.07` | `0.00 ms / 0 rays` | `4.56` | `0` | `16.25` | `0/0` | bounded benefits but stays default-off |
| high-alt strict | `240` | `1` | `58.27` | `7.91` | `8.03` | `42.33` | `0.00 ms / 0 rays` | `37.56` | `450` | `17.24` | `0/0` | no improvement; clipmap dominated |

Current decision:

- Keep the clean prefetch throttle default-off; it is useful but not a playable-mode CPU fix by itself.
- Keep `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS=0` by default; cap `8` plus prefetch throttle was rejected after leaving `2809` missing interested voxel bricks and mid coverage around `0.70`.
- Non-capture validation showed capture is not the CPU cause; clipmap catch-up can still dominate at `102.28 ms`.
- Next CPU pass should target incremental/backlog-aware mid-clipmap update or async/budgeted clipmap generation.
- Bounded promotion remains default-off and remains not the performance culprit.

## Overnight Playability Ladder - 2026-06-03

Audit:

```text
overnight_playability_ladder_20260603
```

Artifacts:

```text
build\captures\overnight_playability_ladder_20260603\overnight_summary.csv
build\captures\overnight_playability_ladder_20260603\overnight_table.md
build\captures\overnight_playability_ladder_20260603\contact_sheet.png
```

Mid-clipmap path summary:

- interest is built by `SparseClipmapTileCache::UpdateInterest` / `UpdateVoxelInterest`
- missing bricks are discovered by comparing current interested height/voxel bricks to resident/generated brick maps
- brick generation is synchronous in `PumpGeneration`, `PumpVoxelGenerationForRing`, `GenerateTile`, and `GenerateVoxelBrick`
- pump order is interest-score/ring ordered with some feedback front-insertion, not a full screen-critical proof
- missing/deferred voxel bricks fall back to lower ownership paths, so a cap can preserve `miss=0` while still producing visual terrain/shore regressions
- high-alt stays hard because its large footprint leaves hundreds of interested voxel bricks missing and keeps safe budget guards closed

New logs/counters:

- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` under `VENPOD_SPARSE_CPU_DETAIL=1`
- records backlog-aware active state, effective pump budget, budget hit, backlog height/voxel counts, oldest/max backlog age, pruned stale entries, visible-critical/noncritical missing voxel bricks, missing height/voxel counts, and mid coverage

Implemented candidate:

- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=0` default
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MS=4.0` when enabled
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MIN_COVERAGE_PCT=95`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MAX_CRITICAL_MISSING=128`
- budget is disabled before startup/public open, during coverage emergencies, below the coverage threshold, or above the critical-missing threshold
- backlog entries are deduped/carried while still current-interest and pruned only when stale or resident
- `GenerateVoxelBrick` also reserves temporary column-cache capacity for its existing 5x5 coarse sampling path; this is behavior-preserving allocation reduction

Key rows, all with background split `0.375` and clean prefetch throttle:

| Scenario | Frame | Candidate | CPU update ms | Request | Gen | Clip | Surface stage | Trim | Missing voxel | Backlog voxel | Max backlog age | Mid coverage | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict baseline | `240` | `0` | `24.01` | `11.77` | `11.34` | `0.90` | `4.94` | `0.00` | `0` | n/a | n/a | `100` | `11.26` | `0/0` | baseline |
| fixed strict backlog4 | `240` | `1` | `16.05` | `7.94` | `7.34` | `0.77` | `3.31` | `0.00` | `0` | `0` | `0` | `100` | `11.61` | `0/0` | safe fixed improvement |
| walk strict baseline | `480` | `0` | `90.67` | `23.63` | `24.15` | `37.76` | `21.45` | `5.13` | `91` | n/a | n/a | `98` | `15.60` | `0/0` | movement baseline |
| walk strict backlog4 | `480` | `1` | `43.23` | `16.36` | `16.52` | `7.28` | `14.64` | `3.07` | `415` | `415` | `117` | `95` | `9.25` | `0/0` | best short walk row |
| walk strict backlog4 long | `600` | `1` | `79.84` | `20.12` | `10.33` | `45.80` | `18.29` | `3.59` | `494` | `494` | `262` | `94` | `20.95` | `0/0` | catch-up returns |
| walk bounded64 baseline | `480` | `0` | `32.96` | `16.83` | `11.70` | `1.33` | `7.29` | `3.10` | `0` | n/a | n/a | `100` | `26.16` | `0/0` | comparison |
| walk bounded64 backlog4 | `480` | `1` | `46.75` | `12.43` | `10.68` | `20.89` | `9.96` | `2.75` | `33` | `33` | `18` | `99` | `24.67` | `0/0` | not a bounded win |
| high-alt strict baseline | `240` | `0` | `71.12` | `11.94` | `9.24` | `49.94` | `5.63` | `0.00` | `592` | n/a | n/a | `93` | `26.97` | `0/0` | stress camera |
| high-alt strict backlog4 | `240` | `1` | `52.83` | `6.72` | `5.63` | `40.48` | `3.92` | `0.00` | `638` | `638` | `164` | `92` | `23.50` | `0/0` | guard disables budget |
| noncapture walk baseline | `480` | `0` | `49.63` | `15.98` | `15.88` | `14.70` | `12.70` | `3.07` | `86` | n/a | n/a | `98` | `23.73` | `0/0` | no capture |
| noncapture walk backlog4 | `480` | `1` | `51.32` | `16.15` | `15.18` | `16.69` | `12.45` | `3.30` | `451` | `451` | `113` | `95` | `26.85` | `0/0` | no capture candidate not improved |

Decision:

- The scheduler candidate is a useful default-off diagnostic, not a playable-mode fix yet.
- It reduced the short walk row `90.67 -> 43.23 ms`, but the frame-600 row returned to `79.84 ms` and non-capture did not improve.
- Fixed-camera split plus clean throttle improved to `16.05 ms` sparse CPU in the sampled row.
- High-alt remains clipmap dominated; the guard prevented unsafe throttling, and a relaxed diagnostic produced poorer coverage.
- The refreshed contact sheet shows no obvious new fixed/walk holes; high-alt still has the older bright shoreline/terrain artifact.
- Combined playable mode was not promoted because long-walk/high-alt/non-capture safety did not pass.
- Background split, clean prefetch throttle, backlog-aware pump, and bounded promotion remain default-off.
- Next CPU work should be mid-clipmap generation architecture: async/noncritical generation or incremental interest/scroll reuse, followed by request/surface staging after clip catch-up is stable.

## Mid-Clipmap Drain/ReUse Pass - 2026-06-03

Audit:

```text
mid_clipmap_drain_and_reuse_20260603
```

Artifacts:

```text
build\captures\mid_clipmap_drain_and_reuse_20260603\mid_clipmap_drain_summary.csv
build\captures\mid_clipmap_drain_and_reuse_20260603\mid_clipmap_drain_table.md
build\captures\mid_clipmap_drain_and_reuse_20260603\contact_sheet.png
```

Final code change:

- added `VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS=0` default-off
- expanded `PERF_SPARSE_MID_CLIPMAP_BACKLOG` with budget reason, interest churn, resident/reused interest, backlog enqueue/carry/pump/skip counts, age buckets, per-ring generation, and average/max `GenerateVoxelBrick` ms
- no optimization candidate was kept; an age-priority queue-order prototype was rejected after it failed to validate cleanly and did not solve high-alt coverage debt

Classification:

- primary root: fallback contract / streaming architecture
- generation is the immediate cost mechanism, but moving or capping generation is unsafe until fallback-validity is explicit

Key evidence:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Backlog | Max age | Critical/noncritical | Coverage | Budget reason | Generated voxel | Avg/max gen ms | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict public | `380` | `17.49` | `1.79` | `0.61` | `0` | `0` | `0` | `0/0` | `100` | `4` | `0` | `0/0` | fixed path is not the blocker |
| walk fixed-dt | `600` | `38.89` | `6.71` | `5.69` | `419` | `419` | `218` | `13/406` | `95` | `0` | `4` | `1.149/2.507` | deterministic replay keeps budget active |
| walk realtime | `600` | `56.95` | `22.51` | `21.55` | `535` | `535` | `112` | `19/516` | `94` | `2` | `24` | `0.829/1.925` | coverage guard forces catch-up |
| high-alt strict | `240` | `50.38` | `36.24` | `30.26` | `1858` | `1858` | `25` | `1100/758` | `80` | `2` | `24` | `1.121/1.884` | high-alt footprint is not safely deferrable |
| noncapture walk | `600` | `44.02` | `7.05` | `6.08` | `435` | `435` | `211` | `0/435` | `95` | `0` | `5` | `1.005/1.744` | request/surface stage dominate this sample |

Decision:

- The high-alt row is the decisive stop condition: `1858` missing current-interest bricks, `80%` coverage, and `1100` critical missing bricks mean queue-order or cap tuning would hide required work.
- The realtime walk row shows the same mechanism at a smaller scale when coverage slips from `95` to `94` and the guard correctly disables the time budget.
- The fallback/lower-owner contract is not strong enough to defer broad missing mid-clipmap bricks without visual risk.
- The old blunt pump cap remains rejected.
- Next safe pass should design async/noncritical mid-clipmap generation with fallback-validity classification and staged upload accounting.

## Async Mid-Clipmap Fallback Validity Pass - 2026-06-03

Audit:

```text
async_mid_clipmap_fallback_validity_20260603
```

Artifacts:

```text
build\captures\async_mid_clipmap_fallback_validity_20260603\async_mid_fallback_summary.csv
build\captures\async_mid_clipmap_fallback_validity_20260603\async_mid_fallback_table.md
build\captures\async_mid_clipmap_fallback_validity_20260603\contact_sheet.png
```

Default-off changes:

- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0`
- `PERF_SPARSE_MID_CLIPMAP_FALLBACK` logs fallback-valid/invalid/unknown counts, high-alt counts, finer/coarser availability, rejection reasons, async placeholders, and upload placeholders
- async generation was not implemented because the classifier found no proven fallback-valid missing bricks in the debt rows

Fallback path findings:

- `PS_Raymarch` tries resident finer mid-voxel rings first, then uses coarser parents only in restricted low-alt/ray-angle cases.
- High-alt views explicitly reject coarser mid-voxel parents, so coarser parent residency is not a safe CPU proof for high-alt deferral.
- CPU can currently prove only complete finer-ring coverage for a missing brick.
- CPU cannot currently prove per-brick Far-SVO, water, sky, or shoreline validity; those remain shader/per-ray decisions.
- Unknown fallback is treated as critical by design.

Classifier rows, all with background split `0.375`, clean prefetch throttle, backlog-aware pump, and drain/reuse diagnostics enabled:

| Scenario | Frame | CPU update ms | Clip ms | Missing voxel | Fallback valid | Fallback invalid | Fallback unknown | Async eligible | Sync required | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `13.91` | `0.81` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.42` | `0/0` | no missing mid-voxel debt |
| walk fixed-dt | `600` | `37.72` | `6.55` | `269` | `0` | `61` | `208` | `0` | `269` | `97` | `0` | `4.42` | `0/0` | backlog debt remains |
| walk realtime | `600` | `37.26` | `5.80` | `308` | `0` | `119` | `189` | `0` | `308` | `96` | `0` | `10.20` | `0/0` | no proven safe async subset |
| high-alt | `240` | `116.40` | `101.65` | `3354` | `0` | `2183` | `1171` | `0` | `3354` | `62` | `2` | `21.68` | `0/0` | broad footprint is fallback-blocked |
| noncapture walk | `402` | `61.47` | `1.13` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `1` | `32.84` | `0/0` | direct noncapture run reached only a usable frame-402 row |

Decision:

- Classification result: missing-debt rows are fallback-invalid/unknown dominated, with `0` fallback-valid and `0` async-eligible bricks.
- High-alt is decisive: `3354` missing current-interest voxel bricks, `2183` invalid, `1171` unknown, `0` valid, and coverage `62`.
- Async noncritical generation is blocked until the fallback contract can produce a nonzero trustworthy valid subset.
- Staged upload accounting is present only as zero placeholders because no worker completions are produced.
- Contact sheet verdict: fixed and walk rows are comparable to prior state; high-alt still shows the existing broad artifact/coverage problem.
- All defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, async candidate, and bounded repair are default-off.
- The rejected blunt pump cap and removed age-priority prototype remain rejected.

Next safe pass:

- design a fallback contract that can prove per-brick/per-region lower ownership: finer mid coverage, Far-SVO domain/material validity, water/shoreline safety, and public readiness state
- add a worker-generated mid-brick payload/apply path only after such proof exists, with dedupe, generation stamps, safe frame-boundary apply, and upload/completion budgets
- do not move generation async by treating unknown fallback as safe

## Mid-Clipmap Fallback Contract Ownership Pass - 2026-06-03

Audit:

```text
mid_clipmap_fallback_contract_ownership_20260603
```

Artifacts:

```text
build\captures\mid_clipmap_fallback_contract_ownership_20260603\fallback_contract_summary.csv
build\captures\mid_clipmap_fallback_contract_ownership_20260603\fallback_contract_table.md
build\captures\mid_clipmap_fallback_contract_ownership_20260603\contact_sheet.png
```

New default-off knobs and logs:

- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=0`
- `PERF_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT`

Fallback contract map:

- `PS_Raymarch` samples the preferred resident mid-voxel brick, then resident finer rings, then coarser parents only when the low-alt/ray-angle guard allows it.
- High-alt views reject coarser mid-voxel parent fallback; that rejection is a safety/visual guard, not just a missing CPU optimization.
- CPU can currently prove complete finer-ring coverage.
- CPU can now diagnose Far-SVO domain coverage for a missing mid brick, but domain coverage is not material/occupancy/shoreline ownership proof.
- Far-SVO material validity, deterministic water, sky/no-hit, and shoreline/mixed-cell safety are still shader/per-ray or metadata-missing decisions.
- Unknown fallback remains critical and async-ineligible.

Chosen proof candidate:

- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=1` was implemented as a default-off diagnostic.
- It checks whether the missing mid-brick bounds are inside a ready Far-SVO page/domain with complete page coverage.
- It intentionally does not mark the brick valid because the CPU still lacks per-brick Far-SVO material/occupancy and water/shoreline safety metadata.

Classifier rows, all with background split `0.375`, clean prefetch throttle, backlog-aware pump, fallback classifier, fallback contract diagnostics, and Far-SVO domain proof enabled:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Valid | Invalid | Unknown | Far-SVO domain valid | Far-SVO material unknown | Async eligible | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.88` | `1.11` | `0.47` | `0` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.57` | `0/0` | no debt |
| walk fixed-dt | `600` | `64.68` | `27.35` | `14.41` | `335` | `0` | `67` | `268` | `42` | `42` | `0` | `96` | `0` | `12.36` | `0/0` | domain subset remains material-unknown |
| walk realtime | `600` | `79.30` | `38.62` | `24.50` | `463` | `0` | `12` | `451` | `5` | `5` | `0` | `94` | `2` | `22.82` | `0/0` | coverage emergency |
| high-alt strict | `240` | `99.59` | `85.49` | `78.04` | `2773` | `0` | `1602` | `1171` | `433` | `433` | `0` | `69` | `2` | `30.32` | `0/0` | high-alt fallback-contract blocker |
| noncapture walk | `600` | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | `12.16` | n/a | direct run did not emit fallback contract detail |

High-alt diagnosis:

- The previous pass found `3354` high-alt missing bricks with `0` valid.
- This matched retry found `2773` missing bricks with `433` inside the Far-SVO domain, but all `433` remained `farSvoMaterialUnknown`.
- `2340` missing bricks were outside the CPU-proved Far-SVO domain; `2132` had coarser parents rejected by high-alt policy; `1602` were screen-critical; `1171` remained mixed-owner/shader-only unknown.
- Result: `0` fallback-valid, `0` async-eligible, and async remains blocked.

Decision:

- No async generation was implemented.
- No async plan-only queue was implemented because `asyncEligible` stayed `0`.
- The proof candidate is safe to keep default-off as diagnostics.
- Visual verdict: fixed and walk rows remain comparable to prior captures; high-alt still shows the known bright shoreline/terrain artifact.
- All defaults remain unchanged, including background split, clean throttle, backlog-aware pump, fallback classifier, async candidate, and bounded repair.
- The rejected blunt pump cap and removed age-priority prototype remain rejected.

Next safe pass:

- add lightweight fallback-owner feedback or CPU metadata for Far-SVO material/occupancy and shoreline/water safety
- use that proof to convert unknowns into valid or invalid before async generation
- if valid remains zero, reduce high-alt/current-interest footprint under an explicit ownership contract rather than deferring unknown bricks

## Autonomous Streaming Playability Engineering - 2026-06-03

Audit:

```text
autonomous_streaming_playability_engineering_20260603
```

Artifacts:

```text
build\captures\autonomous_streaming_playability_engineering_20260603\summary.csv
build\captures\autonomous_streaming_playability_engineering_20260603\table.md
build\captures\autonomous_streaming_playability_engineering_20260603\contact_sheet.png
```

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- `git diff --check` on touched source passed except existing LF-to-CRLF warnings
- known `rayDir` shadow warnings remain in full rebuilds

Code architecture review:

- Current-interest is built in `SparseClipmapTileCache::UpdateVoxelInterest`.
- That interest set is a broad cache/resident footprint, not a strict visible-this-frame set.
- High-alt mode adds wide terrain-line/fan candidates for multiple rings, which keeps `currentInterestVoxel` around `9216` and can produce thousands of missing current-interest bricks.
- `PumpGeneration` and `PumpVoxelGenerationForRing` still do synchronous main-thread voxel brick generation.
- Render feedback exists (`QueueVoxelRenderFeedbackCoord` fed from ownership stats), but sampled rows show it is narrow and often `accepted=0`; it is not a complete visible-critical owner map.
- Budget and coverage emergency logic currently use broad interest coverage. When broad coverage drops below the threshold, the pump budget is disabled and catch-up generation returns.
- Shader fallback is per-ray: preferred mid brick, resident finer rings, coarser parents only under low-alt/ray-angle guards, then broader background/Far-SVO/water/sky logic. High-alt rejects coarser mid parents.

Fresh baseline rows, all using background split `0.375`, clean prefetch throttle, CPU detail, backlog-aware pump, fallback classifier, fallback contract diagnostics, and Far-SVO domain proof diagnostics:

| Scenario | Frame | CPU update ms | Request ms | Gen ms | Clip ms | Pump ms | Surface stage ms | GPU ray ms | Missing voxel | Valid/Invalid/Unknown | Critical/Noncritical | Coverage | Budget reason | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.58` | `6.63` | `7.06` | `0.89` | `0.20` | `3.62` | `11.18` | `0` | `0/0/0` | `0/0` | `100` | `4` | not blocked |
| walk fixed-dt | `600` | `43.44` | `22.53` | `10.64` | `6.52` | `5.39` | `10.14` | `12.49` | `413` | `0/0/413` | `0/413` | `95` | `0` | request/surface/gen are now material buckets |
| walk realtime | `600` | `78.94` | `16.13` | `7.38` | `52.18` | `40.20` | `7.35` | `14.20` | `474` | `0/100/374` | `100/374` | `94` | `2` | broad coverage drop disables budget |
| high-alt strict | `240` | `99.59` | `7.62` | `6.46` | `85.49` | `78.04` | `3.46` | `30.32` | `2773` | `0/1602/1171` | `1602/1171` | `69` | `2` | reused prior matched retry; high-alt remains fallback-contract blocked |
| noncapture direct | `600` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0.82` | n/a | n/a | n/a | n/a | n/a | clean direct run, but no sparse detail emitted |

Rejected branch:

- Branch chosen first: over-broad current-interest / visible-critical split.
- Reason: realtime walk and high-alt rows show full-interest coverage emergency forcing catch-up even though some missing debt is likely cache/prefetch debt.
- Prototype: a temporary default-off ring-based visible-critical guard that used critical-ring coverage for the pump guard while preserving broad cache interest.
- Result on walk fixed-dt: CPU `43.44 -> 33.69`, but missing voxel debt grew `413 -> 897`.
- Result on walk realtime: CPU `78.94 -> 124.03`, clip `52.18 -> 91.46`, missing voxel `474 -> 1069`, parent-held visual failure `27 -> 1193`.
- Decision: removed. The prototype reduced one short row but was not safe or robust.

Why it failed:

- Ring membership is not the same as visible ownership.
- Letting broad cache debt accumulate can still invalidate parent-held/visual safety and force a worse catch-up path.
- The renderer needs sampled or metadata-backed owner relevance before it can separate visible-critical from prefetch.
- This is not a budget-tuning problem; the missing proof is ownership/relevance classification.

Final decision:

- No new optimization code remains from this pass.
- No new environment knob remains from this pass.
- All defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair are still default-off/default-neutral.
- The blunt pump cap remains rejected.
- The removed age-priority queue prototype remains rejected.
- Async generation remains blocked because the fallback-valid subset in debt rows is still zero.
- Combined playable mode was not tested because the only new candidate failed safety/performance validation.

Current bottleneck after this pass:

- Fixed split rows are near the desired CPU range.
- Walk fixed-dt is no longer primarily clipmap-bound; request prep, generation, surface staging, and trim are material buckets.
- Walk realtime and high-alt are still clipmap-pump blocked when broad coverage debt triggers safety catch-up.
- High-alt remains an ownership/fallback-contract blocker, not a queue-order problem.

Next implementation slice:

- Add lightweight sampled missing-preferred-mid feedback in the raymarch/ownership path.
- The feedback should report bounded tile/hash/reservoir counts for missing preferred mid bricks that are actually sampled by visible rays, plus the final owner that covered those pixels: finer mid, coarser mid, Far-SVO, water, sky, miss/unsafe, or unknown/mixed.
- This unlocks a real `visibleCriticalInterest` split: sampled invalid/unknown missing bricks remain critical, while unsampled cache-footprint bricks can become prefetch/backlog.
- If sampled feedback shows most high-alt missing bricks are truly sampled and invalid/unknown, the next fix is not async. It is Far-SVO/water/shoreline metadata or a smaller high-alt current-interest contract.
- Once visible-critical proof exists, add async noncritical/prefetch generation with generation stamps, dedupe, safe frame-boundary apply, and upload budgets.

Plain-English status:

- GPU is no longer the first fixed/walk blocker when the default-off background split is enabled.
- The renderer is still not playable because CPU streaming can still turn broad current-interest debt into synchronous catch-up.
- The next safe fix is not another cap. It is ownership-aware sampled visibility feedback so the engine can stop treating all desired cache bricks as equally frame-critical.

## Sampled Missing Mid Feedback and Visible-Critical Split - 2026-06-03

Audit:

```text
sampled_missing_mid_feedback_and_visible_critical_split_20260603
```

Artifacts:

```text
build\captures\sampled_missing_mid_feedback_and_visible_critical_split_20260603\summary.csv
build\captures\sampled_missing_mid_feedback_and_visible_critical_split_20260603\table.md
build\captures\sampled_missing_mid_feedback_and_visible_critical_split_20260603\contact_sheet.png
```

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- source whitespace check passed except existing LF-to-CRLF warnings

Feedback implementation:

- Added default-off `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=0`.
- First attempted bounded shader-side missing-mid hash buckets. That path hit the stop condition: a feedback-enabled capture timed out after `240s` with no useful log/capture output while `VENPOD.exe` was still consuming CPU. The shader/ABI changes were removed to avoid the previous runtime DXC/churn pattern.
- Final path is `feedbackMode=cpu_projected_bounds`: CPU enumerates missing voxel interest, projects each missing brick AABB through the current camera, and estimates sampled versus unsampled missing debt.
- Added `SparseClipmapTileCache::CollectMissingVoxelInterest`.
- Added logs:
  - `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK`
  - bounded `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET`
- Optional diagnostic knobs:
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_CPU_MAX_COORDS`
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_SCREEN_PAD`
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET_LOG_MAX`

Feedback table, with background split `0.375`, clean throttle, CPU detail, backlog-aware pump, fallback classifier, fallback contract diagnostics, Far-SVO domain proof, and missing-sample feedback enabled:

| Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU ray | Missing | Valid/Invalid/Unknown | Far-SVO domain/material unknown | Sampled approx | Unsampled approx | Sampled % | Coverage | Budget reason | Miss/unsafe | Feedback ms | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `27.67` | `10.77` | `15.97` | `0.92` | `0.20` | `13.34` | `0` | `0/0/0` | `0/0` | `0` | `0` | `0` | `100` | `4` | `0/0` | `0.41` | no debt |
| walk fixed-dt | `600` | `73.21` | `22.82` | `10.55` | `34.24` | `14.89` | `15.68` | `290` | `0/74/216` | `44/44` | `220` | `70` | `75` | `96` | `0` | `0/0` | `0.52` | mostly projected-visible; not safe to defer unknowns |
| walk realtime | `600` | `71.12` | `23.00` | `9.76` | `34.51` | `32.40` | `9.10` | `551` | `0/75/476` | `9/9` | `502` | `49` | `91` | `93` | `2` | `0/0` | `0.53` | coverage emergency; debt mostly visible |
| high-alt late | `400` | `57.58` | `8.20` | `6.98` | `42.39` | `35.12` | `21.16` | `467` | `0/257/210` | `40/40` | `14` | `453` | `2` | `94` | `2` | `0/0` | `0.62` | over-broad high-alt interest, but sampled subset unknown/invalid |
| noncapture walk | `600` | `122.01` | `67.22` | `34.92` | `12.17` | n/a | `50.90` | `449` | `0/18/431` | `4/4` | `379` | `70` | `84` | `95` | `0` | `0/0` | `0.77` | request/gen/surface dominate; missing still mostly visible |

Diagnosis:

- Fixed public has no missing-mid debt.
- Walk fixed-dt, realtime walk, and noncapture walk are mostly projected-visible (`75%`, `91%`, and `84%`). Those sampled/projected-visible bricks remain fallback-invalid or fallback-unknown, so they cannot be moved to prefetch/async safely.
- Late high-alt is the interesting over-broad case: only `14/467` missing bricks projected visible at frame `400`, while `453/467` looked like cache/prefetch debt. However, the visible subset still has no proven valid owner.
- The failed ring heuristic was disproven as too crude because it did not know sampled visibility or owner validity; realtime validation already showed the parent-held visual failure explosion.

Decision:

- `VISIBLE_CRITICAL_INTEREST_V2` was not implemented in this pass.
- Reason: the final feedback is post-hoc diagnostics. A correct V2 must run before pump budget selection, split sampled/likely-visible missing from cache/prefetch missing, and make pump order prioritize visible-critical work. A guard-only change would risk repeating the rejected ring heuristic.
- Async remains blocked because `fallbackValid=0` in all sampled debt rows.
- All defaults remain unchanged. Background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair remain default-off/default-neutral.
- Blunt pump cap and the removed age-priority prototype remain rejected.

Next implementation slice:

- Implement a pre-pump `visibleCriticalInterest` state using this CPU projection path or a cheaper projected-tile cache.
- Gate coverage emergency on visible-critical coverage, not broad cache coverage, but only after the pump can prioritize visible-critical missing before prefetch.
- Keep sampled/projected-visible unknown or invalid bricks critical.
- For walk/realtime, focus on fallback ownership proof or reducing actual sampled missing debt, since this feedback says most movement debt is visible.
- For high-alt, use visible-critical/cache split to stop unsampled cache debt from forcing synchronous catch-up, while preserving sampled unknown/invalid safety.

## Streaming Playability Campaign Until Valid Candidate - 2026-06-04

Audit:

```text
streaming_playability_campaign_until_valid_candidate_20260604
```

Artifacts:

```text
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\summary.csv
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\table.md
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\playable_candidate_table.md
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\contact_sheet.png
```

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Engineering cycles attempted:

1. Branch H: high-alt visible-critical/cache split before pump/coverage.
2. Branch W: shared voxel column cache for mid-clipmap generation.
3. Secondary request/surface probes: screen-critical request reuse and surface upload interval.
4. Combined playable stack validation.
5. Direct noncapture logging probe.

New default-off code:

- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=0`
- `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP`
- extra `columnCache=active/entries/hHit/hMiss/rHit/rMiss` fields on `PERF_SPARSE_MID_CLIPMAP_BACKLOG`

Branch H result:

- Implemented a high-alt-only pre-pump visible-critical/cache split using the CPU projected missing-mid feedback.
- Missing projected-visible unknown/invalid bricks stay critical.
- Unsampled/cache bricks stay tracked but stop forcing a high-alt coverage emergency.
- Pump order prioritizes projected-visible missing bricks through `SparseClipmapTileCache::PrioritizeVoxelGenerationCoords`.

High-alt baseline to Branch H:

| Row | Frame | CPU | Req | Gen | Clip | GPU ray | Missing | Sampled/unsampled | Coverage | Budget reason | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| high-alt baseline | `400` | `44.95` | `9.10` | `5.41` | `30.42` | `21.39` | `2384` | `194/2190` | `73` | `2` | `0/0` | over-broad cache debt |
| high-alt prepump split | `400` | `25.23` | `7.07` | `5.53` | `12.62` | `16.31` | `2074` | `66/2008` | `99` visible / `77` cache | `0` | `0/0` | accepted default-off high-alt win |

Visual verdict:

- The campaign contact sheet shows no new obvious holes or white-terrain regression from Branch H.
- The known high-alt bright shoreline/terrain artifact remains.

Branch W result:

- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1` reduced one fixed-dt walk row (`CPU 41.08 -> 29.89`, `clip 17.51 -> 6.66`) but did not hold in realtime (`CPU 31.14 baseline -> 48.84`, `clip 23.54`).
- Keep it default-off as experimental/diagnostic. It is not part of the playable candidate stack.

Secondary probes:

- Screen-critical request reuse env did not materially reduce request work and was rejected (`realtime CPU 42.74`, request `13.89`, clip `17.58`, budget reason `3`).
- Surface upload interval reduced the selected-frame clip cost but moved work into raw/stage spikes (`rawMs 95.22` at frame `600`). Dirty-stage logs indicate full metadata work for small dirty/removed sets is the real staging cost, not upload bandwidth.

Combined playable stack tested:

```text
VENPOD_RENDER_QUALITY=playable
VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1
VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375
VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1
VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=1
VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1
```

| Scenario | Frame | CPU | Req | Gen | Clip | GPU ray | Raw/body | Missing | Sampled/unsampled | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed playable | `380` | `12.06` | `6.56` | `4.72` | `0.78` | `8.45` | `31.47/n/a` | `0` | `0/0` | not 60, but fixed GPU/CPU stack is close enough to expose wrapper/remaining overhead |
| realtime walk playable | `600` | `67.72` | `13.47` | `9.02` | `42.16` | `6.24` | `92.78/100.04` | `424` | `349/75` | not playable; sampled-visible clip debt returned |
| high-alt playable | `400` | `35.83` | `8.97` | `6.15` | `20.71` | `11.14` | `69.13/n/a` | `2897` | `119/2778` | GPU improved, high-alt CPU/cache debt remains |

Noncapture result:

- A direct `build\bin\VENPOD.exe` probe with forced `VENPOD_LOG_FILE` detached and left live `VENPOD.exe` processes without producing the requested log. The probes were stopped.
- This is a runtime-harness blocker for validating true noncapture 60 FPS. Do not claim 60 FPS from the capture rows.

Decision:

- Completion state: strong default-off candidate plus exact remaining blocker table, not validated 60 FPS.
- Branch H is safe to keep as a default-off high-alt candidate.
- Branch W and secondary request/surface interval probes are not final candidates.
- Async remains blocked for movement because sampled/projected-visible missing debt is still fallback-invalid/unknown.
- Background split, clean throttle, backlog-aware pump, visible-critical prepump, shared column cache, fallback diagnostics/proofs, async, and bounded repair all remain default-off/default-neutral.
- Blunt pump cap, age-priority queue ordering, and the failed ring-only visible-critical heuristic remain rejected.

Remaining blockers:

- Realtime/walk: sampled-visible mid-clipmap debt still drives clip/pump spikes; deferring it would weaken ownership because `fallbackValid=0`.
- High-alt: prepump split reduces over-broad cache catch-up, but cache backlog and sampled unknowns remain; the known shoreline artifact still needs later correctness work.
- Surface staging: dirty surface metadata still does full metadata work for small dirty/removed sets; the safe next CPU slice is incremental metadata patch/append with retire accounting.
- Request/gen: movement still spends about `13-22 ms` request and `7-10 ms` generation in representative rows.
- Tooling: `perf_noncapture_smoke.ps1` now provides noncapture log-only validation before any true 60 FPS claim.

Plain-English status:

- The renderer now has a stronger default-off candidate stack: background split plus clean throttle plus high-alt visible-critical prepump split.
- It is not a 60 FPS candidate. First noncapture candidate rows are fixed frame `380` raw `54.67 ms`, walk realtime frame `600` raw `246.81 ms`, and high-alt frame `400` raw `91.11 ms`.
- The walk row changes the immediate CPU priority: request/gen/clip/trim were `49.52/26.35/7.94/6.76 ms`, surface extract/stage were `21.86/55.24 ms`, and GPU ray was only `5.70 ms`. The next best work is not another clip cap; it is a surface staging/request-generation fix that preserves ownership.

## Streaming Playability Surface/Pool Cycle - 2026-06-04

Artifacts:

```text
build\captures\noncapture_playability_incremental_surface_20260604\table.md
build\captures\noncapture_playability_incremental_surface_statsbatch_20260604\table.md
build\captures\noncapture_playability_large_pool_65536_walk_20260604\table.md
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\summary.csv
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\table.md
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\playable_candidate_table.md
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\contact_sheet.png
```

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Accepted fixes:

- Added default-off `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=0`.
- Added behavior-preserving `SparseSurfaceRangeAllocator` stats refresh batching during dirty surface staging.
- Updated `perf_noncapture_smoke.ps1` to enable the new surface metadata candidate and keep producing parseable fixed/walk/high-alt rows.

Measured progression on realtime walk frame `600`:

| Row | Raw ms | CPU sparse ms | Request | Gen | Clip | Surface extract | Surface stage | GPU ray | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| first noncapture stack | `246.81` | `90.60` | `49.52` | `26.35` | `7.94` | `21.86` | `55.24` | `5.70` | `0/0` | full surface staging/metadata dominated |
| incremental metadata | `76.55` | `59.26` | `24.94` | `12.30` | `18.54` | `9.71` | `23.36` | `18.57` | `0/0` | partial surface win |
| metadata + allocator stats batch | `117.90` | `48.05` | `26.41` | `11.72` | `6.34` | `10.93` | `4.11` | `19.63` | `0/0` | stage spike fixed; raw still bad |
| large pool/page table | `99.42` | `40.64` | `19.95` | `13.77` | `6.89` | `9.84` | `2.86` | `14.29` | `0/0` | replacement scans removed |

Consolidated current opt-in stack, including large sparse pool/page table:

| Scenario | Frame | Raw ms | CPU sparse ms | Request | Gen | Clip | Pump | GPU ray | Missing | Sampled/unsampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `381` | `28.05` | `18.36` | `12.68` | `4.84` | `0.84` | `0.00` | `10.54` | `0` | `0/0` | `0/0` |
| walk realtime | `600` | `109.56` | `74.30` | `20.80` | `16.48` | `36.98` | `34.69` | `18.55` | `436` | `366/70` | `0/0` |
| high-alt | `400` | `54.64` | `28.40` | `8.31` | `6.71` | `13.37` | `5.73` | `14.42` | `1882` | `45/1837` | `0/0` |

Rejected/not accepted:

- request resident fast-path lookup patch was tried and removed; no reliable request win
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1` retest did not improve realtime walk enough to keep in the candidate stack
- `VENPOD_VSYNC=0` did not make walk playable

Decision:

- This cycle is a safe partial candidate, not a 60 FPS candidate.
- Surface metadata/staging is materially better and should stay as default-off candidate code.
- The next measured blocker is realtime walk mid-clipmap pump/generation returning to `clipMs=36.98` / `pumpMs=34.69` in the consolidated run. Because `366/436` missing bricks are projected sampled and fallback-valid remains zero, the next fix cannot defer unknown sampled bricks. It must inspect pump input/order and generation/reuse for those sampled-visible bricks.

## Streaming Playability Request/Stats Cycle - 2026-06-04

Audit:

```text
build\captures\noncapture_request_detail_20260604
build\captures\noncapture_request_detail_large_pool_20260604
build\captures\noncapture_single_flush_walk_20260604
build\captures\noncapture_bounded64_single_flush_walk_20260604
build\captures\noncapture_critical_reuse_walk_20260604
build\captures\noncapture_candidate_stack_accepted_20260604
build\captures\streaming_playability_campaign_until_valid_candidate_20260604\table.md
```

Accepted additions:

- `PERF_SPARSE_REQUEST_DETAIL` splits request into hierarchy, terrain-critical, terrain surface prefetch, stats flush, and remaining `otherMs`.
- `VENPOD_SPARSE_STATS_SINGLE_FLUSH=0` is default-off. When enabled, request/generation/post-upload phases rely on the deferred stats flush instead of forcing a second explicit `FlushStats()` scan.
- `perf_noncapture_smoke.ps1` now uses the large sparse pool/page table in the candidate env and supports `-Bounded64` and `-CriticalReuse` comparison switches. The default harness clears the rejected critical-reuse env.

Rejected in this cycle:

- `bounded_repair`/bound `64` as a performance stack component. It promoted surface but drove walk frame-600 clip/pump to about `71/70 ms`.
- `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_MAX_SPEED=64` for walk. It reduced request work but starved critical requests and caused clip catch-up.
- targeted coverage catch-up; it did not drain frame-600 debt and was removed.

Latest accepted noncapture candidate stack:

```text
VENPOD_RENDER_QUALITY=playable
VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1
VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375
VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1
VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=1
VENPOD_SPARSE_STATS_SINGLE_FLUSH=1
VENPOD_SPARSE_CPU_DETAIL=1
VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=1
VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS=1
VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=1
VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=1
VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=1
VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=1
VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1
VENPOD_SPARSE_MAX_PAGES=65536
VENPOD_SPARSE_PAGE_TABLE=131072
```

Rows:

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Surface extract/stage | GPU ray ms | Missing | Sampled/unsampled | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `375` | `21.39` | `16.27` | `10.93` | `4.55` | `0.79` | n/a | `6.93` | `0` | `0/0` | `0/0` | close, still over `16.67 ms` |
| walk realtime | `600` | `112.09` | `64.87` | `33.39` | `23.55` | `7.87` | `19.46/5.76` | `18.08` | `447` | `378/69` | `0/0` | not playable; request/gen/surface/GPU dominate |
| high-alt | `399` | `53.11` | `29.52` | `7.92` | `5.49` | `16.10` | n/a | `14.49` | `1874` | `44/1830` | `0/0` | visible-critical prepump helps, still not 60 |

Decision:

- This is not a validated 60 FPS candidate.
- The accepted stack is stronger than the earlier campaign stack, but realtime walk is still far over budget.
- The next measured branch is not another deferral heuristic. It is to split request `otherMs` and inspect hidden-exact/surface extraction coupling, because walk frame `600` had `requestMs=33.39`, `otherMs=22.25`, hidden-exact accepted `53` coords, and `surfaceExtractMs=19.46`.
- Do not promote any defaults. Background split, clean throttle, surface incremental metadata, stats single flush, backlog pump, visible-critical prepump, fallback diagnostics/proofs, async, and bounded repair remain default-off/default-neutral.

## Streaming Playability Real Fix Campaign Resume - 2026-06-04

Persistent campaign goal:

- `streaming_playability_real_fix_campaign_20260604`
- local continuation handoff: `handoff.md`

Latest accepted code changes after the request/stats cycle:

- `src/main_launcher.cpp`
  - fixed the public-open-frame latch so `sparseStartupPublicRenderOpenedFrame` does not refresh every frame after public render opens
  - added default-off direct-footprint diagnostic config/logging
- `src/Simulation/SparseClipmap.h/.cpp`
  - added default-off direct-footprint diagnostic plumbing
  - changed voxel-slot eviction to preserve voxel payload vector capacity while resetting metadata; this is behavior-preserving and reduces allocation churn
- `perf_noncapture_smoke.ps1`
  - added direct-footprint diagnostic switch

Build/test after those changes:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Latest focused validation:

- `build/captures/noncapture_voxel_slot_reuse_candidate_all_20260604/table.md`

| Scenario | Frame | Raw ms | CPU sparse ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU ray | Missing | Sampled/unsampled | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `23.96` | `21.41` | `4.15` | `16.35` | `0.90` | `0.00` | `19.96/2.38` | `6.16` | `0` | `0/0` | `0/0` | close but not 60 |
| walk realtime | `900` | `77.86` | `46.70` | `12.39` | `3.15` | `31.12` | `17.11` | `6.39/3.19` | `15.52` | `436` | `366/70` | `0/0` | clip/parent-held catch-up returned |
| high-alt | `401` | `48.99` | `24.63` | `5.62` | `5.27` | `13.73` | `5.48` | n/a | `11.87` | `1813` | `41/1772` | `0/0` | improved but not 60 |

Rejected latest probes:

- post-open surface extraction cap at `4 ms`: moved debt and worsened walk CPU/clip
- `VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS=1`: diagnostic only, unstable across matrix
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`: did not solve recurring walk spikes
- larger mid voxel pool / lowered interest-percent tuning: missing dropped but `parentHeld` / LOD visual ownership failure exploded, causing broad catch-up

Updated decision:

- The next blocker is parent-held LOD ownership/catch-up, not just missing-brick backlog, request `otherMs`, or pool capacity.
- The larger-pool experiments prove that eliminating missing bricks is insufficient when the shader still has to hold a coarser parent ring where a preferred child should own.
- Do not continue with pool-size tuning, queue caps, or unknown-fallback deferral.

Next implementation branch:

1. Map `RAY_DIAGNOSTIC_MID_PARENT_HELD` / `lodParentHeld` from `PS_Raymarch.hlsl` back to preferred child clipmap coordinates.
2. Add bounded default-off parent-held ownership feedback or a CPU projection approximation that avoids heavy shader hash-bucket compile churn.
3. Prioritize those preferred child bricks in the pump/generation path instead of broad catch-up.
4. Validate noncapture fixed, realtime walk, and high-alt.

## Research Questions

These are the questions worth discussing with researchers:

1. What invariant should a streaming sparse voxel renderer enforce before presenting a public frame?

2. How should exact surface, mid voxel, Far-SVO, water, and sky arbitrate ownership when exact data is absent but deterministic terrain/water truth is known?

3. Should lower LOD be allowed to own pixels inside the exact band after startup, and if so what proof makes it legitimate?

4. How should water/terrain parity be resolved at coarse Far-SVO mixed cells where water-plane T is only 1-2 units before terrain T?

5. Is the Far-SVO representation too coarse/mixed for shoreline ownership, requiring a water-aware Far-SVO/material encoding, or is the current resolver simply rejecting valid water?

6. Should public readiness be camera-footprint based instead of global cache/readiness based?

7. What is the cheapest safe alternative to per-pixel Far-SVO recovery in the miss path?

8. Is a zero-feedback hidden-exact proof the right startup/public contract, or should it be replaced by a bounded foreground contract that can converge within a frame budget?

## Streaming Playability Campaign Continuation - 2026-06-04

This continuation kept the active campaign goal alive and tried the next safe measured branches instead of stopping at diagnostics.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_table.md`
- rejected summaries: `rejected_generation_cap8_summary.csv`, `rejected_critical_reuse_tol2_summary.csv`, `rejected_surface_skip_sort_summary.csv`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Branches:

- CPU projected parent-held feedback was tried as a default-off diagnostic. It accepted almost no useful coords and did not fix walk f600.
- A general generation cap was rejected and removed. It deferred necessary work and worsened walk f600.
- Screen-critical reuse forward tolerance was rejected and removed. It activated reuse but created worse coverage/catch-up debt.
- Surface timed skip-sort was rejected and removed. It did not reduce the surface bucket.
- Timed surface extraction now skips one unused global queue sort before the real per-class extraction pass. This is behavior-preserving cleanup, not a playable candidate.

Latest combined matrix:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | GPU | Missing | Sampled/unsampled | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `103.13` | `43.44` | `19.55` | `13.98` | `9.90` | `7.50` | `13.11` | `28` | `24/4` | `0/0` | not stable/60 |
| walk realtime | `600` | `144.49` | `115.15` | `26.16` | `23.74` | `65.22` | `47.49` | `20.94` | `445` | `376/69` | `0/0` | sampled-visible fallback-unknown debt |
| high-alt | `378` | `45.14` | `23.26` | `5.07` | `5.48` | `12.70` | `4.96` | `10.71` | `1833` | `33/1800` | `0/0` | high-alt prepump split is useful |

Conclusion:

- VENPOD still has no validated 60 FPS candidate.
- High-alt cache debt is over-broad and has a default-off visible-critical/cache split that helps.
- Walk/realtime debt is mostly sampled/projected-visible and fallback-unknown or invalid. It cannot be safely deferred or sent async under the current ownership contract.
- Further local queue caps, generation caps, and reuse tolerance are rejected. The next work must build the ownership-aware streaming pipeline: visible-critical sync/guarded work, async cache/prefetch generation, frame-boundary apply/upload, budgeted surface extraction, generation/edit stamps, and owner metadata or feedback for movement fallback.

## Current One-Line State

VENPOD is no longer blocked by the old Far-SVO domain gap, startup now waits for same-camera shader-unsafe exact-contract proof by default, and the default-off lower-res background split has strong GPU reductions; CPU work now has default-off sampled/projected missing-mid feedback showing that walk debt is mostly visible and fallback-unknown while late high-alt has broad unsampled cache debt, so the next real fix is a pre-pump visible-critical/cache-interest state plus ownership proof, not another cap.
### Streaming Playability Real Fix Campaign - 2026-06-04 Active State

Accepted cleanup in this continuation:

- `src/Simulation/SparseVoxelWorld.cpp`
- `src/Simulation/SparseVoxelWorld.h`
- `src/main_launcher.cpp`
- Behavior-preserving surface extraction queue hygiene:
  - stale surface queues are pruned when no pending surface payload exists;
  - stale coord aliases are removed on failed direct extraction;
  - launcher skips extraction attempts when pending surface payload count is zero.

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Latest current matrix:

- `build/captures/current_after_surface_queue_cleanup_20260604`
- copied into `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/`

Rows:

| Scenario | Frame | Raw ms | CPU update ms | Request ms | Gen ms | Clip ms | Surface extract ms | GPU ray ms | Missing voxel | Sampled/unsampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `380` | `40.06` | `9.07` | `2.49` | `5.60` | `0.98` | n/a parsed | `7.22` | `0` | `0/0` | `0/0` |
| walk realtime | `600` | `105.52` | `65.19` | `30.04` | `26.18` | `8.92` | `15.04` | `36.16` | `435` | `366/69` | `0/0` |
| high-alt | `400` | `69.39` | `44.03` | `12.73` | `11.19` | `20.10` | n/a parsed | `16.61` | `1994` | `62/1932` | `0/0` |

Important interpretation:

- The accepted cleanup did not create a playable candidate.
- Post-open hidden-exact surface extraction still bursts; fixed frame `384` showed `29.62 ms` prepublish surface extraction.
- Walk realtime remains the primary representative blocker: exact request/prep, exact page generation, surface extraction, and GPU/gaps are all material.
- High-alt is safer than before because visible-critical prepump keeps visible coverage high, but cache debt remains.

Rejected probes:

- direct footprint columns, shared column cache, general surface budget `4`, post-open prepublish max `4`, hidden exact post-open surface budget `16`, and bounded64 current stack.

Next concrete implementation:

- Extract worker-safe exact sparse page payload generation and add default-off async exact page generation.
- Keep main-thread apply/upload/surface staging; discard stale or edit-invalid async completions.
- Do not move sampled unknown mid debt async and do not lower correctness guards.

### Streaming Playability Campaign Exact Generation Branch - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

New default-off knobs:

- `VENPOD_SPARSE_EXACT_ASYNC_GENERATION=0`
- `VENPOD_SPARSE_EXACT_ASYNC_VISIBLE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_QUEUE_MAX=256`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME=32`
- `VENPOD_SPARSE_EXACT_DIRECT_GENERATION=0`

Branches:

- all-class async exact generation: rejected. Fixed improved, but walk realtime regressed from `75.77` to `100.37 ms` CPU; clip/pump rose from `45.99/31.28` to `56.60/54.29`, and missing voxel rose from `453` to `512`.
- speculative-only async exact generation: no effect in sampled rows. It queued/applied `0` async bricks.
- direct exact generation: rejected. Walk realtime was `82.33 ms` CPU with `46.30 ms` clip, still worse than baseline and far from playable.

Conclusion:

- no validated 60 FPS candidate exists;
- exact visible/current generation cannot be moved async without integrating ownership/readiness, because mid-clipmap catch-up repays the deferred readiness debt;
- async remains viable only for cache/prefetch or CPU-proved fallback-valid work;
- next implementation must be the ownership-aware streaming state machine: visible sampled unknown work stays guarded, cache/prefetch work moves to a worker queue, and frame-boundary apply/upload/surface extraction are budgeted separately.

### Active Campaign Control - 2026-06-04

The active `/goal` is `streaming_playability_real_fix_campaign_20260604`. `handoff.md` is the durable campaign handoff and should be updated after every meaningful branch.

Anti-stop rule: do not end after one diagnostic, one failed heuristic, or one small partial win. Continue through measured safe branches until there is a validated noncapture playable candidate, a strong candidate with exact remaining bottlenecks, or a hard architecture/tool blocker proven after multiple attempts.

Latest branch state:

- `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` is a default-off high-alt/cache partial still in the worktree. It reduced high-alt pump cost but does not solve representative walk/realtime.
- Post-open surface cap, surface strict/buried, global generation budget `8`, broad exact async, direct exact generation, blunt pump cap, age-priority, and ring-only visible-critical heuristics remain rejected.
- Walk/realtime is blocked by sampled/projected-visible fallback-unknown mid debt plus exact request/generation/surface/GPU costs. Unknown fallback remains critical.
- The next real implementation is an ownership-aware streaming state machine: visible sampled unknown work guarded, cache/prefetch async/deferred, worker payload generation with generation/edit stamps, frame-boundary apply/upload/surface budgets, and owner proof metadata/feedback for movement fallback.

### Visible Priority Pump Branch - 2026-06-04

Default-off knob: `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP=0`.

What changed:

- Projected-visible missing mid-voxel coords can be moved to the front of the existing voxel generation queue.
- No default behavior change.
- No coverage guard weakening.
- Unknown fallback remains critical.
- Added `PERF_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY` and harness parsing fields `midVisiblePriorityProjected` / `midVisiblePriorityPrioritized`.

Measured result:

- Build passed; ctest passed.
- Walk baseline rerun: raw `69.36`, CPU `58.73`, clip/pump `28.07/25.79`.
- Walk priority only: raw `79.40`, CPU `48.62`, clip/pump `21.35/19.30`, `335` prioritized; CPU partial but raw worse.
- Walk priority + parallel mid pump: raw `64.18`, CPU `43.18`, clip/pump `11.94/9.87`.
- Walk priority + parallel mid + exact parallel: raw `59.55`, CPU `44.41`, gen `10.01`, GPU `10.14`; still not playable.
- High-alt with the parallel stack regressed to raw `75.05`, CPU `41.20`; reject as global candidate.

Decision:

- `VISIBLE_PRIORITY_PUMP` is safe to keep default-off as an ownership-lane primitive and measured partial.
- It is not a validated playable candidate.
- Next implementation should make lane classification persistent inside the clipmap cache: visible-critical/cache/prefetch tags, separate backlog ages and budgets, and eventual async only for cache/prefetch or CPU-proved fallback-valid work.

### Streaming Playability Candidate Stack - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test passed. Harness now has `-WalkFixedDtMs`, default `0`.

Accepted default-off stack: playable render scale, background split `0.375`, clean terrain prefetch throttle, backlog-aware pump, incremental pressure trim `8192`, and parallel visible mid-voxel pump with `4` workers.

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `27.45` | `23.00` | `4.88` | `17.30` | `0.81` | `0.00` | n/a | `8.08` | `0/0` | `0/0` |
| walk realtime | `600` | `62.73` | `43.05` | `16.06` | `16.83` | `10.14` | `7.60` | `11.88` | `16.56` | `444/284` | `0/0` |
| high-alt | `386` | `51.50` | `26.41` | `4.67` | `7.08` | `14.65` | `6.39` | n/a | `11.79` | `1451/0` | `0/0` |

Fixed-dt walk isolated the main partial: raw/CPU `158.68/137.57` baseline, `71.10/51.18` with incremental trim, and `60.24/41.75` with incremental trim + parallel mid pump.

Rejected: streaming lane scheduler, priority-side-effect removal patch, trim+parallel surface, trim+cache-only defer as global stack, worker column cache, exact parallel, surface sort cache, hidden exact scan budget, surface-ready publish queue, and terrain-critical inline surface defer as global stack.

Decision: strong default-off candidate, not 60 FPS. No defaults changed. Remaining walk blocker is distributed request/generation/clip/surface/GPU cost plus sampled fallback-unknown mid debt. Next work is persistent ownership lanes in clipmap state, request-prep/touch caching by source/lane, and visible-critical/cache surface budgets.

### Terrain-Critical Parallel Generation Retained Partial - 2026-06-04

Continuation outcome:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Retained default-off branch:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`

Implementation summary:

- Added `SparseVoxelWorld::PumpGenerationForCoordsParallel`.
- The launcher collects terrain-critical exact sparse coords already required this frame.
- Workers generate CPU payloads using worker-local terrain column caches.
- The main thread applies payloads and continues through existing resident/upload/surface queues.
- This avoids treating unknown fallback as safe and avoids the failure mode of async visible exact generation.

Artifacts:

- retained no-heavy timing: `build/captures/candidate_terrain_critical_parallel_generation_min16_20260604`
- retained full diagnostics: `build/captures/candidate_terrain_critical_parallel_generation_min16_diagnostics_20260604`
- rejected resident touch: `build/captures/candidate_request_resident_touch_cache_20260604`
- rejected surface visible lane: `build/captures/candidate_surface_visible_lane_pump_20260604`
- scenario-specific background scale `0.25`: `build/captures/candidate_bgscale025_parallel_gen_20260604`
- scenario-specific mid interest interval `2`: `build/captures/candidate_mid_interval2_parallel_gen_20260604`

Full-diagnostic retained rows:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe | Parallel generated |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.85` | `19.36` | `3.67` | `14.98` | `0.71` | `0.00` | `18.00` | `6.31` | `0/0` | `0/0/0` | `100` | `0/0` | `0` |
| walk realtime | `600` | `50.55` | `50.79` | `15.27` | `14.97` | `20.53` | `9.16` | `11.21` | `19.30` | `455/254` | `0/130/325` | `94` | `0/0` | `33` |
| high-alt | `360` | `47.65` | `26.71` | `5.72` | `6.55` | `14.43` | `4.05` | `3.95` | `9.96` | `1759/0` | `0/1238/521` | `100` | `0/0` | `0` |

Rejected and removed:

- resident-touch cache: did not improve representative request cost.
- surface visible-lane pump: worsened walk raw and caused high-alt clip spike.

Scenario-specific, not global:

- background pass scale `0.25` helped high-alt/GPU but walk raw stayed worse than retained min16 stack.
- mid interest interval `2` helped high-alt/clip but worsened fixed/walk raw.

Decision:

- terrain-critical parallel generation is safe to keep default-off as a partial candidate;
- it is not a validated playable candidate;
- no defaults changed;
- the remaining representative blocker is the lack of persistent ownership-lane streaming across request, generation, clipmap pump, surface extraction, and upload/apply.

Next implementation:

- build persistent visible/current, cache, prefetch, and maintenance lanes in streaming state;
- keep visible sampled fallback-invalid/unknown work synchronous and guarded;
- move only cache/prefetch or CPU-proved fallback-valid work to async worker generation;
- budget apply/upload/surface extraction per lane;
- add request-prep/touch caching by lane/source without repeating the rejected fast-resident and resident-touch failures.

### Streaming Lane Queue Priority Branch - 2026-06-04

Retained default-off implementation:

- `VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY=0`
- `SparseVoxelWorldConfig::streamingLaneQueuePriority`
- `SparseVoxelWorldStats::streamingLaneQueuePriorityActive`
- harness switch: `perf_noncapture_smoke.ps1 -StreamingLaneQueuePriority`

Reason:

- `SparseStreamingLane` was already tracked and logged, but exact sparse generation/upload/surface queue ordering ignored it.
- The retained branch makes queue scoring lane-aware within each residency class, so `publicCritical` and `visible` work can drain before `prefetch/cache` work without dropping anything or weakening ownership.

Validation:

- Release build passed.
- `ctest` passed `1/1`.
- Full diagnostics: `build/captures/candidate_streaming_lane_queue_priority_diagnostics_20260604`.
- Campaign artifacts copied to `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/`.

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` | `0/0` | `0/0/0` | `100` | `0/0` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` | `447/248` | `0/93/354` | `95` | `0/0` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` | `1755/0` | `0/1235/520` | `100` | `0/0` |

Decision:

- retained as a default-off safe partial;
- not a validated playable candidate;
- no defaults changed.

Rejected after the retained branch:

- parallel surface extraction: no material win and fixed regressed;
- parallel exact generation: walk CPU improved, but raw/high-alt regressed;
- mid worker column cache: severe CPU regression;
- direct footprint columns: severe request/generation regression;
- mid pump `8` workers: higher overhead;
- background pass scale `0.25`: GPU reduction but CPU/raw explosion;
- generation budget `8`: starved useful work;
- buried-solid surface fast path: severe CPU regression.

Current blocker:

- fixed remains near `20 ms` with generation/surface bursts;
- walk remains around `54 ms` raw with request/generation/clip/surface/GPU all material and sampled fallback-invalid/unknown mid debt;
- high-alt is stable but still has broad unsampled cache backlog.

Next real implementation is persistent ownership lanes in streaming state with separate cache/prefetch backlog, worker payload generation only for safe noncritical lanes, and lane-specific apply/upload/surface budgets.

### Continuation Probes After Lane Priority - 2026-06-04

Build/test after removing the rejected transient persistent-lane prototype:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/candidate_streaming_lane_queue_priority_noheavy_baseline_20260604`
- `build/captures/candidate_persistent_mid_lanes_20260604_highalt360`
- `build/captures/candidate_request_fast_resident_touch_20260604`
- `build/captures/candidate_surface_class_sort_cache_20260604`
- `build/captures/candidate_surface_general_strict_budget_20260604`
- `build/captures/candidate_direct_exact_generation_20260604`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_rejected_continuation_20260604.md`

Matched no-heavy retained baseline:

| Scenario | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Backlog |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `22.24` | `22.55` | `4.18` | `17.52` | `0.84` | `0.00` | `19.53` | `6.03` | `0` |
| walk realtime | `59.00` | `35.16` | `14.15` | `14.72` | `6.27` | `4.32` | `11.54` | `11.80` | `435` |
| high-alt | `49.12` | `27.55` | `6.57` | `6.71` | `14.26` | `4.52` | `4.23` | `10.09` | `1753` |

Rejected continuation branches:

- transient persistent mid lanes were implemented, tested, and removed. It was only queue ordering. Walk raw moved `59.00 -> 55.18`, but walk CPU regressed `35.16 -> 42.54`, fixed regressed `22.24 -> 24.05`, and high-alt regressed `49.12 -> 49.99`.
- `RequestFastResidentTouch`: request did not improve; walk request/CPU `16.78/44.03`.
- `SurfaceClassSortCache`: high-alt regressed to raw/CPU `61.88/37.21`.
- `SurfaceGeneralStrictBudget`: walk regressed to raw/CPU `60.86/51.73`.
- `DirectExactGeneration`: fixed CPU improved, but walk/high-alt regressed; walk raw/CPU `59.11/50.10`, high-alt raw `54.51`.

Decision:

- No new candidate was retained.
- All defaults remain unchanged.
- Do not repeat the simple persistent queue-ordering prototype.
- The required next slice is a real ownership-lane state machine: separate visible-critical/cache/prefetch state, async only for cache/prefetch or CPU-proved valid fallback, and lane-specific apply/upload/surface budgets.

### Campaign Continuation - Persistent Terrain Column Cache Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/terrain_column_cache_rejected_20260605.md`

What happened:

- A default-off persistent terrain column cache was tried because generation and surface extraction remain material in fixed/walk rows.
- It was tested with capped variants, then removed.
- It did not weaken ownership or fallback rules, but it was not a stable performance win.

Key rows:

| Row | Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| retained baseline | fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | reference |
| retained baseline | walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | reference |
| retained baseline | high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | reference |
| terrain cache `65536` | fixed | `384` | `24.50` | `23.69` | `3.91` | `18.96` | `0.81` | `0.00` | `25.66/n/a` | `7.20` | rejected fixed regression |
| terrain cache `65536` | walk realtime | `600` | `50.73` | `47.94` | `14.07` | `16.81` | `17.04` | `4.45` | `12.59/3.02` | `10.68` | rejected partial/mixed |
| terrain cache `65536` | high-alt | `395` | `43.95` | `24.93` | `3.28` | `6.49` | `15.16` | `4.45` | n/a | `10.28` | rejected CPU/clip regression |

Decision:

- No `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_*` knob remains.
- All defaults remain unchanged.
- This branch should not be repeated as a cross-frame map cache.

Next measured branch:

- inspect request planning/cache behavior before more generation micro-caches;
- current CPU detail repeatedly shows material request cost and `centerDelta=0/0/0 fullRebuild=1`;
- if the request set is being rebuilt on unchanged request keys, implement a behavior-preserving request reuse path instead of dropping or deferring requests.

### Campaign Continuation - Request-Side Existing Flags Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_side_existing_flags_rejected_20260605.md`

What was tested:

- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=1`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=1`

Result:

- Neither existing request-side flag is retained.
- Pressure-trim guard removed trim scans, but no-heavy walk regressed to raw/CPU `74.33/52.83`; clip/pump grew to `23.48/21.22`.
- Hidden tracked scan budgeting did not improve representative walk; raw/CPU stayed `74.03/47.07` and budget hits remained.
- No code changed for these probes; all defaults remain unchanged.

Current best retained reference:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` |

Decision:

- Stop chasing isolated request micro-flags.
- `PERF_SPARSE_REQUEST_DETAIL otherMs` is not exclusive; it subtracts hierarchy and stats only, so do not use it as a standalone optimization bucket.
- Next real implementation is ownership-aware streaming state: persistent visible-critical/cache/prefetch lanes, lane-specific generation/apply/upload/surface budgets, and async only for cache/prefetch or CPU-proved fallback-valid work.

### Streaming Playability Explicit Source Lanes - 2026-06-05

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained default-off candidate:

- `VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES=0`
- `perf_noncapture_smoke.ps1 -RequestExplicitSourceLanes`
- explicit-lane residency touch APIs in `SparseBrickPool` and `SparseVoxelWorld`

Implementation summary:

- Broad view-cone and terrain-prefetch request sources can now preserve `Prefetch` lane while still touching visible residency.
- Ownership/near/motion/collision/edit/terrain-critical requests remain high-lane/default guarded.
- Existing resident records are lane-promotion only, so an explicit lower source lane cannot demote already visible/public-critical work.
- Default behavior is unchanged.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_parallel_mid_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_parallel_surface_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_campaign_20260605_table.md`

Key rows:

| Branch | Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing voxel | Coverage | Gen lanes cache/prefetch/visible/public | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---|
| baseline | fixed | `384` | `23.01` | `19.91` | `3.92` | `15.20` | `0.77` | n/a | n/a | `6.14` | `0` | `100` | `0/0/1198/0` | `0/0` | reference |
| source lanes | fixed | `384` | `23.13` | `21.62` | `3.76` | `17.09` | `0.76` | n/a | n/a | `5.72` | `0` | `100` | `0/1072/120/0` | `0/0` | lane split works, fixed CPU slightly worse |
| baseline | walk realtime | `600` | `72.31` | `64.99` | `14.23` | `14.46` | `36.29` | `25.31` | `10.89` | `15.16` | `469` | `94` | `0/0/3115/34` | `0/0` | reference |
| source lanes | walk realtime | `600` | `79.25` | `49.97` | `13.92` | `13.35` | `22.68` | `20.26` | `12.69` | `16.55` | `441` | `94` | `0/2996/50/28` | `0/0` | CPU improves, raw still bad |
| baseline | high-alt | `395` | `44.29` | `30.09` | `6.98` | `6.49` | `16.62` | `5.52` | n/a | `10.07` | `1769` | `99` | `0/0/5384/8` | `0/0` | reference |
| source lanes | high-alt | `394` | `49.18` | `24.55` | `5.51` | `6.22` | `12.81` | `5.36` | n/a | `10.53` | `1860` | `99` | `0/5431/2/8` | `0/0` | CPU improves, raw still bad |

Rejected/removed:

- prefetch-lane surface deferral. It produced surface/publish backlog in the thousands and did not improve frame time.

Not accepted globally:

- source lanes plus parallel mid pump: lower clip/pump but worse request/gen/CPU.
- source lanes plus parallel surface extraction: walk regression.

Decision:

- source lanes are a safe default-off partial state-machine slice, not a playable candidate.
- no defaults changed.
- next implementation must carry lanes through publish/apply/surface readiness with explicit visible-critical guarantees; sampled fallback-invalid/unknown work remains synchronous/guarded.

### Streaming Playability Publish Lane Propagation - 2026-06-05

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained behavior-neutral infrastructure:

- `SparsePendingPageTablePublish::streamingLane`
- `SparseBrickUploadPacket::streamingLane`
- lane-aware `SparsePagePublishQueue::Enqueue(...)`
- lane totals in `SparsePagePublishQueueStats`
- `PERF_SPARSE_PAGE_PUBLISH_LANES`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface4_vsync0_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/publish_lane_propagation_vsync0_20260605`

Rejected probe:

- `VENPOD_SPARSE_POST_OPEN_PREPUBLISH_SURFACE_MAX_MS=4`
- fixed frame `360`: raw/CPU `23.39/16.49`, but `publishPending=3619`, `publishLag=139`, `publishSurfGate=435/21`
- walk frame `600`: raw/CPU `52.85/26.88`, but `publishPending=4542`, `publishLag=515`, `publishSurfGate=485/20`
- decision: rejected because it shifts surface/publish debt instead of producing a stable playable candidate

Publish-lane propagation validation:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Missing voxel | Generation lanes cache/prefetch/visible/public | Upload lanes cache/prefetch/visible/public | Publish queue lanes cache/prefetch/visible/public | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|---:|---|
| fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | n/a | `0/1737/122/0` | `0/0/0/0` | frame `360`: `0/12/0/0` | `0/0` | fixed publish debt is prefetch, but fixed is not the main blocker |
| walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | `437` | `0/5188/105/27` | `0/1156/722/0` | `0/0/5/0` | `0/0` | representative publish/surface debt is visible, not prefetch |
| high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | `1709` | `0/6958/4/8` | `0/0/0/0` | frame `360`: `0/0/0/0` | `0/0` | high-alt remains broad cache/mid debt, not publish debt in this row |

Decision:

- publish-lane propagation is retained as behavior-neutral infrastructure and diagnostics.
- it is not a playable candidate.
- the walk frame `600` publish/surface debt is visible-lane (`queue=0/0/5/0`, `qsurf=0/43/0/0`), so prefetch surface/publish deferral is not the next safe fix.
- all defaults remain unchanged.

Current blocker:

- walk remains expensive because sampled visible fallback-invalid/unknown mid and exact/surface work cannot be deferred safely.
- next safe work must prove a valid fallback owner, reduce actual visible sampled footprint under a screen/ray contract, or split the streaming state machine further. Do not repeat blind prefetch surface deferral, local queue-order heuristics, blunt caps, or unknown-fallback async.

### Campaign Continuation - Generation Lane Budget Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generation_lane_budget_rejected_20260605.md`

The default-off generation lane budget prototype was tried as a state-machine slice for exact generation:

- cache/prefetch generation cap: `4`
- background split `0.375`, clean throttle, explicit source lanes, and streaming lane queue priority enabled
- no work was dropped and no correctness policy was changed

Rows:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `360` | `15.59` | `21.10` | `4.13` | `16.20` | `0.76` | `0.00` | `18.96` | `5.90` | `0` | `100` | `0/0` |
| walk realtime | `600` | `65.15` | `63.94` | `13.76` | `13.99` | `36.17` | `23.78` | `10.81` | `14.21` | `556` | `93` | `0/0` |
| high-alt | `360` | `44.25` | `28.32` | `6.58` | `5.35` | `16.38` | `5.34` | `3.09` | `9.57` | `2183` | `99` | `0/0` |

Decision:

- rejected and removed from code;
- no generation-lane-budget env knobs remain;
- representative walk regressed, so this is not a retained candidate;
- next branch is to probe existing parallel exact generation because generation remains a repeated bucket before moving to request/surface buckets.

### Campaign Continuation - Footprint Signature Reuse Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/footprint_signature_reuse_rejected_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/footprint_signature_reuse_vsync0_20260605`

Decision:

- default-off footprint-signature reuse was implemented, tested, rejected, and removed;
- no `VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_SIGNATURE_*` env remains;
- all defaults remain unchanged.

Rows:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `24.88` | `30.04` | `22.96` | `27.63` | `0.84` | `0.88` | `0.00` | `0.00` | `0` | `0` | `100` | `100` | rejected fixed regression |
| walk realtime | `600` | `52.16` | `84.14` | `45.85` | `68.78` | `17.39` | `40.93` | `5.44` | `28.00` | `447` | `460` | `95` | `94` | rejected representative regression |
| high-alt | `360` | `50.05` | `48.71` | `29.79` | `31.91` | `16.39` | `16.87` | `5.36` | `5.65` | `2228` | `2196` | `99` | `99` | no useful CPU win |

Mechanism:

- baseline walk frame `600`: `newV/goneV=0/0`, `budgetReason=0`, generated `5` voxel bricks.
- candidate walk frame `600`: `newV/goneV=49/49`, `budgetReason=2`, generated `24` voxel bricks.
- coarser signature reuse delayed interest churn, then coverage emergency forced catch-up.

Next:

- do not repeat signature/interval skip variants;
- true incremental scroll/delta reuse is the safe version if this branch is continued;
- request-cache inspection did not expose a clean blind cache win because frame `600` request cost overlaps visible/recovery, pressure trim, and hidden/miss feedback work.

### Campaign Continuation - Voxel Footprint Reuse Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_rejected_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_vsync0_20260605`

The default-off candidate:

- `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_FOOTPRINT_REUSE=1`
- harness switch `-MidClipmapVoxelFootprintReuse`

Result:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Req before | Req candidate | Gen before | Gen candidate | Clip before | Clip candidate | Pump before | Pump candidate | Surface before | Surface candidate | GPU before | GPU candidate | Missing before | Missing candidate | Coverage before | Coverage candidate | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `24.88` | `25.05` | `22.96` | `22.64` | `4.55` | `4.42` | `17.55` | `16.95` | `0.84` | `1.25` | `0.00` | `0.00` | `21.18` | `20.88` | `6.02` | `6.62` | `0` | `0` | `100` | `100` | neutral fixed |
| walk realtime | `600` | `52.16` | `59.33` | `45.85` | `78.03` | `13.95` | `17.16` | `14.49` | `15.85` | `17.39` | `45.00` | `5.44` | `32.09` | `9.54` | `9.70` | `14.27` | `18.69` | `447` | `441` | `95` | `94` | rejected representative regression |
| high-alt | `360` | `50.05` | `53.72` | `29.79` | `34.47` | `7.22` | `8.55` | `6.17` | `7.10` | `16.39` | `18.82` | `5.36` | `5.92` | `3.87` | `4.10` | `10.75` | `10.64` | `2228` | `2228` | `99` | `99` | rejected high-alt regression |

Failure mechanism:

- the candidate reused some voxel-interest frames, but walk frame `600` had `newV/goneV=109/109`, `budgetReason=2`, `genVoxel=32`, and `pumpVoxel=32.09`;
- delaying the voxel footprint still accumulated deterministic entering/leaving debt and forced catch-up;
- this confirms that the correct fix is not another signature or interval skip.

Decision:

- rejected and removed from code;
- no `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_FOOTPRINT_REUSE` env remains;
- no `-MidClipmapVoxelFootprintReuse` harness switch remains;
- all defaults remain unchanged.

Next:

- implement true mid-clipmap incremental scroll/delta update if feasible: preserve unchanged resident bricks by spatial key/stamp and update entering/leaving footprint changes as they occur.
- if that refactor is too large to implement immediately, add a narrow `UpdateVoxelInterest` cost split for candidate generation, terrain sampling, sort/emit, and backlog carry, because that directly informs the delta implementation rather than adding generic diagnostics.

### Campaign Continuation - Mid-Clipmap Interest Detail Timing - 2026-06-05

New default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL=0`
- noncapture switch `-MidClipmapInterestDetail`
- log `PERF_SPARSE_MID_CLIPMAP_INTEREST_DETAIL`

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_timing_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_vsync0_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `21.32` | `22.63` | `4.10` | `17.72` | `0.80` | `0.00` | frame log `23.37/2.05` | `7.40` | `0` | `100` | `0/0` |
| walk realtime | `600` | `60.84` | `38.38` | `16.54` | `14.86` | `6.95` | `5.10` | `11.13/2.61` | `19.44` | `415` | `95` | `0/0` |
| high-alt | `360` | `51.32` | `32.84` | `8.48` | `6.74` | `17.61` | `5.80` | `4.70/1.71` | `10.14` | `2184` | `99` | `0/0` |

Interest detail:

| Scenario | Frame | Reuse | Line | Anchor | Sort/emit | Backlog | Diagnostics | Candidates | Emitted | Missing | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `1` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0` | `0` | `0` | `100` |
| walk realtime | `600` | `1` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0` | `0` | `415` | `95` |
| high-alt | `360` | `0` | `0.20` | `1.37` | `1.65` | `0.11` | `0.51` | `12522` | `9216` | `2184` | `99` |

Decision:

- interest construction is not the next single blocker in the measured rows.
- do not implement another signature/interval skip.
- representative walk is a mixed bucket: GPU, request, generation, surface extraction/staging, and smaller clip/pump.
- fixed is blocked by generation/surface/GPU.
- high-alt still has mid debt, but pump and broader streaming/apply costs matter more than candidate construction alone.

Next:

- target pump/generation/apply separation or surface extraction/staging.
- true mid-clipmap delta reuse remains valid only if it updates entering/leaving changes as they occur and preserves unchanged resident bricks by key/stamp.

### Campaign Continuation - Existing Branch Validation - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/existing_branch_validation_20260605.md`

Branches tested against the current VSync-off stack:

- `-ParallelMidVoxelPump`
- `-PostOpenSurfaceMaxMs 4 -SurfaceReadyPublishQueue -SurfaceReadyPublishPressure`
- `-RequestFastResidentTouch`

Baseline rows from `interest_detail_vsync0_20260605`:

- fixed frame `384`: raw/CPU `21.32/22.63`
- walk realtime frame `600`: raw/CPU `60.84/38.38`
- high-alt frame `360`: raw/CPU `51.32/32.84`

Branch outcomes:

- parallel mid pump: rejected as stack member; walk CPU improved to `31.68`, but raw regressed to `78.06`, fixed regressed to `39.07`, and high-alt stayed about `51.50`.
- post-open surface cap plus ready queue: rejected; fixed CPU improved, but walk raw/CPU regressed to `89.93/50.70` and surface-ready pending grew to `5153` with oldest age `525`.
- request fast resident touch: rejected; request time improved, but walk raw/CPU regressed to `90.48/52.88` with clip/pump `28.70/26.25` and missing voxel `530`.

Decision:

- no existing isolated flag becomes a playable candidate member.
- all defaults remain unchanged.
- the next implementation must be a lane-aware streaming state-machine slice, not another local flag: visible/current ownership, hidden/post-open repair, and cache/prefetch work need independent scheduling and backlog accounting.

### Campaign Continuation - Async Exact Prefetch Lane Stack - 2026-06-05

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- Release build passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.

Implemented default-off:

- `VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME=0`
- `PERF_SPARSE_EXACT_ASYNC` now logs `maxEnqueue`, enqueue lane counts, and apply lane counts.

Validation result:

- async exact prefetch lane moved only `Cache`/`Prefetch` streaming-lane work.
- `Visible` and `PublicCritical` async counts stayed `0` in sampled rows.
- `missScreenPct=0` and `unsafeNearMissScreenPct=0` stayed `0`, but this is not success by itself.

Best current default-off stack:

- playable render quality
- background split `0.375`
- clean prefetch throttle
- backlog-aware pump
- critical reuse
- terrain critical parallel generation
- surface extraction max `1`
- incremental pressure trim
- explicit source lanes
- streaming lane queue priority
- worker column cache
- async exact prefetch lane
- parallel mid pump
- request fast resident touch

Rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.87` | `18.01` | `4.25` | `12.99` | `0.76` | `0.00` | `6.26` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `72.81` | `35.76` | `15.36` | `14.19` | `6.20` | `4.35` | `5.54` | `451/273` | `95` | `0/0` |
| high-alt | `360` | `52.49` | `30.92` | `8.82` | `5.58` | `16.51` | `4.59` | `11.30` | `1746/0` | `100` | `0/0` |

Walk-window average, frames `596-607`:

- baseline stack: raw/CPU `79.78/48.63`
- async prefetch: raw/CPU `74.92/42.59`
- async prefetch + parallel mid: raw/CPU `71.17/39.11`
- async prefetch + parallel mid + request fast touch: raw/CPU `64.32/32.90`
- plus parallel exact: raw/CPU `65.11/30.40`, mixed because fixed and walk average raw were not better

Rejected:

- async enqueue caps `8` and `16`: safe but lost throughput.
- parallel surface extraction: fixed raw regressed to `36.83`, walk max raw worsened.
- parallel exact is not the strongest accepted stack; keep as comparison only for now.

Decision:

- no validated 60 FPS candidate.
- the strongest stack is a real default-off partial candidate, not a default/playable release.
- remaining blockers are measured mixed CPU buckets: request prep, exact generation, clip/pump, high-alt mid debt.
- next work should implement a lane-aware streaming state-machine slice with async/cache/prefetch generation and apply/upload accounting while visible/public ownership remains synchronous and guarded.
## Active Goal Compact Handoff - 2026-06-05

New compact resume file:

- `active-goal-handoff.md`

Build/test after accepted-state cleanup:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Branches attempted and removed:

- post-open hidden-exact prefetch lane: rejected because critical/pressure work dominated and performance regressed
- hidden-exact direct parallel generation: rejected because it was fixed-path helpful but walk/high-alt mixed or regressive

Fresh accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `10.26` | `1746/0` | `100` | `0/0` |

Next branch should not be another hidden-exact deferral heuristic. Target request/gen coupling or exact-generation pipeline architecture with staged worker/apply accounting.

## Active Goal Branch Ladder Closure - 2026-06-05

Compact resume file:

- `active-goal-handoff.md`

Post-cleanup build/test:

- `.\build.ps1 -Config Release`: passed before compaction after transient branches were removed.
- `ctest --test-dir build --output-on-failure -C Release`: passed after compaction, `1/1`.

Latest accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | Surface extract/stage | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `0.00` | `26.50/2.35` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `6.59` | `9.58/2.57` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `4.93` | `6.64/1.80` | `10.26` | `1746/0` | `100` | `0/0` |

Additional branches attempted after the hidden-exact probes:

| Branch | Artifact | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---|---:|---:|---:|---|
| generation queue fast membership | `generation_queue_fast_membership_candidate_20260605` | `23.73/20.21` | `68.62/31.12` | `52.43/31.06` | removed; fixed/high-alt regressed and raw did not improve |
| pressure trim free page guard | `pressure_trim_free_page_guard_candidate_20260605` | `22.06/17.65` | `67.73/37.26` | `52.75/27.53` | not accepted; walk clip/raw regressed |
| parallel surface extraction | `parallel_surface_extraction_candidate_20260605` | `20.59/18.02` | `62.23/47.27` | `50.51/33.12` | not accepted; walk/high-alt CPU regressed |
| direct footprint columns | `direct_footprint_columns_candidate_20260605` | `22.95/19.19` | `59.53/52.45` | `54.01/30.89` | not accepted; request/gen/clip regressed |
| persistent terrain column cache | `persistent_terrain_column_cache_candidate_20260605` | `33.95/19.33` | `64.29/47.90` | `49.39/42.70` | removed; cache growth/hash overhead made runtime worse |

Removed transient code flags:

- `VENPOD_SPARSE_GENERATION_QUEUE_FAST_MEMBERSHIP`
- `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_PERSISTENT`

Decision:

- no validated 60 FPS candidate exists;
- the strongest opt-in stack remains far above budget: fixed about `23 ms`, walk about `58 ms`, high-alt about `51 ms`;
- this is now an architecture problem, not an isolated flag problem;
- the next code path should create persistent ownership lanes across request, generation, upload/apply, surface extraction, and publish queues;
- visible/public-critical and sampled fallback-invalid/unknown work must remain synchronous/readiness-gated, while only cache/prefetch or CPU-proven fallback-valid work can move async.

## Generated-Lane Accounting Slice - 2026-06-05

Retained behavior-neutral accounting:

- `SparseVoxelWorldStats::generated*LaneBricksLastFrame`
- `PERF_SPARSE_GENERATED_LANES`
- generated-lane CSV columns in `perf_noncapture_smoke.ps1`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files had no code whitespace errors, only existing LF-to-CRLF warnings.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generated_lane_accounting_v2_20260605`

Accepted-stack rows:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Queued gen lanes prefetch/visible/public | Generated lanes prefetch/visible/public | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.54` | `18.76` | `3.98` | `13.99` | `0.77` | `6.71` | `915/121/0` | `24/121/0` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `67.95` | `55.11` | `17.91` | `16.07` | `21.10` | `17.24` | `4929/142/47` | `15/88/47` | `523/303` | `94` | `0/0` |
| high-alt | `360` | `51.02` | `28.72` | `8.27` | `5.14` | `15.31` | `6.37` | `6968/11/8` | `20/11/8` | `1769/0` | `100` | `0/0` |

Decision:

- visible/public exact generation is the immediate generation wall in representative rows;
- cache/prefetch async expansion alone cannot solve fixed/walk;
- sampled fallback-invalid/unknown work remains critical;
- next implementation should target faster same-frame visible/public exact generation, likely persistent synchronous workers or a staged public-critical generation/apply path.

### Campaign Continuation - Parallel Exact Std Execution Rejection - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_rejected_20260605.md`

Candidate:

- temporary `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION=1`
- chunked `std::execution::par` in existing exact-generation parallel paths

Rows:

| Scenario | Baseline raw/CPU/gen/clip | Candidate raw/CPU/gen/clip | Decision |
|---|---:|---:|---|
| fixed | `24.54/18.76/13.99/0.77` | `25.13/20.86/15.13/0.75` | rejected |
| walk realtime | `67.95/55.11/16.07/21.10` | `72.46/46.34/12.53/20.45` | rejected; CPU improvement did not survive raw validation |
| high-alt | `51.02/28.72/5.14/15.31` | `49.42/33.21/5.57/19.07` | rejected; CPU/clip regressed |

Decision:

- candidate removed after validation
- no `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION` flag remains
- post-removal build and ctest passed
- visible/public exact work still needs a real same-frame pipeline improvement rather than another per-call thread-launch variant

### Campaign Continuation - Persistent Exact Workers Partial Candidate - 2026-06-05

New default-off flag:

- `VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS=0`
- harness: `-ParallelExactPersistentWorkers`

Implementation:

- persistent exact-generation worker pool in `SparseVoxelWorld`
- active only when exact parallel generation is also enabled
- waits for current-frame batch completion before apply/presentation
- does not defer visible/public unknown ownership

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_2w_candidate_20260605`

2-worker result:

| Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `21.81` | `17.17` | `3.85` | `12.55` | `0.76` | `0.00` | `6.97` | `0/0` | `0/0` |
| walk realtime | `67.23` | `32.40` | `13.18` | `12.82` | `6.38` | `4.25` | `20.19` | `409/209` | `0/0` |
| high-alt | `51.21` | `28.80` | `8.71` | `4.74` | `15.34` | `4.20` | `13.16` | `1764/0` | `0/0` |

Decision:

- 2-worker persistent exact generation is a safe partial default-off candidate.
- 4-worker setting regressed high-alt and is not the chosen setting.
- combined candidate is still not playable or 60 FPS.
- next measured blockers are fixed surface extraction, walk CPU/GPU/frame gap, and high-alt clip/pump/GPU.

### Campaign Continuation - Post-Open Surface Cap Rejection - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_persistent_exact_2w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_rejected_20260605.md`

Finding:

- fixed frame `384` surface extraction spike was pre-publish surface gate work: `extracted=139`, `elapsedMs=21.33`, `maxMs=40`.
- `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS=1` does not bound that path.

Candidate:

- existing `VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS=1`
- no code change

Result:

| Scenario | Raw | CPU | Request | Gen | Clip | Surface extract | GPU | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `22.54` | `24.75` | `11.19` | `12.80` | `0.75` | `5.43` | `6.93` | rejected |
| walk realtime | `67.93` | `47.09` | `18.45` | `14.06` | `14.56` | `12.17` | `14.14` | rejected |
| high-alt | `69.45` | `39.02` | `13.81` | `6.15` | `19.05` | `4.60` | `5.80` | rejected |

Decision:

- cap-only surface throttling is not the fix.
- it needs a publish/backlog-aware surface architecture if revisited.

### Campaign Continuation - Persistent Mid Workers and Trim Budget Probe - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS=0`
- harness switch `-ParallelMidVoxelPumpPersistentWorkers`

Implementation:

- added a persistent worker pool for the existing parallel mid-voxel pump path in `SparseClipmap`;
- the pool is only active when parallel mid voxel pump is already enabled;
- the frame still waits for the generated batch before apply, so visible ownership is not deferred;
- default behavior is unchanged.

Build/test after implementation:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim8192_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim4096_walk_probe_20260605`

Rows:

| Candidate | Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| persistent mid workers | fixed f384 | `20.88` | `18.59` | `3.73` | `14.07` | `0.79` | `0.00` | `6.07` | `0/0` | partial keep |
| persistent mid workers | walk f600 | `79.36` | `39.97` | `16.56` | `15.27` | `8.12` | `5.95` | n/a | `0/0` | CPU/clip better, raw noisy |
| persistent mid workers | high-alt f360 | `48.43` | `28.12` | `8.37` | `4.87` | `14.87` | `5.04` | `8.59` | `0/0` | partial keep |
| persistent mid workers + interest interval 2 | fixed f384 | `21.09` | `18.86` | `3.80` | `14.12` | `0.94` | `0.00` | `6.07` | `0/0` | mixed |
| persistent mid workers + interest interval 2 | walk f600 | `63.63` | `38.51` | `13.82` | `12.34` | `12.33` | `10.01` | `15.67` | `0/0` | mixed |
| persistent mid workers + interest interval 2 | high-alt f360 | `49.54` | `19.61` | `7.96` | `4.81` | `6.83` | `4.55` | `9.99` | `0/0` | CPU/clip win, raw not solved |
| trim scan budget 8192 | walk f600 | `67.32` | `39.04` | `10.79` | `13.19` | `15.03` | `12.66` | `15.58` | `0/0` | rejected |
| trim scan budget 4096 | walk f600 | `62.31` | `37.77` | `14.02` | `16.84` | `6.89` | `5.02` | `9.07` | `0/0` | probe only |

Decision:

- persistent mid workers remain a default-off partial candidate.
- interest interval `2` is not a default or final stack setting.
- smaller pressure-trim scan budgets reduced scanned records and `pressureTrimMs`, but shifted cost into terrain-critical hierarchy/generation/clip; this branch is not a stack fix.
- no candidate is near 60 FPS; the top remaining work is now a real lane-aware generation/surface/publish architecture rather than another scalar budget.

### Post-Compaction Branch Rejections - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_coord_batch_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_miss_feedback_queued_only_candidate_20260605`

Exact coordinate batch generation:

- temporary branch batched hidden-exact feedback coordinates.
- fixed f384: raw `22.92`, CPU `18.51`, gen `13.22`, surface extract `24.73`, GPU `5.55`.
- walk f600: raw `89.44`, CPU `34.29`, gen `12.55`, clip `6.53`, GPU `5.41`.
- high-alt f360: raw `50.61`, CPU `27.30`, clip `14.50`, GPU `10.80`.
- rejected and removed because fixed generation did not improve, surface extraction got worse, and walk raw regressed.

Pressure trim miss-feedback queued-only:

- temporary branch skipped resident pressure-trim scans when miss feedback was the only pressure source and free pages were healthy.
- fixed f384: raw `25.12`, CPU `20.38`, gen `14.74`, GPU `6.59`.
- walk f600: raw `64.14`, CPU `41.85`, request `11.42`, clip `16.87`, GPU `11.49`.
- high-alt f360: raw `49.43`, CPU `26.72`, clip `14.12`, GPU `10.24`.
- rejected and removed because the local trim win did not become a safe frame-time win.

Current conclusion:

- no new accepted stack member from these branches.
- no default behavior changed.
- the next useful code branch should be a small ownership-lane streaming state-machine slice, not another isolated generation/trim/surface knob.

### Upload Lane Budget Candidate - Rejected and Removed - 2026-06-05

Attempted a default-off lane-aware upload queue budget:

- budgeted cache/prefetch upload lanes after protected visible/terrain-critical/exact uploads
- added a high-alt opt-out after the first high-alt regression
- parsed active/opt-out state for validation

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_32_32_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_4_8_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_8_16_highalt_optout_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/post_upload_guard_control_20260605`

Rows:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| upload budget 8/16 | `22.12/18.42` | `66.65/36.50` | `52.95/29.63` | rejected; high-alt window regressed |
| upload budget 32/32 | `24.31/19.17` | `83.18/33.37` | `87.22/52.74` | rejected; high-alt/walk raw regression |
| upload budget 4/8 | `21.72/19.21` | `68.37/47.91` | `50.32/30.18` | rejected; not better than same-code control |
| same-code control | `22.99/18.64` | `66.81/47.72` | `57.33/29.83` | control |
| 8/16 high-alt opt-out | `24.66/19.68` | `63.53/49.63` | `50.60/34.89` | rejected; target raw moved, but CPU/window did not |

Cleanup:

- removed `VENPOD_SPARSE_UPLOAD_LANE_BUDGETS`
- removed `PopBestUploadForLane`
- removed the harness switches and parser columns
- `.\build.ps1 -Config Release` passed
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`

Decision:

- upload queue selection alone is too local and unstable.
- next work must carry ownership lanes through request, generation, upload/apply, surface extraction, and page publication as one state-machine slice.

### Exact Local Column Block Candidate - Rejected and Removed - 2026-06-05

Tried a behavior-preserving exact-generation micro-optimization:

- temporary flag: `VENPOD_SPARSE_EXACT_LOCAL_COLUMN_BLOCK_GENERATION=1`
- temporary harness switch: `-ExactLocalColumnBlockGeneration`
- idea: replace the per-brick unordered terrain-column cache with a fixed local height/relief block for exact brick generation.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_candidate_20260605`

Rows:

| Scenario | Control raw/CPU/request/gen/clip | Candidate raw/CPU/request/gen/clip | Decision |
|---|---:|---:|---|
| fixed f384 | `21.41/19.13/4.26/14.11/0.75` | `19.89/22.18/3.93/17.41/0.83` | rejected; CPU/gen worsened |
| walk f600 | `55.66/36.42/10.36/11.17/14.88` | `58.70/52.03/16.18/18.20/17.64` | rejected; hard CPU regression |
| high-alt f360 | `44.66/22.17/8.12/4.55/9.49` | `46.16/25.39/8.16/6.64/10.58` | rejected; CPU/gen/raw regressed |

Decision:

- removed the flag, harness switch, and local-block implementation.
- `.\build.ps1 -Config Release` passed after cleanup.
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.
- no local-block symbols remain.
- retained only behavior-neutral `GenerateExactBrickForConfig(...)` helper centralization.

Interpretation:

- exact generation is not fixed by swapping the local terrain-column cache implementation.
- the remaining blocker is still the ownership-aware streaming pipeline: visible/public work needs either faster same-frame completion or a valid owner/readiness state that permits deferral. Unknown fallback remains critical.

### Formal Streaming State-Machine Direction - 2026-06-05

The code already carries partial lane data through request, generation, upload, surface extraction, and publish stats. That is not enough. The missing piece is a persistent per-brick work ticket that owns the full lifecycle.

Current lifecycle:

1. `main_launcher.cpp` submits/touches requests with residency and streaming lane.
2. `SparseVoxelWorld::GenerateQueuedBrick` and the parallel exact helpers generate payloads.
3. `SparseVoxelWorld::ApplyGeneratedBrickPayload` marks generated, queues upload, and queues surface extraction immediately.
4. Upload, surface extraction, and page publish run through separate local queues.
5. Public ownership depends on the combination of stages that happen to finish in time.

Why the local fixes failed:

- upload lane budgets optimized one queue and regressed high-alt/window behavior.
- surface caps/workers either created backlog or moved CPU into clip/gen/request.
- exact batching/local-column generation did not reduce generation enough and often worsened CPU.
- generated-lane accounting showed the expensive work is mostly `Visible`/`PublicCritical`.
- fallback classifiers showed sampled walk debt has no safe async subset.

Next implementation slice:

- add `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0`.
- create a ticket per demanded brick with:
  - ownership class: public critical, sampled visible, hidden repair, cache, prefetch, fallback valid, unknown critical;
  - required and completed stage masks: CPU generated, upload, surface, publish;
  - fallback proof and edit revision;
  - deadline/request/sample frames and backlog age.
- shadow-log the tickets first with no behavior change.
- then route queue pop order through tickets while preserving current functions.
- finally allow async/budgeted generation only for cache/prefetch/fallback-valid tickets.

The first coded step should be ticket-only shadow mode plus logs. The second step should schedule existing queues from tickets. The third step should add async/apply only for safe lanes. This is the narrowest implementation path that still addresses the architecture bug.

### Streaming Work Ticket Shadow V1 - 2026-06-05

Implemented the first coded state-machine slice:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0` default-off
- `perf_noncapture_smoke.ps1 -StreamingTicketScheduler`
- `PERF_SPARSE_STREAMING_TICKETS`

What the code records when enabled:

- per-brick shadow ticket keyed by coord;
- ownership class: public critical, sampled visible, hidden repair, cache, prefetch, fallback valid, unknown critical;
- required/completed stage mask: CPU generated, GPU uploaded, surface ready, page published;
- request/touch/update frame and edit revision;
- ticket removal on completion or eviction.

Validation:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_shadow_v1_20260605`

Rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | GPU | Ticket state |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `382` | `26.51` | `5.76` | `3.33` | `1.61` | `0.82` | `0.00` | `6.60` | active `796`, all prefetch pending CPU |
| walk realtime | `600` | `69.96` | `51.41` | `13.95` | `12.78` | `24.66` | `10.90` | `17.29` | active `4889`; sampled/prefetch `1213/3676`; pending CPU/upload/surface/publish `3228/1661/62/1661` |
| high-alt | `360` | `44.12` | `23.85` | `8.64` | `5.02` | `10.18` | `0.00` | `9.28` | active `6110`; public/sampled/prefetch `8/15/6087`; pending CPU `6110` |

Decision:

- keep as default-off shadow state.
- this is not yet a speedup.
- it gives the missing cross-stage lifecycle view needed to stop optimizing generation/upload/surface/publish independently.

Next code step:

- protected ticket scheduling mode:
  - queue pop order driven by ticket ownership;
  - public/sampled/unknown protected first;
  - cache/prefetch budgeted and age-tracked;
  - surface and publish use the same ticket ownership;
  - async remains limited to cache/prefetch/fallback-valid tickets.

### Protected Ticket Scheduling Continuation - 2026-06-05

Implemented the second coded ticket-scheduler slice:

- `VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING=0` default-off
- `perf_noncapture_smoke.ps1 -StreamingTicketProtectedScheduling`
- `PERF_SPARSE_STREAMING_TICKETS` logs `protected` and `protectedSorts`

Behavior:

- active only when `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=1`;
- generation/upload/surface queues can sort by persistent ticket ownership and required stage;
- public critical, unknown critical, sampled visible, hidden repair, and fallback-valid tickets outrank cache/prefetch;
- refined implementation skips protected sorting for cache/prefetch-only queues;
- no queue work is dropped, no unknown fallback is treated as safe, and no default changed.

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- `git diff --check` on touched tracked files: only existing LF-to-CRLF warnings

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_refined_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_budget32_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_gen32_only_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/gen32_incremental_trim_candidate_20260605`

Window table:

| Candidate | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Verdict |
|---|---:|---:|---:|---|
| ticket shadow control | `24.53/54.92` | `68.40/102.26` | `55.68/67.97` | control |
| protected ticket scheduling | `27.43/57.65` | `63.97/79.15` | `46.55/59.26` | mixed; fixed churn |
| refined protected scheduling | `24.89/58.54` | `66.25/80.41` | `45.75/58.99` | partial keep |
| hidden exact budgets `32/32/32/64` | `24.41/39.89` | `70.56/121.00` | `51.80/80.20` | rejected |
| hidden exact generation `32` only | `22.73/37.38` | `64.35/84.00` | `49.87/64.85` | mixed |
| gen32 + incremental pressure trim | `25.26/44.30` | `72.25/89.51` | `57.48/84.05` | rejected |

Interpretation:

- protected ticket scheduling is a real partial: walk tail and high-alt improve versus ticket shadow control, while fixed is roughly preserved after the prefetch/cache-only guard.
- hidden-exact generation-only pacing proves fixed hidden-exact repair is a large burst source, but the stack is not globally valid because high-alt regresses.
- full hidden-exact budget throttling and incremental trim are rejected.
- the renderer is still not playable: representative walk remains around mid-60 ms raw and high-alt around mid-40 ms raw.

Next measured implementation direction:

- ticket-controlled apply/upload/surface/publish pacing by ownership lane;
- keep public/sampled/unknown protected;
- stage hidden repair after public/sampled work;
- keep cache/prefetch/fallback-valid budgeted and age-tracked;
- do not use another isolated scalar cap as the next main branch.

### Streaming Ticket Stage Pacing Rejected - 2026-06-05

Attempted a page-publish stage-pacing candidate and removed it. The branch added a ticket-priority ready-pop path to `SparsePagePublishQueue`, an active/high-alt-gated runtime flag, per-frame counters, a `PERF_SPARSE_STREAMING_TICKET_STAGE_PACING` log line, and harness switches. All of those were removed after validation.

Artifacts:

- broad candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_candidate_20260605`
- same-code control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_control_20260605`
- high-alt-only probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_highalt_only_candidate_20260605`

Rows:

| Row | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| same-code control | `23.88/53.43` | `83.12/108.59` | `65.15/93.08` | control |
| broad stage pacing | `26.77/62.45` | `86.76/206.33` | `54.86/83.41` | rejected |

The broad candidate helped high-alt but regressed fixed and walk windows; walk max raw spiked to `206.33 ms`. The high-alt-only variant also failed: the parser missed the exact `PERF frame=400` row, but `PERF_FRAME_END` logs show late high-alt raw outliers at frames `403-407`: `87.09`, `90.58`, `75.97`, `87.04`, `107.13`.

Post-removal status:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- `git diff --check` on cleanup files: only LF-to-CRLF warnings

Decision:

- do not reintroduce stage-pacing publish selection.
- isolated page publish ordering is not the fix.
- latest same-code control indicates the next blocker is walk realtime frame `600`: raw/CPU `83.47/65.82`, request/gen/clip/pump `18.98/16.92/29.90/27.55`, with miss/unsafe still `0/0` but frame time unacceptable.

### Request Accounting Split and Pressure-Trim Guard Rejection - 2026-06-05

Added and retained a behavior-neutral request-detail split:

- `hierarchyOtherMs`
- `preHierarchyMs`
- true residual `otherMs`
- old residual `legacyOtherMs`
- `pressureTrimPressure=free/gen/miss`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_accounting_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_queued_scan_pool_guard_candidate_20260605`

Baseline result:

| Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface Extract/Stage | GPU | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f380 | `36.34/11.83` | `3.94` | `6.85` | `1.04/0.00` | `6.04/2.21` | `7.83` | raw noisy |
| walk f600 | `91.48/62.52` | `28.51` | `25.28` | `8.70/6.03` | `14.41/3.82` | `23.75` | pressure trim `9.31`, terrain critical `6.67`, stats `3.75`, hidden exact `3.63`, true other `3.88` |
| high-alt f400 | `511.69/41.06` | `13.67` | `10.94` | `16.43/6.27` | `6.84/2.38` | `15.62` | raw frame-gap outlier |

Rejected and removed:

- `VENPOD_SPARSE_PRESSURE_TRIM_QUEUED_SCAN_REQUIRES_POOL_PRESSURE`
- harness `-PressureTrimQueuedScanPoolGuard`

Candidate result:

| Scenario | Raw/CPU | Request | Gen | Clip/Pump | GPU | Decision |
|---|---:|---:|---:|---:|---:|---|
| fixed f380 | `33.58/9.40` | `2.82` | `5.69` | `0.88/0.00` | `9.21` | no representative win |
| walk f600 | `111.69/60.04` | `22.94` | `29.10` | `7.97/5.93` | `24.94` | rejected |
| high-alt f400 | `69.53/34.53` | `11.08` | `8.63` | `14.80/5.83` | `15.51` | rejected |

Reason:

- the candidate was designed to skip generation-only queued trim scans, but the bad frames reported miss-feedback pressure (`pressureTrimPressure=0/0/1`), so the scan remained active.
- walk raw and generation regressed.
- the guard was removed; only the accounting remains.

Next measured branch:

- hidden-exact/miss-feedback request admission and generation/surface coupling are now better targets than queued-trim gating.

## Async Low-Priority Apply / Persistent Mid Validation - 2026-06-05

Purpose:

- test whether exact async low-priority apply pacing can smooth the candidate stack without starving visible/public work.
- validate persistent mid workers as part of the same default-off stack.

Retained code:

- `VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME=0`
- harness parameter `-AsyncExactLowPriorityMaxApplyPerFrame`
- `PERF_SPARSE_EXACT_ASYNC lowPriorityMaxApply=... deferredLowPriority=...`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Rows:

| Candidate | Scenario | Raw/CPU | Request | Gen | Clip/Pump | GPU | Avg/Max Raw | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---|
| control | fixed | `22.30/18.53` | `4.23` | `13.58` | `0.71/0.00` | `6.61` | `22.51/53.25` | control |
| control | walk | `63.23/41.15` | `17.26` | `16.77` | `7.10/5.57` | `13.53` | `85.93/624.57` | control |
| control | high-alt | `97.26/62.90` | `16.33` | `11.87` | `34.67/20.92` | `20.85` | `93.93/117.04` | control |
| cap8 | fixed | `23.72/22.42` | `4.59` | `17.06` | `0.75/0.00` | `7.54` | `23.76/53.87` | rejected; `504` deferred completions |
| cap8 | walk | `49.35/43.71` | `17.16` | `19.07` | `7.46/5.56` | `20.87` | `69.72/96.42` | rejected |
| cap8 | high-alt | `65.54/38.04` | `8.96` | `6.72` | `22.35/12.80` | `11.10` | `58.57/73.27` | rejected |
| cap32 | fixed | `24.85/18.53` | `4.38` | `13.37` | `0.78/0.00` | `5.87` | `23.33/52.65` | partial |
| cap32 | walk | `55.92/48.01` | `12.61` | `12.78` | `22.60/11.36` | `16.81` | `65.01/78.52` | partial |
| cap32 | high-alt | `56.24/37.21` | `8.27` | `6.72` | `22.21/12.87` | `13.58` | `55.64/75.46` | partial |
| cap32 + persistent mid | fixed | `23.49/19.26` | `4.61` | `13.93` | `0.72/0.00` | `6.12` | `23.00/51.02` | strongest partial |
| cap32 + persistent mid | walk | `52.74/43.95` | `12.24` | `12.62` | `19.07/8.25` | `9.57` | `60.90/71.20` | strongest partial |
| cap32 + persistent mid | high-alt | `58.03/34.17` | `10.24` | `7.65` | `16.28/6.11` | `11.43` | `56.00/63.54` | strongest partial |
| cap32 + persistent mid + exact2 | walk | `63.71/51.25` | `18.39` | `16.47` | `16.37/4.14` | `15.05` | `72.74/106.83` | rejected |

Visual:

- walk contact sheet: coherent enough for continued investigation, still coarse/blocky.
- high-alt contact sheet: bright white shoreline/terrain artifact remains.

Decision:

- the campaign has a stronger partial default-off candidate, not a playable result.
- do not promote any defaults.
- do not continue with cap-only work; remaining work needs high-alt ownership/shoreline correction and streaming architecture cleanup.

### Streaming Playability Hidden Exact / Cache Defer Follow-Up - 2026-06-05

Build/test:

- Release build passed.
- `ctest` passed `1/1`.

Artifacts:

- hidden repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_repair_lane_candidate_20260605`
- water repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_repair_lane_candidate_20260605`
- cache-only perf: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/cache_only_defer_candidate_20260605`
- cache-only visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_20260605/contact_sheet.png`

New default-off knobs:

- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE=0`

Hidden exact repair lane:

- non-water post-open hidden repair split is behaviorally useful but ineffective for current rows because accepted feedback is mostly water feedback.
- water-inclusive repair lane reduces walk CPU but moves pressure into upload/post-wait and large hidden repair debt.
- water-inclusive walk f600: raw/CPU `85.91/31.75`, request/gen/clip/pump `15.59/7.38/8.76/6.67`, GPU `14.32`, but `hiddenExactMissing=3068/0`, upload lane prefetch `886`, post-wait `31.83`.
- both hidden repair knobs remain diagnostic/default-off only.

Cache-only defer:

- `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` remains default-off.
- high-alt f400: raw/CPU `54.59/22.30`, request/gen/clip/pump `8.34/7.02/6.94/0.01`, GPU `12.39`.
- high-alt scheduler state: `cacheOnlyDefer=1867`, visible-critical coverage `99`, cache coverage `79`, parent-held failure `0`, miss/unsafe `0/0`.
- fixed path is unchanged because missing mid debt is zero.
- walk f600 remains projected-visible and cache-only defer stays inactive: raw/CPU `69.45/52.64`, request/gen/clip/pump `18.85/16.87/16.89/4.54`, surface extract/stage `12.51/3.00`, backlog voxel `432`, max age `280`.

Visual:

- high-alt cache-only contact sheet still shows the large pale lower-screen terrain/shore band.
- numeric miss/unsafe `0/0` is not enough to call it visually correct.

Decision:

- no validated 60 fps candidate exists.
- all public defaults remain unchanged.
- high-alt over-broad cache debt has a safe default-off reduction path, but high-alt visual ownership remains unresolved.
- walk/realtime debt is projected-visible and cannot be safely deferred without ownership proof.
- next safe branch is high-alt visual ownership/shoreline contract, or request/generation/surface coupling for projected-visible walk debt under the ticket/state-machine model.

### High-Alt Background Split Exact-Band Ownership Finding - 2026-06-05

New artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_owner55_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_material54_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_badpixel_20260605/bad_pixels.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode58_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode57_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_surfacefill_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_candidate_20260605`

The high-alt pale lower-screen terrain/shore band has a more specific root now. It is not primarily caused by cache-only defer or mid-clipmap pump scheduling. Background reason mode `57` showed the lower band dominated by `far_svo_unavailable_or_rejected`, while the sampled lower-band rays are downward and cross the water plane at terrain/water distances. That means the pale band is sky-like fallback where a terrain/water/background owner is expected.

The key code path is the split-background exact-band gate:

- `Renderer.cpp` clears sparse-near flag bit `8` during background split unless `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1`.
- `PS_Raymarch.hlsl` reads that bit as `sparseSurfaceRaymarchFill`.
- `BackgroundHitAllowedByExactNear` only lets mid/Far-SVO/far-water own inside the exact/surface ownership radius when that bit is set.
- when it is off, unstenciled exact-band pixels in the lower-res background pass can reject lower owners and fall through to sky-like output.

Probe result:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` visually removed the high-alt lower pale band.
- frame `400` sky-like percentage dropped from about `41.18%` to about `20.51%`.
- frame `400` layer mix moved toward plausible water/background ownership: far water about `8.2461%`, water context about `12.2994%`, sky about `2.4502%`.

Safety result:

- this is not a validated fix.
- high-alt frame `400` with surface fill reported `shaderUnsafeContractNonReady=198`.
- walk frame `600` with surface fill reported `shaderUnsafeContractNonReady=83`.
- walk frame `600` also stayed far from playable: raw/CPU about `62.13/49.03`, request/gen/clip/pump `18.77/13.77/16.46/6.01`, GPU about `16.94`.

Decision:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` remains diagnostic/default-off.
- do not promote it as a playable fix while shader-unsafe exact-contract debt remains.
- the next high-alt implementation branch should be a narrow validated background-fill ownership path:
  - prove deterministic water/Far-SVO ownership for those split-fill pixels, or
  - improve exact foreground/water readiness so the exact band is not exposed incomplete, or
  - admit only CPU/shader-proved deterministic water/Far-SVO and keep unknown fallback blocked.
- do not keep attacking the high-alt pale band with pump caps, cache-only scheduling, stage pacing, pressure-trim gating, or hidden-exact water-lane promotion.

## Surface-Fill Exact Repair / Cached Water Proof - 2026-06-05

Implemented a default-off surface-fill safety branch:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR=0`
- deterministic water proof for exact-band shader-unsafe samples
- proof cache keyed by brick and invalidated on edit revision

Results:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| uncached high-alt f480 | `97.66/72.94` | `49.46` | `14.78` | `8.69/0.00` | `30.42` | `116.30/423.04` | rejected; proof CPU spike |
| cached high-alt f480 | `57.03/32.60` | `16.95` | `8.98` | `6.65/0.00` | `18.70` | `63.14/88.16` | useful partial |
| cached walk f600 | `77.12/185.37` | `142.32` | `21.02` | `22.00/9.64` | `26.69` | `86.70/219.96` | rejected movement stack |
| cached walk + hidden budget f600 | `122.00/46.32` | `22.82` | `16.77` | `6.70/4.18` | `39.97` | `158.98/902.15` | rejected |

The cache fixed the repeated deterministic-water proof hotspot: high-alt f480 owner feedback dropped `32.09 ms -> 4.12 ms`. The stack still is not playable. It keeps high-alt exact contract debt small by f480, but walk exact repair collides with hidden-exact debt and remains far over budget.

Decision: keep all knobs default-off. Do not promote surface fill or exact repair. The next branch must either validate the cached high-alt visual path and reduce residual CPU/GPU, or return to projected-visible walk streaming debt.

## High-Alt-Only Surface Fill / Incremental Pressure Trim - 2026-06-05

Implemented a default-off high-alt-only activation path for background-pass surface fill:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY=0`
- `perf_noncapture_smoke.ps1 -BackgroundPassSurfaceFillHighAltOnly`

This separates requested water proof / exact repair from per-frame active surface fill. Normal walk no longer inherits high-alt surface-fill exact repair.

Key rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| high-alt-only walk f600 | `65.39/50.02` | `15.03` | `13.49` | `21.48/9.64` | `15.37` | `71.44/86.49` | gate works; no repair collision |
| pressure-trim fixed f384 | `25.18/19.28` | `5.04` | `13.38` | `0.84/0.00` | `8.21` | `27.40/60.28` | partial |
| pressure-trim walk f600 | `63.49/33.26` | `9.16` | `17.66` | `6.42/4.16` | `17.44` | `69.16/81.81` | partial |
| pressure-trim high-alt f480 | `55.10/24.35` | `7.99` | `9.38` | `6.98/0.00` | `17.57` | `57.96/75.45` | partial, visual plausible |

Rejected follow-ups:

- surface-parallel extraction regressed walk CPU/generation/request.
- terrain-critical parallel generation did not activate useful work at the target frame and regressed raw/window.
- background scale `0.25` lowered GPU but regressed CPU/clip/raw.

Visual:

- combined contact sheet: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- high-alt accepted stack: plausible, old pale lower-screen band absent in this sample
- walk accepted stack: no white terrain/shore holes in the sampled frame, still coarse/low-detail

Decision:

- no 60 FPS candidate yet.
- keep all knobs default-off.
- current strongest stack is safe enough to carry as a candidate for further engineering, but not to promote.
- remaining bottleneck is distributed, not a single cap: fixed generation/surface/post-wait; walk generation/surface/GPU/post-wait; high-alt GPU/post-wait/residual generation/surface.
## Streaming Playability Real Fix Campaign - 2026-06-05 Continuation

This continuation specifically tested whether the remaining playability issue could still be handled by local queue/scheduler/trim fixes. It could not.

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/playable_candidate_table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/contact_sheet.png`

Validated current stack, still not playable:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip/Pump | Surface extract/stage | GPU ray |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `23.80` | `22.31` | `4.50` | `17.00` | `0.80/0.00` | `20.87/2.27` | `6.04` |
| walk realtime | `600` | `59.51` | `35.60` | `15.40` | `14.67` | `5.51/4.01` | `11.32/2.74` | `12.95` |
| high-alt | `398` | `51.66` | `26.21` | `4.94` | `5.18` | `16.09/9.74` | `3.78/1.79` | `10.25` |

Visual/contract checks:

- fixed accepted-stack capture passed.
- walk accepted-stack capture passed terrain-critical readiness at frame `600`.
- high-alt accepted-stack capture passed terrain-critical readiness at frame `360`, but the known bright shoreline/terrain artifact remains visible in the contact sheet.
- incremental pressure trim at scan budget `4096` failed the walk terrain-critical readiness check:
  - frame `604`: postMissing `3`
  - frame `606`: postMissing `7`

Rejected branches in this continuation:

| Branch | Why rejected |
|---|---|
| persistent terrain column cache | worsened fixed/walk target rows despite cache hits |
| async exact prefetch lane | no target-row async work applied because current classification still collapsed work into visible class |
| request fast resident touch | did not reduce target request and worsened fixed/high-alt |
| parallel exact generation | slight fixed-only win; walk/high-alt not improved |
| explicit lanes + async prefetch | exposed huge prefetch lane debt and helped high-alt target, but walk CPU/clip/backlog regressed |
| explicit source lanes only | walk regressed badly |
| incremental pressure trim `4096` | perf win, but failed walk terrain-critical readiness |
| incremental pressure trim `16384` | did not preserve useful perf win |
| pressure trim free-page guard | worsened fixed/high-alt and increased generation/backlog pressure |
| streaming lane queue priority | fixed target improved slightly, but walk/high-alt CPU/clip regressed |
| protected streaming ticket scheduler | raw/window regression in walk/high-alt |

Interpretation:

The engine is not failing because one queue is sorted wrong. The measured state is that source lanes can reveal broad prefetch/cache debt, but the work is still scheduled through visible-class generation/upload/surface paths. That is why async prefetch has no safe useful work in the current rows and why explicit lanes without full class separation regresses movement.

Required next architecture slice:

- add an ownership-critical class distinct from source/prefetch lane.
- carry it through request planning, CPU generation, GPU upload/apply, surface extraction, and publish.
- visible sampled fallback-invalid/unknown bricks remain critical.
- cache/prefetch-only work drains opportunistically and may be async.
- surface extraction and upload need their own critical/prefetch queues and budgets.
- every perf win must pass terrain-critical capture validation, not just `miss=0` / `unsafe=0`.

### Ownership-Class Split Follow-Up - 2026-06-05

Patch:

- added default-off `VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS=0`
- wired `perf_noncapture_smoke.ps1 -PrefetchLaneSpeculativeClass`
- added `SparseVoxelWorldConfig::prefetchLaneSpeculativeClass`
- in `SparseVoxelWorld::TouchResidencyClassWithStreamingLane*`, `Visible + Prefetch` becomes `Speculative + Prefetch` when enabled
- `PERF_SPARSE_STREAMING_LANES` now includes `prefetchSpeculativeClassActive` and `prefetchSpeculativeTouches`

Why:

Previous logs showed `genLane` was mostly prefetch but `genClass` stayed visible. This patch moved the classification boundary into `SparseVoxelWorld`, so prefetch-source requests can enter speculative-class queues without treating sampled/critical unknown fallback as safe.

Results:

| Candidate | Row | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---|
| class split | fixed f380 | `37.58/5.71` | `1.86` | `3.05` | `0.77/0.00` | `1.84/1.52` | `6.95` | CPU lower, raw/post-wait worse |
| class split | walk f600 | `57.04/39.98` | `10.16` | `9.66` | `20.13/9.28` | `11.13/2.52` | `10.61` | walk CPU/clip worse than accepted |
| class split | high-alt f360 | `57.19/31.57` | `8.79` | `7.14` | `15.62/6.22` | `3.89/1.88` | `10.88` | high-alt worse |
| class split + async | fixed f384 | `20.56/20.82` | `4.54` | `15.24` | `1.02/0.00` | `20.50/2.40` | `6.41` | fixed target partial |
| class split + async | walk f600 | `62.18/44.70` | `12.00` | `12.33` | `20.35/9.41` | `11.52/2.62` | `6.01` | rejected; result backlog `73`, deferred `73` |
| class split + async | high-alt f360 | `51.29/29.28` | `7.54` | `6.62` | `15.11/5.86` | `5.79/1.65` | `8.97` | result backlog `185`, deferred `176` |

Decision:

- this is not a playable candidate.
- async is no longer a no-op for prefetch, but without upload/apply/surface budgets it creates result debt and does not fix walk.
- next safe slice is stage-specific ownership queues: critical upload/surface first, prefetch upload/surface budgeted separately, with terrain-critical capture validation.

## Surface Lane Stage / Protected Ticket Rejection - 2026-06-05

After compaction, the campaign continued with the stage-specific lane hypothesis. Build/test after cleanup passed:

- `.\build.ps1 -Config Release`
- `ctest --test-dir build --output-on-failure -C Release`, `1/1`

The lane-stage prototype was removed after validation. No `sparseSurfaceLaneStage`, `SURFACE_LANE_STAGE`, `SurfaceLaneStage`, `PumpSurfaceExtractionAroundTimedForClassAndLane`, or `PopFirstQueuedBrickOfStreamingLane` symbols remain.

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_zero_prefetch_probe`
- `build/captures/streaming_playability_real_fix_campaign_20260605/streaming_ticket_protected_walk_probe`

Rows:

| Branch | Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---|
| lane-stage budgets | fixed f384 | `18.77/19.47` | `4.18` | `14.49` | `0.78/0.00` | `19.52/1.99` | `5.75` | fixed-only partial |
| lane-stage budgets | walk f600 | `65.98/35.18` | `11.29` | `12.33` | `11.55/9.57` | `11.45/2.81` | `9.57` | rejected; raw/window worse than accepted stack |
| lane-stage budgets | high-alt f360 | `49.08/29.22` | `8.00` | `5.91` | `15.30/6.06` | `5.33/1.53` | `6.72` | partial only |
| zero-prefetch lane stage | walk f600 | `56.48/36.49` | `15.11` | `14.14` | `7.22/5.56` | `12.46/2.62` | `12.55` | rejected; visible surface work remains and window avg raw `60.96 ms` |
| protected ticket probe | walk f600 | `62.21/47.56` | `12.37` | `11.91` | `23.25/11.40` | `11.78/2.66` | `13.84` | rejected; clip/CPU worsened |

Interpretation:

- source lanes, publish lanes, upload lane fields, tickets, and queue sorting are present but insufficient.
- the effective streaming state still collapses sampled-visible or visible-class debt into frame-critical generation/surface/clip/publish work.
- surface/ticket caps shift debt into catch-up, gap, post-wait, or readiness failures.
- next implementation must be a coherent ownership-critical state machine, not another local queue tweak.
### Ownership-Stage Queue Branch - 2026-06-05

Default-off candidate implemented:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS=0`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET=8`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET=8`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS`

Validation artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_explicit_lanes_candidate`

Rows:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| ownership queues | `44.05/14.51` | `117.65/79.63` | `98.90/60.35` | rejected |
| ownership queues + explicit lanes | `48.94/12.13` | `90.49/80.23` | `84.48/49.17` | rejected |

Interpretation:

- Stage-budgeting by ownership is not enough because the current request/generation pipeline still labels nearly all useful staged work as visible/critical.
- Explicit source lanes expose prefetch queues, but the combined stack regresses and does not approach playability.
- The next fix must move earlier in the pipeline: request/admission must carry a durable ownership-critical state before generation/upload/surface/publish scheduling can help.
