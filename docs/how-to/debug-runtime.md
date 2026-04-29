# Debug Runtime Behavior

VENPOD keeps expensive diagnostics off by default so the public demo runs smoothly.

## Enable Runtime Diagnostics

Set `VENPOD_DIAGNOSTICS` before launching:

```powershell
cd VENPOD
$env:VENPOD_DIAGNOSTICS = "1"
.\run.ps1
```

This creates `build/bin/venpod_runtime.log` and enables debug-level engine logging.

## Enable DirectX 12 Debug Validation

DirectX validation is useful for resource-state bugs, but it can make the demo much slower.

```powershell
$env:VENPOD_D3D_DEBUG = "1"
.\run.ps1
```

Use this only while investigating rendering or synchronization issues.

## Disable Physics For Isolation

```powershell
$env:VENPOD_DISABLE_PHYSICS = "1"
.\run.ps1
```

If a bug disappears with physics disabled, start in `PhysicsDispatcher`, `CS_ChunkScanner.hlsl`, or `CS_GravityChunk.hlsl`.

## Use Static Chunks

```powershell
$env:VENPOD_STATIC_CHUNKS = "1"
.\run.ps1
```

This bypasses the infinite streaming path and copies a fixed 2x2 chunk patch. It is useful for separating shader/camera issues from chunk streaming issues.

## Common Symptoms

Terrain starts far away:

Check chunk queue ordering in `InfiniteChunkManager`. Nearby chunks should be queued before far-edge chunks.

A hitch appears after walking for a while:

Check render-window recentering in `VoxelWorld`. Recentering clears chunk-copy caches and should not happen every few chunks.

Painting causes a stall:

Check that `CS_Brush.hlsl` is dispatching over a brush-local bounding box, not the full render buffer.
