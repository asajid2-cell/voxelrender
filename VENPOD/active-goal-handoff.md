# Active Goal Handoff

Generated: 2026-06-05

This is the compact survival file for the active VENPOD campaign. If chat compacts, first read `instruct.md` for the long-run operating contract, then resume from this file before reading the larger `handoff.md`, `debug-handoff.md`, `root.md`, or `debug.md`.

## Active Goal

`streaming_playability_real_fix_campaign_20260604`

Drive VENPOD from the current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes, without weakening public-frame ownership correctness or promoting defaults without proof.

Tool goal currently active:

```text
Drive VENPOD toward stable 60 FPS by identifying and patching structural engine bottlenecks at their source: preserve public-frame correctness and visual coverage, measure the dominant cost stage before each change, implement core dataflow/architecture fixes rather than tuning knobs, and reject changes that merely shift debt or trade FPS for instability.
```

## Measurement Foundation Reset - 2026-06-07 (READ FIRST)

A review of the whole campaign found the dominant reason progress stalled is the
**measurement foundation**, not the engineering. Before trusting any further
accept/reject decision, use the new deterministic bench. Key findings:

1. **Walk benchmark was never deterministic (env-var name bug).**
   `perf_noncapture_smoke.ps1 -WalkFixedDtMs` sets
   `VENPOD_SPARSE_WALK_TEST_FIXED_DT`, but `src/main_launcher.cpp` reads
   `VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS` (line ~1684). So the fixed-dt knob has
   always been a no-op: the walk camera advanced by `speed * real-wall-clock-dt`,
   making the camera path depend on frame timing (perf<->input feedback loop) and
   every run non-reproducible. This explains the wildly inconsistent "same
   scenario" baselines (33/41/54/78/135/350 ms) across the campaign.
   FIX: set `VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS` directly, or correct the harness
   env-var name. New `walk_bench.ps1` sets the correct var itself.

2. **vsync is NOT the high-alt contaminator.** Near-identical runs that differ
   only by vsync are within noise:
   `highalt_async_lane_promote_20260607` (vsync on) avg raw `34.59 ms` vs
   `highalt_async_lane_promote_novsync_20260607` (vsync off) `34.31 ms`. The frame
   is genuinely ~34 ms CPU-bound, far over the 16.67 ms vsync interval, so
   present-backpressure never engages. (Vsync only matters once frames approach
   the refresh budget.) Still, benchmark with `-DisableVSync` to remove the
   variable.

3. **Evidence-based frame budget (high-alt f376..404, ~34.6 ms CPU-bound, GPU
   only ~10.7 ms and async):**
   - `clipMs` (mid-clipmap voxel interest) ~15.8 ms = **#1 cost**.
   - `postWait` "gap" ~8.65 ms = **#2 cost, never attacked**. Per-frame
     `PERF_FRAME_END sparsePost=` shows it is dominated by **synchronous
     surfExtract** (e.g. f40 postWait 10.16 of which surfExtract 7.87), NOT
     feedback retirement (`feedbackSplit` sums to ~0.03). Surface extraction is
     running synchronously in the post-fence-wait region.
   - request ~8 ms, generation ~6 ms, surfExtract ~3.5 ms.

4. **Confirmed root cause of clip cost.**
   `SparseClipmapTileCache::QueuePredictedVisibleVoxelInterest`
   ([SparseClipmap.cpp](src/Simulation/SparseClipmap.cpp) ~L2855) deep-copies **9
   live containers** (deques/unordered_sets/unordered_maps), runs the full
   `UpdateVoxelInterest` rebuild **a second time** with a predicted camera, then
   restores all 9. So each frame pays **2-3 full interest rebuilds + ~18 deep
   container copies**, generating ~50k candidate inserts for a ~9k interest set
   (~30k duplicates). Prior fixes failed because they bolted caches/parallel
   builders onto this mutate-then-restore shape (doubling allocation) instead of
   making the builder **pure** (a `(camera,policy)->ordered plan` that mutates
   nothing, consumed by both the live and predicted paths, so prediction needs no
   snapshot/restore). That pure refactor + golden-equivalence test is the real
   Phase 2 fix.

### Deterministic-bench experiments (2026-06-07) — two directions RULED OUT

First trustworthy walking baseline (`walk_bench.ps1`, fixed dt, vsync off, 5x308
frames): raw median ~34-42 ms (~24-29 FPS), noise band +/-7.6 ms (20%), p99 ~60-87 ms.
Steady frame is spread with NO single dominator: request ~10 (pressureTrim ~3.5,
hierarchy/terrainCritical ~1.5, statsFlush ~1.3, missFb ~0.7, hiddenExact ~1),
clip ~12 (interest ~6 + synchronous pump ~6), generation ~7, surfExtract ~3.5.
`fullRebuild=1` and often `centerDelta=0/0/0` (full interest rebuild even when the
camera did not cross a brick boundary).

1. **Parallel/async stack RULED OUT as a median lever.** Enabling
   ParallelMidVoxelPump + ParallelSurfaceExtraction + ParallelExactGeneration +
   IncrementalPressureTrim(2048) moved `clip` down (~12) but raised
   postWait/surfExtract (~10/4.4); **median got slightly WORSE (~41-47 vs ~34-42)**,
   only tighter p99. Threading a stage does not shorten the serial per-frame chain;
   the main thread still blocks on each stage's output within the frame.

2. **Pressure-trim free-page guard RULED OUT (catastrophic).** Hypothesis: trim is
   wasteful (scans 131072 pool records, evicts only 8, runs every frame on
   missFeedbackPressure while 35k pages are free; cost ~3.5 ms is independent of
   scan budget). Reality: gating it **doubled the whole frame** (median 67-84 ms,
   clip 31, gen 17, postWait 17). The trim is **load-bearing** — it keeps the
   resident working set small enough that every downstream stage stays cheap. This
   confirms the campaign's "pressure-trim gating is a rejected family" even on the
   clean bench.

3. **Smaller interest set (work-volume reduction) RULED OUT (thrash).** Cutting
   `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT` 75->40 (cap 9216->4915) **doubled the
   frame** (median 70-84 ms) and exploded request (25 ms) + generation (18 ms) +
   gapPrev hitches (150-240 ms). Fewer resident bricks -> more misses -> on-demand
   request/gen thrash. The 9216/75% set is sized to just cover the view; shrinking
   it breaks coverage. (Note: `MID_VOXEL_RADIUS_XZ` does NOT change the cap, which
   is pool*pct, so radius alone is not a work-volume knob.)

4. **Existing age-based interest reuse RULED OUT (stale->thrash), but it is the
   WRONG reuse.** `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE` age 1
   fired (interest ~6ms->~1ms) but `missingVoxel` exploded ~130->5184 (~40x) with
   req/gen ~doubling. Root cause: the reuse gate (`staticVoxelInterestSignatureMatches`,
   SparseClipmap.cpp ~L4022) deliberately ZEROES camera/forward/velocity before
   comparing, so it reuses on AGE alone, ignoring that the camera walked forward ->
   stale set -> coverage holes -> repair thrash. (NOTE: prior "pump=246" in clip
   detail is a COUNT, not ms; clipMs stayed 7-20ms. The disqualifier is the
   missingVoxel/coverage explosion, not pump time.)
   IMPLICATION: a reuse gated on `centerDelta==0` (brick cell unchanged => set is
   genuinely identical, not merely "recent") would not go stale. That is the one
   principled local fix not yet tried -- a code change adding a centerDelta==0
   condition to the reuse gate. Expected gain may be sub-noise (~interest 6ms on
   centerDelta=0 frames only) unless a stable set also reduces gen/pump churn.

5. **Center-gated interest reuse (the frontier thesis itself) IMPLEMENTED + RULED
   OUT.** Added `voxelInterestRecenterGate` (reuse while camera stays in the same
   finest-ring brick cell), built, benched, reverted. Regressed: median ~56 ms (vs
   ~38), request 18.6, gen 13, clip hitches to 121 ms at recenter. Mechanism:
   stable-center frames were perfect (interest 1 ms, missingVoxel 0), but each
   recenter dumped the accumulated frontier delta (~1479 bricks) -> pump 118 ms.
   **The per-frame full rebuild IS the generation load-balancer** — it feeds
   generation smoothly (~46 bricks/frame); skipping it batches work into recenter
   bursts. Forward-walking cost is mostly irreducible (must generate+mesh terrain
   you walk into). See `frontier-streaming-design.md` "RESULT: Step 1c failed". The
   revised real lever is PREFETCH-AHEAD generation (workers generate the frontier
   before arrival, off the critical path), not gating the rebuild. Do not retry
   interest reuse.

6. **Stable-30 via hard pump budget IMPLEMENTED + RULED OUT (catastrophic).**
   Phase 1 target was the p99 tail. Found the spikes are the mid-voxel PUMP hitting
   180-280 ms in dense terrain regions (per-brick cost ~2.2 ms, ~20x normal),
   because the coverage-first policy zeroes `pumpBudgetMs` to catch up. Spikes are
   terrain-location dependent, NOT walk-speed dependent (tested speed 10 vs 38 -
   same spikes, just shifted frames). Implemented `voxelPumpHardBudgetMs` (a hard,
   always-enforced per-pump time budget, code retained default-off as a building
   block). At 8 ms it bounded the pump (clip 25->17, gen/surface/postWait collapsed)
   BUT request exploded to 50 ms and produced multi-second `gapPrev` freezes
   (5 s, 8 s, **36 s**): bounding generation lets the missing-brick backlog grow
   unbounded -> request re-scans it every frame AND the coverage-emergency
   eventually force-generates the whole backlog at once. Bounding cannot work alone;
   it must be paired with admission control (which separately thrashes coverage).

**FINAL VERDICT (8 experiments, trustworthy bench): neither 60 FPS nor stable 30 is
reachable by any local/bounding change.** Parallelize, trim-gate, shrink-set,
interest-reuse (age + center-gated), pump-budget - every one breaks the coupled
coverage/generation equilibrium and shifts or amplifies the cost (worst case 36 s
freezes). The unbounded full per-frame work IS the only stable operating point
(~26 FPS avg with terrain-dependent spikes). The ONLY remaining path is an
architectural rewrite that decouples generation from the per-frame coverage
requirement: **prefetch-ahead generation** - worker threads generate terrain INTO A
PERSISTENT CACHE ahead of the camera along its path, so the per-frame thread renders
already-resident terrain and does little/no generation. This removes both the pump
spikes AND the backlog explosion (generation is never on the critical path). It is a
multi-week build with a graveyard of partial prior attempts; it is NOT a same-day
fix. See `frontier-streaming-design.md` lever (A).

**Conclusion: the engine is a tightly-coupled steady-state system, and the full
per-frame rebuild over a view-sized set IS the stable operating point.** Every
perturbation tested (parallelize, bigger set, smaller set, stale reuse) breaks the
coverage/generation equilibrium and explodes a stage. The ~38ms cost buys that
stability. Local/flag optimization is exhausted; real 60 FPS needs a different
streaming architecture (generate+mesh terrain ONCE on cell entry and persist it,
rather than re-deriving the working set every frame). Local
stage-level changes either shift cost (parallelism) or break the coverage
equilibrium in BOTH directions (no-trim balloons, smaller-set thrashes). Three
clean rejections on a trustworthy bench. Work-volume reduction is ruled out because
the set is sized to just cover the view. **The one remaining non-destructive lever:
amortize the per-frame FULL rebuild across frames.** `fullRebuild=1` runs every
frame even when `centerDelta=0/0/0` (camera did not cross a brick boundary), so the
emitted interest set is ~identical to last frame yet fully recomputed (~candidate
gen + sort + request scan). Keep the SAME set (no thrash); skip the redundant
recompute when center+view are unchanged. NOTE: prior signature/ring-plan reuse
attempts failed, but on the broken benchmark and at high-alt (continuous motion,
centerDelta rarely 0). Walking with fixed dt has frequent centerDelta=0, so reuse
has real opportunity here. Next step: find why the existing
`VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE` does not fire on walking
and make it fire safely (reuse emitted set, still run apply/queue/reservation).

### Measurement Protocol (use for ALL walk perf work)

- North-star scenario: **walking** (gameplay), not high-alt (worst case).
- Run `.\walk_bench.ps1 -Runs 5 -NoBuild` (forces fixed dt + vsync off, parses
  every `PERF_FRAME_END`, reports median/mean/p99/max + per-stage bottleneck table
  + worst hitch frames + the **noise band** = spread of per-run medians).
- A change is REAL only if it moves **median AND p99** beyond the noise band.
  Do not accept/reject on a single run or a single target frame.
- `PERF_FRAME_END` is per-frame; `PERF_SPARSE_CPU_DETAIL` (clip/req/gen) is only
  ~every 16 frames, so treat clip/req/gen as supplementary means.

## Latest Baseline Correction - 2026-06-07

The accepted high-alt stack is **not** the stale 14 FPS line below from the
June 6 pass. That line describes older rejected/regressed states. Current
accepted comparison baseline:

- Artifact: `build/captures/highalt_async_lane_promote_repeat_20260607`
- Command:
  `.\perf_noncapture_smoke.ps1 -Config Release -Scenario highalt -StackPreset highalt-currentfirst -OutputDir build\captures\highalt_async_lane_promote_repeat_20260607 -NoBuild -ParallelSurfaceExtraction -ParallelSurfaceExtractionTimeBudgeted -ParallelSurfaceExtractionMaxWorkers 4`
- Window f376..404 raw/body average: `33.73/33.88 ms`, about `29-30 FPS`
- f400 raw/body: `39.95/34.21 ms`
- f400 CPU sparse prep: `25.25 ms`
- f400 request/generation/clip: `7.62/6.02/11.59 ms`
- f400 postWait/surfaceExtract/surfaceStage/sparseUpload: `9.38/4.13/1.27/1.40 ms`
- f400 GPU/ray: `10.77/9.23 ms`
- f400 coverage: visible-critical `100`, cache `99`, total `100`

Interpretation: still far from 60 FPS, but the correct accepted baseline is
roughly 30 FPS. Treat any 14-16 FPS result as a rejected branch unless a newer
accepted artifact explicitly says otherwise.

Current-tree anchor after later dirty-source state - 2026-06-07:

- Artifact: `build/captures/highalt_current_tree_anchor_20260607`
- Same high-alt command and parallel surface flags as above.
- Window f376..404 raw/body average: `41.09/40.95 ms`, about `24-25 FPS`.
- f400 raw/body: `44.14/40.84 ms`.
- f400 CPU/request/generation/clip: `32.96/9.10/7.47/16.37 ms`.
- f400 predicted visible rebuild: `8.42 ms`, phase split
  `0.52/6.52/0.39/0.13 ms` for snapshot/rebuild/restore/queue.
- f400 request detail: pressure trim `3.64 ms`, stats flush `1.10 ms`,
  hidden exact `1.78 ms`.
- f400 visible-critical/cache/total coverage: `99/92/99`.
- f401/f402/f403 visible-critical coverage stayed `99/99/99` with only
  `6` projected visible misses each frame and no overdue reservations.

Use this current-tree anchor when comparing new work in this dirty source
state. The earlier `33.73/33.88 ms` artifact remains useful history, but the
current working tree measured slower before the hidden-exact experiment below.

Rejected hidden-exact active-work outstanding list - 2026-06-07:

- Motivation: hidden exact tracks thousands of historical coords and the
  pre-request outstanding-work pass scanned `sparseHiddenExactMissTrackedCoords`
  every frame. f400 had hidden exact `1.78 ms` and tracked count around
  `8500`, so this looked like a lifecycle ownership problem.
- Experiment: added `sparseHiddenExactActiveWorkSet/Coords`, marked accepted
  or rediscovered active hidden-exact work, and used that compact list for the
  pre-request outstanding count while leaving the later full audit in place.
- Validation:
  build and tests passed. Smoke artifact:
  `build/captures/highalt_hidden_exact_active_work_20260607`.
- Result:
  rejected and reverted. The window improved locally to `37.71/38.13 ms`, and
  f400 coverage was `100/92/100`, but f402/f403 visible-critical coverage
  collapsed to `91/91` with projected visible misses `812/734`. The anchor had
  f402/f403 visible-critical coverage `99/99` with only `6/6` projected misses.
- Decision:
  do not retry compacting hidden-exact outstanding accounting without proving
  how it preserves the downstream mid-clipmap visible reservation timing. The
  hidden-exact scan is real cost, but stale/ready historical tracking is coupled
  to later visible-critical scheduling in this stack.
- Revert verification:
  marker search for `sparseHiddenExactActiveWork`,
  `markHiddenExactActiveWork`, and `pruneHiddenExactActiveWork` is clean.
  `.\build.ps1 -Config Release` and `.\build\bin\VENPODTests.exe` passed after
  revert.

Current-tree interest detail probe - 2026-06-07:

- Artifact: `build/captures/highalt_current_tree_interest_detail_20260607`
- Command: current high-alt smoke with `-MidClipmapInterestDetail`.
- Window with detail instrumentation: `38.53/38.67 ms` raw/body, so do not use
  it as the pacing baseline; use it for cost shape.
- f400 predicted visible: `10.29 ms`, phase split
  `0.44/7.50/0.47/0.33 ms` for snapshot/rebuild/restore/queue.
- f400 current interest detail:
  `lineMs=0.15`, `anchorMs=2.44`, `sortEmitMs=2.13`,
  `backlogMs=0.08`, `diagMs=0.71`.
- f400 candidate shape:
  `candidates=20780`, `candidateAttempts=50572`,
  `duplicateHits=29792`, `scoreUpdates=11758`, `emitted=9216`.
  Source split:
  - line `2061/450/234` attempts/duplicates/scoreUpdates
  - anchor terrain `35524/18040/7453`
  - anchor footprint `11037/10487/3774`
  - anchor camera `1950/815/297`
- Interpretation:
  local hash/index/candidate-pruning variants have already failed, and this
  detail run reinforces why: the problem is not one small source of duplicate
  candidates. Runtime pays a full current interest rebuild and a full predicted
  visible rebuild in the same public-frame path. The next viable clipmap
  architecture target should be explicit incremental/frontier ownership or an
  async/deadline interest builder with a single source of truth for current,
  predicted, queued, resident, and overdue states. Do not retry pure collector
  swaps, local duplicate pruning, or candidate index substitutions as isolated
  fixes.

Retained known-empty early request skip - 2026-06-07:

- Evidence before change:
  current-tree anchor f400 request detail had `requestAttempts=133`,
  `nonResident=132`, `allocated=22`, and `knownEmpty=110`. Those known-empty
  requests still passed through nonresident admission, free-page/reserve checks,
  budget checks, and only then got skipped inside `RequestBrickDetailed`.
- Retained code:
  added `SparseVoxelWorld::TrySkipKnownEmptyRequest(const BrickCoord&)`, which
  owns the same known-empty condition/cache/stat mutation used by
  `RequestBrickDetailed`. The central `requestSparseBrick` lambda now calls it
  immediately after the resident-page check when `allowEmptyFastPath` is true,
  before free-page, replacement, reserve, class-budget, and total-budget work.
  `RequestBrickDetailed` reuses the same helper so the condition remains
  single-sourced.
- Tests:
  extended `TestSparseVoxelWorldLifecycle` to verify the fast path skips and
  caches a high-air brick without allocation, and refuses to skip an edited
  high-air brick.
- Validation:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed.
- Smoke 1:
  `build/captures/highalt_known_empty_early_skip_20260607`.
  Window raw/body `35.56/35.56 ms` versus current-tree anchor `41.09/40.95 ms`.
  f400 request nonresident attempts dropped `132 -> 23` while known-empty skips
  stayed `110`, allocated requests stayed `23`, and f401/f402/f403
  visible-critical coverage was `99/99/100` with no overdue reservations.
- Smoke 2:
  `build/captures/highalt_known_empty_early_skip_rerun_20260607`.
  Window raw/body `37.81/38.24 ms`, still better than anchor. f400 nonresident
  attempts stayed `23`, known-empty skips stayed `110`, and f401/f402/f403
  visible-critical coverage improved to `100/100/100` with no overdue
  reservations.
- Decision:
  keep. This is a structural admission-order fix: known-empty coords no longer
  consume the expensive nonresident request path. It does not cap work, reduce
  quality, or weaken visible-critical coverage.

Rejected bounded trim candidate selection - 2026-06-07:

- Motivation: f400 pressure trim still scans two full resident-record passes:
  `trimScan=calls/records/candidates/evicted:2/131072/16930/8`.
  Existing incremental cursor trim was rejected because it cut scans but broke
  visible-critical coverage. This experiment preserved the full scan and exact
  comparator semantics, but retained only the best `maxEvictions` candidates
  instead of materializing every eligible trim/replacement candidate.
- Validation:
  build and tests passed. Smoke artifact:
  `build/captures/highalt_bounded_trim_candidates_20260607`.
- Result:
  rejected and reverted. Window raw/body regressed to `43.03/43.80 ms`.
  f400 pressure trim did not improve (`2.79 -> 2.95 ms`), while f400 clip
  interest worsened (`9.32 -> 12.85 ms`) and surface extract worsened
  (`4.13 -> 5.20 ms`). Coverage at f400 remained `100/93/100`, but total
  frame time was worse.
- Decision:
  do not revive bounded top-K trim storage. The real trim fix still needs an
  owned eviction-candidate/lifecycle index or active/background lists that
  avoid scanning `m_pool.Records()` while preserving future visible-critical
  residency shape.
- Revert verification:
  marker search for `KeepBestEvictionCandidate`, `candidateCount`, and
  `candidates.reserve(static_cast<size_t>(maxEvictions))` is clean.
  `.\build.ps1 -Config Release` and `.\build\bin\VENPODTests.exe` passed after
  revert.

## 2026-06-06 Structural Bottleneck Pass Update

Current rough high-alt FPS is still about `14 FPS`, not close to 60. The retained
like-for-like high-alt baseline after the accepted stats cleanup is:

- artifact: `build/captures/highalt_currentfirst_stats_ticket_single_pass_20260606`
- f376..404 avg/max raw: `69.53/90.40 ms`
- f376..404 avg/max body: `70.21/95.52 ms`
- f400 raw/body: `73.32/69.57 ms`
- f400 CPU/request/gen/clip/surfaceExtract/GPU frame/ray:
  `39.41/10.16/8.56/20.68/5.51/13.83/12.29 ms`
- f400 visible-critical/cache coverage: `100/93`
- f400 reservation tickets active/due/overdue: `493/0/0`

Retained code from this slice:

- `SparseVoxelWorld::RefreshStats` now computes streaming-ticket oldest age in
  the existing ticket loop instead of scanning `m_streamingTickets` twice.
- Existing retained SparseClipmap work remains: source-level voxel-interest
  diagnostics, predicted-visible timing fields, and the camera-height vertical
  band overlap skip.

Rejected branches from this slice:

- Page-table published lookup guard in `RefreshStats`: regressed to
  `81.06/82.48 ms` window raw/body and raised f400 request/stats cost; reverted.
- Predicted admission temp-stats skip: broke the scheduler with hundreds of
  overdue visible tickets; reverted.
- Footprint dominated candidate skip: regressed to about `114/115 ms`; reverted.
- Miss-only resident trim, pressure-trim guard ablation, incremental trim 4096,
  parallel surface extraction, voxel interest reuse age 1, hidden tracked scan
  budget 512: all rejected or not retained because they shifted cost or worsened
  coverage/backlog.
- Predicted voxel generation queue limit:
  `build/captures/highalt_currentfirst_predicted_queue_limit_20260606`
  regressed the high-alt window to `110.19/110.66 ms` avg raw/body, f400
  visible/cache coverage to `99/90`, and f402/f403 visible coverage to `90/90`;
  reverted. Do not revive this truncation approach. The discarded candidates
  were doing real stabilizing work.
- Mid-clipmap per-interest terrain sample cache:
  `build/captures/highalt_currentfirst_interest_terrain_cache_20260606` and
  `build/captures/highalt_currentfirst_interest_terrain_cache_rerun_20260606`.
  This reduced predicted-visible elapsed time (`f400 12.49 -> 10.10/8.46 ms`,
  `f402 23.60 -> 21.64/18.68 ms`) and f400 `clipMs` in the rerun
  (`20.68 -> 16.21 ms`), but the full window still regressed to
  `76.36/76.95 ms` and `75.34/75.85 ms` avg raw/body versus the retained
  `69.53/70.21 ms`, with higher post-wait/upload/surface pressure. Reverted.
  Do not accept a prediction micro-optimization unless the full frame window
  improves without downstream pressure.

Next measured blocker:

- `clipMs` and predicted visible admission remain the dominant CPU-side source
  (`~20-30 ms` clip, `~12-30 ms` predicted admission in high-alt samples), but
  truncating prediction output destabilizes coverage. The next valid branch
  should remove duplicate work inside full prediction/interest construction or
  make its dataflow incremental, not cap the number of predicted coords or skip
  lifecycle accounting.

Latest validation state:

- `.\build.ps1 -Config Release`: passed after reverting rejected queue-limit and
  terrain-cache work.
- `.\build\bin\VENPODTests.exe`: passed.
- Known pre-existing warnings: `main_launcher.cpp` local `rayDir` shadowing.

## 2026-06-06 Structural Bottleneck Continuation

User reinforced that the goal is structural bottleneck removal, not turning
quality/budget knobs until a row happens to read 60 FPS.

GPU-side inspection:

- The retained high-alt baseline already has the background pass split enabled:
  `foreground=960x540`, `background=360x203`, scale `0.375`.
- `gpuRayMs=12.29` includes the low-res background raymarch, transition, and
  composite between the sparse-surface and overlay GPU timestamps.
- Frame 400 ownership is mostly background sky/water/far-SVO:
  `midVoxel=3`, `farSvo=15468`, `farWater=19621`, `sky=38081`.
- Sparse surface is not the primary GPU bottleneck in this sample:
  `surfaceGpu=1.32 ms`, `surfaceFragments=152800`, `overdrawRatio=0.34`.

Rejected branch:

- Tried a geometric high-alt upward-sky early-out in `PS_Raymarch.hlsl` before
  far water/SVO traversal. This was reverted. The shader compiled, but the app
  then stalled after the `PS_Raymarch.hlsl` cache hit and did not reach normal
  frame logging within the 180 s smoke timeout. Do not retry monolithic shader
  edits casually; their compile/PSO validation cost is currently too high for
  exploratory branches.

CPU-side source finding:

- `QueuePredictedVisibleVoxelInterest` currently snapshots live scheduler
  containers, calls full `UpdateVoxelInterest`, copies the predicted
  `m_voxelGenerationQueue`, then restores live state.
- This makes prediction a full stateful interest rebuild even though the caller
  only needs candidate coords to admit as visible reservations.
- Prior queue move/snapshot tricks were rejected; do not revive them.
- The next correct implementation is to extract a pure candidate-builder from
  `UpdateVoxelInterest` that uses the same anchors, scoring, ring quotas, and
  sorting but writes coords to a caller-owned vector. Then
  `QueuePredictedVisibleVoxelInterest` can feed those coords into the existing
  live `queuePredictedCoord` admission path without mutating/restoring
  `m_voxelGenerationQueue`, `m_queuedVoxelSet`, `m_voxelBacklogFirstFrame`,
  `m_voxelInterestSet`, or priority sets.

Rejected CPU refactor:

- Tried an in-place collect-only mode on `UpdateVoxelInterest` so prediction
  could reuse the same candidate construction without snapshotting/restoring
  live queues.
- Artifacts:
  `build/captures/highalt_currentfirst_prediction_collectonly_20260606` and
  `build/captures/highalt_currentfirst_prediction_collectonly_emitcap_20260606`.
- The collect-only version reduced some local predicted-visible elapsed samples
  (`f400 12.49 -> 9.48 ms` before the emit cap), but it changed visible
  reservation admission and shifted pressure downstream.
- The capped rerun still regressed the retained high-alt window:
  `70.52/71.44 ms` avg raw/body vs retained `69.53/70.21`.
- It also regressed f400:
  raw/body `83.36/70.14 ms` vs retained `73.32/69.57`, coverage
  `99/89` visible/cache vs retained `100/93`, and reservation tickets
  `1108` active with `600` overdue vs retained `493/0`.
- Reverted. The lesson is that the pure candidate-builder cannot merely reuse
  `UpdateVoxelInterest` with side effects gated off; the prediction admission
  contract is coupled to backlog/current-interest lifecycle. The next branch
  must preserve exact admission order and reservation lifecycle semantics, or
  introduce explicit lane/state ownership for predicted reservations.

Rejected request-side accounting branch:

- Tried extending the existing `SparseVoxelWorld` stats deferral transaction to
  cover pressure trim before request admission, replacing the mid-trim
  free-page decision with `GetPool().FreePageCount()` so trim functions would
  not each force a full `RefreshStats()` before the request-phase flush.
- Artifact: `build/captures/highalt_currentfirst_pressure_trim_stats_defer_20260606`.
  First run looked locally promising: f376..404 avg raw/body improved from
  retained `69.53/70.21 ms` to `68.19/69.06 ms`; f400 request dropped
  `10.16 -> 7.65 ms`, CPU dropped `39.41 -> 35.56 ms`, coverage stayed
  `100/93`, and reservation tickets stayed healthy (`494/0/0`).
- Confirmation artifact:
  `build/captures/highalt_currentfirst_pressure_trim_stats_defer_rerun_20260606`.
  Rejected because the repeated full window regressed to `75.42/76.17 ms`
  avg raw/body with higher upload/post-wait/surface pressure, despite f400
  request remaining lower (`7.86 ms`) and coverage/tickets staying intact.
- Reverted. Lesson: pressure-trim stats batching by itself is not enough; if
  this path is revisited it needs explicit lifecycle/debt accounting so the
  saved request accounting work does not destabilize downstream upload/frame
  pacing.

Rejected predicted-visible timing probe:

- Tried forcing `QueuePredictedVisibleVoxelInterest` phase timing on even when
  full `voxelInterestDetail` is disabled, so normal high-alt runs would log
  nonzero `phaseMs=snapshot/rebuild/restore/queue`.
- Artifact: `build/captures/highalt_currentfirst_predicted_phase_timing_20260606`.
  Rejected because the run regressed badly: f376..404 avg raw/body
  `118.64/119.28 ms`, f400 raw/body `116.00/114.73 ms`, and f400 streaming
  tickets ballooned to `7620` active. Reverted after build/test.
- The probe still produced useful direction before rejection:
  - f400 predicted elapsed `19.87 ms`, phase split
    `0.64/16.95/0.48/0.58 ms` for snapshot/rebuild/restore/queue.
  - f402/f403 predicted elapsed `34.67/35.58 ms`, rebuild
    `29.84/30.52 ms`.
  - Conclusion: the dominant cost is the full `UpdateVoxelInterest` rebuild,
    not copying/restoring queues or the final predicted admission pass. The
    next prediction-side structural branch should extract or incrementally
    maintain the voxel-interest candidate builder while preserving exact
    admission/reservation lifecycle semantics.

Rejected quota-helper extraction:

- Tried a behavior-preserving first extraction step by moving
  `UpdateVoxelInterest` ring-quota calculation into
  `BuildVoxelInterestRingQuotas`, with the same weighting and quota balancing,
  then calling it from `UpdateVoxelInterest`.
- Artifacts:
  `build/captures/highalt_currentfirst_voxel_interest_quota_helper_20260606`
  and `build/captures/highalt_currentfirst_voxel_interest_quota_helper_rerun_20260606`.
- Rejected because the full high-alt window regressed twice despite healthy
  f400 mid coverage/reservations:
  - first run f376..404 avg raw/body `81.25/82.26 ms`;
  - rerun f376..404 avg raw/body `75.53/76.25 ms`;
  - retained baseline remains `69.53/70.21 ms`.
- Reverted after build/test. Lesson: even apparently behavior-equivalent hot
  path refactors need a narrow equivalence harness before performance smoke.
  The next candidate-builder work should first add a default-off/local
  equivalence checker that compares emitted voxel-interest coords/order between
  the live path and any extracted builder, without changing the normal runtime
  path.

Retained test-only guardrail:

- Added a sparse core test in `TestSparseClipmapTileCache` that records the
  current missing voxel-interest set, calls
  `QueuePredictedVisibleVoxelInterest`, then verifies the current
  `interestedVoxelBricks`, `missingInterestedVoxelBricks`, and sorted missing
  interest set are unchanged.
- This is not a frame-time win, but it protects the key contract needed for the
  next structural refactor: predicted-visible collection/admission must not
  mutate current public-frame interest while it prepares future work.
- Validation: `.\build.ps1 -Config Release` passed, then
  `.\build\bin\VENPODTests.exe` passed.

Retained debug candidate collector:

- Added `SparseClipmapTileCache::CollectPredictedVisibleVoxelInterestForDebug`.
  It runs the current stateful predicted-visible rebuild path, returns the
  predicted voxel candidate order to a caller-owned vector, then restores the
  live voxel queues/sets and stats snapshot. It is not called by the runtime
  frame path.
- Extended `TestSparseClipmapTileCache` to verify the debug collector returns
  candidate work before generation and preserves the current missing
  voxel-interest set/count. The existing predicted-admission preservation test
  remains.
- Validation: `.\build.ps1 -Config Release` passed, then
  `.\build\bin\VENPODTests.exe` passed.
- Next structural branch: implement a pure/non-mutating voxel-interest
  candidate builder and first compare its output against
  `CollectPredictedVisibleVoxelInterestForDebug`; only wire it into
  `QueuePredictedVisibleVoxelInterest` if the candidate order and current
  interest preservation contract match.

Retained pure debug candidate builder:

- Added `SparseClipmapTileCache::CollectPredictedVisibleVoxelInterestPureForDebug`.
  It computes predicted voxel candidates directly from camera/forward/policy,
  resident voxel slots, terrain heights, ring quotas, anchor bands, and
  per-ring candidate sorting without mutating live interest, queues, stats, or
  reservations.
- Extended `TestSparseClipmapTileCache` to compare the pure debug collector
  output against the stateful debug collector output from
  `CollectPredictedVisibleVoxelInterestForDebug`; the test requires identical
  candidate order and preserves the current missing-interest set.
- Validation: `.\build.ps1 -Config Release` passed, then
  `.\build\bin\VENPODTests.exe` passed.
- This still does not change the runtime frame path and is not a performance
  candidate by itself. The next runtime branch can replace the stateful
  prediction rebuild inside `QueuePredictedVisibleVoxelInterest` with the pure
  collector, but must be validated with the high-alt baseline and rejected if
  reservation health, coverage, upload/post-wait, or the full window regresses.

Validation after rejected shader branch:

- `.\build.ps1 -Config Release`: passed after reverting the shader early-out.
- `.\build\bin\VENPODTests.exe`: passed before the rejected shader rerun.
- No VENPOD process should be left running from the timed-out captures.

Validation after rejected collect-only branch:

- `.\build.ps1 -Config Release`: passed after reverting collect-only.
- `.\build\bin\VENPODTests.exe`: passed after reverting collect-only.

## Goal Reinforcement - 2026-06-05

User explicitly asked for a durable goal that survives compaction and prevents early stopping.

Use the existing active `/goal`; do not create a new narrow diagnostic goal unless this one is completed or blocked by the formal criteria below.

## Self-Goal Contract - 2026-06-05

The working goal is:

> Build a validated default-off VENPOD playable candidate stack, or prove with code evidence that the current architecture cannot safely reach one without a specific ownership-aware streaming refactor.

This goal exists because prior work repeatedly stopped after a diagnostic, a partial win, or a failed heuristic. That is not sufficient. The next engineer/agent must keep iterating through the measured blocker ladder until one formal completion state is reached.

Hard anti-stop rules:

- Do not stop after adding one counter if the counter identifies an obvious next safe code change.
- Do not stop after one prototype fails if another measured branch remains safe and specific.
- Do not stop after a partial candidate unless the exact remaining bottleneck table is written and no local safe branch remains.
- Do not call the renderer playable until representative noncapture rows meet the frame-time target and visual/contact-sheet validation is clean.
- Do not accept numeric `miss=0` / `unsafe=0` while visible terrain, shoreline, ownership, or frame pacing remains bad.
- Before any final answer or likely compaction point, update this file with:
  - latest build/test state,
  - candidate flags tested,
  - accepted/rejected branch decisions,
  - target-frame and window metrics,
  - visual verdict,
  - exact next measured blocker.

Completion states:

1. **Validated 60 FPS candidate**: representative noncapture fixed and walk rows are `<= 16.67 ms` total frame time with clean visual and ownership validation.
2. **Strong non-60 candidate**: strongest safe default-off stack is validated, combined playable mode is tested, and the remaining top blockers are measured by scenario.
3. **Hard architecture blocker**: at least two plausible branches are attempted or ruled out by code evidence, and the missing ownership/streaming refactor is specified precisely enough to implement.
4. **Hard tool/build blocker**: build, ctest, shader compilation, runtime, or validation tooling blocks further safe progress after attempted fixes.

Current best-known engineering posture:

- Background split is a real GPU candidate but remains default-off.
- Clean terrain prefetch throttle helps CPU but remains default-off.
- Backlog-aware pump and parallel mid pump help selected rows but are not enough.
- Async generation remains blocked for sampled unknown/fallback-invalid debt.
- High-alt visual correctness remains blocked by a bright sparse-surface/shoreline artifact that is not caused by the high-alt-only stage budget gate.
- Walk/realtime still has distributed request, generation, clip/pump, surface extraction/stage, residual GPU, and frame-gap cost.
- The next real implementation must not be another isolated cap. It should carry ownership/source lane state across request, generation, upload/apply, surface extraction, and publish readiness, or document why that state-machine slice is required before more local optimization can be correct.

Current operational target:

- keep moving through the measured blocker ladder until there is either a validated 60 FPS candidate, a strongest validated non-60 stack with an exact remaining blocker table, or a hard architecture/tool blocker;
- start every work session from this file, then the latest campaign artifacts, then code;
- update this file before any final answer or compaction-sensitive pause;
- do not end after a single counter, failed heuristic, or obvious next action;
- do not claim success from build/test or miss/unsafe alone.

Immediate focus after the latest accepted stack:

- inspect and fix the real surface/generation/frame-gap path, not more scalar caps;
- prior tests rejected surface max-ms budgeting, parallel surface extraction, terrain-critical parallel generation, lower background scale, compact logging, buried-solid fast path, and global surface-fill exact repair;
- the next viable code slice must either avoid unnecessary unchanged surface extraction, reduce per-brick surface extraction cost without moving work into unsafe backlog debt, or explain why a lane/state-machine refactor is required.

## Surface-Ready Lane Accounting / Front-Sort Rejection - 2026-06-06

Retained behavior-neutral accounting:

- `PERF_SPARSE_SURFACE_READY_PUBLISH` now logs pending surface-ready publish lanes:
  `pendingLane=cache/prefetch/visible/public:a/b/c/d`.
- `perf_noncapture_smoke.ps1` parses and reports:
  `surfaceReadyPublishLaneCache`, `surfaceReadyPublishLanePrefetch`,
  `surfaceReadyPublishLaneVisible`, `surfaceReadyPublishLanePublic`.

Build/test after retaining accounting and removing the failed prototype:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.

Artifacts:

- fixed accounting sanity: `build/captures/ownership_lane_surface_ready_accounting_20260605`
- accepted-stack short walk baseline: `build/captures/ownership_lane_surface_ready_accounting_walk120_accepted_20260605`
- rejected front-sort probe: `build/captures/streaming_ticket_front_sort_walk120_accepted_20260606`

Accepted-stack short walk baseline (`-WalkFrame 120`, accepted partial stack, no front-sort):

- target f120 raw/CPU/request/gen/clip/pump/surface/GPU:
  `65.86/38.56/13.55/9.66/15.34/5.85/7.96/17.38`
- window f96..124 avg/max raw: `72.65/91.67`
- `PERF_SPARSE_SURFACE_READY_PUBLISH frame=120`: pending `0`,
  `pendingLane=0/0/0/0`, scanned/promoted/deferred `31/31/31`
- `PERF_SPARSE_PAGE_PUBLISH_LANES frame=120`: all publish/surface-ready lanes `0`
- `PERF_SPARSE_STREAMING_TICKETS frame=120`: active `8509`,
  ownership public/sampled/prefetch `0/120/8389`, all pending CPU

Conclusion:

- Surface-ready publish pressure is not the immediate walk blocker in this accepted-stack sample.
- The live debt is mostly CPU-stage prefetch ticket backlog plus sampled/public trickle, while frame time is distributed across request, clip/pump, surface, GPU, and frame gaps.

Rejected prototype:

- Tried a default-off `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_FRONT_SORT_ONLY`
  / `-StreamingTicketGenerationFrontSortOnly` branch that only sorted the protected generation queue front by frame budget.
- It was removed after validation because it worsened the matched short walk probe:
  target raw `118.76` vs baseline `65.86`, window avg/max raw `124.01/323.16` vs `72.65/91.67`.
- It pulled more public/visible work into the frame and regressed request/clip/surface instead of reducing scheduler cost.

Next direction:

- Do not retry generation front-only ticket sorting as a main fix.
- Do not chase surface-ready publish pressure unless a later full-walk artifact shows nonzero lane pending.
- The next real branch should split public/sampled correctness work from broad prefetch/cache CPU-stage debt at the streaming state-machine level, or directly reduce request/clip/pump/GPU cost without changing public-frame ownership readiness.

## Low-Lane Queued Trim Rejection - 2026-06-06

Attempted and removed a default-off branch that trimmed already-admitted queued cache/prefetch lane bricks under prefetch generation queue pressure.

Removed symbols/options:

- `SparseVoxelWorld::TrimQueuedStreamingLaneBackgroundBricks`
- `VENPOD_SPARSE_PREFETCH_QUEUE_PRESSURE_TRIM`
- `VENPOD_SPARSE_PREFETCH_QUEUE_PRESSURE_TRIM_THRESHOLD`
- `VENPOD_SPARSE_PREFETCH_QUEUE_PRESSURE_TRIM_BUDGET`
- `VENPOD_SPARSE_PREFETCH_QUEUE_PRESSURE_TRIM_KEEP_RADIUS_XZ/Y`
- `perf_noncapture_smoke.ps1 -PrefetchQueuePressureTrim*`

Validation artifacts:

- aggressive trim: `build/captures/prefetch_queue_pressure_trim_walk120_accepted_20260606`
- conservative trim: `build/captures/prefetch_queue_pressure_trim_b8_walk120_accepted_20260606`

Matched short-walk comparison against accepted-stack baseline:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `72.65/91.67` | accepted baseline |
| trim threshold4096 budget64 | `107.68` | `79.85` | `22.51` | `17.47` | `39.85` | `24.26` | `8.90` | `11.88` | `81.20/113.77` | rejected/removed |
| trim threshold8192 budget8 | `107.35` | `54.77` | `22.94` | `23.98` | `7.83` | `6.59` | `15.51` | `6.81` | `114.79/247.53` | rejected/removed |

Evidence:

- Aggressive trim reduced f120 prefetch generation-lane backlog from baseline `8389` to about `4359`, but frame time regressed badly.
- Conservative trim barely reduced f120 backlog (`8171`) and still regressed target/window frame time.
- Trimming already-admitted queued low-lane work causes request/generation/clip/surface/frame-gap churn rather than a useful scheduling win.

Build/test after removal:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warnings: `main_launcher.cpp` `rayDir` shadows a previous local.

Current next direction:

- Do not retry low-lane queued eviction/trim as the main fix.
- If broad prefetch debt is still the target, handle it before allocation/admission or through ownership-ticket budgets, not after work has entered generation/upload/surface queues.
- The strongest architectural next branch is admission/backpressure by ownership ticket and lifecycle stage: public/sample-visible same-frame readiness first; cache/prefetch admitted only within leftover budgets and with clear lane accounting through generation, upload/apply, surface extraction, and publish.

## Predicted Visible Admission Rejection - 2026-06-06

Retained default-off diagnostic surface:

- `VENPOD_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE_ADMISSION`
- `perf_noncapture_smoke.ps1 -MidClipmapPredictedVisibleAdmission*`
- `SparseClipmapTileCache::QueuePredictedVisibleVoxelInterest(...)`

Additional retained diagnostics/scheduler scaffolding after the follow-up slice:

- `SparseClipmapCacheStats` now exposes async visible reservation ticket counts:
  active, due, overdue, and max age.
- `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP` now logs
  `reservationTickets=active/due/overdue/hits/maxAge`.
- `perf_noncapture_smoke.ps1` parses those into:
  `midReservationTicketsActive`, `midReservationTicketsDue`,
  `midReservationTicketsOverdue`, `midReservationTicketHits`,
  `midReservationTicketMaxAge`.
- Predicted reservations now carry the predicted lead-frame deadline and sample
  index, duplicate reservations keep the earliest deadline, and async visible
  generation sorts current visible-critical work before reservation work, then
  sorts reservations by deadline.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Validation artifacts:

- sparse ray version, rejected: `build/captures/highalt_predicted_visible_admission128_incrementaltrim2048_20260606`
- current-interest projection version, rejected: `build/captures/highalt_predicted_interest_admission512_incrementaltrim2048_20260606`
- reservation projection version, rejected as candidate: `build/captures/highalt_predicted_reservation512_incrementaltrim2048_20260606`
- reservation hit diagnostic: `build/captures/highalt_predicted_reservation_ticketdiag512_20260606`
- deadline ticket 512, current-first final: `build/captures/highalt_predicted_deadline_ticket512_currentfirst_20260606`

Matched high-alt findings:

| Probe | f400 coverage | f402/f403 coverage | Window avg/max raw | Decision |
|---|---:|---:|---:|---|
| accepted-stack baseline | `99` | `86/84` | `53.64/53.74 ms` earlier broad stack window | baseline blocker |
| sparse ray predicted visible 128 | `99` | `86/85` | `96.70/105.48 ms` | rejected |
| projected future interest 512, inserted into current interest | `93` | `85/85` | `71.60/78.82 ms` | rejected |
| projected future reservation 512 | `99` | `86/84` | `126.41/136.44 ms` | rejected as candidate |
| reservation hit diagnostic 512 | `99` | `86/84` | `79.83/85.11 ms` | prediction proved mostly correct, throughput/deadline blocker |
| deadline ticket 512, current-first | `99` | `88/88` | `74.55/78.40 ms` | retained default-off architecture slice, not candidate |

Decision:

- Sparse ray sampling was too weak; it queued only `1` coord around f401 and did not address the f402/f403 collapse.
- Projecting full future interest proved the right diagnostic target because it queued hundreds of future coords, but inserting those coords into `m_voxelInterestSet` polluted current-frame coverage and is architecturally wrong.
- The reservation version fixed that pollution: f400/f401 returned to `99%` coverage while predicted coords stayed out of current interest.
- Reservation hit diagnostics show prediction is not the main miss: f402 had
  `1027/1125` current visible-critical misses already reserved, and f403 had
  `1115/1233` hits before deadline scheduling.
- Deadline-aware current-first scheduling improved the collapse to f402/f403
  `88/88`, while preserving f400/f401 `99/99`; however, it still regressed the
  high-alt window to `74.55 ms` raw average with upload/post-wait pressure.

Next measured blocker:

- Do not accept broad speculative future-interest generation as the high-alt fix.
- Keep the ownership lesson: future visible work must be represented as reservations/tickets that do not count against current public-frame coverage.
- The next viable branch is not better prediction; it is controlled promotion
  from reservation to resident/surfaced/uploaded state. The f402/f403 misses are
  mostly predicted, but only a small async/apply/upload/surface slice can land
  before the deadline. Next work should cap and stage deadline-critical tickets
  through generation, apply/upload, and surface extraction separately, so current
  public-critical work stays first and future tickets cannot create upload or
  post-wait spikes.

### Reservation Apply Throttle Slice - 2026-06-06

Retained default-off implementation:

- `SparseClipmapTileCache::ApplyAsyncNoncriticalVoxelGenerationCompletions(...)`
  now accepts optional `maxVisibleReservationApply`.
- Current visible-critical async results still use the existing visible-critical
  apply budget. Reservation-only visible async results can be capped separately
  and are requeued before pending ownership is cleared.
- `SparseClipmapCacheStats` now reports
  `asyncVisibleReservationAppliedLastFrame`,
  `asyncVisibleReservationApplyDeferredLastFrame`, and
  `asyncVisibleReservationApplyLimitLastFrame`.
- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` now logs
  `reservationApply=limit/applied/deferred`.
- New default-neutral env/harness knob:
  `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_RESERVATION_MAX_APPLY`,
  exposed as `perf_noncapture_smoke.ps1 -MidClipmapAsyncVisibleReservationMaxApply`.
- `perf_noncapture_smoke.ps1` now writes `run_manifest.txt` with the exact
  smoke command and bound parameters for future reproducibility.

Validation:

- `.\build.ps1 -Config Release`: passed; known pre-existing `rayDir` shadow
  warnings remain.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: only LF-to-CRLF warnings.
- Parser-only smoke pass: passed on
  `build/captures/highalt_predicted_deadline_ticket512_resapply8_footprint_20260606`.

Probe artifacts and decisions:

| Probe | Artifact | Key result | Decision |
|---|---|---|---|
| cap 8, missing cache-only deferral | `build/captures/highalt_predicted_deadline_ticket512_resapply8_footprint_20260606` | f400 `99`, f402/f403 `88/88`, window avg/max raw `108.41/150.82 ms`; async visible stayed `0`, reservationApply `8/0/0` | invalid comparison / not candidate |
| cap 8, attempted full-stack reconstruction | `build/captures/highalt_predicted_deadline_ticket512_resapply8_fullstack_20260606` | stayed startup-held through f400; no `PERF frame=400`, frame-end f400 raw about `15.33 ms` but public render was still held | invalid public comparison |
| no cap, same attempted full-stack reconstruction | `build/captures/highalt_predicted_deadline_ticket512_defaultapply_fullstack_20260606` | same startup-held failure mode as cap 8 | proves reproduction stack mismatch, not cap-specific |

Important caution:

- The old accepted/rejected artifacts did not record the exact harness command.
  Reconstructing the current-first high-alt stack from memory was error-prone:
  missing `-MidClipmapFootprintInterestSignature` changed interest churn, and
  adding `-MidClipmapCacheOnlyDefer` in the reconstructed command held startup
  through frame 400. Use `run_manifest.txt` for new artifacts before drawing
  conclusions.

Next measured blocker:

- Keep the default-off throttle and counters, but do not claim a performance
  win from it yet.
- First reproduce `highalt_predicted_deadline_ticket512_currentfirst_20260606`
  with an exact manifest-bearing command or add enough harness preset support to
  make that stack deterministic.
- Then re-test reservation apply caps (`8`, `16`, `24`) against that reproduced
  current-first baseline and only accept a cap if async-visible reservation
  deferrals actually appear while current visible coverage stays at least
  `99/99` f400/f401 and `88/88` f402/f403 with lower raw/body windows.

Follow-up retained slice:

- `perf_noncapture_smoke.ps1` now has `-StackPreset highalt-currentfirst`,
  with manifest `effectiveParameters`, managed startup public-render proof env,
  and explicit frame-end logging. The preset owns the high-alt current-first
  async stack instead of relying on ambient shell variables.
- `src/main_launcher.cpp` now distinguishes visible prepump projection from
  sync-budget eligibility. Async visible apply safety uses the fact that the
  prepump projected/tagged visible ownership this frame, not whether that
  prepump was allowed to drive the sync pump budget. This fixed a real stall:
  completed async-visible results could sit pending on low-coverage frames
  because `active=0` disabled apply even though the visible/reservation sets
  were valid.

Validation after follow-up:

- `.\build.ps1 -Config Release`: passed; known pre-existing `rayDir` shadow
  warnings remain.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: only LF-to-CRLF warnings.

Artifacts:

| Probe | Artifact | Key result | Decision |
|---|---|---|---|
| preset, uncapped apply-safe | `build/captures/highalt_currentfirst_preset_applysafe_20260606` | f400/f401 visible coverage `100/99`, f402/f403 `91/91`; f402 now applies completed async visible `0/24/24/0/0` with `reservationApply=4294967295/3/0`; window raw/body `144.40/146.12 ms` avg, max `197.74/197.70 ms` | retained mechanical fix, not performance candidate |
| preset, reservation cap 8 | `build/captures/highalt_currentfirst_preset_applysafe_resapply8_20260606` | cap exercised deferral: f400 `8/8/8`, f401 `8/8/16`, f402 `8/8/13`, f403 `8/8/5`; target raw improved to `127.65 ms`, but window raw/body stayed `145.38/145.96 ms`; f402/f403 coverage slipped to `90/89` | reject as candidate, keep throttle instrumentation |
| preset, reservation cap 16 | `build/captures/highalt_currentfirst_preset_applysafe_resapply16_20260606` | cap exercised but regressed badly: window raw/body `207.27/204.72 ms`, max `346.54/346.50 ms` | reject |

Decision:

- Keep the apply-safety fix and harness preset.
- Do not promote reservation apply caps as a candidate. Cap 8 reduces one target
  raw frame but does not improve the window and loses coverage; cap 16 is worse.
- The dominant blocker is now stage coupling after visible async apply: upload,
  surface extraction, post-wait/frame gaps, and sync parallel pump spikes still
  dominate. The next branch should pace reservation promotion through upload and
  surface stages with ownership budgets, not only cap voxel apply.

## Persistent Moving-Window Reservation / Parallel Pump Validation - 2026-06-06

Retained current architecture slice:

- Persistent async visible reservation map in `SparseClipmapTileCache`, with deadline/sample metadata and stale pruning.
- Moving-window reservation enqueue from `main_launcher.cpp`, default-off:
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_PRIORITY=1`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION=1`
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_RESERVATION_STALE_FRAMES=36`
- Startup public render moving-window proof must be enabled for public-frame comparisons:
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF=1`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_PROOF=1`
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION=1`
- Added behavior-neutral startup config telemetry to the existing `Sparse mid clipmap ...` log:
  `parallelPump`, `persistentParallelPump`, `parallelPumpWorkers`, `parallelPumpMin`,
  `sharedColumnCache`, `directFootprint`, and `parallelWorkerColumnCache`.

Rejected/invalidated during this slice:

- Current-visible async prequeue branch was removed earlier in this turn; it either starved moving-window reservations or fail-fast crashed.
- The first short-path parallel run without startup moving-window public proof was not a valid public comparison:
  `build/captures/pp480_20260606`.
  It reported f480 raw `27.68 ms`, but `SPARSE_STARTUP_PUBLIC_RENDER_HELD`,
  `active=0x5DF`, ownership off, surface raster off, and `gpuRay=0`.

Validation:

- `.\build.ps1 -Config Release`: passed; only existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Config probe: `build/captures/ppcfg_20260606`
  - startup config confirmed `parallelPump=1`, `persistentParallelPump=1`,
    workers `4`, min `8`;
  - early frames activated parallel pump, e.g. f0 `parallelPump=1/16/4/7.87`.
- Comparable public-open run: `build/captures/pppublic480_20260606`
  - public path active at f480: `PERF_BACKEND_PIPE active=0x7FF`, ownership on, surface raster on;
  - f480 raw/body: `83.83/78.30 ms`;
  - window f456..484 avg/max raw: `76.35/86.48 ms`, avg/max body `86.06/104.97 ms`;
  - f480 CPU/request/gen/clip/pump/mainThreadBrickGen/surfaceExtract/GPU ray:
    `41.24/15.61/15.66/9.96/5.60/5.60/14.65/20.55 ms`;
  - f480 visible-critical coverage `99`, missing visible `11`, missing cache `2486`;
  - moving window `samples/ready/missing/coords/coverage/reserve/queued:3/0/356/252/94/252/0`;
  - async visible `enq/complete/apply/drop/dup:11/16/16/0/0`;
  - parallel pump active at f480: `parallelPump=1/12/4/5.55`.

Trajectory:

- This is a real improvement over the earlier public f480 range (`~108-145 ms` raw), but it is still roughly `12 FPS` at the target frame and `~13 FPS` by local raw-window average.
- The remaining public-frame blockers are distributed: CPU request/generation/clip, surface extraction/stage, GPU ray/surface, and frame-gap/post-wait. Parallel pump helps the sync pump slice but does not solve 60 FPS.
- Next measured blocker should be the public-open f456..484 window, not startup/private-held rows. The most useful next branch is reducing public-frame surface extraction and GPU ray/surface cost while maintaining the moving-window ownership contract.

### Rejected Hidden-Critical Prepublish Surface Cap - 2026-06-06

Tried and removed a narrow default-off cap for post-open hidden-exact critical pre-publish surface extraction:

- temporary env: `VENPOD_SPARSE_HIDDEN_EXACT_MISS_POST_OPEN_PREPUBLISH_CRITICAL_SURFACE_MAX`
- temporary perf line: `PERF_SPARSE_PRE_PUBLISH_SURFACE_HIDDEN_CAP`
- artifact: `build/captures/hiddenprep16_pppublic480_20260606`

Matched against public-open baseline `build/captures/pppublic480_20260606`:

| Probe | f480 raw/body | window avg/max raw | surfaceExtract | pump/mainThreadBrickGen | missing visible | visible coverage | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| baseline public-open | `83.83/78.30` | `76.35/86.48` | `14.65` | `5.60/5.60` | `11` | `99` | retained baseline |
| hidden prepublish cap 16 | `96.83/91.07` | `100.26/132.79` | `12.09` | `10.48/10.48` | `62` | `98` | rejected/removed |

Evidence:

- The cap fired at f480: hidden critical extracted `16`, skipped `1`, hiddenCriticalCoords `34`.
- It reduced pre-publish surface extraction only modestly (`14.65 -> 12.09 ms`) but increased clip/pump/mid visible debt and worsened target/window frame time.
- Ownership miss/unsafe stayed `0`, but `lodParentHeld=1` and visible missing grew, so this is not an acceptable performance trade.

Conclusion:

- Post-open hidden-exact surface work cannot be solved by a local cap after it has already entered the critical pre-publish path.
- The next implementation should classify hidden repair earlier as a maintenance/repair ownership lane with separate request/generation/upload/surface/publish budgets, while preserving public-critical and sampled-visible completion.

Validation after removal:

- `.\build.ps1 -Config Release`: passed; known `rayDir` shadow warnings only.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Symbol search for the rejected env/perf line: clean.

## Low-Lane Admission Backpressure Rejection - 2026-06-06

Attempted and removed a second default-off branch at the pre-allocation request layer:

- `VENPOD_SPARSE_LOW_LANE_ADMISSION_BACKPRESSURE`
- `VENPOD_SPARSE_LOW_LANE_ADMISSION_BACKPRESSURE_PREFETCH_GEN_QUEUE`
- `VENPOD_SPARSE_LOW_LANE_ADMISSION_BACKPRESSURE_INCLUDE_CACHE`
- `perf_noncapture_smoke.ps1 -LowLaneAdmissionBackpressure*`

This branch skipped nonresident prefetch-lane admissions when the previous frame's prefetch generation queue exceeded a threshold, while leaving resident touches, public/visible streaming lanes, terrain-critical, collision, and edited requests untouched. It was the correct layer to test after queued trim failed, but the scalar policy still regressed.

Validation artifacts:

- threshold 4096: `build/captures/low_lane_admission_backpressure_walk120_accepted_20260606`
- threshold 8192: `build/captures/low_lane_admission_backpressure_t8192_walk120_accepted_20260606`

Matched short-walk comparison:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| admission threshold4096 | `96.24` | `72.45` | `17.09` | `21.15` | `34.19` | `19.05` | `17.76` | `6.44` | `3262` | `106.63/231.33` | rejected/removed |
| admission threshold8192 | `118.85` | `75.91` | `23.47` | `26.03` | `26.38` | `24.67` | `18.74` | `6.37` | `6544` | `102.88/125.35` | rejected/removed |

Evidence:

- threshold4096 fired hard from frame 10 onward, often skipping hundreds of prefetch admissions per frame.
- threshold8192 fired more conservatively and still regressed badly.
- Both runs reduced prefetch backlog relative to baseline but moved cost into request/generation/clip/pump/surface/frame gaps and increased critical/noncritical missing counts.

Build/test after removal:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Live-code symbol search for both rejected branches: clean.

Updated architecture conclusion:

- Simple scalar admission backpressure is not enough even at the right layer.
- The engine needs an ownership-ticket scheduler with per-stage reservations and completion contracts, not a threshold that blindly stops prefetch source-lane work.
- Next viable implementation should make request/admission choose from explicit ownership classes with budgets for `publicCritical`, `sampledVisible`, `hiddenRepair`, `cache`, and `prefetch`, and then carry the same ticket through generation, upload/apply, surface extraction, and publish readiness.

## Ownership Generation Stage Budget Scaffold - 2026-06-06

Implemented a narrow default-off architecture slice that extends the existing ownership-stage budget path upstream into CPU generation:

- `SparseVoxelWorld::PumpGenerationAroundForOwnershipCritical(...)`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGETS`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGET`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudgets`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudget`
- new perf line: `PERF_SPARSE_OWNERSHIP_GENERATION_BUDGETS`

Important behavior boundary:

- Existing `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS` still controls upload/surface ownership budgets.
- Generation-stage ownership budgeting is separately gated by `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGETS`.
- Defaults remain unchanged.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

Probe artifacts:

- first split, critical allowed to consume full generation budget:
  `build/captures/ownership_stage_generation_budget_walk120_accepted_20260606`
- revised split, reserved noncritical generation budget:
  `build/captures/ownership_stage_generation_budget_reserved_walk120_accepted_20260606`

Matched short-walk comparison:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| gen stage, critical-full | `113.78` | `71.88` | `20.19` | `22.60` | `29.06` | `13.28` | `16.14` | `17.94` | `9150` | `126.53/218.06` | rejected config |
| gen stage, noncritical reserve 8 | `117.99` | `74.87` | `25.65` | `24.63` | `24.57` | `14.57` | `14.31` | `12.82` | `8286` | `128.74/255.02` | rejected config |

Decision:

- The code scaffold is retained default-off because it is the first cross-stage ownership-ticket generation hook and is separately gated.
- The tested configurations are rejected as candidate stack members.
- Critical-only generation starves prefetch/cache progression and worsens gaps.
- A small noncritical reserve is still not enough; cost moves into request/generation/clip/surface/frame gaps.

Next blocker:

- The ownership-ticket idea needs real per-class quotas and measured demand, not a boolean critical/noncritical split.
- Next implementation should expose ticket ownership counts by generation queue (`publicCritical`, `sampledVisible`, `hiddenRepair`, `fallbackValid`, `prefetch`, `cache`, `unknownCritical`) and use those counts to choose explicit generation quotas before pumping.
- Until then, do not use `-OwnershipStageGenerationBudgets` in the strongest candidate stack.

## Ownership Generation Budget Final Rejection / Pending Ownership Accounting - 2026-06-06

This section supersedes the earlier "Ownership Generation Stage Budget Scaffold" note. The generation-stage ownership budget behavior was removed from launcher/harness and from the live public API after explicit quota variants also regressed.

Removed/rejected behavior surface:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGETS`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_GENERATION_BUDGET`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudgets`
- `perf_noncapture_smoke.ps1 -OwnershipStageGenerationBudget`
- temporary `PumpGenerationAroundForOwnershipCritical` / ticket-ownership generation helpers

Additional rejected probes:

| Probe | Target raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU ray | Prefetch gen lane | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | `65.86` | `38.56` | `13.55` | `9.66` | `15.34` | `5.85` | `7.96` | `17.38` | `8389` | `72.65/91.67` | baseline |
| critical-full generation split | `113.78` | `71.88` | `20.19` | `22.60` | `29.06` | `13.28` | `16.14` | `17.94` | `9150` | `126.53/218.06` | rejected/removed |
| noncritical reserve 8 | `117.99` | `74.87` | `25.65` | `24.63` | `24.57` | `14.57` | `14.31` | `12.82` | `8286` | `128.74/255.02` | rejected/removed |
| explicit multi-call quotas | `136.73` | `90.79` | `28.99` | `34.85` | `26.93` | `10.76` | `16.64` | `22.87` | `8801` | `145.25/242.05` | rejected/removed |
| single-pass quotas | `114.57` | `74.53` | `24.23` | `23.33` | `26.96` | `10.87` | `17.45` | `6.61` | `8546` | `150.86/869.49` | rejected/removed |

Retained low-cost diagnostic:

- `PERF_SPARSE_STREAMING_TICKETS` now logs `genPendingOwnership=public/sampled/hidden/cache/prefetch/fallback/unknown:...`.
- This is counted from the existing streaming-ticket stats loop for tickets with pending CPU generation, not by scanning the generation queue.
- A direct generation-queue scan was tested and rejected because even "passive" accounting regressed frame time badly (`build/captures/generation_queue_ownership_accounting_walk120_accepted_20260606`, f120 raw `141.75`, window avg/max `132.77/159.66`).

Accepted final accounting artifact:

- `build/captures/generation_pending_ownership_accounting_walk120_accepted_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `71.74/39.64/13.52/10.44/15.67/4.69/9.59/14.24`
- window f96..124 avg/max raw: `72.58/126.17`
- `genPendingOwnership` at f120:
  `public/sampled/hidden/cache/prefetch/fallback/unknown:0/127/0/0/8608/0/0`
- surface-ready publish pending remains `0`, pending lanes `0/0/0/0`

Validation after final cleanup:

- `perf_noncapture_smoke.ps1` parse check: passed.
- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

Architecture conclusion:

- Do not reintroduce scalar generation ownership quotas, repeated queue resorting, generation-queue scans, low-lane trim, or backlog-threshold admission skipping as the main path.
- The useful signal is that pending CPU-generation demand is overwhelmingly prefetch with a small sampled-visible slice, while frame cost is distributed across request, generation, clip/pump, surface, GPU, and gaps.
- Next work should implement ownership tickets with per-stage demand/reservation and completion contracts without walking giant queues repeatedly: admission should create durable ownership demand, public/sampled work needs readiness guarantees, and prefetch/cache should consume leftover generation/upload/surface capacity without causing visible catch-up churn.

## Streaming Ticket Stage Demand Accounting Scaffold - 2026-06-06

Implemented a default-off ownership architecture scaffold:

- `SparseVoxelWorldConfig::streamingTicketStageDemandAccounting`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_DEMAND_ACCOUNTING=0`
- `perf_noncapture_smoke.ps1 -StreamingTicketStageDemandAccounting`
- maintained `m_streamingTicketPendingStageOwnershipCounts[stage][ownership]`

Intent:

- Keep a durable, O(1)-read source of per-stage ownership demand inside the ticket lifecycle.
- Update demand counts on ticket touch, ownership promotion, stage completion, and removal.
- Leave normal behavior and accepted-stack accounting unchanged unless the flag is explicitly enabled.

Rejected during this slice:

- A protected-sort guard based on stage-demand counters. It changed later-stage queue ordering semantics and regressed:
  `build/captures/ticket_stage_demand_accounting_walk120_accepted_20260606`
  f120 raw `213.29`, window avg/max `119.10/213.29`.
- The guard was removed.

Validation:

- `perf_noncapture_smoke.ps1` parse check: passed.
- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

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

- Keep stage-demand accounting default-off as architecture scaffolding, not as a candidate or FPS win.
- Do not use demand counters to skip protected sorting without a stricter proof that queue ordering stays equivalent.
- Next implementation should consume this demand model in a real reservation path: per-stage public/sampled completion guarantees plus leftover prefetch/cache capacity, without scanning/sorting giant queues each frame.

## Generation Ownership Worklist Scaffold - 2026-06-06

Implemented another default-off ownership primitive:

- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipQueues`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES=0`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipQueues`
- persistent generation ownership worklists:
  `m_generationOwnershipWorklists[StreamingTicketOwnership]`
- indexed ownership entry table:
  `m_generationOwnershipWorklistEntries`

Intent:

- Provide a cheap future selection source for generation-stage reservations without rescanning `m_generationQueue`.
- Keep accepted-stack behavior unchanged unless explicitly enabled.

Important validation result:

- Ungated ownership alias maintenance was first tried and rejected immediately:
  `build/captures/generation_ownership_alias_scaffold_default_walk120_accepted_20260606`
  f120 raw `142.62`, window avg/max `125.19/171.39`.
- The alias maintenance was then gated behind `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_QUEUES`.
- A deque-backed active-path probe was also rejected:
  `build/captures/generation_ownership_alias_enabled_walk120_rejected_20260606`
  f120 raw `86.95`, window avg/max `80.96/99.23`.
- The implementation was then replaced with vector worklists plus a `BrickCoord -> {ownership,index}` table so normal removal is swap-remove O(1), with a fallback find only if an entry is already inconsistent.

Validation after indexed replacement:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

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

Decision:

- Keep generation ownership worklists default-off as scaffolding only.
- Do not claim this as a candidate or FPS win yet; the default-off repeat was spike-noisy and the enabled path is not consumed by a reservation scheduler.
- Do not revive the rejected `std::deque` alias/remove-all approach.
- Next stronger architecture direction: build a gated generation reservation pump that consumes the indexed worklists directly, guarantees public/sampled ownership first, and spends leftover capacity on prefetch/cache. Validate that it improves motion stability without increasing public missing, surface-ready backlog, or raw frame spikes.

## Generation Ownership Reservation Pump Attempt - 2026-06-06

Implemented a default-off reservation consumer for the indexed generation ownership worklists:

- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipReservations`
- `SparseVoxelWorldConfig::streamingTicketGenerationOwnershipReservationMax`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATIONS=0`
- `VENPOD_SPARSE_STREAMING_TICKET_GENERATION_OWNERSHIP_RESERVATION_MAX=64`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipReservations`
- `perf_noncapture_smoke.ps1 -StreamingTicketGenerationOwnershipReservationMax <n>`

Intent:

- Consume `PublicCritical`, `UnknownCritical`, `SampledVisible`, and `HiddenRepair` worklists before the old generation queue/class fallback.
- Keep the path gated and default-off.
- Add a cap so protected reservation does not have to consume the whole generation budget.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

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
- The important failure is architectural: capping the pre-pass does not create prefetch/cache progress because the fallback protected sort continues consuming visible/public after the cap.
- Next direction should not be "front-load protected then fallback." It needs one integrated generation scheduler that allocates explicit per-frame ownership shares: public/sampled minimums, then bounded visible repair, then prefetch/cache fill. That scheduler must consume indexed worklists or an equivalent O(1) bucket source and avoid sorting the giant queue every frame.

## Generation Ownership Share Scheduler Attempt - 2026-06-06

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

Implementation notes:

- The share scheduler replaces the rejected protected pre-pass when enabled.
- It scales public/visible/prefetch quotas for small generation calls so public minimums do not monopolize every call.
- It suppresses low-priority prefetch and reallocates that slice to visible ownership while sampled/unknown/hidden worklist debt is above the visible-debt gate.
- It still uses the same indexed worklists and O(1) normal removal path.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

Default-off safety probe:

- artifact:
  `build/captures/generation_ownership_share_defaultoff_walk120_accepted_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `92.84/71.31/18.91/14.57/37.80/36.51/7.20/13.64`
- window f96..124 avg/max raw: `76.83/99.07`
- generated lanes: cache/prefetch/visible/public `0/0/55/48`
- surface-ready publish pending `0`
- this run was noisy and not a performance claim; it verifies the new flags stay disabled by default.

Fixed-share probe before quota scaling:

- artifact:
  `build/captures/generation_ownership_share_default_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `71.82/34.13/15.90/11.62/6.59/5.43/8.20/12.46`
- window f96..124 avg/max raw: `76.64/125.33`
- generated lanes: cache/prefetch/visible/public `0/0/53/48`
- result: no prefetch moved because small generation calls were monopolized by public/visible quotas.

Scaled-share probe:

- artifact:
  `build/captures/generation_ownership_share_scaled_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `57.36/55.37/17.15/9.76/28.46/18.14/10.61/6.73`
- window f96..124 avg/max raw: `84.00/105.02`
- generated lanes: cache/prefetch/visible/public `0/15/36/48`
- async prefetch enqueued/applied `18/15`
- result: prefetch finally moved, but visible debt rose too high (`criticalMissing=223`), and surface extraction/window time regressed.

Visible-debt-gated share probe:

- artifact:
  `build/captures/generation_ownership_share_debtgate160_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `82.86/42.07/20.43/14.20/7.42/6.03/12.35/15.61`
- window f96..124 avg/max raw: `73.64/127.68`
- generated lanes: cache/prefetch/visible/public `0/11/37/48`
- async prefetch enqueued/applied `18/11`
- `criticalMissing=88`, surface-ready publish pending `0`
- result: correctness recovered versus scaled-share, and prefetch moved, but frame spikes and surface extraction regressed versus the indexed-worklist scaffold and accepted rough baseline.

Decision:

- Reject the share scheduler as an active candidate. Keep it default-off as a measured architecture branch.
- The useful proof is that indexed worklists can allocate cross-ownership CPU generation without rescanning, but low-priority prefetch generation immediately creates downstream surface/publish work and can destabilize the frame.
- Next direction should move beyond generation-only shares: prefetch/cache need lane-aware downstream staging, likely CPU-generated but upload/surface/publish-deferred or separately budgeted until visible debt and frame budget permit. Do not keep tuning generation shares alone.

## Low-Priority Downstream Deferral Attempt - 2026-06-06

Implemented a default-off downstream staging gate for low-priority generated payloads:

- `SparseVoxelWorldConfig::streamingTicketLowPriorityDownstreamDeferral`
- `SparseVoxelWorldConfig::streamingTicketLowPriorityDownstreamPromoteMax`
- `VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_DEFERRAL=0`
- `VENPOD_SPARSE_STREAMING_TICKET_LOW_PRIORITY_DOWNSTREAM_PROMOTE_MAX=16`
- `perf_noncapture_smoke.ps1 -StreamingTicketLowPriorityDownstreamDeferral`
- `perf_noncapture_smoke.ps1 -StreamingTicketLowPriorityDownstreamPromoteMax <n>`

Implementation notes:

- Only cache/prefetch generated payloads can be held after `GeneratedCPU`; visible/public still queue upload/surface immediately.
- Deferred generated bricks stay in `m_generated`, with record state `GeneratedCPU`, and are promoted once per frame from upload pop paths.
- Promotion queues upload and surface extraction, then extends the streaming ticket to upload/surface/publish stages.
- Eviction/upload completion cleanup now removes stale deferred-generated set entries.

Validation:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `perf_noncapture_smoke.ps1` parse check: passed.
- Known pre-existing warning remains: `main_launcher.cpp` local `rayDir` shadow warnings.

Share `48/32/32`, visible-debt gate `160`, downstream promote `4`:

- artifact:
  `build/captures/generation_share_downstream_deferral_promote4_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `51.36/64.03/17.55/10.64/35.83/25.66/9.02/16.01`
- window f96..124 avg/max raw: `74.96/99.04`
- generated lanes: cache/prefetch/visible/public `0/16/35/47`
- async prefetch enqueued/applied `18/16`
- `criticalMissing=210`, surface-ready pending `0`
- result: downstream deferral cut the share-scheduler max spike (`127.68 -> 99.04`) and kept surface max below `15 ms`, but visible debt became unacceptable.

Share `48/64/8`, visible-debt gate `160`, downstream promote `4`:

- artifact:
  `build/captures/generation_share_visible64_prefetch8_downstream_deferral_promote4_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `68.79/32.31/16.60/10.46/5.23/4.08/7.72/15.61`
- window f96..124 avg/max raw: `71.61/95.45`
- generated lanes: cache/prefetch/visible/public `0/12/36/48`
- async prefetch enqueued/applied `12/12`
- `criticalMissing=113`, surface-ready pending `0`
- result: best aligned downstream-deferral probe; it moves a small prefetch trickle and improves the prior share-scheduler spike profile, but still loses to the indexed-worklist scaffold (`65.96/76.09`) and is not an active candidate.

Share `48/64/4`, visible-debt gate `160`, downstream promote `2`:

- artifact:
  `build/captures/generation_share_visible64_prefetch4_downstream_deferral_promote2_walk120_20260606`
- f120 raw/CPU/request/gen/clip/pump/surface/GPU ray:
  `66.77/44.09/17.00/11.56/15.51/4.98/9.35/11.80`
- window f96..124 avg/max raw: `75.03/156.26`
- generated lanes: cache/prefetch/visible/public `0/11/45/47`
- async prefetch enqueued/applied `11/11`
- `criticalMissing=99`, surface-ready pending `0`
- result: rejected; lower trickle/promotion caused a worse frame spike.

Decision:

- Reject low-priority downstream deferral as an active candidate for now. Keep it default-off as architecture scaffolding.
- Do not keep sweeping local share/promotion values; the current design can move prefetch CPU work without immediate surface pressure, but it still cannot prove stable frame time or visible debt control.
- Next direction should add explicit observability/backlog accounting for `GeneratedCPU` deferred downstream work and then integrate upload/surface/publish budgets by ownership, instead of allowing promotion from upload pop paths to remain invisible in summaries.

## Resume Anchor - 2026-06-05

The active `/goal` is already set and must remain active until a formal completion state is reached. Do not replace this with a narrow one-pass diagnostic goal.

Operating rule:

- start from the latest accepted candidate stack and current artifacts;
- inspect the measured blocker before coding;
- implement only a default-off or behavior-preserving branch;
- validate fixed, walk/realtime, high-alt, and noncapture where possible;
- update this file before any final/compaction-sensitive handoff;
- do not stop just because one branch fails if the next measured branch is safe and obvious.

Current latest state:

- the active tool goal is `streaming_playability_real_fix_campaign_20260604`;
- the campaign has already rejected many isolated caps/thread/queue heuristics; do not re-run them as the main fix;
- background split `0.375`, clean throttle, backlog-aware pump, visible-critical prepump, async exact prefetch lane, parallel mid pump, request fast resident touch, and persistent exact workers remain default-off candidates;
- the current strongest stack still misses playable frame time: fixed is around `20-24 ms` raw, walk is around `60-80 ms` raw, and high-alt is around `48-53 ms` raw depending row;
- current measured blockers are no longer one simple bucket: fixed has hidden-exact generation and surface extraction bursts, walk has request/gen/clip/surface/GPU/frame-gap cost, and high-alt still has clip/pump/GPU/raw cost;
- `VENPOD_PERF_FRAME_END_LOG_INTERVAL` was added default-neutral so validation can use every-frame frame-end windows instead of fragile target-frame-only parsing;
- `perf_noncapture_smoke.ps1` now writes `window_summary.csv` and `window_table.md` from `PERF_FRAME_END` rows.

Current implementation posture:

1. First validate candidate windows with VSync off and `VENPOD_PERF_FRAME_END_LOG_INTERVAL=1`.
2. Pick the largest measured bucket from those windows, not from memory.
3. If the next branch is a local, behavior-preserving pipeline optimization, implement and validate it.
4. If isolated knobs keep moving cost between queues, stop knob work and implement or document the real ownership-aware streaming state-machine slice:
   - public-visible, sampled-visible, hidden/post-open repair, cache, and prefetch lanes;
   - generation/apply/upload/surface/publish budgets by lane;
   - worker payload generation only for cache/prefetch or CPU-proven fallback-valid work;
   - same-frame completion or readiness gating for visible/public/unknown lanes.
5. Do not final after diagnostics unless the next code step would weaken correctness, require a broad rewrite, or hit a real build/tool blocker.

Latest accepted-stack validation artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_framewindow_20260605`

Target-frame rows:

| Scenario | Raw ms | CPU ms | Request | Gen | Clip | Pump | Surface extract | GPU | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `19.58` | `17.89` | `3.82` | `13.13` | `0.93` | `0.00` | `20.60` | `5.67` | stream lanes `0/0` |
| walk realtime f600 | `58.94` | `48.30` | `12.55` | `12.87` | `22.86` | `11.62` | `9.78` | `17.85` | stream lanes `0/0` |
| high-alt f360 | `48.21` | `30.41` | `8.96` | `4.78` | `16.66` | `6.45` | `8.13` | `5.99` | stream lanes `0/0` |

Frame-window rows (`target-24` through `target+4`):

| Scenario | Avg raw | Max raw | Avg surface extract | Max surface extract | Avg upload | Avg post-wait | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed | `22.32` | `49.28` | `6.26` | `20.60` | `0.37` | `10.80` | fixed is near target but has exact/surface burst tail |
| walk realtime | `107.87` | `1290.46` | `9.62` | `10.62` | `6.33` | `23.22` | contains one `frame=601` gap outlier; target row still too slow without it |
| high-alt | `49.77` | `60.30` | `5.90` | `8.61` | `1.08` | `11.94` | clip/pump and steady gaps remain |

Important correction:

- `frameend_interval_validation_20260605` used the mixed persistent-mid/interest-2 probe stack and is not the accepted-stack baseline.
- Use `accepted_stack_framewindow_20260605` for the next implementation decision.

## Completion Rules

Do not stop after one diagnostic, one failed heuristic, or one partial improvement.

The goal is complete only when one of these is true:

1. Validated 60 FPS candidate: representative noncapture fixed and walk rows have total frame time `<= 16.67 ms`, with no visual/correctness regression.
2. Strong non-60 candidate: the strongest safe default-off stack is validated, the remaining top bottleneck is measured by scenario, and the next blocker is specific.
3. Hard architecture blocker: at least two plausible branches were attempted or ruled out by code evidence, and the exact missing ownership/streaming refactor is documented.
4. Hard tool/build blocker: build, test, shader compile, or runtime blocker prevents safe continuation after attempted fixes.

Not valid completion:

- "Added diagnostic; next pass should use it."
- "One heuristic failed; next pass should try something else."
- "Async remains blocked; next pass should prove ownership."
- "Candidate failed; next recommended action is obvious."
- "60 FPS not reached" without a measured next-bottleneck table.

## Clean-Harness / Parallel Mid Pump Validation - 2026-06-05

Build/test after the retained code cleanup:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Retained changes:

- `perf_noncapture_smoke.ps1` no longer forces `VENPOD_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP=1`.
- New harness switch: `-MidClipmapVisibleCriticalPrepump`.
- `src/main_launcher.cpp` now skips missing-brick projection/priority tagging unless a real consumer is active:
  - high-alt prepump can affect the frame,
  - visible priority/lane guard is enabled,
  - or missing-sample feedback is explicitly enabled.

Why this matters:

- The previous accepted-stack walk measurements were polluted by inactive prepump projection/priority tagging.
- Clean walk lane diagnostic at frame `600`: raw `63.51`, CPU `55.81`, request/gen/clip/pump `12.94/16.70/26.15/14.31`, surface extract `11.01`, GPU `12.22`.
- `MidInterestInterval=2` is rejected in the clean harness: raw `91.39`, CPU `125.58`, clip/pump `73.78/56.74`.

Current strongest clean default-off partial:

| Stack | Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | Surface extract | GPU | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| parallel mid pump | fixed | `384` | `23.80` | `22.31` | `4.50` | `17.00` | `0.80` | `0.00` | `20.87` | `6.04` | partial, not playable |
| parallel mid pump | walk realtime | `600` | `59.51` | `35.60` | `15.40` | `14.67` | `5.51` | `4.01` | `11.32` | `12.95` | strongest current clean walk row |
| parallel mid pump | high-alt | `398` | `51.66` | `26.21` | `4.94` | `5.18` | `16.09` | `9.74` | `3.78` | `10.25` | still high-alt blocked |

Rejected in this clean cycle:

- `MidInterestInterval=2`: made walk catch-up much worse.
- `-TerrainCriticalParallelGeneration` on top of parallel mid: activated but worsened walk to raw/CPU `63.32/48.43`.
- `-ParallelMidWorkerColumnCache` on top of parallel mid: CPU slightly lower (`36.71`) but raw/window worse than plain parallel mid.
- high-alt `-MidClipmapVisibleCriticalPrepump -MidClipmapVisibleCoverageGuardV2 -MidClipmapCacheOnlyDefer`: activated and proved cache debt (`missingCache` about `1864-1868`), but high-alt window regressed; not a stack member.

Visual check:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605/contact_sheet.png`
- Walk capture frames `560/580/600` showed no obvious white terrain holes or sky leaks, but terrain remains coarse/blocky.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/walk_lane_diagnostics_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/walk_parallel_mid_pump_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/validated_parallel_mid_stack_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/highalt_visible_critical_prepump_clean_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_parallel_mid_walk_20260605`

Current blocker after the clean cycle:

- This is not one more scalar knob. Fixed/walk/high-alt remain distributed across request, exact generation, surface extraction, residual GPU/post-wait, and high-alt clip debt.
- The next real implementation slice is persistent ownership-lane state across request, generation, upload/apply, surface extraction, and publication. Unknown sampled/fallback-invalid work stays guarded; only cache/prefetch or CPU-proven fallback-valid work can be async/budgeted.

## Non-Negotiable Guardrails

- Public-frame contract: every visible pixel must have a legitimate owner that agrees with deterministic terrain/water truth.
- Valid owners: exact sparse surface, valid mid voxel clipmap, valid Far-SVO, deterministic water, sky.
- Unknown fallback is not valid.
- Do not weaken correctness policy.
- Do not use `missScreenPct=0` / `unsafeNearMissPct=0` alone as success.
- Do not revive:
  - blunt mid pump cap
  - removed age-priority prototype
  - failed ring-only visible-critical heuristic
  - shader hash-bucket feedback
  - hidden-only prepublish caps
  - prefetch surface caps
  - low-lane surface caps
  - simple generation lane caps
  - signature/interval skip variants
  - inline generated surface
  - full visible-lane cache-only deferral
- Do not make default-off systems default-on without behavior-equivalence proof.

Defaults that must remain unchanged:

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

## Current Best Stack

Best validated default-off stack so far:

- `VENPOD_RENDER_QUALITY=playable`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_ENABLE=1`
- `VENPOD_RAYMARCH_BACKGROUND_PASS_SCALE=0.375`
- clean terrain prefetch throttle
- backlog-aware mid pump
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

## 2026-06-05 Goal Restart / Baseline

User asked to make the goal explicit, compaction-survivable, and resistant to early stopping.

Active tool goal confirmed:

```text
streaming_playability_real_fix_campaign_20260604: drive VENPOD from current default-off partial candidates to a validated playable candidate through real noncapture validation and measured engine fixes, with explicit anti-stop rules, persistent handoff updates, and no correctness weakening or default promotion without proof.
```

Baseline commands for this restart:

- `git status --short`: worktree is dirty with many existing tracked and untracked VENPOD changes; do not revert unrelated work.
- `.\build.ps1 -Config Release`: passed, `ninja: no work to do`; known trailing `vswhere.exe` warning after success.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

First code-path inspection for the next implementation slice:

- `SparseVoxelWorld::ApplyGeneratedBrickPayload()` preserves `generationRecord.streamingLane` into generated counters and streaming tickets, then queues upload and surface extraction.
- Upload packets carry both `residencyClass` and `streamingLane`.
- `StreamingWorkTicket` records `residencyClass`, `streamingLane`, and ownership class.
- Surface extraction queues are still primarily indexed by `SparseResidencyClass`; the existing `PumpSurfaceExtractionAroundTimedForClass()` can separate Edited/Collision/Visible/Speculative but not Cache/Prefetch/Visible/PublicCritical lanes inside the same visible class.
- Publish gating still treats non-speculative residency as exact-surface-required, so visible-class prefetch/cache work can still become surface/publish pressure unless it is demoted or lane-gated before it reaches those queues.

Implication:

- The next real branch should be lane-aware surface/publish accounting or a persistent ownership-lane state propagation check, not another scalar cap.
- The specific suspected collapse is: some prefetch/cache/hidden repair work becomes `Visible` residency for correctness/readiness reasons, then downstream surface extraction and page publish paths cannot budget it separately by `streamingLane`.
- A safe fix must keep PublicCritical/SampledVisible/UnknownCritical synchronous/guarded while allowing only Cache/Prefetch or CPU-proven fallback-valid work to be budgeted/deferred.

All remain opt-in/default-off.

Best current noncapture/window evidence:

| Row | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Max raw | Max CPU | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| baseline stack walk window | `79.78` | `48.63` | `10.81` | `8.15` | `29.65` | `90.19` | `61.03` | too slow |
| async exact prefetch lane | `74.92` | `42.59` | `13.16` | `7.01` | `22.40` | `98.91` | `68.74` | partial |
| async prefetch + parallel mid | `71.17` | `39.11` | `13.73` | `8.91` | `16.45` | `81.72` | `49.47` | useful |
| async prefetch + parallel mid + request fast touch | `64.32` | `32.90` | `12.38` | `7.17` | `13.33` | `72.81` | `40.91` | best current stack, not playable |

Representative best-stack frame rows:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.87` | `18.01` | `4.25` | `12.99` | `0.76` | `0.00` | `6.26` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `72.81` | `35.76` | `15.36` | `14.19` | `6.20` | `4.35` | `5.54` | `451/273` | `95` | `0/0` |
| high-alt | `360` | `52.49` | `30.92` | `8.82` | `5.58` | `16.51` | `4.59` | `11.30` | `1746/0` | `100` | `0/0` |

## Accepted Current Code Changes

Async exact generation prefetch-lane support is present and validated as lane-safe:

- `src/Simulation/SparseVoxelWorld.h`
- `src/Simulation/SparseVoxelWorld.cpp`
- `src/main_launcher.cpp`
- `perf_noncapture_smoke.ps1`

New knobs:

- `VENPOD_SPARSE_EXACT_ASYNC_PREFETCH_LANE=0`
- `VENPOD_SPARSE_EXACT_ASYNC_MAX_ENQUEUE_PER_FRAME=0`

Validation showed `asyncEnqueuedLaneVisible=0` and `asyncEnqueuedLanePublic=0` in sampled rows.

## Rejected Recent Branch

`VENPOD_SPARSE_REQUEST_SKIP_REDUNDANT_LANE_TOUCH` was implemented, validated, and removed.

Why rejected:

- skipped resident lane touches (`requestRedundantLaneTouchSkip=419` at walk f600)
- regressed walk 12-frame avg raw/CPU from `64.32/32.90` to `74.61/37.17`
- no remaining code references should exist

## Current Measured Next Blocker

Fixed f384 in the best stack is dominated by hidden-exact miss generation, not mid debt:

- `PERF_SPARSE_HIDDEN_EXACT_MISS_GENERATION frame=384 generated=121 budget=256 tracked=153 feedbackCoords=121`
- `PERF_SPARSE_HIDDEN_EXACT_MISS_UPLOAD frame=384 uploaded=121 budget=256 tracked=153`
- `PERF_SPARSE_HIDDEN_EXACT_MISS_PUBLISH frame=384 published=121 tracked=153 critical=121`
- `PERF_SPARSE_CPU_DETAIL frame=384 ... genMs=12.99`
- `PERF_SPARSE_STREAMING_LANES frame=384 ... genLane=cache/prefetch/visible/public:0/1798/121/0`

Next branch to inspect before patching:

Post-open hidden-exact repair scheduling. The likely safe slice is to keep startup/same-camera shader-unsafe/public-critical hidden exact repair synchronous, but route post-open non-public-critical hidden repair into a maintenance/prefetch lane or lower/budgeted generation path. This must not disable the startup unsafe gate or make hidden repair "safe" if it is public-critical.

Search first:

```powershell
rg -n "HIDDEN_EXACT|hiddenExact|HiddenExact|sparseHiddenExact|PERF_SPARSE_HIDDEN_EXACT|sparseHiddenExactMissGenerationBudget|sparseHiddenExactMissPostOpenGenerationBudget|sparseStartupHiddenExactMissGenerationBudget" src\main_launcher.cpp
```

Inspect around hidden-exact request and generation sections near:

- env setup around lines `1718-1909`
- tracking state around `3899-4185`
- request submission around `6685-6692`, `7242-7510`
- generation/upload/publish around `9878` and adjacent `PERF_SPARSE_HIDDEN_EXACT_MISS_*` logs

## Required Validation Discipline

For every accepted candidate:

1. Build: `.\build.ps1 -Config Release`
2. Tests: `ctest --test-dir build --output-on-failure -C Release`
3. Noncapture validation with fixed, walk, high-alt when applicable.
4. Record body/raw/total, CPU, request, gen, clip, pump, surface, trim, GPU, missing/sampled, coverage, miss/unsafe.
5. Reject/remove if visual/correctness regresses, visible/public critical ownership is weakened, or numbers only move by hiding work.
6. Update this file before final response or any likely compaction boundary.

Current required artifact directory:

`build/captures/streaming_playability_campaign_until_valid_candidate_20260604`

Required docs to keep synchronized:

- `active-goal-handoff.md`
- `handoff.md`
- `debug-handoff.md`
- `root.md`
- `debug.md`

## Immediate Resume Plan

1. Do not retry hidden-exact post-open prefetch lane or hidden-exact direct parallel generation; both were tested on 2026-06-05 and rejected.
2. Continue from the measured accepted-state best stack.
3. The current top noncapture buckets are still walk request/gen/clip and high-alt clip/request, with fixed still blocked by hidden-exact exact generation in CPU but not improved safely by the tested hidden-exact branches.
4. Next likely branch should target request/gen coupling or exact-generation pipeline architecture with a broader staged apply/worker queue, not another direct hidden-exact heuristic.

## Campaign Start Branches - 2026-06-05

Build/test after accepted-state removal:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- known `rayDir` shadow warnings remain

## Latest Resume Anchor - 2026-06-05 Mid Persistent Worker Slice

New default-off code retained:

- `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_PERSISTENT_WORKERS=0`
- harness switch: `-ParallelMidVoxelPumpPersistentWorkers`

Implementation summary:

- persistent worker pool for the existing parallel mid-voxel pump path in `SparseClipmap`
- active only when `VENPOD_SPARSE_MID_CLIPMAP_PARALLEL_VOXEL_PUMP=1`
- preserves same-frame generation/apply semantics; it removes transient thread creation only
- behavior is unchanged at default settings

Build/test after this code:

- `.\build.ps1 -Config Release`: passed
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim8192_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/mid_persistent_workers_interest2_trim4096_walk_probe_20260605`

Target-frame results:

| Candidate | Scenario | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | GPU ms | Miss/unsafe | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| persistent mid workers | fixed f384 | `20.88` | `18.59` | `3.73` | `14.07` | `0.79` | `0.00` | `6.07` | `0/0` | partial |
| persistent mid workers | walk f600 | `79.36` | `39.97` | `16.56` | `15.27` | `8.12` | `5.95` | n/a | `0/0` | CPU/clip better, raw noisy |
| persistent mid workers | high-alt f360 | `48.43` | `28.12` | `8.37` | `4.87` | `14.87` | `5.04` | `8.59` | `0/0` | partial |
| persistent mid workers + interest interval 2 | fixed f384 | `21.09` | `18.86` | `3.80` | `14.12` | `0.94` | `0.00` | `6.07` | `0/0` | not final |
| persistent mid workers + interest interval 2 | walk f600 | `63.63` | `38.51` | `13.82` | `12.34` | `12.33` | `10.01` | `15.67` | `0/0` | mixed |
| persistent mid workers + interest interval 2 | high-alt f360 | `49.54` | `19.61` | `7.96` | `4.81` | `6.83` | `4.55` | `9.99` | `0/0` | high-alt CPU win, raw not solved |
| trim scan budget 8192 | walk f600 | `67.32` | `39.04` | `10.79` | `13.19` | `15.03` | `12.66` | `15.58` | `0/0` | rejected as stack member |
| trim scan budget 4096 | walk f600 | `62.31` | `37.77` | `14.02` | `16.84` | `6.89` | `5.02` | `9.07` | `0/0` | marginal probe only |

Trim-budget branch decision:

- lowering `VENPOD_SPARSE_PRESSURE_TRIM_SCAN_BUDGET` reduced scan records (`65536 -> 16384 -> 8192`) and pressure trim time (`5.39 ms -> 3.84 ms -> 2.58 ms` in compared walk rows);
- the total frame did not move decisively because work shifted into terrain-critical hierarchy/generation/clip and frame gaps;
- keep as diagnostic/tuning evidence, not a validated stack fix.

Current blocker after latest probes:

- fixed still sits around `21 ms` raw with hidden/exact generation and surface extraction pressure;
- walk remains around `62-67+ ms` raw with mixed request, exact generation, mid clip/pump, surface extraction, GPU, and frame gaps;
- high-alt improves in CPU with the mid-worker/interest probe but remains around `48-52 ms` raw;
- no current candidate is close to 60 FPS.

Next measured branch:

- stop chasing scalar budgets unless tied to a specific bucket;
- inspect and implement a real lane-aware surface/publish or generation/apply architecture slice, because cap/thread/tuning variants have repeatedly shifted cost instead of removing it;
- if coding continues before a larger refactor, target one behavior-preserving pipeline overhead: shared trim budget across callers, surface-ready publish backlog accounting, or exact generation batching/apply overhead, then validate with fixed/walk/high-alt/noncapture.

Two branches were attempted and rejected, then removed from live code:

1. `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_PREFETCH_LANE`
   - idea: route post-open hidden-exact maintenance repair to prefetch lane and avoid direct sync hidden-exact generation
   - result: branch barely found maintenance work; fixed had only `1` prefetch request and `122` critical requests, walk had `0` prefetch requests
   - direct hidden-exact defers stayed `0`
   - performance regressed: fixed CPU/gen `17.99/13.18 -> 20.04/14.93`, walk raw/CPU `57.86/32.48 -> 78.37/35.92`
   - decision: rejected and removed

2. `VENPOD_SPARSE_HIDDEN_EXACT_PARALLEL_GENERATION`
   - idea: preserve hidden-exact critical work but batch direct hidden-exact generation through existing parallel exact generation helper
   - 4-worker/min-8 result: fixed CPU/gen improved `17.99/13.18 -> 13.23/7.60`, but walk regressed `32.48 -> 42.40` CPU
   - min-96 result: fixed CPU/gen improved `17.99/13.18 -> 12.97/7.56`; walk raw improved slightly `57.86 -> 55.91` but CPU regressed `32.48 -> 39.92`; high-alt CPU regressed `28.98 -> 32.10`
   - decision: mixed, not accepted as a real candidate, removed

Validation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_prefetch_lane_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_prefetch_lane_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_parallel_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_parallel_min96_candidate_20260605`

Fresh accepted-state baseline from this cycle:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `10.26` | `1746/0` | `100` | `0/0` |

## Campaign Branch Ladder Closure - 2026-06-05

Post-cleanup verification:

- `.\build.ps1 -Config Release`: passed before compaction after removing transient branches.
- `ctest --test-dir build --output-on-failure -C Release`: passed after compaction, `1/1`.

The latest accepted-state baseline remains:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | Pump ms | Surface extract/stage | GPU ms | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.96` | `17.99` | `3.91` | `13.18` | `0.89` | `0.00` | `26.50/2.35` | `6.56` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `57.86` | `32.48` | `11.31` | `12.60` | `8.56` | `6.59` | `9.58/2.57` | `13.41` | `427/262` | `95` | `0/0` |
| high-alt | `360` | `51.12` | `28.98` | `8.09` | `4.74` | `16.14` | `4.93` | `6.64/1.80` | `10.26` | `1746/0` | `100` | `0/0` |

Five more branches were attempted after the hidden-exact probes. None became part of the candidate stack:

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

- The campaign has now ruled out multiple isolated local knobs after real validation.
- Small queue, cache, surface, trim, and hidden-exact heuristics either regress representative walk/high-alt rows or move cost into another guarded bucket.
- The strongest default-off stack is still not playable: fixed is about `23 ms` raw, walk is about `58 ms`, and high-alt is about `51 ms`.
- The remaining blocker is architectural rather than a missing scalar: VENPOD needs an ownership-aware streaming state machine with per-lane budgets and safe apply/upload, not another single-frame heuristic.

Next real implementation slice:

1. Split streaming state into visible/public-critical, sampled-visible, hidden/post-open repair, cache, and prefetch lanes before request/generation/surface/publish.
2. Keep visible/public-critical and sampled fallback-invalid/unknown work synchronous or readiness-gated.
3. Move only cache/prefetch or CPU-proven fallback-valid work to staged worker generation.
4. Apply/upload generated payloads at frame boundaries with lane budgets, backlog age, and pressure accounting.
5. Preserve the current public ownership contract; unknown fallback remains critical.

If coding continues, do not start with another isolated flag. Start by making lane state persistent across request, generation, upload/apply, surface extraction, and publish queues, then validate with noncapture fixed/walk/high-alt rows.

## Generated-Lane Accounting Slice - 2026-06-05

Behavior-neutral code retained:

- `SparseVoxelWorldStats::generated*LaneBricksLastFrame`
- `PERF_SPARSE_GENERATED_LANES`
- `perf_noncapture_smoke.ps1` CSV columns:
  - `generatedLaneCache`
  - `generatedLanePrefetch`
  - `generatedLaneVisible`
  - `generatedLanePublic`

Build/test:

- `.\build.ps1 -Config Release`: passed, known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: no code whitespace errors, only existing LF-to-CRLF warnings.

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/generated_lane_accounting_v2_20260605`

Accepted-stack validation with generated-lane counts:

| Scenario | Frame | Raw ms | CPU ms | Request ms | Gen ms | Clip ms | GPU ms | Queued gen lanes prefetch/visible/public | Generated lanes prefetch/visible/public | Async applied prefetch | Missing/sampled | Coverage | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `24.54` | `18.76` | `3.98` | `13.99` | `0.77` | `6.71` | `915/121/0` | `24/121/0` | `24` | `0/0` | `100` | `0/0` |
| walk realtime | `600` | `67.95` | `55.11` | `17.91` | `16.07` | `21.10` | `17.24` | `4929/142/47` | `15/88/47` | `15` | `523/303` | `94` | `0/0` |
| high-alt | `360` | `51.02` | `28.72` | `8.27` | `5.14` | `15.31` | `6.37` | `6968/11/8` | `20/11/8` | `20` | `1769/0` | `100` | `0/0` |

Decision:

- The expensive fixed/walk generation work is mostly `Visible`/`PublicCritical`, not cache/prefetch.
- A broader cache/prefetch async generation expansion is not enough to solve the current frame cost.
- Unknown/sampled visible work still cannot be deferred or moved async without ownership proof.
- The next real fix must make visible/public exact generation faster while preserving same-frame ownership, likely via a persistent synchronous worker pool or a staged public-critical generation/apply path that can complete before presentation.
- Cache/prefetch async remains useful but is no longer the first blocker in these rows.

Do not misread this as a performance candidate. It is a retained architecture accounting slice that prevents the next pass from chasing the wrong lane.

## Parallel Exact Std Execution Candidate - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_std_execution_rejected_20260605.md`

Build/test:

- candidate `.\build.ps1 -Config Release`: passed, known `rayDir` warnings
- candidate `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`
- after removal, build and ctest passed again

Result:

| Scenario | Baseline raw/CPU/gen/clip | Candidate raw/CPU/gen/clip | Decision |
|---|---:|---:|---|
| fixed | `24.54/18.76/13.99/0.77` | `25.13/20.86/15.13/0.75` | rejected |
| walk realtime | `67.95/55.11/16.07/21.10` | `72.46/46.34/12.53/20.45` | rejected; CPU improved but raw worsened |
| high-alt | `51.02/28.72/5.14/15.31` | `49.42/33.21/5.57/19.07` | rejected; CPU/clip regressed |

Decision:

- removed the std-execution candidate code/env/harness switch
- no `VENPOD_SPARSE_EXACT_PARALLEL_STD_EXECUTION` symbol remains
- all defaults remain unchanged

Next:

- do not retry per-call thread-launch variants as the main fix
- visible/public exact generation still needs a real same-frame pipeline improvement, likely persistent workers or a lane-aware public-critical generation/apply stage

## Persistent Exact Workers Candidate - Partial Keep Default-Off - 2026-06-05

New default-off flag:

- `VENPOD_SPARSE_EXACT_PERSISTENT_WORKERS=0`
- harness: `-ParallelExactPersistentWorkers`
- only active when `VENPOD_SPARSE_EXACT_PARALLEL_GENERATION=1`

Implementation:

- adds a persistent exact-generation worker pool inside `SparseVoxelWorld`
- replaces per-batch thread creation only when the new flag is enabled
- still waits for generated bricks in the same frame and applies them before presentation
- does not move visible/public or unknown fallback work to deferred async ownership

Validation artifacts:

- 4-worker candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_candidate_20260605`
- 2-worker candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/parallel_exact_persistent_workers_2w_candidate_20260605`

Decision:

- 4 workers rejected as stack setting: high-alt CPU/clip regressed.
- 2 workers retained as a partial default-off candidate.

2-worker rows:

| Scenario | Raw | CPU | Request | Gen | Clip | Pump | GPU | Missing/sampled | Miss/unsafe | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `21.81` | `17.17` | `3.85` | `12.55` | `0.76` | `0.00` | `6.97` | `0/0` | `0/0` | improved, still over budget |
| walk realtime | `67.23` | `32.40` | `13.18` | `12.82` | `6.38` | `4.25` | `20.19` | `409/209` | `0/0` | CPU improved, raw still bad |
| high-alt | `51.21` | `28.80` | `8.71` | `4.74` | `15.34` | `4.20` | `13.16` | `1764/0` | `0/0` | roughly neutral CPU/raw |

Current strongest stack is still not 60 FPS.

Remaining measured blockers:

- fixed: CPU `17.17 ms` plus large surface extraction accounting (`21.33 ms`) keeps raw at `21.81 ms`.
- walk realtime: CPU `32.40 ms`, GPU `20.19 ms`, and frame gap keep raw at `67.23 ms`.
- high-alt: clip/pump and GPU remain material, raw `51.21 ms`.

Next safest branch:

- target surface extraction/staging accounting or the remaining walk GPU/frame-gap source only after confirming it is not capture/log parsing noise.
- do not add more exact-thread variants unless they change the pipeline architecture.

## Post-Open Surface Cap 1 ms - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_persistent_exact_2w_candidate_20260605`
- note: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/postopen_surface_cap1_rejected_20260605.md`

Reason tested:

- fixed frame `384` showed `PERF_SPARSE_PRE_PUBLISH_SURFACE extracted=139 elapsedMs=21.33 maxMs=40`
- the current `SurfaceExtractionMaxMs 1` cap does not bound post-open pre-publish surface extraction

Result:

- setting `VENPOD_SPARSE_POST_OPEN_PRE_PUBLISH_SURFACE_EXTRACTION_MAX_MS=1` reduced one extract slice, but kept a huge publish backlog (`queuedPublishes=8098`) and regressed representative rows
- walk CPU/clip became `47.09/14.56`
- high-alt raw/CPU became `69.45/39.02`

Decision:

- reject cap-only surface fix
- do not add it to the candidate stack
- next surface work must include surface-ready publish/backlog architecture, not just a lower post-open cap

## Background Pass Scale 0.25 - Rejected - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/background_scale025_persistent_exact_2w_candidate_20260605`
- note: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/background_scale025_rejected_20260605.md`

Result:

| Scenario | Scale 0.375 raw/CPU/GPU | Scale 0.25 raw/CPU/GPU | Decision |
|---|---:|---:|---|
| fixed | `21.81/17.17/6.97` | `31.78/20.52/5.96` | rejected |
| walk realtime | `67.23/32.40/20.19` | `75.21/45.98/11.97` | rejected |
| high-alt | `51.21/28.80/13.16` | `52.99/33.43/7.55` | rejected |

Decision:

- lower background scale improved GPU in walk/high-alt but worsened CPU/raw enough to reject it.
- keep the stack at background scale `0.375` until a visual/GPU-specific path is needed again.
- next branch should focus CPU generation/request/backlog.

## Persistent Surface Workers - Rejected and Removed - 2026-06-05

Implementation attempted:

- default-off persistent surface extraction worker pool behind the existing parallel surface extraction path;
- harness switch for `VENPOD_SPARSE_SURFACE_PERSISTENT_WORKERS`;
- no ownership deferral: extraction results were still applied on the same frame.

Build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Validation artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch16_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/persistent_surface_workers_4w_batch8_candidate_20260605`

Comparison against `parallel_exact_persistent_workers_2w_candidate_20260605`:

| Candidate | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| base persistent exact 2w | `21.81/17.17` | `67.23/32.40` | `51.21/28.80` | baseline |
| surface persistent 2w | `25.30/22.76` | `53.88/42.62` | `49.34/29.30` | rejected; fixed and walk CPU regressed |
| surface persistent 4w | `18.90/18.22` | `67.16/42.00` | `50.49/29.60` | rejected; fixed raw improved but walk CPU/clip regressed |
| surface persistent 4w batch16 | `21.84/19.62` | `60.95/44.43` | `47.96/30.80` | rejected; raw mixed, CPU/clip regressed |
| surface persistent 4w batch8 | `22.92/25.86` | `64.86/52.48` | `51.09/27.99` | rejected; fixed/walk CPU regressed |

Decision:

- removed the persistent surface worker code and harness flag;
- no `SURFACE_PERSISTENT`, `surfaceParallelPersistent`, or `PersistentSurfaceExtraction` symbols remain;
- do not retry surface parallelism as a thread-backend tweak.

## Diagnostic-Off Candidate Check - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/accepted_stack_noheavydiag_20260605`

Result:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | GPU | Miss/unsafe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed | `384` | `22.55` | approx `20.57 sparse prep` | `4.66` | `14.00` | `0.74` | `6.07` | `0/0` |
| walk realtime | `600` | `63.72` | `31.85` | `10.99` | `12.74` | `8.09` | n/a parsed row | `0/0` |
| high-alt | `360` | `52.95` | `27.91` | `8.84` | `4.68` | `14.38` | n/a parsed row | `0/0` |

Interpretation:

- heavy fallback diagnostics are not the primary playability blocker;
- turning them off does not make the current candidate playable.

## Bounded64 Comparison Check - 2026-06-05

Artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/bounded64_noheavydiag_candidate_20260605`

Rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed | `380` | `26.02` | `5.69` | `2.85` | `2.00` | `0.84` | not comparable to fixed frame `384` hidden-exact burst |
| walk realtime | `600` | `58.55` | `43.63` | `12.75` | `12.48` | `18.38` | raw improved vs strict diagnostic-on row, CPU/clip regressed |
| high-alt | `360` | `48.27` | `32.15` | `8.89` | `6.36` | `16.89` | raw improved, CPU/clip regressed |

Decision:

- bounded64 remains comparison-only/default-off;
- it is still not the performance culprit, but it is not a CPU fix.

## Current Campaign State - 2026-06-05

Build/test after rejected surface worker removal:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Current strongest stack remains the persistent exact 2-worker stack with background split `0.375`, clean throttle, backlog-aware pump, visible-critical prepump, async exact prefetch lane, parallel mid pump, request fast resident touch, and stats single flush. It is still not playable.

Measured remaining blockers:

- fixed: hidden-exact exact generation and surface extraction bursts keep raw around `22 ms`.
- walk: request/gen/clip plus GPU/frame gaps keep raw around `60-70 ms`; walk missing debt is mostly projected-visible and cannot be deferred without ownership proof.
- high-alt: visible-critical prepump avoids some over-broad debt, but clip/pump/GPU/raw remain around `48-53 ms`.

Next real engineering direction:

- stop isolated queue/thread/cap tweaks;
- implement the ownership-aware streaming state machine slice:
  - persistent lanes for public-visible, sampled-visible, hidden/post-open repair, cache, and prefetch;
  - generation/apply/upload/surface/publish budgets by lane;
  - worker payload generation only for cache/prefetch or CPU-proven fallback-valid lanes;
  - same-frame completion or readiness gating for visible/public/unknown lanes.

## Post-Compaction Local Branches Rejected and Removed - 2026-06-05

Post-cleanup build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- No symbols remain for `VENPOD_SPARSE_EXACT_COORD_BATCH_GENERATION`, `PumpGenerationForCoordsBatch`, `VENPOD_SPARSE_PRESSURE_TRIM_MISS_FEEDBACK_QUEUED_ONLY`, or `PERF_SPARSE_PRESSURE_TRIM_POLICY`.

Exact coordinate batch generation:

- artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_coord_batch_candidate_20260605`
- temporary branch batched hidden-exact feedback coords before exact generation.
- fixed f384: raw `22.92`, CPU `18.51`, request/gen/clip `4.53/13.22/0.75`, surface extract `24.73`, GPU `5.55`.
- walk f600: raw `89.44`, CPU `34.29`, request/gen/clip `15.19/12.55/6.53`, surface extract `9.80`, GPU `5.41`.
- high-alt f360: raw `50.61`, CPU `27.30`, request/gen/clip `8.17/4.62/14.50`, GPU `10.80`.
- decision: rejected and removed. Fixed generation did not drop, fixed surface extraction worsened, and walk raw regressed badly.

Pressure trim miss-feedback queued-only:

- artifact: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_miss_feedback_queued_only_candidate_20260605`
- temporary branch skipped resident pressure-trim scanning when miss feedback was the only pressure source and free pages were healthy.
- fixed f384: raw `25.12`, CPU `20.38`, request/gen/clip `4.82/14.74/0.81`, surface extract `21.54`, GPU `6.59`.
- walk f600: raw `64.14`, CPU `41.85`, request/gen/clip `11.42/13.55/16.87`, surface extract `10.69`, GPU `11.49`.
- high-alt f360: raw `49.43`, CPU `26.72`, request/gen/clip `8.02/4.57/14.12`, GPU `10.24`.
- policy log confirmed resident scan skipping in walk, but the local trim win did not become a playable-frame win.
- decision: rejected and removed. It improved one trim subpath but regressed fixed and left walk/high-alt far above budget.

Current campaign conclusion:

- no new accepted code stack member came from these two branches.
- the engine is past the point where isolated exact-generation batching or pressure-trim gating is likely to solve playability.
- next real fix should be a lane-aware streaming state-machine slice that coordinates request, generation, apply/upload, surface extraction, and publish by ownership lane.

## Upload Lane Budget Candidate Rejected and Removed - 2026-06-05

Build/test after cleanup:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_32_32_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_4_8_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/upload_lane_budget_8_16_highalt_optout_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/post_upload_guard_control_20260605`

Result:

| Branch | Fixed raw/CPU | Walk raw/CPU | High-alt raw/CPU | Decision |
|---|---:|---:|---:|---|
| fresh control | `25.61/28.65` | `69.03/48.41` | `49.13/27.22` | baseline for first probe |
| upload budget 8/16 | `22.12/18.42` | `66.65/36.50` | `52.95/29.63` | rejected; high-alt window regressed |
| upload budget 32/32 | `24.31/19.17` | `83.18/33.37` | `87.22/52.74` | rejected; high-alt/walk raw regression |
| upload budget 4/8 | `21.72/19.21` | `68.37/47.91` | `50.32/30.18` | rejected; not better than same-code control |
| post-guard control | `22.99/18.64` | `66.81/47.72` | `57.33/29.83` | same-code control |
| 8/16 high-alt opt-out | `24.66/19.68` | `63.53/49.63` | `50.60/34.89` | rejected; target raw moved, but CPU/window did not |

Decision:

- The upload-lane budget path was a real lane-aware architecture slice, but not a stable stack win.
- Fixed/walk target rows could improve locally, but high-alt or window behavior regressed and same-code control did not support accepting it.
- Removed `VENPOD_SPARSE_UPLOAD_LANE_BUDGETS`, `PopBestUploadForLane`, the harness switches, and parser columns.
- No `SPARSE_UPLOAD_LANE_BUDGET` symbols remain.
- The next useful slice is still ownership-lane state-machine work, but it must coordinate request, generation, upload/apply, surface extraction, and publish together rather than isolating upload queue selection.

## Exact Local Column Block Candidate Rejected and Removed - 2026-06-05

Post-cleanup verification:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- no `EXACT_LOCAL_COLUMN`, `ExactLocalColumn`, `exactLocalColumn`, `GenerateBrickWithLocalColumnBlock`, or `ExactLocal` symbols remain.

Artifacts:

- control: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_control_20260605`
- candidate: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/exact_local_block_candidate_20260605`

Result:

| Scenario | Control raw/CPU/gen | Candidate raw/CPU/gen | Decision |
|---|---:|---:|---|
| fixed f384 | `21.41/19.13/14.11` | `19.89/22.18/17.41` | rejected; raw moved but CPU/gen worsened |
| walk f600 | `55.66/36.42/11.17` | `58.70/52.03/18.20` | rejected; CPU/gen/request regressed hard |
| high-alt f360 | `44.66/22.17/4.55` | `46.16/25.39/6.64` | rejected; CPU/gen/raw regressed |

Window comparison:

- control avg raw: fixed `21.75`, walk `64.31`, high-alt `43.80`
- candidate avg raw: fixed `21.74`, walk `63.32`, high-alt `45.55`
- candidate worsened representative CPU/generation and high-alt window behavior.

Decision:

- removed the temporary flag `VENPOD_SPARSE_EXACT_LOCAL_COLUMN_BLOCK_GENERATION` and harness switch.
- retained only behavior-neutral `GenerateExactBrickForConfig(...)` centralization in `SparseVoxelWorld`.
- this rules out the local terrain-column cache replacement as a real fix; exact generation pressure remains a pipeline/state-machine problem, not this micro-optimization.

Current formal state:

- many plausible local branches have now been validated and rejected or retained only as default-off partials.
- the strongest default-off stack still does not reach playable frame time.
- the next coding step must be a coordinated ownership-lane state-machine slice, not another isolated cap/cache/thread tweak.
- if that slice cannot be implemented safely without a larger rewrite, the valid completion state is a hard architecture blocker with the exact refactor sequence, not another diagnostic-only pass.

## Formal Architecture Slice Plan - 2026-06-05

Current frame-blocking call graph:

1. `main_launcher.cpp` builds visible/critical/prefetch requests and touches `SparseVoxelWorld` residency/streaming lanes.
2. `SparseVoxelWorld::GenerateQueuedBrick` or parallel exact helpers generate CPU brick payloads.
3. `SparseVoxelWorld::ApplyGeneratedBrickPayload` immediately:
   - marks CPU-generated,
   - queues upload,
   - queues surface extraction unless the buried/empty fast path applies,
   - increments generated-lane stats.
4. Upload, surface extraction, and page-table publish run as adjacent but separately budgeted queues.
5. Shader/public ownership observes whatever combination of generated/uploaded/surfaced/published state made it through this frame.

Why local branches failed:

- request, generation, upload, surface extraction, and publish each have partial lane awareness, but there is no single per-brick ownership ticket that says:
  - this work is public-visible and must complete before present or hold readiness;
  - this work is sampled-visible but fallback-invalid/unknown and must remain critical;
  - this work is hidden/post-open repair and can be staged without disturbing public ownership;
  - this work is cache/prefetch and can be async/budgeted/deferred;
  - this work is CPU-proven fallback-valid and may be generated asynchronously.
- Because the queues are not governed by one state machine, optimizing a single queue moves pressure into the next queue or creates backlog/coverage regressions.
- Generated-lane accounting proved the dominant fixed/walk generation is `Visible`/`PublicCritical`, not cache/prefetch; async expansion for prefetch cannot solve the main rows.
- Fallback classifiers proved sampled/unknown visible debt cannot be deferred safely.

Next implementation unit, not another diagnostic:

### Streaming Work Ticket V1

Add a persistent per-brick work ticket owned by `SparseVoxelWorld`:

- `coord`
- `residencyClass`
- `streamingLane`
- `ownershipClass`:
  - `PublicCritical`
  - `SampledVisible`
  - `HiddenRepair`
  - `Cache`
  - `Prefetch`
  - `FallbackValid`
  - `UnknownCritical`
- `requiredStageMask`:
  - CPU generated
  - GPU upload queued/applied
  - surface extracted/known-empty
  - page table published
- `completedStageMask`
- `fallbackProof`:
  - none/unknown
  - finer
  - coarser
  - Far-SVO material-valid
  - water
  - sky
  - previous resident valid
- `editRevision`
- `requestFrame`, `lastTouchedFrame`, `lastSampledFrame`
- `deadlineFrame` for public/sampled lanes
- queue/backlog age and last failure reason

Default state:

- Default behavior unchanged.
- New mode must be behind `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0`.

Scheduler rules:

- `PublicCritical`, `SampledVisible`, and `UnknownCritical` are not deferred for performance. They either complete all required stages before public present or trigger readiness/guard behavior.
- `HiddenRepair` can be budgeted after public-critical work, but cannot masquerade as visible-ready.
- `Cache`, `Prefetch`, and `FallbackValid` may use async generation and staged apply/upload budgets.
- Unknown fallback is never async-eligible.
- Surface extraction and publish budgets are keyed by the same ticket ownership class as generation/upload, not by a separate local queue heuristic.

Validation plan for the first implementation:

1. Ticket-only shadow mode:
   - build tickets and stage masks beside the current pipeline;
   - log ticket counts by ownership/stage;
   - no behavior change.
2. Protected scheduling mode:
   - still use current generation/upload/surface/publish functions;
   - choose queue pops through the ticket scheduler;
   - public/sampled/unknown tickets stay protected;
   - cache/prefetch/fallback-valid tickets are budgeted.
3. Async/apply mode:
   - only cache/prefetch/fallback-valid tickets can enter worker queues;
   - uploads/surface/publish apply at frame boundaries with per-lane budgets and age counters.
4. Readiness integration:
   - public presentation must check unresolved public/sampled/unknown tickets, not broad cache footprint debt.

Minimum success target:

- fixed noncapture raw moves below the current `~21-25 ms` band without hidden/surface bursts;
- walk/high-alt raw reduces materially without holes or parent-held failure explosion;
- ticket backlog does not grow unbounded;
- `missScreenPct=0`, `unsafeNearMissPct=0`, and visual contact sheets stay clean;
- all new behavior remains default-off.

What not to do next:

- do not add another scalar cap to generation, upload, surface, or trim alone;
- do not add another thread backend that still waits for all visible/public work in one frame without stage ownership;
- do not expand async generation until a ticket is cache/prefetch/fallback-valid;
- do not defer sampled unknown walk debt.

## Streaming Work Ticket Shadow V1 - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=0`
- harness switch: `-StreamingTicketScheduler`
- log: `PERF_SPARSE_STREAMING_TICKETS`

Implementation:

- `SparseVoxelWorld` now has a shadow `StreamingWorkTicket` map when the flag is enabled.
- Tickets track:
  - residency class and streaming lane;
  - ownership class: public critical, sampled visible, hidden repair, cache, prefetch, fallback valid, unknown critical;
  - required/completed stages: CPU generated, GPU uploaded, surface ready, page published;
  - request/touch/update frame and edit revision.
- The shadow mode updates tickets at request allocation, generated-payload apply, upload complete, surface extraction/known-empty, page-table publish, residency/lane touch escalation, and eviction.
- No queue selection or deferral behavior changes yet.

Build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched files: only existing LF-to-CRLF warnings.

Validation artifact:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_shadow_v1_20260605`

Rows:

| Scenario | Frame | Raw | CPU | Request | Gen | Clip | Pump | GPU | Ticket result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `382` | `26.51` | `5.76` | `3.33` | `1.61` | `0.82` | `0.00` | `6.60` | active `796`, all prefetch pending CPU |
| walk realtime | `600` | `69.96` | `51.41` | `13.95` | `12.78` | `24.66` | `10.90` | `17.29` | active `4889`; sampled/prefetch `1213/3676`; pending CPU/upload/surface/publish `3228/1661/62/1661` |
| high-alt | `360` | `44.12` | `23.85` | `8.64` | `5.02` | `10.18` | `0.00` | `9.28` | active `6110`; public/sampled/prefetch `8/15/6087`; pending CPU `6110` |

Decision:

- keep ticket shadow mode default-off.
- this is not a performance fix yet; it is the first retained state-machine slice.
- it confirms the next scheduler must protect sampled-visible/public tickets while separately budgeting large prefetch/cache ticket debt.

Next implementation step:

- add protected ticket scheduling mode under the same default-off flag or a second flag:
  - existing queue pop functions should prefer public/sampled/unknown tickets first;
  - cache/prefetch tickets should obey budgets and age/drain accounting;
  - surface and publish queues must use the same ticket ownership as generation/upload;
  - behavior must be validated against fixed, walk, high-alt, and noncapture windows.

## Protected Ticket Scheduling and Follow-Up Probes - 2026-06-05

New default-off code retained:

- `VENPOD_SPARSE_STREAMING_TICKET_PROTECTED_SCHEDULING=0`
- harness switch: `-StreamingTicketProtectedScheduling`
- `PERF_SPARSE_STREAMING_TICKETS` now logs `protected` and `protectedSorts`

Implementation:

- protected scheduling only activates when `VENPOD_SPARSE_STREAMING_TICKET_SCHEDULER=1`.
- generation, upload, and surface queues can be sorted by ticket ownership/stage.
- public critical, unknown critical, sampled visible, hidden repair, and fallback-valid tickets outrank cache/prefetch.
- the refined helper returns early for cache/prefetch-only queues, preserving existing queue behavior there.
- no work is dropped or made default-on.

Build/test:

- `.\build.ps1 -Config Release`: passed after the refined helper.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched tracked files: no code whitespace errors, only existing LF-to-CRLF warnings.

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

- refined protected walk f600: raw/CPU `68.44/43.90`, request/gen/clip `17.86/16.40/9.61`, GPU `13.03`, miss/unsafe `0/0`.
- refined protected high-alt f360: raw/CPU `47.30/24.90`, request/gen/clip `9.11/6.22/9.57`, GPU `9.76`, miss/unsafe `0/0`.
- hidden exact gen32 fixed f360: raw/CPU/gen `23.13/11.68/6.73`; fixed window improved, but this is not a global stack because high-alt window regressed.
- incremental pressure trim did not reduce the stack; walk f600 still had `pressureTrimMs=5.74`.

Current decision:

- keep streaming ticket shadow and refined protected scheduling default-off as the latest state-machine slice.
- do not keep full hidden-exact budget throttling as a candidate.
- hidden-exact generation-only pacing is a mixed probe, not the global candidate stack.
- reject incremental pressure trim for this stack.
- no 60 FPS/playable candidate exists yet.

Current remaining blocker:

- the best safe stack is still mixed per scenario:
  - fixed can be pushed into the low-20 ms raw band but has hidden-exact/surface bursts;
  - walk remains about mid-60 ms raw with request/gen/clip/surface/GPU all material;
  - high-alt improves with protected scheduling but remains around mid-40 ms raw.
- the next code slice should make ticket scheduling control apply/upload/surface/publish pacing together, rather than using scalar caps that shift debt into the next stage.

## Streaming Ticket Stage Pacing Rejected - 2026-06-05

Attempted and removed:

- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING`
- `VENPOD_SPARSE_STREAMING_TICKET_STAGE_PACING_MIN_CAMERA_Y`
- `PopReadyByLanePriority` / `PopReadyProtectedLane`
- `PERF_SPARSE_STREAMING_TICKET_STAGE_PACING`
- harness switches for stage pacing

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_candidate_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_control_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/streaming_ticket_stage_pacing_highalt_only_candidate_20260605`

Window results:

| Row | Fixed avg/max raw | Walk avg/max raw | High-alt avg/max raw | Decision |
|---|---:|---:|---:|---|
| same-code control | `23.88/53.43` | `83.12/108.59` | `65.15/93.08` | control |
| broad stage pacing | `26.77/62.45` | `86.76/206.33` | `54.86/83.41` | rejected |

The high-alt-only gated probe was also rejected. It did not write parser summaries, but the high-alt log contains late raw outliers after frame `400`: `87.09`, `90.58`, `75.97`, `87.04`, `107.13`.

Cleanup verification:

- `.\build.ps1 -Config Release`: passed after removal.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on cleanup files: only LF-to-CRLF warnings.

Decision:

- page-publish selection alone is rejected and should not be restored.
- latest same-code control now points back to walk clip/pump/request/gen as the immediate blocker: walk f600 raw/CPU `83.47/65.82`, request/gen/clip/pump `18.98/16.92/29.90/27.55`.
- continue by inspecting the retained protected-ticket stack and the mid-clipmap pump path; do not stop at this cleanup.

## Request Accounting Split and Pressure-Trim Guard Rejection - 2026-06-05

Retained:

- richer `PERF_SPARSE_REQUEST_DETAIL` accounting:
  - `hierarchyOtherMs`
  - `preHierarchyMs`
  - true `otherMs`
  - `legacyOtherMs`
  - `pressureTrimPressure=free/gen/miss`

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/request_accounting_baseline_20260605`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/pressure_trim_queued_scan_pool_guard_candidate_20260605`

Walk f600 baseline:

- raw/CPU `91.48/62.52`
- request/gen/clip/pump `28.51/25.28/8.70/6.03`
- surface extract/stage `14.41/3.82`
- GPU `23.75`
- request split: pressure trim `9.31`, terrain critical `6.67`, stats `3.75`, hidden exact `3.63`, true other `3.88`

Rejected and removed:

- `VENPOD_SPARSE_PRESSURE_TRIM_QUEUED_SCAN_REQUIRES_POOL_PRESSURE`
- `-PressureTrimQueuedScanPoolGuard`

Reason:

- representative walk/high-alt pressure was miss-feedback driven (`pressureTrimPressure=0/0/1`), so the guard did not skip the queued scan.
- walk row regressed to raw/CPU `111.69/60.04`, with generation `29.10`.
- keep the accounting, not the guard.

Next branch:

- target hidden-exact/miss-feedback admission or generation/surface coupling rather than queued-trim gating.

## Async Low-Priority Apply / Persistent Mid Validation - 2026-06-05

Active goal remains open:

- `streaming_playability_real_fix_campaign_20260604`
- objective: reach a validated playable candidate or a measured hard blocker without weakening public-frame ownership

Retained code/logging:

- `VENPOD_SPARSE_EXACT_ASYNC_LOW_PRIORITY_MAX_APPLY_PER_FRAME=0` default.
- `perf_noncapture_smoke.ps1` supports `-AsyncExactLowPriorityMaxApplyPerFrame`.
- `PERF_SPARSE_EXACT_ASYNC` and campaign parsing report `lowPriorityMaxApply` and `deferredLowPriority`.
- low-priority exact async completions can be capped only when explicitly enabled; visible/public completions are not capped by this low-priority limit.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` on touched code: only existing LF-to-CRLF warnings.

Artifacts:

- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/table.md`
- `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- walk visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cap32_persistent_mid_walk_20260605/contact_sheet.png`
- high-alt visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cap32_persistent_mid_highalt_20260605/contact_sheet.png`

Rows use the noncapture harness with playable render quality, background split `0.375`, clean throttle, async exact prefetch lane, ticket scheduler/protected scheduling, explicit source lanes, request fast resident touch, critical reuse, and parallel mid voxel pump unless noted.

| Candidate | Scenario | Raw ms | CPU ms | Request | Gen | Clip/Pump | GPU | Avg/Max Raw | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| control | fixed | `22.30` | `18.53` | `4.23` | `13.58` | `0.71/0.00` | `6.61` | `22.51/53.25` | control |
| control | walk | `63.23` | `41.15` | `17.26` | `16.77` | `7.10/5.57` | `13.53` | `85.93/624.57` | control, big outlier |
| control | high-alt | `97.26` | `62.90` | `16.33` | `11.87` | `34.67/20.92` | `20.85` | `93.93/117.04` | control |
| cap8 | fixed | `23.72` | `22.42` | `4.59` | `17.06` | `0.75/0.00` | `7.54` | `23.76/53.87` | rejected; `504` deferred completions |
| cap8 | walk | `49.35` | `43.71` | `17.16` | `19.07` | `7.46/5.56` | `20.87` | `69.72/96.42` | rejected with cap8 debt |
| cap8 | high-alt | `65.54` | `38.04` | `8.96` | `6.72` | `22.35/12.80` | `11.10` | `58.57/73.27` | rejected with cap8 debt |
| cap32 | fixed | `24.85` | `18.53` | `4.38` | `13.37` | `0.78/0.00` | `5.87` | `23.33/52.65` | partial |
| cap32 | walk | `55.92` | `48.01` | `12.61` | `12.78` | `22.60/11.36` | `16.81` | `65.01/78.52` | partial |
| cap32 | high-alt | `56.24` | `37.21` | `8.27` | `6.72` | `22.21/12.87` | `13.58` | `55.64/75.46` | partial |
| cap32 + persistent mid | fixed | `23.49` | `19.26` | `4.61` | `13.93` | `0.72/0.00` | `6.12` | `23.00/51.02` | strongest partial |
| cap32 + persistent mid | walk | `52.74` | `43.95` | `12.24` | `12.62` | `19.07/8.25` | `9.57` | `60.90/71.20` | strongest partial |
| cap32 + persistent mid | high-alt | `58.03` | `34.17` | `10.24` | `7.65` | `16.28/6.11` | `11.43` | `56.00/63.54` | strongest partial |
| cap32 + persistent mid + exact2 | walk | `63.71` | `51.25` | `18.39` | `16.47` | `16.37/4.14` | `15.05` | `72.74/106.83` | rejected |

Visual verdict:

- walk contact sheet is coherent enough for continued perf work, but still coarse/blocky.
- high-alt contact sheet still shows the known bright white shoreline/terrain artifact and is not acceptable.
- `missScreenPct=0` and `unsafeNearMissPct=0` in sampled rows are necessary but not sufficient.

Decision:

- no validated 60 FPS candidate exists.
- cap8 is rejected because it improves some rows by creating fixed-camera low-priority apply debt.
- cap32 plus persistent mid workers is the strongest default-off partial stack from this cycle, but it is not playable.
- persistent exact workers with this stack are rejected.
- all defaults remain unchanged.
- next branch must address high-alt ownership/shoreline visual correctness and the streaming state machine; another scalar cap is unlikely to be the real fix.

## Hidden Exact Repair Lane and Cache-Only Defer Follow-Up - 2026-06-05

Active goal remains open. This was not a completion state.

Build/test before this follow-up:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- known `rayDir` shadow warnings remain.

New retained default-off knobs:

- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE=0`
- `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE=0`

Harness/logging:

- `perf_noncapture_smoke.ps1` supports `-HiddenExactPostOpenRepairLane`.
- `perf_noncapture_smoke.ps1` supports `-HiddenExactPostOpenWaterRepairLane`.
- new log: `PERF_SPARSE_HIDDEN_EXACT_REPAIR_LANE`.

Hidden exact repair-lane decision:

- non-water post-open hidden repair split is correct but ineffective for the current rows.
- fixed/walk post-open hidden-exact accepted feedback is mostly water feedback, so non-water split reports `repairAccepted=0` and leaves the old forced critical path active.
- water-inclusive repair lane reduces walk CPU but shifts pressure into upload/post-wait and grows hidden exact repair debt into thousands.
- do not promote either hidden repair lane; keep them diagnostic/default-off only.

Artifacts:

- non-water lane: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_repair_lane_candidate_20260605`
- water-inclusive lane: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/hidden_exact_water_repair_lane_candidate_20260605`
- water-inclusive high-alt visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_hidden_exact_water_repair_highalt_20260605/contact_sheet.png`

Water-inclusive rows:

| Scenario | Raw | CPU | Request | Gen | Clip/Pump | Surface Extract/Stage | GPU | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| fixed f380 | `28.21` | `14.45` | `9.05` | `4.55` | `0.84/0.00` | `11.06/3.37` | `10.29` | mixed |
| walk f600 | `85.91` | `31.75` | `15.59` | `7.38` | `8.76/6.67` | `11.31/2.05` | `14.32` | CPU improves, raw/upload debt worse |
| high-alt f400 | `67.33` | `47.01` | `11.96` | `5.97` | `29.08/7.19` | `6.67/1.64` | `12.60` | not solved |

Water-inclusive walk f600 details:

- `repairAccepted=86`, `criticalAccepted=0`, `forcedGenerated=0`, `forcedUploaded=0`, `priorityPublished=0`.
- `hiddenExactMissing=3068/0`, `hiddenExactAccepted=86`.
- `uploadLane prefetch=886`, `sparsePost upload=14.19`, `postWait=31.83`.

Cache-only visible-critical defer follow-up:

- `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=0` remains default-off.
- branch is not the failed ring heuristic; it uses projected/sample evidence from visible-critical prepump.
- high-alt cache-only defer is a scoped performance partial:
  - high-alt f400 raw/CPU `54.59/22.30`
  - request/gen/clip/pump `8.34/7.02/6.94/0.01`
  - `cacheOnlyDefer=1867`, `coverageVisibleCritical=99`, `coverageCache=79`
  - `parentHeldFailure=0`, `missScreenPct=0`, `unsafeNearMissPct=0`
- fixed is unchanged by cache-only defer because there is no missing mid debt.
- walk remains projected-visible, so cache-only defer stays inactive and is not a walk fix:
  - walk f600 raw/CPU `69.45/52.64`
  - request/gen/clip/pump `18.85/16.87/16.89/4.54`
  - surface extract/stage `12.51/3.00`
  - `cacheOnlyDefer=0`, backlog voxel `432`, max age `280`

Artifacts:

- perf rows: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/cache_only_defer_candidate_20260605`
- high-alt visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_20260605/contact_sheet.png`

Visual verdict:

- cache-only high-alt capture passes numeric ownership gates but still shows the large pale lower-screen terrain/shore band.
- image stats at frames `360/380/400` show sky-like percent around `41.4/38.0/41.2`; visual correctness is not signed off.
- therefore cache-only defer is a retained default-off diagnostic/performance partial, not a validated playable candidate.

Current state after this follow-up:

- no validated 60 FPS candidate exists.
- strongest partial stack is still scenario-dependent and default-off.
- high-alt over-broad cache debt can be reduced safely, but high-alt visual ownership remains unresolved.
- walk/realtime is not over-broad cache debt; it remains projected-visible and distributed across request, generation, clip, surface extraction, post-wait, and residual GPU.
- next real branch should not be another cap. It should target either:
  - the high-alt visual ownership/shoreline contract if pursuing high-alt correctness, or
  - request/generation/surface coupling for projected-visible walk debt under the ticket/state-machine model.

## High-Alt Background Split Exact-Band Ownership Finding - 2026-06-05

Active goal remains open:

- `streaming_playability_real_fix_campaign_20260604`
- no validated 60 FPS/playable candidate exists
- no defaults should be promoted from this finding

New diagnostic artifacts:

- normal high-alt cache-only visual: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_20260605/contact_sheet.png`
- owner mode 55: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_owner55_20260605`
- material mode 54: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_material54_20260605`
- targeted bad-pixel audit: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_badpixel_20260605/bad_pixels.csv`
- background reason mode 58: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode58_20260605`
- background reason mode 57: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_mode57_20260605`
- surface-fill high-alt visual probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/visual_cache_only_defer_highalt_surfacefill_20260605`
- surface-fill noncapture/perf probe: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_candidate_20260605`

Finding:

- the large pale lower-screen high-alt band is not primarily caused by cache-only defer, mid-clipmap pump scheduling, or over-broad cache debt.
- mode 57 reason sampling over the lower band counted mostly `far_svo_unavailable_or_rejected`, not legitimate sky.
- representative lower-band rays are downward from camera y about `385.68`, with water-plane intersections around `613-1274` units; sky ownership there is visually wrong even when miss/unsafe counters are zero.
- the root is the lower-res background split exact-band admission path.

Code path:

- `Renderer.cpp` clears sparse-near flag bit `8` for background split unless `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1`.
- `PS_Raymarch.hlsl` uses that bit as `sparseSurfaceRaymarchFill`.
- `BackgroundHitAllowedByExactNear` only allows mid/Far-SVO/far-water inside the exact/surface ownership radius when this fill bit is set.
- therefore, unstenciled exact-band pixels in the lower-res background pass can be prevented from using valid-looking lower owners and can fall through to sky-like output.

Surface-fill probe result:

- setting `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL=1` removes the pale high-alt lower band in the visual probe.
- high-alt frame `400` sky-like image percentage dropped from about `41.18%` to about `20.51%`.
- frame `400` layer mix shifted toward plausible water/background ownership:
  - `farWaterScreenPct` about `8.2461`
  - `waterContextScreenPct` about `12.2994`
  - `skyScreenPct` about `2.4502`
- this proves the visual artifact is an ownership admission problem in the split path, not a scheduler-only problem.

Safety failure:

- surface-fill is not validated and must not be promoted.
- high-alt surface-fill frame `400` reported `shaderUnsafeNonReady=198/0`, `shaderUnsafeContractNonReady=198`, and `shaderUnsafeContractSamples=249`.
- walk surface-fill frame `600` also reported `shaderUnsafeNonReady=83/0` and `shaderUnsafeContractNonReady=83`.
- surface-fill walk stayed slow: frame `600` raw/CPU about `62.13/49.03`, request/gen/clip/pump `18.77/13.77/16.46/6.01`, GPU about `16.94`.
- high-alt frame `400` improved visually and raw was about `48.76`, but exact `PERF` parsing was incomplete; this is not a performance sign-off.

Decision:

- keep `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL` diagnostic/default-off.
- do not call surface-fill a playable fix until the unsafe exact-contract debt is explained or eliminated.
- do not spend the next high-alt pass on mid-pump caps or cache-only scheduling; that is no longer the visual root.
- next implementation branch should be a validated background-fill ownership path:
  - either prove the surface-fill pixels are deterministic water/Far-SVO owners and exclude them from unsafe exact debt under a narrow contract,
  - or improve exact foreground/water readiness so those exact-band pixels are ready before background fill is exposed,
  - or add a narrower split-fill admission rule that allows only CPU/shader-proved deterministic water/Far-SVO, not arbitrary unknown fallback.

Current bottlenecks after this finding:

- high-alt visual: background split exact-band ownership admission and shader-unsafe contract debt.
- high-alt performance: cache-only defer can reduce over-broad cache debt, but visual sign-off is blocked by the ownership admission issue.
- walk/realtime: projected-visible streaming debt remains distributed across request, generation, clip/pump, surface extraction, upload/post-wait, and residual GPU; surface-fill does not solve it.

Do not repeat:

- do not promote surface fill as-is.
- do not treat the high-alt pale band as solved by `missScreenPct=0` / `unsafeNearMissPct=0`.
- do not return to scalar caps, ring-only visible-critical, stage pacing, pressure-trim gating, or hidden-exact water lane promotion for this issue.

## Surface-Fill Exact Repair / Cached Water Proof - 2026-06-05

Code changes in this pass:

- `src/main_launcher.cpp`
  - added default-off `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_WATER_PROOF=0`
  - added default-off `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_EXACT_REPAIR=0`
  - added deterministic-water proof for surface-fill exact-band shader-unsafe samples
  - cached deterministic-water proof results by brick and edit revision
  - extended `PERF_SPARSE_SHADER_UNSAFE_WATER_PROOF` with cache hits/misses/size/max and `computeMs`
- `perf_noncapture_smoke.ps1`
  - added switches for surface fill, water proof, and exact repair

Validation artifacts:

- uncached high-alt f480: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_highalt_f480_20260605`
- cached high-alt f480: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_highalt_f480_20260605`
- cached walk f600: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_walk_f600_20260605`
- cached walk + existing hidden-exact scan budget: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_exactrepair_cached_walk_budgetedhidden_f600_20260605`
- updated root campaign rows: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv` and `table.md`

Rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Contract result | Decision |
|---|---:|---:|---:|---:|---:|---:|---|---|
| uncached exact-repair high-alt f480 | `97.66/72.94` | `49.46` | `14.78` | `8.69/0.00` | `30.42` | `116.30/423.04` | raw `66`, water-proved `61`, repair `5` | rejected; proof CPU spike |
| cached exact-repair high-alt f480 | `57.03/32.60` | `16.95` | `8.98` | `6.65/0.00` | `18.70` | `63.14/88.16` | raw `31`, water-proved `27`, repair `4` | useful partial, not playable |
| cached exact-repair walk f600 | `77.12/185.37` | `142.32` | `21.02` | `22.00/9.64` | `26.69` | `86.70/219.96` | raw `92`, water-proved `39`, repair `48/53` | rejected movement stack |
| cached exact-repair walk + hidden budget f600 | `122.00/46.32` | `22.82` | `16.77` | `6.70/4.18` | `39.97` | `158.98/902.15` | raw `47`, water-proved `42`, repair `5` | rejected; raw/window regression |

Important findings:

- Water proof alone did not solve high-alt: initial high-alt f400 had `provenWater=0`, `remainingContractNonReady=228`.
- Surface-fill exact repair plus water proof converged high-alt contract debt by f480 to a small repair set, and cached proof removed the owner-feedback CPU spike:
  - f480 owner feedback `32.09 ms -> 4.12 ms`
  - request `49.46 ms -> 16.95 ms`
  - window max raw `423.04 -> 88.16`
- The cache is behavior-preserving inside the default-off water-proof branch because it is invalidated on `SparseEditStore::RevisionSerial()`.
- This is not a playable candidate:
  - high-alt cached f480 remains `57.03 ms` raw / `32.60 ms` CPU / `18.70 ms` GPU
  - walk f600 with exact repair collides with hidden-exact debt and remains far over budget
  - existing hidden-exact tracked scan budgeting improves the target CPU but worsens raw/window badly and is rejected for this stack

Decision:

- keep surface fill, water proof, exact repair, hidden-exact scan budgeting, background split, clean throttle, backlog pump, and bounded repair default-off.
- keep cached deterministic-water proof as a useful default-off safety/perf improvement for future surface-fill validation.
- do not call surface-fill exact repair a validated playable candidate.
- next measured blockers:
  - high-alt: residual raw/GPU/CPU plus visual/contact-sheet validation for the cached surface-fill path
  - walk: projected-visible exact/mid streaming debt and hidden-exact repair collision, not water-proof CPU
  - global: logging/diagnostic overhead can distort perf rows; compare no-heavy/compact log rows before attributing spikes to streaming work.

## High-Alt-Only Surface Fill / Incremental Pressure Trim - 2026-06-05

Code changes:

- added default-off `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL_HIGH_ALT_ONLY=0`
- changed launcher plumbing so surface-fill water proof / exact repair can be requested while surface fill is active only on frames where high-alt LOD admission is allowed
- `perf_noncapture_smoke.ps1` now supports `-BackgroundPassSurfaceFillHighAltOnly`

Artifacts:

- accepted fixed: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_highalt_only_pressuretrim_fixed_f380_20260605`
- accepted walk: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_highalt_only_pressuretrim_walk_f600_20260605`
- accepted high-alt: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/surfacefill_highalt_only_pressuretrim_highalt_f480_20260605`
- contact sheet: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/contact_sheet.png`
- updated campaign rows: `build/captures/streaming_playability_campaign_until_valid_candidate_20260604/summary.csv`, `table.md`, `playable_candidate_table.md`

Validation:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Window avg/max raw | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| fixed f384 | `25.18/19.28` | `5.04` | `13.38` | `0.84/0.00` | `8.21` | `27.40/60.28` | partial |
| walk f600 | `63.49/33.26` | `9.16` | `17.66` | `6.42/4.16` | `17.44` | `69.16/81.81` | partial |
| high-alt f480 | `55.10/24.35` | `7.99` | `9.38` | `6.98/0.00` | `17.57` | `57.96/75.45` | partial; visual plausible |

Rejected in this cycle:

- global surface-fill exact repair remains rejected for walk because it hit `185.37 ms` CPU / `142.32 ms` request at f600.
- surface-parallel extraction regressed walk CPU to `60.54 ms`.
- terrain-critical parallel generation did not activate useful work and regressed raw/window.
- background scale `0.25` lowered GPU but regressed CPU/clip/raw.

Decision:

- high-alt-only surface-fill gating is safer than global surface fill because walk no longer activates the hidden-exact repair collision.
- incremental pressure trim / free-page guard is a real default-off CPU win in this stack.
- this is a stronger visually checked default-off stack, not a 60 FPS candidate.
- all defaults remain unchanged.
- remaining top buckets are distributed: fixed generation/surface/post-wait, walk generation/surface/GPU/post-wait, high-alt GPU/post-wait plus residual generation/surface.
## Streaming Playability Real Fix Campaign - 2026-06-05 Continuation

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`
- `build/captures/streaming_playability_real_fix_campaign_20260605/table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/playable_candidate_table.md`
- `build/captures/streaming_playability_real_fix_campaign_20260605/contact_sheet.png`

Important correction from visual validation:

- incremental pressure trim at scan budget `4096` looked like a useful perf win in noncapture rows:
  - fixed f384 raw/CPU `23.80/22.31 -> 22.33/20.58`
  - walk f600 raw/CPU `59.51/35.60 -> 52.32/30.58`
- but the walk capture for the same stack failed terrain-critical readiness:
  - frame `604`: postMissing `3`
  - frame `606`: postMissing `7`
- the same walk capture without incremental pressure trim passed:
  - readyFrame `600`, postNonReady `0`
- therefore the `4096` pressure-trim candidate is rejected for this reduced stack. Do not call it a validated playable candidate.
- a repaired `16384` scan-budget trim row was also tested and rejected because it lost the useful win:
  - fixed f384 raw/CPU `26.26/22.66`
  - walk f600 raw/CPU `55.90/41.84`
  - high-alt f360 raw/CPU `51.27/29.69`

Additional branches tested after compaction:

| Branch | Result | Decision |
|---|---|---|
| persistent terrain column cache | cache hits existed, but fixed/walk targets worsened | rejected |
| async exact prefetch lane | enqueued/applied `0` in target rows because generation still collapsed into visible class | rejected |
| request fast resident touch | touched resident pages but did not reduce target request; fixed/high-alt worsened | rejected |
| parallel exact generation | slight fixed-only improvement; walk/high-alt not improved | rejected |
| explicit source lanes + async prefetch | exposed huge prefetch lane debt and helped high-alt target, but walk CPU/clip/backlog regressed | rejected |
| explicit source lanes only | walk regressed badly | rejected |
| streaming lane queue priority | fixed target improved slightly, walk CPU `45.81` and high-alt CPU `35.11` worsened | rejected |
| protected streaming ticket scheduler + trim | walk raw `78.41`, high-alt raw `61.42`; windows worsened | rejected |

Current best validated stack remains the previous default-off stack, not a new 60 FPS candidate:

- `VENPOD_RENDER_QUALITY=playable`
- background split `0.375`
- clean prefetch throttle
- critical reuse
- parallel mid pump with persistent workers
- no incremental pressure trim in the validated walk visual row

Representative rows:

| Scenario | Frame | Raw ms | CPU ms | Request | Gen | Clip/Pump | Surface extract/stage | GPU ray | Visual/contract |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| fixed | `384` | `23.80` | `22.31` | `4.50` | `17.00` | `0.80/0.00` | `20.87/2.27` | `6.04` | fixed capture passed |
| walk realtime | `600` | `59.51` | `35.60` | `15.40` | `14.67` | `5.51/4.01` | `11.32/2.74` | `12.95` | accepted-stack walk capture passed; trim capture failed |
| high-alt | `398` | `51.66` | `26.21` | `4.94` | `5.18` | `16.09/9.74` | `3.78/1.79` | `10.25` | readiness passed; known bright shoreline/terrain artifact remains visible |

Current root after this continuation:

- this is no longer a single scalar cap or queue-sort problem.
- source lanes and tickets exist, but the effective generation/upload/surface class still collapses broad prefetch/cache work into visible-class queues in movement rows.
- explicit source lanes revealed the debt (`genLane` mostly prefetch), but class scheduling still treated it as visible (`genClass` mostly visible), so local sorting/async variants either did nothing or regressed walk.
- async remains blocked for sampled fallback-unknown/invalid debt.
- pressure trim cannot be accepted unless it preserves terrain-critical readiness in walk.

Next real implementation slice:

1. split streaming source lane from ownership-critical class in `SparseVoxelWorld`.
2. carry the ownership class from request planning through CPU generation, upload/apply, surface extraction, and publish.
3. visible/sampled fallback-invalid or unknown bricks stay critical.
4. cache/prefetch bricks may be budgeted, generated async, and surfaced later only when they do not participate in the public-frame ownership contract.
5. keep separate queues and budgets for generation, upload, and surface extraction.
6. validate with noncapture rows plus terrain-critical capture checks before accepting any perf win.

Do not repeat:

- do not call incremental pressure trim `4096` safe for this stack.
- do not keep trying queue-order scalar tweaks, lane priority, ticket protected sorting, or pressure-trim scan-budget tuning as the main fix.
- do not move unknown sampled fallback to async.
- do not claim 60 FPS: the best validated walk row is still about `59.51 ms` raw.

### Ownership-Class Split Follow-Up - 2026-06-05

New default-off diagnostic/candidate flag:

- `VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS=0`
- `perf_noncapture_smoke.ps1 -PrefetchLaneSpeculativeClass`

What changed:

- `SparseVoxelWorldConfig` now has `prefetchLaneSpeculativeClass`.
- `TouchResidencyClassWithStreamingLane*()` maps `Visible + Prefetch` touches to `Speculative + Prefetch` when the flag is enabled.
- Existing genuinely visible/collision/edit/public-critical promotions are not downgraded; the pool only promotes residency class, so real visible touches still win.
- `PERF_SPARSE_STREAMING_LANES` now logs `prefetchSpeculativeClassActive` and `prefetchSpeculativeTouches`.

Validation artifacts:

- class split only: `build/captures/streaming_playability_real_fix_campaign_20260605/prefetch_speculative_worldclass_candidate`
- class split + async prefetch: `build/captures/streaming_playability_real_fix_campaign_20260605/prefetch_speculative_async_candidate`

Rows:

| Row | Raw/CPU | Request | Gen | Clip/Pump | GPU | Class/async result | Decision |
|---|---:|---:|---:|---:|---:|---|---|
| class split fixed f380 | `37.58/5.71` | `1.86` | `3.05` | `0.77/0.00` | `6.95` | prefetch moved to speculative queue | rejected; raw/post-wait worsened |
| class split walk f600 | `57.04/39.98` | `10.16` | `9.66` | `20.13/9.28` | `10.61` | genClass `3171/47/0/0` | rejected; CPU/clip worse |
| class split high-alt f360 | `57.19/31.57` | `8.79` | `7.14` | `15.62/6.22` | `10.88` | genClass `6227/22/0/0` | rejected; raw/CPU worse |
| class split + async fixed f384 | `20.56/20.82` | `4.54` | `15.24` | `1.02/0.00` | `6.41` | result/pending `24/24`, applied `7` | partial fixed-only |
| class split + async walk f600 | `62.18/44.70` | `12.00` | `12.33` | `20.35/9.41` | `6.01` | result/pending `73/73`, deferred `73` | rejected; walk regression/backlog |
| class split + async high-alt f360 | `51.29/29.28` | `7.54` | `6.62` | `15.11/5.86` | `8.97` | result/pending `185/185`, deferred `176` | not enough, backlog |

Conclusion:

- This proved the exact state-machine issue: moving prefetch source work out of visible class reduces request/gen but exposes post-wait/surface/upload and clip debt.
- Async prefetch now actually runs, but its result/apply backlog grows and walk still regresses.
- The next fix cannot be only request classification. It must add stage-specific critical/prefetch queues and budgets for upload/apply and surface extraction, with terrain-critical readiness validation.

Do not repeat:

- do not accept `VENPOD_SPARSE_PREFETCH_LANE_SPECULATIVE_CLASS=1` by itself.
- do not combine it with async prefetch as a playable candidate yet.
- do not treat lower CPU in one phase as success while raw/post-wait or readiness regresses.

### Post-Compaction Surface/Ticket Branches - 2026-06-05

Build/test after removing the rejected lane-stage prototype:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Known `rayDir` shadow warnings and trailing `vswhere.exe` warning remain.
- `rg` found no remaining `sparseSurfaceLaneStage`, `SURFACE_LANE_STAGE`, `SurfaceLaneStage`, `PumpSurfaceExtractionAroundTimedForClassAndLane`, or `PopFirstQueuedBrickOfStreamingLane` symbols.

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/surface_lane_stage_zero_prefetch_probe`
- `build/captures/streaming_playability_real_fix_campaign_20260605/streaming_ticket_protected_walk_probe`
- updated rows in `build/captures/streaming_playability_real_fix_campaign_20260605/summary.csv`, `table.md`, and `playable_candidate_table.md`

Rows:

| Branch | Scenario | Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU ray | Decision |
|---|---|---:|---:|---:|---:|---:|---:|---|
| lane-stage surface budget | fixed f384 | `18.77/19.47` | `4.18` | `14.49` | `0.78/0.00` | `19.52/1.99` | `5.75` | fixed-only partial |
| lane-stage surface budget | walk f600 | `65.98/35.18` | `11.29` | `12.33` | `11.55/9.57` | `11.45/2.81` | `9.57` | rejected; walk raw/window worse than accepted stack |
| lane-stage surface budget | high-alt f360 | `49.08/29.22` | `8.00` | `5.91` | `15.30/6.06` | `5.33/1.53` | `6.72` | partial only |
| zero-prefetch surface probe | walk f600 | `56.48/36.49` | `15.11` | `14.14` | `7.22/5.56` | `12.46/2.62` | `12.55` | rejected; visible surface work remains and window avg raw `60.96 ms` |
| protected ticket probe | walk f600 | `62.21/47.56` | `12.37` | `11.91` | `23.25/11.40` | `11.78/2.66` | `13.84` | rejected; clip/CPU worse than clean parallel-mid stack |

Decision:

- The lane-stage prototype was removed; no rejected symbols remain.
- Blind low-lane/prefetch surface deferral remains rejected.
- Protected ticket scheduling remains rejected as a movement candidate.
- Current best validated stack remains unchanged and still not 60 FPS: fixed about `23.80 ms`, walk about `59.51 ms`, high-alt about `51.66 ms` raw in representative rows.
- This satisfies the local-branch evidence for the hard architecture-blocker direction: lane fields, tickets, and queue sorting are present, but the engine still lacks a coherent ownership-critical state machine through generation, upload/apply, surface extraction, and publish readiness.

Next implementation, if continuing code:

1. Stop trying scalar lane/surface/ticket caps.
2. Implement a larger default-off state-machine slice that carries an ownership-critical class separately from source lane through request planning, CPU generation, upload/apply, surface extraction, page publish, and public readiness.
3. Keep critical sampled fallback-invalid/unknown work synchronous/guarded.
4. Let only cache/prefetch/fallback-valid work use separate budgets, async generation/apply, and independent backlog/age accounting.
5. Validate every performance win with noncapture rows plus terrain-critical capture checks.

## Ownership-Stage Queue Branch - 2026-06-05

Implemented a default-off ownership-stage slice:

- `VENPOD_SPARSE_OWNERSHIP_STAGE_BUDGETS=0`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_UPLOAD_BUDGET=8`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_SURFACE_BUDGET=8`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS`

Code shape:

- Added persistent upload/surface ownership queues to `SparseVoxelWorld`.
- Critical ownership is `PublicCritical`, `SampledVisible`, `UnknownCritical`, and hidden-repair ownership.
- Noncritical ownership is only `Cache`, `Prefetch`, or CPU-proved `FallbackValid`.
- Lane/residency touches refresh ownership queue aliases, so a later visible/public touch can promote pending work out of noncritical queues.

Build/test:

- `.\build.ps1 -Config Release`: passed with known `rayDir` warnings and trailing `vswhere.exe` warning.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Artifacts:

- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_stage_candidate`
- `build/captures/streaming_playability_real_fix_campaign_20260605/ownership_queue_explicit_lanes_candidate`
- updated `summary.csv` and `table.md`

Rows:

| Candidate | Scenario | Raw/CPU | Request/gen/clip/pump | Surface extract | GPU | Decision |
|---|---|---:|---:|---:|---:|---|
| ownership queue stage | fixed | `44.05/14.51` | `4.49/8.93/1.09/0.00` | n/a | `12.66` | rejected |
| ownership queue stage | walk | `117.65/79.63` | `23.31/21.38/34.92/17.99` | `14.51` | `15.52` | rejected |
| ownership queue stage | high-alt | `98.90/60.35` | `15.91/12.61/31.81/22.07` | n/a | `24.46` | rejected |
| ownership queue + explicit lanes | fixed | `48.94/12.13` | `3.49/7.80/0.84/0.00` | n/a | `14.63` | rejected |
| ownership queue + explicit lanes | walk | `90.49/80.23` | `26.46/26.00/27.73/11.56` | `12.69` | `25.78` | rejected |
| ownership queue + explicit lanes | high-alt | `84.48/49.17` | `12.79/8.82/27.55/19.26` | n/a | `11.89` | rejected |

Decision:

- The first implementation used queue scans and was invalid as a candidate because scan overhead dominated.
- Persistent ownership queues fixed the scan overhead, but the current accepted stack labels almost all queued stage work as visible/critical, so there is no safe noncritical work to budget.
- Explicit source lanes create prefetch queues, but total fixed/walk/high-alt rows still regress badly versus the current validated stack.
- This branch should not be promoted or used in the playable stack.

Next measured blocker:

- The problem is earlier than stage selection: request/generation is still classifying too much work as visible/current-critical, and sampled walk debt remains fallback-invalid/unknown.
- The next real fix must introduce a source-of-truth ownership state at request/admission time, not another upload/surface stage budget.

## Deferred Downstream Observability - 2026-06-06

Implemented default-neutral observability for low-priority `GeneratedCPU` downstream deferral:

- `SparseVoxelWorldStats::deferredGeneratedDownstreamPending`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamPendingCache`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamPendingPrefetch`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamPendingVisible`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamPendingPublicCritical`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamPromotedLastFrame`
- `SparseVoxelWorldStats::deferredGeneratedDownstreamStaleLastFrame`
- `PERF_SPARSE_DEFERRED_DOWNSTREAM`
- smoke CSV fields `deferredDownstream*`

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.

Validation artifact:

- `build/captures/deferred_downstream_observability_visible64_prefetch8_promote4_walk120_20260606`

Focused walk f120 row:

- raw/CPU: `70.38/95.11 ms`
- request/gen/clip/pump/surface/GPU: `20.22/17.26/57.59/46.40/11.41/7.28 ms`
- window avg/max raw: `72.20/126.89 ms`
- generated lanes: cache/prefetch/visible/public `0/12/56/47`
- deferred downstream pending: total `865`, lanes `0/759/23/83`
- deferred downstream promoted/stale: `0/0`
- `criticalMissing=184`, `nonCriticalMissing=120`, `surfaceReadyPublishPending=0`

Decision:

- Keep the observability. It is not an FPS candidate and should not be sold as progress toward 60 by itself.
- The run proves why the prior downstream deferral branch regressed: deferred generated payloads can be reclassified to visible/public ownership while still stuck in `GeneratedCPU`, and the current promotion path did not drain them in the measured window.
- Stop sweeping share/promotion scalars blindly. The next implementation should integrate upload/surface/publish draining with ownership state so upgraded visible/public deferred payloads are promoted immediately or protected by a public-frame correctness rule.

## Critical-Touch Deferred Downstream Promotion - 2026-06-06

Implemented ownership-aware downstream promotion for deferred `GeneratedCPU` payloads:

- Added `PromoteDeferredGeneratedDownstreamForOwnership(...)`.
- Added `PromoteDeferredGeneratedDownstreamForCoord(...)`.
- Added `PromoteDeferredGeneratedDownstreamIfCritical(...)`.
- Critical residency/lane touch paths now promote deferred `GeneratedCPU` payloads immediately after ticket ownership is updated.
- `PopBestUploadForOwnershipCritical(...)` now materializes matching deferred payloads before selecting ownership upload work.
- `PopUploadForCoord(...)` can rescue an exact deferred `GeneratedCPU` payload before upload.
- Exact-coordinate upload now removes ownership queue aliases as well as main/class aliases.
- Added a post-upload `PERF_SPARSE_DEFERRED_DOWNSTREAM phase=postUpload` line; the smoke parser already uses the last matching frame line, so CSV rows now report post-upload backlog when present.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.

Primary validation artifact:

- `build/captures/deferred_downstream_critical_touch_promote_visible64_prefetch8_promote4_walk120_20260606`

Primary walk f120 result:

- raw/CPU: `80.14/69.29 ms`
- request/gen/clip/pump/surface/GPU: `17.37/13.62/38.29/37.07/9.95/13.61 ms`
- window avg/max raw: `73.23/100.33 ms`
- deferred pending: total `698`, lanes `0/698/0/0`
- promoted/stale: `2/0`
- `criticalMissing=103`, `nonCriticalMissing=202`, `surfaceReadyPublishPending=0`

Ownership-stage validation artifact:

- `build/captures/deferred_downstream_critical_touch_ownership_stage_promote4_walk120_20260606`

Ownership-stage walk f120 result:

- raw/CPU: `82.89/33.53 ms`
- request/gen/clip/pump/surface/GPU: `20.29/12.37/0.85/0.00/10.64/6.25 ms`
- window avg/max raw: `97.21/161.59 ms`
- deferred pending: total `9`, lanes `0/9/0/0`
- `criticalMissing=0`, `nonCriticalMissing=0`
- ownership-stage upload: critical/noncritical/nonBudget `14/8/8`
- queued upload/surface remain prefetch-heavy: upload `663`, surface `375`

Decision:

- Keep the critical-touch promotion and post-upload observability. This fixes the specific ownership correctness bug where visible/public deferred payloads remained parked in low-priority `GeneratedCPU`.
- Do not accept either stack as a 60 FPS/playable candidate. The primary stack still runs around `13.7 FPS` by window average, and ownership-stage budgets still create large raw/post-wait instability despite clean critical missing at f120.
- Next architecture work should attack prefetch-heavy upload/surface/publish backlog and post-wait behavior with ownership-preserving budgets, not weaken the new critical promotion rule.

## Ownership-Stage Publish Budget Scaffold - 2026-06-06

Implemented default-off publish-stage ownership control:

- `SparsePagePublishQueue::PopReadyForOwnershipCritical(...)`
- `VENPOD_SPARSE_OWNERSHIP_STAGE_PUBLISH_BUDGET`
- `perf_noncapture_smoke.ps1 -OwnershipStagePublishBudget <n>`
- ownership-stage publish loop now pops critical publishes first and caps noncritical publishes by the new budget.
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS` now logs `publish=critical/noncritical/nonBudget`.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.

Budget sweep before/around the publish scaffold:

| Artifact | Upload/surface/publish | f120 raw/CPU | crit/noncrit | window avg/max raw | postWait avg/max | Decision |
|---|---:|---:|---:|---:|---:|---|
| `ownership_stage_noncritical0_walk120_20260606` | `0/0/unbounded` | `69.11/34.62` | `109/119` | `70.18/191.04` | `18.46/26.30` | rejected; public correctness regressed |
| `ownership_stage_upload8_surface0_walk120_20260606` | `8/0/unbounded` | `87.97/41.56` | `10/11` | `100.75/182.98` | `38.65/54.01` | rejected; nearly correct but unstable |
| `ownership_stage_upload8_surface1_walk120_20260606` | `8/1/unbounded` | `104.82/42.69` | `10/1` | `108.14/196.38` | `44.15/54.98` | rejected |
| `ownership_stage_upload8_surface2_walk120_20260606` | `8/2/unbounded` | `120.07/50.11` | `0/0` | `122.56/214.47` | `49.64/63.25` | rejected; correctness restored at too high a cost |
| `deferred_downstream_critical_touch_ownership_stage_promote4_walk120_20260606` | `8/8/unbounded` | `82.89/33.53` | `0/0` | `97.21/161.59` | `40.28/46.09` | rejected; correctness clean but not playable |
| `ownership_stage_publish_budget8_walk120_20260606` | `8/8/8` | `77.89/136.99` | `8/0` | `96.06/206.59` | `41.90/48.46` | rejected; publish cap alone not enough |
| `ownership_stage_publish_budget0_walk120_20260606` | `8/8/0` | `79.72/152.09` | `0/0` | `99.81/198.04` | `39.15/43.67` | rejected; critical clean but clip/pump/post-wait still bad |

Decision:

- Keep the publish budget scaffold default-off. It completes the ownership-stage API shape across upload, surface, and publish.
- Do not promote any ownership-stage publish-budget stack as playable.
- Publish count is not the sole cause of the stall: even publish `0` still has high clip/pump and post-wait with large prefetch upload/surface backlog.
- Next work should target the mid-clipmap/clip pump interaction with prefetch-heavy upload/surface debt, while preserving critical-touch downstream promotion and critical-first publish selection.

## Visible-Critical Mid Cache Defer Terrain Prefetch Throttle - 2026-06-06

Implemented and validated a request-side throttle that uses the previous frame's visible-critical mid-clipmap proof:

- Added carried state in `main_launcher.cpp`:
  - `sparseMidClipmapCacheOnlyDeferForTerrainThrottleLastFrame`
  - `sparseMidClipmapVisibleCriticalCoverageForTerrainThrottleLastFrame`
  - `sparseMidClipmapVisibleCriticalMissingForTerrainThrottleLastFrame`
- Terrain surface prefetch now skips when:
  - `VENPOD_SPARSE_MID_CLIPMAP_CACHE_ONLY_DEFER=1`
  - previous mid prepump deferred only cache work
  - visible-critical mid missing is `0`
  - visible-critical coverage is at/above the mid pump coverage threshold
  - ownership miss and unsafe-near-miss are both `0`
  - valley atmosphere is within the existing clean threshold
- Existing legacy clean throttle still requires startup-open/public-clean; the mid-cache throttle can fire during startup catchup because it is gated by the visible-critical proof.
- `PERF_SPARSE_CPU_DETAIL` now appends `terrainPrefetchMidCacheThrottle=<0|1>`.

Build/test:

- `.\build.ps1 -Config Release`: passed after final code shape.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` shadow warnings remain.
- `git diff --check` only reports existing CRLF conversion warnings.

Accepted validation artifact:

- `build/captures/ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_walk120_20260606`

Accepted walk f120 result:

| Raw/CPU | Request | Gen | Clip/Pump | Surface extract/stage | GPU | Window avg/max raw |
|---:|---:|---:|---:|---:|---:|---:|
| `34.51/26.27 ms` | `19.33` | `4.76` | `2.17/0.00` | `1.03/0.50` | `1.04` | `34.51/34.51` |

Key proof lines:

- `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP frame=120 ... cacheOnlyDefer=3586 ... missingVisibleCritical=0 ... coverageVisibleCritical=100 ... missScreenPct=0 unsafeNearMissPct=0`
- `PERF_SPARSE_CPU_DETAIL frame=120 ... terrainPrefetch=...:0.00/0/0/0/0/1 terrainPrefetchMidCacheThrottle=1`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS frame=120 ... missScreenPct=0 unsafeNearMissPct=0`

Comparison:

- Before this fix, best visible-critical/cache-defer run was roughly `75.25 ms` raw at f120 (`~13 FPS`) with terrain prefetch around `33-35 ms`.
- The first persisted-state attempt still failed because startup-open was folded into the shared clean predicate: `64.43 ms` raw, `terrainPrefetchMidCacheThrottle=0`.
- The accepted split predicate drops f120 to `34.51 ms`, roughly `29 FPS`.

Rejected probes this turn:

| Artifact | Result | Decision |
|---|---:|---|
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle2_walk120_20260606` | `64.43 ms` raw, terrain prefetch still `29.40 ms`, throttle `0` | rejected; startup gate blocked the new path |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_reuse_walk120_20260606` | `35.52 ms` raw, terrain critical reuse still `0` | rejected; script switch did not activate reuse and no win |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_startup_reuse_walk120_20260606` | `35.15 ms` raw, reuse still `0` | rejected and reverted; unproven startup reuse relaxation |
| `ownership_stage_publish0_mid_visible_cache_defer_terrain_throttle3_noheavy_walk120_20260606` | `57.19 ms` raw, request `28.50 ms` | rejected; disabling heavy diagnostics worsened this setup |

Current rough FPS and trajectory:

- Current best validated rough FPS at walk f120 is about `1000 / 34.51 = 29 FPS`.
- This is materially better than the previous `~13 FPS` visible-cache-defer baseline, but still not 60 FPS.
- The trajectory is good because the last change removed an entire speculative prefetch wall without weakening public-frame checks.
- The next measured wall is request-side critical/repair work:
  - `PERF_SPARSE_REQUEST_DETAIL frame=120 reqMs=19.33`
  - `terrainCriticalMs=10.97`
  - `hiddenExact=4.10`
  - `pressureTrimMs=1.87`
  - `statsFlushMs=0.94`
- Next work should not restart broad scalar sweeps. Add diagnostics or ownership-aware reuse for terrain critical only after proving why the existing ready-footprint reuse is not valid/firing, then attack hidden-exact repair and pressure trim separately.

## Terrain-Critical Reuse Frame-State Probe - 2026-06-06

Implemented default-off terrain-critical reuse instrumentation and experimental frame-state maintenance:

- `VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_FRAME_STATE=0`
- `VENPOD_SPARSE_TERRAIN_CRITICAL_REUSE_REPAIR_ONLY=0`
- `perf_noncapture_smoke.ps1 -TerrainCriticalReuseFrameState`
- `perf_noncapture_smoke.ps1 -TerrainCriticalReuseRepairOnly`
- `PERF_SPARSE_REQUEST_DETAIL` and `PERF_SPARSE_TERRAIN_CRITICAL` now include:
  - `reuseRepair`
  - `reuseReject`
  - `reuseCached`
  - `reuseBad`

Important code finding:

- The terrain-critical ready-footprint cache was maintained inside the runtime logging block, so it could be stale or missing on non-logged frames.
- The new frame-state path can maintain that cache every frame, but it is default-off because the measured repair-only behavior is not yet a stable FPS win.

Build/test:

- `.\build.ps1 -Config Release`: passed.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Existing `rayDir` warnings remain.

Key probes:

| Artifact | f120 raw | Request | Terrain critical | Result |
|---|---:|---:|---:|---|
| `terraincritical_reuse_frame_state_walk120_20260606` | `43.29 ms` | `21.77` | `11.19` | rejected; frame-state proved cache had `946` coords but one bad coord forced full ray scan (`reuseReject=0x800`) |
| `terraincritical_reuse_repair_only_walk120_20260606` | `62.97 ms` | `12.93` | `0.50` | rejected; request improved but raw/post-wait/surface worsened |
| `terraincritical_reuse_repair_only_repeat_walk120_20260606` | `37.12 ms` | `10.38` | `0.40` | mixed but not accepted over the prior `34.51 ms` best |
| `terraincritical_reuse_repair_only_pressure_guard_walk120_20260606` | `61.03 ms` | `11.62` | `0.00` | rejected; pressure guard plus reuse shifted work to surface/post-wait |
| `terraincritical_reuse_repair_only_accounting_walk120_20260606` | `66.61 ms` | `13.60` | `0.63` | rejected; accounting fix did not solve debt shift |
| `terraincritical_reuse_gated_baseline_repeat_walk120_20260606` | `38.01 ms` | `19.51` | `10.65` | default-off sanity check, close to prior accepted class but not a new best |
| `terraincritical_gated_baseline_pressure_guard_walk120_20260606` | `39.84 ms` | `18.07` | `10.46` | rejected; pressure trim guard alone not enough |

Decision:

- Keep the telemetry and default-off switches.
- Do not promote terrain-critical frame-state reuse or repair-only reuse into the default playable stack.
- The useful architectural proof is that the cached critical footprint exists and is mostly valid (`reuseCached=946`, often only `reuseBad=1`), but repairing it naively shifts work into generation/surface/post-wait.
- Current best rough FPS remains the prior `34.51 ms` f120 point, about `29 FPS`, not 60.

Next work:

- Do not broaden repair-only reuse as a scalar.
- Add ownership/downstream accounting around cached-footprint repairs: only reuse if the repair set can be drained without increasing protected generation/surface/publish debt in the same frame.
- Hidden-exact remains a consistent request cost (`~4 ms`) and is likely the next cleaner target after terrain-critical repair accounting.

## Hidden-Exact Startup Deferral - 2026-06-06

Changed the default for `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP` from `1` to `0`.

Reason:

- At walk f120, public startup was still held by mid/shader/terrain-proof conditions, not hidden-exact:
  - `hiddenExactBlocked=0`
  - `hiddenExactRepairBlocked=0`
  - `hiddenExactStartup=0`
  - `hiddenExactConverged=1`
- The old default still spent about `4.10 ms` in hidden-exact startup warmup each frame:
  - `PERF_SPARSE_HIDDEN_EXACT_MISS frame=120 ms=4.06`
  - `waterProbeRays/candidates=9417/1518`
  - `feedback=0 accepted=0`
- Deferring hidden-exact until public open keeps the old behavior available by setting `VENPOD_SPARSE_HIDDEN_EXACT_MISS_DURING_STARTUP=1`, while avoiding speculative startup work that is not part of the current public-frame blocker set.

Validation:

- `.\build.ps1 -Config Release`: passed, with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted artifacts:

| Artifact | Frame | Raw | Request | Hidden exact | Correctness |
|---|---:|---:|---:|---:|---|
| `hidden_exact_startup_default_off_walk120_20260606` | 120 | `29.88 ms` | `15.44 ms` | `0.00 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |
| `hidden_exact_startup_default_off_walk240_20260606` | 240 | `26.76 ms` | `12.25 ms` | `0.00 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |

Current rough FPS:

- Best current validated walk f120 point is now `1000 / 29.88 = 33.5 FPS`.
- Walk f240 checked at `26.76 ms`, about `37.4 FPS`.
- Still not 60 FPS, but the trajectory remains good: the last two accepted slices removed speculative startup work while preserving visible-critical and ownership/public-frame checks.

Rejected probe:

| Artifact | Result | Decision |
|---|---:|---|
| `hidden_exact_water_budget_walk120_20260606` | `126.66 ms` raw, hidden-exact `6.76 ms`, tracked water coords `340` | rejected; budgeting the water scan changed startup history and shifted debt into generation/pressure/surface |

Next likely wall:

- Request is still dominated by terrain-critical scan/accounting:
  - f120: `terrainCriticalMs=10.90` inside `reqMs=15.44`
  - f240: `terrainCriticalMs=10.41` inside `reqMs=12.25`
- Continue with ownership-safe terrain-critical reuse/accounting, not hidden-exact startup work.

## Terrain-Critical Distance Default 1024 - 2026-06-06

Changed the default `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE` fallback from `sparseTerrainSurfacePrefetchDistance` (`3072` in the current stack) to `min(1024, sparseTerrainSurfacePrefetchDistance)`.

Reason:

- Current public exact foreground ownership is `exactNear=1024` and `surfaceRasterMax=1024`.
- The accepted visible-critical mid path already proves the non-exact handoff:
  - `coverageVisibleCritical=100`
  - `missingVisibleCritical=0`
  - ownership `missScreenPct=0`, `unsafeNearMissPct=0`
- Tracing terrain-critical exact rays to `3072` was spending roughly `10.4-10.9 ms` proving pages outside the current exact foreground ownership range.
- The env override still exists: set `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE=3072` for wider exact-proof experiments.

Validation:

- `.\build.ps1 -Config Release`: passed, with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted default artifacts:

| Artifact | Frame | Raw | Request | Terrain critical | Correctness |
|---|---:|---:|---:|---:|---|
| `terraincritical_distance_default1024_walk120_20260606` | 120 | `22.51 ms` | `8.81 ms` | `4.67 ms` | distance `1024`, visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |
| `terraincritical_distance_default1024_walk240_20260606` | 240 | `20.59 ms` | `6.54 ms` | `4.83 ms` | distance `1024`, visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |

Additional direct-log check:

- `terraincritical_distance_default1024_walk420_20260606` exited cleanly at frame `428`, but the summary script failed because there was no `PERF frame=420` line.
- Direct log evidence at frame `420` still showed:
  - `PERF_CAMERA_EXPOSURE frame=420 ... hiddenExactMissing=0/0 ... midCov=1.00/0.61`
  - visible-critical prepump frame `420`: `missingVisibleCritical=0`, `coverageVisibleCritical=100`, `missScreenPct=0`, `unsafeNearMissPct=0`
  - ownership stage frame `420`: `missScreenPct=0`, `unsafeNearMissPct=0`
  - clean shutdown at frame `428`

Rejected / not promoted:

| Artifact | Result | Decision |
|---|---:|---|
| `terraincritical_reuse_frame_state_hiddenoff_walk120_20260606` | `30.57 ms` raw, reuse rejected by `0x800` (`reuseBad=1`) | keep default-off; cached footprint still has a bad coord at f120 |
| `terraincritical_reuse_frame_state_hiddenoff_walk240_20260606` | `35.85 ms` raw, request `2.92 ms`, reuse `1` | not accepted; request win shifted to raw gap/post-wait |
| `terraincritical_reuse_frame_state_hiddenoff_repeat_walk240_20260606` | `28.84 ms` raw, request `2.68 ms`, reuse `1` | mixed; body improved but raw still behind distance clamp |

Current rough FPS:

- Best validated walk f120 point is now `1000 / 22.51 = 44.4 FPS`.
- Walk f240 is `20.59 ms`, about `48.6 FPS`.
- Direct f360 from the f420 log was `20.36 ms`, about `49.1 FPS`.
- Still not 60 FPS, but this is a major improvement over the previous `29.88 ms` f120 point and keeps public-frame correctness checks clean.

Next likely wall:

- Remaining f120 request cost is `8.81 ms`, with terrain-critical still `4.67 ms`, pressure trim `2.23 ms`, stats flush `0.68 ms`, and hidden-exact `0.00 ms`.
- Next architectural target should be a safer terrain-critical proof replacement or budgeted terrain height sampling; frame-state reuse is useful evidence but not yet acceptable as a default due raw pacing.

## Terrain-Critical Grid Default 13x9 - 2026-06-06

Changed the default screen-critical terrain proof grid from the old maximum `21x13` to `13x9`.

Rationale:

- The 1024-distance clamp removed out-of-contract exact proof, but terrain-critical still spent `~4.7-4.8 ms`.
- The `13x9` grid keeps the public exact proof clean for the accepted startup-held walk frames while cutting request cost materially.
- Env overrides remain available:
  - `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RAYS_X`
  - `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RAYS_Y`

Validation:

- `cmake --build build --config Release` through the VS dev environment: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Accepted actual-default stack probes:

| Artifact | Frame | Raw | Rough FPS | Request | Terrain critical | Correctness |
|---|---:|---:|---:|---:|---:|---|
| `terraincritical_grid_default13x9_walk120_20260606` | 120 | `16.48 ms` | `60.7` | `5.71 ms` | `2.22 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |
| `terraincritical_grid_default13x9_walk240_20260606` | 240 | `17.15 ms` | `58.3` | `3.62 ms` | `2.03 ms` | visible-critical `100`, miss/unsafe `0/0`, terrain post nonready `0` |

Rejected pressure-trim probes:

| Artifact | Result | Decision |
|---|---:|---|
| `pressure_incremental_default1024_walk120_20260606` | `20.30 ms` raw, pressure trim `1.84 ms` | rejected as broad default; f120 improved over distance clamp but not enough |
| `pressure_incremental_default1024_walk240_20260606` | `26.81 ms` raw, pressure trim `0.00 ms` | rejected; later frame regressed badly |
| `pressure_incremental8192_default1024_walk120_20260606` | `19.24 ms` raw, pressure trim `0.27 ms` | rejected as broad default; local f120 improvement only |
| `pressure_incremental8192_default1024_walk240_20260606` | `24.04 ms` raw, window avg/max `27.39/30.73` | rejected; shifted debt into later generation/surface work |

Rejected grid probe:

| Artifact | Result | Decision |
|---|---:|---|
| `terraincritical_grid_default11x7_stack_walk120_20260606` | `17.69 ms` raw, clean correctness | rejected; misses 60 FPS at f120 |
| `terraincritical_grid_default11x7_stack_repeat_walk120_20260606` | `17.73 ms` raw, clean correctness | rejected; repeat confirmed f120 regression |
| `terraincritical_grid_default11x7_stack_walk240_20260606` | `15.49 ms` raw, clean correctness | not promoted; f240 win does not offset f120 regression |

Current rough FPS:

- Current best validated startup-held walk f120 point is `16.48 ms`, about `60.7 FPS`.
- Current f240 point is `17.15 ms`, about `58.3 FPS`.
- This is a real improvement from the earlier `34.51 ms`, `29.88 ms`, and `22.51 ms` milestones, but it is not yet a complete 60 FPS renderer.

Important caveat:

- These are startup-held public-frame rows (`mode=rebrun`) under the accepted ownership gate. They prove the public frame can be made clean and near-60, but they do not yet prove unheld traversal or broader visual stability.
- Bare walk probes without the accepted stack are not comparable and still show large debt (`terraincritical_grid_default11x7_walk120_20260606` at `89.80 ms`, `terraincritical_grid_default11x7_walk240_20260606` at `85.68 ms`).

Next likely wall:

- f120 still has request `5.71 ms`, generation `5.92 ms`, surface extract `2.86 ms`, and pressure trim `2.16 ms`.
- f240 still has request `3.62 ms`, generation `4.17 ms`, and surface extract `3.09 ms`.
- Next work should move from startup-held proof optimization to stable open/play traversal: track why public render remains held, then attack post-open motion debt without weakening `missScreenPct=0`, `unsafeNearMissPct=0`, and visible-critical coverage.

## Startup Mid Visible-Proof Gate Probe - 2026-06-06

Added default-off startup gate instrumentation/experimental path:

- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_VISIBLE_PROOF=0` by default.
- When enabled, startup mid-voxel proof can use the previous frame's projected visible-critical mid proof instead of broad cache coverage.
- Held-frame telemetry now logs:
  - `midVoxelVisibleProof=valid/coverage/missing/cache:{}/{}/{}/{}`

Why it exists:

- At the current accepted held frames, broad mid coverage is only `61/46`, so `midVoxelBlocked=1`.
- The actual projected visible-critical proof is already clean:
  - `missingVisibleCritical=0`
  - `coverageVisibleCritical=100`
  - `missScreenPct=0`
  - `unsafeNearMissPct=0`
- This showed the startup gate was waiting for cache-completeness, not for screen-visible ownership correctness.

Validation:

- `cmake --build build --config Release` through the VS dev environment: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Default-off verification:

| Artifact | Frame | Raw | Gate | Correctness |
|---|---:|---:|---|---|
| `startup_mid_visible_proof_defaultoff_walk360_20260606` | 360 | `16.36 ms` | still held by `midVoxelBlocked=1`, visible proof disabled | visible-critical `100`, miss/unsafe `0/0` |

Experimental open probe:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_mid_visible_proof_default_walk360_20260606` | gate opened around frame `101`; frame 360 moved camera to `(8.73,-41.00,470.93)` and regressed to `47.50 ms` raw, with clip `37.31 ms`, GPU `9.90 ms`, visible-critical `94` | rejected as default; useful because it exposes post-open motion debt |
| `startup_mid_visible_proof_parallel_mid_walk360_20260606` | `55.28 ms` raw; parallel pump active `24` bricks/`4` workers but request and raw worsened | rejected |
| `startup_mid_visible_proof_pumpcap8_walk360_20260606` | `52.73 ms` raw; pump capped to `8`, but visible-critical collapsed to `55` | rejected; cap starves visible ownership |

Current state:

- Accepted default remains the conservative startup-held near-60 path.
- Experimental visible-proof release is not promoted because post-open traversal is not stable.
- The next real architecture target is not another startup gate scalar. It is post-open mid-voxel scheduling:
  - preserve projected visible-critical coverage while moving cache/noncritical mid generation off the frame,
  - or amortize/reuse moving clipmap interest so `clipMs` does not jump to `30-40 ms`,
  - then re-enable the visible-proof gate only after the moving path holds visible-critical coverage and frame time together.

## Split Visible Mid-Clipmap Pump Probe - 2026-06-06

Added a default-off experimental split pump:

- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP=0`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET`, default `8`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET`, default `0`

Implementation:

- `SparseClipmapTileCache::PumpGenerationSplitVisiblePriority(...)` pumps height normally, then pumps queued visible-priority voxel coords under a separate cap.
- The cache/backfill pass skips any still-visible-priority coords, so cache budget cannot steal the visible lane.
- Split cache deferral now feeds the existing terrain prefetch throttle so intentional cache debt does not trigger extra terrain surface prefetch work.
- The path is env-gated and not default.

Validation:

- `cmake --build build --config Release` through VS dev env: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_fixeddt16_walk360_20260606` | fixed walk dt did not help: f360 `61.46 ms`, pump `37.59 ms`, visible-critical `95` | rejected; movement dt is not the root cause |
| `startup_visible_proof_splitvis8_cache0_walk360_20260606` | f360 pump bounded to `8.29 ms`, but raw `57.50 ms`, request `29.94 ms`, visible-critical `51` | rejected; budget 8 starves visible catch-up |
| `startup_visible_proof_splitvis16_cache0_walk360_20260606` | f360 `38.75 ms`, mid pump `0.00 ms`, visible-critical `100`, cache coverage `61`, but shader unsafe exact nonready `140` and GPU ray `18.68 ms` | useful direction, not acceptable/default-ready |
| `startup_visible_proof_splitvis16_exactrepair_walk360_20260606` | f360 `80.86 ms`; exact repair generated/uploaded `48` per frame and worsened gen/surface cost while unsafe samples remained | rejected |

Current interpretation:

- Split visible pumping is a better architecture than global pump caps: it can keep projected visible-critical mid coverage at `100` without synchronous mid pump debt.
- It does not solve playable unheld traversal yet. Once mid coverage is stable, the next blocker shifts to exact/surface/GPU work after open: unsafe exact non-ready samples, surface extraction spikes, and GPU ray time.
- Do not promote visible-proof startup release until a post-open exact/surface readiness lane is bounded and public-frame clean.

Follow-up open-path probes:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_cache0_walk420_20260606` | gate opened around frame `409`; f420 moved camera, shader unsafe clean, but visible-critical fell to `94`, post-open hidden surface catchup extracted `144`, f424 raw `55.83 ms`, clip `31.32 ms`, pump `14.41 ms` | rejected as default; proves first moving frames are the real wall |
| `startup_visible_proof_splitvis24_postopen48_walk420_20260606` | paced post-open exact budgets reduced hidden surface spike (`33` extracted at f424), but visible-critical stayed `93` and f424 raw was `79.24 ms`, pump `22.53 ms` | rejected; more visible budget costs too much synchronously |
| `startup_visible_proof_splitvis24_directfootprint_postopen48_walk420_20260606` | direct footprint reduced some surface/exact debt but f420 raw `64.68 ms`, f424 raw `71.93 ms`, visible-critical `94` | rejected |
| `startup_forwardprefetch384_splitvis16_postopen48_walk420_20260606` | temporary startup forward-prefetch experiment opened at frame `403` but flooded held startup frames (`~70-80 ms`) and did not improve moved visible coverage (`94`) | rejected and removed from code |
| `startup_visible_proof_splitvis16_sharedcol_postopen48_walk420_20260606` | shared column cache caused expensive held startup frames and still left moved visible coverage around `94` | rejected |
| `startup_visible_proof_splitvis16_postopen48_walk420_20260606` | paced exact catchup alone left f420/f424 visible-critical around `94`, with large tracked hidden-exact backlog | rejected as sufficient fix |

Updated interpretation:

- The gate can open after shader-unsafe repair converges, but the first public movement immediately creates roughly `300` projected visible mid-voxel misses.
- Synchronously pumping `16-24` visible mid bricks per frame is too expensive (`~14-23 ms`) and still too slow to restore `>=95-100` visible coverage during movement.
- Post-open exact/surface budgets should remain bounded, but they are not the main blocker once mid movement starts.
- Next architecture should replace synchronous mid visible pumping with an actual background/async mid-voxel generation lane or a stronger precomputed moving-window cache. More scalar budgets, direct footprint, shared column cache, and startup forward velocity did not solve it.

## Parallel Split-Visible Mid Pump Probe - 2026-06-06

Added default-off worker-backed split-visible mid-voxel generation:

- Existing `-ParallelMidVoxelPump` / `-ParallelMidVoxelPumpPersistentWorkers` now apply inside `PumpGenerationSplitVisiblePriority(...)` when safe.
- The parallel path only runs for voxel queues, not the shared column-cache mode or edited overlay cases.
- Slot allocation remains on the main thread; brick voxel fill is parallelized with the existing persistent/temporary worker path.
- Telemetry reports `parallelPump=active/bricks/workers/wallMs` for the split lane.

Validation:

- `cmake --build build --config Release` through the VS dev environment: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_parallel_postopen48_walk420_20260606` | f420 raw `82.58 ms`, CPU `40.73`, request `12.78`, generation `13.68`, clip `14.26`, pump `9.85`, surface `17.03`, GPU `17.93`, visible-critical `99`, miss/unsafe `0/0`; f424 raw `70.88 ms`, visible-critical `97`, parent-held failure, parallel pump `16` bricks/`4` workers/`8.71 ms` | rejected as default; useful architecture probe |

Interpretation:

- Parallel split-visible pumping reduced the visible pump wall in the moving frame compared with the synchronous split path (`~8.7 ms` for `16` bricks at f424 versus the earlier `~14.4 ms` sync row).
- It did not make the open path viable. Frame time moved into request/generation/surface/GPU/untracked work, and visible-critical coverage still fell after movement.
- Keep this default-off as a stepping stone, not a promoted setting. The next useful move is a true background/async mid-voxel ownership lane whose results are ready before the public frame needs them, not more synchronous workers attached to the same frame.

## Async Noncritical Mid-Voxel Staging Probe - 2026-06-06

Added a default-off async cache/noncritical mid-voxel staging lane:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_GEN=0` remains the default.
- New caps:
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_QUEUE_MAX`, default `256`
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_ENQUEUE`, default `16`
  - `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_NONCRITICAL_MAX_APPLY`, default `16`
- The worker generates procedural mid-voxel payloads into a staging result queue.
- Initial completion publish ran after interest update and edit invalidation, before visible-critical prepump coverage was computed; the follow-up publish-barrier pass below moved apply behind the coverage/ownership checks.
- Async queueing is disabled while edits are active; stale edit-revision results are discarded.
- Pending async cache coords remain eligible for synchronous visible generation if they later become screen-critical.
- Async publish is guarded so noncritical completions do not evict resident clipmap slots when no free slot exists.
- Backlog telemetry now reports mid async worker time, queue depth, result depth, pending count, and enqueue/complete/apply/drop/duplicate counters.

Validation:

- `cmake --build build --config Release` through the VS dev environment: passed with existing `rayDir` warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncnoncrit64_postopen48_walk420_20260606` | async lane active, but f420 raw `64.59 ms`, visible-critical `88`, backlog line `async=enq/complete/apply/drop/dup:5/2/2/0/0`; earlier frames applied large async batches and shifted cost into clip/surface/GPU | rejected |
| `startup_visible_proof_splitvis16_asyncnoncrit64_noevict_postopen48_walk420_20260606` | no-evict publish guard added; f420 raw worsened to `133.33 ms`, visible-critical `87`, main-thread pump `22.83`, clip `46.32`; async applied `46` at f420 | rejected |
| `startup_visible_proof_splitvis16_asyncnoncrit8_noevict_postopen48_walk420_20260606` | paced async to `8` enqueue/apply; f408 visible-critical `99`, but f420 raw `113.43 ms`, visible-critical `96`, parent-held failure, request/gen/clip `20.17/17.88/49.61`; f424 raw `103.10`, visible-critical `97` | rejected as default |

Current interpretation:

- This is the right architectural family but not the finished lane. It proves off-frame mid-voxel generation and pre-prepump publish are wired, and it preserves public-frame correctness checks.
- Current publish still lands too much work on the public frame: GPU upload/dirty snapshot, clip publish, and main-thread bookkeeping dominate once async completions become resident.
- The next step should be a two-stage background lane: worker generation first, then a separately budgeted publish/upload barrier that is coverage-aware and does not publish cache work during visible-critical catch-up frames.
- Do not promote async noncritical generation. Keep it default-off as a measured scaffold for the next publish-barrier pass.

## Async Noncritical Publish Barrier Probe - 2026-06-06

Changed the default-off async noncritical mid-voxel apply point:

- `ApplyAsyncNoncriticalVoxelGenerationCompletions(...)` no longer runs immediately after interest update/edit invalidation.
- Async cache/noncritical completions are applied only when:
  - async noncritical generation is enabled,
  - visible-critical prepump is active,
  - `sparseMidClipmapPrepumpMissingVisibleCritical == 0`,
  - there is no `sparseMidClipmapVisualOwnershipFailure`,
  - previous retire ownership miss and unsafe-near-miss percentages are both `0`.
- Otherwise the worker may continue generating staged payloads, but publish/apply is capped to `0`.

Validation:

- `cmake --build build --config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncnoncrit8_publishbarrier_postopen48_walk420_20260606` | f420 raw `84.36 ms`, CPU `67.62`, request/gen/clip/pump `18.03/16.08/33.49/28.49`, GPU ray `16.66`, visible-critical `95`, missing visible/cache `268/3113`; f420 async result/pending `30/31`, apply `0`; f424 raw `107.25 ms`, visible-critical `94` | rejected as default |

Interpretation:

- The barrier worked mechanically: async cache publish stayed at `0` while visible-critical misses existed, so it did not inject cache residency/publish debt into the unsafe public frame.
- It did not solve motion. Cache debt grew to roughly `3100`, visible-critical still fell below target after movement, and raw frames stayed far from 60 FPS.
- The next viable async slice must separate visible-critical async completions from cache/noncritical completions. Visible-critical staged payloads need a safe high-priority publish path during catch-up, while cache completions remain deferred behind the coverage/ownership barrier.

## Async Visible-Critical Mid-Voxel Staging Probe - 2026-06-06

Implemented a default-off visible-critical companion path on the same async mid-voxel worker:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN=0`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY`
- async requests/results carry a `visibleCritical` tag;
- apply scans the shared result queue by class, so cache/noncritical results can remain deferred while visible-critical results behind them are still publishable;
- visible-critical async results are applied only if the coord is still in the visible-priority set;
- visible-critical async is disabled before startup public render opens, so hidden startup prewarm does not double-fill every frame;
- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` logs `asyncVisible=enq/complete/apply/drop/dup`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_splitvis16_asyncvisible16_postopen48_walk420_20260606` | ungated first run incorrectly queued visible async during hidden startup; f420 raw `91.33 ms`, visible-critical `98`, `asyncVisible` applied `16`; rejected and fixed by post-open gating |
| `startup_visible_proof_splitvis16_asyncvisible8_postopen_gated_postopen48_walk420_20260606` | post-open gated, but early return prevented async queueing when sync visible budget was `0`; direct f420 `65.45 ms`, visible-critical `95`; rejected as inconclusive |
| `startup_visible_proof_splitvis16_asyncvisible8_postopen_gated_earlyfix_postopen48_walk420_20260606` | early-return fixed; harness still missed `PERF frame=420`, direct `PERF_FRAME_END` f420 raw `71.77 ms`, body `61.71`, visible-critical `95`, parent-held failure `1`, f424 visible-critical `95` | rejected as default |

Interpretation:

- The visible-critical async scaffold is useful and should stay default-off: it adds the missing priority split that the cache publish barrier exposed.
- It is not a playable candidate. The first moved frames still lose visible-critical coverage (`~95`) and carry large cache/mid debt (`~3500`) plus exact/surface post-open work.
- The next branch should not be another cap. The engine needs predictive post-open visible work before the gate opens or a moving-window ownership reservation that keeps the next camera footprint ready without adding public-frame dirty/upload/surface bursts.

## Startup Predictive Visible Mid-Voxel Proof Probe - 2026-06-06

Added a default-off startup predictive visible proof slice:

- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_PROOF=0`
- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_DISTANCE`, default `384`
- `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_PREDICTIVE_VISIBLE_MAX_COORDS`, default `384`

Implementation:

- While the startup public render gate is held and visible mid-voxel proof is active, the prepump projection can now append a second predicted visible footprint.
- The predicted footprint uses flattened camera forward plus current clipmap velocity prediction, dedupes against current visible coords, and feeds the same visible-priority proof/pump path.
- Prepump telemetry now logs `predictiveVisible`.
- The feature remains default-off and is not part of the accepted stack.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_predictive384_splitvis16_asyncvisible8_postopen48_walk420_20260606` | opened at frame `391`; f420 raw `119.12 ms`, body `112.03`, CPU update `86.09`, request/gen/clip/pump `21.04/19.79/45.23/39.05`, GPU frame/ray `24.78/20.30`, visible-critical `95`, missing visible/cache `247/3416`, backlog voxel `3663`, `asyncVisible=8/8/8/0/0`; f424 raw `128.03 ms`, visible-critical `94`; post-open prepump reported `predictiveVisible=0` | rejected as default |

Interpretation:

- Naive single predicted-view projection is the wrong shape. It opened earlier than the prior visible-async rows and made the moved public frames worse.
- `predictiveVisible=0` after open shows the pre-open predictive addition did not keep the actual moved footprint ready.
- Do not tune this by scalar distance/cap as the next move. The useful next architecture is a real moving-window readiness contract: current visible proof plus multiple future camera samples/trajectory footprints resident and published before release, with downstream dirty/upload/surface debt bounded before public motion is allowed.

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

Implementation:

- The visible-critical projection helper now accepts an explicit view position and basis, so future samples can use their own yaw/pitch.
- Startup moving-window proof blocks the visible-proof gate until all sampled future footprints are missing-free.
- In the walk harness, moving-window projection can follow the scripted walk yaw/position instead of sampling a fake held-camera forward ray.
- Optional post-open moving-window priority keeps tagging future-footprint coords after release.
- Telemetry:
  - `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP ... movingWindow=samples/ready/missing/coords/coverage`
  - held gate log now includes `midVoxelMovingWindowProof`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_splitvis16_asyncvisible8_postopen48_walk420_20260606` | first straight-forward window version opened at frame `403`; f420 raw `114.07 ms`, visible-critical `95`; pre-open window incorrectly reported `3/3` ready because it sampled the held camera, not scripted walk | rejected; diagnostic flaw fixed |
| `startup_visible_proof_movingwindow3x128_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk420_20260606` | scripted-walk window detected real future misses: f396 `movingWindow=3/0/299/70/95`, f400 `3/0/211/45/96`; f420 held and clean `3/3/0/0/100`, frame-end raw `70.19 ms` while still hidden | useful proof; not an open result |
| `startup_visible_proof_movingwindow3x128_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | opened at frame `458`; f460 visible-critical `100`, but f480 regressed to raw `95.37 ms`, visible-critical `97`, missing visible/cache `159/2633`, request/gen/clip/pump `18.15/16.87/43.88/22.47`, GPU ray `15.81` | rejected as default; one-shot window does not sustain motion |
| `startup_visible_proof_movingwindow3x128_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | post-open future priority opened earlier at frame `445` but worsened f480 to raw `115.04 ms`, visible-critical `95`, movingWindow `3/0/572/82/91`, main-thread brick gen `43.55`, GPU ray `23.56` | rejected |

Interpretation:

- The corrected moving-window proof is architecturally useful because it finds future walk misses before release and delays opening until the sampled trajectory is clean.
- It is not enough for a playable path. A one-shot pre-open reservation is consumed after release, and continuous frame-local future priority overloads the same public-frame pump.
- Next step should be a paced moving-window reservation queue: keep a rolling future footprint N frames ahead, generate/publish/upload its visible-critical bricks before they become current, and only allow the public frame to consume already-resident reservations. Do not route future reservations through the same immediate visible pump as current misses.

## Async Moving-Window Reservation Lane Probe - 2026-06-06

Added a default-off async reservation class separate from current visible priority:

- `SparseClipmapTileCache::QueueAsyncVisibleReservationVoxelCoords(...)`
- `m_asyncVisibleReservationVoxelSet`
- startup env:
  - `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MID_VOXEL_MOVING_WINDOW_ASYNC_RESERVATION=0`
- post-open env:
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_ASYNC_RESERVATION=0`

What changed:

- Future moving-window coords can now be queued as visible-critical async reservations without being inserted into `m_visiblePriorityVoxelSet`.
- Async visible results remain valid if the coord is still either current visible-priority or async-reserved.
- Startup reservation completions may apply while the public gate is still held.
- `PERF_SPARSE_MID_CLIPMAP_VISIBLE_CRITICAL_PREPUMP` now logs `movingWindow=samples/ready/missing/coords/coverage/reserve/queued`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only existing LF-to-CRLF warnings.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_asyncreservation_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | startup-only reservation opened early at frame `412`; f480 raw `100.20 ms`, visible-critical `95`, missing visible/cache `242/2598`, request/gen/clip/pump `18.47/22.69/42.15/37.30`, GPU ray `28.75`; f480 moving window inactive post-open | rejected as default |
| `startup_visible_proof_movingwindow3x128_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | continuous reservation stayed active after open; f420 `reserve/queued=86/16`, f480 `reserve/queued=46/16`, `asyncVisible=8/8/8`; f480 raw `118.92 ms`, visible-critical `95`, missing visible/cache `302/2342`, request/gen/clip/pump `16.87/15.98/47.24/21.20` | rejected |

Interpretation:

- The separate reservation set is mechanically useful: future-window coords no longer have to become current visible-priority sync-pump work.
- The current per-frame async worker/apply shape cannot sustain motion. With enqueue/apply effectively capped at `8`, the reservation lane remains behind the future footprint and public frames still hit `250-300+` visible misses.
- Next useful architecture is a persistent reservation scheduler with deadlines and queue depth, not another frame-local projection:
  - keep reservations alive across frames instead of clearing/replacing them every prepump,
  - order by earliest future sample/deadline,
  - allow higher hidden/pre-open drain while public render is held,
  - publish/upload reservations before they become current visible misses,
  - keep cache/noncritical publication behind the visible ownership barrier.

## Persistent Deadline-Ordered Async Reservation Scheduler - 2026-06-06

Implemented a default-off persistent reservation scheduler for moving-window mid-voxel async visible work.

What changed:

- Replaced the frame-local reservation membership set with `m_asyncVisibleReservations`, a map keyed by `SparseVoxelClipmapCoord`.
- Each reservation tracks `firstFrame`, `lastSeenFrame`, `deadlineFrame`, and `sampleIndex`.
- `QueueAsyncVisibleReservationVoxelCoords(...)` now keeps reservations alive across frames, prunes resident/non-interested/non-queued/stale coords, sorts candidates by earliest deadline/sample/first frame, then queues visible-critical async work in that order.
- `SetVisiblePriorityVoxelCoords(...)` no longer clears future reservations.
- Async visible results remain valid when their coord is still current visible-priority or present in the reservation map.
- Launcher moving-window projection now buckets reservation coords per future sample and passes a deadline derived from travel time.
- New default-off/runtime-tuning env:
  - `VENPOD_SPARSE_MID_CLIPMAP_MOVING_WINDOW_RESERVATION_STALE_FRAMES`, default `24`; test used `36`.

Validation:

- `.\build.ps1 -Config Release`: passed with existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check`: only LF-to-CRLF warnings.
- Crash audit: a redundant reservation set version plus an experimental async-dominant sync throttle produced `0xC0000409` fail-fast dumps. The throttle and explicit current-visible async queue were removed. The final kept implementation uses the reservation map as the single source of truth and reran successfully.

Evidence:

| Artifact | Result | Decision |
|---|---:|---|
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible8_postopen48_walk480_20260606` | f480 raw `156.96 ms`, visible-critical `93`, missing visible/cache `419/2290`, `asyncVisible=8/7/7`, movingWindow `3/0/722/24/89/24/8` | rejected |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_continuous_scriptedwalk_splitvis16_asyncvisible16_postopen48_walk480_20260606` | earlier run: opened `403`; f480 raw `108.49 ms`, visible-critical `99`, missing visible/cache `17/2333`, `asyncVisible=16/16/16`, movingWindow `3/0/172/46/97/46/16`; f484 visible-critical `100` | useful direction, not 60 FPS |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_maponly_continuous_scriptedwalk_splitvis16_asyncvisible16_postopen48_walk480_20260606` | map-only final validation: opened `408`; f480 raw `144.89 ms`, visible-critical `98`, missing visible/cache `85/2244`, `asyncVisible=16/16/16`, movingWindow `3/0/192/5/97/5/1`; f484 visible-critical `99`, missing visible `32` | stable but rejected as default |
| `startup_visible_proof_movingwindow3x128_persistent_asyncreservation_asyncdominant0_*` | attempted to throttle sync visible pump when coverage was high; reduced one stale run to `76.60 ms` but starved generation, and later fail-fast crashed when explicit current-visible async queueing was added | removed |

Interpretation:

- The trajectory is architecturally correct but not yet a 60 FPS solution. Persistent reservations and cap-16 async visible work can keep public visible coverage near `98-100%`, which is much better than cap-8 and frame-local priority.
- The frame-time problem is now clearly elsewhere too: f480 still spends tens of ms in main-thread mid generation, surface extraction/staging, GPU ray, and frame gaps/post-wait. A single async worker lane plus synchronous visible pump cannot hit 16.7 ms.
- Next work should not be another scalar moving-window knob. The next architectural cut is a safe split between async enqueue and sync generation:
  - current visible-priority async enqueue must be callable without running the sync pump,
  - sync visible generation must become an emergency-only budget,
  - moving-window reservations need multi-worker or batched async generation,
  - post-open surface/publish catchup must be separately budgeted so visible coverage wins do not still cost 100+ ms.

## Repair Lane Classification And Bounded Admission - 2026-06-06

Retained architecture slice:

- Added `Simulation::SparseStreamingLane::Repair` between `Prefetch` and `Visible`.
- Carried repair lane counts through request/generation/generated/upload/surface, async exact generation, deferred downstream, surface-ready publish, page-publish, and ownership-stage-budget telemetry.
- `ClassifyStreamingTicketOwnership(Repair)` maps to `HiddenRepair`.
- `HiddenRepair` is intentionally noncritical for downstream stage budgeting; public-critical and visible correctness remain separate.
- `perf_noncapture_smoke.ps1` now parses five-lane logs:
  `cache/prefetch/repair/visible/public`.

Important rejected setting:

- Unbounded post-open hidden-exact repair lane was rejected:
  `build/captures/repairlane_enabled_pppublic480_20260606`
- f480 raw/body `128.03/125.75 ms`, local window avg/max raw `123.65/150.32 ms`.
- It created huge visible-class downstream queues even though the lane was noncritical:
  queued upload about `7960`, queued surface about `3371`.
- Conclusion: lane identity alone is not enough. Hidden repair admission must be bounded before downstream queues.

Retained bounded repair admission:

- Added post-open repair-lane request caps:
  - `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE_MAX_REQUESTS`, default `16`.
  - `VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_WATER_REPAIR_LANE_MAX_REQUESTS`, default `min(8, repairMax)`.
- Added `repairLimitSkips`, `repairMax`, and `repairWaterMax` to
  `PERF_SPARSE_HIDDEN_EXACT_REPAIR_LANE` and the smoke parser.
- The cap is applied only after the hidden-exact coord is still not render-ready, so ready/active no-ops do not consume it.

Validation:

- `.\build.ps1 -Config Release`: passed, only existing `rayDir` shadow warnings.
- `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- `git diff --check` for touched files: clean except existing LF-to-CRLF warnings.
- Capped repair artifact:
  `build/captures/repairlane_cap16_pppublic480_20260606`

Capped repair result:

- f480 raw/body: `80.91/73.92 ms`.
- local f456..484 avg/max raw: `78.86/90.74 ms`, avg/max body `78.13/91.86 ms`.
- f480 request/gen/clip/pump/surfaceExtract/gpuRay:
  `12.95/13.75/39.21/16.94/10.63/5.80 ms`.
- f480 repair evidence:
  `accepted=8`, `repairAccepted=8`, `repairLimitSkips=71`, `repairMax=16`, `repairWaterMax=8`.
- f480 downstream queues:
  queued upload `388` total with `347` prefetch and `41` repair;
  queued surface `10` total, all repair.
- ownership remained public-valid in the sampled frame:
  `PERF_BACKEND_PIPE active=0x7FF`, `missScreenPct=0`, `unsafeNearMissPct=0`.

Decision:

- Keep repair lane classification and parser support.
- Keep bounded repair admission defaulted behind the repair-lane flag.
- Do not enable unbounded repair lane again.
- The capped repair lane is an improvement over the rejected unbounded setting, but it is not a 60 FPS candidate. Current comparable public walking remains about `80 ms`, roughly `12-13 FPS`.
- Next blocker is still the public-open f456..484 window: clip/pump, main-thread generation, surface/publish catchup, GPU ray/surface, and frame gap/post-wait. The next branch should reduce one of those measured costs while preserving lane ownership.

## Mid-Clipmap Interest Reuse Validation - 2026-06-06

Measured blocker after bounded repair:

- In `build/captures/repairlane_cap16_pppublic480_20260606`, f480 had
  `clipMs=39.21`, `pumpMs=16.94`, `mainThreadBrickGenMs=16.94`.
- `PERF_SPARSE_CPU_DETAIL frame=480` showed `centerDelta=0/0/0`,
  `fullRebuild=1`, and `clip=interest/reuse/pump:18.64/0/18.20`.
- That means the clipmap interest footprint was being rebuilt on a frame where the sparse center did not move, because the interest signature keys off fine-grained camera/forward/velocity state.

Accepted existing control:

- `perf_noncapture_smoke.ps1` already exposes the safe existing knob:
  `-MidInterestInterval 2`, which maps to
  `VENPOD_SPARSE_MID_INTEREST_INTERVAL=2`.
- Defaults remain unchanged.
- This is not a visual/ownership shortcut; it reuses the last mid-clipmap interest set for one frame and still keeps the public ownership gates active.

Validation artifact:

- `build/captures/mid_interest_interval2_repaircap_pppublic480_20260606`

Interval-2 result:

- f480 body `72.72 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `13.76/12.35/15.99/11.54/11.54/10.25/16.16 ms`.
- local f456..484 avg/max raw: `69.60/93.86 ms`.
- local f456..484 avg/max body: `76.06/98.89 ms`.
- f480 `clip=interest/reuse/pump:1.39/1/12.80`.
- f480 visible-critical coverage improved to `96`, cache coverage `75`,
  visible/cache misses `247/1957`.
- Public path remained valid:
  `PERF_BACKEND_PIPE active=0x7FF`, `miss=0`, `unsafeNearMiss=0`.

Rejected control:

- `build/captures/mid_interest_interval4_repaircap_pppublic480_20260606`
- f480 body `73.64 ms`, `clipMs=36.01`, coverage visible-critical `93`,
  visible/cache misses `427/1948`.
- window avg/max raw `72.61/91.18`, avg/max body `81.79/95.34`.
- It rebuilt on f480 and worsened body/window stability, so do not use interval 4 as the next candidate.

Decision:

- Add `-MidInterestInterval 2` to the strongest current public-open test stack.
- Do not promote the default yet; this is a validated default-off/harness setting.
- The rough current public walking state is still about `70-76 ms` in this window, roughly `13-14 FPS`, not close to 60 FPS.
- Next measured blockers after interval 2 are still distributed:
  GPU ray can spike (`16.16 ms` at f480), surface extraction is about `7-10 ms`,
  post-wait remains about `15 ms`, and sync mid pump still costs about `11-13 ms`
  on reused-interest frames.

## Footprint Interest Signature - 2026-06-06

Implemented a default-off footprint-style mid-clipmap interest signature:

- Config/env:
  - `SparseClipmapConfig::footprintInterestSignature`
  - `VENPOD_SPARSE_MID_CLIPMAP_FOOTPRINT_INTEREST_SIGNATURE=1`
  - `perf_noncapture_smoke.ps1 -MidClipmapFootprintInterestSignature`
- Defaults remain unchanged.
- Startup log now reports `interestInterval` and `footprintSignature`.
- In footprint mode, the interest signature:
  - quantizes camera position to a fraction of the smallest voxel brick footprint,
  - coarsens forward-vector buckets,
  - coarsens velocity buckets by motion-lookahead scale.

Rejected first attempt:

- `build/captures/mid_footprint_signature_repaircap_pppublic480_20260606`
- It kept velocity too fine, still rebuilt at f480, and regressed:
  f480 body `75.38 ms`, clip `39.33 ms`, window avg/max raw `73.63/99.24 ms`,
  avg/max body `84.50/94.28 ms`, visible-critical coverage `94`.

Accepted validation:

- Build/test:
  - `.\build.ps1 -Config Release`: passed, only existing `rayDir` shadow warnings.
  - `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Artifact:
  `build/captures/mid_footprint_signature_v2_repaircap_pppublic480_20260606`
- f480 body `66.79 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `13.93/12.02/20.57/15.61/15.61/7.82/5.54 ms`.
- f456..484 avg/max raw `66.07/83.52 ms`.
- f456..484 avg/max body `70.15/86.99 ms`.
- f480 `clip=interest/reuse/pump:1.52/1/16.92`.
- f480 visible/cache coverage `94/74`; visible/cache misses `366/1948`.
- Public path stayed valid:
  `active=0x7FF`, `miss=0`, `unsafeNearMiss=0`.

Mixed/rejected combo:

- `build/captures/mid_footprint_signature_interval2_repaircap_pppublic480_20260606`
- f480 body `66.98 ms`, clip `26.90 ms`, pump `21.63 ms`,
  visible/cache coverage `95/75`, visible/cache misses `304/1944`.
- window avg/max raw `70.68/79.61 ms`, avg/max body `69.73/86.29 ms`.
- It improves maximum frame time slightly but worsens average raw and target pump cost.
- Do not carry the combo as the next strongest stack without broader validation.

Decision:

- Carry `-MidClipmapFootprintInterestSignature` as the stronger current public-open test stack setting.
- Drop `-MidInterestInterval 2` from the strongest stack unless specifically testing cadence reuse.
- This moves public walking from roughly `70-76 ms` to roughly `66-70 ms`
  in the f456..484 window, about `14-15 FPS`, still far from 60 FPS.
- Remaining measured blockers: sync mid pump around `15-17 ms` on f480,
  surface extraction around `8 ms`, post-wait around `15 ms`, request/gen about
  `12-14 ms` each, plus occasional GPU ray spikes in other variants.

## Post-Open Async Visible Split Pump - 2026-06-06

Implemented and retained a default-off post-open-only split-visible pump gate:

- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_GEN`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_ENQUEUE`
- `VENPOD_SPARSE_MID_CLIPMAP_ASYNC_VISIBLE_CRITICAL_MAX_APPLY`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_POST_OPEN_ONLY`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_VISIBLE_PUMP_BUDGET`
- `VENPOD_SPARSE_MID_CLIPMAP_SPLIT_CACHE_PUMP_BUDGET`
- matching `perf_noncapture_smoke.ps1` switches.

Reason:

- The split pump can set sync visible generation budget to `0`, which is only safe
  after the startup public proof has opened.
- A pre-open split-zero run starved visible-proof generation because startup policy
  disables async visible generation while public render is held.
- The retained gate keeps normal startup visible pumping, then activates split pump
  only after public open.

Invalid/rejected probe:

- Artifact: `build/captures/mid_async_visible_split0_footprint_repaircap_pppublic480_20260606`
- It omitted `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME=480`, so public render
  remained held through f480.
- f480 body `156.38 ms`, active `0x5DF`, ownership off, surface raster off,
  coverage `0`; do not compare it as a public-open result.

Accepted validation:

- Build/test:
  - `.\build.ps1 -Config Release`: passed, only existing `rayDir` shadow warnings.
  - `ctest --test-dir build --output-on-failure -C Release`: passed `1/1`.
- Artifact:
  `build/captures/mid_async_visible_postopen_split0_footprint_repaircap_pppublic480b_20260606`
- Strongest stack included:
  `-MidClipmapFootprintInterestSignature`,
  `-MidClipmapAsyncVisibleCriticalGeneration`,
  `-MidClipmapAsyncVisibleCriticalMaxEnqueue 24`,
  `-MidClipmapAsyncVisibleCriticalMaxApply 24`,
  `-MidClipmapSplitVisiblePump`,
  `-MidClipmapSplitVisiblePumpPostOpenOnly`,
  `-MidClipmapSplitVisiblePumpBudget 0`,
  `-MidClipmapSplitCachePumpBudget 0`,
  bounded repair lane, ownership budgets, explicit source lanes, and persistent
  parallel mid pump workers `4`.

Accepted result:

- f480 body `56.29 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `13.38/11.79/5.36/0.01/0.01/9.38/4.11 ms`.
- f456..484 avg/max raw `53.17/70.27 ms`.
- f456..484 avg/max body `56.58/74.40 ms`.
- window avg/max postWait `15.85/16.63 ms`.
- window avg/max surfaceExtract `8.68/10.01 ms`.
- window avg/max sparseUpload `1.93/3.00 ms`.
- f480 visible-critical/cache coverage `94/74`.
- f480 visible/cache misses `354/1981`.
- f480 async visible `enq/complete/apply/drop/dup:21/21/21/0/0`.
- f480 async worker cost `40.87 ms`, queue depth `22`, result depth `1`,
  pending `24`.
- Public path stayed valid:
  `PERF_BACKEND_PIPE active=0x7FF`, ownership on, surface raster on,
  `miss=0`, `unsafeNearMiss=0`.

Decision:

- Carry the post-open-only split-visible async pump as the strongest current
  default-off public-open walk stack.
- This moves public walking from the footprint-only `66-70 ms` window to about
  `53-57 ms`, roughly `18-19 FPS`.
- This is a real trajectory improvement but still far from 60 FPS.
- Do not enable split-visible sync budget `0` before public open.
- The next measured blockers are now post-wait/frame gap around `16 ms`,
  surface extraction around `9-10 ms`, request/gen around `12-13 ms` each,
  cache missing around `1981`, and async mid backlog/pending `24` with about
  `40 ms` worker-side generation cost.

## Generic Incremental Resident Trim Extension - 2026-06-06

Implemented and retained a default-off extension to the existing incremental
pressure trim path:

- `SparseVoxelWorld::TrimResidentBricks(...)` now honors
  `SparseVoxelWorldConfig::incrementalPressureTrim`.
- Added `m_trimResidentCursor`, separate from the existing background resident
  and queued-background trim cursors.
- Default behavior remains unchanged unless
  `VENPOD_SPARSE_PRESSURE_TRIM_INCREMENTAL=1`.

Reason:

- The strongest post-open async visible stack still performed two resident trim
  scans over `131072` records in the public f480 row, while evicting only `8`
  bricks.
- Existing incremental trim only covered the background-specific trim paths, so
  generic resident trim still performed full scans.

Rejected probe:

- `build/captures/mid_async_visible_postopen_split0_surfacebudget0_footprint_repaircap_pppublic480_20260606`
- `-OwnershipStageSurfaceBudget 0` reduced some noncritical surface work but
  worsened the window:
  avg raw/body `58.78/66.83 ms` vs accepted prior `53.17/56.58 ms`.
- It also left a huge queued surface backlog at f480:
  queued surface `3123`, split `cache/prefetch/repair/visible/public:0/1482/1641/0/0`.
- Decision: reject scalar noncritical surface budget zero. Current surface work
  is not safely removable this way.

Incremental resident trim validations:

| Probe | f480 raw/body | window avg/max raw | window avg/max body | trim scan records | f480 safety | Decision |
|---|---:|---:|---:|---:|---|---|
| baseline post-open async split | `50.55/56.29` | `53.17/70.27` | `56.58/74.40` | `131072` | `miss=0 unsafe=0 active=0x7FF` | previous best |
| incremental trim 8192 | `30.66/32.84` | `31.81/44.53` | `37.44/50.21` | `16384` | `miss=0 unsafe=0 active=0x7FF` | useful |
| incremental trim 4096 | `47.62/30.82` | `29.83/47.62` | `34.10/47.58` | `8192` | `miss=0 unsafe=0 active=0x7FF` | useful |
| incremental trim 2048 | `24.76/27.11` | `27.37/39.41` | `29.72/43.20` | `4096` | `miss=0 unsafe=0 active=0x7FF` | accepted current |
| incremental trim 1024 | `44.73/32.68` | `31.53/44.73` | `35.68/50.14` | `2048` | `miss=0 unsafe=0 active=0x7FF` | rejected; worse window |

Accepted artifact:

- `build/captures/mid_async_visible_postopen_split0_incrementaltrim2048_footprint_repaircap_pppublic480_20260606`

Accepted f480 details:

- f480 body/raw `27.11/24.76 ms`.
- f456..484 avg/max raw `27.37/39.41 ms`.
- f456..484 avg/max body `29.72/43.20 ms`.
- f480 request/gen/clip/pump/mainThread/surfaceExtract/gpuRay:
  `7.46/7.65/13.71/0.01/0.01/2.38/9.73 ms`.
- f480 trim scan `calls/records/candidates/evicted:2/4096/0/0`.
- f480 queued upload/surface both `0`.
- f480 async visible `24/24/24/0/0`, async worker `28.85 ms`,
  pending `24`.
- f480 visible/cache coverage `97/76`, visible/cache misses `139/2040`.
- Public path valid:
  `active=0x7FF`, ownership on, surface raster on,
  `miss=0`, `unsafeNearMiss=0`.

Decision:

- Carry `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`
  with the current strongest default-off stack.
- This moves the public walking window from about `53-57 ms` to about
  `27-30 ms`, roughly `34-37 FPS` in the short f456..484 public-open window.
- Still not a 60 FPS candidate. The remaining public walking blockers are now
  GPU ray around `10 ms`, request/gen around `7-8 ms` each, postWait around
  `6-8 ms`, and occasional clip rebuild spikes around `13-14 ms`.
- Next validation should broaden this stack beyond the short f480 walk window:
  longer walk, fixed/static, high-alt, and visual/contact-sheet checks.

## Broad Validation And Noncritical Budget Branches - 2026-06-06

Broadened the current strongest stack:

- Artifact:
  `build/captures/strongstack_incrementaltrim2048_allscenarios_20260606`
- Stack included `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`,
  post-open async visible generation `24/24`, split visible/cache pump `0/0`,
  ownership budgets, explicit source lanes, hidden exact post-open repair lanes,
  and persistent parallel mid pump workers `4`.

Broad result:

| Scenario | target raw/body | window avg/max raw | window avg/max body | ownership | Notes |
|---|---:|---:|---:|---|---|
| fixed f380 | `35.99/34.50 ms` | `34.99/35.99 ms` | `34.63/36.64 ms` | `miss=0 unsafe=0` | postWait/upload/surface still large |
| walk f600 | `39.92/34.86 ms` | `34.85/52.33 ms` | `38.13/52.31 ms` | `miss=0 unsafe=0` | motion hitches remain |
| high-alt f400 | `56.70/54.88 ms` | `53.64/57.20 ms` | `53.74/56.29 ms` | `miss=0 unsafe=0` | postWait/upload/GPU/prePhys dominate |

Decision:

- The short f456..484 public walking row was not representative enough.
- Current broader rough FPS is closer to fixed `28-29 FPS`, walking `26-29 FPS`,
  and high-alt `18-19 FPS`.
- Do not describe the project as near 60 FPS yet. The trajectory is good because
  the architecture is now ownership-clean and measurable, but the broad frame
  budget is still about `2-3x` too high in hard views.

Default-off implementation retained:

- Added `VENPOD_SPARSE_OWNERSHIP_STAGE_ADAPTIVE_NONCRITICAL=1`.
- Added smoke switches:
  `-OwnershipStageAdaptiveNoncritical`,
  `-OwnershipStageAdaptiveMinVisibleCoverage`,
  `-OwnershipStageAdaptiveMaxMissingVisible`,
  `-OwnershipStageAdaptiveUploadFloor`,
  `-OwnershipStageAdaptiveSurfaceFloor`.
- Runtime logs append:
  `adaptive=enabled/active/publicOpen/coverageHealthy/ownershipClean/coverage/missing/uploadFloor/surfaceFloor/uploadBudget/surfaceBudget`.
- This is default-off and should be treated as diagnostic/experimental until a
  broad scenario run beats the baseline.

Rejected/non-accepted probes:

| Probe | Artifact | Result | Decision |
|---|---|---|---|
| static `-OwnershipStageUploadBudget 0` | `strongstack_incrementaltrim2048_uploadbudget0_allscenarios_20260606` | fixed improved, but high-alt/walk were mixed and large upload/surface backlogs accumulated | reject as scalar knob |
| adaptive floor `2/2` | `strongstack_incrementaltrim2048_adaptive_noncrit_floor2_allscenarios_20260606` | fixed/walk/high-alt windows worsened; high-alt still dropped visible-critical coverage after f401 | reject |
| adaptive upload `0`, surface `8` | `strongstack_incrementaltrim2048_adaptive_upload0_surface8_allscenarios_20260606` | a fixed-only probe looked good, but the comparable all-scenario run regressed badly | reject for current stack |
| async visible caps `64/64` | `highalt_asyncvisible64_incrementaltrim2048_20260606` | high-alt coverage still dropped to `86/84` at f402/f403 and frame time worsened to about `99 ms` window raw | reject |

Important finding:

- The high-alt visible-critical coverage drop at f402/f403 is also present in
  the baseline all-scenario run, not just in upload-budget-zero probes.
- Raising async visible caps does not solve it.
- The next real blocker is not a simple noncritical budget cap. It is motion
  stability of the visible-critical set, plus high-alt upload/GPU/post-wait
  pressure.

Next direction:

- Keep the adaptive noncritical governor default-off unless a future broad run
  proves it.
- Investigate why high-alt visible-critical coverage changes from about
  `99%` at f400/f401 to `86/84%` at f402/f403 even with ownership clean.
- Prefer an ownership-preserving visible-critical prediction/reservation or
  LOD/fallback-proof fix over larger async caps or scalar noncritical throttles.

## Stress Camera Clipmap Velocity Diagnostic - 2026-06-06

Implemented a default-off diagnostic switch:

- Runtime env: `VENPOD_SPARSE_MID_CLIPMAP_STRESS_CAMERA_VELOCITY=1`.
- Smoke flag: `-MidClipmapStressCameraVelocity`.
- Code path: high-alt/stress-camera frames can feed clipmap motion lookahead
  from the frame-to-frame residency camera delta instead of the normal
  post-input movement delta.

Why it was tested:

- High-alt uses the stress camera.
- The normal clipmap velocity calculation uses `cameraPos - cameraPosBeforeInputMovement`.
- For stress-camera frames, `cameraPosBeforeInputMovement` is captured after
  stress-camera motion, so clipmap velocity is effectively zero and ordinary
  motion lookahead cannot predict high-alt orbit movement.

Validation:

- Diagnostic enabled:
  `build/captures/highalt_stressclipvelocity_incrementaltrim2048_20260606`
  - f400 raw/body `91.52/94.20 ms`.
  - window avg/max raw `92.21/99.68 ms`.
  - coverage still fell `99/99 -> 86/84` at f400..f403.
  - Reject: it widened work/cost but did not solve the visible-critical drop.
- Diagnostic default-off verification:
  `build/captures/highalt_default_after_stressvelocityflag_20260606`
  - Coverage pattern remained `99/99 -> 86/84`.
  - Confirms the new switch is not accidentally active in the accepted stack.

Related rejected probes:

- Existing moving-window reservation path via direct env:
  `build/captures/highalt_movingwindow_reservation2048_incrementaltrim2048_20260606`
  queued `0` reservations before the f402 transition and did not prevent the
  coverage drop.
- `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT=100`:
  `build/captures/highalt_interest100_incrementaltrim2048_20260606`
  increased interest size and frame time but worsened f402/f403 coverage to
  about `83/82`.

Sharper conclusion:

- The f402 transition is not merely async capacity, current-interest
  reservation, or a missing stress velocity scalar.
- The important backlog line is still the f402 interest rotation:
  `newV/goneV:3080/3080`.
- The next useful architecture slice must admit or protect predicted future
  visible-critical coordinates before they become current-interest `newV`, and
  it must do so without broadening the entire high-alt interest set.

## Mid-Visible Debt Ownership Stage Throttle - 2026-06-06

Implemented a default-off ownership-stage policy hook:

- Runtime envs:
  - `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE=1`
  - `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MIN_COVERAGE`
  - `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_MAX_MISSING_VISIBLE`
  - `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_UPLOAD_FLOOR`
  - `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_SURFACE_FLOOR`
- Smoke flags:
  - `-OwnershipStageMidVisibleDebtThrottle`
  - `-OwnershipStageMidVisibleDebtUploadFloor`
  - `-OwnershipStageMidVisibleDebtSurfaceFloor`
- `PERF_SPARSE_OWNERSHIP_STAGE_BUDGETS` now logs `midDebt=...`.
- `perf_noncapture_smoke.ps1` manifests and parses the mid-debt settings.

Intent:

- When public rendering is open, ownership is clean, and mid visible-critical
  coverage is below target, reduce only noncritical sparse-world upload/surface
  budgets.
- Critical, visible, repair, and public lanes remain eligible.
- This protects visible recovery from broad prefetch downstream debt without
  weakening public-frame ownership correctness.

Same-build high-alt comparison:

| Run | Artifact | Window raw/body | Max raw/body | Notes |
|---|---|---:|---:|---|
| default-off baseline | `highalt_currentfirst_preset_currentbaseline_20260606` | `140.54/141.72 ms` | `196.11/196.04 ms` | avg post-wait `30.14 ms`, avg upload `12.34 ms`, avg surface extract `10.85 ms` |
| mid-debt floor `0/0` | `highalt_currentfirst_preset_midvisibledebt_stage0_20260606` | `138.87/140.13 ms` | `196.89/196.83 ms` | post-wait/upload improved, but `gapPrev` rose to `37.18 ms` |
| mid-debt floor `2/2` | `highalt_currentfirst_preset_midvisibledebt_stage2_20260606` | `139.96/141.70 ms` | `187.17/187.12 ms` | best shape so far: avg post-wait `23.82 ms`, avg upload `5.89 ms`, avg surface extract `10.30 ms` |

Decision:

- Retain the default-off runtime/harness hook and telemetry.
- Do not promote it into the strongest preset yet.
- It proves downstream prefetch upload/surface debt is a real part of the
  frame-time problem, but it does not solve the high-alt window or approach
  60 FPS. The remaining dominant cost is mid-clipmap interest/full-rebuild and
  visible set churn under high-alt motion.

Next direction:

- Keep using ownership-stage mid-debt throttle as a diagnostic and possible
  guardrail, not as the main fix.
- The next architecture slice should attack the large `interest`/full-rebuild
  cost and f402/f403 visible-interest churn: incremental/delta interest reuse
  or a deadline ticket lifecycle that carries predicted visible work through
  generation, apply, upload, and surface without flooding current interest.

## Voxel Interest Signature Reuse - 2026-06-06

Implemented a default-off voxel-interest reuse slice:

- Runtime envs:
  - `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE=1`
  - `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE_MAX_AGE`
- Smoke flags:
  - `-MidClipmapVoxelInterestSignatureReuse`
  - `-MidClipmapVoxelInterestSignatureReuseMaxAge`
- `SparseClipmapTileCache::UpdateVoxelInterest` now has a separate
  voxel-scale signature. When it matches, it reuses the existing
  `m_voxelInterestSet`, touches resident bricks, refreshes missing-interest
  queues, and avoids candidate generation/sort for that frame.
- The temporary predicted-visible path calls `UpdateVoxelInterest(..., false)`
  so prediction still computes a fresh projected queue and does not mutate the
  live reuse signature.
- `PERF_SPARSE_MID_CLIPMAP_BACKLOG` now logs `voxelReuse=active/age`.
- `perf_noncapture_smoke.ps1` manifests and parses
  `midVoxelInterestReuseActive` and `midVoxelInterestReuseAge`.

Same-build high-alt comparison:

| Run | Artifact | Window raw/body | Max raw/body | Notes |
|---|---|---:|---:|---|
| default-off baseline | `highalt_currentfirst_preset_postvoxelreuse_currentbaseline_20260606` | `136.64/138.16 ms` | `180.80/191.39 ms` | reference after code landed |
| voxel reuse age `1` only | `highalt_currentfirst_preset_voxelreusesig_age1_20260606` | `138.01/137.37 ms` | `222.18/222.15 ms` | f401 reuse cut interest, but present/gap hitch worsened |
| voxel reuse age `1` + mid-debt `2/2` | `highalt_currentfirst_preset_voxelreuse_age1_middebt2_20260606` | `134.70/135.77 ms` | `180.30/180.25 ms` | best narrow shape; target cache slipped `93 -> 92` |
| voxel reuse age `2` + mid-debt `2/2` | `highalt_currentfirst_preset_voxelreuse_age2_middebt2_20260606` | `134.98/136.49 ms` | `217.63/217.58 ms` | reject; stale-interest/gap hitch |

Decision:

- Retain voxel-interest signature reuse default-off.
- Do not promote it into the strongest preset yet.
- Age `1` plus mid-visible-debt `2/2` is a useful architecture slice because
  it attacks both expensive interest rebuild and downstream prefetch debt, but
  it remains around `135 ms` in the high-alt window and still needs broad
  fixed/walk/high-alt validation before being trusted.
- Age `2` is rejected.

Next direction:

- Reuse helps only on adjacent frames whose coarse voxel footprint is stable.
  The f402/f403 transition still forces a rebuild and sync pump.
- The next real step should make the deadline ticket lifecycle survive through
  apply/upload/surface, or make the interest rebuild incremental by ring/anchor
  instead of all-or-nothing across the full `9216` voxel interest set.

## Structural Bottleneck Reset - 2026-06-06

The active tool goal was reset after user feedback to:

```text
Drive VENPOD toward stable 60 FPS by identifying and patching structural engine
bottlenecks at their source: preserve public-frame correctness and visual
coverage, measure the dominant cost stage before each change, implement core
dataflow/architecture fixes rather than tuning knobs, and reject changes that
merely shift debt or trade FPS for instability.
```

Reason:

- Recent work drifted into scalar knob/cap probes.
- The user explicitly rejected that process and asked for structural
  bottleneck engineering instead.
- Future work must start with a bottleneck table and patch the top measured
  stage, not add another cap and hope FPS appears.

Rejected cache-queue-cap branch:

- Implemented and then removed a default-off mid-clipmap cache queue cap:
  `VENPOD_SPARSE_MID_CLIPMAP_CACHE_QUEUE_BUDGET` /
  `-MidClipmapCacheQueueBudget`.
- Probe artifacts:
  - `highalt_currentfirst_preset_voxelreuse_age1_middebt2_midcacheq256_20260606`
    window raw/body `134.96/134.73 ms`, max `146.87/146.82 ms`,
    but target mid cache coverage collapsed to `63%`.
  - `highalt_currentfirst_preset_voxelreuse_age1_middebt2_midcacheq96_20260606`
    window raw/body `127.07/127.22 ms`, max `160.84/160.80 ms`,
    but target mid cache coverage collapsed to `62%`.
  - `highalt_currentfirst_preset_voxelreuse_age1_middebt2_midcacheq96_debtgated_20260606`
    window raw/body `146.78/146.98 ms`, max `220.73/220.68 ms`,
    also with target cache coverage `62%`.
- Decision: reject and remove. The branch improved or moved frame time only by
  starving cache coverage, so it violates the visual stability contract.
- Do not revive this as a tuning path.

Current next action:

1. Re-establish the current default-off baseline after removing the rejected
   cap branch.
2. Produce a ranked bottleneck table from the representative noncapture window:
   request, mid-interest, generation, async apply, upload, surface extraction,
   page publish, GPU, post-wait, and gap.
3. Patch the top measured structural stage only if the patch preserves
   ownership, coverage, and max-hitch invariants.

## Structural Bottleneck Pass - 2026-06-06

Clean post-reset baseline:

- Artifact: `build/captures/highalt_currentfirst_structural_baseline_20260606`.
- Window frames `376-404`: raw/body avg `78.73/80.20 ms`, max
  `103.03/113.38 ms`.
- Target frame `400`: body/raw `77.84/81.79 ms`, sparse CPU `50.17 ms`,
  sparse split `request/gen/clip = 12.87/8.10/29.18 ms`.
- `PERF_SPARSE_CLIPMAP frame=400`: clip `29.18 ms`, interest `26.07 ms`,
  queued/missing voxel `1105/612`, visible-critical coverage `100%`,
  cache coverage `93%`.
- Ranked table artifact:
  `build/captures/highalt_currentfirst_structural_baseline_20260606/structural_bottleneck_table.md`.

Rejected probes in this pass:

- Exact per-ring interest plan reuse:
  - Artifact: `highalt_currentfirst_ringplanreuse_20260606`.
  - Result: window raw/body avg `350.90/359.92 ms`, max
    `651.06/651.01 ms`; target interest `108.84 ms`; reuse hits `0`.
  - Decision: removed. The exact plan changes every frame in the high-alt
    path, so this only added hash/planning overhead.
- Local terrain-height cache inside `UpdateVoxelInterest`:
  - Artifact: `highalt_currentfirst_heightcache_20260606`.
  - Result: window raw/body avg `125.95/125.20 ms`, worse than baseline;
    target interest only moved to `24.64 ms`.
  - Decision: removed. Duplicate `HeightAt` calls are not the dominant
    interest cost.
- Bounded frustum sampler replacement for `QueuePredictedVisibleVoxelInterest`:
  - Artifact: `highalt_currentfirst_predsample_20260606`.
  - Result: window raw/body avg `167.24/171.88 ms`, max raw/body
    `327.35/327.30 ms`; target backlog `1689`, max age `231`,
    visible-critical coverage `99%`, cache coverage `81%`, reservation tickets
    inactive.
  - Decision: removed. It reduced duplicate full-interest work but broke the
    scheduler/reservation contract.
- Shared ordered candidate-plan builder for live and predicted interest:
  - Artifacts:
    - `highalt_currentfirst_planbuilder_20260606`
    - `highalt_currentfirst_planbuilder_nonresident_20260606`
    - `highalt_currentfirst_planbuilder_nosetlive_20260606`
  - Best final probe still regressed badly: window raw/body avg
    `247.83/252.87 ms`, max `349.12/348.95 ms`; target frame `400`
    raw/body `250.38/244.25 ms`, clip `69.05 ms`, interest `58.78 ms`,
    active reservation tickets `1153`, overdue `636`.
  - Decision: removed. The builder split preserved much of the queue shape but
    introduced extra allocation/copy/hash cost and over-expanded prediction
    reservations. Do not repeat this as a broad shared builder. If revisited,
    it must be a narrow zero-copy queue-projection helper that exactly mirrors
    the old generated-queue filtering before reserving tickets.
- Top-K selection change from `partial_sort` to `nth_element` + sort:
  - Artifact: `highalt_currentfirst_nthsort_20260606`.
  - Result: window raw/body avg `211.89/219.04 ms`, max
    `544.14/544.09 ms`; target frame `400` raw/body `179.78/191.13 ms`,
    clip `100.80 ms`, interest `75.96 ms`.
  - Decision: removed. It preserved visible coverage but badly worsened the
    hot stage and caused frame-end upload/surface hitches. Keep the existing
    `partial_sort` until a better candidate ordering architecture exists.
- Candidate de-dup map pre-reserve:
  - Artifact: `highalt_currentfirst_candidate_map_reserve_20260606`.
  - Result: the smoke wrapper timed out after frame `404`, then parse-only
    succeeded. Window raw/body avg `393.73/396.72 ms`, max
    `513.16/513.12 ms`; target frame `400` raw/body `371.96/385.38 ms`,
    clip `90.60 ms`, interest `84.40 ms`.
  - Decision: removed. Reserving the unordered map to the full theoretical
    candidate capacity massively increased memory/allocation pressure. Do not
    repeat this broad reserve; if map allocation is revisited, measure actual
    unique candidate counts first and use a much tighter structure.

Useful diagnostic:

- Artifact: `highalt_currentfirst_interestdetail_20260606`.
- This run enabled `-MidClipmapInterestDetail`, so timings include diagnostic
  overhead and should not be used as FPS validation.
- Window detail frames `376-404`:
  - avg/max line `1.43/2.97 ms`
  - avg/max anchor `14.45/41.23 ms`
  - avg/max sort/emit `12.90/24.64 ms`
  - avg/max backlog `0.52/4.24 ms`
  - avg/max diagnostics `4.84/9.04 ms`
  - avg/max candidates `36167/62904`
  - avg/max emitted `24027/27648`
- Frame `400`: `line=0.66`, `anchor=25.96`, `sortEmit=13.68`,
  `candidates=33275`, `emitted=18432`, live interested voxel `9216`.
- Interpretation: line terrain sampling is minor. Anchor candidate generation
  and candidate sort/emit dominate. Emitted counts of `18432` or `27648` while
  live interest is `9216` show that full `UpdateVoxelInterest` is being run
  two or three times in some frames, mainly through predicted-visible admission.

Next structural target:

- Do not replace prediction admission with a loose sampler unless it preserves
  the existing reservation/deadline/priority semantics.
- The correct architecture direction is to split `UpdateVoxelInterest` into:
  1. a reusable candidate-plan builder that can produce ordered voxel coords
     without mutating live queues/stats;
  2. a live current-frame apply path;
  3. a predicted-visible reservation path that consumes the ordered coords and
     preserves deadline tickets.
- That removes duplicate rebuild mutation/snapshot/restore work while keeping
  the queue ordering and reservation behavior that the sampler broke.
- Update after the rejected broad builder: the intent above is still right, but
  the implementation must not materialize full `vector + unordered_set` plans
  for the live path. The viable version is likely an iterator/projection over
  the existing candidate queue construction, or a prediction-only helper that
  emits exactly the old nonresident generated queue without allocating a second
  full interest set.
- Update after the rejected top-K swap: do not treat the sort call as an
  isolated STL-choice problem. The expensive work is the whole candidate
  production and priority contract; replacing `partial_sort` with
  `nth_element` + top-sort regressed badly.
- Update after rejected map reserve: avoid "obvious" container preallocation
  unless it is sized from measured unique counts. The theoretical candidate
  capacity is much larger than the useful unique set and caused severe hitches.

## Mid-Voxel Candidate Source Attribution / Camera-Band Skip - 2026-06-06

Retained diagnostic instrumentation:

- `SparseClipmapCacheStats` and `PERF_SPARSE_MID_CLIPMAP_INTEREST_DETAIL` now
  expose candidate attempts, duplicate hits, and score updates by source:
  `line`, `anchorTerrain`, `anchorFootprint`, and `anchorCamera`.
- This only adds detailed source counters when `-MidClipmapInterestDetail` is
  enabled; normal validation should still use no-detail runs.

Measured source-attribution artifact:

- `build/captures/highalt_currentfirst_candidate_source_detail_20260606`
- Detail window frames `376-404`: avg attempts `115622`, avg duplicates
  `79040`, duplicate rate `68.4%`, avg candidates `36581`, avg anchor
  `5.72 ms`, avg sort/emit `4.92 ms`.
- Source totals:
  - line: attempts `225045`, duplicates `96561`, updates `2925`;
  - anchor terrain: attempts `1393264`, duplicates `638738`, updates `99066`;
  - anchor footprint: attempts `110214`, duplicates `104762`, updates `37597`;
  - anchor camera: attempts `1393264`, duplicates `1294030`, updates `2878`.
- Interpretation: the camera-height safety band was 44.6% of attempts and
  92.9% duplicate hits, while only 0.2% of its duplicate hits improved score.

Retained structural patch:

- Artifact with detail: `highalt_currentfirst_camera_band_overlap_skip_20260606`.
- Artifact without detail, for FPS comparison:
  `highalt_currentfirst_camera_band_overlap_skip_nodetail_20260606`.
- Code change: when emitting the low-priority camera-height band for an anchor
  cell, skip camera y-coordinates already covered by the lower-score terrain or
  footprint vertical band for that same horizontal cell. This preserves camera
  candidates outside terrain/footprint coverage while avoiding guaranteed-losing
  unordered-map duplicate probes.
- Frame `400` detail comparison:
  - attempts `127832 -> 76363`;
  - duplicate hits `94557 -> 43088`;
  - camera attempts `55648 -> 4179`;
  - camera duplicate hits `53097 -> 1628`;
  - clip/interest `22.18/20.38 ms -> 20.66/18.81 ms`;
  - visible-critical coverage stayed `100%`.
- No-detail validation vs clean baseline:
  - baseline `highalt_currentfirst_structural_baseline_20260606` window
    raw/body avg `78.73/80.20 ms`, max `103.03/113.38 ms`; frame `400`
    raw/body `77.84/81.79 ms`, clip/interest `29.18/26.07 ms`.
  - patched no-detail window raw/body avg `71.33/73.17 ms`, max
    `116.32/116.71 ms`; frame `400` raw/body `69.74/71.24 ms`,
    clip/interest `20.62/18.87 ms`.
  - visible-critical coverage stayed `100%`, cache coverage `93%`.
- Decision: keep this patch. It is a source-level removal of losing work, not a
  budget cap. It does not solve 60 FPS; current rough high-alt FPS is still
  about `14 FPS` in this validation window.

Next structural target:

- The remaining duplicate waste is now primarily anchor terrain and footprint:
  terrain duplicates are large but sometimes useful (`~15%` duplicate score
  updates), while footprint is tiny in attempts but very duplicate-heavy and
  often score-improving.
- Do not revive broad plan-builder, `nth_element`, or full-map reserve attempts.
- Next safe direction is a measured source-level candidate architecture:
  identify repeated horizontal anchor footprints before vertical expansion, then
  merge score metadata before emitting y-bands. Any such patch must preserve the
  lower-score update semantics and keep visible-critical coverage at `100%`.

Rejected follow-up:

- Artifact with detail: `highalt_currentfirst_anchor_band_dominance_20260606`.
  It added a conservative whole-band dominance table and looked promising under
  detail logging: avg attempts `68533 -> 52562`, duplicate rate
  `47.2% -> 31.2%`, window raw/body `69.54/70.25 ms`.
- No-detail artifact: `highalt_currentfirst_anchor_band_dominance_nodetail_20260606`.
  This regressed badly: window raw/body avg `88.00/88.46 ms`, max
  `189.44/189.42 ms`; frame `400` raw/body `87.41/83.05 ms`, clip/interest
  `21.26/19.24 ms`; visible-critical coverage stayed `100%` but performance
  was worse than baseline.
- Decision: removed. Do not reintroduce a separate unordered-map dominance table
  in the anchor hot loop. The extra structure can hide under detail-mode
  timings but harm the real path. If terrain/footprint duplication is revisited,
  use a lower-overhead ordered/inline merge or restructure anchor generation
  itself, then validate without detail logging.

Predicted-visible attribution / rejected bounded horizon:

- Behavior-neutral diagnostic retained in `main_launcher.cpp`:
  `PERF_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE` now includes `samplesRun` and
  `elapsedMs`, measuring the full predicted-visible admission loop.
- Artifact: `highalt_currentfirst_predicted_timing_20260606`.
  Key examples:
  - frame `400`: queued `512/512`, `samplesRun=1`, `elapsedMs=11.82`,
    frame raw/body `71.62/70.87 ms`, clip/interest `19.85/18.11 ms`,
    visible-critical coverage `100%`;
  - frame `402`: queued `231/512`, `samplesRun=2`, `elapsedMs=25.39`;
  - frame `403`: queued `120/512`, `samplesRun=2`, `elapsedMs=24.66`.
- Interpretation: predicted admission is a major part of `clipMs`, but it is
  also maintaining the reservation horizon that keeps later coverage from
  collapsing. Do not treat it as disposable overhead.
- Rejected code branch: prediction-only `residentInterestLimitOverride` in
  `UpdateVoxelInterest`, with predicted interest capped to `maxCoords * 4`.
  Artifact: `highalt_currentfirst_predicted_interest_limit_20260606`.
  Result: window raw/body avg `113.28/114.64 ms`, max `723.37/723.33 ms`;
  frame `400` queued only `45`, reservations fell to `29` active tickets,
  cache coverage fell to `80%`, visible-critical coverage `99%`; frames
  `402/403` coverage fell to `85/84%`.
- Decision: removed. The current predictor needs a broad interest horizon to
  discover nonresident future-visible coords. A future fix must preserve that
  horizon without rebuilding/mutating/restoring the full live interest state for
  every prediction sample. The promising direction is not a smaller horizon; it
  is a prediction-only read-only candidate emitter or reusable ordered stream
  that preserves full-horizon ordering and reservation semantics with less state
  churn.

Rejected prediction state-churn probes:

- Artifact: `highalt_currentfirst_prediction_queue_move_20260606`.
  Change tried: move/swap the predicted generation queue out of
  `m_voxelGenerationQueue` instead of copying it after the predicted rebuild,
  plus reserve `m_visiblePriorityVoxelSet`, `m_asyncVisibleReservations`, and
  `m_queuedVoxelSet` for `maxCoords`.
  Result: window raw/body avg `82.50/87.32 ms`, frame `400` raw/body
  `81.30/81.94 ms`, predicted elapsed avg/max `18.74/33.54 ms`,
  visible-critical coverage minimum `91%`. Decision: removed.
- Artifact: `highalt_currentfirst_prediction_queue_move_only_20260606`.
  Change tried: keep only the predicted queue move/swap and remove the reserve
  calls.
  Result: window raw/body avg `90.68/91.88 ms`, max `391.35/391.33 ms`;
  frame `400` raw/body `88.40/98.36 ms`, predicted elapsed avg/max
  `16.18/27.05 ms`, coverage minimum `91%`. Decision: removed.
- Interpretation: do not optimize this path with local container move/reserve
  tweaks. The cost and coverage behavior are tied to the full predicted rebuild
  and reservation semantics. The next viable patch needs a deeper prediction
  candidate stream/refactor, not swapping the final queue object or reserving
  hot sets.

Predicted-visible phase timing / rejected signature cache:

- Retained diagnostic:
  `PERF_SPARSE_MID_CLIPMAP_PREDICTED_VISIBLE` now includes
  `phaseMs=snapshot/rebuild/restore/queue`. These internal phase timers are
  gated behind `voxelInterestDetail`; normal no-detail runs keep the outer
  `elapsedMs` only and print zero phase splits.
- Attribution artifact: `highalt_currentfirst_predicted_phase_detail_20260606`.
  This run is not a performance candidate because detail timing made the window
  very slow: raw/body avg `267.74/268.42 ms`. The attribution is still useful:
  predicted admission averaged `51.59 ms`, with phase averages
  snapshot/rebuild/restore/queue `3.43/39.65/1.70/1.67 ms`. The rebuild phase
  dominates; snapshot/restore/queue are not the main structural cost.
- Rejected code branch: predicted-visible signature cache that reused the
  previous full predicted generation queue when the quantized predicted view
  signature matched.
  Artifact: `highalt_currentfirst_predicted_signature_cache_20260606`.
  Result: it produced reuse hits but regressed badly: window raw/body avg
  `105.35/108.59 ms`; frame `400` raw/body `113.95/102.52 ms`; predicted
  elapsed avg/max `16.63/47.63 ms`; coverage minimum `91%`. Some reuse hits
  queued far too few useful reservations, so the cache harmed reservation
  coverage and frame time.
  Decision: removed.
- Current retained no-detail validation after removing the cache:
  `highalt_currentfirst_phase_gated_current_20260606`.
  Window raw/body avg `63.89/64.99 ms`, max `85.92/90.34 ms`; frame `400`
  raw/body `62.91/66.89 ms`, clip/interest `20.71/18.90 ms`, visible-critical
  coverage `100%`, cache coverage `93%`; frames `402/403` coverage
  `89/90%` with active reservation tickets `903/877`.
- Updated conclusion: the next structural patch should not cache entire
  predicted queues by view signature. The right target remains a prediction-only
  ordered candidate stream that avoids full live-state mutation while rebuilding
  enough of the broad horizon every frame to preserve reservation coverage.

Pure predicted collector runtime wiring rejected - 2026-06-06:

- Retained diagnostics:
  - `SparseClipmapTileCache::CollectPredictedVisibleVoxelInterestPureForDebug`
    now has an optional resident-touch side channel. Its returned queue remains
    nonresident-only and still matches the stateful debug collector order.
  - `TestSparseClipmapTileCache` now includes a high-alt four-ring policy check
    using the runtime camera regime. It verifies stateful/pure predicted queue
    equivalence and verifies that pure prediction can expose resident candidates
    that the stateful rebuild used to touch as a hidden side effect.
- Rejected runtime branch 1:
  replaced `QueuePredictedVisibleVoxelInterest`'s snapshot/update/restore block
  with the pure predicted candidate queue.
  Artifact: `highalt_currentfirst_pure_predicted_runtime_20260606`.
  Result: f376..404 avg raw/body `73.67/74.55 ms`, max
  `93.92/97.87 ms`; frame 400 raw/body `83.73/74.13 ms`, coverage
  visible/cache `99/92`, reservation tickets active/due/overdue
  `1175/660/659`. Decision: removed from runtime.
- Rejected runtime branch 2:
  same pure queue, plus explicit predicted resident `lastTouchedFrame` updates
  from the new side channel.
  Artifact: `highalt_currentfirst_pure_predicted_resident_touch_20260606`.
  Result: f376..404 avg raw/body `78.78/79.52 ms`, max
  `319.58/319.56 ms`; frame 400 raw/body `70.80/73.19 ms`, coverage
  visible/cache `99/92`, reservation tickets active/due/overdue
  `1190/675/673`. Decision: removed from runtime.
- Runtime is back on the stateful predicted rebuild path. Build and
  `VENPODTests.exe` pass after the revert.
- Lesson: the pure collector is not yet a drop-in runtime replacement even
  when resident touches are preserved. The remaining difference is likely not
  candidate order in the tested high-alt policy; it is admission/pressure timing
  across the full frame window. Do not wire it again without a narrower proof
  for reservation due/overdue behavior across frames 376..404.

Retained predicted-visible pacing - 2026-06-07:

- Dominant fresh-tree probe before patch:
  `highalt_currentfirst_current_probe_20260607`.
  Window f376..404 raw/body avg `91.71/93.09 ms`, max
  `120.59/120.57 ms`. Frame 400 raw/body `97.62/91.33 ms`;
  CPU/request/gen/clip/surfaceExtract/GPU frame/ray
  `43.28/12.16/9.72/21.39/9.25/7.49/6.62 ms`;
  visible/cache coverage `100/93`; reservations active/due/overdue
  `493/0/0`. Frames 402/403 were the real spike: clip `53.67/60.54 ms`,
  interest `39.95/41.92 ms`, and predicted-visible elapsed
  `27.11/28.59 ms` because both configured prediction samples ran in one
  frame.
- Rejected ablation-only evidence:
  `highalt_currentfirst_predicted_samples1_ablation_20260607`.
  Manual no-preset run with one sample lowered duplicate rebuild work but did
  not preserve the configured lead-horizon semantics; use only as attribution.
- Retained patch:
  `src/main_launcher.cpp` now paces predicted-visible admission so the
  configured sample count is treated as a rotating horizon set. Only one
  predicted sample rebuild runs per frame, alternating sample index by
  `frameCount % sampleCount`, instead of stacking all full predicted
  `UpdateVoxelInterest` rebuilds into a single frame when the first horizon
  does not fill `maxCoords`.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_predicted_paced_20260607`
    window raw/body avg `72.01/72.55 ms`, max `86.01/90.18 ms`;
    frame 400 raw/body `77.17/72.01 ms`, clip `19.67 ms`,
    visible/cache coverage `99/93`, reservations `504/0/0`.
  - Smoke 2: `highalt_currentfirst_predicted_paced_rerun_20260607`
    window raw/body avg `66.18/67.02 ms`, max `91.37/91.35 ms`;
    frame 400 raw/body `68.38/66.58 ms`, CPU/request/gen/clip/surfaceExtract
    `38.77/9.90/8.14/20.73/5.12 ms`, visible/cache coverage `99/93`,
    reservations `504/0/0`.
- Decision: keep. This is not a final 60 FPS solution, but it removes the
  repeated predicted-interest rebuild spike structurally while preserving the
  configured prediction horizon over adjacent frames. Next dominant remaining
  costs after the retained patch are still clip interest around `20-26 ms` and
  frame-window post-wait/surface/upload variance; do not return to same-frame
  multi-sample prediction unless a future batched predictor avoids repeated
  full `UpdateVoxelInterest` rebuilds.

Rejected post-pacing voxel-interest reuse and async-visible fallback - 2026-06-07:

- Voxel-interest signature reuse was retested after the retained predicted
  pacing patch because the older rejection was measured against a much slower
  baseline.
- First post-pacing reuse run:
  `highalt_currentfirst_paced_voxelreuse_age1_20260607`.
  Window raw/body avg `63.46/64.19 ms`, max `81.69/84.46 ms`;
  frame 400 raw/body `65.10/62.67 ms`; visible/cache coverage `99/93`,
  reservations `506/0/0`. This looked promising but needed repeat validation.
- Invalid repeat:
  `highalt_currentfirst_paced_voxelreuse_age1_rerun_20260607`.
  The smoke window contained only one parsed frame, so do not use it for
  performance decisions.
- Valid repeat:
  `highalt_currentfirst_paced_voxelreuse_age1_rerun2_20260607`.
  Window raw/body avg `73.20/73.70 ms`, max `125.59/125.57 ms`;
  frame 400 raw/body `78.76/71.43 ms`; visible/cache coverage `99/93`,
  reservations `506/0/0`. Decision: do not promote voxel reuse into
  `highalt-currentfirst`; keep it default-off. It is not repeat-stable enough.
- Structural finding from the repeat:
  reuse saves only identical-footprint adjacent frames, e.g. frame 401. Frames
  402/403 still full-rebuild current interest, drop visible/cache coverage to
  about `91/89`, and enter sync repair with `parallelPump=24` because the
  coverage guard disables the budgeted path.
- Rejected code experiment:
  briefly patched general `PumpGeneration` so that, after synchronous fallback
  repair, it also fed `QueueAsyncVoxelGenerationMatchingPriority(true, ...)`.
  This made the fallback path enqueue async visible work on f402/f403
  (`asyncVisible enq=24` instead of `0`) but worsened the real smoke:
  artifact `highalt_currentfirst_generalpump_asyncvisible_20260607`, window
  raw/body avg `69.34/70.59 ms`, max `103.46/105.13 ms`; f402/f403 still
  sync-pumped 24 bricks and clip/pump cost increased. Decision: removed.
  Build and `VENPODTests.exe` pass after removal.
- Current rough high-alt performance after retained pacing remains the paced
  rerun baseline: about `66-67 ms/frame`, roughly `15 FPS`, not close to 60 FPS.
- Next viable architecture target:
  stop trying to reuse whole interest sets or tack async enqueue onto the
  fallback. The real bottleneck is the f402/f403 transition where current
  interest churn creates 800+ visible misses and drops into synchronous repair.
  The next design should either make visible-interest construction incremental
  by ring/anchor, or create a prediction candidate stream/deadline lifecycle
  that carries work through generation/apply/upload/surface before the current
  frame needs it, without full `UpdateVoxelInterest` mutation and without
  depending on identical-footprint reuse.

Retained voxel-interest terrain-height cache - 2026-06-07:

- User correctly pushed back that `14-15 FPS` is not meaningful success. The
  next patch attacked a real CPU source in the full voxel-interest rebuild:
  repeated `m_terrain.HeightAt(...)` calls for the same ring brick coordinates
  across centerlines, high-alt fans, anchors, and footprint-neighbor checks.
- Retained code:
  `SparseClipmapTileCache::UpdateVoxelInterest` now keeps a per-ring terrain
  height cache keyed by `(brickX, brickZ)` during the rebuild. Candidate
  construction still samples the same brick-center world positions and emits
  the same visible/cache interest counts; it avoids recomputing procedural
  terrain height for duplicated candidate coordinates.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_interest_terraincache_20260607`
    window raw/body avg `61.53/61.96 ms`, but max `158.76/158.74 ms` due an
    isolated `postRender=102.32 ms` hitch on f397. The sparse/clip path on that
    hitch frame was normal (`clip=13.92`, `interest=12.09`).
    f400 interest `13.99 ms`; f402 `17.85 ms`; f403 `17.20 ms`;
    visible/cache coverage stayed `99/93` at f400 and `91/89` at f402/f403.
  - Smoke 2: `highalt_currentfirst_interest_terraincache_rerun_20260607`
    window raw/body avg `76.25/76.63 ms`, max `503.64/503.62 ms` from a
    pre-window scheduling gap carried into f376/f377. Hotspot frames still
    showed the cache win: f400 interest `14.68 ms`, f402 `17.79 ms`,
    f403 `16.82 ms`, with coverage unchanged.
  - Smoke 3: `highalt_currentfirst_interest_terraincache_rerun2_20260607`
    window raw/body avg `67.96/68.28 ms`, max `115.46/115.44 ms`; f400
    interest `14.32 ms`, f402 `16.52 ms`, f403 `17.16 ms`, coverage unchanged.
- Comparison against retained paced baseline:
  `highalt_currentfirst_predicted_paced_rerun_20260607` had f400/f402/f403
  interest `18.52/25.65/26.29 ms`. The terrain cache repeatedly cuts those
  hot frames by roughly `4-9 ms` without changing interest cardinality or
  visible/cache coverage. Whole-window averages are still polluted by separate
  frame-pacing/post-render/gap hitches, so this is not a 60 FPS solution.
- Rejected refinement:
  a flat vector terrain cache (`highalt_currentfirst_interest_flatterraincache_20260607`)
  was slower and unstable: window raw/body avg `74.75/85.91 ms`; f400/f402/f403
  interest `17.81/20.80/21.09 ms`. Reverted to the `unordered_map` cache.
- Rejected cross-frame persistent terrain cache:
  `highalt_currentfirst_interest_persistentterraincache_20260607`.
  This moved the height cache onto `SparseClipmapTileCache` so current and
  predicted interest rebuilds could share static terrain samples across
  frames. It improved f400/f402 (`13.66/16.32 ms`) but destabilized f403:
  `interest=74.53 ms`, `prep=91.38 ms`, body `131.34 ms`. Decision: removed.
  Keep the local per-ring cache until a persistent design has stable eviction
  and instrumentation for cache size/hit/miss behavior.
- Current rough performance after retained pacing + terrain-height cache:
  deterministic clip-interest cost is lower, but the high-alt window is still
  only around the low-to-mid teens FPS in practice because remaining costs are
  post-wait/frame pacing, upload/surface, and the f402/f403 sync repair path.
  The next structural target should not be another interest micro-cache; it
  should address the frame-pacing/post-render hitches or the visible-miss burst
  that forces `parallelPump=24` sync repair at f402/f403.

Retained projected-visible split-lane selection - 2026-06-07:

- User correctly called out that `14 FPS` is outside the realm where correctness
  polish matters. The next retained change attacked the f402/f403 control-flow
  failure where projected visible misses existed and were prioritized, but
  `sparseMidClipmapVisibleCriticalPrepumpActive` was false, so the engine fell
  back to generic `PumpGeneration` and synchronously generated 24 visible bricks
  on the main thread.
- Retained code in `src/main_launcher.cpp`:
  - Adds `sparseMidClipmapProjectedVisibleLaneReady`.
  - Keeps `sparseMidClipmapVisibleCriticalPrepumpActive` as the broad coverage
    budget override.
  - Allows the split visible pump/cache-defer path when projected visible work
    was computed and prioritized, even if the coverage guard still disables the
    budgeted pump.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_projected_split_lane_20260607`
    window raw/body avg `62.87/63.02 ms`, max `69.97/69.95 ms`.
    f402/f403 main-thread brick generation dropped from the prior local-cache
    values of about `9.61/7.74 ms` to `0.13/0.15 ms`; `parallelPump` dropped
    from `24` sync bricks to `0`; async visible enqueue resumed at `24/24`.
  - Smoke 2: `highalt_currentfirst_projected_split_lane_rerun_20260607`
    window raw/body avg `67.74/67.97 ms`, max `160.71/160.69 ms` due an
    unrelated f382 post-wait/ownership-feedback hitch (`postWait=112.76 ms`,
    `gapPrev=98.65 ms` on following frame). The targeted f402/f403 behavior
    repeated: main-thread brick generation stayed `0.13/0.15 ms`, async visible
    enqueue stayed `24/24`, and public miss/unsafe screen percentages stayed
    `0`.
- Tradeoff:
  f402/f403 visible debt still exists (`missingVisible` around `798/834` in the
  lane log) because the split lane only queues async work; it does not make the
  single async generator catch up within the same frame. This is still a better
  structural state than paying synchronous repair on the public frame.
- Current rough performance after retained split-lane fix:
  high-alt still sits around `63-68 ms/frame` in the measured window, roughly
  `15-16 FPS`. This is not close to 60 FPS. The retained improvement is that a
  bad sync-repair control-flow path is removed; the next bottlenecks are async
  generation throughput/debt lifecycle, clip-interest rebuild cost, and
  post-wait/frame-pacing hitches.

Rejected async voxel generation worker pool - 2026-06-07:

- Experiment:
  replaced the single async voxel generation thread with a small worker pool
  using `parallelVoxelPumpMaxWorkers` when parallel voxel pump was enabled.
  The queue, pending set, result queue, and apply gates were left unchanged.
- Validation before rejection:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke: `highalt_currentfirst_async_workerpool_20260607`
    window raw/body avg `185.26/185.57 ms`, max `3571.41/3571.39 ms`.
    The run hit a catastrophic scheduling gap around f399/f400
    (`gapPrev=3509.87 ms`). f402/f403 visible debt did not meaningfully improve.
- Decision:
  rejected and reverted. The current source is back to the single async worker.
  Build and `VENPODTests.exe` passed after revert. Do not retry a naive worker
  pool without a proper bounded scheduler, CPU affinity/priority strategy, and
  instrumentation for queue latency versus worker wall time.

Candidate reserve / bounded-rank audit - 2026-06-07:

- User pushed back correctly that `14 FPS` means the system is still outside
  practical correctness. Treat visual correctness as a guardrail, not the win.
- Low-risk retained edit in `src/Simulation/SparseClipmap.cpp`:
  `UpdateVoxelInterest` now reserves `candidateIndexByCoord` with the same
  estimated capacity already used for the candidate vector. This preserves
  stateful/pure ordering and passed:
  - `.\build.ps1 -Config Release`
  - `.\build\bin\VENPODTests.exe`
- Reserve-only smoke:
  `highalt_currentfirst_candidate_reserve_20260607`.
  The window summary was polluted by unrelated multi-second scheduling hitches
  (`avgRawMs=295.93`, `maxRawMs=6263.30`, `gapPrev=6206.25`), so do not use
  that window average as the rough FPS. Per-frame sparse timings around the
  target remained comparable:
  - f400 `clip=16.79`, `interest=14.87`, body/raw `66.67/69.89`
  - f402 `clip=21.08`, `interest=18.44`, body/raw `71.34/65.49`
  - f403 `clip=26.67`, `interest=17.73`, body/raw `70.83/71.36`
- Rejected sort/ranking experiment:
  made candidate ties deterministic in both stateful and pure collectors and
  replaced bounded `partial_sort` with `nth_element + sort`.
  Build/tests passed, but smoke `highalt_currentfirst_bounded_rank_20260607`
  was worse:
  - window raw/body avg `78.68/78.38 ms`
  - f400 `interest=16.32` vs reserve-only `14.87`
  - f403 `interest=19.25` vs reserve-only `17.73`
  Reverted the tie-break and `nth_element` changes. Do not retry this as a
  standalone fix.
- Current rough FPS after retained split-lane + local terrain cache +
  candidate-map reserve:
  still roughly `14-16 FPS` in clean comparable frames, not close to 60.
  The dominant structural problem is that mid-voxel interest rebuilds are still
  full candidate-set rebuilds every frame (`~15-19 ms` interest alone in the
  high-alt target area, with prior detail showing up to `~108k` attempts for
  `9216` emitted bricks). Next work should change the interest architecture
  itself: incremental/delta interest, per-ring dirty movement thresholds, or a
  bounded lane model that avoids generating and deduplicating all candidates
  every public frame.

Rejected per-ring voxel interest reuse/cadence - 2026-06-07:

- Experiment:
  added a retained per-ring voxel interest cache inside `UpdateVoxelInterest`.
  First version reused only when a conservative ring footprint hash matched;
  this was too strict and produced no reuse in the high-alt target frames.
  Second version allowed outer rings to carry a one-frame stale footprint while
  ring 0 rebuilt every frame.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_outer_ring_reuse_20260607`
    window raw/body avg `62.30/62.70 ms`, max `72.18/81.06 ms`.
    f400/f402 interest improved (`13.57/16.02 ms`) but f403 worsened
    (`21.44 ms`) with a body outlier.
  - Smoke 2: `highalt_currentfirst_outer_ring_reuse_rerun_20260607`
    window raw/body avg `63.71/63.98 ms`, max `71.36/72.29 ms`.
    f402 again improved (`interest=16.69`, missingVoxel `363`) but f403
    again paid back the debt (`interest=19.49`, pump `7.69`).
- Decision:
  rejected and reverted. The two-frame f402/f403 interest sum was essentially
  unchanged versus reserve-only, and the change shifted work/debt onto rebuild
  frames rather than lowering the structural cost. Build and tests passed after
  revert. Do not revive one-frame stale ring cadence without a design that
  incrementally updates changed ring deltas instead of deferring them.

Frame wait / GPU ray diagnostics - 2026-06-07:

- Inspection:
  `perfPostWaitGapMs` comes from waiting on the current swap-chain frame
  context's prior fence before resetting its command allocator:
  `commandQueue->WaitForFenceValue(ctx.fenceValue)` in `src/main_launcher.cpp`.
  The smoke preset runs with vsync on unless `-DisableVSync` is passed.
- No-vsync diagnostic:
  `highalt_currentfirst_novsync_diag_20260607`
  used the retained source state after reverting ring cadence.
  Result: window raw/body avg `68.50/69.15 ms`, postWait avg `19.26 ms`,
  f400 GPU frame/ray `21.34/16.12 ms`, f402 GPU frame/ray `21.56/16.33 ms`,
  f403 GPU frame/ray `22.56/17.22 ms`.
  Disabling vsync did not collapse post-wait; it made the run worse. Treat
  post-wait as GPU/frame-context back-pressure, not just presentation pacing.
- Background scale diagnostic:
  `highalt_currentfirst_bgscale025_diag_20260607`
  forced background pass scale from `0.375` (`360x203`) to `0.25` (`240x135`).
  It reduced some ray timestamp samples (`f400 gpuRay=13.49 ms`) but did not
  change the frame class: clean frames still sat roughly `62-69 ms`, and the
  window had a large scheduling hitch (`maxRaw=476.31 ms`).
- Decision:
  no code retained. Lowering background resolution alone is a knob, not a
  structural fix, and it does not get close to 60 FPS. The next structural GPU
  target should be shader/work culling inside `PS_Raymarch.hlsl` /
  `RaymarchBackgroundField`: reduce per-pixel far/mid traversal and avoid
  unnecessary water/far-field probes for pixels already classified by sparse
  surface or background ownership. CPU interest is still too expensive, but
  GPU ray time is also over budget at `~16-17 ms` in the target window.

Shader compile / signature reuse / trim probes - 2026-06-07:

- Shader compile recovery:
  after the rejected `RaymarchBackgroundField` cull attempt was reverted, the
  next smoke initially timed out on a fresh `PS_Raymarch.hlsl` DXC compile.
  Killing the hung harness left a completed cache file:
  `PS_Raymarch_hlsl_ps_6_0_main_795fc84c5cf0529e.cso` (`7386940` bytes).
  Rerunning with that cache present completed normally:
  `highalt_currentfirst_post_shader_revert_cached_20260607`.
- Current clean retained baseline:
  `highalt_currentfirst_post_shader_revert_cached_20260607`
  window raw/body avg `64.80/65.00 ms` (`~15.4 FPS`), target f400
  raw/body `64.58/64.81 ms`, CPU update `36.24 ms`, clip `17.43 ms`,
  interest `15.54 ms`, request `10.11 ms`, gen `8.70 ms`,
  surface extract `6.36 ms`, GPU frame/ray `21.10/15.97 ms`,
  coverage `99`, missingVoxel `602`, mainThreadBrickGenMs `0.08`.
  This confirms the split visible-lane fix is still holding; the remaining
  problem is not the prior 9 ms sync visible generation regression.
- Signature reuse probes:
  explicit `-MidClipmapVoxelInterestSignatureReuse` showed some reuse on
  intermediate frames (`voxelReuse=1/1`) and one probe improved the 28-frame
  window to `63.24 ms`, but an edited-code default rerun did not reproduce a
  win (`64.85 ms` window, target raw `70.62 ms`). The default-change patch was
  reverted. Do not count signature reuse alone as an accepted fix; it is only
  a clue that the real direction is stable interest epochs / delta interest.
- Pressure trim probes:
  `-PressureTrimFreePageGuard` removed the full-pool trim scans but shifted
  debt into resident/surface/upload work and worsened the window to
  `70.50 ms`. `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 32768`
  still spent about `5.5 ms` in trim and produced a `612 ms` hitch. Both are
  rejected as standalone fixes. The trim path needs an indexed queue or true
  eviction-candidate cache, not scan-budget tuning.

Rejected moving-window priority / high-alt camera-band gate - 2026-06-07:

- Moving-window priority experiment:
  `-MidClipmapMovingWindowPriority` tried prioritizing current moving-window
  work ahead of the normal target frame. It worsened the window to about
  `84 ms/frame`, left f402/f403 visible coverage around `77/76`, and increased
  post-wait pressure. Rejected. The moving window cannot simply jump the queue;
  it needs deadline/state ownership through generation, upload, surface, and
  publish if it is going to help.
- High-alt camera-band gate experiment:
  temporarily skipped camera-height vertical anchor bands in
  `UpdateVoxelInterest` and the pure debug collector when high-altitude camera
  bands were far from terrain/footprint bands. This was structurally plausible
  because interest-detail logs showed about `4524` camera-anchor attempts in
  f402, but it was not dominant.
- Validation before rejection:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke: `highalt_currentfirst_skip_highalt_camera_band_20260607`
    window raw/body avg `69.34/69.59 ms`, max `94.15/94.13 ms`;
    f400 raw/body `65.93/69.77 ms`, CPU/request/gen/clip/surfaceExtract
    `34.84/9.62/8.57/16.63/5.77 ms`, GPU frame/ray `21.53/16.21 ms`.
- Decision:
  rejected and reverted. Build and `VENPODTests.exe` pass after revert. Do not
  retry anchor-source pruning unless a detail run shows it is a dominant stage
  and the full-frame smoke improves. The current rough clean baseline remains
  `highalt_currentfirst_post_shader_revert_cached_20260607`, about
  `64.80/65.00 ms` or `~15.4 FPS`, still far outside the 60 FPS target.

Restored baseline and rejected queue-backed trim - 2026-06-07:

- After reverting the high-alt camera-band gate, a clean accepted-source smoke
  was rerun:
  `highalt_currentfirst_restored_after_camera_gate_revert_20260607`.
  Window raw/body avg `63.71/63.83 ms`, max `70.30/70.28 ms`, or about
  `15.7 FPS`. Frame 400 raw/body `64.25/64.83 ms`; CPU/request/gen/clip
  `34.30/9.40/7.64/17.25 ms`; interest `14.76 ms`; surface extract
  `6.13 ms`; GPU frame/ray `20.39/15.13 ms`; post-wait window avg
  `18.38 ms`; coverage `99/93`; main-thread brick gen `0.10 ms`.
- Experiment:
  changed `SparseVoxelWorld::TrimQueuedBackgroundBricks` to build trim
  candidates from generation/upload/deferred queues plus generated payload
  entries instead of scanning every pool record. This was intended to preserve
  queued-eviction semantics while removing one O(pool) scan.
- Validation before rejection:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke: `highalt_currentfirst_queue_backed_trim_20260607`
    window raw/body avg `67.10/67.21 ms`, max `71.41/74.31 ms`.
    It reduced trim scanned records at f400 from about `131072` to `73101`,
    but pressure-trim time worsened to `5.77 ms` and request time to
    `10.93 ms`; f402 pressure-trim reached `6.65 ms`.
- Decision:
  rejected and reverted. Build and `VENPODTests.exe` pass after revert. The
  lesson is that replacing flat pool scans with unordered queue validation is
  not enough; it trades linear memory traversal for expensive hash/map probes.
  A future trim fix needs a maintained eviction-candidate index or per-state
  dense active-record lists owned by the pool/lifecycle transitions, not an
  ad hoc queue walk.
- Current rough FPS:
  still `~15-16 FPS` in the clean high-alt window. The next structural target
  should be one of:
  (1) an actual incremental/delta voxel-interest builder to remove the
  `~15-18 ms` full candidate rebuilds,
  (2) an ownership-aware async visible deadline pipeline so f402/f403 visible
  misses are generated/applied/surfaced before they become current-frame debt,
  or
  (3) a real GPU raymarch work reduction, because `gpuRay` remains
  `~15-17 ms` and frame-context post-wait remains `~18 ms`.

Current interest-detail and rejected candidate-index rewrites - 2026-06-07:

- Detail probe:
  `highalt_currentfirst_interestdetail_current_20260607`.
  This run is intentionally slower because detail timing is enabled, but it
  shows the current interest substage shape:
  - f400 interest `15.84 ms`, candidates `33275`, attempts `76363`,
    duplicate hits `43088`, emitted `18432`, interested `9216`.
    Substage timing: line `0.34 ms`, anchor `3.13 ms`, sort/emit `4.13 ms`,
    backlog `0.16 ms`, diagnostics `1.56 ms`.
  - f402/f403 interest `19.01/19.58 ms`, candidates `41428/41901`,
    attempts `108651/108827`, duplicate hits `67223/66926`, emitted `18432`.
    Substage timing: anchor `4.57/4.48 ms`, sort/emit `4.61/4.83 ms`.
  - Conclusion: the dominant structural issue is still rebuilding and
    deduplicating a full candidate universe every public frame, especially on
    f402/f403. The many duplicate hits are a symptom, but changing only the
    map implementation did not help.
- Rejected lightweight candidate-key rewrite:
  changed the local candidate-index maps in the pure debug collector and
  runtime `UpdateVoxelInterest` from full `SparseVoxelClipmapCoord` keys to an
  implicit-ring `{x,y,z}` key. Build/tests passed, but smoke
  `highalt_currentfirst_candidate_key_20260607` regressed the window to
  `67.26/67.43 ms` raw/body and left interest roughly unchanged or worse
  (`f400/f402/f403 interest 15.34/18.90/18.56 ms`). Reverted.
- Rejected open-addressed candidate index:
  replaced the two local `std::unordered_map` candidate indexes with a
  per-ring open-addressed index to remove node allocation and preserve exact
  candidate order. Build/tests passed, but smoke
  `highalt_currentfirst_candidate_openindex_20260607` regressed badly:
  window `79.00/79.25 ms` raw/body, f400/f402/f403 interest
  `19.88/24.82/25.15 ms`. Reverted.
- Decision:
  do not keep exploring candidate hash/index variants as standalone fixes.
  The next interest-side implementation must avoid doing the full work:
  explicit per-ring delta updates, candidate epoch carry-forward with verified
  changed-coordinate insertion/removal, or an async/deadline pipeline that
  builds future visible interest outside the public-frame critical path.

Rejected runtime pure-prediction admission - 2026-06-07:

- Experiment:
  replaced the runtime `QueuePredictedVisibleVoxelInterest` snapshot/full
  `UpdateVoxelInterest`/restore path with the retained pure debug collector
  `CollectPredictedVisibleVoxelInterestPureForDebug`, and also tagged/touched
  resident predicted coords before queueing missing candidates.
- Validation before rejection:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke: `highalt_currentfirst_prediction_pure_runtime_20260607`
    window raw/body avg `64.87/65.15 ms`, max `72.68/72.66 ms`.
    f400 raw/body `72.68/65.15 ms`; CPU/request/gen/clip
    `38.72/9.89/7.67/21.15 ms`; interest `18.90 ms`; surface extract
    `6.87 ms`; GPU frame/ray `19.21/14.13 ms`.
    f400 visible/cache coverage `99/90`, missingVoxel `850`,
    backlogVoxel `1357`.
    f400 reservation tickets regressed to active/due/overdue/hits/maxAge
    `1099/590/590/1/20` versus the accepted baseline's healthy
    `504/0/0/4/0` class.
    f402/f403 visible/cache coverage stayed only `90/88` and `90/87`, with
    overdue reservations `26/25`.
- Decision:
  rejected and reverted. Build and `VENPODTests.exe` pass after revert. This
  proved that pure candidate equivalence in debug is not enough to replace the
  runtime path: admission, resident touch, backlog, and reservation lifecycle
  semantics are coupled. Do not retry this by swapping collector internals.
  The next viable runtime prediction fix must first make the deadline pipeline
  explicit and measurable, or move toward true incremental interest ownership
  where current-frame, predicted-visible, queued, resident, and overdue states
  have one source of truth.
- Current rough FPS remains `~15-16 FPS`, not close to 60. Treat this as an
  architecture problem with three measured top costs: full voxel-interest
  candidate rebuilds (`~15-19 ms`), GPU ray/post-wait pressure (`~15-20 ms`
  each), and async-visible debt that is queued but not generated/applied/surfaced
  before it becomes visible current-frame debt.

Retained visible-reservation async pending fill - 2026-06-07:

- Evidence before change:
  `highalt_currentfirst_restored_after_camera_gate_revert_20260607` showed the
  prediction system was finding future visible work (`PREDICTED_VISIBLE`
  queued `512/512` at f399/f400/f401), but the async generation worker only had
  `24` visible jobs in flight at a time (`workerQueueDepth` about `22`,
  `asyncPending=24`). f402/f403 then still arrived with large projected-visible
  misses (`813/853`) even though reservations existed and were healthy.
- Retained code:
  `QueueAsyncVoxelGenerationMatchingPriority(true, ...)` now treats visible
  reservations as a deadline lane. It snapshots the current async pending set,
  skips already-pending coords, keeps the existing current-visible/deadline/
  sample/first-frame ordering, and fills the existing
  `asyncNoncriticalGenerationQueueMax` pending capacity for visible reservation
  work. The normal noncritical lane still uses the per-frame enqueue cap.
  `TryQueueAsyncVoxelGeneration` gained a private `bypassEnqueueLimit` flag so
  only this deadline-fill path can exceed the visible per-frame intake while
  still respecting the configured queue max.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_visible_pending_fill_20260607`
    window raw/body avg `60.21/60.53 ms`, max `68.52/70.58 ms`.
  - Smoke 2: `highalt_currentfirst_visible_pending_fill_rerun_20260607`
    window raw/body avg `60.27/60.54 ms`, max `67.67/69.91 ms`.
  - Accepted baseline comparison:
    `highalt_currentfirst_restored_after_camera_gate_revert_20260607` was
    `63.71/63.83 ms`, max `70.30/70.28 ms`. This retained change repeatedly
    saves about `3.4-3.5 ms` in the high-alt window without reintroducing
    main-thread visible generation.
- Health checks:
  f400 stayed healthy: visible/cache coverage `99/93`, reservation tickets
  `500/0/0/4/0`, main-thread brick gen `0.09-0.18 ms`.
  f402/f403 visible misses improved but did not disappear:
  baseline `813/853`; retained rerun `757/779`. f402/f403 visible coverage
  stayed around `91/91`, and overdue reservations stayed `0`.
- New known pressure:
  the async visible lane now intentionally fills pending capacity
  (`asyncPending=256`). Results can backlog (`asyncResultDepth` about `51-69`
  at f402/f403) while apply remains `24/frame`, and background worker time
  accumulates (`asyncBrickGenMs` about `42-61 ms` on f402/f403 log rows).
  This is acceptable as a retained step because full-window pacing improved and
  reservation health did not regress, but it proves the next visible-lane
  bottleneck is apply/upload/surface throughput and deadline aging, not initial
  reservation discovery.
- Current rough FPS:
  still only about `16.6 FPS` (`~60 ms/frame`), far from 60 FPS. The trajectory
  improved because prediction now feeds a real in-flight deadline queue, but the
  engine still needs major structural work: drain/apply/surface visible results
  without public-frame hitches, remove full voxel-interest rebuilds, and reduce
  GPU ray/post-wait pressure.

Post pending-fill probes and rejections - 2026-06-07:

- Detail probe:
  `highalt_currentfirst_visible_pending_fill_interestdetail_20260607`.
  This run is slower because detail timing is enabled, but it confirms the
  current post-change bottleneck shape:
  - f400 predicted-visible elapsed `10.38 ms`, phase split
    `0.57/8.26/0.40/0.32 ms` for snapshot/rebuild/restore/queue.
  - f400 interest detail: `15.84+ ms` class remains, with `33275`
    candidates, `76363` attempts, `43088` duplicate hits, sort/emit
    `4.75 ms`, anchor `3.00 ms`.
  - f402/f403 interest detail still rebuilds about `41k` candidates from
    `108k` attempts with `66k-67k` duplicate hits.
  - f402/f403 async visible queue is full (`asyncPending=256`), result depth
    can reach `61/140`, and visible misses remain high. The next correct fix
    must either reduce the full interest/prediction rebuild or add explicit
    visible-result lifecycle ownership through apply/upload/surface.
- Rejected visible apply cap diagnostic:
  `highalt_currentfirst_visible_pending_fill_apply256_diag_20260607` used
  `-MidClipmapAsyncVisibleCriticalMaxApply 256`.
  Window raw/body regressed to `73.97/74.11 ms` versus the retained
  `60.27/60.54 ms`. Upload, surface extraction, GPU frame/ray, and post-wait
  all rose. f402/f403 visible misses improved only slightly
  (`743/767`), so raising visible apply is a debt-shift, not a fix. Do not
  promote or code this as an adaptive cap without a real downstream lane budget.
- Rejected pure missing-candidate prediction runtime swap:
  tried replacing the runtime `QueuePredictedVisibleVoxelInterest` stateful
  snapshot/full `UpdateVoxelInterest`/restore path with
  `CollectPredictedVisibleVoxelInterestPureForDebug`, but this time without the
  earlier resident-touch reservation loop. Build and `VENPODTests.exe` passed,
  but smoke `highalt_currentfirst_pure_missing_prediction_runtime_20260607`
  regressed to window raw/body `66.90/67.12 ms`.
  It also reproduced the reservation-health failure:
  f400 reservation tickets `1119/611/611/0/20`, backlog max age `243`,
  cache coverage `89`, and f402/f403 stale backlog max age `245/246`.
- Decision:
  reverted the pure runtime swap after build/test. The retained source remains
  the visible-reservation async pending-fill change only. The pure debug
  collector can match candidate order in an isolated test, but runtime
  prediction is coupled to queued/backlog/reservation lifecycle in ways the
  pure collector does not currently preserve. The next prediction-side fix
  needs explicit lifecycle state, not another collector swap.
- Validation after reverting rejected pure swap:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. No new smoke was needed because the only runtime experiment from this
  slice was reverted back to the previously smoked pending-fill source.

Rejected pure full-stream prediction runtime swap - 2026-06-07:

- Hypothesis:
  the prior pure runtime swap might have failed because it asked
  `CollectPredictedVisibleVoxelInterestPureForDebug` for only `maxCoords`
  upfront, while the stateful runtime path builds the full predicted queue and
  lets the existing admission loop stop after `maxCoords` newly queued coords.
  A narrower experiment used the pure collector with `UINT32_MAX` so it returned
  the full candidate stream, then kept the existing `queuePredictedCoord`
  admission path unchanged.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_pure_fullstream_prediction_runtime_20260607`
    looked locally promising on frame time: window raw/body
    `55.98/56.36 ms`, max `61.53/68.51 ms`. However it already showed a
    severe f400 reservation debt spike: tickets `1206/694/680/9/27` and
    cache coverage `92` versus retained pending-fill `500/0/0/4/0` and `93`.
  - Smoke 2: `highalt_currentfirst_pure_fullstream_prediction_runtime_rerun_20260607`
    rejected the branch outright: window raw/body `96.48/96.01 ms`, max
    `123.11/123.06 ms`, f400 CPU/request/gen/clip `50.34/15.18/10.72/24.42`,
    GPU frame/ray `29.00/23.39`, and reservation tickets still bad at
    `1187/675/661/8/27`.
- Decision:
  reverted after build/test. This confirms the runtime prediction contract is
  not just candidate order or full-stream truncation. The stateful
  `UpdateVoxelInterest` rebuild/restore path is coupled to live queue,
  backlog, and reservation lifecycle in ways that pure collection does not
  preserve under the current async pending-fill scheduler. Do not retry pure
  runtime prediction swaps without first designing explicit predicted/current
  lifecycle state and stale-reservation retirement.
- Validation after revert:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Retained source remains the visible-reservation async pending-fill
  change only; current accepted rough FPS remains about `16.6 FPS`.

Rejected signature-reuse follow-up - 2026-06-07:

- Diagnostic:
  `highalt_currentfirst_pending_fill_sigreuse_diag_20260607` initially looked
  slightly positive at window raw/body `59.60/59.92 ms` versus retained
  pending-fill `60.27/60.54 ms`.
- Confirmation:
  `highalt_currentfirst_pending_fill_sigreuse_rerun_20260607` did not hold.
  Window raw/body regressed to `69.06/69.18 ms`, max `84.30/84.27 ms`.
  f400 CPU/request/gen/clip was `41.66/12.45/9.93/19.27`, with coverage
  still `99/93` and reservations still clean, but the frame-time regression
  rejects it.
- Decision:
  do not promote `voxelInterestSignatureReuse` by default. It is too noisy as
  a policy-level skip and can hide/update-debt timing rather than reducing the
  underlying work reliably.

Accepted dominated predicted-anchor cleanup - 2026-06-07:

- Code:
  `src/Simulation/SparseClipmap.cpp` now skips the predicted voxel-interest
  anchor when it quantizes to the same grid center as the camera anchor and
  has a smaller/equal radius with a worse score. This removes guaranteed
  duplicate candidate insert/probe work in the no-motion predicted-admission
  path. The same guard is mirrored in the pure debug collector to keep the
  comparison path aligned.
- Validation:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke 1: `highalt_currentfirst_skip_dominated_pred_anchor_20260607`
    window raw/body `59.12/59.29 ms`, max `65.28/65.25 ms`.
  - Smoke 2: `highalt_currentfirst_skip_dominated_pred_anchor_rerun_20260607`
    window raw/body `59.61/59.76 ms`, max `66.74/67.75 ms`.
- Health:
  f400 stayed reservation-clean and visually covered: visible/cache coverage
  `99/93`, reservation tickets `500/0/0/4/0`, no overdue reservations, and
  main-thread brick generation remained `0.10 ms`.
- Current rough FPS:
  about `16.8 FPS` from the confirmed `59.61 ms` raw-window average. This is
  still nowhere near 60 FPS, but it is a retained structural cleanup because
  it removes provably dominated work without changing budgets, coverage
  targets, or downstream debt.
- Next structural target:
  the big remaining cost is still full voxel-interest/request rebuilding and
  visible-result lifecycle throughput. Do not spend more time on small default
  flags until the engine has explicit current/predicted interest lifecycle
  state or a real visible apply/upload/surface ownership path.

Rejected follow-up experiments after dominated-anchor cleanup - 2026-06-07:

- Detail diagnostic:
  `highalt_currentfirst_skip_dominated_pred_anchor_interestdetail_20260607`
  ran to completion but the smoke parser failed because the normal
  `PERF frame=400` line was absent; the log still contains `PERF_FRAME_END`
  and mid-clipmap frame 400 data. Use it only as diagnostic evidence, not as a
  window-summary gate.
  Late-frame predicted-visible admission still costs about `8-10 ms` elapsed
  per frame. The phase detail, when enabled, shows the stateful rebuild remains
  the dominant sub-phase (`~6-13 ms` on frames 399-403).
- Rejected predicted-visible one-frame queue cache:
  attempted to cache the temporary predicted voxel generation queue by
  quantized predicted-view signature, with edit-revision invalidation and a
  one-frame age limit. Build and tests passed, but smoke
  `highalt_currentfirst_pred_visible_queue_cache_20260607` regressed hard:
  window raw/body `73.48/73.21 ms`, f400 CPU/request/gen/clip
  `39.27/10.75/9.32/19.19`, and f402/f403 cache coverage fell to `89`.
  The cache was reverted. This confirms the predicted queue is not a pure
  geometric artifact; it is coupled to live queue/reservation/result lifecycle.
- Rejected visible scheduler pending-filter variants:
  1. `highalt_currentfirst_visible_scheduler_pending_filter_20260607` filtered
     already-pending coords before sorting and partial-sorted only the enqueue
     window. It produced `61.26/61.70 ms` with clean health, but changed some
     async enqueue behavior and was not clearly better than the accepted
     baseline.
  2. `highalt_currentfirst_visible_scheduler_pending_filter_fullsort_20260607`
     kept full sorting after pending filtering to preserve fallback traversal,
     but regressed to `67.47/67.64 ms`.
  Both variants were reverted. Do not retry this scheduler micro-optimization
  without adding explicit scheduler timing and proving it is a dominant cost.
- Validation after reverting rejected follow-ups:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Rejected cache/scheduler symbols are absent from source; retained
  source is still the visible-reservation async pending-fill change plus the
  dominated predicted-anchor cleanup.
- Current rough FPS note:
  latest smokes are noisy. The best confirmed retained duplicate-anchor runs
  were `59.12-59.61 ms` (`~16.8 FPS`), while two post-revert health-correct
  confirmation smokes landed around `65.81-67.42 ms` (`~14.8-15.2 FPS`) with
  clean f400 reservations and `99/93` visible/cache coverage. Treat current
  rough FPS as `~15-17 FPS`, not near 60 FPS. The next real fix must change
  the lifecycle architecture, not cache or sort around it.

Rejected pure prediction with explicit resident lifecycle - 2026-06-07:

- Diagnosis:
  the stateful `QueuePredictedVisibleVoxelInterest` rebuild does not only
  produce candidate order. Its temporary `UpdateVoxelInterest` also touches
  resident predicted bricks by updating `lastTouchedFrame`, and that side
  effect survives the live-state restore. The pure debug collector exposes
  resident touch candidates, but prior pure runtime swaps did not preserve this
  lifecycle effect.
- Experiment 1:
  replaced the stateful snapshot/rebuild/restore path with
  `CollectPredictedVisibleVoxelInterestPureForDebug(..., UINT32_MAX,
  &residentTouches)`, then explicitly applied `lastTouchedFrame` to resident
  touch coords before running the existing admission loop.
  Build and tests passed, but smoke
  `highalt_currentfirst_pure_prediction_explicit_resident_touch_20260607`
  failed health despite a superficially okay window (`59.19/59.49 ms`):
  f400 reservation tickets became `1169/657/646/8/27`, f402/f403 cache
  coverage fell to `89`, and f402 async visible work showed
  `256/256/13/243` enqueue/complete/apply/drop. Reverted.
- Experiment 2:
  added a separate total-admission cap so `maxCoords` limited visible
  reservation side effects, not only newly queued bricks. Build and tests
  passed, but smoke
  `highalt_currentfirst_pure_prediction_resident_touch_admitcap_20260607`
  still failed health: f400 reservations `1048/540/538/2/20`, backlog max age
  `242`, f402/f403 coverage `88-90`, and stale backlog max age `244-245`.
  Reverted.
- Decision:
  resident touches are one missing lifecycle side effect, but not the whole
  contract. The stateful prediction rebuild is also implicitly managing stale
  backlog/reservation age and live queued-work overlap in ways the pure stream
  does not preserve. Do not attempt another pure runtime swap until there is an
  explicit predicted/current lifecycle model with bounded reservation age,
  resident touch propagation, stale queued-work retirement, and tests that
  assert those states directly.
- Validation after revert:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Source is back to the accepted stateful prediction path; no
  `predictedResidentTouch` or admission-cap runtime symbols remain.

Rejected stable temporal voxel-interest reuse - 2026-06-07:

- Current retained baseline before this experiment:
  `highalt_currentfirst_current_baseline_20260607_cont` window raw/body
  `59.29/59.40 ms` (`~16.9 FPS`). At f400, CPU/request/gen/clip was
  `33.99/9.58/7.85/16.54`, and `clip` was dominated by voxel interest
  (`interest=14.58`, `pump=0.90`). Coverage health was clean:
  visible/cache `99/93`, reservations `500/0/0/4/0`, backlog max age `27`.
- Experiment:
  added a bounded one-frame temporal reuse path inside `UpdateVoxelInterest`
  that reused the current voxel interest set when prior coverage was healthy
  (`missing * 10 <= interested`), signature drift was small, diagnostics were
  off, and age was within `voxelInterestSignatureReuseMaxAgeFrames`, even when
  explicit `voxelInterestSignatureReuse` was disabled. Added a sparse-core
  test proving small drift reused and a large camera jump rebuilt.
- Result:
  build/tests passed, and f400 `clipMs` improved from `16.54` to `12.46`
  (`interest=10.57`, `voxelReuse=active/age:1/1`). However the smoke regressed:
  `highalt_currentfirst_stable_temporal_reuse_20260607` window raw/body
  `62.50/62.58 ms`, upload rose from `3.69` to `6.25 ms`, surface extraction
  rose from `5.75` to `6.63 ms`, and f400 surface extract queue ballooned
  (`extractQueued=1783` vs baseline `569`). Health was superficially clean
  (`99/93`, reservations `501/0/0/4/0`), but the debt moved downstream.
- Decision:
  reverted. This confirms voxel-interest rebuild is a real structural cost,
  but stale reuse/freezing is not an acceptable fix. The next architecture pass
  should build an explicit incremental interest delta/frontier model that can
  update newly entered/leaving regions without creating downstream
  upload/surface debt. Do not retry blanket temporal reuse unless it also
  accounts for surface/upload publication pressure and proves the queued
  surface set stays bounded.
- Validation after revert:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Search markers `stable voxel reuse`, `voxelInterestStableTemporalReuse`,
  and `absSignatureDelta` are absent.

Rejected voxel-interest candidate de-dup micro-architecture attempts - 2026-06-07:

- Motivation:
  interest-detail logs showed late high-alt rebuilds around `25k` unique
  candidates and `38-40k` attempts, with sort/emit and anchor de-dup work
  contributing several milliseconds. Tried to reduce structural rebuild cost
  without changing coverage or lifecycle semantics.
- Attempt 1, raw candidate merge:
  in normal runtime and pure predicted collection, replaced per-candidate
  unordered-map de-dup with raw candidate collection, coordinate/score sort,
  duplicate merge, then the existing priority partial-sort. Build and tests
  passed, but smoke `highalt_currentfirst_raw_candidate_merge_20260607`
  regressed: window raw/body `60.32/60.67 ms`, f400 `clipMs=20.89`
  (`interest=18.91`) vs retained baseline `16.54` (`interest=14.58`).
  Reverted.
- Attempt 2, narrower per-ring candidate key:
  kept the existing unordered-map algorithm but changed the candidate lookup
  key from full `SparseVoxelClipmapCoord` to a per-ring `(x,y,z)` key to avoid
  hashing ring. Build and tests passed, but smoke
  `highalt_currentfirst_candidate_key_20260607` regressed hard: window raw/body
  `64.19/64.25 ms`, f400 upload/surface debt increased
  (`avgSparseUploadMs=6.41`, `avgSurfaceExtractMs=7.00`), while f400 `clipMs`
  was not better (`16.96`). Reverted.
- Decision:
  do not spend more turns on candidate de-dup representation changes without a
  stronger profiler. The dominant issue is not a local data-structure swap; it
  is the lifecycle/frontier architecture that forces full interest rebuilds and
  couples predicted/current work to upload and surface publication.
- Validation after revert:
  marker search for `VoxelInterestCandidateKey`, `useRawCandidateMerge`, and
  `insertionOrder` returned no source hits. `.\build.ps1 -Config Release`
  passed and `.\build\bin\VENPODTests.exe` passed.

Accepted structural surface extraction batch path - 2026-06-07:

- Problem:
  retained baseline `highalt_currentfirst_current_baseline_20260607_cont`
  was still about `59.29/59.40 ms` raw/body (`~16.9 FPS`). Surface extraction
  was not the only bottleneck, but it was a real main-thread/publication cost:
  f400 pre-publish surface took `5.18 ms`, surface extract `5.21 ms`, and the
  existing parallel surface path was inactive (`surfaceParallel=0`) because the
  high-alt path mostly used serial class/ownership/explicit critical pumps.
- Change:
  added a shared clean no-edit surface batch helper in `SparseVoxelWorld` and
  routed class, ownership, and explicit pre-publish critical coordinate lists
  through it when `VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION=1`,
  `m_edits.EditedBrickCount()==0`, and `m_surfaceDirtyRegions.empty()`. Dirty
  or edited surface extraction still uses the existing serial path. The batch
  helper also marks `kStreamingTicketStageSurfaceReady`, preserving lifecycle
  completion for parallel extraction.
- Validation:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. No-parallel fallback check
  `highalt_currentfirst_surface_batch_no_parallel_check_20260607` remained
  baseline-like: window raw/body `59.19/59.32 ms`, f400
  `surfaceParallel=0`, visible/cache `99/93`.
- Parallel-path smoke:
  `highalt_currentfirst_surface_batch_explicit_critical_20260607` completed
  cleanly but the smoke parser failed because the dense `PERF_SPARSE frame=400`
  line was not emitted after surface/publish debt drained. Manual frame-end
  window over frames `376-403`: raw/body `50.47/50.70 ms`, max raw/body
  `56.86/59.69`, about `19.7-19.8 FPS`. Health markers stayed comparable:
  f400 visible/cache coverage `99/93`; f402/f403 visible/cache coverage
  `91/90` and `91/90`, matching the existing public-frame transition pattern.
  Surface/upload/publish debt improved sharply: f400 streaming tickets had
  `pending=cpu/upload/surface/publish:8568/0/0/0` versus baseline
  `5607/1953/562/1954`.
- Decision:
  keep the patch. It is not close to 60 FPS, but it is a real structural win:
  it removes a serial publication bottleneck and drains downstream surface debt
  instead of hiding it. Next target is not more surface knobs; remaining frame
  time is dominated by CPU request/generation/clip and render/GPU wait. The
  likely next architecture pass is incremental voxel-interest/frontier
  rebuilding plus decoupled prediction/current lifecycle, not candidate
  de-dup swaps or blanket temporal reuse.

Accepted passive gameplay-query gate for high-alt sparse runtime - 2026-06-07:

- Problem:
  after surface batching, the best retained current stack was still about
  `50.47/50.70 ms` raw/body (`~19.8 FPS`). A predicted-visible disabled A/B
  (`highalt_currentfirst_surface_batch_no_predicted_visible_20260607`) did not
  materially improve the window: `51.28/51.44 ms`, with f400 visible/cache
  `99/78`. That falsified predicted-visible as the sole frame-window blocker.
- Measurement:
  added temporary `PERF_PRE_PHYS_BREAKDOWN` instrumentation around the
  pre-physics region. Smoke `highalt_currentfirst_prephys_breakdown_20260607`
  showed `prePhys` around `9.55 ms` average; f400 was `10.21 ms`, dominated by
  synchronous sparse gameplay queries: `groundSupport=7.83 ms` and
  `brushFallback=2.33 ms`. These were paid every frame during the high-altitude
  stress camera even though walking collision and passive brush preview were not
  render-critical. The temporary instrumentation was removed after the gate was
  validated; use the capture logs above as the retained measurement record.
- Change:
  in `main_launcher.cpp`, gate ground query dispatch and CPU
  `FindCollisionSupportBelow` by `gameplayInputEnabled && !flightMode`, and
  force stale ground results invalid when sparse ground is authoritative but no
  ground query is needed. Also gate sparse CPU brush raycast fallback so it runs
  only while brush paint/erase input is active; passive preview keeps the stable
  aim/default distance instead of doing a synchronous sparse raycast each frame.
- Validation:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. The first query gate killed `groundSupport` but left passive
  `brushFallback` because the current stack does not own GPU brush raycast.
  The final smoke
  `highalt_currentfirst_query_gate_no_passive_brush_20260607` completed
  normally; the parser again missed dense `PERF_SPARSE frame=400`, so the
  window was parsed manually from `PERF_FRAME_END` frames `376-403`:
  raw/body `47.73/47.93 ms`, max raw/body `54.02/55.37`, about `20.9 FPS`.
  `prePhys` fell to `0.07 ms` average, with f400
  `groundSupport=0.00` and `brushFallback=0.00`. Public-frame health markers
  remained in-family: f400 visible/cache `99/93`, f402/f403 `91/90`.
  After removing the temporary instrumentation, a clean retry
  `highalt_currentfirst_query_gate_final_retry_20260607` also passed the parser
  and confirmed the structural part of the fix (`avgPrePhysMs=0.07`), but the
  total window was noisy/worse: raw/body `55.66/55.79 ms`, with `postWait`
  averaging `15.22 ms`, surface extract `5.61 ms`, and f400 `clipMs=18.43`.
  Treat the current rough FPS as a range around `18-21 FPS`, not as a stable
  21 FPS win. The query gate removed real synchronous work; it did not make the
  engine globally faster in every run because downstream sparse post/surface and
  interest costs still dominate.
- Decision:
  keep the gate. It is a real structural fix: render-only/stress frames no
  longer pay synchronous gameplay collision/preview queries. It still does not
  put the engine near 60 FPS. The next measured bottlenecks are `postWait`
  sparse post work around `10 ms`, predicted/current interest rebuild around
  `8-16 ms` depending on frame, surface extraction around `4-5 ms`, and GPU
  ray/surface work. The trajectory is upward but not sufficient; the next
  architecture target should be decoupling incremental interest/frontier and
  reducing per-frame sparse post publication/surface work, not more query
  gating.

Rejected predicted-visible pressure/cadence gates - 2026-06-07:

- Motivation:
  final retry still showed `clipMs=18.43` at f400 and predicted visible
  admission logging around `8-9 ms` on f399-f403. A no-predicted A/B improved
  clip cost but damaged transition coverage (`f402/f403` fell to about `83/82`
  cache coverage vs the predicted path staying around `89-90`). The safe target
  was therefore not removal, but avoiding redundant rebuilds when prediction
  was already carrying enough visible reservations.
- Attempt 1, reservation pressure gate:
  added a pre-admission skip when current visible misses were tiny and active
  reservations/visible queue looked saturated. Build passed, but smoke
  `highalt_currentfirst_predicted_pressure_gate_20260607` showed no
  `skipped=pressure` lines. The pre-admission stats do not expose the saturation
  visible in the post-admission logs, so the patch was a no-op. Removed.
- Attempt 2, looser reservation gate:
  removed the queued-visible requirement and kept visible-missing/reservation
  pressure. Build passed, but
  `highalt_currentfirst_predicted_pressure_gate_v2_20260607` still produced no
  skip lines. Removed.
- Attempt 3, cadence after strong admission:
  tracked the last frame that queued at least half of max predicted coords and
  attempted to skip the immediate following frames while current visible misses
  were low. Build passed, but
  `highalt_currentfirst_predicted_cadence_gate_20260607` still produced no
  `skipped=cadence` lines because the pre-admission current stats still showed
  enough debt that prediction was doing real work before the visible prepump log
  reported health. Removed.
- Decision:
  do not retry simple predicted-visible skip gates from `main_launcher.cpp`.
  They either do not fire or risk the known f402/f403 coverage drop. The real
  fix must be inside the clipmap interest architecture: reuse/share the
  expensive current/predicted candidate frontier, or make predicted admission
  consume an incremental projected frontier instead of running another full
  `UpdateVoxelInterest` rebuild.

Accepted surface parallel stats accumulation - 2026-06-07:

- Problem:
  after the surface batch extraction path was accepted, `surfaceParallel`
  counters could be misleading because `ExtractSurfaceBatchNoEdit` overwrote
  `m_parallelSurfaceExtractionBricksLastFrame`,
  `m_parallelSurfaceExtractionWorkersLastFrame`, and
  `m_parallelSurfaceExtractionWallMsLastFrame` on every batch. A frame can run
  multiple surface batches from pre-publish and general/ownership paths, so the
  final tiny batch hid how much work actually ran through the parallel path.
- Change:
  in `SparseVoxelWorld::ExtractSurfaceBatchNoEdit`, accumulate
  `parallelSurfaceExtractionBricksLastFrame` and wall time across all batches in
  the frame, and keep the maximum worker count observed. The per-frame reset in
  `BeginFrame` remains the boundary. This is an observability/measurement fix,
  not a behavior or FPS optimization.
- Validation:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Smoke `highalt_currentfirst_surface_stats_accum_20260607` exited
  normally but again missed dense `PERF_SPARSE frame=400`; manual
  `PERF_FRAME_END` window over frames `376-403` was raw/body
  `41.76/42.08 ms`, max raw/body `48.63/53.02`, and `prePhys=0.06 ms`.
  Do not treat that window as a stable FPS claim because recent runs are noisy.
  The important validation is that CPU detail lines now show larger accumulated
  `surfaceParallel=active/bricks/workers/wallMs` values when multiple batches
  run in a frame, e.g. early catchup frames reporting `20`, `31`, `49` bricks
  instead of only the last batch's small count.

Rejected isolated incremental trim on current async-lane stack - 2026-06-07:

- Motivation:
  current accepted async-lane high-alt repeat still scans two full trim passes
  per frame once the resident pool is large:
  `trimScan=calls/records/candidates/evicted:2/131072/16930/8` at f400. This
  looked like a clean structural target because the pool is scanning 65,536
  records twice to evict only 8 bricks.
- Accepted comparison baseline:
  `build/captures/highalt_async_lane_promote_repeat_20260607`, invoked as
  `.\perf_noncapture_smoke.ps1 -Config Release -Scenario highalt -StackPreset highalt-currentfirst -OutputDir build\captures\highalt_async_lane_promote_repeat_20260607 -NoBuild -ParallelSurfaceExtraction -ParallelSurfaceExtractionTimeBudgeted -ParallelSurfaceExtractionMaxWorkers 4`.
  Window raw/body avg `33.73/33.88 ms`, max `41.21/41.18 ms`. f400 raw/body
  `39.95/34.21 ms`, CPU/request/gen/clip `25.25/7.62/6.02/11.59`, postWait
  `9.38 ms`, GPU/ray `10.77/9.23`. f400 visible-critical coverage `99`,
  f402/f403 coverage `100/100`.
- Rejected probe:
  `highalt_currentfirst_incrementaltrim2048_isolated_20260607` used the generic
  `highalt-currentfirst` preset plus
  `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`. It cut
  trim scans to `4096`, but window raw/body regressed to `47.12/47.82 ms` and
  f402/f403 visible-critical coverage dropped to `89/90`.
- Rejected apples-to-apples probe:
  `highalt_async_lane_promote_parallel_surface_incrementaltrim2048_20260607`
  repeated the same trim setting against the accepted parallel-surface stack.
  It also cut trim scans to `4096`, but window raw/body regressed to
  `41.16/41.93 ms` and f402/f403 visible-critical coverage again dropped to
  `89/90`.
- Decision:
  do not enable the existing incremental trim path on this stack. The full scan
  is wasteful, but it currently preserves enough residency shape to avoid
  f402/f403 visible-critical debt. Bounded scan saves CPU locally and creates
  later clip/pump debt. A future trim fix needs a maintained eviction-candidate
  index or lifecycle-owned active/background lists that protect near-future
  visible reservations, not blind cursor sampling over the resident pool.
- Current accepted rough high-alt FPS:
  about `29-30 FPS` average, not the rejected `14-16 FPS` states. Still far
  from 60 FPS.

Rejected footprint-over-terrain candidate dominance skip - 2026-06-07:

- Measurement:
  detail smoke `highalt_async_lane_promote_interestdetail_current_20260607`
  on the current accepted stack showed f400 voxel-interest detail
  `candidates=33275`, `candidateAttempts=72424`, `duplicateHits=39149`.
  Source split: terrain `52003/25312/7874` attempts/duplicates/scoreUpdates,
  footprint `11037/10487/3774`, camera `3885/1334/297`. Predicted-visible
  rebuild was still about `6-8 ms` per frame.
- Experiment:
  in both the runtime interest builder and the pure/debug collector, skipped a
  footprint-band candidate when it overlapped the terrain vertical band and its
  vertical score could not beat the already-emitted terrain candidate for the
  same x/z/y. This looked source-safe because `addCandidate` only updates on a
  strictly lower score.
- Validation before rejection:
  - Build: `.\build.ps1 -Config Release`
  - Tests: `.\build\bin\VENPODTests.exe`
  - Smoke: `highalt_async_lane_promote_footprint_dominance_20260607`
    window raw/body `38.58/39.54 ms`, max `97.84/97.81 ms`, worse than
    accepted `33.73/33.88 ms`.
  - f400/f401/f402 visible-critical coverage stayed `100`, but f403 dropped to
    `90` with `missingVisibleCritical=830`.
- Decision:
  reverted. Even apparently dominated duplicate candidates can alter emitted
  ordering/reservation timing enough to break the future visible-critical
  transition. Do not retry footprint duplicate pruning as a local score check.
  A viable interest fix must preserve the full candidate stream contract or
  introduce an explicit frontier/lifecycle model with tests for f402/f403-style
  reservation timing.
- Revert verification:
  marker search for `footprintVerticalScore` and `terrainVerticalScore` is
  clean. `.\build.ps1 -Config Release` and `.\build\bin\VENPODTests.exe`
  passed after revert.

Current performance interpretation and next architecture target - 2026-06-07:

- Important correction:
  the `14-16 FPS` states were rejected/regressed branches, not the accepted
  stack. The accepted apples-to-apples stack remains
  `highalt_async_lane_promote_repeat_20260607`, window raw/body average
  `33.73/33.88 ms` (`~29-30 FPS`). This is still not near 60 FPS, but it is
  not the failed 14 FPS line.
- Accepted f400 cost shape:
  CPU sparse prep is still the first wall: `25.25 ms` split as
  request/generation/clip `7.62/6.02/11.59`. GPU frame/ray is
  `10.77/9.23 ms`. `PERF_FRAME_END postWait=9.38 ms` is not a GPU fence wait
  (`PERF wait=0.00`); it is sparse post/command-begin work after the frame
  context wait, including surface extraction `4.13 ms`, upload `1.40 ms`, and
  surface stage `1.27 ms`.
- Background split/mask finding:
  the current split renders the low-resolution raymarch into its own background
  DSV, so it cannot use the full-resolution sparse-surface stencil to reject
  covered pixels during the expensive ray pass. The later composite is stencil
  gated, but by then the ray work is already done. A useful fix needs a
  conservative downsampled coverage mask, not a naive second low-res surface
  raster pass; marking a low-res texel covered when only one full-res subpixel
  is covered would create visible unshaded background holes.
- Surface parallel A/B:
  `highalt_async_lane_promote_no_parallel_surface_ab_20260607` removed the
  accepted `-ParallelSurfaceExtraction` flags. It regressed to f400 raw/body
  `46.26/45.99 ms`, f400 CPU prep `29.02 ms`, surface extract `5.22 ms`, and
  postWait `13.87 ms`. Therefore do not disable current per-batch threading.
- Decision:
  the trajectory is only good if the next changes remove structural main-frame
  work. Do not keep turning budgets. The next two defensible architecture
  candidates are:
  1. a persistent/asynchronous surface extraction pipeline that publishes
     completed surfaces without per-frame thread spawn/join and without blocking
     public-critical coverage; and
  2. an explicit clipmap interest frontier/lifecycle model that reuses the
     current/predicted candidate stream while preserving f402/f403
     visible-critical reservation timing.
  GPU background masking remains valuable, but CPU prep/post must be cut first
  because f400 is CPU-bound before it is GPU-bound.

Rejected persistent surface extraction worker pool - 2026-06-07:

- Motivation:
  accepted f400 `PERF_FRAME_END postWait=9.38 ms` included surface extraction
  `4.13 ms`, and `SparseVoxelWorld::ExtractSurfaceBatchNoEdit` created and
  joined fresh `std::thread`s for each surface batch. This looked like a
  structural hot-path fix: keep the batch semantics but reuse worker threads and
  per-worker terrain-column caches.
- Experiment:
  added a persistent surface worker pool to `SparseVoxelWorld`, mirroring the
  existing persistent exact-generation worker design. The batch still waited
  for all results before updating `SparseSurfaceCache`; no surface publication
  or visual correctness contract was intentionally changed.
- Validation:
  `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe`
  passed. Smoke
  `highalt_persistent_surface_workers_20260607` used the accepted stack:
  `.\perf_noncapture_smoke.ps1 -Config Release -Scenario highalt -StackPreset highalt-currentfirst -OutputDir build\captures\highalt_persistent_surface_workers_20260607 -NoBuild -ParallelSurfaceExtraction -ParallelSurfaceExtractionTimeBudgeted -ParallelSurfaceExtractionMaxWorkers 4`.
- Result:
  rejected. The surface slice improved, but total frame time regressed:
  window raw/body `38.74/38.79 ms` versus accepted `33.73/33.88 ms`.
  f400 raw/body `43.32/46.42 ms` versus accepted `39.95/34.21 ms`.
  f400 surface extract improved `4.13 -> 3.43 ms`, but CPU sparse prep
  worsened `25.25 -> 34.05 ms`, especially clip/interest
  `11.59 -> 16.28 ms`. This likely increased scheduling/cache contention or
  changed CPU residency enough that the saved surface thread lifecycle cost was
  not a frame win.
- Decision:
  reverted completely. Marker search for `PersistentSurfaceExtraction`,
  `m_persistentSurface`, and
  `ExtractSurfaceBatchNoEditWithPersistentWorkers` is clean. Rebuild and tests
  passed after revert. Do not retry "persistent workers but still synchronous
  batch join" as the surface solution. A useful surface architecture change
  needs to decouple extraction completion from the main-frame post path or
  reduce demanded surface work, not just reuse worker threads inside the same
  synchronous batch.

Rejected existing voxel-interest signature reuse promotion - 2026-06-07:

- Motivation:
  accepted f400 still pays full mid-voxel interest rebuild:
  `clip=interest/reuse/pump:9.32/0/0.97`, with
  `centerDelta=1/0/1 fullRebuild=1`. The code already has
  `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE`, so test the
  existing reuse contract before writing new frontier code.
- Probe:
  `highalt_existing_voxel_interest_reuse_20260607` invoked the accepted stack
  plus `-MidClipmapVoxelInterestSignatureReuse
  -MidClipmapVoxelInterestSignatureReuseMaxAge 4`.
- Result:
  rejected as a baseline promotion. The window regressed to raw/body
  `35.45/35.67 ms` versus accepted `33.73/33.88 ms`. f400 raw/body was
  `40.95/36.00 ms`, and f400 still rebuilt voxel interest:
  `clip=interest/reuse/pump:9.79/0/0.94`. The flag did reuse on some earlier
  frames, but it did not remove the f400 rebuild and increased total window
  time.
- Decision:
  do not simply turn on the existing signature reuse flag. A real clipmap fix
  needs a motion-aware incremental/frontier data model that can preserve the
  visible-critical reservation timing across f402/f403, not a coarse reuse gate
  that misses the expensive moving-camera rebuilds.
