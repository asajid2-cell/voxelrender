#include "SparseBrickPool.h"

#include <algorithm>
#include <vector>

namespace VENPOD::Simulation {

namespace {

bool RequiresPublishedPageTableEntry(BrickLifecycleState state) {
    return state == BrickLifecycleState::Resident ||
           state == BrickLifecycleState::DirtyCPU ||
           state == BrickLifecycleState::DirtyGPU;
}

} // namespace

bool SparseBrickPool::Initialize(uint32_t maxPages, uint32_t pageTableCapacity) {
    if (maxPages == 0 || pageTableCapacity < maxPages * 2u) {
        return false;
    }

    m_records.assign(maxPages, BrickResidentRecord{});
    m_freePages.clear();
    m_resident.clear();
    m_dirtyQueue.clear();

    try {
        m_pageTable.Reset(pageTableCapacity);
    } catch (...) {
        return false;
    }

    for (uint32_t page = 0; page < maxPages; ++page) {
        m_records[page].pageIndex = page;
        m_records[page].state = BrickLifecycleState::Missing;
        m_freePages.push_back(page);
    }

    return true;
}

uint32_t SparseBrickPool::AllocatePage(const BrickCoord& coord) {
    auto existing = m_resident.find(coord);
    if (existing != m_resident.end()) {
        return existing->second;
    }

    if (m_freePages.empty()) {
        return INVALID_BRICK_PAGE;
    }

    const uint32_t page = m_freePages.front();
    m_freePages.pop_front();

    BrickResidentRecord& record = m_records[page];
    const uint32_t nextGeneration = record.generation + 1u;
    record = BrickResidentRecord{};
    record.coord = coord;
    record.pageIndex = page;
    record.generation = nextGeneration == 0 ? 1u : nextGeneration;
    record.state = BrickLifecycleState::Requested;
    record.dirtyCpu = true;
    record.dirtyGpu = true;

    m_resident.emplace(coord, page);
    m_dirtyQueue.push_back(coord);
    return page;
}

bool SparseBrickPool::FreePage(const BrickCoord& coord) {
    return Evict(coord);
}

bool SparseBrickPool::MarkGeneratingCPU(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    return TransitionRecord(m_records[it->second], BrickLifecycleState::GeneratingCPU);
}

bool SparseBrickPool::MarkGeneratedCPU(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    return TransitionRecord(m_records[it->second], BrickLifecycleState::GeneratedCPU);
}

bool SparseBrickPool::QueueUpload(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    BrickResidentRecord& record = m_records[it->second];
    if (!TransitionRecord(record, BrickLifecycleState::UploadQueued)) {
        return false;
    }
    record.dirtyGpu = true;
    return true;
}

bool SparseBrickPool::BeginUpload(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    return TransitionRecord(m_records[it->second], BrickLifecycleState::UploadingGPU);
}

bool SparseBrickPool::AbortUpload(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    BrickResidentRecord& record = m_records[it->second];
    if (!TransitionRecord(record, BrickLifecycleState::UploadQueued)) {
        return false;
    }
    record.dirtyGpu = true;
    return true;
}

bool SparseBrickPool::PublishResident(
    const BrickCoord& coord,
    uint32_t flags,
    uint32_t occupancyWord0,
    uint32_t occupancyWord1)
{
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }

    BrickResidentRecord& record = m_records[it->second];
    if (!TransitionRecord(record, BrickLifecycleState::Resident)) {
        return false;
    }

    if (!m_pageTable.InsertOrAssign(
            coord,
            record.pageIndex,
            record.generation,
            flags,
            occupancyWord0,
            occupancyWord1)) {
        record.state = BrickLifecycleState::UploadingGPU;
        return false;
    }

    record.dirtyCpu = false;
    record.dirtyGpu = false;
    return true;
}

bool SparseBrickPool::QueueEviction(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    return TransitionRecord(m_records[it->second], BrickLifecycleState::EvictQueued);
}

