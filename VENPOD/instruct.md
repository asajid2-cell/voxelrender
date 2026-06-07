# VENPOD Long-Run Agent Instructions

This file is for future Codex/agent sessions after compaction, interruption, or long-running work. Read it before coding. It captures the intention behind the current VENPOD campaign, the failure modes of prior work, and the operating rules needed to keep progress aligned.

## North Star

VENPOD should become a real-time infinite voxel terrain renderer where every visible pixel has a legitimate, explainable owner under a frame budget.

The intended engine identity is not just "sparse voxels" or "chunk streaming." The novel direction is proof-carrying voxel streaming:

- exact sparse surface, mid voxel, Far-SVO, deterministic water, and sky are all possible owners;
- each owner must be allowed by deterministic terrain/water truth or an explicit fallback proof;
- streaming work should be scheduled by ownership criticality, not by generic queue order;
- performance wins are only valid if they preserve visual ownership correctness.

Target: stable playable motion trending toward 60 FPS, with representative noncapture frame times at or below `16.67 ms` and clean visual/ownership validation.

## Current Reality

The engine is not currently 60 FPS in gameplay.

Latest rough state from the June 2026 handoff:

- fixed/static view: roughly `20-24 ms` raw, about `42-50 FPS`;
- current comparable public walking/realtime row with moving-window proof/reservations and verified parallel mid pump:
  f480 raw/body `83.83/78.30 ms`, local f456..484 avg/max raw `76.35/86.48 ms`,
  roughly `12-13 FPS`;
- latest bounded repair-lane public walking row:
  f480 raw/body `80.91/73.92 ms`, local f456..484 avg/max raw `78.86/90.74 ms`,
  roughly `11-13 FPS`; this validates bounded hidden-repair admission but is not a 60 FPS candidate;
- latest bounded repair plus mid-interest interval-2 row:
  f480 body `72.72 ms`, local f456..484 avg/max raw `69.60/93.86 ms`,
  local avg/max body `76.06/98.89 ms`, roughly `13-14 FPS`; this validates
  one-frame mid-clipmap interest reuse as a useful default-off stack setting,
  while interval 4 is rejected;
- latest bounded repair plus footprint interest signature row:
  f480 body `66.79 ms`, local f456..484 avg/max raw `66.07/83.52 ms`,
  local avg/max body `70.15/86.99 ms`, roughly `14-15 FPS`; this supersedes
  interval-2 as the strongest current public-open walk stack setting, while
  footprint plus interval-2 is mixed and not carried by default;
- latest footprint plus post-open-only async visible split pump row:
  f480 body `56.29 ms`, local f456..484 avg/max raw `53.17/70.27 ms`,
  local avg/max body `56.58/74.40 ms`, roughly `18-19 FPS`; this supersedes
  footprint-only as the strongest current default-off public-open walk stack,
  while split visible budget `0` before public open is rejected because it can
  starve the startup public proof;
- latest post-open async visible split plus generic incremental resident trim row:
  f480 raw/body `24.76/27.11 ms`, local f456..484 avg/max raw
  `27.37/39.41 ms`, local avg/max body `29.72/43.20 ms`, roughly
  `34-37 FPS`; this supersedes the async split-only row as the strongest
  current default-off public-open walk stack, using
  `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`;
- unbounded post-open repair lane is rejected:
  f480 raw/body `128.03/125.75 ms`, local f456..484 avg/max raw `123.65/150.32 ms`,
  and it floods downstream visible-class queues;
- a private/startup-held f480 row at `27.68 ms` was invalid for public-frame comparison because the public render gate was held, ownership and surface raster were inactive, and `gpuRay=0`;
- high-altitude view: roughly `48-52 ms`, about `19-21 FPS`;
- latest predicted-visible high-alt branch showed the future-visible predictor
  mostly overlaps the later visible-critical misses, but deadline/current-first
  scheduling is still too slow: retained default-off current-first ticket probe
  improved f402/f403 coverage to `88/88` but measured `74.55/78.40 ms` window
  raw avg/max; a later reservation-apply throttle slice added default-off
  counters/knobs but did not yet produce a valid performance win;
- newer high-alt follow-up added `-StackPreset highalt-currentfirst` and fixed
  async-visible apply safety so completed visible async results can publish on
  budget-disabled prepump frames. This is architecturally retained, but cap
  probes are still not candidates: uncapped apply-safe is about `144 ms` window
  raw average in the preset, cap 8 exercises deferral but stays about `145 ms`
  and loses f402/f403 coverage, and cap 16 regresses to about `207 ms`;
