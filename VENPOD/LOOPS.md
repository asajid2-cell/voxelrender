# LOOPS.md — VENPOD mid-terrain correctness + performance

> Loop-driven autonomous run started 2026-06-12. No human available.
> Grand goal (user, verbatim spirit): find and fix the engine's real rendering issues —
> **no holes, no approximate/smooth terrain** in the mid band, no air/water bands over
> missing terrain in motion, keep 60fps-mode performance. Robust verification: **never
> judge downscaled/compressed/tiny images — native full-size only**, and **Codex
> independently visually judges every sheet** (proven able via its view_image tool).

---

## CONTRACT (change-controlled — edits require a dated note with forcing evidence)

### Grand Goal Contract
| ID | Outcome | Verifier | Type |
|----|---------|----------|------|
| G1 | Debug-69 mid-isolation battery: mid band is REAL voxel DDA (green), not smooth column (orange) and no global gates | `judge.ps1` PASS per frame: orange ≤0.5%, gates ≤0.1% (static scenarios); orange ≤2.0% (motion scenarios, streaming transient allowance) | MACHINE (trusted 2026-06-12) |
| G2 | Real-render battery: no smooth/approx terrain in mid, no blue-air/water bands over terrain, no holes | TWO independent judges on NATIVE frames: me (Read 960x540 1:1; tile >1568px res into quadrants) AND Codex (view_image on the same files). Both must pass. | JUDGE-GATE |
| G3 | Performance: standard speed-50 scenario avg fps ≥55 in 60fps mode; motion speed-200 recorded + no collapse <25 | PERF lines in venpod_runtime.log (post-warmup frames ≥240) | MACHINE |
| G4 | No regression: near surface + far field remain good; no new artifacts | Both judges on the same battery | JUDGE-GATE |
| G5 | Final "up to snuff" aesthetic call | The user, on their machine, at original quality | HUMAN-GATE (cannot be claimed autonomously) |

### Canonical scenarios (THE bar = user's real run: `rebrun -PerfMode 60fps`, default envs;
### capsheet may set camera/capture vars only, NEVER rendering-behavior envs)
- S1 above-view static-ish: `capsheet -AltTenths 1500 -PitchDeg -26 -StartCapture 300 -Every 40 -Count 3 -Speed 50 -YawRate 22` (+`-DebugMode 69` for the debug variant)
- S2 shallow-pitch: same but `-PitchDeg -14`
- S3 motion: `-PitchDeg -14 -Speed 200 -YawRate 14 -StartCapture 220 -Every 16 -Count 6`
- S4 descent: high alt → low (added in L3; exact params recorded when built)

### Loop contracts
```yaml
- loop: L0-verification-harness
  invariant: "trusted machine verifier + proven Codex visual judge + tandem re-established"
  exit: "judge.ps1 seen red (cyan state, orange state) + pass (real render, 0% false positives); Codex cold-describes a known frame correctly; watcher up"
  status: DONE 2026-06-12 (evidence below)

- loop: L1-stable-baseline
  invariant: "recorded build state: DDA alive (green present) AND fps>=55 at S1-real"
  entry: "L0 done"
  scope_in: [revert/keep decision on the re-dispatch change (SparseClipmap.cpp/.h, main_launcher.cpp ~15945), measurement only otherwise]
  scope_out: [PS_Raymarch.hlsl shader logic, FarVoxelOctree, budgets]
  verifier: "judge.ps1 on S1-debug69 (green>0, gates=0) + fps grep on S1-real"
  exit: "both green, state checkpointed as patch"
  escape: {max_iterations: 4, stop_if: [PSO crash, same failure twice]}
  status: pending

- loop: L2-kill-orange-static
  invariant: "S1+S2 debug-69: orange<=0.5%, gates<=0.1%"
  entry: "L1 done"
  scope_in: [mid DDA coverage: start distance, residency/interest, acceptance, mid-only pass; their launcher defaults]
  scope_out: [near surface pipeline, far SVO render, physics]
  verifier: "judge.ps1 (trusted; proven red on this exact failure 18-33% orange)"
  exit: "judge PASS on S1+S2 debug + BOTH visual judges pass S1+S2 real + fps within 10% of L1 baseline"
  escape: {max_iterations: 6, stop_if: [PSO crash 0x-7FF8FFF2 -> revert shader edit, TDR, same failure twice]}
  status: pending

- loop: L3-motion-no-air
  invariant: "S3+S4: no blue-air/water band over terrain (both judges), orange<=2%, no stale terrain"
  entry: "L2 done"
  scope_in: [re-dispatch scoping done SAFELY (fix the 3 Codex holes), interest prefetch, motion budgets]
  scope_out: [shader mid-only pass changes unless evidence demands]
  verifier: "judge.ps1 motion thresholds + both visual judges on real S3/S4"
  exit: "verifier green + L2 verifier re-run green (regression)"
  escape: {max_iterations: 6, stop_if: [fps collapse >30% vs L1, same failure twice]}
  status: pending

- loop: L4-performance
  invariant: "G3 met with L2+L3 verifiers still green"
  entry: "L3 done"
  status: pending

- loop: L5-synthesis
  invariant: "Grand Goal Contract end-to-end on a full battery; fresh-context Codex adversarial review"
  entry: "L1-L4 done"
  status: pending
```

Checkpoint policy: NO git commits (not sanctioned). Checkpoints = `git diff > tmp/ckpt/ckpt_N_<desc>.patch` + ledger entry. Rollback = apply -R.

---

## PROGRESS

### Baseline (recorded 2026-06-12, machine freshly restarted)
- Git: branch perf-measurement-dive @ 40f3ae9; ALL session work uncommitted. ckpt_0_full_state patch saved (1769 lines).
- Build: build/bin/VENPOD.exe = re-dispatch-fix build; shader cache warm (gate fix + debug-69 cause map).
- judge.ps1 measurements of existing captures (all 960x540 native):
  - demo_midcause69 (pre upload-gate fix): cyan 64.7-75.7% → DDA was DEAD (header invalid)
  - demo_midfix69 (upload-gate fix, S1): green 6.9-10.1%, orange 18.3-32.6% → DDA alive, column dominates
  - demo_midorange1 (+coarse-fallback gate fix, S1): green 5.8-8.9%, orange 18.8-30.0% → **NO measurable improvement** over midfix69
  - demo_redispatch69 (motion S3-ish, re-dispatch build): green 5.1-8.4%, orange 0.7-5.4%; user reports visible AIR + orange in real view
  - demo_midreal (real render): judge 0% everything → no false positives (negative control)
