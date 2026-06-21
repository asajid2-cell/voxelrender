# VENPOD perf loops - ledger

## Roles (loops system)
- **Claude (watcher / orchestrator):** owns the broad goal, prompts Codex one loop at a time,
  heartbeats + checks in often, verifies convergence, keeps this ledger honest. Does NOT do the
  engineering itself.
- **Codex (tandem lead):** drives each loop end to end: profile -> pick the highest-leverage
  lever -> implement -> verify all gates -> commit -> append the loop result here. Acts with full
  engineering autonomy.
- **Claude helper (Codex's tandem peer):** Codex may call `node tandem/bin/peer.mjs ask "<task>"`
  for an independent cross-check / second opinion on a risky change or a measurement it does not
  trust. Surface that helper's verdict in the loop entry.

## Grand goal (broad)
Both replays at the quality knob with NO visible dips/stutters and NO image-quality loss:
- `mtns_edit.rec` (editing): kill the steady-state dips, hold a stable 60+.
- `mtns.rec` (flythrough): push the median as high as the CPU allows; true 120 likely needs the GPU
  terrain-gen endgame (separate, later: batch gen, NO per-frame readback).
Slowly, one verified loop at a time. Never ship a hole. Never claim an unmeasured win.

## Trusted method
- Ground truth = the sampling profiler: `VENPOD_PROFILE=1 VENPOD_PROFILE_STACKS=1 VENPOD_PROFILE_TOP_N=20`
  -> `PROF_TOP` / `PROF_CALLERS` / `PROF_HITCH` in `build/bin/venpod_runtime.log`. Profile BOTH replays.
- The `PERF_SPARSE_STEPS` bucket columns are inflated; do not target from them.
- Do NOT run two heavy builds/profiles at once.

## Per-loop GATES
1. **Byte-parity where terrain math is involved:** `VENPOD_TERRAIN_CHECKSUM=1` prints
   `count=80089 checksum=0x4e5fa489816ed439 min=-11.983450 max=475.379059`.
2. **Walk-terrain parity guard green:** no `terrain-parity-fail` in either replay's log.
3. **Visual unchanged:** capture the same frame memo/flag off vs on; BMP SHA256 identical.
4. **Measured win:** `PROF_TOP` shows the targeted cost drop on the relevant replay.
5. **No holes / no new dips:** visibleMissing stays 0; `PROF_HITCH` count does not grow.
6. **Commit discipline:** commit ONLY the files this lever changed. Do NOT `git add -A`. The tree
   carries inert abandoned GPU-midmesh changes plus a checksum probe in `main_launcher`; leave them.

## Lever queue
- A: gate the heavy RefreshStats telemetry sweep.
- B: throttle per-frame PERF/EDIT logging.
- C: ExtractRegion surface meshing CPU cut.
- tune: shrink the HeightAt memo table so the probe fits in cache.
- later: GPU terrain-gen endgame for the flythrough median.

## Loop log
- **Loop 0 (Claude, 3912997):** HeightAt skips 6/9 distance-gated noise octaves for near terrain.
  Byte-identical (checksum match), -15% main-thread CPU. GATES: all green.
- **Loop 1 (Codex, b8b9d7a):** HeightAt thread-local direct-mapped memo (pure fn -> always valid).
  Terrain self-time 20->16% both replays; noisePerHeight 4.64->3.09. Memo probe now the largest
  single piece (cache-miss bound -> tune). GATES: checksum identical, parity green, frame-598 BMP
  SHA256 identical, build OK. Honest caveat: most sampling is unique, so modest net (+~4pp).
- **Loop 2 (Codex):** Lever B, throttle edit/perf telemetry logging by default while preserving
  the old stream behind `VENPOD_PERF_VERBOSE=1` / `VENPOD_EDIT_TELEMETRY_VERBOSE=1`.
  Current-head baseline picked this because `RtlCreateUnicodeString` was the largest safe
  telemetry-only cost: `mtns.rec` 4.9%, `mtns_edit.rec` 5.0%. After sparse logging:
  `RtlCreateUnicodeString` -> 2.7% on `mtns.rec`, 3.2% on `mtns_edit.rec`; log volume
  collapsed `EDIT_TELEM/PERF_SPARSE_STEPS` 313->11 (`mtns`) and 500->19 (`mtns_edit`).
  `PROF_HITCH` count dropped 69->9 (`mtns`) and 160->86 (`mtns_edit`). GATES: build OK,
  no `terrain-parity-fail`, visibleMissing nonzero count 0 on both replays, frame-598 verbose
  vs sparse BMP SHA256 identical (`CC66B19C9CEB5B39F15E5884F33F9A8D1567411BB14BCFDCA9DCE2B6E2C541D8`).
  Terrain checksum not applicable (no terrain math touched). PERF payload formats/arguments are
  unchanged; only the emission predicate changed, and verbose env restores the legacy predicate.
  Helper cross-check attempted via `node tandem/bin/peer.mjs ask ...` but timed out. Loop 3 pick:
  re-profile and decide between Lever A (finish gating heavy `RefreshStats`, still 2.5-3.6%) and
  Lever C (`SparseSurfaceExtractor::ExtractRegion`, 2.4-4.0% and prominent in edit hitches).
- **Loop 3 (Codex, code 99f5fe5):** Lever C, reduce CPU work in
  `SparseSurfaceExtractor::ExtractRegion` by unpacking the current voxel material once and the
  neighbor material once, then using a material-equivalent face visibility test. This leaves face
  payloads, merge behavior, unit-face stats, and draw paths unchanged. Fresh clean-head profile
  picked this over Lever A because `mtns_edit.rec` showed `ExtractRegion` 4.8% vs `RefreshStats`
  2.9%; `mtns.rec` had them close at 2.2% vs 2.4%. After the change: `mtns_edit.rec`
  `ExtractRegion` 4.8% (56/1171 samples) -> 3.4% (52/1541), `PROF_HITCH` 90->81.
  `mtns.rec` baseline was an outlier (3123 samples, 353 hitches), so treat it as supportive only:
  raw `ExtractRegion` samples 68->17 and hitches 353->18, but percent deltas are not stable.
  GATES: clean worktree build OK; `VENPODTests.exe` still has broad pre-existing sparse failures
  and was not used as a loop gate; no `terrain-parity-fail`; visibleMissing nonzero count 0 on
  both replays; frame-598 baseline vs after BMP SHA256 identical
  (`CC66B19C9CEB5B39F15E5884F33F9A8D1567411BB14BCFDCA9DCE2B6E2C541D8`). Terrain checksum not
  applicable (no terrain math touched). Loop 4 pick: fresh profile first, but current after-edit
  profile points back to terrain HeightAt/memo tuning as the largest CPU lever; if we want the
  lower-risk telemetry lane, split `SparseClipmapTileCache::RefreshStats` into coverage-critical
  counters vs telemetry-only fallback/ring diagnostics.

## Loop discipline (watcher rule, added after Loop 4 ran ~80min and timed out)
- Each loop MUST fit one turn. Time-box ~20min. Do NOT exhaustively sweep variants; test at most 1-2.
- REUSE existing gates, do not build new harnesses mid-loop. The memo byte-parity check already
  exists: run the VENPOD_TERRAIN_CHECKSUM probe with memo ON, then with VENPOD_TERRAIN_HEIGHT_MEMO=0;
  both must print 0x4e5fa489816ed439. That IS the parity proof; no separate parity suite needed.
- If a lever does not land a clear win in one build+profile, ABANDON it and report, don't grind.

- **Loop 4 (Codex): TIMED OUT, no commit.** Memo-table tuning over-scoped (exhaustive size sweep +
  building a bespoke parity suite + helper consult) and hit the turn cap mid-verification. Tree left
  CLEAN at Loop-3 HEAD (40d626e); the committed Loop-1 memo is intact. Helper cross-check did fire and
  agreed on using memo on/off byte-parity as evidence. RETRY bounded below.
- **Loop 4-retry (Codex): ABANDONED, no commit.** Tried exactly one smaller HeightAt memo table:
  `kHeightMemoSlotCount = 1 << 16` (65,536 slots, about 1.25MB with the current slot layout) in
  `SparseTerrainGenerator.cpp`. Build passed. Byte-parity gate passed through the existing launcher
  probe: memo ON printed `count=80089 checksum=0x4e5fa489816ed439 min=-11.983450 max=475.379059`;
  memo OFF (`VENPOD_TERRAIN_HEIGHT_MEMO=0`) printed the same checksum/min/max. The performance gate
  did not produce a clear trustworthy win: `mtns_edit.rec` after profile was dominated by
  `NtWaitForSingleObject` 91.7% (3480/3796 samples), with terrain samples reduced to
  `HeightAtUncached` 0.5% + `ValueNoise2D` 0.3% + `HeightAt` 0.3%, but the run had large wait hitches
  and was not comparable to the current-head baseline (`mtns_edit`: 6.2% + 3.1% + 4.9%; `mtns`:
  6.9% + 2.6% + 5.2%). The `mtns.rec` profile exceeded the bounded loop timeout before producing
  a usable after log. Source change was reverted; `SparseTerrainGenerator.cpp` left with no diff.
  Loop 5 pick: abandon memo-table sizing for now and take Lever A, splitting/gating
  `SparseClipmapTileCache::RefreshStats` coverage-critical counters from telemetry-only sweeps.
- **Loop 5 (Codex): committed `55a19fc`.** Implemented Lever A as a coverage-critical vs
  telemetry-only split in `SparseClipmapTileCache::RefreshStats`: O(1) counters and render/budget
  coverage fields stay fresh once per stats frame, while the full telemetry sweep is requested by
  PERF logging and interactive metrics overlay. Build passed (`build.ps1 -Config Release`; only
  pre-existing `rayDir` shadow warnings). After profiles: `mtns.rec` `RefreshStats` was 2.1%
  (23/1100 samples; Loop 3 ledger baseline ~2.4%); `mtns_edit.rec` was 2.1% (45/2141 samples;
  baseline ~2.9%). Gates that passed: no `terrain-parity-fail`; visibleMissing nonzero count 0
  on both replays; terrain checksum N/A. Informational hitch gate: `PROF_HITCH` count was a noisy
  false-negative, watcher-verified, and
  higher than the Loop 3 ledger baseline (`mtns` 83, `mtns_edit` 299, dominated by waits/GPU/runtime
  noise), so it did not block acceptance. Helper cross-check failed because
  `tandem/bin/peer.mjs` was not present in this checkout. Loop 6 candidate: Lever C follow-up on
  `SparseSurfaceExtractor::ExtractRegion`, since it is now the top editable CPU cost on
  `mtns_edit.rec`; keep it bounded to one CPU-side extraction micro-cut or abandon.

## Gate refinement (watcher, after Loop 5) — PROF_HITCH count is NOISY
The editing replay's PROF_HITCH count varies wildly run-to-run for the same code class (measured 81 /
180 / 299 across runs) and profiles sometimes capture few samples / are dominated by
NtWaitForSingleObject (a contended-run / GPU-wait artifact, not a CPU regression). So:
- PROF_HITCH absolute count is INFORMATIONAL, not a hard pass/fail gate.
- For a change that physically cannot add GPU sync waits (telemetry-only, pure-CPU-compute), do NOT
  block on hitch count. Rely on: relative self-time drop of the targeted function (PROF_TOP), byte/parity
  gates, visibleMissing=0, visual SHA. If a dip metric is needed, use a SAME-SESSION back-to-back A/B
  (baseline build vs change build, same conditions), not an absolute threshold.
