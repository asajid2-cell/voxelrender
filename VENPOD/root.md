# VENPOD Root-Cause Decision Log

Canonical current handoff: `debug-handoff.md`. Update that file first for session memory; keep this file as the focused root-cause log.

Generated: 2026-06-02

## Clean-Harness / Parallel Mid Pump Validation - 2026-06-05

Retained behavior/default-neutral cleanup:

- `perf_noncapture_smoke.ps1` no longer forces `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`.
- New explicit harness switch: `-MidClipmapVisibleCriticalPrepump`.
- `src/main_launcher.cpp` now avoids projected missing-brick scan/priority tagging unless prepump can affect the frame, visible priority/lane guard is enabled, or missing-sample feedback is explicitly enabled.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/walk_lane_diagnostics_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/validated_parallel_mid_stack_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605/contact_sheet.png`

Key rows:

| Stack | Scenario | Raw/CPU | Request/gen/clip/pump | Surface extract | GPU | Decision |
|---|---|---:|---:|---:|---:|---|
| clean control | walk f600 | `63.51/55.81` | `12.94/16.70/26.15/14.31` | `11.01` | `12.22` | true clean blocker |
| parallel mid | fixed f384 | `23.80/22.31` | `4.50/17.00/0.80/0.00` | `20.87` | `6.04` | partial |
| parallel mid | walk f600 | `59.51/35.60` | `15.40/14.67/5.51/4.01` | `11.32` | `12.95` | strongest clean walk row |
| parallel mid | high-alt f398 | `51.66/26.21` | `4.94/5.18/16.09/9.74` | `3.78` | `10.25` | still blocked |

Rejected in this clean cycle:

- `MidInterestInterval=2`
- terrain-critical parallel generation on top of parallel mid
- worker-local mid column cache on top of parallel mid as a stack member
- high-alt projected visible-critical prepump/cache defer as a stack member

Decision:

- Parallel mid pump is a real default-off partial, but not a playable candidate.
- No default was promoted.
- The next real fix is not another isolated cap/thread toggle. The remaining blocker is ownership-lane state across request, exact generation, upload/apply, surface extraction, and publish.

## Upload Lane Budget Candidate Rejected - 2026-06-05

Attempted a default-off upload lane budget slice, then removed it after same-code control validation.

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

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Decision:

- removed `VENPOD_SPARSE_UPLOAD_LANE_BUDGETS`, `PopBestUploadForLane`, harness flags, and parser columns;
- no `SPARSE_UPLOAD_LANE_BUDGET` symbols remain;
- isolated upload queue selection is not the next fix. Ownership lanes must be carried through request, generation, apply/upload, surface extraction, and publish readiness as one state-machine slice.

## Ownership-Stage Queue Branch Rejected - 2026-06-05

Implemented and validated a default-off ownership-stage queue slice:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS=0`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET=8`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET=8`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS`

Build/test passed:

- `.\build.ps1 -Config Release`
- `ctest --test-dir build --output-on-failure -C Release`

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_explicit_lanes_candidate`
- updated `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- updated `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`

Rows:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| ownership queues | `44.05/14.51` | `117.65/79.63` | `98.90/60.35` | rejected |
| ownership queues + explicit source lanes | `48.94/12.13` | `90.49/80.23` | `84.48/49.17` | rejected |

Decision:

- Persistent ownership queues fixed the naive scan overhead, but the current stack labels nearly all queued stage work as visible/critical.
- Explicit source lanes produce prefetch queues, but the full stack still regresses badly versus the current validated stack.
- The blocker is earlier than stage selection: request/generation still admits too much work as visible/current-critical, while sampled walk debt remains fallback-invalid/unknown.
- Do not promote ownership-stage budgets. Do not repeat upload/surface-only stage budgeting as the next fix.

## Goal Charter / Window Validation Anchor - 2026-06-05

Active goal remains `streaming_playability_real_fix_campaign_20260604`.

Default-neutral validation infrastructure:

- `VENPOD_PERF_FRAME_END_LOG_INTERVAL`, unset by default.
- `perf_noncapture_smoke.ps1` writes `window_summary.csv` / `window_table.md`.

Accepted-stack validation artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_framewindow_20260605`

Rows:

- fixed f384: raw `19.58`, CPU `17.89`, request/gen/clip `3.82/13.13/0.93`, surface extract `20.60`, GPU `5.67`
- walk realtime f600: raw `58.94`, CPU `48.30`, request/gen/clip `12.55/12.87/22.86`, surface extract `9.78`, GPU `17.85`
- high-alt f360: raw `48.21`, CPU `30.41`, request/gen/clip `8.96/4.78/16.66`, surface extract `8.13`, GPU `5.99`

Decision: use this accepted-stack validation for the next branch. `frameend_interval_validation_20260605` used a mixed persistent-mid/interest-2 probe and is not the accepted baseline.

## Streaming Playability Resume Rejections - 2026-06-05

### Persistent Surface Workers Rejected - 2026-06-05

Attempted a default-off persistent surface extraction worker pool behind existing parallel surface extraction, then removed it after validation.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch16_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch8_candidate_20260605`

Against the persistent exact 2-worker stack, the best surface-worker variants either regressed fixed/walk CPU or only moved raw time while clip/CPU worsened:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| baseline | `21.81/17.17` | `67.23/32.40` | `51.21/28.80` | current partial |
| surface persistent 4w | `18.90/18.22` | `67.16/42.00` | `50.49/29.60` | rejected |
| surface persistent 4w batch16 | `21.84/19.62` | `60.95/44.43` | `47.96/30.80` | rejected |

Decision: no persistent surface worker flag remains. Do not retry surface extraction as a thread-backend tweak; future surface work needs publish/backlog-aware lane architecture.

### Diagnostic-Off / Bounded64 Checks - 2026-06-05

- `accepted_stack_noheavydiag_20260605`: heavy diagnostics are not the first blocker; walk remains about `63.72 ms` raw and high-alt about `52.95 ms` raw.
- `bounded64_noheavydiag_candidate_20260605`: bounded64 remains default-off/comparison-only; raw moved in some rows but CPU/clip regressed.
- Current remaining blocker is the missing ownership-aware streaming state machine across request, generation, upload/apply, surface extraction, and publish.

Latest durable campaign memory:

- `handoff.md`, section `Campaign Resume - Visible Cache And Inline Surface Rejections - 2026-06-05`

Rejected/diagnostic-only branches from the resume:

- Inline generated surface extraction was implemented, measured, rejected, and removed.
  - fixed `25.10/40.16 raw/CPU`
  - walk realtime `96.91/112.71`
  - high-alt `52.23/35.95`
  - it reduced surface extraction but moved work into generation/clip/CPU
- Full visible-lane/cache-only deferral is rejected.
  - it classified all missing bricks as cache, deferred `3586` bricks indefinitely, left broad mid coverage at `61`, and drove request cost to about `41 ms`
  - this is a fake performance win and must not be used as the playable stack
- High-alt-only projected cache deferral is diagnostic-only.
  - high-alt pump improved `5.63 -> 1.82 ms`, but high-alt raw stayed about `52 ms` and walk regressed
- Current-stack parallel mid pump retest is not a new win.
  - parallel pump activated in walk, but CPU regressed to `60.51 ms`

Current root decision:

- Incremental scroll/reuse is not the current representative walk blocker; latest walk frame `600` had `newV=0`, `goneV=0`, reused interest, and old visible debt generated serially.
- Walk/realtime missing debt remains mostly projected-visible and fallback-invalid/unknown, so unknown bricks cannot be moved async or treated as cache.
- High-alt has real unsampled cache debt, but safe cache deferral alone is not enough.
- Next real implementation slice must split exact/surface request, generation, publish, and surface extraction into public-critical versus repair/cache lanes with separate budgets and apply/upload accounting.

## Streaming Playability Surface Lane Budget Rejection - 2026-06-05

Latest durable campaign memory:

- `handoff.md`, section `Campaign Continuation - Surface Lane Budget Rejected - 2026-06-05`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_lane_budget_rejected_20260605.md`

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Rejected and removed branches:

- hidden post-open pre-publish surface cap:
  - fixed `22.37/21.65 raw/CPU`
  - walk `74.88/52.92`
  - high-alt `49.68/34.30`
  - rejected because general surface extraction refilled the frame and walk regressed
- combined hidden post-open cap plus low-lane surface extraction budget:
  - fixed `23.86/18.56 raw/CPU`
  - walk `56.01/69.32`
  - high-alt `183.41/36.64`
  - rejected because it shifted work into catch-up/gap debt and regressed representative rows

Current root decision:

- Do not retry hidden-only pre-publish caps, prefetch surface caps, or low-lane surface caps.
- The remaining blocker is not a simple surface-budget tuning problem.
- Next fixes must reduce real visible/sampled work or add ownership-proved async/apply architecture; unknown fallback and sampled visible debt remain guarded.

## Streaming Playability Publish Lane Propagation - 2026-06-05

Latest build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Latest durable campaign memory:

- `handoff.md`, section `Campaign Continuation - Publish Lane Propagation - 2026-06-05`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/publish_lane_propagation_vsync0_20260605/table.md`

New retained behavior-neutral infrastructure:

- `SparsePendingPageTablePublish::streamingLane`
- `SparseBrickUploadPacket::streamingLane`
- lane-aware page publish queue stats
- `PERF_SPARSE_PAGE_PUBLISH_LANES`

Decision:

- publish-lane propagation is useful and safe because it only carries existing lane metadata into upload/publish diagnostics at default behavior.
- the rejected `VENPOD_SPARSE_POST_OPEN_PREPUBLISH_SURFACE_MAX_MS=4` probe improved some raw rows but accumulated thousands of pending publishes/surface gates, so it is debt shifting, not a playable candidate.
- the representative walk frame `600` still had raw/CPU `74.08/49.25 ms`; its publish queue was visible-lane (`0/0/5/0`) and `qsurf=0/43/0/0`, so prefetch publish/surface deferral is not the next safe walk fix.
- all defaults remain unchanged.

Current root decision:

- the remaining walk blocker is visible sampled/unknown work distributed across request/generation/clip/surface/GPU, not cache-only publish debt.
- next safe work must either prove ownership for sampled missing mid bricks, reduce the actually visible sampled footprint under a screen/ray contract, or implement a larger ownership-aware streaming state split. Do not retry local prefetch surface deferral, queue-order heuristics, blunt caps, or unknown-fallback async.

## Streaming Playability Explicit Source Lanes - 2026-06-05

Latest build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Latest durable campaign memory:

- `handoff.md`, section `Campaign Continuation - Explicit Source Lanes - 2026-06-05`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_campaign_20260605_table.md`

New retained default-off candidate:

- `VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES=0`
- `perf_noncapture_smoke.ps1 -RequestExplicitSourceLanes`
- explicit-lane residency touch APIs in `SparseBrickPool` and `SparseVoxelWorld`

Decision:

- explicit source lanes are a real partial state-machine slice: broad view-cone and terrain-prefetch requests can retain visible residency while preserving `Prefetch` generation/upload/surface lane.
- walk and high-alt CPU improved in matched rows, but raw frame time stayed far above budget, so this is not a playable candidate.
- blind prefetch-lane surface deferral was implemented, measured, rejected, and removed because it created large surface/publish backlog and did not improve frame time.
- parallel mid pump remained mixed and parallel surface extraction regressed walk; neither is accepted globally here.
- all defaults remain unchanged.

Current root decision:

- the remaining fix must carry ownership lanes through the full streaming/publication state machine: request, interest, generation, clipmap pump, upload/apply, surface extraction, and publish readiness.
- do not repeat local queue ordering, blind surface deferral, ring-only visible-critical classification, age-priority, or blunt pump caps.

## Streaming Playability Request/Generation Continuation - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Latest durable campaign memory:

- `handoff.md`, section `Campaign Continuation - Request Slice Rejected / Critical Generation Batch - 2026-06-04`

New retained default-off candidate:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`
- `SparseVoxelWorld::PumpGenerationForCoordsParallel(...)`

Decision:

- request resident-touch cache was implemented, measured, rejected, and removed.
- terrain-critical same-frame parallel generation is retained as a safe default-off partial: walk raw improved from `65.89` to `51.34 ms`, but CPU remains about `46.57 ms`, so this is not a playable candidate.
- V2 visible coverage guard remains high-alt/cache-debt-only; the combined walk row worsened raw time.
- no defaults were promoted and no correctness policy was weakened.

## Streaming Playability Real Fix Campaign V2/Interval-2 Update - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed, `ninja: no work to do`.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched code/harness had no whitespace errors, only existing LF-to-CRLF warnings.

New default-off code retained:

- `VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS=0`
- `PERF_SPARSE_STREAMING_LANES`
- behavior-neutral `SparseStreamingLane { Cache, Prefetch, Visible, PublicCritical }` metadata/queue counters
- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2=0`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_v2_interval2_cycle_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_v2_interval2_cycle_table.md`
- `build/captures/accepted_stack_interval2_v2_all_20260604/`

Key rows:

| Branch | Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | GPU | Missing sampled/unsampled | Coverage | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| V2 guard | high-alt | `385` | `48.84` | `26.00` | `4.40` | `5.78` | `15.81` | `6.06` | `10.18` | `1/1864` | `99` | `0/0` | real isolated high-alt cache-debt win |
| V2 guard | walk realtime | `600` | `74.15` | `34.44` | `11.59` | `13.18` | `9.65` | `7.17` | `12.35` | `298/196` | `94` | `0/0` | safe no-op; sampled-visible debt stayed critical |
| interval-2 | walk realtime | `600` | `67.99` | `40.70` | `15.63` | `16.71` | `8.34` | `6.13` | `13.72` | `239/190` | `95` | `0/0` | partial clip-interest win |
| accepted retest | fixed | `379` | `30.06` | `18.51` | `11.86` | `5.77` | `0.89` | `0.00` | `9.08` | `0/0` | `100` | `0/0` | not 60 fps |
| accepted retest | walk realtime | `600` | `114.88` | `49.00` | `12.05` | `15.62` | `21.30` | `18.79` | `23.11` | `295/188` | `94` | `0/0` | not 60 fps |
| accepted retest | high-alt | `400` | `112.25` | `53.42` | `19.69` | `19.11` | `14.61` | `7.49` | `23.11` | `4/1873` | `99` | `0/0` | not 60 fps |

Rejected in this cycle:

- pressure-trim free-page guard: moved cost into surface/GPU/raw (`136.15 ms` walk raw)
- parallel surface extraction: regressed CPU/raw (`106.14 ms` walk raw)
- parallel exact generation: regressed frame/missing debt (`73.96 ms` walk raw)
- direct footprint columns: regressed request/gen/GPU (`101.25 ms` walk raw)
- async exact generation: no useful work (`enqueued/applied=0/0`) and regressed (`119.41 ms` walk raw)

Decision:

- No validated 60 FPS candidate exists.
- V2 visible-critical coverage guard is safe default-off and useful for high-alt cache-only debt, but it cannot defer sampled-visible walk debt without weakening ownership.
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2` is a useful candidate setting for reducing some clipmap interest work, but it is not enough and showed frame variance.
- The remaining blocker is a coupled ownership-aware streaming/publication architecture gap:
  - sampled-visible fallback-invalid/unknown work must stay public-critical;
  - cache/prefetch must be separated earlier and carried through request, generation, surface extraction, upload/apply, and publication;
  - local caps/threading variants just move cost or create unsafe debt.

Next implementation:

- Implement explicit public-critical/sampled-visible/cache/prefetch/maintenance lanes across exact request, generation, surface extraction, upload/apply, and page publication queues.
- Add per-lane budgets, backlog ages, and apply/upload pressure.
- Only async/budget cache/prefetch or CPU-proven fallback-valid work. Unknown sampled fallback remains critical.

## Streaming Playability Real Fix Campaign Closure - 2026-06-04

Latest continuation:

- Exact ViewCone speculative demotion / exact async probe was measured as a no-op and removed from code/harness.
  - walk realtime frame `600`: raw `71.90`, CPU `43.56`, request/gen/clip/pump `14.86/15.52/13.17/10.94`, GPU `14.43`, missing `438`, sampled `240`, miss/unsafe `0/0`
  - exact public-lane demotions: `0`
  - exact async enqueued/applied: `0/0`
  - high-alt frame `393`: raw `48.94`, CPU `25.67`, clip/pump `14.83/4.29`, missing `1486`, sampled `0`, exact async enqueued/applied `0/0`, miss/unsafe `0/0`
- VSync-off probe did not turn the accepted stack into a frame-pacing fix:
  - walk realtime frame `600`: raw `74.62`, body `69.82`, CPU `45.68`, GPU `6.04`, `vsync=0`
- Mid-clipmap cache-only defer was measured and rejected as a candidate.
  - high-alt frame `404`: raw `62.14`, CPU `33.82`, clip/pump `22.22/9.37`, missing `1501`, sampled `1101`, visible-critical coverage `87`, defer count `0`, miss/unsafe `0/0`
  - walk realtime frame `600`: raw `66.53`, CPU `51.54`, request/gen/clip/pump `11.91/13.41/26.20/13.26`, missing `488`, sampled `289`, defer count `0`, miss/unsafe `0/0`

Decision:

- No validated 60 FPS candidate exists.
- The strongest default-off stack remains the worker-local mid column cache stack:
  - fixed raw about `23.77`, CPU `16.95`, GPU `7.97`
  - walk realtime raw about `49.61`, CPU `24.04`, GPU `12.72`
  - high-alt raw about `50.85`, CPU `30.05`, GPU `10.05`
- The next real implementation is an ownership-aware streaming/publication state split:
  - public-critical/sampled-visible lanes stay guarded;
  - cache/prefetch lanes get separate request, generation, surface extraction, upload/apply, and publication budgets;
  - async generation applies only to cache/prefetch or CPU-proven fallback-valid work;
  - sampled fallback-unknown/invalid work remains critical.

Do not retry:

- exact ViewCone speculative demotion
- global cache-only defer
- VSync-only tuning
- queue caps/ring heuristics that defer sampled unknown fallback

## Streaming Playability Real Fix Campaign Update - 2026-06-04

Latest continuation:

- A public-critical-only page-table surface gate probe was implemented, validated, rejected, and removed.
- Build/test after removal passed:
  - `.\build.ps1 -Config Release`
  - `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_gate_public_critical_only_candidate_20260604/`

Validation:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface | GPU | Missing sampled | Coverage | Miss/unsafe | Gate result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `22.30` | `21.79` | `4.35` | `16.09` | `1.35` | `0.00` | `18.90/1.90` | `6.25` | `0` | `100` | `0/0` | `latePublishes=0` |
| walk realtime | `600` | `92.00` | `60.95` | `10.41` | `11.81` | `38.70` | `36.83` | `8.86/2.64` | `17.36` | `421` | `94` | `0/0` | `latePublishes=0`, `criticalDefers=6` |
| high-alt | `360` | `49.84` | `30.70` | `7.16` | `5.80` | `17.74` | `5.81` | `3.59/1.81` | `10.87` | `0` | `99` | `0/0` | `latePublishes=0` |

Decision:

- Rejected and removed. The target frames had no noncritical late-publish opportunity, so a page-table surface gate bypass is not the missing architecture slice.
- Walk regressed into clip/pump catch-up and was much worse than the accepted stack.
- Do not retry this as `VENPOD_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL_ONLY`; no such flag remains in the code.
- The next real fix must introduce explicit streaming lanes before request/generation/surface/publish, not try to infer them at the page-table gate.

Latest continuation:

- `.\build.ps1 -Config Release`: passed after adding `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=0`.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- The new flag is default-off. It counts the surface-ready side queue as protected visible publish pressure only when both `VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE=1` and `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=1` are enabled.
- `PERF_SPARSE_SURFACE_READY_PUBLISH` now logs `pressure=0/1`.

Latest publication-queue validation:

| Branch | Walk raw ms | Walk CPU ms | Request | Gen | Clip | Pump | Surface | GPU | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| strongest accepted stack | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `5.82` | `8.57/2.92` | `12.72` | `0/0` | baseline |
| surface-ready queue only | `90.29` | `61.98` | `10.45` | `13.83` | `37.69` | `35.59` | `8.52/2.93` | `16.19` | `0/0` | rejected; side queue hid pressure and clip catch-up exploded |
| surface-ready queue + pressure | `62.23` | `43.31` | `12.61` | `14.94` | `15.75` | `4.02` | `8.59/2.77` | `17.22` | `0/0` | rejected; better than queue-only but worse than accepted |

