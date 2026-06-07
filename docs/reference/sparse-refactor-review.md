# Sparse Refactor Review Checklist

Use this page when reviewing the sparse voxel refactor. It separates the current
intended path from the dense legacy fallback and lists the verification gates
that should be green before public presentation.

For the broader handoff packet, see
[Public review manifest](public-review-manifest.md).

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

- public review docs/artifacts, public PowerShell script parsing, markdown file
  links and anchors, contact-sheet presence, tracked/staged source-artifact
  git visibility, and generated-artifact ignore
  patterns before build;
- sparse backend pipe readiness;
- dense legacy fallback launch/exit smoke for comparison availability;
- render ownership quality and stability;
- stationary flicker stability;
- sparse surface fragment visibility;
- GPU sparse raycast health;
- miss-feedback residency planning;
- brush feedback parity, apply, and authoritative modes;
- sparse edit persistence through `rebrun.ps1 -SparseEditFile` during the
  seeded-surface smoke;
- default local sparse physics without GPU packet/apply flags;
- sparse GPU physics diagnostics and guarded proposal apply;
- engine backbuffer capture with runtime ownership assertions;
- stress-camera engine backbuffer capture for high-flight/far-terrain review,
  also with runtime ownership assertions;
- public demo MP4/contact-sheet generation through
  `VENPOD/public_demo_capture.ps1`, covered by the sparse regression gate with
  runtime ownership assertions for the captured source frames.

The regression wrapper validates normal/stress capture windows and public demo
capture parameters before building, so invalid review-media settings fail
without touching generated artifacts.
It also scans saved runtime logs for critical/error, device-removed, timeout,
and sparse readiness/ownership failure markers across sparse smoke stages.
For normal capture, stress capture, and public demo capture, it additionally
parses `PERF_RENDER_OWNERSHIP` and requires post-ready terrain ownership,
positive mid/far ownership, visible far-SVO pixels, positive surface fragments,
and zero `miss` / `unsafeNearMiss` pixels.
Direct engine capture rejects broad output directories and clears stale capture
frames before launch so old frame files cannot satisfy a new smoke. Capture
outputs are limited to dedicated folders under `VENPOD/build/captures/` or
`VENPOD/build/logs/`; parent/repository-root outputs such as `-OutputDir ..`
and runtime/source trees such as `VENPOD/build/bin/` are rejected.

The latest public contact sheet from the engine capture gate is stored at
[docs/media/sparse-engine-contact-sheet.png](../media/sparse-engine-contact-sheet.png).
It is generated from the renderer's own DX12 backbuffer readback after sparse
warmup.

## Runtime Signals To Check

Healthy sparse runs should show:

- `PERF_BACKEND_PIPE ... active=0x7FF ... warn=0x0` after warmup;
- `PERF_RENDER_OWNERSHIP` samples with low `miss` and `unsafeNearMiss`;
- visible `farSvo` ownership after far SVO readiness;
- render-smoke samples with meaningful simultaneous mid/far ownership;
- fast-request telemetry with scaled visible/collision request planning and
  zero request skips;
- `PERF_SPARSE` with no upload overflow, no persistent publish backlog, and
  bounded retry counters;
- `PERF_SPARSE_SURFACE` with nonzero GPU faces/ranges and no cull overflow;
- `midCov` and `farCov` samples when mid/far layers are active.

## Definition-Of-Done Evidence

