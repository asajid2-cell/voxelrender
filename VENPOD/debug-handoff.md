# VENPOD Debug Handoff

Generated: 2026-06-02

Canonical session memory file. Update this file after each meaningful debugging or optimization pass. Treat `root.md`, `debug.md`, and `handoff.md` as source/reference docs; this file is the current consolidated handoff for surviving chat compactions and handing work to other engineers/researchers.

## Active Campaign Goal - 2026-06-04

Current active goal:

```text
streaming_playability_real_fix_campaign_20260604
```

Goal:

Drive VENPOD from the current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes. Do not stop after one diagnostic, one failed heuristic, one partial win, or capture-only improvement.

Campaign handoff:

- `handoff.md`

The top of `handoff.md` now contains the current anti-stop rules, validation protocol, scoreboard, implementation slices, and immediate work order. Update it after every meaningful cycle.

Latest clean-harness validation:

- Build/test passed after cleanup:
  - `.\build.ps1 -Config Release`
  - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- Retained cleanup:
  - `perf_noncapture_smoke.ps1` no longer forces `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`
  - new explicit switch: `-MidClipmapVisibleCriticalPrepump`
  - `src/main_launcher.cpp` only runs projected missing-brick scan/priority tagging when prepump can affect high-alt, visible priority/lane guard is enabled, or missing-sample feedback is enabled
- Clean control walk f600: raw/CPU `63.51/55.81`, request/gen/clip/pump `12.94/16.70/26.15/14.31`, surface extract `11.01`, GPU `12.22`
- Current strongest clean partial is plain parallel mid pump:
  - fixed f384 raw/CPU `23.80/22.31`
  - walk realtime f600 raw/CPU `59.51/35.60`, request/gen/clip/pump `15.40/14.67/5.51/4.01`, surface extract `11.32`, GPU `12.95`
  - high-alt f398 raw/CPU `51.66/26.21`, clip/pump `16.09/9.74`
- Visual check:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605/contact_sheet.png`
  - no obvious white holes/sky leaks in walk frames `560/580/600`, but terrain remains coarse/blocky
- Rejected in the clean cycle:
  - `MidInterestInterval=2`
  - `-TerrainCriticalParallelGeneration` with parallel mid
  - `-ParallelMidWorkerColumnCache` with parallel mid as a stack member
  - high-alt projected visible-critical prepump/cache defer as a stack member
- Current blocker:
  - no validated 60 FPS candidate
  - remaining cost is distributed across request, exact generation, surface extraction, residual GPU/post-wait, and high-alt clip/pump
  - next real code slice must be ownership-lane state through request/generation/upload/surface/publish, not another isolated scalar cap

Latest validation anchor:

- `VENPOD_PERF_FRAME_END_LOG_INTERVAL` was added default-neutral; unset preserves old logging.
- `perf_noncapture_smoke.ps1` now writes `window_summary.csv` and `window_table.md`.
- Current accepted-stack baseline is `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_framewindow_20260605`.
- Target rows: fixed f384 `19.58 ms` raw / `17.89 ms` CPU; walk realtime f600 `58.94/48.30`; high-alt f360 `48.21/30.41`.
- Window rows: fixed avg raw `22.32`, walk avg raw `107.87` with a frame-601 gap outlier, high-alt avg raw `49.77`.
- Do not use `frameend_interval_validation_20260605` as the accepted baseline; it used the mixed persistent-mid/interest-2 probe stack.
- Next implementation must be chosen from the accepted-stack table and must not retry rejected scalar trim/surface/queue/thread toggles.

Latest resume cycle:

1. Durable handoff updated:
   - `handoff.md`, section `Campaign Resume - Visible Cache And Inline Surface Rejections - 2026-06-05`
2. Rejected and removed inline generated surface branch:
   - temporary `VENPOD_SPARSE_SURFACE_INLINE_GENERATED` code was removed
   - it reduced queued surface extraction but moved/cascaded cost into generation/clip/CPU
   - walk realtime regressed to `raw=96.91`, `CPU=112.71`
3. Rejected current visible-lane/cache-only combo:
   - artifact `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visible_critical_existing_stack_vsync0_20260605`
   - it deferred `3586` cache bricks indefinitely, left broad mid coverage at `61`, and drove request cost to about `41 ms`
   - this is a fake win and is not a candidate stack member
4. High-alt-only projected cache deferral remains diagnostic-only:
   - artifact `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visible_cache_highalt_only_vsync0_20260605`
   - high-alt pump dropped `5.63 -> 1.82 ms`, but raw stayed about `52 ms` and walk regressed
5. Parallel mid pump current-stack retest is not a new win:
   - artifact `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_current_stack_vsync0_20260605`
   - walk parallel pump activated (`8` bricks, `4` workers, `2.23 ms` wall), but CPU regressed to `60.51 ms`
6. Current root remains:
   - walk debt is sampled/projected-visible and fallback-invalid/unknown, so it cannot be deferred or async-generated safely
   - high-alt has unsampled cache debt, but the safe cache split is too small to move raw frame time materially
   - next real slice is ownership-aware exact/surface request/generation/publish lanes with separate budgets and async/apply accounting, not another mid-pump queue tweak

Latest campaign continuation after surface lane budget attempts:

1. Build/test after cleanup passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Durable handoff updated:
   - `handoff.md`, section `Campaign Continuation - Surface Lane Budget Rejected - 2026-06-05`
   - artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_lane_budget_rejected_20260605.md`
3. Rejected and removed branch 1:
   - hidden post-open pre-publish surface cap
   - temporary envs `VENPOD_SPARSE_PRE_PUBLISH_SURFACE_LANE_BUDGETS` and `VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_POST_OPEN_SURFACE_BUDGET`
   - result: fixed `22.37/21.65 raw/CPU`, walk `74.88/52.92`, high-alt `49.68/34.30`
   - reason: hidden cap did not solve surface cost; general surface extraction refilled the frame and walk regressed
4. Rejected and removed branch 2:
   - combined hidden post-open cap plus low-lane surface extraction budget
   - temporary envs `VENPOD_SPARSE_SURFACE_LANE_BUDGETS`, `VENPOD_SPARSE_SURFACE_LOW_LANE_MAX_PER_FRAME`, `VENPOD_SPARSE_SURFACE_HIDDEN_POST_OPEN_BUDGET`
   - result: fixed `23.86/18.56 raw/CPU`, walk `56.01/69.32`, high-alt `183.41/36.64`
   - reason: local bucket changes created catch-up/gap regressions and did not produce a playable candidate
5. Decision:
   - no symbols for these rejected flags remain in source or harness
   - do not retry hidden-only pre-publish caps, prefetch surface caps, or low-lane surface caps as the next fix
   - remaining work must cut real visible/sampled work or add ownership-proved async/apply architecture; simple caps keep shifting debt

Latest campaign continuation after publish lane propagation:

1. Build/test passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Durable handoff updated:
   - `handoff.md`, section `Campaign Continuation - Publish Lane Propagation - 2026-06-05`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/publish_lane_propagation_vsync0_20260605/table.md`
3. New retained behavior-neutral infrastructure:
   - `SparsePendingPageTablePublish::streamingLane`
   - `SparseBrickUploadPacket::streamingLane`
   - lane-aware `SparsePagePublishQueue::Enqueue(...)`
   - lane totals in `SparsePagePublishQueueStats`
   - `PERF_SPARSE_PAGE_PUBLISH_LANES`
4. What it does:
   - carries explicit streaming lane metadata into upload and page-table publish diagnostics
   - promotes edited publishes to `PublicCritical`
   - keeps duplicate queue records at the highest observed lane
   - leaves default behavior unchanged
5. Rejected probe:
   - `VENPOD_SPARSE_POST_OPEN_PREPUBLISH_SURFACE_MAX_MS=4` lowered selected raw/CPU rows but created thousands of pending publishes/surface gates
   - fixed frame `360`: `publishPending=3619`, `publishLag=139`, `publishSurfGate=435/21`
   - walk frame `600`: `publishPending=4542`, `publishLag=515`, `publishSurfGate=485/20`
   - decision: rejected as debt shifting
6. Key validation rows:
   - fixed frame `384`: raw/CPU/request/gen/clip/surface/GPU `21.09/20.59/3.93/15.92/0.72/19.52/6.45`; frame `360` publish queue lanes `0/12/0/0`
   - walk frame `600`: raw/CPU/request/gen/clip/pump/surface/GPU `74.08/49.25/18.30/17.66/13.25/10.64/13.07/18.68`; publish queue lanes `0/0/5/0`, upload lanes `0/1156/722/0`, `qsurf=0/43/0/0`
   - high-alt frame `373`: raw/CPU/request/gen/clip/pump/GPU `46.60/22.84/4.78/5.91/12.14/5.97/11.68`; generation lanes `0/6958/4/8`
7. Decision:
   - publish lane propagation is retained as behavior-neutral architecture/diagnostic infrastructure
   - it is not a playable candidate
   - representative walk publish/surface debt is visible-lane, not prefetch-lane, so prefetch publish/surface deferral is not the next safe walk fix
8. Current blocker:
   - walk remains expensive because visible sampled/unknown mid and exact/surface work cannot be deferred safely
   - next work must prove a real fallback owner for sampled missing mid bricks, reduce actual visible sampled footprint under a correct contract, or split the streaming state machine further

Latest campaign continuation after explicit source lanes:

1. Build/test passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Durable handoff updated:
   - `handoff.md`, section `Campaign Continuation - Explicit Source Lanes - 2026-06-05`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_campaign_20260605_table.md`
3. New retained default-off candidate:
   - `VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES=0`
   - harness switch `-RequestExplicitSourceLanes`
   - explicit-lane touch APIs in `SparseBrickPool` and `SparseVoxelWorld`
4. What it does:
   - preserves explicit `Prefetch` source lanes for broad view-cone and terrain-prefetch requests while leaving visible/public-critical work guarded
   - maps hierarchical `ViewCone`, flight light-mode view-cone, and terrain surface prefetch touches to `Prefetch` lane behind the flag
   - leaves behavior unchanged by default
5. Key A/B:
   - walk baseline frame `600`: raw/CPU/clip/pump `72.31/64.99/36.29/25.31`, gen lanes `0/0/3115/34`
   - walk source lanes frame `600`: raw/CPU/clip/pump `79.25/49.97/22.68/20.26`, gen lanes `0/2996/50/28`
   - high-alt baseline frame `395`: raw/CPU/clip `44.29/30.09/16.62`, gen lanes `0/0/5384/8`
   - high-alt source lanes frame `394`: raw/CPU/clip `49.18/24.55/12.81`, gen lanes `0/5431/2/8`
6. Decision:
   - explicit source lanes are retained as a safe default-off partial because the lane separation works and CPU improves in movement/high-alt
   - not a playable candidate because raw remains bad and fixed CPU slightly regressed
7. Rejected/removed:
   - prefetch-lane surface deferral; it created surface/publish backlog in the thousands and did not improve frame time
8. Rejected/not accepted in this stack:
   - parallel surface extraction regressed walk
   - parallel mid pump was mixed: it reduced clip/pump but inflated request/gen/CPU and is not a global accept
9. Current blocker:
   - lanes must persist through publish/apply/surface readiness, not just request/generation queue ordering
   - sampled visible fallback-invalid/unknown work still cannot be deferred
   - next implementation should add lane-specific publish/apply/surface budgets with visible-critical readiness guarantees

Latest campaign continuation after request/generation slice:

1. Build/test passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Durable handoff updated:
   - `handoff.md`, section `Campaign Continuation - Request Slice Rejected / Critical Generation Batch - 2026-06-04`
3. Rejected and removed:
   - default-off resident-touch cache
   - walk row worsened from raw/CPU/request `65.89/45.21/15.44` to `70.79/44.71/16.16`
   - existing fast-resident touch remains rejected as a CPU fix; it improved raw in one row but worsened CPU/clip
4. New retained default-off candidate:
   - `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
   - `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
   - `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`
   - `SparseVoxelWorld::PumpGenerationForCoordsParallel(...)`
5. Terrain-critical parallel generation result:
   - no-heavy accepted stack walk: raw/CPU/gen/clip/pump/GPU `65.89/45.21/18.19/11.55/8.91/19.91`
   - min16 parallel generation walk: raw/CPU/gen/clip/pump/GPU `51.34/46.57/15.07/16.84/5.63/17.63`
   - protected generated `19`, parallel generated `19`, parallel wall `1.06 ms`
   - decision: safe default-off partial raw win, not a playable candidate
6. V2 guard combination:
   - walk raw worsened to `71.84 ms`
   - decision: V2 remains high-alt/cache-debt-only, not part of the global walk stack
7. Current blocker:
   - no validated 60 FPS candidate
   - walk still distributed across request/gen/clip/surface/GPU
   - next real architecture slice is ownership-aware lane propagation through exact generation, surface extraction, upload/apply, and publication queues

Latest campaign continuation after visible-critical V2 / interval-2 cycle:

1. Build/test passed:
   - `.\build.ps1 -Config Release`, `ninja: no work to do`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
   - `git diff --check` on touched code/harness had no whitespace errors, only existing LF-to-CRLF warnings
2. New default-off diagnostics/prototype kept:
   - `VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS=0`
   - `PERF_SPARSE_STREAMING_LANES`
   - behavior-neutral `SparseStreamingLane { Cache, Prefetch, Visible, PublicCritical }`
   - `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2=0`
   - harness switch `-MidClipmapVisibleCoverageGuardV2`
3. Artifacts:
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
4. V2 result:
   - walk realtime frame `600`: raw `74.15`, CPU `34.44`, clip/pump `9.65/7.17`, missing `494`, sampled/unsampled `298/196`, defer `0`, `miss/unsafe=0/0`
   - high-alt frame `385`: raw `48.84`, CPU `26.00`, clip/pump `15.81/6.06`, missing `1865`, sampled/unsampled `1/1864`, defer `1864`, `miss/unsafe=0/0`
   - fixed frame `365`: raw `18.56`, CPU `11.94`, missing `0`, `miss/unsafe=0/0`
   - decision: V2 is a real default-off high-alt cache-debt fix, not a walk fix
5. `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2` result:
   - walk realtime frame `600`: raw `67.99`, CPU `40.70`, request/gen/clip/pump `15.63/16.71/8.34/6.13`, missing `429`, sampled/unsampled `239/190`, coverage `95`, `miss/unsafe=0/0`
   - decision: partial clip-interest win only, not a playable candidate
6. Rejected branches in this cycle:
   - pressure-trim free-page guard: walk raw `136.15`, CPU `56.97`, surface backlog/GPU/raw spike
   - parallel surface extraction: walk raw `106.14`, CPU `59.94`, surface/clip/GPU regression
   - parallel exact generation: walk raw `73.96`, CPU `44.57`, missing sampled `410`, frame regression
   - direct footprint columns: walk raw `101.25`, CPU `67.27`, request/gen/GPU regression
   - async exact generation: walk raw `119.41`, CPU `54.01`, async enqueued/applied `0/0`, no useful work
7. Accepted-stack all-scenario retest with interval-2/V2:
   - fixed frame `379`: raw `30.06`, CPU `18.51`, GPU `9.08`, missing `0`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `114.88`, CPU `49.00`, request/gen/clip/pump `12.05/15.62/21.30/18.79`, GPU `23.11`, missing `483`, sampled/unsampled `295/188`, `miss/unsafe=0/0`
   - high-alt frame `400`: raw `112.25`, CPU `53.42`, request/gen/clip/pump `19.69/19.11/14.61/7.49`, GPU `23.11`, missing `1877`, sampled/unsampled `4/1873`, defer `1873`, `miss/unsafe=0/0`
8. Current decision:
   - no validated 60 FPS candidate exists
   - V2 and interval-2 are safe default-off partials, not enough
   - remaining blocker is a coupled ownership-aware streaming/publication architecture gap, not a missing local cap
   - next real code slice must put public-critical/sampled-visible/cache/prefetch/maintenance lanes into request, exact generation, surface extraction, upload/apply, and publication queues with separate budgets and backlog accounting

Latest campaign continuation after exact ViewCone/cache-defer closure:

1. Build/test passed after removing the exact ViewCone no-op prototype:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Campaign artifacts updated:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
3. Exact ViewCone speculative demotion / exact async was rejected and removed:
   - walk realtime frame `600`: raw `71.90`, CPU `43.56`, request/gen/clip/pump `14.86/15.52/13.17/10.94`, GPU `14.43`, missing `438`, sampled `240`, `miss/unsafe=0/0`
   - demoted exact ViewCone requests: `0`
   - exact async enqueued/applied: `0/0`
   - high-alt frame `393`: raw `48.94`, CPU `25.67`, clip/pump `14.83/4.29`, missing `1486`, sampled `0`, exact async enqueued/applied `0/0`
4. VSync-off was not the fix:
   - walk realtime frame `600`: raw `74.62`, body `69.82`, CPU `45.68`, GPU `6.04`, `vsync=0`
5. Mid-clipmap cache-only defer was rejected as a candidate:
   - high-alt frame `404`: raw `62.14`, CPU `33.82`, clip/pump `22.22/9.37`, missing `1501`, sampled `1101`, visible-critical coverage `87`, defer count `0`
   - walk realtime frame `600`: raw `66.53`, CPU `51.54`, request/gen/clip/pump `11.91/13.41/26.20/13.26`, missing `488`, sampled `289`, defer count `0`
   - both rows preserved `miss/unsafe=0/0`, but the branch did not activate safely and walk regressed
6. Current decision:
   - no validated 60 FPS candidate exists
   - strongest default-off stack remains worker-local mid column cache
   - next implementation must be the ownership-aware streaming/publication state split, not another local queue cap/demotion

Latest campaign continuation after public-critical surface gate split probe:

1. Build/test passed after the rejected probe was removed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Artifact:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_gate_public_critical_only_candidate_20260604/`
3. Temporary probe removed:
   - `VENPOD_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL_ONLY`
   - harness switch `-SurfaceGatePublicCriticalOnly`
   - `PERF_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL`
4. Result:
   - fixed frame `360`: raw `22.30`, CPU `21.79`, request/gen/clip `4.35/16.09/1.35`, surface `18.90/1.90`, GPU `6.25`, `latePublishes=0`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `92.00`, CPU `60.95`, request/gen/clip `10.41/11.81/38.70`, pump `36.83`, surface `8.86/2.64`, GPU `17.36`, `latePublishes=0`, `criticalDefers=6`, coverage `94`, `miss/unsafe=0/0`
   - high-alt frame `360`: raw `49.84`, CPU `30.70`, request/gen/clip `7.16/5.80/17.74`, pump `5.81`, surface `3.59/1.81`, GPU `10.87`, `latePublishes=0`, `miss/unsafe=0/0`
5. Decision:
   - rejected and removed
   - there was no noncritical late-publish opportunity at target frames
   - walk regressed into clip/pump catch-up, worse than the accepted stack
   - do not retry a page-table surface gate bypass as the next fix
   - the next real fix is the explicit ownership-aware streaming state split, not another publish gate tweak

Latest campaign continuation after surface-ready publish pressure probe:

1. Build/test passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. New default-off prototype:
   - `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=0`
   - harness switch `-SurfaceReadyPublishPressure`
   - `PERF_SPARSE_SURFACE_READY_PUBLISH` now logs `pressure=0/1`
3. Code change:
   - when surface-ready publish queue and pressure are both enabled, pending surface-gated visible/collision publishes are counted as protected visible publish pressure in the sparse runtime budget scheduler
   - defaults remain unchanged
4. Artifacts:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_queue_candidate_20260604/`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_pressure_candidate_20260604/`
5. Rejected publication-queue rows:
   - strongest accepted stack walk `600`: raw `49.61`, CPU `24.04`, request/gen/clip `8.83/7.82/7.38`, pump `5.82`, surface `8.57/2.92`, GPU `12.72`, `miss/unsafe=0/0`
   - queue only walk `600`: raw `90.29`, CPU `61.98`, request/gen/clip `10.45/13.83/37.69`, pump `35.59`, surface `8.52/2.93`, GPU `16.19`, coverage `94`, `miss/unsafe=0/0`
   - queue + pressure walk `600`: raw `62.23`, CPU `43.31`, request/gen/clip `12.61/14.94/15.75`, pump `4.02`, surface `8.59/2.77`, GPU `17.22`, coverage `95`, `miss/unsafe=0/0`
6. Decision:
   - surface-ready publish pressure is default-off and rejected as a playable candidate
   - queue-only hid side-queue work from scheduler pressure; pressure accounting made that visible but still regressed the accepted stack
   - this closes local publication-queue pressure tuning as a path to 60 FPS
   - the next real implementation is the public-critical/cache-prefetch streaming and publication state split, with separate generation/surface/upload/publish lanes

Latest campaign continuation after time-budgeted surface parallel probe:

1. Build/test passed:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
   - `git diff --check` on touched code had no code whitespace errors, only existing line-ending warnings
2. New default-off prototype knobs:
   - `VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED=0`
   - `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH=32`
3. Artifact:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_surface_time_budgeted_batch32_candidate_20260604/`
4. Code evidence:
   - accepted walk row surface queue was `qsurf=0/3265/0/0`, so the representative surface debt is visible-class, not speculative/cache
   - async surface extraction for speculative/cache work would be a no-op in this row until the engine has a real public-critical/cache publication split
5. Rejected candidate:
   - fixed frame `360`: raw `21.32`, CPU `20.54`, surface `18.90/2.08`, GPU `5.36`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `76.24`, CPU `36.50`, request/gen/clip `13.67/16.20/6.62`, surface `11.75/2.97`, GPU `14.38`, `miss/unsafe=0/0`
   - high-alt frame `360`: raw `48.76`, CPU `29.78`, clip `16.52`, GPU `10.23`, `miss/unsafe=0/0`
6. Current decision:
   - time-budgeted parallel surface extraction is rejected
   - local surface caps/parallel batches are not the path to a playable candidate
   - the remaining blocker is an ownership-aware streaming/publication state split where public-critical pages remain guarded and cache/prefetch work can be generated/extracted/applied off the public path

Latest campaign continuation after request/surface micro-branches:

1. Build/test passed after default-off request/surface candidates:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
   - `git diff --check` on touched code had no code whitespace errors, only existing line-ending warnings
