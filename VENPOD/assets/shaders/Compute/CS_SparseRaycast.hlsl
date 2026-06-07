// =============================================================================
// VENPOD Sparse Voxel Raycast
// Raycasts the sparse 16^3 brick page table and writes the same compact result
// format used by the legacy dense brush/ground raycast path.
// =============================================================================

#include "../Common/BitPacking.hlsli"

cbuffer SparseRaycastConstants : register(b0) {
    float rayOriginX;
    float rayOriginY;
    float rayOriginZ;
    float maxDistance;

    float rayDirX;
    float rayDirY;
    float rayDirZ;
    float flags;

    uint maxBrickPages;
    uint pageTableCapacity;
    uint maxSteps;
    uint padding0;
};

struct SparseBrickPageEntry {
    int3 coord;
    uint pageIndex;
    uint generation;
    uint flags;
    uint occupancyWord0;
    uint occupancyWord1;
};

StructuredBuffer<uint> SparseBrickVoxelPool : register(t0);
StructuredBuffer<SparseBrickPageEntry> SparseBrickPageTable : register(t1);
StructuredBuffer<uint2> SparseBrickOccupancy : register(t2);
StructuredBuffer<uint> SparseBrickPageGenerations : register(t3);
RWStructuredBuffer<float4> SparseRaycastResult : register(u0);

static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;
static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;

int FloorDiv16(int value) {
    return value >= 0 ? value / 16 : -(((-value) + 15) / 16);
}

int FloorModInt(int value, int modulus) {
    int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

uint HashSparseBrickCoord(int3 coord) {
    uint hash = 2166136261u;
    hash = (hash ^ (uint)coord.x) * 16777619u;
    hash = (hash ^ (uint)coord.y) * 16777619u;
    hash = (hash ^ (uint)coord.z) * 16777619u;
    return hash;
}

uint SparseLocalIndex(uint3 localVoxel) {
    return localVoxel.x +
        localVoxel.y * SPARSE_BRICK_SIZE +
        localVoxel.z * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
}

float PackNormal(int3 normal, bool valid) {
    uint packed = 0;
    packed |= (uint)(normal.x + 1);
    packed |= (uint)(normal.y + 1) << 2;
    packed |= (uint)(normal.z + 1) << 4;
    packed |= (valid ? 1u : 0u) << 6;
    return asfloat(packed);
}

bool LookupSparseBrick(int3 brickCoord, out SparseBrickPageEntry result) {
    result = (SparseBrickPageEntry)0;
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
            result = entry;
            return entry.generation != 0u;
        }
    }
    return false;
}

bool TrySampleSparseVoxel(int3 worldPos, out uint voxel) {
    voxel = PackVoxel(MAT_AIR, 0, 0, 0);
    int3 brickCoord = int3(
        FloorDiv16(worldPos.x),
        FloorDiv16(worldPos.y),
        FloorDiv16(worldPos.z));
    uint3 localVoxel = uint3(
        (uint)FloorModInt(worldPos.x, 16),
        (uint)FloorModInt(worldPos.y, 16),
        (uint)FloorModInt(worldPos.z, 16));

    SparseBrickPageEntry entry;
    if (!LookupSparseBrick(brickCoord, entry)) {
        return false;
    }
    if (entry.pageIndex >= maxBrickPages) {
        return false;
    }
    if (SparseBrickPageGenerations[entry.pageIndex] != entry.generation) {
        return false;
    }

    uint3 subCoord = localVoxel >> 2u;
    uint subIndex = subCoord.x + subCoord.y * 4u + subCoord.z * 16u;
    uint2 pageOccupancy = SparseBrickOccupancy[entry.pageIndex];
    uint occupancyWord = subIndex < 32u ? pageOccupancy.x : pageOccupancy.y;
    uint occupancyBit = subIndex < 32u ? subIndex : subIndex - 32u;
    if (((occupancyWord >> occupancyBit) & 1u) == 0u) {
        return true;
    }

    voxel = SparseBrickVoxelPool[
        entry.pageIndex * SPARSE_BRICK_VOXEL_COUNT +
        SparseLocalIndex(localVoxel)];
    return true;
}

