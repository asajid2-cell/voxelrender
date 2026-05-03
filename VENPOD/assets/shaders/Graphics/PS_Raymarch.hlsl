// =============================================================================
// VENPOD Voxel Raymarcher Pixel Shader
// DDA algorithm for stepping through voxel grid
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"

// Constant buffer
cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

// Voxel grid (read-only for rendering)
StructuredBuffer<uint> VoxelGrid : register(t0);

// Material palette
Texture1D<float4> MaterialPalette : register(t1);
SamplerState PaletteSampler : register(s0);

struct FarVoxelNode {
    uint childBase;
    uint childMask;
    uint material;
    uint flags;
};

struct FarVoxelPage {
    int originX;
    int originY;
    int originZ;
    uint rootNode;
};

StructuredBuffer<FarVoxelNode> FarVoxelNodes : register(t2);
StructuredBuffer<FarVoxelPage> FarVoxelPages : register(t3);
StructuredBuffer<uint> FarVoxelPageIndex : register(t4);
StructuredBuffer<uint4> ChunkValidMask : register(t5);

struct SparseBrickPageEntry {
    int3 coord;
    uint pageIndex;
    uint generation;
    uint flags;
    uint occupancyWord0;
    uint occupancyWord1;
};

struct SparseRayCache {
    int3 brickCoord;
    uint pageIndex;
    uint generation;
    uint valid;
    uint hasEntry;
};

StructuredBuffer<uint> SparseBrickVoxelPool : register(t6);
StructuredBuffer<SparseBrickPageEntry> SparseBrickPageTable : register(t7);
StructuredBuffer<uint2> SparseBrickOccupancy : register(t8);
StructuredBuffer<uint> SparseBrickPageGenerations : register(t9);
StructuredBuffer<uint4> MidClipmapTiles : register(t10);
StructuredBuffer<uint4> MidClipmapLookup : register(t11);
StructuredBuffer<uint> MidClipmapSamples : register(t12);
StructuredBuffer<uint4> MidVoxelClipmapMetadata : register(t13);
StructuredBuffer<uint4> MidVoxelClipmapLookup : register(t14);
StructuredBuffer<uint> MidVoxelClipmapSamples : register(t15);

struct SparseSurfaceFace {
    int voxelX;
    int voxelY;
    int voxelZ;
    uint material;
    uint normalAndDirection;
};

struct SparseSurfaceBrickRange {
    int3 coord;
    uint firstFace;
    uint faceCount;
    uint flags;
};

StructuredBuffer<SparseSurfaceFace> SparseSurfaceFaces : register(t16);
StructuredBuffer<SparseSurfaceBrickRange> SparseSurfaceRanges : register(t17);

static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;
static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_SAMPLE_MISSING = 0u;
static const uint SPARSE_SAMPLE_VOXEL = 1u;
static const uint SPARSE_SAMPLE_EMPTY_SUBBRICK = 2u;
static const uint SPARSE_SAMPLE_EMPTY_BRICK = 3u;
static const uint MID_CLIPMAP_MAGIC = 0x56434C50u;
static const uint MID_VOXEL_CLIPMAP_MAGIC = 0x56435658u;
static const uint MID_CLIPMAP_MAX_SHADER_TILES = 128u;
static const uint MID_CLIPMAP_MAX_SHADER_RINGS = 8u;
static const uint MID_CLIPMAP_LOOKUP_PROBES = 8u;
static const uint MID_VOXEL_CLIPMAP_MAX_BRICKS = 128u;
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 8u;
static const uint SPARSE_SURFACE_RANGE_LOOKUP_PROBES = 8u;

static const float FAR_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FAR_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FAR_SEA_LEVEL = -48.0f;
static const uint FAR_WORLD_SEED = 12345u;
static const bool FAR_TERRAIN_HORIZON_ENABLED = true;
static const float FAR_SVO_ROOT_CELL_SIZE = 512.0f;
static const float FAR_SVO_MIN_CELL_SIZE = 24.0f;
static const int FAR_SVO_MAX_LEVELS = 3;

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct RayHit {
    float4 color;
    float distance;
};

int FloorDiv64(int value) {
    return value >= 0 ? value / 64 : -(((-value) + 63) / 64);
}

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