2. New default-off/instrumentation knobs:
   - `VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH=0`
   - `VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION=0`
   - `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS=4`
   - `VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS=4`
   - `PERF_SPARSE_CPU_DETAIL surfaceParallel=active/bricks/workers/wallMs`
3. Rejected branches:
   - fast resident touch: walk realtime frame `600` regressed to `raw=59.01`, `CPU=38.22`
   - global surface extraction max `1`: walk regressed to `raw=63.83`, `CPU=34.30`
   - parallel surface extraction uncached: walk regressed to `raw=64.81`, `CPU=36.94`
   - parallel surface extraction with worker-local cache: walk regressed to `raw=72.39`, `CPU=41.06`
4. Strongest accepted stack remains the worker-local mid column cache stack:
   - fixed `raw=23.77`, CPU `16.95`, GPU `7.97`
   - walk realtime `raw=49.61`, CPU `24.04`, GPU `12.72`
   - high-alt `raw=50.85`, CPU `30.05`, GPU `10.05`
5. Current decision:
   - no validated 60 FPS candidate exists
   - request fast touch, global surface cap, and parallel synchronous surface extraction should not be part of the playable stack
   - the next safe slice is an ownership-aware streaming/publication split with async surface extraction only for cache/prefetch or otherwise non-public-critical work, main-thread apply, and separate upload/publish budgets

Latest campaign continuation after worker-local column-cache cycle:

1. Build/test passed after the worker-local mid column cache and surface-general-budget instrumentation:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Latest updated campaign artifacts:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
3. Accepted default-off partial:
   - `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE=0`
   - harness switch `-ParallelMidWorkerColumnCache`
   - preserves synchronous visible-critical mid generation while giving each parallel pump worker its own terrain column cache
   - `PERF_SPARSE_MID_CLIPMAP_BACKLOG` now reports worker cache active/entries/hits/misses
4. Strongest continuation rows:
   - fixed frame `360`: raw `23.77`, CPU `16.95`, request/gen/clip `4.15/12.03/0.76`, GPU `7.97`, missing `0`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `49.61`, CPU `24.04`, request/gen/clip `8.83/7.82/7.38`, pump `5.82`, surface extract/stage `8.57/2.92`, GPU `12.72`, missing `420`, sampled `357`, coverage `95`, `miss/unsafe=0/0`
   - high-alt frame `360`: raw `50.85`, CPU `30.05`, request/gen/clip `7.65/6.58/15.82`, pump `4.27`, GPU `10.05`, missing `1765`, sampled `0`, coverage `100`, `miss/unsafe=0/0`
5. Rejected/default-off probes:
   - strict surface time budget: walk raw `102.71`, CPU `40.79`
   - shared mid column cache: walk CPU `44.74`, clip `28.15`, disabled parallel
   - surface sort cache: walk CPU `45.95`, zero cache hits
   - surface partial sort: walk CPU `34.35`, no stack win
   - surface general strict budget: walk CPU `31.92`, skipped general catch-up but protected surface/mid catch-up still dominated
   - mid pump `8` workers: walk CPU `36.38`
   - parallel exact generation with worker cache: no representative walk win
   - async exact generation with visible async off: queued/applied `0`, no-op
6. Current decision:
   - no validated 60 FPS candidate exists
   - Completion B remains the correct state: strongest default-off candidate stack plus exact remaining blocker table
   - worker-local column cache is a safe opt-in partial, not a playable fix
   - representative walk is now balanced across CPU `24.04`, GPU `12.72`, request `8.83`, gen `7.82`, clip/pump `7.38/5.82`, and surface `8.57/2.92`
   - next implementation must be an ownership-aware streaming/publication state split with separate budgets for request prep, visible-critical generation, cache/prefetch generation, upload/apply, and surface publication
   - all defaults remain unchanged; blunt pump cap, age-priority prototype, and failed ring-only visible-critical heuristic remain rejected

Latest campaign continuation after surface/scan cycle:

1. Build/test passed after hidden-tracked scan budgeting:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Build/test passed again after surface sort cache / partial-sort changes:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
3. Updated campaign artifacts:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
4. New default-off diagnostics/candidates:
   - `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=0`
   - `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET=512`
   - `PERF_SPARSE_HIDDEN_EXACT_TRACKED_SCAN`
   - `VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE=0`
   - `VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT=0`
   - `PERF_SPARSE_CPU_DETAIL surface=.../sort/sortHit`
5. Strongest default-off stack remains:
   - fixed frame `385`: raw `34.93`, CPU `11.71`, request/gen/clip `9.75/1.09/0.83`, GPU `5.90`, missing `0`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `46.49`, CPU `31.05`, request/gen/clip `8.15/7.00/15.87`, pump `13.33`, surface extract/stage `9.00/3.31`, GPU `14.60`, missing `385`, sampled `323`, coverage `95`, `miss/unsafe=0/0`
   - high-alt frame `396`: raw `52.25`, CPU `25.55`, request/gen/clip `6.20/6.33/13.01`, GPU `12.12`, missing `1341`, sampled `0`, coverage `99`, `miss/unsafe=0/0`
6. Rejected follow-up branches:
   - hidden tracked scan budget `512`: walk raw `56.03`; scan counts dropped but surface extraction stayed about `9 ms`
   - hidden tracked scan budget `128`: walk raw `53.88`; lower scan budget still did not reduce the surface bucket
   - surface sort cache: walk raw `48.71`; sort calls `3`, cache hits `0`
   - surface partial sort: walk raw `49.23`; small local surface reduction only
   - direct footprint path: walk raw `47.85`; coverage dropped to `94`, missing voxel rose to `485`, budget reason `2`
7. Decision:
   - campaign is at Completion B, not Completion A
   - no validated 60 FPS candidate exists
   - the strongest stack is a safe default-off partial candidate only
   - all defaults remain unchanged
   - next implementation must be an ownership-aware streaming state-machine slice, not another queue-order, scan-budget, or sort-only tweak

Latest campaign continuation after compaction:

1. Build/test passed after the budget-compatible parallel mid-pump patch:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. Latest updated campaign artifacts:
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
   - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_candidate_20260604/`
3. Code/harness changes:
   - `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=1` can now run under the backlog-aware pump budget instead of being disabled by `voxelPumpBudgetActive`
   - budgeted parallel batches are capped by `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS`
   - `m_pumpBudgetHitLastFrame` is marked when budgeted parallel pump consumes the budget and queued work remains
   - `perf_noncapture_smoke.ps1 -NoBacklogAwarePump` exists for diagnostics only
4. Best latest default-off partial stack row:
   - fixed frame `378`: raw `27.78`, CPU `3.63`, request/gen/clip `2.04/0.73/0.83`, GPU `7.33`, missing `0`, `miss/unsafe=0/0`
   - walk realtime frame `600`: raw `70.54`, CPU `35.90`, request/gen/clip `17.17/11.93/6.78`, pump `4.69`, surface extract/stage `11.97/3.28`, GPU `13.72`, missing `391`, sampled `324`, coverage `95`, `miss/unsafe=0/0`
   - high-alt frame `390`: raw `60.77`, CPU `27.59`, request/gen/clip `5.79/8.75/13.05`, pump `4.64`, GPU `16.11`, missing `1495`, sampled `0`, coverage `100`, `miss/unsafe=0/0`
5. Decision:
   - budget-compatible parallel mid pump is a safe default-off partial code candidate
   - it is not a validated 60 FPS/playable candidate
   - hidden exact generation budget `16` is rejected; it lowered coverage and increased clip/pump catch-up in walk
   - surface max `1 ms` is rejected; it moved debt into generation/fixed rows and worsened walk raw time
   - no-backlog parallel pump is diagnostic only, not the accepted safety stack
6. Current measured blocker:
   - walk remains dominated by request admission, exact generation, surface extraction/staging, residual GPU/post-wait, and sampled unknown mid debt
   - async remains blocked for sampled unknown/invalid debt because fallback-valid remains zero
   - next real slice should split exact request/generation/surface-publication into public-critical versus repair/cache lanes with separate budgets and publish readiness

Previous campaign continuation:

1. Build/test passed after cached-column worker and pressure-trim code changes:
   - `.\build.ps1 -Config Release`
   - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
2. `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md` now includes the rejected/no-op continuation rows and pressure-trim rows.
3. New default-off pressure-trim instrumentation/candidate:
   - `PERF_SPARSE_REQUEST_DETAIL` now includes `pressureTrimMs` and `preHierarchy=brush/owner/diagnostic/missFb/hiddenExact`
   - `VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL=0`
   - `VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET=32768`
   - `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=0`
4. Rejected/ruled out:
   - cached exact same-frame parallel generation: walk realtime `cpuUpdateMs=140.79`, high-alt `cpuUpdateMs=236.39`
   - existing async exact generation with cached worker: enqueued `0`; visible exact async remains unsafe without an owner proof
   - hidden-exact water request throttle: reduced hidden-exact accepted requests but caused walk clip catch-up (`clipMs=77.13`)
   - bounded64 current stack: walk realtime `cpuUpdateMs=131.54`, remains comparison-only
   - pressure-trim free-page guard: walk realtime `cpuUpdateMs=141.35`, high-alt `cpuUpdateMs=156.88`
5. Incremental pressure trim result:
   - 32k scan budget walk realtime frame `600`: raw `108.50 ms`, CPU `68.71 ms`, request/gen/clip `29.77/29.80/9.10 ms`, `miss/unsafe=0/0`
   - 16k scan budget high-alt frame `400`: raw `75.70 ms`, CPU `40.75 ms`, request/gen/clip `11.60/11.33/17.81 ms`, `miss/unsafe=0/0`
   - 16k scan budget walk realtime frame `600`: raw `117.43 ms`, CPU `69.71 ms`, request/gen/clip `32.85/30.15/6.69 ms`, `surfaceExtractMs=15.56`, `miss/unsafe=0/0`
   - decision: keep incremental pressure trim default-off as a partial diagnostic/candidate, not a playable fix
6. Current least-bad row:
   - `parallel_mid_pump_candidate` walk realtime frame `600`: `rawMs=75.97`, `cpuUpdateMs=40.80`, `request/gen/clip=18.90/15.79/6.09`, `surfaceExtractMs=11.16`, `gpuRayMs=13.45`, `miss/unsafe=0/0`
   - not an accepted patch win because `parallelPumpActive=0`
7. Current blocker:
   - exact request/generation/surface-publish pipeline around visible sampled terrain debt
   - representative walk still has mostly sampled missing mid debt and `fallbackValid=0`
   - hidden-exact accepted requests are material, but throttling the water lane is not safe enough as a performance fix
   - visible exact async is blocked until an owner/fallback proof says the deferred brick is not public-critical

Immediate first slice status:

1. `perf_noncapture_smoke.ps1` added.
2. fixed/walk/high-alt noncapture candidate rows captured in `build/captures/noncapture_playability_smoke_20260604`.
3. current first noncapture stack was not close to 60 FPS:
   - fixed frame `380`: raw `54.67 ms`, CPU sparse `19.39 ms`, GPU ray `8.44 ms`
   - walk realtime frame `600`: raw `246.81 ms`, CPU sparse `90.60 ms`, request/gen/clip `49.52/26.35/7.94 ms`, surface extract/stage `21.86/55.24 ms`, GPU ray `5.70 ms`
   - high-alt frame `400`: raw `91.11 ms`, CPU sparse `41.13 ms`, clip `18.57 ms`, GPU ray `14.64 ms`
4. surface path was improved:
   - `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=0` added default-off
   - `SparseSurfaceRangeAllocator` stats refresh batching added as behavior-preserving staging optimization
   - walk surface stage dropped from `55.24 ms` first noncapture row to `4.11 ms` in the stats-batch row
5. current consolidated opt-in stack, with large sparse pool/page table, is still not 60 FPS:
   - artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
   - fixed frame `381`: raw `28.05 ms`, CPU sparse `18.36 ms`, GPU ray `10.54 ms`
   - walk realtime frame `600`: raw `109.56 ms`, CPU sparse `74.30 ms`, request/gen/clip `20.80/16.48/36.98 ms`, pump `34.69 ms`, GPU ray `18.55 ms`
   - high-alt frame `400`: raw `54.64 ms`, CPU sparse `28.40 ms`, GPU ray `14.42 ms`
6. current measured blocker is realtime walk sampled-visible mid-clipmap pump/generation debt. Do not go back to surface metadata first unless new rows show it regressed.

Rejected/not accepted in this cycle:

- request resident fast-path lookup patch was tried and removed; it did not reliably reduce request time
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1` was retested with large pool and is not a reliable realtime walk fix
- `VENPOD_VSYNC=0` did not make the walk row playable

Do not skip noncapture validation and do not claim 60 FPS from capture-only rows.

## Source Documents Consolidated