bool SparseBrickPool::Evict(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }

    const uint32_t page = it->second;
    BrickResidentRecord& record = m_records[page];
    if (record.state != BrickLifecycleState::EvictQueued) {
        if (!TransitionRecord(record, BrickLifecycleState::EvictQueued)) {
            return false;
        }
    }

    // Invalidate visibility before the physical page is returned to the free list.
    m_pageTable.Remove(coord);
    m_resident.erase(it);

    const uint32_t generation = record.generation;
    m_records[page] = BrickResidentRecord{};
    m_records[page].pageIndex = page;
    m_records[page].generation = generation;
    m_records[page].state = BrickLifecycleState::Evicted;
    m_freePages.push_back(page);
    return true;
}

bool SparseBrickPool::GetRecord(const BrickCoord& coord, BrickResidentRecord* outRecord) const {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    if (outRecord) {
        *outRecord = m_records[it->second];
    }
    return true;
}

BrickLifecycleState SparseBrickPool::GetState(const BrickCoord& coord) const {
    BrickResidentRecord record;
    if (!GetRecord(coord, &record)) {
        return BrickLifecycleState::Missing;
    }
    return record.state;
}

bool SparseBrickPool::IsResident(const BrickCoord& coord) const {
    uint32_t page = INVALID_BRICK_PAGE;
    return TryGetResidentPage(coord, &page);
}

bool SparseBrickPool::TryGetPage(const BrickCoord& coord, uint32_t* outPageIndex) const {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }

    if (outPageIndex) {
        *outPageIndex = it->second;
    }
    return true;
}

bool SparseBrickPool::TryGetResidentPage(const BrickCoord& coord, uint32_t* outPageIndex) const {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }

    const BrickResidentRecord& record = m_records[it->second];
    if (record.state != BrickLifecycleState::Resident) {
        return false;
    }

    uint32_t page = INVALID_BRICK_PAGE;
    if (!m_pageTable.TryLookupExactGeneration(coord, record.generation, &page, nullptr)) {
        return false;
    }

    if (page != record.pageIndex) {
        return false;
    }

    if (outPageIndex) {
        *outPageIndex = page;
    }
    return true;
}

void SparseBrickPool::Touch(const BrickCoord& coord, uint32_t frameIndex) {
    uint32_t page = INVALID_BRICK_PAGE;
    if (TryGetPage(coord, &page)) {
        m_records[page].lastTouchedFrame = frameIndex;
    }
}

bool SparseBrickPool::MarkResidencyClass(const BrickCoord& coord, SparseResidencyClass residencyClass) {
    return TouchResidencyClass(coord, residencyClass, 0);
}

bool SparseBrickPool::TouchResidencyClass(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex)
{
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }

    BrickResidentRecord& record = m_records[it->second];
    if (frameIndex != 0) {
        record.lastTouchedFrame = std::max(record.lastTouchedFrame, frameIndex);
    }
    if (static_cast<uint8_t>(residencyClass) > static_cast<uint8_t>(record.residencyClass)) {
        record.residencyClass = residencyClass;
    }

    switch (residencyClass) {
        case SparseResidencyClass::Edited:
            record.lastEditedFrame = std::max(record.lastEditedFrame, frameIndex);
            break;
        case SparseResidencyClass::Collision:
            record.lastCollisionFrame = std::max(record.lastCollisionFrame, frameIndex);
            break;
        case SparseResidencyClass::Visible:
            record.lastVisibleFrame = std::max(record.lastVisibleFrame, frameIndex);
            break;
        case SparseResidencyClass::Speculative:
        default:
            record.lastSpeculativeFrame = std::max(record.lastSpeculativeFrame, frameIndex);
            break;
    }
    return true;
}

bool SparseBrickPool::MarkDirty(const BrickCoord& coord) {
    uint32_t page = INVALID_BRICK_PAGE;
    if (!TryGetPage(coord, &page)) {
        return false;
    }

    BrickResidentRecord& record = m_records[page];
    if (record.state == BrickLifecycleState::Resident) {
        if (!TransitionRecord(record, BrickLifecycleState::DirtyCPU)) {
            return false;
        }
    }
    record.dirtyCpu = true;
    record.dirtyGpu = true;
    m_dirtyQueue.push_back(coord);
    return true;
}

