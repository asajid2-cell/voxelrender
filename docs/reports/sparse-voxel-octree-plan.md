# Sparse Voxel Octree Far-Field Plan

> Historical report: this page documents the earlier dense-near-field plus
> visual-far-SVO checkpoint. For the current sparse brick/surface-authoritative
> architecture and review gates, see
> [Engine architecture](../explanation/architecture.md) and
> [Sparse refactor review checklist](../reference/sparse-refactor-review.md).

VENPOD currently renders an editable dense near-field voxel window. That path
is good for painting, collision, physics, and persistence, but it scales by
volume. Increasing dense render distance from `+/-7` to `+/-14` would require
roughly four times the horizontal memory, before source chunks and transient
GPU work.

The sparse voxel octree direction is for visual far distance first. The dense
near field remains the source of truth for gameplay.

## Implemented GPU Node/Page Pass

The current implementation has moved past the implicit shader prototype. The
sandbox now builds a GPU-backed sparse far-field tree at startup:

- `FarVoxelOctree` builds a square forest of far pages around origin.
- Each page is a 1024-voxel root cube with an 8-child sparse tree below it.
- Child root nodes are packed contiguously, so the shader can traverse with
  `childBase + popcount(childMask before ordinal)`.
- A dense top-level page-index buffer maps page-grid coordinates to page
  records. The raymarch shader now walks page cells crossed by the ray instead
  of scanning every SVO page for every far-field pixel.
- Interior cells that are safely below sampled terrain are collapsed into
  coarse leaves, while surface/ravine/cliff cells are subdivided down to the
  configured depth.
- Node and page data are uploaded as structured buffers and bound to the
  fullscreen raymarch pass as `t2`, `t3`, and `t4`.
- `FrameConstants.farFieldParams` enables the pass and reports page count,
  node count, and page size to the shader. `farFieldGridParams` reports page
  radius, page-index side length, and the far-field root Y.
- The diagnostics overlay reports the far SVO state, page count, node count,
  page-index count, page size, and covered world size.

The first measured startup build produced 81 pages and 1,910,633 nodes, covering
9216 world units horizontally. This is a visual far-field representation only.
The dense streaming voxel buffer remains authoritative for editing, collision,
physics, raycast, and persistence.

## Why This Version Is Still Isolated

A gameplay-authoritative SVO/DAG renderer still needs additional
infrastructure:

- CPU or GPU construction of far-field nodes
- a compact node layout
- descriptor and buffer lifetime management
- camera-centered streaming of node pages
- invalidation when terrain generation changes
- optional far-field material/normal mips
- clear separation from brush/collision/readback systems

Adding that authority directly into the existing dense renderer would risk the
currently stable demo. The GPU node/page pass validates real buffer ownership,
root-signature binding, and tree traversal while keeping gameplay semantics in
the known-good dense path.

## Target Architecture

The target split is:

- dense editable near field:
  painting, physics, raycast, collision, persistence, high-frequency detail
- sparse visual far field:
  read-only terrain silhouette, far material color, fogged LOD, no gameplay
  authority

The sparse far field should eventually be one of:

- sparse voxel octree
- sparse voxel DAG for repeated/static terrain
- brick map with per-brick mips and a top-level BVH/octree

For VENPOD's terrain, a brick map plus octree/BVH top level is likely the most
practical next step. The terrain is chunked already, and brick pages match the
streaming model better than one monolithic tree.

## Proposed Node Layout

Candidate GPU node:

```cpp
struct FarVoxelNode {
    uint32_t childBase;      // first child index, or 0xFFFFFFFF for leaf
    uint32_t childMask;      // 8-bit occupancy mask
    uint32_t materialPacked; // dominant material / palette index / flags
    uint32_t boundsPacked;   // quantized origin/level or page-local index
};
```

Candidate page:

```cpp
struct FarVoxelPage {
    int32_t originChunkX;
    int32_t originChunkY;
    int32_t originChunkZ;
    uint32_t rootNode;
};
```

## Integration Rules

- Never use far SVO for brush hit tests until exact coordinate and edit replay
  semantics are implemented.
- Never use far SVO for collision or ground snapping.
- Never draw far SVO through a ray that already traversed the dense editable
  AABB and found air.