- latest high-alt structural pass identified mid-voxel interest candidate
  generation as a core CPU bottleneck. A retained camera-band overlap skip
  removes guaranteed-losing low-priority camera-height candidates already
  covered by terrain/footprint bands. Validated artifact:
  `highalt_currentfirst_camera_band_overlap_skip_nodetail_20260606`, window
  f376..404 raw/body avg `71.33/73.17 ms`, frame 400 raw/body
  `69.74/71.24 ms`, clip/interest `20.62/18.87 ms`, visible-critical coverage
  `100%`. This is only about `14 FPS`, not close to 60, but it is a real
  source-level reduction from the clean high-alt baseline
  `78.73/80.20 ms` window raw/body avg and frame-400 clip/interest
  `29.18/26.07 ms`;
- visual walk smoke is cleaner than before, with no obvious white terrain holes or sky leaks in the latest checked contact sheet, but terrain remains coarse/blocky and high-alt artifacts remain.

Do not claim success from `miss=0`, `unsafe=0`, a passing smoke test, or one clean screenshot. Those have repeatedly hidden remaining frame-time, ownership, shoreline, or LOD problems.

## Spirit Of The Goal

The user wants the engine to move toward a serious, novel voxel renderer, not a pile of fragile patches.

Keep the work aimed at:

1. preserving public-frame ownership correctness;
2. making walking and high-alt views stable under motion;
3. reducing frame time without hiding or deferring unknown visible terrain;
4. building architecture that explains why a fallback is valid;
5. leaving durable handoff evidence after every meaningful branch.

The right direction is an ownership-aware streaming state machine. The wrong direction is another isolated scalar cap, thread-count tweak, broad deferral, or visual mask that merely moves cost or hides missing ownership.

Recent process lesson: every smoke artifact should have a manifest or exact
command. A June 6 reservation-apply validation was slowed by reconstructing an
old high-alt command from memory; the harness now writes `run_manifest.txt`, and
future comparisons should use that instead of inferred flags.
Use `perf_noncapture_smoke.ps1 -StackPreset highalt-currentfirst` for the
current high-alt async-visible branch; do not reconstruct that stack from memory
or ambient `VENPOD_*` variables.

## Required Resume Protocol

At the start of a resumed session:

1. Read this file.
2. Read `active-goal-handoff.md`.
3. Read the top of `handoff.md`, `debug-handoff.md`, and `root.md` only as needed for current state.
4. Inspect the latest campaign artifact named in those files.
5. Check `git status --short` and treat the dirty worktree as user/previous-agent work unless proven otherwise.
6. Do not restart from old assumptions or old chat memory. Trust current files and artifacts.

If the active goal has disappeared after compaction, recreate it with this objective:

```text
Drive VENPOD toward stable 60 FPS by identifying and patching structural engine bottlenecks at their source: preserve public-frame correctness and visual coverage, measure the dominant cost stage before each change, implement core dataflow/architecture fixes rather than tuning knobs, and reject changes that merely shift debt or trade FPS for instability.
```

## Measurement Foundation (READ BEFORE ANY PERF CLAIM)

The campaign stalled mostly because the benchmark itself was untrustworthy. Two
findings (2026-06-07):

- The walk scenario ran with VARIABLE dt: `perf_noncapture_smoke.ps1`'s
  `-WalkFixedDtMs` set `VENPOD_SPARSE_WALK_TEST_FIXED_DT`, but the launcher reads
  `VENPOD_SPARSE_WALK_TEST_FIXED_DT_MS`. The knob was a no-op, so the camera path
  depended on frame timing (perf<->input feedback) and runs were not reproducible.
- vsync on vs off is within noise at high-alt (~34.6 ms either way); the frame is
  genuinely CPU-bound. Benchmark with vsync off anyway to remove the variable.

Rules:

1. Use `.\walk_bench.ps1 -Runs 5 -NoBuild` for walking perf work. It forces the
   correct fixed-dt env var + vsync off, runs N times, and reports
   median/mean/p99/max plus a per-stage bottleneck table, the worst hitch frames,
   and the NOISE BAND (spread of per-run medians).
2. Accept/reject only if median AND p99 move beyond the noise band. Never decide
   on one run or one target frame; the old single-run, single-frame deltas were
   mostly noise.
