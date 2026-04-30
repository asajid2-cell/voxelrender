// =============================================================================
// VENPOD Chunk Generation Compute Shader
// Generates a 64^3 world-space chunk for the vertical traversal sandbox.
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"
#include "../Common/PCGRandom.hlsli"
#include "../Common/SimplexNoise.hlsli"

cbuffer ChunkConstants : register(b0) {
    int chunkWorldOffsetX;
    int chunkWorldOffsetY;
    int chunkWorldOffsetZ;
    uint worldSeed;
    uint chunkSize;
    uint padding[3];
};

RWStructuredBuffer<uint> ChunkVoxelOutput : register(u0);

static const float TERRAIN_MIN_HEIGHT = -332.0;
static const float TERRAIN_MAX_HEIGHT = 664.0;
static const float SEA_LEVEL = -48.0;
static const uint TERRAIN_PRESET_BALANCED = 0;
static const uint TERRAIN_PRESET_EXTREME_MOUNTAINS = 1;
static const uint TERRAIN_PRESET_RAVINES = 2;
static const uint TERRAIN_PRESET_SPIRES = 3;
static const uint TERRAIN_PRESET_CAVERNOUS = 4;
static const uint TERRAIN_PRESET_PERF_TEST = 5;
static const uint TERRAIN_PRESET = TERRAIN_PRESET_EXTREME_MOUNTAINS;

float Smooth01(float value) {
    value = saturate(value);
    return value * value * (3.0 - 2.0 * value);
}

float Ridged(float value, float power) {
    return pow(saturate(1.0 - abs(value)), power);
}

float2 DomainWarp(float2 xz, uint seed) {
    float seedOffset = (float)(seed & 0xFFFFu);
    float3 p = float3(xz.x + seedOffset * 3.17, 0.0, xz.y - seedOffset * 2.31);
    float warpX = FBM3D(p * 0.00075 + float3(17.0, 0.0, 91.0), 4, 0.52, 2.0);
    float warpZ = FBM3D(p * 0.00075 + float3(83.0, 0.0, 29.0), 4, 0.52, 2.0);
    return xz + float2(warpX, warpZ) * 310.0;
}

float OriginScenicUplift(float2 xz) {
    float d = length(xz - float2(96.0, 96.0));
    return (1.0 - Smooth01(d / 420.0)) * 170.0;
}

float RavineMask(float2 xz, uint seed) {
    float2 w = DomainWarp(xz * 0.95 + 1300.0, seed);
    float3 p = float3(w.x, 0.0, w.y);
    float riverField = FBM3D(p * 0.00135 + float3(211.0, 0.0, 37.0), 4, 0.57, 2.0);
    float canyon = 1.0 - smoothstep(0.018, 0.105, abs(riverField));
    float continuity = Smooth01(FBM3D(p * 0.00042 + float3(7.0, 0.0, 181.0), 3, 0.5, 2.0) * 0.5 + 0.5);
    return canyon * continuity;
}

float SpireMask(float2 xz, uint seed) {
    float2 w = DomainWarp(xz + 2700.0, seed);
    float3 p = float3(w.x, 0.0, w.y);
    float n1 = FBM3D(p * 0.0045 + float3(61.0, 0.0, 19.0), 3, 0.5, 2.0);
    float n2 = FBM3D(p * 0.0068 + float3(17.0, 0.0, 97.0), 2, 0.5, 2.0);
    float core = Ridged(n1, 4.5) * Ridged(n2, 2.2);
    float clusters = Smooth01(FBM3D(p * 0.00095 + float3(5.0, 0.0, 233.0), 3, 0.55, 2.0) * 0.8 + 0.45);
    return core * clusters;
}

float MountainMask(float2 xz, uint seed) {
    float2 w = DomainWarp(xz, seed);
    float continent = FBM3D(float3(w.x, 0.0, w.y) * 0.00048, 5, 0.57, 2.0);
    return Smooth01((continent + 0.18) * 1.05);
}