bool LookupSparseSurfaceRange(int3 brickCoord, out SparseSurfaceBrickRange range) {
    range = (SparseSurfaceBrickRange)0;
    const uint tableCapacity = (uint)frame.surfaceParams.w;
    if (frame.surfaceParams.x < 0.5f ||
        tableCapacity == 0u ||
        (tableCapacity & (tableCapacity - 1u)) != 0u) {
        return false;
    }

    const uint mask = tableCapacity - 1u;
    uint slot = HashSparseBrickCoord(brickCoord) & mask;
    [unroll]
    for (uint probe = 0u; probe < SPARSE_SURFACE_RANGE_LOOKUP_PROBES; ++probe) {
        SparseSurfaceBrickRange candidate = SparseSurfaceRanges[slot];
        if (candidate.flags == 0u) {
            return false;
        }
        if (all(candidate.coord == brickCoord)) {
            range = candidate;
            return true;
        }
        slot = (slot + 1u) & mask;
    }
    return false;
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

    uint mask = tableCapacity - 1u;
    uint start = HashSparseBrickCoord(brickCoord) & mask;
    [unroll]
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

float DistanceToSparseSubbrickExit(int3 worldVoxel, float3 currentPos, float3 rayDir) {
    int3 brickCoord = int3(
        FloorDiv16(worldVoxel.x),
        FloorDiv16(worldVoxel.y),
        FloorDiv16(worldVoxel.z));
    int3 brickMinVoxel = brickCoord * 16;
    int3 localVoxel = int3(
        FloorModInt(worldVoxel.x, 16),
        FloorModInt(worldVoxel.y, 16),
        FloorModInt(worldVoxel.z, 16));
    int3 subMinVoxel = brickMinVoxel + (localVoxel / 4) * 4;
    float3 subMin = float3(subMinVoxel);
    float3 subMax = subMin + float3(4.0f, 4.0f, 4.0f);

    float tx = rayDir.x > 0.0f ? (subMax.x - currentPos.x) / rayDir.x :
        (rayDir.x < 0.0f ? (subMin.x - currentPos.x) / rayDir.x : 1e20f);
    float ty = rayDir.y > 0.0f ? (subMax.y - currentPos.y) / rayDir.y :
        (rayDir.y < 0.0f ? (subMin.y - currentPos.y) / rayDir.y : 1e20f);
    float tz = rayDir.z > 0.0f ? (subMax.z - currentPos.z) / rayDir.z :
        (rayDir.z < 0.0f ? (subMin.z - currentPos.z) / rayDir.z : 1e20f);

    return max(0.001f, min(tx, min(ty, tz)) + 0.001f);
}

void RestartSparseDdaAtDistance(
    float3 startPos,
    float3 rayDir,
    float3 deltaDist,
    float dist,
    out int3 voxelPos,
    out float3 sideDist)
{
    float3 restartPos = startPos + rayDir * dist;
    voxelPos = int3(floor(restartPos));
    sideDist.x = (rayDir.x > 0.0f) ? (voxelPos.x + 1.0f - restartPos.x) : (restartPos.x - voxelPos.x);
    sideDist.y = (rayDir.y > 0.0f) ? (voxelPos.y + 1.0f - restartPos.y) : (restartPos.y - voxelPos.y);
    sideDist.z = (rayDir.z > 0.0f) ? (voxelPos.z + 1.0f - restartPos.z) : (restartPos.z - voxelPos.z);
    sideDist *= deltaDist;
    sideDist += float3(dist, dist, dist);
}

bool TrySampleSparseBrickVoxel(
    int3 worldPos,
    uint maxPages,
    uint tableCapacity,
    inout SparseRayCache cache,
    out uint voxel,
    out uint sampleState)
{
    voxel = PackVoxel(MAT_AIR, 0, 0, 0);
    sampleState = SPARSE_SAMPLE_MISSING;
    int3 brickCoord = int3(
        FloorDiv16(worldPos.x),
        FloorDiv16(worldPos.y),
        FloorDiv16(worldPos.z));
    uint3 localVoxel = uint3(
        (uint)FloorModInt(worldPos.x, 16),
        (uint)FloorModInt(worldPos.y, 16),
        (uint)FloorModInt(worldPos.z, 16));

    if (cache.valid == 0u || any(cache.brickCoord != brickCoord)) {
        cache.brickCoord = brickCoord;
        cache.valid = 1u;
        SparseBrickPageEntry entry;
        if (LookupSparseBrick(brickCoord, tableCapacity, entry)) {
            cache.hasEntry = 1u;
            cache.pageIndex = entry.pageIndex;
            cache.generation = entry.generation;
        } else {
            cache.hasEntry = 0u;
            cache.pageIndex = SPARSE_INVALID_PAGE;
            cache.generation = 0u;
        }
    }
    if (cache.hasEntry == 0u) {
        return false;
    }
    if (cache.pageIndex >= maxPages) {
        return false;
    }
    if (SparseBrickPageGenerations[cache.pageIndex] != cache.generation) {
        return false;
    }

    uint localIndex = SparseLocalIndex(localVoxel);
    uint3 subCoord = localVoxel >> 2u;
    uint subIndex = subCoord.x + subCoord.y * 4u + subCoord.z * 16u;
    uint2 pageOccupancy = SparseBrickOccupancy[cache.pageIndex];
    if ((pageOccupancy.x | pageOccupancy.y) == 0u) {
        sampleState = SPARSE_SAMPLE_EMPTY_BRICK;
        return true;
    }
    uint occupancyWord = subIndex < 32u ? pageOccupancy.x : pageOccupancy.y;
    uint occupancyBit = subIndex < 32u ? subIndex : subIndex - 32u;
    // Current occupancy is coarse: one bit means a 4x4x4 sub-brick has content.
    // It is still useful for quickly rejecting empty bricks/lane groups before
    // exact voxel fetch.
    if (((occupancyWord >> occupancyBit) & 1u) == 0u) {
        sampleState = SPARSE_SAMPLE_EMPTY_SUBBRICK;
        return true;
    }

    voxel = SparseBrickVoxelPool[cache.pageIndex * SPARSE_BRICK_VOXEL_COUNT + localIndex];
    sampleState = SPARSE_SAMPLE_VOXEL;
    return true;
}

uint RenderSlotIndex(int3 chunkCoord, out uint3 slotCoord) {
    int3 dims = int3((int)(frame.gridSizeX / 64u), (int)(frame.gridSizeY / 64u), (int)(frame.gridSizeZ / 64u));
    slotCoord = uint3(
        (uint)FloorModInt(chunkCoord.x, dims.x),
        (uint)FloorModInt(chunkCoord.y, dims.y),
        (uint)FloorModInt(chunkCoord.z, dims.z));
    return slotCoord.x + slotCoord.y * (uint)dims.x + slotCoord.z * (uint)dims.x * (uint)dims.y;
}

// Sample voxel from toroidal near-field chunk cache.
uint GetVoxel(
    int3 worldPos,
    inout SparseRayCache sparseCache,
    out bool fromSparse,
    out bool sparseMissing,
    out uint sparseSampleState)
{
    fromSparse = false;
    sparseMissing = false;
    sparseSampleState = SPARSE_SAMPLE_MISSING;
    if (frame.sparseNearParams.x > 0.5f) {
        uint sparseVoxel;
        if (TrySampleSparseBrickVoxel(
                worldPos,
                (uint)frame.sparseNearParams.y,
                (uint)frame.sparseNearParams.z,
                sparseCache,
                sparseVoxel,
                sparseSampleState)) {
            fromSparse = true;
            return sparseVoxel;
        }
        sparseMissing = true;
        if (((uint)frame.sparseNearParams.w & 1u) != 0u) {
            return PackVoxel(MAT_AIR, 0, 0, 0);
        }
    }

    int3 worldChunk = int3(
        FloorDiv64(worldPos.x),
        FloorDiv64(worldPos.y),
        FloorDiv64(worldPos.z));
    uint3 localVoxel = uint3(
        (uint)FloorModInt(worldPos.x, 64),
        (uint)FloorModInt(worldPos.y, 64),
        (uint)FloorModInt(worldPos.z, 64));

    uint3 slotCoord;
    uint chunkIndex = RenderSlotIndex(worldChunk, slotCoord);
    uint4 slotTag = ChunkValidMask[chunkIndex];
    if (slotTag.w == 0u ||
        slotTag.x != (uint)worldChunk.x ||
        slotTag.y != (uint)worldChunk.y ||
        slotTag.z != (uint)worldChunk.z) {
        return PackVoxel(MAT_AIR, 0, 0, 0);
    }

    uint3 bufferPos = slotCoord * 64u + localVoxel;
    uint3 gridSize = uint3(frame.gridSizeX, frame.gridSizeY, frame.gridSizeZ);
    uint idx = LinearIndex3D(bufferPos, gridSize);
    return VoxelGrid[idx];
}

// Box intersection test (AABB ray intersection)
bool IntersectBox(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tMin, out float tMax) {
    float3 safeDir = float3(
        abs(rayDir.x) < 1e-6f ? (rayDir.x < 0.0f ? -1e-6f : 1e-6f) : rayDir.x,
        abs(rayDir.y) < 1e-6f ? (rayDir.y < 0.0f ? -1e-6f : 1e-6f) : rayDir.y,
        abs(rayDir.z) < 1e-6f ? (rayDir.z < 0.0f ? -1e-6f : 1e-6f) : rayDir.z);
    float3 invDir = 1.0f / safeDir;
    float3 t0 = (boxMin - rayOrigin) * invDir;
    float3 t1 = (boxMax - rayOrigin) * invDir;

    float3 tNear = min(t0, t1);
    float3 tFar = max(t0, t1);

    tMin = max(max(tNear.x, tNear.y), tNear.z);
    tMax = min(min(tFar.x, tFar.y), tFar.z);

    return tMax >= tMin && tMax >= 0.0f;
}

bool IntersectBoxWithNormal(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tMin, out float tMax, out float3 normal) {
    float3 invDir = 1.0f / rayDir;
    float3 t0 = (boxMin - rayOrigin) * invDir;
    float3 t1 = (boxMax - rayOrigin) * invDir;

    float3 tNear = min(t0, t1);
    float3 tFar = max(t0, t1);

    tMin = max(max(tNear.x, tNear.y), tNear.z);
    tMax = min(min(tFar.x, tFar.y), tFar.z);
    if (tMax < tMin || tMax < 0.0f) {
        normal = float3(0, 1, 0);
        return false;
    }

    if (tNear.x >= tNear.y && tNear.x >= tNear.z) {
        normal = float3(rayDir.x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
    } else if (tNear.y >= tNear.z) {
        normal = float3(0.0f, rayDir.y > 0.0f ? -1.0f : 1.0f, 0.0f);
    } else {
        normal = float3(0.0f, 0.0f, rayDir.z > 0.0f ? -1.0f : 1.0f);
    }
    return true;
}

RayHit MakeHit(float4 color, float distance) {
    RayHit hit;
    hit.color = color;
    hit.distance = distance;
    return hit;
}

float3 SkySunDirection() {
    return normalize(float3(0.45f, 0.72f, 0.28f));
}

float3 SkyColor(float3 rayDir) {
    float up = saturate(rayDir.y * 0.5f + 0.5f);
    float horizon = pow(saturate(1.0f - abs(rayDir.y) * 1.45f), 2.0f);
    float sun = pow(saturate(dot(rayDir, SkySunDirection())), 420.0f);
    float sunBloom = pow(saturate(dot(rayDir, SkySunDirection())), 18.0f);
    float antiSun = pow(saturate(dot(rayDir, normalize(float3(-0.35f, 0.22f, -0.85f)))), 6.0f);

    float3 lowerSky = float3(0.72f, 0.84f, 0.96f);
    float3 upperSky = float3(0.22f, 0.43f, 0.78f);
    float3 horizonTint = float3(1.00f, 0.84f, 0.58f);
    float3 sky = lerp(lowerSky, upperSky, up);
    sky = lerp(sky, horizonTint, horizon * 0.42f);
    sky += float3(1.00f, 0.72f, 0.34f) * sunBloom * 0.32f;
    sky += float3(1.00f, 0.93f, 0.74f) * sun * 1.75f;
    sky += float3(0.18f, 0.24f, 0.38f) * antiSun * 0.10f;

    return saturate(sky);
}

float3 SkyAmbient(float3 normal) {
    float up = saturate(normal.y * 0.5f + 0.5f);
    float3 groundBounce = float3(0.20f, 0.16f, 0.12f);
    float3 skyBounce = float3(0.34f, 0.48f, 0.68f);
    return lerp(groundBounce, skyBounce, up);
}

float FarSmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

float FarRidged(float value, float power) {
    return pow(saturate(1.0f - abs(value)), power);
}

float FarTerrainHeight(float2 xz, out float mountainMask, out float spireMask, out float ravineMask) {
    // Very cheap far-horizon proxy. This intentionally does not try to match
    // editable near voxels exactly; it gives distant cliffs/spires a coherent
    // silhouette without running the full chunk generator per pixel.
    float n0 = sin(dot(xz, float2(0.00173f, 0.00091f)) + 2.1f);
    float n1 = sin(dot(xz, float2(-0.00077f, 0.00148f)) + 5.7f);
    float n2 = sin(dot(xz, float2(0.00320f, -0.00260f)) + 1.3f);
    float n3 = sin(dot(xz, float2(-0.00510f, 0.00430f)) + 8.4f);

    float continent = (n0 * 0.58f + n1 * 0.42f);
    mountainMask = FarSmooth01((continent + 0.20f) * 0.95f);

    float ridgeA = FarRidged(n2, 1.35f);
    float ridgeB = FarRidged(n3, 1.90f);
    float broadValley = FarRidged(sin(dot(xz, float2(0.00092f, 0.00111f)) + 0.4f), 1.2f);
    spireMask = pow(FarRidged(sin(dot(xz, float2(0.0078f, -0.0062f)) + n1), 2.0f), 3.5f) *
        (0.25f + mountainMask * 0.85f);
    ravineMask = 1.0f - smoothstep(0.02f, 0.12f, abs(sin(dot(xz, float2(0.00135f, -0.00105f)) + 2.6f)));

    float d = length(xz - float2(96.0f, 96.0f));
    float originUplift = (1.0f - FarSmooth01(d / 420.0f)) * 170.0f;

    float height = -85.0f;
    height += continent * 175.0f;
    height += ridgeA * (95.0f + mountainMask * 115.0f);
    height += ridgeB * mountainMask * 62.0f;
    height += spireMask * 150.0f;
    height += originUplift;
    height -= broadValley * (90.0f - mountainMask * 30.0f);
    height -= ravineMask * 230.0f;

    float terraceStep = lerp(10.0f, 22.0f, mountainMask);
    float terraced = floor(height / terraceStep) * terraceStep;
    height = lerp(height, terraced, 0.26f + mountainMask * 0.20f);

    return clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
}

uint FarTerrainMaterial(float2 xz, float height, float mountainMask, float spireMask, float ravineMask) {
    float materialNoise = sin(dot(xz, float2(0.013f, 0.017f)) + sin(dot(xz, float2(0.004f, -0.011f)))) * 0.5f + 0.5f;
    if (height < FAR_SEA_LEVEL + 4.0f) {
        return MAT_SAND;
    }
    if (ravineMask > 0.55f && materialNoise > 0.35f) {
        return MAT_STONE;
    }
    if (spireMask > 0.28f || height > 430.0f) {
        return MAT_STONE;
    }
    if (mountainMask > 0.70f || height > 220.0f) {
        return (materialNoise > 0.45f) ? MAT_STONE : MAT_CONCRETE;
    }
    if (materialNoise > 0.84f) {
        return MAT_CONCRETE;
    }
    return MAT_DIRT;
}

float3 FarTerrainNormal(float2 xz) {
    float mountainMaskA, spireMaskA, ravineMaskA;
    float mountainMaskB, spireMaskB, ravineMaskB;
    float hx0 = FarTerrainHeight(xz - float2(3.0f, 0.0f), mountainMaskA, spireMaskA, ravineMaskA);
    float hx1 = FarTerrainHeight(xz + float2(3.0f, 0.0f), mountainMaskB, spireMaskB, ravineMaskB);
    float hz0 = FarTerrainHeight(xz - float2(0.0f, 3.0f), mountainMaskA, spireMaskA, ravineMaskA);
    float hz1 = FarTerrainHeight(xz + float2(0.0f, 3.0f), mountainMaskB, spireMaskB, ravineMaskB);
    return normalize(float3(hx0 - hx1, 6.0f, hz0 - hz1));
}

float MidClipmapUnpackHeight(uint packedSample) {
    return (float)((int)(packedSample & 0xFFFFu) - 32768);
}

uint MidClipmapUnpackMaterial(uint packedSample) {
    return (packedSample >> 16) & 0xFFu;
}

uint HashMidClipmapTileCoord(int ring, int tileX, int tileZ) {
    uint hash = 2166136261u;
    hash = (hash ^ (uint)ring) * 16777619u;
    hash = (hash ^ (uint)tileX) * 16777619u;
    hash = (hash ^ (uint)tileZ) * 16777619u;
    return hash;
}

float MidClipmapRingCellSize(uint ring) {
    return max(frame.midFieldParams.w, 4.0f) * (float)(1u << min(ring, 7u));
}

bool LookupResidentMidClipmapTile(
    float2 xz,
    uint side,
    uint tileCount,
    uint lookupCapacity,
    uint ringCount,
    out uint tileIndex)
{
    tileIndex = 0u;
    if (lookupCapacity == 0u || (lookupCapacity & (lookupCapacity - 1u)) != 0u) {
        return false;
    }

    uint lookupMask = lookupCapacity - 1u;
    uint maxRing = min(ringCount, MID_CLIPMAP_MAX_SHADER_RINGS);
    [loop]
    for (uint ring = 0u; ring < MID_CLIPMAP_MAX_SHADER_RINGS; ++ring) {
        if (ring >= maxRing) {
            break;
        }

        float cellSize = MidClipmapRingCellSize(ring);
        float tileWorldSize = cellSize * (float)(side - 1u);
        int tileX = (int)floor(xz.x / tileWorldSize);
        int tileZ = (int)floor(xz.y / tileWorldSize);
        uint slot = HashMidClipmapTileCoord((int)ring, tileX, tileZ) & lookupMask;

        [unroll]
        for (uint probe = 0u; probe < MID_CLIPMAP_LOOKUP_PROBES; ++probe) {
            uint4 entry = MidClipmapLookup[slot];
            if (entry.w == 0u) {
                break;
            }
            if ((int)entry.x == (int)ring && (int)entry.y == tileX && (int)entry.z == tileZ) {
                uint compactIndex = entry.w - 1u;
                if (compactIndex < tileCount) {
                    tileIndex = compactIndex;
                    return true;
                }
                return false;
            }
            slot = (slot + 1u) & lookupMask;
        }
    }

    return false;
}

bool SampleResidentMidClipmap(float2 xz, out float height, out uint material) {
    height = 0.0f;
    material = MAT_AIR;

    uint4 header = MidClipmapTiles[0];
    if (header.x != MID_CLIPMAP_MAGIC || header.y < 2u || header.z == 0u) {
        return false;
    }

    uint side = min(header.y, 65u);
    uint tileCount = min(header.z, MID_CLIPMAP_MAX_SHADER_TILES);
    uint lookupCapacity = header.w & 0x00FFFFFFu;
    uint ringCount = header.w >> 24u;
    uint samplesPerTile = side * side;
    uint tileIndex = 0u;
    if (!LookupResidentMidClipmapTile(xz, side, tileCount, lookupCapacity, ringCount, tileIndex)) {
        return false;
    }

    uint4 tile = MidClipmapTiles[tileIndex + 1u];
    if (tile.w == 0u) {
        return false;
    }

    int2 origin = int2((int)tile.x, (int)tile.y);
    float cellSize = (float)(tile.z >> 8u);
    if (cellSize < 1.0f) {
        return false;
    }

    float extent = cellSize * (float)(side - 1u);
    float2 localWorld = xz - float2(origin);
    if (localWorld.x < 0.0f || localWorld.y < 0.0f ||
        localWorld.x > extent || localWorld.y > extent) {
        return false;
    }

    float2 sampleCoord = localWorld / cellSize;
    uint sx = (uint)clamp(floor(sampleCoord.x), 0.0f, (float)(side - 2u));
    uint sz = (uint)clamp(floor(sampleCoord.y), 0.0f, (float)(side - 2u));
    float fx = saturate(sampleCoord.x - (float)sx);
    float fz = saturate(sampleCoord.y - (float)sz);
    uint sampleBase = tileIndex * samplesPerTile;
    uint s00 = MidClipmapSamples[sampleBase + sx + sz * side];
    uint s10 = MidClipmapSamples[sampleBase + (sx + 1u) + sz * side];
    uint s01 = MidClipmapSamples[sampleBase + sx + (sz + 1u) * side];
    uint s11 = MidClipmapSamples[sampleBase + (sx + 1u) + (sz + 1u) * side];
    float h0 = lerp(MidClipmapUnpackHeight(s00), MidClipmapUnpackHeight(s10), fx);
    float h1 = lerp(MidClipmapUnpackHeight(s01), MidClipmapUnpackHeight(s11), fx);
    height = lerp(h0, h1, fz);
    material = MidClipmapUnpackMaterial(s00);
    return true;
}

bool LookupResidentMidVoxelBrick(
    int ring,
    int3 brickCoord,
    uint brickCount,
    uint lookupCapacity,
    out uint brickIndex)
{
    brickIndex = 0u;
    if (lookupCapacity == 0u || (lookupCapacity & (lookupCapacity - 1u)) != 0u) {
        return false;
    }

    uint hash = 2166136261u;
    hash = (hash ^ (uint)ring) * 16777619u;
    hash = (hash ^ (uint)brickCoord.x) * 16777619u;
    hash = (hash ^ (uint)brickCoord.y) * 16777619u;
    hash = (hash ^ (uint)brickCoord.z) * 16777619u;

    uint slot = hash & (lookupCapacity - 1u);
    [unroll]
    for (uint probe = 0u; probe < MID_CLIPMAP_LOOKUP_PROBES; ++probe) {
        uint4 entry = MidVoxelClipmapLookup[slot];
        if (entry.w == 0u) {
            return false;
        }
        uint entryRing = entry.w >> 24u;
        uint compactPlusOne = entry.w & 0x00FFFFFFu;
        if ((int)entry.x == brickCoord.x &&
            (int)entry.y == brickCoord.y &&
            (int)entry.z == brickCoord.z &&
            (int)entryRing == ring) {
            uint compactIndex = compactPlusOne - 1u;
            if (compactIndex < brickCount) {
                brickIndex = compactIndex;
                return true;
            }
            return false;
        }
        slot = (slot + 1u) & (lookupCapacity - 1u);
    }

    return false;
}

bool SampleResidentMidVoxel(float3 worldPos, uint ring, out uint voxel) {
    voxel = PackVoxel(MAT_AIR, 0, 0, 0);

    uint4 header = MidVoxelClipmapMetadata[0];
    if (header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u || header.y != SPARSE_BRICK_SIZE) {
        return false;
    }

    uint brickCount = min(header.z, MID_VOXEL_CLIPMAP_MAX_BRICKS);
    uint lookupCapacity = header.w & 0x00FFFFFFu;
    float cellSize = MidClipmapRingCellSize(ring);
    float brickWorldSize = cellSize * (float)SPARSE_BRICK_SIZE;
    int3 brickCoord = int3(floor(worldPos / brickWorldSize));

    uint brickIndex;
    if (!LookupResidentMidVoxelBrick((int)ring, brickCoord, brickCount, lookupCapacity, brickIndex)) {
        return false;
    }

    uint4 metadata = MidVoxelClipmapMetadata[brickIndex + 1u];
    int3 origin = int3((int)metadata.x, (int)metadata.y, (int)metadata.z);
    float storedCellSize = (float)(metadata.w >> 8u);
    if (storedCellSize < 1.0f) {
        return false;
    }

    float3 localFloat = (worldPos - float3(origin)) / storedCellSize;
    if (any(localFloat < 0.0f) || any(localFloat >= (float)SPARSE_BRICK_SIZE)) {
        return false;
    }

    uint3 local = uint3(localFloat);
    uint localIndex = local.x + local.y * SPARSE_BRICK_SIZE + local.z * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
    voxel = MidVoxelClipmapSamples[brickIndex * SPARSE_BRICK_VOXEL_COUNT + localIndex];
    return true;
}

bool RaymarchMidVoxelClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit voxelHit) {
    voxelHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    uint4 header = MidVoxelClipmapMetadata[0];
    if (frame.midFieldParams.x < 0.5f || header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u) {
        return false;
    }
    if (rayDir.y > 0.42f || rayDir.y < -0.72f) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);
    int budget = frame.renderBudgetParams.z < 0.55f ? 56 : (frame.renderBudgetParams.z < 0.85f ? 88 : 128);

    [loop]
    for (int i = 0; i < budget && t < endDistance; ++i) {
        uint ring = min((uint)floor(saturate((t - startDistance) / max(endDistance - startDistance, 1.0f)) *
            max((float)(header.w >> 24u), 1.0f)), (uint)max((int)(header.w >> 24u) - 1, 0));
        float cellSize = MidClipmapRingCellSize(ring);
        float3 pos = rayOrigin + rayDir * t;
        uint voxel;
        if (SampleResidentMidVoxel(pos, ring, voxel)) {
            uint material = GetMaterial(voxel);
            if (material != MAT_AIR) {
                float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
                float3 normal = FarTerrainNormal(pos.xz);
                float ndotl = saturate(dot(normal, SkySunDirection()));
                float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.34f + ndotl * 0.84f);
                float fogFactor = saturate((t - startDistance) / max(endDistance - startDistance, 1.0f));
                color = lerp(color, SkyColor(rayDir), fogFactor * 0.62f);
                if (frame.debugMode == 9u) {
                    color = lerp(color, float3(1.0f, 0.58f, 0.10f), 0.52f);
                }
                voxelHit = MakeHit(float4(color, baseColor.a), t);
                return true;
            }
            t += max(cellSize * 0.80f, 4.0f);
        } else {
            t += max(cellSize * 1.50f, 12.0f);
        }
    }

    return false;
}

