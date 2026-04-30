# Sparse Voxel Octree Far-Field Plan

VENPOD currently renders an editable dense near-field voxel window. That path
is good for painting, collision, physics, and persistence, but it scales by
volume. Increasing dense render distance from `+/-7` to `+/-14` would require
roughly four times the horizontal memory, before source chunks and transient
GPU work.

The sparse voxel octree direction is for visual far distance first. The dense
near field remains the source of truth for gameplay.

## Implemented GPU Node/Page Pass

The current implementation has moved past the implicit shader prototype. The
sandbox now builds a GPU-backed sparse far-field tree at startup:

- `FarVoxelOctree` builds a square forest of far pages around origin.
- Each page is a 1024-voxel root cube with an 8-child sparse tree below it.
- Child root nodes are packed contiguously, so the shader can traverse with
  `childBase + popcount(childMask before ordinal)`.
- Interior cells that are safely below sampled terrain are collapsed into
  coarse leaves, while surface/ravine/cliff cells are subdivided down to the
  configured depth.
- Node and page data are uploaded as structured buffers and bound to the
  fullscreen raymarch pass as `t2` and `t3`.
- `FrameConstants.farFieldParams` enables the pass and reports page count,
  node count, and page size to the shader.
- The diagnostics overlay reports the far SVO state, page count, node count,
  page size, and covered world size.

The first measured startup build produced 81 pages and 1,910,633 nodes, covering
9216 world units horizontally. This is a visual far-field representation only.
The dense streaming voxel buffer remains authoritative for editing, collision,
physics, raycast, and persistence.

## Why This Version Is Still Isolated

A gameplay-authoritative SVO/DAG renderer still needs additional
infrastructure:

- CPU or GPU construction of far-field nodes
- a compact node layout
- descriptor and buffer lifetime management
- camera-centered streaming of node pages
- invalidation when terrain generation changes
- optional far-field material/normal mips
- clear separation from brush/collision/readback systems

Adding that authority directly into the existing dense renderer would risk the
currently stable demo. The GPU node/page pass validates real buffer ownership,
root-signature binding, and tree traversal while keeping gameplay semantics in
the known-good dense path.

## Target Architecture

The target split is:

- dense editable near field:
  painting, physics, raycast, collision, persistence, high-frequency detail
- sparse visual far field:
  read-only terrain silhouette, far material color, fogged LOD, no gameplay
  authority

The sparse far field should eventually be one of:

- sparse voxel octree
- sparse voxel DAG for repeated/static terrain
- brick map with per-brick mips and a top-level BVH/octree

For VENPOD's terrain, a brick map plus octree/BVH top level is likely the most
practical next step. The terrain is chunked already, and brick pages match the
streaming model better than one monolithic tree.

## Proposed Node Layout

Candidate GPU node:

```cpp
struct FarVoxelNode {
    uint32_t childBase;      // first child index, or 0xFFFFFFFF for leaf
    uint32_t childMask;      // 8-bit occupancy mask
    uint32_t materialPacked; // dominant material / palette index / flags
    uint32_t boundsPacked;   // quantized origin/level or page-local index
};
```

Candidate page:

```cpp
struct FarVoxelPage {
    int32_t originChunkX;
    int32_t originChunkY;
    int32_t originChunkZ;
    uint32_t rootNode;
};
```

## Integration Rules

- Never use far SVO for brush hit tests until exact coordinate and edit replay
  semantics are implemented.
- Never use far SVO for collision or ground snapping.
- Never draw far SVO through a ray that already traversed the dense editable
  AABB and found air.
- Fog and dither transitions should hide the dense/far boundary.
- Far SVO pages should be allowed to lag behind; dense near-field streaming
  remains higher priority.

## Runtime Controls

- Default: far SVO enabled.
- `VENPOD_DISABLE_FAR_SVO=1`: disables the node/page far field and falls back to
  the older procedural far terrain path.

## Verification

Release build succeeded. A diagnostics runtime smoke test confirmed:

- `PS_Raymarch.hlsl` compiled with the expanded root signature.
- the far voxel octree initialized successfully
- dense chunk generation, copy, edit, brush raycast, and physics shaders still
  compiled
- no critical/error/failed/device-removed log entries appeared during the smoke
  run

## Known Limitations

- The far SVO is static around origin; it is not camera-centered streamed yet.
- The node data is built from a CPU approximation of the far terrain function,
  not directly from generated chunk buffers.
- It is read-only visual terrain. It does not participate in brush edits,
  persistence, collision, ground snapping, or physics.
- The page buffers use an upload heap for a simple safe first integration.
  A production version should upload once into default GPU memory.
- The shader still keeps the older procedural far terrain fallback for rays that
  miss the SVO coverage. That is useful for continuity, but a future pass should
  replace it with streamed pages or a top-level page accelerator.

## Next Implementation Pass

1. Move far nodes/pages into default GPU buffers through a copy upload path.
2. Add a top-level page grid accelerator so the shader does not test every page
   for every far-field ray.
3. Stream/rebuild page rings around the camera instead of keeping a static
   origin-centered page forest.
4. Generate far pages from the same terrain source used by chunk generation.
5. Add far-field counters for rays tested, pages tested, tree nodes visited,
   hits, misses, and fallback hits.
6. Add optional low-resolution material/normal mips for smoother far silhouettes.
