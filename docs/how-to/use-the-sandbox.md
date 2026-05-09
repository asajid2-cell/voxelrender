# Use The Sandbox

The sandbox is the main VENPOD tech demo. It shows sparse brick terrain,
surface-authoritative near-field rendering, persistent brush editing, local
sparse physics, mid/far continuity, and runtime diagnostics.

## Launch Sandbox Mode

```powershell
cd VENPOD
.\rebrun.ps1
```

In the launcher, select `Sandbox Mode`.

`rebrun.ps1` launches the sparse renderer by default. For dense legacy
comparison:

```powershell
.\rebrun.ps1 -DenseLegacy
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

Painting targets world-space sparse bricks. GPU sparse raycast and brush
feedback diagnostics are available, while the CPU sparse path remains the
resilient authority for ordinary interaction.

To keep sparse edits across runs, launch with an edit file:

```powershell
.\rebrun.ps1 -SparseEditFile saves\review-edits.vsed
```

The file is loaded when the sparse sandbox starts and saved on shutdown.
When the pause menu is open, the metrics panel also exposes a sparse edit path
field with `Save Edits` and `Load Edits` buttons for manual save/load.

For traversal, hold left mouse while aiming along a path. When the painted path
gets close to the player, VENPOD hands off into a short foot-ramp placement so
the final painted voxels become walkable instead of blocking the camera.

## What To Look For

- Extreme vertical terrain: cliffs, shelves, spires, ravines, basins, and cave
  openings.
- World-space sparse brick residency while moving or flying.
- Persistent runtime brush edits across sparse brick eviction and reload.
- The metrics overlay reporting resident/tracked pages, upload budgets,
  ownership counters, mid/far coverage, brush feedback, physics budget, and far
  SVO state.

## Current Limits

- Brush edits persist during the session and can be saved/loaded with
  `.\rebrun.ps1 -SparseEditFile` or the pause-menu metrics panel.
- Dense legacy remains available as a fallback and regression comparison.
- GPU brush feedback and GPU physics proposal application are guarded hybrid
  paths; CPU sparse authority remains the resilience fallback.
- Mid/far terrain is visual continuity, while near sparse bricks remain the
  editable and collision-critical authority.