bool MidClipmapHeightMaterial(float2 xz, out float height, out uint material, out float mountainMask, out float spireMask, out float ravineMask) {
    if (SampleResidentMidClipmap(xz, height, material)) {
        mountainMask = 0.0f;
        spireMask = 0.0f;
        ravineMask = 0.0f;
        return true;
    }

    height = FarTerrainHeight(xz, mountainMask, spireMask, ravineMask);
    material = FarTerrainMaterial(xz, height, mountainMask, spireMask, ravineMask);
    return false;
}

float3 MidClipmapNormal(float2 xz) {
    float hL, hR, hD, hU;
    uint mat;
    if (SampleResidentMidClipmap(xz - float2(3.0f, 0.0f), hL, mat) &&
        SampleResidentMidClipmap(xz + float2(3.0f, 0.0f), hR, mat) &&
        SampleResidentMidClipmap(xz - float2(0.0f, 3.0f), hD, mat) &&
        SampleResidentMidClipmap(xz + float2(0.0f, 3.0f), hU, mat)) {
        return normalize(float3(hL - hR, 6.0f, hD - hU));
    }
    return FarTerrainNormal(xz);
}

float MidClipmapCellSize(float distanceFromCamera) {
    const float minCell = max(frame.midFieldParams.w, 4.0f);
    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    float ring = saturate((distanceFromCamera - startDistance) / max(frame.midFieldParams.z - startDistance, 1.0f));
    // Four implicit clipmap rings. This is procedural for now, but it follows
    // the intended ownership contract: near sparse bricks own the editable
    // volume; this mid layer owns only ray segments beyond the near exit.
    if (ring < 0.22f) return minCell;
    if (ring < 0.48f) return minCell * 2.0f;
    if (ring < 0.74f) return minCell * 4.0f;
    return minCell * 8.0f;
}

