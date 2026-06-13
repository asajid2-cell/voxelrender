#include "../Common/SharedTypes.hlsli"

// Live edit-overlay bake: each frame, write the staged brush edit deltas directly
// into the resident GPU brick voxel pool BEFORE the raymarch reads it, so paint/
// erase render with zero propagation latency and PS_Raymarch stays unchanged (it
// reads SparseBrickVoxelPool / SparseBrickOccupancy exactly as today). The CPU
// regen/upload path becomes the durable tier for edits that age out of the overlay.
//
// One thread per edit-delta RANGE (a range == one brick == one resident page), so
// each thread EXCLUSIVELY owns its page's voxels and occupancy word: no cross-thread
// races, no atomics needed. Within a range, deltas are ordered (packedLocal,
// revision ascending), so a sequential last-write-wins loop yields the latest edit
// per voxel.

struct ApplyEditDeltasConstants {
    uint rangeCount;
    uint deltaCount;
    uint pageTableCapacity;
    uint maxBrickPages;
};

ConstantBuffer<ApplyEditDeltasConstants> gConstants : register(b0);
StructuredBuffer<SparseEditDelta> gEditDeltas : register(t0);
StructuredBuffer<SparseEditDeltaRange> gEditDeltaRanges : register(t1);
StructuredBuffer<BrickPageEntry> gPageTable : register(t2);
StructuredBuffer<uint> gPageGenerations : register(t3);
RWStructuredBuffer<uint> gBrickPool : register(u0);
RWStructuredBuffer<uint2> gOccupancy : register(u1);

static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;
static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;

uint UnpackMaterial(uint voxel) {
    return voxel & 0xFFu;
}

uint3 UnpackLocal(uint packed) {
    return uint3(
        packed & 0xFFu,
        (packed >> 8) & 0xFFu,
        (packed >> 16) & 0xFFu);
}

uint SparseLocalIndex(uint3 localVoxel) {
    return localVoxel.x +
        localVoxel.y * SPARSE_BRICK_SIZE +
        localVoxel.z * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
}

uint HashSparseBrickCoord(int3 coord) {
    uint hash = 2166136261u;
    hash = (hash ^ (uint)coord.x) * 16777619u;
    hash = (hash ^ (uint)coord.y) * 16777619u;
    hash = (hash ^ (uint)coord.z) * 16777619u;
    return hash;
}

bool LookupSparseBrick(int3 brickCoord, out BrickPageEntry result) {
    result = (BrickPageEntry)0;
    const uint capacity = gConstants.pageTableCapacity;
    if (capacity == 0u || (capacity & (capacity - 1u)) != 0u) {
        return false;
    }
    const uint mask = capacity - 1u;
    const uint start = HashSparseBrickCoord(brickCoord) & mask;
    [loop]
    for (uint probe = 0u; probe < SPARSE_PAGE_TABLE_LOOKUP_PROBES; ++probe) {
        const uint slot = (start + probe) & mask;
        const BrickPageEntry entry = gPageTable[slot];
        if (entry.pageIndex == SPARSE_INVALID_PAGE) {
            return false;
        }
        if (entry.pageIndex == SPARSE_TOMBSTONE_PAGE) {
            continue;
        }
        if (all(entry.coord == brickCoord)) {
            result = entry;
            return entry.generation != 0u;
        }
    }
    return false;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint rangeIndex = dispatchThreadId.x;
    if (rangeIndex >= gConstants.rangeCount) {
        return;
    }

    const SparseEditDeltaRange range = gEditDeltaRanges[rangeIndex];

    BrickPageEntry entry;
    if (!LookupSparseBrick(range.brickCoord, entry)) {
        return; // brick not resident: durable regen owns it
    }
    if (entry.pageIndex >= gConstants.maxBrickPages ||
        entry.pageIndex == SPARSE_INVALID_PAGE ||
        entry.pageIndex == SPARSE_TOMBSTONE_PAGE) {
        return;
    }
    // Reject a stale page that has been reused for a different brick generation.
    if (gPageGenerations[entry.pageIndex] != entry.generation) {
        return;
    }

    const uint pageBase = entry.pageIndex * SPARSE_BRICK_VOXEL_COUNT;
    const uint firstDelta = min(range.firstDelta, gConstants.deltaCount);
    const uint endDelta = min(firstDelta + range.deltaCount, gConstants.deltaCount);

    // This thread is the only writer to this page (unique brick), so read the
    // occupancy word once, OR in the painted sub-bricks, write it back once.
    uint2 occupancy = gOccupancy[entry.pageIndex];

    [loop]
    for (uint i = firstDelta; i < endDelta; ++i) {
        const SparseEditDelta delta = gEditDeltas[i];
        const uint3 localVoxel = UnpackLocal(delta.packedLocal);
        gBrickPool[pageBase + SparseLocalIndex(localVoxel)] = delta.voxel;

        // Set the sub-brick occupancy bit for non-air writes so the raymarch's
        // occupancy early-out does not skip a freshly painted voxel. For air
        // (erase) we never clear: a stale set bit is conservative (the raymarch
        // fetches the voxel, sees air); durable regen recomputes occupancy later.
        if (UnpackMaterial(delta.voxel) != MAT_AIR) {
            const uint3 sub = localVoxel >> 2u;
            const uint subIndex = sub.x + sub.y * 4u + sub.z * 16u;
            if (subIndex < 32u) {
                occupancy.x |= (1u << subIndex);
            } else {
                occupancy.y |= (1u << (subIndex - 32u));
            }
        }
    }

    gOccupancy[entry.pageIndex] = occupancy;
}