float DistanceToSparseBrickExit(int3 worldVoxel, float3 currentPos, float3 rayDir) {
    int3 brickCoord = int3(
        FloorDiv16(worldVoxel.x),
        FloorDiv16(worldVoxel.y),
        FloorDiv16(worldVoxel.z));
    float3 brickMin = float3(brickCoord * 16);
    float3 brickMax = brickMin + float3(16.0f, 16.0f, 16.0f);

    float tx = rayDir.x > 0.0f ? (brickMax.x - currentPos.x) / rayDir.x :
        (rayDir.x < 0.0f ? (brickMin.x - currentPos.x) / rayDir.x : 1e20f);
    float ty = rayDir.y > 0.0f ? (brickMax.y - currentPos.y) / rayDir.y :
        (rayDir.y < 0.0f ? (brickMin.y - currentPos.y) / rayDir.y : 1e20f);
    float tz = rayDir.z > 0.0f ? (brickMax.z - currentPos.z) / rayDir.z :
        (rayDir.z < 0.0f ? (brickMin.z - currentPos.z) / rayDir.z : 1e20f);

    return max(0.001f, min(tx, min(ty, tz)) + 0.001f);
}

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float3 rayOrigin = float3(rayOriginX, rayOriginY, rayOriginZ);
    float3 rayDir = normalize(float3(rayDirX, rayDirY, rayDirZ));
    if (any(isnan(rayDir)) || length(rayDir) < 0.0001f || maxDistance <= 0.0f) {
        SparseRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
        return;
    }

    float3 startPos = rayOrigin + rayDir * 0.001f;
    int3 voxelPos = int3(floor(startPos));
    int3 step = int3(sign(rayDir));
    float3 deltaDist = abs(1.0f / rayDir);
    deltaDist = min(deltaDist, float3(1e20f, 1e20f, 1e20f));

    float3 sideDist;
    sideDist.x = rayDir.x > 0.0f ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = rayDir.y > 0.0f ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = rayDir.z > 0.0f ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    float traveled = 0.0f;
    int3 normal = int3(0, 0, 0);
    uint steps = max(maxSteps, 1u);

    [loop]
    for (uint i = 0u; i < steps && traveled <= maxDistance; ++i) {
        uint voxel;
        bool hasPage = TrySampleSparseVoxel(voxelPos, voxel);
        uint material = GetMaterial(voxel);

        if (hasPage && material != MAT_AIR) {
            SparseRaycastResult[0] = float4(
                (float)voxelPos.x + 0.5f,
                (float)voxelPos.y + 0.5f,
                (float)voxelPos.z + 0.5f,
                PackNormal(normal, true));
            return;
        }

        if (!hasPage) {
            float3 currentPos = startPos + rayDir * traveled;
            float skipDist = DistanceToSparseBrickExit(voxelPos, currentPos, rayDir);
            traveled += skipDist;
            if (traveled > maxDistance) {
                break;
            }
            float3 restartPos = startPos + rayDir * traveled;
            voxelPos = int3(floor(restartPos));
            sideDist.x = rayDir.x > 0.0f ? (voxelPos.x + 1.0f - restartPos.x) : (restartPos.x - voxelPos.x);
            sideDist.y = rayDir.y > 0.0f ? (voxelPos.y + 1.0f - restartPos.y) : (restartPos.y - voxelPos.y);
            sideDist.z = rayDir.z > 0.0f ? (voxelPos.z + 1.0f - restartPos.z) : (restartPos.z - voxelPos.z);
            sideDist *= deltaDist;
            sideDist += float3(traveled, traveled, traveled);
            continue;
        }

        if (sideDist.x <= sideDist.y && sideDist.x <= sideDist.z) {
            voxelPos.x += step.x;
            traveled = sideDist.x;
            sideDist.x += deltaDist.x;
            normal = int3(-step.x, 0, 0);
        } else if (sideDist.y <= sideDist.z) {
            voxelPos.y += step.y;
            traveled = sideDist.y;
            sideDist.y += deltaDist.y;
            normal = int3(0, -step.y, 0);
        } else {
            voxelPos.z += step.z;
            traveled = sideDist.z;
            sideDist.z += deltaDist.z;
            normal = int3(0, 0, -step.z);
        }
    }

    SparseRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
}
