#pragma once

#include "SparseTerrainGenerator.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace VENPOD::Simulation {

struct SparseEditDelta {
    BrickCoord coord;
    uint32_t packedLocal = 0;
    uint32_t voxel = 0;
    uint32_t revision = 0;
};

struct SparseBrushFeedbackRecord {
    int32_t worldX = 0;
    int32_t worldY = 0;
    int32_t worldZ = 0;
    uint32_t voxel = 0;
};

constexpr uint32_t SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT = 0xFFFFFFFFu;

inline bool IsSparseBrushFeedbackMissingResident(const SparseBrushFeedbackRecord& record) {
    return record.voxel == SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT;
}

struct SparseEditDeltaRange {
    BrickCoord coord;
    uint32_t firstDelta = 0;
    uint32_t deltaCount = 0;
    uint32_t latestRevision = 0;
};

struct SparseEditDeltaBatch {
    std::vector<SparseEditDelta> deltas;
    std::vector<SparseEditDeltaRange> ranges;
    std::vector<uint32_t> rangeTable;
    uint32_t inputDeltaCount = 0;
    uint32_t rangeTableCapacity = 0;
    bool overflow = false;
};

uint32_t PackSparseEditLocal(LocalVoxelCoord local);
LocalVoxelCoord UnpackSparseEditLocal(uint32_t packedLocal);

SparseEditDeltaBatch BuildSparseEditDeltaBatch(
    const std::vector<SparseEditDelta>& deltas,
    uint32_t maxDeltas,
    uint32_t maxRanges,
    uint32_t rangeTableCapacity = 0);

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
    void ForEachVoxelInBrick(
        const BrickCoord& coord,
        const std::function<void(uint16_t localIndex, uint32_t packedVoxel)>& visitor) const;
    void ForEachOverlay(
        const std::function<void(const BrickEditOverlay& overlay)>& visitor) const;
    void ApplyToGeneratedBrick(GeneratedSparseBrick& brick) const;
    std::vector<SparseEditDelta> BuildDeltaSnapshotForBricks(
        const std::vector<BrickCoord>& coords,
        uint32_t maxDeltas) const;
    uint32_t GetOverlayRevision(const BrickCoord& coord) const;
    bool SaveToFile(const std::filesystem::path& path);
    bool LoadFromFile(const std::filesystem::path& path);

    size_t EditedBrickCount() const { return m_overlays.size(); }
    size_t EditedVoxelCount() const { return m_editedVoxelCount; }
    const std::vector<SparseEditDelta>& PendingGpuDeltas() const { return m_pendingGpuDeltas; }
    void ClearPendingGpuDeltas(uint32_t consumedCount);
    void ClearPendingGpuDeltas();

private:
    std::unordered_map<BrickCoord, BrickEditOverlay, BrickCoordHash> m_overlays;
    std::vector<SparseEditDelta> m_pendingGpuDeltas;
    size_t m_editedVoxelCount = 0;
};

} // namespace VENPOD::Simulation
