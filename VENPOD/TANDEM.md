# TANDEM ledger — VENPOD moving-ground clip spike

Driver: Claude (source/architecture). Partner: Codex (runtime/empirical).

## Goal
Cut the moving-on-ground frame-time spike. Hard gate: midCov 1.00 converged WHILE MOVING
(no under-streaming "watery holes"). C++-only (no shader).

## What "clip" is (established)
`perfSparseClipmapPrepMs` = whole mid-clipmap section. On a heading-45 walk, the worst
clip frames decompose as:
- `interest` (perfSparseClipmapInterestMs, UpdateVoxelInterest rebuild): ~9-13ms on cross frames
- `pump` (perfSparseClipmapPumpMs, 12534-12860): ~13-18ms on spike frames
- `other` (parentHeldFeedback raymarch + missingSampleFeedback): ~0.5ms typically

## KEY EMPIRICAL FINDING (refuted my source theory)
The original premise "clip = interest rebuild" and my follow-up "pump = brick generation,
parallelize it" are BOTH WRONG:
- The parallel VOXEL brick pump generates 192 bricks in **parWall=0.11ms** (negligible).
  Brick generation is NOT the cost. Enabling parallelVoxelPump did NOT help (ppActive but
  0.11ms; clip unchanged).
- On spike frame f188: pump=18.34ms = HEIGHT tile gen `mainThreadBrickGenMs=6.04` (6 tiles,
  GenerateTile does 33x33=1089 HeightAt calls/tile => ~1ms/tile) + voxel 0.11ms + **~12ms
  unattributed pump-section overhead** (the budget-compute block 12535-12810 + bookkeeping).
- interest=9.59ms is a genuine SECOND co-equal contributor (my budgeted per-ring interest
  rebuild fix — already implemented — addresses this half).

