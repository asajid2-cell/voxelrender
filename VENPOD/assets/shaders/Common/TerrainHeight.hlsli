#ifndef VENPOD_TERRAIN_HEIGHT_HLSLI
#define VENPOD_TERRAIN_HEIGHT_HLSLI

// =============================================================================
// VENPOD Terrain Height / Material — BYTE-FOR-BYTE HLSL port of the CPU
// procedural terrain generator. This is a PARITY port: exactness over elegance.
//
// GROUND TRUTH (C++):
//   src/Simulation/SparseTerrainGenerator.cpp
//     Smooth01, Lerp, Hash3D, ValueNoise2D, HeightAt,
//     SurfaceReliefAtWithCenter, SampleGeneratedVoxelWithColumn
//   src/Simulation/TerrainConstants.h   (SEA_LEVEL_Y / TERRAIN_MIN_Y / MAX_Y)
//   src/Utils/BitPacking.h              (PackVoxel + Material/StateFlags enums)
//   src/Simulation/SparseClipmap.cpp    (sampleColumnCellVoxel / mid-voxel rule)
//
// Default CPU seed (SparseTerrainGenerator.h): 12345u. m_seed is passed in here
// as an explicit `seed` parameter to every function.
//
// CROSS-REFERENCE (not ground truth): assets/shaders/Graphics/PS_Raymarch.hlsl
//   FarHash3D / FarValueNoise2D / FarSmooth01 / FarTerrainHeight. NOTE the
//   PS_Raymarch FarTerrainMaterial is a SIMPLIFIED 5-branch classifier — it is
//   NOT used here. We port the FULL C++ SampleGeneratedVoxelWithColumn.
//
// Every place the C++ and this HLSL could diverge is flagged with `// PARITY:`.
// =============================================================================

// ===== Terrain bounds (TerrainConstants.h) =====
#define TH_SEA_LEVEL_Y    (-48)
#define TH_TERRAIN_MIN_Y  (-332)
#define TH_TERRAIN_MAX_Y  (664)

// ===== Material IDs (Utils::Material, BitPacking.h) =====
#define TH_MAT_AIR      0u
#define TH_MAT_SAND     1u
#define TH_MAT_WATER    2u
#define TH_MAT_STONE    3u
#define TH_MAT_DIRT     4u
#define TH_MAT_BEDROCK  255u

// ===== State flags (Utils::StateFlags, BitPacking.h) =====
#define TH_STATE_ISSTATIC  0x80u   // Bit 7

// -----------------------------------------------------------------------------
// PackVoxel — exact port of Utils::PackVoxel(material, variant, velocity, state).
// Bit layout (BitPacking.h):
//   Bits 31-24: state    Bits 23-16: velocity    Bits 15-08: variant    Bits 07-00: material
// PARITY: C++ args are uint8_t and are masked to 8 bits implicitly by the cast
// chain. We mask explicitly with &0xFFu so out-of-range HLSL ints can't leak
// into a higher field. The CPU call sites only ever pass in-range values, so
// the masks are inert for parity but make the GPU side robust.
// PARITY: the C++ CALL `PackVoxel(Material, variant, 0, StateFlags::IsStatic)`
// — the literal `0` is the VELOCITY argument (3rd), NOT light/variant. Honor
// the 4-argument order exactly.
// -----------------------------------------------------------------------------
uint TH_PackVoxel(uint material, uint variant, uint velocity, uint state) {
    return (material & 0xFFu)
         | ((variant  & 0xFFu) << 8)
         | ((velocity & 0xFFu) << 16)
         | ((state    & 0xFFu) << 24);
}

uint TH_UnpackMaterial(uint voxel) { return voxel & 0xFFu; }
uint TH_UnpackVariant(uint voxel)  { return (voxel >> 8) & 0xFFu; }