bool RaymarchMidClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit midHit) {
    midHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (frame.midFieldParams.x < 0.5f || frame.renderBudgetParams.z < 0.20f) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);

    // Transition contract: this layer is terrain context after the near sparse
    // window. Do not draw it for steep rays where it would appear as detached
    // ceilings or terrain punching through editable near holes.
    if (rayDir.y > 0.30f || rayDir.y < -0.64f) {
        return false;
    }

    float3 previousPos = rayOrigin + rayDir * t;
    float mountainMask, spireMask, ravineMask;
    uint previousMaterial;
    float previousHeight;
    MidClipmapHeightMaterial(previousPos.xz, previousHeight, previousMaterial, mountainMask, spireMask, ravineMask);
    float previousSigned = previousPos.y - previousHeight;

    int stepBudget = frame.renderBudgetParams.z < 0.55f ? 28 : (frame.renderBudgetParams.z < 0.85f ? 44 : 64);
    [loop]
    for (int i = 0; i < stepBudget && t < endDistance; ++i) {
        float cellSize = MidClipmapCellSize(t);
        float stepSize = max(cellSize * 0.90f, 8.0f);
        float nextT = min(t + stepSize, endDistance);
        float3 pos = rayOrigin + rayDir * nextT;
        uint material;
        float height;
        bool residentSample = MidClipmapHeightMaterial(pos.xz, height, material, mountainMask, spireMask, ravineMask);
        float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = t;
            float hi = nextT;
            [unroll]
            for (int refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                uint midMaterial;
                float midHeight;
                bool midResident = MidClipmapHeightMaterial(midPos.xz, midHeight, midMaterial, mm, sm, rm);
                if (midPos.y > midHeight) {
                    lo = mid;
                } else {
                    hi = mid;
                    mountainMask = mm;
                    spireMask = sm;
                    ravineMask = rm;
                    height = midHeight;
                    material = midMaterial;
                    residentSample = midResident;
                }
            }

            float hitT = hi;
            float3 hitPos = rayOrigin + rayDir * hitT;
            float3 normal = MidClipmapNormal(hitPos.xz);
            if (!residentSample) {
                material = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            }
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);

            float3 lightDir = SkySunDirection();
            float ndotl = saturate(dot(normal, lightDir));
            float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.36f + ndotl * 0.86f);
            float fogFactor = saturate((hitT - startDistance) / max(endDistance - startDistance, 1.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.58f);

            // Debug mode 8 highlights mid clipmap ownership in blue-green.
            if (frame.debugMode == 8u) {
                color = lerp(color, float3(0.15f, 0.75f, 1.0f), 0.45f);
            }

            midHit = MakeHit(float4(color, 1.0f), hitT);
            return true;
        }

        t = nextT;
        previousSigned = signedDistance;
    }

    return false;
}