- `root.md`: latest root-cause decision log, guarded foreground contract state, performance pivot.
- `debug.md`: long-form chat/debug rollout summary and historical evidence.
- `handoff.md`: older large VENPOD rendering handoff with prior checkpoints.
- `build/captures/perf_root_cause_20260602/perf_cost_table.md`: current cost waterfall.
- `build/captures/perf_root_cause_20260602/edit_latency_events.txt`: current edit/build latency probe.
- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_table.md`: current sparse CPU movement/high-alt bottleneck table.
- `build/captures/overnight_playability_ladder_20260603/overnight_table.md`: latest mid-clipmap backlog-aware scheduler table.
- `build/captures/mid_clipmap_drain_and_reuse_20260603/mid_clipmap_drain_table.md`: latest drain/reuse diagnostic table and fallback-contract decision.

Workspace-level `../../handoff.md` and `../../issues.md` are unrelated Harmonizer/security handoff documents and were not folded into this VENPOD handoff.

## One-Line Current State

VENPOD is no longer blocked by the old Far-SVO domain gap, startup now waits for same-camera shader-unsafe exact-contract feedback by default, and the default-off lower-res background split now fixes the old fullscreen GPU floor in sampled fixed/walk rows; CPU diagnostics now show the mid-clipmap blocker is an ownership-aware streaming gap where broad current-interest cannot be split into critical vs prefetch safely until sampled missing-preferred-mid owner feedback or equivalent metadata exists.

## Current Guardrails

- Keep `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict` as the default.
- Keep `bounded_repair` default-off.
- Bound `64` is the current best correctness candidate, not a default.
- Keep `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0` by default.
- Keep `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0` by default.
- Keep `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE=1.0` by default.
- Do not do water/mode-70 work right now.
- Do not broaden the foreground correctness contract in the performance pass.
- Do not apply random visual/material fixes.
- Do not reapply the rejected per-pixel Far-SVO traversal helper.
- Do not treat `miss=0` / `unsafeNearMiss=0` as success while frame time and public visual coherence are bad.

## Project Context

VENPOD is a voxel/terrain renderer with several possible visible pixel owners:

- exact sparse surface / sparse page surface
- mid voxel clipmap
- Far-SVO terrain
- deterministic water
- sky/miss fallback

The public-frame invariant under discussion is:

> Every visible pixel must have an owner whose result agrees with deterministic terrain/water truth for the current ray.

The user-facing failure is that the renderer still feels slow, laggy, visually broken, and slow to build/edit. Generic smoke tests passing does not prove the live renderer is acceptable.

Relevant files:

- `src/main_launcher.cpp`
- `assets/shaders/Graphics/PS_Raymarch.hlsl`
- `assets/shaders/Common/SharedTypes.hlsli`
- `src/Graphics/FarVoxelOctree.h`
- `src/Graphics/SparseVoxelGpuResources.cpp`
- `engine_capture_smoke.ps1`
- `perf_root_cause_audit.ps1`
- `basin_water_artifact_audit.ps1`
- `water_shoreline_audit.ps1`

The worktree is dirty. Do not revert unrelated changes.

## Major Correctness Findings

### Far-SVO Domain Bug Fixed

The old default Far-SVO page radius was too small. Broad/high public rays could leave the Far-SVO domain and fall back to analytic far-height continuity even though the engine considered Far-SVO ready.

Fix:

- `src/Graphics/FarVoxelOctree.h`
- default page radius changed from `6` to `12`

Validation:

- `build/captures/current_goal_v3u_farsvo_radius12_control_20260602`
- `build/captures/current_goal_v3v_default_farsvo_radius12_20260602`
- far-height dropped to zero in tested frames

This fixed one real failure class. It did not solve public rendering.

### Startup Unsafe Gate Fixed

Default startup now treats same-camera shader-unsafe exact-contract feedback as part of the public render contract.

Current behavior:

- `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS=1` by default
- public render is held until same-camera shader unsafe feedback is clean

Validation:

- `build/captures/current_goal_v4c_shader_unsafe_default_on_20260602`
- frame 120: `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`, `miss=0`, `farHeight=0`

This closed an early exposure class. It did not solve post-open foreground ownership.

### Default Foreground Promotion Still Blocked

Default strict policy remains stuck after public open:

- `surfaceRasterMax=1024`
- `surfacePromoted=0`
- `surfaceClean=0/1`
- `midVoxelScreenPct` about `19.18-19.21%`
- `missScreenPct=0`
- `unsafeNearMissScreenPct=0`

Primary mechanism:

- wide exact surface promotion requires hidden-exact runtime clean sweeps
- those sweeps are too slow for startup/public open
- later hidden-exact feedback resets progress
- public frames expose coarse mid/Far-SVO ownership too long

Important capture:

- `build/captures/root_probe_promotion_gate_postopen_20260602`

Observed gate blockers:

- `hiddenRuntimeClean=0`
- `hiddenClean=0/8`
- clean rays advanced slowly and reset after feedback
- shader feedback age guard was also stricter than observed cadence

Current diagnosis:

> The default public-frame promotion proof is over-constrained for runtime startup. It requires near-zero-feedback hidden-exact clean sweeps that do not converge quickly enough for real public frames.

### Bounded Promotion Candidate Exists

The code now represents bounded promotion explicitly and keeps it default-off.

Current policy knobs:

- default: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`
- candidate: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair`
- feedback age used in tests: `VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE=12`
- hidden-exact demotion bound: `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND`
- alias: `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND`
- legacy opt-in: `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN=1`

Current log:

- `PERF_SPARSE_SURFACE_PROMOTION_POLICY`

The guarded contract says:

- same-camera shader unsafe must be clean enough
- hidden-exact accepted/missing must be below bound
- hidden-exact repair remains active behind promoted raster
- public miss and unsafe-near-miss stay zero
- demote/block when hidden-exact accepted/missing exceeds bound
- high-alt suppressed views remain excluded

Validation highlights:

- `contract_policy_default_off_20260602`: strict default unchanged, `surfaceRasterMax=1024`, `surfacePromoted=0`, mid voxel about `19.18-19.21%`
- `contract_policy_bounded64_fixed_final_20260602`: promoted by frame 140, stayed promoted through frame 280, mid voxel `12.5476% -> 8.647%`, miss/unsafe zero
- `contract_policy_bounded64_walk_20260602`: blocked/demoted at frame 120 for `113 > 64`, promoted at frame 140 after burst drained, stayed promoted through frame 600, mid voxel `1.7945% -> 0.441%`, miss/unsafe zero
- `contract_policy_bounded64_highalt_20260602`: high-alt path stayed unpromoted with `reason=high_alt_excluded`, miss/unsafe zero
- `contract_policy_bounded16_fixed_20260602`: too strict; demoted at frame 240 when hidden-exact accepted/missing hit `32 > 16`, causing a visible ownership jump

Current correctness decision:

- `64` is the best current candidate bound.
- `32` passed a short fixed-camera probe and may deserve longer testing.
- `16` is too strict.
- The bounded policy should not become default until performance headroom and longer validation exist.

## Basin / Water Findings

Old baseline basin audit:

- `build/captures/current_goal_v4e_post_open_basin_audit_20260602`
- `512` sampled suspicious pixels
- generated land above sea level: `411/512`
- water-before-terrain conflict: `47/512`
- owner totals: `mid_voxel=278`, `far_svo=128`, `exact_sparse_surface_extended=105`

Bounded frame-160 basin audit:

- `build/captures/bounded_basin_audit_frame160_20260602`
- exact ownership increased `105/512 -> 265/512`
- mid voxel decreased `278/512 -> 177/512`
- Far-SVO decreased `128/512 -> 70/512`
- `water_should_draw_before_terrain` decreased `47/512 -> 0/512`
- visible water decreased `44/512 -> 0/512`

Interpretation:

- Water was a real smaller class in the old audit, but bounded promotion removed that sampled class.
- Do not return to mode 70 before foreground/performance work unless new bounded audits show water rows dominate again.

Mode 70 status:

- Waterline reason mode exists, but a quick inclusion in `BackgroundDebugLayerMode()` caused a very slow/hung shader run and was reverted.
- Treat mode 70 as debug-tooling debt.

## Performance Root Cause

The current pivot is performance/root-cause attribution before more visual correctness work. The renderer must become playable and eventually target stable 60 fps, meaning no routine frame should exceed about `16.67 ms`.

### Instrumentation

Existing timing logs used:

- `PERF`
- `PERF_FRAME_END`
- `PERF_SPARSE`
- `PERF_SPARSE_CLIPMAP`
- `PERF_SPARSE_SURFACE`
- `PERF_SPARSE_HIDDEN_EXACT_MISS`
- `PERF_SPARSE_EDIT_PUBLISH`
- `PERF_CAMERA_EXPOSURE`
- `PERF_SPARSE_SURFACE_PROMOTION_POLICY`

Added:

- `PERF_SPARSE_EDIT_LATENCY` in `src/main_launcher.cpp`
- `perf_root_cause_audit.ps1`

Generated artifacts:

- `build/captures/perf_root_cause_20260602/perf_cost_table.csv`
- `build/captures/perf_root_cause_20260602/perf_cost_table.md`
- `build/captures/perf_root_cause_20260602/edit_latency_events.txt`
- `build/captures/perf_non_capture_default_strict_nophys_20260602`
- `build/captures/perf_edit_brush_paint_20260602`

### Cost Waterfall

| Scenario | Frame | Promoted | Raster | Mid % | Body ms | Raw ms | GPU ray ms | CPU update ms | Hidden repair ms | Surface extract ms | Page gen ms | Upload ms | Readback ms | Present/wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| default strict fixed capture | 240 | 0 | 1024 | 19.18 | 113.99 | 87.08 | 61.55 | 55.91 | 4.51 | 10.78 | 9.51 | 0.52 | 0.01 | 57.92 |
| bounded64 fixed capture | 240 | 1 | 2560 | 9.20 | 55.24 | 60.38 | 41.65 | 43.25 | 4.04 | 3.05 | 5.72 | 0.33 | 0.01 | 11.90 |
| bounded64 walk capture | 480 | 1 | 2560 | 0.38 | 124.36 | 83.19 | 25.73 | 92.59 | 2.46 | 5.82 | 9.58 | 1.69 | 0.03 | 31.69 |
| high-alt excluded capture | 240 | 0 | 1024 | 0.00 | 93.07 | 106.02 | 64.29 | 55.40 | 2.74 | 4.35 | 5.25 | 1.64 | 0.01 | 37.58 |
| edit brush paint capture | 240 | 0 | 1024 | 19.19 | 62.77 | 67.40 | 60.12 | 45.81 | 4.04 | 4.92 | 7.40 | 0.46 | 0.01 | 16.88 |
| non-capture strict, physics disabled | 240 | 0 | 1024 | 19.18 | 62.31 | 82.02 | 60.09 | 50.15 | 4.10 | 3.21 | 6.05 | 0.33 | 0.10 | 12.07 |

### Performance Attribution

Current top bottlenecks:

1. GPU raymarch floor.
   - Fixed/default/high-alt/edit/non-capture samples are around `60-64 ms` GPU ray.
   - This alone exceeds the 60 fps frame budget.

2. Movement sparse CPU update.
   - Bound64 walk frame 480 had `cpuUpdateMs=92.59`.
   - Sparse split included about `37.19 ms` request, `37.15 ms` clipmap, `9.58 ms` generation, `6.97 ms` trim.

Secondary/non-dominant costs:

- hidden-exact repair: about `2.46-4.51 ms`
- upload/readback: not dominant in sampled rows
- surface extraction/staging: secondary, about `3.05-10.78 ms`, with walk staging reaching about `11.51 ms`
- capture/debug: inflates some body/gap timings, but non-capture strict/no-physics still has `gpuRayMs=60.09` and `cpuUpdateMs=50.15`

Bounded promotion is not currently the performance culprit:

- bounded64 fixed was faster than strict fixed in the sampled frame
- high-alt remains slow while promotion is excluded
- non-capture strict remains far over budget

### GPU Raymarch Floor Root Cause

Latest audit:

- `build/captures/gpu_raymarch_floor_ablation_20260602/raymarch_floor_summary.csv`
- `build/captures/gpu_raymarch_floor_ablation_20260602/raymarch_floor_table.md`

Preserved correctness state:

- default promotion remains `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`
- `bounded_repair` remains default-off
- bound `64` remains the best current correctness candidate but is not default
- water/mode-70 work is paused
- rejected per-pixel Far-SVO recovery/traversal helper was not reapplied

New default-neutral knobs:

- `VENPOD_RAYMARCH_RENDER_SCALE`, default `1.0`
- `VENPOD_RENDER_QUALITY=playable`, opt-in alias for render scale `0.5` unless `VENPOD_RAYMARCH_RENDER_SCALE` is explicit
- `VENPOD_RAYMARCH_MAX_STEPS_SCALE`, default `1.0`
- `VENPOD_RAYMARCH_MAX_DISTANCE_SCALE`, default `1.0`

Existing knobs found/used:

- `VENPOD_RAYMARCH_MAX_DISTANCE`
- `VENPOD_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_DISTANCE`
- `VENPOD_SPARSE_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT`
- `VENPOD_ENABLE_FAR_SVO`
- `VENPOD_SPARSE_MID_VOXEL_RENDER`
- `VENPOD_SPARSE_MID_CLIPMAP`

Resolution sweep, strict fixed camera, frame `240`:

| Scale | Output | GPU ray ms | Speedup | CPU update ms | Miss/unsafe |
|---:|---:|---:|---:|---:|---:|
| `1.00` | `1920x1080` | `61.92` | `1.00x` | `44.65` | `0/0` |
| `0.75` | `1440x810` | `41.58` | `1.49x` | `63.37` | `0/0` |
| `0.50` | `960x540` | `28.38` | `2.18x` | `65.78` | `0/0` |
| `0.33` | `634x356` | `28.23` | `2.19x` | `68.84` | `0/0` |

Interpretation: native raymarch is pixel-count sensitive, but not close to pixel-linear. Half resolution removes about `33.5 ms`; `0.33` stalls at the same `~28 ms` residual floor.

Max-step sweep, strict fixed camera, native resolution:

| Step scale | Ray budget steps | GPU ray ms | Speedup | Miss/unsafe |
|---:|---:|---:|---:|---:|
| `1.00` | `73` | `61.19` | `1.00x` | `0/0` |
| `0.75` | `55` | `60.64` | `1.01x` | `0/0` |
| `0.50` | `36` | `61.09` | `1.00x` | `0/0` |
| `0.25` | `18` | `60.00` | `1.02x` | `0/0` |

Interpretation: the exposed max-step budget is not the cause of the sampled floor.

Feature ablation, strict fixed camera, native resolution:

| Diagnostic row | GPU ray ms | Delta vs strict | Notes |
|---|---:|---:|---|
| strict baseline | `61.19` | `0.00` | baseline |
| Far-SVO off | `29.41` | `-31.78` | dominant measured GPU contributor |
| mid voxel render off | `52.26` | `-8.93` | secondary |
| runtime quality ceiling 50 | `60.27` | `-0.92` | current quality scalar does not address floor |

High-alt rows:

| Row | Output | GPU ray ms | CPU update ms | Notes |
|---|---:|---:|---:|---|
| high-alt native | `1920x1080` | `79.86` | `260.99` | promotion excluded |
| high-alt half res | `960x540` | `40.66` | `324.02` | GPU improves `1.96x`; CPU remains dominant |
| high-alt half steps | `1920x1080` | `78.15` | `351.81` | step budget does not fix high-alt |

Bounded comparison:

- strict native fixed: `gpuRayMs=61.92`, `surfacePromoted=0`, `surfaceRasterMax=1024`
- bounded64 native fixed: `gpuRayMs=42.70`, `surfacePromoted=1`, `surfaceRasterMax=2560`
- bounded64 half-res fixed: `gpuRayMs=24.01`

Current GPU root:

The `~60 ms` native GPU ray floor is primarily a Far-SVO/background fullscreen raymarch cost with pixel-count sensitivity and a residual `~28 ms` floor. It is not caused by bounded foreground promotion and is not materially improved by reducing the exposed max-step budget.

First implemented playability lever:

- `VENPOD_RAYMARCH_RENDER_SCALE=0.5`
- or `VENPOD_RENDER_QUALITY=playable`

Measured strict fixed-camera effect:

- `gpuRayMs`: `61.92 -> 28.38`
- speedup: `2.18x`
- `missScreenPct=0`
- `unsafeNearMissScreenPct=0`

This is default-off and not a full 60 fps fix. It proves the GPU ray path has a large pixel-count-sensitive component, but leaves residual `~28 ms` GPU ray cost and high CPU sparse update cost.

Recommended next patch:

Do not disable Far-SVO as the correctness fix. Make the Far-SVO/background path cheaper with a lower-res far/background pass, tile/region early reject, or pass split so exact foreground ownership stays full resolution while broad background traversal is not paid by every full-res pixel. Measure before/after with `gpuRayMs`, ownership percentages, `missScreenPct`, and `unsafeNearMissScreenPct`.

### Far-SVO / Background GPU Reduction Pass

Latest audit:

- `build/captures/far_svo_background_gpu_reduction_20260602/far_svo_gpu_summary.csv`
- `build/captures/far_svo_background_gpu_reduction_20260602/far_svo_gpu_table.md`

Default-off knob added:

- `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE`, default `1.0`
- tested candidate value: `0.5`

What the knob does:

- C++ side only.
- Scales `camera.farFieldQuality` before renderer constants are uploaded.
- Uses the shader's existing Far-SVO quality/page-step schedule.
- Does not set `VENPOD_ENABLE_FAR_SVO=0`.
- Does not change `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY`; bounded repair remains default-off.

Rejected instrumentation/candidate:

- Per-attempt Far-SVO/background atomic stats were attempted and removed because the shader variant caused multi-minute runtime DXC compiles.
- A shader-side `defer elevated Far-SVO candidate` branch was also removed.
- Treat `PS_Raymarch.hlsl` source churn as performance/tooling debt until the shader is split or compile time is fixed. The audit used the prior known-good `7386940` byte PS_Raymarch cache entry to keep runtime captures moving after source-hash churn.

Paired results:

| Scenario | Baseline GPU ray | Quality 0.5 GPU ray | Delta | Far-SVO % baseline -> candidate | Miss/unsafe |
|---|---:|---:|---:|---:|---:|
| strict native fixed | `59.96 ms` | `34.20 ms` | `-25.76 ms` | `0.22 -> 0.00` | `0/0` |
| strict half fixed | `35.86 ms` | `28.91 ms` | `-6.95 ms` | `0.22 -> 0.00` | `0/0` |
| bounded64 native fixed | `46.05 ms` | `29.61 ms` | `-16.44 ms` | `0.09 -> 0.00` | `0/0` |
| high-alt native | `78.99 ms` | `74.82 ms` | `-4.17 ms` | `0.92 -> 0.93` | `0/0` |
| high-alt half | `67.23 ms` | `71.97 ms` | `+4.74 ms` | `1.04 -> 1.05` | `0/0` |
| walk native | `0.02 ms` | `2.98 ms` | `+2.96 ms` | `0.00 -> 0.00` | `0/0` |

Decision:

- Keep `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE` default-off/default-neutral.
- It is useful as a diagnostic/perf-budget lever, not a current playable-mode default.
- It gives a strong strict fixed-camera GPU win, but the low-alt fixed rows lose final Far-SVO ownership and the high-alt/walk rows do not show a consistent win.
- Bounded promotion is still not the performance culprit.
- The next real patch should be lower-res far/background pass, tile/region reject, or a pass split that preserves exact foreground and does not rely on suppressing Far-SVO ownership.

### Background Pass Split / Mask Prototype

Latest audit:

- `build/captures/background_pass_split_or_mask_20260602/background_split_summary.csv`
- `build/captures/background_pass_split_or_mask_20260602/background_split_table.md`
- `build/captures/background_pass_split_or_mask_20260602/background_split_contact_sheet.png`

Default-off knobs added:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0`
- tested candidate: enable `1`, scale `0.375`
- audit-only: `VENPOD_RAYMARCH_FIXED_CAMERA=0`

Prototype chosen:

- lower-res background pass split
- exact sparse surface remains full-resolution on the swapchain render target
- `PS_Raymarch` renders to a lower-res offscreen color/depth target
- `PS_BackgroundComposite` samples that target and composites only where full-res stencil is still zero
- no `PS_Raymarch.hlsl` source edit was needed

Measured rows:

| Scenario | Baseline GPU ray | Candidate GPU ray | Notes |
|---|---:|---:|---|
| strict native fixed | `61.33 ms` | `32.37 ms` | GPU target hit, but visual fail |
| strict playable fixed | n/a | `24.39 ms` | diagnostic only |
| bounded64 native fixed | n/a | `28.68 ms` | bounded remains comparison-only/default-off |
| high-alt native | `79.97 ms` | `35.73 ms` | directional only; stress camera was not matched |
| walk native | n/a | `21.01 ms` | CPU still dominated at `110.78 ms` |

Validation decision:

- `missScreenPct=0` and `unsafeNearMissScreenPct=0` stayed zero in sampled split rows.
- Far-SVO sample ownership did not drop to zero (`642` strict samples, `3141` high-alt samples), but split ownership percentages are low-res raymarch sample counters, not true full-res final composite ownership.
- Contact sheet failed visual validation: split rows show white sampled background/sky in unstenciled regions.
- A temporary constant-color composite probe proved the full-resolution stencil composite executes; the bad output is in the lower-res background target content path or SRV contents.
- Keep the prototype default-off. It is not safe for playable mode until the background target content path is fixed and true composite counters exist.

### Background Split Content Fix

Latest audit:

- `build/captures/background_split_content_fix_20260602/background_split_content_summary.csv`
- `build/captures/background_split_content_fix_20260602/background_split_content_table.md`
- `build/captures/background_split_content_fix_20260602/contact_sheet.png`
- split rows include lower-res target readbacks: `background_pass_frame_*.bmp`
- final full-res composites: `engine_frame_*.bmp`

Root cause:

- The background pass SRV was allocated at shader-visible descriptor index `0`.
- `src/UI/ImGuiBackend.cpp` also uses descriptor index `0` for the ImGui font texture.
- ImGui overwrote the descriptor, so `PS_BackgroundComposite` sampled the ImGui font/white texture instead of `BackgroundPassColor`.
- `Renderer` now reserves shader-visible descriptor `0` for ImGui immediately after heap initialization; renderer-owned shader-visible SRVs now start at index `1`.

Debug probes:

- Force-color lower-res background pass now produces a green lower-res readback and a green full-res composite.
- Normal split readbacks now contain plausible terrain/sky/water content.
- `lowResWhiteOrClearPct=0` in all sampled split rows.
- Full-res composite/stencil counters come from `PERF_RENDER_COMPOSITION`: `screen`, `surfaceOwnedPixels`, `backgroundPixels`.

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

- The lower-res background split no longer has the white/clear sampled-output bug in the contact sheet.
- It preserves `missScreenPct=0` and `unsafeNearMissScreenPct=0` in sampled rows.
- Strict native fixed GPU ray time improved `60.38 -> 17.88 ms`.
- Matched high-alt GPU ray time improved `73.76 -> 29.86 ms`.
- Playable fixed split reached `10.62 ms` GPU ray time, but total frame time is still not 60 fps because CPU sparse update is still about `39 ms`.
- Bounded promotion remains not the performance culprit; bounded64 split is comparable to strict split and remains default-off.
- Keep `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0` and `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0` by default.

### Movement Sparse CPU Update Reduction

Audit:

- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_summary.csv`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_table.md`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/contact_sheet.png`

Added CPU detail logging:

- `VENPOD_SPARSE_CPU_DETAIL=1`
- `PERF_SPARSE_CPU_DETAIL`
- request attempts/unique/duplicates/resident/nonresident/allocated and request skip reasons
- trim/replacement scan calls, records, candidates, and evictions
- mid-clipmap interest/reuse/pump, generated height/voxel bricks, missing height/voxel bricks, cap/active state
- surface extract/stage timing and extraction backlog

Implemented optimization:

- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0` by default
- when enabled, broad terrain-surface prefetch is skipped only after public ownership is already clean
- this leaves screen-critical terrain repair, hidden-exact repair, mid-clipmap work, Far-SVO, and promotion policy active
- `PERF_SPARSE_CPU_DETAIL` now logs `terrainPrefetch=ms/rays/budget/seen/new/cleanThrottle`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS` remains default-off and is a rejected diagnostic, not the accepted optimization
- `VENPOD_SPARSE_EVICTION_PARTIAL_SORT=1` keeps equivalent top-K eviction selection by default; `0` restores old full sort

Classification:

- fixed/walk request prep was partly a broad terrain-surface prefetch storm: up to `627` CPU rays and hundreds of resident/readiness touches after ownership was already clean
- after the request storm is reduced, movement/high-alt spikes are still dominated by synchronous mid-voxel clipmap pump/generation and surface staging bursts
- trim/replacement scans are visible during movement but not the fixed-camera blocker

Validation highlights, all with background split enabled at `0.375`:

| Scenario | Frame | Clean throttle | CPU update ms | Request | Gen | Clip | Terrain prefetch | Clip pump | Missing voxel | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict | `240` | `0` | `42.94` | `35.60` | `6.45` | `0.87` | n/a | `0.20` | `0` | `16.72` | `0/0` | baseline |
| fixed strict | `240` | `1` | `21.46` | `8.92` | `10.67` | `1.86` | `0.00 ms / 0 rays` | `0.30` | `0` | `11.48` | `0/0` | fixed CPU roughly halves |
| walk strict | `480` | `0` | `70.24` | `33.72` | `14.01` | `18.71` | `13.19 ms / 627 rays` | `17.85` | `86` | `13.73` | `0/0` | fresh matched walk baseline |
| walk strict | `480` | `1` | `58.49` | `22.10` | `13.55` | `19.28` | `0.00 ms / 0 rays` | `18.10` | `115` | `13.01` | `0/0` | material reduction, still above target |
| walk bounded64 | `480` | `0` | `60.80` | `30.56` | `12.85` | `13.92` | n/a | `12.97` | `48` | `14.26` | `0/0` | bounded comparison |
| walk bounded64 | `480` | `1` | `51.83` | `21.13` | `20.89` | `6.07` | `0.00 ms / 0 rays` | `4.56` | `0` | `16.25` | `0/0` | bounded also benefits, but remains default-off |
| high-alt strict | `240` | `0` | `53.33` | `8.16` | `8.77` | `36.40` | n/a | `30.30` | `1042` | `18.59` | `0/0` | baseline |
| high-alt strict | `240` | `1` | `58.27` | `7.91` | `8.03` | `42.33` | `0.00 ms / 0 rays` | `37.56` | `450` | `17.24` | `0/0` | no improvement; high-alt remains clipmap dominated |

Rejected probe:

- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS=8` plus clean prefetch throttle was rejected: walk CPU stayed high (`62.88 ms`), mid-voxel coverage fell to `0.70`, and missing interested voxel bricks rose to `2809`
- earlier unguarded high-alt cap `8/16` probes reduced CPU but produced obvious white terrain/shore regions
- high-alt needs a different backlog-aware/incremental solution

Decision:

- keep `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0` by default; it is useful but insufficient for playable mode
- matched strict walk sparse CPU improved `70.24 -> 58.49 ms`; fixed sparse CPU improved `42.94 -> 21.46 ms`; `miss/unsafe=0/0`
- non-capture validation showed capture is not the CPU cause; clipmap catch-up can still dominate at `102.28 ms`
- high-alt remains a separate clipmap-pump bottleneck
- bounded promotion remains default-off and remains not the performance culprit

### Overnight Playability Ladder - 2026-06-03

Audit:

- `build/captures/overnight_playability_ladder_20260603/overnight_summary.csv`
- `build/captures/overnight_playability_ladder_20260603/overnight_table.md`
- `build/captures/overnight_playability_ladder_20260603/contact_sheet.png`

Mid-clipmap path summary:

- interest is built in `SparseClipmapTileCache::UpdateInterest` and `UpdateVoxelInterest`
- missing height/voxel bricks are discovered from current interest versus resident/generated brick maps
- brick generation is synchronous on the main thread via `PumpGeneration`, `PumpVoxelGenerationForRing`, `GenerateTile`, and `GenerateVoxelBrick`
- pump order is interest-score/ring ordered, with feedback front-insertion for urgent voxel bricks
- the pump has approximate priority but not a final screen-critical proof
- missing/deferred voxel bricks can fall back to lower ownership paths, so `miss=0` can still coexist with visible terrain/shore regressions if a cap is too blunt

New diagnostics:

- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` under `VENPOD_SPARSE_CPU_DETAIL=1`
- fields include backlog-aware active state, effective pump budget, budget hit, backlog height/voxel counts, oldest/max backlog age, pruned stale entries, critical/noncritical missing voxel bricks, missing height/voxel bricks, and mid coverage

Implemented candidate:

- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=0` by default
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MS=4.0` when enabled
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MIN_COVERAGE_PCT=95`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MAX_CRITICAL_MISSING=128`
- the budget is disabled before startup/public open, during coverage emergencies, below the coverage threshold, or above the critical-missing threshold
- backlog entries are deduped and carried only while still current-interest; stale/resident entries are pruned
- `GenerateVoxelBrick` now reserves enough temporary column-cache capacity for its existing 5x5 coarse footprint sampling path; this is behavior-preserving allocation reduction

Validation highlights, all with background split `0.375` and clean prefetch throttle:

| Scenario | Frame | Candidate | CPU update ms | Request | Gen | Clip | Surface stage | Trim | Missing voxel | Backlog voxel | Max backlog age | Mid coverage | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict baseline | `240` | `0` | `24.01` | `11.77` | `11.34` | `0.90` | `4.94` | `0.00` | `0` | n/a | n/a | `100` | `11.26` | `0/0` | split plus clean throttle |
| fixed strict backlog4 | `240` | `1` | `16.05` | `7.94` | `7.34` | `0.77` | `3.31` | `0.00` | `0` | `0` | `0` | `100` | `11.61` | `0/0` | safe fixed improvement |
| walk strict baseline | `480` | `0` | `90.67` | `23.63` | `24.15` | `37.76` | `21.45` | `5.13` | `91` | n/a | n/a | `98` | `15.60` | `0/0` | movement baseline |
| walk strict backlog4 | `480` | `1` | `43.23` | `16.36` | `16.52` | `7.28` | `14.64` | `3.07` | `415` | `415` | `117` | `95` | `9.25` | `0/0` | best short walk row |
| walk strict backlog4 long | `600` | `1` | `79.84` | `20.12` | `10.33` | `45.80` | `18.29` | `3.59` | `494` | `494` | `262` | `94` | `20.95` | `0/0` | catch-up returns |
| walk bounded64 baseline | `480` | `0` | `32.96` | `16.83` | `11.70` | `1.33` | `7.29` | `3.10` | `0` | n/a | n/a | `100` | `26.16` | `0/0` | comparison |
| walk bounded64 backlog4 | `480` | `1` | `46.75` | `12.43` | `10.68` | `20.89` | `9.96` | `2.75` | `33` | `33` | `18` | `99` | `24.67` | `0/0` | not a bounded win |
| high-alt strict baseline | `240` | `0` | `71.12` | `11.94` | `9.24` | `49.94` | `5.63` | `0.00` | `592` | n/a | n/a | `93` | `26.97` | `0/0` | stress camera |
| high-alt strict backlog4 | `240` | `1` | `52.83` | `6.72` | `5.63` | `40.48` | `3.92` | `0.00` | `638` | `638` | `164` | `92` | `23.50` | `0/0` | budget disabled by guards |
| noncapture walk baseline | `480` | `0` | `49.63` | `15.98` | `15.88` | `14.70` | `12.70` | `3.07` | `86` | n/a | n/a | `98` | `23.73` | `0/0` | no capture |
| noncapture walk backlog4 | `480` | `1` | `51.32` | `16.15` | `15.18` | `16.69` | `12.45` | `3.30` | `451` | `451` | `113` | `95` | `26.85` | `0/0` | no capture not improved |

Visual/status decision:

- fixed and walk strict candidate rows looked comparable to baselines in the refreshed contact sheet
- high-alt still has the pre-existing bright shoreline/terrain artifact; it is not solved
- the old blunt pump cap remains rejected
- the backlog-aware pump is useful but not default/playable-safe yet: it hit the short-walk target (`90.67 -> 43.23 ms`) but frame `600` returned to `79.84 ms`, non-capture did not improve, and high-alt is still clipmap dominated
- combined playable mode was not promoted because long-walk/high-alt/non-capture safety did not pass
- background split, clean prefetch throttle, backlog-aware pump, and bounded promotion all remain default-off
- next pass should target mid-clipmap generation architecture: async/noncritical generation or incremental interest/scroll reuse, then request/surface staging after clip catch-up is stable

### Mid-Clipmap Drain/ReUse Pass - 2026-06-03

Audit:

- `build/captures/mid_clipmap_drain_and_reuse_20260603/mid_clipmap_drain_summary.csv`
- `build/captures/mid_clipmap_drain_and_reuse_20260603/mid_clipmap_drain_table.md`
- `build/captures/mid_clipmap_drain_and_reuse_20260603/contact_sheet.png`

Final code change:

- `VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS=0` default-off
- expanded `PERF_SPARSE_MID_CLIPMAP_BACKLOG` with budget reason, interest churn, resident/reused interest, backlog enqueue/carry/pump/skip, age buckets, per-ring generation, and average/max `GenerateVoxelBrick` ms
- no optimization candidate was kept; an age-priority queue-order prototype was tried and rejected after it failed to validate cleanly and did not solve high-alt coverage debt

Budget disable reason codes:

- `0`: active/no disable
- `1`: startup/public gate
- `2`: coverage below threshold
- `3`: critical missing above threshold
- `4`: visual ownership guard
- `5`: coverage emergency

Root classification:

- primary root: fallback contract / streaming architecture
- generation cost is the immediate mechanism, but capping/reordering generation is unsafe until fallback-validity is explicit

Key rows:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Backlog | Max age | Critical/noncritical | Coverage | Budget reason | Generated voxel | Avg/max gen ms | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict public | `380` | `17.49` | `1.79` | `0.61` | `0` | `0` | `0` | `0/0` | `100` | `4` | `0` | `0/0` | fixed path is not the blocker |
| walk fixed-dt | `600` | `38.89` | `6.71` | `5.69` | `419` | `419` | `218` | `13/406` | `95` | `0` | `4` | `1.149/2.507` | deterministic replay keeps budget active |
| walk realtime | `600` | `56.95` | `22.51` | `21.55` | `535` | `535` | `112` | `19/516` | `94` | `2` | `24` | `0.829/1.925` | coverage guard forces catch-up |
| high-alt strict | `240` | `50.38` | `36.24` | `30.26` | `1858` | `1858` | `25` | `1100/758` | `80` | `2` | `24` | `1.121/1.884` | broad footprint is not safely deferrable |
| noncapture walk | `600` | `44.02` | `7.05` | `6.08` | `435` | `435` | `211` | `0/435` | `95` | `0` | `5` | `1.005/1.744` | request/surface stage dominate this sample |

Decision:

- high-alt is the decisive stop condition: `1858` missing current-interest bricks, `80%` coverage, and `1100` critical missing bricks
- realtime walk shows the same mechanism when coverage slips from `95` to `94`
- the current fallback/lower-owner contract is not strong enough to defer broad missing mid-clipmap bricks without visual risk
- the old blunt pump cap remains rejected
- background split, clean prefetch throttle, backlog-aware pump, and bounded repair remain default-off
- next pass should design async/noncritical mid-clipmap generation with fallback-validity classification and staged GPU upload accounting

Minimum refactor outline:

- keep a persistent prioritized mid-clipmap backlog with age and visibility metadata
- add an async/noncritical voxel generation queue deduped by brick key
- classify whether each missing brick has a valid lower-LOD/Far-SVO fallback before deferring it
- consume completed worker bricks through the existing staged upload path with explicit upload budget accounting
- only then revisit incremental interest/scroll reuse and request/surface staging

### Async Mid-Clipmap Fallback Validity Pass - 2026-06-03

Audit:

- `build/captures/async_mid_clipmap_fallback_validity_20260603/async_mid_fallback_summary.csv`
- `build/captures/async_mid_clipmap_fallback_validity_20260603/async_mid_fallback_table.md`
- `build/captures/async_mid_clipmap_fallback_validity_20260603/contact_sheet.png`

Default-off changes:

- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0`
- `PERF_SPARSE_MID_CLIPMAP_FALLBACK` logs fallback-valid/invalid/unknown counts, high-alt counts, finer/coarser availability, rejection reasons, async placeholders, and upload placeholders
- async generation was not implemented because the classifier found no proven fallback-valid missing bricks in the debt rows

Fallback path findings:

- missing preferred mid-voxel bricks fall back in shader space through resident finer rings, then coarser parents only in restricted low-alt/ray-angle cases
- high-alt views reject coarser mid-voxel parents, so coarser parent residency is not a safe CPU proof for high-alt deferral
- CPU can currently prove only complete finer-ring coverage for a missing brick
- CPU cannot currently prove per-brick Far-SVO, water, sky, or shoreline validity; those remain shader/per-ray decisions
- unknown fallback is treated as critical

Classifier rows, all with background split `0.375`, clean prefetch throttle, backlog-aware pump, and drain/reuse diagnostics enabled:

| Scenario | Frame | CPU update ms | Clip ms | Missing voxel | Fallback valid | Fallback invalid | Fallback unknown | Async eligible | Sync required | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `13.91` | `0.81` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.42` | `0/0` | no missing mid-voxel debt |
| walk fixed-dt | `600` | `37.72` | `6.55` | `269` | `0` | `61` | `208` | `0` | `269` | `97` | `0` | `4.42` | `0/0` | backlog debt remains |
| walk realtime | `600` | `37.26` | `5.80` | `308` | `0` | `119` | `189` | `0` | `308` | `96` | `0` | `10.20` | `0/0` | no proven safe async subset |
| high-alt | `240` | `116.40` | `101.65` | `3354` | `0` | `2183` | `1171` | `0` | `3354` | `62` | `2` | `21.68` | `0/0` | broad footprint is fallback-blocked |
| noncapture walk | `402` | `61.47` | `1.13` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `1` | `32.84` | `0/0` | direct noncapture run reached only a usable frame-402 row |

Decision:

- classification result: missing-debt rows are fallback-invalid/unknown dominated, with `0` fallback-valid and `0` async-eligible bricks
- high-alt is decisive: `3354` missing current-interest voxel bricks, `2183` invalid, `1171` unknown, `0` valid, and coverage `62`
- async noncritical generation is blocked until the fallback contract can produce a nonzero trustworthy valid subset
- staged upload accounting is present only as zero placeholders because no worker completions are produced
- contact sheet verdict: fixed and walk rows are comparable to prior state; high-alt still shows the existing broad artifact/coverage problem
- all defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, async candidate, and bounded repair are default-off
- the rejected blunt pump cap and removed age-priority prototype remain rejected

Next safe pass:

- design a fallback contract that can prove per-brick/per-region lower ownership: finer mid coverage, Far-SVO domain/material validity, water/shoreline safety, and public readiness state
- add a worker-generated mid-brick payload/apply path only after such proof exists, with dedupe, generation stamps, safe frame-boundary apply, and upload/completion budgets
- do not move generation async by treating unknown fallback as safe

### Mid-Clipmap Fallback Contract Ownership Pass - 2026-06-03

Audit:

- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/fallback_contract_summary.csv`
- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/fallback_contract_table.md`
- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/contact_sheet.png`

New default-off diagnostics:

- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=0`
- `PERF_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT`

Fallback contract map:

- preferred mid-voxel miss fallback in `PS_Raymarch`: preferred resident brick, resident finer rings, then coarser parents only in restricted low-alt/ray-angle cases
- high-alt rejects coarser mid-voxel parents as a safety/visual guard
- CPU-provable valid fallback remains complete finer-ring coverage
- Far-SVO page/domain coverage can now be diagnosed per missing brick, but Far-SVO material/occupancy/shoreline ownership is still unknown
- water and sky ownership are shader/per-ray decisions and remain unknown at brick level
- unknown fallback remains critical and async-ineligible

Chosen proof:

- default-off Far-SVO domain proof, not Far-SVO validity
- checks missing-brick bounds against ready Far-SVO page/domain coverage
- does not make a brick async-eligible because material/occupancy/water/shoreline ownership is not CPU-proved

Rows, all with background split `0.375`, clean prefetch throttle, backlog-aware pump, fallback classifier, fallback contract diagnostics, and Far-SVO domain proof enabled:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Valid | Invalid | Unknown | Far-SVO domain valid | Far-SVO material unknown | Async eligible | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.88` | `1.11` | `0.47` | `0` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.57` | `0/0` | no debt |
| walk fixed-dt | `600` | `64.68` | `27.35` | `14.41` | `335` | `0` | `67` | `268` | `42` | `42` | `0` | `96` | `0` | `12.36` | `0/0` | domain subset remains material-unknown |
| walk realtime | `600` | `79.30` | `38.62` | `24.50` | `463` | `0` | `12` | `451` | `5` | `5` | `0` | `94` | `2` | `22.82` | `0/0` | coverage emergency |
| high-alt strict | `240` | `99.59` | `85.49` | `78.04` | `2773` | `0` | `1602` | `1171` | `433` | `433` | `0` | `69` | `2` | `30.32` | `0/0` | high-alt fallback-contract blocker |
| noncapture walk | `600` | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | `12.16` | n/a | direct run did not emit fallback contract detail |

High-alt diagnosis:

- previous classifier row: `3354` missing, `0` valid, `2183` invalid, `1171` unknown
- this pass retry: `2773` missing, `0` valid, `1602` invalid, `1171` unknown
- Far-SVO domain proof found `433` domain-covered missing bricks, but all `433` were material/owner unknown
- `2340` missing bricks were outside the CPU-proved Far-SVO domain, `2132` coarser parents were rejected by high-alt policy, and `1602` were screen-critical
- async generation and async plan-only remain blocked because `asyncEligible=0`

Decision:

- proof candidate is safe to keep default-off as diagnostics
- no performance optimization or async path was implemented
- fixed/walk contact-sheet rows remain comparable; high-alt still has the known bright shoreline/terrain artifact
- all defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, async candidate, and bounded repair are default-off
- rejected blunt pump cap and removed age-priority prototype remain rejected

Next safe pass:

- add lightweight fallback-owner feedback or CPU metadata for Far-SVO material/occupancy and water/shoreline safety
- convert shader-only unknowns into explicit valid/invalid before async generation
- if valid remains zero, reduce high-alt/current-interest footprint under an explicit ownership contract instead of deferring unknown bricks

### Autonomous Streaming Playability Engineering - 2026-06-03

Audit:

- `build/captures/autonomous_streaming_playability_engineering_20260603/summary.csv`
- `build/captures/autonomous_streaming_playability_engineering_20260603/table.md`
- `build/captures/autonomous_streaming_playability_engineering_20260603/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Architecture review:

- `UpdateVoxelInterest` builds a broad resident/cache footprint, not a true visible-frame interest set.
- High-alt adds broad terrain/fan candidates across rings and can create thousands of missing current-interest bricks.
- `PumpGeneration` / `PumpVoxelGenerationForRing` still generate voxel bricks synchronously on the main thread.
- Coverage/budget guards are computed over broad interest coverage, so coverage drops can disable the pump budget and force catch-up.
- Existing render feedback is too narrow to act as a visible-critical owner map.
- Shader fallback remains per-ray and high-alt rejects coarser mid parents; unknown fallback is still critical.

Fresh rows with background split `0.375`, clean throttle, CPU detail, backlog-aware pump, fallback classifier, contract diagnostics, and Far-SVO domain proof enabled:

| Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU ray | Missing | Valid/Invalid/Unknown | Critical/Noncritical | Coverage | Reason | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.58` | `6.63` | `7.06` | `0.89` | `0.20` | `11.18` | `0` | `0/0/0` | `0/0` | `100` | `4` | fixed path not blocked |
| walk fixed-dt | `600` | `43.44` | `22.53` | `10.64` | `6.52` | `5.39` | `12.49` | `413` | `0/0/413` | `0/413` | `95` | `0` | request/surface/gen are material |
| walk realtime | `600` | `78.94` | `16.13` | `7.38` | `52.18` | `40.20` | `14.20` | `474` | `0/100/374` | `100/374` | `94` | `2` | coverage guard disables budget |
| high-alt strict | `240` | `99.59` | `7.62` | `6.46` | `85.49` | `78.04` | `30.32` | `2773` | `0/1602/1171` | `1602/1171` | `69` | `2` | reused prior matched retry; fallback-contract blocked |
| noncapture direct | `600` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0.82` | n/a | n/a | n/a | n/a | n/a | clean run, not a comparable sparse-detail row |

Rejected prototype:

- A temporary default-off ring-based visible-critical guard was tried and removed.
- Fixed-dt walk improved in isolation: CPU `43.44 -> 33.69`, but missing voxel debt grew `413 -> 897`.
- Realtime walk regressed: CPU `78.94 -> 124.03`, clip `52.18 -> 91.46`, missing voxel `474 -> 1069`, parent-held visual failure `27 -> 1193`.
- Verdict: ring coverage is not a safe ownership contract; it can hide broad debt until parent-held/visual catch-up explodes.

Decision:

- No new optimization remains in source from this pass.
- No new env knob remains from this pass.
- All defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair remain default-off/default-neutral.
- Rejected blunt pump cap remains rejected.
- Removed age-priority queue prototype remains rejected.
- Async remains blocked because fallback-valid subset is still zero in debt rows.
- Combined playable candidate was not tested because the only new candidate failed validation.

Current bottleneck:

- Fixed split rows are close to acceptable CPU.
- Walk fixed-dt is split across request, generation, surface staging, and trim.
- Walk realtime and high-alt still turn broad current-interest debt into synchronous clip catch-up.
- The missing architecture piece is sampled/owner-aware visible-critical classification.

Next safe pass:

- Add lightweight sampled missing-preferred-mid feedback that records which visible rays actually sampled missing preferred mid bricks and which final owner covered them.
- Use bounded tile/hash/reservoir feedback, not unbounded full-frame atomics.
- Split broad `currentInterest` into visible-critical and cache/prefetch only after that evidence exists.
- Unknown sampled missing remains critical. Unsampled cache footprint can become prefetch/backlog.
- Then add async/prefetch generation and staged upload budgets.

### Sampled Missing Mid Feedback and Visible-Critical Split - 2026-06-03

Audit:

- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/summary.csv`
- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/table.md`
- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- touched source whitespace check passed except existing LF-to-CRLF warnings
- known `rayDir` shadow warnings remain

New default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=0`
- `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK`
- bounded `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET`

Implementation:

- Shader-side bounded hash buckets were attempted first, but the feedback-enabled capture hit the runtime compile/churn stop condition: after a `240s` timeout there was no useful log/capture output and `VENPOD.exe` was still consuming CPU. The shader/ABI changes were removed.
- Final feedback path is CPU-side `feedbackMode=cpu_projected_bounds`.
- It enumerates missing voxel interest with `SparseClipmapTileCache::CollectMissingVoxelInterest`, projects missing brick AABBs into the current camera, and estimates projected-visible versus projected-offscreen missing debt.
- Optional knobs:
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_CPU_MAX_COORDS`
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_SCREEN_PAD`
  - `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET_LOG_MAX`

Rows with background split `0.375`, clean throttle, CPU detail, backlog-aware pump, fallback classifier, fallback contract diagnostics, Far-SVO domain proof, and missing-sample feedback enabled:

| Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU ray | Missing | Valid/Invalid/Unknown | Far-SVO domain/material unknown | Sampled approx | Unsampled approx | Sampled % | Coverage | Reason | Miss/unsafe | Feedback ms | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `27.67` | `10.77` | `15.97` | `0.92` | `0.20` | `13.34` | `0` | `0/0/0` | `0/0` | `0` | `0` | `0` | `100` | `4` | `0/0` | `0.41` | no missing debt |
| walk fixed-dt | `600` | `73.21` | `22.82` | `10.55` | `34.24` | `14.89` | `15.68` | `290` | `0/74/216` | `44/44` | `220` | `70` | `75` | `96` | `0` | `0/0` | `0.52` | mostly projected-visible |
| walk realtime | `600` | `71.12` | `23.00` | `9.76` | `34.51` | `32.40` | `9.10` | `551` | `0/75/476` | `9/9` | `502` | `49` | `91` | `93` | `2` | `0/0` | `0.53` | coverage emergency, mostly projected-visible |
| high-alt late | `400` | `57.58` | `8.20` | `6.98` | `42.39` | `35.12` | `21.16` | `467` | `0/257/210` | `40/40` | `14` | `453` | `2` | `94` | `2` | `0/0` | `0.62` | broad cache debt; sampled subset unknown/invalid |
| noncapture walk | `600` | `122.01` | `67.22` | `34.92` | `12.17` | n/a | `50.90` | `449` | `0/18/431` | `4/4` | `379` | `70` | `84` | `95` | `0` | `0/0` | `0.77` | request/gen/surface dominate; missing still mostly projected-visible |

Decision:

- Feedback was implemented, but the final mode is CPU projected-bounds, not shader owner feedback.
- Fixed path has no missing-mid debt.
- Walk fixed-dt, walk realtime, and noncapture walk are mostly projected-visible (`75%`, `91%`, and `84%`) and still fallback-invalid/unknown. These are not safe visible-critical deferral candidates.
- Late high-alt shows the over-broad cache-interest problem clearly: only `2%` of frame-400 missing bricks projected visible, while `453/467` were projected offscreen/cache debt. The sampled subset still has no proven valid owner.
- The previous ring heuristic is confirmed too crude: it failed because it did not know sampled visibility or fallback ownership.
- `VISIBLE_CRITICAL_INTEREST_V2` was not implemented. A safe V2 must run before pump budgeting and make the pump prioritize visible-critical missing before prefetch. A post-hoc guard-only change would risk repeating the rejected ring heuristic.
- Async remains blocked because `fallbackValid=0`.
- All defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair remain default-off/default-neutral.
- Blunt pump cap and the removed age-priority queue prototype remain rejected.

Next safe implementation slice:

- Add a real pre-pump `visibleCriticalInterest` state using projected/sample feedback.
- Split current interest into sampled/likely-visible critical and cache/prefetch before the pump budget decision.
- Keep sampled/projected-visible unknown or invalid bricks critical.
- Prioritize visible-critical missing bricks before prefetch in the pump.
- For walk/realtime, do not expect visible-critical split alone to solve CPU: the feedback says most debt is visible, so fallback owner proof or reducing sampled missing debt is still required.
- For high-alt, visible-critical/cache split is promising because much of the debt is unsampled cache footprint.

### Streaming Playability Campaign Until Valid Candidate - 2026-06-04

Audit:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Campaign cycles:

1. Fresh baseline with background split `0.375`, clean throttle, backlog-aware pump, fallback diagnostics, Far-SVO proof, and missing-sample feedback.
2. Branch H: high-alt pre-pump visible-critical/cache split.
3. Branch W: shared voxel column cache.
4. Secondary probes: screen-critical request reuse env and surface upload interval env.
5. Combined playable stack validation.
6. Direct noncapture logging probe.

New default-off flags:

- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=0`

New/extended logs:

