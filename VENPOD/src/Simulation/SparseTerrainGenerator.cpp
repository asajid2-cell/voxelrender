#include "SparseTerrainGenerator.h"

#include "Simulation/TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VENPOD::Simulation {

namespace {

uint32_t AddFlag(uint32_t flags, BrickResidencyFlags flag) {
    return flags | static_cast<uint32_t>(flag);
}

} // namespace

float SparseTerrainGenerator::Smooth01(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float SparseTerrainGenerator::Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

uint32_t SparseTerrainGenerator::Hash3D(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed ^ 2166136261u;
    h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float SparseTerrainGenerator::ValueNoise2D(float x, float z, uint32_t seed) {
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t z0 = static_cast<int32_t>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const float sx = Smooth01(fx);
    const float sz = Smooth01(fz);

    auto sample = [seed](int32_t ix, int32_t iz) {
        return static_cast<float>(Hash3D(ix, 0, iz, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };

    const float a = Lerp(sample(x0, z0), sample(x0 + 1, z0), sx);
    const float b = Lerp(sample(x0, z0 + 1), sample(x0 + 1, z0 + 1), sx);
    return Lerp(a, b, sz) * 2.0f - 1.0f;
}

float SparseTerrainGenerator::HeightAt(int32_t worldX, int32_t worldZ) const {
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);

    const float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, m_seed + 11u);
    const float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, m_seed + 23u);
    const float ridge = 1.0f - std::abs(ridgeSource);
    const float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, m_seed + 37u);

    float height = -64.0f;
    height += broad * 155.0f;
    height += ridge * ridge * 180.0f;
    height += detail * 18.0f;

    const float originDx = x - 96.0f;
    const float originDz = z - 96.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    height += (1.0f - Smooth01(originDistance / 420.0f)) * 120.0f;

    return std::clamp(height, static_cast<float>(TERRAIN_MIN_Y), static_cast<float>(TERRAIN_MAX_Y));
}