- Fog and dither transitions should hide the dense/far boundary.
- Far SVO pages should be allowed to lag behind; dense near-field streaming
  remains higher priority.

## Runtime Controls

- Default: far SVO enabled.
- `VENPOD_DISABLE_FAR_SVO=1`: disables the node/page far field and falls back to
  the older procedural far terrain path.

## Performance Follow-Up

The first GPU-backed SVO pass had a major shader-side scaling bug: every
far-field ray iterated the whole page list before entering any octree nodes.
That made cost scale as `pixels x pages x tree traversal`, which is not an
acceptable default once the page forest grows.

The current pass adds a page-index grid and changes far-field traversal to:

```text
ray -> 2D page-grid DDA -> page-index lookup -> per-page octree traversal
```

This keeps the SVO visual path bounded by the number of page cells a ray
actually crosses. The dense editable window remains the gameplay source of
truth, and the far SVO is still disabled automatically by adaptive quality when
frame time exceeds budget.

The procedural far-terrain fallback was also reduced to a cheaper continuity
layer. Before this pass it could run a second high-cost heightfield march after
the SVO path missed, including multiple occupancy probes per step. It now uses
fewer steps and only enables the extra occupancy-assisted stepping at the
highest far-field quality level.

The source-chunk budget was also corrected. The visible dense window is
`19 x 7 x 19 = 2,527` chunks, so a resident+queued cap below that value makes
full dense coverage impossible. The current runtime budget is visible chunks
plus a small queue margin. When that budget is full but the current visible
window still has missing chunks, the streamer now trims non-visible source
chunks first instead of allowing stale source chunks to block visible coverage.
This preserves the large dense render distance while avoiding the VRAM paging
behavior that looked like a near-crash after several seconds.

The dense recenter path also no longer clears the multi-GB voxel ping-pong
buffers. Recenter now clears a small per-chunk valid mask and the copy shader
marks each `64 x 64 x 64` slot valid as it is uploaded. Raymarch rendering,
brush raycast, and ground raycast check this mask before reading the dense voxel
buffer, so stale memory is ignored without paying a full-buffer clear. This
keeps the current dense architecture, but removes the worst recenter hitch and
documents the next possible refactor: a true two-window handoff that can fill a
new render origin in the background before presentation switches.

### Runtime Smoothness Follow-Up

The next hitch source was queue ordering and blocking synchronization rather
than raw render distance. The current pass keeps the large `1216 x 448 x 1216`
dense window and the 3,000-voxel raymarch distance, but changes how background
work is admitted:

- chunk unload no longer waits on generation fences; pending chunks are simply
  deferred and retried later
- dense raymarch distance and step count no longer jump down on one slow frame
- copy budget and far-field quality now adjust gradually under frame pressure
- chunk generation is budgeted separately from chunk copying
- generation is pumped after the frame has been queued, so it does not sit
  directly in front of the render pass on the direct queue
- the chunk-budgeted physics path no longer falls back to full dense-buffer
  physics if its indirect pipeline is unavailable
- startup skips the old full clear of both multi-GB dense voxel buffers; the
  first active-region fill clears the tiny chunk-valid masks instead
- diagnostics logging that used to run at `info` during movement, recentering,
  and painting has been moved to debug/non-flushing paths

## Verification

Release build succeeded. A diagnostics runtime smoke test confirmed:

- `PS_Raymarch.hlsl` compiled with the expanded root signature.
- the far voxel octree initialized successfully
- dense chunk generation, copy, edit, brush raycast, and physics shaders still
  compiled
- `CS_CopyChunkToBuffer.hlsl` marks copied chunk slots in the valid mask
- `CS_BrushRaycast.hlsl` ignores invalid dense chunk slots
- the startup path skips the old dense full-buffer clear
- the app shut down cleanly after a diagnostics smoke run
- no critical/error/failed/device-removed log entries appeared during the smoke
  run

## Known Limitations

- The far SVO is static around origin; it is not camera-centered streamed yet.
- The node data is built from a CPU approximation of the far terrain function,
  not directly from generated chunk buffers.