- `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP`
- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` now includes `columnCache=active/entries/hHit/hMiss/rHit/rMiss`

Accepted candidate:

- Branch H uses CPU projected missing-mid feedback before pump budgeting in high-alt views.
- It splits projected-visible/current-frame missing from cache debt.
- Projected-visible fallback-invalid/unknown bricks remain critical.
- Cache/prefetch bricks are not dropped; they remain tracked and drain opportunistically.
- Pump order prioritizes projected-visible missing through `SparseClipmapTileCache::PrioritizeVoxelGenerationCoords`.

High-alt result:

| Row | Frame | CPU | Req | Gen | Clip | GPU ray | Missing | Sampled/unsampled | Coverage | Reason | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline high-alt | `400` | `44.95` | `9.10` | `5.41` | `30.42` | `21.39` | `2384` | `194/2190` | `73` | `2` | `0/0` | broad cache debt forced catch-up |
| Branch H high-alt | `400` | `25.23` | `7.07` | `5.53` | `12.62` | `16.31` | `2074` | `66/2008` | `99` visible / `77` cache | `0` | `0/0` | accepted default-off high-alt win |

Rejected/diagnostic candidates:

- Shared voxel column cache helped fixed-dt walk (`CPU 41.08 -> 29.89`, `clip 17.51 -> 6.66`) but was unreliable in realtime (`CPU 31.14 baseline -> 48.84`, `clip 23.54`). Keep it default-off diagnostic/experimental.
- Screen-critical request reuse env did not reduce request cost enough and worsened the realtime row (`CPU 42.74`, `budgetReason=3`). Rejected.
- Surface upload interval lowered the selected-frame clip cost but moved work into raw/stage spikes (`rawMs=95.22`). Dirty-stage logs show full metadata work for small dirty/removed sets is the real surface-staging issue. Rejected.

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
| fixed playable | `380` | `12.06` | `6.56` | `4.72` | `0.78` | `8.45` | `31.47/n/a` | `0` | `0/0` | not a 60 FPS proof |
| realtime walk playable | `600` | `67.72` | `13.47` | `9.02` | `42.16` | `6.24` | `92.78/100.04` | `424` | `349/75` | not playable; clip/pump debt returned |
| high-alt playable | `400` | `35.83` | `8.97` | `6.15` | `20.71` | `11.14` | `69.13/n/a` | `2897` | `119/2778` | GPU is good; CPU/cache debt remains |

Visual:

- top-level `contact_sheet.png` includes fixed, walk, high-alt, playable, and rejected experiment frames.
- No new obvious holes or white-terrain regression from accepted Branch H.
- Known high-alt bright shoreline/terrain artifact remains.

Noncapture:

- Direct `build\bin\VENPOD.exe` probes with forced `VENPOD_LOG_FILE` detached and left live `VENPOD.exe` processes without producing the requested log. The processes were killed.
- This campaign has no clean new noncapture row. Do not claim validated 60 FPS.

Decision:

- Completion state is a strong default-off candidate plus exact remaining blocker table, not a validated 60 FPS candidate.
- Branch H is safe to keep default-off.
- Shared column cache remains default-off diagnostic only.
- Background split, clean throttle, backlog-aware pump, visible-critical prepump, shared column cache, fallback diagnostics/proofs, async, and bounded repair all remain default-off/default-neutral.
- Blunt pump cap, age-priority queue ordering, and the failed ring-only heuristic remain rejected.
- Async remains blocked for sampled movement debt because `fallbackValid=0`.

Current top blockers after this campaign:

- realtime/walk: sampled-visible mid-clipmap debt drives clip/pump spikes; cannot be deferred without ownership proof
- high-alt: Branch H fixes the broad cache catch-up mechanism, but cache backlog and sampled unknowns remain
- surface staging: dirty surface metadata still appears to do full metadata work for small dirty/removed sets
- request/gen: movement still spends roughly `13-22 ms` request and `7-10 ms` generation in representative rows
- noncapture validation: needs a reliable log-only harness before true 60 FPS can be claimed

Next recommended action:

- Keep Branch H default-off and continue from the remaining measured CPU blockers.
- For movement, do not do another visible-critical deferral unless sampled ownership becomes provable.
- Implement an incremental surface metadata staging path, or a request-prep incremental cache, as the next non-ownership CPU slice.
- Fix/add a noncapture log-only harness so final playable candidates can be judged against `16.67 ms` without capture-wrapper overhead.

Plain-English status:

- VENPOD has a stronger default-off candidate stack than before, and high-alt over-broad cache catch-up now has a real measured fix.
- The renderer is still not playable. Fixed frames are much better, but realtime walking still hits sampled-visible streaming debt and the combined playable capture row is far over budget.

### Streaming Playability Request/Stats Cycle - 2026-06-04

Audit:

- `build/captures/noncapture_request_detail_20260604`
- `build/captures/noncapture_request_detail_large_pool_20260604`
- `build/captures/noncapture_single_flush_walk_20260604`
- `build/captures/noncapture_bounded64_single_flush_walk_20260604`
- `build/captures/noncapture_critical_reuse_walk_20260604`
- `build/captures/noncapture_candidate_stack_accepted_20260604`
- consolidated: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Accepted additions:

- `PERF_SPARSE_REQUEST_DETAIL` splits request into hierarchy, terrain-critical, terrain surface prefetch, stats flush, and remaining `otherMs`.
- `VENPOD_SPARSE_STATS_SINGLE_FLUSH=0` is default-off. Opt-in avoids the second explicit sparse stats scan after deferred stats already flushed.
- `perf_noncapture_smoke.ps1` uses a larger sparse pool/page table in the candidate env and has `-Bounded64` / `-CriticalReuse` comparison switches. The default harness clears rejected critical reuse.

Latest accepted stack:

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

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Surface extract/stage | GPU ray | Missing | Sampled/unsampled | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `375` | `21.39` | `16.27` | `10.93` | `4.55` | `0.79` | n/a | `6.93` | `0` | `0/0` | `0/0` | close, not 60 |
| walk realtime | `600` | `112.09` | `64.87` | `33.39` | `23.55` | `7.87` | `19.46/5.76` | `18.08` | `447` | `378/69` | `0/0` | not playable; request/gen/surface/GPU dominate |
| high-alt | `399` | `53.11` | `29.52` | `7.92` | `5.49` | `16.10` | n/a | `14.49` | `1874` | `44/1830` | `0/0` | prepump helps, not 60 |

Rejected/not accepted:

- bounded64 as a performance stack component; it drove walk clip/pump to about `71/70 ms`
- screen-critical reuse at speed `64`; it starved critical requests and caused clip catch-up
- targeted coverage catch-up; it did not drain the frame-600 debt and was removed

Current blocker:

- Not a 60 FPS candidate.
- Walk no longer points to only one clip-pump fix in the accepted row. Top measured buckets are request `33.39 ms`, generation `23.55 ms`, surface extraction `19.46 ms`, GPU `18.08 ms`, and clip `7.87 ms`.
- Next branch: split request `otherMs` and inspect hidden-exact/surface extraction coupling. Do not add another deferral heuristic unless sampled ownership becomes provable.

All defaults remain unchanged, including background split, clean throttle, surface incremental metadata, stats single flush, backlog pump, visible-critical prepump, fallback diagnostics/proofs, async, bounded repair, and strict promotion policy.

### Streaming Playability Real Fix Campaign Resume - 2026-06-04

Persistent campaign goal:

- `streaming_playability_real_fix_campaign_20260604`
- local continuation handoff: `handoff.md`

Latest build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Latest accepted changes:

- `src/main_launcher.cpp`
  - public-open-frame latch fix: `sparseStartupPublicRenderOpenedFrame` now records the public-open frame once instead of being refreshed after open
  - added default-off `VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS=0` diagnostic and `directFootprint` logging
- `src/Simulation/SparseClipmap.h/.cpp`
  - added default-off direct-footprint diagnostic plumbing
  - behavior-preserving voxel-slot reuse: slot eviction now preserves the voxel payload vector capacity while resetting metadata
- `perf_noncapture_smoke.ps1`
  - added direct-footprint diagnostic switch

Rejected/diagnostic-only latest probes:

- post-open surface extraction cap at `4 ms`: rejected, moved debt and worsened walk CPU/clip
- direct-footprint voxel column path: diagnostic only, unstable across broader matrix
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`: did not solve walk spikes
- larger mid voxel pool and lower interest-percent tuning: rejected because missing could drop while `parentHeld` visual ownership failure exploded

Latest focused matrix:

- `build/captures/noncapture_voxel_slot_reuse_candidate_all_20260604/table.md`

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ray ms | Missing | Sampled/unsampled | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `23.96` | `21.41` | `4.15` | `16.35` | `0.90` | `0.00` | `6.16` | `0` | `0/0` | `0/0` | fixed close but still over 60 FPS budget |
| walk realtime | `900` | `77.86` | `46.70` | `12.39` | `3.15` | `31.12` | `17.11` | `15.52` | `436` | `366/70` | `0/0` | clip/parent-held catch-up returned |
| high-alt | `401` | `48.99` | `24.63` | `5.62` | `5.27` | `13.73` | `5.48` | `11.87` | `1813` | `41/1772` | `0/0` | improved but not 60 |

Current decision:

- The next blocker is parent-held LOD ownership/catch-up, not only missing-brick backlog.
- Larger pool tuning proved this: missing counts dropped, but `parentHeld` / LOD visual ownership failure exploded, which forced broad synchronous catch-up.
- Do not chase this with pool size, queue caps, or guard lowering.

Next real implementation branch:

1. Inspect `PS_Raymarch.hlsl` parent-held logic:
   - `RAY_DIAGNOSTIC_MID_PARENT_HELD`
   - preferred ring vs actual ring
   - `lodParentHeld` / parent-held ownership failure counters
2. Add bounded default-off parent-held ownership feedback or CPU projection:
   - map parent-held pixels back to preferred child clipmap coordinates/rings
   - avoid heavy shader hash buckets or compile-churn paths
3. Prioritize those child bricks in generation/pump instead of broad catch-up.
4. Validate noncapture fixed, realtime walk, and high-alt.

Do not mark the campaign complete until the parent-held branch is attempted or a concrete architecture/tooling blocker is documented.

### Edit / Build Latency

New edit latency logging showed successful brush paint feedback paths:

- frame `120` queued, frame `123` GPU-applied, `3` frames delayed, `16` records, `4` pages touched, `4` publishes completed
- frame `165` queued, frame `168` GPU-applied, `3` frames delayed, `29` records, `1` page touched
- frame `210` queued, frame `213` GPU-applied, `3` frames delayed, `7` records, `2` pages touched, `2` publishes completed

No successful sampled edit hit CPU fallback, missing resident hints, overflow, or delta mismatch.

The edit smoke failed its final coverage/path gate:

- `cases=3/4`
- `caseQueued=3/3/3/0`
- `pathCells=0`

Treat that run as a partial edit-latency probe, not edit correctness validation.

Current edit/build interpretation:

- brush GPU feedback adds about a 3-frame feedback/apply delay
- visible edit correctness is not yet automatically measured; `firstVisibleFrame=0` until material-diff validation exists
- edit/build lag is currently masked by the baseline render frame cost and sparse request/generation spikes

## Build / Test Status

Latest same-frame parallel generation cycle:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_generation_candidate_v2_20260604/`

Decision:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0` was added default-off, but did not activate in the latest matrix (`parallelPumpActive=0`), so it is not an accepted playability fix.
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=0` was added default-off and did activate, but is rejected as a candidate stack member.
- Parallel exact generation regressed walk realtime to `rawMs=209.53`, `cpuUpdateMs=146.40`, `clipMs=81.87`, `missingVoxel=527`; high-alt regressed to `rawMs=176.07`, `cpuUpdateMs=77.50`.
- Miss/unsafe stayed `0/0`, but that is not success because frame time and streaming debt regressed.
- Do not retry direct exact generation as the next branch. The remaining blocker is the cached exact generation/request/surface pipeline around visible sampled terrain debt.

Latest sampled missing-mid feedback pass:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Known warning:

- existing `rayDir` shadow warnings in `src/main_launcher.cpp` may still appear in full rebuilds
- the build script may still print the existing trailing `vswhere.exe` lookup warning after the build has succeeded

## Current Recommended Next Work

Do this next:

1. Continue movement sparse CPU work by converting feedback into a real pre-pump visible-critical/cache-interest state.
   - The latest pass implemented bounded missing-preferred-mid feedback as CPU projected-bounds diagnostics after shader feedback hit a compile/churn stop condition.
   - Walk fixed-dt/realtime/noncapture debt is mostly projected-visible and fallback-unknown/invalid; do not defer that as prefetch.
   - Late high-alt debt is mostly unsampled/cache footprint; it is the strongest candidate for a visible-critical/cache split.
   - A safe V2 must run before pump budgeting and prioritize visible-critical missing before prefetch.
   - Do not do another ring heuristic, queue-order tweak, coverage guard lowering, or unknown-to-async move.

2. Keep the lower-res background split default-off while validating it longer.
   - Do not make it default or playable-mode default yet.
   - Use absolute-frame stress-camera settings for matched high-alt validation.
   - Continue visual/contact-sheet checks for lower-res background artifacts.

3. Reduce secondary CPU costs after visible-critical classification is addressed.
   - request prep remains around `16-20 ms` in the latest movement rows
   - surface staging/extraction remains material in movement rows (`14-18 ms` in selected rows)
   - trim/replacement scans are visible but not the first fixed-camera cost

4. Keep bounded foreground promotion default-off until there is performance headroom.

5. Resume correctness validation later:
   - longer movement bound-64 and bound-32 comparison
   - matched-frame basin audit if possible
   - final default candidate decision only after performance is no longer the first blocker

## Do Not Repeat

- Do not call the Far-SVO radius issue unresolved; radius 12 fixed that tested domain gap.
- Do not reapply the rejected per-pixel Far-SVO traversal helper.
- Do not make `bounded_repair` default in the performance pass.
- Do not return to mode 70 before performance/root attribution work.
- Do not rely on `miss=0` alone as a proof of visual coherence.
- Do not treat wrapper dark-image failures on debug-color BMPs as automatic renderer failures when logs/BMPs exist.
- Do not use strict startup hidden-exact repair blocking as a default; it can hold public render indefinitely.

## High-Value Resume Commands

```powershell
git status --short
rg -n "PERF_FRAME_END|PERF_SPARSE_SURFACE_PROMOTION_POLICY|PERF_CAMERA_EXPOSURE|PERF_SPARSE_EDIT_LATENCY" src assets engine_capture_smoke.ps1 *.ps1
Get-Content build\captures\perf_root_cause_20260602\perf_cost_table.md
Get-Content build\captures\perf_root_cause_20260602\edit_latency_events.txt
Get-Content build\captures\gpu_raymarch_floor_ablation_20260602\raymarch_floor_table.md
Import-Csv build\captures\gpu_raymarch_floor_ablation_20260602\raymarch_floor_summary.csv | Format-Table -AutoSize
Get-Content build\captures\background_pass_split_or_mask_20260602\background_split_table.md
Import-Csv build\captures\background_pass_split_or_mask_20260602\background_split_summary.csv | Format-Table -AutoSize
Get-Content build\captures\background_split_content_fix_20260602\background_split_content_table.md
Import-Csv build\captures\background_split_content_fix_20260602\background_split_content_summary.csv | Format-Table -AutoSize
Get-Content build\captures\movement_sparse_cpu_update_reduction_20260602\cpu_sparse_table.md
Import-Csv build\captures\movement_sparse_cpu_update_reduction_20260602\cpu_sparse_summary.csv | Format-Table -AutoSize
Get-Content build\captures\overnight_playability_ladder_20260603\overnight_table.md
Import-Csv build\captures\overnight_playability_ladder_20260603\overnight_summary.csv | Format-Table -AutoSize
Get-Content build\captures\mid_clipmap_drain_and_reuse_20260603\mid_clipmap_drain_table.md
Import-Csv build\captures\mid_clipmap_drain_and_reuse_20260603\mid_clipmap_drain_summary.csv | Format-Table -AutoSize
Get-Content build\captures\async_mid_clipmap_fallback_validity_20260603\async_mid_fallback_table.md
Import-Csv build\captures\async_mid_clipmap_fallback_validity_20260603\async_mid_fallback_summary.csv | Format-Table -AutoSize
Get-Content build\captures\mid_clipmap_fallback_contract_ownership_20260603\fallback_contract_table.md
Import-Csv build\captures\mid_clipmap_fallback_contract_ownership_20260603\fallback_contract_summary.csv | Format-Table -AutoSize
Get-Content build\captures\autonomous_streaming_playability_engineering_20260603\table.md
Import-Csv build\captures\autonomous_streaming_playability_engineering_20260603\summary.csv | Format-Table -AutoSize
Get-Content build\captures\sampled_missing_mid_feedback_and_visible_critical_split_20260603\table.md
Import-Csv build\captures\sampled_missing_mid_feedback_and_visible_critical_split_20260603\summary.csv | Format-Table -AutoSize
rg -n "PERF_SPARSE_MID_CLIPMAP_FALLBACK frame=(240|380|600)" build\captures\async_mid_clipmap_fallback_validity_20260603\*\venpod_runtime.log
rg -n "PERF_FRAME_END frame=240|PERF_CAMERA_EXPOSURE frame=240|PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=240" build\captures\contract_policy_default_off_20260602\venpod_runtime.log
rg -n "PERF_FRAME_END frame=240|PERF_CAMERA_EXPOSURE frame=240|PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=240" build\captures\contract_policy_bounded64_fixed_final_20260602\venpod_runtime.log
rg -n "PERF_FRAME_END frame=480|PERF_CAMERA_EXPOSURE frame=480|PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=480" build\captures\contract_policy_bounded64_walk_20260602\venpod_runtime.log
rg -n "PERF_FRAME_END frame=240|PERF_CAMERA_EXPOSURE frame=240|PERF_SPARSE_SURFACE_PROMOTION_POLICY frame=240" build\captures\contract_policy_bounded64_highalt_20260602\venpod_runtime.log
```

## Research Questions

- What public-frame invariant should a streaming sparse voxel renderer enforce before presenting gameplay?
- Should public readiness be camera-footprint based instead of global cache/readiness based?
- Can lower LOD own pixels inside the exact band, and what deterministic equivalence/error bound makes that legitimate?
- Is bounded foreground repair a better contract than zero-feedback hidden-exact proof?
- What is the cheapest safe alternative to per-pixel Far-SVO recovery in the miss path?
- How should shoreline ownership be encoded/resolved at coarse Far-SVO mixed cells?
- What shader/raymarch architecture can meet 60 fps while preserving exact foreground ownership?

### Ownership-Stage Queue Branch - 2026-06-05

Default-off candidate:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS=0`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET=8`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET=8`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS`

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_explicit_lanes_candidate`
- updated `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- updated `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`

Rows:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| ownership queues | `44.05/14.51` | `117.65/79.63` | `98.90/60.35` | rejected |
| ownership queues + explicit lanes | `48.94/12.13` | `90.49/80.23` | `84.48/49.17` | rejected |

Decision:

- Ownership queues are default-off and not a playable stack member.
- With current source lanes, nearly all stage work is visible/critical, so there is no safe noncritical stage work to budget.
- With explicit source lanes, prefetch queues appear, but fixed/walk/high-alt regress versus the current validated stack.
- Do not repeat upload/surface-only budget branches. The next implementation must move ownership-critical classification earlier into request/admission and generation.

## Active Goal / Branch Ladder Closure - 2026-06-05

Compact active-goal resume file:

- `active-goal-handoff.md`

Post-cleanup verification:

- `.\build.ps1 -Config Release`: passed before compaction after removing transient branches.
- `ctest --test-dir build --output-on-failure -C Release`: passed after compaction, `1/1`.

Latest accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | Surface extract/stage | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `0.00` | `26.50/2.35` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `6.59` | `9.58/2.57` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `4.93` | `6.64/1.80` | `10.26` | `1746/0` | `100` | `0/0` |

Rejected/not-accepted branches after the hidden-exact probes:

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

Current decision:

- No validated 60 FPS candidate exists.
- The strongest opt-in stack is materially better but still too slow: fixed about `23 ms`, walk about `58 ms`, high-alt about `51 ms`.
- Repeated isolated knobs are no longer credible as the main path. They either regress representative rows or move work into guarded catch-up.
- The next real fix is an ownership-aware streaming state machine: persistent lane state across request, generation, upload/apply, surface extraction, and publish queues.
- Visible/public-critical and sampled fallback-invalid/unknown work stays synchronous/readiness-gated; only cache/prefetch or CPU-proven fallback-valid work can move async.

## Generated-Lane Accounting Slice - 2026-06-05

Behavior-neutral accounting retained:

- `SparseVoxelWorldStats::generated*LaneBricksLastFrame`
- `PERF_SPARSE_GENERATED_LANES`
- generated-lane CSV columns in `perf_noncapture_smoke.ps1`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: no code whitespace errors, only existing LF-to-CRLF warnings.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generated_lane_accounting_v2_20260605`

Rows:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Queued gen lanes prefetch/visible/public | Generated lanes prefetch/visible/public | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.54` | `18.76` | `3.98` | `13.99` | `0.77` | `6.71` | `915/121/0` | `24/121/0` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `67.95` | `55.11` | `17.91` | `16.07` | `21.10` | `17.24` | `4929/142/47` | `15/88/47` | `523/303` | `94` | `0/0` |
| high-alt | `360` | `51.02` | `28.72` | `8.27` | `5.14` | `15.31` | `6.37` | `6968/11/8` | `20/11/8` | `1769/0` | `100` | `0/0` |

Decision:

- Fixed and walk generation cost is mostly visible/public-critical.
- Cache/prefetch async is useful but not sufficient for the current wall.
- The next implementation must accelerate visible/public exact generation while preserving same-frame ownership; do not defer sampled unknown work.

### Active Goal Compact Handoff - 2026-06-05

New compact resume file:

- `active-goal-handoff.md`

Active goal remains `streaming_playability_real_fix_campaign_20260604`.

Campaign start/update on 2026-06-05:

- Build passed after final accepted-state cleanup: `.\build.ps1 -Config Release`
- Test passed after final accepted-state cleanup: `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- Known `rayDir` shadow warnings remain.

Two hidden-exact branches were attempted, measured, rejected, and removed from live code:

- post-open hidden-exact prefetch lane: found almost no maintenance-lane work and regressed fixed/walk
- hidden-exact direct parallel generation: improved fixed CPU/gen but regressed or mixed walk/high-alt CPU, so it is not an accepted candidate

Fresh accepted-state baseline from the campaign cycle:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `10.26` | `1746/0` | `100` | `0/0` |

Do not retry these hidden-exact branches as simple toggles. Next real branch should target request/gen coupling or exact-generation pipeline architecture with staged worker/apply accounting.

### Streaming Playability Campaign Continuation - 2026-06-04

Active goal:

- `streaming_playability_real_fix_campaign_20260604`
- Do not mark complete. No validated 60 FPS candidate exists.

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Continuation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_table.md`
- rejected branch summaries in the same directory:
  - `rejected_generation_cap8_summary.csv`
  - `rejected_critical_reuse_tol2_summary.csv`
  - `rejected_surface_skip_sort_summary.csv`
  - `surface_extract_sort_fix_summary.csv`

Branches tried in the continuation:

- Shader parent-held feedback was already rejected earlier because it caused runtime DXC compile/churn.
- CPU projected parent-held feedback remained default-off diagnostic only; it accepted almost no useful child coords and did not fix walk f600.
- `VENPOD_SPARSE_GENERAL_GENERATION_MAX_BUDGET=8` was rejected and removed from runtime. It worsened walk f600 by deferring necessary work.
- Screen-critical reuse forward tolerance was rejected and removed. It activated reuse but regressed badly (`raw 195.84`, `CPU 116.81`, `coverage 94`, `budgetReason 2`).
- Surface timed skip-sort was rejected and removed. It did not reduce `surfaceExtractMs` or total frame time.
- Timed surface extraction now avoids an unused global queue sort before per-class extraction. This is behavior-preserving cleanup; per-class ordering remains unchanged.

Latest combined matrix, with background split/playable quality, clean throttle, stats single flush, backlog-aware pump, visible-critical prepump, and fallback diagnostics:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU ray | Missing | Sampled/unsampled | Coverage | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `103.13` | `43.44` | `19.55` | `13.98` | `9.90` | `7.50` | n/a | `13.11` | `28` | `24/4` | `99` | `0/0` | not a stable 60 row |
| walk realtime | `600` | `144.49` | `115.15` | `26.16` | `23.74` | `65.22` | `47.49` | `13.26/3.48` | `20.94` | `445` | `376/69` | `94` | `0/0` | sampled-visible debt, coverage emergency |
| high-alt | `378` | `45.14` | `23.26` | `5.07` | `5.48` | `12.70` | `4.96` | n/a | `10.71` | `1833` | `33/1800` | `99` visible / `80` cache | `0/0` | visible-critical prepump helps high-alt |

Current blocker classification:

- GPU has a credible default-off path via background split.
- High-alt CPU has a credible default-off cache/critical split via `VISIBLE_CRITICAL_PREPUMP`.
- Walk/realtime is still blocked by sampled/projected-visible mid-clipmap debt with fallback unknown/invalid. Unknown fallback remains critical.
- Queue caps, generation caps, reuse tolerance, and async of unknown bricks are not safe fixes.

Next implementation slice:

1. Build an ownership-aware streaming pipeline:
   - visible-critical sampled bricks stay synchronous or readiness-blocked
   - cache/prefetch bricks are async/deferred
   - worker-generated CPU payloads apply on safe frame boundaries
   - upload/apply and surface extraction/staging have separate budgets and backlog age logs
   - generation/edit stamps discard stale completions
2. Add the metadata or feedback needed to convert movement fallback unknowns into explicit valid/invalid:
   - Far-SVO material/occupancy/shoreline proof
   - coarser LOD error/ray-angle proof
   - deterministic water/sky proof or sampled owner feedback
3. Validate with noncapture rows before claiming playable/60 FPS.

Plain-English state:

- The engine is not failing because the last cap was tuned badly. It is failing because sampled visible walk debt has no CPU-provable fallback owner, while too much streaming/generation/surface work is still done synchronously on the main frame.
### Streaming Playability Real Fix Campaign - Active Handoff - 2026-06-04

Active goal:

- `streaming_playability_real_fix_campaign_20260604`
- This is still active. Do not mark complete: no validated 60 FPS/noncapture playable candidate exists.

Latest build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Accepted cleanup:

- `src/Simulation/SparseVoxelWorld.cpp`
- `src/Simulation/SparseVoxelWorld.h`
- `src/main_launcher.cpp`
- Behavior-preserving surface extraction queue hygiene:
  - prune stale surface extraction queues when no pending surface payload exists;
  - remove stale per-coord surface queue aliases on failed direct extraction;
  - skip launcher surface extraction attempts when pending surface payload count is zero.

Authoritative latest matrix:

- `build/captures/current_after_surface_queue_cleanup_20260604`
- campaign copies:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_table.md`

