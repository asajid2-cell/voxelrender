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
    uint payload;
};

struct SparseSurfaceBrickRange {
    int3 coord;
    uint firstFace;
    uint faceCount;
    uint flags;
};

StructuredBuffer<SparseSurfaceFace> SparseSurfaceFaces : register(t16);
StructuredBuffer<SparseSurfaceBrickRange> SparseSurfaceRanges : register(t17);
RWStructuredBuffer<uint> RenderOwnershipStats : register(u0);

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
static const uint MID_CLIPMAP_MAX_SHADER_TILES = 256u;
static const uint MID_CLIPMAP_MAX_SHADER_RINGS = 8u;
static const uint MID_CLIPMAP_LOOKUP_PROBES = 8u;
static const uint MID_VOXEL_CLIPMAP_MAX_BRICKS = 512u;
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

struct PSOutput {
    float4 color : SV_Target;
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

float BackgroundRenderQuality() {
    return clamp(frame.renderBudgetParams.w, 0.25f, 1.0f);
}

int ScaleBackgroundStepBudget(int fullBudget, int mediumBudget, int lowBudget) {
    const float quality = BackgroundRenderQuality();
    if (quality < 0.62f) {
        return lowBudget;
    }
    if (quality < 0.84f) {
        return mediumBudget;
    }
    return fullBudget;
}

int ScaleBackgroundRefineBudget(int fullBudget, int mediumBudget, int lowBudget) {
    return ScaleBackgroundStepBudget(fullBudget, mediumBudget, lowBudget);
}

float FarFieldRenderQuality() {
    return min(BackgroundRenderQuality(), clamp(frame.renderBudgetParams.z, 0.25f, 1.0f));
}

int ScaleFarFieldStepBudget(int fullBudget, int mediumBudget, int lowBudget) {
    const float quality = FarFieldRenderQuality();
    if (quality < 0.62f) {
        return lowBudget;
    }
    if (quality < 0.84f) {
        return mediumBudget;
    }
    return fullBudget;
}

int ScaleFarFieldRefineBudget(int fullBudget, int mediumBudget, int lowBudget) {
    return ScaleFarFieldStepBudget(fullBudget, mediumBudget, lowBudget);
}

float BackgroundMissingSampleSkipScale() {
    const float quality = BackgroundRenderQuality();
    if (quality < 0.62f) {
        return 2.75f;
    }
    if (quality < 0.84f) {
        return 2.20f;
    }
    return 1.55f;
}

bool BackgroundDebugLayerMode() {
    return frame.debugMode == 8u || frame.debugMode == 9u ||
        frame.debugMode == 49u || frame.debugMode == 50u;
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

uint FarHash3D(int x, int y, int z, uint seed) {
    uint h = seed ^ 2166136261u;
    h = (h ^ (uint)x) * 16777619u;
    h = (h ^ (uint)y) * 16777619u;
    h = (h ^ (uint)z) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float FarValueNoise2D(float x, float z, uint seed) {
    int x0 = (int)floor(x);
    int z0 = (int)floor(z);
    float fx = x - (float)x0;
    float fz = z - (float)z0;
    float sx = FarSmooth01(fx);
    float sz = FarSmooth01(fz);

    float s00 = (float)(FarHash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    float s10 = (float)(FarHash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    float s01 = (float)(FarHash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    float s11 = (float)(FarHash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    float a = lerp(s00, s10, sx);
    float b = lerp(s01, s11, sx);
    return lerp(a, b, sz) * 2.0f - 1.0f;
}

float FarTerrainHeight(float2 xz, out float mountainMask, out float spireMask, out float ravineMask) {
    // Must match Simulation::SparseTerrainGenerator::HeightAt. Earlier far
    // fallback used a separate sine heightfield, which composited a different
    // world behind the sparse/mid layers and produced detached cliffs, holes,
    // and corrupted-looking horizon transitions.
    float broad = FarValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, FAR_WORLD_SEED + 11u);
    float ridgeSource = FarValueNoise2D(
        xz.x * 0.0100f + 41.0f,
        xz.y * 0.0100f - 17.0f,
        FAR_WORLD_SEED + 23u);
    float ridge = 1.0f - abs(ridgeSource);
    float detail = FarValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        FAR_WORLD_SEED + 37u);

    float height = -64.0f;
    height += broad * 155.0f;
    height += ridge * ridge * 180.0f;
    height += detail * 18.0f;

    float2 originDelta = xz - float2(96.0f, 96.0f);
    height += (1.0f - FarSmooth01(length(originDelta) / 420.0f)) * 120.0f;

    mountainMask = saturate((ridge * ridge * 180.0f + max(height - 160.0f, 0.0f)) / 300.0f);
    spireMask = 0.0f;
    ravineMask = 0.0f;
    return clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
}

float QuantizeTerrainHeight(float height, float verticalStep) {
    verticalStep = max(verticalStep, 1.0f);
    return floor(height / verticalStep) * verticalStep;
}

float FarFallbackCellSize(float distanceFromCamera) {
    const float t = saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) return 8.0f;
    if (t < 0.42f) return 12.0f;
    if (t < 0.68f) return 18.0f;
    return 28.0f;
}

float2 FarFallbackCellCenter(float2 xz, float cellSize) {
    return (floor(xz / cellSize) + 0.5f) * cellSize;
}

float FarTerrainHeightVoxelized(
    float2 xz,
    float distanceFromCamera,
    out float mountainMask,
    out float spireMask,
    out float ravineMask)
{
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    const float2 sampleXz = FarFallbackCellCenter(xz, cellSize);
    const float rawHeight = FarTerrainHeight(sampleXz, mountainMask, spireMask, ravineMask);
    return QuantizeTerrainHeight(rawHeight, max(4.0f, cellSize * 0.75f));
}

uint FarTerrainMaterial(float2 xz, float height, float mountainMask, float spireMask, float ravineMask) {
    // Match the generated top-surface material rule used by
    // SparseTerrainGenerator::SampleGeneratedVoxel.
    if (height < FAR_SEA_LEVEL + 6.0f) {
        return MAT_SAND;
    }
    if (height > 260.0f) {
        return MAT_STONE;
    }
    return MAT_DIRT;
}

float3 FarTerrainVoxelNormal(float2 xz, float distanceFromCamera) {
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    float mountainMaskA, spireMaskA, ravineMaskA;
    float mountainMaskB, spireMaskB, ravineMaskB;
    float hx0 = FarTerrainHeightVoxelized(
        xz - float2(cellSize, 0.0f),
        distanceFromCamera,
        mountainMaskA,
        spireMaskA,
        ravineMaskA);
    float hx1 = FarTerrainHeightVoxelized(
        xz + float2(cellSize, 0.0f),
        distanceFromCamera,
        mountainMaskB,
        spireMaskB,
        ravineMaskB);
    float hz0 = FarTerrainHeightVoxelized(
        xz - float2(0.0f, cellSize),
        distanceFromCamera,
        mountainMaskA,
        spireMaskA,
        ravineMaskA);
    float hz1 = FarTerrainHeightVoxelized(
        xz + float2(0.0f, cellSize),
        distanceFromCamera,
        mountainMaskB,
        spireMaskB,
        ravineMaskB);
    return normalize(float3(hx0 - hx1, max(cellSize * 2.0f, 6.0f), hz0 - hz1));
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

float ProjectRayDepth(float rayDistance, float3 rayDir) {
    static const float kNearPlane = 0.05f;
    static const float kFarPlane = 10000.0f;
    if (rayDistance >= 1e19f) {
        return 0.999999f;
    }
    const float viewZ = max(dot(rayDir, frame.cameraForward.xyz) * rayDistance, kNearPlane);
    return min(saturate((viewZ - kNearPlane) / (kFarPlane - kNearPlane)), 0.999999f);
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

bool LookupResidentMidClipmapTileInRing(
    float2 xz,
    uint side,
    uint tileCount,
    uint lookupCapacity,
    uint ring,
    out uint tileIndex)
{
    tileIndex = 0u;
    if (lookupCapacity == 0u || (lookupCapacity & (lookupCapacity - 1u)) != 0u) {
        return false;
    }

    float cellSize = MidClipmapRingCellSize(ring);
    float tileWorldSize = cellSize * (float)(side - 1u);
    int tileX = (int)floor(xz.x / tileWorldSize);
    int tileZ = (int)floor(xz.y / tileWorldSize);
    uint slot = HashMidClipmapTileCoord((int)ring, tileX, tileZ) & (lookupCapacity - 1u);

    [unroll]
    for (uint probe = 0u; probe < MID_CLIPMAP_LOOKUP_PROBES; ++probe) {
        uint4 entry = MidClipmapLookup[slot];
        if (entry.w == 0u) {
            return false;
        }
        if ((int)entry.x == (int)ring && (int)entry.y == tileX && (int)entry.z == tileZ) {
            uint compactIndex = entry.w - 1u;
            if (compactIndex < tileCount) {
                tileIndex = compactIndex;
                return true;
            }
            return false;
        }
        slot = (slot + 1u) & (lookupCapacity - 1u);
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

bool SampleResidentMidClipmapRing(
    float2 xz,
    uint ring,
    out float height,
    out uint material,
    out float cellSize)
{
    height = 0.0f;
    material = MAT_AIR;
    cellSize = MidClipmapRingCellSize(ring);

    uint4 header = MidClipmapTiles[0];
    if (header.x != MID_CLIPMAP_MAGIC || header.y < 2u || header.z == 0u) {
        return false;
    }

    uint side = min(header.y, 65u);
    uint tileCount = min(header.z, MID_CLIPMAP_MAX_SHADER_TILES);
    uint lookupCapacity = header.w & 0x00FFFFFFu;
    uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (ring >= ringCount) {
        return false;
    }

    uint tileIndex = 0u;
    if (!LookupResidentMidClipmapTileInRing(xz, side, tileCount, lookupCapacity, ring, tileIndex)) {
        return false;
    }

    uint4 tile = MidClipmapTiles[tileIndex + 1u];
    if (tile.w == 0u) {
        return false;
    }

    int2 origin = int2((int)tile.x, (int)tile.y);
    cellSize = (float)(tile.z >> 8u);
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
    uint sampleBase = tileIndex * side * side;
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

bool SampleResidentMidClipmapFallback(
    float2 xz,
    uint preferredRing,
    out float height,
    out uint material,
    out float cellSize)
{
    height = 0.0f;
    material = MAT_AIR;
    cellSize = MidClipmapRingCellSize(preferredRing);

    uint4 header = MidClipmapTiles[0];
    const uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (header.x != MID_CLIPMAP_MAGIC || header.z == 0u || ringCount == 0u) {
        return false;
    }

    const uint clampedPreferred = min(preferredRing, ringCount - 1u);
    if (SampleResidentMidClipmapRing(xz, clampedPreferred, height, material, cellSize)) {
        return true;
    }

    [loop]
    for (uint offset = 1u; offset < MID_CLIPMAP_MAX_SHADER_RINGS; ++offset) {
        const uint coarserRing = clampedPreferred + offset;
        if (coarserRing >= ringCount) {
            break;
        }
        if (SampleResidentMidClipmapRing(xz, coarserRing, height, material, cellSize)) {
            return true;
        }
    }

    [loop]
    for (uint offsetFine = 1u; offsetFine < MID_CLIPMAP_MAX_SHADER_RINGS; ++offsetFine) {
        if (offsetFine > clampedPreferred) {
            break;
        }
        const uint finerRing = clampedPreferred - offsetFine;
        if (SampleResidentMidClipmapRing(xz, finerRing, height, material, cellSize)) {
            return true;
        }
    }

    return false;
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

bool SampleResidentMidVoxelFallback(
    float3 worldPos,
    uint preferredRing,
    out uint voxel,
    out uint actualRing,
    out float actualCellSize)
{
    voxel = PackVoxel(MAT_AIR, 0, 0, 0);
    actualRing = preferredRing;
    actualCellSize = MidClipmapRingCellSize(preferredRing);

    uint4 header = MidVoxelClipmapMetadata[0];
    const uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u || ringCount == 0u) {
        return false;
    }

    const uint clampedPreferred = min(preferredRing, ringCount - 1u);
    if (SampleResidentMidVoxel(worldPos, clampedPreferred, voxel)) {
        actualRing = clampedPreferred;
        actualCellSize = MidClipmapRingCellSize(actualRing);
        return true;
    }

    // Missing preferred-ring data is a residency gap, not proof that the world
    // is empty. Try progressively coarser rings first so the hierarchy degrades
    // to stable context instead of flashing through to far/sky ownership.
    [loop]
    for (uint offset = 1u; offset < MID_CLIPMAP_MAX_SHADER_RINGS; ++offset) {
        const uint coarserRing = clampedPreferred + offset;
        if (coarserRing >= ringCount) {
            break;
        }
        if (SampleResidentMidVoxel(worldPos, coarserRing, voxel)) {
            actualRing = coarserRing;
            actualCellSize = MidClipmapRingCellSize(actualRing);
            return true;
        }
    }

    // Finer rings are less likely in far segments, but using one if it is
    // resident is still better than treating the segment as missing terrain.
    [loop]
    for (uint offsetFine = 1u; offsetFine < MID_CLIPMAP_MAX_SHADER_RINGS; ++offsetFine) {
        if (offsetFine > clampedPreferred) {
            break;
        }
        const uint finerRing = clampedPreferred - offsetFine;
        if (SampleResidentMidVoxel(worldPos, finerRing, voxel)) {
            actualRing = finerRing;
            actualCellSize = MidClipmapRingCellSize(actualRing);
            return true;
        }
    }

    return false;
}

bool IsMidVoxelAirOrMissing(float3 worldPos, uint ring) {
    uint neighborVoxel;
    if (!SampleResidentMidVoxel(worldPos, ring, neighborVoxel)) {
        // A missing neighbor is an unknown residency boundary, not a real
        // surface. Treat it as occupied so mid bricks do not draw artificial
        // chunk shells while streaming catches up.
        return false;
    }
    return GetMaterial(neighborVoxel) == MAT_AIR;
}

bool IsResidentMidVoxelExposed(float3 worldPos, uint ring, float cellSize, out float3 normal) {
    normal = float3(0.0f, 0.0f, 0.0f);
    const float3 dx = float3(cellSize, 0.0f, 0.0f);
    const float3 dy = float3(0.0f, cellSize, 0.0f);
    const float3 dz = float3(0.0f, 0.0f, cellSize);

    if (IsMidVoxelAirOrMissing(worldPos + dx, ring)) normal += float3(1.0f, 0.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dx, ring)) normal += float3(-1.0f, 0.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos + dy, ring)) normal += float3(0.0f, 1.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dy, ring)) normal += float3(0.0f, -1.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos + dz, ring)) normal += float3(0.0f, 0.0f, 1.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dz, ring)) normal += float3(0.0f, 0.0f, -1.0f);

    const float normalLength = length(normal);
    if (normalLength <= 0.001f) {
        normal = float3(0.0f, 1.0f, 0.0f);
        return false;
    }
    normal /= normalLength;
    return true;
}

bool IsResidentMidVoxelTaggedSurface(uint voxel) {
    return (GetState(voxel) & STATE_VISUAL_SURFACE) != 0u;
}

float NextMidVoxelCellBoundaryT(float3 rayOrigin, float3 rayDir, float currentT, float cellSize) {
    float3 pos = rayOrigin + rayDir * currentT;
    float3 cell = floor(pos / cellSize);
    float nextT = 1e20f;

    if (abs(rayDir.x) > 0.0001f) {
        float boundaryX = ((rayDir.x > 0.0f) ? (cell.x + 1.0f) : cell.x) * cellSize;
        float tx = (boundaryX - rayOrigin.x) / rayDir.x;
        if (tx > currentT + 0.01f) {
            nextT = min(nextT, tx);
        }
    }
    if (abs(rayDir.y) > 0.0001f) {
        float boundaryY = ((rayDir.y > 0.0f) ? (cell.y + 1.0f) : cell.y) * cellSize;
        float ty = (boundaryY - rayOrigin.y) / rayDir.y;
        if (ty > currentT + 0.01f) {
            nextT = min(nextT, ty);
        }
    }
    if (abs(rayDir.z) > 0.0001f) {
        float boundaryZ = ((rayDir.z > 0.0f) ? (cell.z + 1.0f) : cell.z) * cellSize;
        float tz = (boundaryZ - rayOrigin.z) / rayDir.z;
        if (tz > currentT + 0.01f) {
            nextT = min(nextT, tz);
        }
    }

    if (nextT >= 1e19f) {
        return currentT + max(cellSize, 4.0f);
    }
    return max(nextT + 0.02f, currentT + 0.05f);
}

bool RaymarchMidVoxelClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit voxelHit) {
    voxelHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    if ((sparseNearFlags & 4u) == 0u) {
        return false;
    }
    if (frame.midResidencyParams.y < 0.04f || frame.midResidencyParams.w < 1.0f) {
        return false;
    }
    // Mid-voxel clipmaps are the most expensive background detail layer because
    // a miss can still require many resident-brick probes. Keep them as a high
    // quality detail path and let the cheaper height clipmap own continuity
    // while the runtime scheduler is already downshifting background work.
    if (((BackgroundRenderQuality() < 0.58f) ||
         (frame.renderBudgetParams.z < 0.50f && BackgroundRenderQuality() < 0.90f)) &&
        !BackgroundDebugLayerMode()) {
        return false;
    }

    uint4 header = MidVoxelClipmapMetadata[0];
    if (frame.midFieldParams.x < 0.5f || header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u) {
        return false;
    }
    // Mid voxel bricks are coarse context. They should extend the horizon, not
    // become foreground cliffs/ceilings when the player looks up/down around the
    // exact sparse-surface near field.
    if (rayDir.y > 0.20f || rayDir.y < -0.68f) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);
    int budget = frame.renderBudgetParams.z < 0.55f
        ? ScaleBackgroundStepBudget(56, 44, 32)
        : (frame.renderBudgetParams.z < 0.85f
            ? ScaleBackgroundStepBudget(88, 68, 48)
            : ScaleBackgroundStepBudget(128, 96, 64));

    [loop]
    for (int i = 0; i < budget && t < endDistance; ++i) {
        uint ring = min((uint)floor(saturate((t - startDistance) / max(endDistance - startDistance, 1.0f)) *
            max((float)(header.w >> 24u), 1.0f)), (uint)max((int)(header.w >> 24u) - 1, 0));
        float cellSize = MidClipmapRingCellSize(ring);
        float3 pos = rayOrigin + rayDir * t;
        uint voxel;
        uint actualRing;
        float actualCellSize;
        if (SampleResidentMidVoxelFallback(pos, ring, voxel, actualRing, actualCellSize)) {
            float nextCellT = NextMidVoxelCellBoundaryT(rayOrigin, rayDir, t, actualCellSize);
            uint material = GetMaterial(voxel);
            if (material != MAT_AIR) {
                if (!IsResidentMidVoxelTaggedSurface(voxel)) {
                    t = min(nextCellT, t + max(actualCellSize, 4.0f));
                    continue;
                }
                float3 normal;
                if (!IsResidentMidVoxelExposed(pos, actualRing, actualCellSize, normal)) {
                    normal = FarTerrainNormal(pos.xz);
                }
                float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
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
            t = min(nextCellT, t + max(actualCellSize, 4.0f));
        } else {
            t += max(cellSize * BackgroundMissingSampleSkipScale(), 12.0f);
        }
    }

    return false;
}

bool MidClipmapResidentHeightMaterial(float2 xz, out float height, out uint material) {
    if (SampleResidentMidClipmap(xz, height, material)) {
        return true;
    }

    height = 0.0f;
    material = MAT_AIR;
    return false;
}

bool MidClipmapResidentHeightMaterialForDistance(
    float2 xz,
    float distanceFromCamera,
    out float height,
    out uint material,
    out float cellSize)
{
    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    uint4 header = MidClipmapTiles[0];
    const uint ringCount = max(header.w >> 24u, 1u);
    const uint preferredRing = min((uint)floor(saturate((distanceFromCamera - startDistance) /
        max(endDistance - startDistance, 1.0f)) * (float)ringCount), ringCount - 1u);

    if (SampleResidentMidClipmapFallback(xz, preferredRing, height, material, cellSize)) {
        return true;
    }

    height = 0.0f;
    material = MAT_AIR;
    cellSize = MidClipmapRingCellSize(preferredRing);
    return false;
}

float3 MidClipmapNormal(float2 xz, float distanceFromCamera) {
    float hL, hR, hD, hU;
    uint mat;
    float cellSize;
    if (!MidClipmapResidentHeightMaterialForDistance(xz, distanceFromCamera, hL, mat, cellSize)) {
        return float3(0.0f, 1.0f, 0.0f);
    }
    const float offset = max(cellSize, 3.0f);
    float unusedCell;
    if (MidClipmapResidentHeightMaterialForDistance(xz - float2(offset, 0.0f), distanceFromCamera, hL, mat, unusedCell) &&
        MidClipmapResidentHeightMaterialForDistance(xz + float2(offset, 0.0f), distanceFromCamera, hR, mat, unusedCell) &&
        MidClipmapResidentHeightMaterialForDistance(xz - float2(0.0f, offset), distanceFromCamera, hD, mat, unusedCell) &&
        MidClipmapResidentHeightMaterialForDistance(xz + float2(0.0f, offset), distanceFromCamera, hU, mat, unusedCell)) {
        return normalize(float3(hL - hR, offset * 2.0f, hD - hU));
    }
    return float3(0.0f, 1.0f, 0.0f);
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
    if (frame.midResidencyParams.x < 0.04f || frame.midResidencyParams.z < 1.0f) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);

    // Transition contract: this layer is terrain context after the near sparse
    // window. Do not draw it for steep rays where it would appear as detached
    // ceilings or terrain punching through editable near holes.
    if (rayDir.y > 0.12f || rayDir.y < -0.20f) {
        return false;
    }

    float3 previousPos = rayOrigin + rayDir * t;
    uint previousMaterial;
    float previousHeight;
    float previousCellSize;
    bool previousResident = MidClipmapResidentHeightMaterialForDistance(
        previousPos.xz,
        t,
        previousHeight,
        previousMaterial,
        previousCellSize);
    previousHeight = QuantizeTerrainHeight(previousHeight, previousCellSize);
    float previousSigned = previousPos.y - previousHeight;

    int stepBudget = frame.renderBudgetParams.z < 0.55f
        ? ScaleBackgroundStepBudget(28, 22, 16)
        : (frame.renderBudgetParams.z < 0.85f
            ? ScaleBackgroundStepBudget(44, 34, 24)
            : ScaleBackgroundStepBudget(64, 48, 32));
    [loop]
    for (int i = 0; i < stepBudget && t < endDistance; ++i) {
        float cellSize = MidClipmapCellSize(t);
        float stepSize = max(cellSize * 0.90f, 8.0f);
        if (previousResident && previousSigned > 0.0f && rayDir.y < -0.035f) {
            const float verticalStep = previousSigned / max(-rayDir.y, 0.055f);
            const float qualityStepCap = lerp(cellSize * 4.0f, cellSize * 7.0f, BackgroundRenderQuality());
            stepSize = clamp(verticalStep * 0.62f, stepSize, max(stepSize, qualityStepCap));
        }
        float nextT = min(t + stepSize, endDistance);
        float3 pos = rayOrigin + rayDir * nextT;
        uint material;
        float height;
        float sampleCellSize;
        bool residentSample = MidClipmapResidentHeightMaterialForDistance(
            pos.xz,
            nextT,
            height,
            material,
            sampleCellSize);
        height = QuantizeTerrainHeight(height, sampleCellSize);
        float signedDistance = pos.y - height;

        if (!residentSample) {
            t = nextT;
            previousResident = false;
            continue;
        }

        if (!previousResident) {
            t = nextT;
            previousSigned = signedDistance;
            previousResident = true;
            continue;
        }

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = t;
            float hi = nextT;
            const int refineBudget = ScaleBackgroundRefineBudget(5, 4, 3);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                uint midMaterial;
                float midHeight;
                float midCellSize;
                bool midResident = MidClipmapResidentHeightMaterialForDistance(
                    midPos.xz,
                    mid,
                    midHeight,
                    midMaterial,
                    midCellSize);
                midHeight = QuantizeTerrainHeight(midHeight, midCellSize);
                if (!midResident || midPos.y > midHeight) {
                    lo = mid;
                } else {
                    hi = mid;
                    height = midHeight;
                    material = midMaterial;
                }
            }

            float hitT = hi;
            float3 hitPos = rayOrigin + rayDir * hitT;
            float3 normal = MidClipmapNormal(hitPos.xz, hitT);
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
        previousResident = true;
    }

    return false;
}

uint FarVoxelChildNodeIndex(uint childBase, uint childMask, uint childOrdinal) {
    uint precedingMask = childMask & ((1u << childOrdinal) - 1u);
    return childBase + countbits(precedingMask);
}

bool FarSvoLeafSurfaceHit(
    float3 rayOrigin,
    float3 rayDir,
    float leafT0,
    float leafT1,
    float nodeSize,
    out float hitT,
    out float3 hitNormal,
    out uint hitMaterial);

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
            // Far SVO pages contain coarse solid-interior leaves so traversal can
            // skip deep terrain volume. Those leaves are not visual surface
            // geometry; drawing them caused huge rectangular slabs in high-angle
            // captures. Surface/detail leaves remain renderable.
            if ((node.flags & 2u) != 0u || node.material == MAT_AIR) {
                continue;
            }
            if (nodeSize <= 40.0f) {
                float candidateT = max(tNear, startDist);
                if (candidateT < nearestT) {
                    nearestT = candidateT;
                    nearestNormal = boxNormal;
                    nearestMaterial = node.material;
                    hit = true;
                }
            } else {
                float candidateT;
                float3 candidateNormal;
                uint candidateMaterial;
                if (FarSvoLeafSurfaceHit(
                    rayOrigin,
                    rayDir,
                    max(tNear, startDist),
                    min(tFar, nearestT),
                    nodeSize,
                    candidateT,
                    candidateNormal,
                    candidateMaterial) &&
                    candidateT < nearestT) {
                    nearestT = candidateT;
                    nearestNormal = candidateNormal;
                    nearestMaterial = candidateMaterial;
                    hit = true;
                }
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

bool FarSvoLeafSurfaceHit(
    float3 rayOrigin,
    float3 rayDir,
    float leafT0,
    float leafT1,
    float nodeSize,
    out float hitT,
    out float3 hitNormal,
    out uint hitMaterial)
{
    hitT = 1e20f;
    hitNormal = float3(0.0f, 1.0f, 0.0f);
    hitMaterial = MAT_STONE;

    if (leafT1 <= leafT0) {
        return false;
    }

    float t = leafT0;
    float3 pos = rayOrigin + rayDir * t;
    float mountainMask, spireMask, ravineMask;
    float height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
    float previousSigned = pos.y - height;
    float previousT = t;

    // A raw SVO leaf AABB is only a conservative container. Drawing the AABB
    // entry face creates the giant rectangular sheets seen in captures. Accept a
    // leaf only where the ray actually crosses the far terrain surface.
    const float stepSize = clamp(nodeSize * 0.55f, 16.0f, 96.0f);
    const int sampleBudget = ScaleFarFieldStepBudget(8, 6, 5);
    [loop]
    for (int sample = 0; sample < sampleBudget && t < leafT1; ++sample) {
        t = min(t + stepSize, leafT1);
        pos = rayOrigin + rayDir * t;
        height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        const float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = previousT;
            float hi = t;
            const int refineBudget = ScaleFarFieldRefineBudget(5, 4, 3);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                const float mid = (lo + hi) * 0.5f;
                const float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                const float midHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
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

            hitT = hi;
            const float3 hitPos = rayOrigin + rayDir * hitT;
            hitNormal = FarTerrainNormal(hitPos.xz);
            hitMaterial = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            return true;
        }

        previousSigned = signedDistance;
        previousT = t;
    }

    return false;
}

bool RaymarchSparseFarField(float3 rayOrigin, float3 rayDir, float startDist, out RayHit farHit) {
    farHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    // The page-indexed far SVO is a high-detail distant voxel layer, not a
    // guaranteed background fill. When the runtime budget is under pressure,
    // the cheaper procedural far-height layer owns ordinary horizon continuity.
    // Steep downward rays are the exception: the height fallback deliberately
    // refuses them to avoid detached sheets, so SVO keeps a tiny coverage path
    // there to prevent terrain-to-sky pops during high-altitude stress views.
    if (frame.farOwnershipParams.x < 0.5f ||
        frame.farOwnershipParams.y < 0.999f ||
        frame.farOwnershipParams.z <= 0.0f) {
        return false;
    }

    const float farQuality = min(frame.renderBudgetParams.z, frame.farOwnershipParams.w);
    const bool steepDownCoverageRay = rayDir.y < -0.88f && rayDir.y > -0.95f;
    const bool farSvoQualityAllowed = farQuality >= 0.35f ||
        (steepDownCoverageRay && farQuality >= 0.25f);
    if (frame.farFieldParams.x < 0.5f || !farSvoQualityAllowed ||
        frame.farFieldParams.y < 1.0f || frame.farFieldParams.z < 1.0f) {
        return false;
    }
    // The far SVO is the authoritative distant voxel layer. It must be allowed
    // to answer high/downward validation rays; otherwise those rays fall
    // through to the procedural height fallback and produce huge sheet-like
    // terrain bands.
    if (rayDir.y > 0.18f || rayDir.y < -0.95f) {
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
    int maxPageSteps = farQuality < 0.55f
        ? ScaleFarFieldStepBudget(6, 5, 4)
        : (farQuality < 0.72f
            ? ScaleFarFieldStepBudget(8, 6, 5)
            : (farQuality < 0.85f
                ? ScaleFarFieldStepBudget(10, 8, 6)
                : (farQuality < 0.95f
                    ? ScaleFarFieldStepBudget(16, 12, 8)
                    : ScaleFarFieldStepBudget(24, 18, 12))));

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

    // Keep this as a horizon/continuity fallback behind the caller-selected
    // transition distance. Earlier versions rejected steep downward rays here,
    // which made high-altitude stress views pop terrain pixels into miss/sky
    // whenever Far SVO backed off under budget pressure.
    if (rayDir.y > 0.24f) {
        return false;
    }

    const float farMaxDist = 10400.0f;
    const float farTerrainCeiling = FAR_TERRAIN_MAX_HEIGHT + 64.0f;
    float t = max(startDist, 160.0f);
    if (rayOrigin.y > farTerrainCeiling) {
        if (rayDir.y >= -0.001f) {
            return false;
        }
        const float ceilingT = (farTerrainCeiling - rayOrigin.y) / rayDir.y;
        if (ceilingT > farMaxDist) {
            return false;
        }
        t = max(t, max(ceilingT, 0.0f));
    }
    float previousT = t;
    float3 previousPos = rayOrigin + rayDir * t;
    float mountainMask, spireMask, ravineMask;
    float previousHeight = FarTerrainHeightVoxelized(previousPos.xz, previousT, mountainMask, spireMask, ravineMask);
    float previousSigned = previousPos.y - previousHeight;
    if (previousSigned <= 0.0f) {
        float originMountainMask, originSpireMask, originRavineMask;
        float originHeight = FarTerrainHeightVoxelized(
            rayOrigin.xz,
            0.0f,
            originMountainMask,
            originSpireMask,
            originRavineMask);
        if (rayOrigin.y > originHeight) {
            float lo = 0.0f;
            float hi = t;
            const int refineBudget = ScaleFarFieldRefineBudget(7, 5, 4);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                float midHeight = FarTerrainHeightVoxelized(midPos.xz, mid, mm, sm, rm);
                if (midPos.y > midHeight) {
                    lo = mid;
                } else {
                    hi = mid;
                    mountainMask = mm;
                    spireMask = sm;
                    ravineMask = rm;
                    previousHeight = midHeight;
                }
            }

            float hitT = max(hi, 64.0f);
            float3 hitPos = rayOrigin + rayDir * hitT;
            float3 normal = FarTerrainVoxelNormal(hitPos.xz, hitT);
            uint material = FarTerrainMaterial(hitPos.xz, previousHeight, mountainMask, spireMask, ravineMask);
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);
            float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
            float lighting = max(dot(normal, lightDir), 0.18f);
            float3 color = baseColor.rgb * lighting;
            float fogFactor = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.90f + 0.10f);
            farHit = MakeHit(float4(color, 1.0f), hitT);
            return true;
        }
    }

    // This is a continuity fallback behind the page-indexed SVO, not the main
    // far renderer. Keep it cheap enough that sky/horizon pixels cannot become
    // the frame-time bottleneck.
    int farStepBudget = frame.renderBudgetParams.z < 0.6f
        ? ScaleFarFieldStepBudget(24, 18, 12)
        : (frame.renderBudgetParams.z < 0.9f
            ? ScaleFarFieldStepBudget(36, 28, 20)
            : ScaleFarFieldStepBudget(48, 36, 26));
    [loop]
    for (int i = 0; i < farStepBudget && t < farMaxDist; ++i) {
        float distanceStep = lerp(96.0f, 360.0f, saturate(t / farMaxDist));
        float svoStep = frame.renderBudgetParams.z > 0.92f
            ? FarSvoSuggestedStep(rayOrigin, rayDir, t)
            : distanceStep;
        float stepSize = max(FAR_SVO_MIN_CELL_SIZE, max(svoStep, distanceStep));
        if (previousSigned > 0.0f && rayDir.y < -0.030f) {
            const float verticalStep = previousSigned / max(-rayDir.y, 0.045f);
            const float qualityStepCap = lerp(distanceStep * 1.50f, distanceStep * 3.25f, BackgroundRenderQuality());
            stepSize = clamp(verticalStep * 0.68f, stepSize, max(stepSize, qualityStepCap));
        }
        t += stepSize;

        float3 pos = rayOrigin + rayDir * t;
        float height = FarTerrainHeightVoxelized(pos.xz, t, mountainMask, spireMask, ravineMask);
        float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = previousT;
            float hi = t;
            const int refineBudget = ScaleFarFieldRefineBudget(5, 4, 3);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                float midHeight = FarTerrainHeightVoxelized(midPos.xz, mid, mm, sm, rm);
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
            float3 normal = FarTerrainVoxelNormal(hitPos.xz, hitT);
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

    // Last-resort downward continuity. If the coarse far heightfield misses,
    // do not expose raw sky below the world during high-altitude motion; a
    // fogged bedrock floor is visually safer and matches the vertical-world
    // contract that there is a bottom layer beneath caverns and ravines.
    if (rayDir.y < -0.035f) {
        const float bedrockY = FAR_TERRAIN_MIN_HEIGHT - 8.0f;
        float bedrockT = (bedrockY - rayOrigin.y) / rayDir.y;
        if (bedrockT >= max(startDist, 160.0f) && bedrockT < farMaxDist) {
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (MAT_STONE + 0.5f) / 256.0f, 0);
            float3 normal = float3(0.0f, 1.0f, 0.0f);
            float lighting = max(dot(normal, SkySunDirection()), 0.16f);
            float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.28f + lighting * 0.55f);
            float fogFactor = saturate((bedrockT - 900.0f) / (farMaxDist - 900.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.94f + 0.06f);
            farHit = MakeHit(float4(color, 1.0f), bedrockT);
            return true;
        }
    }

    return false;
}

