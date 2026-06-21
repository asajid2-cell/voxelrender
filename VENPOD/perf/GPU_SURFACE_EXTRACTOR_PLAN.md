# GPU Surface Extractor — the median lever (next after mid-mesh)

Target: `surfExtract` ~3.8ms EVERY frame (sum 2354 over the editing replay) -- the near-voxel
streaming surface mesh, the #1 reason the median sits at ~74fps instead of 120. Move it to the GPU
the SAME way the mid-mesh migration did (which is now the proven template + reusable machinery).

## What it is (CPU side, mapped)
- Unit: `SparseSurfaceExtractor::Extract(brick, neighborSampler)` (src/Simulation/SparseSurfaceExtractor.cpp).
  Per-brick: for each voxel in the SPARSE_BRICK_SIZE^3 brick, for each of 6 directions
  (kDirections, NegX..PosZ), if the neighbor voxel is air -> emit a SparseSurfaceFace in that
  direction. Boundary voxels use `neighborSampler` (the adjacent brick or terrain). No-edit path =
  `ExtractSurfaceNoEditWithTerrain` (terrain generator, overlay-free -> GPU-safe, same constraint as
  the mid-mesh GPU path).
- Driven from `PumpSurfaceExtractionAround*` over `m_pendingSurfaceBricks` (the dirty/pending brick
  list); already parallel on CPU threads (the promoted parallelSurfaceExtraction win). The GPU just
  moves that parallel per-brick work onto the idle GPU.
- Face format = SparseSurfaceFace (16B), SHARED -- same A/B harness style as mid-mesh.
- CHECK FIRST: whether Extract does GREEDY MESHING (merging coplanar adjacent faces into wider quads)
  or one-face-per-voxel-face. If greedy, the GPU shader must replicate it EXACTLY for bit-equality
  (or we A/B as a multiset and accept different merge granularity -> would change face count, so must
  replicate). Read SparseSurfaceExtractor.cpp fully before writing the shader.

## Reused machinery (from the mid-mesh migration, already built)
- Production face buffer (per-slot fixed-capacity) + per-slot atomic counts + overflow status, with a
  SEPARATE count-clear pass (the race fix). -> mirror for surface bricks (per-brick-slot).
- GPU compact-copy into the existing draw ranges + a CS that builds indirect draw args from counts
  (so the existing surface indexed draw is reused unchanged).
- Commit gate (overflow==0 + version match) + CPU fallback (edited/overflow/new -> CPU/voxel-owned).
- VENPOD_*_GPU_OWN style flag; CPU `PumpSurfaceExtraction` SKIPS GPU-owned bricks.

## Build order (mirror the mid-mesh steps, each A/B-gated)
1. GPU surface extract compute shader: thread-per-voxel (or per-brick group), 6-neighbor air test
   using the brick voxels + uploaded neighbor samples, atomic-append faces to a per-brick-slot
   production buffer + count. A/B each dirty brick's CPU faces vs GPU multiset == 0 extra/0 missing.
   (Validate the NO-EDIT path first, like mid-mesh B1.3.)
2. Build indirect draw args from counts; compact-copy committed bricks into the surface draw ranges.
3. Flag-gated GPU surface DRAW; visual A/B pixel-identical (capture, like mid-mesh: terrain diff
   collapses to capture noise). visibleMissing=0.
4. GPU_OWN: CPU PumpSurfaceExtraction skips GPU-owned bricks; overflow/edit/new -> CPU fallback,
   never a hole. Measure surfExtract p50/p99 collapse + body p50 (the 74fps->? median win).
5. A/B promote (editing + flythrough, interleaved): body p50 drops, surfExtract leaves the CPU,
   visibleMissing=0, visual identical, bounded GPU mem.

## Why this is the real 120fps lever
Mid-mesh promotion killed the 14-85ms STUTTERS (18 frames). This surface promotion attacks the
EVERY-FRAME ~3.8ms median cost -- the dominant CPU sink keeping the median at 74fps. The neighbor
sampling at brick borders is the main new wrinkle (the GPU needs adjacent-brick samples uploaded, or
a global voxel SRV it can sample); design that explicitly. Per-brick face count is bounded
(<= 3 * BRICK^3 faces worst case) -> fixed-capacity sizing is clean.