SparseTerrainGenerator::ScenicSpawn SparseTerrainGenerator::FindScenicSpawn(
    int32_t originX,
    int32_t originZ,
    float playerHeight,
    int32_t searchRadius,
    int32_t sampleSpacing) const
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int32_t kDirectionCount = 16;
    constexpr int32_t kNearViewDistance = 128;
    constexpr int32_t kFarViewDistance = 640;
    constexpr int32_t kViewStep = 32;

    ScenicSpawn best;
    best.score = -std::numeric_limits<float>::infinity();

    searchRadius = std::max(sampleSpacing, searchRadius);
    sampleSpacing = std::max(16, sampleSpacing);
    const float safeEyeOffset = playerHeight + 3.0f;

    for (int32_t z = originZ - searchRadius; z <= originZ + searchRadius; z += sampleSpacing) {
        for (int32_t x = originX - searchRadius; x <= originX + searchRadius; x += sampleSpacing) {
            const float height = HeightAt(x, z);
            const int32_t groundY = static_cast<int32_t>(std::floor(height));
            if (groundY <= SEA_LEVEL_Y + 6 || groundY >= TERRAIN_MAX_Y - 16) {
                continue;
            }

            const uint32_t groundVoxel = SampleGeneratedVoxel(x, groundY, z);
            const uint8_t groundMaterial = Utils::UnpackMaterial(groundVoxel);
            if (groundMaterial == Utils::Material::Air ||
                groundMaterial == Utils::Material::Water ||
                groundMaterial == Utils::Material::Bedrock) {
                continue;
            }

            bool hasHeadClearance = true;
            const int32_t clearanceTop =
                groundY + static_cast<int32_t>(std::ceil(playerHeight + 8.0f));
            for (int32_t y = groundY + 1; y <= clearanceTop; ++y) {
                if (Utils::UnpackMaterial(SampleGeneratedVoxel(x, y, z)) != Utils::Material::Air) {
                    hasHeadClearance = false;
                    break;
                }
            }
            if (!hasHeadClearance) {
                continue;
            }

            float localMin = height;
            float localMax = height;
            for (int32_t oz = -32; oz <= 32; oz += 16) {
                for (int32_t ox = -32; ox <= 32; ox += 16) {
                    const float h = HeightAt(x + ox, z + oz);
                    localMin = std::min(localMin, h);
                    localMax = std::max(localMax, h);
                }
            }
            const float localRelief = localMax - localMin;
            if (localRelief > 118.0f) {
                continue;
            }
            const float eyeY = static_cast<float>(groundY) + safeEyeOffset;

            float bestDirectionScore = -std::numeric_limits<float>::infinity();
            float bestYaw = 0.0f;
            float bestForwardClearance = 0.0f;
            for (int32_t dirIndex = 0; dirIndex < kDirectionCount; ++dirIndex) {
                const float yaw = (static_cast<float>(dirIndex) / static_cast<float>(kDirectionCount)) * kPi * 2.0f;

                bool nearBlocked = false;
                float openScore = 0.0f;
                float scenicScore = 0.0f;
                float directionReliefMin = height;
                float directionReliefMax = height;
                float forwardClearance = 0.0f;

                constexpr float kViewConeOffsets[] = {
                    -0.70f,
                    -0.35f,
                    0.0f,
                    0.35f,
                    0.70f,
                };
                for (float yawOffset : kViewConeOffsets) {
                    const float coneYaw = yaw + yawOffset;
                    const float dirX = std::cos(coneYaw);
                    const float dirZ = std::sin(coneYaw);
                    for (int32_t d = kViewStep; d <= kFarViewDistance; d += kViewStep) {
                        const int32_t sx = static_cast<int32_t>(std::round(
                            static_cast<float>(x) + dirX * static_cast<float>(d)));
                        const int32_t sz = static_cast<int32_t>(std::round(
                            static_cast<float>(z) + dirZ * static_cast<float>(d)));
                        const float sampleHeight = HeightAt(sx, sz);
                        directionReliefMin = std::min(directionReliefMin, sampleHeight);
                        directionReliefMax = std::max(directionReliefMax, sampleHeight);

                        const float dropBelowEye = eyeY - sampleHeight;
                        if (d <= kNearViewDistance && dropBelowEye < 14.0f) {
                            nearBlocked = true;
                            break;
                        }
                        if (yawOffset == 0.0f && d <= kNearViewDistance) {
                            forwardClearance = static_cast<float>(d);
                        }

                        openScore += std::clamp((dropBelowEye + 24.0f) / 128.0f, 0.0f, 1.0f);
                        const float skylineBand = 1.0f -
                            std::clamp(std::abs(dropBelowEye - 48.0f) / 192.0f, 0.0f, 1.0f);
                        scenicScore += skylineBand * (d > kNearViewDistance ? 1.0f : 0.25f);
                    }
                    if (nearBlocked) {
                        break;
                    }
                }

                if (nearBlocked) {
                    continue;
                }

                const float directionRelief = directionReliefMax - directionReliefMin;
                const float directionScore =
                    openScore * 0.42f +
                    scenicScore * 0.62f +
                    std::clamp(directionRelief / 260.0f, 0.0f, 1.0f) * 8.0f;
                if (directionScore > bestDirectionScore) {
                    bestDirectionScore = directionScore;
                    bestYaw = yaw;
                    bestForwardClearance = forwardClearance;
                }
            }

            if (!std::isfinite(bestDirectionScore)) {
                continue;
            }

            const float distanceFromOrigin =
                std::sqrt(static_cast<float>((x - originX) * (x - originX) + (z - originZ) * (z - originZ)));
            const float heightScore =
                10.0f -
                std::clamp(std::abs(height - 150.0f) / 180.0f, 0.0f, 1.0f) * 6.0f;
            const float reliefScore = std::clamp(localRelief / 110.0f, 0.0f, 1.0f) * 5.0f;
            const float slopePenalty = std::max(0.0f, localRelief - 72.0f) * 0.09f;
            const float distancePenalty = distanceFromOrigin * 0.006f;
            const float score =
                bestDirectionScore +
                heightScore +
                reliefScore -
                slopePenalty -
                distancePenalty;

            if (score > best.score) {
                best.found = true;
                best.worldX = x;
                best.worldZ = z;
                best.groundY = groundY;
                best.eyeY = eyeY;
                best.yaw = bestYaw;
                best.pitch = -0.18f;
                best.score = score;
                best.forwardClearance = bestForwardClearance;
                best.localRelief = localRelief;
            }
        }
    }

    if (best.found) {
        return best;
    }

    const int32_t fallbackGround = static_cast<int32_t>(std::floor(HeightAt(originX, originZ)));
    best.found = false;
    best.worldX = originX;
    best.worldZ = originZ;
    best.groundY = fallbackGround;
    best.eyeY = static_cast<float>(fallbackGround) + safeEyeOffset;
    best.yaw = 0.0f;
    best.pitch = -0.22f;
    best.score = 0.0f;
    return best;
}

