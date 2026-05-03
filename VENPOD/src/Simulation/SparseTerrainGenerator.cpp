#include "SparseTerrainGenerator.h"

#include "Simulation/TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>

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
