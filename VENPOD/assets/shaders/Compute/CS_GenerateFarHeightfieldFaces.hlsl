#include "../Common/SharedTypes.hlsli"
#include "../Common/FarTerrainShared.hlsli"

// GPU far-heightfield owner producer.
//
// Writes fixed-capacity SparseSurfaceFace rows for the existing sparse-surface
// raster path. Payload 0 means "inactive slot"; VS_SparseSurface clips those
// rows before they can write depth/stencil. Active rows must match the
// PS_Raymarch FarTerrainHeightVoxelized convention: distance-dependent snapped
// XZ cell, spawn-land floor reshape, and ceil quantized top height.

struct SparseSurfaceFace {
    int worldX;
    int worldY;
    int worldZ;
    uint payload;
};

cbuffer FarOwnerParams : register(b0) {
    // x=faceCount, y=cellCount, z=baseGridCellSize, w=worldSeed
    uint4 gridParams;
    // x=originX bits, y=originZ bits, z=farHandoffDistance bits, w=ownerMaxDistance bits
    uint4 originParams;
    // xyz=cameraPosition bits, w=reserved
    uint4 cameraParams;
};

RWStructuredBuffer<SparseSurfaceFace> OutFaces : register(u0);

static const float FAR_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FAR_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FAR_SEA_LEVEL_LOCAL = -48.0f;

float FarOwnerSmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

