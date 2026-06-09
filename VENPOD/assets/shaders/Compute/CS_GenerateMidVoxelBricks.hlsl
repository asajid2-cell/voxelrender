// =============================================================================
// VENPOD Phase-1 GPU mid-voxel brick generation (FULL parity).
//
// Generates one 16^3 mid-voxel LOD brick (SPARSE_BRICK_SIZE = 16, 4096 voxels)
// whose per-voxel packed values are BYTE-IDENTICAL to the REAL resident brick
// produced by SparseClipmap.cpp `GenerateVoxelBrickPayload` on a PRISTINE
// (unedited) cache. That is: the raw per-cell `sampleColumnCellVoxel` PLUS the
// neighbor-dependent post-process:
//   (1) surface-band Stone deep-fill,
//   (2) footprint-touch occupancy refill (steep-slope hole fill),
//   (3) stone->dirt near-surface skin,
//   (4) VisualSurface flag from 6 neighbor cell materials,
// and the three brick-level early-outs (full-air above terrain, full-air when
// the lowest cell is above every column, full-stone deep underground).
//
// Edited-overlay branches (hasEditedCells / editCellKey / edit summaries) are
// intentionally omitted: the GPU path is pristine-only in v1, matching a fresh
// SparseClipmapTileCache with no SparseEditStore (hasEditedOverlays == false).
//
// GROUND TRUTH: SparseClipmap.cpp GenerateVoxelBrickPayload (~4842-5623).
//
// DESIGN (one thread group per brick, 256 threads):
//   Phase A: cooperatively fill the (haloSide x haloSide) = 18x18 groupshared
//            column grid {height, relief} from TH_HeightAt / TH_SurfaceRelief.
//            324 columns across 256 threads (each thread does 1-2). relief is
//            computed eagerly for all columns; it is a pure function of
//            (worldX,worldZ,height) so eager == the engine's lazy compute.
//            Per-thread brick aggregates (minColumnHeight/maxColumnHeight) are
//            reduced via a single-thread pass to keep float reduction order
//            irrelevant (min/max are order-independent anyway).
//   GroupMemoryBarrierWithGroupSync().
//   Phase B: thread t owns cell column (x = t & 15, z = t >> 4); it runs the
//            full y-loop of 16 voxels + the 4 post-process steps, reading the
//            groupshared halo columns + computing its own 25 footprint columns.
// =============================================================================

#include "../Common/TerrainHeight.hlsli"

// Batch request: one entry per brick to generate this dispatch. 32 bytes, matches
// the C++ BrickGenRequest in MidVoxelGpuGenerator.h. The CPU pump decides origin/
// cellSize/destSlot (residency policy); the GPU only generates the voxel samples.
struct BrickGenRequest {
    int  originX;
    int  originY;
    int  originZ;
    int  cellSize;
    uint destSlot;   // sample pool slot: writes OutSamples[destSlot*4096 + idx]
    uint pad0;
    uint pad1;
    uint pad2;
};

cbuffer GenParams : register(b0) {
    uint4 misc;            // x=seed, y=requestCount, z=pad, w=pad
};

StructuredBuffer<BrickGenRequest> Requests : register(t0);  // requestCount entries
RWStructuredBuffer<uint> OutSamples : register(u0);   // brick b -> [b*4096 .. +4095]

#define HALO_SIDE 18              // SPARSE_BRICK_SIZE + 2
#define HALO_COUNT (HALO_SIDE * HALO_SIDE)   // 324

// Groupshared halo column grid. Index = z * HALO_SIDE + x (matches columns[z][x]).
groupshared float g_height[HALO_COUNT];
groupshared float g_relief[HALO_COUNT];
// Brick-level aggregates over the halo grid (computed by thread 0).
groupshared float g_minColumnHeight;
groupshared float g_maxColumnHeight;

// --- CPU integer helpers (in-range => plain ops; see SparseClipmap.cpp) ---
// RoundToInt32Clamped / FloorToInt32Clamped / CeilToInt32Clamped reduce to
// round/floor/ceil for the finite, in-range doubles produced here.
// SaturatingAddInt32(a,b) == a+b in range.