3. Walking is the north-star scenario; high-alt is the worst case, not the goal.
4. Current measured CPU budget is owned by two stages: mid-clipmap voxel interest
   (`clipMs`, ~16 ms, run 2-3x/frame via the predicted snapshot/restore path) and
   synchronous surface extraction inside the `postWait` gap (~8 ms). Those two are
   the path to 60 FPS; attack them at the source (pure interest builder; async or
   budgeted surface extraction), not with caps.

## Structural Bottleneck Protocol

Do not run a blind knob search.

Every performance branch must start from a measured bottleneck table for the
current baseline/window. The table must identify which stage dominates and
whether the cost is CPU request planning, mid-clipmap interest construction,
generation, async apply, upload staging, surface extraction, page publish, GPU
ray/surface work, post-wait, or frame gaps.

A change is allowed only if it attacks that measured stage at the source. Good
branches change dataflow, lifecycle ownership, incremental update strategy,
worker staging, or render ownership contracts. Bad branches simply cap, defer,
hide, or starve work and hope FPS improves.

Reject a branch immediately if it:

- improves frame time by reducing visible/cache coverage;
- increases public-frame unknown/invalid fallback debt;
- shifts work into upload/surface/publish/post-wait without reducing total
  window cost;
- only works in one target frame while worsening max hitch;
- adds another scalar cap without a lifecycle reason.

For the current high-alt work, the rejected lesson is explicit: pruning the
mid-clipmap cache queue can reduce average milliseconds, but it destroys
mid-cache coverage and creates unstable visuals. Do not revive that approach.
The next valid high-alt branch must explain and reduce the dominant measured
stage, likely mid-clipmap interest rebuild/churn or downstream lifecycle debt,
without starving coverage.

## Completion Criteria

Only stop as complete when one of these is true:

1. Validated 60 FPS candidate:
   - representative noncapture fixed and walking rows are `<= 16.67 ms`;
   - visual/contact-sheet validation is clean;
   - ownership/miss/unsafe/fallback checks are clean.
2. Strong non-60 candidate:
   - strongest default-off stack is validated;
   - exact remaining bottleneck table is written;
   - no local safe next branch remains.
3. Architecture blocker:
   - at least two plausible local branches have been attempted or ruled out by code evidence;
   - the required ownership/state-machine refactor is specified precisely enough to implement.
4. Tool/build blocker:
   - build/test/shader/runtime tooling blocks further safe validation after attempted fixes.

Do not stop because:

- a diagnostic was added;
- one prototype failed;
- one metric improved;
- a contact sheet looked better;
- `miss=0` or `unsafe=0`;
- frame time improved in fixed view only;
- capture-mode performance looked good but noncapture did not.

## Architectural Direction

ACTIVE PLAN (2026-06-07): see `frontier-streaming-design.md`. Local/flag
optimization is proven exhausted (4 bench rejections). The committed direction is a
quantized, incrementally-maintained working set: re-derive the set only on a
discrete recenter (frontier delta) instead of every frame from continuous camera
state, plus a persistent generation/mesh cache. Roll out flag-gated and
bench-validated; Step 1 (gate per-frame request/interest/view-cone rebuild to
recenter frames only) is the make-or-break thesis test. The older ownership-ticket
notes below remain valid background but the frontier plan is the concrete next work.



Implement a single concept that survives through the whole terrain lifecycle: an ownership ticket.

Each request/page/brick should eventually carry:

- ownership class: `publicCritical`, `sampledVisible`, `hiddenRepair`, `cache`, `prefetch`, `maintenance`;
- source lane: camera visible, forward cone, high altitude, shoreline, edit-near, background cache, etc.;
- fallback proof: none, exact required, mid valid, Far-SVO valid, water valid, sky valid, lower LOD valid, unknown;
- required stages: requested, generated, uploaded/applied, surfaced, published;
- completed stages;
- request/sample frames and backlog age;
- screen/error importance;
- safe-to-async and safe-to-defer decisions.

The same ticket, or equivalent state, must influence:

```text
request planning -> generation -> upload/apply -> surface extraction -> publish readiness -> render ownership
```

This is the central missing slice. Prior local optimizations failed because generation, upload, surface extraction, and publication were optimized independently while ownership criticality was lost or collapsed into broad residency classes.

## Implementation Order

Prefer this order unless current evidence clearly contradicts it:

