// =============================================================================
// VENPOD Initialization Compute Shader
// Procedural terrain generation using Simplex noise
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"
#include "../Common/PCGRandom.hlsli"
#include "../Common/SimplexNoise.hlsli"

// Root constants
cbuffer InitConstants : register(b0) {
    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint seed;
};

// Output buffer
RWStructuredBuffer<uint> VoxelOutput : register(u0);

// Sea level for oceans and lakes
static const float SEA_LEVEL = 80.0;

// Generate terrain height for XZ coordinate
float GenerateTerrainHeight(float2 xz, uint worldSeed) {
    // Scale coordinates for terrain features
    float3 pos = float3(xz.x, 0, xz.y);

    // Add seed offset for variation
    pos += float3(worldSeed * 0.01, 0, worldSeed * 0.01);

    // Base continent shape (very low frequency)
    float continents = FBM3D(pos * 0.0008, 2, 0.5, 2.0);

    // Rolling hills (medium frequency)
    float hills = FBM3D(pos * 0.003, 4, 0.6, 2.0);

    // Fine detail (high frequency)
    float detail = FBM3D(pos * 0.015, 3, 0.5, 2.0);

    // Mountains (ridged noise for sharp peaks)
    float mountains = RidgedNoise3D(pos * 0.002, 4, 0.5);

    // Combine layers
    float height = 0.0;
    height += continents * 40.0;         // Large-scale elevation changes
    height += hills * 25.0;              // Rolling terrain
    height += detail * 8.0;              // Small bumps
    height += max(0, mountains) * 60.0;  // Sharp mountain peaks (only positive)

    // Base level at 60, with variation
    height += 60.0;

    // Clamp to reasonable range
    return clamp(height, 5.0, gridSizeY - 10.0);
}

// Select surface material based on biome
uint SelectSurfaceMaterial(float2 xz, uint worldSeed, float height, float seaLevel) {
    float3 biomePos = float3(xz.x, 0, xz.y) * 0.001 + float3(worldSeed * 0.1, 100, worldSeed * 0.1);

    // Temperature (decreases with height and latitude)
    float temperature = SimplexNoise3D(biomePos * 1.5) * 0.5 + 0.5;
    temperature -= (height - 60.0) * 0.003;  // Colder at high altitudes

    // Moisture
    float moisture = SimplexNoise3D(biomePos * 2.0 + float3(500, 0, 0)) * 0.5 + 0.5;

    // Underwater terrain gets different materials
    if (height < seaLevel - 5) {
        return MAT_SAND;  // Sandy ocean floor
    }

    // Determine surface material
    if (temperature < 0.3) {
        return MAT_ICE;  // Cold biome
    }
    else if (temperature < 0.6) {
        return (moisture > 0.5) ? MAT_DIRT : MAT_STONE;  // Temperate
    }
    else {
        return MAT_SAND;  // Hot/desert
    }
}

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= gridSizeX || DTid.y >= gridSizeY || DTid.z >= gridSizeZ) {
        return;
    }

    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint index = LinearIndex3D(DTid, gridSize);

    // Generate random variant for visual variety
    uint random = Random3D(DTid, seed);
    uint variant = random & 0xFF;

    // Default to air
    uint material = MAT_AIR;
    uint state = 0;

    // Generate terrain height for this XZ column
    float terrainHeight = GenerateTerrainHeight(float2(DTid.x, DTid.z), seed);

    // Bedrock floor (unbreakable bottom layer)
    if (DTid.y == 0) {
        material = MAT_BEDROCK;
        state = STATE_IS_STATIC;
    }
    // Below terrain surface
    else if (DTid.y < terrainHeight) {
        // 3D cave carving
        float3 cavePos = float3(DTid.xyz) * 0.03 + float3(seed * 0.01, 0, 0);
        float caveNoise = FBM3D(cavePos, 3, 0.5, 2.0);

        // Carve caves where noise is below threshold
        if (caveNoise < -0.25) {
            material = MAT_AIR;  // Cave hollow
        }
        else {
            // Determine subsurface material
            float depthFromSurface = terrainHeight - DTid.y;

            if (depthFromSurface < 1.5) {
                // Surface layer - biome-specific
                material = SelectSurfaceMaterial(float2(DTid.x, DTid.z), seed, terrainHeight, SEA_LEVEL);
                state = STATE_IS_STATIC;
            }
            else if (depthFromSurface < 5.0) {
                // Subsoil
                material = MAT_DIRT;
                state = STATE_IS_STATIC;
            }
            else {
                // Deep underground - mostly stone with ore veins
                material = MAT_STONE;
                state = STATE_IS_STATIC;

                // Ore deposits (rare)
                if (DTid.y < 50) {
                    float oreNoise = SimplexNoise3D(float3(DTid.xyz) * 0.08 + float3(1000, 0, 0));
                    if (oreNoise > 0.7) {
                        material = MAT_LAVA;  // Rare lava pockets (placeholder for ore)
                    }
                }
            }
        }
    }

    // Water bodies - fill air below sea level with water
    if (material == MAT_AIR && DTid.y < SEA_LEVEL) {
        material = MAT_WATER;
        state = 0;  // Water is movable (not static)
    }

    // Beach transition - convert dirt/stone to sand near water level
    if (material == MAT_DIRT || material == MAT_STONE) {
        if (DTid.y >= SEA_LEVEL - 3 && DTid.y <= SEA_LEVEL + 2) {
            // Check if near water (noise-based shore detection)
            float shoreNoise = SimplexNoise3D(float3(DTid.x, 0, DTid.z) * 0.05);
            if (shoreNoise > -0.3) {  // Creates irregular shorelines
                material = MAT_SAND;
            }
        }
    }

    // Pack and write voxel
    uint voxel = PackVoxel(material, variant, 0, state);
    VoxelOutput[index] = voxel;
}
