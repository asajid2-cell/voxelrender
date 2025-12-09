// =============================================================================
// VENPOD Chunk-to-Buffer Copy Compute Shader
// Copies a 64³ chunk into a specific region of the 256³ render buffer
// =============================================================================

#include "../Common/SharedTypes.hlsli"

// Copy parameters
cbuffer CopyChunkConstants : register(b0) {
    uint destOffsetX;      // Destination offset in 256³ buffer (0-192 in steps of 64)
    uint destOffsetY;      // Destination offset in 256³ buffer (0-192 in steps of 64)
    uint destOffsetZ;      // Destination offset in 256³ buffer (0-192 in steps of 64)
    uint chunkSize;        // Always 64 for infinite chunks
    uint destGridSizeX;    // Always 256 for render buffer
    uint destGridSizeY;    // Always 256 for render buffer
    uint destGridSizeZ;    // Always 256 for render buffer
    uint padding;
};

// Source: Single 64³ chunk buffer
StructuredBuffer<uint> ChunkVoxelInput : register(t0);

// Destination: 256³ render buffer
RWStructuredBuffer<uint> RenderBufferOutput : register(u0);

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check (operating on 64³ chunk)
    if (DTid.x >= chunkSize || DTid.y >= chunkSize || DTid.z >= chunkSize) {
        return;
    }

    // Calculate source index (local chunk coordinates)
    uint srcIndex = DTid.x + DTid.y * chunkSize + DTid.z * chunkSize * chunkSize;

    // Calculate destination coordinates in 256³ buffer
    uint destX = destOffsetX + DTid.x;
    uint destY = destOffsetY + DTid.y;
    uint destZ = destOffsetZ + DTid.z;

    // Bounds check destination (ensure we don't write outside 256³ buffer)
    if (destX >= destGridSizeX || destY >= destGridSizeY || destZ >= destGridSizeZ) {
        return;  // Skip voxels outside render buffer
    }

    // Calculate destination index in 256³ buffer
    uint destIndex = destX + destY * destGridSizeX + destZ * destGridSizeX * destGridSizeY;

    // Copy voxel from chunk to render buffer
    RenderBufferOutput[destIndex] = ChunkVoxelInput[srcIndex];
}
