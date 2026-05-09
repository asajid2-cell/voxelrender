#include "SparseCollision.h"

#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>

namespace VENPOD::Simulation {

namespace {

constexpr float kAabbMaxExclusiveEpsilon = 0.0001f;

int32_t InclusiveMinVoxel(float value) {
    return static_cast<int32_t>(std::floor(value));
}

int32_t InclusiveMaxVoxel(float value) {
    return static_cast<int32_t>(std::floor(value - kAabbMaxExclusiveEpsilon));
}

SparseCollisionAabb TranslateAabb(
    const SparseCollisionAabb& aabb,
    float deltaX,
    float deltaY,
    float deltaZ) {
    return SparseCollisionAabb{
        aabb.minX + deltaX,
        aabb.minY + deltaY,
        aabb.minZ + deltaZ,
        aabb.maxX + deltaX,
        aabb.maxY + deltaY,
        aabb.maxZ + deltaZ
    };
}

} // namespace

CollisionSample SparseCollisionQuery::Sample(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    uint32_t voxel = 0;
    if (m_edits && m_edits->TryGetVoxel(worldX, worldY, worldZ, &voxel)) {
        return {ClassifyVoxel(voxel), voxel, true};
    }

    voxel = m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
    return {ClassifyVoxel(voxel), voxel, false};
}

SparseCollisionVolumeResult SparseCollisionQuery::TestAabb(
    const SparseCollisionAabb& aabb,
    bool liquidsBlock) const {
    SparseCollisionVolumeResult result;

    if (aabb.maxX <= aabb.minX || aabb.maxY <= aabb.minY || aabb.maxZ <= aabb.minZ) {
        result.hasUnknown = true;
        result.unknownVoxels = 1;
        result.blocked = true;
        result.firstBlockingSample = {CollisionSampleStatus::UnknownBlocked, 0, false};
        return result;
    }

    const int32_t minX = InclusiveMinVoxel(aabb.minX);
    const int32_t minY = InclusiveMinVoxel(aabb.minY);
    const int32_t minZ = InclusiveMinVoxel(aabb.minZ);
    const int32_t maxX = InclusiveMaxVoxel(aabb.maxX);
    const int32_t maxY = InclusiveMaxVoxel(aabb.maxY);
    const int32_t maxZ = InclusiveMaxVoxel(aabb.maxZ);

    for (int32_t z = minZ; z <= maxZ; ++z) {
        for (int32_t y = minY; y <= maxY; ++y) {
            for (int32_t x = minX; x <= maxX; ++x) {
                const CollisionSample sample = Sample(x, y, z);
                ++result.sampledVoxels;

                bool sampleBlocks = false;
                switch (sample.status) {
                case CollisionSampleStatus::KnownAir:
                    break;
                case CollisionSampleStatus::KnownSolid:
                    ++result.solidVoxels;
                    sampleBlocks = true;
                    break;
                case CollisionSampleStatus::KnownLiquid:
                    ++result.liquidVoxels;
                    result.hasLiquid = true;
                    sampleBlocks = liquidsBlock;
                    break;
                case CollisionSampleStatus::UnknownBlocked:
                default:
                    ++result.unknownVoxels;
                    result.hasUnknown = true;
                    sampleBlocks = true;
                    break;
                }

                if (sampleBlocks && !result.blocked) {
                    result.blocked = true;
                    result.firstBlockingSample = sample;
                    result.firstBlockingX = x;
                    result.firstBlockingY = y;
                    result.firstBlockingZ = z;
                }
            }
        }
    }

    return result;
}

SparseCollisionSweepResult SparseCollisionQuery::SweepAabb(
    const SparseCollisionAabb& aabb,
    float deltaX,
    float deltaY,
    float deltaZ,
    uint32_t steps,
    bool liquidsBlock) const {
    SparseCollisionSweepResult result;

    if (std::abs(deltaX) < 0.0001f &&
        std::abs(deltaY) < 0.0001f &&
        std::abs(deltaZ) < 0.0001f) {
        result.hitVolume = TestAabb(aabb, liquidsBlock);
        result.blocked = result.hitVolume.blocked;
        result.safeFraction = result.blocked ? 0.0f : 1.0f;
        result.hitFraction = result.blocked ? 0.0f : 1.0f;
        return result;
    }

    steps = std::max(1u, steps);
    float lastSafeFraction = 0.0f;

    for (uint32_t step = 1; step <= steps; ++step) {
        const float fraction = static_cast<float>(step) / static_cast<float>(steps);
        const SparseCollisionAabb probe = TranslateAabb(
            aabb,
            deltaX * fraction,
            deltaY * fraction,
            deltaZ * fraction);
        SparseCollisionVolumeResult volume = TestAabb(probe, liquidsBlock);
        if (volume.blocked) {
            result.blocked = true;
            result.safeFraction = lastSafeFraction;
            result.hitFraction = fraction;
            result.hitVolume = volume;
            return result;
        }
        lastSafeFraction = fraction;
    }

    result.safeFraction = 1.0f;
    result.hitFraction = 1.0f;
    return result;
}

SparseCollisionSupportResult SparseCollisionQuery::FindSupportBelow(
    const SparseCollisionAabb& footprintAabb,
    float maxDrop,
    bool liquidsSupport) const {
    SparseCollisionSupportResult result;
    if (footprintAabb.maxX <= footprintAabb.minX ||
        footprintAabb.maxZ <= footprintAabb.minZ ||
        maxDrop < 0.0f) {
        return result;
    }

    const int32_t minX = InclusiveMinVoxel(footprintAabb.minX);
    const int32_t minZ = InclusiveMinVoxel(footprintAabb.minZ);
    const int32_t maxX = InclusiveMaxVoxel(footprintAabb.maxX);
    const int32_t maxZ = InclusiveMaxVoxel(footprintAabb.maxZ);
    const int32_t startY = InclusiveMinVoxel(footprintAabb.minY);
    const int32_t endY = static_cast<int32_t>(std::floor(footprintAabb.minY - maxDrop));

    for (int32_t y = startY; y >= endY; --y) {
        for (int32_t z = minZ; z <= maxZ; ++z) {
            for (int32_t x = minX; x <= maxX; ++x) {
                const CollisionSample sample = Sample(x, y, z);
                ++result.sampledVoxels;
                if (sample.status == CollisionSampleStatus::KnownSolid) {
                    ++result.solidVoxels;
                    result.found = true;
                    result.supportX = x;
                    result.supportY = y;
                    result.supportZ = z;
                    result.voxel = sample.voxel;
                    result.fromEdit = sample.fromEdit;
                    return result;
                }
                if (sample.status == CollisionSampleStatus::KnownLiquid) {
                    ++result.liquidVoxels;
                    if (liquidsSupport) {
                        result.found = true;
                        result.supportX = x;
                        result.supportY = y;
                        result.supportZ = z;
                        result.voxel = sample.voxel;
                        result.fromEdit = sample.fromEdit;
                        return result;
                    }
                }
                if (sample.status == CollisionSampleStatus::UnknownBlocked) {
                    result.found = true;
                    result.supportX = x;
                    result.supportY = y;
                    result.supportZ = z;
                    result.voxel = sample.voxel;
                    result.fromEdit = sample.fromEdit;
                    return result;
                }
            }
        }
    }

    return result;
}

CollisionSampleStatus SparseCollisionQuery::ClassifyVoxel(uint32_t voxel) {
    const uint8_t material = Utils::UnpackMaterial(voxel);
    if (material == Utils::Material::Air) {
        return CollisionSampleStatus::KnownAir;
    }
    if (material == Utils::Material::Water ||
        material == Utils::Material::Lava ||
        material == Utils::Material::Oil) {
        return CollisionSampleStatus::KnownLiquid;
    }
    return CollisionSampleStatus::KnownSolid;
}

} // namespace VENPOD::Simulation
