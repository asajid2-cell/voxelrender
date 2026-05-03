#include "SparseEditStore.h"

namespace VENPOD::Simulation {

void SparseEditStore::SetVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
    const BrickCoord brick = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    const LocalVoxelCoord local = LocalVoxelFromWorld(worldX, worldY, worldZ);
    const uint16_t localIndex = LocalVoxelIndex(local);

    BrickEditOverlay& overlay = m_overlays[brick];
    if (overlay.voxels.empty()) {
        overlay.coord = brick;
    }

    const bool inserted = overlay.voxels.find(localIndex) == overlay.voxels.end();
    overlay.voxels[localIndex] = packedVoxel;
    overlay.revision++;
    overlay.dirtyDisk = true;
    overlay.dirtyGpu = true;
    if (inserted) {
        ++m_editedVoxelCount;
    }
}

bool SparseEditStore::TryGetVoxel(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    uint32_t* outVoxel) const
{
    const BrickCoord brick = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    auto overlayIt = m_overlays.find(brick);
    if (overlayIt == m_overlays.end()) {
        return false;
    }

    const uint16_t localIndex = LocalVoxelIndex(LocalVoxelFromWorld(worldX, worldY, worldZ));
    auto voxelIt = overlayIt->second.voxels.find(localIndex);
    if (voxelIt == overlayIt->second.voxels.end()) {
        return false;
    }

    if (outVoxel) {
        *outVoxel = voxelIt->second;
    }
    return true;
}

bool SparseEditStore::HasOverlay(const BrickCoord& coord) const {
    return m_overlays.find(coord) != m_overlays.end();
}

void SparseEditStore::ApplyToGeneratedBrick(GeneratedSparseBrick& brick) const {
    auto overlayIt = m_overlays.find(brick.coord);
    if (overlayIt == m_overlays.end()) {
        return;
    }

    for (const auto& [localIndex, packedVoxel] : overlayIt->second.voxels) {
        if (localIndex < brick.voxels.size()) {
            brick.voxels[localIndex] = packedVoxel;
        }
    }

    SparseTerrainGenerator::ComputeOccupancyAndFlags(brick);
}

} // namespace VENPOD::Simulation