// Per-local cell mapping (integer cellSize => exact), mirrors worldXByLocal etc.
// worldByLocal[i]    = origin + Round((i + 0.5) * cell)
// worldMinByLocal[i] = origin + Floor(i * cell)
// worldMaxByLocal[i] = max(worldMinByLocal[i], origin + Ceil((i+1) * cell) - 1)
int WorldByLocal(int origin, int i, int cell) {
    return origin + (int)round(((float)i + 0.5f) * (float)cell);
}
int WorldMinByLocal(int origin, int i, int cell) {
    return origin + (int)floor((float)i * (float)cell);
}
int WorldMaxByLocal(int origin, int i, int cell) {
    int mn = WorldMinByLocal(origin, i, cell);
    int mx = origin + (int)ceil(((float)(i + 1)) * (float)cell) - 1;
    return max(mn, mx);
}

// Halo coordinate mapping (mirrors worldXByHalo / worldXMinByHalo / worldXMaxByHalo).
// h in [0, HALO_SIDE-1]; interior h maps to local i = h-1.
int WorldByHalo(int origin, int h, int cell, int sampleStep) {
    if (h == 0)             return WorldByLocal(origin, 0, cell) - sampleStep;
    if (h == HALO_SIDE - 1) return WorldByLocal(origin, 15, cell) + sampleStep;
    return WorldByLocal(origin, h - 1, cell);
}
int WorldMinByHalo(int origin, int h, int cell, int sampleStep) {
    if (h == 0)             return WorldMinByLocal(origin, 0, cell) - sampleStep;
    if (h == HALO_SIDE - 1) return WorldMaxByLocal(origin, 15, cell) + 1;
    return WorldMinByLocal(origin, h - 1, cell);
}
int WorldMaxByHalo(int origin, int h, int cell, int sampleStep) {
    if (h == 0)             return WorldMinByLocal(origin, 0, cell) - 1;
    if (h == HALO_SIDE - 1) return WorldMaxByLocal(origin, 15, cell) + sampleStep;
    return WorldMaxByLocal(origin, h - 1, cell);
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID,
          uint3 gtid : SV_GroupThreadID,
          uint3 gid  : SV_GroupID) {
    // One thread group per brick; group index selects the request.
    const uint reqIndex = gid.x;
    if (reqIndex >= misc.y) {
        return;
    }
    const BrickGenRequest req = Requests[reqIndex];
    const int originX = req.originX;
    const int originY = req.originY;
    const int originZ = req.originZ;
    const int cellSize = max(1, req.cellSize);
    const uint seed = misc.x;
    // Base offset into the shared sample pool for this brick's destination slot.
    const uint outBase = req.destSlot * 4096u;
    const int sampleStep = max(1, cellSize);          // RoundToInt32Clamped(cellSize)
    const int surfaceBandDepth = max(sampleStep * 2, 2);
    const bool coarse = (float)cellSize > 1.5f;        // ring.cellSize > 1.5f

    const uint tid = gtid.x;   // 0..255

    // ---------------- Phase A: fill groupshared halo column grid ----------------
    // 324 columns, 256 threads -> thread does column tid and (tid+256) if < 324.
    [unroll(2)]
    for (uint pass = 0u; pass < 2u; ++pass) {
        uint c = tid + pass * 256u;
        if (c < (uint)HALO_COUNT) {
            int hx = (int)(c % (uint)HALO_SIDE);
            int hz = (int)(c / (uint)HALO_SIDE);
            int wx = WorldByHalo(originX, hx, cellSize, sampleStep);
            int wz = WorldByHalo(originZ, hz, cellSize, sampleStep);
            float h = TH_HeightAt(wx, wz, seed);
            g_height[c] = h;
            g_relief[c] = TH_SurfaceRelief(wx, wz, h, 4, seed);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Brick aggregates (thread 0). min/max are order-independent => exact.
    if (tid == 0u) {
        float mn = (float)TH_TERRAIN_MAX_Y;
        float mx = (float)TH_TERRAIN_MIN_Y;
        for (uint i = 0u; i < (uint)HALO_COUNT; ++i) {
            mn = min(mn, g_height[i]);
            mx = max(mx, g_height[i]);
        }
        g_minColumnHeight = mn;
        g_maxColumnHeight = mx;
    }

    GroupMemoryBarrierWithGroupSync();

    const float minColumnHeight = g_minColumnHeight;
    const float maxColumnHeight = g_maxColumnHeight;

    // ---- Brick-level early-outs (write to ALL voxels owned by this thread) ----
    // 256 threads / 4096 voxels => each thread owns 16 voxels (its (x,z) column).
    const int x = (int)(tid & 15u);
    const int z = (int)((tid >> 4) & 15u);

    const int minWorldYBrick = WorldByLocal(originY, 0, cellSize);   // worldYByLocal[0]
    const int maxWorldYBrick = WorldMaxByLocal(originY, 15, cellSize); // worldYMaxByLocal[15]

    // Early-out 1: brick fully above terrain & sea -> all AIR.
    bool fullAirAboveTerrain = (originY > TH_TERRAIN_MAX_Y && originY > TH_SEA_LEVEL_Y);
    // Early-out 2: lowest cell above every column (and above sea) -> all AIR.
    bool fullAirAboveColumns =
        (minWorldYBrick > TH_SEA_LEVEL_Y && (float)minWorldYBrick > maxColumnHeight);
    // Early-out 3: deep underground -> all STONE (IsStatic, no VisualSurface).
    bool fullStone =
        (maxWorldYBrick > TH_TERRAIN_MIN_Y + 2 &&
         (float)(maxWorldYBrick + surfaceBandDepth) < minColumnHeight);

    const uint airVoxel = TH_MAT_AIR;   // PackVoxel(Air,0,0,0) == 0
    const uint stoneStaticVoxel = TH_PackVoxel(TH_MAT_STONE, 0u, 0u, TH_STATE_ISSTATIC);

    if (fullAirAboveTerrain || fullAirAboveColumns) {
        for (int y = 0; y < 16; ++y) {
            OutSamples[outBase + x + y * 16 + z * 256] = airVoxel;
        }
        return;
    }
    if (fullStone) {
        for (int y = 0; y < 16; ++y) {
            OutSamples[outBase + x + y * 16 + z * 256] = stoneStaticVoxel;
        }
        return;
    }

    // ---------------- Phase B: per-cell column post-process ----------------
    // Halo indices for this (x,z): center=(z+1)(x+1) etc. Index = hz*18 + hx.
    #define HIDX(hz, hx) ((hz) * HALO_SIDE + (hx))
    const int ic = HIDX(z + 1, x + 1);   // center
    const int ipx = HIDX(z + 1, x + 2);  // +X
    const int inx = HIDX(z + 1, x);      // -X
    const int ipz = HIDX(z + 2, x + 1);  // +Z
    const int inz = HIDX(z, x + 1);      // -Z

    const float centerHeight = g_height[ic];
    const float centerRelief = g_relief[ic];
    const float posXHeight = g_height[ipx];
    const float negXHeight = g_height[inx];
    const float posZHeight = g_height[ipz];
    const float negZHeight = g_height[inz];

    // Cell world XZ bounds for this (x,z).
    const int cxMin = WorldMinByLocal(originX, x, cellSize);
    const int cxMax = WorldMaxByLocal(originX, x, cellSize);
    const int czMin = WorldMinByLocal(originZ, z, cellSize);
    const int czMax = WorldMaxByLocal(originZ, z, cellSize);
    const int centerWX = WorldByLocal(originX, x, cellSize);   // worldXByLocal[x]
    const int centerWZ = WorldByLocal(originZ, z, cellSize);

    // Neighbor halo cell XZ bounds (for classify calls).
    const int hxMinPosX = WorldMinByHalo(originX, x + 2, cellSize, sampleStep);
    const int hxMaxPosX = WorldMaxByHalo(originX, x + 2, cellSize, sampleStep);
    const int hxMinNegX = WorldMinByHalo(originX, x, cellSize, sampleStep);
    const int hxMaxNegX = WorldMaxByHalo(originX, x, cellSize, sampleStep);
    const int hzMinPosZ = WorldMinByHalo(originZ, z + 2, cellSize, sampleStep);
    const int hzMaxPosZ = WorldMaxByHalo(originZ, z + 2, cellSize, sampleStep);
    const int hzMinNegZ = WorldMinByHalo(originZ, z, cellSize, sampleStep);
    const int hzMaxNegZ = WorldMaxByHalo(originZ, z, cellSize, sampleStep);

    // --- minHorizontalColumnHeight over {center,+X,-X,+Z,-Z} ---
    float minHorizontalColumnHeight =
        min(min(min(centerHeight, posXHeight), negXHeight),
            min(posZHeight, negZHeight));

    // --- maxFootprintColumn over center + 8 corner halo cols + 25 footprint cols ---
    // Start with centerColumn, then 8 corners (columns[z][x], [z][x+2], [z+2][x],
    // [z+2][x+2]) and +X/-X/+Z/-Z, then 25 cell-footprint columns (coarse only).
    // The CPU iteration ORDER matters only for ties (strict '>' keeps the first).
    // footprintColumns[] order in CPU: center,+X,-X,+Z,-Z, [z][x],[z][x+2],
    // [z+2][x],[z+2][x+2], then 25 footprint columns. Strict '>' keeps the first
    // on ties, so the iteration order is honored exactly.
    float maxFootprintColumnHeight = centerHeight;
    int   maxFpWX = centerWX;
    int   maxFpWZ = centerWZ;
    float maxFpRelief = centerRelief;

    // World XZ of the 9 halo columns used as footprint entries.
    // centerColumn = columns[z+1][x+1] -> world (worldXByHalo[x+1], worldZByHalo[z+1])
    //              = (worldXByLocal[x], worldZByLocal[z]) = (centerWX, centerWZ).
    int wxC  = centerWX;                                  int wzC  = centerWZ;
    int wxPX = WorldByHalo(originX, x + 2, cellSize, sampleStep); int wzPX = centerWZ;
    int wxNX = WorldByHalo(originX, x,     cellSize, sampleStep); int wzNX = centerWZ;
    int wxPZ = centerWX;                                  int wzPZ = WorldByHalo(originZ, z + 2, cellSize, sampleStep);
    int wxNZ = centerWX;                                  int wzNZ = WorldByHalo(originZ, z,     cellSize, sampleStep);
    // corners
    int wxC00 = wxNX; int wzC00 = wzNZ;   // columns[z][x]
    int wxC02 = wxPX; int wzC02 = wzNZ;   // columns[z][x+2]
    int wxC20 = wxNX; int wzC20 = wzPZ;   // columns[z+2][x]
    int wxC22 = wxPX; int wzC22 = wzPZ;   // columns[z+2][x+2]

    #define FP(H, WX, WZ, R) \
        if ((H) > maxFootprintColumnHeight) { \
            maxFootprintColumnHeight = (H); maxFpWX = (WX); maxFpWZ = (WZ); maxFpRelief = (R); }

    FP(posXHeight,            wxPX, wzPX, g_relief[ipx]);
    FP(negXHeight,            wxNX, wzNX, g_relief[inx]);
    FP(posZHeight,            wxPZ, wzPZ, g_relief[ipz]);
    FP(negZHeight,            wxNZ, wzNZ, g_relief[inz]);
    FP(g_height[HIDX(z, x)],       wxC00, wzC00, g_relief[HIDX(z, x)]);
    FP(g_height[HIDX(z, x + 2)],   wxC02, wzC02, g_relief[HIDX(z, x + 2)]);
    FP(g_height[HIDX(z + 2, x)],   wxC20, wzC20, g_relief[HIDX(z + 2, x)]);
    FP(g_height[HIDX(z + 2, x + 2)], wxC22, wzC22, g_relief[HIDX(z + 2, x + 2)]);

    // 25 cell-footprint columns (coarse only): 5x5 grid over the cell's XZ span.
    if (coarse) {
        for (uint sampleZIndex = 0u; sampleZIndex < 5u; ++sampleZIndex) {
            int sampleZ = czMin +
                (int)(((int)(czMax - czMin) * (int)sampleZIndex + 2) / 4);
            for (uint sampleXIndex = 0u; sampleXIndex < 5u; ++sampleXIndex) {
                int sampleX = cxMin +
                    (int)(((int)(cxMax - cxMin) * (int)sampleXIndex + 2) / 4);
                float fh = TH_HeightAt(sampleX, sampleZ, seed);
                if (fh > maxFootprintColumnHeight) {
                    maxFootprintColumnHeight = fh;
                    maxFpWX = sampleX; maxFpWZ = sampleZ;
                    // relief computed lazily on CPU; eager here is identical.
                    maxFpRelief = TH_SurfaceRelief(sampleX, sampleZ, fh, 4, seed);
                }
            }
        }
    }
    #undef FP

    const int terrainTopYCenter = (int)floor(centerHeight);

    // ----------------------------- y loop -----------------------------
    for (int y = 0; y < 16; ++y) {
        int worldY        = WorldByLocal(originY, y, cellSize);     // worldYByLocal[y]
        int cellMinWorldY = WorldMinByLocal(originY, y, cellSize);
        int cellMaxWorldY = WorldMaxByLocal(originY, y, cellSize);

        uint outVoxel;

        // (1) surface-band Stone deep-fill.
        if (cellMaxWorldY > TH_TERRAIN_MIN_Y + 2 &&
            (float)(cellMaxWorldY + surfaceBandDepth) < minHorizontalColumnHeight) {
            OutSamples[outBase + x + y * 16 + z * 256] = stoneStaticVoxel;
            continue;
        }

        // Raw center sample.
        uint voxel = TH_SampleColumnCellVoxelHR(
            centerWX, centerWZ, centerHeight, centerRelief,
            cellMinWorldY, cellMaxWorldY, worldY, seed);
        uint material = TH_UnpackMaterial(voxel);

        // (2) footprint-touch occupancy refill (steep-slope hole fill).
        if (material == TH_MAT_AIR && coarse) {
            bool footprintTouchesTerrain =
                (float)cellMinWorldY <= maxFootprintColumnHeight;
            if (footprintTouchesTerrain) {
                int prefY = min(worldY, (int)floor(maxFootprintColumnHeight));
                voxel = TH_SampleColumnCellVoxelHR(
                    maxFpWX, maxFpWZ, maxFootprintColumnHeight, maxFpRelief,
                    cellMinWorldY, cellMaxWorldY, prefY, seed);
                material = TH_UnpackMaterial(voxel);
            }
        }

        // (3) stone -> dirt near-surface skin.
        if (coarse && material == TH_MAT_STONE) {
            int terrainTopY = terrainTopYCenter;
            bool nearGeneratedSurfaceSkin =
                cellMaxWorldY >= (terrainTopY - surfaceBandDepth) &&
                cellMinWorldY <= (terrainTopY + max(1, sampleStep));
            if (nearGeneratedSurfaceSkin && centerHeight < 160.0f) {
                if (centerRelief < 10.0f) {
                    uint variant = TH_UnpackVariant(voxel);
                    voxel = TH_PackVoxel(TH_MAT_DIRT, variant, 0u, TH_STATE_ISSTATIC);
                    material = TH_MAT_DIRT;
                }
            }
        }

        // (4) VisualSurface flag from neighbor cell materials.
        if (material != TH_MAT_AIR) {
            int terrainTopY = terrainTopYCenter;
            int proceduralSurfaceY =
                (material == TH_MAT_WATER) ? TH_SEA_LEVEL_Y : terrainTopY;
            bool nearProceduralSurface =
                cellMaxWorldY >= (proceduralSurfaceY - surfaceBandDepth) &&
                cellMinWorldY <= proceduralSurfaceY;
            bool coarseSlopeEnvelopeSurface =
                coarse &&
                (maxFootprintColumnHeight - minHorizontalColumnHeight) >=
                    (float)(max(2, sampleStep)) * 0.75f &&
                (float)cellMinWorldY <= maxFootprintColumnHeight &&
                (float)cellMaxWorldY >= minHorizontalColumnHeight;

            // 6 neighbor classify calls (exact CPU arg order).
            uint nm0 = TH_ClassifyColumnCellMaterial(posXHeight, cellMinWorldY, cellMaxWorldY);
            uint nm1 = TH_ClassifyColumnCellMaterial(negXHeight, cellMinWorldY, cellMaxWorldY);
            uint nm2 = TH_ClassifyColumnCellMaterial(
                centerHeight, cellMinWorldY + sampleStep, cellMaxWorldY + sampleStep);
            uint nm3 = TH_ClassifyColumnCellMaterial(
                centerHeight, cellMinWorldY - sampleStep, cellMaxWorldY - sampleStep);
            uint nm4 = TH_ClassifyColumnCellMaterial(posZHeight, cellMinWorldY, cellMaxWorldY);
            uint nm5 = TH_ClassifyColumnCellMaterial(negZHeight, cellMinWorldY, cellMaxWorldY);

            bool visualSurface = nearProceduralSurface || coarseSlopeEnvelopeSurface;
            visualSurface = visualSurface ||
                TH_IsSurfaceNeighbor(material, nm0) ||
                TH_IsSurfaceNeighbor(material, nm1) ||
                TH_IsSurfaceNeighbor(material, nm2) ||
                TH_IsSurfaceNeighbor(material, nm3) ||
                TH_IsSurfaceNeighbor(material, nm4) ||
                TH_IsSurfaceNeighbor(material, nm5);

            if (visualSurface) {
                voxel |= TH_STATE_VISUALSURFACE << 24u;
            }
            outVoxel = voxel;
        } else {
            outVoxel = airVoxel;
        }

        OutSamples[outBase + x + y * 16 + z * 256] = outVoxel;
    }
}
