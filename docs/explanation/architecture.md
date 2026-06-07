# Engine Architecture

VENPOD is a small DirectX 12 voxel engine tech demo. It is intentionally built
close to the graphics API so the rendering, streaming, synchronization, and GPU
simulation systems are visible in the repository.

## Runtime Flow

The application starts in `VENPOD/src/main.cpp`, opens the launcher, and then
enters either the sand simulator or the sandbox. The public tech-demo path is
the sandbox in `VENPOD/src/main_launcher.cpp`.

Each sandbox frame roughly does this:

1. Process SDL input.
2. Move the player/camera in stable world coordinates.
3. Plan sparse brick residency for camera, movement, brush intent, feedback,
   collision, and near-ownership holes.
4. Generate sparse terrain bricks and replay persistent edit overlays.
5. Stage brick, occupancy, surface, mid-clipmap, and page-table uploads under
   frame/byte budgets.
6. Run sparse raycast/brush feedback and local dirty-region physics.
7. Render sparse surfaces first, then fill remaining pixels with controlled
   mid/far background layers.
8. Retire GPU readbacks for raycast, miss feedback, ownership, brush feedback,
   physics proposals, and diagnostics.

## Coordinate Spaces

VENPOD uses several coordinate spaces, and keeping them separate is central to
the current stability work.

| Space | Meaning |
| --- | --- |
| World space | Stable player, camera, chunk, brush, and edit positions |
| Chunk space | Signed `64 x 64 x 64` chunk coordinates |
| Sparse brick space | Signed `16 x 16 x 16` world-brick coordinates |
| Local voxel space | Voxel coordinate inside one chunk |
| Sparse local space | Voxel coordinate inside one sparse brick |
| Render-local space | Position inside the moving dense render buffer |
| Screen/camera space | Ray generation and fullscreen rendering |

Sparse gameplay uses stable world brick coordinates. The legacy render buffer
can still recenter, but player/camera world positions should not be mutated by
that compatibility path.

## Rendering

The sparse path renders near-field voxel surfaces as rasterized extracted
quads. The sparse surface pass writes depth/stencil, then
`VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` shades only the remaining
background pixels for mid voxel/height clipmaps, far SVO, sky, and controlled
fallbacks. The renderer receives:

- camera basis vectors
- sparse page table, brick pool, occupancy, and surface buffers
- mid clipmap residency and far SVO ownership metadata
- brush preview information
- third-person character position
- material palette texture

The dense render buffer still exists as the legacy renderer and as a small
compatibility shim in sparse runtime mode.

## Chunk Streaming

The active sparse path generates deterministic `16 x 16 x 16` bricks in stable
world coordinates. A CPU mirror owns lifecycle state; the GPU page table is only
the published visibility snapshot.

The dense legacy path still splits terrain into `64 x 64 x 64` chunks. Each
generated chunk owns a 1 MB GPU buffer.

`InfiniteChunkManager` handles:

- chunk queueing around the camera
- GPU terrain generation
- allocator and fence reuse for generation commands
- unloading chunks outside the budget
- deferred cleanup for GPU-owned resources

`SparseVoxelWorld` owns sparse requests, generation, uploads, edit overlays,
surface extraction, page-table invalidations, and residency metrics. `VoxelWorld`
owns the legacy dense render buffers.

## Sparse Page Publication

The current sparse upload path uses the direct graphics queue. Brick payload and
occupancy copies are ordered on that queue before the matching GPU page-table
entry is published. The CPU brick pool is the authoritative residency owner; the
GPU page table is a delayed visibility snapshot that shaders may use only after
the publish queue releases the entry.

`SparsePagePublishQueue` stores the page-table entry index, brick coordinate,
physical page, generation, residency class, ready frame, and ready fence. It
withholds publication until both frame and fence readiness are satisfied, drops
stale pending publishes whose CPU entry no longer matches, and lets newer
generations replace older pending entries for reused slots. Runtime delay
knobs exercise this path in regression without introducing a separate copy
queue.

A future dedicated copy queue would need explicit queue ownership and fence
plumbing for each payload and page-table publication. Until that work exists,
R-028 is validated only for the current direct-queue async publication contract,
not for independent cross-queue uploads.

## Terrain

The current terrain is designed as a vertical traversal sandbox. The conceptual
terrain range is `Y = -332` to `Y = 664`, but VENPOD does not allocate that full
height densely. It streams a local vertical window around the player.

The terrain generator combines broad land forms, ridged mountains, needle
spires, ravines, terraces, cave/cavern masks, low basin water, and material
variation. The fields are sampled in world coordinates so chunk borders remain
coherent.

## Editing

Sparse brush targeting uses world-space sparse raycasts, with GPU sparse
raycast and brush feedback available as diagnostics and guarded apply paths.
Brush edits are stored as sparse per-brick overlays and replayed over generated
terrain when bricks are regenerated or reuploaded.
GPU brush feedback apply only commits payloads whose header and sentinel records
both report zero missing residents, with no overflow and no duplicate edit
coordinates. Missing, truncated, duplicate, or stale feedback falls back to CPU
sparse authority.

This makes painting useful as a traversal mechanic: bridges, ramps, stairs,
platforms, and tunnels can survive render-window streaming during the session.

## Physics

Sparse local physics is dirty/active-region based and default-on in sparse
runtime mode. GPU physics packet upload/readback can propose moves, but CPU
validation guards page generation, exact edit-revision parity, edit-delta
status consistency, malformed local coordinates, destination residency, and
same-batch conflicts before accepting a proposal.

Relevant shaders:

- `CS_SparseRaycast.hlsl`
- `CS_SparseBrushFeedback.hlsl`
- `CS_SparseMissFeedback.hlsl`
- `CS_SparsePhysicsPackets.hlsl`
- `CS_ChunkScanner.hlsl`
- `CS_PrepareIndirect.hlsl`
- `CS_GravityChunk.hlsl`

## Sparse Layers

The near layer is editable sparse bricks. The mid layer is a clipmap with height
and coarse voxel residency metadata. The far layer is an async GPU-backed SVO
plus controlled far fallback. Ownership counters track near/surface, mid voxel,
mid height, far SVO, far height, sky, miss, and unsafe near miss pixels.

## Important Tradeoffs

- Dense legacy remains useful as a regression fallback, but the sparse path is
  the architecture intended to replace it.
- Far terrain is approximate and visual; near sparse bricks remain the editable
  and collision-critical authority.
- Brush edits persist during a session and can be saved/loaded with
  `.\rebrun.ps1 -SparseEditFile` or the pause-menu metrics panel.
- The engine favors explicit DirectX 12 systems over middleware so interviewers
  can inspect the low-level rendering and synchronization work.
