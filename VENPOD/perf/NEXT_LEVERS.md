# Next perf levers (queued, from the sampling-profiler ground truth)

Order is provisional — RE-RANK from the POST-dedup profile (Codex's HeightAt dedup will reshuffle).
Method: VENPOD_PROFILE=1 VENPOD_PROFILE_STACKS=1 over mtns.rec (flythrough) + mtns_edit.rec (editing);
read PROF_TOP / PROF_CALLERS / PROF_HITCH in build/bin/venpod_runtime.log. Trust PROF_*, NOT the
PERF_SPARSE_STEPS bucket columns (those are inflated ~4x — clipInterest claimed 6.4ms vs real 1.47).

## DONE
- 3912997: HeightAt skips 6/9 distance-gated noise octaves (near terrain). Byte-identical
  (80089-pt checksum 0x4e5fa489816ed439), -15% main-thread CPU. Committed + verified.
- IN FLIGHT (Codex): HeightAt dedup/memo (pure function -> always-valid cache).

## LEVER A — RefreshStats heavy telemetry runs ~15x more than consumed  [SAFE, independent]
SparseClipmapTileCache::RefreshStats (SparseClipmap.cpp:8976). Lines 8982-8997 are cheap O(1) fields
needed every call. Lines 8998+ are HEAVY TELEMETRY-ONLY: a 245-tile scan + 16384-brick sweep
(m_voxelSlotByCoord) + queue/reservation loops + ~60 field copies. Already throttled once/frame
(m_statsHeavyRefreshOncePerFrame), but still runs EVERY frame even though the data is only consumed
by the PERF_SPARSE log + debug overlay, which fire only when
`editingThisFrame || slowFrameThisFrame || frameCount%15==0` (main_launcher.cpp:26484).
FIX: gate the heavy block by (will-log-this-frame || debugOverlayActive) — pass that predicate into
RefreshStats (or split into RefreshStatsCheap always + RefreshStatsHeavy on demand). ~5% steady-state.
Telemetry-only -> no correctness/visual risk. Verify: PROF_TOP RefreshStats drops; PERF lines on the
logged frames are unchanged (same values when they DO log).

## LEVER B — per-frame PERF logging is an observer effect on dips  [SAFE, independent]
RtlCreateUnicodeString ~3.9% steady (17% of dip frame 228) = spdlog formatting the giant EDIT_TELEM +
PERF_SPARSE_STEPS lines (main_launcher.cpp:26487/26537). The `slowFrameThisFrame` (body>30ms) trigger
means every DIP also pays the full string format -> logging makes the dip worse. During editing it
fires EVERY frame (editingThisFrame). FIX: throttle the heavy per-frame edit/perf logging (interval or
env-gate, default sparse), and/or format dip telemetry more cheaply. Keep a VENPOD_PERF_VERBOSE escape
hatch so profiling still gets per-frame lines. Pairs with Lever A (same gate).

## LEVER C — ExtractRegion (surface meshing)  [independent of terrain]
SparseSurfaceExtractor::ExtractRegion (SparseSurfaceExtractor.cpp:224): per-voxel brick scan + 6-neighbor
face test + emit. ~6% editing (top of dip frame 254 at 33%), ~1.7% flythrough. Calls NO terrain (0
HeightAt) -> dedup won't touch it. Already async-parallel. CPU options: cheaper neighbor test / fewer
emitted faces (greedy) / better culling. GPU migration is the GPU_SURFACE_EXTRACTOR_PLAN.md target but
carries the mid-mesh Step-4 readback-regression risk — prefer a CPU cut first.

## LOWER PRIORITY
- mid-mesh BuildMidHeightSurfaceSnapshot: 3 dips (edit frames 618/640/670, 47-55ms). It calls HeightAt
  heavily, so the terrain dedup should soften it for free. Re-check after dedup before doing more.
- GPU allocation/sync stalls (NtGdiDdDDIDestroyAllocation2 / WaitForSynchronizationObjectFromCpu):
  STARTUP/warmup only (edit frames 3/24, fly frame 28) — not steady-state. Deprioritized.
- clipInterest RefreshInterestTouchFrames (~3%): 12288 hash lookups/frame to stamp LRU. Throttle-able
  but DELICATE (premature eviction -> pop-in). Needs eviction-threshold safety analysis first.
