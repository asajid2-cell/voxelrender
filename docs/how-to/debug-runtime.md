# Debug Runtime Behavior

VENPOD keeps expensive diagnostics off by default so the public demo runs smoothly.

## Enable Runtime Diagnostics

Set `VENPOD_DIAGNOSTICS` before launching:

```powershell
cd VENPOD
$env:VENPOD_DIAGNOSTICS = "1"
.\rebrun.ps1
```

This creates `build/bin/venpod_runtime.log` and enables debug-level engine logging.

The same can be done through the rebuild/run helper:

```powershell
.\rebrun.ps1 -Diagnostics
```

## Enable DirectX 12 Debug Validation

DirectX validation is useful for resource-state bugs, but it can make the demo much slower.

```powershell
$env:VENPOD_D3D_DEBUG = "1"
.\rebrun.ps1
```

Use this only while investigating rendering or synchronization issues.

## Disable Physics For Isolation

```powershell
$env:VENPOD_DISABLE_PHYSICS = "1"
.\rebrun.ps1
```

If a bug disappears with physics disabled, start in `SparseVoxelWorld`,
`SparseRuntimeBudget`, `PhysicsDispatcher`, or `CS_SparsePhysicsPackets.hlsl`
for sparse mode. Dense legacy physics still uses the older chunk scanner and
gravity shaders.

Sparse local physics is default-on in sparse runtime mode. GPU sparse physics
proposal application is still a guarded diagnostic path.

## Disable The Far SVO

```powershell
$env:VENPOD_DISABLE_FAR_SVO = "1"
.\rebrun.ps1
```

Use this to separate far SVO ownership from sparse near surfaces and mid
clipmap/far-height fallback.

## Use Static Chunks

```powershell
$env:VENPOD_STATIC_CHUNKS = "1"
.\rebrun.ps1 -DenseLegacy
```

This bypasses the infinite streaming path and copies a fixed 2x2 chunk patch. It is useful for separating shader/camera issues from chunk streaming issues.

## Run Sparse Regression

```powershell
.\sparse_regression.ps1 -Config Release
```

This runs the combined sparse gate: render/backend readiness, flicker stability,
surface fragments, GPU raycast health, miss feedback, brush feedback/apply,
sparse edit persistence, GPU physics diagnostics, and engine backbuffer capture.

## Common Symptoms

Terrain starts far away:

In sparse mode, check `SparseBrickRequestPlanner`, miss feedback telemetry, and
`PERF_SPARSE_OWNERSHIP_PRESSURE`. In dense legacy mode, check chunk queue
ordering in `InfiniteChunkManager`.

A hitch appears after walking for a while:

Check frame pressure, upload byte defers, page-table publish backlog, and
surface extraction/culling counters in `PERF_SPARSE`. In dense legacy mode,
check render-window recentering in `VoxelWorld`.

Painting causes a stall:

Check sparse brush feedback, dirty render regions, and local physics wakeup
regions. Broad full-brick refreshes should be limited to first publication or
broad edits.

Far terrain looks wrong but nearby terrain is stable:

Disable the SVO with `VENPOD_DISABLE_FAR_SVO=1`. If the issue disappears, start
with `FarVoxelOctree` and the `RaymarchSparseFarField` path in
`PS_Raymarch.hlsl`. If the issue remains, inspect mid clipmap coverage
(`midCov`) and ownership counters for far-height fallback, sky, miss, and
unsafe near miss.
