#pragma once

#include "SparseVoxelTypes.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

struct SparseSurfaceFaceAllocation {
    uint32_t firstFace = 0;
    uint32_t capacity = 0;
    uint32_t faceCount = 0;
    uint32_t generation = 0;
};

struct SparseSurfaceRangeAllocatorStats {
    uint32_t allocationCount = 0;
    uint32_t allocatedCapacity = 0;
    uint32_t freeRangeCount = 0;
    uint32_t largestFreeRange = 0;
    uint32_t pendingRetiredRangeCount = 0;
    uint32_t pendingRetiredCapacity = 0;
    uint32_t allocationFailures = 0;
};

class SparseSurfaceRangeAllocator {
public:
    void Initialize(uint32_t maxFaces, uint32_t retirementDelayFrames = 3);
    void Clear();
    void BeginFrame(uint64_t frameIndex);
    void BeginFrame(uint64_t completedRetirementToken, uint64_t currentRetirementToken);

    bool AllocateOrResize(
        const BrickCoord& coord,
        uint32_t faceCount,
        SparseSurfaceFaceAllocation* outAllocation);
    void Free(const BrickCoord& coord);
    void ReleaseNotIn(const std::unordered_set<BrickCoord, BrickCoordHash>& liveCoords);

    bool TryGet(const BrickCoord& coord, SparseSurfaceFaceAllocation* outAllocation = nullptr) const;
    const SparseSurfaceRangeAllocatorStats& GetStats() const { return m_stats; }

private:
    struct FreeRange {
        uint32_t firstFace = 0;
        uint32_t count = 0;
    };

    struct RetiredRange {
        uint32_t firstFace = 0;
        uint32_t count = 0;
        uint64_t retireToken = 0;
    };

    void RetireRange(uint32_t firstFace, uint32_t count);
    void AddFreeRange(uint32_t firstFace, uint32_t count);
    void RefreshStats();

    uint32_t m_maxFaces = 0;
    uint32_t m_retirementDelayFrames = 3;
    uint64_t m_completedRetirementToken = 0;
    uint64_t m_currentRetirementToken = 0;
    std::unordered_map<BrickCoord, SparseSurfaceFaceAllocation, BrickCoordHash> m_allocations;
    std::vector<FreeRange> m_freeRanges;
    std::vector<RetiredRange> m_retiredRanges;
    SparseSurfaceRangeAllocatorStats m_stats;
};

} // namespace VENPOD::Simulation
