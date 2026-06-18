# GPU visual mesh extraction — Scope B plan (render-only, no readback)

Status: **Phase B0 (data contract)**. CPU mid-mesh baseline is tagged `midmesh-cpu-baseline`
and default-on; it is the reference/fallback for this entire pass.

## Why this, why now
Phase 0 profile (default-on build, `mtns_edit`): **CPU-bound** — sparse `body` mean 18.7 /
p95 39.9 / max 66.8 ms; `gpuMs` mean 7.5 / p95 11.2 / **max 29.4** ms (~9 ms avg headroom,
but the GPU already spikes near budget on some frames). Worst-frame CPU split:
`genPrep` 19.1 (terrain GENERATION — CPU-authoritative, deferred), `surfExtract` 15.5
(near-surface meshing — render-only), `clipInterest` 8.6, `reqPrep` 6.2; mid-mesh ~1 ms.

This pass proves a **render-only GPU extraction pipeline** on the lowest-authority path
(mid/far visual mesh) before touching the high-value near-surface `surfExtract`, and long
before the high-risk `genPrep`/terrain-gen authority question.

**Non-goal this pass:** GPU `HeightAt`/`genPrep`/terrain generation. It is CPU-authoritative
for collision, physics, edits, and render correctness; moving it implies readback, duplicate
generation, or byte-parity. Only revisit after the render-only pipeline is proven.

## Success criteria (gates for the whole pass)
- No synchronous readback in normal/shipped frames.
- CPU fallback path remains intact and is the reference.
- GPU path is toggle-gated (`VENPOD_MIDMESH_GPU_EXTRACT`, default 0).
- No holes, seams, or misplaced terrain vs the CPU path.
- No collision/edit behavior change (this path touches only the rendered mesh).
- No GPU p95/p99 regression that exceeds the CPU win.
- No new failure beyond the existing 28-test baseline.

---

## Phase B0 — DATA CONTRACT

The GPU extractor reproduces exactly what `SparseClipmapTileCache::BuildMidHeightSurfaceSnapshot`
+ `extractTileMesh` produce for one tile, on the GPU, writing into the *same* GPU face buffer
the CPU path already fills — so the existing `RenderSparseSurfaceFaces` indirect draw consumes
it unchanged.

### Input (per tile to extract)
1. **Tile identity**: synthetic `BrickCoord {x, ring, z}` + world origin `(originX, originZ)`
   + `cellSize`. (Same keys the range allocator uses today.)
2. **Height samples**: the tile's `packedSamples` — a `side × side` grid (`side = tileSampleSide`,
   `cellCount = side - 1`), one `uint32` per sample packing `material` + quantized `Y`
   (`UnpackMidHeightSurfaceSampleMaterial` / `...SampleY`). Made GPU-resident in a per-tile
   **height-sample buffer** (a structured buffer region keyed by tile slot). This UPLOAD
   REPLACES the current face upload — and it is SMALLER (side²·4 B ≈ 17 KB/tile vs ~4600·16 B
   ≈ 74 KB/tile of faces), so net upload bandwidth drops.
3. **LOD**: `mergeCells` (the camera-distance LOD, incl. the cached slope refinement). Computed
   CPU-side (`computeTileLod`) and passed as a per-tile constant — it depends on camera distance
   + the tile's content, both already known CPU-side cheaply.
4. **Finer-ring suppression**: `childMask` / `childResident[4]` (4 bits) — so the extractor skips
   quadrants covered by resident finer child tiles (the existing no-monolith rule).
5. **Edit footprint**: the per-tile set of edited cell boxes (`editXzBoxes` clipped to this tile)
   as a small per-tile mask/list, so edited cells emit nothing (the live voxel raymarch shows the
   edit there) — identical to the CPU `cellInEditFootprint` skip.
6. **Build version**: `midMeshBuildVersion` (config fingerprint) — a global constant; a change
   invalidates all GPU tiles.

### Output (per tile)
1. **Faces**: `SparseSurfaceFace[]` (16 B: `worldX/Y/Z` + packed `payload` = direction[3] |
   quadWidth[5] | quadHeight[5] | voxel[19]) written into this tile's region of the shared GPU
   face buffer. The vertex shader already expands each into 2 triangles from the packed extent +
   direction — **the draw path is unchanged**.
