#include "SparseCollision.h"

#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace VENPOD::Simulation {

namespace {

constexpr uint64_t kMaxCollisionVolumeSamples = 32768;
constexpr uint64_t kMaxCollisionSupportSamples = 32768;
constexpr uint32_t kMaxCollisionSweepSteps = 1024;

bool IsFiniteAabb(const SparseCollisionAabb& aabb) {
    return std::isfinite(aabb.minX) &&
        std::isfinite(aabb.minY) &&
        std::isfinite(aabb.minZ) &&
        std::isfinite(aabb.maxX) &&
        std::isfinite(aabb.maxY) &&
        std::isfinite(aabb.maxZ);
}

bool TryFloorToInt32(float value, int32_t& out) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(static_cast<double>(value));
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(floored);
    return true;
}

bool TryBuildVoxelRange(float minValue, float maxValue, int32_t& minVoxel, int32_t& maxVoxel) {
    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || maxValue <= minValue) {
        return false;
    }

    const float maxExclusive = std::nextafter(maxValue, -std::numeric_limits<float>::infinity());
    if (!TryFloorToInt32(minValue, minVoxel) || !TryFloorToInt32(maxExclusive, maxVoxel)) {
        return false;
    }

    return maxVoxel >= minVoxel;
}

uint64_t RangeCount(int32_t minVoxel, int32_t maxVoxel) {
    return static_cast<uint64_t>(
        static_cast<int64_t>(maxVoxel) - static_cast<int64_t>(minVoxel) + 1);
}

bool ExceedsSampleLimit(
    int32_t minX,
    int32_t maxX,
    int32_t minY,
    int32_t maxY,
    int32_t minZ,
    int32_t maxZ,
    uint64_t maxSamples) {
    const uint64_t countX = RangeCount(minX, maxX);
    const uint64_t countY = RangeCount(minY, maxY);
    const uint64_t countZ = RangeCount(minZ, maxZ);
    return countX > maxSamples ||
        countY > maxSamples / countX ||
        countZ > maxSamples / (countX * countY);
}

SparseCollisionVolumeResult UnknownBlockedVolume(
    int32_t x = 0,
    int32_t y = 0,
    int32_t z = 0) {
    SparseCollisionVolumeResult result;
    result.blocked = true;
    result.hasUnknown = true;
    result.firstBlockingSample = {CollisionSampleStatus::UnknownBlocked, 0, false};
    result.firstBlockingX = x;
    result.firstBlockingY = y;
    result.firstBlockingZ = z;
    result.sampledVoxels = 1;
    result.unknownVoxels = 1;
    return result;
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
    if (!IsFiniteAabb(aabb)) {
        return UnknownBlockedVolume();
    }

    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;
    int32_t maxZ = 0;
    if (!TryBuildVoxelRange(aabb.minX, aabb.maxX, minX, maxX) ||
        !TryBuildVoxelRange(aabb.minY, aabb.maxY, minY, maxY) ||
        !TryBuildVoxelRange(aabb.minZ, aabb.maxZ, minZ, maxZ) ||
        ExceedsSampleLimit(minX, maxX, minY, maxY, minZ, maxZ, kMaxCollisionVolumeSamples)) {
        const int32_t blockingX = minX;
        const int32_t blockingY = minY;
        const int32_t blockingZ = minZ;
        return UnknownBlockedVolume(blockingX, blockingY, blockingZ);
    }

    SparseCollisionVolumeResult result;

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

    if (!IsFiniteAabb(aabb) ||
        !std::isfinite(deltaX) ||
        !std::isfinite(deltaY) ||
        !std::isfinite(deltaZ)) {
        result.blocked = true;
        result.safeFraction = 0.0f;
        result.hitFraction = 0.0f;
        result.hitVolume = UnknownBlockedVolume();
        return result;
    }

    if (std::abs(deltaX) < 0.0001f &&
        std::abs(deltaY) < 0.0001f &&
        std::abs(deltaZ) < 0.0001f) {
        result.hitVolume = TestAabb(aabb, liquidsBlock);
        result.blocked = result.hitVolume.blocked;
        result.safeFraction = result.blocked ? 0.0f : 1.0f;
        result.hitFraction = result.blocked ? 0.0f : 1.0f;
        return result;
    }

    steps = std::clamp(steps, 1u, kMaxCollisionSweepSteps);
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
    if (!IsFiniteAabb(footprintAabb) || !std::isfinite(maxDrop) || maxDrop < 0.0f) {
        return result;
    }

    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t endY = 0;
    int32_t maxZ = 0;
    if (!TryBuildVoxelRange(footprintAabb.minX, footprintAabb.maxX, minX, maxX) ||
        !TryBuildVoxelRange(footprintAabb.minZ, footprintAabb.maxZ, minZ, maxZ) ||
        !TryFloorToInt32(footprintAabb.minY, minY) ||
        !TryFloorToInt32(footprintAabb.minY - maxDrop, endY)) {
        return result;
    }

    const int32_t startY = minY;
    if (endY > startY ||
        ExceedsSampleLimit(minX, maxX, endY, startY, minZ, maxZ, kMaxCollisionSupportSamples)) {
        return result;
    }

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
