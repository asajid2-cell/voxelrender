// =============================================================================
// VENPOD Chunk Generation Compute Shader
// Generates a single 64^3 chunk using world coordinates for seamless terrain
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"
#include "../Common/PCGRandom.hlsli"
#include "../Common/SimplexNoise.hlsli"

// Chunk-specific constants (MUST MATCH Chunk.cpp ChunkConstants layout!)
cbuffer ChunkConstants : register(b0) {
    // NOTE: Using individual int32 values instead of int3 to avoid HLSL cbuffer padding issues
    int chunkWorldOffsetX;  // World voxel X position
    int chunkWorldOffsetY;  // World voxel Y position
    int chunkWorldOffsetZ;  // World voxel Z position
    uint worldSeed;         // Procedural generation seed
    uint chunkSize;         // Always 64 for infinite chunks
    uint padding[3];        // Pad to 32 bytes (8 DWORDs)
};

// Output buffer (64^3 voxels for this chunk)
RWStructuredBuffer<uint> ChunkVoxelOutput : register(u0);

static const float TERRAIN_MIN_HEIGHT = 4.0;
static const float TERRAIN_MAX_HEIGHT = 124.0;
static const float SEA_LEVEL = 16.0;

float Smooth01(float value) {
    value = saturate(value);
    return value * value * (3.0 - 2.0 * value);
}

// Generate terrain height for XZ coordinate.
float GenerateTerrainHeight(float2 xz, uint seed) {
    float seedOffset = (float)(seed & 0xFFFFu);
    float3 pos = float3(xz.x + seedOffset * 3.17, 0.0, xz.y - seedOffset * 2.31);

    // Domain warp keeps the landforms connected without obvious grid-aligned bands.
    float warpX = FBM3D(pos * 0.0010 + float3(17.0, 0.0, 91.0), 3, 0.52, 2.0);
    float warpZ = FBM3D(pos * 0.0010 + float3(83.0, 0.0, 29.0), 3, 0.52, 2.0);
    float3 warped = pos + float3(warpX * 230.0, 0.0, warpZ * 230.0);

    // Broad lift creates continent-scale peaks and valleys.
    float continent = FBM3D(warped * 0.00055, 4, 0.58, 2.0);
    float mountainMask = Smooth01((continent + 0.10) * 1.15);

    // Ridged noise gives extreme-hills silhouettes without random needle spikes.
    float ridgeBase = FBM3D(warped * 0.00165 + float3(0.0, 37.0, 0.0), 4, 0.50, 2.0);
    float ridges = 1.0 - abs(ridgeBase);
    ridges = pow(saturate(ridges), 1.45);

    // Valley carving lowers broad passes between mountain chains.
    float valleyNoise = FBM3D(warped * 0.00125 + float3(41.0, 0.0, 73.0), 3, 0.55, 2.0);
    float valleys = pow(saturate(1.0 - abs(valleyNoise)), 1.35);

    float shelfBreaks = pow(saturate(abs(FBM3D(warped * 0.0033 + float3(9.0, 0.0, 53.0), 3, 0.48, 2.0))), 1.2);
    float shoulder = FBM3D(warped * 0.0065 + float3(5.0, 0.0, 11.0), 2, 0.48, 2.0);
    float fine = SimplexNoise3D(warped * 0.022);

    float height = 42.0;
    height += continent * 42.0;
    height += ridges * (0.35 + mountainMask) * 86.0;
    height -= valleys * (1.15 - mountainMask * 0.35) * 48.0;
    height += shelfBreaks * mountainMask * 18.0;
    height += shoulder * 10.0;
    height += fine * 3.0;

    return clamp(height, TERRAIN_MIN_HEIGHT, TERRAIN_MAX_HEIGHT);
}

float RiverMask(float2 xz, uint seed) {
    float seedOffset = (float)(seed & 0xFFFFu);
    float3 pos = float3(xz.x + seedOffset * 1.73, 0.0, xz.y - seedOffset * 1.19);
    float riverField = FBM3D(pos * 0.0018 + float3(113.0, 0.0, 47.0), 3, 0.55, 2.0);
    float riverCenter = 1.0 - smoothstep(0.018, 0.070, abs(riverField));
    float continuity = Smooth01(FBM3D(pos * 0.00055 + float3(7.0, 0.0, 211.0), 2, 0.5, 2.0) * 0.5 + 0.5);
    return riverCenter * continuity;
}

float BasinMask(float2 xz, uint seed) {
    float seedOffset = (float)(seed & 0xFFFFu);
    float3 pos = float3(xz.x - seedOffset * 0.91, 0.0, xz.y + seedOffset * 1.41);
    float basin = FBM3D(pos * 0.0009 + float3(31.0, 0.0, 157.0), 3, 0.55, 2.0);
    return Smooth01((basin - 0.38) * 2.8);
}

