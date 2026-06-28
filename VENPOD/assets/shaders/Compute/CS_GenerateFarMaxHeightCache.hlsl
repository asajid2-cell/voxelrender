#include "../Common/SharedTypes.hlsli"
#include "../Common/FarTerrainShared.hlsli"

// GPU far-terrain conservative max-height cache.
//
// Mode level 0 writes leaf max heights by sampling the runtime voxelized
// far-height convention over each cache cell. Higher levels reduce the previous
// level with a 2x2 max. Consumers must treat out-of-cache as "possible hit" and
// fall back to RaymarchFarTerrain.

cbuffer FarMaxHeightCacheParams : register(b0) {
    // x=level, y=srcOffset, z=dstOffset, w=worldSeed
    uint4 generationParams;
    // x=srcWidth, y=srcHeight, z=dstWidth, w=dstHeight
    uint4 dimensionParams;
    // x=originX bits, y=originZ bits, z=leafCellSize bits, w=heightPad bits
    uint4 originParams;
    // xyz=cameraPosition bits, w=reserved
    uint4 cameraParams;
};

RWStructuredBuffer<float> HeightCache : register(u0);

static const float FAR_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FAR_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FAR_SEA_LEVEL_LOCAL = -48.0f;

float FarCacheSmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

uint FarCacheHash3D(int x, int y, int z, uint seed) {
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

float FarCacheValueNoise2D(float x, float z, uint seed) {
    const int x0 = (int)floor(x);
    const int z0 = (int)floor(z);
    const float fx = x - (float)x0;
    const float fz = z - (float)z0;
    const float sx = FarCacheSmooth01(fx);
    const float sz = FarCacheSmooth01(fz);

    const float s00 = (float)(FarCacheHash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s10 = (float)(FarCacheHash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s01 = (float)(FarCacheHash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s11 = (float)(FarCacheHash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    return lerp(lerp(s00, s10, sx), lerp(s01, s11, sx), sz) * 2.0f - 1.0f;
}

struct FarCacheTerrainSample {
    float height;
    float mountainMask;
};

FarCacheTerrainSample FarCacheTerrainHeight(float2 xz, uint seed) {
    FTS_TerrainSample sharedSample = FTS_RawTerrainHeight(xz, seed);
    FarCacheTerrainSample sample;
    sample.height = sharedSample.height;
    sample.mountainMask = sharedSample.mountainMask;
    return sample;
#if 0
    const float broad = FarCacheValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, seed + 11u);
    const float ridgeSource = FarCacheValueNoise2D(
        xz.x * 0.0100f + 41.0f,
        xz.y * 0.0100f - 17.0f,
        seed + 23u);
    const float ridge = 1.0f - abs(ridgeSource);
    const float detail = FarCacheValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        seed + 37u);
    const float ridgeHeight = ridge * ridge;

    float height = -64.0f + broad * 92.0f + ridgeHeight * 150.0f + detail * 3.0f;
    const float2 originDelta = xz - float2(192.0f, 224.0f);
    const float originDistance = length(originDelta);
    const float originComfort =
        1.0f - FarCacheSmooth01(saturate((originDistance - 180.0f) / 520.0f));
    const float publicRegionHeight =
        -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f +
        (1.0f - FarCacheSmooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - FarCacheSmooth01(originDistance / 420.0f)) * 58.0f;
    height = lerp(height, publicRegionHeight, originComfort * 0.94f);
    const float publicCapInfluence =
        1.0f - FarCacheSmooth01(saturate((originDistance - 220.0f) / 420.0f));
    const float publicCap = 58.0f + FarCacheSmooth01(saturate(originDistance / 640.0f)) * 114.0f;
    height = lerp(height, min(height, publicCap), publicCapInfluence);

    const float submergedBlend =
        1.0f - FarCacheSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 28.0f)) / 86.0f));
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            (FAR_SEA_LEVEL_LOCAL - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f +
            (1.0f - FarCacheSmooth01(originDistance / 520.0f)) * 18.0f;
        height = lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand =
        1.0f - FarCacheSmooth01(saturate((originDistance - 260.0f) / 980.0f));
    const float lowlandUpper =
        1.0f - FarCacheSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 96.0f)) / 120.0f));
    const float lowlandFloor =
        FarCacheSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL - 40.0f)) / 64.0f));
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    const float playableShelfHeight =
        (FAR_SEA_LEVEL_LOCAL + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f +
        (1.0f - FarCacheSmooth01(saturate(originDistance / 460.0f))) * 42.0f;
    height = lerp(height, playableShelfHeight, playableBankBlend);

    const float publicBasinBand =
        FarCacheSmooth01(saturate((originDistance - 360.0f) / 240.0f)) *
        (1.0f - FarCacheSmooth01(saturate((originDistance - 1700.0f) / 760.0f))) *
        FarCacheSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL - 38.0f)) / 56.0f)) *
        (1.0f - FarCacheSmooth01(saturate((height - (FAR_SEA_LEVEL_LOCAL + 180.0f)) / 140.0f)));
    const float publicBasinFloor = (FAR_SEA_LEVEL_LOCAL - 12.0f) + broad * 2.0f + detail * 0.35f;
    height = lerp(height, min(height, publicBasinFloor), publicBasinBand * 0.80f);

    const float backdropNoise = FarCacheValueNoise2D(
        xz.x * 0.0018f + 19.0f,
        xz.y * 0.0018f - 31.0f,
        seed + 211u);
    const float backdropRidgeSource = FarCacheValueNoise2D(
        xz.x * 0.0032f - 71.0f,
        xz.y * 0.0032f + 43.0f,
        seed + 227u);
    const float backdropRidge = 1.0f - abs(backdropRidgeSource);
    const float backdropBreakup = FarCacheValueNoise2D(
        xz.x * 0.0075f + 203.0f,
        xz.y * 0.0075f - 167.0f,
        seed + 271u);
    const float backdropNotch =
        FarCacheSmooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    const float silhouetteRidge =
        saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    const float backdropBand =
        FarCacheSmooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - FarCacheSmooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    const float northBackdrop = FarCacheSmooth01(saturate((xz.y - 1180.0f) / 900.0f));
    const float sideBackdrop = FarCacheSmooth01(saturate((abs(xz.x - 192.0f) - 820.0f) / 980.0f));
    const float backdropFacing = saturate(northBackdrop + sideBackdrop * 0.58f);
    const float silhouetteContinuity = saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    const float backdropInfluence =
        backdropBand * backdropFacing * FarCacheSmooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    const float backdropHeight =
        248.0f + backdropBand * 160.0f + silhouetteContinuity * 186.0f + backdropNoise * 26.0f;
    height = lerp(height, max(height, backdropHeight), backdropInfluence * 0.70f);

    const float westCorridor = FarCacheSmooth01(saturate((192.0f - xz.x - 520.0f) / 820.0f));
    const float eastCorridor = FarCacheSmooth01(saturate((xz.x - 192.0f - 520.0f) / 820.0f));
    const float southBlend = FarCacheSmooth01(saturate((360.0f - xz.y) / 1200.0f));
    const float westNorthBlend = FarCacheSmooth01(saturate((xz.y - 360.0f) / 920.0f));
    const float routeDistanceBand =
        FarCacheSmooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - FarCacheSmooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    const float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    const float routeRidgeNoiseA = FarCacheValueNoise2D(
        xz.x * 0.0024f + 113.0f,
        xz.y * 0.0024f - 89.0f,
        seed + 251u);
    const float routeRidgeNoiseB = FarCacheValueNoise2D(
        xz.x * 0.0068f - 37.0f,
        xz.y * 0.0068f + 151.0f,
        seed + 263u);
    const float routeBreakup = FarCacheValueNoise2D(
        xz.x * 0.0110f - 211.0f,
        xz.y * 0.0110f + 73.0f,
        seed + 281u);
    const float routeNotch =
        FarCacheSmooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    const float routeRidge =
        saturate(0.26f + (1.0f - abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f);
    const float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
    height = lerp(height, max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    FarCacheTerrainSample sample;
    sample.height = clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
    sample.mountainMask = saturate((ridgeHeight * 150.0f + max(height - 160.0f, 0.0f)) / 300.0f);
    return sample;
#endif
}

float FarCacheFallbackCellSize(float distanceFromCamera) {
    return FTS_FallbackCellSize(distanceFromCamera);
#if 0
    const float t = saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) return 8.0f;
    if (t < 0.42f) return 12.0f;
    if (t < 0.68f) return 18.0f;
    return 28.0f;
#endif
}

