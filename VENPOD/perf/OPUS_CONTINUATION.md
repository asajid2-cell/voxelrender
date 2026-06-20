# VENPOD Production-FPS Continuation (Opus, cross-context)

> Single source of truth for resuming the 120-FPS convergence task across fresh contexts.
> A context rollover is NOT task completion. Resume execution; do not issue a task-final response.

## Source / build state
- Repo: `z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD`  branch `wip-edit-latency`  HEAD `1899031`
- Uncommitted (experimental infra, all gated default-OFF): `src/Simulation/SparseClipmap.cpp`, `src/Simulation/SparseClipmap.h`, `src/main_launcher.cpp`
- Binary: `build/bin/VENPOD.exe` (Release). Tests: `build/bin/VENPODTests.exe`
- 10 physical / 16 logical cores.

## Build command (cmd-direct; build.ps1 is broken — vswhere temp issue)
```
cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat\" -arch=amd64 -host_arch=amd64 > NUL && cd /d z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build && cmake --build . --config Release --parallel"
```

## Run / benchmark
- Recordings: `build/bin/mtns.rec` (flythrough, ~480 frames), `build/bin/mtns_edit.rec` (editing, ~671 frames).
- Replay: `.\playrun.ps1 -Path build\bin\mtns_edit.rec` (sets VENPOD_REPLAY, VENPOD_VSYNC=0, edit telemetry on; no rebuild).
- Env for clean runs: `VENPOD_EXIT_AFTER_FRAMES=<n>`, `VENPOD_PERF_SUMMARY_LOG_INTERVAL=1`. Log lands at `build/bin/venpod_runtime.log` (copy/tag per run).
- Protocol: Release, no debug layer/PIX/readback/per-frame disk log for clean runs; ignore first 120 warm-up frames; >=5 interleaved A-B-B-A; reject if same-binary control drifts >3%. Report p50/p90/p95/p99/p99.9/max + bucket counts (>8.33/10/12.5/16.67/25/33/50ms).
- KNOWN MEASUREMENT DEFECT: back-to-back launches degrade buildMs ~2-3x (system contention). Insert a build/analysis gap between runs, or build a same-process deterministic edit-burst bench (Loop D4, unbuilt). This is a harness defect to control, NOT a termination condition.

## PROMOTED production config (THE win — keep intact)
- `parallelSurfaceExtraction` default-ON: `main_launcher.cpp:1302` `ReadUIntEnv("VENPOD_SPARSE_SURFACE_PARALLEL_EXTRACTION", 1u)`.
- A/B (clean): editing p50 77->102fps, dips>33ms -68%, >50ms -87%; flythrough p50 67->104fps, dips>33ms -89%.

## Experimental flags (ALL default-OFF; infra retained, gated)
| Env | line | status |
|---|---|---|
| VENPOD_MIDMESH_DIRTY_REGION_EXTRACT | main_launcher.cpp:3166 | region dirty-rect extract; interior bit-exact, border has ~6 mismatches (interior-only gate at SparseClipmap.cpp:7910) |
| VENPOD_MIDMESH_DIRTY_REGION_VALIDATE | :3168 | full re-extract + multiset compare per dirty tile; logs MIDMESH_DIRTYREGION_MISMATCH, falls back to full on mismatch |
| VENPOD_MIDMESH_ASYNC_REMESH | :3172 | route reusable tiles through budget+deferral — REFUTED (backlog diverges) |
| VENPOD_MIDMESH_EXTRACT_SCRATCH | :3175 | per-worker scratch — neutral |
| VENPOD_MIDMESH_WORKSTEAL | :3178 | work-steal pre-pass — ~15% on heavy builds, washes out at frame level |
| VENPOD_MIDMESH_EXTRACT_MAX_WORKERS | :3161 | worker cap — neutral/unhelpful |
| VENPOD_MIDMESH_LOD_CACHE(_VALIDATE) | :3155 | computeTileLod cache — correct but useless (0.1ms) |