uint FarVoxelChildNodeIndex(uint childBase, uint childMask, uint childOrdinal) {
    uint precedingMask = childMask & ((1u << childOrdinal) - 1u);
    return childBase + countbits(precedingMask);
}

bool TraverseFarVoxelPage(
    float3 rayOrigin,
    float3 rayDir,
    uint rootNode,
    float3 pageMin,
    float pageSize,
    float startDist,
    inout float nearestT,
    inout float3 nearestNormal,
    inout uint nearestMaterial)
{
    uint nodeStack[64];
    float3 minStack[64];
    float sizeStack[64];
    int stackCount = 0;

    nodeStack[stackCount] = rootNode;
    minStack[stackCount] = pageMin;
    sizeStack[stackCount] = pageSize;
    stackCount++;

    bool hit = false;
    [loop]
    while (stackCount > 0) {
        stackCount--;
        uint nodeIndex = nodeStack[stackCount];
        float3 nodeMin = minStack[stackCount];
        float nodeSize = sizeStack[stackCount];

        if (nodeIndex >= (uint)frame.farFieldParams.z) {
            continue;
        }

        float tNear, tFar;
        float3 boxNormal;
        if (!IntersectBoxWithNormal(rayOrigin, rayDir, nodeMin, nodeMin + nodeSize, tNear, tFar, boxNormal)) {
            continue;
        }
        if (tFar < startDist || tNear > nearestT) {
            continue;
        }

        FarVoxelNode node = FarVoxelNodes[nodeIndex];
        if ((node.flags & 1u) != 0u || node.childMask == 0u || node.childBase == 0xFFFFFFFFu) {
            float candidateT = max(tNear, startDist);
            if (candidateT < nearestT) {
                nearestT = candidateT;
                nearestNormal = boxNormal;
                nearestMaterial = node.material;
                hit = true;
            }
            continue;
        }

        float childSize = nodeSize * 0.5f;
        [unroll]
        for (uint child = 0; child < 8; ++child) {
            if ((node.childMask & (1u << child)) == 0u || stackCount >= 64) {
                continue;
            }

            float3 childMin = nodeMin + float3(
                (child & 1u) ? childSize : 0.0f,
                (child & 2u) ? childSize : 0.0f,
                (child & 4u) ? childSize : 0.0f);

            uint childNode = FarVoxelChildNodeIndex(node.childBase, node.childMask, child);
            nodeStack[stackCount] = childNode;
            minStack[stackCount] = childMin;
            sizeStack[stackCount] = childSize;
            stackCount++;
        }
    }

    return hit;
}

bool RaymarchSparseFarField(float3 rayOrigin, float3 rayDir, float startDist, out RayHit farHit) {
    farHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (frame.farFieldParams.x < 0.5f || frame.renderBudgetParams.z < 0.25f ||
        frame.farFieldParams.y < 1.0f || frame.farFieldParams.z < 1.0f) {
        return false;
    }
    if (rayDir.y > 0.18f || rayDir.y < -0.42f) {
        return false;
    }

    const uint pageCount = (uint)frame.farFieldParams.y;
    const int pageRadius = (int)frame.farFieldGridParams.x;
    const int pageSide = (int)frame.farFieldGridParams.y;
    float pageSize = max(frame.farFieldParams.w, 1.0f);
    if (pageRadius <= 0 || pageSide <= 0) {
        return false;
    }

    // The first SVO integration scanned every page for every far-field pixel.
    // That is correct but far too expensive once the page forest grows. This
    // top-level 2D DDA only probes the page cells crossed by the ray in X/Z,
    // then traverses the octree for those candidate pages.
    const float farMaxDist = 10400.0f;
    float t = max(startDist, 32.0f);
    float nearestT = 1e20f;
    float3 nearestNormal = float3(0, 1, 0);
    uint nearestMaterial = MAT_STONE;

    float2 rayXZ = rayDir.xz;
    float2 originXZ = rayOrigin.xz;
    int maxPageSteps = frame.renderBudgetParams.z < 0.6f ? 18 : (frame.renderBudgetParams.z < 0.9f ? 28 : 40);

    [loop]
    for (int stepIndex = 0; stepIndex < maxPageSteps && t < farMaxDist && t < nearestT; ++stepIndex) {
        float3 pos = rayOrigin + rayDir * t;
        int px = (int)floor(pos.x / pageSize);
        int pz = (int)floor(pos.z / pageSize);

        if (px >= -pageRadius && px <= pageRadius &&
            pz >= -pageRadius && pz <= pageRadius) {
            uint denseIndex = (uint)((pz + pageRadius) * pageSide + (px + pageRadius));
            uint pageIndex = FarVoxelPageIndex[denseIndex];

            if (pageIndex != 0xFFFFFFFFu && pageIndex < pageCount) {
                FarVoxelPage page = FarVoxelPages[pageIndex];
                float3 pageMin = float3((float)page.originX, (float)page.originY, (float)page.originZ);

                float tNear, tFar;
                if (IntersectBox(rayOrigin, rayDir, pageMin, pageMin + pageSize, tNear, tFar) &&
                    tFar >= startDist &&
                    tNear <= nearestT) {
                    TraverseFarVoxelPage(
                        rayOrigin,
                        rayDir,
                        page.rootNode,
                        pageMin,
                        pageSize,
                        startDist,
                        nearestT,
                        nearestNormal,
                        nearestMaterial);
                }
            }
        }

        float nextTx = 1e20f;
        if (abs(rayXZ.x) > 0.0001f) {
            float boundaryX = ((rayXZ.x > 0.0f) ? (float)(px + 1) : (float)px) * pageSize;
            nextTx = (boundaryX - originXZ.x) / rayXZ.x;
        }

        float nextTz = 1e20f;
        if (abs(rayXZ.y) > 0.0001f) {
            float boundaryZ = ((rayXZ.y > 0.0f) ? (float)(pz + 1) : (float)pz) * pageSize;
            nextTz = (boundaryZ - originXZ.y) / rayXZ.y;
        }

        float nextT = min(nextTx, nextTz);
        if (nextT <= t + 0.5f || nextT >= 1e19f) {
            t += max(64.0f, pageSize * 0.125f);
        } else {
            t = nextT + 0.5f;
        }
    }

    if (nearestT >= 1e19f) {
        return false;
    }

    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (nearestMaterial + 0.5f) / 256.0f, 0);
    float3 lightDir = SkySunDirection();
    float ndotl = saturate(dot(nearestNormal, lightDir));
    float3 color = baseColor.rgb * (SkyAmbient(nearestNormal) * 0.34f + ndotl * 0.82f);
    float fogFactor = saturate((nearestT - 900.0f) / (10400.0f - 900.0f));
    color = lerp(color, SkyColor(rayDir), fogFactor * 0.72f);
    farHit = MakeHit(float4(color, 1.0f), nearestT);
    return true;
}