uint FarOwnerHash3D(int x, int y, int z, uint seed) {
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

float FarOwnerValueNoise2D(float x, float z, uint seed) {
    const int x0 = (int)floor(x);
    const int z0 = (int)floor(z);
    const float fx = x - (float)x0;
    const float fz = z - (float)z0;
    const float sx = FarOwnerSmooth01(fx);
    const float sz = FarOwnerSmooth01(fz);

    const float s00 = (float)(FarOwnerHash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s10 = (float)(FarOwnerHash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s01 = (float)(FarOwnerHash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s11 = (float)(FarOwnerHash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    const float a = lerp(s00, s10, sx);
    const float b = lerp(s01, s11, sx);
    return lerp(a, b, sz) * 2.0f - 1.0f;
}

struct FarOwnerTerrainSample {
    float height;
    float mountainMask;
    float spireMask;
    float ravineMask;
};

FarOwnerTerrainSample FarOwnerTerrainHeight(float2 xz, uint seed) {
    FTS_TerrainSample sharedSample = FTS_RawTerrainHeight(xz, seed);
    FarOwnerTerrainSample sample;
    sample.height = sharedSample.height;
    sample.mountainMask = sharedSample.mountainMask;
    sample.spireMask = sharedSample.spireMask;
    sample.ravineMask = sharedSample.ravineMask;
    return sample;
#if 0
    const float broad = FarOwnerValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, seed + 11u);
    const float ridgeSource = FarOwnerValueNoise2D(
        xz.x * 0.0100f + 41.0f,
        xz.y * 0.0100f - 17.0f,
        seed + 23u);
    const float ridge = 1.0f - abs(ridgeSource);
    const float detail = FarOwnerValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        seed + 37u);
    const float ridgeHeight = ridge * ridge;

    float height = -64.0f;
    height += broad * 92.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 3.0f;

    const float2 originDelta = xz - float2(192.0f, 224.0f);
    const float originDistance = length(originDelta);
    const float originComfort =
        1.0f - FarOwnerSmooth01(saturate((originDistance - 180.0f) / 520.0f));
    const float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - FarOwnerSmooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - FarOwnerSmooth01(originDistance / 420.0f)) * 58.0f;
    height = lerp(height, publicRegionHeight, originComfort * 0.94f);
    const float publicCapInfluence =
        1.0f - FarOwnerSmooth01(saturate((originDistance - 220.0f) / 420.0f));
    const float publicCap =
        58.0f + FarOwnerSmooth01(saturate(originDistance / 640.0f)) * 114.0f;
    height = lerp(height, min(height, publicCap), publicCapInfluence);

    const float submergedBlend =
        1.0f - FarOwnerSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 28.0f)) / 86.0f));
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            (FAR_SEA_LEVEL_LOCAL - 8.0f) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - FarOwnerSmooth01(originDistance / 520.0f)) * 18.0f;
        height = lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand =
        1.0f - FarOwnerSmooth01(saturate((originDistance - 260.0f) / 980.0f));
    const float lowlandUpper =
        1.0f - FarOwnerSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 96.0f)) / 120.0f));
    const float lowlandFloor =
        FarOwnerSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL - 40.0f)) / 64.0f));
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    const float playableShelfHeight =
        (FAR_SEA_LEVEL_LOCAL + 18.0f) +
        broad * 28.0f +
        ridgeHeight * 10.0f +
        detail * 1.5f +
        (1.0f - FarOwnerSmooth01(saturate(originDistance / 460.0f))) * 42.0f;
    height = lerp(height, playableShelfHeight, playableBankBlend);

    const float publicBasinBand =
        FarOwnerSmooth01(saturate((originDistance - 360.0f) / 240.0f)) *
        (1.0f - FarOwnerSmooth01(saturate((originDistance - 1700.0f) / 760.0f))) *
        FarOwnerSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL - 38.0f)) / 56.0f)) *
        (1.0f - FarOwnerSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 180.0f)) / 140.0f)));
    const float publicBasinFloor =
        (FAR_SEA_LEVEL_LOCAL - 12.0f) + broad * 2.0f + detail * 0.35f;
    height = lerp(height, min(height, publicBasinFloor), publicBasinBand * 0.80f);

    const float backdropNoise = FarOwnerValueNoise2D(
        xz.x * 0.0018f + 19.0f,
        xz.y * 0.0018f - 31.0f,
        seed + 211u);
    const float backdropRidgeSource = FarOwnerValueNoise2D(
        xz.x * 0.0032f - 71.0f,
        xz.y * 0.0032f + 43.0f,
        seed + 227u);
    const float backdropRidge = 1.0f - abs(backdropRidgeSource);
    const float backdropBreakup = FarOwnerValueNoise2D(
        xz.x * 0.0075f + 203.0f,
        xz.y * 0.0075f - 167.0f,
        seed + 271u);
    const float backdropNotch =
        FarOwnerSmooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    const float silhouetteRidge =
        saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    const float backdropBand =
        FarOwnerSmooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - FarOwnerSmooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    const float northBackdrop = FarOwnerSmooth01(saturate((xz.y - 1180.0f) / 900.0f));
    const float sideBackdrop = FarOwnerSmooth01(saturate((abs(xz.x - 192.0f) - 820.0f) / 980.0f));
    const float backdropFacing = saturate(northBackdrop + sideBackdrop * 0.58f);
    const float silhouetteContinuity = saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    const float backdropInfluence =
        backdropBand *
        backdropFacing *
        FarOwnerSmooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    const float backdropHeight =
        248.0f +
        backdropBand * 160.0f +
        silhouetteContinuity * 186.0f +
        backdropNoise * 26.0f;
    height = lerp(height, max(height, backdropHeight), backdropInfluence * 0.70f);

    const float westCorridor = FarOwnerSmooth01(saturate((192.0f - xz.x - 520.0f) / 820.0f));
    const float eastCorridor = FarOwnerSmooth01(saturate((xz.x - 192.0f - 520.0f) / 820.0f));
    const float southBlend = FarOwnerSmooth01(saturate((360.0f - xz.y) / 1200.0f));
    const float westNorthBlend = FarOwnerSmooth01(saturate((xz.y - 360.0f) / 920.0f));
    const float routeDistanceBand =
        FarOwnerSmooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - FarOwnerSmooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    const float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    const float routeRidgeNoiseA = FarOwnerValueNoise2D(
        xz.x * 0.0024f + 113.0f,
        xz.y * 0.0024f - 89.0f,
        seed + 251u);
    const float routeRidgeNoiseB = FarOwnerValueNoise2D(
        xz.x * 0.0068f - 37.0f,
        xz.y * 0.0068f + 151.0f,
        seed + 263u);
    const float routeBreakup = FarOwnerValueNoise2D(
        xz.x * 0.0110f - 211.0f,
        xz.y * 0.0110f + 73.0f,
        seed + 281u);
    const float routeNotch =
        FarOwnerSmooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    const float routeRidge =
        saturate(
            0.26f +
            (1.0f - abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f);
    const float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = lerp(
        height,
        max(height, routeBackdropHeight),
        routeCorridor * routeRidge * routeNotch * 0.68f);

    FarOwnerTerrainSample sample;
    sample.height = clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
    sample.mountainMask = saturate((ridgeHeight * 150.0f + max(height - 160.0f, 0.0f)) / 300.0f);
    sample.spireMask = 0.0f;
    sample.ravineMask = 0.0f;
    return sample;
#endif
}

