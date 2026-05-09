#pragma once

#include "SparseSurfaceExtractor.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

struct SparseSurfaceCacheStats {
    uint32_t cachedBricks = 0;
    uint32_t knownBricks = 0;
    uint32_t knownEmptySurfaceBricks = 0;
    uint32_t totalUnitFaces = 0;
    uint32_t totalFaces = 0;
    uint32_t unitFacesGeneratedLastUpdate = 0;
    uint32_t facesGeneratedLastUpdate = 0;
    uint32_t bricksUpdatedLastFrame = 0;
    uint32_t bricksPartiallyUpdatedLastFrame = 0;
    uint32_t facesRemovedByPartialUpdatesLastFrame = 0;
    uint32_t bricksRemovedLastFrame = 0;
    uint32_t emptyFastPathBricksLastFrame = 0;
    uint32_t pendingGpuDirtyBricks = 0;
    uint32_t pendingGpuRemovedBricks = 0;
    uint32_t serial = 0;
};

struct SparseSurfaceBrickRange {
    BrickCoord coord;
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
    uint32_t flags = 0;
};

constexpr uint32_t kSparseSurfaceRangeValid = 1u;
constexpr uint32_t kSparseSurfaceDirectionMaskShift = 8u;
constexpr uint32_t kSparseSurfaceDirectionMaskBits = 0x3Fu;

inline uint32_t SparseSurfaceDirectionBit(uint32_t direction) {
    return direction < 6u ? (1u << direction) : 0u;
}

inline uint32_t SparseSurfacePackRecordFlags(uint32_t baseFlags, uint32_t directionMask) {
    return (baseFlags & 0xFFu) |
        ((directionMask & kSparseSurfaceDirectionMaskBits) << kSparseSurfaceDirectionMaskShift);
}

inline uint32_t SparseSurfaceRecordDirectionMask(uint32_t flags) {
    return (flags >> kSparseSurfaceDirectionMaskShift) & kSparseSurfaceDirectionMaskBits;
}

uint32_t BuildSparseSurfaceDirectionMask(const std::vector<SparseSurfaceFace>& faces);

struct SparseSurfaceDrawArgs {
    uint32_t indexCountPerInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t baseVertexLocation = 0;
    uint32_t startInstanceLocation = 0;
};

static_assert(sizeof(SparseSurfaceDrawArgs) == 20);

struct SparseSurfaceRecord {
    BrickCoord coord;
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
    // Low byte carries lifecycle/valid bits. Bits 8..13 carry the set of
    // exposed face directions present in this record for coarse backface cull.
    uint32_t flags = 0;
    uint32_t generation = 0;
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;
    int32_t maxZ = 0;
};

static_assert(sizeof(SparseSurfaceRecord) == 52);

struct SparseSurfaceClusterRecord {
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    uint32_t firstRecord = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;
    int32_t maxZ = 0;
    uint32_t recordCount = 0;
    uint32_t faceCount = 0;
    uint32_t flags = 0;
};

static_assert(sizeof(SparseSurfaceClusterRecord) == 40);

uint64_t SparseSurfaceMortonKey(const BrickCoord& coord);
void ComputeSparseSurfaceFaceBounds(
    const SparseSurfaceFace* faces,
    uint32_t faceCount,
    int32_t* outMinX,
    int32_t* outMinY,
    int32_t* outMinZ,
    int32_t* outMaxX,
    int32_t* outMaxY,
    int32_t* outMaxZ);
void SortSparseSurfaceRecordsForClusters(std::vector<SparseSurfaceRecord>& records);
std::vector<SparseSurfaceClusterRecord> BuildSparseSurfaceClusters(
    const std::vector<SparseSurfaceRecord>& records,
    uint32_t recordsPerCluster = 32,
    uint32_t maxClusterExtentVoxels = 0);

struct SparseSurfaceDrawBatch {
    BrickCoord coord;
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
};

struct SparseSurfaceBrickFaceCount {
    BrickCoord coord;
    uint32_t faceCount = 0;
};

struct SparseSurfaceDirtyBrick {
    BrickCoord coord;
    uint32_t serial = 0;
};

struct SparseSurfaceFaceRun {
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
};

std::vector<SparseSurfaceFaceRun> BuildSparseSurfaceChangedFaceRuns(
    const SparseSurfaceFace* currentFaces,
    const SparseSurfaceFace* previousFaces,
    uint32_t faceCount);

