# SIEGE 3: VENPOD mid-voxel terrace regeneration (root-cause, user's REAL scene)

## WIN
On the user's exact run (`rebrun -NoBuild`, no input, no walk-test), a static camera looking at the
terraces: the periodic mid-voxel regeneration bursts STOP. PROVEN by (a) reproduce the churn on the
IDLE (no-walk-test) scene at the user's magnitude, (b) a toggle that makes the churn come/go (causal),
(c) HUMAN-GATE user confirms.

## SCOPE-HONESTY: "A cited example or reported symptom is a SAMPLE of the defect, never its extent."

## CONSTRAINTS: NO code fix until the mechanism is PROVEN on the IDLE scene. Stop touching plausible fixes.

## SOLVED GROUND (proven, protected — do NOT re-litigate):
- Churn is 100% MID-VOXEL raymarch (owner cross-ref). Near-field is irrelevant.
- All 4 near-field trim fixes = DEAD: user toggled VENPOD_SPARSE_TRIM_STATIONARY_VISIBLE, ZERO change.
- My walk-test repro's 295->0 toggle sensitivity is a HARNESS ARTIFACT — does NOT reproduce on user scene.
- At rest: genVoxel/evictVoxel=0, midUpload=0, owner-diff=0 (all mid-voxel inputs stable). Churn is
  DRY terrain (not water/frameIndex). PERIODIC bursts (~every 4-5 frames @30fps in user video).

## INSTRUMENT REGISTRY
| gauge | verdict | note |
| static_diff over LONG window | TRUSTED | noise floor 45 vs 46 proven |
| eviction/trim counts | LYING (wrong system) | user toggle refuted |
| WALK-TEST static repro | LYING (harness artifact) | doesn't match user scene |
| IDLE scenario repro | UNPROVEN → must validate matches user | NEXT |

## AUDIT C — must reproduce the IDLE (no-walk-test) scene = the user's actual game.

## APPROACH TREE (after idle repro)
| # | class | prediction | probe | kill-criteria | status |
|1| render-feedback loop cycle | disabling feedback stops idle churn | -DisableMidFeedback on IDLE + MISS_FEEDBACK off | churn persists w/ all feedback off = not it | NEXT |
|2| a pump/generation on N-frame cadence | midGen/dirty bumps periodically | per-frame midGen/serial on IDLE | no periodic bump = not it | queued |
|3| GPU/UAV race (non-deterministic raymarch) | same static state renders differently | determinism test / debug modes | deterministic = not it | queued |
|4| animated terrain shading I haven't found (grass sway etc.) | a time term applied to dry terrain | grep shader for time in terrain path | none applied to dry terrain = not it | queued |

## INSTRUMENT AUDIT RESULT (TRUSTED)
Pixel-diff burst-count on in-engine captures = TRUSTED: idle FIX-OFF = 27% burst frames (32/119),
user SCREEN RECORDING = 28% (39/140). MATCH → capture faithfully shows the bug, NOT suppressed.
Idle FIX-ON = 2% (2/119) → the trim fix IS a real reduction (user's single-env toggle earlier reverted
only 1 of 3 fixes → looked unchanged). RESIDUAL = the 2 fix-on bursts + the majority the trim removes.
User can't reliably see the visual difference → MUST use telemetry, not perception.

## ROOT-CAUSE PLAN (empirical, telemetry-correlated)
Run idle FIX-OFF (reproduces bug @27%) with per-frame telemetry. Identify burst frames from the pixel
diff. Find the telemetry event (generation/upload/feedback/dirty/pump) that fires ON burst frames vs
quiet frames. That correlation = the cause. Then toggle that cause → bursts vanish = proven.
Burst location (both on/off): y[553..677] band = the mid-distance terraces. Bursts span wide x (whole
terrace row) → a per-frame event re-rendering a whole terrace row/tile, not scattered noise.

