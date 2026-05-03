#pragma once

#include "SparseVoxelTypes.h"

#include <cstdint>
#include <vector>

namespace VENPOD::Simulation {

class SparsePageTable {
public:
    explicit SparsePageTable(uint32_t capacity = 0);

    void Reset(uint32_t capacity);
    bool InsertOrAssign(
        const BrickCoord& coord,
        uint32_t pageIndex,
        uint32_t generation,
        uint32_t flags = 0,
        uint32_t occupancyWord0 = 0,
        uint32_t occupancyWord1 = 0);
    bool Remove(const BrickCoord& coord);
    bool TryLookup(
        const BrickCoord& coord,
        uint32_t* outPageIndex = nullptr,
        uint32_t* outFlags = nullptr,
        uint32_t* outGeneration = nullptr) const;
    bool TryLookupExactGeneration(
        const BrickCoord& coord,
        uint32_t expectedGeneration,
        uint32_t* outPageIndex = nullptr,
        uint32_t* outFlags = nullptr) const;
    bool TryGetEntryIndex(const BrickCoord& coord, uint32_t* outEntryIndex) const;

    uint32_t Capacity() const { return static_cast<uint32_t>(m_entries.size()); }
    uint32_t Count() const { return m_count; }
    uint32_t MaxProbeCount() const { return m_maxProbeCount; }
    const std::vector<BrickPageEntry>& Entries() const { return m_entries; }

private:
    static constexpr uint32_t EMPTY_PAGE = INVALID_BRICK_PAGE;
    static constexpr uint32_t TOMBSTONE_PAGE = INVALID_BRICK_PAGE - 1u;

    uint32_t FindSlot(const BrickCoord& coord, bool forInsert, bool* outFound) const;
    static bool IsPowerOfTwo(uint32_t value);

    std::vector<BrickPageEntry> m_entries;
    uint32_t m_count = 0;
    mutable uint32_t m_maxProbeCount = 0;
};

} // namespace VENPOD::Simulation
