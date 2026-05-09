#pragma once

#include "SparsePageTable.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace VENPOD::Simulation {

struct SparseBrickPoolValidationResult {
    bool ok = true;
    uint32_t activeRecords = 0;
    uint32_t freePages = 0;
    uint32_t pageTableEntries = 0;
    uint32_t residentMapErrors = 0;
    uint32_t freeListErrors = 0;
    uint32_t pageTableErrors = 0;
    uint32_t missingPublishedPageTableEntries = 0;
};

class SparseBrickPool {
public:
    bool Initialize(uint32_t maxPages, uint32_t pageTableCapacity);

    uint32_t AllocatePage(const BrickCoord& coord);
    bool FreePage(const BrickCoord& coord);
    bool MarkGeneratingCPU(const BrickCoord& coord);
    bool MarkGeneratedCPU(const BrickCoord& coord);
    bool QueueUpload(const BrickCoord& coord);
    bool BeginUpload(const BrickCoord& coord);
    bool AbortUpload(const BrickCoord& coord);
    bool PublishResident(
        const BrickCoord& coord,
        uint32_t flags = 0,
        uint32_t occupancyWord0 = 0,
        uint32_t occupancyWord1 = 0);
    bool QueueEviction(const BrickCoord& coord);
    bool Evict(const BrickCoord& coord);
    bool GetRecord(const BrickCoord& coord, BrickResidentRecord* outRecord = nullptr) const;
    BrickLifecycleState GetState(const BrickCoord& coord) const;
    bool IsResident(const BrickCoord& coord) const;
    bool TryGetPage(const BrickCoord& coord, uint32_t* outPageIndex = nullptr) const;
    bool TryGetResidentPage(const BrickCoord& coord, uint32_t* outPageIndex = nullptr) const;
    void Touch(const BrickCoord& coord, uint32_t frameIndex);
    bool MarkResidencyClass(const BrickCoord& coord, SparseResidencyClass residencyClass);
    bool TouchResidencyClass(
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex);
    bool MarkDirty(const BrickCoord& coord);
    bool MarkHasPersistentEdits(const BrickCoord& coord);

    uint32_t MaxPages() const { return static_cast<uint32_t>(m_records.size()); }
    uint32_t ResidentCount() const { return static_cast<uint32_t>(m_resident.size()); }
    uint32_t FreePageCount() const { return static_cast<uint32_t>(m_freePages.size()); }
    const SparsePageTable& PageTable() const { return m_pageTable; }
    const std::vector<BrickResidentRecord>& Records() const { return m_records; }
    SparseBrickPoolValidationResult ValidateInvariants() const;

private:
    bool TransitionRecord(BrickResidentRecord& record, BrickLifecycleState nextState);

    std::vector<BrickResidentRecord> m_records;
    std::deque<uint32_t> m_freePages;
    std::unordered_map<BrickCoord, uint32_t, BrickCoordHash> m_resident;
    std::deque<BrickCoord> m_dirtyQueue;
    SparsePageTable m_pageTable;
};

} // namespace VENPOD::Simulation
