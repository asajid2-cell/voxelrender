// =============================================================================
// VENPOD Sparse Brush Feedback
// Emits compact world-space edit records for resident sparse bricks.
// =============================================================================

#include "../Common/BitPacking.hlsli"

#define BRUSH_MODE_PAINT   0
#define BRUSH_MODE_ERASE   1
#define BRUSH_MODE_REPLACE 2
#define BRUSH_MODE_FILL    3

#define BRUSH_SHAPE_SPHERE   0
#define BRUSH_SHAPE_CUBE     1
#define BRUSH_SHAPE_CYLINDER 2

cbuffer SparseBrushFeedbackConstants : register(b0) {
    float positionX;
    float positionY;
    float positionZ;
    float radius;

    uint material;
    uint mode;
    uint shape;
    float strength;

    int startX;
    int startY;
    int startZ;
    uint volumeX;

    uint volumeY;
    uint volumeZ;
    uint seed;
    uint maxRecords;

    int hitNormalX;
    int hitNormalY;
    int hitNormalZ;
    uint hasHitNormal;

    uint maxBrickPages;
    uint pageTableCapacity;
    uint frameIndex;
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
RWStructuredBuffer<uint4> SparseBrushFeedback : register(u0);

static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;
static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT = 0xFFFFFFFFu;

int FloorDiv16(int value) {
    return value >= 0 ? value / 16 : -(((-value) + 15) / 16);
}

int FloorModInt(int value, int modulus) {
    int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

uint HashSparseBrush(uint3 v, uint s) {
    uint h = 2166136261u ^ s;
    h = (h ^ v.x) * 16777619u;
    h = (h ^ v.y) * 16777619u;
    h = (h ^ v.z) * 16777619u;
    h ^= h >> 16u;
    h *= 2246822519u;
    h ^= h >> 13u;
    h *= 3266489917u;
    h ^= h >> 16u;
    return h;
}

float Random01(uint h) {
    h ^= h >> 16u;
    h *= 2246822519u;
    h ^= h >> 13u;
    h *= 3266489917u;
    h ^= h >> 16u;
    return (float)(h & 0x00FFFFFFu) / 16777215.0f;
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

bool LookupSparseBrick(int3 brickCoord, out SparseBrickPageEntry result) {
    result = (SparseBrickPageEntry)0;
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
            result = entry;
            return entry.generation != 0u;
        }
    }
    return false;
}

bool TrySampleResidentVoxel(int3 worldPos, out uint voxel) {
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
    if (!LookupSparseBrick(brickCoord, entry) || entry.pageIndex >= maxBrickPages) {
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

float SDFSphere(float3 p, float3 center, float r) {
    return length(p - center) - r;
}

float SDFBox(float3 p, float3 center, float3 halfExtents) {
    float3 d = abs(p - center) - halfExtents;
    return length(max(d, 0.0f)) + min(max(d.x, max(d.y, d.z)), 0.0f);
}

float SDFCylinder(float3 p, float3 center, float r, float h) {
    float3 d = p - center;
    float2 dh = abs(float2(length(d.xz), d.y)) - float2(r, h);
    return min(max(dh.x, dh.y), 0.0f) + length(max(dh, 0.0f));
}

float BrushSdf(float3 p, float3 center) {
    if (shape == BRUSH_SHAPE_CUBE) {
        return SDFBox(p, center, float3(radius, radius, radius));
    }
    if (shape == BRUSH_SHAPE_CYLINDER) {
        return SDFCylinder(p, center, radius, radius);
    }
    return SDFSphere(p, center, radius);
}

[numthreads(8, 8, 8)]
void main(uint3 id : SV_DispatchThreadID) {
    if (all(id == uint3(0u, 0u, 0u))) {
        SparseBrushFeedback[0].y = frameIndex;
    }

    if (id.x >= volumeX || id.y >= volumeY || id.z >= volumeZ) {
        return;
    }

    int3 worldVoxel = int3(startX, startY, startZ) + int3(id);
    float3 voxelCenter = float3(worldVoxel) + 0.5f;
    float3 brushCenter = float3(positionX, positionY, positionZ);
    float sdf = BrushSdf(voxelCenter, brushCenter);
    if (sdf > 0.5f) {
        return;
    }

    if (hasHitNormal != 0u) {
        float3 normal = float3((float)hitNormalX, (float)hitNormalY, (float)hitNormalZ);
        float faceSide = dot(voxelCenter - brushCenter, normal);
        if (mode == BRUSH_MODE_PAINT && faceSide < -0.35f) {
            return;
        }
        if ((mode == BRUSH_MODE_ERASE || mode == BRUSH_MODE_REPLACE) && faceSide > 0.65f) {
            return;
        }
    }

    uint currentVoxel;
    if (!TrySampleResidentVoxel(worldVoxel, currentVoxel)) {
        InterlockedAdd(SparseBrushFeedback[0].w, 1u);
        uint missingWriteIndex = 0;
        InterlockedAdd(SparseBrushFeedback[0].x, 1u, missingWriteIndex);
        SparseBrushFeedback[0].y = frameIndex;
        if (missingWriteIndex < maxRecords) {
            SparseBrushFeedback[missingWriteIndex + 1u] = uint4(
                (uint)worldVoxel.x,
                (uint)worldVoxel.y,
                (uint)worldVoxel.z,
                SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT);
        } else {
            SparseBrushFeedback[0].z = 1u;
        }
        return;
    }

    uint currentMaterial = GetMaterial(currentVoxel);
    if (currentMaterial == MAT_BEDROCK) {
        return;
    }

    uint random = HashSparseBrush((uint3)worldVoxel, seed);
    uint variant = random & 0xFFu;
    uint newVoxel = currentVoxel;
    if (mode == BRUSH_MODE_PAINT) {
        if (currentMaterial != MAT_AIR) {
            return;
        }
        newVoxel = PackVoxel(material, variant, 0, STATE_IS_STATIC);
    } else if (mode == BRUSH_MODE_ERASE) {
        if (currentMaterial == MAT_AIR) {
            return;
        }
        newVoxel = PackVoxel(MAT_AIR, 0, 0, 0);
    } else if (mode == BRUSH_MODE_REPLACE) {
        if (currentMaterial == MAT_AIR) {
            return;
        }
        newVoxel = PackVoxel(material, variant, 0, STATE_IS_STATIC);
    } else if (mode == BRUSH_MODE_FILL) {
        newVoxel = PackVoxel(material, variant, 0, STATE_IS_STATIC);
    } else {
        return;
    }

    if (strength < 1.0f && sdf > -0.5f) {
        float edgeFactor = saturate(1.0f - sdf / 0.5f);
        if (((float)variant / 255.0f) > edgeFactor * strength) {
            return;
        }
    }

    if (newVoxel == currentVoxel) {
        return;
    }

    uint writeIndex = 0;
    InterlockedAdd(SparseBrushFeedback[0].x, 1u, writeIndex);
    SparseBrushFeedback[0].y = frameIndex;
    if (writeIndex < maxRecords) {
        SparseBrushFeedback[writeIndex + 1u] = uint4(
            (uint)worldVoxel.x,
            (uint)worldVoxel.y,
            (uint)worldVoxel.z,
            newVoxel);
    } else {
        SparseBrushFeedback[0].z = 1u;
    }
}
