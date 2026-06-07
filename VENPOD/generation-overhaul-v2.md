# VENPOD Generation/Streaming Overhaul (V2)

Status: ACTIVE PLAN (2026-06-07). This supersedes `frontier-streaming-design.md`
(which scoped an incremental gating fix that was proven insufficient). Read
`active-goal-handoff.md` "FINAL VERDICT" first.

## Why we are doing this

8 experiments on the trustworthy walk bench proved the current engine cannot reach
60 FPS or even stable 30 by ANY local change. Root cause: the engine couples three
things that must be independent —
1. coverage (what's needed), re-derived every frame from continuous camera state;
2. generation (terrain gen + meshing), run synchronously on the critical path and
   FORCED unbounded to satisfy coverage ("generate-or-freeze");
3. frame rendering, blocked until generation satisfies coverage.

This is debt from a finite-chunk sandbox stretched into an infinite sparse-voxel
world. The "never show a hole" policy puts unbounded generation on the frame's
critical path → terrain-dependent spikes (180-280 ms) and, when bounded, backlog
explosions (up to 36 s freezes). The unbounded full per-frame rebuild is the only
stable point (~26 FPS). It must be re-architected, not tuned.

## Target architecture: background-streamed, best-available-LOD renderer

The standard correct design for an infinite LOD voxel world. Invert the control flow
from "frame drives synchronous generate-to-cover" to "background streams; frame
renders best-available."

Core principles:
1. **Render best-available LOD, never block.** The public frame renders whatever is
   currently resident at the best LOD it has (exact sparse -> mid clipmap -> far SVO
   -> sky). Missing fine detail shows as COARSER, never as a hole and never as a
   freeze. This is the keystone: it removes "generate-or-freeze."
2. **Generation is a pure background producer.** All terrain gen + surface meshing
   runs on a worker pool, feeding a ready-results queue. The main thread NEVER
   generates terrain; it only APPLIES ready results within a bounded per-frame
   upload budget. Generation throughput is bounded by workers, not the frame.
3. **Prefetch-ahead.** Workers generate cells around AND ahead of the camera (along
   its velocity) so detail is usually resident before it's needed.
4. **Persistent gen/mesh cache.** Generated+meshed bricks persist in a bounded LRU
   keyed by cell coord + edit-version, so eviction/re-entry/edits don't regenerate
   from seed. (Edits write an overlay; affected cells re-queue in the background.)
5. **Discrete-cell frontier residency.** Residency is maintained by discrete-cell
   deltas on recenter, not a continuous per-frame full re-derivation. Safe now,
   because async generation + best-available render mean staleness = "detail arrives
   a few frames late," not a freeze or hole.

The current codebase already has the PIECES (sparse brick pool, mid clipmap, far
SVO, worker pools, surface extractor). The overhaul rewires the CONTROL FLOW and
adds the persistent cache + prefetch-ahead producer. Much of the existing
synchronous/coverage-blocking machinery gets deleted at cutover.

## The core tradeoff (accepted)

Stable framerate REQUIRES rendering transiently-coarser terrain while detail streams
in (during fast motion / fresh regions), instead of freezing to keep it sharp. When
stationary or slow, it converges to full detail. This is the deal the architecture
makes; it is why it can be stable. (The current engine refuses this and freezes
instead — that is the debt.)

## Staged rollout (each: flag-gated, bench-validated, committed, reversible)

Master flag: `VENPOD_STREAMING_V2`. Build V2 alongside the old path; A/B on the
bench; cut over and delete the old path at the end. Validate every stage on
`walk_bench.ps1` (median + p99 + the multi-second-stall check + coverage/visual),
including the dense-terrain regions (~frame 730+ at speed 38) that expose the spikes.

- **Stage 0 — Seam.** Introduce the V2 module + master flag as a no-op pass-through
  over the current path. No behavior change; establishes the boundary. Bench-neutral.
- **Stage 1 — Best-available-LOD render decouple (KEYSTONE).** Change the public
  render/publish gate so it NEVER blocks on exact-voxel coverage — render
  best-available LOD. Remove "generate-or-freeze." Expect: spikes collapse (frame no
  longer waits for generation); visuals show transient coarseness, not holes/freezes.
  This is the make-or-break and the single biggest win. Validate frame stability AND
  that fallback LOD looks acceptable (no white holes, no sky leaks).
- **Stage 2 — Generation as pure background producer.** Move ALL gen+mesh to workers
  feeding a ready queue; main thread applies ready bricks within a per-frame budget;
  delete synchronous/forced generation from the frame path. Combined with Stage 1 the
  frame never waits on generation.
- **Stage 3 — Prefetch-ahead + persistent gen/mesh cache.** Workers prefetch ahead of
  the camera; bounded LRU cache so re-entry/edits don't regenerate. Reduces visible
  coarseness during motion.
- **Stage 4 — Discrete-cell frontier residency.** Replace the per-frame full interest
  re-derivation with recenter deltas (cheap; safe now). Removes the residual
  per-frame planning cost.
- **Stage 5 — Cut over + delete debt.** Make V2 default once it beats the old path
  across fixed/walk/high-alt on the bench; remove the old synchronous path and the
  large set of now-dead coverage-emergency flags/knobs. Shed the debt.

Stage 1 is the keystone: if rendering best-available LOD gives a stable frame with
acceptable visuals, the thesis is proven and Stages 2-5 build the clean producer
behind it. If best-available fallback looks unacceptable, the LOD chain itself needs
work first (Stage 1a: strengthen mid/far fallback quality) before decoupling.

## Discipline (unchanged from this session)

- `walk_bench.ps1` (fixed dt, vsync off, per-frame, median/p99/noise band, dense
  regions, multi-second-stall detection) is the sole accept/reject authority.
- One stage, one flag, bench + visual validation, git checkpoint. No stacking
  unvalidated changes. Old path stays until V2 wins, then deleted.
- Consider extracting the V2 streaming control out of the 1.37MB main_launcher.cpp
  monolith into its own module as part of shedding debt.