bool FarSvoCellOccupied(float3 cellMin, float cellSize) {
    // First SVO pass: implicit node occupancy over the far procedural terrain.
    // This is intentionally read-only and visual-only. A node is occupied when
    // its world-space AABB intersects the terrain volume described by sampled
    // heightfield bounds. Empty cells can be skipped like sparse octree nodes.
    float3 cellMax = cellMin + cellSize;
    if (cellMax.y < FAR_TERRAIN_MIN_HEIGHT || cellMin.y > FAR_TERRAIN_MAX_HEIGHT + cellSize) {
        return false;
    }

    float2 c0 = cellMin.xz;
    float2 c1 = cellMin.xz + float2(cellSize, 0.0f);
    float2 c2 = cellMin.xz + float2(0.0f, cellSize);
    float2 c3 = cellMin.xz + float2(cellSize, cellSize);
    float2 cc = cellMin.xz + cellSize * 0.5f;

    float mm, sm, rm;
    float h0 = FarTerrainHeight(c0, mm, sm, rm);
    float h1 = FarTerrainHeight(c1, mm, sm, rm);
    float h2 = FarTerrainHeight(c2, mm, sm, rm);
    float h3 = FarTerrainHeight(c3, mm, sm, rm);
    float hc = FarTerrainHeight(cc, mm, sm, rm);

    float maxH = max(max(max(h0, h1), max(h2, h3)), hc) + cellSize * 0.35f;
    return cellMin.y <= maxH && cellMax.y >= FAR_TERRAIN_MIN_HEIGHT;
}

float FarSvoCellExitDistance(float3 rayOrigin, float3 rayDir, float3 cellMin, float cellSize, float currentT) {
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, cellMin, cellMin + cellSize, tMin, tMax)) {
        return currentT + cellSize;
    }
    return max(currentT + 1.0f, tMax + 0.75f);
}

float FarSvoSuggestedStep(float3 rayOrigin, float3 rayDir, float currentT) {
    float3 pos = rayOrigin + rayDir * currentT;
    float cellSize = FAR_SVO_ROOT_CELL_SIZE;

    [unroll]
    for (int level = 0; level < FAR_SVO_MAX_LEVELS; ++level) {
        float3 cellMin = floor(pos / cellSize) * cellSize;
        if (!FarSvoCellOccupied(cellMin, cellSize)) {
            return min(FarSvoCellExitDistance(rayOrigin, rayDir, cellMin, cellSize, currentT) - currentT,
                       cellSize * 1.25f);
        }
        cellSize *= 0.5f;
    }

    return max(FAR_SVO_MIN_CELL_SIZE, cellSize);
}

bool RaymarchFarTerrain(float3 rayOrigin, float3 rayDir, float startDist, out RayHit farHit) {
    farHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (!FAR_TERRAIN_HORIZON_ENABLED || frame.renderBudgetParams.z < 0.15f) {
        return false;
    }

    // Keep the fallback in the horizon band. Steep downward/upward rays should
    // show only the dense editable window or sky; otherwise the cheap heightfield
    // can look like an overhead sheet when the dense window has an air gap.
    if (rayDir.y > 0.18f || rayDir.y < -0.42f) {
        return false;
    }

    const float farMaxDist = 10400.0f;
    float t = max(startDist, 900.0f);
    float previousT = t;
    float3 previousPos = rayOrigin + rayDir * t;
    float mountainMask, spireMask, ravineMask;
    float previousHeight = FarTerrainHeight(previousPos.xz, mountainMask, spireMask, ravineMask);
    float previousSigned = previousPos.y - previousHeight;

    // This is a continuity fallback behind the page-indexed SVO, not the main
    // far renderer. Keep it cheap enough that sky/horizon pixels cannot become
    // the frame-time bottleneck.
    int farStepBudget = frame.renderBudgetParams.z < 0.6f ? 24 : (frame.renderBudgetParams.z < 0.9f ? 36 : 48);
    [loop]
    for (int i = 0; i < farStepBudget && t < farMaxDist; ++i) {
        float distanceStep = lerp(96.0f, 360.0f, saturate(t / farMaxDist));
        float svoStep = frame.renderBudgetParams.z > 0.92f
            ? FarSvoSuggestedStep(rayOrigin, rayDir, t)
            : distanceStep;
        float stepSize = max(FAR_SVO_MIN_CELL_SIZE, max(svoStep, distanceStep));
        t += stepSize;

        float3 pos = rayOrigin + rayDir * t;
        float height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = previousT;
            float hi = t;
            [unroll]
            for (int refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                float midHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
                if (midPos.y > midHeight) {
                    lo = mid;
                } else {
                    hi = mid;
                    mountainMask = mm;
                    spireMask = sm;
                    ravineMask = rm;
                    height = midHeight;
                }
            }

            float hitT = hi;
            float3 hitPos = rayOrigin + rayDir * hitT;
            float3 normal = FarTerrainNormal(hitPos.xz);
            uint material = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);

            float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
            float lighting = max(dot(normal, lightDir), 0.18f);
            float3 color = baseColor.rgb * lighting;

            // Extra fog hides the fact that this is a heightfield fallback, not
            // the exact editable voxel buffer.
            float fogFactor = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.90f + 0.10f);
            farHit = MakeHit(float4(color, 1.0f), hitT);
            return true;
        }

        previousT = t;
        previousSigned = signedDistance;
    }

    return false;
}

