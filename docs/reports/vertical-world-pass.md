# VENPOD Vertical World Pass

## Summary

This pass changes VENPOD from a shallow fixed-Y terrain demo into a moving
vertical-window sandbox. The engine now keeps a bounded local render buffer
around the player while chunk coordinates, generation, copy placement, and
render sampling operate in signed world-space Y.

The default terrain preset is `extreme mountains`: large cliffs, terraces,
needle spires, ravines, cavern mouths, basin water, and cave carving generated
from world-coordinate noise so chunk seams remain stable.

## Previous Limits

- Terrain constants only allowed `Y=4..124`.
- Chunks were always queued at fixed layers `Y=0,1`.
- The active render buffer origin used fixed `Y=0`.
- Copy-to-render-buffer mapped chunk Y with `chunk.y - TERRAIN_CHUNK_MIN_Y`.
- Spawn was hardcoded near the top of the old 128-voxel terrain span.
- Metrics reported X/Z streaming but not vertical render-window bounds.
- The terrain generator was still primarily a height field, so it could not
  make convincing deep drops or cavernous vertical traversal spaces.

## Current Vertical Model

The conceptual terrain range is:

- `TERRAIN_MIN_Y = -332`
- `TERRAIN_MAX_Y = 664`
- chunk range `Y=-6..10`

The renderer does not allocate this full range. It uses a moving render window:

- `17 x 4 x 17` chunks
- `1088 x 256 x 1088` voxels
- `303,038,464` voxels per buffer

This keeps render-buffer capacity close to the old shallow window:

- old: `25 x 2 x 25 = 1,250` chunks
- current: `15 x 6 x 15 = 1,350` chunks

Loaded chunks use a larger background window:

- horizontal load distance: `+/-10`
- vertical load window: `3 chunks below`, `2 chunks above`
- steady loaded budget near spawn: `2,646` chunks

## Terrain Generator

`CS_GenerateChunk.hlsl` now combines:

- broad continent/valley shape
- ridged mountain chains
- clustered needle spires
- ravine/canyon masks
- terraced shelf bands
- cave and cavern mouth masks
- low basin/river water
- biome/material variation by altitude, ravine mask, and mountain mask
- a scenic origin uplift so the initial spawn is near interesting vertical terrain

The generator remains deterministic per world coordinate and seed. Adjacent
chunks sample the same world-space fields, so terrain continuity does not depend
on chunk-local coordinates.

## Runtime Safeguards

- Render buffer size is kept near the previous GPU memory budget.
- Chunk generation remains budgeted per frame.
- Chunk copy work remains capped by the adaptive copy budget.
- Buffer swaps do not block on chunk-copy fences; the renderer keeps the last
  stable buffer if copy work is behind.
- Raymarch budget is bounded at `1600` voxels / `1536` steps.
- VSync remains enabled for the public `static` tech demo target.
- Metrics overlay now reports world position, chunk coordinate, render Y bounds,
  visible chunks, loaded chunks, generated voxels, pixels, copy budget, skipped
  work, and raymarch budget.

## Verification

Release build:

```powershell
.\build.ps1 -Config Release
```

Runtime smoke test:

- Sandbox launched.
- `PS_Raymarch.hlsl` compiled.
- `CS_GenerateChunk.hlsl` compiled.
- No runtime `critical`, `error`, `failed`, or `timeout` log lines were found.
- Streaming converged to `READ=1156 WRITE=1156`.
- Loaded chunks converged to `2646`.
- Visual capture showed steep walls, shelves, cavernous drops, and terrain near
  spawn with the overlay holding around 60 FPS.

## Remaining Architectural Blockers

This is real vertical chunk streaming, but not yet a full world-streaming
architecture:

- Physics operates on the local render buffer, not on a persistent world-chunk
  simulation layer.
- Spawn selection is still a seeded scenic spawn plus ground detection, not a
  CPU/GPU search over many candidate terrain columns.
- Terrain presets are compile-time shader constants, not runtime UI options.
- The chunk generation queue still uses a single priority queue rather than
  separate near-field, vertical, and background lanes.

## Next Refactor

The next terrain/traversal pass should build on the persistent chunk-edit layer:

1. Add disk persistence and exact generated-material queries for brush edits.
2. Add a chunk metadata pass for surface height, biome, cave/ravine masks, and
   spawn candidates.
3. Split chunk streaming queues by urgency:
   near render window, vertical safety margin, far background preload.
4. Add runtime terrain preset switching through constants or a small terrain
   settings buffer.
5. Add a low-cost depth/step diagnostic path for measuring raymarch work per
   frame on GPU instead of only reporting configured budgets.

## Persistent Edit Pass

The brush path previously wrote only to the current local render buffers. That
made painting look correct immediately, but edits were not part of stable world
state. When the render window shifted, copied chunks came back from generated
terrain and the old `m_modifiedChunks` set was cleared, so bridges, tunnels, and
stairs could disappear after streaming.