float GenerateTerrainHeight(float2 xz, uint seed, out float mountainMask, out float spireMask, out float ravineMask) {
    float2 w = DomainWarp(xz, seed);
    float3 p = float3(w.x, 0.0, w.y);

    float continent = FBM3D(p * 0.00044, 5, 0.58, 2.0);
    mountainMask = Smooth01((continent + 0.15) * 1.08);

    float ridgeA = Ridged(FBM3D(p * 0.00125 + float3(0.0, 37.0, 0.0), 5, 0.50, 2.0), 1.35);
    float ridgeB = Ridged(FBM3D(p * 0.00210 + float3(83.0, 0.0, 11.0), 4, 0.52, 2.0), 1.80);
    float broadValley = Ridged(FBM3D(p * 0.00086 + float3(41.0, 0.0, 73.0), 4, 0.55, 2.0), 1.15);
    float shelf = pow(saturate(abs(FBM3D(p * 0.0034 + float3(9.0, 0.0, 53.0), 3, 0.48, 2.0))), 1.25);
    float shoulder = FBM3D(p * 0.0060 + float3(5.0, 0.0, 11.0), 2, 0.48, 2.0);

    spireMask = SpireMask(xz, seed) * (0.35 + mountainMask * 0.9);
    ravineMask = RavineMask(xz, seed);

    float height = -85.0;
    height += continent * 210.0;
    height += ridgeA * (125.0 + mountainMask * 170.0);
    height += ridgeB * mountainMask * 95.0;
    height += spireMask * 360.0;
    height += shelf * mountainMask * 42.0;
    height += shoulder * 20.0;
    height += OriginScenicUplift(xz);
    height -= broadValley * (90.0 - mountainMask * 30.0);
    height -= ravineMask * 230.0;

    if (TERRAIN_PRESET == TERRAIN_PRESET_RAVINES) {
        height -= ravineMask * 115.0;
    } else if (TERRAIN_PRESET == TERRAIN_PRESET_SPIRES) {
        height += spireMask * 170.0;
    } else if (TERRAIN_PRESET == TERRAIN_PRESET_CAVERNOUS) {
        height -= broadValley * 55.0;
    } else if (TERRAIN_PRESET == TERRAIN_PRESET_PERF_TEST) {
        height = lerp(height, 80.0 + continent * 60.0, 0.65);
    }

    float terraceStep = lerp(10.0, 22.0, mountainMask);
    float terraced = floor(height / terraceStep) * terraceStep;
    height = lerp(height, terraced, 0.26 + mountainMask * 0.20);

    return clamp(height, TERRAIN_MIN_HEIGHT, TERRAIN_MAX_HEIGHT);
}

float CaveMask(float3 worldPos, float terrainHeight, float mountainMask, float ravineMask, uint seed) {
    float depth = terrainHeight - worldPos.y;
    if (depth < 10.0) {
        return 0.0;
    }

    float3 p = worldPos + float3((float)(seed & 0xFFFFu) * 1.3, 0.0, 73.0);
    float tunnelA = Ridged(FBM3D(p * 0.0085 + float3(13.0, 47.0, 97.0), 3, 0.52, 2.0), 2.2);
    float tunnelB = Ridged(FBM3D(p * 0.0042 + float3(211.0, 19.0, 7.0), 4, 0.54, 2.0), 2.8);
    float cavern = Smooth01(FBM3D(p * 0.0021 + float3(17.0, 89.0, 191.0), 3, 0.5, 2.0) * 0.7 + 0.35);
    float verticalGate = Smooth01((depth - 20.0) / 90.0) * Smooth01((220.0 - depth) / 130.0);
    float mouthBoost = ravineMask * Smooth01((terrainHeight - worldPos.y) / 42.0);
    return (tunnelA * 0.55 + tunnelB * 0.35 + cavern * 0.30 + mouthBoost * 0.65) *
           verticalGate * (0.55 + mountainMask * 0.45);
}

float WaterSurfaceHeight(float2 xz, uint seed, float terrainHeight, float ravineMask) {
    float basin = Smooth01((FBM3D(float3(xz.x, 0.0, xz.y) * 0.00082 + float3(31.0, 0.0, 157.0), 3, 0.55, 2.0) - 0.30) * 2.1);
    basin *= saturate((-8.0 - terrainHeight) / 130.0);
    float river = ravineMask * saturate((90.0 - terrainHeight) / 180.0);
    float waterMask = max(basin, river * 0.75);

    if (waterMask <= 0.05) {
        return -9999.0;
    }

    float basinSurface = SEA_LEVEL + basin * 16.0;
    float riverSurface = max(terrainHeight + 1.0, SEA_LEVEL + river * 20.0);
    return lerp(basinSurface, riverSurface, saturate(river * 1.25));
}

