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
    // PRIORITY 3: Active region offset for 4×4×4 optimization
    int activeRegionOffsetX;  // Camera chunk X - 1 (start of active region)
    int activeRegionOffsetY;  // Camera chunk Y - 1
    int activeRegionOffsetZ;  // Camera chunk Z - 1
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
// MUST match IsMovable() in CS_GravityChunk.hlsl!
bool IsVoxelActive(uint voxel) {
    uint material = GetMaterial(voxel);
    uint state = GetState(voxel);

    // Air is not active
    if (material == MAT_AIR) return false;

    // Static voxels (bedrock, frozen) are not active
    if (state & STATE_IS_STATIC) return false;

    // All movable materials are active (matches CS_GravityChunk.hlsl IsMovable)
    // Granular/falling: sand, dirt, gunpowder
    // Liquids: water, lava, oil, acid, honey, concrete (wet)
    // Gases: smoke, steam
    // Special: fire
    if (material == MAT_SAND || material == MAT_DIRT || material == MAT_GUNPOWDER ||
        material == MAT_WATER || material == MAT_LAVA || material == MAT_OIL ||
        material == MAT_ACID || material == MAT_HONEY || material == MAT_CONCRETE ||
        material == MAT_SMOKE || material == MAT_STEAM || material == MAT_FIRE) {
        return true;
    }

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

    // FIX: Use LOCAL buffer coordinates (groupId), NOT world coordinates
    // groupId directly maps to chunks in the LOCAL render buffer (0 to chunkCountX/Y/Z - 1)
    // The physics shader also uses these local coordinates, so they MUST match!
    //
    // For 1600x128x1600 buffer with CHUNK_SIZE=16:
    //   groupId.x: 0-99 (1600/16 chunks)
    //   groupId.y: 0-7  (128/16 chunks)
    //   groupId.z: 0-99 (1600/16 chunks)
    //
    // activeRegionOffset is NOT used here because:
    // 1. Both scanner and physics operate on the LOCAL buffer
    // 2. World coordinates are handled by UpdateActiveRegion (chunk copying)
    // 3. Using world offsets here would cause index mismatch with physics shader

    uint3 localChunkId = groupId;  // Direct mapping: dispatch coords = buffer coords

    // Calculate chunk index in chunk control buffer (LOCAL buffer coordinates)
    uint chunkIndex = localChunkId.x + localChunkId.y * chunkCountX + localChunkId.z * chunkCountX * chunkCountY;

    // Each thread handles a 2x2x2 region within the chunk
    uint3 baseVoxelInChunk = groupThreadId * 2;
    uint3 chunkBase = localChunkId * chunkSize;  // Local voxel position in buffer

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

        // ALWAYS add chunks with any voxels to active list
        // This ensures they get copied from READ to WRITE buffer
        // even if they're not actively simulating physics
        if (gs_particleCount > 0) {
            // Chunk has voxels - add to active list for copying
            uint listIndex;
            InterlockedAdd(ActiveChunkCount[0], 1, listIndex);
            ActiveChunkList[listIndex] = chunkIndex;
        }

        // Track active vs sleeping state for optimization
        if (gs_hasActiveVoxel > 0) {
            // Chunk has active voxels - wake it up
            ctrl.isActive = 1; // Active
            ctrl.sleepTimer = 0;
        } else if (ctrl.isActive == 1) {
            // Was active, now idle - increment sleep timer
            ctrl.sleepTimer++;
            if (ctrl.sleepTimer >= sleepThreshold) {
                ctrl.isActive = 0; // Sleeping
            }
        }

        ChunkControlBuffer[chunkIndex] = ctrl;
    }
}
