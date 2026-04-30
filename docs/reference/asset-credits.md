# Asset Credits

## Block Character Reference

The in-game third-person avatar is a procedural block character rendered in
`PS_Raymarch.hlsl`. It is not a bundled mesh file; it is built from simple
oriented cuboids so it works with VENPOD's existing raymarch renderer.

The proportions and style are based on Kenney's Blocky Characters asset pack:

- Source: https://www.kenney.nl/assets/blocky-characters
- License: Creative Commons CC0
- Credit: Kenney.nl

The implementation is intentionally procedural to avoid adding a mesh loader,
animation system, or third-party model dependency to the demo.