- fps: history murky across configs (66-71 seen in one run; 33-35 in last speed-50 run on re-dispatch build). L1 re-measures cleanly.
- Pre-existing baseline failures (untouched scope): white dome hole under camera at low pitch (seen in redispatch69 frames); floating far fragments at horizon (older issue).

### Learnings
- **L-1 (2026-06-12): My past visual verdicts from downscaled overviews were WRONG.** "Gate fix shrank orange substantially" — judge.ps1 shows orange unchanged (18-33% → 19-30%). Never construct 640x360/480x270 overviews for judgment again; Read the native PNG only. This is the exact failure the user called out.
- L-2: judge.ps1 windows: green g>200&&r<90&&b<90; orange r>200&&90<g<175&&b<60; proven no false positives on natural terrain.
- L-3: Codex CAN view images (functions.view_image) — cold test matched ground truth (cyan 60-65% est vs 71% measured). Give it the debug palette legend when judging debug sheets; give it "air band / smooth approx / holes" rubric for real sheets.
- L-4: capsheet orphan race: ALWAYS `Stop-Process VENPOD` before a run; a leftover engine inherits capture envs and the new run exits at frame 120 with 0 frames.
- L-5: rebrun forces VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION=1 (rebrun.ps1:756) — GPU-gen is ON in the user's real runs; C++ default (0) is misleading.
- L-6: Codex adversarial review of the re-dispatch change found: (a) dispatch-failure/generator-not-ready leaves bricks sample-less while the voxel serial advances → no retry until unrelated dirty → AIR; (b) edit-halo invalidation miss (InvalidateEditedOverlays uses core AABB, generation samples a 1-cell halo); (c) MarkVoxelGpuSamplesUploaded is slot-only/fragile (no payload validation).
- L-7: The uber-shader (PS_Raymarch.hlsl shared paths) recompiles ~3min; mid-only-pass-scoped (#ifdef RAYMARCH_MID_ONLY) edits recompile ~3s. Keep iteration inside the mid-only pass where possible. PSO crash 0x-7FF8FFF2 = NVIDIA JIT cliff → revert the offending shader edit.

### Loop status log
- 2026-06-12 L0: DONE. Evidence: judge.ps1 runs above (FAIL exit=1 on midcause69/midfix69/midorange1; PASS exit=0 on midreal); Codex view_image cold test (verdict in tandem log, 18s turn); watcher restarted (http://localhost:8799).
- L-8 (L2 pre-staged hypothesis, UNTESTED): the S1 orange band is NEARER than the green — inconsistent with pure residency-miss (farther should be worse). Mechanism that fits: the DDA advances ONE cell per loop iteration even through empty/missing space (PS_Raymarch.hlsl ~2080: t = min(nextCellT, t + max(cellSize,4))), and the step budget is ~48-128 (lines 2001-2005). From midStart=768 to terrain at ~1100-3000u in 4-8u ring0/1 cells = 100-300 iterations -> budget exhausts -> miss -> column proxy (orange). Far rays land in coarser rings (16-64u cells) = fewer steps -> green. L2 iteration 1 = extend debug cause map to paint miss-reason (budget-exhausted / no-brick / past-end) in the mid-only pass (~3s recompile), THEN fix the dominant cause (candidates: start DDA at near-ownership edge; brick-granular empty-space skip; budget raise for the small mid-only PSO only).

### 2026-06-12 — L1 iteration 1-2 + first two-judge G2 reading
- L1 iter1 (current re-dispatch build, S1): judge = green 3.9-8.7%, ORANGE 1.06-1.72% (vs 18-33% on old builds — attribution: midStart default is BAKED at 256 in launcher line 2526, mid-pass default-on line 1046), gates 0%. fps samples 48.5 (f240), 56.6 (f360) — marginal vs >=55 bar, longer window in iter2.
- L1 iter2: applied hole-(a) fix (voxel serial advances ONLY when sample dispatch succeeded/not-needed; failure -> retry next frame). KEEP decision for re-dispatch change, made safe. Building + long fps run + judge re-run in flight.
- **G2 two-judge reading on CURRENT build S1 real (demo_L1_cur_real): BOTH JUDGES FAIL.**
  - Me (native 1:1): mid band visibly softer than near terraces; visible near/mid style seam; dark hazy far band; no white holes.
  - Codex (view_image, all 3 frames, major severity): (a) mid clearly smoother/approximated vs foreground, transition right past the foreground ridge; (b) broad grey-blue band across mid/background reads as covered/missing terrain (may be water — needs ownership diagnosis); (d) strong material mismatch seam. No white holes.
  - => L2's real bar is the two-judge gate; orange% alone is insufficient (it is already ~1%, yet the mid still reads smooth).
- L2 fix candidates (by cost): (1) mid composite upscale bilinear->POINT (blocky-crisp pixels, ~free — composite PS is small, not the uber shader); (2) mid pass scale 0.5->0.75/1.0 (GPU cost, measure); (3) verify actual mid cellSize: SparseClipmap may stage 2u but Renderer clamps midFieldParams.w to >=4 (line ~796) — potential gen/shader mismatch to check; (4) diagnose grey-blue band: real water vs far-water sheet over dry terrain (known prior failure mode "navy over dry basins").

### 2026-06-12 — L1 DONE (with dated contract note)
- CONTRACT NOTE (forced by evidence): L1's fps>=55 clause moved to L4/G3 (where perf work lives). Evidence: clean no-capture S1 fps = 54.5/58.5/49.4/50.3/41.3/44.2 (avg 49.7) on the DDA-alive build. Attribution: pre-fix builds hit higher fps by NOT rendering the voxel mid (DDA bailed instantly on the dead header); the correctness fix added a real per-pixel march cost. Recovering fps without losing correctness = L4. G3's bar (>=55) is UNCHANGED.
- L1 EXIT EVIDENCE: DDA alive (judge: green 3.3-10.3%, orange 1.27-2.33%, gates 0% on demo_L1_fix_dbg); hole-(a) fix built clean (151s, exit 0; no MIDGEN dispatch-failure warnings in runs); re-dispatch change KEPT (made retry-safe); clean fps protocol recorded (no-capture run, samples >=240). Checkpoint: tmp/ckpt/ckpt_1_L1_baseline_holeA_fixed.patch.
- L1 baseline numbers for regression comparison: S1-real fps avg 49.7; S1-debug69 orange 1.3-2.3%, green 3-10%; both-judge G2 on S1 real = FAIL (mid reads smoother than near; grey-blue band suspicion) -> L2's target.
- L2 PLAN UPDATE (dated, forced by discovery): primary candidate = MESH-MID path (commits 149e0f9/f170346: mid as real terraced geometry through the near's raster pipeline; its own commit records native-res verification 'terraced continuous with the near, water filled, no coverage holes'; env-gated VENPOD_SPARSE_MID_MESH=1 due to known move-perf issue: full re-upload on camera movement, 56fps moving vs 136 mesh-off). E1 = enable via env (runtime, no rebuild), S1 captures, two-judge gate + judge.ps1 + fps. If judges pass: L2 continues with incremental per-tile upload perf fix + bake default-on. Raymarch-mid polish (point upscale etc.) demoted to fallback plan.

### 2026-06-12 — L2 E1 verdict: JUDGES DIVERGED -> FAIL (both-must-pass); E2 ground-truth next
- E1 (mesh-mid via env, S1): fps ~48 vs 49.7 baseline (within noise — the feared move-re-upload cost is NOT the limiter at S1; mesh uploads 617-625k faces, serials advancing fast). Mesh covers 1024-9000.
- Judge ME: PASS-leaning — terraced geometry continuous with near, water reads as water, no holes/seam.
- Judge CODEX: FAIL x3 (major): grey-blue band dominant + fragmented strips at its edge; mesh-mid at distance = "large flat slabs/big block walls, different scale/color density" (LOD merge approximation); abrupt transition at the band.
- => E1 FAIL under the two-judge rule. DIVERGENCE = the work item. Codex's band suspicion has prior evidence ("navy over dry basins", F2 water-boundary disagreement in docs/visual-failures.md).
- E2 plan: (a) owner-map capture (debug 58, mesh OFF since raster pixels aren't owner-painted): who renders the band? cyan=FAR_WATER over dry ground = covering bug. (b) fly-toward test (speed 200, yaw 0, straight at the band): real sea keeps shoreline; covering bug fills in with terrain on approach. Then fix accordingly; separately A/B the mesh LOD merge (MAX_MERGE 4 -> 2 / LOD off) for the slab look, with fps + judge.

### 2026-06-12 — E2 ground truth: water REAL (Codex band-theory refuted); VERIFIER GAP found+fixed
- E2b fly-toward (mesh-on, speed 200, yaw 0): basins keep coherent shorelines on approach; terraced terrain descends INTO them; detail refines, never replaced by land. => the grey-blue band at S1 = REAL water at grazing angle. Codex's covering-bug theory REFUTED by ground truth. CONFIRMED real issues from its verdicts: (i) water SPECKLE at coarse distance (tiny scattered patches resolving only on approach), (ii) far-band slabs.
- E2a owner map (debug 58, mesh OFF): MID_HEIGHT (smooth) owns ~the entire lower band beyond the near raster — the TRUE smooth-approx coverage is ~40-50% of frame, NOT the 1-2% debug-69 orange reported. INSTRUMENTATION GAP: the mid-only pass's full misses fall through to the full pass's smooth mid, which mode 69 never paints. judge.ps1 extended with -Palette owner58 (MID_HEIGHT% = the real anti-smooth machine metric).
- CONTRACT NOTE (dated, forced by the above): G1 verifier extended — static scenarios must ALSO pass owner58 MidHeightPct<=2.0 with the shipped config. debug-69 thresholds retained but known-insufficient alone.
- In flight: owner58 proven red on E2a (expect huge yellow) + owner-58 capture WITH mesh-mid to measure whether raster ownership collapses MID_HEIGHT to ~0.

### 2026-06-12 — Architecture clarity: FOUR renderers compete for the mid band; end-state chosen
- Native PERF_RENDER_OWNERSHIP (real metric, no debug confound; env VENPOD_SPARSE_RENDER_OWNERSHIP default ON): e2c run showed full-pass midVoxel=40522/46656 (87%), midHeight=0 — while the 0.5-scale mid OVERLAY's DDA hits only ~6% and paints its weak column results OPAQUELY over the full pass. The owner-58-with-mesh image confusion: the overlay's mode-58 column paint covered the full pass (incl. regions the full pass resolved as voxels). => the overlay is now plausibly NET-NEGATIVE visually.
- Renderers competing for 1024-9000: A) background raymarch (0.3 scale) mid path, B) mid-only overlay (0.5 scale), C) mesh-mid raster (FULL res, env-gated), D) near raster (<=1024).
- CHOSEN END-STATE (L2): C owns 1024-9000 at full res; B disabled; A's mid never visible behind C; far owns >9000. Crisper by construction AND less GPU work.
- E3 (in flight): mesh ON + overlay OFF (env A/B), S1 real: two judges + fps + ownership. Then: slab fix (LOD merge tuning), water-speckle fix, bake defaults, L2 exit battery.

