# Sparse Completion Audit

This audit maps the active sparse-refactor objective to concrete artifacts. It
is intentionally stricter than a smoke-test summary: passing gates are evidence
only where they cover the requirement being claimed.

## Objective

Work on VENPOD until the sparse voxel refactor described in `refactor.md` is
complete, hardened, documented, and ready for public review.

Concrete review criteria:

- sparse surface-authoritative path is the default demo path;
- dense legacy remains available only as fallback/comparison;
- sparse terrain, streaming, edits, collision, physics, and mid/far continuity
  have regression evidence;
- public docs explain how to build, run, review, capture media, and understand
  known limits;
- generated public media and smoke artifacts can be reproduced;
- remaining incomplete areas are explicit and not hidden behind green tests.

## Prompt-To-Artifact Checklist

| Requirement | Evidence Inspected | Status |
| --- | --- | --- |
| `refactor.md` describes the sparse architecture and validation history. | [refactor.md](../../refactor.md) records the sparse brick/page-table/surface/feedback/physics/capture passes and latest full sparse regression evidence. | Covered |
| Sparse path is active/default. | [README.md](../../README.md), [Runtime reference](runtime.md), and `VENPOD/rebrun.ps1` document/run sparse surface-authoritative mode by default. | Covered |
| Dense path is preserved as fallback. | `.\rebrun.ps1 -DenseLegacy` is documented in README, runtime reference, review checklist, and manifest; `sparse_regression.ps1` includes a dense legacy fallback smoke that positively verifies the requested/active dense backend, disabled sparse raymarch path, and dense dispatcher contract. | Covered by gate |
| Build works in Release. | `.\VENPOD\sparse_regression.ps1 -Config Release` builds first; latest full run passed. | Covered by full gate |
| CPU/unit sparse core tests pass. | `VENPODSparseCore` passed during the latest full sparse regression. | Covered by full gate |
| Page-table lifecycle rejects stale data without blocking valid republish. | `SparsePageTable` tests cover generation-aware lookup, tombstone probing, stale-generation rejection, and existing-entry updates at the load threshold; `SparsePagePublishQueue` tests reject invalid/tombstone publish pages before publication. | Covered by unit tests |
| Sparse coordinate conversion is stable at signed-world extremes. | `FloorDiv`, `FloorMod`, `BrickCoord::FromWorldVoxel`, and `LocalVoxelFromWorld` tests cover negative coordinates and `int32_t` min/max world voxels. | Covered by unit tests |
| Sparse render backend is coherent after warmup. | Render/backend smoke checks pipe readiness, ownership quality, far SVO readiness, surface lookahead, ownership pressure telemetry, and positive mid/far ownership continuity telemetry. | Covered by full gate |
| Flicker/page flashing is guarded. | Every-frame flicker smoke checks ownership stability after warmup, requires enough post-ready ownership samples, bounds terrain/miss deltas, and rejects post-ready miss or unsafe-near-miss pixels. | Covered by full gate |
| Sparse surfaces are visible and GPU path is optimized. | Seeded-surface smoke positively verifies diagnostic surface seed, GPU faces/draws/records/payload, GPU cull dispatch and accepted draws, stable+compact draw mode, raster fragments, a settled GPU-surface frame with no staged/deferred/retry backlog, no surface overflow, and engine capture validates nonblank backbuffer frames. | Covered by full gate |
| Sparse edit persistence is usable. | `SparseEditStore` unit coverage, `-SparseEditFile`, pause-menu save/load controls, and seeded-surface `.vsed` verifier. | Covered |
| Sparse raycast/brush behavior is tested. | GPU raycast health smoke positively verifies accepted GPU raycast hits, zero strict diagnostic rejects/misses, and fallback within limits; brush feedback parity/apply/authoritative smokes verify parity, GPU commits, authoritative completion, and CPU fallback; brush feedback apply rejects missing-resident header counts, missing-resident sentinel records, overflowed payloads, or duplicate-coordinate GPU payloads before commit; CPU sparse path remains authority. | Covered with fallback |
| Collision samples sparse data. | Sparse collision volume/sweep/support tests, malformed/oversized collision query guards, and default local-physics smoke positively verifies active collision backend, sampled sparse body-collision voxels, solid support, and grounded/landing state. | Covered |
| Local sparse physics is default-on and bounded. | Default local sparse-physics smoke verifies CPU moves, zero GPU packet/apply counters, packet/processed/move counts within logged local budgets, no physics overflow/malformed counters, and sparse body-collision authority telemetry. | Covered |
| GPU physics proposal path is guarded. | GPU physics smoke positively verifies packet staging, GPU result/proposal readback with nonzero generation/checksum metadata, CPU-authoritative apply, zero clean-smoke proposal rejects, zero missing-below support readbacks, and no physics upload/result overflow; CPU validation guards cover well-formed consumed status bits, page generation, exact edit-revision parity, expected-page status consistency, edit-delta status consistency, malformed expected-page/local-coordinate payloads, destination residency, and batch conflicts. | Covered as guarded path |
| Runtime scheduler remains bounded under pressure. | `SparseRuntimeBudgetScheduler` tests cover frame pressure, ownership pressure, miss feedback retries, upload planning, far upload throttling, and saturating arithmetic for extreme budget/queue counters; miss-feedback smoke positively verifies pending feedback, effective ownership pressure, runtime sampling plan, and zero stale/overflow feedback status in `PERF_SPARSE`. | Covered by unit tests plus smoke |
| Mid/far terrain continuity exists. | Far SVO readiness with visible SVO ownership, positive `midCov`/`farCov`, render-smoke `PERF_RENDER_OWNERSHIP` assertions for meaningful simultaneous mid/far ownership, fast-request telemetry assertions for scaled visible/collision request planning with zero skips, normal/stress/public-demo capture ownership assertions for post-ready terrain with visible far-SVO pixels and no miss/unsafe-near-miss pixels, normal/stress engine capture contact sheets, and `SparseClipmapPolicy` tests for near-exit handoff boundaries. | Smoke-covered plus unit guard |
| Public media can be regenerated. | `VENPOD/public_demo_capture.ps1` generates validated contact sheet/stats/log and probes/decodes MP4; sparse regression also parses the public-demo runtime log for terrain ownership, mid/far ownership, surface fragments, and zero miss/unsafe-near-miss pixels. | Covered |
| Public docs are organized and linked. | README, docs index, architecture, runtime reference, review checklist, public review manifest, sandbox how-to, demo capture how-to, and the `sparse_regression.ps1` public-review doc/link/source-artifact verifier. | Covered |
| Generated artifacts are not accidentally public source. | `.gitignore` excludes build/log/capture outputs; public manifest lists generated paths; the sparse regression public-review verifier checks required ignore patterns before build. | Covered |

## Latest Gate Evidence

Latest full gate:

```powershell
powershell -ExecutionPolicy Bypass -File .\VENPOD\sparse_regression.ps1 -Config Release
```

Latest observed result: passed on 2026-05-10 after the sparse surface ABI,
frame-constant ABI, RHI upload-lifetime, fullscreen root-signature, dense
raycast packed-word decode, and monotonic raycast readback frame-count
hardening. The adversarial pass first exposed a GPU-raycast health failure:
valid readbacks were queued with the swapchain frame index instead of the
monotonic producer frame, so the freshness guard could classify delayed
readbacks incorrectly. The launcher now passes `frameCount` to the brush/ground
raycast queue and retire calls, and the public verifier pins those call sites.
After that fix, the targeted GPU-raycast smoke reported
`accepted=1 fallback=0 rejected=0 miss=0`, and the full sparse regression
passed.

