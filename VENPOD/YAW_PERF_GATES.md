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
