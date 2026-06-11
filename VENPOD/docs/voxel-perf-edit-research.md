# Voxel perf + real-time editing/painting — SOTA research (2026-06-11)

Deep fan-out (103-agent verified, 24/25 claims confirmed). Two focuses; editing/painting is PRIORITY.

## PRIORITY — real-time voxel editing/painting without re-checking the whole world
The convergent answer: LOCALIZE edits to touched bricks, and DECOUPLE paint (material/color) from geometry.

- **R1 (highest payoff, low complexity) — DIRTY-BRICK TRACKING.** Stop rebuilding the page table +
  re-uploading the full sample buffer + re-extracting the full surface every brush tick. Mark only the
  bricks a brush touches (+ face-sharing neighbors) dirty; re-extract/re-upload ONLY those. The single
  change that removes the global rescan. (Teardown per-volume localization; universal canonical fix.)
- **R2 (high payoff, low complexity) — PAINT-AS-ATTRIBUTE-DELTA.** Store paint/material in a SEPARATE
  sparse attribute buffer (color/roughness/material-id) sampled at raymarch+surface time; a paint stroke
  writes only this buffer and NEVER touches geometry or triggers meshing. Zero re-extraction for
  color/material edits. Validated by HashDAG (CGF 2020, attributes decoupled, Morton-indexed), Molenaar &
  Eisemann 2023 (CGF 14757, independent attribute compression), Teardown (8-bit palette-index remap via
  precomputed translation tables — paint is a lookup, never structural), Aokana (separate color stream).
- **R3 (medium) — GPU-SIDE LOCALIZED EDITS.** Dispatch the brush as a compute shader over only the
  affected brick(s), writing edits directly into the GPU brick pool, NO CPU readback/round-trip stall.
- **R4 (high ceiling, high complexity) — EDITABLE COMPRESSED STRUCTURE (HashDAG / GPU-SVDAG-Editing 2024).**
  Two-phase build-temp-SVO-then-merge GPU edit + decoupled compressed attributes; real-time 3D painting,
  no CPU readback, (131072)^3 scale. STRUCTURAL REDESIGN (away from brick-pool+extracted-mesh) — long-horizon.

## FOCUS 1 — mesh-mid + engine speed
- **R5 (highest for mesh-mid) — GPU CLUSTER CULLING.** Meshletize the terraced mid mesh; amplification
  shader does per-meshlet frustum + normal-cone (backface) culling using precomputed bounds (sphere +
  normal-cone 4-tuple), dispatches mesh shaders only for visible meshlets. Attacks the 900k-2M unculled
  faces (D3D12 MeshletCull sample; the near surface already has cluster culling — extend to the mid).
- **R6 — GEOMETRY-CLIPMAP LOD/STREAMING.** Incremental L-shaped (toroidal '+') boundary updates +
  per-ring screen-space-error LOD instead of full-snapshot rebuilds (Asirvatham/Hoppe GPU Gems 2 Ch.2).
  = the mesh-mid incremental upload (re-terrace only the boundary ring, not the full mesh).
- R7 — HiZ occlusion + indirect-draw compaction; raymarch accel (empty-space skip / hierarchical DDA) far field.

## Key sources
HashDAG (Careil/Billeter/Eisemann CGF 2020, DOI 10.1111/cgf.13916; github.com/Phyronnaz/HashDAG);
Molenaar & Eisemann CGF 2023 (10.1111/cgf.14757) + Pacific Graphics 2024 GPU-SVDAG-Editing
(github.com/mathijs727/GPU-SVDAG-Editing); Teardown dev blog (blog.voxagon.se); Aokana (arXiv 2505.02017);
D3D12 MeshletCull (microsoft/DirectX-Graphics-Samples); geometry clipmaps (NVIDIA GPU Gems 2 Ch.2).
