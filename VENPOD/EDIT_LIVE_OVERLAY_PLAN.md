# Live edit-overlay in the renderer — plan (wip-edit-latency)

## Why
Editing (paint/erase) speckles + hitches because edits live only in the CPU
`SparseEditStore` and are PROPAGATED each frame into the things the renderer
samples (mid voxel brick regen, exact brick regen+upload, mid-mesh rebuild).
That propagation is budgeted → latency = skipped-voxel speckle, cost = hitch.
Measured: fully CPU-bound (gpuRead 0.01ms), resolution-independent, the
`clip` (clipmap-prep) phase balloons 1.8ms→20-46ms during a stroke. A budget
knob trades speckle for hitch and cannot win (proved: raising it → 100% hitch
AND still failed the speckle test; reverted in 6188048).

## The fix (user-approved 2026-06-13)
Make the renderer read edits LIVE from a GPU edit overlay; demote the
regen/upload/mesh propagation to lazy background durability.

## KEY DISCOVERY — the GPU overlay already exists, wired only to PHYSICS
- GPU buffers exist: `SparseEditDeltas` / `SparseEditDeltaRanges` /
  `SparseEditDeltaRangeTable` in `SparseVoxelGpuResources` (created ~393-426;
  config `maxEditDeltas=8192`, `maxEditDeltaRanges=2048`).
- Proven GPU lookup exists: `CS_SparsePhysicsPackets.hlsl:110 TrySampleEditDelta(brickCoord, localVoxel, out voxel, out revision)`
  — hash-probe rangeTable (64 probes) → match brick → linear scan deltas in
  range → return latest-revision voxel. Plus `TrySampleSparseVoxelWithEditDeltas`.
- Staging exists: `SparseVoxelGpuResources::StageEditDeltas` + `EmitEditDeltaCopy`,
  fed by `BuildSparseEditDeltaBatch` (deltas+ranges+rangeTable). Called in
  `main_launcher.cpp:18438` — but GATED behind `enableSparsePhysicsPacketUpload
  && enableSparsePhysicsGpu`, and the physics path CONSUMES the pending queue
  (`ClearPendingGpuEditDeltas`) and snapshots only simulated bricks.
- `PS_Raymarch.hlsl` already `#include`s SharedTypes (defines `SparseEditDelta`,
  `SparseEditDeltaRange`) and already has `HashSparseBrickCoord` (line 163 — MUST
  verify it matches `CS_SparsePhysicsPackets.hlsl:60`'s hash, or the table lookup
  fails). Needs `PackLocal` (trivial).

## Scoping subtlety (the real design decision)
Buffer caps at 8192 deltas / 2048 bricks. A session accumulates ~47k deltas. So
the render overlay must be SCOPED to the visible/near edited bricks (like physics
scopes to simulated bricks), capped at maxEditDeltas. The lazy background
propagation makes older/distant edits durable so they can leave the overlay and
still render correctly from the pool/clipmap. Overlay = recent/near; pool = durable.

## Phases (verify each with Codex view_image at full detail + delrepro perf)
1. **Render-scoped delta staging.** Build a delta batch for edited bricks within
   a radius of the camera (cap 8192), stage to GPU (own ticket, NOT consuming the
   physics queue), bind SRVs for render. Run regardless of physics flags.
2. **Raymarch samples overlay.** Port `TrySampleEditDelta` into PS_Raymarch; add
   delta count/range/capacity to the raymarch frame CB; bind the 3 SRVs to the
   fullscreen root signature (currently ends at t17 — use t18/t19/t20). Call in
   `TrySampleSparseBrickVoxel` (PS_Raymarch:287) BEFORE the occupancy early-outs
   (paint into empty subbrick) AND override after pool fetch (erase). Also
   `SampleResidentMidVoxel` (~1715) for the mid layer. Keep branchless-friendly
   (NVIDIA JIT cliff). VERIFY: edits render live with regen pumps DISABLED.
3. **Stencil landmine.** Near RASTER (PS_SparseSurface) + mid MESH write stencil
   before the stencil==0-gated raymarch, hiding any PS-only fix over stale
   geometry. Near erase: existing `liveErased` discard may cover it — verify.
   Mid mesh: edit-footprint suppression already committed (8df0681) — confirm it
   reacts live (it's tied to height-tile regen serial; may need a live footprint).
4. **Demote propagation to lazy.** Once overlay is authoritative for render, drop
   PumpEditedBrickRegens/PumpRegeneratedEditUploads/PumpEditedHeightTileRegens to
   low-priority background (or post-stroke) — reclaims the hitch. The
   redundant-regen-skip (6188048) makes that background work cheap.

## Verification rig
`delrepro.ps1 -Case 2 -ElevateY 55 -YawDegPerSec 8 -RealAim` (airborne, moving,
UI hidden). Codex judges captures at full detail (my Read downsamples). NOTE:
absolute ms is noisy on this box (background load) — compare clip-phase / path
counts / A-B same-session, not raw frame ms across runs.

## Status
- [x] Diagnosis + regression revert (6188048) + redundant-regen skip
- [ ] Phase 1..4
