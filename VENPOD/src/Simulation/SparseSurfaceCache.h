#pragma once

#include "SparseSurfaceExtractor.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace VENPOD::Simulation {

struct SparseSurfaceCacheStats {
    uint32_t cachedBricks = 0;
    uint32_t totalFaces = 0;
    uint32_t facesGeneratedLastUpdate = 0;
    uint32_t bricksUpdatedLastFrame = 0;
    uint32_t bricksRemovedLastFrame = 0;
    uint32_t serial = 0;
};

struct SparseSurfaceBrickRange {
    BrickCoord coord;
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
    uint32_t flags = 0;
};

struct SparseSurfaceGpuSnapshot {
    std::vector<SparseSurfaceFace> faces;
    // Hash table keyed by BrickCoord. Invalid entries have flags == 0.
    std::vector<SparseSurfaceBrickRange> ranges;
    uint32_t rangeCount = 0;
    uint32_t rangeTableCapacity = 0;
    uint32_t serial = 0;
    uint32_t candidateBricks = 0;
    uint32_t visibleBricks = 0;
    uint32_t culledBricks = 0;
};

struct SparseSurfaceVisibilityConfig {
    bool enabled = false;
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
};

class SparseSurfaceCache {
public:
    void BeginFrame();

    bool UpdateBrick(
        const GeneratedSparseBrick& brick,
        const SparseNeighborSampler& neighborSampler = {});
    bool RemoveBrick(const BrickCoord& coord);
    void Clear();

    const std::vector<SparseSurfaceFace>* FindFaces(const BrickCoord& coord) const;
    bool BuildContiguousFaceList(std::vector<SparseSurfaceFace>& outFaces) const;
    bool BuildGpuSnapshot(
        SparseSurfaceGpuSnapshot& outSnapshot,
        const SparseSurfaceVisibilityConfig* visibility = nullptr) const;
    static bool TryLookupRangeInSnapshot(
        const SparseSurfaceGpuSnapshot& snapshot,
        const BrickCoord& coord,
        SparseSurfaceBrickRange* outRange = nullptr);

    const SparseSurfaceCacheStats& GetStats() const { return m_stats; }

private:
    std::unordered_map<BrickCoord, std::vector<SparseSurfaceFace>, BrickCoordHash> m_facesByBrick;
    SparseSurfaceCacheStats m_stats;
};

} // namespace VENPOD::Simulation
