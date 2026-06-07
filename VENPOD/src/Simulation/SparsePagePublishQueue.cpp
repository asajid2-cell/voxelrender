#include "SparsePagePublishQueue.h"

#include <algorithm>

namespace VENPOD::Simulation {

namespace {

SparseStreamingLane DefaultPublishStreamingLane(SparseResidencyClass residencyClass) {
    switch (residencyClass) {
        case SparseResidencyClass::Edited:
        case SparseResidencyClass::Collision:
            return SparseStreamingLane::PublicCritical;
        case SparseResidencyClass::Visible:
            return SparseStreamingLane::Visible;
        case SparseResidencyClass::Speculative:
        default:
            return SparseStreamingLane::Cache;
    }
}

SparseStreamingLane MaxStreamingLane(SparseStreamingLane a, SparseStreamingLane b) {
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

void AddLaneCount(
    SparseStreamingLane lane,
    size_t& cache,
    size_t& prefetch,
    size_t& repair,
    size_t& visible,
    size_t& publicCritical) {
    switch (lane) {
        case SparseStreamingLane::PublicCritical:
            ++publicCritical;
            break;
        case SparseStreamingLane::Visible:
            ++visible;
            break;
        case SparseStreamingLane::Repair:
            ++repair;
            break;
        case SparseStreamingLane::Prefetch:
            ++prefetch;
            break;
        case SparseStreamingLane::Cache:
        default:
            ++cache;
            break;
    }
}

} // namespace

SparseDelayedInvalidationDecision DecideSparseDelayedInvalidation(
    const SparseDelayedInvalidationInput& input)
{
    if (!input.cpuEntries || input.entryIndex >= input.cpuEntryCount) {
        return SparseDelayedInvalidationDecision::Stage;
    }

    const BrickPageEntry& cpuEntry = input.cpuEntries[input.entryIndex];
    const bool cpuSlotValid =
        cpuEntry.pageIndex != INVALID_BRICK_PAGE &&
        cpuEntry.pageIndex != INVALID_BRICK_PAGE - 1u;
    const bool cpuStillMatchesOld =
        cpuSlotValid &&
        cpuEntry.coord == input.coord &&
        cpuEntry.pageIndex == input.pageIndex &&
        cpuEntry.generation == input.generation;

    // Once the CPU slot has been reused, the old invalidation no longer owns
    // this page-table entry. Staging it can transiently wipe the replacement
    // entry on the GPU and make a CPU-ready brick look missing to the shader.
    if (cpuSlotValid && !cpuStillMatchesOld) {
        return SparseDelayedInvalidationDecision::SkipAlreadyReplaced;
    }

    return SparseDelayedInvalidationDecision::Stage;
}

bool SparsePagePublishQueue::IsValidPublish(const SparsePendingPageTablePublish& publish) {
    return publish.entryIndex != UINT32_MAX &&
        publish.pageIndex != INVALID_BRICK_PAGE &&
        publish.pageIndex != INVALID_BRICK_PAGE - 1u &&
        publish.generation != 0u;
}

bool SparsePagePublishQueue::IsReady(
    const SparsePendingPageTablePublish& publish,
    uint32_t currentFrame,
    uint64_t completedFenceValue)
{
    if (currentFrame < publish.readyFrame) {
        return false;
    }
    return publish.readyFenceValue == 0u ||
        completedFenceValue >= publish.readyFenceValue;
}

bool SparsePagePublishQueue::IsOwnershipCritical(const SparsePendingPageTablePublish& publish)
{
    if (publish.residencyClass == SparseResidencyClass::Edited ||
        publish.residencyClass == SparseResidencyClass::Collision) {
        return true;
    }
    return publish.streamingLane == SparseStreamingLane::PublicCritical ||
        publish.streamingLane == SparseStreamingLane::Visible;
}

SparsePagePublishQueueEvent SparsePagePublishQueue::Enqueue(
    uint32_t entryIndex,
    const BrickCoord& coord,
    uint32_t pageIndex,
    uint32_t generation,
    uint32_t readyFrame,
    uint64_t readyFenceValue,
    SparseResidencyClass residencyClass)
{
    return Enqueue(
        entryIndex,
        coord,
        pageIndex,
        generation,
        readyFrame,
        readyFenceValue,
        residencyClass,
        DefaultPublishStreamingLane(residencyClass));
}

SparsePagePublishQueueEvent SparsePagePublishQueue::Enqueue(
    uint32_t entryIndex,
    const BrickCoord& coord,
    uint32_t pageIndex,
    uint32_t generation,
    uint32_t readyFrame,
    uint64_t readyFenceValue,
    SparseResidencyClass residencyClass,
    SparseStreamingLane streamingLane)
{
    SparsePendingPageTablePublish pending;
    pending.entryIndex = entryIndex;
    pending.coord = coord;
    pending.pageIndex = pageIndex;
    pending.generation = generation;
    pending.readyFrame = readyFrame;
    pending.readyFenceValue = readyFenceValue;
    pending.residencyClass = residencyClass;
    pending.streamingLane = streamingLane;
    if (!IsValidPublish(pending)) {
        return SparsePagePublishQueueEvent::IgnoredInvalid;
    }

    if (m_entrySet.insert(entryIndex).second) {
        if (residencyClass == SparseResidencyClass::Edited) {
            m_queue.push_front(pending);
            return SparsePagePublishQueueEvent::QueuedEdited;
        }
        m_queue.push_back(pending);
        return SparsePagePublishQueueEvent::Queued;
    }

    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->entryIndex != entryIndex) {
            continue;
        }

