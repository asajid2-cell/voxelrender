# VENPOD Engine Stability Map - 2026-07-01

## Current Ground Truth

Authoritative repo state for this map:

- Branch: `wip-edit-latency`
- HEAD: `05c323c Checkpoint: no-dip ~100fps build shipped to main as default; next = present-pacing/temporal-far reworks`
- Default run contract: bare `.\rebrun.ps1 -NoBuild` uses `PerfMode=quality`.
- Active Loop 63 baseline in `perf/LOOPS.md`: idle/walk/yaw/edit are still dominated by
  `PERF_GPU/raymarchMs` around 19-23 ms in default full-resolution quality captures. The quiet
  100 FPS checkpoint is a useful historical commit, not the current optimization proof for this
  dirty Loop 63 worktree.
- Current worktree is not clean. It contains older uncommitted height-pump/window diagnostics work plus untracked stabilization scripts/report. Treat those as a separate checkpoint decision before starting a new optimization loop.

This means the next phase is not "make the whole engine fast from scratch." It is: preserve the current default quality baseline, remove remaining dips, and make the control plane understandable enough that future changes do not reintroduce coupled stutters.

## Quality Contract

Default quality should mean outcomes, not implementation details:

- Native output, `RenderScale=1.0`.
- Full-resolution near/surface rendering.
- Far terrain visually equivalent to the current sharp quality path.
- No visible holes or unsafe ownership: `visibleMissing=0`, `residentMissingSurface=0`, `unsafeNearMiss=0`.
- Stable idle, walk, yaw, and edit/brush behavior after warmup.
- Internal reuse, caching, temporal amortization, LOD, or deferred work are allowed only if the final image and correctness gates hold.

Do not define quality as "fresh full-resolution far raymarch for every far pixel every frame." That is an implementation choice and the source of the remaining dip class.

## Frame Workload Pipeline

The real engine frame is orchestrated mostly in `src/main_launcher.cpp`. The important work sequence is:

1. Read previous-frame telemetry and residency state.
2. Budget far-SVO upload/finalization with `SparseRuntimeBudgetScheduler::BuildFarUploadBudget`.
3. Build frame pressure from smoothed CPU, predicted CPU, GPU frame, and GPU raymarch timings.
4. Choose dense chunk copy/generation budgets and sparse background render budgets.
5. Evaluate sparse runtime pressure and protected/background scales.
6. Process input, camera, replay, brush state, and sparse collision/grounding.
7. Wait for the current frame context fence, optional frame-latency waitable, and GPU timestamp readback.
8. Retire GPU readbacks: brush, ground raycast, miss feedback, ownership, surface cull, physics.
9. Admit sparse brick requests by class: public critical, visible, collision, edited, cache/speculative.
10. Pump exact sparse generation and mid clipmap generation.
11. Plan and stage GPU uploads through `SparseFrameUploadPlan`.
12. Publish page-table entries after upload/fence readiness.
13. Extract sparse surfaces and update mid mesh/mid clipmap resources.
14. Render sparse surfaces, full-screen raymarch/background, optional background split, overlay/UI.
15. Present, signal fence, then pump trailing dense chunk generation.
16. Update the next-frame prediction and emit telemetry.

Key `main_launcher.cpp` reading anchors in the current checkout:

- Frame loop body starts around `main_launcher.cpp:6392`.
- Budget/scheduler preparation is around `main_launcher.cpp:6453-7020`.
- Clipmap pump is around `main_launcher.cpp:14200-14278`.
- Frame-latency waitable, frame-context fence wait, and GPU timestamp readback are around `main_launcher.cpp:14647-14671`.
- The historically named `postWait` region is not just waiting; it spans post-fence feedback/readback/physics retirement before command-list reset, roughly `main_launcher.cpp:14673-15479`.
- Command-list reset and sparse GPU `BeginFrame` are around `main_launcher.cpp:15478-15521`.
- Upload planning, page publishing, surface extraction, and mid resource updates occupy the large `main_launcher.cpp:15522-19000+` region.
- Render command recording is around `main_launcher.cpp:20309-26570`.
- Command-list execute, swap, present, and signal are around `main_launcher.cpp:26587-26637`.
- End-of-frame prediction and telemetry are around `main_launcher.cpp:26683-27000`.

## Core Boundaries

- `SparseRuntimeBudget.*`: pure decision layer for frame pressure, background render quality, request budgets, upload budgets, surface/physics processing budgets, far upload, and miss feedback.
- `SparseVoxelWorld.*`: exact brick lifecycle: request queues, generation, upload queues, page invalidation/publish, edits, surface extraction, local physics.
- `SparseClipmap.*`: mid height/voxel clipmap interest, generation, snapshots, mid mesh inputs.
- `SparseVoxelGpuResources.*` and `SparseSurfaceGpuResources.*`: upload rings, readbacks, sparse GPU tables, surface buffers, ownership/feedback resources.
- `Renderer.*` and shaders: surface raster, depth prepass, full-screen raymarch, background split, mid overlay, GPU timestamps.
- `Window.*` and `DX12CommandQueue.*`: swapchain/present/fence pacing.
- `rebrun.ps1`: default quality contract and env preset source of truth.

