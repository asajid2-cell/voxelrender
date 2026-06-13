#pragma once

#include "SparseTerrainGenerator.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
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

inline bool CanApplySparseBrushFeedbackPayload(
    uint32_t missingResidentCount,
    uint32_t missingResidentHintCount,
    bool overflowed)
{
    return missingResidentCount == 0u && missingResidentHintCount == 0u && !overflowed;
}

inline bool SparseBrushFeedbackPayloadOverflowed(
    uint32_t reportedRecordCount,
    uint32_t maxRecordCount,
    uint32_t headerOverflowFlag)
{
    return headerOverflowFlag != 0u || reportedRecordCount > maxRecordCount;
}

struct SparseBrushFeedbackVoxelKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const SparseBrushFeedbackVoxelKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct SparseBrushFeedbackVoxelKeyHash {
    size_t operator()(const SparseBrushFeedbackVoxelKey& key) const noexcept {
        uint32_t hash = 2166136261u;
        hash = (hash ^ static_cast<uint32_t>(key.x)) * 16777619u;
        hash = (hash ^ static_cast<uint32_t>(key.y)) * 16777619u;
        hash = (hash ^ static_cast<uint32_t>(key.z)) * 16777619u;
        return static_cast<size_t>(hash);
    }
};

inline bool HasDuplicateSparseBrushFeedbackVoxels(
    const std::vector<SparseBrushFeedbackRecord>& records)
{
    std::unordered_set<SparseBrushFeedbackVoxelKey, SparseBrushFeedbackVoxelKeyHash> seen;
    seen.reserve(records.size());
    for (const SparseBrushFeedbackRecord& record : records) {
        if (IsSparseBrushFeedbackMissingResident(record)) {
            continue;
        }
        const SparseBrushFeedbackVoxelKey key{record.worldX, record.worldY, record.worldZ};
        if (!seen.insert(key).second) {
            return true;
        }
    }
    return false;
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
    bool truncated = false;
};

uint32_t PackSparseEditLocal(LocalVoxelCoord local);
LocalVoxelCoord UnpackSparseEditLocal(uint32_t packedLocal);

SparseEditDeltaBatch BuildSparseEditDeltaBatch(
    const std::vector<SparseEditDelta>& deltas,
    uint32_t maxDeltas,
    uint32_t maxRanges,
    uint32_t rangeTableCapacity = 0);

bool IsSparseEditPersistencePathAllowed(const std::filesystem::path& path);

struct BrickEditOverlay {
    BrickCoord coord;
    std::unordered_map<uint16_t, uint32_t> voxels;
    uint32_t revision = 0;
    // Global RevisionSerial() value when this overlay was last touched. Lets
    // consumers process only overlays changed since their last pass instead of
    // re-walking every overlay ever made (the edit-hitch fix).
    uint64_t lastGlobalRevision = 0;
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

    uint64_t RevisionSerial() const { return m_revisionSerial; }
    size_t EditedBrickCount() const { return m_overlays.size(); }
    size_t EditedVoxelCount() const { return m_editedVoxelCount; }
    const std::vector<SparseEditDelta>& PendingGpuDeltas() const { return m_pendingGpuDeltas; }
    void ClearPendingGpuDeltas(uint32_t consumedCount);
    void ClearPendingGpuDeltas();

private:
    std::unordered_map<BrickCoord, BrickEditOverlay, BrickCoordHash> m_overlays;
    std::vector<SparseEditDelta> m_pendingGpuDeltas;
    size_t m_editedVoxelCount = 0;
    uint64_t m_revisionSerial = 0;
};

} // namespace VENPOD::Simulation
