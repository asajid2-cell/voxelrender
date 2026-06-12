# TANDEM ledger — VENPOD moving-ground clip spike

Driver: Claude (source/architecture). Partner: Codex (runtime/empirical).

## Goal
Cut the moving-on-ground frame-time spike. Hard gate: midCov 1.00 converged WHILE MOVING
(no under-streaming "watery holes"). C++-only (no shader).

## What "clip" is (established)
`perfSparseClipmapPrepMs` = whole mid-clipmap section. On a heading-45 walk, the worst
clip frames decompose as:
- `interest` (perfSparseClipmapInterestMs, UpdateVoxelInterest rebuild): ~9-13ms on cross frames
- `pump` (perfSparseClipmapPumpMs, 12534-12860): ~13-18ms on spike frames
- `other` (parentHeldFeedback raymarch + missingSampleFeedback): ~0.5ms typically

## KEY EMPIRICAL FINDING (refuted my source theory)
The original premise "clip = interest rebuild" and my follow-up "pump = brick generation,
parallelize it" are BOTH WRONG:
- The parallel VOXEL brick pump generates 192 bricks in **parWall=0.11ms** (negligible).
  Brick generation is NOT the cost. Enabling parallelVoxelPump did NOT help (ppActive but
  0.11ms; clip unchanged).
- On spike frame f188: pump=18.34ms = HEIGHT tile gen `mainThreadBrickGenMs=6.04` (6 tiles,
  GenerateTile does 33x33=1089 HeightAt calls/tile => ~1ms/tile) + voxel 0.11ms + **~12ms
  unattributed pump-section overhead** (the budget-compute block 12535-12810 + bookkeeping).
- interest=9.59ms is a genuine SECOND co-equal contributor (my budgeted per-ring interest
  rebuild fix — already implemented — addresses this half).

