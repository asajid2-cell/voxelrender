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