bool SparseTerrainGenerator::IsDefinitelyEmptyBrick(
    const BrickCoord& coord,
    float verticalSafetyMargin) const
{
    const int32_t minY = coord.y * SPARSE_BRICK_SIZE;
    if (minY <= SEA_LEVEL_Y || minY <= TERRAIN_MIN_Y + 2) {
        return false;
    }

    const int32_t minX = coord.x * SPARSE_BRICK_SIZE;
    const int32_t minZ = coord.z * SPARSE_BRICK_SIZE;
    const int32_t maxX = minX + SPARSE_BRICK_SIZE - 1;
    const int32_t maxZ = minZ + SPARSE_BRICK_SIZE - 1;
    const int32_t midX = minX + SPARSE_BRICK_SIZE / 2;
    const int32_t midZ = minZ + SPARSE_BRICK_SIZE / 2;

    float maxHeight = static_cast<float>(TERRAIN_MIN_Y);
    const int32_t sampleX[3] = {minX, midX, maxX};
    const int32_t sampleZ[3] = {minZ, midZ, maxZ};
    for (int32_t z : sampleZ) {
        for (int32_t x : sampleX) {
            maxHeight = std::max(maxHeight, HeightAt(x, z));
        }
    }

    return static_cast<float>(minY) > maxHeight + verticalSafetyMargin;
}

uint32_t SparseTerrainGenerator::SampleGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    const uint8_t variant = static_cast<uint8_t>(Hash3D(worldX, worldY, worldZ, m_seed) & 0xFFu);

    if (worldY <= TERRAIN_MIN_Y + 2) {
        return Utils::PackVoxel(Utils::Material::Bedrock, variant, 0, Utils::StateFlags::IsStatic);
    }

    const float height = HeightAt(worldX, worldZ);
    if (static_cast<float>(worldY) <= height) {
        const float depth = height - static_cast<float>(worldY);
        uint8_t material = Utils::Material::Stone;
        if (depth < 2.0f) {
            material = height < static_cast<float>(SEA_LEVEL_Y + 6) ? Utils::Material::Sand :
                (height > 260.0f ? Utils::Material::Stone : Utils::Material::Dirt);
        } else if (depth < 8.0f) {
            material = Utils::Material::Dirt;
        }
        return Utils::PackVoxel(material, variant, 0, Utils::StateFlags::IsStatic);
    }

    if (worldY <= SEA_LEVEL_Y && height < static_cast<float>(SEA_LEVEL_Y - 2)) {
        return Utils::PackVoxel(Utils::Material::Water, variant, 0, 0);
    }

    return Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
}

