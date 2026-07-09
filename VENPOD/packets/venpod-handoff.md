# Learning Dossier — VENPOD near→mid raster handoff

Target depth: **level 3** (debug or modify it safely) for the whole subject; the spine packet
(P6) targets **level 5** (teach it) because it is the transferable idea.

How to use: read top to bottom once now. Then do the **Review** schedule cold — no scrolling
back. The packets are ordered by leverage; if you only keep one, keep P6.

Anchors are real: file:line, log rows, and screenshots from the 4-day session.

---

## P1 — "Sharpening" was never regeneration. It was a visible ownership handoff.

Why this matters:
Two days were lost because the bug was mis-named. You (and Codex) kept hunting a "redraw loop"
that did not exist. Naming the symptom correctly is what unlocked the whole fix.

Before -> After:
- Before: terrain near me is being *regenerated* — thrown away and rebuilt — as I move.
- After: nothing regenerates. A **coarse mid-raster height grid** owns the pixels near me, and
  when exact voxel data arrives it **overpaints** the coarse grid. The eye reads that
  coarse→fine swap, and the coarse grid's own LOD bands shifting under camera motion, as
  "sharpening." It is a handoff artifact, not a rebuild.

Mechanism:
Three things change those pixels in view: (a) exact tiles becoming resident and overdrawing
mid; (b) `computeTileLod`'s distance bands (thresholds 2200 / 5000) sweeping across ground as
you move, changing the mid face size; (c) dirty mid tiles re-uploading while visible.

Concrete anchor:
Debug mode 94 (`PS_SparseSurface.hlsl`, gate `debugMode==94 && surfaceDebugPassKind==2 &&
debugFaceExtent>=4`) magenta-tints exactly the coarse mid faces. Magenta sitting right on the
"sharpening" grid = proof the visible layer is mid raster, not a redraw.

Failure mode if missing:
You bisect "redraw" call sites forever (the MIDMESH_WAVE_TRACE showed **zero** re-extracts at
the bad frames — there was nothing to bisect).

Check yourself:
1. What are the three distinct ways a mid pixel can visibly change without any regeneration?
2. If mode 94 shows magenta on the sharpening band, what has that ruled out?
3. A different engine shows terrain "popping" on movement. What's the first thing you'd probe
   before assuming a rebuild loop?

Teachback: In 90s, explain to a teammate why "regeneration wave" was the wrong name, using the
mode-94 magenta screenshot.

Review: Now, +1d, +3d, +7d, +21d.

---

## P2 — Ready ≠ allowed to own pixels. The 2560 radius is a *draw intention*, not coverage.

Why this matters:
This is THE root cause, and the one Codex named only in the session's final minutes. Every
failed fix ignored it.

Before -> After:
- Before: `surfaceRasterMaxDistance = 2560` means exact terrain covers everything within 2560.
- After: it means exact is *allowed to draw* out to 2560 **if its data exists there**. Exact
  extraction/upload is incremental and budgeted, and at altitude `high_alt_excluded` clamps it
  to 1024 — so the exact ring is *always* partly empty, and mid silently fills the holes. That
  is *why* mid draws so close to the camera in the first place.

Mechanism:
Mid's near-exclusion keys on `cameraParams.surfaceRasterMaxDistance`
(`main_launcher.cpp:24638-24643`) while exact's actual reach is independently clamped
(`high_alt_excluded`, `main_launcher.cpp:23701-23705`). Two different scalars → an annulus that
exact is "responsible for" but does not actually cover.

Concrete anchor:
The coverage probe: `EXACT_COVERAGE ... tilesReady=0/107` at spawn — the advertised ring was
literally 0% real. Later, `VOID_PROBE` frame 860: 84/327 visible bricks `notSurfaceKnown`.

Failure mode if missing:
You "fix" the wave by pushing mid past a radius → grey holes, because exact can't backfill.
(This is P3.)

Check yourself:
1. Why can exact's advertised radius and its actual covered radius disagree?
2. What single log line would you print to *prove* the ring is incomplete before touching any
   policy?
3. If you shrink the mid-exclusion radius to kill sharpening, what new artifact appears and
   why?

Teachback: Explain "draw intention vs coverage guarantee" using the `tilesReady=0/107` log.

Review: Now, +1d, +3d, +7d, +21d.

---

## P3 — A scalar radius cannot arbitrate a per-tile reality. That mismatch IS the bug.

