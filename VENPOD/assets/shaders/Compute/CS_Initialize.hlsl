// =============================================================================
// VENPOD Initialization Compute Shader
// Fills voxel buffer with test pattern / noise
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"
#include "../Common/PCGRandom.hlsli"

// Root constants
cbuffer InitConstants : register(b0) {
    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint seed;
};

// Output buffer
RWStructuredBuffer<uint> VoxelOutput : register(u0);

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= gridSizeX || DTid.y >= gridSizeY || DTid.z >= gridSizeZ) {
        return;
    }

    // Calculate linear index
    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint index = LinearIndex3D(DTid, gridSize);

    // Generate random values for this voxel
    uint random = Random3D(DTid, seed);
    uint variant = random & 0xFF;

    // Default to air
    uint material = MAT_AIR;
    uint state = 0;

    // Simple test pattern for visibility
    // Bedrock floor (bottom layer)
    if (DTid.y == 0) {
        material = MAT_BEDROCK;
        state = STATE_IS_STATIC;
    }
    // Stone layer (layers 1-20)
    else if (DTid.y < 20) {
        material = MAT_STONE;
        state = STATE_IS_STATIC;
    }
    // Dirt layer (layers 20-30)
    else if (DTid.y < 30) {
        material = MAT_DIRT;
        state = STATE_IS_STATIC;
    }
    // Sand pile on top (tall tower in center for visibility)
    else if (DTid.y < 60) {
        // Create a column of sand in the center
        float3 center = float3(gridSizeX * 0.5f, 0, gridSizeZ * 0.5f);
        float dist = length(float3(DTid.x, 0, DTid.z) - center);
        if (dist < 20.0f) {
            material = MAT_SAND;
            state = 0;  // Not static, can fall
        }
    }

    // Pack voxel data
    uint voxel = PackVoxel(material, variant, 0, state);

    // Write to buffer
    VoxelOutput[index] = voxel;
}