Current root decision:

- Surface-ready publish pressure is safe to keep default-off as a diagnostic/prototype, but rejected as part of the playable candidate stack.
- Local publication queue pressure tuning is closed as a 60 FPS path.
- The remaining fix is the ownership-aware streaming/publication state split: public-critical visible/collision pages must be separated from cache/prefetch work before async surface/generation lanes can help safely.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after default-off time-budgeted parallel surface extraction.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched code had no code whitespace errors, only existing line-ending warnings.

New default-off knobs:

- `VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_WORKERS=4`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MIN_BRICKS=4`
- `VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH=32`

Rejected continuation rows:

| Branch | Walk raw ms | Walk CPU ms | Request | Gen | Clip | Surface | GPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| strongest accepted stack | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `8.57/2.92` | `12.72` | current baseline |
| fast resident touch | `59.01` | `38.22` | `14.70` | `15.75` | `7.70` | `12.25/3.06` | `5.67` | rejected |
| surface extraction max `1` | `63.83` | `34.30` | `13.22` | `15.13` | `5.93` | `9.89/2.46` | `11.95` | rejected |
| parallel surface uncached | `64.81` | `36.94` | `14.68` | `16.41` | `5.84` | `16.11/2.91` | `12.70` | rejected |
| parallel surface cached | `72.39` | `41.06` | `14.28` | `19.01` | `7.75` | `12.78/2.89` | `9.87` | rejected |
| parallel surface time-budgeted batch `32` | `76.24` | `36.50` | `13.67` | `16.20` | `6.62` | `11.75/2.97` | `14.38` | rejected |

Current root decision:

- No validated 60 FPS candidate exists.
- The strongest stack remains the worker-local mid column cache stack.
- Synchronous worker batching is not enough for surface extraction because the public surface gate is time-budgeted; batch overruns just move debt into request/generation/clip.
- The accepted walk row had `qsurf=0/3265/0/0`, proving representative surface debt is visible-class rather than speculative/cache; async speculative surface extraction would be a no-op until public-critical/cache publication state is split.
- The next real fix should split streaming/publication state into public-critical and cache/prefetch lanes, then async-generate/extract only non-public-critical work with main-thread apply/upload budgets.

## Campaign Continuation - Worker-Local Mid Column Cache - 2026-06-04

Active campaign handoff: `handoff.md`.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the worker-local mid column cache and surface-general-budget instrumentation.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- Known `rayDir` shadow warnings remain.

Updated artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`

Accepted default-off partial:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE=0`
- purpose: preserve the synchronous visible-critical mid-clipmap generation path while giving each parallel pump worker its own terrain column cache
- this improves generation throughput without treating sampled unknown fallback as safe

Strongest continuation stack:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `360` | `23.77` | `16.95` | `4.15` | `12.03` | `0.76` | `0.00` | `7.17/1.60` | `7.97` | `0` | `100` | `0/0` |
| walk realtime | `600` | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `5.82` | `8.57/2.92` | `12.72` | `357` | `95` | `0/0` |
| high-alt | `360` | `50.85` | `30.05` | `7.65` | `6.58` | `15.82` | `4.27` | `3.90/1.89` | `10.05` | `0` | `100` | `0/0` |

Rejected/default-off probes:

- strict surface time budget: walk regressed to raw `102.71`, CPU `40.79`
- shared mid column cache: disabled parallel and regressed walk to CPU `44.74`, clip `28.15`
- surface sort cache: zero cache hits and walk CPU `45.95`
- surface partial sort: walk CPU `34.35`, no stack win
- surface general strict budget: skipped general catch-up but protected surface/mid catch-up still dominated; walk CPU `31.92`
- mid pump `8` workers: worsened walk CPU to `36.38`
- parallel exact generation with worker cache: no representative walk win
- async exact generation with visible async off: no-op in sampled rows

Current root decision:

- No validated 60 FPS candidate exists. Current completion state is Completion B: strongest default-off candidate stack plus exact remaining blocker table.
- The worker-local column cache is safe to keep as an opt-in partial, but it is not a playable fix.
- Representative walk is now balanced across CPU and GPU buckets: CPU `24.04`, request `8.83`, generation `7.82`, clip/pump `7.38/5.82`, surface `8.57/2.92`, GPU `12.72`, raw `49.61`.
- The next real fix should be an ownership-aware streaming/publication state split with separate budgets for request prep, visible-critical generation, cache/prefetch generation, upload/apply, and surface publication.
- All defaults remain unchanged; blunt pump cap, age-priority prototype, and ring-only visible-critical heuristic remain rejected.

## Streaming Playability Campaign Surface/Scan Continuation - 2026-06-04

Active campaign handoff: `handoff.md`.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after hidden-tracked scan budgeting
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- `.\build.ps1 -Config Release`: passed after surface sort cache / partial-sort changes
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Updated artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`

New default-off candidates:

- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET=512`
- `VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE=0`
- `VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT=0`

Current strongest stack remains:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `385` | `34.93` | `11.71` | `9.75` | `1.09` | `0.83` | `0.00` | n/a | `5.90` | `0` | `100` | `0/0` |
| walk realtime | `600` | `46.49` | `31.05` | `8.15` | `7.00` | `15.87` | `13.33` | `9.00/3.31` | `14.60` | `323` | `95` | `0/0` |
| high-alt | `396` | `52.25` | `25.55` | `6.20` | `6.33` | `13.01` | `4.65` | n/a | `12.12` | `0` | `99` | `0/0` |

Rejected follow-up branches:

- Hidden-tracked scan budget `512`: walk raw `56.03`; scan counts dropped but surface extraction stayed about `9 ms`.
- Hidden-tracked scan budget `128`: walk raw `53.88`; lower scan budget still did not reduce the surface bucket.
- Surface sort cache: walk raw `48.71`; sort calls `3`, cache hits `0`.
- Surface partial sort: walk raw `49.23`; small surface reduction only, no stack win.
- Direct footprint path: walk raw `47.85`; coverage dropped to `94`, missing voxel rose to `485`, and budget reason became `2`.

Current root decision:

- No validated 60 FPS candidate exists. The run is at Completion B: strongest default-off candidate plus exact remaining blocker table.
- Representative walk remains dominated by combined CPU and GPU costs: CPU `31.05 ms`, GPU ray `14.60 ms`, raw `46.49 ms`.
- The next real fix is not another queue-order, scan-budget, or sort-only tweak. It should be an ownership-aware streaming state-machine slice that separates visible-critical, cache/prefetch, generated, staged, uploaded, and fallback-valid work with independent budgets.
- All defaults remain unchanged.

## Budget-Compatible Parallel Mid Pump Update - 2026-06-04

Active campaign handoff: `handoff.md`.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the budget-compatible parallel mid-pump patch
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Patch:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=1` now works with the backlog-aware pump budget instead of being disabled whenever `voxelPumpBudgetActive` is true.
- The budgeted parallel path limits pending worker bricks to `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS` and marks budget hit when queued work remains after consuming the budget.
- Defaults remain unchanged. The parallel pump is still default-off.

Latest artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Best current partial row:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `378` | `27.78` | `3.63` | `2.04` | `0.73` | `0.83` | `0.00` | n/a | `7.33` | `0` | `0` | `100` | `0/0` |
| walk realtime | `600` | `70.54` | `35.90` | `17.17` | `11.93` | `6.78` | `4.69` | `11.97/3.28` | `13.72` | `391` | `324` | `95` | `0/0` |
| high-alt | `390` | `60.77` | `27.59` | `5.79` | `8.75` | `13.05` | `4.64` | n/a | `16.11` | `1495` | `0` | `100` | `0/0` |

Decision:

- Accepted as a default-off partial code candidate: it finally activates the existing parallel mid pump under the backlog-aware safety budget and reduces representative walk CPU versus the surface-max4 row (`44.10 -> 35.90 ms`) without miss/unsafe regression.
- Not a validated playable candidate: representative walk is still `70.54 ms` raw and fixed/high-alt are still over the 60 FPS budget.
- Rejected this slice:
  - no-backlog parallel as accepted stack; useful diagnostic, but it disables the backlog-aware safety policy
  - trim `8k` as general stack; high-alt became unstable (`coverage=86`, sampled debt `865`)
  - hidden exact generation budget `16`; walk coverage dropped to `94` and clip/pump rose
  - surface max `1 ms`; it moved debt into generation/fixed rows and worsened walk raw time
  - pressure-trim free guard remains rejected

Current root decision:

- The mid-pump worker path is now a real opt-in partial, but the engine is still not playable.
- The current walk blocker is no longer just clip pump. The remaining measured buckets are request admission (`17.17 ms`), exact generation (`11.93 ms`), surface extraction/staging (`11.97/3.28 ms`), and residual GPU/post-wait around sampled unknown mid debt (`391` missing, `324` sampled, fallback-valid still zero).
- The next architecture slice should split exact request/generation/surface-publication into public-critical versus repair/cache lanes with separate budgets and publish readiness, rather than adding another queue-order tweak or treating unknown fallback as safe.

## Streaming Playability Campaign Update - 2026-06-04

Active campaign handoff: `handoff.md`.

Latest continuation after compaction:

- Build passed after cached-column worker and pressure-trim code changes: `.\build.ps1 -Config Release`
- Tests passed: `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- Master campaign table now includes the rejected/no-op continuation rows and pressure-trim rows:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Rejected/ruled out this continuation:

- cached exact same-frame parallel generation:
  - walk realtime: `rawMs=207.85`, `cpuUpdateMs=140.79`, `clipMs=77.03`
  - high-alt: `rawMs=207.86`, `cpuUpdateMs=236.39`
- existing async exact generation with cached worker:
  - enqueued `0` in representative rows because current queued exact work is visible-class; visible async remains blocked without an owner proof
- hidden-exact water throttle (`VENPOD_SPARSE_HIDDEN_EXACT_WATER_MAX_REQUESTS=1`):
  - reduced hidden-exact accepted requests but moved the walk row into clip catch-up: `cpuUpdateMs=117.13`, `clipMs=77.13`
- bounded64 current stack:
  - walk realtime: `cpuUpdateMs=131.54`, `clipMs=68.64`, still default-off/comparison-only
- pressure-trim free-page guard:
  - walk realtime: `rawMs=164.69`, `cpuUpdateMs=141.35`, `clipMs=72.25`
  - high-alt: `rawMs=115.77`, `cpuUpdateMs=156.88`

Current root decision:

- The walk/realtime blocker is not solved by generation threading, water-lane throttling, or bounded promotion.
- Pressure-trim scanning is a real request-phase cost, but a free-page guard is unsafe and incremental trim is only a partial default-off candidate.
  - incremental trim 32k walk realtime: `rawMs=108.50`, `cpuUpdateMs=68.71`, `request/gen/clip=29.77/29.80/9.10`
  - incremental trim 16k high-alt: `rawMs=75.70`, `cpuUpdateMs=40.75`, `request/gen/clip=11.60/11.33/17.81`
  - incremental trim 16k walk realtime: `rawMs=117.43`, `cpuUpdateMs=69.71`, `request/gen/clip=32.85/30.15/6.69`, `surfaceExtractMs=15.56`
- The best current row remains `parallel_mid_pump_candidate` walk realtime frame `600`: `rawMs=75.97`, `cpuUpdateMs=40.80`, `request/gen/clip=18.90/15.79/6.09`, `surfaceExtractMs=11.16`, `gpuRayMs=13.45`, `miss/unsafe=0/0`; however `parallelPumpActive=0`, so this is a least-bad stack row, not an accepted patch win.
- Next branch must target the exact request/generation/surface-publish pipeline with ownership-aware classification:
  - hidden-exact accepted requests,
  - terrain-critical request admission,
  - generated/uploaded/surface-ready state,
  - surface extraction/publish lag.
- Do not move visible exact work async and do not throttle hidden-exact repair unless a valid owner/fallback proof explains why the deferred work is not public-critical.

Latest same-frame generation cycle:

- Build passed: `.\build.ps1 -Config Release`
- Tests passed: `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- Updated artifacts:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Default-off knobs added this cycle:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0`
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=0`

Decision:

- Parallel mid-clipmap pump did not activate in the latest noncapture matrix (`parallelPumpActive=0`), so its better walk row cannot be credited to the patch.
- Parallel exact generation activated, but is rejected: walk realtime regressed to `rawMs=209.53`, `cpuUpdateMs=146.40`, `clipMs=81.87`, `missingVoxel=527`; high-alt regressed to `rawMs=176.07`, `cpuUpdateMs=77.50`.
- Miss/unsafe stayed `0/0`, but the candidate is not safe as a playability fix because it increases streaming debt and frame time.
- The next branch should not retry direct exact generation. The remaining blocker is the cached exact generation/request/surface pipeline around visible sampled terrain debt.

Latest continuation checkpoint:

- active goal: `streaming_playability_real_fix_campaign_20260604`
- build passed after latest accepted code changes: `.\build.ps1 -Config Release`
- tests passed: `ctest --test-dir build --output-on-failure -C Release`, `1/1`
- accepted:
  - public-open-frame latch fix in `src/main_launcher.cpp`
  - behavior-preserving voxel-slot storage reuse in `SparseClipmap`
- rejected/diagnostic-only:
  - post-open surface extraction cap
  - `VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS=1`
  - `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`
  - larger mid voxel pool / lower interest-percent tuning as a fix

Latest focused matrix:

- `build/captures/noncapture_voxel_slot_reuse_candidate_all_20260604/table.md`

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ray ms | Missing | Sampled/unsampled | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `23.96` | `21.41` | `4.15` | `16.35` | `0.90` | `0.00` | `6.16` | `0` | `0/0` | `0/0` | close, not 60 |
| walk realtime | `900` | `77.86` | `46.70` | `12.39` | `3.15` | `31.12` | `17.11` | `15.52` | `436` | `366/70` | `0/0` | clip/parent-held catch-up returned |
| high-alt | `401` | `48.99` | `24.63` | `5.62` | `5.27` | `13.73` | `5.48` | `11.87` | `1813` | `41/1772` | `0/0` | improved, still not 60 |

Current root decision:

- The next blocker is parent-held LOD ownership/catch-up, not simply missing brick count or pool capacity.
- Increasing mid voxel pool capacity can reduce missing counts, but it caused `parentHeld` / visual ownership failure to explode. That forces broad synchronous catch-up and is not a safe performance fix.
- The next implementation branch should map parent-held pixels back to preferred child clipmap coordinates and prioritize those child bricks, using bounded feedback or a CPU projection path. Do not add another queue cap, pool-size tweak, or unknown-fallback deferral.

Accepted latest fixes:

- `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=0` added default-off. Opt-in surface staging now patches/appends dirty surface metadata instead of full metadata rebuilds when fixed range tables/stable draw slots/compact draws are active.
- `SparseSurfaceRangeAllocator` now batches stats recomputation across surface dirty staging frees/resizes. This is behavior-preserving and reduced the measured dirty-stage allocator overhead.
- `VENPOD_SPARSE_STATS_SINGLE_FLUSH=0` added default-off. Opt-in avoids the second explicit sparse stats scan after deferred stats already flushed in request/generation/post-upload phases.
- `PERF_SPARSE_REQUEST_DETAIL` now splits request into hierarchy, terrain-critical, terrain surface prefetch, stats flush, and remaining other time.
- `perf_noncapture_smoke.ps1` is the current log-only noncapture scoreboard harness.

Latest consolidated opt-in candidate stack:

```text
VENPOD_RENDER_QUALITY=playable
VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1
VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375
VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1
VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=1
VENPOD_SPARSE_STATS_SINGLE_FLUSH=1
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

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Latest noncapture rows:

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ray ms | Missing | Sampled/unsampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `375` | `21.39` | `16.27` | `10.93` | `4.55` | `0.79` | `0.00` | `6.93` | `0` | `0/0` | `0/0` |
| walk realtime | `600` | `112.09` | `64.87` | `33.39` | `23.55` | `7.87` | `5.28` | `18.08` | `447` | `378/69` | `0/0` |
| high-alt | `399` | `53.11` | `29.52` | `7.92` | `5.49` | `16.10` | `5.94` | `14.49` | `1874` | `44/1830` | `0/0` |

Decision:

- Not a validated 60 FPS candidate.
- Surface staging metadata upload is no longer the first walk blocker after the accepted fixes, but surface extraction is still material.
- Large sparse pool/page table removed replacement scans in focused walk rows, but it remains opt-in and does not solve 60 FPS.
- Realtime walk is currently blocked by request/generation/surface extraction/GPU with clip secondary in the accepted row. The next branch must split request `otherMs` and inspect hidden-exact/surface extraction coupling without deferring sampled unknown/invalid bricks.

Rejected/not accepted:

- request resident fast-path lookup patch, removed after no reliable win
- shared column cache as realtime walk fix
- vsync-off as a playability fix
- bounded64 as a performance stack component; it promoted surface but drove walk clip/pump to about `71/70 ms`
- screen-critical reuse at speed `64`; it reduced request work by starving critical requests and causing clip catch-up
- targeted coverage catch-up; it did not drain the frame-600 debt and was removed

## Current Invariant

A public frame is valid only if every visible pixel has an owner whose result agrees with deterministic terrain/water truth for that ray.

Valid owners:

- exact sparse surface
- mid voxel, only outside the exact-required band or when proven equivalent enough for the current camera footprint
- Far-SVO, only inside the valid Far-SVO domain and not contradicting terrain/water truth
- deterministic water, only when water is first along the ray and terrain truth allows it
- sky, only when deterministic terrain/water says no earlier hit exists

## Current Suspect

Post-open shoreline/basin pixels are being accepted with coarse mid/Far-SVO ownership because the foreground handoff has not proven enough exact foreground coverage to promote the wider sparse-surface raster band.

Current latest checkpoint says:

- `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS=1` now keeps the public frame hidden until same-camera exact-contract unsafe feedback is clean.
- After public open, `surfaceRasterMax` remains `1024`.
- `surfacePromoted=0`.
- `surfaceClean=0/1`.
- `midVoxelScreenPct` remains about `19.20%`.
- hidden-exact feedback can still appear after open; when feedback reaches zero, the clean-ray sweep is still far from complete (`cleanRays=758/9417`, `clean=0/8` at frame 240 in the latest validation).

The important correction: this is not simply "startup proof was clean, so consume it and promote wide." The startup proof set was too narrow. The regular hidden-exact scanner continued finding foreground exact misses after the startup proof looked clean.

So the immediate blocker is still not water. The renderer is showing exact-near only plus coarse lower LOD in a visually sensitive post-open frame, and the proof/promotion contract has not established that lower LOD is legitimate there.

## Current Root Decision

The default public-frame promotion proof is over-constrained for runtime startup. It requires hidden-exact runtime clean sweeps that are too slow and reset on later feedback, leaving the renderer stuck at `surfaceRasterMax=1024` and exposing about `19%` mid-voxel ownership after public open.

The opt-in bounded promotion policy is the first evidence-backed candidate fix. It promoted the wide surface raster to `2560`, reduced mid voxel ownership from about `19.2%` to about `9.4%` in the first fixed-camera probe, and did not introduce miss/unsafe regressions.

The broader validation pass now supports that direction: the long fixed-camera run stayed promoted and clean, the low-altitude walk run eventually promoted and stayed clean, and the bounded basin audit materially increased exact ownership while reducing mid/Far-SVO ownership. The high-altitude stress run passed safety checks but did not exercise promotion because that path suppressed wide promotion.

Defaulting decision still pending:

- If bounded promotion becomes default, do it with explicit repair/demotion counters.
- If it fails, classify failure by hidden-exact growth, flicker, frame time, miss/unsafe, or unchanged basin ownership.
- Return to mode-70 water classification only after the foreground contract is validated or disproven.

## Evidence

- Far-SVO radius bug fixed:
  - default page radius `6 -> 12`
  - old broad-view analytic far-height fallback removed in tested captures

- Startup unsafe gate fixed:
  - `src/main_launcher.cpp`
  - `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` default is now `1`
  - validation: `build/captures/current_goal_v4c_shader_unsafe_default_on_20260602`
  - frame 120: `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`, `miss=0`, `farHeight=0`

- Remaining post-open failure:
  - surface still not promoted wide
  - mid voxel still owns a large screen fraction
  - latest attempted startup wide-promotion edit did not help and was reverted

- Basin audit:
  - `build/captures/current_goal_v4e_post_open_basin_audit_20260602`
  - `411/512` sampled suspicious pixels are generated land above sea level
  - `47/512` are water-before-terrain conflicts
  - dominant buckets include `far_svo|stone|mixed = 125`, `mid_voxel|stone|side = 93`, `mid_voxel|dirt|mixed = 93`

- Hidden-exact startup/repair probes:
  - `build/captures/root_probe_startup_hiddenexact_block_20260602`
    - startup hidden-exact proof reached a clean audit, but regular hidden-exact feedback continued after the proof
    - still no wide promotion: `surfaceRasterMax=1024`, `surfacePromoted=0`, mid voxel about `19.16%`
    - contradiction observed: startup proof appeared converged while runtime `hiddenExactClean` stayed `0/8`
  - `build/captures/root_probe_repair_clean8_env_20260602`
    - old repair logic could report `hiddenExactRepairClean=8/8` while `hiddenExactRepairCleanRays=0`
    - conclusion: the old quick convergence shortcut was bogus and should not be trusted as a proof
  - `build/captures/root_patch_startup_repair_scan_default_20260602`
    - strict repair blocking inherited 16 startup proof phases
    - by frame 420 it was still scanning (`startupProofScan=45153/150672`) and public render had not opened
  - `build/captures/root_patch_repair_scan_phase1_default_20260602`
    - changing repair-only proof to one phase completed the proof, but the repair path deadlocked in startup-proof mode and never entered warmup repair scanning
    - code fix retained: repair proof mode now stops after the startup full sweep completes
  - `build/captures/root_patch_repair_warmup_default_20260602`
    - warmup repair scanning became active, but regular feedback persisted at roughly `1-35` accepted candidates per logged frame around frames 354-379
    - strict repair gate still did not release by frame 380
  - `build/captures/root_patch_repair_code_default_off_postopen_20260602`
    - repair blocking is back off by default
    - build passed and post-open smoke passed
    - frames 160/200/240/280 remain visually/contractually stuck: `surfaceRasterMax=1024`, `surfacePromoted=0`, `midVoxelScreenPct=19.20%`, `miss=0`, `unsafeNearMiss=0`
  - `build/captures/root_probe_promotion_gate_postopen_20260602`
    - added `PERF_SPARSE_SURFACE_PROMOTION_GATE`
    - post-open smoke passed
    - wide promotion gate was blocked at frames 120/140/160/180/200/220/240/260/280
    - `blockers=shaderMissing/runtimeClean/hiddenMiss/shaderNonReady` was usually `1/1/0/0`; when new hidden-exact feedback appeared it became `1/1/1/0`
    - `hiddenRuntimeClean=0`, `hiddenClean=0/8` throughout
    - clean rays were far too slow for startup/public promotion: examples `50/9417` at frame 180, `1922/9417` at frame 240, `4699/9417` at frame 260, then reset to `0/9417` when feedback appeared at frame 280
    - `shaderFeedbackRecent=0` at the logged 20-frame samples because shader unsafe owner-frame age was about `10`, while `VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE` defaults to `5`
  - `build/captures/root_probe_bounded_policy_default_off_20260602`
    - confirms the new bounded-policy flag does not change default behavior
    - post-open smoke passed
  - `build/captures/root_probe_bounded_promotion_postopen_20260602`
    - env:
      - `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN=1`
      - `VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE=12`
    - post-open smoke passed
    - wide promotion activated: `surfaceRasterMax=2560`, `surfacePromoted=1`, `surfaceClean=1/1`
    - surface screen coverage rose from about `47.7%` to `55.3% -> 59.0%`
    - mid voxel dropped from about `19.2%` to `12.52% -> 9.39%`
    - `miss=0`, `unsafeNearMiss=0`
    - bounded repair stayed active while hidden-exact feedback still appeared (`hiddenExactAccepted=2` at frame 160, `32` at frame 280)

- Bounded-promotion validation pass:
  - `build/captures/bounded_long_fixed_20260602`
    - same fixed camera, longer run, frames `160..720`
    - post-open smoke passed
    - `surfaceRasterMax=2560`, `surfacePromoted=1`, `surfaceClean=1/1` at every sampled frame
    - `midVoxelScreenPct` dropped from `12.6196%` at frame 160 to `4.1305%` at frame 720
    - `surfaceScreenPct` rose from `55.2018%` to `64.1976%`
    - sampled `hiddenExactAccepted` bursts were bounded and drained: `0, 0, 8, 0, 21, 0, 2, 0`
    - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
    - sampled frame body times were still heavy, roughly `46-77 ms`
  - `build/captures/bounded_stress_camera_20260602`
    - high-altitude stress smoke passed safety gates, but it did not prove wide promotion
    - `highAlt=1` path kept `surfacePromoted=0` and `surfaceRasterMax=1024`
    - shader unsafe contract non-ready stayed high in the sampled high-altitude path, so promotion suppression appears intentional for this mode
    - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
    - sampled frame body times were roughly `104-107 ms`
  - `build/captures/bounded_walk_test_20260602`
    - low-altitude movement/walk smoke passed
    - pre-contract opt-in promotion was delayed while hidden-exact/shader feedback was not recent enough, then activated at frame 480 and stayed active through frame 720
    - the later named-policy bound-64 walk probe superseded this timing by promoting at frame 140 after a bounded repair burst drained
    - `midVoxelScreenPct` went from `17.9144%` at frame 160 to `0.5201%` at frame 720
    - sampled `hiddenExactAccepted` stayed below the current demotion bound: max `43` in sampled frames
    - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
    - sampled frame body times were still expensive, roughly `90-122 ms`
  - `build/captures/bounded_basin_audit_frame160_20260602`
    - compared old baseline frame-121 basin audit with bounded fixed-camera frame 160; not an exact same-frame comparison because the frame-121 bounded one-shot did not open public render
    - exact sparse surface ownership increased from `105/512` to `265/512`
    - mid voxel ownership decreased from `278/512` to `177/512`
    - Far-SVO ownership decreased from `128/512` to `70/512`
    - `water_should_draw_before_terrain` decreased from `47/512` to `0/512`
    - visible water decreased from `44/512` to `0/512`
    - coarse fallback basin-terrain reason decreased from `9/512` to `0/512`
    - all `512/512` sampled rows classified as generated land above sea level in the bounded frame-160 audit

## Current Decision Gate

Do not force strict startup hidden-exact repair blocking as a default. It is useful as a probe, but with the current scanner and budget it can hold public render indefinitely while it keeps discovering candidates.

Current bucket for why wide exact surface promotion is blocked after the public frame opens:

| Bucket | Meaning |
|---|---|
| runtime clean counter not advancing enough | primary; `sparseHiddenExactRuntimeCleanFrames` requires full clean ray sweeps and is reset by later feedback |
| owner-frame age guard | secondary; shader unsafe feedback is often age `10` while promotion accepts max age `5` |
| promotion requires public-frame-only proof | private warmup proof is intentionally ignored |
| startup pressure mode suppresses promotion | frame/open state prevents counter advancement or promotion action |
| surface upload/backlog gate | exact surface data exists but upload/backlog blocks render promotion |
| code path reverted/missing | the promotion path never consumes the clean proof |
| proof contract too strict | primary design issue; exact zero-feedback proof is too expensive or non-convergent for startup/public frame |
| lower-LOD equivalence contract missing | mid/Far-SVO owners need an explicit proof or bound inside the sensitive band |

## Current Guarded Contract Patch

The current code now represents the bounded policy explicitly, still default-off:

- default: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`
- opt-in candidate: `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair`
- feedback age used in tests: `VENPOD_SPARSE_SURFACE_PROMOTION_FEEDBACK_MAX_AGE=12`
- hidden-exact sweep knob: `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_DEMOTE_BOUND`
- initial bounded-promotion alias: `VENPOD_SPARSE_SURFACE_PROMOTION_HIDDEN_EXACT_BOUND`
- legacy opt-in still works: `VENPOD_SPARSE_SURFACE_PROMOTION_ACCEPTS_CURRENT_HIDDEN_EXACT_CLEAN=1`

New log:

- `PERF_SPARSE_SURFACE_PROMOTION_POLICY`
  - logs policy, enabled state, promoted state, reason, shader unsafe clean status, feedback age, hidden-exact accepted/missing, hidden-exact bound, repair-active status, miss/unsafe cleanliness, high-alt exclusion, demotion, and demotion reason

The guarded contract now says:

- same-camera shader unsafe must be clean enough for promotion
- hidden-exact accepted/missing must be below the current bounded threshold
- hidden-exact repair must remain active behind the promoted raster
- public miss and unsafe-near-miss must stay zero
- promotion demotes/blocks when hidden-exact accepted/missing exceeds the bound
- high-altitude suppressed views remain explicitly excluded

Default-off validation:

- `build/captures/contract_policy_default_off_20260602`
  - `policy=strict enabled=0`
  - `surfaceRasterMax=1024`, `surfacePromoted=0`
  - `midVoxelScreenPct` stayed about `19.18-19.21%`
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`

Bound sweep:

- `build/captures/contract_policy_bounded128_fixed_20260602`
  - promoted by frame 140, stayed promoted through frame 280
  - mid voxel at frame 280: `8.7147%`
- `build/captures/contract_policy_bounded64_fixed_final_20260602`
  - promoted by frame 140, stayed promoted through frame 280
  - `surfaceScreenPct` rose `55.459% -> 59.7338%`
  - `midVoxelScreenPct` dropped `12.5476% -> 8.647%`
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
  - frame 240 body/raw: `55.24/60.38 ms`
- `build/captures/contract_policy_bounded32_fixed_20260602`
  - promoted and stayed promoted through frame 280
  - mid voxel at frame 280: `8.3256%`
- `build/captures/contract_policy_bounded16_fixed_20260602`
  - too strict in this probe
  - demoted at frame 240 when hidden-exact accepted/missing hit `32 > 16`
  - `surfaceRasterMax` collapsed back to `1024`
  - `midVoxelScreenPct` returned to `19.1792%`
  - temporal jump was visible around the demotion/re-promotion window

Movement/high-alt validation:

- `build/captures/contract_policy_bounded64_walk_20260602`
  - at frame 120, bound `64` correctly blocked/demoted a burst of `113` hidden-exact accepted/missing
  - promoted at frame 140 once the burst drained under the bound
  - stayed promoted through frame 600
  - sampled frames 360-560 had `midVoxelScreenPct=1.7945% -> 0.441%`
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
  - frame 480 body/raw: `124.36/83.19 ms`
- `build/captures/contract_policy_bounded64_highalt_20260602`
  - high-altitude stress path logged `reason=high_alt_excluded`
  - `surfaceRasterMax=1024`, `surfacePromoted=0`
  - `missScreenPct=0`, `unsafeNearMissScreenPct=0`
  - frame 240 body/raw: `93.07/106.02 ms`

Current decision:

- `64` is the best current candidate bound from this sweep.
- `32` also passed the short fixed-camera probe and may deserve a longer walk/basin pass.
- `16` is too strict for the observed fixed-camera repair bursts.
- The policy should remain default-off until a longer movement run, matched-frame basin audit, and frame-cost profile are finished.

## Performance Root Cause

Current pivot: do not do more broad visual correctness work until the renderer becomes playable. The bounded foreground contract remains default-off:

- default remains `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`
- `bounded_repair` remains an opt-in correctness candidate
- water/mode-70 work is paused
- foreground contract broadening is paused

Instrumentation found or added:

- Existing per-frame logs already cover `PERF`, `PERF_FRAME_END`, `PERF_SPARSE`, `PERF_SPARSE_CLIPMAP`, `PERF_SPARSE_SURFACE`, `PERF_SPARSE_HIDDEN_EXACT_MISS`, `PERF_SPARSE_EDIT_PUBLISH`, `PERF_CAMERA_EXPOSURE`, and `PERF_SPARSE_SURFACE_PROMOTION_POLICY`.
- Added `PERF_SPARSE_EDIT_LATENCY` for brush/edit queue and GPU-apply latency.
- Added `perf_root_cause_audit.ps1` to normalize scenario logs into `build/captures/perf_root_cause_20260602/perf_cost_table.csv` and `.md`.

Cost waterfall, selected frames:

| Scenario | Frame | Promoted | Raster | Mid % | Body ms | Raw ms | GPU ray ms | CPU update ms | Hidden repair ms | Surface extract ms | Page gen ms | Upload ms | Readback ms | Present/wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| default strict fixed capture | 240 | 0 | 1024 | 19.18 | 113.99 | 87.08 | 61.55 | 55.91 | 4.51 | 10.78 | 9.51 | 0.52 | 0.01 | 57.92 |
| bounded64 fixed capture | 240 | 1 | 2560 | 9.20 | 55.24 | 60.38 | 41.65 | 43.25 | 4.04 | 3.05 | 5.72 | 0.33 | 0.01 | 11.90 |
| bounded64 walk capture | 480 | 1 | 2560 | 0.38 | 124.36 | 83.19 | 25.73 | 92.59 | 2.46 | 5.82 | 9.58 | 1.69 | 0.03 | 31.69 |
| high-alt excluded capture | 240 | 0 | 1024 | 0.00 | 93.07 | 106.02 | 64.29 | 55.40 | 2.74 | 4.35 | 5.25 | 1.64 | 0.01 | 37.58 |
| edit brush paint capture | 240 | 0 | 1024 | 19.19 | 62.77 | 67.40 | 60.12 | 45.81 | 4.04 | 4.92 | 7.40 | 0.46 | 0.01 | 16.88 |
| non-capture strict, physics disabled | 240 | 0 | 1024 | 19.18 | 62.31 | 82.02 | 60.09 | 50.15 | 4.10 | 3.21 | 6.05 | 0.33 | 0.10 | 12.07 |

Current attribution:

- The renderer is far over the 16.67 ms frame budget even without capture. The non-capture strict no-physics run still has about `60 ms` GPU raymarch and `50 ms` CPU prep at frame 240.
- Capture/debug artifacts inflate some `body`/gap numbers, especially the strict fixed capture, but they are not the core problem. Raw frame time stays roughly comparable to non-capture, and the GPU raymarch floor remains.
- Bounded promotion is not the performance culprit in the fixed-camera comparison. The bounded64 fixed capture is faster than strict fixed in the sampled frame (`41.65 ms` GPU ray vs `61.55 ms`, `55.24/60.38 ms` body/raw vs `113.99/87.08 ms`).
- High altitude is still slow while promotion is excluded, so the cost is not caused only by wide exact surface promotion.
- Movement shifts the bottleneck toward CPU sparse streaming: the walk frame has `92.59 ms` CPU update, with large sparse request and clipmap work (`37.19 ms` request, `37.15 ms` clip, `9.58 ms` generation, `6.97 ms` trim).
- Hidden-exact repair is not the dominant cost in these samples (`2.46-4.51 ms`).
- Upload/readback are not the dominant sampled costs (`upload` under about `1.7 ms`; feedback/readback lower-bound near zero in these frames).
- Surface extraction/staging is secondary but nontrivial: extraction reaches `10.78 ms` in strict fixed and surface staging reaches `11.51 ms` in the walk frame.

Edit/build latency:

- The brush paint path now logs `PERF_SPARSE_EDIT_LATENCY`.
- Successful edit feedback applied after `3` frames:
  - frame `120` queued, frame `123` GPU-applied, dirty queued, and `4` edited publishes completed
  - frame `165` queued, frame `168` GPU-applied
  - frame `210` queued, frame `213` GPU-applied and `2` edited publishes completed
- No edit fallback, missing-resident hints, or overflow occurred in the successful edit samples.
- Brush work is not the main spike in the sampled edit run. Post-edit raw spikes around `105-127 ms` have `brush=0` and are dominated by sparse request/generation plus the persistent `~61 ms` GPU raymarch floor.
- The edit smoke still failed its own coverage gate by exit frame `460`: `cases=3/4`, `caseQueued=3/3/3/0`, `pathCells=0`. Treat it as a partial latency probe, not a correctness pass.
- First visible correct edit frame is still not proven by an automated material-diff check; the new latency log leaves `firstVisibleFrame=0` until that check is implemented.

Current performance root:

The immediate 60 fps blocker is twofold:

1. A GPU raymarch floor of roughly `60-64 ms` in fixed/default/high-alt/edit samples, which alone exceeds the entire frame budget.
2. CPU sparse streaming/update work that can reach `90+ ms` during movement, mostly sparse request/clipmap/generation/trim work.

Recommended next optimization patch:

Start with the GPU raymarch floor, because it alone prevents 60 fps even when CPU work is moderate. Add a narrow raymarch-cost reduction pass that makes the background ray pass cheaper for pixels not needing full traversal: lower/partition the fullscreen background ray workload, preserve exact surface ownership, and log before/after `gpuRayMs`, `rayScale`, `rayBudget`, ownership percentages, miss, and unsafe. Then separately tune the movement CPU sparse request/clipmap path.

## GPU Raymarch Floor Root Cause

Audit: `gpu_raymarch_floor_ablation_20260602`

Artifacts:

- `build/captures/gpu_raymarch_floor_ablation_20260602/raymarch_floor_summary.csv`
- `build/captures/gpu_raymarch_floor_ablation_20260602/raymarch_floor_table.md`

New default-neutral knobs:

- `VENPOD_RAYMARCH_RENDER_SCALE`, default `1.0`
- `VENPOD_RENDER_QUALITY=playable`, opt-in alias for render scale `0.5` unless an explicit render scale is set
- `VENPOD_RAYMARCH_MAX_STEPS_SCALE`, default `1.0`
- `VENPOD_RAYMARCH_MAX_DISTANCE_SCALE`, default `1.0`

Existing knobs used:

- `VENPOD_RAYMARCH_MAX_DISTANCE`
- `VENPOD_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_DISTANCE`
- `VENPOD_SPARSE_RAYMARCH_MAX_STEPS`
- `VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT`
- `VENPOD_ENABLE_FAR_SVO`
- `VENPOD_SPARSE_MID_VOXEL_RENDER`
- `VENPOD_SPARSE_MID_CLIPMAP`

Resolution sweep, strict fixed camera at frame `240`:

| Scale | Output | GPU ray ms | Speedup | CPU update ms | Miss % | Unsafe % |
|---:|---:|---:|---:|---:|---:|---:|
| `1.00` | `1920x1080` | `61.92` | `1.00x` | `44.65` | `0` | `0` |
| `0.75` | `1440x810` | `41.58` | `1.49x` | `63.37` | `0` | `0` |
| `0.50` | `960x540` | `28.38` | `2.18x` | `65.78` | `0` | `0` |
| `0.33` | `634x356` | `28.23` | `2.19x` | `68.84` | `0` | `0` |

The renderer is pixel-count sensitive at native resolution, but not close to fully pixel-linear. Half resolution removes about `33.5 ms` of GPU ray time, then the pass hits a residual floor around `28 ms`.

Max-step sweep, strict fixed camera at native resolution:

| Step scale | Ray budget steps | GPU ray ms | Speedup | Miss % | Unsafe % |
|---:|---:|---:|---:|---:|---:|
| `1.00` | `73` | `61.19` | `1.00x` | `0` | `0` |
| `0.75` | `55` | `60.64` | `1.01x` | `0` | `0` |
| `0.50` | `36` | `61.09` | `1.00x` | `0` | `0` |
| `0.25` | `18` | `60.00` | `1.02x` | `0` | `0` |

The sampled floor is not explained by the exposed max-step budget. Lowering the budget from `73` to `18` barely moves `gpuRayMs`.

Feature attribution, strict fixed camera at native resolution:

| Diagnostic row | GPU ray ms | Delta vs strict | Interpretation |
|---|---:|---:|---|
| strict baseline | `61.19` | `0.00` | baseline |
| Far-SVO off | `29.41` | `-31.78` | Far-SVO/background path is the largest measured GPU contributor |
| mid voxel render off | `52.26` | `-8.93` | mid voxel path is secondary |
| runtime quality ceiling 50 | `60.27` | `-0.92` | current runtime quality scalar does not address the floor |

Important nuance: the final ownership percentage for `farSvo` is small in the fixed frame, but disabling the Far-SVO path still cuts the ray pass roughly in half. That points to broad per-pixel Far-SVO/background traversal/check overhead, not just the pixels ultimately classified as Far-SVO.

High-altitude check:

| Row | Output | GPU ray ms | CPU update ms | Notes |
|---|---:|---:|---:|---|
| high-alt native | `1920x1080` | `79.86` | `260.99` | promotion excluded; expensive GPU and CPU path |
| high-alt half res | `960x540` | `40.66` | `324.02` | GPU improves `1.96x`, CPU remains worse |
| high-alt half steps | `1920x1080` | `78.15` | `351.81` | step budget does not fix GPU or CPU |

Bounded promotion comparison:

- strict native fixed: `gpuRayMs=61.92`, `surfacePromoted=0`, `surfaceRasterMax=1024`
- bounded64 native fixed: `gpuRayMs=42.70`, `surfacePromoted=1`, `surfaceRasterMax=2560`
- bounded64 half-res fixed: `gpuRayMs=24.01`

Bounded promotion is still not the GPU performance culprit in this pass. It remains default-off for correctness/shippability reasons, not because it caused the ray floor.

Current GPU root:

The `~60 ms` native GPU ray floor is primarily a Far-SVO/background fullscreen raymarch cost with pixel-count sensitivity and a large residual floor. It is not caused by the bounded foreground policy, and it is not materially reduced by the exposed max-step budget. The first measured playability lever is default-off render scaling: `VENPOD_RAYMARCH_RENDER_SCALE=0.5` or `VENPOD_RENDER_QUALITY=playable`, which reduces strict fixed-camera GPU ray time from `61.92 ms` to `28.38 ms` without miss/unsafe regressions in the sampled frame. This is a lever, not a full 60 fps fix, because CPU sparse work and the residual `~28 ms` ray floor remain over budget.

Recommended next patch after this pass:

Do not default bounded promotion and do not disable Far-SVO as a correctness fix. Instead, make the Far-SVO/background ray path cheaper: run Far-SVO/background at a lower internal resolution, add tile/region early reject, or split the far/background pass from exact foreground ownership so full-resolution pixels do not all pay the Far-SVO traversal/check cost. Keep `missScreenPct=0` and `unsafeNearMissScreenPct=0` in the before/after table.

## Far-SVO/background GPU reduction pass

Audit: `far_svo_background_gpu_reduction_20260602`

Artifacts:

- `build/captures/far_svo_background_gpu_reduction_20260602/far_svo_gpu_summary.csv`
- `build/captures/far_svo_background_gpu_reduction_20260602/far_svo_gpu_table.md`

Default-off candidate added:

- `VENPOD_RAYMARCH_FAR_SVO_QUALITY_SCALE`, default `1.0`
- tested candidate value: `0.5`

This knob is C++-side only. It scales the existing `camera.farFieldQuality` value before uploading renderer constants, so the shader uses its existing Far-SVO page-step budget schedule. It does not set `VENPOD_ENABLE_FAR_SVO=0` and does not change the bounded promotion default.

Rejected instrumentation/candidate path:

- A default-off per-attempt Far-SVO/background atomic stats variant was tried and removed.
- A shader-side `defer elevated Far-SVO candidate` branch was also tried and removed.
- Those shader-source changes produced multi-minute runtime DXC compiles and/or variants that failed to reach the frame loop without manually seeding the prior known-good shader cache. Treat shader-source churn in `PS_Raymarch.hlsl` as a tooling/performance hazard until shader compile time is fixed or the raymarch shader is split.

Paired audit results at sampled frames:

| Scenario | Candidate | GPU ray ms | Delta | Speedup | Far-SVO % | Miss/unsafe | Notes |
|---|---|---:|---:|---:|---:|---:|---|
| strict native fixed | baseline | `59.96` | `0.00` | `1.00x` | `0.22` | `0/0` | strict, unpromoted |
| strict native fixed | quality `0.5` | `34.20` | `-25.76` | `1.75x` | `0.00` | `0/0` | large GPU win but low-alt Far-SVO final ownership drops to zero |
| strict half fixed | baseline | `35.86` | `0.00` | `1.00x` | `0.22` | `0/0` | half-res render scale |
| strict half fixed | quality `0.5` | `28.91` | `-6.95` | `1.24x` | `0.00` | `0/0` | smaller gain |
| bounded64 native fixed | baseline | `46.05` | `0.00` | `1.00x` | `0.09` | `0/0` | comparison only; bounded remains default-off |
| bounded64 native fixed | quality `0.5` | `29.61` | `-16.44` | `1.56x` | `0.00` | `0/0` | comparison only |
| high-alt native | baseline | `78.99` | `0.00` | `1.00x` | `0.92` | `0/0` | promotion excluded |
| high-alt native | quality `0.5` | `74.82` | `-4.17` | `1.06x` | `0.93` | `0/0` | weak GPU gain, CPU worsened |
| high-alt half | baseline | `67.23` | `0.00` | `1.00x` | `1.04` | `0/0` | CPU very high |
| high-alt half | quality `0.5` | `71.97` | `+4.74` | `0.93x` | `1.05` | `0/0` | worse |
| walk native | baseline | `0.02` | `0.00` | `1.00x` | `0.00` | `0/0` | almost all exact/surface at sampled frame |
| walk native | quality `0.5` | `2.98` | `+2.96` | `0.01x` | `0.00` | `0/0` | not useful for this sampled walk frame |

Current decision:

- The quality-scale knob is safe to keep default-off as a diagnostic/perf-budget lever because all sampled rows stayed `miss=0` and `unsafeNearMiss=0`.
- It is not yet a strong playable-mode default candidate. The strict fixed row improves materially, but the low-alt fixed rows drop final Far-SVO ownership to zero and high-alt/half/walk rows do not show a consistent win.
- Bounded promotion remains not the performance culprit: the bounded64 native comparison improved from `46.05 -> 29.61 ms` under the same quality-scale lever, and bounded remains default-off.
- The main root is still broad fullscreen background/Far-SVO cost. The next real optimization should be a lower-res far/background pass, tile/region reject, or pass split that preserves foreground clarity and does not depend on suppressing Far-SVO ownership.

## Background pass split / mask prototype

Audit: `background_pass_split_or_mask_20260602`

Artifacts:

- `build/captures/background_pass_split_or_mask_20260602/background_split_summary.csv`
- `build/captures/background_pass_split_or_mask_20260602/background_split_table.md`
- `build/captures/background_pass_split_or_mask_20260602/background_split_contact_sheet.png`

Default-off prototype added:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE`, default `0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE`, default `1.0`
- tested candidate: enable `1`, scale `0.375`
- audit-only fixed camera knob: `VENPOD_RAYMARCH_FIXED_CAMERA`, default `0`