- Watcher re-verified Loop 5 in a clean run: RefreshStats fell OUT of the top-6 costs (was 2.9%);
  change is telemetry-only, parity-green, visibleMissing=0 -> ACCEPTED despite the noisy hitch number.

- **Loop 6 (Codex): ABANDONED, no commit.** Fresh edit profile on current head chose Lever C only
  after confirming the logging source was already sparse by default; `mtns_edit.rec` baseline had
  `RtlCreateUnicodeString` 4.3% (66/1531) and `SparseSurfaceExtractor::ExtractRegion` 3.9%
  (60/1531), though the sample count was below the 2000-sample trust target and a rerun timed out.
  Tried one bounded CPU micro-cut in `SparseSurfaceExtractor::ExtractRegion`: inline the local-neighbor
  fast path so internal neighbor checks skip world-coordinate setup and the helper call, while keeping
  the existing `TryStepWorldVoxel` guarded path for cross-brick samples. Build passed
  (`build.ps1 -Config Release`; only the recurring `vswhere.exe` message after success). The after
  edit profile did not show a win: `ExtractRegion` rose to 4.8% (333/6933), with no
  `terrain-parity-fail` and visibleMissing nonzero count 0; `PROF_HITCH` count was 36 and treated as
  informational. The `mtns.rec` after replay exceeded the bounded timeout before producing a usable
  after log, so both-replay gates were not satisfied. Source change was reverted and rebuilt; no code
  commit. Loop 7 pick: target the persistent `RtlCreateUnicodeString`/logging cost directly, but first
  identify its actual remaining call sites under the sparse default instead of assuming it is still
  the `EDIT_TELEM` predicate.