// DDA Raymarcher
RayHit Raymarch(float3 rayOrigin, float3 rayDir) {
    // Must cover the diagonal of the moving render window. Keep this generous:
    // shortening it can make startup look like a black/empty screen while chunks
    // are visible but beyond the ray budget.
    const bool sparseNearActive = frame.sparseNearParams.x > 0.5f;
    float maxDist = sparseNearActive
        ? clamp(frame.renderBudgetParams.x, 64.0f, 3000.0f)
        : clamp(frame.renderBudgetParams.x, 900.0f, 3000.0f);
    int maxSteps = sparseNearActive
        ? clamp((int)frame.renderBudgetParams.y, 4, 2048)
        : clamp((int)frame.renderBudgetParams.y, 640, 2048);

    // CRITICAL FIX: Grid bounds in WORLD coordinates (not buffer coordinates)
    // The buffer is a moving window, so grid bounds = regionOrigin + bufferSize
    float3 gridMin = frame.regionOrigin.xyz;
    float3 gridMax = frame.regionOrigin.xyz + float3(frame.gridSizeX, frame.gridSizeY, frame.gridSizeZ);

    // Find ray entry point into grid
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, gridMin, gridMax, tMin, tMax)) {
        if (frame.debugMode == 43u) {
            return MakeHit(float4(0.18f, 0.08f, 0.20f, 1.0f), 1e20f);
        }
        RayHit farHit;
        if (RaymarchMidVoxelClipmap(rayOrigin, rayDir, 32.0f, farHit)) {
            return farHit;
        }
        if (RaymarchMidClipmap(rayOrigin, rayDir, 32.0f, farHit)) {
            return farHit;
        }
        if (RaymarchSparseFarField(rayOrigin, rayDir, 32.0f, farHit)) {
            return farHit;
        }
        if (RaymarchFarTerrain(rayOrigin, rayDir, 32.0f, farHit)) {
            return farHit;
        }
        return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    }
    if (frame.debugMode == 43u) {
        float shade = saturate((tMax - max(tMin, 0.0f)) / 1024.0f);
        return MakeHit(float4(0.05f + shade, 0.12f, 0.26f, 1.0f), 1e20f);
    }

    // Start raymarching from grid entry point (or ray origin if inside grid).
    // Clamp traversal to the box exit so sky/horizon rays stop as soon as they
    // leave the render volume instead of burning the full maxDist budget.
    const float rayEpsilon = 0.001f;
    float entryDist = max(tMin, 0.0f) + rayEpsilon;
    float maxMarchDist = min(maxDist, max(tMax - entryDist, 0.0f));
    float3 startPos = rayOrigin + rayDir * entryDist;

    // Start position in voxel grid
    int3 voxelPos = int3(floor(startPos));

    // DDA setup
    float3 safeRayDir = float3(
        abs(rayDir.x) < 1e-6f ? (rayDir.x < 0.0f ? -1e-6f : 1e-6f) : rayDir.x,
        abs(rayDir.y) < 1e-6f ? (rayDir.y < 0.0f ? -1e-6f : 1e-6f) : rayDir.y,
        abs(rayDir.z) < 1e-6f ? (rayDir.z < 0.0f ? -1e-6f : 1e-6f) : rayDir.z);
    float3 deltaDist = abs(1.0f / safeRayDir);
    int3 step = int3(sign(rayDir));

    float3 sideDist;
    sideDist.x = (rayDir.x > 0.0f) ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = (rayDir.y > 0.0f) ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = (rayDir.z > 0.0f) ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    float3 normal = float3(0, 1, 0);
    float dist = 0.0f;
    const bool sparseOnlyMode = frame.sparseNearParams.x > 0.5f && (((uint)frame.sparseNearParams.w & 1u) != 0u);
    // Sparse mode must traverse in brick/subbrick-sized jumps. Per-voxel page
    // table probing across a full-screen raymarch can saturate the GPU before
    // the first frame fence retires.
    const bool sparseSkipAcceleration = sparseOnlyMode;

    // DDA traversal
    SparseRayCache sparseCache;
    sparseCache.brickCoord = int3(0, 0, 0);
    sparseCache.pageIndex = SPARSE_INVALID_PAGE;
    sparseCache.generation = 0u;
    sparseCache.valid = 0u;
    sparseCache.hasEntry = 0u;

    if (frame.debugMode == 44u) {
        bool voxelFromSparse = false;
        bool sparseMissing = false;
        uint sparseSampleState = SPARSE_SAMPLE_MISSING;
        uint voxel = GetVoxel(voxelPos, sparseCache, voxelFromSparse, sparseMissing, sparseSampleState);
        uint material = GetMaterial(voxel);
        float3 color = material != MAT_AIR ? float3(0.2f, 0.9f, 0.35f) :
            (sparseMissing ? float3(0.9f, 0.35f, 0.20f) : float3(0.12f, 0.16f, 0.22f));
        return MakeHit(float4(color, 1.0f), 1e20f);
    }

    if (frame.debugMode == 46u) {
        SparseSurfaceBrickRange surfaceRange;
        const int3 surfaceBrick = int3(
            FloorDiv16(voxelPos.x),
            FloorDiv16(voxelPos.y),
            FloorDiv16(voxelPos.z));
        if (LookupSparseSurfaceRange(surfaceBrick, surfaceRange)) {
            const float fill = saturate((float)surfaceRange.faceCount / 512.0f);
            return MakeHit(float4(0.05f, 0.25f + fill * 0.70f, 0.95f, 1.0f), 1e20f);
        }
        return MakeHit(float4(0.12f, 0.06f, 0.18f, 1.0f), 1e20f);
    }

    if (frame.debugMode == 45u) {
        maxSteps = min(maxSteps, 1);
    } else if (frame.debugMode == 47u) {
        maxSteps = min(maxSteps, 64);
    }

    for (int i = 0; i < maxSteps; i++) {
        bool voxelFromSparse = false;
        bool sparseMissing = false;
        uint sparseSampleState = SPARSE_SAMPLE_MISSING;
        uint voxel = GetVoxel(voxelPos, sparseCache, voxelFromSparse, sparseMissing, sparseSampleState);
        uint material = GetMaterial(voxel);

        if (sparseSkipAcceleration && sparseMissing) {
            float3 currentPos = startPos + rayDir * dist;
            float skipDist = DistanceToSparseBrickExit(voxelPos, currentPos, rayDir);
            dist += skipDist;
            if (dist > maxMarchDist) break;

            RestartSparseDdaAtDistance(startPos, rayDir, deltaDist, dist, voxelPos, sideDist);
            continue;
        }

        // Hit non-air voxel?
        if (material != MAT_AIR) {
            // Sample material color from palette
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);

            // Simple skybox/IBL-style lighting: direct sun plus directional
            // sky/ground ambient so shaded cliffs still read in the vertical world.
            float3 lightDir = SkySunDirection();
            float ndotl = saturate(dot(normal, lightDir));
            float3 ambient = SkyAmbient(normal) * 0.35f;

            // Add slight variant-based color variation
            uint variant = GetVariant(voxel);
            float variantNoise = (variant / 255.0f) * 0.1f - 0.05f;  // +/- 5%

            float3 finalColor = baseColor.rgb * (ambient + ndotl * 0.86f) * (1.0f + variantNoise);
            if (frame.debugMode == 7u) {
                float3 sparseTint = voxelFromSparse ? float3(0.35f, 1.0f, 0.42f) : float3(1.0f, 0.38f, 0.28f);
                finalColor = lerp(finalColor, sparseTint, 0.55f);
            }

            // Depth fog
            float fogFactor = saturate(dist / maxDist);
            float3 fogColor = SkyColor(rayDir);
            finalColor = lerp(finalColor, fogColor, fogFactor * 0.5f);

            // Use material's alpha from palette (enables transparency for water, glass, etc.)
            return MakeHit(float4(finalColor, baseColor.a), entryDist + dist);
        }

        if (sparseSkipAcceleration && voxelFromSparse && sparseSampleState >= SPARSE_SAMPLE_EMPTY_SUBBRICK) {
            float3 currentPos = startPos + rayDir * dist;
            float skipDist = sparseSampleState == SPARSE_SAMPLE_EMPTY_BRICK
                ? DistanceToSparseBrickExit(voxelPos, currentPos, rayDir)
                : DistanceToSparseSubbrickExit(voxelPos, currentPos, rayDir);
            dist += skipDist;
            if (dist > maxMarchDist) break;

            RestartSparseDdaAtDistance(startPos, rayDir, deltaDist, dist, voxelPos, sideDist);
            continue;
        }

        // Step to next voxel boundary
        float nextDist;
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                nextDist = sideDist.x;
                voxelPos.x += step.x;
                sideDist.x += deltaDist.x;
                normal = float3(-step.x, 0, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = float3(0, 0, -step.z);
                dist = nextDist;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                nextDist = sideDist.y;
                voxelPos.y += step.y;
                sideDist.y += deltaDist.y;
                normal = float3(0, -step.y, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = float3(0, 0, -step.z);
                dist = nextDist;
            }
        }

        if (dist > maxMarchDist) break;
    }

    if (frame.debugMode == 45u || frame.debugMode == 46u || frame.debugMode == 47u) {
        return MakeHit(float4(0.05f, 0.18f, 0.12f, 1.0f), 1e20f);
    }

    // If the ray cleanly exits the dense editable cache, continue into the
    // far-field renderer from just beyond the cache. This preserves the earlier
    // protection against drawing far terrain through missing near chunks, while
    // avoiding a hard sky cutoff when the camera pans past the near window.
    if (entryDist + dist >= tMax - 1.0f) {
        RayHit farHit;
        float farStart = max(tMax + 8.0f, entryDist + dist);
        if (RaymarchMidVoxelClipmap(rayOrigin, rayDir, farStart, farHit)) {
            return farHit;
        }
        if (RaymarchMidClipmap(rayOrigin, rayDir, farStart, farHit)) {
            return farHit;
        }
        // Do not invoke the page-indexed SVO for every sky pixel that has
        // already crossed the dense cache; that path is correct but too costly
        // as a background fill. The cheaper heightfield fallback is enough for
        // continuity behind the editable window.
        if (rayDir.y > -0.18f && rayDir.y < 0.10f &&
            RaymarchFarTerrain(rayOrigin, rayDir, farStart, farHit)) {
            return farHit;
        }
    }

    return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
}

