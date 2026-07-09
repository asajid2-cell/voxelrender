# Near→Mid Raster Handoff — Postmortem & Real Fix Plan

**Scope:** the "sharpening wave" — mid-height raster terrain near the camera visibly collapsing
into finer voxels as exact surface data arrives, proven by debug mode 94.
**Source material:** the full 2-day Codex session (rollout 2026-07-04, 18,463 lines), the 14
uncommitted files in the working tree, `LOOPS.md`, `CAMPAIGN.md`, `docs/Brain-dump.md`.

---

## 1. The bug, stated correctly (finally)

There are **two layers**, and the campaign conflated them for most of two days:

**Layer 1 — the visual mechanism (what you see).**
The raster mid-height surface (pass kind 2, built by `SparseClipmap`, drawn in
`renderSparseMidMeshLayer`) is allowed to own pixels **right up to the camera**. Its coarse
face-size grid changes in view for three reasons: (a) exact-surface tiles become resident and
overpaint the coarse mid faces (mid→exact sharpening), (b) `computeTileLod`'s XZ distance bands
(thresholds 2200 / 5000 → face sizes cell/32/64) sweep across terrain as you move (mid→mid
re-banding), (c) dirty mid tiles re-upload while visible. All three read as "terrain
regenerating/sharpening."

**Layer 2 — the structural cause (why mid is near you at all).**
`surfaceRasterMaxDistance` (e.g. 2560) is a **draw intention, not a coverage guarantee**. Exact
surface extraction + GPU page upload is incremental and budgeted; the promotion gate's shader
feedback **never sees pixels already claimed by mid raster** (the mid-underlay audit proved it:
`exact_brick_not_resident=15`, `surface_faces_exist_but_ray_has_no_exact_intersection=7`…), and
the `high_alt_excluded` branch (`main_launcher.cpp:23701-23705`) clamps exact to
`sparseExactNearDistance` independently of what mid is told. So **the exact ring is genuinely
full of holes at all times**, and mid silently fills them — that is *why* mid draws so close.

This directly answers both of your final questions:

- **"Why is there even mid raster height grid so close to us?"** → Because exact residency
  inside its advertised radius was never guaranteed, and mid is the hole-filler. Both mid
  near-gates (`sparseMidMeshNearExclusionDistance`, `surfaceRasterMinDistance`) default to 0
  *by design accident*, and removing the filler exposes the holes.
- **"Why are there holes in our inner ring / why isn't our ring full?"** → Because exact
  extraction/upload is incremental, budgeted, feedback-blind to mid-claimed pixels, and
  promotion is a *global boolean over a scalar radius* rather than per-tile readiness. This was
  **hypothesized in the session's final minutes but never measured or fixed.** No coverage
  metric ("N of M tiles inside radius R are render-ready") exists anywhere in the engine today.

---

## 2. Where the campaign went wrong

### 2.1 Category error: a readiness problem was attacked as a distance policy
Every fix attempt — ~16 of them — was a variant of **"forbid mid inside radius R"**:

| Attempt | Mechanism | Outcome |
|---|---|---|
| Near-exclusion 1024/512/256 | CPU `surfaceRasterMinDistance` | grey hole / residual grid |
| `SUPPRESS_VISIBLE_NEW_UPLOAD` | hold new mid publishes | grey void (nothing covers) |
| Stencil `INCR_SAT→KEEP` | mid stops claiming stencil | raymarch overdraws mid, grey |
| Underlay discard in prepass | PS discard inside exact band | grey, `surfaceFragments≈331` |
| Draw-order swap (mid first) | exact wins on top | grey (stale stencil rejects mid) |
| `EXCLUDE_PUBLIC_EXACT=1` (current default!) | `midMin = exactMax` | **the grey holes you saw last** |