Chosen prototype:

- lower-resolution background pass split
- exact sparse surface raster stays full-resolution on the swapchain target
- `PS_Raymarch` renders into a lower-resolution offscreen target
- a tiny `PS_BackgroundComposite` pass composites that target only where full-resolution stencil remains zero
- no `PS_Raymarch.hlsl` source churn was required for the prototype

Measured timing result:

| Scenario | Baseline GPU ray | Split GPU ray | Result |
|---|---:|---:|---|
| strict native fixed | `61.33 ms` | `32.37 ms` | `1.89x`, GPU target hit |
| strict playable fixed | n/a | `24.39 ms` | global half-res plus split; diagnostic only |
| bounded64 native fixed | n/a | `28.68 ms` | comparison only; bounded remains default-off |
| high-alt native | `79.97 ms` | `35.73 ms` | directional only; stress camera was not matched |
| walk native | n/a | `21.01 ms` | diagnostic movement sample; CPU still `110.78 ms` |

Safety counters:

- sampled split rows kept `missScreenPct=0`
- sampled split rows kept `unsafeNearMissScreenPct=0`
- Far-SVO sample ownership did not collapse to zero (`642` strict samples, `3141` high-alt samples), but split-row ownership percentages are low-resolution raymarch sample counters, not true full-resolution composite counters

Visual decision:

- The prototype is not correctness-preserving yet.
- Contact sheet shows white sampled background/sky in unstenciled regions for split rows.
- A temporary constant-color composite probe confirmed the full-resolution stencil composite executes.
- Therefore the bad output is in the lower-resolution background target content path or its SRV contents, not the stencil gate itself.
- Keep the code default-off. Do not make it playable-mode default.

## Background split content fix

Audit: `background_split_content_fix_20260602`

Artifacts:

- `build/captures/background_split_content_fix_20260602/background_split_content_summary.csv`
- `build/captures/background_split_content_fix_20260602/background_split_content_table.md`
- `build/captures/background_split_content_fix_20260602/contact_sheet.png`
- split rows include `background_pass_frame_*.bmp` lower-res target readbacks and `engine_frame_*.bmp` final composites

Root cause of the white split output:

- the renderer heap manager allocated the background-pass SRV at shader-visible descriptor index `0`
- `src/UI/ImGuiBackend.cpp` also uses descriptor index `0` for the ImGui font texture
- ImGui overwrote the descriptor, so `PS_BackgroundComposite` sampled the ImGui font/white texture instead of `BackgroundPassColor`
- reserving shader-visible descriptor `0` for ImGui during renderer initialization moved renderer-owned SRVs to index `1+`

Debug probes:

- force-color lower-res background pass now composites correctly into the full-res target
- normal lower-res target readbacks now contain plausible terrain/sky/water content instead of white/clear output
- `lowResWhiteOrClearPct=0` in all sampled split rows
- `PERF_RENDER_COMPOSITION` provides full-resolution `surfaceOwnedPixels` / `backgroundPixels`; these are used as the true foreground stencil/composite counters in the audit table

Matched validation:

| Scenario | Frame | Split | Background res | Foreground stencil % | Composite % | GPU ray ms | CPU update ms | Far-SVO % | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| strict native fixed baseline | `380` | `0` | `1920x1080` | `47.778` | `52.222` | `60.38` | `41.89` | `0.219` | `0/0` | baseline |
| strict native fixed split | `380` | `1` | `720x405` | `83.813` | `16.187` | `17.88` | `39.33` | `0.030` | `0/0` | content path fixed |
| strict playable fixed split | `380` | `1` | `360x203` | `83.763` | `16.237` | `10.62` | `39.20` | `0.031` | `0/0` | playable render scale plus split |
| bounded64 native fixed split | `380` | `1` | `720x405` | `85.938` | `14.063` | `18.75` | `40.71` | `0.023` | `0/0` | bounded remains comparison-only |
| high-alt native baseline matched | `240` | `0` | `1920x1080` | `36.442` | `63.558` | `73.76` | `164.84` | `0.293` | `0/0` | absolute-frame stress camera |
| high-alt native split matched | `240` | `1` | `720x405` | `84.404` | `15.596` | `29.86` | `64.08` | `0.043` | `0/0` | absolute-frame stress camera |
| walk native split | `480` | `1` | `720x405` | `84.593` | `15.407` | `17.86` | `81.12` | `0.127` | `0/0` | CPU sparse update still dominates |

Decision:

- the lower-res background split is now visually plausible in the sampled contact sheet and preserves `missScreenPct=0` / `unsafeNearMissScreenPct=0`
- strict native fixed GPU ray time improved `60.38 -> 17.88 ms`, exceeding the 25% target
- matched high-alt GPU ray time improved `73.76 -> 29.86 ms`
- playable fixed split reached `10.62 ms` GPU ray time, but total frame time is still not 60 fps because CPU sparse update remains about `39 ms`
- bounded promotion remains not the performance culprit; bounded64 with split is in the same GPU range as strict split and remains default-off
- keep the split default-off/default-neutral until longer movement/high-alt validation and final visual QA are done
- next recommended performance target is movement sparse CPU request/clipmap/generation/trim cost, not another broad 60 fps goal

## Movement sparse CPU update reduction pass

Audit: `movement_sparse_cpu_update_reduction_20260602`

Artifacts:

- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_summary.csv`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/cpu_sparse_table.md`
- `build/captures/movement_sparse_cpu_update_reduction_20260602/contact_sheet.png`

New CPU detail logging:

- `VENPOD_SPARSE_CPU_DETAIL=1`
- emits `PERF_SPARSE_CPU_DETAIL`
- request counters: attempts, unique, duplicates, resident/nonresident, allocated, budget skips, camera center delta, full rebuild flag
- scan counters: trim calls/records/candidates/evicted and replacement calls/records/candidates/evicted
- clipmap counters: interest/reuse/pump ms, generated height/voxel bricks, missing height/voxel bricks, cap/active state
- surface counters: extract/stage ms and extraction backlog

Optimization implemented:

- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0` by default
- when enabled, broad terrain-surface prefetch is skipped only after public ownership is already clean (`miss=0`, `unsafe=0`, voxel-terrain pressure clean)
- screen-critical terrain repair, hidden-exact repair, mid-clipmap work, Far-SVO, and promotion policy are not disabled
- `PERF_SPARSE_CPU_DETAIL` now logs `terrainPrefetch=ms/rays/budget/seen/new/cleanThrottle`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS` remains a rejected/default-off diagnostic, not the accepted optimization
- `VENPOD_SPARSE_EVICTION_PARTIAL_SORT` keeps equivalent top-K eviction selection by default and can be set to `0` for the old full-sort path

CPU classification:

- fixed and low-alt movement request prep includes a broad terrain-surface prefetch storm: hundreds of resident/readiness touches and up to `627` CPU rays even after public ownership is clean
- after that request work is reduced, movement/high-alt spikes are still dominated by synchronous mid-voxel clipmap pump/generation and surface staging bursts
- trim/replacement scans are visible during movement but were not the fixed-camera bottleneck

Validation highlights, split enabled at `0.375`:

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

- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_MAX_BRICKS=8` plus the clean prefetch throttle was rejected: walk CPU stayed high (`62.88 ms`), mid-voxel coverage fell to `0.70`, and missing interested voxel bricks rose to `2809`
- earlier unguarded high-alt cap `8/16` probes reduced CPU but produced obvious white terrain/shore regions, so blunt pump caps are not a high-alt solution

Decision:

- keep the clean terrain-surface prefetch throttle default-off; it is useful, but not enough for playable mode
- the accepted throttle reduces matched strict walk sparse CPU `70.24 -> 58.49 ms` and fixed sparse CPU `42.94 -> 21.46 ms` with `miss/unsafe=0/0`
- high-alt remains a separate CPU bottleneck dominated by clipmap pump/generation
- non-capture validation showed capture is not the CPU cause; clipmap catch-up can still dominate at `102.28 ms`
- bounded promotion remains default-off and remains not the primary performance culprit

## Overnight Playability Ladder - 2026-06-03

Audit: `overnight_playability_ladder_20260603`

Artifacts:

- `build/captures/overnight_playability_ladder_20260603/overnight_summary.csv`
- `build/captures/overnight_playability_ladder_20260603/overnight_table.md`
- `build/captures/overnight_playability_ladder_20260603/contact_sheet.png`

Mid-clipmap path summary:

- mid-clipmap interest is built in `SparseClipmapTileCache::UpdateInterest` and `UpdateVoxelInterest`
- missing height/voxel bricks are discovered by comparing current interest against resident/generated brick maps
- height/voxel bricks are generated synchronously on the main thread through `PumpGeneration`, `PumpVoxelGenerationForRing`, `GenerateTile`, and `GenerateVoxelBrick`
- current pump order is interest-score/ring ordered, with feedback/front insertion for urgent voxel bricks, then queue order
- the pump has ring/distance-style priority but not a final screen-space visibility proof
- deferred/missing voxel bricks fall back to lower ownership paths, so a blunt cap can create visible terrain/shore holes even while public miss counters stay zero
- high-alt remains dominated by the large clipmap footprint: hundreds of missing interested voxel bricks and low coverage keep the safe scheduler budget disabled

New diagnostics:

- `VENPOD_SPARSE_CPU_DETAIL=1` now also emits `PERF_SPARSE_MID_CLIPMAP_BACKLOG`
- logged fields include backlog-aware active state, effective pump budget, budget hit, backlog height/voxel counts, oldest/max backlog age, pruned stale backlog entries, visible-critical missing voxel bricks, noncritical missing voxel bricks, missing height/voxel bricks, mid coverage, and GPU/CPU frame context

Default-off scheduler candidate:

- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MS=4.0` when the candidate is enabled
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MIN_COVERAGE_PCT=95`
- `VENPOD_SPARSE_MID_CLIPMAP_PUMP_BUDGET_MAX_CRITICAL_MISSING=128`
- budget is disabled until startup/public readiness opens, during coverage emergencies, when mid coverage falls below the threshold, or when critical missing bricks exceed the guard
- backlog entries are deduped and carried while still in current interest; stale/resident entries are pruned instead of silently dropped

Behavior-preserving secondary optimization:

- `GenerateVoxelBrick` now reserves enough temporary column-cache capacity for the existing 5x5 coarse footprint sampling path
- this does not change generated terrain output and was kept because it reduces allocation churn in a hot synchronous generation path

Validation highlights, all with background split `0.375` and clean prefetch throttle enabled:

| Scenario | Frame | Candidate | CPU update ms | Request | Gen | Clip | Surface stage | Trim | Missing voxel | Backlog voxel | Max backlog age | Mid coverage | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict baseline | `240` | `0` | `24.01` | `11.77` | `11.34` | `0.90` | `4.94` | `0.00` | `0` | n/a | n/a | `100` | `11.26` | `0/0` | split plus clean throttle |
| fixed strict backlog4 | `240` | `1` | `16.05` | `7.94` | `7.34` | `0.77` | `3.31` | `0.00` | `0` | `0` | `0` | `100` | `11.61` | `0/0` | safe fixed improvement |
| walk strict baseline | `480` | `0` | `90.67` | `23.63` | `24.15` | `37.76` | `21.45` | `5.13` | `91` | n/a | n/a | `98` | `15.60` | `0/0` | movement baseline |
| walk strict backlog4 | `480` | `1` | `43.23` | `16.36` | `16.52` | `7.28` | `14.64` | `3.07` | `415` | `415` | `117` | `95` | `9.25` | `0/0` | best captured walk row after reserve |
| walk strict backlog4 long | `600` | `1` | `79.84` | `20.12` | `10.33` | `45.80` | `18.29` | `3.59` | `494` | `494` | `262` | `94` | `20.95` | `0/0` | catch-up returns; budget disabled |
| walk bounded64 baseline | `480` | `0` | `32.96` | `16.83` | `11.70` | `1.33` | `7.29` | `3.10` | `0` | n/a | n/a | `100` | `26.16` | `0/0` | comparison only |
| walk bounded64 backlog4 | `480` | `1` | `46.75` | `12.43` | `10.68` | `20.89` | `9.96` | `2.75` | `33` | `33` | `18` | `99` | `24.67` | `0/0` | not a bounded performance win |
| high-alt strict baseline | `240` | `0` | `71.12` | `11.94` | `9.24` | `49.94` | `5.63` | `0.00` | `592` | n/a | n/a | `93` | `26.97` | `0/0` | stress camera |
| high-alt strict backlog4 | `240` | `1` | `52.83` | `6.72` | `5.63` | `40.48` | `3.92` | `0.00` | `638` | `638` | `164` | `92` | `23.50` | `0/0` | budget disabled by guards |
| noncapture walk baseline | `480` | `0` | `49.63` | `15.98` | `15.88` | `14.70` | `12.70` | `3.07` | `86` | n/a | n/a | `98` | `23.73` | `0/0` | no capture |
| noncapture walk backlog4 | `480` | `1` | `51.32` | `16.15` | `15.18` | `16.69` | `12.45` | `3.30` | `451` | `451` | `113` | `95` | `26.85` | `0/0` | no capture candidate not improved |

Visual decision:

- fixed and walk strict candidate rows look comparable to their baselines in the contact sheet
- high-alt still shows the pre-existing bright shoreline/terrain artifact; the scheduler did not clearly introduce a new hole, but high-alt is not solved
- the old blunt mid pump cap remains rejected because it caused missing bricks, coverage loss, and white terrain/shore regressions

Current decision:

- keep the backlog-aware pump default-off; it is useful diagnostic architecture, not a playable-mode default
- the frame-480 walk row met the minimum target (`90.67 -> 43.23 ms`), but the frame-600 and non-capture rows prove the backlog does not drain reliably yet
- high-alt is still clipmap dominated and needs a deeper incremental scroll or async/noncritical generation design
- combined playable mode was not promoted/tested as a candidate because Phase 4 did not fully pass long-walk/high-alt/non-capture safety
- background split, clean prefetch throttle, bounded repair, and the new mid scheduler all remain default-off
- next pass should target mid-clipmap generation architecture: async/noncritical generation or incremental interest/scroll reuse, then revisit request/surface staging once clip catch-up is stable

## Mid-Clipmap Drain/ReUse Pass - 2026-06-03

Audit: `mid_clipmap_drain_and_reuse_20260603`

Artifacts:

- `build/captures/mid_clipmap_drain_and_reuse_20260603/mid_clipmap_drain_summary.csv`
- `build/captures/mid_clipmap_drain_and_reuse_20260603/mid_clipmap_drain_table.md`
- `build/captures/mid_clipmap_drain_and_reuse_20260603/contact_sheet.png`

Final code change:

- added default-off diagnostics only: `VENPOD_SPARSE_MID_CLIPMAP_DRAIN_REUSE_DIAGNOSTICS=0`
- expanded `PERF_SPARSE_MID_CLIPMAP_BACKLOG` with budget disable reason, interest churn, resident/reused interest, backlog enqueue/carry/pump/skip counts, age buckets, per-ring generation, and average/max voxel generation ms
- kept the previous backlog-aware pump default-off
- attempted an age-priority queue-order prototype, but rejected and removed it from final code after it failed to provide a clean safe validation path

Budget disable reason codes:

- `0`: budget active / no disable
- `1`: startup/public gate
- `2`: coverage below threshold
- `3`: critical missing above threshold
- `4`: visual ownership guard
- `5`: coverage emergency

Failure root classification:

- primary root: Root 5, fallback contract / streaming architecture
- supporting root: generation cost is the mechanism, but not the safe patch target by itself

Evidence:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Backlog | Max age | Critical/noncritical | Coverage | Budget reason | Generated voxel | Avg/max gen ms | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed strict public | `380` | `17.49` | `1.79` | `0.61` | `0` | `0` | `0` | `0/0` | `100` | `4` | `0` | `0/0` | fixed path is not the blocker |
| walk fixed-dt | `600` | `38.89` | `6.71` | `5.69` | `419` | `419` | `218` | `13/406` | `95` | `0` | `4` | `1.149/2.507` | deterministic replay keeps budget active; old backlog remains |
| walk realtime | `600` | `56.95` | `22.51` | `21.55` | `535` | `535` | `112` | `19/516` | `94` | `2` | `24` | `0.829/1.925` | coverage drops below guard and full catch-up returns |
| high-alt strict | `240` | `50.38` | `36.24` | `30.26` | `1858` | `1858` | `25` | `1100/758` | `80` | `2` | `24` | `1.121/1.884` | broad high-alt footprint is not safely deferrable |
| noncapture walk | `600` | `44.02` | `7.05` | `6.08` | `435` | `435` | `211` | `0/435` | `95` | `0` | `5` | `1.005/1.744` | noncapture sample did not reproduce clip spike; request/surface stage dominate |

Interpretation:

- The long-walk spike is not a simple full-rebuild/reuse bug: fixed-dt and noncapture rows can reuse the interest signature and keep clip pump around `6-7 ms` while still carrying old backlog.
- The realtime walk spike appears when coverage slips from `95` to `94`, budget reason `2`, and the system re-enters synchronous catch-up.
- High-alt is the decisive blocker: current interest is `9216` voxel bricks, `1858` are missing, mid coverage is `80`, and `1100` missing bricks are in the current critical rings.
- Only `24` voxel bricks are generated in the high-alt frame, at about `1.12 ms` average per brick, so synchronous catch-up is intrinsically too slow.
- Deferring those bricks with the current fallback is not safe: high-alt still has the older bright shoreline/terrain artifact, and previous blunt caps caused white terrain/shore holes.

Call graph:

- `main_launcher.cpp` sparse update loop
- `SparseClipmapTileCache::UpdateInterest`
- `SparseClipmapTileCache::UpdateVoxelInterest`
- `SparseClipmapTileCache::PumpGeneration`
- `SparseClipmapTileCache::PumpVoxelGenerationForRing`
- `SparseClipmapTileCache::GenerateTile`
- `SparseClipmapTileCache::GenerateVoxelBrick`
- `SparseClipmapTileCache::BuildGpuSnapshot`
- renderer mid-clipmap upload and shader ownership

Unsafe deferral point:

- Missing current-interest voxel bricks remain absent from `m_voxelSlotByCoord`.
- The shader can fall through to lower owners, but the current lower/far fallback is not proven visually valid for the high-alt/shore footprint.
- Therefore the coverage guard correctly disables the budget instead of silently dropping work.

Minimum required refactor:

- persistent prioritized mid-clipmap backlog with age and visibility metadata
- async/noncritical voxel generation queue with dedupe and bounded completed-brick upload
- a fallback-validity classification that distinguishes bricks safely carried by Far-SVO/lower LOD from bricks that must remain synchronous
- incremental interest/scroll reuse to reduce churn after the fallback contract exists
- staged GPU upload accounting so async generation does not just move the spike into upload/present

Decision:

- no new optimization was kept from this pass
- diagnostics are safe to keep default-off
- all correctness/perf candidates remain default-off: background split, clean prefetch throttle, backlog-aware pump, bounded repair
- the blunt pump cap remains rejected
- next pass should be an async/noncritical mid-clipmap generation and fallback-validity design, not more queue-order tuning

## Async Mid-Clipmap Fallback Validity Pass - 2026-06-03

Audit: `async_mid_clipmap_fallback_validity_20260603`

Artifacts:

- `build/captures/async_mid_clipmap_fallback_validity_20260603/async_mid_fallback_summary.csv`
- `build/captures/async_mid_clipmap_fallback_validity_20260603/async_mid_fallback_table.md`
- `build/captures/async_mid_clipmap_fallback_validity_20260603/contact_sheet.png`

Default-off code change:

- added `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=0`
- added `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0` as a logged/read candidate flag only; async generation was not implemented because the classifier found no safe async-eligible bricks
- added `PERF_SPARSE_MID_CLIPMAP_FALLBACK` with fallback-valid/invalid/unknown counts, high-alt counts, finer/coarser availability, rejection reasons, async placeholders, and upload placeholders
- default behavior is unchanged when the classifier is off

Fallback path findings:

- When a preferred mid-voxel brick is missing, `PS_Raymarch` first tries resident finer rings, then uses coarser parents only in restricted low-alt/ray-angle cases.
- High-alt views explicitly reject coarser mid-voxel parents and prefer Far-SVO until the mid target set is mature.
- CPU can currently prove only one safe brick-level fallback: complete finer-ring coverage for the missing brick.
- CPU cannot prove per-brick Far-SVO, water, sky, or shoreline validity from current state; those are shader/per-ray decisions.
- Coarser parent residency is observable on CPU, but it is ray- and view-dependent in shader space, so it is classified as unknown, not async-safe.
- The old blunt cap produced white terrain/shore failures because it deferred current-interest bricks whose lower owner was not explicitly valid.
- Generated mid bricks still enter the renderable path through the existing synchronous generation, dirty-slot snapshot, and mid-clipmap GPU upload path; there is no safe worker-produced payload/apply path yet.

Classifier validation:

| Scenario | Frame | CPU update ms | Clip ms | Missing voxel | Fallback valid | Fallback invalid | Fallback unknown | Async eligible | Sync required | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `13.91` | `0.81` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.42` | `0/0` | no mid debt |
| walk fixed-dt | `600` | `37.72` | `6.55` | `269` | `0` | `61` | `208` | `0` | `269` | `97` | `0` | `4.42` | `0/0` | backlog debt remains |
| walk realtime | `600` | `37.26` | `5.80` | `308` | `0` | `119` | `189` | `0` | `308` | `96` | `0` | `10.20` | `0/0` | no proven safe async subset |
| high-alt | `240` | `116.40` | `101.65` | `3354` | `0` | `2183` | `1171` | `0` | `3354` | `62` | `2` | `21.68` | `0/0` | broad high-alt footprint is fallback-blocked |
| noncapture walk | `402` | `61.47` | `1.13` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `1` | `32.84` | `0/0` | direct noncapture run reached only a usable frame-402 row |

Decision:

- Classification result: mostly fallback-invalid/unknown; no fallback-valid subset exists in the missing-debt rows.
- Async generation is blocked for this pass. Implementing it would require deferring unknown bricks, which is the same unsafe class as the rejected blunt cap.
- The high-alt row is decisive: `3354` missing current-interest voxel bricks, `2183` invalid, `1171` unknown, `0` valid.
- Staged upload accounting is logged as zero placeholders because no async completions are produced yet; the upload path still needs a worker-payload/apply design.
- Contact sheet verdict: fixed/walk rows are comparable to prior state; high-alt still shows the existing broad artifact/coverage problem. No async visual change was introduced.
- All defaults remain unchanged: background split, clean throttle, backlog-aware pump, fallback classifier, async candidate, and bounded repair are default-off.
- The rejected blunt mid pump cap and removed age-priority prototype remain rejected.

Next safe pass:

- design the fallback contract needed to mark noncritical bricks valid: per-brick/per-region owner proof for finer mid coverage, Far-SVO domain/material validity, water/shoreline safety, and public readiness state
- then add a worker-generated mid-brick payload path with dedupe, generation stamps, safe frame-boundary apply, and explicit upload/completion budgets
- do not move generation async until the classifier can produce a nonzero, trustworthy fallback-valid subset

## Mid-Clipmap Fallback Contract Ownership Pass - 2026-06-03

Audit: `mid_clipmap_fallback_contract_ownership_20260603`

Artifacts:

- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/fallback_contract_summary.csv`
- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/fallback_contract_table.md`
- `build/captures/mid_clipmap_fallback_contract_ownership_20260603/contact_sheet.png`

Default-off additions:

- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=0`
- `PERF_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT`

Fallback contract map:

- Shader order for preferred mid-voxel miss remains: resident preferred brick, resident finer rings, then coarser parents only in restricted low-alt/ray-angle cases.
- High-alt mid-voxel rays still reject coarser parents as a safety/visual guard; CPU coarser-parent residency is not enough to defer high-alt missing bricks.
- CPU-provable valid owner remains complete finer-ring coverage.
- CPU can now diagnose whether a missing mid brick is inside the Far-SVO page/domain, but that is not a full fallback proof because Far-SVO material, occupancy, water/shoreline parity, and per-ray ownership are still shader-side or metadata-missing.
- Water and sky ownership are per-ray shader decisions; they remain unknown at brick level.
- Unknown fallback remains critical and async-ineligible.

Contract diagnostic rows, all with background split `0.375`, clean prefetch throttle, backlog-aware pump, fallback classifier, and Far-SVO domain proof diagnostics enabled:

| Scenario | Frame | CPU update ms | Clip ms | Pump ms | Missing voxel | Valid | Invalid | Unknown | Far-SVO domain valid | Far-SVO material unknown | Async eligible | Coverage | Budget reason | GPU ray ms | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.88` | `1.11` | `0.47` | `0` | `0` | `0` | `0` | `0` | `0` | `0` | `100` | `4` | `11.57` | `0/0` | no debt |
| walk fixed-dt | `600` | `64.68` | `27.35` | `14.41` | `335` | `0` | `67` | `268` | `42` | `42` | `0` | `96` | `0` | `12.36` | `0/0` | domain subset remains material-unknown |
| walk realtime | `600` | `79.30` | `38.62` | `24.50` | `463` | `0` | `12` | `451` | `5` | `5` | `0` | `94` | `2` | `22.82` | `0/0` | coverage emergency |
| high-alt strict | `240` | `99.59` | `85.49` | `78.04` | `2773` | `0` | `1602` | `1171` | `433` | `433` | `0` | `69` | `2` | `30.32` | `0/0` | decisive fallback-contract blocker |
| noncapture walk | `600` | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | `12.16` | n/a | direct noncapture run exited cleanly, but did not emit fallback detail |

Decision:

- The selected proof candidate was Far-SVO per-brick domain diagnostics, not Far-SVO fallback validity.
- It reduced the unknown explanation gap but did not create a safe async subset: all Far-SVO-domain-covered missing bricks remained material/owner unknown.
- High-alt changed from the previous classifier's `3354` missing, `0` valid row to this pass's matched retry with `2773` missing, `433` Far-SVO-domain-covered, `433` material-unknown, and still `0` async-eligible.
- Async generation and async plan-only remain blocked because there is still no trustworthy fallback-valid subset.
- Visual verdict: fixed/walk captures remain comparable; high-alt still has the known bright shoreline/terrain artifact and is not fixed by this diagnostic pass.
- All defaults remain unchanged. The rejected blunt pump cap and removed age-priority queue prototype remain rejected.

Next safe pass:

- add lightweight fallback-owner feedback or CPU metadata that can prove material/occupancy/shoreline ownership for Far-SVO/coarser/water/sky fallback regions
- alternatively reduce high-alt/current-interest footprint only if the ownership contract remains explicit and no missing/white/shore holes appear
- do not implement async generation until the contract diagnostics produce a nonzero valid subset

## Autonomous Streaming Playability Engineering - 2026-06-03

Audit: `autonomous_streaming_playability_engineering_20260603`

Artifacts:

- `build/captures/autonomous_streaming_playability_engineering_20260603/summary.csv`
- `build/captures/autonomous_streaming_playability_engineering_20260603/table.md`
- `build/captures/autonomous_streaming_playability_engineering_20260603/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain
- final code state keeps no new optimization prototype from this pass

Architecture finding:

- `SparseClipmapTileCache::UpdateVoxelInterest` builds a broad resident/cache footprint, not a true visible-current-frame set.
- High-alt adds broad terrain/fan candidates across rings, so `currentInterestVoxel` stays around `9216` and can create thousands of missing current-interest bricks.
- `PumpGeneration` / `PumpVoxelGenerationForRing` still generate voxel bricks synchronously on the main thread.
- Backlog/budget guards are computed from broad interest coverage. When broad coverage drops, the guard disables the budget and forces catch-up.
- Existing render feedback only samples a narrow owner path and is not a complete visible-critical/missing-brick ownership map.
- Shader fallback remains per-ray: preferred mid brick, resident finer rings, then coarser parents only in restricted low-alt/ray-angle cases. High-alt rejects coarser parents.

Fresh rows with background split `0.375`, clean throttle, CPU detail, backlog-aware pump, fallback classifier, contract diagnostics, and Far-SVO domain proof enabled:

| Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU ray | Missing | Valid/Invalid/Unknown | Critical/Noncritical | Coverage | Budget reason | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `14.58` | `6.63` | `7.06` | `0.89` | `0.20` | `11.18` | `0` | `0/0/0` | `0/0` | `100` | `4` | fixed path is not blocked |
| walk fixed-dt | `600` | `43.44` | `22.53` | `10.64` | `6.52` | `5.39` | `12.49` | `413` | `0/0/413` | `0/413` | `95` | `0` | clip is no longer first bucket; request/surface/gen are material |
| walk realtime | `600` | `78.94` | `16.13` | `7.38` | `52.18` | `40.20` | `14.20` | `474` | `0/100/374` | `100/374` | `94` | `2` | coverage guard disables budget and clip catch-up returns |
| high-alt strict | `240` | `99.59` | `7.62` | `6.46` | `85.49` | `78.04` | `30.32` | `2773` | `0/1602/1171` | `1602/1171` | `69` | `2` | reused prior matched retry because fresh wrapper failed; fallback-contract blocked |
| noncapture direct | `600` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0.82` | n/a | n/a | n/a | n/a | n/a | clean run, but not a comparable sparse streaming sample |

Rejected prototype:

- A temporary default-off ring-based visible-critical coverage guard was tried and removed.
- Fixed-dt walk improved in isolation: CPU `43.44 -> 33.69`, clip `6.52 -> 7.24`, but missing voxel debt grew `413 -> 897`.
- Realtime walk regressed: CPU `78.94 -> 124.03`, clip `52.18 -> 91.46`, missing voxel `474 -> 1069`, parent-held visual failure `27 -> 1193`.
- Verdict: ring-level visible-critical approximation is not an ownership contract. It can hide broad debt until parent-held/visual catch-up explodes.

Decision:

- No new optimization remains in source from this pass.
- Background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair all remain default-off/default-neutral.
- Blunt pump cap remains rejected. Removed age-priority queue prototype remains rejected.
- Async remains blocked because fallback-valid subset is still zero in debt rows.
- A real current-interest split needs sampled/owner-aware visible-critical feedback or metadata, not ring coverage.

Next implementation slice:

- Add lightweight sampled missing-preferred-mid feedback that records which visible rays sampled a missing preferred mid brick and which owner covered the pixel: finer mid, coarser mid, Far-SVO, water, sky, miss/unsafe, or unknown/mixed.
- Keep feedback bounded by tiles/hash buckets/reservoir sampling to avoid full-frame atomics and shader compile churn.
- Use that feedback to split `currentInterest` into visible-critical and cache/prefetch interest. Unknown sampled missing stays critical; unsampled cache footprint can become prefetch/backlog.
- Only after that split is proven safe, add async/prefetch generation and staged upload/apply budgets.

## Sampled Missing Mid Feedback and Visible-Critical Split - 2026-06-03

Audit: `sampled_missing_mid_feedback_and_visible_critical_split_20260603`

Artifacts:

- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/summary.csv`
- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/table.md`
- `build/captures/sampled_missing_mid_feedback_and_visible_critical_split_20260603/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- `git diff --check` on touched source passed except existing LF-to-CRLF warnings
- known `rayDir` shadow warnings remain

Implementation:

- Added `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=0` as a default-off diagnostic.
- Shader hash-bucket feedback was attempted first, but the feedback-enabled capture hit the runtime shader compile/churn stop condition: no log/capture output after a 240 second timeout, with the process still burning CPU. The shader/ABI changes were removed.
- Final implementation is a bounded CPU projected-bounds approximation. It enumerates missing voxel interest, projects missing brick bounds into the current camera view, and logs whether missing bricks are projected-visible or projected-offscreen.
- Added `SparseClipmapTileCache::CollectMissingVoxelInterest`.
- Added `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK` and bounded `PERF_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET` logs.
- Default-off tuning knobs: `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_CPU_MAX_COORDS`, `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_SCREEN_PAD`, and `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_BUCKET_LOG_MAX`.

Feedback rows, all with background split `0.375`, clean throttle, CPU detail, backlog-aware pump, fallback classifier, contract diagnostics, Far-SVO domain proof, and missing-sample feedback enabled:

| Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU ray | Missing | Valid/Invalid/Unknown | Far-SVO domain/material unknown | Sampled approx | Unsampled approx | Sampled % | Coverage | Reason | Miss/unsafe | Feedback ms | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed public | `380` | `27.67` | `10.77` | `15.97` | `0.92` | `0.20` | `13.34` | `0` | `0/0/0` | `0/0` | `0` | `0` | `0` | `100` | `4` | `0/0` | `0.41` | no missing debt |
| walk fixed-dt | `600` | `73.21` | `22.82` | `10.55` | `34.24` | `14.89` | `15.68` | `290` | `0/74/216` | `44/44` | `220` | `70` | `75` | `96` | `0` | `0/0` | `0.52` | mostly projected-visible |
| walk realtime | `600` | `71.12` | `23.00` | `9.76` | `34.51` | `32.40` | `9.10` | `551` | `0/75/476` | `9/9` | `502` | `49` | `91` | `93` | `2` | `0/0` | `0.53` | coverage emergency, mostly projected-visible |
| high-alt late | `400` | `57.58` | `8.20` | `6.98` | `42.39` | `35.12` | `21.16` | `467` | `0/257/210` | `40/40` | `14` | `453` | `2` | `94` | `2` | `0/0` | `0.62` | broad cache debt; sampled subset still unknown/invalid |
| noncapture walk | `600` | `122.01` | `67.22` | `34.92` | `12.17` | n/a | `50.90` | `449` | `0/18/431` | `4/4` | `379` | `70` | `84` | `95` | `0` | `0/0` | `0.77` | request/gen/surface dominate; missing still mostly projected-visible |

Decision:

- The previous ring-based visible-critical heuristic was too crude. It deferred/accumulated work without knowing whether missing bricks were sampled, and realtime validation already showed parent-held visual failure explosion.
- Walk fixed-dt, realtime walk, and noncapture walk are mostly projected-visible by the CPU approximation. Their missing bricks also remain fallback-invalid or fallback-unknown, so a visible-critical deferral would be unsafe for the primary movement blocker.
- Late high-alt proves a real over-broad current-interest case: only `2-6%` of missing debt projected into view in the late stress-camera rows. But the sampled subset is still unknown/invalid, and the current feedback is post-hoc diagnostics rather than a pre-pump state machine.
- `VISIBLE_CRITICAL_INTEREST_V2` was not implemented because a safe version must run before pump budgeting and must prioritize sampled/likely-visible missing bricks before prefetch. Wiring the post-hoc approximation directly into the guard would risk repeating the rejected ring heuristic.
- Async remains blocked because `fallbackValid=0` and no valid owner class was proven for sampled missing bricks.
- All defaults remain unchanged. Background split, clean throttle, backlog-aware pump, fallback classifier, Far-SVO proof, async, and bounded repair remain default-off/default-neutral.

Next implementation slice:

- Build a real pre-pump `visibleCriticalInterest` state, not another ring heuristic.
- Use projected/sample feedback before pump budget selection to split visible/sampled critical missing from cache/prefetch missing.
- Make the pump prioritize visible-critical missing bricks before prefetch.
- For walk/realtime, focus on fallback ownership proof or reducing actual sampled missing debt; the current feedback says most debt there is visible, not merely cache.
- For high-alt, the visible-critical split is promising, but must keep sampled unknown/invalid bricks critical and must not use unsampled cache debt to trigger synchronous catch-up.

## Streaming Playability Campaign Until Valid Candidate - 2026-06-04

Audit: `streaming_playability_campaign_until_valid_candidate_20260604`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Implementation:

- Added `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=0`, default-off.
- Added `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP`.
- Added a high-alt pre-pump visible-critical/cache split using the CPU projected missing-mid feedback path. It only changes pump prioritization and coverage/budget reasoning for high-alt views with missing mid-voxel debt; sampled/projected-visible unknown or invalid bricks remain critical, and cache/prefetch bricks remain tracked.
- Added `SparseClipmapTileCache::PrioritizeVoxelGenerationCoords`.
- Added `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=0`, default-off experimental/diagnostic only. It reuses terrain column samples across a voxel pump batch and logs `columnCache=...` in the mid-clipmap backlog line.

Main rows, all using background split `0.375`, clean throttle, backlog-aware pump, fallback diagnostics, Far-SVO proof, and missing-sample feedback unless noted:

| Scenario | Frame | CPU ms | Req | Gen | Clip | GPU ray | Missing | Sampled/unsampled | Coverage | Reason | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed baseline | `380` | `10.84` | `5.93` | `4.15` | `0.76` | `12.02` | `0` | `0/0` | `100` | `4` | `0/0` | fixed is not the blocker |
| walk fixed-dt baseline | `600` | `41.08` | `14.21` | `6.21` | `17.51` | `11.32` | `422` | `357/65` | `95` | `0` | `0/0` | movement debt mostly visible |
| walk realtime baseline | `600` | `31.14` | `13.13` | `8.58` | `6.61` | `14.41` | `212` | `196/16` | `97` | `0` | `0/0` | sampled-visible movement debt |
| high-alt baseline | `400` | `44.95` | `9.10` | `5.41` | `30.42` | `21.39` | `2384` | `194/2190` | `73` | `2` | `0/0` | broad cache debt forces catch-up |
| high-alt prepump split | `400` | `25.23` | `7.07` | `5.53` | `12.62` | `16.31` | `2074` | `66/2008` | `99` visible / `77` cache | `0` | `0/0` | accepted default-off high-alt win |
| realtime + shared column cache | `600` | `48.84` | `14.13` | `8.20` | `23.54` | `14.19` | `122` | `114/8` | `98` | `0` | `0/0` | rejected as unreliable for realtime |
| realtime + request reuse env | `600` | `42.74` | `13.89` | `8.36` | `17.58` | `11.54` | `154` | `138/16` | `98` | `3` | `0/0` | rejected; request did not drop enough |
| realtime + surface upload interval | `600` | `35.69` | `15.35` | `9.48` | `7.43` | `9.64` | `373` | `321/52` | `95` | `0` | `0/0` | rejected; moved work into raw/stage spikes |

Combined playable stack tested:

```text
VENPOD_RENDER_QUALITY=playable
VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1
VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375
VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1
VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=1
VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1
```

Playable rows:

| Scenario | Frame | CPU ms | Req | Gen | Clip | GPU ray | Raw/body | Missing | Sampled/unsampled | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed playable | `380` | `12.06` | `6.56` | `4.72` | `0.78` | `8.45` | `31.47/n/a` | `0` | `0/0` | GPU is good; capture total still over 16.67 ms |
| realtime walk playable | `600` | `67.72` | `13.47` | `9.02` | `42.16` | `6.24` | `92.78/100.04` | `424` | `349/75` | not playable; clip/pump debt returned |
| high-alt playable | `400` | `35.83` | `8.97` | `6.15` | `20.71` | `11.14` | `69.13/n/a` | `2897` | `119/2778` | better GPU, still CPU/cache debt |

Visual/contact-sheet verdict:

- `contact_sheet.png` shows no new obvious holes/white-terrain regression from the accepted high-alt prepump split.
- The known high-alt bright shoreline/terrain artifact remains.
- Rejected walk experiments did not produce a candidate-quality visual/perf result.

Noncapture status:

- A direct runtime probe with `VENPOD_LOG_FILE` forced from `build\bin` detached and left `VENPOD.exe` processes without producing the requested forced log. Those probes were killed.
- The current campaign therefore does not have a clean new noncapture row. Do not treat capture rows as a validated 60 FPS claim.

Decision:

- Completion state is a strong default-off candidate plus exact blockers, not a 60 FPS candidate.
- The high-alt over-broad cache/current-interest problem has a safe default-off improvement.
- Walk/realtime debt remains mostly projected-visible and fallback-unknown/invalid; visible-critical deferral or async would weaken ownership there.
- Shared column cache, screen-critical request reuse, and surface upload interval experiments were rejected for final candidate use.
- Background split, clean throttle, backlog-aware pump, visible-critical prepump, shared column cache, fallback diagnostics/proofs, async, and bounded repair remain default-off/default-neutral.
- Blunt pump cap, age-priority queue ordering, and the failed ring-only heuristic remain rejected.

Remaining top blockers:

- realtime walk: mid-clipmap clip/pump spikes on sampled-visible unknown/invalid debt, then request/gen around `13-22 ms`/`7-10 ms`
- high-alt: broad cache/current-interest debt is improved by prepump split but still leaves cache backlog, sampled unknowns, and known shoreline artifact
- surface staging: dirty surface metadata staging still performs full metadata work for small dirty/removed sets; simple upload interval moves cost into raw/stage spikes
- noncapture validation: `perf_noncapture_smoke.ps1` now provides a reliable log-only mode; first candidate-stack rows are fixed `54.67 ms` raw, walk realtime `246.81 ms` raw, and high-alt `91.11 ms` raw

Next implementation slice:

- Keep `VISIBLE_CRITICAL_PREPUMP` as a default-off high-alt candidate.
- For movement, do not defer sampled unknown bricks. The next safe CPU work is either a real sampled ownership proof for movement debt, or a non-ownership optimization in request/surface staging.
- Surface/request/generation is now the clearest movement CPU refactor: the first noncapture walk row spent request/gen/clip/trim `49.52/26.35/7.94/6.76 ms` plus surface extract/stage `21.86/55.24 ms`.
- Use `perf_noncapture_smoke.ps1` for every playable-candidate claim; do not rely on capture-only timing.

## Next Decision Work

Do not return to broad root-cause hunting. The next work is performance-first:

- keep the lower-res background split default-off and run longer validation only if needed for playable-mode candidacy
- continue CPU work with a pre-pump visible-critical/cache-interest split backed by sampled/projected missing-mid evidence
- keep async/noncritical generation blocked until fallback validity produces a nonzero safe subset
- keep the clean prefetch throttle as a default-off candidate, but do not use it as the playable-mode CPU fix by itself
- reduce surface staging/generation bursts after the mid-clipmap pump backlog is handled
- keep bounded foreground promotion default-off until performance headroom exists
- only resume matched basin/correctness validation after the renderer is playable

Likely next design choice:

- If the foreground contract must be exact: make the exact coverage proof camera-footprint based and bounded, not a global/perpetual hidden-exact zero-feedback gate.
- If lower LOD may carry foreground: add an explicit equivalence/bounded-error contract for mid/Far-SVO ownership inside the sensitive band, then allow promotion/opening based on that contract.
- If a small number of exact misses is acceptable behind lower LOD: make that bounded repair policy explicit and measurable, instead of relying on `miss=0` or hidden clean counters that do not cover the full visible contract.

## Water Debug Status

Mode 70 would be useful for the smaller water/terrain boundary issue, but the latest checkpoint says a quick mode-70 inclusion caused a slow/hung shader run and was reverted. Treat mode 70 as debug-tooling debt until foreground handoff is bucketed.

## Streaming Playability Campaign Continuation - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_table.md`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Continuation result:

- The campaign did not reach a validated 60 FPS candidate.
- Accepted direction remains default-off: lower-res background split, clean prefetch throttle, backlog-aware pump, sampled missing-mid feedback, and high-alt `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP`.
- Rejected and removed from runtime in this continuation:
  - general generation cap
  - screen-critical reuse forward tolerance
  - surface timed skip-sort
- CPU parent-held feedback remains only a default-off diagnostic; it did not fix walk f600.
- Timed surface extraction avoids an unused global sort before the per-class extraction pass; per-class extraction ordering is unchanged.

Latest matrix:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | GPU | Missing | Sampled/unsampled | Coverage | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `103.13` | `43.44` | `19.55` | `13.98` | `9.90` | `7.50` | `13.11` | `28` | `24/4` | `99` | `0/0` | not 60 |
| walk realtime | `600` | `144.49` | `115.15` | `26.16` | `23.74` | `65.22` | `47.49` | `20.94` | `445` | `376/69` | `94` | `0/0` | sampled-visible debt, still blocked |
| high-alt | `378` | `45.14` | `23.26` | `5.07` | `5.48` | `12.70` | `4.96` | `10.71` | `1833` | `33/1800` | `99 visible / 80 cache` | `0/0` | prepump split helps |

Decision:

- High-alt is partially solved by splitting visible-critical from cache debt.
- Walk/realtime is not safely deferrable: most missing mid debt is sampled/projected-visible and fallback-unknown/invalid.
- The next real fix is an ownership-aware streaming pipeline, not more queue caps:
  - visible-critical sampled bricks stay synchronous/guarded
  - cache/prefetch bricks move to async generation
  - worker-generated CPU payloads apply on frame boundaries
  - upload/surface extraction have separate budgets
  - owner metadata/feedback must turn unknown movement fallback into explicit valid/invalid before async can handle it
### Streaming Playability Real Fix Campaign - 2026-06-04 Active State

Build/test remain green after the latest accepted cleanup:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Accepted code cleanup:

- `src/Simulation/SparseVoxelWorld.cpp`
- `src/Simulation/SparseVoxelWorld.h`
- `src/main_launcher.cpp`
- Surface extraction queue hygiene now prunes stale queues when no pending surface payload exists and skips extraction attempts when the world reports zero pending surface payloads. This is behavior-preserving cleanup, not a playable fix.

Latest current matrix:

- `build/captures/current_after_surface_queue_cleanup_20260604`
- campaign copies:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_table.md`

Key rows:

- fixed frame `380`: raw `40.06 ms`, CPU `9.07 ms`, GPU ray `7.22 ms`, miss/unsafe `0/0`; frame `384` still had post-open prepublish surface extraction `29.62 ms`
- walk realtime frame `600`: raw `105.52 ms`, CPU `65.19 ms`, request `30.04 ms`, gen `26.18 ms`, clip `8.92 ms`, surface extract `15.04 ms`, GPU ray `36.16 ms`, miss/unsafe `0/0`
- high-alt frame `400`: raw `69.39 ms`, CPU `44.03 ms`, request `12.73 ms`, gen `11.19 ms`, clip `20.10 ms`, GPU ray `16.61 ms`, visible-critical coverage `99`, cache coverage `78`, miss/unsafe `0/0`

Rejected probes:

- direct footprint columns: rejected after full-matrix regression
- shared column cache: rejected
- general surface extraction budget `4`: rejected
- post-open prepublish max `4 ms`: rejected; shifted debt into walk/high-alt catch-up
- hidden exact post-open surface budget `16`: rejected; shifted debt into walk/high-alt catch-up
- bounded64 current stack: rejected as a performance answer; walk demoted/blocked at frame `600`

Current decision:

- No validated playable candidate exists.
- The next real architecture slice is default-off async exact sparse page generation, not another cap:
  - extract a pure worker-safe `GeneratedSparseBrick` builder with local terrain column cache;
  - protect edits with a revision/snapshot guard or discard async completions while edits are active;
  - apply completed bricks on the main thread through existing generated/upload/surface queues;
  - log async queue/apply/upload/surface backlog and validate fixed/walk/high-alt/noncapture.

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

Decision:

- all-class async exact generation is rejected: walk realtime regressed from `75.77` to `100.37 ms` CPU because async visible/current exact pages created readiness debt that mid-clipmap catch-up repaid.
- speculative-only async exact generation is safe but ineffective in sampled rows: it queued/applied `0` async bricks.
- direct exact generation is rejected: walk realtime stayed worse than baseline (`82.33 ms` CPU, `46.30 ms` clip).
- no validated playable candidate exists.
- the remaining fix is ownership-aware streaming state, not another exact-generation tweak:
  - sampled visible fallback-unknown work stays synchronous/guarded;
  - cache/prefetch or CPU-proved fallback-valid work can move async;
  - apply/upload/surface extraction need independent budgets and backlog age accounting.

### Active Campaign Control - 2026-06-04

The active `/goal` is `streaming_playability_real_fix_campaign_20260604`; `handoff.md` is the durable continuation file and must be updated after each meaningful branch.

Do not stop after one diagnostic, one failed heuristic, or one small partial. Completion requires either a validated noncapture playable candidate, a strong candidate with exact remaining bottlenecks, or a hard architecture/tool blocker proven after multiple safe branches.

Latest state:

- `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` is a default-off high-alt/cache partial. It is safe partial evidence, not a playable candidate.
- High-alt over-broad cache debt has a credible visible-critical/cache direction.
- Walk/realtime remains sampled/projected-visible and fallback-unknown/invalid; it cannot be safely deferred or async-generated under the current ownership contract.
- Rejected branches remain rejected: blunt pump cap, age-priority, ring-only visible-critical, broad exact async, direct exact generation, global generation cap, surface caps that shift debt.
- Next implementation must build the ownership-aware streaming state machine with separate visible-critical/cache/prefetch lanes, worker payload generation only for safe lanes, frame-boundary apply/upload/surface budgets, and owner proof metadata for movement debt.

### Visible Priority Pump Branch - 2026-06-04

Added `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP=0` default-off.

- It prioritizes CPU-projected-visible missing mid-voxel coords before pump, without changing budgets/guards or treating unknown fallback as safe.
- Build and ctest passed.
- Walk: CPU/clip/pump improved from `58.73/28.07/25.79` to `48.62/21.35/19.30`, but raw worsened from `69.36` to `79.40`.
- Walk with priority + parallel mid pump improved raw to `64.18` and CPU to `43.18`.
- Walk with priority + parallel mid + exact parallel reached raw `59.55`, CPU `44.41`, GPU `10.14`, still far from 60 FPS.
- The parallel stack regressed high-alt to raw `75.05`, CPU `41.20`; it is rejected as a global candidate.

Decision: keep visible-priority pump as default-off ownership-lane evidence only. It is not a playable candidate. The next slice should move lane classification into `SparseClipmapTileCache` and track persistent visible/cache backlog instead of doing ad hoc queue surgery in the launcher.

### Streaming Playability Candidate Stack - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test passed after reverting the rejected priority-side-effect patch. `perf_noncapture_smoke.ps1` now supports `-WalkFixedDtMs` with default `0`.

Accepted default-off stack:

- playable render scale + background split `0.375`
- clean terrain prefetch throttle
- backlog-aware mid pump
- incremental pressure trim, scan budget `8192`
- parallel visible mid-voxel pump, `4` workers

Fresh baseline versus accepted stack:

| Scenario | Baseline raw/CPU | Accepted raw/CPU | Accepted req/gen/clip/pump/GPU | Missing/sampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|
| fixed | `46.41/14.23` | `27.45/23.00` | `4.88/17.30/0.81/0.00/8.08` | `0/0` | `0/0` |
| walk realtime | `152.99/82.44` | `62.73/43.05` | `16.06/16.83/10.14/7.60/16.56` | `444/284` | `0/0` |
| high-alt | `93.16/51.47` | `51.50/26.41` | `4.67/7.08/14.65/6.39/11.79` | `1451/0` | `0/0` |

Rejected in this continuation: streaming lane scheduler, priority-side-effect removal patch, trim+parallel surface, trim+cache-only defer as global stack, worker column cache, exact parallel, surface sort cache, hidden exact tracked scan budget, surface-ready publish queue, terrain-critical inline surface defer.

Decision: Completion B state, not 60 FPS. No defaults changed. Remaining blockers are distributed request/generation/clip/surface/GPU costs plus sampled fallback-unknown walk debt. Next implementation is persistent streaming lanes in the clipmap cache, request-prep/touch caching by source/lane, and visible-critical versus cache/background surface budgets.

### Terrain-Critical Parallel Generation Retained Partial - 2026-06-04

Retained default-off candidate:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`

This branch parallelizes same-frame terrain-critical exact sparse brick payload generation, then applies/upload-publishes on the main thread through the existing safe path. It does not defer visible/current ownership work.

Artifacts:

- `build/captures/candidate_terrain_critical_parallel_generation_min16_20260604`
- `build/captures/candidate_terrain_critical_parallel_generation_min16_diagnostics_20260604`

Full-diagnostic retained rows:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.85` | `19.36` | `3.67` | `14.98` | `0.71` | `0.00` | `18.00` | `6.31` | `0/0` | `0/0/0` | `100` | `0/0` |
| walk realtime | `600` | `50.55` | `50.79` | `15.27` | `14.97` | `20.53` | `9.16` | `11.21` | `19.30` | `455/254` | `0/130/325` | `94` | `0/0` |
| high-alt | `360` | `47.65` | `26.71` | `5.72` | `6.55` | `14.43` | `4.05` | `3.95` | `9.96` | `1759/0` | `0/1238/521` | `100` | `0/0` |

Decision:

- retain as a default-off partial, not a playable candidate;
- resident-touch cache and surface visible-lane pump were rejected and removed;
- background scale `0.25` and mid-interest interval `2` are scenario-specific evidence, not accepted global stack changes;
- no defaults changed.

Current bottleneck:

- representative walk remains around `50 ms` raw with distributed request, generation, clip/pump, surface, and GPU costs;
- sampled missing mid debt remains fallback-invalid/unknown and cannot be deferred;
- the next real fix is persistent ownership lanes plus lane-specific generation/apply/upload/surface budgets, not more caps.

### Streaming Lane Queue Priority Branch - 2026-06-04

New retained default-off branch:

- `VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY=0`
- exact sparse generation/upload/surface queue scores can optionally prioritize existing `SparseStreamingLane` values within the same residency class.
- no default behavior change; no work is dropped; no fallback/coverage guard is weakened.

Build/test passed:

- `.\build.ps1 -Config Release`
- `ctest --test-dir build --output-on-failure -C Release` (`1/1`)

Artifacts:

- `build/captures/candidate_streaming_lane_queue_priority_diagnostics_20260604`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png` (log-only)

Retained stack rows:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` | `447/248` | `95` | `0/0` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` | `1755/0` | `100` | `0/0` |

Decision:

- keep lane queue priority default-off as a safe partial;
- it improves walk CPU versus the prior retained terrain-critical row (`50.79 -> 39.44 ms`);
- it is not a playable/60 FPS candidate;
- no defaults were promoted.