| Scenario | Frame | Raw ms | CPU update ms | Request ms | Gen ms | Clip ms | Surface extract ms | GPU ray ms | Missing voxel | Sampled/unsampled | Coverage | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `40.06` | `9.07` | `2.49` | `5.60` | `0.98` | n/a parsed | `7.22` | `0` | `0/0` | `100` | `0/0` | still has post-open surface bursts (`29.62 ms` at frame `384`) |
| walk realtime | `600` | `105.52` | `65.19` | `30.04` | `26.18` | `8.92` | `15.04` | `36.16` | `435` | `366/69` | `95` | `0/0` | not playable; request/gen/surface/GPU all high |
| high-alt | `400` | `69.39` | `44.03` | `12.73` | `11.19` | `20.10` | n/a parsed | `16.61` | `1994` | `62/1932` | visible `99`, cache `78` | `0/0` | visible-critical safe, cache debt remains |

Rejected/default-off probes in this continuation:

- direct footprint columns: rejected after full matrix regression.
- shared column cache: rejected (`walk CPU 64.48 ms`).
- general surface budget `4`: rejected.
- post-open prepublish max `4 ms`: rejected; fixed improved but walk/high-alt catch-up worsened.
- hidden exact post-open surface budget `16`: rejected; fixed improved but walk/high-alt worsened.
- bounded64 current stack: rejected as perf answer; walk demoted/blocked at frame `600`.

Current diagnosis:

- The engine is still slow because the exact sparse streaming path is not asynchronous/ownership-staged:
  - exact request/prep and resident readiness touches are expensive;
  - exact page generation is still main-thread;
  - prepublish/surface extraction still bursts;
  - mid visible-critical split helps high-alt public coverage but does not solve sampled walk debt;
  - GPU split helps but walk still has GPU/gap cost.
- Async mid generation remains blocked for sampled unknown/invalid fallback.
- Exact sparse page generation is the next practical async slice, but `GenerateBrickWithCachedTerrainColumns` must first become a worker-safe pure payload builder.

Next implementation slice:

1. Extract worker-safe exact page payload generation:
   - local column cache;
   - no mutation of `m_surfaceTerrainColumnCache`;
   - no pool/generated/upload/surface state mutation;
   - edit revision/snapshot guard, or async disabled/discarded when edits active.
2. Add default-off async exact page generation:
   - worker produces `GeneratedSparseBrick`;
   - main thread applies completed payloads at frame boundary;
   - stale/no-longer-requested/edited completions discarded.
3. Log async queue/apply/upload/surface backlog and validate fixed/walk/high-alt/noncapture.

Do not repeat:

- Do not cap/drop visible or unknown ownership work to fake FPS.
- Do not treat miss/unsafe zero as enough while raw frame time is bad.
- Do not revive blunt pump cap, age priority, ring-only visible-critical heuristic, Far-SVO suppression, or quality-scale ownership suppression.

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

Branch outcomes:

- all-class async exact generation was rejected. Fixed improved, but walk realtime regressed: `cpuUpdateMs 75.77 -> 100.37`, `clipMs 45.99 -> 56.60`, `pumpMs 31.28 -> 54.29`, `missingVoxel 453 -> 512`. Cause: async visible/current exact pages create readiness debt that mid-clipmap catch-up repays.
- speculative-only async exact generation queued/applied `0` async bricks in sampled rows. It is safe as a default-off diagnostic, not a fix.
- direct exact generation using `SparseTerrainGenerator::GenerateBrick` was rejected. Walk remained worse than baseline (`cpuUpdateMs 82.33`, `clipMs 46.30`) and no playable candidate appeared.

Current representative rows:

| Run | Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU | Missing | Miss/unsafe | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | walk realtime | `600` | `75.77` | `15.22` | `14.53` | `45.99` | `31.28` | `19.02` | `453` | `0/0` | not playable |
| async all classes | walk realtime | `600` | `100.37` | `26.59` | `17.15` | `56.60` | `54.29` | `5.62` | `512` | `0/0` | rejected |
| async speculative only | walk realtime | `600` | `67.11` | `30.03` | `28.09` | `8.97` | `6.49` | `6.12` | `454` | `0/0` | no async work occurred |
| direct exact | walk realtime | `600` | `82.33` | `17.86` | `18.16` | `46.30` | `31.72` | `19.14` | `469` | `0/0` | rejected |

Decision:

- no validated 60 FPS/playable candidate exists;
- background split, clean throttle, backlog-aware pump, visible-critical prepump, exact async, exact direct generation, and bounded repair remain default-off;
- visible exact async is not safe without an ownership/readiness state machine;
- async is still appropriate only for cache/prefetch or CPU-proved fallback-valid work;
- next real implementation must split ownership-aware interest before request/generation/pump, keep sampled fallback-unknown work guarded, then worker-generate only cache/prefetch or fallback-valid payloads with frame-boundary apply and upload/surface budgets.

### Active Campaign Control - 2026-06-04

The active `/goal` is `streaming_playability_real_fix_campaign_20260604`. The durable continuation handoff is now `handoff.md`; update it after every meaningful branch so compaction cannot lose the campaign state.

Current rule: do not stop after one diagnostic, one failed heuristic, or one small partial win. Continue through measured branches until there is either a validated noncapture playable candidate, a strong candidate with an exact remaining bottleneck table, or a hard architecture/tool blocker proven after multiple safe attempts.

Latest branch state to preserve:

- `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` is a default-off high-alt/cache partial still in the worktree. It reduced high-alt clip/pump from about `16.58/5.40` to `13.50/1.74`, but it does not solve walk/realtime and is not a playable candidate.
- Post-open surface cap, surface strict/buried, global generation budget `8`, broad exact async, direct exact generation, blunt pump cap, age-priority, and ring-only visible-critical heuristics remain rejected.
- Walk/realtime remains blocked by sampled/projected-visible fallback-unknown mid debt plus exact request/generation/surface/GPU costs. Unknown fallback must stay critical.
- Next real implementation is the ownership-aware streaming state machine: visible sampled unknown work guarded, cache/prefetch work async/deferred, frame-boundary apply/upload/surface budgets, and owner proof metadata/feedback for movement debt.

### Visible Priority Pump Branch - 2026-06-04

New default-off knob: `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP=0`.

Result:

- Build passed and ctest passed `1/1`.
- The branch prioritizes CPU-projected-visible missing mid-voxel coords in the existing voxel generation queue. It does not change budgets, lower guards, or treat unknown fallback as safe.
- Walk baseline rerun: raw `69.36`, CPU `58.73`, clip/pump `28.07/25.79`.
- Walk with priority: raw `79.40`, CPU `48.62`, clip/pump `21.35/19.30`, prioritized `335`; CPU partial but raw worsened.
- Walk with priority + parallel mid pump: raw `64.18`, CPU `43.18`, clip/pump `11.94/9.87`.
- Walk with priority + parallel mid + exact parallel: raw `59.55`, CPU `44.41`, gen `10.01`, clip/pump `22.83/10.75`; best new walk raw but still far over budget.
- High-alt with the parallel stack regressed to raw `75.05` / CPU `41.20`, so the stack is rejected globally.

Decision: keep `VISIBLE_PRIORITY_PUMP` as a default-off ownership-lane primitive/diagnostic, not as a playable candidate. Parallel mid/exact stack is not accepted globally. Next implementation should move lane classification into `SparseClipmapTileCache` with persistent visible/cache tags and separate critical/cache backlog accounting.

### Streaming Playability Candidate Stack - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- Release build passed after reverting the rejected priority-side-effect patch.
- ctest passed `1/1`.
- Known `rayDir` warnings remain.

Harness:

- `perf_noncapture_smoke.ps1` now has `-WalkFixedDtMs`, default `0`.

Accepted stack, still default-off:

- playable render scale + background split `0.375`
- clean terrain prefetch throttle
- backlog-aware pump
- incremental pressure trim with scan budget `8192`
- parallel visible mid-voxel pump with `4` workers
- diagnostics enabled during validation

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `27.45` | `23.00` | `4.88` | `17.30` | `0.81` | `0.00` | n/a | `8.08` | `0/0` | `0/0` |
| walk realtime | `600` | `62.73` | `43.05` | `16.06` | `16.83` | `10.14` | `7.60` | `11.88` | `16.56` | `444/284` | `0/0` |
| high-alt | `386` | `51.50` | `26.41` | `4.67` | `7.08` | `14.65` | `6.39` | n/a | `11.79` | `1451/0` | `0/0` |

Fixed-dt walk evidence: baseline raw/CPU `158.68/137.57`, incremental trim `71.10/51.18`, incremental trim + parallel mid pump `60.24/41.75`.

Rejected this continuation: streaming lane scheduler, priority-side-effect removal patch, trim+parallel surface, trim+cache-only defer as global stack, worker column cache, exact parallel, surface sort cache, hidden exact scan budget, surface-ready publish queue, and terrain-critical inline surface defer as global stack.

Decision:

- Completion B state: strong validated default-off candidate and exact remaining blocker table, not full 60 FPS.
- No defaults changed.
- Candidate visual captures passed smoke/contact-sheet generation. High-alt may still show the older bright shoreline/terrain artifact class; this stack did not fix that correctness debt.

Remaining blockers:

- fixed: exact/general generation about `17 ms`;
- walk: request, generation, clip/pump, surface extraction, and GPU are all material; sampled missing mid debt remains fallback-unknown/invalid;
- high-alt: improved but still has broad unsampled cache debt and clip/pump cost.

Next implementation: persistent visible/cache/prefetch lanes inside clipmap streaming state, request-prep/touch cache by source/lane without repeating the fast-resident coverage-collapse failure, visible-critical/cache surface budgets, and a diagnostic-off run of the accepted stack to measure true overhead.

### Terrain-Critical Parallel Generation Retained Partial - 2026-06-04

Latest continuation state for active goal `streaming_playability_real_fix_campaign_20260604`.

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained default-off candidate:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`

Implementation:

- `SparseVoxelWorld::PumpGenerationForCoordsParallel` batches current-frame terrain-critical exact sparse brick payload generation on worker threads.
- Workers use local terrain column caches.
- Main thread applies generated payloads through the existing resident/upload/surface path.
- This is same-frame required work, not async deferral. It does not make unknown fallback safe.

Artifacts:

- `build/captures/candidate_terrain_critical_parallel_generation_min16_20260604`
- `build/captures/candidate_terrain_critical_parallel_generation_min16_diagnostics_20260604`

Diagnostic validation:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.85` | `19.36` | `3.67` | `14.98` | `0.71` | `0.00` | `18.00` | `6.31` | `0/0` | `0/0/0` | `100` | `0/0` |
| walk realtime | `600` | `50.55` | `50.79` | `15.27` | `14.97` | `20.53` | `9.16` | `11.21` | `19.30` | `455/254` | `0/130/325` | `94` | `0/0` |
| high-alt | `360` | `47.65` | `26.71` | `5.72` | `6.55` | `14.43` | `4.05` | `3.95` | `9.96` | `1759/0` | `0/1238/521` | `100` | `0/0` |

Rejected/removed in this continuation:

- resident-touch cache: failed to improve representative request cost.
- surface visible-lane pump: worsened walk raw and produced high-alt clip spike.

Not accepted globally:

- background pass scale `0.25`: high-alt/GPU helper but worse walk raw than retained min16 stack.
- mid interest interval `2`: high-alt/clip helper but worse fixed/walk raw.

Decision:

- keep terrain-critical parallel generation default-off as a safe partial;
- do not call it a playable candidate;
- no defaults changed;
- background split, clean throttle, backlog-aware pump, fallback diagnostics, async, and bounded repair remain default-off;
- blunt pump cap, age-priority prototype, ring-only heuristic, broad exact async, direct exact generation, resident touch cache, and surface visible-lane pump remain rejected.

Current state of the engine:

- Fixed can produce near-20 ms raw rows but still has generation/surface burst risk.
- Walk realtime is still the representative blocker at about `50 ms` raw with distributed request, generation, clip/pump, surface, and GPU costs.
- Walk sampled missing mid debt remains fallback-invalid/unknown, so it cannot be deferred safely.
- High-alt is no longer the sole blocker but still carries broad cache debt and clip/pump work.

Next real implementation:

- persistent ownership lanes in streaming state, not more local caps:
  - visible/current ownership lane: sync/guarded;
  - cache/prefetch lane: persistent backlog, async CPU payload generation;
  - frame-boundary apply/upload/surface budgets by lane;
  - request-prep/touch cache by lane/source;
  - ownership metadata to turn sampled fallback unknown into explicit valid/invalid.

### Streaming Lane Queue Priority Branch - 2026-06-04

Retained default-off branch:

- `VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY=0`
- `SparseVoxelWorldConfig::streamingLaneQueuePriority`
- `SparseVoxelWorldStats::streamingLaneQueuePriorityActive`
- `perf_noncapture_smoke.ps1 -StreamingLaneQueuePriority`

Implementation:

- Exact sparse generation/upload/surface queues already counted `SparseStreamingLane`, but scoring ignored it.
- With the new flag enabled, queue score includes lane priority inside the same residency class: `publicCritical > visible > prefetch > cache`.
- This does not drop work, lower guards, alter fallback classification, or make unknown fallback safe.
- Default behavior is unchanged.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` warnings remain.

Artifacts:

- `build/captures/candidate_streaming_lane_queue_priority_diagnostics_20260604`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png` (log-only; noncapture harness emitted no BMPs)

Rows, retained stack:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` | `0/0` | `0/0/0` | `100` | `0/0` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` | `447/248` | `0/93/354` | `95` | `0/0` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` | `1755/0` | `0/1235/520` | `100` | `0/0` |

Decision:

- Keep lane queue priority as a safe default-off partial.
- It improved walk CPU from the prior retained terrain-critical row (`50.79 -> 39.44 ms`) and preserved `miss/unsafe=0/0`.
- It is not a 60 FPS/playable candidate: walk raw remains about `54 ms`; fixed remains about `20 ms`.
- No defaults changed.

Rejected after this branch:

- parallel surface extraction: no material win, fixed regressed;
- parallel exact generation: walk CPU improved but raw worsened and high-alt regressed;
- mid worker column cache: severe walk/high-alt CPU regression;
- direct footprint columns: severe request/generation regression;
- mid pump `8` workers: higher overhead and worse raw;
- background pass scale `0.25`: GPU down, CPU/raw exploded;
- general generation budget `8`: worsened all scenarios;
- buried-solid surface fast path: worsened walk/high-alt heavily.

Current state:

- fixed is close to budget but still over, with exact generation/surface bursts;
- walk realtime remains the representative blocker: request, generation, clip, surface extraction, GPU, and sampled fallback-invalid/unknown mid debt are all material;
- high-alt is stable but carries broad unsampled cache backlog and clip/pump/GPU cost.

Next real implementation:

- persistent ownership lanes in streaming state:
  - lane-tag clipmap interest and exact requests when they are created;
  - keep sampled visible fallback-invalid/unknown work synchronous/guarded;
  - split cache/prefetch into persistent backlog with age;
  - worker-generate only cache/prefetch payloads or CPU-proved fallback-valid work;
  - apply/upload/surface-extract by lane with separate budgets;
  - add owner metadata/feedback only to convert sampled unknowns into explicit valid/invalid.

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

- A default-off persistent terrain column cache was tried to reduce exact generation and surface extraction.
- Capped variants were tested at `262144` and `65536` entries.
- The prototype was rejected and removed from code.
- No `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_*` env knob remains.
- All defaults remain unchanged.

Key rows:

| Row | Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| retained publish-lane baseline | fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | reference |
| retained publish-lane baseline | walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | reference |
| retained publish-lane baseline | high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | reference |
| persistent column cache `65536` | fixed | `384` | `24.50` | `23.69` | `3.91` | `18.96` | `0.81` | `0.00` | `25.66/n/a` | `7.20` | rejected fixed regression |
| persistent column cache `65536` | walk realtime | `600` | `50.73` | `47.94` | `14.07` | `16.81` | `17.04` | `4.45` | `12.59/3.02` | `10.68` | rejected partial/mixed |
| persistent column cache `65536` | high-alt | `395` | `43.95` | `24.93` | `3.28` | `6.49` | `15.16` | `4.45` | n/a | `10.28` | rejected CPU/clip regression |

Next measured branch:

- inspect request planning/cache behavior before trying more generation micro-caches;
- current rows repeatedly show material request cost and `centerDelta=0/0/0 fullRebuild=1`;
- determine whether request sets are being rebuilt despite unchanged request keys;
- if yes, implement a behavior-preserving request reuse path keyed by request center, forward cone, budgets, and policy flags;
- do not drop requests, do not defer sampled fallback-invalid/unknown work, and do not weaken coverage/critical guards.

### Campaign Continuation - Request-Side Existing Flags Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_side_existing_flags_rejected_20260605.md`

Tested existing default-off flags:

- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=1`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=1`

Decision:

- Neither flag is retained as a playable candidate stack member.
- Pressure-trim guard reduced request time in some rows, but no-heavy walk regressed to raw/CPU `74.33/52.83` with clip/pump `23.48/21.22`.
- Hidden tracked scan budgeting did not improve representative walk (`74.03/47.07`) and still hit scan budgets.
- No code changed for these probes and all defaults remain unchanged.

Best retained reference remains:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` |

Next implementation:

- stop chasing isolated request micro-flags;
- implement the real ownership-aware streaming state-machine slice:
  - persistent visible-critical/cache/prefetch lanes;
  - sampled fallback-invalid/unknown work remains synchronous/guarded;
  - cache/prefetch work gets separate generation/apply/upload/surface budgets;
  - async only for cache/prefetch or CPU-proved fallback-valid work;
  - owner metadata/feedback remains required before sampled unknown fallback can become async.

### Campaign Continuation - Generation Lane Budget Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generation_lane_budget_rejected_20260605.md`

Decision:

- default-off generation lane budget was implemented, tested, rejected, and removed;
- no `VENPOD_SPARSE_GENERATION_LANE_BUDGET` or `VENPOD_SPARSE_GENERATION_CACHE_PREFETCH_BUDGET` env remains;
- representative walk regressed to raw/CPU `65.15/63.94` with clip/pump `36.17/23.78`;
- fixed raw reached `15.59`, but CPU/gen/surface remained too high and this did not become a candidate stack;
- high-alt raw/CPU was `44.25/28.32`, still not enough to retain over the walk regression;
- all defaults remain unchanged.

Current next branch:

- probe existing default-off parallel exact generation infrastructure because generation remains a repeated fixed/walk bucket;
- do not implement another queue-order cap unless ownership/fallback proof changes.

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

Decision:

- default-off voxel-footprint signature reuse was implemented, tested, rejected, and removed.
- no `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_FOOTPRINT_REUSE` env remains.
- no `-MidClipmapVoxelFootprintReuse` noncapture harness switch remains.
- all defaults remain unchanged.

Rows:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `24.88` | `25.05` | `22.96` | `22.64` | `0.84` | `1.25` | `0.00` | `0.00` | `0` | `0` | `100` | `100` | neutral fixed |
| walk realtime | `600` | `52.16` | `59.33` | `45.85` | `78.03` | `17.39` | `45.00` | `5.44` | `32.09` | `447` | `441` | `95` | `94` | rejected representative regression |
| high-alt | `360` | `50.05` | `53.72` | `29.79` | `34.47` | `16.39` | `18.82` | `5.36` | `5.92` | `2228` | `2228` | `99` | `99` | rejected high-alt regression |

Mechanism:

- walk candidate frame `600`: `newV/goneV=109/109`, `budgetReason=2`, `genVoxel=32`, `pumpVoxel=32.09`.
- the candidate reused some frames but still accumulated entering/leaving footprint debt and forced synchronous catch-up.
- signature/interval skip variants are rejected; do not retry them.

Next:

- true mid-clipmap incremental scroll/delta update is the safe version: apply entering/leaving brick changes as they occur instead of suppressing rebuilds.
- if the refactor scope is too large, first add a narrow `UpdateVoxelInterest` cost split for candidate generation, terrain sampling, sort/emit, and backlog carry to guide the delta implementation.

### Campaign Continuation - Mid-Clipmap Interest Detail Timing - 2026-06-05