The latest full gate verifier covered the public docs/artifacts/file links,
source-artifact git visibility, generated-artifact ignore patterns, public
PowerShell parsing, source guard snippets, and stale root-signature rejection.
It then ran the Release build, `VENPODSparseCore`, dense fallback smoke, all
sparse runtime smokes, normal/stress engine captures, and public demo MP4 plus
runtime ownership validation.

Follow-up compact gate on 2026-05-10 passed after sparse surface cluster
metadata hardening. The follow-up covered Release build, `VENPODSparseCore`, the
public-review verifier, sparse render/backend smoke, default local sparse
physics, and GPU sparse physics.

Follow-up focused validation on 2026-05-10 passed after sparse brick-pool
capacity admission hardening. The follow-up covered Release build and
`VENPODSparseCore`; compact sparse regression was then rerun with the
public-review verifier and sparse render/backend, default local physics, and GPU
physics smokes active.

Follow-up focused validation on 2026-05-10 passed after sparse edit-delta
range-table hardening. The follow-up covered Release build and
`VENPODSparseCore`; compact sparse regression was then rerun with the
public-review verifier and sparse render/backend, default local physics, and GPU
physics smokes active.

Observed coverage from the latest pass:

- public-review docs/artifacts/scripts/link/anchor/index-map verifier;
- public-review tracked/staged source-artifact git-visibility verifier;
- `VENPODSparseCore`;
- page-table load-limit update hardening;
- sparse brick-pool initialization rejects zero page capacity and page-table
  capacity requirements that would wrap before allocation;
- page-publish invalid/tombstone page rejection;
- sparse collision malformed/oversized query guards;
- sparse collision sweep step-count cap for malformed or accidental huge sweep
  requests;
- positive sparse body-collision sampling/support assertion in default local
  physics smoke;
- runtime scheduler saturation guards;
- GPU physics exact edit-revision parity and edit-delta status consistency
  hardening;
- GPU physics expected-page status consistency and zero-generation rejection;
- GPU physics source/destination world-coordinate overflow rejection before CPU
  proposal mutation;
- shared checked brick/local-to-world conversion and sparse surface extraction
  rejection for out-of-range brick coordinates;
- sparse terrain generation, local physics staging, and surface-cache
  visibility/dirty-region math use checked brick/local-to-world conversion
  instead of direct `BrickCoord * 16` arithmetic;
- sparse scenic spawn search clamps malformed radius/spacing/player-height
  inputs and avoids signed overflow near extreme world origins;
- CPU sparse DDA raycast rejects non-finite input, clamps maximum traversal
  distance/steps, and fails closed at signed world-coordinate step boundaries;
- sparse GPU raycast and miss-feedback dispatch now reject malformed origins,
  invalid page metadata, invalid distances, and invalid ray directions before
  publishing shader constants; dispatch constants use clamped finite distances,
  step counts, basis vectors, FOV, and aspect values;
- sparse GPU physics packet dispatch now rejects malformed packet/edit
  metadata, invalid page/range-table capacities, and oversized packet counts
  before publishing constants or computing dispatch groups;
- sparse character horizontal/vertical movement and grounding now sanitize body
  poses and dimensions, clamp velocity/snap/sweep inputs, and reject malformed
  requests before collision/support scans;
- sparse world dirty-region neighbor propagation and local-physics support/move
  planning now use checked signed-coordinate stepping at brick and voxel
  boundaries, and local physics staging caps malformed oversized packet
  requests before reserving work storage;
- sparse residency trimming, upload value sorting, queued background trimming,
  and priority replacement now use 64-bit/saturating sparse brick distance
  scores and unsigned keep-radius comparisons instead of narrowing deltas or
  squaring `int32_t` differences;
- sparse surface extraction/cache boundary math now checks world-neighbor steps
  and saturates merged face bounds so valid max-world boundary bricks cannot
  wrap neighbor samples or surface-record bounds;
- sparse surface visibility culling now fails open for malformed camera,
  lookahead, basis, or derived projection inputs and clamps FOV/aspect/distance
  fields before deciding to cull cached surface bricks;
- sparse GPU resource config/stat validation now rejects malformed page-table,
  upload-ring, feedback, clipmap, physics-packet, and edit-delta capacities
  before allocation or public stat computation;
- sparse surface GPU config validation now rejects malformed face, IA stream,
  range-table, draw-command, upload-ring, cull-dispatch, cluster, and payload
  copy capacities before allocation or dispatch publication;
- sparse surface GPU cull dispatch now sanitizes camera position, basis, FOV,
  aspect, max-distance, and padding constants before publishing them to the
  cull shader;
- sparse surface range allocation now uses saturating retire-token, coalescing,
  generation, and stats math so stable surface face ranges cannot wrap at
  `uint32_t` or frame-token boundaries;
- sparse surface cluster metadata now uses bounded reserve sizing, saturating
  face-count/index metadata, 64-bit extent checks, and overflow-free Morton
  sort-key biasing so malformed record bounds, extreme brick coordinates, or
  face totals cannot wrap into valid-looking GPU cull/draw clusters;
- far SVO CPU build config now rejects non-finite/invalid page size, non-finite
  root height, excessive depth, and page-origin ranges that cannot safely floor
  to signed world coordinates before recursive SVO construction or page
  publication;
- sparse brush preview/commit builds a checked bounded edit volume before
  scanning voxels, rejecting non-finite radius/position input and extreme
  signed world-coordinate ranges;
- CPU sparse brush edits and GPU brush-feedback dispatch now share the same
  checked brush voxel-bound helper, so non-finite input, signed-coordinate
  overflow boundaries, radius/strength clamping, and total voxel caps are
  enforced before either CPU scan or GPU dispatch;
- collision/brush residency planning sanitizes non-finite motion, body, and
  brush-intent inputs before deriving sparse brick requests;
- basic and stress sparse request planners clamp malformed radius/prefetch
  inputs, cap request counts, and skip signed-coordinate overflow requests;
- sparse view-cone request planning rejects malformed origins, clamps optional
  FOV/aspect/distance/step inputs, caps request counts, and checks coverage/DDA
  brick offsets before emitting visible-residency requests;
- hierarchical and collision request planning now cap composed request counts,
  prediction lengths, shell/motion radii, and checked signed-coordinate offsets
  before emitting collision, visible, ownership-recovery, or speculative
  residency requests;
- positive GPU physics packet/readback/apply assertion in GPU-physics smoke;
- mid-range ownership boundary hardening for exhausted near/mid handoffs;
- render/backend smoke;
- positive mid/far continuity telemetry assertion in render/backend smoke;
- fast-request telemetry assertion in render/backend and stress-camera capture
  smokes;
- dense legacy fallback smoke;
- flicker smoke;
- positive seeded-surface GPU path and edit persistence smoke;
- positive GPU raycast health assertion in GPU-raycast smoke;
- positive miss-feedback pressure and sampling-plan smoke;
- brush feedback, apply, and authoritative smokes, including clean-path
  rejection of unexpected duplicate edit-record payloads;