        if (pending.generation < it->generation) {
            return SparsePagePublishQueueEvent::IgnoredStale;
        }
        if (pending.generation == it->generation &&
            (!(pending.coord == it->coord) || pending.pageIndex != it->pageIndex)) {
            return SparsePagePublishQueueEvent::IgnoredStale;
        }

        const bool wasEdited = it->residencyClass == SparseResidencyClass::Edited;
        const bool isEdited = residencyClass == SparseResidencyClass::Edited;
        pending.residencyClass = (wasEdited || isEdited)
            ? SparseResidencyClass::Edited
            : residencyClass;
        pending.streamingLane = MaxStreamingLane(it->streamingLane, pending.streamingLane);
        if (pending.residencyClass == SparseResidencyClass::Edited) {
            pending.streamingLane = SparseStreamingLane::PublicCritical;
        }
        m_queue.erase(it);
        if (pending.residencyClass == SparseResidencyClass::Edited) {
            m_queue.push_front(pending);
            return isEdited && !wasEdited
                ? SparsePagePublishQueueEvent::PromotedEdited
                : SparsePagePublishQueueEvent::Replaced;
        }
        m_queue.push_back(pending);
        return SparsePagePublishQueueEvent::Replaced;
    }

    m_entrySet.erase(entryIndex);
    return Enqueue(
        entryIndex,
        coord,
        pageIndex,
        generation,
        readyFrame,
        readyFenceValue,
        residencyClass,
        streamingLane);
}

bool SparsePagePublishQueue::PopReady(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    SparsePendingPageTablePublish* outPublish)
{
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (!IsReady(*it, currentFrame, completedFenceValue)) {
            continue;
        }
        SparsePendingPageTablePublish publish = *it;
        m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublish) {
            *outPublish = publish;
        }
        return true;
    }
    return false;
}

bool SparsePagePublishQueue::PopReadyOfClass(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    SparseResidencyClass residencyClass,
    SparsePendingPageTablePublish* outPublish)
{
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->residencyClass != residencyClass ||
            !IsReady(*it, currentFrame, completedFenceValue)) {
            continue;
        }
        SparsePendingPageTablePublish publish = *it;
        m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublish) {
            *outPublish = publish;
        }
        return true;
    }
    return false;
}

bool SparsePagePublishQueue::PopReadyForCoord(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    const BrickCoord& coord,
    SparsePendingPageTablePublish* outPublish)
{
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (!(it->coord == coord) ||
            !IsReady(*it, currentFrame, completedFenceValue)) {
            continue;
        }
        SparsePendingPageTablePublish publish = *it;
        m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublish) {
            *outPublish = publish;
        }
        return true;
    }
    return false;
}

bool SparsePagePublishQueue::PopReadyForAnyCoord(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    const std::unordered_set<BrickCoord, BrickCoordHash>& coords,
    SparsePendingPageTablePublish* outPublish)
{
    if (coords.empty()) {
        return false;
    }
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (coords.find(it->coord) == coords.end() ||
            !IsReady(*it, currentFrame, completedFenceValue)) {
            continue;
        }
        SparsePendingPageTablePublish publish = *it;
        m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublish) {
            *outPublish = publish;
        }
        return true;
    }
    return false;
}

