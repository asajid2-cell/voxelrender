# Sparse Voxel Octree Far-Field Plan

VENPOD currently renders an editable dense near-field voxel window. That path
is good for painting, collision, physics, and persistence, but it scales by
volume. Increasing dense render distance from `+/-7` to `+/-14` would require
roughly four times the horizontal memory, before source chunks and transient
GPU work.

The sparse voxel octree direction is for visual far distance first. The dense
near field remains the source of truth for gameplay.

## Current First Pass

The first implementation is an implicit shader-side sparse voxel octree over
the existing far procedural terrain silhouette.

It adds:

- coarse far-field cells
- cell occupancy tests against sampled terrain height bounds
- hierarchical descent for occupied cells
- empty-cell skipping for air cells
- horizon-only rendering so the far field does not leak through the dense
  editable window

This is not a persistent GPU node pool yet. It is a safe prototype of the
traversal model: the ray shader asks whether large spatial cells are empty and
skips them before refining toward smaller occupied cells.

## Why This Version First

A full SVO/DAG renderer needs new infrastructure:

- CPU or GPU construction of far-field nodes
- a compact node layout
- descriptor and buffer lifetime management
- camera-centered streaming of node pages
- invalidation when terrain generation changes
- optional far-field material/normal mips
- clear separation from brush/collision/readback systems

Adding all of that directly into the existing dense renderer would risk the
currently stable demo. The implicit SVO pass lets us validate traversal and
visual behavior before adding persistent node streaming.

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

## Next Implementation Pass

1. Add a `FarVoxelField` runtime object owned by the sandbox/renderer layer.
2. Generate low-resolution brick occupancy from the same terrain source as
   chunk generation.
3. Upload a compact node/page buffer.
4. Bind that buffer as `t2` in the fullscreen raymarch root signature.
5. Replace the implicit shader occupancy test with actual node traversal.
6. Add overlay metrics:
   node count, page count, far rays tested, far hits, empty skips, traversal
   steps, upload budget, and far-field memory.

## Known Limitation Of Current Pass

The current implicit SVO uses the far procedural height approximation. It is a
visual acceleration prototype, not an exact representation of generated chunks.
It should improve the shape of the architecture and make far-distance tests
cheaper, but it does not yet solve persistent far edits or exact far terrain.
