# Engine Architecture

VENPOD is built as a small DirectX 12 engine rather than on top of a game engine. The goal is to make the graphics and GPU-simulation systems visible.

## Main Runtime Flow

The application starts in `src/main.cpp`, opens a Win32 launcher, and then enters either the sand simulator or the sandbox. The public tech-demo path is the sandbox in `src/main_launcher.cpp`.

Each sandbox frame roughly does this:

1. Process SDL input.
2. Move the camera.
3. Stream chunks around the camera.
4. Dispatch GPU raycasts for ground and brush targeting.
5. Run local physics work.
6. Render the voxel world with the raymarching pixel shader.
7. Request small GPU readbacks for next-frame raycast results.
8. Swap the voxel read/write buffers.

## Rendering

The renderer draws a fullscreen triangle and lets `PS_Raymarch.hlsl` trace rays through the voxel grid. The shader receives:

- camera basis vectors
- render buffer dimensions
- region origin in world space
- brush preview information
- a structured buffer containing packed voxel data
- a material palette texture

The render buffer is a moving window. The shader converts world coordinates into buffer-local coordinates by subtracting the region origin.

## Chunk Streaming

Infinite terrain is split into `64 x 64 x 64` chunks. Each generated chunk owns a 1 MB GPU buffer.

`InfiniteChunkManager` handles:

- chunk queueing around the camera
- GPU terrain generation
- allocator and fence reuse for generation commands
- unloading chunks outside the budget

`VoxelWorld` owns the visible render buffer. It copies generated chunks into a `25 x 2 x 25` chunk window centered near the player. The visible window is double-buffered so the renderer can read one buffer while compute work writes the other.

## Physics

Physics is compute-driven and chunk-scoped. `CS_ChunkScanner.hlsl` scans a local physics region around the player and queues chunks that contain movable voxels. `CS_PrepareIndirect.hlsl` prepares indirect dispatch arguments, and `CS_GravityChunk.hlsl` updates active chunks.

Generated terrain and generated ocean water are static by default. Brush-painted materials can still be dynamic.

## Editing

Brush targeting uses `CS_BrushRaycast.hlsl`, which writes a tiny raycast result instead of reading the full voxel buffer back to the CPU. Painting uses `CS_Brush.hlsl` and dispatches only over the brush's bounding box.

This keeps editing local and avoids full-buffer compute work for small brush strokes.

## Important Tradeoffs

The current render-window recenter operation clears copy caches and repopulates the visible buffer. That is simple and robust, but it can still cause a brief hitch if triggered too often. The current snapshot delays recentering until the camera has moved far enough from the window center.

The next major improvement would be a GPU-side rolling-window copy so recentering only moves edge chunks instead of invalidating the whole visible window.
