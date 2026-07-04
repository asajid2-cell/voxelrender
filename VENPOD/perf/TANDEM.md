
## 2026-07-03 SIEGE3: view-follow stale trim = the at-rest regeneration (CONVERGED)
Partner: codex → CONVERGED with my telemetry trace, and pinned the exact residual eviction path.
My independent empirical chain: bursts↔surfEmit r=0.526; cpuSerial climbs at rest (~0.8/frame fix-on,
~8.5 fix-off); pool 93% FREE; evictLast/pressureTrim=0 → a NON-pressure eviction I couldn't locate.
Codex's independent code trace: VENPOD_SPARSE_VIEW_FOLLOW_TRIM (default-on, main_launcher.cpp:1526)
→ TrimStaleResidentBricks (SparseVoxelWorld.cpp:5669) evicts ANY resident brick with
lastTouchedFrame < currentFrame-120 (test 5711, evict 5727), IGNORING pool pressure/free pages/
stationary camera. At rest the per-frame replay only touches the current request/repair subset;
visible terrain outside it ages past 120 frames → shed with 30k free pages → re-requested → regen →
surface re-extract → visible regeneration. Toggle: VENPOD_SPARSE_VIEW_FOLLOW_TRIM=0.
STATUS: running causal proof (idle -ViewFollowTrimOff) — expect cpuSerial flat + bursts↓.

## 2026-07-03 SIEGE3 FRONT 2 (moving): hard LOD handoff = root (CONVERGED)
Partner codex CONVERGED: moving artifact = hard LOD handoffs, NOT surface regeneration. (1) mid-height
mesh mergeCells hard thresholds 2200/5000u no hysteresis (SparseClipmap.cpp:7852), parent skipped when
finer child resident (7587) binary; (2) mid-voxel raymarch ring select hard floor()*ringCount
(PS_Raymarch.hlsl:2026) no inter-ring blend. Trim EXONERATED for moving (cpuSerial residual = legit
streaming; computeTileLod never queues surface extract). No geomorph/blend anywhere. Fix = geomorph/blend
FEATURE per system. Pixels can't measure (motion saturates) → need user eyes via isolation toggles
VENPOD_SPARSE_MID_MESH_LOD=0 / VENPOD_SPARSE_MID_VOXEL_RENDER=0.