- default local sparse physics smoke;
- GPU physics smoke;
- engine backbuffer capture with runtime ownership assertion;
- stress-camera engine backbuffer capture with runtime ownership assertion;
- public demo capture validation with MP4 stream verification and runtime
  ownership assertion.
- latest full Release sparse regression after surface lifecycle hardening,
  including dense fallback, flicker, seeded-surface edit persistence, GPU
  raycast, miss feedback, brush feedback observe/apply/authoritative, default
  local physics, GPU physics, normal/stress engine capture, and public demo MP4
  validation.

Additional hardening and focused validation:

- Sparse clipmap policy inputs now sanitize non-finite configuration and query
  distances before near/mid/far transition math, LOD cell sizing, and ownership
  tests. `VENPODSparseCore` covers NaN/Inf clipmap config and query inputs, and
  a compact Release sparse regression passed with sparse render/backend mid/far
  continuity and GPU-physics positive gates active.
- Sparse clipmap interest streaming now sanitizes non-finite camera, view,
  velocity, and prediction inputs before deriving height-tile and voxel-brick
  coordinates. `VENPODSparseCore` covers NaN/Inf interest input and verifies
  generated mid/far requests stay near the sanitized origin instead of leaking
  undefined floor-to-int results into clipmap coordinates.
- Sparse clipmap interest streaming now also clamps extreme finite camera,
  prediction, terrain-center, tile-origin, and voxel-origin conversions before
  they can narrow into `int32_t` grid or world coordinates. `VENPODSparseCore`
  covers large finite camera/velocity input and verifies generated height and
  voxel clipmap origins avoid saturated integer endpoints.
- Sparse character-controller movement now fails closed on malformed horizontal
  targets, vertical targets, and grounding bodies before sampling sparse
  collision/support. `VENPODSparseCore` covers NaN/Inf poses, unbounded
  sweep-step requests, non-finite velocity, and non-finite snap distances; a
  compact Release sparse regression passed with default local physics and GPU
  physics positive gates active.
- Sparse world local-physics support planning now skips below-brick and
  falling-voxel destinations that would overflow signed coordinates, and
  staging caps oversized packet requests before reserving. `VENPODSparseCore`
  covers `int32_t::min()` brick/voxel boundaries and `UINT32_MAX` staging
  requests; a compact Release sparse regression passed with render/backend,
  default local physics, and GPU physics positive gates active.
- Sparse residency trim/replacement scoring now uses saturating 64-bit distance
  math. `VENPODSparseCore` covers full signed-range queued trim and
  large-distance resident trim/replacement cases whose squared deltas exceed
  `int32_t`; a compact Release sparse regression passed with render/backend,
  default local physics, and GPU physics positive gates active.
- Sparse surface extraction/cache boundary math now skips overflowing neighbor
  samples and clamps merged face bounds at signed world limits.
  `VENPODSparseCore` covers valid max-world boundary bricks for extractor and
  cache snapshots; a compact Release sparse regression passed with
  render/backend, default local physics, and GPU physics positive gates active.
- Far SVO config-origin validation now rejects malformed build configs before
  page-origin casts or recursive bounds construction. `VENPODSparseCore` covers
  non-finite page size, out-of-range page origins, and excessive depth through
  the shared builder helper; a compact Release sparse regression passed with
  visible far-SVO ownership, render/backend, default local physics, and GPU
  physics positive gates active.
- Sparse surface visibility culling now fails open on malformed camera,
  lookahead, basis, or derived projection inputs. `VENPODSparseCore` covers a
  malformed visibility snapshot and verifies cached surface bricks remain
  visible; a compact Release sparse regression passed with render/backend,
  default local physics, and GPU physics positive gates active.
- Sparse GPU resource config/stat validation now rejects malformed runtime
  capacities before allocation, D3D12 view sizing, or public stat estimation.
  `VENPODSparseCore` covers the default valid config plus oversized mid
  clipmap, voxel clipmap, miss-feedback, physics-packet, and edit-delta limits;
  a Release build, focused sparse core test, and compact sparse regression
  passed with render/backend, default local physics, and GPU physics positive
  gates active.
- Sparse surface GPU config validation now rejects malformed surface runtime
  capacities before allocation, IA stream/view sizing, stable draw/range-table
  publication, or GPU cull dispatch publication. `VENPODSparseCore` covers the
  default valid config plus malformed face capacity, IA overflow, fixed range
  table capacity, upload-ring sizing, excessive face/range/upload allocation
  limits, fixed range-table capacity beyond draw-command-backed visible-record
  capacity, cull dispatch groups, cluster sizing, fast-accept thresholds, and
  payload copy budgets. Surface snapshot staging also rechecks remapped draw
  commands, records, and clusters after range allocation/clustering, uses
  64-bit fixed-table load-factor math, and uses checked fallback
  brick/local-to-world bounds conversion; a Release build, focused sparse core
  test, and compact sparse regression passed with render/backend, default local
  physics, and GPU physics positive gates active.
- Sparse surface GPU cull dispatch now sanitizes camera/basis/projection
  constants before shader publication. Non-finite camera position, degenerate
  or non-finite basis vectors, malformed FOV/aspect, max-distance, and padding
  are replaced or clamped to finite runtime values; a Release build, focused
  sparse core test, and compact sparse regression passed with accepted GPU cull
  draws, render/backend, default local physics, and GPU physics positive gates
  active.
- Sparse surface range allocation now saturates retire-token addition, clamps
  retired/free ranges to allocator capacity, computes coalesced range ends with
  64-bit intermediates, keeps allocation generations nonzero across in-place
  and moved resizes, and saturates stats counters. `VENPODSparseCore` covers
  generation advancement, saturated retire-token behavior, and `uint32_t`
  boundary coalescing; a Release build, focused sparse core test, and compact
  sparse regression passed with accepted GPU cull draws, render/backend,
  default local physics, and GPU physics positive gates active.
- Renderer frame-constant publication now sanitizes GPU-visible sparse
  ownership values before the raymarch shader sees them. Non-finite camera,
  region-origin, mid-field start/end/cell-size, mid/far coverage ratios,
  far-quality inputs, far SVO coverage, far root height, and near ownership
  center/radius fall back to bounded values instead of publishing NaN/Inf
  transition metadata to HLSL.
- GPU physics proposal apply now rejects expected-page status metadata that
  claims page match/stale results without the required expected-page status bit,
  or claims both page match and stale simultaneously. `VENPODSparseCore`
  covers both malformed readback cases and verifies the source voxel is not
  mutated. The CPU packet-status constants now live in `SparseVoxelWorld.h`,
  and sparse core checks their shader-facing numeric ABI values.
- GPU physics proposal apply now also requires the CPU-current destination voxel
  to exactly match the GPU-sampled destination voxel before applying the move,
  in addition to requiring an air destination. `VENPODSparseCore` covers a
  mismatched destination readback and verifies neither source nor destination is
  mutated.
- GPU physics proposal apply also rejects zero work-generation proposals before
  sampling or mutating sparse edits. `VENPODSparseCore` verifies the malformed
  proposal is counted as rejected, requeued for CPU work, and leaves the source
  voxel unchanged.