Rejected after this branch: parallel surface extraction, parallel exact generation, mid worker column cache, direct footprint columns, mid pump `8` workers, background scale `0.25`, generation budget `8`, and buried-solid surface fast path.

Current remaining blocker is distributed streaming architecture: fixed generation/surface bursts, walk request/generation/clip/surface/GPU plus sampled fallback-invalid/unknown mid debt, and high-alt broad cache backlog.

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

- fixed: raw/CPU `22.24/22.55`, request/gen/clip/surface/GPU `4.18/17.52/0.84/19.53/6.03`
- walk realtime: raw/CPU `59.00/35.16`, request/gen/clip/surface/GPU `14.15/14.72/6.27/11.54/11.80`, backlog `435`
- high-alt: raw/CPU `49.12/27.55`, request/gen/clip/surface/GPU `6.57/6.71/14.26/4.23/10.09`, backlog `1753`

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

Decision:

- A default-off persistent terrain column cache was implemented, tested with capped variants, then removed.
- It did not become a retained candidate because fixed/high-alt regressed or became unstable, and the best walk row still stayed far from playable.
- No `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_*` knob remains.
- All defaults remain unchanged.

Key comparison:

| Row | Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| retained baseline | fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | reference |
| retained baseline | walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | reference |
| retained baseline | high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | reference |
| terrain cache `65536` | fixed | `384` | `24.50` | `23.69` | `3.91` | `18.96` | `0.81` | `0.00` | `25.66/n/a` | `7.20` | rejected |
| terrain cache `65536` | walk realtime | `600` | `50.73` | `47.94` | `14.07` | `16.81` | `17.04` | `4.45` | `12.59/3.02` | `10.68` | rejected partial |
| terrain cache `65536` | high-alt | `395` | `43.95` | `24.93` | `3.28` | `6.49` | `15.16` | `4.45` | n/a | `10.28` | rejected |

Next branch:

- Inspect request planning/cache behavior. Current rows repeatedly show material request cost and `centerDelta=0/0/0 fullRebuild=1`; determine whether request sets are rebuilt despite unchanged request keys.
- If true, implement behavior-preserving request reuse keyed by request center, forward cone, budgets, and policy flags.

### Campaign Continuation - Request-Side Existing Flags Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_side_existing_flags_rejected_20260605.md`

Existing default-off flags tested:

- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=1`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=1`

Decision:

- Neither flag is retained as a playable candidate stack member.
- Pressure-trim guard reduces request time in some rows but shifts/uncovers clip-pump debt; no-heavy walk became raw/CPU `74.33/52.83` with clip/pump `23.48/21.22`.
- Hidden tracked scan budgeting did not improve the representative walk row (`74.03/47.07`) and still hit scan budgets.
- No code changed for these probes; all defaults remain unchanged.

Current best retained reference remains lane-priority default-off partial:

- fixed frame `384`: raw/CPU `19.88/19.79`
- walk realtime frame `600`: raw/CPU `53.71/39.44`
- high-alt frame `360`: raw/CPU `47.78/26.47`

Current decision:

- Stop chasing isolated request micro-flags.
- The next real implementation direction is the ownership-aware streaming state-machine slice: persistent visible-critical/cache/prefetch lanes, separate generation/apply/upload/surface budgets by lane, and async only for cache/prefetch or CPU-proved fallback-valid work.

### Campaign Continuation - Generation Lane Budget Rejected - 2026-06-05

The default-off generation lane budget prototype was implemented, measured, rejected, and removed.

- Build after removal passed.
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.
- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generation_lane_budget_rejected_20260605.md`

Rows:

- fixed frame `360`: raw/CPU `15.59/21.10`, gen `16.20`, surface extract `18.96`, miss/unsafe `0/0`
- walk realtime frame `600`: raw/CPU `65.15/63.94`, clip/pump `36.17/23.78`, missing `556`, coverage `93`, miss/unsafe `0/0`
- high-alt frame `360`: raw/CPU `44.25/28.32`, clip/pump `16.38/5.34`, missing `2183`, coverage `99`, miss/unsafe `0/0`

Decision:

- no generation-lane-budget env remains;
- the prototype is rejected because representative walk regressed and it did not produce a stable candidate;
- next measured branch is existing parallel exact generation, then request or surface buckets if that fails.

### Campaign Continuation - Footprint Signature Reuse Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/footprint_signature_reuse_rejected_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/footprint_signature_reuse_vsync0_20260605`

Decision:

- default-off footprint-signature reuse was implemented, tested, rejected, and removed.
- no `VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_SIGNATURE_*` env remains.
- post-removal Release build and ctest passed.
- all defaults remain unchanged.

Key comparison:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.88` | `30.04` | `22.96` | `27.63` | `0.84` | `0.88` | `0.00` | `0.00` | `0` | `0` | `100` | `100` |
| walk realtime | `600` | `52.16` | `84.14` | `45.85` | `68.78` | `17.39` | `40.93` | `5.44` | `28.00` | `447` | `460` | `95` | `94` |
| high-alt | `360` | `50.05` | `48.71` | `29.79` | `31.91` | `16.39` | `16.87` | `5.36` | `5.65` | `2228` | `2196` | `99` | `99` |

Root decision:

- signature/interval reuse is rejected because it accumulates interest debt and causes coverage-emergency catch-up.
- the safe path is true incremental scroll/delta update, not skipping rebuilds.
- request-cache inspection did not expose a clean next blind fix; representative walk remains a mixed bucket across clip, generation, request, surface, and GPU.

### Campaign Continuation - Voxel Footprint Reuse Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_rejected_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_vsync0_20260605`

Decision:

- default-off voxel-footprint signature reuse was implemented, tested, rejected, and removed.
- post-removal Release build passed.
- post-removal `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.
- no `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_FOOTPRINT_REUSE` env remains.
- no `-MidClipmapVoxelFootprintReuse` harness switch remains.
- all defaults remain unchanged.

Key comparison:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.88` | `25.05` | `22.96` | `22.64` | `0.84` | `1.25` | `0.00` | `0.00` | `0` | `0` | `100` | `100` |
| walk realtime | `600` | `52.16` | `59.33` | `45.85` | `78.03` | `17.39` | `45.00` | `5.44` | `32.09` | `447` | `441` | `95` | `94` |
| high-alt | `360` | `50.05` | `53.72` | `29.79` | `34.47` | `16.39` | `18.82` | `5.36` | `5.92` | `2228` | `2228` | `99` | `99` |

Root decision:

- voxel-only signature reuse still accumulated entering/leaving footprint churn (`newV/goneV=109/109`) and triggered coverage-emergency synchronous catch-up at walk frame `600`.
- signature/interval skip variants are now fully rejected.
- the next safe implementation branch is true mid-clipmap incremental scroll/delta update, with unchanged resident brick reuse by spatial key and generation/edit stamp.

### Campaign Continuation - Mid-Clipmap Interest Detail Timing - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_timing_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_vsync0_20260605`

New default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL=0`
- log `PERF_SPARSE_MID_CLIPMAP_INTEREST_DETAIL`

Build/test:

- Release build passed.
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.

Key timing:

- fixed frame `384`: raw/CPU `21.32/22.63`; interest reused and line/anchor/sort/backlog all `0.00`; fixed is blocked by generation/surface/GPU, not clipmap interest.
- walk realtime frame `600`: raw/CPU `60.84/38.38`; interest reused and line/anchor/sort/backlog all `0.00`; top buckets are GPU `19.44`, request `16.54`, gen `14.86`, surface extract/stage `11.13/2.61`, and clip/pump `6.95/5.10`.
- high-alt frame `360`: raw/CPU `51.32/32.84`; interest construction split line/anchor/sort/backlog `0.20/1.37/1.65/0.11`, while clip/pump was `17.61/5.80`.

Decision:

- another interest reuse/skip is not the next fix.
- the next measured branches are pump/generation/apply separation, surface extraction/staging, request prep, or a true entering/leaving mid-clipmap delta refactor.

### Campaign Continuation - Existing Branch Validation - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/existing_branch_validation_20260605.md`

Tested existing default-off branches:

| Branch | Fixed result | Walk realtime result | High-alt result | Decision |
|---|---|---|---|---|
| parallel mid pump | raw/CPU `39.07/27.63` | raw/CPU `78.06/31.68` | raw/CPU `51.50/31.37` | rejected as stack member |
| post-open surface cap + ready queue | raw/CPU `23.92/16.82`, pending `3233` | raw/CPU `89.93/50.70`, pending `5153`, oldest age `525` | raw/CPU `54.55/34.14` | rejected due surface-ready debt/regression |
| request fast resident touch | raw/CPU `22.51/21.14` | raw/CPU `90.48/52.88`, clip/pump `28.70/26.25` | raw/CPU `49.66/29.82` | rejected due walk catch-up regression |

Decision:

- existing isolated flags did not produce a valid stack.
- all defaults remain unchanged.
- current evidence points to a lane-aware streaming state-machine slice: separate visible/current ownership, hidden/post-open repair, and cache/prefetch work with independent budgets and backlog accounting.

### Campaign Continuation - Async Exact Prefetch Lane Stack - 2026-06-05

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test passed:

- `.\build.ps1 -Config Release`
- `ctest --test-dir build --output-on-failure -C Release` (`1/1`)

New default-off slice:

- `VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME=0`
- async enqueue/apply lane counts are logged in `PERF_SPARSE_EXACT_ASYNC`.

Decision:

- cache/prefetch exact generation can be queued async while visible/public streaming lanes remain synchronous/guarded.
- validation kept async visible/public enqueue counts at `0`.
- enqueue caps `8` and `16` were safe but over-throttled and rejected.
- parallel mid pump became useful only after async prefetch reduced other pressure.
- request fast resident touch became useful only in the async-prefetch + parallel-mid stack.
- parallel surface extraction was rejected; fixed raw regressed badly.
- parallel exact generation is mixed, not accepted as the strongest stack.

Best current default-off stack:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.87` | `18.01` | `4.25` | `12.99` | `0.76` | `0.00` | `6.26` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `72.81` | `35.76` | `15.36` | `14.19` | `6.20` | `4.35` | `5.54` | `451/273` | `95` | `0/0` |
| high-alt | `360` | `52.49` | `30.92` | `8.82` | `5.58` | `16.51` | `4.59` | `11.30` | `1746/0` | `100` | `0/0` |

12-frame walk-window average improved from baseline raw/CPU `79.78/48.63` to `64.32/32.90`, but this is still not playable and not a 60 FPS candidate.

Current blocker:

- mixed CPU debt remains: request prep, exact generation, clip/pump, and high-alt mid debt.
- next implementation should be a real lane-aware streaming state-machine slice, not another isolated flag.
- all defaults remain unchanged.
## Active Goal Compact Handoff - 2026-06-05

New compact resume file:

- `active-goal-handoff.md`

Active goal remains `streaming_playability_real_fix_campaign_20260604`.

Accepted-state build/test after this cycle:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Two hidden-exact branches were tried and removed:

- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_PREFETCH_LANE`: rejected because nearly all sampled work still came through critical/pressure classification; fixed/walk regressed.
- `VENPOD_SPARSE_HIDDEN_EXACT_PARALLEL_GENERATION`: rejected because fixed CPU/gen improved but walk/high-alt CPU regressed or stayed mixed.

Fresh accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `10.26` | `1746/0` | `100` | `0/0` |

Current decision: do not retry hidden-exact post-open lane deferral or direct hidden-exact parallel generation as simple fixes. The next real fix needs to target request/gen coupling or exact-generation pipeline architecture with staged worker/apply accounting.

## Active Goal Branch Ladder Closure - 2026-06-05

Compact resume file:

- `active-goal-handoff.md`

Post-cleanup build/test:

- `.\build.ps1 -Config Release`: passed before compaction.
- `ctest --test-dir build --output-on-failure -C Release`: passed after compaction, `1/1`.

Latest accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `0.00` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `6.59` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `4.93` | `10.26` | `1746/0` | `100` | `0/0` |

Latest rejected/not-accepted branches:

- generation queue fast membership: removed; fixed/high-alt regressed and raw did not improve.
- pressure trim free page guard: not accepted; walk clip/raw regressed.
- parallel surface extraction: not accepted; walk/high-alt CPU regressed.
- direct footprint columns: not accepted; request/gen/clip regressed.
- persistent terrain column cache: removed; cache growth/hash overhead worsened runtime.

Removed transient flags:

- `VENPOD_SPARSE_GENERATION_QUEUE_FAST_MEMBERSHIP`
- `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_PERSISTENT`

Root decision:

- no validated 60 FPS candidate exists;
- the best opt-in stack is still far above budget: fixed about `23 ms`, walk about `58 ms`, high-alt about `51 ms`;
- isolated local knobs have been exhausted enough to stop treating this as a missing scalar;
- next implementation must be an ownership-aware streaming state-machine slice with persistent visible/public-critical, sampled-visible, hidden/post-open repair, cache, and prefetch lanes across request, generation, upload/apply, surface extraction, and publish queues;
- unknown fallback remains critical and must not be moved async.

## Generated-Lane Accounting Slice - 2026-06-05

Retained behavior-neutral accounting:

- `SparseVoxelWorldStats::generated*LaneBricksLastFrame`
- `PERF_SPARSE_GENERATED_LANES`
- generated-lane columns in `perf_noncapture_smoke.ps1`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generated_lane_accounting_v2_20260605`

Key result:

| Scenario | Frame | Gen ms | Queued gen lanes prefetch/visible/public | Generated lanes prefetch/visible/public | Miss/unsafe |
|---|---:|---:|---:|---:|---:|
| fixed | `384` | `13.99` | `915/121/0` | `24/121/0` | `0/0` |
| walk realtime | `600` | `16.07` | `4929/142/47` | `15/88/47` | `0/0` |
| high-alt | `360` | `5.14` | `6968/11/8` | `20/11/8` | `0/0` |

Root decision:

- the representative generation cost is visible/public-critical, not mostly cache/prefetch;
- moving more cache/prefetch work async will not solve the current fixed/walk generation wall;
- unknown sampled visible work remains critical;
- next code path should accelerate visible/public exact generation without deferring ownership, likely via persistent synchronous workers or a same-frame staged public-critical generation/apply pipeline.

### Parallel Exact Std Execution Candidate - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_rejected_20260605.md`

Rows:

| Scenario | Baseline raw/CPU/gen/clip | Candidate raw/CPU/gen/clip | Decision |
|---|---:|---:|---|
| fixed | `24.54/18.76/13.99/0.77` | `25.13/20.86/15.13/0.75` | rejected |
| walk realtime | `67.95/55.11/16.07/21.10` | `72.46/46.34/12.53/20.45` | rejected |
| high-alt | `51.02/28.72/5.14/15.31` | `49.42/33.21/5.57/19.07` | rejected |

Decision:

- temporary `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION` removed
- build and ctest passed after removal
- all defaults remain unchanged
- per-call exact parallel variants are not the current fix

### Persistent Exact Workers Candidate - Partial Keep Default-Off - 2026-06-05

New default-off flag:

- `VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS=0`

Validation:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_2w_candidate_20260605`

Rows:

| Scenario | Raw | CPU | Gen | Clip | GPU | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed | `21.81` | `17.17` | `12.55` | `0.76` | `6.97` | `0/0` | partial keep |
| walk realtime | `67.23` | `32.40` | `12.82` | `6.38` | `20.19` | `0/0` | CPU improved; raw not solved |
| high-alt | `51.21` | `28.80` | `4.74` | `15.34` | `13.16` | `0/0` | neutral |

Decision:

- keep 2-worker persistent exact generation default-off.
- do not promote defaults.
- no 60 FPS candidate yet.
- next blocker has shifted from pure exact generation to fixed surface extraction and walk/high-alt mixed CPU/GPU/frame-gap.

### Post-Open Surface Cap 1 ms - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_persistent_exact_2w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_rejected_20260605.md`

Decision:

- existing post-open pre-publish surface max `1 ms` was tested and rejected.
- it reduced one extraction slice but produced/kept a large publish backlog and regressed walk/high-alt.
- do not use cap-only surface throttling as the playable stack fix.

### Persistent Mid-Clipmap Workers and Trim Budget Probe - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS=0`

Validation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim8192_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim4096_walk_probe_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Result:

| Candidate | Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| persistent mid workers | fixed f384 | `20.88` | `18.59` | `3.73` | `14.07` | `0.79` | `0.00` | `6.07` | partial keep |
| persistent mid workers | walk f600 | `79.36` | `39.97` | `16.56` | `15.27` | `8.12` | `5.95` | n/a | target raw noisy |
| persistent mid workers | high-alt f360 | `48.43` | `28.12` | `8.37` | `4.87` | `14.87` | `5.04` | `8.59` | partial keep |
| interest interval 2 probe | walk f600 | `63.63` | `38.51` | `13.82` | `12.34` | `12.33` | `10.01` | `15.67` | mixed, not final |
| interest interval 2 probe | high-alt f360 | `49.54` | `19.61` | `7.96` | `4.81` | `6.83` | `4.55` | `9.99` | CPU win, raw not solved |
| trim budget 8192 | walk f600 | `67.32` | `39.04` | `10.79` | `13.19` | `15.03` | `12.66` | `15.58` | rejected |
| trim budget 4096 | walk f600 | `62.31` | `37.77` | `14.02` | `16.84` | `6.89` | `5.02` | `9.07` | probe only |

Decision:

- persistent mid workers are safe to keep default-off because default behavior is unchanged.
- interest interval `2` and smaller pressure-trim scan budgets are not validated stack defaults.
- lowering trim scan records reduced `pressureTrimMs`, but cost shifted into terrain-critical hierarchy/generation/clip; it is not a real playability fix.
- remaining blockers are architectural: fixed exact/surface generation pressure, walk mixed request/gen/clip/surface/GPU/gaps, and high-alt mid/GPU/raw time.

### Post-Compaction Branch Rejections - 2026-06-05

Build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Two local branches were attempted after compaction and then removed:

| Branch | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| exact coordinate batch generation | `22.92/18.51` | `89.44/34.29` | `50.61/27.30` | rejected; batching hidden-exact coords did not reduce fixed generation and worsened walk raw |
| pressure trim miss-feedback queued-only | `25.12/20.38` | `64.14/41.85` | `49.43/26.72` | rejected; reduced a trim subpath but did not reduce frame time safely |

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_coord_batch_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_miss_feedback_queued_only_candidate_20260605`

No rejected branch symbols remain. No defaults changed.

Decision:

- the campaign should move to a real ownership-lane streaming state-machine slice rather than more isolated queue, batch, or trim gates.
- required lanes: public-visible, sampled-visible, hidden/post-open repair, cache, and prefetch.
- each lane needs explicit request, generation, apply/upload, surface extraction, and publish budgets plus readiness behavior.

### Exact Local Column Block Candidate - Rejected - 2026-06-05

Artifacts:

- control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_control_20260605`
- candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_candidate_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Result:

| Scenario | Control raw/CPU/gen | Candidate raw/CPU/gen | Decision |
|---|---:|---:|---|
| fixed f384 | `21.41/19.13/14.11` | `19.89/22.18/17.41` | rejected |
| walk f600 | `55.66/36.42/11.17` | `58.70/52.03/18.20` | rejected |
| high-alt f360 | `44.66/22.17/4.55` | `46.16/25.39/6.64` | rejected |

Decision:

- temporary exact local-column-block generation was removed.
- no rejected local-block symbols remain.
- only behavior-neutral exact-generation helper centralization remains.
- this disproves the local terrain-column block replacement as a playable fix; generation pressure is still architectural/pipeline-level.

### Formal Streaming State-Machine Direction - 2026-06-05

The next implementation unit is a default-off streaming work-ticket scheduler, not another isolated queue knob.

Current blocking path:

1. request/touch assigns residency and streaming lane;
2. exact generation creates a CPU payload;
3. `ApplyGeneratedBrickPayload` immediately queues upload and surface extraction;
4. upload, surface extraction, and page publication are budgeted separately;
5. public render sees whichever stages completed, without one ownership ticket governing the full lifecycle.

