# Deep Packet — The Two Holes (suppressed vs absent)

Target depth: level 3 (diagnose which hole you have and pick the right fix).
Prerequisite this packet installs first: **how a pixel gets an owner.** You can't tell the two
holes apart until you hold that model.

---

## Prerequisite — the painter stack + the foreground mask

Think of every screen pixel as a wall, and a stack of painters lining up to paint it,
finest-detail first:

```
1. exact surface raster   (finest, near)
2. mid raster             (coarse fallback)
3. background raymarch     (far / procedural, runs at HALF resolution for speed)
4. sky
```

A pixel is **black/grey when NO painter paints it.** That's the only way to get a hole. So every
hole question reduces to: *for this pixel, why did all four painters decline?*

Now the one perf detail that makes Hole A possible — the **foreground mask**:

The background raymarch (painter 3) is expensive, so it runs at half res. To stop it wasting
work (and scribbling) on near terrain that painters 1–2 already own, the engine builds a
**foreground mask**: "these pixels belong to the near passes — background, skip them."

That mask is built from the **depth prepass**. The rule the engine trusts:

> "If a near pass wrote depth for this pixel, it's foreground → background must NOT paint here."

Hold that sentence. Hole A is what happens when that rule gets a lie fed into it.

---

## Hole A — SUPPRESSED (Round 3: grey / "censored" world)

One-line model: **the data exists and a painter drew it, but a stale gate told the mask "this
isn't foreground," so the half-res background scribbled over the real pixel.**

The bug, exact line — `PS_SparseSurfaceDepthPrepass.hlsl`:

```hlsl
if (surfaceDebugPassKind == 2u &&                       // the MID pass
    UnderlayPixelIsInsidePublicExactSurface(worldPos))  // inside exact radius
    discard;                                             // <-- drop from the PREPASS
```

Walk the causality:
1. Mid has real faces here. In the **color pass**, mid paints the pixel correctly.
2. But in the **depth prepass**, that discard drops the mid pixel — so it's absent from the
   foreground mask.
3. The mask now lies: "no near pass owns this pixel → it's background."
4. The half-res background pass paints its coarse low-res guess **over** the correct mid color.
5. You see grey / blocky / "censored" terrain — NOT pure black, because something (the wrong
   thing) IS being painted there.

Why did turning the contract ON reveal this? Latent-bug unmasking:
- **Contract off:** exact draws EVERYWHERE inside the radius, unconditionally. Exact writes the
  prepass → the mask is correct → the mid discard was harmless (exact covered those pixels).
- **Contract on:** exact only draws READY bricks. A held-mid brick now depends on **mid** to
  write the mask — but the discard removes mid from the mask → hole. The contract didn't create
  the bug; it changed who's responsible for the mask and exposed a discard that was always
  wrong.

Fix: delete the discard. (Plus re-enable GPU mid-direct-draw under the contract, which had been
needlessly gated off.) Data existed the whole time — the fix is to **stop hiding it**.

Diagnostic fingerprint: **grey/low-res garbled** (background is painting), and a coverage probe
says the brick is **READY**. The data is present.

---

## Hole B — ABSENT (Round 4: black patches in basins, present even contract-off)

One-line model: **no painter has any paint for this spot — exact was never generated, mid was
never generated, and background is forbidden by the radius — so literally nothing paints it.**

The three-part conspiracy (all three must hold):
1. **Exact never extracted.** The brick is `notSurfaceKnown` — the surface cache never built it.
   The probe found **84 of 327** camera-visible bricks in this state (basin floors/walls that
   fell outside the generation interest).
2. **Mid has no face there either.** Mid can't fall back because mid was never generated there
   either.
3. **Background hit rejected by the radius.** The raymarch DID find the terrain, but
   `BackgroundHitAllowedByExactNear` (`PS_Raymarch.hlsl:4131`) rejects any background hit inside
   the ownership radius (and the quality preset strips the surface-fill flag that would have
   allowed it — `Renderer.cpp:828`). Fourth appearance of the radius-fallacy.

Result: painters 1, 2, 3 all decline → `BACKGROUND_LAYER_NONE` → **black** (no owner, dark clear
color — not a low-res guess like Hole A, because nothing is painting at all).

Why no existing repair loop could heal it — this is the deep part:
Every repair loop **samples from a drawn pixel** (shader unsafe-miss reads what the surface pass
wrote; parent-held reads what got drawn). A void **draws nothing → emits no sample → makes no
request.** It is structurally invisible to draw-driven repair. You could add ten more
pixel-driven repair loops and none would ever touch it.

Fix: VIEW COVERAGE REPAIR — the ONE loop that doesn't need a drawn pixel. It marches heightfield
rays from the camera every 10 frames and, for any hit brick that is `notSurfaceKnown`, requests
generation (plus the cliff-wall span above it). View-driven, not draw-driven. Data was absent —
the fix is to **go produce it**.

Diagnostic fingerprint: **black/dark** (nothing painting), and a coverage probe says the brick
is **notSurfaceKnown**. The data is absent.

---

## The diagnostic key (memorize this, it's the whole packet)

Point one probe at the hole and ask: **does the data exist right now?**

```
                    SUPPRESSED (Hole A)          ABSENT (Hole B)
data exists?        YES (brick READY)            NO (notSurfaceKnown)
who's painting?     background, over real mid    nobody
looks like          grey / low-res garbled       black / dark
root cause          a wrong gate hides it        it was never generated
the fix             remove the gate              generate the data
this session        prepass discard deleted      VIEW COVERAGE REPAIR
```

Two things that look identical on screen (dark patch) have **opposite underlying state** —
present-but-hidden vs never-made — and therefore **opposite fixes** (stop hiding vs start
making). Reading the coverage probe (`READY` vs `notSurfaceKnown`) is what tells them apart. If
you skip the probe and guess, you'll try to "un-hide" data that was never there, or try to
"generate" data that already exists and is merely masked.

---

## Check yourself

1. A dark patch appears. What single measurement decides whether it's Hole A or Hole B, and what
   are the two possible readings?
2. Why does Hole A look grey/garbled while Hole B looks black? (Tie it to which painter, if any,
   is active.)
3. The contract was OFF and the world was whole; you turn the contract ON and grey holes appear.
   Which hole type, and why did the contract *reveal* rather than *create* it?
4. Why can a pixel-driven repair loop never fix Hole B, no matter how many you add?
5. Transfer: you delete a caching layer and suddenly see stale data. Is that "suppressed" or
   "absent" in this vocabulary, and what does that tell you about where to look?

## Teachback
Draw the painter stack, then narrate one pixel through Hole A and one through Hole B, ending each
with the probe reading that identifies it. 2 minutes.

## Review
Now, +1d (the diagnostic-key table cold), +3d (near-neighbor: given a screenshot + probe reading,
name the hole and the fix), +7d, +21d.