## Baseline distributions (promoted build)
- Flythrough: p50 ~9.6-9.9ms/~101-104fps, p95 ~20.7, p99 ~24-31, frames>33ms ~0-4.
- Editing: p50 ~9.8-10.0ms/~100-102fps, p99 ~43.7ms, frames>33ms present. **EDITING TAIL = the unmet gate.**

## Targets
- Intermediate: p50<=8.5, p95<=10, p99<=12.5, zero engine frames>16.67ms.
- Final: p50<=8.33, p95<=9, p99<=10, zero visible stutter, bounded queues/memory, exact converged geometry.

## Root cause (clean-data, confirmed)
- Dips = CPU mid-mesh re-extraction of content-dirty (edited) fine tiles. ~35 fine tiles/build * ~3.3ms each; even parallelized ~6.5x => ~17.8ms spike.
- Extraction ALREADY parallelizes ~6.5x (frame 670: 127ms serial -> 17.8ms@16w). Gate is WORK-bound: need fewer fine tiles re-meshed OR cheaper per-fine-tile.
- Region dirty-rect cuts a fine tile ~3.3ms -> ~0.2ms. THIS is the bounded work-reduction lever.

## PROGRESS (this session) — STEP 1-3 DONE, border splice SOLVED
- STEP 1 coverage math DONE: of 531 re-extractions, keyMis 63% (pre-pass territory), boundary 32% (the addressable win), interior 4%. The p99-driving editing builds are ~80-100% boundary tiles. Material → proceeded.
- STEP 2 border splice DONE — THREE root-cause fixes, validator 0-mismatch x3 over full editing replay incl borders:
  1. FROZEN-CULL OVERRIDE: region re-extract + validator reference use the tile's CACHED cull params (eff* locals override distanceCullBounds/blockCullBounds), not the current camera. The face SET is camera-cull-dependent and the cache freezes build-time cull (computeMeshCacheHit ignores cull); mixing current-cull in-rect with frozen-cull out-of-rect caused EXTRA/missing=0 mismatches. SparseClipmap.cpp: eff* decl ~6741, lambdas ~6790/6823, set/restore around the region extract.
  2. FOOTPRINT-TOUCH -1: rect low edge `lx0 = floor((b.minX - tileOX - 1)/cs)` to match cellInEditFootprint's INCLUSIVE overlap (box edge on a cell boundary marks the cell below as edited → full skips it).
  3. INCLUSIVE FAR-EDGE FILTER: rect-loop box filter uses `> tileOX+tileSpan` (not `>=`) so a box whose min edge lands on the tile's far border isn't dropped while cellInEditFootprint still marks that border cell. THIS was the dominant border class.
- STEP 3 build lifecycle DONE: hybrid via shared `computeRegionSpliceRect` lambda (~6741) used by BOTH the pre-pass skip and the main loop. Pre-pass now runs WITH region on (removed `!dirtyRegionExtract` gate) and skips only tiles that will actually splice (status==2, small rect); new/recenter/LOD/too-large misses still get the parallel full pre-pass. Build count A=18 vs B=20 (NOT inflated; the old 16->36 is fixed). Interior-only gate REMOVED (`if (drrSmall)`→ now `if (regionStatus==2)`).
- drr counters: `drr=used/keyMis/cullMis/noBox/noInt/bound/large`. Validate run: used=138 keyMis=16 large=0 (down from 335 keyMis / 51 large pre-hybrid).
- DIAGNOSTIC INSTRUMENTATION still in the validator mismatch branch (cull-param dump + symmetric face-diff + inFoot flag) — harmless (only fires on mismatch), consider trimming before final commit.

## STEP 4 RESULT (clean control, drift ~2%) — HYBRID IS NEUTRAL → REVERTED, PIVOTED
- A-B-A-B editing A/B, control now clean (A-vs-A p50 drift 2.3%, p99 0.8%): B (hybrid) ≈ A (baseline).
  A p50 13.6-13.9 p99 ~71.9 >50ms 31-38 | B p50 13.5-13.8 p99 ~70.2 >50ms 41-53. p99 only ~2% (below the 15% gate); >50ms slightly WORSE in B.
