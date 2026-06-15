# HANDOFF — Incremental mid-edit GPU bake (approach B)

> Self-handoff written 2026-06-15 before a context compaction. Read this top-to-bottom
> before resuming. Branch `wip-edit-latency`. Repo `z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD`.
> Companion ledger with full history: `LOOPS.md` ("GOAL 2"); cross-session memory:
> `~/.claude/.../memory/venpod-edit-live-overlay.md`. Tandem: `node tandem/bin/peer.mjs`
> (use a FRESH `peer.mjs new` session — prior Codex threads are context-exhausted).

## 0. THE TASK
Make terrain edits not tank perf. Editing-while-moving currently drops to ~20fps because each
edited MID chunk is FULLY regenerated on the CPU (`GenerateVoxelBrickPayload`). Implement
**approach B: a GPU compute bake that applies edits into the mid-voxel clipmap incrementally**,
mirroring the proven near-layer bake. The NEAR layer is already incremental (good); only the MID
layers full-regen on edit. This is a real GPU-compute feature, not a tweak — build it in steps
with a parity gate.

## 1. CURRENT SHIPPED STATE (committed, working — do NOT regress)
- `4b410c4` LOOK-AROUND FIX (user-confirmed: no-edit 80-120fps). Hidden-exact stationary quiesce:
  when stationary+not-editing+post-startup, force wide-raster promotion clean + suppress non-critical
  shader-unsafe foreground repair. Same-session A/B: warm stationary-yaw p50 45->17ms (22->59fps),
  miss=0. Default ON; escape `VENPOD_SPARSE_HIDDEN_EXACT_STATIONARY_GATE=0`. Code in main_launcher.cpp:
  quiesce computed ~7178 (after `sparseCameraSpeedLastFrame = sparseAdmissionSpeed`), counter at ~5142,
  foreground-repair gate ~8189 (`!hiddenExactStationaryQuiesced`), wide-raster clean ~19951.
- `a9be26c` height interest view-independent when stationary (kills the 41-58ms yaw mid-mesh upload).
- `bddd846`/`6046e35`/`1560892` earlier stationary fixes (voxel interest signature forward-zero +
  anchors, exact-hierarchy plan replay, stationary trim suppression). `e467835` residency-class
  telemetry (EDIT_TELEM `res: spec/vis/coll/edit`). Mesh per-tile extraction cache (earlier, 9c6f5b1).
- Working tree currently CLEAN at `4b410c4` (all experiments reverted). Verify with `git status`.

## 2. WHY APPROACH B (and why A / shader-read are OUT)
- **A (CPU partial re-aggregate of edit-footprint cells): REJECTED.** Codex's decisive reason:
  edited mid bricks often start `gpuGenerated` with `brick.voxels.clear()` -> NO resident CPU voxel
  base to patch -> would fall back to full regen on first edit (keeps the hitch). And
  `GenerateVoxelBrickPayload` (SparseClipmap.cpp:5360) is too entangled (shared column cache, 25-sample
  coarse footprint aggregation, relief, water/stone, neighbor-driven VisualSurface flags, 1-cell halo).
- **Shader-read (sample edit overlay live in PS_Raymarch): OFF THE TABLE.** A prior attempt hit the
  DXC -O3 in-process JIT compile cliff (7.3MB blob -> PSO OOM), reverted. Do NOT add overlay sampling
  to PS_Raymarch.
- **B (GPU mid-bake): the fix.** Keep PS_Raymarch untouched. Bake edits into the mid sample pool on GPU.