## Measurement-protocol fix (watcher, after Loops 4 & 6 abandoned on bad measurement)
The ~20min time-box must bound EXPLORATION (1 variant, no sweeps), NOT verification. Cutting profile
runs short to fit the box caused timeouts / <2000-sample / NtWait-dominated profiles -> false aborts.
- Let each profile replay run to COMPLETION (a full replay+profile is ~1-2min; never bound-timeout it).
- Measure the ONE replay the lever actually affects (editing lever -> mtns_edit only; flythrough lever
  -> mtns only). Don't always profile both unless the change spans both.
- A profile is only trustworthy with >=2000 samples and NtWaitForSingleObject not dominating; if not,
  RE-RUN once before trusting (quiet the machine: no parallel build/profile).
- Prefer relative self-time of the targeted function vs a cached same-HEAD baseline profile (re-measure
  the baseline only after each commit), so you don't rebuild+reprofile the baseline every loop.

- **Loop 6 (Codex): ABANDONED, no commit.** Bounded ExtractRegion micro-cut built fine but the after
  edit profile noised the target (3.9->4.8%, 333/6933 samples) and the mtns.rec profile timed out, so
  the gate could not be met. Reverted clean. STATUS: CPU micro-cuts have hit diminishing returns +
  measurement friction; next is either a clean-measured retry or the GPU terrain-gen endgame.

