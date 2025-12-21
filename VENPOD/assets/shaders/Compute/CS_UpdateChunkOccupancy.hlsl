// =============================================================================
// VENPOD Chunk Occupancy Update Compute Shader
// Scans voxel buffer and updates occupancy texture for raymarching acceleration
// Each thread group processes one chunk (64x64x64 voxels)
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"

// Constants
cbuffer OccupancyConstants : register(b0) {
    uint gridSizeX;      // Full grid size in voxels (1600)
    uint gridSizeY;      // Full grid size in voxels (128)
    uint gridSizeZ;      // Full grid size in voxels (1600)
    uint chunkSize;      // Voxels per chunk (64)

    uint chunksX;        // Number of chunks in X (25)
    uint chunksY;        // Number of chunks in Y (2)
    uint chunksZ;        // Number of chunks in Z (25)
    uint padding;
}

// Input voxel grid
StructuredBuffer<uint> VoxelGrid : register(t0);

// Output occupancy texture (1 byte per chunk: 0 = empty, 1 = has solid voxels)
// Using RWTexture3D for efficient random access in raymarcher
RWTexture3D<uint> ChunkOccupancy : register(u0);

// Shared memory for reduction (count non-air voxels in chunk)
groupshared uint gs_hasSolid;

// Each thread group = one chunk
// Use 8x8x8 threads = 512 threads per chunk
// Each thread processes 8x8x8 = 512 voxels (64³/512 = 512 voxels per thread)
[numthreads(8, 8, 8)]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint flatIdx : SV_GroupIndex) {
    // Initialize shared memory (only first thread)
    if (flatIdx == 0) {
        gs_hasSolid = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Calculate chunk's voxel range
    uint3 chunkVoxelStart = groupId * chunkSize;

    // Each thread scans a subset of the chunk
    // With 512 threads and 64³ = 262,144 voxels per chunk,
    // each thread processes 512 voxels
    uint voxelsPerThread = (chunkSize * chunkSize * chunkSize) / 512;  // = 512

    // Calculate starting voxel for this thread within the chunk
    uint threadLinearStart = flatIdx * voxelsPerThread;

    // Scan voxels assigned to this thread
    bool foundSolid = false;
    for (uint i = 0; i < voxelsPerThread && !foundSolid; ++i) {
        uint linearIdx = threadLinearStart + i;

        // Convert linear index to 3D position within chunk
        uint localZ = linearIdx / (chunkSize * chunkSize);
        uint localY = (linearIdx / chunkSize) % chunkSize;
        uint localX = linearIdx % chunkSize;

        // Calculate global voxel position
        uint3 voxelPos = chunkVoxelStart + uint3(localX, localY, localZ);

        // Bounds check
        if (voxelPos.x >= gridSizeX || voxelPos.y >= gridSizeY || voxelPos.z >= gridSizeZ) {
            continue;
        }

        // Sample voxel
        uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
        uint voxelIdx = LinearIndex3D(voxelPos, gridSize);
        uint voxel = VoxelGrid[voxelIdx];
        uint material = GetMaterial(voxel);

        // Check if solid (anything that's not air)
        if (material != MAT_AIR) {
            foundSolid = true;
        }
    }

    // Atomic OR to shared memory if we found solid
    if (foundSolid) {
        InterlockedOr(gs_hasSolid, 1);
    }

    GroupMemoryBarrierWithGroupSync();

    // First thread writes result to occupancy texture
    if (flatIdx == 0) {
        ChunkOccupancy[groupId] = gs_hasSolid;
    }
}