static const uint BACKGROUND_LAYER_NONE = 0u;
static const uint BACKGROUND_LAYER_MID_VOXEL = 1u;
static const uint BACKGROUND_LAYER_MID_HEIGHT = 2u;
static const uint BACKGROUND_LAYER_FAR_SVO = 3u;
static const uint BACKGROUND_LAYER_FAR_HEIGHT = 4u;

static const uint RENDER_OWNER_TOTAL = 0u;
static const uint RENDER_OWNER_NEAR = 1u;
static const uint RENDER_OWNER_MID_VOXEL = 2u;
static const uint RENDER_OWNER_MID_HEIGHT = 3u;
static const uint RENDER_OWNER_FAR_SVO = 4u;
static const uint RENDER_OWNER_FAR_HEIGHT = 5u;
static const uint RENDER_OWNER_SKY = 6u;
static const uint RENDER_OWNER_MISS = 7u;
static const uint RENDER_OWNER_FRAME = 8u;
static const uint RENDER_OWNER_SURFACE = 9u;
static const uint RENDER_OWNER_UNSAFE_NEAR_MISS = 10u;

bool RenderOwnershipEnabled() {
    return frame.farFieldGridParams.w > 0.5f;
}

void RecordRenderOwnership(uint owner) {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_TOTAL], 1u);
    if (owner <= RENDER_OWNER_MISS || owner == RENDER_OWNER_SURFACE || owner == RENDER_OWNER_UNSAFE_NEAR_MISS) {
        InterlockedAdd(RenderOwnershipStats[owner], 1u);
    }
    RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
}