uint32_t SparseTerrainGenerator::SampleGeneratedSurfaceVoxel(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    int32_t sampleStep) const
{
    const uint32_t voxel = SampleGeneratedVoxel(worldX, worldY, worldZ);
    const uint8_t material = Utils::UnpackMaterial(voxel);
    if (material == Utils::Material::Air) {
        return voxel;
    }

    const int32_t step = std::max(1, sampleStep);
    const int32_t directions[6][3] = {
        { 1, 0, 0 },
        { -1, 0, 0 },
        { 0, 1, 0 },
        { 0, -1, 0 },
        { 0, 0, 1 },
        { 0, 0, -1 },
    };

    // Coarse mid-field rendering needs a visual shell, not a single sampled
    // surface layer. A one-cell shell is easy for the mid raymarcher to skip,
    // while a dense filled volume draws large slabs. Two coarse cells gives the
    // renderer enough thickness to hit stable visual terrain without exposing
    // deep interiors.
    constexpr int32_t kShellSteps = 2;
    for (int32_t shell = 1; shell <= kShellSteps; ++shell) {
        const int32_t distance = step * shell;
        for (const auto& direction : directions) {
            const uint32_t neighbor = SampleGeneratedVoxel(
                worldX + direction[0] * distance,
                worldY + direction[1] * distance,
                worldZ + direction[2] * distance);
            const uint8_t neighborMaterial = Utils::UnpackMaterial(neighbor);
            if (neighborMaterial == Utils::Material::Air) {
                return voxel;
            }
            if (material == Utils::Material::Water && neighborMaterial != Utils::Material::Water) {
                return voxel;
            }
            if (material != Utils::Material::Water && neighborMaterial == Utils::Material::Water) {
                return voxel;
            }
        }
    }

    // Mid/far visual clipmaps should not carry dense interior volume. Rendering
    // those coarse filled cells produces page-like slabs once traversal becomes
    // more exact. The authoritative generated terrain function and near sparse
    // bricks still retain the full solid volume for collision/editing.
    return Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
}

GeneratedSparseBrick SparseTerrainGenerator::GenerateBrick(const BrickCoord& coord) const {
    GeneratedSparseBrick brick;
    brick.coord = coord;

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const int32_t worldX = coord.x * SPARSE_BRICK_SIZE + x;
                const int32_t worldY = coord.y * SPARSE_BRICK_SIZE + y;
                const int32_t worldZ = coord.z * SPARSE_BRICK_SIZE + z;
                brick.voxels[LocalVoxelIndex({x, y, z})] = SampleGeneratedVoxel(worldX, worldY, worldZ);
            }
        }
    }

    ComputeOccupancyAndFlags(brick);
    return brick;
}

void SparseTerrainGenerator::ComputeOccupancyAndFlags(GeneratedSparseBrick& brick) {
    bool anySolid = false;
    bool anyAir = false;
    bool anyWater = false;
    bool homogeneous = true;
    const uint8_t firstMaterial = Utils::UnpackMaterial(brick.voxels[0]);

    brick.occupancyWord0 = 0;
    brick.occupancyWord1 = 0;

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const uint32_t voxel = brick.voxels[LocalVoxelIndex({x, y, z})];
                const uint8_t material = Utils::UnpackMaterial(voxel);
                homogeneous = homogeneous && material == firstMaterial;
                if (material == Utils::Material::Air) {
                    anyAir = true;
                    continue;
                }

                anySolid = true;
                anyWater = anyWater || material == Utils::Material::Water;

                const uint32_t subX = x >> 2;
                const uint32_t subY = y >> 2;
                const uint32_t subZ = z >> 2;
                const uint32_t subIndex = subX + subY * 4u + subZ * 16u;
                if (subIndex < 32u) {
                    brick.occupancyWord0 |= 1u << subIndex;
                } else {
                    brick.occupancyWord1 |= 1u << (subIndex - 32u);
                }
            }
        }
    }

    uint32_t flags = 0;
    if (!anySolid) {
        flags = AddFlag(flags, BrickResidencyFlags::Empty);
    }
    if (anySolid && !anyAir && !anyWater) {
        flags = AddFlag(flags, BrickResidencyFlags::Solid);
    }
    if (homogeneous) {
        flags = AddFlag(flags, BrickResidencyFlags::Homogeneous);
    }
    if (anyWater) {
        flags = AddFlag(flags, BrickResidencyFlags::HasWater);
    }
    brick.flags = flags;
}

} // namespace VENPOD::Simulation
