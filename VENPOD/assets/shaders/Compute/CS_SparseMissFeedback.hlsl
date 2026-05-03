// =============================================================================
// VENPOD Sparse Missing-Brick Feedback
// Samples a small screen-space ray grid and reports sparse brick pages that the
// current GPU page table cannot resolve. This is a residency feedback pass, not
// a renderer.
// =============================================================================

cbuffer SparseMissFeedbackConstants : register(b0) {
    float3 cameraOrigin;
    float maxDistance;
    float3 cameraForward;
    float stepDistance;
    float3 cameraRight;
    float tanHalfFov;
    float3 cameraUp;
    float aspectRatio;
    uint maxBrickPages;
    uint pageTableCapacity;
    uint rayGrid;
    uint maxRecords;
    uint maxSteps;
    uint frameIndex;
    uint padding0;
    uint padding1;
};

struct SparseBrickPageEntry {
    int3 coord;
    uint pageIndex;
    uint generation;
    uint flags;
    uint occupancyWord0;
    uint occupancyWord1;
};

StructuredBuffer<SparseBrickPageEntry> SparseBrickPageTable : register(t0);
RWStructuredBuffer<uint4> MissingBrickFeedback : register(u0);

static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;

int FloorDiv16(int value) {
    return value >= 0 ? value / 16 : -(((-value) + 15) / 16);
}

uint HashSparseBrickCoord(int3 coord) {
    uint hash = 2166136261u;
    hash = (hash ^ (uint)coord.x) * 16777619u;
    hash = (hash ^ (uint)coord.y) * 16777619u;
    hash = (hash ^ (uint)coord.z) * 16777619u;
    return hash;
}

bool HasSparseBrickPage(int3 brickCoord) {
    if (pageTableCapacity == 0u || (pageTableCapacity & (pageTableCapacity - 1u)) != 0u) {
        return false;
    }

    uint mask = pageTableCapacity - 1u;
    uint start = HashSparseBrickCoord(brickCoord) & mask;
    [loop]
    for (uint probe = 0u; probe < 64u; ++probe) {
        uint slot = (start + probe) & mask;
        SparseBrickPageEntry entry = SparseBrickPageTable[slot];
        if (entry.pageIndex == SPARSE_INVALID_PAGE) {
            return false;
        }
        if (entry.pageIndex == SPARSE_TOMBSTONE_PAGE) {
            continue;
        }
        if (all(entry.coord == brickCoord)) {
            return entry.generation != 0u && entry.pageIndex < maxBrickPages;
        }
    }
    return false;
}

void RecordMissingBrick(int3 brickCoord, uint sampleIndex) {
    uint writeIndex = 0u;
    InterlockedAdd(MissingBrickFeedback[0].x, 1u, writeIndex);
    if (writeIndex >= maxRecords) {
        return;
    }

    MissingBrickFeedback[writeIndex + 1u] = uint4(
        (uint)brickCoord.x,
        (uint)brickCoord.y,
        (uint)brickCoord.z,
        sampleIndex);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    if (groupIndex == 0u) {
        MissingBrickFeedback[0] = uint4(0u, frameIndex, 0u, 0u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadId.x >= rayGrid || dispatchThreadId.y >= rayGrid ||
        rayGrid == 0u || maxRecords == 0u || maxDistance <= 0.0f || stepDistance <= 0.0f) {
        return;
    }

    float center = (float)(rayGrid - 1u) * 0.5f;
    float ndcX = center > 0.0f ? ((float)dispatchThreadId.x - center) / center : 0.0f;
    float ndcY = center > 0.0f ? ((float)dispatchThreadId.y - center) / center : 0.0f;
    float3 rayDir = normalize(
        cameraForward +
        cameraRight * ndcX * tanHalfFov * aspectRatio +
        cameraUp * ndcY * tanHalfFov);

    uint steps = min(maxSteps, 4096u);
    [loop]
    for (uint step = 0u; step <= steps; ++step) {
        float distance = (float)step * stepDistance;
        if (distance > maxDistance) {
            break;
        }

        float3 sampleWorld = cameraOrigin + rayDir * distance;
        int3 worldVoxel = int3(
            (int)floor(sampleWorld.x),
            (int)floor(sampleWorld.y),
            (int)floor(sampleWorld.z));
        int3 brickCoord = int3(
            FloorDiv16(worldVoxel.x),
            FloorDiv16(worldVoxel.y),
            FloorDiv16(worldVoxel.z));

        if (!HasSparseBrickPage(brickCoord)) {
            RecordMissingBrick(brickCoord, step);
            break;
        }
    }
}
