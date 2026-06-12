# VENPOD — a voxel world that runs on the GPU

**A from-scratch DirectX 12 engine that streams an infinite, editable voxel world — with terrain generated on the GPU and falling-sand-style material simulation.**

![VENPOD rendering its streamed voxel world](docs/media/sparse-engine-contact-sheet.png)

*Real in-engine captures: an infinite voxel landscape of cliffs, caves, and water you can fly through and reshape in real time.*

**Why it's hard:** no game engine, no graphics library — just raw DirectX 12. That means hand-managing GPU memory, descriptor heaps, command queues, and fence synchronization; writing a custom voxel renderer (DDA raymarch + rasterized sparse surfaces); generating terrain on the GPU with compute shaders; and simulating falling-sand-style materials (sand, water, lava) across the world.

**What you can do in it:** fly through an infinite world, paint and erase voxels to carve tunnels and build bridges, and watch sand / water / lava-style materials flow and settle — streamed in sparse bricks as you move, with your edits persisting.

**Run it** (Windows + a DirectX 12 GPU):

```powershell
cd VENPOD
.\setup.ps1     # one-time: vcpkg deps + build
.\rebrun.ps1    # launch the sandbox
```

Everything below is the engineering detail — architecture, build internals, controls, and honest limitations.

## Feature Highlights

- DirectX 12 rendering backend with explicit resource state transitions.
- Runtime HLSL shader compilation with DXC.
- Sparse near-field brick pool using `16 x 16 x 16` world-space bricks.
- Generation-aware CPU/GPU page-table publication.
- Rasterized sparse surface path with GPU culling and indirect draw commands.
- Mid voxel/height clipmap continuity and async far SVO background ownership.
- Legacy fullscreen HLSL raymarch renderer over a moving dense voxel window for
  fallback and comparison.
- Conceptual vertical terrain range from `Y = -332` to `Y = 664`.
- Sparse residency planning, byte-aware upload scheduling, page publish queues,
  and fence-aware surface range retirement.
- GPU sparse raycast/brush feedback diagnostics with CPU-authoritative fallback.
- Sparse edit overlays so brush changes survive eviction/reload during a run.
- Traversal brush handoff that finishes line-of-sight painting as a ramp near
  the player feet.
- Local sparse physics on dirty/active regions by default; GPU proposal
  validation is available as an experimental diagnostic path.
- Dear ImGui runtime metrics overlay.

## Architecture

```text
Input / Camera
  -> sparse residency planner
  -> sparse brick generation / edit overlays
  -> GPU upload ring and page-table publish queue
  -> sparse surface extraction and GPU culling
  -> mid/far clipmap and far SVO background layers
  -> sparse local physics and feedback readbacks
  -> ImGui diagnostics
```

VENPOD now treats stable world-space sparse bricks as the main development
architecture. Dense moving-buffer code still exists as a legacy path and as a
small compatibility owner for some older buffers in sparse runtime mode.

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

For the current sparse demo path:

```powershell
cd VENPOD
.\rebrun.ps1
```

For dense legacy comparison:

```powershell
cd VENPOD
.\rebrun.ps1 -DenseLegacy
```

To load and save sparse brush edits across runs:

```powershell
cd VENPOD
.\rebrun.ps1 -SparseEditFile saves\review-edits.vsed
```

To generate public review media from the in-engine DX12 capture path:

```powershell
cd VENPOD
.\public_demo_capture.ps1 -Config Release
```

To generate one review reel that combines normal, high-flight, and
waterline/submerged validated segments:

```powershell
cd VENPOD
.\public_demo_capture.ps1 -Config Release -ReviewReel
```

To regenerate the broader visual review suite used by the completion ledger:

```powershell
cd VENPOD
.\visual_review_capture.ps1 -Config Release
```

This produces normal, walk, long-walk, fast-flight, long-fast-flight,
fast water-transition, long fast-water transition, waterline, and
long-waterline contact sheets plus a manual checklist and CSV summary under
`VENPOD/build/logs/visual_review_capture/`. Passing this
wrapper does not by itself mean the visuals are accepted for release; it creates
the evidence reviewers use for that decision.

The full sparse regression gate also runs a short dense legacy fallback smoke so
that comparison path stays covered. It also verifies the public demo capture
runtime log for terrain ownership, mid/far ownership, visible far-SVO pixels,
surface fragments, and zero sparse miss/unsafe-near-miss pixels:

```powershell
powershell -ExecutionPolicy Bypass -File .\VENPOD\sparse_regression.ps1 -Config Release
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
- How-to: [Capture a public demo](docs/how-to/capture-public-demo.md)
- How-to: [Debug runtime behavior](docs/how-to/debug-runtime.md)
- Explanation: [Engine architecture](docs/explanation/architecture.md)
- Reference: [Runtime reference](docs/reference/runtime.md)
- Reference: [Sparse refactor review checklist](docs/reference/sparse-refactor-review.md)
- Reference: [Public review manifest](docs/reference/public-review-manifest.md)
- Reference: [Sparse completion audit](docs/reference/sparse-completion-audit.md)
- Reference: [Asset credits](docs/reference/asset-credits.md)
- Historical report: [Vertical world pass](docs/reports/vertical-world-pass.md)
- Historical report: [Sparse voxel octree far-field plan](docs/reports/sparse-voxel-octree-plan.md)

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
  reports/              historical implementation reports
```

## Known Limits

VENPOD is a graphics programming tech demo, not a packaged game engine.

Current limitations:

- The sparse renderer is the active development path, but dense legacy is still
  kept for regression comparison and public fallback.
- Brush edits can be saved and loaded with `.\rebrun.ps1 -SparseEditFile` or
  from the pause-menu metrics panel.
- GPU brush feedback and GPU physics proposal application are guarded hybrid
  paths; CPU sparse authority remains the resilience fallback.
- Mid/far terrain is coherent enough for smoke/regression gates, but the final
  long-distance hierarchy still needs visual polish and more LOD work.
- Public review media is generated on demand by `.\public_demo_capture.ps1`;
  large MP4 artifacts are not checked into git.

Generated build outputs are intentionally excluded from version control.

## License

MIT. See [LICENSE](LICENSE).