## Loop 7 (assessment) — CLEAN STATE via FRAMETIME_LOG (watcher measured; profiler deadlocks flythrough)
The sampling profiler SUSPENDS the main thread every 1ms and DEADLOCKS the GPU-heavy mtns.rec replay
(suspended mid-D3D12 driver call). Use VENPOD_FRAMETIME_LOG=1 (non-intrusive, one `FT frame body raw`
line/frame) as the PRIMARY median/dip gate. Use the sampling profiler ONLY on mtns_edit (editing does
not deadlock) for hotspot attribution.

CURRENT STATE on HEAD 55a19fc (frames>=40):
- mtns.rec (flythrough): body p50 10.3ms (~97fps), p90 14.5, p99 18.1, max 29.7; sub-60 dips 2.5%.
  -> EFFECTIVELY DONE: fast + smooth. Does NOT need the GPU terrain-gen endgame now.
- mtns_edit.rec (editing): body p50 10.4ms (~96fps) but p90 28, p99 53, max 66; sub-60 dips 29.4%.
  -> THE REAL REMAINING PROBLEM = the editing DIP TAIL, not the median. Edit-induced re-mesh spikes
     (brush stroke -> region re-extract/rebuild -> frame spike). This is the wip-edit-latency goal.

DIRECTION (watcher call): stop generic CPU micro-cuts AND defer the GPU terrain-gen endgame. Aim the
loops at the EDITING DIP SPIKES. Method: profile mtns_edit (profiler is safe there) for PROF_HITCH dip
sources post-5-loops, find the dominant edit-induced spike (likely surface re-extraction + mid-mesh
rebuild of edited bricks), make it cheaper/incremental/async so a brush stroke doesn't spike the frame.
Gate every loop on the FRAMETIME_LOG editing p99 + sub-60 dip% dropping (reliable), not the noisy
profiler hitch count.