- GPU physics proposal apply now rejects malformed proposal status words before
  sampling or mutating sparse edits. Proposal payloads must carry the
  shader-written consumed bit and may only use known status bits; missing
  consumed status or unknown future/corrupt bits are counted as rejected and
  requeued for fresh CPU work. `VENPODSparseCore` covers both malformed cases,
  and the GPU-physics smoke parser requires at least one well-formed consumed
  proposal status in runtime logs.
- GPU physics result retirement now applies the same consumed/known-bit status
  filter before counting valid results or forwarding proposal payloads to CPU
  apply, so malformed readback rows cannot inflate proposal telemetry or reach
  the later apply validator. Runtime physics telemetry exposes dropped malformed
  rows as `gpuMalformed`, and the GPU-physics smoke fails if any are observed.
- GPU brush-feedback apply now rejects duplicate edit-record coordinates in an
  otherwise complete payload. Payloads with zero missing-resident header and
  sentinel counts and no overflow still apply on the GPU feedback path, but
  duplicate-coordinate payloads fall back to CPU replay in authoritative mode
  instead of relying on ambiguous last-write ordering. `VENPODSparseCore` covers
  the duplicate detector, and the brush-feedback smoke parser now fails if a
  clean diagnostic run reports duplicate feedback records.
- GPU brush-feedback retirement now honors the shader-written overflow bit in
  the readback header, not only record counts above CPU capacity. A payload with
  an in-range count but set overflow flag is still treated as incomplete and
  cannot reach GPU apply. `VENPODSparseCore` covers exact-capacity,
  count-over-capacity, and header-flag overflow decisions. The brush-feedback
  regression assertions also fail if runtime `PERF_SPARSE` telemetry reports a
  nonzero feedback overflow, reports a stale feedback readback drop, or if any
  parity-failure line appears in the log.
- GPU brush-feedback apply now also requires both the readback header
  missing-resident count and the observed missing-resident sentinel records to
  be zero. Header/record disagreement is treated as incomplete feedback and
  routes through the same CPU fallback path instead of silently applying a
  partial resident-only edit list. `VENPODSparseCore` covers the completeness
  helper decision.
- Sparse brush-feedback dispatch now uses the shared sparse brush voxel-bound
  helper before publishing shader constants. This rejects malformed
  world-position/radius/strength inputs and clamps dispatch radius/strength to
  the same contract as CPU sparse brush edits. `VENPODSparseCore` covers the
  helper directly, and the focused Release regression passed with
  brush-feedback parity, apply, and authoritative smokes active.
- Sparse brush-feedback parity diagnostics now use the same two-signal
  missing-resident contract as apply/fallback. The diagnostic fails if a
  missing-resident case reports only the header count or only sentinel records,
  `PERF_SPARSE` logs the sentinel count as `brushGpuFbHint` next to
  `brushGpuFbMiss`, and the regression brush-feedback apply gates require CPU
  fallback to observe both signals.
- The public-review verifier now checks that required public source artifacts
  and scripts are tracked or staged in git and not ignored, while generated
  logs/captures/cache files remain ignored. This keeps review handoff files
  commit-visible without requiring generated evidence to enter the repository.
- Dense legacy fallback now uses `VoxelWorldConfig::worldSeed` instead of a
  hardcoded chunk seed, preserving the default seed while keeping fallback
  terrain reproducible and configurable. A Release build and targeted sparse
  regression subset passed with dense legacy fallback, sparse render/backend,
  and GPU-physics smokes active.
- Sparse view-cone request planning now caps malformed request counts, rejects
  non-finite camera origins, clamps optional FOV/aspect/distance/step inputs,
  uses checked coverage offsets, and stops brick DDA on coordinate overflow.
  `VENPODSparseCore` covers the malformed cases directly, and a compact Release
  sparse regression passed with render/backend fast-request and mid/far
  continuity telemetry active.
- Hierarchical and collision request planning now sanitize composed camera,
  velocity, prediction, visible/speculative distance, body height, request-count,
  and radius inputs before deriving sparse residency requests. Collision and
  motion shells use checked offsets and widened priority math, and
  `VENPODSparseCore` covers malformed hierarchical input plus signed-boundary
  centers that must not wrap request coordinates. A compact Release sparse
  regression passed with render/backend fast-request and mid/far continuity
  telemetry active.
- Sparse GPU raycast and miss-feedback dispatch now guard shader constant
  publication against non-finite/out-of-range origins, invalid directions,
  invalid page metadata, and malformed distance/FOV/aspect/step inputs. This is
  runtime-validated by the GPU-raycast and miss-feedback smokes, which passed
  with accepted GPU raycast hits, zero fallback/reject/miss diagnostics, and
  zero stale/overflow miss-feedback telemetry.
- Sparse GPU physics packet dispatch now guards packet counts, page-table
  capacity, edit-delta counts, edit-range counts, and edit-range table capacity
  before binding shader resources or deriving dispatch groups. The focused
  Release sparse regression passed with GPU physics packets/results/proposals
  active, `malformedRows=0`, `missingBelow=0`, and `rejects=0`.
- Sparse generation/upload/surface class queues now remove stale aliases for a
  brick before inserting a retouched class entry. `VENPODSparseCore` verifies
  that generation, upload, and surface class upgrades leave one live class
  bucket entry instead of accumulating stale aliases, and a compact Release
  sparse regression passed with render/backend and GPU-physics smokes active.
  The latest full Release sparse regression also passed after this hardening,
  including the public-review verifier, `VENPODSparseCore`, dense fallback,
  all sparse runtime smokes, engine captures, stress capture, and public demo
  MP4 validation.
- The latest full Release sparse regression passed after the positive gates for
  mid/far continuity, seeded sparse surfaces, GPU raycast health, miss-feedback
  ownership pressure, brush feedback, sparse body collision, and GPU physics
  packet/readback/apply were all active. That run also regenerated and verified
  engine captures, stress captures, seeded sparse edit persistence, and the
  public demo MP4 stream.
- The sparse regression gate now front-loads a public-review docs/artifacts
  verifier. It checks required handoff files/scripts, parses public PowerShell
  entrypoints, verifies tracked contact sheet presence and minimum size, checks
  generated-artifact `.gitignore` patterns, and walks README, `refactor.md`,
  and docs markdown file links plus local Markdown anchors before build or
  runtime smoke stages. The latest full Release sparse regression passed with
  the verifier reporting 21 artifacts, 11 scripts, and 15 Markdown files.
- Runtime reference documentation now lists the sparse regression skip switches,
  including `-SkipPublicReviewDocs`, and states that public review runs should
  keep the verifier enabled. A compact Release sparse regression passed after
  the reference update with the verifier reporting 15 Markdown files.
- The public-review verifier now requires the repository `LICENSE` alongside
  README, `refactor.md`, docs, scripts, and tracked media. A compact Release
  sparse regression passed with 17 required artifacts verified.
- Asset credits are now part of the explicit public handoff: README, docs index,
  and public review manifest link `docs/reference/asset-credits.md`, and the
  public-review verifier requires it. A compact Release sparse regression passed
  with 18 required artifacts verified.
- The public-review verifier now asks Git to check representative generated
  paths for logs, captures, edit files, runtime logs, and far SVO caches, so the
  gate verifies actual ignore behavior instead of only matching `.gitignore`
  text. A compact Release sparse regression passed with these probes enabled.