- DRIFT-IMMUNE per-tile fact (validate timing): region extract is 5.74x cheaper than full (regionSum 61.5ms vs fullRefSum 352.7ms, same tiles same process). REAL but doesn't move the frame.
- ROOT CAUSE the speedup didn't translate: baseline main-loop extractMs ≈ 0 — the PROMOTED PARALLEL PRE-PASS already extracts edited tiles off the main thread (~16x parallel). The hybrid MOVED those tiles to a SERIAL region splice in the main loop (extractMs 10-14ms), so 5.74x-per-tile lost to 16x-parallel; serial region stacked onto the worst frames (>50ms regression).
- The editing-tail p99 frames are dominated by `clip` (clipInterest, 12-16ms degraded) and `gen`, NOT mid-mesh extract. e.g. frame 671: prep 20.7 / sparse 19.7 / clip 16.2; frame 251: gen 17.4.
- DECISION: region splice REVERTED to gated default-OFF (VENPOD_MIDMESH_DIRTY_REGION_EXTRACT=0). Production = promoted baseline, intact. The region infra is bit-exact + validated (4x 0-mismatch) and retained gated for the case where extraction ever becomes serial/dominant. Do NOT re-pursue region for the editing tail — extraction is already pre-pass-amortized.

## PIVOT (next context) — attack the REAL editing-tail driver: clipInterest + gen
- The editing-tail p99 is clip/gen-bound, not extract-bound. NEXT: attribute cleanly (settled machine), then target clipInterest (`sparseSplit clip`) — can it be incremental (update only edit-changed interest) instead of a full per-frame rebuild? And `gen` (terrain generation prep) on streaming frames.
- MEASUREMENT: this session's machine is contention-degraded (~20 launches; clean baseline editing p50 ~10ms/p99 ~43ms, degraded shows p50 ~13.7/p99 ~72). The A-vs-A control finally settled to ~2% after analysis gaps. Next context: start fresh (clean machine), re-baseline, then attribute the editing tail with MIDMESH_SELFTIME + sparseSplit correlation on the slowest frames.
- Find clipInterest in SparseClipmap*: grep `clipInterest` / `ClipInterest` / `interest`. Find `gen` = generation prep (perfSparseGenerationPrepMs / genPrep).

## clipInterest ENTRY POINTS + characterization (pivot starting point)
- Call site: `main_launcher.cpp:12410` `sparseClipmapTileCache.UpdateInterest(...)`, timed from `:12402` (sparseClipmapInterestStart) -> the `clip` field in PERF `sparseSplit=req/gen/clip/trim`.
- Definition: `SparseClipmap.cpp:1888` `SparseClipmapTileCache::UpdateInterest(...)` — large (~hundreds of lines): voxel interest lines, anchors (terrain/footprint/camera), sort/emit, backlog pump, async generation.
- REUSE early-out: `SparseClipmap.cpp:1991-2012`. `BuildInterestSignature(...)` is built from CAMERA pos/forward/velocity/prediction + policy — NOT from edits/content. If `signature == m_lastInterestSignature` (or intervalReuse over `policy.interestUpdateIntervalFrames`), it returns early (m_interestReusedLastFrame=1, cheap).
- THEREFORE: clipInterest is cheap on a STATIC camera, but does a FULL rebuild whenever the camera MOVES. Both replays move the camera every frame -> full rebuild every frame = the 12-16ms `clip`. This is a CAMERA-MOTION cost, hits BOTH flythrough AND editing (so optimizing it helps both gates).
- PIVOT LEVERS (next loop, needs clean machine):
  1. INCREMENTAL interest: on small camera moves most interested tiles persist; only the leading/trailing ring edge changes. Rebuild only the delta (add new-edge tiles, drop trailing) instead of the full set. Correctness-preserving. DRIFT-IMMUNE measurement: compute incremental + full in the same frame, diff the resulting interest sets (must match), and time each separately (like the region validate pattern).
  2. `policy.interestUpdateIntervalFrames` >1 = interval reuse (skip N frames). Risks stale interest -> tile pop-in/holes; only safe with predictive prefetch margin. Lower-effort but correctness-sensitive.