uint SelectSurfaceMaterial(float2 xz, uint seed, float height, float seaLevel, float mountainMask, float spireMask, float ravineMask) {
    float3 biomePos = float3(xz.x, 0, xz.y) * 0.001 + float3(seed * 0.1, 100, seed * 0.1);
    float temperature = SimplexNoise3D(biomePos) * 0.5 + 0.5;
    temperature -= (height - 120.0) * 0.0018;
    float materialNoise = SimplexNoise3D(biomePos * 3.3 + float3(19.0, 0.0, 61.0)) * 0.5 + 0.5;

    if (height < seaLevel + 4.0) {
        return MAT_SAND;
    }
    if (ravineMask > 0.55 && materialNoise > 0.35) {
        return MAT_STONE;
    }
    if (spireMask > 0.28 || height > 430.0) {
        return (temperature < 0.42) ? MAT_ICE : MAT_STONE;
    }
    if (mountainMask > 0.70 || height > 220.0) {
        return (materialNoise > 0.45) ? MAT_STONE : MAT_CONCRETE;
    }
    if (temperature > 0.73 && materialNoise > 0.45) {
        return MAT_SAND;
    }
    if (materialNoise > 0.84) {
        return MAT_CONCRETE;
    }
    return MAT_DIRT;
}

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint3 localPos = DTid;
    if (any(localPos >= chunkSize)) {
        return;
    }

    int3 chunkWorldOffset = int3(chunkWorldOffsetX, chunkWorldOffsetY, chunkWorldOffsetZ);
    int3 worldPosI = chunkWorldOffset + int3(localPos);
    float3 worldPos = float3(worldPosI);
    float2 xz = float2(worldPos.x, worldPos.z);

    uint random = Random3D(uint3(worldPosI), worldSeed);
    uint variant = random & 0xFF;

    float mountainMask;
    float spireMask;
    float ravineMask;
    float terrainHeight = GenerateTerrainHeight(xz, worldSeed, mountainMask, spireMask, ravineMask);
    float waterSurface = WaterSurfaceHeight(xz, worldSeed, terrainHeight, ravineMask);

    uint material = MAT_AIR;
    uint state = 0;

    const int bedrockBaseY = (int)TERRAIN_MIN_HEIGHT;
    const int bedrockTopY = bedrockBaseY + 2 + (int)(random & 3u);
    if (worldPosI.y <= bedrockTopY) {
        material = MAT_BEDROCK;
        state = STATE_IS_STATIC;
    }

    float density = terrainHeight - worldPos.y;

    // Shelf and overhang density makes some ridge faces protrude instead of
    // staying pure heightfields. It is bounded so chunks remain coherent.
    float shelfBand = Ridged(frac((worldPos.y + FBM3D(float3(xz.x, 0.0, xz.y) * 0.002, 2, 0.5, 2.0) * 30.0) / 38.0) * 2.0 - 1.0, 2.3);
    density += shelfBand * mountainMask * 18.0;

    float caveMask = CaveMask(worldPos, terrainHeight, mountainMask, ravineMask, worldSeed);
    bool carvedCave = caveMask > 0.72;

    if (material == MAT_BEDROCK) {
        // Bottom shell is intentionally uncarvable and uneditable by brush.
    } else if (density > 0.0 && !carvedCave) {
        float depthBelowSurface = max(0.0, terrainHeight - worldPos.y);
        if (depthBelowSurface < 2.0) {
            material = SelectSurfaceMaterial(xz, worldSeed, terrainHeight, SEA_LEVEL, mountainMask, spireMask, ravineMask);
            state = STATE_IS_STATIC;
        } else if (depthBelowSurface < 7.0 && mountainMask < 0.55) {
            material = MAT_DIRT;
            state = STATE_IS_STATIC;
        } else {
            material = MAT_STONE;
            state = STATE_IS_STATIC;
        }
    } else if (waterSurface > -9000.0 && worldPos.y < waterSurface && worldPos.y > terrainHeight - 18.0) {
        material = MAT_WATER;
        state = STATE_IS_STATIC;
    }

    uint voxel = PackVoxel(material, variant, 0, state);
    uint localIndex = localPos.x + localPos.y * chunkSize + localPos.z * chunkSize * chunkSize;
    ChunkVoxelOutput[localIndex] = voxel;
}