This pass adds a sparse persistent edit overlay:

- edits are stored by stable `ChunkCoord`
- each edited chunk stores only touched local voxel indices
- local indices use signed-world coordinate conversion, including negative Y
- generated chunks are copied normally, then edited voxels are replayed into the
  render buffer with `CS_ApplyVoxelEdits.hlsl`
- the replay path is applied separately to READ and WRITE ping-pong buffers
- upload buffers are ringed with the chunk-copy allocators so edit replays do not
  overwrite data still in use by the GPU

The runtime overlay now reports:

- edited chunk count
- edited voxel count
- edit voxels/chunks applied during the latest chunk upload
- targeted world voxel
- targeted chunk coordinate
- targeted local voxel coordinate
- whether the current target has a persistent edit overlay entry

A startup self-test validates the coordinate conversion and overlay lookup for
positive coordinates, chunk boundaries, negative coordinates, and deep negative
Y. The test currently covers CPU-side persistence semantics; it does not read
back GPU replayed voxels.

### Current Edit Semantics

Runtime-session persistence is implemented. Disk save/load is not implemented
yet.

Paint, replace, fill, and erase are stored as voxel overrides. The CPU overlay
does not yet sample generated terrain before deciding whether a paint operation
would have affected only air in the shader. In normal traversal use this matches
the important case, painting platforms or stairs into air. A future pass should
add generated/material queries for exact CPU/GPU brush parity.

### Physics Safeguard

The old physics scan still operates over the local render buffer rather than a
budgeted persistent chunk simulation. On the vertical world that scan can hitch
badly during startup. Infinite-world physics is now opt-in with:

```powershell
$env:VENPOD_ENABLE_INFINITE_PHYSICS='1'
```

The default public sandbox prioritizes smooth streaming, traversal, raycast, and
persistent painting. This is the deepest safe version without refactoring physics
to operate on a small active chunk set.

### Verification

Release build:

```powershell
.\build.ps1 -Config Release
```

Runtime checks:

- `PS_Raymarch.hlsl`, `CS_GenerateChunk.hlsl`, and `CS_ApplyVoxelEdits.hlsl`
  compiled.
- Persistent edit apply pipeline initialized.
- Persistent edit coordinate self-test passed for six signed-coordinate cases.
- Logged smoke runs reached the main loop and streamed the render window without
  runtime `critical`, `error`, `failed`, or `timeout` log lines.
- With infinite physics disabled by default, startup avoids the old full-buffer
  physics hitch.

### Next Edit/Traversal Refactor

1. Add disk save/load for edited chunk overlays.
2. Add a generated-material query path so CPU brush persistence exactly matches
   GPU brush paint/replace rules.
3. Add a debug command that paints a known voxel, forces a recenter, returns, and
   checks the overlay/replay path.
4. Refactor physics into a chunk-budgeted world simulation that can safely run in
   the infinite vertical sandbox.

## Brush Correctness and Physics Budget Pass

The brush path now carries the GPU raycast normal through the CPU persistence
path. The immediate GPU brush and persistent overlay both use the same brush
center, radius, shape, mode, material, seed, and render-window origin. The CPU
overlay mirrors the shader SDF for sphere, cube, and cylinder brushes, and the
overlay debug metrics report the current hit voxel, chunk/local coordinate, hit
normal, brush radius, mode, material, evaluated voxels, rejected voxels, and
recorded persistent edits.

### CPU/GPU Brush Parity Status

Exact generated-terrain sampling on CPU is still not implemented; that would
require either a generated-material query path or a GPU edit-record pass. The
new behavior is a safer approximation:

- paint/build records only the visible air side of the raycast face
- erase/replace records the solid side of the raycast face
- existing persistent solid overlays block later paint-mode records, matching
  the GPU rule that paint only adds into air
- existing persistent air overlays remain air until replaced or filled
- chunk-boundary and negative-Y overlay cases are covered by the startup
  self-test

This prevents the old failure where paint mode persisted material inside solid
terrain that the immediate GPU brush never visibly added.

### Infinite Physics Refactor

Infinite physics remains opt-in:

```powershell
$env:VENPOD_ENABLE_INFINITE_PHYSICS='1'
```

When enabled, the scanner no longer sweeps the full vertical render buffer. It
builds an active chunk list from a player-centered, budgeted local window:

- normal budget: `512` candidate physics chunks
- frame-pressure budget: `256` candidate physics chunks
- observed opt-in scan window: `11 x 4 x 11 = 484` chunks
- scan origin follows the player in local render-buffer chunk coordinates
- physics still runs through the existing active-list/indirect dispatch path

