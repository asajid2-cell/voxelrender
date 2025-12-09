// =============================================================================
// VENPOD Chunk-Based Gravity Physics Compute Shader
// Processes only active chunks for sparse simulation
// Each thread group handles one chunk from the active list
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"

// Physics constants
cbuffer PhysicsConstants : register(b0) {
    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint frameIndex;

    float deltaTime;
    float gravity;
    uint simulationFlags;
    uint chunkSize;     // CHUNK_SIZE (16)

    uint chunkCountX;
    uint chunkCountY;
    uint chunkCountZ;
    uint padding;
}

// Active chunk list - array of chunk indices to simulate
Buffer<uint> ActiveChunkList : register(t0);

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGridIn : register(t1);

// Output voxel grid (write)
RWStructuredBuffer<uint> VoxelGridOut : register(u0);

// Material properties lookup (helpers for physics)
bool IsMovable(uint material) {
    return material == MAT_SAND || material == MAT_WATER || material == MAT_LAVA || material == MAT_OIL;
}

bool IsEmpty(uint material) {
    return material == MAT_AIR;
}

// Decompose chunk index to 3D position
uint3 ChunkIndexToPos(uint chunkIndex) {
    uint x = chunkIndex % chunkCountX;
    uint y = (chunkIndex / chunkCountX) % chunkCountY;
    uint z = chunkIndex / (chunkCountX * chunkCountY);
    return uint3(x, y, z);
}

// Safe voxel access
uint GetVoxelSafe(int3 pos) {
    if (pos.x < 0 || pos.x >= (int)gridSizeX ||
        pos.y < 0 || pos.y >= (int)gridSizeY ||
        pos.z < 0 || pos.z >= (int)gridSizeZ) {
        return PackVoxel(MAT_BEDROCK, 0, 0, 0); // Out of bounds = bedrock
    }

    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint idx = LinearIndex3D(uint3(pos), gridSize);
    return VoxelGridIn[idx];
}

void SetVoxel(uint3 pos, uint voxel) {
    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint idx = LinearIndex3D(pos, gridSize);
    VoxelGridOut[idx] = voxel;
}

// PCG hash for pseudo-random
uint PCGHash(uint seed) {
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Each thread group handles one active chunk
// Within the thread group, threads process individual voxels
// [numthreads(8, 8, 4)] = 256 threads per chunk
// But we need to cover 16x16x16 = 4096 voxels per chunk
// So each thread handles 16 voxels (4x4 in one dimension)
[numthreads(8, 8, 4)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    // Get chunk index from active list
    uint chunkIndex = ActiveChunkList[groupId.x];

    // Convert chunk index to 3D chunk position
    uint3 chunkPos = ChunkIndexToPos(chunkIndex);

    // Calculate base voxel position for this chunk
    uint3 chunkBase = chunkPos * chunkSize;

    // Each thread handles a 2x2x4 region (to cover 16x16x16 = 4096 voxels with 256 threads)
    uint3 threadBase = uint3(
        groupThreadId.x * 2,
        groupThreadId.y * 2,
        groupThreadId.z * 4
    );

    // Process 2x2x4 voxels per thread
    for (uint dz = 0; dz < 4; dz++) {
        for (uint dy = 0; dy < 2; dy++) {
            for (uint dx = 0; dx < 2; dx++) {
                uint3 localPos = threadBase + uint3(dx, dy, dz);
                int3 pos = int3(chunkBase + localPos);

                // Bounds check
                if (pos.x >= (int)gridSizeX || pos.y >= (int)gridSizeY || pos.z >= (int)gridSizeZ) {
                    continue;
                }

                uint currentVoxel = GetVoxelSafe(pos);
                uint material = GetMaterial(currentVoxel);

                // Default: copy voxel unchanged
                uint outputVoxel = currentVoxel;

                // Skip air (don't copy it - prevents overwriting painted voxels in WRITE buffer)
                if (material == MAT_AIR) {
                    continue;  // Don't write anything for AIR voxels
                }

                // Bedrock stays in place
                if (material == MAT_BEDROCK) {
                    SetVoxel(uint3(pos), outputVoxel);
                    continue;
                }

                // Skip static/non-movable materials
                if (!IsMovable(material)) {
                    SetVoxel(uint3(pos), outputVoxel);
                    continue;
                }

                // === FALLING SAND PHYSICS ===

                // Check voxel below
                int3 belowPos = pos + int3(0, -1, 0);

                if (belowPos.y >= 0) {
                    uint belowVoxel = GetVoxelSafe(belowPos);
                    uint belowMaterial = GetMaterial(belowVoxel);

                    // Can fall straight down?
                    if (IsEmpty(belowMaterial)) {
                        // Move down
                        outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));  // Clear current
                        SetVoxel(uint3(belowPos), outputVoxel);             // Fill below
                        continue;
                    }

                    // Sand: try diagonal down (slide)
                    if (material == MAT_SAND) {
                        // Random direction based on position + frame
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                        int dir = (rng & 1) ? 1 : -1;

                        int3 diagPos1 = pos + int3(dir, -1, 0);
                        int3 diagPos2 = pos + int3(-dir, -1, 0);

                        // Try first diagonal
                        if (diagPos1.x >= 0 && diagPos1.x < (int)gridSizeX) {
                            uint diagVoxel = GetVoxelSafe(diagPos1);
                            if (IsEmpty(GetMaterial(diagVoxel))) {
                                outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                SetVoxel(uint3(diagPos1), outputVoxel);
                                continue;
                            }
                        }

                        // Try second diagonal
                        if (diagPos2.x >= 0 && diagPos2.x < (int)gridSizeX) {
                            uint diagVoxel = GetVoxelSafe(diagPos2);
                            if (IsEmpty(GetMaterial(diagVoxel))) {
                                outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                SetVoxel(uint3(diagPos2), outputVoxel);
                                continue;
                            }
                        }
                    }

                    // Liquids: horizontal spread (simplified)
                    if (IsLiquid(material)) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                        int dir = (rng & 1) ? 1 : -1;

                        int3 sidePos = pos + int3(dir, 0, 0);

                        if (sidePos.x >= 0 && sidePos.x < (int)gridSizeX) {
                            uint sideVoxel = GetVoxelSafe(sidePos);
                            if (IsEmpty(GetMaterial(sideVoxel))) {
                                // Spread horizontally
                                outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                SetVoxel(uint3(sidePos), outputVoxel);
                                continue;
                            }
                        }
                    }
                }

                // No movement possible - stay put
                SetVoxel(uint3(pos), outputVoxel);
            }
        }
    }
}