bool IntersectAvatarBox(float3 localOrigin, float3 localDir, float3 boxMin, float3 boxMax, out float tNear, out float3 normal) {
    float3 invDir = 1.0f / localDir;
    float3 t0 = (boxMin - localOrigin) * invDir;
    float3 t1 = (boxMax - localOrigin) * invDir;
    float3 tMin3 = min(t0, t1);
    float3 tMax3 = max(t0, t1);

    tNear = max(max(tMin3.x, tMin3.y), tMin3.z);
    float tFar = min(min(tMax3.x, tMax3.y), tMax3.z);
    if (tFar < max(tNear, 0.0f)) {
        return false;
    }

    if (tNear < 0.0f) {
        tNear = tFar;
    }

    float3 hit = localOrigin + localDir * tNear;
    float3 dMin = abs(hit - boxMin);
    float3 dMax = abs(hit - boxMax);
    float best = dMin.x;
    normal = float3(-1, 0, 0);
    if (dMax.x < best) { best = dMax.x; normal = float3(1, 0, 0); }
    if (dMin.y < best) { best = dMin.y; normal = float3(0, -1, 0); }
    if (dMax.y < best) { best = dMax.y; normal = float3(0, 1, 0); }
    if (dMin.z < best) { best = dMin.z; normal = float3(0, 0, -1); }
    if (dMax.z < best) { normal = float3(0, 0, 1); }
    return true;
}

void TestAvatarPart(
    float3 localOrigin,
    float3 localDir,
    float3 boxMin,
    float3 boxMax,
    float3 color,
    inout float nearestT,
    inout float3 nearestNormal,
    inout float3 nearestColor)
{
    float t;
    float3 normal;
    if (IntersectAvatarBox(localOrigin, localDir, boxMin, boxMax, t, normal) && t < nearestT) {
        nearestT = t;
        nearestNormal = normal;
        nearestColor = color;
    }
}

bool RenderBlockCharacter(float3 rayOrigin, float3 rayDir, out float tHit, out float4 color) {
    tHit = 1e20f;
    color = float4(0, 0, 0, 0);

    if (frame.characterPosition.w < 0.5f) {
        return false;
    }

    float3 feet = frame.characterPosition.xyz;
    float3 forward = normalize(float3(frame.cameraForward.x, 0.0f, frame.cameraForward.z));
    if (length(forward) < 0.001f) {
        forward = float3(0, 0, 1);
    }
    float3 right = normalize(float3(frame.cameraRight.x, 0.0f, frame.cameraRight.z));

    float3 rel = rayOrigin - feet;
    float3 localOrigin = float3(dot(rel, right), rel.y, dot(rel, forward));
    float3 localDir = normalize(float3(dot(rayDir, right), rayDir.y, dot(rayDir, forward)));

    float3 nearestNormal = float3(0, 1, 0);
    float3 nearestColor = float3(0.2f, 0.45f, 0.95f);

    // CC0 blocky-character style: head, torso, arms, and legs as simple cuboids.
    TestAvatarPart(localOrigin, localDir, float3(-0.65f, 0.00f, -0.35f), float3(-0.08f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.08f, 0.00f, -0.35f), float3( 0.65f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-0.95f, 2.70f, -0.42f), float3( 0.95f, 5.35f, 0.42f), float3(0.18f, 0.55f, 0.95f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.55f, 2.45f, -0.34f), float3(-0.98f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.98f, 2.45f, -0.34f), float3( 1.55f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.05f, 5.25f, -0.58f), float3( 1.05f, 7.20f, 0.58f), float3(0.86f, 0.64f, 0.42f), tHit, nearestNormal, nearestColor);

    if (tHit >= 1e19f) {
        return false;
    }

    float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
    float lighting = max(dot(nearestNormal, lightDir), 0.25f);
    color = float4(nearestColor * lighting, 1.0f);
    return true;
}

// Render brush preview as semi-transparent overlay
float4 RenderBrushPreview(float3 rayOrigin, float3 rayDir, float3 brushCenter, float brushRadius, uint brushShape, float3 baseColor) {
    // Safety check: Don't render if camera is too close to or inside the brush
    float distToCenter = length(rayOrigin - brushCenter);
    if (distToCenter < brushRadius * 1.5f) {
        // Camera is too close - don't render to avoid visual glitches
        return float4(0, 0, 0, 0);
    }

    // Ray-sphere intersection for brush preview
    float3 oc = rayOrigin - brushCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - brushRadius * brushRadius;
    float discriminant = b * b - c;

    if (discriminant < 0.0f) {
        return float4(0, 0, 0, 0);  // No intersection
    }

    float t = -b - sqrt(discriminant);
    if (t < 0.0f) t = -b + sqrt(discriminant);  // Inside sphere
    if (t < 0.0f) return float4(0, 0, 0, 0);    // Behind camera

    // Don't render if intersection is too close (less than 2 voxels away)
    if (t < 2.0f) {
        return float4(0, 0, 0, 0);
    }

    float3 hitPoint = rayOrigin + rayDir * t;

    // For sphere shape, use distance-based alpha
    if (brushShape == 0) {  // Sphere
        float dist = length(hitPoint - brushCenter);
        float normalizedDist = dist / brushRadius;

        // Create wireframe effect - more opaque at edges
        float edgeFactor = abs(normalizedDist - 0.95f) < 0.05f ? 0.6f : 0.15f;

        // Fresnel-like effect for better visibility
        float3 normal = normalize(hitPoint - brushCenter);
        float fresnel = pow(1.0f - abs(dot(normal, rayDir)), 2.0f);
        float alpha = lerp(edgeFactor, 0.4f, fresnel);

        return float4(baseColor, alpha);
    }
    else {  // Cube or cylinder - simple semi-transparent rendering
        return float4(baseColor, 0.25f);
    }
}

float4 main(PSInput input) : SV_Target {
    // Camera data from constant buffer
    float3 cameraPos = frame.cameraPosition.xyz;
    float3 forward = frame.cameraForward.xyz;
    float3 right = frame.cameraRight.xyz;
    float3 up = frame.cameraUp.xyz;
    float fov = frame.cameraPosition.w;
    float aspectRatio = frame.cameraForward.w;

    // Ray direction from UV
    float2 ndc = input.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;  // Flip Y

    float tanHalfFov = tan(fov * 0.5f);

    float3 rayDir = normalize(
        forward +
        right * ndc.x * tanHalfFov * aspectRatio +
        up * ndc.y * tanHalfFov
    );

    if (frame.debugMode == 42u) {
        return float4(0.08f, 0.16f, 0.22f, 1.0f);
    }
    if (frame.debugMode == 48u) {
        uint voxel = SparseBrickVoxelPool[0];
        uint material = GetMaterial(voxel);
        float shade = saturate((float)material / 16.0f);
        return float4(0.05f + shade, 0.10f, 0.18f, 1.0f);
    }

    // Render voxel world
    RayHit worldHit = Raymarch(cameraPos, rayDir);
    float4 voxelColor = worldHit.color;

    float avatarT;
    float4 avatarColor;
    if (RenderBlockCharacter(cameraPos, rayDir, avatarT, avatarColor) && avatarT < worldHit.distance) {
        voxelColor = avatarColor;
    }

    // Render brush preview overlay if valid position
    if (frame.brushParams.z > 0.5f) {  // hasValidPosition
        float3 brushPos = frame.brushPosition.xyz;
        float brushRadius = frame.brushPosition.w;
        uint brushMaterial = (uint)frame.brushParams.x;
        uint brushShape = (uint)frame.brushParams.y;

        // Get material color for preview
        float u = (float(brushMaterial) + 0.5f) / 256.0f;
        float3 materialColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0).rgb;

        // Render semi-transparent brush preview
        float4 brushPreview = RenderBrushPreview(cameraPos, rayDir, brushPos, brushRadius, brushShape, materialColor);

        // Alpha blend preview over voxel color
        if (brushPreview.a > 0.0f) {
            voxelColor.rgb = lerp(voxelColor.rgb, brushPreview.rgb, brushPreview.a);
        }
    }

    return voxelColor;
}