1. Instrument ticket/lane state without behavior change.
2. Split metrics by ownership class and lifecycle stage.
3. Give public-critical and sampled-visible work a true same-frame path through all required stages.
4. Split cache/prefetch/hidden repair budgets so they cannot steal public-critical surface or publish budget.
5. Add fallback proof feedback for lower LOD, Far-SVO, water, and sky ownership.
6. Only then make async/deferred generation broader.
7. Separately reduce GPU cost with an ownership-preserving far/background pass split, tiled reject, or lower-res background traversal.

Keep risky behavior behind default-off flags until validated. Do not promote defaults without equivalence evidence.

## Decision Rules

When choosing a next branch:

- pick the largest measured blocker from latest noncapture/window artifacts;
- prefer behavior-preserving accounting or default-off architecture slices;
- reject branches that reduce CPU by increasing visible unknown debt, fallback invalidity, shoreline artifacts, or frame gaps;
- if a branch merely moves cost between generation, clip/pump, surface, upload, GPU, or post-wait, remove it or mark it diagnostic-only;
- update handoff files before stopping.

Important prior rejections:

- repeated scalar caps and queue trims;
- broad visible deferral without fallback proof;
- global surface extraction thread/backend tweaks;
- async work for sampled fallback-unknown/invalid visible debt;
- changes that make fixed view better while walking/high-alt regress;
- using visual masking, atmosphere, fake heightfield, fake sand, or fake water to cover ownership holes.
- `2026-06-06`: static noncritical upload budget `0`, adaptive noncritical
  floors, and visible async caps `64/64` are rejected for the current strongest
  stack. They either regress broad scenario windows, accumulate hidden debt, or
  fail to fix the high-alt visible-critical coverage drop.

Latest broad reality:

- Strongest retained stack still includes incremental resident trim with
  `-IncrementalPressureTrim -IncrementalPressureTrimScanBudget 2048`.
- Short public walking f456..484 can reach roughly `27-30 ms`, but broad
  validation is lower quality:
  fixed `~35 ms`, walking `~35-38 ms`, high-alt `~54 ms`.
- Current rough broader FPS:
  fixed `28-29 FPS`, walking `26-29 FPS`, high-alt `18-19 FPS`.
- Public ownership remains clean in these rows (`miss=0`, `unsafeNearMiss=0`),
  but clean ownership does not mean motion-stable 60 FPS.
- High-alt visible-critical coverage falls from about `99%` at f400/f401 to
  `86/84%` at f402/f403 in the baseline strong stack. Treat this as the next
  measured motion-stability blocker.
- New evidence: moving-window reservation over the current interest set,
  stress-camera clipmap velocity, and `MID_VOXEL_INTEREST_PCT=100` all failed
  to fix this. The f402 line has a `newV/goneV` burst around `3080/3080`, so
  the next architecture slice must admit/protect predicted future visible
  coordinates before they enter current interest, without widening the entire
  high-alt interest set.
- Newer evidence: broad predicted-visible admission is also rejected as a
  candidate. Sparse ray admission queued too little; full future-interest
  projection queued hundreds of coords but either polluted current coverage
  when inserted into `m_voxelInterestSet` or regressed frame time badly when
  kept as reservations. Future visible work must be a deadline/ownership ticket
  separate from current interest, and it must target the subset that will become
  sampled-visible/visible-critical instead of flooding broad predicted interest.
- Latest evidence: the prediction overlap is already strong. In the reservation
  hit diagnostic, f402 had `1027/1125` visible-critical misses already reserved
  and f403 had `1115/1233`; deadline-aware current-first scheduling improved the
  collapse to `88/88` but still regressed the high-alt window to about
  `74.55 ms` raw average. The next branch should not chase prediction accuracy
  first. It should stage deadline-critical ticket promotion through generation,
  apply/upload, and surface extraction without letting future tickets steal
  current public-critical work or create upload/post-wait spikes.
- Latest ownership-stage result: `VENPOD_SPARSE_OWNERSHIP_STAGE_MID_VISIBLE_DEBT_THROTTLE`
  is retained default-off. A `2/2` floor reduced high-alt max raw/body from
  about `196/196 ms` to `187/187 ms` and cut post-wait/upload pressure, but the
  window remained about `140 ms`. This confirms prefetch downstream debt is real
  competition, but the main blocker remains mid-clipmap interest rebuild /
  visible-set churn, not another scalar budget cap.