This keeps the old chunk physics code isolated from startup streaming pressure.
It is much safer than the previous `32 x 16 x 32 = 16,384` chunk scan, but it is
still a local render-buffer physics system, not a persistent world-chunk
simulation. Infinite physics should remain opt-in until it has active-count
readback, edit-priority queues, and better material rules for generated water.

### Verification Update

Release build:

```powershell
.\build.ps1 -Config Release
```

Default sandbox smoke:

- `Infinite physics: no`
- persistent edit apply pipeline initialized
- persistent edit self-test passed
- diagnostics reached steady frame logs
- visible render buffers converged to `READ=1156 WRITE=1156`
- no `critical`, `error`, `failed`, or `timeout` log lines

Opt-in infinite physics smoke:

- `Infinite physics: yes`
- visible buffers converged to `READ=1156 WRITE=1156`
- physics scan ran as `11 x 4 x 11 = 484` chunks
- indirect physics dispatch completed repeatedly
- the process remained responsive during the smoke run
- no `critical`, `error`, `failed`, or `timeout` log lines

### Remaining Weak Spots

- Visual brush persistence still needs manual validation with real painting,
  recentering, and returning to the edited area.
- CPU persistence still approximates generated air/solid state from the hit
  normal instead of querying exact generated terrain.
- Infinite physics has a much smaller scan, but active voxel count is not yet
  read back to the overlay.
- Edited chunks are naturally prioritized when near the player, but there is not
  yet a separate recently-edited physics queue.

## GPU Brush Edit Feedback Pass

Brush persistence now uses the shader as the authority for edited voxels. The
brush compute shader writes a compact `uint2(localRenderIndex, packedVoxel)`
event only when it actually changes a voxel. The CPU no longer has to guess
which voxels the shader accepted for the normal path.

### Design

The feedback path is asynchronous:

1. `CS_Brush.hlsl` paints the READ buffer without recording events for immediate
   visual feedback.
2. The same shader paints the WRITE buffer with event recording enabled.
3. A GPU counter tracks the compact event count with `InterlockedAdd`.
4. The event buffer and counter are copied into a rotating readback slot.
5. The frame fence is attached to that slot after command submission.
6. Later frames retire completed slots and convert local render indices back to
   stable world/chunk/local coordinates using the render-window origin captured
   at dispatch time.
7. Exact changed voxels are stored in the sparse persistent chunk overlays.

This fixes the main CPU/GPU brush parity blocker for runtime persistence:
paint-only-into-air, erase-only-solid, replace, fill, edge-strength rejection,
bedrock rejection, shape bounds, and material packing are all decided by the GPU
shader before an edit is persisted.

### Runtime Safeguards

- Readback uses four rotating slots and never blocks the frame to wait for a
  brush result.
- The feedback buffer stores up to `131,072` edit events per stroke/frame.
- Overflow is clamped and reported instead of reading past the buffer.
- If all readback slots are pending, the system falls back to the CPU overlay
  approximation rather than dropping immediate painting.
- The captured `regionOriginWorld` prevents recenter drift: event indices are
  translated using the origin from the frame that produced the GPU edits.

### Diagnostics

The metrics overlay now includes:

- queued GPU feedback strokes
- pending readback slots
- GPU edit events applied on the last retired slot
- dropped feedback events
- overflowed feedback events

Diagnostics logs also report applied GPU feedback events in debug mode.

### Verification Update

Release build:

```powershell
.\build.ps1 -Config Release
```

Runtime shader/launch smoke:

- `CS_Brush.hlsl` compiled successfully with the expanded root signature.
- GPU brush edit feedback initialized with `131072` compact events and four
  async readback slots.
- Automated sandbox input held left mouse in the running app, which produced
  real brush dispatches.
- Runtime logs showed exact GPU feedback application, including strokes such as
  `319`, `437`, `514`, and `499` applied edit events.
- No `error`, `failed`, `critical`, `timeout`, device-removed, or feedback
  overflow/slot-skip log lines appeared during the smoke.

### Remaining Weak Spots

- Disk save/load for edit overlays is still not implemented.
- The fallback CPU persistence path remains approximate and is only used if GPU
  feedback cannot allocate a readback slot.
- Full manual validation should still include painting a bridge, forcing a
  render-window recenter, and returning to confirm visual replay across chunk
  streaming.

## Positioning and Recentering Stability Pass

The positioning audit found two concrete stability risks:

- `InfiniteChunkManager::Update` converted camera world coordinates with
  `static_cast<int32_t>`, which truncates negative positions toward zero. The
  render-window path used `floor`, so streaming and rendering could disagree
  around negative X/Y/Z boundaries.
- Vertical recentering shifted the render window on every Y chunk boundary.
  With a 4-chunk-high render window, this was more aggressive than needed and
  could force frequent cache invalidation while collision/raycast readbacks were
  still one frame behind.