bool SparseBrickPool::MarkHasPersistentEdits(const BrickCoord& coord) {
    auto it = m_resident.find(coord);
    if (it == m_resident.end()) {
        return false;
    }
    m_records[it->second].hasPersistentEdits = true;
    return true;
}

SparseBrickPoolValidationResult SparseBrickPool::ValidateInvariants() const {
    SparseBrickPoolValidationResult result;
    result.freePages = static_cast<uint32_t>(m_freePages.size());

    std::vector<bool> freeSeen(m_records.size(), false);
    for (uint32_t page : m_freePages) {
        if (page >= m_records.size() || freeSeen[page]) {
            ++result.freeListErrors;
            result.ok = false;
            continue;
        }
        freeSeen[page] = true;
        const BrickLifecycleState state = m_records[page].state;
        if (state != BrickLifecycleState::Missing &&
            state != BrickLifecycleState::Evicted) {
            ++result.freeListErrors;
            result.ok = false;
        }
    }

    for (const auto& mapped : m_resident) {
        const BrickCoord& coord = mapped.first;
        const uint32_t page = mapped.second;
        if (page >= m_records.size() || freeSeen[page]) {
            ++result.residentMapErrors;
            result.ok = false;
            continue;
        }

        const BrickResidentRecord& record = m_records[page];
        if (record.pageIndex != page ||
            !(record.coord == coord) ||
            record.state == BrickLifecycleState::Missing ||
            record.state == BrickLifecycleState::Evicted) {
            ++result.residentMapErrors;
            result.ok = false;
            continue;
        }
        ++result.activeRecords;
    }

    for (uint32_t page = 0; page < m_records.size(); ++page) {
        if (freeSeen[page]) {
            continue;
        }
        const BrickResidentRecord& record = m_records[page];
        if (record.state == BrickLifecycleState::Missing ||
            record.state == BrickLifecycleState::Evicted) {
            continue;
        }
        auto it = m_resident.find(record.coord);
        if (it == m_resident.end() || it->second != page) {
            ++result.residentMapErrors;
            result.ok = false;
        }

        if (RequiresPublishedPageTableEntry(record.state)) {
            uint32_t pageTablePage = INVALID_BRICK_PAGE;
            if (!m_pageTable.TryLookupExactGeneration(
                    record.coord,
                    record.generation,
                    &pageTablePage,
                    nullptr) ||
                pageTablePage != page) {
                ++result.pageTableErrors;
                ++result.missingPublishedPageTableEntries;
                result.ok = false;
            }
        }
    }

    uint32_t countedPageTableEntries = 0;
    for (const BrickPageEntry& entry : m_pageTable.Entries()) {
        if (entry.pageIndex == INVALID_BRICK_PAGE ||
            entry.pageIndex == INVALID_BRICK_PAGE - 1u) {
            continue;
        }
        ++countedPageTableEntries;
        auto it = m_resident.find(entry.coord);
        if (it == m_resident.end() ||
            it->second >= m_records.size()) {
            ++result.pageTableErrors;
            result.ok = false;
            continue;
        }
        const BrickResidentRecord& record = m_records[it->second];
        if (record.pageIndex != entry.pageIndex ||
            record.generation != entry.generation) {
            ++result.pageTableErrors;
            result.ok = false;
            continue;
        }

        const bool stateCanHavePublishedPage =
            record.state == BrickLifecycleState::Resident ||
            record.state == BrickLifecycleState::DirtyCPU ||
            record.state == BrickLifecycleState::DirtyGPU ||
            record.state == BrickLifecycleState::UploadQueued ||
            record.state == BrickLifecycleState::UploadingGPU;
        if (!stateCanHavePublishedPage) {
            ++result.pageTableErrors;
            result.ok = false;
        }
    }
    result.pageTableEntries = countedPageTableEntries;
    if (countedPageTableEntries != m_pageTable.Count()) {
        ++result.pageTableErrors;
        result.ok = false;
    }

    return result;
}

bool SparseBrickPool::TransitionRecord(BrickResidentRecord& record, BrickLifecycleState nextState) {
    if (!IsValidLifecycleTransition(record.state, nextState)) {
        return false;
    }
    record.state = nextState;
    return true;
}

} // namespace VENPOD::Simulation
