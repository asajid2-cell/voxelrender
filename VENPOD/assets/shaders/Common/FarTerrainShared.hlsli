#ifndef VENPOD_FAR_TERRAIN_SHARED_HLSLI
#define VENPOD_FAR_TERRAIN_SHARED_HLSLI

// Shared far-terrain convention for GPU cached/owner products.
// This mirrors PS_Raymarch's FarTerrainHeightVoxelized path: raw far height,
// spawn-land reshape at the consumer site, distance-dependent XZ snapping, and
// ceil top-height quantization. Conservative consumers should still add their
// own max-height padding or fallback-to-raymarch uncertainty handling.

static const float FTS_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FTS_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FTS_SEA_LEVEL = -48.0f;

struct FTS_TerrainSample {
    float height;
    float mountainMask;
    float spireMask;
    float ravineMask;
};

float FTS_Smooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

uint FTS_Hash3D(int x, int y, int z, uint seed) {
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

float FTS_ValueNoise2D(float x, float z, uint seed) {
    const int x0 = (int)floor(x);
    const int z0 = (int)floor(z);
    const float fx = x - (float)x0;
    const float fz = z - (float)z0;
    const float sx = FTS_Smooth01(fx);
    const float sz = FTS_Smooth01(fz);

    const float s00 = (float)(FTS_Hash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s10 = (float)(FTS_Hash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s01 = (float)(FTS_Hash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s11 = (float)(FTS_Hash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    return lerp(lerp(s00, s10, sx), lerp(s01, s11, sx), sz) * 2.0f - 1.0f;
}

FTS_TerrainSample FTS_RawTerrainHeight(float2 xz, uint seed) {
    const float broad = FTS_ValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, seed + 11u);
    const float ridgeSource = FTS_ValueNoise2D(
        xz.x * 0.0100f + 41.0f,
        xz.y * 0.0100f - 17.0f,
        seed + 23u);
    const float ridge = 1.0f - abs(ridgeSource);
    const float detail = FTS_ValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        seed + 37u);
    const float ridgeHeight = ridge * ridge;

    float height = -64.0f + broad * 92.0f + ridgeHeight * 150.0f + detail * 3.0f;
    const float2 originDelta = xz - float2(192.0f, 224.0f);
    const float originDistance = length(originDelta);

    const float originComfort =
        1.0f - FTS_Smooth01(saturate((originDistance - 180.0f) / 520.0f));
    const float publicRegionHeight =
        -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f +
        (1.0f - FTS_Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - FTS_Smooth01(originDistance / 420.0f)) * 58.0f;
    height = lerp(height, publicRegionHeight, originComfort * 0.94f);

    const float publicCapInfluence =
        1.0f - FTS_Smooth01(saturate((originDistance - 220.0f) / 420.0f));
    const float publicCap = 58.0f + FTS_Smooth01(saturate(originDistance / 640.0f)) * 114.0f;
    height = lerp(height, min(height, publicCap), publicCapInfluence);

    const float submergedBlend =
        1.0f - FTS_Smooth01(saturate((height - (FTS_SEA_LEVEL + 28.0f)) / 86.0f));
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            (FTS_SEA_LEVEL - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f +
            (1.0f - FTS_Smooth01(originDistance / 520.0f)) * 18.0f;
        height = lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand =
        1.0f - FTS_Smooth01(saturate((originDistance - 260.0f) / 980.0f));
    const float lowlandUpper =
        1.0f - FTS_Smooth01(saturate((height - (FTS_SEA_LEVEL + 96.0f)) / 120.0f));
    const float lowlandFloor =
        FTS_Smooth01(saturate((height - (FTS_SEA_LEVEL - 40.0f)) / 64.0f));
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    const float playableShelfHeight =
        (FTS_SEA_LEVEL + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f +
        (1.0f - FTS_Smooth01(saturate(originDistance / 460.0f))) * 42.0f;
    height = lerp(height, playableShelfHeight, playableBankBlend);

    const float publicBasinBand =
        FTS_Smooth01(saturate((originDistance - 360.0f) / 240.0f)) *
        (1.0f - FTS_Smooth01(saturate((originDistance - 1700.0f) / 760.0f))) *
        FTS_Smooth01(saturate((height - (FTS_SEA_LEVEL - 38.0f)) / 56.0f)) *
        (1.0f - FTS_Smooth01(saturate((height - (FTS_SEA_LEVEL + 180.0f)) / 140.0f)));
    const float publicBasinFloor = (FTS_SEA_LEVEL - 12.0f) + broad * 2.0f + detail * 0.35f;
    height = lerp(height, min(height, publicBasinFloor), publicBasinBand * 0.80f);

    const float backdropNoise = FTS_ValueNoise2D(
        xz.x * 0.0018f + 19.0f,
        xz.y * 0.0018f - 31.0f,
        seed + 211u);
    const float backdropRidgeSource = FTS_ValueNoise2D(
        xz.x * 0.0032f - 71.0f,
        xz.y * 0.0032f + 43.0f,
        seed + 227u);
    const float backdropRidge = 1.0f - abs(backdropRidgeSource);
    const float backdropBreakup = FTS_ValueNoise2D(
        xz.x * 0.0075f + 203.0f,
        xz.y * 0.0075f - 167.0f,
        seed + 271u);
    const float backdropNotch = FTS_Smooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    const float silhouetteRidge =
        saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    const float backdropBand =
        FTS_Smooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - FTS_Smooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    const float northBackdrop = FTS_Smooth01(saturate((xz.y - 1180.0f) / 900.0f));
    const float sideBackdrop = FTS_Smooth01(saturate((abs(xz.x - 192.0f) - 820.0f) / 980.0f));
    const float backdropFacing = saturate(northBackdrop + sideBackdrop * 0.58f);
    const float silhouetteContinuity = saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    const float backdropInfluence =
        backdropBand * backdropFacing * FTS_Smooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    const float backdropHeight =
        248.0f + backdropBand * 160.0f + silhouetteContinuity * 186.0f + backdropNoise * 26.0f;
    height = lerp(height, max(height, backdropHeight), backdropInfluence * 0.70f);

    const float westCorridor = FTS_Smooth01(saturate((192.0f - xz.x - 520.0f) / 820.0f));
    const float eastCorridor = FTS_Smooth01(saturate((xz.x - 192.0f - 520.0f) / 820.0f));
    const float southBlend = FTS_Smooth01(saturate((360.0f - xz.y) / 1200.0f));
    const float westNorthBlend = FTS_Smooth01(saturate((xz.y - 360.0f) / 920.0f));
    const float routeDistanceBand =
        FTS_Smooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - FTS_Smooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    const float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    const float routeRidgeNoiseA = FTS_ValueNoise2D(
        xz.x * 0.0024f + 113.0f,
        xz.y * 0.0024f - 89.0f,
        seed + 251u);
    const float routeRidgeNoiseB = FTS_ValueNoise2D(
        xz.x * 0.0068f - 37.0f,
        xz.y * 0.0068f + 151.0f,
        seed + 263u);
    const float routeBreakup = FTS_ValueNoise2D(
        xz.x * 0.0110f - 211.0f,
        xz.y * 0.0110f + 73.0f,
        seed + 281u);
    const float routeNotch = FTS_Smooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    const float routeRidge =
        saturate(0.26f + (1.0f - abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f);
    const float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
    height = lerp(height, max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    FTS_TerrainSample sample;
    sample.height = clamp(height, FTS_TERRAIN_MIN_HEIGHT, FTS_TERRAIN_MAX_HEIGHT);
    sample.mountainMask = saturate((ridgeHeight * 150.0f + max(height - 160.0f, 0.0f)) / 300.0f);
    sample.spireMask = 0.0f;
    sample.ravineMask = 0.0f;
    return sample;
}

float FTS_FallbackCellSize(float distanceFromCamera) {
    const float t = saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) return 8.0f;
    if (t < 0.42f) return 12.0f;
    if (t < 0.68f) return 18.0f;
    return 28.0f;
}

float FTS_QuantizeTopHeight(float height, float verticalStep) {
    verticalStep = max(verticalStep, 1.0f);
    return ceil(height / verticalStep) * verticalStep;
}

float FTS_SpawnLandBand(float2 xz, float spawnLandGate) {
    const float2 originDelta = xz - float2(192.0f, 224.0f);
    const float originDistance = length(originDelta);
    const float rawBand = 1.0f - FTS_Smooth01(saturate((originDistance - 200.0f) / 90000.0f));
    return rawBand * step(0.5f, spawnLandGate);
}

float FTS_SpawnLandFloorBase(float2 xz, uint seed) {
    const float broad = FTS_ValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, seed + 11u);
    const float detail = FTS_ValueNoise2D(xz.x * 0.035f - 13.0f, xz.y * 0.035f + 29.0f, seed + 37u);
    return (FTS_SEA_LEVEL + 56.0f) + broad * 18.0f + detail * 3.0f;
}

float FTS_SpawnLandReshapeHeight(float2 xz, float height, float mountainMask, uint seed, float spawnLandGate) {
    const float band = FTS_SpawnLandBand(xz, spawnLandGate);
    const float floorHeight = FTS_SpawnLandFloorBase(xz, seed) + mountainMask * 80.0f;
    return lerp(height, max(height, floorHeight), band);
}

float FTS_VoxelizedHeight(
    float2 xz,
    float distanceFromCamera,
    uint seed,
    float spawnLandGate,
    out FTS_TerrainSample sample)
{
    const float cellSize = FTS_FallbackCellSize(distanceFromCamera);
    const float2 sampleXz = (floor(xz / cellSize) + 0.5f) * cellSize;
    sample = FTS_RawTerrainHeight(sampleXz, seed);
    return FTS_QuantizeTopHeight(
        FTS_SpawnLandReshapeHeight(sampleXz, sample.height, sample.mountainMask, seed, spawnLandGate),
        max(4.0f, cellSize * 0.75f));
}

uint FTS_TerrainMaterial(float2 xz, float height, uint seed) {
    const float hx0 = FTS_RawTerrainHeight(xz - float2(4.0f, 0.0f), seed).height;
    const float hx1 = FTS_RawTerrainHeight(xz + float2(4.0f, 0.0f), seed).height;
    const float hz0 = FTS_RawTerrainHeight(xz - float2(0.0f, 4.0f), seed).height;
    const float hz1 = FTS_RawTerrainHeight(xz + float2(0.0f, 4.0f), seed).height;
    const float localRelief =
        max(max(abs(hx0 - height), abs(hx1 - height)), max(abs(hz0 - height), abs(hz1 - height)));

    if (height < FTS_SEA_LEVEL) return MAT_WATER;
    if (height < FTS_SEA_LEVEL + 48.0f && localRelief < 36.0f) return MAT_SAND;
    if (height < FTS_SEA_LEVEL + 72.0f) {
        return (height < FTS_SEA_LEVEL + 48.0f && localRelief < 36.0f) ? MAT_SAND : MAT_DIRT;
    }
    if (height < FTS_SEA_LEVEL + 128.0f) {
        return (height < FTS_SEA_LEVEL + 86.0f && localRelief < 58.0f) ? MAT_SAND : MAT_DIRT;
    }
    if (localRelief > 10.0f || height > 160.0f) return MAT_STONE;
    return MAT_DIRT;
}

#endif
