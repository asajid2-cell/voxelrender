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

## ATTEMPT 1 (shader-read) — HIT THE JIT CLIFF, reverted
Implemented the full shader-read slice: editDeltaParams CB field + 3 SRVs
(t18/t19/t20) on the fullscreen root sig (params 20/21/22) + ported
TrySampleEditDelta into PS_Raymarch's TrySampleSparseBrickVoxel (overlay
authoritative: one early check covers paint-into-empty AND erase-override). All
plumbing was correct (compiled, gated off by editDeltaParams.x default 0).
- Standalone dxc.exe (DXC 1.8.2502) compiles it fine at -O3.
- The ENGINE in-process compile of the GIANT PS_Raymarch at -O3 EXPLODES: the
  per-sample hash-probe+scan loops inline at every voxel-sample site -> 7.3MB
  shader blob (normal is KB) -> PSO creation fails E_OUTOFMEMORY (0x8007000E).
  Reducing probe 64->32 + simplifying the inner loop (drop revision tracking,
  last-match-wins) got it to COMPILE (178s!) but still 7.3MB -> PSO OOM.
CONCLUSION: per-sample edit-overlay LOOPS inside the inlined fullscreen PS are
unviable on this compiler. This is the NVIDIA/DXC JIT cliff Codex warned about.
Reverted the 4 files (SharedTypes, PS_Raymarch, Renderer.cpp/.h) to clean tree.

## ATTEMPT 2 — compute pre-pass bakes deltas into the pool (VALIDATED w/ Codex)
New CS_ApplyEditDeltasToPool: one thread per COALESCED latest-unique delta ->
LookupSparseBrick -> reject missing/tombstone/oob/generation-mismatch -> write
SparseBrickVoxelPool[pageIndex*4096+localIndex]=voxel + InterlockedOr the sub-brick
occupancy bit (NON-AIR only). PS_Raymarch UNCHANGED (no compile explosion); edits
go live; CPU regen/upload demotes to durable tier.

CONCRETE STEPS (Codex-converged, see TANDEM.md):
1. CPU: build a coalesced latest-unique (brickCoord,packedLocal) delta batch for
   bake (compute thread order undefined). Reuse staging (StageEditDeltas) but ensure
   coalesce. Stage to GPU each frame, NOT gated on physics, NOT consuming physics queue.
2. New `CS_ApplyEditDeltasToPool.hlsl`: read delta SRV + page table + page gens;
   RWStructuredBuffer<uint> SparseBrickVoxelPool (write voxel); InterlockedOr
   RWStructuredBuffer occupancy (uint2 word; subIndex=(local>>2).x+y*4+z*16,
   0..31->.x/32..63->.y) for non-air only; never clear bits for air (conservative).
3. New dispatcher (SparseEditBakePass or in Renderer) + UAV descriptors/transition
   helpers in SparseVoxelGpuResources (pool+occupancy already UAV-capable:
   CreateUAV at SparseVoxelGpuResources.cpp:171 pool, :213 occupancy).
4. main_launcher: hook AFTER all upload-ring copies, BEFORE RenderVoxels. Frame
   order: copies -> transition pool+occupancy to UNORDERED_ACCESS (page table/gens/
   deltas stay NON_PIXEL_SHADER_RESOURCE) -> dispatch -> UAV barriers -> transition
   pool+occupancy back to NON_PIXEL|PIXEL_SHADER_RESOURCE -> render. Bake MUST be the
   last writer to the pool before render (rerun/fence if any late copy).
5. Phase 4: demote PumpEditedBrickRegens/PumpRegeneratedEditUploads/height pumps to
   lazy/post-stroke (durable tier). Redundant-regen-skip (6188048) keeps it cheap.
Do NOT bake mid-clipmap in v1 (near bricks are the authoritative live tier).
REJECTED alternatives (Codex): lower -O3 (brittle driver bet), coarse 3D mask
(still a per-sample fetch + 2nd exact lookup), separate live-edit pool (still needs
PS changes). Bake wins: PS reads pool/occupancy exactly as today.

## Status
- [x] Diagnosis + regression revert (6188048) + redundant-regen skip
- [x] Attempt 1 (shader-read) — reverted, hit DXC -O3 in-process compile cliff
- [x] Attempt 2 (compute-bake) — LANDED + verified (c0891ab). Codex PASS: speckle gone.
- [x] Lag fix: bake is INCREMENTAL (this frame's brush deltas only, e182fb4) +
      gated to active editing (4c4d7b0). A/B: bake now ~free (ON 33.0 vs OFF 33.8ms,
      was ~60ms with the full-history snapshot). Codex PASS, no leftover stale voxels.
- [x] Propagation demotion REVERTED — mid-mesh suppression must stay per-frame
      (Codex caught terrain slabs occluding the carve when coalesced to every 4th).
- [ ] REMAINING (separate, lower priority): ragged voxel rim / blocky terracing at
      the carve edge (edge polish, NOT a bake bug). Phase 3 stencil landmine is moot
      for near (raster discard + mesh suppression already handle it); mid-clipmap
      live bake not done (near is the authoritative live tier).

## How it works now (summary)
Brush edit -> SparseEditStore (CPU). Each frame, the deltas applied THAT frame are
captured (recordSparseBrushStamp -> sparseLiveBakeFrameDeltas) and staged; a compute
pass (CS_ApplyEditDeltasToPool, one thread per range/brick) writes them into the
resident GPU brick pool + occupancy right before the raymarch (PS_Raymarch unchanged).
The CPU regen/upload remains the durable tier (recomposites full overlay on
restream/regen). Toggle: VENPOD_SPARSE_EDIT_LIVE_BAKE (default 1).