### Coordinate Space Audit

- Stable world space:
  `cameraPos`, `renderCameraPos`, `playerFeetWorld`, `characterPreview.feet`,
  `brushHitWorld`, `groundHitWorld`, chunk origins, and persistent edit world
  voxel coordinates.
- Render-window local space:
  `cameraPosLocal`, `playerFeetRenderLocal`, GPU brush/ground ray origins,
  GPU raycast result positions, brush dispatch positions, physics scan center,
  chunk-copy destination offsets.
- Chunk space:
  `ChunkCoord`, `cameraChunk`, `activeRegionCenter`, loaded/generated chunk
  keys, persistent edit overlay keys.
- Local voxel space:
  per-chunk local voxel indices and local X/Y/Z from `ChunkCoord::LocalCoord`.
- Screen/camera space:
  mouse NDC, camera basis vectors, ray directions, third-person chase camera
  offset.

The invariant is now explicit: recentering changes only `regionOriginWorld` and
the active render-window chunk center. It must not mutate `cameraPos` or
character world position.

### Fixes

- Added `ChunkCoord::FloorDiv` and `ChunkCoord::LocalCoord` helpers.
- Added tests for zero, positive chunk boundaries, `-1`, `-64`, `-65`, high
  positive Y, and low negative Y coordinate conversion.
- Fixed `InfiniteChunkManager` camera chunk conversion to use `floor` before
  `FromWorldPosition`.
- Replaced per-Y-chunk recentering with vertical render-window hysteresis:
  the Y window shifts only when the camera gets near the bottom/top local
  margins.
- Added recenter invariant logs with old/new origin, delta, camera before/after,
  local position before/after, reason, and whether the camera changed.
- Added positioning overlay lines for camera world, render camera world,
  character world, render origin, player feet world/local, feet chunk/local
  voxel, active vertical center, and last recenter reason/delta.
- Ground snapping now rejects stale vertical readbacks and requires an upward
  ground normal, reducing vertical snaps from stale or non-walkable hits.

### Boundary Test Mode

Set this environment variable before launching Sandbox:

```powershell
$env:VENPOD_BOUNDARY_TEST='1'
```

The test enables flight, moves the camera across X, Z, and Y boundaries in a
repeatable pattern, logs recenter events, and reports any discontinuous camera
world-position jump beyond the scripted motion budget.

### Verification Update

Release build:

```powershell
.\build.ps1 -Config Release
```

Boundary smoke:

- launched Sandbox with `VENPOD_BOUNDARY_TEST=1`, diagnostics, and runtime log
- forced horizontal X recenter events in both directions
- forced horizontal Z recenter events in both directions
- forced vertical Y recenter events in both directions
- every `RECENTER_INVARIANT` line reported `changed=0`
- log scan found no `error`, `failed`, `critical`, `timeout`,
  `device removed`, `hung`, `discontinuity`, or invariant-violation lines

### Remaining Weak Spots

- The boundary test is a runtime smoke path, not a headless unit test.
- Vertical render-window movement is now less aggressive. Later validation found
  that flying far above the conceptual terrain range could still move streaming
  policy into empty generated space; that issue is addressed in the high-Y
  streamer clamp section below.

## Scanline and Snap Stability Fix

The follow-up visual audit found two separate issues that looked related in
gameplay but had different causes.

### Render Scanlines

The long horizontal and vertical lines across the world were caused by the old
`Renderer::RenderCrosshair` path. That function reused the fullscreen voxel
raymarch pipeline, set only four root constants, then drew two scissored
fullscreen triangles for the crosshair. Because the bound pixel shader was still
the voxel raymarch shader, those thin strips re-rendered world voxels with stale
or partial frame constants. The result was position-dependent terrain bands that
looked like scanlines.

Fixes:

- `main_launcher.cpp` and `main_sandbox.cpp` now draw the crosshair through the
  ImGui foreground draw list.
- `Renderer::RenderCrosshair` is now intentionally inert so an accidental future
  call cannot reintroduce the raymarch strips.

### Player Position Snaps

The recenter invariant remained stable: render-window origin changes did not
mutate `cameraPos`. The remaining visible teleport path was collision response.
Large upward ground-raycast corrections were treated as blocked movement and
rolled the camera back in X/Z while zeroing vertical velocity. In the vertical
world, those large corrections are often stale readbacks, cliff walls, ceilings,
or cache-refill edge cases, not walkable stairs.

Fixes:

- Large upward ground corrections are now rejected instead of snapping Y or
  rolling back X/Z.
- The existing relevance checks still require a nearby query origin, a still
  relevant player position, and an upward surface normal before normal ground
  snapping can occur.

### Verification Update

Release build:

```powershell
.\build.ps1 -Config Release
```

Runtime checks:

- normal Sandbox launch with diagnostics and runtime log enabled
- boundary-test Sandbox launch with forced X/Z/Y render-window recenters
- log scan found no `error`, `failed`, `critical`, `timeout`, `exception`,
  `device removed`, `hung`, `discontinuity`, or invariant-violation lines
- boundary recenter logs continued to report `changed=0`

Remaining risk:

- Brush and ground raycast result readbacks still use the older immediate
  readback API shape. The launcher now pairs the readback value with the query
  origin metadata that produced it, but a fuller cleanup should move these small
  readbacks to a fenced asynchronous ring, matching the newer GPU brush edit
  feedback design.

## Brush Targeting Correction

A follow-up pass tried to make traversal painting finish at the player's feet by
moving the entire brush ray origin from the camera eye to the collision/feet
point. That was the wrong level of abstraction. It made empty-air strokes fall
back near or under the player, and those newly painted voxels could intersect
the collision body and lift or snap the camera.

Fixes:

- GPU brush raycast targeting is eye/crosshair based again.
- Empty-air fallback placement is also eye/crosshair based and resolves at a
  fixed distance in front of the view, not under the player.
- Hit validation now measures against the completed query origin that matches
  the readback result.
- Ground collision readbacks are paired with the completed ground-query origin
  instead of the newest pending origin.
- Ground snap relevance thresholds were tightened so stale or offset results
  are rejected rather than used to move the player.

Traversal-friendly "paint a path to my feet" behavior should be implemented as
a brush placement policy later, using the eye ray for targeting and then
projecting/offsetting the final build volume intentionally. It should not be
implemented by moving the raw ray origin into the player's collision body.

## Close Traversal Brush Ramp

The first implementation of feet-origin painting made targeting unreliable
because it moved the source ray itself. The follow-up implementation keeps the
raycast eye/crosshair based and applies traversal behavior only at final brush
placement time.

Current behavior:

- Erase strokes keep exact raycast placement.
- Build strokes keep exact placement until the brush gets close to the player.
- Near the player, the brush center is smoothly lowered toward foot level.
- If the brush would land inside the player radius, it is pushed slightly
  forward along the view direction before dispatch.
- The close-ramp-adjusted brush position is used for both the compute dispatch
  and the preview while actively painting.
- Diagnostics report whether the close ramp is active and the final brush
  placement world position.

This gives sustained line-of-sight painting a short automatic ramp-down near
the player without reintroducing underfoot fallback placement.

### Verification Update

- Release build passed after the brush targeting/readback metadata fix.
- `ctest` found no registered tests in the current build tree.
- Normal diagnostics smoke launch exited cleanly.
- Extended boundary-test smoke crossed X, Z, and Y render-window recenter
  thresholds.
- Recenter invariant logs reported `changed=0` for player/camera world
  position across those shifts.
- Log scan found no `error`, `failed`, `critical`, `timeout`, `exception`,
  `device removed`, `hung`, `discontinuity`, invariant-violation, or
  corruption lines.
- A later Release build also passed after the close traversal brush ramp was
  added.

## Movement and Render Stability Audit

The next low-level review found two remaining failure classes.

### DDA Render/Raycast Stability

`PS_Raymarch.hlsl` and `CS_BrushRaycast.hlsl` both used DDA traversal that
started directly on voxel/grid boundaries and then assigned traversal distance
after incrementing `sideDist`. When the camera or ray entry point landed on an
integer voxel plane, that could make adjacent pixels/rays disagree about which
cell boundary had been crossed. Visually this shows up as position-dependent
line/plane artifacts, especially around horizontal or vertical voxel planes.

Fixes:

- Ray traversal now starts with a small epsilon offset from the entry point.
- DDA distance is captured before advancing the selected axis' `sideDist`.
- Raymarch hit distance now includes the grid-entry distance, keeping
  third-person avatar depth comparison consistent.
- Brush raycast traversal uses the same boundary-safe stepping logic, so
  targeting and rendering agree more closely.

### Fence-Safe Raycast Readbacks

The launcher collision path still depended on the older 16-byte raycast
readback API shape. The previous code copied the GPU result into one readback
resource and immediately mapped it from the CPU in the same frame. The copy had
only been recorded into the command list, not executed yet, so the CPU could
consume a stale result while pairing it with newer world/render-origin metadata.
That is exactly the kind of bug that can produce apparent teleporting when a
stale ground hit is accepted by collision.

Fixes:

- `VoxelWorld` now has a three-slot brush/ground raycast readback ring for the
  launcher path.
- Each swapchain frame writes raycast results into its own readback slot.
- The slot is only mapped after that frame index's fence has completed.
- Query metadata is stored per slot and retired with the matching GPU result.
- The old immediate API still exists for legacy call sites, but the Sandbox
  launcher now uses the fence-safe path.

Verification:

