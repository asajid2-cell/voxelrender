# VENPOD Frontier / Persistent Streaming — Design & Incremental Plan

Status: DESIGN, with a CRITICAL UPDATE 2026-06-07 — the core thesis (gate the
per-frame rebuild on recenter) was IMPLEMENTED, BENCHED, and FAILED. See
"## RESULT: Step 1c failed and why" below before doing anything else. Read
`instruct.md` and the experiments section of `active-goal-handoff.md` first.

## RESULT: Step 1c failed and why (2026-06-07)

Implemented center-gated voxel-interest reuse (`voxelInterestRecenterGate`, reuse
while the camera stays in the same finest-ring brick cell — the "correct" gate,
not the camera-blind age reuse). Built clean, benched on the deterministic walk,
then REVERTED. Result: regressed (median ~56 ms vs ~38 baseline; request 18.6, gen
13; clip hitches to 121 ms at recenter frames).

Mechanism (from per-frame logs): on stable-center frames the gate worked perfectly
(interest 1 ms, pump 0.4 ms, missingVoxel 0, clip 2.5 ms). But at each recenter,
missingVoxel jumped to ~1479 and the pump exploded to ~118 ms. The reuse holds the
set stable across ~13 frames, the camera drifts, then the recenter dumps the whole
accumulated frontier delta at once -> pump can't absorb it -> 121 ms hitch.

KEY INSIGHT (revises the whole thesis): **the per-frame full rebuild is the
engine's generation load-balancer.** It is NOT redundant waste — by re-deriving the
frontier every frame it feeds generation smoothly at ~46 bricks/frame. Skipping it
(in ANY form: age reuse, center-gated reuse) batches the same work into recenter
bursts that blow the pump budget. This is why all 5 experiments failed: every one
either shifts cost or breaks the smooth per-frame generation feed.

Corollary: the true cost of forward walking is mostly IRREDUCIBLE per-frame
frontier work — generating+meshing+uploading the ~46 NEW bricks the camera walks
into each frame (gen 7 + pump 6 + surface 3.5) plus interest scoring (6) + request
planning (10). You cannot skip generating terrain you are about to walk into.

REVISED real levers (none are a same-day flag fix):
- (A) Prefetch-ahead generation: run worker threads AHEAD of the camera along its
  known velocity so the frontier is generated BEFORE it is needed and is resident on
  arrival -> frontier gen leaves the critical path. Different from the rejected
  parallel test (which parallelized SAME-frame work). This is the genuine fix for
  forward motion but has a graveyard of prior attempts (predicted-visible admission,
  async exact prefetch lane) — must be done carefully on the trustworthy bench.
- (B) Cheaper per-brick gen/mesh (profile per-brick hotspots; micro-opts may stack).
- (C) Cheaper interest SCORING without skipping the rebuild (reduce the ~50k
  candidate attempts / ~30k duplicates at the source, like the retained camera-band
  skip) — keeps the smooth feed, only trims the compute. Likely sub-noise alone.

The original Step 1/2/3 below (gate rebuild on recenter) is SUPERSEDED by this
result. Do not retry interest reuse in any form.

---

(Original design retained below for context.)

## Why this exists

Four clean experiments on the trustworthy deterministic walk bench (`walk_bench.ps1`)
proved **local/flag optimization is exhausted** — every perturbation breaks the
coverage/generation equilibrium:

| tested | result |
|---|---|
| parallelize stages (workers) | cost relocated; median slightly worse |
| stop trim (bigger working set) | frame doubled (set balloons) |
| shrink interest set (less work) | frame doubled (coverage thrash) |
| age-based interest reuse | missingVoxel 130->5184 (40x); coverage holes + repair thrash |

Walking baseline: raw median ~34-42 ms (~24-29 FPS), noise band +/-7.6 ms (20%),
p99 ~60-87 ms. The ~38 ms is spread with no single dominator (request ~10, clip
~12 = interest 6 + pump 6, generation ~7, surfExtract ~3.5). The full per-frame
rebuild over a view-sized working set is the STABLE operating point; the cost buys
the stability.

## Root cause (precise, from the lifecycle map)