- The tracked sparse contact sheet is now validated as a PNG before build:
  signature, initial IHDR chunk, and nonzero dimensions are checked. The compact
  Release sparse regression passed with the contact sheet reported as `960x360`.
- The public-review verifier now requires the script surface documented in the
  runtime reference and parses every top-level `VENPOD/*.ps1` helper, including
  legacy/developer aliases. The verifier uses its own relative-path helper so it
  works on the PowerShell/.NET host used by the current repo. A compact Release
  sparse regression passed with 21 required artifacts and 11 scripts verified.
- The runtime reference now explicitly separates the preferred public scripts
  from legacy/developer helpers (`rebuild.ps1`, `rerun.ps1`, and
  `visual_capture_smoke.ps1`) while noting that the verifier still parses those
  helpers. A compact Release sparse regression passed after the documentation
  clarification.
- The public-review verifier now treats `docs/index.md` as an actual document
  map: every Markdown file under `docs/` except the index itself must be linked
  from it. A compact Release sparse regression passed with the docs map check
  enabled.
- Dense legacy fallback is now exercised by `sparse_regression.ps1`. A compact
  regression subset passed with dense fallback smoke, sparse render/backend
  smoke, and GPU-physics smoke enabled after the gate change.
- The dense fallback smoke now positively verifies the requested/active
  `dense-legacy` backend, disabled sparse raymarch path, and dense dispatcher
  contract. A targeted Release regression subset passed with dense fallback,
  sparse render/backend, and GPU-physics smokes active after the assertion was
  added.
- The normal and stress-camera engine capture smokes now parse their runtime
  logs and fail unless post-ready ownership samples stay terrain-owned with zero
  miss and unsafe-near-miss pixels while exercising both mid and far terrain
  ownership plus visible far-SVO pixels. This makes the public media gates
  streaming/ownership checks, not only nonblank image checks.
- The sparse render/backend smoke and stress-camera capture smoke now require
  positive `PERF_SPARSE_FAST_REQUEST` telemetry. Clean fast-flight runs must
  show scaled request planning, nonzero visible/collision request budgets, and
  zero free/class/total request skips.
- Public demo capture validation now uses the same runtime ownership assertion
  after MP4/contact-sheet generation, with an explicit configurable capture
  start frame. The public demo gate therefore proves the encoded demo source
  frames have terrain ownership, mid/far ownership, visible far-SVO pixels,
  surface fragments, and zero miss/unsafe-near-miss pixels.
- The reviewer checklist, public-demo how-to, README, docs index, and runtime
  reference now all describe the capture ownership gates consistently. A compact
  Release sparse regression path passed with the public-review verifier,
  render/backend smoke, and GPU-physics smoke active after the documentation
  sync.
- Brush-feedback clean diagnostics now fail if runtime feedback reports
  duplicate edit records. The duplicate-payload fallback guard remains covered
  by `VENPODSparseCore`, while the apply and authoritative runtime smokes now
  positively report `duplicate=False` for the clean GPU feedback path.
- GPU-physics clean diagnostics now reject missing destination support from
  both dedicated GPU result rows (`missingBelow`) and aggregate runtime physics
  telemetry (`gpuMissingBelow`), keeping both readback summaries aligned with
  the fail-closed smoke gate. A targeted Release sparse regression passed with
  the public-review verifier, render/backend smoke, and GPU-physics smoke
  active after this parser hardening.
- Public demo capture now rejects broad cleanup targets such as the VENPOD
  project root, repository root, and build root before deleting prior capture
  artifacts. The guard was validated with unsafe `-OutputDir .`,
  `-OutputDir ..`, absolute repository-root, and `-OutputDir build\logs`
  rejections plus successful dedicated-folder `-SkipVideo` captures. Capture
  outputs are now restricted to dedicated folders under `build\captures` or
  `build\logs`, with source/runtime trees such as `build\bin` rejected before
  cleanup or folder creation. Capture frame/FPS parameters are also validated
  before cleanup starts. The output directory guard now runs before directory
  creation as well.
- Engine capture smoke now rejects invalid capture parameters before build,
  launch, or output directory creation. Invalid count, interval, exit frame, and
  overflowing capture-window probes were rejected, and a minimal valid capture
  still passed.
- The in-engine backbuffer BMP writer now validates capture dimensions, readback
  row pitch, readback byte count, readback buffer size, BMP row size, image
  size, and total BMP file size before mapping or writing capture artifacts. A
  Release rebuild and one-frame late engine capture smoke passed after the
  change. A later adversarial pass also made capture directory creation and
  final file writes fail cleanly with logged errors instead of throwing or
  reporting success after a failed write.
- Engine capture smoke now also rejects broad output directories before build or
  cleanup, clears stale `engine_frame_*.bmp`/summary artifacts in dedicated
  capture folders before launch, and preserves unrelated files in that folder.
  Unsafe `.`, `..`, absolute repository-root, `build\logs`, and
  `VENPOD\build\captures` probes were rejected, while one-frame cleanup and
  parent-guard probes passed in dedicated folders. Output is now restricted to
  dedicated `build\captures` or `build\logs` subfolders, with runtime/source
  descendants such as `build\bin` rejected before cleanup or launch; the public
  review verifier checks that both capture wrappers retain this guard.
- The top-level sparse regression wrapper now front-loads the same public-review
  capture parameter checks for normal engine capture, stress engine capture, and
  public demo capture. Invalid engine count, stress interval, and public-demo FPS
  probes were rejected before the sparse regression banner or build step, while a
  compact valid Release regression path still passed.
- Runtime shader compilation now bounds shader-source and shader-cache reads,
  rejects oversized include/source blobs before the DXC `UINT32` size handoff,
  initializes failed include outputs, and uses non-throwing filesystem existence
  probes for include/cache lookup. Shader cache hashes now also encode missing
  or unreadable includes, so stale compiled bytecode cannot hide a shader input
  that the current compiler invocation would reject. This closes a public-review
  hardening gap where a corrupt or accidental large shader/cache file could
  cause excessive allocation or truncated compiler input.
- Far-SVO cache loading is now size-aware before allocating cache vectors:
  cache files over the review-time cap or whose header-declared payload size
  does not exactly match the file length are ignored and rebuilt. This prevents
  corrupt local far-cache headers from forcing oversized CPU allocations before
  cache contents are proven structurally consistent.
- Sparse edit `.vsed` loading now verifies the exact byte length implied by the
  header overlay/voxel counts before reserving overlay maps or walking records,
  and rejects files over the review-time size cap. Unit coverage includes a
  header that declares a huge voxel count in a short file and verifies the
  previous edit state is preserved after rejection.
- Direct engine backbuffer capture startup now treats an unusable
  `VENPOD_CAPTURE_DIR` as a logged capture-disable condition instead of letting
  `std::filesystem::create_directories` throw during startup. The public capture
  wrappers still provide the stricter output-tree guard before this engine path
  is reached.
- Runtime numeric environment parsing now rejects partial numeric strings,
  overflow, negative values, and non-whitespace suffixes instead of accepting
  the prefix parsed by `strtoul`. Malformed public-review or diagnostic env
  settings therefore fall back to defaults instead of silently selecting a
  surprising low budget or frame count.