float FarCacheQuantizeTopHeight(float height, float verticalStep) {
    return FTS_QuantizeTopHeight(height, verticalStep);
}

float FarCacheSpawnLandBand(float2 xz) {
    return FTS_SpawnLandBand(xz, 1.0f);
}

float FarCacheSpawnLandFloorBase(float2 xz, uint seed) {
    return FTS_SpawnLandFloorBase(xz, seed);
}

float FarCacheReshapeHeight(float2 xz, float height, float mountainMask, uint seed) {
    return FTS_SpawnLandReshapeHeight(xz, height, mountainMask, seed, 1.0f);
}

float FarCacheVoxelizedHeight(float2 xz, float distanceFromCamera, uint seed) {
    FTS_TerrainSample sample;
    return FTS_VoxelizedHeight(xz, distanceFromCamera, seed, 1.0f, sample);
}

float FarCacheLeafMaxHeight(uint cellX, uint cellZ, uint width, float originX, float originZ, float cellSize, float heightPad, float3 camera, uint seed) {
    const float2 cellMin = float2(originX, originZ) + float2((float)cellX, (float)cellZ) * cellSize;
    float maxHeight = FAR_TERRAIN_MIN_HEIGHT;
    [unroll]
    for (uint sy = 0u; sy < 5u; ++sy) {
        [unroll]
        for (uint sx = 0u; sx < 5u; ++sx) {
            const float2 uv = (float2((float)sx, (float)sy) + float2(0.5f, 0.5f)) / 5.0f;
            const float2 sampleXz = cellMin + uv * cellSize;
            const float distanceGuess = length(sampleXz - camera.xz);
            maxHeight = max(maxHeight, FarCacheVoxelizedHeight(sampleXz, distanceGuess, seed));
        }
    }
    return min(FAR_TERRAIN_MAX_HEIGHT + heightPad, maxHeight + heightPad);
}

