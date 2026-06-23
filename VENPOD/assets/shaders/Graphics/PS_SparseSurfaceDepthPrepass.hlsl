#include "../Common/SharedTypes.hlsli"
#include "../Common/BitPacking.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

struct SparseBrickPageEntry {
    int3 coord;
    uint pageIndex;
    uint generation;
    uint flags;
    uint occupancyWord0;
    uint occupancyWord1;
};

StructuredBuffer<uint> SparseBrickVoxelPool : register(t6);
StructuredBuffer<SparseBrickPageEntry> SparseBrickPageTable : register(t7);
StructuredBuffer<uint2> SparseBrickOccupancy : register(t8);
StructuredBuffer<uint> SparseBrickPageGenerations : register(t9);

static const float FAR_SEA_LEVEL = -48.0f;
static const float FAR_WATER_SURFACE_Y = FAR_SEA_LEVEL + 1.0f;
static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;
static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;

struct PSInput {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint material : MATERIAL0;
    nointerpolation uint faceDirection : TEXCOORD2;
    float distance : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

int FloorModInt(int value, int modulus) {
    int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

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

uint SparseLocalIndex(uint3 localVoxel) {
    return localVoxel.x + localVoxel.y * SPARSE_BRICK_SIZE +
        localVoxel.z * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
}

bool LookupSparseBrick(int3 brickCoord, uint tableCapacity, out SparseBrickPageEntry result) {
    result = (SparseBrickPageEntry)0;
    if (tableCapacity == 0u || (tableCapacity & (tableCapacity - 1u)) != 0u) {
        return false;
    }

    const uint mask = tableCapacity - 1u;
    const uint start = HashSparseBrickCoord(brickCoord) & mask;
    [loop]
    for (uint probe = 0u; probe < SPARSE_PAGE_TABLE_LOOKUP_PROBES; ++probe) {
        const uint slot = (start + probe) & mask;
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

bool TrySampleSparseSurfaceVoxel(int3 worldVoxel, out uint voxel) {
    voxel = 0u;
    if (frame.sparseNearParams.x < 0.5f) {
        return false;
    }
    const uint maxPages = (uint)frame.sparseNearParams.y;
    const uint tableCapacity = (uint)frame.sparseNearParams.z;
    const int3 brickCoord = int3(
        FloorDiv16(worldVoxel.x),
        FloorDiv16(worldVoxel.y),
        FloorDiv16(worldVoxel.z));
    const uint3 localVoxel = uint3(
        (uint)FloorModInt(worldVoxel.x, 16),
        (uint)FloorModInt(worldVoxel.y, 16),
        (uint)FloorModInt(worldVoxel.z, 16));

    SparseBrickPageEntry entry;
    if (!LookupSparseBrick(brickCoord, tableCapacity, entry) || entry.pageIndex >= maxPages) {
        return false;
    }
    if (SparseBrickPageGenerations[entry.pageIndex] != entry.generation) {
        return false;
    }

    const uint3 subCoord = localVoxel >> 2u;
    const uint subIndex = subCoord.x + subCoord.y * 4u + subCoord.z * 16u;
    const uint2 pageOccupancy = SparseBrickOccupancy[entry.pageIndex];
    const uint occupancyWord = subIndex < 32u ? pageOccupancy.x : pageOccupancy.y;
    const uint occupancyBit = subIndex < 32u ? subIndex : subIndex - 32u;
    if (((occupancyWord >> occupancyBit) & 1u) == 0u) {
        return false;
    }

    voxel = SparseBrickVoxelPool[entry.pageIndex * SPARSE_BRICK_VOXEL_COUNT + SparseLocalIndex(localVoxel)];
    return true;
}

uint ResolveSparseSurfaceMaterial(uint bakedMaterial, float3 worldPos, float3 normal,
                                  out bool liveErased) {
    liveErased = false;
    uint liveVoxel = 0u;
    const int3 sampleVoxel = int3(floor(worldPos - normalize(normal) * 0.5f));
    if (TrySampleSparseSurfaceVoxel(sampleVoxel, liveVoxel)) {
        const uint liveMaterial = GetMaterial(liveVoxel);
        if (liveMaterial != MAT_AIR) {
            return liveMaterial;
        }
        liveErased = true;
    }
    return bakedMaterial;
}

void main(PSInput input) {
    const float surfaceDistance = distance(input.worldPos, frame.cameraPosition.xyz);
    const float exactNearDistance = max(frame.exactNearParams.x, 0.0f);
    bool liveErased = false;
    const uint material = ResolveSparseSurfaceMaterial(input.material, input.worldPos, input.normal, liveErased);
    const float eraseDiscardRange = exactNearDistance > 0.0f
        ? exactNearDistance
        : 192.0f;
    if (liveErased && surfaceDistance <= eraseDiscardRange) {
        discard;
    }
    const bool aboveWaterView = frame.cameraPosition.y >= FAR_WATER_SURFACE_Y - 0.5f;
    const bool sparseWaterVoxelOccludedByPlane = false;
    float waterPlaneT = 0.0f;
    bool deterministicWaterBeforeSurface = false;
    if (material != MAT_WATER && aboveWaterView) {
        const float3 toSurface = input.worldPos - frame.cameraPosition.xyz;
        const float rayLength = length(toSurface);
        if (rayLength > 0.001f) {
            const float3 rayDir = toSurface / rayLength;
            if (rayDir.y < -0.0001f) {
                waterPlaneT = (FAR_WATER_SURFACE_Y - frame.cameraPosition.y) / rayDir.y;
                deterministicWaterBeforeSurface =
                    waterPlaneT >= 4.0f &&
                    waterPlaneT < rayLength - 0.25f &&
                    input.worldPos.y < FAR_WATER_SURFACE_Y + 18.0f;
            }
        }
    }
    const bool sparseSubmergedTerrainOccludedByPlane =
        material != MAT_WATER &&
        aboveWaterView &&
        (input.worldPos.y < FAR_WATER_SURFACE_Y - 0.05f ||
         deterministicWaterBeforeSurface);

    if (sparseWaterVoxelOccludedByPlane ||
        sparseSubmergedTerrainOccludedByPlane) {
        discard;
    }
}