- Sparse startup prewarm radii from environment settings are now clamped before
  reserve sizing and signed loop bounds. The reserve computation also widens to
  `size_t`, preventing extreme numeric env values from causing startup
  allocation spikes, unsigned wraparound, or invalid signed loop casts.
- Focused upload selection now caches per-class value ordering for the current
  focus/frame until the upload queues are mutated. Repeated protected upload
  picks in the same frame no longer re-sort the same class queue for every
  popped brick, and unit coverage verifies repeated focused pops keep nearest
  visible uploads ordered correctly.
- Render-ownership and GPU physics diagnostic readbacks now validate the shader
  payload frame against the queued readback frame before publishing counters.
  Stale payloads are dropped with explicit telemetry, and the sparse regression
  public verifier now checks that these guards remain present in the source.
- GPU physics result readback now records the queued packets' expected shader
  checksums per readback slot and drops rows whose packet index or checksum does
  not match the queued frame. This gives result/proposal rows an explicit
  cross-frame stale-payload guard even though the compact GPU result ABI does not
  carry a standalone frame field.
- Page-table publish retry now refuses to replace a pending publish for the same
  table slot when the retry belongs to a different coord/page. This keeps an old
  failed retry from displacing the current replacement publish and leaving the
  GPU page table without the CPU-authoritative entry. `VENPODSparseCore` covers
  the different-page retry guard.
- Surface GPU snapshot staging now snapshots and restores the sparse face-range
  allocator on staging failure or pre-copy emit failure. A failed
  metadata/payload staging attempt can no longer retire or move face ranges that
  the GPU never received; the public verifier checks that the rollback guard
  remains present in the source.
- Sparse surface GPU copy emission now preflights every full-buffer and partial
  copy region against the upload ring and destination buffer capacities before
  issuing D3D12 copies or mutating residency/mirror state. Failed pre-copy
  validation restores staged upload/allocator state and is pinned by
  `VENPODSparseCore` copy-range tests plus the public source verifier.
- Sparse voxel GPU copy emission now uses the same explicit upload/destination
  bounds contract for brick payloads, partial brick ranges, page-table entries,
  GPU physics packets, edit deltas, and mid-clipmap uploads. Malformed tickets
  fail before D3D12 copy submission, are reported as upload overflow telemetry,
  and are pinned by copy-range unit coverage plus source verifier snippets.
- Legacy dense brush/ground raycast readback slots are now one-shot and
  producer-frame guarded. A skipped queue or repeated retire cannot reuse a
  stale modulo-slot payload as a fresh brush or grounding result, and
  `VENPODSparseCore` covers the readback lifecycle helper contract.
- The launcher now passes the monotonic `frameCount` into dense/GPU
  brush/ground raycast readback queue and retire calls, while still using the
  swapchain index only for the local metadata arrays. This keeps the freshness
  guard from mistaking valid three-frame-old readbacks for same-frame payloads;
  the public verifier pins the call-site contract.
- Legacy dense brush/ground raycast readbacks now decode the packed GPU word via
  byte copy instead of type-punning through a `float*`. This keeps the fallback
  raycast path defined under Release optimization, and the public verifier pins
  the helper so the aliasing hazard does not return.
- Sparse surface GPU cull-stat readbacks now also record their producer frame
  and reject same-frame retirement, matching the one-shot readback freshness
  pattern used by sparse feedback/readback paths. The runtime call sites now
  pass the monotonically increasing frame counter instead of the swapchain
  backbuffer index so the guard does not suppress valid three-frame-later
  readback retirement.
- Sparse surface GPU-visible draw, record, and cluster structs now have
  offset-level ABI coverage in `VENPODSparseCore`, not only total-size static
  asserts. The public verifier also pins the matching C++ and HLSL struct
  definitions so future shader/runtime field drift is caught during review
  validation instead of showing up as corrupted indirect draws.
- The frame-constants CPU mirror now has compile-time offset assertions for
  every shader-visible field, and the shared HLSL comment for
  `farFieldGridParams.w` now matches its ownership-stat flag use in the
  raymarch and sparse-surface shaders. The public verifier pins both sides of
  this shared constant-buffer ABI.
- `GPUBuffer::InitializeWithData` now retains its temporary upload resource
  until the owning buffer is reset or destroyed. A default-heap initialization
  copy can therefore execute after the helper returns without depending on a
  released upload allocation; the public verifier pins this lifetime guard.
- The fullscreen raymarch root signature no longer carries an unused `b1`
  32-bit constants parameter. The public verifier pins the expected late sparse
  descriptor bindings and rejects that stale root-constant parameter so shader
  register/root-index drift is caught in review.
- Sparse edit persistence now refuses to save or load through non-`.vsed`
  paths. This prevents the pause-menu or `VENPOD_SPARSE_EDIT_FILE` path from
  accidentally truncating arbitrary local files while preserving the documented
  sparse edit format; `VENPODSparseCore` verifies rejection leaves an existing
  non-`.vsed` file untouched.
- Public cleanup now resolves and guards every `clean.ps1` removal target before
  recursive deletion. The script refuses to clean the project root or anything
  outside the VENPOD project tree, and the public-review verifier pins the guard
  snippets alongside the capture-script cleanup guards.
- Public setup now uses a unique ImGui extraction temp directory and guards
  project-tree deletion targets before replacing `vendor/imgui` or cleaning the
  build directory. The verifier pins those setup-script guards as part of the
  public script surface.
- Sparse surface range retirement tokens are now monotonic. A regressed frame or
  fence token cannot stamp newly freed surface ranges with an older retirement
  point and make face-buffer memory reusable before the original GPU-safe
  horizon; `VENPODSparseCore` covers the non-monotonic token case.
- Sparse regression runtime logs are now scanned for critical/error,
  device-removed, timeout, and explicit sparse readiness/ownership failure
  markers across sparse smoke stages, not only the dense fallback stage. A
  compact valid Release regression path passed with the stricter render and
  GPU-physics log checks enabled.
- GPU physics proposal apply now rejects edit-delta status payloads that carry no
  sampled edit revision at all. `VENPODSparseCore` covers the inconsistent
  status case, and a targeted sparse regression subset passed with GPU physics
  proposal/readback active after the change.
- GPU physics proposal apply now also rejects malformed expected-page status
  payloads that carry no valid expected page index/generation. `VENPODSparseCore`
  covers this fail-closed case, and a targeted sparse regression subset passed
  with GPU physics proposal/readback active after the change.
- GPU physics proposal apply now rejects malformed source or destination local
  coordinates outside the 16^3 sparse brick bounds before converting them to
  world coordinates. `VENPODSparseCore` covers a payload that would otherwise
  mutate an outside-brick voxel, and a targeted sparse regression subset passed
  with GPU physics proposal/readback active after the change.
- GPU physics proposal apply now requeues proposal sources when the CPU apply
  budget is zero instead of dropping retired GPU work. `VENPODSparseCore` covers
  the zero-budget no-mutation/no-rejection path, and a targeted Release
  regression passed with render/backend plus GPU-physics smokes active.
- Staged sparse physics packets now survive backpressure before proposal apply:
  local zero-move execution requeues staged work, and GPU-apply mode requeues
  staged packets when the GPU packet submit path does not actually dispatch.
  `VENPODSparseCore` covers the local zero-move requeue path, and a targeted
  Release regression passed with render/backend plus GPU-physics smokes active.