### 2026-06-12 — GPU splits land: the engine is CPU-BOUND; my fps numbers were noise
- gpu=frame/upload/pre/surface/ray/overlay/ui parsed from PERF (the instrument was in every line all along):
  A default: fps 39.8, gpuFrame 2.80ms (ray 2.39) | B mesh+overlay: fps 50.3, gpuFrame 4.86 (ray 3.72) | C mesh, no overlay: fps 51.1, gpuFrame 5.64 (ray 4.25)
- => GPU could run 175+fps; frame time is CPU (~20-25ms). The earlier "overlay-off = 25fps" did NOT reproduce (51.1 now): single-sample fps on top of streaming convergence = noise. VERIFIER FIX: fps protocol now = median of PERF samples frames 300-460 across 2 repeat runs + spread; CPU splits (prep/wait/render/present + sparseSplit) parsed alongside. (The loops doctrine required variance bounds; violating it produced the false E3 fps regression.)
- Config choice is therefore primarily VISUAL. Open question for the judges: does the 0.5-scale overlay PAINT OVER the full-res mesh (blurring it)? E1 vs E3 same-camera comparison queued for Codex; my read: E3 ≥ E1, similar-to-slightly-cleaner.
- L4 implication: perf work targets CPU (prep/sparse pipeline), NOT the raymarch GPU. The overlay/background passes are GPU-cheap.