| Requirement | Evidence | Status |
| --- | --- | --- |
| Player spawns on sparse-rendered terrain | Sparse smoke and engine capture smoke warm up through the sparse surface-authoritative path; `VENPODSparseCore` covers scenic spawn validation plus malformed extreme-origin fallback behavior. | Covered by gate |
| Sparse near terrain is visibly coherent | Render ownership quality/stability, flicker smoke, surface-fragment smoke, checked terrain/surface coordinate conversion, signed-boundary surface neighbor/bounds guards, malformed surface visibility-culling fail-open guards, sparse surface GPU config validation for malformed runtime capacities, GPU cull dispatch constant sanitization for malformed camera/projection input, surface range allocator boundary guards for stable face-range lifecycle, surface cluster metadata guards for extreme bounds, extreme Morton sort keys, and saturated face counts, and backbuffer/public-demo capture ownership assertions with visible far-SVO pixels. | Covered by gate |
| World-space coordinates remain stable | Sparse coordinate conversion tests cover negative coordinates and signed `int32_t` min/max world voxels. | Covered by unit tests |
| Page-table updates cannot publish stale generations | Page-table tests cover exact-generation lookup, tombstone probing, delayed invalidation, pending publish replacement, stale same-page replacement/retry rejection, invalid/tombstone publish rejection, existing-entry updates at the load threshold, and brick-pool capacity admission without wrapped page-table sizing. | Covered by unit tests |
| Brush paint uses world-space sparse raycast | Sparse CPU raycast/edit tests plus malformed-input and extreme-coordinate DDA/brush-volume guards; GPU raycast dispatch guards shader constants before launch; CPU sparse brush edits and GPU brush-feedback dispatch share checked brush voxel bounds before scan/dispatch; GPU raycast health and brush feedback parity/apply smokes remain active; GPU brush feedback applies only payloads with zero missing-resident header/sentinel counts, no shader/count overflow, no stale readback drop, and unique edit coordinates, and clean smokes fail if duplicate feedback payloads appear. | Covered with CPU-authoritative fallback |
| Painted edits persist after eviction/reload | Sparse edit overlay, dirty render region, eviction protection, generated-brick replay tests, malformed edit-file rejection, nonzero edit-revision epoch reset coverage, edit-delta truncation/duplicate/range-table overflow guards, `VENPOD_SPARSE_EDIT_FILE` save/load coverage in `VENPODSparseCore`, and seeded-surface regression persistence through `-SparseEditFile`. | Covered |
| Collision samples sparse bricks, not height snap | Sparse collision volume/sweep/support tests, fail-closed malformed/oversized query guards, bounded sweep step counts, character movement/grounding input sanitization before collision/support scans, sanitized collision/brush residency planning, and sparse body-collision telemetry. | Covered by unit tests |
| Local physics runs on dirty/active bricks only | Sparse local physics tests including signed-boundary support/move overflow guards and capped oversized staging requests, default local sparse-physics smoke, and GPU physics smoke; GPU physics dispatch guards packet/edit metadata before shader launch; local/GPU proposal application remains guarded by checked world-coordinate conversion, consumed/known status bits, page generation, expected-page status consistency, exact edit-revision parity, edit-delta status consistency, malformed expected-page/local-coordinate rejection, source/destination coordinate-overflow rejection, residency, and batch-conflict checks. | Covered for local CPU authority |
| Runtime budgets do not wrap under pressure | Sparse runtime scheduler tests cover frame/ownership pressure, request/upload planning, request-planner radius/request caps, saturating residency trim/replacement distance scoring, view-cone and hierarchical/collision request input/offset guards, far upload throttling, sparse GPU resource config/stat validation for malformed runtime capacities, and saturating arithmetic for extreme budget counters. | Covered by unit tests |
| Fast flight does not flash stale chunks | Stress camera/request sparse smoke plus fast-request telemetry assertions and stress-camera capture ownership assertion with zero miss/unsafe-near-miss pixels. | Smoke-covered |
| Flying high shows coherent far LOD, not a finite dense cube | Far SVO readiness, far-SVO malformed config-origin guards, mid/far coverage telemetry, normal/stress/public-demo capture ownership assertions with visible far-SVO pixels, and `SparseClipmapPolicy` near-exit boundary tests that prevent inverted mid ownership. | Smoke-covered plus unit guard |
| Metrics expose page residency, skips, uploads, and GPU timings | `PERF_SPARSE`, `PERF_BACKEND_PIPE`, `PERF_RENDER_OWNERSHIP`, `PERF_SPARSE_SURFACE`, and split GPU timings. | Covered |
| Dense legacy can still be selected for regression comparison | `.\rebrun.ps1 -DenseLegacy` and the dense legacy fallback smoke in `sparse_regression.ps1`. | Covered by gate |

## Current Known Limits

- Dense legacy is still present for fallback and regression comparison.
- Sparse brush edits can be saved and loaded with
  `.\rebrun.ps1 -SparseEditFile` or the pause-menu metrics panel.
- GPU brush feedback and GPU physics proposal application remain guarded hybrid
  paths; CPU sparse authority remains the resilience fallback.
- Mid/far terrain is coherent enough for smoke gates, but final long-distance
  LOD and visual polish are still future work.
- Public demo MP4s are generated on demand under `VENPOD/build/captures/` and
  are not checked into git.
