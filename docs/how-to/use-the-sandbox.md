# Use The Sandbox

The sandbox is the main VENPOD tech demo. It shows infinite terrain streaming, raymarched voxel rendering, physics, and brush editing.

## Launch Sandbox Mode

```powershell
cd VENPOD
.\run.ps1
```

In the launcher, select `Sandbox Mode`.

## Move Around

- `WASD`: Move horizontally
- Mouse: Look
- `Space`: Jump
- Double-tap `Space`: Toggle flight mode
- `Space` in flight mode: Fly up
- `Shift` in flight mode: Fly down
- `Tab`: Toggle mouse capture
- `Esc`: Open or close the pause menu

## Paint And Erase Voxels

- Left mouse: Paint with the selected material
- Right mouse: Erase
- `Q` / `E`: Change material
- `[` / `]`: Change brush radius

Painting uses GPU raycasting to find the target voxel. The brush compute shader only dispatches over the brush bounds, so painting remains local instead of scanning the full render buffer.

## Read The Scene

The terrain is generated as `64 x 64 x 64` voxel chunks. The visible render window is centered around the player and holds `25 x 2 x 25` chunks. VENPOD keeps a larger loaded chunk budget around that window so nearby terrain is ready before it becomes visible.
