# Use The Sandbox

The sandbox is the main VENPOD tech demo. It shows vertical terrain streaming,
raymarched voxel rendering, persistent brush editing, chunk-budgeted physics,
and runtime diagnostics.

## Launch Sandbox Mode

```powershell
cd VENPOD
.\run.ps1
```

In the launcher, select `Sandbox Mode`.

For a rebuild-and-run loop:

```powershell
.\rebrun.ps1
```

## Move Around

| Input | Action |
| --- | --- |
| `WASD` | Move horizontally |
| Mouse | Look |
| `Space` | Jump |
| Double-tap `Space` | Toggle flight mode |
| `Space` in flight mode | Fly up |
| `Shift` in flight mode | Fly down |
| `V` | Toggle first-person / third-person camera |
| `Tab` | Toggle mouse capture |
| `Esc` | Open or close the pause menu |

## Paint And Erase Voxels

| Input | Action |
| --- | --- |
| Left mouse | Paint with the selected material |
| Right mouse | Erase |
| `Q` / `E` | Change material |
| `[` / `]` | Change brush radius |

Painting uses GPU raycasting to find the target voxel. The brush compute shader
dispatches over the brush bounds, not the full render buffer.

For traversal, hold left mouse while aiming along a path. When the painted path
gets close to the player, VENPOD hands off into a short foot-ramp placement so
the final painted voxels become walkable instead of blocking the camera.

## What To Look For

- Extreme vertical terrain: cliffs, shelves, spires, ravines, basins, and cave
  openings.
- World-space chunk streaming while moving or flying.
- Persistent runtime brush edits across render-window recentering.
- The metrics overlay reporting visible chunks, loaded/generated chunks,
  generated voxel capacity, brush feedback, physics budget, and far SVO state.

## Current Limits

- Brush edits persist during the session but are not saved to disk by default.
- The sparse voxel octree far field is visual-only.
- Infinite physics is conservative by default so the sandbox stays responsive.