[numthreads(128, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint index = dtid.x;
    const uint level = generationParams.x;
    const uint srcOffset = generationParams.y;
    const uint dstOffset = generationParams.z;
    const uint seed = generationParams.w;
    const uint srcWidth = max(1u, dimensionParams.x);
    const uint srcHeight = max(1u, dimensionParams.y);
    const uint dstWidth = max(1u, dimensionParams.z);
    const uint dstHeight = max(1u, dimensionParams.w);
    const uint dstCount = dstWidth * dstHeight;
    if (index >= dstCount) {
        return;
    }

    const uint cellX = index % dstWidth;
    const uint cellZ = index / dstWidth;
    const uint outIndex = dstOffset + index;
    if (level == 0u) {
        HeightCache[outIndex] = FarCacheLeafMaxHeight(
            cellX,
            cellZ,
            dstWidth,
            asfloat(originParams.x),
            asfloat(originParams.y),
            asfloat(originParams.z),
            asfloat(originParams.w),
            float3(asfloat(cameraParams.x), asfloat(cameraParams.y), asfloat(cameraParams.z)),
            seed);
        return;
    }

    float maxHeight = FAR_TERRAIN_MIN_HEIGHT;
    [unroll]
    for (uint dz = 0u; dz < 2u; ++dz) {
        [unroll]
        for (uint dx = 0u; dx < 2u; ++dx) {
            const uint childX = cellX * 2u + dx;
            const uint childZ = cellZ * 2u + dz;
            if (childX < srcWidth && childZ < srcHeight) {
                maxHeight = max(maxHeight, HeightCache[srcOffset + childZ * srcWidth + childX]);
            }
        }
    }
    HeightCache[outIndex] = maxHeight;
}