Why this matters:
~16 fix attempts were all secretly the *same* fix ("forbid mid within radius R"), walking a
Pareto frontier between two failures. Recognizing they were isomorphic is what stopped the
thrash.

Before -> After:
- Before: the right knob is the distance where mid stops and exact starts; tune it.
- After: there is no correct scalar. Coverage is **per-tile and incomplete**, so any radius is
  either too small (sharpening still visible) or too big (grey holes where exact isn't ready).
  You cannot tune your way out; you must make ownership **per-tile and readiness-gated**.

Mechanism:
Exclude-mid-by-radius R with exact coverage that is ragged per tile: pixels where R says "exact
owns" but exact has no resident tile fall through to background → grey. Pixels where R is small
enough to avoid that still show the coarse mid → exact swap.

Concrete anchor:
The failure table in `plan.md` §2.1 — near-exclusion 1024/512/256, stencil KEEP, prepass
discard, draw-order swap, `EXCLUDE_PUBLIC_EXACT=1` — every row lands on "grey hole" or
"residual grid."

Failure mode if missing:
You keep proposing a new radius/clamp and re-walk the same frontier.

Check yourself:
1. Give the two-failure dichotomy a scalar radius is trapped between.
2. Why does "mid is the *unconditional* fallback; exact is gated per tile" make grey holes
   *unrepresentable* rather than just unlikely?
3. Name another system where a global threshold is standing in for per-item state.

Teachback: Explain why the 16 attempts were one attempt, using the failure table.

Review: Now, +1d, +3d, +7d, +21d.

---

## P4 — "Magenta == 0" proved mid didn't draw, not that exact drew. Verifier inversion.

Why this matters:
Codex declared victory 4+ times on this metric and was falsified by your screenshot each time.
It is the single most expensive verification mistake in the log.

Before -> After:
- Before: mode 94 magenta gone → the sharpening is fixed.
- After: mode 94 tints *mid* faces. Zero magenta only proves **mid did not draw**. Deleting mid
  entirely trivially passes it — and produces grey holes. A one-sided metric rewards the wrong
  behavior.

Mechanism:
The correct success condition is two-sided: exact ∪ mid must tile the near field (no holes)
AND no owner flips in view (no sharpening). That needs the owner overlay (mode 96) + a
hole/flip *count*, not the absence of one color.

Concrete anchor:
The `timedOut=3` counter in the "passing" logs — the machine literally recorded the in-view
pops it was calling green. And single-frame captures (240/660/860) cannot show a temporal pop
at all.

Failure mode if missing:
You optimize a proxy to zero and ship a regression that any moving, full-scene capture would
have caught.

Check yourself:
1. Why does "magenta == 0" have a trivial degenerate solution? What is it?
2. What two-sided condition actually defines "fixed" here?
3. Why is a single still frame structurally blind to this bug?

Teachback: Explain the verifier inversion using the `timedOut=3`-as-green example.

Review: Now, +1d, +3d, +7d, +21d.

---

## P5 — Commit-late means a visible upgrade must *dissolve*, never pop. No one-frame path.

Why this matters:
The ownership contract's *structure* was right but still visibly popped, because a "hold N
frames then swap" is still a pop — just a delayed one.

Before -> After:
- Before: hold the exact upgrade for a while, then commit when it's "safe."
- After: for a **visible** brick there must be *no code path that commits in a single frame*. It
  either commits while unseen/subpixel/far, or it **fades in face-by-face** over ~60 frames.
  The old `timedOut` branch was a delayed pop and had to be deleted as a red invariant.

Mechanism:
`ResolveExactBrickOwnership` (~`main_launcher.cpp:1176`): the commit predicate is
`safeCommit = !visible || subpixel || farSafe || recentExactFlap` — note **no `timedOut`**. A
visible ready brick instead emits a growing face-prefix (`drawFraction` → scaled
`indexCountPerInstance` in `BuildFallbackDrawArgsExcluding`) so exact bleeds in gradually.

Concrete anchor:
Consecutive-frame 16×16 block delta: contract-on **16.1** vs stock **16.3**, zero blocks over
threshold. The dissolve is invisible per frame. Counter `instantVisibleCommits`/`instantRecommit`
held at 0.

Failure mode if missing:
"It holds then snaps" — scattered sub-second pops (your "little areas that do it").

Check yourself:
1. Why is "hold 45 frames, then commit" still the sharpening bug?
2. What are the only conditions under which a visible brick may commit in one frame, and why
   are they safe?
3. How would you *measure* that no pop remains, without eyeballing?

Teachback: Explain why a timeout is a delayed pop, using the block-delta 16.1-vs-16.3 result.

Review: Now, +1d, +3d, +7d, +21d.

---

## P6 (capstone) — A pixel with no owner is always a bug. Pixel-driven repair can't heal an undrawn pixel.

Why this matters:
The black voids looked like a streaming budget limit. They were not — they were a *coverage
gap no feedback loop could see*. This is the transferable insight and the spine of the whole
saga.

Before -> After:
- Before: black patches = the engine can't keep up (compute/budget).
- After: at 104 FPS with the upload ring at 1%, nothing is budget-starved — so **every hole is
  a bug**. The voids were terrain bricks the surface cache **never extracted** (basins/cliff
  walls outside generation interest), which were *invisible to every repair loop* because all
  repair is driven by samples from **drawn** pixels — and nothing draws a void, so nothing
  samples it, so nothing requests it. A blind spot, not a backlog.

Mechanism:
Three gaps conspire: exact never extracted there + mid has no face there + the background
raymarch *finds* the terrain but `BackgroundHitAllowedByExactNear` (`PS_Raymarch.hlsl:4131`)
**rejects** any hit inside the ownership radius (and the quality preset strips the fill flag,
`Renderer.cpp:828`). No layer may paint → `BACKGROUND_LAYER_NONE` → black. Fix: a **view-driven**
probe (march heightfield rays every 10 frames, queue `notSurfaceKnown` hit bricks + their cliff
wall span) — the only repair that can see undrawn terrain.

Concrete anchor:
`VOID_PROBE frame=860 ... notKnown=84/327`; after VIEW_COVERAGE_REPAIR, `notKnown` converges to
0 within ~40 frames; the field of patches in your screenshot collapsed to one brick-column.

The unifying thread (say this out loud): the same fallacy appeared **four times** —
*ready ≠ allowed-to-own ≠ actually-covering*. Exact being ready didn't mean it owned (P2). A
radius saying "exact owns" didn't mean exact covered (P3). Magenta-absent didn't mean
exact-present (P4). And a raymarch hit being valid didn't mean it was *allowed* to paint (P6).
Every bug in this saga is one confusion between **permission, readiness, and actual coverage**.

Failure mode if missing:
You "solve" holes by throwing more budget at a system that was never budget-limited, or by
another radius clamp.

Check yourself:
1. Why is a feedback loop keyed on drawn-pixel samples *structurally* unable to heal a void?
2. What one observation (two numbers) proves a hole is a bug and not a budget limit?
3. State the four-times fallacy in one sentence and map each instance to its packet.
4. (Transfer) Where else does "the thing that reported success only proves a *proxy* succeeded"
   bite — name a non-graphics example.

Teachback: Teach the whole saga in 3 minutes as one idea (permission ≠ readiness ≠ coverage),
using one anchor per instance. This is the level-5 bar.

Review: Now, +1d, +3d, +7d, +21d, and re-teach at +21d to a rubber duck from memory.

---

## Interleave these (don't review in a block — discriminate)

- Draw intention (radius) **vs** actual coverage (per-tile residency)   [P2 vs P3]
- Readiness (data exists) **vs** permission to display (commit gate)     [P2 vs P5]
- Depth-prepass/mask pixel **vs** color-pass pixel                       [why the prepass discard made holes]
- Delayed commit (timeout) **vs** gradual commit (dissolve)              [P5]
- Budget limit **vs** coverage blind spot                               [P6]

## Parked (Later — low leverage, don't packet unless they block you)
- Per-draw CBV 64-slot ring (real fix, but mechanical; know it *exists* so counters are trustworthy)
- XZ vs 3D clip metric details
- The exact spdlog format strings
- Why `PS_Raymarch.hlsl` edits risk the NVIDIA JIT compile-cliff (know the *rule*: don't touch it casually)

## One residual (honest open thread)
One brick-column strip in the wave replay still voids while reporting `Ready`. Different
micro-cause (missing extracted face on a wall). Tracked in `plan.md` §6.2. Good candidate for
your first solo debug using the tools now in place (`VOID_PROBE`, mode 96).
