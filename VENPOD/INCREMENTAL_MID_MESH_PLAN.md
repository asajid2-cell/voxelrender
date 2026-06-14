# Incremental / chunked mid-mesh — design (wip-edit-latency)

## Why (measured)
The mid-height mesh is ONE monolithic ~1.5M-face CPU snapshot
(`SparseClipmapTileCache::BuildMidHeightSurfaceSnapshot` @ SparseClipmap.cpp:6382),
re-extracted in full and re-uploaded whenever ANYTHING changes:
- an edit (height-dirty serial bump) -> full 1.5M-face rebuild (~60-86ms)
- terrain streaming in (looking around) -> serial bump -> full rebuild
- camera move/turn -> full rebuild (turn now gated; translation re-centers)

Telemetry: `PERF_SPARSE_STEPS midMeshUpload=50-86ms` is the dominant frame spike;
GPU is idle (gpuMs 7-27ms). User: "fine if I don't move; look/turn -> 10fps". The
throttle (commit 1198e8d) is a bandaid; the real fix is to chunk the mesh.

## Key facts (investigated)
- The build ALREADY iterates per tile (`m_tiles`, slot-based cache, `maxTiles`) and
  emits each tile's faces, then concatenates into one `SparseSurfaceGpuSnapshot`.
- `SparseSurfaceGpuSnapshot` (SparseSurfaceCache.h:144) = flat `faces` vector + a
  BrickCoord-keyed hash table of ranges. It is ALREADY per-brick-organized.
- `SparseSurfaceGpuResources` (shared by near surface AND mid mesh) already has
  `StageDirtyPayloadSnapshot` (incremental, near surface uses it) vs `StageSnapshot`
  (full, mid mesh uses it). So GPU-side incremental update already works.
- WRINKLE: per-tile LOD (`mergeCells`) depends on CAMERA DISTANCE (tileCamDist), so a
  tile's faces change as the camera translates -> can't naively cache per-tile across
  camera moves. BUT edits/streaming change only ONE tile's CONTENT, independent of
  camera -> per-tile dirty re-extraction fixes the two user complaints directly.

## Design — per-tile mesh chunks with dirty re-extraction
Treat each clipmap tile as a mesh CHUNK keyed by its tile coord (mapped to a synthetic
BrickCoord for the existing per-brick snapshot/range machinery).
1. Persistent per-tile face cache: keep each resident tile's extracted faces +
   the (cameraDistanceBucket, contentSerial) they were built at.
2. Dirty set: a tile is dirty when its content changes (edit overlap or stream-in =
   the tile's packedSamples regenerated) OR its camera-distance LOD bucket changes.
   - EDIT: mark only the overlapped tile(s) dirty (the InvalidateEditedHeightTiles
     path already computes which tiles an edit touches - reuse it).
   - STREAM: mark the newly-resident tile dirty.
   - CAMERA: mark tiles whose LOD bucket changed (most don't for small moves).
3. Each frame: re-extract only DIRTY tiles into their cache slots; emit a dirty-payload
   snapshot of just those tiles; `StageDirtyPayloadSnapshot` + `EmitCopy`. Draw all
   resident tile chunks (unchanged chunks keep their GPU faces).
4. Remove the monolithic `BuildMidHeightSurfaceSnapshot` + `StageSnapshot` per-change.

## Phasing (verify each via PERF_SPARSE_STEPS midMeshUpload + Codex visual)
- P1: per-tile extraction into a persistent cache + dirty tracking for EDITS and
  STREAM (the user's two complaints). Keep camera-move on the existing throttle for
  now (LOD bucket change). Edits/streaming -> only dirty tiles re-extract+upload.
- P2: camera-distance LOD bucketing so camera moves only re-extract bucket-changed
  tiles (kills the translation rebuild too).
- P3: drop the throttle bandaid entirely once P1+P2 hold.

## Risk / invariants
- Hole-free: the build's finer-ring suppression (childResident) couples tiles; a tile's
  emitted faces depend on whether FINER child tiles are resident. So a child becoming
  resident/non-resident dirties the PARENT tile too. Track that dependency or
  conservatively dirty the 4 parents on child residency change.
- The synthetic tile->BrickCoord key must not collide with real brick keys in the
  snapshot hash (use a disjoint coord space / ring bits).
- Verify face count + visual parity vs the monolithic build (Codex view_image) at
  ground + altitude; the mid mesh is the always-present terrain floor (minDistance 0).

## Status
- [x] Diagnosis + throttle bandaid (1198e8d) + turn-gate (a61108b)
- [x] P1 per-tile face cache (9c6f5b1) — DONE + VERIFIED. Cache keyed by content
      version (GenerateTile bumps -> edits/stream/regen invalidate only that tile) +
      mergeCells LOD + childMask finer-ring residency + origin/ring + buildVersion.
      This folds in P2 (LOD bucketing) since mergeCells is in the key. MEASURED:
      mesh-on-edit 60-86ms EVERY frame -> p50 0.1ms; LOOKING/MOVING 68.6 -> 12.5ms
      (15 -> 80 fps). Codex hole-check PASS (no tile-cache artifacts). Edits/streaming
      now re-emit ONLY the changed tile.
- REMAINING (polish, not the core overhaul):
  - P1.5 incremental UPLOAD: the build still CPU-reassembles+uploads the full
    snapshot when it runs (~23ms throttled spike). Faces+clipmap-samples are
    entangled in one snapshot; needs the range-allocator dirty-payload path
    (StageDirtyPayloadSnapshot) + dirtyBricks from cache misses. Then drop the
    throttle and run per-frame.
  - clipInterest ~19ms/frame during EDITING is now the editing bottleneck (the
    mid-VOXEL clipmap interest scan) — separate system from the mesh.
