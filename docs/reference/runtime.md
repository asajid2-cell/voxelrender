# Runtime Reference

## Modes

The launcher exposes two modes:

- `Sandbox Mode`: infinite vertical terrain explorer and main public tech demo
- `Sand Simulator`: smaller material-physics sandbox

## Controls

| Input | Action |
| --- | --- |
| `WASD` | Move |
| Mouse | Look |
| `Space` | Jump |
| Double-tap `Space` | Toggle flight mode |
| `Space` in flight mode | Fly up |
| `Shift` in flight mode | Fly down |
| `V` | Toggle first-person / third-person camera |
| Left mouse | Paint |
| Right mouse | Erase |
| `Q` / `E` | Previous / next material |
| `[` / `]` | Decrease / increase brush radius |
| `Tab` | Toggle mouse capture |
| `Esc` | Pause menu; the metrics panel is editable while paused, including sparse edit save/load |

## Runtime Environment Variables

| Variable | Effect |
| --- | --- |
| `VENPOD_MODE=sandbox` | Launches directly into the sandbox path when supported by the launcher |
| `VENPOD_DIAGNOSTICS=1` | Enables debug logs and writes `build/bin/venpod_runtime.log` |
| `VENPOD_LOG_FILE=1` | Writes the runtime log without enabling all diagnostics |
| `VENPOD_D3D_DEBUG=1` | Enables DirectX 12 debug layer and GPU validation |
| `VENPOD_DISABLE_PHYSICS=1` | Skips physics dispatches |
| `VENPOD_ENABLE_INFINITE_PHYSICS=1` | Enables the experimental infinite-world physics path |
| `VENPOD_DISABLE_FAR_SVO=1` | Disables the sparse far-field SVO path |
| `VENPOD_STATIC_CHUNKS=1` | Uses a fixed chunk patch for streaming isolation |
| `VENPOD_BOUNDARY_TEST=1` | Enables the boundary/recenter movement diagnostic path |
| `VENPOD_ENABLE_EXPERIMENTAL_SPARSE=1` | Allows the sparse renderer/runtime path |
| `VENPOD_RENDER_BACKEND=sparse` | Requests the sparse brick backend |
| `VENPOD_SPARSE_RAYMARCH=1` | Enables sparse shader bindings and background sparse raymarch diagnostics |
| `VENPOD_SPARSE_ONLY=1` | Uses sparse runtime test mode instead of legacy dense streaming |
| `VENPOD_SPARSE_SURFACE_AUTHORITATIVE=1` | Makes sparse raster surfaces the authoritative near-field renderer |
| `VENPOD_SPARSE_MISS_FEEDBACK=1` | Enables GPU missing-brick feedback planning |
| `VENPOD_SPARSE_GPU_RAYCAST=1` | Enables the GPU sparse raycast path |
| `VENPOD_SPARSE_BRUSH_FEEDBACK=1` | Enables GPU sparse brush feedback readback |
| `VENPOD_ENABLE_SPARSE_PHYSICS=1` | Enables local sparse physics when sparse runtime mode is active |
| `VENPOD_SPARSE_PHYSICS_GPU=1` | Enables GPU sparse physics packet upload/readback |
| `VENPOD_SPARSE_PHYSICS_GPU_APPLY=1` | Allows guarded CPU application of GPU physics proposals |
| `VENPOD_SPARSE_EDIT_FILE=path` | Loads sparse edit overlays on startup and saves them on shutdown; `.\rebrun.ps1 -SparseEditFile path` sets this for normal runs, and relative paths resolve from the `VENPOD/` project root when launched by the scripts |
| `VENPOD_EXIT_AFTER_FRAMES=N` | Exits automatically after N frames for smoke tests |

## Key Constants

