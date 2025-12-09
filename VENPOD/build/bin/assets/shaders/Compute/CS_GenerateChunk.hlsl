// =============================================================================
// VENPOD Chunk Generation Compute Shader
// Generates a single 64³ chunk using world coordinates for seamless terrain
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

// Output buffer (64³ voxels for this chunk)
RWStructuredBuffer<uint> ChunkVoxelOutput : register(u0);

// Sea level for oceans and lakes
// FIX: Reduced from 80.0 to 40.0 to generate more air above terrain
static const float SEA_LEVEL = 40.0;

// Generate terrain height for XZ coordinate
// CRITICAL: Uses world coordinates, not local chunk coordinates
// OPTIMIZED: Reduced octaves for better performance
float GenerateTerrainHeight(float2 xz, uint seed) {
    // Scale coordinates for terrain features
    float3 pos = float3(xz.x, 0, xz.y);

    // Add seed offset for variation
    pos += float3(seed * 0.01, 0, seed * 0.01);

    // Base continent shape (very low frequency) - reduced from 2 to 1 octave
    float continents = SimplexNoise3D(pos * 0.0008);

    // Rolling hills (medium frequency) - reduced from 4 to 2 octaves
    float hills = FBM3D(pos * 0.003, 2, 0.6, 2.0);

    // Mountains (single octave for performance)
    float mountains = abs(SimplexNoise3D(pos * 0.002)) * 2.0 - 1.0;  // Ridged effect

    // Combine layers
    float height = 0.0;
    height += continents * 40.0;         // Large-scale elevation changes
    height += hills * 25.0;              // Rolling terrain
    height += max(0, mountains) * 50.0;  // Sharp mountain peaks (only positive)

    // FIX: Reduced base level from 60.0 to 30.0 to generate more air
    height += 30.0;

    // FIX: Clamp to lower max height (was 200.0, now 60.0) to ensure air above terrain
    // This prevents 100% solid chunks - terrain tops out at Y=60, chunks go to Y=64
    return clamp(height, 5.0, 60.0);
}

// Select surface material based on biome
// OPTIMIZED: Simplified biome calculation for better performance
uint SelectSurfaceMaterial(float2 xz, uint seed, float height, float seaLevel) {
    float3 biomePos = float3(xz.x, 0, xz.y) * 0.001 + float3(seed * 0.1, 100, seed * 0.1);

    // Single noise sample for temperature
    float temperature = SimplexNoise3D(biomePos * 1.0) * 0.5 + 0.5;
    temperature -= (height - 60.0) * 0.003;  // Colder at high altitudes

    // Underwater terrain gets different materials
    if (height < seaLevel - 5) {
        return MAT_SAND;  // Sandy ocean floor
    }

    // Determine surface material (simplified)
    if (temperature < 0.35) {
        return MAT_ICE;  // Cold biome
    }
    else if (temperature < 0.65) {
        return MAT_DIRT;  // Temperate
    }
    else {
        return MAT_SAND;  // Hot/desert
    }
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
    else if (worldPos.y < SEA_LEVEL) {
        // Above terrain but below sea level - water
        material = MAT_WATER;
        state = 0;  // Water is dynamic
    }

    // Pack voxel data
    uint voxel = PackVoxel(material, variant, 0, state);

    // Write to output buffer using LOCAL index (not world index!)
    uint localIndex = localPos.x + localPos.y * chunkSize + localPos.z * chunkSize * chunkSize;
    ChunkVoxelOutput[localIndex] = voxel;
}
