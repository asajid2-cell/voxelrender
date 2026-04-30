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
| `Esc` | Pause menu |

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

## Key Constants

| Value | Meaning |
| --- | --- |
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
| 81 pages | Default far SVO page count at this checkpoint |
| 1,910,633 nodes | Observed default far SVO node count at this checkpoint |
| 1024 voxels | Far SVO root page size |

## Important Files

| Path | Role |
| --- | --- |
| `VENPOD/src/main_launcher.cpp` | Sandbox runtime loop |
| `VENPOD/src/Graphics/Renderer.cpp` | Fullscreen raymarch render pass setup |
| `VENPOD/src/Graphics/FarVoxelOctree.cpp` | GPU-backed visual far-field SVO builder |
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
| `VENPOD/run.ps1` | Run the built executable |
| `VENPOD/rebrun.ps1` | Rebuild and run the sandbox in one step |
| `VENPOD/clean.ps1` | Remove generated build files |