- Latest voxel-interest result:
  `VENPOD_SPARSE_MID_CLIPMAP_VOXEL_INTEREST_SIGNATURE_REUSE` is retained
  default-off. Age `1` plus mid-visible-debt `2/2` gave the best narrow
  high-alt shape (`134.70/135.77 ms` window raw/body, max `180.30/180.25 ms`),
  but it is still far from 60 FPS and not broad-validated. Age `2` is rejected
  because stale reuse caused a `217 ms` max hitch. The next architecture work
  should make rebuilds incremental by ring/anchor or carry deadline tickets
  through apply/upload/surface; do not just increase reuse age.
- 2026-06-06 structural reset finding:
  `highalt_currentfirst_structural_baseline_20260606` is the clean measured
  baseline after cap cleanup: window raw/body avg `78.73/80.20 ms`, target
  frame sparse split `request/gen/clip = 12.87/8.10/29.18 ms`, and clip
  interest `26.07 ms`. Rejected and removed in this pass: exact ring-plan
  reuse (`350.90/359.92 ms` window, zero hits), local terrain-height cache
  (`125.95/125.20 ms` window, little interest gain), and bounded prediction
  sampler (`167.24/171.88 ms` window, backlog age `231`, cache coverage `81%`).
  Detail run `highalt_currentfirst_interestdetail_20260606` shows the real
  interior split: avg anchor `14.45 ms`, sort/emit `12.90 ms`, line `1.43 ms`,
  with emitted counts often `18432` or `27648` for a live interest set of
  `9216`. Next structural fix should split `UpdateVoxelInterest` into an
  ordered candidate-plan builder plus separate live-apply and predicted-ticket
  apply paths. Do not replace predicted admission with a loose sampler unless
  it preserves reservation/deadline/priority semantics.
- Follow-up rejection: a broad shared candidate-plan builder was tried and
  removed. Best artifact `highalt_currentfirst_planbuilder_nosetlive_20260606`
  still regressed to `247.83/252.87 ms` window raw/body and target clip
  `69.05 ms` with `1153` active reservation tickets and `636` overdue. Do not
  repeat a full materialized `vector + unordered_set` plan. Any future split
  must be zero-copy or prediction-only and must exactly mirror the old
  nonresident generated-queue filtering before reserving deadline tickets.
- Follow-up rejection: replacing candidate `partial_sort` with `nth_element`
  plus sorting the top range was tried and removed. Artifact
  `highalt_currentfirst_nthsort_20260606` regressed to `211.89/219.04 ms`
  window raw/body and target clip/interest `100.80/75.96 ms`. Do not treat
  this as an isolated STL sort-choice problem; the bottleneck is the whole
  candidate-production and priority contract.
- Follow-up rejection: reserving the candidate de-dup `unordered_map` to the
  full theoretical candidate capacity was tried and removed. Artifact
  `highalt_currentfirst_candidate_map_reserve_20260606` regressed to
  `393.73/396.72 ms` window raw/body and target clip/interest `90.60/84.40 ms`.
  Do not repeat broad container preallocation here; if allocation is revisited,
  size it from measured unique counts or replace the data structure.

## Validation Expectations

For meaningful performance work, validate at minimum:

- fixed/static scenario;
- realtime walking scenario;
- high-altitude scenario;
- noncapture frame-end/window rows when available;
- visual/contact-sheet review for representative frames;
- ownership/miss/unsafe/fallback counters.

Track both target-frame and window behavior. A single target frame can hide hitches. A window with one giant outlier must be explained, not averaged away.

Useful artifacts and scripts usually include:

- `perf_noncapture_smoke.ps1`
- `engine_capture_smoke.ps1`
- `visual_review_capture.ps1`
- `active-goal-handoff.md`
- `handoff.md`
- `debug-handoff.md`
- `root.md`
- latest `build/captures/...` campaign folder

## Compaction And Memory Limits

Future agents may lose hidden reasoning, old chat state, or exact context. Do not rely on memory. Leave facts in files.

Before any final response after substantial work:

1. update `active-goal-handoff.md` with latest status;
2. update `handoff.md` with branch decisions and artifacts;
3. update this file only if the operating strategy changes;
4. state what was retained, rejected, or left default-off;
5. state the exact next measured blocker.

If unsure, prefer a small verified architecture step plus a durable handoff over a speculative broad rewrite.

## User-Intent Guardrail

The user is trying to build something ambitious and novel. Help keep the work rigorous without letting it become endless diagnostics.

The spirit is:

```text
Make VENPOD an ownership-certified, proof-carrying sparse voxel world renderer that can stream an infinite editable world under real frame budgets.
```

Everything else is in service of that.