- Release build passed.
- Diagnostics smoke launch exited cleanly.
- Runtime log scan found no shader compile failures, device removal, timeouts,
  invariant violations, discontinuity reports, or critical/error lines.
- No registered CTest tests currently exist in the build tree.

## Render-Window Border Cleanup

The next visible artifact was a hard chunk/render-window boundary immediately
after spawn. Two causes were addressed:

- Missing chunk slots were not cleared before the streaming copy pass filled
  them. The render buffers are now cleared to air on first fill and after
  recentering, so not-yet-loaded chunks do not raymarch stale voxel data.
- The 4-chunk-tall render window had too little above-camera headroom for the
  extreme terrain. Terrain peaks could hit the top of the render volume and
  appear as a flat clipping plane. The render window was rebalanced from
  `17 x 4 x 17` to `15 x 6 x 15`, trading some horizontal edge range for a
  taller vertical slice around the player.

The chunk-copy refill order is now center-out, so the player chunk and immediate
neighbors are copied before far render-window corners during startup and after
recenter events.

## Far-Range Rendering Experiment

A later pass tried to extend apparent render distance by adding a procedural
far-terrain fallback inside `PS_Raymarch.hlsl`. The idea was useful for future
LOD work: after the dense editable voxel window misses, the shader can draw a
cheap non-editable distant silhouette so the world does not end abruptly.

That first implementation was not acceptable as the default. It ran in the
fullscreen raymarch path, generated a second terrain system that was not backed
by streamed chunks or persistent edits, and produced huge floating/extreme
mountain forms outside the real world. It also pushed frame time into hundreds
of milliseconds in the default sandbox.

Current status:

- The far-terrain fallback has been re-enabled as a cheap horizon-only
  silhouette behind `FAR_TERRAIN_HORIZON_ENABLED = true`.
- Default gameplay, collision, brush edits, and persistence still use only the
  dense streamed voxel window.
- The dense window remains bounded and editable; its current default is now
  `19 x 7 x 19` chunks after the view-distance increase below.

Future direction:

- Build far distance as an explicit LOD system, not a hidden per-pixel terrain
  generator in the main voxel raymarch.
- Generate/cache low-resolution terrain tiles from the same terrain source as
  real chunks.
- Fade between dense chunks and far LOD with fog/dither.
- Keep far LOD read-only and visually distinct until edit persistence supports
  committing changes back into loaded chunks.

## High-Y Streamer Clamp

A later movement smoke found another apparent teleport/empty-world path: if the
player flew above the conceptual terrain Y range, the raw camera chunk could
move above `TERRAIN_CHUNK_MAX_Y`. The chunk manager then centered load/unload
decisions around that out-of-range Y. Since all real terrain chunks were below
the unload window, the manager could delete the terrain below the player and
leave the dense render window looking empty.

The fix preserves the core positioning invariant:

- player/camera world coordinates are never clamped or mutated by streaming
- the raw camera chunk is still reported for diagnostics
- the chunk manager clamps only its vertical load/unload center
- the dense render window clamps only its vertical render center

For the current constants, high camera chunks now behave like this:

- raw camera chunk Y can continue above the terrain range, for example `14`
- stream center clamps to `7`, keeping a `4..10` loaded Y band available
- render center clamps to `8`, keeping a `4..10` dense visible Y band available

Verification:

- Release build passed.
- A 75-second diagnostics boundary run reached vertical phase 4/5 and flew to
  about world Y `953`.
- Logs showed raw chunks such as `[1,14,1]` while stream/render centers stayed
  clamped inside terrain-backed bands.
- Loaded chunks stayed nonzero and recovered through the vertical test instead
  of unloading the whole terrain set.
- Log scan found no `critical`, `error`, `failed`, `timeout`, device-removal,
  discontinuity, or recenter-invariant violation lines.

## Fast-Flight Render Refill Pass

Fast flying exposed a separate render-window policy problem. On every normal
recenter, the dense voxel ping-pong buffers were cleared to air, both copy
caches were invalidated, and the terrain was then copied back in under the
normal per-frame budget. That made the whole world visibly flash out before the
center-out refill caught up.

Fixes:

- Normal recentering clears stale dense voxel contents before refill. This can
  reveal temporary air where chunks are not copied yet, but it avoids the worse
  failure where old chunks are interpreted under a new `regionOriginWorld` and
  appear as floating islands or false collision/raycast targets.
- The current READ buffer is filled first during refill, because that is the
  buffer being rendered.
- Ping-pong swaps are temporarily gated while the render window is refilling, so
  the renderer does not alternate between a more-filled and less-filled buffer.
- Recenter/startup refill temporarily boosts the chunk-copy budget.
- Default streaming generation was raised from `4` to `8` chunks per frame, and
  the pending generation queue was raised from `256` to `1024`.