New default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL=0`
- `-MidClipmapInterestDetail`
- `PERF_SPARSE_MID_CLIPMAP_INTEREST_DETAIL`

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_timing_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_vsync0_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Key rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `21.32` | `22.63` | `4.10` | `17.72` | `0.80` | `0.00` | frame log `23.37/2.05` | `7.40` | `0` | `100` | `0/0` |
| walk realtime | `600` | `60.84` | `38.38` | `16.54` | `14.86` | `6.95` | `5.10` | `11.13/2.61` | `19.44` | `415` | `95` | `0/0` |
| high-alt | `360` | `51.32` | `32.84` | `8.48` | `6.74` | `17.61` | `5.80` | `4.70/1.71` | `10.14` | `2184` | `99` | `0/0` |

Interest split:

- fixed frame `384`: interest reused, line/anchor/sort/backlog all `0.00`.
- walk realtime frame `600`: interest reused, line/anchor/sort/backlog all `0.00`; representative walk is no longer blocked by interest construction in this row.
- high-alt frame `360`: line `0.20`, anchor `1.37`, sort/emit `1.65`, backlog `0.11`, diagnostics `0.51`; interest construction is only part of the `17.61 ms` clip cost.

Decision:

- do not repeat interest-signature or interval skip variants.
- next branch should target pump/generation/apply separation, surface extraction, request prep, or a true entering/leaving delta refactor.

### Campaign Continuation - Existing Branch Validation - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/existing_branch_validation_20260605.md`

Existing default-off branches tested:

- `-ParallelMidVoxelPump`
- `-PostOpenSurfaceMaxMs 4 -SurfaceReadyPublishQueue -SurfaceReadyPublishPressure`
- `-RequestFastResidentTouch`

Results:

| Branch | Fixed | Walk realtime | High-alt | Decision |
|---|---|---|---|---|
| parallel mid pump | raw `39.07`, CPU `27.63` | raw `78.06`, CPU `31.68` | raw `51.50`, CPU `31.37` | rejected as stack member; partial pump CPU win, raw/fixed regress |
| post-open surface cap + ready queue | raw `23.92`, CPU `16.82`, pending `3233` | raw `89.93`, CPU `50.70`, pending `5153`, oldest age `525` | raw `54.55`, CPU `34.14` | rejected; shifts work into surface-ready debt |
| request fast resident touch | raw `22.51`, CPU `21.14` | raw `90.48`, CPU `52.88`, clip/pump `28.70/26.25` | raw `49.66`, CPU `29.82` | rejected; request win triggers/uncovers clip catch-up |

Decision:

- none of these existing flags becomes part of the playable candidate stack;
- all defaults remain unchanged;
- next work must be a lane-aware streaming state-machine slice, not another isolated flag.

### Campaign Continuation - Async Exact Prefetch Lane Stack - 2026-06-05

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

New default-off work:

- `VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME=0`
- `PERF_SPARSE_EXACT_ASYNC` logs `maxEnqueue` and enqueue/apply lane counts.
- Harness flags: `-AsyncExactPrefetchLane`, `-AsyncExactMaxEnqueuePerFrame`.

Ownership rule preserved:

- async exact prefetch-lane only allows `Cache`/`Prefetch` streaming lanes to use async generation.
- `Visible` and `PublicCritical` streaming lanes still stay synchronous unless the old explicit visible-async flag is enabled; it was not enabled in validation.
- sampled rows had `asyncEnqueuedLaneVisible=0`, `asyncEnqueuedLanePublic=0`, `missScreenPct=0`, and `unsafeNearMissScreenPct=0`.

Best current default-off stack:

- VSync off
- `VENPOD_RENDER_QUALITY=playable`
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

12-frame walk-window average, frames `596-607`:

| Stack | Avg raw | Avg CPU | Avg request | Avg gen | Avg clip | Max raw | Max CPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| baseline stack | `79.78` | `48.63` | `10.81` | `8.15` | `29.65` | `90.19` | `61.03` | baseline |
| async prefetch lane | `74.92` | `42.59` | `13.16` | `7.01` | `22.40` | `98.91` | `68.74` | partial; raw spike remains |
| async prefetch + parallel mid | `71.17` | `39.11` | `13.73` | `8.91` | `16.45` | `81.72` | `49.47` | useful companion |
| async prefetch + parallel mid + request fast touch | `64.32` | `32.90` | `12.38` | `7.17` | `13.33` | `72.81` | `40.91` | best current stack, not playable |
| plus parallel exact | `65.11` | `30.40` | `11.70` | `7.00` | `11.68` | `78.00` | `42.52` | mixed |

Rejected/mixed:

- async enqueue caps `8` and `16`: safe but over-throttled, losing the CPU/clip win.
- parallel surface extraction: rejected; fixed raw regressed to `36.83` and walk max raw worsened.
- parallel exact generation: mixed; frame-600 CPU/raw improved, but fixed and walk average raw were not best.

Current state:

- no validated 60 FPS candidate.
- strongest stack is default-off and materially better, but representative noncapture rows remain far above `16.67 ms`.
- all defaults remain unchanged, including background split, clean throttle, backlog-aware pump, async prefetch lane, bounded repair, and promotion policy.

Next measured blocker:

- build the real lane-aware streaming state-machine slice:
  - visible/public ownership lane synchronous and guarded;
  - cache/prefetch async generation with apply/upload accounting;
  - keyed/incremental request prep;
  - faster synchronous mid visible generation or real owner proof before deferral.

### Parallel Exact Std Execution Candidate - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_rejected_20260605.md`

Candidate:

- temporary `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION=1`
- chunked `std::execution::par` inside existing exact-generation parallel paths
- same-frame exact generation only; no ownership deferral

Rows:

| Scenario | Baseline raw/CPU/gen/clip | Candidate raw/CPU/gen/clip | Decision |
|---|---:|---:|---|
| fixed | `24.54/18.76/13.99/0.77` | `25.13/20.86/15.13/0.75` | rejected |
| walk realtime | `67.95/55.11/16.07/21.10` | `72.46/46.34/12.53/20.45` | rejected; CPU improved but raw worsened |
| high-alt | `51.02/28.72/5.14/15.31` | `49.42/33.21/5.57/19.07` | rejected; CPU/clip regressed |

Decision:

- removed the std-execution code/env/harness switch
- no `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION` symbol remains
- post-removal build and ctest passed
- all defaults remain unchanged
- next implementation should be a persistent same-frame generation pipeline or lane-aware public-critical generation/apply refactor, not another per-call thread variant

### Persistent Exact Workers Candidate - Partial Keep Default-Off - 2026-06-05

New default-off flag:

- `VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS=0`
- harness: `-ParallelExactPersistentWorkers`

Implementation:

- persistent worker pool in `SparseVoxelWorld`
- used only for exact parallel-generation batches when explicitly enabled
- waits for the batch before apply/presentation, so visible/public ownership is not deferred

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_2w_candidate_20260605`

Result:

| Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `21.81` | `17.17` | `3.85` | `12.55` | `0.76` | `0.00` | `6.97` | `0/0` | `0/0` | partial keep |
| walk realtime | `67.23` | `32.40` | `13.18` | `12.82` | `6.38` | `4.25` | `20.19` | `409/209` | `0/0` | CPU win, raw not solved |
| high-alt | `51.21` | `28.80` | `8.71` | `4.74` | `15.34` | `4.20` | `13.16` | `1764/0` | `0/0` | neutral |

Decision:

- 2-worker persistent exact pool is retained default-off as a partial candidate.
- 4-worker setting is not accepted because high-alt regressed.
- this is still not a 60 FPS candidate.
- next measured blockers are fixed surface extraction, walk CPU/GPU/frame gap, and high-alt mid clip/pump.

### Post-Open Surface Cap 1 ms - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_persistent_exact_2w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_rejected_20260605.md`

Finding:

- fixed `surfExtract=21.33 ms` came from `PERF_SPARSE_PRE_PUBLISH_SURFACE` using post-open max `40 ms`, not from the later general `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS=1` path.

Rejected run:

| Scenario | Raw | CPU | Request | Gen | Clip | Surface extract | GPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `22.54` | `24.75` | `11.19` | `12.80` | `0.75` | `5.43` | `6.93` | rejected; CPU regressed |
| walk realtime | `67.93` | `47.09` | `18.45` | `14.06` | `14.56` | `12.17` | `14.14` | rejected |
| high-alt | `69.45` | `39.02` | `13.81` | `6.15` | `19.05` | `4.60` | `5.80` | rejected |

Decision:

- cap-only post-open pre-publish surface extraction is not safe/useful as a stack member.
- fixed frame `384` still showed `queuedPublishes=8098`.
- next surface fix would need a real surface-ready publish/backlog architecture.

### Background Pass Scale 0.25 - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/background_scale025_persistent_exact_2w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/background_scale025_rejected_20260605.md`

Rows:

| Scenario | Scale 0.375 raw/CPU/GPU | Scale 0.25 raw/CPU/GPU | Decision |
|---|---:|---:|---|
| fixed | `21.81/17.17/6.97` | `31.78/20.52/5.96` | rejected |
| walk realtime | `67.23/32.40/20.19` | `75.21/45.98/11.97` | rejected |
| high-alt | `51.21/28.80/13.16` | `52.99/33.43/7.55` | rejected |

Decision:

- background scale `0.25` is not a current stack candidate.
- CPU/raw regressions outweigh GPU savings.
- next branch returns to CPU generation/request/backlog.

### Persistent Surface Workers Rejected - 2026-06-05

Attempted and removed a default-off persistent surface extraction worker pool behind the existing parallel surface extraction path.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch16_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch8_candidate_20260605`

Best comparison against the persistent exact 2-worker stack:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| baseline | `21.81/17.17` | `67.23/32.40` | `51.21/28.80` | current partial |
| surface persistent 4w | `18.90/18.22` | `67.16/42.00` | `50.49/29.60` | rejected |
| surface persistent 4w batch16 | `21.84/19.62` | `60.95/44.43` | `47.96/30.80` | rejected |
| surface persistent 4w batch8 | `22.92/25.86` | `64.86/52.48` | `51.09/27.99` | rejected |

Decision:

- raw improvements were mixed and came with representative CPU/clip regressions.
- no persistent surface worker code or env flag remains.
- do not retry surface extraction as a thread-backend tweak; future surface work needs publish/backlog-aware lane architecture.

### Diagnostic-Off and Bounded64 Checks - 2026-06-05

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_noheavydiag_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/bounded64_noheavydiag_candidate_20260605`

Findings:

- turning off heavy fallback diagnostics does not make the stack playable: walk still around `63.72 ms` raw and high-alt around `52.95 ms` raw.
- bounded64 comparison improved some raw rows but regressed CPU/clip; it remains default-off and is not the CPU fix.
- current remaining blocker is architectural lane ownership across request, generation, apply/upload, surface extraction, and publish, not another isolated queue/thread/cap knob.

### Persistent Mid Workers and Trim Budget Probe - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS=0`

Implementation:

- persistent worker pool for the existing parallel mid-voxel pump path
- only active when parallel mid voxel pump is enabled
- same-frame generation/apply semantics are preserved
- no default behavior change

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim8192_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim4096_walk_probe_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Rows:

| Candidate | Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| persistent mid workers | fixed f384 | `20.88` | `18.59` | `3.73` | `14.07` | `0.79` | `0.00` | `6.07` | `0/0` | partial keep |
| persistent mid workers | walk f600 | `79.36` | `39.97` | `16.56` | `15.27` | `8.12` | `5.95` | n/a | `0/0` | CPU/clip better, raw noisy |
| persistent mid workers | high-alt f360 | `48.43` | `28.12` | `8.37` | `4.87` | `14.87` | `5.04` | `8.59` | `0/0` | partial keep |
| persistent mid workers + interest interval 2 | fixed f384 | `21.09` | `18.86` | `3.80` | `14.12` | `0.94` | `0.00` | `6.07` | `0/0` | mixed |
| persistent mid workers + interest interval 2 | walk f600 | `63.63` | `38.51` | `13.82` | `12.34` | `12.33` | `10.01` | `15.67` | `0/0` | mixed |
| persistent mid workers + interest interval 2 | high-alt f360 | `49.54` | `19.61` | `7.96` | `4.81` | `6.83` | `4.55` | `9.99` | `0/0` | CPU/clip win, raw not solved |
| trim budget 8192 | walk f600 | `67.32` | `39.04` | `10.79` | `13.19` | `15.03` | `12.66` | `15.58` | `0/0` | rejected |
| trim budget 4096 | walk f600 | `62.31` | `37.77` | `14.02` | `16.84` | `6.89` | `5.02` | `9.07` | `0/0` | probe only |

Decision:

- persistent mid workers remain default-off and are safe to keep as a partial candidate.
- interest interval `2` is not a default or final stack setting.
- lower pressure-trim scan budgets are not a validated stack fix; the reduced trim work shifted into terrain-critical hierarchy/generation/clip.
- no candidate is close to 60 FPS.
- next useful branch should be a real lane-aware generation/surface/publish architecture slice, not another isolated cap/thread/budget knob.

### Post-Compaction Local Branch Rejections - 2026-06-05

Build/test after branch cleanup:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Rejected branches:

| Branch | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Why rejected |
|---|---:|---:|---:|---|
| exact coordinate batch generation | `22.92/18.51` | `89.44/34.29` | `50.61/27.30` | hidden-exact batch was active, but fixed generation did not improve, surface extraction spiked, and walk raw regressed |
| pressure trim miss-feedback queued-only | `25.12/20.38` | `64.14/41.85` | `49.43/26.72` | resident trim scan was skipped when safe, but the local trim win did not become a frame-time win and fixed regressed |

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_coord_batch_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_miss_feedback_queued_only_candidate_20260605`

Cleanup:

- temporary exact coordinate batch symbols removed.
- temporary pressure-trim queued-only symbols removed.
- campaign `summary.csv` and `table.md` include the rejected rows.

Current decision:

- no new accepted code stack member from this continuation.
- current strongest stack remains default-off and still misses playable targets.
- next work should stop treating request/generation/clip/surface/publish as separable knobs and implement a small ownership-lane streaming state-machine slice with per-lane budgets and frame-boundary apply rules.

### Upload Lane Budget Candidate Rejected - 2026-06-05

Attempted and removed a default-off upload-lane budget slice:

- `VENPOD_SPARSE_UPLOAD_LANE_BUDGETS`
- `VENPOD_SPARSE_UPLOAD_CACHE_LANE_BUDGET`
- `VENPOD_SPARSE_UPLOAD_PREFETCH_LANE_BUDGET`
- `PopBestUploadForLane`
- harness switches and parser columns

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
| post-guard control | `22.99/18.64` | `66.81/47.72` | `57.33/29.83` | control |
| 8/16 high-alt opt-out | `24.66/19.68` | `63.53/49.63` | `50.60/34.89` | rejected; target raw moved, CPU/window did not |

Decision:

- Upload lane budgeting improved some target raw/CPU rows but did not survive same-code window/control validation.
- Removed the branch after `.\build.ps1 -Config Release` and `ctest --test-dir build --output-on-failure -C Release` passed.
- No `SPARSE_UPLOAD_LANE_BUDGET` or `PopBestUploadForLane` symbols remain.
- This reinforces the current architecture decision: queue selection alone is too local. The fix must carry ownership lanes through request, generation, apply/upload, surface extraction, and publish readiness together.

### Exact Local Column Block Candidate Rejected - 2026-06-05

Attempted and removed a default-off exact-generation micro-optimization:

- `VENPOD_SPARSE_EXACT_LOCAL_COLUMN_BLOCK_GENERATION`
- harness switch `-ExactLocalColumnBlockGeneration`
- local fixed height/relief block path for exact brick generation

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_candidate_20260605`

Rows:

| Scenario | Control raw/CPU/gen | Candidate raw/CPU/gen | Decision |
|---|---:|---:|---|
| fixed f384 | `21.41/19.13/14.11` | `19.89/22.18/17.41` | rejected |
| walk f600 | `55.66/36.42/11.17` | `58.70/52.03/18.20` | rejected |
| high-alt f360 | `44.66/22.17/4.55` | `46.16/25.39/6.64` | rejected |

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- no `EXACT_LOCAL_COLUMN`, `GenerateBrickWithLocalColumnBlock`, or related local-block symbols remain

Decision:

- branch rejected and removed.
- local column-block generation worsened CPU/generation in representative rows.
- retained only behavior-neutral exact-generation helper centralization.
- remaining exact/visible work is a streaming pipeline/state-machine problem, not this local terrain-sampling cache issue.

### Formal Streaming State-Machine Direction - 2026-06-05

Current blocker:

- request, generation, upload, surface extraction, and publish have partial lane/accounting data, but there is no persistent ownership ticket controlling the full brick lifecycle.
- local queue fixes keep moving pressure between stages because the stages are not governed by one ownership/readiness state machine.
- dominant fixed/walk work is mostly visible/public-critical, so it cannot be deferred unless a valid owner/readiness proof exists.

Next actual implementation slice:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0` default-off.
- create a persistent per-brick ticket with ownership class, required/completed stage masks, fallback proof, edit revision, request/sample/deadline frames, and backlog age.
- ownership classes:
  - public critical
  - sampled visible
  - hidden repair
  - cache
  - prefetch
  - fallback valid
  - unknown critical
- scheduling rules:
  - public/sampled/unknown tickets are protected or readiness-gated;
  - hidden repair is budgeted after public/sampled;
  - cache/prefetch/fallback-valid can be async/budgeted;
  - unknown fallback never becomes async-eligible.
- first code step: ticket shadow mode and logs, no behavior change.
- second code step: ticket-based queue pop order for existing generation/upload/surface/publish functions.
- third code step: async generation/apply only for cache/prefetch/fallback-valid tickets.

This is the smallest credible fix path remaining. Do not resume with another scalar cap or thread-backend tweak unless it is part of this ticket scheduler.

### Streaming Work Ticket Shadow V1 - 2026-06-05

New default-off retained code:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0`
- harness switch `-StreamingTicketScheduler`
- `PERF_SPARSE_STREAMING_TICKETS`

Implementation:

- shadow per-brick `StreamingWorkTicket` in `SparseVoxelWorld`
- ownership classes: public critical, sampled visible, hidden repair, cache, prefetch, fallback valid, unknown critical
- stage mask: CPU generated, GPU uploaded, surface ready, page published
- updates on request, generation apply, upload complete, surface ready/known empty, page publish, lane/residency touch, and eviction
- no queue scheduling or rendering behavior changes yet

Build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_shadow_v1_20260605`

Rows:

| Scenario | Frame | Raw | CPU | Ticket state |
|---|---:|---:|---:|---|
| fixed | `382` | `26.51` | `5.76` | active `796`, all prefetch pending CPU |
| walk realtime | `600` | `69.96` | `51.41` | active `4889`; sampled/prefetch `1213/3676`; pending CPU/upload/surface/publish `3228/1661/62/1661` |
| high-alt | `360` | `44.12` | `23.85` | active `6110`; public/sampled/prefetch `8/15/6087`; pending CPU `6110` |

Decision:

- default-off shadow mode is safe to keep.
- no 60 FPS candidate yet.
- next branch should convert this from observation into protected scheduling: generation/upload/surface/publish queue pop order and budgets must consume the same ticket ownership class.

### Protected Ticket Scheduling Continuation - 2026-06-05

New retained default-off code:

- `VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING=0`
- harness switch `-StreamingTicketProtectedScheduling`
- `PERF_SPARSE_STREAMING_TICKETS` now logs `protected` and `protectedSorts`

Implementation:

- requires `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=1`
- generation, upload, and surface queues can be sorted by ticket ownership/stage
- public critical, unknown critical, sampled visible, hidden repair, and fallback-valid tickets outrank cache/prefetch
- refined helper skips sorting cache/prefetch-only queues, preserving existing behavior there
- no work is dropped or made default-on

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- `git diff --check` on touched tracked files: no code whitespace errors, only existing LF-to-CRLF warnings

Artifacts:

- control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_control_20260605`
- unrefined candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_candidate_20260605`
- refined candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_refined_candidate_20260605`
- hidden exact full budget probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_budget32_candidate_20260605`
- hidden exact generation-only probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_gen32_only_candidate_20260605`
- trim probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/gen32_incremental_trim_candidate_20260605`

Window results:

| Candidate | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| ticket shadow control | `24.53/54.92` | `68.40/102.26` | `55.68/67.97` | control |
| protected ticket scheduling | `27.43/57.65` | `63.97/79.15` | `46.55/59.26` | mixed; fixed churn |
| refined protected scheduling | `24.89/58.54` | `66.25/80.41` | `45.75/58.99` | partial keep default-off |
| hidden exact post-open budgets `32/32/32/64` | `24.41/39.89` | `70.56/121.00` | `51.80/80.20` | rejected; upload spike |
| hidden exact post-open generation `32` only | `22.73/37.38` | `64.35/84.00` | `49.87/64.85` | mixed; fixed/walk partial, high-alt regression |
| gen32 + incremental pressure trim | `25.26/44.30` | `72.25/89.51` | `57.48/84.05` | rejected |

Target-frame notes:

- refined protected walk frame `600`: raw/CPU `68.44/43.90`, request/gen/clip `17.86/16.40/9.61`, GPU `13.03`, miss/unsafe `0/0`
- refined protected high-alt frame `360`: raw/CPU `47.30/24.90`, request/gen/clip `9.11/6.22/9.57`, GPU `9.76`, miss/unsafe `0/0`
- hidden exact generation-only fixed frame `360`: raw/CPU/gen `23.13/11.68/6.73`; fixed improved, but high-alt regressed in the window
- incremental pressure trim did not help this stack; walk frame `600` still had `pressureTrimMs=5.74`

Current decision:

- keep streaming ticket shadow and refined protected scheduling default-off as the latest state-machine slice
- do not keep full hidden-exact budget throttling as a candidate
- hidden-exact generation-only pacing is a mixed probe, not the global candidate stack
- reject incremental pressure trim for this stack
- no 60 FPS/playable candidate exists yet

Current remaining blocker:

- fixed can be pushed into the low-20 ms raw band but still has hidden-exact/surface bursts
- walk remains around mid-60 ms raw with request, generation, clip, surface, and GPU all material
- high-alt improves with protected scheduling but remains around mid-40 ms raw
- the next code slice should make ticket scheduling control apply/upload/surface/publish pacing together instead of using scalar caps that shift debt into the next stage

### Streaming Ticket Stage Pacing Rejected - 2026-06-05

Attempted default-off page-publish stage pacing and removed it after validation:

- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING_MIN_CAMERA_Y`
- `PopReadyByLanePriority` / `PopReadyProtectedLane`
- `PERF_SPARSE_STREAMING_TICKET_STAGE_PACING`
- harness stage-pacing switches