These are all **isomorphic**: they exclude mid by a *scalar radius* while exact coverage is
*per-tile and incomplete*. The mismatch between a scalar contract and per-tile reality **is**
the bug. Any radius-shaped fix must fail in exactly one of two ways — sharpening still visible
(radius too small) or grey holes (radius bigger than actual coverage). Two days were spent
walking that Pareto frontier instead of leaving it.

### 2.2 Metric inversion: "mode-94 magenta == 0" was treated as success
Mode 94 tints coarse **mid** faces. Zero magenta proves *mid didn't draw* — it says nothing
about *exact drawing instead*. Deleting mid trivially achieves it. Codex optimized to this
metric at least four times (checkpoints at 07-05 14:41, 07-06 00:39, 07-06 02:58, and the final
`update_goal complete` at 07-06 05:40) and each "green" was falsified by your next screenshot.
The correct success metric was always two-sided: **exact∪mid must tile the near field with no
in-view owner flips** — i.e. mode 96 (owner overlay) + the `PERF_RENDER_OWNERSHIP` counters,
plus a hole counter that never got written.

### 2.3 Verification against the wrong distribution
Greens were measured on `captures\vertical_bob_repro.vnrd` at frame 660 — a high-altitude,
stationary-XZ, exact-demoted slice. Your live interactive camera (low, moving, turning) was the
actual failing distribution. Late in the session Codex itself noted the replay measured 0%
while live screenshots were broken. Every future gate must include a default live-like scene.

### 2.4 The agreed design was never implemented
After the adversarial review (Einstein/Laplace agents), everyone — including you — converged on
the same architecture: per-tile **state machine** (`VisibleLod{Mid,Exact}` +
`PendingLod{...}`), **generate early / commit late / display only when stable**, separate
**request radius vs display radius**, and a **`SafeToCommitVisibleChange()`** gate. What was
actually built: the ownership *verifier* (counters + mode 96), the CBV aliasing fix, and…
**another radius clamp** (`EXCLUDE_PUBLIC_EXACT`), shipped as default on the strength of
`protectedMidSurfaceFragments: 1,340,568 → 0` — which is §2.2 all over again. The state
machine, the pending/commit queues, and the radius split exist only in the transcript.

### 2.5 The root was named but never proven
"Exact extraction is budgeted/incremental so the ring has holes" was stated in the final
message and left as hypothesis. Nothing logs per-tile exact residency vs the advertised
radius. **You cannot contract mid until you can measure exact coverage.** Measurement must
precede policy — the campaign did policy first, sixteen times.

### 2.6 Even the radii disagree with each other
The mid exclusion keys on `cameraParams.surfaceRasterMaxDistance`
(`main_launcher.cpp:24638-24643`) while the exact pass's own reach can be independently clamped
(`high_alt_excluded` → `sparseExactNearDistance`, demotion, non-promotion). Two different
scalars, one annulus of guaranteed nothing whenever they disagree.

### 2.7 What the campaign got RIGHT (keep all of this)
- **Per-draw CBV slots** (`Renderer.cpp:1263-1290`, 64-slot ring, cursor reset in `BeginFrame`)
  — a real latent bug; exact and mid draws were stomping each other's constants, which had been
  corrupting the owner counters and at least one grey regression. Valid, keep.
- **Owner-split fragment counters + mode 96** (`RENDER_OWNER_EXACT/MID/PROTECTED_*`,
  `SparseVoxelGpuResources.cpp:1712-1793`) — this is the verifier the real fix needs. Keep.
- **`tileVisibleInCameraView()`** frustum test with vertical LOS bound (`SparseClipmap.cpp`) —
  this is the core of `SafeToCommitVisibleChange`. Keep.
- **`suppressVisibleDirtyUpload` (+ held-child catch-up machinery)** — the in-view commit hold
  for mid→mid churn, i.e. half of "commit late" already exists. Keep.
- **XZ raster clip, XZ-only streaming motion, alt-throttle 384, bounded_repair promotion** —
  churn reducers, keep.
