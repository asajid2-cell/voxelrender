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
| `VENPOD_PLAYER_RADIUS_TENTHS=N` | Overrides the player/camera collision radius in tenths of a voxel; default is `15` (`1.5` voxels) so sparse walking keeps practical clearance from steep voxel surfaces |
| `VENPOD_ENABLE_EXPERIMENTAL_SPARSE=1` | Allows the sparse renderer/runtime path |
| `VENPOD_RENDER_BACKEND=sparse` | Requests the sparse brick backend |
| `VENPOD_SPARSE_RAYMARCH=1` | Enables sparse shader bindings and background sparse raymarch diagnostics |
| `VENPOD_SPARSE_DEBUG_MODE=50/51/52` | Overrides sparse visual debug coloring: `50` marks sparse surface ownership yellow, `51` colors sparse surfaces by height relative to the camera, and `52` colors sparse surfaces by camera distance |
| `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE=0/1` | Holds public world rendering during sparse startup while terrain residency, upload, and surface publication catch up; default is `1` so the first visible frame is not a half-loaded sparse world. The gate is one-way: once a valid public frame has opened, later streaming pressure must be handled by the terrain-critical scheduler, not by blanking the world again |
| `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME=N` | Minimum startup frame before public world rendering can begin when the startup gate is enabled; default is `112` after the May 15 startup terrain-gap regression |
| `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME=N` | Maximum frame where readiness can extend the startup public-render hold; default is `360` |
| `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS=N` | Minimum ready sparse render bricks required before the startup gate may open after `MIN_FRAME`; default is `512` |
| `VENPOD_SPARSE_ONLY=1` | Uses sparse runtime test mode instead of legacy dense streaming |
| `VENPOD_SPARSE_SURFACE_AUTHORITATIVE=1` | Makes sparse raster surfaces the authoritative near-field renderer |
| `VENPOD_SPARSE_SURFACE_STABLE_NEAR_CULL=0/1` | Enables distance-stable near-surface raster culling; default is `1`. Set `0` for culling isolation while leaving the rest of sparse rendering active |
| `VENPOD_SPARSE_SURFACE_CULLING=0/1` | Enables sparse surface culling; default is `1`. Setting `0` disables the optimized GPU-cull path and may trip capture-script default-path checks, so use it only for targeted isolation |
| `VENPOD_SPARSE_SURFACE_CULL_DISTANCE=N` | Overrides the stable near-surface raster cull distance; default is 1536 in sparse surface-authoritative mode so walking views stay on extracted voxel surfaces before mid/far layers take over |
| `VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS=N` | Overrides the surface ownership radius used to keep background layers out of the editable near-field view; default is 1536 with stable near culling |
| `VENPOD_SPARSE_SURFACE_NEIGHBOR_COVERAGE=0/1` | Requires horizontal surface-neighbor coverage before publishing raster surface bricks; default is `1` to avoid isolated exact-surface islands while surrounding bricks are still catching up |
| `VENPOD_SPARSE_SURFACE_RASTER_MAX_DISTANCE=N` | Clips rasterized exact sparse-surface faces beyond the foreground band; default is `1536` with stable near culling so lake and walking views stay on extracted voxel faces before mid/far context takes over |
| `VENPOD_SPARSE_CATCHUP_MIN_VOXEL_TERRAIN_PIXELS_PCT=N` | Minimum screen percentage that must be owned by exact sparse surface, near sparse raymarch, mid voxel clipmap, or far SVO before the residency scheduler stops catch-up; default is `68` |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH=0/1` | Enables the terrain-aware visible-surface prefetch lane; default is `1`, using the CPU terrain height function to request full-resolution sparse bricks at actual view/terrain intersections instead of relying only on sparse camera rays through air |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_MAX_REQUESTS=N` | Caps new visible terrain-surface brick requests per frame; default is `128` and still respects the global visible request/page budgets |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_RESERVE_REQUESTS=N` | Reserves visible-request budget for terrain-surface ownership before generic view-cone residency can spend it; default is `192` so the widened screen-critical target set can drain without leaving a one-brick cap hole |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_DISTANCE=N` | Maximum CPU terrain-intersection distance for the surface prefetch lane; default is `1536` so walking views load exact surface bricks before the mid-voxel clipmap owns the scene |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_STRIDE=N` | CPU terrain-intersection march stride in voxels; default is `16` |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_RAYS_X/Y=N` | Screen-space ray grid used by terrain-aware surface prefetch; defaults are `9x7` |
| `VENPOD_SPARSE_TERRAIN_SURFACE_PREFETCH_MAX_COORDS=N` | Number of surface-neighborhood bricks requested per terrain prefetch hit; default is `7`. The May 14 surface-only probe (`1`) regressed the steep terrain gate, so keep this as a diagnostic knob rather than a default simplification |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_PREFETCH=0/1` | Enables the screen-critical terrain-intersection lane that runs before generic sparse view planning; default is `1` |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_MAX_REQUESTS=N` | Startup screen-critical terrain request cap; default is `256` after the shoreline route showed the exact foreground needs more protected request headroom before mid-voxel fallback takes over |
| Screen-critical new requests | The screen-critical lane is protected and uses its own cap rather than the runtime-scaled visible-reserve cap, so downscaled background streaming cannot leave one missing terrain brick inside the protected near view |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RECOVERY=0/1` | Reuses the screen-critical terrain lane after startup while sparse ownership pressure is active; default is `1` as a partial recovery path, not a final terrain residency guarantee |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_CONTINUOUS=0/1` | Keeps the screen-critical terrain lane active after startup even before ownership pressure reports a failure; default is `1` so near visible terrain remains the per-frame target set instead of waiting for delayed miss/valley feedback |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RECOVERY_MAX_REQUESTS=N` | Request cap for post-startup screen-critical recovery; default is at least `64` |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RECOVERY_MIN_LEVEL=N` | Ownership-pressure level required before post-startup screen-critical recovery; default is `1` |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RECOVERY_MIN_VALLEY_PCT=N` | Additional valley-atmosphere threshold for recovery; default is `0` because delayed valley ownership feedback starts too late for the focused May 14 route |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE/STRIDE=N` | Distance and march stride for screen-critical terrain rays; defaults are `1024` and `8` |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RAYS_X/Y=N` | Screen-critical terrain ray grid; defaults are `17x11` |
| `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_MAX_COORDS=N` | Number of brick-neighborhood coordinates requested per screen-critical terrain hit; default is `7`; the full `3x3x3` shell was tested on the May 15 long-walk route and over-requested enough to worsen unsafe-near ownership |
| Terrain-critical empty pages | Screen-critical and GPU miss-feedback terrain requests bypass the known-empty fast path, so protected near empty/air bricks are uploaded as real GPU-visible empty pages instead of being absent from the sparse page table |
| Terrain-hole ownership classifier | `RENDER_OWNER_UNSAFE_NEAR_MISS` is reserved for terrain-adjacent sparse misses whose first missing page is close to the expected terrain crossing. Grazing silhouette rays through open air are not counted as terrain holes |
| `VENPOD_SPARSE_TERRAIN_CRITICAL_UNSAFE_CONTEXT_LOG=0/1` | Emits per-frame `PERF_SPARSE_TERRAIN_CRITICAL_UNSAFE_CONTEXT` rows whenever ownership readback reports unsafe-near pixels; default is `1` |
| `VENPOD_SPARSE_TERRAIN_CRITICAL_LOG_EVERY_FRAME=0/1` | Forces the same unsafe-context diagnostic every frame; default is `0` |
| `VENPOD_SPARSE_TERRAIN_NEAR_SURFACE_PREFETCH=0/1` | Near-field terrain surface footprint prefetch. Default is `1`; it keeps real surface bricks near the camera from being starved by broader view-cone residency |
| `VENPOD_SPARSE_TERRAIN_NEAR_SURFACE_PREFETCH_RADIUS_BRICKS=N` | Radius for the optional near-field terrain surface footprint; default is `8` bricks when enabled |
| `VENPOD_SPARSE_TERRAIN_NEAR_SURFACE_PREFETCH_MAX_REQUESTS=N` | Maximum new visible requests for the optional near-field terrain footprint; default is `192` when enabled |
| `VENPOD_SPARSE_EXACT_NEAR_DISTANCE=N` | Distance in voxels where background/proxy layers are forbidden from satisfying terrain/water ownership; default is `1024` so missing near exact sparse voxels remain visible to diagnostics instead of being hidden by fake mid/far fallback |
| `VENPOD_SPARSE_VOXEL_TERRAIN_ONLY=0/1` | Controls whether sparse public rendering may use procedural mid/far height and far-water fallback as terrain. Default is `1`, so terrain is owned by exact sparse surfaces, sparse raymarch, mid-voxel clipmaps, and far SVO/voxel LOD; set `0` only for legacy height-proxy continuity experiments |
| `VENPOD_SPARSE_MID_START=N` | Overrides the mid clipmap handoff distance; default is `1024` so coarse mid-voxel terrain starts behind the public walking-distance surface shell |
| `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS=N` | Overrides the mid-voxel clipmap capacity; default is `6144` after A/B captures preserved far-gap and waterline hard gates while reducing mid-voxel GPU allocation and interest/pump work versus the 9216-slot probe |
| `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT=N` | Limits the active mid-voxel interest set as a percentage of cache capacity; default is `95` so the larger mid-voxel cache keeps hysteresis slack while prioritizing visible coverage |
| `VENPOD_SPARSE_MID_CATCHUP_TILE_BUDGET=N` | Protected mid-voxel clipmap generation budget used while voxel-terrain ownership is below target; default is `48` in voxel-only terrain mode and otherwise follows `VENPOD_SPARSE_MID_TILE_BUDGET` (`72`) so far mid-voxel catchup cannot monopolize a frame |
| `VENPOD_SPARSE_MID_HEIGHT_CLIPMAP=0/1` | Enables the legacy 2D mid-height clipmap that the mid-voxel shader still uses for column bounds; default is `1`. Disabling it is diagnostic-only because the May 17 fastflight probe reintroduced unsafe-near ownership |
| `VENPOD_SPARSE_MID_HEIGHT_TILE_BUDGET=N` | Per-frame generation cap for the 2D mid-height clipmap; default is `16` while voxel-only terrain is enabled so height maintenance cannot consume the same large catchup budget as mid-voxel bricks |
| `VENPOD_SPARSE_MID_HEIGHT_LOD_THROTTLE_BUDGET=N` | Extra cap for mid-height generation while high-altitude LOD throttling is active; default is `4`, preserving the column-bound data path without spending the full mid-height budget in far-SVO-dominated views |
| `VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET=N` | Extra mid-voxel clipmap catchup budget used when interested voxel coverage is below target and exact terrain-critical bricks are already ready; default is `48` to fill far gaps without reintroducing long CPU pump spikes |
| `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_CATCHUP_BUDGET=N` | Cap for final-percent mid-voxel catchup when the only pressure is soft voxel-terrain ownership deficit, no miss/unsafe/valley failure is visible, and interested voxel coverage is already at least `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_MIN_COVERAGE_PCT`; default is `24` |
| `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_MIN_COVERAGE_PCT=N` | Minimum interested mid-voxel coverage before the soft-deficit cap is allowed; default is `95`, so hard coverage failures and terrain-critical catchup still use the stronger path |
| `VENPOD_SPARSE_RAYMARCH_WATERLINE_MAX_SCALE_PERCENT=N` | Caps fullscreen sparse raymarch distance/step scale near or below sea level; default is `25` after the waterline route proved ownership stayed clean while GPU ray time dropped from roughly `18 ms` to below `5 ms` |
| `VENPOD_SPARSE_MID_VOXEL_WALK_DDA=0/1` | Enables the full 3D mid-voxel DDA for walking-height background terrain after sparse raster surfaces miss; default is `0` because the May 14 steep-route A/B raised mid-voxel ownership but failed native temporal stability |
| `VENPOD_SPARSE_WALK_TEST_PITCH_DEG=N` | Overrides scripted walk-capture pitch in degrees; default is `-4` so movement captures look forward instead of down into terrain walls |
| `VENPOD_SPARSE_MISS_FEEDBACK=1` | Enables GPU missing-brick feedback planning |
| `VENPOD_SPARSE_MISS_FEEDBACK_TERRAIN_SURFACE_REMAP=0/1` | Remaps sparse miss-feedback coordinates to the CPU terrain surface column before requesting visible bricks; default is `1` after the one-coordinate remap improved the May 14 startup/steep terrain gates |
| `VENPOD_SPARSE_MISS_FEEDBACK_SURFACE_MAX_COORDS=N` | Number of remapped surface-neighborhood coordinates per feedback record; default is `1` because wider shells increased valley-atmosphere coverage on the steep route |
| `VENPOD_SPARSE_GPU_RAYCAST=1` | Enables the GPU sparse raycast path |
| `VENPOD_SPARSE_BRUSH_FEEDBACK=1` | Enables GPU sparse brush feedback readback |
| `VENPOD_SPARSE_BRUSH_FEEDBACK_STRICT_RESIDENT_ONLY=1` | In authoritative brush feedback diagnostics, skips the intentional nonresident fallback case and fails on any CPU fallback for resident strokes. In the live brush path, strict mode preflights the brush footprint and defers the stamp while edited-residency bricks or page-table publications catch up; `.\rebrun.ps1 -SparseBrushFeedbackStrictResidentOnly` sets this |
| `VENPOD_SPARSE_BRUSH_FEEDBACK_MOVING_DIAGNOSTIC=1` | Makes the brush feedback diagnostic recenter from the moving camera each diagnostic case; `.\rebrun.ps1 -SparseBrushFeedbackMovingDiagnostic` sets this and the sparse regression gate uses it with stress-camera movement |
| `VENPOD_SPARSE_BRUSH_PAINT_SMOKE=1` | Forces the normal held-paint brush path for a bounded frame window while GPU brush feedback is authoritative. The smoke targets deterministic terrain near the sparse scenic spawn so strict resident-only validation is not dependent on random stress-camera aim; `.\rebrun.ps1 -SparseBrushPaintSmoke` sets this for the sparse brush paint regression |
| `VENPOD_DISABLE_BRUSH_INPUT=1` | Ignores live mouse/scroll brush input while still updating the rest of the runtime; `engine_capture_smoke.ps1` sets this so scripted terrain captures cannot accidentally dirty sparse bricks from stale mouse-button state |
| `VENPOD_ENABLE_SPARSE_PHYSICS=0` | Disables default-on local sparse physics when sparse runtime mode is active |
| `VENPOD_SPARSE_PHYSICS_GPU=1` | Enables GPU sparse physics packet upload/readback |
| `VENPOD_SPARSE_PHYSICS_GPU_APPLY=1` | Allows guarded CPU application of GPU physics proposals |
| `VENPOD_SPARSE_PHYSICS_GPU_STRICT=1` | Fails the runtime log if GPU physics proposal readback/apply sees missing support, malformed rows, rejected proposals, or upload overflow; `.\rebrun.ps1 -SparseGpuPhysicsStrict` sets this |
| `VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED=1` | Queues the non-fluid material GPU-physics diagnostic seed. `.\rebrun.ps1 -SparsePhysicsDiagnosticSeed` sets this explicitly; `-SparsePhysicsSmoke` sets it by default unless a fluid-only diagnostic is requested |
| `VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED=1` | Queues the fluid GPU-physics diagnostic seed. It can be combined with `VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_MATERIAL_SEED=1` for the strict mixed material/fluid smoke |
| `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES=N` | Delays sparse GPU page-table publication by N frames after brick payload upload; used by the async page-publish regression gate |
| `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES=N` | Adds N direct-queue fence values to sparse GPU page-table publication readiness; used to exercise fence-wait telemetry |
| `VENPOD_SPARSE_EDIT_FILE=path` | Loads sparse edit overlays on startup and saves them on shutdown; `.\rebrun.ps1 -SparseEditFile path` sets this for normal runs, and relative paths resolve from the `VENPOD/` project root when launched by the scripts |
| `VENPOD_EXIT_AFTER_FRAMES=N` | Exits automatically after N frames for smoke tests |

## Key Constants

| Value | Meaning |
| --- | --- |
| `16 x 16 x 16` | Sparse near-field brick dimensions |
| `12288` | Default sparse brick-page pool; raises exact-terrain residency headroom so voxel-only terrain does not churn near the walking view |
| `2097152` | Default sparse surface-face GPU capacity |
| `192,224` XZ | Public-review sparse terrain comfort basin center used by the CPU generator and HLSL far-terrain mirror |
| `32768` entries | Default sparse page table capacity; configurable by environment and clamped to at least twice the sparse brick-page pool |
| `112` frames | Default sparse startup public-render hold before the first public world frame; startup frame-0 captures intentionally use `-MinUniqueSampleColors 1` because held frames are a flat loading/sky state, not world rendering |

## Sparse Readiness Diagnostics

Sparse runtime logs include `PERF_SPARSE_READINESS` rows. These rows summarize
active sparse brick render-readiness states: `requested`, `uploadQueued`,
`uploading`, `residentMissingSurface`, `residentEmpty`, `ready`, dirty states,
and eviction states. Use them with `PERF_RENDER_OWNERSHIP` and the capture CSVs
when diagnosing holes: pixels tell what was drawn, readiness rows tell what the
sparse brick pipeline had available at that frame.

`PERF_SPARSE_TERRAIN_CRITICAL` rows also report screen-critical terrain target
readiness. `pre*` fields classify the target set before same-frame protected
publish; `post*` fields classify the same target set at log time after the frame
has had a chance to generate/upload/publish protected terrain. A healthy public
frame should have no `postMissing`, `postRequested`, `postUploadQueued`,
`postUploading`, or `postResidentMissingSurface` for the focused terrain route.
If any post-publish terrain-critical target is still not ready, the runtime also
emits `PERF_SPARSE_TERRAIN_CRITICAL_NONREADY` with up to twelve
`brickX,brickY,brickZ:State` samples. `engine_capture_smoke.ps1` fails
post-ready captures when any post-publish terrain-critical target is still
missing, requested, generating, upload-queued, uploading, or resident without
surface data.

When `PERF_RENDER_OWNERSHIP` reports unsafe-near pixels, the default
`PERF_SPARSE_TERRAIN_CRITICAL_UNSAFE_CONTEXT` row correlates that ownership
sample with the current terrain-critical target set. If the row shows all
targets as `postReady` or `postEmpty`, the failure is not generation/upload
backlog; inspect the shader ownership classifier or the screen target mapping.

`PERF_RENDER_OWNERSHIP` rows include `lodParentHeld` /
`parentHeldUntilChildrenReady`. These count mid-voxel pixels where the preferred
fine clipmap ring was missing and the shader used a coarser resident ring
instead. Nonzero values are expected during streaming catch-up; they mean the
renderer held parent LOD instead of exposing a child-ring hole.
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
| `VENPOD/sparse_regression.ps1` | Combined sparse smoke/regression gate, including public-review doc/link/source-artifact verification, dense fallback launch coverage, and capture runtime ownership assertions |
| `VENPOD/engine_capture_smoke.ps1` | Engine backbuffer capture smoke test with early capture-window validation |
| `VENPOD/public_demo_capture.ps1` | Validated public demo contact sheet and MP4 generation; sparse regression additionally checks the generated runtime log for sparse ownership health, including visible far-SVO pixels |
| `VENPOD/visual_review_capture.ps1` | Manual visual review suite for normal, walk, long-walk, fast-flight, long-fast-flight, fast water-transition, long fast-water transition, waterline, and long-waterline captures, including a checklist and CSV summary of active gates and observed ownership metrics |

Developer and legacy helper scripts are still parsed by the public-review
verifier, but they are not the preferred public review path:

| Script | Role |
| --- | --- |
| `VENPOD/rebuild.ps1` | Older quick rebuild helper; `build.ps1` is the documented build entrypoint |
| `VENPOD/rerun.ps1` | Short alias that forwards common options to `rebrun.ps1` |
| `VENPOD/visual_capture_smoke.ps1` | Older window/GDI capture smoke; `engine_capture_smoke.ps1`, `public_demo_capture.ps1`, and `visual_review_capture.ps1` are the review media paths |

## Sparse Regression Options

The public review gate should normally run without skip switches:

```powershell
powershell -ExecutionPolicy Bypass -File .\VENPOD\sparse_regression.ps1 -Config Release
```

`sparse_regression.ps1` also exposes targeted skip switches for focused local
debugging, including `-SkipTests`, `-SkipDenseLegacySmoke`,
`-SkipFlickerSmoke`, `-SkipSurfaceSmoke`, `-SkipGpuRaycastSmoke`,
`-SkipMissFeedbackSmoke`, `-SkipBrushFeedbackSmoke`,
`-SkipBrushFeedbackApplySmoke`, `-SkipBrushFeedbackAuthoritativeSmoke`,
`-SkipBrushFeedbackStrictResidentSmoke`, `-SkipBrushFeedbackMovingSmoke`,
`-SkipBrushPaintSmoke`, `-SkipDefaultPhysicsSmoke`,
`-SkipGpuPhysicsStrictSmoke`, `-SkipAsyncPagePublishSmoke`,
`-SkipEngineCaptureSmoke`,
`-SkipStressEngineCaptureSmoke`, `-SkipFastFlightEngineCaptureSmoke`,
and `-SkipPublicDemoCapture`.

`-SkipPublicReviewDocs` disables the pre-build public review verifier. Use it
only for narrow script debugging; public review runs should leave it enabled so
required docs/scripts, links, anchors, public script parsing, contact-sheet
presence, source-artifact tracked/staged git visibility, and generated-artifact
ignore patterns are checked before the build.