- The vertical load window now covers the full visible vertical render window:
  render below/above is now `4/2` chunks, load below/above is now `5/3`
  chunks, and unload below/above is now `6/4` chunks. Static assertions enforce
  that load distances cover render distances. This spends more of the dense
  buffer below the player so valleys and connected lower ground are visible
  instead of being clipped away while sky consumes most of the Y range.
- Chunk generation now rebuilds its pending queue when the streaming center
  moves, so fast traversal does not keep generating stale far chunks from an
  older camera position before filling the new near-field.
- Render-window recentering is deferred while the previous refill is still
  below critical coverage, preventing fast flight from repeatedly resetting the
  copied-chunk cache before the current window has filled.
- The defer gate now has a hard edge escape hatch: if the camera approaches the
  dense buffer edge, recentering proceeds even if refill is incomplete. This
  prevents fast movement from outrunning the render window and seeing the world
  appear as isolated islands.
- Vertical recentering now targets a ground-biased local eye height and is
  evaluated every frame, not only on chunk-boundary crossings. This lets the
  window settle back downward after high flight instead of wasting most of the
  vertical buffer on sky.

Verification:

- Release build passed.
- A diagnostics boundary/fly smoke ran through high-speed X/Z movement and
  vertical phase 4.
- The runtime log showed clears on recenter events; those clears are intentional
  until the dense buffer is replaced by an overlap-preserving chunk-slot/ring
  renderer.
- Loaded/copied chunk coverage recovered into the `1,300+` to `1,400+` range
  during fast vertical flight instead of staying around the old missing-layer
  `1,150` range.
- Log scan found no `critical`, `error`, `failed`, `timeout`, device-removal,
  discontinuity, or recenter-invariant violation lines.

Remaining limitation:

- This is still a dense editable near-field renderer, not Minecraft-style chunk
  meshes with independent persistent drawables. The current pass removes stale
  wrong-world contents and false collision targets, but true zero-pop streaming
  needs the next refactor: keep stable world chunk drawables alive outside the
  dense edit/raycast buffer and fade/crossfill dense chunks as they become
  editable.

## Render Window Rubberband Follow-Up

The latest review found that the remaining "teleport" reports were mostly
render-window artifacts, not direct mutation of `cameraPos`. The invariant logs
continued to report `changed=0`, but the dense buffer could still make the world
look like it jumped:

- On recenter, `regionOriginWorld` changed immediately.
- The old dense buffer contents stayed in place until the chunk-copy budget
  refilled the new window.
- During that gap, the raymarch shader interpreted old voxel slots as if they
  belonged to the new origin.
- Those stale voxels could render as floating island shapes and could also feed
  brush/ground raycast readbacks.

Fixes in this follow-up:

- Recenter clears both ping-pong buffers to air before refilling, so stale
  chunks are not rendered in the wrong coordinate frame.
- The copy pass prioritizes the currently presented READ buffer first.
- Once READ reaches critical coverage, WRITE is allowed to catch up so
  ping-pong swapping and physics can recover.
- Vertical render-window Y is clamped back to the terrain-backed band. The
  camera can fly above the terrain, but the dense world window no longer chases
  far above the generated terrain and repeatedly reloads empty Y space.
- The far-horizon shader path is enabled as a visual-only silhouette so the
  dense edit window does not read as a hard island edge from high viewpoints.
- Per-frame chunk-copy status logging was moved from info to debug to reduce
  runtime log overhead.

Verification:

- Release build passed.
- A 75-second boundary/fly smoke and a 50-second follow-up smoke crossed X, Z,
  and Y recenter paths.
- Recenter invariant logs continued to report `changed=0`.
- The high-Y phase showed raw camera chunk Y values above the terrain range
  while render chunk Y stayed clamped at the terrain-backed maximum.
- After the READ-first change, WRITE starvation was corrected by allowing WRITE
  refill once READ reaches critical coverage.
- Log scan found no `device removed`, `hung`, `timeout`, boundary
  discontinuity, or recenter invariant violation lines.

Remaining blocker:

- The clear-on-recenter behavior is a safe intermediate, not the final
  architecture. The correct long-term solution is a chunk-addressed render
  representation or GPU overlap-shift/ring-slot system so overlapping chunks
  survive origin shifts without either stale contents or air gaps.

## Hole And Streaming Follow-Up

The latest visual holes were a separate dense-buffer refill issue. The
render-window invariant was holding, but fast movement could still leave holes
or warped far silhouettes because the buffers were not both converging.

Root causes found:

- The refill path could mark the window "stable" after critical coverage even
  when hundreds of edge chunks were still missing from one or both ping-pong
  buffers.
- After that critical coverage point, the copy budget fell back to the normal
  small per-frame budget. Under fast flight, that made edge holes persist for
  too long.