Residency is already persistent (`SparseBrickPool::m_resident` slot-by-coord,
free-page queue, frame touches). The waste is that the **working set is fully
re-derived every frame from continuous camera state**, even when the discrete
clipmap center has not moved:

- request planner re-`Plan()`s the whole footprint each frame
  (`main_launcher.cpp:10352`), full rebuild when `sparseCenter` changes (`:10347`);
- collision shell is a full triple-loop every frame (`:10374`);
- mid-clipmap interest does `m_voxelInterestSet.clear()` + full anchor/candidate
  rebuild every frame (`SparseClipmap.cpp:1735`, `UpdateVoxelInterest`);
- **view-cone prefetch raycasts a grid on CONTINUOUS `cameraPos` every frame**
  (`main_launcher.cpp:10387-10437`) — the main steady-state churn source;
- no persistent generation/mesh cache: an evicted brick is regenerated from seed on
  re-request (`SparseTerrainGenerator::GenerateBrick`); only edits persist.

At walk speed (38 u/s * 16 ms dt = ~0.6 voxels/frame, brick = SPARSE_BRICK_SIZE
voxels) the discrete center crosses a cell boundary only ~every 13 frames. So
~12/13 frames re-derive a working set that is identical at the discrete level — pure
redundant work, plus eviction-churn regeneration.

## The design: quantized, incrementally-maintained working set

Make the working set a pure function of the **discrete clipmap center**, maintained
**incrementally at the frontier**, instead of re-derived from continuous camera
state every frame.

Two pillars:

### Pillar A — Frontier delta (attacks request ~10 + interest ~6, and most gen ~7)

- The resident/interest working set = discrete clipmap rings around the discrete
  center. It changes ONLY on a **recenter event** (center crosses a cell boundary).
- On recenter: compute the delta — coords in the ENTERING ring(s) (added) and the
  LEAVING ring(s) (removed). Request/generate only the entering coords.
- Between recenters (the ~12/13 common case): **skip** the request `Plan()`, the
  collision-shell rescan, the clipmap interest rebuild, and the continuous view-cone
  raycast. The persistent resident set + upload/pump/publish backlog drain continue
  unchanged. Rendering uses the stable resident set at the moving sub-cell viewpoint
  (correct — the camera sees the same resident terrain from a slightly different
  position).

Why this does NOT thrash like age-reuse: age-reuse kept a STALE set (camera moved,
set was not updated -> holes). Frontier delta keeps the set CORRECT — it is updated
exactly at the discrete frontier on recenter; it merely stops re-deriving an
unchanged set every frame. Coverage is preserved by construction.

Forward bias: to avoid a visible miss when the camera turns or accelerates into a
new cell, prefetch the entering ring 1-2 cells AHEAD in the motion/forward
direction — still discrete/quantized, computed on recenter, not continuous.

### Pillar B — Persistent generation/mesh cache (attacks eviction-churn gen + surface)

- Bounded CPU-side LRU cache of generated + surface-extracted bricks keyed by coord
  (+ edit-version), so a brick evicted from GPU residency and later re-requested is
  restored from cache instead of regenerated from seed.
- Decouples bounded GPU residency (trim stays load-bearing) from generation cost.
- Note there is already a `SparseSurfaceCache`; evaluate extending it vs a new cache.

### Recenter amortization

A recenter adds a ring of N coords at once -> potential spike. Since recenters are
~13 frames apart at walk speed, prefetch the next entering ring over the
intervening frames with a per-frame budget, so no single frame hitches. This also
helps the p99 tail (the gapPrev hitches likely coincide with recenter + catch-up).

## Why prior failures do not apply

- Not parallelization (that only relocated cost): this REMOVES the work on ~12/13
  frames.
- Not set shrink (that thrashed coverage): same set size, same coverage.
- Not stale reuse (that went stale): the set is correctly maintained at the frontier.

## Incremental, flag-gated, bench-validated rollout

Every step: default-off flag, validate on `walk_bench.ps1 -Runs 5`, accept only if
median AND p99 beat the +/-7.6 ms noise band AND coverage (missScreenPct,
coverageVisibleCritical, missingVoxel) does not regress. Commit or revert each step.

