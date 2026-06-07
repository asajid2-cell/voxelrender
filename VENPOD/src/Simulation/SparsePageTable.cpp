#include "SparsePageTable.h"

#include <algorithm>
#include <stdexcept>

namespace VENPOD::Simulation {

SparsePageTable::SparsePageTable(uint32_t capacity) {
    if (capacity > 0) {
        Reset(capacity);
    }
}

void SparsePageTable::Reset(uint32_t capacity) {
    if (!IsPowerOfTwo(capacity)) {
        throw std::invalid_argument("SparsePageTable capacity must be a power of two");
    }

    m_entries.assign(capacity, BrickPageEntry{});
    for (auto& entry : m_entries) {
        entry.pageIndex = EMPTY_PAGE;
    }
    m_count = 0;
    m_maxProbeCount = 0;
}

bool SparsePageTable::InsertOrAssign(
    const BrickCoord& coord,
    uint32_t pageIndex,
    uint32_t generation,
    uint32_t flags,
    uint32_t occupancyWord0,
    uint32_t occupancyWord1)
{
    if (pageIndex == EMPTY_PAGE || pageIndex == TOMBSTONE_PAGE || m_entries.empty()) {
        return false;
    }
    if (generation == 0) {
        return false;
    }

    bool found = false;
    const uint32_t slot = FindSlot(coord, true, &found);
    if (slot == UINT32_MAX) {
        return false;
    }

    if (!found) {
        if ((m_count + 1u) * 100u / Capacity() > 70u) {
            return false;
        }
        ++m_count;
    }

    m_entries[slot].coord = coord;
    m_entries[slot].pageIndex = pageIndex;
    m_entries[slot].generation = generation;
    m_entries[slot].flags = flags;
    m_entries[slot].occupancyWord0 = occupancyWord0;
    m_entries[slot].occupancyWord1 = occupancyWord1;
    return true;
}

bool SparsePageTable::Remove(const BrickCoord& coord) {
    if (m_entries.empty()) {
        return false;
    }

    bool found = false;
    const uint32_t slot = FindSlot(coord, false, &found);
    if (!found || slot == UINT32_MAX) {
        return false;
    }

    m_entries[slot] = BrickPageEntry{};
    m_entries[slot].pageIndex = TOMBSTONE_PAGE;
    --m_count;
    return true;
}

bool SparsePageTable::TryLookup(
    const BrickCoord& coord,
    uint32_t* outPageIndex,
    uint32_t* outFlags,
    uint32_t* outGeneration) const
{
    if (m_entries.empty()) {
        return false;
    }

    bool found = false;
    const uint32_t slot = FindSlot(coord, false, &found);
    if (!found || slot == UINT32_MAX) {
        return false;
    }

    if (outPageIndex) {
        *outPageIndex = m_entries[slot].pageIndex;
    }
    if (outFlags) {
        *outFlags = m_entries[slot].flags;
    }
    if (outGeneration) {
        *outGeneration = m_entries[slot].generation;
    }
    return true;
}

bool SparsePageTable::TryLookupExactGeneration(
    const BrickCoord& coord,
    uint32_t expectedGeneration,
    uint32_t* outPageIndex,
    uint32_t* outFlags) const
{
    uint32_t generation = 0;
    if (!TryLookup(coord, outPageIndex, outFlags, &generation)) {
        return false;
    }
    return generation == expectedGeneration;
}

bool SparsePageTable::TryGetEntryIndex(const BrickCoord& coord, uint32_t* outEntryIndex) const {
    if (m_entries.empty() || !outEntryIndex) {
        return false;
    }

    bool found = false;
    const uint32_t slot = FindSlot(coord, false, &found);
    if (!found || slot == UINT32_MAX) {
        return false;
    }

    *outEntryIndex = slot;
    return true;
}

uint32_t SparsePageTable::FindSlot(const BrickCoord& coord, bool forInsert, bool* outFound) const {
    *outFound = false;
    if (m_entries.empty()) {
        return UINT32_MAX;
    }

    const uint32_t mask = Capacity() - 1u;
    const uint32_t start = HashBrickCoord32(coord) & mask;
    uint32_t firstTombstone = UINT32_MAX;

    for (uint32_t probe = 0; probe < Capacity(); ++probe) {
        const uint32_t slot = (start + probe) & mask;
        const BrickPageEntry& entry = m_entries[slot];

        if (entry.pageIndex == EMPTY_PAGE) {
            m_maxProbeCount = std::max(m_maxProbeCount, probe + 1u);
            if (forInsert && firstTombstone != UINT32_MAX) {
                return firstTombstone;
            }
            return forInsert ? slot : UINT32_MAX;
        }

        if (entry.pageIndex == TOMBSTONE_PAGE) {
            if (forInsert && firstTombstone == UINT32_MAX) {
                firstTombstone = slot;
            }
            continue;
        }

        if (entry.coord == coord) {
            *outFound = true;
            m_maxProbeCount = std::max(m_maxProbeCount, probe + 1u);
            return slot;
        }
    }

    if (forInsert && firstTombstone != UINT32_MAX) {
        return firstTombstone;
    }
    return UINT32_MAX;
}

bool SparsePageTable::IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

} // namespace VENPOD::Simulation
