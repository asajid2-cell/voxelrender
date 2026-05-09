#include "SparseSurfaceRangeAllocator.h"

#include <algorithm>

namespace VENPOD::Simulation {

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
}

void SparseSurfaceRangeAllocator::BeginFrame(uint64_t frameIndex) {
    BeginFrame(
        frameIndex,
        frameIndex + static_cast<uint64_t>(m_retirementDelayFrames));
}

void SparseSurfaceRangeAllocator::BeginFrame(
    uint64_t completedRetirementToken,
    uint64_t currentRetirementToken)
{
    m_completedRetirementToken = completedRetirementToken;
    m_currentRetirementToken = std::max(completedRetirementToken, currentRetirementToken);
    for (auto it = m_retiredRanges.begin(); it != m_retiredRanges.end();) {
        if (it->retireToken > completedRetirementToken) {
            ++it;
            continue;
        }
        AddFreeRange(it->firstFace, it->count);
        it = m_retiredRanges.erase(it);
    }
    RefreshStats();
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
        ++existing->second.generation;
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
        generation = existing->second.generation + 1u;
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
    m_retiredRanges.push_back({firstFace, count, m_currentRetirementToken});
}

void SparseSurfaceRangeAllocator::AddFreeRange(uint32_t firstFace, uint32_t count) {
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
        const uint32_t backEnd = back.firstFace + back.count;
        if (range.firstFace <= backEnd) {
            const uint32_t rangeEnd = range.firstFace + range.count;
            back.count = std::max(backEnd, rangeEnd) - back.firstFace;
        } else {
            merged.push_back(range);
        }
    }
    m_freeRanges = std::move(merged);
}

void SparseSurfaceRangeAllocator::RefreshStats() {
    uint32_t allocatedCapacity = 0;
    for (const auto& item : m_allocations) {
        allocatedCapacity += item.second.capacity;
    }
    uint32_t largestFree = 0;
    for (const FreeRange& range : m_freeRanges) {
        largestFree = std::max(largestFree, range.count);
    }
    uint32_t pendingRetiredCapacity = 0;
    for (const RetiredRange& range : m_retiredRanges) {
        pendingRetiredCapacity += range.count;
    }

    const uint32_t failures = m_stats.allocationFailures;
    m_stats = {};
    m_stats.allocationFailures = failures;
    m_stats.allocationCount = static_cast<uint32_t>(m_allocations.size());
    m_stats.allocatedCapacity = allocatedCapacity;
    m_stats.freeRangeCount = static_cast<uint32_t>(m_freeRanges.size());
    m_stats.largestFreeRange = largestFree;
    m_stats.pendingRetiredRangeCount = static_cast<uint32_t>(m_retiredRanges.size());
    m_stats.pendingRetiredCapacity = pendingRetiredCapacity;
}

} // namespace VENPOD::Simulation