- GPU brush-feedback apply now uses a shared completeness guard: only
  non-overflowed payloads with zero missing-resident header count and zero
  observed missing-resident sentinel records may commit GPU edit records.
  `VENPODSparseCore` covers complete, missing-resident, and overflowed payload
  decisions, and a targeted sparse regression subset passed after the change.
- Authoritative GPU brush feedback now CPU-replays the matching pending stroke
  if the retired readback payload is stale. The GPU readback stats expose the
  queued frame that was dropped, the launcher pops only that frame's pending
  authoritative stroke, and a targeted Release regression passed with the
  brush-feedback authoritative smoke active.
- The sparse regression wrapper now positively verifies brush-feedback evidence
  instead of only scanning for failures. Observe/apply/authoritative stages must
  complete the seven-case parity diagnostic suite; apply modes must observe GPU
  edit commits and missing-resident CPU fallback; authoritative mode must also
  prove a pending stroke completed through GPU apply. A targeted Release
  regression passed with all three brush-feedback stages active.
- The sparse render/backend smoke now positively verifies mid/far continuity
  evidence instead of only relying on absence of ownership failures. The gate
  requires active mid-clipmap coverage/work telemetry, nonzero mid terrain
  ownership, meaningful simultaneous mid/far terrain ownership, and visible
  far-SVO ownership in `PERF_RENDER_OWNERSHIP`. A
  compact Release regression passed with the verifier, render/backend smoke, the
  new mid/far assertion, and GPU-physics smoke active.
- The sparse GPU-physics smoke now positively verifies GPU physics evidence
  instead of only scanning for failure markers. The gate requires sparse GPU
  physics to be enabled, at least one staged GPU physics packet, at least one GPU
  result/proposal readback, a nonzero CPU-authoritative GPU proposal apply, and
  no edit/physics upload overflow. A compact Release regression passed with the
  verifier, render/backend smoke, and the new GPU-physics assertion active.
- The default local sparse-physics smoke now positively verifies sparse
  body-collision authority instead of only checking physics motion. The gate
  requires the sparse collision backend to be active, body-collision voxel
  sampling to occur, solid sparse support to be observed, and grounded/landing
  state to retire in telemetry. A targeted Release regression passed with the
  verifier, render/backend smoke, default local-physics smoke, and GPU-physics
  smoke active.
- The sparse GPU-raycast smoke now positively verifies raycast health instead
  of only scanning for failure markers. The gate requires the
  `SPARSE_GPU_RAYCAST health observed` marker, at least the configured accepted
  hit count, and a fallback percentage no higher than the configured maximum. A
  targeted Release regression passed with the verifier, render/backend smoke,
  GPU-raycast smoke, and GPU-physics smoke active.
- The seeded-surface smoke now positively verifies the sparse surface GPU path
  instead of only relying on surface-fragment failure markers and edit-file
  persistence. The gate requires the diagnostic seed, nonzero GPU
  faces/draws/records/resident payload, GPU cull dispatch with accepted draws,
  stable+compact draw mode, raster fragments, a settled frame with no staged,
  deferred, allocation-failure, retry, or overflow backlog, and no surface
  overflow. A
  targeted Release regression passed with the verifier, render/backend smoke,
  seeded-surface smoke, persisted `.vsed` verification, and GPU-physics smoke
  active.
- The miss-feedback smoke now positively verifies the feedback pressure signal
  and runtime sampling plan instead of only checking that an ownership-pressure
  line exists. The gate requires nonzero pending miss feedback in `PERF_SPARSE`,
  nonzero effective ownership pressure, positive feedback grid/distance/stride
  values in `PERF_SPARSE_OWNERSHIP_PRESSURE`, and zero
  `missFbStale`/`missFbOverflow` status in `PERF_SPARSE`. A targeted Release regression
  passed with the verifier, render/backend smoke, miss-feedback smoke, and
  GPU-physics smoke active.
- Sparse edit persistence now rejects empty overlay records and non-empty
  overlays with zero revision during load, matching the verifier's overlay
  contract. `VENPODSparseCore` covers both malformed files and verifies that
  failed loads preserve existing overlays.
- Sparse edit revisions now avoid the reserved zero revision when a per-brick
  edit stream reaches the `uint32_t` ceiling. The store resets that brick to a
  fresh nonzero revision epoch, drops stale queued GPU deltas for the same brick,
  and republishes the complete edited overlay under the new epoch. Sparse brush
  preview deltas use the same nonzero epoch rule without mutating the overlay.
  `VENPODSparseCore` covers the near-saturated load, maximum-revision delta,
  complete-overlay epoch reset, and saturated-preview path. A targeted Release
  sparse regression subset passed afterward with render/backend, brush feedback
  observe/apply/authoritative, and GPU physics proposal/readback smokes active.
- Sparse edit-delta batching now avoids staging stale duplicate voxel revisions
  when the pending edit queue exceeds the GPU delta cap. Oversized batches
  coalesce duplicate `BrickCoord`/local entries to the newest revision before
  applying the upload cap, report whether the result was genuinely truncated,
  and let the runtime clear pending queues that were fully represented after
  coalescing. `VENPODSparseCore` covers true truncation versus fully represented
  duplicate coalescing, range-cap truncation, and invalid range-table capacity.
  GPU edit-delta overflow telemetry now follows the true truncation flag instead
  of treating fully represented duplicate coalescing as data loss. A compact
  Release sparse regression passed with render/backend plus GPU physics
  proposal/readback smokes active.
- Sparse edit-delta batching now narrows input sizes through an explicit
  saturating helper and reports failed range-table insertion as both overflowed
  and truncated, so too-small GPU lookup tables cannot masquerade as a complete
  represented edit payload.
- Sparse edit-delta GPU upload staging now uses overflow-aware range-table
  sizing and aligned offset arithmetic. The dynamic range-table target is
  computed with wide intermediates, initialization rejects impossible 2x
  capacity relationships before they can wrap, and edit-delta staging fails
  closed if any aligned upload offset addition would overflow. The same
  aligned-append helper now protects brick-upload, mid-clipmap, page-table,
  physics-packet, and edit-delta admission/staging offsets, so impossible
  wrapped byte ranges cannot be accepted or emitted.
  A Release rebuild, `VENPODSparseCore`, and compact sparse regression passed
  after the change.
- Sparse surface GPU staging now uses the same overflow-aware aligned range
  pattern for payload patches, changed metadata blocks, and fallback full
  snapshot uploads. Face source/destination range validation also widens before
  addition, so malformed or extreme surface spans fail closed before upload
  copies are emitted. A Release rebuild, `VENPODSparseCore`, and targeted sparse
  regression with seeded surface smoke/edit persistence active passed after the
  change.
- Sparse surface indexed-IA stream creation now rejects `maxFaces` capacities
  whose vertex or index stream byte totals would narrow when written into
  D3D12's 32-bit buffer-view sizes. The sizing rule is shared with a
  device-free unit test covering zero, nominal, max-valid, and one-past-valid
  capacities. A Release rebuild, `VENPODSparseCore`, and targeted sparse
  regression with seeded surface smoke/edit persistence active passed after the
  change.
- Far SVO staged uploads now validate aggregate byte totals, per-stage offsets,
  default/upload buffer capacities, pending copy ranges, and upload progress
  counter updates before emitting GPU copies. A Release rebuild,
  `VENPODSparseCore`, and compact sparse regression passed with far SVO
  readiness observed at full upload/page coverage.