// -----------------------------------------------------------------------------
// Smooth01 — C++: clamp(value,0,1); return v*v*(3-2v).
// PARITY: C++ uses std::clamp; for finite non-NaN inputs this equals saturate().
// We use clamp(...,0,1) to mirror the source literally. Inputs are always finite.
// -----------------------------------------------------------------------------
float TH_Smooth01(float value) {
    value = clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float TH_Lerp(float a, float b, float t) {
    // PARITY: C++ form is `a + (b - a) * t` — NOT HLSL lerp() (which is the same
    // algebraically but the intrinsic may FMA-fuse differently). Write it out.
    return a + (b - a) * t;
}

// -----------------------------------------------------------------------------
// Hash3D — exact port. C++ casts possibly-negative int32 via
// static_cast<uint32_t>(x): a 2's-complement bit reinterpret. HLSL `(uint)x` on
// an int is also a bit reinterpret, so the bit pattern matches.
// PARITY: uint32 multiply/xor/shift wrap identically on CPU and GPU.
// -----------------------------------------------------------------------------
uint TH_Hash3D(int x, int y, int z, uint seed) {
    uint h = seed ^ 2166136261u;
    h = (h ^ (uint)x) * 16777619u;
    h = (h ^ (uint)y) * 16777619u;
    h = (h ^ (uint)z) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// -----------------------------------------------------------------------------
// ValueNoise2D — exact port.
// PARITY: C++ x0 = static_cast<int32_t>(std::floor(x)). HLSL (int)floor(x)
// truncates toward zero, but floor() already produced an integral float, so the
// cast is exact for both. Matches FarValueNoise2D.
// PARITY: hash sample = (Hash3D(...) & 0xFFFFFFu) / float(0xFFFFFFu). C++ uses
// static_cast<float>(0xFFFFFFu) = 16777215.0. Use the same literal.
// -----------------------------------------------------------------------------
float TH_ValueNoise2D(float x, float z, uint seed) {
    int x0 = (int)floor(x);
    int z0 = (int)floor(z);
    float fx = x - (float)x0;
    float fz = z - (float)z0;
    float sx = TH_Smooth01(fx);
    float sz = TH_Smooth01(fz);

    float s00 = (float)(TH_Hash3D(x0,     0, z0,     seed) & 0xFFFFFFu) / 16777215.0f;
    float s10 = (float)(TH_Hash3D(x0 + 1, 0, z0,     seed) & 0xFFFFFFu) / 16777215.0f;
    float s01 = (float)(TH_Hash3D(x0,     0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    float s11 = (float)(TH_Hash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;

    float a = TH_Lerp(s00, s10, sx);
    float b = TH_Lerp(s01, s11, sx);
    return TH_Lerp(a, b, sz) * 2.0f - 1.0f;
}

// -----------------------------------------------------------------------------
// HeightAt — exact port of SparseTerrainGenerator::HeightAt.
// PARITY: C++ uses std::clamp(t,0,1) inside Smooth01 args. We use saturate(...)
// for those nested clamps to [0,1]; identical for finite inputs. std::abs ->
// abs, std::sqrt -> sqrt, std::min/max -> min/max, std::clamp(a,lo,hi) -> the
// 3-arg clamp(). The `if (blend > 0.0f)` guards in C++ are kept; when the blend
// is 0 the lerp is a no-op either way (mathematically identical to the
// PS_Raymarch unconditional lerp), but we keep the guards to mirror source.
// PARITY: seed offsets must be EXACT: +11u,+23u,+37u,+211u,+227u,+271u,+251u,
// +263u,+281u.
// PARITY: SEA_LEVEL_Y arithmetic is done in INT then cast to float in C++, e.g.
// static_cast<float>(SEA_LEVEL_Y + 28). We compute (float)(TH_SEA_LEVEL_Y + 28)
// so the integer add happens first — matches exactly.
// -----------------------------------------------------------------------------
float TH_HeightAt(int worldX, int worldZ, uint seed) {
    float x = (float)worldX;
    float z = (float)worldZ;

    float broad = TH_ValueNoise2D(x * 0.0045f, z * 0.0045f, seed + 11u);
    float ridgeSource = TH_ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, seed + 23u);
    float ridge = 1.0f - abs(ridgeSource);
    float detail = TH_ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, seed + 37u);

    float ridgeHeight = ridge * ridge;

    float height = -64.0f;
    height += broad * 145.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 8.0f;

    float originDx = x - 192.0f;
    float originDz = z - 224.0f;
    float originDistance = sqrt(originDx * originDx + originDz * originDz);
    float originComfort = 1.0f - TH_Smooth01(saturate((originDistance - 180.0f) / 520.0f));
    float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - TH_Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - TH_Smooth01(originDistance / 420.0f)) * 58.0f;
    height = TH_Lerp(height, publicRegionHeight, originComfort * 0.94f);
    float publicCapInfluence =
        1.0f - TH_Smooth01(saturate((originDistance - 220.0f) / 420.0f));
    float publicCap =
        58.0f +
        TH_Smooth01(saturate(originDistance / 640.0f)) * 114.0f;
    height = TH_Lerp(height, min(height, publicCap), publicCapInfluence);

    float submergedBlend =
        1.0f - TH_Smooth01(saturate((height - (float)(TH_SEA_LEVEL_Y + 28)) / 86.0f));
    if (submergedBlend > 0.0f) {
        float submergedShelfHeight =
            (float)(TH_SEA_LEVEL_Y - 8) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - TH_Smooth01(originDistance / 520.0f)) * 18.0f;
        height = TH_Lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    float playableBankBand =
        (1.0f - TH_Smooth01(saturate((originDistance - 260.0f) / 980.0f)));
    float lowlandUpper =
        1.0f - TH_Smooth01(saturate((height - (float)(TH_SEA_LEVEL_Y + 96)) / 120.0f));
    float lowlandFloor =
        TH_Smooth01(saturate((height - (float)(TH_SEA_LEVEL_Y - 40)) / 64.0f));
    float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    if (playableBankBlend > 0.0f) {
        float playableShelfHeight =
            (float)(TH_SEA_LEVEL_Y + 18) +
            broad * 28.0f +
            ridgeHeight * 10.0f +
            detail * 1.5f +
            (1.0f - TH_Smooth01(saturate(originDistance / 460.0f))) * 42.0f;
        height = TH_Lerp(height, playableShelfHeight, playableBankBlend);
    }
    float publicBasinBand =
        TH_Smooth01(saturate((originDistance - 360.0f) / 240.0f)) *
        (1.0f - TH_Smooth01(saturate((originDistance - 1700.0f) / 760.0f))) *
        TH_Smooth01(saturate((height - (float)(TH_SEA_LEVEL_Y - 38)) / 56.0f)) *
        (1.0f - TH_Smooth01(saturate((height - (float)(TH_SEA_LEVEL_Y + 180)) / 140.0f)));
    float publicBasinFloor =
        (float)(TH_SEA_LEVEL_Y - 12) +
        broad * 2.0f +
        detail * 0.35f;
    if (publicBasinBand > 0.0f) {
        height = TH_Lerp(height, min(height, publicBasinFloor), publicBasinBand * 0.80f);
    }
    float backdropNoise = TH_ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, seed + 211u);
    float backdropRidgeSource =
        TH_ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, seed + 227u);
    float backdropRidge = 1.0f - abs(backdropRidgeSource);
    float backdropBreakup =
        TH_ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, seed + 271u);
    float backdropNotch =
        TH_Smooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    float silhouetteRidge =
        saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    float backdropBand =
        TH_Smooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - TH_Smooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    float northBackdrop =
        TH_Smooth01(saturate((z - 1180.0f) / 900.0f));
    float sideBackdrop =
        TH_Smooth01(saturate((abs(x - 192.0f) - 820.0f) / 980.0f));
    float backdropFacing =
        saturate(northBackdrop + sideBackdrop * 0.58f);
    float silhouetteContinuity =
        saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    float backdropInfluence =
        backdropBand *
        backdropFacing *
        TH_Smooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    float backdropHeight =
        248.0f +
        backdropBand * 160.0f +
        silhouetteContinuity * 186.0f +
        backdropNoise * 26.0f;
    height = TH_Lerp(height, max(height, backdropHeight), backdropInfluence * 0.70f);

    float westCorridor = TH_Smooth01(saturate((192.0f - x - 520.0f) / 820.0f));
    float eastCorridor = TH_Smooth01(saturate((x - 192.0f - 520.0f) / 820.0f));
    float southBlend = TH_Smooth01(saturate((360.0f - z) / 1200.0f));
    float westNorthBlend = TH_Smooth01(saturate((z - 360.0f) / 920.0f));
    float routeDistanceBand =
        TH_Smooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - TH_Smooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    float routeRidgeNoiseA =
        TH_ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, seed + 251u);
    float routeRidgeNoiseB =
        TH_ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, seed + 263u);
    float routeBreakup =
        TH_ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, seed + 281u);
    float routeNotch =
        TH_Smooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    float routeRidge =
        saturate(
            0.26f +
            (1.0f - abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f);
    float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = TH_Lerp(height, max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    // PARITY: final clamp to [TERRAIN_MIN_Y, TERRAIN_MAX_Y] as floats.
    return clamp(height, (float)TH_TERRAIN_MIN_Y, (float)TH_TERRAIN_MAX_Y);
}

// -----------------------------------------------------------------------------
// SurfaceRelief — port of SurfaceReliefAtWithCenter.
// PARITY: C++ offset = max(1, sampleOffset). The neighbor world coords use
// TryAddInt32 saturating add; for the small offsets used here (4) no overflow is
// possible, so plain integer add matches. The 4 taps are +/-x and +/-z around
// the center height. Returns localMax - localMin over {center, 4 taps}.
// -----------------------------------------------------------------------------
float TH_SurfaceRelief(int worldX, int worldZ, float centerHeight, int sampleOffset, uint seed) {
    int offset = max(1, sampleOffset);
    int xMinus = worldX - offset;
    int xPlus  = worldX + offset;
    int zMinus = worldZ - offset;
    int zPlus  = worldZ + offset;

    float localMin = centerHeight;
    float localMax = centerHeight;

    float h0 = TH_HeightAt(xMinus, worldZ, seed);
    float h1 = TH_HeightAt(xPlus,  worldZ, seed);
    float h2 = TH_HeightAt(worldX, zMinus, seed);
    float h3 = TH_HeightAt(worldX, zPlus,  seed);

    localMin = min(localMin, h0); localMax = max(localMax, h0);
    localMin = min(localMin, h1); localMax = max(localMax, h1);
    localMin = min(localMin, h2); localMax = max(localMax, h2);
    localMin = min(localMin, h3); localMax = max(localMax, h3);

    return localMax - localMin;
}

// -----------------------------------------------------------------------------
// SampleVoxel — exact port of SampleGeneratedVoxelWithColumn. Returns the PACKED
// uint32 voxel. THIS is the full classifier (PS_Raymarch FarTerrainMaterial is a
// reduced 5-branch version and must NOT be used).
// PARITY: variant = Hash3D(worldX,worldY,worldZ, seed) & 0xFFu — uses the RAW
// seed (m_seed), NOT a +offset seed. depth = height - (float)worldY.
// PARITY: all comparisons mix `worldY` (int) with `SEA_LEVEL_Y + k` (int) and
// `height`/`depth`/`relief` (float). The int comparisons (e.g. worldY <=
// SEA_LEVEL_Y + 14) must stay INTEGER comparisons; the float thresholds
// (e.g. (float)(SEA_LEVEL_Y - 2)) must compute the int add first then cast.
// PARITY: the bedrock/solid branches pass velocity=0, state=IsStatic; the water
// branch passes velocity=0, state=0; air is all-zero.
// -----------------------------------------------------------------------------
uint TH_SampleVoxel(int worldX, int worldY, int worldZ, float height, float relief, uint seed) {
    if (worldY <= TH_TERRAIN_MIN_Y + 2) {
        uint variant = TH_Hash3D(worldX, worldY, worldZ, seed) & 0xFFu;
        return TH_PackVoxel(TH_MAT_BEDROCK, variant, 0u, TH_STATE_ISSTATIC);
    }

    if ((float)worldY <= height) {
        uint variant = TH_Hash3D(worldX, worldY, worldZ, seed) & 0xFFu;
        float depth = height - (float)worldY;
        bool steepSurface = relief > 10.0f || height > 160.0f;
        uint material = TH_MAT_STONE;
        bool nearWaterlineBank =
            height >= (float)(TH_SEA_LEVEL_Y - 2) &&
            height <  (float)(TH_SEA_LEVEL_Y + 72) &&
            worldY <= TH_SEA_LEVEL_Y + 14 &&
            depth  <  96.0f;
        bool lowlandExposedBank =
            height >= (float)(TH_SEA_LEVEL_Y + 18) &&
            height <  (float)(TH_SEA_LEVEL_Y + 128) &&
            worldY <= TH_SEA_LEVEL_Y + 96 &&
            depth  <  72.0f;
        bool submergedTerrainColumn =
            height < (float)TH_SEA_LEVEL_Y;
        bool dryOrIntertidalSurface =
            !submergedTerrainColumn &&
            height >= (float)(TH_SEA_LEVEL_Y - 2) &&
            height <  (float)(TH_SEA_LEVEL_Y + 6);
        bool lowlandShoreTop =
            height < (float)(TH_SEA_LEVEL_Y + 72) &&
            depth  < 4.0f;
        if (submergedTerrainColumn && depth < 6.0f) {
            material = TH_MAT_DIRT;
        } else if (dryOrIntertidalSurface && depth < 16.0f && relief < 14.0f) {
            material = TH_MAT_SAND;
        } else if (lowlandShoreTop) {
            material =
                (height < (float)(TH_SEA_LEVEL_Y + 48) && relief < 36.0f)
                    ? TH_MAT_SAND
                    : TH_MAT_DIRT;
        } else if (nearWaterlineBank) {
            material =
                (height < (float)(TH_SEA_LEVEL_Y + 72) && relief < 52.0f && depth < 96.0f)
                    ? TH_MAT_SAND
                    : TH_MAT_DIRT;
        } else if (lowlandExposedBank) {
            material =
                (height < (float)(TH_SEA_LEVEL_Y + 86) && relief < 58.0f && depth < 42.0f)
                    ? TH_MAT_SAND
                    : TH_MAT_DIRT;
        } else if (depth < 2.0f && !steepSurface) {
            material = TH_MAT_DIRT;
        } else if (depth < 5.0f && relief < 6.0f) {
            material = TH_MAT_DIRT;
        }
        return TH_PackVoxel(material, variant, 0u, TH_STATE_ISSTATIC);
    }

    if (worldY <= TH_SEA_LEVEL_Y && height < (float)TH_SEA_LEVEL_Y) {
        uint variant = TH_Hash3D(worldX, worldY, worldZ, seed) & 0xFFu;
        return TH_PackVoxel(TH_MAT_WATER, variant, 0u, 0u);
    }

    return TH_PackVoxel(TH_MAT_AIR, 0u, 0u, 0u);
}

// -----------------------------------------------------------------------------
// MidVoxelCellSample — replicates SparseClipmap.cpp `sampleColumnCellVoxel`
// (lines 5143-5205): for one coarse cell of a mid-voxel LOD brick, choose a
// representative world voxel Y inside the cell, then classify it via the full
// per-voxel sampler above. The cell spans [cellWorldY, cellWorldY + cellSize-1]
// in Y (and likewise in X/Z); `cellWorldX/Z` is the representative column the
// engine uses (the per-step `worldXByLocal[i]` / `worldZByLocal[i]`).
//
// preferredWorldY in the engine call is `worldY` (worldYByLocal[i], i.e. the
// cell's representative voxel center). We take cellWorldY as that representative
// center as well (caller passes the same value the CPU uses).
//
// This port covers the GENERATED branches only. The engine's edited-overlay
// short-circuit (tryEditedCellVoxel) is runtime brush state, not procedural
// terrain, and is intentionally omitted — a from-scratch GPU brick has no edits.
//
// PARITY: column.height = HeightAt(cellWorldX, cellWorldZ); column.relief =
// SurfaceReliefAtWithCenter(cellWorldX, cellWorldZ, height, 4). The CPU computes
// relief lazily but with the SAME inputs, so eager computation here matches.
// PARITY: terrainTopY = FloorToInt32Clamped(column.height) == (int)floor(height)
// for in-range heights (height is already clamped to [MIN_Y,MAX_Y], well inside
// int range). FloorToInt32Clamped only differs from (int)floor at INT_MIN/MAX,
// unreachable here.
// PARITY: representative-Y rules, in branch order:
//   (a) maxWorldY <= TERRAIN_MIN_Y+2  -> bedrock band: sampleY = clamp(pref, minY, maxY)
//   (b) submergedColumn && minY<=SEA && maxY>terrainTopY (overlapsWater):
//         waterMinY = max(minY, terrainTopY+1); waterMaxY = min(maxY, SEA_LEVEL_Y);
//         sampleY = clamp(pref, waterMinY, waterMaxY)
//   (c) minY <= height (solid): cellContainsTerrainTop = maxY >= terrainTopY;
//         representativeY = cellContainsTerrainTop ? terrainTopY : pref;
//         solidMaxY = min(maxY, terrainTopY); sampleY = clamp(repY, minY, solidMaxY)
//   (d) else -> AIR.
// PARITY: SaturatingAddInt32(terrainTopY, 1) == terrainTopY + 1 for in-range
// values. clamp(a, lo, hi) here is the integer 3-arg clamp; HLSL clamp on ints
// matches std::clamp for lo<=hi (which holds in every branch above).
// -----------------------------------------------------------------------------
uint TH_MidVoxelCellSample(int cellWorldX, int cellWorldY, int cellWorldZ, int cellSize, uint seed) {
    int cs = max(1, cellSize);
    int minWorldY = cellWorldY;
    int maxWorldY = cellWorldY + cs - 1;     // PARITY: cell covers [Y, Y+cs-1]
    int preferredWorldY = cellWorldY;        // representative voxel center (engine worldYByLocal[i])

    float height = TH_HeightAt(cellWorldX, cellWorldZ, seed);
    float relief = TH_SurfaceRelief(cellWorldX, cellWorldZ, height, 4, seed);

    // (a) bedrock band
    if (maxWorldY <= TH_TERRAIN_MIN_Y + 2) {
        int sampleY = clamp(preferredWorldY, minWorldY, maxWorldY);
        return TH_SampleVoxel(cellWorldX, sampleY, cellWorldZ, height, relief, seed);
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
        return TH_SampleVoxel(cellWorldX, sampleY, cellWorldZ, height, relief, seed);
    }

    // (c) solid
    if ((float)minWorldY <= height) {
        int solidMaxY = min(maxWorldY, terrainTopY);
        bool cellContainsTerrainTop = maxWorldY >= terrainTopY;
        int representativeY = cellContainsTerrainTop ? terrainTopY : preferredWorldY;
        int sampleY = clamp(representativeY, minWorldY, solidMaxY);
        // PARITY: engine path here is sampleColumnVoxel(column, sampleY) which is
        // exactly SampleGeneratedVoxelWithColumn with the column's height+relief.
        return TH_SampleVoxel(cellWorldX, sampleY, cellWorldZ, height, relief, seed);
    }

    // (d) air
    return TH_PackVoxel(TH_MAT_AIR, 0u, 0u, 0u);
}

// =============================================================================
// Post-process helpers — exact ports of the neighbor-dependent rules in
// SparseClipmap.cpp `GenerateVoxelBrickPayload` (the resident-brick path that
// runs AFTER the raw per-cell sampleColumnCellVoxel).
// =============================================================================

// ===== State flag for the LOD render surface marker (Utils::StateFlags) =====
#define TH_STATE_VISUALSURFACE  0x10u   // Bit 4

// -----------------------------------------------------------------------------
// TH_SampleColumnCellVoxelHR — exact port of the lambda `sampleColumnCellVoxel`
// (SparseClipmap.cpp ~5143-5205), GENERATED branches only. Identical to
// TH_MidVoxelCellSample but takes the column's height+relief + the explicit cell
// Y bounds + preferredWorldY EXACTLY as the engine call site supplies them, and
// the cell's world XZ comes from the chosen column (centerColumn or
// maxFootprintColumn). Edited-cell short-circuit is omitted (pristine path).
// PARITY: terrainTopY = FloorToInt32Clamped(height) == (int)floor(height) for
// in-range heights. SaturatingAddInt32(topY,1) == topY+1 in range. clamp() on
// ints matches std::clamp for lo<=hi (holds in every branch).
// -----------------------------------------------------------------------------
uint TH_SampleColumnCellVoxelHR(
    int colWorldX, int colWorldZ, float height, float relief,
    int minWorldY, int maxWorldY, int preferredWorldY, uint seed)
{
    if (maxWorldY <= TH_TERRAIN_MIN_Y + 2) {
        int sampleY = clamp(preferredWorldY, minWorldY, maxWorldY);
        return TH_SampleVoxel(colWorldX, sampleY, colWorldZ, height, relief, seed);
    }

    int terrainTopY = (int)floor(height);
    bool submergedColumn = height < (float)TH_SEA_LEVEL_Y;
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

    if ((float)minWorldY <= height) {
        int solidMaxY = min(maxWorldY, terrainTopY);
        bool cellContainsTerrainTop = maxWorldY >= terrainTopY;
        int representativeY = cellContainsTerrainTop ? terrainTopY : preferredWorldY;
        int sampleY = clamp(representativeY, minWorldY, solidMaxY);
        return TH_SampleVoxel(colWorldX, sampleY, colWorldZ, height, relief, seed);
    }

    return TH_PackVoxel(TH_MAT_AIR, 0u, 0u, 0u);
}

// -----------------------------------------------------------------------------
// TH_ClassifyColumnCellMaterial — exact port of the lambda
// `classifyColumnCellMaterial` (SparseClipmap.cpp ~5110-5142), pristine path
// (edited short-circuit omitted). Returns the Material id (not packed voxel).
// Only `height` of the column is used (relief unused here).
// -----------------------------------------------------------------------------
uint TH_ClassifyColumnCellMaterial(
    float columnHeight, int minWorldY, int maxWorldY)
{
    if (maxWorldY <= TH_TERRAIN_MIN_Y + 2) {
        return TH_MAT_BEDROCK;
    }
    int terrainTopY = (int)floor(columnHeight);
    if (columnHeight < (float)TH_SEA_LEVEL_Y &&
        minWorldY <= TH_SEA_LEVEL_Y &&
        maxWorldY > terrainTopY) {
        return TH_MAT_WATER;
    }
    if ((float)minWorldY <= columnHeight) {
        return TH_MAT_STONE;
    }
    return TH_MAT_AIR;
}

// -----------------------------------------------------------------------------
// TH_IsSurfaceNeighbor — exact port of the lambda `isSurfaceNeighbor`
// (SparseClipmap.cpp ~5206-5214).
// -----------------------------------------------------------------------------
bool TH_IsSurfaceNeighbor(uint material, uint neighborMaterial) {
    if (neighborMaterial == TH_MAT_AIR) {
        return true;
    }
    if (material == TH_MAT_WATER && neighborMaterial != TH_MAT_WATER) {
        return true;
    }
    return false;
}

#endif // VENPOD_TERRAIN_HEIGHT_HLSLI