float FarOwnerFallbackCellSize(float distanceFromCamera) {
    return FTS_FallbackCellSize(distanceFromCamera);
#if 0
    const float t = saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) return 8.0f;
    if (t < 0.42f) return 12.0f;
    if (t < 0.68f) return 18.0f;
    return 28.0f;
#endif
}

float FarOwnerQuantizeTopHeight(float height, float verticalStep) {
    return FTS_QuantizeTopHeight(height, verticalStep);
}

float FarOwnerSpawnLandBand(float2 xz) {
    return FTS_SpawnLandBand(xz, 1.0f);
}

float FarOwnerSpawnLandFloorBase(float2 xz, uint seed) {
    return FTS_SpawnLandFloorBase(xz, seed);
}

float FarOwnerReshapeHeight(float2 xz, float height, float mountainMask, uint seed) {
    return FTS_SpawnLandReshapeHeight(xz, height, mountainMask, seed, 1.0f);
}

float2 FarOwnerCellMinFromCenter(float2 xz, float cellSize) {
    return floor(xz / cellSize) * cellSize;
}

float2 FarOwnerCellCenterFromMin(float2 minXz, float cellSize) {
    return minXz + cellSize * 0.5f;
}

float FarOwnerVoxelizedHeightAtCellCenter(
    float2 cellCenter,
    float distanceFromCamera,
    uint seed,
    out FarOwnerTerrainSample rawSample)
{
    FTS_TerrainSample sharedSample;
    const float height = FTS_VoxelizedHeight(cellCenter, distanceFromCamera, seed, 1.0f, sharedSample);
    rawSample.height = sharedSample.height;
    rawSample.mountainMask = sharedSample.mountainMask;
    rawSample.spireMask = sharedSample.spireMask;
    rawSample.ravineMask = sharedSample.ravineMask;
    return height;
}

uint FarOwnerTerrainMaterial(float2 xz, float height, FarOwnerTerrainSample sample, uint seed) {
    return FTS_TerrainMaterial(xz, height, seed);
}

uint FarOwnerPackPayload(uint direction, uint voxel, uint width, uint height) {
    return ((direction & 0x7u) << 29u) |
        (((width - 1u) & 0x1Fu) << 24u) |
        (((height - 1u) & 0x1Fu) << 19u) |
        (voxel & 0x0007FFFFu);
}

void FarOwnerWriteInactive(uint index) {
    OutFaces[index].worldX = 0;
    OutFaces[index].worldY = 0;
    OutFaces[index].worldZ = 0;
    OutFaces[index].payload = 0u;
}

bool FarOwnerBuildCell(
    uint cellX,
    uint cellZ,
    uint cellCount,
    float baseCellSize,
    float originX,
    float originZ,
    float farHandoff,
    float ownerMaxDistance,
    float3 camera,
    uint seed,
    out float2 cellMin,
    out float2 cellCenter,
    out float topY,
    out uint material,
    out uint faceCellSize)
{
    if (cellX >= cellCount || cellZ >= cellCount) {
        cellMin = 0.0f.xx;
        cellCenter = 0.0f.xx;
        topY = 0.0f;
        material = MAT_AIR;
        faceCellSize = 1u;
        return false;
    }

    const float2 baseMin = float2(
        floor(originX + (float)cellX * baseCellSize),
        floor(originZ + (float)cellZ * baseCellSize));
    const float2 baseCenter = baseMin + baseCellSize * 0.5f;

    float distanceGuess = length(float3(baseCenter.x - camera.x, -camera.y, baseCenter.y - camera.z));
    float cellSize = FarOwnerFallbackCellSize(distanceGuess);
    cellMin = FarOwnerCellMinFromCenter(baseCenter, cellSize);
    cellCenter = FarOwnerCellCenterFromMin(cellMin, cellSize);

    FarOwnerTerrainSample sample;
    topY = FarOwnerVoxelizedHeightAtCellCenter(cellCenter, distanceGuess, seed, sample);
    float distanceWithHeight = length(float3(cellCenter.x - camera.x, topY - camera.y, cellCenter.y - camera.z));
    cellSize = FarOwnerFallbackCellSize(distanceWithHeight);
    cellMin = FarOwnerCellMinFromCenter(cellCenter, cellSize);
    cellCenter = FarOwnerCellCenterFromMin(cellMin, cellSize);
    topY = FarOwnerVoxelizedHeightAtCellCenter(cellCenter, distanceWithHeight, seed, sample);
    distanceWithHeight = length(float3(cellCenter.x - camera.x, topY - camera.y, cellCenter.y - camera.z));

    if (distanceWithHeight < farHandoff ||
        distanceWithHeight > ownerMaxDistance ||
        topY <= FAR_SEA_LEVEL_LOCAL) {
        material = MAT_AIR;
        faceCellSize = 1u;
        return false;
    }

    faceCellSize = (uint)round(cellSize);
    material = FarOwnerTerrainMaterial(cellCenter, topY, sample, seed);
    return true;
}