- **Step 0 (instrument, no behavior change):** count recenter vs non-recenter frames
  on the deterministic walk; confirm ~12/13 are non-recenter. Confirms the prize.
- **Step 1 (the make-or-break test):** flag to gate the per-frame request `Plan()` +
  collision rescan + clipmap interest rebuild + view-cone raycast to run ONLY on
  recenter frames (centerDelta != 0) or frame 0. Between recenters, skip them; let
  residency/upload/pump drain. Bench: expect request+interest+gen -> near 0 on
  non-recenter frames, median dropping toward pump/surface-only. **Gate hard on
  coverage** — if missingVoxel/missScreenPct explode, the continuous prefetch was
  load-bearing -> go to Step 2 first.
- **Step 2 (forward-biased discrete ring):** if Step 1 shows frontier misses on
  turns/acceleration, add discrete forward-ring prefetch on recenter (1-2 cells
  ahead). Re-bench.
- **Step 3 (recenter amortization):** spread entering-ring generation across frames
  until the next recenter with a per-frame budget; target the p99/gapPrev tail.
- **Step 4 (Pillar B persistent gen/mesh cache):** restore evicted bricks from cache
  instead of regenerating; targets residual gen churn and back-and-forth motion.

### Step 0 result (2026-06-07) + Step 1 implementation anchors

Step 0 CONFIRMED: on the deterministic walk, ~95% of frames have a stable center
(`centerDelta=0/0/0`; 74 zero vs 4 nonzero sampled, frames >=300). So the
re-derivation is redundant on ~19/20 frames. Prize confirmed.

Step 1 implementation anchors (main_launcher.cpp):
- `sparseRequestFullRebuildThisFrame` is initialized to `1u` EVERY frame when
  `enableSparseHierarchicalRequests` (line ~6444) -> the hierarchical path (active
  on walk; nonzero hierarchyMs/terrainCriticalMs) forces full rebuild every frame.
  This is THE thing Step 1 must change: set it to 1 only on recenter
  (`sparseCenter != lastSparseRequestCenter`) when the frontier-gate flag is on, in
  BOTH the hierarchical (`if`) and non-hierarchical (`else`) branches.
- Non-hierarchical branch already gates the visible `Plan()` on recenter (line
  ~10347) but runs the collision-shell loop (~10376, cheap touches) and the
  continuous view-cone raycast prefetch (~10387-10437) every frame.
- Hierarchical branch (~10240-10344) runs terrain-surface-prefetch raycasts on
  continuous cameraPos (gated by budget; was 0ms in the baseline so not the current
  cost) plus the terrain-critical/hierarchy planning (the ~1.3-1.8ms hierarchyMs).
- The mid-clipmap interest rebuild (`UpdateInterest`/`UpdateVoxelInterest`, called
  ~10938) is SEPARATE and runs every frame (~6ms interest). Its recenter gate must
  check the MID clipmap center, and must update the set correctly on recenter (NOT
  the camera-blind age reuse that thrashed).
- `lastSparseResidencyCameraWorld` / `sparseCenter` give the recenter signal.

Recommended Step 1 sequencing (each its own flag + bench + commit/revert):
  1a. Gate the continuous prefetch raycasts (view-cone + terrain-surface) to
      recenter-only (safe: speculative work). Smallest, safest first cut.
  1b. Gate the hierarchical full-rebuild (line 6444 init) to recenter-only.
  1c. Gate the mid-clipmap interest rebuild to recenter-only (correct set update on
      recenter, not stale reuse).
Validate coverage HARD at each (missingVoxel must not explode like age-reuse did).

Step 1 is the thesis test. If median drops materially with coverage held, the
architecture is validated and Steps 2-4 refine it. If coverage cannot be held even
with Step 2's forward ring, the working-set definition itself must change before
proceeding — stop and reassess rather than forcing it.

## Guardrails

- Keep `walk_bench.ps1` (fixed dt + vsync off + per-frame logging + noise band) as
  the sole accept/reject authority for walking. Never decide on one run/one frame.
- Preserve the trim (load-bearing) and the working-set size (coverage equilibrium).
- One flag, one measured step, commit at each checkpoint. No stacking unvalidated
  changes.
