# Learning Dossier — VENPOD near→mid raster handoff

The spine of the whole campaign, one sentence:

> **A representation having correct data ready is NOT the same as that representation being
> allowed to produce visible pixels this frame — and any system that conflates the two, or
> swaps ownership instantly while you're looking, produces artifacts.**

Everything below is a facet of that sentence. Target depth for the Now packets: **level 3**
(you can debug/modify safely). Next packets: level 2. Later: parked.

---

## Queue

**Now (must hold to continue safely)**
1. Readiness ≠ permission to display
2. A scalar radius cannot arbitrate per-tile coverage
3. A visible representation swap must be spread over time, never instant

**Next (high leverage, review soon)**
4. Undrawn pixels are invisible to pixel-driven repair
5. A debug metric hitting zero can mean "fixed" OR "layer deleted" — metrics must be two-sided
6. Symptom identity is not stable — name the mechanism, not the appearance

**Later (parked implementation detail)**
- Per-draw CBV slot ring (constant-buffer aliasing fix)
- Stencil ref values / exact-first draw order
- Exact env-knob catalog (promotion policy, request-radius scale, dither frames)

---

## Packet 1 — Readiness ≠ permission to display

Why this matters:
This is the root confusion that cost two days. Exact terrain data can be fully generated,
extracted, and GPU-resident, and STILL it must not necessarily draw this frame.

Before -> After:
- Before: "If the exact surface for a tile exists, draw it. That's the whole handoff."
- After: "Existence gates *eligibility*. A separate *commit* decision (is it safe to swap what
  the player sees right now?) gates *display*. Generate early, commit late."

Mechanism:
Two independent predicates per tile.
`ExactRenderReady(brick)` = every covering brick is surface-known + extracted + uploaded +
fence-complete (`CheckExactCoverageBrickReady`, main_launcher.cpp). `SafeToCommitVisibleChange`
= not-visible OR sub-pixel OR far OR recently-exact. A tile becomes exact-owned only when BOTH
are true; otherwise mid keeps owning it (as a held fallback).

Concrete anchor:
`ResolveExactBrickOwnership` in main_launcher.cpp — the `nextExact` decision:
`else if (exactReady) { if (safeCommit) nextExact = true; else <hold/dissolve> }`. Exact-ready
alone never flips a visible brick.

Failure mode:
Without the split, exact "arrives" mid-frame and overpaints coarse mid terrain the instant its
data lands — in the player's line of sight. That IS the sharpening wave.

Check yourself:
1. State the two predicates and what each gates.
2. A brick is exact-ready and dead-center in your view, camera moving. Does it draw as exact
   this frame? Why/why not?
3. Where else in graphics does "ready but not yet shown" appear? (Hint: texture streaming,
   async shader compile / PSO warm-up.)

Teachback:
90 seconds, using the `ResolveExactBrickOwnership` branch, on why exact data existing doesn't
mean exact draws.

Review: Now, +1d, +3d, +7d, +21d. Target level 3.

---

## Packet 2 — A scalar radius cannot arbitrate per-tile coverage

Why this matters:
Codex tried ~16 fixes over two days. Every one was a variant of "forbid mid within radius R."
Every one failed the same way, for a structural reason worth internalizing.