## Stability Problems That Remain

1. `main_launcher.cpp` is still the hidden control plane. It collects the same pressure signals multiple times, applies local boosts/caps, then calls pure scheduler functions. This makes correct behavior hard to reason about and hard to unit-test.

2. Some budgets are centralized and tested, but their inputs are not. The scheduler is clean; the snapshot assembly around it is sprawling.

3. Far/background rendering was the current dominant floor, and Loop 63 now has the first real
   architectural fix for it. The previous low-resolution background split rendered `PS_Raymarch`
   into a separate scaled target with a freshly cleared DSV, so it shaded foreground pixels that the
   main full-resolution surface stencil later hid. The new experimental foreground-mask path renders
   sparse surface depth/stencil into the low-res background DSV first, so the low-res raymarch only
   shades pixels the background product owns. This is still env-gated and not the default quality
   policy, but it changes the problem from "background split is edit-unsafe" to "masked background
   product is correctness-clean, while walk/edit still have CPU/postWait tails."

4. Present/fence pacing is not fully settled. Prior waitable throttling had contradictory results because desktop/session variance was large. Treat present/fence work as a measurement loop first, not a code loop.

5. Edit/brush remains coupled to generation, upload, surface extraction, mid-clipmap/midmesh, page publication, and GPU feedback. It needs the same control-plane snapshot treatment before deeper optimization.

6. The current dirty worktree can confuse the baseline. The older height-pump cap may still be useful, and the Window fallback may be good diagnostics, but they are not part of HEAD's shipped no-dip checkpoint. Resolve them before attributing new measurements.

## Refactor Strategy

Do not start with a renderer rewrite.

The first refactor should be behavior-preserving control-plane extraction:

1. Create a small `SparseFrameWorkloadSnapshot` / `SparseBudgetContext` layer that assembles the inputs currently scattered through `main_launcher.cpp`.
2. Keep decisions delegated to `SparseRuntimeBudgetScheduler` unless a new pure decision is genuinely missing.
3. Unit-test the extracted inputs/decisions through `VENPODTests`.
4. Add one compact `PERF_WORKLOAD_DECISION` telemetry line only after the decision object exists, so runtime captures can tie dips to decisions without another giant log observer effect.

The goal is not fewer lines for its own sake. The goal is that every frame-changing subsystem has an explicit admission rule, spill rule, protected-work rule, and verifier.

Current verifier caveat: `_agent_build.bat` passes, but `VENPODTests` is red in the current dirty
worktree with 28 sparse-core failures. Until those failures are triaged as pre-existing or fixed, the
full unit binary cannot be used as a green regression gate. Any refactor loop must either first repair
that baseline or use a narrower verifier that is proven red/green for the extracted behavior.

## First Loops

### Loop 60 - Baseline And Worktree Hygiene

Invariant: current dirty worktree is classified before new optimization.

Actions:

- Decide whether the height-pump admission cap belongs on top of `05c323c`, is already superseded, or should be parked.
- Decide whether the Window swapchain diagnostic/fallback belongs as a separate robustness patch.
- Keep the new capture/analyzer scripts only if they are the chosen verifier path for this phase.

Verifier:

- `git status --short --branch` is documented.
- `_agent_build.bat` passes.
- Default quality smoke/capture uses the selected clean baseline.
- `visibleMissing`, `residentMissingSurface`, and `unsafeNearMiss` remain zero where the harness emits them.

### Loop 61 - Extract Workload Snapshot

Invariant: budget decisions are assembled through one testable frame workload snapshot without changing behavior.

Scope in:

- `SparseRuntimeBudget.h/.cpp`
- a small helper/source file if needed
- the budget-prep parts of `main_launcher.cpp`
- `test/test_sparse_core.cpp`

Scope out:

- shaders
- renderer pass order
- generation algorithms
- visual quality knobs

Verifier:

- `VENPODTests` covers the extracted snapshot/decision builder.
- Existing `SparseRuntimeBudgetScheduler` tests remain green.
- A short default-quality smoke emits the same pressure class/budget decisions as before for representative idle frames.

Entry caveat: this loop cannot start against the full `VENPODTests` binary until Loop 60 resolves
the current 28-failure baseline or provides a targeted test executable/check for the new helper.

### Loop 62 - Present/Fence Measurement

Invariant: present/fence pacing is either proven real in the current shipped baseline or removed from the top-priority list.

Actions:

- Run clean multi-run A/B with waitable off/on, vsync off, cache warm, and low-overhead logs.
- Compare `rawMs`, `body`, `gapPrev`, `perfFenceWaitMs`, `PERF_GPU`, and `PERF_WAITSPLIT`.

Verifier:

- At least 3 comparable runs per side or a stronger deterministic replay.
- GPU timing valid.
- Same visual/correctness gates.
- Conclusion based on effect size larger than same-config variance.

### Loop 63 - Temporal Far-Field Visual Product

Invariant: grazing far-raymarch dips are reduced without lowering visible quality.

Actions:

- Do not optimize the rejected no-hit mask path as-is.
- Prototype a cached/reprojected far visual product or far atmosphere/horizon reconstruction that preserves the current image before profiling it.
- Keep exact full-resolution raymarch as fallback for disocclusion, high-error regions, and validation.

Verifier:

- Far-horizon visual A/B against current quality baseline.
- `raymarchMs` p99/max drops on the known grazing-dip replay.
- `visibleMissing=0`, `residentMissingSurface=0`, `unsafeNearMiss=0`.
- No ghosting/shimmer regressions in yaw/walk.

## Loop 63 Update - Background Split Reality

Measured on 2026-07-01:

- Full-quality matched yaw baseline: `rawP50/P95/P99/max=21.31/26.98/30.68/31.02`,
  `rayP50/P95=20.15/24.78`.
- Background split scale 0.5 with surface fill on: yaw `rawP50/P95/P99/max=12.60/18.31/20.19/21.90`,
  correctness clean but visually softer in the distance.
- Background split scale 0.5 with surface fill off: yaw `10.76/14.43/15.61/16.30`, idle
  `10.12/15.13/16.59/19.51`, walk `11.72/16.94/18.54/21.40`.
- Edit with fill off failed correctness: `unsafeNearMiss=16%` at sample frame 195.
- Edit with fill on passed correctness but remained too slow: `18.81/29.53/32.21/34.16`.
- A naive adaptive fallback keyed on exact-contract non-ready feedback was rejected: without a hold it
  still failed edit correctness; with a 60-frame hold it stayed in fill and measured
  `18.76/30.26/32.93/41.45` in edit.
- Experimental foreground mask, scale 0.5, surface fill off, measured 2026-07-01:
  - idle: `rawP50/P95/P99/max=8.50/13.12/13.61/14.67`, zero frames over 16.7, correctness clean.
  - yaw: `10.45/13.07/13.97/14.16`, zero frames over 16.7, correctness clean.
  - walk: `13.15/17.97/22.63/25.24`, zero frames over 33, correctness clean, but 33 frames over
    16.7 attributed to postWait/body tail.
  - edit: `13.19/26.20/30.91/34.61`, correctness clean (`maxUnsafeNearMiss=0`,
    `maxResidentMissingSurface=0`, `maxVisibleMissing=0`), but still has postWait/surface tail.
  - matched yaw visual diff versus full-quality baseline improved versus the previous no-fill
    product: `meanAbsRgbSum=1.109/1.269/1.877` at frames 220/260/300.

Concrete next architecture target:

- Promote the foreground-mask product from probe to robust product only after the remaining tail is
  mapped and a final visual/correctness pass is clean. The first mask implementation is an explicit
  low-res sparse surface depth/stencil boundary, not a heuristic fill controller.
- The first adaptive-fill attempt is rejected. It proved the signal must be more explicit than raw
  contract-non-ready feedback, and the renderer-side foreground mask is the better boundary.
- Treat this as a renderer product refactor, not a quality downgrade. Full-resolution exact surfaces
  stay foreground-owned; the optimization is avoiding work in pixels the background product does not
  own.

Next measured blocker:

- Under the masked product, raymarch is no longer the dominant p95 tail in walk/edit. Walk has
  `rayP95=4.46` and `gpuP95=6.06`; edit has `rayP95=4.74` and `gpuP95=6.33`. Raw top-frame logs
  attribute the remaining tail mainly to pre-publish surface extraction:
  edit top frames extract `50-66` coords with `34-42` hidden-critical coords and
  `surfExtract=10.8-14.7ms`; walk top frames extract `19-35` general coords with
  `surfExtract=3.6-5.3ms` plus up to `2.14ms` mid upload. The next loop should target why
  post-open publish still requires these same-frame surface extracts and reduce work volume/cost,
  not move the same work around or re-open fill/no-fill heuristics.
- Analyzer support now records the pre-publish surface classes directly in `frame_map.csv` and
  `summary.csv`. Current masked-product map:
  - edit: `prePublishSurfaceElapsedP95/max=8.07/14.66`,
    `prePublishSurfaceExtractedP95/max=44/70`, maxima `hiddenCritical=42`,
    `hiddenTracked=1`, `general=47`, `queuedPublishesMax=141`.
  - walk: `prePublishSurfaceElapsedP95/max=4.76/5.56`,
    `prePublishSurfaceExtractedP95/max=42/59`, maxima `hiddenCritical=0`,
    `hiddenTracked=0`, `general=59`, `queuedPublishesMax=86`.
  This changes the next fix from "cap surface extraction" to "separate critical foreground repair
  from general surface debt at the pre-publish gate."

## Traps To Avoid

- Do not quote raw frame median without variance. Prior runs varied by about 6 ms across desktop load.
- Do not treat lower render scale or the old background split as a quality fix.
- Do not optimize any mask/no-hit branch before it passes visual equivalence.
- Do not move more logic into `main_launcher.cpp`; extract decisions out of it.
- Do not chase sub-millisecond CPU slices using raw frame time. Use phase timers/profiler counters.
- Do not mix dirty diagnostic patches with a new perf claim.
