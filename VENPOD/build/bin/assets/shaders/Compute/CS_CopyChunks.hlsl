// =============================================================================
// VENPOD Buffer Copy Compute Shader
// Copies entire READ buffer to WRITE buffer to preserve non-active chunks
// =============================================================================

#include "../Common/SharedTypes.hlsli"

cbuffer CopyConstants : register(b0) {
    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint padding;
};

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGridIn : register(t0);

// Output voxel grid (write)
RWStructuredBuffer<uint> VoxelGridOut : register(u0);

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= gridSizeX || DTid.y >= gridSizeY || DTid.z >= gridSizeZ) {
        return;
    }

    // Linear index
    uint index = DTid.x + DTid.y * gridSizeX + DTid.z * gridSizeX * gridSizeY;

    // Simple copy
    VoxelGridOut[index] = VoxelGridIn[index];
}
