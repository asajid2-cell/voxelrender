# GPU Terrain Generation — Implementation Plan

**Goal:** Move mid-voxel clipmap terrain generation from CPU worker threads (~10 cores, the
streaming bottleneck) to a GPU compute shader (thousands of lanes) → bigger render distance +
instant streaming. Targets the **mid-voxel clipmap layer** (the coarse LOD the raymarch reads at
mid distance), which is self-contained.

**Key enabler:** `PS_Raymarch.hlsl::FarTerrainHeight` (~line 637) is already a near-exact HLSL
port of C++ `SparseTerrainGenerator::HeightAt` (`SparseTerrainGenerator.cpp:88`). This is a
**parity port**, not a rewrite. The DX12 compute plumbing is well-precedented by
`PhysicsDispatcher` (`CreateInitializePipeline:347`, `DispatchInitialize:197`).

## Architecture

```
CPU pump decides WHICH brick coords are needed (unchanged; cheap residency policy)
  → emits compact list {ring,x,y,z,destSlot} into a request buffer (SRV)
  → CS_GenerateMidVoxelBricks  Dispatch(requestCount): 1 group/brick, threads = Y-cells
      - reads request → origin, cellSize from ring
      - calls TerrainHeight.hlsli (HeightAt + relief + FULL material classifier)
      - writes 4096 uints into MidVoxelClipmapSamples[destSlot*4096..] (UAV)
      - writes metadata[destSlot+1] + lookup entry (UAV)
  → UAV→SRV barrier
  → PS_Raymarch SampleResidentMidVoxel reads identical layout (UNCHANGED)
```
No CPU `GenerateVoxelBrickPayload`, no `BuildGpuSnapshot` voxel packing, no upload-ring copy for
GPU-generated bricks. CPU keeps residency/eviction policy (cheap); GPU owns content generation
(expensive).

## Exact GPU layout the CS must write (mirror `BuildGpuSnapshot` SparseClipmap.cpp:5682-5832)
- **MidVoxelClipmapMetadata** (t13, uint4[(maxBricks+1)]): `[0]`=header `{MAGIC=0x56435658, 16,
  brickCount, (lookupCapacity&0xFFFFFF)|(ringCount<<24)}`; `[b+1]`=`{originX,originY,originZ,
  (ring&0xFF)|(cellSize<<8)}`.
- **MidVoxelClipmapLookup** (t14, uint4[lookupCapacity], open-addressed, FNV hash over
  (ring,x,y,z)): `{x,y,z, ((ring&0xFF)<<24)|((compactIndex+1)&0xFFFFFF)}`; w==0 = empty.
- **MidVoxelClipmapSamples** (t15, uint[]): brick `b` → `[b*4096 .. b*4096+4095]`, voxel index
  `x + y*16 + z*256`. Per-voxel = `Utils::PackVoxel(material,variant,light,stateFlags)`.
- Mid-voxel layer does NOT use the occupancy words (only the full-res path does).

## Parity divergences to reconcile (Phase 0 — the real risk)
1. **Seed:** C++ `m_seed=12345`; HLSL `FarWorldSeed()=asuint(frame.exactNearParams.y)`. Pass
   `m_terrain.Seed()` as a CS constant; assert equality at init.
2. **Material classifier NOT ported:** HLSL `FarTerrainMaterial` (~913) is a simplified 5-branch;
   CPU `SampleGeneratedVoxelWithColumn` (`SparseTerrainGenerator.cpp:628`) is ~10 branches. Must
   port the FULL classifier.
3. **Relief:** CPU uses 4-tap `SurfaceReliefAtWithCenter` (`:242`) — replicate exactly.
4. **Representative-Y per coarse cell:** mid path `sampleColumnCellVoxel`
   (`SparseClipmap.cpp:5143-5205`) picks a representative Y per cell (terrain-top vs preferred-Y,
   water band) — replicate, don't just sample center.
5. **float determinism:** floor/saturate/lerp ordering, (int)floor on negatives — bit-check.
6. **Edit overlays:** v1 keeps edited bricks on CPU path (skip GPU gen if edits touch coord).

## DX12 infra gap
Mid-voxel buffers (`SparseVoxelGpuResources.cpp:283-325`) are SRV-only, COPY_DEST↔SRV. Need:
add `BufferUsage::UnorderedAccess`, `CreateUAV`, accessors, and a UAV↔SRV transition each frame
(mirror `:2707-2738`). `BufferUsage::UnorderedAccess=32` + `CreateUAV` already exist.

## Phases
- **Phase 0 — POC + byte-parity (3–5 days).** Create `Common/TerrainHeight.hlsli` (single source
  of truth: extract noise/relief/`FarTerrainHeight` from PS_Raymarch + port full
  `SampleGeneratedVoxelWithColumn` + representative-Y as `MidVoxelSampleCell`). Create
  `Compute/CS_GenerateMidVoxelBricks.hlsl`. Dev readback path (gate `VENPOD_GPU_MIDGEN_POC=1`),
  `memcmp` GPU brick vs `GenerateVoxelBrickPayload` across flat/mountain/shoreline/submerged/
  bedrock coords until byte-identical. Use heightdump for height-only parity first.
- **Phase 1 — full GPU gen behind flag, CPU fallback (1.5–2.5 wk).** Add UAVs to mid buffers; new
  `MidVoxelGpuGenerator` class (model on PhysicsDispatcher); `enableGpuMidVoxelGeneration` config;
  pump allocates slot+lookup but enqueues a request instead of CPU-filling; branch
  `main_launcher.cpp:13758,15242` to dispatch+barrier. Start with CPU building the small (~512)
  metadata+lookup while CS fills samples; A/B `capvis` pixel-identical.
- **Phase 2 — full GPU residency + grow (2–3 wk).** Lookup/metadata writes into CS; grow
  `maxVoxelBricks` 512→4096–16384 (HLSL cap `MID_VOXEL_CLIPMAP_MAX_BRICKS=16384` already allows),
  grow radius/ringCount; predictive prefetch along velocity; race hardening (double-buffer request
  buffer, UAV barriers, slot-not-read-before-gen).
- **Phase 3 — retire CPU mid-gen (~1 wk, optional).**

**Total ~5–7 weeks; usable verifiable win after Phase 1 (~2–3 wk in). Biggest risk = Phase 0
parity, not the DX12 plumbing.**

## Files
- CREATE: `assets/shaders/Common/TerrainHeight.hlsli`, `assets/shaders/Compute/CS_GenerateMidVoxelBricks.hlsl`, `src/Graphics/MidVoxelGpuGenerator.{h,cpp}`
- CHANGE: `src/Graphics/SparseVoxelGpuResources.{h,cpp}` (UAVs), `src/Simulation/SparseClipmap.{h,cpp}` (flag + request path), `src/main_launcher.cpp` (dispatch branch + barrier)
- REFERENCE (parity ground truth): `src/Simulation/SparseTerrainGenerator.cpp` (HeightAt:88, SampleGeneratedVoxelWithColumn:628), `src/Simulation/SparseClipmap.cpp` (GenerateVoxelBrickPayload:4842, BuildGpuSnapshot:5642), `src/Simulation/PhysicsDispatcher.cpp` (dispatch pattern)

## Verification tooling
`capvis.ps1` (A/B pixel diff), heightdump (height-only parity), per-frame byte-compare in POC.