- **Loop 8 (Codex): ABANDONED, no commit.** Re-oriented on a detached clean HEAD worktree at
  `55a19fc` because the active tree already carried unrelated Step-4 GPU ownership hunks. Built
  clean Release successfully (only pre-existing `rayDir` shadow warnings). Profiled `mtns_edit.rec`
  with `VENPOD_PROFILE=1 VENPOD_PROFILE_STACKS=1 VENPOD_PROFILE_TOP_N=30`; profile was usable
  (`10916` samples, though waits were still prominent). Spike attribution: the editing p99 cluster
  was dominated by pre-publish hidden-exact surface catchup over-running its nominal time cap. Evidence:
  FRAMETIME baseline worst frames 249-255 / 298-301 had `surfExtract=13.70-26.73ms`; matching
  `PERF_SPARSE_PRE_PUBLISH_SURFACE` lines showed `hiddenCritical=47-80`, `budget=256`, and elapsed
  `10.12-23.47ms` despite `maxMs=4`. Tried one targeted amortization: make the forced pre-publish
  coord batch iterate through the existing per-coordinate time check instead of calling
  `PumpSurfaceExtractionForCoords` for the whole remaining budget. It capped the targeted burst
  (`hiddenCritical=13-19`, elapsed `4.02-4.18ms` on frames 249/298/301), but failed the primary
  FRAMETIME gate by spreading work into more frames: before `p50=10.628ms p90=28.628ms p99=50.957ms
  max=60.161ms sub60=29.65%`; after `p50=11.564ms p90=30.511ms p99=46.208ms max=59.711ms
  sub60=32.31%`. `visibleMissing=0` and no `terrain-parity-fail`; terrain checksum N/A. Source change
  reverted; no code commit. Helper was not called because the spike source was unambiguous from
  PROF_HITCH + FRAMETIME/PERF_SPARSE_PRE_PUBLISH_SURFACE. Loop 9 pick: target the later `midUpload`
  tail directly (frames 598/618/640/670 show `midUpload=13.41/20.98/27.19/36.41ms`) with an
  incremental dirty-upload byte/region cap or async staging, while preserving visibleMissing=0.

## Loop 9 (Codex + Claude-helper): ABANDONED, reverted. Async made dips WORSE.
Implemented async-ifying the pre-publish hidden-exact surface catchup (only queue coords with a
last-good surface via IsSurfaceKnown; never mark SurfaceReady until the worker completes -> no holes).
The 3-tier system worked well: Codex hit the no-hole invariant, the Claude helper did a rigorous
code-level analysis (publish gates on IsSurfaceKnown at main_launcher.cpp:16766; async marks ready only
after UpdateBrickWithExtractedFaces at SparseVoxelWorld.cpp:4751) and specified the safe design.
WATCHER VERIFICATION (FRAMETIME_LOG mtns_edit): no-hole HELD (visibleMissing=0 every frame), BUT the
dips REGRESSED: p50 10.4->12.66ms, p99 53.4->59.35ms, sub-60 dips 29.4%->38.6%. Codex recheck on the
same source-backed async patch after rebuilding also failed the clean baseline gate:
`p50=12.348ms p90=35.666ms p99=61.686ms max=103.114ms sub60=36.70%`, with visibleMissing=0 and no
`terrain-parity-fail`. The async coordination + deferred-publish overhead outweighed moving the work
off-thread. Reverted to HEAD (also clean-sliced the inert Step-4 GPU-midmesh cruft + checksum probe in
the same pass -> tree now clean at 55a19fc).

