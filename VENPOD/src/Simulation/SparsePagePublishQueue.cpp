#include "SparsePagePublishQueue.h"

#include <algorithm>

namespace VENPOD::Simulation {

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

    if (cpuSlotValid && !cpuStillMatchesOld && !input.replacementPublishPending) {
        return SparseDelayedInvalidationDecision::SkipAlreadyReplaced;
    }

    return SparseDelayedInvalidationDecision::Stage;
}

bool SparsePagePublishQueue::IsValidPublish(const SparsePendingPageTablePublish& publish) {
    return publish.entryIndex != UINT32_MAX &&
        publish.pageIndex != INVALID_BRICK_PAGE &&
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

SparsePagePublishQueueEvent SparsePagePublishQueue::Enqueue(
    uint32_t entryIndex,
    const BrickCoord& coord,
    uint32_t pageIndex,
    uint32_t generation,
    uint32_t readyFrame,
    uint64_t readyFenceValue,
    SparseResidencyClass residencyClass)
{
    SparsePendingPageTablePublish pending;
    pending.entryIndex = entryIndex;
    pending.coord = coord;
    pending.pageIndex = pageIndex;
    pending.generation = generation;
    pending.readyFrame = readyFrame;
    pending.readyFenceValue = readyFenceValue;
    pending.residencyClass = residencyClass;
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

        const bool wasEdited = it->residencyClass == SparseResidencyClass::Edited;
        const bool isEdited = residencyClass == SparseResidencyClass::Edited;
        pending.residencyClass = (wasEdited || isEdited)
            ? SparseResidencyClass::Edited
            : residencyClass;
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
        residencyClass);
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

void SparsePagePublishQueue::RequeueFront(const SparsePendingPageTablePublish& publish) {
    if (!IsValidPublish(publish)) {
        return;
    }
    if (!m_entrySet.insert(publish.entryIndex).second) {
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->entryIndex == publish.entryIndex) {
                m_queue.erase(it);
                break;
            }
        }
    }
    m_queue.push_front(publish);
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
        stats.maxReadyFrameLag = std::max(
            stats.maxReadyFrameLag,
            currentFrame - publish.readyFrame);
    }
    return stats;
}

} // namespace VENPOD::Simulation
