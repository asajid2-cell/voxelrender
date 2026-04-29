# VENPOD

VENPOD is a DirectX 12 voxel engine tech demo written in C++20 and HLSL. It renders a first-person infinite terrain sandbox with GPU-generated chunks, HLSL raymarching, voxel physics, brush editing tools, and an ImGui diagnostics layer.

The project is intended to show low-level graphics and GPU-simulation work clearly: descriptor management, command queues, resource barriers, shader compilation, allocator reuse, fence synchronization, chunk streaming, and compute-driven simulation.

<p align="center">
  <img src="docs/screenshots/painting_example_1.png" width="80%" alt="VENPOD procedural voxel terrain">
</p>

## Current Snapshot

This branch is a stable public tech-demo snapshot. The infinite-world sandbox is the main mode. It includes:

- DirectX 12 rendering infrastructure with explicit resource state management.
- Runtime HLSL shader compilation with DXC.
- DDA raymarching in the pixel shader.
- GPU-generated terrain in `64 x 64 x 64` voxel chunks.
- A visible render window of `25 x 2 x 25` chunks, or 1,250 chunks total.
- A loaded-world budget of `33 x 2 x 33` chunks, or 2,178 chunks total.
- GPU brush raycasting and localized brush compute dispatches.
- Chunk-scoped physics and diagnostics.

## Build And Run

VENPOD currently targets Windows with DirectX 12.

Prerequisites:

- Windows 10 or 11
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20 or newer
- Git
- PowerShell

From the repository root:

```powershell
cd VENPOD
.\setup.ps1
.\run.ps1
```

For normal development after setup:

```powershell
cd VENPOD
.\build.ps1
.\run.ps1
```

Manual build:

Open a Visual Studio Developer PowerShell or Developer Command Prompt, then run:

```powershell
cd VENPOD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build\bin\VENPOD.exe
```

## Controls

- `WASD`: Move
- Mouse: Look
- `Space`: Jump
- Double-tap `Space`: Toggle flight mode
- `Space` / `Shift` in flight mode: Fly up / down
- Left mouse: Paint voxels
- Right mouse: Erase voxels
- `Q` / `E`: Previous / next material
- `[` / `]`: Decrease / increase brush radius
- `Tab`: Toggle mouse capture
- `Esc`: Pause menu

## Documentation

The docs are organized using the Diataxis model:

- Tutorial: [Build and run VENPOD](docs/tutorials/build-and-run.md)
- How-to: [Use the sandbox](docs/how-to/use-the-sandbox.md)
- How-to: [Debug runtime behavior](docs/how-to/debug-runtime.md)
- Explanation: [Engine architecture](docs/explanation/architecture.md)
- Reference: [Runtime reference](docs/reference/runtime.md)

## Project Layout

```text
VENPOD/
  assets/shaders/       HLSL graphics and compute shaders
  src/Core/             windowing, timing, and app infrastructure
  src/Graphics/         renderer and DirectX 12 RHI helpers
  src/Input/            keyboard, mouse, and brush input
  src/Simulation/       voxel buffers, chunk streaming, physics
  src/UI/               ImGui panels and overlays
  vendor/imgui/         Dear ImGui source
docs/
  screenshots/          README images
  tutorials/            guided learning docs
  how-to/               task-oriented docs
  explanation/          design and architecture docs
  reference/            controls and runtime details
```

## Notes

This is a graphics programming demo, not a packaged game. The code favors explicit DirectX 12 systems over engine middleware so the rendering and synchronization work is visible in the repository.

Generated build outputs are intentionally excluded from version control. Rebuild locally with the PowerShell scripts or CMake commands above.

## License

MIT. See [LICENSE](LICENSE).
