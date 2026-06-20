# GPU Mid-Mesh Extraction — Promote from SHADOW to PRODUCTION

Goal: move the 14–85ms CPU clipmap mesh rebuilds (MIDMESH_SELFTIME buildMs; the worst dip
spikes, ~18 frames in mtns_edit.rec) onto the idle GPU. The extraction logic is already built +
validated bit-equal to the CPU (Phase B1.1→B1.3f-c). What's missing is production wiring.

## Current state (mapped)
- Shadow extractor: `MidMeshGpuExtractResources` (src/Graphics/MidMeshGpuExtractResources.cpp).
  Uploads dirty tiles' samples+metadata (StageDirtyTiles/EmitCopy), dispatches compute (B13a top →
  B13b risers → B13c skirts → B13d child-suppress → B13e edit-skip → B13f-a LOD merge → B13f-b water
  → B13f-c distance cull), writes faces to an ISOLATED `m_smokeFaceBuffer`, A/Bs vs the CPU
  `meshCacheFaces` in EQUALITY mode. Driven from main_launcher.cpp ~17202-17321, gated by
  `sparseMidMeshGpuExtract*` env flags (all default-off). Per-tile `meta.faceCapacity =
  tile.faceCount + margin` (BORROWS the CPU count — must remove this dependency).
- Production draw: `SparseSurfaceGpuResources` (src/Graphics/SparseSurfaceGpuResources.h). Shared by
  surface + mid-mesh. `m_faceBuffer` + `m_faceRangeAllocator` (SparseSurfaceRangeAllocator) +
  `m_drawArgsBuffer`/`m_drawArgsMirror` (per-tile indirect draw args). CPU path:
  BuildMidHeightSurfaceSnapshot → StageDirtyPayloadSnapshot (allocate ranges + copy faces) → EmitCopy
  → indirect draw (main_launcher.cpp ~17150, ~22714). Face format = SparseSurfaceFace (16B), SHARED
  with the GPU extractor (already validated equal).

## The structural blocker
GPU mesh output size is data-dependent; the range allocator needs faceCount to place a tile's faces.
Two ways to decouple from the CPU mesh:
- (A) Two-pass GPU: pass 1 atomically COUNTS faces/tile → readback → CPU allocates ranges → pass 2
  writes. Adds 1-frame latency (count readback) + 2 dispatches.
- (B) Fixed per-tile face CAPACITY (mid-mesh tiles have a bounded face count for a given mergeCells):
  reserve capacity*tiles in m_faceBuffer, GPU writes faces + an atomic per-tile count into a counts
  buffer, GPU (or a tiny CS) writes the indirect draw args from the counts. No CPU count, no readback.
  Wastes (capacity - actual) face slots; bounded + acceptable. PREFERRED first cut.

## Build order (each step builds + validates before the next)
1. Production face output for the GPU extractor: a per-tile-ranged face buffer (fixed capacity) +
   a per-tile counts buffer; the B13f-c compute writes faces at `tileSlot*capacity + atomicIdx` and
   increments the count. Keep writing the debug buffer too; A/B harness still proves equality. No
   draw/CPU change yet. TEST: GPU_EXTRACT A/B stays equal (extraGpuFaces==0, missingGpuFaces==0).
2. GPU-written indirect draw args: a small CS converts per-tile counts → SparseSurfaceDrawArgs
   (baseFace=tileSlot*capacity, instanceCount=count). TEST: args match the CPU drawArgs for the same
   tiles.
3. Commit gate + GPU mid-mesh DRAW path (flag-gated, parallel to CPU): bind the GPU face buffer +
   GPU draw args; draw GPU-committed tiles from GPU, the rest from CPU. version match =
   meshContentVersion == the version the samples were uploaded at. TEST: visual A/B (capture grids)
   GPU-draw vs CPU-draw identical; PERF: buildMs spikes move off CPU onto gpuFrameMs.
4. Drop the CPU mid-mesh extraction for GPU-committed tiles (BuildMidHeightSurfaceSnapshot skips
   them); CPU stays as fallback for non-committed / frustum-edge / edited tiles. TEST: buildMs p50/max
   collapse; visibleMissing=0; both replays.
5. A/B promote: editing + flythrough, 5 interleaved, gate = no visual diff, buildMs spikes gone,
   no new dips, bounded GPU mem.

## Codex review — implementation requirements (correct-by-construction)
The shadow B1.3/topFace path is NOT reusable as-is; build a distinct production path:
1. FIXED capacity. The debug path uses `baseFace = slot * meta.faceCapacity` with per-tile-varying
   capacity (MidMeshGpuExtractResources.cpp:326) -> overlapping/unstable ranges. Production MUST use a
   single `fixedCapacity` constant: `base = slot * fixedCapacity`. Allocate `maxTiles * fixedCapacity`
   faces + per-slot counts/status buffers (persistent), NOT the one-tile smoke buffer (cpp:457).
2. SEPARATE count-clear pass (GPU race). The extraction shader zeroes faceCount from `dispatchId.x==0`
   with only `AllMemoryBarrierWithGroupSync` (CS_MidMeshExtractTopFaces.hlsl:392/397) — group-local,
   but a 33-side tile dispatches MANY groups, so a late group 0 wipes counts other groups appended.
   Clear the per-slot counts in a dedicated clear pass (host memset-upload, or a clear CS) BEFORE the
   extraction dispatch. Do NOT clear inside the extraction shader.
3. REAL slot indexing. RunB13aTopFaceDispatch hard-codes `ctl.slot=0`, `baseFace=0`, binds smoke
   buffers (cpp:1708/1855). Production must pass the actual tile slot + bind the persistent per-slot
   sample/meta/output/count resources, and write `slot*fixedCapacity + atomicIndex`.
4. PRODUCTION-RANGE A/B. The FULL harness reads the debug buffer from offset 0 (cpp:1893/2039).
   Step-1 validation must compare each dirty tile's CPU `meshCacheFaces` against
   `productionFaces[slot*fixedCapacity .. +count]`.
5. OVERFLOW = hard report, never silent truncate. If a tile's face count exceeds fixedCapacity, log it
   (so we size up) and fall that tile back to CPU.

## Invariants
- CPU extractTileMesh stays the authoritative reference + the fallback. Never ship a hole: a tile
  whose GPU faces aren't committed/ready draws from CPU.
- No image change: the GPU mesh is bit-equal (already proven); the draw must be pixel-identical.
- Edited tiles: the GPU extractor's B13e edit-skip already matches the CPU edit-footprint skip; the
  live edit renders via the voxel raymarch there (unchanged). Editing dips are surfExtract (separate,
  near-voxel) — NOT addressed by this; this targets the mid-mesh clipmap rebuild dips.

## Note on the bigger lever
surfExtract (near-voxel surface, ~3.8ms EVERY frame, sum 2354) is the median/baseline cost and would
need a SEPARATE GPU surface extractor (unbuilt). This mid-mesh promotion kills the worst STUTTERS
(14-85ms) and is the proven, tractable first GPU migration + the template for the surface one.
