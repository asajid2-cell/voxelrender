# Runtime Reference

## Modes

The launcher exposes two modes:

- `Sandbox Mode`: infinite terrain explorer and main public tech demo
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
| Left mouse | Paint |
| Right mouse | Erase |
| `Q` / `E` | Previous / next material |
| `[` / `]` | Decrease / increase brush radius |
| `Tab` | Toggle mouse capture |
| `Esc` | Pause menu |

## Runtime Environment Variables

| Variable | Effect |
| --- | --- |
| `VENPOD_DIAGNOSTICS=1` | Enables debug logs and writes `build/bin/venpod_runtime.log` |
| `VENPOD_LOG_FILE=1` | Writes the runtime log without enabling all diagnostics |
| `VENPOD_D3D_DEBUG=1` | Enables DirectX 12 debug layer and GPU validation |
| `VENPOD_DISABLE_PHYSICS=1` | Skips physics dispatches |
| `VENPOD_STATIC_CHUNKS=1` | Uses a fixed chunk patch for streaming isolation |

## Key Constants

| Value | Meaning |
| --- | --- |
| `64 x 64 x 64` | Infinite chunk voxel dimensions |
| 1 MB | GPU buffer size per generated chunk |
| `25 x 2 x 25` | Visible render window in chunks |
| 1,250 chunks | Visible render buffer capacity |
| `33 x 2 x 33` | Loaded-world budget around the player |
| 2,178 chunks | Loaded-world chunk budget |
| `32 x 8 x 32` | Local physics scan region in physics chunks |

## Important Files

| Path | Role |
| --- | --- |
| `VENPOD/src/main_launcher.cpp` | Sandbox runtime loop |
| `VENPOD/src/Graphics/Renderer.cpp` | Fullscreen raymarch render pass |
| `VENPOD/src/Simulation/VoxelWorld.cpp` | Visible voxel buffers and chunk copy path |
| `VENPOD/src/Simulation/InfiniteChunkManager.cpp` | Chunk queueing, generation, and unloading |
| `VENPOD/src/Simulation/PhysicsDispatcher.cpp` | Compute dispatch orchestration |
| `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` | Voxel raymarching shader |
| `VENPOD/assets/shaders/Compute/CS_GenerateChunk.hlsl` | Procedural chunk generation |
| `VENPOD/assets/shaders/Compute/CS_Brush.hlsl` | Brush painting compute shader |
| `VENPOD/assets/shaders/Compute/CS_GravityChunk.hlsl` | Chunk-local physics shader |