### KEY LESSON (two failed approaches now): do NOT RELOCATE the surface work, REDUCE it.
Loop 8 (cap+spread) and Loop 9 (async) both made sub-60% WORSE because they moved/smeared the same
~47-80-coord hidden-exact extraction volume rather than shrinking it. The next attempt MUST cut the
WORK VOLUME or per-coord cost, with evidence it reduces (not relocates). Open questions to diagnose
FIRST (no code): WHY do 47-80 hiddenCritical coords need a FORCED pre-publish flush in one frame? Is it
a BACKLOG (edits generate hidden-critical faster than the 4ms/frame budget clears -> periodic forced
flush spike)? Can the edit halo be smaller, the per-coord extract cheaper, or a coarse-now/refine-later
surface shown so the forced flush is unnecessary? Measure the backlog dynamics before attempting a fix.

## Loop 10 (diagnostic): hidden-exact pre-publish root cause, no code change
Measured from existing clean-head logs (`build/bin/loop8_head_ft.log`, plus noisier
`loop9_before_ft.log`) and source inspection only. No source change, no commit.

Finding 1: the 47-80 `hiddenCritical` coords are not a visible `hiddenCritical` drain backlog in the
pre-publish counter. They are a post-open hidden-exact critical set that becomes publish/surface
critical in bursts. In `loop8_head_ft`, frames 236-245 extracted only general coords (`hiddenCritical=0`,
`general=1-18`), then frame 248 jumped to `hiddenCritical=48`; frames 288-297 were only
`hiddenTracked=1` plus general work, then frame 298 jumped to `hiddenCritical=80`. The richer
`loop9_before_ft` log shows the same mechanism with explicit hidden-exact state growth: frame 248
`hiddenCritical=49`, `hemTracked=247`, `hemOut=48`, `stateRequested=49`; frames 249-255 then
`hiddenCritical=58/64/62/48/49/48/48`, while the broader surface queue backlog also grew
`qsurf=32 -> 118` and `qEdit=2 -> 60`. So there is a real surface queue backlog during edits, but the
specific p99 surface spike is the same-frame forced surfacing of newly critical hidden-exact coords,
not a hidden-critical queue slowly accumulating and periodically dumping.

Finding 2: the pre-publish flush is synchronous because page-table publish is gated on exact surface
coherence for non-speculative, non-empty pages. `main_launcher.cpp` pumps terrain-critical and
`sparseHiddenExactMissCriticalCoordsLastFrame` before publish, then publish checks
`publishNeedsExactSurface` and defers any non-speculative non-empty page whose surface cache is not
known before calling `MarkGpuPageTablePublished`. This preserves the invariant that an exact page made
public has a matching exact sparse surface, avoiding resident-missing-surface holes where the foreground
exact page suppresses lower fallback. It is not all coords: speculative pages are exempt, empty pages
are exempt, and source already separates terrain-critical, hidden-critical, hidden-tracked, and general.
The truly same-frame-critical subset is startup/terrain/public-critical repair; post-open hidden repair
coords do not need to be treated as all-or-nothing surface debt.

Finding 3: per-coord extraction already has partial-region support but frequently loses that benefit.
`QueueSurfaceDirtyRegionNoStats` queues only the edited brick plus six face-neighbor bricks when the
dirty region touches a boundary, not a 27-brick halo. `UpdateBrickRegion` expands the dirty region by
one voxel and extracts only that region, but it falls back to full-brick extraction if any existing
merged face with area >1 overlaps the expanded region. The brush path already suppresses surface
refresh when occupancy class does not change. This makes a pure per-coord micro-cut less promising
than cutting the number of coords classified as forced hidden-critical this frame.