## 3. IMPLEMENTATION PLAN (approach B, detailed)
Target buffer: `m_midVoxelClipmapSamples` (in `src/Graphics/MidVoxelGpuGenerator.{h,cpp}` — has UAV,
written by `CS_GenerateMidVoxelBricks`). Dirty driver: `InvalidateEditedOverlays` (SparseClipmap.cpp:1662)
already maps changed overlays -> resident mid bricks + 1-cell halo (reuse its mapping). Edit deltas are
already on GPU: `SparseEditDeltas/Ranges/RangeTable` buffers (PhysicsDispatcher; shared with physics +
the near bake). Near-bake precedent to mirror: `assets/shaders/Compute/CS_ApplyEditDeltasToPool.hlsl`,
dispatched at main_launcher.cpp:21075 (`DispatchApplyEditDeltasToPool`), ready-gated `IsApplyEdit
DeltasToPoolReady()` :21066.

Steps (each independently verifiable):
1. **Route edited mid bricks through the EXISTING GPU generator** (`CS_GenerateMidVoxelBricks`) instead
   of the CPU `PumpEditedBrickRegens`/`GenerateVoxelBrickPayload` path. Currently edited bricks fall to
   the CPU path precisely because the GPU generator doesn't know edits. This step alone removes the CPU
   regen cost but leaves edits UN-reflected in mid (the GPU gen is pristine/no-edits) -> NOT shippable
   alone (edit would vanish at distance). It's the foundation for step 2.
2. **New compute pass `CS_ApplyMidEditCellsToClipmap`** (add to MidVoxelGpuGenerator, alongside
   CS_GenerateMidVoxelBricks). For each invalidated resident mid slot, AFTER the pristine GPU gen writes
   the slot, apply LOD-aware edited-cell summaries into the same slot in `m_midVoxelClipmapSamples`.
   It reads the SparseEditDeltas buffers + maps full-res world deltas to each ring's mid local cell.
3. **Dispatch ordering:** run the mid edit-apply pass AFTER mid sample generation/upload writes
   (main_launcher ~16225) and BEFORE the raymarch; UAV barrier after, transition back to SRV. Mirror the
   near-bake placement (~21066-21090). Ready-gate it like the near bake.
4. **Drive the dirty set** from InvalidateEditedOverlays' resident-brick mapping (1662) — only invalidated
   slots get the apply pass.

## 4. CORRECTNESS PITFALLS (MUST cover — from the CPU generator, must match it)
Reference the CPU logic in `tryEditedCellVoxel` (SparseClipmap.cpp ~5556-5624) and `addEditedVoxelToCell`
(~5625). The GPU pass MUST reproduce:
- **LOD aggregation rule** (per mid cell, ring `cellSize`): SOLID edit always wins (`foundSolid` ->
  `solidVoxel`). AIR edit wins if `localAirEdit` (cellSize<=1.5 && foundAir) OR `clusteredCoarseAirEdit`
  (cellSize>1.5 && solidCount==0 && airCount>=coarseAirClusterThreshold), where
  `coarseAirClusterThreshold = (cellSize<=16 ? 1 : max(8, ceil(cellSize)))`. Otherwise the cell keeps its
  procedural/base value (no edit override).
- **Halo:** dirty cells must include >=1 local-cell neighbor in X/Y/Z (VisualSurface/neighbor material
  classification depends on adjacent cells, incl. cross-brick halo). The CPU uses `editHaloSide =
  SPARSE_BRICK_SIZE+2`.