float NearBackgroundStartDistance() {
    // The sparse surface pass owns the exact editable near field. The
    // fullscreen pass should start close enough to prevent sky gaps behind
    // resident surfaces, but not so close that procedural far terrain fills
    // holes in the player's immediate editable/collision space.
    if (frame.midFieldParams.x > 0.5f) {
        return clamp(frame.midFieldParams.y * 0.30f, 96.0f, 192.0f);
    }
    return 160.0f;
}

float SurfaceAuthoritativeBackgroundStartDistance() {
    // The raster sparse-surface pass owns the editable foreground. In this mode
    // the fullscreen pass is only sky/horizon context, so it must not synthesize
    // near terrain through temporary surface-cache holes.
    if (frame.midFieldParams.x > 0.5f) {
        return max(frame.midFieldParams.y, 896.0f);
    }
    return 896.0f;
}

bool IntersectSphere(float3 rayOrigin, float3 rayDir, float3 center, float radius, out float tNear, out float tFar) {
    tNear = 0.0f;
    tFar = 0.0f;
    if (radius <= 0.0f) {
        return false;
    }

    float3 oc = rayOrigin - center;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return false;
    }

    float root = sqrt(discriminant);
    tNear = -b - root;
    tFar = -b + root;
    return tFar >= max(tNear, 0.0f);
}