- The hidden WRITE buffer could lag behind the presented READ buffer. When the
  renderer/physics later swapped buffers, the user could see chunks disappear
  even though READ had looked mostly complete.
- The active render center has hysteresis, so it can be offset from the raw
  camera chunk. The load margin now needs to cover that offset, not only the
  visible radius.
- The generation queue was previously cleared on camera-chunk changes. During
  fast movement this repeatedly discarded useful near-edge generation work.
- The far-horizon proxy was drawing through holes inside the dense editable
  window. That made missing chunks look like huge overhead or background spike
  geometry leaking into the near field.
- Normal chunk unload and deferred-delete paths emitted debug logs per chunk.
  In diagnostics runs, that produced large log bursts exactly when streaming
  pressure was highest.

Fixes in this follow-up:

- `UpdateChunks` now keeps calling `UpdateActiveRegion` until both READ and
  WRITE chunk caches reach the full expected visible count, not just critical
  coverage.
- `UpdateActiveRegion` keeps the boosted copy budget active while either
  ping-pong cache is incomplete.
- Recenter frames get a larger one-frame copy burst so a cleared dense window
  is repopulated faster before normal streaming budgets resume.
- WRITE catch-up now begins after an initial READ floor instead of waiting for
  75% READ coverage. This avoids the hidden buffer staying empty when fast
  vertical movement or missing generated chunks prevents READ from reaching
  the old threshold.
- The horizontal load/unload margins were widened to cover the render-window
  hysteresis offset.
- Pending generation work is no longer thrown away on every camera chunk move;
  stale queue entries are skipped when popped instead.
- The far-horizon proxy only renders when a ray misses the dense editable
  AABB. If a ray traverses the dense window and finds air, it returns sky
  instead of drawing proxy terrain through the hole.
- The far-horizon proxy is restricted to a horizon-angle band so it does not
  appear as overhead sheets or underfoot islands during steep look angles.
- Per-chunk unload/cache/delete logs were lowered to trace level.
- Per-frame copy pulse logs were reduced to periodic debug status; the overlay
  remains the source of live frame-by-frame streaming metrics.

Verification:

- Release build passed after the changes.
- Boundary smoke logs from the previous check showed no recenter invariant
  violations, no boundary discontinuities, and no device removed/hung/timeout
  errors.
- A follow-up 24-second diagnostics boundary run showed no device removed,
  hung, timeout, boundary-discontinuity, or recenter-invariant failures.
- Runtime metrics showed WRITE recovering with READ instead of staying at zero
  after the READ-first refill path. One sampled refill reached
  `READ=1186 WRITE=1186` and passed critical coverage.

Remaining limitation:

- Fast flight can still outrun a dense full-window refill because each recenter
  invalidates chunk-slot placement for the whole 19 x 7 x 19 editable volume.
  This pass makes the current architecture recover more aggressively and avoids
  false far-proxy geometry, but the true fix is still a chunk-addressed render
  representation: stable per-chunk slots, overlap-preserving origin shifts, and
  a separate visual far-LOD that is never used for collision, brush, or edit
  feedback.

## View Distance Increase

The dense editable render window was raised from `15 x 7 x 15` chunks to
`19 x 7 x 19` chunks:

- old dense capacity: `960 x 448 x 960 = 412,876,800` voxels per buffer
- new dense capacity: `1216 x 448 x 1216 = 662,241,280` voxels per buffer
- visible chunk slots: `1,575` to `2,527`
- load/unload margins: `+/-14` load and `+/-18` unload horizontally

This is not a literal `+/-14` dense-distance doubling. A literal double from
`+/-7` to `+/-14` would allocate `1856 x 448 x 1856`, or about `1.54B` voxels
per buffer. With two dense ping-pong buffers that is roughly `12.8 GB` before
source chunk buffers, descriptors, physics, swapchain, or edit feedback. That
is not a good default for a stable public demo.

The visual far-horizon proxy distance was doubled from `5,200` to `10,400`
world units. It remains visual-only and horizon-limited; it should not feed
brushes, collision, physics, or edit persistence.

Verification:

- Release build passed.
- A short diagnostics startup smoke stayed alive past startup with the larger
  buffers.
- Runtime log reported `VoxelWorld initialized: 1216x448x1216 grid (5054 MB)`.
- Runtime log reported `load: 14 chunks, render: 9 chunks, unload: 18 chunks`.

Next architecture note:

- The structure we want for much larger voxel view distance is a sparse voxel
  octree, sparse voxel DAG, or brick-map/BVH-style acceleration structure.
- The current dense box is simple and editable, but its cost grows with volume.
  Doubling horizontal dense distance costs about four times as much memory.
- A practical next step is not "make the dense box enormous"; it is to keep a
  dense editable near field and add a sparse read-only far field built from
  chunk bricks, mips, or an SVO/DAG-like hierarchy for ray traversal.
