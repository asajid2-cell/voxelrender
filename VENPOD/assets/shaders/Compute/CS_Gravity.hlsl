// =============================================================================
// VENPOD Gravity & Basic Physics Compute Shader
// Implements falling sand, liquids, and collision detection
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
    uint padding;
}

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGridIn : register(t0);

// Output voxel grid (write)
RWStructuredBuffer<uint> VoxelGridOut : register(u0);

// Material properties lookup (helpers for physics)
// Must match IsMovable in CS_GravityChunk.hlsl
bool IsMovable(uint material) {
    return material == MAT_SAND || material == MAT_DIRT || material == MAT_WATER ||
           material == MAT_LAVA || material == MAT_OIL ||
           material == MAT_SMOKE || material == MAT_FIRE || material == MAT_ACID ||
           material == MAT_HONEY || material == MAT_CONCRETE || material == MAT_GUNPOWDER ||
           material == MAT_STEAM;
}

bool IsEmpty(uint material) {
    return material == MAT_AIR;
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

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= gridSizeX || DTid.y >= gridSizeY || DTid.z >= gridSizeZ) {
        return;
    }

    int3 pos = int3(DTid);
    uint currentVoxel = GetVoxelSafe(pos);
    uint material = GetMaterial(currentVoxel);

    // Default: copy voxel unchanged
    uint outputVoxel = currentVoxel;

    // For AIR voxels: check if there's a painted voxel in the WRITE buffer
    // If WRITE has non-AIR, preserve it (it was painted). Otherwise write AIR.
    // This is handled by using InterlockedCompareExchange - only write if position is "empty"
    if (material == MAT_AIR) {
        // Write AIR to initialize the WRITE buffer at this position
        // But use atomic operation to not overwrite painted voxels
        uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
        uint idx = LinearIndex3D(DTid, gridSize);

        // Try to write AIR only if position currently contains garbage/uninitialized
        // Actually, we can't detect this. Let's just write AIR - brush will paint after physics.
        SetVoxel(DTid, PackVoxel(MAT_AIR, 0, 0, 0));
        return;
    }

    // Bedrock stays in place
    if (material == MAT_BEDROCK) {
        SetVoxel(DTid, outputVoxel);
        return;
    }

    // Skip static/non-movable materials
    if (!IsMovable(material)) {
        SetVoxel(DTid, outputVoxel);
        return;
    }

    // === FALLING SAND PHYSICS ===

    // Check voxel below
    int3 belowPos = pos + int3(0, -1, 0);

    if (belowPos.y >= 0) {
        uint belowVoxel = GetVoxelSafe(belowPos);
        uint belowMaterial = GetMaterial(belowVoxel);

        // Can fall straight down?
        if (IsEmpty(belowMaterial)) {
            // Try to claim the destination voxel atomically
            uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
            uint belowIdx = LinearIndex3D(uint3(belowPos), gridSize);

            uint originalValue;
            InterlockedCompareExchange(VoxelGridOut[belowIdx], PackVoxel(MAT_AIR, 0, 0, 0),
                                       PackVoxel(material, GetVariant(currentVoxel), 0, 0),
                                       originalValue);

            // Only clear current position if we successfully claimed the destination
            if (originalValue == PackVoxel(MAT_AIR, 0, 0, 0)) {
                SetVoxel(DTid, PackVoxel(MAT_AIR, 0, 0, 0));
                return;
            }
        }

        // Sand: try diagonal down (slide)
        if (material == MAT_SAND) {
            // Random direction based on position + frame
            uint rng = PCGHash(DTid.x + DTid.y * 1000 + DTid.z * 1000000 + frameIndex);
            int dir = (rng & 1) ? 1 : -1;

            int3 diagPos1 = pos + int3(dir, -1, 0);
            int3 diagPos2 = pos + int3(-dir, -1, 0);

            // Try first diagonal
            if (diagPos1.x >= 0 && diagPos1.x < (int)gridSizeX) {
                uint diagVoxel = GetVoxelSafe(diagPos1);
                if (IsEmpty(GetMaterial(diagVoxel))) {
                    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
                    uint diagIdx = LinearIndex3D(uint3(diagPos1), gridSize);

                    uint originalValue;
                    InterlockedCompareExchange(VoxelGridOut[diagIdx], PackVoxel(MAT_AIR, 0, 0, 0),
                                               PackVoxel(material, GetVariant(currentVoxel), 0, 0),
                                               originalValue);

                    if (originalValue == PackVoxel(MAT_AIR, 0, 0, 0)) {
                        SetVoxel(DTid, PackVoxel(MAT_AIR, 0, 0, 0));
                        return;
                    }
                }
            }

            // Try second diagonal
            if (diagPos2.x >= 0 && diagPos2.x < (int)gridSizeX) {
                uint diagVoxel = GetVoxelSafe(diagPos2);
                if (IsEmpty(GetMaterial(diagVoxel))) {
                    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
                    uint diagIdx = LinearIndex3D(uint3(diagPos2), gridSize);

                    uint originalValue;
                    InterlockedCompareExchange(VoxelGridOut[diagIdx], PackVoxel(MAT_AIR, 0, 0, 0),
                                               PackVoxel(material, GetVariant(currentVoxel), 0, 0),
                                               originalValue);

                    if (originalValue == PackVoxel(MAT_AIR, 0, 0, 0)) {
                        SetVoxel(DTid, PackVoxel(MAT_AIR, 0, 0, 0));
                        return;
                    }
                }
            }
        }

        // Liquids: horizontal spread (simplified)
        if (IsLiquid(currentVoxel)) {
            uint rng = PCGHash(DTid.x + DTid.y * 1000 + DTid.z * 1000000 + frameIndex);
            int dir = (rng & 1) ? 1 : -1;

            int3 sidePos = pos + int3(dir, 0, 0);

            if (sidePos.x >= 0 && sidePos.x < (int)gridSizeX) {
                uint sideVoxel = GetVoxelSafe(sidePos);
                if (IsEmpty(GetMaterial(sideVoxel))) {
                    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
                    uint sideIdx = LinearIndex3D(uint3(sidePos), gridSize);

                    uint originalValue;
                    InterlockedCompareExchange(VoxelGridOut[sideIdx], PackVoxel(MAT_AIR, 0, 0, 0),
                                               PackVoxel(material, GetVariant(currentVoxel), 0, 0),
                                               originalValue);

                    if (originalValue == PackVoxel(MAT_AIR, 0, 0, 0)) {
                        SetVoxel(DTid, PackVoxel(MAT_AIR, 0, 0, 0));
                        return;
                    }
                }
            }
        }
    }

    // No movement possible - stay put
    SetVoxel(DTid, outputVoxel);
}
