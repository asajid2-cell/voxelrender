# Yaw / streaming perf — objective gates (wip-edit-latency)

Three separable bugs (proven by profiling, see report in session):
- **A. Height interest mutates on stationary yaw** → tiles regenerate → HeightDirtySerial
  climbs → full mid-mesh upload. (A/B proven: view-indep height interest cut uploads 20→2.)
- **B. Mid-mesh upload is full-snapshot** (1.5M faces, 41–58ms) with no dirty path.
- **C. Exact-surface streaming churn** on yaw (genPrep+surfExtract+reqPrep ≈ 22ms),
  independent of mid-mesh (proven with VENPOD_SPARSE_MID_MESH=0).

INVARIANT we are enforcing:
> Pure camera rotation may change priority, culling, draw order.
> Pure camera rotation must NOT regenerate tiles, bump content-dirty serials,
> or upload unchanged geometry.

## Measurement discipline
- This box has cumulative GPU-memory drift across launches → **absolute fps is NOT a
  reliable cross-run gate.** Use deterministic counts/serials/bytes, and same-session
  A/B for any fps. Rotation test = walk-test speed=0, yaw=50, fixed camera, observe
  steady window (frame ≥ 300/400). turnperf.ps1 / perfmap.ps1 are the harnesses.

## Gates (each commit must pass its gate before landing)

- **G1 — height interest stationary (Commit 1).** Pure yaw, fixed pos, steady window:
  - HeightDirtySerial delta over the window ≤ 5  (baseline ≈ +520)
  - mid-mesh uploads over the run ≤ 3            (baseline 20–22)
  - reliable; no fps dependence.

- **G2 — dirty semantics (Commit 2).** A height tile that is regenerated/reprioritized
  but whose mesh-relevant content is unchanged must NOT bump HeightDirtySerial nor mark
  meshDirty. Verified via per-reason counters: heightTilesGenerated may be >0 while
  heightTilesContentChanged == 0 ⇒ serial bump == 0.

- **G3 — incremental upload (Commit 4).** uploadedFaces per upload ∝ dirty tiles, not
  total resident tiles. Single-tile content change ⇒ uploadedFaces ≤ ~a few k (one tile),
  NOT ~1.5M. Metric: midMeshUploadedFaces / midMeshTotalFaces ≪ 1 during gameplay.

- **G4 — NO STALE TERRAIN (correctness, all commits).** Moving + editing + looking still
  shows correct, up-to-date terrain (no stale tiles, no holes). Codex visual + edit smoke.
  This gate VETOES any perf win that corrupts the terrain.

- **G5 — exact-surface bounded (Commit 5/6).** After instrumenting terrainCrit: yaw
  reqPrep+genPrep+surfExtract bounded per frame (hard caps honored), and we KNOW whether
  yaw cost is new-content vs reprocessing-existing (counters answer it).

## Commit sequence (from the engineering review)
1. Stationary height-interest gate (translation-based + hysteresis, default on).  [G1,G4]
2. HeightDirtySerial bumps only on real mesh-content change.                       [G2,G4]
3. Debug counters (added FIRST in practice, so gates are measurable).              [enables G1–G5]
4. Incremental dirty-tile mid-mesh upload (StageDirtyPayloadSnapshot) + per-frame budget. [G3,G4]
5. Exact-surface terrainCrit counters + disable toggle.                            [G5]
6. Bound exact-surface yaw work (caps + tighter trim).                             [G5,G4]

Status: instrumenting first (commit 3 content) so gates are measurable.

## Cost-center C — DIAGNOSED (hard data, 2026-06-14)
Long stationary-yaw (1300 frames, ~5 rotations), resident-brick trajectory:
  fixes ON : 11k->21k->27k->32k->32719->32714->32719 (PLATEAU at pool cap 32768)
  fixes OFF: 4k->17k->24k->30k->32720->32697->32722 (SAME saturation)
genPrep stays 5-9ms after saturation; frame rawMs WORSENS 32->48ms as it fills.
=> NOT caused by my stationary fixes (A/B identical). NOT new content (it's reprocessing
   at the ceiling). ROOT: exact-surface streaming (terrainCrit requests visible surface
   for every viewed direction) accumulates the exact pool to its hard cap; generation
   ~24 bricks/frame OUTPACES trim ~8 bricks/frame (sparseTrimBudget/PressureTrimBudget),
   so the pool saturates and then evict-on-turn/regen-on-turn-back churns forever. The
   full-look-around exact working set EXCEEDS the 32768-page pool.
Candidate fixes (need tandem design + G4 hole-check; over-trim risks holes):
  - rate-balance: scale pressure-trim budget with the free-page deficit so the pool
    holds its reserve instead of saturating (evict only beyond keepRadius=13 => safe,
    off-screen bricks). Gate Gc: resident plateaus BELOW cap (<= ~28672), genPrep
    settles toward ~0 after one full rotation, frame rawMs stops worsening. G4 holds.
  - or bound the viewed-direction accumulation (position-stable core + bounded
    speculative), or tighten trim radius.

## Cost-center C — FIX ATTEMPT FAILED THE GATE (honest result, 2026-06-14)
Tandem (codex) corrected the approach: scaling the plain TrimResidentBricks is
hole-risky (it ignores frustum/lastVisibleFrame; visible far surface reaches ~1024
voxels >> keepRadius 13). Safe path: deficit-scaled VISIBILITY-AWARE
TrimBackgroundResidentBricks (skips current-frame-visible) AFTER the terrain-critical
probe. Implemented + measured (long stationary yaw):
  - Pool bounding WORKS: resident 32719 -> plateaus ~28460 (free ~4040 reserve). ✓
  - G4 holes: owner-map miss= pixels == 0 all run. SAFE. ✓
  - Gate Gc FAILED: genPrep still 7-9ms, rawMs still ~48ms. NO frame-time win. ✗
=> REVERTED. The drain bounds the pool but does NOT reduce the churn cost, because
   the exact-surface generation churn is FUNDAMENTAL: turning to any new direction
   generates that direction's exact surface bricks (~9ms genPrep, CPU-bound ~24/frame),
   and the full-360 working set exceeds the pool no matter how you trim. Pool/trim
   management can't fix it.
REAL cost-C fix (substantial, next): (a) GPU terrain generation so regen-on-turn is
   cheap (existing plan, see [[venpod-gpu-terrain-gen]]), OR (b) shrink the exact-surface
   footprint (coarser surface LOD / shorter terrainCrit screen-critical distance) so the
   surround fits the pool and turn-back hits cache. Not a trim tweak.