- **Current state, not just this-frame deltas:** erasing a prior solid edit must restore the procedural
  base if no remaining edit owns that mid cell. Because step-1's pristine GPU gen writes the base first,
  then the apply pass overlays the CURRENT edit overlay state, an erased voxel correctly reverts to base.
  (Do NOT bake only the current frame's delta like a naive near-clone — Codex flagged this.)
- **Counts** (`nonAirSamples`/`surfaceSamples`): stats-only for GPU bricks; do NOT block the fix on
  keeping CPU counters exact.

## 5. VERIFICATION GATES (do not flip to done without these)
1. **Visual:** edit terrain at mid distance while moving -> carved/painted changes appear in mid voxels
   WITHOUT several-frame CPU regen lag. Capture + (fresh) Codex view_image judge.
2. **Perf:** `propMidRegen` -> ~0 during live edits; edit-frame CPU cost no longer scales with dirty mid
   bricks. Measure with the moving-edit repro (below). Baseline to beat: EDIT-window p50 ~25-40ms,
   clipInterest(edit) ~9-15ms, edit-invalidation block ~8.2ms (UpdateInterest itself is only ~1ms).
3. **Parity test (correctness):** compare GPU-baked dirty bricks vs CPU `GenerateVoxelBrickPayload` for:
   fine rings + coarse rings, solid edits, air edits below/above the cluster threshold, boundary/halo
   edits, steep slopes. Must match (or document acceptable LOD differences).
4. **No-holes:** miss=0 camera pass over edited mid terrain (owner-map `miss=`).

## 6. HOW TO MEASURE / REPRO (harnesses + commands)
- **Build:** `cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat\" -arch=amd64 >nul 2>&1 && cmake --build \"z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build\" --target VENPOD"`
  (VS18 not 2022; use the ABSOLUTE build path — the PowerShell working dir resets between calls).
- **Run:** `rebrun.ps1 -PerfMode 60fps -NoBuild` (forces sandbox + the real env). Frame cap:
  `-ExitAfterFrames N`.
- **Moving-edit repro (the slow case):** `delrepro.ps1 -Case 0 -WalkSpeed 15 -YawDegPerSec 30 -RadiusTenths 60 -RealAim -Frames 1000` with `$env:VENPOD_EDIT_TELEMETRY="1"`. Reads PRE(240-299)/EDIT(300-479)/POST(480+) frame split. Case 0=paint, 2=erase. Stationary-edit is already fast (~4.7ms); the cost is editing WHILE MOVING.
- **Telemetry** (`VENPOD_EDIT_TELEMETRY=1`): `EDIT_TELEM` (prop: midRegen/heightTiles/invBricks/invTiles;
  res: residentBricks/freePages/spec/vis/coll/edit), `PERF_SPARSE_STEPS` (clipInterest/genPrep/surfExtract/
  midMeshUpload/trim/gpuMs/body), `PERF_SPARSE_REQ`, `PERF_RENDER_OWNERSHIP` (miss=, midVoxel=, midHeight=,
  lodParentHeld=). The edit-invalidation block (PumpEditedHeightTileRegens + InvalidateEditedOverlays +
  InvalidateEditedHeightTiles) is timed INSIDE `clipInterest` (main_launcher ~12052-12085).
- Other harnesses: `yawverify.ps1` (look-around perf), `perfmap.ps1` (idle/look/move/movelook breakdown),
  `exactsweep.ps1`/`lodpolicy.ps1` (footprint/LOD sweeps).

## 7. MEASUREMENT DISCIPLINE (learned the hard way — DO NOT violate)
- **SAME-SESSION A/B ONLY.** This box has GPU-mem drift across launches; absolute fps is NOT comparable
  cross-run. Compare fix on/off back-to-back, and trust count/serial/byte/residency metrics over fps.
- **REBUILD after reverting** or you measure a stale binary (this burned me once — an A/B showed "no
  saturation" because the reverted drain was still in the running exe).
- **~50 launches/session -> GPU-mem exhaustion crash-on-launch** (runs start returning 0 frames / dying
  early). A reboot clears it. If runs start dying, stop and ask the user to reboot.

## 8. DEAD ENDS — already tried, DO NOT REPEAT (with why)
1. Footprint knobs (`SCREEN_CRITICAL_DISTANCE` 1024->256, `SURFACE_PREFETCH_DISTANCE` 3072->384,
   `MID_VOXEL_RADIUS_XZ` 14->8, `MID_VOXEL_INTEREST_PCT` 75->25): NONE reduce the exact-pool resident
   (it saturates ~99.8% regardless). Not the lever.
2. Visibility-aware pool DRAIN: bounds resident 32.7k->28.5k but NO frame win (work continues). Reverted.
3. Incremental pressure trim: no-op.
4. **O(recent) edit-overlay scan** (`ForEachOverlayChangedSince`): the overlay ITERATION is NOT the edit
   cost — same-session A/B was 9.2 vs 9.5ms (no diff). The cost is the REGEN WORK, not the scan. Reverted.
   (i.e. don't "optimize the invalidation loop"; optimize/eliminate the per-chunk REGEN — that's why B.)
5. `EDIT_MESH_REBUILD=0`: saves only ~3.5ms (the height-tile half) + has a visual tradeoff. Not the full fix.
6. **Collapsing the double mid representation: REJECTED.** voxel clipmap = near/mid (owns midVoxel pixels);
   height mesh (`m_tiles`) = FAR out to 9000. A/B verified: mesh-off is identical on GROUND walk but at
   ALTITUDE-looking-down the distant terrain goes MISSING/grey. They're complementary LOD tiers, not
   duplicates. `midHeight=0` owner reading was misleading (mesh's far coverage below horizon at ground).
7. Probe-gating hidden-exact (suppress the miss probe): made it fill MORE. The real look-around fix was
   the wide-raster-clean + foreground-repair gate (4b410c4). Don't touch the probe.

## 9. VERIFIED ARCHITECTURE FACTS (don't re-investigate)
- LOD tiers: near exact raster (<~1024) + mid-voxel clipmap (raymarched, m_voxelBricks ~16384, owns the
  visible mid) + mid-HEIGHT mesh (m_tiles ~512, far out to 9000) + far SVO.
- 5 LOD rings = distance-band ANNULI (BuildRings SparseClipmap.cpp:456: ring r = [start+span*r/N,
  start+span*(r+1)/N], cell = minCell*growth^r). NOT 5x waste — this is what makes 9km terrain feasible.
- **Backface culling: CONFIRMED correct** — `m_sparseSurfacePipeline` = CULL_MODE_BACK + frontCounter
  Clockwise (Renderer.cpp:1906). CULL_NONE pipelines are fullscreen passes only (culling irrelevant).
- Straight-walk surface SHIMMER = shader LOD-ring flicker (a fixed point flips LOD ring as its distance
  changes while walking). CHEAP (surfExtract 0.87ms, 3 mesh uploads/899 frames). User said this is FINE
  if intended/cheap -> do NOT spend on it. It is NOT expensive regeneration.
- Edit cost ROOT (what B fixes): per-frame full CPU chunk regen (`GenerateVoxelBrickPayload`) of edited
  mid bricks while editing-while-moving. Near is incremental (bake); mid is not.

## 10. KEY CODE LOCATIONS (quick index)
- CPU mid regen to AVOID + the aggregation rule to MIRROR: `GenerateVoxelBrickPayload` SparseClipmap.cpp:5360;
  `tryEditedCellVoxel` ~5556-5624; `addEditedVoxelToCell` ~5625.
- Dirty driver: `InvalidateEditedOverlays` SparseClipmap.cpp:1662; CPU regen pump `PumpEditedBrickRegens` :1751.
- Edit-invalidation block (timed in clipInterest): main_launcher.cpp ~12052-12085.
- GPU mid pool + generator: `src/Graphics/MidVoxelGpuGenerator.{h,cpp}` (`m_midVoxelClipmapSamples`,
  `CS_GenerateMidVoxelBricks`). Mid sample gen/upload in main_launcher ~16225.
- Near-bake precedent: `assets/shaders/Compute/CS_ApplyEditDeltasToPool.hlsl`; dispatch main_launcher.cpp:21075;
  ready-gate :21066; `PhysicsDispatcher::DispatchApplyEditDeltasToPool` (PhysicsDispatcher.h:255).
- Edit GPU buffers: `SparseEditDeltas/Ranges/RangeTable` (PhysicsDispatcher).