Required V1:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0` default-off.
- per-brick ticket with ownership class, required/completed stage masks, fallback proof, edit revision, request/sample frames, deadline, and backlog age.
- public/sampled/unknown tickets are same-frame protected or readiness-gated.
- hidden repair is budgeted after public/sampled work.
- cache/prefetch/fallback-valid tickets may be async/budgeted.
- surface extraction and publish scheduling must consume the same ownership ticket as generation/upload.

This is the smallest credible architecture slice left. The prior local branches show that generation, upload, surface, and trim cannot be optimized independently into a playable renderer while preserving ownership correctness.

### Streaming Work Ticket Shadow V1 - 2026-06-05

New default-off code:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0`
- `PERF_SPARSE_STREAMING_TICKETS`

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_shadow_v1_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Result:

| Scenario | Frame | Raw | CPU | Ticket state |
|---|---:|---:|---:|---|
| fixed | `382` | `26.51` | `5.76` | active `796`, all prefetch pending CPU |
| walk realtime | `600` | `69.96` | `51.41` | active `4889`; sampled/prefetch `1213/3676`; pending CPU/upload/surface/publish `3228/1661/62/1661` |
| high-alt | `360` | `44.12` | `23.85` | active `6110`; public/sampled/prefetch `8/15/6087`; pending CPU `6110` |

Decision:

- retained as default-off shadow state only.
- no scheduling or rendering behavior changes yet.
- next step is protected ticket-based queue pop order across generation/upload/surface/publish.

### Protected Ticket Scheduling Continuation - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING=0`
- harness switch `-StreamingTicketProtectedScheduling`
- `PERF_SPARSE_STREAMING_TICKETS` includes `protected` and `protectedSorts`

Implementation:

- protected scheduling requires `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=1`.
- generation, upload, and surface queues can be ordered by ticket ownership/stage.
- public critical, unknown critical, sampled visible, hidden repair, and fallback-valid tickets outrank cache/prefetch.
- refined helper skips protected sorting for cache/prefetch-only queues, preserving existing order there.
- no work is dropped and no defaults changed.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched tracked files: only existing LF-to-CRLF warnings.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_protected_refined_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_budget32_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_postopen_gen32_only_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/gen32_incremental_trim_candidate_20260605`

Window summary:

| Candidate | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| ticket shadow control | `24.53/54.92` | `68.40/102.26` | `55.68/67.97` | control |
| protected ticket scheduling | `27.43/57.65` | `63.97/79.15` | `46.55/59.26` | mixed; fixed churn |
| refined protected scheduling | `24.89/58.54` | `66.25/80.41` | `45.75/58.99` | partial keep default-off |
| hidden exact budgets `32/32/32/64` | `24.41/39.89` | `70.56/121.00` | `51.80/80.20` | rejected |
| hidden exact generation `32` only | `22.73/37.38` | `64.35/84.00` | `49.87/64.85` | mixed, not global |
| gen32 + incremental pressure trim | `25.26/44.30` | `72.25/89.51` | `57.48/84.05` | rejected |

Decision:

- refined protected ticket scheduling is retained default-off as the latest state-machine slice.
- full hidden-exact post-open budget throttling is rejected because it moved debt into upload and regressed walk/high-alt windows.
- hidden-exact generation-only pacing is a mixed probe: useful fixed/walk signal, but not a global candidate due high-alt regression.
- incremental pressure trim is rejected for this stack.
- no 60 FPS candidate exists yet.

Remaining blocker:

- fixed still has hidden-exact/surface bursts;
- walk remains a mixed request/generation/clip/surface/GPU frame;
- high-alt benefits from protected tickets but remains around mid-40 ms raw;
- the next credible code slice is ticket-controlled apply/upload/surface/publish pacing, not another isolated scalar cap.

### Streaming Ticket Stage Pacing Rejected - 2026-06-05

Tried a default-off page-publish stage-pacing slice and removed it after validation.

Removed:

- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING_MIN_CAMERA_Y`
- `PopReadyByLanePriority` / `PopReadyProtectedLane`
- `PERF_SPARSE_STREAMING_TICKET_STAGE_PACING`
- harness switches for stage pacing

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_highalt_only_candidate_20260605`

Result:

| Row | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| same-code control | `23.88/53.43` | `83.12/108.59` | `65.15/93.08` | control |
| broad stage pacing | `26.77/62.45` | `86.76/206.33` | `54.86/83.41` | rejected |

The high-alt-only variant also failed due late raw outliers (`87-107 ms`) even though the parser missed the exact frame-400 summary. Post-removal build and `ctest` passed.

Decision:

- page publish selection alone is rejected.
- continue from same-code control: walk f600 is currently the most important target row, with raw/CPU `83.47/65.82` and request/gen/clip/pump `18.98/16.92/29.90/27.55`.

### Request Accounting Split and Pressure-Trim Guard Rejection - 2026-06-05

Retained behavior-neutral `PERF_SPARSE_REQUEST_DETAIL` split:

- `hierarchyOtherMs`
- `preHierarchyMs`
- true residual `otherMs`
- old residual `legacyOtherMs`
- `pressureTrimPressure=free/gen/miss`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_accounting_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_queued_scan_pool_guard_candidate_20260605`

Walk f600 request-accounting baseline:

- raw/CPU `91.48/62.52`
- request/gen/clip/pump `28.51/25.28/8.70/6.03`
- surface extract/stage `14.41/3.82`
- GPU `23.75`
- request split: pressure trim `9.31`, terrain critical `6.67`, stats flush `3.75`, hidden exact `3.63`, true other `3.88`

Rejected and removed:

- `VENPOD_SPARSE_PRESSURE_TRIM_QUEUED_SCAN_REQUIRES_POOL_PRESSURE`
- `-PressureTrimQueuedScanPoolGuard`

Reason:

- representative bad frames were miss-feedback pressure (`pressureTrimPressure=0/0/1`), not generation-only pressure, so the guard did not skip queued trimming.
- candidate walk regressed to raw/CPU `111.69/60.04` with gen `29.10`.

Decision:

- keep request accounting.
- do not continue queued-trim gating.
- next branch should address hidden-exact/miss-feedback admission or generation/surface coupling.

### Async Low-Priority Apply / Persistent Mid Validation - 2026-06-05

New retained default-off control:

- `VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME=0`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- Release build passed.
- `ctest` passed `1/1`.

Result:

| Candidate | Fixed Avg/Max Raw | Walk Avg/Max Raw | High-alt Avg/Max Raw | Decision |
|---|---:|---:|---:|---|
| control | `22.51/53.25` | `85.93/624.57` | `93.93/117.04` | control |
| cap8 | `23.76/53.87` | `69.72/96.42` | `58.57/73.27` | rejected; fixed deferred `504` low-priority completions |
| cap32 | `23.33/52.65` | `65.01/78.52` | `55.64/75.46` | partial |
| cap32 + persistent mid | `23.00/51.02` | `60.90/71.20` | `56.00/63.54` | strongest partial |
| cap32 + persistent mid + exact2 | `23.05/51.79` | `72.74/106.83` | `57.91/67.92` | rejected |

Decision:

- no validated 60 FPS candidate exists.
- cap32 plus persistent mid workers is the strongest default-off partial stack, but it is not playable.
- cap8 is rejected because it shifts work into low-priority apply debt.
- persistent exact workers are rejected in this stack.
- high-alt still has the known bright white shoreline/terrain artifact in the contact sheet.
- all defaults remain unchanged.
- next root decision should focus on high-alt ownership/shoreline correctness and the streaming state machine, while continuing to measure distributed walk costs.

## Streaming Playability Hidden Exact / Cache Defer Follow-Up - 2026-06-05

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

Decision log:

- non-water post-open hidden repair split is ineffective because current fixed/walk post-open accepted feedback is mostly water feedback.
- water-inclusive hidden repair lane reduces walk CPU but shifts work into upload/post-wait and grows hidden exact debt; do not promote.
- cache-only visible-critical defer is a scoped high-alt performance partial, not a walk fix and not a visual fix.

Rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | Surface Extract/Stage | GPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| water repair walk f600 | `85.91/31.75` | `15.59` | `7.38` | `8.76/6.67` | `11.31/2.05` | `14.32` | CPU win, upload/post-wait/debt fail |
| cache-only fixed f384 | `24.22/22.02` | `4.78` | `16.42` | `0.81/0.00` | `26.00/2.02` | `6.93` | unchanged/no missing mid debt |
| cache-only walk f600 | `69.45/52.64` | `18.85` | `16.87` | `16.89/4.54` | `12.51/3.00` | `5.91` | inactive on projected-visible walk debt |
| cache-only high-alt f400 | `54.59/22.30` | `8.34` | `7.02` | `6.94/0.01` | `5.01/1.93` | `12.39` | scoped high-alt CPU/clip win |

High-alt cache-only details:

- `cacheOnlyDefer=1867`
- visible-critical coverage `99`
- cache coverage `79`
- parent-held failure `0`
- miss/unsafe `0/0`
- visual contact sheet still shows the large pale lower-screen terrain/shore band

Current conclusion:

- no validated 60 fps candidate exists.
- all public defaults remain unchanged.
- high-alt cache debt can be reduced safely, but high-alt visual ownership remains unresolved.
- walk/realtime remains projected-visible debt and cannot be safely deferred without ownership proof.
- next work should focus on high-alt visual ownership/shoreline contract or projected-visible walk request/generation/surface coupling under the ticket/state-machine model.

## High-Alt Background Split Exact-Band Ownership Finding - 2026-06-05

Latest high-alt visual diagnostics changed the next branch.

The large pale lower-screen high-alt band is not primarily a mid-clipmap pump/cache-only defer failure. Background reason mode sampling over the lower band is dominated by `far_svo_unavailable_or_rejected`, and representative rays are downward with plausible water-plane crossings. This is visually wrong sky-like output despite the usual numeric miss/unsafe counters staying clean.

The relevant code path is the background split exact-band admission rule:

- `Renderer.cpp` clears sparse-near flag bit `8` for background split unless `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1`.
- `PS_Raymarch.hlsl` uses that bit as `sparseSurfaceRaymarchFill`.
- `BackgroundHitAllowedByExactNear` only allows mid/Far-SVO/far-water to own pixels inside the exact/surface ownership radius when that fill flag is set.
- with the fill flag off, unstenciled exact-band pixels in the lower-res background pass can reject valid-looking water/background owners and fall through to sky-like output.

Surface-fill probe:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` visually removed the high-alt lower pale band.
- frame `400` sky-like image percentage dropped from about `41.18%` to about `20.51%`.
- layer mix shifted toward plausible water/background ownership: far water about `8.25%`, water context about `12.30%`, sky about `2.45%`.

Safety blocker:

- surface fill is not contract-safe yet.
- high-alt frame `400` reported `shaderUnsafeContractNonReady=198`.
- walk frame `600` reported `shaderUnsafeContractNonReady=83`.
- therefore surface fill remains diagnostic/default-off and must not be promoted.

Decision:

- do not continue high-alt visual work as another scheduling/cap problem.
- next branch should validate a narrow background-fill ownership contract:
  - prove that the filled pixels are deterministic water/Far-SVO owners and keep exact-unsafe accounting correct, or
  - make exact foreground/water readiness cover those pixels before public/split fill, or
  - add a narrow admission rule for only CPU/shader-proved deterministic water/Far-SVO, leaving unknown fallback blocked.
- walk/realtime remains a separate projected-visible streaming debt problem across request, generation, clip/pump, surface extraction, upload/post-wait, and residual GPU.

### Surface-Fill Exact Repair / Cached Water Proof - 2026-06-05

Patch:

- added default-off `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF=0`
- added default-off `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR=0`
- cached deterministic-water proof results by brick and edit revision
- extended `PERF_SPARSE_SHADER_UNSAFE_WATER_PROOF` with cache and compute timing

Validation:

| Row | Raw/CPU | Request | GPU | Contract | Decision |
|---|---:|---:|---:|---|---|
| uncached high-alt f480 | `97.66/72.94` | `49.46` | `30.42` | raw `66`, water `61`, repair `5` | rejected CPU spike |
| cached high-alt f480 | `57.03/32.60` | `16.95` | `18.70` | raw `31`, water `27`, repair `4` | useful partial, not playable |
| cached walk f600 | `77.12/185.37` | `142.32` | `26.69` | raw `92`, water `39`, repair `48/53` | rejected movement stack |
| cached walk + hidden budget f600 | `122.00/46.32` | `22.82` | `39.97` | raw `47`, water `42`, repair `5` | rejected raw/window regression |

Decision:

- cached water proof is a useful default-off safety/perf improvement for future surface-fill validation.
- surface-fill exact repair is not a playable candidate and must not be promoted.
- high-alt still needs visual/contact-sheet validation of the cached path plus residual CPU/GPU reduction.
- walk remains projected-visible streaming debt and hidden-exact repair collision, not a water-proof CPU problem.

## High-Alt-Only Surface Fill / Incremental Pressure Trim - 2026-06-05

Latest default-off stack adds `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY=0`. In candidate mode, surface-fill water proof / exact repair can be requested but only becomes active on frames where high-alt LOD admission is allowed. This avoids the global walk hidden-exact collision.

Build/test:

- Release build passed.
- `ctest --test-dir build --output-on-failure -C Release` passed `1/1`.

Rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `25.18/19.28` | `5.04` | `13.38` | `0.84/0.00` | `8.21` | `27.40/60.28` | partial |
| walk f600 | `63.49/33.26` | `9.16` | `17.66` | `6.42/4.16` | `17.44` | `69.16/81.81` | partial |
| high-alt f480 | `55.10/24.35` | `7.99` | `9.38` | `6.98/0.00` | `17.57` | `57.96/75.45` | partial; contact sheet plausible |

Rejected in this cycle:

- surface-parallel extraction: walk CPU regressed to `60.54 ms`.
- terrain-critical parallel generation: no useful parallel critical work at f600; raw/window regressed.
- background scale `0.25`: GPU improved, but CPU/clip/raw regressed.

Decision:

- no validated 60 FPS candidate exists.
- strongest current stack is a default-off visual/contract partial only.
- high-alt-only surface-fill plus cached water proof is safer than global surface-fill; it does not activate repair in normal walk.
- incremental pressure trim/free-page guard is a real default-off CPU win.
- all defaults stay unchanged.
- remaining blockers: fixed generation/surface/post-wait, walk generation/surface/GPU/post-wait, high-alt GPU/post-wait plus residual generation/surface.
## Streaming Playability Real Fix Campaign - 2026-06-05 Continuation

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/playable_candidate_table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/contact_sheet.png`

Result:

- no validated 60 FPS candidate was reached.
- the current best validated stack remains the previous default-off stack: playable render scale, background split `0.375`, clean prefetch throttle, critical reuse, and parallel mid pump.
- incremental pressure trim `4096` is rejected for this stack despite its perf win because the walk visual capture reported terrain-critical post-publish nonready at frames `604` and `606`.
- the same walk capture without incremental pressure trim passed with postNonReady `0`.
- incremental pressure trim `16384`, streaming lane priority, protected ticket scheduling, persistent terrain column cache, request fast resident touch, parallel exact generation, and explicit-lane async variants were all tested and rejected or kept diagnostic-only.

Key rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU ray | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| accepted fixed f384 | `23.80/22.31` | `4.50` | `17.00` | `0.80/0.00` | `20.87/2.27` | `6.04` | visual passed, not 60 |
| accepted walk f600 | `59.51/35.60` | `15.40` | `14.67` | `5.51/4.01` | `11.32/2.74` | `12.95` | visual passed, not 60 |
| accepted high-alt f398 | `51.66/26.21` | `4.94` | `5.18` | `16.09/9.74` | `3.78/1.79` | `10.25` | readiness passed; known bright shoreline/terrain artifact remains |
| trim4096 walk f600 | `52.32/30.58` | `11.12` | `13.78` | `5.67/4.00` | `11.79/2.71` | `12.68` | rejected; terrain-critical postNonReady `3/7` in visual capture |
| trim16384 walk f600 | `55.90/41.84` | `16.52` | `17.52` | `7.77/5.85` | `11.99/3.16` | `13.07` | rejected; no useful perf win |
| lane-priority walk f600 | `55.44/45.81` | `12.96` | `12.40` | `20.39/9.33` | `11.25/2.45` | `6.51` | rejected; clip/CPU regression |
| protected-ticket walk f600 | `78.41/39.52` | `13.13` | `14.71` | `11.64/9.36` | `12.74/3.37` | `14.63` | rejected; raw/window regression |

Current root decision:

The remaining blocker is an ownership-aware streaming state-machine gap, not a missing scalar budget. Existing lanes can identify prefetch/cache source work, but generation/upload/surface scheduling still collapses much of that debt into visible-class queues. Safe async remains blocked for sampled fallback-unknown/invalid work. A real fix needs separate ownership-critical class and source/prefetch lane carried through generation, upload/apply, surface extraction, and publish.

Next implementation slice:

Build a default-off ownership-class scheduler:

- visible/sampled fallback-invalid or unknown bricks stay critical.
- cache/prefetch-only bricks do not drive public-frame readiness.
- generation, upload, and surface extraction have separate critical and prefetch queues.
- async is allowed only for cache/prefetch or CPU-proved fallback-valid work.
- terrain-critical capture checks must pass before accepting any perf row.

### Ownership-Class Split Follow-Up - 2026-06-05

Implemented a default-off test slice:

- `VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS=0`
- maps `Visible + Prefetch` touches to speculative-class/prefetch-lane inside `SparseVoxelWorld` when enabled
- exposed via `perf_noncapture_smoke.ps1 -PrefetchLaneSpeculativeClass`

Results:

| Candidate | Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---|
| class split | fixed f380 | `37.58/5.71` | `1.86` | `3.05` | `0.77/0.00` | `6.95` | rejected; raw/post-wait worsened |
| class split | walk f600 | `57.04/39.98` | `10.16` | `9.66` | `20.13/9.28` | `10.61` | rejected; CPU/clip worsened |
| class split | high-alt f360 | `57.19/31.57` | `8.79` | `7.14` | `15.62/6.22` | `10.88` | rejected |
| class split + async prefetch | fixed f384 | `20.56/20.82` | `4.54` | `15.24` | `1.02/0.00` | `6.41` | fixed-only partial |
| class split + async prefetch | walk f600 | `62.18/44.70` | `12.00` | `12.33` | `20.35/9.41` | `6.01` | rejected; async backlog/regression |
| class split + async prefetch | high-alt f360 | `51.29/29.28` | `7.54` | `6.62` | `15.11/5.86` | `8.97` | not enough; async backlog |

Decision:

- keep the new flag default-off and diagnostic only.
- it proves source/class separation is necessary but insufficient by itself.
- the next implementation must carry ownership class into upload/apply and surface extraction budgets; otherwise CPU generation savings move into post-wait/surface/clip debt.

## Surface Lane Stage / Protected Ticket Rejection - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_zero_prefetch_probe`
- `build/captures/streaming_playability_real_fix_campaign_20260605/streaming_ticket_protected_walk_probe`
- updated `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`, `table.md`, and `playable_candidate_table.md`

Rows:

| Branch | Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---|
| lane-stage budgets | fixed f384 | `18.77/19.47` | `4.18` | `14.49` | `0.78/0.00` | `19.52/1.99` | `5.75` | fixed-only partial |
| lane-stage budgets | walk f600 | `65.98/35.18` | `11.29` | `12.33` | `11.55/9.57` | `11.45/2.81` | `9.57` | rejected; raw/window worse than accepted stack |
| lane-stage budgets | high-alt f360 | `49.08/29.22` | `8.00` | `5.91` | `15.30/6.06` | `5.33/1.53` | `6.72` | not enough |
| zero-prefetch lane stage | walk f600 | `56.48/36.49` | `15.11` | `14.14` | `7.22/5.56` | `12.46/2.62` | `12.55` | rejected; visible surface work remains |
| protected ticket probe | walk f600 | `62.21/47.56` | `12.37` | `11.91` | `23.25/11.40` | `11.78/2.66` | `13.84` | rejected; clip/CPU worsened |

Decision:

- the lane-stage prototype was removed; no rejected symbols remain.
- blind prefetch/low-lane surface deferral and protected-ticket scheduling remain rejected as movement fixes.
- current best validated stack remains unchanged and not 60 FPS.
- local caps/sorts have reached diminishing returns; next real fix must be an ownership-critical state machine carried through request, generation, upload/apply, surface extraction, and publish readiness.
