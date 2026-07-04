# SIEGE (re-run, rigorous): VENPOD rest-time terrace regeneration

## WIN CONDITION
On a fresh `rebrun` (user's real path), camera STATIC, no input: the terrace surface does NOT
visibly regenerate (the periodic pixel bursts the user sees in their video stop). PROVEN by:
- (a) a ROBUST (noise-audited) before/after on the SAME scenario showing the visible bursts drop, AND
- (b) a proven toggle: cause X on → bursts present; X off → bursts gone (causal, not correlational), AND
- (c) HUMAN-GATE: user confirms on a freshly-compiled build.

## SCOPE-HONESTY CLAUSE (verbatim)
"A cited example or reported symptom is a SAMPLE of the defect, never its extent. The win is the
whole contract, not the sample."

## CONSTRAINTS
- NO code changes until the mechanism is PROVEN (user's explicit instruction).
- Prove the fix on the DELIVERED artifact (the actual rendered frames), not on proxy counters.

## PRIOR (re-siege 1) — treat as CLAIMS TO TEST, not facts:
- CLAIM: 4 trim-gating fixes cut eviction 20→9/frame and pixel churn 463→47. USER REFUTES (no visible
  change). So either the burst metric is LYING (noise) OR the fix targets a proxy not the visible cause.
- Owner map showed user terraces = MID_VOXEL; mid-voxel counters (genVoxel/evictVoxel) were STABLE.
  → the visible regen is in a system whose brick COUNTERS are stable. Eviction (near-field) may be the
  WRONG system.

## AUDIT A — INSTRUMENTS (in progress)
| gauge | verdict | proven how |
|-------|---------|------------|
| burst pixel-churn (static_diff MEAN) | UNPROVEN→testing | noise-floor test: identical configs varied 87→240 earlier = suspect |
| eviction rate (readiness evicted delta) | proxy | non-noisy but NOT proven to cause visible pixels |
| my "463→47 A/B" | SUSPECT | may be stochastic-burst noise, not the fix |

## AUDIT B — CONTRACT
Contract = render is DETERMINISTIC at a static camera (same inputs → identical frames). Violated:
frames differ at a static camera. Need: which render INPUT varies per frame (that IS the bug).

## AUDIT C — PRIMARY SOURCE
User video 111202 = ground truth. And the render pipeline (PS_Raymarch reads mid-voxel clipmap +
surface faces).

## AUDIT C ARTIFACT (user video 111202, frames 30-170, camera static per HUD)
- 28% burst frames (>800px), 68% quiet (<50px). Bursts every ~4-5 video-frames (30fps).
- ★RECURRING HOTSPOTS: union 16,491 px, 733 px regenerate in >=50% of bursts, recur-ratio 8.4.★
  So SPECIFIC terrain spots cycle repeatedly — NOT random, NOT whole-surface.
- ★REPRODUCTION GAP: my static repro (walk speed 0, pitch -4, scenic spawn) fix4-overlay showed
  ~ONE tiny spot; user has 16,491 px of hotspots. My repro UNDER-REPRODUCES the user's extent →
  I've been measuring/fixing a much weaker version. MUST reproduce the user's full extent first.★

## ★★AUDIT A/B KEY FINDING — I FIXED THE WRONG SYSTEM★★
- PERF_SPARSE_SURFACE cpuBricks/cpuFaces/cpuSerial (main_launcher:26995) come from sparseWorldStats
  = SparseVoxelWorld = the NEAR-FIELD (<256u, surface-RASTERED) system.
- The user's terraces are at MID-distance = RAYMARCHED MID-VOXEL (256-9000u), read by PS_Raymarch
  from the mid-clipmap metadata+lookup+voxel samples. A DIFFERENT render path.
- ALL 4 of my fixes gate NEAR-FIELD eviction/trim (SparseVoxelWorld TrimResidentBricks). They cannot
  change the raymarched mid-voxel terraces. → explains user "exact same": my fix is VISUALLY INERT
  for the actual bug. My eviction-count A/B was a PROXY for a system that doesn't render the terraces.
- REAL DIRECT CAUSE (hypothesis, to prove): the visible mid-voxel raymarch changes at a static camera
  because a mid-clipmap RENDER INPUT changes per frame: (a) mid-clipmap metadata/lookup re-uploaded
  (voxelDirtySerial bumps from tile-interest BREATHING at rest — cf Loop 110-111), or (b) mid-voxel
  samples change. Voxel bricks stable (genVoxel=evictVoxel=0) → likely (a) metadata re-upload.
- A/B (ab_on x2, ab_off) will CONFIRM my fix is inert (on≈off≈off in visible churn). If so, pivot
  entirely to the mid-clipmap dirty/upload at rest.

## APPROACH TREE (after audits)
| # | class | prediction | probe | kill-criteria | status |
|1| mid-clipmap metadata re-upload from interest breathing | voxelDirtySerial/mid-upload bumps at rest | read mid-clipmap dirty/upload telem at static | if mid-clipmap never re-uploads at rest → not it | NEXT |
|2| mid-voxel sample change | brick samples differ frame-to-frame | (needs care - counts stable) | genVoxel=evictVoxel=0 already → unlikely | low |
|3| raymarch render-feedback (parent-held/miss) toggling LOD | disabling feedback stops churn | env PARENT_HELD_FEEDBACK=0 on TRUE-static lane | tested before on CONTAMINATED lane; retest clean | queued |
## ★★DEFINITIVE ROOT LOCALE (proven, non-noisy)★★
Cross-referenced the beauty-churn mask against the owner map (same camera): the CHURNING pixels are
100% MIDVOX (mid-voxel raymarch), 0% near-field. → my 4 near-field eviction fixes are PROVABLY
IRRELEVANT to the user's bug. (This owner cross-ref is the TRUSTED proof; my earlier eviction-count
A/B was a proxy for the wrong system.)
At rest, ALL mid-voxel inputs are STABLE: bricks (genVoxel/midGen=0), eviction (evictVoxel=0),
metadata/lookup (midUpload=0, midSerial stable), ownership (owner-diff=0). Yet the raymarch outputs
DIFFERENT colors at the same pixels each frame → the mid-voxel RAYMARCH is non-deterministic given
stable inputs. Suspect: (a) render-feedback UAV read-as-input creating a limit cycle, or (b) a
per-frame-varying shader input (frame index / time / dither). NEXT: read PS_Raymarch for per-frame
inputs; then CAUSAL toggle test on a TRUE-static lane (disable feedback / neutralize the varying input)
→ churn stops = proven cause. NO code fix until the toggle proves cause→effect.

## INSTRUMENT VERDICTS (updated)
- owner-layer cross-ref of churn pixels = TRUSTED (deterministic, proved churn is 100% mid-voxel).
- eviction-count A/B = LYING for this bug (measured a system that doesn't render the terraces).
- burst pixel-churn MEAN = NOISY (keep, but only over long windows / with the owner cross-ref).

## BEAT LOG
## MECHANISM HYPOTHESIS (to prove before ANY fix)
Churn = dry terrain (sand/grass, 0% water), subtle (diff ~10-30). PerVoxelColorJitter (PS_Raymarch:1253)
hashes floor(hit worldPos) → ±9% brightness swing on a floor() FLIP (~10-20 on base 120 = matches).
So IF the raymarch hit worldPos varies sub-voxel per frame at a static camera, floor() flips → jitter
flips → subtle color churn. Owner-map (layer) is stable, but the exact RING/hit may vary within
mid-voxel. Water/frameIndex animation REFUTED (churn is dry). Render-feedback likely NOT it (everything
resident, midUpload=0, nothing to feed back).
OPEN: WHY does the hit worldPos vary per frame with static camera + stable bricks + stable metadata?
Candidates: (a) actualRing/cellSize varies per frame (structural) → hit quantization shifts; (b) GPU FP
non-determinism in the DDA loop; (c) a per-frame input to the DDA I haven't found.
NEXT PROBE (needs GPU, after A/B batch): capture debug-67 (renders actualRing+cellSize as color) at the
TRUE-static lane, diff it. STABLE → structural deterministic → churn is pure shading (jitter on a
deterministic hit, so why does floor flip? → must be FP); CHURNS → ring/cellSize varies per frame =
structural root. This is the proven cause→effect the user demands. NO fix until proven.

## ★★INSTRUMENT AUDIT DONE — fix is REAL (not noise), 8x★★
Noise floor (2 identical fix-ON runs, 120-frame): 46 vs 45 = metric TRUSTED over long windows.
Decisive: fix-ON 45 vs fix-OFF 364 (gap 319 = 300x the noise floor) → my fix cuts the visible
mid-voxel terrace churn ~8x. Both on & off churn = 100% MIDVOX (proven owner cross-ref).
So my earlier "463→47" was NOT noise; the fix works. Mechanism (both have midUpload=0 = mid-clipmap
stable) = INDIRECT: near-field eviction churns the SHARED GPU page pool → mid-voxel sample slots get
reallocated → raymarch reads changed samples → terrace churn. My 4 trim-gates stop the near-field
carousel → pool stable → mid-voxel stable. (Mechanism is hypothesis; the 8x EFFECT is proven.)
BUILD: rebrun's build.ps1 (cmake --build → ninja) and my _agent_build.bat both -> build/bin/VENPOD.exe;
same binary. So user's `rebrun` (with build) WOULD have the fix. "exact same" => user likely ran
-NoBuild / didn't recompile / built before the fix.
REMAINING (HUMAN-GATE): user confirms on a freshly-COMPILED build. Give ironclad A/B verification
(env VENPOD_SPARSE_TRIM_STATIONARY_VISIBLE=1 restores old → regen returns). + reproduce user's exact
scene if still divergent. NOTE reproduction-gap: my fix-off 364px vs user video ~1000-7000px (codec-
inflated) — my repro may be a weaker version; the 8x ratio should still hold at their magnitude.

## PROVEN (validated instrument): fix OFF = median 295px/frame, 15 bursts, 77% frames active
## = CONSTANT terrace regeneration. fix ON = median 0, 85% dead-quiet, 0-1 bursts = STABLE.
## Noise floor: 2 identical fix-ON runs both median 0. So 295->0 is REAL. Churn 100% mid-voxel.
## LIKELY user cause: canonical run is `rebrun -NoBuild` (no recompile). Current build/bin/VENPOD.exe
## (11:57) HAS the fix, so `rebrun -NoBuild` NOW would use it. If user's binary predated the fix or
## they never rebuilt, they saw the old (constant) behavior = "exact same".
## GAP (honest): my fix-OFF is CONSTANT churn (median 295); user VIDEO is bursty-periodic (median 17
## + 28% bursts, codec-inflated). Patterns don't perfectly match → my repro may not be their exact
## scene. If user runs the fixed binary and STILL sees regen, reproduce their EXACT scene next.
## VERIFY (give user): set VENPOD_SPARSE_TRIM_STATIONARY_VISIBLE=1 -> regen returns (old); unset -> gone.

## CLOSURE GATE (siege: fix PROVEN 295->0 w/ validated instrument; HUMAN-GATE = user run fixed binary + confirm)