## Open question the partner is resolving NOW (job bjg836t0m)
Split the 18ms pump precisely: budget-compute block ms vs PumpGeneration-call ms vs
height-gen ms. If the ~12ms is pure budget-compute overhead, that's the cheapest SAFE win
(no coverage risk — it's not generation). The partner is adding PERF_SPARSE_PUMP_SPLIT
temp timers + benching.

## My implemented fix so far (SparseClipmap.cpp/.h, main_launcher.cpp env knob)
Frame-budgeted per-ring interest rebuild (voxelInterestRebuildRingsPerFrame, default 2):
on a footprint cross, rebuild only N rings, carry the rest from the prior interest set
(coverage-safe superset). Reduced interest spike but NOT the dominant pump spike.
midCov stayed 1.00 in all A/B runs.

## RESOLVED (partner's PERF_SPARSE_PUMP_SPLIT diagnostic, ground truth)
preludeMs=0.00 (budget-compute block is FREE), tailMs=0.00, **callMs=25.26** on worst frame
(f188). The ENTIRE pump spike is inside the PumpGeneration CALL. With voxelBudget=192 gen'd
in 0.11ms (parallel), the call cost is the HEIGHT TILE pump: heightBudget=16 tiles x
GenerateTile's 33x33=1089 HeightAt calls = ~17k HeightAt/frame, single-threaded.
=> The two co-equal clip spike sources: (1) interest rebuild ~10ms [fixed: per-ring budget],
(2) HEIGHT tile pump ~6-25ms [FIX: parallelize GenerateTile loop like the voxel pump].
Parallelizing the height pump is coverage-neutral by construction (same tiles, same budget,
fanned across workers). Do NOT touch HeightAt (guardrail) — only the caller loop.
Partner left a useful perfSparseClipmapBudgetMs + PERF_SPARSE_PUMP_SPLIT diagnostic in
main_launcher.cpp (kept).

## Decision (CONVERGED) — pending partner's pump split [historical]
- If budget-compute overhead dominates (~12ms): optimize/cache that block (cheap, safe).
- If height-gen dominates: parallelize the HEIGHT pump (coverage-neutral) — do NOT touch HeightAt.
- Keep the interest-budget fix for the interest half.

---
## Claude (Fable, main driver) cross-check — 2026-06-10 later session
My INDEPENDENT source read (before re-reading this ledger) ranked the cell-cross spike as the
INTEREST REBUILD (full re-expand of 5 ring anchor shells, ~74ms) and proposed budget/incremental
+ req-cache. THAT WAS THE BLIND SPOT: I anchored on the obvious suspect (interest) and assumed the
PUMP was cheap/GPU (carried over from the earlier far-SVO/gen work). Codex's runtime ground-truth
(PERF_SPARSE_PUMP_SPLIT: prelude=0, callMs=25.26; voxel gen 0.11ms parallel) proved the dominant
half is the single-threaded HEIGHT TILE pump (~17k HeightAt/frame). Interest is only co-equal ~10ms.
=> CONVERGENCE on the FIX once the empirical split was in hand: parallelize the height pump
(coverage-neutral; don't touch HeightAt) + per-ring interest budget. Both now in the working tree
(fix-clip agent). This is the textbook tandem outcome: empirical vantage corrected the source theory.

LIVE TRACKS (3 independent):
1. Workflow fix-clip (used Codex): parallelHeightPump + interest ring-budget — IMPLEMENTED, benching.
2. Fresh Codex re-derivation (job launched, reads HEAD, blind to the WIP): independent re-check +
   REQ-side levers (pressureTrim / PlanHierarchical per-coord dispatch) for the upcoming fix-req.
3. Me: judge coverage (midCov 1.00) + holes sheets myself before commit; reconcile all three.

OPEN: does the height-pump parallelization hold midCov 1.00 while moving? (same tiles/budget fanned
to workers => should be coverage-neutral by construction, but VERIFY on the walk, not by argument.)
REQ side (7.4ms steady) is still unaddressed by fix-clip — that's fix-req + Codex's input.

---
## RESOLVED cross-check (3 independent tracks, 2026-06-10)
DIVERGENCE found + resolved by ground-truth:
- Fresh Codex (READ-ONLY, "did not build/run") ranked clip lever #1 = incrementalize the INTEREST
  rebuild. Claude source analysis: SAME. Both MISSED the height pump.
- Earlier Codex (RAN it, added PERF_SPARSE_PUMP_SPLIT, callMs=25.26): HEIGHT TILE pump is the
  dominant/co-dominant half — invisible to code-reading.
- RESOLUTION: measurement wins. Two source-only analyses both missed it; only the empirical pass
  caught it. Workflow correctly parallelizes the height pump BECAUSE of the measurement.
CONVERGENCE (all three agree):
- Clip = parallelize height pump + budget interest rebuild over frames (carry old set as SUPERSET).
- Req = dedupe per-coord requestSparseBrick dispatch + cache PlanHierarchical across unchanged
  signatures + gate/budget pressureTrim. (Codex levers 4-6; align with fix-req attribution.)
- SAFETY (Codex's "what I would NOT do", caught a flaw in Claude's hypothesis #2): do NOT do naive
  edge-only toroidal as source of truth — interior coords change priority after a cross; do NOT cap
  critical interest/request admission (silent under-stream => holes). Synchronously admit leading
  edge + near-camera coords every frame regardless of budget.
ACTION: ensure fix-req implements dedupe + PlanHierarchical signature-cache (Codex 4-5). Verify
height-pump parallelization holds midCov 1.00 on the actual walk (empirically, not by argument).

---
## fix-clip VERIFIED (2026-06-10, gates run on the actual walk)
OPEN ITEM CLOSED: height-pump parallelization holds midCov 1.00 while moving — confirmed
empirically, not by argument.
Same-session A/B (median of 3 reps, NORMAL logging, vsync off, speed 45) full-fix OFF vs ON:
- heading 45 : med 22.49->21.71, p95 42.56->36.42 (-14%), worst 69.89->51.64 (-26%)
- heading 200: med 24.11->19.62 (-19%), p95 44.48->31.95 (-28%), worst 83.02->48.11 (-42%)
- standing (speed 0): med 14.69->13.49, worst 49.12->38.08 (NOT regressed)
- stress-fly (yaw90 speed250 pitch-55, 8 workers): badMarkers=0, clean exit, midCov 1.00
GATES: (a) midCov 1.00 converged both headings + stress (dips are startup-only frames 0-1);
(b) 6 walk capture frames (h45 + h200) read as images — NO watery holes / NO new artifacts;
(c) standing fps not regressed; (d) 0 of 0x7FF8FFF2|Failed to create graphics|Map failed|
DEVICE_HUNG across ~16 runs + stress-fly. Worst-case cell-cross hitch cut 26-42%.
FILES: SparseClipmap.cpp/.h (parallel height pump + per-ring interest budget),
main_launcher.cpp (env knobs, default ON). Partner's PUMP_SPLIT diag reverted by track.

---
## mid LOD screen-space-error review (2026-06-11)
TANDEM watcher started; partner turn was still running when local source+math reached a verdict.
LOCAL VERDICT:
- Screen-space ring boundaries are valid math, but with the actual 60deg vertical FOV and half-res
  mid pass a 4u cell is already below a 3px footprint at MID_START=768. Literal K=3-5 therefore
  makes the 1-3k visible band coarser, not finer.
- Current SparseClipmap rings are not true annular screen-space shells: UpdateVoxelInterest still
  builds a camera/forward bubble per ring. Replacing BuildRings boundaries alone changes preferred
  LOD/anchor distances, but does not make brick count proportional to screen pixels.
- Any ring-growth/boundary change must update CPU policy, CPU feedback/projection paths, shader
  MidClipmapRingCellSize/preferred-ring selection, and include the new layout params in the
  interest signature. Current ringGrowthFactor is not represented in the signature.
ACTIONABLE: Do not ship K=3-5 screen-space boundaries as the next quality fix. If tested, use it as
a diagnostic with K near 1px/full-scale or 1.5px at 1080, and prioritize projected-visible admission
or AA/TAA/geomorph for the remaining jaggies.

---
## editable SVDAG architecture read (2026-06-11)
TANDEM watcher started; peer job launched blind to the local conclusion but did not return before
the local source pass completed.
LOCAL VERDICT:
- Current checkout is hybrid: near sparse surface mesh raster, mid sparse clipmap/raymarch, far
  analytic height + CPU-built FarVoxelOctree. The exact numeric cap summary is stale: shader
  MID_VOXEL_CLIPMAP_MAX_BRICKS is 49152, while main_launcher still defaults the CPU mid cap to
  16384. The cap wall remains real, but the current constants are not normalized.
- Edit path is not fully GPU-authoritative. CS_SparseBrushFeedback emits compact records, then
  SparseVoxelWorld::ApplyEditedVoxels applies them on CPU, regenerates brick payloads, and queues
  surface extraction. Async surface extraction exists but explicitly excludes edited terrain.
- Do not full rip-and-replace first. Keep the proven near rasterized surface + water/stencil
  ownership as foreground authority, and replace the broken mid/far backing with an editable DAG
  behind it. Later, if the DAG renderer/mesh extraction proves equivalent, collapse near onto it.
RATIONALE:
- The user-visible failure is mid/far scale/coverage plus edit hitch; near mesh quality is the
  only part already proven visually correct.
- Full replacement would couple DAG traversal, editing, surface shading, water, collision, brush
  raycast, and stencil ownership in one high-risk cut.
- New DAG traversal should be a separate pass/resource set, not more code in PS_Raymarch; that
  uber-shader is already at the NVIDIA driver-JIT/TDR cliff.

---
## OVERHAUL KICKOFF — architecture LOCKED (2026-06-11, Claude driving + Codex)
Human directive: implement the full editable-SVDAG overhaul → (1) edits/adds via raymarch brush
with NO hitch; (2) terrain renders fully+correctly to horizon — no old flat mid, no holes, no giant
far blocks.

CONVERGED across BOTH minds + two independent Codex sessions:
1. KEEP the near rasterized surface mesh (RenderSparseSurfaceFaces, own PSO) + water/stencil as
   foreground authority. It's the only visually-proven part.
2. REPLACE the broken mid/far backing (RenderVoxels fullscreen background) with an editable SVDAG.
   Minimal blast radius; solves both goals. NOT a full rip-replace.
3. The existing FarVoxelOctree IS the SVO seed: Node{childBase,childMask,material,flags}, paged
   roots, GPU-resident, and `FarVoxelChildNodeIndex = childBase + countbits(precedingMask)` is
   already DAG-compatible → static DAG render needs ZERO traversal-logic change; only the BUILDER
   de-dups subtrees.
4. **CONSTRAINT (Codex, non-negotiable): the DAG raymarch goes in a SEPARATE pass/shader, NOT
   bolted into PS_Raymarch.hlsl — that uber-shader is at the JIT/TDR cliff. A fresh simple DAG
   shader is also easier to get correct (no accumulated altitude/quality gates) and is where the
   "no old mid / no holes" win actually comes from (one coherent structure replacing 3 fighting
   layers).**
5. Edit path today is NOT GPU-authoritative: CS_SparseBrushFeedback → ApplyEditedVoxels (CPU) →
   regenerate bricks → queue surface extraction (sync for edited bricks) = the hitch. Target:
   GPU-authoritative DAG edits, no CPU readback.
6. Numeric parity drift to fix in cutover: shader cap 49152 vs CPU default 16384.

LOCKED PHASES:
- P1 (foundational, isolated, testable, ZERO render risk): add bottom-up DAG de-dup compression to
  the FarVoxelOctree builder + deepen past the 16u leaf floor. Measure compression ratio + build
  cost empirically (ground-truth the "it fits" claim before building the renderer on it).
- P2: NEW separate fullscreen DAG raymarch pass (own PSO/shader, off the uber-shader) rendering the
  deepened DAG as mid+far, depth-composited behind the near mesh. Retire RenderVoxels mid/far for
  the covered range → no old mid, no holes, no giant blocks.
- P3: GPU-authoritative DAG edits (HashDAG/GPU-SVDAG-Editing build-temp-SVO-then-merge, no readback)
  + async near re-mesh → no hitch.
- P4: decouple attributes (Morton material stream) for cheap recolor/material edits.
- P5: cutover — delete dead mid/far gates, reconcile 49152/16384, validate native-res across motion.

NEXT: Claude builds P1 DAG compressor + measures; re-engages Codex with ground-truth numbers (build
cost at finer resolution is the key empirical feasibility question — settle by measurement, not
argument).

---
## P1 RESULT (measured + verified, 2026-06-11)
Added gated DAG materialization to FarVoxelOctree::BuildCpuData (VENPOD_FARVOXEL_DAG_ANALYZE=1).
MEASURED on real terrain (coarse 16u leaf floor, maxDepth 6, 625 pages):
  tree 16,107,312 nodes (245.8 MiB) -> DAG 409,635 nodes + 2,821,579 ptrs = 17.0 MiB (39.3x), in 0.7s.
  verify=OK: re-expanded DAG vs tree in lockstep across 8 page roots (200,630 nodes) -> EXACT match
  (leaf-ness, childMask, material, flags all agree). The pointer-format DAG reproduces the geometry.
Format LOCKED: DagNode{uint childPtrBase, childMask, material, flags} (16B fixed) + flat
ChildPointers[]; child = ChildPointers[childPtrBase + countbits(childMask & ((1<<c)-1))]. Leaves
carry material/flags, childPtrBase=0xFFFFFFFF. All gated -> normal rebrun.ps1 runs byte-identical.

## P2 SEAM MAP (Codex independent render-code read, CONVERGED + 1 correction, 2026-06-11)
DIVERGENCE Codex caught: depth-compositing is WRONG for this engine. Fullscreen passes use
depthWriteMask=ZERO, VS_Fullscreen emits fixed z=0, PS_Raymarch outputs only SV_Target (no SV_Depth)
-> depth alone can't gate the DAG behind the near mesh. The real mechanism is STENCIL OWNERSHIP.

CONVERGED concrete seam (build on this):
- Near sparse surface writes stencil ownership: RenderSparseSurfaceFaces uses stencilEnable + REPLACE
  + OMSetStencilRef(1) (Renderer.cpp:1050,1737). Background raymarch paints only where stencil==0.
- NEW PS_DagRaymarch.hlsl + m_dagRaymarchPipeline, gated default-off (env flag beside the mid-pass
  flag, main_launcher.cpp:1041 / Renderer.h:27). DO NOT add DAG to PS_Raymarch (regs already t0..t17,
  root params 0..19, at the JIT/TDR cliff).
- Minimal root sig: b0 frame constants, t0 DagNodes, t1 ChildPointers, t2 DagPageTable (+t3 material
  palette if needed). Separate one-descriptor tables (matches existing renderer pattern).
- COMPOSITE direct into main RT AFTER RenderVoxels, BEFORE RenderOverlays (main_launcher.cpp:20408).
  PSO: RTV main R8G8B8A8, DSV main D24S8, depthEnable=false, stencilEnable=true, frontStencilFunc=
  EQUAL, stencilRef=0, readMask=0xFF, writeMask=0, blendEnable=true, alpha=1 on DAG hit / 0 on miss.
  Mirrors the existing mid composite ownership test (Renderer.cpp:992,1963). DAG overwrites broken
  mid/far where it hits, leaves sky/old fallback on miss, overlays stay on top. NO PS_Raymarch change.
LANDMINES: render DAG AFTER RenderVoxels (before it, old raymarch overwrites). Don't copy the private
low-res DSV background-split pattern unless going offscreen — direct main-RT stencil composite is
cleaner for P2.

NEXT (Claude building): (A) productionize DAG -> persistent GPU buffers (DagNodes/ChildPointers/
pages + SRVs), gated. (B) PS_DagRaymarch.hlsl porting TraverseFarVoxelPage to the ptr format +
m_dagRaymarchPipeline (stencil EQUAL 0, alpha-blend, no depth) wired after RenderVoxels. Then run +
cross-check the rendered output with Codex (native-res, holes/old-mid gone).

## P2-A DONE + VERIFIED (DAG GPU residency, 2026-06-11)
FarVoxelOctree.h/.cpp: BuildDagFromTree (canonical materialization helper) + EnsureDagResident
(one-shot InitializeWithData upload of m_dagNodeBuffer/m_dagChildPtrBuffer/m_dagPageBuffer + CreateSRV
+ shader-read transition). Wired in main_launcher.cpp right after EmitPendingGpuUploadCopies
(line ~14300), gated by VENPOD_FARVOXEL_DAG; builds once when the tree is resident, retries until
then. RAN: "[DAG-RESIDENT] nodes=409635 ptrs=2821579 pages=625 (17.0 MiB) in 886.9 ms; SRVs valid=1",
no crash, no errors. Default runs (flag off) byte-identical. NOTE: BuildDagFromTree runs ~0.7s
synchronously on the main thread the frame it first builds -> a one-time hitch; move to a worker or
build-time later (fine for now).
NEXT: P2-B = PS_DagRaymarch.hlsl + m_dagRaymarchPipeline (Codex's stencil-EQUAL-0 + alpha-blend
composite after RenderVoxels) -> first visible DAG render. Then cross-check with Codex at native res.

## P2-B SHADER DONE (assets/shaders/Graphics/PS_DagRaymarch.hlsl, 2026-06-11)
Self-contained DAG raymarch (NOT in PS_Raymarch). #includes SharedTypes.hlsli for FrameConstants.
Reconstructs the ray verbatim from PS_Raymarch (cameraPosition/Forward/Right/Up + tan(fov*0.5)).
Page-march (96 steps, DagDistanceToPageExit 2D x/z DDA) -> DagPageIndex[denseIndex] -> DagPages[pi]
-> TraverseDagPage (64-deep stack, ptr-indirection: child = DagChildPointers[childPtrBase +
countbits(childMask&((1<<c)-1))]); leaves rendered as TRUE voxel boxes (nearest box hit, box normal,
material color), air (material 0) skipped. Miss -> alpha 0 (sky/fallback shows). Hit -> alpha 1.
Registers: b0 frame, t0 DagNodes, t1 DagChildPointers, t2 DagPages, t3 DagPageIndex (reuse far tree's
GetPageIndexSRV - DAG pages are emitted in the SAME order as tree pages, so the denseIndex map is
valid). Self-reviewed: ptr indexing matches BuildDagFromTree; leaf test consistent; 64-stack is the
SAME depth the shipping TraverseFarVoxelPage uses (safe for maxDepth-6/16u pages) -> TDR risk is
LOWER than the uber-shader (tiny isolated shader, low register competition). Codex investigated it
but its written verdict didn't surface (bridge quirk); risks cross-checked locally instead.

## P2-B REMAINING (the pipeline + wiring -> first visible frame). References gathered:
- Root sig is EXPLICIT (DX12GraphicsPipeline.cpp CreateRootSignature reads desc.rootParams), NOT
  reflected. GraphicsPipelineDesc{ vector<RootParameter> rootParams; vector<StaticSamplerDesc>
  staticSamplers; vector<DXGI_FORMAT> rtvFormats; DXGI_FORMAT dsvFormat; depthEnable; stencilEnable;
  stencilReadMask; stencilWriteMask; frontStencilFunc; frontStencilPassOp...; blendEnable; ... }.
  RootParameter{ RootParamType type; uint shaderRegister; uint registerSpace; visibility;
  numDescriptors; D3D12_DESCRIPTOR_RANGE_TYPE rangeType; num32BitValues; }.
- TODO CreateDagRaymarchPipeline(device): compile VS_Fullscreen + PS_DagRaymarch (ps_6_0); rootParams
  = [ CBV b0, table SRV t0, table SRV t1, table SRV t2, table SRV t3 ] (mirror the fullscreen CBV
  binding - find how b0 frame constants is bound in RenderVoxels: SetGraphicsRootConstantBufferView
  vs a CBV root param at index 0). PSO state (Codex's composite): rtvFormats={main R8G8B8A8_UNORM},
  dsvFormat=main D24S8, depthEnable=false, stencilEnable=true, stencilReadMask=0xFF, stencilWriteMask=0,
  frontStencilFunc=EQUAL, blendEnable=true. cullMode=NONE.
- TODO RenderDagRaymarch(cmdList, dagNodeSRV, dagChildPtrSRV, dagPageSRV, dagPageIndexSRV, camera,
  farFieldParams): bind pipeline, OMSetStencilRef(0), upload frame constants (set farFieldParams
  x=1,y=pageCount,z=nodeCount,w=pageSize; farFieldGridParams x=pageRadius,y=pageSide), bind CBV +
  4 SRV tables, draw fullscreen triangle (3 verts). Mirror RenderVoxels' constant upload.
- TODO main_launcher: after renderer->RenderVoxels(...) and BEFORE RenderOverlays (~line 20408),
  gated by VENPOD_FARVOXEL_DAG && farVoxelOctree.IsDagReady(), call renderer->RenderDagRaymarch(
  commandList.Get(), farVoxelOctree.GetDagNodeSRV(), GetDagChildPtrSRV(), GetDagPageSRV(),
  farVoxelOctree.GetPageIndexSRV(), camera, {pageCount=GetDagPageCount(), nodeCount=GetDagNodeCount(),
  pageSize, pageRadius}).
- Then build + run (VENPOD_MODE=sandbox + VENPOD_FARVOXEL_DAG=1) + capture native-res + cross-check
  with Codex that holes/old-mid/giant-blocks are gone behind the near mesh.

## P2-B DONE — DAG RENDERS END-TO-END (2026-06-11)
Built CreateDagRaymarchPipeline + RenderDagRaymarch (Renderer.cpp/.h) + launcher wiring (after
RenderVoxels, before overlays, gated by IsDagReady). Pipeline = fullscreen depth/stencil state
(stencil EQUAL ref 0) + blendEnable. Debugged stage-by-stage with capture probes (quality mode):
- magenta-everywhere -> fills stencil==0 (sky), near mesh blocks it: PASS RUNS, stencil composite OK.
- cbuffer color probe -> pink (enabled=1, nodeCount=409635, pageSize=1024): CBV bound correctly.
- gridParams probe -> blue (pageRadius=12, pageSide=25): grid params correct.
- BUG FOUND + FIXED: rendering interior-leaf cells (kInteriorLeafFlag, conservative below-surface
  solid) as boxes created false sky geometry. Skip them (like the original's heightfield-recovery
  treatment) -> DAG now renders real far terrain into the gaps. Verified via cyan hit-tint.
NOTE: a 64-stack-deep traversal in a tiny shader did NOT TDR (ran fine), confirming the local
"separate simple shader is safer than the uber-shader" call.

## P2-C FINDING — the BUILD-COST WALL (Codex predicted this) (2026-06-11)
- At coarse 16u leaves the DAG is a DOWNGRADE vs the (decent) quality-mode baseline: blocky + its
  terrain horizon is lower than the baseline's far analytic mountains. The DAG only WINS at FINE res.
- Made leaf floor env-configurable (VENPOD_FARVOXEL_LEAF_FLOOR + VENPOD_FAR_SVO_MAX_DEPTH; cache
  filename now includes _lf). leafFloor in FarVoxelOctreeConfig, used in BuildNodeInto.
- floor=4/depth8 cold CPU build did NOT finish in 4.5 min (no "octree initialized") -> the full fine
  intermediate TREE build is the bottleneck (the DAG compresses the RESULT, but BuildCpuData's
  recursive per-page tree build over 625 pages is too slow at fine res). This is the build-cost wall.
- => Making the DAG a net win needs either (a) a faster/GPU tree build, (b) building the DAG DIRECTLY
  at fine res without the full intermediate tree (what HashDAG/GPU-SVDAG-Editing do), or (c) a
  moderate floor (testing floor=8/depth7 now) that builds tractably. The render side is solved; the
  fine-res BUILD is the next hard problem.
- ALSO: in 60fps perf mode (background-split 0.3 + foreground 0.5 = where the user's holes/giant
  blocks actually live) the DAG composites to an intermediate resolved separately -> invisible there.
  P2-D = wire the DAG into the perf-mode target flow. (Quality-mode baseline is decent, which
  localizes the user's reported problems to the perf path.)

## *** STRATEGY PIVOT — the problem is SHADING, not geometry (2026-06-11) ***
Human goal (explicit): the end goal is good terrain VISUALS + PERF, NOT SVDAG. If the empirical
result isn't good enough, go back to the SOTA drawing board. ALL verification at ORIGINAL/native res.

CRITICAL PROCESS FIX: I'd been judging DOWNSCALED captures (the Read tool shrinks 1920x1080 -> ~600px,
hiding the artifacts). Tiling the native 1920x1080 into 1:1 640x540 crops revealed the TRUE problem.

NATIVE-RES GROUND TRUTH (quality mode, DAG off): the dominant ugliness is a MILKY WHITE/GREY HAZE
BAND washing out the mid/far + flat hard-edged "billboard" far mountains + the "static-TV" look. This
is SHADING/COMPOSITE, NOT a geometry-LOD failure.

CONVERGED (Claude + fresh Codex, both independent, grounded in code):
- Milky band = far hits hazed toward sky 2-3x (DOUBLE/TRIPLE blend): per-hit far SVO haze
  PS_Raymarch.hlsl:3506-3509 (fogFactor*0.60 + horizonHaze*0.36 + 0.16 FLOOR), far heightfield
  3675-3681 / 3793-3799, the SkyColor horizon-air band 543-551, THEN a redundant SECOND atmosphere
  blend 5354-5372 (layerWeight 0.24/0.18). Stacked = the washout.
- Blocky "static-TV" = PS_BackgroundComposite.hlsl point-samples (Load, line 30) instead of using the
  declared linear BackgroundSampler -> the 0.3-scale background (~576x324) HARD nearest-upscales to
  1080p. The linear sampler was declared but unused.
- Far "billboards" = mostly far HEIGHTFIELD + over-haze (low-alt horizon rays defer SVO to heightfield
  at 5105-5135), NOT missing SVDAG geometry. => SVDAG would NOT fix the dominant artifact; it'd risk
  trading grey haze for blocky box-leaves.

VERDICT: PARK the SVDAG (kept, gated). Fix the SHADING first (cheap, high-impact, targets the actual
symptom). Then reassess geometry only if it's still the limiter.

FIXES APPLIED (shaders, runtime-compiled, DAG-independent, affect the REAL render):
1. PS_BackgroundComposite.hlsl: Load -> SampleLevel(BackgroundSampler, uv) = linear upscale (kills
   perf-mode blocky static-TV).
2. PS_Raymarch.hlsl 5367-69: cut the redundant 2nd atmosphere blend ~3x (0.24/0.18 -> 0.08/0.06,
   farContext 0.10 -> 0.035) = kills the milky double-wash; the first tuned haze still carries it.
NEXT LEVER if still milky: reduce per-hit far haze floors (the +0.16 at 3509, +0.20 at 3677/3795,
and the 0.60/0.36 weights). VERIFY every change at native 1:1 crops, quality AND 60fps perf.

## *** RESOLVED — the real "far from coherent" was the MID-PASS being OFF (2026-06-11) ***
The dominant failure (user: "FAR from coherent", "make a real capture harness") was NOT haze and NOT
geometry. Built capsheet.ps1 (capture->contact-sheet harness, full frames across motion in the user's
60fps mode). It revealed: mid slopes render as flat-lit VERTICAL-STRIPE CARDBOARD WALLS.
ROOT CAUSE: VENPOD_RAYMARCH_MID_PASS_ENABLE defaulted to 0 (main_launcher.cpp:1042), so the engine's
proper mid-voxel DDA renderer (the mid pass, which has real form shading) was OFF -> 60fps fell back
to the flat column-proxy (MakeMidVoxelColumnClipmapHit normal=(0,1,0)) in the cliff'd uber-shader.
FIX (shipped): default -> 1u (rebuilt). Mid now renders coherent cubic voxel terrain w/ form. Verified
coherent across 9-frame walk+scan + aerial + the plain default, at 75-95 fps. Also added the
form-shaded column to the mid-only pass (#ifdef RAYMARCH_MID_ONLY -> MidClipmapNormal), off the cliff.
LANDMINES CONFIRMED: (1) PS_Raymarch.hlsl uber-shader is at the NVIDIA JIT/PSO cliff (0x-7FF8FFF2) AND
~3-7min to recompile (8.7MB) -> mid/far shading fixes MUST go in the separate mid-only pass (~1s
compile). (2) The SVDAG overhaul (parked behind VENPOD_FARVOXEL_DAG) was a WRONG TURN for this problem.
REMAINING (honest): mid is coherent but BLOCKY/large voxels (lower-res than near) -> fineness is a
separate follow-up. User is final judge: rebrun -PerfMode 60fps -NoBuild.

## 2026-06-11 (cont.) — Mid "smooth approximation" = column proxy winning; DDA per-ray miss

**Debug 69 added (user's idea: "use debug to nail it"):** mid-only pass cause-map.
GREEN = real DDA voxel hit · ORANGE = DDA passed global gates but this ray's march missed (smooth
column took over) · RED/BLUE/MAGENTA/CYAN = a specific global early-out fired
(bit4-off / residency / budget-quality / header). Lives in `#ifdef RAYMARCH_MID_ONLY` → fast PSO.

**First capture (debug 69, 150u above, pitch -26):** the ENTIRE mid band is a uniform slab of the
smooth column proxy. Near (bottom) + far (horizon) render fine; the mid is 100% column.

**TANDEM (partner: codex) → CONVERGED on the mechanism, DIVERGED on the fix framing.**
Codex independently traced the yellow to `RaymarchMidVoxelColumnClipmap` winning whenever
`RaymarchMidVoxelClipmap` (DDA) returns false, and flagged 3 DDA-miss reasons + 1 architectural point:
(1) height-tile-exists-but-3D-brick-absent, (2) DDA starts at midStart(768) not at the actual
surface crossing — no pre-mid surface recovery like the column has (2541-2572), (3) steep-downward
rays hit the rayDir.y clamp. Architectural: the mid-only overlay is *allowed* to paint a smooth proxy
even in voxel-terrain-only mode, where the main background path (4967-4969) disables it. Codex's first
move: make the mid-only pass transparent-on-DDA-miss instead of drawing the column.

**EMPIRICAL (venpod_runtime.log, above-view):** midCov=1.00/1.00, midVoxels=16384 resident,
midVoxInterest=12288/12288, bgQuality=1.00, exactNear=1024, midStart=768, midEnd=9000. → bricks ARE
resident, quality full; the global early-outs should NOT fire. So the miss is **per-ray inside the
march/acceptance logic** (2014-2093), NOT residency/budget. The debug-69 cause-map re-capture will
confirm ORANGE (per-ray miss) vs a flat global-gate color. Then: fix the DDA acceptance so it covers
the mid → kill the column.

## 2026-06-11 (cont.) — ROOT CAUSE FOUND: mid-voxel metadata header never uploaded under GPU-gen

**Debug-69 cause-map result:** the entire mid band painted CYAN = the header/midField early-out
(PS_Raymarch.hlsl:1946) fires on EVERY ray. midFieldParams.x=1.0 (height column works), so it's the
mid-VOXEL clipmap GPU metadata header that's invalid (magic 0x56435658 missing).

**TANDEM (partner: codex) → CONVERGED on the fix; ground-truth resolved a real DIVERGENCE.**
I read enableGpuMidVoxelGeneration's C++ default = 0 (OFF) and argued GPU-gen wasn't active. Codex
read the runtime and assumed GPU-gen ON (samples empty by design). The contradiction was the bug's
hiding spot. Ground truth settled it: venpod_runtime.log prints "[MIDGEN] GPU mid-voxel sample
generation ENABLED", and rebrun.ps1:756-757 FORCE-sets VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION=1 by
default. So GPU-gen IS on in every rebrun run (incl. the user's gameplay) -> Codex's runtime read won.

**Mechanism:** under GPU-gen, BuildGpuSnapshot emits ZERO voxel sample ranges (samples come from the
compute dispatch) -> snapshot.voxelSamples is empty. But the upload gate
(SparseVoxelGpuResources.cpp:851 and :2090) required !voxelSamples.empty() to upload the voxel layer,
so it suppressed the ENTIRE voxel-layer upload -- including the CPU-built metadata header + lookup.
The header therefore never reached m_midVoxelClipmapMetadata -> RaymarchMidVoxelClipmap bailed at the
header check on every ray -> smooth height column won every mid pixel. Corroborated by midVoxelUpload=0
on every frame while the height layer uploaded fine (midUpload=473).

**FIX (CPU, both gates + memcpy guard):** drop the !voxelSamples.empty() requirement from the voxel
upload gates so metadata+lookup upload even with zero samples; guard the voxel-sample memcpy on
voxelSampleBytes>0. Aligns with the snapshot comment "metadata+lookup are still CPU-built and uploaded
as usual." Next: build + debug-69 recapture -> expect CYAN -> GREEN (real voxel DDA hits).

## 2026-06-11 (cont.) — Fix VERIFIED alive; attacking the ORANGE (per-ray DDA miss)

Upload-gate fix CONFIRMED visually (above-view, native): debug-69 mid went 100% CYAN -> GREEN+ORANGE.
GREEN = real voxel DDA hits (the mid-voxel renderer is ALIVE for the first time); a non-debug capture
shows real blocky voxel terrain in the green regions. ORANGE = DDA ran but missed that ray -> smooth
column proxy won (still-smooth band, the remaining target).

**TANDEM (partner: codex) → on the ORANGE:** Codex traced the steep-downward orange to the
coarser-parent fallback gate (PS_Raymarch.hlsl:2036) being narrower (rayDir.y > -0.58) than the DDA's
accepted downward range (minAllowedRayY = -0.68). Applied Codex's fix: widen fallback to
rayDir.y >= minAllowedRayY. CLAUDE CROSS-CHECK / partial DIVERGENCE: the green/orange boundary in the
user's native shot is ~rayDir.y -0.34, so Codex's gate only addresses the steepest orange; the bulk
(-0.34..-0.58) likely misses because the DDA starts at midStart=768 while those steeper rays hit
terrain CLOSER than 768u (march starts past the surface). Plan: capture gate-fix alone, then
gate-fix + lowered VENPOD_SPARSE_MID_START to isolate the start effect. Empirical ablation, not theory.

## 2026-06-11 (cont.) — Streaming catch-up (user: "air before renderer catches up")

After the mid renders real voxels, user reports the mid-voxel terrain STREAMS IN too slowly during
motion -> visible "air" band until it catches up. Evidence: mid-voxel generation is budget-throttled.
GPU-gen is cheap and midVoxelUpload=0 (only tiny metadata), yet steady-state coverage-catchup defaulted
to 48 bricks/frame (main_launcher.cpp). The reproducible deficit is the startup/descent ramp
(coverage 0.62->1.00 over ~80 frames at gen=192). capsheet walk-test does NOT reproduce in-flight
deficit (coverage stays 1.00 after convergence) -> the USER is the in-flight verifier.

FIX (built): raised VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET 48->256, clean/backlog tail caps
24/32->96, startup 192->384. Plan: measure startup-ramp fills faster (fewer frames to coverage 1.00)
+ fps holds; then user verifies in-flight. Codex context was blown (4.3M) and its last 'wait' returned
a STALE verdict -- reset to a fresh thread.

## 2026-06-11 (cont.) — Streaming: budget was the WRONG lever (tandem caught it), REVERTED

**TANDEM (partner: codex) → DIVERGED from my gen-budget approach; data confirmed Codex.**
I raised the mid-voxel gen budgets (coverage 48->256, clean/backlog 24/32->96, startup 192->384) to
speed catch-up. EMPIRICAL: the startup/descent ramp stayed pinned at gen=192 (a different cap -- the
motion-pump-burst floor, main_launcher.cpp ~12714) and steady fps DROPPED 66->46. Codex independently
found why: BuildGpuSnapshot (SparseClipmap.cpp:6053-6083) re-emits a GPU-gen request for EVERY resident
GPU brick (~12k) on EVERY metadata upload -- so a higher catchup budget just triggers more full
re-dispatches -> fps loss, no catch-up gain. Also: mid interest is pinned at 12288 (16384*75% cap) and
the predictive-visible-admission prefetch is OFF by default -- so "air ahead of motion" is partly an
interest-admission problem, not gen-rate.

ALL FOUR BUDGET CHANGES REVERTED to 48/24/32/192. The REAL enabler is fixing the full re-dispatch
(only re-gen NEW/dirty bricks, not all 12k) -- that restores fps headroom AND frees GPU to gen new
terrain faster. That's the next step, but it carries regression risk on the just-resurrected DDA, so it
needs careful verification. capsheet CANNOT reproduce the in-flight air (coverage stays 1.00 after
convergence) -> the USER is the in-flight verifier.
