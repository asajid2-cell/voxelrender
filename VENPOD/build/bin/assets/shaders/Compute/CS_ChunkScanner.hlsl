// =============================================================================
// VENPOD Chunk Scanner Compute Shader
// Scans voxel grid to determine which chunks are active (have moving particles)
// and builds an active chunk list for sparse dispatch
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/BitPacking.hlsli"

// Constants for chunk scanning
cbuffer ChunkScanConstants : register(b0) {
    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint frameIndex;

    uint chunkCountX;
    uint chunkCountY;
    uint chunkCountZ;
    uint chunkSize;     // CHUNK_SIZE (16)

    uint sleepThreshold; // Frames before chunk goes to sleep
    uint padding0;
    uint padding1;
    uint padding2;
};

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGridRead : register(t0);

// Chunk management buffers
RWStructuredBuffer<ChunkControl> ChunkControlBuffer : register(u0);
RWBuffer<uint> ActiveChunkList : register(u1);
RWBuffer<uint> ActiveChunkCount : register(u2);

// Group shared memory for reduction
groupshared uint gs_hasActiveVoxel;
groupshared uint gs_particleCount;

// Check if a voxel is "active" (can move)
bool IsVoxelActive(uint voxel) {
    uint material = GetMaterial(voxel);
    uint state = GetState(voxel);

    // Air is not active
    if (material == MAT_AIR) return false;

    // Static voxels (bedrock, frozen) are not active
    if (state & STATE_IS_STATIC) return false;

    // Falling materials (sand, water, etc.) are active
    if (material == MAT_SAND || material == MAT_WATER ||
        material == MAT_LAVA || material == MAT_OIL) {
        return true;
    }

    // Fire is always active
    if (material == MAT_FIRE) return true;

    // Everything else is potentially active but settled
    return false;
}

// Thread group: one thread per voxel in chunk (16x16x16 = 4096 threads)
// But that's too many threads, so we use 8x8x8 = 512 threads, each handling 2x2x2 voxels
[numthreads(8, 8, 8)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
    // Initialize shared memory
    if (groupIndex == 0) {
        gs_hasActiveVoxel = 0;
        gs_particleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Calculate chunk index
    uint chunkIndex = groupId.x + groupId.y * chunkCountX + groupId.z * chunkCountX * chunkCountY;

    // Each thread handles a 2x2x2 region within the chunk
    uint3 baseVoxelInChunk = groupThreadId * 2;
    uint3 chunkBase = groupId * chunkSize;

    uint localActive = 0;
    uint localParticles = 0;

    // Scan 2x2x2 voxels per thread
    for (uint dz = 0; dz < 2; dz++) {
        for (uint dy = 0; dy < 2; dy++) {
            for (uint dx = 0; dx < 2; dx++) {
                uint3 localPos = baseVoxelInChunk + uint3(dx, dy, dz);
                uint3 worldPos = chunkBase + localPos;

                // Bounds check
                if (worldPos.x >= gridSizeX || worldPos.y >= gridSizeY || worldPos.z >= gridSizeZ)
                    continue;

                // Linear index in voxel grid
                uint voxelIdx = worldPos.x + worldPos.y * gridSizeX + worldPos.z * gridSizeX * gridSizeY;
                uint voxel = VoxelGridRead[voxelIdx];

                uint material = GetMaterial(voxel);
                if (material != MAT_AIR) {
                    localParticles++;
                }

                if (IsVoxelActive(voxel)) {
                    localActive = 1;
                }
            }
        }
    }

    // Atomic OR to shared memory
    if (localActive > 0) {
        InterlockedOr(gs_hasActiveVoxel, 1);
    }
    InterlockedAdd(gs_particleCount, localParticles);

    GroupMemoryBarrierWithGroupSync();

    // First thread writes chunk state
    if (groupIndex == 0) {
        ChunkControl ctrl = ChunkControlBuffer[chunkIndex];

        ctrl.particleCount = gs_particleCount;

        if (gs_hasActiveVoxel > 0) {
            // Chunk has active voxels - wake it up
            ctrl.isActive = 1; // Active
            ctrl.sleepTimer = 0;

            // Add to active list
            uint listIndex;
            InterlockedAdd(ActiveChunkCount[0], 1, listIndex);
            ActiveChunkList[listIndex] = chunkIndex;
        } else if (ctrl.isActive == 1) {
            // Was active, now idle - increment sleep timer
            ctrl.sleepTimer++;
            if (ctrl.sleepTimer >= sleepThreshold) {
                ctrl.isActive = 0; // Sleeping
            } else {
                // Still warming down - keep in active list
                uint listIndex;
                InterlockedAdd(ActiveChunkCount[0], 1, listIndex);
                ActiveChunkList[listIndex] = chunkIndex;
            }
        }
        // Sleeping chunks stay sleeping

        ChunkControlBuffer[chunkIndex] = ctrl;
    }
}
