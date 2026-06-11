# VENPOD Visual Failure Characterization (2026-06-10)

Captured from scripted flights/walks on HEAD `393476e` and judged frame-by-frame
(dense grids + full-size frames in `build/captures/analyze/`). **All failures
reproduce in BOTH render modes** (quality single-pass *and* 60fps gameplay
multi-pass) — confirmed via `flythrough_test` (60fps) vs `flythrough` (quality).
They are **fundamental and long-standing** (they match the user's earliest
screenshots), not capture/streaming artifacts. Prior "fixes" (commit `0692519`)
were verified on too-narrow curated frames and missed these in motion.

The NEAR/MID terrain (green terraced foreground) consistently looks GOOD. Every
failure is concentrated in the **FAR-distance rendering and the far/mid handoff
+ water classification at distance** — the far-height + far-SVO + horizon-haze +
far-water subsystem the session has repeatedly patched.

---

## F1 — Floating far terrain / detached horizon  *(SEVERE, omnipresent)*
- **Look:** distant terrain renders as grey-and-white spiky/blobby shapes
  *floating* above a band of sky/haze, disconnected from the green mid/near
  terrain below. The white is a haze/sky blend whitening the far peaks.
- **Where:** every frame, every altitude — ground walk AND flight, both modes.
- **Evidence:** `analyze/walk_skyblobs_full.png` (clearest), `fly_fragments_full.png`
  (horizon band), `fly60_check.png` (60fps).
- **Hypothesis:** the far-horizon haze dissolves the LOWER far terrain to
  sky-color but leaves the peaks, creating floating islands; and/or a
  coverage/vertical gap between the mid-ring edge and the far layer with sky
  showing through. The whitening is over-bright haze applied to the far layer.

## F2 — Fragments floating on water  *(SEVERE over water)*
- **Look:** tan/green blocky terrain patches scattered ON navy water surfaces —
  detached, sitting at water level like debris.
- **Where:** over any water basin, mid + far distance, both modes.
- **Evidence:** `analyze/fly_fragments_full.png` (bottom), `fly60_check.png`.
- **Hypothesis:** far-SVO/far-height leaf bricks classified as land sitting over
  far-water. The `0692519` "fragments" fix addressed the near-ring band but NOT
  this farther band/altitude (or it regressed).

## F3 — Water-hole peppering of mid terrain  *(MODERATE)*
- **Look:** green mid-distance land punctured with many small navy water holes →
  noisy / swiss-cheese.
- **Where:** mid-distance band while flying.
- **Hypothesis:** dense small procedural lakes + water bleeding through at
  distance; likely the same water-classification disagreement as F2 at smaller
  scale. (Need to confirm the lakes are by-design vs render error.)

## F4 — Aerial wash-out  *(MODERATE)*
- **Look:** from ~1500u looking down, terrain is a flat low-contrast grey-green
  murk — over-hazed, no detail or relief.
- **Where:** high-altitude top-down.
- **Evidence:** `analyze/aerial_reveal_grid.png`.
- **Hypothesis:** aerial/far haze + the FixB palette unification leaving the
  high-altitude far layer flat and grey.

---

## Owner-map attribution (debug 58) — CORRECTED via tandem (Codex caught an enum mix-up)
**CORRECTED legend:** debug-58 (PS_Raymarch.hlsl:5226) colors by the **BACKGROUND_LAYER**
enum via `DebugOwnerLayerColor`, NOT the RENDER_OWNER enum. True mapping:
**green=MID_VOXEL, yellow=MID_HEIGHT (and near-exact surface, :6260), blue=FAR_SVO,
orange=FAR_HEIGHT, cyan=FAR_WATER, dark=SKY/none.**
(The earlier draft below read the colors against the wrong enum and inverted
FAR_SVO/FAR_HEIGHT — see TANDEM.md. The floating peaks are **orange = FAR_HEIGHT
(the CONTINUOUS backdrop), not far-SVO**. FAR_SVO (blue) is barely present. So F1 is
the continuous far-height layer rendering detached peaks with SKY gaps — a far-height
march/arbitration-routing bug, NOT far-SVO sparseness. Re-attribution in progress.)

### (superseded draft)
Legend (from `DebugOwnerLayerColor`): **green=NEAR, yellow=MID_VOXEL,
blue=MID_HEIGHT, orange=FAR_SVO, cyan=FAR_HEIGHT, dark navy=SKY/miss.**
Frames: `build/captures/own_walk_owner2/`, `own_fly_o/` (+ paired normals
`own_walk_n/`, `own_fly_n/`).

- **F1 (floating far terrain) = FAR_SVO (orange) + SKY (navy) gaps.** The distant
  peaks are far-SVO octree leaves; the band *between* them and the mid terrain is
  unowned SKY, NOT cyan FAR_HEIGHT. ⇒ **far-SVO renders only its sparse resident
  chunks and its gaps fall through to SKY instead of the continuous FAR_HEIGHT
  backdrop.** The `dca7bfd` "far-SVO → far-height fall-through" is NOT active in
  these ground/low-altitude views. This is THE root of the floating-islands look.
- **F2 (water fragments) = the MID_VOXEL (yellow) ↔ FAR_HEIGHT (cyan) water
  boundary.** Far water is FAR_HEIGHT (cyan); near water/terrain is MID_VOXEL
  (yellow); the tan/green fragments sit at that handoff — a layer-disagreement at
  the mid→far water boundary, not random debris.
- Confirms the whole failure set is the **far-SVO / far-height / handoff**
  subsystem; NEAR + MID_VOXEL (the good-looking foreground) are healthy.

## Fix campaign (proposed)
1. **Owner-map attribution** (`-DebugMode 58`: green=MID_VOXEL, orange=FAR_HEIGHT,
   blue=FAR_SVO, cyan=FAR_WATER) on each failure scenario — confirm exactly which
   layer paints the floating blobs (F1), the water fragments (F2), and whether the
   F1 gap is sky (no owner) or hazed-far. This is diagnostic, not demo capture.
2. Targeted fixes per failure, in severity order (F1, F2, then F3/F4).
3. **Verification standard: VIDEO review** — dense frame grids across full motion
   at multiple altitudes/headings, judged frame-by-frame. NOT curated stills
   (that is what let these survive). Adopt the user's standard.