Artifacts:

- broad candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_candidate_20260605`
- same-code control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_control_20260605`
- high-alt-only failed probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_highalt_only_candidate_20260605`

Window results:

| Row | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| same-code control | `23.88/53.43` | `83.12/108.59` | `65.15/93.08` | control |
| broad stage pacing | `26.77/62.45` | `86.76/206.33` | `54.86/83.41` | rejected |

High-alt-only gated pacing also failed. Its parser did not produce summary files because it missed the exact `PERF frame=400` gate, but `PERF_FRAME_END` rows show late raw outliers: frame `403=87.09`, `404=90.58`, `405=75.97`, `406=87.04`, `407=107.13`.

Cleanup/build status after removal:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- `git diff --check`: only existing LF-to-CRLF warnings on touched cleanup files

Decision:

- do not reintroduce stage-pacing publish selection.
- publish selection alone is not the architecture fix.
- current same-code control points to walk clip/pump/request/gen as the next measured blocker: f600 raw/CPU `83.47/65.82`, request/gen/clip/pump `18.98/16.92/29.90/27.55`.

### Request Accounting Split and Pressure-Trim Guard Rejection - 2026-06-05

Retained behavior-neutral logging:

- `PERF_SPARSE_REQUEST_DETAIL` now reports `hierarchyOtherMs`, `preHierarchyMs`, true `otherMs`, `legacyOtherMs`, and pressure reasons `free/gen/miss`.

Artifacts:

- request accounting: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_accounting_baseline_20260605`
- rejected candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_queued_scan_pool_guard_candidate_20260605`

Request-accounting baseline:

| Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface Extract/Stage | GPU | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f380 | `36.34/11.83` | `3.94` | `6.85` | `1.04/0.00` | `6.04/2.21` | `7.83` | raw had wrapper/gap noise |
| walk f600 | `91.48/62.52` | `28.51` | `25.28` | `8.70/6.03` | `14.41/3.82` | `23.75` | pressure trim `9.31`, terrain critical `6.67`, stats `3.75`, hidden exact `3.63`, true other `3.88` |
| high-alt f400 | `511.69/41.06` | `13.67` | `10.94` | `16.43/6.27` | `6.84/2.38` | `15.62` | frame-gap raw outlier |

Rejected and removed:

- `VENPOD_SPARSE_PRESSURE_TRIM_QUEUED_SCAN_REQUIRES_POOL_PRESSURE`
- harness `-PressureTrimQueuedScanPoolGuard`

Rejected result:

| Scenario | Raw/CPU | Request | Gen | Clip/Pump | GPU | Verdict |
|---|---:|---:|---:|---:|---:|---|
| fixed f380 | `33.58/9.40` | `2.82` | `5.69` | `0.88/0.00` | `9.21` | no representative win |
| walk f600 | `111.69/60.04` | `22.94` | `29.10` | `7.97/5.93` | `24.94` | rejected; generation/raw regressed |
| high-alt f400 | `69.53/34.53` | `11.08` | `8.63` | `14.80/5.83` | `15.51` | rejected |

Reason:

- the bad frames were miss-feedback pressure, not generation-only pressure, so the queued-scan pool guard did not apply (`pressureTrimPressure=0/0/1`).
- do not continue queued-trim gating as the main branch.
- next measured branch should target hidden-exact/miss-feedback request admission, generation/surface coupling, or surface extraction backlog.

### Async Low-Priority Apply / Persistent Mid Validation - 2026-06-05

Audit:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cap32_persistent_mid_walk_20260605/contact_sheet.png`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cap32_persistent_mid_highalt_20260605/contact_sheet.png`

New retained default-off knob:

- `VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME=0`

Harness/logging:

- `perf_noncapture_smoke.ps1` supports `-AsyncExactLowPriorityMaxApplyPerFrame`.
- `PERF_SPARSE_EXACT_ASYNC` and campaign parsing now include `lowPriorityMaxApply` and `deferredLowPriority`.
- default `0` keeps current unlimited low-priority apply behavior.

Validation:

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip/Pump | GPU | Avg/Max Raw | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| control | fixed | `22.30` | `18.53` | `4.23` | `13.58` | `0.71/0.00` | `6.61` | `22.51/53.25` | control |
| control | walk | `63.23` | `41.15` | `17.26` | `16.77` | `7.10/5.57` | `13.53` | `85.93/624.57` | control |
| control | high-alt | `97.26` | `62.90` | `16.33` | `11.87` | `34.67/20.92` | `20.85` | `93.93/117.04` | control |
| cap8 | fixed | `23.72` | `22.42` | `4.59` | `17.06` | `0.75/0.00` | `7.54` | `23.76/53.87` | rejected; `504` deferred completions |
| cap8 | walk | `49.35` | `43.71` | `17.16` | `19.07` | `7.46/5.56` | `20.87` | `69.72/96.42` | rejected |
| cap8 | high-alt | `65.54` | `38.04` | `8.96` | `6.72` | `22.35/12.80` | `11.10` | `58.57/73.27` | rejected |
| cap32 + persistent mid | fixed | `23.49` | `19.26` | `4.61` | `13.93` | `0.72/0.00` | `6.12` | `23.00/51.02` | strongest partial |
| cap32 + persistent mid | walk | `52.74` | `43.95` | `12.24` | `12.62` | `19.07/8.25` | `9.57` | `60.90/71.20` | strongest partial |
| cap32 + persistent mid | high-alt | `58.03` | `34.17` | `10.24` | `7.65` | `16.28/6.11` | `11.43` | `56.00/63.54` | strongest partial |
| cap32 + persistent mid + exact2 | walk | `63.71` | `51.25` | `18.39` | `16.47` | `16.37/4.14` | `15.05` | `72.74/106.83` | rejected |

Visual:

- walk candidate frames remain coherent but coarse/blocky.
- high-alt still has the known bright white shoreline/terrain artifact.
- this is not a validated playable stack even though sampled miss/unsafe counters stayed clean.

Decision:

- strongest partial stack is `background split + playable render quality + clean throttle + async exact prefetch lane + ticket scheduler/protected scheduling + explicit source lanes + fast resident touch + critical reuse + parallel mid pump + low-priority apply cap 32 + persistent mid workers`.
- keep it default-off and treat it as a diagnostic/perf candidate only.
- cap8 and persistent exact workers are rejected.
- all public defaults remain unchanged.
- next work must address the high-alt visual ownership/shoreline failure and remaining distributed walk CPU/GPU cost, not another isolated scalar cap.

### Streaming Playability Hidden Exact / Cache Defer Follow-Up - 2026-06-05

Audit artifacts:

- hidden repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_repair_lane_candidate_20260605`
- water repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_repair_lane_candidate_20260605`
- cache-only perf: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/cache_only_defer_candidate_20260605`
- cache-only visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_20260605/contact_sheet.png`

Build/test:

- Release build passed.
- `ctest` passed `1/1`.

New default-off knobs:

- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE=0`

Hidden exact repair-lane result:

- non-water post-open hidden repair split is ineffective in current rows because post-open accepted feedback is mostly water feedback.
- water-inclusive repair lane reduces walk CPU but shifts cost into upload/post-wait and grows hidden exact debt.
- water-inclusive walk f600: raw/CPU `85.91/31.75`, request/gen/clip/pump `15.59/7.38/8.76/6.67`, GPU `14.32`, but `hiddenExactMissing=3068/0`, upload lane prefetch `886`, post-wait `31.83`.
- keep both knobs default-off and diagnostic only.

Cache-only visible-critical defer:

- existing `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` remains default-off.
- high-alt f400 improves to raw/CPU `54.59/22.30`, request/gen/clip/pump `8.34/7.02/6.94/0.01`, with `cacheOnlyDefer=1867`, visible-critical coverage `99`, cache coverage `79`, parent-held failure `0`, miss/unsafe `0/0`.
- fixed is unchanged because missing mid debt is zero.
- walk remains projected-visible, so cache-only defer is inactive and not a walk fix: walk f600 raw/CPU `69.45/52.64`, request/gen/clip/pump `18.85/16.87/16.89/4.54`, surface extract/stage `12.51/3.00`, backlog voxel `432`, max age `280`.

Visual:

- high-alt cache-only contact sheet still shows the large pale lower-screen terrain/shore band.
- numeric ownership gates passing is not enough; this is not a validated visual or playable candidate.

Decision:

- no validated 60 fps candidate exists.
- high-alt over-broad cache debt can be reduced safely, but high-alt visual ownership remains unresolved.
- walk/realtime debt is projected-visible and cannot be safely deferred without ownership proof.
- next work should target high-alt visual ownership/shoreline contract or the request/generation/surface coupling for projected-visible walk debt under the ticket/state-machine model.

### High-Alt Background Split Exact-Band Ownership Finding - 2026-06-05

Current resume point for active goal `streaming_playability_real_fix_campaign_20260604`.

New artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_owner55_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_material54_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_badpixel_20260605/bad_pixels.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode58_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode57_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_surfacefill_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_candidate_20260605`

Diagnosis:

- the high-alt pale lower-screen band is not primarily a mid-clipmap scheduling artifact.
- mode 57 sampling over the lower band was dominated by `far_svo_unavailable_or_rejected`.
- lower-band rays are downward and cross the water plane at plausible terrain/water distances, so sky-like output is visually wrong.
- the root is the background split exact-band admission path.

Relevant code path:

- `Renderer.cpp` clears sparse-near flag bit `8` for background split unless `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1`.
- `PS_Raymarch.hlsl` treats bit `8` as `sparseSurfaceRaymarchFill`.
- `BackgroundHitAllowedByExactNear` uses that flag to decide whether mid/Far-SVO/far-water may own pixels inside the exact/surface ownership radius.
- with the fill flag off, unstenciled exact-band pixels in the lower-res background pass can reject valid-looking background/water owners and fall through to sky-like output.

Surface-fill probe:

- enabling `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` visually removed the high-alt lower pale band.
- frame `400` sky-like percentage dropped from about `41.18%` to about `20.51%`.
- frame `400` layer mix became more plausible: far water about `8.2461%`, water context about `12.2994%`, sky about `2.4502%`.

Why this is not a fix yet:

- surface-fill also introduced/preserved exact-contract unsafe debt.
- high-alt frame `400`: `shaderUnsafeNonReady=198/0`, `shaderUnsafeContractNonReady=198`.
- walk frame `600`: `shaderUnsafeNonReady=83/0`, `shaderUnsafeContractNonReady=83`.
- walk remained slow with surface-fill: raw/CPU about `62.13/49.03`, request/gen/clip/pump `18.77/13.77/16.46/6.01`, GPU about `16.94`.

Decision:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` is a strong default-off diagnostic proving the admission bug, not a validated playable-mode fix.
- all defaults remain unchanged.
- the next high-alt branch should be a narrow validated background-fill ownership path or exact/water readiness fix, not another mid-pump scheduling tweak.
- if surface-fill pixels can be proved deterministic water/Far-SVO owners, add a narrow contract-safe admission path; if not, exact foreground/water readiness must improve before allowing split-background fill there.

Do not repeat:

- do not promote surface fill.
- do not use miss/unsafe zero alone as proof of visual correctness.
- do not treat the pale band as solved by cache-only defer.
- do not return to scalar caps, stage-pacing, pressure-trim gating, or hidden-exact water-lane promotion for this issue.

### Surface-Fill Exact Repair / Cached Water Proof - 2026-06-05

New default-off knobs:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR=0`
- optional cache bound: `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF_CACHE_MAX`, default `8192`

Code changes:

- `src/main_launcher.cpp` now has a deterministic-water proof for surface-fill exact-band shader-unsafe samples.
- proof requires sea-level overlap, no edit overlay, generated height below sea level over the full `16x16` x/z brick footprint, and generated sea-level material `Water`.
- proof results are cached by brick and invalidated on `SparseEditStore::RevisionSerial()`.
- remaining non-proven samples remain exact-contract non-ready and can be requested through surface-fill exact repair.
- `perf_noncapture_smoke.ps1` can enable surface fill, water proof, and exact repair.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_highalt_f480_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_highalt_f480_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_walk_f600_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_walk_budgetedhidden_f600_20260605`

Key rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Contract result | Decision |
|---|---:|---:|---:|---:|---:|---:|---|---|
| uncached high-alt f480 | `97.66/72.94` | `49.46` | `14.78` | `8.69/0.00` | `30.42` | `116.30/423.04` | raw `66`, water `61`, repair `5` | rejected; proof CPU spike |
| cached high-alt f480 | `57.03/32.60` | `16.95` | `8.98` | `6.65/0.00` | `18.70` | `63.14/88.16` | raw `31`, water `27`, repair `4` | useful partial, not playable |
| cached walk f600 | `77.12/185.37` | `142.32` | `21.02` | `22.00/9.64` | `26.69` | `86.70/219.96` | raw `92`, water `39`, repair `48/53` | rejected movement stack |
| cached walk + hidden budget f600 | `122.00/46.32` | `22.82` | `16.77` | `6.70/4.18` | `39.97` | `158.98/902.15` | raw `47`, water `42`, repair `5` | rejected; raw/window regression |

Decision:

- cached water proof fixed a real CPU hotspot in this default-off validation path: high-alt f480 owner feedback `32.09 ms -> 4.12 ms` and request `49.46 ms -> 16.95 ms`.
- surface-fill exact repair is still not a validated playable candidate.
- existing hidden-exact tracked scan budgeting is rejected for this stack because it reduced target CPU but worsened raw/window badly.
- keep surface fill, water proof, exact repair, background split, clean throttle, backlog pump, hidden-exact scan budgeting, and bounded repair default-off.

### High-Alt-Only Surface Fill / Incremental Pressure Trim - 2026-06-05

New default-off knob:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY=0`

What changed:

- water proof and exact repair can now be requested without forcing global background surface-fill
- high-alt-only mode activates surface-fill only when high-alt LOD admission is allowed
- `perf_noncapture_smoke.ps1` supports `-BackgroundPassSurfaceFillHighAltOnly`

Accepted partial stack:

- `VENPOD_RENDER_QUALITY=playable`
- background split enabled at scale `0.375`
- clean prefetch throttle
- high-alt-only surface fill + water proof + exact repair
- critical reuse, async exact prefetch lane, ticket/protected scheduling, explicit source lanes, fast resident touch
- parallel mid pump with persistent workers
- visible-critical prepump/cache-only defer
- incremental pressure trim + free-page guard

Rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `25.18/19.28` | `5.04` | `13.38` | `0.84/0.00` | `8.21` | `27.40/60.28` | partial |
| walk f600 | `63.49/33.26` | `9.16` | `17.66` | `6.42/4.16` | `17.44` | `69.16/81.81` | partial |
| high-alt f480 | `55.10/24.35` | `7.99` | `9.38` | `6.98/0.00` | `17.57` | `57.96/75.45` | partial, visual plausible |

Rejected:

- global surface-fill exact repair remains rejected for walk because it hit `185.37 ms` CPU / `142.32 ms` request at f600.
- surface-parallel extraction regressed walk CPU to `60.54 ms`.
- terrain-critical parallel generation did not activate useful parallel work and regressed raw/window.
- background pass scale `0.25` lowered GPU but regressed CPU/clip/raw.

Visual/artifacts:

- root contact sheet: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- high-alt visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_highalt_surfacefill_highalt_only_pressuretrim_f480_20260605/contact_sheet.png`
- walk visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_walk_surfacefill_highalt_only_pressuretrim_f600_20260605/contact_sheet.png`
- campaign rows: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`, `table.md`, `playable_candidate_table.md`

Decision:

- this is a stronger visually checked default-off stack, not a 60 FPS candidate.
- high-alt-only surface-fill avoids the normal walk hidden-exact repair collision.
- incremental pressure trim is a real CPU win and should remain in the candidate stack.
- all defaults remain unchanged.
- next measured blocker is no longer one single subsystem: fixed/walk/high-alt are dominated by generation/surface/post-wait/residual GPU in different proportions.
### Streaming Playability Real Fix Campaign - 2026-06-05 Continuation

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/playable_candidate_table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/contact_sheet.png`

Latest validated state:

- no 60 FPS candidate exists.
- the strongest currently validated default-off stack remains playable render quality + background split `0.375` + clean prefetch throttle + critical reuse + parallel mid pump.
- representative accepted rows:
  - fixed f384: raw/CPU `23.80/22.31`, request/gen/clip `4.50/17.00/0.80`, GPU ray `6.04`
  - walk f600: raw/CPU `59.51/35.60`, request/gen/clip `15.40/14.67/5.51`, GPU ray `12.95`
  - high-alt f398: raw/CPU `51.66/26.21`, request/gen/clip `4.94/5.18/16.09`, GPU ray `10.25`
- visual:
  - fixed and accepted-stack walk captures passed terrain-critical readiness
  - high-alt capture passed readiness but still shows known bright shoreline/terrain artifact

Important rejection:

- incremental pressure trim `4096` is not a validated candidate in this reduced stack.
- it improved walk noncapture perf (`59.51/35.60 -> 52.32/30.58`) but the walk capture failed terrain-critical readiness:
  - frame `604` postMissing `3`
  - frame `606` postMissing `7`
- without incremental pressure trim, the same walk capture passed with postNonReady `0`.
- `16384` scan budget was tested and rejected because it lost the useful perf win (`walk 55.90/41.84`).

Rejected follow-ups:

- persistent terrain column cache
- async exact prefetch lane
- request fast resident touch
- parallel exact generation
- explicit source lanes, with and without async prefetch
- pressure-trim free-page guard
- streaming lane queue priority
- protected streaming ticket scheduler

Current root:

The remaining blocker is an ownership-aware streaming state-machine gap. Source lanes exist and can reveal huge prefetch debt, but generation/upload/surface scheduling still treats too much broad cache/prefetch work as visible-class work. Async remains blocked for sampled fallback-unknown/invalid debt. Queue sort tweaks, scalar trim budgets, and local async prefetch do not solve this safely.

Next real fix:

- implement a default-off ownership-class scheduler in `SparseVoxelWorld`.
- separate public/sampled-critical ownership class from source lane/cache/prefetch lane.
- carry that class through request planning, generation, upload/apply, surface extraction, and publish.
- keep sampled fallback-unknown/invalid bricks critical.
- allow async/budgeted work only for cache/prefetch or CPU-proved fallback-valid bricks.
- validate with noncapture rows and terrain-critical capture checks before accepting any perf result.

Do not repeat:

- do not call trim `4096` safe.
- do not keep tuning pressure-trim budgets as the main fix.
- do not repeat lane-priority or ticket-protected scheduler variants unless the ownership-class state machine changes first.
- do not move unknown sampled fallback to async.

### Ownership-Class Split Follow-Up - 2026-06-05

New default-off knob:

- `VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS=0`

Code changes:

- `SparseVoxelWorldConfig::prefetchLaneSpeculativeClass`
- `SparseVoxelWorld::TouchResidencyClassWithStreamingLane*` maps `Visible + Prefetch` to `Speculative + Prefetch` only when the flag is enabled
- `perf_noncapture_smoke.ps1 -PrefetchLaneSpeculativeClass`
- `PERF_SPARSE_STREAMING_LANES` logs `prefetchSpeculativeClassActive` and `prefetchSpeculativeTouches`

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/prefetch_speculative_worldclass_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/prefetch_speculative_async_candidate`

Rows:

| Candidate | Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---|
| class split | fixed f380 | `37.58/5.71` | `1.86` | `3.05` | `0.77/0.00` | `6.95` | rejected; raw/post-wait worse |
| class split | walk f600 | `57.04/39.98` | `10.16` | `9.66` | `20.13/9.28` | `10.61` | rejected; CPU/clip worse |
| class split | high-alt f360 | `57.19/31.57` | `8.79` | `7.14` | `15.62/6.22` | `10.88` | rejected |
| class split + async | fixed f384 | `20.56/20.82` | `4.54` | `15.24` | `1.02/0.00` | `6.41` | fixed-only partial |
| class split + async | walk f600 | `62.18/44.70` | `12.00` | `12.33` | `20.35/9.41` | `6.01` | rejected; async result backlog |
| class split + async | high-alt f360 | `51.29/29.28` | `7.54` | `6.62` | `15.11/5.86` | `8.97` | not enough; async backlog |

Decision:

- keep flag default-off and diagnostic only.
- class separation is necessary but not sufficient.
- next implementation must split upload/apply and surface extraction by ownership-critical vs prefetch/cache class. Otherwise generation savings shift into post-wait/surface/clip and walk remains bad.

Current best validated stack remains unchanged:

- playable render quality
- background split `0.375`
- clean prefetch throttle
- critical reuse
- parallel mid pump with persistent workers
- no prefetch speculative class
- no async prefetch as playable candidate

### Surface Lane Stage / Protected Ticket Rejection - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` warnings and trailing `vswhere.exe` warning remain.

Latest artifact rows were added to:

- `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/playable_candidate_table.md`

Rejected/removed:

- lane-filtered surface extraction prototype:
  - fixed f384 `18.77/19.47` raw/CPU, but walk f600 `65.98/35.18` and high-alt f360 `49.08/29.22`
  - removed `VENPOD_SPARSE_SURFACE_LANE_STAGE_BUDGETS` and helper code
  - no rejected symbols remain
- zero-prefetch surface probe:
  - walk f600 `56.48/36.49`
  - window avg raw `60.96 ms`
  - rejected because visible surface work remained and it was still not playable
- protected ticket probe:
  - walk f600 `62.21/47.56`
  - clip/pump `23.25/11.40`
  - rejected versus clean parallel-mid stack

Current decision:

- current best validated stack remains unchanged and not 60 FPS: fixed about `23.80 ms`, walk about `59.51 ms`, high-alt about `51.66 ms` raw.
- blind prefetch/low-lane surface deferral, protected tickets, scalar caps, queue sorting, and thread-backend tweaks have been exhausted as local fixes.
- the hard blocker is architectural: ownership-critical class must be carried coherently through request, generation, upload/apply, surface extraction, publish, and readiness.
- sampled fallback-invalid/unknown work remains critical; only cache/prefetch or CPU-proved fallback-valid work can be async/budgeted.