float SurfaceAuthoritativeBackgroundStartForRay(
    float3 rayOrigin,
    float3 rayDir,
    float3 gridMin,
    float3 gridMax)
{
    float startDistance = SurfaceAuthoritativeBackgroundStartDistance();
    float nearEntry;
    float nearExit;
    if (IntersectBox(rayOrigin, rayDir, gridMin, gridMax, nearEntry, nearExit)) {
        // The sparse raster surface pass owns this world-space near volume.
        // Background layers are context behind it, not replacement terrain
        // inside it. Starting after ray-box exit prevents far/mid terrain from
        // drawing through resident-surface holes or late uploads.
        startDistance = max(startDistance, max(nearExit, 0.0f) + 8.0f);
    }
    float sphereEntry;
    float sphereExit;
    if (IntersectSphere(
            rayOrigin,
            rayDir,
            frame.nearOwnershipParams.xyz,
            frame.nearOwnershipParams.w,
            sphereEntry,
            sphereExit)) {
        // The raster sparse surface layer is culled by a stable world-space
        // near radius, not by the legacy dense render AABB. This explicit
        // ownership sphere prevents mid/far fallback from drawing through
        // camera-centered near holes while sparse pages are still streaming.
        startDistance = max(startDistance, max(sphereExit, 0.0f) + 8.0f);
    }
    return startDistance;
}

