#pragma once

#include "SparseTerrainGenerator.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace VENPOD::Simulation {

struct BrickEditOverlay {
    BrickCoord coord;
    std::unordered_map<uint16_t, uint32_t> voxels;
    uint32_t revision = 0;
    bool dirtyDisk = false;
    bool dirtyGpu = false;
};

class SparseEditStore {
public:
    void SetVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel);
    bool TryGetVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t* outVoxel = nullptr) const;
    bool HasOverlay(const BrickCoord& coord) const;
    void ApplyToGeneratedBrick(GeneratedSparseBrick& brick) const;

    size_t EditedBrickCount() const { return m_overlays.size(); }
    size_t EditedVoxelCount() const { return m_editedVoxelCount; }

private:
    std::unordered_map<BrickCoord, BrickEditOverlay, BrickCoordHash> m_overlays;
    size_t m_editedVoxelCount = 0;
};

} // namespace VENPOD::Simulation

