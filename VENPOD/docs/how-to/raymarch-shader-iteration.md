# PS_Raymarch Shader Iteration

Use this workflow when editing `assets/shaders/Graphics/PS_Raymarch.hlsl`.
It avoids stale runtime shader files and makes the expensive cache miss explicit.

## Refresh Only

```powershell
.\raymarch_shader_iter.ps1 -RefreshOnly
```

This copies `assets` into `build/bin/assets` and fails if source
`PS_Raymarch.hlsl` differs from the runtime copy.

## Warm Cache, Then Capture Frame 300

```powershell
.\raymarch_shader_iter.ps1 -WarmRaymarch -CaptureFrame300 -CaptureName raymarch_shader_iter_validation
```

The warm run captures frame 1 and prints whether `PS_Raymarch.hlsl` was a cache
hit or miss. If it was a miss, this is where the full DXC compile cost is paid.
The frame-300 run then uses the warmed cache and prints `PS_Raymarch cache: HIT`.

## Capture Only From Existing Cache

```powershell
.\raymarch_shader_iter.ps1 -CaptureFrame300 -CaptureName raymarch_frame300_cached
```

This still refreshes assets and checks source/runtime parity before running.

## Notes

`rebrun.ps1 -NoBuild` now refreshes runtime assets before launch. This prevents
false positives where source shaders were edited but `build/bin/assets` still
contained an older shader.