2. **Range + draw args**: `firstFace`, `faceCount`, `directionMask`, AABB — the same
   `SparseSurfaceBrickRange` / `SparseSurfaceDrawArgs` / `SparseSurfaceRecord` the CPU emits, so
   the existing range allocator + indirect draw + cluster cull consume GPU-produced tiles exactly
   like CPU-produced ones. (B2 lets the GPU also WRITE these for indirect draw.)
3. **Face count is variable per tile** → either (a) a conservative per-tile capacity + a GPU
   atomic append counter, or (b) a two-pass count-then-write. B1 picks one; the CPU path's
   per-tile face counts give the capacity bound.

### Edit / recenter / streaming behavior
- A tile re-extracts on GPU iff its CPU-side `meshContentVersion` (or `mergeCells`/`childMask`)
  changed — the SAME dirty set the CPU path computes. The dirty worklist + parallel pre-pass we
  shipped become "which tiles to dispatch a GPU extract for" instead of "which to CPU-extract".
- Edits upload the changed tile's samples (small) + re-dispatch its extract. No new authority:
  the edit store stays CPU; only the *rendered* mesh is GPU-built.
- Recenter / new tiles: dispatch GPU extracts for the new coords; old tiles' face regions persist
  (same range-allocator persistence the CPU path relies on). The `maxFaces=3M` headroom + the
  reseed safety hatch carry over.

### Per-tile serial / version
- `meshContentVersion` (per tile) + `midMeshBuildVersion` (global) gate re-extraction, exactly as
  today. A GPU-extracted tile records the version it was built at; the dispatch list is the diff.

### Fallback
- `VENPOD_MIDMESH_GPU_EXTRACT=0` (default) → the current CPU path, untouched, is the shipped build.
- `=1` → GPU extraction; CPU path remains compiled and is the A/B reference. The two must produce
  the same faces (validated by debug-only hash/AABB/face-count compare, below).

---

## Phases B1 → B4

- **B1 — GPU mid/far extraction prototype.** A compute shader that extracts the visual mesh for a
  small controlled set of mid/far tiles (start with a fixed handful, then the real dirty set),
  writing `SparseSurfaceFace[]` per the contract. CPU path stays available for A/B. No readback.
- **B2 — GPU cull / indirect draw.** Once B1 draws correctly, let GPU-produced geometry feed GPU
  cluster cull + `ExecuteIndirect` (the resource already supports `useGpuCull`/indirect). A natural
  extension of B1, not a separate project.
- **B3 — measure (A/B).** CPU body mean/p95/p99/max vs GPU; GPU mean/p95/p99/max; GPU queue stalls;
  upload/dispatch cost; sync waits; visual correctness; edit/recenter behavior.
- **B4 — decide extension.** If B holds and GPU headroom is acceptable, extend the same extraction
  pipeline to the near-surface `surfExtract` (~15.5 ms render-only peak — the meaningful win).

## Instrumentation (REQUIRED from B1, day one)
The profile shows GPU max already ~29 ms, so the GPU path must prove it doesn't trade a CPU win for
a worse GPU spike. Capture, per frame, behind the toggle:
- compute dispatch time (GPU timestamp queries around the extract dispatch)
- graphics GPU time (existing `gpuMs`) — and whether extract overlaps or serializes with rendering
- barrier/transition + sync time
- copy/upload time (the per-tile sample upload)
- CPU submission/record time
- GPU p95/p99 (not just mean)
Log as `GPU_EXTRACT` lines parallel to `MIDMESH_SELFTIME`. A win requires: CPU body drops AND GPU
p95/p99 does not rise by more than the CPU saving.

## Validation
- CPU path is the reference. Debug builds may do **optional, delayed/offline** readback to compare
  GPU vs CPU per-tile face counts, AABBs, and a face hash — never a required per-frame readback in
  the shipped path. Visual: `capvis` ground + altitude contact sheets, A/B CPU vs GPU, read for
  holes/seams/misplacement.

## Deferred to a later pass (NOT this one) — genPrep / terrain-gen authority
Open questions to answer only after the render-only pipeline is proven:
- Can CPU collision/edit authority stay CPU-side while the GPU generates *visual* terrain?
- Can GPU and CPU terrain stay byte-identical (collision/edit parity)?
- Can readback be avoided entirely? If not, can CPU generation become lazy/on-demand rather than
  per-frame?

## Roadmap
1. GPU visual extraction for mid/far terrain (B1+B2).
2. GPU culling / indirect draw integration (B2).
3. Extend visual extraction toward near-surface `surfExtract` (B4).
4. Only then revisit GPU `genPrep` / terrain-generation authority (separate pass).