- Diagnosis tooling: modes 94/95/96, `MIDMESH_HANDOFF` / `MIDMESH_WAVE_TRACE` logs,
  `rebrun_env.ps1`, the mid-underlay audit. Keep.

---

## 3. The real fix: a per-tile ownership contract

The question the engine must answer, per world tile, per frame (your words, and both review
agents agreed): **"Which representation is allowed to produce visible pixels here, this
frame?"** — answered by *readiness + hysteresis*, never by a scalar radius.

### 3.1 The contract

```
owner(tile) ∈ { Exact, Mid }            // near field; Far handled by existing midEnd boundary

owner(tile) = Exact  iff  ExactRenderReady(tile)                    // never Exact when not ready ⇒ no holes, by construction
                      AND ( owner_prev(tile) == Exact               // hysteresis: once exact, stay exact
                            OR SafeToCommitVisibleChange(tile) )    // commit late: flip only when unseen/subpixel
owner(tile) = Mid    otherwise                                      // mid is ALWAYS the fallback; never radius-excluded

ExactRenderReady(tile): every exact brick covering the tile footprint is
    resident in SparseSurfaceCache (m_knownBricks / m_facesByBrick),
    extracted, uploaded, and fence-complete on the GPU. Atomic per mid-tile
    footprint — partial exact coverage of a mid tile does NOT flip it
    (reuse the childHandoffMask atomic-2x2 pattern in SparseClipmap.cpp).

SafeToCommitVisibleChange(tile):
    !tileVisibleInCameraView(tile, padding)                          // out of frustum (already implemented)
    OR distanceXZ(tile) > farSafeSwapDistance                        // far enough that the swap is invisible
    OR projectedFaceSizeDelta(tile) < ~1 pixel                       // coarse→fine delta is subpixel at this range
    OR tile has a pending voxel EDIT (correctness beats stability)   // edits bypass the hold
    OR heldLongerThan(maxHoldSeconds) → commit via 2-4 frame dither  // bound memory/staleness; last resort
```

**Generate early:** exact *request/stream* radius = display radius × ~1.4. Exact tiles are
generated, extracted, and uploaded while mid still owns them (pending, invisible). By the time
the camera approaches, `ExactRenderReady` is already true and the flip happens out-of-view or
subpixel. This kills the "prepare exact only when the camera arrives" behavior that makes the
handoff visible.

**Demotion:** once Exact, a tile stays Exact while resident. Eviction under memory pressure
demotes out-of-view tiles first; an in-view demotion uses the same commit gate.

**The scalar radii become advisory only** — they bound *request budgets* (how far to stream
exact), not *display ownership*. `high_alt_excluded` shrinks the request radius; mid
automatically owns whatever exact doesn't, so a shrunken exact ring can never create holes or
an ownership dispute again.

### 3.2 Why this cannot reproduce either failure mode
- **No grey holes:** Mid is never excluded by geometry/radius. A pixel loses mid coverage only
  when its tile's owner is Exact, and owner can only be Exact when exact is *proven
  render-ready* there. The failure "mid removed + exact absent" is unrepresentable.
- **No in-view sharpening:** owner flips are gated by `SafeToCommitVisibleChange`. A tile you
  are looking at keeps its current representation until you look away / it's subpixel / a
  dithered forced commit. Mid→mid band changes and dirty re-uploads go through the same gate
  (the `suppressVisibleDirtyUpload` machinery already holds them; it gains the same catch-up
  commit path).

---

## 4. Implementation plan

