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
    return material == MAT_SAND || material == MAT_DIRT || material == MAT_WATER || material == MAT_LAVA || material == MAT_OIL ||
           material == MAT_SMOKE || material == MAT_FIRE || material == MAT_ACID || material == MAT_HONEY ||
           material == MAT_CONCRETE || material == MAT_GUNPOWDER || material == MAT_STEAM;
}

bool IsEmpty(uint material) {
    return material == MAT_AIR;
}

bool IsFlammable(uint material) {
    return material == MAT_WOOD || material == MAT_OIL || material == MAT_GUNPOWDER;
}

bool IsDissolvable(uint material) {
    // Materials that acid can dissolve
    return material == MAT_STONE || material == MAT_DIRT || material == MAT_WOOD ||
           material == MAT_SAND || material == MAT_ICE || material == MAT_CONCRETE;
}

bool IsLiquidOrGas(uint material) {
    return material == MAT_WATER || material == MAT_LAVA || material == MAT_OIL ||
           material == MAT_ACID || material == MAT_HONEY || material == MAT_CONCRETE ||
           material == MAT_SMOKE || material == MAT_STEAM;
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

// Count liquid depth at a given XZ position
// Returns the Y-height of the liquid surface (or 0 if no liquid)
// Works for all liquid materials
int GetLiquidColumnHeight(int3 pos) {
    // Start from current position and scan upward to find liquid surface
    int surfaceY = pos.y;

    // Scan up to find the top of liquid column
    for (int checkY = pos.y; checkY < (int)gridSizeY; checkY++) {
        int3 checkPos = int3(pos.x, checkY, pos.z);
        uint voxel = GetVoxelSafe(checkPos);
        uint mat = GetMaterial(voxel);

        if (mat == MAT_WATER || mat == MAT_OIL || mat == MAT_LAVA ||
            mat == MAT_ACID || mat == MAT_HONEY || mat == MAT_CONCRETE) {
            surfaceY = checkY;  // Update surface height
        } else if (!IsEmpty(mat)) {
            break;  // Hit solid - stop scanning
        }
    }

    return surfaceY;
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

                // Process static/non-movable materials
                if (!IsMovable(material)) {
                    // === ICE MELTING ===
                    // Ice melts when near fire or lava
                    if (material == MAT_ICE) {
                        int3 neighbors[6];
                        neighbors[0] = pos + int3(1, 0, 0);
                        neighbors[1] = pos + int3(-1, 0, 0);
                        neighbors[2] = pos + int3(0, 1, 0);
                        neighbors[3] = pos + int3(0, -1, 0);
                        neighbors[4] = pos + int3(0, 0, 1);
                        neighbors[5] = pos + int3(0, 0, -1);

                        for (int i = 0; i < 6; i++) {
                            uint neighborVoxel = GetVoxelSafe(neighbors[i]);
                            uint neighborMat = GetMaterial(neighborVoxel);

                            // Ice melts near heat sources
                            if (neighborMat == MAT_FIRE || neighborMat == MAT_LAVA) {
                                // Ice melts into water (non-static)
                                SetVoxel(uint3(pos), PackVoxel(MAT_WATER, GetVariant(currentVoxel), 0, 0));
                                continue;  // Skip to next voxel
                            }
                        }
                    }

                    SetVoxel(uint3(pos), outputVoxel);
                    continue;
                }

                // =====================================================================
                // SMOKE PHYSICS - Billows upward and dissipates gradually
                // =====================================================================
                if (material == MAT_SMOKE) {
                    uint currentLife = GetLife(currentVoxel);
                    uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);

                    // Dissipate smoke based on life
                    if (currentLife == 0) {
                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                        continue;
                    }

                    // Slow dissipation - only decrement every ~4 frames for billowing effect
                    uint newLife = currentLife;
                    if ((rng & 0x3) == 0) {
                        newLife = currentLife - 1;
                    }
                    uint newState = (GetState(currentVoxel) & ~STATE_LIFE_MASK) | (newLife & STATE_LIFE_MASK);

                    // Young smoke (high life) rises vigorously
                    // Old smoke (low life) spreads horizontally and billows
                    bool moved = false;

                    if (currentLife > 8) {
                        // === VIGOROUS RISING (young smoke) ===
                        int3 abovePos = pos + int3(0, 1, 0);
                        if (abovePos.y < (int)gridSizeY) {
                            uint aboveVoxel = GetVoxelSafe(abovePos);
                            if (IsEmpty(GetMaterial(aboveVoxel))) {
                                SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                SetVoxel(uint3(abovePos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                                moved = true;
                            }
                        }
                    }

                    // === BILLOWING - spreads in all directions ===
                    // Both young (if blocked) and old smoke billow
                    if (!moved) {
                        // Only billow if we have enough life remaining (prevents infinite spread)
                        if (newLife > 3) {
                            int3 expansions[8];
                            expansions[0] = pos + int3(1, 0, 0);
                            expansions[1] = pos + int3(-1, 0, 0);
                            expansions[2] = pos + int3(0, 0, 1);
                            expansions[3] = pos + int3(0, 0, -1);
                            expansions[4] = pos + int3(0, 1, 0);  // Still try up
                            expansions[5] = pos + int3(1, 1, 0);  // Diagonal up
                            expansions[6] = pos + int3(-1, 1, 0); // Diagonal up
                            expansions[7] = pos + int3(0, 1, 1);  // Diagonal up

                            int startIdx = (rng >> 4) & 0x7;
                            // Only try one random direction per frame (prevents tsunami)
                            int idx = startIdx;
                            int3 expandPos = expansions[idx];
                            uint expandVoxel = GetVoxelSafe(expandPos);
                            if (IsEmpty(GetMaterial(expandVoxel))) {
                                // Clone smoke with REDUCED life (prevents infinite spread)
                                uint cloneLife = newLife - 2;  // Significant life reduction
                                uint cloneState = (newState & ~STATE_LIFE_MASK) | (cloneLife & STATE_LIFE_MASK);
                                SetVoxel(uint3(expandPos), PackVoxel(material, (rng >> 8) & 0xFF, 0, cloneState));
                            }
                        }
                    }

                    if (moved) {
                        continue;  // Already moved smoke upward
                    }

                    SetVoxel(uint3(pos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                    continue;
                }

                // =====================================================================
                // STEAM PHYSICS - Like smoke but dissipates faster
                // =====================================================================
                if (material == MAT_STEAM) {
                    uint currentLife = GetLife(currentVoxel);
                    if (currentLife == 0) {
                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                        continue;
                    }

                    uint newLife = currentLife - 1;
                    uint newState = (GetState(currentVoxel) & ~STATE_LIFE_MASK) | (newLife & STATE_LIFE_MASK);
                    uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                    bool moved = false;

                    int3 abovePos = pos + int3(0, 1, 0);
                    if (abovePos.y < (int)gridSizeY) {
                        uint aboveVoxel = GetVoxelSafe(abovePos);
                        if (IsEmpty(GetMaterial(aboveVoxel))) {
                            SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                            SetVoxel(uint3(abovePos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                            continue;
                        }
                    }

                    SetVoxel(uint3(pos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                    continue;
                }

                // =====================================================================
                // GUNPOWDER PHYSICS - Explodes when near fire/lava
                // =====================================================================
                if (material == MAT_GUNPOWDER) {
                    int3 neighbors[6];
                    neighbors[0] = pos + int3(1, 0, 0);
                    neighbors[1] = pos + int3(-1, 0, 0);
                    neighbors[2] = pos + int3(0, 1, 0);
                    neighbors[3] = pos + int3(0, -1, 0);
                    neighbors[4] = pos + int3(0, 0, 1);
                    neighbors[5] = pos + int3(0, 0, -1);

                    for (int i = 0; i < 6; i++) {
                        uint neighborVoxel = GetVoxelSafe(neighbors[i]);
                        uint neighborMat = GetMaterial(neighborVoxel);

                        if (neighborMat == MAT_FIRE || neighborMat == MAT_LAVA) {
                            // EXPLODE! Create blast wave of fire for chain reactions
                            int explosionRadius = 5;
                            for (int ex = -explosionRadius; ex <= explosionRadius; ex++) {
                                for (int ey = -explosionRadius; ey <= explosionRadius; ey++) {
                                    for (int ez = -explosionRadius; ez <= explosionRadius; ez++) {
                                        float dist = sqrt(float(ex*ex + ey*ey + ez*ez));
                                        if (dist <= explosionRadius) {
                                            int3 explodePos = pos + int3(ex, ey, ez);
                                            uint explodeVoxel = GetVoxelSafe(explodePos);
                                            uint explodeMat = GetMaterial(explodeVoxel);

                                            if (explodeMat != MAT_BEDROCK) {
                                                // Inner radius (3 blocks) - complete destruction + fire
                                                if (dist <= 3.0f) {
                                                    // Spawn temporary fire to trigger chain reactions
                                                    SetVoxel(uint3(explodePos), PackVoxel(MAT_FIRE, 0, 0, 3));
                                                }
                                                // Outer radius (3-5 blocks) - just clear
                                                else {
                                                    SetVoxel(uint3(explodePos), PackVoxel(MAT_AIR, 0, 0, 0));
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            continue;
                        }
                    }
                    // Gunpowder falls like sand in the falling physics section
                }

                // =====================================================================
                // FIRE PHYSICS - Handle fire BEFORE falling physics (fire stays put!)
                // =====================================================================
                if (material == MAT_FIRE) {
                    uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);

                    // Decrement life counter
                    uint currentLife = GetLife(currentVoxel);
                    if (currentLife == 0) {
                        // Fire burns out → becomes smoke
                        SetVoxel(uint3(pos), PackVoxel(MAT_SMOKE, GetVariant(currentVoxel), 0, 15));
                        continue;
                    }

                    uint newLife = currentLife - 1;
                    uint newState = (GetState(currentVoxel) & 0xF0) | (newLife & 0x0F);

                    // Try to spread to adjacent flammable voxels
                    int3 neighbors[6];
                    neighbors[0] = pos + int3(1, 0, 0);
                    neighbors[1] = pos + int3(-1, 0, 0);
                    neighbors[2] = pos + int3(0, 1, 0);
                    neighbors[3] = pos + int3(0, -1, 0);
                    neighbors[4] = pos + int3(0, 0, 1);
                    neighbors[5] = pos + int3(0, 0, -1);

                    for (int i = 0; i < 6; i++) {
                        int3 neighborPos = neighbors[i];
                        uint neighborVoxel = GetVoxelSafe(neighborPos);
                        uint neighborMat = GetMaterial(neighborVoxel);

                        // Water extinguishes fire → creates steam
                        if (neighborMat == MAT_WATER) {
                            SetVoxel(uint3(pos), PackVoxel(MAT_STEAM, GetVariant(currentVoxel), 0, 12));
                            SetVoxel(uint3(neighborPos), PackVoxel(MAT_STEAM, GetVariant(neighborVoxel), 0, 12));
                            continue;
                        }

                        if (IsFlammable(neighborMat)) {
                            // 1 in 4 chance to ignite
                            uint spreadChance = (rng >> (i * 2)) & 0x3;
                            if (spreadChance == 0) {
                                // Ignite the flammable material
                                // Wood and oil burn for 60 frames
                                SetVoxel(uint3(neighborPos), PackVoxel(MAT_FIRE, GetVariant(neighborVoxel), 0, 60));
                            }
                        }
                    }

                    // Randomly spawn smoke above fire
                    int3 abovePos = pos + int3(0, 1, 0);
                    uint aboveVoxel = GetVoxelSafe(abovePos);
                    if (IsEmpty(GetMaterial(aboveVoxel))) {
                        uint smokeChance = rng & 0x7; // 1 in 8 chance
                        if (smokeChance == 0) {
                            SetVoxel(uint3(abovePos), PackVoxel(MAT_SMOKE, (rng >> 8) & 0xFF, 0, 15));
                        }
                    }

                    // === NAPALM DROPLETS - Drop burning oil that sticks and ignites ===
                    // Only spawn napalm from fires with high life (sustained fires)
                    if (currentLife > 30) {
                        // 1 in 16 chance to spawn a napalm droplet
                        uint napalmChance = (rng >> 10) & 0xF;
                        if (napalmChance == 0) {
                            int3 belowPos = pos + int3(0, -1, 0);
                            if (belowPos.y >= 0) {
                                uint belowVoxel = GetVoxelSafe(belowPos);
                                uint belowMat = GetMaterial(belowVoxel);

                                // Spawn burning oil droplet that will fall
                                if (IsEmpty(belowMat)) {
                                    // Create oil droplet that's already ignited
                                    uint napalmState = STATE_IS_IGNITED;  // Mark as ignited oil
                                    SetVoxel(uint3(belowPos), PackVoxel(MAT_OIL, (rng >> 16) & 0xFF, 0, napalmState));
                                }
                                // If flammable material below, ignite it immediately
                                else if (IsFlammable(belowMat)) {
                                    SetVoxel(uint3(belowPos), PackVoxel(MAT_FIRE, GetVariant(belowVoxel), 0, 60));
                                }
                            }
                        }
                    }

                    // Update fire with decremented life
                    SetVoxel(uint3(pos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
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

                    // Liquids: comprehensive 4-directional flow with unbiased spreading
                    if (IsLiquid(currentVoxel)) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);

                        // === CHECK FOR WATER INTERACTIONS ===
                        if (material == MAT_WATER) {
                            // Check all 6 adjacent voxels for interactions
                            int3 neighbors[6];
                            neighbors[0] = pos + int3(1, 0, 0);
                            neighbors[1] = pos + int3(-1, 0, 0);
                            neighbors[2] = pos + int3(0, 1, 0);
                            neighbors[3] = pos + int3(0, -1, 0);
                            neighbors[4] = pos + int3(0, 0, 1);
                            neighbors[5] = pos + int3(0, 0, -1);

                            bool hasIceNeighbor = false;
                            int iceNeighborCount = 0;

                            for (int i = 0; i < 6; i++) {
                                uint neighborVoxel = GetVoxelSafe(neighbors[i]);
                                uint neighborMat = GetMaterial(neighborVoxel);

                                // Water touching lava turns to stone and creates steam burst
                                if (neighborMat == MAT_LAVA) {
                                    SetVoxel(uint3(pos), PackVoxel(MAT_STONE, GetVariant(currentVoxel), 0, STATE_IS_STATIC));
                                    // Create steam burst above (more dramatic effect)
                                    int3 abovePos = pos + int3(0, 1, 0);
                                    if (IsEmpty(GetMaterial(GetVoxelSafe(abovePos)))) {
                                        SetVoxel(uint3(abovePos), PackVoxel(MAT_STEAM, GetVariant(currentVoxel), 0, 15));
                                    }
                                    // Additional steam in random adjacent directions
                                    uint rng = PCGHash(pos.x * 7 + pos.y * 13 + pos.z * 31 + frameIndex);
                                    if ((rng & 0x1) == 0) {
                                        int3 steamPos = pos + int3((rng & 0x2) ? 1 : -1, 1, 0);
                                        if (IsEmpty(GetMaterial(GetVoxelSafe(steamPos)))) {
                                            SetVoxel(uint3(steamPos), PackVoxel(MAT_STEAM, (rng >> 8) & 0xFF, 0, 12));
                                        }
                                    }
                                    continue;  // Skip to next voxel in outer loop
                                }
                                // Water touching fire → both become steam
                                else if (neighborMat == MAT_FIRE) {
                                    SetVoxel(uint3(pos), PackVoxel(MAT_STEAM, GetVariant(currentVoxel), 0, 12));  // Water becomes steam
                                    SetVoxel(uint3(neighbors[i]), PackVoxel(MAT_STEAM, GetVariant(neighborVoxel), 0, 12));  // Fire becomes steam
                                    continue;  // Skip to next voxel
                                }
                                // Count ice neighbors for freezing
                                else if (neighborMat == MAT_ICE) {
                                    hasIceNeighbor = true;
                                    iceNeighborCount++;
                                }
                            }

                            // Water freezes if surrounded by ice (3+ ice neighbors)
                            if (hasIceNeighbor && iceNeighborCount >= 3) {
                                SetVoxel(uint3(pos), PackVoxel(MAT_ICE, GetVariant(currentVoxel), 0, STATE_IS_STATIC));
                                continue;  // Skip to next voxel
                            }
                        }

                        // === PRIORITY 2: DIAGONAL DOWN FLOW (slope flow) ===
                        // Flow diagonally down slopes

                        uint belowVoxel = GetVoxelSafe(belowPos);
                        uint belowMaterial = GetMaterial(belowVoxel);

                        bool moved = false;  // Track if voxel moved this frame

                        // Try diagonal flow if on solid ground OR if surrounded by water
                        // This allows water to flow down slopes AND escape from pools
                        if (!IsEmpty(belowMaterial)) {
                            // Try all 4 diagonal directions in random order
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);   // +X diagonal
                            diagonals[1] = pos + int3(-1, -1, 0);  // -X diagonal
                            diagonals[2] = pos + int3(0, -1, 1);   // +Z diagonal
                            diagonals[3] = pos + int3(0, -1, -1);  // -Z diagonal

                            // Shuffle based on RNG to avoid bias
                            int startIdx = (rng >> 2) & 0x3;  // Random start point (0-3)

                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 diagPos = diagonals[idx];
                                uint diagVoxel = GetVoxelSafe(diagPos);
                                if (IsEmpty(GetMaterial(diagVoxel))) {
                                    outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(diagPos), outputVoxel);
                                    moved = true;
                                    break;  // Exit loop immediately after moving
                                }
                            }
                        }

                        // === PRIORITY 3: HORIZONTAL SPREAD (puddle leveling) ===
                        // Only spread if destination water column is LOWER (pressure-based flow)

                        if (!moved) {  // Only spread horizontally if we didn't move diagonally
                            // Get current water column height
                            int myHeight = GetLiquidColumnHeight(pos);

                            // Shuffle to try directions in random order
                            int startIdx = (rng >> 4) & 0x3;

                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);   // +X
                            horizontals[1] = pos + int3(-1, 0, 0);  // -X
                            horizontals[2] = pos + int3(0, 0, 1);   // +Z
                            horizontals[3] = pos + int3(0, 0, -1);  // -Z

                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 sidePos = horizontals[idx];
                                uint sideVoxel = GetVoxelSafe(sidePos);

                                if (IsEmpty(GetMaterial(sideVoxel))) {
                                    // Check if neighbor water column is lower than ours
                                    int neighborHeight = GetLiquidColumnHeight(sidePos);

                                    // Only spread if we're leveling out (neighbor is at least 1 block lower)
                                    // The "1" prevents single-voxel jittering while allowing natural leveling
                                    if (neighborHeight < myHeight - 1) {
                                        outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                        SetVoxel(uint3(sidePos), outputVoxel);
                                        moved = true;
                                        break;
                                    }
                                }
                            }
                        }

                        // If voxel moved, we're done - skip the "stay put" write below
                        if (moved) {
                            continue;  // Now this correctly skips to next voxel
                        }
                    }

                    // Lava: slower liquid physics (spreads 1 direction at a time)
                    if (material == MAT_LAVA) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);

                        // === LAVA INTERACTIONS ===
                        // Check all 6 adjacent voxels for interactions
                        int3 neighbors[6];
                        neighbors[0] = pos + int3(1, 0, 0);
                        neighbors[1] = pos + int3(-1, 0, 0);
                        neighbors[2] = pos + int3(0, 1, 0);
                        neighbors[3] = pos + int3(0, -1, 0);
                        neighbors[4] = pos + int3(0, 0, 1);
                        neighbors[5] = pos + int3(0, 0, -1);

                        for (int i = 0; i < 6; i++) {
                            int3 neighborPos = neighbors[i];
                            uint neighborVoxel = GetVoxelSafe(neighborPos);
                            uint neighborMat = GetMaterial(neighborVoxel);

                            // Lava + Water → turn water to stone
                            if (neighborMat == MAT_WATER) {
                                SetVoxel(uint3(neighborPos), PackVoxel(MAT_STONE, GetVariant(neighborVoxel), 0, STATE_IS_STATIC));
                            }
                            // Lava ignites flammable materials (wood, oil)
                            else if (IsFlammable(neighborMat)) {
                                // 1 in 2 chance to ignite (faster than fire spreading)
                                uint igniteChance = (rng >> (i * 2)) & 0x1;
                                if (igniteChance == 0) {
                                    SetVoxel(uint3(neighborPos), PackVoxel(MAT_FIRE, GetVariant(neighborVoxel), 0, 60));
                                }
                            }
                        }

                        uint belowVoxel = GetVoxelSafe(belowPos);
                        uint belowMaterial = GetMaterial(belowVoxel);

                        bool moved = false;

                        // Diagonal flow when on solid ground
                        if (!IsEmpty(belowMaterial)) {
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);
                            diagonals[1] = pos + int3(-1, -1, 0);
                            diagonals[2] = pos + int3(0, -1, 1);
                            diagonals[3] = pos + int3(0, -1, -1);

                            int startIdx = (rng >> 2) & 0x3;

                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 diagPos = diagonals[idx];
                                uint diagVoxel = GetVoxelSafe(diagPos);
                                if (IsEmpty(GetMaterial(diagVoxel))) {
                                    outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(diagPos), outputVoxel);
                                    moved = true;
                                    break;
                                }
                            }
                        }

                        // Horizontal spread - SLOWER than water but still pressure-based
                        // Only try 2 random directions instead of all 4 (compromise between speed and pooling)
                        if (!moved) {
                            int myHeight = GetLiquidColumnHeight(pos);

                            // Pick 2 random directions to try (slower than water's 4)
                            int startIdx = (rng >> 4) & 0x3;
                            int tryCount = 2;  // Only try 2 directions (slower than water)

                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);
                            horizontals[1] = pos + int3(-1, 0, 0);
                            horizontals[2] = pos + int3(0, 0, 1);
                            horizontals[3] = pos + int3(0, 0, -1);

                            for (int i = 0; i < tryCount; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 sidePos = horizontals[idx];
                                uint sideVoxel = GetVoxelSafe(sidePos);

                                if (IsEmpty(GetMaterial(sideVoxel))) {
                                    int neighborHeight = GetLiquidColumnHeight(sidePos);

                                    // Lava spreads to lower areas (pressure-based like water)
                                    // Use threshold of 2 blocks instead of 1 for slower pooling
                                    if (neighborHeight < myHeight - 2) {
                                        outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                        SetVoxel(uint3(sidePos), outputVoxel);
                                        moved = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (moved) {
                            continue;
                        }
                    }

                    // Oil: like water but floats on water
                    if (material == MAT_OIL) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                        uint currentState = GetState(currentVoxel);
                        bool isIgnited = (currentState & STATE_IS_IGNITED) != 0;

                        // === NAPALM BEHAVIOR ===
                        // If oil is ignited (napalm droplet), check if it should burst into fire
                        if (isIgnited) {
                            uint belowVoxel = GetVoxelSafe(belowPos);
                            uint belowMaterial = GetMaterial(belowVoxel);

                            // Turn into fire when hitting ground or any non-air material
                            if (!IsEmpty(belowMaterial)) {
                                SetVoxel(uint3(pos), PackVoxel(MAT_FIRE, GetVariant(currentVoxel), 0, 45));
                                // Also ignite the surface below if flammable
                                if (IsFlammable(belowMaterial)) {
                                    SetVoxel(uint3(belowPos), PackVoxel(MAT_FIRE, GetVariant(belowVoxel), 0, 60));
                                }
                                continue;
                            }
                            // Keep falling as burning oil (napalm droplet will ignite on landing)
                        }

                        // === OIL IGNITION ===
                        // Oil ignites when touching fire or lava
                        int3 neighbors[6];
                        neighbors[0] = pos + int3(1, 0, 0);
                        neighbors[1] = pos + int3(-1, 0, 0);
                        neighbors[2] = pos + int3(0, 1, 0);
                        neighbors[3] = pos + int3(0, -1, 0);
                        neighbors[4] = pos + int3(0, 0, 1);
                        neighbors[5] = pos + int3(0, 0, -1);

                        for (int i = 0; i < 6; i++) {
                            uint neighborVoxel = GetVoxelSafe(neighbors[i]);
                            uint neighborMat = GetMaterial(neighborVoxel);

                            // Oil catches fire from fire or lava
                            if (neighborMat == MAT_FIRE || neighborMat == MAT_LAVA) {
                                // Instant ignition (oil is highly flammable)
                                SetVoxel(uint3(pos), PackVoxel(MAT_FIRE, GetVariant(currentVoxel), 0, 60));
                                continue;  // Skip to next voxel in outer loop
                            }
                        }

                        uint belowVoxel = GetVoxelSafe(belowPos);
                        uint belowMaterial = GetMaterial(belowVoxel);

                        bool moved = false;

                        // Oil floats on water - swap positions if oil is above water
                        if (belowMaterial == MAT_WATER) {
                            // Swap: oil goes up, water goes down
                            outputVoxel = PackVoxel(MAT_WATER, GetVariant(belowVoxel), 0, 0);  // Water moves up
                            SetVoxel(uint3(pos), outputVoxel);
                            SetVoxel(uint3(belowPos), PackVoxel(MAT_OIL, GetVariant(currentVoxel), 0, 0));  // Oil stays below
                            continue;  // Done processing this voxel
                        }

                        // Diagonal flow (only if not on water)
                        if (!moved && !IsEmpty(belowMaterial)) {
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);
                            diagonals[1] = pos + int3(-1, -1, 0);
                            diagonals[2] = pos + int3(0, -1, 1);
                            diagonals[3] = pos + int3(0, -1, -1);

                            int startIdx = (rng >> 2) & 0x3;

                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 diagPos = diagonals[idx];
                                uint diagVoxel = GetVoxelSafe(diagPos);
                                if (IsEmpty(GetMaterial(diagVoxel))) {
                                    outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(diagPos), outputVoxel);
                                    moved = true;
                                    break;
                                }
                            }
                        }

                        // Horizontal spread (same as water - pressure-based)
                        if (!moved) {
                            int myHeight = GetLiquidColumnHeight(pos);
                            int startIdx = (rng >> 4) & 0x3;

                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);
                            horizontals[1] = pos + int3(-1, 0, 0);
                            horizontals[2] = pos + int3(0, 0, 1);
                            horizontals[3] = pos + int3(0, 0, -1);

                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 sidePos = horizontals[idx];
                                uint sideVoxel = GetVoxelSafe(sidePos);

                                if (IsEmpty(GetMaterial(sideVoxel))) {
                                    int neighborHeight = GetLiquidColumnHeight(sidePos);

                                    if (neighborHeight < myHeight - 1) {
                                        outputVoxel = PackVoxel(material, GetVariant(currentVoxel), 0, 0);
                                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                        SetVoxel(uint3(sidePos), outputVoxel);
                                        moved = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (moved) {
                            continue;
                        }
                    }

                    // =====================================================================
                    // ACID PHYSICS - Corrosive liquid
                    // =====================================================================
                    if (material == MAT_ACID) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);

                        // Dissolve adjacent materials
                        int3 neighbors[6];
                        neighbors[0] = pos + int3(1, 0, 0);
                        neighbors[1] = pos + int3(-1, 0, 0);
                        neighbors[2] = pos + int3(0, 1, 0);
                        neighbors[3] = pos + int3(0, -1, 0);
                        neighbors[4] = pos + int3(0, 0, 1);
                        neighbors[5] = pos + int3(0, 0, -1);

                        for (int i = 0; i < 6; i++) {
                            uint neighborVoxel = GetVoxelSafe(neighbors[i]);
                            uint neighborMat = GetMaterial(neighborVoxel);

                            // Acid + Water → Neutralization (becomes dirt)
                            if (neighborMat == MAT_WATER) {
                                SetVoxel(uint3(pos), PackVoxel(MAT_DIRT, GetVariant(currentVoxel), 0, STATE_IS_STATIC));
                                SetVoxel(uint3(neighbors[i]), PackVoxel(MAT_DIRT, GetVariant(neighborVoxel), 0, STATE_IS_STATIC));
                                continue;  // Acid neutralized, skip to next voxel
                            }
                            // Acid dissolves certain materials
                            else if (IsDissolvable(neighborMat)) {
                                uint dissolveChance = (rng >> (i * 2)) & 0xF;
                                if (dissolveChance == 0) {
                                    SetVoxel(uint3(neighbors[i]), PackVoxel(MAT_AIR, 0, 0, 0));
                                }
                            }
                        }

                        // Flow like water
                        uint belowVoxel = GetVoxelSafe(belowPos);
                        uint belowMaterial = GetMaterial(belowVoxel);
                        bool moved = false;

                        if (!IsEmpty(belowMaterial)) {
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);
                            diagonals[1] = pos + int3(-1, -1, 0);
                            diagonals[2] = pos + int3(0, -1, 1);
                            diagonals[3] = pos + int3(0, -1, -1);
                            int startIdx = (rng >> 2) & 0x3;
                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 diagPos = diagonals[idx];
                                if (IsEmpty(GetMaterial(GetVoxelSafe(diagPos)))) {
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(diagPos), PackVoxel(material, GetVariant(currentVoxel), 0, 0));
                                    moved = true;
                                    break;
                                }
                            }
                        }

                        if (!moved) {
                            int myHeight = GetLiquidColumnHeight(pos);
                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);
                            horizontals[1] = pos + int3(-1, 0, 0);
                            horizontals[2] = pos + int3(0, 0, 1);
                            horizontals[3] = pos + int3(0, 0, -1);
                            int startIdx = (rng >> 4) & 0x3;
                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                int3 sidePos = horizontals[idx];
                                if (IsEmpty(GetMaterial(GetVoxelSafe(sidePos)))) {
                                    int neighborHeight = GetLiquidColumnHeight(sidePos);
                                    if (neighborHeight < myHeight - 1) {
                                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                        SetVoxel(uint3(sidePos), PackVoxel(material, GetVariant(currentVoxel), 0, 0));
                                        moved = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (moved) continue;
                    }

                    // =====================================================================
                    // HONEY PHYSICS - Super viscous liquid
                    // =====================================================================
                    if (material == MAT_HONEY) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                        if ((rng & 0x3) != 0) continue;  // Only move 1 in 4 frames

                        uint belowVoxel = GetVoxelSafe(belowPos);
                        bool moved = false;

                        if (!IsEmpty(GetMaterial(belowVoxel))) {
                            int idx = (rng >> 4) & 0x3;
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);
                            diagonals[1] = pos + int3(-1, -1, 0);
                            diagonals[2] = pos + int3(0, -1, 1);
                            diagonals[3] = pos + int3(0, -1, -1);
                            if (IsEmpty(GetMaterial(GetVoxelSafe(diagonals[idx])))) {
                                SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                SetVoxel(uint3(diagonals[idx]), PackVoxel(material, GetVariant(currentVoxel), 0, 0));
                                moved = true;
                            }
                        }

                        if (!moved) {
                            int myHeight = GetLiquidColumnHeight(pos);
                            int idx = (rng >> 6) & 0x3;
                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);
                            horizontals[1] = pos + int3(-1, 0, 0);
                            horizontals[2] = pos + int3(0, 0, 1);
                            horizontals[3] = pos + int3(0, 0, -1);
                            if (IsEmpty(GetMaterial(GetVoxelSafe(horizontals[idx])))) {
                                int neighborHeight = GetLiquidColumnHeight(horizontals[idx]);
                                if (neighborHeight < myHeight - 3) {
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(horizontals[idx]), PackVoxel(material, GetVariant(currentVoxel), 0, 0));
                                    moved = true;
                                }
                            }
                        }

                        if (moved) continue;
                    }

                    // =====================================================================
                    // CONCRETE PHYSICS - Hardens into stone
                    // =====================================================================
                    if (material == MAT_CONCRETE) {
                        uint rng = PCGHash(pos.x + pos.y * 1000 + pos.z * 1000000 + frameIndex);
                        uint currentLife = GetLife(currentVoxel);

                        if (currentLife >= 15) {
                            SetVoxel(uint3(pos), PackVoxel(MAT_STONE, GetVariant(currentVoxel), 0, STATE_IS_STATIC));
                            continue;
                        }

                        uint newLife = currentLife + 1;
                        uint newState = (GetState(currentVoxel) & ~STATE_LIFE_MASK) | (newLife & STATE_LIFE_MASK);
                        uint belowVoxel = GetVoxelSafe(belowPos);
                        bool moved = false;

                        if (!IsEmpty(GetMaterial(belowVoxel))) {
                            int3 diagonals[4];
                            diagonals[0] = pos + int3(1, -1, 0);
                            diagonals[1] = pos + int3(-1, -1, 0);
                            diagonals[2] = pos + int3(0, -1, 1);
                            diagonals[3] = pos + int3(0, -1, -1);
                            int startIdx = (rng >> 2) & 0x3;
                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                if (IsEmpty(GetMaterial(GetVoxelSafe(diagonals[idx])))) {
                                    SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                    SetVoxel(uint3(diagonals[idx]), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                                    moved = true;
                                    break;
                                }
                            }
                        }

                        if (!moved) {
                            int myHeight = GetLiquidColumnHeight(pos);
                            int3 horizontals[4];
                            horizontals[0] = pos + int3(1, 0, 0);
                            horizontals[1] = pos + int3(-1, 0, 0);
                            horizontals[2] = pos + int3(0, 0, 1);
                            horizontals[3] = pos + int3(0, 0, -1);
                            int startIdx = (rng >> 4) & 0x3;
                            for (int i = 0; i < 4; i++) {
                                int idx = (startIdx + i) % 4;
                                if (IsEmpty(GetMaterial(GetVoxelSafe(horizontals[idx])))) {
                                    int neighborHeight = GetLiquidColumnHeight(horizontals[idx]);
                                    if (neighborHeight < myHeight - 1) {
                                        SetVoxel(uint3(pos), PackVoxel(MAT_AIR, 0, 0, 0));
                                        SetVoxel(uint3(horizontals[idx]), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                                        moved = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (!moved) {
                            SetVoxel(uint3(pos), PackVoxel(material, GetVariant(currentVoxel), 0, newState));
                        }
                        continue;
                    }
                }

                // No movement possible - stay put
                SetVoxel(uint3(pos), outputVoxel);
            }
        }
    }
}
