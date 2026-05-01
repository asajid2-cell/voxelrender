# VENPOD

VENPOD is a DirectX 12 voxel engine tech demo written in C++20 and HLSL. It
renders an infinite first-person voxel sandbox with GPU-generated terrain,
chunk streaming, HLSL raymarching, persistent brush editing, voxel physics,
ImGui diagnostics, and an experimental sparse voxel octree far-field renderer.

The project is built to show low-level graphics and simulation engineering:
descriptor management, command queues, resource barriers, shader compilation,
allocator reuse, fence synchronization, chunk streaming, GPU readback, and
compute-driven editing/simulation.

## Current Demo

The public demo path is `Sandbox Mode`.

In the sandbox you can:

- explore an extreme vertical voxel world with cliffs, ravines, spires, shelves,
  basin water, and cave-like terrain openings
- paint and erase voxels directly into the streamed world
- use painting as traversal support by building bridges, ramps, platforms, and
  tunnels
- move across streamed chunk boundaries while edits persist during the session
- inspect runtime metrics for FPS, frame time, pixels, voxel capacity, streaming
  queues, chunk copy work, brush feedback, physics, and far SVO state

Screenshots and demo video are intentionally not included yet. They will be
added after the current checkpoint is captured.

## Feature Highlights

- DirectX 12 rendering backend with explicit resource state transitions.
- Runtime HLSL shader compilation with DXC.
- Fullscreen HLSL raymarch renderer over a moving dense voxel window.
- GPU-generated terrain in `64 x 64 x 64` voxel chunks.
- `1 MB` GPU buffer per generated chunk.
- Dense editable render window of `19 x 7 x 19` chunks.
- `1216 x 448 x 1216` dense voxel render buffer, or `662,437,888` voxels per
  buffer.
- Conceptual vertical terrain range from `Y = -332` to `Y = 664`.
- Chunk loading, unloading, fence-aware deferred cleanup, and recenter
  diagnostics.
- GPU brush raycasting with asynchronous compact edit feedback.
- Sparse per-chunk edit overlays so brush changes survive render-window
  streaming and recentering during a run.
- Traversal brush handoff that finishes line-of-sight painting as a ramp near
  the player feet.
- Chunk-budgeted physics path that avoids full vertical-buffer scans by default.
- Experimental GPU-backed sparse voxel octree far field with a page-index
  accelerator for visual distance.
- Dear ImGui runtime metrics overlay.

## Architecture

```text
Input / Camera
  -> InfiniteChunkManager
  -> GPU chunk generation
  -> persistent edit overlays
  -> moving dense render buffer
  -> HLSL raymarch renderer
  -> optional sparse far-field SVO
  -> ImGui diagnostics
```

VENPOD keeps gameplay and editing in a dense local voxel window centered around
the player. World chunks use stable signed world/chunk coordinates, while the
render buffer is a moving GPU window. The shader converts world coordinates into
buffer-local coordinates with the active render origin.

The sparse voxel octree path is visual-only in this checkpoint. It extends far
terrain silhouettes without making the SVO authoritative for collision, brush
edits, or physics.

## Build

VENPOD targets Windows and DirectX 12.

Requirements:

- Windows 10 or 11
- DirectX 12 capable GPU
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20 or newer
- Git
- PowerShell
- vcpkg

Recommended setup:

```powershell
cd VENPOD
.\setup.ps1
```

`setup.ps1` imports the Visual Studio build environment, installs or locates
Ninja, installs vcpkg packages, checks the vendored ImGui source, configures
CMake, and builds the executable.

Manual build, if you manage dependencies yourself:

```powershell
cd VENPOD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

## Run

For normal use:

```powershell
cd VENPOD
.\run.ps1
```

For the fastest rebuild-and-test loop:

```powershell
cd VENPOD
.\rebrun.ps1
```

Choose `Sandbox Mode` in the launcher.

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
| Left mouse | Paint voxels |
| Right mouse | Erase voxels |
| `Q` / `E` | Previous / next material |
| `[` / `]` | Decrease / increase brush radius |
| `Tab` | Toggle mouse capture |
| `Esc` | Pause menu |

## Documentation

The docs are organized using the Diataxis model:

- Tutorial: [Build and run VENPOD](docs/tutorials/build-and-run.md)
- How-to: [Use the sandbox](docs/how-to/use-the-sandbox.md)
- How-to: [Debug runtime behavior](docs/how-to/debug-runtime.md)
- Explanation: [Engine architecture](docs/explanation/architecture.md)
- Reference: [Runtime reference](docs/reference/runtime.md)
- Report: [Vertical world pass](docs/reports/vertical-world-pass.md)
- Report: [Sparse voxel octree far-field plan](docs/reports/sparse-voxel-octree-plan.md)

## Project Layout

```text
VENPOD/
  assets/shaders/       HLSL graphics and compute shaders
  src/Core/             windowing, timing, and app infrastructure
  src/Graphics/         renderer, far SVO, and DirectX 12 RHI helpers
  src/Input/            keyboard, mouse, and brush input
  src/Simulation/       voxel buffers, chunk streaming, physics, edit overlays
  src/UI/               ImGui panels and overlays
  vendor/imgui/         Dear ImGui source, vendored for reproducible demo builds
docs/
  tutorials/            guided learning docs
  how-to/               task-oriented docs
  explanation/          design and architecture docs
  reference/            controls and runtime details
  reports/              implementation reports and future work
```

## Known Limits

VENPOD is a graphics programming tech demo, not a packaged game engine.

Current limitations:

- Far SVO terrain is visual-only and static around origin in this checkpoint.
  Its page index avoids full page-list scans, but pages are not streamed around
  the camera yet.
- Brush edits persist during a runtime session, but disk save/load for edited
  chunks is not the default public path yet.
- Infinite physics remains conservative and budgeted; the stable demo favors
  responsiveness over simulating the whole vertical world at once.
- Screenshots and demo video still need to be captured for the public repo.

Generated build outputs are intentionally excluded from version control.

## License

MIT. See [LICENSE](LICENSE).