- It is read-only visual terrain. It does not participate in brush edits,
  persistence, collision, ground snapping, or physics.
- The page buffers use an upload heap for a simple safe first integration.
  A production version should upload once into default GPU memory.
- The shader still keeps the older procedural far terrain fallback for rays that
  miss the SVO coverage. That is useful for continuity, but a future pass should
  replace it with streamed camera-centered pages.
- Dense render-window recenter still changes origin immediately; the valid mask
  prevents stale rendering and removes the full-buffer clear, but a future
  two-window handoff would make fast-flight recentering visually smoother.

## Next Implementation Pass

1. Move far nodes/pages into default GPU buffers through a copy upload path.
2. Stream/rebuild page rings around the camera instead of keeping a static
   origin-centered page forest.
3. Generate far pages from the same terrain source used by chunk generation.
4. Add far-field counters for rays tested, pages tested, tree nodes visited,
   hits, misses, and fallback hits.
5. Add optional low-resolution material/normal mips for smoother far silhouettes.

## Runtime Cache And Physics Pass

This pass moved the dense editable renderer toward the long-term sparse brick
pool/page-table design without replacing the gameplay source of truth yet.

Implemented:

- Dense near-field slots are now toroidal. A world chunk maps to a stable
  physical slot by `worldChunk mod renderChunkGrid`, so overlapping chunks can
  survive render-window recentering instead of forcing a full rewrite.
- Chunk-valid masks became exact slot tags (`worldChunkX/Y/Z/valid`) instead of
  one-bit occupancy. The raymarcher and brush raycast now reject stale slots by
  exact world chunk, which fixes the old scanline/chunk-border artifacts caused
  by stale dense memory.
- The copy shader root signature was corrected from 8 to 12 DWORD constants so
  the world chunk tags are actually delivered to `CS_CopyChunkToBuffer`.
- Brush raycast and dense raymarch lookups now use world-space chunk lookup
  through the toroidal slot tags. This keeps camera/player world coordinates
  stable while the cache moves underneath them.
- GPU brush edit feedback now records which ping-pong buffer produced the edit
  events, so async persistence maps local render indices through the correct
  slot table instead of assuming the current READ buffer.
- The source chunk streamer now queues around the active render-window center,
  not only the raw camera chunk. This fixes the mismatch where the render cache
  requested chunks that the source streamer had not generated yet.
- Startup no longer leaves startup mode just because the first 128 queued
  chunks drained. It waits for the visible render target (`2,527` chunks) before
  enabling normal unload behavior.
- The chunk-generation allocator ring increased from 3 to 8 slots, and the
  adaptive generation budget can climb to 8 chunks/frame while frame time is
  healthy. This keeps the large render distance but fills the initial cache
  substantially faster.
- Chunk generation is now batched: the per-frame generation budget records
  multiple chunk dispatches into one command list and one fence signal instead
  of submitting one direct-queue command list per chunk. This reduces startup
  queue pressure and the near-crash feeling caused by many tiny GPU submissions.
- The generation queue now has an exact queued-coordinate set. This prevents
  duplicate pending chunks from making the source streamer look full while the
  visible render window is still missing pages.
- The render-copy pass queues missing visible chunks as urgent demand before
  the copy budget can early-out. Visible pages are no longer treated as
  speculative load-buffer work.
- The copy-skip test now trusts exact toroidal slot tags instead of legacy
  copied-chunk sets. This fixed the plateau where `cached` stopped near
  `1,900/2,527` even though the renderer still had visible page misses.
- Render-buffer stability now requires full visible coverage. The old 75%
  critical-coverage gate reduced early blanking but allowed permanent holes at
  the edge of the visible window.
- Far SVO data is cached on disk in the runtime directory. On this machine the
  far SVO startup dropped from roughly 2.3 seconds of CPU build time to about
  27 ms from cache.
- Far SVO loading is now asynchronous. The sandbox starts the CPU build/cache
  read on a background task, enters the main loop without waiting for it, then
  uploads the completed node/page buffers to the GPU when the task is ready.
  A cold no-cache smoke showed first frame logging before the far tree finished
  its roughly 4.1 second CPU build, so far SVO construction no longer blocks
  launch.