[numthreads(128, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint index = dtid.x;
    const uint faceCount = gridParams.x;
    if (index >= faceCount) {
        return;
    }

    const uint cellCount = max(1u, gridParams.y);
    const float baseCellSize = (float)max(1u, gridParams.z);
    const uint seed = gridParams.w;
    const float originX = asfloat(originParams.x);
    const float originZ = asfloat(originParams.y);
    const float farHandoff = asfloat(originParams.z);
    const float ownerMaxDistance = asfloat(originParams.w);
    const float3 camera = float3(asfloat(cameraParams.x), asfloat(cameraParams.y), asfloat(cameraParams.z));

    const uint cellSlots = max(1u, faceCount / max(1u, cellCount * cellCount));
    const uint slot = index % cellSlots;
    const uint cellIndex = index / cellSlots;
    if (cellIndex >= cellCount * cellCount || slot >= 2u) {
        FarOwnerWriteInactive(index);
        return;
    }

    const uint cellX = cellIndex % cellCount;
    const uint cellZ = cellIndex / cellCount;

    float2 cellMin;
    float2 cellCenter;
    float topY;
    uint material;
    uint faceCellSize;
    if (!FarOwnerBuildCell(
            cellX,
            cellZ,
            cellCount,
            baseCellSize,
            originX,
            originZ,
            farHandoff,
            ownerMaxDistance,
            camera,
            seed,
            cellMin,
            cellCenter,
            topY,
            material,
            faceCellSize)) {
        FarOwnerWriteInactive(index);
        return;
    }

    if (slot == 0u) {
        OutFaces[index].worldX = (int)floor(cellMin.x);
        OutFaces[index].worldY = (int)floor(topY) - 1;
        OutFaces[index].worldZ = (int)floor(cellMin.y);
        OutFaces[index].payload = FarOwnerPackPayload(3u, material, faceCellSize, faceCellSize);
        return;
    }

    const float dx = cellCenter.x - camera.x;
    const float dz = cellCenter.y - camera.z;
    uint direction = 4u;
    if (abs(dx) > abs(dz)) {
        direction = dx >= 0.0f ? 0u : 1u;
    } else {
        direction = dz >= 0.0f ? 4u : 5u;
    }

    const uint sideHeight = min(32u, max(1u, (uint)ceil(topY - FAR_SEA_LEVEL_LOCAL)));
    const int topFaceY = (int)floor(topY);
    OutFaces[index].worldY = topFaceY - (int)sideHeight;
    OutFaces[index].payload = FarOwnerPackPayload(direction, material, faceCellSize, sideHeight);

    if (direction == 0u) {
        OutFaces[index].worldX = (int)floor(cellMin.x);
        OutFaces[index].worldZ = (int)floor(cellMin.y);
    } else if (direction == 1u) {
        OutFaces[index].worldX = (int)floor(cellMin.x + (float)faceCellSize) - 1;
        OutFaces[index].worldZ = (int)floor(cellMin.y);
    } else if (direction == 4u) {
        OutFaces[index].worldX = (int)floor(cellMin.x);
        OutFaces[index].worldZ = (int)floor(cellMin.y);
    } else {
        OutFaces[index].worldX = (int)floor(cellMin.x);
        OutFaces[index].worldZ = (int)floor(cellMin.y + (float)faceCellSize) - 1;
    }
}