float SparseMissingPageBackgroundStartForRay(
    float3 rayOrigin,
    float3 rayDir,
    float3 gridMin,
    float3 gridMax)
{
    float startDistance = SurfaceAuthoritativeBackgroundStartDistance();
    float nearEntry;
    float nearExit;
    if (IntersectBox(rayOrigin, rayDir, gridMin, gridMax, nearEntry, nearExit)) {
        // Missing sparse pages are a residency problem, not ownership proof.
        // Wait until the ray exits the dense editable cache, but do not require
        // the larger spherical sparse-surface cull boundary here: that sphere
        // is intentionally conservative for raster visibility and can suppress
        // legitimate horizon continuity for too long during fast camera motion.
        startDistance = max(startDistance, max(nearExit, 0.0f) + 8.0f);
    }
    return startDistance;
}

float FarLayerStartAfterBackground(float backgroundStartDistance) {
    if (frame.midFieldParams.x < 0.5f) {
        return max(backgroundStartDistance, 0.0f);
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float handoffDistance = startDistance + (endDistance - startDistance) * 0.62f;
    if (frame.backgroundOwnershipParams.w > 0.5f) {
        handoffDistance = frame.backgroundOwnershipParams.y;
    }
    return max(backgroundStartDistance, min(endDistance, handoffDistance));
}

bool RaymarchBackgroundField(
    float3 rayOrigin,
    float3 rayDir,
    float startDist,
    bool includeSparseFarField,
    bool allowWideHeightAngles,
    out RayHit backgroundHit,
    out uint backgroundLayer)
{
    backgroundHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    backgroundLayer = BACKGROUND_LAYER_NONE;

    if (RaymarchMidVoxelClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        return true;
    }
    if (RaymarchMidClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
        backgroundLayer = BACKGROUND_LAYER_MID_HEIGHT;
        return true;
    }
    const float farStartDist = FarLayerStartAfterBackground(startDist);
    if (includeSparseFarField && RaymarchSparseFarField(rayOrigin, rayDir, farStartDist, backgroundHit)) {
        backgroundLayer = BACKGROUND_LAYER_FAR_SVO;
        return true;
    }

    const bool heightAngleOk = allowWideHeightAngles
        ? (rayDir.y < 0.24f)
        : (rayDir.y > -0.28f && rayDir.y < 0.10f);
    // Respect the ownership start chosen by the caller. Surface-authoritative
    // mode deliberately pushes background layers behind the near sparse volume;
    // pulling the far heightfield back toward the camera makes coarse terrain
    // draw through near-surface holes as warped foreground sheets.
    const float heightStart = farStartDist;
    if (heightAngleOk && RaymarchFarTerrain(rayOrigin, rayDir, heightStart, backgroundHit)) {
        backgroundLayer = BACKGROUND_LAYER_FAR_HEIGHT;
        return true;
    }

    return false;
}

RayHit DebugBackgroundLayerHit(RayHit hit, uint layer) {
    if (layer == BACKGROUND_LAYER_MID_VOXEL) {
        RecordRenderOwnership(RENDER_OWNER_MID_VOXEL);
    } else if (layer == BACKGROUND_LAYER_MID_HEIGHT) {
        RecordRenderOwnership(RENDER_OWNER_MID_HEIGHT);
    } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
        RecordRenderOwnership(RENDER_OWNER_FAR_SVO);
    } else if (layer == BACKGROUND_LAYER_FAR_HEIGHT) {
        RecordRenderOwnership(RENDER_OWNER_FAR_HEIGHT);
    } else {
        RecordRenderOwnership(RENDER_OWNER_MISS);
    }
    if (frame.debugMode != 49u && frame.debugMode != 50u) {
        return hit;
    }

    float3 tint = float3(0.08f, 0.10f, 0.14f);
    if (layer == BACKGROUND_LAYER_MID_VOXEL) {
        tint = float3(1.0f, 0.52f, 0.10f);
    } else if (layer == BACKGROUND_LAYER_MID_HEIGHT) {
        tint = float3(0.15f, 0.75f, 1.0f);
    } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
        tint = float3(0.85f, 0.24f, 1.0f);
    } else if (layer == BACKGROUND_LAYER_FAR_HEIGHT) {
        tint = float3(0.45f, 1.0f, 0.30f);
    }
    if (frame.debugMode == 50u) {
        hit.color.rgb = tint;
    } else {
        hit.color.rgb = lerp(hit.color.rgb, tint, 0.58f);
    }
    return hit;
}