## ★★EMPIRICAL ROOT-CAUSE CHAIN (telemetry-proven, 2026-07-03)★★
User demanded telemetry, not perception ("might have imagined" the visual diff). Chain, each link proven:
1. Trusted instrument: idle FIX-OFF pixel-diff = 27% burst frames == user video 28% (capture NOT suppressing).
2. Per-frame correlation (PERF_FRAME_END timings vs changed_px, joined by engine frame#):
   `surfEmit` = #1 correlate r=0.526 (fix-on), 0.257 (fix-off); surfStage #2. **midUpload r=0.000 →
   mid-clipmap re-upload theory DEAD.** The bug tracks the SPARSE SURFACE extract→stage→emit pipeline.
3. Surface `cpuSerial` (=m_surfaceCache serial, SparseVoxelWorld.cpp:8600) CLIMBS at static camera:
   fix-on ~0.8/frame (1030→1178), fix-off ~8.5/frame (3004→4528). = surface RE-EXTRACTED at rest.
4. Surface extraction is queued ONLY on brick (re)generation (ApplyGeneratedBrickPayload:1950,
   PromoteDeferred:1891, async:4639/4878) or edit-dirty (6563, edit-only → N/A at rest).
   → bricks are being RE-GENERATED at rest.
5. Eviction TREADMILL: readiness `evicted` climbs fix-on ~10/frame, fix-off ~22/frame; `generating`=9-36,
   `reqVis`=9-20/frame; `resident` STABLE ~2240. Evict N, regen N, net ~0 = treadmill.
6. ★NOT capacity pressure★: pool 93% FREE (free=30500/32768, usagePct=28.8%). evictLast=0,
   pressureTrim=0 (my fix killed pressure trim), replaceEvict=0 — yet eviction climbs via ANOTHER path.
   → SPURIOUS eviction of resident bricks with a near-empty pool.
7. My trim fix cuts the rate 8.5→0.8/frame (real, matches fix-on 2% vs fix-off 27% bursts) but a
   RESIDUAL eviction path remains → residual regen → residual surface re-extract → residual bursts.

OPEN (the last link): WHAT re-requests/evicts resident bricks at rest with a 93%-free pool.
Candidates: interest-set breathing (tiles flip in/out per frame → re-request), readiness `evicted`-state
entries leaking (count > tracked, grows unbounded → maybe re-processed), a non-pressure interest/LOD evict.
NEXT: find the reqVis source + the non-pressure eviction path; CAUSAL TOGGLE (stop at-rest requests →
cpuSerial stops climbing → bursts stop) = the proof before any fix.

## ★★★ROOT CAUSE PROVEN (causal toggle, 2026-07-03)★★★
ROOT = VENPOD_SPARSE_VIEW_FOLLOW_TRIM (default ON, main_launcher.cpp:1526) → TrimStaleResidentBricks
(SparseVoxelWorld.cpp:5669, call 12223). Evicts ANY resident brick with lastTouchedFrame < currentFrame-120
(test 5711, evict 5727), IGNORING pool pressure/free pages/stationary camera. At rest the per-frame
request replay only touches the current request/repair subset; fully-resident visible terrain outside it
is never re-touched → ages past 120 frames → shed with a 93%-free pool → re-requested → regenerated a
slightly different surface → THE VISIBLE REGENERATION.
CAUSAL PROOF (idle scene, 3-way, same camera):
  view-follow-trim DEFAULT: median 452, 32/119 burst frames, max 2941  (== user video 28%)
  my partial trim gates:    median 0,   2/119,  max 1857                (residual)
  VENPOD_SPARSE_VIEW_FOLLOW_TRIM=0: median 0, 0/119, max 45  → ELIMINATED
  + cpuSerial FLATLINES at 1836 (was climbing forever), gpuFaces flat 602018, evicted=0, generating=0.
Convergence: Codex independent trace found the exact path; my telemetry chain (surfEmit r=0.526 +
cpuSerial climb + 93%-free pool) converged. NOT capacity, NOT mid-clipmap, NOT frameIndex/water.
Instrument audit: pixel-diff burst-count TRUSTED (fix-off 27% == user video 28%). Toggle is CAUSAL.

## FIX PLAN (proven cleared)
Preserve the trim's purpose (bound working set during flight) but stop it shedding still-wanted terrain.
Option B (chosen, minimal+proven): skip TrimStaleResidentBricks when camera effectively STATIONARY
(sparseCameraSpeedLastFrame < motionLookaheadMinSpeed) — stationary ⊂ trim-off (proven), trim still runs
when moving, at-rest residency ~7% of pool so zero memory risk. Gate at main_launcher.cpp:12223.
(Option A = touch all wanted-visible resident bricks each frame; more general but enumeration is deep/
risky — deferred, cross-checking with Codex.)
VERIFY AFTER FIX: rebuild, re-run idle (default, no env) → must match vftoff (0 bursts, cpuSerial flat).
Then A/B env still works (VENPOD_SPARSE_VIEW_FOLLOW_TRIM=1 restores). HUMAN-GATE: user confirms.

## BEAT LOG
- 2026-07-03: Root PROVEN via causal toggle (view-follow-trim off → 0 bursts, cpuSerial flat).
  Fix = gate TrimStaleResidentBricks on stationary (main_launcher.cpp:12223 + config ~1534,
  env VENPOD_SPARSE_VIEW_FOLLOW_TRIM_WHILE_STATIONARY=1 restores old). Codex converged on same fix.
- 2026-07-03: Rebuilt (exe 17:29:46), verified DEFAULT build (no env): siege3_idle_fixed =
  0/119 burst frames, max Δ33, cpuSerial FLAT at 1833, evicted=0, generating=0. Regeneration GONE.
  (old default = 2 bursts + cpuSerial climbing.) Disconfirmation A/B running (restore → bursts return).

## CLOSURE GATE — status
- [x] Instrument audit: pixel-diff burst-count TRUSTED (fix-off 27% == user video 28%); cpuSerial +
      readiness telemetry cross-check. All cited gauges proven.
- [x] Root cause proven (not correlated): causal toggle moves the effect on/off.
- [x] Fix verified on the DELIVERED artifact (default build, no env): 0 bursts + cpuSerial flat.
- [x] Disconfirmation A/B: siege3_idle_restore (WHILE_STATIONARY=1) → 3 bursts + cpuSerial climb
      RETURN (env only exists in fixed code → proves new binary + gate is the cause).
- [x] Regression sweep: WALK (siege3_walk_fixed) → ran to frame 479 no crash; trim ACTIVE while
      moving (evicted 1902→3213, stops when bot halts); memory bounded (free ~28-29k, 86%+);
      visibleMissing=0. Moving path unchanged; only at-rest behavior changed.
- [ ] HUMAN-GATE: user runs `rebrun -NoBuild` (exe already rebuilt 17:29) at rest and confirms.
## CLOSURE GATE — CLEARED (all automated boxes; human-gate pending). Fix shipped, proven both directions.
Committed 1e950d0 (wip-edit-latency). USER CONFIRMED rest regen GONE.

## ============ FRONT 2: MOVING regeneration/shimmer (user: "still very bad when we move") ============
## WIN: moving around, surface does not abnormally regenerate/shimmer (genuine motion-compensated
## novelty near zero beyond legitimate streaming/parallax + LOD refine). PROVEN by telemetry (cpuSerial
## re-extraction rate isolated from legit streaming via same-trajectory A/B) + a causal toggle + user gate.
## INSTRUMENT AUDIT: raw pixel-diff LIES on motion (parallax saturates). TRUSTED = cpuSerial/surfEmit
## re-extraction rate (camera-independent). Walk path is DETERMINISTIC → same-trajectory A/B isolates
## a mechanism's contribution cleanly. Walk bot verified moving (cam -208→-157, yaw 0.34→0.76 in window).
## HYPOTHESES:
##  H1 same root as rest: recency trim evicts STILL-VISIBLE terrain while moving (side/persistent terrain
##     not re-requested → ages out → evict+regen). Probe: walk VIEW_FOLLOW_TRIM=0 vs default, same path;
##     if cpuSerial climb rate DROPS → trim is (part of) the moving cause. General fix = don't evict
##     bricks within the camera interest/render footprint (spatial, not pure recency).
##  H2 LOD coverage-handoff pop (childMask cache-key cascades, computeTileLod) as camera crosses bands.
##  H3 subpixel edge crawl / temporal aliasing (jittered-TAA feature territory; memory Loop 96/115).
## STATUS: running walk VIEW_FOLLOW_TRIM=0 A/B (siege3_walk_vftoff) to test H1 via cpuSerial rate.

## MOVING FINDINGS (2026-07-03)
## H1 (trim) PARTIAL: same-path A/B cpuSerial 5.5/fr (trim on) vs 3.6/fr (off) = trim adds ~1.9/fr
##   spurious (turn-around re-request). BUT motion-compensated pixel novelty identical (~31000 both)
##   AND saturated by parallax → trim is NOT the dominant VISIBLE moving cause. Pixels CANNOT measure
##   moving regen (motion saturates even block-matched mc-novelty) → telemetry-only + motion-FREE probes.
## H2 (LOD pop) LIKELY DOMINANT: NO geomorph/fade/blend/hysteresis anywhere (grep empty). mergeCells
##   flips at hard distance thresholds 2200u/5000u (SparseClipmap.cpp:7852, no dead-band). Mid-voxel
##   ring residency shifts as camera moves (res 3893/4214→3339/5460). computeTileLod @7798.
## ★KEY (memory Loop 110-111): re-tessellation is NON-IDEMPOTENT — a mesh rebuild re-tessellates
##   UNCHANGED regions to slightly different geometry (stitching/LOD reads neighbor context). This is
##   the moving crawl AND is testable MOTION-FREE: force a tile rebuild twice at a fixed camera, diff →
##   nonzero = the bug. Fix = make tessellation deterministic in tile-local+interest terms (double-
##   rebuild diff = 0). Also: LOD threshold HYSTERESIS (cheap) if tiles flip merge/LOD at band edges.
## NEXT: motion-free idempotency probe (double-rebuild diff); Codex independent LOD/handoff read.

## MOVING ROOT CAUSE — IDENTIFIED (Codex CONVERGED 2026-07-03)
Moving artifact = HARD LOD HANDOFFS (no hysteresis/geomorph/blend anywhere — grep empty), NOT surface
regeneration (that was the rest bug). Two independent hard-pop systems:
  1. Mid-HEIGHT mesh: mergeCells flips at hard dist thresholds 2200u/5000u (SparseClipmap.cpp:7852);
     parent block SKIPPED when finer child resident (7587) = binary ownership, no blend. LOD cache
     (7811) is memoization NOT hysteresis.
  2. Mid-VOXEL raymarch: ring select = hard floor(saturate()*ringCount) (PS_Raymarch.hlsl:2026) →
     SampleResidentMidVoxelFallback preferred/finer/coarser (1764). No inter-ring blend.
Trim EXONERATED for moving: its cpuSerial residual = legit exact streaming/repair, computeTileLod/
childMask never queues surface extraction (Codex). mc-novelty can't measure (motion saturates).
FIX = geomorph/fade/blend at the handoff (a rendering FEATURE, memory task #14 "DO FIRST"); the two
systems need separate treatment. NO cheap gate: smooth movement crosses each threshold ONCE →
single hard pop → hysteresis won't help (only helps jitter); needs a true morph/blend.
ISOLATION TOGGLES (which system's pop dominates — needs the USER's EYES, pixels saturate on motion):
  VENPOD_SPARSE_MID_MESH_LOD=0      → freezes mid-height mesh mergeCells pop
  VENPOD_SPARSE_MID_VOXEL_RENDER=0  → removes mid-voxel ring/fallback pop
  (debug modes 59/65/70 = ring/parent-held/chunk ownership viz)
STATUS: root cause found; fix is a geomorph/blend FEATURE + unmeasurable-by-pixels → need user to run
the 2 toggles and report which kills the moving shimmer, then build the targeted blend WITH their gate.
