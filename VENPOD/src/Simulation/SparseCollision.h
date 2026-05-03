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

class SparseCollisionQuery {
public:
    SparseCollisionQuery(const SparseTerrainGenerator& terrain, const SparseEditStore* edits = nullptr)
        : m_terrain(terrain), m_edits(edits) {}

    CollisionSample Sample(int32_t worldX, int32_t worldY, int32_t worldZ) const;

private:
    static CollisionSampleStatus ClassifyVoxel(uint32_t voxel);

    const SparseTerrainGenerator& m_terrain;
    const SparseEditStore* m_edits = nullptr;
};

} // namespace VENPOD::Simulation