float WaterSurfaceHeight(float2 xz, uint seed, float terrainHeight) {
    float river = RiverMask(xz, seed);
    river *= saturate((54.0 - terrainHeight) / 38.0);
    float basin = BasinMask(xz, seed) * saturate((24.0 - terrainHeight) / 18.0);
    float waterMask = max(river, basin);

    if (waterMask <= 0.05) {
        return -1.0;
    }

    float riverSurface = max(terrainHeight + 1.0, SEA_LEVEL + river * 8.0);
    float basinSurface = SEA_LEVEL + basin * 11.0;
    return lerp(basinSurface, riverSurface, saturate(river * 1.4));
}

// Select surface material based on biome
// OPTIMIZED: Simplified biome calculation for better performance
uint SelectSurfaceMaterial(float2 xz, uint seed, float height, float seaLevel) {
    float3 biomePos = float3(xz.x, 0, xz.y) * 0.001 + float3(seed * 0.1, 100, seed * 0.1);

    // Single noise sample for temperature
    float temperature = SimplexNoise3D(biomePos * 1.0) * 0.5 + 0.5;
    temperature -= (height - 70.0) * 0.004;  // Colder at high altitudes
    float materialNoise = SimplexNoise3D(biomePos * 3.3 + float3(19.0, 0.0, 61.0)) * 0.5 + 0.5;

    // Underwater terrain gets different materials
    if (height < seaLevel - 5) {
        return MAT_SAND;  // Sandy ocean floor
    }

    if (height > 96.0) {
        return (temperature < 0.45) ? MAT_ICE : MAT_STONE;
    }
    if (height > 76.0) {
        return (materialNoise > 0.52) ? MAT_STONE : MAT_CONCRETE;
    }
    if (temperature > 0.72 && materialNoise > 0.45) {
        return MAT_SAND;
    }
    if (materialNoise > 0.84) {
        return MAT_CONCRETE;
    }
    if (temperature < 0.28) {
        return MAT_ICE;
    }
    return MAT_DIRT;
}

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Local position within chunk [0-63]
    uint3 localPos = DTid;

    // Bounds check
    if (any(localPos >= chunkSize)) {
        return;
    }

    // CRITICAL: Calculate world position for noise sampling
    // This ensures chunks seamlessly connect without visible seams
    int3 chunkWorldOffset = int3(chunkWorldOffsetX, chunkWorldOffsetY, chunkWorldOffsetZ);
    int3 worldPos = chunkWorldOffset + int3(localPos);

    // Generate random variant for visual variety (uses world position for consistency)
    uint random = Random3D(uint3(worldPos), worldSeed);
    uint variant = random & 0xFF;

    // ===== PROPER TERRAIN GENERATION =====
    // Generate terrain height for this XZ column using world coordinates
    float terrainHeight = GenerateTerrainHeight(float2(worldPos.x, worldPos.z), worldSeed);
    float waterSurface = WaterSurfaceHeight(float2(worldPos.x, worldPos.z), worldSeed, terrainHeight);

    // Default to air
    uint material = MAT_AIR;
    uint state = 0;

    // Check if this voxel is below terrain surface
    if (worldPos.y < terrainHeight) {
        // Underground voxel
        float depthBelowSurface = terrainHeight - worldPos.y;

        if (depthBelowSurface < 1.5) {
            // Surface layer - use biome-appropriate material
            material = SelectSurfaceMaterial(float2(worldPos.x, worldPos.z), worldSeed, terrainHeight, SEA_LEVEL);
            state = STATE_IS_STATIC;
        }
        else if (depthBelowSurface < 5.0) {
            // Shallow subsurface - dirt
            material = MAT_DIRT;
            state = STATE_IS_STATIC;
        }
        else {
            // Deep underground - stone (ore veins removed for performance)
            material = MAT_STONE;
            state = STATE_IS_STATIC;
        }
    }
    else if (waterSurface > 0.0 && worldPos.y < waterSurface) {
        // Water is deliberate: rivers and basin lakes only, not a global default plane.
        material = MAT_WATER;
        state = STATE_IS_STATIC;  // Terrain oceans are static; brush-painted water remains dynamic
    }

    // Pack voxel data
    uint voxel = PackVoxel(material, variant, 0, state);

    // Write to output buffer using LOCAL index (not world index!)
    uint localIndex = localPos.x + localPos.y * chunkSize + localPos.z * chunkSize * chunkSize;
    ChunkVoxelOutput[localIndex] = voxel;
}