| Value | Meaning |
| --- | --- |
| `16 x 16 x 16` | Sparse near-field brick dimensions |
| `4096` pages | Default sparse brick page-pool capacity in current smoke runs |
| `1024+` entries | Sparse page table capacity, configurable by environment |
| `64 x 64 x 64` | Infinite chunk voxel dimensions |
| 1 MB | GPU buffer size per generated chunk |
| `19 x 7 x 19` | Dense editable render window in chunks |
| 2,527 chunks | Dense visible render-window chunk capacity |
| `1216 x 448 x 1216` | Dense voxel render-buffer dimensions |
| 662,437,888 voxels | Dense voxel render-buffer capacity per buffer |
| `Y = -332..664` | Conceptual generated vertical terrain range |
| `+/-14` chunks | Horizontal background load distance |
| `5 below / 3 above` chunks | Vertical background load window around player |
| `+/-18` chunks | Horizontal unload distance |
| `6 below / 4 above` chunks | Vertical unload window |
| 81 pages | Observed default far SVO page count for the current cached dataset |
| 1,910,633 nodes | Observed default far SVO node count for the current cached dataset |
| 1024 voxels | Far SVO root page size |

## Important Files

| Path | Role |
| --- | --- |
| `VENPOD/src/main_launcher.cpp` | Sandbox runtime loop |
| `VENPOD/src/Graphics/Renderer.cpp` | Fullscreen raymarch render pass setup |
| `VENPOD/src/Graphics/SparseVoxelGpuResources.cpp` | Sparse brick, occupancy, upload, and page-table GPU resources |
| `VENPOD/src/Graphics/SparseSurfaceGpuResources.cpp` | Sparse surface payload, metadata, indirect draw, and culling resources |
| `VENPOD/src/Graphics/FarVoxelOctree.cpp` | GPU-backed visual far-field SVO builder |
| `VENPOD/src/Simulation/SparseVoxelWorld.cpp` | Sparse brick lifecycle, generation, edits, upload queues, and residency metrics |
| `VENPOD/src/Simulation/SparseBrickPool.cpp` | Sparse physical page allocator and page-table consistency owner |
| `VENPOD/src/Simulation/SparsePagePublishQueue.cpp` | Fence/frame-gated sparse page-table publication queue |
| `VENPOD/src/Simulation/SparseRuntimeBudget.cpp` | Runtime budget, pressure, feedback, upload, and physics scheduling policies |
| `VENPOD/src/Simulation/SparseClipmap.cpp` | Mid height/voxel clipmap generation and residency metadata |
| `VENPOD/src/Simulation/TerrainConstants.h` | Shared terrain and streaming constants |
| `VENPOD/src/Simulation/VoxelWorld.cpp` | Dense voxel buffers, chunk copy path, edit overlays |
| `VENPOD/src/Simulation/InfiniteChunkManager.cpp` | Chunk queueing, generation, and unloading |
| `VENPOD/src/Simulation/PhysicsDispatcher.cpp` | Compute dispatch orchestration |
| `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` | Dense voxel and far SVO raymarching shader |
| `VENPOD/assets/shaders/Compute/CS_GenerateChunk.hlsl` | Procedural chunk generation |
| `VENPOD/assets/shaders/Compute/CS_Brush.hlsl` | Brush painting compute shader |
| `VENPOD/assets/shaders/Compute/CS_BrushRaycast.hlsl` | GPU brush and ground raycast shader |
| `VENPOD/assets/shaders/Compute/CS_GravityChunk.hlsl` | Chunk-local physics shader |

## Scripts

| Script | Role |
| --- | --- |
| `VENPOD/setup.ps1` | Configure dependencies and build from a fresh checkout |
| `VENPOD/build.ps1` | Build the configured project |
| `VENPOD/run.ps1` | Run the built executable with the current environment |
| `VENPOD/rebrun.ps1` | Rebuild and run the sparse sandbox by default; use `-DenseLegacy` for the old renderer or `-SparseEditFile saves\name.vsed` for sparse edit persistence |
| `VENPOD/clean.ps1` | Remove generated build files |
| `VENPOD/sparse_regression.ps1` | Combined sparse smoke/regression gate |
| `VENPOD/engine_capture_smoke.ps1` | Engine backbuffer capture smoke test |
