#pragma once

#include "SparseEditStore.h"

#include <cstdint>

namespace VENPOD::Simulation {

enum class CollisionSampleStatus {
    KnownAir,
    KnownSolid,
    KnownLiquid,
    UnknownBlocked
};

struct CollisionSample {
    CollisionSampleStatus status = CollisionSampleStatus::UnknownBlocked;
    uint32_t voxel = 0;
    bool fromEdit = false;
};

struct SparseCollisionAabb {
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
};

struct SparseCollisionVolumeResult {
    bool blocked = false;
    bool hasLiquid = false;
    bool hasUnknown = false;
    CollisionSample firstBlockingSample{};
    int32_t firstBlockingX = 0;
    int32_t firstBlockingY = 0;
    int32_t firstBlockingZ = 0;
    uint32_t sampledVoxels = 0;
    uint32_t solidVoxels = 0;
    uint32_t liquidVoxels = 0;
    uint32_t unknownVoxels = 0;
};

struct SparseCollisionSweepResult {
    bool blocked = false;
    float safeFraction = 1.0f;
    float hitFraction = 1.0f;
    SparseCollisionVolumeResult hitVolume{};
};

struct SparseCollisionSupportResult {
    bool found = false;
    int32_t supportX = 0;
    int32_t supportY = 0;
    int32_t supportZ = 0;
    uint32_t voxel = 0;
    bool fromEdit = false;
    uint32_t sampledVoxels = 0;
    uint32_t solidVoxels = 0;
    uint32_t liquidVoxels = 0;
};

class SparseCollisionQuery {
public:
    SparseCollisionQuery(const SparseTerrainGenerator& terrain, const SparseEditStore* edits = nullptr)
        : m_terrain(terrain), m_edits(edits) {}

    CollisionSample Sample(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    SparseCollisionVolumeResult TestAabb(const SparseCollisionAabb& aabb, bool liquidsBlock = false) const;
    SparseCollisionSweepResult SweepAabb(
        const SparseCollisionAabb& aabb,
        float deltaX,
        float deltaY,
        float deltaZ,
        uint32_t steps,
        bool liquidsBlock = false) const;
    SparseCollisionSupportResult FindSupportBelow(
        const SparseCollisionAabb& footprintAabb,
        float maxDrop,
        bool liquidsSupport = false) const;

private:
    static CollisionSampleStatus ClassifyVoxel(uint32_t voxel);

    const SparseTerrainGenerator& m_terrain;
    const SparseEditStore* m_edits = nullptr;
};

} // namespace VENPOD::Simulation