Before -> After:
- Before: "Push mid out to distance R and let exact own everything inside R."
- After: "`surfaceRasterMax` (e.g. 2560) is a DRAW INTENTION, not a coverage guarantee. Exact
  extraction is incremental, so inside R coverage is patchy and per-tile. A single scalar can't
  describe a patchy set — so any radius is either too small (sharpening still shows) or too big
  (grey holes where exact isn't ready)."

Mechanism:
Mid exclusion was a distance test in three places (CPU min-distance, VS clip, prepass discard).
Exact residency is per-brick and lags the advertised radius. Scalar-vs-set mismatch is the bug;
the two failure images are the two ends of the same impossible tradeoff.

Concrete anchor:
The whole failure table in plan.md §2.1 (near-exclusion 1024/512/256, stencil KEEP, prepass
discard, draw-order swap, `EXCLUDE_PUBLIC_EXACT=1`). And the coverage proof `VOID_PROBE` /
`EXACT_COVERAGE` showing `tilesReady=0/107` at spawn — the ring was never full.

Failure mode:
"Magenta gone" screenshots that are actually grey-hole screenshots. You trade a visible wave
for visible voids and call it progress.

Check yourself:
1. Why must a scalar-radius fix land on exactly one of {sharpening, holes} and never neither?
2. If you HAD to keep a radius, what would you have to measure first to make it safe? (Answer:
   proven-coverage radius; shrink display radius to it.)
3. Name another system where "advertised extent ≠ actually-filled extent" bites you.

Teachback:
Explain the two-ended Pareto trap using the failure table, in 90 seconds.

Review: Now, +1d, +3d, +7d, +21d. Target level 3.

---

## Packet 3 — A visible representation swap must be spread over time, never instant

Why this matters:
The first "real fix" implemented the per-tile contract correctly but STILL sharpened live,
because of one setting. This packet is that setting.

Before -> After:
- Before: "Hold the visible brick until a timeout, then commit it." (`safeCommit = ... ||
  timedOut`)
- After: "There is NO code path that commits a visible brick in one frame. A visible ready
  brick either dissolves in face-by-face over ~60 frames or waits. `timedOut>0` while moving is
  itself the bug, not a success."

Mechanism:
The old rule force-flipped every visible ready brick 0.5–1.4s after readiness as an instant
unfilter — the wave became scattered sub-second pops. The dissolve replaces the pop: hold ~30
frames (hash-jittered), then grow a per-brick face prefix each frame
(`indexCountPerInstance = drawFaces*6`) via `BuildFallbackDrawArgsExcluding`'s fraction map, so
detail fades in. Out-of-view / sub-pixel / far commits stay instant.

Concrete anchor:
`ResolveExactBrickOwnership` dissolve branch + the fraction-scaled draw args in
`SparseSurfaceGpuResources::BuildFallbackDrawArgsExcluding`. Verification: stationary
consecutive-frame max block-delta 16.1 (contract on) vs 16.3 (stock) — invisible.

Failure mode:
An instant swap is a step function in the image → the eye reads any step as a pop, no matter
how "correct" the new pixels are.

Check yourself:
1. Why is an instant swap visible even when the new representation is strictly better?
2. Which commits are allowed to stay instant, and why are they safe?
3. Consecutive-frame block-delta ~= stock proves what, exactly? What would a pop look like in
   that metric?

Teachback:
Explain why "correct but instant" loses to "gradual" for visible LOD swaps.

Review: Now, +1d, +3d, +7d, +21d. Target level 3.

---

## Packet 4 — Undrawn pixels are invisible to pixel-driven repair

Why this matters:
The black voids couldn't be healed by any existing repair loop. Understanding *why* is the
deepest transferable idea in the campaign.

Before -> After:
- Before: "The renderer has feedback loops that detect and repair missing terrain, so gaps
  self-heal."
- After: "Every feedback loop samples from *drawn* pixels (shader unsafe-miss, parent-held).
  A void draws nothing, so nothing samples it, so nothing requests it. A hole that no layer
  paints is structurally invisible to draw-driven repair — it needs a *view-driven* probe."

Mechanism:
The voids were bricks the exact cache had never extracted (`notSurfaceKnown`), mid had no face,
and the background raymarch's hit was rejected inside the ownership radius
(`BackgroundHitAllowedByExactNear`). No layer writes → no feedback sample → permanent. Fix:
march heightfield rays from the camera every 10 frames and queue any `notSurfaceKnown` hit
(plus its cliff wall span) for generation — the ONE loop that doesn't depend on a pixel being
drawn first.

Concrete anchor:
VIEW COVERAGE REPAIR in main_launcher.cpp (`VENPOD_SPARSE_VIEW_COVERAGE_REPAIR`,
`VIEW_COVERAGE_REPAIR` log). Probe found 84/327 visible bricks `notSurfaceKnown`.

Failure mode:
You add more pixel-driven repair and it never touches the void, because the void has no pixel
to drive it.

Check yourself:
1. Why can't the shader unsafe-miss feedback ever repair a true void?
2. What's the minimum property a repair loop needs to reach a no-owner hole?
3. Transfer: a dashboard shows "0 errors" but a whole service never emits logs. Same bug class —
   name it.

Teachback:
Explain feedback-blindness of undrawn pixels using the 84/327 probe result.

Review: Now, +1d, +3d, +7d, +21d. Target level 2→3.

---

## Packet 5 — A metric hitting zero can mean "fixed" OR "layer deleted"

Why this matters:
"Mode 94 magenta == 0" was declared a fix at least four times and falsified by the next
screenshot each time. This is a verification-design lesson, not a rendering one.

Before -> After:
- Before: "The debug overlay shows the artifact. Artifact gone from overlay = fixed."
- After: "Mode 94 tints *mid drawing in the exact zone*. Zero can mean 'exact took over'
  (good) OR 'mid was deleted and nothing replaced it' (grey holes). A one-sided metric can't
  tell success from a worse failure. Success metrics must be two-sided: exact∪mid tiles the
  field AND no in-view flips."

Mechanism:
Optimizing to `magenta==0` rewards deleting the layer. The correct gate pairs a positive
(coverage/owner overlay mode 96 is green where exact owns, blue where mid holds) with a
negative (no `FILTER_FAILED`, no in-view instant commits) — and validates on a *moving* scene
with consecutive-frame diffs, not a single frame that can't show a temporal pop.

Concrete anchor:
plan.md §2.2/§2.3. The "green" logs that printed `timedOut=3` — the counter WAS the pop, logged
and accepted as pass. Verification battery: default + wave + bob, A/B vs contract-off.

Failure mode:
Goodhart. You hill-climb the proxy and ship a regression that any two-sided check would have
caught.

Check yourself:
1. Give the two readings of "mode 94 magenta = 0" and how to disambiguate them.
2. Why is a single-frame capture structurally unable to catch the sharpening wave?
3. Design a two-sided pass condition for a cache: what's the positive and the negative half?

Teachback:
Explain why one-sided metrics reward deletion, using the magenta=0 story.

Review: +1d, +3d, +7d, +21d. Target level 2→3.

---

## Packet 6 — Symptom identity is not stable; name the mechanism, not the appearance

Why this matters:
The bug wore three names — "shimmer," "regeneration wave," "sharpening wave" — and each rename
changed what got fixed. Two days went to chasing appearances.

Before -> After:
- Before: "It looks like terrain regenerating, so hunt for a redraw/rebuild loop."
- After: "The appearance is the eye's interpretation of a mechanism. 'Regeneration' was
  actually coarse mid faces being overpainted by finer exact faces at the moment data lands —
  no rebuild loop existed. Diagnose to the mechanism (who owns these pixels and why did it
  change?) before naming the fix."

Mechanism:
Wave-trace showed no late re-extracts at the artifact frames — killing the "redraw loop"
hypothesis. Owner overlays localized it to the mid-vs-exact pass boundary. Only then did the
right question appear: "which representation is allowed to produce visible pixels here?"

Concrete anchor:
The identity drift in the rollout (shimmer fixed early → "regeneration" → user's own reframe to
"sharpening wave that collapses bigger cubes into smaller voxels"). `MIDMESH_WAVE_TRACE`
showing hot frames only in the settle window, none at the artifact frames.

Failure mode:
You fix the thing the name implies (a rebuild loop) and the artifact stays, because the name
was a guess about mechanism.

Check yourself:
1. What evidence killed the "redraw loop" reading?
2. Restate the bug as an ownership question in one sentence.
3. Next time a bug "looks like X," what's the first thing to confirm before fixing X?

Teachback:
Tell the three-name story and the moment the mechanism replaced the appearance.

Review: +1d, +3d, +7d, +21d. Target level 2.

---

## Interleaving — discriminate these near-neighbors (don't let them blur)

- Readiness (data exists) vs Permission (safe to display) — Packet 1
- Generation/request radius vs Display/ownership radius — Packet 2
- Instant commit vs Dissolve commit — Packet 3
- Draw-driven feedback vs View-driven probe — Packet 4
- Overlay-shows-artifact (one-sided) vs Coverage-and-flips (two-sided) — Packet 5
- Appearance (sharpening) vs Mechanism (in-view ownership flip) — Packet 6

At +7d, shuffle these and explain each pair's difference cold. If two blur together, re-read
only that packet.

## Review log
- T0 (this session): teachback packets 1–3 out loud.
- +1d: answer all Check-yourself #1s cold.
- +3d: diagnose a near-neighbor (below).
- +7d: teachback packets 4–6 from memory + interleave discrimination.
- +21d: transfer to a different subsystem (below).

Near-neighbor diagnosis drill (+3d): "After enabling the contract, a hilltop 300m away flickers
between coarse and fine every few seconds while you stand still." Which packet's mechanism is
implicated, and what one env knob would you check first?

Transfer drill (+21d): apply the readiness-vs-permission split to async shader/PSO warm-up in a
different renderer, or to texture mip streaming. Where's the "commit late" gate there?
