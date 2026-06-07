# VENPOD Streaming Playability Real Fix Campaign Handoff

## 2026-06-05 Restart Contract

Active tool goal:

```text
streaming_playability_real_fix_campaign_20260604: drive VENPOD from current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes, with explicit anti-stop rules, persistent handoff updates, and no correctness weakening or default promotion without proof.
```

This handoff is the compact resume anchor. On compaction or resume:

1. Read this file and `active-goal-handoff.md` before coding.
2. Do not create a narrow replacement goal unless the active campaign reaches a formal completion state.
3. Do not stop after a diagnostic, one failed prototype, or a partial improvement while another measured, safe implementation branch remains.
4. Preserve the public-frame ownership contract: every visible pixel must have a valid owner, and unknown fallback is not valid.
5. Keep all default-off systems default-off unless behavior-equivalence is proven.
6. Update this file before any final answer or compaction-sensitive pause.

Formal completion states:

- validated 60 FPS candidate in representative noncapture fixed and walk rows, with clean visual and ownership validation;
- strongest validated non-60 default-off stack with exact remaining blocker table;
- hard architecture blocker with the exact missing ownership/streaming refactor specified;
- hard build/test/shader/runtime blocker after attempted fixes.

Restart baseline:

- `git status --short`: dirty worktree with many existing VENPOD changes; do not revert unrelated work.
- `.\build.ps1 -Config Release`: passed, `ninja: no work to do`; known trailing `vswhere.exe` warning remains after success.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

First live architecture finding for the next branch:

- Generated/upload packets and streaming tickets carry `streamingLane`.
- Surface extraction queues and publish gating are still mostly residency-class driven.
- This means `Visible` residency work that is actually cache/prefetch/hidden repair can still consume visible-class surface/publish work unless lane identity is preserved through those downstream gates.
- The next safe implementation branch should inspect and, if validated, make surface extraction / publish readiness lane-aware. PublicCritical, sampled-visible, and unknown-critical work stays guarded; cache/prefetch or CPU-proven fallback-valid work may be separately budgeted.

## 2026-06-06 Surface-Ready Lane Accounting / Front-Sort Rejection

Retained behavior-neutral accounting:

- `PERF_SPARSE_SURFACE_READY_PUBLISH` now logs `pendingLane=cache/prefetch/visible/public:a/b/c/d`.
- `perf_noncapture_smoke.ps1` parses those values into the summary/table as:
  `surfaceReadyPublishLaneCache`, `surfaceReadyPublishLanePrefetch`,
  `surfaceReadyPublishLaneVisible`, `surfaceReadyPublishLanePublic`.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.

Artifacts:

- fixed accounting sanity: `build/captures/ownership_lane_surface_ready_accounting_20260605`
- accepted-stack short walk baseline: `build/captures/ownership_lane_surface_ready_accounting_walk120_accepted_20260605`
- rejected front-sort probe: `build/captures/streaming_ticket_front_sort_walk120_accepted_20260606`

Accepted-stack short walk baseline (`-WalkFrame 120`, accepted partial stack):

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Window avg/max raw |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `72.65/91.67` |
| front-sort prototype | `118.76` | `188.47` | `114.69` | `34.21` | `39.54` | `23.70` | `18.46` | `15.50` | `124.01/323.16` |

Lane evidence at baseline f120:

- `PERF_SPARSE_SURFACE_READY_PUBLISH`: pending `0`, `pendingLane=0/0/0/0`.
- `PERF_SPARSE_PAGE_PUBLISH_LANES`: publish and surface-ready lanes all `0`.
- `PERF_SPARSE_STREAMING_TICKETS`: active `8509`, ownership public/sampled/prefetch `0/120/8389`, all pending CPU.

Decision:

- Surface-ready publish pressure is not the immediate accepted-stack walk blocker in this sample.
- The attempted default-off generation front-only ticket sort was removed after validation; do not retry it as the main fix.
- Next useful work should split public/sampled correctness work from broad prefetch/cache CPU-stage debt in the streaming state machine, or reduce request/clip/pump/GPU cost without weakening public-frame readiness.

## 2026-06-06 Low-Lane Queued Trim Rejection

Attempted and removed a default-off prefetch/cache queued-brick trim branch. It targeted already-admitted queued low-lane work (`Requested`, `GeneratedCPU`, `UploadQueued`) under prefetch generation queue pressure.

Removed code/harness surface:

- `SparseVoxelWorld::TrimQueuedStreamingLaneBackgroundBricks`
- `VENPOD_SPARSE_PREFETCH_QUEUE_PRESSURE_TRIM*`
- `perf_noncapture_smoke.ps1 -PrefetchQueuePressureTrim*`

Validation artifacts:

- aggressive trim: `build/captures/prefetch_queue_pressure_trim_walk120_accepted_20260606`
- conservative trim: `build/captures/prefetch_queue_pressure_trim_b8_walk120_accepted_20260606`

Matched accepted-stack short walk:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `72.65/91.67` | baseline |
| trim threshold4096 budget64 | `107.68` | `79.85` | `22.51` | `17.47` | `39.85` | `24.26` | `8.90` | `11.88` | `81.20/113.77` | rejected/removed |
| trim threshold8192 budget8 | `107.35` | `54.77` | `22.94` | `23.98` | `7.83` | `6.59` | `15.51` | `6.81` | `114.79/247.53` | rejected/removed |

Decision:

- Trimming already-admitted low-lane queued work is the wrong layer. It reduces or barely touches the backlog while increasing request/generation/clip/surface/frame-gap churn.
- Future work should not reintroduce this as a main performance path.
- If the prefetch backlog remains the blocker, fix admission/backpressure before page allocation and queue insertion, or implement ownership-ticket budgets across generation, upload/apply, surface extraction, and publish readiness.

## 2026-06-06 Predicted Visible Admission Rejection

Goal: fix the high-alt f402/f403 visible-critical coverage collapse without weakening public ownership. Baseline strong stack still drops from about `99/99` at f400/f401 to `86/84` at f402/f403.

Retained default-off diagnostic/API surface:

- `SparseClipmapTileCache::QueuePredictedVisibleVoxelInterest(...)`
- `VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION`
- `perf_noncapture_smoke.ps1 -MidClipmapPredictedVisibleAdmission*`
- Async visible reservation ticket diagnostics in `SparseClipmapCacheStats`,
  `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP`, and
  `perf_noncapture_smoke.ps1`.
- Deadline-aware current-first visible async enqueue ordering:
  current visible-critical coords sort before reservation tickets; reservations
  then sort by predicted deadline/sample.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts and decisions:

| Probe | Artifact | f400 coverage | f402/f403 coverage | Window avg/max raw | Decision |
|---|---|---:|---:|---:|---|
| sparse ray admission 128 | `build/captures/highalt_predicted_visible_admission128_incrementaltrim2048_20260606` | `99` | `86/85` | `96.70/105.48 ms` | rejected |
| future-interest projection inserted into current interest 512 | `build/captures/highalt_predicted_interest_admission512_incrementaltrim2048_20260606` | `93` | `85/85` | `71.60/78.82 ms` | rejected |
| future-interest reservation 512 | `build/captures/highalt_predicted_reservation512_incrementaltrim2048_20260606` | `99` | `86/84` | `126.41/136.44 ms` | rejected as candidate |
| reservation hit diagnostics 512 | `build/captures/highalt_predicted_reservation_ticketdiag512_20260606` | `99` | `86/84` | `79.83/85.11 ms` | prediction mostly correct |
| deadline ticket 512, current-first | `build/captures/highalt_predicted_deadline_ticket512_currentfirst_20260606` | `99` | `88/88` | `74.55/78.40 ms` | retained default-off slice, not candidate |

Architecture result:

- Sparse ray prediction under-sampled the relevant future ownership set.
- Full future-interest projection found and queued hundreds of future coords, proving the target must be clipmap-interest based rather than ray-hit based.
- Future coords must not enter `m_voxelInterestSet` before they are current; doing so corrupts current visible-critical coverage.
- Reservation-based future work is the correct ownership shape because it can queue async visible work without counting as present-frame interest.
- Reservation hit diagnostics proved the predictor is mostly hitting the future
  current-visible set: f402 had `1027/1125` hits and f403 had `1115/1233` hits
  in `highalt_predicted_reservation_ticketdiag512_20260606`.
- Deadline-aware scheduling improved f402/f403 coverage to `88/88`, but the
  high-alt window remained too slow (`74.55 ms` avg raw) and upload/post-wait
  pressure rose. This is architecture progress, not a playable candidate.

Next direction:

- Do not retry broad predicted-visible admission as the main fix.
- Do not spend the next branch on prediction accuracy first; the evidence says
  the predicted set already overlaps the f402/f403 visible-critical set.
- Build the next ownership-ticket stage around controlled promotion:
  deadline-critical tickets need separate generation, apply/upload, and surface
  budgets, with current public-critical work always first and future tickets
  prevented from creating upload or post-wait spikes.

Follow-up retained default-off slice:

- `SparseClipmapTileCache::ApplyAsyncNoncriticalVoxelGenerationCompletions(...)`
  now has optional `maxVisibleReservationApply`.
- Current visible-critical async applies remain governed by the existing visible
  budget; reservation-only visible async applies can be separately capped and
  requeued without clearing pending ownership.
- New stats/log/parser fields:
  `reservationApply=limit/applied/deferred`,
  `midReservationApplyLimit`, `midReservationApplied`,
  `midReservationApplyDeferred`.
- New env/harness knob:
  `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY`,
  `perf_noncapture_smoke.ps1 -MidClipmapAsyncVisibleReservationMaxApply`.
- `perf_noncapture_smoke.ps1` writes `run_manifest.txt` for exact command
  reproducibility.

Validation after the retained slice:

- `.\build.ps1 -Config Release`: passed, with pre-existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1 -ParseOnly` passed on
  `build/captures/highalt_predicted_deadline_ticket512_resapply8_footprint_20260606`.

Probe caution:

- `highalt_predicted_deadline_ticket512_resapply8_footprint_20260606` is not a
  candidate: f402/f403 stayed `88/88`, but the window was `108.41/150.82 ms`,
  async visible stayed `0`, and `reservationApply=8/0/0`, so the cap did not
  actually exercise reservation apply deferral.
- Reconstructed full-stack probes with and without the cap
  (`highalt_predicted_deadline_ticket512_resapply8_fullstack_20260606`,
  `highalt_predicted_deadline_ticket512_defaultapply_fullstack_20260606`) both
  stayed startup-held through frame 400. They are invalid public comparisons and
  show why the new manifest is needed.
- Next step is to reproduce
  `highalt_predicted_deadline_ticket512_currentfirst_20260606` with a
  manifest-bearing command, then tune reservation apply caps against that exact
  baseline.

Follow-up:

- Added `perf_noncapture_smoke.ps1 -StackPreset highalt-currentfirst` with
  managed startup public-render proof env and manifest `effectiveParameters`.
- Fixed the async-visible apply gate in `src/main_launcher.cpp`: completed
  async-visible work can now apply after the prepump has projected visible
  ownership, even if the prepump is not allowed to drive the sync pump budget.
- Retained validation artifact:
  `build/captures/highalt_currentfirst_preset_applysafe_20260606`.
  f402 now applies pending async-visible results (`0/24/24/0/0`) instead of
  leaving them blocked when `active=0`.
- Cap probes are rejected as candidates:
  - cap 8:
    `build/captures/highalt_currentfirst_preset_applysafe_resapply8_20260606`,
    deferral exercised but window stayed about `145 ms` and f402/f403 coverage
    slipped to `90/89`;
  - cap 16:
    `build/captures/highalt_currentfirst_preset_applysafe_resapply16_20260606`,
    window regressed to about `207 ms`.
- Keep the throttle counters/knob and the apply-safety fix. Next work should
  pace reservation promotion through upload/surface/publish ownership stages,
  because apply-only caps shift the bottleneck rather than solving it.

Build/test after removal:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warnings: `main_launcher.cpp` local `rayDir` shadow warnings.

## 2026-06-06 Low-Lane Admission Backpressure Rejection

Attempted and removed a second default-off branch at the request/admission layer:

- `VENPOD_SPARSE_LOW_LANE_ADMISSION_BACKPRESSURE*`
- `perf_noncapture_smoke.ps1 -LowLaneAdmissionBackpressure*`

The branch skipped nonresident prefetch-lane admissions when the previous frame's prefetch generation queue exceeded a threshold. It intentionally did not skip resident touches, public/visible streaming lanes, terrain-critical, collision, or edited requests.

Artifacts:

- threshold4096: `build/captures/low_lane_admission_backpressure_walk120_accepted_20260606`
- threshold8192: `build/captures/low_lane_admission_backpressure_t8192_walk120_accepted_20260606`

Matched short-walk rows:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| admission threshold4096 | `96.24` | `72.45` | `17.09` | `21.15` | `34.19` | `19.05` | `17.76` | `6.44` | `3262` | `106.63/231.33` | rejected/removed |
| admission threshold8192 | `118.85` | `75.91` | `23.47` | `26.03` | `26.38` | `24.67` | `18.74` | `6.37` | `6544` | `102.88/125.35` | rejected/removed |

Decision:

- The scalar admission guard was the correct layer to test after queued trim failed, but it still moved cost into request/generation/clip/pump/surface/frame gaps.
- Reducing prefetch backlog alone is not predictive of frame-time improvement.
- Do not reintroduce backlog-threshold low-lane admission skipping as a main path.

Build/test after removal:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Live-code symbol search for both rejected branches: clean.

Current architecture conclusion:

- The next real branch should not be another scalar trim/admission knob.
- Implement an ownership-ticket scheduler/state slice: admission budgets by ownership class, explicit per-stage reservations, and the same lane/proof state carried through generation, upload/apply, surface extraction, and publish readiness.
- Public-critical and sampled-visible readiness must have a completion contract; cache/prefetch must consume only leftover stage budgets rather than entering the same visible-class workstream and being cleaned up later.

## 2026-06-06 Ownership Generation Stage Budget Scaffold

Retained a default-off scaffold that extends ownership-stage budgets into CPU generation:

- `SparseVoxelWorld::PumpGenerationAroundForOwnershipCritical(...)`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGETS`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGET`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudgets`
- new perf line: `PERF_SPARSE_OWNERSHIP_GENERATION_BUDGETS`

Behavior boundary:

- Existing `-OwnershipStageBudgets` still gates upload/surface behavior.
- Generation-stage splitting requires the new explicit `-OwnershipStageGenerationBudgets` switch.
- Defaults remain unchanged.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing `rayDir` shadow warnings remain.

Artifacts:

- critical-full generation split:
  `build/captures/ownership_stage_generation_budget_walk120_accepted_20260606`
- reserved noncritical generation split:
  `build/captures/ownership_stage_generation_budget_reserved_walk120_accepted_20260606`

Matched short-walk rows:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| gen stage, critical-full | `113.78` | `71.88` | `20.19` | `22.60` | `29.06` | `13.28` | `16.14` | `17.94` | `9150` | `126.53/218.06` | rejected config |
| gen stage, noncritical reserve 8 | `117.99` | `74.87` | `25.65` | `24.63` | `24.57` | `14.57` | `14.31` | `12.82` | `8286` | `128.74/255.02` | rejected config |

Decision:

- Retain the separated default-off generation-stage hook as architecture scaffolding, not as a candidate.
- Do not include `-OwnershipStageGenerationBudgets` in the strongest stack.
- A boolean critical/noncritical split is too blunt; it either starves broad prefetch progression or still moves cost into other CPU/gap buckets.

Next implementation target:

- Add ticket ownership counts for the generation queue, split by `publicCritical`, `sampledVisible`, `hiddenRepair`, `fallbackValid`, `prefetch`, `cache`, and `unknownCritical`.
- Use those counts to allocate explicit generation quotas by ownership class before pumping, instead of a binary critical/noncritical split.

## 2026-06-06 Ownership Generation Budget Final Rejection / Pending Ownership Accounting

This supersedes the earlier "Ownership Generation Stage Budget Scaffold" section. The generation-stage ownership budget path was removed after the explicit quota variants also regressed. Do not include or reintroduce it as the next main fix.

Removed/rejected behavior surface:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGETS`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGET`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudgets`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudget`
- temporary generation helpers for ownership-critical, ticket-ownership, and explicit quota pumping

Rejected short-walk probes against the accepted baseline:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| critical-full generation split | `113.78` | `71.88` | `20.19` | `22.60` | `29.06` | `13.28` | `16.14` | `17.94` | `9150` | `126.53/218.06` | rejected/removed |
| noncritical reserve 8 | `117.99` | `74.87` | `25.65` | `24.63` | `24.57` | `14.57` | `14.31` | `12.82` | `8286` | `128.74/255.02` | rejected/removed |
| explicit multi-call quotas | `136.73` | `90.79` | `28.99` | `34.85` | `26.93` | `10.76` | `16.64` | `22.87` | `8801` | `145.25/242.05` | rejected/removed |
| single-pass quotas | `114.57` | `74.53` | `24.23` | `23.33` | `26.96` | `10.87` | `17.45` | `6.61` | `8546` | `150.86/869.49` | rejected/removed |

Retained low-cost accounting:

- `PERF_SPARSE_STREAMING_TICKETS` logs `genPendingOwnership=public/sampled/hidden/cache/prefetch/fallback/unknown:...`.
- It counts existing streaming tickets with pending CPU generation inside the already-existing ticket stats loop. It is intentionally not a generation-queue scan.
- A direct queue-scan accounting probe was rejected because it was not performance-neutral:
  `build/captures/generation_queue_ownership_accounting_walk120_accepted_20260606`,
  f120 raw `141.75`, window avg/max raw `132.77/159.66`.

Accepted final accounting artifact:

- `build/captures/generation_pending_ownership_accounting_walk120_accepted_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `71.74/39.64/13.52/10.44/15.67/4.69/9.59/14.24`
- window f96..124 avg/max raw: `72.58/126.17`
- f120 `genPendingOwnership`:
  `public/sampled/hidden/cache/prefetch/fallback/unknown:0/127/0/0/8608/0/0`
- f120 surface-ready publish pending `0`, pending lanes `0/0/0/0`

Validation after cleanup:

- `perf_noncapture_smoke.ps1` parse check: passed.
- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

Current architecture conclusion:

- Repeated scalar knobs are now well covered and rejected: front-sort, low-lane queued trim, low-lane admission backpressure, binary generation ownership split, and explicit generation quota pumping all moved cost or regressed windows.
- Pending CPU-generation demand is mostly prefetch plus a small sampled-visible slice, but the cost is not solved by starving prefetch. The engine needs durable ownership demand/reservation across request, generation, upload/apply, surface extraction, and publish readiness.
- Next viable code should avoid queue rescans/re-sorts in hot paths. Use ticket-owned stage demand, per-stage reservations, and completion contracts so public/sampled work is guaranteed while cache/prefetch consumes leftover capacity without catch-up churn.

## 2026-06-06 Persistent Moving-Window Reservation / Parallel Pump Validation

Current retained code state:

- Persistent async visible reservations are map-owned and deadline/sample ordered in `SparseClipmapTileCache`.
- Moving-window reservations are queued from the launcher under default-off envs.
- Startup config logging now includes parallel-pump and column-cache config fields:
  `parallelPump`, `persistentParallelPump`, `parallelPumpWorkers`, `parallelPumpMin`,
  `sharedColumnCache`, `directFootprint`, `parallelWorkerColumnCache`.

Removed/rejected in the same branch:

- Current-visible async prequeue was removed. First ordering starved moving-window reservations; reservation-first ordering fail-fast crashed.
- Async-dominant sync throttling was removed after starvation/crash evidence.

Important validation correction:

- `build/captures/pp480_20260606` is **not** a valid public-frame comparison despite f480 raw `27.68 ms`.
- It was private/startup-held at f480:
  `SPARSE_STARTUP_PUBLIC_RENDER_HELD`, `active=0x5DF`, ownership off, surface raster off, `gpuRay=0`.
- Future comparisons must include startup public moving-window proof envs as well as mid-clipmap moving-window priority envs.

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` shadow warnings only.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- config/activation probe: `build/captures/ppcfg_20260606`
- invalid private-held run: `build/captures/pp480_20260606`
- comparable public-open run: `build/captures/pppublic480_20260606`

Public-open f480 result (`pppublic480_20260606`):

| Metric | Value |
|---|---:|
| target raw/body | `83.83 / 78.30 ms` |
| window f456..484 avg/max raw | `76.35 / 86.48 ms` |
| window avg/max body | `86.06 / 104.97 ms` |
| CPU/request/gen/clip | `41.24 / 15.61 / 15.66 / 9.96 ms` |
| pump/mainThreadBrickGen | `5.60 / 5.60 ms` |
| surface extract/stage | `14.65 / 2.31 ms` |
| GPU frame/ray | `25.30 / 20.55 ms` |
| visible coverage / missing visible / missing cache | `99 / 11 / 2486` |
| movingWindow samples/ready/missing/coords/coverage/reserve/queued | `3/0/356/252/94/252/0` |
| asyncVisible enq/complete/apply/drop/dup | `11/16/16/0/0` |
| parallelPump active/bricks/workers/wallMs | `1/12/4/5.55` |

Decision:

- Parallel mid pump is now verified to activate when configured; the prior `parallelPump=0` run was ambiguous because the startup config was not logged and the public proof/env mix differed.
- It is useful but not sufficient. The current comparable public-frame rough FPS is about `12 FPS` at f480 and about `13 FPS` by local raw-window average, not 60 FPS.
- Compared with earlier public f480 rows around `108-145 ms`, the trajectory is favorable, but remaining cost is spread across request/generation/clip, surface extraction/stage, GPU ray/surface, and frame gaps.

Next measured blocker:

- Work from `pppublic480_20260606`, not the invalid private-held row.
- Reduce public-frame surface extraction and GPU ray/surface cost while preserving the moving-window ownership contract.
- Keep startup public moving-window proof enabled when comparing motion captures.

### Rejected hidden-critical prepublish surface cap

Tried a targeted default-off branch after the public-open surface evidence showed f480 pre-publish surface extraction was mostly hidden repair:

- temporary env: `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PREPUBLISH_CRITICAL_SURFACE_MAX`
- temporary perf line: `PERF_SPARSE_PRE_PUBLISH_SURFACE_HIDDEN_CAP`
- probe artifact: `build/captures/hiddenprep16_pppublic480_20260606`

It was removed after validation.

| Probe | f480 raw/body | window avg/max raw | surfaceExtract | pump/mainThreadBrickGen | missing visible | visible coverage | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| public-open baseline | `83.83/78.30` | `76.35/86.48` | `14.65` | `5.60/5.60` | `11` | `99` | retained |
| hidden prepublish cap 16 | `96.83/91.07` | `100.26/132.79` | `12.09` | `10.48/10.48` | `62` | `98` | rejected/removed |

Why it failed:

- The cap did fire (`cap=16`, hidden critical extracted `16`, skipped `1` at f480).
- It only shaved about `2.6 ms` from surface extraction, while mid visible debt and pump cost increased and the local raw window worsened.
- Ownership miss/unsafe remained clean, but `lodParentHeld=1` and missing visible rose, so it shifted pressure rather than improving public-frame playability.

Updated direction:

- Do not reintroduce a local post-open hidden surface cap as the main fix.
- Hidden repair needs to be classified earlier as a maintenance/repair ownership lane and carried through request, generation, upload/apply, surface extraction, and publish budgets.

Validation after removing the branch:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Rejected symbol search: clean.

## 2026-06-06 Streaming Ticket Stage Demand Accounting Scaffold

Implemented a default-off ownership architecture scaffold:

- `SparseVoxelWorldConfig::streamingTicketStageDemandAccounting`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_DEMAND_ACCOUNTING=0`
- `perf_noncapture_smoke.ps1 -StreamingTicketStageDemandAccounting`
- maintained `m_streamingTicketPendingStageOwnershipCounts[stage][ownership]`

Intent:

- Keep durable per-stage ownership demand inside the ticket lifecycle.
- Update demand counts on ticket touch, ownership promotion, stage completion, and removal.
- Leave accepted-stack behavior unchanged unless explicitly enabled.

Rejected during this slice:

- A protected-sort guard based on stage-demand counters. It changed later-stage queue ordering semantics and regressed:
  `build/captures/ticket_stage_demand_accounting_walk120_accepted_20260606`,
  f120 raw `213.29`, window avg/max `119.10/213.29`.
- The guard was removed.

Validation:

- `perf_noncapture_smoke.ps1` parse check: passed.
- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known pre-existing `rayDir` shadow warnings remain.

Default-off accepted-stack probe:

- artifact: `build/captures/ticket_stage_demand_defaultoff_walk120_accepted_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `68.00/44.17/18.18/20.21/5.74/4.41/8.80/12.14`
- window f96..124 avg/max raw: `71.03/81.06`
- `genPendingOwnership`: `48/147/0/0/8040/0/0`
- surface-ready publish pending `0`

Enabled scaffold probe:

- artifact: `build/captures/ticket_stage_demand_enabled_walk120_rejected_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `69.74/41.61/14.92/10.50/16.18/5.93/7.84/12.34`
- window f96..124 avg/max raw: `69.63/86.72`
- `genPendingOwnership`: `0/131/0/0/8401/0/0`
- surface-ready publish pending `0`

Decision:

- Keep stage-demand accounting default-off as scaffolding, not as a candidate or FPS win.
- Do not use demand counters to skip protected sorting without a stricter queue-order equivalence proof.
- Next implementation should consume this demand model in a real reservation path: public/sampled completion guarantees plus leftover prefetch/cache capacity, without scanning/sorting giant queues each frame.

## 2026-06-06 Generation Ownership Worklist Scaffold

Implemented a default-off ownership primitive:

- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipQueues`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES=0`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipQueues`
- persistent generation worklists:
  `m_generationOwnershipWorklists[StreamingTicketOwnership]`
- indexed ownership entry table:
  `m_generationOwnershipWorklistEntries`

Intent:

- Provide a future generation-stage reservation source without rescanning `m_generationQueue`.
- Keep accepted-stack behavior unchanged unless explicitly enabled.

Validation result:

- Ungated alias maintenance was rejected:
  `build/captures/generation_ownership_alias_scaffold_default_walk120_accepted_20260606`,
  f120 raw `142.62`, window avg/max `125.19/171.39`.
- Alias maintenance is now gated by `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES`.
- Deque-backed active alias maintenance was also rejected:
  `build/captures/generation_ownership_alias_enabled_walk120_rejected_20260606`,
  f120 raw `86.95`, window avg/max `80.96/99.23`.
- The current implementation replaces the deque aliases with vector worklists plus a `BrickCoord -> {ownership,index}` table. Normal removal is swap-remove O(1), with fallback search only for inconsistent entries.

Build/test after indexed replacement:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing `rayDir` shadow warnings remain.

Default-off indexed probe:

- artifact:
  `build/captures/generation_ownership_indexed_defaultoff_walk120_accepted_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `61.18/60.44/18.51/16.19/25.73/14.60/9.00/12.75`
- window f96..124 avg/max raw: `80.70/283.05`
- `genPendingOwnership`: `48/132/0/0/8302/0/0`
- surface-ready publish pending `0`

Enabled indexed probe:

- artifact:
  `build/captures/generation_ownership_indexed_enabled_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `76.09/44.95/17.32/14.18/13.43/12.14/8.01/13.63`
- window f96..124 avg/max raw: `65.96/76.09`
- `genPendingOwnership`: `48/139/0/0/8448/0/0`
- surface-ready publish pending `0`
- not accepted as a candidate by itself because no reservation scheduler consumes it yet.

Decision:

- Keep generation ownership worklists default-off as scaffolding only.
- Do not claim this as a candidate or FPS win yet; the default-off repeat was spike-noisy and the enabled path is not consumed by a reservation scheduler.
- Do not revive the rejected `std::deque` alias/remove-all approach.
- Next stronger architecture direction: build a gated generation reservation pump that consumes the indexed worklists directly, guarantees public/sampled ownership first, and spends leftover capacity on prefetch/cache. Validate that it improves motion stability without increasing public missing, surface-ready backlog, or raw frame spikes.

## 2026-06-06 Generation Ownership Reservation Pump Attempt

Implemented a second default-off switch that consumes the indexed generation ownership worklists before the existing queue/class fallback:

- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipReservations`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipReservationMax`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATIONS=0`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATION_MAX=64`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipReservations`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipReservationMax <n>`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing `rayDir` shadow warnings remain.

Uncapped protected reservation probe:

- artifact:
  `build/captures/generation_ownership_reservations_protected_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `86.69/71.34/19.84/13.27/38.21/36.94/9.04/11.83`
- window f96..124 avg/max raw: `74.09/99.98`
- generated lanes: cache/prefetch/visible/public `0/0/66/48`
- `genPendingOwnership`: `48/106/0/0/8109/0/0`
- surface-ready publish pending `0`

Protected reservation capped at 48:

- artifact:
  `build/captures/generation_ownership_reservations_cap48_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `91.74/68.62/18.70/13.16/36.74/35.40/9.04/11.34`
- window f96..124 avg/max raw: `75.34/101.85`
- generated lanes: cache/prefetch/visible/public `0/0/66/48`
- `genPendingOwnership`: `48/103/0/0/8103/0/0`
- surface-ready publish pending `0`

Decision:

- Reject the reservation pump as an active candidate. Keep it default-off only as a measured branch.
- Capping the pre-pass does not restore prefetch/cache progress because the fallback protected sort continues consuming visible/public after the cap.
- Next scheduler should be integrated, not "protected pre-pass plus old fallback": explicit public/sampled minimums, bounded visible repair, then prefetch/cache fill, all from indexed worklists or equivalent O(1) buckets.

## 2026-06-06 Generation Ownership Share Scheduler Attempt

Implemented an integrated default-off scheduler over the indexed ownership worklists:

- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipShareScheduler`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipSharePublicMin`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipShareVisibleMax`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipSharePrefetchMin`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipShareVisibleDebtGate`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_SCHEDULER=0`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PUBLIC_MIN=48`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_MAX=32`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_PREFETCH_MIN=32`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_SHARE_VISIBLE_DEBT_GATE=160`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing `rayDir` shadow warnings remain.

Key probes:

- default-off safety:
  `build/captures/generation_ownership_share_defaultoff_walk120_accepted_20260606`,
  f120 raw `92.84`, window avg/max `76.83/99.07`, generated lanes `0/0/55/48`.
- fixed-share before quota scaling:
  `build/captures/generation_ownership_share_default_walk120_20260606`,
  f120 raw `71.82`, window avg/max `76.64/125.33`, generated lanes `0/0/53/48`.
- scaled-share:
  `build/captures/generation_ownership_share_scaled_walk120_20260606`,
  f120 raw `57.36`, window avg/max `84.00/105.02`, generated lanes `0/15/36/48`,
  async prefetch enqueued/applied `18/15`, `criticalMissing=223`.
- visible-debt-gated share:
  `build/captures/generation_ownership_share_debtgate160_walk120_20260606`,
  f120 raw `82.86`, window avg/max `73.64/127.68`, generated lanes `0/11/37/48`,
  async prefetch enqueued/applied `18/11`, `criticalMissing=88`, surface-ready pending `0`.

Decision:

- Reject the share scheduler as an active candidate. Keep it default-off as a measured architecture branch.
- It proved the indexed worklists can allocate cross-ownership CPU generation without rescanning, but low-priority prefetch generation creates downstream surface/publish work and frame spikes.
- Next direction should not tune generation shares alone. Prefetch/cache need lane-aware downstream staging: likely CPU-generated first, with upload/surface/publish deferred or separately budgeted until visible debt and frame budget permit.

## 2026-06-06 Low-Priority Downstream Deferral Attempt

Implemented a default-off downstream staging gate:

- `SparseVoxelWorldConfig::streamingTicketLowPriorityDownstreamDeferral`
- `SparseVoxelWorldConfig::streamingTicketLowPriorityDownstreamPromoteMax`
- `VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_DEFERRAL=0`
- `VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_PROMOTE_MAX=16`
- `perf_noncapture_smoke.ps1 -StreamingTicketLowPriorityDownstreamDeferral`
- `perf_noncapture_smoke.ps1 -StreamingTicketLowPriorityDownstreamPromoteMax <n>`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing `rayDir` shadow warnings remain.

Key probes:

- share `48/32/32`, gate `160`, promote `4`:
  `build/captures/generation_share_downstream_deferral_promote4_walk120_20260606`,
  f120 raw `51.36`, window avg/max `74.96/99.04`,
  generated lanes `0/16/35/47`, async prefetch `18/16`, `criticalMissing=210`.
- share `48/64/8`, gate `160`, promote `4`:
  `build/captures/generation_share_visible64_prefetch8_downstream_deferral_promote4_walk120_20260606`,
  f120 raw `68.79`, window avg/max `71.61/95.45`,
  generated lanes `0/12/36/48`, async prefetch `12/12`, `criticalMissing=113`.
- share `48/64/4`, gate `160`, promote `2`:
  `build/captures/generation_share_visible64_prefetch4_downstream_deferral_promote2_walk120_20260606`,
  f120 raw `66.77`, window avg/max `75.03/156.26`,
  generated lanes `0/11/45/47`, async prefetch `11/11`, `criticalMissing=99`.

Decision:

- Reject low-priority downstream deferral as an active candidate for now. Keep it default-off as architecture scaffolding.
- Best aligned probe is `48/64/8 + promote4`: it moves a small prefetch trickle and reduces the share-scheduler spike profile, but still loses to the indexed-worklist scaffold (`65.96/76.09`) and is not stable enough.
- Next direction: add explicit observability/backlog accounting for deferred `GeneratedCPU` downstream work, then integrate upload/surface/publish budgets by ownership. Do not keep sweeping local share/promotion values without that visibility.

## 2026-06-05 Clean-Harness / Parallel Mid Pump Validation

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained cleanup:

- `perf_noncapture_smoke.ps1` no longer forces `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`.
- New explicit harness switch: `-MidClipmapVisibleCriticalPrepump`.
- `src/main_launcher.cpp` skips projected missing-brick scan/priority tagging unless prepump can actually affect high-alt, visible priority/lane guard is enabled, or missing-sample feedback is explicitly enabled.

Clean validation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/walk_lane_diagnostics_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/walk_parallel_mid_pump_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/validated_parallel_mid_stack_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/highalt_visible_critical_prepump_clean_20260605`
- visual check: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605/contact_sheet.png`

Key rows:

| Stack | Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| clean harness control | walk realtime | `600` | `63.51` | `55.81` | `12.94` | `16.70` | `26.15` | `14.31` | `11.01` | `12.22` | measured true blocker after cleanup |
| parallel mid pump | fixed | `384` | `23.80` | `22.31` | `4.50` | `17.00` | `0.80` | `0.00` | `20.87` | `6.04` | partial, not playable |
| parallel mid pump | walk realtime | `600` | `59.51` | `35.60` | `15.40` | `14.67` | `5.51` | `4.01` | `11.32` | `12.95` | strongest current clean walk row |
| parallel mid pump | high-alt | `398` | `51.66` | `26.21` | `4.94` | `5.18` | `16.09` | `9.74` | `3.78` | `10.25` | high-alt still blocked |

Rejected in this clean cycle:

- `MidInterestInterval=2`: walk raw/CPU `91.39/125.58`.
- `-TerrainCriticalParallelGeneration` with parallel mid: activated but worsened walk to raw/CPU `63.32/48.43`.
- `-ParallelMidWorkerColumnCache` with parallel mid: CPU slightly lower but raw/window worse than plain parallel mid.
- high-alt projected visible-critical prepump/cache defer: activated and proved broad cache debt, but high-alt window regressed and stayed above `50 ms`.

Visual verdict:

- Walk capture smoke passed for frames `560/580/600`.
- No obvious white terrain holes or sky leaks in the contact sheet.
- Terrain is still visibly coarse/blocky, so this is not a visual/playability victory.

Current decision:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0` remains default-off and is a partial only.
- All defaults remain unchanged.
- The remaining blocker is distributed request/exact generation/surface extraction/GPU/high-alt clip debt.
- Next real implementation must carry ownership lanes through request, generation, upload/apply, surface extraction, and publication rather than adding another isolated cap/thread/scheduler toggle.

## 2026-06-05 Goal Charter / Window Validation Anchor

Active tool goal:

```text
streaming_playability_real_fix_campaign_20260604
```

The active goal remains open. Do not replace it with a narrow one-pass diagnostic. Completion requires either a validated 60 FPS candidate, a strong default-off candidate with exact remaining blocker table, a hard architecture blocker after plausible branches are attempted/ruled out, or a hard build/tool blocker.

Default-neutral validation infrastructure added:

- `VENPOD_PERF_FRAME_END_LOG_INTERVAL`, unset by default and behavior-preserving.
- `perf_noncapture_smoke.ps1 -FrameEndLogInterval`.
- `perf_noncapture_smoke.ps1` now writes `window_summary.csv` and `window_table.md` from `PERF_FRAME_END`.

Latest accepted-stack validation:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_framewindow_20260605`

Target-frame rows:

| Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract | GPU | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `19.58` | `17.89` | `3.82` | `13.13` | `0.93` | `0.00` | `20.60` | `5.67` | near but still over 60 FPS with burst tail |
| walk realtime f600 | `58.94` | `48.30` | `12.55` | `12.87` | `22.86` | `11.62` | `9.78` | `17.85` | not playable |
| high-alt f360 | `48.21` | `30.41` | `8.96` | `4.78` | `16.66` | `6.45` | `8.13` | `5.99` | not playable |

Window rows (`target-24` through `target+4`):

| Scenario | Avg raw | Max raw | Avg surface extract | Max surface extract | Avg upload | Avg post-wait | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed | `22.32` | `49.28` | `6.26` | `20.60` | `0.37` | `10.80` | exact/surface bursts remain |
| walk realtime | `107.87` | `1290.46` | `9.62` | `10.62` | `6.33` | `23.22` | contains one frame-601 gap outlier; target row still too slow without it |
| high-alt | `49.77` | `60.30` | `5.90` | `8.61` | `1.08` | `11.94` | clip/pump and steady gaps remain |

Important correction:

- `frameend_interval_validation_20260605` used the mixed persistent-mid/interest-2 probe stack. It is not the accepted-stack baseline.
- Resume next implementation decisions from `accepted_stack_framewindow_20260605`.

Next engineering branch:

- Do not retry scalar trims, free-page guard, surface-ready queue, persistent surface workers, persistent mid+interest2, hidden-exact deferral, or direct hidden-exact parallel toggles as the main fix.
- The remaining fix needs a lane-aware streaming state slice or a clearly behavior-preserving pipeline optimization chosen from the accepted-stack table.

## 2026-06-05 Surface Worker Rejection / Current Blocker

Attempted a default-off persistent surface extraction worker pool behind the existing parallel surface extraction path. It was removed after validation because representative CPU/clip regressions outweighed mixed raw improvements.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch16_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch8_candidate_20260605`

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| baseline persistent exact 2w | `21.81/17.17` | `67.23/32.40` | `51.21/28.80` | current partial |
| surface persistent 4w | `18.90/18.22` | `67.16/42.00` | `50.49/29.60` | rejected |
| surface persistent 4w batch16 | `21.84/19.62` | `60.95/44.43` | `47.96/30.80` | rejected |
| surface persistent 4w batch8 | `22.92/25.86` | `64.86/52.48` | `51.09/27.99` | rejected |

Diagnostics-off and bounded64 comparison:

- `accepted_stack_noheavydiag_20260605`: heavy diagnostics are not the first blocker; walk remains about `63.72 ms` raw and high-alt about `52.95 ms` raw.
- `bounded64_noheavydiag_candidate_20260605`: bounded64 remains default-off/comparison-only and is not a CPU fix.

Current blocker:

- isolated thread/cap/queue knobs have now repeatedly failed or moved cost;
- the next real implementation is an ownership-aware streaming state machine carrying public-visible, sampled-visible, hidden repair, cache, and prefetch lanes through request, generation, upload/apply, surface extraction, and publish;
- unknown fallback remains critical; async/worker generation is only safe for cache/prefetch or CPU-proven fallback-valid lanes.

Generated: 2026-06-04

Current active goal:

```text
streaming_playability_real_fix_campaign_20260604
```

Objective:

Drive VENPOD from the current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes. Do not stop after one diagnostic, one failed heuristic, or one partial win. Preserve public-frame ownership correctness.

This file is the campaign handoff for surviving compaction. Update it after every meaningful validation/fix cycle. Older VENPOD rendering history remains below this section.

## Active Operating Contract - 2026-06-05

The active `/goal` is already set:

```text
streaming_playability_real_fix_campaign_20260604:
drive VENPOD from current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes, with explicit anti-stop rules, persistent handoff updates, and no correctness weakening or default promotion without proof.
```

This is the continuity contract for every resumed turn:

- Do not stop after adding diagnostics, rejecting one prototype, or finding the next obvious bottleneck.
- Do not stop with a plan when the next measured implementation branch is safe and local enough to attempt.
- After every branch, classify it as retained, rejected/removed, or diagnostic-only, then continue if the next measured blocker is obvious.
- Keep `handoff.md` updated before any final response or long pause; keep `debug-handoff.md`, `root.md`, and `debug.md` synchronized after meaningful validation/fix cycles.
- A final response is only valid when it reports a validated candidate, a strong candidate with exact remaining blocker table, a hard architecture/tool blocker, or a user-requested pause/status.

Non-negotiable correctness guard:

- Every visible pixel must have a legitimate owner: exact sparse surface, valid mid voxel clipmap, valid Far-SVO, deterministic water, or sky.
- Unknown fallback is not a valid owner.
- Do not treat `missScreenPct=0` / `unsafeNearMissPct=0` as success while frame time or visual coherence is still bad.
- Do not weaken foreground/public ownership policy, lower coverage guards to fake CPU wins, suppress Far-SVO ownership, revive the blunt pump cap, revive the age-priority queue, revive the failed ring-only visible-critical heuristic, or reintroduce the shader hash-bucket feedback path.

Defaults that must remain unchanged unless behavior is exactly preserved:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0`
- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0`
- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=0`
- `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=0`
- `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`

Current best candidate stack for validation remains default-off:

- background split: `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1`, `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375`
- clean prefetch throttle: `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1`
- explicit source lanes: `VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES=1`
- lane queue priority / diagnostics as used by `perf_noncapture_smoke.ps1`
- parallel mid worker column cache / terrain-critical parallel generation where measured safe
- bounded foreground `bound=64` is comparison-only and must not be defaulted.

Immediate measured implementation branch after compaction:

1. Continue from the publish/surface lane evidence, not from a new broad investigation.
2. Inspect `src/main_launcher.cpp` around pre-publish surface extraction, surface-ready publish queue, and publish lane stats.
3. Implement one default-off lane-aware pre-publish/surface readiness slice only if it can protect public/visible publishes while limiting hidden/cache/prefetch catch-up debt.
4. If that slice shifts debt into pending surface/publish queues or regresses walk/high-alt, reject/remove it and pivot to the next measured lane: request/generation/clip or surface staging, based on the fresh noncapture table.
5. Validate fixed, walk realtime/long, high-alt, and noncapture rows with VSync off; record raw/body/CPU/GPU, request/gen/clip/pump/surface, lane queue/backlog, missing/coverage, and visual notes.

## Campaign Resume - Visible Cache And Inline Surface Rejections - 2026-06-05

Active goal remains:

```text
streaming_playability_real_fix_campaign_20260604
```

Latest build/test before this resume:

- `.\build.ps1 -Config Release`: passed after cleanup.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Fresh baseline artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/inline_generated_surface_baseline_vsync0_20260605`

Fresh baseline rows, using background split `0.375`, clean throttle, critical reuse, terrain critical parallel generation, surface extraction max `1`, incremental pressure trim `16k`, explicit source lanes, lane queue priority, worker column cache, and mid interest detail:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing/sample | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|
| fixed | `384` | `21.97` | `21.93` | `4.28` | `16.56` | `1.08` | `0.00` | `20.10/2.24` | `6.78` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `88.21` | `49.84` | `12.79` | `13.04` | `23.99` | `21.59` | `9.42/2.82` | `17.98` | `563/378` | `93` | `0/0` |
| high-alt | `360` | `52.57` | `32.26` | `7.57` | `6.91` | `17.77` | `5.63` | `4.05/2.21` | `10.84` | `2231/0` | `99` | `0/0` |

Rejected and removed branch:

- Inline generated surface extraction for newly generated sparse pages.
- Temporary symbols removed: `VENPOD_SPARSE_SURFACE_INLINE_GENERATED`, `SurfaceInlineGenerated`, `TryInlineGeneratedSurface`, `PERF_SPARSE_SURFACE_INLINE_GENERATED`.
- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/inline_generated_surface_candidate_vsync0_20260605`

Rows:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---|
| fixed | `384` | `25.10` | `40.16` | `4.86` | `34.46` | `0.83` | `0.00` | `1.19/2.29` | n/a | rejected; moved surface work into generation/CPU |
| walk realtime | `600` | `96.91` | `112.71` | `21.86` | `39.10` | `51.74` | `33.69` | `3.31/3.59` | n/a | rejected regression |
| high-alt | `360` | `52.23` | `35.95` | `8.46` | `10.16` | `17.32` | `5.26` | `1.42/1.73` | n/a | not a win |

Rejected/default-off diagnostic branch:

- Full visible-lane/cache-only deferral with `-MidClipmapVisibleLaneGuard -MidClipmapVisibleCoverageGuardV2 -MidClipmapCacheOnlyDefer`.
- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visible_critical_existing_stack_vsync0_20260605`
- It classified all missing bricks as cache in fixed/walk/high-alt, deferred `3586` bricks indefinitely, left broad mid coverage at `61`, and drove request cost to about `41 ms`.
- Decision: reject this combination. It is a fake win because it hides debt behind unbounded cache backlog and does not improve raw frame time.

High-alt-only visible-cache check:

- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visible_cache_highalt_only_vsync0_20260605`
- This omitted the global lane guard. Fixed/walk stayed closer to baseline, and high-alt pump dropped `5.63 -> 1.82 ms`, but raw high-alt was essentially unchanged (`52.57 -> 52.28 ms`) and walk regressed (`88.21 -> 94.90 ms`).
- Decision: diagnostic-only; not a retained playable stack member.

Budget-compatible parallel mid pump retest on the current stack:

- Artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_current_stack_vsync0_20260605`
- Parallel pump activated in walk (`8` bricks, `4` workers, `2.23 ms` wall), but walk CPU regressed to `60.51 ms` because request/gen/surface rose. High-alt also did not improve materially.
- Decision: keep the existing flag default-off as a partial candidate from earlier rows, but this current-stack retest is not accepted as a new win.

Current conclusion:

- Incremental scroll/reuse is not the representative walk blocker in the latest baseline: walk frame `600` reported `newV=0`, `goneV=0`, and reused interest, while serial visible voxel pump generated `24` old-debt bricks at `21.59 ms`.
- Walk/realtime debt is still mostly projected-visible and fallback-invalid/unknown, so it cannot be moved async or treated as cache without an ownership proof.
- High-alt has over-broad unsampled cache debt, but the safe projected/cache deferral path only cuts a few milliseconds of pump and does not move raw frame time enough.
- The next real implementation slice is not another mid pump queue tweak. It is an ownership-aware exact/surface pipeline split: public-critical request/generation/surface publish stays guarded, while repair/cache/prefetch exact and surface work gets separate queues, budgets, async generation/extraction, and frame-boundary apply accounting.

## Campaign Continuation - Surface Lane Budget Rejected - 2026-06-05

Active goal remains:

```text
streaming_playability_real_fix_campaign_20260604
```

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Rejected and removed branch 1:

- hidden post-open pre-publish surface cap
- temporary envs:
  - `VENPOD_SPARSE_PRE_PUBLISH_SURFACE_LANE_BUDGETS`
  - `VENPOD_SPARSE_PRE_PUBLISH_HIDDEN_POST_OPEN_SURFACE_BUDGET`
- artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/prepublish_lane_budget_vsync0_20260605`

Rows:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---|
| fixed | `384` | `22.37` | `21.65` | `3.95` | `16.94` | `0.74` | `0.00` | `19.19/1.91` | `6.09` | `0` | `100` | `0/0` | not enough |
| walk realtime | `600` | `74.88` | `52.92` | `10.07` | `11.64` | `31.20` | `28.70` | `9.57/2.65` | `16.87` | `453` | `94` | `0/0` | rejected regression |
| high-alt | `360` | `49.68` | `34.30` | `8.61` | `7.44` | `18.23` | `5.66` | `4.60/2.18` | `9.58` | `2178` | `99` | `0/0` | not enough |

Finding:

- The hidden-only cap did not fix surface cost. General surface extraction immediately refilled the frame, so the branch shifted work rather than solving it.

Rejected and removed branch 2:

- combined hidden post-open cap plus low-lane surface extraction budget
- temporary envs:
  - `VENPOD_SPARSE_SURFACE_LANE_BUDGETS`
  - `VENPOD_SPARSE_SURFACE_LOW_LANE_MAX_PER_FRAME`
  - `VENPOD_SPARSE_SURFACE_HIDDEN_POST_OPEN_BUDGET`
- temporary world config/stats fields were removed.
- artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_lane_budget_vsync0_20260605`

Rows:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---|
| fixed | `384` | `23.86` | `18.56` | `4.51` | `13.26` | `0.77` | `0.00` | n/a | `6.59` | `0` | `100` | `0/0` | raw worse |
| walk realtime | `600` | `56.01` | `69.32` | `10.99` | `12.35` | `45.96` | `33.61` | `9.64/3.37` | `17.63` | `479` | `94` | `0/0` | rejected CPU/clip regression |
| high-alt | `360` | `183.41` | `36.64` | `10.47` | `7.49` | `18.68` | `5.69` | `5.24/2.14` | `13.07` | `2185` | `99` | `0/0` | rejected raw spike |

Decision:

- Do not retry hidden-only pre-publish caps, prefetch surface caps, or low-lane surface caps as the next fix.
- The retained code has no symbols for these rejected flags.
- The remaining blocker is not a simple surface-budget tuning problem.
- Next engineering direction must reduce or move real visible work with ownership proof:
  - sampled/visible mid debt is still invalid/unknown and cannot be deferred;
  - exact/surface generation and publish work needs a real state-machine split or async/apply pipeline;
  - request/generation/clip/surface/GPU are distributed in walk, so the next accepted fix must cut one bucket without creating catch-up debt.

Detailed artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_lane_budget_rejected_20260605.md`

## Campaign Continuation - Publish Lane Propagation - 2026-06-05

Active goal remains:

```text
streaming_playability_real_fix_campaign_20260604
```

Purpose of this continuation:

- carry explicit streaming lanes beyond request/generation into page-table publish records and upload packets;
- make publish/surface readiness lane composition observable;
- verify whether the remaining post-open publish/surface debt is cache/prefetch work or visible-critical work before adding any deferral policy.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

New retained behavior-neutral plumbing:

- `SparsePendingPageTablePublish::streamingLane`
- `SparseBrickUploadPacket::streamingLane`
- lane-aware `SparsePagePublishQueue::Enqueue(...)`
- lane totals in `SparsePagePublishQueueStats`
- `PERF_SPARSE_PAGE_PUBLISH_LANES`

Behavior:

- default behavior is unchanged;
- old enqueue callers still map residency class to the same default lane;
- edited publishes are promoted to `PublicCritical`;
- duplicate queue records keep the highest lane;
- the new log exposes queue/ready/surface-ready lanes without changing publish order or readiness.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface4_vsync0_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/publish_lane_propagation_vsync0_20260605`

Rejected env-only probe before the plumbing patch:

- `VENPOD_SPARSE_POST_OPEN_PREPUBLISH_SURFACE_MAX_MS=4` improved some raw rows but accumulated thousands of pending publish/surface records.
- fixed frame `360`: raw/CPU `23.39/16.49`, but `publishPending=3619`, `publishLag=139`, `publishSurfGate=435/21`.
- walk frame `600`: raw/CPU `52.85/26.88`, but `publishPending=4542`, `publishLag=515`, `publishSurfGate=485/20`.
- decision: rejected as debt shifting, not a playable candidate.

Publish-lane propagation validation, common candidate stack with background split `0.375`, clean prefetch throttle, explicit source lanes, lane diagnostics, parallel mid worker cache, parallel mid pump, terrain-critical parallel generation, and incremental pressure trim:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Missing voxel | Generation lanes cache/prefetch/visible/public | Upload lanes cache/prefetch/visible/public | Publish queue lanes cache/prefetch/visible/public | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|---:|---|
| fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | n/a | `0/1737/122/0` | `0/0/0/0` | frame `360`: `0/12/0/0` | `0/0` | fixed publish debt is prefetch, but fixed is not the main blocker |
| walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | `437` | `0/5188/105/27` | `0/1156/722/0` | `0/0/5/0` | `0/0` | remaining publish/surface gate is visible, not prefetch |
| high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | `1709` | `0/6958/4/8` | `0/0/0/0` | frame `360`: `0/0/0/0` | `0/0` | high-alt still has broad cache/mid debt, but not publish/surface debt in this row |

Decision:

- publish-lane propagation is retained as behavior-neutral infrastructure and a useful diagnostic step;
- it does not produce a playable candidate by itself;
- the representative walk frame `600` publish/surface debt is visible-lane (`queue=0/0/5/0`, `qsurf=0/43/0/0`), so prefetch surface/publish deferral is not the next safe fix for walk;
- sampled visible fallback-invalid/unknown mid work still cannot be deferred or moved async without a real owner proof;
- all defaults remain unchanged.

Next measured blocker:

- walk remains distributed across visible request/generation/clip/surface/GPU work, with missing sampled mid debt still unknown/invalid;
- the next safe architecture slice is not another prefetch deferral, but an ownership-aware visible work reduction path: either prove a valid fallback owner for sampled missing mid bricks, reduce the visible sampled footprint with a correct screen/ray contract, or refactor the streaming state machine so visible-critical and cache/prefetch publication/apply/surface work are separated end to end.

## Campaign Continuation - Explicit Source Lanes - 2026-06-05

Active goal remains:

```text
streaming_playability_real_fix_campaign_20260604
```

Purpose of this continuation:

- stop treating every visible-resident request as generation/upload/surface `Visible` lane work;
- preserve explicit lower-priority source lanes for view-cone and terrain-prefetch requests;
- keep sampled visible/ownership-critical work guarded and synchronous;
- validate with noncapture rows before declaring anything playable.

Build/test:

- `.\build.ps1 -Config Release`: passed after the retained source-lane patch and after removing the rejected surface-defer branch.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

New retained default-off candidate:

- `VENPOD_SPARSE_REQUEST_EXPLICIT_SOURCE_LANES=0`
- harness switch: `perf_noncapture_smoke.ps1 -RequestExplicitSourceLanes`
- new explicit-lane touch APIs:
  - `SparseBrickPool::TouchResidencyClassWithStreamingLane(...)`
  - `SparseBrickPool::TouchResidencyClassWithStreamingLaneKnownPage(...)`
  - `SparseVoxelWorld::TouchResidencyClassWithStreamingLane(...)`
  - `SparseVoxelWorld::TouchResidencyClassWithStreamingLaneKnownPage(...)`

Behavior:

- when disabled, behavior is unchanged;
- when enabled, selected broad/cache sources can keep `Visible` residency while preserving an explicit lower `Prefetch` streaming lane;
- ownership, near, motion, collision, edit, and terrain-critical requests remain high-lane/default guarded;
- existing resident records are still lane-promotion only, so an explicit lower lane cannot demote already visible/public-critical work.

Source-lane mapping currently enabled behind the flag:

- hierarchical `ViewCone` visible residency requests use `Prefetch` lane;
- flight light-mode view-cone visible requests use `Prefetch` lane;
- terrain surface prefetch and already-renderable prefetch touches use `Prefetch` lane;
- critical ownership and visible sampled debt remain visible/public-critical.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_parallel_mid_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_parallel_surface_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_surface_defer_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_campaign_20260605_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/source_lane_campaign_20260605_table.md`

Key matched rows, common stack:

- background split `0.375`
- clean terrain prefetch throttle
- streaming lane queue priority
- parallel mid worker column cache
- terrain critical parallel generation

| Branch | Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing voxel | Coverage | Gen lanes cache/prefetch/visible/public | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---|
| baseline | fixed | `384` | `23.01` | `19.91` | `3.92` | `15.20` | `0.77` | n/a | n/a | `6.14` | `0` | `100` | `0/0/1198/0` | `0/0` | reference |
| source lanes | fixed | `384` | `23.13` | `21.62` | `3.76` | `17.09` | `0.76` | n/a | n/a | `5.72` | `0` | `100` | `0/1072/120/0` | `0/0` | lane split works, fixed CPU slightly worse |
| baseline | walk realtime | `600` | `72.31` | `64.99` | `14.23` | `14.46` | `36.29` | `25.31` | `10.89` | `15.16` | `469` | `94` | `0/0/3115/34` | `0/0` | reference |
| source lanes | walk realtime | `600` | `79.25` | `49.97` | `13.92` | `13.35` | `22.68` | `20.26` | `12.69` | `16.55` | `441` | `94` | `0/2996/50/28` | `0/0` | CPU improves, raw still bad |
| baseline | high-alt | `395` | `44.29` | `30.09` | `6.98` | `6.49` | `16.62` | `5.52` | n/a | `10.07` | `1769` | `99` | `0/0/5384/8` | `0/0` | reference |
| source lanes | high-alt | `394` | `49.18` | `24.55` | `5.51` | `6.22` | `12.81` | `5.36` | n/a | `10.53` | `1860` | `99` | `0/5431/2/8` | `0/0` | CPU improves, raw still bad |

Additional branch results:

- `source lanes + parallel mid pump`: walk raw improved relative to source-lane-only (`79.25 -> 69.70`) and clip/pump dropped (`22.68/20.26 -> 12.92/10.08`), but CPU worsened (`49.97 -> 59.57`) because request/gen inflated. Not accepted as a global stack.
- `source lanes + parallel surface extraction`: fixed was acceptable, but walk regressed (`74.40 raw`, `67.65 CPU`, `37.91 clip`). Rejected for this stack.
- `source lanes + surface prefetch-lane defer`: implemented, validated, and removed. It created huge surface/publish backlog (`surfaceLane prefetch` thousands, `publishPending` about `3700`, publish lag hundreds) and did not improve frame time. Do not repeat blind prefetch surface deferral.

Decision:

- no validated 60 FPS or playable candidate exists;
- explicit source lanes are retained as a default-off partial state-machine slice;
- surface-defer prefetch lane is rejected and removed;
- parallel mid pump remains a mixed comparison-only lever, not accepted globally here;
- parallel surface extraction remains rejected for this stack;
- all defaults remain unchanged.

Current engine state after this continuation:

- source lanes prove the architecture direction is correct: broad view/prefetch work can be separated from visible/public-critical work without dropping requests;
- this alone is not enough because walk still spends material time in request, generation, clip/pump, surface extraction, GPU, and untracked wait/raw variance;
- representative walk remains far over budget (`~70-80 ms raw` in these matched rows);
- high-alt CPU improves but raw remains far over budget and still carries broad cache debt;
- fixed remains near but above budget and still has generation/surface bursts.

Next real implementation:

- persist lanes through the whole publication pipeline, not just request/generation queue touch:
  - request/interested state;
  - generation queue;
  - clipmap pump;
  - upload/apply;
  - surface extraction;
  - page publish/readiness;
  - backlog age/drain by lane.
- do not defer sampled fallback-invalid/unknown work;
- move only cache/prefetch or CPU-proved fallback-valid work to async/deferred queues;
- fix surface/publish backlog by lane with explicit visible-critical publish guarantee before trying another surface budget.

## Campaign Continuation - Request Slice Rejected / Critical Generation Batch - 2026-06-04

Operating contract:

- Active goal remains `streaming_playability_real_fix_campaign_20260604`.
- Do not stop after one diagnostic or one partial branch.
- Do not weaken ownership, do not treat unknown fallback as safe, and do not promote defaults.
- Keep rejected prototypes removed unless they are explicitly diagnostic-only and useful.

Build/test:

- `.\build.ps1 -Config Release`: passed after the retained critical-generation batch patch.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Validation artifacts from this continuation:

- `build/captures/candidate_trim_parallel_mid_noheavydiag_20260604/`
- `build/captures/candidate_trim_parallel_mid_fast_resident_20260604/`
- `build/captures/candidate_request_resident_touch_cache_20260604/`
- `build/captures/candidate_terrain_critical_parallel_generation_20260604/`
- `build/captures/candidate_terrain_critical_parallel_generation_min16_20260604/`
- `build/captures/control_noheavydiag_highalt_after_parallel_gen_patch_20260604/`
- `build/captures/candidate_parallel_gen_min16_v2_guard_20260604/`

Rejected branch:

- A default-off resident-touch cache was implemented and validated, then removed.
- It skipped repeated ready-page touches but did not improve request cost and worsened walk raw:
  - no-heavy accepted stack walk: raw/CPU/request `65.89/45.21/15.44`
  - resident-touch cache walk: raw/CPU/request `70.79/44.71/16.16`
- The existing `VENPOD_SPARSE_REQUEST_FAST_RESIDENT_TOUCH` retest also remains rejected as a CPU fix:
  - walk raw improved `65.89 -> 57.99`, but CPU/clip worsened `45.21/11.55 -> 53.63/21.14`.

Retained default-off candidate:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`
- New same-frame method:
  - `SparseVoxelWorld::PumpGenerationForCoordsParallel(...)`
- Purpose:
  - batch terrain-critical requested exact bricks and generate them on worker threads, then apply/upload/surface-queue them on the main thread in the same frame
  - this is not async deferral and does not weaken ownership

Key rows with current accepted stack plus no-heavy diagnostics:

| Row | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract | GPU | Protected generated | Parallel generated | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| no-heavy control | fixed | `22.08` | `21.87` | `4.00` | `17.04` | `0.80` | `0.00` | `23.22` | `7.49` | `0` | `0` | reference |
| no-heavy control | walk realtime | `65.89` | `45.21` | `15.44` | `18.19` | `11.55` | `8.91` | `11.83` | `19.91` | `30` | `0` | reference |
| no-heavy control | high-alt | `46.00` | `20.53` | `4.60` | `5.44` | `10.48` | `4.28` | n/a | `11.47` | `8` | `0` | reference |
| parallel gen min16 | fixed | `22.62` | `21.99` | `4.58` | `16.55` | `0.85` | `0.00` | n/a | `7.18` | `0` | `0` | unchanged |
| parallel gen min16 | walk realtime | `51.34` | `46.57` | `14.64` | `15.07` | `16.84` | `5.63` | `11.96` | `17.63` | `19` | `19` | raw win, CPU still high |
| parallel gen min16 | high-alt | `47.63` | `26.18` | `6.12` | `7.59` | `12.45` | `4.81` | n/a | `9.90` | `8` | `0` | branch inactive; high-alt variance remains |
| min16 + V2 guard | walk realtime | `71.84` | `37.58` | `13.93` | `15.67` | `7.96` | `5.67` | `12.97` | `15.28` | `33` | `32` | rejected as global stack; raw worsened |

Current decision:

- No 60 FPS candidate exists.
- Terrain-critical parallel generation is a safe default-off partial candidate for walk raw time, but it does not solve CPU and does not become default.
- V2 visible coverage guard remains useful as a high-alt/cache-debt tool, not a global stack addition.
- Request-touch caching is rejected and removed.
- Fast resident touch remains rejected as a CPU/request fix.

Current remaining blocker after this continuation:

- Representative walk is still far over budget even after the partial generation batch:
  - raw about `51.34 ms`
  - CPU about `46.57 ms`
  - request/gen/clip/surface/GPU remain distributed at roughly `14.64/15.07/16.84/11.96/17.63 ms`
- The next real slice must be larger than touch caching:
  - persistent ownership-aware streaming lanes inside the sparse world/clipmap state, or
  - separate visible-critical versus cache generation/upload/surface budgets at the data-structure level, or
  - surface extraction/apply split by visible-critical lane with backlog accounting.

## Campaign Continuation - Visible-Critical V2 / Interval-2 Cycle - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed, `ninja: no work to do`.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.
- `git diff --check` on touched code/harness reported no whitespace errors, only existing LF-to-CRLF warnings.

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

New code kept:

- Behavior-neutral streaming lane metadata and diagnostics:
  - `SparseStreamingLane { Cache, Prefetch, Visible, PublicCritical }`
  - resident-record lane promotion/touch helpers
  - queue lane counts for generation/upload/surface queues
  - `VENPOD_SPARSE_STREAMING_LANE_DIAGNOSTICS=0`
  - `PERF_SPARSE_STREAMING_LANES`
- Default-off mid-clipmap visible-critical coverage guard V2:
  - `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_COVERAGE_GUARD_V2=0`
  - harness switch `-MidClipmapVisibleCoverageGuardV2`
  - only active with the existing cache-only-defer/prepump classification
  - does not treat sampled-visible or unknown fallback as safe

Key validation:

| Branch | Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface | GPU | Missing sampled/unsampled | Coverage | Miss/unsafe | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| V2 guard | walk realtime | `600` | `74.15` | `34.44` | `11.59` | `13.18` | `9.65` | `7.17` | `9.78/2.64` | `12.35` | `298/196` | `94` | `0/0` | safe no-op; sampled-visible debt stayed critical |
| V2 guard | high-alt | `385` | `48.84` | `26.00` | `4.40` | `5.78` | `15.81` | `6.06` | n/a | `10.18` | `1/1864` | `99` | `0/0` | real isolated win; deferred cache-only high-alt debt |
| V2 guard | fixed | `365` | `18.56` | `11.94` | `6.99` | `4.14` | `0.80` | `0.00` | n/a | `6.07` | `0/0` | `100` | `0/0` | safe fixed row |
| interval-2 | walk realtime | `600` | `67.99` | `40.70` | `15.63` | `16.71` | `8.34` | `6.13` | `9.22/3.13` | `13.72` | `239/190` | `95` | `0/0` | partial clip-interest win, still far over budget |
| pressure-trim guard | walk realtime | `600` | `136.15` | `56.97` | `17.86` | `24.96` | `14.12` | `11.21` | `14.49/4.25` | `24.02` | `247/161` | `95` | `0/0` | rejected; moved cost into surface/GPU/raw |
| parallel surface | walk realtime | `600` | `106.14` | `59.94` | `18.12` | `19.67` | `22.12` | `19.49` | `16.53/3.47` | `17.05` | `289/189` | `94` | `0/0` | rejected; parallel extraction regressed |
| parallel exact | walk realtime | `600` | `73.96` | `44.57` | `18.42` | `13.31` | `12.82` | `10.24` | `10.66/3.31` | `14.21` | `410/95` | `94` | `0/0` | rejected; frame/missing debt regressed |
| direct footprint | walk realtime | `600` | `101.25` | `67.27` | `28.74` | `27.94` | `10.57` | `8.57` | `11.51/3.82` | `27.94` | `271/167` | `95` | `0/0` | rejected; request/gen/GPU regression |
| async exact | walk realtime | `600` | `119.41` | `54.01` | `19.56` | `21.59` | `12.84` | `10.25` | `11.44/3.07` | `19.96` | `252/178` | `95` | `0/0` | rejected/no-op; async enqueued/applied `0/0` |
| accepted stack retest | fixed | `379` | `30.06` | `18.51` | `11.86` | `5.77` | `0.89` | `0.00` | n/a | `9.08` | `0/0` | `100` | `0/0` | no 60 fps |
| accepted stack retest | walk realtime | `600` | `114.88` | `49.00` | `12.05` | `15.62` | `21.30` | `18.79` | `12.55/3.27` | `23.11` | `295/188` | `94` | `0/0` | no 60 fps |
| accepted stack retest | high-alt | `400` | `112.25` | `53.42` | `19.69` | `19.11` | `14.61` | `7.49` | n/a | `23.11` | `4/1873` | `99` | `0/0` | no 60 fps |

Current decision:

- No validated 60 FPS candidate exists.
- V2 visible-critical coverage guard is a real default-off high-alt cache-debt fix, but it does not solve representative walk because walk debt remains sampled/projected-visible.
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2` is a useful default-off/runtime candidate for cutting some clip interest rebuild work, but it is not enough and showed frame variance in the all-scenario retest.
- Exact/surface generation variants tested in this cycle are rejected for this stack:
  - pressure-trim free-page guard
  - parallel surface extraction
  - parallel exact generation
  - direct footprint columns
  - async exact generation
- Remaining measured blockers are coupled:
  - walk/realtime: sampled-visible mid debt plus exact request/generation/surface work, with request/gen/clip/surface/GPU all material in different rows
  - high-alt: V2 can defer unsampled cache debt, but frame time still varies and GPU/request/gen can spike
  - fixed: close in isolated rows, but not stable in all-scenario retest

Next implementation direction:

- Keep V2 and streaming lane diagnostics default-off.
- Stop trying isolated queue caps or generation variants against sampled-visible unknown debt.
- Build the actual ownership-aware streaming/publication state split:
  - request planner must emit public-critical, sampled-visible, cache/prefetch, and maintenance lanes before allocation/generation;
  - exact generation, surface extraction, upload, and publication must consume those lanes with separate budgets/backlogs;
  - sampled-visible fallback-unknown/invalid remains guarded and synchronous/public;
  - cache/prefetch can be async/budgeted only when classified out of public ownership;
  - per-lane backlog age and apply/upload pressure must be logged.

Do not repeat:

- Do not retry pressure-trim free-page guard as a standalone playability fix.
- Do not retry parallel surface/exact generation or direct footprint columns without a new reason; all regressed this stack.
- Do not treat async exact as implemented; it enqueued zero work in the representative row.
- Do not use V2 as a walk fix; it correctly refuses to defer sampled-visible walk debt.

## Campaign Continuation - Exact ViewCone and Cache Defer Closure - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after removing the exact ViewCone no-op prototype.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/exact_viewcone_prefetch_async_candidate_20260604/`
- `build/captures/exact_viewcone_prefetch_async_highalt_candidate_20260604/`
- `build/captures/accepted_stack_vsync_off_probe_20260604/`
- `build/captures/mid_cache_only_defer_highalt_candidate_20260604/`
- `build/captures/mid_cache_only_defer_walk_candidate_20260604/`
- campaign table updated:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Branches closed:

- Exact ViewCone speculative demotion / exact async probe was measured as a no-op and removed from code/harness.
  - walk realtime frame `600`: raw `71.90`, CPU `43.56`, request/gen/clip/pump `14.86/15.52/13.17/10.94`, GPU `14.43`, missing `438`, sampled `240`, coverage `94`, miss/unsafe `0/0`
  - `PERF_SPARSE_EXACT_PUBLIC_LANES`: `viewDemoted=0`, `viewPrefetchDemoted=0`
  - `PERF_SPARSE_EXACT_ASYNC`: enqueued/completed/applied all `0`
  - high-alt frame `393`: raw `48.94`, CPU `25.67`, clip/pump `14.83/4.29`, missing `1486`, sampled `0`, async enqueued/applied `0`, miss/unsafe `0/0`
- VSync-off probe did not reveal a present-only fix.
  - walk realtime frame `600`: raw `74.62`, body `69.82`, CPU `45.68`, GPU `6.04`, `vsync=0`, miss/unsafe `0/0`
  - raw/body remained far over budget, so frame time is not just VSync pacing.
- Mid-clipmap cache-only defer was measured and rejected as a candidate.
  - high-alt frame `404`: raw `62.14`, CPU `33.82`, clip/pump `22.22/9.37`, missing `1501`, sampled `1101`, unsampled `400`, visible-critical coverage `87`, old budget reason `2`, defer count `0`, miss/unsafe `0/0`
  - walk realtime frame `600`: raw `66.53`, CPU `51.54`, request/gen/clip/pump `11.91/13.41/26.20/13.26`, missing `488`, sampled `289`, visible-critical coverage `96`, old budget reason `2`, defer count `0`, miss/unsafe `0/0`
  - The branch did not activate under the safety guard and regressed representative walk.

Current state:

- No validated 60 FPS candidate exists.
- Strongest default-off candidate stack remains the worker-local mid column cache stack from the campaign table:
  - fixed: raw about `23.77`, CPU `16.95`, GPU `7.97`
  - walk realtime: raw about `49.61`, CPU `24.04`, GPU `12.72`
  - high-alt: raw about `50.85`, CPU `30.05`, GPU `10.05`
- The remaining blocker is not another queue cap or ring heuristic. Representative movement is a coupled ownership/publication problem:
  - sampled/projected-visible mid debt remains fallback-unknown or fallback-invalid;
  - exact request/generation/surface publication still treats visible/public work and cache/prefetch work too similarly;
  - surface extraction and request/generation are still material buckets;
  - cache-only deferral cannot safely help once visible-critical coverage is bad.

Next implementation direction:

- Build the actual ownership-aware streaming/publication state split instead of more local demotion probes:
  - explicit public-critical/sampled-visible, cache/prefetch, and maintenance lanes before request, generation, surface extraction, upload/apply, and page publication;
  - sampled fallback-unknown/invalid work remains guarded;
  - cache/prefetch work can be budgeted or async-generated only after it is classified out of the public owner path;
  - surface extraction and page publication need separate public-critical versus cache/prefetch queues and backlog/age accounting;
  - async generation should start only for cache/prefetch or CPU-proven fallback-valid work, with edit/generation stamps and frame-boundary apply.

Do not repeat:

- Do not retry exact ViewCone speculative demotion; it had zero representative work.
- Do not use cache-only defer as a global candidate; it did not activate safely and regressed walk.
- Do not treat VSync as the primary problem; the VSync-off row still exceeded budget badly.

## Campaign Continuation - Public-Critical Surface Gate Split Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the probe was removed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_gate_public_critical_only_candidate_20260604/`

Probe attempted and removed:

- temporary flag: `VENPOD_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL_ONLY`
- temporary harness switch: `-SurfaceGatePublicCriticalOnly`
- temporary log: `PERF_SPARSE_SURFACE_GATE_PUBLIC_CRITICAL`

Behavior tested:

- page-table surface-ready gate would continue to defer terrain-critical, hidden-exact tracked/critical, collision, and edited publishes
- non-public-critical visible publishes would be allowed through even if exact surface extraction was not yet known
- surface extraction still remained queued; this was not a deletion of surface work

Validation with the accepted stack plus the temporary flag:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe | Gate result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `22.30` | `21.79` | `4.35` | `16.09` | `1.35` | `0.00` | `18.90/1.90` | `6.25` | `0` | `100` | `0/0` | `latePublishes=0` |
| walk realtime | `600` | `92.00` | `60.95` | `10.41` | `11.81` | `38.70` | `36.83` | `8.86/2.64` | `17.36` | `421` | `94` | `0/0` | `latePublishes=0`, `criticalDefers=6` |
| high-alt | `360` | `49.84` | `30.70` | `7.16` | `5.80` | `17.74` | `5.81` | `3.59/1.81` | `10.87` | `0` | `99` | `0/0` | `latePublishes=0` |

Decision:

- Rejected and removed. The target frames had no noncritical late-publish opportunity, so this was not the missing state split.
- Walk regressed to a coverage-catch-up shape (`clip=38.70`, `pump=36.83`), worse than the accepted stack.
- Do not retry a page-table surface gate bypass as the next fix.
- The next real implementation still needs an explicit ownership-aware streaming state machine:
  - request/build produces public-critical, sampled-visible, cache/prefetch, and maintenance lanes;
  - only public-critical/sampled fallback-invalid-or-unknown work participates in coverage emergencies;
  - cache/prefetch work gets separate generation, surface extraction, upload/apply, and publication budgets;
  - sampled fallback-unknown work remains guarded until a real owner proof exists.

## Campaign Continuation - Surface-Ready Publish Pressure Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after adding default-off surface-ready publish pressure accounting.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_queue_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surface_ready_publish_pressure_candidate_20260604/`
- campaign-level `summary.csv` and `table.md` now include both rows.

New default-off prototype knob:

- `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=0`
- harness switch: `-SurfaceReadyPublishPressure`

Code change:

- When both `VENPOD_SPARSE_SURFACE_READY_PUBLISH_QUEUE=1` and `VENPOD_SPARSE_SURFACE_READY_PUBLISH_PRESSURE=1` are enabled, the side queue of surface-gated visible/collision page publishes is counted as protected visible publish pressure in the sparse runtime budget scheduler.
- Default behavior is unchanged.
- `PERF_SPARSE_SURFACE_READY_PUBLISH` now logs `pressure=0/1`.

Validation, accepted stack plus surface-ready publish queue:

| Branch | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface | GPU | Missing sampled | Coverage | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| strongest accepted stack | walk realtime `600` | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `5.82` | `8.57/2.92` | `12.72` | `357` | `95` | `0/0` | baseline |
| surface-ready queue only | walk realtime `600` | `90.29` | `61.98` | `10.45` | `13.83` | `37.69` | `35.59` | `8.52/2.93` | `16.19` | `422` | `94` | `0/0` | rejected; side queue hid pressure and clip catch-up exploded |
| surface-ready queue + pressure | walk realtime `600` | `62.23` | `43.31` | `12.61` | `14.94` | `15.75` | `4.02` | `8.59/2.77` | `17.22` | `369` | `95` | `0/0` | rejected; better than queue-only but worse than accepted stack |

Current decision:

- Surface-ready publish pressure is safe as a default-off diagnostic/prototype, but it is rejected as a playable candidate.
- The queue-only row proved that hiding surface-gated visible publish work from the scheduler is unsafe for performance.
- The pressure row proved that merely feeding the side queue back into scheduler pressure is not enough; it increases protected catch-up work and regresses fixed/walk against the accepted stack.
- No validated 60 FPS/noncapture playable candidate exists.
- The strongest validated stack remains the worker-local mid column cache stack:
  - fixed `raw=23.77`, CPU `16.95`, GPU `7.97`
  - walk realtime `raw=49.61`, CPU `24.04`, GPU `12.72`
  - high-alt `raw=50.85`, CPU `30.05`, GPU `10.05`

Next real implementation direction:

- Stop local publication-queue pressure tuning.
- Build the ownership-aware streaming/publication state split:
  - public-critical visible/collision pages stay guarded and synchronous until surface/page-table publication is safe;
  - cache/prefetch pages have separate generation, surface extraction, upload/apply, and publish lanes;
  - non-public-critical surface extraction can become async only after the page is classified out of the public owner path;
  - the scheduler must account for public-critical and cache/prefetch backlog separately instead of using one ready-publish queue.

## Campaign Continuation - Time-Budgeted Surface Parallel Probe - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after adding the default-off time-budgeted surface parallel batch cap.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched code reported no code whitespace errors, only existing LF/CRLF warnings.
- Known `rayDir` shadow warnings remain.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_surface_time_budgeted_batch32_candidate_20260604/`

New default-off prototype knobs:

- `VENPOD_SPARSE_SURFACE_PARALLEL_TIME_BUDGETED=0`
- `VENPOD_SPARSE_SURFACE_PARALLEL_MAX_BATCH=32`
- harness switches:
  - `-ParallelSurfaceExtractionTimeBudgeted`
  - `-ParallelSurfaceExtractionMaxBatch`

Code evidence before the probe:

- The accepted walk row surface queue was `qsurf=0/3265/0/0`, meaning the queued surface debt was visible-class, not speculative/cache.
- Async surface extraction for speculative/cache work would be a no-op in that representative row unless the engine first adds a real public-critical/cache publication split.
- The previous parallel surface extraction probe pulled a public surface batch of `48` and overran the timed surface lane.

Validation, with the accepted stack plus `-ParallelSurfaceExtraction -ParallelSurfaceExtractionTimeBudgeted -ParallelSurfaceExtractionMaxBatch 32`:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Surface extract/stage | Parallel surface | GPU | Missing | Coverage | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `21.32` | `20.54` | `3.76` | `15.98` | `0.79` | `18.90/2.08` | `18/4/1.26` | `5.36` | `0` | `100` | `0/0` | rejected; surface work ballooned |
| walk realtime | `600` | `76.24` | `36.50` | `13.67` | `16.20` | `6.62` | `11.75/2.97` | `32/4/2.95` | `14.38` | `443` | `95` | `0/0` | rejected; worse than accepted stack |
| high-alt | `360` | `48.76` | `29.78` | `7.02` | `6.23` | `16.52` | `3.58/1.86` | `11/4/1.05` | `10.23` | `2228` | `99` | `0/0` | no meaningful stack win |

Current decision:

- The time-budgeted surface parallel probe is rejected as a performance candidate.
- It preserved `miss/unsafe=0/0`, but it shifted debt into visible request/generation/publication and regressed the representative walk row.
- This closes the obvious local surface-parallel branch. The remaining blocker is architectural, not a missing worker count or batch size:
  - visible/public page publication, surface extraction, and request/generation are still coupled;
  - representative walk surface debt is visible-class, not cache/speculative;
  - async extraction is only safe after the engine has a queryable public-critical/cache split.

## Campaign Continuation - Request/Surface Micro-Branches - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the surface parallel extraction candidate.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
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
- `PERF_SPARSE_CPU_DETAIL` now reports `surfaceParallel=active/bricks/workers/wallMs`
- `perf_noncapture_smoke.ps1` exposes `-RequestFastResidentTouch`, `-SurfaceExtractionMaxMs`, `-ParallelSurfaceExtraction`, and `-ParallelSurfaceExtractionMaxWorkers`

Branches attempted after the worker-local mid column cache:

| Branch | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Surface extract/stage | GPU | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| strongest accepted stack | walk realtime `600` | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `8.57/2.92` | `12.72` | `0/0` | baseline to beat |
| fast resident touch | walk realtime `600` | `59.01` | `38.22` | `14.70` | `15.75` | `7.70` | `12.25/3.06` | `5.67` | `0/0` | rejected; safe but slower |
| global surface extraction max `1` | walk realtime `600` | `63.83` | `34.30` | `13.22` | `15.13` | `5.93` | `9.89/2.46` | `11.95` | `0/0` | rejected; high-alt-only direction, walk worsened |
| parallel surface extraction, uncached | walk realtime `600` | `64.81` | `36.94` | `14.68` | `16.41` | `5.84` | `16.11/2.91` | `12.70` | `0/0` | rejected; worker helper recomputed columns |
| parallel surface extraction, worker cache | walk realtime `600` | `72.39` | `41.06` | `14.28` | `19.01` | `7.75` | `12.78/2.89` | `9.87` | `0/0` | rejected; worker batch overran time-budgeted surface lane |

Current decision:

- No validated 60 FPS/noncapture playable candidate exists.
- The strongest validated default-off stack remains the worker-local mid column cache stack:
  - fixed `raw=23.77`, CPU `16.95`, GPU `7.97`
  - walk realtime `raw=49.61`, CPU `24.04`, GPU `12.72`
  - high-alt `raw=50.85`, CPU `30.05`, GPU `10.05`
- Fast resident touch, global surface cap, and parallel surface extraction are rejected as performance candidates.
- They can remain default-off diagnostics/prototypes for now, but should not be included in the candidate stack.

Next real implementation direction:

- Stop trying one-bucket caps, sort tweaks, or synchronous worker batches as the main path.
- Build an ownership-aware streaming/publication state split:
  - visible-critical pages stay synchronous/guarded until exact surface/page-table publication is safe;
  - cache/prefetch pages can generate/extract surface asynchronously because they are not required public owners this frame;
  - surface extraction results apply on the main thread at a frame boundary with upload/apply budgets;
  - request prep, generation, surface extraction, page publish, and GPU upload need separate lane/backlog age accounting.
- The smallest next safe code slice is async surface extraction for cache/prefetch or otherwise non-public-critical surface work, not parallel synchronous extraction for the time-budgeted public surface gate.

## Campaign Continuation - Worker-Local Mid Column Cache - 2026-06-04

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the worker-local mid column cache and surface-general-budget instrumentation.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Updated campaign artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Accepted default-off partial:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE=0`
- Harness switch: `-ParallelMidWorkerColumnCache`
- Purpose: preserve the synchronous visible-critical mid-clipmap generation path while giving each parallel pump worker its own terrain column cache.
- This avoids treating sampled unknown fallback as safe. It changes generation throughput only.

Best continuation row, with background split `0.375`, playable quality, clean prefetch throttle, backlog-aware pump/diagnostics, missing-sample feedback, terrain critical reuse, hidden exact gen budget `24`, surface extraction max `1`, incremental pressure trim `16k`, and worker-local mid column cache:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `23.77` | `16.95` | `4.15` | `12.03` | `0.76` | `0.00` | `7.17/1.60` | `7.97` | `0` | `100` | `0/0` | still not 60 FPS |
| walk realtime | `600` | `49.61` | `24.04` | `8.83` | `7.82` | `7.38` | `5.82` | `8.57/2.92` | `12.72` | `357` | `95` | `0/0` | strongest safe continuation row |
| high-alt | `360` | `50.85` | `30.05` | `7.65` | `6.58` | `15.82` | `4.27` | `3.90/1.89` | `10.05` | `0` | `100` | `0/0` | unsampled cache debt remains |

Rejected/default-off probes in this continuation:

- `VENPOD_SPARSE_SURFACE_STRICT_TIME_BUDGET=1`: rejected. Walk frame `600` regressed to `raw=102.71`, `CPU=40.79`.
- existing `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1`: rejected. It reduced per-brick repeated work but disabled parallel and regressed walk to `CPU=44.74`, `clip=28.15`, `budgetReason=2`.
- existing `VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE=1`: rejected. Sort cache had zero hits and walk regressed to `CPU=45.95`.
- existing `VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT=1`: rejected. Walk `CPU=34.35`, still worse than the worker-cache stack.
- new `VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET=1`: rejected as a perf fix. It skipped general surface catch-up, but protected surface work and mid catch-up still dominated; walk `CPU=31.92`, `budgetReason=2`.
- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS=8`: rejected. Pump wall dropped in some frames, but walk CPU regressed to `36.38`.
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=1` with worker cache: rejected/no win for representative walk.
- `VENPOD_SPARSE_EXACT_ASYNC_GENERATION=1` with visible async off: no-op in sampled rows (`async enqueued/applied=0`), not a fix.

New default-off/instrumentation knobs:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_WORKER_COLUMN_CACHE=0`
- `VENPOD_SPARSE_SURFACE_GENERAL_STRICT_BUDGET=0`
- `VENPOD_SPARSE_SURFACE_GENERAL_MIN_BUDGET_MS=4`
- Harness parameters:
  - `-ParallelMidWorkerColumnCache`
  - `-ParallelMidVoxelPumpMaxWorkers`
  - `-ParallelExactGenerationMaxWorkers`
  - `-SurfaceGeneralStrictBudget`
  - `-SurfaceGeneralMinBudgetMs`

Current decision:

- No validated 60 FPS/noncapture playable candidate exists.
- Completion state remains **Completion B**: strongest default-off candidate stack plus exact remaining blocker table.
- The worker-local column cache is a safe default-off partial, but it does not reach playability.
- The representative walk row is now balanced across several buckets rather than one obvious local fix:
  - CPU `24.04 ms`
  - request `8.83 ms`
  - exact gen `7.82 ms`
  - mid clip/pump `7.38/5.82 ms`
  - surface extract/stage `8.57/2.92 ms`
  - GPU ray `12.72 ms`
  - raw frame `49.61 ms`
- This means the next successful step cannot be another blind cap or queue-order tweak. It needs an ownership-aware streaming/publication state split with separate budgets for request prep, visible-critical generation, cache/prefetch generation, upload/apply, and surface publication.

Next concrete implementation direction:

1. Keep the worker-local mid column cache in the opt-in candidate stack.
2. Do not keep trying surface sort/cache/worker-count toggles as the main path.
3. Build the real streaming state-machine slice:
   - CPU-side visible-critical set from projected/sample feedback;
   - cache/prefetch set that never participates in public coverage emergency;
   - async generation only for cache/prefetch or CPU-proved fallback-valid work;
   - visible sampled fallback-unknown bricks remain synchronous/guarded;
   - frame-boundary apply/upload budget;
   - surface publication budget split between visible-critical and catch-up/general.
4. If that slice is too broad for one pass, start with the surface publication split because current rows prove general surface work can consume time after visible-critical ownership is already safe.

## Active Operating Goal

This is the durable `/goal` for the agent:

```text
streaming_playability_real_fix_campaign_20260604:
drive VENPOD from default-off partial candidates to a validated playable candidate
through measured engine fixes, noncapture validation, and explicit rejection of
unsafe shortcuts.
```

The goal is not complete until one of the formal completion states below is met.
Compaction must not reset this into a one-pass diagnostic task.

Current execution branch:

1. Keep the validated GPU stack as a test candidate only:
   - `VENPOD_RENDER_QUALITY=playable`
   - `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1`
   - `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375`
2. Keep the validated CPU helper flags as opt-in only:
   - clean prefetch throttle
   - surface incremental metadata adds
   - stats single flush
   - backlog-aware pump
   - visible-critical prepump diagnostics/split
3. Do not repeat rejected branches:
   - blunt pump caps
   - age-priority queue ordering
   - ring-only visible-critical heuristics
   - pool-size tuning as a correctness/performance fix
   - unknown-fallback deferral
   - CPU projected parent-held feedback as-is; it accepted almost no useful child coords
4. Attack the current measured blocker:
   - realtime/walk has mostly projected-visible mid debt with fallback-valid still zero
   - high-alt benefits from visible-critical/cache split, but is not the representative walk blocker
   - parent-held/LOD catch-up is a symptom of missing ownership-aware streaming state
   - the next real code slice must separate visible-critical, cache/prefetch, generated, staged, uploaded, and fallback-valid states instead of only reordering one queue
5. Validate with noncapture fixed, realtime walk, and high-alt rows before accepting.

Do not switch branches merely because another diagnostic is easier. Switch only when
the measured table shows a larger blocker or this branch hits a concrete build,
tooling, runtime, or ownership-contract blocker.

## Campaign Engineering Cycle - Surface Queue / Tracked Scan Continuation - 2026-06-04

Build/test:

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

New default-off diagnostics/candidates:

- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGET=512`
- `PERF_SPARSE_HIDDEN_EXACT_TRACKED_SCAN`
- `VENPOD_SPARSE_SURFACE_CLASS_SORT_CACHE=0`
- `VENPOD_SPARSE_SURFACE_CLASS_PARTIAL_SORT=0`
- `PERF_SPARSE_CPU_DETAIL surface=.../sort/sortHit`

Current strongest default-off stack:

| Scenario | Frame | Raw ms | Body ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `385` | `34.93` | `24.54` | `11.71` | `9.75` | `1.09` | `0.83` | `0.00` | n/a | `5.90` | `0` | `100` | `0/0` |
| walk realtime | `600` | `46.49` | `55.44` | `31.05` | `8.15` | `7.00` | `15.87` | `13.33` | `9.00/3.31` | `14.60` | `323` | `95` | `0/0` |
| high-alt | `396` | `52.25` | `50.66` | `25.55` | `6.20` | `6.33` | `13.01` | `4.65` | n/a | `12.12` | `0` | `99` | `0/0` |

Rejected follow-up branches:

- Hidden-tracked scan budget `512`: walk raw `56.03`, CPU `32.09`, surf extract/stage `8.94/2.91`; scan counts dropped but the surface bucket stayed material.
- Hidden-tracked scan budget `128`: walk raw `53.88`, CPU `31.02`, surf extract/stage `9.16/2.87`; lower scan budget did not reduce the actual surface bucket.
- Surface class sort cache: walk raw `48.71`, CPU `34.87`, surface sort calls `3`, hits `0`; queue/focus dirtiness prevented reuse.
- Surface partial sort: walk raw `49.23`, CPU `35.14`, surf extract/stage `8.48/3.00`; small local surface reduction, not a stack win.
- Direct footprint path: walk raw `47.85`, CPU `33.96`, missing voxel `485`, coverage `94`, budget reason `2`; it increased missing/catch-up pressure.

Decision:

- Completion state is **Completion B**: strongest default-off stack plus exact remaining blocker table, not a validated 60 FPS candidate.
- The representative walk row still fails by a wide margin: raw `46.49 ms`, CPU `31.05 ms`, GPU ray `14.60 ms`.
- Local queue-order, scan-budget, sort-only, and direct-footprint tweaks have not solved the sampled movement path.
- The next real slice should be an ownership-aware streaming state-machine change: explicit visible-critical/cache/prefetch lanes, generation state, staged publication, and separate budgets for critical generation versus surface publication.
- All defaults remain unchanged. Background split, clean throttle, backlog-aware pump, fallback diagnostics/proofs, async, bounded repair, hidden tracked scan budgeting, and surface sort candidates remain default-off.

## Campaign Engineering Cycle - Budget-Compatible Parallel Mid Pump - 2026-06-04

Build/test:

- `.\build.ps1 -Config Release`: passed after the budget-compatible parallel mid-pump patch
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_gen24_surface_max4_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_no_backlog_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_no_backlog_trim8k_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_no_backlog_trim8k_walk900_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_no_backlog_freeguard_walk900_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_hidden_gen16_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_budgeted_mid_pump_surface_max1_candidate_20260604/`
- master ledger:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Patch:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=1` can now run under the backlog-aware pump budget instead of being disabled by `voxelPumpBudgetActive`.
- When the backlog-aware budget is active, the parallel pending batch is capped by `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS` so the opt-in worker path cannot drain the whole queue in one frame.
- `m_pumpBudgetHitLastFrame` is marked when the budgeted parallel path consumes the budget and still has queued bricks.
- `perf_noncapture_smoke.ps1` gained `-NoBacklogAwarePump` for diagnostics only; the accepted candidate keeps backlog-aware pump enabled.

Results:

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Sampled | Coverage | Parallel active | Safety | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| gen24 + surface max4 | walk realtime | `50.51` | `44.10` | `14.79` | `11.08` | `18.21` | `5.53` | `11.31/3.52` | `18.58` | `446` | `376` | `95` | `0` | `0/0` | partial only |
| no-backlog parallel diagnostic | walk realtime | `79.09` | `41.27` | `20.81` | `12.35` | `8.08` | `5.98` | `13.36/4.92` | `5.62` | `137` | `109` | `98` | `1` | `0/0` | diagnostic only |
| no-backlog parallel + trim8k | walk realtime | `65.62` | `41.29` | `19.89` | `12.44` | `8.94` | `6.80` | `12.12/3.39` | `6.48` | `93` | `70` | `98` | `1` | `0/0` | rejected; high-alt unstable |
| no-backlog parallel + trim8k | high-alt | `55.63` | `40.13` | `6.57` | `9.93` | `23.62` | `10.25` | n/a | `8.71` | `1175` | `865` | `86` | `1` | `0/0` | rejected coverage/sample regression |
| no-backlog parallel + free guard | walk realtime f900 | `98.31` | `39.79` | `9.08` | `7.16` | `23.53` | `20.49` | `4.59/4.99` | `19.64` | `132` | `104` | `98` | `1` | `0/0` | rejected again |
| budget-compatible parallel mid pump | fixed | `27.78` | `3.63` | `2.04` | `0.73` | `0.83` | `0.00` | n/a | `7.33` | `0` | `0` | `100` | `0` | `0/0` | safe fixed |
| budget-compatible parallel mid pump | walk realtime | `70.54` | `35.90` | `17.17` | `11.93` | `6.78` | `4.69` | `11.97/3.28` | `13.72` | `391` | `324` | `95` | `1` | `0/0` | accepted partial |
| budget-compatible parallel mid pump | high-alt | `60.77` | `27.59` | `5.79` | `8.75` | `13.05` | `4.64` | n/a | `16.11` | `1495` | `0` | `100` | `1` | `0/0` | accepted partial |
| budgeted parallel + gen16 | walk realtime | `69.95` | `40.43` | `16.00` | `11.83` | `12.57` | `10.49` | `12.28/4.14` | `13.49` | `444` | `371` | `94` | `1` | `0/0` | rejected |
| budgeted parallel + surface max1 | walk realtime | `88.13` | `40.32` | `21.18` | `12.32` | `6.73` | `4.42` | `11.12/3.59` | `18.84` | `433` | `370` | `95` | `1` | `0/0` | rejected |

Decision:

- Keep the budget-compatible parallel mid pump default-off as a real partial code candidate. It finally makes the existing parallel mid pump active inside the backlog-aware safety budget and improves the representative walk CPU path without miss/unsafe regression.
- This is not a 60 FPS candidate. The best latest walk row is still `rawMs=70.54`, `cpuUpdateMs=35.90`, with `requestMs=17.17`, `genMs=11.93`, `surfaceExtractMs=11.97`, `surfaceStageMs=3.28`, and `gpuRayMs=13.72`.
- Hidden exact generation budget `24` remains the best tested budget in this slice. Budget `16` caused coverage/budget catch-up debt in walk and is rejected.
- Post-open surface max `4 ms` is only a partial helper; max `1 ms` is rejected because it moves debt into generation/fixed rows and does not lower representative walk.
- The no-backlog parallel rows prove worker pump can reduce missing debt, but disabling backlog-aware safety is not the accepted stack.

Current blocker after this cycle:

- Representative walk still has sampled/visible unknown mid debt (`391` missing, `324` sampled) and no fallback-valid async subset.
- The top remaining buckets are now exact/sparse request admission, exact generation, surface extraction/staging, and residual GPU/post-wait, not only mid-clip pump.
- The next real architecture slice should split exact request/generation/surface publication into public-critical versus repair/cache lanes with separate generation and surface budgets, while preserving visible sampled unknown work as guarded/critical.

## Campaign Engineering Cycle - Request Pressure Trim - 2026-06-04

Build/test:

- `.\build.ps1 -Config Release`: passed after the pressure-trim code changes
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_phase_split_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_free_guard_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/incremental_pressure_trim_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/incremental_pressure_trim_16k_candidate_20260604/`
- master ledger updated:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

New default-off knobs:

- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=0`
- `VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL=0`
- `VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET=32768`

New request detail fields:

- `pressureTrimMs`
- `preHierarchy=brush/owner/diagnostic/missFb/hiddenExact`
- `pressureTrimFreePageGuard`

Findings:

- The old `PERF_SPARSE_REQUEST_DETAIL otherMs` bucket was not primarily hidden-exact feedback. A large part was pressure trim/full-cache scanning before request admission.
- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=1` is rejected. It avoids full pressure trim when free pages look plentiful, but it starves cleanup/catch-up and causes streaming debt to explode.
- Incremental pressure trim is a safer default-off partial candidate. It slices background trim scans through persistent cursors instead of scanning all background resident/queued records in one frame.
- The 16k scan budget is better than 32k for high-alt in the sampled row, but it does not improve representative walk.

Results:

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract | GPU | Missing | Sampled | Coverage | Safety | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| free-page guard | walk realtime | `164.69` | `141.35` | `24.32` | `44.75` | `72.25` | `68.85` | `21.05` | `18.11` | `524` | `454` | `93` | `0/0` | rejected |
| free-page guard | high-alt | `115.77` | `156.88` | `44.74` | `58.57` | `53.56` | `15.11` | n/a | `16.50` | `2025` | `73` | `98` | `0/0` | rejected |
| incremental trim 32k | walk realtime | `108.50` | `68.71` | `29.77` | `29.80` | `9.10` | `6.74` | `15.11` | `12.80` | `445` | `376` | `95` | `0/0` | partial only |
| incremental trim 32k | high-alt | `94.96` | `55.33` | `17.54` | `14.68` | `23.10` | `7.40` | n/a | `19.29` | `1996` | `65` | `99` | `0/0` | partial only |
| incremental trim 16k | walk realtime | `117.43` | `69.71` | `32.85` | `30.15` | `6.69` | `4.16` | `15.56` | `6.20` | `443` | `375` | `95` | `0/0` | not a walk win |
| incremental trim 16k | high-alt | `75.70` | `40.75` | `11.60` | `11.33` | `17.81` | `6.42` | n/a | `11.42` | `1978` | `63` | `99` | `0/0` | high-alt partial win |

Decision:

- Keep incremental pressure trim default-off as a useful partial candidate/diagnostic. It is not sufficient for a playable stack.
- Reject the free-page guard. It creates exactly the kind of hidden streaming debt this campaign is trying to avoid.
- The representative walk row is still blocked by visible exact request/generation and surface extraction around sampled unknown terrain debt:
  - `requestMs=32.85`
  - `genMs=30.15`
  - `surfaceExtractMs=15.56`
  - `missingVoxel=443`, `sampledMissingApprox=375`, `fallbackValid=0`
- Do not move this visible exact debt async and do not throttle hidden-exact/terrain-critical request lanes without an ownership proof. Prior exact async, exact parallel, hidden-exact water throttle, blunt caps, and ring/age heuristics are rejected.

Current campaign status:

- A 60 FPS noncapture candidate has not been reached.
- Best current candidate stack remains default-off and incomplete:
  - background split
  - playable render scale
  - clean prefetch throttle
  - stats single flush
  - surface incremental metadata adds
  - visible-critical prepump
  - backlog-aware pump
  - optional incremental pressure trim
- The next implementation slice is larger than another queue tweak: introduce ownership-aware exact work states that separate visible-critical exact bricks from repair/cache/surface-publish work, with separate generation, upload, and surface extraction budgets.

## Campaign Engineering Cycle - Same-Frame Parallel Generation - 2026-06-04

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/baseline_after_parallel_pump_patch_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_mid_pump_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_generation_candidate_v2_20260604/`

New default-off knobs added:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VISIBLE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MAX_WORKERS=4`
- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PUMP_MIN_BRICKS=8`
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION_MIN_BRICKS=8`

Results:

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | GPU | Missing | Safety | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| baseline after patch | fixed | `46.85` | `12.92` | `3.87` | `7.59` | `1.45` | `0.00` | `11.20` | `0` | `0/0` | baseline only |
| baseline after patch | walk realtime | `130.36` | `73.68` | `36.79` | `30.16` | `6.70` | `4.24` | `32.75` | `436` | `0/0` | not playable |
| baseline after patch | high-alt | `49.46` | `25.48` | `6.08` | `6.26` | `13.13` | `5.15` | `11.26` | `1810` | `0/0` | not 60 |
| parallel mid pump | walk realtime | `75.97` | `40.80` | `18.90` | `15.79` | `6.09` | `4.08` | `13.45` | `439` | `0/0` | `parallelPumpActive=0`; improvement cannot be credited |
| parallel exact generation v2 | fixed | `52.74` | `11.30` | `2.97` | `7.29` | `1.03` | `0.00` | `10.88` | `0` | `0/0` | exact parallel active but not a fixed win |
| parallel exact generation v2 | walk realtime | `209.53` | `146.40` | `37.00` | `27.51` | `81.87` | `79.37` | `27.83` | `527` | `0/0` | rejected regression |
| parallel exact generation v2 | high-alt | `176.07` | `77.50` | `31.38` | `18.86` | `27.25` | `6.56` | `24.12` | `2030` | `0/0` | rejected regression |

Decisions:

- Same-frame parallel mid-clipmap pump is safe to keep default-off, but it did not activate in the latest matrix because the current rows were running under the backlog-aware time budget. It is diagnostic/candidate-only, not an accepted playability fix.
- Same-frame parallel exact generation is rejected as a playability candidate. It activated, but direct terrain payload generation lost the cached-column advantage and caused broader request/clip/publish/surface debt. Do not use it in the playable stack.
- Miss and unsafe stayed zero, but this is not success; walk and high-alt regressed badly under exact parallel.
- Current remaining blocker after rejecting exact parallel is still the walk/realtime streaming state: request prep plus generated/uploaded/surface backlog around visible sampled terrain debt. The next useful code branch must target cached exact generation/request/surface pipeline architecture, not direct parallel terrain generation.

## Campaign Engineering Cycle - Rejected Local Streaming Branches - 2026-06-04

Build/test:

- `.\build.ps1 -Config Release`: passed after the cached-column worker patch.
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_generation_cached_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/async_exact_generation_cached_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_throttle_candidate_20260604/`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/bounded64_current_stack_candidate_20260604/`
- master table updated at `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Patch:

- Refactored exact brick generation so worker paths can preserve the cached-column terrain generation algorithm with per-worker local column caches.
- Default behavior is unchanged.
- `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION` remains default-off and is not an accepted candidate.

Results:

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | GPU | Missing | Safety | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| cached exact parallel | fixed | `58.14` | `16.63` | `4.11` | `8.46` | `4.05` | `0.00` | `12.06` | `0` | `0/0` | no fixed win |
| cached exact parallel | walk realtime | `207.85` | `140.79` | `33.64` | `30.10` | `77.03` | `74.37` | `6.48` | `452` | `0/0` | rejected regression |
| cached exact parallel | high-alt | `207.86` | `236.39` | `68.43` | `72.69` | `95.25` | `10.03` | `6.38` | `2021` | `0/0` | rejected regression |
| async exact cached | walk realtime | `144.58` | `90.44` | `30.79` | `30.85` | `28.77` | `24.46` | `33.27` | `423` | `0/0` | rejected/no-op; enqueued `0` |
| hidden-exact water throttle | walk realtime | `93.13` | `117.13` | `28.04` | `11.93` | `77.13` | `55.63` | `5.40` | `460` | `0/0` | rejected; clip catch-up returned |
| bounded64 current stack | walk realtime | `188.26` | `131.54` | `37.36` | `25.52` | `68.64` | `65.51` | `19.91` | `736` | `0/0` | rejected for performance; remains correctness comparison only |

Interpretation:

- The visible exact queue is the hot class; existing async exact does not enqueue with visible async disabled. Enabling visible async would defer visible sampled unknown work and is not allowed without an ownership proof.
- Hidden-exact water repair was a real source of exact requests in the best walk row (`96/97` accepted hidden-exact requests), but throttling that lane is not a safe performance fix: it reduced exact requests and immediately pushed the frame into mid-clipmap catch-up debt.
- Bounded64 is still not the performance fix in this current stack; it remains default-off and comparison-only.
- Parallelizing exact generation, even with local cached columns, is not the next local fix. It increases downstream request/clip/surface pressure.

Current best measured row in this campaign remains:

- `parallel_mid_pump_candidate` walk realtime frame `600`: `rawMs=75.97`, `cpuUpdateMs=40.80`, `requestMs=18.90`, `genMs=15.79`, `clipMs=6.09`, `surfaceExtractMs=11.16`, `gpuRayMs=13.45`, `missingVoxel=439`, `sampledMissingApprox=371`, `miss/unsafe=0/0`.
- This row is not a validated fix because `parallelPumpActive=0`; it is the current least-bad stack row and still far from 60 fps.

Next branch:

- Split and then reduce the request/exact-surface pipeline, not another generation-threading branch:
  - hidden-exact accepted requests and water candidates,
  - terrain-critical request admission,
  - exact page generation budget,
  - surface extraction/publish backlog,
  - upload/apply/publish lag.
- Any fix must preserve visible sampled unknown/invalid work as guarded/critical. Do not move visible exact work async and do not throttle hidden-exact repair unless a valid owner/fallback proof explains why the deferred work is not public-critical.

## Non-Negotiable Contract

Every visible pixel must have a legitimate owner whose result agrees with deterministic terrain/water truth.

Valid owners:

- exact sparse surface
- valid mid voxel clipmap
- valid Far-SVO
- deterministic water
- sky

Invalid:

- unknown fallback
- dropped visible-critical bricks
- hidden cache debt exposed as holes/white terrain/shoreline
- `miss=0` / `unsafe=0` by itself

## Defaults That Must Stay Unchanged

Keep these default-off/default-neutral unless behavior is identical at default:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=1.0`
- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=0`
- `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=0`
- `VENPOD_SPARSE_STATS_SINGLE_FLUSH=0`
- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_VALIDITY_CLASSIFIER=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FALLBACK_CONTRACT_DIAGNOSTICS=0`
- `VENPOD_SPARSE_MID_CLIPMAP_FAR_SVO_FALLBACK_PROOF=0`
- `VENPOD_SPARSE_MID_CLIPMAP_MISSING_SAMPLE_FEEDBACK=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0`
- `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=strict`

Keep `bounded_repair` and bound `64` as default-off correctness candidates only.

## Explicit Anti-Stop Rules

Do not end a run merely because:

- one diagnostic was added
- one heuristic failed
- one partial optimization worked
- the next action is obvious
- capture rows improved but noncapture validation is missing
- `missScreenPct=0` and `unsafeNearMissPct=0`
- the engine still needs another measured CPU slice

Only stop when one formal state is reached:

1. **Validated 60 FPS candidate**
   - noncapture fixed and representative walk total frame time are `<=16.67 ms`
   - miss/unsafe stay zero
   - contact sheet has no new white terrain/shoreline/holes
   - no ownership-policy weakening

2. **Strong candidate but not 60**
   - combined candidate stack tested
   - at least two plausible implementation branches attempted or ruled out by code evidence
   - exact remaining bottleneck table produced
   - next action is either non-obvious or requires larger scoped refactor

3. **Hard architecture blocker**
   - exact state-machine/call-graph blocker documented
   - missing metadata/proof/refactor slice specified
   - no safe local patch remains without weakening ownership

4. **Hard tool/build blocker**
   - exact failed build/tool/runtime step documented
   - attempted fixes documented

## Campaign Continuation Checkpoint - 2026-06-04

This is the current resume point for the active goal
`streaming_playability_real_fix_campaign_20260604`.

Latest build/test after the accepted code changes:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Accepted during the latest continuation:

- public-open-frame fix in `src/main_launcher.cpp`
  - `sparseStartupPublicRenderOpenedFrame` now latches once when the public gate opens instead of being refreshed every frame
  - this removed stale post-open hidden-exact catch-up from long walk rows
- behavior-preserving voxel-slot reuse in `SparseClipmap`
  - `AllocateVoxelSlot` and `AllocateVoxelSlotForMinRing` now preserve voxel payload vector capacity while resetting slot metadata on eviction
  - this reduces allocator churn without changing generated brick contents or ownership rules
- existing default-off candidate pieces remain available but not defaulted:
  - background split
  - clean terrain prefetch throttle
  - surface incremental metadata
  - stats single flush
  - backlog-aware pump
  - visible-critical prepump
  - fallback diagnostics/proofs

Rejected or diagnostic-only in the latest continuation:

- post-open surface extraction cap at `4 ms`
  - rejected because it moved debt into later frames and worsened walk CPU/clip
- `VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS=1`
  - added as a default-off diagnostic, but not accepted; one frame looked good while the broader matrix stayed unstable
- `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`
  - did not solve recurring walk spikes
- `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS=24576`
  - rejected because it expanded current-interest and caused parent-held visual failure / large pump catch-up
- `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS=24576` plus lower interest percent
  - rejected because missing dropped but `parentHeld`/LOD visual failure exploded into thousands of pixels

Latest focused matrix after the behavior-preserving voxel-slot reuse fix:

Artifact:

- `build/captures/noncapture_voxel_slot_reuse_candidate_all_20260604/table.md`

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Pump ms | Surface extract/stage | GPU ray ms | Missing | Sampled/unsampled | Coverage | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `23.96` | `21.41` | `4.15` | `16.35` | `0.90` | `0.00` | `19.96/2.38` | `6.16` | `0` | `0/0` | `100` | `0/0` | fixed is close but still over budget |
| walk realtime | `900` | `77.86` | `46.70` | `12.39` | `3.15` | `31.12` | `17.11` | `6.39/3.19` | `15.52` | `436` | `366/70` | `95` | `0/0` | not playable; clip/parent-held catch-up returned |
| high-alt | `401` | `48.99` | `24.63` | `5.62` | `5.27` | `13.73` | `5.48` | n/a | `11.87` | `1813` | `41/1772` | `99` visible / `80` cache | `0/0` | improved, still not 60 |

Current technical decision:

- The current blocker is no longer just "missing bricks" or pool capacity.
- Increasing voxel-brick capacity can reduce missing counts, but it exposes or amplifies `parentHeld` / LOD ownership failure where the shader uses a coarser parent ring where the preferred child should own.
- That parent-held visual ownership path disables budgeting and forces broad synchronous catch-up. It is why large-pool tuning faked progress on missing counts while making visual ownership worse.
- The next real fix is ownership-aware parent-held feedback and targeted child-coordinate streaming, not another queue cap, pool-size tweak, or unknown-fallback deferral.

Next implementation target:

1. Inspect the shader/CPU parent-held path:
   - `RAY_DIAGNOSTIC_MID_PARENT_HELD`
   - `lodParentHeld`
   - preferred ring vs actual ring selection in `PS_Raymarch.hlsl`
   - CPU logs that report parent-held/visual ownership failure and budget reason `4`
2. Add a bounded default-off parent-held ownership feedback or CPU projection path:
   - identify preferred child coordinates/rings behind parent-held pixels
   - avoid heavy full-frame atomics and avoid the previous shader hash-bucket compile churn
3. Prioritize exactly those child bricks in the mid-clipmap pump/generation path.
4. Validate fixed, realtime walk, high-alt, and noncapture rows.

Do not mark the goal complete until this parent-held ownership branch is attempted or a concrete tooling/architecture blocker prevents it.

## Current Measured State

GPU:

- background split is the major default-off GPU candidate
- strict native fixed GPU previously improved about `60.38 -> 17.88 ms`
- playable fixed split GPU reached about `8-11 ms`
- GPU is no longer the primary fixed/walk candidate blocker when split is enabled

CPU, latest accepted noncapture candidate stack:

- fixed row: `rawMs=21.39`, `cpuUpdateMs=16.27`, `requestMs=10.93`, `genMs=4.55`, `clipMs=0.79`, `gpuRayMs=6.93`
- realtime walk row: `rawMs=112.09`, `cpuUpdateMs=64.87`, `requestMs=33.39`, `genMs=23.55`, `clipMs=7.87`, `surfaceExtractMs=19.46`, `surfaceStageMs=5.76`, `gpuRayMs=18.08`
- high-alt row: `rawMs=53.11`, `cpuUpdateMs=29.52`, `requestMs=7.92`, `genMs=5.49`, `clipMs=16.10`, `gpuRayMs=14.49`

Interpretation:

- fixed path is still over `16.67 ms` even with the stack enabled
- realtime walk is not playable; current accepted row is dominated by request/generation/surface extraction/GPU, not only clip pump
- high-alt has a real over-broad cache-current-interest fix, but it is still default-off and incomplete

## First Noncapture Scoreboard - 2026-06-04

New harness:

- `perf_noncapture_smoke.ps1`
- log-only, no backbuffer capture, launches through `run.ps1`, exits via `VENPOD_EXIT_AFTER_FRAMES`
- writes `summary.csv` and `table.md`
- supports `-ParseOnly` so existing logs can be re-scored after parser fixes without rerunning VENPOD

Artifacts:

- `build/captures/noncapture_playability_smoke_20260604/summary.csv`
- `build/captures/noncapture_playability_smoke_20260604/table.md`

Rows, using the current default-off candidate stack:

- `VENPOD_RENDER_QUALITY=playable`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375`
- `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_CLEAN_THROTTLE=1`
- `VENPOD_SPARSE_MID_CLIPMAP_BACKLOG_AWARE_PUMP=1`
- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`
- fallback/sample diagnostics enabled

| Scenario | Frame | Body ms | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Surface extract ms | Surface stage ms | Trim ms | GPU ray ms | Missing voxel | Sampled/unsampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `380` | `53.04` | `54.67` | `19.39` | `9.64` | `8.72` | `1.03` | n/a | n/a | `0.00` | `8.44` | `0` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `246.08` | `246.81` | `90.60` | `49.52` | `26.35` | `7.94` | `21.86` | `55.24` | `6.76` | `5.70` | `450` | `378/72` | `95` | `0/0` |
| high-alt | `400` | `87.92` | `91.11` | `41.13` | `12.17` | `10.38` | `18.57` | n/a | n/a | `0.00` | `14.64` | `1982` | `61/1921` | `99` | `0/0` |

Interpretation:

- This is not a 60 FPS candidate. Noncapture fixed is about `54.67 ms` raw, realtime walk is about `246.81 ms` raw, and high-alt is about `91.11 ms` raw.
- GPU is not the current walk blocker under the stack (`gpuRayMs=5.70` in walk).
- Realtime walk no longer points primarily at clip pump in this row. The dominant measured buckets are request prep (`49.52 ms`), generation (`26.35 ms`), surface extraction/staging (`21.86/55.24 ms`), and a large raw/post gap.
- High-alt still benefits from visible-critical prepump: only `61/1982` missing bricks are projected sampled, coverage is `99`, and budget reason is `0`. It remains too slow because CPU clip/generation/request and GPU high-alt still matter.
- Walk debt remains mostly sampled/unknown (`378/450`), so it still cannot be deferred as cache or async-generated under the ownership contract.

Immediate next implementation slice:

1. Inspect the surface extraction/staging path that produced `21.86 ms` extract and `55.24 ms` stage in noncapture walk.
2. Determine whether this is visible-critical upload/apply, redundant unchanged staging, backlog flush, or a synchronization/post-gap artifact.
3. Implement one default-off/behavior-preserving surface staging or request-prep fix only after proving the dominant mechanism.
4. Keep the visible-critical prepump candidate default-off and high-alt-only until longer visual validation.

## Evidence That Guides The Next Fix

## Noncapture Surface/Pool Campaign Cycle - 2026-06-04

Artifacts:

- `build/captures/noncapture_playability_incremental_surface_20260604/table.md`
- `build/captures/noncapture_playability_incremental_surface_statsbatch_20260604/table.md`
- `build/captures/noncapture_playability_large_pool_65536_walk_20260604/table.md`
- `build/captures/noncapture_playability_large_pool_shared_column_walk_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Accepted code changes:

- `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=0` default-off.
  - When enabled with fixed range table, stable draw slots, and compact stable draws, new dirty surface bricks can append/patch metadata instead of rebuilding full metadata mirrors.
  - Walk noncapture stage line changed from `metadataFull=1` style full work to `metadataFull=0 metadataIncr=1`.
- `SparseSurfaceRangeAllocator` now supports behavior-preserving stats refresh batching.
  - Surface dirty staging batches allocator frees/resizes and recomputes allocator stats before upload ticket stats are read.
  - This is not an env-gated behavior change; allocation/free semantics are unchanged.
- `perf_noncapture_smoke.ps1` now includes `VENPOD_SPARSE_SURFACE_INCREMENTAL_METADATA_ADDS=1` in the candidate env and records nearest logged PERF frame when the exact target frame is absent.

Measured effect:

| Candidate row | Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Surface extract ms | Surface stage ms | GPU ray ms | Missing | Miss/unsafe | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| pre-surface fix | walk realtime | `600` | `246.81` | `90.60` | `49.52` | `26.35` | `7.94` | `21.86` | `55.24` | `5.70` | `450` | `0/0` | full surface metadata/stage dominated |
| incremental metadata | walk realtime | `600` | `76.55` | `59.26` | `24.94` | `12.30` | `18.54` | `9.71` | `23.36` | `18.57` | `450` | `0/0` | strong partial surface win |
| metadata + allocator stats batch | walk realtime | `600` | `117.90` | `48.05` | `26.41` | `11.72` | `6.34` | `10.93` | `4.11` | `19.63` | `428` | `0/0` | stage fixed; raw still bad |
| stats batch + large pool 65536 | walk realtime | `600` | `99.42` | `40.64` | `19.95` | `13.77` | `6.89` | `9.84` | `2.86` | `14.29` | `437` | `0/0` | replacement scans removed |
| shared column cache + large pool | walk realtime | `600` | `81.48` | `42.32` | `19.99` | `14.59` | `7.72` | `9.23` | `3.07` | `14.45` | `450` | `0/0` | not a reliable gen/clip win |

Consolidated current candidate stack:

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

Final consolidated noncapture table:

| Scenario | Frame | Raw ms | CPU sparse ms | Request ms | Gen ms | Clip ms | Pump ms | Surface extract/stage | GPU ray ms | Missing | Sampled/unsampled | Coverage | Miss/unsafe | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `375` | `21.39` | `16.27` | `10.93` | `4.55` | `0.79` | `0.00` | n/a | `6.93` | `0` | `0/0` | `100` | `0/0` | not 60, but closest fixed row |
| walk realtime | `600` | `112.09` | `64.87` | `33.39` | `23.55` | `7.87` | `5.28` | `19.46/5.76` | `18.08` | `447` | `378/69` | `95` | `0/0` | not playable; request/gen/surface/GPU dominate |
| high-alt | `399` | `53.11` | `29.52` | `7.92` | `5.49` | `16.10` | `5.94` | n/a | `14.49` | `1874` | `44/1830` | `99` visible / `79` cache | `0/0` | improved but not 60 |

Rejected or not accepted this cycle:

- Request resident fast-path lookup patch was tried and removed. It did not produce a reliable request win.
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1` was retested with large pool. It did not materially improve realtime walk and is not part of the candidate stack.
- `VENPOD_VSYNC=0` did not make the walk row playable; sparse/GPU work remained dominant.
- `VENPOD_SPARSE_SURFACE_PROMOTION_POLICY=bounded_repair` with bound `64` is not part of the performance stack; it promoted surface but drove walk clip/pump to about `71/70 ms`.
- `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_MAX_SPEED=64` was rejected for walk because it reduced request work by starving critical requests and causing clip catch-up.
- `VENPOD_SPARSE_MID_CLIPMAP_TARGETED_COVERAGE_CATCHUP` was tried and removed; it did not reduce the frame-600 debt.

Current decision:

- This is **not** a validated 60 FPS candidate.
- The surface metadata path is now a real accepted default-off partial fix.
- The allocator stats batching is a real behavior-preserving staging fix.
- `VENPOD_SPARSE_STATS_SINGLE_FLUSH=1` is a small accepted default-off request/stats win; default remains `0`.
- Larger sparse pool/page table is a useful opt-in playability candidate because it removes replacement scans, but it is not a default change.
- The current blocker after the accepted fixes is realtime walk request/generation/surface extraction/GPU. In the consolidated row, `requestMs=33.39`, `genMs=23.55`, `surfaceExtractMs=19.46`, `gpuRayMs=18.08`, and `clipMs=7.87`.

Next concrete implementation branch:

1. Split the remaining `PERF_SPARSE_REQUEST_DETAIL otherMs` bucket in walk; frame `600` had `requestMs=33.39`, `statsFlushMs=4.26`, `terrainCriticalMs=6.44`, `hierarchyMs=6.88`, and `otherMs=22.25`.
2. Inspect hidden-exact request/surface extraction coupling. The same walk frame had hidden-exact feedback accepted `53` coords and `surfaceExtractMs=19.46`.
3. Target one concrete CPU slice after the split: request bookkeeping/cache, hidden-exact admission, or surface extraction skip/budget for non-visible work.
4. Keep surface incremental metadata, stats single flush, background split, clean throttle, backlog pump, visible-critical prepump, and large pool as default-off candidate stack pieces until longer visual validation exists.

Sampled/projected missing-mid feedback:

- walk fixed-dt frame `600`: mostly projected-visible
- walk realtime frame `600`: mostly projected-visible
- noncapture walk earlier: mostly projected-visible
- high-alt late: mostly unsampled/cache debt

Fallback validity:

- fallback-valid subset remained `0` in sampled debt rows
- Far-SVO domain proof alone is not enough; material/occupancy/shoreline owner remains unknown
- high-alt rejects coarser mid parents by shader policy
- water/sky are shader/per-ray ownership decisions, not CPU-provable per brick

Rejected fixes:

- blunt pump cap: missing bricks/coverage loss/white terrain or shoreline risk
- age-priority queue ordering: did not solve high-alt/realtime debt
- ring-only visible-critical heuristic: too crude, caused parent-held visual failure explosion
- shared voxel column cache: helped one fixed-dt row, unreliable in realtime
- screen-critical request reuse env: did not reduce request cost enough
- surface upload interval: moved work into raw/stage spikes
- async generation: blocked for sampled movement debt because fallback-valid is zero

## Required First Implementation Slice

Fix or add reliable noncapture log-only validation before claiming playability.

Required behavior:

- run same sparse runtime env as capture smoke
- do not set `VENPOD_CAPTURE_DIR`
- force `VENPOD_LOG_FILE` to an absolute path
- run foreground and wait for exit
- fail if process detaches, log is missing, selected frames are missing, or `VENPOD_EXIT_AFTER_FRAMES` is ignored
- produce fixed, walk realtime, high-alt rows with:
  - `totalFrameMs` / `rawMs` / `bodyMs`
  - `cpuUpdateMs`
  - `requestMs`
  - `genMs`
  - `clipMs`
  - `surfaceStageMs`
  - `gpuRayMs`
  - missing/sampled/coverage/backlog
  - miss/unsafe

Do not continue claiming playable candidates from capture-only rows.

## Main Architecture Fix Plan

### Slice 1: Real Interest Classes

Convert mid-clipmap interest from one broad set into explicit classes:

```cpp
enum class MidClipmapInterestClass {
    VisibleCritical,
    SampledLikelyVisible,
    NearFuture,
    CachePrefetch,
    Maintenance
};
```

Each interested voxel brick should carry:

```cpp
struct MidClipmapInterestEntry {
    SparseVoxelClipmapCoord coord;
    MidClipmapInterestClass cls;
    bool projectedVisible;
    bool fallbackValid;
    bool fallbackUnknown;
    uint32_t firstSeenFrame;
    uint32_t lastSeenFrame;
    uint32_t priorityScore;
};
```

Rules:

- visible/projected-visible fallback-invalid or fallback-unknown remains critical
- cache/prefetch can be budgeted/deferred
- cache/prefetch is not dropped
- coverage guard uses visible-critical coverage, not broad cache coverage
- pump prioritizes visible-critical before near-future/cache/prefetch

The existing `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP` is a partial high-alt proof. Generalize it into this state model instead of adding another guard-only heuristic.

### Slice 2: Non-Ownership CPU Buckets

Movement debt is mostly sampled-visible, so deferring it is unsafe. Attack CPU work that does not change ownership:

1. Surface metadata staging:
   - fix full metadata work for small dirty/removed sets
   - implement incremental metadata patch/append/retire accounting
   - validate raw/body spikes, not just selected-frame CPU

2. Request prep cache:
   - cache request footprints by camera brick/cell and relevant policy
   - update by camera delta instead of rebuilding broad request sets
   - target request ms below `5-8 ms` in representative walk rows

3. Incremental scroll/reuse:
   - only if logs show unchanged resident bricks churn
   - update entering/leaving strips/rings
   - preserve edit/generation stamps

### Slice 3: Ownership Proof Before Async

Async is only allowed for:

- cache/prefetch bricks
- fallback-valid bricks
- non-visible near-future bricks with valid public owner

Async is forbidden for:

- sampled visible fallback-unknown bricks
- fallback-invalid bricks
- shoreline/mixed unknowns

Worker path requirements:

- worker generates CPU payload only
- GPU upload/apply happens on frame boundary
- queue dedupes by brick key
- stale completions discarded by generation/edit stamp
- upload/apply budget logged separately

## Validation Matrix For Every Real Candidate

Run:

1. fixed public / playable
2. walk fixed-dt frame `600`
3. walk realtime frame `600`
4. high-alt frame `400` or matched high-alt frame
5. noncapture fixed
6. noncapture walk
7. bounded64 comparison only when relevant

Record:

- total/raw/body
- cpuUpdate/request/gen/clip/pump/surfaceStage/surfaceExtract/trim/upload
- gpuRay
- currentInterest/missing/sampled/unsampled/fallbackValid/fallbackUnknown
- critical/cache coverage
- backlog depth/age
- miss/unsafe
- contact sheet verdict

## Scoreboard

60 FPS candidate requires:

- noncapture fixed `<=16.67 ms`
- noncapture representative walk `<=16.67 ms`
- no correctness or visual regression

Intermediate CPU targets:

- realtime/walk CPU under `35 ms`, then under `25 ms`, then under `16.67 ms`
- request under `5-8 ms`
- clip/pump under `8-10 ms` for walk
- surface staging no raw spikes above frame budget
- GPU ray under `10-12 ms` in playable stack

## Next Immediate Work Order

1. Implement/fix noncapture log-only harness.
2. Run fixed/walk/high-alt noncapture candidate stack.
3. If still over budget, implement incremental surface metadata staging.
4. Validate capture and noncapture.
5. Implement request prep cache if request still dominates.
6. Generalize visible-critical/cache interest classes.
7. Re-test combined playable stack.

Do not skip step 1.

---

# Historical VENPOD Rendering Handoff

Canonical current handoff: `debug-handoff.md`. Update that file first for session memory; this file is retained as historical checkpoint context.

## Checkpoint - 2026-06-02 Stop-and-Plan

Goal is active and not complete.

User-facing state:

- Rendering is still not coherent enough for public gameplay.
- Startup/public-open is less unsafe after the latest validated patch, but the
  world still shows slow terrain arrival and wrong-looking shoreline/mid/far
  ownership after public render opens.
- Do not claim this slice solved VENPOD rendering.

Code change kept in this checkpoint:

- `src/main_launcher.cpp`
  - `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` default changed from `0` to
    `1`.
  - Reason: same-camera shader-unsafe feedback is now treated as part of the
    public render contract. The world should not be revealed while lower LOD is
    known to be hiding non-ready exact-surface terrain for the current view.

Validation for the kept change:

- Build:
  - `.\build.ps1 -Config Release`
  - passed after the final cleanup build.
- Default capture after enabling the gate by default:
  - `build/captures/current_goal_v4c_shader_unsafe_default_on_20260602`
  - normal run, no env override.
  - frame 60: public render held, private warmup active,
    `shaderUnsafeContractNonReady=91`.
  - frame 100: public render still held, private warmup active,
    `shaderUnsafeClean=1/1`, `shaderUnsafeContractNonReady=0`,
    hidden exact missing/accepted `0`.
  - frame 120: public frame is clean for the strict exact contract:
    `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`, `miss=0`,
    `farHeight=0`, mid/far coverage `1.00/1.00`.

Important failed path removed before checkpoint:

- Tried to let startup private proof promote the wider exact surface raster band.
- Validation artifact:
  - `build/captures/current_goal_v4g_startup_wide_promotion_20260602`
- Result:
  - No useful delta. At frame 120, `surfaceRasterMax` remained `1024`,
    `surfacePromoted=0`, and `midVoxelScreenPct` remained about `19.36%`.
  - That patch was reverted. Do not repeat the same edit blindly.

Remaining proven blocker:

- After public render opens, the visible frame is still exact-near only:
  - `surfaceRasterMax=1024`
  - `surfacePromoted=0`
  - about `19%` of the screen is still `mid_voxel`
  - shoreline/basin bad-pixel audit still finds mid/Far-SVO coarse ownership.
- The current policy waits for `sparseHiddenExactRuntimeCleanFrames >= 8` before
  wide raster promotion, but during startup/private warmup that runtime clean
  counter does not advance enough. At frame 120, hidden exact has no missing
  requests, yet `hiddenExactClean=0/8` and wide promotion is still blocked.
- This is a foreground handoff/promotion policy issue, not a material/color
  issue.

Post-open evidence:

- `build/captures/current_goal_v4e_post_open_basin_audit_20260602`
  - sampleCount `512`
  - generated terrain above sea level, not missing water: `411`
  - water-plane before CPU terrain, inspect water ownership: `47`
  - visible water: `44`
  - coarse fallback owns basin terrain: `9`
  - dominant owner/material/face buckets:
    - `far_svo|stone|mixed = 125`
    - `mid_voxel|stone|side = 93`
    - `mid_voxel|dirt|mixed = 93`
    - `mid_voxel|water|top = 44`
- Interpretation:
  - Most basin patches are generated terrain, not missing water.
  - There is a smaller water/shoreline ordering issue, mostly far-SVO stone
    mixed cells where the CPU-side water plane is about 1-2 units before terrain
    at t about 1056-1496.
  - Do not patch water first unless the next owner-contract audit proves it is
    the dominant user-facing failure.

Diagnostic tooling note:

- Mode `70` water-reason debug is still not reliable. A quick attempt to include
  it in `BackgroundDebugLayerMode()` caused a very slow/hung shader run and was
  reverted. Treat mode 70 as debug-tooling debt, not as a rendering fix.

Next plan:

1. Stop broad/aesthetic work.
2. Fix the post-open foreground handoff policy:
   - understand why wide exact surface promotion remains blocked after current
     shader unsafe and hidden-exact feedback are clean;
   - likely culprit is use of the runtime clean-frame counter during startup,
     where private warmup/pressure mode prevents the counter from advancing.
3. Validate the next fix by requiring a real delta in the same default view:
   - `surfacePromoted=1`
   - `surfaceRasterMax > 1024`
   - `midVoxelScreenPct` materially lower than about `19%`
   - `miss=0`, `farHeight=0`, no proxy/fake fallback regression.
4. Only after foreground handoff is correct, revisit the smaller
   water/shoreline ordering rows.

## Current State - 2026-06-02 User-Facing Rendering Still Broken

Goal is active and not complete.

User-facing truth:

- The latest confirmed code fix changed the default Far-SVO page radius from 6
  to 12. That fixed one real owner-contract bug: broad views were leaving the
  Far-SVO domain and falling back to analytic far-height continuity.
- That fix did not make VENPOD a coherent public renderer. The user still sees
  slow terrain arrival, shoreline/water gaps, partial exact chunks, and
  wrong-looking terrain in live views.
- Do not treat passing stress captures or `farHeight=0` as completion. Those
  only prove one failure class is closed for tested cameras.

Current suspected blocker:

- VENPOD is still exposing internal streaming states in public frames. It mixes
  exact sparse surface, mid voxel, Far-SVO, water, and sky while some visible
  terrain is not render-ready for the current view.
- Streaming while rendering is normal. The bug is the missing public-frame LOD
  contract: if exact is absent, mid/far/water must be a legitimate owner for
  that pixel, and if deterministic terrain exists, sky/miss/water must not win
  accidentally.
- Existing readiness metrics can still be misleading because they report global
  cache/readiness state, not necessarily the exact visible footprint for the
  current camera.

Next work must stay mechanism-facing:

1. Reproduce the current live broken view from the user's screenshots.
2. Classify current bad pixels by final owner and terrain truth:
   exact / mid / Far-SVO / water / sky / miss.
3. Patch only the proven owner-contract failure, not materials or terrain
   aesthetics.
4. Validate by showing the same view no longer exposes incomplete or wrong LOD
   ownership, not merely by passing a generic smoke gate.

## Current State - 2026-06-02 v3v Far-SVO Domain Fix Landed

Goal is active and not complete.

User-facing truth:

- Rendering is not declared solved, but one major public-owner failure class is
  now fixed: broad/high/default views no longer need analytic far-height because
  the Far-SVO grid was too small.
- The previous root-cause thread was correct: VENPOD reported Far-SVO "ready"
  for its configured cache, but the configured cache only covered radius 6
  pages. Public rays could leave that domain, so deterministic terrain had no
  resident mid/Far-SVO owner and fell back to analytic far-height continuity.
- This was an architecture/readiness mismatch, not a material, water, or terrain
  generation issue.

Code changed in this slice:

- `src/Graphics/FarVoxelOctree.h`
  - Default `FarVoxelOctreeConfig::pageRadius` changed from `6` to `12`.
  - This makes the default Far-SVO domain 25x25 pages at page size 1024
    (`25600` world units coverage) instead of 13x13 pages (`13312` units).
- `src/Graphics/SparseVoxelGpuResources.cpp`
  - Render ownership log now prints far-height mid sample total and stored
    count as `midSamples/stored`.
  - Readback loop variable warning cleaned up.
- The attempted HLSL `ring+1` marker experiment was reverted because the new
  PS_Raymarch binary stalled before capture/PSO completion. Do not retry that
  marker path without first isolating shader iteration/PSO cost.

Validation artifacts:

- Baseline before default change:
  - `build/captures/current_goal_v3t_mid_feedback_stored_count_20260602`
  - failed strict `-MaxHeightProxyScreenPct 0`
  - frame 90: `farHeight=13379`,
    `farHeightReason=13379/13379/0/0/0/13379/13379/0/0`
  - meaning: all far-height pixels were continuity fallback, outside mid
    diagnostic range (`midSamples=0`) and outside Far-SVO page grid
    (`farPageOutGrid=farHeight`).
- Radius-12 control:
  - `build/captures/current_goal_v3u_farsvo_radius12_control_20260602`
  - cold-built `venpod_far_svo_cache_r12_d6_s12345.bin`
  - cache: 625 pages, 19,747,106 nodes, 301.33 MB, build about 25s, GPU upload
    about 74ms.
  - far-height dropped to `0` through frames 80-120.
- Default validation after code change:
  - `build/captures/current_goal_v3v_default_farsvo_radius12_20260602`
  - passed strict stress smoke with `-MaxHeightProxyScreenPct 0`
  - `maxHeightProxyScreen=0.00%`, `miss=0`, terrain-critical `postNonReady=0`
  - Far-SVO loaded from cache in about 295ms at startup.
- Default timeline check:
  - `build/captures/current_goal_v3w_default_timeline_radius12_20260602`
  - wrapper aborted because requested frame 1 was held by startup gate, but BMPs
    for 121/241/361 were saved.
  - public frames show coherent terrain compared with the old all-water/holes
    screenshots.
  - frame 120/240/360: `farHeight=0`, `miss=0`,
    `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`,
    `midCov=1.00/1.00`, `farCov=1.00/1.00`.

Current blocker after this fix:

- The broad Far-SVO domain gap is fixed for the tested stress/default views.
- Startup still holds early frames while mid/exact readiness proves clean; that
  is now a loading/readiness-policy issue rather than the far-height fallback
  renderer bug.
- User-visible rendering still needs more validation in live/free-camera views
  before goal completion. Next work should be a fresh public-view inventory from
  current code, focused on any remaining no-owner, waterline, or exact/mid
  contract failures. Do not go back to stone/material tuning or terrain shape
  unless a fresh owner audit proves geometry/material is the active blocker.

Next mechanism-facing work:

1. Run/inspect a normal `rebrun` or public-demo capture using the default
   radius-12 cache, including the user-facing camera class that previously
   showed shoreline holes.
2. If broken pixels remain, audit their owner reasons against the current
   contract: exact, mid, Far-SVO, water, sky, or miss.
3. Patch the next concrete owner-contract failure only. Do not weaken gates or
   hide gaps cosmetically.

## Current State - 2026-06-02 v3o Public LOD Coverage Root Cause

Goal is active and not complete.

User-facing truth:

- Rendering is still broken in live/default gameplay views. Startup is less
  stalled than before, and close foreground can look more coherent, but broad
  and shoreline views still expose slow streaming, gaps, and wrong-looking LOD
  ownership.
- The current blocker is not material color, water styling, terrain generation,
  or "startup scope." The renderer is still sometimes showing public frames
  where deterministic terrain exists but no resident voxel LOD owner is
  available, so the shader falls back to analytic far-height continuity.
- Streaming while rendering is normal and not itself the bug. The bug is that
  VENPOD's public owner contract is incomplete: exact, mid, far SVO, water, and
  sky are not always selected from a fully resident/current-view coverage set.

Newest evidence:

- Stress capture with far-height reason counters:
  - `build/captures/current_goal_v3o_farheight_reason_counters_20260602`
  - frame 90: `farHeight=13379`,
    `farHeightReason=13379/13379/0/0/0/13379`
  - frame 120: `farHeight=8895`,
    `farHeightReason=8895/8895/0/0/0/8895`
  - frame 130: `farHeight=9803`,
    `farHeightReason=9803/9803/0/0/0/9803`
- Meaning of the reason counters:
  - every farHeight pixel came from the continuity fallback path in
    `DebugBackgroundMissHit()`;
  - every sampled fallback point had missing resident mid voxel coverage;
  - every sampled fallback point also had missing Far-SVO page coverage;
  - none of these pixels were explained by mid sampled air, mid solid, or the
    normal background height path.

Current root cause:

- Broad/high/current-view terrain coverage is not being requested or resident
  for the actual visible footprint. The engine can report mid/Far-SVO
  "coverage" or "ready" globally while the specific diagnostic terrain points
  visible in the frame have neither a resident mid voxel sample nor a Far-SVO
  page.
- Exact sparse feedback can repair some nearby terrain after the frame, but that
  is not a valid public-frame owner for these broad views. Lower LOD must be
  present and legitimate before analytic continuity is needed.

Do not do next:

- Do not tune stone/material colors.
- Do not change water visuals.
- Do not change terrain shape/closure/skyline.
- Do not retry the removed broad per-pixel Far-SVO traversal helper; it caused
  GPU readback instability and heavy frame time in
  `current_goal_v3n_stress_far_svo_continuity_patch_20260602`.
- Do not claim smoke gates prove this is solved while `farHeight` analytic
  fallback is still visible.

Next mechanism-facing work:

1. Inspect mid clipmap and Far-SVO interest/page request generation for the
   high/default visible footprint.
2. Find why the visible diagnostic terrain points are outside resident mid/Far
   coverage despite readiness metrics.
3. Patch request/coverage policy so the public frame has a real voxel owner
   before `BuildDeterministicFarTerrainContinuityHit()` is needed.
4. Validate with a stress capture that includes `-MaxHeightProxyScreenPct 0`;
   target is `farHeight=0`, `miss=0`, no fake proxy/water regressions, and no
   GPUBuffer map/readback instability.

## Current State - 2026-06-02 Public Contract v2o: Hidden-Exact Requests Prioritized, Current-View Proof Still Late

Goal is active and not complete.

User-facing truth:

- Rendering is still not solved. Close/foreground exact terrain is better than the old fully broken first-frame view, but public views can still show slow terrain repair, shoreline/water inconsistency, and lower-LOD gaps.
- The current root is not material color or terrain shape. The engine is still exposing streaming/ownership internals: exact sparse surface, lower LOD terrain, and water are not yet bound by a coherent public-frame contract.
- The architecture is not hopeless. Streaming while rendering is normal. The broken part is that VENPOD displays frames while current-view exact-contract proof is stale or incomplete, and lower LOD/water does not always carry those pixels legitimately.

Fresh validated artifacts:

- Baseline latest budget/water-plane run:
  - `build/captures/current_goal_v2m_frame240_pre_render_priority_wide_20260602`
  - frame 240: `surface=55.3743%`, `midVoxel=15.0673%`, `farSvo=0.7324%`, `farWater=3.7799%`, `miss=0`, `heightProxy=0`
  - pre-render hidden exact at frame 240: `visited=389/9417`, `timeExpired=1`, `feedback=158`, `accepted=112`, `stillMissing=112`
  - post-render shader feedback for `ownerFrame=240`: `contractNonReady=111`, repaired by frame 247
  - camera exposure still reported clean from stale owner proof: `shaderUnsafeAge=15`, `shaderUnsafeContractNonReady=0`
  - water lifecycle: `water_exact_exists_but_not_visible_order_or_depth=45`, `water_brick_not_resident=32`
- Strict hidden-miss demotion control:
  - `build/captures/current_goal_v2m_frame240_strict_hidden_demote_control_20260602`
  - made lower-LOD ownership worse: `surface=39.697%`, `farWater=11.9537%`, post-render `contractNonReady=163`
  - conclusion: simply collapsing wide exact raster when hidden misses exist is not a fix; lower LOD/water cannot yet carry the view coherently by itself.
- Failed duplicate-water-probe removal:
  - `build/captures/current_goal_v2n_frame240_no_duplicate_water_probe_20260602`
  - worsened post-render `contractNonReady=190` and raised `farWater=5.7932%`
  - reverted as a direction; do not resurrect it blindly.
- Validated priority-lane patch:
  - `build/captures/current_goal_v2o_frame240_hidden_exact_critical_lane_20260602`
  - hidden-exact pre-render reserve rejects dropped to `0`
  - pre-render `stillMissing` dropped from `112` to `38`
  - layer mix stayed roughly stable: `surface=55.5135%`, `farWater=3.8759%`, `miss=0`, `heightProxy=0`
  - post-render shader feedback still found `contractNonReady=122`, repaired by frame 247
  - conclusion: hidden-exact requests were too low priority, but request priority alone does not solve current-view proof.
- Failed submitted-active tracking experiment:
  - `build/captures/current_goal_v2p_frame240_track_submitted_active_hidden_exact_20260602`
  - failed terrain-critical gate with `postMissing=48` on frames 244-246
  - reverted. Over-broadly promoting submitted-active hidden-exact coords into the critical set starves/displaces actual terrain-critical work.

Code state after v2o:

- `src/main_launcher.cpp`
  - Hidden-exact pre-render requests now use the protected render-critical request lane and high queue priority:
    - `terrainCriticalRequest=true`
    - priority `60000` for terrain, `70000` for water
  - The failed submitted-active critical tracking experiment has been reverted.
  - The failed duplicate-water-pass removal has been reverted.
  - Existing water-plane alignment and wider post-open probe defaults remain in place.
- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Debug mode `70` still exists for waterline resolver reasons. It is diagnostic only.

Current blocker:

- CPU pre-render hidden-exact proof is too slow/sparse for moving public views. At frame 240 it times out after only a few hundred general rays, while shader owner feedback later proves that many current-frame exact-contract bricks were still non-ready.
- Shader owner feedback is the accurate proof, but it arrives after the frame was displayed. This is the core disconnect: public render is using stale/old proof (`ownerFrame=225`) to display a new camera frame (`frame=240`), then post-render repair discovers the actual non-ready terrain.
- Lower LOD/water cannot yet be trusted as a coherent fallback for those missing exact-contract pixels. The strict demotion control increased far-water and non-ready ownership rather than fixing the scene.

Next concrete work:

- Do not tune material colors, skyline closure, terrain shape, route composition, or fake fallbacks.
- Do not rely on smoke gates alone; they can pass while post-render owner feedback repairs the already displayed frame.
- Next mechanism-facing fix should address one of these directly:
  1. Make current-view proof available before display, likely by a private/current-camera owner prepass or public-frame hold until same-camera owner proof is clean.
  2. Or make the lower LOD/water fallback truly coherent for exact-missing pixels, then prove strict demotion no longer increases `farWater`/`contractNonReady`.
  3. Or add a camera-motion/stale-proof guard so old owner feedback cannot mark a new moving view clean.
- Any next patch must be validated against frame 240 with:
  - `contractNonReady` for `ownerFrame=240`
  - `hiddenExactMissing`
  - water lifecycle reasons
  - layer mix (`surface`, `farWater`, `midVoxel`, `miss`, `heightProxy`)
  - no terrain-critical gate failure.

## Current State - 2026-06-02 Public Contract v2j: Waterline Lower-LOD Arbitration Still Broken

Goal is active and not complete.

User-facing truth:

- Rendering is still broken in public playable views. It is somewhat faster and closer up, but distant terrain still streams in slowly and shoreline/water pixels remain inconsistent.
- Do not claim this is solved. The current problem is not material color, terrain closure, route composition, or fake fallback. It is the public voxel ownership contract around exact sparse terrain, lower LOD terrain, and deterministic water.
- Streaming while rendering is normal. VENPOD's current bug is that public frames are allowed to show a mixed, incoherent set of owners while exact sparse terrain and waterline ownership are still being repaired.

What was proven after v2c:

- Exact foreground publishing improved the old shoreline patchwork, but did not finish the contract.
- Dedicated hidden exact water probing improved water exact request coverage:
  - v2d: `waterFeedback=0`, still `22/24` water bricks missing.
  - v2e/v2f: water requests started happening; missing dropped to `14/24`.
  - v2g with water brick radius `1` reduced missing to `4/24` but overflowed sparse surface upload capacity, so radius `1` is not a safe default.
- Exact sparse surface water-plane discard reduced one blocker, but the current bad sampled pixels are still mostly owned by raymarched lower LOD, especially far SVO.
- Latest v2j validation:
  - normal capture: `build/captures/current_goal_v2j_frame240_foreground_waterline_owner_20260602`
  - owner debug: `build/captures/current_goal_v2j_frame240_debug_owner_mode55_20260602`
  - basin audit: `build/captures/current_goal_v2j_frame240_basin_water_audit_20260602`
  - layer frame 240: `surfaceScreenPct=51.5461`, `midVoxel=16.477`, `farSvo=0.7374`, `farWater=6.1637`, `miss=0`, `heightProxy=0`, overflow `0`
  - lifecycle still: `water_brick_not_resident=14`, `water_exact_exists_but_not_visible_order_or_depth=10`
  - targeted basin audit still reports `water_should_draw_before_terrain=70`, with owners mostly `far_svo=60`, `mixed=9`, `sky=1`

Current root blocker:

- Raymarch/lower-LOD arbitration is still letting far SVO or mid voxel own pixels where deterministic water or exact sparse terrain should own earlier on the ray.
- The likely next split is:
  - the water resolver rejects because shader `FarTerrainHeight` says land while the CPU/audit expects water,
  - or the final far-SVO path bypasses the water/exact foreground resolver,
  - or the debug/audit classifier is misidentifying far water vs far SVO.

Next concrete work:

- Do not keep changing tolerances blindly.
- Add a reason-coded same-camera diagnostic for why `TryResolveDeterministicWaterBeforeBackground` rejects the v2j `water_should_draw_before_terrain` pixels, or run an equivalent CPU/shader parity audit for those exact waterline positions.
- Patch only after that diagnostic proves whether the active bug is CPU/GPU terrain truth drift, resolver bypass, owner classification error, or an exact water publish lifecycle issue.

## Current State - 2026-06-02 Public Contract v2c: Shoreline Exact Publish Fix

Goal is active and not complete.

User-facing truth:

- Rendering is improved, not finished. The low-shoreline/water patchwork class is materially better, but broad/far terrain can still look rough and needs another focused pass.
- The current fixed mechanism was not material tuning or terrain generation. It was the exact foreground publish contract: lower LOD/water was carrying public pixels while exact sparse terrain was still stuck behind high-altitude throttles and surface-ready page-table publish lag.

What was proven:

- Previous default low-shoreline capture:
  - `build/captures/current_goal_startup_hidden_repair_default_v1_lowshore_160_520_20260602`
  - smoke summary: `maxFarWaterScreen=23.59%`, `minVoxelTerrainScreen=47.46%`
  - frame 240 timeline: `surfaceScreenPct=26.4332`, `farWaterScreenPct=20.6716`
  - camera exposure around frame 240 could show `surfaceRasterMax=1024`, `surfacePromoted=0`, terrain critical `lodThrottle=1`, and shader feedback then found hundreds of exact-contract non-ready bricks.
- Control with wide exact surface and higher surface record cap improved visuals but exposed a publish/surface bottleneck:
  - `build/captures/current_goal_lowshore_no_highalt_lod_records65535_control_160_520_20260602`
  - old 32k surface record cap no longer overflowed, but transient terrain-critical `UploadingGPU` remained because page-table publish/surface-ready work lagged.
- Control raising only post-open pre-publish surface extraction time proved the remaining mechanism:
  - `build/captures/current_goal_lowshore_public_contract_v2b_surface40_control_160_520_20260602`
  - passed with `postNonReady=0`, `maxFarWaterScreen=11.41%`, `minVoxelTerrainScreen=56.81%`

Code changed in this slice:

- `src/Graphics/SparseSurfaceGpuResources.h`
  - Default `maxDrawCommands` / surface record capacity is now `65535`.
  - Reason: wide exact-surface ownership can require more than the old 32768 records; the old cap caused overflow when exact terrain was allowed to carry shoreline views.
- `src/main_launcher.cpp`
  - High-altitude exact throttling is disabled when the last public frame has far-water ownership risk or exact-repair pressure.
  - High-altitude exact raster demotion is also blocked for far-water-heavy / exact-repair-heavy views.
  - Default `VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS` is now `40` instead of `8`.
  - Reason: page-table publish is surface-ready-gated. Without enough post-open pre-publish surface extraction, exact bricks can sit in `UploadingGPU` and public frames keep showing water/lower-LOD patchwork.

Validation after patch:

- Build passed:
  - `.\build.ps1 -Config Release`
  - only existing `rayDir` shadow warnings.
- Low-shoreline default capture passed:
  - `build/captures/current_goal_lowshore_public_contract_v2c_default_160_520_20260602`
  - smoke summary: `maxMiss=0`, `maxHeightProxyScreen=0.01%`, `maxFarSvoScreen=3.06%`, `maxFarWaterScreen=12.80%`, `minVoxelTerrainScreen=55.46%`
  - terrain-critical: `readyFrame=160`, `postNonReady=0`
  - frame 240: `surfaceRasterMax=2560`, `surfacePromoted=1`, `lodThrottle=0`, `shaderUnsafeContractNonReady=0`, `sameFramePublish=60/60`
  - frame 240 timeline: `surfaceScreenPct=49.2749`, `farWaterScreenPct=7.7735`
- Normal default smoke passed:
  - `build/captures/current_goal_public_contract_v2c_default_smoke_20260602`
  - terrain-critical: `readyFrame=200`, `postNonReady=0`

What this changed for the user:

- Public shoreline frames no longer stay in the old mode where high-altitude throttling says lower LOD is fine while exact terrain is visibly missing.
- Exact foreground terrain now gets promoted/published earlier, reducing water/LOD patchwork around shorelines.

Remaining issues / next work:

- Do not claim the engine is finished. Distant/broad lower-LOD terrain still looks rough and there are still visible far/mid representation issues.
- Next focused path should use the new v2c capture as baseline and inspect remaining visible gaps by owner:
  - exact surface actually absent vs mid/far representation,
  - legitimate water vs water over terrain,
  - far/mid geometry quality after exact publish is no longer the dominant bottleneck.
- Do not return to material color tuning, terrain shape closure, route composition, or fake fallbacks unless a same-camera owner audit proves that is the active failure.

## Current State - 2026-06-02 Startup Hidden-Exact Repair Default v1

Goal is active and not complete.

User-facing truth:

- Rendering is still not fixed. The first-public startup path is better, but moving/general shoreline views can still show slow terrain repair, water/shoreline gaps, and lower-LOD ownership that looks wrong.
- The current blocker is the public voxel ownership/readiness contract, not material color tuning. Public frames must not mix partial exact terrain, coarse fallback, and water in ways that hide deterministic terrain.
- The architecture is not hopeless: streaming while rendering is normal. VENPOD's problem is that lower LOD, water, exact sparse surface, and exact repair are not yet tied into a single coherent public-frame contract.

Newest code changed:

- `src/main_launcher.cpp`
  - Defaulted `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP` to `1`.
  - Defaulted `VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS` to `1`.
  - Defaulted `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_CLEAN_FRAMES` to `1`.
- `rebrun.ps1`
  - Defaults startup hidden-exact repair on unless the caller explicitly overrides it.
- `engine_capture_smoke.ps1`
  - Defaults startup hidden-exact repair on instead of disabling/removing it.

Evidence before patch:

- Artifact: `build/captures/current_goal_fresh_default_public_120_400_20260602`
- Capture request starting at frame 120 only produced frames 200-440 because public render was held while startup repair worked.
- Runtime log showed repeated reactive shader feedback waves after owner readback:
  - frame 54 owner feedback: `contractNonReady=253`, `requested=48`
  - later owner frames produced new hidden exact non-ready batches
- This proved the old public path was opening/repairing from feedback instead of prewarming the first public view.

Control before patch:

- Artifact: `build/captures/current_goal_startup_hidden_repair_control_120_400_20260602`
- Enabling startup hidden-exact repair manually requested thousands of hidden exact bricks before public render.
- By frame 100, hidden repair converged with `hiddenExactProofStillMissing=0`.
- By public frame 160, shader unsafe feedback had `shaderUnsafeNonReady=0/0` and `shaderUnsafeContractNonReady=0`.

Validation after patch:

- Build passed:
  - `.\build.ps1 -Config Release`
  - Only existing `rayDir` shadow warnings.
- Static public capture passed:
  - `build/captures/current_goal_startup_hidden_repair_default_v1_120_400_20260602`
  - all 8 requested frames captured from 120
  - `maxMiss=0`
  - `maxHeightProxyScreen=0`
  - `maxFarSvoScreen=0.09%`
  - `maxFarWaterScreen=0.35%`
  - terrain-critical `readyFrame=120 postNonReady=0`
- Walking capture passed:
  - `build/captures/current_goal_startup_hidden_repair_default_v1_walk_120_560_20260602`
  - `maxMiss=0`
  - `maxHeightProxyScreen=0.01%`
  - `maxFarSvoScreen=0.95%`
  - `maxFarWaterScreen=7.61%`
  - terrain-critical `readyFrame=120 postNonReady=0`
- High/free stress capture passed but remains rough:
  - `build/captures/current_goal_startup_hidden_repair_default_v1_highflight_240_480_20260602`
  - `maxMiss=0`
  - `maxHeightProxyScreen=1.60%`
  - `maxFarSvoScreen=54.99%`
  - `maxFarWaterScreen=45.17%`
  - Runtime still had motion-triggered shader exact-contract repair bursts after public open.
- Low-shoreline stress capture completed and must be inspected next:
  - `build/captures/current_goal_startup_hidden_repair_default_v1_lowshore_160_520_20260602`
  - `maxMiss=0`
  - `maxHeightProxyScreen=0.01%`
  - `maxFarSvoScreen=3.07%`
  - `maxFarWaterScreen=23.59%`
  - `minVoxelTerrainScreen=47.46%`

What this proves:

- Startup hidden-exact repair is a real improvement for the first public frame. It is not a material tweak or fake fallback.
- It does not solve the full public contract. Moving/broad/shoreline views can still expose wrong-looking water/lower-LOD ownership and reactive exact repair.

Next concrete work:

- Inspect the low-shoreline contact sheet and logs because it best matches the user's current screenshots.
- For the worst current shoreline frames, run same-camera owner/material/water debug and decide whether bad pixels are:
  - water owning where deterministic terrain should exist,
  - mid/far legitimately carrying but representing too coarsely,
  - exact terrain late/non-ready after camera motion,
  - or a gate/owner contract mismatch.
- Patch only the proven mechanism. Do not return to material tuning, skyline closure, terrain generation, or fake fallback changes without direct evidence.

## Current State - 2026-06-02 Startup Public Contract / Shader Unsafe Default v1

Goal is active and not complete.

User-facing truth:

- Rendering is still not finished, but one real public-contract bug was fixed in this slice.
- The old default opened public render too early. Public render could open around frame 44, while shader unsafe feedback at frame 54 still reported hundreds of exact-contract samples as non-ready.
- That meant the first visible world was a partial exact/mid/far/water mix that continued to assemble in front of the user. Metrics like `miss=0` and `heightProxy=0` were true but incomplete evidence.

Evidence before patch:

- Default first-public/static capture:
  - `build/captures/current_goal_water_occluder_guard_v1_static_public_061_241_20260602`
  - public frames around 61 still showed slow owner transition.
  - surface rose from about `40.9%` to `57.7%`.
  - far-water fell from about `8.25%` to `0.55%`.
- Strict control with `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS=1`:
  - `build/captures/current_goal_startup_shader_unsafe_blocks_control_061_141_20260602`
  - public capture did not appear until frame 181/201 because private warmup was still proving/repairing the exact contract.
  - first visible frame was materially more stable.

Code changed:

- `src/main_launcher.cpp`
  - Changed default `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` from `0` to `1`.
  - Public render now stays in private warmup until shader unsafe/exact-contract proof is clean. This is not a material/fallback tweak; it prevents exposing the mixed partial-exact startup image.
- `engine_capture_smoke.ps1`
  - Changed default capture start from frame `120` to `200`, because frame 120 can now be intentionally held/private and is not a public-terrain sample.
- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Added the same dry-background guard to precomputed far-water occluder resolution that the local water path already used.
  - Validation showed this was safe but not the dominant mover for the slow public assembly issue.

Validation after patch:

- Build:
  - `.\build.ps1 -Config Release`
  - passed with only existing `rayDir` shadow warnings.
- Default smoke:
  - `.\engine_capture_smoke.ps1 -Config Release -NoBuild`
  - artifact: `build/captures/engine_sparse_20260601-231246-664`
  - passed.
  - terrain-critical `readyFrame=200`, `postNonReady=0`.
  - `miss=0`, `heightProxy=0`.
  - captured public temporal deltas were low after open.
- Default static proof range:
  - `build/captures/current_goal_startup_shader_unsafe_default_v1_181_261_20260602`
  - passed.
  - max public `farWaterScreenPct` in sampled range was about `0.94%`, down from the old early-public `8%` class.
- Walk retry:
  - `build/captures/current_goal_startup_shader_unsafe_default_v1_walk_200_400_retry_20260602`
  - passed.
  - `miss=0`, `heightProxy=0`, `maxFarSvoScreen≈0.35%`, `maxFarWaterScreen≈2.18%`.

Remaining blockers:

- Do not mark the goal complete.
- High/free broad camera still renders mostly through far SVO/far water and is expensive:
  - `build/captures/current_goal_startup_shader_unsafe_default_v1_highflight_200_400_20260602`
  - GPU raymarch around `90-116 ms` in sampled broad frames.
  - high/free `surfacePromoted=0`, so lower LOD is intentionally carrying the view, but the broad-view quality/perf is not yet a Minecraft-like final state.
- Exact surface still increases over time after public open in normal views, though the visible temporal delta is much smaller than before. The next work should target whether remaining promotion is legitimate detail refinement or still visible owner correction.
- Do not return to material tuning. Continue with owner/LOD contract evidence: broad-view far SVO/water legitimacy, moving-camera lower LOD quality, and any current visible shoreline/water holes.

## Current State - 2026-06-02 High/Free-Camera Public LOD Contract v1

Goal is active and not complete.

User-facing truth:

- Rendering is still not solved. The engine is closer, but public frames can still show slow terrain catchup and gaps/incorrect lower-LOD ownership, especially in broad high/free views and around water/shorelines.
- The architecture is not inherently wrong because rendering and streaming can coexist, but VENPOD's current public LOD contract is leaky: exact, mid, far SVO, far-height continuity, water, and request feedback are not consistently agreeing on who owns a visible pixel while exact terrain is still loading.
- The current blocker is not scope and not material tuning. It is the runtime contract for voxel ownership/readiness: if exact sparse surface is unavailable, the lower LOD must be coherent immediately, and exact repair must continue without letting sky/water/fake height or partial exact chunks produce broken terrain.

Recent code changed:

- `src/main_launcher.cpp`
  - High/free-camera public render now suppresses wide exact-surface promotion when lower LOD is already carrying the frame coherently.
  - In those high-altitude cases, `surfaceRasterMax` is capped back near `exactNear` instead of expanding to wide repair distance.
  - Mid clipmap catchup throttling now considers combined lower-LOD ownership from `farSVO + farWater`, not just far SVO alone.
- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Existing hidden-exact feedback repair also covers far-height fallback hits, so far-height continuity cannot silently hide missing exact terrain.

What improved:

- Default public smoke and scripted walking captures passed after the balanced public gate work:
  - `miss=0`
  - `heightProxy=0`
  - terrain-critical ready by first sampled public frames.
- High/free-camera exact surface over-promotion was reduced:
  - high/free `surfaceRasterMax` dropped from `2560` to `1024`
  - high/free exact surface screen share around frame 240 dropped from about `17.2%` to about `3.7%`
- High/free CPU streaming pressure improved:
  - frame 236 mid clipmap pump dropped from about `115 ms` to about `27 ms`
  - frame 236 total frame time dropped from about `202 ms` to about `106 ms`

What remains broken:

- High/free camera validation still failed a terrain-critical readiness check with one exact brick in `UploadingGPU` at frame 236.
- There is likely a contract mismatch or publish-ordering bug:
  - public lower LOD appears to cover the frame with `miss=0` and no unsafe near miss,
  - but the terrain-critical gate still blocks on an exact brick that may be non-public-critical under the high-altitude LOD policy,
  - or the upload/publish path is counting work before `GetRenderReadinessState` actually reports drawable.
- GPU raymarch cost remains high in broad views; CPU streaming improved, but the broad-view frame is still expensive.
- User screenshots still show slow far terrain assembly and persistent gaps near water/shoreline. Those are not solved by the latest patch.

Next concrete debugging path:

- Inspect the protected upload/publish/readiness path for the frame-236 `UploadingGPU` brick.
- Decide with evidence whether it is:
  - a real GPU surface/page-table publish ordering bug, or
  - a terrain-critical gate using the wrong exact-ownership contract for high/free LOD views.
- Then validate against the actual user-visible issue: broad public frames should render coherently immediately from lower LOD, while exact surfaces upgrade without holes, water takeover, or fake height masking.

## Current State - 2026-06-02 Far-Height Hidden Exact Repair Gate v1

Goal is active and not complete.

User-facing truth:

- Rendering is still not solved. The engine is better than before, but public frames can still look like terrain is slowly assembling because lower LOD / fallback owners sometimes carry pixels while exact/surface terrain is still being repaired.
- The architecture is not inherently impossible: Minecraft-style streaming also renders and streams concurrently. The current VENPOD blocker is a leaky public LOD contract, not "streaming while rendering" by itself.
- The concrete failure pattern is: exact terrain that should exist can be missing/non-render-ready, while mid/far/far-height fallback draws later terrain and hides the missing exact brick from the normal feedback path.

New mechanism found:

- In high/free-camera stress views, frame 350 showed exact-contract hidden samples in `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK`, but foreground repair was suppressed because the high-altitude LOD gate was active.
- Before patch:
  - `contractSamples=6`
  - `nonReady=6`
  - `contractNonReady=6`
  - states were `Missing`
  - `fgRepair=active/eligible/requested/tracked/cap:0/6/0/0/0`
- That means public terrain could be drawn by lower LOD/far-height continuity while the exact-contract bricks stayed missing.

Code changed:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Hidden exact feedback now includes `BACKGROUND_LAYER_FAR_HEIGHT` fallback hits, not only mid voxel, far SVO, and far water.
  - Reason: far-height continuity fallback should not silently bypass exact repair when it hides exact-contract terrain.
- `src/main_launcher.cpp`
  - Foreground shader feedback repair is now allowed during high-altitude LOD only when the same owner frame actually used far-height fallback pixels.
  - Reason: high-altitude LOD may be allowed to carry public pixels, but it must not suppress repair of exact-contract missing bricks hidden by that fallback.

Dead-end tested and reverted:

- Increased high-altitude far-SVO search/page budgets in `PS_Raymarch.hlsl`.
- Validation showed no material change in far-height screen share, so the patch was reverted.
- Do not return to SVO budget tuning unless a new audit proves SVO search budget is the blocker.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Before/failure artifact:
  - `build/captures/current_goal_highflight_midclosure_feedback_v1_frame350_20260602`
- Reverted dead-end artifact:
  - `build/captures/current_goal_highflight_farsvo_search_v1_frame350_20260602`
- After/fix artifact:
  - `build/captures/current_goal_farheight_feedback_repair_v1_frame350_20260602`
- After patch at frame 350:
  - `contractSamples=6`
  - `nonReady=0`
  - `contractNonReady=0`
  - states became `ReadyToRender` or `ResidentEmpty`
  - `surfaceRasterMax=2560`
  - `surfacePromoted=1`
  - `miss=0`
  - `unsafeNearMiss=0`

What this proves:

- One real render-contract bug was fixed: far-height/lower-LOD public pixels no longer suppress exact-contract repair in the validated high/free-camera frame.
- This is not material tuning, not a water change, not terrain generation, and not a fake fallback.

What remains broken:

- Early high/free-camera public frames still show small but real far-height/height-proxy peaks around `1.4-1.5%`.
- User-visible captures still show slow filling and shoreline/terrain gaps in some views.
- The next useful validation is a moving/waterline public capture after this patch, then same-frame owner/material/debug on any current visible wrong pixels.
- Do not mark the goal complete.

## Current State - 2026-06-01 Mid-Voxel DDA Default Replaces Low-Altitude Far-Height Leak

Goal is active and not complete.

User-facing truth:

- This slice fixes one real public rendering contract leak, not a cosmetic material issue.
- The runtime log advertised `Sparse terrain render contract: voxel-only terrain; procedural mid/far height fallback disabled`, but low-altitude/waterline public frames still showed a large `farHeight` / `heightProxy` owner share.
- The leak came from a separate sparse-miss continuity path: when resident mid closure failed, `DebugBackgroundMissHit` could return `BuildDeterministicFarTerrainContinuityHit` as `BACKGROUND_LAYER_FAR_HEIGHT` even in voxel-only mode.
- That means public pixels could be filled by deterministic heightfield continuity instead of the intended voxel LOD path. This matches the user complaint that terrain can look coherent in metrics while still rendering as fake sheets/gaps around water.

Evidence before patch:

- Baseline review reel:
  - `build/captures/current_goal_public_contract_review_reel_20260601`
- Waterline baseline:
  - `waterline/layer_screen_timeline.csv`
  - frame 200: `farHeightScreenPct=10.9598`, `heightProxyScreenPct=10.9598`
  - frame 240: `farHeightScreenPct=9.6994`, `heightProxyScreenPct=9.6994`
  - frame 280: `farHeightScreenPct=15.0611`, `heightProxyScreenPct=15.0611`
- Owner debug:
  - `build/captures/current_goal_waterline_owner55_farheight_probe_20260601`
  - raymarch background was dominated by yellow mid/far-height owner color.
- Control:
  - `VENPOD_SPARSE_MID_VOXEL_WALK_DDA=1`
  - `build/captures/current_goal_waterline_middda_control_20260601`
  - converted those pixels to `midVoxel` and reduced `farHeightScreenPct` to `0` with `miss=0` and `unsafeNearMiss=0`.

Code changed:

- `src/main_launcher.cpp`
  - Changed default `VENPOD_SPARSE_MID_VOXEL_WALK_DDA` from `0` to `1`.
  - The env override remains available: set `VENPOD_SPARSE_MID_VOXEL_WALK_DDA=0` to reproduce the old behavior.

Why this is aligned with the public contract:

- It does not change terrain generation.
- It does not change water.
- It does not make fake height fallback look nicer.
- It moves low-altitude fallback ownership from `farHeight` / height proxy to the resident mid-voxel DDA path, which is an actual voxel LOD owner.

Validation after patch:

- Build:
  - `.\build.ps1 -Config Release`
  - passed.
- Waterline default after patch:
  - `build/captures/current_goal_middda_default_waterline_20260601`
  - passed.
  - `farHeightScreenPct=0` for sampled frames.
  - `miss=0`, `unsafeNearMiss=0`.
- Moving public walk after patch:
  - `build/captures/current_goal_middda_default_walk_121_601_20260601`
  - passed.
  - frame 120: `midVoxelScreenPct=17.0471`, `farHeightScreenPct=0`, `miss=0`
  - frame 240: `midVoxelScreenPct=8.8837`, `farHeightScreenPct=0`, `miss=0`
  - frame 600: `midVoxelScreenPct=0.122`, `farHeightScreenPct=0`, `miss=0`
  - contact sheet looked stable; no obvious old grey/fake height regression.
- Default smoke after patch:
  - `build/captures/current_goal_middda_default_smoke_20260601`
  - passed.
  - `farHeightScreenPct=0` for sampled public frames.
- High/free-camera guard:
  - `build/captures/current_goal_middda_default_highflight_20260601`
  - passed.
  - Still has small intermittent high-altitude `farHeightScreenPct` peaks (`maxHeightProxy=1.64%`), but this is not the dominant low-altitude/waterline leak fixed here.

Remaining blockers:

- Do not mark the goal complete.
- This slice removes a proven low-altitude fake height-owner leak, but the broader public contract still needs more work.
- Remaining evidence-backed work:
  - investigate the small high/free-camera far-height fallback share and decide whether it is legitimate horizon continuity or another fake owner leak;
  - reproduce any current live/manual view that still shows terrain slowly filling in, using same-frame owner/material/debug captures;
  - if exact/mid/far/water still disagree on current pixels, patch that owner path directly.

## Current State - 2026-06-01 Exact Sand-Bank Readability v1

Goal is active and not complete.

User-facing truth:

- This slice targeted the remaining late-walk shoreline/bank failure class after ownership/debug evidence showed it was not a missing exact/mid/far/sky problem.
- The renderer was drawing legitimate exact sparse surface, but the view was dominated by raw sand side faces and high grid contrast, which made coherent terrain read as broken voxel slabs.
- This is a presentation/readability improvement for exact-owned terrain, not a fake fallback or ownership mask.

Evidence before patch:

- Late moving walk frames in `current_goal_bounded_public_exact_upgrade_v2_walk_smoke_20260601` still failed visual markers:
  - frame 400 `terrainLikePct=24.65`
  - frame 440 `terrainLikePct=22.15`
  - frame 480 `terrainLikePct=20.00`
  - frame 520 `terrainLikePct=11.01`
- Owner/readiness was clean:
  - `miss=0`
  - `unsafe=0`
  - `surfacePromoted=1`
  - mid/far screen share tiny after exact promotion.
- Owner debug:
  - `build/captures/current_goal_owner_debug_walk_55_20260601`
  - showed the bad frames were overwhelmingly exact sparse surface.
- Material debug:
  - `build/captures/current_goal_material_debug_walk_54_20260601`
  - showed mostly sand/dirt/stone, not missing/sky/water takeover.
- Face debug:
  - `build/captures/current_goal_face_debug_walk_64_20260601`
  - exact side-face percentages by sampled pixels:
    - frame 400: `66.38%`
    - frame 440: `43.53%`
    - frame 480: `46.61%`
    - frame 520: `70.68%`
  - This proved the failure was exact bank/side presentation, not streaming non-coverage.

Code changed in `assets/shaders/Graphics/PS_SparseSurface.hlsl`:

- Added sand side-face bank shading:
  - compact/damp/dry bank tones,
  - vertical strata/contour variation,
  - stronger blend at distance so large side banks stop reading as uniform yellow sheets.
- Reduced exact side-face grid contrast, especially for sand:
  - sand side grid scale added;
  - side distance grid fade tightened;
  - side-face dry grid strength reduced.
- No changes to:
  - ownership,
  - exact/mid/far handoff,
  - terrain generation,
  - water ownership,
  - fake fallback/proxy behavior,
  - streaming budgets.

Validation:

- Build/assets:
  - `.\build.ps1 -Config Release`
  - build had no C++ work; runtime assets refreshed.
- Moving walk after:
  - `build/captures/current_goal_exact_sand_bank_readability_v1_walk_smoke_20260601`
  - passed; terrain-critical `readyFrame=320 postNonReady=0`.
  - formerly failing visual marker now passes:
    - frame 400 `terrainLikePct=67.82`
    - frame 440 `terrainLikePct=49.38`
    - frame 480 `terrainLikePct=49.56`
    - frame 520 `terrainLikePct=73.30`
  - owner stats remained clean:
    - `miss=0`
    - `unsafe=0`
    - `surfacePromoted=1`
    - no far-water/proxy regression observed in timeline.
- Default smoke after:
  - `build/captures/current_goal_exact_sand_bank_readability_v1_default_smoke_20260601`
  - passed; terrain-critical `readyFrame=120 postNonReady=0`.
- Waterline smoke after:
  - `build/captures/current_goal_exact_sand_bank_readability_v1_waterline_smoke_20260601`
  - passed; terrain-critical `readyFrame=180 postNonReady=0`.
- Skyline/free-view after:
  - `build/captures/current_goal_exact_sand_bank_readability_v1_skyline_probe_20260601`
  - passed; terrain-critical `readyFrame=240 postNonReady=0`.
  - contact sheet shows coherent lower-LOD public coverage over water/terrain.
  - high-altitude exact proof is intentionally not promoted:
    - `surfaceRasterMax=1024`
    - `surfacePromoted=0`
    - shader exact contract non-ready remains high in sampled frames.
  - request pressure remains bounded:
    - shader feedback `unsafe=0 requested=0` in sampled high-alt frames.
    - hidden exact high-alt throttle accepts at most `16` per sampled frame.

Remaining blockers:

- Do not mark goal complete.
- This fixes the validated exact sand-bank readability class, but it does not prove all user-visible live/free-camera gaps are gone.
- Next best work:
  - run a fresh user-facing/free-camera or route-level capture after both the exact-upgrade throttling and sand-bank readability patches;
  - if new failures are owner-contract issues, patch owner/handoff;
  - if new failures are valid exact geometry looking bad, classify by face/material first before changing renderer logic.

## Current State - 2026-06-01 Public Exact-Upgrade Flood Bounded

Goal is active and not complete.

User-facing truth:

- This slice fixes another real public-rendering contract problem: low-altitude public frames were still flooding exact sparse repair/publish work even when there was no unsafe near-miss and lower LOD/water/sky already owned the public frame.
- This does not finish the renderer. Late walk frames still fail the old visual terrain-like marker because they are sandy/water-heavy exact-surface composition frames, not because of `MISS`/unsafe ownership.
- The remaining visible problems are now split more cleanly:
  - early/public-frame exact-upgrade churn is reduced;
  - shoreline/sand/water presentation still needs a separate owner/material/water diagnosis in the user-visible bad view.

Root cause fixed:

- In `current_goal_first_public_walk_normal_20260601`, public render opened around frame 100 with coherent lower LOD, but exact proof/repair flooded work:
  - hidden exact frame 102 before earlier run: up to `feedback=256 accepted=256`, `outstanding=143`, `timeExpired=1`.
  - frame 120 before this slice: `hiddenExactMissing=1469`, `shaderUnsafeNonReady=0/0`, `miss=0`.
  - frame 124 before: shader feedback had `unsafe=0` but `requested=241` exact bricks.
  - terrain-critical post check failed at frame 179 on an isolated `UploadingGPU` brick.
- The immediate bug was that the shader "foreground repair" cap only limited high-priority tracking. Safe broad exact repair still requested all non-ready exact-contract samples when `unsafe=0`.
- Hidden-exact post-open warmup also used a broad public-frame budget (`512` rays / `8.0ms` / `256` requests), which is too aggressive for exact-upgrade work after the world is already public.

Code changed in `src/main_launcher.cpp`:

- Changed default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_RAY_BUDGET` from `512` to `128`.
- Changed default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PROBE_MAX_MS_TENTHS` from `80` to `30`.
- Changed default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_WARMUP_MAX_REQUESTS` from at least `256` to `min(hiddenExactMaxRequests, 32)`.
- Changed default `VENPOD_SPARSE_SHADER_UNSAFE_FOREGROUND_REPAIR_MAX_REQUESTS` from `256` to `48`.
- Added `VENPOD_SPARSE_SHADER_UNSAFE_BROAD_EXACT_REPAIR`, default `0`.
  - With the default, actual unsafe near-miss still gets urgent repair.
  - Safe exact-upgrade feedback requests only the bounded foreground-selected set.
  - The old broad safe exact repair can be re-enabled explicitly for diagnostics.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Early public walk before:
  - `build/captures/current_goal_first_public_walk_normal_20260601`
  - failed terrain-critical post check at frame 179: `22,25,165:UploadingGPU`.
  - frame 120: `hiddenExactMissing=1469`, surface not promoted, shader unsafe actual `0`.
  - frame 124: shader feedback `unsafe=0 requested=241`.
- Early public walk after:
  - `build/captures/current_goal_bounded_public_exact_upgrade_v2_first_public_walk_20260601`
  - passed terrain-critical readiness: `samples=80 readyFrame=100 postNonReady=0`.
  - frame 100 hidden exact: `visited=128`, `timeLimitMs=3.0`, `maxRequests=32`, `feedback=2`.
  - frame 120: `surfacePromoted=1`, `hiddenExactMissing=4`, `miss=0`, `unsafe=0`.
  - frame 124: shader feedback `unsafe=0 requested=48`, with `cap=193` skipped.
- Default smoke after:
  - `build/captures/current_goal_bounded_public_exact_upgrade_v2_default_smoke_20260601`
  - passed; terrain-critical `readyFrame=120 postNonReady=0`.
- Waterline smoke after:
  - `build/captures/current_goal_bounded_public_exact_upgrade_v2_waterline_smoke_20260601`
  - passed; terrain-critical `readyFrame=180 postNonReady=0`.
- Moving walk after:
  - `build/captures/current_goal_bounded_public_exact_upgrade_v2_walk_smoke_20260601`
  - still fails visual marker on frames 400/440/480/520.
  - owner/readiness is clean in those frames: `miss=0`, `unsafe=0`, `surfacePromoted=1`.
  - frame 440 hidden exact now `feedback=12 accepted=12` instead of broader repair bursts.

Remaining blockers:

- Do not mark goal complete.
- Public exact-upgrade flood is bounded, but the renderer still has visible shoreline/sand/water quality issues in the late walk/user-visible views.
- Owner debug for late walk frames (`current_goal_owner_debug_walk_55_20260601`) showed those frames are overwhelmingly exact sparse surface, not mid/far holes.
- Material debug for the same frames (`current_goal_material_debug_walk_54_20260601`) showed mostly sand/dirt/stone exact terrain. That means the next fix should target exact-surface/water/shoreline presentation or the camera/world composition for those views, not hidden exact streaming flood.
- Next best work:
  - reproduce the user's current bad manual shoreline/water view or use late walk frames 400-520;
  - capture normal + owner + material + water debug same-camera;
  - if water/sky owns where CPU terrain should be solid, patch ownership;
  - if exact surface owns and material is sand/bank, patch exact surface/waterline presentation rather than streaming.

## Current State - 2026-06-01 High-Altitude Exact-Proof Flood Throttled

Goal is active and not complete.

User-facing truth:

- This slice improves one real runtime blocker: high/free-camera public views were still flooding the exact sparse pipeline even when mid/far LOD already covered the frame safely.
- The engine is still not finished. This does not fix all shoreline/sand visual composition issues or every live rendering artifact.
- It does reduce the "terrain slowly builds itself in while the frame is already public" failure mode for high-altitude/general views.

Root cause found:

- In `current_goal_bounded_repair_128_v5_skyline_probe_20260601`, the public high-view frames had safe lower-LOD ownership:
  - `missPct=0`
  - `unsafeNearMissPct=0`
  - Far SVO and mid coverage complete
  - contact sheet visually coherent enough for lower LOD
- But exact proof/repair still flooded the sparse pipeline:
  - `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK` had `unsafe=0` but `requested=253-256` per sampled frame.
  - `PERF_SPARSE_HIDDEN_EXACT_MISS` accepted up to `162` exact requests in high-altitude LOD-throttled frames.
  - Hidden exact tracked/outstanding work climbed into the thousands.
  - frame 440 before: `uploading=10865`, `publishPending=11788`.
- That is the architecture mismatch the user kept seeing: exact-upgrade proof work was being treated like public-critical streaming even though lower LOD was already the legitimate visible owner.

Code changed in `src/main_launcher.cpp`:

- Changed default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_HIGH_ALTITUDE_MAX_REQUESTS` to `16` instead of at least `256`.
- Under high-altitude LOD-throttled safe coverage, hidden-exact feedback now uses idle probe budget/time instead of the broad runtime probe:
  - ray budget drops to idle budget (`64` in validation)
  - time limit drops to idle limit (`1.5 ms` in validation)
- Shader unsafe feedback no longer submits broad exact-upgrade requests when:
  - `unsafe=0`
  - high-altitude LOD coverage is already allowed
  - the sample is only an exact contract non-ready candidate.
- Actual unsafe near-miss feedback still remains render-critical.
- The feedback loop now skips already-ready samples instead of calling the request path for them.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Before artifact:
  - `build/captures/current_goal_bounded_repair_128_v5_skyline_probe_20260601`
  - failed terrain-critical post check on isolated `UploadingGPU` exact bricks after public ready.
  - frame 440: `uploading=10865`, `publishPending=11788`, shader feedback `requested=255` with `unsafe=0`.
- After artifact:
  - `build/captures/current_goal_highalt_broad_exact_throttle_v2_skyline_probe_20260601`
  - passed terrain-critical readiness: `samples=164 readyFrame=240 postNonReady=0`.
  - shader feedback at frames 240/280/320/360/400/440: `unsafe=0`, `requested=0`.
  - hidden exact accepted at sampled frames: `0, 0, 16, 16, 9, 0`.
  - hidden exact probe time: about `1.5 ms`, not broad `8-25 ms`.
  - frame 440 after: `uploading=2022`, `publishPending=2029`.
  - owner pressure remained clean: `missPct=0`, `unsafeNearMissPct=0`.
- Control captures:
  - default smoke passed: `build/captures/current_goal_highalt_broad_exact_throttle_v2_default_smoke_20260601`
  - waterline smoke passed: `build/captures/current_goal_highalt_broad_exact_throttle_v2_waterline_smoke_20260601`
  - moving walk still fails the old visual `terrainLikePct` marker on sand/water frames:
    - `build/captures/current_goal_highalt_broad_exact_throttle_v2_walk_smoke_20260601`
    - owner contract is clean there: `missPct=0`, `unsafeNearMissPct=0`, `surfacePromoted=1`, mid/far tiny after frame 380.

Remaining blockers:

- Do not mark the goal complete.
- High/free-camera exact proof flooding is improved, but public rendering is still not a finished Minecraft-like presentation system.
- Ground/walk shoreline frames still look visually weak because of sand/bank/water composition and exact surface readability, not because of the high-alt exact-flood path.
- Startup is still held through early frames by terrain-critical startup proof; this slice did not redesign startup.
- Next best work:
  - reproduce a current user-visible ground/shoreline bad frame with same-frame owner/material/water debug;
  - if owner stats are clean, target shoreline/exact-surface/water presentation directly;
  - if owner stats show water/sky/miss winning over deterministic terrain, patch that owner path.

## Current State - 2026-06-01 Public Rendering Contract Still Not Solved

Goal is active and not complete.

User-facing truth:

- The engine is still visibly broken in broader public/gameplay views: terrain can appear slowly, exact/mid/far layers can mix incoherently, and shoreline/water-adjacent views still show gaps or wrong-looking ownership.
- Some targeted fixes improved specific symptoms, but they did not complete the actual runtime capability. Do not claim success from smoke passes alone.
- The real objective remains coherent public rendering: once a frame is shown, every visible pixel needs a legitimate owner. Exact sparse terrain can stream/promote later, but lower LOD must cover honestly until exact is render-ready.

What has actually improved:

- Public startup no longer waits for wide exact proof by default; first public frames can open on coherent lower LOD instead of waiting for hidden exact proof.
- Small hidden-exact repair bursts no longer collapse the entire exact surface band in moving views.
- Some hidden exact misses are repaired through foreground feedback.
- A sparse underwater-tint bug near shorelines was fixed.
- Several old diagnoses were invalidated by better same-frame camera/audit alignment.

What is still blocking the engine:

- The current architecture still leaks terrain build/proof/repair state into public rendering.
- Exact sparse surface generation/upload/extraction/GPU publish is not atomic enough from the renderer's point of view.
- The renderer can show partial exact chunks mixed with mid/far fallback before exact terrain is fully render-ready.
- Mid/far can legally cover pixels, but in many views they are visually too coarse or are exposed for too long because exact promotion lags.
- Some gates were checking "is wide exact proof clean?" when the actual public contract should be "is the visible frame coherently covered by some valid LOD?"

Most recent active mechanism to inspect:

- Skyline/high-view probe after the bounded repair patch failed terrain-critical readiness on isolated `UploadingGPU` bricks while `lodThrottle=1`.
- Artifacts:
  - `build/captures/current_goal_bounded_repair_128_v5_skyline_probe_20260601`
- The important question is whether those `UploadingGPU` bricks cause visible broken pixels, or whether the gate is treating exact-upgrade work as public-critical while lower LOD already covers the frame.
- Do not weaken gates blindly. First prove whether the failure is a real owner-contract break or a false positive under LOD-throttled public coverage.

Next correct work:

1. Inspect the skyline/high-view probe images and owner timelines.
2. Classify visible failures as owner-contract bugs, exact publish latency, LOD transition leaks, or visual LOD quality.
3. If deterministic terrain is visible as sky/miss/wrong water, patch the owner path.
4. If lower LOD coherently owns the pixel but exact proof is still pending, patch the public presentation contract so exact repair does not stall or destabilize the visible frame.
5. Validate with same-frame normal/owner/material captures, not only smoke gates.

## Current State - 2026-06-01 Moving LOD Demotion Pop Reduced

Goal is active and not complete.

User-facing truth:

- This slice reduced one real moving-view LOD pop: a small hidden-exact repair burst no longer globally collapses the exact surface raster while shader/public ownership says the view is safe.
- The engine is still not done. Moving walk smoke still fails its visual marker on sandy/shoreline-heavy frames, but the old owner-contract failure is not present in that capture.

Root cause fixed:

- In `current_goal_startup_shader_gate_off_v4_walk_smoke_20260601`, frame 440 had:
  - shader feedback clean: `shaderUnsafeContractNonReady=0`, `unsafe=0`, all sampled contract bricks ready;
  - hidden-exact repair burst: `hiddenExactMissing=59/0`, `hiddenExactAccepted=28`;
  - global exact raster demoted anyway: `surfaceRasterMax=1024`, `surfacePromoted=0`;
  - mid/far still carried the frame (`miss=0`, `unsafeNearMiss=0`), but the whole exact band visibly popped down because the hidden-exact demotion tolerance was only 16.
- That was too strict for moving cameras. Hidden-exact CPU repair is not the same as the public owner contract when shader feedback says current exact-contract samples are ready and lower LOD is covering safely.

Code changed:

- `src/main_launcher.cpp`
  - Increased default `VENPOD_SPARSE_SURFACE_DEMOTION_MAX_HIDDEN_EXACT_MISSING` from `16` to `128`.
  - This only affects the already bounded keep-wide path. Wide exact still demotes for actual shader unsafe/contract non-ready feedback or broad hidden-exact backlogs above the bound.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Moving walk before:
  - `build/captures/current_goal_startup_shader_gate_off_v4_walk_smoke_20260601`
  - frame 440: `surfaceRasterMax=1024`, `surfacePromoted=0`, `hiddenExactMissing=59/0`, `midVoxelScreenPct=1.5207`, `missPct=0`, `unsafeNearMissPct=0`.
- Moving walk after:
  - `build/captures/current_goal_bounded_repair_128_v5_walk_smoke_20260601`
  - frame 440: `surfaceRasterMax=2560`, `surfacePromoted=1`, `hiddenExactMissing=6/0`, `midVoxelScreenPct=0.2558`, `missPct=0`, `unsafeNearMissPct=0`.
  - frame 360 also stayed promoted instead of dropping to `1024`.
  - walk smoke still fails the same visual marker (`terrainLikePct`) at frames 440/480/520; this is not fixed by owner/raster promotion because those frames are dominated by sandy banks/shoreline composition rather than missing terrain.
- Default smoke after:
  - `build/captures/current_goal_bounded_repair_128_v5_default_smoke_20260601`
  - passed; terrain-critical `readyFrame=120`, `postNonReady=0`.
- Waterline smoke after:
  - `build/captures/current_goal_bounded_repair_128_v5_waterline_smoke_20260601`
  - passed; terrain-critical `readyFrame=180`, `postNonReady=0`.

Remaining blockers:

- Do not mark goal complete.
- Public owner contract is cleaner: static open is earlier, moving small repair bursts do not collapse the exact band, and tested frames have `miss=0` / `unsafeNearMiss=0`.
- Still unresolved:
  - broader live/free-camera views can still expose slow exact promotion or visually poor LOD/material/geometry;
  - moving walk visual marker still fails due sandy/shoreline route composition;
  - exact and lower LOD transitions still need broader validation in the user's reported problem views, not just this scripted walk.
- Next best work:
  - reproduce a current user-visible broken live/free-camera view with owner debug and layer percentages after these fixes;
  - if ownership is clean, stop treating it as streaming holes and target LOD representation/terrain composition;
  - if ownership shows sky/water/miss winning over deterministic terrain, patch that owner path directly.

## Current State - 2026-06-01 Public Open No Longer Waits For Wide Exact Proof

Goal is active and not complete.

User-facing truth:

- This slice fixed one real architecture/presentation mistake: startup was still waiting for wide exact-surface proof even though mid/far were already capable of carrying the public frame coherently.
- The renderer now opens earlier with a coherent lower LOD, then promotes exact surface when the current view is clean.
- This does not finish the engine. Remaining problems are LOD pop/promotion under motion and visual quality/composition in some shoreline/bank views.

Root cause found:

- `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` defaulted on.
- By frame 60-80, static rebrun had Far SVO ready, mid voxel coverage at `1.00/1.00`, terrain-critical requests mostly drained, and `miss=0` later, but public render stayed hidden.
- The only blocker after frame 100 was `shaderUnsafeBlocked=1` with `shaderUnsafeClean=1/2`.
- Shader-unsafe feedback is wide-exact promotion proof. It repeatedly found newly rendered wide-exact private frames with repair work, so startup waited until around frame 240 even though coherent lower LOD was already available.
- That was the wrong public contract: exact proof should control exact promotion, not block the first public coherent frame.

Code changed:

- `src/main_launcher.cpp`
  - Changed default `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` from `1` to `0`.
  - Left the gate available as an opt-in strict diagnostic/startup mode.
  - Existing shader-unsafe feedback still drives repair and exact raster promotion; it just no longer blocks public open by default.

Validation artifacts:

- Build passed:
  - `.\build.ps1 -Config Release`
- Before this patch:
  - `build/captures/current_goal_shader_clean_stale_v3_rebrun_20260601`
  - first public capture: `engine_frame_0241.bmp`
  - held through frame 220 with all main readiness clean except `shaderUnsafeBlocked=1`, `shaderUnsafeClean=1/2`.
- After this patch, focused rebrun:
  - `build/captures/current_goal_startup_shader_gate_off_v4_rebrun_20260601`
  - first public capture: `engine_frame_0101.bmp`
  - frame 101: `surfaceRasterMax=1024`, `surfacePromoted=0`, `hiddenExactMissing=83/0`, `hiddenExactAccepted=83`, `farCov=1.00/1.00`, `midCov=1.00/1.00`.
  - visual: coherent lower LOD, not the old all-water/missing-terrain collapse.
- Longer static rebrun:
  - `build/captures/current_goal_startup_shader_gate_off_v4_long_rebrun_20260601`
  - frame 101: lower LOD coherent.
  - frame 300: exact promoted, `surfaceRasterMax=2560`, `surfacePromoted=1`, `hiddenExactMissing=0/0`.
  - frame 701: exact remains promoted, `hiddenExactMissing=0/0`.
  - ownership pressure selected frames: `missPct=0`, `unsafeNearMissPct=0`.
- Default smoke:
  - `build/captures/current_goal_startup_shader_gate_off_v4_default_smoke_20260601`
  - passed; terrain-critical `readyFrame=120`, `postNonReady=0`.
- Moving walk smoke:
  - `build/captures/current_goal_startup_shader_gate_off_v4_walk_smoke_20260601`
  - script still fails visual marker on low `terrainLikePct` at frames 440/480/520.
  - Owner/readiness evidence is clean in those frames:
    - `missPct=0`, `unsafeNearMissPct=0`;
    - frame 440 had temporary hidden-exact repair `hiddenExactMissing=59/0`, then frame 480 was promoted again with `hiddenExactMissing=0/0`;
    - mid/far coverage stayed complete.
  - The failure is not the old miss/hidden-terrain owner-contract bug; it is now shoreline/bank/LOD presentation and route composition/visual quality.

Remaining blockers:

- Do not mark goal complete.
- Static startup/public-open behavior is materially better, but exact promotion still visibly changes distant terrain between frame 101 and frame 300.
- Moving views can still drop from promoted exact to lower LOD temporarily as the camera enters new terrain, then repair/promotion catches up.
- Some moving walk frames are visually poor because the route looks across sandy banks/shorelines, not because of `MISS` or unsafe terrain ownership.
- Next real rendering work should target coherent LOD transition under motion:
  - either prefetch/promote exact surface ahead of camera motion sooner;
  - or make lower LOD and exact promotion transition atomically/visually stable so terrain does not visibly rebuild;
  - validate on moving public captures, not only static smoke.

## Current State - 2026-06-01 Foreground Repair Fast-Lane + Bounded Keep-Wide Fix

Goal is active and not complete.

User-facing truth:

- Rendering is not finished, but this turn made a real public rendering-contract improvement.
- The prior visible class was partial exact/mid/far state leaking while foreground exact repair lagged.
- The current validated captures are much more coherent in the default rebrun and fixed walking route, but the engine still has heavy startup/private-frame work and still needs broader live-view validation.

Root causes addressed:

1. Foreground exact repair was only being requested, not enrolled in the existing hidden-exact render-ready fast lane.
   - Before: frame 480 in `current_goal_fg_repair_reserve_v2_walk_smoke_20260601` had `shaderUnsafeContractNonReady=66`; those selected foreground repair bricks competed with generic queues.
   - Fix: shader-selected foreground repair coords now enter `sparseHiddenExactMissTrackedCoords` and the critical coord set after request admission, so they get the existing priority generation/upload/surface/publish treatment.
   - Telemetry now reports `fgRepair=active/eligible/requested/tracked/cap`.

2. A small post-promotion hidden-exact repair burst could globally demote the exact surface band.
   - Before: in `current_goal_fg_repair_tracked_v3_rebrun_20260601`, frame 411 found 7 hidden-exact repair bricks; by frame 420 all were ready, but public raster still collapsed from `surfaceRasterMax=2560` to `1024` because promotion had been reset.
   - Fix: initial wide exact promotion remains strict, but once promoted, a bounded shader-clean hidden-exact repair burst can keep the wide exact band. Default demotion hidden-exact tolerance is now 16 and bounded keep-wide is default-on.

Files changed in this slice:

- `src/main_launcher.cpp`
  - foreground shader repair coords are tracked through the hidden-exact render-ready fast lane;
  - `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK` and `PERF_CAMERA_EXPOSURE` include foreground repair tracked count;
  - bounded hidden-exact keep-wide default changed so tiny repair bursts do not collapse the whole public exact LOD.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Default rebrun after v4:
  - `build/captures/current_goal_bounded_repair_keepwide_v4_rebrun_20260601`
  - first public capture remains frame 161, not frame 1;
  - frame 161: `surfaceRasterMax=2560`, `surfacePromoted=1`, `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`, mid/far complete;
  - frame 420 now stays wide/promoted: `surfaceRasterMax=2560`, `surfacePromoted=1`, no unsafe/non-ready exact contract.
- Moving fixed-dt walk after v4:
  - `build/captures/current_goal_bounded_repair_keepwide_v4_walk_smoke_20260601`
  - script still flags terrainLikePct on sand/water-heavy frames, but owner/readiness is clean:
    - frame 400: `shaderUnsafeContractNonReady=0`, `surfacePromoted=1`, midVoxel `0.0076%`, miss `0`;
    - frame 440: `shaderUnsafeContractNonReady=0`, `surfacePromoted=1`, midVoxel `0.6435%`, miss `0`;
    - frame 480: `shaderUnsafeContractNonReady=0`, `surfacePromoted=1`, midVoxel `0.147%`, miss `0`.
  - Contact sheet looks coherent shoreline/sand/water, not the old partial-streaming collapse.
- Default smoke:
  - `build/captures/current_goal_bounded_repair_keepwide_v4_default_smoke_20260601`
  - passed; `readyFrame=120`, `postNonReady=0`, `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`.

Remaining blockers:

- Do not mark the goal complete.
- Startup/private render is still heavy: early frames perform large hidden-exact generation/upload/surface work before the first public frame.
- First public default rebrun capture is frame 161, not immediate.
- This slice improves coherent presentation, but broader arbitrary live views still need validation against the user's current screenshots.
- The next real blocker is either:
  - reducing startup/private-frame exact proof/build work without exposing broken public frames; or
  - reproducing any remaining user-visible holes/gaps in a same-frame owner/readiness capture and patching that owner path.

## Current State - 2026-06-01 Public Rendering Still Broken, Active Blocker Is Coherent LOD Presentation

Goal is active and not complete.

User-facing truth:

- The engine is still not a proper Minecraft-like playable renderer.
- It is somewhat faster and some close views are more coherent, but live/public views still show slow terrain convergence, holes/gaps, water/shoreline inconsistencies, and mixed exact/mid/far terrain.
- Do not claim success from smoke gates, isolated capture passes, or telemetry that does not match the visible frame.

What is actually stopping us now:

- The architecture is not fundamentally impossible, but the current production/presentation boundary is wrong.
- VENPOD is still doing too much terrain proof/generation/extraction/publish while public frames are visible.
- The renderer can expose partially ready exact sparse terrain, mid voxel fallback, far SVO, and water in the same view instead of presenting one coherent LOD state.
- Minecraft-like engines avoid this class by building chunk/section render data off the public path and publishing coherent sections atomically; VENPOD still has repair/proof/streaming state leaking into the image.

Latest concrete fixes in `src/main_launcher.cpp`:

- Tightened wide exact-surface promotion so public exact raster does not promote while current-view exact-contract bricks are non-ready.
  - Baseline rebrun opened around frame 81 with `surfacePromoted=1`, `surfaceRasterMax=2560`, `shaderUnsafeContractNonReady=58`, and `hiddenExactMissing=58`.
  - After the patch, the first public rebrun capture moved to around frame 161 and only promoted wide exact once current-view exact feedback was clean.
- Foreground exact repair requests now use the terrain-critical reserve path.
  - In the moving walk frame 480, this improved `shaderUnsafeContractNonReady` from `82` to `66` and reduced generation backlog from about `1455` to `616`.
  - It did not fully solve moving-view catchup.

Current validation state:

- Default and waterline smoke captures passed after the public exact-promotion tightening:
  - `build/captures/current_goal_clean_exact_contract_v1_default_smoke_20260601`
  - `build/captures/current_goal_clean_exact_contract_v1_waterline_smoke_20260601`
- Moving walk still fails visual marker frames around 440/480:
  - `build/captures/current_goal_fg_repair_reserve_v2_walk_smoke_20260601`
  - `miss=0`, `unsafeNearMiss=0`, no proxy regression, but foreground exact catchup remains behind under movement.

Current blocker:

- Foreground exact terrain can still be requested/selected too late or drain too slowly through generation/upload/surface extraction/GPU publish/draw-slot readiness.
- Lower LOD often covers the pixel coherently enough to avoid `MISS`, but the result is still visually wrong or slow because exact terrain is not promoted fast/coherently enough.
- The next work must directly improve this public rendering contract, not material color, skyline closure, route composition, or broad reporting.

Next correct work:

1. Measure the remaining moving/live bad view at the exact failing frames with owner/material/readiness evidence.
2. Fix the foreground exact render-ready pipeline so selected current-view exact repair bricks get generated, uploaded, surface-extracted, GPU-published, and drawable in bounded time.
3. Keep lower LOD as a legitimate coherent presentation layer until exact is render-ready, instead of mixing partial exact and partial fallback state.
4. Validate on user-visible captures, not only smoke gates.

## Current State - 2026-06-01 Hidden Background Terrain Miss Fixed For Validated Views

Goal is active and not complete.

User-facing truth:

- Rendering is still not a finished Minecraft-like engine, but one concrete public owner-contract failure was fixed in this turn.
- Do not claim the whole rendering goal is complete from this slice.
- The validated improvement is specific: deterministic far/background terrain that was diagnosed as present no longer falls through to `MISS` in the high-flight/general-view capture.

Root cause fixed:

- `DebugBackgroundMissHit` could classify a pixel as `backgroundHiddenTerrainMiss`: deterministic far terrain existed along the ray, but resident mid/far/water owners all failed.
- The shader then returned `RENDER_OWNER_MISS`, producing visible holes even though the procedural world contained terrain.
- A previous attempt to broadly re-enable the existing far-height path under `voxelTerrainOnly` did not help (`miss` stayed around `6771-6838`) and was reverted.
- The working fix is narrower:
  - only in `backgroundHiddenTerrainMiss`;
  - only after resident mid-voxel closure fails;
  - build a deterministic far-terrain continuity hit from the already-computed diagnostic terrain distance;
  - allow the continuity hit out to the far world coverage scale (`13312`) instead of rejecting it at the older `10400` diagnostic loop cap.

File changed in this slice:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - added `BuildDeterministicFarTerrainContinuityHit(...)`;
  - calls it from `DebugBackgroundMissHit(...)` for `backgroundHiddenTerrainMiss`;
  - leaves exact/mid/far/water ownership policies otherwise unchanged;
  - no material tuning, water fallback, terrain generation, exactNear, or skyline closure changes.

Validation:

- Shader/runtime parity was explicitly refreshed with:
  - `.\raymarch_shader_iter.ps1 -RefreshOnly`
- Important stale-asset trap:
  - `engine_capture_smoke.ps1 -NoBuild` does not refresh shader assets.
  - Always refresh with `raymarch_shader_iter.ps1 -RefreshOnly` after editing shader sources.
- Failed/invalidated experiments:
  - `current_goal_voxel_farheight_gate_v1_highflight_20260601`: broad voxel-only far-height gate made miss worse (`miss=6790`).
  - `current_goal_voxel_farheight_angle_v2_fresh_highflight_20260601`: widened height angle did not help (`miss=6838`).
  - `current_goal_hidden_miss_continuity_v2_highflight_20260601`: targeted continuity helped but was incomplete (`miss=2521`).
  - `current_goal_hidden_miss_continuity_trustdiag_v3_highflight_20260601`: removing the local height recheck did not change miss (`miss=2521`).
- Passing artifact:
  - `build/captures/current_goal_hidden_miss_continuity_distcap_v4_highflight_20260601`
  - result: smoke passed, `miss=0`, `unsafeNearMiss=0`, terrain-critical `readyFrame=240`, `postNonReady=0`.
  - key frames:
    - shaderFrame 330: `farHeight=4816`, `miss=0`
    - shaderFrame 340: `farHeight=11308`, `miss=0`
    - shaderFrame 350: `farHeight=33525`, `miss=0`
    - shaderFrame 360: `farHeight=161`, `miss=0`
    - shaderFrame 370: `farHeight=11732`, `miss=0`
    - shaderFrame 380: `farHeight=6315`, `miss=0`
- Default public validation:
  - `build/captures/current_goal_hidden_miss_continuity_v4_default_public_20260601`
  - passed, `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`.
- Waterline validation:
  - `build/captures/current_goal_hidden_miss_continuity_v4_waterline_20260601`
  - passed, `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`.
- Moving walk validation:
  - `build/captures/current_goal_hidden_miss_continuity_v4_walk_public_20260601`
  - script failed `terrainLikePct` on late shoreline frames, but ownership stayed clean:
    - late frames had `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`;
    - visual contact sheet looked like low sandy shoreline/water composition rather than the old miss/hidden-terrain hole class.
  - Do not treat this as proof the visual baseline is complete; it means this particular owner-contract miss class is not the cause of that walk marker failure.

Remaining blockers:

- The full goal is not done.
- Shader iteration is still painfully slow:
  - `PS_Raymarch` cache miss plus driver PSO creation can take roughly 9 minutes on this machine.
  - This is itself a practical rendering-engine blocker for further iteration.
- Public rendering still needs validation in arbitrary live `rebrun` views after this patch.
- Startup/streaming can still feel slow because exact surface/mid voxel production and publish work happen while public frames are visible.
- The next real rendering task should target either:
  - live/default view classes that still show visible holes after this patch, using exact-frame owner/material captures; or
  - the production/presentation boundary causing slow visible terrain promotion.

Next correct work:

1. Do not revisit the broad `voxelTerrainOnly` far-height gate; it was tested and made miss worse.
2. Keep the targeted `backgroundHiddenTerrainMiss` continuity path unless a later same-frame audit proves it creates false terrain.
3. Reproduce any remaining user-visible broken live view with shader parity confirmed first.
4. If ownership is clean but the image still looks wrong, diagnose composition/LOD/presentation separately; do not call it a miss-streaming bug.

## Current State - 2026-06-01 Latest Active Blocker: Background Hidden Terrain Misses

Goal is active and not complete.

User-facing truth:

- Rendering is still not acceptable. The engine can show coherent close terrain in some routes, but public/general views still expose holes, slow convergence, water/shoreline weirdness, and mixed exact/mid/far ownership.
- Do not claim success from smoke gates, isolated captures, or improved frame timing. The visible game frame is the source of truth.

Latest mechanism-level delta:

- A promotion bug was patched in `src/main_launcher.cpp`.
- Before the patch, exact sparse surface raster could stay promoted at wide range even when hundreds/thousands of exact candidates were still non-ready:
  - high-flight frame 280 before patch: `surfaceRasterMax=2560`, `surfacePromoted=1`, `shaderUnsafeContractNonReady=256`, `hiddenExactMissing=2938/96`.
- Patch added bounds to the "repair-only non-ready keeps wide raster" path:
  - wide exact promotion is kept only when non-ready and hidden-exact counts stay under the existing demotion limits.
- After the patch, the same high-flight case demotes instead of pretending exact is ready:
  - frame 280 after patch: `surfaceRasterMax=1024`, `surfacePromoted=0`, `shaderUnsafeContractNonReady=256`, `hiddenExactMissing=2741/96`.

What this proved:

- One real presentation bug was fixed: partial exact sparse state was being presented too aggressively.
- This did not fix the renderer. It exposed the next blocker more cleanly.

Current active blocker:

- High-flight/general-view captures still fail with visible miss holes.
- Debug mode 58 on the failing high-flight frame shows a large red band along the horizon/silhouette.
- In `DebugBackgroundMissHit`, that red is `backgroundHiddenTerrainMiss` for this case, not unsafe exact-near miss.
- Meaning: deterministic background terrain should exist, but the active mid/far/water ownership chain does not produce a valid owner for those pixels.
- This is now the next root-cause path. It is not material tuning, route composition, water aesthetics, or exact-near startup proof.

Most suspicious code path:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
- Investigate `RaymarchBackgroundField`, `RaymarchSparseFarField`, far-SVO start distance, far page-step budget, horizon/upward ray rejection, and the `voxelTerrainOnly` early return that prevents procedural far-height fallback.
- Do not add fake proxy terrain to hide the problem. First prove whether far SVO traversal starts too late, steps too little, rejects horizon rays, or lacks a legitimate deterministic continuity fallback.

Latest relevant artifacts:

- Before repair-bound patch:
  - `build/captures/current_goal_highflight_probe_20260601`
- After repair-bound patch:
  - `build/captures/current_goal_highflight_repair_bounds_v1_20260601`
- Debug owner/frame captures:
  - `build/captures/current_goal_highflight_frame340_owner55_v1_20260601`
  - `build/captures/current_goal_highflight_frame340_midpath68_v1_20260601`
  - `build/captures/current_goal_highflight_frame350_owner58_v1_20260601`

Next correct work:

1. Continue the root-cause path in `PS_Raymarch.hlsl`, focused on `backgroundHiddenTerrainMiss`.
2. Determine whether missing background terrain is due to far-SVO traversal budget/start/rejection or a contract gap when `voxelTerrainOnly` disables far deterministic fallback.
3. Patch only the proven mechanism.
4. Validate with the same high-flight capture and then normal/walk public captures.
5. Do not drift into materials, terrain shape, route composition, startup-only metrics, or broad report generation.

## Current State - 2026-06-01 Coherent Public LOD Still Not Solved

Goal is active and not complete.

User-facing truth:

- The renderer is better than the earliest broken captures, but it is still not a finished gameplay renderer.
- It can still feel laggy because terrain production/repair work runs during public frames.
- It can still look wrong because exact sparse surfaces, mid voxels, far SVO, and water are not presented as one coherent promoted LOD state across all views.
- Do not claim success from smoke passing or from a single route looking better.

Latest validated deltas:

- Page-table publish stall was fixed earlier:
  - old frame 40-80 publish average was about `1295ms`;
  - current publish cost is low in sampled frames.
- Mid voxel generation was sped up in `src/Simulation/SparseClipmap.cpp` by caching repeated terrain column samples within each generated voxel brick.
  - Moving-run sampled `PERF_SPARSE_CLIPMAP` improved from about `avgPrep=38.12ms`, `avgPumpVoxel=28.34ms` to about `avgPrep=28.96ms`, `avgPumpVoxel=21.94ms` in the first validation after that patch.
- Hidden-exact post-open repair was bounded in `src/main_launcher.cpp` so clean public frames no longer spend the full old repair budget every frame.
  - Latest capture: `build/captures/current_goal_postopen_probe_v1_walk_20260601`
  - sampled hidden-exact repair uses `timeLimitMs=8.0`, `rayBudget=512`.
  - frame 120/180 still show public-frame work and mid coverage catching up:
    - frame 120: `surfaceScreenPct=65.06`, `midVoxelScreenPct=6.25`, `body=136.83ms`
    - frame 180: `surfaceScreenPct=70.10`, `midVoxelScreenPct=2.24`, `body=150.64ms`
    - frame 240: `surfaceScreenPct=79.34`, `midVoxelScreenPct=0.29`, `body=88.06ms`
    - frame 360: `surfaceScreenPct=89.25`, `midVoxelScreenPct=0.014`, `body=73.61ms`

Current blocker:

- The architecture is not impossible, but the current production/presentation boundary is wrong.
- Exact sparse surface production, hidden-exact proof/repair, surface extraction/publish, and mid voxel generation still compete with public rendering.
- The public renderer is allowed to show mixed partially ready states instead of presenting stable LOD regions that promote atomically when render-ready.
- Minecraft-like engines avoid this by having a chunk/section presentation contract: build chunk data off the public path, publish whole coherent sections, and keep an older/lower LOD/placeholder stable until the replacement is ready. VENPOD is still doing too much repair/proof/generation while the user is looking at the frame.

Next correct work:

1. Keep focusing on the runtime voxel rendering/presentation bug, not materials, skyline closure, route composition, or broad reports.
2. Inspect the current mixed-state renderer contract:
   - exact surface readiness/promotion;
   - mid voxel readiness/promotion;
   - water/depth ownership around shorelines;
   - whether pass ordering lets lower LOD or water win where exact surface is drawable.
3. Patch toward coherent public presentation:
   - render only stable published terrain sections;
   - promote exact/mid regions only when render-ready;
   - do not let partial repair state globally or locally produce visible holes/fallback chunks.
4. Validate on user-visible captures, especially early public frames and water/shoreline views, not only smoke metrics.

## Current State - 2026-06-01 Publish Stall Fixed, Exact Surface Production Still Slow

Goal is active and not complete.

User-visible state:

- Rendering is still not fully solved.
- Public frames are more coherent than the earlier broken/water/grey-fallback captures, but terrain can still feel slow to converge because exact sparse surfaces are still produced on the runtime path.
- Do not claim completion from smoke passing.

Latest real capability delta:

- A major startup/render stall in page-table publish was removed.
- Previous validation after pre-publish extraction still had `publish` spikes:
  - frames 40-80 average `publish=1295.01ms`
  - max `publish=2051.09ms`
- Root cause was not GPU copy itself:
  - priority publish repeatedly scanned thousands of queued page publishes;
  - each successful publish also triggered `SparseVoxelWorld::RefreshStats()`, which scans resident records.
- Patch:
  - `SparsePagePublishQueue::PopReadyBatchForAnyCoord(...)`
    - batch-pops terrain-critical / hidden-exact ready publishes with one queue scan.
  - `src/main_launcher.cpp`
    - pre-pops terrain-critical and hidden-exact priority publish candidates once per frame;
    - defers sparse stats refresh while publishing pages;
    - keeps bounded pre-publish surface extraction from the prior patch.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Startup/public capture passed:
  - `build/captures/current_goal_publish_batchpop_v1_startup_20260601`
  - terrain-critical readiness:
    - `readyFrame=120`
    - `postNonReady=0`
- Publish timing improved:
  - previous stats-batch run, frames 40-80:
    - `PublishAvg=1295.0115`
    - `PublishMax=2051.09`
    - `BodyAvg=1613.32975`
  - current batch-pop run, frames 40-80:
    - `PublishAvg=1.83975`
    - `PublishMax=2.68`
    - `BodyAvg=330.45`
- Public frame 120 is much more coherent than old broken captures but not a final product-quality proof:
  - `surfaceScreenPct=63.7476`
  - `midVoxelScreenPct=3.9024`
  - `farSvoScreenPct=0.137`
  - `miss/proxy` remain clean in the sampled timeline.

Current remaining blocker:

- Page-table publish is no longer the bottleneck.
- Runtime frame time and startup convergence are now dominated by:
  - hidden-exact CPU foreground probing, often capped around `75ms` during startup and `25ms` post-open;
  - exact sparse surface extraction, around `40-47ms` in startup catch-up frames.
- This is still too synchronous:
  - exact surface extraction/proof is being performed on the runtime/render frame;
  - public rendering can still show some mid-voxel fallback while exact surfaces catch up.

Current architectural diagnosis:

- The LOD architecture is not inherently impossible:
  - exact sparse foreground,
  - mid voxel transition,
  - far SVO backdrop.
- The broken part is the production/presentation boundary:
  - exact surface generation/extraction/publish/readiness is still coupled to the render-frame loop;
  - public rendering sees partially ready exact state instead of atomically promoted coherent chunks/regions;
  - lower LOD is used as a live repair substitute instead of a coherent presentation level.

Next focused work:

1. Do not revisit material color, far closure, route shape, or water fallback without new owner evidence.
2. Attack exact-surface production next:
   - reduce hidden-exact CPU proof cost or move it out of every hot startup frame;
   - reduce exact surface extraction cost or make its work happen in a bounded pre-public build phase;
   - preserve coherent public presentation: exact surfaces should promote only when render-ready, while lower LOD should remain legitimate and stable until then.
3. Validate with:
   - startup/public timing,
   - public frame images,
   - midVoxelScreenPct and surfaceScreenPct convergence,
   - no miss/proxy/farWater regression.

## Current State - 2026-06-01 Publish/Surface Extraction Bottleneck

Goal is active and not complete.

User-visible state:

- Rendering is still not solved.
- Close/public views are more coherent than older broken captures after the exact-water and handoff fixes, but startup and streaming still expose broken terrain classes.
- The user is still seeing terrain assemble slowly, holes/gaps, and water/shoreline inconsistency. Treat that as the source of truth.

Most recent proven mechanism:

- Startup/public-open stalls are dominated by exact sparse surface readiness, not by far-SVO readiness or mid clipmap coverage.
- Logs showed `PERF_FRAME_END` `sparsePost.publish` spikes in the hundreds of ms to over 1s:
  - frame 36 around `1022ms`
  - frame 37 around `824ms`
  - many frames 40-80 around `400-600ms`
- Page-table publish itself was not the full cost. The publish loop was doing synchronous exact surface extraction when a ready page needed an exact surface and `surfaceKnown` was false.
- Dedicated surface extraction outside publish is capped very low by default:
  - `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS`, default `3`
- So there are two bad modes:
  - old mode: publish blocks and does expensive surface extraction inline, causing long startup/frame stalls;
  - attempted nonblocking mode: publish stops blocking, but the 3ms extraction path cannot drain thousands of pending exact surfaces fast enough, so public readiness never opens in the smoke window.

Patches currently in the working tree from this investigation:

- `src/Simulation/SparsePagePublishQueue.h/.cpp`
  - Added `PopReadyForAnyCoord(...)` to avoid repeated queue scans for hidden-exact priority coords.
  - Added `RequeueBack(...)` during the failed nonblocking publish experiment.
- `src/Graphics/SparseVoxelGpuResources.h/.cpp`
  - Added page-table copy batch begin/end helpers and optional managed resource-state handling in `EmitPageTableCopy`.
- `src/main_launcher.cpp`
  - Added a hidden-exact critical coord set for faster priority publish matching.
  - Uses `PopReadyForAnyCoord(...)` for hidden-exact priority publish.
  - Wraps page-table copy entries in a page-table copy batch.
  - Contains the failed nonblocking exact-surface publish gate experiment.

Validation results:

- `build/captures/current_goal_publish_lookup_v1_startup_20260601`
  - Build passed.
  - Queue lookup optimization was correct but not sufficient.
  - Publish spikes remained large.
- `build/captures/current_goal_pagtable_batch_v1_startup_20260601`
  - Build passed.
  - Page-table transition batching was semantically reasonable but did not fix startup.
  - Publish spikes remained large.
- `build/captures/current_goal_publish_nonblocking_surface_v1_startup_20260601`
  - Build passed but validation failed.
  - Public readiness did not open by the smoke capture window.
  - At frame 120:
    - `surfaceQueued=5861`
    - `surfaceResidentPayloads=1402`
    - `surfaceGpuRecords=1413`
    - `uploading=6955`
    - `hiddenExactStillMissing=3398`
    - `terrainCriticalBlocked=1`
    - `shaderUnsafeBlocked=1`
    - mid/far coverage was already available
  - Conclusion: simply making publish nonblocking starves exact surface publication unless surface extraction/publish readiness is moved earlier or budgeted properly.

Current root cause statement:

- The renderer is mixing partially ready exact sparse terrain with lower LOD because exact surface extraction/GPU-publish readiness is not draining in a coherent startup/public-open phase.
- Mid/far coverage can be complete while exact surface draw records are still missing.
- Lower LOD then hides exact gaps or public render is held too long, depending on gate settings.
- This is a real streaming/render pipeline issue, not material color, terrain shape, far-SVO cache, or water fallback.

Next concrete engineering step:

1. Fix/revert the failed nonblocking surface gate in `src/main_launcher.cpp`; do not leave it as-is.
2. Move exact surface extraction pressure out of the page-table publish loop into a bounded, prioritized pre-publish/startup drain.
3. Give startup/public-held exact surface extraction enough budget to produce GPU surface records/draw slots before public open, without letting publish block for hundreds of ms.
4. Validate with:
   - Release build
   - startup smoke
   - first public frame timing
   - `publish` ms timeline
   - `surfaceQueued`, `surfaceGpuRecords`, `ready`, `hiddenExactStillMissing`
   - visual public frames for water/shoreline/holes

Do not mark complete until public gameplay frames are coherent and the startup/streaming path is bounded.

## Current State - 2026-06-01 Exact Water Draw v1

Goal is active and not complete.

User-visible objective remains:

- Public gameplay frames must show coherent voxel terrain/water.
- Do not redefine success around green smoke logs.
- Avoid material-only polish or startup/reporting detours unless they directly unblock visible rendering correctness.

Latest proven mechanism:

- Frame 420 shoreline/water artifacts were not primarily missing terrain.
- Exact water top surfaces existed, were resident, had GPU records/draw slots, but were deliberately discarded by `PS_SparseSurface.hlsl` in above-water views.
- That forced the analytic/deterministic water plane or far-water background path to own broad basin pixels even when exact sparse water geometry was available.
- This violated the intended public ownership contract: exact sparse surface should own when it is resident and drawable; lower/far/analytic water should be fallback.

Patch made:

- `assets/shaders/Graphics/PS_SparseSurface.hlsl`
  - Stopped discarding `MAT_WATER` sparse surface faces in above-water views.
  - Kept discard for non-water terrain below the deterministic water surface.
  - Updated comment: analytic water is fallback/background, not a replacement for resident exact water.
- Existing unvalidated plane alignment patch is still present:
  - `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - `assets/shaders/Graphics/PS_SparseSurface.hlsl`
  - `FAR_WATER_SURFACE_Y = FAR_SEA_LEVEL + 1.0f`
  - This aligns analytic water with generated water voxel top faces.

Validation:

- Build/asset refresh passed:
  - `.\build.ps1 -Config Release`
- Same moving frame 420 capture after exact-water draw:
  - `build/captures/current_goal_exact_water_draw_v1_frame420_20260601`
  - Capture passed with `-SkipOwnershipDiagnostics`.
  - Camera was valid:
    - `cameraInside=0`
    - `feetInside=0`
  - Terrain critical readiness was clean at frame 420:
    - `postNonReady=0`
  - Layer shift at frame 420:
    - before exact-water draw / water surface patch frame 420:
      - `surfaceScreenPct=44.43`
      - `midVoxelScreenPct=17.29`
      - `farWaterScreenPct=8.80`
      - `waterContextScreenPct=6.18`
    - after exact-water draw:
      - `surfaceScreenPct=76.19`
      - `midVoxelScreenPct=0.13`
      - `farWaterScreenPct=0.61`
      - `waterContextScreenPct=32.86`
  - Interpretation:
    - Foreground basin water is now resident exact sparse surface ownership, not far-water/mid fallback.
    - This is a real renderer ownership fix, not a cosmetic material tweak.
- Moving route contact sheet after patch:
  - `build/captures/current_goal_exact_water_draw_v1_walk_180_520_20260601`
  - Frames 180-520 look materially more coherent around water/shoreline.
  - No obvious old broad teal/gray broken water sheet in the contact sheet.
  - Layer timeline shows:
    - `farWaterScreenPct` stays low, mostly `0.0-1.7%`.
    - `midVoxelScreenPct` drops below `1%` after frame 255 and near zero by later shoreline frames.
    - `surfaceScreenPct` rises steadily to `91%` by frame 525.
    - `missScreenPct=0`, `unsafeNearMissScreenPct=0`, `heightProxyScreenPct=0`.
- Current default/public stationary view after patch:
  - `build/captures/current_goal_exact_water_draw_v1_default_public_20260601`
  - Contact sheet frames 120-320 are coherent; no obvious broken terrain chunks or broad wrong water sheet.
  - Layer timeline is stable:
    - `surfaceScreenPct` about `63-66%`
    - `midVoxelScreenPct` drops from about `4.19%` at frame 120 to about `1.79%` at frame 330
    - `farWaterScreenPct` about `0.31-0.37%` after frame 120
    - `waterContextScreenPct` about `14.03-14.07%`
    - `missScreenPct=0`, `unsafeNearMissScreenPct=0`, `heightProxyScreenPct=0`
  - Startup/public render is still held until the private readiness gate opens; this capture does not prove startup is ideal, only that the current public frames are more coherent once shown.

Important caveats:

- Do not mark the goal complete.
- The exact-water fix improves the current moving shoreline failure class, but it has not proven every user screenshot/view class is solved.
- The frame 420 camera can shift between separate runs because walk motion depends on when public rendering opens; use longer route contact sheets and camera logs, not a single-frame screenshot, for broad claims.
- The prior full ownership-diagnostic capture could stall in early startup/publish work. That startup inefficiency is real but separate from this renderer ownership fix.

Next concrete work:

1. Run a current default/live `rebrun` public sequence and compare against the user's "slowly streams in / holes remain" class.
2. If remaining visible issues exist, sample current pixels and classify owner:
   - exact sparse surface
   - exact sparse water
   - mid voxel
   - far SVO
   - far water
   - sky/miss
3. Do not return to material polishing unless owner/material evidence says geometry is correct and only readability is bad.
4. If full ownership diagnostics still stalls before public render, treat that as a startup/publish efficiency issue only after current public-frame rendering correctness is checked.

## Current State - 2026-06-01 Bounded Handoff Helped, Early Public Collapse Remains

Goal is active and not complete.

User-visible state:

- Rendering is still not fixed. Later public moving frames are materially more coherent, but early public frames can still collapse into mid-closure/coarse fallback for a short window, and broader live views can still show terrain assembling slowly or rendering with gaps.
- Do not treat smoke pass or ownership CSV success as completion. The user-visible goal is coherent gameplay terrain.

Latest code direction:

- `src/main_launcher.cpp`
  - Added bounded exact-repair handoff logic so a limited hidden-exact repair set does not globally collapse the wide exact surface raster band.
  - Defaults now allow bounded shader/hide-exact dirty counts before demotion:
    - `VENPOD_SPARSE_SURFACE_PROMOTION_MAX_SHADER_NONREADY=64`
    - `VENPOD_SPARSE_SURFACE_PROMOTION_MAX_HIDDEN_EXACT_MISSING=96`
    - `VENPOD_SPARSE_SURFACE_DEMOTION_MAX_HIDDEN_EXACT_MISSING=max(128, promotion)`
    - `VENPOD_SPARSE_SURFACE_PROMOTION_CLEAN_FRAMES=1`
  - Added `VENPOD_SPARSE_SURFACE_BOUNDED_HIDDEN_EXACT_REPAIR_KEEPS_WIDE_RASTER`, default `1`.
- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Diagnostic mode `68` still exists and separates mid closure, mid column proxy, raw mid DDA, far SVO, and water ownership paths.

Latest validation artifacts:

- `build/captures/current_goal_gateoff_f300_bounded_handoff_v2_20260601`
- `build/captures/current_goal_public_walk_bounded_handoff_v2_20260601`
- `build/captures/current_goal_public_walk_bounded_handoff_v3_20260601`

What improved:

- Frame-300 diagnostic with public gate disabled no longer collapses into broad mid closure:
  - previous frame 300 mid voxel screen pct was about `16.8-17.1`.
  - bounded handoff v2 frame 300 mid voxel screen pct is about `2.1`.
  - `surfaceRasterMax=2560`, `surfacePromoted=1`, `miss=0`, `unsafeNearMiss=0`.
- Public moving validation v3 fixed the later frame-330 spike:
  - v2 frame 330: `midVoxelScreenPct=15.8864`, `surfaceScreenPct=44.1104`.
  - v3 frame 330: `midVoxelScreenPct=0.9263`, `surfaceScreenPct=49.1949`.
  - frames 340/360/400/440 stay below about `0.8` mid voxel screen pct.

What is still broken:

- Public moving validation v3 still has an early collapse:
  - frame 190: `surfaceScreenPct=43.5836`, `midVoxelScreenPct=17.4389`.
  - frame 200: `surfaceScreenPct=44.1672`, `midVoxelScreenPct=17.3826`.
  - frame 210 recovers to `surfaceScreenPct=60.3282`, `midVoxelScreenPct=3.0361`.
- Mechanism:
  - frame 190 has hidden-exact `accepted/stillMissing=160`, above the `128` bounded demotion cap.
  - frame 200 has shader unsafe `contractNonReady=112`, above the `64` bounded shader cap.
  - the renderer globally falls back to mid closure during that repair burst, even though lower LOD coverage is otherwise complete.

Current grounded conclusion:

- A real handoff/readiness bug was improved, but the full goal is not solved.
- The next root cause is not material color, terrain generation, water tuning, route composition, or far-SVO readiness.
- The next mechanism to debug is the early public-frame repair burst: why visible exact/non-ready and hidden-exact counts spike around frames 190-200 after public render opens, and whether the correct fix is earlier repair/prefetch, local rather than global demotion, or a stricter public-open readiness condition.

Next focused step:

1. Capture/inspect frames 180-210 from `current_goal_public_walk_bounded_handoff_v3_20260601` or rerun that slice if artifacts are incomplete.
2. Audit the exact bricks responsible for frame-190 hidden-exact `160` and frame-200 shader unsafe `112`.
3. Patch the request/readiness path so this early burst is drained before it causes public-frame fallback, or make demotion local so missing exact bricks do not collapse the whole screen.
4. Validate with before/after frame 190/200/210 images and metrics.

## Current State - 2026-06-01 Mid-Closure Fallback Still Dominates Moving Public Frames

Goal is active and not complete.

User-visible state:

- Rendering is still not fixed. Public/moving frames can still show terrain assembling slowly, holes/gaps, and incoherent water/shoreline/fallback ownership.
- Do not claim smoke/audit success as completion. The target is coherent runtime terrain.

Latest mechanism confirmed after compaction:

- Added temporary diagnostic shader mode `68` in `assets/shaders/Graphics/PS_Raymarch.hlsl`.
- Mode `68` separates mid-voxel subpaths:
  - mid closure = yellow,
  - mid column proxy = cyan,
  - raw mid DDA = green,
  - far SVO = blue,
  - water = dark blue.
- Same frame/camera debug showed the large visible fallback terrain band is dominated by `TryBuildResidentMidVoxelClosureHit`, not raw mid DDA and not far SVO.

Latest artifacts:

- `build/captures/current_goal_gateoff_f300_midsubpath68_20260601`
- `build/captures/current_goal_gateoff_f300_normal_20260601`
- `build/captures/current_goal_gateoff_f300_threshold_probe_20260601`

Key telemetry from the threshold probe:

- At frame 300:
  - camera is valid: `cameraInside=0`, `feetInside=0`.
  - far SVO and mid coverage are complete: `farStage=complete`, `farCov=1.00/1.00`, `midCov=1.00/0.97`.
  - shader unsafe exact feedback is clean: `shaderUnsafeNonReady=0/64`.
  - current hidden-exact scanner still finds and accepts missing exact work: `hiddenExactMissing=34/96`, `hiddenExactAccepted=34`.
  - exact surface raster collapses to `surfaceRasterMax=1024`, `surfacePromoted=0`.
  - screen ownership after collapse:
    - `surfaceScreenPct=43.76`,
    - `midVoxelScreenPct=17.15`,
    - `farWaterScreenPct=9.01`,
    - `missScreenPct=0`.
  - background ownership:
    - `midVoxelPct=30.49`,
    - `farWaterPct=16.02`,
    - `farSvoPct=1.10`,
    - `missPct=0`.

Current grounded interpretation:

- This is a coherent handoff/readiness bug, not a camera-validity or far-cache bug.
- The renderer has complete lower LOD coverage and no global miss, but the exact surface band is globally demoted whenever current hidden-exact repair discovers a small dirty set.
- That global demotion forces broad terrain back through mid closure and water/background fallback, which is why terrain appears to slowly build in and remains patchy in moving/public views.
- Relaxing promotion thresholds by environment did not fix frame 300 because accepted hidden-exact work still keeps the current view unpromoted.

Next focused engineering path:

1. Do not tune materials, skyline closure, water color, terrain generation, or route composition.
2. Inspect `src/main_launcher.cpp` promotion/handoff logic around `surfaceClean`, `surfacePromoted`, hidden-exact accepted/missing thresholds, and exact raster max selection.
3. Patch the handoff so a small bounded set of hidden-exact repair candidates does not globally collapse the exact surface band if shader unsafe feedback says current visible exact ownership is render-ready.
4. Preserve terrain-critical safety:
   - unsafe near misses must remain zero,
   - true non-ready exact foreground must still clamp/prefetch,
   - no fake height/water/proxy fallbacks.
5. Validate on the same frame-300 moving/default camera:
   - `surfaceRasterMax` should stay wide when shader unsafe is clean,
   - `midVoxelScreenPct` should not jump to ~17% from a small hidden-exact repair burst,
   - `miss`, proxy, and unsafe near miss remain zero.

## Current State - 2026-05-31 Handoff Hysteresis v2

Goal is active and not complete.

Patch made:

- `src/main_launcher.cpp`
  - Exact sparse surface raster dynamic promotion is now enabled by default.
  - Initial promotion remains strict: current-view shader unsafe feedback and hidden-exact feedback must be clean before widening exact raster.
  - After promotion, the handoff now has hysteresis:
    - `VENPOD_SPARSE_SURFACE_DEMOTION_MAX_SHADER_NONREADY` default: `max(8, promotion threshold)`.
    - `VENPOD_SPARSE_SURFACE_DEMOTION_MAX_HIDDEN_EXACT_MISSING` default: `max(96, promotion threshold)`.
  - Reason: audit showed the remaining bad default-frame regions were mostly mid voxel ring1/cell24 at ~2500 units. The previous default capped exact raster at 1792, forcing visible terrain into coarse mid LOD. A first attempt at dynamic promotion still collapsed the whole view for one small hidden-exact repair burst (`17` candidates) even though shader unsafe feedback said visible exact terrain was ready.

Evidence before patch:

- `build/captures/current_default_161_641_normal`
  - frame 401: `surfaceRasterMax=1792`, `midVoxelScreenPct ~= 12.10`.
- `build/captures/current_default_161_641_normal/visual_bad_pixel_audit_f401.csv`
  - suspicious pixels: `79 mid_voxel`, `45 exact_sparse_surface`, `4 far_svo`.
  - material: `84 stone`, `44 sand`.
- `build/captures/current_default_161_641_normal/midfar_geometry_audit_f401.csv`
  - `79/83` mid/far bad samples were `mid_voxel`.
  - all mid samples were `ring1`, `cellSize=24`, average hit distance about `2545`.
  - dominant face type: side faces (`50/83`).
- Dynamic 2560 probe showed mid voxel could drop to low single digits, but frame 284 hidden-exact repair (`17`) demoted the view back to the old coarse fallback path.

Validation after patch:

- Build passed:
  - `.\build.ps1 -Config Release`
- Default camera validation:
  - `build/captures/handoff_hysteresis_v2_default_161_641`
  - frame 161/281/401/521/641 all reported `surfaceRasterMax=2560`, `surfacePromoted=1`, `hiddenExactMissing=0/0`, `shaderUnsafeNonReady=0/0`.
  - mid voxel screen pct at sampled frames:
    - frame 165: `2.8971`
    - frame 285: `2.0364`
    - frame 405: `1.8208`
    - frame 525: `1.7299`
    - frame 645: `1.6964`
  - no `miss`, no `unsafeNearMiss`, no height proxy.
  - The old frame-285 ownership collapse is gone:
    - before v2: frame 285/288 jumped to `midVoxel ~= 366743`, `farSurface=0`.
    - after v2: frame 285/288 is `midVoxel ~= 42226`, `farSurface ~= 267899`.
- Moving walk validation:
  - `build/captures/handoff_hysteresis_v2_walk_180_460`
  - no camera/feet inside terrain in sampled exposures.
  - frame 260 onward generally holds `surfaceRasterMax=2560`, `surfacePromoted=1`.
  - mid voxel drops below 1% in frames 240-360, then rises again in later water-crossing views where terrain is farther/side-heavy.
  - no miss/proxy/unsafeNearMiss regression.
- Frame 420 shoreline audit:
  - `build/captures/handoff_hysteresis_v2_walk_180_460/water_shoreline_audit_f420`
  - `128/128` sampled shoreline boundary pixels are `raymarch_exact_layer|sand|terrain_boundary_near_water`.
  - Interpretation: that foreground water edge is exact sand/terrain boundary, not water incorrectly owning terrain.

Current grounded conclusion:

- A real renderer handoff bug was fixed: exact surface was capped too tightly by default, and dynamic promotion was too brittle. This forced visible terrain around 2.5k units into mid ring1/cell24 and periodically collapsed back to coarse fallback.
- Static/default camera is materially better and stable after v2.
- The full goal is still not complete. Moving/water-crossing views can still look rough because later views expose farther terrain and side-heavy terrain composition even with a 2560 exact band.

Next focused step:

1. Use `handoff_hysteresis_v2_walk_180_460` as the current baseline.
2. For frames where mid voxel rises again (`405-465`), run a targeted mid/far geometry audit if the visible image still looks broken.
3. Decide from that audit whether the next patch should be:
   - promoted exact raster distance beyond 2560 with measured cost,
   - mid ring1 geometry quality / cell size,
   - or terrain/shoreline shape composition.
4. Do not revert to material tuning or broad startup gates unless the current frame evidence points there.

## Current State - 2026-05-31 Post-Compaction Check

Goal is active and not complete.

Fresh state check after compaction:

- No `VENPOD` process is currently running.
- The previously timed-out validation run actually finished and captured frames:
  - `build/captures/startup_public_silhouette_continuity_v1_f161_241/engine_frame_0161.bmp`
  - `engine_frame_0181.bmp`
  - `engine_frame_0201.bmp`
  - `engine_frame_0221.bmp`
  - `engine_frame_0241.bmp`
- The run shut down cleanly at frame 265.

Important result:

- Re-ran `terrain_hole_ray_audit.ps1` on first-public frame 161 after the silhouette-continuity v1 terrain-shape patch.
- Output:
  - `build/captures/startup_public_silhouette_continuity_v1_f161_241/terrain_hole_ray_audit_f161.csv`
  - `dominantBucket=true_procedural_air` for `64/64` sampled skyline-gap pixels.
- Interpretation: the silhouette-continuity v1 patch did not fix the sampled first-public skyline holes. It may have changed world shape/cost, but it did not create terrain truth along the audited rays.

Current grounded conclusion:

- The renderer/streaming issue is still unresolved.
- One handoff path improved earlier, but the user-visible terrain remains broken in broad/live views.
- First-public skyline holes are still generated as procedural air in the audited pixels.
- The current working direction must stay mechanism-facing: identify why the visible public frame is composed of wrong/incoherent terrain owners, not mark smoke/audit improvements as success.

Next focused step:

1. Do not continue broad terrain-shape tuning from this failed v1 result.
2. Use the latest failing public/live frame and owner/material captures to identify which visible regions are:
   - procedural air,
   - exact sparse unavailable,
   - exact sparse drawable but not winning,
   - mid/far fallback owning too much,
   - or water owning where terrain should be.
3. Patch only the specific owner/readiness/terrain-truth path proven by that capture.

## Current State - 2026-05-31 Targeted Handoff/Streaming Fix

Goal is active and not complete.

User-visible objective:

- Public gameplay frames must show coherent voxel terrain without slow partial assembly, holes, wrong water/fallback ownership, or coarse mid/far chunks where exact foreground terrain should exist.
- Do not claim success from a smoke pass or audit if the runtime image is still broken.

What changed most recently:

- `src/main_launcher.cpp` now prioritizes hidden-exact tracked bricks through GPU page-table publish, not just generation/upload.
- Foreground unsafe exact repair now defaults to repairing even small visible non-ready exact sets instead of ignoring residual sets below 33.
- Exact sparse surface promotion is now strict by default:
  - default `VENPOD_SPARSE_SURFACE_PROMOTION_MAX_SHADER_NONREADY=0`
  - default `VENPOD_SPARSE_SURFACE_PROMOTION_MAX_HIDDEN_EXACT_MISSING=0`
  - the wider exact surface band stays closed while current-view exact foreground proof is dirty.

Latest validation artifact:

- `build/captures/moving_frame760_820_strict_promotion_v1`
- Build passed before this run.
- Frames 760/780/800:
  - camera valid, no camera/feet inside terrain.
  - Far SVO complete, mid coverage about 0.98-0.99.
  - shader unsafe exact non-ready remained high: 94, 75, 35.
  - `surfaceRasterMax=1024`, `surfacePromoted=0`.
  - foreground repair requested those missing exact bricks.
- Frame 820:
  - shader unsafe exact non-ready reached 0.
  - hidden-exact missing reached 0.
  - `surfaceRasterMax=1792`, `surfacePromoted=1`.
  - contact sheet looks coherent for this route slice.

Current interpretation:

- This fixed one concrete renderer/streaming failure mode: partial wide exact-surface draw while foreground exact bricks were known missing.
- The system is still not solved globally. User screenshots still show slow far/foreground streaming and shoreline/water/gap artifacts in broader live views.
- Hidden-exact probing still hits the 25 ms time limit in some frames, so request discovery is still expensive and may lag in broad views.

Next focused step:

1. Do not drift to materials, terrain generation, route composition, or docs.
2. Run a broader same-camera/live-style capture with exact-frame owner/material/debug output.
3. If broken pixels remain, classify their real owner:
   - exact surface exists but is not winning,
   - exact surface missing/not drawable,
   - mid/far fallback legitimately owning,
   - water owning before deterministic terrain,
   - or procedural truth air/water.
4. Patch only the proven owner/readiness path.

## Latest Status - 2026-05-31 After Compaction

Goal is still active and not solved.

User-visible state:

- Public gameplay terrain still does not meet the bar. It can still show slow terrain assembly, mixed lower-LOD fallback/water, and visual gaps in moving/general views.
- Do not claim the renderer is fixed. Recent work improved one concrete mechanism only.

Latest patch made this session:

- `src/main_launcher.cpp` now has a bounded shader-unsafe foreground repair lane.
- When shader feedback sees mid/far/water/fallback owning pixels while closer exact terrain is expected and not render-ready, it can promote those exact brick requests as urgent terrain-critical visible work.
- This does not change materials, generation, water fallback, far-SVO policy, or exactNear.

Validation result:

- Release build passed.
- Moving smoke capture still failed visual markers.
- Mechanism improved:
  - old moving frame 300: `surfacePromoted=0`, `shaderUnsafeNonReady=80/32`.
  - new moving frame 300: `surfacePromoted=1`, `shaderUnsafeNonReady=1/32`.
  - new frame 180/184 foreground repair requested 64 current-view hidden exact bricks instead of only a few.
- This proves current-view exact request admission was too weak before, but it does not solve the full rendering problem.

Current next question:

- Since exact hidden-miss readiness is now much better by frame 300 but the image still fails, the next step is owner/debug capture on the current failing frames to identify the remaining bad pixels:
  - exact surface draw/cull/coverage issue,
  - legitimate lower-LOD/background terrain carrying too much screen,
  - water/fallback wrongly owning land,
  - or smoke visual classifier catching a different composition problem.

Anti-drift:

- The next useful result must explain or fix remaining bad pixels in the current public gameplay capture. Do not move to material tuning, terrain generation, route composition, or broad reports without evidence from owner/debug captures.

## State Check - 2026-05-31 Current Session

Goal is active and not complete.

Current user-visible problem:

- Terrain rendering is still broken in public gameplay views: distant terrain slowly assembles, exact/mid/far/water ownership is visibly inconsistent, and holes/gaps remain even after waiting.
- Do not describe recent work as "fixed"; at best, it has isolated mechanisms and improved some selected captures.

Current code/worktree state:

- `src/main_launcher.cpp`, `rebrun.ps1`, and `engine_capture_smoke.ps1` currently contain an in-progress nonblocking/dynamic raster path:
  - startup hidden-exact repair no longer blocks public render by default (`VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS=0`);
  - render-time exact sparse surface raster can be clamped to a foreground band while hidden-exact repair is unresolved;
  - request/repair can still target the wider exact band.
- Latest patch changed the promotion gate:
  - stores shader unsafe feedback readiness counts from `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK`;
  - uses current-view shader `nonReady` and hidden-exact `stillMissing/accepted` thresholds to decide whether exact raster can promote;
  - no longer blocks promotion just because stale hidden-exact tracked coords remain in the tracking vector.

Most recent concrete mechanism:

- Early/public frames look broken when partial distant exact sparse surfaces draw over coherent lower LOD before the exact band is actually ready.
- A foreground raster cap around `1792` avoids the worst checker/panel breakup, but it is too coarse if never promoted.
- A static/default `2560` cap previously reduced bad mid ring/cell ownership at frame 300, but early public frames could still show partial exact leakage.
- New default behavior after patch:
  - frame 60 stays foreground-capped while hidden exact is still uploading/missing;
  - frames 120+ promote to `surfaceRasterMax=2560` once current-view unsafe feedback is below threshold.

Validation artifacts from this session:

- Build passed:
  - `.\build.ps1 -Config Release`
- Default static/public validation after patch:
  - `build/captures/shader_unsafe_promotion_v2_default_60_240_20260531`
  - `build/captures/shader_unsafe_promotion_v2_default_300_20260531`
  - frame 60: `surfaceRasterMax=1792`, `hiddenExactMissing=1538/32`, `surfacePromoted=0`
  - frame 120: `surfaceRasterMax=2560`, `shaderUnsafeNonReady=6/32`, `surfacePromoted=1`
  - frame 300: `surfaceRasterMax=2560`, `shaderUnsafeNonReady=6/32`, `surfacePromoted=1`
  - frame 300 layer metrics: `surfaceScreenPct=56.624`, `midVoxelScreenPct=4.233`, `farWaterScreenPct=5.9196`, `missScreenPct=0`
- Comparison against stuck foreground cap:
  - old v1 default frame 300 stayed `surfaceRasterMax=1792`, with `midVoxelScreenPct` about `11.53`.
  - v2 drops mid voxel to about `4.23` without fake proxy/miss.
- 3072 promoted cap experiment:
  - `build/captures/shader_unsafe_promotion_v2_raster3072_60_240_20260531`
  - `build/captures/shader_unsafe_promotion_v2_raster3072_300_20260531`
  - only small frame-300 mid improvement (`~4.23 -> ~3.84`) and much heavier early hidden-exact work (`~79ms` probe at frame 60), so do not make 3072 the next default without a stronger reason.
- Moving walk validation still fails user-visible expectations:
  - `build/captures/shader_unsafe_promotion_v2_walk_180_460_20260531`
  - frame 180 exposure: `surfaceRasterMax=1792`, `shaderUnsafeNonReady=122/32`, `farWaterScreenPct=33.5527`
  - frame 300 exposure: `surfaceRasterMax=1792`, `shaderUnsafeNonReady=80/32`, `midVoxelScreenPct=25.7678`, `farWaterScreenPct=7.4051`
  - later frames are mostly exact surface, but the moving view still shows fallback/water/lower LOD carrying pixels while exact catches up.

Next focused step:

1. Do not call the renderer fixed.
2. Keep the shader-unsafe promotion gate; it is a real static/default improvement.
3. Next root-cause path is the moving-view fallback/water dominance when `shaderUnsafeNonReady` is high:
   - determine whether fallback water is legitimate water or hiding deterministic land in frames 180/300;
   - determine why current-view exact non-ready remains high during movement even though terrain-critical post readiness is zero;
   - likely next mechanism is hidden-exact/current-view repair priority or a better coherent lower LOD owner for those pixels, not material tuning.
4. Do not broaden to 3072 by default yet; it added early repair cost and did not solve the moving fallback issue.

Anti-drift:

- The next useful result is not another report. It is a mechanism change that makes the same public gameplay view render coherently earlier and remain coherent as exact terrain streams.

## Authoritative Current State - 2026-05-31

Read this section first. Older sections below contain useful history but include superseded hypotheses.

Active goal:

- Fix user-visible voxel terrain rendering so gameplay/public frames show coherent terrain, not slow partial assembly, holes, water/fallback over terrain, or coarse mid/far chunks where exact terrain should be visible.
- Do not drift into material tuning, skyline/generation work, route composition, or broad reports unless they directly explain the current renderer/streaming failure.

What is actually proven now:

- The renderer is not "fixed." The current root issue is still runtime terrain ownership/readiness under broad camera views.
- Static/waterline captures can look coherent and are no longer enough. Stress/general views still expose broken terrain and slow exact recovery.
- A prior hidden-exact feedback path was useful but incomplete: it can request missing exact terrain hidden by lower LOD, but broad high-altitude/general views can flood exact surface work and still miss current-frame critical terrain.
- Latest mechanism found:
  - In stress view frame 484, terrain-critical exact bricks were visibly required but missing because broad feedback/hidden-exact work and free-page pressure starved the small current-frame critical probe.
  - Patch v1 gave terrain-critical selected bricks a reserve even during high-altitude LOD throttle and stopped broad shader unsafe feedback from being render-critical during that high-altitude mode.
  - This fixed the targeted frame-484 `postMissing=8` to `postNonReady=0`.
  - But the same run then failed with sparse surface GPU upload/record overflow around frames 410-429, meaning the exact-surface workload is still being admitted too broadly under stress views.

Current code state:

- `src/main_launcher.cpp`
  - terrain-critical reserve is active whenever screen-critical prefetch is enabled, including high-altitude LOD-throttled frames.
  - shader unsafe feedback is only treated as render-critical when not in high-altitude LOD mode.
- `src/Simulation/SparseVoxelWorld.cpp/.h`
  - request/replacement priority work exists, including queued lower-priority eviction for higher-priority requests.
- `handoff.md` is now the continuity source. Keep updating this top section before stopping.

Most recent validation artifacts:

- Pre-patch stress failure:
  - `build/captures/current_stress_public_180_460_20260531`
  - `build/captures/current_stress_frame484_normal_20260531`
  - frame 484 failed: `postMissing=8`.
- Owner debug for same frame:
  - `build/captures/current_stress_frame484_owner55_20260531`
- After terrain-critical reserve v1:
  - `build/captures/stress_frame484_priority_reserve_v1_20260531`
  - frame 484 terrain-critical fixed: `samples=8 readyFrame=484 postNonReady=0`.
  - new failure: sparse surface runtime contract reports upload overflow around frames 410-429.

Current blocker:

- The renderer/streaming system is choosing too much exact-surface recovery work in broad/stress views, filling or pressuring surface GPU records/pool work, while the visible image still relies on mixed fallback layers.
- The correct next mechanism is not "make material prettier" or "raise budgets blindly." It is admission policy:
  - current-frame terrain-critical exact bricks must always win;
  - broad hidden-exact/unsafe feedback should not flood exact surface records in high-altitude/general broad views;
  - lower LOD should carry broad terrain coherently until exact can be promoted without overflowing surface publish/upload capacity.

Next smallest patch direction:

- In `src/main_launcher.cpp`, inspect hidden-exact request/probe admission around the hidden-exact max request variables and `requestHiddenExactCoord`.
- Add a high-altitude/general-view throttle for broad hidden-exact repair after public render is open:
  - keep terrain-critical probe active/reserved;
  - suppress or sharply cap broad hidden-exact feedback in high-altitude LOD-throttled frames;
  - do not affect startup proof/public-open readiness unless proven necessary.
- Rebuild and validate:
  - stress frame 484 must keep `postNonReady=0`;
  - frames 410-429 must no longer hit sparse surface upload/record overflow;
  - a broader stress capture must show no terrain-critical misses;
  - a normal walk/default capture must not regress.

Anti-drift reminder:

- Every probe must answer what changed, what signal is expected, and what file/code path is next if it fails.
- Do not declare success from local green checks if the visible terrain still slowly assembles or shows holes/fallback terrain.

Last updated: 2026-05-31 compaction state; current lead is render pass ownership/depth, not another broad streaming audit

## Current Resume State - 2026-05-31

Read this first after compaction. The active goal is still user-visible rendering correctness: public gameplay frames must show coherent terrain, not water/background/fallback layers covering terrain or slow partial assembly.

What is proven now:

- Hidden-exact feedback/admission work was only a partial path. It improved some lifecycle cases, but it did not fix the user-visible broken rendering.
- A stale startup hidden-exact proof bug was found and patched in `src/main_launcher.cpp`: when hidden-exact startup proof mode is disabled, old startup proof tracked coords are now retired after public render opens. This removed huge stale `startupProofStillMissing`/recovery noise, but it did not fix the visible scene.
- The latest moving walk capture still has a serious render ownership issue. Frame 300 is the important repro:
  - artifact: `build/captures/startup_proof_retire_v1_walk_180_500_20260531/engine_frame_0300.bmp`
  - camera is valid and above dry terrain: `cameraAbove=8.28`, `feetAbove=2.28`, terrain height about `-44.28`, sea level `-48`.
  - terrain-critical exact readiness is clean in that run, but the image shows a large water/background layer covering foreground terrain.
  - layer metrics around frame 300: surface about `33.47%`, mid about `16.71%`, farWater about `30.33%`, miss about `0.03%`.
- That makes the current best mechanism-facing hypothesis: the fullscreen raymarch water/background pass is not being clipped/composited correctly against exact sparse surface depth/ownership, or sparse surface depth is not written/used correctly. This is a renderer pass arbitration/depth problem, not material tuning.
- Frame 500 failure in the same moving smoke is mostly a close sand-wall route/composition artifact:
  - artifact: `build/captures/startup_proof_retire_v1_walk_180_500_20260531/engine_frame_0500.bmp`
  - visual audit mostly exact sparse surface sand. Do not use that frame as the root water/streaming repro.
- Static waterline capture after the stale-proof patch is mostly stable and does not reproduce the "foreground becomes water" issue:
  - artifact dir: `build/captures/startup_proof_retire_v1_waterline_347_887_20260531`

Next concrete code path:

- Inspect render pass order and depth state:
  - `src/main_launcher.cpp` around the sparse surface pass and fullscreen raymarch pass, roughly lines `13945-14040`.
  - `src/Graphics/Renderer.cpp` implementations for `RenderSparseSurfaceFaces` and the raymarch/background pass.
  - depth-stencil state creation and binding for sparse surface and fullscreen raymarch.
- Answer: does sparse surface write usable depth, and does fullscreen raymarch respect it so water/background cannot overwrite nearer exact terrain?
- Patch only if that mechanism is proven. Do not drift back to shader color/material, terrain generation, far closure, skyline closure, or generic request-lifecycle reports.

## Compaction Resume Note

- Read this file before continuing after compaction, per user request.
- Active goal is still the user-visible rendering result: terrain must look coherent in gameplay/public views, not just pass local audits.
- The goal tool currently reports this objective as active. Do not create a duplicate goal.
- Do not claim that the terrain is fixed. The latest useful fixes improved first-public-frame coherence and removed the dominant close mid-voxel ring1 handoff band in the static waterline view, but settled public views still have skyline/procedural-air gaps and smaller shoreline/fallback questions.
- Current next mechanism: hidden-exact feedback/recovery is seeing thousands of still-missing foreground bricks but accepting almost none. Instrument why feedback candidates are not admitted/render-ready before changing visuals or terrain generation.

## Latest Current State - Visible Retention Guard v1 - 2026-05-31

Capability delta:

- The latest patch fixed the specific moving-camera terrain-critical bug where current-frame visible exact bricks were requested/published, then became `Missing` again before render validation.
- This is not the full rendering fix. Public views can still show slow exact recovery and fallback/water ownership while hidden-exact proof remains missing.

Files changed in the latest patch:

- `src/main_launcher.cpp`
  - terrain-critical/current-view requests now touch already-tracked coords as `Visible`.
  - terrain-critical missing brick requests now use high queue priority `250000`.
- `src/Simulation/SparseVoxelWorld.cpp`
  - frame-aware trim/replacement now skips resident bricks marked `Visible` in the current frame.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Moving validation passed:
  - `build/captures/visible_retention_guard_v1_walk_180_460_20260531`
  - command:
    - `.\engine_capture_smoke.ps1 -NoBuild -WalkTest -ExitAfterFrames 520 -CaptureStartFrame 180 -CaptureIntervalFrames 40 -CaptureCount 8 -OutputDir "build\captures\visible_retention_guard_v1_walk_180_460_20260531" -SkipOwnershipDiagnostics`
  - terrain-critical readiness: `samples=340 readyFrame=180 postNonReady=0`
- Static waterline sanity passed:
  - `build/captures/visible_retention_guard_v1_waterline167_20260531`
  - `readyFrame=167 postNonReady=0`

Before/after moving ownership timeline:

- Failed moving baseline:
  - frame 180: surface `60.3913`, mid `1.6964`, farWater `7.5044`, miss `0`
  - frame 240: surface `51.32`, mid `1.484`, farWater `17.7397`, miss `0.0231`
  - frame 300: surface `33.2548`, mid `16.646`, farWater `30.3255`, miss `0.0644`
  - frame 360: surface `42.8887`, mid `6.9469`, farWater `5.0654`, miss `0.0032`
  - frame 420: surface `39.4225`, mid `21.1105`, farWater `6.7193`, miss `0.0021`
  - frame 480: surface `93.1482`, mid `0`, farWater `0`, miss `0`
- After visible retention guard:
  - frame 180: surface `60.7357`, mid `1.4933`, farWater `7.2304`, miss `0.0002`
  - frame 240: surface `51.6731`, mid `1.8166`, farWater `16.8961`, miss `0.018`
  - frame 300: surface `33.6691`, mid `5.7995`, farWater `39.3112`, miss `0.1097`
  - frame 360: surface `39.7768`, mid `8.1444`, farWater `6.5753`, miss `0.0074`
  - frame 420: surface `38.0215`, mid `28.393`, farWater `8.0128`, miss `0.0017`
  - frame 480: surface `90.3074`, mid `0`, farWater `0`, miss `0`

Current blocker:

- Terrain-critical `postMissing` is now zero in the validated moving run, but hidden-exact foreground proof/recovery is still unhealthy.
- In `visible_retention_guard_v1_walk_180_460_20260531\venpod_runtime.log`:
  - frame 240: `startupProofStillMissing=185`, recovery `185/0`
  - frame 300: `startupProofStillMissing=977`, `startupProofStateMissing=969`, recovery `969/0`
  - frame 360: `startupProofStillMissing=1858`, `startupProofStateMissing=1853`, recovery `1853/35`
  - frame 420: `startupProofStillMissing=2723`, `startupProofStateMissing=2715`, recovery `2715/8`
  - frames 515-519 have many hidden-exact feedback candidates but `accepted=0` or near zero.
- This matches the remaining user-visible issue: coarse fallback/water can still carry pixels while exact foreground recovery lags.

Next smallest mechanism-facing task:

- Instrument hidden-exact feedback candidate return/reject reasons inside `requestHiddenExactCoord`.
- Count at least:
  - max-request-limit skips,
  - startup-proof skips,
  - already-ready skips,
  - submitted-active skips,
  - requestSparseBrick free-page/class/total/rejected/known-empty/buried/unknown failures.
- Rerun the moving capture and patch only the proven admission/readiness break.
- Do not go back to material polish, far-SVO shape, terrain generation, water aesthetics, or gate weakening.

## Latest Interruption Resume State - Moving Public View Still Fails - 2026-05-31

This is the current state after the exact surface range alignment patch:

- Static waterline/public frame improved: the obvious close ring1/cell24 mid-voxel ownership band dropped sharply, and first-public static waterline no longer has the old build-in behavior.
- The goal is not solved. A moving/default public capture still fails terrain-critical readiness and still shows the class the user cares about: new views expose missing exact terrain while fallback owners hide it.
- Latest failed moving capture:
  - `build/captures/surface_ownership_prefetch_default_v1_walk_180_460_20260531`
  - command used:
    - `.\engine_capture_smoke.ps1 -NoBuild -WalkTest -ExitAfterFrames 520 -CaptureStartFrame 180 -CaptureIntervalFrames 40 -CaptureCount 8 -OutputDir "build\captures\surface_ownership_prefetch_default_v1_walk_180_460_20260531" -SkipOwnershipDiagnostics`
- Terrain-critical failures after public render:
  - frame 224: `postMissing=1`, sample `90,28,179:Missing`
  - frame 226: `postMissing=1`, sample `88,27,181:Missing`
  - frame 282: `postMissing=1`, sample `27,20,167:Missing`
  - frame 283: `postMissing=1`, sample `26,20,166:Missing`
  - frame 348: `postMissing=1`, sample `-115,27,96:Missing`
  - frame 349: `postMissing=3`, samples `-114,27,96:Missing;-115,26,96:Missing;-115,27,95:Missing`
  - frame 364: `postMissing=2`, samples `-115,26,97:Missing;-115,26,98:Missing`
  - frame 365: `postMissing=2`, samples `-116,26,97:Missing;-115,27,97:Missing`
  - frame 380: `postMissing=1`, sample `-115,26,99:Missing`
- Layer timeline from that failed moving run shows this is not a single static-waterline issue:
  - frame 180: `surfaceScreenPct=60.3913`, `midVoxelScreenPct=1.6964`, `farWaterScreenPct=7.5044`, `missScreenPct=0`
  - frame 240: `surfaceScreenPct=51.32`, `midVoxelScreenPct=1.484`, `farWaterScreenPct=17.7397`, `missScreenPct=0.0231`
  - frame 300: `surfaceScreenPct=33.2548`, `midVoxelScreenPct=16.646`, `farWaterScreenPct=30.3255`, `missScreenPct=0.0644`
  - frame 360: `surfaceScreenPct=42.8887`, `near/waterContext about 25.8`, `midVoxelScreenPct=6.9469`, `farWaterScreenPct=5.0654`
  - frame 420: `surfaceScreenPct=39.4225`, `midVoxelScreenPct=21.1105`, `farWaterScreenPct=6.7193`
  - frame 480: `surfaceScreenPct=93.1482`, `midVoxelScreenPct=0`, `farWaterScreenPct=0`

Current working hypothesis:

- Static exact-range alignment fixed one handoff error.
- Moving public views still expose exact bricks that become `Missing` after public render.
- The likely mechanism is not material, water aesthetics, or terrain generation. It is request/readiness lifecycle for current-view exact terrain: protected/critical request coverage or re-admission is incomplete as the camera moves, so lower LOD/water can carry pixels while exact render-ready bricks lag behind.
- Next step must inspect the failed moving log around frames 224/226/282/348-380 and answer why those exact coords are still `Missing`: not requested, requested too late, accepted but starved, generation/upload/extraction/publish delayed, evicted, or contract mismatch.
- Do not claim the exact-range patch solved rendering. It only improved the stable static view.

## Latest Patch - Exact Surface Range Alignment - 2026-05-31

Mechanism proven this turn:

- In the stable waterline frame after startup hidden-exact repair, sampled visual failures were mostly:
  - `mid_voxel` stone,
  - ring1 / cell size 24,
  - hit distance about 2.45k-2.55k,
  - side-heavy or unclassified coarse terrain.
- Current exact sparse surface raster/ownership stopped at 2048 while exact terrain prefetch/proof already scanned to 3072.
- That meant the engine had exact foreground truth available in the broader public band, but the renderer still handed visually prominent 2.5k terrain to coarse mid voxel.

Patch:

- `src/main_launcher.cpp`
  - default `VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS` is now aligned to the terrain surface prefetch distance, capped by the surface cull distance, when stable near cull is enabled.
  - This makes the default raster/ownership distance 3072 in the current config instead of 2048.
  - `VENPOD_SPARSE_SURFACE_RASTER_MAX_DISTANCE` already derives from ownership radius, so it also defaults to 3072.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Probe before making the default:
  - `build/captures/exact_surface_3072_probe_waterline150_20260531`
  - mid voxel screen ownership dropped to about 1.09%.
- Default patched run:
  - `build/captures/surface_ownership_prefetch_default_v1_waterline167_20260531`
  - captured public frame: `engine_frame_0167.bmp`
  - terrain-critical readiness passed, ready frame 167.
  - layer screen at frame 180:
    - `surfaceScreenPct=59.9545`
    - `midVoxelScreenPct=1.0761`
    - `farSvoScreenPct=0.1600`
    - `farWaterScreenPct=5.8601`
    - `missScreenPct=0`
    - `heightProxyScreenPct=0`
- Previous startup-gated frame 91/temporal baseline had:
  - `surfaceScreenPct` about 54.75-54.95
  - `midVoxelScreenPct` about 5.69-6.04
- Temporal validation:
  - `build/captures/surface_ownership_prefetch_default_v1_temporal_167_407_20260531`
  - `midVoxelScreenPct` decays from 1.0783 at frame 180 to 0.7875 at frame 420.
  - large temporal change stays effectively zero except one tiny 0.033 sampled event; no visible build-in like the old frame-60-to-900 behavior.
- Shoreline audit after patch:
  - `build/captures/surface_ownership_prefetch_default_v1_shoreline_audit_frame167_20260531`
  - most shoreline samples still exact sand/dirt.
  - a small ambiguous `far_svo_or_sky|mixed` bucket remains and needs a narrower follow-up.
- Skyline hole audit after patch:
  - `build/captures/surface_ownership_prefetch_default_v1_hole_ray_audit_frame167_20260531`
  - 64/64 auto-selected skyline gaps classified as `true_procedural_air` by the current CPU/shader audit mirror.
  - Treat this as evidence that the remaining skyline gaps are not the same renderer missing-exact bug. Verify with current engine-side audit before changing generation.
- Basin/water artifact audit after patch:
  - `build/captures/surface_ownership_prefetch_default_v1_basin_water_audit_frame167_20260531`
  - many sampled gray basin patches are exact generated land above sea level.
  - the audit owner classifier is imperfect for far-water colors, so do not over-read owner names from this CSV without fixing the classifier.

Capability delta:

- The static public waterline view no longer asks ring1/cell24 mid voxel to render most of the visibly prominent 2.5k terrain band.
- The user-visible coarse gray mid band is substantially reduced without material tuning, fake fallback, far-SVO takeover, water masking, or terrain generation changes.

Remaining risks:

- Startup hold is longer for the waterline view because more exact surface must be ready before public render: ready frame moved to about 167 in this validation.
- Remaining skyline holes currently look like generated/procedural air, not hidden exact misses, but this needs current engine-side confirmation before any terrain-generation patch.
- Moving-camera/new-region streaming has not been revalidated after this exact-range default change.
- Shoreline/far-water ambiguous samples remain and need a focused owner/truth audit if they are still visibly bad.

## Latest Validated State - 2026-05-31

This is the current state to continue from after compaction:

- The active goal is still rendering correctness: public gameplay frames must show coherent terrain, not partial exact chunks over coarse fallback/water.
- A real startup/public-opening issue was found:
  - public rendering opened while hidden exact foreground terrain was still uploading/non-drawable,
  - lower LOD/fallback owners hid missing exact terrain,
  - the world visibly assembled itself over time.
- Patch retained in the current root-cause path:
  - `src/main_launcher.cpp` now defaults `VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS` to enabled,
  - startup hidden-exact repair requires zero missing by default,
  - `engine_capture_smoke.ps1` and `rebrun.ps1` no longer force hidden-exact repair blocking off by default,
  - terrain-critical readiness checks ignore held/loading frames and validate only public frames after the hold releases.
- Validated capability delta:
  - before: waterline first public frame opened around frame 61 with thousands of hidden exact bricks still non-drawable and surface ownership grew visibly for hundreds of frames,
  - after: first public frame opens around frame 91 only after startup hidden-exact proof is clean,
  - temporal frame diffs from frame 91 to 391 show near-zero visible pop-in for the static waterline camera.
- Important artifacts:
  - pre-fix baseline: `build/captures/waterline_current_001_901_20260530`
  - first public after gate: `build/captures/hidden_exact_repair_gate_v1_first_public_20260530/engine_frame_0091.bmp`
  - passing first-public gate run: `build/captures/hidden_exact_repair_gate_v1_first_public_pass_20260530`
  - temporal stability run: `build/captures/hidden_exact_repair_gate_v1_temporal_91_391_20260530`
  - debug frames: `hidden_exact_repair_gate_v1_material54_frame91_20260530`, `hidden_exact_repair_gate_v1_owner55_frame91_20260530`, `hidden_exact_repair_gate_v1_face65_frame91_20260530`
  - shoreline audit: `build/captures/hidden_exact_repair_gate_v1_shoreline_audit_frame91_20260530`
- Remaining visible problems are not solved:
  - settled/general waterline views still have visible terrain/skyline/shoreline gaps,
  - mid voxel and far water still own meaningful screen area in the stable frame,
  - the shoreline audit still has a smaller ambiguous/fallback group near water,
  - moving cameras can still expose new regions through slow promotion,
  - performance with strict startup hidden-exact proof is expensive and may not be the final public loading policy.
- Do not continue by claiming the startup gate fixed rendering. It only fixed one verified failure mode: public opening before exact foreground proof was clean.
- Next useful patch must be chosen from current bad pixels in the stable public frame, not old invalid route screenshots.

## Current State Snapshot - 2026-05-30

The renderer is still not fixed.

Fresh waterline/default capture:

- Artifact folder: `build/captures/waterline_current_001_901_20260530`
- Public render is held at startup and first visible capture is frame 61, not frame 1.
- The app opens the world before exact sparse surface is settled:
  - frame 60 hidden exact tracked: `4410`
  - frame 60 hidden exact still missing: `3060`
  - frame 60 sparse `genQueued`: `7063`
  - frame 60 sparse `publishPending`: `3977`
  - frame 60 renderable/ready: `3511`
- Exact surface keeps replacing fallback over time:
  - frame 60 `surfaceOwnedPixels`: `946439`
  - frame 120 `surfaceOwnedPixels`: `1086989`
  - frame 300 `surfaceOwnedPixels`: `1117295`
  - frame 900 `surfaceOwnedPixels`: `1136220`
- Waterline ownership still includes large fallback owners after settling:
  - frame 900 `midVoxel`: about `120772`
  - frame 900 `farSvo`: about `6441`
  - frame 900 `farWater`: about `121495`
  - frame 900 `miss`: `0`

Current interpretation:

- The user-visible failure is not a simple crash or missing SVO load.
- Far SVO and mid coverage can be ready while exact sparse surface is still generating/uploading/publishing.
- Public frames show partial exact chunks over mid/far/water fallback, so the scene appears to assemble slowly.
- Even when the pipeline settles, shoreline/waterline terrain still has inconsistent ownership and visible gaps/bad fallback regions.
- The immediate root-cause path is public LOD promotion and exact-vs-fallback ownership coherence, not material tuning or terrain generation.

Do not claim completion until the same public waterline/general view shows coherent terrain early and remains coherent after settling.

## Latest Root-Cause Update

Current validated mechanism:

- Moving/public frames were not only "slow"; request priority was inverted.
- `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK` was requesting hundreds of visible bricks even when `unsafe=0`.
- Those requests were admitted with `terrainCriticalRequest=true`, so they could consume emergency page capacity before the current-frame terrain-critical screen pass.
- Result before the fix:
  - frame 184: shader feedback requested `244`, terrain critical had `postMissing=13`, `criticalSkip free=13`, `free=12`
  - frame 244: terrain critical had `postMissing=12`, `criticalSkip free=12`, `free=12`
- Patch 1 in `src/main_launcher.cpp`:
  - GPU unsafe feedback now gets terrain-critical admission only when `renderOwnerUnsafeNearMissPixelsLastRetire > 0`.
  - Ordinary feedback still streams, but preserves current-frame terrain-critical reserve.
- Validation after patch 1:
  - frame 184 shader feedback `requested=4` instead of `244`
  - frame 184 terrain critical `postMissing=0`, `criticalSkip free=0`, `free=1707`
  - frame 244 terrain critical `postMissing=0`, `criticalSkip free=0`, `free=4033`
  - residual small `postMissing` remained at other frames.
- Patch 2 in `src/main_launcher.cpp`:
  - terrain-critical protected generation drain re-admits a coord if it became `Missing` after initial planning and before protected generation/upload/surface.
  - This fixes same-frame churn where a critical coord can disappear before the protected drain sees it.
- Validation after patch 2:
  - `build/captures/critical_readmit_v1_walk_180_300_20260530`
  - terrain-critical readiness contract did not fail.
  - Capture script still failed visual `terrainLikePct` at frame 180 because the scripted walk camera is close to a sand wall; this is not proof that rendering is complete.
  - frame 300 still shows distant skyline/shoreline gaps, so do not call the user-visible rendering fixed yet.

Next root-cause target:

- Use the waterline/default view that matches the user screenshots, not the close-wall walk frame, to isolate the remaining distant/shoreline gaps.
- Do not return to material tuning.
- Do not weaken smoke gates.
- Do not claim success from the terrain-critical gate alone.

## Active Goal

Fix VENPOD's user-visible voxel terrain rendering so public gameplay frames show coherent, correct terrain instead of broken mid/far voxel chunks, holes, slow streaming, or wrongly owned shoreline terrain.

This is a rendering correctness goal, not a report-generation or smoke-test goal. Do not call the work done unless the visible world actually looks coherent.

## User-Visible Failure

- Terrain still appears to build slowly in public views.
- Close/shoreline views can show broad water or gaps where terrain should be visible.
- Even after streaming settles, distant/shoreline terrain can still look broken, blocky, or inconsistently owned.
- The user explicitly does not want cosmetic masking, fake fallback re-enabling, or unrelated startup efficiency work.

## Anti-Drift Rules For This Goal

- Before any probe, state the specific rendering hypothesis it tests.
- Do not run repeated captures unless code or the invariant changed.
- Do not tune materials, fog, water aesthetics, skyline thresholds, or terrain generation unless an audit proves that is the current root cause.
- Passing a smoke test is not enough. The visual output must improve.
- Avoid broad reports. Use diagnostics only when they directly choose the next renderer patch.

## Proven Current State

- The old camera-inside-terrain failure was fixed earlier; do not reuse those invalid screenshots as evidence.
- Public exact sparse surface raster distance was separated from wider ownership/feedback radius. That improved close-up exact terrain stability.
- Hidden exact-miss feedback helped exact terrain eventually stream in, but did not solve the remaining user-visible broken rendering.
- A waterline capture after reverting the failed water guard is stable and passes readiness by frame 60:
  - `surfaceScreenPct` about `42.60`
  - `midVoxelScreenPct` about `17.45`
  - `farSvoScreenPct` about `0.15`
  - `farWaterScreenPct` about `7.52`
  - `missScreenPct = 0`
- Owner debug for the same waterline view shows the visibly bad distant band is mostly `mid_voxel`, not far SVO.
- Mid face debug shows this band is side-face heavy. The bad read is resident mid-voxel representation/ownership, not simply missing far SVO.
- Current patch `mid_shell_dda_public_off_v1b` makes the raw mid-voxel shell DDA opt-in for low-altitude public views. The safer mid column/far layers still render the band.
- Waterline frame 60 after this patch:
  - `surfaceScreenPct` about `42.60`
  - `midVoxelScreenPct` about `16.45`
  - `farSvoScreenPct` about `0.35`
  - `farWaterScreenPct` about `7.53`
  - `missScreenPct = 0`
- This is a partial visual improvement: fewer floating/chunky raw mid-shell artifacts, but the remaining far/mid band is still coarse and grey.
- Continued after compaction from the absolute project root `Z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD`. Do not run VENPOD commands from `WorkRepo`; that is a sibling workspace and caused one path miss.
- The current public waterline still opens coherently by frame 60 with no miss/proxy, but it is not visually complete:
  - `surfaceScreenPct` about `42.60`
  - `midVoxelScreenPct` about `16.45`
  - `farSvoScreenPct` about `0.35`
  - `farWaterScreenPct` about `7.53`
  - `missScreenPct = 0`
- A mixed-shoreline mid-voxel generation bug was found in `SparseClipmap.cpp`: coarse sand cells were converted to water when any neighboring footprint dipped below sea level. This can make lower LOD draw water over land while exact pages stream in. The conversion was removed, but the fixed static waterline did not change, proving that view is no longer primarily using that raw mid-voxel shoreline conversion.
- Exact-over-water composition was tested. Allowing exact sparse surfaces to override `FAR_WATER` proved the ordering issue exists, but exact `MAT_WATER` rendered as a noisy sparse surface grid. The retained rule is: exact land may override fallback water, exact water does not.
- Letting far SVO compete with low-altitude mid-column hits did not materially change the fixed waterline. Disabling the mid-column owner made the view worse and introduced a small miss, proving the mid-column path is still required as a public fallback carrier.

## Recent Trap: Failed Water Guard

I tried a dry-terrain-before-water guard in `PS_Raymarch.hlsl` that called `DiagnosticFarTerrainWouldHit` from water fallback paths.

Result:
- Build passed, but capture hit repeated `GPUBuffer::Map failed` and `Capture failed to create readback buffer: 0x887A0005`.
- The patch was unvalidated and unstable.
- It was backed out.

Do not reintroduce that expensive water-path diagnostic unless it is redesigned and proven cheap/stable.

## Recent Trap: Failed Rising Mid-Column v1

I tried extending `RaymarchMidVoxelColumnClipmap` so low-altitude gently rising rays could find height crossings before raw mid-voxel DDA side faces.

Result:
- It compiled and captured after a small compile fix.
- It changed the image, proving this handoff path matters.
- It produced large gray rectangular column/shelf artifacts in the waterline band.
- Correct `-SparseDebugMode 65` showed those gray blocks were the new column path, not a clean terrain surface.
- The patch was backed out.

Do not retry a broad rising-height-column replacement without a stronger surface criterion. A valid follow-up would need to reject steep/side-heavy crossings or route the band to a better LOD owner.

## Recent Trap: Mid-Column Disabled A/B

I disabled the low-altitude mid-column owner to see whether complete far SVO could carry the waterline/general view.

Result:
- Capture: `build/captures/mid_column_disabled_ab_waterline60_20260529/engine_frame_0060.bmp`
- `midVoxelScreenPct` increased from about `16.45` to `17.91`.
- `farWaterScreenPct` dropped from about `7.53` to `6.97`.
- `missScreenPct` rose from `0` to `0.0024`.
- Visually, shoreline/fallback coherence got worse.

The A/B was reverted. Do not disable the mid-column path as a broad fix.

## Recent Finding: Water vs Exact Surface Ordering

`ResolveExactSparseSurfaceBeforeBackground()` previously only ran for `MID_VOXEL` and `FAR_SVO`, not `FAR_WATER`. That means fallback water could hide ready exact land/surface records.

Current retained shader behavior:
- `FAR_WATER` now participates in exact-before-background resolution and hidden exact feedback.
- If the exact candidate material is `MAT_WATER`, it does not override smooth fallback water. This avoids noisy sparse-surface water grids.
- If exact land exists before fallback water, it can now win.

Artifacts:
- Bad broad exact-over-water test: `build/captures/exact_over_water_v1_waterline60_20260529/engine_frame_0060.bmp`
- Retained exact-land-only rule: `build/captures/exact_land_over_water_v1_waterline60_20260529/engine_frame_0060.bmp`
- Far-SVO competition test: `build/captures/far_svo_competes_mid_column_v1_waterline60_20260529/engine_frame_0060.bmp`

## Current Best Diagnosis

The current waterline/general-view failure is a renderer handoff/representation issue:

- Low-altitude views use exact sparse surface in the foreground.
- The distant visible terrain band is mostly `mid_voxel`.
- `RaymarchMidVoxelColumnClipmap` is the safe public carrier for many low-altitude pixels. Turning it off makes the image worse.
- Raw 3D mid-voxel shell DDA is still bad as a public fallback and remains opt-in.
- The distant band is still too dependent on the scalar mid-column/closure representation while exact sparse terrain slowly promotes. Far SVO is complete but does not currently replace this band under conservative distance ordering.
- Water can still be a symptom when exact land is not ready, but the retained exact-land-over-water rule prevents ready exact land from being hidden by fallback water.

A naive rising-column replacement made blocky shelves. The next fix should target actual owner selection:
- improve the mid-column/closure representation so it is a valid coherent LOD,
- or make exact foreground promotion happen as coherent regions rather than partial chunks over water,
- or add a narrow owner rule where far SVO replaces mid-column only when an audit proves it is visually closer/better for a given ray class.

Current patch chose the first narrow step for public low-altitude views: raw mid shell DDA is no longer default unless `VENPOD_SPARSE_MID_VOXEL_WALK_DDA=1` or debug mode 65/67 is active.

## Important Artifacts

- Stable current waterline after water guard revert:
  - `build/captures/waterline60_after_water_guard_revert_20260529/engine_frame_0060.bmp`
  - `build/captures/waterline60_after_water_guard_revert_20260529/layer_screen_timeline.csv`
  - `build/captures/waterline60_after_water_guard_revert_20260529/ownership_timeline.csv`
- Owner debug showing distant bad band is mostly mid voxel:
  - `build/captures/far_svo_parity_waterline_owner55_20260529/engine_frame_0060.bmp`
- Mid face debug showing side-face-heavy mid terrain:
  - `build/captures/waterline_mid_face_debug65_20260529/engine_frame_0060.bmp`
- Mid ring debug:
  - `build/captures/waterline_mid_ring_debug67_20260529/engine_frame_0060.bmp`
- Current patch normal validation:
  - `build/captures/mid_shell_dda_public_off_v1b_waterline60_20260529/engine_frame_0060.bmp`
  - `build/captures/mid_shell_dda_public_off_v1b_waterline60_20260529/layer_screen_timeline.csv`
- Current patch owner debug:
  - `build/captures/mid_shell_dda_public_off_v1b_owner55_waterline60_20260529/engine_frame_0060.bmp`

## Files Currently In The Root-Cause Path

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - `RaymarchBackgroundField`
  - `RaymarchMidVoxelClipmap`
  - `RaymarchMidVoxelColumnClipmap`
  - `TryBuildResidentMidVoxelClosureHit`
- `assets/shaders/Graphics/PS_SparseSurface.hlsl`
  - exact surface debug/readability paths
- `src/main_launcher.cpp`
  - public exact surface distance and hidden exact feedback settings
- `src/Graphics/Renderer.cpp`
  - frame constants / shader params

## Next Mechanism-Facing Step

Continue from the low-altitude public fallback contract:

1. Do not use the failed broad rising-column v1.
2. Keep raw mid shell DDA out of public low-altitude views unless a later audit proves it is needed.
3. Do not disable mid-column broadly; it is currently necessary.
4. Next likely issue is partial exact promotion over fallback:
   - exact sparse surfaces appear chunk-by-chunk while mid-column/water remains behind them,
   - shader unsafe feedback still reports many requested/non-ready exact bricks in moving views,
   - user-visible result is terrain “building in” rather than coherent LOD promotion.
5. The next useful patch should make promotion coherent or make the fallback truth match exact terrain better. Candidate directions:
   - exact surface regional promotion/hysteresis: draw exact chunks only when adjacent/screen-relevant surface coverage is coherent, otherwise let mid-column carry the region,
   - better hidden-exact feedback for fallback water/mid regions that are inside the public exact band,
   - mid-column shoreline truth parity if an audit proves it draws water where CPU exact terrain says land.
6. The fix must preserve ownership semantics:
   - no fake height proxy,
   - no water masking,
   - no broad far-SVO takeover,
   - no terrain generation change.
7. Validate against the same waterline frame and a valid moving camera:
   - image looks less broken,
   - `missScreenPct` remains `0`,
   - `farWaterScreenPct` does not increase as a mask,
   - exact promotion does not create water/sand pop-in,
   - moving view must have `cameraInside=0` and `feetInside=0`; discard underwater/waterline stress captures with `cameraMat=2`.

## Commands To Reuse

Build:

```powershell
.\build.ps1 -Config Release
```

Waterline validation:

```powershell
.\engine_capture_smoke.ps1 -NoBuild -WaterlineCamera -ExitAfterFrames 125 -CaptureStartFrame 60 -CaptureIntervalFrames 1 -CaptureCount 1 -OutputDir "build\captures\<name>" -SkipOwnershipDiagnostics
```

Owner debug:

```powershell
.\engine_capture_smoke.ps1 -NoBuild -WaterlineCamera -SparseDebugMode 55 -ExitAfterFrames 125 -CaptureStartFrame 60 -CaptureIntervalFrames 1 -CaptureCount 1 -OutputDir "build\captures\<name>" -SkipOwnershipDiagnostics
```

Mid face debug:

```powershell
.\engine_capture_smoke.ps1 -NoBuild -WaterlineCamera -SparseDebugMode 65 -ExitAfterFrames 125 -CaptureStartFrame 60 -CaptureIntervalFrames 1 -CaptureCount 1 -OutputDir "build\captures\<name>" -SkipOwnershipDiagnostics
```

## Definition Of Progress

A real capability delta here means the same public waterline/general gameplay view renders coherent terrain earlier or more consistently, not merely that a CSV, gate, or cache changed.

## Current Resume State - 2026-05-31

Active goal:

Fix VENPOD's user-visible voxel terrain rendering so public gameplay frames show coherent, correct terrain instead of broken mid/far voxel chunks, holes, slow streaming, or wrongly owned shoreline terrain.

Do not mark this complete. The current screenshots still show slow streaming, partial terrain promotion, holes/gaps, and bad shoreline/waterline ownership.

Corrected latest diagnosis:

- The fullscreen raymarch/depth-stencil overwrite theory is mostly disproven for the current walk frame 300 probe.
- Render order is:
  - exact sparse surface draws first and writes stencil 1,
  - fullscreen raymarch draws only where stencil equals 0.
- Owner debug frame 300 showed blue/green fallback regions are not overwriting yellow exact surfaces. They are pixels where exact surface never owned the pixel.
- Current lead is therefore exact sparse surface noncoverage / incoherent promotion:
  - exact terrain becomes visible one brick/patch at a time,
  - mid/far/water fallback legitimately fills stencil-zero gaps,
  - the result is patchy terrain "building in" rather than a coherent LOD transition.

Latest concrete mechanism found:

- `VENPOD_SPARSE_SURFACE_NEIGHBOR_COVERAGE` is meant to avoid isolated exact bricks drawing as detached panels.
- In `src/main_launcher.cpp`, CPU surface snapshot visibility/neighborhood filtering was bypassed whenever GPU surface culling was active.
- The dirty-payload fast path also bypassed neighbor coverage.
- That matches the visible failure mode: individual exact bricks promote over mid/far/water fallback instead of promoting as coherent surface regions.

Patch in progress:

- `src/main_launcher.cpp` now has a v1b attempt:
  - neighbor coherence prefilter only becomes active after public render is no longer startup-held,
  - dirty-payload fast path is disabled only while post-startup neighbor coherence is active,
  - startup no longer stalls like the first v1 attempt did.
- Build succeeded after v1b.
- Single-frame walk frame 300 capture passed:
  - `build/captures/surface_neighbor_prefilter_v1b_walk300_normal_20260531/engine_frame_0300.bmp`
  - `build/captures/surface_neighbor_prefilter_v1b_walk300_normal_20260531/venpod_runtime.log`

Immediate next step:

1. Inspect `surface_neighbor_prefilter_v1b_walk300_normal_20260531/engine_frame_0300.bmp`.
2. Compare against baseline `startup_proof_retire_v1_walk_180_500_20260531/engine_frame_0300.bmp`.
3. Parse v1b layer/ownership/composition timelines.
4. If v1b improves coherence without unacceptable cost, run owner debug 55 and a longer walk capture.
5. If v1b hurts perf or leaves patchwork unchanged, the next fix should be more explicit regional promotion state, not more material tuning or terrain generation.

Current anti-drift rule:

Only count work as progress if public gameplay frames become visibly more coherent or a specific mechanism preventing that is proven and patched. Do not drift into material color, skyline closure, broad startup gates, or generic reports unless directly tied to this exact rendering failure.

## State Check - 2026-05-31

User asked for current state after compaction/interruption.

Active goal remains open: fix user-visible voxel terrain rendering so public gameplay frames are coherent instead of showing broken mid/far chunks, holes, slow streaming, or wrongly owned shoreline terrain.

Important correction:

- The current `surface_neighbor_prefilter_v1b` experiment is not proven good.
- Visual check of `build/captures/surface_neighbor_prefilter_v1b_walk300_normal_20260531/engine_frame_0300.bmp` against `build/captures/startup_proof_retire_v1_walk_180_500_20260531/engine_frame_0300.bmp` shows v1b creates large missing/panel-like terrain regions and should not be treated as a solved rendering fix.
- The root issue is still public-frame coherence: exact surface, mid voxel, far SVO, and water fallback are not promoting/handing off as one coherent terrain representation.

What is actually proven:

- Earlier admission/priority patches improved some streaming starvation cases:
  - high-altitude broad hidden-exact repair no longer floods surface records in the stress frame 484 case,
  - shader unsafe feedback no longer starves terrain-critical exact requests when `unsafe=0`,
  - selected smoke/stress gates can pass.
- Passing those gates does not prove the world looks right. The screenshots and v1b visual check still show broken public rendering.

Current best next step:

1. Do not continue treating neighbor prefilter v1b as success.
2. Inspect why exact surface noncoverage remains in public pixels without simply hiding exact chunks.
3. Compare owner/depth/terrain truth for v1b's newly missing panel regions versus the baseline:
   - was exact surface intentionally suppressed by neighbor coverage,
   - did mid/far/water fail to provide a coherent fallback,
   - did stencil/depth ordering block fallback after exact was suppressed,
   - is the fallback terrain/water truth inconsistent with exact terrain.
4. The next patch should make LOD promotion coherent, not just suppress exact bricks.

Capability state:

- Not solved.
- Startup/streaming is somewhat better in selected scenarios, but public rendering remains visibly broken.
- The next successful change must make the same gameplay view look more coherent, not merely pass telemetry.

## Current State - 2026-05-31 Protected Raster Cap Candidate

Read this before continuing after compaction.

Goal is active and not complete: fix user-visible voxel terrain rendering so gameplay frames show coherent terrain instead of patchy exact/mid/far/water ownership.

Do not revive `surface_neighbor_prefilter_v1b` as the fix. It was visually worse and current source no longer contains the claimed v1b neighbor prefilter behavior.

Latest concrete mechanism:

- Current source waterline frame 181 showed exact sparse surface owning too much of the distant mountain band.
- Owner debug mode 55 showed lots of magenta exact surface beyond the protected foreground band.
- Metrics before cap were roughly:
  - `surfaceRasterMax=3072`
  - `surfaceScreenPct=59.94`
  - `midVoxelScreenPct=1.08`
  - `farSvoScreenPct=0.16`
  - `farWaterScreenPct=5.86`
  - `missScreenPct=0`
- Env experiment `VENPOD_SPARSE_SURFACE_RASTER_MAX_DISTANCE=1792` reduced far exact sparse leakage:
  - `surfaceScreenPct=48.87`
  - `midVoxelScreenPct=11.90`
  - `farSvoScreenPct=0.35`
  - `farWaterScreenPct=5.87`
  - `missScreenPct=0`
  - waterline and walk captures still passed, and distant terrain looked more coherent.

Patch currently in `src/main_launcher.cpp`:

- Default exact surface raster max is now the protected foreground band:
  - `max(exactNear + 768, 1536)`, clamped by ownership/cull radius.
- Rationale: exact sparse raster should cover close playable terrain and shoreline/side-face fixes, but public distant continuity should come from mid/far LOD until exact terrain is truly ready.

Important: this source patch has not yet been built or validated after being made default. Immediate next action:

1. Build Release.
2. Capture waterline frame 181 normal and owner debug 55 with no env override.
3. Capture walk frame 300 with no env override.
4. Confirm logs now report `surfaceRasterMax=1792` and match the env experiment.
5. If confirmed, continue with longer public-view validation. Do not mark solved yet.

## Current State - 2026-05-31 2560 Raster Cap Default

The 1792 protected raster cap was validated but was too aggressive. It fixed far exact-surface leakage, but the remaining bad-pixel audit showed visible public terrain shifted to mid ring1/cell24:

- `visual_bad_pixel_audit` on default frame 300 after the 1792 cap:
  - 78 sampled bad pixels owned by `mid_voxel`
  - 44 exact shoreline/foreground samples
  - 5 far SVO samples
- `midfar_geometry_audit`:
  - 78/83 mid/far samples were `mid_voxel_cell_size_or_lod`
  - 42 samples were `mid_voxel, cellSize=24, side_face`
  - median-ish hit distances were around 2.5km
- Mechanism:
  - shader `VENPOD_SPARSE_MID_PUBLIC_FINE_RING_END=3400` makes the public shader prefer fine mid ring longer,
  - CPU clipmap policy still builds normal rings over `MID_END=6400`, so ring0 ends near 2368,
  - at ~2.5km the shader cannot find ring0 and silently falls back to ring1/cell24 column/voxel representation.

Dead end tested:

- Env `VENPOD_SPARSE_MID_END=10528` aligned CPU and shader ring math, but it visually worsened the frame and introduced nonzero miss:
  - `missScreenPct` reached ~0.14 by frame 300,
  - far handoff moved too late (`farHandoff=6916`),
  - frame showed noisy/pale panels.
- Do not make `MID_END=10528` the fix.

Patch now in source:

- Default exact sparse surface raster cap is `max(exactNear + 1536, 2560)`, clamped by ownership/cull radius.
- Rationale: keep exact surface through the 2.5km visible transition band where mid ring1/cell24 looked broken, but still stay below the old 3072 cap that overexposed far exact panels.

Validated after source patch/build:

- Build Release succeeded.
- Default `rebrun` frame 300:
  - artifact: `build/captures/raster_default_2560_rebrun300_normal_20260531/engine_frame_0300.bmp`
  - `surfaceRasterMax=2560`
  - `surfaceScreenPct=58.625`
  - `midVoxelScreenPct=2.3632`
  - `farSvoScreenPct=0.195`
  - `farWaterScreenPct=5.8539`
  - `missScreenPct=0`
  - `voxelTerrainScreenPct=61.1832`
- Compared with the 1792 default:
  - mid voxel in the problem public view dropped from ~11.6% to ~2.4%
  - miss stayed 0
  - far water stayed stable
- Walk frame 300 also passed:
  - artifact: `build/captures/raster_default_2560_walk300_normal_20260531/engine_frame_0300.bmp`
  - `missScreenPct=0`
  - `midVoxelScreenPct=2.1907`
  - `surfaceScreenPct=63.721`

This is a real rendering ownership improvement, but still not final completion. The world can still have composition/shape artifacts and early public render/startup behavior must be rechecked from a user-visible run. Next mechanism if continuing:

1. Capture a longer early/public run with the 2560 default and determine when the first coherent frame is actually shown.
2. If startup still shows broken terrain instead of holding/loading, inspect `sparseStartupPublicRenderHeldThisFrame` and manual `rebrun` env differences.
3. If the first public frame is coherent but later motion exposes gaps, run owner/bad-pixel audits on the new failing camera, not the old 1792 artifacts.

## Current State - 2026-05-31 Stable Exact Foreground v1

Read this before continuing after compaction.

Goal is active and not complete. The user-visible target is coherent gameplay terrain, not green smoke logs.

Latest mechanism proven:

- The 2560 exact raster cap improved one static frame, but dynamic exact-surface promotion caused visible slow build-in.
- In a long static public capture, the same view changed substantially as exact sparse panels took over distant mid/far terrain:
  - old dynamic promotion frame 61 -> 481 changed about 13.01% of pixels,
  - owner debug showed distant mountains turning from mostly mid voxel into magenta far exact sparse surface.
- Forcing a stable 1792 foreground exact cap made the same static view temporally stable:
  - frame 61 -> 481 changed about 1.009% in the env experiment,
  - after the source patch, frame 61 -> 481 changed about 0.575%.

Patch now in `src/main_launcher.cpp`:

- Broad exact-surface raster promotion is opt-in:
  - default `VENPOD_SPARSE_SURFACE_RASTER_DYNAMIC_PROMOTION=0`,
  - default exact surface public cap is the stable foreground band, about 1792 with current exactNear,
  - broad 2560 promoted cap can still be enabled explicitly for experiments.
- Shader unsafe foreground repair max now follows the active exact contract:
  - foreground default repair max is the 1792 foreground band,
  - broad promoted repair is used only when dynamic promotion is enabled.
- Shader unsafe feedback no longer queues non-contract exact bricks unless the sample is render-critical.

Validated after patch:

- Build Release succeeded.
- Static public normal capture:
  - artifact: `build/captures/stable_exact_foreground_v1_normal_20260531`
  - `surfaceRasterMax=1792`
  - `surfacePromoted=0`
  - miss 0
  - far/mid coverage 1.00/1.00
  - frame 61 -> 481 changed about 0.575%.
- Owner debug:
  - artifact: `build/captures/stable_exact_foreground_v1_owner55_20260531`
  - no broad magenta exact takeover in distant mountains.
- Waterline capture:
  - artifact: `build/captures/stable_exact_foreground_v1_waterline_20260531`
  - frame 181 -> 301 changed about 0.499%
  - miss 0.
- Full smoke capture:
  - artifact: `build/captures/stable_exact_foreground_v1b_smoke_20260531`
  - terrain-critical ready by frame 180,
  - miss 0,
  - `surfaceRasterMax=1792`,
  - `surfacePromoted=0`.

Capability delta:

- The renderer no longer replaces stable mid/far background terrain with distant exact sparse panels over time by default.
- This directly reduces the user-visible "terrain slowly builds itself in" failure for static public views.

Not solved:

- Mid/far representation is now carrying more public background terrain, so any remaining bad distant chunks must be diagnosed as mid/far representation or owner-contract problems on the current patched source, not old 2560 artifacts.
- Shader unsafe telemetry can still report many `nonReady` samples outside the active exact contract. Requests are filtered, but diagnostics should be split into in-contract vs out-of-contract counts before using that number as a health gate.
- First shown public frame and moving/free-camera views still need current-source validation. Do not claim the rendering is fixed until a current failing user view is captured and explained.

Next best step:

1. If the user asks for status, say the main current fix is stable foreground exact ownership, not final rendering completion.
2. If continuing work, first add/split shader unsafe in-contract telemetry so logs stop mixing non-contract exact samples with actual unsafe render requirements.
3. Then capture the user's current broken camera/view with normal, owner debug, material debug, and bad-pixel audit on the patched source.
4. Patch only the owner/LOD mechanism proven by that current capture.

## Current State - 2026-05-31 Startup Warmup Interruption

Read this section first after compaction.

Goal is active and not complete:

- Fix user-visible voxel terrain rendering so public gameplay frames show coherent terrain instead of broken mid/far chunks, holes, slow streaming, or wrong shoreline ownership.
- Anti-drift anchor: logs and smoke gates are only useful if they move the actual runtime view toward coherent terrain.

What is currently proven:

- Stable foreground exact ownership reduced the "distant exact panels slowly build in" problem for static views.
- Shader unsafe telemetry was split into contract vs out-of-contract non-ready samples.
- Broad shader fallback feedback was reduced to near-only feedback so it no longer floods requests with distant/out-of-contract fallback bricks.
- Early public frames still had real in-contract shader unsafe samples before the user should see the world.

Latest attempted mechanism:

- `src/main_launcher.cpp` added a startup public render gate for shader-unsafe contract readiness.
- It also added a private render warmup mode intended to render the world behind an opaque loading screen so ownership feedback can warm exact terrain before public display.

Current bug / interruption point:

- The private warmup run stalled/fails because ownership stats are still disabled while `sparseStartupPublicRenderHeld` is true.
- Verified code location: `src/main_launcher.cpp` around `cameraParams.renderOwnershipStatsEnabled`.
- Current expression still starts with `!sparseStartupPublicRenderHeld`, so private warmup renders but does not collect ownership feedback.
- Result: the startup gate waits for shader unsafe feedback that private warmup cannot produce.

Immediate next patch:

- Change `cameraParams.renderOwnershipStatsEnabled` so it is enabled during `sparseStartupPrivateRenderWarmupThisFrame` as well as public render.
- Rebuild Release.
- Rerun startup private warmup capture and verify:
  - held log shows `privateRenderWarmup=1`,
  - backend active mask/report shows ownership stats enabled during held warmup,
  - shader unsafe feedback arrives while public render is held,
  - public render opens only after in-contract unsafe samples are clean,
  - first visible terrain frame is coherent rather than broken/still-streaming.

Do not drift into:

- material tuning,
- water/fog cosmetics,
- route composition,
- broad terrain generation,
- weakening smoke gates.

If startup still stalls after enabling ownership during private warmup, inspect whether ownership readback is queued/retired while held:

- `QueueRenderOwnershipReadback`
- `cameraParams.farFieldGridParams.w`
- render ownership interval gating
- readback retire path

## Current State - 2026-05-31 Private Warmup + Terrain Critical Publish

Read this section first after compaction.

Goal is active and not complete. The user-visible target is coherent terrain in public gameplay frames; do not redefine success around a passing capture.

Patches now in `src/main_launcher.cpp`:

1. Private startup warmup can collect ownership feedback.

- Previous bug: private warmup rendered while public display was held, but `cameraParams.renderOwnershipStatsEnabled` still required `!sparseStartupPublicRenderHeld`.
- Result: the startup shader-unsafe gate waited for feedback the warmup could not produce.
- Patch: ownership stats are enabled when `sparseStartupPrivateRenderWarmupThisFrame` is true.

2. Terrain-critical page-table publish no longer starves on the normal 48 surface-ready gate budget.

- Stress/free-camera failure showed:
  - `PERF_SPARSE_TERRAIN_CRITICAL_NONREADY frame=248 phase=post count=1 samples=15,-1,8:UploadingGPU`
  - `PERF_SPARSE frame=248 ... publishSurfGate=1/48`
- Mechanism: a visible terrain-critical brick had been generated/uploaded, but the page-table publish gate exhausted its 48 surface extraction budget before making that brick GPU drawable, so a public moving frame could still have a hidden/late exact terrain hole.
- Patch: when terrain-critical publish overtime is active, `sparsePageTableSurfaceReadyGateExtractBudgetThisFrame` is boosted to at least `sparseTerrainScreenCriticalProtectedSurfaceBudget`.

Validation after patches:

- Build:
  - `.\build.ps1 -Config Release`
  - passed with only existing `rayDir` C4456 warnings.

- Default startup/public render:
  - artifact: `build/captures/startup_private_warmup_v2_normal_smoke_500_20260531`
  - command used longer exit frame because public render is intentionally held during warmup:
    `.\engine_capture_smoke.ps1 -NoBuild -ExitAfterFrames 500 -CaptureStartFrame 120 -CaptureIntervalFrames 20 -CaptureCount 13 -MinUniqueSampleColors 1`
  - passed.
  - first captured public frame: `engine_frame_0160.bmp`.
  - terrain-critical readiness: `readyFrame=141 postNonReady=0`.
  - visual contact sheet is stable; temporal deltas after first public frame are tiny.

- Post-open static default:
  - artifact: `build/captures/startup_private_warmup_v2_public_frames_20260531`
  - passed.
  - frame 201 -> 241 changed `0.011%`.
  - later static deltas were ~0 to `0.022%`.

- Waterline:
  - artifact: `build/captures/startup_private_warmup_v2_waterline_20260531`
  - passed.
  - frame-to-frame temporal change after public open was 0 in sampled frames.

- Stress/free-camera before surface-gate patch:
  - artifact: `build/captures/startup_private_warmup_v2_stress_20260531`
  - failed terrain-critical readiness at frame 248 due one `UploadingGPU` brick.
  - later sampled stress view had miss/sky spikes, e.g. frame 300 `missScreenPct=0.4038`, `skyScreenPct=4.0779`.

- Stress/free-camera after surface-gate patch:
  - artifact: `build/captures/terraincritical_surface_gate_v1_stress_20260531`
  - passed: `readyFrame=201 postNonReady=0`.
  - frame 300 improved to `missScreenPct=0.0705`, `skyScreenPct=0.0115`.
  - frame 315 improved to `missScreenPct=0.0001`.

Capability delta:

- The renderer now performs private ownership-feedback warmup before public display, so the first public default/waterline frames are no longer the obviously broken partially-streamed frames.
- Runtime moving/free-camera terrain-critical publish no longer loses visible exact terrain solely because a generic surface-ready publish budget was exhausted.

Not solved:

- Stress/free-camera views still rely heavily on far SVO and far water when high above/over water:
  - after patch around frame 300, owner mix is still roughly farSVO ~37.7%, farWater ~43.4%, exact surface ~7.3%, mid ~11.2%.
- This may be legitimate high-altitude LOD/water, but it still reads visually patchy in broad overhead views. Do not call the goal complete.
- Next real mechanism to inspect if continuing:
  - Are broad far-water pixels correctly water according to deterministic terrain truth, or are they hiding terrain that far SVO/mid should own?
  - Use current patched stress frame 300/315, not old artifacts.
  - Produce far-water-vs-terrain-truth audit for water-owned pixels, not another generic noncoverage audit.
  - If far water is hiding terrain, patch far water arbitration/terrain truth. If CPU/shader truth says water/air, this is terrain generation/composition, not renderer ownership.

Avoid next:

- More stone/material polish.
- Skyline closure.
- Exact surface range widening by default.
- Lowering gates.
- Treating high-altitude stress water composition as proof of renderer failure without CPU/shader terrain truth.

## Current State - 2026-05-31 High-Altitude Repair / Water Ownership

Read this section first after compaction. The goal is still active and not complete.

User-visible issue remains:

- Startup/public render is improved, but broad gameplay/free-camera views can still show slow terrain fill-in, water/shoreline gaps, and incoherent mid/far/exact ownership.
- Do not claim a smoke pass means the renderer is fixed.

Latest concrete evidence:

- A fixed owner-debug decoder showed many dark-blue mode-55 pixels are actually water, not far SVO.
- Audit artifact:
  - `build/captures/terraincritical_surface_gate_v1_farwater_truth_audit_v2_20260531`
- Key audit result:
  - 384 sampled artifact pixels.
  - Owners: water 157, exact_sparse_surface_extended 109, mid_voxel 76, far_svo 42.
  - 112 water-owned pixels had deterministic terrain before the water plane by more than 2 units.
  - 98 water-owned pixels had deterministic terrain before the water plane by more than 8 units.
- Interpretation:
  - The renderer/streaming system is still allowing water or lower LOD to hide dry terrain that should become exact/far terrain first.
  - This is a real rendering/ownership-streaming defect, not just material or terrain composition.

Attempted but reverted:

- A shader-side `PS_Raymarch.hlsl` terrain-before-water guard was tried.
- It compiled but caused the raymarch/runtime path to stall badly.
- Do not resurrect a heavy shader loop in `PS_Raymarch.hlsl` for this. Prefer CPU/request-side repair or a bounded shader-side invariant only if proven cheap.

Current C++ patch direction:

- `src/main_launcher.cpp` high-alt hidden-exact feedback was previously throttled to zero after public open:
  - logs showed `opened=1 ownershipPressure=1 highAltThrottle=1 probe=0 maxRequests=0`.
- Patch v1:
  - default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_HIGH_ALTITUDE_MAX_REQUESTS` changed from 0 to a nonzero request budget.
  - high-alt post-open upload/surface catchup no longer zeroes itself.
- Patch v2:
  - post-open hidden-exact probe time default increased from 6 ms to 25 ms.
  - high-alt max requests default increased to 256.

Validation so far:

- Build passed after the C++ patches.
- v1 artifact:
  - `build/captures/highalt_hidden_exact_repair_v1_stress_20260531`
  - Smoke passed with `readyFrame=201 postNonReady=0`.
  - Hidden-exact repair became active post-open instead of staying at zero requests.
- v1 water audit:
  - `build/captures/highalt_hidden_exact_repair_v1_farwater_truth_audit_20260531`
  - Owners: exact_sparse_surface_extended 172, water 147, far_svo 65.
  - Water-owned terrain-before-water cases still remained: 91 >2 units, 72 >8 units.
- v2 artifact:
  - `build/captures/highalt_hidden_exact_repair_v2_stress_20260531`
  - Smoke passed with `readyFrame=201 postNonReady=0`.
  - Repair visited many more rays and accepted more feedback, but broad view still may be visually wrong.

Important open validation:

- v2 owner/material captures and the v2 far-water truth audit were not completed before interruption.
- Immediate next commands should capture:
  - `highalt_hidden_exact_repair_v2_stress_owner55_20260531`
  - `highalt_hidden_exact_repair_v2_stress_material54_20260531`
  - `highalt_hidden_exact_repair_v2_farwater_truth_audit_20260531`
- Direct question:
  - Did v2 materially reduce water-owned dry-terrain-before-water pixels, or is hidden-exact repair still missing those regions?

Likely next decisions:

- If v2 reduces water-over-dry-terrain materially, continue request-side repair/prioritization until the view converges in bounded time.
- If v2 does not reduce it, inspect the hidden-exact CPU scanner and request selection:
  - It may be missing dry terrain before water.
  - It may be sampling too sparsely.
  - It may be prioritizing the wrong candidate.
  - It may be treating water-plane proximity as sufficient terrain ownership.
- If exact surface exists but water still owns, inspect pass/depth/order for water drawing over terrain.

Do not drift into:

- Stone/material tuning.
- Skyline closure.
- Terrain generation shape.
- Route composition.
- Startup-only gate polishing unless it directly fixes the user-visible broken render.

## Current State - 2026-05-31 Exact Surface Exists But Water Still Owns

Read this before doing more startup or request-budget work. The goal is still active and not complete.

The latest validated mechanism is not "missing terrain everywhere." For sampled bad water/shoreline pixels, much of the exact terrain is already resident and drawable, but the final rendered owner is still water.

Important artifacts:

- `build/captures/highalt_hidden_exact_repair_v2_frame350_farwater_truth_audit_20260531`
- `build/captures/water_hidden_exact_lifecycle_frame350_20260531/exact_request_lifecycle_audit.csv`
- `build/captures/water_hidden_exact_lifecycle_frame350_20260531/exact_request_lifecycle_summary.csv`
- `build/captures/water_exact_surface_noncoverage_frame350_20260531/exact_surface_noncoverage_audit.csv`

Key evidence:

- Frame 350 water-owned dry-terrain audit:
  - 127 sampled water pixels.
  - 46 water pixels had deterministic terrain before water by more than 2 units.
  - 42 water pixels had deterministic terrain before water by more than 8 units.
- Lifecycle audit for those 46 rows:
  - 46/46 bricks were generated, uploaded, and resident.
  - 46/46 had surface extraction completed.
  - 45/46 had GPU surface records by frame 300.
  - 45/46 were in the visible request set.
  - Mid/far hidden miss was 0 for this sample.
- Exact surface noncoverage/intersection audit for the same rows:
  - 24/46 were classified `exact_exists_but_mid_won_depth_or_order`.
  - 43/46 brick resident.
  - 43/46 surface known.
  - 42/46 GPU surface record present.
  - 42/46 GPU draw slot present.
  - 25/46 had nearest exact surface candidate found before the water hit.
  - 0/46 were outside surface raster distance.
  - 0/46 were backface rejected.

Interpretation:

- Hidden-exact request repair helped some missing-brick cases, but it did not solve the current water/shoreline rendering defect.
- For many bad pixels, exact sparse surface data exists and should geometrically beat water, but the final image still shows water.
- The next likely root cause is exact-surface raster/pass/depth/order integration versus the fullscreen raymarch/water pass.

Do next:

- Inspect render pass order and depth setup for sparse exact surface versus fullscreen raymarch/water.
- Inspect `assets/shaders/Graphics/VS_SparseSurface.hlsl` depth output/projection math.
- Inspect sparse surface PSO depth state and whether raymarch overwrites exact surface color/depth.
- Patch the smallest proven pass/depth/order issue, then rerun the same frame-350 water-owned dry-terrain audit.

Do not do next:

- Do not widen terrain generation, tune materials, tune skyline closure, or call this a startup-only problem.
- Do not reapply the reverted heavy `PS_Raymarch.hlsl` pre-water probe. It did not improve the audit and made shader iteration worse.
- Do not spend another loop only raising request budgets unless the exact surface is proven missing again.

## Current State - 2026-05-31 Capture/Audit Alignment Fixed

Read this before trusting any older water/shoreline audit.

Important correction:

- Older frame-350 water/shoreline audits often used the nearest `PERF_CAMERA_EXPOSURE` row at frame 360.
- The stress camera used to advance only after the public-render gate opened, so shader/startup changes shifted the stress-camera time at a given frame.
- Result: some CSV rows were precise but wrong, because the image, owner debug, and CPU ray audit were not using the same camera.
- This explains several contradictory conclusions, especially broad claims that water was hiding many exact surfaces.

Code changes now in place:

- `src/main_launcher.cpp`
  - Added `VENPOD_SPARSE_STRESS_CAMERA_ABSOLUTE_FRAME`.
  - `engine_capture_smoke.ps1` enables it for stress/skyline captures.
  - Stress-camera diagnostic captures now use frame-based time, not gate-open-relative time.
  - Runtime now logs `PERF_CAMERA_EXPOSURE` on exact backbuffer capture frames, not only every 60 frames.
- `basin_water_artifact_audit.ps1`
  - Now requires an exact camera exposure row for the requested frame.
  - It fails loudly instead of silently using the nearest camera frame.

Validation artifacts:

- Camera stability probes:
  - `build/captures/stress_absframe_probe_a_20260531`
  - `build/captures/stress_absframe_probe_b_20260531`
  - Same frame 360 camera after the fix:
    - `cam=(-315.32,435.00,-486.55)`
    - `yaw=0.865 pitch=-0.445`
- Exact capture-frame log check:
  - `build/captures/capture_camera_exact_frame350_check_20260531`
  - confirmed `PERF_CAMERA_EXPOSURE frame=350` exists for `engine_frame_0350.bmp`.

Corrected same-frame high-alt/water audit:

- Baseline artifacts:
  - `build/captures/stress_absframe_frame360_baseline_20260531`
  - `build/captures/stress_absframe_frame360_owner55_20260531`
  - `build/captures/stress_absframe_frame360_material54_20260531`
  - `build/captures/stress_absframe_frame360_farwater_truth_audit_20260531`
- Corrected frame 360 audit result:
  - owner: `exact_sparse_surface_extended=306`, `water=72`, `mid_voxel=6`
  - water-owned samples with terrain before water:
    - `>2 units = 2`
    - `>8 units = 2`
  - This means the older large water-over-dry-terrain bucket was mostly an audit/camera mismatch.
- Exact noncoverage for the remaining 2 rows:
  - `build/captures/stress_absframe_frame360_exact_noncoverage_20260531`
  - both rows: exact brick resident, surface known, GPU surface record present, draw slot present, nearest exact face before water.

Dead-end tested and reverted:

- Tried sparse-surface PSO `D3D12_CULL_MODE_NONE` again with stable camera.
  - Artifact: `build/captures/sparse_surface_cull_none_absframe_v1_*`
  - It did not reduce same-frame water artifacts; water-over-terrain got slightly worse.
  - Source reverted to `D3D12_CULL_MODE_BACK`.
- Tried expanding `ResolveExactSparseSurfaceBeforeBackground` to probe six neighbor bricks.
  - Artifact: `build/captures/exact_resolver_neighbor_v1_frame360_20260531`
  - It reduced one tiny water-over-terrain count but visibly regressed the normal render with blue holes and more mid/water ownership.
  - Source and runtime shader asset were reverted.

Current visual check:

- Waterline/public sequence:
  - `build/captures/current_waterline_sequence_20260531`
  - frames 200, 240, 280, 320, 360 are visually stable/coherent in this capture.
  - temporal changed-pixel pct:
    - 200->240: `0.195%`
    - 240->280: `0.022%`
    - 280->320: `0`
    - 320->360: `0.022%`

What this means:

- Do not keep chasing the old broad "water hides dry terrain" diagnosis without same-frame camera evidence.
- The current high-alt frame 360 and waterline sequence are much more coherent than the user screenshots that triggered the last reset.
- Remaining rendering issues must be reproduced with exact-frame camera logs and same-frame owner/material captures.
- If the user still sees "all water then terrain slowly streams in," capture that exact live/default view class using the now-fixed capture/audit path before patching renderer logic.

Recommended next:

1. Reproduce the user's current broken live/default view using exact-frame capture logging.
2. Use same-frame normal/owner/material captures only.
3. If bad pixels are water or mid/far, run the targeted audit from that exact frame.
4. Patch only the owner/path that the same-frame audit proves wrong.

## Current State - 2026-05-31 Shoreline/Moving View Underwater-Tint Bug Fixed

Read this before continuing terrain/rendering work. The goal is still active and not complete.

## Current State - 2026-06-01 Repair-Only Exact Handoff v2

Latest user-visible objective:

- Fix public voxel terrain rendering so gameplay frames show coherent terrain, not partial exact chunks mixed with coarse mid/far fallback, water sheets, holes, or slow visible build-in.
- Do not drift into material tuning, startup-only metrics, or report generation unless it directly improves visible rendering correctness.

Latest code changes:

- `src/main_launcher.cpp`
  - Added `VENPOD_SPARSE_SURFACE_REPAIR_ONLY_NONREADY_KEEPS_WIDE_RASTER`, default `1`.
  - Changed startup shader unsafe non-ready default:
    - `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_MAX_CONTRACT_NONREADY`
    - old default `0`
    - new default `64`
  - Exact surface handoff now distinguishes:
    - actual unsafe near miss pixels, and
    - repair-only non-ready exact candidates that are covered safely by lower LOD.
  - When exact surface is already promoted and shader unsafe feedback is recent but actual unsafe near misses are zero, non-ready exact candidates no longer globally collapse the whole view back to coarse fallback.

Why this was changed:

- Before this patch, public moving frames around 190-200 could globally demote the wide exact surface band even when:
  - `unsafe=0`
  - `miss=0`
  - the feedback was only repair/non-ready exact bookkeeping.
- That created the visible failure where terrain looked like it was rebuilding itself through coarse mid closure/fallback.

Validation:

- Build passed:
  - `.\build.ps1 -Config Release`
- Targeted early moving capture:
  - `build/captures/current_goal_public_walk_repaironly_keepwide_v2_190_20260601`
  - Old mid-closure spike around frames 190/200 dropped from about `17%` mid voxel to about `2%`.
  - `surfaceRasterMax=2560`
  - `surfacePromoted=1`
  - unsafe near miss remained clean.
- Longer moving capture:
  - `build/captures/current_goal_public_walk_repaironly_keepwide_v2_180_500_20260601`
  - Early-mid frames no longer show the old collapse:
    - frame 180 midVoxel about `2.30%`
    - frame 220 midVoxel about `2.05%`
    - frame 300 midVoxel about `0.62%`
  - Remaining visible issue appears later around water/shoreline frames 380-460.

Current remaining root issue:

- Do not mark the goal complete.
- The early exact/mid handoff collapse is improved, but later frames still show broken shoreline/water composition.
- Frame 420 is the current active suspect:
  - normal capture:
    - `build/captures/current_goal_public_walk_repaironly_keepwide_v2_180_500_20260601/engine_frame_0420.bmp`
  - debug captures:
    - `build/captures/current_goal_frame420_debug_repaironly_v2_20260601/mode_55/engine_frame_0420.bmp`
    - `build/captures/current_goal_frame420_debug_repaironly_v2_20260601/mode_56/engine_frame_0420.bmp`
    - `build/captures/current_goal_frame420_debug_repaironly_v2_20260601/mode_61/engine_frame_0420.bmp`
    - `build/captures/current_goal_frame420_debug_repaironly_v2_20260601/mode_64/engine_frame_0420.bmp`
    - `build/captures/current_goal_frame420_debug_repaironly_v2_20260601/mode_68/engine_frame_0420.bmp`
- Owner/material/water debug says:
  - exact/surface terrain is present,
  - a broad water/far-water band is being selected over/through terrain,
  - this is not primarily hidden exact miss or far-SVO readiness.

Next concrete debugging target:

- Diagnose and fix water/shoreline ownership/depth ordering in frame 420.
- Inspect:
  - `assets/shaders/Graphics/PS_Raymarch.hlsl`
    - `RaymarchFarWater`
    - near water plane selection
    - `skipLocalWater`
    - water return path through `DebugBackgroundLayerHitWithExactFeedback`
  - `assets/shaders/Graphics/PS_SparseSurface.hlsl`
    - sparse surface discard/occlusion against water plane
- Direct question:
  - Why is water/far-water winning a broad horizontal band when exact/near terrain is present and drawable?
- Do not continue material tuning or startup-only gate tuning before answering that.

Reproduction:

- Stationary default public capture is currently coherent:
  - `build/captures/default_public_sequence_clean_current`
  - `build/captures/rebrun_live_contract_capture_current`
- A moving public walk did reproduce a real user-visible bad class:
  - `build/captures/moving_public_walk_current`
  - Same route frames 380/400 looked like terrain was turning into broad water/teal sheets.
  - Camera state showed the camera was above generated terrain and above the actual sea level, but close to the shoreline/low basin:
    - frame 380 before patch: `camY=-28`, `terrainH=-36.28`, `cameraAbove=8.28`
    - sea level is `-48`
- Same-frame owner/material debug for frame 380:
  - `build/captures/moving_public_walk_frame380_owner55`
  - `build/captures/moving_public_walk_frame380_material54`
  - Debug was dominated by exact/surface material, not missing terrain or far SVO.

Root cause fixed:

- `assets/shaders/Graphics/PS_SparseSurface.hlsl` treated cameras as `underwaterView` when:
  - `frame.cameraPosition.y < FAR_SEA_LEVEL + 8.0`
- With `FAR_SEA_LEVEL=-48`, cameras up to y=`-40` were shaded as underwater even while physically above water.
- That made valid exact sparse terrain near shorelines render with underwater fog/tint and read as "all water" or broken streaming.

Patch:

- Changed `underwaterView` in `PS_SparseSurface.hlsl` to match the actual underwater contract:
  - `frame.cameraPosition.y < FAR_SEA_LEVEL - 0.5`
- This is a rendering logic fix, not a material/color polish pass and not a fallback.

Validation after patch:

- Build/asset refresh:
  - `.\build.ps1 -Config Release`
- Moving public walk after patch:
  - `build/captures/moving_public_walk_underwater_fix_v1`
  - Visually removes the broad teal underwater tint from near-water terrain.
  - The script still flags frames 380/400 using `terrainLikePct`; that gate appears to misclassify mostly sand/water shoreline frames as sky-like, so do not treat that specific marker alone as proof of a terrain hole.
- High stress view:
  - `build/captures/stress_high_current_after_underwater_fix`
  - Passed and looked coherent.
- Intentional waterline/underwater view:
  - `build/captures/waterline_after_underwater_fix_v1`
  - Passed; underwater rendering remains active when actually underwater.
- Normal smoke with ownership:
  - `build/captures/normal_smoke_after_underwater_fix_v1`
  - Passed terrain-critical and ownership diagnostics.
- Longer moving walk after patch:
  - `build/captures/moving_public_walk_long_after_underwater_fix`
  - Still fails the script's simple visual marker at frames 360/400, but the contact sheet shows low shoreline/sand-bank composition rather than the old teal underwater-tint failure.
  - Later ownership samples have `miss` near zero and no unsafe near misses; farWater grows when the route looks across large basin water.

Current remaining work:

- Do not mark the goal complete.
- The shoreline tint bug is one concrete visible fix, but the broader goal still needs more moving-view validation for:
  - true holes/gaps during motion,
  - exact/mid/far ownership coherence under camera movement,
  - water/shoreline ownership when exact terrain is present,
  - far/mid streaming when moving into new view corridors.
- Next useful probe should target moving-camera frames where the image visibly has actual gaps or wrong owner pixels, not just `terrainLikePct` false positives on sand-heavy frames.

## Current State - 2026-05-31 Coherent Foreground Exact Handoff v1

Read this before continuing after compaction. The active goal remains: fix public voxel terrain rendering so gameplay frames are coherent, not merely make audits pass.

What was disproven:

- The moving fixed-dt capture scheduler is not the current blocker.
- A short moving capture wrote requested BMPs at frames 160 and 200:
  - `build/captures/moving_fixeddt_capture_repro_short`
- The earlier zero-frame capture was likely timeout/kill behavior, not a scheduler bug.

Root mechanism found:

- `src/main_launcher.cpp` allowed the outer exact sparse surface foreground band to draw out to about `1792` even when the current-view exact feedback was dirty.
- That mixed partial exact sparse surface with lower LOD mid/far fallback in public frames.
- This matches the visible symptom: terrain appears to build itself in, with partial chunks and inconsistent LOD ownership.

Patch:

- Added `VENPOD_SPARSE_SURFACE_COHERENT_FOREGROUND_HANDOFF`, default on.
- When hidden-exact/current-view proof is dirty, cap exact sparse surface raster max to `exactNear` instead of the wider foreground band.
- When the current view is clean/promoted, reopen the wider foreground surface band.
- This is a rendering ownership/handoff fix, not a material or fake-fallback fix.

Build:

- `.\build.ps1 -Config Release` passed.
- Warnings only; no build failure.

Clean validation artifact:

- `build/captures/moving_fixeddt_coherent_handoff_v1_cleanlog`
- Contact sheet frames 160, 200, 240, 280, 320 look coherent for this moving route; no obvious old teal-water tint or broken mid/far chunk collapse in this sequence.
- Camera stayed valid:
  - `cameraInside=0`
  - `feetInside=0`
- Far SVO and mid coverage were ready:
  - `farStage=complete`
  - `farCov=1.00/1.00`
  - `midCov` about `0.97-1.00`

Important clean telemetry:

- Frame 160:
  - `surfaceRasterMax=1792`
  - `surfacePromoted=1`
  - `surfaceClean=2/2`
  - `shaderUnsafeNonReady=32/32`
  - `hiddenExactMissing=0/32`
- Frame 200:
  - `surfaceRasterMax=1024`
  - `surfacePromoted=0`
  - `surfaceClean=0/2`
  - `shaderUnsafeNonReady=160/32`
  - exact foreground band correctly held back while dirty.
- Frame 240:
  - `surfaceRasterMax=1792`
  - `surfacePromoted=1`
  - `surfaceClean=2/2`
  - `shaderUnsafeNonReady=30/32`
  - `hiddenExactMissing=2/32`
  - This is a borderline case to keep watching: promoted despite two hidden-exact misses.
- Frame 280:
  - `surfaceRasterMax=1024`
  - `surfacePromoted=0`
  - `surfaceClean=1/2`
  - `hiddenExactMissing=19/32`
  - exact foreground band correctly held back.
- Frame 320:
  - `surfaceRasterMax=1792`
  - `surfacePromoted=1`
  - `surfaceClean=2/2`
  - `shaderUnsafeNonReady=0/32`
  - `hiddenExactMissing=0/32`

Ownership notes from clean run:

- `unsafeNearMiss=0` throughout sampled ownership rows.
- `miss` was near zero, but nonzero later:
  - frame 300 ownership row had `miss=3`
  - frame 345 ownership row had `miss=11`
- `farWater` rises later because this route looks across large basin water; do not treat farWater alone as a terrain bug without same-frame owner/audit evidence.

Current state:

- This patch improves public-frame LOD coherence for one moving route by avoiding partial wide exact-surface draw while exact foreground proof is dirty.
- It does not prove the broader user screenshots are solved.
- The goal is still active.

Next work:

1. Run normal smoke with ownership diagnostics after this patch, not `-SkipOwnershipDiagnostics`.
2. Reproduce the user's current broken live/default view with exact-frame camera logging.
3. For any remaining visible holes or wrong terrain/water pixels, use same-frame normal/owner/material captures before patching.
4. Watch the frame-240 borderline case where `surfacePromoted=1` while `hiddenExactMissing=2/32`.
5. If that case correlates with visible artifacts, tighten promotion to require hidden-exact clean in addition to shader unsafe clean.

## Current State - 2026-06-02 Fresh Ownership Proof Exposed Handoff/Streaming Bug

Read this before continuing after compaction. Do not claim the renderer is solved.

User-visible state:

- Rendering is still broken/laggy in broader playable views.
- Terrain still appears to stream in slowly.
- Some frames remain visually incoherent around water/shorelines and far terrain.
- The current active goal is still the public rendering contract: every visible pixel needs a legitimate coherent owner, not just eventual exact terrain.

What was learned:

- The older public-frame handoff was using stale ownership proof.
- Default ownership proof cadence was effectively too slow:
  - `VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL` was 15.
  - Frame 240 could render using owner proof from around frame 225.
  - Post-render feedback later discovered unsafe exact-contract terrain that the public frame had already shown incorrectly.
- Setting ownership proof to every frame is directionally correct, but it exposes the real mechanism instead of fixing it alone.

Patch state from interrupted work:

- `src/main_launcher.cpp` currently has:
  - `sparseRenderOwnershipInterval` default changed to `1`.
  - promotion/repair feedback max-age defaults changed to `5` so every-frame readback is considered fresh despite the readback-ring delay.
  - `sparseSurfaceRasterDemotionMaxShaderNonReady` default was changed to `48`, but this attempt did not fix the issue because the promotion path still uses the stricter promotion limit while unpromoted.
- Build succeeded after these edits.
- The `48` demotion-threshold change should not be treated as a proven fix. It is probably a failed partial patch unless followed by a real bounded-wide/public handoff fix.

Validation artifacts:

- `build/captures/current_goal_v2r_frame240_ownership_every_frame_age5_20260602`
  - Smoke passed.
  - Ownership proof age improved from stale `~15` frames to about `4` frames.
  - But visual/layer ownership got worse:
    - surface fell to about `41.6%`.
    - mid voxel rose to about `21.9%`.
    - far-water rose to about `10.8%`.
  - Meaning: fresh proof correctly detects current exact non-ready terrain, but the renderer responds by globally collapsing exact raster to the near band and letting lower LOD/water take over too much.

- `build/captures/current_goal_v2s_frame240_fresh_owner_bounded_wide_20260602`
  - Failed terrain-critical gate around frame 249.
  - Missing critical exact terrain remained non-ready.
  - Sparse page pool was near full and repair pressure was high.
  - The attempted demotion-threshold tweak did not keep wide exact raster active because the code still saw the surface as unpromoted.

Current root cause hypothesis:

- The architecture is not fundamentally impossible, but the current public rendering contract is internally inconsistent:
  1. Exact sparse surface is treated as authoritative for foreground quality.
  2. The renderer is allowed to show public frames while exact foreground proof is incomplete.
  3. Lower LOD mid/far/water can hide exact misses, so feedback arrives late.
  4. When fresh feedback is enabled, the handoff reacts too bluntly by demoting exact raster globally.
  5. Repair requests then compete with a nearly full sparse page pool and critical terrain can still be missing.

What this means:

- We are not blocked by scope in the sense that Minecraft-like streaming is impossible.
- We are blocked by an engine-contract mismatch:
  - exact, mid, far, and water are being mixed as independent fallback layers instead of a coherent LOD hierarchy with render-ready promotion rules.
  - "resident CPU brick" is not enough; public exact ownership needs GPU surface record/draw-slot readiness.
  - feedback and request priority are currently reactive and late, so visible frames can expose partial streaming.

Next mechanism-facing work:

1. Do not do material tuning or terrain generation changes.
2. Keep every-frame ownership proof and fresh age handling if validation continues to support it.
3. Fix the public handoff response:
   - bounded current-frame exact non-ready should trigger repair without globally collapsing into incoherent water/mid/far takeover.
   - exact foreground should widen only when render-ready proof is clean enough.
   - lower LOD should carry pixels coherently when exact is not ready, not produce holes or water takeover.
4. Fix request pressure/priority:
   - render-critical/protected exact terrain must not be starved by foreground repair or page-pool pressure.
   - readiness must mean GPU drawable, not merely CPU resident.
5. Re-run same-camera captures and report visual/layer deltas, not just green logs.

## Current State - 2026-06-02 v2w Startup Hidden-Exact Warm, Not Gate

Read this before continuing after compaction. The goal is still active and the
renderer is not globally done.

Problem addressed in this slice:

- Startup/public opening was still doing hidden-exact repair as a hard public
  render gate. That made the engine behave like "prove exact foreground before
  public render" instead of "open when mid/far/surface proof can present a
  coherent frame, then keep exact repair streaming."
- A control disabling hidden-exact feedback during startup entirely was a dead
  end:
  - normal/default smoke then failed scheduled captures at frames 200/220 and
    did not open until about frame 240.
  - reason: shader-unsafe proof needs startup foreground warming to get exact
    bricks ready enough for the first public view.

Patch now in current worktree:

- Keep hidden-exact miss feedback during startup enabled.
- Change only the default for `VENPOD_SPARSE_STARTUP_HIDDEN_EXACT_REPAIR_BLOCKS`
  to `0` in:
  - `src/main_launcher.cpp`
  - `rebrun.ps1`
  - `engine_capture_smoke.ps1`
- This means hidden-exact repair can warm/request bricks during loading, but its
  separate convergence flag no longer blocks public render by default.

Validated artifacts:

- `build/captures/current_goal_v2w_normal_smoke_hidden_warm_no_block_20260602`
  - Build already passed before this run.
  - Normal ownership smoke passed.
  - terrain-critical: `readyFrame=200`, `postNonReady=0`.
  - public metrics after frame 121:
    - `minSurface=64.88`
    - `maxMid=3.54`
    - `maxFarWater=0.39`
    - `maxMiss=0`
    - `maxHeightProxy=0`
    - ownership `maxMiss=0`, `maxUnsafe=0`
  - Startup held log improved versus v2t normal:
    - v2t normal held last logged frame was `140`.
    - v2w normal held last logged frame is `80`.

- `build/captures/current_goal_v2w_moving_public_hidden_warm_no_block_20260602`
  - Moving public walk passed.
  - terrain-critical: `readyFrame=121`, `postNonReady=0`.
  - contact sheet is coherent for this route; no visible old partial-streaming
    collapse.
  - public metrics after frame 121:
    - `minSurface=56.88`
    - `maxMid=14.90`
    - `maxFarWater=2.19`
    - `maxMiss=0`
    - `maxHeightProxy=0`
    - ownership `maxMiss=0`, `maxUnsafe=0`

Important interpretation:

- This is a real startup/public-contract improvement, not a cosmetic rendering
  fix.
- It does not solve every live camera. The remaining goal is still to find any
  public-frame views where visible pixels are owned by the wrong layer.
- Do not disable `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP` by default;
  that was tested and delayed public opening in the default/static scenario.

Next work:

1. Reproduce the user's current live broken view if possible with capture/log.
2. If the broken view is public and not under the loading overlay, run same-frame
   normal/owner/material/water captures and identify the dominant wrong owner.
3. If the problem is only visible during held startup/private warmup, make sure
   the real window shows only the loading overlay and not the private render.
4. Continue fixing actual owner/readiness failures; do not resume material,
   terrain-generation, or skyline tuning.

## Current State - 2026-06-02 v3b High-Altitude LOD / Water Contract

Read this before continuing after compaction. The goal remains active; this is
progress on one real broken public view class, not global completion.

Problem proven in this slice:

- The earlier v2w startup change helped normal/walk captures, but broad/high
  public views still behaved badly.
- A high-flight stress capture before this patch timed out around frame 336.
  Logs showed public rendering was open, but exact repair was still being driven
  as foreground-critical:
  - `PERF_SPARSE_HIDDEN_EXACT_MISS` around 33-40 ms/frame.
  - `timeExpired=1`.
  - `shaderUnsafeContractNonReady` present but `unsafe=0`.
  - camera hundreds of units above terrain.
- Short high-flight after the first policy patch exited, but failed the surface
  runtime contract with upload overflow starting around frame 140.
- Root cause chain:
  1. High-altitude LOD carry was blocked by ordinary exact-contract non-ready
     and hidden-exact repair pressure, even when there was no unsafe near miss.
  2. Far-water-heavy frames vetoed high-altitude LOD carry, forcing exact repair
     back on for a broad lake view.
  3. Shader water fallback could replace a closer mid/far terrain hit with water
     without checking that water was actually in front of that terrain hit.
  4. Hidden-exact high-altitude throttle capped general requests, but the
     separate water feedback lane still accepted up to 128 requests/frame,
     filling the exact surface allocator even when exact was not the public
     owner.

Patch now in current worktree:

- `src/main_launcher.cpp`
  - High-altitude LOD carry is blocked by actual unsafe near-miss pressure, not
    by ordinary exact-contract non-ready or hidden-exact repair pressure.
  - Far-water share no longer vetoes high-altitude LOD carry. Water correctness
    is handled by owner ordering, not by forcing exact-wide surface rendering.
  - Shader unsafe foreground repair does not request ordinary exact-contract
    repair in high-altitude LOD-carried views; it remains allowed only for hard
    unsafe near-miss feedback.
  - Hidden-exact high-altitude broad throttle now also throttles the water
    feedback lane:
    - high-alt cadence uses idle probe interval,
    - water request cap uses the high-alt max request cap instead of bypassing
      it with `VENPOD_SPARSE_HIDDEN_EXACT_WATER_MAX_REQUESTS`.

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - `TryResolveDeterministicWaterBeforeBackground()` now refuses water if the
    background terrain hit is clearly above the water band.
  - It also always requires water to be in front of the background hit, or only
    slightly behind within the coarse shoreline tolerance. The old foreground
    waterline path skipped this distance check, so missing exact terrain could
    appear as broad water until exact pages streamed in.

Validation artifacts:

- Build:
  - `.\build.ps1 -Config Release` passed.
  - Existing warnings only: local `rayDir` shadow warnings in `main_launcher.cpp`.

- Failed intermediate proof points:
  - `build/captures/current_goal_v2y_high_flight_short_lod_contract_20260602`
    - exited, but failed surface contract: upload overflow frames 140-159.
  - `build/captures/current_goal_v3a_high_flight_lod_water_contract_20260602`
    - high-alt throttle active and exact repair reduced, but water feedback still
      bypassed throttle; overflow frames 210-219.

- Passing current captures:
  - `build/captures/current_goal_v3b_high_flight_full_regression_20260602`
    - full high/general capture passed.
    - terrain-critical `readyFrame=121`, `postNonReady=0`.
    - `overflowHits=0`.
    - `highAltThrottle` active in public high frames.
    - public layer metrics after frame 121:
      - `maxMiss=0`
      - `maxUnsafe=0`
      - `maxFarWater=75.1995`
      - `maxSurface=17.9354`
    - contact sheet is coherent broad LOD/water, no exact-sparse mosaic
      assembling over water.

  - `build/captures/current_goal_v3b_normal_smoke_regression_20260602`
    - normal smoke passed.
    - terrain-critical `readyFrame=200`, `postNonReady=0`.
    - `overflowHits=0`, `maxMiss=0`, `maxUnsafe=0`.
    - surface remains the dominant owner as expected for the normal view.

  - `build/captures/current_goal_v3b_moving_smoke_regression_20260602`
    - moving stress capture passed.
    - terrain-critical `readyFrame=121`, `postNonReady=0`.
    - `overflowHits=0`, `maxMiss=0`, `maxUnsafe=0`.

  - `build/captures/current_goal_v3b_waterline_regression_20260602`
    - waterline capture passed after the shader water owner change.
    - terrain-critical `readyFrame=121`, `postNonReady=0`.
    - `overflowHits=0`, `maxMiss=0`, `maxUnsafe=0`.
    - `maxFarWater=0` for this underwater/waterline path.

Important interpretation:

- This is a real public rendering-contract fix:
  - high/far public views no longer demand exact sparse proof as the visible
    owner;
  - water can no longer blindly replace closer lower-LOD terrain;
  - hidden-exact water feedback no longer floods exact surface allocation in
    high-altitude LOD-carried views.
- This is not a cosmetic/material fix and not a gate weakening.
- The high view is still water-heavy because the deterministic world/lower LOD
  says those views are broad basin/lake views. If a future screenshot shows
  specific water-over-dry-terrain errors, audit that exact frame's far-water
  owner pixels against CPU terrain truth and mid/far hit distance.

Next work:

1. Do not return to material or skyline tuning.
2. If live `rebrun` still looks broken, capture that exact camera and classify
   whether the remaining wrong pixels are:
   - far-water over closer terrain,
   - mid/far missing deterministic terrain,
   - exact sparse surface non-ready where exact is truly required,
   - or legitimate procedural basin/water.
3. For any remaining water-heavy public failure, use the v3b shader distance
   rule as the invariant: water may own only if it is in front of terrain or
   within the bounded shoreline tolerance.
4. Goal is not complete until live/default public gameplay views are coherent
   across normal, moving, high, and waterline camera classes.

## Current State - 2026-06-02 v3h Coherent Public LOD / Bounded Hidden Exact

Read this before continuing after compaction. The goal remains active. This
section supersedes the older "startup must prove hidden exact before public"
direction.

Root mechanisms proven after v3b:

- Public/default rendering was still exposing a wide exact-surface band before
  hidden-exact foreground proof was clean. That produced the visible "world
  assembling" behavior: partial exact chunks mixed with mid/far fallback.
- Startup/private proof was still asking for a wider exact band than public
  render should show, so startup could stall on a contract the user should not
  see yet.
- Post-open hidden-exact repair was too expensive for public frames:
  `PERF_SPARSE_HIDDEN_EXACT_MISS` was about 25 ms/frame in default runs, while
  public rendering was already coherent through exact-near plus mid/far LOD.

Patches now in current worktree:

- `src/main_launcher.cpp`
  - Wide exact-surface public ownership now requires the hidden-exact runtime
    sweep to be clean enough:
    `sparseHiddenExactRuntimeCleanFrames >= sparseHiddenExactMissCleanIdleFrames`.
  - Startup shader-unsafe gate only requires wide surface-promotion proof when
    wide exact promotion is actually part of the public contract.
  - Private startup proof uses the same public contract:
    strict exact-near if hidden-exact is not clean, full raster max only after
    hidden-exact clean proof.
  - Default post-open hidden-exact repair is bounded:
    - ray budget: 512
    - probe time: 4 ms
    - warmup max requests: 32

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Prior exact-band alignment remains in the worktree: hidden-exact/unsafe
    feedback does not extend beyond the strict surface ownership band.

Validated artifacts:

- `build/captures/current_goal_v3e_shader_exact_band_strict_near_20260602_rerun`
  - strict-near capture passed.
  - frame 120: `surfaceRasterMax=1024`,
    `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`.

- `build/captures/current_goal_v3g_startup_gate_matches_public_lod_20260602`
  - default public capture passed.
  - public opened around frame 61/81 with strict exact-near plus mid/far carry.
  - frame 81: `surfaceRasterMax=1024`, `surfacePromoted=0`,
    `shaderUnsafeContractNonReady=0`, `miss=0`, `unsafe=0`.

- `build/captures/current_goal_v3h_default_bounded_post_open_hidden_exact_20260602`
  - default bounded hidden-exact capture passed.
  - hidden-exact cost dropped from about 25 ms/frame to about 4 ms/frame.
  - frame 120: `surfaceRasterMax=1024`,
    `shaderUnsafeContractNonReady=0`, `hiddenExactMissing=0/0`,
    `miss=0`, `unsafe=0`.
  - temporal owner deltas after opening were tiny, so this reduced assembly
    behavior without hiding errors through materials or fake fallback.

- Regressions passed after v3h:
  - `current_goal_v3h_normal_smoke_regression_20260602`
  - `current_goal_v3h_moving_smoke_regression_20260602`
  - `current_goal_v3h_high_flight_regression_20260602`
  - `current_goal_v3h_waterline_regression_20260602`

Important interpretation:

- This is real public rendering-contract progress:
  - the world should no longer open by showing a half-ready wide exact band;
  - public frames use exact-near plus legitimate mid/far LOD until exact proof
    is clean;
  - hidden-exact repair continues in the background instead of eating about
    25 ms of public frame time.
- This is not global completion:
  - lower LOD is still visible beyond exact-near;
  - frame body time is still high, around 66-74 ms in the default capture;
  - live free-camera views may still show wrong owners or geometry/LOD gaps;
  - if a screenshot is broken, capture that exact camera and classify the
    owner/reason instead of returning to materials, skyline, or terrain shape.

Next mechanism-facing work:

1. Capture the current live broken view, not old scripted views.
2. For visible holes/gaps, classify the owner and truth:
   exact non-ready, mid/far wrong geometry, far-water over terrain, sky/miss, or
   legitimate procedural air/water.
3. If public render is coherent but slow, profile the remaining frame body after
   hidden-exact is bounded. Do not undo the coherent LOD contract to chase speed.
4. Goal is still active until live/default gameplay rendering is coherent across
   close, waterline, broad/high, and moving views.

## Current State - 2026-06-02 v3k Startup Stall / Private Warmup Exposure

Read this before continuing after compaction. The goal is still active. Do not
claim the renderer is finished.

What was proven in this slice:

- The user's "frame 1 is broken / startup is stalled" complaint matches current
  telemetry. In the default baseline:
  - `SPARSE_STARTUP_PUBLIC_RENDER_HELD` was true through early frames.
  - startup hidden-exact warmup was allowed to run the 8192-ray / 75 ms profile.
  - frame 50: `PERF_SPARSE_HIDDEN_EXACT_MISS ms=82.25`,
    `feedback=716`, `accepted=716`.
  - frame 60: public was opening, but `PERF_FRAME_END body=127.33 ms`,
    `surfExtract=45.75 ms`, `hiddenExactMissing=994/0`.
- A bounded A/B with:
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_RAY_BUDGET=512`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_HIDDEN_EXACT_PROBE_MAX_MS_TENTHS=40`
  proved this was not required for coherent public render:
  - frame 60 body dropped to about `71-76 ms`.
  - surface extraction dropped to about `5-6 ms`.
  - `shaderUnsafeContractNonReady=0`, `miss=0`, `unsafeNearMiss=0`.
- Startup shader-unsafe blocking was also no longer necessary under the current
  public contract. With `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS=0`, the
  first public frames stayed coherent:
  - frame 60/61 `surfaceRasterMax=1024`, `midCov=1.00/1.00`,
    `miss=0`, `unsafeNearMiss=0`, `farHeight=0`.

Patches now in current worktree:

- `src/main_launcher.cpp`
  - Startup hidden-exact warmup default ray budget changed from `8192` to `512`.
  - Startup hidden-exact warmup default probe time changed from `75 ms` to
    `4 ms`.
  - `VENPOD_SPARSE_STARTUP_SHADER_UNSAFE_BLOCKS` default changed from `1` to
    `0`. Shader-unsafe feedback still runs after public open and still controls
    later wide exact-surface promotion; it no longer forces private startup
    render proof before the coherent exact-near + mid/far public contract opens.

- `src/Graphics/Renderer.cpp`
  - Voxel-terrain-only shader flag is now set from the requested sparse contract
    even if sparse-near raymarch resource binding is temporarily not active.
    This was intended to keep legacy height proxy disabled through binding gaps.

Validation artifacts:

- Baseline before patch:
  - `build/captures/current_goal_v3i_baseline_public_timeline_20260602`
  - comparison CSV:
    `build/captures/current_goal_v3i_startup_bounded_comparison.csv`

- Bounded startup A/B:
  - `build/captures/current_goal_v3i_ab_bounded_startup_hidden_exact_20260602`
  - passed.

- Default after bounded startup:
  - `build/captures/current_goal_v3i_default_bounded_startup_hidden_exact_20260602`
  - passed.

- Default after disabling private shader-unsafe block by default:
  - `build/captures/current_goal_v3k_default_no_private_shader_block_20260602`
  - frame 60: `body=69.67 ms`, `surfExtract=3.85 ms`,
    `surfaceRasterMax=1024`, `midCov=1.00/1.00`,
    `hiddenExactMissing=9/0`.
  - frame 60/61 ownership:
    `miss=0`, `unsafeNearMiss=0`, `farHeight=0`.

- Normal validation:
  - `build/captures/current_goal_v3k_normal_validation_no_private_startup_20260602`
  - passed with ownership diagnostics.
  - terrain-critical `readyFrame=61`, `postNonReady=0`.
  - `maxMiss=0`, `maxUnsafe=0`, `maxHeightProxyScreen=0`.

Remaining blocker proven, not fixed:

- Broad/high stress views still show a separate issue:
  - `build/captures/current_goal_v3j_stress_no_farheight_proxy_20260602`
  - frame 91/121 still had `farHeightScreenPct` about `0.60/0.44`.
  - This does not come from the simple C++ voxel-only flag binding gap. It is
    likely the shader's deterministic continuity fallback in
    `DebugBackgroundMissHit()` / `BuildDeterministicFarTerrainContinuityHit()`
    when resident mid/far voxel layers do not own a deterministic terrain ray.
  - Do not remove that fallback blindly: without a resident mid/far replacement
    it will become a miss/sky hole. The next fix should explain why Far SVO or
    mid voxel failed those rays, then make a real voxel owner handle them.

- Basin/water audit for stress frame 91:
  - `build/captures/current_goal_v3i_basin_water_audit_frame91_20260602`
  - summary: `252/256` sampled gray/terrain patches are CPU-generated terrain
    above sea level, not missing water.
  - Only `3` sampled water rays were "water before terrain"; water is not the
    dominant proven bug in that audited stress frame.

Next mechanism-facing work:

1. For broad/stress views, audit the `farHeight` continuity pixels:
   - pixel, ray, diagnostic terrain T,
   - whether Far SVO had a page/node at that position,
   - whether mid voxel sampled air/missing,
   - why resident voxel LOD did not own before analytic continuity fallback.
2. Replace the analytic `farHeight` continuity fallback with a real resident
   mid/far voxel owner only after the audit identifies the missing owner reason.
3. Keep startup hidden-exact bounded; do not re-enable private shader-unsafe
   startup blocking unless a capture proves exact-near + mid/far public opening
   is unsafe.

## Current State - 2026-06-02 v3l/v3m/v3n FarHeight Owner Investigation

Read this before continuing after compaction. The goal is still active. Do not
claim rendering is fixed.

What was tested:

- Stress debug mode 50 was captured at frame 91:
  - `build/captures/current_goal_v3l_stress_debug50_farheight_reason_20260602`
  - Harness exited nonzero, but `engine_frame_0091.bmp` was written.
  - Quantized non-UI colors were dominated by underwater/sky/debug classes;
    pure red missing-owner was tiny (`~0.02%`). This mode is useful, but it is
    not enough to prove the normal `farHeight` owner reason.

- Shader patch attempted in `PS_Raymarch.hlsl`:
  - `TryBuildResidentMidVoxelClosureHit()` now allows high-altitude diagnostic
    terrain before `midStart`/`exactNear` to sample resident ring0 mid voxels.
  - It also probes a few small post-crossing offsets to tolerate voxel
    quantization.
  - A compile typo (`preferredRing`) was fixed.
  - Stable stress capture:
    `build/captures/current_goal_v3m_stress_midclosure_steep_patch_20260602`
  - Result: did not clear `farHeight`.
    - frame 90 `farHeightScreenPct=0.6452`
    - frame 120 `farHeightScreenPct=0.429`
    - `miss=0`, but `heightProxy/farHeight` still violates the public contract.
  - Conclusion: remaining `farHeight` is not simply caused by the mid-closure
    distance/angle gate.

- A resident Far-SVO continuity helper was tried, then removed:
  - It attempted to traverse the resident Far SVO page around the diagnostic
    terrain crossing before falling back to analytic `farHeight`.
  - Artifact/log:
    `build/captures/current_goal_v3n_stress_far_svo_continuity_patch_20260602`
  - Result: rejected. It destabilized validation/readback:
    - repeated `GPUBuffer::Map failed: 0x-7785FFFB`
    - `own=0` in backend pipe after the issue
    - harness failed: no post-ready ownership sample
    - frame body around `130 ms`
  - The helper was removed. Do not reapply this broad per-pixel SVO traversal
    closure. If Far-SVO recovery is pursued, it needs a cheaper precomputed or
    budgeted mechanism, not a new per-pixel page traversal in the miss path.

Current source state after v3n:

- `assets/shaders/Graphics/PS_Raymarch.hlsl` still contains the high-altitude
  pre-mid resident mid-voxel closure relaxation and post-crossing probes.
- The unstable resident Far-SVO continuity helper/call was removed.
- Runtime shader assets were refreshed after removal.

Sharper current diagnosis:

- Broad/high views still produce public `farHeight` analytic continuity fallback
  where deterministic terrain exists but exact surface is not ready and resident
  mid/far owners did not claim the ray.
- The remaining fallback is coupled to high-altitude exact-band non-ready
  feedback:
  - example stress frame 91 after stable mid-closure patch:
    `shaderUnsafeContractNonReady=152`, `unsafeSample dist=955`
  - frame 121:
    `shaderUnsafeContractNonReady=197`, `unsafeSample dist=1018`
- This means a high/free-camera downward view is still treating the 1024-unit
  exact/surface band as render-critical, then repairing exact bricks after the
  frame while analytic continuity keeps the public image coherent-looking.

Next mechanism-facing work:

1. Do not tune materials, water, terrain generation, or skyline.
2. Do not retry the removed per-pixel Far-SVO traversal helper.
3. Add a cheap reason signal for remaining `farHeight` fallback:
   - whether it came from normal `RaymarchBackgroundField()` height path or
     `DebugBackgroundMissHit()`;
   - diagnostic terrain T and ray angle bucket;
   - whether mid closure sampled missing/air/solid;
   - whether Far SVO page at diagnostic XZ existed.
4. The likely architectural fix is to change high/free-camera public ownership:
   exact sparse should not be mandatory for the whole 1024-unit downward band.
   Either promote an actual lower-LOD owner for those pixels or shrink the
   high-alt exact render-critical contract. Do not solve that by drawing
   analytic `farHeight`.

## Streaming Playability Real Fix Campaign Continuation - 2026-06-04

Active goal:

- `streaming_playability_real_fix_campaign_20260604`
- Do not mark complete. VENPOD is not at a validated 60 FPS candidate.

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed, `1/1`
- known `rayDir` shadow warnings remain

Latest continuation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_continuation_table.md`
- rejected branch summaries in the same directory:
  - `rejected_generation_cap8_summary.csv`
  - `rejected_critical_reuse_tol2_summary.csv`
  - `rejected_surface_skip_sort_summary.csv`
  - `surface_extract_sort_fix_summary.csv`

Continuation branches tried:

- CPU projected parent-held feedback: default-off diagnostic only. It accepted almost no useful child coords and did not fix walk frame 600.
- General generation cap `8`: rejected and removed from runtime. It worsened walk frame 600 (`raw 121.55`, `CPU 72.84`) by deferring necessary generated work.
- Screen-critical reuse forward tolerance: rejected and removed. It activated reuse but regressed badly (`raw 195.84`, `CPU 116.81`, `coverage 94`, `budgetReason 2`).
- Surface extraction timed skip-sort: rejected and removed. It did not reduce `surfaceExtractMs` or total frame time.
- Timed surface extraction now avoids an unused global queue sort before per-class extraction. This is a behavior-preserving cleanup; per-class ordering remains unchanged.

Latest combined candidate matrix, with background split/playable quality, clean throttle, stats single flush, backlog-aware pump, visible-critical prepump, and fallback diagnostics:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract/stage | GPU ray | Missing | Sampled/unsampled | Coverage | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `103.13` | `43.44` | `19.55` | `13.98` | `9.90` | `7.50` | n/a | `13.11` | `28` | `24/4` | `99` | `0/0` | not a stable fixed 60 row |
| walk realtime | `600` | `144.49` | `115.15` | `26.16` | `23.74` | `65.22` | `47.49` | `13.26/3.48` | `20.94` | `445` | `376/69` | `94` | `0/0` | sampled-visible debt, coverage emergency |
| high-alt | `378` | `45.14` | `23.26` | `5.07` | `5.48` | `12.70` | `4.96` | n/a | `10.71` | `1833` | `33/1800` | `99` visible / `80` cache | `0/0` | visible-critical prepump helps high-alt |

Current blocker classification:

- High-alt: over-broad cache/current-interest debt is real. `VISIBLE_CRITICAL_PREPUMP` is the right default-off high-alt direction and keeps cache debt from forcing broad catch-up.
- Walk/realtime: not over-broad in the same way. Missing mid debt is mostly projected-visible and fallback-unknown/invalid. Deferring it would weaken ownership.
- Generation/request/surface caps and reuse shortcuts do not solve walk. They either move debt into later catch-up or create worse coverage/parent-held behavior.
- Async generation remains blocked for sampled walk debt because fallback-valid remains zero.

Next implementation slice that can actually move playability:

1. Stop trying caps/reuse heuristics for sampled walk debt.
2. Implement a real ownership-aware streaming state machine:
   - visible-critical sampled bricks stay synchronous or readiness-blocked
   - cache/prefetch bricks are async/deferred
   - generated CPU payloads are produced off-thread
   - GPU upload/apply happens at frame boundaries with a budget
   - surface extraction/staging also becomes queued/budgeted with visible-critical priority
   - per-brick generation/edit stamps reject stale completions
3. In parallel, add owner metadata/feedback needed to turn movement unknowns into valid/invalid owners:
   - Far-SVO material/occupancy/shoreline validity
   - coarser LOD error/ray-angle proof
   - water/sky proof or sampled owner feedback
4. Validate only with noncapture rows before claiming 60 FPS.

Plain-English status:

- GPU has a credible default-off path.
- High-alt CPU has a credible default-off cache/critical split.
- Walk/realtime is still blocked by visible sampled terrain debt with unknown fallback. This is why queue caps, reuse tolerance, and async generation cannot be made safe yet.
### Streaming Playability Real Fix Campaign - Active Handoff - 2026-06-04

Active goal:

- `streaming_playability_real_fix_campaign_20260604`
- Objective: drive VENPOD toward a validated playable candidate with real noncapture validation, anti-stop rules, persistent handoff updates, and no correctness weakening/default promotion without proof.

Latest build/test:

- `.\build.ps1 -Config Release`: passed after the accepted surface queue cleanup.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings remain.

Accepted code change in this continuation:

- `src/Simulation/SparseVoxelWorld.cpp`
- `src/Simulation/SparseVoxelWorld.h`
- `src/main_launcher.cpp`
- Added behavior-preserving stale surface extraction queue cleanup:
  - if there are no pending surface payloads, stale surface extraction queues/class queues are pruned instead of scanned;
  - direct coord surface extraction now removes stale queue aliases when no pending payload exists;
  - launcher skips surface extraction attempts when the world reports zero pending surface payloads.
- This is queue hygiene only; it did not solve the campaign.

Latest authoritative current matrix:

- `build/captures/current_after_surface_queue_cleanup_20260604`
- copied to campaign artifacts:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/latest_current_after_surface_queue_cleanup_table.md`

Rows with common candidate stack (`playable`, background split `0.375`, clean throttle, backlog-aware pump, fallback diagnostics, visible-critical prepump):

| Scenario | Frame | Raw ms | CPU update ms | Request ms | Gen ms | Clip ms | Surface extract ms | GPU ray ms | Missing voxel | Sampled/unsampled | Coverage | Miss/unsafe | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `40.06` | `9.07` | `2.49` | `5.60` | `0.98` | n/a parsed | `7.22` | `0` | `0/0` | `100` | `0/0` | frame `384` still showed post-open prepublish surface spike `29.62 ms` |
| walk realtime | `600` | `105.52` | `65.19` | `30.04` | `26.18` | `8.92` | `15.04` | `36.16` | `435` | `366/69` | `95` | `0/0` | request/gen/surface/GPU all bad; not playable |
| high-alt | `400` | `69.39` | `44.03` | `12.73` | `11.19` | `20.10` | n/a parsed | `16.61` | `1994` | `62/1932` | `99` visible-critical / `78` cache | `0/0` | visible-critical prepump protects public coverage, cache debt remains |

Rejected or diagnostic-only in this continuation:

- `VENPOD_SPARSE_MID_CLIPMAP_DIRECT_FOOTPRINT_COLUMNS=1`
  - first walk probe improved CPU, but full matrix regressed fixed/walk/high-alt; rejected.
- `VENPOD_SPARSE_MID_CLIPMAP_SHARED_COLUMN_CACHE=1`
  - walk CPU regressed to `64.48 ms`; rejected.
- `VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET=4`
  - did not fix fixed surface spike and produced unstable debt; rejected.
- `VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS=4`
  - capped fixed prepublish surface but worsened walk/high-alt clip catch-up; rejected.
- `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_SURFACE_BUDGET=16`
  - reduced fixed surface extraction but worsened walk/high-alt debt; rejected.
- bounded64 comparison with current candidate stack:
  - walk frame `600` demoted/blocked (`hiddenExactMissing=373 > 64`) and regressed; bounded repair remains default-off and not a performance fix.

Current engineering diagnosis:

- We still do not have a validated playable candidate.
- The remaining problem is not one queue number:
  - walk is dominated by exact sparse request/prep, exact page generation, prepublish/surface extraction, GPU ray/gaps, and some clip pump;
  - fixed still sees post-open hidden-exact prepublish surface bursts;
  - high-alt has over-broad cache debt, but visible-critical prepump keeps visible coverage safe.
- Async generation remains blocked for sampled visible unknown/invalid mid debt.
- Exact sparse page generation is a better async candidate than mid-clipmap generation because it already has `GenerateBrickWithCachedTerrainColumns`, but it is not worker-safe as-is:
  - it mutates `m_surfaceTerrainColumnCache`;
  - it applies `SparseEditStore` overlays;
  - it transitions pool/generated/upload/surface state on the main thread.

Next real implementation slice:

1. Extract a pure exact sparse page payload builder:
   - local terrain column cache only;
   - no mutation of `SparseVoxelWorld` caches;
   - no GPU/pool/state mutation;
   - edit-safe via revision/overlay snapshot or async disabled/discarded when edits are active.
2. Add default-off async exact page generation for non-edited requested bricks:
   - worker builds `GeneratedSparseBrick`;
   - main thread applies only at frame boundary;
   - state must still transition through generated/upload/surface queues on main thread;
   - stale completions discarded by coord state + edit revision.
3. Add logs:
   - asyncExactQueueDepth/enqueued/generated/completed/applied/discarded/oldestAge/workerMs/mainApplyMs;
   - syncGeneratedVisible/Collision/Speculative;
   - upload/surface backlog after async apply.
4. Validate fixed, walk realtime, high-alt, and noncapture with the candidate stack.

Do not repeat:

- Do not use surface/global caps that only defer hidden/general surface debt and trigger catch-up elsewhere.
- Do not treat unknown fallback as async safe.
- Do not revive blunt mid pump caps, age priority, or ring-only visible-critical heuristics.
- Do not promote background split, clean throttle, backlog pump, bounded repair, or async defaults without proof.

### Streaming Playability Campaign - Exact Generation Branch Result - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

New default-off knobs:

- `VENPOD_SPARSE_EXACT_ASYNC_GENERATION=0`
- `VENPOD_SPARSE_EXACT_ASYNC_VISIBLE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_QUEUE_MAX=256`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_APPLY_PER_FRAME=32`
- `VENPOD_SPARSE_EXACT_DIRECT_GENERATION=0`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Branches attempted:

- all-class async exact generation: rejected. Fixed improved, but walk realtime regressed from `75.77` to `100.37 ms` CPU and clip/pump rose to `56.60/54.29 ms`. Asyncing visible/current exact pages created readiness debt that mid-clipmap catch-up paid back.
- speculative-only async exact generation: no effect in sampled rows. It queued/applied `0` async bricks, so it is safe as a no-op diagnostic but not a fix.
- direct exact generation using `SparseTerrainGenerator::GenerateBrick`: rejected. It did not reduce the walk blocker (`82.33 ms` CPU, `46.30 ms` clip) and did not produce a playable candidate.

Representative rows:

| Run | Scenario | Frame | CPU ms | Req | Gen | Clip | Pump | GPU | Missing | Coverage | Miss/unsafe | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | walk realtime | `600` | `75.77` | `15.22` | `14.53` | `45.99` | `31.28` | `19.02` | `453` | `94` | `0/0` | still blocked |
| async all classes | walk realtime | `600` | `100.37` | `26.59` | `17.15` | `56.60` | `54.29` | `5.62` | `512` | `94` | `0/0` | rejected regression |
| async speculative only | walk realtime | `600` | `67.11` | `30.03` | `28.09` | `8.97` | `6.49` | `6.12` | `454` | `95` | `0/0` | no async work occurred; run variance, not a fix |
| direct exact | walk realtime | `600` | `82.33` | `17.86` | `18.16` | `46.30` | `31.72` | `19.14` | `469` | `94` | `0/0` | rejected |
| direct exact | high-alt | `375` | `34.66` | `7.07` | `9.49` | `18.10` | `5.29` | `11.92` | `1862` | `99 visible / 79 cache` | `0/0` | partial high-alt improvement only |

Decision:

- No validated playable candidate exists.
- Exact-page async is only safe for cache/speculative work unless visibility/readiness is integrated; visible exact async regresses walk.
- Walk remains dominated by sampled/projected-visible debt and catch-up. The next real work is not another exact-generation tweak; it is ownership-aware streaming state:
  - split visible sampled work from cache/prefetch before request/generation/pump;
  - async only cache/prefetch or CPU-proved fallback-valid work;
  - keep visible fallback-unknown work synchronous/guarded;
  - separately budget request prep, upload/apply, and surface extraction with backlog age.

### Goal Control Handoff - 2026-06-04

Active `/goal`:

- `streaming_playability_real_fix_campaign_20260604`
- Objective: drive VENPOD from default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes, with explicit anti-stop rules, persistent handoff updates, and no correctness weakening or default promotion without proof.
- Status: active. Do not mark complete unless a validated noncapture playable candidate exists or a hard architecture/tool blocker is proven after multiple safe branches.

Why this handoff exists:

- The campaign has produced real fixes and rejections, but it has also stopped too early after diagnostics or one failed prototype.
- Future continuations must preserve the current state, continue from measured evidence, and update this file after each meaningful branch.
- Do not restart from broad root-cause hunting unless local context is missing; use the tables and artifacts below.

Latest branch still in worktree:

- Default-off high-alt/cache pump slice:
  - `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0`
  - `perf_noncapture_smoke.ps1` supports `-MidClipmapCacheOnlyDefer`
  - build and ctest passed after the patch.
- Behavior: when high-alt visible-critical prepump is active, visible/projected-critical bricks are prioritized and unsampled cache bricks can be deferred instead of forcing broad pump catch-up.
- Decision: this is safe default-off partial evidence, not a playable candidate and not a walk fix.

Latest branch table:

| Branch | Fixed | Walk realtime | High-alt | Decision |
|---|---|---|---|---|
| matched baseline | raw `21.89`, CPU `20.86` | raw `55.84`, CPU `39.83`, clip `6.80` | raw `50.86`, CPU `29.29`, clip/pump `16.58/5.40` | comparison row |
| cache-only defer | raw `26.88`, CPU `22.39` | raw `57.35`, CPU `64.48`, flag inactive | raw `49.96`, CPU `26.31`, clip/pump `13.50/1.74` | safe default-off high-alt partial, not a walk fix |
| post-open surface cap `1 ms` | raw `19.94`, CPU `18.87` | raw `75.18`, CPU `34.79` | raw `55.17`, CPU `34.37` | reject; shifts debt |
| pressure trim free guard | raw `24.50`, CPU `20.70` | raw `55.45`, CPU `38.98` | raw `51.33`, CPU `29.68` | minor request win, not enough |
| surface strict/buried | raw `24.16`, CPU `22.16` | raw `101.70`, CPU `44.04`, publish backlog | raw `51.87`, CPU `30.76` | reject |
| generation budget `8` | raw `18.02`, CPU `22.39` | raw `67.90`, CPU `67.39`, clip `46.16` | raw `43.96`, CPU `27.49` | reject globally; high-alt evidence only |

Campaign artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`

Anti-stop rules:

- Do not stop after adding a diagnostic if the next measured code change is obvious.
- Do not stop after one failed heuristic if another safe branch is still supported by evidence.
- Do not call `missScreenPct=0` / `unsafeNearMissPct=0` success while frame time and public visual coherence are bad.
- Do not accept a candidate without build, ctest, fixed/walk/high-alt validation, and noncapture validation when available.
- Do not claim 60 FPS unless representative noncapture total frame time is at or below `16.67 ms`.
- Do not promote defaults. All playable stack components remain opt-in until longer validation exists.

Current engine state:

- GPU has a strong default-off path: background split at scale `0.375`.
- Fixed-camera rows can approach useful numbers, but surface/prepublish and generation spikes remain unstable.
- High-alt over-broad cache debt is now partially handled by visible-critical/cache splitting.
- Representative walk/realtime remains the hardest blocker: missing mid debt is mostly projected-visible and fallback-unknown/invalid, so it cannot be safely deferred or async-generated under the current ownership contract.
- Global caps and broad async create readiness debt that mid-clipmap catch-up repays later.

Next real implementation sequence:

1. Keep the high-alt cache-only defer branch only as default-off partial evidence unless it regresses visuals.
2. Build the ownership-aware streaming state machine instead of more global caps:
   - explicit lanes: visible-critical, sampled/likely-visible, cache, prefetch, maintenance;
   - visible sampled fallback-unknown work remains synchronous/guarded;
   - cache/prefetch work is allowed to defer and eventually go async;
   - every brick carries generation/edit stamps;
   - worker payload generation never mutates render/GPU state;
   - main-thread apply/upload/surface staging is frame-boundary and budgeted.
3. Add owner proof metadata or feedback for movement debt:
   - Far-SVO material/occupancy/shoreline validity;
   - coarser LOD error/ray-angle proof;
   - deterministic water/sky proof or bounded sampled owner feedback.
4. Validate the combined stack with `perf_noncapture_smoke.ps1` before any playable claim.

### Visible Priority Pump Branch - 2026-06-04

New default-off knob:

- `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY_PUMP=0`

Implementation:

- `src/main_launcher.cpp`
  - Reuses the existing CPU projected missing-mid bounds path.
  - When enabled, projected-visible missing mid-clipmap voxel coords are moved to the front of the voxel generation queue through `SparseClipmapTileCache::PrioritizeVoxelGenerationCoords`.
  - It does not reduce budgets, lower guards, mark unknown fallback safe, or change default behavior.
  - Existing high-alt `VISIBLE_CRITICAL_PREPUMP` remains the only path that rewrites coverage/budget reasoning.
  - Adds `PERF_SPARSE_MID_CLIPMAP_VISIBLE_PRIORITY`.
- `perf_noncapture_smoke.ps1`
  - Adds `-MidClipmapVisiblePriorityPump`.
  - Parses `midVisiblePriorityProjected` and `midVisiblePriorityPrioritized`.

Build/test:

- `.\build.ps1 -Config Release`: passed, with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Validation artifacts:

- `build/captures/visible_priority_walk_baseline_20260604`
- `build/captures/visible_priority_walk_candidate_20260604`
- `build/captures/visible_priority_fixed_candidate_20260604`
- `build/captures/visible_priority_highalt_candidate_20260604`
- `build/captures/visible_priority_parallel_pump_walk_candidate_20260604`
- `build/captures/visible_priority_parallel_mid_exact_walk_candidate_20260604`
- `build/captures/visible_priority_parallel_mid_exact_fixed_candidate_20260604`
- `build/captures/visible_priority_parallel_mid_exact_highalt_candidate_20260604`
- campaign table updated:
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
  - `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`

Key rows:

| Stack | Scenario | Raw ms | CPU ms | Req | Gen | Clip | Pump | GPU | Missing/sampled | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline rerun | walk | `69.36` | `58.73` | `14.19` | `16.43` | `28.07` | `25.79` | `17.72` | `516/324` | `0/0` | reference |
| visible priority | walk | `79.40` | `48.62` | `13.61` | `13.63` | `21.35` | `19.30` | `14.06` | `494/312`, prioritized `335` | `0/0` | CPU/clip partial, raw worsened |
| visible priority | fixed | `21.26` | `21.35` | `4.21` | `16.20` | `0.92` | `0.00` | `6.41` | `0/0` | `0/0` | fixed unchanged |
| visible priority | high-alt | `44.48` | `28.57` | `5.00` | `5.53` | `18.04` | `5.86` | `11.11` | `1760/46`, prioritized `51` | `0/0` | safe partial, not enough |
| visible priority + parallel mid pump | walk | `64.18` | `43.18` | `14.96` | `16.27` | `11.94` | `9.87` | `12.86` | `491/293`, prioritized `317` | `0/0` | pump partial, still not playable |
| visible priority + parallel mid + exact parallel | walk | `59.55` | `44.41` | `11.55` | `10.01` | `22.83` | `10.75` | `10.14` | `465/287`, prioritized `311` | `0/0` | best new walk raw, still far over budget |
| visible priority + parallel mid + exact parallel | high-alt | `75.05` | `41.20` | `13.51` | `8.26` | `19.42` | `8.42` | `11.89` | `1460/0` | `0/0` | rejected as global stack; high-alt regression |

Decision:

- Keep `VISIBLE_PRIORITY_PUMP` default-off as an ownership-lane ordering primitive and diagnostic. It proves projected-visible prioritization can reduce walk CPU/clip/pump work.
- Do not call it a playable candidate. Raw frame time remains far over budget, and the parallel mid/exact stack regresses high-alt.
- Do not promote parallel mid pump or exact parallel generation as a global candidate. They are useful measured branches but not safe enough across the matrix.

Updated blocker:

- Walk still has sampled/projected-visible fallback-unknown mid debt plus exact request/generation/surface and GPU costs.
- The next real implementation remains the ownership-aware streaming state machine, but this branch provides a concrete first lane operation: projected-visible bricks can be prioritized without weakening ownership.
- The next code slice should stop doing per-frame broad queue surgery in `main_launcher.cpp` and move lane classification into `SparseClipmapTileCache`: persistent per-brick lane tags, visible/cache backlog age, and separate critical/cache pump budgets with no default behavior change.

### Streaming Playability Candidate Stack - 2026-06-04

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- Release build passed after reverting a rejected priority-side-effect patch.
- ctest passed `1/1`.
- Known `rayDir` shadow warnings remain.

Harness change:

- `perf_noncapture_smoke.ps1` adds `-WalkFixedDtMs`, default `0`, for deterministic walk comparisons.

Accepted default-off stack, not defaulted:

- playable render scale plus background split `0.375`
- clean terrain prefetch throttle
- backlog-aware mid pump
- incremental pressure trim, scan budget `8192`
- parallel visible mid-voxel pump, `4` workers
- diagnostics enabled during validation: CPU detail, drain/reuse, fallback classifier/contract/Far-SVO proof, missing sample feedback, visible-critical prepump

Fresh baseline versus accepted stack:

| Scenario | Baseline raw/CPU | Accepted raw/CPU | Accepted req/gen/clip/pump/GPU | Missing/sampled | Miss/unsafe |
|---|---:|---:|---:|---:|---:|
| fixed | `46.41/14.23` | `27.45/23.00` | `4.88/17.30/0.81/0.00/8.08` | `0/0` | `0/0` |
| walk realtime | `152.99/82.44` | `62.73/43.05` | `16.06/16.83/10.14/7.60/16.56` | `444/284` | `0/0` |
| high-alt | `93.16/51.47` | `51.50/26.41` | `4.67/7.08/14.65/6.39/11.79` | `1451/0` | `0/0` |

Fixed-dt walk:

- baseline raw/CPU `158.68/137.57`
- incremental trim raw/CPU `71.10/51.18`
- incremental trim + parallel mid pump raw/CPU `60.24/41.75`

Rejected in this continuation:

- streaming lane scheduler: catastrophic walk row, raw `141.88`, CPU `230.37`; code removed.
- priority-side-effect removal patch: worsened walk CPU to `133.29`; reverted.
- incremental trim + parallel surface: rejected, walk raw `103.20`, CPU `68.86`.
- incremental trim + cache-only defer: rejected as global stack, walk raw `128.87`, despite high-alt partial.
- parallel mid worker column cache: rejected, walk raw `73.62`, CPU `71.99`.
- exact parallel on accepted stack: no material win, walk raw `62.62`, CPU `41.76`.
- surface sort cache: rejected, walk raw `75.97`, CPU `53.10`.
- hidden exact tracked scan budget: rejected, walk raw `63.60`, high-alt coverage dropped to `86`.
- surface-ready publish queue: rejected, walk raw `88.45`, CPU `72.39`.
- terrain-critical inline surface defer: rejected as global stack, walk raw `76.12` despite high-alt raw `45.19`.

Decision:

- Completion B state: strongest validated default-off candidate and exact remaining blocker table, not full 60 FPS.
- No defaults were promoted.
- Candidate visual captures passed smoke/contact-sheet generation. High-alt may still show the older bright shoreline/terrain artifact class; this stack did not fix that correctness debt.

Remaining blockers:

- fixed: exact/general generation about `17.30 ms`;
- walk: request `16.06`, generation `16.83`, clip `10.14`, surface extraction `11.88`, GPU `16.56`, and sampled missing mid debt remains fallback-unknown/invalid;
- high-alt: improved, but clip/pump `14.65/6.39`, GPU `11.79`, and broad unsampled cache debt remain.

Next implementation:

1. Move persistent visible/cache/prefetch ownership lanes into `SparseClipmapTileCache` / streaming state.
2. Add request-prep/touch cache by source/lane; do not repeat the rejected fast-resident coverage-collapse path.
3. Split surface extraction/apply budgets by visible-critical versus cache/background.
4. Measure the accepted stack with diagnostics disabled; current rows include diagnostic overhead.

### Terrain-Critical Parallel Generation Retained Partial - 2026-06-04

Active goal: `streaming_playability_real_fix_campaign_20260604`.

Build/test after retained code and rejected-probe removal:

- `.\build.ps1 -Config Release`: passed, with known `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained default-off candidate:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MAX_WORKERS=4`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_PARALLEL_GENERATION_MIN_BRICKS=16`

Implementation shape:

- `SparseVoxelWorld::PumpGenerationForCoordsParallel` batches same-frame terrain-critical exact sparse brick payload generation on worker threads.
- Workers use worker-local terrain column caches.
- Main thread still applies generated payloads through existing resident/upload/surface paths.
- This is not async deferral and does not weaken ownership.

Artifacts:

- retained timing: `build/captures/candidate_terrain_critical_parallel_generation_min16_20260604`
- retained full diagnostics: `build/captures/candidate_terrain_critical_parallel_generation_min16_diagnostics_20260604`
- rejected resident-touch cache: `build/captures/candidate_request_resident_touch_cache_20260604`
- rejected surface visible-lane pump: `build/captures/candidate_surface_visible_lane_pump_20260604`
- scenario-specific background scale `0.25`: `build/captures/candidate_bgscale025_parallel_gen_20260604`
- scenario-specific mid interest interval `2`: `build/captures/candidate_mid_interval2_parallel_gen_20260604`

Full-diagnostic retained rows:

| Scenario | Frame | Raw ms | CPU ms | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe | Parallel generated |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.85` | `19.36` | `3.67` | `14.98` | `0.71` | `0.00` | `18.00` | `6.31` | `0/0` | `0/0/0` | `100` | `0/0` | `0` |
| walk realtime | `600` | `50.55` | `50.79` | `15.27` | `14.97` | `20.53` | `9.16` | `11.21` | `19.30` | `455/254` | `0/130/325` | `94` | `0/0` | `33` |
| high-alt | `360` | `47.65` | `26.71` | `5.72` | `6.55` | `14.43` | `4.05` | `3.95` | `9.96` | `1759/0` | `0/1238/521` | `100` | `0/0` | `0` |

Decision:

- Keep terrain-critical parallel generation as a default-off partial candidate.
- It is not a playable candidate. Walk still sits around `50 ms` raw with CPU/GPU distributed work.
- Resident-touch cache and surface visible-lane pump were rejected and removed.
- Background scale `0.25` and mid interest interval `2` are scenario-specific evidence, not accepted global stack changes.
- No defaults changed.

Current blocker:

- fixed can hit near-20 ms rows but still has exact generation and surface burst risk.
- walk realtime is still representative: request about `15 ms`, generation about `15 ms`, clip about `20 ms`, surface extraction about `11 ms`, GPU about `19 ms`, and sampled missing mid debt is fallback-invalid/unknown.
- high-alt improved but still has broad cache debt and clip/pump work.

Next action:

- Stop spending branches on local caps or queue ordering.
- Build persistent ownership lanes in streaming state:
  - visible/current ownership lane remains sync/guarded;
  - cache/prefetch lane gets persistent backlog and async CPU payload generation;
  - apply/upload/surface extraction are budgeted by lane;
  - request-prep/touch work is cached by lane/source;
  - sampled fallback unknown must become explicit valid/invalid before movement debt can move async.

### Streaming Lane Queue Priority Branch - 2026-06-04

New retained default-off branch:

- `VENPOD_SPARSE_STREAMING_LANE_QUEUE_PRIORITY=0`
- `SparseVoxelWorldConfig::streamingLaneQueuePriority`
- `SparseVoxelWorldStats::streamingLaneQueuePriorityActive`
- `perf_noncapture_smoke.ps1 -StreamingLaneQueuePriority`

What changed:

- exact sparse generation/upload/surface queues already tracked `SparseStreamingLane`, but queue scores ignored it;
- when enabled, higher lanes (`publicCritical`, then `visible`, then `prefetch`, then `cache`) sort ahead within the same residency class;
- no work is dropped, no coverage/critical guard is lowered, and unknown fallback is still critical;
- default behavior is unchanged.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` warnings remain.

Artifacts:

- accepted diagnostics: `build/captures/candidate_streaming_lane_queue_priority_diagnostics_20260604`
- campaign summary: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- campaign table: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- decision table: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- contact sheet: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png` (log-only; noncapture harness emitted no BMPs)

Retained stack rows:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing/sampled | Fallback valid/invalid/unknown | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` | `0/0` | `0/0/0` | `100` | `0/0` |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` | `447/248` | `0/93/354` | `95` | `0/0` |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` | `1755/0` | `0/1235/520` | `100` | `0/0` |

Decision:

- keep lane queue priority as a default-off safe partial;
- it improves representative walk CPU versus the prior terrain-critical retained row (`50.79 -> 39.44 ms`) while preserving coverage and miss/unsafe;
- it is not a playable candidate; walk raw remains about `54 ms`, fixed remains about `20 ms`;
- no defaults were promoted.

Rejected after this branch:

- parallel surface extraction: no material win; fixed regressed;
- parallel exact generation: walk CPU improved but raw worsened and high-alt regressed;
- mid worker column cache: severe walk/high-alt CPU regression;
- direct footprint columns: severe request/generation regression;
- mid pump `8` workers: more overhead and worse CPU/raw;
- background pass scale `0.25`: GPU dropped but CPU/raw exploded;
- general generation budget `8`: worsened all scenarios;
- buried-solid surface fast path: worsened walk/high-alt heavily.

Current remaining blocker:

- fixed is close but still over 16.67 ms because generation/surface bursts remain;
- walk realtime is distributed across request, generation, clip, surface extraction, GPU, and sampled fallback-invalid/unknown mid debt;
- high-alt is stable but still has broad unsampled cache backlog and clip/pump/GPU cost.

Next implementation:

- build persistent ownership lanes in streaming state rather than another local cap:
  - lane-tag clipmap interest and exact requests at creation time;
  - keep sampled visible fallback-invalid/unknown work synchronous/guarded;
  - queue cache/prefetch separately with persistent backlog and age;
  - worker-generate cache/prefetch payloads with generation/edit stamps;
  - apply/upload/surface-extract by lane with separate budgets;
  - add owner metadata/feedback only to convert sampled unknowns into explicit valid/invalid.

### Continuation Probes After Lane Priority - 2026-06-04

Build/test after rejected prototype removal:

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

- transient persistent mid lanes were implemented, tested, and removed. It was a simple persistent queue-ordering prototype, not a full ownership-lane state machine. Walk raw moved `59.00 -> 55.18`, but walk CPU regressed `35.16 -> 42.54`, fixed regressed `22.24 -> 24.05`, and high-alt regressed `49.12 -> 49.99`.
- `RequestFastResidentTouch` did not reduce request cost; walk request/CPU regressed to `16.78/44.03`.
- `SurfaceClassSortCache` regressed high-alt to raw/CPU `61.88/37.21`.
- `SurfaceGeneralStrictBudget` moved cost into clip/CPU; walk raw/CPU regressed to `60.86/51.73`.
- `DirectExactGeneration` improved fixed CPU but worsened raw and representative rows; walk raw/CPU `59.11/50.10`, high-alt raw `54.51`.

Decision:

- No new candidate was retained in this continuation.
- All defaults remain unchanged.
- The simple persistent queue-ordering idea is rejected; the needed fix is a broader ownership-lane state machine with lane-specific generation/apply/upload/surface budgets.
- Current completion state remains a strong default-off partial stack, not validated 60 FPS.

### Campaign Continuation - Persistent Terrain Column Cache Rejected - 2026-06-05

Build/test after rejected prototype removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/terrain_column_cache_rejected_20260605.md`

What was tried:

- a default-off persistent terrain column cache intended to reduce exact generation and surface extraction by keeping deterministic terrain column height/relief data across frames;
- capped variants at `262144` and `65536` entries;
- no ownership or fallback policy was changed.

Decision:

- rejected and removed from code;
- no `VENPOD_SPARSE_TERRAIN_COLUMN_CACHE_*` env knob remains;
- fixed and high-alt regressed or became unstable, while the best walk row still stayed far from playable;
- all defaults remain unchanged.

Key rows:

| Variant | Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract/stage | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| retained publish-lane baseline | fixed | `384` | `21.09` | `20.59` | `3.93` | `15.92` | `0.72` | `0.00` | `19.52/1.94` | `6.45` | reference |
| retained publish-lane baseline | walk realtime | `600` | `74.08` | `49.25` | `18.30` | `17.66` | `13.25` | `10.64` | `13.07/3.34` | `18.68` | reference |
| retained publish-lane baseline | high-alt | `373` | `46.60` | `22.84` | `4.78` | `5.91` | `12.14` | `5.97` | n/a | `11.68` | reference |
| persistent column cache `65536` | fixed | `384` | `24.50` | `23.69` | `3.91` | `18.96` | `0.81` | `0.00` | `25.66/n/a` | `7.20` | rejected fixed regression |
| persistent column cache `65536` | walk realtime | `600` | `50.73` | `47.94` | `14.07` | `16.81` | `17.04` | `4.45` | `12.59/3.02` | `10.68` | rejected partial/mixed |
| persistent column cache `65536` | high-alt | `395` | `43.95` | `24.93` | `3.28` | `6.49` | `15.16` | `4.45` | n/a | `10.28` | rejected CPU/clip regression |

Next measured branch:

- inspect request planning/cache behavior before trying more terrain-generation micro-caches;
- current rows repeatedly show material request cost and `centerDelta=0/0/0 fullRebuild=1`, so determine whether request sets are being rebuilt despite unchanged request keys;
- if true, implement a behavior-preserving request reuse path keyed by camera/request center, policy, forward cone, budgets, and diagnostic flags;
- do not drop requests, do not defer sampled fallback-invalid/unknown work, and do not weaken coverage/critical guards.

### Campaign Continuation - Request-Side Existing Flags Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_side_existing_flags_rejected_20260605.md`

Tested existing default-off flags:

- `VENPOD_SPARSE_PRESSURE_TRIM_FREE_PAGE_GUARD=1`
- `VENPOD_SPARSE_HIDDEN_EXACT_TRACKED_SCAN_BUDGETED=1`

Decision:

- neither flag is retained as a playable candidate stack member;
- pressure-trim guard reduced request time in some rows, but no-heavy walk regressed to raw/CPU `74.33/52.83` because clip/pump grew to `23.48/21.22`;
- hidden tracked scan budgeting did not reduce the representative walk row (`74.03/47.07`) and still hit scan budgets;
- no code changed for these probes and all defaults remain unchanged.

Important correction:

- `PERF_SPARSE_REQUEST_DETAIL otherMs` is not an exclusive bucket; it subtracts hierarchy and stats only, so terrain-critical, pressure-trim, hidden-exact, and other sub-costs can overlap in interpretation.

Current best retained reference remains the lane-priority/default-off partial:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `19.88` | `19.79` | `3.75` | `15.31` | `0.72` | `0.00` | `18.08` | `6.66` | close but not 60 FPS |
| walk realtime | `600` | `53.71` | `39.44` | `11.17` | `11.99` | `16.26` | `5.42` | `10.90` | `15.70` | representative blocker |
| high-alt | `360` | `47.78` | `26.47` | `5.80` | `5.95` | `14.72` | `4.33` | `3.74` | `11.20` | broad cache/clip/GPU blocker |

Next implementation direction:

- stop chasing isolated request micro-flags;
- implement the ownership-aware streaming state-machine slice:
  - persistent visible-critical/cache/prefetch lanes;
  - sampled fallback-invalid/unknown work remains synchronous/guarded;
  - cache/prefetch work gets separate generation/apply/upload/surface budgets;
  - async only for cache/prefetch or CPU-proved fallback-valid work;
  - owner metadata/feedback is needed before sampled unknown fallback can become async.

### Campaign Continuation - Generation Lane Budget Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generation_lane_budget_rejected_20260605.md`

Decision:

- default-off generation lane budget was implemented, measured, rejected, and removed;
- no generation-lane-budget env knobs remain;
- safety counters stayed `0/0`, but representative walk regressed and the prototype did not create a playable candidate;
- all defaults remain unchanged.

Key rows:

| Scenario | Frame | Raw | CPU | Req | Gen | Clip | Pump | Surface extract | GPU | Missing | Coverage | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `360` | `15.59` | `21.10` | `4.13` | `16.20` | `0.76` | `0.00` | `18.96` | `5.90` | `0` | `100` | fixed raw good, CPU/gen not solved |
| walk realtime | `600` | `65.15` | `63.94` | `13.76` | `13.99` | `36.17` | `23.78` | `10.81` | `14.21` | `556` | `93` | rejected regression |
| high-alt | `360` | `44.25` | `28.32` | `6.58` | `5.35` | `16.38` | `5.34` | `3.09` | `9.57` | `2183` | `99` | not enough to retain |

Next branch:

- do not repeat simple lane caps;
- probe existing default-off parallel exact generation infrastructure because generation remains a repeated bucket;
- if it fails or shifts cost into upload/surface, reject it and move to request prep or surface extraction/staging.

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
- this was not the old frame-interval skip, but it failed for the same reason: delayed interest churn accumulated debt and forced synchronous catch-up;
- all defaults remain unchanged.

Key rows:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `24.88` | `30.04` | `22.96` | `27.63` | `0.84` | `0.88` | `0.00` | `0.00` | `0` | `0` | `100` | `100` | rejected fixed regression |
| walk realtime | `600` | `52.16` | `84.14` | `45.85` | `68.78` | `17.39` | `40.93` | `5.44` | `28.00` | `447` | `460` | `95` | `94` | rejected representative regression |
| high-alt | `360` | `50.05` | `48.71` | `29.79` | `31.91` | `16.39` | `16.87` | `5.36` | `5.65` | `2228` | `2196` | `99` | `99` | no useful CPU win |

Walk failure mechanism:

- baseline frame `600`: `newV/goneV=0/0`, `budgetReason=0`, `pumpBudgetMs=4.00`, generated `5` voxel bricks.
- candidate frame `600`: `newV/goneV=49/49`, `budgetReason=2`, `pumpBudgetMs=0.00`, generated `24` voxel bricks.
- coarser signature reuse skipped too long, then coverage emergency forced catch-up.

Current decision:

- do not repeat signature/interval skip variants;
- the safe version is true incremental scroll/delta reuse: apply entering/leaving brick changes as they occur instead of suppressing rebuilds;
- request-cache inspection after this showed request prep is already partly reusing terrain-critical rays and is mixed with pressure/hidden feedback overhead, so a blind request-cache patch is not the next obvious safe win;
- representative walk remains a mixed bucket: clip/interest/pump, exact generation, request prep, surface extraction/staging, and GPU all remain material.

### Campaign Continuation - Voxel Footprint Reuse Rejected - 2026-06-05

Build/test after removal:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_rejected_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/voxel_footprint_reuse_vsync0_20260605`

Decision:

- default-off voxel-footprint signature reuse was implemented, tested, rejected, and removed;
- no `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_FOOTPRINT_REUSE` env remains;
- no `-MidClipmapVoxelFootprintReuse` noncapture harness switch remains;
- all defaults remain unchanged.

Key rows:

| Scenario | Frame | Raw before | Raw candidate | CPU before | CPU candidate | Clip before | Clip candidate | Pump before | Pump candidate | Missing before | Missing candidate | Coverage before | Coverage candidate | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `24.88` | `25.05` | `22.96` | `22.64` | `0.84` | `1.25` | `0.00` | `0.00` | `0` | `0` | `100` | `100` | neutral fixed |
| walk realtime | `600` | `52.16` | `59.33` | `45.85` | `78.03` | `17.39` | `45.00` | `5.44` | `32.09` | `447` | `441` | `95` | `94` | rejected representative regression |
| high-alt | `360` | `50.05` | `53.72` | `29.79` | `34.47` | `16.39` | `18.82` | `5.36` | `5.92` | `2228` | `2228` | `99` | `99` | rejected high-alt regression |

Failure mechanism:

- candidate frame `600` had `newV/goneV=109/109`, `budgetReason=2`, generated `32` voxel bricks, and `pumpVoxel=32.09`;
- skipping/reusing the voxel footprint still accumulates entering/leaving debt, then coverage emergency forces synchronous catch-up;
- do not repeat signature/interval skip variants.

Next:

- the next real branch is true mid-clipmap incremental scroll/delta update: update entering/leaving brick changes as they occur and preserve resident/unchanged bricks by key/stamp;
- if that is too large, add a targeted `UpdateVoxelInterest` cost split that directly identifies candidate generation, terrain sampling, sort/emit, and backlog costs needed to implement the delta refactor.

### Campaign Continuation - Mid-Clipmap Interest Detail Timing - 2026-06-05

New default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_INTEREST_DETAIL=0`
- noncapture switch: `-MidClipmapInterestDetail`
- log: `PERF_SPARSE_MID_CLIPMAP_INTEREST_DETAIL`

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_timing_20260605.md`
- measured run: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/interest_detail_vsync0_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Rows with VSync off, background split, clean throttle, critical reuse, terrain critical parallel generation, surface extraction max `1`, incremental pressure trim, explicit source lanes, streaming lane queue priority, worker column cache, and interest detail:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `21.32` | `22.63` | `4.10` | `17.72` | `0.80` | `0.00` | frame log `23.37/2.05` | `7.40` | `0` | `100` | `0/0` |
| walk realtime | `600` | `60.84` | `38.38` | `16.54` | `14.86` | `6.95` | `5.10` | `11.13/2.61` | `19.44` | `415` | `95` | `0/0` |
| high-alt | `360` | `51.32` | `32.84` | `8.48` | `6.74` | `17.61` | `5.80` | `4.70/1.71` | `10.14` | `2184` | `99` | `0/0` |

Interest split:

| Scenario | Frame | Reuse | Line | Anchor | Sort/emit | Backlog | Diagnostics | Candidates | Emitted | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `1` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0` | `0` | interest not blocker |
| walk realtime | `600` | `1` | `0.00` | `0.00` | `0.00` | `0.00` | `0.00` | `0` | `0` | interest already reused; other buckets dominate |
| high-alt | `360` | `0` | `0.20` | `1.37` | `1.65` | `0.11` | `0.51` | `12522` | `9216` | construction only part of clip cost |

Decision:

- do not implement another interest-signature or interval skip;
- representative walk is now a mixed bucket: GPU, request, generation, surface extraction/staging, and smaller clip/pump;
- fixed is blocked by generation/surface/GPU, not mid-clipmap interest;
- high-alt still has mid debt, but `UpdateVoxelInterest` construction itself is not the dominant cost.

Next safe branch:

- inspect pump/generation/apply separation and surface extraction rather than skipping interest work;
- if mid-clipmap delta reuse is continued, it must be true entering/leaving update, not any signature/interval suppression.

### Campaign Continuation - Existing Branch Validation - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/existing_branch_validation_20260605.md`

Tested existing default-off branches against the current stack:

- `-ParallelMidVoxelPump`
- `-PostOpenSurfaceMaxMs 4 -SurfaceReadyPublishQueue -SurfaceReadyPublishPressure`
- `-RequestFastResidentTouch`

Baseline:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract/stage | GPU | Missing | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `21.32` | `22.63` | `4.10` | `17.72` | `0.80` | `0.00` | `23.37/2.05` | `7.40` | `0` | `100` |
| walk realtime | `600` | `60.84` | `38.38` | `16.54` | `14.86` | `6.95` | `5.10` | `11.13/2.61` | `19.44` | `415` | `95` |
| high-alt | `360` | `51.32` | `32.84` | `8.48` | `6.74` | `17.61` | `5.80` | `4.70/1.71` | `10.14` | `2184` | `99` |

Results:

| Branch | Fixed | Walk realtime | High-alt | Decision |
|---|---|---|---|---|
| parallel mid pump | raw `39.07`, CPU `27.63` | raw `78.06`, CPU `31.68`, pump fired `8/4/2.53 ms` | raw `51.50`, CPU `31.37` | rejected as stack member; partial CPU win but raw/fixed regress |
| post-open surface cap + ready queue | raw `23.92`, CPU `16.82`, pending `3233` | raw `89.93`, CPU `50.70`, pending `5153`, oldest age `525` | raw `54.55`, CPU `34.14`, pending `2433` | rejected; converts work into surface-ready debt |
| request fast resident touch | raw `22.51`, CPU `21.14` | raw `90.48`, CPU `52.88`, clip/pump `28.70/26.25`, missing `530` | raw `49.66`, CPU `29.82` | rejected; request win uncovers/causes clip catch-up regression |

Decision:

- these existing branches are not playable candidate stack members;
- all defaults remain unchanged;
- no correctness counters regressed in sampled rows, but frame time and backlog/debt behavior failed.

Next real implementation:

- implement a lane-aware streaming state-machine slice instead of more isolated flags:
  - visible/current ownership work separate from hidden/post-open repair and cache/prefetch surface work;
  - hidden/post-open repair surfaces need independent budget/backlog age/drain accounting;
  - request prep needs keyed/incremental request planning, not resident-touch-only bypass;
  - mid pump needs true entering/leaving delta reuse or async/cache-only generation after ownership proof, not queue caps.

### Campaign Continuation - Async Exact Prefetch Lane Stack - 2026-06-05

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`
- existing visual sheet for this campaign: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

New default-off slice:

- `VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME=0`
- `PERF_SPARSE_EXACT_ASYNC` now logs `maxEnqueue` and async enqueue/apply lane counts.
- `perf_noncapture_smoke.ps1` has `-AsyncExactPrefetchLane` and `-AsyncExactMaxEnqueuePerFrame`.

Decision:

- async exact generation can now enqueue only `Cache`/`Prefetch` streaming-lane work even when the residency class is `Visible`.
- `Visible` and `PublicCritical` streaming lanes still stay synchronous unless the older explicit visible-async flag is enabled, which was not used.
- validation rows kept `asyncEnqueuedLaneVisible=0` and `asyncEnqueuedLanePublic=0`; no visible/public ownership work was moved async.

Candidate stack tested with VSync off, playable render quality, background split `0.375`, clean throttle, backlog-aware pump, critical reuse, terrain critical parallel generation, surface extraction max `1`, incremental pressure trim, explicit source lanes, streaming lane queue priority, worker column cache, async exact prefetch lane, parallel mid pump, and request fast resident touch:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.87` | `18.01` | `4.25` | `12.99` | `0.76` | `0.00` | `6.26` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `72.81` | `35.76` | `15.36` | `14.19` | `6.20` | `4.35` | `5.54` | `451/273` | `95` | `0/0` |
| high-alt | `360` | `52.49` | `30.92` | `8.82` | `5.58` | `16.51` | `4.59` | `11.30` | `1746/0` | `100` | `0/0` |

12-frame walk-window comparison around frames `596-607`:

| Stack | Avg raw | Avg CPU | Avg request | Avg gen | Avg clip | Max raw | Max CPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| baseline stack | `79.78` | `48.63` | `10.81` | `8.15` | `29.65` | `90.19` | `61.03` | baseline |
| async prefetch lane | `74.92` | `42.59` | `13.16` | `7.01` | `22.40` | `98.91` | `68.74` | partial; raw spike remains |
| async prefetch + parallel mid | `71.17` | `39.11` | `13.73` | `8.91` | `16.45` | `81.72` | `49.47` | useful default-off companion |
| async prefetch + parallel mid + request fast touch | `64.32` | `32.90` | `12.38` | `7.17` | `13.33` | `72.81` | `40.91` | best current stack, not playable |
| plus parallel exact | `65.11` | `30.40` | `11.70` | `7.00` | `11.68` | `78.00` | `42.52` | mixed; better CPU/body, not best raw average |

Rejected/mixed follow-ups:

- async enqueue caps `8` and `16`: lane-safe, but lost most of the CPU/clip benefit.
- parallel surface extraction: high-alt raw improved, but fixed regressed badly (`36.83` raw) and walk max raw worsened.
- parallel exact generation: useful comparison and frame-600 improvement, but not accepted as the main stack because fixed and walk average raw were not better than the request-fast stack.

Current playability state:

- no validated 60 FPS candidate; even the best noncapture walk row is far above `16.67 ms`.
- the strongest current default-off stack is materially better but still blocked by mixed CPU buckets: request prep, exact generation, clip/pump, and high-alt mid debt.
- all defaults remain unchanged; background split, clean throttle, backlog-aware pump, async prefetch lane, request fast touch, parallel mid, bounded repair all remain opt-in/default-off.

Next measured blocker:

- implement a real lane-aware streaming state-machine slice instead of isolated toggles:
  - visible/public ownership lane stays synchronous/guarded;
  - cache/prefetch exact generation can be async and drained with apply/upload accounting;
  - request prep needs a keyed/incremental cache for resident touches and source lanes;
  - mid visible debt still needs either true ownership proof or faster synchronous generation; unknown fallback remains critical.
## Active Goal Compact Handoff - 2026-06-05

Use `active-goal-handoff.md` as the compact first-read resume file for the current campaign.

Active goal: `streaming_playability_real_fix_campaign_20260604`.

Accepted-state build/test after this cycle:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Hidden-exact branches attempted and removed:

- post-open hidden-exact prefetch lane: rejected; nearly all work remained critical/pressure and fixed/walk regressed
- hidden-exact direct parallel generation: rejected; fixed CPU/gen improved but walk/high-alt CPU regressed or stayed mixed

Fresh accepted-state baseline:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `10.26` | `1746/0` | `100` | `0/0` |

Current next action: target request/gen coupling or exact-generation pipeline architecture with staged worker/apply accounting. Do not retry hidden-exact post-open deferral or hidden-exact direct parallel generation as simple toggles.

### Campaign Branch Ladder Closure - 2026-06-05

Compact resume source: `active-goal-handoff.md`.

Post-cleanup status:

- `.\build.ps1 -Config Release`: passed before compaction after removing transient branches.
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

Current decision:

- the strongest default-off stack is still not playable: fixed about `23 ms`, walk about `58 ms`, high-alt about `51 ms`;
- isolated queue/cache/surface/trim/hidden-exact knobs have repeatedly moved cost or regressed representative rows;
- the next viable code path is an ownership-aware streaming state machine with persistent visible/public-critical, sampled-visible, hidden/post-open repair, cache, and prefetch lanes carried through request, generation, upload/apply, surface extraction, and publish queues;
- unknown fallback remains critical and must not be moved async.

### Generated-Lane Accounting Slice - 2026-06-05

Behavior-neutral code retained:

- `SparseVoxelWorldStats::generated*LaneBricksLastFrame`
- `PERF_SPARSE_GENERATED_LANES`
- generated-lane CSV columns in `perf_noncapture_smoke.ps1`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: no code whitespace errors, only existing LF-to-CRLF warnings.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generated_lane_accounting_v2_20260605`

Accepted-stack validation:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Queued gen lanes prefetch/visible/public | Generated lanes prefetch/visible/public | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.54` | `18.76` | `3.98` | `13.99` | `0.77` | `6.71` | `915/121/0` | `24/121/0` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `67.95` | `55.11` | `17.91` | `16.07` | `21.10` | `17.24` | `4929/142/47` | `15/88/47` | `523/303` | `94` | `0/0` |
| high-alt | `360` | `51.02` | `28.72` | `8.27` | `5.14` | `15.31` | `6.37` | `6968/11/8` | `20/11/8` | `1769/0` | `100` | `0/0` |

Decision:

- visible/public exact generation is the hard generation wall in representative rows;
- broader cache/prefetch async generation is not enough to solve the current accepted stack;
- sampled visible/public fallback-invalid or unknown work remains critical and cannot be deferred;
- next implementation should make visible/public exact generation faster without deferral, likely through a persistent synchronous worker pool or same-frame staged public-critical generation/apply path.

## Parallel Exact Std Execution Candidate - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_rejected_20260605.md`

Rows:

| Scenario | Baseline raw/CPU/gen/clip | Candidate raw/CPU/gen/clip | Decision |
|---|---:|---:|---|
| fixed | `24.54/18.76/13.99/0.77` | `25.13/20.86/15.13/0.75` | rejected |
| walk realtime | `67.95/55.11/16.07/21.10` | `72.46/46.34/12.53/20.45` | rejected; raw worsened |
| high-alt | `51.02/28.72/5.14/15.31` | `49.42/33.21/5.57/19.07` | rejected; CPU/clip regressed |

Decision:

- temporary `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION` was removed after validation.
- post-removal build and ctest passed.
- no std-execution symbol remains.
- all defaults remain unchanged.
- visible/public exact generation remains the current same-frame generation wall.

## Persistent Exact Workers Candidate - Partial Keep Default-Off - 2026-06-05

New default-off flag:

- `VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS=0`
- harness: `-ParallelExactPersistentWorkers`

Validation:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_2w_candidate_20260605`

Rows:

| Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `21.81` | `17.17` | `3.85` | `12.55` | `0.76` | `0.00` | `6.97` | `0/0` | partial keep |
| walk realtime | `67.23` | `32.40` | `13.18` | `12.82` | `6.38` | `4.25` | `20.19` | `0/0` | partial keep, not playable |
| high-alt | `51.21` | `28.80` | `8.71` | `4.74` | `15.34` | `4.20` | `13.16` | `0/0` | neutral |

Decision:

- keep the 2-worker persistent exact pool default-off as a partial candidate.
- do not use the 4-worker setting as the stack candidate; it regressed high-alt.
- no defaults changed.
- next blocker is no longer just exact generation; fixed still has surface extraction cost, walk has CPU plus GPU/frame-gap, and high-alt still has mid clip/pump plus GPU.

## Post-Open Surface Cap 1 ms - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_persistent_exact_2w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_rejected_20260605.md`

Decision:

- tested existing `VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS=1`.
- rejected because walk/high-alt regressed and fixed still carried a huge publish backlog.
- fixed frame `384` showed `queuedPublishes=8098` even with the cap.
- do not use cap-only surface throttling as a stack member.
- any future surface fix needs publish/backlog-aware architecture.

## Persistent Mid Workers and Trim Budget Probe - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS=0`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim8192_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim4096_walk_probe_20260605`

Build/test:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Rows:

| Candidate | Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| persistent mid workers | fixed f384 | `20.88` | `18.59` | `3.73` | `14.07` | `0.79` | `0.00` | `6.07` | partial keep |
| persistent mid workers | walk f600 | `79.36` | `39.97` | `16.56` | `15.27` | `8.12` | `5.95` | n/a | raw noisy |
| persistent mid workers | high-alt f360 | `48.43` | `28.12` | `8.37` | `4.87` | `14.87` | `5.04` | `8.59` | partial keep |
| interest interval 2 probe | walk f600 | `63.63` | `38.51` | `13.82` | `12.34` | `12.33` | `10.01` | `15.67` | mixed, not final |
| interest interval 2 probe | high-alt f360 | `49.54` | `19.61` | `7.96` | `4.81` | `6.83` | `4.55` | `9.99` | CPU win, raw not solved |
| trim budget 8192 | walk f600 | `67.32` | `39.04` | `10.79` | `13.19` | `15.03` | `12.66` | `15.58` | rejected |
| trim budget 4096 | walk f600 | `62.31` | `37.77` | `14.02` | `16.84` | `6.89` | `5.02` | `9.07` | probe only |

Decision:

- persistent mid workers are a safe default-off partial candidate.
- interest interval `2` and smaller pressure trim scan budgets are not validated stack defaults.
- trim scans were measured and reducible, but reducing them moved cost into terrain-critical hierarchy/generation/clip instead of producing a real frame-time win.
- next work should be a lane-aware generation/surface/publish architecture slice, not another isolated cap/thread/budget knob.

### Post-Compaction Branch Cleanup - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Rejected and removed:

| Branch | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| exact coordinate batch generation | `22.92/18.51` | `89.44/34.29` | `50.61/27.30` | rejected; fixed gen flat, surface worsened, walk raw regressed |
| pressure trim miss-feedback queued-only | `25.12/20.38` | `64.14/41.85` | `49.43/26.72` | rejected; local trim scan win did not move frame time |

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_coord_batch_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_miss_feedback_queued_only_candidate_20260605`

Cleanup:

- no `VENPOD_SPARSE_EXACT_COORD_BATCH_GENERATION`, `PumpGenerationForCoordsBatch`, `VENPOD_SPARSE_PRESSURE_TRIM_MISS_FEEDBACK_QUEUED_ONLY`, or `PERF_SPARSE_PRESSURE_TRIM_POLICY` symbols remain.
- no default behavior changed.

Decision:

- do not continue with isolated batching/trim gates.
- next implementation should be the ownership-lane streaming state-machine slice: public-visible, sampled-visible, hidden repair, cache, and prefetch lanes with separate generation/apply/upload/surface/publish budgets.

## Upload Lane Budget Candidate Rejected - 2026-06-05

Attempted and removed a default-off upload lane budget slice.

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
- removed harness switches/parser columns
- post-cleanup build and ctest passed

Decision:

- upload queue selection alone is not an accepted stack fix.
- future lane work must coordinate request, generation, upload/apply, surface extraction, and publish readiness instead of optimizing one queue in isolation.

## Protected Ticket Scheduling Continuation - 2026-06-05

Active goal:

- `streaming_playability_real_fix_campaign_20260604`

Retained default-off code:

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

Anti-stop rule for next resume:

- do not stop after adding another local counter or isolated queue cap
- next implementation must either coordinate stage pacing by ticket ownership lane or prove with captures why that slice cannot be made safe
- if the stage-pacing slice is safe but partial, immediately retest the combined playable stack and then move to the next measured bucket

## Streaming Ticket Stage Pacing Rejected - 2026-06-05

Attempted and removed a default-off page-publish stage pacing slice:

- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING_MIN_CAMERA_Y`
- `SparsePagePublishQueue::PopReadyByLanePriority`
- `SparsePagePublishQueue::PopReadyProtectedLane`
- harness switches `-StreamingTicketStagePacing` and `-StreamingTicketStagePacingMinCameraY`

Artifacts:

- broad candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_candidate_20260605`
- same-code control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_control_20260605`
- high-alt-only failed probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_highalt_only_candidate_20260605`

Window results:

| Row | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| same-code control | `23.88/53.43` | `83.12/108.59` | `65.15/93.08` | control |
| broad stage pacing | `26.77/62.45` | `86.76/206.33` | `54.86/83.41` | rejected |

High-alt-only stage pacing was also rejected. The parser did not emit summary files because the high-alt run missed the exact `PERF frame=400` parser gate, but the frame-end log shows late high-alt raw outliers: frame `403=87.09`, `404=90.58`, `405=75.97`, `406=87.04`, `407=107.13`.

Cleanup status:

- rejected symbols were removed from `SparsePagePublishQueue`, `main_launcher.cpp`, and `perf_noncapture_smoke.ps1`.
- `.\build.ps1 -Config Release`: passed after removal.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched cleanup files: no code whitespace errors, only existing LF-to-CRLF warnings.
- campaign `summary.csv` and `table.md` include the control and broad rejected rows.

Decision:

- publish selection/stage pacing alone is not the fix.
- do not reintroduce `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING`.
- the same-code control shows walk realtime is again the largest target-frame blocker: raw/CPU `83.47/65.82`, request/gen/clip/pump `18.98/16.92/29.90/27.55`.
- next measured branch should inspect and target walk clip/pump or request/generation debt in the retained protected-ticket stack, not page publish selection.

## Request Accounting Split and Pressure-Trim Guard Rejection - 2026-06-05

Retained behavior-neutral request accounting:

- `PERF_SPARSE_REQUEST_DETAIL` now logs `hierarchyOtherMs`, `preHierarchyMs`, true residual `otherMs`, old residual `legacyOtherMs`, and `pressureTrimPressure=free/gen/miss`.
- no default behavior changed.

Artifacts:

- request-accounting baseline: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_accounting_baseline_20260605`
- rejected pressure-trim guard: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_queued_scan_pool_guard_candidate_20260605`

Key baseline row:

- walk f600 raw/CPU `91.48/62.52`
- request/gen/clip/pump `28.51/25.28/8.70/6.03`
- surface extract/stage `14.41/3.82`
- GPU ray `23.75`
- request split: pressure trim `9.31`, terrain critical `6.67`, stats flush `3.75`, hidden exact `3.63`, true residual `3.88`

Tried and removed:

- `VENPOD_SPARSE_PRESSURE_TRIM_QUEUED_SCAN_REQUIRES_POOL_PRESSURE`
- harness switch `-PressureTrimQueuedScanPoolGuard`

Result:

| Row | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| request-accounting baseline | `36.34/11.83` | `91.48/62.52` | `511.69/41.06` | diagnostic; high-alt raw was a frame-gap outlier |
| queued-scan pool guard | `33.58/9.40` | `111.69/60.04` | `69.53/34.53` | rejected |

Reason:

- bad walk/high-alt frames were driven by miss-feedback pressure (`pressureTrimPressure=0/0/1`), so the generation-only queued-scan guard did not skip the queued scan.
- walk generation and raw time regressed.
- the branch was removed; only richer request-detail logging remains.

Next measured direction:

- do not continue with queued-trim gating as the main branch.
- request is now partly explained; largest remaining work is distributed across hidden-exact/miss-feedback request pressure, exact generation, surface extraction, GPU, and sampled visible mid debt.
- next safe branch should target hidden-exact/miss-feedback admission/surface coupling or generation/surface pipeline work, with default-off gating and no deferral of sampled unknown fallback.

## Async Low-Priority Apply / Persistent Mid Validation - 2026-06-05

Retained:

- `VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME=0` default.
- `-AsyncExactLowPriorityMaxApplyPerFrame` in `perf_noncapture_smoke.ps1`.
- async log/parser fields `lowPriorityMaxApply` and `deferredLowPriority`.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`

Validation summary:

| Candidate | Fixed Avg/Max Raw | Walk Avg/Max Raw | High-alt Avg/Max Raw | Decision |
|---|---:|---:|---:|---|
| control | `22.51/53.25` | `85.93/624.57` | `93.93/117.04` | control |
| cap8 | `23.76/53.87` | `69.72/96.42` | `58.57/73.27` | rejected; fixed had `504` deferred low-priority completions |
| cap32 | `23.33/52.65` | `65.01/78.52` | `55.64/75.46` | partial |
| cap32 + persistent mid | `23.00/51.02` | `60.90/71.20` | `56.00/63.54` | strongest partial |
| cap32 + persistent mid + exact2 | `23.05/51.79` | `72.74/106.83` | `57.91/67.92` | rejected |

Visual verdict:

- walk candidate does not show obvious white holes in sampled frames but remains coarse/blocky.
- high-alt still shows the known bright white shoreline/terrain artifact.

Decision:

- no validated playable or 60 FPS candidate exists.
- cap32 plus persistent mid workers is useful evidence but remains default-off.
- cap8 and persistent exact workers are rejected in this stack.
- all defaults remain unchanged.
- next pass should not be another scalar cap; it must address high-alt ownership/shoreline and the streaming state-machine bottleneck.

## Streaming Playability Hidden Exact / Cache Defer Follow-Up - 2026-06-05

Latest survival file:

- `active-goal-handoff.md`

Build/test:

- Release build passed.
- `ctest` passed `1/1`.

New default-off knobs:

- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE=0`

Artifacts:

- hidden repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_repair_lane_candidate_20260605`
- water repair: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_repair_lane_candidate_20260605`
- cache-only perf: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/cache_only_defer_candidate_20260605`
- cache-only visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_20260605/contact_sheet.png`

Decision:

- non-water hidden repair lane is ineffective for current rows because accepted post-open feedback is mostly water feedback.
- water-inclusive hidden repair lane lowers walk CPU but shifts cost into upload/post-wait and hidden repair debt; keep diagnostic/default-off.
- cache-only visible-critical defer improves high-alt CPU/clip (`54.59/22.30` raw/CPU at f400, clip/pump `6.94/0.01`) but does not affect projected-visible walk debt (`69.45/52.64` raw/CPU at f600).
- high-alt cache-only contact sheet still shows the large pale lower-screen terrain/shore band.
- no validated playable or 60 FPS candidate exists; all defaults remain unchanged.

## High-Alt Background Split Exact-Band Ownership Finding - 2026-06-05

Resume-critical finding for `streaming_playability_real_fix_campaign_20260604`:

- the high-alt pale lower-screen band is not primarily a mid-clipmap scheduling or cache-only defer bug.
- background reason mode over the band is dominated by `far_svo_unavailable_or_rejected`.
- lower-band rays are downward and cross the water plane at plausible terrain/water distances, so sky-like output there is not visually acceptable.

Root:

- lower-res background split clears sparse-near flag bit `8` unless `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1`.
- `PS_Raymarch.hlsl` uses bit `8` as `sparseSurfaceRaymarchFill`.
- `BackgroundHitAllowedByExactNear` blocks mid/Far-SVO/far-water ownership inside the exact/surface ownership radius unless the fill flag is set.
- therefore unstenciled exact-band pixels in the background pass can reject background/water owners and fall through to sky-like output.

Surface-fill probe:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` visually removed the pale high-alt lower band.
- frame `400` sky-like image percentage dropped from about `41.18%` to `20.51%`.
- far-water/water-context ownership increased and sky ownership dropped in the frame-400 layer logs.

Why this remains blocked:

- surface-fill introduces/preserves shader-unsafe exact-contract debt.
- high-alt frame `400`: `shaderUnsafeContractNonReady=198`.
- walk frame `600`: `shaderUnsafeContractNonReady=83`.
- walk remains slow even with surface-fill: raw/CPU about `62.13/49.03`.

Decision:

- keep surface-fill diagnostic/default-off.
- do not promote background split + surface fill as playable.
- next high-alt branch must validate ownership for split-fill pixels or improve exact/water readiness; do not keep tuning pump/cache caps for this visual artifact.
- walk/realtime remains a separate projected-visible streaming debt problem.

## Surface-Fill Exact Repair / Cached Water Proof - 2026-06-05

Retained default-off changes:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF=0`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR=0`
- deterministic-water proof cache keyed by brick and edit revision

Measured rows:

| Row | Raw/CPU | Request | GPU | Decision |
|---|---:|---:|---:|---|
| uncached high-alt f480 | `97.66/72.94` | `49.46` | `30.42` | rejected CPU spike |
| cached high-alt f480 | `57.03/32.60` | `16.95` | `18.70` | useful partial, not playable |
| cached walk f600 | `77.12/185.37` | `142.32` | `26.69` | rejected movement stack |
| cached walk + hidden budget f600 | `122.00/46.32` | `22.82` | `39.97` | rejected raw/window regression |

Decision:

- cached water proof is useful, but surface-fill exact repair is not a validated playable candidate.
- all defaults remain unchanged.
- next work should validate cached high-alt visuals and then target remaining high-alt CPU/GPU, or pivot to projected-visible walk streaming debt.

## High-Alt-Only Surface Fill / Incremental Pressure Trim - 2026-06-05

Latest safe partial stack:

- background split at scale `0.375`
- playable render scale
- clean prefetch throttle
- high-alt-only surface fill with cached water proof and exact repair
- critical reuse, async exact prefetch lane, ticket/protected scheduling, explicit source lanes, fast resident touch
- parallel mid pump with persistent workers
- visible-critical prepump/cache-only defer
- incremental pressure trim + free-page guard

New default-off knob:

- `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY=0`

Rows:

| Scenario | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `25.18/19.28` | `5.04` | `13.38` | `0.84/0.00` | `8.21` | `27.40/60.28` | partial |
| walk f600 | `63.49/33.26` | `9.16` | `17.66` | `6.42/4.16` | `17.44` | `69.16/81.81` | partial |
| high-alt f480 | `55.10/24.35` | `7.99` | `9.38` | `6.98/0.00` | `17.57` | `57.96/75.45` | partial; visual plausible |

Rejected:

- global surface-fill exact repair for walk
- hidden-exact tracked scan budgeting for this stack
- surface-parallel extraction
- terrain-critical parallel generation
- background scale `0.25`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/playable_candidate_table.md`

Decision:

- no validated 60 FPS candidate exists.
- all defaults remain unchanged.
- high-alt-only surface-fill is safer than global fill but remains default-off.
- incremental pressure trim is a real default-off CPU win.
- next work should target the distributed remaining buckets instead of another cap: fixed generation/surface/post-wait; walk generation/surface/GPU/post-wait; high-alt residual GPU/post-wait/generation/surface.

## Campaign Continuation - Surface Lane Stage / Protected Ticket Rejection - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` warnings and trailing `vswhere.exe` message remain.

Rejected and removed:

- temporary lane-filtered surface extraction prototype:
  - removed `PumpSurfaceExtractionAroundTimedForClassAndLane`
  - removed `PopFirstQueuedBrickOfStreamingLane`
  - removed `VENPOD_SPARSE_SURFACE_LANE_STAGE_BUDGETS` plumbing and harness flags
  - no matching symbols remain in `src/main_launcher.cpp`, `perf_noncapture_smoke.ps1`, `SparseVoxelWorld.cpp`, or `SparseVoxelWorld.h`

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_zero_prefetch_probe`
- `build/captures/streaming_playability_real_fix_campaign_20260605/streaming_ticket_protected_walk_probe`
- updated `summary.csv`, `table.md`, and `playable_candidate_table.md` in `build/captures/streaming_playability_real_fix_campaign_20260605`

Rows:

| Branch | Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---|
| lane-stage budgets | fixed f384 | `18.77/19.47` | `4.18` | `14.49` | `0.78/0.00` | `19.52/1.99` | `5.75` | fixed-only partial |
| lane-stage budgets | walk f600 | `65.98/35.18` | `11.29` | `12.33` | `11.55/9.57` | `11.45/2.81` | `9.57` | rejected; raw/window worse than accepted stack |
| lane-stage budgets | high-alt f360 | `49.08/29.22` | `8.00` | `5.91` | `15.30/6.06` | `5.33/1.53` | `6.72` | not enough |
| zero-prefetch lane stage | walk f600 | `56.48/36.49` | `15.11` | `14.14` | `7.22/5.56` | `12.46/2.62` | `12.55` | rejected; visible surface work remains and window avg raw `60.96 ms` |
| protected ticket probe | walk f600 | `62.21/47.56` | `12.37` | `11.91` | `23.25/11.40` | `11.78/2.66` | `13.84` | rejected; clip/CPU worsened |

Current architecture conclusion:

- The code now has source lanes, publish lanes, upload lane fields, tickets, and queue sorting, but these are not enough.
- The effective state still collapses work into frame-critical generation/surface/clip/publish paths once movement produces sampled visible or visible-class debt.
- Surface or ticket caps shift debt into catch-up, gap, post-wait, or readiness failures.
- The next real slice is a coherent ownership-critical state machine, not another queue priority tweak.

Required next architecture slice:

1. Distinguish ownership-critical class from source/prefetch lane at request creation.
2. Preserve that class through CPU generation, GPU upload/apply, surface extraction, and page publish.
3. Keep sampled fallback-invalid/unknown bricks critical.
4. Let only cache/prefetch or CPU-proved fallback-valid work use async/budgeted lanes.
5. Maintain separate queues and budgets for generation, upload/apply, and surface extraction.
6. Validate with noncapture rows and terrain-critical capture checks before accepting any performance win.
### Ownership-Stage Queue Branch - 2026-06-05

Implemented default-off ownership-stage queues and budgets:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS=0`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET=8`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET=8`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS`

Validation:

- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_explicit_lanes_candidate`

Results:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| ownership queues | `44.05/14.51` | `117.65/79.63` | `98.90/60.35` | rejected |
| ownership queues + explicit source lanes | `48.94/12.13` | `90.49/80.23` | `84.48/49.17` | rejected |

Conclusion:

- The initial scan implementation was invalid because scan overhead dominated.
- Persistent ownership queues fixed that overhead but proved stage selection is not the current root: current work is mostly visible/critical, and explicit source lanes regress the stack.
- Next code should move ownership-critical classification earlier into request/admission and generation, not keep tuning upload/surface budgets.

## Deferred Downstream Observability - 2026-06-06

Added default-neutral measurement for low-priority `GeneratedCPU` downstream deferral:

- engine stats: pending total, pending cache/prefetch/visible/public, promoted last frame, stale last frame
- runtime log: `PERF_SPARSE_DEFERRED_DOWNSTREAM`
- smoke CSV: `deferredDownstreamEnabled`, `deferredDownstreamPending`, lane split, promoted, stale, generated total

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.

Artifact:

- `build/captures/deferred_downstream_observability_visible64_prefetch8_promote4_walk120_20260606`

Walk f120 result:

| Raw/CPU | Request | Gen | Clip/Pump | Surface | GPU | Window avg/max raw |
|---:|---:|---:|---:|---:|---:|---:|
| `70.38/95.11` | `20.22` | `17.26` | `57.59/46.40` | `11.41` | `7.28` | `72.20/126.89` |

Ownership/backlog result:

- generated lanes cache/prefetch/visible/public: `0/12/56/47`
- deferred downstream pending total: `865`
- deferred pending lanes cache/prefetch/visible/public: `0/759/23/83`
- promoted/stale: `0/0`
- `criticalMissing=184`, `nonCriticalMissing=120`, `surfaceReadyPublishPending=0`

Conclusion:

- The observability is good and should stay.
- The configuration is still rejected as a candidate.
- The important discovery is that cache/prefetch deferred `GeneratedCPU` payloads can later become visible/public while still deferred, and the current downstream promotion path does not drain them in the measured window.
- Next architecture work should make downstream promotion ownership-aware across upload, surface, and publish instead of continuing local generation-share and promote-count tuning.

## Critical-Touch Deferred Downstream Promotion - 2026-06-06

Implemented:

- `PromoteDeferredGeneratedDownstreamForOwnership(...)`
- `PromoteDeferredGeneratedDownstreamForCoord(...)`
- `PromoteDeferredGeneratedDownstreamIfCritical(...)`
- critical residency/lane touch promotion for deferred `GeneratedCPU`
- ownership-critical upload pop promotion before selecting upload work
- exact-coordinate upload rescue for deferred `GeneratedCPU`
- ownership alias cleanup in exact-coordinate upload
- post-upload `PERF_SPARSE_DEFERRED_DOWNSTREAM phase=postUpload`

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.

Primary artifact:

- `build/captures/deferred_downstream_critical_touch_promote_visible64_prefetch8_promote4_walk120_20260606`

Primary f120:

| Raw/CPU | Request | Gen | Clip/Pump | Surface | GPU | Window avg/max raw |
|---:|---:|---:|---:|---:|---:|---:|
| `80.14/69.29` | `17.37` | `13.62` | `38.29/37.07` | `9.95` | `13.61` | `73.23/100.33` |

Primary backlog:

- deferred pending total: `698`
- deferred pending cache/prefetch/visible/public: `0/698/0/0`
- promoted/stale: `2/0`
- `criticalMissing=103`, `nonCriticalMissing=202`

Ownership-stage artifact:

- `build/captures/deferred_downstream_critical_touch_ownership_stage_promote4_walk120_20260606`

Ownership-stage f120:

| Raw/CPU | Request | Gen | Clip/Pump | Surface | GPU | Window avg/max raw |
|---:|---:|---:|---:|---:|---:|---:|
| `82.89/33.53` | `20.29` | `12.37` | `0.85/0.00` | `10.64` | `6.25` | `97.21/161.59` |

Ownership-stage backlog:

- deferred pending cache/prefetch/visible/public: `0/9/0/0`
- `criticalMissing=0`, `nonCriticalMissing=0`
- ownership-stage upload critical/noncritical/nonBudget: `14/8/8`
- queued upload/surface remain prefetch-heavy: `663/375`

Conclusion:

- The visible/public deferred `GeneratedCPU` correctness issue is fixed by critical-touch promotion.
- This is not a playable/60 FPS candidate. The primary stack is still roughly `13-14 FPS`; ownership-stage is correctness-clean at f120 but has bad window instability.
- Next work should preserve this correctness rule and reduce prefetch-heavy upload/surface/publish/post-wait backlog.

## Ownership-Stage Publish Budget Scaffold - 2026-06-06

Added default-off publish ownership selection:

- `SparsePagePublishQueue::PopReadyForOwnershipCritical(...)`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_PUBLISH_BUDGET`
- smoke flag `-OwnershipStagePublishBudget`
- ownership-stage publish loop drains critical publishes first and caps noncritical publishes.
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS` now logs `publish=critical/noncritical/nonBudget`.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` warnings remain.

Measured rows:

| Artifact | Upload/surface/publish | f120 raw/CPU | crit/noncrit | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---|
| `ownership_stage_noncritical0_walk120_20260606` | `0/0/unbounded` | `69.11/34.62` | `109/119` | `70.18/191.04` | rejected; correctness loss |
| `ownership_stage_upload8_surface0_walk120_20260606` | `8/0/unbounded` | `87.97/41.56` | `10/11` | `100.75/182.98` | rejected |
| `ownership_stage_upload8_surface1_walk120_20260606` | `8/1/unbounded` | `104.82/42.69` | `10/1` | `108.14/196.38` | rejected |
| `ownership_stage_upload8_surface2_walk120_20260606` | `8/2/unbounded` | `120.07/50.11` | `0/0` | `122.56/214.47` | rejected |
| `deferred_downstream_critical_touch_ownership_stage_promote4_walk120_20260606` | `8/8/unbounded` | `82.89/33.53` | `0/0` | `97.21/161.59` | rejected |
| `ownership_stage_publish_budget8_walk120_20260606` | `8/8/8` | `77.89/136.99` | `8/0` | `96.06/206.59` | rejected |
| `ownership_stage_publish_budget0_walk120_20260606` | `8/8/0` | `79.72/152.09` | `0/0` | `99.81/198.04` | rejected |

Conclusion:

- Keep the default-off publish scaffold; it completes ownership-aware selection across upload, surface, and publish.
- No publish-budget stack is playable.
- Publish count alone is not the stall source. Even publish `0` leaves high clip/pump/post-wait and prefetch-heavy upload/surface debt.
- Next work should focus on mid-clipmap/clip pump interaction with prefetch-heavy downstream debt while preserving critical promotion and critical-first publish.

## Visible-Critical Mid Cache Defer Terrain Prefetch Throttle - 2026-06-06

Implemented a validated request-side throttle for terrain surface prefetch when the prior mid-clipmap prepump proves only cache backlog remains:

- carried previous-frame visible-critical mid proof into request phase
- skipped terrain surface prefetch when cache-only defer is active, visible-critical missing is `0`, visible-critical coverage is sufficient, and ownership miss/unsafe are `0`
- kept the legacy clean throttle's startup-open requirement unchanged
- added `terrainPrefetchMidCacheThrottle` to `PERF_SPARSE_CPU_DETAIL`

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` warnings remain.

Accepted artifact:

- `build/captures/ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_walk120_20260606`

Accepted f120:

| Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Decision |
|---:|---:|---:|---:|---:|---:|---|
| `34.51/26.27` | `19.33` | `4.76` | `2.17/0.00` | `1.03/0.50` | `1.04` | accepted partial, about `29 FPS` |

Correctness/proof:

- visible-critical mid: `missingVisibleCritical=0`, `coverageVisibleCritical=100`
- ownership: `missScreenPct=0`, `unsafeNearMissPct=0`
- terrain prefetch skipped: `terrainPrefetch=...:0.00/0/0/0/0/1`, `terrainPrefetchMidCacheThrottle=1`

Rejected:

| Artifact | Result | Decision |
|---|---:|---|
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle2_walk120_20260606` | `64.43 ms`, throttle `0` | rejected; startup gate blocked throttle |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_reuse_walk120_20260606` | `35.52 ms`, reuse `0` | rejected |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_startup_reuse_walk120_20260606` | `35.15 ms`, reuse `0` | rejected and reverted |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_noheavy_walk120_20260606` | `57.19 ms` | rejected; worse |

Current state:

- Best rough walk f120 FPS is now `~29 FPS`, not 60.
- The trajectory is good: prior visible-cache-defer baseline was about `75 ms` raw and was dominated by speculative terrain prefetch; that wall is now gone under visible-critical proof.
- Remaining request wall from accepted run: `reqMs=19.33`, with `terrainCriticalMs=10.97`, `hiddenExact=4.10`, `pressureTrimMs=1.87`, `statsFlushMs=0.94`.
- Next work should diagnose why terrain-critical ready-footprint reuse does not fire, then split terrain critical reuse, hidden-exact repair, and pressure trim into ownership-aware reusable/throttled paths without weakening public miss/unsafe checks.

## Terrain-Critical Reuse Frame-State Probe - 2026-06-06

Added default-off reuse instrumentation:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_FRAME_STATE`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_REPAIR_ONLY`
- smoke switches `-TerrainCriticalReuseFrameState` and `-TerrainCriticalReuseRepairOnly`
- log fields `reuseRepair`, `reuseReject`, `reuseCached`, `reuseBad`

Finding:

- Ready-footprint reuse state was effectively tied to runtime logging cadence. Frame-state maintenance fixes that, but the measured repair path is not yet a default candidate.
- At f120, the cached terrain-critical footprint can be large and mostly valid: `reuseCached=946`, often `reuseBad=1`.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Probe outcomes:

| Artifact | f120 raw | Request | Terrain critical | Verdict |
|---|---:|---:|---:|---|
| `terraincritical_reuse_frame_state_walk120_20260606` | `43.29` | `21.77` | `11.19` | rejected; one bad cached coord still forced full scan |
| `terraincritical_reuse_repair_only_walk120_20260606` | `62.97` | `12.93` | `0.50` | rejected; request win shifted to raw/post-wait |
| `terraincritical_reuse_repair_only_repeat_walk120_20260606` | `37.12` | `10.38` | `0.40` | mixed, not better than prior best |
| `terraincritical_reuse_repair_only_pressure_guard_walk120_20260606` | `61.03` | `11.62` | `0.00` | rejected |
| `terraincritical_reuse_repair_only_accounting_walk120_20260606` | `66.61` | `13.60` | `0.63` | rejected |
| `terraincritical_reuse_gated_baseline_repeat_walk120_20260606` | `38.01` | `19.51` | `10.65` | default-off sanity check |
| `terraincritical_gated_baseline_pressure_guard_walk120_20260606` | `39.84` | `18.07` | `10.46` | rejected |

Decision:

- Keep instrumentation and default-off switches.
- Do not enable repair-only terrain-critical reuse by default.
- Current best remains the prior terrain-prefetch throttle point: about `34.51 ms`, roughly `29 FPS`.
- Next target should be ownership/downstream accounting for cached-footprint repairs or hidden-exact repair cost, not another scalar reuse/pressure-trim sweep.

## Hidden-Exact Startup Deferral - 2026-06-06

Changed default `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP` from `1` to `0`.

Accepted rationale:

- At walk f120, hidden-exact was not a startup public-render blocker:
  - `hiddenExactBlocked=0`
  - `hiddenExactRepairBlocked=0`
  - `hiddenExactStartup=0`
  - `hiddenExactConverged=1`
- The old default still spent about `4 ms` on hidden-exact startup warmup with no accepted work:
  - `PERF_SPARSE_HIDDEN_EXACT_MISS frame=120 ms=4.06`
  - `waterProbeRays/candidates=9417/1518`
  - `feedback=0 accepted=0`
- The old behavior is still available through `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP=1`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted probes:

| Artifact | Frame | Raw | Request | Hidden exact | Correctness |
|---|---:|---:|---:|---:|---|
| `hidden_exact_startup_default_off_walk120_20260606` | 120 | `29.88 ms` | `15.44 ms` | `0.00 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |
| `hidden_exact_startup_default_off_walk240_20260606` | 240 | `26.76 ms` | `12.25 ms` | `0.00 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |

Rejected:

| Artifact | Result | Decision |
|---|---:|---|
| `hidden_exact_water_budget_walk120_20260606` | `126.66 ms` raw, hidden-exact `6.76 ms`, tracked water coords `340` | rejected; progressive water scan changed startup debt and regressed badly |

Current state:

- Best validated rough walk f120 FPS is now about `33.5 FPS` (`29.88 ms`), not 60.
- Walk f240 measured about `37.4 FPS` (`26.76 ms`).
- Remaining request wall is terrain-critical scan/accounting: f120 `terrainCriticalMs=10.90`, f240 `10.41`.

## Terrain-Critical Distance Default 1024 - 2026-06-06

Changed default `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE` from the terrain surface prefetch distance (`3072` in this stack) to `min(1024, sparseTerrainSurfacePrefetchDistance)`.

Rationale:

- Current exact foreground ownership is `exactNear=1024` and `surfaceRasterMax=1024`.
- Visible-critical mid/far proof is already clean at the accepted frames: `coverageVisibleCritical=100`, ownership miss/unsafe `0/0`.
- The old terrain-critical scanner was tracing exact terrain to `3072`, spending roughly `10.4-10.9 ms` in request even when no public-frame terrain misses remained.
- The env override still works for wider proof runs: `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE=3072`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted probes:

| Artifact | Frame | Raw | Request | Terrain critical | Correctness |
|---|---:|---:|---:|---:|---|
| `terraincritical_distance_default1024_walk120_20260606` | 120 | `22.51 ms` | `8.81 ms` | `4.67 ms` | distance `1024`, visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |
| `terraincritical_distance_default1024_walk240_20260606` | 240 | `20.59 ms` | `6.54 ms` | `4.83 ms` | distance `1024`, visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |

Additional direct-log check:

- `terraincritical_distance_default1024_walk420_20260606` exited cleanly at frame `428`, but the smoke summary failed because no `PERF frame=420` line was logged.
- Direct frame `420` evidence remained clean: `hiddenExactMissing=0/0`, visible-critical `100`, ownership miss/unsafe `0/0`.
- Direct frame `360` `PERF` line from that run: raw `20.36 ms`, about `49.1 FPS`.

Rejected / default-off:

| Artifact | Result | Decision |
|---|---:|---|
| `terraincritical_reuse_frame_state_hiddenoff_walk120_20260606` | `30.57 ms`, `reuseReject=0x800`, `reuseBad=1` | rejected; one bad cached coord still causes scan |
| `terraincritical_reuse_frame_state_hiddenoff_walk240_20260606` | `35.85 ms`, request `2.92 ms`, reuse `1` | rejected; raw/post-wait worsened |
| `terraincritical_reuse_frame_state_hiddenoff_repeat_walk240_20260606` | `28.84 ms`, request `2.68 ms`, reuse `1` | mixed, not better than default distance clamp |

Current state:

- Best validated rough walk f120 FPS is now about `44.4 FPS` (`22.51 ms`), still not 60.
- Walk f240 is about `48.6 FPS` (`20.59 ms`).
- Remaining f120 request wall: `reqMs=8.81`, `terrainCriticalMs=4.67`, `pressureTrimMs=2.23`, `statsFlushMs=0.68`, hidden-exact `0.00`.

## Terrain-Critical Grid Default 13x9 - 2026-06-06

Changed default terrain-critical screen proof rays from `21x13` to `13x9`; env overrides still work through `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RAYS_X/Y`.

Validation:

- `cmake --build build --config Release` through VS dev env: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted actual-default stack probes:

| Artifact | Frame | Raw | Rough FPS | Request | Terrain critical | Correctness |
|---|---:|---:|---:|---:|---:|---|
| `terraincritical_grid_default13x9_walk120_20260606` | 120 | `16.48 ms` | `60.7` | `5.71 ms` | `2.22 ms` | visible-critical `100`, miss/unsafe `0/0`, post nonready `0` |
| `terraincritical_grid_default13x9_walk240_20260606` | 240 | `17.15 ms` | `58.3` | `3.62 ms` | `2.03 ms` | visible-critical `100`, miss/unsafe `0/0`, post nonready `0` |

Rejected:

| Artifact | Result | Decision |
|---|---:|---|
| `pressure_incremental_default1024_walk120_20260606` | `20.30 ms` raw | rejected; local f120 improvement only |
| `pressure_incremental_default1024_walk240_20260606` | `26.81 ms` raw | rejected; later regression |
| `pressure_incremental8192_default1024_walk120_20260606` | `19.24 ms` raw | rejected; local f120 improvement only |
| `pressure_incremental8192_default1024_walk240_20260606` | `24.04 ms` raw, window avg/max `27.39/30.73` | rejected; shifted debt later |
| `terraincritical_grid_default11x7_stack_walk120_20260606` | `17.69 ms` raw | rejected; misses f120 target |
| `terraincritical_grid_default11x7_stack_repeat_walk120_20260606` | `17.73 ms` raw | rejected; repeat confirmed |
| `terraincritical_grid_default11x7_stack_walk240_20260606` | `15.49 ms` raw | not promoted; f240 win is not enough with f120 regression |

Current state:

- Current rough startup-held public-frame FPS: f120 `60.7`, f240 `58.3`.
- This is close to the 60 FPS target, but not a finished playable renderer because the rows are still startup-held (`mode=rebrun`) and broader unheld traversal/visual stability are not proven.
- Next work should target public-open and post-open motion debt while preserving visible-critical `100` and ownership miss/unsafe `0/0`.

## Startup Mid Visible-Proof Gate Probe - 2026-06-06

Added a default-off experimental gate:

- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF=0`
- held telemetry now includes `midVoxelVisibleProof=valid/coverage/missing/cache`

Finding:

- The held default f360 row has broad mid coverage `61/46`, but projected visible-critical proof is clean: `missingVisibleCritical=0`, `coverageVisibleCritical=100`, miss/unsafe `0/0`.
- Enabling visible proof releases startup around frame `101`, proving the previous blocker was broad cache completion rather than screen-visible ownership.
- The released moving path is not stable yet.

Validation:

- Build passed through VS dev env with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_mid_visible_proof_defaultoff_walk360_20260606` | `16.36 ms`, held, visible-critical `100`, miss/unsafe `0/0` | accepted default-off verification |
| `startup_mid_visible_proof_default_walk360_20260606` | opened around frame `101`; f360 `47.50 ms`, clip `37.31 ms`, GPU `9.90 ms`, visible-critical `94` | rejected as default; exposes post-open debt |
| `startup_mid_visible_proof_parallel_mid_walk360_20260606` | `55.28 ms`, parallel pump active but worse | rejected |
| `startup_mid_visible_proof_pumpcap8_walk360_20260606` | `52.73 ms`, visible-critical fell to `55` | rejected |

Next target:

- Build post-open mid-voxel scheduling that keeps projected visible-critical coverage while avoiding synchronous `30-40 ms` clipmap rebuild/pump frames.
- Do not promote the visible-proof gate until unheld traversal is clean and near 60.

## Split Visible Mid-Clipmap Pump Probe - 2026-06-06

Added default-off split pump knobs:

- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET`, default `8`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET`, default `0`

What changed:

- `SparseClipmapTileCache::PumpGenerationSplitVisiblePriority(...)` pumps visible-priority mid-voxel coords under their own cap, then optionally pumps cache coords while skipping remaining visible-priority coords.
- Split cache deferral is reported to the existing terrain prefetch throttle.
- This remains experimental and off by default.

Validation:

- Build passed through VS dev env with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_fixeddt16_walk360_20260606` | `61.46 ms`, pump `37.59 ms` | rejected; fixed dt did not solve it |
| `startup_visible_proof_splitvis8_cache0_walk360_20260606` | `57.50 ms`, pump `8.29 ms`, visible-critical `51` | rejected; too little visible catch-up |
| `startup_visible_proof_splitvis16_cache0_walk360_20260606` | `38.75 ms`, pump `0.00 ms`, visible-critical `100`, but unsafe exact nonready `140`, GPU ray `18.68 ms` | useful direction but not default-ready |
| `startup_visible_proof_splitvis16_exactrepair_walk360_20260606` | `80.86 ms`; exact repair worsened gen/surface cost and unsafe samples remained | rejected |

Current state:

- Current accepted default is still startup-held near-60, not playable unheld.
- Split visible pumping is promising because it avoids global cap starvation, but the next blocker after mid coverage is exact/surface/GPU readiness during open traversal.
- Do not promote the visible-proof gate until post-open exact/surface readiness is bounded and clean.

Follow-up open-path evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_cache0_walk420_20260606` | opened around frame `409`; f420/f424 moved path regressed to visible-critical `94`, f424 raw `55.83 ms`, pump `14.41 ms`, surface catchup `~28-31 ms` | rejected |
| `startup_visible_proof_splitvis24_postopen48_walk420_20260606` | paced exact catchup, but f424 visible-critical `93`, raw `79.24 ms`, pump `22.53 ms` | rejected |
| `startup_visible_proof_splitvis24_directfootprint_postopen48_walk420_20260606` | f420 raw `64.68 ms`, f424 raw `71.93 ms`, visible-critical `94` | rejected |
| `startup_forwardprefetch384_splitvis16_postopen48_walk420_20260606` | startup forward-prefetch experiment caused `~70-80 ms` held frames and still left moved coverage `94`; code removed | rejected |
| `startup_visible_proof_splitvis16_sharedcol_postopen48_walk420_20260606` | shared column cache also caused expensive held startup and did not fix moved coverage | rejected |
| `startup_visible_proof_splitvis16_postopen48_walk420_20260606` | exact catchup pacing alone left moved visible-critical near `94` and large hidden-exact backlog | rejected |

Next architecture:

- The real open-path wall is synchronous mid-voxel generation under movement.
- First moving frames create about `300` visible-priority mid misses; pumping `16-24` sync bricks costs `14-23 ms` and still cannot restore coverage fast enough.
- Next work should be an actual async/background mid-voxel generation lane or a moving-window cache that is built before public motion, not another global cap or startup scalar.

## Parallel Split-Visible Mid Pump Probe - 2026-06-06

Added default-off worker-backed split-visible mid-voxel generation:

- Existing `-ParallelMidVoxelPump` / `-ParallelMidVoxelPumpPersistentWorkers` now apply inside `PumpGenerationSplitVisiblePriority(...)` when safe.
- The path avoids shared column-cache mode and edited overlays.
- Slot allocation stays on the main thread; voxel fill uses the existing persistent/temporary worker code.
- Split-pump telemetry now reports `parallelPump=active/bricks/workers/wallMs`.

Validation:

- Build passed through VS dev env with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_parallel_postopen48_walk420_20260606` | f420 raw `82.58 ms`, CPU `40.73`, request `12.78`, generation `13.68`, clip `14.26`, pump `9.85`, surface `17.03`, GPU `17.93`, visible-critical `99`, miss/unsafe `0/0`; f424 raw `70.88 ms`, visible-critical `97`, parent-held failure, parallel pump `16` bricks/`4` workers/`8.71 ms` | rejected as default; useful architecture probe |

Current architecture verdict:

- The parallel split-visible branch is worth keeping default-off because it proves the split lane can use workers and reduces direct pump wall versus the synchronous version.
- It is not a 60 FPS path. The first moved public frames still create visible mid misses faster than the frame-local pump can resolve them, and the cost shifts into request/generation/surface/GPU work.
- Next work should move generation earlier than the public frame needs it: a true background/async mid-voxel ownership lane or a predictive moving-window cache with explicit publish/ownership barriers.

## Async Noncritical Mid-Voxel Staging Probe - 2026-06-06

Added a default-off async cache/noncritical mid-voxel staging lane:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0` remains default.
- Queue/apply caps:
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_QUEUE_MAX`, default `256`
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_ENQUEUE`, default `16`
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_APPLY`, default `16`
- Worker fills staged procedural mid-voxel payloads off-frame.
- Initial publish ran after interest update/edit invalidation and before visible-critical prepump coverage; the follow-up publish-barrier pass below moved apply behind the coverage/ownership checks.
- Edits disable async queueing; stale edit-revision results are discarded.
- If a pending cache coord becomes visible-critical, the synchronous visible lane can still generate it.
- Noncritical async publish does not evict resident clipmap slots when no free slot exists.
- Backlog telemetry now exposes mid async queue/result/pending and enqueue/complete/apply/drop/duplicate counters.

Validation:

- Build passed through VS dev env with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncnoncrit64_postopen48_walk420_20260606` | async active but f420 raw `64.59 ms`, visible-critical `88`; earlier async publishes shifted cost into clip/surface/GPU | rejected |
| `startup_visible_proof_splitvis16_asyncnoncrit64_noevict_postopen48_walk420_20260606` | no-evict publish guard; f420 raw `133.33 ms`, visible-critical `87`, clip `46.32`, async applied `46` | rejected |
| `startup_visible_proof_splitvis16_asyncnoncrit8_noevict_postopen48_walk420_20260606` | paced to `8`; f408 visible-critical `99`, but f420 raw `113.43 ms`, visible-critical `96`, parent-held failure; f424 raw `103.10`, visible-critical `97` | rejected as default |

Verdict:

- Keep the code default-off because it is a useful scaffold for true background ownership work.
- Do not treat it as a performance win. It moved generation off-frame but still publishes residency, dirty snapshot, and GPU upload debt into the public frame.
- Next pass should split async generation from async publish: publish only under a coverage-aware barrier, preferably favoring visible-critical completions and deferring cache completions until visible-critical `100` and ownership miss/unsafe remain clean.

## Async Noncritical Publish Barrier Probe - 2026-06-06

Changed the default-off async noncritical mid-voxel apply point:

- `ApplyAsyncNoncriticalVoxelGenerationCompletions(...)` now runs after visible-critical prepump/ownership state is known.
- Async cache/noncritical completions are applied only when visible-critical prepump is active, missing visible-critical is `0`, visual ownership failure is false, and prior retire miss/unsafe percentages are both `0`.
- When that barrier is closed, worker generation can continue staging payloads, but apply is capped to `0`.

Validation:

- `cmake --build build --config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncnoncrit8_publishbarrier_postopen48_walk420_20260606` | f420 raw `84.36 ms`, CPU `67.62`, request/gen/clip/pump `18.03/16.08/33.49/28.49`, GPU ray `16.66`, visible-critical `95`, missing visible/cache `268/3113`; f420 async result/pending `30/31`, apply `0`; f424 raw `107.25 ms`, visible-critical `94` | rejected as default |

Decision:

- Keep the publish barrier default-off with async noncritical generation as architecture scaffolding.
- Reject it as a candidate stack member. It proves cache publish can be blocked during unsafe visible-critical catch-up, but it starves cache publication and does not restore moving visible-critical coverage.
- Next work should split async results by ownership at publish time: visible-critical completions need their own high-priority safe publish path, while cache/noncritical completions remain barrier-gated until visible-critical coverage and ownership checks are clean.

## Async Visible-Critical Mid-Voxel Staging Probe - 2026-06-06

Added a default-off visible-critical async companion to the mid-voxel worker:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY`
- staged async requests/results now carry a visible-critical tag;
- apply has separate visible-critical and noncritical caps, and can skip/defer cache results while still applying visible-critical results;
- visible-critical async is gated to post-open public frames so hidden startup does not double the catch-up work;
- backlog telemetry logs `asyncVisible=enq/complete/apply/drop/dup`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncvisible16_postopen48_walk420_20260606` | ungated implementation queued visible async during hidden startup and doubled work; f420 raw `91.33 ms`, visible-critical `98`, `asyncVisible` applied `16` | rejected; fixed with post-open gate |
| `startup_visible_proof_splitvis16_asyncvisible8_postopen_gated_postopen48_walk420_20260606` | post-open gated, but split-pump early return prevented async queueing when sync visible budget was `0`; direct f420 `65.45 ms`, visible-critical `95` | rejected/inconclusive |
| `startup_visible_proof_splitvis16_asyncvisible8_postopen_gated_earlyfix_postopen48_walk420_20260606` | early-return fixed; harness missed target `PERF`, but direct f420 `PERF_FRAME_END` raw `71.77 ms`, body `61.71`, visible-critical `95`, parent-held failure `1`; f424 visible-critical `95` | rejected as default |

Decision:

- Keep the visible-critical async scaffold default-off because it is the correct priority split missing from the cache-only async lane.
- Do not promote it. It does not preserve moved visible-critical coverage or approach 60 FPS.
- Next work should move readiness earlier than the public open frame: predictive visible mid-voxel generation or moving-window ownership reservations before release, with downstream dirty/upload/surface work staged so public motion does not pay the burst.

## Startup Predictive Visible Mid-Voxel Proof Probe - 2026-06-06

Added a default-off startup predictive visible proof slice:

- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_PROOF=0`
- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_DISTANCE`, default `384`
- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_MAX_COORDS`, default `384`

What changed:

- While startup public render is held and visible mid-voxel proof is active, the prepump projection can append a deduped predicted visible footprint.
- The prediction uses flat camera forward plus the existing clipmap velocity prediction, then routes those coords through the same visible-priority proof/pump path.
- Prepump logs now include `predictiveVisible`.
- The path is default-off and diagnostic only.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_predictive384_splitvis16_asyncvisible8_postopen48_walk420_20260606` | gate opened at frame `391`; f420 raw `119.12 ms`, body `112.03`, CPU update `86.09`, request/gen/clip/pump `21.04/19.79/45.23/39.05`, GPU frame/ray `24.78/20.30`, visible-critical `95`, missing visible/cache `247/3416`, backlog voxel `3663`, `asyncVisible=8/8/8/0/0`; f424 raw `128.03 ms`, visible-critical `94`; post-open prepump logged `predictiveVisible=0` | rejected as default |

Decision:

- Keep the code default-off only as a diagnostic scaffold unless later cleanup removes it.
- Reject the naive predicted-view branch. It opened earlier and worsened both CPU and GPU frame cost without preserving moved visible-critical coverage.
- Next work should be a moving-window ownership readiness contract, not another scalar projection tweak: prove current plus several future camera footprints are generated, resident, uploaded/surfaced as needed, and ownership-clean before opening public motion.

## Moving-Window Mid-Voxel Readiness Probe - 2026-06-06

Added a default-off moving-window readiness scaffold:

- Startup gate proof:
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_PROOF=0`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_SAMPLES`, default `3`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_STEP_DISTANCE`, default `128`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_MAX_COORDS`, default `1024`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_SCRIPTED_WALK`, default `1`
- Post-open priority diagnostic:
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY=0`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY_SAMPLES`, default `3`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY_STEP_DISTANCE`, default `128`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY_MAX_COORDS`, default `1024`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY_SCRIPTED_WALK`, default `1`

What changed:

- Future projection now takes an explicit view basis, so scripted-walk samples can use future yaw/pitch instead of the current held-camera basis.
- Startup moving-window proof blocks visible-proof release until all sampled future footprints are clean.
- Optional post-open moving-window priority can keep tagging future-footprint coords after release.
- Prepump telemetry includes `movingWindow=samples/ready/missing/coords/coverage`; held telemetry includes `midVoxelMovingWindowProof`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_splitvis16_asyncvisible8_postopen48_walk420_20260606` | first straight-forward version opened at frame `403`; f420 raw `114.07 ms`, visible-critical `95`; pre-open window falsely reported `3/3` ready because it did not follow scripted walk | rejected; diagnostic flaw fixed |
| `startup_visible_proof_movingwindow3x128_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk420_20260606` | scripted-walk projection detected future misses while held: f396 `movingWindow=3/0/299/70/95`, f400 `3/0/211/45/96`; f420 held clean `3/3/0/0/100`, frame-end raw `70.19 ms` | useful proof, not open |
| `startup_visible_proof_movingwindow3x128_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | opened at frame `458`; f460 visible-critical `100`, but f480 raw `95.37 ms`, visible-critical `97`, missing visible/cache `159/2633`, request/gen/clip/pump `18.15/16.87/43.88/22.47`, GPU ray `15.81` | rejected as default |
| `startup_visible_proof_movingwindow3x128_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | post-open priority opened at frame `445` but f480 worsened to raw `115.04 ms`, visible-critical `95`, movingWindow `3/0/572/82/91`, main-thread brick gen `43.55`, GPU ray `23.56` | rejected |

Decision:

- Keep the moving-window code default-off as a measured scaffold.
- Do not promote the one-shot or continuous frame-local variants.
- Next work should build a paced reservation lane: future visible-critical coords enter a rolling queue, are generated/published/uploaded ahead of time, and public frames only consume completed reservations. The current immediate pump cannot carry both present and future ownership at 60 FPS.

## Async Moving-Window Reservation Lane Probe - 2026-06-06

Added a default-off async reservation class separate from current visible priority:

- `SparseClipmapTileCache::QueueAsyncVisibleReservationVoxelCoords(...)`
- `m_asyncVisibleReservationVoxelSet`
- startup env:
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION=0`
- post-open env:
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION=0`

What changed:

- Future moving-window coords can be queued as visible-critical async reservations without entering the current visible-priority sync pump.
- Async visible results are accepted if the coord is still current visible-priority or async-reserved.
- Startup reservation completions may apply while the public gate is still held.
- Prepump telemetry now reports `movingWindow=samples/ready/missing/coords/coverage/reserve/queued`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_asyncreservation_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | startup-only reservation opened at frame `412`; f480 raw `100.20 ms`, visible-critical `95`, missing visible/cache `242/2598`, request/gen/clip/pump `18.47/22.69/42.15/37.30`, GPU ray `28.75` | rejected |
| `startup_visible_proof_movingwindow3x128_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | continuous reservation queued future coords after open (`reserve/queued=86/16` at f420, `46/16` at f480) and `asyncVisible=8/8/8`, but f480 raw worsened to `118.92 ms`, visible-critical `95`, missing visible/cache `302/2342` | rejected |

Decision:

- Keep the separate async reservation set default-off as scaffold.
- Do not promote either reservation run. The shape is closer, but the per-frame reservation enqueue/apply budget is far below moving-footprint demand.
- Next step should be a persistent deadline-ordered reservation scheduler: reservations survive across frames, are ordered by earliest future sample, and drain/publish/upload ahead of public use. Future coords should not be reselected from scratch each frame or compete with current visible misses in the synchronous pump.

## Persistent Deadline-Ordered Async Reservation Scheduler - 2026-06-06

Implemented the next scheduler slice for default-off moving-window reservations.

What changed:

- `SparseClipmapTileCache` now stores moving-window async visible reservations in `m_asyncVisibleReservations`, keyed by coord with `firstFrame`, `lastSeenFrame`, `deadlineFrame`, and `sampleIndex`.
- Reservations persist across prepump frames instead of being cleared and rebuilt every frame.
- Reservation queueing prunes invalid/stale entries, sorts by earliest deadline/sample/first frame, and then queues visible-critical async work.
- `SetVisiblePriorityVoxelCoords(...)` no longer destroys future reservations.
- Async visible completions remain valid if the coord is current visible-priority or still present in the reservation map.
- Launcher moving-window projection now keeps per-sample reservation buckets and passes deadline metadata into the cache.
- `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_RESERVATION_STALE_FRAMES` controls reservation expiry; default `24`, latest tests used `36`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only LF-to-CRLF warnings.
- A redundant reservation-set version and an experimental async-dominant sync throttle caused `0xC0000409` fail-fast crashes; both were removed from the kept code path. The final map-only scheduler reran normally.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | f480 raw `156.96 ms`, visible-critical `93`, missing visible/cache `419/2290`, movingWindow `3/0/722/24/89/24/8` | rejected |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible16_postopen48_walk480_20260606` | f480 raw `108.49 ms`, visible-critical `99`, missing visible/cache `17/2333`, f484 visible-critical `100` | useful direction, not 60 FPS |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_maponly_continuous_scriptedwalk_splitvis16_asyncvisible16_postopen48_walk480_20260606` | final map-only validation opened `408`; f480 raw `144.89 ms`, visible-critical `98`, missing visible/cache `85/2244`; f484 visible-critical `99` | stable but rejected as default |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_asyncdominant0_*` | attempted sync-pump throttle; one stale run hit `76.60 ms` but starved generation, then the explicit current-visible async queue variant fail-fast crashed | removed |

Decision:

- Keep the persistent map-only scheduler default-off as the current architectural scaffold.
- Do not promote it yet. Cap-16 improves visible stability but not frame time.
- Next work should split visible-priority async enqueue from synchronous generation safely, then make sync visible generation emergency-only. After that, attack the remaining costs: surface/publish catchup, main-thread mid generation, GPU ray cost, and frame gaps.

## Repair Lane Classification And Bounded Admission - 2026-06-06

Implemented and retained a repair lane for hidden-exact post-open work:

- `Simulation::SparseStreamingLane::Repair`, ranked between `Prefetch` and `Visible`.
- Repair lane telemetry in request/generation/generated/upload/surface queues, async exact generation, deferred downstream, page publish, surface-ready publish, and ownership-stage budgets.
- `ClassifyStreamingTicketOwnership(Repair)` now maps to `HiddenRepair`.
- `HiddenRepair` stays noncritical for stage budgets, so public-critical and sampled-visible work keep the correctness path.
- `perf_noncapture_smoke.ps1` parses five-lane logs:
  `cache/prefetch/repair/visible/public`.

Rejected validation:

- Artifact: `build/captures/repairlane_enabled_pppublic480_20260606`
- Unbounded post-open repair lane was rejected.
- f480 raw/body `128.03/125.75 ms`, window avg/max raw `123.65/150.32 ms`.
- It flooded downstream visible-class queues: queued upload about `7960`, queued surface about `3371`.
- Decision: never enable the repair lane without bounded admission.

Retained bounded admission:

- Added caps:
  - `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE_MAX_REQUESTS`, default `16`.
  - `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE_MAX_REQUESTS`, default `min(8, repairMax)`.
- Added smoke/log fields:
  `hiddenRepairLimitSkips`, `hiddenRepairMax`, `hiddenRepairWaterMax`.
- The cap is checked after readiness/active filtering, so it applies to real unresolved repair candidates.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Artifact: `build/captures/repairlane_cap16_pppublic480_20260606`
- f480 raw/body `80.91/73.92 ms`, window avg/max raw `78.86/90.74 ms`.
- f480 request/gen/clip/pump/surfaceExtract/gpuRay:
  `12.95/13.75/39.21/16.94/10.63/5.80 ms`.
- f480 repair line:
  `accepted=8`, `repairAccepted=8`, `repairLimitSkips=71`, `repairMax=16`, `repairWaterMax=8`.
- f480 downstream queues:
  upload `388` total (`347` prefetch, `41` repair), surface `10` total, all repair.
- Public path stayed valid:
  `PERF_BACKEND_PIPE active=0x7FF`, `missScreenPct=0`, `unsafeNearMissPct=0`.

Decision:

- Keep repair lane classification, parser support, and bounded post-open repair admission.
- Do not claim this as a playable candidate. It repairs the failed repair-lane trajectory but still leaves public walking around `80 ms`, roughly `12-13 FPS`.
- Next measured blocker remains the public-open f456..484 window, especially clip/pump, main-thread generation, surface/publish catchup, GPU ray/surface, and post-wait/frame gaps.

## Mid-Clipmap Interest Reuse Validation - 2026-06-06

Validated an existing default-off/harness control against the bounded repair stack:

- `perf_noncapture_smoke.ps1 -MidInterestInterval 2`
- Environment equivalent: `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`
- Defaults remain unchanged.

Reason:

- Bounded repair baseline f480 showed `clipMs=39.21` with
  `clip=interest/reuse/pump:18.64/0/18.20`, `centerDelta=0/0/0`,
  and `fullRebuild=1`.
- The interest signature invalidates on fine camera/forward/velocity changes even when the emitted clipmap footprint barely changes.

Accepted validation:

- Artifact: `build/captures/mid_interest_interval2_repaircap_pppublic480_20260606`
- f480 body `72.72 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `13.76/12.35/15.99/11.54/11.54/10.25/16.16 ms`.
- f456..484 avg/max raw `69.60/93.86 ms`.
- f456..484 avg/max body `76.06/98.89 ms`.
- f480 `clip=interest/reuse/pump:1.39/1/12.80`.
- f480 visible-critical coverage/cache coverage `96/75`.
- f480 visible/cache misses `247/1957`.
- Public path stayed valid: `active=0x7FF`, `miss=0`, `unsafeNearMiss=0`.

Rejected validation:

- Artifact: `build/captures/mid_interest_interval4_repaircap_pppublic480_20260606`
- f480 body `73.64 ms`, `clipMs=36.01`, visible-critical coverage `93`.
- window avg/max raw `72.61/91.18`, avg/max body `81.79/95.34`.
- It rebuilt on f480 and worsened body/window stability, so do not carry interval 4 forward.

Decision:

- Carry `-MidInterestInterval 2` in the current strongest public-open test stack.
- This is not a 60 FPS candidate. Current public walking is roughly `70-76 ms`
  in the f456..484 window, about `13-14 FPS`.
- Next blockers after this accepted setting are GPU ray spikes, surface extraction,
  post-wait/frame gaps, and remaining sync mid pump cost.

## Footprint Interest Signature - 2026-06-06

Implemented a default-off footprint-aware interest reuse signature:

- `SparseClipmapConfig::footprintInterestSignature`
- `VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE`
- `perf_noncapture_smoke.ps1 -MidClipmapFootprintInterestSignature`
- Startup config log now includes `interestInterval` and `footprintSignature`.

Intent:

- Avoid rebuilding the full mid-clipmap interest set on sub-footprint camera,
  forward, or velocity jitter.
- Preserve the public ownership contract; this only changes when the clipmap
  interest set is considered reusable.

Rejected first footprint attempt:

- Artifact: `build/captures/mid_footprint_signature_repaircap_pppublic480_20260606`
- Velocity was still too fine in the signature.
- f480 body `75.38 ms`, clip `39.33 ms`.
- window avg/max raw `73.63/99.24 ms`, avg/max body `84.50/94.28 ms`.
- Rejected.

Accepted footprint-v2:

- Build/test:
  - `.\build.ps1 -Config Release`: passed, only existing `rayDir` shadow warnings.
  - `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Artifact: `build/captures/mid_footprint_signature_v2_repaircap_pppublic480_20260606`
- f480 body `66.79 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `13.93/12.02/20.57/15.61/15.61/7.82/5.54 ms`.
- f456..484 avg/max raw `66.07/83.52 ms`.
- f456..484 avg/max body `70.15/86.99 ms`.
- f480 `clip=interest/reuse/pump:1.52/1/16.92`.
- f480 visible/cache coverage `94/74`, visible/cache misses `366/1948`.
- Public path valid: `active=0x7FF`, `miss=0`, `unsafeNearMiss=0`.

Mixed combo result:

- Artifact: `build/captures/mid_footprint_signature_interval2_repaircap_pppublic480_20260606`
- f480 body `66.98 ms`, clip `26.90 ms`, pump `21.63 ms`.
- window avg/max raw `70.68/79.61 ms`, avg/max body `69.73/86.29 ms`.
- It improves max frame time slightly, but worsens average raw and target pump.
- Do not carry the combo as the strongest stack yet.

Decision:

- Carry `-MidClipmapFootprintInterestSignature` as the current strongest public-open stack setting.
- Do not carry `-MidInterestInterval 2` alongside it by default.
- Current public walking is roughly `66-70 ms` in the f456..484 window,
  about `14-15 FPS`, not close to 60 FPS.
- Remaining blockers: sync mid pump, request/gen, surface extraction, post-wait,
  and occasional GPU ray spikes.

## 2026-06-06 Post-Open Async Visible Split Pump

Implemented a default-off post-open-only gate for split visible pumping:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET`
- matching `perf_noncapture_smoke.ps1` switches.

Intent:

- Allow visible-critical async generation to carry current visible mid-voxel work
  after the public startup proof has opened.
- Keep normal synchronous startup proof pumping before open, because startup
  policy intentionally disables async visible generation while public render is held.

Rejected/invalid comparison:

- Artifact: `build/captures/mid_async_visible_split0_footprint_repaircap_pppublic480_20260606`
- The run omitted `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME=480`, so public
  render stayed held through f480.
- f480 body `156.38 ms`, active `0x5DF`, ownership off, surface raster off,
  mid-voxel visible proof invalid.
- It is useful only as evidence that split visible budget `0` before public open
  can starve the startup proof.

Accepted validation:

- Build/test:
  - `.\build.ps1 -Config Release`: passed, only existing `rayDir` shadow warnings.
  - `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Artifact:
  `build/captures/mid_async_visible_postopen_split0_footprint_repaircap_pppublic480b_20260606`

Accepted public-open row:

| Metric | Value |
|---|---:|
| f480 body | `56.29 ms` |
| f456..484 avg/max raw | `53.17 / 70.27 ms` |
| f456..484 avg/max body | `56.58 / 74.40 ms` |
| f480 request/gen/clip | `13.38 / 11.79 / 5.36 ms` |
| f480 pump/mainThreadBrickGen | `0.01 / 0.01 ms` |
| f480 surfaceExtract/gpuRay | `9.38 / 4.11 ms` |
| window avg/max postWait | `15.85 / 16.63 ms` |
| window avg/max surfaceExtract | `8.68 / 10.01 ms` |
| window avg/max sparseUpload | `1.93 / 3.00 ms` |
| f480 visible/cache coverage | `94 / 74` |
| f480 visible/cache misses | `354 / 1981` |
| f480 asyncVisible enq/complete/apply/drop/dup | `21 / 21 / 21 / 0 / 0` |
| f480 async worker cost / pending | `40.87 ms / 24` |

Ownership/public path:

- `PERF_BACKEND_PIPE frame=480` had `active=0x7FF`, ownership on, surface raster on.
- `PERF_RENDER_OWNERSHIP retireFrame=480` had `miss=0`, `unsafeNearMiss=0`.

Decision:

- Carry this as the strongest current default-off public-open walk stack:
  footprint interest signature plus post-open-only async visible split pump,
  split visible/cache sync budgets `0/0`, async visible enqueue/apply `24/24`,
  bounded repair lane, ownership/source-lane stack, and persistent parallel mid
  pump workers `4`.
- It supersedes the footprint-only row:
  `66.07/70.15 ms` avg raw/body -> `53.17/56.58 ms`.
- Rough current public walking is now about `18-19 FPS`, not 60 FPS.
- Do not activate split visible budget `0` before public open.

Next blockers:

- post-wait/frame gap around `16 ms`;
- surface extraction around `9-10 ms`;
- request/generation still around `12-13 ms` each;
- cache missing remains high around `1981`;
- async mid generation now hides main-thread pump cost but leaves pending `24`
  and about `40 ms` worker-side generation cost.

## 2026-06-06 Generic Incremental Resident Trim Extension

Implemented a default-off extension to the existing incremental pressure trim:

- `SparseVoxelWorld::TrimResidentBricks(...)` now uses the same incremental scan
  policy as the background trim paths when
  `VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL=1`.
- Added a dedicated `m_trimResidentCursor`.
- Defaults remain unchanged.

Why this was the next target:

- The strongest post-open async visible split stack still spent request/update
  time on two full resident trim scans.
- f480 baseline CPU detail:
  `trimScan=calls/records/candidates/evicted:2/131072/15530/8`.
- Existing incremental trim did not apply to generic `TrimResidentBricks`.

Rejected surface-budget probe:

- Artifact:
  `build/captures/mid_async_visible_postopen_split0_surfacebudget0_footprint_repaircap_pppublic480_20260606`
- `-OwnershipStageSurfaceBudget 0` worsened the public window:
  avg/max raw `58.78/69.32`, avg/max body `66.83/82.45`.
- It also accumulated queued surface backlog at f480:
  total `3123`, split `0/1482/1641/0/0`.
- Decision: reject scalar noncritical surface budget zero.

Incremental trim probes:

| Probe | Artifact | f480 raw/body | window avg/max raw | window avg/max body | trim records | Decision |
|---|---|---:|---:|---:|---:|---|
| baseline | `mid_async_visible_postopen_split0_footprint_repaircap_pppublic480b_20260606` | `50.55/56.29` | `53.17/70.27` | `56.58/74.40` | `131072` | previous best |
| incremental 8192 | `mid_async_visible_postopen_split0_incrementaltrim8192_footprint_repaircap_pppublic480_20260606` | `30.66/32.84` | `31.81/44.53` | `37.44/50.21` | `16384` | useful |
| incremental 4096 | `mid_async_visible_postopen_split0_incrementaltrim4096_footprint_repaircap_pppublic480_20260606` | `47.62/30.82` | `29.83/47.62` | `34.10/47.58` | `8192` | useful |
| incremental 2048 | `mid_async_visible_postopen_split0_incrementaltrim2048_footprint_repaircap_pppublic480_20260606` | `24.76/27.11` | `27.37/39.41` | `29.72/43.20` | `4096` | accepted current |
| incremental 1024 | `mid_async_visible_postopen_split0_incrementaltrim1024_footprint_repaircap_pppublic480_20260606` | `44.73/32.68` | `31.53/44.73` | `35.68/50.14` | `2048` | rejected |

Accepted current f480 details:

- request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `7.46/7.65/13.71/0.01/0.01/2.38/9.73 ms`.
- trim scan `calls/records/candidates/evicted:2/4096/0/0`.
- queued upload `0`, queued surface `0`.
- async visible `enq/complete/apply/drop/dup:24/24/24/0/0`.
- visible/cache coverage `97/76`, visible/cache misses `139/2040`.
- public path valid:
  `PERF_BACKEND_PIPE active=0x7FF`, ownership on, surface raster on;
  `PERF_RENDER_OWNERSHIP miss=0 unsafeNearMiss=0`.

Decision:

- Carry `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`
  as part of the strongest current default-off public-open walk stack.
- Rough public walking is now about `34-37 FPS` in the short f456..484 window,
  not 60 FPS.
- This is the biggest accepted trajectory improvement in this campaign slice,
  but it still needs broader validation.

Next blockers:

- GPU ray remains around `10 ms`.
- Request and exact generation are around `7-8 ms` each.
- Post-wait/frame gap is around `6-8 ms`.
- Clip can still spike when footprint interest rebuilds, e.g. f480 `13.71 ms`.
- Async mid generation still has worker-side cost and pending depth; it is no
  longer on the main thread but can become visible under longer motion.
- Next required validation: longer walk, fixed/static, high-alt, and visual review
  for the new strongest stack before calling it a strong non-60 candidate.

## 2026-06-06 Broad Validation, Adaptive Budget Governor, And Rejections

Broad validation artifact:

- `build/captures/strongstack_incrementaltrim2048_allscenarios_20260606`

Current broader state:

| Scenario | target raw/body | window avg/max raw | window avg/max body | Clean public ownership |
|---|---:|---:|---:|---|
| fixed f380 | `35.99/34.50 ms` | `34.99/35.99 ms` | `34.63/36.64 ms` | yes |
| walk f600 | `39.92/34.86 ms` | `34.85/52.33 ms` | `38.13/52.31 ms` | yes |
| high-alt f400 | `56.70/54.88 ms` | `53.64/57.20 ms` | `53.74/56.29 ms` | yes |

Interpretation:

- Short f480 walking at `27-30 ms` was a useful local win but not representative.
- Rough broader FPS is fixed `28-29`, walking `26-29`, high-alt `18-19`.
- The trajectory is still good because ownership is clean and the bottlenecks
  are measurable, but the renderer is not on a 60 FPS trajectory without a
  bigger motion/high-alt streaming fix.

Implemented default-off diagnostic:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL=1`
- Smoke flags:
  `-OwnershipStageAdaptiveNoncritical`,
  `-OwnershipStageAdaptiveMinVisibleCoverage`,
  `-OwnershipStageAdaptiveMaxMissingVisible`,
  `-OwnershipStageAdaptiveUploadFloor`,
  `-OwnershipStageAdaptiveSurfaceFloor`.
- The runtime computes noncritical ownership-stage upload/surface budgets from:
  public render open, visible-critical coverage/missing counts, and last retired
  ownership miss/unsafe feedback.
- Defaults are unchanged. Do not promote this until broad validation improves.

Rejected probes:

| Branch | Artifact | Why rejected |
|---|---|---|
| static upload budget `0` | `strongstack_incrementaltrim2048_uploadbudget0_allscenarios_20260606` | fixed improved, but walk/high-alt were mixed and visible/surface/upload debt accumulated |
| adaptive floor `2/2` | `strongstack_incrementaltrim2048_adaptive_noncrit_floor2_allscenarios_20260606` | worse all-scenario windows; high-alt coverage still fell after f401 |
| adaptive upload `0`, surface `8` | `strongstack_incrementaltrim2048_adaptive_upload0_surface8_allscenarios_20260606` | fixed-only looked promising, comparable all-scenario run regressed badly |
| visible async caps `64/64` | `highalt_asyncvisible64_incrementaltrim2048_20260606` | no coverage fix; high-alt window worsened to about `99 ms` raw |

Key correction:

- The high-alt visible-critical coverage collapse at f402/f403 exists in the
  baseline strong stack too:
  coverage `99/99` at f400/f401, then `86/84` at f402/f403.
- Do not attribute that only to upload budget zero.
- The next blocker is visible-critical motion stability and high-alt upload/GPU
  pressure, not another scalar noncritical cap.

Next branch:

- Find why high-alt motion produces a large new visible-critical set that is not
  ready by the next frame.
- Prefer prediction/reservation, fallback-proof ownership, or a same-frame
  visible-critical lifecycle fix.
- Avoid larger async caps and broad noncritical throttles unless a narrow probe
  shows broad scenario improvement.

## 2026-06-06 Stress Camera Velocity Diagnostic

Added a default-off diagnostic:

- `VENPOD_SPARSE_MID_CLIPMAP_STRESS_CAMERA_VELOCITY=1`
- smoke flag: `-MidClipmapStressCameraVelocity`

Reason:

- High-alt is driven by the stress camera.
- Stress-camera motion happens before `cameraPosBeforeInputMovement` is captured,
  so the existing clipmap velocity is zero in high-alt even though the camera is
  orbiting.
- Feeding clipmap lookahead from the frame-to-frame residency camera delta was a
  plausible way to predict the f402/f403 visible set.

Result:

| Probe | Artifact | Outcome |
|---|---|---|
| stress velocity enabled | `highalt_stressclipvelocity_incrementaltrim2048_20260606` | rejected: f400 raw/body `91.52/94.20 ms`, window raw `92.21/99.68 ms`, coverage still `99/99 -> 86/84` |
| default-off verification | `highalt_default_after_stressvelocityflag_20260606` | switch is safely inactive by default; coverage pattern unchanged |
| moving-window reservation | `highalt_movingwindow_reservation2048_incrementaltrim2048_20260606` | rejected: queued `0` reservations before f402, did not prevent coverage drop |
| interest pct 100 | `highalt_interest100_incrementaltrim2048_20260606` | rejected: more work and worse f402/f403 coverage, about `83/82` |

Conclusion:

- The f402/f403 high-alt failure is a future-interest admission problem.
- Existing moving-window reservation only scans the current interest set, so it
  cannot reserve the f402 `newV=3080` burst before those coords enter interest.
- Broadly increasing interest capacity or feeding full stress velocity into the
  existing lookahead only increases cost.
- Next branch should add a narrow predicted-visible admission/protection path:
  predicted future visible-critical coords need their own low-width queue or
  ticket lane before becoming current-interest `newV`, without expanding the
  whole high-alt interest set.

## 2026-06-06 Mid-Visible Debt Stage Throttle

Added and validated a default-off ownership-stage hook:

- Runtime:
  `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE=1`
  plus min-coverage, max-missing, upload-floor, and surface-floor envs.
- Harness:
  `-OwnershipStageMidVisibleDebtThrottle`,
  `-OwnershipStageMidVisibleDebtUploadFloor`,
  `-OwnershipStageMidVisibleDebtSurfaceFloor`.
- Logs:
  `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS ... midDebt=...`.
- Parser/manifest:
  `perf_noncapture_smoke.ps1` records the settings and target-frame mid-debt
  activation/budget fields.

Same-build high-alt artifacts:

| Run | Artifact | Window raw/body | Max raw/body | Decision |
|---|---|---:|---:|---|
| baseline | `highalt_currentfirst_preset_currentbaseline_20260606` | `140.54/141.72 ms` | `196.11/196.04 ms` | reference |
| floor `0/0` | `highalt_currentfirst_preset_midvisibledebt_stage0_20260606` | `138.87/140.13 ms` | `196.89/196.83 ms` | reject as candidate; too much `gapPrev` |
| floor `2/2` | `highalt_currentfirst_preset_midvisibledebt_stage2_20260606` | `139.96/141.70 ms` | `187.17/187.12 ms` | retain default-off only |

Conclusion:

- This confirms noncritical prefetch upload/surface debt is competing with
  visible recovery.
- It is not enough for the 60 FPS path. The dominant remaining blocker is still
  high-alt mid-clipmap interest rebuild / visible set churn, not a single stage
  budget.

## 2026-06-06 Voxel Interest Signature Reuse

Added a default-off voxel-interest reuse architecture slice:

- Runtime:
  `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE=1`
  and `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE`.
- Harness:
  `-MidClipmapVoxelInterestSignatureReuse`,
  `-MidClipmapVoxelInterestSignatureReuseMaxAge`.
- Logs/parser:
  `PERF_SPARSE_MID_CLIPMAP_BACKLOG ... voxelReuse=active/age:...`,
  parsed as `midVoxelInterestReuseActive` and `midVoxelInterestReuseAge`.

Behavior:

- `UpdateVoxelInterest` now has a separate coarse voxel-scale signature.
- On match, it reuses the current voxel interest set, refreshes resident touches
  and missing-interest queues, and skips candidate generation/sort.
- The predicted-visible temporary path disables signature reuse, so projection
  remains fresh.

Focused high-alt artifacts:

| Run | Artifact | Window raw/body | Max raw/body | Decision |
|---|---|---:|---:|---|
| baseline | `highalt_currentfirst_preset_postvoxelreuse_currentbaseline_20260606` | `136.64/138.16 ms` | `180.80/191.39 ms` | reference |
| reuse age `1` | `highalt_currentfirst_preset_voxelreusesig_age1_20260606` | `138.01/137.37 ms` | `222.18/222.15 ms` | reject alone; hitchy |
| reuse age `1` + mid-debt `2/2` | `highalt_currentfirst_preset_voxelreuse_age1_middebt2_20260606` | `134.70/135.77 ms` | `180.30/180.25 ms` | retain default-off only |
| reuse age `2` + mid-debt `2/2` | `highalt_currentfirst_preset_voxelreuse_age2_middebt2_20260606` | `134.98/136.49 ms` | `217.63/217.58 ms` | reject |

Conclusion:

- Voxel-interest signature reuse is useful but not sufficient. It trims stable
  adjacent-frame interest work, but f402/f403 still require full rebuild and
  sync visible pump.
- Keep it default-off until broad validation proves fixed/walk/high-alt do not
  regress.