- Far SVO cache load/save now validates page-grid sizing, cache element counts,
  stream byte counts, vector/stat count narrowing, and cache read/write sizes.
  Corrupt or oversized cache headers fall back to rebuild instead of forcing
  unsafe allocations or narrowed stream operations. A Release rebuild,
  `VENPODSparseCore`, and compact sparse regression passed with far SVO
  readiness observed at full upload/page coverage.
- Shared RHI buffer helpers now reject wrapped upload byte ranges, null initial
  data, oversized SRV/UAV element counts, and CBV sizes that cannot fit D3D12's
  32-bit view fields. A Release rebuild, `VENPODSparseCore`, and compact sparse
  regression passed after the change.
- Sparse page-table publish staging now rejects invalid and tombstone physical
  page sentinels before enqueue/retry, and older same-page generations cannot
  replace newer pending publishes through either enqueue replacement or retry.
  `VENPODSparseCore` covers invalid page, tombstone page, zero generation,
  invalid retry publishes, stale same-page replacement rejection, and stale
  same-page retry rejection.
- Sparse collision AABB/sweep/support queries now reject non-finite,
  out-of-range, and oversized inputs before voxel scanning. AABB/sweep failures
  are treated as unknown-blocked, support failures return no sampled support, and
  `VENPODSparseCore` covers the malformed bounds, infinite delta, and oversized
  scan cases.
- Sparse runtime budget arithmetic now saturates scaled budgets, request
  admission totals, processing catch-up, physics move catch-up, byte-limited
  defer accounting, and opportunistic far-upload doubling instead of allowing
  extreme counters to wrap. `VENPODSparseCore` covers scaled-budget saturation,
  request total saturation, byte-limited defer saturation, and far-upload
  trickle expansion saturation. The latest full Release sparse regression passed
  after the change.
- Sparse world-to-brick coordinate conversion now uses wide intermediates for
  floor division and modulo, so `int32_t` minimum world coordinates cannot
  overflow during brick/local conversion. `VENPODSparseCore` covers extreme
  signed-world coordinates, and a targeted Release sparse regression passed with
  render/backend and GPU-physics smokes active after the change.
- Mid-range ownership metadata now fails closed when the near-owned volume
  exhausts the configured mid clipmap range. `VENPODSparseCore` covers both the
  old false-positive ray segment and the disabled transition-metadata case, and
  a Release rebuild plus focused sparse core test passed after the change.

Latest generated measurements:

- seeded sparse edit persistence: 8 overlays, 405 voxels, 2622 bytes;
- normal engine capture: six nonblank 1920x1080 frames; runtime ownership
  `samples=125`, `minTerrain=62.24%`, `miss=0`, `unsafeNearMiss=0`,
  `farSvo=39814`;
- stress-camera capture: five nonblank 1920x1080 frames; runtime ownership
  `samples=100`, `minTerrain=89.01%`, `miss=0`, `unsafeNearMiss=0`,
  `farSvo=103410`;
  fast-request telemetry `samples=3`, `scale=4`, `spec/vis/coll=24/130/130`,
  `total=208`, `skips=0/0/0`;
- public demo MP4: `1280x720`, 16 frames, 117045 bytes; runtime ownership
  `samples=24`, `minTerrain=62.24%`, `miss=0`, `unsafeNearMiss=0`,
  `farSvo=39813`.
- compact GPU-physics runtime subset after coordinate-overflow hardening:
  render/backend smoke passed fast-request and mid/far continuity gates, and
  GPU-physics smoke reported `wellFormedStatus=True`, `missingBelow=0`,
  `rejects=0`.
- compact render/backend and GPU-physics runtime subset after the shared
  checked-conversion/surface-extractor hardening: render smoke still reported
  visible far SVO, fast-request telemetry `skips=0/0/0`, simultaneous mid/far
  ownership, and GPU physics still reported `wellFormedStatus=True`,
  `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after terrain,
  local-physics, and surface-cache conversion hardening: render smoke passed
  visible far SVO, fast-request, and mid/far continuity gates, and GPU physics
  again reported `wellFormedStatus=True`, `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after sparse collision
  sweep-step hardening: render smoke reported visible far SVO, fast-request
  telemetry `samples=2`, `scale=4`, `spec/vis/coll=24/130/130`, `total=208`,
  `skips=0/0/0`, and GPU physics again reported `wellFormedStatus=True`,
  `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after scenic-spawn input
  hardening: render smoke again reported visible far SVO, fast-request
  telemetry `samples=2`, `scale=4`, `spec/vis/coll=24/130/130`, `total=208`,
  `skips=0/0/0`, and GPU physics again reported `wellFormedStatus=True`,
  `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after CPU sparse raycast
  input hardening: render smoke reported visible far SVO, fast-request
  telemetry `samples=2`, `scale=4`, `spec/vis/coll=24/130/130`, `total=208`,
  `skips=0/0/0`, and GPU physics again reported `wellFormedStatus=True`,
  `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after sparse brush input
  hardening: render smoke reported visible far SVO, fast-request telemetry
  `samples=2`, `scale=4`, `spec/vis/coll=24/130/130`, `total=208`,
  `skips=0/0/0`, and GPU physics again reported `wellFormedStatus=True`,
  `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after
  collision/brush-residency planner input hardening: render smoke reported
  visible far SVO, fast-request telemetry `samples=2`, `scale=4`,
  `spec/vis/coll=24/130/130`, `total=208`, `skips=0/0/0`, and GPU physics
  again reported `wellFormedStatus=True`, `missingBelow=0`, `rejects=0`.
- compact render/backend and GPU-physics runtime subset after basic/stress
  request-planner radius hardening: render smoke reported visible far SVO,
  fast-request telemetry `samples=2`, `scale=4`,
  `spec/vis/coll=24/130/130`, `total=208`, `skips=0/0/0`, and GPU physics
  again reported `wellFormedStatus=True`, `missingBelow=0`, `rejects=0`.

Tracked visual evidence:

- [docs/media/sparse-engine-contact-sheet.png](../media/sparse-engine-contact-sheet.png)

Generated evidence from the latest pass:

- `VENPOD/build/logs/sparse_surface_edits.vsed`
- `VENPOD/build/logs/dense_legacy_smoke.log`
- `VENPOD/build/logs/sparse_engine_capture/`
- `VENPOD/build/logs/sparse_stress_engine_capture/`
- `VENPOD/build/logs/public_demo_capture/`

## Remaining Limits

These are not hidden by the green gate:

- GPU brush feedback and GPU physics proposal application remain guarded hybrid
  paths. CPU sparse authority is still the fallback for correctness.
- Mid/far terrain now has positive coverage and ownership gates, but final
  long-distance LOD polish remains future work.
- Dense legacy remains in tree for fallback and comparison, and the regression
  gate now proves it still selects the dense backend with sparse raymarch
  disabled.
- Generated MP4/log/capture artifacts are intentionally not tracked in git.

## Audit Conclusion

The sparse refactor is covered for public review: the default sparse demo path,
regression gates, media capture path, docs, and known limitations are explicit.
The remaining limits above prevent claiming that every future sparse feature is
finished, but they are documented and guarded rather than unverified.