- DO clean-attribute FIRST (settled machine): confirm `clip` is the dominant editing-tail AND flythrough cost (degraded data says yes: clip 12-16ms is ~75% of prep on heavy frames), and check `m_interestReusedLastFrame` (PERF interestReused) to confirm it's NOT reusing during the replays (camera moving).

## clipInterest MEASURED (env-gated VENPOD_CLIPINTEREST_PROFILE=1, in-process phase timers — drift-immune) — CORRECTS the "12-16ms" attribution
- UpdateInterest is NOT 12-16ms. Measured: rebuild frames ~3.13ms (25% of frames; interval=2 so rebuild every other moving frame), reuse frames ~0.9ms. Avg ~1.4ms. The "clip 12-16ms" in sparseSplit was EXTERNAL CONTENTION (the degraded session: raw frame 65-107ms vs prep ~20ms) + a wider clip-window, NOT UpdateInterest.
- Rebuild split (CASE C): heightMs 0.036 (negligible) | RefreshStats 0.75 | UpdateVoxelInterest 2.34 (DOMINANT). Reuse split: RefreshInterestTouchFrames 0.36 (CASE A REFUTED — not dominant) | RefreshStats 0.52.
- So clipInterest's two pieces: (1) UpdateVoxelInterest 2.34ms on 25% of frames (~0.6ms weighted); (2) RefreshStats heavy telemetry 0.5-0.75ms EVERY frame.
- WHY NOT a clean win (do NOT rush either):
  - RefreshStats heavy block (SparseClipmap.cpp:8903, gate ~8931) is ENTANGLED with gameplay SIDE-EFFECTS: it calls PruneAsyncVisibleReservations AND prunes m_visiblePriorityVoxelSet (erases, ~8997-9008) inline with the telemetry sweeps. AND missingInterestedTiles (cheap 245-tile sweep) is consumed for a gameplay budget decision (main_launcher.cpp:13817) every frame. The expensive 16384-brick voxel sweep feeds only logging (main_launcher 24xxx). Gating/moving needs to separate prune side-effects from telemetry + verify every gated field is log-only -> non-trivial, bug-prone. NOT a tail-of-session change.
  - UpdateVoxelInterest incremental = the user's CASE C, residency-correctness risk (pop-in/holes).
- VERDICT: clipInterest is ~1.4ms, fixes are entangled/risky for a ~0.5ms sub-gate win, and it is NOT the mission p99 driver. Per "do not polish clipInterest indefinitely" -> RERANK.
- INSTRUMENTATION LEFT (all env-gated, production-safe): CLIPINTEREST profile log (VENPOD_CLIPINTEREST_PROFILE), drrTime in BUILDSPLIT (prints 0 when region off), validator mismatch dump (only on validate mismatch). Trim before any final/public commit; harmless to production (all default-off).

