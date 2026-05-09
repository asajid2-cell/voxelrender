# Sparse Refactor Review Checklist

Use this page when reviewing the sparse voxel refactor. It separates the current
intended path from the dense legacy fallback and lists the verification gates
that should be green before public presentation.

## Intended Demo Path

```powershell
cd VENPOD
.\rebrun.ps1
```

`rebrun.ps1` launches the sparse surface-authoritative sandbox by default. Use
`.\rebrun.ps1 -DenseLegacy` only when comparing against the old dense renderer.

## Required Gates

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\VENPOD\build.ps1 -Config Release
ctest --test-dir .\VENPOD\build -C Release --output-on-failure
powershell -ExecutionPolicy Bypass -File .\VENPOD\sparse_regression.ps1 -Config Release
```

The regression gate covers:

- sparse backend pipe readiness;
- render ownership quality and stability;
- stationary flicker stability;
- sparse surface fragment visibility;
- GPU sparse raycast health;
- miss-feedback residency planning;
- brush feedback parity, apply, and authoritative modes;
- sparse edit persistence through `rebrun.ps1 -SparseEditFile` during the
  seeded-surface smoke;
- sparse GPU physics diagnostics and guarded proposal apply;
- engine backbuffer capture.

The latest public contact sheet from the engine capture gate is stored at
[docs/media/sparse-engine-contact-sheet.png](../media/sparse-engine-contact-sheet.png).
It is generated from the renderer's own DX12 backbuffer readback after sparse
warmup.

## Runtime Signals To Check

Healthy sparse runs should show:

- `PERF_BACKEND_PIPE ... active=0x7FF ... warn=0x0` after warmup;
- `PERF_RENDER_OWNERSHIP` samples with low `miss` and `unsafeNearMiss`;
- `PERF_SPARSE` with no upload overflow, no persistent publish backlog, and
  bounded retry counters;
- `PERF_SPARSE_SURFACE` with nonzero GPU faces/ranges and no cull overflow;
- `midCov` and `farCov` samples when mid/far layers are active.

## Definition-Of-Done Evidence

| Requirement | Evidence | Status |
| --- | --- | --- |
| Player spawns on sparse-rendered terrain | Sparse smoke and engine capture smoke warm up through the sparse surface-authoritative path. | Covered by gate |
| Sparse near terrain is visibly coherent | Render ownership quality/stability, flicker smoke, surface-fragment smoke, and backbuffer capture. | Covered by gate |
| Brush paint uses world-space sparse raycast | Sparse CPU raycast/edit tests plus GPU raycast health and brush feedback parity/apply smokes. | Covered with CPU-authoritative fallback |
| Painted edits persist after eviction/reload | Sparse edit overlay, dirty render region, eviction protection, generated-brick replay tests, `VENPOD_SPARSE_EDIT_FILE` save/load coverage in `VENPODSparseCore`, and seeded-surface regression persistence through `-SparseEditFile`. | Covered |
| Collision samples sparse bricks, not height snap | Sparse collision volume/sweep/support tests and sparse body-collision telemetry. | Covered by unit tests |
| Local physics runs on dirty/active bricks only | Sparse local physics tests and GPU physics smoke; GPU proposal application remains guarded. | Covered for local CPU authority |
| Fast flight does not flash stale chunks | Stress camera/request sparse smoke with ownership quality/stability and unsafe-near-miss telemetry. | Smoke-covered |
| Flying high shows coherent far LOD, not a finite dense cube | Far SVO readiness, mid/far coverage telemetry, and engine capture smoke. | Smoke-covered |
| Metrics expose page residency, skips, uploads, and GPU timings | `PERF_SPARSE`, `PERF_BACKEND_PIPE`, `PERF_RENDER_OWNERSHIP`, `PERF_SPARSE_SURFACE`, and split GPU timings. | Covered |
| Dense legacy can still be selected for regression comparison | `.\rebrun.ps1 -DenseLegacy`. | Available |

## Current Known Limits

- Dense legacy is still present for fallback and regression comparison.
- Sparse brush edits can be saved and loaded with
  `.\rebrun.ps1 -SparseEditFile` or the pause-menu metrics panel.
- GPU brush feedback and GPU physics proposal application remain guarded hybrid
  paths; CPU sparse authority remains the resilience fallback.
- Mid/far terrain is coherent enough for smoke gates, but final long-distance
  LOD and visual polish are still future work.
- A polished public demo video still needs a capture pass.
