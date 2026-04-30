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
3. Stream chunks around the camera.
4. Dispatch GPU raycasts for ground and brush targeting.
5. Run budgeted local physics work.
6. Apply brush edits to the current dense voxel buffers.
7. Render with the fullscreen HLSL raymarch pass.
8. Queue compact GPU readbacks for next-frame raycast and brush feedback.
9. Swap voxel read/write buffers when copy fences allow it.

## Coordinate Spaces

VENPOD uses several coordinate spaces, and keeping them separate is central to
the current stability work.

| Space | Meaning |
| --- | --- |
| World space | Stable player, camera, chunk, brush, and edit positions |
| Chunk space | Signed `64 x 64 x 64` chunk coordinates |
| Local voxel space | Voxel coordinate inside one chunk |
| Render-local space | Position inside the moving dense render buffer |
| Screen/camera space | Ray generation and fullscreen rendering |

The render buffer can recenter. Player and camera world positions should not be
mutated by recentering. The shader samples the dense buffer by subtracting the
active render origin from world-space ray positions.

## Rendering

The renderer draws a fullscreen triangle and lets
`VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` trace rays through the dense
voxel buffer. The shader receives:

- camera basis vectors
- render buffer dimensions
- region origin in world space
- brush preview information
- third-person character position
- packed voxel buffer SRV
- material palette texture
- optional sparse far-field node/page buffers

The dense render buffer is `1216 x 448 x 1216` voxels, equivalent to a
`19 x 7 x 19` chunk window.

## Chunk Streaming

Infinite terrain is split into `64 x 64 x 64` chunks. Each generated chunk owns
a 1 MB GPU buffer.

`InfiniteChunkManager` handles:

- chunk queueing around the camera
- GPU terrain generation
- allocator and fence reuse for generation commands
- unloading chunks outside the budget
- deferred cleanup for GPU-owned resources

`VoxelWorld` owns the visible dense render buffers. It copies generated chunks
into the moving render window centered near the player. The visible buffers are
double-buffered so the renderer can read one buffer while compute work writes
the other.

## Terrain

The current terrain is designed as a vertical traversal sandbox. The conceptual
terrain range is `Y = -332` to `Y = 664`, but VENPOD does not allocate that full
height densely. It streams a local vertical window around the player.

The terrain generator combines broad land forms, ridged mountains, needle
spires, ravines, terraces, cave/cavern masks, low basin water, and material
variation. The fields are sampled in world coordinates so chunk borders remain
coherent.

## Editing

Brush targeting uses `CS_BrushRaycast.hlsl`, which writes a tiny raycast result
instead of reading the full voxel buffer back to the CPU.

Painting uses `CS_Brush.hlsl` and dispatches over the brush bounds. GPU brush
feedback records compact edit events asynchronously, and `VoxelWorld` stores
those edits in sparse per-chunk overlays. When a chunk is copied back into the
dense render buffer, its overlay is replayed on top of generated terrain.

This makes painting useful as a traversal mechanic: bridges, ramps, stairs,
platforms, and tunnels can survive render-window streaming during the session.

## Physics

Physics is compute-driven and chunk-budgeted. The infinite-world path avoids
full vertical-buffer scans by default. Candidate chunks are processed locally
and conservatively so the public sandbox remains responsive.

Relevant shaders:

- `CS_ChunkScanner.hlsl`
- `CS_PrepareIndirect.hlsl`
- `CS_GravityChunk.hlsl`

## Sparse Far Field

`FarVoxelOctree` builds a GPU-backed page/node forest for visual far terrain.
The raymarch shader binds those structured buffers as a read-only far-field
representation.

The SVO path is not gameplay-authoritative in this checkpoint. Collision,
brushes, persistent edits, and physics still use the dense streamed world.

## Important Tradeoffs

- The dense render buffer is intentionally bounded; larger distance should come
  from far-field LOD rather than only increasing dense voxel memory.
- Far SVO terrain is visual-only for now.
- Brush edits persist during a session, but the public path does not yet ship
  disk save/load for edited chunks.
- The engine favors explicit DirectX 12 systems over middleware so interviewers
  can inspect the low-level rendering and synchronization work.
