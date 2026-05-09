#pragma once

#include "SparseVoxelTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace VENPOD::Simulation {

struct SparsePendingPageTablePublish {
    uint32_t entryIndex = UINT32_MAX;
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    uint32_t readyFrame = 0;
    uint64_t readyFenceValue = 0;
    SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
};

enum class SparsePagePublishQueueEvent : uint8_t {
    IgnoredInvalid,
    Queued,
    QueuedEdited,
    Replaced,
    PromotedEdited
};

struct SparsePagePublishQueueStats {
    size_t total = 0;
    size_t ready = 0;
    size_t waitingFrame = 0;
    size_t waitingFence = 0;
    size_t edited = 0;
    uint32_t maxReadyFrameLag = 0;
};

enum class SparseDelayedInvalidationDecision : uint8_t {
    Stage,
    SkipAlreadyReplaced
};

struct SparseDelayedInvalidationInput {
    const BrickPageEntry* cpuEntries = nullptr;
    size_t cpuEntryCount = 0;
    uint32_t entryIndex = UINT32_MAX;
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    bool replacementPublishPending = false;
};

SparseDelayedInvalidationDecision DecideSparseDelayedInvalidation(
    const SparseDelayedInvalidationInput& input);

class SparsePagePublishQueue {
public:
    SparsePagePublishQueueEvent Enqueue(
        uint32_t entryIndex,
        const BrickCoord& coord,
        uint32_t pageIndex,
        uint32_t generation,
        uint32_t readyFrame,
        uint64_t readyFenceValue,
        SparseResidencyClass residencyClass);

    bool PopReady(
        uint32_t currentFrame,
        uint64_t completedFenceValue,
        SparsePendingPageTablePublish* outPublish);
    void RequeueFront(const SparsePendingPageTablePublish& publish);
    void Clear();

    bool Empty() const { return m_queue.empty(); }
    size_t Size() const { return m_queue.size(); }
    bool ContainsEntry(uint32_t entryIndex) const;
    size_t ReadyCount(uint32_t currentFrame, uint64_t completedFenceValue) const;
    SparsePagePublishQueueStats GetStats(uint32_t currentFrame, uint64_t completedFenceValue) const;

private:
    std::deque<SparsePendingPageTablePublish> m_queue;
    std::unordered_set<uint32_t> m_entrySet;

    static bool IsValidPublish(const SparsePendingPageTablePublish& publish);
    static bool IsReady(
        const SparsePendingPageTablePublish& publish,
        uint32_t currentFrame,
        uint64_t completedFenceValue);
};

} // namespace VENPOD::Simulation