RayHit DebugBackgroundMissHit(float3 rayDir) {
    const bool expectedSky = rayDir.y > -0.12f;
    RecordRenderOwnership(expectedSky ? RENDER_OWNER_SKY : RENDER_OWNER_MISS);
    if (frame.debugMode == 50u) {
        if (expectedSky) {
            return MakeHit(float4(0.18f, 0.42f, 0.95f, 1.0f), 1e20f);
        }
        // Pure red means the background ownership chain found no resident
        // mid/far layer for this pixel. This is intentionally harsh: it makes
        // clipmap residency gaps and fallback suppression visible in screenshots.
        // Upward sky rays are shown blue instead, so real sky is not confused
        // with missing terrain ownership.
        return MakeHit(float4(1.0f, 0.05f, 0.02f, 1.0f), 1e20f);
    }
    return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
}

RayHit DebugUnsafeNearMissHit(float3 rayDir) {
    RecordRenderOwnership(RENDER_OWNER_UNSAFE_NEAR_MISS);
    if (frame.debugMode == 50u) {
        // Magenta marks a near-field ownership hole: the ray crossed missing
        // sparse pages inside the editable/collision volume and no allowed
        // background layer could safely take ownership behind the transition.
        return MakeHit(float4(1.0f, 0.02f, 0.75f, 1.0f), 1e20f);
    }
    return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
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
    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    const bool sparseSurfaceAuthoritative = sparseNearActive && ((sparseNearFlags & 2u) != 0u);
    const bool sparseSurfaceRaymarchFill = sparseNearActive && ((sparseNearFlags & 8u) != 0u);

    if (sparseSurfaceAuthoritative && !sparseSurfaceRaymarchFill) {
        RayHit backgroundHit;
        uint backgroundLayer;
        const float backgroundStart = SurfaceAuthoritativeBackgroundStartForRay(
            rayOrigin,
            rayDir,
            gridMin,
            gridMax);
        if (RaymarchBackgroundField(
            rayOrigin,
            rayDir,
            backgroundStart,
            true,
            true,
            backgroundHit,
            backgroundLayer)) {
            return DebugBackgroundLayerHit(backgroundHit, backgroundLayer);
        }
        return DebugBackgroundMissHit(rayDir);
    }

    // Find ray entry point into grid
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, gridMin, gridMax, tMin, tMax)) {
        if (frame.debugMode == 43u) {
            return MakeHit(float4(0.18f, 0.08f, 0.20f, 1.0f), 1e20f);
        }
        RayHit farHit;
        uint farLayer;
        if (RaymarchBackgroundField(rayOrigin, rayDir, 32.0f, true, true, farHit, farLayer)) {
            return DebugBackgroundLayerHit(farHit, farLayer);
        }
        return DebugBackgroundMissHit(rayDir);
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
    const bool sparseOnlyMode = frame.sparseNearParams.x > 0.5f && ((sparseNearFlags & 1u) != 0u);
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
    bool sawSparseMissing = false;
    float firstSparseMissingDist = 1e20f;

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
            sawSparseMissing = true;
            firstSparseMissingDist = min(firstSparseMissingDist, dist);
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
            RecordRenderOwnership(RENDER_OWNER_NEAR);
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

    if (sparseOnlyMode && sawSparseMissing) {
        RayHit backgroundHit;
        uint backgroundLayer;
        // Missing sparse pages inside the editable/collision volume are not
        // proof that far terrain owns that ray segment. Keep the background
        // renderer behind the same transition band as the surface path so stale
        // or late near pages cannot be filled by detached coarse terrain.
        const float holeFillStart = max(
            entryDist + firstSparseMissingDist + 24.0f,
            SparseMissingPageBackgroundStartForRay(rayOrigin, rayDir, gridMin, gridMax));
        if (RaymarchBackgroundField(
            rayOrigin,
            rayDir,
            holeFillStart,
            true,
            true,
            backgroundHit,
            backgroundLayer)) {
            return DebugBackgroundLayerHit(backgroundHit, backgroundLayer);
        }
        return DebugUnsafeNearMissHit(rayDir);
    }

    // If the ray cleanly exits the dense editable cache, continue into the
    // far-field renderer from just beyond the cache. This preserves the earlier
    // protection against drawing far terrain through missing near chunks, while
    // avoiding a hard sky cutoff when the camera pans past the near window.
    if (entryDist + dist >= tMax - 1.0f) {
        RayHit farHit;
        uint farLayer;
        float farStart = max(tMax + 8.0f, entryDist + dist);
        if (RaymarchBackgroundField(rayOrigin, rayDir, farStart, true, false, farHit, farLayer)) {
            return DebugBackgroundLayerHit(farHit, farLayer);
        }
    }

    return DebugBackgroundMissHit(rayDir);
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
    // The preview is a screen-space overlay on top of whatever world renderer is
    // active. If stale brush state puts the preview very near the camera, the
    // sphere projects as a giant fisheye dome and hides the sparse surface pass.
    float3 toBrush = brushCenter - rayOrigin;
    float distToCenter = length(toBrush);
    if (brushRadius <= 0.0f || distToCenter < max(brushRadius * 3.75f, 12.0f)) {
        return float4(0, 0, 0, 0);
    }

    float3 brushDir = toBrush / max(distToCenter, 0.001f);
    if (dot(brushDir, rayDir) <= 0.0f) {
        // Brush is behind this pixel ray; do not let behind-camera preview
        // state leak into the visible frame.
        return float4(0, 0, 0, 0);
    }

    const float angularRadius = asin(saturate(brushRadius / max(distToCenter, 0.001f)));
    if (angularRadius > 0.34f) {
        return float4(0, 0, 0, 0);
    }

    // Ray-sphere intersection for brush preview
    float3 oc = -toBrush;
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

[earlydepthstencil]
PSOutput main(PSInput input) {
    PSOutput output;
    output.color = float4(0.0f, 0.0f, 0.0f, 1.0f);

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
        output.color = float4(0.08f, 0.16f, 0.22f, 1.0f);
        return output;
    }
    if (frame.debugMode == 48u) {
        uint voxel = SparseBrickVoxelPool[0];
        uint material = GetMaterial(voxel);
        float shade = saturate((float)material / 16.0f);
        output.color = float4(0.05f + shade, 0.10f, 0.18f, 1.0f);
        return output;
    }

    // Render voxel world
    RayHit worldHit = Raymarch(cameraPos, rayDir);
    float4 voxelColor = worldHit.color;
    float depthDistance = worldHit.distance;

    float avatarT;
    float4 avatarColor;
    if (RenderBlockCharacter(cameraPos, rayDir, avatarT, avatarColor) && avatarT < worldHit.distance) {
        voxelColor = avatarColor;
        depthDistance = avatarT;
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

    output.color = voxelColor;
    return output;
}