- HLSL compilation now has a local `.venpod_shader_cache` bytecode cache keyed
  by shader source, recursive includes, entry point, target profile, defines,
  debug flag, and optimization level. Warm launches avoid recompiling unchanged
  DXC shaders and create compute pipelines from cached bytecode.
- The adaptive runtime budget no longer starves source generation while visible
  pages are missing. Chunk generation stays at the high batched budget until
  the visible source set is resident, while copy and far-field quality can still
  scale under frame pressure. The controller now also carries a predicted frame
  cost from the previous CPU/GPU timing sample instead of reacting only to a
  single raw frame.
- GPU timestamp queries now measure the main command list. The overlay and
  runtime log report total GPU frame work, pre-render work, raymarch cost, and
  UI/readback cost alongside CPU fence/present timings.
- Runtime `PERF` log lines now report FPS, frame ms, CPU phase times, copy and
  generation budgets, generated/source records, queue pressure, cache coverage,
  exact toroidal page misses, urgent visible queues, missing generated/loaded
  chunks, checked/skipped chunks, physics scan work, scheduler prediction, and
  GPU timings. Use
  `VENPOD_LOG_FILE=1` to capture these without enabling verbose diagnostics.
- Infinite physics is default-on but remains local and budgeted. Idle physics
  now scans a smaller local window, while brush/edit activity pins a dirty
  region near the edited world position for short, higher-priority follow-up
  scans. This is still a chunk-region scheduler, not a per-voxel active-region
  physics solver.

Verification:

- Release build passed.
- A 10-second D3D debug smoke ran without root-signature, device-removed,
  critical, failed, or timeout log entries.
- A cached-startup smoke reported:
  - shader bytecode warm launch: compute pipeline creation dropped from
    multi-second DXC compilation to millisecond-scale cached pipeline creation
  - far SVO async source `cache`; observed CPU read about `21.8 ms` and GPU
    upload about `3.8 ms`
  - cold no-cache far SVO build did not block first frame; first `PERF frame=0`
    logged while the background build was still running
  - startup phase complete at `2,528` generated chunks for a `2,527` chunk
    visible target
  - source records `2,623`, queue `0`
  - render cache converged to `cached=2527/2527/2527`
  - exact page misses converged to `pageMiss=0/0`
  - render-copy source misses reduced to `missingGen=0`, `missingLoad=0`
  - idle local physics scan reduced from `256/256` to about `100/128` chunks
    with physics still default-on
  - GPU timestamp query reported steady raymarch work around `1.7-2.0 ms` and
    main command-list GPU work around `2.2-2.5 ms`
  - steady frame logs around 6 ms on the test RTX 3070 Ti run, with physics
    default-on and dirty-region scheduled

Remaining limitations:

- The dense editable representation is still a large ping-pong voxel volume.
  The toroidal slot tags make it behave more like a page table, but the memory
  backing is still dense.
- The dense copy pipeline still has to populate both ping-pong buffers before
  the scene is fully editable/simulatable. The current pass prevents permanent
  holes and stale slots, but the real long-term fix is still a sparse brick
  pool/page table so unchanged pages are not copied as dense `64^3` bricks.
- Far SVO CPU build/cache read is asynchronous, but GPU upload still happens as
  one main-thread handoff when the task finishes. The observed upload is small
  on the test system, but a production version should upload into default GPU
  memory through an explicit upload ring and amortize very large far-field
  updates across frames.
- The next major architecture step is a real sparse brick pool: allocate only
  resident `64^3` bricks, keep a GPU page table from world chunk to brick index,
  raymarch through the page table for the near field, and reserve the dense
  buffer only for edit/physics staging or remove it entirely.
- The current near field still uses dense ping-pong storage with toroidal
  page tags. This pass did not honestly replace it with a sparse brick pool.
  The code now has the telemetry needed to justify that refactor, but the next
  implementation must change the shader binding model from `VoxelGrid +
  ChunkValidMask` to `BrickPool + PageTable`.
- Voxel clipmaps are still represented by the sparse far SVO plus procedural
  far fallback, not by a formal multi-ring clipmap hierarchy. The next pass
  should add explicit rings with per-ring resolution, upload cadence, and
  page-table residency metrics.
