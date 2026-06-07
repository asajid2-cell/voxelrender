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
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;
static const float SPARSE_BRICK_SIZE_F = 16.0f;
static const float SPARSE_RAY_EPSILON = 0.0001f;

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
    for (uint probe = 0u; probe < SPARSE_PAGE_TABLE_LOOKUP_PROBES; ++probe) {
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

struct BrickDdaAxis {
    int step;
    float nextT;
    float deltaT;
};

BrickDdaAxis BuildBrickDdaAxis(float origin, float direction, int brickCoord) {
    BrickDdaAxis axis;
    axis.step = 0;
    axis.nextT = 1e30f;
    axis.deltaT = 1e30f;

    if (abs(direction) < 1e-6f) {
        return axis;
    }

    if (direction > 0.0f) {
        axis.step = 1;
        float boundary = (float)(brickCoord + 1) * SPARSE_BRICK_SIZE_F;
        axis.nextT = max((boundary - origin) / direction, 0.0f);
        axis.deltaT = SPARSE_BRICK_SIZE_F / direction;
    } else {
        axis.step = -1;
        float boundary = (float)brickCoord * SPARSE_BRICK_SIZE_F;
        axis.nextT = max((boundary - origin) / direction, 0.0f);
        axis.deltaT = -SPARSE_BRICK_SIZE_F / direction;
    }
    return axis;
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

    int3 worldVoxel = int3(
        (int)floor(cameraOrigin.x),
        (int)floor(cameraOrigin.y),
        (int)floor(cameraOrigin.z));
    int3 brickCoord = int3(
        FloorDiv16(worldVoxel.x),
        FloorDiv16(worldVoxel.y),
        FloorDiv16(worldVoxel.z));

    BrickDdaAxis axisX = BuildBrickDdaAxis(cameraOrigin.x, rayDir.x, brickCoord.x);
    BrickDdaAxis axisY = BuildBrickDdaAxis(cameraOrigin.y, rayDir.y, brickCoord.y);
    BrickDdaAxis axisZ = BuildBrickDdaAxis(cameraOrigin.z, rayDir.z, brickCoord.z);

    uint steps = min(max(maxSteps, (uint)ceil(maxDistance / SPARSE_BRICK_SIZE_F) * 4u + 8u), 4096u);
    float distance = 0.0f;
    [loop]
    for (uint step = 0u; step <= steps && distance <= maxDistance; ++step) {
        if (!HasSparseBrickPage(brickCoord)) {
            RecordMissingBrick(brickCoord, step);
            break;
        }

        float nextDistance = min(axisX.nextT, min(axisY.nextT, axisZ.nextT));
        if (!isfinite(nextDistance) || nextDistance > maxDistance) {
            break;
        }

        const float tieEpsilon = 0.0005f;
        if (axisX.nextT <= nextDistance + tieEpsilon) {
            brickCoord.x += axisX.step;
            axisX.nextT += axisX.deltaT;
        }
        if (axisY.nextT <= nextDistance + tieEpsilon) {
            brickCoord.y += axisY.step;
            axisY.nextT += axisY.deltaT;
        }
        if (axisZ.nextT <= nextDistance + tieEpsilon) {
            brickCoord.z += axisZ.step;
            axisZ.nextT += axisZ.deltaT;
        }
        distance = max(nextDistance + SPARSE_RAY_EPSILON, distance + SPARSE_RAY_EPSILON);
    }
}