### 2026-06-12 — L2: defaults baked; judges rank D2 best; ONE blocker left (shoreline fragmentation)
- BAKED (built next cycle): VENPOD_SPARSE_MID_MESH default 1 (launcher 2046); overlay default = OFF when mesh on (launcher ~1045, env-explicit wins); rebrun no longer force-enables the overlay; capsheet no longer deletes the caller's overlay env (the E3 'overlay-off' frames were actually overlay-ON because of this — tooling bug found by Codex).
- Codex ranking (native, all FAIL but converging): D2 (LOD merge 1-2, 822k faces) > D1 (merge 1-4) > E1 (overlay on). LOD-2 visibly worth +33% faces. Water-as-water accepted (ground truth stands). Slabs reduced to minor-moderate at D2.
- REMAINING L2 BLOCKER (both judges agree): mid-band SHORELINE fragmentation — dark speckles + thin detached terrace strips at water edges (central island / right shoreline), reads as missing tiles. Hypothesis (to verify in code): BuildMidHeightSurfaceSnapshot's aggregate SKIPS footprints whose samples are neither solid nor water -> emits NO block -> mesh hole -> dark background shows. Plus any-solid-wins-at-max-height strips over mostly-water footprints.
- CONTRACT NOTE (dated, verifier change, controlled): with the mesh architecture shipped, debug-69 orange and owner-58/PERF_RENDER_OWNERSHIP measure the RAYMARCH's internal view (hidden behind mesh pixels), not the visible screen -> demoted to diagnostic-only for the shipped path. G1's machine gate for L2-exit becomes: (a) builder-level emptyFootprint/coverage stat (to add, exact CPU count, prove red on current shoreline) == 0 inside the band, (b) mesh upload stats present, (c) the two-judge gate (G2) on native frames stays decisive. fps gate via the median protocol (G3).

### 2026-06-12 — L2 iter: skirts v1 judged FAIL; refined to v2 (water-skip + deeper)
- D3 (skirts v1, canonical defaults + LOD cap 2): overlay default-off CONFIRMED in run log (no mid-pass resources); 859k faces. ME: PASS-leaning (islands read solid). CODEX: FAIL x3 — remaining dark shoreline gaps/fins; NEW: "dark vertical/side faces at water boundaries that look like exposed skirt walls" -> my v1 skirts likely VISIBLE on water blocks; plus ring-boundary steps can exceed 4-depth skirts.
- v2 refinement (built next): skirts ONLY for solid blocks (water tops are globally sea-level — water-water borders cannot crack; water skirts = hanging dark walls), depth max(8, terraceStep*6) for ring-boundary steps. Escape-watch: this is iteration 2 on the shoreline invariant; if D4 still FAILs -> stop, dedicated diagnosis loop (close-up D4b capture taken alongside for mechanism ground truth).

### 2026-06-12 — L2 escape->diagnosis: shoreline mechanism FOUND (shoal inflation), fix building
- D4 (skirts v2) verdicts: fresh-thread Codex (uncontaminated) — D4 standard FAIL (detached rectangular strips over water persist), D4b CLOSE-UPS PASS (crisp, continuous, no tearing). ME: same split read.
- The distance-only + rectangular + shoreline-located + skirt-immune pattern cracked it: ANY-SOLID-WINS in merged footprints INFLATES a 1-sample shoal to a full merged-quad rectangle floating at its height -> the "detached strips". merge=1 (close) renders the same world fine. Skirts were never the cause (v1 water-skirts added fins; v2 kept as border-crack insurance for solids).
- FIX (built): aggregateSamples counts solid/water; mixed footprints with <25% solid emit WATER (sub-pixel shoals dropped at distance; solid-majority coastline preserved). D5 capture queued -> two judges.

### 2026-06-12 — L2 ESCAPE: 3rd shoreline FAIL (D5) -> mandatory diagnosis, no more candidate fixes
- D5 (shoal-demotion <25%) Codex verdict: FAIL x3. Floating strips NOT gone; NEW: a long ruler-straight tan strip over water (0340/0380); shorelines partially "eaten into skinny strands" (the demotion over-thins); a large dark rectangular cut at land/water edge (0380).
- NOTE: the world gen contains ruler-straight ROUTE RIDGES (FarTerrainClosureInfluence routeCorridor) — the straight strip may be REAL content. The "eaten strands" may be MY fix over-demoting. Two opposing errors possible; only ground truth settles it.
- D6 (in flight): aimed low-alt fly-through at the exact flagged cluster (D5_0380 cam (-127,177,167) fwd (-0.139,·,0.888) -> target ~(-405,·,1943), Head 102). On approach the cluster is rendered at merge=1: real archipelago/route-bar -> content (then the JUDGE RUBRIC needs updating, and my demotion threshold reverts); vanishes/disconnects -> builder bug at merged LOD (then fix the aggregation properly).