### Phase 0 — Restore a sane baseline (30 min)
1. Flip `VENPOD_SPARSE_MID_MESH_EXCLUDE_PUBLIC_EXACT` default **1 → 0**
   (`main_launcher.cpp:3770`). This removes the known-holes configuration; the world is whole
   again (with sharpening — that's the honest red state).
2. Commit the dirty tree as **two commits**: (a) instrumentation + valid fixes (CBV slots,
   owner counters, mode 96, XZ clip, visibility suppression, audits); (b) the exclusion-clamp
   experiment, message stating it's superseded by this plan. Baseline = playable + measurable.

### Phase 1 — Measure before policy: the coverage proof (the missing step)
3. Add `ExactRenderReady(footprint)` query: `SparseSurfaceCache` exposes per-brick
   known/extracted state (`m_knownBricks`, `m_facesByBrick`); `SparseSurfaceGpuResources`
   exposes per-brick uploaded serial/fence completion. Compose in `main_launcher.cpp` (or a
   small `TerrainOwnership` module) at mid-tile granularity.
4. Log per frame: `EXACT_COVERAGE frame=N advertisedR=2560 tilesReady=X tilesTotal=Y
   ringComplete=X/Y nearestHole=D`. Run the default live scene + bob replay. **This number is
   the proof of §1 Layer 2** — expect ringComplete well below 1.0 and to *stay* below 1.0
   during motion. This is the red baseline every later phase must move.
5. While here, instrument owner flips: `OWNERSHIP_FLIP frame=N tile=... from=Mid to=Exact
   visible=0/1` — in-view flips are the sharpening events; count them per frame.

### Phase 2 — The ownership table
6. Build `ResolveTileOwners()` (new, called once per frame before snapshot/draw): produces the
   per-tile owner map per §3.1, holding previous-owner state + held-pending sets. Persist owner
   state per tile coord (same lifetime/keying as `m_midMeshEmittedCoords`).
7. Implement `SafeToCommitVisibleChange` from existing parts: `tileVisibleInCameraView()`
   (exists), XZ distance (exists), projected-size delta (`debugFaceExtent` math from
   `VS_SparseSurface.hlsl` ported to CPU — face world size / distance vs FOV), dwell timeout.
8. Split radii: add `VENPOD_SPARSE_EXACT_REQUEST_RADIUS_SCALE` (default ~1.4). Exact
   streaming/extraction/upload interest uses `displayRadius × scale`; display ownership uses
   only the table. `high_alt_excluded` and demotion clamp the *request* radius only.

### Phase 3 — Enforcement at the draw
9. **Mid pass:** stop using `surfaceRasterMinDistance` for exclusion (leave it 0). Exclude mid
   **per tile** where `owner==Exact`. Cheapest correct mechanism: the mid mesh already draws
   via per-brick indirect draw commands (`useMidMeshIndirect`, range allocator) — filter the
   draw-command list by owner, no re-extraction, no shader change, flips are one-frame atomic.
   (Fallback if the direct-GPU path complicates it: per-tile ownership bits in a small SRV,
   tested in `VS_SparseSurface.hlsl` like the existing clip — but prefer the CPU filter.)
10. **Exact pass:** unchanged — draws wherever resident; stencil arbitration (exact first,
    ref 1; mid `NOT_EQUAL`) is already correct post-CBV-fix. Optionally also filter exact draw
    commands to `owner==Exact` tiles for strictness (prevents exact overpainting a held mid
    tile before its commit — this is precisely the in-view sharpen), i.e. **the commit gate
    must bind the exact pass too**, not just mid removal.
11. **Mid→mid churn through the same gate:** dirty re-uploads and LOD band re-emissions commit
    only via `SafeToCommitVisibleChange`; held tiles catch up the frame they leave the frustum
    (extend `m_midMeshHeldChildCatchupCoords` pattern). Voxel-edit dirties bypass the hold.

### Phase 4 — Cleanup
12. Demote the global promotion boolean to a request-budget advisor; delete or repurpose the
    `EXCLUDE_PUBLIC_EXACT` env (superseded); keep `bounded_repair` for the repair lane.
13. Re-check `kSparseSurfaceConstantUploadSlots=64` headroom once draws are ownership-filtered.

## 5. Verification methodology (binding — this is what §2.2/2.3 lacked)

**Invariants (machine-checked every run, logged every 20 frames):**
- **A. No holes:** near-field pixels owned by exact∪mid ≈ 100%; background/far fragments inside
  the mid range ≈ 0 (`PERF_RENDER_OWNERSHIP` counters — already exist).
- **B. No in-view flips:** `OWNERSHIP_FLIP ... visible=1` count == 0 during pure camera motion
  (edits exempt). This is the sharpening wave, expressed as an integer.
- **C. Coverage converges:** `EXACT_COVERAGE ringComplete` reaches 1.0 within N seconds of
  camera rest, and display radius never exceeds proven coverage.

**Scenes (all three, every gate):** (1) default live-like run, no replay, low camera, WASD-style
motion; (2) `vertical_bob_repro.vnrd`; (3) a forward-walk replay (record once). Native-res
captures; compare consecutive matched-pose frames.

**Visual verifier:** **mode 96** (green=exact, blue=mid, magenta=violation) is primary.
**Mode 94 magenta==0 is explicitly NOT a success criterion** — write this into LOOPS.md.
Mode 94 remains useful only as "where does coarse mid own pixels," an observation, not a gate.

**Human gate:** your live screenshots remain the final arbiter; no loop closes without them.
Machine-green + human-unchecked = still red. (This exact rubber-stamp happened 4+ times.)

**Pitfalls (learned the hard way, do not repeat):**
- Never touch `PS_Raymarch.hlsl` for debug overlays — DXC compile-cliff, cache invalidation,
  minutes-long stalls; it wedged the session twice.
- Use `rebrun_env.ps1` for every test run (env leakage between tests poisoned early A/Bs).
- Shader-cache key changes force recompiles — batch shader edits.

## 6. Round-2 postmortem (2026-07-06, contract implemented but still fails live)

Codex implemented Phases 0–3 behind `VENPOD_TERRAIN_OWNERSHIP_CONTRACT=1`: coverage proof
(`EXACT_COVERAGE tilesReady=0/107` — hole theory confirmed), per-exact-brick ownership table
(`ResolveExactBrickOwnership`, `main_launcher.cpp:~1176`), exact draw-command filtering
(`BuildFallbackDrawArgsExcluding`), request-radius split (1.4×), mode-96 semantics. The
structure is right. It still sharpens live because of four deviations:

1. **The in-view hold is a countdown, not a gate.** `safeCommit = !visible || subpixel ||
   farSafe || timedOut` (`main_launcher.cpp:1270`) with `MAX_HOLD_FRAMES=45` +
   `DITHER_FRAMES=90` hash jitter (`:4646-4648`). At ~100 fps every visible ready brick
   force-commits 0.5–1.4 s after readiness, as an instant one-frame unfilter — no crossfade.
   The plan's "bounded timeout" was a *last resort* against unbounded staleness; as built it is
   the **common path**, so the wave became scattered sub-second pops ("little areas that do
   it"). `subpixel` (0.75 px) never saves a near brick; `farSafe` never saves a near brick;
   `!visible` rarely true at the crosshair. Net effect near the camera: timeout always decides.
2. **Timeout is the common path because readiness still arrives in-view.** "Generate early" was
   implemented as only a prefetch-radius bump (2560→3584); extraction/upload *throughput*
   budgets were untouched, so with `notKnown=259627` at spawn the pipeline saturates and bricks
   keep becoming ready after the camera already sees them. Generate-early requires
   budget/priority (order requests by camera-approach), not just radius.
3. **Mid→mid sharpening is outside the contract entirely.** Only *exact* draw commands are
   filtered. `computeTileLod` band sweeps (2200/5000), new mid tiles replacing coarser
   parents, and held-tile catch-ups still publish in view (plan §4 step 11 was skipped).
4. **Verification again could not see the bug.** Single-frame captures (240 / 660 / 840-880)
   cannot show a temporal pop; and Invariant B (visible commits == 0) was never enforced — the
   "green" logs literally printed `timedOut=3` per window, i.e. **the machine evidence recorded
   the in-view pops and they were accepted as pass**. `timedOut>0` while moving IS the bug.

**Round-2 fixes, in order:**
- Treat `timedOut>0` (and any visible non-subpixel commit) as a red invariant. Gate on a
  *moving* replay with consecutive-frame diffs / short video, never single frames.
- Make the visible hold indefinite by default (`MAX_HOLD_FRAMES=0` = no timeout); bound memory
  instead by capping the pending set and evicting out-of-view pendings first. If a forced
  visible commit must exist, it must be a real per-brick crossfade/dissolve over ~20-30 frames
  (stencil-dither or alpha ramp in the exact pass), not an instant unfilter.
- Raise/prioritize exact extraction+upload so readiness precedes visibility: budget envs +
  priority queue ordered by time-to-enter-view along camera velocity. Measure with
  `EXACT_COVERAGE` converging to ~1.0 *ahead* of the camera.
- Put mid→mid re-banding and new-tile publishes under the same SafeToCommit gate (plan §4
  step 11), with out-of-view catch-up.
- Keep: the contract structure, coverage audit, mode 96, draw-command filtering — all correct.

## 6.1 Round-3 (2026-07-06, Claude): holes root-caused + dissolve commit SHIPPED

The user's live contract-on run showed grey/garbled holes again. Root causes found and fixed:

1. **The hole mechanism was a leftover radius clamp in the background mask.**
   `PS_SparseSurfaceDepthPrepass.hlsl` discarded ALL mid-pass pixels inside the public exact
   radius from the background-pass foreground mask (a round-1 A15 relic). Under the contract,
   exact draws are brick-filtered, so those pixels were mid-colored on screen but absent from
   the mask — the half-res background pass composited over live mid → dark blocky holes +
   low-res garbled patches. **Fix: discard deleted.** The mask now reflects actual foreground.
   (Contract-off never showed it because exact itself marked those mask pixels.)
2. **GPU mid-mesh direct-draw was disabled under the contract** (`sparseMidMeshGpuDrawForThisPass`)
   for no reason (the mid ownership filter set is always empty) — GPU-extracted tiles without a
   landed CPU copy vanished. **Fix: re-enabled; mid is never filtered.**
3. **Timeout pop replaced with per-face dissolve.** `ResolveExactBrickOwnership` no longer has
   any path that instantly commits a visible brick. Visible ready bricks hold ~30 frames
   (hash-jittered ±45) then fade in over `VENPOD_TERRAIN_OWNERSHIP_DISSOLVE_FRAMES=60` frames
   by drawing a growing prefix of the brick's faces (fraction map → scaled
   `indexCountPerInstance` in `BuildFallbackDrawArgsExcluding`). Out-of-view/subpixel/far
   commits stay instant. Bricks that were exact within the last 120 frames re-commit instantly
   (edit/dirty-flap protection, `VENPOD_TERRAIN_OWNERSHIP_RECOMMIT_WINDOW_FRAMES`).

**Machine verification (all with contract on unless noted):**
- Default scene frame 240: near field fully covered, no holes (was: user-screenshot voids).
- Wave replay frames 840-880 A/B vs contract-off: identical coverage; the small black patches
  center-frame are PRE-EXISTING in stock (separate follow-up — they are the raw exact-data
  hole that started this hunt, visible without the contract too).
- Stationary-scene consecutive-frame diffs (10 captures, 8-frame spacing): contract-on max
  16x16-block delta 16.1, zero blocks >30 — same envelope as stock (16.3, zero). No pops.
- Counters: `dissolving` 30-90 steady, `instantRecommit=0`, no `FILTER_FAILED`, no CBV
  slot exhaustion. Mode 96: foreground green, held-mid blue, zero magenta.
- Human live gate: pending (`.\rebrun_env.ps1 -NoBuild -Env VENPOD_TERRAIN_OWNERSHIP_CONTRACT=1`).

**Still open after human pass:** flip contract default on; the pre-existing black patches;
watch for mid→mid band churn in live play (plan §4 step 11 remains unimplemented).

## 6.2 Round-4 (2026-07-06, Claude): the black no-owner voids — root cause + VIEW COVERAGE REPAIR

User confirmed streaming is good but "patches of mid don't generate" (black voids, present in
stock too). Root-caused via elimination (owner overlays 71/58, water-discard probe mode 97, CPU
void probe):

- **Mechanism:** the voids are pixels NO layer may paint. (1) The exact surface cache had
  **never extracted** the bricks (`VOID_PROBE`: 84/327 camera-visible terrain bricks
  `notSurfaceKnown` — basin floors/walls outside the generation interest); (2) the mid mesh
  has no face there; (3) the background raymarch DID find terrain but
  `BackgroundHitAllowedByExactNear` (PS_Raymarch:4131) **rejects hits inside the ownership
  radius** — with the quality preset's background surface-fill stripped
  (`Renderer.cpp:828-830` clears near-flag bit 8), every layer falls to the hard radius
  rejection → `BACKGROUND_LAYER_NONE` → black. Fourth instance of the
  radius-as-coverage-guarantee fallacy.
- **The user's instinct was right:** not compute-bound (104 FPS, upload ring 1%); these bricks
  were invisible to every pixel-driven repair loop because nothing draws them → nothing
  samples them → nothing requests them.
- **Fix shipped — VIEW COVERAGE REPAIR** (`main_launcher.cpp`, next to the parent-held
  feedback): every 10 frames, march a 24x14 grid of camera rays over the CPU heightfield;
  any hit brick that is `notSurfaceKnown` queues a Visible-class request on the Cache lane
  (plus the visible **wall span** of bricks between ray altitude and heightfield top — cliff
  walls were the residual), budget 48/interval, 300-frame cooldown, `VIEW_COVERAGE_REPAIR`
  log. Envs: `VENPOD_SPARSE_VIEW_COVERAGE_REPAIR` (default 1), `..._INTERVAL`,
  `..._MAX_REQUESTS`, `VENPOD_DEBUG_VOID_PROBE` (verbose). Also: rebrun.ps1 now honors a
  pre-set `VENPOD_RAYMARCH_BACKGROUND_PASS_SURFACE_FILL` override; mode 97 added
  (cyan-tint what the submerged-water discard removes — that hypothesis was ELIMINATED).
- **Verified (wave replay, worst frames 840-880):** `notKnown` converges to 0 at rest in ~2
  intervals; the mid-distance band that was a dark half-empty smear is now fully terraced
  terrain; the multi-lobe voids collapsed to **one single brick-column strip** (stable,
  pre-existing; its brick reports surface-ready yet paints nothing — different micro-cause,
  see follow-ups). Default scene regression-free.
- **Follow-ups:** (a) the one residual strip — brick claims Ready but no visible face; chase
  with per-face extraction audit; (b) consider extending `BackgroundHitAllowedByExactNear`'s
  fill branch to FAR_HEIGHT (needs PS_Raymarch edit — compile-cliff, NVIDIA JIT fragility
  documented in-shader; only attempt with byte-identical-restore fallback); (c) generation
  interest should learn from the probe (why were basin bricks outside interest at all).

## 7. Order of work, first session
1. Phase 0 (baseline + commits) → 2. Phase 1 (coverage proof; STOP and look at the numbers —
   they should confirm §1 Layer 2; if ringComplete is already ~1.0 at rest, the hole theory
   needs revisiting before building Phase 2) → 3. Phases 2+3 behind one env flag
   (`VENPOD_TERRAIN_OWNERSHIP_CONTRACT=1`) → 4. run the §5 battery, flip default only after the
   human gate passes on all three scenes.
