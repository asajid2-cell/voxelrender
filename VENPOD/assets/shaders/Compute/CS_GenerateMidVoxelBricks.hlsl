// =============================================================================
// VENPOD Phase-0 POC: GPU mid-voxel LOD brick generation.
//
// Generates one 16^3 mid-voxel LOD brick (SPARSE_BRICK_SIZE = 16, 4096 voxels)
// whose per-voxel packed values are intended to be BYTE-IDENTICAL to the CPU
// per-cell rule in SparseClipmap.cpp `sampleColumnCellVoxel` (the procedural,
// pre-post-process voxel before footprint-fill / stone->dirt skinning /
// VisualSurface flagging are applied in GenerateVoxelBrickPayload's loop).
//
// GROUND TRUTH for the cell mapping (SparseClipmap.cpp, ~line 5216-5248):
//   worldXByLocal[i]    = originX + Round((i + 0.5) * cellSize)   (cell CENTER col)
//   worldXMinByLocal[i] = originX + Floor(i * cellSize)
//   worldXMaxByLocal[i] = max(min, originX + Ceil((i+1) * cellSize) - 1)
//   (identical for Y and Z). The center column drives HeightAt/Relief; the cell
//   min/max Y bound the representative sample Y; preferredY = worldYByLocal[y].
//
// For the POC the mid ring uses an INTEGER cellSize (minCellSize = 16). With an
// integer cellSize every Round/Floor/Ceil above lands on an exact integer, so
// the mapping is exactly reproducible here.
//
// This shader inlines the `sampleColumnCellVoxel` rule (SparseClipmap.cpp
// ~line 5143-5205) using the shared parity helpers in TerrainHeight.hlsli.
// =============================================================================

#include "../Common/TerrainHeight.hlsli"

cbuffer GenParams : register(b0) {
    int4  originAndCell;   // x=originX, y=originY, z=originZ, w=cellSize
    uint4 misc;            // x=seed, y=brickCount (=1 for POC), z=pad, w=pad
};

RWStructuredBuffer<uint> OutSamples : register(u0);   // 4096 uints, brick 0

// CPU `sampleColumnCellVoxel` (generated branches only; the engine's edited
// overlay short-circuit is runtime brush state, intentionally omitted for a
// from-scratch brick). Inputs are the exact CPU cell bounds + the center
// column world coords.
uint SampleColumnCell(
    int colWorldX, int colWorldZ,
    int minWorldY, int maxWorldY, int preferredWorldY,
    uint seed)
{
    float height = TH_HeightAt(colWorldX, colWorldZ, seed);
    float relief = TH_SurfaceRelief(colWorldX, colWorldZ, height, 4, seed);

    // (a) bedrock band
    if (maxWorldY <= TH_TERRAIN_MIN_Y + 2) {
        int sampleY = clamp(preferredWorldY, minWorldY, maxWorldY);
        return TH_SampleVoxel(colWorldX, sampleY, colWorldZ, height, relief, seed);
    }

    int terrainTopY = (int)floor(height);
    bool submergedColumn = height < (float)TH_SEA_LEVEL_Y;

    // (b) water overlap
    bool overlapsWater =
        submergedColumn &&
        minWorldY <= TH_SEA_LEVEL_Y &&
        maxWorldY > terrainTopY;
    if (overlapsWater) {
        int waterMinY = max(minWorldY, terrainTopY + 1);
        int waterMaxY = min(maxWorldY, TH_SEA_LEVEL_Y);
        int sampleY = clamp(preferredWorldY, waterMinY, waterMaxY);
        return TH_SampleVoxel(colWorldX, sampleY, colWorldZ, height, relief, seed);
    }

    // (c) solid
    if ((float)minWorldY <= height) {
        int solidMaxY = min(maxWorldY, terrainTopY);
        bool cellContainsTerrainTop = maxWorldY >= terrainTopY;
        int representativeY = cellContainsTerrainTop ? terrainTopY : preferredWorldY;
        int sampleY = clamp(representativeY, minWorldY, solidMaxY);
        return TH_SampleVoxel(colWorldX, sampleY, colWorldZ, height, relief, seed);
    }

    // (d) air
    return TH_PackVoxel(TH_MAT_AIR, 0u, 0u, 0u);
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const int originX = originAndCell.x;
    const int originY = originAndCell.y;
    const int originZ = originAndCell.z;
    const int cellSize = max(1, originAndCell.w);
    const uint seed = misc.x;

    // Each thread strides over the 4096 voxels of brick 0.
    // Voxel index layout (LocalVoxelIndex): i = x + y*16 + z*256.
    for (uint i = dtid.x; i < 4096u; i += 64u) {
        int lx = (int)(i & 15u);
        int ly = (int)((i >> 4) & 15u);
        int lz = (int)((i >> 8) & 15u);

        // CPU cell mapping (integer cellSize => exact):
        //   center col  = origin + cell*l + cell/2-ish via Round((l+0.5)*cell)
        //   cell min     = origin + Floor(l*cell)
        //   cell max     = origin + Ceil((l+1)*cell) - 1
        int colWorldX = originX + (int)round(((float)lx + 0.5f) * (float)cellSize);
        int colWorldZ = originZ + (int)round(((float)lz + 0.5f) * (float)cellSize);

        int minWorldY = originY + (int)floor((float)ly * (float)cellSize);
        int maxWorldYRaw = originY + (int)ceil(((float)ly + 1.0f) * (float)cellSize) - 1;
        int maxWorldY = max(minWorldY, maxWorldYRaw);
        int preferredWorldY = originY + (int)round(((float)ly + 0.5f) * (float)cellSize);

        OutSamples[i] = SampleColumnCell(
            colWorldX, colWorldZ,
            minWorldY, maxWorldY, preferredWorldY,
            seed);
    }
}