Recommended Loop 11 fix: enable/use the existing post-open hidden-exact repair lane as the default
work-volume cut, without async and without spreading the same forced batch. Source evidence:
`enableSparseHiddenExactPostOpenRepairLane` is currently default-off; when off, post-open hidden-exact
requests are marked critical (`hiddenExactRequestIsCritical = !hiddenExactPostOpenRepairLaneCandidate`)
and the pre-publish path flushes `sparseHiddenExactMissCriticalCoordsLastFrame` wholesale. When on after
the public render gate has opened, repair candidates become non-critical repair-lane work capped by
`VENPOD_SPARSE_HIDDEN_EXACT_POST_OPEN_REPAIR_LANE_MAX_REQUESTS` (default 16), and both pre-publish and
publish switch to critical-only lists. On the measured spike frames this would remove the forced
post-open hidden-critical surface volume from the spike frame (`hiddenCritical=48-80 -> 0 forced
post-open repair coords`, with only any true critical startup/terrain coords still forced). Expected
FRAMETIME impact: cut the measured pre-publish surface slice by about 10-23ms on clean Loop 8 spike
frames 248-255/298-301 (`preMs=10.12-23.47`) and by 29-42ms in the noisier Loop 9-before run. The
gate for Loop 11 should be `VENPOD_FRAMETIME_LOG=1` on `mtns_edit.rec`, visibleMissing must remain 0,
and logs should prove `PERF_SPARSE_PRE_PUBLISH_SURFACE hiddenCritical` collapses on post-open frames
without increasing sub-60 dip percentage.

## Loop 11 (Codex + helper): hidden-exact foreground-deficit admission gate
Implemented the work-volume cut in `src/main_launcher.cpp`: post-open hidden-exact probe coords are
tracked but demoted out of same-frame critical catchup unless they map to a real foreground deficit.
Ownership `missPct>0` / `unsafeNearMissPct>0` still promotes conservatively; surface-fill contract
repair is coordinate-specific using the post-waterproof shader contract-nonready coord set, and helper
feedback added a pending-critical latch so foreground repair coords stay critical until
`ReadyToRender` / `ResidentEmpty`. Startup proof remains protected by the startup public-render gate.
Post-open generation/upload/surface/pre-publish/publish forced lists now use the critical set only, so
parent-held hidden-exact proof work stays normal tracked repair work.

Same-binary FRAMETIME_LOG gate on `mtns_edit` (`editbench.ps1`, frames >= 240): baseline with the new
gate disabled (`loop11_before_gateoff_rerun_ft.log`) was `raw p50=17.33ms p99=60.29ms sub60=53.94%`
(`body p50=17.28ms p99=59.22ms sub60=53.64%`). After coordinate gate
(`loop11_after_coordgate_ft.log`) was `raw p50=11.46ms p99=27.81ms sub60=12.42%`
(`body p50=11.44ms p99=27.79ms sub60=12.42%`). Work-volume proof after frame 240:
hidden-exact generated total `636 -> 272`, generation lines `40 -> 10`, critical feedback coord max
`436 -> 82`, and pre-publish hiddenCritical total `621 -> 272`.

No-hole proof held: `visibleMissing=0`, `PERF_RENDER_OWNERSHIP missMax=0`, and `unsafeNearMissMax=0`
in both before/after frametime logs and both capture logs. Visual steady-state proof used the same
moving edit sequence with gate off/on at frames 600/640/680/720; all BMP SHA-256 hashes matched exactly:
`78E49071F874F91C57A7702963326C939A6F1263E9518014C398F34C49317481`,
`37C9EDD93A3AC407AA206766105A984245EF375B1D2B49FB2FC15EF29AEA1655`,
`81C878418DCAF5065414C990E227922D7228F889102961AC1E892949BA8126F9`, and
`5D59DF4EA1BD9E707AB0FF057AE8866AE3D5D48A4D833EE44AC5DF952865314D`.

Validation notes: Release build passed with only pre-existing `rayDir` shadow warnings. `VENPODTests`
is not a clean gate in this tree; it still reports 28 existing sparse/terrain failures unrelated to
this one-file runtime gate. Commit: final hash reported after commit. Loop 12 pick: reduce the
remaining edit tail from dirty surface extraction itself, likely by preserving partial-region extraction
more often when merged faces overlap a small edit region.
