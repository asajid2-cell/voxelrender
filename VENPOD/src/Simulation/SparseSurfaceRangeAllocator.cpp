#include "SparseSurfaceRangeAllocator.h"

#include <algorithm>
#include <limits>

namespace VENPOD::Simulation {

namespace {

uint64_t SaturatingAddUint64(uint64_t a, uint64_t b) {
    if (a > std::numeric_limits<uint64_t>::max() - b) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
}

uint32_t SaturatingAddUint32(uint32_t a, uint32_t b) {
    if (a > std::numeric_limits<uint32_t>::max() - b) {
        return std::numeric_limits<uint32_t>::max();
    }
    return a + b;
}

uint32_t SaturatingSizeToUint32(size_t value) {
    return value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(value);
}

uint32_t NextNonZeroGeneration(uint32_t generation) {
    const uint32_t next = generation + 1u;
    return next == 0u ? 1u : next;
}

} // namespace

void SparseSurfaceRangeAllocator::Initialize(uint32_t maxFaces, uint32_t retirementDelayFrames) {
    Clear();
    m_maxFaces = maxFaces;
    m_retirementDelayFrames = retirementDelayFrames;
    if (maxFaces > 0) {
        m_freeRanges.push_back({0u, maxFaces});
    }
    RefreshStats();
}

void SparseSurfaceRangeAllocator::Clear() {
    m_maxFaces = 0;
    m_retirementDelayFrames = 3;
    m_completedRetirementToken = 0;
    m_currentRetirementToken = 0;
    m_allocations.clear();
    m_freeRanges.clear();
    m_retiredRanges.clear();
    m_stats = {};
    m_statsRefreshBatchDepth = 0;
    m_statsRefreshDirty = false;
}

void SparseSurfaceRangeAllocator::BeginFrame(uint64_t frameIndex) {
    BeginFrame(
        frameIndex,
        SaturatingAddUint64(frameIndex, static_cast<uint64_t>(m_retirementDelayFrames)));
}

void SparseSurfaceRangeAllocator::BeginFrame(
    uint64_t completedRetirementToken,
    uint64_t currentRetirementToken)
{
    m_completedRetirementToken = std::max(m_completedRetirementToken, completedRetirementToken);
    m_currentRetirementToken = std::max(
        m_currentRetirementToken,
        std::max(m_completedRetirementToken, currentRetirementToken));
    for (auto it = m_retiredRanges.begin(); it != m_retiredRanges.end();) {
        if (it->retireToken > m_completedRetirementToken) {
            ++it;
            continue;
        }
        AddFreeRange(it->firstFace, it->count);
        it = m_retiredRanges.erase(it);
    }
    RefreshStats();
}

void SparseSurfaceRangeAllocator::BeginStatsRefreshBatch() {
    ++m_statsRefreshBatchDepth;
}

void SparseSurfaceRangeAllocator::EndStatsRefreshBatch() {
    if (m_statsRefreshBatchDepth == 0) {
        return;
    }
    --m_statsRefreshBatchDepth;
    if (m_statsRefreshBatchDepth == 0 && m_statsRefreshDirty) {
        m_statsRefreshDirty = false;
        RecomputeStats();
    }
}

bool SparseSurfaceRangeAllocator::AllocateOrResize(
    const BrickCoord& coord,
    uint32_t faceCount,
    SparseSurfaceFaceAllocation* outAllocation)
{
    if (outAllocation) {
        *outAllocation = {};
    }
    if (faceCount == 0) {
        Free(coord);
        return true;
    }

    auto existing = m_allocations.find(coord);
    if (existing != m_allocations.end() && existing->second.capacity >= faceCount) {
        existing->second.faceCount = faceCount;
        existing->second.generation = NextNonZeroGeneration(existing->second.generation);
        if (outAllocation) {
            *outAllocation = existing->second;
        }
        RefreshStats();
        return true;
    }

    uint32_t firstFace = UINT32_MAX;
    for (auto it = m_freeRanges.begin(); it != m_freeRanges.end(); ++it) {
        if (it->count < faceCount) {
            continue;
        }
        const uint64_t allocationEnd =
            static_cast<uint64_t>(it->firstFace) + static_cast<uint64_t>(faceCount);
        if (allocationEnd > static_cast<uint64_t>(m_maxFaces)) {
            continue;
        }
        firstFace = it->firstFace;
        it->firstFace += faceCount;
        it->count -= faceCount;
        if (it->count == 0) {
            m_freeRanges.erase(it);
        }
        break;
    }

    if (firstFace == UINT32_MAX) {
        ++m_stats.allocationFailures;
        return false;
    }

    uint32_t generation = 1;
    if (existing != m_allocations.end()) {
        generation = NextNonZeroGeneration(existing->second.generation);
        RetireRange(existing->second.firstFace, existing->second.capacity);
        existing->second = {firstFace, faceCount, faceCount, generation};
        if (outAllocation) {
            *outAllocation = existing->second;
        }
        RefreshStats();
        return true;
    }

    SparseSurfaceFaceAllocation allocation{firstFace, faceCount, faceCount, generation};
    m_allocations.emplace(coord, allocation);
    if (outAllocation) {
        *outAllocation = allocation;
    }
    RefreshStats();
    return true;
}

void SparseSurfaceRangeAllocator::Free(const BrickCoord& coord) {
    auto existing = m_allocations.find(coord);
    if (existing == m_allocations.end()) {
        return;
    }
    RetireRange(existing->second.firstFace, existing->second.capacity);
    m_allocations.erase(existing);
    RefreshStats();
}

void SparseSurfaceRangeAllocator::ReleaseNotIn(
    const std::unordered_set<BrickCoord, BrickCoordHash>& liveCoords)
{
    for (auto it = m_allocations.begin(); it != m_allocations.end();) {
        if (liveCoords.find(it->first) != liveCoords.end()) {
            ++it;
            continue;
        }
        RetireRange(it->second.firstFace, it->second.capacity);
        it = m_allocations.erase(it);
    }
    RefreshStats();
}

bool SparseSurfaceRangeAllocator::TryGet(
    const BrickCoord& coord,
    SparseSurfaceFaceAllocation* outAllocation) const
{
    auto existing = m_allocations.find(coord);
    if (existing == m_allocations.end()) {
        return false;
    }
    if (outAllocation) {
        *outAllocation = existing->second;
    }
    return true;
}

void SparseSurfaceRangeAllocator::RetireRange(uint32_t firstFace, uint32_t count) {
    if (count == 0) {
        return;
    }
    if (firstFace >= m_maxFaces) {
        return;
    }
    const uint64_t maxCount =
        static_cast<uint64_t>(m_maxFaces) - static_cast<uint64_t>(firstFace);
    if (static_cast<uint64_t>(count) > maxCount) {
        count = static_cast<uint32_t>(maxCount);
    }
    if (count == 0) {
        return;
    }
    m_retiredRanges.push_back({firstFace, count, m_currentRetirementToken});
}

void SparseSurfaceRangeAllocator::AddFreeRange(uint32_t firstFace, uint32_t count) {
    if (count == 0) {
        return;
    }
    if (firstFace >= m_maxFaces) {
        return;
    }
    const uint64_t maxCount =
        static_cast<uint64_t>(m_maxFaces) - static_cast<uint64_t>(firstFace);
    if (static_cast<uint64_t>(count) > maxCount) {
        count = static_cast<uint32_t>(maxCount);
    }
    if (count == 0) {
        return;
    }
    m_freeRanges.push_back({firstFace, count});
    std::sort(m_freeRanges.begin(), m_freeRanges.end(), [](const FreeRange& a, const FreeRange& b) {
        return a.firstFace < b.firstFace;
    });

    std::vector<FreeRange> merged;
    merged.reserve(m_freeRanges.size());
    for (const FreeRange& range : m_freeRanges) {
        if (merged.empty()) {
            merged.push_back(range);
            continue;
        }
        FreeRange& back = merged.back();
        const uint64_t backEnd =
            static_cast<uint64_t>(back.firstFace) + static_cast<uint64_t>(back.count);
        if (range.firstFace <= backEnd) {
            const uint64_t rangeEnd =
                static_cast<uint64_t>(range.firstFace) + static_cast<uint64_t>(range.count);
            const uint64_t mergedEnd =
                std::min<uint64_t>(
                    std::max(backEnd, rangeEnd),
                    static_cast<uint64_t>(m_maxFaces));
            const uint64_t mergedCount =
                mergedEnd > static_cast<uint64_t>(back.firstFace)
                    ? mergedEnd - static_cast<uint64_t>(back.firstFace)
                    : 0u;
            back.count = mergedCount > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(mergedCount);
        } else {
            merged.push_back(range);
        }
    }
    m_freeRanges = std::move(merged);
}

void SparseSurfaceRangeAllocator::RefreshStats() {
    if (m_statsRefreshBatchDepth > 0) {
        m_statsRefreshDirty = true;
        return;
    }
    RecomputeStats();
}

void SparseSurfaceRangeAllocator::RecomputeStats() {
    uint32_t allocatedCapacity = 0;
    for (const auto& item : m_allocations) {
        allocatedCapacity = SaturatingAddUint32(allocatedCapacity, item.second.capacity);
    }
    uint32_t largestFree = 0;
    for (const FreeRange& range : m_freeRanges) {
        largestFree = std::max(largestFree, range.count);
    }
    uint32_t pendingRetiredCapacity = 0;
    for (const RetiredRange& range : m_retiredRanges) {
        pendingRetiredCapacity = SaturatingAddUint32(pendingRetiredCapacity, range.count);
    }

    const uint32_t failures = m_stats.allocationFailures;
    m_stats = {};
    m_stats.allocationFailures = failures;
    m_stats.allocationCount = SaturatingSizeToUint32(m_allocations.size());
    m_stats.allocatedCapacity = allocatedCapacity;
    m_stats.freeRangeCount = SaturatingSizeToUint32(m_freeRanges.size());
    m_stats.largestFreeRange = largestFree;
    m_stats.pendingRetiredRangeCount = SaturatingSizeToUint32(m_retiredRanges.size());
    m_stats.pendingRetiredCapacity = pendingRetiredCapacity;
}

} // namespace VENPOD::Simulation