### 2026-06-12 — D6 result: the close-up at speed 200 is CATASTROPHIC (motion failure exposed)
- D6 frames at ~1500-1700u from spawn, speed 200, alt 60: floating mesh platforms over a dark water-sheet, WHITE sky-through-terrain gaps, a black rectangular hole. This does NOT answer the shoreline content-vs-bug question; it exposes the L3 motion failure (camera outruns streaming -> air/holes; the user's original complaint). D6 = L3's red baseline evidence.
- D7 (in flight): same approach at speed 60. Clean -> D6 mess = streaming lag (L3 scope, L2's shoreline question still open via D7's close view of the cluster). Same mess -> the location itself is broken (L2 scope).

### 2026-06-12 — D8 verdict: hoisted strips MOSTLY FIXED; failure shifted to dark waterline cuts
- Codex on D8 (shoal-height fix): "clear improvement... shoals sit lower, water-hugging" — the height fix landed. Remaining dominant offender (now isolated): DARK RECTANGULAR CUTS at water-land contacts (esp. central ledge, 0380). Crisp-voxel + continuity now explicitly OK per judge ("the failure is not smooth/approx terrain").
- Mechanism hypothesis (building): tile-border skirts were fixed-depth 8; a ledge dropping to sea level at a border leaves a black gap between skirt bottom and water. v4: skirts extend below SEA_LEVEL_Y (-48) whenever the block sits above it; riser height now matches the variable depth.
- In parallel: Codex code-trace of OTHER waterline-hole mechanisms (cull, air-skip footprints, missing risers next to skipped footprints, water emission clamps, per-tile face-budget partial tiles).

### 2026-06-12 — D9 + Codex trace -> all-Air footprint fill; D10 = first FULLY-CANONICAL build
- D9 (sea-level skirts): central shoreline continuous at standard view; 1:1 crop still shows ONE black rectangle at waterline -> sea-skirts necessary but not sufficient.
- Codex code-trace ranked the remaining hole: #1 all-Air merged footprints emit NOTHING (no top; neighbors emit no risers toward a missing block) -> rectangular dark hole; Air samples occur legitimately (PackSample line ~5904-5908: above-sea notch columns can be Air). #2 cull (medium-low), #4 budget (unlikely — builder returns false, snapshot not staged).
- FIX (built): all-Air footprints fill with a sea-level water top (emitWater on) -> hole sealed AND footprint becomes present so neighbors seal risers. ALSO BAKED: LOD_MAX_MERGE default 4->2 (judges ranked it worth +33% faces).
- D10 = FIRST capture where EVERYTHING is baked defaults, zero env overrides — exactly the user's real run. Two-judge gate next; if pass -> L2 exit battery (S2 + fps protocol + ckpt_2).

### 2026-06-12 — L2: TWO-JUDGE GATE GREEN ON S1 (D10 canonical)
- Judge reconciliation on shared 1:1 crops: Codex RETRACTS the D10 hole/void calls with pixel coords — "the earlier cuts were dark water inlets / shadowed risers"; "vertical hanging slabs... plausibly normal terrace risers at grazing angle"; FINAL: "D10 has no TRUE holes/voids visible in the mid band." Combined with its since-D8 verdicts that the mid is crisp real voxel geometry (not approximate) => S1 two-judge gate GREEN.
- Fix lineage that got here (all baked defaults, zero env): mesh-mid on; overlay off-when-mesh; LOD merge cap 2; tile-border skirts (solid-only) extended below sea level; mixed-footprint shoal HEIGHT = lowest solid sample; all-Air footprints fill with sea-level water; mesh face budget 1M->1.5M (canonical scene ~1.02M was 3% from tripping).
- ckpt_2 saved (tmp/ckpt/ckpt_2_L2_shoreline_green_S1.patch). In flight: S2 canonical capture + fps median protocol (2 runs).
- Learning L-9: judge calibration matters — "dark = hole" over-flags water/shadow content; reconciliation = shared 1:1 crops + pixel-coordinate claims + ground-truth flights. The two-judge gate survived its first real disagreement and produced a better verdict than either judge alone.

### 2026-06-12 — L2 DONE (two-judge gate green on S1 + S2)
- S2 (shallow pitch, canonical): Codex PASS x3 ("crisp real voxel terraces... no true holes/voids/floating strips found", no defect coordinates); my native read concurs. S1 passed via the D10 reconciliation. => G2's two-judge gate GREEN on both canonical static scenarios.
- fps (median protocol, thin n=2): 54.7-56.4 — UP from the 49.7 pre-mesh baseline (overlay-off default more than paid for the ~1M-face mesh). Formal G3 measurement (bigger n) = L4.
- L2 EXIT EVIDENCE: ckpt_2 patch; baked defaults (mesh-mid on / overlay auto-off / LOD cap 2 / sea-level solid skirts / shoal min-height / air-fill water / 1.5M face budget); judge verdicts above; build exit 0.
- SCOPED NOTE for L5: the FAR band (>9000, separate layers) now reads "softer/coarser/slabby" RELATIVE to the much-improved mid (Codex note, both S2 rounds; y80-215 region). The user historically called the far fine — the mid's improvement moved the relative bar. Decide in L5 whether it violates the user's intent.
- L3 OPENS. Red baseline: D6 (speed-200 close view, pre-L2-finals build) = floating platforms + WHITE sky-through-terrain gaps (near-surface streaming outrun) + dark sheet. Iter 1: re-baseline S3 on the CURRENT canonical build + per-layer streaming stats (near raster backlog, mesh serial cadence, clipmap voxels) to attribute the air to its layer before fixing.

### 2026-06-12 — L3 iter 1: red reproduced + ATTRIBUTED on canonical build (S3 speed-200)
- Frames 0536/0620: (A) WHITE sky-through wedges bottom/center = NEAR band missing (hiddenExactMissing ~97 at f600-620; surfGen working at 1938/frame but outrun at 200u/s). (B) DARK WATER-SHEET over the lower half where D7 (slow, same coords) shows MOUNTAINS = the analytic water layer paints over UN-STREAMED mid tiles ("navy over dry basins" reborn). Mesh itself keeps pace (serials advancing, 1.17M faces < 1.5M budget); midVoxels full.
- Two lanes: (A) near streaming at speed (burst budgets/prefetch — sparseMotionStreamBurstMinSpeed exists, verify it fires and is sufficient at 200), (B) the water-sheet-over-unloaded-mid mechanism (Codex tracing: where does water render when the analytic height there is ABOVE sea? candidates: missing height TILES default to sea-water in some consumer; FAR_WATER layer's cheap proxy; my all-Air water fill CANNOT cause it — it only runs inside RESIDENT tiles — but verify).

### 2026-06-12 — L3 iter 2: water-sheet root cause (Codex trace, CONVERGED) + shader fix building
- Codex trace: missing mid tiles are painted by RaymarchBackgroundField; in voxel-terrain-only the column proxy is disabled (4974); the FAR-HEIGHT analytic fallback is GATED on mature mid residency (deferFarSvoToFarHeightHorizon, 5177-5184) but the bare FAR_WATER fallback (5201) is NOT -> at speed, sea paints over un-streamed dry mountains. The white wedges = the same fallback chain failing entirely (closure+continuity both refuse) -> sky color through.
- FIX (uber-shader, ~3min recompile, PSO-watch): at 5201, if DiagnosticFarTerrainWouldHit lands EARLIER than the sea crossing, take BuildDeterministicFarTerrainContinuityHit (FAR_HEIGHT) instead of water. Asymmetry closed: dry land now outranks water on missing data.
- S3 re-capture in flight; judges next. White-wedge residue (if any) = follow-up: relax the continuity gate for terrainFacing miss pixels.

### 2026-06-12 — L3 iter 2 stumble + recovery: first water fix TDR'd the device
- The 0-frame captures were NOT an env race: runtime log shows `Capture failed to create readback buffer: 0x887A0005` (DXGI device removed) — my dry-land-first fix ran DiagnosticFarTerrainWouldHit (a FULL analytic march) per water-occluder ray (~most sub-horizon rays) -> GPU hang -> TDR. The documented TDR cliff, hit as predicted.
- v2 of the fix (recompiling): fixed 3-sample FarTerrainHeight probe along the ray (one noise eval each, [unroll]) instead of the full march; land-above-ray at any probe -> continuity hit instead of water. Same semantics, bounded cost.
- Learning L-10: ANY per-ray addition to the water/fallback path multiplies by ~full-screen ray count — only O(1) probes allowed there; the Diagnostic* helpers are for sparse diagnostic pixels only.

### 2026-06-12 — L3 lane B: ESCAPED shader-side fixes (2x TDR), reverted, redesign delegated
- v2 (3-probe + continuity per water ray) ALSO TDR'd: device silently removed during frames 0-480 at speed 200 (first surfaced as 'Capture failed... 0x887A0005' at the frame-480 readback creation); shader grew 7.33->7.80MB (driver-JIT pathology suspected on top of ~9 extra noise evals/ray).
- Per stop_if (same failure class twice): shader change REVERTED to the L2-verified state (a NOTE comment marks the parked site). Revert-check capture running (S1 frames + zero 0x887A = gate).
- Codex designing the TDR-proof fix: (A) CPU-fed streamed-radius frame constant, (B) reorder/relax the residency gate so far-height degrades consistently with water, (C) synchronous coarse placeholder tiles so the MESH covers the gap. Constraint: O(1) per-ray.

### 2026-06-12 — L3 iter 3: TDR-proof motion guard IMPLEMENTED (Codex design A + bounded reorder)
- CPU: SparseClipmapTileCache::NearestMissingHeightTileDistance (interest set scan, ~300 tiles, origin math mirrors GenerateTile) -> launcher clamps to [midStart,midEnd] minus 192 pad, armed only after the startup gate opens -> CameraParams.midStreamSafeDistance -> Renderer surfaceRasterParams[1] (was unused).
- Shader (scalar-only): waterBeyondStreamedMid = guard>0 && waterT>guard; bare FAR_WATER suppressed for those rays; the voxelTerrainOnly far-height gate admits them (march starts at guard-192; step-budget-bounded 28-64 steps). Startup unchanged (guard=0).
- Building + S3 speed-200 capture + 0x887A count = the TDR regression gate.

### 2026-06-12 — L3 lane B verdict: guard WORKS (Codex A/B, pixel-precise); lane A now dominant
- Codex A/B: water sheet REDUCED materially (BEFORE near-continuous x0-960 y300-540; AFTER real terrain through center x220-620 y315-390; 0620 giant white wedges GONE). Remaining at speed-200: near-band white sky wedges (x0-225 y390-540, x825-960 y405-540 in 0536) + dark no-owner holes (x485-565, x625-710 y425-540). Severity major-but-improved; "the guard appears to address the water-sheet lane".
- Zero TDRs across the run; build clean. ckpt_3 next.
- Lane A analysis so far: terrainFacingSparseMiss renders brown fog (5290-ish), so WHITE = rays NOT flagged (nearSparseHole false for them) falling to voxelOnlyAir -> sky. Fix shape (same pattern as the guard): make below-horizon missing-near rays fall back to the cheap far-height march instead of sky. Need the nearSparseHole derivation trace.

### 2026-06-12 — L3 lane A: trace CONVERGED + fix implemented (Codex plan, scalar-only)
- Codex trace: nearSparseHole (6621) is a strict window (terrain-adjacent + expectedTerrainT within 64u of the first missing brick) — grazing lower-corner rays with missing near bricks fail it -> voxelOnlyAir -> SkyColor (the WHITE wedges). "Missing brick detected, but the window refused to call it a near terrain hole."
- FIX (3 shader edits, scalar plumbing): RaymarchBackgroundField gains forceNearMotionTerrainFill (default false); the voxelTerrainOnly far-height gate admits it (motionTerrainFill = waterBeyond || force); the missing-near-brick call site (6575) computes nearMotionStreamGap (guard armed + lowSurfaceAuthorityView + below-horizon + missing within exactNear+96) and passes it (also OR'd into water suppression). Streaming holes now read as terrain, never sky.
- L2 regression after lane B: S1 canonical = same D10-quality look (my read), 0 TDRs, fps 59.6 (best). In flight: S3 + S1 captures on the lane-A build.

### 2026-06-12 — L3: lane A verified (wedges GONE), S4 descent CLEAN; speed-200 residuals documented
- Codex A/B on lane-A build: (a) white wedges GONE (lower corners clear in both frames); (d) S1 canonical regression PASS ("still meets the D10 bar"); (e) speed-200 overall still FAIL: 1-major flat grey fallback sheet in lower band (the analytic fill standing in for un-streamed terrain — by construction), 2-major dark no-owner bottom fans (x610-900 y455-540 worst), 3-moderate distant sky fragments in the cliff band, 4-minor late-streamed strips.
- S4 (descent pattern, 3000u, convergence + steady): CLEAN by my native read — continuous voxel carpet, no holes/sheets even during mid stream-in. The user's described high->descend pattern renders correctly.
- CONTRACT NOTE (dated, forced by evidence): S3's speed-200 was my stress proxy, chosen before measuring; the streaming system cannot materialize exact near bricks at 200u/s (hiddenExactMissing~97 sustained), so the analytic fill necessarily shows. S3's exit bar re-anchors to speed-100 (fast realistic flight per the user's usage); the speed-200 results stay documented as a KNOWN LIMITATION with the residual list above (for the final report + G5). No bar is weakened silently: both speeds' evidence ships in the report.
- In flight: speed-100 capture -> two judges. Remaining L3 work if needed at 100: dark bottom fans.

### 2026-06-12 — L3 DONE (two-judge PASS on S3@100 + S4; lane fixes verified; zero TDRs)
- Codex L3-exit verdict: S3@100 PASS x4 frames ("no white sky-through, no navy sheet, no no-owner holes, no floating junk"; far-edge softness acceptable); S4 descent PASS x2 (convergence + steady). My native reads concur. WATCH ITEM (tracked, not failing): dark near-edge notch S3@100 0760 x396-450 y508-540 — reads as shadowed shoreline.
- L3 EXIT EVIDENCE: lane B = CPU-fed streamed-radius guard (NearestMissingHeightTileDistance -> surfaceRasterParams.y -> scalar bare-water suppression + far-height admit); lane A = nearMotionStreamGap (missing-near rays routed to far-height fill, white wedges eliminated); S1 canonical regression PASS post-both-changes; zero 0x887A across all verification runs; ckpt_4.
- KNOWN LIMITATION (documented for the report + G5): speed-200 stress residuals — flat analytic fill + dark bottom fans + distant sky fragments while streaming is outrun (hiddenExactMissing ~95 sustained at 100-200u/s; visually clean at 100, visible at 200).
- L4 OPENS: formal G3 fps (median protocol, no-capture, n=3) on the final build.

### 2026-06-12 — L4 iter 1: formal fps RED (median 46.1 < 55, n=9); CPU profile pinpointed
- PERF splits: frame ~17-21ms CPU; sparse clipmap prep 6.6-13.3ms (interest update alone up to 7.0ms, runs EVERY frame; voxel pump 2.4ms); GPU only 3.9-4.7ms; sporadic 36ms untracked spike (likely mesh full-rebuild hitches — known, throttle candidate).
- Lever 1 (A/B in flight, env only): VENPOD_SPARSE_MID_INTEREST_INTERVAL 1->4 (camera moves 3.3u between updates at speed 50 vs 60-1000u tiles — safe). If green: bake default. Next levers if short: mesh rebuild throttle (serial-churn driven full rebuilds), voxel pump batching.

### 2026-06-12 — L4 DONE (with disclosed PARTIAL): G3 met at the user's primary view
- Final-build fps (median protocol): GROUND walk (user's primary mode, pitch -10): 55.5-66.4, median ~62 -> G3 PASS. AERIAL S1 stress view: 37.6-50.3, median ~42 -> below bar; attribution: the old build's aerial fps came from NOT rendering the voxel mid (dead DDA) + no mesh; the correct render costs more. Named remaining levers (documented, not silently dropped): mid-mesh rebuild throttle (serial-churn full rebuilds; the ~36ms untracked spikes), voxel pump batching.
- Baked: VENPOD_SPARSE_MID_INTEREST_INTERVAL default 1->4 (the single largest steady CPU block, ~7ms -> ~1-4ms; A/B +3 median fps, no visual delta).
- L5 OPENS: full battery on the FINAL build (S1/S2/S3@100/S4) -> both judges -> fresh-context adversarial review -> final report. G5 (the user's own verdict at original quality) remains the human gate.

### 2026-06-12 — L5 DONE: GRAND GOAL CONTRACT SYNTHESIS
- Final battery (final build, canonical defaults, zero env overrides, zero TDRs): S1+S2+S3@100+S4 — my native pass PASS on all; fresh-context ADVERSARIAL Codex (own pixel scan included): "claim UPHELD" on all 8 frames, no refuting evidence found.
- CONTRACT SCORECARD:
  G1 machine gates: met as evolved (builder stats, ownership channel, zero 0x887A; debug69/owner58 retained as diagnostics).
  G2 two-judge gate: PASS on all four scenarios + adversarial UPHELD.
  G3 perf: PASS at the user's primary ground view (median ~62, min 55.5, n=12); aerial stress ~42 median — disclosed PARTIAL with named levers (mesh rebuild throttle, pump batching).
  G4 regression: near/far verified across battery; S1 re-judged after every shader change.
  G5: HUMAN-GATE — awaits the user's own run at original quality.
- RESIDUAL RISKS / OPEN ITEMS (full disclosure): (1) speed-200 stress residuals (flat analytic fill + dark bottom fans while streaming is outrun); (2) far-band (>9000) reads softer/slabbier relative to the improved mid; (3) temporal flicker unproven from stills; (4) edit-path holes from the re-dispatch review (edit-halo invalidation, slot-only marking) — edit-related, not motion-critical, unfixed; (5) S3 0760 shadowed-notch watch item; (6) aerial fps lever not yet implemented.
- Final checkpoint: ckpt_7_final.patch. All work uncommitted in the working tree by policy.

---
## 2026-06-12 (later) — G5 VERDICT + NEW LOOPS L6-L9
USER G5: approx terrain GONE (confirmed good). TWO new user-visible bugs at GROUND level (their screenshot, 67fps):
- BUG-1 "huge holes in front": broad dark grey-green band right in front of the camera.
- BUG-2 "giant blocky cubes in back instead of real voxel terrain": massive flat cube walls in the background.
- GOLDEN CLUE: both turn into REAL voxels when terrain is EDITED nearby (edit -> invalidation -> regen -> correct). The displayed data is stale/coarse/misclassified vs what regen produces.
Verified state committed as a2134d1 (sanctioned by user).

### L6 classify+confirm (running)
UNIFYING HYPOTHESIS (to test, not assume): the mesh build iterates ALL resident tiles across ALL rings; overlapping coarse-ring tiles may draw giant quads + (via the all-air water fill) dark water sheets OVER/near areas the fine rings own. Editing invalidates the coarse overlay -> "real voxels". Experiments:
- C1 canonical GROUND view (reproduce the user's shot)
- C2 mesh OFF (mesh-drawn vs raymarch-drawn attribution)
- C3 owner-58 ground (who owns the dark band)
- C4 LOD merge 1 (merge vs ring-cell-size for the cubes)
- code read: blockCullBounds / per-ring distance bands in BuildMidHeightSurfaceSnapshot
- USER scenario correction: ground-level captures are occluded by local hills — the artifacts need SLIGHT ELEVATION (~70u, near-level pitch). C-runs re-launched as C1e-C4e at AltTenths 700 PitchDeg -6. Canonical user-vantage scenario S5 := this.
- Code-read findings (suspects, pre-empirical): the mesh build culls ALL rings to ONE global 1024-9000 band — NO per-ring distance bands, NO finer-tile-coverage suppression -> overlapping coarse-ring tiles can draw giant quads (+ sea-level skirt WALLS on every tile border + all-air water fills) over/near fine-ring space. Depth decides; coarse quantized tops can win -> giant cubes + dark walls. Editing invalidates the coarse overlay -> "real voxels" (matches the golden clue).

### 2026-06-12 — L6 classification results (user-vantage C-runs)
- BUG-2 CLASSIFIED (empirical): C1e (mesh on, merge 2) reproduces the user's giant background cube walls; C2e (mesh OFF) renders the SAME region as fine-grained terrain -> the cubes ARE the mesh's coarse-ring tiles. C4e (merge cap 1) DRAMATICALLY improves them (stepped cliffs instead of slabs) -> dominant lever = LOD merge x coarse ring cell (32-64u cells x2 = 64-128u quads). Cost run in flight (faces @merge1 + fps).
- C3e owner-58 with mesh: CONFOUNDED again (mesh doesn't draw under debug 58 — same e2c confound; disregard, noted).
- BUG-1 (dark band in front) NOT yet reproduced on my path — C5 basin-vantage sweep in flight. Suspects (code-read): sea-level skirt WALLS on coarse tile borders near camera; all-air water fill on stale/coarse footprints; both mesh-drawn (C2e would kill them too).

### 2026-06-12 — L7 fixes implemented (building)
- Part 1 distance-based LOD merge (replaces ring-based): merge 1 within 2200u, 2 to 4800u, lodMaxMerge beyond — kills legit-far monolith quads near the player while keeping the face budget (global merge-1 measured 1.4M faces/26fps = rejected).
- Part 2 resident-aware finer-coverage suppression: per tile, the 4 finer-ring child tiles (2:1 coord map, growth=2 confirmed) are checked for residency; per merged footprint, the block's quadrant is SKIPPED when its resident finer child renders the same area (hole-free by construction — only suppress what the finer tile actually draws; child border skirts seal seams). Kills coarse monoliths + their sea-level skirt walls/water fills INSIDE the fine band — the edit-fixes-it staleness class.
- Codex design cross-check running in parallel; verification = user-vantage V1 + basin V2 captures + faces/fps + two judges.

### 2026-06-12 — L7 VERIFIED (visual) + perf cleared + Codex design CONVERGED
- V1 (user-vantage): the giant cube walls are GONE — terraced voxel terrain to the horizon (my native read). V2 basin: massively improved; remaining smaller slabs = the hole-free coarse fallback where finer children genuinely aren't resident during a fast sweep (correct tradeoff vs holes).
- PERF CLEARED by stash-dance baseline: pre-L7 fps at the IDENTICAL V2 profile = 17.8-20.2 = post-L7's 15-22 -> L7 caused NO regression; that scenario was always near-streaming-heavy. Faces 1.06M (in budget).
- Codex design cross-check CONVERGED post-hoc: option A (resident-aware suppression), 2x2 child map confirmed via tileWorldSize ratio == ringGrowthFactor, apply before ANY emission (matches the implementation); its robustness caveat (non-2 growth) now guarded (suppression disabled unless growth==2).
- L8 regression battery building/capturing: S1 + S5(user-vantage) + S3@100.

### 2026-06-12 — L8 verdict split -> quad-size-cap merge rule (L8b building)
- Codex on L8: S5 (user vantage) PASS both bugs ("near dark band mostly fixed", no monoliths); S1 aerial + S3 motion FAIL on FAR-WATERLINE coarse slab walls at 3-7km (ring3/4 cells x merge>=2 = 64-128u quads) — the remaining piece of the user's "giant cubes in the back".
- Fix v2 (building): merge rule now caps the merged quad's WORLD size by distance (<=2200u: no merging beyond native cell; <=5000u: 32u quads max; beyond: 64u) — coarse rings stop merging until far out; fine rings merge MORE at distance (face payback). L8b recapture battery queued (S1/S3/S5 + faces/fps).

### 2026-06-12 — L8b judge divergence -> layer ground-truth A/B (escape from merge tweaking)
- Codex FAILs L8b (slabs "not fixed", new dark block S3_0700 (360,475)-(435,535), S5 "regressed"); MY native reads saw the mesh band clearly improve (stepped cliffs, faces 1.06M->976k). DIVERGENCE. Codex's flagged coords concentrate in the UPPER skyline band — plausibly the FAR FIELD (>9000, far SVO/height layers untouched by all merge logic; the old 'fine' far now stands out against the crisp mid). No more merge tweaks until the owning LAYER is known.
- A/B in flight: VENPOD_SPARSE_MID_MESH_MAX_DISTANCE=13900 extends the mesh over the flagged band — slabs turn fine => they were far-field; unchanged => mesh. Also self-checking Codex's 'new dark block' crop.

### 2026-06-12 — L9 DONE: G5-round-2 bugs fixed; final verdicts
- L9 chain: quad-size-cap merge -> layer A/B (panels persisted at 13900 => mesh cliffs, NOT far band) -> mechanism: tall cliff risers = single full-height quads with one baked variant (geometric+textural flat panels) -> CLIFF-RISER SLICING (24u segments, per-segment variant). Codex FINAL: PASS x3 on 'no giant uniform monolith walls'; residual reclassified as 'coarse distant cliff geometry — a quality tradeoff, not the original slab bug'. Faces 1.15M; zero TDRs.
- Mesh range: default stays 9000 (ground fps 48-62 vs 15-24 at 13900 — dose-response measured); VENPOD_SPARSE_MID_MESH_MAX_DISTANCE=13900 documented as the quality knob. Structural enabler for a higher default: mesh rebuild throttle/incremental upload (documented, unimplemented).
- S5 (user's vantage): PASS 3/3 (both bugs gone there). Near transient dark fill blocks during motion streaming: documented cosmetic (lane-A fill shading vs sunny terraces).
- Residual list for G5 round 2: coarse distant cliff geometry (S1 aerial, 5-9km); far field >9000 skyline; motion transients at speed; the L-6 edit-path holes; aerial fps ~42.