## Open question the partner is resolving NOW (job bjg836t0m)
Split the 18ms pump precisely: budget-compute block ms vs PumpGeneration-call ms vs
height-gen ms. If the ~12ms is pure budget-compute overhead, that's the cheapest SAFE win
(no coverage risk — it's not generation). The partner is adding PERF_SPARSE_PUMP_SPLIT
temp timers + benching.

## My implemented fix so far (SparseClipmap.cpp/.h, main_launcher.cpp env knob)
Frame-budgeted per-ring interest rebuild (voxelInterestRebuildRingsPerFrame, default 2):
on a footprint cross, rebuild only N rings, carry the rest from the prior interest set
(coverage-safe superset). Reduced interest spike but NOT the dominant pump spike.
midCov stayed 1.00 in all A/B runs.

## RESOLVED (partner's PERF_SPARSE_PUMP_SPLIT diagnostic, ground truth)
preludeMs=0.00 (budget-compute block is FREE), tailMs=0.00, **callMs=25.26** on worst frame
(f188). The ENTIRE pump spike is inside the PumpGeneration CALL. With voxelBudget=192 gen'd
in 0.11ms (parallel), the call cost is the HEIGHT TILE pump: heightBudget=16 tiles x
GenerateTile's 33x33=1089 HeightAt calls = ~17k HeightAt/frame, single-threaded.
=> The two co-equal clip spike sources: (1) interest rebuild ~10ms [fixed: per-ring budget],
(2) HEIGHT tile pump ~6-25ms [FIX: parallelize GenerateTile loop like the voxel pump].
Parallelizing the height pump is coverage-neutral by construction (same tiles, same budget,
fanned across workers). Do NOT touch HeightAt (guardrail) — only the caller loop.
Partner left a useful perfSparseClipmapBudgetMs + PERF_SPARSE_PUMP_SPLIT diagnostic in
main_launcher.cpp (kept).

## Decision (CONVERGED) — pending partner's pump split [historical]
- If budget-compute overhead dominates (~12ms): optimize/cache that block (cheap, safe).
- If height-gen dominates: parallelize the HEIGHT pump (coverage-neutral) — do NOT touch HeightAt.
- Keep the interest-budget fix for the interest half.

---
## Claude (Fable, main driver) cross-check — 2026-06-10 later session
My INDEPENDENT source read (before re-reading this ledger) ranked the cell-cross spike as the
INTEREST REBUILD (full re-expand of 5 ring anchor shells, ~74ms) and proposed budget/incremental
+ req-cache. THAT WAS THE BLIND SPOT: I anchored on the obvious suspect (interest) and assumed the
PUMP was cheap/GPU (carried over from the earlier far-SVO/gen work). Codex's runtime ground-truth
(PERF_SPARSE_PUMP_SPLIT: prelude=0, callMs=25.26; voxel gen 0.11ms parallel) proved the dominant
half is the single-threaded HEIGHT TILE pump (~17k HeightAt/frame). Interest is only co-equal ~10ms.
=> CONVERGENCE on the FIX once the empirical split was in hand: parallelize the height pump
(coverage-neutral; don't touch HeightAt) + per-ring interest budget. Both now in the working tree
(fix-clip agent). This is the textbook tandem outcome: empirical vantage corrected the source theory.

LIVE TRACKS (3 independent):
1. Workflow fix-clip (used Codex): parallelHeightPump + interest ring-budget — IMPLEMENTED, benching.
2. Fresh Codex re-derivation (job launched, reads HEAD, blind to the WIP): independent re-check +
   REQ-side levers (pressureTrim / PlanHierarchical per-coord dispatch) for the upcoming fix-req.
3. Me: judge coverage (midCov 1.00) + holes sheets myself before commit; reconcile all three.

OPEN: does the height-pump parallelization hold midCov 1.00 while moving? (same tiles/budget fanned
to workers => should be coverage-neutral by construction, but VERIFY on the walk, not by argument.)
REQ side (7.4ms steady) is still unaddressed by fix-clip — that's fix-req + Codex's input.

---
## RESOLVED cross-check (3 independent tracks, 2026-06-10)
DIVERGENCE found + resolved by ground-truth:
- Fresh Codex (READ-ONLY, "did not build/run") ranked clip lever #1 = incrementalize the INTEREST
  rebuild. Claude source analysis: SAME. Both MISSED the height pump.
- Earlier Codex (RAN it, added PERF_SPARSE_PUMP_SPLIT, callMs=25.26): HEIGHT TILE pump is the
  dominant/co-dominant half — invisible to code-reading.
- RESOLUTION: measurement wins. Two source-only analyses both missed it; only the empirical pass
  caught it. Workflow correctly parallelizes the height pump BECAUSE of the measurement.
CONVERGENCE (all three agree):
- Clip = parallelize height pump + budget interest rebuild over frames (carry old set as SUPERSET).
- Req = dedupe per-coord requestSparseBrick dispatch + cache PlanHierarchical across unchanged
  signatures + gate/budget pressureTrim. (Codex levers 4-6; align with fix-req attribution.)
- SAFETY (Codex's "what I would NOT do", caught a flaw in Claude's hypothesis #2): do NOT do naive
  edge-only toroidal as source of truth — interior coords change priority after a cross; do NOT cap
  critical interest/request admission (silent under-stream => holes). Synchronously admit leading
  edge + near-camera coords every frame regardless of budget.
ACTION: ensure fix-req implements dedupe + PlanHierarchical signature-cache (Codex 4-5). Verify
height-pump parallelization holds midCov 1.00 on the actual walk (empirically, not by argument).

---
## fix-clip VERIFIED (2026-06-10, gates run on the actual walk)
OPEN ITEM CLOSED: height-pump parallelization holds midCov 1.00 while moving — confirmed
empirically, not by argument.
Same-session A/B (median of 3 reps, NORMAL logging, vsync off, speed 45) full-fix OFF vs ON:
- heading 45 : med 22.49->21.71, p95 42.56->36.42 (-14%), worst 69.89->51.64 (-26%)
- heading 200: med 24.11->19.62 (-19%), p95 44.48->31.95 (-28%), worst 83.02->48.11 (-42%)
- standing (speed 0): med 14.69->13.49, worst 49.12->38.08 (NOT regressed)
- stress-fly (yaw90 speed250 pitch-55, 8 workers): badMarkers=0, clean exit, midCov 1.00
GATES: (a) midCov 1.00 converged both headings + stress (dips are startup-only frames 0-1);
(b) 6 walk capture frames (h45 + h200) read as images — NO watery holes / NO new artifacts;
(c) standing fps not regressed; (d) 0 of 0x7FF8FFF2|Failed to create graphics|Map failed|
DEVICE_HUNG across ~16 runs + stress-fly. Worst-case cell-cross hitch cut 26-42%.
FILES: SparseClipmap.cpp/.h (parallel height pump + per-ring interest budget),
main_launcher.cpp (env knobs, default ON). Partner's PUMP_SPLIT diag reverted by track.