## FULL SPARSE-PHASE ATTRIBUTION (existing PERF_SPARSE_STEPS timers, editing replay post-warmup, by SUM = where time goes)
| phase | sum | p50 | p95 | max | note |
|---|---|---|---|---|---|
| postWaitRegion | 2201 | 3.7 | 12.9 | 39.5 | = perfPostWaitGapMs, a GAP not a phase = EXTERNAL CONTENTION stall (PERF_UNTRACKED gaps=postWait). ~0 in a clean env. NOT optimizable VENPOD work. |
| clipInterest(window) | 1856 | 4.0 | 9.3 | 18.7 | perfSparseClipmapInterestMs wraps the WHOLE clip window (main_launcher:12402+). UpdateInterest itself = only ~1.4ms (measured). The other ~2.6ms = SetFarSvoFallbackMetadata + the PREDICTED-VISIBLE-ADMISSION block (12423+). |
| surfExtract | 1326 | 2.0 | 8.3 | 17.9 | perfSparseSurfaceExtractMs (near-voxel surface extract; distinct from mid-mesh BUILDSPLIT extractMs). |
| reqPrep | 735 | 1.3 | 3.5 | 7.6 | already sub-split in PERF_SPARSE_REQ (terrainCrit/hierarchy/statsFlush/pressureTrim/.../hiddenExact). |
| genPrep | 445 | 0.2 | 4.8 | 11.7 | already sub-split in PERF_GENPREP (asyncExactApply/worker/parallelWall). |
| clipPump | 17 | ~0 | ~0 | 4.4 | NEGLIGIBLE (earlier "4.8ms sustained" was a single startup frame). |
| midMeshUpload/midSnapshot | 215/150 | low | | | |
- KEY: there is NO single dominant editing-tail lever. After removing the contention gap (postWaitRegion), the tail is a SUM of modest phases (clipInterest-window ~4, surfExtract ~2, reqPrep ~1.3, genPrep, predicted-admission ~2.6). Reaching 120Hz = BROAD small wins (the user's pivot #4: "~1.5-1.7ms for 120Hz"), not one big fix. clipInterest (1.4ms) was correctly de-prioritized.
- CLEAN-ENV NEED: the degraded session makes postWaitRegion (contention) the apparent #1. A fresh clean machine is needed to see the TRUE clean p99 phase mix. In-process phase sums (above) are the best available proxy and already rank the VENPOD work.

## RANKED NEXT TARGETS (by VENPOD-phase sum, contention excluded)
1. PREDICTED-VISIBLE-ADMISSION (~2.6ms in the clipInterest window, main_launcher.cpp:12423+, `enableSparseMidClipmapPredictedVisibleAdmission`). FIRST: verify it's actually running in the replay (conditions at 12426-12431: asyncVisibleCriticalGeneration, ownershipMissPct==0, gate-opened) and time it directly (split the clipInterest window: UpdateInterest vs the rest). If it rebuilds view bases + enumerates predicted-visible coords every frame, incrementalize or skip when the predicted view is unchanged. ~2.6ms p50 = meets the 0.5ms gate if cleanly cut.
2. surfExtract (perfSparseSurfaceExtractMs, ~2.0ms p50) — find its definition + whether it's dirty-incrementalizable.
3. reqPrep sub-phases (PERF_SPARSE_REQ already logs the split — parse it to find the dominant sub-phase, likely terrainCritical or hiddenExact, then dirty-queue it).
4. genPrep async-apply (PERF_GENPREP split — asyncExactApply on edit frames).

## (HISTORICAL) RERANK -> PIVOT #1: reqPrep / hiddenExact
- Evidence: PERF sparseSplit `req` was 2.81-7.80ms on heavy frames (bigger than clipInterest's pieces). The user's NEXT PIVOT ORDER #1 = "reqPrep / hiddenExact full-set work -> dirty/revision queue".
- METHOD (works in the degraded env): instrument reqPrep/hiddenExact with the SAME in-process phase-timer pattern (env-gated profile log) to identify the actual O(N) op BEFORE changing behavior (the user's mandate — do not infer from the aggregate `req` timer). Find reqPrep: grep `reqPrep` / `perfSparseRequestPrepMs` / `hiddenExact` / `HiddenExact` in main_launcher + SparseClipmap. Split into sub-phases, log per frame: which phase dominates, how many frames do full-set work, entries scanned vs changed. Then incrementalize the dominant phase via a dirty/revision queue, validate same-frame full-vs-incremental set equality, A/B the phase timer + production frame p50 at PRODUCTION log interval (NOT interval=1).
- MEASUREMENT DISCIPLINE: frame-level totals are contention-noise in this session; trust in-process phase timers + same-frame full-vs-incremental compares. For the production A/B use default log interval (don't set VENPOD_PERF_SUMMARY_LOG_INTERVAL=1 — that changes RefreshStats/telemetry frequency).

## clipInterest CODE STRUCTURE (scoped — SparseClipmap.cpp:2015+)
- After the signature reuse early-out, the rebuild does: `m_interestSet.clear(); m_generationQueue.clear(); m_queuedSet.clear();` then for EACH ring (policy.BuildRings()), builds anchors = {camera} + motion-lookahead steps + look-bias + predicted, and for each anchor inserts every tile coord within `radius` (a disc) into m_interestSet + queue via queueHeightCoord (hash insert + m_slotByCoord lookup). Then a VOXEL interest pass (lines further down) does the same for voxel bricks. So the cost = O(rings × anchors × radius^2) hash-set clear+reinsert + slot lookups, EVERY frame the camera moves.
- INCREMENTAL design sketch (next loop): keep m_interestSet across frames; when the camera moves by < tile, the disc per anchor barely shifts -> recompute the union of anchor discs and diff against the previous union (add newly-covered coords, drop no-longer-covered). The multi-anchor union complicates a pure ring-shift; simplest correct approach = rebuild the coord union cheaply (it's deterministic from anchors) but only do the expensive queueHeightCoord work (slot lookup + queue) for the DELTA vs the previous union. VALIDATE drift-immune: build incremental m_interestSet + a full one in the same frame, assert set-equality, time each.
- IMPORTANT MEASUREMENT NOTE: in the degraded session, RAW frame time (65-107ms) is dominated by EXTERNAL contention (~50-87ms unaccounted vs prep ~20ms) -> frame-level A/B is noise here. Use the IN-PROCESS phase timer (`clip` in PERF sparseSplit) to measure clipInterest work directly; it is robust to external contention. (This is also why the region frame A/B read as neutral noise — trust phase timers, not raw frame totals, until the machine is clean.)
- ALSO worth a clean-env check before the big incremental rewrite: `policy.Config().interestUpdateIntervalFrames` — if it is 1, bumping to 2-3 WITH the existing predictive-prefetch margin may cut clip cost ~2-3x at low pop-in risk (cheap experiment, A/B the `clip` phase timer + watch visibleMissing/pop-in). Lower-effort first probe than the full incremental rewrite.

## (HISTORICAL) STEP 4 blocked-on-env note — superseded by the result above
- The interleaved A-B-B-A editing A/B ran but the machine is contention-degraded (~15 back-to-back launches): p50 15-18ms (vs clean ~10ms), and the two A controls drifted 68% (p99 143 vs 85) → INVALID per the >3% control-drift rule.
- Degraded hint (NOT promotable): B (hybrid) tail lower than A — B p99 ~81 vs A ~114, B max 109/133 vs A 370/157, B >50ms 64/90 vs A 120/70. p50 ~flat. Suggests the hybrid cuts the editing tail without a p50 regression, but must be re-measured clean.
- NEXT: let the machine settle (analysis gap), then a TIGHT clean A/B: A-B-A on mtns_edit.rec (reject if A-vs-A control drift >3%), then A-B on mtns.rec (flythrough non-regression). Parser: `perf/abparse.sh <log> [warmup=120]` (uses raw frame ms = 2nd value of ms=X/Y). If env won't settle, build the same-process deterministic edit-burst bench (Loop D4) per the directive.
- Promotion gate to honor: 0 mismatch (DONE), no build inflation (DONE 18 vs 20), no flythrough regression, editing p99 improves >=15%, frames>25/33ms materially down, p50 no regression. If it wins → flip `VENPOD_MIDMESH_DIRTY_REGION_EXTRACT` default to 1 (main_launcher.cpp:3166) + keep validate default 0. If not → revert to default 0 (already gated) and pivot (STEP 5).

## HIGHEST-VALUE BRANCH (in progress): hybrid dirty-region extractor
A. Region extraction for cache-valid edited tiles (drrKeysOk && editBox).
B. KEEP existing parallel full extraction for keyMismatch/new/LOD/streaming tiles (do NOT disable pre-pass globally — that doubled build count 16->36 last time).
C. Serial full only below the measured parallel threshold.

Region/validator code: `SparseClipmap.cpp:7860-8016`. Interior-only gate: `7910` (`drrInterior`). Border-inclusive splice logic already present (`7934-7942`) but unreachable behind `drrInterior`. drr counters logged in MIDMESH_BUILDSPLIT (`8174`): `drr=used/keyMis/noBox/noInt/bound/large`.

## Validator status
- Interior region: 0 mismatches (bit-exact splice), confirmed.
- Border region (interior gate removed): ~6 residual multiset mismatches (small dups +3..+24). NOT yet diagnosed face-by-face.

## Latest hypotheses
- FAILED: global dirty-region (pre-pass off) — build count 16->36, p99 42->57. Reverted.
- FAILED: async/budget defer — backlog diverges (staleAge 32f). Reverted.
- OPEN/PROMISING: hybrid (region for interior+border edited tiles, pre-pass for new) reaches editing p99 gate IF border splice -> 0 mismatches.

## EXACT NEXT STEPS (resume here)
1. STEP 1 coverage math: run editing replay with `VENPOD_MIDMESH_DIRTY_REGION_EXTRACT=1 VENPOD_MIDMESH_DIRTY_REGION_VALIDATE=1`, parse MIDMESH_BUILDSPLIT drr counters -> table {total reExtract, used, keyMis, noBox, noInt, bound, large} + drrMs split. Compute projected region coverage over (a) all reExtract, (b) cache-valid edited, (c) ms-weighted, (d) p95/p99-frame-weighted. Continue only if projected p99 reduction is material.
2. STEP 2 border splice: at `SparseClipmap.cpp:7973` mismatch branch, emit symmetric multiset diff (faces only-in-full vs only-in-merged) with tile coord, edit box, scan box, face orientation/material/block coord, near/far edge, corner class. Classify (dup retained / missing replacement / wrong boundary ownership / corner double-ownership / forward-riser fence-post / material / stale revision). Fix one class at a time. Remove interior-only gate (`7910`) ONLY after all border classes pass with 0 mismatches over the full editing replay.
3. STEP 3 build lifecycle: prove build count NOT inflated (was 16->36) before A/B. Per-tile: at-most-one active build per revision; no rebuild after publication w/o newer content revision; coalesce same-frame edits; stale completion can't re-dirty; region publication clears same state as full.
4. STEP 4 hybrid A/B: A=promoted baseline, B=border-safe hybrid; A-B-B-A then B-A; both recs. Promotion gate: 0 mismatches, no build-count inflation, no flythrough regression, editing p99 improves >=15%, frames>25/33ms materially down, p50 no regression.
5. STEP 5 bounded pivot if hybrid fails: (1) dirty sub-tile/chunk meshing + 1-cell halo, (2) per-block cached face ranges, (3) latest-revision job coalescing, (4) temporary exact edit patch published immediately while full builds.
6. STEP 6 steady-state: reprofile promoted build for the ~1.3-1.7ms p50 gap (readiness queries, hiddenExact prep, clipmap-interest, edit apply, completion publish, hash lookups, allocs, queue sort, telemetry format). Promote repeatable >=0.25ms or p95/p99 wins.

## Raw-log paths
- `build/bin/fps_prod_edit_final.log`, `build/bin/fps_prod_mtns_final.log` (latest clean prod, contention-degraded p50 12).
- `build/bin/fps_wsteal.log`, `build/bin/fps_w16.log` (worker A/B).
- Memory ledger: `C:\Users\Ahmed\.claude\projects\z--328-CMPUT328-A2-codexworks-301\memory\venpod-fps-attribution.md`.

## Invariants (do not violate)
- No quality reductions (raymarch steps stay 192; no distance/resolution/bg-scale cuts).
- CPU extractTileMesh is authoritative reference; production stays on validated path.
- No region-with-border in production until validator = 0 mismatches with validate-OFF.
- Promote only measured, correctness-preserving wins; a p99-worsening mean gain is NOT a win.
- ms=X/Y is smoothed/raw frame time, NOT cpu/gpu.
- Zero AI commit attribution.
