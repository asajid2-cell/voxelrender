
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