struct SparseSurfaceGpuSnapshot {
    std::vector<SparseSurfaceFace> faces;
    // Hash table keyed by BrickCoord. Invalid entries have flags == 0.
    std::vector<SparseSurfaceBrickRange> ranges;
    // One indirect-draw-compatible command per visible brick with exposed faces.
    std::vector<SparseSurfaceDrawArgs> drawArgs;
    std::vector<SparseSurfaceDrawBatch> drawBatches;
    std::vector<SparseSurfaceRecord> surfaceRecords;
    // All cached bricks, including currently culled bricks. Used by GPU-side
    // range allocators to distinguish real removals from visibility culling.
    std::vector<SparseSurfaceBrickFaceCount> brickFaceCounts;
    // Retry-safe CPU->GPU synchronization hints. Dirty payloads should only be
    // acknowledged after the GPU copy command was emitted successfully.
    std::vector<SparseSurfaceDirtyBrick> dirtyBricks;
    std::vector<SparseSurfaceDirtyBrick> removedBricks;
    uint32_t rangeCount = 0;
    uint32_t rangeTableCapacity = 0;
    uint32_t drawCommandCount = 0;
    uint32_t serial = 0;
    uint32_t candidateBricks = 0;
    uint32_t visibleBricks = 0;
    uint32_t culledBricks = 0;
    uint32_t lookaheadVisibleBricks = 0;
};

struct SparseSurfaceVisibilityConfig {
    bool enabled = false;
    // Stable near-field snapshots should not be shaped by camera yaw. When
    // false, visibility is a distance/ownership filter only; this prevents the
    // raster surface layer from flashing as the camera turns quickly.
    bool useFrustum = true;
    float cameraX = 0.0f;
    float cameraY = 0.0f;
    float cameraZ = 0.0f;
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    float forwardZ = 1.0f;
    float rightX = 1.0f;
    float rightY = 0.0f;
    float rightZ = 0.0f;
    float upX = 0.0f;
    float upY = 1.0f;
    float upZ = 0.0f;
    float fovYRadians = 1.04719755f;
    float aspectRatio = 1.7777778f;
    float maxDistance = 2500.0f;
    float padding = 24.0f;
    bool useMotionLookahead = false;
    float lookaheadCameraX = 0.0f;
    float lookaheadCameraY = 0.0f;
    float lookaheadCameraZ = 0.0f;
};

class SparseSurfaceCache {
public:
    void BeginFrame();

    bool UpdateBrick(
        const GeneratedSparseBrick& brick,
        const SparseNeighborSampler& neighborSampler = {});
    bool UpdateBrickRegion(
        const GeneratedSparseBrick& brick,
        const SparseSurfaceLocalRegion& region,
        const SparseNeighborSampler& neighborSampler = {});
    bool RemoveBrick(const BrickCoord& coord);
    void Clear();

    const std::vector<SparseSurfaceFace>* FindFaces(const BrickCoord& coord) const;
    bool IsSurfaceKnown(const BrickCoord& coord) const;
    bool BuildContiguousFaceList(std::vector<SparseSurfaceFace>& outFaces) const;
    bool BuildGpuSnapshot(
        SparseSurfaceGpuSnapshot& outSnapshot,
        const SparseSurfaceVisibilityConfig* visibility = nullptr) const;
    void MarkGpuUploadComplete(
        uint32_t completedSerial,
        const std::vector<BrickCoord>& uploadedPayloadBricks,
        const std::vector<BrickCoord>& removedBricks);
    static bool TryLookupRangeInSnapshot(
        const SparseSurfaceGpuSnapshot& snapshot,
        const BrickCoord& coord,
        SparseSurfaceBrickRange* outRange = nullptr);

    const SparseSurfaceCacheStats& GetStats() const { return m_stats; }

private:
    std::unordered_map<BrickCoord, std::vector<SparseSurfaceFace>, BrickCoordHash> m_facesByBrick;
    std::unordered_map<BrickCoord, uint32_t, BrickCoordHash> m_unitFacesByBrick;
    std::unordered_set<BrickCoord, BrickCoordHash> m_knownBricks;
    std::unordered_map<BrickCoord, uint32_t, BrickCoordHash> m_dirtyBrickSerials;
    std::unordered_map<BrickCoord, uint32_t, BrickCoordHash> m_removedBrickSerials;
    SparseSurfaceCacheStats m_stats;

    void RefreshPendingGpuStats();
    void RefreshKnownStats();
};

} // namespace VENPOD::Simulation