bool SparsePagePublishQueue::PopReadyForOwnershipCritical(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    bool ownershipCritical,
    SparsePendingPageTablePublish* outPublish)
{
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (IsOwnershipCritical(*it) != ownershipCritical ||
            !IsReady(*it, currentFrame, completedFenceValue)) {
            continue;
        }
        SparsePendingPageTablePublish publish = *it;
        m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublish) {
            *outPublish = publish;
        }
        return true;
    }
    return false;
}

uint32_t SparsePagePublishQueue::PopReadyBatchForAnyCoord(
    uint32_t currentFrame,
    uint64_t completedFenceValue,
    const std::unordered_set<BrickCoord, BrickCoordHash>& coords,
    uint32_t maxCount,
    std::vector<SparsePendingPageTablePublish>* outPublishes)
{
    if (coords.empty() || maxCount == 0u) {
        return 0u;
    }

    uint32_t popped = 0;
    for (auto it = m_queue.begin(); it != m_queue.end() && popped < maxCount; ) {
        if (coords.find(it->coord) == coords.end() ||
            !IsReady(*it, currentFrame, completedFenceValue)) {
            ++it;
            continue;
        }

        SparsePendingPageTablePublish publish = *it;
        it = m_queue.erase(it);
        m_entrySet.erase(publish.entryIndex);
        if (outPublishes) {
            outPublishes->push_back(publish);
        }
        ++popped;
    }
    return popped;
}

void SparsePagePublishQueue::RequeueFront(const SparsePendingPageTablePublish& publish) {
    if (!IsValidPublish(publish)) {
        return;
    }
    if (!m_entrySet.insert(publish.entryIndex).second) {
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->entryIndex == publish.entryIndex) {
                if (!(publish.coord == it->coord) || publish.pageIndex != it->pageIndex) {
                    return;
                }
                if (publish.pageIndex == it->pageIndex &&
                    publish.generation < it->generation) {
                    return;
                }
                m_queue.erase(it);
                break;
            }
        }
    }
    m_queue.push_front(publish);
}

void SparsePagePublishQueue::RequeueBack(const SparsePendingPageTablePublish& publish) {
    if (!IsValidPublish(publish)) {
        return;
    }
    if (!m_entrySet.insert(publish.entryIndex).second) {
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->entryIndex == publish.entryIndex) {
                if (!(publish.coord == it->coord) || publish.pageIndex != it->pageIndex) {
                    return;
                }
                if (publish.pageIndex == it->pageIndex &&
                    publish.generation < it->generation) {
                    return;
                }
                m_queue.erase(it);
                break;
            }
        }
    }
    m_queue.push_back(publish);
}

void SparsePagePublishQueue::Clear() {
    m_queue.clear();
    m_entrySet.clear();
}

bool SparsePagePublishQueue::ContainsEntry(uint32_t entryIndex) const {
    return m_entrySet.find(entryIndex) != m_entrySet.end();
}

size_t SparsePagePublishQueue::ReadyCount(uint32_t currentFrame, uint64_t completedFenceValue) const {
    size_t count = 0;
    for (const SparsePendingPageTablePublish& publish : m_queue) {
        if (IsReady(publish, currentFrame, completedFenceValue)) {
            ++count;
        }
    }
    return count;
}

SparsePagePublishQueueStats SparsePagePublishQueue::GetStats(
    uint32_t currentFrame,
    uint64_t completedFenceValue) const
{
    SparsePagePublishQueueStats stats;
    stats.total = m_queue.size();
    for (const SparsePendingPageTablePublish& publish : m_queue) {
        AddLaneCount(
            publish.streamingLane,
            stats.laneCache,
            stats.lanePrefetch,
            stats.laneRepair,
            stats.laneVisible,
            stats.lanePublicCritical);
        if (publish.residencyClass == SparseResidencyClass::Edited) {
            ++stats.edited;
        }
        if (currentFrame < publish.readyFrame) {
            ++stats.waitingFrame;
            continue;
        }
        if (publish.readyFenceValue != 0u &&
            completedFenceValue < publish.readyFenceValue) {
            ++stats.waitingFence;
            continue;
        }
        ++stats.ready;
        AddLaneCount(
                publish.streamingLane,
                stats.readyLaneCache,
                stats.readyLanePrefetch,
                stats.readyLaneRepair,
                stats.readyLaneVisible,
                stats.readyLanePublicCritical);
        stats.maxReadyFrameLag = std::max(
            stats.maxReadyFrameLag,
            currentFrame - publish.readyFrame);
    }
    return stats;
}

} // namespace VENPOD::Simulation
