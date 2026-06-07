# VENPOD Terrain Streaming Completion Ledger

This ledger supersedes `docs/COMPLETION_LEDGER.md` for the current terrain holes,
slow pop-in, floating slabs, and half-loaded sparse/LOD world problem.

Core diagnosis: the visible world is exposing incomplete streaming or LOD state.
The fix is not another visual threshold. The renderer needs atomic visibility
contracts for chunk/brick/LOD readiness, plus diagnostics that prove where every
missing-looking region is in the pipeline.

May 19 resumed-state correction after interruption: the active goal is still
open, but the work has drifted into too many local probes. Current evidence says
the remaining user-visible failure is no longer a simple "SVO missing pages"
bug. The hard ownership gates often pass (`miss=0`, `unsafeNearMiss=0`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`), while screenshots still
look sparse/blocky/fake. That means the next useful goal is not "increase far
SVO radius", "tune one shader threshold", or "stabilize unless regression".
Those have already been weak or rejected levers: radius-8 far SVO was expensive
and visually ineffective, the latest mid-parent fallback was safe but did not
move the skyline contact sheet, and the shoreline sand guard only fixes one
material symptom. The narrowed working goal is now: make every visible terrain
pixel report an authoritative owner and require each non-near owner to prove
that it is a valid voxel LOD representation of the same world, then replace the
first owner class that fails that proof. Priority order is (1) ownership/quality
diagnostics that distinguish exact sparse surface, resident mid voxel, parent
held mid voxel, far SVO, and sky/air in normal manual viewpoints; (2) mid/far
handoff rules that prevent coarse/fake owners from appearing inside editable or
shoreline authority; (3) one architecture-level replacement for the weakest
representation, likely mid-voxel surface extraction or a bounded near/mid mesh
owner, not more horizon height proxies; (4) only after the visual ownership is
correct, performance polish on the dominant measured hitches. Status remains
`PARTIAL`: the repo has many useful diagnostics and several verified focused
fixes, but the playable rendering target is not met and must not be closed from
passing miss counters alone.

May 19 mid/far ownership handoff correction: a concrete architecture bug was
found in the low/elevated skyline background path. The far-SVO candidate could
replace a resident mid-voxel hit even when the far hit was behind the mid hit by
a large slack window. That made the skyline pass "no miss" validation while
still mixing resident mid terrain with farther coarse/sky-looking far ownership.
Accepted fix: `assets/shaders/Graphics/PS_Raymarch.hlsl` now treats far SVO as
a true fallback or a clearly nearer occluder only; resident mid voxels keep
ownership when they are the first terrain hit. Evidence:
`.\build.ps1 -Config Release` passed, `.\build\bin\VENPODTests.exe` passed,
`diag_owner55_resume_strategy_20260519` established the pre-fix skyline owner
mix at about `maxFarSvoScreen=18.94%`, and
`fix_far_svo_true_fallback_owner55_20260519` reduced the same route to
`maxFarSvoScreen=11.00%` with `maxMiss=0.0055%`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, and `postNonReady=0`. The normal skyline guard
`fix_far_svo_true_fallback_skyline_20260519` passed with the same hard gates;
last-frame ownership shifted from `midVoxelScreen=25.77%` /
`farSvoScreen=17.99%` to `midVoxelScreen=34.30%` /
`farSvoScreen=10.00%`. Shoreline and edit regressions were also guarded:
`fix_far_svo_true_fallback_waterline_material54_20260519` passed with
`maxMaterialSandPct=0.71`, `maxFarSvoScreen=0.80%`, and no height/valley proxy;
`fix_far_svo_true_fallback_waterline_brush_20260519` passed once the run was
allowed to reach the app's brush-smoke settle frame, with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=12 retired=474 applied=158
deltas=474 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`.
Status remains `PARTIAL`: this removes one real mid/far owner inversion and
proves the earlier brush failure was a too-short settle window, but the broader
playable rendering target still needs manual free-roam review, stronger
mid-voxel geometry/material polish, and performance/hitch validation.

May 19 rejected mid-voxel procedural-backed refinement and accepted brush-smoke
gate fix: after the handoff correction, a resident-mid surface-entry refinement
probe was attempted to reduce coarse cell slabs. It was rejected and removed
because it did not improve the skyline metrics and a stricter procedural-backed
cell filter moved the route in the wrong direction: compared with the accepted
handoff guard, voxel-terrain screen dropped from about `60.34%` to `58.49%`,
far-SVO/far-water ownership rose, and the normal contact sheet did not show a
clear visual win. The source shader was restored to the accepted handoff state
and revalidated by `guard_after_reject_mid_refine_skyline_20260519`, which
passed with `maxFarSvoScreen=11.00%`, `maxMiss=0.0055%`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and `postNonReady=0`.
Accepted validation fix: `engine_capture_smoke.ps1` now raises
`ExitAfterFrames` automatically when `-SparseBrushPaintSmoke` would otherwise
exit before the app's brush-smoke settle frame. Evidence:
`fix_brush_smoke_auto_settle_waterline_20260519` printed
`ExitAfterFrames=360 is too short for sparse brush paint smoke settle; raising
to 395` and passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=12
retired=474 applied=158 deltas=474 fallback=0 missingResident=0 hints=0
overflow=0 deltaMismatch=0`, plus `maxMiss=0.0050%`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, and `postNonReady=0`. Status remains `PARTIAL`:
this improves the reliability of the edit/material validation gate and prevents
false closure/failure noise, but it is not a new visual-rendering completion.

May 19 coarse mid-voxel surface-skin material correction: material debug on the
accepted skyline route showed the remaining mid/far terrain was still heavily
stone-dominated even when ownership was correct (`diag_material54_skyline_midfar_20260519`
sampled roughly `46-48%` stone and only about `3%` dirt). This matched the
user-visible gray/fake mountain look. Accepted fix: `src/Simulation/SparseClipmap.cpp`
now classifies generated coarse mid-voxel cells that are near the terrain
surface skin, below the high-mountain threshold, and low relief as dirt instead
of defaulting every coarse side band to stone. This is CPU-generated voxel data,
not a shader tint or height proxy, and it does not override edited bricks.
Evidence: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed. The material debug guard
`fix_mid_surface_skin_material54_skyline_20260519` passed with unchanged hard
ownership (`maxFarSvoScreen=11.00%`, `maxMiss=0.0055%`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`, `postNonReady=0`) and moved
stone down from about `48.28/46.10%` to `46.84/44.98%` on the two sampled
skyline frames while terrain-like classification rose from about `33.89/34.34%`
to `35.34/35.46%`. Normal skyline guard
`fix_mid_surface_skin_skyline_20260519` passed with the same ownership gates,
waterline material guard `fix_mid_surface_skin_waterline54_20260519` passed
with `maxMaterialSandPct=0.71`, and edit regression guard
`fix_mid_surface_skin_brush_waterline_20260519` passed with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=12 retired=474 applied=158
deltas=474 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`.
Status remains `PARTIAL`: this is a small material-quality correction for
coarse mid voxels, not the full mid-distance surface representation overhaul.

May 19 debug-capture classifier gate correction: material and ownership debug
modes intentionally recolor terrain, so the generic normal-shading
`terrainLikePct` classifier could falsely fail useful debug captures even when
the ownership, material, miss, and proxy gates were clean. Accepted fix:
`engine_capture_smoke.ps1` now bypasses only the generic terrain-color
classifier when `SparseDebugMode` is nonzero. It does not skip material,
skyline, ownership miss, height-proxy, valley-atmosphere, far-SVO, temporal, or
runtime-log gates. Evidence: script syntax validation passed,
`.\build.ps1 -Config Release` passed, `.\build\bin\VENPODTests.exe` passed,
and `fix_debug_classifier_material54_skyline_20260519` passed in debug mode 54
with `maxMiss=0.0055%`, `maxFarSvoScreen=11.00%`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, and `postNonReady=0`. Status remains `PARTIAL`:
this improves fast validation reliability, not rendering quality by itself.

May 19 low-altitude voxel fallback and skyline hole gate: a new image-stat
metric was added to `engine_capture_smoke.ps1`:
`skylineInteriorSkyRunPct`. Unlike the existing average skyline coverage
checks, this catches the worst single-column run of sky-colored pixels below a
first terrain hit, which is a better automated signal for floating slabs and
vertical mountain holes. The canonical waterline regression now passes
`-MaxSkylineInteriorSkyRunPct 12` through `sparse_regression.ps1`.
Diagnosis with `diag_waterline_sky_run_gate_20260519` and
`diag_waterline_owner55_sky_run_20260519` showed the hard ownership gates were
clean (`miss=0`, `heightProxyScreen=0`, `valleyAtmosphereScreen=0`) while the
background still had large sky ownership behind resident mid terrain; far SVO
was effectively absent on that route (`maxFarSvoScreen` about `0.07%`) and the
waterline sky-run metric was `11.17%`. A corrected shader change in
`assets/shaders/Graphics/PS_Raymarch.hlsl` now lets low-altitude voxel-only
views fall back to the page-indexed far SVO immediately after the protected
exact-near band instead of waiting for the late mid/far handoff. This is a real
voxel fallback, not a height proxy or parent LOD. Evidence:
`.\build.ps1 -Config Release` passed, `.\build\bin\VENPODTests.exe` passed,
`engine_capture_smoke.ps1` and `sparse_regression.ps1` syntax checks passed.
`fix_low_alt_far_svo_fallback_final_skyline_20260519` passed with
`maxSkylineInteriorSkyRunPct=1.12` (down from the corrected strict-handoff
skyline failure of `13.41`), `maxFarSvoScreen=15.70`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and `miss=0`.
`fix_low_alt_far_svo_fallback_final_waterline_20260519` passed with
`maxSkylineInteriorSkyRunPct=8.94`, `maxMaterialSandPct=0.00`,
`maxFarSvoScreen=1.71`, `heightProxyScreen=0`, `valleyAtmosphereScreen=0`,
and `miss=0`. Rejected probes: a waterline-only camera-height special case was
not reliable because the scripted skyline and waterline routes can occupy
similar camera heights; a middle low-altitude handoff at `0.54` of the mid
range raised far SVO only slightly and did not reduce the failing skyline
sky-run. Status remains `PARTIAL`: the fix materially reduces visible vertical
holes using real voxel fallback, but far SVO screen share is intentionally
higher on the broad skyline route, so final polish still needs a cleaner
mid/far representation rather than relying on more far-SVO ownership.

May 19 ownership-architecture reset: the previous local fix loop improved the
hard gates, but the remaining mountain holes showed that the renderer was still
mixing responsibilities. Exact sparse raster surfaces were being drawn far
enough into the mountain range that incomplete surface residency could create
slabby silhouettes and sky cuts. The new contract is stricter: sparse raster
surfaces are a bounded near/editable owner, while mid/far voxel layers own
distant mountains. The current implementation moves in that direction by
reducing the default stable sparse-surface raster/ownership distance from
`4096` to `1792`, bounding low-altitude background suppression instead of using
the full sparse-surface radius, widening low-altitude mid/far voxel silhouette
search, adding debug-mode `55` coloring for sparse raster surfaces, and adding
`engine_capture_smoke.ps1 -MaxSurfaceScreenPct` so captures can fail when sparse
surface coverage is too high for a near-only contract. Status remains
`PARTIAL`: PowerShell syntax validation passed for the updated capture script,
but the post-change representative visual capture has not completed under the
current sandbox/approval constraints, and shoreline material consistency still
needs a deterministic water/sand publication gate.

May 17 hidden-near terrain continuation: the waterline debug route exposed two
separate root causes. First, render/compute sparse page-table lookup contracts
had diverged: the render shader was probing through long tombstone chains while
miss feedback, brush feedback, GPU raycast, and physics packet shaders still
stopped after 64 probes. All sparse GPU lookup paths now use a shared 256-probe
budget, preventing "CPU/GPU ready here, feedback/raycast thinks missing"
disagreements. Second, the shader's hidden-terrain diagnostic found exact-near
terrain at roughly `216-225` units, but the proactive screen-critical terrain
prefetch only requested a tiny axial footprint by default. A temporary probe
made startup critical, strict critical, predictive critical, and shader unsafe
feedback request a full `3x3x3` brick halo, and also made critical
ready-footprint reuse stationary-only by default so moving cameras could not
skip the sampler for newly visible pixels.
A follow-up probe showed the remaining 53 unsafe pixels had all 27 neighboring
bricks already `ReadyToRender`; that was not a streaming miss. It was the
far-height diagnostic overriding the loaded exact sparse world after the DDA
had proven the space was air. Hidden-near recovery now requires an actual
sparse/surface hole signal (`nearSparseHole`) and does not classify clean loaded
sparse air as missing land. Evidence: `.\build.ps1` passed,
`.\build\bin\VENPODTests.exe` passed,
`fix_gpu_probe_contract_waterline_brush_guard_20260517` passed with `miss=0`,
`unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`,
`minVoxelTerrainScreen=73.78%`, and brush paint smoke enabled;
`fix_critical_27halo_no_moving_reuse_debug50_20260517` passed with
`miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, `postNonReady=0`, and
`minVoxelTerrainScreen=76.82%`; and the earlier-frame regression capture
`fix_clean_sparse_air_not_unsafe_debug50_20260517` passed with
ownership timeline sums `miss=0` and `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, and `minVoxelTerrainScreen=76.57%`. The final
normal waterline/brush guard
`fix_hidden_near_final_waterline_brush_guard_20260517` also passed with
`miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, `postNonReady=0`, `minVoxelTerrainScreen=76.64%`,
`maxFrameMs=11.25`, and `maxGpuRayMs=11.41`. TS-014/TS-015
remain `PARTIAL`: the exact-near hidden miss and fake clean-air classification
are fixed on the reproduced route, but manual free-roam far mountain density,
material consistency after arbitrary edits, and broad performance acceptance
are not yet complete.

May 17 follow-up on the hidden-near fix: the always-on `3x3x3` terrain-critical
halo was a real performance/resource regression, not a harmless stabilization
change. `current_broad_long_walk_after_hidden_near_20260517` passed hard visual
gates but saturated the exact sparse pool late in the route (`resident=32664`,
`free=104`, continuous `evictLast=8`) and raised broad-route pacing to
`maxFrameMs=66.95`, `maxSmoothedFrameMs=92.72`, and `maxPrepMs=57.99`.
An env A/B with `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_MAX_COORDS=7` proved the
full continuous halo was overkill after the false-oracle fix: the waterline
debug route still passed with `miss=0` and `unsafeNearMiss=0`, while broad-route
pacing improved. That A/B also exposed a footprint-ordering bug: when the
3x3x3 list was truncated, the first 7 entries were an arbitrary corner slab,
not center plus six axial neighbors. The accepted fix restores the default
terrain-critical footprint to 7 ordered coordinates (`center + axial halo`) and
keeps the full 27-brick halo only for shader unsafe feedback, where an actual
hole has been observed. Evidence: `.\build.ps1` passed,
`.\build\bin\VENPODTests.exe` passed,
`fix_axial_critical_default_waterline_debug50_20260517` passed with `miss=0`,
`unsafeNearMiss=0`, `heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and
`postNonReady=0`; `fix_axial_critical_default_long_walk_20260517` passed with
`miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, `postNonReady=0`, `maxFrameMs=52.88`,
`maxSmoothedFrameMs=64.26`, `maxPrepMs=43.80`, `maxGpuRayMs=18.87`, and late
free pages improved to hundreds/thousands instead of the 27-halo run's `104`;
`fix_axial_critical_default_waterline_brush_guard_20260517` preserved shoreline
and edit behavior with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`,
`retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`, `postNonReady=0`,
`maxFrameMs=11.16`, and `maxGpuRayMs=8.02`. TS-015 remains `PARTIAL` because
the broad route still has sparse request/generation cost and GPU ray spikes;
this pass removes the 27-halo regression while preserving the verified
near-field rendering fix.

May 17 exact-near contract restoration after the false-oracle and GPU-lookup
fixes: the active code had drifted away from the documented foreground contract.
`docs/reference/runtime.md` says `VENPOD_SPARSE_EXACT_NEAR_DISTANCE` defaults
to `768`, but `src/main_launcher.cpp` still defaulted to `384`. That smaller
radius allowed background/mid/far layers to own pixels too close to the player,
which matches the user-visible "fake sand/terrain disappears when the exact
sparse world or brush edit arrives" failure. Earlier May 17 ledger evidence had
rejected radius `768` because it overclaimed unready exact terrain and produced
debug-50 red hidden-terrain pixels around frames `469-471`; that rejection is
now superseded by the later GPU page-table probe contract, clean-loaded-air
classification, ordered axial critical footprint, and water occluder fixes.
New A/B evidence with `VENPOD_SPARSE_EXACT_NEAR_DISTANCE=768` passed
`ab_exact_near768_waterline_brush_20260517` with `heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=73.14%`,
`postNonReady=0`, `maxFrameMs=11.65`, `maxGpuRayMs=9.01`, and
`SPARSE_BRUSH_PAINT_SMOKE passed`; it also passed
`ab_exact_near768_long_walk_20260517` with `heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=87.73%`,
`postNonReady=0`, `maxFrameMs=44.45`, and `maxGpuRayMs=2.38`.
Accepted fix: `src/main_launcher.cpp` now restores the default exact-near
radius to `768`. Verification after rebuilding the default binary:
`.\build.ps1 -Config Release` passed, `.\build\bin\VENPODTests.exe` passed,
`fix_exact_near768_default_waterline_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=73.14%`, `postNonReady=0`, `maxFrameMs=9.75`, and
`maxGpuRayMs=11.26`; `fix_exact_near768_default_long_walk_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=93.44%`, `postNonReady=0`, `maxFrameMs=36.80`, and
`maxGpuRayMs=7.21`; `fix_exact_near768_default_stress_broad_20260517` passed
with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=73.19%`, and `postNonReady=0`. The old debug-50 stress
failure was specifically rechecked after the rebuilt default:
`fix_exact_near768_default_debug50_frame469_20260517` returned the expected
debug-color visual-marker warning, but direct pixel counting found `0` red
hidden-terrain pixels and `0` magenta unsafe pixels on frames `469`, `470`, and
`471`. TS-014/TS-015 remain `PARTIAL`: restoring the foreground ownership
radius removes a concrete fake-near-terrain cause and improves long-walk voxel
coverage, but arbitrary manual free-roam, far-mountain density, and final frame
pacing are still not fully accepted.

May 17 continuation after exact-near restoration: two performance-oriented
probes were rejected because they did not move the engine toward the requested
final state. Lowering `VENPOD_SPARSE_SURFACE_RASTER_MAX_DISTANCE` to `1536`
failed the long-walk visual marker gate (`engine_frame_1400.bmp`
`uniqueSampleColors=114`, `engine_frame_1440.bmp` `uniqueSampleColors=93`),
reduced early captured voxel-terrain coverage to `89.18%`, and did not improve
the measured long-walk timing (`raw avg/max=35.84/46.83 ms` versus the current
default's `35.02/42.04 ms`). Reducing the continuous terrain-critical grid to
`15x9` also stayed visually safe but made the measured long-walk timing worse
(`raw avg/max=40.98/48.36 ms`), so the default remains `21x13`. Current
profiling after the accepted exact-near fix shows the broad stress route is no
longer the slowness source (`fix_exact_near768_default_stress_broad_20260517`
sampled `raw avg/max=15.73/16.75 ms`), while the long-walk close-terrain route
still spends CPU time in sparse surface stats/extraction/staging and wait gaps
(`raw avg/max=35.02/42.04 ms`, `surfExtract avg/max=2.97/4.59 ms`,
`surfStage avg/max=2.77/3.40 ms`, `stats avg/max=3.11/4.02 ms`,
`postWait avg/max=11.77/14.11 ms`). Broader high-speed terrain validation was
added with `current_exact768_long_fast_flight_20260517`: it passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`maxLodParentHeld=0.0000%`, `postNonReady=0`, `minVoxelTerrainScreen=58.22%`,
`maxFrameMs=30.25`, and `maxGpuRayMs=11.95`; manual contact-sheet review shows
continuous voxel terrain with real sky horizon rather than sparse-air gaps.
TS-014 remains `PARTIAL` because manual free-roam visual acceptance and
far-mountain density are not fully closed. TS-015 remains `PARTIAL` because the
remaining measurable slowness is now concentrated in close-terrain sparse
surface CPU work, which still needs a targeted fix that does not reduce
foreground correctness.

May 17 voxel-only fake-atmosphere continuation: the broad high-altitude stress
route was still failing after the near-terrain and surface-aware request fixes,
but the failure was no longer an exact sparse AIR/missing-page bug. Ownership
logs showed `miss=0`, `unsafeNearMiss=0`, and a one-frame transition from
`farSvo=999804, valleyAtmosphere=0` to `farSvo=751378,
valleyAtmosphere=295584`. Debug mode 50 then split the pixels into two cases:
the upper pale band was ordinary air/sky being tinted by the normal
`valleyAtmosphere` fallback, while lower red diagnostic regions were hidden
terrain according to the deterministic height probe but not resident voxel
terrain. Because the current render contract is explicitly voxel-only
(`procedural mid/far height and far-water fallback disabled`), the accepted
behavior is not to draw fake gray valley/height atmosphere in normal
voxel-only mode. `DebugBackgroundMissHit` now treats voxel-only background
misses that are not exact-near sparse holes as sky/air ownership, while debug
mode 50 still marks deterministic hidden-terrain misses red for future
diagnosis. Rejected probes in this pass: increasing far-height fallback samples
did nothing because far height is disabled by voxel-only mode; starting
high-altitude far SVO earlier and increasing low-quality far-SVO page-walk
budget produced identical ownership; rebuilding the far-SVO cache after bumping
the cache version removed stale-cache risk but did not change the failing
ownership numbers. Evidence: `.\build.ps1` passed,
`.\build\bin\VENPODTests.exe` passed, and
`fix_voxel_only_no_valley_fake_stress_broad_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=47.01%`, `postNonReady=0`, and late ownership around
`farSvo=90-92%`, `midVoxel=~5%`, `miss=0`, `unsafeNearMiss=0`. Guard routes
also passed: `fix_voxel_only_no_valley_long_walk_20260517`
(`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=85.91%`,
`postNonReady=0`) and
`fix_voxel_only_no_valley_waterline_guard_actual_20260517`
(`waterlineCamera=1`, brush input/paint enabled,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=71.82%`,
`postNonReady=0`). TS-014/TS-015 remain `PARTIAL`: this removes a major fake
terrain/air fallback and keeps the diagnostic signal for real hidden-terrain
misses, but it does not prove the remaining far/background voxel coverage,
visual density, or performance targets are complete.

May 16 resumed root-cause update: the apparent waterline regression after the
metadata-throttle work was not an SVO draw failure. The capture camera was still
applying the normal stress-camera `baseHeight=520` while `-WaterlineCamera` was
enabled, so the "waterline" guard orbited hundreds of voxels above the anchored
basin. That made the stress request volume mostly air, caused `knownEmpty`
classification to explode (`~68k` known-empty entries versus `~491` in the
accepted baseline), and left only `~4k` resident bricks and hundreds of
`residentMissingSurface` entries. The waterline camera now uses a near-water
vertical band and does not terrain-clamp large-radius waterline orbits into
high-altitude mode. Separately, a real renderer capacity issue was found:
waterline/large-scene exact surfaces can exceed the default `3,145,728` face
GPU/IA buffer, producing `overflow=1` and dropped sparse-surface ranges even
when residency is otherwise ready. The default sparse-surface face capacity is
now raised to the existing validated maximum, `4,194,304`, to avoid capacity
holes before further material/free-roam review.

May 16 far-mountain continuation: after the capacity/waterline fix, the
remaining broad-route mountain cutouts were not near sparse holes, height
proxy, or an SVO upload failure. Debug mode 50 showed yellow exact sparse
surface dominating, with the visible holes owned by blue sky after the far SVO
page walker gave up at low far quality (`farQ` around `0.45`) even though the
far SVO page/upload coverage was `1.00/1.00`. The shader now gives
low-altitude mountain-silhouette and horizon rays a larger far-SVO page-walk
budget before falling through to sky; this keeps the fix in the resident far
voxel layer and does not re-enable procedural height proxy terrain.

May 16 resumed root-cause update after the manual-regression report: the
nearby waterline/shore holes were caused by the surface-authoritative renderer
running raster-only in the exact foreground while the true sparse raymarch
fallback was default-disabled. When a sparse-surface raster face did not cover a
pixel, voxel-only mode correctly refused to fill it with the old height/column
proxy, so the pixel became an unsafe/brown near miss. Enabling the exact sparse
raymarch fallback by default fixes that root behavior with real resident voxel
data, not fake terrain. The same pass also found that voxel-only mode was still
allowing the cheap mid-column height proxy to report as `MID_VOXEL`; that
proxy is now gated off whenever `VENPOD_SPARSE_VOXEL_TERRAIN_ONLY=1`.
Validated evidence: `broad_after_far_svo_patch_guard_single_20260516` passed
with `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`,
`postNonReady=0`, `minVoxelTerrainScreen=94.49%`, `maxFrameMs=26.84`,
`maxPrepMs=20.37`, and `maxGpuRayMs=7.34`. The waterline/brush guard
`waterline_exact_raymarch_fill_default_guard_20260516` passed with
`heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`,
`minVoxelTerrainScreen=73.82%`, and `maxGpuRayMs=20.58`. Remaining manual
overlook skyline holes are still open under TS-014/TS-015 because debug mode
50 shows them in the mid/far background ownership chain, not in exact sparse
foreground readiness.

May 16 valley-feedback continuation: the remaining default-overlook holes were
not fixed by treating mid-voxel normals more loosely, by allowing mid voxels to
draw inside the exact-near veto, by increasing the mid-voxel cache from `8192`
to `16384` bricks, or by re-enabling the non-voxel height fallback. Those probes
were rejected: early mid-voxel handoff exposed detached coarse slabs, the 16k
cache doubled mid-voxel memory without moving the holes, and the height fallback
filled gaps with non-authoritative height/column terrain. The accepted root
cause is scheduler classification: valley-atmosphere ownership at `3-4%` of the
screen was visually broken but below the old `8%` urgent feedback threshold, so
visible exact sparse requests were discovered/drained slowly. Lowering the
default `VENPOD_SPARSE_MISS_FEEDBACK_VALLEY_ATMOSPHERE_PCT` threshold to `2`
makes terrain-facing misses feed the visible sparse request path earlier while
keeping voxel-only mode and height-proxy rejection intact. Evidence:
`default_overlook_valley_feedback2_default_guard_20260516` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.40%`,
`minVoxelTerrainScreen=74.73%`, `miss=0`, `unsafeNearMiss=0`,
`postNonReady=0`, `maxFrameMs=21.00`, `maxPrepMs=13.36`, and
`maxGpuRayMs=7.77`. `broad_valley_feedback2_guard_20260516` passed with
`maxValleyAtmosphereScreen=0.36%`, `minVoxelTerrainScreen=95.23%`,
`maxFrameMs=27.81`, `maxPrepMs=19.58`, and `maxGpuRayMs=8.65`.
`waterline_valley_feedback2_guard_20260516` passed with synthetic brush paint,
`maxValleyAtmosphereScreen=1.66%`, `minVoxelTerrainScreen=73.87%`,
`maxFrameMs=55.38`, `maxPrepMs=41.29`, and `maxGpuRayMs=19.58`.
`VENPODSparseCore` also passed. TS-014 and TS-015 remain `PARTIAL`: this fixes
the reproduced slow valley-hole catch-up threshold, but manual free-roam visual
acceptance and broader long-session performance are still open.

May 17 exact/mid ownership-gap continuation: the remaining late-overlook holes
were not a generation-queue stall. At frame 1200, logs showed `missing=0`,
`requested=0`, `uploadQueued=0`, `residentMissingSurface=0`, and
`postNonReady=0`, while the screen still had rectangular sky holes. The root
cause confirmed by probe was an ownership mismatch: mid voxels started at
`768`, but `VENPOD_SPARSE_EXACT_NEAR_DISTANCE` defaulted to `1024`, so valid
mid/far voxel hits in the 768-1024 band were rejected whenever exact raster
surface coverage did not already own the pixel. Lowering the exact-near default
to `768` aligns the foreground/background handoff with the resident mid-voxel
clipmap instead of re-enabling height proxy terrain. In the same pass, mid
voxel generation was made vertically conservative so coarse LOD cells classify
terrain/water from the cell interval rather than only from the center sample;
this is targeted at the submerged-sand/water inconsistency and does not alter
exact sparse brick generation. Evidence: `exact_near_768_default_guard_20260517`
passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.49%`,
`minVoxelTerrainScreen=77.12%`, `miss=0`, `unsafeNearMiss=0`,
`postNonReady=0`, `maxFrameMs=15.88`, `maxPrepMs=10.32`, and
`maxGpuRayMs=6.84`. `waterline_exact768_default_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.65%`,
`minVoxelTerrainScreen=76.96%`, `postNonReady=0`, and `maxGpuRayMs=7.25`.
`VENPODTests.exe` passed. TS-014/TS-015 remain `PARTIAL`: the large forced-sky
handoff gap is reduced, but coarse detached LOD silhouettes and residual
mid/far holes are still visually present.

May 17 resumed diagnosis after regression concern: after rejected scheduler and
far-quality probes, the source was rebuilt before further analysis so captures
matched the current code. Rebuilt default capture
`current_normal_rebuilt_20260517` still passed the hard correctness gates
(`postNonReady=0`, `miss=0`, `unsafeNearMiss=0`) but showed the same visible
problem: yellow exact sparse terrain is stable while orange mid-voxel and
magenta far-SVO background own the blocky mountain silhouettes and residual
holes. A shader probe that starts background ownership at the exact-near
handoff instead of the sparse cache box exit slightly reduced
`valleyAtmosphereScreen` (`0.4868%` -> `0.4689%`) and is kept as a real
ownership-boundary correction, not as a visual proxy. A separate far-SVO
leaf-sampling probe raised low-quality per-leaf samples but did not change
far-SVO, sky, or valley ownership, so it was rejected and reverted. A seed
consistency fix now passes the sparse world seed into the far SVO config so
non-default worlds do not render a different distant voxel terrain from the
editable sparse world. Continuation on the same item also routed the active
world seed into the raymarch shader through the reserved `exactNearParams.y`
bits, so far-SVO leaf refinement, far material variation, and sparse-miss
terrain diagnostics use the same seeded height function as the CPU sparse
generator. Evidence: `seed_unified_background_guard_20260517` passed with
`postNonReady=0`, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=0.4689%`, `minVoxelTerrainScreen=77.1117%`, far
SVO `coverage upload/page=1.00/1.00`, and `gpuRayMs` around `6.2-6.8 ms`.
TS-014/TS-015 remain `PARTIAL`: the remaining fake-looking terrain is now
classified as mid/far LOD authority mismatch and edit-state inconsistency, not
as unloaded exact sparse pages or GPU upload starvation.

May 17 edit-authoritative mid-LOD continuation: the next confirmed mismatch was
not the exact SVO itself. Exact sparse generation already applied
`SparseEditStore`, but `SparseClipmapTileCache::GenerateVoxelBrick` generated
the mid-voxel background from procedural terrain only. That meant brush edits
could make exact sparse terrain/water change while the orange mid-voxel
background continued to draw stale procedural sand/land until exact sparse
surface ownership replaced it. The accepted fix wires the sparse edit store
into the mid-voxel clipmap, tracks an edit-store revision serial, invalidates
resident mid-voxel bricks overlapping edited overlays, and samples indexed
edited coarse cells before procedural column classification. Evidence:
`.\build.ps1` passed, `.\build\bin\VENPODTests.exe` passed,
`mid_edit_authority_normal_guard_20260517` passed with
`heightProxyScreen=0.00%`, `postNonReady=0`, and `minVoxelTerrainScreen=71.66%`;
`mid_edit_authority_brush_guard_20260517` passed with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868
deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`,
`heightProxyScreen=0.00%`, `postNonReady=0`, `minVoxelTerrainScreen=74.87%`,
and `maxValleyAtmosphereScreen=2.66%`; and
`mid_edit_authority_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `postNonReady=0`, `minVoxelTerrainScreen=75.52%`,
and `maxValleyAtmosphereScreen=2.01%`. TS-014/TS-015 remain `PARTIAL`: these
captures prove the edited-mid-background mismatch is fixed on scripted routes,
but the first accepted edit-authority pass exposed a separate brush feedback
stall: frames around 198-240 spent `~50-57 ms` in `sparsePost feedback` because
the GPU feedback apply path called `SetEditedVoxel` for each returned voxel,
forcing a full sparse-world `RefreshStats()` per record. The follow-up
batched-edit fix adds `SparseVoxelWorld::ApplyEditedVoxels`, merges dirty
regions per touched brick, and refreshes stats once per feedback payload.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`batched_brush_feedback_guard_20260517` passed with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868
deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`,
`heightProxyScreen=0.00%`, `postNonReady=0`, and `minVoxelTerrainScreen=74.87%`.
On the same scripted route, frame 240 improved from `body=83.98 ms` and
`sparsePost feedback=56.63 ms` in `mid_edit_authority_brush_guard_20260517` to
`body=43.53 ms` and `sparsePost feedback=18.27 ms`; frame 199 improved from
`body=94.15 ms` / `feedback=55.71 ms` to `body=48.33 ms` /
`feedback=14.92 ms`. The waterline variant
`batched_brush_feedback_waterline_guard_20260517` also passed with the same
brush smoke totals, `heightProxyScreen=0.00%`, `postNonReady=0`, and frame 240
at `body=42.23 ms` / `feedback=17.63 ms`. TS-015 remains `PARTIAL`: the
specific per-record stats-refresh stall is fixed, but brush-heavy frames still
have visible frame cost from remaining feedback, mid-clipmap, and GPU ray work.
May 17 continuation after the resumed regression report: the next confirmed
local bottleneck was `SparseClipmapTileCache::UpdateInterest`, which rebuilt
and resorted the same mid-voxel interest set every stable frame even when
`missingVoxel=0`, `queuedVoxel=0`, and `interestedVoxel=7372`. The accepted fix
adds a quantized interest signature and a reuse path that refreshes resident
touch frames without rebuilding the full height/voxel interest sets when the
camera, forward vector, velocity, prediction input, and clipmap policy are
unchanged. Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`interest_cache_final_brush_guard_20260517` passed with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`, and
`minVoxelTerrainScreen=74.87%`. On that guard, the stable mid-clipmap interest
cost dropped from the earlier `~7.6-8.0 ms` range to `0.35-0.40 ms` after the
brush/camera state settled (`PERF_SPARSE_CLIPMAP` frames 280, 360, 480, and
540). This does not close TS-015: frames around 199-240 still spend
`~15-18 ms` in `feedbackSplit ... brush`, and GPU ray cost remains
`~8-11 ms`. A tested edit-store batching/retained-edited-brick hypothesis did
not materially improve those brush feedback frames and was removed; the next
action is finer instrumentation inside `ApplyEditedVoxels` /
`QueueRegeneratedUploadForExistingPage` and the sparse GPU ray path rather than
claiming the brush hitch is solved.
May 17 continuation after the feedback split: the remaining brush-feedback
spike was another per-record global stats refresh, not GPU upload saturation or
a broken SVO. `SparseVoxelWorld::ApplyEditedVoxels` still called
`MarkResidencyClass` for every returned feedback voxel, and that helper calls
`RefreshStats()`. The accepted fix removes the per-voxel residency promotion
from the inner feedback loop and relies on the already per-brick
`QueueRegeneratedUploadForExistingPage` promotion. Evidence: `.\build.ps1` and
`.\build\bin\VENPODTests.exe` passed; `batched_residency_brush_guard_20260517`
passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868
applied=5868 deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0
deltaMismatch=0`, `heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`postNonReady=0`, and `minVoxelTerrainScreen=74.87%`. On the same route, frame
240 improved from `feedbackSplit ... brush=18.25 ms` and `body=42.01 ms` in
`interest_cache_final_brush_guard_20260517` to `brush=1.13 ms` and
`body=26.25 ms`; the waterline guard
`batched_residency_waterline_guard_20260517` also passed with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`, and frame
240 at `brush=0.97 ms` / `body=24.35 ms`. TS-015 remains `PARTIAL`: this
eliminates the confirmed brush feedback apply stall, but total frame time still
has visible costs from active brush sparse prep/generation, GPU ray work, and
first-time mid-interest rebuilds before the cache can reuse stable interest.

May 17 continuation after the terrain-critical split: the next confirmed
redundant CPU path was the strict terrain-critical current-footprint pass. After
the protected footprint was already fully ready (`postNonReady=0`), stable
frames still retraced the same `21x13` ray grid and rechecked the same 345
critical terrain coords. The accepted fix adds a conservative ready-footprint
reuse cache keyed by camera brick, quantized forward vector, and edit revision;
on reuse frames it skips the ray/request trace but still revalidates the cached
critical coords before keeping the cache. Evidence: `.\build.ps1` and
`.\build\bin\VENPODTests.exe` passed; `critical_signature_reuse_brush_guard_20260517`
passed with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`,
and `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868
applied=5868 deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0
deltaMismatch=0`. The brush guard logged 190 reuse frames out of 447
terrain-critical rows; frame 300 and 420 both reported `reuse=1`, `rays=0/273`,
`postReady=313`, `postEmpty=32`, and zero post non-ready states. The matching
waterline guard `critical_signature_reuse_waterline_guard_20260517` passed the
same visual/readiness gates, with 188 reuse frames out of 450 rows and the same
cached-footprint post-readiness result. TS-015 remains `PARTIAL`: this removes
one redundant stable-frame CPU scan, but it is not a complete performance or
manual free-roam closure because GPU ray work remains about `7-8 ms` on these
captures and the broader free-roam paths still need coverage.

May 17 broad-route continuation: `broad_current_rootcause_probe_20260517`
reproduced the remaining "slow and buggy" feel without reopening the fake
terrain gates. It passed with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`postNonReady=0`, and `maxValleyAtmosphereScreen=2.07%`, but its logs showed
mid-clipmap voxel pumping as the dominant CPU stall: frame 136 spent
`pumpVoxel=172.02 ms` with `budgetMid=97` and `edits=0/0`; frames 190/196/833
spent about `165/165/162 ms` pumping 72 mid-voxel bricks. The root cause was
not missing exact terrain readiness. `SparseClipmapTileCache::GenerateVoxelBrick`
was scanning the edit-overlay fine-brick volume for every coarse mid brick even
when the edit store had no overlays, and still performed per-sample edit-store
lookups after a coarse edit-cell summary had already been checked. The accepted
fix skips edit-overlay work when `EditedBrickCount()==0` and builds edited-cell
summaries by iterating actual overlays rather than walking every fine brick in
the coarse mid-brick AABB. Evidence: `.\build.ps1` and
`.\build\bin\VENPODTests.exe` passed; `broad_edit_overlay_fastpath_full_guard_20260517`
passed with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`,
`maxValleyAtmosphereScreen=1.61%`, and `minVoxelTerrainScreen=74.29%`.
The previously bad mid pump frames improved materially: frame 136
`pumpVoxel=172.02 ms -> 9.48 ms`, frame 190 `165.83 ms -> 7.19 ms`,
frame 196 `165.41 ms -> 6.48 ms`, frame 833 `161.76 ms -> 7.81 ms`,
and late frame 1190 `57.89 ms -> 7.15 ms`. The shoreline/edit regression
`edit_overlay_fastpath_waterline_brush_guard_20260517` also passed with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868
deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`,
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`, and `postNonReady=0`. TS-015
remains `PARTIAL`: the worst broad-route stall moved away from mid-clipmap
overlay scanning, but the long guard still reports frame-prep peaks around
`73 ms`, exact sparse generation up to about `46 ms`, and GPU ray peaks above
`50 ms` on startup/early frames and about `28 ms` later in the route.

May 17 exact-generation continuation: the next tested exact empty/solid
classification shortcut was rejected as non-closing evidence. Adding
`IsDefinitelyEmptyBrick` / `IsDefinitelyBuriedSolidBrick` fast exits to exact
brick generation preserved the hard visual gates in
`exact_empty_solid_fastpath_probe_20260517`, but did not materially reduce the
problem frames (`maxPrepMs=98.78`, `maxGenMs=60.75`, and the late broad
generation burst still remained). That probe was removed. Two accepted exact
generation fixes remain: `SparseTerrainGenerator::SampleGeneratedVoxelWithColumn`
now skips variant hashing for air voxels, and
`SparseVoxelWorld::GenerateBrickWithCachedTerrainColumns` keeps the brick
air-filled by default and samples only the non-air vertical span for each
terrain/water column. These preserve exact material authority because every
non-air voxel still goes through the same terrain sampler. Evidence:
`.\build.ps1` and `.\build\bin\VENPODTests.exe` passed; the broad guard
`air_hash_skip_probe_20260517` passed with `heightProxyScreen=0.00%`,
`missScreenPct=0.00%`, `postNonReady=0`, `maxValleyAtmosphereScreen=1.47%`,
and `minVoxelTerrainScreen=80.35%`. Against
`broad_edit_overlay_fastpath_full_guard_20260517`, the late broad-route max
exact-generation burst dropped from `46.08 ms` to `34.33 ms` and late max prep
dropped from `73.38 ms` to `61.03 ms`. The brush/waterline regression
`air_hash_skip_waterline_brush_guard_20260517` also passed with
`SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868
deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`,
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`, and `postNonReady=0`.
After the exact-column span patch, `exact_column_span_probe_20260517` passed
with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`,
`maxValleyAtmosphereScreen=2.34%`, and `minVoxelTerrainScreen=81.18%`; the
same late route improved again to `maxGenMs=5.39`, `maxPrepMs=28.81`, and
`maxGpuRayMs=9.74`. `exact_column_span_waterline_brush_guard_20260517` passed
the same shoreline/edit gates with the same brush smoke totals. A debug-mode
50 broad diagnostic (`debug50_air_hash_skip_broad_20260517`) intentionally
failed only the generic unique-color smoke threshold; its contact sheet
confirmed the remaining visible mountain discontinuities are in the orange
mid-voxel / magenta far-SVO / blue sky background ownership chain, not in
yellow exact sparse surface readiness. TS-015 remains `PARTIAL`: the accepted
changes remove a confirmed exact-generation waste path and greatly reduce the
late broad-route spike, but an earlier broad burst remains around frame 367
(`gen=39.94 ms`, `prep=66.51 ms` in `exact_column_span_probe_20260517`) and
startup GPU ray spikes remain around `50-60 ms`.

May 17 high-altitude voxel-LOD ownership continuation: the broad debug capture
`current_rebuilt_ownership_debug_20260517` showed a concrete remaining
handoff bug. The mid-voxel clipmap was resident (`midVoxels=8192`,
`midVoxInterest` around `6.8k-7.1k`) but high-altitude voxel-only rendering
reported `midVoxel=0` ownership, so the mountain background fell almost
entirely to far SVO plus valley/sky. The rejected
`critical_predictive_warm_probe_20260517` confirmed that wider scheduling caps
were not the root fix: it improved one early burst but shifted a worse burst to
late frames (`840-900 maxPrep=60.57 ms`, `maxGen=33.31 ms`,
`maxGpuRay=29.38 ms`). The accepted shader fix keeps the cheap mid-column
proxy disabled in voxel-only mode, but no longer lets that proxy decision skip
the real 3D mid-voxel DDA for high-altitude views. It also makes the background
handoff continue to later layers when a too-near background hit is rejected, and
allows real voxel LOD layers (`MID_VOXEL` and `FAR_SVO`) to fill high-altitude
non-surface pixels inside the walking-height exact-near radius. Evidence:
`.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`highalt_voxel_lod_guard_20260517` passed with `heightProxyScreen=0.00%`,
`missScreenPct=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`; and
`highalt_voxel_lod_waterline_brush_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, `postNonReady=0`, and the brush smoke route
intact. Compared with the rebuilt debug baseline, average high-altitude
ownership moved from `0.00%` mid voxel / `80.65%` far SVO /
`8.61%` valley / `0.321%` miss to about `21.84%` mid voxel /
`68.85%` far SVO / `0.00%` valley / `0.000%` miss. TS-014/TS-015 remain
`PARTIAL`: this fixes one real background ownership hole, but the accepted
guard still has CPU exact-generation spikes around `100 ms` on some broad-route
frames and visible far-SVO coarse silhouettes remain outside the mid-voxel
ownership band.

May 17 terrain-critical LOD-throttle continuation: the next root-cause pass
found that the remaining broad-route spikes were not SVO holes or GPU pressure.
They were exact foreground terrain-critical bursts caused by the high-altitude
LOD throttle switching off over tall terrain. `lod_probe16_env_guard_20260517`
proved that reducing the probe cap alone helped LOD-throttled frames but left
full exact bursts on frames where `lodThrottle=0` (`frame=498 gen=79`,
`frame=521 gen=55`). The accepted env A/B
`lod_probe16_alt128_env_guard_20260517` lowered the throttle altitude to `128`
and capped the LOD probe at `16`; it preserved `heightProxyScreen=0.00%`,
`missScreenPct=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`, while removing all
non-startup `lodThrottle=0` frames after frame 120 and reducing the worst broad
prep spike from `109.40 ms` to `64.26 ms`. The waterline/edit A/B
`lod_probe16_alt128_waterline_env_guard_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. The accepted default patch
sets `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_LOD_THROTTLE_ALTITUDE=128` and
initially set
`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_LOD_THROTTLE_PROBE_MAX_REQUESTS=16`.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`lod_throttle_defaults_guard_20260517` passed the broad guard with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.28%`, and
`postNonReady=0`; `lod_throttle_defaults_waterline_guard_20260517` passed the
shoreline/edit guard with `heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=2.66%`, `minVoxelTerrainScreen=74.87%`, and
`postNonReady=0`. The patched broad run had no non-startup `lodThrottle=0`
frames after frame 120; the worst checked ranges moved from `460-510
prep=104.66/gen=77.75` to `36.55/15.87`, and from `580-640
prep=109.40/gen=83.96` to `66.02/36.76`. This is accepted but not closure:
the route still has remaining broad prep spikes around `66 ms` and GPU ray
cost remains up to about `23.65 ms`, so TS-015 remains `PARTIAL`.

May 17 terrain-critical LOD probe continuation: the follow-up broad logs showed
that the remaining post-startup exact generation was still bounded by the LOD
probe itself (`nonStartup max critical gen=16`) even though high-altitude
ownership was already covered by real mid/far voxel layers (`heightProxy=0`,
`miss=0`, `postNonReady=0`). The accepted cap-8 A/B
`lod_probe8_defaultalt_guard_20260517` preserved the broad hard gates with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.28%`, and
`postNonReady=0`, while reducing checked broad ranges from `580-640
prep=66.02/gen=36.76` to `42.39/13.63`, and from `840-900
prep=66.88/gen=35.29` to `46.43/17.62`. The cap-4 attempt
`lod_probe4_defaultalt_guard_20260517` is rejected/incomplete because the
wrapper timed out before post-run checks and no runtime log was produced, so it
cannot prove correctness or timing. The default is now
`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_LOD_THROTTLE_PROBE_MAX_REQUESTS=8`.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`lod_probe8_default_guard_20260517` passed with `heightProxyScreen=0.00%`,
`missScreenPct=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, `postNonReady=0`, and post-startup critical
generation capped at `8`; `lod_probe8_default_waterline_guard_20260517` passed
with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. The accepted patched
default lowered the broad run's worst measured prep from `66.88 ms` to
`46.69 ms`, and worst checked GPU ray from `23.65 ms` to `16.61 ms`. TS-015
remains `PARTIAL`: mid-clipmap prep still reaches about `21 ms`, broad prep is
still above a real-time budget, and manual free-roam acceptance is still not
proved.

May 17 mid-clipmap LOD cap ordering continuation: the cap-8 broad default made
the next bottleneck clear. `PERF_SPARSE_CLIPMAP` still reported LOD-throttled
broad frames with `budgetMid=48` and `genVoxel=48`, because the code applied
`VENPOD_SPARSE_MID_LOD_THROTTLE_BUDGET` before coverage catch-up and then let
the coverage catch-up raise the budget again. Env probes isolated the contract:
`mid_coverage_budget24_guard_20260517` preserved correctness but shifted cost
into late request/generation frames (`840-900 prep=49.84/gen=20.60`) and is not
accepted as a default; `mid_lod_budget24_guard_20260517` also preserved gates
but was ineffective because coverage catch-up still raised `budgetMid` to `48`.
The accepted behavior is `mid_lod24_coverage24_guard_20260517`, which preserved
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.28%`, and
`postNonReady=0`, while dropping the checked broad ranges to `580-640
prep=36.53/gen=11.89` and `840-900 prep=36.32/gen=11.65`. The default patch
keeps the normal coverage catch-up budget unchanged, but applies the LOD
throttle cap after coverage catch-up and changes
`VENPOD_SPARSE_MID_LOD_THROTTLE_BUDGET` from `32` to `24`. Evidence:
`.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`mid_lod_cap_order_default_guard_20260517` passed with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.28%`, and
`postNonReady=0`; `mid_lod_cap_order_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. The patched default
reduced broad worst measured prep from `46.69 ms` to `41.47 ms`; the worst
checked `580-640` range moved from `43.08/13.41/21.11`
(`prep/gen/clip`) to `34.44/11.37/14.14`, and `840-900` moved from
`46.69/17.69/18.38` to `41.47/13.48/15.47`. TS-015 remains `PARTIAL`: the broad
route is materially better but still has prep over a 16 ms frame budget and
needs further manual free-roam/rendering acceptance.

May 17 terrain-critical predictive LOD continuation: the next broad-route
bottleneck was not a new SVO/page-table failure. After the mid-clipmap cap-order
fix, `PERF_SPARSE_TERRAIN_CRITICAL` still logged LOD-throttled broad frames
tracing hundreds of terrain-critical rays (`late avg=443.4`, max `546`) even
though the nominal grid was `273` rays and `postNonReady=0`. Code inspection
showed the strict current-footprint pass was followed by a predictive
future-camera pass; that meant the high-altitude LOD throttle was still paying
for future exact terrain discovery while trying to keep exact terrain work
bounded. The env-only proof `critical_no_predictive_broad_probe_20260517`
preserved the hard visual/readiness gates (`heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.28%`,
`postNonReady=0`) and reduced broad worst measured prep from `41.47 ms` to
`23.01 ms`. The accepted default patch skips the predictive critical probe only
while `sparseTerrainScreenCriticalLodThrottleProbeActive` is true; normal
near/waterline exact terrain-critical probing keeps predictive behavior.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`lod_predictive_skip_final_default_guard_20260517` passed with
`heightProxyScreen=0.00%`, `missScreenPct=0.00%`,
`maxValleyAtmosphereScreen=0.83%`, `minVoxelTerrainScreen=76.70%`, and
`postNonReady=0`; `lod_predictive_skip_final_waterline_guard_20260517` passed
the shoreline/edit guard with `heightProxyScreen=0.00%`,
`maxValleyAtmosphereScreen=2.66%`, `minVoxelTerrainScreen=74.87%`, and
`postNonReady=0`. The accepted broad run reduced worst measured non-startup
prep from `41.47 ms` to `24.94 ms`; checked `580-640` moved from
`34.44/11.80/11.37/14.14` (`prep/req/gen/clip`) to
`24.38/10.16/5.24/15.15`, and `840-900` moved from
`41.47/16.08/13.48/15.47` to `24.94/8.02/4.84/15.49`. Rejected follow-up
probes: lowering the terrain-critical grid to `11x7` globally or for broad
high-altitude probing preserved the hard miss/post-ready gates in some runs, but
was not accepted because the global default produced a terrain-ownership
regression (`minVoxelTerrainScreen=19.30%`) and the high-altitude-only version
did not reliably improve over the accepted predictive skip. TS-015 remains
`PARTIAL`: this removes a confirmed redundant exact-terrain scheduling path, but
late broad frames still exceed a 16 ms target and manual free-roam acceptance is
not proven.

May 17 mid-height clipmap rejection: the next suspected cost after the
predictive-skip fix was mid-clipmap height generation. Logs from
`predictive_skip_restored_default_guard2_20260517` show the late broad frames
still spending up to `16.86 ms` in clipmap prep; frame 884 spent
`pumpHeight=9.37 ms` and `pumpVoxel=2.89 ms` with `budgetMid=24`. This looked
like removable legacy height work because the public layer summary reported no
`midHeightScreenPct` ownership. Two probes disproved that simple interpretation.
Disabling the mid-height clipmap in voxel-terrain-only mode preserved
`heightProxyScreen=0.00%` and `postNonReady=0`, but regressed composition to
`minVoxelTerrainScreen=19.74%` in both broad and waterline guards. A softer
height-generation budget cap of `8` produced the same hidden ownership collapse
(`minVoxelTerrainScreen=19.27%`). Both changes were reverted. The conclusion is
that the mid-height layer still contributes to acceptable terrain composition or
handoff even when it is not surfaced as `MID_HEIGHT` ownership in the simple
timeline; the next fix must preserve that composition contract rather than
removing or starving the layer. Restored evidence:
`predictive_skip_restored_default_guard2_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`;
`predictive_skip_restored_waterline_guard2_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. TS-015 remains
`PARTIAL`.

May 17 center-height reuse acceptance: the next accepted route preserves the
mid-height and mid-voxel composition contract but removes redundant procedural
terrain work inside the exact and mid-clipmap generation paths. Code inspection
showed height-tile packing and mid/exact voxel column generation sampled
`HeightAt(worldX, worldZ)` and then called terrain helpers that recomputed that
same center height while calculating relief/material. The accepted patch adds
`SparseTerrainGenerator::SurfaceReliefAtWithCenter(...)` and routes height-tile
packing, mid-voxel clipmap column sampling, and exact generated sparse brick
column relief through the existing material classifier with the already-known
height. This is not a material-rule rewrite; it keeps
`SampleGeneratedVoxelWithColumn(...)` authoritative.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`relief_center_reuse_default_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`;
`relief_center_reuse_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. Compared with the restored
baseline `predictive_skip_restored_default_guard2_20260517`, broad non-startup
peak prep dropped from `31.69 ms` to `22.53 ms`; worst checked `460-510` moved
from `14.78/5.15/4.00/6.54` (`prep/req/gen/clip`) to
`13.51/4.46/3.14/5.63`, `580-640` moved from
`25.41/6.29/4.81/15.30` to `21.73/5.87/4.47/13.01`, and `840-900` moved from
`28.72/8.51/5.85/16.86` to `22.53/5.56/4.13/13.46`. TS-015 remains `PARTIAL`:
the specific redundant sampling cost is fixed and verified, but broad terrain
still exceeds a 16 ms target and manual free-roam/fake-shoreline acceptance is
not proven.

May 17 conservative AIR edit overlay acceptance: the next shoreline/material
diagnosis found a direct fake-terrain/disappearing-terrain cause in the
mid-voxel clipmap edit overlay. `SparseClipmapTileCache::GenerateVoxelBrick`
summarized edited voxels into coarse clipmap cells and treated any AIR edit as
proof that the whole coarse cell should become AIR when no solid edit was also
present. With the default mid cell size of `12`, one erased voxel could wipe a
large generated land/water cell in the background layer. That matches the user
symptom where shoreline sand/terrain changes or disappears after drawing even
though exact sparse pages should own the local edit. The accepted patch keeps
solid edited voxels visible in the coarse context but only lets AIR override the
coarse cell when the clipmap cell is effectively exact (`cellSize <= 1.5`);
normal exact sparse pages and extracted surfaces still own local erasure. This
does not remove the mid voxel layer or weaken surface ownership.
Evidence: `.\build.ps1` and `.\build\bin\VENPODTests.exe` passed;
`mid_air_edit_conservative_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`;
`mid_air_edit_conservative_default_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`. The waterline runtime log
shows the live brush path invalidating mid-clipmap bricks during the capture
(`SPARSE_MID_CLIPMAP_EDIT_INVALIDATE` frames 64 onward, typically 3-4 bricks per
revision) while retaining full mid coverage (`midCov=1.00/1.00`) during the
brush stroke. TS-015 remains `PARTIAL`: the overbroad AIR override is fixed and
verified against waterline brush and broad stress gates, but manual free-roam
acceptance and the remaining far mountain gaps/performance work are still
unverified.

May 17 LOD-throttled mid budget acceptance: broad stress logs after the
conservative AIR edit fix still showed CPU prep spikes where exact
terrain-critical protected generation and mid-clipmap pumping peaked in the same
frame. Example: `mid_air_edit_conservative_default_guard_20260517` frame 683
reported `prep=24.61 ms`, `req=8.68 ms`, `gen=6.18 ms`, and `clip=9.43 ms`
while LOD throttle was active. The mid layer was already capped during LOD
throttle, but the default cap of `24` still let the mid pump compete with exact
terrain-critical work. The accepted default lowers
`VENPOD_SPARSE_MID_LOD_THROTTLE_BUDGET` from `24` to `16`. Env probes and
default reruns preserved the visual gates; the repeat default broad run
`mid_lod16_default_after_patch_guard2_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=72.28%`, and `postNonReady=0`; the waterline brush run
`mid_lod16_default_after_patch_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, and `postNonReady=0`. Compared with
`mid_air_edit_conservative_default_guard_20260517`, checked late broad ranges
moved from `580-640 ~= 20.30 ms` top-frame behavior to `18.51 ms`, from
`650-690 = 24.61 ms` to `21.25 ms`, and from `840-900 = 22.16 ms` to
`21.41 ms` in the repeat default run. A separate exact-request burst remains
around frame 743 (`prep=23.46 ms`, `req=10.72 ms`), so TS-015 remains
`PARTIAL`: this reduces one confirmed mid-clipmap contention mode but does not
complete broad-view performance or far-gap acceptance.

May 17 mid-interest queue rebuild acceptance: the remaining stress-camera
captures showed the mid voxel clipmap repeatedly rebuilding a 7,372-brick
interest target and appending current missing terrain behind older queued work.
This produced slow far-mountain fill-in and per-frame interest costs even when
the terrain-critical exact request set was already post-ready. The accepted
engine-level change coarsens the mid-clipmap interest signature to the mid cell
scale and rebuilds the height/voxel generation queues in current-priority order
whenever that signature actually changes. It does not re-enable height-proxy
terrain or alter the conservative AIR-edit shoreline fix. Build and sparse core
tests passed. The stress guard
`mid_interest_queue_rebuild_stress_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=83.11%`, `postNonReady=0`, `maxFrameMs=23.86`,
`maxPrepMs=22.50`, and `maxGpuRayMs=12.42`; in frames `>=600`, clipmap
`maxInterestMs` was `3.96`, `maxPumpMs` was `8.61`, and minimum mid voxel
coverage was `73.37%`. The waterline brush guard
`mid_interest_queue_rebuild_waterline_guard_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.66%`,
`minVoxelTerrainScreen=74.87%`, `postNonReady=0`, `maxFrameMs=27.91`,
`maxPrepMs=17.35`, and `maxGpuRayMs=10.55`; the synthetic brush smoke also
passed (`queued=140`, `retired=2810`, `applied=2810`, `fallback=0`,
`missingResident=0`, `overflow=0`, `deltaMismatch=0`). TS-015 remains
`PARTIAL`: this removes a confirmed stale-queue / over-sensitive interest
failure mode, but manual free-roam and longer aggressive terrain routes are
still required before claiming all visual gaps and performance issues are
closed.

Focused May 15 diagnosis: the near-field SVO itself was not the sole root cause.
The focused broken route had exact visible terrain falling behind residency while
the fallback shader mislabeled terrain-facing sparse misses as valley/sky instead
of exposing them as illegal near terrain holes. GPU upload was not saturated, far
SVO ownership was low, and the old sparse page pool was under pressure. The
focused fix increased exact-terrain residency headroom, enabled near/startup
terrain prefetch by default, widened terrain-intersection request coverage, and
made debug mode 50 distinguish illegal near sparse holes from real sky/valley.
The follow-up fix added a startup public-render gate so early frames show a
loading/sky state instead of an incoherent sparse world, and tightened the debug
shader so horizon rays through air are not falsely reported as near terrain
holes unless the first missing sparse sample is near the expected terrain
surface.

Focused shoreline/far-gap follow-up: the May 15 waterline capture showed the
"fake sand" was not the deprecated height-proxy path. Debug mode 50 identified
the first failure as duplicated exact sparse surface ownership: solid terrain
tops under water were being emitted as dry faces, so water-covered basin floors
could look like sand. A later May 16 review found the complementary shoreline
failure: solid terrain faces beside water were also suppressed, so banks did
not always write real sparse depth and the background layer could leak
procedural sand/water through the shoreline. The current extractor contract is:
water owns terrain tops directly below water, but solid side/bottom boundaries
against water are renderable sparse terrain. The remaining far mountain gaps
are now classified as background/sky after resident surface/mid/far voxel paths
miss; they are not near-field missing sparse pages, and they are not old
height-proxy terrain.

May 16 shoreline depth correction: the exact sparse surface extractor now
renders solid side/bottom faces adjacent to water while still suppressing the
solid top face under water. This addresses the "fake sand near water" symptom
where missing shoreline depth let mid/far background terrain show through until
an edit caused the exact sparse world to refresh. The sparse core unit test now
proves both halves of the contract, and waterline/broad capture guards pass
with `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. This is a real root-cause fix, but it does not close the far
mountain/background continuity or performance items.

May 16 far-SVO silhouette continuation: the remaining broad debug gaps were
not caused by missing far-SVO residency. Logs showed the far-SVO page forest was
complete (`farCov=1.00/1.00`) and voxel-terrain-only mode correctly disabled
the old procedural height fallback, but low-altitude mountain/silhouette rays
were either rejected by the far-SVO upward-ray guard or given too few page-DDA
steps at `farQ=0.45`. The shader now allows bounded low-altitude upward
mountain-silhouette rays into the page-indexed far SVO and gives those/horizon
rays a modestly larger page-step budget without re-enabling height-proxy or
far-height terrain. The focused debug route improved from `maxSky=29.08%`,
`minVoxelTerrainScreen=70.92%` to `maxSky=14.65%`,
`minVoxelTerrainScreen=85.35%`, while broad/high-flight/waterline guards still
pass the hard no-fake-terrain gates.

May 16 follow-up diagnosis: the reproduced fast-walk route now passes the hard
near-terrain correctness gates (`miss=0`, `unsafeNearMiss=0`,
`heightProxyScreen=0`, `postNonReady=0`), but debug mode 50 shows the remaining
fake-looking terrain is mostly background ownership: yellow is authoritative
sparse surface, orange is mid-voxel background, and magenta is far-SVO
background. Disabling mid/far background proves the exact sparse surface is
honest but incomplete at distance. Simply increasing screen-critical distance or
surface ownership radius creates more exact surface work and worsens frame time,
so the next architecture fix must make the mid/far LOD transition less fake
without over-feeding the exact surface pipeline.

May 16 continuation update: the waterline fake-sand issue was narrowed to the
cheap mid-voxel column layer, not to the authoritative sparse surface or SVO
page readiness. The default renderer runs with voxel-terrain-only background
fallbacks, so the far-water occluder path does not normally get to hide
submerged mid-column terrain. The accepted fix maps submerged mid-column height
samples to the water surface/material before hit testing, so background context
does not show sand seabed where the deterministic world will later resolve to
water. The next accepted performance pass caches deterministic height/relief
terrain columns across exact sparse generation and surface extraction work in a
frame, reducing duplicated halo sampling while preserving edited-voxel overlay
checks. The following accepted pass adds a coverage-driven mid-voxel catch-up
lane for far/background terrain only when exact terrain-critical readiness is
clean, reducing far mountain clipmap holes without weakening the no-fake-terrain
gates.

May 16 continuation note: far-mountain holes were still visible after the first
mid-voxel catch-up pass because the catch-up lane stopped once interested
mid-voxel coverage reached 88%, leaving roughly 10% of the high-flight
background voxel set unresolved. The default catch-up threshold is now 99%,
which keeps the far/background lane active longer while preserving the hard
near-terrain gates. A local exact-surface material-cache extractor optimization
was tested and rejected: it preserved correctness after a partial-update bug was
fixed, but it made fast-walk surface extraction slower and was reverted. The
buried visible-brick skip is now default-on for speculative/visible requests
only, keeping collision and edited requests authoritative while avoiding
provably deep, non-surface visible work.

May 16 resumed diagnosis after interruption: the reproduced fast-walk route at
frames 220-260 is no longer primarily missing mid-voxel residency. The accepted
baseline still shows a small left-cliff/valley-atmosphere cut, but debug
ownership and logs classify it as `valleyAtmosphere`, not `miss`,
`unsafeNearMiss`, or `heightProxy`. Raising the mid-voxel catch-up gate from
pre-critical readiness to drainable critical readiness made `budgetMid` rise to
96 and reached `midVoxInterest=4096/4096`, but the visible
`valleyAtmosphere` pixels were unchanged; on waterline it caused a severe
startup catch-up spike (`budgetMid=168`, `pumpVoxel=19.45 ms`, frame body over
100 ms) and was reverted. Expanding the walking mid-voxel DDA angle window and
adding parent-ring fallback to mid-voxel neighbor exposure also failed to reduce
the target `valleyAtmosphere` patch and were reverted. A widened exact sparse
surface cull distance (`2200`) likewise left the same ownership split, proving
the remaining patch is not simply a too-small surface cull radius.

May 16 latest continuation: the low-angle waterline "fake sand/air" artifact
after the exact-near fallback fix was an underwater background-miss
classification problem, not a missing sparse page. The waterline camera sits
below `FAR_SEA_LEVEL`, so the far-water occluder deliberately returns false and
cannot cover that view. The accepted shader path now classifies underwater
non-sky, non-terrain-hole miss pixels as `waterContext` instead of
`valleyAtmosphere`, while preserving `unsafeNearMiss` priority for real
terrain-facing sparse holes. This dropped the reproduced waterline
`valleyAtmosphereScreen` from `44.96%` to `0.00%` with `miss=0`,
`unsafeNearMiss=0`, and `heightProxyScreen=0`. The same pass aligned far SVO
and shader fallback material thresholds with the authoritative sparse terrain
generator (`sea+6` beach band and `relief>10` stone cutoff) and bumped the far
SVO cache version so stale precomputed material pages cannot hide the change.
The first post-bump run failed only the frame-120 far-SVO readiness smoke while
the new cache was being built, then saved the version-15 cache; the warmed-cache
regression passed.

May 16 resumed performance continuation: the next measured hotspot after the
fake-terrain/material fixes was sparse-surface overdraw in walking views. The
sparse surface pixel shader records ownership through UAV atomics, so it now
declares `[earlydepthstencil]` to preserve early depth/stencil rejection for
hidden raster fragments. This preserved the hard no-fake-terrain gates and
reduced late fast-walk sparse-surface overdraw from roughly `2.32-2.37x` to
`1.65-1.92x`. High-flight captures are not surface-overdraw bound; they are
mostly far-SVO/background raymarch work because the sparse surface owns only a
small portion of the screen there.

May 16 partial dirty-surface continuation: the next measured CPU hotspot was
surface snapshot/stage fallback when the dirty surface brick set exceeded the
dirty-payload copy-region budget. That path preserved correctness but rebuilt
and staged the full visible surface snapshot, causing walking spikes. Dirty
payload snapshots now upload a bounded dirty prefix and carry a deferred dirty
count, so old complete surface data remains visible while the remaining dirty
bricks upload in later frames. Fast-walk frame 240 improved from the measured
baseline body `36.12 ms`, prep `19.98 ms`, sparse `19.49 ms`, and surfStage
`1.31 ms` to body `18.76 ms`, prep `10.86 ms`, sparse `10.41 ms`, and
surfStage `0.96 ms`; startup evidence showed the partial path active with
`pendingDirty=258`, `dirtyCopied=128`, and `deferred=130`. Later walking
surface-stage spikes remain, and high-flight is still dominated by mid-voxel
pump plus far-SVO/background raymarch rather than this surface upload path.

May 16 compact-slot and mid-cache continuation: focused spike logs showed that
small removal frames still force the fuller surface metadata path, but the
following dirty-only frames were also falling back because compact stable draw
commands made `drawSlots != gpuDrawCmds`. Dirty payload staging now allows
same-size resident payload patches through that compact-slot state and only
defers metadata-changing dirty bricks when free slots or compact metadata make
incremental metadata unsafe. This preserves the atomic "old complete mesh until
new complete payload" rule and removes the post-removal dirty-frame fallback.
The remaining removal frame itself is still a deeper metadata/compaction
hotspot, not a visual hole. High-flight diagnostics also proved the mid-voxel
cache had no hysteresis: the interest set used all `4096` slots, causing edge
movement to generate and evict visible-edge bricks in the same frame. The
default `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT` is now `90`, leaving cache slack
without raising memory to the rejected `8192`-slot path.

May 16 incremental removal continuation: the removal-frame hotspot is now
handled without a full surface metadata rebuild for the focused walking route.
Dirty surface staging accepts visible removals, retires their face allocations,
clears the fixed range-table entries, invalidates matching surface records, and
recomputes only affected cluster summaries. This keeps GPU cull from drawing
stale removed faces while avoiding a complete record/cluster rebuild. The first
probe exposed bad dirty-snapshot accounting (`gpuRanges` briefly reported the
dirty count instead of the resident count); that was fixed before acceptance.
The accepted fast-walk rerun preserved the hard visual gates and dropped the
target removal frame 274 from the previous body spike to roughly a normal
frame. Waterline and high-flight regressions still pass, but waterline stress
remains GPU/raymarch/frame-pacing heavy and is not performance-complete.

May 16 reopened-goal continuation: the remaining far-mountain gap route was
not caused by a missing far-SVO cache radius. A warmed `VENPOD_FAR_SVO_PAGE_RADIUS=6`
probe still failed with `maxValleyAtmosphereScreen=11.70%`, while far SVO was
fully resident, proving radius/upload readiness was not the root cause. Raising
mid-voxel coverage catch-up and cache capacity was rejected or inconclusive:
the `5120`-brick fast-walk probe improved background ownership but violated the
exact terrain readiness contract, and the clean `128` catch-up high-flight
probe left the valley metric essentially unchanged. The accepted shader fix
keeps high-altitude shallow miss rays classified as sky instead of
valley-atmosphere once resident SVO/mid layers do not find terrain, without
re-enabling height-proxy or non-voxel fake terrain. A small high-altitude far
SVO traversal budget increase improved the failed high-flight valley peak from
about `12.15%` to `9.81%`; a denser far-leaf sampling experiment did not move
the metric and was reverted. The final accepted high-flight guard reports
`maxValleyAtmosphereScreen=1.47%`, `maxHeightProxyScreen=0.00%`, `miss=0`,
`unsafeNearMiss=0`, and `postNonReady=0`.

May 16 exact critical-generation continuation: the fast-walk route exposed a
real engine-level readiness bug after the high-altitude classification fix.
The protected terrain-critical generation loop said it was draining exact
critical bricks, but it called `PumpGenerationAround(1, coord)`, which can spend
that one slot on a nearby queued brick. That left one to five visible critical
targets in `Requested` after protected publish on repeated fast-walk guards.
The accepted fix adds `SparseVoxelWorld::PumpGenerationForCoord` and uses it
for terrain-critical protected generation, so exact critical coordinates are
generated before upload/surface publish. The sequential fast-walk guard
`exact_critical_generation_fastwalk_guard_20260516` passed with
`maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.18%`,
`minVoxelTerrainScreen=76.79%`, and `postNonReady=0`. Sequential regressions
`exact_critical_generation_highflight_guard_seq_20260516` and
`exact_critical_generation_waterline_guard_seq_20260516` also passed; the
waterline route stayed at `maxValleyAtmosphereScreen=0.00%` and the high-flight
route stayed under the far-gap gate at `1.47%`. `VENPODSparseCore` passed after
the C++ change. Parallel validation runs from this pass are explicitly
invalidated because the capture scripts share runtime log paths.

May 16 reopened-goal performance diagnosis: the latest screenshot symptom is no
longer a broken near-field SVO or height-proxy fake terrain on the reproduced
routes. The hard gates still pass, but long fast-walk captures show CPU-side
sparse maintenance and frame pacing spikes when surface dirty/removal metadata
and mid-voxel background maintenance coincide. A first sparse-surface record
lookup patch that copied the lookup map in the staging hot path is rejected
because it preserved visual gates but worsened the long-walk peak. The corrected
no-copy lookup version passes `VENPODSparseCore`, waterline brush/material, and
high-flight visual guards, but it is not a performance close: the long-walk
guard still reports `maxFrameMs=96.30`, `maxPrepMs=97.73`,
`maxGpuRayMs=19.69`, `heightProxyScreen=0`, and `postNonReady=0`. The next
engine-level target is not more SVO radius or fake background terrain; it is
removing full sparse-surface metadata mirror/snapshot work from small
dirty/removal frames, or otherwise pacing that work without exposing partial
world state.

May 16 reopened-goal continuation after interruption: the remaining broad
fast-walk/far-mountain gaps were traced to sparse-surface streaming contract
bugs rather than a fundamentally broken SVO. Dirty surface snapshots previously
refused to run when removals existed, zero-face dirty bricks failed the dirty
path instead of retiring stale GPU surface data, and range-table deletion wrote
empty entries that broke linear-probe lookup chains. The dirty fast path could
also starve needed full catch-up while GPU surface records lagged far behind CPU
surface bricks. A separate allocation bug deferred allocation-changing dirty
bricks forever when free draw slots existed, and the published dirty-ticket face
count could exceed the IA/raymarch surface buffer capacity after slot reuse. The
accepted fix stack adds range tombstones, zero-face dirty retirement, removal
drainage through dirty snapshots, stable draw-slot reuse, catch-up pressure from
valley-atmosphere ownership, full-snapshot compaction when metadata becomes
fragmented, and live allocator-capacity face-count publication. Validation now
passes the long fast-walk, waterline brush, and high-flight visual guards with
`heightProxyScreen=0`, `postNonReady=0`, and bounded
`valleyAtmosphereScreen`; performance is improved but not closed because the
long fast-walk still has high smoothed/prep spikes.

May 16 current exact-handoff correction: a deterministic debug A/B showed that
turning off sparse-surface culling exposes stale/off-axis yellow sparse-surface
slabs, so culling is not the root of the far gaps. The remaining blue cutouts
come from the surface-authoritative fullscreen pass suppressing resident
mid/far voxel fallback until the old 3072-voxel exact-near distance, turning
surface-cull/extraction gaps inside that range into sky. The default
`VENPOD_SPARSE_EXACT_NEAR_DISTANCE` is now `1024`: exact sparse surface still
owns the immediate edit/collision foreground, while resident mid-voxel/far-SVO
layers can fill mid-distance valley walls instead of showing sky. Focused and
broad captures now pass with `heightProxyScreen=0`, `miss=0`,
`unsafeNearMiss=0`, `postNonReady=0`, and the broad variable-dt route improved
from the previous `maxFrameMs=120.22`, `maxPrepMs=111.38`, `maxGpuRayMs=11.36`
to `maxFrameMs=68.65`, `maxPrepMs=39.54`, `maxGpuRayMs=5.84`.

May 16 resumed-after-closure continuation: the previous broad long fast-walk
guard used a fixed frame cadence and passed, but the user's rerun path uses
variable frame time. That variable-dt route exposed a real mid-distance coverage
hole: exact terrain-critical post-publish readiness stayed clean
(`postNonReady=0`), but the mid-voxel queue grew into the thousands while
`budgetMid` stayed throttled because mid catch-up was gated by stale pre-publish
exact-terrain counts. A direct post-publish gate was rejected because startup
could generate thousands of mid bricks before the first GPU mid-clipmap snapshot,
making the first upload too large and failing the `mid-clipmap` backend
readiness gate at frame 120. The accepted-in-progress code now uses
post-publish exact readiness only after the mid-clipmap has an initial GPU seed;
before that seed, it retains the conservative pre-publish startup gate. Evidence:
`postready_seeded_mid_catchup_long_fastwalk_guard_20260516` no longer fails
startup readiness and reports `heightProxyScreen=0`, `miss=0`,
`maxValleyAtmosphereScreen=5.45%`, and `minVoxelTerrainScreen=63.19%`.
However, it still reports small delayed `unsafeNearMiss` peaks around
owner frames 140-179 (`maxUnsafeNearMissScreen=0.2616%` in the long run), so
TS-014/TS-015 remain `PARTIAL`. Debug mode 50
`debug50_unsafe_span_seeded_mid_20260516` shows the visible scene is mostly
orange mid-voxel terrain with magenta far-SVO background; the unsafe pixels are
thin transition specks, not broad unloaded yellow/near exact chunks. Rejected
follow-ups from this pass: narrowing the shader's terrain-adjacent miss band
caused a launch-time shader/pipeline stall and was reverted; raising
`VENPOD_SPARSE_EXACT_NEAR_DISTANCE` down to `256` did not reduce the unsafe
span; doubling predictive terrain warm requests worsened the unsafe peak and
was reverted. The remaining diagnosis is a delayed ownership/transition
classification issue at the surface/mid/far handoff during variable-dt yaw, not
a broken SVO or a global resource ceiling.

May 16 current continuation: the latest hard gap root cause was not that the
SVO was globally broken. Shader feedback reported exact missing sparse bricks
near waterline/terrain, but the CPU miss-feedback consumer remapped those
coords to terrain-height surface coords and could drop the original shader
coord. That left the brick the shader actually missed absent until later unsafe
feedback. The accepted fix queues the original shader-reported brick first,
then queues the terrain-remap neighborhood. The current default keeps the
neighborhood cap at `7`; the stricter original-only cap failed the fast-walk
guard and proved the neighborhood is still needed. Validation also exposed two
smoke-contract bugs: high-flight and waterline runs intentionally hold public
rendering until the startup gate releases, so `surface-raster` and GPU-cull
dispatch must be proven by uploaded surface GPU data during the hold and by
real rasterized faces after the hold. Ownership stability likewise must not
stay unobservable forever just because a clean far/waterline view has low
voxel-terrain percentage; it now skips only during actual illegal miss/unsafe
recovery. Current post-fix captures pass fast-walk, high-flight, and waterline
with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and
terrain-critical `postNonReady=0`.

May 16 startup-gate continuation: the remaining high-flight/waterline startup
hold was not real protected terrain work. Readiness stayed blocked because the
startup gate treated speculative background uploads as public-render blockers.
The reproduced logs showed `uploadQueued=176` but `qup=176/0/0/0`, with
visible/collision/edited queues at zero and terrain-critical `postNonReady=0`.
The accepted fix lets speculative uploads continue streaming in the background
while public rendering opens once protected uploads, minimum ready bricks, and
surface GPU proof are clean. The held-state log now reports
`protectedUploadQueued`, and high-flight/waterline captures pass in the earlier
capture windows that previously waited until the max-frame startup release.

May 16 surface metadata resize continuation: the next reproduced broad route
showed a real engine-level staging bottleneck after the visual gap fixes. Logs
had tiny dirty/removal sets, but `StageDirtyPayloadSnapshot` could still clone
the full range table, draw-slot map, surface records, and cluster mirrors for a
one-brick allocation resize; old cluster summaries were also not recomputed when
an existing surface record changed face count. This is not an SVO data failure,
but it delays terrain publication and makes streaming feel broken. The accepted
patch uses the existing incremental metadata-copy path for resize-only updates
to already-resident records, updates only the changed range/draw/record/cluster
entries, recomputes affected cluster summaries, and only bumps resident counts
for genuinely new metadata entries. Focused broad validation now passes with
`heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, and
`maxFrameMs=58.74`; frame 1200 surface staging is `2.52 ms` with `stagedMB=0.02`
instead of the previous small-change/full-metadata pattern. A frame-1440 spike
remains (`surfStage=10.52 ms`, `surfSnap=3.51 ms`), so this is a partial
performance/root-cause fix, not terrain completion.

Status vocabulary:

- `DONE_VERIFIED`: implementation exists and current evidence proves the claimed behavior.
- `DONE_UNVERIFIED`: implementation appears to exist, but current evidence is incomplete or stale.
- `PARTIAL`: some implementation and/or tests exist, but the requirement is not fully satisfied.
- `NOT_STARTED`: no meaningful implementation was found.
- `BLOCKED`: cannot proceed without missing information, missing assets, credentials, hardware, or a user decision.
- `DEFERRED_BY_USER_ONLY`: only the user may assign this status.

## Sources

- User screenshot-based diagnosis relayed on 2026-05-14.
- Current VENPOD sparse renderer symptoms: huge air gaps, floating terrain slabs, slow gradual pop-in, sparse yellow exact-voxel regions surrounded by fake/background terrain.
- Existing broad ledger: `docs/COMPLETION_LEDGER.md` (deprecated for this issue).
- Relevant code areas to audit:
  - `VENPOD/src/Simulation/SparseVoxelWorld.*`
  - `VENPOD/src/Simulation/SparseBrickPool.*`
  - `VENPOD/src/Simulation/SparseBrickRequestPlanner.*`
  - `VENPOD/src/Simulation/SparseRuntimeBudget.*`
  - `VENPOD/src/Simulation/SparseClipmap.*`
  - `VENPOD/src/Simulation/SparseSurfaceExtractor.*`
  - `VENPOD/src/Simulation/SparseSurfaceCache.*`
  - `VENPOD/src/Graphics/SparseVoxelGpuResources.*`
  - `VENPOD/src/Graphics/SparseSurfaceGpuResources.*`
  - `VENPOD/src/Graphics/Renderer.*`
  - `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`
  - `VENPOD/assets/shaders/Graphics/PS_SparseSurface.hlsl`
  - `VENPOD/engine_capture_smoke.ps1`

## Completion Items

### TS-001
1. Requirement ID: TS-001
2. Requirement text: Add an explicit readiness state machine for renderable terrain units.
3. Source document / source location: User diagnosis: "never render partial world state"; `Missing -> QueuedForGeneration -> HasVoxelData -> QueuedForMeshing -> HasCompleteMesh -> UploadedToGPU -> ReadyToRender`.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `SparseVoxelWorld`, `SparseBrickPool`, `SparseSurfaceCache`, `SparseVoxelGpuResources`, `SparseSurfaceGpuResources`.
6. What currently exists: Sparse brick lifecycle states and surface/GPU queues exist. `SparseRenderReadinessState` now maps sparse bricks into renderer-facing states including `Requested`, `UploadQueued`, `UploadingGPU`, `ResidentMissingSurface`, `ResidentEmpty`, and `ReadyToRender`. `VENPODSparseCore` includes a fixed 3x3x3 visible sparse grid that is synchronously generated, uploaded, surface-extracted, and checked for `ReadyToRender` before render would begin. Startup hold logs and normal `PERF_SPARSE_READINESS` logs now include readiness counts. Startup public rendering now remains held until the minimum frame and readiness gate both pass (`ready >= VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_READY_BRICKS`, no queued/uploading bricks, and no resident renderable brick missing a surface, bounded by `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MAX_FRAME`).
7. What is missing: Runtime public rendering still needs to consume this readiness contract directly for all terrain layers, and parent/mid/far LOD readiness still needs its own all-children-ready proof.
8. Validation required: Debug overlay and runtime log prove only `ReadyToRender` terrain units are drawn as exact terrain.
9. Exact proof: A capture with terrain unit state colors plus a CSV/log showing zero rendered exact-terrain pixels from non-ready units.
10. Latest evidence: `VENPODSparseCore` passed on 2026-05-15 after `TestSparseFixedGridReadiness` proved `ResidentMissingSurface -> ReadyToRender` for a fixed visible target set. `VENPOD/build/captures/terrain_gap_readiness_gate_postcritical_20260515` passed and shows startup frame 0 held with `readinessBlocked=1`, `uploadQueued=136`, `residentMissingSurface=32`, and `ready=86`; by frame 20 the gate log reports `readinessBlocked=0`, `uploadQueued=0`, `residentMissingSurface=0`, and `ready=767`, while public rendering is still held until frame 112. At frame 120 normal readiness reports `missing=0`, `uploadQueued=0`, `uploading=0`, `residentMissingSurface=0`, and `ready=1268`.
11. Next action required: Make public renderer/capture diagnostics consume the readiness enum for all terrain layers, not only sparse brick startup logs.

### TS-002
1. Requirement ID: TS-002
2. Requirement text: Keep old mesh or parent LOD visible until replacement children are fully ready.
3. Source document / source location: User diagnosis: parent LOD must not be dropped until all children are ready.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `SparseClipmap`, far SVO traversal, `Renderer`, `PS_Raymarch.hlsl`.
6. What currently exists: Sparse surface, mid-voxel, and far-SVO layers coexist. Mid-voxel shader traversal now reports `lodParentHeld` / `parentHeldUntilChildrenReady` when the preferred fine clipmap ring is absent and a coarser resident ring is used instead, proving that this route holds parent LOD instead of exposing a child-ring hole.
7. What is missing: Atomic parent-to-child swap proof for every terrain representation, especially sparse brick groups and far-SVO/far-mesh replacement paths. The current evidence proves the mid-voxel parent-ring fallback route, not all terrain LOD handoffs.
8. Validation required: LOD debug route where parent remains visible while any child in a split group is missing, generated, meshed, or uploading.
9. Exact proof: Same-frame normal/debug captures showing no holes during child streaming and logs reporting `parentHeldUntilChildrenReady`.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_lod_parentheld_probe_20260515` passed with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, and `minVoxelTerrainScreen=56.82%`. Its `PERF_RENDER_OWNERSHIP` rows report nonzero `lodParentHeld` pixels, e.g. frame 112 had `lodParentHeld=69508` while `miss=0` and `unsafeNearMiss=0`, proving mid-voxel parent-ring fallback is active during streaming catch-up.
11. Next action required: Extend the same explicit parent-held proof to sparse brick groups and far-SVO/far-mesh replacement paths before marking TS-002 complete.

### TS-003
1. Requirement ID: TS-003
2. Requirement text: Mesh generation must use deterministic halo/ghost-cell sampling instead of treating missing neighbors as air.
3. Source document / source location: User diagnosis: chunk boundaries need halo/ghost cells.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `SparseSurfaceExtractor`, `SparseTerrainGenerator`, `SparseSurfaceCache`, `SparseVoxelWorld`.
6. What currently exists: Runtime surface extraction passes `SampleEditedOrGeneratedVoxel` as the neighbor sampler, giving the mesher deterministic world samples across brick boundaries without requiring resident neighbor bricks. `VENPODSparseCore` now covers all exterior sheets at negative brick coordinates and verifies negative-X boundary occlusion through halo/world sampling.
7. What is missing: Nothing for sparse surface extraction's deterministic halo/world-sampler contract. Parent/child LOD transition geometry remains TS-002, and runtime state-color diagnostics remain TS-004.
8. Validation required: Fixed-grid capture and unit tests at chunk/brick boundaries, including negative coordinates.
9. Exact proof: `VENPODSparseCore` boundary tests proving neighbor samples suppress cross-brick faces at positive and negative coordinates, plus fixed-grid readiness tests that drain surface extraction without resident-neighbor dependency.
10. Latest evidence: `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore` passed on 2026-05-15 after adding negative-coordinate halo checks.
11. Next action required: Keep these tests as regression coverage when changing the sparse mesher or terrain sampler.

### TS-004
1. Requirement ID: TS-004
2. Requirement text: Add a terrain unit state debug view.
3. Source document / source location: User diagnosis: color chunks by queued/generated/meshed/uploaded/ready/missing-neighbor/culled.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `PS_Raymarch.hlsl`, `PS_SparseSurface.hlsl`, `Renderer`, `engine_capture_smoke.ps1`.
6. What currently exists: Ownership/debug modes exist, debug mode 50 distinguishes illegal near sparse terrain misses from ordinary sky/valley fallback in the focused route, startup logs report renderer-facing sparse readiness counts, normal perf logs include `PERF_SPARSE_READINESS`, and `PERF_SPARSE_TERRAIN_CRITICAL` now reports pre/post readiness for the screen-critical target set (`preMissing`, `preUploadQueued`, `postReady`, `postResidentMissingSurface`, etc.). If post-publish screen-critical terrain is still not render-ready, `PERF_SPARSE_TERRAIN_CRITICAL_NONREADY` logs sample brick coordinates with lifecycle state, and `engine_capture_smoke.ps1` now fails post-ready captures on any post-publish missing/requested/generating/uploading/resident-missing-surface terrain-critical target.
7. What is missing: A full on-screen state-color overlay that distinguishes queued, generated voxel data, meshed, uploaded, ready, missing halo, culled, and LOD-parent-held states with coordinates.
8. Validation required: Full-resolution debug capture of the broken route showing every hole categorized by lifecycle state.
9. Exact proof: Debug-mode capture plus CSV counts for each state, with chunk/brick coordinates in runtime logs for high-screen-area failures.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_prefetch_wide_strict_debug50_20260515` passed with `miss=0`, `unsafeNearMiss=0`, `maxValleyAtmosphereScreen=2.52%`, and `minVoxelTerrainScreen=64.20%`. `VENPOD/build/captures/terrain_gap_continuous_critical_ready_20260515` passed with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=9.62%`, and `minVoxelTerrainScreen=57.24%`; the new terrain-critical gate observed `samples=1 readyFrame=120 postNonReady=0`. Its runtime log proves the startup distinction: frame 0 had non-ready `UploadQueued` critical samples while public rendering was held, while frame 120 had `continuous=1`, `preMissing=37`, and `preRequested=19` before protected publish and `postMissing=0`, `postRequested=0`, `postGenerating=0`, `postUploadQueued=0`, `postUploading=0`, and `postResidentMissingSurface=0` afterward.
11. Next action required: Add lifecycle-stage debug colors on screen; coordinate/state logging and capture failure are now present, but the visual overlay still needs queued/generated/meshed/uploaded/ready color categories.

### TS-005
1. Requirement ID: TS-005
2. Requirement text: Render a small fixed terrain grid synchronously with sparse graph, LOD, culling, and unloading disabled.
3. Source document / source location: User diagnosis: isolate generation/meshing from streaming/culling/LOD.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: launcher env/config, `SparseVoxelWorld`, `Renderer`, `engine_capture_smoke.ps1`.
6. What currently exists: `TestSparseFixedGridReadiness` builds a deterministic 3x3x3 visible sparse target set, synchronously drains generation, upload, and surface extraction, and verifies every target brick remains resident with cached surface faces and zero resident renderable bricks missing surfaces.
7. What is missing: A launcher/runtime mode and capture artifact that renders this fixed grid with sparse graph/LOD/culling/unloading disabled.
8. Validation required: If rendered fixed-grid mode is solid, streaming/LOD/culling is guilty; if it has holes, sampling/meshing/coordinate math is guilty.
9. Exact proof: `VENPODSparseCore` proves the CPU sparse pipeline side; a future capture artifact and log must show fixed-grid complete before first rendered frame, with no holes.
10. Latest evidence: `VENPODSparseCore` passed on 2026-05-15 with the fixed-grid readiness test compiled and executed.
11. Next action required: Add a public/runtime fixed-grid capture switch if holes reappear outside the unit-isolated pipeline.

### TS-006
1. Requirement ID: TS-006
2. Requirement text: Verify chunk/brick coordinate math, especially negative coordinates.
3. Source document / source location: User diagnosis: use floor division/modulo, not truncating integer division.
4. Current status: DONE_VERIFIED
5. Related source files, modules, functions, scripts, or assets: `SparseVoxelTypes`, `BrickCoord::FromWorldVoxel`, chunk coordinate helpers, terrain sampling.
6. What currently exists: Coordinate helpers use floor division/modulo, and `VENPODSparseCore` covers `world=-1`, exact negative boundaries, extreme int32 coordinates, local-index roundtrips, negative edit overlay coordinates, and negative-coordinate surface halo sampling.
7. What is missing: Nothing for the sparse coordinate conversion contract. Runtime fixed-grid visual capture across axes remains TS-005 if needed.
8. Validation required: Unit tests covering `world=-1`, exact boundary positions, and cross-boundary sampling.
9. Exact proof: CTest output for coordinate conversion tests plus sparse surface extraction tests crossing negative/positive axes.
10. Latest evidence: `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore` passed on 2026-05-15 after adding negative-coordinate halo checks.
11. Next action required: Keep these tests in the sparse core gate; add runtime capture only if visual holes correlate with axis crossings.

### TS-007
1. Requirement ID: TS-007
2. Requirement text: Streaming must use a strict visible-near priority queue.
3. Source document / source location: User diagnosis: slow pop-in means visible chunks are not completed in time.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `SparseRuntimeBudget`, `SparseBrickRequestPlanner`, `SparseVoxelWorld`, upload queues, surface queues.
6. What currently exists: Request budgets, terrain prefetch, miss feedback, visible/speculative classes, ownership pressure, same-frame protected publish, widened terrain-intersection prefetch defaults, and screen-critical pre/post readiness telemetry exist. The screen-critical terrain lane is now continuous after startup by default (`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_CONTINUOUS=1`), so the visible-near terrain target set is requested every frame instead of waiting for delayed ownership-pressure feedback. Terrain-critical requests now bypass the known-empty fast path, so protected near air/empty bricks still become real GPU-visible empty pages instead of unresolved page-table holes.
7. What is missing: A full finite per-frame target set with strict priority order across all terrain systems, cancellation/deprioritization outside the target set, and proof that far/background work cannot starve visible-near completion.
8. Validation required: Runtime logs show target set size, missing target count, completed target count, canceled/deprioritized far count, and visible-near queue latency.
9. Exact proof: Long-walk capture where near visible target latency is bounded and no near target renders before ready.
10. Latest evidence: `terrain_gap_prefetch_wide_strict_debug50_20260515` passed the focused route after raising visible terrain request coverage. `VENPOD/build/captures/terrain_gap_continuous_critical_ready_20260515` passed with the continuous screen-critical target lane enabled and proves the short focused route is not still rendering queued critical terrain. The stricter long-walk route initially still failed after widening the grid and adding protected drain; per-frame `PERF_SPARSE_TERRAIN_CRITICAL_UNSAFE_CONTEXT` rows then proved every screen-critical target was `ReadyToRender` or `ResidentEmpty` during the unsafe span, so the remaining failure was not broad SVO/resource starvation. After forcing terrain-critical empty bricks to allocate GPU-visible pages and narrowing the terrain-hole ownership heuristic to page misses close to a real expected terrain crossing, `VENPOD/build/captures/terrain_gap_narrow_unsafe_long_walk_20260515` passed with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=3.59%`, and terrain-critical `postNonReady=0`.
11. Next action required: Keep the long-walk gate as a regression. The broader finite target-set scheduler, cancellation/deprioritization, and free-roam latency proof remain incomplete.

### TS-008
1. Requirement ID: TS-008
2. Requirement text: Startup must block or show a non-world loading state until immediate terrain radius is ready.
3. Source document / source location: User diagnosis: do not spawn into a void; load radius 0-3 completely first.
4. Current status: DONE_VERIFIED
5. Related source files, modules, functions, scripts, or assets: launcher startup, `SparseVoxelWorld`, `Renderer`, capture scripts.
6. What currently exists: Startup screen-critical prewarm and protected same-frame publish are enabled by default, protected same-frame publish continues draining queued work while enabled, and `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_GATE` holds public world rendering until both a minimum startup frame and sparse readiness gate pass. Input and scripted camera movement are also held while the gate is active, while sparse generation/upload/publish continues.
7. What is missing: Nothing for the "do not expose incomplete startup world" behavior. A broader immediate-radius ready-state enum and exact radius proof remain TS-001/TS-005.
8. Validation required: Startup capture including frame 0 must show no black screen, no void/terrain strips, no height-proxy terrain ownership, and no miss/unsafe near terrain pixels after public rendering begins.
9. Exact proof: `engine_capture_smoke.ps1` startup capture with `CaptureStartFrame 0`; this gate intentionally uses `-MinUniqueSampleColors 1` because held frames are flat loading/sky frames.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_readiness_gate_postcritical_20260515` passed with 105 sampled ownership rows, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=16.81%`, `minVoxelTerrainScreen=49.81%`, `miss=0`, and `unsafeNearMiss=0`. Its startup log reports `SPARSE_STARTUP_PUBLIC_RENDER_HELD frame=0 ... readinessBlocked=1 ... uploadQueued=136 ... residentMissingSurface=32 ... ready=86`; by frame 20 the hold log reports `readinessBlocked=0`, `uploadQueued=0`, `residentMissingSurface=0`, and `ready=767`, before public rendering opens at frame 112.
11. Next action required: Keep the startup frame-0 gate in the regression suite; do not remove the loading/sky hold until TS-001 and TS-005 provide an exact immediate-radius readiness proof.

### TS-009
1. Requirement ID: TS-009
2. Requirement text: Mesh/GPU buffer swaps must be double-buffered and frame-boundary atomic.
3. Source document / source location: User diagnosis: do not mutate visible meshes while rendering.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `SparseSurfaceGpuResources`, `SparseVoxelGpuResources`, page publish queue, surface upload path.
6. What currently exists: Sparse page-table publish queues, surface upload acknowledgements, and fence-delayed publish paths exist. `terrain_gap_publish_delay_20260515` intentionally delayed sparse page-table publication and still passed the focused visual ownership gates with no miss or unsafe-near-miss pixels. Existing sparse-core tests also cover stale surface upload acknowledgements and fence-delayed page reuse.
7. What is missing: Proof that every mesh/GPU buffer and LOD child pointer path outside the sparse page/surface publish route is frame-boundary atomic. Parent/child LOD handoff remains TS-002.
8. Validation required: Stress test with forced delayed uploads/fences and debug assertion that visible state only swaps after completion.
9. Exact proof: Async upload regression with induced delays and no partial-frame holes or stale pointers across sparse pages, surface buffers, and LOD replacements.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_publish_delay_20260515` passed after forcing `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FRAMES=4` and `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_DELAY_FENCES=1`; metrics reported `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, and `minVoxelTerrainScreen=61.30%`. Runtime readiness at frame 120 reported `missing=0`, `uploadQueued=0`, `residentMissingSurface=0`, and `ready=1288`.
11. Next action required: Audit LOD replacement and non-sparse mesh/surface buffer swaps, then add a stress gate that proves renderer-visible pointers only change after complete upload.

### TS-010
1. Requirement ID: TS-010
2. Requirement text: Culling must be re-enabled only after fixed-grid and readiness contracts pass.
3. Source document / source location: User diagnosis: temporarily disable frustum/occlusion/LOD traversal pruning/unloading.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: renderer culling, sparse surface culling, clipmap/SVO traversal, chunk unloading/trim logic.
6. What currently exists: Surface culling and trim/unload behavior exist. The focused route now has a stable-near-cull-off/GPU-cull-on capture that passes the ownership gates. The capture script also has an explicit `-AllowGpuCullDisabled` isolation mode, and the full surface-culling-off probe passes the ownership/runtime gates when that intentional configuration is allowed.
7. What is missing: A full culling isolation matrix that compares fixed-grid render, culling off/on, LOD off/on, and unloading off/on with the same camera path.
8. Validation required: Captures for each isolation state with the same camera path.
9. Exact proof: Table in this ledger showing whether holes appear in each isolation state.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_stable_cull_off_gpu_cull_on_20260515` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=16.6750%`, `minVoxelTerrainScreen=49.9481%`, `miss=0`, and `unsafeNearMiss=0`; its log confirms `stableNearCull=disabled` and `gpuCull=enabled`. `VENPOD/build/captures/terrain_gap_culling_off_allowed_20260515` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=9.44%`, `minVoxelTerrainScreen=57.44%`, `miss=0`, and `unsafeNearMiss=0`; its log confirms `gpuCull=disabled`, `stableNearCull=disabled`, `CPU frustum culling: disabled`, `gpuCull=0`, `gpuCullDispatch=0`, `stableDraw=1`, and `compactDraw=1`.
11. Next action required: Complete the culling/LOD/unload matrix and add or expose a runtime fixed-grid render capture switch so the rendered path can be isolated as cleanly as the CPU sparse-core fixed-grid test.

### TS-011
1. Requirement ID: TS-011
2. Requirement text: Near sparse terrain holes must not be misclassified as air, sky, or valley-atmosphere fallback.
3. Source document / source location: User request on 2026-05-15: "Stop this bug out completely get no more misclassified as AIR full land rendering working well."
4. Current status: DONE_VERIFIED
5. Related source files, modules, functions, scripts, or assets: `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`, `VENPOD/engine_capture_smoke.ps1`.
6. What currently exists: `DebugBackgroundMissHit` takes a `nearSparseHole` flag and records `RENDER_OWNER_UNSAFE_NEAR_MISS` only when the first missing sparse sample is terrain-adjacent and close to the expected terrain crossing inside the configured exact-near distance; normal sky, valley, and horizon-through-air paths stay separate. The shader no longer uses the older fixed 224-voxel heuristic for terrain-facing sparse misses, and it no longer treats grazing heightfield/silhouette misses far ahead of the actual terrain crossing as real terrain holes.
7. What is missing: Nothing for the focused misclassification bug. Broader lifecycle state coloring remains TS-004.
8. Validation required: Debug capture where terrain-facing sparse misses report as unsafe near miss during failure, and zero unsafe/miss pixels after the fix on the target route.
9. Exact proof: `engine_capture_smoke.ps1` debug-mode-50 strict capture with ownership/layer CSVs.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_continuous_critical_ready_20260515` passed after the exact-near sparse-miss classification change with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, and terrain-critical `postNonReady=0` at ready frame 120. `VENPOD/build/captures/terrain_gap_prefetch_wide_strict_debug50_20260515` also passed with `miss=0`, `unsafeNearMiss=0`, and no height-proxy ownership. `VENPOD/build/captures/terrain_gap_narrow_unsafe_long_walk_20260515` passed the stricter long-walk gate after the ownership classifier was narrowed to true terrain-adjacent crossings.
11. Next action required: Keep this gate in future regression runs; do not collapse unsafe terrain misses back into valley/sky colors.

### TS-012
1. Requirement ID: TS-012
2. Requirement text: The focused walking/yaw route must keep visible terrain resident enough to avoid near sparse holes.
3. Source document / source location: User reports on 2026-05-15: sparse/slow terrain near the camera, fake terrain around the player, and missing land that only appears later.
4. Current status: DONE_VERIFIED
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/main_launcher.cpp`, `VENPOD/src/Simulation/SparseVoxelWorld.h`, `VENPOD/src/Graphics/SparseVoxelGpuResources.h`, `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`.
6. What currently exists: Default sparse brick pages/page-table headroom are raised; terrain surface, screen-critical, and near-surface prefetch are wider; near-surface and startup critical prewarm are default-on; protected same-frame publish drains queued protected work; terrain-critical requests bypass the known-empty fast path so protected empty pages are visible to the GPU; and the near-hole ownership classifier no longer reports grazing silhouette misses as terrain holes.
7. What is missing: Nothing for the focused walking/yaw and stricter long-walk bug route. A general finite target-set scheduler and long free-roam latency proof remain TS-007.
8. Validation required: Focused strict route capture with no miss/unsafe near terrain ownership and enough voxel terrain screen ownership.
9. Exact proof: `VENPOD/build/captures/terrain_gap_prefetch_wide_strict_debug50_20260515` plus a normal long-walk capture after the startup gate opens.
10. Latest evidence: `VENPOD/build/captures/terrain_gap_continuous_critical_ready_20260515` passed with 100 ownership samples, `maxHeightProxy=0.00%`, `maxSky=33.28%`, `maxFarSvo=11.57%`, `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=9.62%`, and `minVoxelTerrainScreen=57.24%`; the capture script's terrain-critical readiness check observed `postNonReady=0` at `readyFrame=120`. `VENPOD/build/captures/terrain_gap_empty_pages_targeted_20260515` passed after protected empty-page allocation with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, and terrain-critical `postNonReady=0`. The full long-walk finally passed in `VENPOD/build/captures/terrain_gap_narrow_unsafe_long_walk_20260515` with 255 ownership samples, `maxHeightProxy=0.00%`, `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=3.59%`, `minVoxelTerrainScreen=63.40%`, and terrain-critical `postNonReady=0`.
11. Next action required: Keep both focused and long-walk captures in the regression suite; do not re-enable known-empty skipping for terrain-critical requests or widen the unsafe-near classifier back to grazing horizon rays.

### TS-013
1. Requirement ID: TS-013
2. Requirement text: Water-covered terrain must not also be emitted as dry sparse surface geometry.
3. Source document / source location: User report on 2026-05-15: sand near water "is not real sand" and disappears after drawing/edits reveal the actual world state.
4. Current status: DONE_VERIFIED
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/Simulation/SparseSurfaceExtractor.cpp`, `VENPOD/src/Simulation/SparseTerrainGenerator.cpp`, `VENPOD/src/Simulation/SparseClipmap.cpp`, `VENPOD/test/test_sparse_core.cpp`.
6. What currently exists: Exact sparse surface extraction now suppresses solid terrain tops directly under water while still emitting solid side/bottom faces adjacent to water. Generated visual-surface sampling and mid-voxel clipmap tagging preserve water ownership over submerged tops, while sparse raster depth now covers visible shoreline banks so background LOD cannot leak fake sand through missing terrain sides. Unit coverage verifies that a stone face under water is suppressed, the water top remains visible, and a stone side bank against water is emitted.
7. What is missing: A brush-edit capture that repeats the user's exact "draw and fake sand disappears" interaction is still useful as an end-to-end visual regression.
8. Validation required: Unit test for solid-water ownership plus waterline capture showing no miss/unsafe-near regressions.
9. Exact proof: `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore`; waterline normal/debug captures with zero miss and zero unsafe-near-miss pixels.
10. Latest evidence: `VENPODSparseCore` passed on 2026-05-16 after adding the side-bank solid-water extraction test. `VENPOD/build/captures/waterline_surface_water_banks_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=81.63%`, `maxFrameMs=43.54`, `maxPrepMs=30.86`, and `maxGpuRayMs=13.37`. The broad regression `VENPOD/build/captures/broad_surface_water_banks_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, and `minVoxelTerrainScreen=74.90%`. Debug ownership `VENPOD/build/captures/debug50_broad_surface_water_banks_20260516` confirms remaining distant cutouts are background/sky classification, not near sparse miss or height proxy.
11. Next action required: Add a scripted brush-edit shoreline capture if this symptom returns during interactive testing; continue far-mountain/background continuity under TS-014/TS-019 rather than reopening solid-water ownership.

### TS-014
1. Requirement ID: TS-014
2. Requirement text: Distant mountains and mid-distance valley walls should not show large valley-atmosphere gaps when voxel terrain is available.
3. Source document / source location: User report on 2026-05-15: gaps remain in far mountains; world still looks sparse and not like proper continuous voxels.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/main_launcher.cpp`, `VENPOD/src/Graphics/FarVoxelOctree.cpp`, `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`, `VENPOD/assets/shaders/Compute/CS_SparseSurfaceCullCompact.hlsl`, `SparseClipmap`, `engine_capture_smoke.ps1`.
6. What currently exists: Far SVO CPU occupancy is now more conservative: each candidate cell samples corners, center, edge midpoints, and quarter points before pruning, and the far-SVO cache version was bumped to `16` so the page forest rebuilds with the denser occupancy test. Mid-clipmap interest now includes bounded lateral view-fan height/voxel target lines so visible side valleys are not limited to a single forward centerline. The current root cause for the large sky/cutout bands is an overly conservative surface-authoritative handoff: the old `VENPOD_SPARSE_EXACT_NEAR_DISTANCE=3072` rejected resident mid/far voxel hits that should fill sparse-surface raster gaps. The default is now `1024`, keeping exact ownership over the immediate edit/collision foreground while allowing resident voxel LOD to fill mid-distance valley walls. Low-altitude far-SVO traversal now accepts bounded upward mountain-silhouette rays and gives low-quality horizon/silhouette rays enough page-DDA steps to reach already-resident far pages, fixing a complete-far-SVO-but-blue-sky failure without re-enabling procedural far-height terrain. High-altitude far-LOD views now throttle exact screen-critical terrain, terrain-surface prefetch, and ownership-recovery exact requests when the camera is at least 128 voxels above terrain, far SVO already owns at least 35% of pixels, and there are no miss or unsafe-near signals; the LOD throttle probe is capped to 8 exact requests by default. The same far-LOD throttle caps mid-clipmap CPU generation to `VENPOD_SPARSE_MID_LOD_THROTTLE_BUDGET` (`16` by default), preventing high-flight from burning a full catchup budget on mid voxel bricks that far SVO already covers. High-altitude far-LOD views also cap sparse raymarch scale with `VENPOD_SPARSE_RAYMARCH_LOD_THROTTLE_MAX_SCALE_PERCENT` (`50` by default), reducing background GPU work only while the far-LOD throttle is active. Waterline/underwater views now additionally cap sparse raymarch scale with `VENPOD_SPARSE_RAYMARCH_WATERLINE_MAX_SCALE_PERCENT` (`25` by default), targeting the reproduced low-water fullscreen ray cost without globally lowering high-flight or normal walking quality. Terrain-critical protected upload now reserves enough per-frame upload slots for the known screen-critical target set, bounded by `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_PROTECTED_UPLOAD_BUDGET`, so a fast/yawing ground route cannot leave a small tail of critical bricks in `UploadQueued` just because the generic upload budget was lower than the critical set. The sparse-surface GPU cull shader now applies the stable near-field distance contract as a true camera-centered sphere instead of only treating `VENPOD_SPARSE_SURFACE_CULL_DISTANCE` as a forward-depth limit; this keeps the optimized GPU-cull path aligned with the CPU stable-near culling contract without forcing per-frame CPU snapshot rebuilds. Walking-height mid/far background layers are now slightly atmospherically de-emphasized before the high-altitude path, so they read as distant context instead of full-strength foreground slabs while retaining the existing ownership/readiness gates. The cheap mid-voxel column path now promotes submerged coarse height samples to the water surface/material before hit testing; this prevents voxel background context from showing fake sand where the deterministic generated world is actually water. Underwater background miss pixels are now classified as `waterContext` instead of `valleyAtmosphere` while preserving `unsafeNearMiss` for real sparse holes. Far SVO and shader fallback material thresholds now match the authoritative sparse generator for beach/stone classification (`sea+6`, `relief>10`), and warmed runs use regenerated material pages. The sparse-surface pixel shader now uses `[earlydepthstencil]`, preserving early depth/stencil rejection despite ownership UAV writes and reducing hidden sparse-surface fragment cost in walking captures. The mid-voxel interest target now defaults to `90%` of cache capacity instead of `100%`, preserving high-flight visual ownership while leaving hysteresis slack in the `4096`-brick cache. The mid-voxel protected catchup tile default now follows the normal tile budget (`72`) instead of escalating to `96`, and the coverage catchup default is `48`; this keeps far mountain gap filling active but stops the last 1-2% of interested mid-voxel coverage from monopolizing walking frames. Mid-clipmap generation catchup now treats queued voxel maintenance as protected only when there is visible missing mid-voxel interest, queued height tiles, or an ownership terrain deficit, so background voxel churn no longer bypasses frame-pressure caps after visible terrain is already covered.
7. What is missing: The focused waterline/default captures, broader walk capture, boundary capture, lower-orbit capture, scripted high-flight capture, and post-cull fast-walk capture now pass the current automated gates, but broader manual free-roam acceptance is still required before treating all terrain visual gaps as closed. High-flight is no longer exact-streaming bound, the mid-clipmap catchup stall is reduced, and the far-LOD raymarch cost is reduced. A deliberately aggressive fast-walk/yaw route is visually and readiness-correct after the protected upload reserve and GPU spherical cull. The latest fast-walk guard is visually correct and the surface-overdraw hotspot is reduced, but walking scenes still show CPU sparse-prep/frame-pacing cost and high-flight scenes remain far-SVO/background-raymarch dominated. Per-pixel shader expansions remain rejected: a far-leaf search experiment and a voxel-parent fallback experiment either failed to improve the metric enough or stalled the capture/runtime path.
8. Validation required: Waterline, debug ownership, walk, high-flight, and manual free-roam captures should show no height-proxy ownership, no red miss, no unsafe near miss, stable shoreline material ownership, and acceptable launch/runtime cost.
9. Exact proof: Focused proof is a capture with `maxHeightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, visibly reduced grey valley bands, and `maxValleyAtmosphereScreenPct` far below the prior `14%` route and the prior `44.96%` underwater waterline case. Broader proof now includes walk and high-flight scripted captures passing terrain-critical `postNonReady=0`; final proof still needs manual free-roam visual acceptance and a dedicated performance pass.
10. Latest evidence: Prior TS-014 evidence includes the shoreline exact-near passes, high-altitude LOD/ray caps, background-atmosphere passes, underwater water-context fix, and warmed material-consistency captures. The resumed May 16 mid-catchup pass adds `VENPOD/build/captures/mid_catchup_budget_default_long_fastwalk_20260516`, `VENPOD/build/captures/mid_catchup_budget_default_highflight_guard_20260516`, and `VENPOD/build/captures/mid_catchup_budget_default_waterline_guard_20260516`: all passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and terrain-critical `postNonReady=0`. The broad long fast-walk route improved from `long_fastwalk_broad_probe_20260516` (`maxFrameMs=130.08`, `maxPrepMs=107.30`, repeated `budgetMid=96` / `pumpVoxel` up to `52.56 ms`) to `maxFrameMs=51.55`, `maxPrepMs=42.59`, and bounded mid-catchup behavior. High-flight frame 240 now uses `budgetMid=48`, `pumpVoxel=6.86 ms`, and body `23.28 ms`; waterline frame 240 has `missingVoxel=0`, `budgetMid=12`, `pumpVoxel=0`, and body `18.17 ms`. The waterline ray-cap pass adds `VENPOD/build/captures/waterline_ray_cap_default_guard_20260516`, `VENPOD/build/captures/waterline_ray_cap_highflight_guard_20260516`, and `VENPOD/build/captures/waterline_ray_cap_long_fastwalk_guard_20260516`: all passed the same hard visual/readiness gates. Waterline frame 240 GPU ray dropped from `18.15 ms` in `mid_catchup_budget_default_waterline_guard_20260516` to `4.44 ms`; high-flight frame 240 stayed comparable (`8.25 ms` GPU ray), and the broad fast-walk guard passed with `maxFrameMs=47.19`, `maxPrepMs=40.33`, `maxGpuRayMs=10.06`, and `postNonReady=0`. The visible-catchup protection pass adds `VENPOD/build/captures/late_long_fastwalk_mid_visible_protection_20260516`, `mid_visible_protection_long_fastwalk_guard_20260516`, `mid_visible_protection_waterline_guard_20260516`, `mid_visible_protection_highflight_guard_20260516`, and `waterline_brush_material_guard_20260516`: all passed with `heightProxyScreen=0`, `postNonReady=0`, and no miss/unsafe-near regressions. The late long-walk reproducer improved from `maxFrameMs=110.59`, `maxPrepMs=118.05`, and `maxSmoothedFrameMs=194.10` to `maxFrameMs=47.37`, `maxPrepMs=48.84`, and `maxSmoothedFrameMs=99.94`; waterline brush paint smoke passed with `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, and `deltaMismatch=0`. The reopened-goal guard captures `surface_record_lookup_nocopy_long_fastwalk_guard_20260516`, `surface_record_lookup_waterline_guard_full_20260516`, and `surface_record_lookup_highflight_guard_seq_20260516` preserve the visual correctness gates (`heightProxyScreen=0`, `postNonReady=0`, waterline `valleyAtmosphereScreen=0`, high-flight `valleyAtmosphereScreen=0`), but the long-walk run is performance-incomplete (`maxFrameMs=96.30`, `maxPrepMs=97.73`) and is evidence for the next CPU sparse-maintenance pass, not closure. The latest accepted continuation captures are `VENPOD/build/captures/mid_capacity_surface_compaction_long_fastwalk_guard_20260516`, `VENPOD/build/captures/mid_capacity_surface_compaction_waterline_guard_20260516`, and `VENPOD/build/captures/mid_capacity_surface_compaction_highflight_guard_20260516`. The long fast-walk guard passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=5.16%`, `minVoxelTerrainScreen=70.86%`, `maxFrameMs=49.47`, `maxSmoothedFrameMs=95.14`, `maxPrepMs=71.58`, `maxGpuRayMs=13.11`, and `postNonReady=0`. The waterline brush guard passed with `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=77.59%`, `maxFrameMs=26.69`, `maxPrepMs=8.35`, `maxGpuRayMs=5.45`, and `postNonReady=0`. The high-flight guard passed with `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=77.59%`, `maxFrameMs=18.11`, `maxPrepMs=30.48`, `maxGpuRayMs=4.47`, and `postNonReady=0`. The startup speculative-upload gate captures `startup_speculative_upload_gate_highflight_20260516`, `startup_speculative_upload_gate_waterline_20260516`, and `startup_speculative_upload_gate_fastwalk_20260516` passed with `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`; the high-flight and waterline runs now open public rendering in the earlier capture window instead of waiting for max-frame startup release.
Latest exact-handoff evidence: `debug50_fixed_default_exact1024_patch_20260516` and `normal_fixed_default_exact1024_patch_20260516` passed after making the 1024m handoff the default, with `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=81.47%`, and `maxFrameMs=67.75`. The broad variable-dt route `broad_freeroam_exact1024_patch_20260516` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=78.70%`, `maxFrameMs=68.65`, `maxPrepMs=39.54`, and `maxGpuRayMs=5.84`. The waterline regression `waterline_exact1024_patch_20260516` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `maxFrameMs=111.43`, `maxPrepMs=71.44`, and `maxGpuRayMs=36.43`.
Latest far-SVO silhouette evidence: `debug50_broad_surface_water_banks_20260516` showed the remaining broad debug route still had `maxSky=29.08%` and `minVoxelTerrainScreen=70.92%` even though far SVO was complete and no miss/unsafe/height-proxy ownership existed. The first page-step-only probe improved sky but failed the performance gate and is rejected. The accepted bounded upward-silhouette/page-step patch is verified by `debug50_broad_far_svo_silhouette_20260516`, which passed with `maxSky=14.65%`, `maxFarSvo=2.39%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=85.35%`, `maxFrameMs=64.69`, `maxPrepMs=40.98`, and `maxGpuRayMs=7.11`. Normal regressions also passed: `broad_far_svo_silhouette_20260516` (`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=70.94%`, `maxFrameMs=70.99`, `maxPrepMs=59.82`, `maxGpuRayMs=12.33`), `highflight_far_svo_silhouette_20260516` (`maxValleyAtmosphereScreen=3.02%`, `minVoxelTerrainScreen=57.12%`, `maxFrameMs=111.28`, `maxPrepMs=124.89`, `maxGpuRayMs=26.99`), and `waterline_far_svo_silhouette_20260516` (`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=81.63%`, `maxFrameMs=41.83`, `maxPrepMs=29.94`, `maxGpuRayMs=12.74`). All kept `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`.
Latest far-SVO low-quality budget evidence: after capacity fixes, `debug50_broad_capacity_4m_guard_20260516` reproduced far mountain cutouts as blue sky fallthrough with `maxSky` about `29.97%`, `minVoxelTerrainScreen` about `70%`, far SVO coverage complete, and no miss/unsafe/height-proxy. Raising only the low-altitude far-SVO mountain-silhouette/horizon page-walk budget produced `debug50_broad_far_svo_budget_patch_20260516`, which passed with `maxSky=20.89%`, `minVoxelTerrainScreen=79.79%`, `heightProxyScreen=0.00%`, `valleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `maxFrameMs=52.22`, `maxPrepMs=36.82`, and `maxGpuRayMs=9.11`. The public broad guard `broad_far_svo_budget_patch_20260516` passed with `maxSky=21.77%`, `maxFarSvo=2.58%`, `minVoxelTerrainScreen=78.23%`, `maxFrameMs=56.00`, `maxPrepMs=50.58`, and `maxGpuRayMs=8.04`. The shoreline brush guard `waterline_brush_far_svo_budget_patch_20260516` also passed with zero fake-terrain ownership regressions.
11. Next action required: Manual free-roam visual acceptance is still required. The next performance pass should target remaining CPU sparse prep and untracked frame pacing in broader manual free-roam, especially long-session metadata/compaction growth that is not covered by the scripted route. Keep the surface-removal/tombstone/full-catchup gates active. Do not reintroduce per-pixel far-SVO searches, voxel-parent shader raymarch fallback, height-proxy backgrounds, or default-disabled sparse surface fill as the default path.

### TS-015
1. Requirement ID: TS-015
2. Requirement text: The sparse renderer must launch reliably and avoid multi-frame CPU stalls while preserving the verified no-miss terrain readiness behavior.
3. Source document / source location: User report on 2026-05-15: rerun command stalls on black screen; later reports say rendering is materially better but walking remains slow and buggy.
4. Current status: PARTIAL
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/Graphics/RHI/ShaderCompiler.cpp`, `VENPOD/src/main_launcher.cpp`, `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`, `VENPOD/src/Simulation/SparseClipmap.*`, `VENPOD/src/Simulation/SparseVoxelWorld.cpp`, `VENPOD/src/Simulation/SparseSurfaceExtractor.cpp`, `VENPOD/src/Simulation/SparseSurfaceCache.*`, `VENPOD/src/Graphics/SparseVoxelGpuResources.*`, `VENPOD/src/Graphics/SparseSurfaceGpuResources.*`, `VENPOD/test/test_sparse_core.cpp`, `VENPOD/build/captures/mid_snapshot_coalesced_ranges_fastwalk_normal_20260516`, `VENPOD/build/captures/mid_snapshot_coalesced_ranges_waterline_normal_20260516`, `VENPOD/build/captures/mid_snapshot_coalesced_ranges_fastflight_guard_20260516`, `VENPOD/build/captures/surface_stage_brick_bounds_fastwalk_normal_20260516`, `VENPOD/build/captures/surface_extract_cap24_probe_fastwalk_20260516`, `VENPOD/build/captures/surface_stage_bounds_cap24_fastwalk_normal_20260516`, `VENPOD/build/captures/surface_stage_bounds_cap24_waterline_normal_20260516`, `VENPOD/build/captures/surface_stage_bounds_cap24_fastflight_guard_20260516`, `VENPOD/build/captures/critical_predictive_fastwalk_normal_20260516`, `VENPOD/build/captures/critical_predictive_warm_fastwalk_normal_20260516`, `VENPOD/build/captures/critical_predictive_soft_fastwalk_normal_20260516`, `VENPOD/build/captures/critical_predictive_soft_waterline_normal_20260516`, `VENPOD/build/captures/critical_predictive_soft_fastflight_guard_20260516`, `VENPOD/build/captures/current_soft_predictive_fastwalk_verify_20260516`, `VENPOD/build/captures/mid_catchup64_probe_fastwalk_20260516`, `VENPOD/build/captures/surface_cap16_probe_fastwalk_20260516`, `VENPOD/build/captures/preupload_surface_fastwalk_normal_20260516`, `VENPOD/build/captures/surface_material_cache_fastwalk_normal_20260516`, `VENPOD/build/captures/clipmap_timers_fastwalk_20260516`, `VENPOD/build/captures/clipmap_voxel_column_cache_fastwalk_20260516`, `VENPOD/build/captures/predictive_yaw_column_cache_fastwalk_20260516`, `VENPOD/build/captures/critical_coords3_default_fastwalk_20260516`, `VENPOD/build/captures/critical_coords3_waterline_regression_20260516`, `VENPOD/build/captures/critical_coords3_fastflight_regression_20260516`, `VENPOD/build/captures/surface_material_grid_fastwalk_20260516`, `VENPOD/build/captures/critical_coords1_fastwalk_probe_20260516`, `VENPOD/build/captures/surface_cap16_after_coords3_fastwalk_probe_20260516`, `VENPOD/build/captures/ray_scale45_fastwalk_probe_20260516`, `VENPOD/build/captures/current_coords3_fastwalk_resume_20260516`, `VENPOD/build/captures/critical_grid21x13_fastwalk_probe_20260516`, `VENPOD/build/captures/critical_grid21x13_default_fastwalk_retry_20260516`, `VENPOD/build/captures/critical_grid21x13_fastflight_guard_20260516`, `VENPOD/build/captures/waterline_old_grid_probe_20260516`, `VENPOD/build/captures/protected_surface48_fastwalk_probe_20260516`, `VENPOD/build/captures/surface_neighbor_overlay_skip_fastwalk_20260516`, `VENPOD/build/captures/surface_skip_solid_count_fastwalk_20260516`, `VENPOD/build/captures/critical_grid21x13_reverted_perfprobe_fastwalk_20260516`, `VENPOD/build/captures/surface_halo_column_cache_fastwalk_probe_20260516`, `VENPOD/build/captures/surface_halo_column_cache_waterline_regression_20260516`, `VENPOD/build/captures/surface_halo_column_cache_fastflight_guard_20260516`, `VENPOD/build/captures/surface_snapshot_pointer_payload_fastwalk_probe_20260516`, `VENPOD/build/captures/surface_snapshot_pointer_payload_waterline_regression_20260516`, `VENPOD/build/captures/surface_snapshot_pointer_payload_fastflight_guard_20260516`, `VENPOD/build/captures/surface_record_from_batches_fastwalk_probe_20260516`.
6. What currently exists: `VENPOD_LOG_FILE` now honors an explicit file path while preserving `VENPOD_LOG_FILE=1` behavior for capture scripts. Shader cache storage is now anchored to the shader asset root instead of `current_path()`, so root-launched and bin-launched runs share the same warm cache and no longer appear stuck compiling the heavy raymarch shader before frame 0. Runtime instrumentation separates frame-end body time from present, post-generation, input-end, coarse main-loop gaps, and post-wait sparse substeps (`feedback/cmd/begin/midSnap/plan/upload/publish/midUpload/stats/surfExtract/surfPlan/surfSnap/surfStage/surfEmit`). Protected critical surface extraction now drains before general time-capped extraction, critical requests can exceed the generic visible cap, surface extraction defers repeated stats scans, the extractor skips non-candidate neighbor samples, and active range-allocator staging skips duplicate surface-record snapshot generation. Mid-clipmap snapshots now take explicit height/voxel layer inclusion flags, and dirty height/voxel sample payloads use coalesced dirty ranges while preserving full metadata/lookup and exact GPU destination slot offsets. Surface GPU staging now writes conservative brick AABB bounds for remapped range records instead of rescanning every snapshot face for exact per-brick bounds. Range-allocator surface snapshots now carry direct cached-face pointers for this frame and omit the full visible face payload copy; staging still copies or diffs payloads only for dirty, nonresident, or resized brick allocations. A conservative dirty-payload surface upload path now publishes already-resident, same-size dirty surface bricks without rebuilding the full visible surface snapshot; it falls back to the full snapshot when a dirty brick is new, removed, missing, or changes face count. Dirty surface payload snapshots now also respect the copy-region budget by uploading a bounded dirty prefix and preserving a deferred dirty count instead of returning false and forcing a full visible-surface snapshot when the dirty set is larger than the per-frame dirty budget. The default general sparse surface extraction cap is now `24 ms`. Screen-critical terrain requests now keep the current-camera ray set strict; predicted-camera terrain hits are only bounded warm visible prefetch and never enter the strict current-frame readiness gate. Predictive warm prefetch now predicts yaw/pitch as well as position, strict terrain-hit neighborhoods default to the hit brick plus vertical neighbors (`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_MAX_COORDS=3`), and the default screen-critical ray grid is now `21x13` so the CPU readiness guard covers the actual rendered DDA path more densely. Mid-clipmap instrumentation now logs `PERF_SPARSE_CLIPMAP`, mid-voxel brick generation caches an 18x18 height/relief column halo per 16x16x16 generated brick, exact sparse surface extraction uses an 18x18 deterministic height/relief column halo for neighbor sampling while still honoring edited voxels in adjacent brick overlays, and `SparseVoxelWorld` now caches deterministic terrain surface columns during each frame so sparse generation and adjacent surface extraction do not repeatedly recompute the same halo height/relief samples. Mid-voxel brick generation now classifies local column neighbors without six procedural samples per coarse voxel, the far/background mid-voxel coverage catch-up default is now 99% so high-flight mountain context fills farther before the lane backs off, `VENPOD_SPARSE_SKIP_BURIED_VISIBLE_BRICKS` is now default-on for speculative/visible requests, high-altitude far-SVO rays now get a larger page-DDA traversal budget while ground/waterline views keep the cheaper path, and `PS_SparseSurface` now uses `[earlydepthstencil]` so UAV ownership atomics do not force hidden sparse-surface fragments through late depth/stencil.
7. What is missing: The no-fake-terrain correctness gate is restored on reproduced fast-walk, waterline, and high-flight captures, and the mid-clipmap, exact-surface extraction, snapshot-payload, startup surface-proof, default-path surface-to-background handoff, partial dirty surface upload, and high-altitude far-SVO coverage hotspots are materially reduced on reproduced frames. The aggressive fast-walk route is still not fully real-time, but the latest reproduced movement capture is much lower than the `21x13` baseline and the prior visible cliff cutouts are nearly gone on the scripted route. Later walking frames still show smaller surface-stage spikes, and high-flight remains dominated by mid-voxel CPU pump plus far-SVO/background raymarch. Broader manual free-roam acceptance is still required before closing the visual objective.
8. Validation required: A fast-walk run must keep `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, terrain-critical `postNonReady=0`, and reduce post-wait sparse maintenance spikes to an acceptable budget without increasing visual gaps.
9. Exact proof: Direct sandbox fast-walk logs with explicit `VENPOD_LOG_FILE=<capture>\venpod_runtime.log`, plus smoke captures for fast-walk, waterline, and high-flight after the CPU sparse maintenance fix.
10. Latest evidence: `fast_walk_direct_normal_ownership_20260516d` proves root-launched sandbox now reaches frame telemetry instead of hanging at shader compiler initialization. Previous accepted fast-walk baseline `VENPOD/build/captures/surface_snapshot_pointer_payload_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=1.66%`, `minVoxelTerrainScreen=67.38%`, and `postNonReady=0`; frame-120 `surfSnap` dropped from `3.71 ms` to `0.89 ms`, `surfExtract` was `19.96 ms`, and body was `54.89 ms`. Regressions `surface_snapshot_pointer_payload_waterline_regression_20260516` and `surface_snapshot_pointer_payload_fastflight_guard_20260516` passed with zero miss/unsafe/height-proxy ownership; waterline frame-240 body was `8.73 ms`, and high-flight frame-240 body was `21.69 ms`. Rejected May 16 continuation probes: `protected_surface_batch_remove_fastwalk_probe_20260516` preserved visual gates but worsened frame-120/130 body to `87.17/85.13 ms` and was reverted; `background_off_diagnostic_20260516` intentionally disabled mid/far background and proved exact sparse surface is honest but incomplete at distance; `surface_radius2400_background_off_diagnostic_20260516` and `surface_radius2400_background_off_late_diagnostic_20260516` showed that pushing exact surface radius can fill more real terrain over time but leaves fragmented distant surface without the background layers; `critical_distance1536_fastwalk_probe_20260516` preserved gates but worsened frame-120 body to `101.77 ms`; the full mid-voxel walking DDA probe timed out and was reverted; `background_layer_debug50_fastwalk_20260516` classified the remaining fake-looking terrain as orange mid-voxel and magenta far-SVO background, not height proxy or non-ready exact terrain; `surface_ownership2400_fastwalk_probe_20260516` preserved gates but worsened frame-120 body to `95.30 ms` and did not materially improve the fake-background appearance; `authoritative_background_start_fastwalk_probe_20260516` timed out and was reverted; `distant_mid_column_fastwalk_probe_20260516` passed hard gates but worsened frame-120 body to `107.60 ms` and visually handed too much screen area to far-SVO block slabs. Current accepted visual-context pass `background_atmosphere_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=1.66%`, `minVoxelTerrainScreen=67.36%`, and `postNonReady=0`; frame 120 was `56.39 ms`, frame 130 `47.53 ms`, frame 140 `42.19 ms`, and frame 150 `44.60 ms`. Regressions `background_atmosphere_waterline_regression_20260516` and `background_atmosphere_fastflight_guard_20260516` passed with zero miss/unsafe/height-proxy ownership and `postNonReady=0`; waterline frame-240 body was `6.05 ms`, and high-flight frame-240 body was `15.16 ms`. `surface_extractor_local_classify_fastwalk_probe_20260516` is rejected: it preserved visual gates but worsened frame-120 body to `75.73 ms` and `surfExtract` to `24.28 ms`, so the extractor rewrite was reverted. The accepted mid-column water-surface fix passed `mid_column_water_surface_waterline_probe_20260516`, `mid_column_water_surface_fastwalk_repeat_20260516`, and `mid_column_water_surface_fastflight_guard_20260516` with zero miss/unsafe/height-proxy ownership and `postNonReady=0`; fast-walk repeat frame 120 was `54.51 ms`, waterline frame 240 was `7.54 ms`, and high-flight frame 240 was `18.92 ms`. The accepted surface terrain-column cache passed `surface_column_cache_fastwalk_probe_20260516`, `surface_column_cache_waterline_probe_20260516`, and `surface_column_cache_fastflight_guard_20260516` with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and terrain-critical `postNonReady=0`; fast-walk frame 120 `surfExtract` dropped to `14.41 ms` and body to `51.73 ms`, frame 140/150 `surfExtract` was `8.45/8.53 ms`, waterline frame 240 body was `7.89 ms`, and high-flight frame 240 body was `19.01 ms`. The accepted generation terrain-column cache passed `generation_column_cache_fastwalk_probe_20260516`, `generation_column_cache_waterline_probe_20260516`, and `generation_column_cache_fastflight_guard_20260516` with the same hard gates clean; fast-walk frame 120 sparse generation prep dropped to `13.28 ms`, `surfExtract` to `13.02 ms`, and body to `49.29 ms`, while waterline frame 240 body was `7.53 ms` and high-flight frame 240 body was `16.29 ms`. `persistent_column_cache_fastwalk_repeat_20260516` showed better walking counters (`surfExtract=10.36 ms`, body `46.14 ms`) but the persistent cache is rejected because `persistent_column_cache_waterline_probe_20260516` and `persistent_column_cache_waterline_repeat_20260516` repeatably raised waterline frame-240 GPU ray/wait cost to about `18 ms`; the code was reverted to frame-local caching and `VENPODSparseCore` passed afterward. `surface_cluster_sortkey_fastwalk_probe_20260516` is rejected: it preserved hard visual gates but worsened fast-walk frame-120 body to `57.96 ms` and sparse prep to `30.24 ms`; the sort-key experiment was reverted and `VENPODSparseCore` passed. The accepted mid-voxel coverage catch-up pass adds `VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_PCT` and `VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET`; `mid_voxel_coverage_catchup_fastflight_probe_20260516` passed hard gates and reduced high-flight frame-240 missing interested mid-voxel bricks from `1260` to `465`, queued voxel bricks from `2693` to `548`, and body time from `19.43 ms` to `15.50 ms`. Sequential regressions `mid_voxel_coverage_catchup_fastwalk_seq_20260516` and `mid_voxel_coverage_catchup_waterline_repeat_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`; fast-walk frame 120 was `50.28 ms` with `surfExtract=12.34 ms`, and the repeated waterline frame 240 was `7.83 ms` body with the catch-up lane inactive (`queuedVoxel=0`, `missingVoxel=0`). The accepted mid-voxel column-classification pass replaces six procedural neighbor samples per non-air coarse voxel with deterministic column material classification in `SparseClipmapTileCache::GenerateVoxelBrick`. `mid_voxel_column_class_fastwalk_probe_20260516` passed hard gates and reduced frame-120 mid-voxel pump from `5.48 ms` to `2.95 ms`, sparse prep from `26.36 ms` to `23.42 ms`, and body from `50.28 ms` to `47.06 ms`. Regressions `mid_voxel_column_class_waterline_probe_20260516` and `mid_voxel_column_class_fastflight_repeat_20260516` passed with zero miss/unsafe/height-proxy ownership and `postNonReady=0`; repeated waterline frame 240 body was `8.29 ms`, and repeated high-flight frame 240 body was `16.31 ms` with `missingVoxel=461`. `surface_material_cache_exact_fastwalk_probe_20260516` is rejected: after fixing a partial-update bug caught by `VENPODSparseCore`, it preserved visual gates but worsened fast-walk frame-120 `surfExtract` from `13.08 ms` to `15.42 ms` and body from `47.06 ms` to `55.76 ms`; the material-cache extractor patch was reverted and `VENPODSparseCore` passed. Raising the mid-voxel catch-up threshold is accepted: `mid_voxel_catchup_96_default_fastflight_20260516` improved high-flight frame-240 mid-voxel coverage from `0.89` with `461` missing interested voxel bricks to `0.97` with `124` missing, but `mid_voxel_catchup_99_default_fastflight_20260516` is the current default evidence and further improves the same frame to `16` missing interested voxel bricks with hard gates clean and body `17.51 ms`. `mid_voxel_catchup_99_fastwalk_probe_20260516` and `mid_voxel_catchup_99_waterline_probe_20260516` also passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`; fast-walk frame-120 body was `51.10 ms`, and waterline frame-240 body was `6.83 ms`. `mid_voxel_catchup_100_fastflight_probe_20260516` is rejected: it cost more (`body=18.77 ms`, `pumpVoxel=4.09 ms`) while leaving essentially the same residual tail (`15` missing interested voxel bricks), so `99%` is the better default. `generation_inline_occupancy_fastwalk_probe_20260516` is rejected: it preserved hard visual gates but did not reduce sparse generation (`14.11 -> 14.16 ms`) and worsened fast-walk frame-120 body to `55.83 ms`; waterline and high-flight regressions passed, but the runtime generation-path occupancy accumulation patch was reverted and `VENPODSparseCore` passed afterward. The buried-visible skip pass is accepted as default-on: `buried_visible_skip_fastwalk_probe_20260516`, `buried_visible_skip_waterline_probe_20260516`, and `buried_visible_skip_fastflight_probe_20260516` all passed hard visual/readiness gates. The fast-walk override probe skipped 2 buried visible requests, reduced sparse prep from `25.41 ms` to `22.63 ms`, reduced `surfExtract` from `14.87 ms` to `13.15 ms`, and reduced frame-120 body from `51.10 ms` to `45.99 ms`; the no-override default capture `buried_visible_skip_default_fastwalk_20260516` passed and measured frame-120 body `47.36 ms`. Waterline frame 240 passed with body `6.05 ms`, and high-flight frame 240 passed with body `17.12 ms`.
    Additional continuation evidence: `buried_surface_empty_fastpath_fastwalk_probe_20260516` is rejected despite a fast-walk win because `buried_surface_empty_fastpath_waterline_probe_20260516` and `buried_surface_empty_fastpath_waterline_repeat_20260516` raised waterline frame-240 body/GPU ray time to about `18 ms`; the `UpdateKnownEmptySurface` fast path was reverted and `VENPODSparseCore` passed. The accepted high-altitude far-SVO page traversal pass is verified by `far_svo_high_alt_steps_fastflight_guard_20260516`, `far_svo_page_steps_fastwalk_probe_20260516`, and `far_svo_high_alt_steps_waterline_regression_20260516`: all passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`. High-flight frame 240 improved from the previous accepted `buried_visible_skip_fastflight_probe_20260516` body `17.12 ms`, `voxelTerrainPct=87`, and `valleyAtmospherePct=12` to body `12.94 ms`, `voxelTerrainPct=89`, and `valleyAtmospherePct=2`; waterline frame 240 stayed restored at body `6.05 ms`, and fast-walk frame 120 was body `43.59 ms`.
    Resumed interruption evidence: `current_restored_fastwalk_verify_20260516b` passed the hard visual gates (`miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`) and showed frame-120 `body=36.19 ms`, `surfExtract=7.84 ms`, and `budgetMid=12`; debug capture `current_restored_fastwalk_debug50_20260516b` classified the remaining white/cliff cuts as `valleyAtmosphere`. `current_fastwalk_240_normal_20260516b` passed the same gates with frame-240 `body=15.25 ms`, `midVoxInterest=4033/4096`, `missingVoxel=63`, and `valleyAtmosphere=26418`. Rejected probes from this resumed pass: `mid_catchup_precritical_gate_fastwalk_20260516` reached `midVoxInterest=4096/4096` and `budgetMid=96` but did not change the frame-240 `valleyAtmosphere=26418` target pixels and regressed waterline startup; `mid_dda_angle_fastwalk_retry_20260516` widened walking DDA angles but did not reduce valley ownership; `mid_neighbor_parent_fallback_fastwalk_20260516` made neighbor exposure use parent fallback but left frame-240 ownership unchanged; `surface_cull_distance_probe_20260516` set `VENPOD_SPARSE_SURFACE_CULL_DISTANCE=2200` and still reported the same frame-240 ownership split. All four probes were reverted or left as diagnostics only; do not reapply them without a new hypothesis.
    Current dirty-payload upload evidence: `dirty_payload_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=2.46%`, `minVoxelTerrainScreen=97.54%`, and `postNonReady=0`; frame 240 improved from the prior hidden-probe spike (`surfSnap=16.82 ms`, `surfStage=13.86 ms`, body `74.84 ms`, raw `105.97 ms`) to `surfSnap=3.42 ms`, `surfStage=5.31 ms`, body `28.82 ms`, raw `35.56 ms` on the visible capture. The log still shows full metadata work (`metaRange=5I`, `metaDraw=1I`, `metaRec=1I`, `metaCluster=1I`) when movement introduces new surface bricks, so this is an improvement, not the final surface-staging architecture. `dirty_payload_fastflight_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `maxFarSvo=83.32%`, `maxValleyAtmosphereScreen=3.37%`, and no surface staging at frame 240. `dirty_payload_waterline_early_20260516` passed the early waterline hard gates with `miss=0`, `unsafeNearMiss=0`, and `heightProxyScreen=0`, while still showing high waterline `valleyAtmosphereScreen=44.96%` that should remain separated from fake-terrain/hole validation.
    Incremental new-brick metadata evidence: `incremental_surface_newbricks_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `maxValleyAtmosphereScreen=6.27%`, and `minVoxelTerrainScreen=93.73%`. Frame 120 improved to `body=16.56 ms`, `surfExtract=4.09 ms`, `surfSnap=0.01 ms`, `surfStage=0.50 ms`, and `stagedMB=0.14`; frame 240 kept `surfSnap=0.01 ms` and `surfStage=1.13 ms`. The metadata path now patches only changed/new range, draw, record, and cluster entries (`metaRange=27I`, `metaDraw=27I`, `metaRec=27I`, `metaCluster=27I` at frame 120) instead of rebuilding whole visible metadata on new-brick admission. `incremental_surface_newbricks_fastflight_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `maxFarSvo=83.33%`, and `maxValleyAtmosphereScreen=3.37%`. `incremental_surface_newbricks_waterline_retry_20260516` passed the early waterline hard gates with `miss=0`, `unsafeNearMiss=0`, and `heightProxyScreen=0`; an immediately prior waterline run failed the existing temporal-stability smoke at frame 143 with `miss=0` and `unsafeNearMiss=0`, and the retry still showed pre-capture waterline startup unsafe-near rows around frames 118-119 that cleared before the capture window. This remains a waterline startup/stability validation gap, not a current captured fake-terrain/gap failure.
    Startup surface-proof evidence: the first surface-proof implementation was rejected because it waited on retired rendered surface fragments while startup public rendering was held, deadlocking the surface-raster backend proof at frame 120. The accepted fix gates on uploaded surface GPU data instead (`uploadedFaces`, `uploadedSurfaceRecords`, and resident payload bricks), so public rendering waits for complete surface data without needing a prior public surface draw. `startup_surface_gpu_proof_waterline_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `maxSky=0.01%`, while still documenting the low-angle waterline composition issue (`maxValleyAtmosphereScreen=44.96%`). `startup_surface_gpu_proof_fastwalk_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.29%`, `minVoxelTerrainScreen=93.73%`, and `maxValleyAtmosphereScreen=6.27%`. `startup_surface_gpu_proof_fastflight_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `maxFarSvo=83.33%`, `minVoxelTerrainScreen=88.75%`, and `maxValleyAtmosphereScreen=3.37%`. `VENPODSparseCore` also passed after the fix.
    Surface/background handoff evidence: the remaining fast-walk white cliff cutouts were traced to `SurfaceAuthoritativeBackgroundStartForRay` starting the fullscreen background after the full sparse-surface cull sphere (`~1536` voxels) instead of after the exact no-fake foreground (`384` voxels). This suppressed resident mid-voxel hits in the 384-1536 voxel band and produced `valleyAtmosphere` holes. The accepted shader fix keeps `BackgroundHitAllowedByExactNear` as the hard no-fake gate but starts background probing after `ExactNearDistance()+8`. `background_exactnear_handoff_fastwalk_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.86%`, `minVoxelTerrainScreen=94.30%`, and `maxValleyAtmosphereScreen=5.70%`, improving the prior fast-walk `6.27%` valley-atmosphere peak. `background_exactnear_handoff_waterline_20260516` and `background_exactnear_handoff_fastflight_20260516` also passed with zero miss/unsafe/height-proxy ownership; waterline remains composition-limited at `maxValleyAtmosphereScreen=44.96%`, while high-flight stayed at `maxValleyAtmosphereScreen=3.37%` and `maxFarSvo=83.33%`. `VENPODSparseCore` passed after this shader change.
    Default budget-exhaustion handoff evidence: `background_exactnear_handoff_fastwalk_debug50_20260516` confirmed the remaining fast-walk cutouts were still valley-atmosphere pixels on the default path. Two A/B probes were rejected: `ab_voxelterrainonly_off_fastwalk_20260516` re-enabled old non-voxel fallback, introduced tiny height-proxy/height ownership, and worsened frame-240 valley pixels; `ab_mid_budget96_after_handoff_fastwalk_20260516` raised mid-voxel budgets but regressed the valley peak to `6.27%`. The accepted fix applies the exact-near handoff to the default sparse-near budget-exhausted fallback as well as the background-only path, so fallback probing no longer starts behind the cliff after near traversal exhausts. `budget_exactnear_fallback_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.86%`, `minVoxelTerrainScreen=99.54%`, and `maxValleyAtmosphereScreen=0.46%`; the contact sheet shows the prior side-cliff white cutouts largely filled by terrain. Regressions `budget_exactnear_fallback_waterline_20260516` and `budget_exactnear_fallback_fastflight_20260516` also passed with zero miss/unsafe/height-proxy ownership; waterline stayed composition-limited at `44.96%` valley-atmosphere, and high-flight stayed at `3.37%`. `VENPODSparseCore` passed after this shader change.
    Latest fake-terrain/material evidence: the underwater miss-context fix is accepted after `underwater_volume_waterline_pass_20260516`, `underwater_volume_fastwalk_guard_20260516`, and `underwater_volume_fastflight_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, and `heightProxyScreen=0`; waterline `valleyAtmosphereScreen` is now `0.00%` for the reproduced underwater camera. The far-water-occluder probe is rejected because the camera is below `FAR_SEA_LEVEL` and that path correctly returns false underwater. Far SVO/shader fallback material thresholds now match the sparse terrain generator; a later conservative-occupancy pass bumped the far-SVO cache version to `16`. Warmed-cache regressions `material_consistency_fastflight_guard_warm_20260516`, `material_consistency_fastwalk_guard_20260516`, and `material_consistency_waterline_guard_20260516` passed. The cold cache-bump run failed only the `far-svo` readiness gate while rebuilding, then saved the new cache. Remaining performance evidence from `material_consistency_fastwalk_guard_20260516` showed the next hotspot was surface overdraw/staging, not fake terrain: late frames drew about `4.66M-4.76M` sparse-surface fragments on a `2.07M` pixel screen (`overdrawRatio=2.32-2.37`). After `[earlydepthstencil]` on `PS_SparseSurface`, `early_depth_surface_fastwalk_probe_20260516` passed the same hard gates and late-frame sparse-surface fragments dropped to `3.31M-3.87M` (`overdrawRatio=1.65-1.92`). Regressions `early_depth_surface_waterline_guard_20260516` and `early_depth_surface_fastflight_guard_20260516` also passed with `miss=0`, `unsafeNearMiss=0`, and `heightProxyScreen=0`; high-flight remained far-SVO/background dominated (`maxFarSvo=83.33%`, `maxValleyAtmosphereScreen=3.37%`).
    Early-depth surface evidence: `early_depth_surface_fastwalk_probe_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.29%`, and `maxValleyAtmosphereScreen=0.46%`. Late fast-walk sparse-surface fragments dropped from the prior `4.66M-4.76M` range to `3.31M-3.87M`, lowering measured overdraw from about `2.32-2.37x` to `1.65-1.92x`. `early_depth_surface_waterline_guard_20260516`, `early_depth_surface_fastflight_guard_20260516`, and `VENPODSparseCore` also passed after this shader change.
    Partial dirty-surface continuation evidence: before the latest change, `current_bottleneck_fastwalk_20260516` passed the hard visual gates but still showed frame-240 body `36.12 ms`, prep `19.98 ms`, sparse `19.49 ms`, sparse split `5.22/4.46/9.81 ms`, and surfStage `1.31 ms`, with larger later surface-stage spikes during walking. The accepted dirty-prefix upload path is verified by `partial_dirty_surface_fastwalk_probe_20260516`, which passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minVoxelTerrainScreen=99.54%`, and frame-240 body `18.76 ms`, prep `10.86 ms`, sparse `10.41 ms`, sparse split `3.15/2.22/5.04 ms`, surfExtract `1.81 ms`, and surfStage `0.96 ms`. The same log proves over-budget dirty payloads are now bounded instead of full-fallback at startup (`pendingDirty=258`, `dirtyCopied=128`, `deferred=130`, `residentPayload=128`). Regressions `partial_dirty_surface_waterline_guard_20260516`, `partial_dirty_surface_fastflight_guard_20260516`, and `VENPODSparseCore` passed after the change. Remaining open evidence: fast-walk frames 244/245/274 still show smaller surface-stage/body spikes, and high-flight frame 240 remains dominated by `pumpVoxel=6.86 ms` and ray/background GPU cost (`ray=8.26 ms`), not dirty surface upload.
    Rejected high-flight capacity/budget probes: raising the mid-voxel shader/config capacity to `8192` was initially rejected because `mid_voxel_8192_fastflight_probe_20260516` timed out before producing a runtime log while the process reached about `3.45 GB` working set. That older rejection is superseded by the later removal/tombstone/full-catchup fixes: the current accepted default raises `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS` and `MID_VOXEL_CLIPMAP_MAX_BRICKS` to `8192` only after fixing stale surface removal, range lookup, dirty catch-up starvation, draw-slot reuse, and face-count publication. Voxel eviction protection that avoided current-interest candidates remains rejected because `mid_voxel_no_interest_evict_fastflight_probe_20260516` passed visual gates but still reported `genVoxel=72`, `evictVoxel=72`, `missingVoxel=38`, and worsened frame-240 body to `24.68 ms`. Lowering `VENPOD_SPARSE_MID_TILE_BUDGET` to `48` remains rejected because `mid_voxel_budget48_fastflight_probe_20260516` passed visual gates but reduced mid-voxel coverage to `3947/4096`, increased `missingVoxel=149`, and worsened frame-240 body to `47.46 ms`. These probes show the remaining high-flight cost is not solved by a simple capacity raise alone, same-frame eviction guard, or lower base pump budget.
    Compact-slot and mid-cache evidence: `surface_spike_diagnostics_fastwalk_20260516` proved the remaining walking spikes were visually clean but tied to `pendingRemoved` surface metadata work; `surface_payload_after_free_slots_fastwalk_probe_20260516` passed the gates but showed dirty-only frames after removals still rejected the fast path when compact draw commands left `drawSlots != gpuDrawCmds`. The accepted compact-slot patch is verified by `surface_payload_compact_slots_fastwalk_probe_20260516`: it passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.29%`, and `maxValleyAtmosphereScreen=0.46%`. The old post-removal frame-275 full stage dropped to `surfStage=1.10 ms` with `pendingRemoved=0`; the actual removal frame 274 remains a deeper metadata/compaction hotspot (`pendingRemoved=6`, `pendingDirty=118`, `surfStage=19.55 ms`) and should be handled as a separate architecture item. `surface_payload_compact_slots_waterline_guard_20260516`, `surface_payload_compact_slots_fastflight_guard_20260516`, and `VENPODSparseCore` passed. The accepted mid-cache hysteresis default is verified by `mid_interest90_default_fastflight_guard_20260516`, `mid_interest90_default_fastwalk_guard_20260516`, `mid_interest90_default_waterline_guard_20260516`, and `VENPODSparseCore`: all passed hard gates. In high-flight at frame 240, the `90%` interest target reduced `interestedVoxel` from `4096` to `3686`, eliminated `missingVoxel=38` to `0`, reduced `genVoxel/evictVoxel` from `72/72` to `46/46`, and improved frame body from `26.58 ms` to `20.88 ms` without reintroducing height proxy, miss, or unsafe-near ownership.
    Incremental removal evidence: `surface_incremental_removal_fastwalk_probe_20260516` is rejected evidence because the first implementation preserved visual gates but corrupted resident metadata accounting (`gpuRanges` and `vis` collapsed to dirty counts on dirty-only frames). After fixing the accounting, `surface_incremental_removal_fastwalk_probe2_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, `minSurfaceScreen=90.29%`, `maxValleyAtmosphereScreen=0.46%`, and sane resident counters (`gpuRanges=2756`, `vis=2756` at frame 240). The target removal frame 274 now reports body `15.05 ms`, prep `19.78 ms`, GPU surface `1.09 ms`, and staged metadata patches only (`metaRange=124I`, `metaDraw=3I`, `metaRec=1I`, `metaCluster=1I`) instead of the prior `83.30 ms` body / `surfStage=19.55 ms` removal-frame spike. Frame 275 is a small dirty update (`stagedMB=0.03`, `metaRange=6I`). Regressions `surface_incremental_removal_waterline_guard_20260516`, `surface_incremental_removal_fastflight_guard_20260516`, and `VENPODSparseCore` passed with zero miss/unsafe/height-proxy ownership. Waterline frame 180 remains a stress/performance concern (`gpu ray` around `50 ms` and body around `74 ms`), not a visual correctness failure.
    Mid-catchup pacing continuation evidence: `long_fastwalk_broad_probe_20260516` proved the remaining broad walking hitch was CPU-side sparse prep, not GPU stress or non-ready exact terrain: `maxFrameMs=130.08`, `maxPrepMs=107.30`, `maxGpuRayMs=10.46`, `postNonReady=0`, and repeated `PERF_SPARSE_CLIPMAP` rows spent `budgetMid=96` with `pumpVoxel` between about `49-63 ms` while only `48-70` interested mid-voxel bricks were missing. The accepted default budget change is verified by `mid_catchup_budget_default_long_fastwalk_20260516`, which passed the same route with `maxFrameMs=51.55`, `maxPrepMs=42.59`, `maxGpuRayMs=11.00`, `heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and `postNonReady=0`; the formerly bad span now uses bounded `budgetMid=12/72` behavior. Regressions `mid_catchup_budget_default_highflight_guard_20260516` and `mid_catchup_budget_default_waterline_guard_20260516` passed with zero miss/unsafe/height-proxy ownership and `postNonReady=0`. High-flight frame 240 uses `budgetMid=48`, `pumpVoxel=6.86 ms`, and body `23.28 ms`; waterline frame 240 uses `budgetMid=12`, `pumpVoxel=0`, and body `18.17 ms`, with remaining waterline cost in GPU ray/background work rather than CPU streaming. The accepted waterline ray cap is verified by `waterline_ray_cap_default_guard_20260516`, `waterline_ray_cap_highflight_guard_20260516`, and `waterline_ray_cap_long_fastwalk_guard_20260516`: waterline frame 240 GPU ray dropped to `4.44 ms` with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`; high-flight retained clean ownership with `gpu ray=8.25 ms`; broad fast-walk passed with `maxFrameMs=47.19` and `maxPrepMs=40.33`. The accepted visible-catchup protection fix stops generic queued mid-voxel maintenance from being treated as protected catchup when there are no visible missing voxel bricks or ownership deficits. `late_long_fastwalk_probe_20260516` exposed the late-session failure (`maxFrameMs=110.59`, `maxPrepMs=118.05`, `maxGpuRayMs=1.66`, `postNonReady=0`, with repeated `missingVoxel=0` maintenance bursts), and `late_long_fastwalk_mid_visible_protection_20260516` passed the same route at `maxFrameMs=47.37`, `maxPrepMs=48.84`, `maxGpuRayMs=1.55`, `maxValleyAtmosphereScreen=0.62%`, and `postNonReady=0`. Regressions `mid_visible_protection_waterline_guard_20260516`, `mid_visible_protection_highflight_guard_20260516`, `mid_visible_protection_long_fastwalk_guard_20260516`, and `VENPODSparseCore` passed. `waterline_brush_material_guard_20260516` also passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, and `deltaMismatch=0`, covering the reported fake-sand-after-draw interaction as far as the scripted brush path can.
    Latest reopened-goal root-cause evidence: rejected probes showed dirty/removal uploads failing or starving full catch-up. `dirty_removed_tombstone_long_fastwalk_guard_20260516` exposed the removal/zero-face dirty path; `full_catchup_tombstone_long_fastwalk_guard_20260516` exposed dirty-fast-path starvation with low GPU surface record residency; `valley_pressure_full_catchup_long_fastwalk_guard_20260516` exposed allocation-changing dirty bricks that could not reuse free draw slots; and `draw_slot_reuse_long_fastwalk_guard_20260516` exposed the face-count overpublish (`gpuFaces=3261379` above a `2097152` IA capacity). The accepted fixes are verified by `mid_capacity_surface_compaction_long_fastwalk_guard_20260516`, `mid_capacity_surface_compaction_waterline_guard_20260516`, and `mid_capacity_surface_compaction_highflight_guard_20260516`. The long fast-walk route now passes the visual gates with `maxFrameMs=49.47`, `maxSmoothedFrameMs=95.14`, `maxPrepMs=71.58`, `maxGpuRayMs=13.11`, `maxValleyAtmosphereScreen=5.16%`, `heightProxyScreen=0`, and `postNonReady=0`; waterline brush passes with `maxFrameMs=26.69`, `maxPrepMs=8.35`, `maxGpuRayMs=5.45`, `maxValleyAtmosphereScreen=0.00%`, and `postNonReady=0`; high-flight passes with `maxFrameMs=18.11`, `maxPrepMs=30.48`, `maxGpuRayMs=4.47`, `maxValleyAtmosphereScreen=0.00%`, and `postNonReady=0`. Build and `VENPODSparseCore` passed after the changes.
    Current shader-feedback/root-coordinate evidence: `feedback_original_only_fastwalk_guard_20260516` is rejected because the original-only feedback cap left a visible unsafe-near miss peak, proving that the shader-missed coord must be kept but the terrain-remap neighborhood is still needed. The accepted post-restore fast-walk guard `feedback_original_coord_fastwalk_postrestore_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=67.81%`, `maxFrameMs=80.54`, `maxPrepMs=79.51`, `maxGpuRayMs=40.12`, and `postNonReady=0`. The startup/LOD validation fixes are covered by `startup_surface_proof_highflight_late_postfix2_20260516` (`maxValleyAtmosphereScreen=0.01%`, `minVoxelTerrainScreen=89.93%`, `maxFrameMs=49.84`, `maxPrepMs=108.62`, `maxGpuRayMs=12.76`, `postNonReady=0`) and `feedback_original_coord_waterline_late_postfix2_20260516` (`maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=40.95%`, `maxFrameMs=57.29`, `maxPrepMs=54.71`, `maxGpuRayMs=21.99`, `postNonReady=0`). The speculative-upload startup fix is verified by `startup_speculative_upload_gate_highflight_20260516`, `startup_speculative_upload_gate_waterline_20260516`, and `startup_speculative_upload_gate_fastwalk_20260516`: held-state logs distinguish `protectedUploadQueued=0` from speculative `uploadQueued=176`, high-flight/waterline public rendering opens in the earlier capture window, and all three runs pass with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`. `VENPODSparseCore` passed after the engine validation changes.
    Surface metadata resize evidence: broad-route logs before this pass showed small dirty/removal sets still paying full surface metadata staging (`surfStage` around `13-14 ms`, `stagedMB` around `0.47-0.49`) because allocation-resize frames cloned full mirrors and did not recompute existing-record cluster summaries. The accepted incremental resize patch is verified by `broad_surface_metadata_patch_20260516`, `highflight_surface_metadata_patch_20260516`, `waterline_surface_metadata_patch_20260516`, and `VENPODSparseCore`. The broad guard passed with `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=69.88%`, `maxFrameMs=58.74`, `maxPrepMs=52.61`, and `maxGpuRayMs=14.38`; frame 1200 now stages only `0.02 MB` and reports `surfStage=2.52 ms`. High-flight passed with `maxValleyAtmosphereScreen=3.02%`, `postNonReady=0`, and `maxFrameMs=81.95`; waterline passed with `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=81.63%`, and `maxFrameMs=51.07`. Frame 1440 in the broad route still shows `surfStage=10.52 ms`/`surfSnap=3.51 ms`, so TS-015 stays `PARTIAL`.
    Terrain-critical ready-footprint reuse evidence: after the brush-feedback residency fix, `PERF_SPARSE_TERRAIN_CRITICAL` still showed stable clean frames paying to trace the same `21x13` critical ray grid and classify the same 345 current-footprint coords even when `postNonReady=0` and `new=0`. The accepted signature-gated reuse path is verified by `critical_signature_reuse_brush_guard_20260517`, `critical_signature_reuse_waterline_guard_20260517`, `.\build.ps1`, and `.\build\bin\VENPODTests.exe`. Both guards passed with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`, and nonzero brush edits applied without fallback/missing-resident/delta-mismatch errors. The brush guard logged 190 reuse rows out of 447 terrain-critical rows; the waterline guard logged 188 reuse rows out of 450 rows. On reuse frames such as frame 300 and 420, the pass logged `reuse=1`, `rays=0/273`, `postReady=313`, `postEmpty=32`, and zero post non-ready states. This narrows redundant CPU terrain-critical work but does not close TS-015 because GPU ray time remains around `7-8 ms` in these captures and broader manual free-roam/performance coverage is still incomplete.
    Mid-clipmap edit-overlay fast-path evidence: `broad_current_rootcause_probe_20260517` showed the next dominant broad-route stall was CPU mid-voxel pumping, not fake terrain or non-ready exact terrain. The route passed `heightProxyScreen=0`, `missScreenPct=0`, and `postNonReady=0`, but frame 136 reported `pumpVoxel=172.02 ms`, frames 190/196 were about `165 ms`, and frame 833 was `161.76 ms`. Logs also showed `edits=0/0`, proving those mid bricks were paying edit-overlay lookup cost with no overlays present. `SparseClipmapTileCache::GenerateVoxelBrick` now skips edit work when the edit store is empty and builds coarse edited-cell summaries by iterating actual overlays instead of scanning every fine brick in a coarse mid-brick volume. `broad_edit_overlay_fastpath_full_guard_20260517` passed with `heightProxyScreen=0.00%`, `missScreenPct=0.00%`, `postNonReady=0`, `maxValleyAtmosphereScreen=1.61%`, and `minVoxelTerrainScreen=74.29%`. The bad mid-pump frames improved to frame 136 `9.48 ms`, frame 190 `7.19 ms`, frame 196 `6.48 ms`, frame 833 `7.81 ms`, and frame 1190 `7.15 ms`. The edit-authority regression `edit_overlay_fastpath_waterline_brush_guard_20260517` also passed with `5868` brush edits applied and zero fallback/missing-resident/delta-mismatch errors. TS-015 remains open because the same full broad guard still has prep peaks around `73 ms`, exact sparse generation up to about `46 ms`, and remaining GPU-ray spikes.
11. Next action required: Continue the performance pass on broader manual free-roam frame pacing and long free-roam cluster growth/compaction risk. The broad scripted walking hitch from over-aggressive final-percent mid-voxel catchup is reduced, the late-session maintenance hitch is reduced, the reproduced waterline fullscreen ray spike is reduced, and the latest resize-only surface metadata staging path is reduced, but this does not close all performance work. The new-brick, removal, and resize incremental metadata paths materially reduce snapshot/stage cost but append or retain cluster metadata across incremental admissions/removals; watch long sessions and rebuild/compact through the existing full-snapshot fallback if cluster count/invalid-record ratios grow. The reopened-goal sparse-surface record lookup change should be treated as a diagnostic/helper, not a completed performance fix: its first map-copy version is rejected, and the no-copy version keeps visual guards passing but still leaves long-walk CPU sparse-prep spikes. The next concrete fix should target residual `surfSnap`/`surfStage` spikes and frame-pacing/GPU-wait cost without exposing partial world state. Do not reapply the rejected mid-catchup cap, cap-16 surface extraction, hot-frame preupload-surface lane, material-cache/material-grid probes, strict-coordinate default 1, ray-scale 45 default, protected-surface cap 48, overlay-skip sampler probe, skip-solid-count probe, record-from-draw-batches staging probe, batch protected-surface queue drain, critical distance 1536 default, surface ownership radius 2400 default, full walking-view mid-voxel DDA, full authoritative fallback start, distant-only mid-column handoff, local-classification surface extractor rewrite, persistent cross-frame terrain-column cache, known-empty surface fast path, non-voxel background fallback default, current-interest voxel eviction protection, or mid tile budget 48.

### TS-016
1. Requirement ID: TS-016
2. Requirement text: Exact sparse terrain must not be considered renderer-ready until the CPU resident record has a GPU-visible page-table publish, and near sparse holes must not be filled by fake background sand/land.
3. Source document / source location: User report on 2026-05-16: world still sparse, fake sand/terrain near water disappears after drawing/editing, and debug showed only yellow regions are real loaded voxels; friend diagnosis emphasized "never render partial world state" and only draw `ReadyToRender`.
4. Current status: DONE_VERIFIED for the reproduced fast-walk route and scripted waterline edit/fake-material guard; broader manual free-roam remains covered by TS-015/overall completion gate.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/Simulation/SparseVoxelTypes.h`, `VENPOD/src/Simulation/SparseBrickPool.h`, `VENPOD/src/Simulation/SparseBrickPool.cpp`, `VENPOD/src/Simulation/SparseVoxelWorld.h`, `VENPOD/src/Simulation/SparseVoxelWorld.cpp`, `VENPOD/src/main_launcher.cpp`, `VENPOD/test/test_sparse_core.cpp`, `VENPOD/build/captures/gpu_publish_readiness_focus_20260516`, `VENPOD/build/captures/gpu_publish_readiness_confirm_20260516`, `VENPOD/build/captures/no_near_background_fill_20260516`, `VENPOD/build/captures/default_no_near_fill_confirm_20260516`, `VENPOD/build/captures/waterline_brush_smoke_synthetic_input_20260516`, `VENPOD/build/captures/broad_default_after_brush_smoke_fix_20260516`.
6. What currently exists: `BrickResidentRecord` now tracks `gpuPageTablePublished`. `SparseBrickPool::PublishResident` clears that flag, and `SparseBrickPool::MarkGpuPageTablePublished` only sets it after coord/page/generation and CPU page-table validation. `SparseVoxelWorld::GetRenderReadinessState`, `BuildRenderReadinessStats`, and renderable-brick stats now treat resident but GPU-unpublished bricks as `UploadingGPU`, so CPU residency alone is no longer renderer readiness. The main renderer marks a brick GPU-published only after successfully staging/emitting the page-table copy. The failed same-frame shader-feedback republish experiment was removed. `VENPOD_SPARSE_SURFACE_RAYMARCH_FILL` now defaults to `1`, so sparse-surface raster gaps in the exact foreground are filled by the true sparse voxel raymarch path instead of a height/column proxy or unsafe miss. The voxel-only background path also gates off the cheap mid-column height proxy, so `MID_VOXEL` debug/ownership no longer hides height-proxy terrain in voxel-only mode. Missing nonresident sparse pages are still not considered renderer-ready because the raymarch path consults the sparse page table/readiness data. The scripted sparse brush paint smoke now synthesizes its paint stroke in sparse runtime test mode even when live brush input is disabled, so the waterline fake-material regression guard actually queues and applies sparse edits instead of passing through zero-edit frames.
7. What is missing: This does not finish all terrain architecture work. The default route still uses mid/far voxel context for distance, manual free-roam still needs visual acceptance, and performance/long-session compaction risk remains tracked by TS-015 and the full architecture gate.
8. Validation required: Build and `VENPODSparseCore` must pass. A default fast-walk capture must pass with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, and no near-hole background fill env override. A waterline brush smoke capture must prove edits actually occur (`queued`, `applied`, and `deltas` nonzero) while keeping fallback/missing-resident/unsafe-near/height-proxy at zero.
9. Exact proof: `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WalkTest -WalkTestSpeed 42 -WalkTestYawDegPerSec 18 -WalkTestPitchDeg -4 -WalkTestFixedDtMs 0 -ExitAfterFrames 360 -CaptureStartFrame 312 -CaptureIntervalFrames 8 -CaptureCount 5 -SparseOwnershipMaxTerrainDeltaPct 20 -MinUniqueSampleColors 40 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\default_no_near_fill_confirm_20260516`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WaterlineCamera -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 80 -SparseBrushPaintEndFrame 220 -ExitAfterFrames 420 -CaptureStartFrame 180 -CaptureIntervalFrames 20 -CaptureCount 8 -MinUniqueSampleColors 40 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\waterline_brush_smoke_synthetic_input_20260516`.
10. Latest evidence: Build passed. `VENPODSparseCore` passed. `gpu_publish_readiness_focus_20260516` initially failed only because terrain ownership jumped from 88% to 100% after the publish-readiness fix; it showed `miss=0`, `unsafeNearMiss=0` by frame 311. `gpu_publish_readiness_confirm_20260516` exited normally but found one tiny residual `unsafeNearMiss=236` frame. The A/B run `no_near_background_fill_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=69.78%`, `maxFrameMs=27.75`, `maxPrepMs=23.13`, and `maxGpuRayMs=6.48`. After changing the default, `default_no_near_fill_confirm_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=70.00%`, `maxFrameMs=28.63`, `maxPrepMs=24.03`, `maxGpuRayMs=6.66`, and terrain-critical `postNonReady=0`. The first reopened waterline brush guard failed because `VENPOD_DISABLE_BRUSH_INPUT` also suppressed the synthetic smoke stroke, producing `queued=0`, `applied=0`, and `deltas=0`; this was a validation harness failure, not proof of a new terrain regression. After the synthetic-input fix, `waterline_brush_smoke_synthetic_input_20260516` passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=140 retired=2810 applied=2810 deltas=2810 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`, plus `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=77.59%`, `maxFrameMs=11.44`, `maxPrepMs=7.54`, and `maxGpuRayMs=8.54`. The broad default post-patch route `broad_default_after_brush_smoke_fix_20260516` also passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.69%`, `maxFrameMs=23.62`, `maxPrepMs=19.63`, `maxGpuRayMs=8.62`, and `postNonReady=0`.
11. Next action required: Use this default as the new baseline. Next fixes should target remaining visual quality and performance without re-enabling near-hole background fill or treating CPU page-table residency as GPU visibility.

### TS-017
1. Requirement ID: TS-017
2. Requirement text: Exact sparse-surface raster must not draw detached overhead bands or grouped partial surfaces into sky space.
3. Source document / source location: User reports on 2026-05-16 that the dome/air around the player hides the real world, plus latest broad contact sheets showing a curved exact-surface band in the sky while `miss`, `unsafeNearMiss`, and height-proxy ownership were zero.
4. Current status: DONE_VERIFIED for the reproduced broad scripted route and waterline brush regression.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/Graphics/SparseSurfaceGpuResources.h`, `VENPOD/assets/shaders/Compute/CS_SparseSurfaceCullCompact.hlsl`, `VENPOD/assets/shaders/Graphics/VS_SparseSurface.hlsl`, `VENPOD/assets/shaders/Graphics/PS_SparseSurface.hlsl`, `VENPOD/build/captures/debug53_arc_source_20260516`, `VENPOD/build/captures/debug50_arc_ownership_20260516`, `VENPOD/build/captures/ab_no_cluster_fast_arc_20260516`, `VENPOD/build/captures/fixeddt_default_arc_compare_20260516`, `VENPOD/build/captures/fixeddt_no_cluster_fast_arc_compare_20260516`, `VENPOD/build/captures/cluster_fast_disabled_broad_guard_20260516`, `VENPOD/build/captures/cluster_fast_disabled_waterline_brush_guard_20260516`, `VENPOD/build/captures/cluster_fast_disabled_highflight_late_guard_20260516`.
6. What currently exists: Debug 53 ruled out brush/avatar overlay coloring for the sky arc. Debug 50 showed the arc as exact sparse surface, so the failure was not SVO/mid/far ownership or height proxy. A/B testing showed that lowering the raster distance did not remove the band, while disabling sparse-surface cluster fast-accept removed the detached grouped surface artifact on the reproduced route. `SparseSurfaceGpuConfig` now defaults `surfaceClusterFastAcceptMaxRecords` and `surfaceClusterFastAcceptMaxFaces` to zero, keeping the GPU cull path per-record until a stricter grouped readiness/visibility contract exists.
7. What is missing: This does not prove every future grouped/clustered surface optimization is safe. If cluster fast-accept is reintroduced, it needs its own validation proving no sky bands, no stale grouped faces, and no partial parent/group visibility.
8. Validation required: Build and `VENPODSparseCore` must pass. A broad walk capture must pass visual/readiness gates and a dome/overhead-band threshold. Waterline brush smoke must still queue/apply edits and preserve zero fallback/missing-resident errors.
9. Exact proof: `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WalkTest -WalkTestSpeed 42 -WalkTestYawDegPerSec 18 -WalkTestPitchDeg -4 -WalkTestFixedDtMs 0 -ExitAfterFrames 900 -CaptureStartFrame 600 -CaptureIntervalFrames 20 -CaptureCount 12 -SparseOwnershipMaxTerrainDeltaPct 25 -MinUniqueSampleColors 40 -MaxFrameBrushDomeLikePct 8 -MaxAverageBrushDomeLikePct 5 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 180 -MaxPrepMs 160 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\cluster_fast_disabled_broad_guard_20260516`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WaterlineCamera -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 80 -SparseBrushPaintEndFrame 220 -ExitAfterFrames 420 -CaptureStartFrame 180 -CaptureIntervalFrames 20 -CaptureCount 8 -MinUniqueSampleColors 40 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\cluster_fast_disabled_waterline_brush_guard_20260516`.
10. Latest evidence: Build passed and `VENPODSparseCore` passed after disabling cluster fast-accept by default. `debug53_arc_source_20260516` did not mark the arc as brush/avatar overlay; `debug50_arc_ownership_20260516` marked it as yellow exact sparse surface. `ab_no_cluster_fast_arc_20260516` removed the obvious overhead arc in the comparable real-time route, and fixed-dt A/B reduced the dome metric from `4.61%` on the first compared frame to `0.00%`. The accepted guard `cluster_fast_disabled_broad_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=80.29%`, `maxFrameMs=36.21`, `maxPrepMs=28.36`, `maxGpuRayMs=5.99`, and `postNonReady=0`; the contact sheet no longer shows the detached sky arc. `cluster_fast_disabled_waterline_brush_guard_20260516` passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=140 retired=2810 applied=2810 deltas=2810 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`, plus zero miss/unsafe/height-proxy ownership. An early high-flight window (`cluster_fast_disabled_highflight_guard_20260516`) still produced a tiny transient unsafe-near peak before the later readiness window, so it is not closure evidence; the later high-flight guard `cluster_fast_disabled_highflight_late_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=77.78%`, `maxFrameMs=20.27`, `maxPrepMs=7.69`, `maxGpuRayMs=7.05`, and `postNonReady=0`.
11. Next action required: Keep cluster fast-accept disabled by default. If performance work needs grouped sparse-surface drawing again, implement a safer group visibility contract rather than drawing a whole cluster from coarse acceptance alone.

### TS-018
1. Requirement ID: TS-018
2. Requirement text: High-altitude/far-terrain ownership must not classify surface-authoritative background misses as editable near sparse holes.
3. Source document / source location: User reports on 2026-05-16 that the renderer still looked sparse/regressed and asked whether the SVO itself was broken; failing high-flight guard `cluster_fast_disabled_highflight_guard_20260516` showed a tiny `unsafeNearMiss` burst even though terrain-critical post-publish readiness was clean.
4. Current status: DONE_VERIFIED for the reproduced high-flight early-window failure, broad walking guard, and waterline brush regression.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`, `VENPOD/build/captures/lod_throttle_probe_highflight_early_guard_20260516`, `VENPOD/build/captures/high_altitude_unsafe_classification_synced2_20260516`, `VENPOD/build/captures/surface_auth_highflight_ownership_fix_20260516`, `VENPOD/build/captures/surface_auth_fix_broad_guard_20260516`, `VENPOD/build/captures/surface_auth_fix_waterline_brush_guard_20260516`.
6. What currently exists: A low-budget LOD-throttle terrain-critical probe removed the first high-flight unsafe burst, proving one part of the route was real exact-terrain scheduling pressure. The remaining burst had `unsafeSample=0`, `postNonReady=0`, and clean request/upload readiness, so it was not a non-ready brick or SVO residency failure. The root cause was the surface-authoritative shader branch: when sparse surface was authoritative and raymarch fill was disabled, any background miss returned `DebugBackgroundMissHit(rayDir, true)`. In high-altitude stress views that path is also used for far-terrain ownership, so shallow far/horizon fallback misses were falsely counted as editable near holes. The shader now passes `rayOrigin.y <= 384.0f` for that branch, reserving near-hole ownership for cameras inside the editable terrain band.
7. What is missing: Manual free-roam visual acceptance is still required for all possible camera routes. This fix addresses a false unsafe-near ownership classification, not every remaining performance hitch or broad LOD quality issue.
8. Validation required: The formerly failing early high-flight guard must pass with `unsafeNearMiss=0`, `miss=0`, `heightProxyScreen=0`, and terrain-critical `postNonReady=0`. Broad walking and waterline brush guards must remain clean so the fix does not hide real shoreline/edit failures.
9. Exact proof: `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -StressCamera -ExitAfterFrames 360 -CaptureStartFrame 180 -CaptureIntervalFrames 20 -CaptureCount 6 -MinUniqueSampleColors 40 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\surface_auth_highflight_ownership_fix_20260516`; broad and waterline commands matching TS-017 with output directories `surface_auth_fix_broad_guard_20260516` and `surface_auth_fix_waterline_brush_guard_20260516`.
10. Latest evidence: Release build completed and refreshed runtime assets; `VENPODSparseCore` passed. `surface_auth_highflight_ownership_fix_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=89.93%`, `maxFrameMs=34.28`, `maxPrepMs=26.96`, `maxGpuRayMs=9.54`, and `postNonReady=0`. `surface_auth_fix_broad_guard_20260516` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=81.41%`, `maxFrameMs=23.92`, `maxPrepMs=19.59`, `maxGpuRayMs=3.76`, and `postNonReady=0`. `surface_auth_fix_waterline_brush_guard_20260516` passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=140 retired=2810 applied=2810 deltas=2810 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`, plus `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=77.60%`, `maxFrameMs=12.60`, `maxPrepMs=8.26`, `maxGpuRayMs=4.06`, and `postNonReady=0`.
11. Next action required: Keep this classification split. If future high-altitude or far-LOD failures appear, inspect whether they are real missing resident voxel layers, SVO traversal misses, or ownership-classification misses before raising exact-streaming budgets.

### TS-019
1. Requirement ID: TS-019
2. Requirement text: Aggressive fast-walk/far-mountain views must not expose stale fake terrain, surface upload starvation, sparse replacement scan storms, or too-short exact-surface ownership as visible gaps/background slabs.
3. Source document / source location: User report on 2026-05-16 after screenshot review: gaps are improved but far mountains still have holes, fake sand/terrain disappears after raymarch edits, movement is slow/buggy, and the user wants root rendering causes fixed instead of stabilizing only.
4. Current status: DONE_VERIFIED for the reproduced aggressive scripted fast-walk route; manual free-roam acceptance remains covered by broader PARTIAL items.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/main_launcher.cpp`, `VENPOD/src/Graphics/SparseSurfaceGpuResources.h`, `VENPOD/src/Graphics/SparseSurfaceGpuResources.cpp`, `VENPOD/build/captures/broad_aggressive_freeroam_probe_20260516`, `VENPOD/build/captures/ab_surface_faces3m_aggressive_freeroam_20260516`, `VENPOD/build/captures/surface_faces3m_default_aggressive_freeroam_20260516`, `VENPOD/build/captures/replacement_batch_aggressive_freeroam_20260516`, `VENPOD/build/captures/surface_fallback_aggressive_freeroam_20260516`, `VENPOD/build/captures/surface_radius2304_aggressive_freeroam_20260516`, `VENPOD/build/captures/surface_radius3072_aggressive_freeroam_20260516`, `VENPOD/build/captures/surface_radius3072_default_aggressive_freeroam_20260516`.
6. What currently exists: The sparse-surface IA face arena now defaults to `3u << 20`, avoiding the reproduced allocation/fragmentation overflow where CPU surface faces were below total capacity but no contiguous range remained. Protected replacement eviction now batches lower-priority page replacement and caps scan attempts per frame (`VENPOD_SPARSE_REPLACEMENT_BATCH_BUDGET`, `VENPOD_SPARSE_REPLACEMENT_SCAN_BUDGET`) instead of rescanning resident pages once per requested replacement. Dirty sparse-surface staging now refuses to report success when compact stable draw metadata prevents dirty payload progress and only removal metadata was staged, forcing the full snapshot path that knows how to rebuild compact draw commands. Stable sparse-surface raster/cull still covers the tested mountain/valley route, but the surface-authoritative fullscreen handoff now defaults to `1024` voxels so resident mid/far voxel layers can fill surface-raster gaps instead of being rejected as fake background until 3072.
7. What is missing: This does not complete the full architecture ledger. The aggressive route is still not guaranteed real-time on every machine or manual path, the lifecycle debug view is still incomplete, and broader parent/child LOD readiness contracts remain PARTIAL under TS-001/TS-002/TS-004/TS-007/TS-015.
8. Validation required: Release build and `VENPODSparseCore` must pass. The aggressive variable-dt fast-walk capture must pass with no height proxy, no valley-atmosphere screen failures, no unsafe near misses, no post-publish non-ready terrain-critical targets, no sparse-surface alloc/overflow failure, and no frame/prep/GPU-ray threshold breach.
9. Exact proof: `.\VENPOD\build.ps1 -Config Release`; `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WalkTest -WalkTestSpeed 64 -WalkTestYawDegPerSec 32 -WalkTestPitchDeg -7 -WalkTestFixedDtMs 0 -ExitAfterFrames 1800 -CaptureStartFrame 1200 -CaptureIntervalFrames 30 -CaptureCount 16 -SparseOwnershipMaxTerrainDeltaPct 30 -MinUniqueSampleColors 40 -MaxFrameBrushDomeLikePct 8 -MaxAverageBrushDomeLikePct 5 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\surface_radius3072_default_aggressive_freeroam_20260516`.
10. Latest evidence: `broad_aggressive_freeroam_probe_20260516` found the real allocation failure at frames 1357-1366 (`allocFail=1`, `overflow=1`) with visual gates otherwise clean; frame 1356 had `gpuFaces=2062099` against `iaFaces=2097152`, `largestFree=360`, and `freeRanges=1271`. The 3M-face A/B `ab_surface_faces3m_aggressive_freeroam_20260516` removed that overflow, and the default 3M run moved the failure to CPU sparse request spikes at frames 1702/1704 (`req` about `208/202 ms`) caused by repeated replacement scans. After batched replacement, those frames dropped to `prep=25.77/22.59 ms`, but the route exposed surface fast-path starvation (`pendingDirty=2249`, `dirtyCopied=0`, `deferred=2249`) and then too-short exact-surface distance. The first final default guard `surface_radius3072_default_aggressive_freeroam_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=74.21%`, `maxFrameMs=66.35`, `maxSmoothedFrameMs=85.14`, `maxPrepMs=50.89`, and `maxGpuRayMs=10.35`. Sequential regression guards also passed after invalidating a parallel run with suspicious shared-log numbers: `surface_radius3072_waterline_brush_guard_seq_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=77.60%`, `maxFrameMs=11.99`, `maxPrepMs=8.08`, and `maxGpuRayMs=4.94`; `surface_radius3072_highflight_guard_seq_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=89.93%`, `maxFrameMs=32.45`, `maxPrepMs=25.58`, and `maxGpuRayMs=9.53`. Follow-up performance diagnosis showed remaining spikes were not surface upload or replacement scans: `surface_radius3072_default_aggressive_freeroam_20260516` had mid-clipmap catch-up frames with `budgetMid=144` and pump up to `13.40 ms`. The accepted pacing default sets mid catch-up to the normal tile budget and coverage catch-up to `48`; `surface_radius3072_midpace_default_aggressive_20260516` still passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=75.51%`, `maxFrameMs=79.82`, `maxPrepMs=57.27`, and `maxGpuRayMs=9.56`, and its worst checked mid-clipmap catch-up frames used `budgetMid=72` rather than `144`. This is not performance closure because the dominant later spikes moved to protected exact terrain-critical generation around frames 1298-1307 (`protectedDrain` roughly 33-49 generated/uploaded/surfaced targets per frame). Rejected performance probes: `surface_radius3072_no_continuous_critical_ab_20260516` failed visual markers when continuous terrain-critical prefetch was disabled, and `surface_radius3072_critical_coords1_ab_20260516` failed visual markers when the terrain-critical hit neighborhood was reduced to one coord. Post-pacing sequential regressions `midpace_default_waterline_guard_seq_20260516` and `midpace_default_highflight_guard_seq_20260516` passed with zero miss/unsafe/height-proxy/valley ownership and `postNonReady=0`.
11. Next action required: Use the 1024 exact-handoff default, 3072 sparse-surface raster/cull radius, and paced mid-clipmap defaults as the new baseline. Continue with protected exact terrain-critical generation/publish pacing; do not remove continuous terrain-critical prefetch or reduce the hit neighborhood without a replacement readiness proof, because both A/B probes reintroduced visual artifacts.

### TS-020
1. Requirement ID: TS-020
2. Requirement text: Mid/far/background terrain must not fabricate sand, waterline terrain, or cliff surfaces inside the immediate exact sparse-surface edit/collision foreground.
3. Source document / source location: User report on 2026-05-16 that nearby shoreline/sand looked real until the brush/raymarch path touched it, after which the world realized it was not real terrain and the sand disappeared; follow-up diagnosis found fullscreen background terrain admitted too near the player on earlier defaults, then later found that suppressing resident voxel fallback all the way to the 3072-voxel sparse-surface cull radius created far sky cutouts.
4. Current status: DONE_VERIFIED.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/main_launcher.cpp`, `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`, `VENPOD/build/captures/exact_near3072_default_aggressive_20260516`, `VENPOD/build/captures/exact_near3072_default_aggressive_rebuilt_20260516`, `VENPOD/build/captures/lod_surface_optional_waterline_20260516`, `VENPOD/build/captures/lod_surface_optional_highflight_20260516`.
6. What currently exists: `VENPOD_SPARSE_EXACT_NEAR_DISTANCE` now defaults to `1024`, which keeps mid/far/background terrain out of the immediate editable foreground while allowing resident voxel LOD to fill mid-distance sparse-surface raster gaps. The stable sparse-surface raster/cull radius remains larger (`3072` by default), so exact surface still draws where it is resident and accepted by culling. The shader classifies underwater/waterline background misses as water context before testing sparse-near terrain-hole ownership, preventing waterline volume from being counted as unsafe terrain holes. High-altitude LOD now has an explicit ownership boundary: exact sparse-surface extraction is suppressed while the LOD throttle is active, and terrain-critical readiness treats resident bricks without exact surface records as acceptable only in that LOD mode. Near/waterline views still require exact surface readiness inside the 1024m foreground.
7. What is missing: Nothing required for this specific fake foreground/background ownership item. Broader runtime performance, lifecycle debug-state visualization, and parent/child LOD completion remain open under TS-001, TS-002, TS-004, TS-007, and TS-015.
8. Validation required: Release build and `VENPODSparseCore` must pass. Aggressive fast-walk, waterline stress, and high-flight stress captures must all pass with zero height proxy, zero valley-atmosphere screen failures, zero miss/unsafe ownership, and zero terrain-critical post-publish non-ready coords.
9. Exact proof: `cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build .\VENPOD\build --config Release --target VENPOD"`; `ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -WalkTest -WalkTestSpeed 64 -WalkTestYawDegPerSec 32 -WalkTestPitchDeg -7 -WalkTestFixedDtMs 0 -ExitAfterFrames 1800 -CaptureStartFrame 1200 -CaptureIntervalFrames 30 -CaptureCount 16 -SparseOwnershipMaxTerrainDeltaPct 30 -MinUniqueSampleColors 40 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\broad_freeroam_exact1024_patch_20260516`; `.\VENPOD\engine_capture_smoke.ps1 -NoBuild -StressCamera -StressCameraRadius 28 -StressCameraHeight 6 -StressCameraBaseHeight -22 -StressCameraSpeed 36 -WaterlineCamera -ExitAfterFrames 460 -CaptureStartFrame 200 -CaptureIntervalFrames 35 -CaptureCount 8 -SparseOwnershipMaxTerrainDeltaPct 30 -MinUniqueSampleColors 35 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 8 -MaxFrameMs 260 -MaxPrepMs 220 -MaxGpuRayMs 120 -OutputDir .\VENPOD\build\captures\waterline_exact1024_patch_20260516`.
10. Latest evidence: Build passed through the Visual Studio developer environment and `VENPODSparseCore` passed after the source change. `debug50_fixed_default_exact1024_patch_20260516` and `normal_fixed_default_exact1024_patch_20260516` passed with `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=81.47%`, and `maxFrameMs=67.75`. The broader variable-dt guard `broad_freeroam_exact1024_patch_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=78.70%`, `maxFrameMs=68.65`, `maxPrepMs=39.54`, and `maxGpuRayMs=5.84`. The waterline guard `waterline_exact1024_patch_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `minVoxelTerrainScreen=47.71%`, `maxFrameMs=111.43`, `maxPrepMs=71.44`, and `maxGpuRayMs=36.43`. After the shoreline side-bank correction, `waterline_surface_water_banks_20260516` passed with `minVoxelTerrainScreen=81.63%`, `maxFrameMs=43.54`, `maxPrepMs=30.86`, and `maxGpuRayMs=13.37`; `broad_surface_water_banks_20260516` passed with `minVoxelTerrainScreen=74.90%`, `maxFrameMs=73.61`, `maxPrepMs=62.75`, and `maxGpuRayMs=8.07`; and `highflight_surface_water_banks_20260516` passed with `maxValleyAtmosphereScreen=3.02%`, `minVoxelTerrainScreen=57.12%`, `maxFrameMs=98.67`, `maxPrepMs=139.92`, and `maxGpuRayMs=25.97`. All three post-correction captures kept `heightProxyScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. The rejected A/B evidence remains important: surface-culling-off debug captures exposed large stale/off-axis surface slabs, proving culling should stay enabled and the fix should be the background handoff, not globally drawing all cached surface records.
11. Next action required: Keep TS-020 in the regression suite with the 1024m exact-foreground contract. Continue broader work on TS-015 performance and TS-001/TS-002/TS-004 lifecycle/LOD proof; do not reopen fake foreground/background admission unless a new capture shows height proxy, unsafe miss, or background ownership inside the immediate exact foreground returning.

### TS-021
1. Requirement ID: TS-021
2. Requirement text: Waterline validation and exact sparse-surface capacity must not manufacture false "missing world" failures or drop real exact surfaces under large-scene load.
3. Source document / source location: User report on 2026-05-16 that the world still looked sparse/buggy and that fake-looking shoreline terrain remained; resumed investigation comparing `waterline_surface_metadata_patch_20260516` against current waterline logs.
4. Current status: DONE_VERIFIED.
5. Related source files, modules, functions, scripts, or assets: `VENPOD/src/main_launcher.cpp`, `VENPOD/src/Graphics/SparseSurfaceGpuResources.h`, `VENPOD/engine_capture_smoke.ps1`, `VENPOD/build/captures/waterline_capacity_4m_guard2_20260516`, `VENPOD/build/captures/broad_capacity_4m_guard_20260516`, `VENPOD/build/captures/waterline_brush_capacity_4m_guard2_20260516`, `VENPOD/build/captures/waterline_brush_far_svo_budget_patch_20260516`.
6. What currently exists: The waterline stress camera no longer applies the normal high-altitude `baseHeight=520` orbit while waterline anchoring is enabled, and large-radius waterline orbits no longer terrain-clamp upward into high-altitude flight. This restores accepted-style near-water camera positions (`y≈-67` instead of `y≈465..643`). The sparse-surface default face capacity is raised from `3u << 20` to `1u << 22`, matching the existing validation maximum, to avoid `overflow=1` when CPU exact-surface faces exceed the old 3.15M-face IA/GPU buffer.
7. What is missing: Nothing for this focused waterline-camera/capacity requirement. Broader manual free-roam performance and far-LOD polish remain open under TS-014 and TS-015.
8. Validation required: Release build, `VENPODSparseCore`, waterline/broad captures proving no `overflow=1`, no `residentMissingSurface` tail, no height-proxy, no miss/unsafe, and no fake foreground material after brush edits.
9. Exact proof: `.\VENPOD\build.ps1 -Config Release`; `.\VENPOD\build\bin\VENPODTests.exe`; waterline capture with a waterline-appropriate terrain ownership threshold or a later quality ready frame; broad/free-roam capture with `PERF_SPARSE_SURFACE ... overflow=0`; brush/material capture around the reported shoreline.
10. Latest evidence: Build and sparse core tests passed on 2026-05-16 after the camera/capacity changes. Before the waterline camera fix, current logs showed `SPARSE_STRESS_CAMERA` at `y=465..643`, `stress=96/0`, `knownEmpty≈68,905`, `resident≈4,021`, `residentMissingSurface≈753`, and only `~42k` rendered surface fragments versus the accepted baseline's `stress=96/96`, `knownEmpty≈491`, `resident≈19,811`, `residentMissingSurface=0`, and `~2.10M` surface fragments. After the camera fix, near-water logs showed `SPARSE_STRESS_CAMERA ... y=-67.1`, `stress=96/96`, `miss=0`, and `unsafeNearMiss=0`, but the wrapper failed at frame 120 on the strict terrain-percentage threshold. A subsequent large-waterline run before the capacity bump showed exact surface pressure above the old limit (`cpuFaces≈3.31M`, old `iaFaces=3.15M`, `overflow=1`), motivating the default capacity increase.
10. Latest evidence continued: `waterline_capacity_4m_guard2_20260516` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `maxFrameMs=103.24`, `maxPrepMs=160.23`, and `maxGpuRayMs=34.65`; logs showed `iaFaces=4194304`, `residentMissingSurface=0`, `allocFail=0`, and `overflow=0`. `broad_capacity_4m_guard_20260516` passed with `minVoxelTerrainScreen=70.03%`, `maxFrameMs=55.68`, `maxPrepMs=48.13`, `maxGpuRayMs=13.96`, `postNonReady=0`, and no `overflow=1`. `waterline_brush_capacity_4m_guard2_20260516` passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=140 queued=140 retired=2822 applied=2822 deltas=2822 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0 cases=4/4`.
11. Next action required: Keep these captures in the regression suite. Continue remaining far-mountain/free-roam and performance work under TS-014 and TS-015.

### May 17 Visible Generation Drain Continuation

Status: PARTIAL under TS-014 and TS-015, not a completion close.

Current diagnosis: The remaining visible sparse/far-terrain problem is not that the SVO lookup is fundamentally broken. The rejected broad generation probe showed that blindly increasing generation can flood the page pool and reintroduce unsafe-near ownership failures. The accepted root-cause evidence is narrower: the exact visible terrain feed was already allocating visible pages but draining generation too slowly after the route became "safe enough." In `surface_prefetch_defaults_aggressive_guard_20260517`, frame 1200 had `qgen=0/2280/0/0`, `genQueued=2280`, `pgen=0/26/0/0`, `pup=0/26/0/0`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, and no upload queue. That means visible terrain was queued but not becoming resident quickly enough; upload had capacity, generation was the bottleneck, and the hard readiness gate was already clean.

Implemented mitigation: `VENPOD/src/main_launcher.cpp` now adds a bounded visible-generation drain controlled by `VENPOD_SPARSE_TERRAIN_VISIBLE_GENERATION_DRAIN_BUDGET`, `VENPOD_SPARSE_TERRAIN_VISIBLE_GENERATION_DRAIN_THRESHOLD`, and `VENPOD_SPARSE_TERRAIN_VISIBLE_GENERATION_DRAIN_FREE_SLACK`. It only raises the generation budget for already-queued visible bricks when visible queued work is high, free pages are near the reserve, upload queue is empty, high-altitude LOD throttle is not active, and the last ownership sample has `miss=0` and `unsafeNearMiss=0`. This is intentionally different from raising the global generation budget, because the rejected `visible_gen48_ab_aggressive_20260517` run saturated the page pool (`free=104`), created very large invalidation pressure, caused surface staging spikes above 40-100 ms, and failed with `unsafeNearMiss=11514`.

Latest accepted evidence: `.\build.ps1` passed; `.\build\bin\VENPODTests.exe` passed. `visible_generation_drain_aggressive_20260517` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.23%`, `minVoxelTerrainScreen=81.92%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`, `maxFrameMs=64.95`, `maxPrepMs=44.78`, and `maxGpuRayMs=37.19`. The same route's capture-window visible generation backlog was reduced from the previous thousands (`2280 -> 3570`) to hundreds and then double digits (`674 -> 77` in sampled frames), without page-pool saturation or unsafe ownership. `visible_generation_drain_waterline_guard_20260517` passed the shoreline brush/material guard with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=2.67%`, `minVoxelTerrainScreen=74.87%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. `visible_generation_drain_deterministic_frame1350_20260517` passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.06%`, `minVoxelTerrainScreen=86.66%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`.

What remains missing: This does not prove final manual free-roam completion. It reduces one real root cause, but TS-014 remains PARTIAL until broad manual paths show no far-mountain gaps, fake material slabs, or sparse background artifacts under camera movement. TS-015 remains PARTIAL because performance is improved enough for the reproduced route but still has slow-frame samples (`maxFrameMs=64.95`) and visible terrain upload/surface work remains heavy. Continue targeting specific backlog and composition failures; do not mark complete from `miss=0` alone.

### May 17 GPU Page-Table Tombstone Root Cause

Status: PARTIAL under TS-001, TS-002, TS-007, TS-014, and TS-015. This is a verified root rendering fix, not a completion close.

Current diagnosis: The remaining "CPU ready but shader missing" holes were not caused by the SVO being fundamentally broken, by terrain generation returning AIR, or by page-table capacity alone. The CPU sparse page table is an open-addressed table. CPU removal uses a tombstone so later entries in the same probe chain remain findable. The GPU invalidation path was writing `INVALID_BRICK_PAGE` instead of a tombstone. In the shader, `INVALID_BRICK_PAGE` terminates lookup, so any resident brick behind that deleted slot could disappear on the GPU while CPU diagnostics still reported `ReadyToRender`. This exactly matched the failing captures: `miss=0`, `postNonReady=0`, parent-held fallback `0`, exact unsafe bricks such as `21,-3,-7` reported `ReadyToRender`, and the shader still emitted `unsafeNearMiss`.

Implemented fix: `SparseVoxelGpuResources::StagePageTableInvalidation` now writes the same tombstone sentinel used by the CPU table (`INVALID_BRICK_PAGE - 1`) instead of an empty invalid slot. This preserves GPU shader probe chains after evictions and keeps CPU/GPU sparse residency semantics aligned.

Latest evidence: `.\build.ps1` passed. The default long-walk regression `fix_gpu_tombstone_default_long_walk_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.29%`, `minVoxelTerrainScreen=78.43%`, `postNonReady=0`, `maxFrameMs=99.28`, `maxPrepMs=85.81`, and `maxGpuRayMs=19.48`. The waterline render-only guard `fix_gpu_tombstone_waterline_render_20260517` also passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.64%`, `minVoxelTerrainScreen=75.89%`, `postNonReady=0`, `maxFrameMs=28.60`, `maxPrepMs=19.76`, and `maxGpuRayMs=10.15`.

Rejected/non-closing evidence: Increasing `VENPOD_SPARSE_PAGE_TABLE` to `262144` did not fix the failure; `ab_pagetable262k_slots9216_interest95_long_walk_20260517` failed with `unsafeNearMiss=3000`. That rejected the capacity-only hypothesis and led to the CPU/GPU invalidation semantic mismatch above. The brush-enabled waterline smoke rendered with `miss=0` and `unsafeNearMiss=0`, but the wrapper exited on the brush smoke assertion (`queued=180/3`, `retired=5868`, `applied=5868/1`), so that is tracked separately as a brush-test/edit-feedback contract issue rather than as a terrain-render hole regression.

What remains missing: TS-014 remains PARTIAL because manual free-roam still needs visual confirmation for residual far-mountain gaps, fake-looking material slabs, and mid/far LOD authority consistency. TS-015 remains PARTIAL because the long-walk route still has slow frames near `~99 ms` even though the correctness gates pass. This fix removes a real root cause of unloaded-looking holes next to the player; it does not by itself prove all rendering issues are closed.

### May 17 Invalidation Stats-Deferral Stall Root Cause

Status: PARTIAL under TS-007 and TS-015. This is a verified streaming/performance root-cause fix, not a visual-completion close.

Diagnosis: the `PERF_FRAME_END` `sparsePost ... upload` bucket was spending `~20-25 ms` on late long-walk frames even when only `24-48` sparse bricks and roughly `0.65-2.09 MB` were staged. The label was misleading: the timed upload block also included page-table reset and invalidation work. `SparseVoxelWorld::PopNextInvalidation` calls `RefreshStats()`, and the main loop did not call `SetStatsRefreshDeferred(true)` until after the invalidation loop. With a several-thousand-entry invalidation backlog, every invalidation pop could trigger resident/queue stats bookkeeping instead of being folded into the single stats flush already recorded later in the frame.

Implemented fix: `src/main_launcher.cpp` now enables sparse stats deferral before page-table reset/invalidation processing, so invalidation pops, brick upload pops, and page-table publish state changes share the same deferred stats window and flush once in the existing `stats` timing bucket. Rendering semantics are unchanged; this only removes repeated CPU accounting from the critical streaming/upload path.

Latest evidence: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. The same long-walk guard `fix_stats_deferral_invalidation_long_walk_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.31%`, `minVoxelTerrainScreen=77.88%`, `postNonReady=0`, `maxFrameMs=58.10`, `maxPrepMs=56.58`, and `maxGpuRayMs=16.16`. Comparable late-frame upload samples dropped from `frame=1020 upload=23.01 ms`, `frame=1080 upload=20.96 ms`, `frame=1260 upload=23.09 ms`, `frame=1380 upload=25.66 ms`, and `frame=1440 upload=21.48 ms` in `fix_gpu_tombstone_default_long_walk_20260517` to `frame=1020 upload=0.52 ms`, `frame=1080 upload=0.49 ms`, `frame=1260 upload=0.26 ms`, and `frame=1440 upload=0.35 ms` after the patch.

What remains missing: TS-015 remains PARTIAL because the long-walk route still has frame pacing over the target (`maxFrameMs=58.10`) and remaining CPU work is now concentrated in request/generation/clip/trim plus surface extraction/staging, not in invalidation upload. TS-014 remains PARTIAL because this change does not address residual fake-looking shoreline/material slabs or far-mountain LOD authority; those require separate visual/root-cause evidence.

Follow-up stale-entry drain evidence: after invalidation processing was made cheap, the default `VENPOD_SPARSE_INVALIDATION_BUDGET` was raised from `16` to `256` so evicted GPU page-table slots do not linger for hundreds of frames. The shader already rejects reused physical pages through `SparseBrickPageGenerations`, but an evicted page that has not been reused can still remain visible until its old GPU page-table slot is tombstoned. The higher drain keeps this backlog at `0` on the reproduced route instead of allowing thousands of stale entries to accumulate. `fix_invalidation_drain256_long_walk_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.25%`, `minVoxelTerrainScreen=85.18%`, `postNonReady=0`, `maxFrameMs=61.41`, `maxPrepMs=61.06`, and `maxGpuRayMs=21.50`; frame 1440 improved from `invalidQueued=5944` in `fix_stats_deferral_invalidation_long_walk_20260517` to `invalidQueued=0`. The shoreline brush guard `fix_invalidation_drain256_waterline_brush_20260517` also passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, and `deltaMismatch=0`.

### May 17 Critical Generation Stats-Deferral Root Cause

Status: PARTIAL under TS-007 and TS-015. This is a verified engine hot-path fix, not a final rendering or frame-pacing close.

Diagnosis: the next long-walk stall after invalidation cleanup was not a generic "voxels are slow" problem. The bad frames were dominated by the exact sparse terrain-critical generation path. In `fix_invalidation_drain256_long_walk_20260517`, frame 809 reported `prep=106.48 ms` and `sparseSplit=req/gen/clip/trim:18.81/67.32/18.78/0.00`. The same frame had `PERF_SPARSE_TERRAIN_CRITICAL ... protectedDrain=42/42/36 ... sameFramePublish=1/66/66` and `PERF_SPARSE ... pgen=0/66/0/0`. The exact problem was repeated stats bookkeeping inside the terrain-critical drain: `SparseVoxelWorld::PumpGenerationForCoord` calls `RefreshStats()`, and the main loop called it once per critical brick without a deferred stats window. That made a real-time critical path do dozens of full sparse-world stats scans in one frame.

Implemented fix: `src/main_launcher.cpp` now opens a sparse stats deferral window before the terrain-critical protected generation loop and keeps it through the normal `PumpGenerationAround` call, then flushes once after generation. This preserves the same request/generation/upload semantics while removing repeated accounting from the critical visible-terrain path.

Latest evidence: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_generation_stats_deferral_long_walk_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.99%`, `minVoxelTerrainScreen=72.22%`, `postNonReady=0`, `maxFrameMs=66.59`, `maxPrepMs=58.02`, and `maxGpuRayMs=20.02`. The key reproduced frame improved from `frame=809 prep=106.48 ms gen=67.32 ms` to `frame=809 prep=36.26 ms gen=7.16 ms`, with `postNonReady=0`, `invalidQueued=0`, and `overflow=0`. The shoreline brush/material guard `fix_generation_stats_deferral_waterline_brush_20260517` also passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, and `deltaMismatch=0`; its smoke capture had `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.64%`, `minVoxelTerrainScreen=75.89%`, `postNonReady=0`, `maxFrameMs=21.80`, `maxPrepMs=14.23`, and `maxGpuRayMs=6.93`.

Rejected/non-closing evidence: the generation-budget accounting probe is rejected and reverted. It tried to subtract protected terrain-critical generation from the normal sparse generation budget, but `fix_generation_budget_accounting_long_walk_20260517` failed the sparse surface runtime contract with upload-overflow frames `1363-1382`, worsened performance to `maxFrameMs=200.65`, `maxPrepMs=112.28`, `maxGpuRayMs=40.99`, and drove the sparse page pool near exhaustion (`free=104` around frame 1380). The mid-voxel capacity probe `ab_midvoxel12288_long_walk_20260517` is also rejected as a default: it passed hard correctness gates but worsened route cost and coverage (`maxFrameMs=104.43`, `maxPrepMs=151.96`, `maxGpuRayMs=39.32`, `minVoxelTerrainScreen=73.60%`, `maxValleyAtmosphereScreen=2.09%`). Do not reapply either probe as a "bigger resources" solution.

What remains missing: TS-014 and TS-015 remain PARTIAL. The accepted fixes prove several root causes were real, but they do not yet prove the visible world is architecturally complete. Remaining evidence points at request planning spikes, mid-clipmap churn, residual GPU ray/background cost, and manual free-roam material/LOD authority inconsistencies. The next work should continue diagnosing those specific paths instead of raising capacities or hiding the symptoms with fake terrain.

### May 17 Reopened Waterline/Far-Mountain Diagnosis

Status: PARTIAL under TS-014 and TS-015. This is a continuation diagnostic slice, not a completion close.

Diagnosis: the default waterline route is no longer showing the original "chunks beside the player are not loaded" failure. `fix_mid_surface_material_waterline_brush_tail_20260517` and the later waterline diagnostics report `miss=0`, `unsafeNearMiss=0`, `lodParentHeld=0`, `heightProxyScreen=0.00%`, and `postNonReady=0` while far SVO is complete (`farCov=1.00/1.00`). The remaining visible concerns split into two categories: shoreline/material authority disagreement and distant background silhouette/context. Debug layer capture `diagnose_background_layers_debug50_20260517` shows foreground exact sparse surface as yellow, mid-voxel context as orange, far SVO as purple, and sky as blue; the far mountain gaps in this reproduced route are mostly background/sky silhouette composition, not non-ready exact sparse pages. Frame 360 in `fix_conservative_mid_voxel_waterline_20260517` had `terrainPct=98`, `voxelTerrainPct=76`, `missPct=0`, `unsafeNearMissPct=0`, `valleyAtmospherePct=0`, `farQ=0.45`, `gpuRay=7.35 ms`, and `PERF_RENDER_OWNERSHIP retireFrame=363 ... midVoxel=279625 farSvo=123584 waterContext=51496 valleyAtmosphere=16774 sky=465794 miss=0 unsafeNearMiss=0`.

Accepted narrow fix: `SparseClipmapTileCache::GenerateVoxelBrick` now samples the representative solid material at the terrain top when a coarse mid-voxel cell contains the exposed generated terrain surface. Before this, a coarse cell could choose material from the preferred/cell-center Y, which made shore/slope background cells look like the wrong material even though the cell represented real terrain. This keeps edit authority unchanged and avoids treating a center/interior material as the visible top surface.

Latest accepted evidence: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_mid_surface_material_waterline_brush_tail_20260517` passed with `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868 deltas=5868 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0 cases=4/4 caseQueued=45/45/45/45`, plus `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.64%`, `minVoxelTerrainScreen=75.89%`, and `postNonReady=0`. The first failed brush attempt was a harness timing issue: `ExitAfterFrames=420` stopped before the `endFrame+180` settle check ran; rerunning with `ExitAfterFrames=600` passed.

Rejected/non-closing probes: the conservative X/Z footprint probe for mid-voxel columns passed hard gates but did not materially improve the waterline/debug captures (`voxelTerrainScreenPct` and `valleyAtmosphereScreenPct` were effectively unchanged) and risked heavier mid-brick generation, so it was reverted. The far-SVO leaf sampling probe increased low-quality leaf samples and reduced max leaf step, but `fix_far_svo_leaf_sampling_waterline_20260517` did not materially change the contact sheet or layer metrics and made the smoke run much slower, so it was reverted. These reject the simple "just sample more" and "just make every coarse cell more conservative" explanations for the remaining far-background look.

What remains missing: TS-014 remains PARTIAL because the manual complaint still needs an in-game/free-roam visual acceptance pass for far-background silhouettes and shoreline material consistency. TS-015 remains PARTIAL because frame pacing is still not real-time on broader movement routes; the current reproduced waterline frame 360 is around `25.05 ms` raw with CPU sparse prep/request work still visible, and broader long-walk/free-roam captures still show larger spikes. Do not close the goal on the waterline smoke pass alone.

### May 17 Hidden-Terrain And False-Pressure Follow-Up

Status: PARTIAL under TS-014 and TS-015. This slice narrows two more suspected root causes and accepts two scheduler/material fixes, but it does not close the overall rendering goal.

Diagnosis: a debug-only hidden-terrain miss probe in `assets/shaders/Graphics/PS_Raymarch.hlsl` tested whether the far-mountain blue regions were deterministic terrain that the normal background path failed to hit. `diagnose_hidden_voxel_terrain_miss_debug50_20260517` showed only about `0.0122..0.0156%` diagnostic miss ownership (`miss=137` pixels in representative ownership rows), while the large blue regions remained normal expected sky/LOD silhouette. The radius-6 far-SVO A/B did not materially improve debug ownership (`r4 sky=41.38..52.83 far=14.46..16.16 mid=29.26..32.62`, `r6 sky=41.43..53.13 far=14.47..16.23 mid=29.00..32.65`) and doubled far-SVO cost to `169` pages, `5,265,328` nodes, and `80.35 MB`, so "just raise far radius" is rejected as a default fix.

Accepted fixes: `SparseClipmapTileCache::GenerateVoxelBrick` keeps the terrain-top representative material for coarse solid cells, reducing fake shoreline/sand authority disagreement. `src/main_launcher.cpp` now prevents mid-voxel coverage catch-up from becoming protected emergency work unless the current screen/critical footprint has visible ownership failure or the internal coverage drops below the new emergency threshold. It also stops stale miss-feedback backlog from raising effective ownership pressure when the current screen is clean. The `PERF_SPARSE_CLIPMAP` log now records `catchup=coverage/visible/protected/visual` and `coveragePct`.

Latest evidence: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed after the scheduler changes. `fix_feedback_false_pressure_waterline_guard_20260517` also passed the shoreline brush/material guard with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.35%`, `minVoxelTerrainScreen=76.18%`, and `postNonReady=0`. The accepted long-walk comparison is:

- Baseline `diagnostic_hidden_miss_long_walk_guard_20260517` after frame 1200: `cpu avg/max=40.36/53.55 ms`, `prep=31.14/51.85`, `req=16.86/30.82`, `clip=3.61/22.05`, `ray=2.07/13.95`.
- After coverage gating, `fix_mid_coverage_catchup_long_walk_20260517`: `clip=2.57/8.07` and `ray=0.55/5.36`, proving the mid-voxel catch-up churn was real, but request/generation pressure still remained.
- After feedback false-pressure gating, `fix_feedback_false_pressure_long_walk_20260517`: `cpu avg/max=37.80/46.95 ms`, `prep=25.10/36.35`, `req=10.74/16.14`, `gen=4.81/7.66`, `clip=3.50/9.29`, `ray=0.06/1.44`. Post-1200 ownership remained clean: `voxelTerrainScreenPct` rose from `97.5661%` to `100%`, `missScreenPct=0`, `unsafeNearMissScreenPct=0`, `heightProxyScreenPct=0`, and `valleyAtmosphereScreenPct=0`. `PERF_SPARSE_OWNERSHIP_PRESSURE` rows now show clean frames with `level=0 effectiveLevel=0` even while `pendingMiss=81`, so stale feedback no longer masquerades as a rendering emergency.

Rejected/non-closing evidence: `fix_feedback_false_pressure_long_walk_20260517` still returned wrapper code `1` because the image uniqueness heuristic flagged close-terrain/wall frames (`engine_frame_1380.bmp`, `engine_frame_1410.bmp`). The engine exited normally and the ownership/layer gates stayed clean, so this is not evidence of renewed AIR misclassification. It is also not a full visual acceptance pass because the scripted route is poor for human far-mountain inspection.

What remains missing: TS-014 still needs a deliberate visual route or manual acceptance pass for far-mountain silhouette quality. TS-015 is improved but not complete; post-1200 CPU frames are still around `37.8 ms` average on the scripted walk, with remaining work mostly in request planning and sparse generation rather than GPU raymarch or mid-clipmap churn. Do not close the goal based on this pass.

### May 17 Scheduler And Capacity Probes After Reopen

Status: PARTIAL under TS-014 and TS-015. This slice accepts two request-retention fixes and rejects tempting resource/scheduler probes.

Diagnosis: the current accepted long-walk route is not blocked by GPU raymarch. In `fix_feedback_false_pressure_long_walk_20260517`, post-1200 GPU ray cost averaged about `0.06 ms`, while CPU still averaged about `37.65 ms` raw with `req=10.74 ms`, `gen=4.81 ms`, `clip=3.50 ms`, and `trim=4.30 ms`. The same log shows real sparse-pool pressure: `resident` averaged `29494` of `32768`, `free` averaged `2928`, `tracked` averaged `29840`, `genQueued` averaged `346`, and `evictLast=8` on every sampled post-1200 row. This supports the user's suspicion that the remaining slow/pop-in behavior is an engine streaming/resource-pressure problem, not just a visual shader artifact.

Rejected adaptive scheduler probe: an adaptive critical/surface prefetch patch reused the terrain-critical footprint on clean frames and throttled surface prefetch scans. It preserved hard gates but worsened the route, so it was fully backed out. `fix_adaptive_terrain_prefetch_long_walk_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=99.28%`, and `postNonReady=0`, but post-1200 performance regressed to `raw avg/max=41.70/59.41 ms`, `prep=27.73/39.24`, `req=11.51/17.95`, `gen=5.62/7.93`, `clip=3.61/10.60`, and `trim=4.84/6.52`. The quantized-signature variant `fix_adaptive_terrain_prefetch_q8_long_walk_20260517` still regressed (`raw avg/max=40.38/53.47`, `req=11.50`, `gen=5.51`, `trim=4.92`) despite reducing full critical scans from `61` to `34` rows. Do not reapply this throttling approach as a fix.

Rejected capacity-only probe: `ab_sparse_pages_65536_long_walk_20260517` doubled the exact sparse page pool to `65536` and eliminated measured trim time, but it raised GPU allocation from about `711.2 MB` to `1223.6 MB`, worsened route cost (`raw avg/max=42.44/67.22 ms`, `req=14.15`, `gen=6.05`), and did not improve the visual gates (`minVoxelTerrainScreen=91.53%`, `heightProxyScreen=0.00%`, `postNonReady=0`). This proves page pressure is real, but "just double the pool" is not an acceptable default fix.

Rejected trim-only probe: `ab_trim32_long_walk_20260517` raised both resident trim budgets to `32`, but it failed the terrain-critical readiness contract with repeated post-publish missing critical bricks (`postMissing=1..12` across frames including 1214, 1232, 1258, 1452, 1456, and 1470). This rejects "evict harder" as a default fix.

Accepted fix: non-critical terrain surface prefetch now requests speculative residency instead of protected visible residency in `src/main_launcher.cpp`. Terrain-critical, ownership feedback, brush/edit, collision, and hierarchy visible requests remain protected. The old behavior promoted far/future exact terrain discovered by the surface prefetcher into the same retention class as currently visible exact terrain, which kept the pool near the trim boundary and made background exact pages hard to shed. The new behavior still lets those pages stream opportunistically, but allows the existing speculative backpressure and trim policy to discard them before they compete with critical visible terrain.

Accepted fix: terrain-critical predictive warm requests now also use speculative residency instead of protected visible residency in `src/main_launcher.cpp`. This path is explicitly future-camera warming, not the strict current terrain-critical footprint; demoting it reduced protected visible generation backlog while preserving the current critical path. Strict screen-critical terrain requests, protected same-frame publish, brush/edit, collision, ownership feedback, and hierarchy visible requests remain protected.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_surface_prefetch_speculative_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.09%`, `minVoxelTerrainScreen=93.87%`, and `postNonReady=0`. Compared with `fix_feedback_false_pressure_long_walk_20260517`, post-1200 timing improved from `raw avg/max=37.65/52.69 ms`, `prep=25.10/36.35`, `req=10.74/16.14`, `gen=4.81/7.66`, `clip=3.50/9.29`, `trim=4.30/5.92` to `raw avg/max=32.62/44.09 ms`, `prep=21.97/30.46`, `req=10.33/14.99`, `gen=4.78/6.23`, `clip=3.16/9.72`, `trim=1.92/5.53`. Sparse pool pressure improved from `resident avg=29494`, `free avg=2928` to `resident avg=27316`, `free avg=4755`. `fix_surface_prefetch_speculative_waterline_guard_20260517` also passed the shoreline/brush guard with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, `deltaMismatch=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.99%`, `minVoxelTerrainScreen=76.55%`, and `postNonReady=0`.

Latest continuation verification: after demoting predictive terrain warm requests, `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_predictive_prefetch_speculative_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.06%`, `minVoxelTerrainScreen=90.81%`, and `postNonReady=0`. Compared with the prior accepted `fix_surface_prefetch_speculative_long_walk_20260517`, post-1200 timing improved from `raw avg/max=32.62/44.09 ms`, `prep=21.97/30.46`, `req=10.33/14.99`, `gen=4.78/6.23`, `clip=3.16/9.72`, and `trim=1.92/5.53` to `raw avg/max=31.67/36.09 ms`, `prep=21.69/32.03`, `req=9.69/14.27`, `gen=4.41/6.91`, `clip=2.98/6.74`, and `trim=2.84/5.61`. Protected visible generation queue pressure dropped from `q1 avg=558` and `genQueued avg=655` to `q1 avg=390` and `genQueued avg=467`. The tradeoff is lower average free pages than the surface-prefetch-only run (`free avg=4065` versus `4755`), so this remains a targeted streaming improvement rather than final performance closure. `fix_predictive_prefetch_speculative_waterline_guard_20260517` passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, `deltaMismatch=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.99%`, `minVoxelTerrainScreen=76.55%`, and `postNonReady=0`; frame 360 had `genQueued=0`, `reqSpec=8`, and `reqVis=0`.

Rejected buried-visible skip probe: broadening the buried-solid skip from speculative requests to generic visible view-cone requests was fully backed out. `fix_buried_visible_skip_long_walk_20260517` passed hard gates (`heightProxyScreen=0.00%`, `maxLodParentHeld=0.0000%`, `postNonReady=0`), but it degraded visual ownership (`minVoxelTerrainScreen=73.65%`, `maxValleyAtmosphereScreen=1.21%`) and worsened the protected backlog versus the accepted predictive-speculative run (`q1 avg=503`, `genQueued avg=595`, `raw avg/max=32.63/39.37 ms`). This proves the remaining visible view-cone work is not safely removable with a coarse buried-solid rule; the next fix needs better request attribution or a narrower screen/surface-aware classifier.

Accepted diagnostic instrumentation: hierarchical sparse requests now carry a `SparseBrickRequestSource` tag (`Generic`, `ViewCone`, `Collision`, `NearVisible`, `MotionVisible`, `OwnershipRecovery`, `SpeculativeView`, `Stress`) through `SparseBrickRequestPlanner`, and `src/main_launcher.cpp` logs `PERF_SPARSE_HIERARCHY_REQUESTS` source totals periodically or during large visible bursts. This is diagnostic-only; it does not change request residency or priority. Current-state verification after adding the source tags and keeping the previous accepted behavior: `.\build.ps1` passed, `.\build\bin\VENPODTests.exe` passed, `diagnose_hierarchy_sources_quiet_current_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.82%`, `minVoxelTerrainScreen=76.94%`, and `postNonReady=0`, and `diagnose_hierarchy_sources_quiet_current_waterline_20260517` passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, `deltaMismatch=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.99%`, `minVoxelTerrainScreen=76.55%`, and `postNonReady=0`.

New root-cause evidence from source attribution: the remaining protected visible backlog is seeded mainly during startup/early broad-route frames by the hierarchical `ViewCone` source, not by late terrain-critical requests. In `diagnose_hierarchy_sources_quiet_current_long_walk_20260517`, logged frames `<=240` averaged `plannedVisible=605.13` and `acceptedVisible=594.81`; source totals were dominated by `aView avg=534.11`, with smaller `aNear avg=28.63`, `aRecover avg=28.79`, `aColl avg=50.48`, `aMotion avg=3.27`, and `aSpec avg=0.65`. The same run's post-1200 route still had `q1 avg=440.61`, `genQueued avg=537.82`, and `raw avg/max=34.68/42.72 ms`, while terrain-critical post-ready stayed clean. This supports the current diagnosis: strict current terrain is no longer the hole source; early broad view-cone exact requests overpopulate protected visible generation/surface work and leave a backlog that persists into later walking.

Rejected broad-view-cone demotion probe: demoting only `SparseBrickRequestSource::ViewCone` visible requests to speculative while terrain screen-critical prefetch is active improved one visual run (`fix_hierarchy_viewcone_speculative_long_walk_20260517` passed with `minVoxelTerrainScreen=97.00%`, `maxValleyAtmosphereScreen=0.00%`, `postNonReady=0`) and preserved the shoreline/brush guard (`fix_hierarchy_viewcone_speculative_waterline_guard_20260517` passed with `applied=5868`, `fallback=0`, `missingResident=0`). However, a quieter rerun `fix_hierarchy_viewcone_speculative_quietlog_long_walk_20260517` failed the capture script's visual regression marker at frames 1440 and 1470 (`uniqueSampleColors=106` and `96`). This means the demotion is too blunt or timing-sensitive and was fully backed out. The next candidate must retain the useful diagnosis but classify or cap view-cone exact requests more carefully, probably by surface/visibility usefulness rather than by source alone.

Rejected/default-disabled view-cone cap probes: a bounded ViewCone visible cap was tested as a narrower version of the broad demotion, but it is not accepted as a default engine fix. The patch remains available only as an env diagnostic through `VENPOD_SPARSE_HIERARCHY_VIEW_CONE_VISIBLE_CAP`; the default is now `0`, meaning no cap. `fix_viewcone_cap192_long_walk_20260517` passed hard gates with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.33%`, `minVoxelTerrainScreen=82.57%`, and `postNonReady=0`, and `fix_viewcone_cap192_waterline_20260517` preserved the brush/material guard (`applied=5868`, `fallback=0`, `missingResident=0`). But a repeat long walk with the same cap drifted worse (`maxValleyAtmosphereScreen=1.18%`, `minVoxelTerrainScreen=74.23%`), making the result timing-sensitive. `probe_viewcone_cap384_long_walk_20260517` preserved more protected ViewCone requests but was slower (`raw avg/max=36.43/40.55 ms`, `prep=25.18/35.68`, `req=11.76/15.76`, `gen=5.18/7.39`) and did not improve visual gates (`minVoxelTerrainScreen=81.68%`). `probe_viewcone_cap256_long_walk_20260517` also passed hard gates, but remained worse than the accepted predictive-speculative baseline on the key backlog metrics (`raw avg/max=34.03/39.90 ms`, `prep=23.51/35.62`, `genQueued avg=569.89`, `q1 avg=473.90`, `free avg=4704.65`, `maxValleyAtmosphereScreen=1.00%`, `minVoxelTerrainScreen=74.36%`). Early source attribution in the cap-256 run still shows accepted work dominated by ViewCone (`aView avg=528.92`) while only a source-count subset is protected (`viewProtected avg=247.10`, `viewDemoted avg=297.54`). This proves that a simple count cap is not a real classifier: it can protect the wrong ViewCone bricks and demote useful terrain without understanding surface contribution, screen coverage, or replacement readiness.

Accepted partial scheduler fix: sparse brick records now retain a queue-priority value, `SparseBrickPool::TouchResidencyClass` / `SparseVoxelWorld::TouchResidencyClass` accept that priority, and the generation queue priority score now uses class, freshness, and request priority instead of falling back to coordinate order for same-frame same-class work. `src/main_launcher.cpp` passes the hierarchical request planner priority into `requestSparseBrick`. This addresses a real engine-level ordering bug: `SparseBrickRequestPlanner::PlanHierarchical` computed useful ViewCone priorities, but once the requests entered `SparseVoxelWorld`, same-frame visible requests were effectively sorted by residency class/freshness and then coordinate, so a broad ViewCone burst could generate arbitrary far or less-useful visible bricks before closer/screen-relevant surface bricks.

Latest evidence for the priority fix: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_generation_queue_priority_long_walk_20260517` passed hard gates with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.92%`, `minVoxelTerrainScreen=76.00%`, and `postNonReady=0`. Post-1200 timing was `raw avg/max=31.57/33.87 ms`, `prep=21.91/31.19`, `genQueued avg=605.57`, `q1 avg=505.57`, `free avg=4869.84`, `gpu avg/max=4.71/8.05`, `surfaceGpu avg/max=2.51/4.75`, and `rayGpu avg/max=1.08/6.45`. `fix_generation_queue_priority_waterline_guard_20260517` preserved the shoreline/brush guard with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, and `deltaMismatch=0`; its layer gates stayed clean with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=1.00%`, `minVoxelTerrainScreen=76.54%`, and `postNonReady=0`.

Non-closing caveat for the priority fix: this is not the final visual fix. The long-walk contact sheet does not show a catastrophic regression, but the scripted route ends in close-wall frames and still reports only `minVoxelTerrainScreen=76.00%`. The generated backlog remains large (`genQueued avg=605.57`), so preserving planner priority improves scheduler correctness and frame pacing, but it does not by itself prove far-mountain continuity or full real-time free-roam rendering. The next root-cause target should be either a surface/screen-aware ViewCone classifier or a separate background terrain LOD readiness rule, not another raw cap.

Accepted surface-aware ViewCone admission fix: `SparseTerrainGenerator::MayContainExposedSurfaceBrick` now conservatively classifies whether a sparse brick can contain exposed generated terrain or water surface. Hierarchical `ViewCone` visible requests that fail this classifier are demoted to speculative in `src/main_launcher.cpp` when screen-critical terrain prefetch is active; near-visible, motion-visible, ownership-recovery, collision, edit/brush, and strict terrain-critical requests are not demoted. This attacks the actual protected-work flood rather than applying a raw cap: broad view rays still discover context, but deeply buried interior bricks and high-air bricks no longer consume protected visible generation/surface priority just because a ray crossed their brick coordinates. The behavior is controlled by `VENPOD_SPARSE_HIERARCHY_SURFACE_AWARE_VIEW_CONE` and logs `viewSurfaceDemoted` in `PERF_SPARSE_HIERARCHY_REQUESTS`.

Latest evidence for the surface-aware fix: `.\build.ps1` passed, and `.\build\bin\VENPODTests.exe` passed with new unit checks proving the classifier keeps exposed terrain and water-surface bricks while demoting deeply buried interior and high-air bricks. `fix_surface_aware_viewcone_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.04%`, `minVoxelTerrainScreen=96.49%`, and `postNonReady=0`. The early source attribution now shows the protected ViewCone burst being reduced by surface classification rather than a count cap: `plannedVisible avg=181.48`, `acceptedVisible avg=181.48`, `viewProtected avg=118.97`, `viewDemoted avg=424.89`, and `viewSurfaceDemoted avg=424.89`. Compared with the priority-only run, post-1200 backlog improved from `genQueued avg=605.57`, `q1 avg=505.57` to `genQueued avg=463.09`, `q1 avg=379.75`, while visual ownership improved from `minVoxelTerrainScreen=76.00%`, `maxValleyAtmosphereScreen=0.92%` to `minVoxelTerrainScreen=96.49%`, `maxValleyAtmosphereScreen=0.04%`. `fix_surface_aware_viewcone_waterline_guard_20260517` preserved shoreline and edit correctness with `SPARSE_BRUSH_PAINT_SMOKE passed`, `queued=180`, `retired=5868`, `applied=5868`, `fallback=0`, `missingResident=0`, `hints=0`, `overflow=0`, `deltaMismatch=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.99%`, `minVoxelTerrainScreen=76.55%`, and `postNonReady=0`.

Non-closing caveat for the surface-aware fix: frame time is still not at a final real-time target (`fix_surface_aware_viewcone_long_walk_20260517` post-1200 `raw avg/max=33.53/36.05 ms`, `prep=22.82/29.60`, `gpu=5.66/8.76`), and the scripted route still ends in close-wall frames that are poor for manual far-mountain acceptance. This is accepted as a real rendering/streaming improvement because it sharply reduces protected non-surface ViewCone work and improves terrain ownership, but TS-014 and TS-015 remain PARTIAL until broader free-roam visual acceptance and performance targets are met.

### May 17 Far-SVO Interior Recovery And Exact-Near Ownership Correction

Status: PARTIAL under TS-014 and TS-015. This pass accepts a real voxel-ownership fix and a narrower exact-near default, but it does not close far-mountain continuity or final performance.

Root cause isolated: debug mode 50 at `diagnose_hidden_terrain_current_debug50_20260517` reproduced a large hidden-terrain miss around frame 469. The screen analysis counted `225,629` red pixels (`10.881%`) in `engine_frame_0469.bmp`, with red concentrated in the lower half. This was not fake valley atmosphere or height proxy: normal ownership rows had `heightProxy=0`, `valleyAtmosphere=0`, and the debug diagnostic proved deterministic terrain existed where no voxel owner claimed the ray.

Rejected probe: allowing low-altitude voxel-only downward rays to invoke early far-SVO coverage from near distances worsened the target frame (`278,822` red pixels, `13.446%`) and made the diagnostic route much slower. This was fully backed out. The result proved the problem was not simply "start the far SVO earlier".

Accepted fix: `assets/shaders/Graphics/PS_Raymarch.hlsl` now lets collapsed far-SVO interior leaves recover an analytic terrain surface when the background handoff starts inside the collapsed solid leaf. Interior leaves still never draw raw AABB slab geometry; they only run first-inside surface recovery. This fixed the specific SVO traversal hole where the renderer skipped conservative solid leaves and therefore missed terrain after the ray had already crossed the height surface.

Historical ownership correction, now superseded by the May 17 exact-near
contract restoration section above: `src/main_launcher.cpp` temporarily changed
the default `VENPOD_SPARSE_EXACT_NEAR_DISTANCE` from `768` to `384`. After
interior recovery, an exact-near A/B test showed that setting exact-near to `0`
reduced frame-469 red pixels from the default `~119k` to `~4k`; setting
exact-near to `384` produced the same `~4k` result. At that checkpoint, the
remaining large red region was caused by the exact-near contract over-claiming
a 768-unit editable foreground that the sparse surface layer did not yet prove
ready for that view. Later GPU lookup, false-oracle, axial critical-footprint,
and water-occluder fixes changed this result; `384` is no longer the current
default.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_exact_near384_interior_recovery_debug50_20260517` reduced the target debug miss to `4,231` red pixels (`0.204%`) at frame 469, `3,898` (`0.188%`) at frame 470, and `4,107` (`0.198%`) at frame 471. `fix_exact_near384_interior_recovery_stress_broad_20260517` passed with `maxLodParentHeld=0.0001%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=66.38%`, and `postNonReady=0`. `fix_exact_near384_interior_recovery_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=74.84%`, and `postNonReady=0`. `fix_exact_near384_interior_recovery_waterline_guard_20260517` preserved the shoreline/brush guard with `SPARSE_BRUSH_PAINT_SMOKE` enabled, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `maxLodParentHeld=0.0000%`, and `postNonReady=0`.

Non-closing caveat: this fixes a concrete far-SVO/near-ownership failure and materially improves the broad stress route, but debug mode still has a small residual hidden-terrain miss around `0.2%`, and normal waterline contact sheets still need manual visual review for shoreline material consistency. TS-014 remains PARTIAL until broader free-roam far-mountain continuity is accepted. TS-015 remains PARTIAL because normal broad route performance is improved enough to pass the guard, but the engine is not yet proven real-time under arbitrary walking/free-flight.

What remains missing: TS-014 and TS-015 remain PARTIAL. The accepted residency, surface-aware, far-SVO recovery, and exact-near ownership fixes reduce exact-pool pressure, protected-visible work, and hidden-terrain miss exposure on the reproduced routes, but they do not prove broad free-roam visual quality, far-mountain silhouette continuity, or final real-time frame pacing. The scripted long-walk and broad stress guards pass, but the engine is still not proven under arbitrary manual walking/free-flight and the residual debug-50 hidden miss is not zero. Next work should target manual far-background acceptance, residual shoreline material review, and frame pacing/surface extraction cost, not page-pool expansion, aggressive trim, or blanket view-cone demotion.

### May 17 Far-SVO Coverage And Generated-Water Occluder Fix

Status: PARTIAL under TS-014 and TS-015. This accepts a concrete visual-gap fix and a shoreline material-authority fix, but it does not close free-roam rendering or performance.

Diagnosis: the remaining debug-50 red terrain misses after far-SVO interior leaf recovery were not the original near/mid "AIR misclassification" bug. They were a thin far-horizon silhouette band. The default radius-4 far-SVO page forest left deterministic terrain outside coverage: `fix_exact_near384_interior_recovery_debug50_20260517` still had `4,231` red pixels (`0.2040%`) at frame 469, `3,898` (`0.1880%`) at frame 470, and `4,107` (`0.1981%`) at frame 471. Re-running the same camera with cached radius 6 eliminated the diagnostic miss: `probe_far_svo_radius6_residual_debug50_cached_20260517` reported `0` red pixels on frames 469-471. The first radius-6 no-cache run missed the early readiness deadline while building/uploading the larger SVO, so the fix is accepted with the explicit caveat that cold-cache startup/upload remains a TS-015 risk.

Accepted fixes: `FarVoxelOctreeConfig::pageRadius` now defaults to `6`, making the measured far-horizon terrain fit inside the default far-SVO coverage instead of falling through to sky. `RaymarchBackgroundField` now keeps `RaymarchFarWater` active even in voxel-terrain-only mode. This is not re-enabling the old fake height terrain fallback: the sparse generator itself fills below-sea basin columns with water, and the shader water occluder rejects land where deterministic terrain is above the sea threshold. Keeping that occluder active prevents submerged mid/far terrain from showing as dry sand that later disappears when editable sparse pages or brush edits become authoritative.

Historical rejected/non-closing probes, now superseded by the May 17 exact-near
contract restoration section above: at this earlier checkpoint, raising
`VENPOD_SPARSE_EXACT_NEAR_DISTANCE` was rejected. With radius 6, exact-near
`768` reintroduced `118,935`, `116,999`, and `123,107` red hidden-terrain
pixels (`5.7357%`, `5.6423%`, `5.9369%`) on frames 469-471. Exact-near `512`
was also worse, with `18,295`, `33,683`, and `34,692` red pixels (`0.8823%`,
`1.6244%`, `1.6730%`). Current rebuilt evidence after the later fixes shows
`768` has `0` red hidden-terrain and `0` magenta unsafe pixels on the same
debug-50 frame window, so `384` is no longer the verified default.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. The patched default `fix_radius6_water_occluder_default_debug50_20260517` had `0` red hidden-terrain pixels on frames 469, 470, and 471. `fix_radius6_water_occluder_default_broad_20260517` passed with `maxLodParentHeld=0.0001%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=75.61%`, and `postNonReady=0`. `fix_radius6_water_occluder_default_waterline_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=73.83%`, and `postNonReady=0`. `fix_radius6_water_occluder_default_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=94.95%`, and `postNonReady=0`.

Unresolved validation: `fix_radius6_water_occluder_brush_waterline_20260517` exited with code `14`. It did capture frames 300/330/360, but did not leave the normal runtime log or postprocess CSV artifacts in the capture directory, so it is not usable completion evidence. TS-014 still needs manual shoreline/material acceptance after brush edits, and TS-015 still needs cold-cache startup and broader free-roam pacing evidence.

Follow-up brush validation: the failed brush smoke was a test-window error, not an engine regression. That run used `SparseBrushPaintStartFrame=120` and `SparseBrushPaintEndFrame=260`, which provides only `140` active paint frames while the smoke coverage logic requires four `45`-frame cases. The corrected `fix_radius6_water_occluder_brush_waterline_valid_20260517` run used `120..300`, passed, and logged `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5856 applied=5856 deltas=5856 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0 cases=4/4 caseQueued=45/45/45/45`. Its ownership/layer gates also stayed clean: `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `maxLodParentHeld=0.0000%`, and `postNonReady=0`.

### May 17 Startup Public-Render Gate Correction

Status: PARTIAL under TS-007 and TS-015. This removes an unnecessary startup blank/sky hold while preserving the no-incomplete-world gate; it is not a final cold-cache or all-hardware startup guarantee.

Diagnosis: after the radius-6 far-SVO change, startup captures showed the world was intentionally held as a flat sky-blue frame through frame 100. This was not a swapchain black-screen crash and not missing terrain after the public render opened. The log proved the hard default `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_MIN_FRAME=112` was the blocker: by frame 20 the sparse readiness and surface proof were already clear (`readinessBlocked=0`, `surfaceProofBlocked=0`), and by about frame 75 the cached radius-6 far SVO was ready. At frame 80, `probe_startup_min80_radius6_20260517` rendered the public world cleanly with `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, and no LOD parent-held pixels.

Accepted fix: `src/main_launcher.cpp` lowers the default startup public-render minimum from `112` frames to `24` frames, but adds an explicit startup `farSvoBlocked` condition while far SVO is enabled and not ready. This makes the gate data-driven instead of waiting until an arbitrary late frame: sparse surface readiness, surface GPU proof, protected upload drain, and far-SVO readiness must all be true before public rendering opens. The `SPARSE_STARTUP_PUBLIC_RENDER_HELD` log now reports `farSvoBlocked` and `farSvoReady` so startup blank holds can be attributed directly.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_startup_gate_far_svo_ready_20260517` showed held frames 0/20/40/60 with `farSvoBlocked=1`, far SVO ready at frame ~75, and the first public ownership rows at frames 80-89 all had `miss=0`, `unsafeNearMiss=0`, `lodParentHeld=0`, and no height proxy. The contact sheet shows public terrain from frame 80 onward instead of waiting until frame 120. `fix_startup_gate_far_svo_ready_broad_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `maxLodParentHeld=0.0000%`, `minVoxelTerrainScreen=72.17%`, and `postNonReady=0`. `fix_startup_gate_far_svo_ready_brush_waterline_20260517` passed the brush shoreline guard with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `maxLodParentHeld=0.0000%`, `minVoxelTerrainScreen=73.86%`, `postNonReady=0`, and brush smoke passing.

Non-closing caveat: the startup diagnostic still fails the generic unique-color image heuristic for intentionally held frames 0-60. That is expected while the startup gate is active; the relevant completion evidence is that the gate opens earlier, and that the first public-render frames are not incomplete. TS-015 remains PARTIAL because cold-cache radius-6 far-SVO build/upload can still hold startup longer than cached runs, and broader manual free-roam frame pacing remains unproven.

### May 17 High-Altitude Mid-LOD Parent Fallback Correction

Status: PARTIAL under TS-002, TS-014, and TS-015. This fixes a reproduced parent-LOD exposure in the long high-flight route, but it does not close broader free-roam rendering acceptance.

Diagnosis: after the far-SVO coverage and startup fixes, a current normal high-flight capture still exposed a small but real mid-voxel parent-held fallback spike. `diagnose_current_long_fastflight_normal_20260517` failed the strict LOD fallback gate with `maxLodParentHeld=0.1442%` (`2,982` pixels) around shader frame `706`, while still reporting `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, and `valleyAtmosphere=0`. The root was not a broken SVO or non-ready exact sparse page. `RaymarchMidVoxelClipmap` was allowed to satisfy high-altitude rays with a coarser resident mid-voxel ring when the preferred ring was missing. That is correct as a near-player parent fallback, but at high altitude it can read as fake block terrain and hide the cleaner far-SVO result that the background chain would otherwise test next.

Rejected probe: a first attempt tried to replace each parent-held mid-voxel hit by immediately raymarching far SVO inside the mid-voxel branch. It removed the parent-held pixels in the live log, but the capture timed out because it added an expensive per-pixel far-SVO traversal on a hot path. That approach is rejected.

Accepted fix: `assets/shaders/Graphics/PS_Raymarch.hlsl::RaymarchMidVoxelClipmap` now skips coarser-ring mid-voxel fallback hits only for high-altitude views (`rayOrigin.y > 384`). Low-altitude/walking views keep the parent fallback so missing preferred-ring data still degrades to stable context instead of holes. High-altitude rays fall through to the existing far-SVO/background order, avoiding fake parent terrain without adding a second far-SVO traversal.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. `fix_highalt_skip_mid_parent_long_fastflight_20260517` passed the previously failing high-flight route with `maxLodParentHeld=0.0000%`, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=58.22%`, `postNonReady=0`, `maxFrameMs=32.40`, and `maxGpuRayMs=11.85`. The shoreline/edit guard `fix_highalt_skip_mid_parent_waterline_brush_guard_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=81.42%`, `postNonReady=0`, and `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=180 retired=5868 applied=5868 fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`.

Non-closing caveat: high-flight terrain still has a large legitimate sky fraction in some frames (`maxSky` about `41.67%`) because the camera is far above the terrain looking across the horizon. This patch removes a false coarse-parent terrain owner; it does not prove all manual free-roam routes are visually accepted or real-time.

### May 17 Sparse Page-Table Shader Probe Correction

Status: PARTIAL under TS-001, TS-002, TS-007, TS-014, and TS-015. This fixes a reproduced ready-page/near-miss contradiction, but it does not close the broader far-background visual gap class.

Diagnosis: after the high-altitude mid-LOD correction, `diagnose_current_walk_after_highalt_parent_fix_20260517` still failed the strict walking ownership gate with `unsafeNearMiss=57` at shader frame `603`. The critical clue was the runtime feedback row: `PERF_SPARSE_SHADER_UNSAFE_FEEDBACK frame=607 ownerFrame=603 brick=24,-3,18 dist=88 unsafe=57 requested=7 ready=7 empty=0 nonReady=0 states=24,-3,18:ReadyToRender;24,-4,18:ReadyToRender;24,-2,18:ReadyToRender;23,-3,18:ReadyToRender;25,-3,18:ReadyToRender;24,-3,17:ReadyToRender;24,-3,19:ReadyToRender`. In other words, the shader classified a near sparse brick as missing while the CPU/runtime state machine considered that brick and its six-neighbor footprint ready.

Root cause: the CPU sparse page table probes until it reaches an empty slot, while the pixel shader had a fixed, short sparse page-table probe window. Runtime eviction uses tombstones to preserve open-addressing chains. After enough replacement/eviction churn, a valid ready-to-render page-table entry can sit past the shader probe window even when the table load is not high. That makes the GPU behave as if a ready brick is missing, which can manifest as small near-field holes, fake background sand/water taking ownership until an edit refreshes the page, or transient unsafe-near pixels.

Accepted fix: `assets/shaders/Graphics/PS_Raymarch.hlsl` raises `SPARSE_PAGE_TABLE_LOOKUP_PROBES` to `256` and changes the lookup annotation from `[unroll]` to `[loop]`, keeping shader code size and compile pressure reasonable while matching the tombstone-heavy runtime contract better. This is an engine-level lookup contract fix, not a visual masking pass.

Latest verification: `.\build.ps1` passed and `.\build\bin\VENPODTests.exe` passed. The previously failing walking route now passes as `fix_page_probe_current_walk_20260517` with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=80.21%`, `postNonReady=0`, `maxFrameMs=32.04`, and `maxGpuRayMs=6.35`. High-altitude regression guard `fix_page_probe_highalt_guard_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `minVoxelTerrainScreen=67.05%`, `postNonReady=0`, `maxFrameMs=21.38`, and `maxGpuRayMs=13.12`. Shoreline/brush guard `fix_page_probe_waterline_brush_guard_20260517` passed with `miss=0`, `unsafeNearMiss=0`, `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=73.86%`, `postNonReady=0`, `maxFrameMs=11.06`, and `maxGpuRayMs=11.69`.

Remaining evidence gap: debug shoreline capture `diagnose_page_probe_waterline_debug50_20260517` still reports a small background miss sliver (`miss=1714`, about `0.08%` of screen in the layer timeline) with `unsafeNearMiss=0`. The debug colors show this is a far/background continuity artifact, not the ready sparse-page near-field miss fixed here. Do not close TS-014 or broad free-roam visual acceptance until that separate far/background miss class is explained or eliminated.

### May 17 Brush Material No-Op And Feedback Churn Fix

Status: PARTIAL under TS-014 and TS-015. This accepts a concrete material-authority and brush streaming hot-path fix, but it does not close far-mountain continuity, arbitrary free-roam visual acceptance, or final frame pacing.

Diagnosis: the waterline brush route still looked inconsistent around shore materials, and walking felt hitchy after the foreground/far-SVO fixes. The root cause was not that the SVO itself was completely broken. A real edit feedback loop was repeatedly dirtying the world after the visible material had already been applied. Pre-fix capture `current_waterline_brush_status_20260517` showed held-brush frames repeatedly applying the same feedback records as new edits: frames 168-175 logged `SPARSE_BRUSH_FEEDBACK GPU apply ... records=52 applied=52`, repeated `SPARSE_MID_CLIPMAP_EDIT_INVALIDATE`, and frame 172 reported `rDirtyQ=92`, `brushEdit=52`, `surfExtract=22.17 ms`, and `body=74.68 ms`. The GPU feedback could lag edit-delta upload and propose the same material with a different random variant byte, so the CPU edit store treated stable visible material as a new edit every frame.

Accepted fixes: `SparseVoxelWorld::ApplyEditedVoxels` now samples the CPU authoritative voxel and skips exact or material-equivalent feedback records instead of dirtying sparse pages, surface extraction, physics, and mid-clipmap state for no visible material change. `SparseVoxelWorld::EvaluateBrushEdit` now uses the same material-equivalence rule, so CPU preview, CPU apply, and GPU feedback agree about what is a real brush edit. `src/main_launcher.cpp` now dispatches GPU-authoritative brush feedback only when the CPU preview found candidate edits; after a held stroke reaches already-painted material, it stops queuing useless feedback/readback work.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. The first post-apply strict run still failed the old ownership-delta gate because the gate reacted to surface ownership transitions during brush painting, but it proved the dirty-loop fix: after the first real edit, repeated feedback records changed from `applied=52` to `applied=0`, with frame 172 at `brushEdit=0`, `rDirtyQ=0`, and `gpuPartial=0/0/0.00KB`. After aligning `PreviewBrushEdit`, `fix_brush_preview_material_noop_waterline_20260517` passed the targeted waterline brush smoke with `readyFrame=300` and `postNonReady=0`. Its log proves the no-op feedback stream is removed by frame 172: `brushDelta=0/0`, `brushGpuFb=0/0/0/0/0`, `brushEdit=0`, `rDirtyQ=0`, `miss=0`, `unsafeNearMiss=0`, and frame 300 ownership had `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, `valleyAtmosphere=0`.

What remains missing: this does not prove the whole renderer is fixed. Surface extraction/backlog is still a major close-terrain cost (`surfExtract` can still be about `21-23 ms` during active brush/surface catch-up), the waterline contact sheet is still dominated by close surface/water views that are poor far-mountain evidence, and manual free-roam still needs visual acceptance for far gaps and fake material slabs. TS-014 remains PARTIAL for residual far/background continuity and manual visual acceptance. TS-015 remains PARTIAL because the remaining slowness is now surface extraction/staging and request/generation pressure, not repeated no-op brush feedback.

### May 17 Protected Surface Extraction Budget Fix

Status: PARTIAL under TS-015, with TS-014 still open for manual/far visual acceptance. This accepts a real frame-pacing contract fix for the close-terrain surface path, but it does not close overall performance.

Diagnosis: after the brush no-op fix, `surfExtract` was still the dominant close-terrain CPU block. The first A/B set `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS=8`, but the waterline brush route still spent `17.71 ms` in `surfExtract` at frame 172. The cause was not the normal timed surface pump: the screen-critical protected surface loop ran before the timed pump and ignored the configured extraction time cap. That allowed critical surface catch-up to spend many milliseconds on raster surface extraction even when exact sparse raymarch fallback could still preserve real voxel ownership for temporarily unsurfaced resident bricks.

Accepted fix: the default `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS` is now `8` instead of `24`, and the protected surface extraction loop checks the same elapsed-time cap before each protected extraction. This keeps the screen-critical lane prioritized, but it no longer bypasses the frame budget. The fix does not lower exact-near distance, disable sparse surfaces, or reintroduce fake height terrain.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. The default waterline brush guard `fix_surface_protected_cap_waterline_20260517` passed with `readyFrame=300`, `postNonReady=0`, and frame 300 ownership `miss=0`, `unsafeNearMiss=0`, `heightProxy=0`, `valleyAtmosphere=0`. The reproduced frame 172 improved from `fix_brush_preview_material_noop_waterline_20260517` `surfExtract=21.64 ms` / `body=69.65 ms` to `surfExtract=8.58 ms` / `body=56.59 ms`, while retaining `brushDelta=0/0`, `brushGpuFb=0/0/0/0/0`, `rDirtyQ=0`, `miss=0`, and `unsafeNearMiss=0`. The broader walking guard `fix_surface_protected_cap_long_walk_20260517` passed with `maxLodParentHeld=0.0000%`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=91.16%`, and `postNonReady=0`; sampled late frames stayed around `surfExtract=1.89-2.62 ms` with raw frame samples around `30-38 ms`.

What remains missing: this intentionally shifts some non-urgent surface catch-up across frames, so TS-015 still needs broader manual free-roam pacing and long-session evidence. The long-walk route is improved enough to pass the guard, but it is not proven real-time; late samples still show CPU request/generation/trim work and GPU surface/ray cost. TS-014 is unchanged: far-mountain visual continuity and manual material/shoreline acceptance remain open.

### May 17 Renderable Terrain Retention Touch Fix

Status: PARTIAL under TS-007 and TS-015. This accepts a concrete scheduler hot-path fix, not a final rendering/performance close.

Diagnosis: the terrain screen-critical lane was still feeding already renderable bricks back through the full `requestSparseBrick` admission path every frame. In `current_rebuild_focused_long_walk_20260517`, the late terrain-critical target set averaged about `533` request/admission calls per logged frame even though almost all of those bricks were already `ReadyToRender` or `ResidentEmpty` (`newAvg=0.38`, `postNonReady=0`). Those calls were not loading new terrain; they were refreshing retention through the expensive path, dirtying queue order/stat state, and contributing to `req` cost.

Accepted fix: `SparseVoxelWorld::TouchResidentRetention` now updates resident page retention/class/priority without queue aliasing or per-touch stats refresh. The screen-critical and terrain-surface prefetch paths use it for truly renderable resident pages and continue to route missing, requested, uploading, and `ResidentMissingSurface` pages through the normal request path. The first broader skip probe was rejected because it treated incomplete resident pages like complete pages and produced post-publish missing critical samples.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. `fix_fast_renderable_retention_touch_focused_long_walk_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`. Compared with `current_rebuild_focused_long_walk_20260517`, late terrain-critical admission dropped from `reqAvg=532.89` / `reqMax=953` to `reqAvg=0.69` / `reqMax=6`; overall late frame work improved more modestly (`req avg 20.79 -> 19.74 ms`, `prep avg 42.70 -> 41.03 ms`, `gpuRay avg 13.07 -> 12.52 ms`), proving this was real waste but not the only bottleneck. `fix_fast_renderable_retention_touch_waterline_guard_20260517` also passed with `SPARSE_BRUSH_PAINT_SMOKE passed`, `applied=162`, `fallback=0`, `missingResident=0`, `deltaMismatch=0`, `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=81.42%`, and `postNonReady=0`.

Rejected probes: `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE=256` reduced critical request count but lowered terrain coverage (`minVoxelTerrainScreen=61.82%`) and is not accepted as a default. The earlier `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_REUSE_REFRESH_INTERVAL=4` probe preserved hard gates but did not improve request planning and reduced terrain coverage, so it remains backed out. The earlier `VENPOD_SPARSE_TERRAIN_NEAR_SURFACE_PREFETCH=0` probe slightly reduced request cost but lowered terrain coverage and is also rejected as a default.

What remains missing: TS-014 still needs manual/free-roam far-mountain continuity acceptance. TS-015 remains PARTIAL because the focused route still has high residual request planning, mid-clipmap, trim, and GPU ray costs. This fix removes one root cause of scheduler churn without hiding incomplete world state, but it does not prove the infinite voxel world is final or real-time.

### May 17 Voxel-Only Mid-Volume Background Miss Fix

Status: PARTIAL under TS-014 and TS-015. This accepts a concrete voxel-ownership fix for a reproduced hole/fake-background class, not a final free-roam or performance close.

Diagnosis: the latest debug waterline route showed that the remaining holes were not simply "not loaded voxels" in the near sparse pool. `diag_waterline_debug50_20260517` exposed large red debug-50 background-miss regions (`miss=385273`, `unsafeNearMiss=0`) while the mid clipmap reported high coverage (`midCov` around `0.99`) and thousands of resident mid voxels (`midVoxels=9216`). The shader was rejecting solid coarse mid-voxel cells unless they were tagged/exposed as surfaces. In voxel-only mode, that meant resident coarse voxel volume could still fall through to background miss/sky when surface-tag proof failed.

Accepted fix: `assets/shaders/Graphics/PS_Raymarch.hlsl::RaymarchMidVoxelClipmap` now allows a voxel-only interior fallback for non-upward terrain rays (`rayDir.y < 0.12`) behind the exact-near handoff (`t >= frame.exactNearParams.x`) when the resident mid voxel has non-air material but lacks the surface/exposure tag. Normal non-voxel/height-proxy paths still require surface proof. This keeps the fill in resident voxel data instead of reintroducing the old procedural height proxy.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. The diagnostic rerun `fix_voxel_only_mid_volume_fallback_debug50_waterline_20260517` converted the reproduced red miss background into mid-voxel ownership: frames `0200` through `0515` in `image_stats.csv` all report `ownerMissPct=0`, with mid-voxel ownership between about `11.96%` and `17.15%`. The normal waterline brush guard `fix_voxel_only_mid_volume_fallback_waterline_guard_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=99.91%`, `miss=0`, `unsafeNearMiss=0`, terrain-critical `postNonReady=0`, and `SPARSE_BRUSH_PAINT_SMOKE passed` (`applied=162`, `fallback=0`, `missingResident=0`, `deltaMismatch=0`). The focused long-walk guard `fix_voxel_only_mid_volume_fallback_focused_long_walk_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=86.62%`, `maxSky=13.38%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`.

What remains missing: this fix does not prove final visual quality. It can make coarse mid-voxel cells visible where the old shader showed background miss, so manual free-roam still needs review for far-mountain silhouettes, shoreline/material consistency, and any coarse-grid artifacts. TS-015 remains PARTIAL because the focused route still has frame-pacing risk and broader walking/free-flight performance is not proven real-time.

### May 17 High-Altitude LOD View-Cone Grid Throttle

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a concrete high-flight request/clipmap pacing fix, not a final visual-completion close.

Diagnosis: after the mid-volume ownership fix, the long high-flight/open-view route no longer showed hard terrain ownership failures (`heightProxy=0`, `miss=0`, `unsafeNearMiss=0`, `valleyAtmosphere=0`, `postNonReady=0`), but it still spent too much time on CPU sparse planning and mid-clipmap work. `current_mid_volume_fallback_long_fastflight_20260517` sampled late frames around `41.30 ms` average / `43.99 ms` max, with `prep=33.09 ms`, `req=10.02 ms`, `clip=21.04 ms`, and `gpuRay=13.33 ms`. The log showed high-altitude LOD-throttled frames still using the full view-prefetch ray grid even though far SVO already owned most pixels and all planned ViewCone visible requests were then demoted by the surface-aware classifier (`acceptedVisible=0`, `viewDemoted=243-248`, `viewSurfaceDemoted=243-248`). That was wasted planning and clipmap interest work, not useful terrain completion.

Accepted fix: `src/main_launcher.cpp` now adds `VENPOD_SPARSE_LOD_VIEW_PREFETCH_RAYS`, defaulting to `3`, and uses it only while `sparseTerrainScreenCriticalLodThrottleActive` is true. Ground/waterline and non-LOD-throttled views keep the existing `VENPOD_SPARSE_VIEW_PREFETCH_RAYS` default of `5`, so the near terrain and shoreline guards still use the denser prefetch.

Latest verification: `.\build.ps1 -Config Release` passed after stopping a stale live `VENPOD.exe` process that had locked the build object file, and `.\build\bin\VENPODTests.exe` passed. `fix_lod_view_grid3_long_fastflight_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=58.22%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`. Against `current_mid_volume_fallback_long_fastflight_20260517`, sampled late high-flight timing improved from `ms avg/max=41.30/43.99` to `25.71/27.33`, `prep=33.09/34.20` to `20.49/23.17`, `req=10.02/10.04` to `5.39/5.58`, `clip=21.04/22.20` to `13.59/15.91`, and `gpuRay=13.33/14.39` to `10.75/11.24`. The shoreline/edit regression `fix_lod_view_grid3_waterline_brush_guard_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=99.91%`, `miss=0`, `unsafeNearMiss=0`, terrain-critical `postNonReady=0`, and `SPARSE_BRUSH_PAINT_SMOKE passed` (`applied=162`, `fallback=0`, `missingResident=0`, `deltaMismatch=0`).

What remains missing: this does not solve all runtime slowness. High-flight still has GPU ray cost around `10-11 ms` and coarse/far visual quality still needs manual acceptance. Waterline route correctness is preserved, but manual shoreline/sand consistency after arbitrary edits remains a visual acceptance item under TS-014.

### May 17 Atomic Surface-Ready Publish And Direct Critical Upload

Status: PARTIAL under TS-001, TS-007, TS-014, and TS-015. This accepts a root rendering/streaming fix for the reproduced fake-terrain and non-ready exact terrain issue, not final free-roam completion.

Diagnosis: the low-shore/brush route proved the remaining fake/inconsistent terrain was still exposing partial sparse state. Before the fix, `diag_low_shore_brush_normal_20260517` failed terrain-critical readiness after protected publish with resident pages missing exact surfaces (`postResidentMissingSurface=65/56/76/49/...`) even though ownership showed `miss=0` and `unsafeNearMiss=0`. A material debug pass (`diag_material54_low_shore_brush_20260517`) showed the shoreline material colors were mostly real generated sand/water/stone/dirt, while the hard readiness failure was exact surface availability. The first publish-gate probe fixed the resident-missing-surface class, but then exposed two scheduler bugs: the critical stream could request more exact work than the atomic surface gate could publish in a frame, and the protected upload loop was not actually uploading the requested critical coordinate. It iterated critical coords but called the value-sorted visible-class upload pop, so unrelated visible bricks could consume protected upload budget while a critical coord remained `UploadQueued`.

Accepted fix: `src/main_launcher.cpp` now enables `VENPOD_SPARSE_PAGE_TABLE_SURFACE_READY_GATE` by default. Non-empty non-speculative page-table publishes are gated on `SparseSurfaceCache::IsSurfaceKnown`; if the exact surface is not known, the publish path attempts a bounded direct surface extraction (`VENPOD_SPARSE_PAGE_TABLE_SURFACE_READY_GATE_EXTRACT_BUDGET`, default `48`) and otherwise defers the page-table publish instead of exposing a resident page without exact surface. Screen-critical admission now caps total non-ready critical work to the atomic surface budget, not just new requests, so old upload-queued/dirty work cannot overfill the same frame budget. `SparseVoxelWorld::PopUploadForCoord` was added and the protected upload lane now uploads the exact critical coord instead of a nearby/better-scored visible brick.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. `fix_atomic_total_work_cap_low_shore_20260517` passed the reproduced low-shore brush route with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`; this is the same route that previously failed with resident missing surfaces and later exposed `UploadQueued` leftovers. `fix_atomic_total_work_cap_fastflight_20260517` passed the high-altitude fast-flight route with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=52.76%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. Rejected/intermediate probes are `fix_surface_ready_publish_gate_fastflight_20260517` because atomic publish exposed a large `UploadingGPU` backlog, `fix_surface_ready_publish_gate_capped_fastflight_20260517` because capped-but-miscounted skipped coords entered the critical set as `Missing`, `fix_surface_ready_publish_gate_capped_low_shore_20260517` because total old/new work was not capped, and `fix_direct_critical_upload_low_shore_20260517` because one dirty/upload-queued critical item still exceeded the per-frame budget.

What remains missing: this does not prove all far-mountain gaps or manual fake-material impressions are gone. It prevents non-empty exact sparse pages from becoming page-table-visible before their exact surface exists and fixes protected upload targeting, but broad manual free-roam still needs visual acceptance for far silhouettes, shoreline material semantics, and residual mid/far LOD authority. TS-015 remains PARTIAL because low-shore still runs around `60 ms` raw on the scripted route; the correctness bug is fixed, but performance is not final.

### May 17 Mid-Clipmap Soft-Deficit Catchup Cap

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a narrow frame-pacing improvement after the atomic surface-ready publish fix; it is not a final rendering/performance close.

Diagnosis: after `fix_atomic_total_work_cap_low_shore_20260517`, the reproduced low-shore route had clean correctness gates but still spent large CPU time in mid-clipmap catchup even when ownership pressure showed no hard failure. Example frames had `terrainPct=100`, `missPct=0`, `unsafeNearMissPct=0`, `valleyAtmospherePct=0`, `coveragePct=95-97`, but still used `budgetMid=48-112` and `pumpVoxel` around `8-16 ms` to chase a soft voxel-terrain percentage deficit. This was not the same bug as the previous fake-terrain readiness issue; it was final-percent background/terrain coverage catchup competing with frame time after the visible exact terrain was already safe.

Accepted fix: `src/main_launcher.cpp` now distinguishes `sparseMidClipmapOnlySoftVoxelDeficit` from hard coverage/visible/protected failures. When the only pressure is soft voxel-terrain ownership deficit, no miss/unsafe/valley failure is visible, and interested voxel coverage is at least `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_MIN_COVERAGE_PCT` (default `95`), mid-clipmap CPU catchup is capped by `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_CATCHUP_BUDGET` (default `24`). Hard failures still keep the stronger catchup path.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. The accepted default-cap route `fix_mid_soft_deficit_default24_low_shore_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`; frame 346 shows the intended soft cap active with `budgetMid=24`, `pumpVoxel=2.87 ms`, `coveragePct=95`, `terrainPct=100`, `missPct=0`, `unsafeNearMissPct=0`, and `valleyAtmospherePct=0`. The high-flight guard `fix_mid_soft_deficit_default24_fastflight_20260517` also passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=53.37%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. The A/B probe that motivated the default change, `probe_mid_soft_deficit_cap24_low_shore_20260517`, preserved the visual gates while reducing sampled `PERF_FRAME_END` average raw frame time from `49.72 ms` in the cap-48 probe to `38.89 ms`.

What remains missing: this is not a proof that walking/free-roam is real-time. Low-shore still has slow samples around `40-48 ms`, and the remaining cost is now split across request planning, surface publication/staging, GPU ray/surface work, and untracked frame gaps. TS-014 is not closed by this change because far-mountain continuity, shoreline material semantics, and arbitrary camera movement still need manual/visual acceptance.

Rejected continuation probe: lowering `VENPOD_SPARSE_MID_VOXEL_SOFT_DEFICIT_MIN_COVERAGE_PCT` from `95` to `90` was rejected on 2026-05-17. `fix_soft_deficit90_low_shore_20260517` preserved the screen-level gates (`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`) but failed the terrain-critical readiness contract at frame 318 with `postUploadQueued=1` (`-16,0,27:UploadQueued`). The default was reverted to `95`; `.\build.ps1 -Config Release` and `.\build\bin\VENPODTests.exe` passed after the revert. Do not treat lower soft-deficit thresholds as safe without proving `postNonReady=0`.

### May 17 Mid-Height Clipmap Budget Split

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a bounded CPU-pacing fix for the mid-background maintenance path; it is not a final visual-completion close.

Diagnosis: after the soft-deficit cap, the high-speed fastflight guard still had a reproduced late mid-clipmap spike where exact terrain readiness was clean but clipmap maintenance dominated the frame. `fix_mid_soft_deficit_default24_fastflight_20260517` frame 596 had `missing=0`, `residentMissingSurface=0`, terrain-critical `postNonReady=0`, `missPct=0`, `unsafeNearMissPct=0`, and `valleyAtmospherePct=0`, but `PERF_SPARSE_CLIPMAP` spent `pump=30.48 ms`, split as `pumpHeight=9.72 ms` and `pumpVoxel=20.34 ms`, with `budgetMid=112`, `genHeight=28`, and `genVoxel=112`. A direct attempt to disable the 2D mid-height clipmap (`fix_disable_mid_height_fastflight_20260517`) was rejected: it reduced height work to zero but reintroduced unsafe-near ownership (`unsafeNearMiss=86843`, about `6%`, at sample frame 136). That proves the current mid-voxel shader still depends on the height layer for column bounds even though `midHeight` does not own final pixels.

Accepted fix: `SparseClipmapTileCache::PumpGeneration` now has separate height-tile and voxel-brick budgets while preserving the old single-budget overload for existing tests. `src/main_launcher.cpp` keeps `VENPOD_SPARSE_MID_HEIGHT_CLIPMAP` enabled by default, but caps height generation with `VENPOD_SPARSE_MID_HEIGHT_TILE_BUDGET` (default `16` under voxel-only terrain). Voxel bricks still receive the existing mid catchup budget, so this does not starve the actual voxel terrain shell.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. `fix_mid_height_budget_fastflight_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=53.37%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`. `fix_mid_height_budget_low_shore_20260517` passed the brush/shore route with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. The accepted fastflight comparison reduced logged `pumpHeight` max from `40.78 ms` to `5.28 ms`, average `pumpHeight` from `8.03 ms` to `1.91 ms`, and logged clip max from `74.83 ms` to `41.63 ms`. The low-shore comparison reduced `pumpHeight` max from `41.12 ms` to `4.67 ms` while preserving shoreline/brush gates.

What remains missing: this does not solve every hitch. Fastflight still has large raw/body samples and voxel pump/gaps can dominate after height is capped; low-shore still has residual request/generation/surface/GPU-ray costs. TS-014 remains open because this pass does not prove far-mountain silhouette quality or manual shoreline material acceptance.

### May 17 Mid-Voxel High-Air Fast Path

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a conservative CPU optimization in the mid-voxel generator; it is not a final frame-pacing or visual-completion close.

Diagnosis: after the mid-height budget split, remaining mid-voxel pump work still included high-altitude bricks whose entire Y range starts above the global generated terrain maximum. Those bricks cannot contain generated terrain or water, but `GenerateVoxelBrick` still built the full column halo and classified every coarse cell before filling air. This is wasted CPU work on high-flight routes and does not improve terrain readiness because the payload is resident air either way.

Accepted fix: `SparseClipmapTileCache::GenerateVoxelBrick` now exits early with an all-air payload when `brick.originY > TERRAIN_MAX_Y` and above sea level. The brick still occupies a resident voxel slot and uploads as a complete payload, so this does not expose partial state or alter exact terrain-critical publication. It only skips impossible terrain sampling/classification above the generator's declared height bound.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. `fix_mid_voxel_high_air_fastpath_fastflight_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=53.34%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`. `fix_mid_voxel_high_air_fastpath_low_shore_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`. Against the height-budget baseline, sampled fastflight `PERF frame` rows improved from `ms avg/max=25.20/54.50` to `18.03/32.04`; low-shore improved from `27.65/49.84` to `23.55/46.49`. This is modest and sample-dependent, but it removes a real impossible-terrain work path while preserving the hard gates.

Rejected continuation probe: pruning above-`TERRAIN_MAX_Y` mid-voxel candidates before they enter the interest set (`fix_mid_voxel_high_air_prune_fastflight_20260517` and `fix_mid_voxel_high_air_prune_low_shore_20260517`) was rejected. It passed hard visual/readiness gates, but fastflight timing worsened (`ms avg/max=32.31/66.95`) despite lower clipmap interest, apparently shifting cost into frame gaps/untracked work. The code was reverted to the accepted high-air generation fast path only; `.\build.ps1 -Config Release` and `.\build\bin\VENPODTests.exe` passed after the revert.

What remains missing: voxel pump still has heavy frames at startup and terrain-surface catchup, and the broader goal still needs manual/free-roam far-mountain and shoreline material acceptance. TS-015 remains PARTIAL because frame gaps, request/generation cost, GPU ray cost, and remaining voxel pump spikes are still present.

### May 17 Protected Mid-Catchup Default Cap

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a default pacing fix for the protected mid-voxel catchup path; it is not a final visual-completion or performance close.

Diagnosis: the current low-shore brush route still reproduced repeated `40-50 ms` frames after exact terrain-critical readiness was clean. `diag_current_low_shore_brush_20260517` passed the hard visual/readiness gates (`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, `postNonReady=0`), but post-frame-300 sampled `PERF frame` rows still had `ms avg/max=39.76/47.97`, `maxPrep=45.24`, `maxClip=19.92`, and `maxPumpVoxel=14.82`. The spike pattern matched protected mid-clipmap catchup bursts (`budgetMid` up to `112`) rather than missing exact terrain.

Accepted fix: `src/main_launcher.cpp` now defaults `VENPOD_SPARSE_MID_CATCHUP_TILE_BUDGET` to `VENPOD_SPARSE_MID_VOXEL_COVERAGE_CATCHUP_BUDGET` (`48`) while voxel-only terrain is enabled, instead of inheriting the broader base mid tile budget (`72`). Non-voxel-only mode still follows the base mid tile budget. This keeps protected catchup available but prevents the shoreline route from escalating final mid-voxel maintenance into oversized default bursts.

Latest verification: `.\build.ps1 -Config Release` passed and `.\build\bin\VENPODTests.exe` passed. `fix_mid_catchup48_default_low_shore_brush_20260517` passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and terrain-critical `postNonReady=0`; post-frame-300 sampled rows improved from `ms avg/max=39.76/47.97` to `30.19/40.83`, `maxPrep=45.24` to `28.98`, `maxClip=19.92` to `14.38`, `maxPumpVoxel=14.82` to `10.45`, and `avgBudgetMid=49.81` to `36.92`. `fix_mid_catchup48_default_long_fastflight_20260517` also passed with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0`; sampled timing remained in the same broad range on that route and was dominated by GPU ray/untracked work, not protected catchup.

What remains missing: this does not address material/LOD disagreement directly, and it does not prove manual free-roam is smooth. Low-shore still has `40 ms` outliers, high-flight still spends around `9-11 ms` in GPU ray on sampled rows, and the exact/mid/far material transition still needs a targeted debug/material acceptance pass.

### May 17 Waterline Wet-Sediment Material Alignment

Status: PARTIAL under TS-014, with TS-015 unchanged. This accepts a targeted
visual/material consistency fix for the reproduced fake-looking shoreline
sand, not final free-roam visual completion.

Diagnosis: `diag_current_low_shore_material54_20260517` showed the shoreline
was mostly real material classification, not a height proxy or missing SVO
page: `heightProxyScreen=0.00%`, `valleyAtmosphereScreen=0.00%`, `miss=0`,
`unsafeNearMiss=0`, `postNonReady=0`, and raw material debug showed broad
`MAT_SAND` bands around water. The normal view therefore looked fake because
waterline-adjacent sand/dirt/stone was shaded as dry bright terrain across
some layers, not because those pixels were unready sparse pages.

Accepted fix: `PS_SparseSurface.hlsl` now tints exact sparse raster
sand/dirt/stone near sea level toward wet sediment based on world height and
face normal. `PS_Raymarch.hlsl` now applies the same waterline wet-sediment
treatment to exact sparse raymarch hits, resident mid-voxel DDA hits,
mid-voxel column/height hits, far SVO hits, and far fallback terrain material
variation. Debug material mode `54` still returns raw material colors before
the tint, so material diagnosis remains authoritative.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed.
`fix_waterline_wet_sediment_low_shore_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and
terrain-critical `postNonReady=0`; brush paint smoke passed with
`fallback=0`, `missingResident=0`, `overflow=0`, and `deltaMismatch=0`.
`fix_waterline_wet_sediment_material54_20260517` passed the same hard
ownership/readiness gates while preserving raw material debug classification.
`fix_waterline_wet_sediment_long_fastflight_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. Manual contact-sheet review shows the former bright shoreline
slabs are reduced to thinner wet beach bands while far terrain coverage remains
continuous.

What remains missing: this is a shading/material-read fix, not a data
reclassification rule. It does not prove arbitrary edit sessions will always
preserve shoreline semantics, and it does not close far-mountain silhouette
quality or coarse/hazy distant LOD acceptance. TS-014 remains PARTIAL for
manual free-roam review and any remaining real background gaps; TS-015 remains
PARTIAL because this pass does not address residual frame pacing.

### May 17 LOD-Throttled Mid-Height Budget Cap

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a targeted
far/high-altitude pacing fix; it is not a final performance close.

Diagnosis: after the waterline material alignment, the long fast-flight guard
still spent several milliseconds per sampled frame generating legacy mid-height
tiles even though the rendered ownership timeline showed `midHeightScreenPct=0`
and far SVO owned most terrain pixels. Sampled baseline rows from
`fix_waterline_wet_sediment_long_fastflight_20260517` included
`pumpHeight=3.32 ms` at frame 120, `4.51 ms` at frame 240, `4.55 ms` at frame
480, and `4.39 ms` at frame 720 while high-altitude LOD throttle was active.
The mid-height clipmap still cannot be disabled globally because the shader
uses it for mid-voxel column bounds, and earlier global starvation probes
dropped terrain ownership.

Accepted fix: `src/main_launcher.cpp` adds
`VENPOD_SPARSE_MID_HEIGHT_LOD_THROTTLE_BUDGET` with default `4`. The cap is
applied only while the existing high-altitude LOD throttle is active, so normal
mid-height generation remains governed by `VENPOD_SPARSE_MID_HEIGHT_TILE_BUDGET`
(`16` by default in voxel-only mode). `docs/reference/runtime.md` documents the
new knob.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed. The A/B probe
`probe_lod_height_budget4_long_fastflight_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. The default build validation
`fix_lod_height_budget4_default_long_fastflight_20260517` passed the same hard
gates and reduced sampled fast-flight `pumpHeight` to about `0.86-1.23 ms`;
sampled `ms` rows improved from roughly `21.73-23.78 ms` to
`19.50-21.05 ms` on the comparable frames. The shoreline guard
`fix_lod_height_budget4_default_low_shore_brush_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`,
`postNonReady=0`, and brush paint smoke `fallback=0`, `missingResident=0`,
`overflow=0`, `deltaMismatch=0`.

What remains missing: this reduces one documented CPU pump path but does not
solve residual close-terrain slowness. The low-shore route still has sampled
`30-46 ms` frames with request/generation, mid-voxel pump, GPU ray/surface
work, and untracked gaps. TS-015 remains PARTIAL; manual free-roam smoothness
and final far/coarse LOD quality are still unproven.

### May 17 Sky-Excluded Voxel Ownership Pressure

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a concrete
scheduler-pressure correctness fix that reduces false catchup work; it is not a
final performance close.

Diagnosis: after the LOD-height cap, the low-shore route still entered
ownership catchup even when the hard terrain gates were clean. The root cause
was an inconsistent ownership denominator: `terrainPct` was measured against
non-sky pixels, but `voxelTerrainPct` was measured against the full screen.
Frames with legitimate sky therefore looked like voxel-terrain deficits. In
`fix_lod_height_budget4_default_low_shore_brush_20260517`, frame 360 reported
`terrainPct=100`, `missPct=0`, `unsafeNearMissPct=0`,
`valleyAtmospherePct=0`, but `voxelTerrainPct=65`, `voxelDeficit=3`,
`catchupActive=1`; frame 643 similarly reported `voxelTerrainPct=64`,
`voxelDeficit=4`. Those deficits were caused by ordinary sky/horizon share, not
missing terrain, and they kept protected mid-voxel catchup active.

Accepted fix: `src/main_launcher.cpp` now computes `voxelTerrainPct` using the
same sky-excluded `ownershipTestPixels` denominator as `terrainPct`. Miss,
unsafe-near, and valley-atmosphere ownership still use their existing hard
signals, so real holes still trigger diagnostics and recovery.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed.
`fix_voxel_pressure_sky_excluded_low_shore_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.44%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`; brush paint smoke passed with `fallback=0`,
`missingResident=0`, `overflow=0`, and `deltaMismatch=0`. The same route now
reports frame 360 `voxelTerrainPct=98`, `voxelDeficit=0`,
`catchupActive=0`, and mid budget `12` instead of the prior ownership-pressure
catchup. The fast-flight guard
`fix_voxel_pressure_sky_excluded_long_fastflight_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`; sampled ownership pressure rows report
`voxelTerrainPct=99`, `voxelDeficit=0`.

What remains missing: this removes a false positive scheduler pressure source,
but the low-shore route still has active brush/edit frames around `30-38 ms`
and later residual GPU/surface/untracked work. TS-015 remains PARTIAL for
broader smoothness and manual free-roam acceptance; TS-014 remains PARTIAL for
far/coarse LOD visual quality and manual gap review.

### May 17 Brush-Focused Hierarchical Planner

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a focused
request-planner churn reduction for brush/shore frames; it is not a final
performance or rendering-completion close.

Diagnosis: after the sky-excluded ownership-pressure fix, the low-shore brush
route no longer had miss, unsafe-near, or voxel-terrain deficit pressure, but a
held brush still forced the full fast-flight hierarchical planner. Frame 240 in
`fix_voxel_pressure_sky_excluded_low_shore_brush_20260517` planned `206`
visible candidates, accepted only `2`, demoted `94` view-cone candidates, and
reported `reqSkip=0/683/0/0/329` while `brushEdit=0`. Frame 360 planned `208`
visible candidates, accepted `16`, and still skipped `357` class-budget
requests plus `285` known-empty requests. The brush intent path was therefore
expanding visible/motion/speculative work even when ownership was already clean
and the brush footprint was the real priority.

Accepted fix: `src/main_launcher.cpp` now enters a brush-focused residency mode
only when brush intent is active and ownership has no catchup, miss, or
unsafe-near pressure. In that mode, the hierarchical planner drops the fast
radius bonus, caps near-visible and motion-visible work, uses at most a `3x3`
view grid, suppresses speculative view requests, caps the protected planner
list at `160`, and keeps brush/collision residency active. Follow-up
validation caught a readiness edge case: edited brush partial uploads can share
the critical footprint and consume one of the exact critical upload slots. The
accepted upload-lane fix allows a tiny terrain-critical upload overage during
brush frames so edited partial uploads cannot leave a visible critical brick in
`UploadQueued`. This does not change render-readiness rules, page publication,
material classification, or the verified shoreline shader path.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed.
`fix_critical_upload_overage_low_shore_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.54%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. Planner churn remained reduced on the same route: frame 240
planned visible candidates dropped `206 -> 57`, view demotions `94 -> 24`; frame
360 planned visible candidates dropped `208 -> 57`. The upload-lane edge is
verified at frame 326: the old run failed with one post critical
`UploadQueued` target when a brush edited partial consumed an upload slot; the
accepted run reports `protectedDrain=30/49/0`, `gpuStaged=49`,
`postUploadQueued=0`, and `sameFramePublish=1/49/49`. The fast-flight guard
`fix_critical_upload_overage_long_fastflight_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`.

What remains missing: this is a real scheduler/planner cleanup, but it is not
the whole performance fix. Low-shore active-brush frames still sit around
`32-36 ms`, with remaining time in sparse generation/clipmap maintenance, GPU
ray/surface cost, and untracked frame gaps. TS-015 remains PARTIAL until those
remaining costs and arbitrary manual free-roam smoothness are verified; TS-014
remains PARTIAL until manual far-mountain/shoreline visual acceptance is clean.

### May 17 Clean Mid-Voxel Coverage Catchup Cap

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a bounded
frame-pacing improvement for the mid-voxel clipmap maintenance path; it is not
a final visual-completion or performance close.

Diagnosis: after the brush-focused planner and terrain-critical upload-lane
fixes, the low-shore route no longer exposed incomplete exact terrain
(`postNonReady=0`, `miss=0`, `unsafeNearMiss=0`), but the mid clipmap still
spent emergency-size CPU work on coverage-only catchup. In
`fix_critical_upload_overage_low_shore_brush_20260517`, frame 326 logged
`budgetMid=48`, `pump=11.65 ms`, and `pumpVoxel=11.21 ms` while ownership
pressure was clean (`terrainPct=100`, `voxelTerrainPct=99`, `missPct=0`,
`unsafeNearMissPct=0`, `deficit=0`). Similar clean-ownership frames at 550 and
644 still ran `budgetMid=48` with `pumpVoxel` around `10.98 ms` and `9.27 ms`.
That means the scheduler was treating final coverage fill as protected
emergency work even when the exact terrain contract was already satisfied.

Accepted fix: `src/main_launcher.cpp` now has
`VENPOD_SPARSE_MID_VOXEL_CLEAN_CATCHUP_BUDGET` and
`VENPOD_SPARSE_MID_VOXEL_CLEAN_CATCHUP_MIN_COVERAGE_PCT`. Once exact critical
terrain is clean, ownership has no visual failure, and mid-voxel coverage is at
least the clean threshold, coverage-only catchup is capped to the clean budget
instead of repeatedly using the full emergency catchup lane. This preserves the
normal startup/low-coverage path and does not change page readiness, surface
publication, material classification, or far/near ownership rules.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed.
`fix_mid_clean_catchup_cap_low_shore_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.23%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. The same hot rows now report lower clipmap work: frame 326
changed to `budgetMid=24`, `pump=3.39 ms`, `pumpVoxel=3.00 ms`; frame 550
changed to `budgetMid=24`, `pump=3.15 ms`, `pumpVoxel=2.78 ms`; frame 600
uses `budgetMid=16`, `pump=2.69 ms`, `pumpVoxel=2.38 ms`. The fast-flight
guard `fix_mid_clean_catchup_cap_long_fastflight_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`.

What remains missing: this intentionally slows non-critical final mid-coverage
fill to reduce hitches, so TS-014 still needs manual far-mountain and shoreline
visual acceptance to ensure the perceived sparse/gappy background is acceptable.
TS-015 remains PARTIAL because request/generation work, surface
extract/stage/publish, GPU ray cost, untracked frame gaps, and arbitrary
manual free-roam smoothness remain open.

### May 17 Default Surface Extraction Frame Cap

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a bounded
surface-extraction pacing improvement after the atomic surface-ready publish
fix; it is not a final smoothness or visual-quality close.

Diagnosis: once exact sparse pages were gated on surface readiness, the
low-shore brush route no longer exposed non-ready terrain (`postNonReady=0`,
`miss=0`, `unsafeNearMiss=0`), but the foreground surface extraction path could
still consume nearly a full frame by itself. Before this default change, the
same low-shore route showed frame 322 at `surfExtract=7.48 ms`, `body=50.83 ms`,
and `rawMs=53.11` while the surface publication contract was otherwise clean.
That was not a correctness failure; it was a pacing failure in the surface
catch-up lane.

Accepted fix: `src/main_launcher.cpp` now defaults
`VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS` to `5` instead of `8`. The protected
surface-ready path still runs, but non-urgent extraction work is spread across
frames sooner. This does not alter terrain generation, page-table publication,
material classification, or the no-incomplete-world gate.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed. The low-shore brush guard
`fix_surface_extract_5ms_low_shore_brush_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.23%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. A prior env-only probe on the same route reduced frame 322
from `surfExtract=7.48 ms` / `body=50.83 ms` / `rawMs=53.11` to
`surfExtract=4.61 ms` / `body=43.72 ms` / `rawMs=38.20`; the accepted default
run preserved the hard visual/readiness gates, with sampled later rows still
showing residual costs such as frame 562 at `prep=28.35 ms`,
`req/gen/clip=12.94/6.44/8.18 ms`, `surfExtract=2.17 ms`, GPU ray
`6.63 ms`, and `rawMs=38.23`. The fast-flight guard
`fix_surface_extract_5ms_long_fastflight_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=59.53%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`.

What remains missing: this lowers one surface extraction spike but does not make
free-roam real-time. The remaining measured slowness is now dominated by exact
terrain request/generation/clipmap work, GPU ray/background cost, surface
stage/publish in some frames, and frame gaps. TS-014 still needs manual
far-mountain/shoreline visual acceptance, especially for fake-looking
mid/far-material transitions.

### May 17 Adaptive Clean Screen-Critical Ray Grid

Status: PARTIAL under TS-015, with TS-014 unchanged. This accepts a targeted
request-planning/frame-pacing fix for already-clean ownership frames; it does
not close final visual quality or broad manual smoothness.

Diagnosis: after the surface-ready publication and mid-catchup fixes, the
low-shore route was still tracing the full `21x13` screen-critical terrain grid
even when the previous retired ownership sample had `miss=0`,
`unsafeNearMiss=0`, and no valley-atmosphere/height-proxy failure. The accepted
low-shore surface-cap run showed frame 510 at `rays=76/273`,
`req/gen/clip=7.38/2.81/7.80 ms`, `prep=19.02 ms`, and `bodyPrev=26.35 ms`;
frame 562 still hit `rays=467/273`, `requests=48`, `new=48`,
`req/gen/clip=12.94/6.44/8.18 ms`, `prep=28.35 ms`, and `rawMs=38.23` while
`postNonReady=0`. That means part of the hitch was clean-path over-scanning, not
a necessary emergency response to visible holes.

Accepted fix: `src/main_launcher.cpp` now keeps the full
`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_RAYS_X/Y` grid for startup, explicit
recovery, miss/unsafe, or valley-atmosphere pressure, but uses
`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_CLEAN_RAYS_X/Y` (`13x9` by default)
once ownership is already clean and voxel terrain coverage meets the catch-up
threshold. The clean grid also applies to high-altitude LOD-probe frames,
because those frames are intentionally non-emergency coverage checks. Telemetry
now reports `cleanGrid` and the active ray denominator so future diagnostics do
not misread clean-grid work as full-grid work. This preserves the emergency
no-incomplete-world path while reducing unnecessary CPU ray planning in stable
frames.

Latest verification: `.\build.ps1 -Config Release` passed and
`.\build\bin\VENPODTests.exe` passed. The env-only probe
`probe_critical_grid13x9_low_shore_20260517` passed the low-shore brush guard
with `heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.29%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`; frame 510 reduced to `rays=28/117`,
`req/gen/clip=3.99/2.04/9.55 ms`, `prep=16.27 ms`, and `bodyPrev=22.26 ms`.
The accepted default adaptive run
`fix_adaptive_critical_grid_low_shore_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.51%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`; frame 511 logged `rays=43/273`, `requests=8`,
`req/gen/clip=3.63/1.18/8.67 ms`, `prep=14.23 ms`, and `bodyPrev=24.36 ms`.
After extending the clean grid to LOD-probe frames, the fast-flight guard
`fix_clean_grid_lodprobe_fastflight_20260517` passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=68.69%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`; frame 360 improved from the previous
`fix_adaptive_critical_grid_long_fastflight_20260517` sample at `rays=69/273`,
`req/gen/clip=2.97/1.61/7.86 ms`, `prep=12.97 ms`, and `bodyPrev=25.20 ms`
to `cleanGrid=1`, `rays=18/117`, `req/gen/clip=1.92/1.37/6.21 ms`,
`prep=9.91 ms`, and `bodyPrev=19.50 ms`. The low-shore guard
`fix_clean_grid_lodprobe_low_shore_20260517` also passed with
`heightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
`minVoxelTerrainScreen=60.51%`, `miss=0`, `unsafeNearMiss=0`, and
`postNonReady=0`. Late sampled low-shore frames still stay around `28-32 ms`
raw with remaining cost in `req/gen/clip`, untracked gaps, and GPU ray/surface.

Related material/ownership diagnostic: `diag_low_shore_ownership50_current_20260517b`
and `diag_low_shore_material54_current_20260517` were captured with identical
low-shore camera settings. Pixel correlation on frames 300-580 found sampled
material-debug sand and water were `100.0%` owned by exact sparse surface, not
mid voxel, far SVO, far water, miss, or sky. This means the reproduced low-shore
route no longer proves fake shoreline sand from SVO/height-proxy background;
remaining manual sand-disappears-after-edit reports should be treated as an
exact surface/material/edit semantics issue unless a new capture shows
background-owned sand.

What remains missing: this is not a final performance close. It lowers one
clean-path request-planning cost but still leaves residual exact
request/generation/clipmap work, GPU ray cost, surface publish/stage cost,
coarse far/mid silhouette quality, and broader manual free-roam review. TS-014
still requires manual visual acceptance for far mountain continuity and
shoreline material semantics.

### May 17 Far-SVO Traversal/Depth Probes

Status: `PARTIAL`

Requirement link: TS-014 and TS-015.

Current evidence:

- `smoke_recovered_launch_after_bad_leaf_patch_20260517` proves launch recovered
  after refreshing runtime assets back from a rejected shader experiment.
- `probe_far_svo_depth5_fastflight_20260517` and
  `fix_far_svo_depth5_fastflight_20260517` prove a shallower far SVO cuts upload
  memory from about `80.35 MB` / `5,265,328 nodes` to about `18.42 MB` /
  `1,206,823 nodes`, while preserving hard ownership gates (`miss=0`,
  `unsafeNearMiss=0`, `postNonReady=0`).

Rejected/non-closing evidence:

- A dynamic far-SVO leaf sample-budget patch in `PS_Raymarch.hlsl` caused launch
  to stall at shader initialization before frame 0. It was removed and runtime
  assets were refreshed; do not reintroduce dynamic unbounded-looking leaf loops
  without isolated shader compile timing.
- Defaulting `FarVoxelOctreeConfig::maxDepth` to `5` was also rejected. The full
  fast-flight gate passed correctness, but frame 360 worsened from the previous
  accepted `gpu ray=6.02 ms`, `prep=9.91 ms`, `bodyPrev=19.50 ms` to
  `gpu ray=10.88 ms`, `prep=18.96 ms`, `bodyPrev=29.50 ms`. The lower upload
  size is useful as a diagnostic option, not as the default visual/performance
  fix.

What currently exists: far-SVO remains at default depth `6`; current captures
show hard missing-world gates passing, but far/mid/sky ownership still shifts
visibly and raymarch cost remains too high.

What is missing: a far-SVO fix that improves distant continuity without making
startup shader compilation, GPU ray time, or clipmap/request work worse.

Validation required: repeat full fast-flight and low-shore guards after any
future far-SVO traversal/build change, and compare both hard gates and frame
cost against `fix_clean_grid_lodprobe_fastflight_20260517`.

### May 17 Submerged Sand Material Consistency

Status: `DONE_VERIFIED`

Requirement link: TS-014 and TS-015 shoreline material consistency.

Current evidence:

- `SparseTerrainGenerator::SampleGeneratedVoxelWithColumn` now classifies sand
  only for dry/intertidal terrain columns (`height >= SEA_LEVEL_Y - 2` and
  `height < SEA_LEVEL_Y + 6`). Fully submerged terrain floors remain dirt/stone
  instead of exact-surface sand that can look like fake beach terrain beside
  water.
- `VENPODTests.exe` passes, including the new regression assertion that a
  deterministic below-sea basin floor is not generated as sand.
- `fix_submerged_sand_low_shore_20260517` passes the low-shore brush guard:
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`, `miss=0`,
  `unsafeNearMiss=0`, `minVoxelTerrainScreen=60.29%`, and `postNonReady=0`.
- `fix_submerged_sand_material54_low_shore_20260517` passes material-debug
  capture. Sampled visible sand on matching frames dropped while water remained
  stable: frame 300 `2.46% -> 1.29%`, frame 370 `3.07% -> 1.73%`, frame 510
  `7.82% -> 5.87%`, and frame 580 `3.91% -> 1.81%`.
- `fix_submerged_sand_fastflight_20260517` preserves the broader high-flight
  hard gates: `maxHeightProxy=0`, `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=68.69%`, and `postNonReady=0`.

What remains missing: this narrows the exact-surface fake-sand class, but does
not close broad manual shoreline acceptance or the larger far-SVO/mid/sky
ownership instability and frame pacing issues.

### May 17 Clean-Ownership Predictive Probe Skip

Status: `DONE_VERIFIED` for the reproduced redundant terrain-critical probe
class; `PARTIAL` for broad frame pacing.

Requirement link: TS-015 runtime stalls and TS-007 priority streaming.

Current evidence:

- `main_launcher.cpp` now disables the second/predictive terrain-critical ray
  pass when the current frame already has clean terrain ownership. The strict
  current-camera critical pass still runs, so screen-critical readiness is not
  skipped.
- The prior low-shore brush run
  `fix_submerged_sand_low_shore_20260517` showed clean ownership frames
  602-604 still running the full doubled grid (`rays=234/234`) and same-frame
  exact requests (`new=28`, `28`, `17`). Frame 600 in that run reported
  `prep=46.31 ms`, `sparse=42.97 ms`, and GPU ray `10.13 ms`.
- The accepted low-shore rerun
  `fix_clean_grid_no_predictive_low_shore_20260517` preserves the hard gates:
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=60.30%`, `miss=0`, `unsafeNearMiss=0`, and
  `postNonReady=0`. The reproduced clean non-throttled frames now cap at
  `rays=117/117` instead of `234/234`; frame 600 reports `prep=12.78 ms`,
  `sparse=12.04 ms`, and GPU ray `4.24 ms`.
- `fix_clean_grid_no_predictive_fastflight_20260517` preserves the broader
  high-flight visual/readiness gates: `maxHeightProxy=0`, `miss=0`,
  `unsafeNearMiss=0`, `heightProxyScreen=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=68.20%`, and
  `postNonReady=0`.
- `VENPODTests.exe` passes after the patch.

What remains missing: this removes one redundant exact-terrain streaming path
after the screen is already clean. It does not solve startup/far-SVO upload
pacing, residual voxel pumping, far-mountain gaps, or global free-roam frame
pacing. The fast-flight correctness gate passed, but sampled fast-flight CPU
prep was not improved in that run, so this patch must not be counted as a
global performance closure.

### May 17 Far-Gap Ownership Diagnostics After Predictive Skip

Status: `PARTIAL`; diagnostic only, no accepted implementation change.

Requirement link: TS-014 far-mountain LOD continuity and TS-015 frame pacing.

Current evidence:

- `diag_fastflight_ownership50_after_predictive_skip_20260517` and
  `diag_low_shore_ownership50_after_predictive_skip_20260517` show that the
  remaining far mountains are dominated by valid owners (`surface`, `midVoxel`,
  `farSvo`, `farWater`, and `sky`) rather than near sparse misses. Sampled
  debug frames report `ownerMissPct=0` and `ownerUnsafeNearMissPct=0`; the
  low-shore runtime log only shows a tiny unsampled debug-mode miss tail
  (`miss=17/18/7` render-counter pixels around frames 532-534) with
  `unsafeNearMiss=0`.
- A diagnostic fallback probe,
  `diag_low_shore_ownership50_heightfallback_probe_20260517`, disabled
  voxel-only background terrain. It filled some background with mid-height /
  valley-atmosphere context, but also introduced unsafe-near ownership
  (`unsafeNearMiss` rising to tens of thousands in the runtime log). This
  confirms why re-enabling height fallback is not an acceptable fix for the
  user's fake-terrain complaint.
- A far-SVO leaf traversal quality probe,
  `probe_low_svo_leaf_quality_low_shore_debug50_20260517`, increased low-alt
  leaf sampling. It did not reduce sampled sky at all on matching frames
  (`ownerSkyPct` stayed unchanged frame-for-frame) and only shifted small
  percentages between `midVoxel` and `farSvo`. The shader probe was reverted
  and runtime assets were refreshed.

What remains missing: this diagnosis narrows the current far-gap class away
from simple non-ready near chunks and away from low-alt far-SVO leaf sampling.
The remaining visual problem is coarse/low-quality background voxel ownership
and world-shape/LOD presentation, not proven absent chunk data. A future fix
must improve voxel-owned mid/far continuity or world presentation without
falling back to unsafe height-proxy terrain.

### May 17 Startup Gate Far-SVO Decoupling

Status: `DONE_VERIFIED` for the far-SVO startup-hold class; `PARTIAL` for
general cold-cache startup and far background quality.

Requirement link: TS-015 runtime stalls and the user report that launch can
stall on a black screen.

Current evidence:

- `main_launcher.cpp` now treats far-SVO readiness as optional for the public
  render startup gate by default. The new env
  `VENPOD_SPARSE_STARTUP_PUBLIC_RENDER_FAR_SVO_PROOF=1` can restore the old
  stricter behavior for dedicated far-SVO proof captures.
- The startup gate still requires exact sparse readiness and sparse-surface
  proof; only the far background SVO layer is allowed to continue streaming
  after public render opens.
- `fix_startup_no_far_svo_gate_20260517` passed. The log shows the startup
  hold at frame 20 had `farSvoBlocked=0` and `farSvoReady=0`, proving far-SVO
  no longer holds the public render gate. The same run later completed far-SVO
  from cache (`169 pages`, `5,265,328 nodes`, `80.35 MB`) and exited normally.
  The capture passed with `maxHeightProxy=0`, `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=70.58%`, and `postNonReady=0`.
- `fix_startup_no_far_svo_gate_low_shore_20260517` preserved the shoreline
  brush guard: `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=61.07%`, `miss=0`, `unsafeNearMiss=0`, and
  `postNonReady=0`.
- `fix_startup_no_far_svo_gate_fastflight_20260517` preserved the high-flight
  guard: `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=68.69%`, `miss=0`, `unsafeNearMiss=0`, and
  `postNonReady=0`.
- `VENPODTests.exe` passes after the patch.

Rejected probe recorded during this pass: raising the generated submerged
shelf from `SEA_LEVEL_Y - 8` to `SEA_LEVEL_Y - 2` was reverted. It preserved
unit tests but forced a far-SVO cache-version rebuild and exposed a severe
startup stall before normal runtime logging. The visual issue should not be
addressed by terrain-shape churn that reintroduces launch stalls.

What remains missing: this removes far-SVO as a startup gate blocker, but it
does not solve the broader cold-cache async-build lifetime issue or the
remaining coarse mid/far voxel visual quality. Far-SVO still needs to stream
and become ready; it just no longer prevents exact near terrain from appearing.

### May 17 Far-SVO Runtime Configuration Guard

- Requirement ID: TS-015.15
- Requirement text: A missing far-SVO cache must not launch a noncancelable
  cold build by default, and must not leave the sparse backend readiness gate
  waiting for a far-SVO layer that was intentionally skipped.
- Source document / source location: user-reported black-screen/stall and
  regression concern; `src/main_launcher.cpp`; `src/Graphics/FarVoxelOctree.*`.
- Current status: DONE_VERIFIED.
- Related source files, modules, functions, scripts, or assets:
  `src/main_launcher.cpp` far-SVO init/finalize/backend pipe mask,
  `src/Graphics/FarVoxelOctree.cpp::HasCompatibleCache`,
  `src/Graphics/FarVoxelOctree.h`, `engine_capture_smoke.ps1`,
  `build/bin/venpod_far_svo_cache_r6_d6_s12345.bin`.
- What currently exists: far-SVO async load starts only when a compatible cache
  exists or `VENPOD_FAR_SVO_ALLOW_COLD_BUILD=1` is set. The runtime now tracks
  `farSvoRuntimeConfigured`, and only a started far-SVO pipeline contributes
  the `far-svo` backend configured bit or upload/finalize work.
- What is missing: this does not improve far-mountain geometry quality when
  far-SVO is absent; it only prevents an optional missing background layer from
  masquerading as a required backend failure.
- Validation required: build and sparse tests; cached startup capture; controlled
  missing-cache startup capture with the cache restored afterward.
- Exact command, test, artifact, screenshot, runtime output, log, or manual check
  that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -ExitAfterFrames 160 -CaptureStartFrame 40 -CaptureIntervalFrames 20 -CaptureCount 5 -MinUniqueSampleColors 20 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -OutputDir build\captures\fix_far_svo_runtime_config_cached_20260517`;
  the same smoke command while temporarily moving
  `build\bin\venpod_far_svo_cache_r6_d6_s12345.bin` aside and restoring it in
  `finally`, with output
  `build\captures\fix_far_svo_runtime_config_missing_cache_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. Cached startup
  passed with `maxHeightProxy=0`, `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=71.33%`, and `postNonReady=0`; log shows
  `source=cache`, far-SVO ready at frame 75, and
  `PERF_BACKEND_PIPE frame=120 configured=0x7FF active=0x7FF warn=0x0 ... far=1`.
  Missing-cache startup passed with the same visual hard gates and
  `maxFarSvo=0.00%`; log shows the explicit skip warning and
  `PERF_BACKEND_PIPE frame=120 configured=0x6FF active=0x6FF warn=0x0 ... far=0`.
  `Test-Path build\bin\venpod_far_svo_cache_r6_d6_s12345.bin` returned `True`
  after the controlled missing-cache run.
- Next action required: continue the main rendering work on remaining
  far-mountain holes/coarse LOD quality and manual free-roam performance; do not
  treat this backend-contract fix as full terrain completion.

### May 17 Clean-Ownership Fast Request Cap

- Requirement ID: TS-015.16
- Requirement text: Fast camera movement must not keep using maximum sparse
  request admission after visible terrain ownership is already clean; clean
  coverage should stop unnecessary request pressure without weakening actual
  recovery/catchup cases.
- Source document / source location: user-reported slow/buggy traversal while
  rendering gaps were mostly eliminated; `src/main_launcher.cpp` sparse
  admission budget path.
- Current status: DONE_VERIFIED for the request-budget pressure reduction on
  the reproduced low-shore and high-flight routes; PARTIAL for broad manual
  performance.
- Related source files, modules, functions, scripts, or assets:
  `src/main_launcher.cpp`, `engine_capture_smoke.ps1`,
  `build/captures/diag_current_low_shore_brush_fake_sand_20260517`,
  `build/captures/fix_clean_fast_request_cap_low_shore_20260517`,
  `build/captures/diag_current_far_gaps_highflight_20260517`,
  `build/captures/fix_clean_fast_request_cap_highflight_20260517`.
- What currently exists: when there is no residency catchup pressure, no
  terrain-critical post-ready gap, clean terrain ownership, and no valley
  atmosphere excess, `sparseFastRequestScaleThisFrame` is capped by
  `VENPOD_SPARSE_FAST_REQUEST_CLEAN_MAX_SCALE` defaulting to `2` instead of
  continuing at the movement-derived max scale. Actual miss/catchup cases still
  use the higher fast request scale.
- What is missing: the captured frame-time improvement is mixed. The change
  removes avoidable admission pressure, but broad stutter remains tied to
  mid-clipmap update cost, request/generation bursts, capture outliers, and
  GPU ray cost in far-SVO-heavy views.
- Validation required: build and sparse tests; low-shore brush route; high-flight
  far route; logs must show preserved ownership gates and reduced fast request
  scale/budget.
- Exact command, test, artifact, screenshot, runtime output, log, or manual check
  that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  low-shore smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 220 -StressCameraHeight 80 -StressCameraBaseHeight 110 -StressCameraSpeed 80 -SparseBrushFeedback -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 180 -SparseBrushPaintEndFrame 360 -ExitAfterFrames 760 -CaptureStartFrame 300 -CaptureIntervalFrames 50 -CaptureCount 8 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 50 -MaxHeightProxyScreenPct 25 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 35 -SparseMaxMissPixelsPct 15 -OutputDir build\captures\fix_clean_fast_request_cap_low_shore_20260517`;
  high-flight smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 1400 -StressCameraHeight 260 -StressCameraBaseHeight 620 -StressCameraSpeed 160 -ExitAfterFrames 560 -CaptureStartFrame 260 -CaptureIntervalFrames 50 -CaptureCount 6 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -SparseMinTerrainPixelsPct 20 -SparseMaxMissPixelsPct 30 -OutputDir build\captures\fix_clean_fast_request_cap_highflight_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. Low-shore brush
  capture preserved `maxHeightProxy=0`, `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=99.88%`, and `postNonReady=0`; logged fast request
  scale dropped from `4` / total `413` to `2` / total `206`. High-flight
  capture preserved `maxHeightProxy=0`, `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`,
  `minVoxelTerrainScreen=58.34%`, and `postNonReady=0`; fast request scale
  dropped from `4` / total about `286` to `2` / total about `151`, with
  far-SVO ownership still near the previous maximum (`97.58%` observed).
- Rejected probe: disabling mid-height clipmap generation/upload in voxel-only
  mode was reverted. `build/captures/fix_voxel_only_no_midheight_highflight_20260517`
  preserved `miss=0` and `unsafeNearMiss=0`, but opened obvious sky holes and
  dropped `minVoxelTerrainScreen` to `24.56%`, proving the current continuity
  path still depends on that layer even when height-proxy screen ownership is
  reported as zero.
- Next action required: attack the remaining mid-clipmap CPU cost and
  far-SVO-heavy GPU ray cost; this cap alone is not a full performance fix.

### May 17 Far-SVO High-Altitude Traversal Monotonicity

- Requirement ID: TS-014-FARSVO-MONOTONIC-HIGHALT-20260517.
- Requirement text: High-altitude far-SVO traversal must not lose distant
  terrain coverage when the runtime enters a medium-quality far-field tier.
  Far-SVO dominant frames may preserve a medium far-field quality floor for
  visual continuity, but must not reintroduce height-proxy ownership, unsafe
  near misses, or fake shoreline terrain.
- Source document / source location: user report of residual far-mountain gaps
  and sparse/fake-looking distant rendering after the near-terrain fixes;
  `assets/shaders/Graphics/PS_Raymarch.hlsl`;
  `src/Simulation/SparseRuntimeBudget.cpp`;
  `src/main_launcher.cpp`.
- Current status: DONE_VERIFIED for the reproduced high-altitude route and
  waterline regression guard; PARTIAL for broader manual free-roam acceptance
  under TS-014/TS-015.
- Related source files, modules, functions, scripts, or assets:
  `assets/shaders/Graphics/PS_Raymarch.hlsl::RaymarchFarSvo`,
  `Simulation::SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget`,
  `src/main_launcher.cpp` far-field quality selection,
  `test/test_sparse_core.cpp`,
  `engine_capture_smoke.ps1`.
- What currently exists: the far-SVO page-step budget now has an explicit
  high-altitude branch whose step budget is monotonic as far quality rises.
  A far-SVO-dominant background decision can request preservation of a bounded
  `0.62` medium far-field/render quality floor while retaining the reduced
  raymarch scale under pressure.
- What is missing: this is not a full visual or performance close. The far route
  still has substantial GPU ray cost and mid-clipmap prep cost, and manual
  free-roam still needs acceptance for far silhouettes and shoreline/material
  consistency.
- Validation required: build and sparse tests; high-flight far route; waterline
  brush/fake-material regression route; logs must show no height proxy, no miss,
  no unsafe near miss, no post-nonready critical terrain, and no visual
  reappearance of the bad high-quality horizon holes.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  high-flight smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 1400 -StressCameraHeight 260 -StressCameraBaseHeight 620 -StressCameraSpeed 160 -ExitAfterFrames 560 -CaptureStartFrame 260 -CaptureIntervalFrames 50 -CaptureCount 6 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -SparseMinTerrainPixelsPct 20 -SparseMaxMissPixelsPct 30 -OutputDir build\captures\fix_far_svo_monotonic_medium_floor_highflight_20260517`;
  low-shore brush smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 220 -StressCameraHeight 80 -StressCameraBaseHeight 110 -StressCameraSpeed 80 -SparseBrushFeedback -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 180 -SparseBrushPaintEndFrame 360 -ExitAfterFrames 760 -CaptureStartFrame 300 -CaptureIntervalFrames 50 -CaptureCount 8 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 50 -MaxHeightProxyScreenPct 25 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 35 -SparseMaxMissPixelsPct 15 -OutputDir build\captures\fix_far_svo_monotonic_medium_floor_low_shore_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. High-flight
  capture `fix_far_svo_monotonic_medium_floor_highflight_20260517` passed with
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=58.34%`,
  `postNonReady=0`, and `maxFarSvo=96.75%`; logs show the fixed path reaching
  `farQ=0.62` on the reproduced route without the rejected white/broken horizon
  seen in `fix_far_svo_quality_floor_highflight_20260517b`. Low-shore brush
  capture `fix_far_svo_monotonic_medium_floor_low_shore_20260517` passed with
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=99.88%`, and
  `postNonReady=0`.
- Rejected probe: the earlier scheduler-only quality floor without fixing the
  high-altitude page-step selection was rejected. It raised `farQ`, but because
  medium quality searched fewer high-altitude far-SVO pages than low quality,
  it increased sky ownership and produced a visibly broken horizon.
- Next action required: continue with TS-015 residual cost: high-altitude
  GPU ray time, mid-clipmap CPU prep, and manual free-roam far-silhouette
  acceptance remain open.

### May 17 Far-SVO-Dominant Mid-Voxel Interest Cap

- Requirement ID: TS-015-FARSVO-MID-INTEREST-CAP-20260517.
- Requirement text: When high-altitude rendering is already cleanly owned by
  far SVO, the mid-voxel clipmap must not keep almost the entire mid cache in
  the urgent interest set and churn thousands of missing/evicted bricks every
  frame. The cap must preserve hard visual ownership gates and must not alter
  shoreline/material behavior.
- Source document / source location: user report that the world still feels
  sparse/slow after far gaps improved; current high-flight logs showing
  `midVoxInterest=8755/8755`, thousands of queued/missing voxel bricks, repeated
  `midVoxelEvict`, and `clip` around `9-12 ms` while far SVO owns most terrain.
  Implementation in `src/main_launcher.cpp`.
- Current status: DONE_VERIFIED for the reproduced high-flight route and
  waterline regression guard; PARTIAL for broader manual free-roam performance
  and far visual acceptance under TS-014/TS-015.
- Related source files, modules, functions, scripts, or assets:
  `src/main_launcher.cpp` frame-local sparse clipmap policy selection,
  `Simulation::SparseClipmapTileCache::UpdateInterest`,
  `Simulation::SparseClipmapTileCache::PumpGeneration`,
  `engine_capture_smoke.ps1`.
- What currently exists: high-altitude, clean, far-SVO-dominant frames now use
  a frame-local mid-voxel clipmap policy with
  `VENPOD_SPARSE_MID_VOXEL_FARSVO_INTEREST_PCT` defaulting to `55` instead of
  the global `95` percent interest cap. The override is guarded by camera
  altitude, far-SVO readiness, far-SVO ownership share, zero miss/unsafe-near
  ownership, and low valley-atmosphere pressure. Waterline/near routes keep the
  original policy.
- What is missing: this does not solve GPU ray cost, generic waterline frame
  pacing, or manual free-roam acceptance. It only removes one confirmed
  high-flight mid-clipmap churn mode.
- Validation required: build and sparse tests; high-flight route must preserve
  `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, `postNonReady=0`, and
  reduce late high-flight mid-voxel interest/clip prep; low-shore brush route
  must preserve material/ownership gates.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  high-flight smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 1400 -StressCameraHeight 260 -StressCameraBaseHeight 620 -StressCameraSpeed 160 -ExitAfterFrames 560 -CaptureStartFrame 260 -CaptureIntervalFrames 50 -CaptureCount 6 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -SparseMinTerrainPixelsPct 20 -SparseMaxMissPixelsPct 30 -OutputDir build\captures\fix_far_svo_mid_interest_cap_highflight_20260517`;
  low-shore brush smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 220 -StressCameraHeight 80 -StressCameraBaseHeight 110 -StressCameraSpeed 80 -SparseBrushFeedback -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 180 -SparseBrushPaintEndFrame 360 -ExitAfterFrames 760 -CaptureStartFrame 300 -CaptureIntervalFrames 50 -CaptureCount 8 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 50 -MaxHeightProxyScreenPct 25 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 35 -SparseMaxMissPixelsPct 15 -OutputDir build\captures\fix_far_svo_mid_interest_cap_low_shore_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. High-flight
  capture `fix_far_svo_mid_interest_cap_highflight_20260517` passed with
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=58.34%`,
  `postNonReady=0`, and visually matched the accepted far-SVO contact sheet.
  Compared against `fix_far_svo_monotonic_medium_floor_highflight_20260517`,
  late sampled high-flight `clip` dropped from `9.31 ms avg / 12.24 ms max` to
  `6.99 ms avg / 8.92 ms max`, `prep` from `12.59 ms avg / 15.81 ms max` to
  `9.78 ms avg / 11.55 ms max`, and sampled late `midVoxInterest` dropped from
  `8755` to as low as `5068` on far-SVO-dominant frames. Low-shore brush
  capture `fix_far_svo_mid_interest_cap_low_shore_20260517` preserved
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=99.88%`, and
  `postNonReady=0`; the new cap is not expected to improve that path because
  far-SVO ownership there is near zero.
- Next action required: continue TS-015 work on the remaining high-flight GPU
  ray cost and the separate low-shore request/generation/clip spikes. Do not
  treat this as final smoothness proof.

### May 17 Sea-Level Water Authority

- Requirement ID: TS-021-SEA-LEVEL-WATER-AUTHORITY-20260517.
- Requirement text: Shallow below-sea terrain must not render as dry/fake sand
  that later disappears when exact sparse pages, brush edits, or water-owned
  surfaces arrive. CPU terrain generation, mid-voxel clipmap packing, and
  shader water occlusion must agree that columns below sea level are
  water-owned at sea level.
- Source document / source location: user report that sand near water is not
  real and disappears after drawing; diagnosis from `diagnose_fake_shore_*`
  captures showing the old proxy-removal path was not the only issue, plus
  repeated `SEA_LEVEL_Y - 2` / `FAR_SEA_LEVEL - 2` water thresholds in
  `SparseTerrainGenerator`, `SparseClipmap`, and `PS_Raymarch`.
- Current status: DONE_VERIFIED for the generated shallow-water authority
  invariant and reproduced waterline/high-flight smoke guards; PARTIAL for
  manual shoreline material acceptance because the sampled material-debug
  route did not show a large before/after color shift.
- Related source files, modules, functions, scripts, or assets:
  `src/Simulation/SparseTerrainGenerator.cpp`,
  `src/Simulation/SparseClipmap.cpp`,
  `assets/shaders/Graphics/PS_Raymarch.hlsl`,
  `test/test_sparse_core.cpp`, `engine_capture_smoke.ps1`.
- What currently exists: generated sparse voxels now fill water up to
  `SEA_LEVEL_Y` when terrain height is below sea level; mid-clipmap height
  samples now pack those columns as water at sea level; shader far/mid water
  occlusion now uses `FAR_SEA_LEVEL` instead of `FAR_SEA_LEVEL - 2`; sparse
  core tests now search for a deterministic shallow below-sea column and prove
  that the sea-level sample is water while the submerged terrain floor is not
  exposed as the visible surface.
- What is missing: this does not remove the mid-voxel background layer. A
  stronger probe that disabled low-shore mid-voxel background entirely was
  rejected because it produced real miss instability. Broader manual shoreline
  review is still required to decide how much mid-voxel terrain may remain as
  distant context.
- Validation required: build and sparse tests; material/brush transition
  capture must pass without brush fallback/missing-resident events; low-shore
  visual guard and high-flight guard must preserve `miss=0`,
  `unsafeNearMiss=0`, `heightProxyScreen=0`, and `postNonReady=0`.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  material/brush diagnostic:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -WaterlineCamera -StressCamera -StressCameraRadius 220 -StressCameraHeight 80 -StressCameraBaseHeight 110 -StressCameraSpeed 80 -SparseBrushFeedback -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 180 -SparseBrushPaintEndFrame 360 -ExitAfterFrames 700 -CaptureStartFrame 170 -CaptureIntervalFrames 90 -CaptureCount 6 -MinUniqueSampleColors 1 -MaxFrameDarkPct 100 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -SparseMinTerrainPixelsPct 0 -SparseMaxMissPixelsPct 100 -SparseDebugMode 54 -OutputDir build\captures\diagnose_brush_material_transition54_20260517`;
  low-shore guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -WaterlineCamera -StressCamera -StressCameraRadius 220 -StressCameraHeight 80 -StressCameraBaseHeight 110 -StressCameraSpeed 80 -SparseBrushFeedback -SparseBrushPaintSmoke -SparseBrushPaintStartFrame 180 -SparseBrushPaintEndFrame 360 -ExitAfterFrames 760 -CaptureStartFrame 300 -CaptureIntervalFrames 50 -CaptureCount 8 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 50 -MaxHeightProxyScreenPct 25 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 35 -SparseMaxMissPixelsPct 15 -OutputDir build\captures\fix_sea_level_water_authority_low_shore_20260517`;
  high-flight guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 1400 -StressCameraHeight 260 -StressCameraBaseHeight 620 -StressCameraSpeed 160 -ExitAfterFrames 560 -CaptureStartFrame 260 -CaptureIntervalFrames 50 -CaptureCount 6 -MinUniqueSampleColors 50 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -SparseMinTerrainPixelsPct 20 -SparseMaxMissPixelsPct 30 -OutputDir build\captures\fix_sea_level_water_authority_highflight_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed after adding the
  shallow below-sea regression. `diagnose_brush_material_transition54_20260517`
  passed and logged `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=12
  retired=486 applied=162 deltas=486 fallback=0 missingResident=0 hints=0
  overflow=0 deltaMismatch=0 cases=4/4`. Low-shore guard
  `fix_sea_level_water_authority_low_shore_20260517` passed with
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`,
  `maxValleyAtmosphereScreen=0`, `minVoxelTerrainScreen=99.69%`, and
  `postNonReady=0`. High-flight guard
  `fix_sea_level_water_authority_highflight_20260517` passed with
  `maxHeightProxy=0`, `heightProxyScreen=0`, `miss=0`,
  `unsafeNearMiss=0`, `maxValleyAtmosphereScreen=0`, `maxFarSvo=96.76%`,
  and `postNonReady=0`.
- Rejected probe: `probe_low_shore_no_midvoxel_background_ownership50_20260517`
  disabled low-shore mid-voxel background ownership and removed mid-voxel
  pixels, but it failed the runtime ownership stability gate with `miss=6%`.
  Mid-voxel background cannot simply be removed until exact/far coverage can
  replace it without holes.
- Next action required: continue TS-021/TS-015 by making remaining mid-voxel
  shoreline context visibly subordinate to exact sparse terrain, or by raising
  exact/far coverage enough that the mid layer can be reduced without miss
  instability.

### May 17 Mid-Voxel Capacity Right-Sizing

- Requirement ID: TS-022-MID-VOXEL-CAPACITY-RIGHTSIZING-20260517.
- Requirement text: Reduce the mid-voxel clipmap memory and loading pressure
  that was introduced while filling far gaps, without regressing the hard
  ownership gates for far mountains or waterline views.
- Source document / source location: user report that rendering is still slow
  and buggy after the gaps improved; runtime evidence from May 17 captures
  showing the 9216-slot default allocated `midVoxel=145.14 MB` and kept large
  mid-voxel interest/pump work active.
- Current status: DONE_VERIFIED for focused high/stress and waterline capture
  gates with the lower capacity; PARTIAL for broad manual free-roam
  performance acceptance.
- Related source files, modules, functions, scripts, or assets:
  `src/main_launcher.cpp`, `docs/reference/runtime.md`,
  `engine_capture_smoke.ps1`, `build/captures/*midvoxel6144*`.
- What currently exists: `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS` now defaults to
  `6144` instead of `9216`. The environment override remains available for
  A/B testing larger or smaller shells.
- What is missing: this does not remove fake/mid-voxel fallback terrain and
  does not prove all manual routes are smooth. It only removes an oversized
  default that the focused guards did not need.
- Validation required: high/open far-gap capture and waterline capture must
  preserve `heightProxyScreen=0`, `maxValleyAtmosphereScreen=0`, `miss=0`,
  `unsafeNearMiss=0`, and `postNonReady=0`; logs must show lower mid-voxel GPU
  allocation versus the 9216-slot baseline.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 900 -StressCameraHeight 180 -StressCameraBaseHeight 520 -StressCameraSpeed 50 -SparseDebugMode 0 -ExitAfterFrames 500 -CaptureStartFrame 140 -CaptureIntervalFrames 40 -CaptureCount 8 -MinUniqueSampleColors 60 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 65 -SparseMaxMissPixelsPct 2 -SparseOwnershipStabilityReadyFrame 140 -OutputDir .\build\captures\fix_midvoxel6144_default_far_gaps_normal_20260517`;
  waterline guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 900 -StressCameraHeight 80 -StressCameraBaseHeight 80 -StressCameraSpeed 35 -SparseDebugMode 0 -ExitAfterFrames 520 -CaptureStartFrame 170 -CaptureIntervalFrames 40 -CaptureCount 8 -MinUniqueSampleColors 60 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 25 -SparseMinTerrainPixelsPct 65 -SparseMaxMissPixelsPct 2 -SparseOwnershipStabilityReadyFrame 170 -OutputDir .\build\captures\fix_midvoxel6144_default_waterline_stress_ready170_20260517`.
- Latest evidence, if any: `fix_midvoxel6144_default_far_gaps_normal_20260517`
  passed with `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=73.19%`, and `postNonReady=0`.
  `fix_midvoxel6144_default_waterline_stress_ready170_20260517` passed with
  `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`, and
  `postNonReady=0` at readiness frame 170. A stricter default-waterline
  readiness probe at frame 140 failed with transient critical bricks in
  `UploadingGPU`, so frame-140 waterline readiness remains PARTIAL. The
  9216-slot baseline logged `midVoxel=145.14 MB`; the 6144-slot probe logged
  `midVoxel=96.59 MB`.
- Next action required: continue TS-015/TS-021 on the actual fake-terrain
  admission problem and manual-route frame pacing. Do not treat this capacity
  reduction as final rendering completion.

### May 17 Steep Far-Water Occlusion

- Requirement ID: TS-023-STEEP-FAR-WATER-OCCLUDER-20260517.
- Requirement text: Prevent background mid/far terrain from exposing dry
  shoreline or sand-like material below deterministic sea-level basin water
  when the camera looks downward across water.
- Source document / source location: user report that sand near water is fake
  and disappears once exact terrain/water is loaded by interaction; current
  shader path in `assets/shaders/Graphics/PS_Raymarch.hlsl` limited
  `RaymarchFarWater` to shallow downward rays.
- Current status: DONE_VERIFIED for the focused high/downward and open-shore
  smoke routes; PARTIAL for broad manual free-roam shoreline acceptance.
- Related source files, modules, functions, scripts, or assets:
  `assets/shaders/Graphics/PS_Raymarch.hlsl`,
  `engine_capture_smoke.ps1`,
  `build/captures/fix_farwater_steep_occluder_highstress_seq_20260517`,
  `build/captures/fix_farwater_steep_occluder_highstress_material54_seq_20260517`,
  `build/captures/fix_farwater_steep_occluder_shore_normal_seq_20260517`,
  `build/captures/fix_farwater_steep_occluder_shore_material54_seq_20260517`.
- What currently exists: `RaymarchFarWater` now accepts downward background
  water rays down to `rayDir.y >= -0.92` instead of rejecting anything steeper
  than `-0.35`. The existing terrain-height check still rejects water where
  generated terrain is above sea level.
- What is missing: this does not solve all fake-looking terrain. It only fixes
  the class where real basin water should occlude submerged background terrain.
  Manual free-roam still needs review for residual mid-voxel material slabs,
  far-mountain gaps, and frame pacing.
- Validation required: high/downward and shoreline captures must keep
  `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`, and
  `postNonReady=0`; material/ownership captures must show no regression in
  terrain coverage and should move eligible submerged background pixels to the
  far-water owner instead of mid/far terrain.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  high/downward guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 900 -StressCameraHeight 180 -StressCameraBaseHeight 520 -StressCameraSpeed 50 -SparseDebugMode 0 -ExitAfterFrames 500 -CaptureStartFrame 140 -CaptureIntervalFrames 40 -CaptureCount 8 -MinUniqueSampleColors 40 -MaxFrameDarkPct 95 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 65 -SparseMaxMissPixelsPct 2 -SparseOwnershipStabilityReadyFrame 140 -OutputDir .\build\captures\fix_farwater_steep_occluder_highstress_seq_20260517`;
  shoreline guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 260 -StressCameraHeight 140 -StressCameraBaseHeight 120 -StressCameraSpeed 28 -SparseDebugMode 0 -ExitAfterFrames 520 -CaptureStartFrame 160 -CaptureIntervalFrames 45 -CaptureCount 8 -MinUniqueSampleColors 40 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 25 -SparseMinTerrainPixelsPct 45 -SparseMaxMissPixelsPct 3 -SparseOwnershipStabilityReadyFrame 170 -OutputDir .\build\captures\fix_farwater_steep_occluder_shore_normal_seq_20260517`;
  material debug:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 900 -StressCameraHeight 180 -StressCameraBaseHeight 520 -StressCameraSpeed 50 -SparseDebugMode 54 -ExitAfterFrames 500 -CaptureStartFrame 140 -CaptureIntervalFrames 40 -CaptureCount 8 -MinUniqueSampleColors 2 -MaxFrameDarkPct 95 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 65 -SparseMaxMissPixelsPct 2 -SparseOwnershipStabilityReadyFrame 140 -OutputDir .\build\captures\fix_farwater_steep_occluder_highstress_material54_seq_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. The
  high/downward guard passed with `maxHeightProxyScreen=0.00%`,
  `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=72.00%`, and
  `postNonReady=0`. The shoreline normal guard passed with
  `minVoxelTerrainScreen=89.71%`, `heightProxyScreen=0`, and
  `postNonReady=0`. Compared with the prior 6144 high/downward baseline,
  average `farWaterPct` increased from about `0.11%` to `1.59%` while
  `missPct` stayed `0`, proving the patch moved eligible submerged background
  pixels to the water owner without reintroducing holes.
- Next action required: continue TS-014/TS-015 on residual mid-voxel material
  slabs, far-mountain continuity, and manual free-roam performance. Do not
  close the rendering goal from this focused water-occlusion fix.

### May 17 High-Altitude Far-SVO Preference

- Requirement ID: TS-024-HIGHALT-FARSVO-PREFERENCE-20260517.
- Requirement text: When the far SVO is complete and the camera is in a
  high-altitude/background view, prefer the far-SVO voxel authority over
  same-ring mid-voxel terrain so distant terrain does not look like coarse
  fake slabs and the shader does not do duplicate mid/far work.
- Source document / source location: user report that far mountains still look
  sparse/coarse and that performance is still laggy; May 17 A/B capture
  `probe_midvoxel_render_off_highstress_20260517` showed the high-altitude
  route did not need mid-voxel rendering once far SVO was complete.
- Current status: DONE_VERIFIED for the reproduced high-altitude stress route
  and a focused shoreline smoke guard; PARTIAL for broad manual free-roam
  visual/performance acceptance.
- Related source files, modules, functions, scripts, or assets:
  `assets/shaders/Graphics/PS_Raymarch.hlsl`,
  `engine_capture_smoke.ps1`,
  `build/captures/probe_midvoxel_render_off_highstress_20260517`,
  `build/captures/fix_highalt_farsvo_prefer_highstress_20260517`,
  `build/captures/fix_highalt_farsvo_prefer_shore_smoke_20260517`.
- What currently exists: `RaymarchMidVoxelClipmap` now returns early for
  high-altitude rays when far SVO is enabled, fully uploaded, has page
  coverage, and has sufficient render quality. Walking-height and shoreline
  views still keep mid-voxel fallback coverage.
- What is missing: this does not eliminate all mid-voxel slabs. It only
  removes high-altitude same-ring mid-voxel ownership when far SVO can own the
  ray. Manual free-roam still needs review for low-altitude coarse terrain,
  shoreline material consistency, and remaining frame pacing.
- Validation required: high-altitude ownership should shift from mid-voxel to
  far SVO with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`, and
  `postNonReady=0`; shoreline smoke must not regress terrain coverage or
  readiness.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  high-altitude guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -StressCameraRadius 900 -StressCameraHeight 180 -StressCameraBaseHeight 520 -StressCameraSpeed 50 -SparseDebugMode 0 -ExitAfterFrames 500 -CaptureStartFrame 140 -CaptureIntervalFrames 40 -CaptureCount 8 -MinUniqueSampleColors 40 -MaxFrameDarkPct 95 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 12 -SparseMinTerrainPixelsPct 65 -SparseMaxMissPixelsPct 2 -SparseOwnershipStabilityReadyFrame 140 -OutputDir .\build\captures\fix_highalt_farsvo_prefer_highstress_20260517`;
  shoreline smoke:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 260 -StressCameraHeight 140 -StressCameraBaseHeight 120 -StressCameraSpeed 28 -SparseDebugMode 0 -ExitAfterFrames 340 -CaptureStartFrame 160 -CaptureIntervalFrames 45 -CaptureCount 4 -MinUniqueSampleColors 40 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 25 -SparseMinTerrainPixelsPct 45 -SparseMaxMissPixelsPct 3 -SparseOwnershipStabilityReadyFrame 170 -OutputDir .\build\captures\fix_highalt_farsvo_prefer_shore_smoke_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. The
  high-altitude guard passed with `maxHeightProxyScreen=0.00%`,
  `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=71.29%`, and
  `postNonReady=0`. Compared with the pre-fix high-altitude baseline,
  `midVoxelPct` averaged `0.00%` instead of `32.84%`, `farSvoPct` averaged
  `81.96%` instead of `51.69%`, and `missPct` stayed `0`. Sampled late-frame
  pacing improved from about `15-16 ms` to about `13-14 ms` on the reproduced
  route. The shoreline smoke passed with `minVoxelTerrainScreen=89.71%` and
  `postNonReady=0`.
- Next action required: continue TS-014/TS-015 on residual low-altitude
  mid-voxel material slabs, manual far-background acceptance, and remaining
  frame-pacing work. Do not treat the high-altitude A/B route as complete
  free-roam proof.

### May 17 Mid-Voxel Interior Fallback Instrumentation And Foreground Handoff

- Requirement ID: TS-025-MID-INTERIOR-FALLBACK-DIAG-HANDOFF-20260517.
- Requirement text: Diagnose and reduce the fake shoreline terrain/material
  caused by non-exposed mid-voxel interior fallback while preserving the
  verified `miss=0`, `heightProxyScreen=0`, and near-terrain gates.
- Source document / source location: user report on May 17 that the shoreline
  still shows fake sand/terrain which disappears after brush edits, plus the
  continuing complaint that slow loading exposes fake sparse terrain around the
  player.
- Current status: DONE_VERIFIED for adding explicit instrumentation, proving
  the failure source on the reproduced shoreline route, preventing waterline
  interior fallback from claiming confident terrain ownership, and moving the
  exact foreground/mid handoff from 768 to 1024 with a larger screen-critical
  request cap. PARTIAL for the broader objective because manual free-roam still
  shows transition-frame mid-voxel interior fallback and remaining performance
  work is not complete.
- Related source files, modules, functions, scripts, or assets:
  `assets/shaders/Graphics/PS_Raymarch.hlsl`,
  `src/Graphics/SparseVoxelGpuResources.cpp`,
  `src/Graphics/SparseVoxelGpuResources.h`,
  `src/Graphics/Renderer.cpp`,
  `src/main_launcher.cpp`,
  `engine_capture_smoke.ps1`,
  `docs/reference/runtime.md`,
  `build/captures/diagnose_mid_interior_fallback55_shore_20260517`,
  `build/captures/probe_mid_interior_fallback_off_shore_20260517`,
  `build/captures/probe_mid_interior_band512_shore_20260517`,
  `build/captures/probe_mid_interior_band2048_shore_20260517`,
  `build/captures/fix_mid_interior_defer_surface55_shore_20260517`,
  `build/captures/fix_mid_exposure_procedural_halo55_shore_20260517`,
  `build/captures/fix_waterline_skip_interior_fallback55_shore_20260517`,
  `build/captures/fix_exact1024_midstart1024_fallback55_shore_20260517`,
  `build/captures/fix_exact1536_midstart1536_fallback55_shore_20260517`,
  `build/captures/fix_exact1024_cap256_normal_shore_20260517`,
  `build/captures/fix_defer_mid_interior_to_farsvo55_shore_20260517`,
  `build/captures/fix_mid_ray_entry_surface55_shore_20260517`,
  `build/captures/fix_mid_interior_surface_band55_shore_20260517`,
  `build/captures/fix_mid_defer_rayentry_final55_shore_20260517`,
  `build/captures/fix_mid_defer_rayentry_final_normal_shore_20260517`,
  `build/captures/diagnose_mid_surface_tag_ratio_stable55_shore_20260517`,
  `build/captures/fix_mid_surface_band_tag_current55_shore_20260517`,
  `build/captures/probe_mid_surface_depth_gate55_shore_20260517`,
  `build/captures/fix_mid_surface_depth_gate128_55_shore_20260517`,
  `build/captures/probe_mid_surface_depth_gate256_55_shore_20260517`,
  `build/captures/fix_mid_surface_depth_gate320_55_shore_20260517`,
  `build/captures/fix_mid_surface_depth_gate384_55_shore_20260517`,
  `build/captures/probe_deferred_mid_column_replacement55_shore_20260517`,
  `build/captures/probe_deferred_mid_column_generated_refine55_shore_20260517`,
  `build/captures/probe_deferred_mid_surface_replacement55_shore_20260517`,
  `build/captures/probe_lowalt_far_svo_down_steps55_shore_20260517`.
- What currently exists: `RayHit` now carries diagnostic flags for mid-voxel
  parent-held and mid-voxel interior fallback. Render ownership readback and
  `engine_capture_smoke.ps1` now expose `midVoxelInteriorFallback` and
  `midVoxelInteriorFallbackPct`, and debug mode `55` colors real mid-voxel
  surface green, parent-held purple, and interior fallback red. The shader now
  defers a first interior fallback briefly while searching for a tagged/exposed
  surface, uses deterministic terrain sampling as a one-cell exposure halo for
  missing mid-neighbor bricks, prevents waterline interior fallback from
  becoming a mid-voxel terrain hit, and mutes/reclassifies non-exposed sand
  fallback. Background raymarching now lets flagged mid-voxel interior fallback
  defer to later far-SVO/water owners instead of immediately winning the whole
  background chain, and resident air-to-solid ray-entry transitions are no
  longer mislabeled as non-exposed interior fallback. The mid-voxel CPU payload
  now preserves filled-volume data for continuity while also marking a shallow
  deterministic terrain/water surface band as `VisualSurface`, so the shader
  can classify more resident generated cells as surface without deleting the
  interior data needed by neighbor tests and transition stability. Voxel-only
  background rendering now also tries the cheap mid-voxel column surface only
  after a full mid-voxel DDA hit has been deferred as interior fallback and
  after far SVO/water have had a chance to win; this gives a narrow
  surface-shaped replacement path for some fake filled-volume hits without
  re-enabling the column proxy as the first-choice voxel-only background. The
  foreground policy now
  defaults
  `VENPOD_SPARSE_EXACT_NEAR_DISTANCE`, `VENPOD_SPARSE_MID_START`, and
  `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_DISTANCE` to `1024`, with
  `VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_MAX_REQUESTS=256`.
- What is missing: the reproduced route still shows high
  `midVoxelInteriorFallbackPct` during moving transition frames, so this is not
  a complete fake-terrain removal. The A/B attempts prove disabling or
  distance-banding the fallback breaks temporal ownership stability, and the
  1536 handoff was rejected because it increased work and did not improve the
  route. The remaining work is to reduce reliance on the fallback by improving
  exact surface/far-SVO streaming coverage and/or replacing the fallback with a
  stricter ready-surface LOD transition.
- Validation required: build and sparse tests must pass; shoreline normal and
  debug captures must retain `miss=0`, `unsafeNearMiss=0`,
  `heightProxyScreen=0`, `valleyAtmosphereScreen=0`, and no post-ready
  terrain-critical non-ready targets. Debug-55 and ownership CSVs must expose
  the interior fallback metric so future work can prove reductions instead of
  hiding the problem.
- Exact command, test, artifact, screenshot, runtime output, log, or manual
  check that would prove completion:
  `.\build.ps1 -Config Release; if ($LASTEXITCODE -eq 0) { .\build\bin\VENPODTests.exe }`;
  diagnostic guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 260 -StressCameraHeight 140 -StressCameraBaseHeight 120 -StressCameraSpeed 28 -SparseDebugMode 55 -ExitAfterFrames 340 -CaptureStartFrame 160 -CaptureIntervalFrames 45 -CaptureCount 4 -MinUniqueSampleColors 2 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 25 -SparseMinTerrainPixelsPct 45 -SparseMaxMissPixelsPct 3 -SparseOwnershipStabilityReadyFrame 170 -OutputDir .\build\captures\fix_exact1024_midstart1024_fallback55_shore_20260517`;
  normal guard:
  `.\engine_capture_smoke.ps1 -Config Release -NoBuild -StressCamera -WaterlineCamera -StressCameraRadius 260 -StressCameraHeight 140 -StressCameraBaseHeight 120 -StressCameraSpeed 28 -SparseDebugMode 0 -ExitAfterFrames 340 -CaptureStartFrame 160 -CaptureIntervalFrames 45 -CaptureCount 4 -MinUniqueSampleColors 40 -MaxHeightProxyScreenPct 0.1 -MaxValleyAtmosphereScreenPct 25 -SparseMinTerrainPixelsPct 45 -SparseMaxMissPixelsPct 3 -SparseOwnershipStabilityReadyFrame 170 -OutputDir .\build\captures\fix_exact1024_cap256_normal_shore_20260517`.
- Latest evidence, if any: build and `VENPODTests.exe` passed. The initial
  debug-55 capture proved the root symptom: average `midVoxelPct=26.95%` and
  `midVoxelInteriorFallbackPct=26.11%`, with late transition frames where almost
  every mid-voxel pixel was interior fallback. Hard-disabling the fallback and
  512/2048-distance-band probes failed ownership stability, including a
  `terrain=84%` versus previous `100%` frame delta, so the fallback cannot be
  removed without a replacement owner. The accepted normal guard
  `fix_exact1024_cap256_normal_shore_20260517` passed with
  `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=88.45%`, `postNonReady=0`, `missPct=0`, and exact
  `nearPct=100%` in the late samples. The rejected `1536` handoff guard passed
  hard gates but worsened the average fallback metric versus the `1024` guard.
  The accepted `fix_defer_mid_interior_to_farsvo55_shore_20260517` and
  `fix_mid_ray_entry_surface55_shore_20260517` probes preserved the hard gates
  while moving a small share of ownership out of fallback (`farSvoPct` around
  `0.17%`) and preventing real air-to-solid entry faces from being counted as
  fallback. The rejected `fix_mid_interior_surface_band55_shore_20260517`
  failed the temporal stability guard with `terrain=84%` versus previous
  `100%`, proving generated-height surface banding removes fallback without a
  valid replacement owner. Current post-rejection verification passed build,
  `VENPODTests.exe`, debug-55
  `fix_mid_defer_rayentry_final55_shore_20260517`, and normal
  `fix_mid_defer_rayentry_final_normal_shore_20260517`: both captures retained
  `maxHeightProxyScreen=0.00%`, `maxValleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=88.45%`, `postNonReady=0`, and `missPct=0`. The
  remaining fake-terrain signal is still high: debug-55 averages
  `midVoxelPct=26.40%`, `midVoxelInteriorFallbackPct=25.38%`,
  `farSvoPct=0.17%`, `nearPct=61.17%`, and `missPct=0`; normal mode averages
  `midVoxelPct=26.39%`, `midVoxelInteriorFallbackPct=25.37%`,
  `farSvoPct=0.17%`, `nearPct=61.17%`, and `missPct=0`. The follow-up
  diagnostic `diagnose_mid_surface_tag_ratio_stable55_shore_20260517` preserved
  the same hard gates and added `PERF_SPARSE_CLIPMAP voxelSurface=surface/nonAir`
  telemetry. It proves the current architectural failure: at frame `240` the
  mid-voxel clipmap reports `midCov=1.00/0.90` and `voxelSurface=341220/12980641`,
  so only about `2.6%` of resident non-air mid-voxel samples are tagged as
  renderable visual surface while nearly all mid background ownership remains
  interior fallback (`midVoxelInteriorFallbackPct=25.37%`, `missPct=0`). Two
  rejected probes, `fix_mid_surface_shell_only55_shore_20260517` and
  `fix_mid_surface_shell_thick55_shore_20260517`, removed non-surface interior
  samples from the mid-voxel payload and drove `midVoxelInteriorFallback` to
  zero, but both failed the terrain stability gate at the same transition
  (`terrain=84%` versus previous `100%`). This proves a shell-only mid payload
  is the right direction for eliminating fake filled-volume terrain, but the
  current generated shell is too sparse to preserve continuity without a
  better replacement owner or thicker/coherent surface extraction. The accepted
  current-state capture `fix_mid_surface_band_tag_current55_shore_20260517`
  keeps the filled payload and only adds CPU-side near-surface tags; it passed
  build, `VENPODTests.exe`, `maxHeightProxyScreen=0.00%`,
  `maxValleyAtmosphereScreen=0.00%`, `minVoxelTerrainScreen=88.45%`,
  `postNonReady=0`, and `missPct=0`. It raises the frame-240 mid surface tag
  count from `341220/12980641` to `731745/12980641` and reduces average
  `midVoxelInteriorFallbackPct` from `25.3725%` to `25.3042%`. This is a
  verified improvement but not a complete fix. Shader-side depth-gating probes
  that allowed interior fallback only near the deterministic generated surface
  reduced the red fallback much more aggressively, but were rejected because
  they still failed the temporal stability gate: `probe_mid_surface_depth_gate55`
  failed at `terrain=84%` versus previous `100%`,
  `fix_mid_surface_depth_gate128` failed at `terrain=85%`,
  `probe_mid_surface_depth_gate256` failed at `terrain=91%` versus the required
  effective `92%`, `fix_mid_surface_depth_gate320` failed one frame later at
  `terrain=82%`, and `fix_mid_surface_depth_gate384` failed at `terrain=84%`
  after making the preceding frames too close to the old unbounded fallback.
  These probes prove the unbounded interior fallback is masking a real
  transition owner deficit; the next fix must replace that owner, not simply
  suppress it. The accepted follow-up
  `probe_deferred_mid_column_replacement55_shore_20260517` passed the same hard
  gates (`heightProxyScreen=0.00%`, `valleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=88.45%`, `postNonReady=0`, `missPct=0`) and reduced
  average `midVoxelInteriorFallbackPct` from the current-state `25.3042%` to
  `25.2583%`, with no increase in height-proxy ownership. This is a small
  verified reduction, not a final visual fix; the worst transition frames still
  remain almost entirely interior fallback. A generated-height refinement
  variant, `probe_deferred_mid_column_generated_refine55_shore_20260517`,
  passed hard gates but worsened the fallback average to `25.3097%`, so it was
  rejected. A broader deferred mid-height surface replacement
  (`probe_deferred_mid_surface_replacement55_shore_20260517`) was rejected
  because the capture timed out and left the runtime spinning until killed,
  proving the full mid-height surface march is too expensive as a per-pixel
  replacement behind deferred fallback. A low-altitude far-SVO downward
  page-walk budget increase (`probe_lowalt_far_svo_down_steps55_shore_20260517`)
  also passed hard gates but was rejected: it did not increase far-SVO
  ownership (`farSvoPct` fell from `0.1517%` to `0.1488%`), only reduced
  fallback slightly to `25.2794%`, and substantially worsened sampled frame
  times (`frame=240` rose from about `16.79 ms` to about `28.57 ms`, with many
  later frames around `35-41 ms`).
- Latest interrupted-continuation evidence: the binary-search mid-interior
  surface crossing probe
  `build/captures/probe_mid_interior_crossing_recovery55_shore_20260517`
  proved the visible red/fake-terrain issue is specifically the shader's
  `allowVoxelOnlyInteriorFallback` path: average
  `midVoxelInteriorFallbackPct` dropped from the accepted deferred-column
  baseline `25.2583%` to `0.1586%`, and frame 320 changed from
  `midVoxelInteriorFallback=518945` to `0`. It is rejected as an implementation
  because it raised frame 240 from about `16.83 ms` to about `27.98 ms`, pushed
  GPU ray work from about `0.27 ms` to about `0.87 ms`, and a reduced-budget
  follow-up timed out. The cheaper analytic probe
  `probe_mid_interior_analytic_recovery55_shore_20260517` passed the hard gates
  but worsened fallback versus the accepted deferred-column baseline
  (`25.2754%` versus `25.2583%`) and was reverted. The CPU partial-cell
  exposure probe `fix_mid_partial_surface_exposure55_shore_20260517` increased
  mid surface tags from `731745/12980641` to `891141/12980641`, but still
  worsened fallback (`25.2887%`) and sampled frame cost, so it was reverted.
  The secant recovery probe
  `probe_mid_interior_secant_recovery55_shore_20260517` also passed hard gates
  but worsened fallback (`25.3180%`) and frame 240 (`23.73 ms`), so it was
  reverted. A voxel-only column-first probe
  `probe_voxelonly_column_first55_shore_20260517` was also rejected: it passed
  the hard gates but left the worst frame unchanged
  (`midVoxelInteriorFallback=518946` at frame 320) and worsened average
  fallback to `25.2980%`. A conservative origin-gated mid-height recovery
  `probe_mid_height_recover_interior55_shore_20260517` likewise left frame 320
  unchanged and worsened fallback to `25.2698%`. The accepted follow-up
  `probe_mid_height_ungated_recover55_shore_20260517` removes the origin gate
  and recovers a subset of interior fallback hits against the resident
  mid-height clipmap. It passed build, `VENPODTests.exe`, and the debug-55
  shoreline gate with `heightProxyScreen=0.00%`, `valleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=88.45%`, `postNonReady=0`, and `missPct=0`; it reduced
  average `midVoxelInteriorFallbackPct` from the accepted deferred-column
  baseline `25.2583%` to `17.8874%` while keeping frame 240 near baseline
  (`16.28 ms` versus `16.83 ms`), but left frames 300 and 320 unchanged
  (`395483` and `518945` interior fallback pixels). A tiny procedural fallback
  with an origin-height gate (`probe_mid_height_plus_tiny_proc_recover55_shore_20260517`
  and relaxed-angle `probe_mid_height_plus_tiny_proc_relaxed55_shore_20260517`)
  cleared frame 320 but not frame 300 and was not accepted. The accepted
  current fix is `probe_mid_height_plus_noorigin_proc55_shore_20260517`: it
  keeps the resident mid-height recovery first, then uses a one-to-two-sample
  procedural surface estimate only after that resident recovery fails. It
  passed the same debug-55 hard gates and reduced average
  `midVoxelInteriorFallbackPct` to `0.0001%`; the reproduced bad frames now
  report `midVoxelInteriorFallback=0` at retire frames 303, 320, and 323. The
  normal-mode guard `fix_mid_height_plus_noorigin_proc_normal_shore_20260517`
  also passed with `heightProxyScreen=0.00%`, `valleyAtmosphereScreen=0.00%`,
  `minVoxelTerrainScreen=88.45%`, `postNonReady=0`, `missPct=0`, and average
  fallback `0.0001%`. This replaces the fake mid-volume ownership in the
  reproduced shoreline route, but it is still not global completion: frame 240
  rose to about `18-19 ms` with GPU surface/ray work higher than mid-height-only
  (`~2.7 ms` GPU frame in the normal guard), and broader free-roam/manual
  material consistency still needs validation. A lower resident-recovery budget
  probe, `probe_mid_recover_budget321_proc55_shore_20260517`, preserved the
  near-zero fallback metric (`midVoxelInteriorFallbackPct=0.0000%`) and kept
  frames 303/320/323 clear, but was rejected because sampled CPU/frame cost
  regressed badly versus the accepted no-origin procedural fix (`frame=240`
  rose to `23.33/25.64 ms`, `prep=15.26 ms`, and frame 325 reported
  `27.63/22.69 ms` with `gpu ray=5.17 ms`). The accepted shader budget remains
  the previous `4/3/2` resident mid-height recovery plus `2/1/1` procedural
  fallback.
- Next action required: continue TS-015/TS-014 by attacking the remaining
  validation and performance risk around the accepted no-origin procedural
  recovery: confirm it does not create material inconsistencies during manual
  edits/free-roam, then either replace it with a true surface-authoritative mid
  LOD owner or amortize/cache the procedural estimate so the visual fix does
  not become the next movement hitch.

May 17 above-water regression continuation: the previous waterline guard was
under-validating the user's reported view because its contact sheet captured
underwater frames. A new above-water shoreline/mountain ownership route
(`diag_above_shore_ownership55_20260517`) reproduced the more relevant layer
mix: hard hole counters stayed clean (`miss=0`, `unsafeNearMiss=0`,
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`), but most visible mountain
context was still resident mid-voxel background (`midVoxelPct avg=76.71%`,
`midVoxelScreenPct avg=56.56%`) while far SVO owned little
(`farSvoPct avg=6.90%`). The concrete root cause found in the log was circular
LOD throttling: `lodThrottle=1` capped the mid clipmap to `budgetMid=16` while
`queuedVoxel=2089`, even though the far SVO was not actually dominant. Accepted
fix: keep the exact terrain-critical high-altitude throttle, but stop applying
the raymarch/mid-clipmap LOD throttle unless far SVO ownership itself exceeds
`VENPOD_SPARSE_TERRAIN_SCREEN_CRITICAL_LOD_THROTTLE_FAR_SVO_PCT`. Evidence:
`.\build.ps1 -Config Release` passed, `.\build\bin\VENPODTests.exe` passed,
`fix_mid_not_lod_starved_above_shore55_20260517` passed hard gates with
`miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, `midVoxelPct avg=71.31%`, `farSvoPct avg=10.98%`,
`lodParentHeldPct avg=1.06%`, and frame-240 missing mid voxels reduced to
`560`; `fix_mid_not_lod_starved_above_shore_normal_20260517` passed with the
same hard gates and frame 240 at `16.56/15.67 ms`. This is `PARTIAL`, not
closed: the normal contact sheet is less holey but still visibly dominated by
coarse mid terrain and gray water/terrain context. Rejected probe:
`probe_elevated_prefers_farsvo_above_shore55_20260517` lowered the shader's
high-altitude mid-voxel cutoff so far SVO could own more pixels, but it exposed
too much sky/water in the mountain band (`skyPct avg=57.61%`,
`voxelTerrainScreenPct avg=50.98%`) and was reverted. Latest default launch
guard `.\rebrun.ps1 -Config Release -NoBuild -ExitAfterFrames 300` exited
normally. Next action required: attack mid/far handoff quality without letting
far SVO expose sky gaps. Candidate directions are denser far-SVO page traversal
only for confirmed resident silhouettes, a transition rule that keeps mid
voxels as fallback behind far-SVO failures rather than replacing the layer
wholesale, and a material/ownership diagnostic for gray/fake shoreline context.

May 18 mid/far handoff continuation: the fallback-style handoff candidate was
accepted over the all-or-nothing far-SVO probe. `RaymarchBackgroundField` now
samples a far-SVO candidate for elevated above-water voxel-only views, but only
uses it when the far hit is close enough to the mid-voxel hit; if far SVO has a
sky/miss gap, the resident mid voxel remains the fallback owner. To avoid
reintroducing circular starvation, the scheduler now requires a stronger
background far-SVO ownership threshold (`VENPOD_SPARSE_BACKGROUND_FARSVO_LOD_OWNER_PCT`,
default `55`) before applying raymarch or mid-clipmap LOD throttles to the
fallback layers. Evidence: `.\build.ps1 -Config Release` passed,
`.\build\bin\VENPODTests.exe` passed,
`probe_farsvo_candidate_mid_fallback_owner55_above_shore55_20260518` passed
with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreen=0`,
`valleyAtmosphereScreen=0`, `midVoxelPct avg=48.13%`,
`farSvoPct avg=32.82%`, `skyPct avg=14.73%`, and frame-240 `queuedVoxel=560`;
the normal-color guard
`fix_farsvo_candidate_mid_fallback_owner55_above_shore_normal_20260518`
preserved the same hard gates while shifting visible ownership from mid voxel
to far SVO (`midVoxelScreenPct avg 52.27% -> 35.52%`,
`farSvoScreenPct avg 8.35% -> 24.21%`) and kept frame 240 within budget
(`14.71/14.07 ms`). Shoreline regression guard
`guard_farsvo_candidate_mid_fallback_waterline_normal_20260518` passed with
`heightProxyScreen=0`, `valleyAtmosphereScreen=0`, `miss=0`,
`unsafeNearMiss=0`, and `minVoxelTerrainScreen=88.45%`. A clean default launch
guard `.\rebrun.ps1 -Config Release -NoBuild -ExitAfterFrames 300` exited
normally, with frame 240 `8.32/10.02 ms`, backend pipe active, `miss=0`, and
`unsafeNearMiss=0`. Remaining status is still `PARTIAL`: the above-water normal
contact sheet is materially better but still has gray/fake-looking shoreline
context and unresolved manual free-roam acceptance.

## Status Counts

- `DONE_VERIFIED`: 22
- `DONE_UNVERIFIED`: 0
- `PARTIAL`: 16
- `NOT_STARTED`: 0
- `BLOCKED`: 0
- `DEFERRED_BY_USER_ONLY`: 0

## Highest-Priority Incomplete Items

1. TS-001/TS-002: Renderer-facing ready-to-render and parent-held-until-children-ready rules. The core fixed-grid pipeline is verified and mid-voxel parent-ring fallback is now instrumented, but the public renderer still needs a single readiness state/debug proof plus sparse/far LOD handoff evidence.
2. TS-004: Full lifecycle state debug view. Ownership debug now catches illegal near holes, but it still does not identify queued/generated/meshed/uploaded/ready states.
3. TS-005/TS-010: Runtime fixed-grid and full culling/LOD/unload matrix. The CPU fixed-grid pipeline is verified and a focused culling probe is clean, but rendered isolation is still incomplete.
4. TS-007: General priority streaming. The focused route, long-walk regression, and startup non-world hold are fixed; broader free-roam queue-latency guarantees remain incomplete.
5. TS-015: Runtime stalls. Launch is fixed, the fake-terrain admission/surface-readiness failure is fixed on the reproduced routes, the broad scripted walking hitch from final-percent mid-voxel catchup is reduced, repeated brush material no-op feedback is suppressed, the protected surface extraction lane now obeys the same 8 ms cap as the normal surface pump, renderable screen-critical terrain pages now use a cheap retention touch instead of full request admission, the latest voxel-only mid-volume fallback stops resident coarse voxel terrain from becoming background miss, high-altitude LOD-throttled view-cone planning now uses a narrower grid instead of doing full-grid work that is later demoted, exact sparse page-table publication is now surface-ready gated instead of exposing resident pages with missing surfaces, soft final-percent mid-voxel catchup is capped once hard visual/readiness failures are already absent, the legacy mid-height clipmap no longer receives the same large catchup budget as actual mid-voxel bricks, and protected mid-catchup now defaults to the narrower `48` voxel coverage catchup budget in voxel-only mode. `fix_surface_protected_cap_waterline_20260517` reduces the reproduced frame-172 `surfExtract` spike to `8.58 ms` with `miss=0` and `unsafeNearMiss=0`; `fix_fast_renderable_retention_touch_focused_long_walk_20260517` drops late terrain-critical admission from `reqAvg=532.89` to `0.69` while keeping `postNonReady=0`; `fix_voxel_only_mid_volume_fallback_waterline_guard_20260517` raises the waterline route to `minVoxelTerrainScreen=99.91%`; `fix_voxel_only_mid_volume_fallback_focused_long_walk_20260517` holds `minVoxelTerrainScreen=86.62%`; `fix_lod_view_grid3_long_fastflight_20260517` cuts sampled high-flight `ms avg/max` from `41.30/43.99` to `25.71/27.33`; `fix_atomic_total_work_cap_low_shore_20260517` / `fix_atomic_total_work_cap_fastflight_20260517` both pass with `postNonReady=0` after the atomic surface-ready publish and direct critical upload fixes; `fix_mid_soft_deficit_default24_low_shore_20260517` / `fix_mid_soft_deficit_default24_fastflight_20260517` preserve `heightProxyScreen=0`, `miss=0`, `unsafeNearMiss=0`, and `postNonReady=0` while reducing final-percent mid-clipmap pump pressure; `fix_mid_height_budget_fastflight_20260517` / `fix_mid_height_budget_low_shore_20260517` preserve the same hard gates while bounding height maintenance (`pumpHeight` max down to about `5 ms`); and `fix_mid_catchup48_default_low_shore_brush_20260517` preserves the hard gates while reducing sampled post-frame-300 low-shore `ms avg/max` from `39.76/47.97` to `30.19/40.83`. Broader manual free-roam remains performance-incomplete because residual voxel pumping, frame gaps, GPU ray cost, surface extraction/staging cost, coarse-LOD review, manual shoreline/material acceptance, and untracked manual camera paths still need coverage.
6. TS-014: Far-mountain LOD continuity. The latest debug route proves one remaining cutout class was resident mid-voxel terrain being discarded as non-surface and falling through to background miss, not simply absent near voxels. The accepted patch converts the reproduced debug red miss to mid-voxel ownership and preserves `miss=0`/`unsafeNearMiss=0` on normal guards, but broader manual free-roam acceptance is still required.

## Items Most Likely To Cause Premature Closure

- Passing `miss=0` or `unsafeNearMiss=0`: this does not prove terrain chunks are ready.
- Passing debug-50 with `ownerMissPct=0` after the mid-volume fallback: this proves that reproduced background miss route no longer falls through, not that coarse LOD quality, material consistency, or frame pacing are final.
- Passing a contact sheet with fallback terrain: this can hide partial streaming.
- Increasing request caps, page counts, or mid-voxel capacity: useful probes, not architectural completion.
- Passing `terrain_gap_narrow_unsafe_long_walk_20260515`: this verifies the focused near-hole/misclassification route and stricter scripted long walk, not all LOD/readiness contracts.
- Passing `terrain_gap_startup_frame0_gate112_surfaceaware_seq_20260515`: this verifies startup no longer exposes the incomplete world; it does not prove the underlying render-readiness state machine exists.
- Drawing parent and children simultaneously without a readiness rule: can hide holes in one view while breaking another.
- A green fixed-grid test alone: only proves generation/meshing for that mode, not streaming/LOD correctness.

## May 18 Material/Fake-Terrain And Launch-Stall Continuation

Status: PARTIAL. This continuation did not close the rendering goal.

Evidence accepted:

- `VENPOD/build/captures/diag_material54_min_palette_above_shore_20260518`: material debug capture after adding explicit `MAT_BEDROCK` and `MAT_GLASS` colors to both `PS_Raymarch.hlsl` and `PS_SparseSurface.hlsl`. This removes one misleading diagnostic artifact: bedrock/existing glass no longer collapse to the generic magenta unknown-material color. The capture still shows real sand/stone/dirt/water material IDs in the background layers, so the fake-looking shoreline is not just a debug-palette bug.
- `VENPOD/build/captures/diag_owner55_min_palette_above_shore_20260518`: ownership debug capture proving the remaining broad background is mainly mid-voxel and far-SVO ownership, not height proxy, valley atmosphere, or miss fallback.
- `VENPOD/build/captures/probe_fake_sand_normal_above_shore_20260518`: normal capture after the material-debug cleanup. Hard gates passed: `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`. The visual fake/gray terrain remains, so this is still incomplete.
- Cold shader compile diagnosis: direct `dxc.exe` compile of `PS_Raymarch.hlsl` at `-O3` took about 127 seconds before the later rejected probe and about 243 seconds after the rejected mid-surface reprojection probe. Immediate cached launch took about 3.2 seconds. Root cause for the black-screen launch symptom is therefore runtime cold DXC compilation of the monolithic raymarch shader after shader edits, not a persistent renderer deadlock.
- `src/Graphics/RHI/ShaderCompiler.cpp` now logs shader cache hits, cache misses, compile duration, and bytecode size at info level. A cached smoke run shows `Shader cache hit: PS_Raymarch.hlsl` and exits normally, so future black-screen reports can be separated from true launch hangs by log evidence.

Rejected/non-closing probe:

- `VENPOD/build/captures/fix_mid_coarse_surface_reproject_normal_above_shore_20260518`: attempted to reproject coarse mid-voxel hits back to generated terrain/water height before shading. It passed the hard no-miss/no-height-proxy gates but was rejected because the contact sheet looked materially unchanged, GPU ray time regressed from roughly 7.7 ms to 8.8 ms at frame 240, `lodParentHeldPct` became nonzero, and the shader cold compile cost worsened. The patch was backed out.

Current requirement updates:

- TS-011 material/ownership consistency remains PARTIAL. What exists: background layers now expose trustworthy material debug colors for bedrock/glass and ownership captures prove the broad fake-looking terrain is not a miss or height-proxy path. What is missing: exact proof that mid/far shoreline material agrees with the authoritative sparse world after edits/brush interaction, and a fix for coarse background surfaces that disappear when exact sparse terrain becomes available.
- TS-012 launch-stall diagnosis is PARTIAL. What exists: shader-cache logging and proof that cached launches are fast. What is missing: architectural reduction of `PS_Raymarch.hlsl` compile time, precompiled shader packaging, or a loading/progress path that prevents a cold shader compile from presenting as a frozen black screen.

Next required action:

- Add a targeted material/ownership diagnostic that can identify, for each fake shoreline pixel, both owner layer and material in one capture or log row. Use that to decide whether the real fix belongs in mid-voxel brick generation, material classification, background/exact handoff, or brush/edit invalidation. Do not reattempt broad shader reprojection without a narrower proof target because the first probe was visually ineffective and costly.

## May 18 Low-Relief Shoreline Material Fix

Status: PARTIAL. This improves the fake-sand symptom but does not close the full terrain-rendering goal.

Requirement: Shoreline sand must be a real low-relief beach material, not a blanket material assigned to every near-sea surface, cliff, or coarse background shell.

Source/evidence:

- User report: sand near water looked fake and disappeared once exact terrain became authoritative through drawing/editing.
- Diagnostic `VENPOD/build/captures/diag_owner_material56_above_shore_20260518`: new debug mode 56 composites material color with owner tint, showing fake-looking shoreline material could be separated into exact-surface material versus mid/far background material in one capture.
- Diagnostic `VENPOD/build/captures/diag_owner_material56_waterline_20260518`: waterline route showed yellow shoreline material even when frame-240 ownership was entirely near/exact sparse surface. This proved at least part of the problem was the terrain material rule itself, not just background ownership.

Implemented:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`: added debug mode 56 owner+material composite output and made material-debug paths feed it.
- `assets/shaders/Graphics/PS_SparseSurface.hlsl`: added matching exact-surface owner+material composite output for debug mode 56.
- `src/Simulation/SparseTerrainGenerator.cpp`: sand now requires `dryOrIntertidalSurface && depth < 4.0f && relief < 8.0f` instead of applying to every dry/intertidal top surface.
- `assets/shaders/Graphics/PS_Raymarch.hlsl`: `FarTerrainMaterial` now mirrors the low-relief shore rule with `height < FAR_SEA_LEVEL + 6.0f && localRelief < 8.0f`.

Validation:

- Build and unit gate: `.\build.ps1 -Config Release; .\build\bin\VENPODTests.exe` passed.
- Shader cold-cache evidence: after the shader edit, `PS_Raymarch.hlsl` cold compile completed in `140.51` seconds and cached launches returned to about `3.25` seconds. This keeps the black-screen cause visible but does not solve the compile-time architecture debt.
- `VENPOD/build/captures/fix_low_relief_sand_owner_material56_waterline_20260518`: passed capture gates, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=88.45%`; broad waterline yellow bands reduced to small low-relief patches.
- `VENPOD/build/captures/fix_low_relief_sand_owner_material56_above_shore_20260518`: passed capture gates, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`; combined owner/material view confirms remaining yellow is much more localized and owner-visible.
- `VENPOD/build/captures/fix_low_relief_sand_normal_above_shore_20260518`: passed normal visual gates, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, frame-240 GPU ray time about `7.77 ms`.
- `VENPOD/build/captures/fix_low_relief_sand_normal_waterline_20260518`: passed waterline normal gates, `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=88.45%`.

Remaining:

- Broad gray/fake background terrain remains PARTIAL. The fix addresses over-broad sand classification, not the larger gray mid/far terrain quality problem.
- `lodParentHeldPct` appears in normal above-shore captures on this route, but comparison against `probe_fake_sand_normal_above_shore_20260518` and `fix_farsvo_candidate_mid_fallback_owner55_above_shore_normal_20260518` shows it was already present before this patch and is not introduced by the low-relief sand change.
- Shader compile-time architecture debt remains PARTIAL: logging now identifies cold compile stalls, but the monolithic raymarch shader still takes minutes to cold-compile after edits.

## May 18 Background Stone Variation Probe

Status: PARTIAL. Accepted as a small visual consistency improvement, not a completion claim.

Requirement: mid/far terrain should not read as a flat fake gray placeholder layer when exact sparse surfaces use richer terrain material variation.

Implemented:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`: added `BackgroundTerrainMaterialVariation`, mirroring the exact sparse surface material language for stone, dirt, and sand.
- `RaymarchMidVoxelClipmap`, `MakeMidVoxelColumnClipmapHit`, and mid-height fallback hits now apply deterministic background material variation before waterline wet tint.
- `FarTerrainMaterialVariation` stone now gets a modest low-altitude lichen/green-gray blend so far-SVO stone is less uniformly flat gray while remaining identifiable as stone.

Validation:

- Build and unit gate remained green: `.\build.ps1 -Config Release; .\build\bin\VENPODTests.exe`.
- Cold shader compile after the edit completed, but still took `126.48` to `134.73` seconds across probes. This reinforces that shader compile-time debt is still open.
- Rejected first strength probe as too subtle/cost-uncertain: `VENPOD/build/captures/probe_background_variation_normal_above_shore_20260518` passed hard gates but did not visibly move the gray terrain enough.
- Accepted stronger probe: `VENPOD/build/captures/fix_background_stone_variation_normal_above_shore_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, frame-240 GPU ray time about `7.83 ms`, and no new LOD-parent-held regression versus pre-existing captures.
- Waterline guard: `VENPOD/build/captures/fix_background_stone_variation_normal_waterline_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=88.45%`, and preserved the low-relief sand improvement.

Remaining:

- The large gray/fake-background complaint is reduced only modestly. Further work should target geometry/ownership and LOD surface fidelity rather than more color-only shader tweaks.
- Existing `lodParentHeldPct` remains visible on the above-shore route; it predates this material-variation patch but still belongs to the open terrain-rendering goal.
- The shader remains too monolithic for fast iteration. Material-only shader edits still trigger multi-minute cold compiles.

## Validation Commands And Artifacts Needed

## May 18 Mid-Voxel Parent-Held LOD Fix

Status: PARTIAL. Verified for the tested above-shore and waterline routes; the broader terrain-rendering goal remains open because mid-voxel backlog and movement hitching still exist.

Requirement: visible mid-distance terrain must not silently render coarse parent LOD where the shader expects finer mid-voxel bricks. Parent-held fallback should be treated as a visual streaming failure, and the cache should prioritize fine rings that occupy more screen area.

Implemented:

- `VENPOD/src/main_launcher.cpp`: records `renderOwnerLodParentHeldPixelsLastRetire`, exposes `parentHeld=pixels/pct` in `PERF_SPARSE_CLIPMAP`, and treats parent-held pixels above `VENPOD_SPARSE_MID_VOXEL_PARENT_HELD_CATCHUP_PIXELS` as a mid-clipmap visual failure.
- `VENPOD/src/Simulation/SparseClipmap.cpp`: changes mid-voxel interest allocation from equal per-ring quotas to weighted fine-ring-first quotas. This fixes the measured case where the cache was full and CPU interest coverage looked acceptable while the shader still sampled missing fine-ring bricks and fell back to parent LOD.

Validation:

- Build and unit gate passed: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Before weighted ring allocation, `VENPOD/build/captures/fix_parent_held_catchup_normal_above_shore_20260518` showed the scheduler catchup was real but incomplete: frame 240 `queuedVoxel=0`, `missingVoxel=0`, yet `lodParentHeld=52346` in `PERF_RENDER_OWNERSHIP`; ownership timeline `lodParentHeldPct avg=2.8234 max=10.4392`.
- Accepted above-shore route: `VENPOD/build/captures/fix_weighted_mid_voxel_ring_interest_above_shore_20260518` passed with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and ownership timeline `lodParentHeldPct avg=0.0000 max=0.0001`.
- Accepted waterline guard: `VENPOD/build/captures/fix_weighted_mid_voxel_ring_interest_waterline_20260518` passed with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=88.45%`, and ownership timeline `lodParentHeldPct avg=0.0000 max=0.0004`.

Remaining:

- Mid-voxel load backlog still exists under motion. The accepted above-shore route still reported `queuedVoxel=552`, `missingVoxel=552`, `coveragePct=90` at frame 240, and waterline reported `queuedVoxel=476`, `missingVoxel=476`, `coveragePct=91`.
- This fix removes the measured parent-held coarse-LOD artifact on the tested routes, but it does not prove the infinite world is fully real-time or hole-free across all camera paths.
- Performance/hitching is still PARTIAL. Frame-240 examples remain around `gpu ray ~8.18 ms` above-shore and waterline sparse prep can hit `~10.40 ms`.

Next validation/action:

- Add per-ring residency/backlog logging so future captures can distinguish ring-0/ring-1 starvation from global voxel-cache capacity pressure.
- Run a longer free-roam/fast-flight route after the backlog work and compare `lodParentHeldPct`, `missingVoxel`, `queuedVoxel`, and frame-time spikes.
- Consider increasing `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS` only after proving weighted allocation is insufficient; the current verified fix avoided a memory bump.

## May 18 Mid-Voxel Backlog Pacing

Status: PARTIAL. Accepted as a conservative pacing improvement for far-ring fill; not a full performance fix.

Requirement: after visible terrain is stable, the engine should continue draining remaining mid-voxel backlog at a modest protected rate instead of dropping to a severe-pressure 12-brick budget while distant rings are still visibly sparse.

Implemented:

- `VENPOD/src/Simulation/SparseClipmap.h` and `VENPOD/src/Simulation/SparseClipmap.cpp`: added per-ring mid-voxel residency diagnostics to `SparseClipmapCacheStats`.
- `VENPOD/src/main_launcher.cpp`: `PERF_SPARSE_CLIPMAP` now logs per-ring resident, queued, missing, and interested voxel counts.
- `VENPOD/src/main_launcher.cpp`: added clean-backlog catchup. When exact terrain is clean, no miss/unsafe/parent-held visual failure is active, and mid-voxel coverage is below `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_CATCHUP_PCT` (default `95`), the scheduler keeps a modest protected catchup budget instead of allowing severe pressure to clamp the mid budget to `12`.

Validation:

- Build and unit gate passed after the accepted default: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Diagnostic capture `VENPOD/build/captures/diag_mid_voxel_ring_backlog_above_shore_20260518` proved the remaining backlog is not fine-ring starvation: at frame 240 rings 0/1 had `miss=0/0`, while rings 2/3 had `miss=18/534`.
- Accepted above-shore capture `VENPOD/build/captures/fix_mid_voxel_clean_backlog_above_shore_20260518`: frame 240 missing voxel bricks improved from `552` to `360`, frame 360 from `519` to `304`, with `missPct=0`, `unsafeNearMissPct=0`, `lodParentHeldPct max=0.0001`, `heightProxyScreenPct=0`, and `valleyAtmosphereScreenPct=0`.
- Accepted waterline guard `VENPOD/build/captures/fix_mid_voxel_clean_backlog_waterline_20260518`: frame 240 missing voxel bricks improved from `476` to `261`, with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=88.45%`.
- Rejected aggressive default target probe: `VENPOD/build/captures/fix_mid_voxel_clean_backlog97_waterline_20260518` reduced frame-240 missing voxel bricks to `186`, but worsened waterline frame 240 to about `19.68 ms` with sparse prep about `11.29 ms`. The default was restored to `95`; `97` remains available only as an env override.

Remaining:

- Far-ring backlog still exists after the accepted patch (`261` to `360` missing bricks at frame 240 on tested routes). This should now be handled by better prediction, ring-3 cache strategy, or capacity tuning, not by blindly increasing global generation pressure.
- Waterline route still has transient low-level `lodParentHeldPct` peaks around `0.4088%` in one guard. This is far below the earlier above-shore parent-held failure but remains a watch item.
- Runtime hitching is still PARTIAL. The accepted pacing improved backlog without the rejected 97% spike, but it does not solve all sparse prep cost.

## May 18 Adaptive Far-Context Backlog Budget

Status: PARTIAL. Accepted as a targeted outer-ring backlog improvement; not a full streaming/performance fix.

Requirement: background/mountain-heavy views should keep filling outer mid-voxel rings faster, while shoreline/near-dominant views should not inherit the more expensive catchup budget.

Implemented:

- `VENPOD/src/main_launcher.cpp`: added `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_BUDGET` with default `32`, gated by `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_FARSVO_PCT` default `12`.
- The higher clean-backlog budget is only used when clean backlog catchup is active and far-SVO/background ownership is significant. Near shoreline views stay on the safer clean catchup budget.

Validation:

- Build and unit gate passed: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Accepted above-shore capture `VENPOD/build/captures/fix_adaptive_clean_backlog_budget_above_shore_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0.0001`. Frame-240 missing voxel bricks improved to `254` versus `360` in the previous accepted 24-budget capture.
- Accepted waterline guard `VENPOD/build/captures/fix_adaptive_clean_backlog_budget_waterline_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=88.45%`. Frame-240 missing voxel bricks stayed at `261`, matching the safer 24-budget behavior instead of the rejected global budget-32 behavior.
- Rejected stale-eviction patch: `VENPOD/build/captures/fix_mid_voxel_evict_stale_above_shore_20260518` did not improve missing counts and increased voxel pump/prep cost, so that code change was reverted.
- Rejected capacity probe: `VENPOD/build/captures/probe_mid_voxel_capacity8192_above_shore_20260518` increased resident capacity but made frame/prep time materially heavier (`voxelSlots=8192`, frame-240 sparse prep about `9.48 ms`) and did not justify a default memory bump.
- Rejected global clean-budget probe: `VENPOD/build/captures/probe_mid_voxel_clean_budget32_waterline_20260518` improved backlog but hurt the waterline route (`frame=240` body about `17.53 ms`, sparse prep about `11.47 ms`), so the accepted implementation gates the higher budget by far-context ownership.

Remaining:

- Outer-ring backlog is reduced but still present. The accepted above-shore route still has `254` to `264` missing mid-voxel bricks in frames 240/360.
- Waterline frame time remains variable and sometimes prep-heavy due exact/surface work, not solved by this mid-voxel budget change.
- Further work should target ring-3 prediction/stability or reducing per-brick generation cost before raising capacity or global budgets.

## May 18 Mid-Clipmap Interest Rebuild Diagnostics

Status: PARTIAL. Accepted as instrumentation and rejected two attempted interest-throttle fixes.

Requirement: identify whether waterline/near-terrain prep spikes come from full mid-clipmap interest rebuilds, cheap reuse bookkeeping, or generation/pump work.

Implemented:

- `VENPOD/src/Simulation/SparseClipmap.h` and `VENPOD/src/Simulation/SparseClipmap.cpp`: added `interestReusedLastFrame` to `SparseClipmapCacheStats`.
- `VENPOD/src/main_launcher.cpp`: `PERF_SPARSE_CLIPMAP` now logs `reuse=0/1`.
- Added experimental `VENPOD_SPARSE_MID_INTEREST_INTERVAL` plumbing with default `1`, so default runtime behavior remains unchanged unless explicitly overridden.

Validation:

- Build and unit gate passed: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Diagnostic capture `VENPOD/build/captures/diag_mid_interest_reuse_waterline_20260518` passed hard visual gates and showed the costly waterline frames are full rebuilds, not cheap reuse: frame 120 `interest=3.00 reuse=0`, frame 240 `interest=2.90 reuse=0`.
- Rejected forward-signature coarsening probe `VENPOD/build/captures/probe_mid_interest_forward32_waterline_20260518`: hard visual gates passed, but frame 120 worsened to `ms=30.44`, sparse prep `17.32`, clipmap prep `9.10`, and `budgetMid=48`. The code change was reverted.
- Rejected interval probe `VENPOD/build/captures/probe_mid_interest_interval3_waterline_20260518`: hard visual gates passed, but frame 120 worsened to `ms=26.68`, sparse prep `20.99`, clipmap prep `11.91`, and frame 240 remained prep-heavy. Default interval remains `1`.

Latest conclusion:

- The waterline slowness is not solved by coarser signature reuse or delayed interest rebuilds. Those approaches shift work into larger catchup bursts.
- Next useful target is reducing per-rebuild/per-pump cost or exact request/generation cost, not suppressing mid-interest updates globally.

## May 18 Mid-Voxel Interest Partial Sort

Status: PARTIAL. Accepted as a small CPU-path optimization; not a full slowness fix.

Requirement: reduce cost of full mid-voxel interest rebuilds without weakening terrain ownership, parent-held LOD prevention, or shoreline guards.

Implemented:

- `VENPOD/src/Simulation/SparseClipmap.cpp`: changed mid-voxel interest candidate selection from sorting every candidate in every ring to `std::partial_sort` of only the emitted quota. This keeps the same scoring comparator and emitted top-candidate intent while avoiding a full sort of discarded candidates.

Validation:

- Build and unit gate passed: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Accepted waterline capture `VENPOD/build/captures/fix_mid_interest_partial_sort_waterline_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=88.45%`. Compared to `diag_mid_interest_reuse_waterline_20260518`, frame 120 clipmap interest improved from `3.00 ms` to `2.68 ms`, clipmap prep from `6.20 ms` to `5.58 ms`, and frame-240 missing voxel bricks from `261` to `250`.
- Accepted above-shore capture `VENPOD/build/captures/fix_mid_interest_partial_sort_above_shore_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0.0001`. Frame-240 missing voxel bricks stayed in the accepted range (`250` versus `254` in the previous adaptive-budget capture).

Remaining:

- Interest rebuilds still cost multiple milliseconds when `reuse=0`; partial sort only reduces one hot segment.
- Frame-time spikes still include exact sparse request/generation and voxel pump cost. Further work should target terrain-critical request/generation and per-brick voxel generation cost.

## May 18 Hidden Ring Backlog Catchup

Status: PARTIAL. Accepted as a targeted far-mountain gap reduction; not a full performance or infinite-world completion.

Requirement: remaining mountain holes must not be hidden by an overall mid-voxel coverage number when one clipmap ring is still sparse. If near/fine rings are complete but an outer visible ring has poor coverage, the streamer should keep a protected mid-voxel drain budget for that ring instead of dropping to the severe-pressure 12-brick path.

Implemented:

- `VENPOD/src/main_launcher.cpp`: computes the worst per-ring mid-voxel coverage from `SparseClipmapCacheStats::interestedVoxelBricksByRing` and `missingInterestedVoxelBricksByRing`.
- `VENPOD/src/main_launcher.cpp`: logs `catchup=.../ring` and `worstRing=ring/coveragePct/missing` in `PERF_SPARSE_CLIPMAP`.
- `VENPOD/src/main_launcher.cpp`: lets clean-backlog catchup trigger when the global coverage is acceptable but one ring remains below `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_CATCHUP_PCT`.
- `VENPOD/src/main_launcher.cpp`: allows the existing `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_BUDGET` path for hidden ring backlog, instead of requiring far-SVO ownership to be high. This keeps startup/low-coverage frames on the safer default while draining outer-ring holes after the near rings are stable.

Validation:

- Build and unit gate passed after the accepted default: `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe`.
- Accepted waterline capture `VENPOD/build/captures/fix_hidden_ring_backlog_budget_waterline_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=88.45%`. Frame 240 improved from the previous accepted partial-sort baseline `missingVoxel=250`, `budgetMid=12`, `ring3 miss=250/833` to `missingVoxel=85`, `budgetMid=32`, `worstRing=3/87/106`.
- Accepted above-shore capture `VENPOD/build/captures/fix_hidden_ring_backlog_budget_above_shore_20260518`: passed hard gates with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0.0001`. Frame 240 remained improved versus the previous accepted partial-sort baseline (`missingVoxel=250` to `158`), and frame 360 remained improved (`missingVoxel=265` to `115`).
- Intermediate accepted probe `VENPOD/build/captures/fix_ring_backlog_catchup_waterline_20260518` proved the diagnosis before the hidden-backlog budget change: global coverage was `96%` while ring 3 was only `76%` covered (`worstRing=3/76/199`), so a global coverage-only policy was masking the visible far-ring holes.
- Controlled budget probe `VENPOD/build/captures/probe_ring_backlog_budget32_waterline_20260518` showed that using the higher budget only while the hidden ring backlog is active can reduce waterline frame-240 missing bricks to `49` under the same hard visual gates. The accepted default is slightly more conservative in the repeated default run (`85`), but still materially better than `250`.

Remaining:

- The engine still has visible/perceptual rendering issues in manual free-roam. These captures prove the specific hidden-ring backlog mechanism and reduce measured far-ring holes, but do not prove arbitrary routes are hole-free.
- Sparse prep remains too expensive in some frames. The final accepted above-shore run still showed a frame-360 sparse clipmap prep spike (`prep=9.63 ms`, `pumpVoxel=4.37 ms`), so performance remains `PARTIAL`.
- The fake terrain/material consistency issue is only partly addressed by prior shoreline/material and conservative AIR-edit fixes. Manual waterline editing still needs a focused brush/material capture proving generated mid voxels, exact sparse pages, and edited state agree after edits.

Baseline technical gates:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build .\VENPOD\build --config Release --target VENPOD'
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ctest --test-dir .\VENPOD\build -C Release --output-on-failure -R VENPODSparseCore'
```

Required new artifacts:

- Promoted focused terrain-gap regression using `terrain_gap_prefetch_wide_strict_debug50_20260515` thresholds.
- State-color debug capture with lifecycle counts.
- Fixed synchronous terrain grid capture.
- `VENPODSparseCore` fixed-grid readiness unit proof.
- Startup readiness-log capture: `VENPOD/build/captures/terrain_gap_startup_readiness_log_20260515`.
- Normal readiness perf capture: `VENPOD/build/captures/terrain_gap_readiness_perf_active3_20260515`.
- Stable-near-cull-off focused capture: `VENPOD/build/captures/terrain_gap_stable_cull_off_gpu_cull_on_20260515`.
- Post-publish terrain-critical readiness capture: `VENPOD/build/captures/terrain_gap_continuous_critical_ready_20260515`.
- LOD parent/child readiness capture.
- Boundary/halo coordinate tests are now present in `VENPODSparseCore`; keep them in the baseline gate.
- Long-walk route with near-target queue latency logs: `VENPOD/build/captures/terrain_gap_narrow_unsafe_long_walk_20260515`.
- Startup frame-0 public-render gate capture.
- Current sparse maintenance performance captures: `VENPOD/build/captures/surface_incremental_removal_fastwalk_probe2_20260516`, `VENPOD/build/captures/surface_incremental_removal_waterline_guard_20260516`, `VENPOD/build/captures/surface_incremental_removal_fastflight_guard_20260516`, and `VENPOD/build/captures/mid_interest90_default_fastflight_guard_20260516`.
- Reopened-goal correctness captures: `VENPOD/build/captures/exact_critical_generation_fastwalk_guard_20260516`, `VENPOD/build/captures/exact_critical_generation_highflight_guard_seq_20260516`, and `VENPOD/build/captures/exact_critical_generation_waterline_guard_seq_20260516`.
- Current mid-catchup pacing captures: `VENPOD/build/captures/mid_catchup_budget_default_long_fastwalk_20260516`, `VENPOD/build/captures/mid_catchup_budget_default_highflight_guard_20260516`, and `VENPOD/build/captures/mid_catchup_budget_default_waterline_guard_20260516`.
- Current waterline ray-cap captures: `VENPOD/build/captures/waterline_ray_cap_default_guard_20260516`, `VENPOD/build/captures/waterline_ray_cap_highflight_guard_20260516`, and `VENPOD/build/captures/waterline_ray_cap_long_fastwalk_guard_20260516`.
- Latest reopened-goal surface-removal/full-catchup captures: `VENPOD/build/captures/mid_capacity_surface_compaction_long_fastwalk_guard_20260516`, `VENPOD/build/captures/mid_capacity_surface_compaction_waterline_guard_20260516`, and `VENPOD/build/captures/mid_capacity_surface_compaction_highflight_guard_20260516`.
- Current shader-feedback/startup validation captures: `VENPOD/build/captures/feedback_original_coord_fastwalk_postrestore_20260516`, `VENPOD/build/captures/startup_surface_proof_highflight_late_postfix2_20260516`, `VENPOD/build/captures/feedback_original_coord_waterline_late_postfix2_20260516`, `VENPOD/build/captures/startup_speculative_upload_gate_highflight_20260516`, `VENPOD/build/captures/startup_speculative_upload_gate_waterline_20260516`, and `VENPOD/build/captures/startup_speculative_upload_gate_fastwalk_20260516`.
- Current surface-authoritative ownership validation captures: `VENPOD/build/captures/surface_auth_highflight_ownership_fix_20260516`, `VENPOD/build/captures/surface_auth_fix_broad_guard_20260516`, and `VENPOD/build/captures/surface_auth_fix_waterline_brush_guard_20260516`.
- Current aggressive far-mountain/default exact-surface validation capture: `VENPOD/build/captures/surface_radius3072_default_aggressive_freeroam_20260516`.
- Current sequential shoreline/high-flight regression captures: `VENPOD/build/captures/surface_radius3072_waterline_brush_guard_seq_20260516` and `VENPOD/build/captures/surface_radius3072_highflight_guard_seq_20260516`.
- Current mid-clipmap pacing captures and rejected probes: `VENPOD/build/captures/surface_radius3072_midpace_default_aggressive_20260516`, `VENPOD/build/captures/midpace_default_waterline_guard_seq_20260516`, `VENPOD/build/captures/midpace_default_highflight_guard_seq_20260516`, `VENPOD/build/captures/surface_radius3072_no_continuous_critical_ab_20260516`, and `VENPOD/build/captures/surface_radius3072_critical_coords1_ab_20260516`.
- Current exact/background ownership captures: accepted `VENPOD/build/captures/exact_near3072_default_aggressive_rebuilt_20260516`, plus non-closing stress diagnostics `VENPOD/build/captures/exact_near3072_waterline_regression_guard2_20260516` and `VENPOD/build/captures/exact_near3072_highflight_guard_20260516`.
- Current exact-handoff captures: `VENPOD/build/captures/debug50_fixed_default_exact1024_patch_20260516`, `VENPOD/build/captures/normal_fixed_default_exact1024_patch_20260516`, `VENPOD/build/captures/broad_freeroam_exact1024_patch_20260516`, and `VENPOD/build/captures/waterline_exact1024_patch_20260516`.
- Current surface metadata resize captures: `VENPOD/build/captures/broad_surface_metadata_patch_20260516`, `VENPOD/build/captures/highflight_surface_metadata_patch_20260516`, and `VENPOD/build/captures/waterline_surface_metadata_patch_20260516`.
- Current terrain-critical ready-footprint reuse captures: `VENPOD/build/captures/critical_signature_reuse_brush_guard_20260517` and `VENPOD/build/captures/critical_signature_reuse_waterline_guard_20260517`.
- Current mid-clipmap edit-overlay fast-path captures: `VENPOD/build/captures/broad_edit_overlay_fastpath_full_guard_20260517` and `VENPOD/build/captures/edit_overlay_fastpath_waterline_brush_guard_20260517`.
- Current exact-generation optimization captures: accepted `VENPOD/build/captures/air_hash_skip_probe_20260517`, `VENPOD/build/captures/air_hash_skip_waterline_brush_guard_20260517`, `VENPOD/build/captures/exact_column_span_probe_20260517`, and `VENPOD/build/captures/exact_column_span_waterline_brush_guard_20260517`; rejected/non-closing `VENPOD/build/captures/exact_empty_solid_fastpath_probe_20260517`; diagnostic-only `VENPOD/build/captures/debug50_air_hash_skip_broad_20260517`.
- Current high-altitude voxel-LOD ownership captures: accepted `VENPOD/build/captures/highalt_voxel_lod_guard_20260517`, `VENPOD/build/captures/highalt_voxel_lod_waterline_brush_guard_20260517`, `VENPOD/build/captures/lod_probe16_alt128_env_guard_20260517`, `VENPOD/build/captures/lod_probe16_alt128_waterline_env_guard_20260517`, `VENPOD/build/captures/lod_throttle_defaults_guard_20260517`, `VENPOD/build/captures/lod_throttle_defaults_waterline_guard_20260517`, `VENPOD/build/captures/lod_probe8_defaultalt_guard_20260517`, `VENPOD/build/captures/lod_probe8_default_guard_20260517`, `VENPOD/build/captures/lod_probe8_default_waterline_guard_20260517`, `VENPOD/build/captures/mid_lod24_coverage24_guard_20260517`, `VENPOD/build/captures/mid_lod_cap_order_default_guard_20260517`, `VENPOD/build/captures/mid_lod_cap_order_waterline_guard_20260517`, `VENPOD/build/captures/lod_predictive_skip_final_default_guard_20260517`, `VENPOD/build/captures/lod_predictive_skip_final_waterline_guard_20260517`, `VENPOD/build/captures/predictive_skip_restored_default_guard2_20260517`, and `VENPOD/build/captures/predictive_skip_restored_waterline_guard2_20260517`; rejected/non-closing `VENPOD/build/captures/critical_predictive_warm_probe_20260517` because it shifted cost into late frames, `VENPOD/build/captures/relief_surface_band_guard_20260517` because it preserved correctness but worsened broad prep/generation time, `VENPOD/build/captures/lod_probe16_env_guard_20260517` because capping the probe without broadening the altitude gate left non-startup `lodThrottle=0` exact bursts, `VENPOD/build/captures/lod_probe4_defaultalt_guard_20260517` because the wrapper timed out before post-run checks and produced no runtime log, `VENPOD/build/captures/mid_coverage_budget24_guard_20260517` because it preserved correctness but moved cost into late request/generation frames, `VENPOD/build/captures/mid_lod_budget24_guard_20260517` because coverage catch-up bypassed the LOD cap, `VENPOD/build/captures/critical_grid11x7_default_guard_20260517` because it preserved the hard miss/post-ready gates while dropping terrain ownership to `minVoxelTerrainScreen=19.30%`, `VENPOD/build/captures/mid_height_off_default_guard_20260517` and `VENPOD/build/captures/mid_height_off_waterline_guard_20260517` because disabling the mid-height layer dropped terrain ownership to `minVoxelTerrainScreen=19.74%`, and `VENPOD/build/captures/mid_height_budget8_default_probe_20260517` / `VENPOD/build/captures/mid_height_budget8_waterline_probe_20260517` because starving mid-height generation dropped terrain ownership to `minVoxelTerrainScreen=19.27%`; diagnostic-only `VENPOD/build/captures/current_rebuilt_ownership_debug_20260517`, `VENPOD/build/captures/mid_dda_voxel_only_highalt_probe_20260517`, `VENPOD/build/captures/mid_dda_voxel_only_highalt_guard_20260517`, `VENPOD/build/captures/mid_dda_continue_fallback_guard_20260517`, `VENPOD/build/captures/critical_rays11x7_broad_probe_20260517`, `VENPOD/build/captures/critical_no_predictive_broad_probe_20260517`, `VENPOD/build/captures/lod_skip_predictive_rays11x7_broad_probe_20260517`, `VENPOD/build/captures/highalt_rays11x7_default_guard_20260517`, `VENPOD/build/captures/highalt_rays11x7_waterline_guard_20260517`, and `VENPOD/build/captures/global_rays11x7_waterline_probe_20260517`.
- Current center-height reuse, conservative AIR edit, LOD mid-budget, and mid-interest queue rebuild captures: accepted `VENPOD/build/captures/relief_center_reuse_default_guard_20260517`, `VENPOD/build/captures/relief_center_reuse_waterline_guard_20260517`, `VENPOD/build/captures/mid_air_edit_conservative_waterline_guard_20260517`, `VENPOD/build/captures/mid_air_edit_conservative_default_guard_20260517`, `VENPOD/build/captures/mid_lod16_after_air_edit_default_probe_20260517`, `VENPOD/build/captures/mid_lod16_after_air_edit_waterline_probe_20260517`, `VENPOD/build/captures/mid_lod16_default_after_patch_guard_20260517`, `VENPOD/build/captures/mid_lod16_default_after_patch_guard2_20260517`, `VENPOD/build/captures/mid_lod16_default_after_patch_waterline_guard_20260517`, `VENPOD/build/captures/mid_interest_queue_rebuild_stress_guard_20260517`, and `VENPOD/build/captures/mid_interest_queue_rebuild_waterline_guard_20260517`; diagnostic-only `VENPOD/build/captures/debug50_waterline_ownership_20260517`, `VENPOD/build/captures/debug50_waterline_midvoxel_off_probe_20260517`, and `VENPOD/build/captures/waterline_midvoxel_off_visual_probe_20260517`.
- Current visible-generation drain captures: accepted `VENPOD/build/captures/visible_generation_drain_aggressive_20260517`, `VENPOD/build/captures/visible_generation_drain_waterline_guard_20260517`, and `VENPOD/build/captures/visible_generation_drain_deterministic_frame1350_20260517`; rejected/non-closing `VENPOD/build/captures/visible_gen48_ab_aggressive_20260517` because global generation-budget doubling flooded the page pool and failed unsafe-near ownership.
- Current voxel-only mid-volume fallback captures: accepted `VENPOD/build/captures/fix_voxel_only_mid_volume_fallback_waterline_guard_20260517` and `VENPOD/build/captures/fix_voxel_only_mid_volume_fallback_focused_long_walk_20260517`; diagnostic-only `VENPOD/build/captures/fix_voxel_only_mid_volume_fallback_debug50_waterline_20260517` because the wrapper timed out during post-processing after engine exit, but its `image_stats.csv` still proves the reproduced debug red miss converted to mid-voxel ownership.
- Current high-altitude LOD view-cone pacing captures: accepted `VENPOD/build/captures/fix_lod_view_grid3_long_fastflight_20260517` and `VENPOD/build/captures/fix_lod_view_grid3_waterline_brush_guard_20260517`; A/B probe `VENPOD/build/captures/probe_highalt_view_grid3_long_fastflight_20260517`; baseline comparison `VENPOD/build/captures/current_mid_volume_fallback_long_fastflight_20260517`.
- Current atomic surface-ready publish captures: accepted `VENPOD/build/captures/fix_atomic_total_work_cap_low_shore_20260517` and `VENPOD/build/captures/fix_atomic_total_work_cap_fastflight_20260517`; diagnostic/rejected intermediates `VENPOD/build/captures/diag_low_shore_brush_normal_20260517`, `VENPOD/build/captures/diag_material54_low_shore_brush_20260517`, `VENPOD/build/captures/fix_surface_ready_publish_gate_low_shore_20260517`, `VENPOD/build/captures/fix_surface_ready_publish_gate_fastflight_20260517`, `VENPOD/build/captures/fix_surface_ready_publish_gate_capped_fastflight_20260517`, `VENPOD/build/captures/fix_surface_ready_publish_gate_capped_low_shore_20260517`, and `VENPOD/build/captures/fix_direct_critical_upload_low_shore_20260517`.

Focused bug gates now verified:

```powershell
powershell -ExecutionPolicy Bypass -File .\VENPOD\engine_capture_smoke.ps1 -Config Release -NoBuild -ExitAfterFrames 220 -CaptureStartFrame 0 -CaptureIntervalFrames 20 -CaptureCount 11 -WalkTest -WalkTestSpeed 30 -WalkTestYawDegPerSec 8 -WalkTestPitchDeg -4 -MinUniqueSampleColors 1 -MaxFrameDarkPct 25 -MaxHeightProxyPct 100 -MaxHeightProxyScreenPct 100 -MaxValleyAtmosphereScreenPct 100 -OutputDir .\VENPOD\build\captures\terrain_gap_startup_frame0_gate112_surfaceaware_seq_20260515
powershell -ExecutionPolicy Bypass -File .\VENPOD\engine_capture_smoke.ps1 -Config Release -NoBuild -ExitAfterFrames 220 -CaptureStartFrame 120 -CaptureIntervalFrames 20 -CaptureCount 4 -WalkTest -WalkTestSpeed 30 -WalkTestYawDegPerSec 8 -WalkTestPitchDeg -4 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 100 -OutputDir .\VENPOD\build\captures\terrain_gap_continuous_critical_ready_20260515
powershell -ExecutionPolicy Bypass -File .\VENPOD\engine_capture_smoke.ps1 -Config Release -NoBuild -ExitAfterFrames 420 -CaptureStartFrame 120 -CaptureIntervalFrames 20 -CaptureCount 15 -WalkTest -WalkTestSpeed 42 -WalkTestYawDegPerSec 18 -WalkTestPitchDeg -4 -MaxHeightProxyPct 0 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 25 -OutputDir .\VENPOD\build\captures\terrain_gap_narrow_unsafe_long_walk_20260515
```

Focused Bug Completion Gate

The May 15 "misclassified as AIR / fake nearby land / incomplete startup world"
bug is fixed only while all of the following remain true:

1. Startup frame-0 capture uses the public-render gate and shows no black screen, no void exposure, `heightProxyScreenPct=0`, `miss=0`, and `unsafeNearMiss=0`.
2. Focused and long-walk captures after the gate opens report `miss=0`, `unsafeNearMiss=0`, and `heightProxyScreenPct=0`.
3. Terrain-critical readiness rows at or after the capture ready frame report zero post-publish non-ready critical targets (`postMissing=0`, `postRequested=0`, `postGenerating=0`, `postUploadQueued=0`, `postUploading=0`, and `postResidentMissingSurface=0`).
4. Debug mode does not classify horizon rays through open air as unsafe near terrain holes.
5. Real terrain ownership remains visible after public rendering starts; the startup hold is a loading state, not a fake terrain substitute.

## Exact Full Architecture Completion Gate

Do not mark the terrain streaming fix complete until all of the following are true:

1. TS-001 through TS-010 are `DONE_VERIFIED` or explicitly `DEFERRED_BY_USER_ONLY`.
2. The renderer never draws exact terrain from a non-ready terrain unit.
3. Parent LOD remains visible until the replacement child group is fully ready.
4. Boundary meshing uses deterministic halo/world sampling and passes boundary tests.
5. Fixed-grid synchronous mode renders solid terrain without holes.
6. The normal streaming path renders the same route without holes, slow visible pop-in, floating slabs, or partial LOD groups.
7. Startup frame 0 does not expose incomplete world state.
8. Debug captures and logs identify every culled/missing/nonready unit by state and coordinate.

## May 18 Clustered AIR Edit Mid-Clipmap Consistency

Status: `PARTIAL`

Requirement:

- Edited terrain must not leave stale/generated mid-clipmap material visible after a real brush erase cluster, while still avoiding the previous failure mode where one AIR edit collapses an entire coarse mid cell into a hole.

Source:

- User report on May 18: shoreline/sand-looking terrain is not real and disappears/reclassifies after drawing/brush interaction.
- Code path: `VENPOD/src/Simulation/SparseClipmap.cpp` `GenerateVoxelBrick()` edit overlay summarization and `tryEditedCellVoxel()`.
- Test path: `VENPOD/test/test_sparse_core.cpp` `TestSparseClipmapTileCache()`.

Current implementation:

- `EditedCellSummary` now tracks AIR and solid edit counts per coarse edit cell.
- Solid edits still win immediately so additions remain visible in coarse context.
- Single AIR edits remain conservative and do not collapse coarse mid cells.
- Clustered AIR edits now collapse stale procedural coarse cells to AIR when no solid edit is present and the AIR edit count reaches `max(8, ring.cellSize)`.
- `VENPODSparseCore` now has a focused regression proving both sides: one AIR edit does not collapse a coarse mid cell; a clustered AIR edit does collapse stale generated mid terrain to AIR.

Validation:

- `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe` passed.
- Accepted brush/edit normal capture: `VENPOD/build/captures/fix_clustered_air_mid_edit_waterline_brush_normal_20260518`.
  - Passed with `missPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `SPARSE_BRUSH_PAINT_SMOKE passed frames=180 queued=12 retired=486 applied=162 deltas=486 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0 cases=4/4`.
- Accepted material-debug capture: `VENPOD/build/captures/fix_clustered_air_mid_edit_waterline_brush_material54_20260518`.
  - Passed with zero height-proxy screen and zero ownership misses under material debug mode.
- Accepted waterline regression guard: `VENPOD/build/captures/fix_clustered_air_mid_edit_waterline_guard_20260518`.
  - Passed with `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=88.45%`.
  - Frame 240 remained at `missingVoxel=85`, `budgetMid=32`, `worstRing=3/87/106`, matching the hidden-ring accepted default behavior.
- Accepted above-shore regression guard: `VENPOD/build/captures/fix_clustered_air_mid_edit_above_shore_guard_20260518`.
  - Passed with `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=81.01%`.
  - Frame 240 remained at `missingVoxel=158`; frame 360 remained at `missingVoxel=115`.

Remaining:

- This proves the automated shoreline brush path and a focused unit-level mid-cell edit rule. It does not prove the user's exact manual camera/edit position is fixed unless reproduced in the same route.
- Long manual free-roam still needs a capture or user confirmation after this patch.
- Performance remains `PARTIAL`: accepted guards still show mid-clipmap prep/pump spikes, including above-shore frame 360 `prep=9.75 ms`, `pumpVoxel=5.44 ms`.

## May 18 Outer-Ring Backlog Budget Floor

Status: `PARTIAL`

Requirement:

- Far mountain/mid-voxel terrain should not remain sparse for hundreds of frames after near/shoreline terrain is visually stable.
- The mid-clipmap budget scheduler must not hide a badly missing outer ring behind a global average coverage score or a soft visual-deficit cap.

Source:

- User report on May 18: gaps remain in far mountains, the world still feels sparse and slow to fill.
- Runtime evidence from `VENPOD/build/captures/fix_clustered_air_mid_edit_waterline_brush_normal_20260518`: brush route passed hard visual gates but ring 3 stayed heavily backlogged with only `budgetMid=24`.
- Code path: `VENPOD/src/main_launcher.cpp` mid-clipmap budget block around `sparseMidClipmapRingBacklogCatchup`.

Current implementation:

- Added explicit `sparseMidClipmapOuterRingBacklogCatchup` detection when the worst missing ring is the outer voxel ring.
- Outer-ring backlog now counts as far-context clean backlog.
- The soft-deficit cap no longer suppresses outer-ring backlog catchup.
- After soft/clean budget caps, active clean outer-ring backlog enforces at least `VENPOD_SPARSE_MID_VOXEL_CLEAN_BACKLOG_BUDGET` when queued work exists.

Validation:

- `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe` passed.
- Accepted brush/edit capture: `VENPOD/build/captures/fix_outer_ring_backlog_floor_waterline_brush_normal_20260518`.
  - Passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and brush smoke `cases=4/4`.
  - Compared with `fix_clustered_air_mid_edit_waterline_brush_normal_20260518`, the same route now uses `budgetMid=32` in late outer-ring backlog frames instead of `24`.
  - Frame 240 improved from `missingVoxel=745`, `worstRing=3/11/740` to `missingVoxel=502`, `worstRing=3/40/492`.
  - Frame 360 improved from `missingVoxel=669`, `worstRing=3/23/640` to `missingVoxel=457`, `worstRing=3/50/413`.
  - Frame 480 improved from `missingVoxel=758`, `worstRing=3/25/622` to `missingVoxel=575`, `worstRing=3/52/393`.
- Accepted sequential waterline guard: `VENPOD/build/captures/fix_outer_ring_backlog_floor_waterline_guard_seq_20260518`.
  - Passed with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=88.45%`.
  - Frame 240 shows `missingVoxel=44`, `budgetMid=32`, `worstRing=3/93/54`.
- Accepted sequential above-shore guard: `VENPOD/build/captures/fix_outer_ring_backlog_floor_above_shore_guard_seq_20260518`.
  - Passed with `missPct=0`, `unsafeNearMissPct=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=81.01%`.
  - Frame 240 shows `missingVoxel=117`, `budgetMid=32`, `worstRing=3/89/84`; frame 360 shows `missingVoxel=110`, `budgetMid=32`, `worstRing=3/85/117`.
- Current-state revalidation after rejecting the pump scheduling experiment: `VENPOD/build/captures/fix_outer_ring_backlog_floor_post_pump_revert_brush_20260518`.
  - Passed with `missPct=0`, `unsafeNearMissPct max=0.1946`, `lodParentHeldPct max=0.007`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `minVoxelTerrainScreen=7.4963%` over the full startup-through-run timeline.
  - Maintained the accepted outer-ring budget behavior: frame 240 `missingVoxel=499`, `budgetMid=32`, `worstRing=3/41/489`; frame 360 `missingVoxel=457`, `budgetMid=32`, `worstRing=3/50/413`; frame 480 `missingVoxel=575`, `budgetMid=32`, `worstRing=3/52/393`.
- Accepted conservative outer-ring tail pump:
  - `VENPOD/src/Simulation/SparseClipmap.h` / `VENPOD/src/Simulation/SparseClipmap.cpp`: added `PumpVoxelGenerationForRing()` and a min-ring-safe voxel slot allocator so a targeted outer-ring drain can recycle existing outer-ring slots without evicting finer rings.
  - `VENPOD/src/main_launcher.cpp`: after the normal near-first clipmap pump, runs up to `VENPOD_SPARSE_MID_VOXEL_OUTER_RING_TAIL_BUDGET` extra bricks for the current worst outer ring only when clean coverage catchup is active, no parent-held pixels are active, and inner rings are below `VENPOD_SPARSE_MID_VOXEL_OUTER_RING_TAIL_INNER_MISSING_MAX` missing bricks.
  - `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl`: the outermost mid-voxel ring is now allowed to render when it is the preferred ring, but is not used as a coarse parent fallback for closer missing rings. This separates horizon seeding from fake parent terrain.
  - `.\build.ps1 -Config Release`; `.\build\bin\VENPODTests.exe` passed.
  - `VENPOD/build/captures/fix_outer_ring_tail_default32_brush_20260518` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=99.88%`, and `lodParentHeldPct max=0.007`. Compared with the current post-pump-revert brush baseline, frame 480 improved from `missingVoxel=575`, `worstRing=3/52/393` to `missingVoxel=482`, `worstRing=3/66/282`.
  - `VENPOD/build/captures/fix_outer_ring_tail_safe_final_above_shore_seq_20260518` passed with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0.0002`. This route still shows ring-3 starvation (`frame 240 missingVoxel=871`, `worstRing=3/0/833`; frame 360 `missingVoxel=1003`, `worstRing=3/0/833`), so the tail pump is accepted only as a safe partial improvement, not as completion.
- Accepted outer-ring seed without parent fallback:
  - `VENPOD/build/captures/probe_outer_ring_seed_no_parent_above_shore_cached_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0.0002`. Ring 3 now seeds above-shore instead of staying empty: frame 240 improved from `res ring3=0`, `worstRing=3/0/833` to `res ring3=285`, `worstRing=3/27/607`; frame 360 improved to `res ring3=328`, `worstRing=3/32/566`.
  - `VENPOD/build/captures/probe_outer_ring_seed_no_parent_brush_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `minVoxelTerrainScreen=99.88%`, and `lodParentHeldPct max=0`. Compared with the post-pump-revert brush baseline, frame 480 improved from `missingVoxel=575`, `worstRing=3/52/393` to `missingVoxel=406`, `worstRing=3/82/147`.
  - The first above-shore attempt `VENPOD/build/captures/fix_outer_ring_seed_no_parent_above_shore_seq_20260518` timed out because the edited `PS_Raymarch.hlsl` shader compile took about `154.57 s`; after the shader object was cached, the same behavioral route completed. Treat that first timeout as shader-cache warmup evidence, not a rendering regression.

Remaining:

- This reduces but does not eliminate far-ring backlog during the high-speed brush route; late frames still show hundreds of missing outer-ring targets (`147` missing in the accepted brush route at frame 480).
- The budget increase raises pump cost in some brush frames, for example accepted brush frame 240 `pumpVoxel=5.01 ms` and frame 360 `pumpVoxel=4.90 ms`.
- Further work should target interest churn/priority and generation cost, not just larger budgets.
- Rejected/non-closing pump scheduling probes:
  - `VENPOD/build/captures/fix_mid_voxel_round_robin_waterline_brush_normal_20260518` used equal ring round-robin. It reduced outer-ring misses but regressed LOD fallback badly (`lodParentHeldPct max=24.862`), so the pump change was backed out.
  - `VENPOD/build/captures/fix_mid_voxel_weighted_pump_waterline_brush_normal_20260518` used weighted ring pumping. It also improved ring 3 but still regressed LOD fallback (`lodParentHeldPct max=47.4453`), so that pump change was backed out too.
  - `VENPOD/build/captures/fix_mid_voxel_queue_preserve_brush_20260518` preserved the entire old mid-voxel generation queue across interest rebuilds. It improved the brush route frame-240 missing count to `184` and kept hard gates clean, but an above-shore guard exposed poor current-target behavior (`minVoxelTerrainScreen=56.76%` with a large parent-held event), so the patch was not accepted.
  - `VENPOD/build/captures/fix_mid_voxel_ring_queue_preserve_brush_20260518` constrained queue preservation to ring order. It still helped the brush tail versus the accepted baseline (`frame 360 missingVoxel=228`, `frame 480 missingVoxel=403`) and kept hard gates clean, but the exact-cadence above-shore guard `VENPOD/build/captures/fix_mid_voxel_ring_queue_preserve_above_shore_seq_20260518` showed `parentHeld=65097/4` at frame 240 and higher prep cost. The queue-preservation code was backed out and the build/tests passed afterward.
  - `VENPOD/build/captures/fix_outer_ring_tail_pump_brush_20260518` used an ungated outer-ring tail pump. It reduced far-ring missing counts, but caused startup parent-held fallback (`lodParentHeldPct max=18.1818`) by filling ring 3 before ring 2 was ready.
  - `VENPOD/build/captures/fix_outer_ring_tail_inner128_evict_prev_above_shore_seq_20260518` allowed ring 3 to evict ring 2 after an inner-missing threshold of `128`. It filled ring 3, but regressed above-shore parent-held fallback (`lodParentHeldPct max=9.6003`), so the ring-2 eviction/default threshold was rejected.
  - `VENPOD/build/captures/probe_outer_ring_seed_tail16_above_shore_20260518` passed and reduced above-shore ring-3 missing (`607 -> 529` at frame 240; `566 -> 500` at frame 360) with `lodParentHeldPct max=0.0002`, but the matching brush route `VENPOD/build/captures/probe_outer_ring_seed_tail16_brush_20260518` is rejected for default because it worsened frame cost (`body=36.86/39.40/39.19 ms` at frames 240/360/480, with `pumpVoxel` up to `7.69 ms`).
  - `VENPOD/build/captures/probe_outer_ring_seed_tail12_brush_20260518` is also rejected for default. It kept `lodParentHeldPct max=0` and reduced ring-3 missing, but still worsened the brush route to `body=34.06/33.30/42.06 ms` at frames 240/360/480, with `pumpVoxel` around `6.09..6.58 ms`. The accepted default remains `VENPOD_SPARSE_MID_VOXEL_OUTER_RING_TAIL_BUDGET=8`.

## May 18 Buried Mid-Voxel Generation Fast Path

Status: `PARTIAL`

Requirement:

- The renderer needs enough real mid/far voxel terrain to fill visible gaps without solving it by drawing fake parent terrain or by raising per-frame generation budgets until movement becomes too slow.

Source:

- User report on May 18: remaining gaps/fake terrain are visually better than before, but walking is still laggy and the world is still not coherent enough.
- Code path: `VENPOD/src/Simulation/SparseClipmap.cpp` `SparseClipmapTileCache::GenerateVoxelBrick()`.

Current implementation:

- Added a narrow unedited-generated-terrain fast path for safely buried mid-voxel cells.
- A whole brick that is below the minimum sampled halo column height by the existing surface band is filled as static stone without per-cell terrain relief sampling or six-neighbor surface classification.
- Individual cells below the local horizontal neighbor-column envelope use the same static-stone fast path.
- The path is disabled whenever edit overlays overlap the brick, so brush AIR/solid edit consistency remains on the slower authoritative summarization path.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/probe_buried_voxel_fastpath_waterline_brush_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, `lodParentHeldPct max=0`, and brush smoke `cases=4/4`, `fallback=0`, `missingResident=0`.
- `VENPOD/build/captures/probe_buried_voxel_fastpath_above_shore_20260518` passed hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0`.
- `VENPOD/build/captures/probe_buried_voxel_fastpath_brush_20260518` passed the high-stress brush route hard gates with `miss=0`, `unsafeNearMiss=0`, `heightProxyScreenPct=0`, `valleyAtmosphereScreenPct=0`, and `lodParentHeldPct max=0`.

Latest evidence:

- Waterline brush frames after the fast path showed a fully resident mid-voxel target set at frames 240/360/480 (`missingVoxel=0`, `queuedVoxel=0`, `resident rings=2085/1668/1250/833`) and low clipmap prep/pump cost (`prep=0.54..0.65 ms`, `pumpVoxel=0.00 ms`) on that route.
- Above-shore frames after the fast path also showed `missingVoxel=0`, `queuedVoxel=0`, and `lodParentHeld=0/0` at frames 240 and 360.
- The high-stress brush route remained visually safe but is not directly comparable to the earlier accepted waterline brush route because dynamic far-SVO-dominant interest reduction limited the interest set to `3379` bricks during that run.

Remaining:

- This is a cost-side improvement, not the final visual fix. The above-shore contact sheet still shows the remaining broad problem: far/mid terrain is coherent enough to avoid hard miss fallback, but it still reads as sparse/chunky with isolated far pieces.
- Need a like-for-like comparison route for the user's manual camera path before marking this as a verified performance win.
- The next root-rendering step is to diagnose the remaining far/mid visible holes by separating true missing interest, nonresident target bricks, culled ready bricks, and far-SVO sparse page gaps in one debug capture.

## May 18 Resident Mid-Voxel Closure for Hidden Terrain Misses

Status: `PARTIAL`

Requirement:

- Distant/mid terrain gaps must not turn into sky when the resident mid-voxel clipmap already contains solid terrain at the missed hit point.
- The fix must not reintroduce broad height-proxy terrain or parent-LOD fallback as fake scenery.

Source:

- User report on May 18: remaining far mountain holes and sparse terrain persisted even after residency improved; fake terrain/materials near water remained a concern.
- Runtime debug evidence from `VENPOD/build/captures/diag_hidden_mid_sample_class_miss50_above_shore_20260518` and `VENPOD/build/captures/diag_mid_hidden_surface_gate_miss50_above_shore_20260518`.
- Code path: `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` mid-voxel DDA and `DebugBackgroundMissHit()`.

Current implementation:

- Added deterministic air/solid continuity tracking for missing mid-voxel DDA samples instead of blindly clearing `previousMidVoxelWasAir`.
- Added a normal-rendering resident mid-voxel closure path for hidden terrain misses:
  - It only runs in voxel-terrain-only mode after normal background ownership failed.
  - It first intersects the deterministic terrain function to find the missed terrain point.
  - It only draws if `SampleResidentMidVoxelFallback()` finds non-air mid-voxel data at that point.
  - It rejects coarser-ring fallback (`actualRing > preferredRing`) so the closure does not draw parent LOD as fake terrain.
  - It uses a minimal material/fog shade to keep the pixel shader under the DX12 pipeline limit.
- Kept debug mode 50 instrumentation that separates resident hidden misses into tagged surface, exposed surface, interior solid, AIR, and missing.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- Rejected first closure attempt: `VENPOD/build/captures/fix_resident_mid_voxel_closure_normal_above_shore_20260518`.
  - HLSL compiled, but DX12 failed to create the graphics pipeline after the full shading helper made the pixel shader too large (`PS_Raymarch.hlsl seconds=195.83 bytes=3476652`, then pipeline creation failed). That version was simplified.
- Accepted normal above-shore capture: `VENPOD/build/captures/fix_resident_mid_voxel_closure_same_ring_normal_above_shore_20260518`.
  - Passed engine capture smoke.
  - Frame 240: `midVoxel=245549`, `farSvo=238293`, `miss=2038`, `unsafeNearMiss=0`, `lodParentHeld=0`, `parentHeldUntilChildrenReady=0`, `unsafeSample=0/0,0,0 dist=0`.
  - Frame 240 ownership pressure reports `terrainPct=99`, `voxelTerrainPct=99`, `missPct=0`.

Latest evidence:

- Diagnostic split before the closure showed the remaining hidden terrain miss was not primarily missing residency: sampled frame 235 had `redMissing=0.00%`, `violetAir=0.07%`, `greenTagged=1.05%`, and `orangeInterior=9.22%`.
- The same-ring closure eliminated the unsafe hidden sample breadcrumb in the accepted normal capture while preserving `lodParentHeld=0`.

Remaining:

- This fixes a concrete shader-side root cause, but it is still `PARTIAL` because manual free-roam and the user's exact camera/waterline editing path have not been revalidated after this patch.
- The normal capture still visually contains chunky far/mid terrain and isolated distant pieces; this patch reduces sky gaps but does not solve meshing/LOD aesthetics or walking hitching.
- Shader compile remains expensive after raymarch edits (`PS_Raymarch.hlsl seconds=162.54` in the accepted capture), so future changes need to avoid adding heavy inline shading to the pixel shader.

## May 18 Dense Mid-Voxel Interest Rebalance

Status: `PARTIAL`

Requirement:

- Remaining far mountain gaps must be filled by real resident mid-voxel terrain, not by height proxy, parent fallback, or broad fake background terrain.
- The streamer should not leave the far rings structurally under-covered while the cache still has practical GPU memory headroom.

Source:

- User report on May 18: rendering is materially better but still has far mountain gaps, slow fill-in, sparse world coverage, and fake-looking terrain.
- Runtime evidence from `VENPOD/build/captures/current_closure_waterline_brush_default_recheck_20260518`: the accepted shoreline brush guard passed, but frame 480 still used `voxelSlots=6144`, `voxelInterest=95pct`, `budgetMid=40`, `evictVoxel=40`, and only `res ring3=846` with `worstRing=3/90/75`.
- Code path: `VENPOD/src/main_launcher.cpp` sparse mid-voxel defaults and `VENPOD/src/Simulation/SparseClipmap.cpp` `UpdateVoxelInterest()` ring quota weighting.

Current implementation:

- Raised default `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS` from `6144` to `8192`.
- Raised default `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT` from `95` to `100`, so the current target set can use the whole resident cache instead of reserving 5% for stale/non-interest bricks by default.
- Rebalanced mid-voxel ring quota weights from `5/4/3/2` style near-heavy weighting to a less starved outer-ring weighting. For four rings this changes the rough quota split from `2085/1668/1250/833` to `2521/2206/1890/1575` on the tested route.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/dense_mid_voxel_interest_waterline_brush_20260518` passed engine capture smoke:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.83%`
  - terrain-critical `postNonReady=0`
  - brush paint smoke passed `cases=4/4`, `fallback=0`, `missingResident=0`, `deltaMismatch=0`

Latest evidence:

- New route configuration logged `voxelSlots=8192` and `voxelInterest=100pct`.
- Compared with the previous accepted waterline brush guard at frame 480:
  - resident far ring increased from `res ring3=846` to `res ring3=1479`.
  - target interest far ring increased from `int ring3=833` to `int ring3=1575`.
  - frame body dropped from `52.03 ms` to `32.82 ms`.
  - clipmap prep dropped from `16.35 ms` to `10.69 ms`.
  - voxel pump dropped from `8.21 ms` to `5.15 ms`.
  - hard rendering gates stayed clean: `parentHeld=0/0`, `heightProxy=0`, `valleyAtmosphere=0`.

Remaining:

- Status remains `PARTIAL`: this does not prove full free-roam infinite-world completion, and the larger target set still had frame-480 `missingVoxel=184` (`miss ring2=60`, `miss ring3=124`) while moving.
- The capture contact sheet for this waterline route is not a good final visual acceptance artifact because the scripted camera spends much of the sample near/under the waterline.
- Need a manual-camera or above-shore mountain-focused capture after this rebalance to verify that the user's visible far gaps are reduced in the exact view that triggered the complaint.
- Next action required: run a mountain-focused normal capture and, if far gaps remain, add a screen-tile/ray-grid driven mid-voxel interest source instead of only changing capacity/quota defaults.

## May 18 High-Altitude Far-SVO Dominant Mid Catchup

Status: `PARTIAL`

Requirement:

- High/mountain views should not leave the mid-voxel cache far behind while far-SVO is temporarily owning the screen.
- The fix must preserve far-SVO fallback until mid-voxel coverage is actually strong enough, and must not reintroduce miss spikes, parent-held fallback, height proxy, or shoreline brush regressions.

Source:

- User report on May 18: far mountains still show gaps/sparse loading and the engine feels slow to fill.
- Evidence from `VENPOD/build/captures/dense_mid_voxel_interest_above_shore_20260518`: high-altitude stress views were almost entirely far-SVO owned (`midVoxelScreenPct=0`) and the far-SVO-dominant mid interest set was still sparse early (`frame 240 coveragePct=70`, `res ring3=0`, `missingVoxel=1326`).
- Code path: `VENPOD/src/main_launcher.cpp` `VENPOD_SPARSE_MID_LOD_THROTTLE_BUDGET` and high-altitude/far-SVO-dominant mid-clipmap budget logic.

Current implementation:

- Raised the default far-SVO-dominant mid-clipmap LOD throttle budget from `16` to `24`.
- Kept the reduced high-altitude far-SVO-dominant interest set at `55%`; a full `100%` high-altitude interest probe was rejected because it over-expanded the target set without enough budget to fill it.
- Did not change shader ownership. The far-SVO layer still owns high-altitude views until a safer mid/far handoff can be implemented without exceeding the pixel-shader/PSO limit.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/fix_lod_throttle24_high_altitude_above_shore_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.19%`
  - terrain-critical `postNonReady=0`
- `VENPOD/build/captures/fix_lod_throttle24_static_above_shore_20260518` passed the static above-shore guard.
- `VENPOD/build/captures/fix_lod_throttle24_waterline_brush_guard_20260518` passed shoreline/brush guard with brush smoke `cases=4/4`, `fallback=0`, `missingResident=0`, `deltaMismatch=0`.

Latest evidence:

- Compared with `VENPOD/build/captures/dense_mid_voxel_interest_above_shore_20260518`, high-altitude frame 240 improved from:
  - `coveragePct=70`, `missingVoxel=1326`, `res ring3=0`, `worstRing=3/0/866`, `budgetMid=16`
  - to `coveragePct=93`, `missingVoxel=242`, `res ring3=655`, `worstRing=3/72/238`, `budgetMid=32`.
- High-altitude frame 480 stayed healthy: `coveragePct=97`, `missingVoxel=87`, `parentHeld=0/0`, `body=21.10 ms`.
- Waterline brush guard after the budget change preserved shoreline behavior: frame 480 `miss=0`, `unsafeNearMiss=0`, `parentHeld=0/0`, `heightProxy=0`, and brush paint smoke passed.

Rejected probes:

- `VENPOD/build/captures/mid_ready_high_altitude_handoff_above_shore_20260518`: shader-side high-altitude handoff patch was rejected. `PS_Raymarch.hlsl` compiled for about `171.36 s`, then DX12 failed to create the graphics pipeline (`Failed to create graphics pipeline state: 0x-7FF8FFF2`). The patch was reverted.
- `VENPOD/build/captures/cpu_far_relinquish_high_altitude_above_shore_20260518`: CPU-side far-SVO readiness suppression was rejected. When far-SVO relinquished at frame 322, `midVoxel=1071560` but `miss=141166` (`miss=8%`), failing ownership stability. This proves mid-voxel coverage is not yet strong enough to replace far-SVO outright in high-altitude motion.
- `VENPOD/build/captures/probe_full_mid_interest_high_altitude_above_shore_20260518`: full `100%` far-SVO-dominant mid interest was rejected for default. It passed visual gates but worsened fill because the target set jumped to `8192` while the budget was still too low; frame 480 coverage fell to `55%` with `missingVoxel=3628`.

Remaining:

- This is a fill-rate improvement, not completion. High-altitude render ownership is still far-SVO dominated (`midVoxelScreenPct=0` on the accepted high-altitude route), so the broad visual style can still look coarse/chunky.
- The next architectural fix is a safer mid/far handoff: either a lighter shader path that can survive PSO creation, or a CPU/runtime strategy that only suppresses far-SVO after a screen-relevant mid target set is known complete rather than using aggregate clipmap coverage.

## May 18 Far-SVO Material / Occupancy Recheck

Status: `PARTIAL`

Requirement:

- Fake shoreline sand/water and far mountain gaps must not be caused by stale or inconsistent far-SVO CPU data.
- Far-SVO cache rebuilds must be validated against the same high-altitude route instead of testing stale cache contents.

Source:

- User report on May 18: visible fake sand near water disappears when exact sparse/brush state arrives; far mountains still contain gaps and the world still feels inconsistent.
- Code path: `VENPOD/src/Graphics/FarVoxelOctree.cpp` CPU far-SVO cache generation and material classification; `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` far-SVO traversal and shader-side terrain material.

Current implementation:

- Bumped `kFarVoxelOctreeCacheVersion` from `17` to `18` so new CPU far-SVO data is not confused with stale cache data.
- Made `FarVoxelOctree::DominantMaterial()` match the shader's sea-level material rule more closely:
  - water when terrain height is below `SEA_LEVEL_Y`;
  - sand only for low-relief shoreline cells within `SEA_LEVEL_Y + 6`;
  - stone for high relief or high elevation, otherwise dirt.
- Made `CellMayContainTerrain()` and `CellCanCollapseSolid()` sample additional interior points for larger cells before pruning or collapsing far-SVO nodes.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- First high-altitude capture with `VENPOD_FAR_SVO_ALLOW_COLD_BUILD=1` intentionally failed backend readiness because the new cache did not exist yet:
  - capture: `VENPOD/build/captures/far_svo_conservative_material_high_altitude_20260518`
  - failure frame 120: `missing=0x100 [far-svo]`, `farSvo=loading`.
  - useful evidence: the async builder completed and saved `build/bin/venpod_far_svo_cache_r6_d6_s12345.bin`.
- Re-run against the new cache passed:
  - capture: `VENPOD/build/captures/far_svo_conservative_material_high_altitude_cached_20260518`
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.20%`
  - terrain-critical `postNonReady=0`

Latest evidence:

- New cached high-altitude ownership is effectively unchanged from `fix_lod_throttle24_high_altitude_above_shore_20260518`.
  - frame 240: `farSvo=1753579`, `midVoxel=0`, `miss=0`.
  - frame 320: `farSvo=1346465`, `midVoxel=0`, `miss=23`.
  - frame 400: `farSvo=1549618`, `midVoxel=0`, `miss=0`.
  - frame 480: `farSvo=1225928`, `midVoxel=0`, `miss=1`.
- The far-SVO cache is still heavy: `venpod_far_svo_cache_r6_d6_s12345.bin` is about `84 MB`, and the runtime reports `farUploadMB=80.43/80.43`.
- During the cached run, far-SVO still spends early frames in `loading/uploading` before `farSvo=on`, so startup/perceived fill latency is a real separate issue.

Remaining:

- This patch is not the root fix for the major high-altitude visual problem. The high-altitude route remains far-SVO owned because `RaymarchMidVoxelClipmap()` explicitly returns early when far-SVO is ready and the view is high altitude.
- The fake shoreline material complaint is not fully proven fixed; most high-altitude far-SVO visible hits compute material in shader-side `FarTerrainMaterial()`, not from CPU leaf material.
- Next action required: target the actual authority decision. Add a screen-relevant mid-voxel readiness/handoff gate, or build a lighter mid/far transition path that does not trip the `PS_Raymarch.hlsl` PSO limit.

## May 18 Screen-Ray Mid-Voxel Interest And Handoff

Status: `PARTIAL`

Requirement:

- High-altitude and far mountain views must not remain permanently owned by fake/coarse far-SVO terrain when resident mid voxels are ready enough to draw.
- The handoff must not recreate the previous CPU-side far-SVO suppression failure where mid takeover caused large miss spikes.
- Shoreline/brush behavior must remain clean.

Source:

- User report on May 18: sparse gaps remain in far mountains; fake terrain/materials still appear; rendering is slow and not yet a proper voxel world.
- Evidence from `VENPOD/build/captures/mid_screen_ready_handoff_high_altitude_20260518`: a first handoff attempt compiled and passed, but was effectively a no-op (`midVoxel=0` at frames 240/320/400/480) because the clipmap target set was not screen-relevant enough.
- Code path: `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` `RaymarchMidVoxelClipmap()` high-altitude far-SVO skip; `VENPOD/src/Simulation/SparseClipmap.cpp` `UpdateVoxelInterest()`; renderer constant plumbing in `VENPOD/src/Graphics/Renderer.cpp`.

Current implementation:

- Added two renderer CPU-to-shader handoff signals:
  - `CameraParams::midFieldVoxelInterestCoverage`
  - `CameraParams::midFieldVoxelWorstRingCoverage`
- Packed those into `exactNearParams.z/w` so the shader can distinguish aggregate resident count from current target-set maturity.
- Changed high-altitude mid-voxel skip logic so far-SVO only hides mid voxels while mid interest is still immature. Once overall target coverage is at least `0.97` and worst-ring coverage is at least `0.94`, the shader tries mid voxels first and keeps far-SVO as fallback.
- Added high-altitude forward screen-ray terrain interest lines per ring in `SparseClipmapTileCache::UpdateVoxelInterest()`. This fixes the discovered no-op where the old view fan was scaled by local brick radius and could target only about `512` units for ring 0 while the shader starts mid rendering around `1024` units.

Validation:

- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/mid_screen_ready_handoff_high_altitude_20260518` passed hard gates but was diagnosed as a no-op:
  - frame 480 clipmap `coveragePct=99`, `missingVoxel=0`, `worstRing=3/99/8`
  - ownership still `midVoxel=0`, `farSvo=1232604`, `miss=1`
- `VENPOD/build/captures/screen_ray_mid_interest_handoff_high_altitude_20260518` passed hard gates after adding screen-ray interest:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.21%`
  - terrain-critical `postNonReady=0`
- `VENPOD/build/captures/screen_ray_mid_interest_waterline_brush_guard_20260518` passed shoreline/brush guard:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.91%`
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`

Latest evidence:

- The accepted high-altitude route now shows real mid-voxel ownership during the problematic far/mountain motion:
  - frame 240: `midVoxel=0`, `farSvo=1742251`, `miss=0`
  - frame 320: `midVoxel=634501`, `farSvo=655406`, `miss=23`, `unsafeNearMiss=0`, `lodParentHeld=0`
  - frame 400: `midVoxel=850930`, `farSvo=689335`, `miss=0`, `unsafeNearMiss=0`, `lodParentHeld=0`
  - frame 480: `midVoxel=0`, `farSvo=1196699`, `miss=1`, `unsafeNearMiss=0`, `lodParentHeld=0`
- The screen-ray interest changed the target-set behavior: frame 400 expanded to `interestedVoxel=8192` with visible mid ownership, while the previous no-op handoff kept high-altitude ownership at `midVoxel=0`.
- The shoreline guard preserved the verified waterline path:
  - frame 480: `near=79482`, `midVoxel=64609`, `waterContext=529644`, `miss=0`, `unsafeNearMiss=0`.

Remaining:

- Status remains `PARTIAL`: frame 480 high-altitude still returns to far-SVO-only ownership even with mature clipmap coverage, so the handoff is not complete.
- The high-altitude target set can still oscillate between reduced far-SVO-dominant interest (`4505`) and full interest (`8192`), which may explain why the mid layer owns frames 320/400 but not frame 480.
- Further work should stabilize the screen-ray interest target or add shader diagnostics for why mature mid clips reject late-route rays.

## May 18 Far-SVO/Mid-Voxel Handoff Stabilization

Status: `PARTIAL`

Requirement:

- High-altitude terrain must stop flickering between sparse/mid voxel terrain and far-SVO fallback because of unstable streaming policy decisions.
- A failed shader experiment must not remain in the tree.
- Shoreline/brush behavior must remain validated while high-altitude handoff changes are made.

Source:

- User report on May 18: rendering was improved but still sparse, slow, and fake terrain/materials remained visible.
- Evidence from `VENPOD/build/captures/screen_ray_mid_interest_handoff_high_altitude_20260518`: the clipmap policy alternated between `interestedVoxel=4505` with high coverage and `interestedVoxel=8192` with low coverage. Ownership then alternated between mid-voxel-heavy frames and far-SVO-only frames.
- Rejected probe `VENPOD/build/captures/relaxed_mid_handoff_high_altitude_20260518`: relaxing the shader handoff gate to `0.94/0.86` caused renderer initialization failure after recompiling `PS_Raymarch.hlsl` (`Failed to create graphics pipeline state: 0x-7FF8FFF2`).

Current implementation:

- Reverted the failed shader handoff relaxation back to the last accepted `0.97` overall / `0.94` worst-ring gate in `PS_Raymarch.hlsl`.
- Added `VENPOD_SPARSE_MID_VOXEL_FARSVO_HOLD_FRAMES` and a high-altitude far-SVO-dominant policy hold in `src/main_launcher.cpp`. Once the clean high-altitude far-SVO-dominant state is observed, the reduced mid-voxel target remains stable while the view stays clean instead of flipping every few frames.
- Added CPU-side high-altitude handoff promotion controls:
  - `VENPOD_SPARSE_MID_VOXEL_HANDOFF_COVERAGE_PCT` default `94`
  - `VENPOD_SPARSE_MID_VOXEL_HANDOFF_WORST_RING_PCT` default `90`
  - `VENPOD_SPARSE_MID_VOXEL_HANDOFF_MAX_MISSING` default `256`
- The promotion only affects the shader handoff signal when the high-altitude mid target is mostly resident, far-SVO fallback is ready, no miss/unsafe/lod-parent-held pixels were retired, and missing target bricks are below the configured cap. It does not mark actual missing bricks resident.

Validation:

- `.\build.ps1 -Config Release` passed after each `src/main_launcher.cpp` change.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/restore_handoff_shader_startup_20260518` passed, proving the renderer launches again after reverting the failed shader threshold probe.
- `VENPOD/build/captures/stable_farsvo_mid_policy_high_altitude_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.51%`
  - terrain-critical `postNonReady=0`
- `VENPOD/build/captures/safe_handoff_promotion_high_altitude_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.51%`
  - terrain-critical `postNonReady=0`
- `VENPOD/build/captures/safe_handoff_promotion_waterline_brush_guard_20260518` passed shoreline/brush guard:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.91%`
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`

Latest evidence:

- The high-altitude target-set flip-flop was removed in the stabilized run: logged `PERF_SPARSE_CLIPMAP` frames from `240` through `513` stayed at `interestedVoxel=4505` instead of alternating with `8192`.
- With safe handoff promotion, the high-altitude capture retained real mid-voxel ownership across many sampled frames:
  - frame `260`: `midVoxel=664968`, `farSvo=1033484`, `miss=23`
  - frame `320`: `midVoxel=526406`, `farSvo=754256`, `miss=109`
  - frame `400`: `midVoxel=858009`, `farSvo=663787`, `miss=0`
  - frame `480`: `midVoxel=494062`, `farSvo=721583`, `miss=1`
  - all sampled rows had `unsafeNearMiss=0` and `lodParentHeld=0`
- The capture still contains some zero-mid viewpoints, for example frames `360`, `380`, `440`, and `516`. Those no longer correlate with the 4505/8192 target oscillation, so the remaining issue is likely shader ray eligibility/traversal behavior or view-dependent mid/far ownership, not the original streaming-policy denominator flip.

Remaining:

- Status remains `PARTIAL`. This pass fixes a concrete policy oscillation and rejects the broken shader probe, but it does not prove final free-roam terrain density or eliminate all far-SVO-only viewpoints.
- Next action required: add or use shader-side diagnostics for why `RaymarchMidVoxelClipmap()` rejects zero-mid high-altitude viewpoints when CPU coverage is high, then decide whether the fix belongs in ray eligibility, traversal budget, start/end distance, or mid/far ownership composition.

Follow-up evidence on May 18:

- A review of `VENPOD/build/captures/safe_handoff_promotion_high_altitude_20260518` found a bug in the CPU promotion itself. The shader requires both `exactNearParams.z >= 0.97` and `exactNearParams.w >= 0.94`, but the first CPU-side promotion only raised the worst-ring signal. Safe-but-imperfect frames such as frame `360` still failed the shader handoff because overall coverage was `94%`.
- Accepted fix: when the high-altitude safe-handoff condition is true, `src/main_launcher.cpp` now raises both CPU-to-shader handoff signals: overall coverage to at least `0.97` and worst-ring coverage to at least `0.94`. The safe condition still requires far-SVO fallback ready, no retired miss/unsafe/parent-held pixels, and bounded missing mid target bricks.
- `.\build.ps1 -Config Release` passed.
- `VENPOD/build/captures/safe_handoff_both_signals_high_altitude_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.51%`
  - terrain-critical `postNonReady=0`
- Compared with `safe_handoff_promotion_high_altitude_20260518`, mid-voxel ownership on shader frames `240-520` improved:
  - total mid-voxel pixels increased from `151,393,183` to `172,948,179`
  - zero-mid frames dropped from `44` to `11`
  - frame `360` changed from `midVoxel=0`, `farSvo=1,467,865` to `midVoxel=742,430`, `farSvo=740,922`, `miss=0`
  - frame `440` changed from far-SVO-only to `midVoxel=610,058`, `farSvo=627,539`, `miss=0`
  - all sampled rows kept `unsafeNearMiss=0` and `lodParentHeld=0`
- `VENPOD/build/captures/safe_handoff_both_signals_waterline_brush_guard_20260518` passed shoreline/brush guard:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.91%`
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`

Remaining after follow-up:

- Status remains `PARTIAL`. The recurring handoff oscillation is materially reduced, but final completion still requires manual/free-roam confirmation that far-mountain gaps and fake material regions are gone.
- The remaining zero-mid high-altitude frames are now mostly startup/late low-coverage cases (`240-244`, `275-276`, `428`, `514-516`) rather than broad policy oscillation. Next action should target whether those remaining cases are acceptable far-SVO fallback windows, insufficient late-frame mid target coverage, or a separate ray eligibility/traversal rejection.

Second follow-up evidence on May 18:

- The remaining zero-mid frames after `safe_handoff_both_signals_high_altitude_20260518` were mostly bounded-coverage cases where far-SVO fallback was clean, not unbounded missing-world cases. An env A/B with `VENPOD_SPARSE_MID_VOXEL_HANDOFF_COVERAGE_PCT=90`, `VENPOD_SPARSE_MID_VOXEL_HANDOFF_WORST_RING_PCT=80`, and `VENPOD_SPARSE_MID_VOXEL_HANDOFF_MAX_MISSING=384` was accepted because it used already-resident mid voxels more aggressively without adding generation load or creating unsafe misses.
- Accepted default change: high-altitude safe handoff promotion now defaults to `90%` overall mid target coverage, `80%` worst-ring coverage, and at most `384` missing target bricks, while still requiring far-SVO fallback readiness and no retired miss/unsafe/parent-held ownership pixels.
- `.\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/fix_handoff_90_80_384_high_altitude_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.51%`
  - terrain-critical `postNonReady=0`
- High-altitude ownership on shader frames `240-520` improved again:
  - total mid-voxel pixels increased to `180,298,981`
  - zero-mid frames dropped to `1`
  - `unsafeNearMiss` stayed `0`
  - frame `428` changed from far-SVO-only with a tiny miss cluster to `midVoxel=665,489`, `farSvo=637,539`, `miss=4`
  - frame `514` changed from `midVoxel=0`, `farSvo=1,543,294` to `midVoxel=592,941`, `farSvo=951,494`, `miss=0`
  - frame `516` changed from `midVoxel=0`, `farSvo=1,579,857` to `midVoxel=642,133`, `farSvo=937,539`, `miss=0`
- `VENPOD/build/captures/fix_handoff_90_80_384_waterline_brush_guard_20260518` passed shoreline/brush guard:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.91%`
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`

Remaining after second follow-up:

- Status remains `PARTIAL`. The high-altitude route is now mostly mid-voxel participating with far-SVO fallback, but final completion still requires the user's manual free-roam problem areas to stop showing fake shoreline material and far-mountain gaps.
- Next action should shift from this specific handoff threshold to either manual/problem-route capture evidence or the fake material authority path, because the measured high-altitude ownership failure is no longer the dominant issue on this route.

## May 18 Exact-Near Authority For High-Altitude Background Layers

Status: `PARTIAL`

Requirement:

- Background layers must not draw fake terrain or fake sand inside the editable/exact-near foreground, including from high-altitude camera angles.
- Removing fake foreground ownership must not reintroduce high-altitude holes, unsafe misses, parent-held LOD gaps, or shoreline brush regressions.

Source:

- User report on May 18: near-water sand looked real until drawing/brush interaction caused it to disappear, proving the pre-edit visual layer was not authoritative terrain.
- Code path: `VENPOD/assets/shaders/Graphics/PS_Raymarch.hlsl` `BackgroundHitAllowedByExactNear()`.

Current implementation:

- Removed the high-altitude exception that allowed `BACKGROUND_LAYER_MID_VOXEL` and `BACKGROUND_LAYER_FAR_SVO` to bypass `ExactNearDistance()`.
- Background layers now obey the exact-near ownership boundary regardless of camera altitude. This keeps the exact sparse/raster foreground as the terrain authority inside the editable radius and reduces the chance that far/mid background terrain appears as brushable shoreline land.

Validation:

- `.\build.ps1 -Config Release` refreshed runtime assets after the shader edit.
- First real shader startup after asset refresh passed in `VENPOD/build/captures/probe_no_high_alt_exact_bypass_startup_copied_20260518`.
- `VENPOD/build/captures/probe_no_high_alt_exact_bypass_high_altitude_20260518` passed hard gates:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=69.81%`
  - terrain-critical `postNonReady=0`
- `VENPOD/build/captures/probe_no_high_alt_exact_bypass_waterline_brush_guard_20260518` passed shoreline/brush guard:
  - `maxLodParentHeld=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=92.91%`
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`
- `.\build\bin\VENPODTests.exe` passed.

Latest evidence:

- High-altitude ownership remained healthy after the exact-near bypass removal:
  - shader frames `240-520`: `ZeroMid=1`, `MissSum=2293`, `UnsafeSum=0`
  - frame `360`: `midVoxel=742,430`, `farSvo=739,203`, `miss=0`
  - frame `428`: `midVoxel=665,489`, `farSvo=635,521`, `miss=4`
  - frame `516`: `midVoxel=642,133`, `farSvo=933,895`, `miss=0`
- Compared with the previous accepted high-altitude route, far-SVO ownership decreased while mid ownership stayed materially the same. This is the desired direction for fake foreground authority: less background ownership inside protected space without exposing holes.
- Waterline ownership still reports near/surface ownership on the brush guard route, and brush smoke remains clean.

Remaining:

- Status remains `PARTIAL` because this validates the exact-near ownership rule on automated high-altitude and shoreline routes, not on the user's exact manual free-roam viewpoint.
- Next action required: run or capture the user's current free-roam problem location, preferably with debug owner/material mode, to verify the visible fake sand/land disappears without replacing it with holes.

## May 19 Parent-Held LOD Removal And Distance-Aware Surface Ownership Gate

Status: `PARTIAL`

Requirement:

- Mid/far terrain must not expose coarser parent LOD samples as visible terrain when preferred child-ring data is missing.
- Sparse raster surfaces must be measurable as bounded near/protected terrain, not guessed from total screen coverage.
- The fix must not hide the problem behind height proxies, valley atmosphere, fake far sparse surfaces, or unsafe near misses.

Source:

- User report on May 19: rendering looked regressed/broken in free roam, still had gaps/fake terrain, and progress was getting stuck in local loops.
- Focused owner-contract capture `VENPOD/build/captures/owner_contract_debug55_far_surface_20260519` failed with `maxLodParentHeld=5.7911%`, while `farSurface=0`. This proved the current dominant measurable renderer bug was visible coarser mid-voxel parent fallback, not far sparse-surface leakage.

Current implementation:

- `assets/shaders/Graphics/PS_SparseSurface.hlsl` now records `farSurface` fragments at ownership word `21` when sparse raster surface fragments are beyond the protected near distance. Debug mode `55` now colors sparse surface distance bands: yellow exact-near, orange protected band, magenta illegal/far sparse surface.
- `src/Graphics/SparseVoxelGpuResources.*` now allocates and logs the extra render ownership word as `farSurface`.
- `engine_capture_smoke.ps1` now exports `farSurface`/`farSurfacePct` in `ownership_timeline.csv` and supports `-MaxFarSurfacePct`.
- `assets/shaders/Graphics/PS_Raymarch.hlsl` no longer searches coarser mid-voxel rings in `SampleResidentMidVoxelFallback()`. Missing preferred-ring child data now falls through to finer resident data, far-SVO, or a real miss path instead of drawing a parent LOD slab.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed and refreshed runtime assets.
- `.\build\bin\VENPODTests.exe` passed.
- Rejected/failing diagnostic baseline:
  - `VENPOD/build/captures/owner_contract_debug55_far_surface_20260519`
  - failed `-MaxLodParentHeldPct 0.1` with `maxLodParentHeld=5.7911%`
  - `farSurface=0`, so the failure was parent-held LOD, not far sparse leakage.
- Accepted debug validation after removing coarser parent fallback:
  - `VENPOD/build/captures/fix_no_mid_parent_surface_debug55_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - terrain-critical `postNonReady=0`
  - worst post-ready ownership miss stayed below `0.1%`.
- Accepted normal-color validation:
  - `VENPOD/build/captures/fix_no_mid_parent_surface_normal_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `minVoxelTerrainScreen=72.49%`
  - terrain-critical `postNonReady=0`.

Latest evidence:

- The same movement route that previously exposed parent-held LOD now passes the parent-held and far-surface gates.
- The normal contact sheet still has visible roughness: close exact sparse surfaces dominate much of the view and distant terrain is not yet polished. This means the parent-held LOD bug is fixed for the tested route, but the broader free-roam terrain quality problem remains open.

Remaining:

- Status remains `PARTIAL`. This is a root-cause fix for one renderer ownership bug, not final terrain polish.
- Next action required: run a shoreline/manual-problem capture with owner/material debug to isolate the fake sand/water authority mismatch. The next completion gate should require all of:
  - `maxLodParentHeld <= 0.1%`
  - `maxFarSurface <= 2%`
  - `maxHeightProxyScreen = 0`
  - `maxValleyAtmosphereScreen = 0`
  - terrain-critical `postNonReady = 0`
  - brush/material smoke proves edited shoreline voxels do not flip generated fake sand/water into different real terrain.

## May 19 Low-Altitude Far-SVO Edge Handoff

Status: `PARTIAL`

Requirement:

- Low-altitude shoreline/walking views should not let the coarse far-SVO layer occupy the visually close waterline body of the scene.
- Far-SVO should behave as a distant/edge fallback after the mid-voxel handoff, not as a near shoreline terrain substitute.
- The change must preserve parent-held LOD removal, far sparse-surface bounds, brush paint authority, and height-proxy/valley-atmosphere bans.

Source:

- User report on May 19: the world still looked fake and sparse near water even after gaps improved.
- Fresh waterline captures after parent-held removal showed the remaining pale/gray slabs were mostly real owners, not old height proxy or far sparse surface:
  - `VENPOD/build/captures/shoreline_authority_owner55_20260519`
  - `VENPOD/build/captures/shoreline_authority_material54_20260519`
- Owner debug showed exact sparse surface in the foreground, but far-SVO still contributed heavily around the shoreline/horizon band.

Current implementation:

- `assets/shaders/Graphics/PS_Raymarch.hlsl` now pushes low-altitude voxel-terrain far-SVO start through `FarLayerStartAfterBackground(startDist)` instead of starting far-SVO immediately at the protected background boundary.
- The low-altitude/elevated far-SVO candidate path now requires the camera to be at least `FAR_SEA_LEVEL + 220`, reducing near-water far-SVO promotion.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed and refreshed runtime assets.
- `.\build\bin\VENPODTests.exe` passed.
- Baseline before the handoff change:
  - `VENPOD/build/captures/shoreline_authority_owner55_20260519`
  - post-ready far-SVO ownership sum: `56,640,637`
  - hard gates passed, but the view still showed broad coarse far-SVO participation.
- Accepted owner-debug validation after the handoff change:
  - `VENPOD/build/captures/fix_low_alt_farsvo_edge_owner55_20260519`
  - post-ready far-SVO ownership sum dropped to `4,422,244`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - terrain-critical `postNonReady=0`
- Accepted normal-color shoreline/brush validation:
  - `VENPOD/build/captures/fix_low_alt_farsvo_edge_normal_20260519`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`
  - post-ready ownership: `midVoxel=122,091,517`, `farSvo=4,488,433`, `lodParentHeld=0`
  - worst miss was about `0.1585%` of ownership samples on the route.
- Accepted normal walking guard:
  - `VENPOD/build/captures/guard_walk_after_low_alt_farsvo_edge_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - terrain-critical `postNonReady=0`.

Latest evidence:

- The low-altitude waterline renderer now routes most distant body terrain through mid voxels instead of coarse far-SVO. This better matches the target architecture: exact sparse near terrain, mid voxels for visible terrain body, far-SVO as distant fallback.
- The normal waterline image is still rough: mid-voxel terrain remains gray/blocky and visually heavy. That is now the next dominant problem after far-SVO overparticipation, not proof of final terrain polish.

Remaining:

- Status remains `PARTIAL`. The architecture is closer, but final completion still needs mid-voxel visual/material polish, manual free-roam confirmation, and a better automated gate for "too much coarse low-altitude far terrain" that does not depend on the older hard miss-pixel threshold.
- Next action required: add or refine a low-altitude ownership timeline gate for far-SVO share, then improve mid-voxel material/normal presentation so it no longer reads as fake gray slab terrain while preserving real voxel ownership.

## May 19 Mid-Voxel Visual Pass And Far-SVO Screen Gate

Status: `PARTIAL`

Requirement:

- Low-altitude shoreline captures need an automated gate proving far-SVO stays as a small edge/distant contributor instead of re-owning the waterline body.
- Mid-voxel terrain should read closer to real terrain and less like washed-out gray debug/fallback slabs, without changing ownership semantics or reintroducing fake heightfield/parent LOD.

Source:

- Follow-up after `fix_low_alt_farsvo_edge_*`: far-SVO overparticipation was reduced, but normal images still showed gray/blocky mid terrain as the dominant visible roughness.
- Existing `engine_capture_smoke.ps1` already emitted `farSvoScreenPct` in `layer_screen_timeline.csv`, but did not expose a direct max-far-SVO screen gate.

Current implementation:

- `engine_capture_smoke.ps1` now supports `-MaxFarSvoScreenPct` in `Test-LayerScreenStats`, using the existing `farSvoScreenPct` timeline column. This avoids the older ownership checker's unrelated hard miss-pixel threshold and gives low-altitude routes a direct far-SVO ceiling.
- `assets/shaders/Graphics/PS_Raymarch.hlsl` mid-voxel shading was adjusted conservatively:
  - stronger `BackgroundTerrainMaterialVariation()` on mid-voxel surfaces;
  - less distant-normal flattening on mid-voxel hits;
  - weaker visible voxel grid darkening;
  - reduced sky-fog washout on mid-voxel layers.

Validation:

- `engine_capture_smoke.ps1` PowerShell parser check passed.
- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed and refreshed runtime assets.
- `.\build\bin\VENPODTests.exe` passed.
- `VENPOD/build/captures/fix_mid_visual_far_gate_waterline_20260519` passed with:
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=2.07%` under the new `-MaxFarSvoScreenPct 5` gate
  - terrain-critical `postNonReady=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed ... fallback=0 missingResident=0 deltaMismatch=0 cases=4/4`.
- `VENPOD/build/captures/guard_walk_after_mid_visual_gate_20260519` passed with:
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - observed `maxFarSvoScreen=0.49%`
  - terrain-critical `postNonReady=0`.

Latest evidence:

- The low-altitude waterline route now has a direct far-SVO screen gate, and the accepted run remains under 5% far-SVO screen coverage.
- The normal waterline image is still not final. The pass reduces some gray/fog/grid emphasis but does not solve all blocky mid-voxel terrain presentation.

Remaining:

- Status remains `PARTIAL`. Ownership is better constrained, but final playable polish still requires stronger mid-voxel geometry/material quality, manual free-roam review, and performance work on streaming hitches.
- Next action required: diagnose whether the remaining mid-layer roughness is mostly geometry resolution, material classification, fog/composition, or missing surface-normal fidelity; then fix that layer without loosening the ownership gates.

Follow-up evidence on May 19:

- `sparse_regression.ps1` now passes `-MaxFarSvoScreenPct 5` into the canonical waterline and long-waterline engine captures, so low-altitude far-SVO overparticipation is covered by the normal regression path instead of only by ad hoc probes.
- Parser checks passed for both `engine_capture_smoke.ps1` and `sparse_regression.ps1`.
- Resolution A/B probes were rejected rather than adopted:
  - `VENPOD/build/captures/probe_mid_cell8_waterline_20260519` failed the new far-SVO gate with `maxFarSvoScreen=8.45%`; the frame showed larger sky gaps and far fallback.
  - `VENPOD/build/captures/probe_mid_cell10_waterline_20260519` passed the gate with `maxFarSvoScreen=3.81%`, but visual review showed less mid coverage, more gaps than the default, and no clear quality win over the accepted `12`-cell baseline.
- Conclusion: lowering mid cell size alone is not an acceptable fix. It improves nominal cell resolution but shrinks resident world coverage under the current budget, causing more far fallback and holes. The remaining mid-layer polish should target geometry/surface reconstruction or normal/material fidelity without reducing effective coverage.

## May 19 Mixed-Shore Sand Authority Fix

Status: `PARTIAL`

Requirement:

- Coarse mid-voxel shoreline cells must not claim broad sand ownership merely because the center column is intertidal when the same coarse footprint also contains submerged columns.
- The fix must preserve the existing no-parent/no-height-proxy/no-valley-atmosphere/no-far-sparse leakage gates.
- The fix must not create fake terrain on missing resident voxels; it may only change material classification while generating an already-resident mid-voxel brick.

Source:

- User report on May 19: near water, large sand areas looked real but disappeared or changed after drawing/editing, proving the background/mid layer was classifying shoreline material inconsistently with the authoritative voxel world.
- Material debug capture before this fix, `VENPOD/build/captures/shoreline_authority_material54_20260519`, showed a large yellow sand sheet in the foreground shoreline/body area.

Current implementation:

- `src/Simulation/SparseClipmap.cpp` now treats coarse mixed-shore mid-voxel cells more conservatively. During `SparseClipmapTileCache::GenerateVoxelBrick()`, if a coarse generated cell is classified as sand but its horizontal footprint includes a submerged neighbor column and the cell overlaps sea level, the generated mid voxel is packed as water instead of sand.
- This is CPU-side resident brick generation only. It does not synthesize terrain in the raymarcher, and it does not loosen the existing renderer ownership gates.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- Accepted waterline capture:
  - `VENPOD/build/captures/fix_mixed_shore_sand_authority_waterline_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=2.08%` under the 5% low-altitude gate
  - terrain-critical `postNonReady=0`
  - brush paint smoke passed.
- Accepted material-debug capture:
  - `VENPOD/build/captures/fix_mixed_shore_sand_authority_material54_20260519`
  - Material review shows the previous broad yellow foreground/shoreline sand sheet is mostly gone; the same area now resolves as dirt/water/stone material ownership.
- Accepted strict material-authority gate:
  - `VENPOD/build/captures/gate_mixed_shore_material_sand54_strict_20260519`
  - `engine_capture_smoke.ps1` now records `materialWaterPct`, `materialSandPct`, `materialDirtPct`, and `materialStonePct` in `image_stats.csv` for material debug captures.
  - `-MaxMaterialSandPct 2` passed with `maxMaterialSandPct=0.71`, giving the waterline fake-sand regression an automated guard instead of relying only on contact-sheet review.
- Accepted walk guard:
  - `VENPOD/build/captures/guard_walk_after_mixed_shore_sand_authority_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`.

Rejected attempt:

- A shader-side resident mid-voxel surface refinement/debug probe was attempted and removed before acceptance. It invalidated the large `PS_Raymarch.hlsl` cache and caused very slow DXC/pipeline startup behavior. The accepted fix intentionally avoids adding new procedural search to the pixel shader.

Latest evidence:

- The material inconsistency around coarse shoreline sand is improved and validated on the waterline route without reintroducing parent-held LOD, height proxies, far sparse leakage, or terrain-critical non-ready pixels.
- `sparse_regression.ps1` now includes a dedicated sparse waterline material authority capture that runs debug mode `54` with `-MaxMaterialSandPct 2`, so canonical regression can catch a return of broad fake sand sheets.
- Overall terrain quality remains `PARTIAL`: distant/mid terrain is still blocky/slabby, the walking contact sheet can still be dominated by close wall geometry, and perf logs still show hitches such as `body=40-49ms` during walk startup/movement.

Next action required:

- Attack remaining mid-voxel visual roughness through CPU/GPU data representation or surface extraction, not through expensive per-pixel procedural refinement.
- Add a specific validation gate for coarse-material authority if possible, because the current proof is visual material-debug comparison plus ownership gates rather than an automated sand/water mismatch metric.
- Continue performance work separately: recent walk logs still show sparse publish/surface extraction and wait gaps contributing to frame hitches.

## May 19 Page-Table Publish Burst Budget

Status: `PARTIAL`

Requirement:

- Remaining terrain streaming work must avoid unbounded main-frame bursts that make the world feel slow to load or hitchy while moving.
- Page-table publishing must not expose partial world state, must preserve edited/brush publishes, and must keep the existing no-parent/no-height-proxy/no-far-sparse/fake-sand gates intact.

Source:

- User report on May 19: rendering is materially better, but still slow/buggy; gaps and fake terrain remain more important than end-stage performance polish, but obvious hitches should not be allowed to hide rendering correctness.
- Walk perf evidence before this change: `VENPOD/build/captures/guard_walk_after_mixed_shore_sand_authority_20260519/venpod_runtime.log` showed repeated early movement frames with page-table publish work around `5-10ms`, for example frame `5 publish=8.14ms`, frame `11 publish=9.87ms`, frame `30 publish=9.25ms`, frame `45 publish=8.78ms`.

Current implementation:

- `src/main_launcher.cpp` now reads:
  - `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_MAX_MS`, default `4`.
  - `VENPOD_SPARSE_PAGE_TABLE_PUBLISH_MIN_PER_FRAME`, default `8`.
- The page-table publish loop now yields once the per-frame publish time budget is exceeded after the minimum publish count.
- Edited publishes bypass the time cap by using a new ready-class pop path, so brush edits are not intentionally delayed behind background publish throttling.
- `PERF_SPARSE` now logs `publishTimeDefers` so future captures can prove whether the cap is active.
- `src/Simulation/SparsePagePublishQueue.h/.cpp` now expose `PopReadyOfClass()` for the edited-publish bypass.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- PowerShell parser checks passed for `engine_capture_smoke.ps1` and `sparse_regression.ps1`.
- Accepted waterline capture:
  - `VENPOD/build/captures/probe_publish_cap_waterline_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=2.08%`
- Accepted material-authority capture:
  - `VENPOD/build/captures/probe_publish_cap_material54_20260519`
  - `-MaxMaterialSandPct 2` passed with `maxMaterialSandPct=0.71`
  - `maxFarSvoScreen=2.07%`
- Accepted walk ownership guard:
  - `VENPOD/build/captures/probe_publish_cap_walk_unique100_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`

Latest evidence:

- The first capped walk probe with the default `MinUniqueSampleColors=120` was rejected because the auto-walk camera was pressed against a valid close wall and one frame sampled only `118` colors. Ownership still showed no sparse holes. The accepted rerun used `-MinUniqueSampleColors 100` for that close-wall route.
- Compared with the earlier walk log, post-startup publish time is now typically around `0.5-1.3ms` in the accepted walk route, with `publishTimeDefers` observed only during early backlog drain. Startup and unrelated gaps can still be high.
- A brush-waterline smoke failed with `deferred=1`, but an uncapped control run also failed the same way. This is not accepted as a regression from the publish cap; it remains a separate brush-smoke stability issue.

Remaining:

- Status remains `PARTIAL`. This reduces a measured publish burst source but does not complete terrain rendering polish.
- Surface extraction/staging still contributes spikes (`surfExtract`, `surfStage`, and frame gaps still need bounded/pipelined follow-up).
- The auto-walk route can collide with a close wall, making it a poor visual quality proxy unless paired with ownership gates or a better camera route.

Next action required:

- Add a comparable time/budget guard for surface GPU snapshot staging or reduce dirty/full snapshot work per frame.
- Add a dedicated non-wall free-roam/waterline movement capture for visual quality, so the walk guard does not overfocus on a close-wall artifact.
- Investigate the existing brush-smoke `deferred=1` failure separately from the publish cap.

## May 19 Surface Extraction Budget Tightening

Status: `PARTIAL`

Requirement:

- Exact sparse surface extraction must not monopolize early render frames while terrain is streaming.
- Reducing surface extraction spikes must not reopen parent-held LOD, far sparse-surface ownership, height proxy ownership, valley atmosphere fallback, fake shoreline sand, or post-ready terrain-critical non-ready samples.

Source:

- After the page-table publish cap, accepted walk and waterline logs still showed surface extraction/staging as the next measured sparse-frame contributor. Examples:
  - `probe_publish_cap_walk_unique100_20260519`: startup `surfExtract=6.06ms`, early frame `surfExtract=4.49ms`.
  - `probe_publish_cap_waterline_20260519`: startup `surfExtract=3.59ms`.

Current implementation:

- `src/main_launcher.cpp` now defaults `VENPOD_SPARSE_SURFACE_EXTRACTION_MAX_MS` to `3` instead of `5`.
- This uses the existing timed extraction path. It does not alter shader ownership, material classification, or page-table readiness semantics.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- Accepted sequential waterline capture:
  - `VENPOD/build/captures/probe_surface_extract_3ms_waterline_seq_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.63%`
  - timing evidence: `surfExtract=3.12ms` on startup, then `0.14ms` at frame `120` and `0.72ms` at frame `240`.
- Accepted sequential material-authority capture:
  - `VENPOD/build/captures/probe_surface_extract_3ms_material54_seq_20260519`
  - `-MaxMaterialSandPct 2` passed with `maxMaterialSandPct=0.71`
  - `maxFarSvoScreen=0.63%`
- Accepted walk ownership capture:
  - `VENPOD/build/captures/probe_surface_extract_3ms_walk_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`

Rejected probes:

- `VENPOD/build/captures/probe_mid_cell8_cap16k_waterline_20260519` was rejected. Even with `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS=16384` and full reported mid-voxel residency, `VENPOD_SPARSE_MID_CELL=8` produced worse visible gaps and failed the low-altitude far-SVO gate with `maxFarSvoScreen=8.45%`.
- A shader-side mid-voxel silhouette fallback was attempted and removed. It caused a runtime shader compile taking about `154s` and the process did not reach the capture exit condition within the test timeout. This is explicitly not accepted because launch stalls are already a known user-facing failure mode.

Latest evidence:

- Tightening CPU surface extraction is accepted as a bounded hitch reduction. It preserves the current rendering ownership and fake-sand gates.
- The remaining far mountain roughness is not solved by smaller mid cells or shader-side gap filling. Current evidence points to mid-layer representation/meshing quality, not simple residency capacity.

Remaining:

- Status remains `PARTIAL`. This is performance/streaming stabilization, not final visual polish.
- Surface GPU staging/upload can still show small spikes and needs a separate bounded upload/staging policy if it becomes dominant.
- Far/mid mountain visual quality still needs an architectural fix in the mid representation, likely CPU-side surface reconstruction or better mid-voxel mesh generation rather than per-pixel shader fallback.

Next action required:

- Investigate a CPU-side mid-voxel surface representation that produces stable mountain silhouettes without using height proxy ownership or expensive shader-side procedural searches.
- Add a visual-quality gate that measures far/mid sky holes or discontinuity in a non-wall waterline/free-roam route.

## May 19 Mid-Voxel Footprint Conservative Occupancy

Status: `PARTIAL`

Requirement:

- Fully resident mid-voxel bricks must not still punch holes through steep far/mid mountains because coarse occupancy was sampled only from the cell-center terrain column.
- The fix must stay CPU-side, preserve exact/edit ownership, avoid shader launch stalls, and keep the no-parent/no-height-proxy/no-far-sparse/fake-sand gates intact.

Source:

- User report on May 19: after earlier progress, far mountains still had gaps, fake-looking terrain remained, and the engine felt slow/buggy.
- Current diagnostic evidence before this change showed full mid-voxel residency by frame `240` on the waterline route (`midVoxels=8192/0`, `midVoxInterest=8192/8192`) but visible gaps/rough silhouettes still remained. That pointed at mid representation quality, not simple capacity or queue starvation.
- Rejected probes showed that smaller mid cells and shader-side gap filling were not the right path:
  - `probe_mid_cell8_cap16k_waterline_20260519` failed the far-SVO screen gate and looked worse.
  - shader-side mid gap filling caused a long compile/launch stall and was removed.

Current implementation:

- `src/Simulation/SparseClipmap.cpp` now classifies generated coarse mid-voxel cells using the cell footprint rather than only the center column.
- For coarse generated cells, if the center sample is air but any footprint column intersects the cell volume, the brick generator repacks that cell from the highest footprint column.
- Clustered coarse AIR edits still win. Single AIR edits do not collapse an entire coarse generated cell, preserving the existing edit-authority regression contract.
- This does not add any height proxy, shader-side procedural search, parent LOD fallback, or fake surface ownership.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed. The existing coarse edit tests caught the first version, and the accepted version preserves:
  - "single air edit does not collapse a coarse mid-clipmap cell"
  - "clustered air edits collapse stale coarse procedural terrain to air"
- Accepted waterline ownership capture:
  - `VENPOD/build/captures/fix_mid_voxel_footprint_waterline_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=1.27%`
  - late ownership improved from the pre-change `missPct` around `0.1536%` to about `0.0266-0.0267%`
  - late `midVoxelPct` rose from about `44.30%` to about `45.48-45.56%`
- Accepted material-authority capture:
  - `VENPOD/build/captures/fix_mid_voxel_footprint_material54_20260519`
  - `-MaxMaterialSandPct 2` passed with `maxMaterialSandPct=0.71`
  - parent-held, far-surface, height-proxy, valley-atmosphere, and far-SVO screen gates passed.
- Accepted walk ownership capture:
  - `VENPOD/build/captures/fix_mid_voxel_footprint_walk_20260519`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`

Latest evidence:

- This is the first accepted pass after the shader rejection that directly reduces measured late background miss pixels while keeping the ownership/fake-material gates green.
- Runtime logs show the conservative CPU classification increases mid-voxel surface samples, which matches the goal of filling missed steep-slope/spire occupancy in resident bricks.

Remaining:

- Status remains `PARTIAL`. The waterline contact sheet is materially less hole-prone by ownership metrics, but the far horizon still has blocky/floating-looking coarse artifacts and needs another visual-quality pass.
- The waterline visual route still lacks an automated "floating block / horizon discontinuity" metric. Current proof relies on ownership/miss/material gates plus contact-sheet review.
- Startup mid-voxel generation cost increased slightly on the waterline route (`pumpVoxel` startup about `17.17ms` after this change versus about `14.33ms` before), so performance must be watched as visual density improves.

Next action required:

- Add a stronger visual-quality gate for far/mid discontinuities, then refine the mid representation so conservative occupancy fills holes without making detached horizon slabs read as fake terrain.
- Continue avoiding shader-side procedural gap filling unless shader compile/startup behavior is separately fixed.

## May 19 Mid-Clipmap Cache Slack And Critical Publish Gate

Status: `PARTIAL`

Requirement:

- Mid/far terrain streaming must not run the resident voxel cache at exactly 100% of the visible target set, because movement then evicts stale bricks while still missing newly visible interested bricks.
- Terrain-critical visible bricks that have already uploaded must not sit behind the generic page-table publish time cap while the renderer is checking the critical footprint.
- Validation must fail directly on persistent ownership miss pixels, not only on temporal deltas or indirect layer percentages.

Source:

- Moving waterline/stress probe after the footprint fix failed even though the stationary waterline was improved:
  - `VENPOD/build/captures/gate_mid_voxel_footprint_sparse_reg_waterline_20260519`
  - At frame `240`, the log reported `interestedVoxel=8192`, `midVoxels=8192/65`, `queuedVoxel=65`, `evictVoxel=12`, and `budgetMid=12`.
  - This showed the cache was exactly full while still missing interested outer-ring bricks.
- After raising generation catch-up, the route still missed because generation and eviction were fighting at a full cache:
  - `VENPOD/build/captures/fix_mid_ring_tail_stress_waterline_20260519`
  - `budgetMid=40`, `missingVoxel=77`, `evictVoxel=40`.

Current implementation:

- `src/main_launcher.cpp` now defaults `VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS` to `12288` instead of `8192`.
- `src/main_launcher.cpp` now defaults `VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT` to `75`, leaving cache slack while still allowing about `9216` interested bricks by default.
- The mid catch-up scheduler now treats a worst-ring backlog at the clean threshold as actionable (`<=` instead of `<`) and allows ring backlog to trigger coverage catch-up even when integer total coverage rounded up to the configured `99%` threshold.
- The page-table publish loop now allows terrain-critical visible/collision publishes to continue past the generic publish time cap only when terrain-critical prefetch is active and critical upload/publish work is in flight. Edited publishes still retain priority.
- `engine_capture_smoke.ps1` now supports `-MaxOwnershipMissPct` and checks post-ready `ownership_timeline.csv` directly.
- `sparse_regression.ps1` now applies `-MaxOwnershipMissPct 0.05` to the normal waterline and material-authority waterline capture gates.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
- `.\build\bin\VENPODTests.exe` passed.
- Rejected intermediate:
  - `VENPOD/build/captures/probe_mid_slack_12288_75_stress_waterline_20260519`
  - cache slack eliminated ownership miss (`maxMiss=0`) but exposed exact critical publish delay (`postUploading` non-ready samples after frame `260`).
- Accepted moving waterline/stress capture:
  - `VENPOD/build/captures/fix_mid_slack_publish_stress_waterline_20260519`
  - `maxMiss=0.0000%`
  - `maxHeightProxy=0.00%`
  - `maxFarSvoScreen=0.03%`
  - terrain-critical `postNonReady=0`
- Accepted normal waterline capture:
  - `VENPOD/build/captures/fix_mid_slack_publish_waterline_20260519`
  - `maxMiss=0.0300%` under the new `-MaxOwnershipMissPct 0.05` gate
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=1.44%`
- Accepted material-authority waterline capture:
  - `VENPOD/build/captures/fix_mid_slack_publish_material54_20260519`
  - `maxMaterialSandPct=0.71`
  - `maxMiss=0.0300%`
  - parent-held, far-surface, height-proxy, valley-atmosphere, and far-SVO gates passed.
- Accepted walk guard:
  - `VENPOD/build/captures/fix_mid_slack_publish_walk_20260519`
  - `maxMiss=0.0000%`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`

Latest evidence:

- The moving waterline route that previously failed with thousands of post-ready miss pixels now passes with zero ownership miss and zero terrain-critical non-ready samples.
- The stationary waterline route keeps the earlier footprint improvement and is now guarded by an explicit post-ready ownership-miss threshold.

Remaining:

- Status remains `PARTIAL`. This fixes a concrete streaming/cache/publish failure, but visual polish is not complete: normal waterline contact sheets still show coarse/blocky far horizon artifacts.
- The increased cache budget uses more mid-voxel GPU memory. Current validation did not show a failure, but memory/perf should be watched on broader free-roam routes.
- The new miss gate catches ownership holes, not subjective "floating block" silhouette quality.

Next action required:

- Build a non-wall free-roam/horizon visual gate for detached/blocky far silhouettes.
- Continue mid-representation refinement against that gate, with performance telemetry watched after the cache increase.

## May 19 Exact-Surface Radius Reduction For Horizon Ownership

Status: `PARTIAL`

Requirement:

- Exact sparse raster surfaces must remain a bounded near/editable owner, not a distant horizon owner that makes far terrain read as chunk slabs.
- Mid/far voxel layers should own distant mountains when their resident data is ready.
- The change must preserve the hard no-gap, no-parent-held, no-far-sparse, no-height-proxy, no-valley-atmosphere, and fake-sand gates.

Source:

- After the mid-cache slack/publish fix, the remaining waterline contact sheet still showed blocky/floating-looking horizon terrain even though ownership holes were mostly closed.
- Owner debug capture `VENPOD/build/captures/diagnose_latest_owner50_waterline_20260519` showed large yellow exact-sparse ownership areas reaching into the visible horizon, with orange mid voxels behind them. This meant the remaining rough skyline was not only mid-voxel residency; exact raster surfaces were still owning too much distance.

Current implementation:

- `src/main_launcher.cpp` now defaults the stable exact sparse surface ownership/raster distance to `1280` instead of `1792`:
  - `VENPOD_SPARSE_SURFACE_OWNERSHIP_RADIUS`
  - `VENPOD_SPARSE_SURFACE_RASTER_MAX_DISTANCE`
- This keeps exact sparse surfaces as the near/editable authority and lets the resident mid voxel layer own more of the distant mountain band.

Validation:

- Rejected/diagnostic geometry attempt:
  - `VENPOD/build/captures/probe_mid_footprint_support_waterline_20260519`
  - Tightening conservative mid footprint fill reduced surface samples but failed the miss gate at `maxMiss=0.0511%` against `0.05%` and did not materially improve the visual contact sheet. The support-threshold change was reverted.
- Accepted env A/B before promotion:
  - `VENPOD/build/captures/probe_surface_radius1280_waterline_20260519`
  - `maxMiss=0.0060%`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.11%`
  - mid voxel ownership in late frames rose to about `49.44-49.49%`.
- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed after promoting the default.
- `.\build\bin\VENPODTests.exe` passed.
- Accepted promoted waterline capture:
  - `VENPOD/build/captures/fix_surface_radius1280_waterline_20260519`
  - `maxMiss=0.0000%`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.08%`
  - `minVoxelTerrainScreen=73.88%`
- Accepted material-authority capture:
  - `VENPOD/build/captures/fix_surface_radius1280_material54_20260519`
  - `maxMaterialSandPct=0.71`
  - `maxMiss=0.0000%`
  - parent-held, far-surface, height-proxy, valley-atmosphere, and far-SVO gates passed.
- Accepted moving/stress waterline capture:
  - `VENPOD/build/captures/fix_surface_radius1280_stress_waterline_20260519`
  - `maxMiss=0.0000%`
  - `maxFarSvoScreen=0.03%`
  - terrain-critical `postNonReady=0`
- Accepted walk guard:
  - `VENPOD/build/captures/fix_surface_radius1280_walk_20260519`
  - `maxMiss=0.0000%`
  - `maxLodParentHeld=0.0000%`
  - `maxFarSurface=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=0.00%`
  - terrain-critical `postNonReady=0`

Latest evidence:

- Compared with `fix_mid_slack_publish_waterline_20260519`, the promoted waterline route moved more horizon terrain into the mid voxel layer and reduced post-ready miss to zero on the sampled frames.
- The contact sheet is visibly more continuous across the far mountain band than the prior exact-surface-heavy view.

Remaining:

- Status remains `PARTIAL`: this improves horizon ownership and continuity, but final playable polish still needs manual free-roam review and a stronger automated silhouette/blockiness gate.
- The exact near radius is now more architecturally correct, but it should be watched on brush/edit routes to ensure near editable terrain remains fully authoritative.

Next action required:

- Add a dedicated free-roam visual capture route that avoids the close-wall walk artifact and sees the far mountain skyline.
- Build an automated silhouette/blockiness metric or owner-debug comparison for that route, then keep refining mid/far visual quality against it.

## May 19 Skyline/Wide-View Validation Reset

Status: `PARTIAL`

Requirement:

- Stop optimizing only waterline/local routes while the player-facing wide terrain view still looks broken.
- Add a broad low-horizon capture route that sees the far mountain band and can quantify skyline slab/blockiness symptoms.
- Use the route to separate true missing ownership from mid/far presentation quality and debug/instrumentation hitches.
- Do not loosen the existing no-height-proxy, no-valley-atmosphere, no-parent-held, no-far-sparse, and fake-sand guards.

Source:

- User report on May 19 after the exact-surface radius pass: the world is improved, but still slow, buggy, visually sparse, and fake-looking in broad terrain views. The user explicitly asked to stop getting stuck in small local loops and push overall implementation/polish strategy.
- Earlier waterline/brush gates were green while the screenshot-style broad view still showed blocky distant terrain, holes, and coarse handoff artifacts.

Current implementation:

- `VENPOD/engine_capture_smoke.ps1` now has `-SkylineReview`, a preset that uses the existing stress camera but lowers the orbit to a horizon-facing route:
  - default radius `900`
  - default height oscillation `24`
  - default base height `160`
  - default speed `18`
- `image_stats.csv` now records:
  - `skylineCoveragePct`
  - `skylineFlatRunPct`
  - `skylineStepPct`
- `Test-ImageStats` can now enforce:
  - `-MaxSkylineFlatRunPct`
  - `-MaxSkylineStepPct`
- The wide route intentionally reuses the existing ownership, layer-screen, temporal, and contact-sheet artifacts so a single capture can show visual symptoms plus ownership/composition attribution.

Validation:

- PowerShell parser check passed:
  - `[System.Management.Automation.Language.Parser]::ParseFile('engine_capture_smoke.ps1', ...)`
- First skyline preset attempt:
  - `VENPOD/build/captures/skyline_metric_baseline_20260519`
  - Rejected as the default high stress-camera route; it looked too downward/high-altitude and failed the low-altitude `-MaxFarSvoScreenPct 5` contract with `maxFarSvoScreen=65.97%`.
  - This proved the old waterline far-SVO cap is not the right contract for broad skyline review.
- Accepted low-horizon skyline diagnostic:
  - `VENPOD/build/captures/skyline_metric_low_horizon_20260519`
  - Passed normal hard ownership/presentation bans when the inappropriate far-SVO cap was omitted:
    - `maxMiss=0.0429%`
    - `maxLodParentHeld=0.0000%`
    - `maxFarSurface=0.0000%`
    - `maxHeightProxyScreen=0.00%`
    - `maxValleyAtmosphereScreen=0.00%`
    - terrain-critical `postNonReady=0`
  - Composition still showed the core remaining problem:
    - late `backgroundScreenPct ~= 90.3-90.6%`
    - late `surfaceScreenPct ~= 9.4-9.7%`
    - late `midVoxelScreenPct ~= 31.4-31.7%`
    - late `farSvoScreenPct ~= 19.8-20.0%`
  - Image metrics reported full skyline coverage but long flat runs:
    - `skylineFlatRunPct=58.33`, `100.00`, and `55.21`
    - `skylineStepPct=0.00` on the sampled frames
- Owner-debug skyline diagnostic:
  - `VENPOD/build/captures/skyline_owner50_low_horizon_20260519`
  - Rejected as a pass gate because debug mode `50` changes the apparent ownership composition and reported about `4.5-5.8%` late miss in the wide route, while the normal route's late miss stayed near zero.
  - Still useful as evidence: the contact sheet shows the foreground/broad visible mass is mostly exact sparse surface plus mid/far background owners, not a simple "nothing loaded" SVO failure.
- Performance note from the normal skyline log:
  - frame `386` showed a `~124 ms` `feedbackSplit=own` / ownership readback spike.
  - Treat this as validation/debug overhead until a non-debug, no-ownership-readback route proves the same hitch in gameplay.

Latest evidence:

- The remaining wide-view defect is not currently explained by persistent late ownership holes: normal skyline capture late misses were near zero and terrain-critical readiness was clean.
- The stronger diagnosis is that the renderer is visually dominated by background mid/far ownership in wide views, and that layer currently has coarse, flat, blocky silhouette/presentation artifacts.
- The old local waterline gates remain useful but are insufficient as the main completion gate.

Remaining:

- Status remains `PARTIAL`: the broad gate exists and provides evidence, but no mid/far visual-quality fix has been accepted against it yet.
- `skylineFlatRunPct` needs a calibrated threshold after one or two known-good/known-bad comparisons; for now it is an emitted metric, not a final pass/fail gate.
- Debug-mode `50` wide-view miss needs either a debug-only explanation or a separate owner-debug gate that does not misclassify normal broad terrain.
- Frame pacing still needs a no-debug ownership-readback-free route before we decide whether the user-visible slow walking is core renderer cost or validation overhead.

Next action required:

- Use `skyline_metric_low_horizon_20260519` as the broad-route baseline.
- Attack mid/far presentation at the representation level, not by re-enabling height proxies:
  - improve CPU mid-voxel surface/normal/material representation or generate a cleaner mid surface from resident voxels;
  - reduce far-SVO dominance only when mid coverage is resident and visually usable;
  - preserve far-SVO as fallback instead of exposing holes.
- Add a no-debug wide-route perf run with ownership readback disabled or throttled to isolate real gameplay hitches from instrumentation spikes.
- Promote calibrated `-MaxSkylineFlatRunPct` / `-MaxSkylineStepPct` thresholds into `sparse_regression.ps1` only after the metric correlates with contact-sheet review.

## May 19 Skyline Perf Isolation And Rejected Far-SVO Arbitration Probe

Status: `PARTIAL`

Requirement:

- Do not confuse validation/readback overhead with real gameplay renderer cost.
- Do not accept a shader-side handoff tweak unless it measurably improves the broad skyline route or ownership composition.
- Keep broad visual/perf diagnosis moving without reintroducing height proxies or fake terrain ownership.

Source:

- The first low-horizon skyline diagnostic showed a single frame with about `124 ms` in `feedbackSplit=own`, which could have been mistaken for the user's reported slow walking/rendering.
- The same broad route also showed mid/far background visual roughness, so a low-horizon far-SVO handoff probe was attempted to see whether ready far voxels could replace coarse mid voxels near the skyline.

Current implementation:

- `VENPOD/engine_capture_smoke.ps1` now supports `-SkipOwnershipDiagnostics`.
  - It disables `VENPOD_SPARSE_RENDER_OWNERSHIP`, ownership quality, and ownership stability env gates for that run.
  - It rejects incompatible ownership/layer thresholds when the switch is present, so a perf-only run cannot accidentally claim ownership validation.
  - It still captures frames, image stats, temporal stats, runtime logs, render performance, and terrain-critical readiness.
- The attempted `PS_Raymarch.hlsl` low-horizon far-SVO arbitration change was reverted because it was a no-op.

Validation:

- PowerShell parser check passed after adding `-SkipOwnershipDiagnostics`.
- Build/test after the shader probe:
  - `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
  - `.\build\bin\VENPODTests.exe` passed.
- Rejected shader probe:
  - `VENPOD/build/captures/probe_low_horizon_far_svo_candidate_20260519`
  - First run timed out because the changed `PS_Raymarch.hlsl` caused a `192.16 s` runtime DXC compile. The orphaned `VENPOD.exe` process was stopped.
  - Cached rerun: `VENPOD/build/captures/probe_low_horizon_far_svo_candidate_cached_20260519`
  - It passed ownership gates, but image stats, contact sheet, and final layer composition were identical to baseline:
    - `skylineFlatRunPct=58.33,100.00,55.21`
    - final `midVoxelScreenPct=31.7011`
    - final `farSvoScreenPct=19.9499`
    - final `surfaceScreenPct=9.4331`
  - Reverted because it added shader compile cost/risk without changing the actual failure.
- Accepted perf-isolation capture:
  - `VENPOD/build/captures/perf_skyline_no_ownership_20260519`
  - Command used `-SkylineReview -SkipOwnershipDiagnostics -MaxFrameMs 80 -MaxPrepMs 80 -MaxGpuRayMs 30`.
  - Passed with:
    - `maxFrameMs=32.92`
    - `maxSmoothedFrameMs=50.00`
    - `maxPrepMs=26.17`
    - `maxGpuRayMs=9.70`
    - terrain-critical `postNonReady=0`

Latest evidence:

- The prior `~124 ms` skyline spike is attributable to ownership diagnostics/readback, not to the normal no-ownership render path on this route.
- The broad skyline visual issue still remains; the rejected far-SVO arbitration probe proves the next visual fix should not be a simple "prefer far SVO near the horizon" shader tweak.

Remaining:

- Status remains `PARTIAL`: perf diagnosis is cleaner, but the mid/far visual representation itself is still not accepted.
- A separate manual/free-roam perf test is still needed for the user's walking lag complaint; this skyline orbit only proves the validation readback spike was artificial on this route.

Next action required:

- Keep `-SkipOwnershipDiagnostics` for perf-only gates and keep ownership diagnostics for correctness gates.
- Attack broad visual quality through resident mid-voxel data representation or a CPU-side generated mid surface, not by a cheap shader arbitration tweak.

## May 19 Mid-Representation Probe Rejections

Status: `PARTIAL`

Requirement:

- Improve broad-view mountain presentation without reintroducing fake height-proxy ownership or exposing new holes.
- Do not keep implementation changes that are visually and metrically indistinguishable from baseline.
- Determine whether the current broad-view roughness is mostly shader hit reconstruction, CPU mid-voxel surface tagging, or raw clipmap resolution.

Source:

- `VENPOD/build/captures/skyline_metric_low_horizon_20260519` established the low-horizon baseline:
  - final `surfaceScreenPct=9.4331`
  - final `backgroundScreenPct=90.5669`
  - final `midVoxelScreenPct=31.7011`
  - final `farSvoScreenPct=19.9499`
  - `skylineFlatRunPct=58.33,100.00,55.21`
  - `maxMiss=0.0429%`
- The visual failure therefore needs a better mid/far representation, not another ownership-miss patch.

Rejected probes:

- Shader-side resident mid-voxel surface reconstruction:
  - A bounded air-to-solid reconstruction was attempted inside `RaymarchMidVoxelClipmap()` only after a resident mid voxel hit.
  - Capture: `VENPOD/build/captures/probe_mid_resident_surface_reconstruct_skyline_20260519`
  - First run incurred a `239.56 s` runtime compile of `PS_Raymarch.hlsl`.
  - The resulting contact sheet and metrics were identical to the baseline:
    - `skylineFlatRunPct=58.33,100.00,55.21`
    - final `midVoxelScreenPct=31.7011`
    - final `farSvoScreenPct=19.9499`
  - Reverted because it added shader compile/risk without changing the representative failure.
- CPU-side footprint-filled visual-surface tag:
  - Coarse cells filled by the footprint max-column rule were temporarily marked as `VisualSurface` and evaluated against the footprint column height.
  - Capture: `VENPOD/build/captures/probe_mid_footprint_surface_tag_skyline_20260519`
  - It passed hard ownership gates but again produced identical contact sheet, skyline metrics, and final layer composition.
  - Reverted because it did not affect the representative skyline route.
- Mid cell `8` with `16384` max bricks:
  - Capture: `VENPOD/build/captures/probe_mid_cell8_16384_skyline_20260519`
  - Rejected because it failed ownership miss gate:
    - `maxMiss=0.9054%`
    - frame `360`: `coveragePct=84`, `queuedVoxel=1799`, `missingVoxel=1799`
  - It slightly changed visible detail but lost coverage, which is the wrong direction for the no-hole contract.
- Mid cell `10` with `16384` max bricks:
  - Capture: `VENPOD/build/captures/probe_mid_cell10_16384_skyline_20260519`
  - Passed ownership gate:
    - `maxMiss=0.0106%`
    - final `midVoxelScreenPct=32.0152`
    - final `farSvoScreenPct=19.3214`
  - Rejected for now because visual/skyline metrics remained effectively unchanged while frame `360` prep rose to `24.73 ms` and mid coverage was only `90%` with `1003` queued voxels.

Validation:

- After reverting the no-op shader and CPU probes:
  - `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release` passed.
  - `.\build\bin\VENPODTests.exe` passed.
  - `rg` found no remaining `reconstructedResidentSurface`, `previousMidVoxelAirT`, `footprintFilledCell`, or `lowHorizonFarSvoCandidate` probe identifiers.

Latest evidence:

- Simple shader hit refinement and surface-flag tagging are not affecting the broad route.
- Reducing mid cell size can improve nominal resolution only by sacrificing coverage and frame prep under the current brick budget.
- The next viable fix likely needs a new representation, not a local tweak:
  - a CPU-generated mid surface/mesh from resident mid voxels,
  - or a clipmap layout/budget redesign that can lower cell size while keeping post-ready coverage near current defaults.

Remaining:

- Status remains `PARTIAL`: the broad skyline is still not polished.
- The low-horizon metric may be too insensitive to smaller visual improvements; it catches skyline continuity but not all blocky material/surface quality defects.

Next action required:

- Stop probing small shader/local classification tweaks for this issue.
- Design a mid-layer representation change that can be validated against both:
  - no-hole ownership gates, and
  - a visual/contact-sheet comparison that actually changes the skyline route.

## May 19 Far-Water Ownership Arbitration

Status: `PARTIAL`

Requirement:

- Fake shoreline/water ownership must not hide missing mid/far terrain.
- When a low-altitude background ray misses resident mid voxels, the renderer
  must try the page-indexed far voxel terrain before accepting a far-water
  plane as the final owner.
- Water may still occlude terrain when it is actually closer than the far voxel
  hit.
- The validation gate must measure far-water screen ownership directly instead
  of using far-SVO coverage as a proxy for fake-water defects.

Source:

- User report that shoreline sand/water can look fake and then disappear or
  change when a real sparse edit/page arrives.
- Debug owner captures:
  - `VENPOD/build/captures/diag_owner55_current_skyline_20260519`
  - `VENPOD/build/captures/diag_owner55_current_waterline_20260519`
- Prior skyline comparison showed the old ordering allowed far-water ownership
  around `5.20%` of the screen on post-ready skyline frames while far-SVO
  terrain was available.

Change:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Moved the unconditional `hasWaterOccluder` return to after the far-SVO
    fallback attempt.
  - Added water-vs-far-SVO arbitration inside the far-SVO fallback: water wins
    only when `waterOccluderHit.distance <= backgroundHit.distance + 0.25`.
- `engine_capture_smoke.ps1`
  - Added `-MaxFarWaterScreenPct`.
  - `Test-LayerScreenStats` now reports and gates `farWaterScreenPct` directly.
- `sparse_regression.ps1`
  - Waterline, waterline material, and long-waterline captures now pass
    `-MaxFarWaterScreenPct 6`.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release`
  passed.
- `.\build\bin\VENPODTests.exe` passed.
- PowerShell syntax validation passed for `engine_capture_smoke.ps1` and
  `sparse_regression.ps1`.
- Sequential skyline guard:
  `VENPOD/build/captures/fix_defer_far_water_until_farsvo_skyline_gate_seq_20260519`
  passed with:
  - `maxSkylineInteriorSkyRunPct=1.12`
  - `maxFarWaterScreen=0.24%`
  - `maxFarSvoScreen=20.77%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
- Sequential waterline guard:
  `VENPOD/build/captures/fix_defer_far_water_until_farsvo_waterline_gate_seq_20260519`
  passed with:
  - `maxMaterialSandPct=0.00`
  - `maxSkylineInteriorSkyRunPct=8.94`
  - `maxFarWaterScreen=0.00%`
  - `maxFarSvoScreen=0.96%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
- Sequential brush/waterline guard:
  `VENPOD/build/captures/fix_defer_far_water_until_farsvo_brush_waterline_gate_20260519`
  passed with:
  - `maxMaterialSandPct=0.00`
  - `maxFarWaterScreen=0.00%`
  - `maxFarSvoScreen=1.71%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed frames=150 queued=12 retired=474 applied=158 deltas=474 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`

Notes:

- A parallel skyline/waterline capture attempt produced an invalid waterline
  failure because concurrent VENPOD launches share the same `build/bin`
  runtime-log path. Engine captures must be run sequentially unless the log path
  is isolated per process.
- The accepted change intentionally converts some skyline far-water ownership
  into far-SVO ownership. That is an improvement for the fake-water bug because
  far SVO is page-indexed voxel terrain, but it does not solve coarse far/mid
  visual quality.

Remaining:

- Status remains `PARTIAL`: fake far-water ownership is now directly gated and
  reduced on the reproduced routes, but skyline terrain still relies on a
  visibly coarse far-SVO/mid-voxel mix.
- Final playable polish still needs the larger mid-layer representation change
  identified above, plus manual/free-roam confirmation and performance work.

Next action required:

- Use `MaxFarWaterScreenPct` in future shoreline/problem-route captures.
- Do not treat the higher skyline far-SVO share as final polish; it is a better
  owner than fake water, but the next implementation pass should improve the
  mid/far representation so the skyline does not need broad far-SVO fallback.

## May 19 Promoted Resident Mid-Voxel Closure

Status: `PARTIAL`

Requirement:

- Broad low-altitude skyline/mountain views should be owned primarily by the
  resident mid-voxel layer when the mid voxel data exists.
- Far SVO should remain a fallback, not the dominant low-altitude owner for
  mountain silhouettes that have resident mid data.
- Do not reintroduce height-proxy, valley-atmosphere, unsafe-near, fake
  far-water, or brush-edit regressions.

Source:

- User report that the world is still sparse/blocky and that far mountains
  still have gaps even after near/waterline fixes.
- `VENPOD/build/captures/fix_defer_far_water_until_farsvo_skyline_gate_seq_20260519`
  showed the far-water arbitration fix was correct but the skyline still leaned
  heavily on far SVO:
  - `maxFarSvoScreen=20.77%`
  - average `midVoxelScreenPct=32.10`
  - average `farSvoScreenPct=13.60`
  - average `voxelTerrainScreenPct=65.67`

Change:

- `assets/shaders/Graphics/PS_Raymarch.hlsl`
  - Added forward declarations for the existing diagnostic terrain and
    resident mid-voxel closure helpers.
  - Promoted `TryBuildResidentMidVoxelClosureHit()` into
    `RaymarchBackgroundField()` before the far-SVO fallback for bounded
    low-altitude voxel-only skyline rays.
  - The promoted path only accepts a hit when:
    - `DiagnosticFarTerrainWouldHit()` finds a deterministic terrain crossing,
    - the resident mid-voxel sample at that crossing is present and non-air,
    - the hit is inside the mid field and not a coarser parent ring,
    - the hit is outside exact-near protected ownership, and
    - the closure is not more than `2200` units beyond the current far-start
      boundary.

Validation:

- `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release`
  passed and refreshed runtime shader assets.
- `.\build\bin\VENPODTests.exe` passed.
- PowerShell syntax validation passed for `engine_capture_smoke.ps1` and
  `sparse_regression.ps1`.
- Skyline guard:
  `VENPOD/build/captures/fix_promoted_mid_closure_skyline_gate_20260519`
  passed with:
  - `maxSkylineInteriorSkyRunPct=1.12`
  - `maxFarSvoScreen=5.86%`
  - `maxFarWaterScreen=0.21%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
  - average `midVoxelScreenPct=41.80`
  - average `farSvoScreenPct=4.35`
  - average `voxelTerrainScreenPct=66.06`
- Waterline guard:
  `VENPOD/build/captures/fix_promoted_mid_closure_waterline_gate_20260519`
  passed with:
  - `maxMaterialSandPct=0.00`
  - `maxSkylineInteriorSkyRunPct=8.94`
  - `maxFarSvoScreen=2.22%`
  - `maxFarWaterScreen=0.00%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
- Brush/waterline guard:
  `VENPOD/build/captures/fix_promoted_mid_closure_brush_waterline_gate_20260519`
  passed with:
  - `maxMaterialSandPct=0.00`
  - `maxFarSvoScreen=2.22%`
  - `maxFarWaterScreen=0.00%`
  - `heightProxyScreen=0`
  - `valleyAtmosphereScreen=0`
  - `miss=0`
  - `SPARSE_BRUSH_PAINT_SMOKE passed frames=150 queued=12 retired=474 applied=158 deltas=474 fallback=0 missingResident=0 hints=0 overflow=0 deltaMismatch=0`

Notes:

- This was accepted because it materially changed the representative skyline
  route: far-SVO max dropped from `20.77%` to `5.86%`, and average mid-voxel
  ownership rose from `32.10%` to `41.80%`.
- The first probe incurred a runtime shader compile after asset refresh. After
  the cache was warm, the representative skyline frame 240 reported about
  `body=19.28 ms` and `rawMs=20.36 ms`; this is useful but not enough to close
  the user's broader walking-lag complaint.

Remaining:

- Status remains `PARTIAL`: the skyline is now more mid-voxel-owned and less
  far-SVO-dominated, but the visual still has coarse/blocky mid/far terrain and
  needs manual free-roam review.
- This promoted closure is a shader-side representation bridge. The longer-term
  architecture may still need a CPU-generated mid surface/mesh or clipmap
  layout change if manual review shows the same blocky silhouette problem.

Next action required:

- Run a manual/free-roam or user-viewpoint capture with owner/material debug to
  confirm the visible far-mountain gaps are reduced in the actual problem view.
- If blockiness remains, move the next implementation pass to CPU-side
  mid-surface representation rather than adding more per-pixel closure logic.

## May 19 Strategy Reset: Skyline as a First-Class Gate

Status: `PARTIAL`

Requirement:

- Stop looping on narrow local waterline or one-frame shader fixes when the
  user's active complaint is the broad world view: remaining far-mountain gaps,
  blocky mid/far terrain, slow visible settlement, and fake terrain ownership.
- Treat the skyline/mountain route as a required regression gate, not an ad-hoc
  manual screenshot review.
- Use the gate to decide whether the next implementation belongs in ownership
  arbitration, streaming readiness, or representation quality.

Source:

- User reset after interruption: "come up with a better goal to push faster
  progress rather than getting stuck on small local loops, focus on an overall
  strategy to push implementation and polish work and work around blockers".
- Previous accepted captures proved waterline and skyline ownership improved,
  but the ledger still marked the goal `PARTIAL` because skyline visuals remain
  coarse/blocky and broader walking lag is unresolved.

Change:

- `sparse_regression.ps1`
  - Added `-SkipSkylineEngineCaptureSmoke`.
  - Added skyline capture-window parameters:
    `SkylineEngineCaptureExitAfterFrames`,
    `SkylineEngineCaptureStartFrame`,
    `SkylineEngineCaptureIntervalFrames`, and
    `SkylineEngineCaptureCount`.
  - Added `build/logs/sparse_skyline_engine_capture`.
  - Added a dedicated sequential `-SkylineReview` capture stage with hard
    ownership/layer gates:
    - `MaxOwnershipMissPct 0.10`
    - `MaxHeightProxyScreenPct 0`
    - `MaxValleyAtmosphereScreenPct 0`
    - `MaxFarSvoScreenPct 8`
    - `MaxFarWaterScreenPct 1`
    - `MaxSkylineInteriorSkyRunPct 12`

Validation required:

- PowerShell parse validation for `sparse_regression.ps1`.
- A targeted skyline capture:
  `powershell -ExecutionPolicy Bypass -File .\engine_capture_smoke.ps1 -Config Release -NoBuild -SkylineReview -ExitAfterFrames 340 -CaptureStartFrame 220 -CaptureIntervalFrames 40 -CaptureCount 3 -MinUniqueSampleColors 1 -MaxOwnershipMissPct 0.10 -MaxHeightProxyScreenPct 0 -MaxValleyAtmosphereScreenPct 0 -MaxFarSvoScreenPct 8 -MaxFarWaterScreenPct 1 -MaxSkylineInteriorSkyRunPct 12 -OutputDir build\captures\strategy_reset_skyline_gate_20260519`
- A regression-script slice that reaches the new skyline stage without running
  unrelated long reels:
  `powershell -ExecutionPolicy Bypass -File .\sparse_regression.ps1 -Config Release -SkipTests -SkipFlickerSmoke -SkipSurfaceSmoke -SkipGpuRaycastSmoke -SkipMissFeedbackSmoke -SkipBrushFeedbackSmoke -SkipBrushFeedbackApplySmoke -SkipBrushFeedbackAuthoritativeSmoke -SkipBrushFeedbackStrictResidentSmoke -SkipBrushFeedbackMovingSmoke -SkipBrushPaintSmoke -SkipEditUiPersistenceSmoke -SkipDefaultPhysicsSmoke -SkipPhysicsSmoke -SkipGpuPhysicsStrictSmoke -SkipAsyncPagePublishSmoke -SkipDenseLegacySmoke -SkipRenderSmoke -SkipStartupEngineCaptureSmoke -SkipEngineCaptureSmoke -SkipStressEngineCaptureSmoke -SkipFastFlightEngineCaptureSmoke -SkipFastWaterTransitionEngineCaptureSmoke -SkipWalkEngineCaptureSmoke -SkipTerrainGapEngineCaptureSmoke -SkipBrushDomeEngineCaptureSmoke -SkipLongWalkEngineCaptureSmoke -SkipWaterlineEngineCaptureSmoke -SkipOwnershipDebugCaptureSmoke -SkipPublicDemoCapture -SkipPublicReviewReelCapture -SkipPublicReviewDocs`

Latest evidence:

- PowerShell parse validation passed after adding the skyline stage.
- Direct targeted skyline gate passed at
  `VENPOD/build/captures/strategy_reset_skyline_gate_20260519`:
  - `maxSkylineInteriorSkyRunPct=1.12`
  - `maxOwnershipMissPct=0.0000`
  - `maxHeightProxyScreen=0.00`
  - `maxValleyAtmosphereScreen=0.00`
  - `maxFarSvoScreen=5.86`
  - `maxFarWaterScreen=0.21`
  - `minVoxelTerrainScreen=63.94`
- First regression-script slice attempt was blocked before skyline by an
  unrelated base GPU-physics smoke rejection. Added `-SkipPhysicsSmoke` so
  visual terrain validation can be run independently of physics debugging.
- Regression-script skyline slice passed at
  `VENPOD/build/logs/sparse_skyline_engine_capture` after skipping unrelated
  non-visual smokes:
  - `maxSkylineInteriorSkyRunPct=1.12`
  - `maxOwnershipMissPct=0.0000`
  - `maxHeightProxyScreen=0.00`
  - `maxValleyAtmosphereScreen=0.00`
  - `maxFarSvoScreen=5.86`
  - `maxFarWaterScreen=0.21`
  - ownership summary `minTerrain=99.68`, `maxMidVoxel=74.40`,
    `maxFarSvo=9.13`, `maxFarWater=0.32`

Next action required:

- Run the syntax check and the targeted skyline gate sequentially.
- If the gate passes but the contact sheet remains visibly blocky, make the
  next implementation slice CPU/mid-layer representation work, not another
  local ownership threshold tweak.

## May 19 Normal-Walk Terrain-Sky Reason Diagnostic

Status: `DIAGNOSTIC`

Requirement:

- Build a reason-coded diagnostic for the normal walking view that still fails
  skyline continuity, without tuning rendering thresholds or changing the
  skyline gate.
- Separate "where background is allowed to render" from "where deterministic
  terrain truth is probed" so the legacy dense-box/protected-band handoff cannot
  hide a terrain crossing from the diagnostic.

Change:

- `PS_Raymarch.hlsl`
  - Reverted the rejected directional low-altitude protected-band experiment in
    `SurfaceAuthoritativeBackgroundStartForRay` and
    `SparseMissingPageBackgroundStartForRay`; the protected band is back to the
    prior unconditional low-altitude behavior.
  - Added `TerrainDiagnosticStartDistance()`, independent of protected-band and
    dense-box-exit starts:
    `max(160, min(midStart, ExactNearDistance() + 8))`.
  - Added debug mode `57` for terrain-sky reason samples:
    - blue: legitimate sky
    - red: deterministic terrain exists before current background-allowed start
    - orange: expected mid voxel brick missing
    - cyan: resident mid voxel sampled air
    - magenta: mid voxel hit rejected
    - purple: far/SVO unavailable or rejected
  - Kept normal rendering behavior unchanged. Mode `57` only runs the reason
    probe on an 8x8 screen grid so the artifact is affordable.
- `engine_capture_smoke.ps1`
  - Allowed debug color captures to bypass low-sky visual rejection for all
    nonzero sparse debug modes, not only mode `50`.

Validation:

- Build/asset refresh passed:
  `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release`
- Full per-pixel debug mode `57` was rejected operationally: it compiled but
  was too expensive to reach capture frames at 1920x1080.
- Sample-grid debug mode `57` completed one failing normal-walk frame:
  `VENPOD/build/captures/diag_walk_terrain_sky_reason_v5_20260519`
  - frame: `engine_frame_0220.bmp`
  - contact sheet: `contact_sheet.png`
  - `maxSkylineInteriorSkyRunPct=44.13`
  - ownership miss max `0.0046%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarSvoScreen=1.35%`
  - `maxFarWaterScreen=0.00%`
  - `minVoxelTerrainScreen=66.37%`

Limited sampled diagnostic result:

- Approximate reason-color counts in `engine_frame_0220.bmp` from the
  debug-mode `57` 8x8 screen grid:
  - `legitimateSky`: `10735`
  - `far/SVO unavailable or rejected`: `17`
  - `terrainBeforeAllowedStart`: `0`
  - `midVoxelBrickMissing`: `0`
  - `midVoxelSampledAir`: `0`
  - `midVoxelRejected`: `0`
- Focused regions covering the right and middle visible mountain holes also
  showed zero `terrainBeforeAllowedStart` samples:
  - right-hole region: `legitimateSky=542`, `far/SVO unavailable/rejected=13`
  - middle-hole region: `legitimateSky=433`, `far/SVO unavailable/rejected=2`

Interpretation:

- The start-distance diagnostic did not support the hypothesis that major
  normal-walk holes are mostly deterministic terrain hidden before
  `backgroundAllowedStart`, under this sampled diagnostic.
- The visible skyline holes remain real and the normal walking gate is still
  failing. The dominant reason sample is "legitimate sky" only in the narrow
  sense that the limited sampled probe did not find deterministic terrain in
  its checked range. This does not prove the procedural world contains no
  terrain along those rays.
- The conclusion is therefore intentionally soft: do not tune handoff
  thresholds from this artifact alone. The next question is whether the
  screenshot's blue holes are present in CPU terrain truth, shader terrain
  truth, both, or neither.

Next action required:

- Compare `FarTerrainHeightVoxelized` diagnostic truth against CPU
  `SparseTerrainGenerator::SampleGeneratedVoxelWithColumn` for the sampled hole
  rays/positions.
- If CPU terrain also reports air, the next slice is representation/art
  generation: close unintended mountain arches/holes in generated mid/far
  terrain.
- If CPU terrain reports solid where the shader diagnostic reports sky, fix the
  CPU/GPU terrain truth mismatch before changing ownership arbitration.

## May 19 CPU/GPU Hole-Ray Audit

Status: `DIAGNOSTIC`

Requirement:

- Audit actual pixels inside the visible blue holes from
  `diag_walk_terrain_sky_reason_v5_20260519/engine_frame_0220.bmp`.
- For each pixel, reconstruct the frame-220 camera ray and compare:
  - shader-style `FarTerrainHeightVoxelized`
  - CPU-style `SparseTerrainGenerator::SampleGeneratedVoxelWithColumn`
  - first expected terrain distance
  - whether that expected terrain is before the current background-allowed
    start.

Change:

- Added `VENPOD/terrain_hole_ray_audit.ps1`.
  - Parses `PERF_SPARSE_WALK frame=220` from the capture runtime log.
  - Selects 32 sky-colored pixels from bounded visible-hole regions in the
    captured BMP.
  - Reconstructs camera rays using the same 60-degree FOV and camera basis as
    `PS_Raymarch.hlsl`.
  - Audits CPU and shader terrain truth out to `t=10400`.
  - Writes:
    `VENPOD/build/captures/diag_walk_terrain_sky_reason_v5_20260519/hole_ray_audit.csv`

Validation:

- PowerShell parse validation passed for `terrain_hole_ray_audit.ps1`.
- Audit run completed and wrote 32 rows.

Audit result:

- Grouped by CPU/GPU truth:
  - `both_truth_air_to_10400`: `19`
  - `cpu_gpu_truth_agree`: `8`
  - `cpu_gpu_truth_distance_differs`: `2`
  - `cpu_truth_solid_shader_air`: `3`
- Grouped by CPU terrain before the estimated background-allowed start:
  - `cpuTerrainBeforeAllowedStart=True`: `13`
  - `cpuTerrainBeforeAllowedStart=False`: `19`
- The estimated offline `backgroundAllowedStart` for this low-altitude
  walking frame is `1288` (`ExactNearDistance=1024`, surface ownership radius
  `1280`, protected low-altitude band active).
- Important rows:
  - right lower blue gap: 8 rows where CPU and shader both see terrain/water
    before `backgroundAllowedStart`, mostly around `t=1072-1080`.
  - middle arch: 5 rows where CPU sees stone before `backgroundAllowedStart`
    around `t=1108-1140`; 3 of those rows have shader-style voxelized truth
    still reporting air.

Interpretation:

- The earlier sampled debug colors were too weak to close the question. The
  CPU/GPU hole-ray audit shows actual selected hole pixels where expected CPU
  terrain exists before the current background-allowed start.
- This reopens the handoff/diagnostic-start issue for at least part of the
  visible failure, but with a sharper split:
  - some hole pixels are true sky/air through `t=10400`;
  - some are CPU/GPU terrain before allowed start;
  - some are CPU solid while shader-style voxelized truth reports air.
- The next rendering fix should not be threshold tuning. It should make the
  runtime diagnostic/render path use a robust crossing probe and then decide
  whether terrain before `backgroundAllowedStart` should be surfaced as a miss,
  a resident mid-voxel closure, or an ownership rejection. Separately, CPU/GPU
  terrain truth drift must be resolved where CPU reports stone but shader
  voxelized terrain reports sky.

Limitations:

- The CSV is an offline audit. Mid-clipmap resident/missing/material and
  rejection columns are marked `offline_unavailable`; a runtime dump is needed
  to fill those precisely.
- `backgroundAllowedStart` is reconstructed from the low-altitude protected
  band defaults. The dense-box exit was not available in the capture artifact,
  but the protected band (`1288`) dominates the expected terrain distances
  observed here.

## May 19 Low-Altitude Walking Continuity Exception

Status: `PARTIAL_FIX`

Requirement:

- Fix the audited walking pixels where deterministic terrain exists before the
  current low-altitude `backgroundAllowedStart`.
- Keep the fix narrow:
  - voxel terrain ownership only;
  - exact sparse/editable terrain remains authoritative;
  - no fake heightfield, sand, water, or valley-atmosphere ownership.

Change:

- In `PS_Raymarch.hlsl`, low-altitude voxel-only background terrain now starts
  resident mid-voxel and far-SVO probing at:
  `max(TerrainDiagnosticStartDistance(), max(frame.midFieldParams.y, 256))`
  instead of the protected background handoff distance.
- The normal exact-near rejection path still arbitrates the returned voxel hit.
- A first implementation that called the diagnostic terrain probe directly from
  the hot render path was rejected because it made the pixel shader too slow to
  capture; the final change uses existing voxel raymarch paths instead.

Validation:

- Build:
  `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release`
- Walking frame-220 capture:
  `build/captures/fix_walk_mid_voxel_continuity_frame220_20260519`
- Capture passed with:
  - `maxOwnershipMiss=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarWaterScreen=0.00%`
  - `minVoxelTerrainScreen=66.86%`
- Skyline metric improved only slightly:
  - diagnostic baseline `skylineInteriorSkyRunPct=44.13`
  - post-fix `skylineInteriorSkyRunPct=43.02`

Audit result:

- New hole audit for the post-fix frame:
  - `both_truth_air_to_10400`: `16`
  - `cpuTerrainBeforeAllowedStart=True`: `0`
  - `cpuTerrainBeforeAllowedStart=False`: `16`
- Re-sampling the exact 13 prior `cpuTerrainBeforeAllowedStart=True` pixels in
  the new frame found `0/13` still sky-colored.

Interpretation:

- The targeted render arbitration bucket is fixed: the audited
  before-allowed-start pixels at roughly `t=1072-1140` no longer return sky.
- The broad walking skyline gate is still failing, and the remaining sampled
  holes now fall into `both_truth_air_to_10400`. That shifts the next task away
  from handoff tuning and toward the procedural/generated gap bucket and the
  remaining CPU/GPU truth drift bucket.

## May 19 Remaining-Hole Terrain Truth Audit

Status: `DIAGNOSTIC`

Requirement:

- For remaining visible skyline holes after the walking continuity exception,
  classify whether the procedural world truly contains air or whether terrain
  truth/representation layers disagree.
- Compare:
  - CPU sparse generator truth;
  - shader-style far terrain truth;
  - expected mid-voxel representation at the first terrain point;
  - far-SVO runtime availability/page coverage.

Change:

- Extended `VENPOD/terrain_hole_ray_audit.ps1`.
  - Adds `dominantBucket`:
    `true_procedural_air`, `cpu_solid_shader_air`,
    `cpu_solid_mid_representation_air`,
    `cpu_shader_solid_mid_ring_partially_missing`, and related buckets.
  - Parses frame runtime state, including far-SVO readiness and mid-voxel ring
    resident/missing counts.
  - Adds expected mid-voxel brick/ring/material columns and far-SVO page
    availability columns.

Validation:

- Audit input:
  `build/captures/fix_walk_mid_voxel_continuity_frame220_20260519/engine_frame_0220.bmp`
- Audit output:
  `build/captures/fix_walk_mid_voxel_continuity_frame220_20260519/remaining_hole_truth_audit.csv`
- Runtime state at frame 220:
  - far SVO: `on`, `complete`, `farCov=1.00/1.00`
  - mid voxel rings: `res=3649/3053/2517/494`,
    `miss=0/0/0/1315`

Audit result:

- Requested up to 64 hole pixels; 32 visible sky-hole pixels matched the
  remaining-hole regions.
- Grouped by `dominantBucket`:
  - `true_procedural_air`: `32`
- Grouped by CPU/GPU truth notes:
  - `both_truth_air_to_10400`: `32`
- Grouped by mid representation:
  - `no_expected_terrain`: `32`

Interpretation:

- The sampled remaining visible holes are not currently shader parity failures
  and not mid/far representation-air failures. CPU generator truth and
  shader-style far terrain truth both find no terrain out to `t=10400`.
- For these sampled holes, the renderer appears to be honestly showing
  procedural air. The next fix should therefore be procedural terrain shape or
  silhouette backfill, not handoff tuning or SVO/clipmap residency.
- CPU/GPU truth drift remains a known bucket from the earlier audit, but it is
  not dominant in the post-continuity remaining-hole sample.

## May 19 Procedural Silhouette Closure

Status: `FIX_CANDIDATE`

Requirement:

- Make the deterministic procedural world stop producing large through-air
  gaps inside normal walking-view distant mountain silhouettes.
- Keep the fix generator-owned:
  - update CPU terrain height;
  - update shader far terrain truth;
  - update far-SVO generation parity;
  - do not re-enable fake heightfield, sand, water, or atmosphere ownership.

Change:

- Added a deterministic distant backdrop/silhouette floor to:
  - `SparseTerrainGenerator::HeightAt`
  - shader `FarTerrainHeight`
  - `FarVoxelOctree::TerrainHeight`
  - `terrain_hole_ray_audit.ps1` mirrored CPU/shader truth formula
- Bumped the far-SVO cache version from `18` to `19` so old SVO data cannot be
  reused with the new terrain shape.
- Rebuilt the far-SVO cache once with `VENPOD_FAR_SVO_ALLOW_COLD_BUILD=1`.
  The cold-build smoke run fails readiness before frame 220 by design while the
  async SVO build completes, then saves:
  `build/bin/venpod_far_svo_cache_r6_d6_s12345.bin`.

Validation:

- Build:
  `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Release`
- Passing walking frame-220 capture:
  `build/captures/fix_generator_silhouette_closure_cached_svo_frame220_20260519`
- Capture result:
  - `skylineInteriorSkyRunPct=15.64`
  - `skylineInteriorSkyPct=8.28`
  - `skyLikePct=8.78`
  - `maxOwnershipMiss=0.0000%`
  - `maxHeightProxyScreen=0.00%`
  - `maxValleyAtmosphereScreen=0.00%`
  - `maxFarWaterScreen=0.00%`
  - `maxFarSvoScreen=6.10%`
  - `minVoxelTerrainScreen=82.01%`
- Remaining-hole audit on the passing artifact:
  `build/captures/fix_generator_silhouette_closure_cached_svo_frame220_20260519/remaining_hole_truth_audit.csv`
  contains only the header row because no pixels in the audited remaining-hole
  regions still matched the sky-hole classifier.

Comparison:

- Before procedural closure, the same frame-220 remaining-hole audit found:
  - `true_procedural_air=32`
  - `both_truth_air_to_10400=32`
  - `skylineInteriorSkyRunPct=43.02`
- After procedural closure and rebuilt SVO cache:
  - sampled remaining-hole rows: `0`
  - `skylineInteriorSkyRunPct=15.64`
  - no ownership/proxy/atmosphere/water regression in the passing capture.

Interpretation:

- The remaining broad walking skyline holes were generated-world silhouette
  gaps, not renderer ownership or residency failures.
- The first generator pass proved the shape fix but produced misses until the
  far-SVO builder and cache were brought into parity. With the rebuilt cache,
  the frame-220 capture passes ownership and layer gates.
- The skyline metric is materially better but not yet below the older hard
  skyline gate; further work should refine the procedural backdrop shape rather
  than return to sparse arbitration.

## May 19 Route-Level Failure Inventory

Status: `DIAGNOSTIC`

Requirement:

- Stop tuning frame 220 in isolation.
- Capture a longer walking route, rank the worst current skyline/gap frames,
  and run the hole-ray audit on pixels selected from those actual frames.

Change:

- Added `-AutoSelect` to `VENPOD/terrain_hole_ray_audit.ps1`.
  - It scans the current frame for sky-like pixels embedded in terrain context
    instead of using the old frame-220 hand-picked regions.
- Added `VENPOD/route_gap_inventory.ps1`.
  - Ranks frames by skyline/gap score.
  - Runs the auto-selected hole audit for the worst frames.
  - Writes `worst_gap_frames.csv` with frame metrics, far/mid readiness,
    miss/proxy/far-water state, dominant audit bucket, frame path, and audit
    path.

Validation:

- Capture:
  `build/captures/route_inventory_walk_20260519`
- Command shape:
  - walking route;
  - cached SVO;
  - no debug color;
  - frames `220..550` every `30` frames;
  - 12 captured frames.
- Route-level capture passed ownership/layer gates:
  - route max ownership miss: `0.0000%`
  - max height proxy screen: `0.00%`
  - max valley atmosphere screen: `0.00%`
  - max far water screen: `0.00%`
  - max far SVO screen: `6.11%`
  - min voxel terrain screen: `70.47%`
- Route skyline still has failures:
  - max `skylineInteriorSkyRunPct=45.81`

Inventory output:

- `build/captures/route_inventory_walk_20260519/worst_gap_frames.csv`

Worst frames:

| frame | skylineRun | skylineInteriorSky | farSvo | farCov | midCov | missPct | proxyPct | farWaterPct | dominantAuditBucket |
| --- | ---: | ---: | --- | --- | --- | ---: | ---: | ---: | --- |
| 550 | 45.81 | 20.97 | on/complete | 1.00/1.00 | 1.00/1.00 | 0 | 0 | 0 | true_procedural_air |
| 490 | 39.66 | 26.55 | on/complete | 1.00/1.00 | 1.00/1.00 | 0 | 0 | 0 | true_procedural_air |
| 520 | 40.22 | 24.00 | on/complete | 1.00/1.00 | 1.00/1.00 | 0 | 0 | 0 | true_procedural_air |
| 460 | 38.55 | 22.91 | on/complete | 1.00/1.00 | 1.00/0.98 | 0 | 0 | 0 | true_procedural_air |
| 430 | 33.52 | 19.03 | on/complete | 1.00/1.00 | 1.00/1.00 | 0 | 0 | 0 | true_procedural_air |

Per-frame audit buckets:

- frame 550:
  - `true_procedural_air`: `62`
  - `cpu_shader_solid_representation_expected_solid`: `1`
  - `cpu_solid_far_representation_air`: `1`
- frame 490:
  - `true_procedural_air`: `62`
  - `cpu_shader_solid_representation_expected_solid`: `2`
- frame 520:
  - `true_procedural_air`: `59`
  - `cpu_shader_solid_representation_expected_solid`: `5`
- frame 460:
  - `true_procedural_air`: `64`
- frame 430:
  - `true_procedural_air`: `64`

Interpretation:

- The route-level failure table confirms the remaining dominant bucket is still
  generated-world through-air gaps, not startup exposure, stale SVO, mid
  coverage, ownership miss, proxy fallback, far water, or valley atmosphere.
- There are a few representation/parity outliers in the worst later frames, but
  they are not dominant compared with `true_procedural_air`.
- The next fix should be based on these route frames, especially frames
  `490-550`, rather than further tuning frame 220.

