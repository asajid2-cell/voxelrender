# Campaign: Sparse Terrain Propagation Wave

## Win Condition
The recorded replay `wave_repro.vnrd` no longer shows the moving terrain propagation wave, while mid mesh remains enabled, terrain continuity is not broken, and exact/live terrain ownership still works for near edits. Human visual confirmation is a final gate.

## Constraints & Anti-goals
- Do not ship broad disables such as `VENPOD_SPARSE_MID_MESH=0`, grey terrain, or broken continuity.
- Do not treat dirty redraw suppression as a fix; it starves required mid mesh upload.
- Prefer narrow ownership/readiness fixes over cosmetic hiding.
- Diagnostics may remain behind debug modes/env flags only.

## Terrain Map
Known true:
- Replay exists: `wave_repro.vnrd`.
- Debug 71 showed the visible terrain in the replay window is sparse mid mesh raster draw.
- Dirty-redraw cut is invalid: it removes required mid generation/upload and greys terrain.
- Freezing mid CPU rebuild and surface upload after warmup did not remove the visual wave.
- Debug 73 live-material delta overlay tracks the propagation band, mostly as live-erased candidates.
- Debug 74, which keeps live-erased raster faces, did not remove the user-visible wave.

Known false:
- The bug is not solved by removing one mid dirty redraw line.
- The bug is not solely the `liveErased` discard branch.
- The bug is not only fullscreen mid voxel DDA or parent-held fallback.

Unknown:
- Whether the wave is caused by shader ownership clipping, draw-pass depth/stencil handoff, exact surface promotion/readiness, or mid mesh cull/LOD state.
- Whether mid mesh geometry is actually changing in the wave window or only which representation owns pixels.

## Approach Tree
| # | Approach class | Prediction | Cheapest probe | Kill criteria | Status |
|---|---|---|---|---|---|
| 1 | Explicit pass ownership | Mid mesh is being affected by exact/live ownership constants or shader logic intended for exact surface | Add pass identity/debug colors and compare replay | Wave persists when mid pass ignores exact/live ownership | live |
| 2 | Geometry/LOD churn | Mid mesh face set changes along the wave | Log/count per-tile LOD/child/cull state and compare to wave frames | No spatially matching geometry/state churn | live |
| 3 | Depth/stencil handoff | Wave is pass ordering/stencil/clip, not face generation | Disable/alter only ownership tests in mid pass | No visual change | live |

## Fronts
| Front | Mechanism | State | Last advance |
|---|---|---|---|
| Attribution | self/debug | live | 2026-07-04: debug 73/74 narrowed to ownership boundary, not discard-only |
| Fix | self | pending | needs attributed source |
| Verification | replay/contact sheets + human gate | pending | stock/debug captures available |

## Beat Log
- 2026-07-04: Campaign opened. Current highest-leverage move: read sparse surface draw order/constants and prove whether mid mesh pass receives exact/live ownership behavior.
- 2026-07-05: Root-cause checkpoint. Replay frame 660 proved exact surface data is valid but was being visually overwritten when mid mesh was enabled: debug 80 with mid on showed grey/no exact and `surfaceFragments` around 310; the same frame with `VENPOD_SPARSE_MID_MESH=0` showed yellow exact foreground and `surfaceFragments` around 1.14M. Patch made mid mesh a real underlay by drawing it before exact and restoring the exact stencil ref to the normal surface-owned value. Verification: `captures/codex_orderfix_mode80/engine_frame_0660.bmp` shows exact foreground with mid enabled, `logs/codex_orderfix_mode80.log` reports `surfaceFragments` around 1.5M at frame 660, debug 95 frame 660 shows no near magenta mid-inside-exact overlay, and normal frames 640-665 show no grey/censored foreground.

## Learnings
- A locator overlay can correlate with the wave without being causal; debug 73 tracks the boundary, debug 74 killed discard-only as the root cause.

## BLOCKED / Decisions needed
- None.
