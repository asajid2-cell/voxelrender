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
// Mid-HEIGHT clipmap resident-tile ceiling. This is ONLY a min() clamp on the
// resident tile count + a compact-index bound check; the tile lookup is a hash
// probe (LookupResidentMidClipmapTile) whose cost is independent of tile count,
// so raising this does NOT unroll/bloat the PSO the way the voxel-brick cap does.
// Grown 256 -> 512 so the GPU-gen larger world (ringCount=5, endDistance=9000)
// can keep its full mid-height interest set (~285-306 tiles) resident; at 256 the
// interest set overflowed and midCov(height) capped at 256/~296 ~= 0.87.
static const uint MID_CLIPMAP_MAX_SHADER_TILES = 512u;
static const uint MID_CLIPMAP_MAX_SHADER_RINGS = 8u;
static const uint MID_CLIPMAP_LOOKUP_PROBES = 8u;
static const uint MID_VOXEL_CLIPMAP_MAX_BRICKS = 16384u;
// The CPU sparse page table probes until it reaches an empty slot. Runtime
// eviction leaves tombstones behind, so valid ready-to-render bricks can sit
// past a short probe window even at modest load. Keep the shader probe budget
// high enough that "CPU ready / GPU missing" does not expose holes or fake
// background terrain in the editable near field.
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;
static const uint SPARSE_SURFACE_RANGE_LOOKUP_PROBES = 32u;

static const float FAR_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FAR_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FAR_SEA_LEVEL = -48.0f;
static const float FAR_WATER_SURFACE_Y = FAR_SEA_LEVEL + 1.0f;
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
    uint diagnosticFlags;
};

static const uint RAY_DIAGNOSTIC_MID_PARENT_HELD = 1u;
static const uint RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK = 2u;
static const uint RAY_DIAGNOSTIC_MID_COLUMN = 4u;
static const uint RAY_DIAGNOSTIC_MID_CLOSURE = 8u;

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
        if (all(candidate.coord == brickCoord) && ((candidate.flags & 1u) != 0u)) {
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
    hit.diagnosticFlags = 0u;
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
        frame.debugMode == 49u || frame.debugMode == 50u ||
        frame.debugMode == 58u || frame.debugMode == 59u ||
        frame.debugMode == 60u || frame.debugMode == 61u ||
        frame.debugMode == 62u || frame.debugMode == 63u ||
        frame.debugMode == 65u || frame.debugMode == 66u ||
        frame.debugMode == 67u;
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
    // TANDEM fix (F1): far-height terrain hazes TOWARD SkyColor, but at moderate
    // distance the far bumps stay only ~half-hazed and read as detached grey
    // chunks against crisp sky in the gaps between ridge tops. Lift the
    // near-horizon sky toward a pale atmosphere so the gaps match the semi-hazed
    // bumps -> one coherent hazy ridge instead of floating chunks. Squared narrow
    // band fades by rayDir.y ~0.30 (upper sky untouched); subtle (no F4 murk).
    const float horizonAirBand = saturate((0.30f - abs(rayDir.y)) / 0.30f);
    const float3 horizonAir = float3(0.80f, 0.84f, 0.88f);
    sky = lerp(sky, horizonAir, horizonAirBand * horizonAirBand * 0.38f);
    sky += float3(1.00f, 0.72f, 0.34f) * sunBloom * 0.32f;
    sky += float3(1.00f, 0.93f, 0.74f) * sun * 1.75f;
    sky += float3(0.18f, 0.24f, 0.38f) * antiSun * 0.10f;

    return saturate(sky);
}

float3 SkyAmbient(float3 normal) {
    float up = saturate(normal.y * 0.5f + 0.5f);
    float3 groundBounce = float3(0.36f, 0.32f, 0.28f);
    float3 skyBounce = float3(0.50f, 0.56f, 0.62f);
    return lerp(groundBounce, skyBounce, up);
}

float UnderwaterCaustic(float3 worldPos) {
    const float t = (float)(frame.frameIndex & 2047u) * 0.018f;
    const float a = sin(worldPos.x * 0.115f + worldPos.z * 0.071f + t);
    const float b = sin(worldPos.x * 0.047f - worldPos.z * 0.163f - t * 0.72f);
    const float c = sin((worldPos.x + worldPos.z + worldPos.y * 0.35f) * 0.091f + t * 1.37f);
    return saturate(0.50f + a * b * 0.24f + c * 0.14f);
}

float UnderwaterParticulate(float3 worldPos) {
    const float t = (float)(frame.frameIndex & 4095u) * 0.009f;
    const float a = sin(worldPos.x * 0.029f + worldPos.y * 0.061f + t);
    const float b = sin(worldPos.z * 0.037f - worldPos.y * 0.023f - t * 0.83f);
    return saturate(0.52f + a * 0.15f + b * 0.13f);
}

float3 UnderwaterVolumeTint(float3 worldPos, float distanceFromCamera, float baseFog, out float fogStrength) {
    const float waterColumn = saturate((distanceFromCamera - 72.0f) / 300.0f);
    const float aboveWaterPenalty = saturate((worldPos.y - FAR_SEA_LEVEL + 8.0f) / 88.0f);
    const float verticalSilhouette = saturate((worldPos.y - frame.cameraPosition.y - 12.0f) / 120.0f);
    const float particulate = UnderwaterParticulate(worldPos);
    fogStrength = saturate(
        baseFog +
        waterColumn * 0.22f +
        aboveWaterPenalty * 0.20f +
        verticalSilhouette * 0.14f);
    const float3 deepTint = lerp(
        float3(0.15f, 0.36f, 0.40f),
        float3(0.12f, 0.32f, 0.36f) + particulate * float3(0.020f, 0.038f, 0.034f),
        waterColumn);
    const float3 surfaceVolumeTint =
        float3(0.22f, 0.45f, 0.49f) + particulate * float3(0.018f, 0.032f, 0.028f);
    return lerp(
        deepTint,
        surfaceVolumeTint,
        saturate(aboveWaterPenalty * 0.42f + verticalSilhouette * 0.28f));
}

float3 DistantLodShadeNormal(float3 normal, float distanceFromCamera, float strength) {
    const float distanceBlend = saturate((distanceFromCamera - 900.0f) / 6200.0f);
    return normalize(lerp(normal, float3(0.0f, 1.0f, 0.0f), saturate(strength * (0.35f + distanceBlend * 0.55f))));
}

float VoxelGridLine(float2 worldUv, float cellSize, float strength) {
    const float safeCellSize = max(cellSize, 1.0f);
    const float2 cell = abs(frac(worldUv / safeCellSize) - 0.5f);
    const float gridLine = 1.0f - smoothstep(0.455f, 0.495f, max(cell.x, cell.y));
    return saturate(gridLine * strength);
}

float FarSmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

float FarRidged(float value, float power) {
    return pow(saturate(1.0f - abs(value)), power);
}

uint FarWorldSeed() {
    return asuint(frame.exactNearParams.y);
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
    const uint worldSeed = FarWorldSeed();
    float broad = FarValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, worldSeed + 11u);
    float ridgeSource = FarValueNoise2D(
        xz.x * 0.0100f + 41.0f,
        xz.y * 0.0100f - 17.0f,
        worldSeed + 23u);
    float ridge = 1.0f - abs(ridgeSource);
    float detail = FarValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        worldSeed + 37u);

    float ridgeHeight = ridge * ridge;

    // VISUAL PASS iter1 (landforms): mirror the CHEAP amplitude changes from the CPU
    // HeightAt (broad 145 -> 92, detail 8 -> 3). These add no ops, so they do not
    // risk the startup GPU TDR. The terrace quantization and spawn-landmass reshape
    // are intentionally NOT applied in this per-step gradient/far fallback (see note
    // below) to keep it cheap; the actual geometry copies carry those.
    float height = -64.0f;
    height += broad * 92.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 3.0f;

    float2 originDelta = xz - float2(192.0f, 224.0f);
    float originDistance = length(originDelta);
    float originComfort = 1.0f - FarSmooth01(saturate((originDistance - 180.0f) / 520.0f));
    float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - FarSmooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - FarSmooth01(originDistance / 420.0f)) * 58.0f;
    height = lerp(height, publicRegionHeight, originComfort * 0.94f);
    float publicCapInfluence =
        1.0f - FarSmooth01(saturate((originDistance - 220.0f) / 420.0f));
    float publicCap =
        58.0f + FarSmooth01(saturate(originDistance / 640.0f)) * 114.0f;
    height = lerp(height, min(height, publicCap), publicCapInfluence);

    float submergedBlend =
        1.0f - FarSmooth01(saturate((height - (FAR_SEA_LEVEL + 28.0f)) / 86.0f));
    if (submergedBlend > 0.0f) {
        float submergedShelfHeight =
            (FAR_SEA_LEVEL - 8.0f) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - FarSmooth01(originDistance / 520.0f)) * 18.0f;
        height = lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    float playableBankBand =
        1.0f - FarSmooth01(saturate((originDistance - 260.0f) / 980.0f));
    float lowlandUpper =
        1.0f - FarSmooth01(saturate((height - (FAR_SEA_LEVEL + 96.0f)) / 120.0f));
    float lowlandFloor =
        FarSmooth01(saturate((height - (FAR_SEA_LEVEL - 40.0f)) / 64.0f));
    float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    float playableShelfHeight =
        (FAR_SEA_LEVEL + 18.0f) +
        broad * 28.0f +
        ridgeHeight * 10.0f +
        detail * 1.5f +
        (1.0f - FarSmooth01(saturate(originDistance / 460.0f))) * 42.0f;
    height = lerp(height, playableShelfHeight, playableBankBlend);
    float publicBasinBand =
        FarSmooth01(saturate((originDistance - 360.0f) / 240.0f)) *
        (1.0f - FarSmooth01(saturate((originDistance - 1700.0f) / 760.0f))) *
        FarSmooth01(saturate((height - (FAR_SEA_LEVEL - 38.0f)) / 56.0f)) *
        (1.0f - FarSmooth01(saturate((height - (FAR_SEA_LEVEL + 180.0f)) / 140.0f)));
    float publicBasinFloor =
        (FAR_SEA_LEVEL - 12.0f) +
        broad * 2.0f +
        detail * 0.35f;
    height = lerp(height, min(height, publicBasinFloor), publicBasinBand * 0.80f);
    float backdropNoise = FarValueNoise2D(
        xz.x * 0.0018f + 19.0f,
        xz.y * 0.0018f - 31.0f,
        worldSeed + 211u);
    float backdropRidgeSource = FarValueNoise2D(
        xz.x * 0.0032f - 71.0f,
        xz.y * 0.0032f + 43.0f,
        worldSeed + 227u);
    float backdropRidge = 1.0f - abs(backdropRidgeSource);
    float backdropBreakup = FarValueNoise2D(
        xz.x * 0.0075f + 203.0f,
        xz.y * 0.0075f - 167.0f,
        worldSeed + 271u);
    float backdropNotch =
        FarSmooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    float silhouetteRidge = saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    float backdropBand =
        FarSmooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - FarSmooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    float northBackdrop = FarSmooth01(saturate((xz.y - 1180.0f) / 900.0f));
    float sideBackdrop = FarSmooth01(saturate((abs(xz.x - 192.0f) - 820.0f) / 980.0f));
    float backdropFacing = saturate(northBackdrop + sideBackdrop * 0.58f);
    float silhouetteContinuity = saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    float backdropInfluence =
        backdropBand *
        backdropFacing *
        FarSmooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    float backdropHeight =
        248.0f +
        backdropBand * 160.0f +
        silhouetteContinuity * 186.0f +
        backdropNoise * 26.0f;
    height = lerp(height, max(height, backdropHeight), backdropInfluence * 0.70f);

    float westCorridor = FarSmooth01(saturate((192.0f - xz.x - 520.0f) / 820.0f));
    float eastCorridor = FarSmooth01(saturate((xz.x - 192.0f - 520.0f) / 820.0f));
    float southBlend = FarSmooth01(saturate((360.0f - xz.y) / 1200.0f));
    float westNorthBlend = FarSmooth01(saturate((xz.y - 360.0f) / 920.0f));
    float routeDistanceBand =
        FarSmooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - FarSmooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    float routeRidgeNoiseA = FarValueNoise2D(
        xz.x * 0.0024f + 113.0f,
        xz.y * 0.0024f - 89.0f,
        worldSeed + 251u);
    float routeRidgeNoiseB = FarValueNoise2D(
        xz.x * 0.0068f - 37.0f,
        xz.y * 0.0068f + 151.0f,
        worldSeed + 263u);
    float routeBreakup = FarValueNoise2D(
        xz.x * 0.0110f - 211.0f,
        xz.y * 0.0110f + 73.0f,
        worldSeed + 281u);
    float routeNotch =
        FarSmooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    float routeRidge =
        saturate(
            0.26f +
            (1.0f - abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f);
    float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = lerp(height, max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    // NOTE: the spawn-landmass reshape is intentionally NOT applied in this raymarch
    // copy of the terrain function. It is the gradient/far-fallback evaluator and is
    // marched per-step whole-screen during the startup fill (before surface/mid-voxel
    // stream in); adding the reshape here pushed that one frame past the ~2s GPU TDR
    // limit (DEVICE_HUNG). The actual terrain geometry keeps the reshape via the CPU
    // HeightAt, GPU TH_HeightAt, and FarVoxelOctree copies, so the world is still solid
    // land near spawn; only this fallback's far/gradient shape omits it (faded by 2800u).

    mountainMask = saturate((ridgeHeight * 150.0f + max(height - 160.0f, 0.0f)) / 300.0f);
    spireMask = 0.0f;
    ravineMask = 0.0f;
    return clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
}

float2 FarTerrainClosureInfluence(float2 xz) {
    const uint worldSeed = FarWorldSeed();
    float2 originDelta = xz - float2(192.0f, 224.0f);
    float originDistance = length(originDelta);

    float backdropNoise = FarValueNoise2D(
        xz.x * 0.0018f + 19.0f,
        xz.y * 0.0018f - 31.0f,
        worldSeed + 211u);
    float backdropRidgeSource = FarValueNoise2D(
        xz.x * 0.0032f - 71.0f,
        xz.y * 0.0032f + 43.0f,
        worldSeed + 227u);
    float backdropRidge = 1.0f - abs(backdropRidgeSource);
    float backdropBreakup = FarValueNoise2D(
        xz.x * 0.0075f + 203.0f,
        xz.y * 0.0075f - 167.0f,
        worldSeed + 271u);
    float backdropNotch =
        FarSmooth01(saturate((backdropBreakup - 0.08f) / 0.58f));
    float silhouetteRidge = saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    float backdropBand =
        FarSmooth01(saturate((originDistance - 1360.0f) / 700.0f)) *
        (1.0f - FarSmooth01(saturate((originDistance - 5200.0f) / 1200.0f)));
    float northBackdrop = FarSmooth01(saturate((xz.y - 1180.0f) / 900.0f));
    float sideBackdrop = FarSmooth01(saturate((abs(xz.x - 192.0f) - 820.0f) / 980.0f));
    float backdropFacing = saturate(northBackdrop + sideBackdrop * 0.58f);
    float silhouetteContinuity = saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    float backdropInfluence =
        backdropBand *
        backdropFacing *
        FarSmooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f) *
        0.62f;

    float westCorridor = FarSmooth01(saturate((192.0f - xz.x - 520.0f) / 820.0f));
    float eastCorridor = FarSmooth01(saturate((xz.x - 192.0f - 520.0f) / 820.0f));
    float southBlend = FarSmooth01(saturate((360.0f - xz.y) / 1200.0f));
    float westNorthBlend = FarSmooth01(saturate((xz.y - 360.0f) / 920.0f));
    float routeDistanceBand =
        FarSmooth01(saturate((originDistance - 780.0f) / 420.0f)) *
        (1.0f - FarSmooth01(saturate((originDistance - 4300.0f) / 1200.0f)));
    float routeRidgeNoiseA = FarValueNoise2D(
        xz.x * 0.0024f + 113.0f,
        xz.y * 0.0024f - 89.0f,
        worldSeed + 251u);
    float routeRidgeNoiseB = FarValueNoise2D(
        xz.x * 0.0068f - 37.0f,
        xz.y * 0.0068f + 151.0f,
        worldSeed + 263u);
    float routeBreakup = FarValueNoise2D(
        xz.x * 0.0110f - 211.0f,
        xz.y * 0.0110f + 73.0f,
        worldSeed + 281u);
    float routeNotch =
        FarSmooth01(saturate((routeBreakup - 0.02f) / 0.60f));
    float routeRidge =
        saturate(
            0.26f +
            (1.0f - abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f);
    float routeCorridor = routeDistanceBand * saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend) *
        routeRidge *
        routeNotch *
        0.68f;

    return float2(
        saturate(backdropInfluence),
        saturate(routeCorridor + (worldSeed == 0u ? 0.0f : 0.0f)));
}

float3 DebugClosureColor(float3 worldPos) {
    const float2 influence = FarTerrainClosureInfluence(worldPos.xz);
    const float base = 1.0f - saturate(max(influence.x, influence.y));
    return saturate(
        float3(0.05f, 0.26f, 0.08f) * base +
        float3(1.0f, 0.78f, 0.05f) * influence.x +
        float3(1.0f, 0.05f, 0.05f) * influence.y);
}

float3 DebugOwnerLayerColor(uint layer) {
    if (layer == 1u) {
        return float3(0.05f, 0.95f, 0.25f);
    }
    if (layer == 2u) {
        return float3(1.0f, 0.86f, 0.08f);
    }
    if (layer == 3u) {
        return float3(0.20f, 0.42f, 1.0f);
    }
    if (layer == 4u) {
        return float3(1.0f, 0.45f, 0.08f);
    }
    if (layer == 5u) {
        return float3(0.02f, 0.78f, 1.0f);
    }
    return float3(0.10f, 0.12f, 0.14f);
}

float QuantizeTerrainHeight(float height, float verticalStep) {
    verticalStep = max(verticalStep, 1.0f);
    return floor(height / verticalStep) * verticalStep;
}

float QuantizeTerrainTopHeight(float height, float verticalStep) {
    verticalStep = max(verticalStep, 1.0f);
    return ceil(height / verticalStep) * verticalStep;
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

// Spawn-landmass agreement for the analytic far layers (CONSUMER-SITE form).
// The geometry layers (CPU HeightAt / TH_HeightAt / FarVoxelOctree) lift
// near-origin terrain onto a land floor (SEA+56 + noise, band (d-200)/9300,
// gone by ~9500u). FarTerrainHeight itself MUST NOT carry this block: adding
// ANY code inside that function (even branch-gated off, even a 3-op constant
// floor) regressed the uber-shader PSO so badly that the startup frame went
// 75ms -> 2.9-3.3s and TDR'd the device (DEVICE_REMOVED 0x887A0005, measured
// 2026-06-09; the function is inlined ~50x including every far march loop).
// Instead the few consumers whose DISAGREEMENT is visible (far-water existence,
// water-occluder replacement of real land hits, far material water/lake
// classification) apply the reshape via this band helper at their single
// call sites, outside all march loops.
// STARTUP-TDR SAFETY: gated on the mid-voxel ring residency signal (same
// signal and rationale as deferFarSvoToFarHeightHorizon): during the startup
// whole-screen fallback fill this returns 0 and every cheap analytic-water
// early-out behaves exactly as before; residency crosses 0.5 around frame ~31
// while the public render stays HELD until frame ~120, so the far-field morph
// from flooded basin to spawn land is never publicly visible.
float FarSpawnLandBand(float2 xz) {
    const float2 originDelta = xz - float2(192.0f, 224.0f);
    const float originDistance = length(originDelta);
    // TANDEM widen 9300 -> 35000: solid continent fills the render range (matches
    // the geometry copies SparseTerrainGenerator/TerrainHeight/FarVoxelOctree).
    const float rawBand = 1.0f - FarSmooth01(saturate((originDistance - 200.0f) / 90000.0f));
    // BRANCHLESS residency gate (driver-JIT safety): the early-out
    // 'if (midResidencyParams.y < 0.5) return 0' crashes the NVIDIA driver when
    // this is inlined many times in the deep far path. step()-multiply is
    // bit-identical (0 below the gate, rawBand at/above). The Renderer latch
    // holds the published signal >= 0.51 once height OR voxel coverage is good,
    // so the reshape stays active at altitude; this gate only hides the startup flash.
    return rawBand * step(0.5f, frame.midResidencyParams.y);
}

// Base of the geometry spawn-land floor's NOISE terms (broad*18 + detail*3;
// see SparseTerrainGenerator::HeightAt / FarVoxelOctree::TerrainHeight:
// floor = SEA+56 + broad*18 + ridgeHeight*40 + detail*3). broad and detail are
// the SAME first/third noises FarTerrainHeight evaluates at this xz (identical
// frequency/offset/seed), so wherever both are inlined at the same coordinate
// the optimizer can share the hash chains. The ridgeHeight*40 term is NOT
// evaluated here: every consumer already holds FarTerrainHeight's mountainMask
// for this xz, and wherever the floor can matter (height <= 160) that mask is
// exactly ridgeHeight/2 unsaturated, so ridgeHeight*40 == mountainMask*80 for
// free (see FarSpawnLandReshapeHeight).
// WHY THE NOISE TERMS MATTER (was a constant SEA+56): the geometry floor
// carries -21..+61u of noise. In the band fade annulus the water/land
// classification of a deep basin column is hyper-sensitive to the floor value
// (lifted = lerp(h, max(h, floor), band) straddles SEA), so the constant floor
// disagreed with geometry over whole basins: a hard circular arc at the mid
// ring boundary (mid renders true navy water, far renders pale lifted "land")
// plus tan far-SVO leaf fragments floating on mid water.
float FarSpawnLandFloorBase(float2 xz) {
    const uint worldSeed = FarWorldSeed();
    const float broad = FarValueNoise2D(xz.x * 0.0045f, xz.y * 0.0045f, worldSeed + 11u);
    const float detail = FarValueNoise2D(
        xz.x * 0.035f - 13.0f,
        xz.y * 0.035f + 29.0f,
        worldSeed + 37u);
    return (FAR_SEA_LEVEL + 56.0f) + broad * 18.0f + detail * 3.0f;
}

// Reshaped-height view for consumer sites, in geometry-floor NOISE parity.
// mountainMask must be FarTerrainHeight's out mask for this same xz:
//  - height <= 160: mask = saturate((ridgeHeight*150 + 0)/300) = ridgeHeight/2
//    (unsaturated, ridgeHeight <= 1), so mask*80 == ridgeHeight*40 EXACTLY.
//  - height > 160: the recovered floor tops out at 29 + 80 = 109 < 160 <
//    height, so max(height, floor) ignores the floor either way. Safe.
// BRANCHLESS ON PURPOSE (driver-JIT safety, 2026-06-10): an early-out
// 'if (band <= 0) return height;' here — inlined ~dozens of times into deep
// far-field control flow — deterministically crashed the NVIDIA driver at
// EXECUTION time (nvwgf2umx.dll 0xC0000005, same fault offset, ~frame 58 of
// the crash profile, exactly when the high-altitude background path first
// activates). The unconditional form is semantics-identical: lerp(h, x, 0)
// == h exactly and the floor terms are finite/bounded, so band == 0 still
// returns the raw height bit-for-bit.
float FarSpawnLandReshapeHeight(float2 xz, float height, float mountainMask) {
    const float band = FarSpawnLandBand(xz);
    const float floorHeight = FarSpawnLandFloorBase(xz) + mountainMask * 80.0f;
    return lerp(height, max(height, floorHeight), band);
}

// Same floor with a precomputed band + per-leaf floor base: the SVO
// leaf-resolution loops compute the band and the broad/detail floor base ONCE
// per leaf (the leaf interval spans <= ~55u: band drift < 0.6% of the 9300u
// ramp, broad/detail drift well under the leaf quantization) and reapply the
// cheap floor per height sample. mountainMask is the per-sample mask from the
// FarTerrainHeight call that produced `height`, recovering the per-sample
// ridgeHeight*40 floor term exactly (see FarSpawnLandReshapeHeight).
float FarSpawnLandApplyFloor(float height, float band, float floorBase, float mountainMask) {
    return lerp(height, max(height, floorBase + mountainMask * 80.0f), band);
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
    // Spawn-land agreement: the voxelized render-side far height IS the world
    // the geometry layers describe, so it must carry the spawn-land floor.
    // Reshaping here (one body) makes the far-height backdrop march, its
    // gradient normals, the mid procedural air test and the mid interior
    // recovery all agree with the reshaped geometry; FarTerrainHeight itself
    // stays untouched (in-function edits TDR the PSO, see FarSpawnLandBand).
    return QuantizeTerrainTopHeight(
        FarSpawnLandReshapeHeight(sampleXz, rawHeight, mountainMask),
        max(4.0f, cellSize * 0.75f));
}

uint FarTerrainMaterial(float2 xz, float height, float mountainMask, float spireMask, float ravineMask) {
    // Match the generated top-surface material rule used by
    // SparseTerrainGenerator::SampleGeneratedVoxel.
    float mm, sm, rm;
    const float hx0 = FarTerrainHeight(xz - float2(4.0f, 0.0f), mm, sm, rm);
    const float hx1 = FarTerrainHeight(xz + float2(4.0f, 0.0f), mm, sm, rm);
    const float hz0 = FarTerrainHeight(xz - float2(0.0f, 4.0f), mm, sm, rm);
    const float hz1 = FarTerrainHeight(xz + float2(0.0f, 4.0f), mm, sm, rm);
    const float localRelief =
        max(max(abs(hx0 - height), abs(hx1 - height)), max(abs(hz0 - height), abs(hz1 - height)));

    // Spawn-land agreement: every caller passes a height that ALREADY carries
    // the spawn-land floor (FarTerrainHeightVoxelized / FarSpawnLandApplyFloor),
    // so classify it directly. Re-applying the reshape here double-lifted the
    // band-annulus heights by up to +24u, biasing the far material toward dry
    // land over columns the geometry keeps as water. The relief gradient above
    // intentionally stays raw: the reshape is locally smooth, so relief
    // differences cancel in the abs() terms.

    if (height < FAR_SEA_LEVEL) {
        return MAT_WATER;
    }
    if (height < FAR_SEA_LEVEL + 48.0f && localRelief < 36.0f) {
        return MAT_SAND;
    }
    if (height < FAR_SEA_LEVEL + 72.0f) {
        return (height < FAR_SEA_LEVEL + 48.0f && localRelief < 36.0f) ? MAT_SAND : MAT_DIRT;
    }
    if (height < FAR_SEA_LEVEL + 128.0f) {
        return (height < FAR_SEA_LEVEL + 86.0f && localRelief < 58.0f) ? MAT_SAND : MAT_DIRT;
    }
    if (localRelief > 10.0f || height > 160.0f) {
        return MAT_STONE;
    }
    return MAT_DIRT;
}

float3 ApplyWaterlineWetTerrainTint(float3 baseColor, uint material, float worldY, float normalY, float strengthScale) {
    if (material != MAT_SAND && material != MAT_DIRT && material != MAT_STONE) {
        return baseColor;
    }

    const float waterlineFace = 1.0f - saturate((worldY - FAR_SEA_LEVEL + 2.0f) / 14.0f);
    const float verticalBank = saturate((0.54f - normalY) / 0.62f);
    const float wetBoundary = waterlineFace * (0.35f + verticalBank * 0.65f);
    const float3 wetSediment = material == MAT_SAND
        ? float3(0.40f, 0.43f, 0.34f)
        : float3(0.34f, 0.39f, 0.34f);
    return lerp(baseColor, wetSediment, saturate(wetBoundary * strengthScale));
}

float3 FarTerrainMaterialVariation(
    float3 baseColor,
    uint material,
    float2 xz,
    float height,
    float distanceFromCamera)
{
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    const int cellX = (int)floor(xz.x / max(cellSize, 1.0f));
    const int cellZ = (int)floor(xz.y / max(cellSize, 1.0f));
    const int cellY = (int)floor(height / max(cellSize * 0.75f, 4.0f));
    const uint worldSeed = FarWorldSeed();
    const float patch =
        (float)(FarHash3D(cellX, cellY, cellZ, worldSeed + 131u) & 0xFFFFu) / 65535.0f;
    const float broad =
        (float)(FarHash3D(cellX >> 2, cellY, cellZ >> 2, worldSeed + 173u) & 0xFFFFu) / 65535.0f;
    // PALETTE UNIFICATION (altitude patchwork): ramp the variation in over
    // ~500-2500u instead of 700-5900u. The old slow ramp left far ground at
    // ~10-12% variation in the 1.5-3k band where mid voxels next to it carry
    // ~45-55% (BackgroundTerrainMaterialVariation) -> far read as a flat pale
    // base palette, a different art style. Matched gates: far dirt 0.56 /
    // stone 0.74 at blend 1 vs mid ~0.54-0.60 converged.
    const float distanceBlend = saturate((distanceFromCamera - 500.0f) / 2000.0f);
    const float farSoftening = saturate((distanceFromCamera - 1200.0f) / 4200.0f);
    const float materialPatch = lerp(patch, broad, 0.36f + farSoftening * 0.34f);

    float3 varied = baseColor;
    if (material == MAT_DIRT) {
        // PALETTE UNIFICATION: center the far grass/scrub on the SAME hue family
        // the near+mid layers share (BackgroundTerrainMaterialVariation grassDirt
        // (0.36,0.52,0.25) / dryScrub (0.50,0.49,0.32)). The old darker/browner
        // pair made far ground read as a different world than the mid band.
        const float3 grass = lerp(float3(0.33f, 0.50f, 0.23f), float3(0.40f, 0.54f, 0.27f), materialPatch);
        const float3 scrub = lerp(float3(0.50f, 0.49f, 0.32f), float3(0.44f, 0.45f, 0.28f), broad);
        const float exposedTop = saturate((height - 54.0f) / 150.0f);
        varied = lerp(grass, scrub, exposedTop * 0.58f + (1.0f - materialPatch) * 0.20f);
    } else if (material == MAT_STONE) {
        varied = lerp(float3(0.40f, 0.42f, 0.36f), float3(0.58f, 0.55f, 0.44f), materialPatch * 0.70f + 0.10f);
        const float lowStoneVegetation =
            (1.0f - saturate((height - (FAR_SEA_LEVEL + 48.0f)) / 240.0f)) *
            smoothstep(0.36f, 0.92f, broad);
        const float weatheredFace = smoothstep(0.48f, 0.88f, broad) * (1.0f - farSoftening * 0.30f);
        varied = lerp(varied, float3(0.42f, 0.49f, 0.32f), saturate(lowStoneVegetation * 0.28f + weatheredFace * 0.10f));
    } else if (material == MAT_SAND) {
        varied = lerp(float3(0.62f, 0.56f, 0.36f), float3(0.76f, 0.67f, 0.42f), materialPatch);
    }

    if (material == MAT_STONE) {
        const float lowMountainBlend = saturate((170.0f - height) / 150.0f) * farSoftening;
        varied = lerp(varied, float3(0.38f, 0.44f, 0.32f), lowMountainBlend * 0.24f);
    }
    // PALETTE UNIFICATION: halved (0.18 -> 0.08). The grey-down desaturated the
    // whole far band vs the crisp mid voxels in aerial views; distance haze in
    // the shade sites already carries the atmospheric read.
    const float3 aerialTerrain = lerp(float3(0.43f, 0.49f, 0.35f), float3(0.54f, 0.53f, 0.45f), broad);
    varied = lerp(varied, aerialTerrain, farSoftening * 0.08f);
    varied = lerp(varied, baseColor, farSoftening * 0.08f);
    varied = lerp(baseColor, varied, distanceBlend * (material == MAT_STONE ? 0.74f : 0.56f));
    return ApplyWaterlineWetTerrainTint(varied, material, height, 1.0f, 0.62f);
}

// AERIAL HAZE GATE: the far-shade haze floors and distance terms exist to
// dissolve the HORIZON silhouette at ground level (continuous hazy ridge).
// Steeply-downward rays (aerial views) resolve nearby well-lit surfaces, where
// those same floors wash the far band toward pale sky and break the one-world
// read against the crisp mid voxels. Scale the haze down as rays leave the
// horizon band. Ground-level rays that reach the far backdrop are always far
// shallower than -0.18 (the camera is only a few voxels above the terrain), so
// the committed hazy-ridge look is unchanged. Cheap: 3 ALU.
float FarHazeDowncastScale(float rayDirY) {
    return 1.0f - saturate((-rayDirY - 0.18f) / 0.30f) * 0.65f;
}

float3 BackgroundTerrainMaterialVariation(
    float3 baseColor,
    uint material,
    float3 worldPos,
    float3 normal,
    float distanceFromCamera,
    float strength)
{
    // TANDEM grain fix (w/ Codex): the per-cell color variation read as a
    // "static TV filter" across the mid band - small (10-22u) high-contrast cells
    // render as only a few pixels each at mid distance, and the old distance ramp
    // AMPLIFIED them. Fix = ~3x larger cells (coherent patches) + lower per-cell
    // contrast + the strength now FADES with distance (atmospheric coherence)
    // instead of growing. Near-field keeps enough variation to not read flat.
    const float cellScale = lerp(30.0f, 68.0f, saturate((distanceFromCamera - 900.0f) / 3600.0f));
    const int cellX = (int)floor(worldPos.x / cellScale);
    const int cellY = (int)floor(worldPos.y / max(cellScale, 1.0f));
    const int cellZ = (int)floor(worldPos.z / cellScale);
    const uint worldSeed = FarWorldSeed();
    const float patch =
        (float)(FarHash3D(cellX, cellY, cellZ, worldSeed + 311u) & 0xFFFFu) / 65535.0f;
    const float largePatch =
        (float)(FarHash3D(cellX >> 2, cellY >> 1, cellZ >> 2, worldSeed + 337u) & 0xFFFFu) / 65535.0f;
    const float stonePatch = lerp(patch, largePatch, 0.48f);
    const float slope = saturate((0.72f - normal.y) / 0.64f);
    const float highExposure = saturate((worldPos.y - 70.0f) / 190.0f);
    const float distanceBlend = saturate((distanceFromCamera - 520.0f) / 2400.0f);

    float3 varied = baseColor;
    if (material == MAT_STONE) {
        const float3 coolStone = float3(0.40f, 0.42f, 0.36f);
        const float3 warmStone = float3(0.60f, 0.56f, 0.44f);
        const float3 lichenStone = float3(0.42f, 0.49f, 0.32f);
        const float lowAltitudeLichen = (1.0f - saturate((worldPos.y - FAR_SEA_LEVEL - 32.0f) / 220.0f));
        varied = lerp(coolStone, warmStone, stonePatch * 0.46f + 0.22f);
        const float ledgeLichen = (1.0f - slope) * smoothstep(0.54f, 0.92f, largePatch);
        varied = lerp(varied, lichenStone, saturate(ledgeLichen * 0.12f + lowAltitudeLichen * 0.08f));
    } else if (material == MAT_DIRT) {
        // TANDEM palette tune: greener grass, quieter dry-scrub so the mid reads
        // as clean grassland matching the near surface, not loud olive/tan.
        const float3 grassDirt = float3(0.32f, 0.58f, 0.24f);
        const float3 exposedDirt = float3(0.48f, 0.42f, 0.31f);
        const float3 dryScrub = float3(0.45f, 0.50f, 0.30f);
        varied = lerp(grassDirt, exposedDirt, slope * 0.68f + highExposure * 0.22f);
        varied = lerp(varied, dryScrub, smoothstep(0.62f, 0.94f, largePatch) * 0.10f);
    } else if (material == MAT_SAND) {
        const float3 dampSand = float3(0.56f, 0.52f, 0.37f);
        const float3 drySand = float3(0.72f, 0.66f, 0.43f);
        varied = lerp(dampSand, drySand, patch * 0.36f + (1.0f - slope) * 0.10f);
    }

    // INVERTED distance ramp: variation strength now FADES with distance
    // (distanceBlend) instead of growing, so the mid/far reads as coherent
    // smooth material under haze, not per-cell static. ~0.31 near-mid -> ~0.18-0.26 far.
    const float materialStrength = saturate(strength * (material == MAT_STONE ? (0.54f - distanceBlend * 0.18f) : (0.48f - distanceBlend * 0.20f)));
    return lerp(baseColor, varied, materialStrength);
}

// Per-WORLD-VOXEL micro color jitter (lever 2). The per-cell variation above gives
// large color patches; within a patch every block is identical, which is what still
// reads as flat-colored terraces. This adds a stable per-integer-voxel brightness +
// tiny hue skew (hashed on floor(worldPos)) so adjacent blocks differ subtly, like
// natural block-to-block texture. Cheap: one FarHash3D + a few mul/add. Fades out
// with distance so it never becomes far-field noise/shimmer.
float3 PerVoxelColorJitter(float3 color, float3 worldPos, float distanceFromCamera) {
    const int vx = (int)floor(worldPos.x);
    const int vy = (int)floor(worldPos.y);
    const int vz = (int)floor(worldPos.z);
    const uint h = FarHash3D(vx, vy, vz, FarWorldSeed() + 911u);
    const float j = (float)(h & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;      // ~[-1,1]
    const float j2 = (float)((h >> 16) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    const float fade = 1.0f - saturate((distanceFromCamera - 180.0f) / 440.0f);
    const float bright = 1.0f + j * 0.045f * fade;                       // +/-4.5% brightness (was 9%)
    // tiny per-channel hue skew (halved) so it's natural variation, not grey noise
    const float3 hue = float3(1.0f + j2 * 0.014f, 1.0f + j * 0.010f, 1.0f - j2 * 0.012f);
    return color * bright * lerp(float3(1.0f, 1.0f, 1.0f), hue, fade);
}

float3 DebugMaterialColor(uint material) {
    if (material == MAT_WATER) {
        return float3(0.05f, 0.38f, 1.0f);
    }
    if (material == MAT_SAND) {
        return float3(1.0f, 0.84f, 0.12f);
    }
    if (material == MAT_DIRT) {
        return float3(0.18f, 0.78f, 0.20f);
    }
    if (material == MAT_STONE) {
        return float3(0.55f, 0.55f, 0.55f);
    }
    if (material == MAT_AIR) {
        return float3(0.03f, 0.04f, 0.06f);
    }
    if (material == MAT_BEDROCK) {
        return float3(0.12f, 0.12f, 0.13f);
    }
    if (material == MAT_GLASS) {
        return float3(0.70f, 0.95f, 1.0f);
    }
    return float3(1.0f, 0.10f, 0.90f);
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
    return normalize(float3(
        (hx0 - hx1) * 0.58f,
        max(cellSize * 3.4f, 10.0f),
        (hz0 - hz1) * 0.58f));
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

void AccumulateMidClipmapMaterialWeight(
    uint candidateMaterial,
    float candidateWeight,
    bool allowWater,
    inout float bestWeight,
    inout uint bestMaterial)
{
    if (candidateWeight <= bestWeight) {
        return;
    }
    if (!allowWater && candidateMaterial == MAT_WATER) {
        return;
    }
    if (candidateMaterial == MAT_AIR) {
        return;
    }
    bestWeight = candidateWeight;
    bestMaterial = candidateMaterial;
}

uint ResolveMidClipmapBilinearMaterial(
    uint s00,
    uint s10,
    uint s01,
    uint s11,
    float fx,
    float fz,
    float height)
{
    const uint m00 = MidClipmapUnpackMaterial(s00);
    const uint m10 = MidClipmapUnpackMaterial(s10);
    const uint m01 = MidClipmapUnpackMaterial(s01);
    const uint m11 = MidClipmapUnpackMaterial(s11);

    const float w00 = (1.0f - fx) * (1.0f - fz);
    const float w10 = fx * (1.0f - fz);
    const float w01 = (1.0f - fx) * fz;
    const float w11 = fx * fz;

    // Mid-column fallback is a public LOD, not a water mask. CPU clipmap
    // samples clamp submerged columns to sea level, so a mixed shoreline cell
    // commonly contains both water and land corners. The old shader used s00
    // material for the whole bilinear height sample; one water corner could
    // therefore turn an interpolated land slope into water until exact sparse
    // chunks streamed in. Prefer non-water material whenever the interpolated
    // surface has risen above the sea plane.
    const bool aboveSeaSurface = height > FAR_SEA_LEVEL + 0.75f;
    float bestWeight = -1.0f;
    uint bestMaterial = MAT_AIR;
    AccumulateMidClipmapMaterialWeight(m00, w00, !aboveSeaSurface, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m10, w10, !aboveSeaSurface, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m01, w01, !aboveSeaSurface, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m11, w11, !aboveSeaSurface, bestWeight, bestMaterial);
    if (bestMaterial != MAT_AIR) {
        return bestMaterial;
    }

    bestWeight = -1.0f;
    AccumulateMidClipmapMaterialWeight(m00, w00, true, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m10, w10, true, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m01, w01, true, bestWeight, bestMaterial);
    AccumulateMidClipmapMaterialWeight(m11, w11, true, bestWeight, bestMaterial);
    return bestMaterial;
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
    material = ResolveMidClipmapBilinearMaterial(s00, s10, s01, s11, fx, fz, height);
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
    material = ResolveMidClipmapBilinearMaterial(s00, s10, s01, s11, fx, fz, height);
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
    bool allowCoarserParent,
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

    // Missing preferred-ring data is not proof that the terrain is air. Finer
    // resident data is always acceptable. Low-altitude horizon terrain may also
    // hold a coarser resident parent so children do not punch sky holes while
    // streaming; callers keep high-altitude views stricter so broad captures do
    // not regress back into blocky parent slabs.
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

    if (allowCoarserParent) {
        [loop]
        for (uint offsetCoarse = 1u; offsetCoarse < MID_CLIPMAP_MAX_SHADER_RINGS; ++offsetCoarse) {
            const uint coarserRing = clampedPreferred + offsetCoarse;
            if (coarserRing >= ringCount) {
                break;
            }
            if (SampleResidentMidVoxel(worldPos, coarserRing, voxel)) {
                actualRing = coarserRing;
                actualCellSize = MidClipmapRingCellSize(actualRing);
                return true;
            }
        }
    }

    return false;
}

bool ProceduralMidVoxelCellIsAir(float3 worldPos, float distanceFromCamera) {
    // Exact early-outs hoisted ABOVE the heavy analytic height evaluation
    // (FarTerrainHeightVoxelized inlines the full ~11-octave FarTerrainHeight).
    // worldPos.y <= FAR_SEA_LEVEL is always solid: either y <= height, or
    // height < y <= sea which the water rule below classified solid anyway
    // (this also subsumes the old FAR_TERRAIN_MIN_HEIGHT + 2 check, -332 < -48).
    if (worldPos.y <= FAR_SEA_LEVEL) {
        return false;
    }
    // The quantized reshaped height can never exceed FAR_TERRAIN_MAX_HEIGHT
    // (raw clamp 664; reshape floor SEA+56 only raises toward it) plus the max
    // ceil() quantize step max(4, 28*0.75) = 21, so anything above 664+24 is
    // provably air without evaluating the noise.
    if (worldPos.y > FAR_TERRAIN_MAX_HEIGHT + 24.0f) {
        return true;
    }
    float mountainMask, spireMask, ravineMask;
    const float height = FarTerrainHeightVoxelized(
        worldPos.xz,
        distanceFromCamera,
        mountainMask,
        spireMask,
        ravineMask);
    return worldPos.y > height;
}

bool IsMidVoxelAirOrMissing(float3 worldPos, uint ring, float distanceFromCamera) {
    uint neighborVoxel;
    if (!SampleResidentMidVoxel(worldPos, ring, neighborVoxel)) {
        // Missing neighbor bricks are a residency boundary, but the generated
        // terrain function is deterministic. Use it as a one-cell halo for
        // exposure classification only, so valid mid-voxel surfaces do not get
        // mislabeled as interior fallback while adjacent bricks stream in.
        return ProceduralMidVoxelCellIsAir(worldPos, distanceFromCamera);
    }
    return GetMaterial(neighborVoxel) == MAT_AIR;
}

bool IsResidentMidVoxelExposed(float3 worldPos, uint ring, float cellSize, float distanceFromCamera, out float3 normal) {
    normal = float3(0.0f, 0.0f, 0.0f);
    const float3 dx = float3(cellSize, 0.0f, 0.0f);
    const float3 dy = float3(0.0f, cellSize, 0.0f);
    const float3 dz = float3(0.0f, 0.0f, cellSize);

    if (IsMidVoxelAirOrMissing(worldPos + dx, ring, distanceFromCamera)) normal += float3(1.0f, 0.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dx, ring, distanceFromCamera)) normal += float3(-1.0f, 0.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos + dy, ring, distanceFromCamera)) normal += float3(0.0f, 1.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dy, ring, distanceFromCamera)) normal += float3(0.0f, -1.0f, 0.0f);
    if (IsMidVoxelAirOrMissing(worldPos + dz, ring, distanceFromCamera)) normal += float3(0.0f, 0.0f, 1.0f);
    if (IsMidVoxelAirOrMissing(worldPos - dz, ring, distanceFromCamera)) normal += float3(0.0f, 0.0f, -1.0f);

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

bool MidClipmapResidentHeightMaterialForDistance(
    float2 xz,
    float distanceFromCamera,
    out float height,
    out uint material,
    out float cellSize);
void ApplyMidVoxelColumnWaterSurface(inout float height, inout uint material);

bool RaymarchMidVoxelClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit voxelHit) {
    voxelHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    const bool voxelTerrainOnlyMode = (sparseNearFlags & 16u) != 0u;
    if ((sparseNearFlags & 4u) == 0u) {
        return false;
    }
    if (frame.midResidencyParams.y < 0.04f || frame.midResidencyParams.w < 1.0f) {
        return false;
    }
    // Mid-voxel clipmaps are still the only mid-distance layer that preserves
    // voxel structure. Do not drop them during normal scheduler pressure, or
    // public captures collapse back into heightfield/far fallback terrain even
    // while resident voxel bricks are available.
    if (frame.renderBudgetParams.z < 0.30f &&
        BackgroundRenderQuality() < 0.45f &&
        !BackgroundDebugLayerMode()) {
        return false;
    }

    uint4 header = MidVoxelClipmapMetadata[0];
    if (frame.midFieldParams.x < 0.5f || header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u) {
        return false;
    }
    // The resident mid-voxel payload is a coherent low-resolution filled
    // volume. The DDA can participate in normal views again, but it still
    // rejects downward-facing interior/underside hits so the first visible hit
    // is an exposed terrain face, not the bottom of a coarse cell.
    const bool highAltitudeVoxelView = rayOrigin.y > 384.0f;
    const bool walkingMidVoxelDda = (sparseNearFlags & 32u) != 0u;
    const bool midVoxelDdaDiagnosticMode = frame.debugMode == 65u || frame.debugMode == 67u;
    if (!highAltitudeVoxelView && !walkingMidVoxelDda && !midVoxelDdaDiagnosticMode) {
        // Low-altitude public views should not expose the raw 3D mid-voxel
        // shell by default. The height/column mid path and far SVO provide the
        // coherent public LOD while exact surfaces stream in; this shell DDA is
        // still available for explicit walk-DDA experiments and debug modes.
        return false;
    }
    const bool walkingTopSurfaceOnly = !highAltitudeVoxelView && !walkingMidVoxelDda;
    const bool lowAltitudeGrazingSkylineRay =
        voxelTerrainOnlyMode &&
        !highAltitudeVoxelView &&
        rayDir.y > 0.04f &&
        rayDir.y < 0.22f;
    const float minAllowedRayY = highAltitudeVoxelView ? -1.01f : (walkingTopSurfaceOnly ? -0.96f : -0.68f);
    const float maxAllowedRayY = highAltitudeVoxelView ? 0.20f : 0.42f;
    if (rayDir.y > maxAllowedRayY || rayDir.y < minAllowedRayY) {
        return false;
    }
    const float highAltitudeFarQuality = min(frame.renderBudgetParams.z, frame.farOwnershipParams.w);
    const bool highAltitudeFarSvoReady =
        frame.farOwnershipParams.x > 0.5f &&
        frame.farOwnershipParams.y >= 0.999f &&
        frame.farOwnershipParams.z > 0.0f &&
        frame.farFieldParams.x > 0.5f &&
        frame.farFieldParams.y > 0.0f &&
        frame.farFieldParams.z > 0.0f &&
        highAltitudeFarQuality >= 0.35f;
    const bool highAltitudeMidVoxelReadyForHandoff =
        frame.exactNearParams.z >= 0.97f &&
        frame.exactNearParams.w >= 0.94f &&
        frame.midResidencyParams.w >= 1.0f;
    if (highAltitudeVoxelView &&
        highAltitudeFarSvoReady &&
        !highAltitudeMidVoxelReadyForHandoff &&
        rayDir.y <= 0.18f) {
        // Once the far SVO is complete, it is the cleaner high-altitude voxel
        // authority while mid is still catching up. When the screen-relevant
        // mid target set is mature, try resident mid voxels first and keep
        // far-SVO as fallback instead of permanently hiding real voxel terrain.
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);
    int budget = frame.renderBudgetParams.z < 0.55f
        ? ScaleBackgroundStepBudget(48, 36, 24)
        : (frame.renderBudgetParams.z < 0.85f
            ? ScaleBackgroundStepBudget(88, 68, 48)
            : ScaleBackgroundStepBudget(128, 96, 64));
    if (lowAltitudeGrazingSkylineRay) {
        budget = max(budget, ScaleBackgroundStepBudget(176, 132, 104));
    }
    RayHit deferredInteriorFallbackHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    bool hasDeferredInteriorFallbackHit = false;
    float deferredInteriorFallbackEnd = 0.0f;
    bool previousMidVoxelWasAir = false;

    [loop]
    for (int i = 0; i < budget && t < endDistance; ++i) {
        if (hasDeferredInteriorFallbackHit && t >= deferredInteriorFallbackEnd) {
            voxelHit = deferredInteriorFallbackHit;
            return true;
        }
        uint ring = min((uint)floor(saturate((t - startDistance) / max(endDistance - startDistance, 1.0f)) *
            max((float)(header.w >> 24u), 1.0f)), (uint)max((int)(header.w >> 24u) - 1, 0));
        float cellSize = MidClipmapRingCellSize(ring);
        float3 pos = rayOrigin + rayDir * t;
        uint voxel;
        uint actualRing;
        float actualCellSize;
        // ALTITUDE PATCHWORK FIX (complaint 2): high-altitude views may read
        // coarser resident parents too. The fine rings' interest bubbles span
        // only +-9 bricks of the camera in XZ (ring0 +-576u .. ring2 +-2304u),
        // so at altitude most ground inside a fine ring's t-band has no
        // preferred-ring brick; the coarse rings (ring3 +-4608u, ring4 +-9216u)
        // DO cover it and midCov 1.00 keeps them resident. Refusing them
        // painted that ground far-style with scattered fine-brick patches (the
        // art-style patchwork). The angular acceptability test at the hit site
        // below keeps parents from reading as fake block terrain.
        const bool allowCoarserParentFallback =
            highAltitudeVoxelView ||
            (rayDir.y > -0.58f &&
             rayDir.y < 0.18f);
        if (SampleResidentMidVoxelFallback(pos, ring, allowCoarserParentFallback, voxel, actualRing, actualCellSize)) {
            float nextCellT = NextMidVoxelCellBoundaryT(rayOrigin, rayDir, t, actualCellSize);
            uint material = GetMaterial(voxel);
            if (material != MAT_AIR) {
                if (frame.debugMode == 67u) {
                    const float solidRingShade =
                        ((float)actualRing + 1.0f) / max((float)(header.w >> 24u), 1.0f);
                    voxelHit = MakeHit(
                        float4(1.0f, solidRingShade, saturate(actualCellSize / 48.0f), 1.0f),
                        t);
                    return true;
                }
                if (highAltitudeVoxelView && actualRing > ring) {
                    // Accept the coarse parent only while its angular
                    // footprint stays small (cell <= ~0.014 rad at the hit
                    // distance). The bound is chosen so each ring's parent
                    // becomes acceptable before the next-finer ring's interest
                    // bubble runs out (16u cells by ~1143u vs ring1 +-1152u,
                    // 32u by ~2286u vs ring2 +-2304u, 64u by ~4571u vs ring3
                    // +-4608u, and t >= ground rho always), so the ground
                    // reads as ONE mid-style radial resolution gradient
                    // instead of a mid/far patchwork. Chunkier parents still
                    // fall through to the far layers.
                    if (actualCellSize > max(12.0f, t * 0.014f)) {
                        previousMidVoxelWasAir = false;
                        t = min(nextCellT, t + max(actualCellSize, 4.0f));
                        continue;
                    }
                }
                float3 normal;
                const bool taggedMidVoxelSurface = IsResidentMidVoxelTaggedSurface(voxel);
                const bool exposedMidVoxel = IsResidentMidVoxelExposed(pos, actualRing, actualCellSize, t, normal);
                const bool rayEntryMidVoxelSurface = previousMidVoxelWasAir;
                const float exactMidFallbackStart = max(frame.exactNearParams.x, 0.0f);
                const bool allowVoxelOnlyInteriorFallback =
                    voxelTerrainOnlyMode &&
                    rayDir.y < 0.12f &&
                    t >= exactMidFallbackStart;
                if (!exposedMidVoxel && !taggedMidVoxelSurface && !rayEntryMidVoxelSurface && !allowVoxelOnlyInteriorFallback) {
                    previousMidVoxelWasAir = false;
                    t = min(nextCellT, t + max(actualCellSize, 4.0f));
                    continue;
                }
                // The FIRST solid mid-voxel the ray meets after air/missing is the
                // front face of the coarse terrain volume. Its blocky 6-neighbor
                // exposure normal often points sideways (a slope/bank cell whose
                // up-neighbor is also solid), which previously made the
                // normal-rejection below SKIP it -> the ray escaped to sky/water,
                // punching the ragged mid-LOD holes (diagnosed via debugMode 65:
                // the whole mid band shaded orange = side-face hits). A front face
                // is solid land regardless of which way the voxel face points, so
                // give ray-entry hits a sky-facing normal and never reject them.
                const bool frontFaceMidVoxelSurface = rayEntryMidVoxelSurface;
                if (!exposedMidVoxel || (frontFaceMidVoxelSurface && normal.y < 0.05f)) {
                    normal = normalize(float3(-rayDir.x, max(abs(rayDir.y), 0.35f), -rayDir.z));
                }
                const bool interiorFallbackHit =
                    allowVoxelOnlyInteriorFallback && !exposedMidVoxel && !taggedMidVoxelSurface && !rayEntryMidVoxelSurface;
                const float minNormalY = walkingTopSurfaceOnly ? 0.14f : -0.18f;
                const float surfaceMinNormalY =
                    lowAltitudeGrazingSkylineRay && rayEntryMidVoxelSurface ? -0.04f : -0.18f;
                const bool residentMidVoxelSurface =
                    taggedMidVoxelSurface || exposedMidVoxel || rayEntryMidVoxelSurface || allowVoxelOnlyInteriorFallback;
                const bool rejectedMidVoxelNormal = frontFaceMidVoxelSurface
                    ? false
                    : (residentMidVoxelSurface
                        ? normal.y < surfaceMinNormalY
                        : normal.y < minNormalY);
                if (rejectedMidVoxelNormal && !BackgroundDebugLayerMode()) {
                    previousMidVoxelWasAir = false;
                    t = min(nextCellT, t + max(actualCellSize, 4.0f));
                    continue;
                }
                if (material == MAT_SAND && pos.y <= FAR_SEA_LEVEL + 2.0f) {
                    material = MAT_WATER;
                } else if (interiorFallbackHit && material == MAT_SAND) {
                    material = pos.y <= FAR_SEA_LEVEL + 8.0f ? MAT_WATER : MAT_DIRT;
                }
                const bool waterlineInteriorFallback =
                    interiorFallbackHit &&
                    pos.y <= FAR_SEA_LEVEL + 8.0f &&
                    (material == MAT_WATER || material == MAT_SAND);
                if (waterlineInteriorFallback) {
                    previousMidVoxelWasAir = false;
                    t = min(nextCellT, t + max(actualCellSize, 4.0f));
                    continue;
                }
                float hitDistance = t;
                float3 hitPos = pos;
                bool recoveredInteriorSurface = false;
                if (interiorFallbackHit && rayDir.y < -0.001f) {
                    float proceduralLo = 0.0f;
                    float proceduralHi = t;
                    const int proceduralRefineBudget = ScaleBackgroundRefineBudget(2, 1, 1);
                    [loop]
                    for (int refine = 0; refine < proceduralRefineBudget; ++refine) {
                        const float mid = (proceduralLo + proceduralHi) * 0.5f;
                        const float3 midPos = rayOrigin + rayDir * mid;
                        float midMountainMask;
                        float midSpireMask;
                        float midRavineMask;
                        const float midHeight = FarTerrainHeightVoxelized(
                            midPos.xz,
                            max(mid, startDistance),
                            midMountainMask,
                            midSpireMask,
                            midRavineMask);
                        const float midSurfaceY =
                            midHeight < FAR_SEA_LEVEL ? FAR_SEA_LEVEL : midHeight;
                        if (midPos.y > midSurfaceY) {
                            proceduralLo = mid;
                        } else {
                            proceduralHi = mid;
                        }
                    }
                    const float recoveredT = max(proceduralHi, exactMidFallbackStart);
                    if (recoveredT <= t + max(actualCellSize, 16.0f) &&
                        recoveredT >= exactMidFallbackStart) {
                        hitDistance = recoveredT;
                        hitPos = rayOrigin + rayDir * hitDistance;
                        normal = float3(0.0f, 1.0f, 0.0f);
                        recoveredInteriorSurface = true;
                    }
                }
                float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
                if (frame.debugMode == 54u || frame.debugMode == 56u) {
                    voxelHit = MakeHit(float4(DebugMaterialColor(material), 1.0f), hitDistance);
                    return true;
                }
                if (frame.debugMode == 59u) {
                    const float ringShade = (float)(actualRing + 1u) / max((float)(header.w >> 24u), 1.0f);
                    voxelHit = MakeHit(float4(ringShade, 1.0f - ringShade * 0.62f, saturate(actualCellSize / 96.0f), 1.0f), hitDistance);
                    return true;
                }
                if (frame.debugMode == 63u) {
                    voxelHit = MakeHit(float4(saturate(normal * 0.5f + 0.5f), 1.0f), hitDistance);
                    return true;
                }
                if (frame.debugMode == 65u) {
                    const bool parentHeld = actualRing > ring;
                    const bool unresolvedInterior = interiorFallbackHit && !recoveredInteriorSurface;
                    float3 debugFaceColor = float3(1.0f, 0.48f, 0.05f);
                    if (normal.y > 0.62f) {
                        debugFaceColor = float3(0.05f, 0.95f, 0.18f);
                    } else if (normal.y < -0.35f) {
                        debugFaceColor = float3(0.95f, 0.05f, 0.95f);
                    }
                    if (parentHeld) {
                        debugFaceColor = float3(0.86f, 0.18f, 1.0f);
                    }
                    if (unresolvedInterior) {
                        debugFaceColor = float3(1.0f, 0.08f, 0.02f);
                    }
                    voxelHit = MakeHit(float4(debugFaceColor, 1.0f), hitDistance);
                    return true;
                }
                if (frame.debugMode == 66u) {
                    const float distanceBand = saturate(hitDistance / 6400.0f);
                    voxelHit = MakeHit(float4(distanceBand, distanceBand, distanceBand, 1.0f), hitDistance);
                    return true;
                }
                if (frame.debugMode == 62u) {
                    voxelHit = MakeHit(float4(DebugClosureColor(hitPos), 1.0f), hitDistance);
                    return true;
                }
                const bool ring0MidVoxel =
                    actualRing == 0u &&
                    actualCellSize <= 12.5f;
                baseColor.rgb = BackgroundTerrainMaterialVariation(
                    baseColor.rgb,
                    material,
                    hitPos,
                    normal,
                    hitDistance,
                    ring0MidVoxel ? 0.52f : 0.72f);
                baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, normal.y, 0.78f);
                if (material == MAT_STONE || material == MAT_DIRT || material == MAT_SAND) {
                    baseColor.rgb = PerVoxelColorJitter(baseColor.rgb, hitPos, hitDistance);
                }
#ifdef RAYMARCH_MID_ONLY
                // ANALYTIC GRADIENT NORMAL (mid-only pass): central-difference the terrain
                // height field instead of the blocky 6-neighbor voxel-face normal, so the
                // terraced coarse LOD shades as real mountain slopes. Epsilon scales with the
                // LOD cell -> low-pass filters height noise to the geometry frequency (no
                // shimmer). smoothstep geomorph keeps crisp voxel normals near, smooth far.
                // (Lives only in the mid-only PSO; the 4 FarTerrainHeight inlines are too heavy
                // for the uber-shader's PSO to compile, which is the whole reason for the split.)
                float gradE = max(actualCellSize * 1.5f, 10.0f);
                float gmmN, gsmN, grmN;
                float ghL = FarTerrainHeight(hitPos.xz - float2(gradE, 0.0f), gmmN, gsmN, grmN);
                float ghR = FarTerrainHeight(hitPos.xz + float2(gradE, 0.0f), gmmN, gsmN, grmN);
                float ghD = FarTerrainHeight(hitPos.xz - float2(0.0f, gradE), gmmN, gsmN, grmN);
                float ghU = FarTerrainHeight(hitPos.xz + float2(0.0f, gradE), gmmN, gsmN, grmN);
                float3 gradNormal = normalize(float3(ghL - ghR, 2.0f * gradE, ghD - ghU));
                // TANDEM crispness fix: the old 512-1400 ramp smoothed the mid
                // terrain's crisp voxel-block faces into soft slopes by ~1.4k,
                // which read as "fuzzy" vs the crisp near surface (the user wants
                // mid to match the gorgeous near). Push the smoothing out to
                // 2500-6500 so the visible mid keeps its block-face normals (crisp,
                // like near); only the genuine far gradient-smooths (where blocks
                // would alias/shimmer anyway).
                float midSmoothBlend = smoothstep(2500.0f, 6500.0f, hitDistance);
                float3 shadeNormal = normalize(lerp(normal, gradNormal, midSmoothBlend));
                float ndotl = max(dot(shadeNormal, SkySunDirection()), 0.0f);
                float3 color = baseColor.rgb * (SkyAmbient(shadeNormal) * 0.42f + ndotl * 0.82f + 0.06f);
#else
                float3 shadeNormal = DistantLodShadeNormal(
                    normal,
                    hitDistance,
                    ring0MidVoxel ? 0.34f : 0.18f);
                float ndotl = saturate(dot(shadeNormal, SkySunDirection()) * 0.62f + 0.28f);
                float3 color = baseColor.rgb * (SkyAmbient(shadeNormal) * 0.76f + ndotl * 0.34f);
#endif
                const float nearMidContext = 1.0f - saturate((hitDistance - startDistance) / 620.0f);
                const float3 contextLift = lerp(SkyColor(rayDir), float3(0.48f, 0.52f, 0.48f), 0.35f);
#ifndef RAYMARCH_MID_ONLY
                color = lerp(color, contextLift, nearMidContext * 0.08f);
#endif
                if (interiorFallbackHit && !recoveredInteriorSurface) {
#ifdef RAYMARCH_MID_ONLY
                    // Mid pass: the analytic gradient normal gives interior-fallback (coarse,
                    // surface-unresolved) cells a real surface, so skip the grey "continuity
                    // tint" crutch that made distant terrain read as grey vertical smears. Only
                    // keep the water tint; land cells render with true material color + gradient.
                    if (material == MAT_WATER) {
                        color = lerp(float3(0.12f, 0.32f, 0.36f), color, 0.45f);
                    }
#else
                    const float fallbackConfidence = 1.0f - saturate((actualCellSize - 4.0f) / 28.0f);
                    const float3 continuityTint = material == MAT_WATER
                        ? float3(0.12f, 0.32f, 0.36f)
                        : lerp(float3(0.38f, 0.43f, 0.36f), SkyColor(rayDir), 0.22f);
                    color = lerp(continuityTint, color, fallbackConfidence * 0.55f);
#endif
                }
                color = max(color, baseColor.rgb * 0.54f + 0.045f);
                float fogFactor = saturate((hitDistance - startDistance) / max(endDistance - startDistance, 1.0f));
                if (frame.debugMode == 60u) {
                    voxelHit = MakeHit(float4(float3(fogFactor, fogFactor, fogFactor), 1.0f), hitDistance);
                    return true;
                }
#ifdef RAYMARCH_MID_ONLY
                color = lerp(color, SkyColor(rayDir), fogFactor * 0.12f);
#else
                color = lerp(color, SkyColor(rayDir), fogFactor * 0.34f);
#endif
                if (frame.debugMode == 9u) {
                    color = lerp(color, float3(1.0f, 0.58f, 0.10f), 0.52f);
                }
                voxelHit = MakeHit(float4(color, baseColor.a), hitDistance);
                if (actualRing > ring) {
                    voxelHit.diagnosticFlags |= RAY_DIAGNOSTIC_MID_PARENT_HELD;
                }
                if (interiorFallbackHit && !recoveredInteriorSurface) {
                    voxelHit.diagnosticFlags |= RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK;
                    if (!hasDeferredInteriorFallbackHit) {
                        deferredInteriorFallbackHit = voxelHit;
                        hasDeferredInteriorFallbackHit = true;
                        deferredInteriorFallbackEnd =
                            t + min(max(actualCellSize * 10.0f, 96.0f), 384.0f);
                    }
                    previousMidVoxelWasAir = false;
                    t = min(nextCellT, t + max(actualCellSize, 4.0f));
                    continue;
                }
                return true;
            }
            previousMidVoxelWasAir = true;
            t = min(nextCellT, t + max(actualCellSize, 4.0f));
        } else {
            previousMidVoxelWasAir = lowAltitudeGrazingSkylineRay
                ? true
                : ProceduralMidVoxelCellIsAir(pos, t);
            t += max(cellSize * BackgroundMissingSampleSkipScale(), 12.0f);
        }
    }

    if (hasDeferredInteriorFallbackHit) {
        voxelHit = deferredInteriorFallbackHit;
        return true;
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
        return normalize(float3((hL - hR) * 0.70f, offset * 2.8f, (hD - hU) * 0.70f));
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

RayHit MakeMidVoxelColumnClipmapHit(float3 rayOrigin, float3 rayDir, float hitT, uint material, float cellSize) {
    float3 hitPos = rayOrigin + rayDir * hitT;
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
    if (frame.debugMode == 54u || frame.debugMode == 56u) {
        return MakeHit(float4(DebugMaterialColor(material), 1.0f), hitT);
    }
    baseColor.rgb = BackgroundTerrainMaterialVariation(
        baseColor.rgb,
        material,
        hitPos,
        normal,
        hitT,
        0.64f);
    baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, normal.y, 0.62f);
    float3 shadeNormal = DistantLodShadeNormal(normal, hitT, 0.16f);
    float ndotl = saturate(dot(shadeNormal, SkySunDirection()) * 0.58f + 0.40f);
    float3 color = baseColor.rgb * (SkyAmbient(shadeNormal) * 0.92f + ndotl * 0.46f);
    const float grid = VoxelGridLine(hitPos.xz, max(cellSize, 4.0f), 0.020f);
    color *= lerp(1.0f, 0.94f, grid);
    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    const float fogFactor = saturate((hitT - startDistance) / max(endDistance - startDistance, 1.0f));
    color = lerp(color, SkyColor(rayDir), fogFactor * 0.28f);
    if (frame.debugMode == 9u) {
        color = lerp(color, float3(1.0f, 0.58f, 0.10f), 0.52f);
    }
    RayHit hit = MakeHit(float4(color, baseColor.a), hitT);
    hit.diagnosticFlags |= RAY_DIAGNOSTIC_MID_COLUMN;
    return hit;
}

void ApplyMidVoxelColumnWaterSurface(inout float height, inout uint material) {
    if (material == MAT_WATER || height < FAR_SEA_LEVEL) {
        height = FAR_SEA_LEVEL;
        material = MAT_WATER;
    }
}

bool RaymarchMidVoxelColumnClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit columnHit) {
    columnHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (frame.midFieldParams.x < 0.5f || frame.renderBudgetParams.z < 0.20f) {
        return false;
    }
    if (frame.midResidencyParams.x < 0.04f || frame.midResidencyParams.z < 1.0f) {
        return false;
    }

    // This is the safe mid-distance terrain context for walking views: it
    // draws block-stepped top surfaces instead of the full shell volume.
    if (rayDir.y > 0.06f) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    float t = max(startDist, startDistance);
    if (rayDir.y < -0.001f) {
        float3 firstPos = rayOrigin + rayDir * t;
        uint firstMaterial;
        float firstHeight;
        float firstCellSize;
        bool firstResident = MidClipmapResidentHeightMaterialForDistance(
            firstPos.xz,
            t,
            firstHeight,
            firstMaterial,
            firstCellSize);
        firstCellSize = max(firstCellSize, 4.0f);
        firstHeight = QuantizeTerrainHeight(firstHeight, firstCellSize);
        ApplyMidVoxelColumnWaterSurface(firstHeight, firstMaterial);
        if (firstResident && firstPos.y <= firstHeight) {
            uint originMaterial;
            float originHeight;
            float originCellSize;
            bool originResident = MidClipmapResidentHeightMaterialForDistance(
                rayOrigin.xz,
                startDistance,
                originHeight,
                originMaterial,
                originCellSize);
            originHeight = QuantizeTerrainHeight(originHeight, max(originCellSize, 4.0f));
            ApplyMidVoxelColumnWaterSurface(originHeight, originMaterial);
            if (originResident && rayOrigin.y > originHeight + 1.0f) {
                float lo = 0.0f;
                float hi = t;
                uint material = firstMaterial;
                float cellSize = firstCellSize;
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
                        max(mid, startDistance),
                        midHeight,
                        midMaterial,
                        midCellSize);
                    midCellSize = max(midCellSize, 4.0f);
                    midHeight = QuantizeTerrainHeight(midHeight, midCellSize);
                    ApplyMidVoxelColumnWaterSurface(midHeight, midMaterial);
                    if (!midResident || midPos.y > midHeight) {
                        lo = mid;
                    } else {
                        hi = mid;
                        material = midMaterial;
                        cellSize = midCellSize;
                    }
                }
                columnHit = MakeMidVoxelColumnClipmapHit(rayOrigin, rayDir, max(hi, startDist), material, cellSize);
                return true;
            }
        }
    }
    int stepBudget = frame.renderBudgetParams.z < 0.55f
        ? ScaleBackgroundStepBudget(44, 32, 20)
        : (frame.renderBudgetParams.z < 0.85f
            ? ScaleBackgroundStepBudget(76, 58, 40)
            : ScaleBackgroundStepBudget(104, 78, 54));

    [loop]
    for (int i = 0; i < stepBudget && t < endDistance; ++i) {
        float3 pos = rayOrigin + rayDir * t;
        uint material;
        float height;
        float sampleCellSize;
        if (!MidClipmapResidentHeightMaterialForDistance(pos.xz, t, height, material, sampleCellSize)) {
            t += max(MidClipmapCellSize(t) * BackgroundMissingSampleSkipScale(), 12.0f);
            continue;
        }

        const float cellSize = max(sampleCellSize, 4.0f);
        height = QuantizeTerrainHeight(height, cellSize);
        ApplyMidVoxelColumnWaterSurface(height, material);
        const float2 cell = floor(pos.xz / cellSize);
        float nextT = endDistance;
        if (abs(rayDir.x) > 0.0001f) {
            const float boundaryX = ((rayDir.x > 0.0f) ? (cell.x + 1.0f) : cell.x) * cellSize;
            const float tx = (boundaryX - rayOrigin.x) / rayDir.x;
            if (tx > t + 0.01f) {
                nextT = min(nextT, tx);
            }
        }
        if (abs(rayDir.z) > 0.0001f) {
            const float boundaryZ = ((rayDir.z > 0.0f) ? (cell.y + 1.0f) : cell.y) * cellSize;
            const float tz = (boundaryZ - rayOrigin.z) / rayDir.z;
            if (tz > t + 0.01f) {
                nextT = min(nextT, tz);
            }
        }
        if (nextT >= endDistance - 0.001f) {
            nextT = min(endDistance, t + max(cellSize * 2.0f, 16.0f));
        }

        float hitT = 1e20f;
        if (rayDir.y < -0.001f) {
            const float topT = (height - rayOrigin.y) / rayDir.y;
            if (topT >= t - 0.02f && topT <= nextT + 0.02f) {
                hitT = max(topT, t);
            }
        }
        if (hitT < 1e19f) {
            columnHit = MakeMidVoxelColumnClipmapHit(rayOrigin, rayDir, hitT, material, cellSize);
            return true;
        }

        t = max(nextT + 0.02f, t + 0.05f);
    }

    return false;
}

bool RaymarchMidClipmap(float3 rayOrigin, float3 rayDir, float startDist, out RayHit midHit) {
    midHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    // Legacy scalar mid-height fallback is not part of the sparse-brick public
    // render contract. Keeping the full fallback compiled into PS_Raymarch made
    // small voxel-path edits push the shader over the PSO creation limit, while
    // the active sparse-only path uses RaymarchMidVoxelClipmap instead.
    return false;
#if 0
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
    if (rayDir.y > 0.04f) {
        return false;
    }

    if (rayDir.y < -0.72f) {
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
    ApplyMidVoxelColumnWaterSurface(previousHeight, previousMaterial);
    float previousSigned = previousPos.y - previousHeight;
    if (previousResident && previousSigned <= 0.0f) {
        uint originMaterial;
        float originHeight;
        float originCellSize;
        bool originResident = MidClipmapResidentHeightMaterialForDistance(
            rayOrigin.xz,
            startDistance,
            originHeight,
            originMaterial,
            originCellSize);
        originHeight = QuantizeTerrainHeight(originHeight, originCellSize);
        ApplyMidVoxelColumnWaterSurface(originHeight, originMaterial);
        if (!originResident || rayOrigin.y <= originHeight + 1.0f) {
            return false;
        }

        float lo = 0.0f;
        float hi = t;
        uint material = previousMaterial;
        float height = previousHeight;
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
                max(mid, startDistance),
                midHeight,
                midMaterial,
                midCellSize);
            midHeight = QuantizeTerrainHeight(midHeight, midCellSize);
            ApplyMidVoxelColumnWaterSurface(midHeight, midMaterial);
            if (!midResident || midPos.y > midHeight) {
                lo = mid;
            } else {
                hi = mid;
                height = midHeight;
                material = midMaterial;
            }
        }

        float hitT = max(hi, startDist);
        float3 hitPos = rayOrigin + rayDir * hitT;
        float3 normal = MidClipmapNormal(hitPos.xz, hitT);
        float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
        baseColor.rgb = BackgroundTerrainMaterialVariation(
            baseColor.rgb,
            material,
            hitPos,
            normal,
            hitT,
            0.64f);
        baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, normal.y, 0.62f);
        float3 lightDir = SkySunDirection();
        // Smooth-shade the voxel staircase (bias lighting normal toward macro-up so step
        // risers don't read as contour bands); keep some true-normal slope definition.
        float3 litNormal = normalize(normal + float3(0.0f, 1.0f, 0.0f) * 1.35f);
        float ndotl = saturate(dot(litNormal, lightDir));
        float3 color = baseColor.rgb * (SkyAmbient(litNormal) * 0.82f + ndotl * 0.60f);
        const float midGrid = VoxelGridLine(hitPos.xz, max(MidClipmapCellSize(hitT), 4.0f), 0.020f);
        color *= lerp(1.0f, 0.985f, midGrid);
        float fogFactor = saturate((hitT - startDistance) / max(endDistance - startDistance, 1.0f));
        color = lerp(color, SkyColor(rayDir), fogFactor * 0.24f);
        if (frame.debugMode == 8u) {
            color = lerp(color, float3(0.15f, 0.75f, 1.0f), 0.45f);
        }

        midHit = MakeHit(float4(color, 1.0f), hitT);
        return true;
    }

    int stepBudget = frame.renderBudgetParams.z < 0.55f
        ? ScaleBackgroundStepBudget(24, 18, 12)
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
        ApplyMidVoxelColumnWaterSurface(height, material);
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
                ApplyMidVoxelColumnWaterSurface(midHeight, midMaterial);
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
            baseColor.rgb = BackgroundTerrainMaterialVariation(
                baseColor.rgb,
                material,
                hitPos,
                normal,
                hitT,
                0.64f);
            baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, normal.y, 0.62f);

            float3 lightDir = SkySunDirection();
            float3 litNormal = normalize(normal + float3(0.0f, 1.0f, 0.0f) * 1.35f);
            float ndotl = saturate(dot(litNormal, lightDir));
            float3 color = baseColor.rgb * (SkyAmbient(litNormal) * 0.82f + ndotl * 0.60f);
            const float midGrid = VoxelGridLine(hitPos.xz, max(MidClipmapCellSize(hitT), 4.0f), 0.020f);
            color *= lerp(1.0f, 0.985f, midGrid);
            float fogFactor = saturate((hitT - startDistance) / max(endDistance - startDistance, 1.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.24f);

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
#endif
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
    inout float cachedOriginHeight,
    out float hitT,
    out float3 hitNormal,
    out uint hitMaterial);

// Lazily computes (once per ray) the reshaped terrain height under rayOrigin.
// rayOrigin is constant across every leaf a ray visits, but both leaf-surface
// resolvers used to re-run the full FarTerrainHeight noise PER LEAF for it.
// Sentinel -1e30 = not yet computed (real heights are clamped to >= -332).
float CachedFarSvoOriginHeight(float3 rayOrigin, inout float cachedOriginHeight) {
    if (cachedOriginHeight <= -1e29f) {
        float originMountainMask, originSpireMask, originRavineMask;
        const float rawOriginHeight = FarTerrainHeight(
            rayOrigin.xz,
            originMountainMask,
            originSpireMask,
            originRavineMask);
        cachedOriginHeight = FarSpawnLandReshapeHeight(
            rayOrigin.xz,
            rawOriginHeight,
            originMountainMask);
    }
    return cachedOriginHeight;
}

bool FarSvoInteriorLeafSurfaceRecovery(
    float3 rayOrigin,
    float3 rayDir,
    float leafT0,
    inout float cachedOriginHeight,
    out float hitT,
    out float3 hitNormal,
    out uint hitMaterial)
{
    hitT = 1e20f;
    hitNormal = float3(0.0f, 1.0f, 0.0f);
    hitMaterial = MAT_STONE;

    const float t = max(leafT0, 0.0f);
    const float3 pos = rayOrigin + rayDir * t;
    // Spawn-land agreement: the rebuilt SVO cache bakes the RESHAPED world, so
    // leaf recovery must bisect the reshaped height. Against the raw height the
    // reshaped-land leaves (floor ~SEA+56) never contain a surface crossing and
    // every aerial ray over spawn land fell through to water/sky (the navy
    // flooded band and the pale miss halo).
    const float spawnBand = FarSpawnLandBand(pos.xz);
    // Per-leaf noise-floor base (broad/detail); the ridge term rides the
    // per-sample mountainMask inside FarSpawnLandApplyFloor. UNCONDITIONAL on
    // purpose (driver-JIT safety, see FarSpawnLandReshapeHeight): when band is
    // 0 the ApplyFloor lerp ignores the base, so gating it behind
    // 'spawnBand > 0' only added a select to deep control flow.
    const float spawnFloorBase = FarSpawnLandFloorBase(pos.xz);
    float mountainMask, spireMask, ravineMask;
    const float rawLeafHeight = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
    float height = FarSpawnLandApplyFloor(rawLeafHeight, spawnBand, spawnFloorBase, mountainMask);
    if (pos.y > height) {
        return false;
    }

    const float originHeight = CachedFarSvoOriginHeight(rayOrigin, cachedOriginHeight);
    if (rayOrigin.y <= originHeight) {
        return false;
    }

    float lo = 0.0f;
    float hi = t;
    const bool highAltitudeFarLeaf = rayOrigin.y > 384.0f;
    const int refineBudget = highAltitudeFarLeaf
        ? ScaleFarFieldRefineBudget(6, 5, 4)
        : ScaleFarFieldRefineBudget(6, 4, 3);
    [loop]
    for (int refine = 0; refine < refineBudget; ++refine) {
        const float mid = (lo + hi) * 0.5f;
        const float3 midPos = rayOrigin + rayDir * mid;
        float mm, sm, rm;
        const float rawMidHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
        const float midHeight = FarSpawnLandApplyFloor(rawMidHeight, spawnBand, spawnFloorBase, mm);
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

    hitT = max(hi, 64.0f);
    const float3 hitPos = rayOrigin + rayDir * hitT;
    // NOTE (altitude patchwork): do NOT quantize the xz fed to
    // FarTerrainNormal/FarTerrainMaterial here. Both a FarFallbackCellSize
    // cell-center and a fully branchless clamp+floor quantizer retriggered the
    // nvlddmkm shader-program-header device loss at frame ~61 (the same
    // driver-JIT bug documented at FarSpawnLandReshapeHeight) - these deeply
    // inlined leaf functions tolerate no restructuring. The per-pixel
    // normal/material aliasing is instead tamed at the far-SVO SHADE site
    // (distance-adaptive DistantLodShadeNormal strength), which the driver
    // accepts.
    hitNormal = FarTerrainNormal(hitPos.xz);
    hitMaterial = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
    return true;
}

bool TraverseFarVoxelPage(
    float3 rayOrigin,
    float3 rayDir,
    uint rootNode,
    float3 pageMin,
    float pageSize,
    float startDist,
    inout float cachedOriginHeight,
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
            if (node.material == MAT_AIR) {
                continue;
            }
            const bool interiorLeaf = (node.flags & 2u) != 0u;
            if (interiorLeaf) {
                // Interior leaves are conservative solid volume, not drawable
                // AABB geometry. They still prove that this ray has reached
                // deterministic terrain. Recover the true heightfield crossing
                // before/current leaf instead of leaving a hole until water or a
                // later coarse page happens to own the pixel.
                float candidateT;
                float3 candidateNormal;
                uint candidateMaterial;
                const float recoveryStartT = max(tNear, startDist);
                if (recoveryStartT <= min(tFar, nearestT) &&
                    FarSvoInteriorLeafSurfaceRecovery(
                    rayOrigin,
                    rayDir,
                    recoveryStartT,
                    cachedOriginHeight,
                    candidateT,
                    candidateNormal,
                    candidateMaterial) &&
                    candidateT < nearestT) {
                    nearestT = candidateT;
                    nearestNormal = candidateNormal;
                    nearestMaterial = candidateMaterial;
                    hit = true;
                }
            } else if (nodeSize <= 40.0f && max(tNear, startDist) > 6200.0f) {
                float candidateT = max(tNear, startDist);
                if (candidateT < nearestT) {
                    const float3 candidatePos = rayOrigin + rayDir * candidateT;
                    uint candidateMaterial = node.material;
                    if (candidateMaterial == MAT_WATER &&
                        candidatePos.y > FAR_SEA_LEVEL + 1.0f) {
                        candidateMaterial = candidatePos.y < FAR_SEA_LEVEL + 96.0f
                            ? MAT_SAND
                            : MAT_DIRT;
                    }
                    nearestT = candidateT;
                    nearestNormal = boxNormal;
                    nearestMaterial = candidateMaterial;
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
                    cachedOriginHeight,
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
        uint childNodes[8];
        float3 childMins[8];
        float childNear[8];
        int childCount = 0;

        [unroll]
        for (uint child = 0; child < 8; ++child) {
            if ((node.childMask & (1u << child)) == 0u) {
                continue;
            }

            float3 childMin = nodeMin + float3(
                (child & 1u) ? childSize : 0.0f,
                (child & 2u) ? childSize : 0.0f,
                (child & 4u) ? childSize : 0.0f);

            float childTNear, childTFar;
            if (!IntersectBox(rayOrigin, rayDir, childMin, childMin + childSize, childTNear, childTFar) ||
                childTFar < startDist ||
                childTNear > nearestT) {
                continue;
            }

            int insertAt = childCount;
            [loop]
            while (insertAt > 0 && childTNear < childNear[insertAt - 1]) {
                childNodes[insertAt] = childNodes[insertAt - 1];
                childMins[insertAt] = childMins[insertAt - 1];
                childNear[insertAt] = childNear[insertAt - 1];
                insertAt--;
            }

            childNodes[insertAt] = FarVoxelChildNodeIndex(node.childBase, node.childMask, child);
            childMins[insertAt] = childMin;
            childNear[insertAt] = childTNear;
            childCount++;
        }

        [loop]
        for (int childIndex = childCount - 1; childIndex >= 0 && stackCount < 64; --childIndex) {
            nodeStack[stackCount] = childNodes[childIndex];
            minStack[stackCount] = childMins[childIndex];
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
    inout float cachedOriginHeight,
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
    // Spawn-land agreement: the rebuilt SVO cache bakes the RESHAPED world, so
    // a leaf is accepted where the ray crosses the RESHAPED far surface. Against
    // the raw height, spawn-band land leaves (floor ~SEA+56 over the old
    // below-sea basin) contained no crossing, so the whole reshaped landmass
    // fell through to water/sky in aerial views (navy band + miss halo).
    const float spawnBand = FarSpawnLandBand(pos.xz);
    // Per-leaf noise-floor base (broad/detail); the ridge term rides the
    // per-sample mountainMask inside FarSpawnLandApplyFloor. UNCONDITIONAL on
    // purpose (driver-JIT safety, see FarSpawnLandReshapeHeight).
    const float spawnFloorBase = FarSpawnLandFloorBase(pos.xz);
    float mountainMask, spireMask, ravineMask;
    const float rawEntryHeight = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
    float height = FarSpawnLandApplyFloor(rawEntryHeight, spawnBand, spawnFloorBase, mountainMask);
    float previousSigned = pos.y - height;
    float previousT = t;
    if (previousSigned <= 0.0f) {
        const float originHeight = CachedFarSvoOriginHeight(rayOrigin, cachedOriginHeight);
        if (rayOrigin.y > originHeight) {
            float lo = 0.0f;
            float hi = t;
            const bool highAltitudeFarLeaf = rayOrigin.y > 384.0f;
            const int refineBudget = highAltitudeFarLeaf
                ? ScaleFarFieldRefineBudget(6, 5, 4)
                : ScaleFarFieldRefineBudget(6, 4, 3);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                const float mid = (lo + hi) * 0.5f;
                const float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                const float rawMidHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
                const float midHeight = FarSpawnLandApplyFloor(rawMidHeight, spawnBand, spawnFloorBase, mm);
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

            hitT = max(hi, 64.0f);
            const float3 hitPos = rayOrigin + rayDir * hitT;
            // NOTE: no xz quantization here - driver-JIT fragile, see
            // FarSvoInteriorLeafSurfaceRecovery.
            hitNormal = FarTerrainNormal(hitPos.xz);
            hitMaterial = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            return true;
        }

    }

    // A raw SVO leaf AABB is only a conservative container. Drawing the AABB
    // entry face creates the giant rectangular sheets seen in captures. Accept a
    // leaf only where the ray actually crosses the far terrain surface.
    const bool highAltitudeFarLeaf = rayOrigin.y > 384.0f;
    const float farLeafQuality = FarFieldRenderQuality();
    const float leafStepScale = highAltitudeFarLeaf
        ? (farLeafQuality < 0.62f ? 0.48f : 0.40f)
        : (farLeafQuality < 0.62f ? 0.85f : 0.55f);
    const float stepSize = clamp(
        nodeSize * leafStepScale,
        16.0f,
        highAltitudeFarLeaf ? (farLeafQuality < 0.62f ? 84.0f : 72.0f) : (farLeafQuality < 0.62f ? 128.0f : 96.0f));
    const int sampleBudget = highAltitudeFarLeaf
        ? ScaleFarFieldStepBudget(18, 14, 10)
        : ScaleFarFieldStepBudget(8, 5, 3);
    [loop]
    for (int sample = 0; sample < sampleBudget && t < leafT1; ++sample) {
        t = min(t + stepSize, leafT1);
        pos = rayOrigin + rayDir * t;
        const float rawSampleHeight =
            FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        height = FarSpawnLandApplyFloor(rawSampleHeight, spawnBand, spawnFloorBase, mountainMask);
        const float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = previousT;
            float hi = t;
            const int refineBudget = highAltitudeFarLeaf
                ? ScaleFarFieldRefineBudget(5, 4, 3)
                : ScaleFarFieldRefineBudget(5, 4, 2);
            [loop]
            for (int refine = 0; refine < refineBudget; ++refine) {
                const float mid = (lo + hi) * 0.5f;
                const float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                const float rawRefineHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
                const float midHeight = FarSpawnLandApplyFloor(rawRefineHeight, spawnBand, spawnFloorBase, mm);
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
            // NOTE: no xz quantization here - driver-JIT fragile, see
            // FarSvoInteriorLeafSurfaceRecovery.
            hitNormal = FarTerrainNormal(hitPos.xz);
            hitMaterial = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            return true;
        }

        previousSigned = signedDistance;
        previousT = t;
    }

    return false;
}

// traversalCap: callers that can only ACCEPT a hit nearer than some bound pass
// it here so the page DDA and octree traversal prune everything beyond it (it
// simply seeds the same nearestT pruning the traversal already applies after
// its first recorded hit). Any hit nearer than the cap is found identically;
// callers without a bound pass 1e20f for the pre-existing behavior.
bool RaymarchSparseFarField(float3 rayOrigin, float3 rayDir, float startDist, float traversalCap, out RayHit farHit) {
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
    const bool steepDownCoverageRay = rayDir.y < -0.88f;
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
    const bool lowAltitudeFarSvoView = rayOrigin.y <= 384.0f;
    // High/free-camera views still see upward mountain-silhouette rays near
    // the top of the frame. Rejecting those rays here creates true no-owner
    // holes in voxel-only mode because the procedural height fallback is
    // intentionally disabled. Let the SVO own the same silhouette cone as
    // walking views; empty rays still return false through traversal.
    const float upwardFarSvoLimit = 0.42f;
    if (rayDir.y > upwardFarSvoLimit) {
        return false;
    }

    const uint pageCount = (uint)frame.farFieldParams.y;
    const int pageRadius = (int)frame.farFieldGridParams.x;
    const int pageSide = (int)frame.farFieldGridParams.y;
    float pageSize = max(frame.farFieldParams.w, 1.0f);
    if (pageRadius <= 0 || pageSide <= 0) {
        return false;
    }

    const bool highAltitudeFarSvoView = rayOrigin.y > 384.0f;

    // The first SVO integration scanned every page for every far-field pixel.
    // That is correct but far too expensive once the page forest grows. This
    // top-level 2D DDA only probes the page cells crossed by the ray in X/Z,
    // then traverses the octree for those candidate pages.
    const float farMaxDist = 10400.0f;
    float t = max(startDist, 32.0f);
    float nearestT = traversalCap;
    bool anySvoHit = false;
    float cachedOriginHeight = -1e30f;
    float3 nearestNormal = float3(0, 1, 0);
    uint nearestMaterial = MAT_STONE;

    float2 rayXZ = rayDir.xz;
    float2 originXZ = rayOrigin.xz;
    const bool lowAltitudeHorizonCoverageRay =
        !highAltitudeFarSvoView &&
        rayDir.y > -0.18f &&
        rayDir.y <= 0.08f;
    const bool lowAltitudeMountainSilhouetteRay =
        !highAltitudeFarSvoView &&
        rayDir.y > 0.08f &&
        rayDir.y <= upwardFarSvoLimit;
    int maxPageSteps = 0;
    if (highAltitudeFarSvoView) {
        maxPageSteps = farQuality < 0.55f
            ? ScaleFarFieldStepBudget(28, 22, 16)
            : (farQuality < 0.72f
                ? ScaleFarFieldStepBudget(30, 24, 18)
                : (farQuality < 0.85f
                    ? ScaleFarFieldStepBudget(32, 26, 20)
                    : (farQuality < 0.95f
                        ? ScaleFarFieldStepBudget(34, 28, 22)
                        : ScaleFarFieldStepBudget(36, 30, 24))));
    } else {
        maxPageSteps = farQuality < 0.55f
            ? (lowAltitudeMountainSilhouetteRay
                ? ScaleFarFieldStepBudget(28, 22, 16)
                : (lowAltitudeHorizonCoverageRay
                    ? ScaleFarFieldStepBudget(20, 16, 12)
                    : ScaleFarFieldStepBudget(6, 5, 4)))
            : (farQuality < 0.72f
                ? (lowAltitudeMountainSilhouetteRay
                    ? ScaleFarFieldStepBudget(20, 16, 12)
                    : (lowAltitudeHorizonCoverageRay
                        ? ScaleFarFieldStepBudget(16, 13, 10)
                        : ScaleFarFieldStepBudget(8, 6, 5)))
                : (farQuality < 0.85f
                    ? (lowAltitudeMountainSilhouetteRay
                        ? ScaleFarFieldStepBudget(22, 18, 14)
                        : (lowAltitudeHorizonCoverageRay
                            ? ScaleFarFieldStepBudget(18, 15, 12)
                            : ScaleFarFieldStepBudget(10, 8, 6)))
                    : (farQuality < 0.95f
                        ? ScaleFarFieldStepBudget(16, 12, 8)
                        : ScaleFarFieldStepBudget(24, 18, 12))));
    }

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
                    if (TraverseFarVoxelPage(
                        rayOrigin,
                        rayDir,
                        page.rootNode,
                        pageMin,
                        pageSize,
                        startDist,
                        cachedOriginHeight,
                        nearestT,
                        nearestNormal,
                        nearestMaterial)) {
                        anySvoHit = true;
                    }
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
        if (nextT >= 1e19f && abs(rayXZ.x) <= 0.0001f && abs(rayXZ.y) <= 0.0001f) {
            break;
        }
        if (nextT <= t + 0.5f || nextT >= 1e19f) {
            t += max(64.0f, pageSize * 0.125f);
        } else {
            t = nextT + 0.5f;
        }
    }
    if (!anySvoHit) {
        return false;
    }

    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (nearestMaterial + 0.5f) / 256.0f, 0);
    float3 hitPos = rayOrigin + rayDir * nearestT;
    if (!highAltitudeFarSvoView && nearestT < 2600.0f) {
        float rawMountainMask, rawSpireMask, rawRavineMask;
        // Spawn-land agreement: validate nearby SVO hits against the RESHAPED
        // height; the raw height is below sea across the spawn band and used to
        // reject every legitimate reshaped-land hit here.
        const float rawSvoHeight =
            FarTerrainHeight(hitPos.xz, rawMountainMask, rawSpireMask, rawRavineMask);
        const float rawHeight = FarSpawnLandReshapeHeight(hitPos.xz, rawSvoHeight, rawMountainMask);
        const float surfaceTolerance = max(8.0f, FarFallbackCellSize(nearestT) * 0.75f);
        if (rawHeight < FAR_SEA_LEVEL ||
            hitPos.y > rawHeight + surfaceTolerance) {
            return false;
        }
    }
    if (frame.debugMode == 54u || frame.debugMode == 56u) {
        farHit = MakeHit(float4(DebugMaterialColor(nearestMaterial), 1.0f), nearestT);
        return true;
    }
    if (frame.debugMode == 59u) {
        const float cellShade = saturate(FarFallbackCellSize(nearestT) / 160.0f);
        farHit = MakeHit(float4(0.18f, cellShade, 1.0f - cellShade * 0.55f, 1.0f), nearestT);
        return true;
    }
    if (frame.debugMode == 63u) {
        farHit = MakeHit(float4(saturate(nearestNormal * 0.5f + 0.5f), 1.0f), nearestT);
        return true;
    }
    if (frame.debugMode == 65u) {
        float3 debugFaceColor = float3(1.0f, 0.48f, 0.05f);
        if (nearestNormal.y > 0.62f) {
            debugFaceColor = float3(0.05f, 0.95f, 0.18f);
        } else if (nearestNormal.y < -0.35f) {
            debugFaceColor = float3(0.95f, 0.05f, 0.95f);
        }
        farHit = MakeHit(float4(debugFaceColor, 1.0f), nearestT);
        return true;
    }
    if (frame.debugMode == 66u) {
        const float distanceBand = saturate(nearestT / 6400.0f);
        farHit = MakeHit(float4(distanceBand, distanceBand, distanceBand, 1.0f), nearestT);
        return true;
    }
    if (frame.debugMode == 62u) {
        farHit = MakeHit(float4(DebugClosureColor(hitPos), 1.0f), nearestT);
        return true;
    }
    baseColor.rgb = FarTerrainMaterialVariation(baseColor.rgb, nearestMaterial, hitPos.xz, hitPos.y, nearestT);
    // PALETTE UNIFICATION (altitude patchwork): shade with the EXACT mid-voxel
    // constants (ndotl *0.62+0.28, ambient *0.76 + ndotl*0.34, floor
    // *0.54+0.045). The old combo (flatten 0.58, lifted *0.78+0.42, floor
    // 0.62+0.050) lit far ground flatter and brighter than the mid voxels
    // beside it -> washed/pale far vs crisp mid, two art styles.
    // Flatten strength is mid-matched (0.34) NEAR but grows with distance:
    // the SVO leaf normals are raw 3u-epsilon analytic samples, and past
    // ~2km one pixel spans many world units, so unflattened they alias into
    // grey per-pixel lighting speckle (a third art style). Branchless ramp;
    // quantizing the leaf-sample xz instead is driver-JIT fatal (see the
    // leaf-exit notes).
    const float svoFlatten = 0.34f + saturate((nearestT - 1200.0f) / 2400.0f) * 0.44f;
    float3 shadeNormal = DistantLodShadeNormal(nearestNormal, nearestT, svoFlatten);
    float3 lightDir = SkySunDirection();
    float ndotl = saturate(dot(shadeNormal, lightDir) * 0.62f + 0.28f);
    float3 color = baseColor.rgb * (SkyAmbient(shadeNormal) * 0.76f + ndotl * 0.34f);
    color = max(color, baseColor.rgb * 0.54f + 0.045f);
    const float farSvoGridFade = saturate((nearestT - 1200.0f) / 6200.0f);
    const float farSvoGrid = VoxelGridLine(
        hitPos.xz,
        FarFallbackCellSize(nearestT),
        lerp(0.036f, 0.020f, farSvoGridFade));
    color *= lerp(1.0f, 0.965f, farSvoGrid);
    float fogFactor = saturate((nearestT - 900.0f) / (10400.0f - 900.0f));
    const float horizonHaze = saturate((0.20f - abs(rayDir.y)) / 0.20f);
    if (frame.debugMode == 60u) {
        const float debugFog = saturate(fogFactor * 0.42f + horizonHaze * 0.14f);
        farHit = MakeHit(float4(float3(debugFog, debugFog, debugFog), 1.0f), nearestT);
        return true;
    }
    color = lerp(
        color,
        SkyColor(rayDir),
        (fogFactor * 0.60f + horizonHaze * 0.36f + 0.16f) * FarHazeDowncastScale(rayDir.y));
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
    // whenever Far SVO backed off under budget pressure. The upward ceiling is
    // 0.12 (was 0.04): distant mountain silhouettes (peaks up to
    // FAR_TERRAIN_MAX_HEIGHT seen from ground level over the far ownership band)
    // climb to rayDir.y ~ +0.07, so the old 0.04 ceiling leaked sky between the
    // sparse far-SVO chunks. Rays that clear all peaks still return false (sky).
    // Raised 0.12 -> 0.22: the far heightfield now owns the full distant
    // silhouette band (the deferral hands it the mountain tips up to ~0.22 that
    // the sparse SVO used to paint as detached dark blobs).
    if (rayDir.y > 0.22f) {
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
        // If the caller pushed the far-height fallback behind a near ownership
        // boundary, an already-inside first sample usually means the ray crossed
        // the heightfield before that boundary. Resolve that crossing instead
        // of surfacing a raw miss; otherwise high-altitude and fast-motion views
        // expose holes even though the procedural terrain is continuous.
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
            float3 normal = DistantLodShadeNormal(FarTerrainVoxelNormal(hitPos.xz, hitT), hitT, 0.34f);
            uint material = FarTerrainMaterial(hitPos.xz, previousHeight, mountainMask, spireMask, ravineMask);
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);
            if (frame.debugMode == 54u || frame.debugMode == 56u) {
                farHit = MakeHit(float4(DebugMaterialColor(material), 1.0f), hitT);
                return true;
            }
            baseColor.rgb = FarTerrainMaterialVariation(baseColor.rgb, material, hitPos.xz, previousHeight, hitT);
            // Shade identically to the loop-branch hit below: same shared sun,
            // same ambient-aware fill, same ambient floor, same haze. The two
            // branches resolve the SAME far surface, so a near-vertical-down
            // first-hit must not read darker/less-hazed than the loop result a
            // few pixels away (that mismatch is the dark backdrop blob).
            // PALETTE UNIFICATION (altitude patchwork): both branches now use
            // the EXACT mid-voxel shade constants so far-owned ground reads as
            // the same art style as the mid voxels beside it.
            float3 lightDir = SkySunDirection();
            float lighting = saturate(dot(normal, lightDir) * 0.62f + 0.28f);
            float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.76f + lighting * 0.34f);
            color = max(color, baseColor.rgb * 0.54f + 0.045f);
            const float farGridFade = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            const float farGrid = VoxelGridLine(
                hitPos.xz,
                FarFallbackCellSize(hitT),
                lerp(0.044f, 0.018f, farGridFade));
            color *= lerp(1.0f, 0.965f, farGrid);
            float fogFactor = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            const float horizonHaze = saturate((0.20f - abs(rayDir.y)) / 0.20f);
            // Dissolve distant far-height peaks into a continuous hazy ridge. The
            // mid-far peaks read as detached lit chunks with sky gaps between them;
            // ramp haze by DISTANCE across a wide horizon band (not just the up-tips)
            // so far chunks melt to sky/haze while nearer mid terrain stays visible.
            const float horizonHazeWide = saturate((0.35f - abs(rayDir.y)) / 0.35f);
            const float midFarHaze = saturate((hitT - 3500.0f) / 5000.0f);
            const float farHazeAmount = saturate(
                fogFactor * 0.60f + horizonHazeWide * 0.42f
                + midFarHaze * (0.42f + horizonHazeWide * 0.40f) + 0.20f)
                * FarHazeDowncastScale(rayDir.y);
            color = lerp(color, SkyColor(rayDir), farHazeAmount);
            farHit = MakeHit(float4(color, 1.0f), hitT);
            return true;
        }

    }

    // This is a continuity fallback behind the page-indexed SVO, not the main
    // far renderer. Keep it cheap enough that sky/horizon pixels cannot become
    // the frame-time bottleneck.
    int farStepBudget = frame.renderBudgetParams.z < 0.6f
        ? ScaleFarFieldStepBudget(28, 20, 14)
        : (frame.renderBudgetParams.z < 0.9f
            ? ScaleFarFieldStepBudget(40, 30, 22)
            : ScaleFarFieldStepBudget(52, 40, 28));
    // PERF (recover fps after the 0.12->0.22 silhouette widening): upward-grazing
    // rays (rayDir.y > 0.06) only paint the thin mountain-tip silhouette band; past
    // the peaks they march to budget and return sky. They need far fewer steps than
    // downward rays that resolve a real surface, and the per-step height sample is
    // the dominant far-march cost. Halving their budget cuts the cost the widened
    // band added while keeping the continuous lit horizon (the silhouette band is
    // still owned up to 0.22; downward/level rays keep the full budget).
    if (rayDir.y > 0.06f) {
        farStepBudget = max(farStepBudget / 2, 12);
    }
    [loop]
    for (int i = 0; i < farStepBudget && t < farMaxDist; ++i) {
        float distanceStep = lerp(128.0f, 420.0f, saturate(t / farMaxDist));
        float svoStep = frame.renderBudgetParams.z > 0.92f
            ? FarSvoSuggestedStep(rayOrigin, rayDir, t)
            : distanceStep;
        float stepSize = max(FAR_SVO_MIN_CELL_SIZE, max(svoStep, distanceStep));
        if (previousSigned > 0.0f && rayDir.y < -0.030f) {
            const float verticalStep = previousSigned / max(-rayDir.y, 0.045f);
            const float qualityStepCap = lerp(distanceStep * 1.50f, distanceStep * 3.25f, BackgroundRenderQuality());
            stepSize = clamp(
                verticalStep * 0.58f,
                FAR_SVO_MIN_CELL_SIZE,
                max(stepSize, qualityStepCap));
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
            float3 normal = DistantLodShadeNormal(FarTerrainVoxelNormal(hitPos.xz, hitT), hitT, 0.34f);
            uint material = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);
            if (frame.debugMode == 54u || frame.debugMode == 56u) {
                farHit = MakeHit(float4(DebugMaterialColor(material), 1.0f), hitT);
                return true;
            }
            baseColor.rgb = FarTerrainMaterialVariation(baseColor.rgb, material, hitPos.xz, height, hitT);

            // Single shared sun (was normalize(0.5,1.0,0.3), a steeper/higher
            // sun than every other layer). Using SkySunDirection() so the far
            // backdrop is lit from the same angle as the mid/near terrain in
            // front of it -> no brightness/shading step at the mid/far seam.
            float3 lightDir = SkySunDirection();
            float lighting = saturate(dot(normal, lightDir) * 0.62f + 0.28f);
            // Ambient-aware fill matching the mid-voxel visual language
            // (SkyAmbient + ndotl) so the distant terrain reads hazy-lit, not
            // a dark blob. PALETTE UNIFICATION (altitude patchwork): now the
            // EXACT mid-voxel constants -> one art style across the handoff.
            float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.76f + lighting * 0.34f);
            color = max(color, baseColor.rgb * 0.54f + 0.045f);
            const float farGridFade = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            const float farGrid = VoxelGridLine(
                hitPos.xz,
                FarFallbackCellSize(hitT),
                lerp(0.044f, 0.018f, farGridFade));
            color *= lerp(1.0f, 0.965f, farGrid);

            // Atmospheric haze toward the sky keeps the distant silhouette
            // hazy-lit rather than a hard dark edge.
            float fogFactor = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            const float horizonHaze = saturate((0.20f - abs(rayDir.y)) / 0.20f);
            // SILHOUETTE-TIP DISSOLVE (60fps mid-far): the far-height peaks read as
            // detached floating chunks because their tips hold a hard lit edge
            // against the sky while real geometry gaps between peaks expose sky. Ramp
            // extra haze as the ray climbs the upward silhouette band (rayDir.y -> the
            // 0.22 ceiling) so the distant tips melt into a continuous hazy ridge
            // instead of isolated blocks. Shading-only; terrain height untouched.
            // Dissolve distant far-height peaks into a continuous hazy ridge. The
            // mid-far peaks read as detached lit chunks with sky gaps between them;
            // ramp haze by DISTANCE across a wide horizon band (not just the up-tips)
            // so far chunks melt to sky/haze while nearer mid terrain stays visible.
            const float horizonHazeWide = saturate((0.35f - abs(rayDir.y)) / 0.35f);
            const float midFarHaze = saturate((hitT - 3500.0f) / 5000.0f);
            const float farHazeAmount = saturate(
                fogFactor * 0.60f + horizonHazeWide * 0.42f
                + midFarHaze * (0.42f + horizonHazeWide * 0.40f) + 0.20f)
                * FarHazeDowncastScale(rayDir.y);
            color = lerp(color, SkyColor(rayDir), farHazeAmount);
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

// Reflective water surface: fresnel-weighted sky reflection + sun glint + depth-darkened
// base, with a gentle animated ripple normal. Replaces the old flat teal plane.
float3 ShadeWaterSurface(float3 rayDir, float3 hitPos, float waterT) {
    const float t = (float)(frame.frameIndex & 8191u) * 0.02f;
    const float2 rip = float2(
        sin(hitPos.x * 0.080f + hitPos.z * 0.050f + t),
        sin(hitPos.z * 0.070f - hitPos.x * 0.060f + t * 0.8f));
    // GLINT/RIPPLE DISTANCE FADE: at altitude the ripple wavelength (~80-125u)
    // is a few pixels, so the narrow pow-200 glint lobe strobes per-pixel into a
    // white dot-grid moire on the far ocean. Fade ripple amplitude and glint out
    // past ~1500u; near water keeps the full sparkle. Glint gain unified at 1.6
    // with the near raster sheet (PS_SparseSurface) so the handoff is invisible.
    const float rippleFade = 1.0f - saturate((waterT - 1500.0f) / 2500.0f);
    const float3 n = normalize(float3(rip.x * 0.05f * rippleFade, 1.0f, rip.y * 0.05f * rippleFade));
    const float ndotv = saturate(dot(n, -rayDir));
    const float fresnel = 0.03f + 0.97f * pow(1.0f - ndotv, 5.0f);
    float3 refl = reflect(rayDir, n);
    refl.y = abs(refl.y);
    const float3 skyRefl = SkyColor(refl);
    const float glint = pow(saturate(dot(refl, SkySunDirection())), 200.0f);
    const float depth = saturate((waterT - 40.0f) / 600.0f);
    const float3 deep = lerp(float3(0.10f, 0.27f, 0.35f), float3(0.05f, 0.16f, 0.24f), depth);
    float3 color = lerp(deep, skyRefl, saturate(fresnel * 0.88f));
    color += float3(1.0f, 0.96f, 0.82f) * glint * 1.6f * rippleFade;
    const float fog = saturate((waterT - 650.0f) / 3800.0f);
    color = lerp(color, SkyColor(rayDir), fog * 0.38f);
    return color;
}

bool RaymarchFarWater(float3 rayOrigin, float3 rayDir, float startDist, out RayHit waterHit) {
    waterHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (rayOrigin.y < FAR_WATER_SURFACE_Y) {
        return false;
    }

    // Basin water is generated deterministically below sea level. Keep it as
    // an occluder for downward background rays too, or mid/far terrain can
    // expose dry shoreline material that disappears when exact water pages
    // arrive.
    if (rayDir.y >= -0.015f || rayDir.y < -0.92f) {
        return false;
    }

    const float waterT = (FAR_WATER_SURFACE_Y - rayOrigin.y) / rayDir.y;
    if (waterT < max(startDist, 32.0f) || waterT > 10400.0f) {
        return false;
    }

    const float3 hitPos = rayOrigin + rayDir * waterT;
    float mountainMask, spireMask, ravineMask;
    const float terrainHeight = FarTerrainHeight(hitPos.xz, mountainMask, spireMask, ravineMask);
    // Water existence must agree with the RESHAPED geometry: the spawn-land
    // block lifts the old flooded basin onto solid land out to ~9500u, so the
    // analytic sea sheet must not exist there (it painted a navy ocean band
    // over real land in aerial views). Consumer-site reshape; see
    // FarSpawnLandBand for why FarTerrainHeight cannot carry this term.
    if (FarSpawnLandReshapeHeight(hitPos.xz, terrainHeight, mountainMask) >= FAR_SEA_LEVEL) {
        return false;
    }

    waterHit = MakeHit(float4(ShadeWaterSurface(rayDir, hitPos, waterT), 1.0f), waterT);
    return true;
}

float LowerLodWaterlineTolerance(float backgroundDistance) {
    const float baseTolerance = max(24.0f, min(96.0f, backgroundDistance * 0.015f));
    // Coarse mid/far voxel surfaces can put a waterline side face noticeably
    // in front of the analytic sea-plane crossing. Treat deterministic water
    // as the foreground owner across that representation error, but keep the
    // tolerance bounded so real foreground terrain still occludes water.
    const float coarseTolerance = max(128.0f, min(384.0f, backgroundDistance * 0.12f));
    return max(baseTolerance, coarseTolerance);
}

static const uint BACKGROUND_LAYER_NONE = 0u;
static const uint BACKGROUND_LAYER_MID_VOXEL = 1u;
static const uint BACKGROUND_LAYER_MID_HEIGHT = 2u;
static const uint BACKGROUND_LAYER_FAR_SVO = 3u;
static const uint BACKGROUND_LAYER_FAR_HEIGHT = 4u;
static const uint BACKGROUND_LAYER_FAR_WATER = 5u;

float3 DebugOwnerMaterialColor(float3 materialColor, uint layer) {
    float3 ownerTint = float3(0.95f, 0.95f, 0.95f);
    float ownerWeight = 0.18f;
    if (layer == BACKGROUND_LAYER_MID_VOXEL) {
        ownerTint = float3(0.05f, 0.95f, 0.25f);
        ownerWeight = 0.34f;
    } else if (layer == BACKGROUND_LAYER_MID_HEIGHT) {
        ownerTint = float3(1.0f, 0.86f, 0.08f);
        ownerWeight = 0.42f;
    } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
        ownerTint = float3(0.20f, 0.42f, 1.0f);
        ownerWeight = 0.36f;
    } else if (layer == BACKGROUND_LAYER_FAR_HEIGHT) {
        ownerTint = float3(1.0f, 0.45f, 0.08f);
        ownerWeight = 0.46f;
    } else if (layer == BACKGROUND_LAYER_FAR_WATER) {
        ownerTint = float3(0.04f, 0.28f, 0.95f);
        ownerWeight = 0.24f;
    } else {
        ownerTint = float3(1.0f, 0.08f, 0.72f);
        ownerWeight = 0.46f;
    }
    return saturate(materialColor * (1.0f - ownerWeight) + ownerTint * ownerWeight);
}

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
static const uint RENDER_OWNER_FAR_WATER = 11u;
static const uint RENDER_OWNER_WATER_CONTEXT = 12u;
static const uint RENDER_OWNER_VALLEY_ATMOSPHERE = 13u;
static const uint RENDER_OWNER_LOD_PARENT_HELD = 14u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_COUNT = 15u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_BRICK_X = 16u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_BRICK_Y = 17u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_BRICK_Z = 18u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_DIST = 19u;
static const uint RENDER_OWNER_MID_INTERIOR_FALLBACK = 20u;
static const uint RENDER_OWNER_FAR_SURFACE = 21u;
static const uint RENDER_OWNER_FAR_HEIGHT_CONTINUITY = 22u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_MISSING = 23u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_AIR = 24u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_SOLID = 25u;
static const uint RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_PRESENT = 26u;
static const uint RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_MISSING = 27u;
static const uint RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_OUT_OF_GRID = 28u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_COUNT = 29u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_LIST_BASE = 40u;
static const uint RENDER_OWNER_UNSAFE_SAMPLE_LIST_CAPACITY = 256u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_LIST_BASE =
    RENDER_OWNER_UNSAFE_SAMPLE_LIST_BASE + RENDER_OWNER_UNSAFE_SAMPLE_LIST_CAPACITY * 4u;
static const uint RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_LIST_CAPACITY = 256u;

float ExactNearDistance() {
    return max(frame.exactNearParams.x, 0.0f);
}

bool CameraUnderwaterForShading() {
    return frame.cameraPosition.y < FAR_WATER_SURFACE_Y - 0.5f;
}

float PublicExactSurfaceDistance() {
    const float rasterMax = max(frame.surfaceRasterParams.x, 0.0f);
    if (rasterMax > 0.0f) {
        return max(rasterMax, ExactNearDistance());
    }
    return ExactNearDistance();
}

bool SparseExactResidentAirOrWaterAt(float3 worldPos) {
    if (frame.sparseNearParams.x <= 0.5f) {
        return false;
    }

    const uint maxPages = (uint)frame.sparseNearParams.y;
    const uint tableCapacity = (uint)frame.sparseNearParams.z;
    const int3 worldVoxel = int3(floor(worldPos));
    const int3 brickCoord = int3(
        FloorDiv16(worldVoxel.x),
        FloorDiv16(worldVoxel.y),
        FloorDiv16(worldVoxel.z));

    SparseBrickPageEntry entry;
    if (!LookupSparseBrick(brickCoord, tableCapacity, entry)) {
        return false;
    }
    if (entry.pageIndex >= maxPages ||
        SparseBrickPageGenerations[entry.pageIndex] != entry.generation) {
        return false;
    }

    const uint2 pageOccupancy = SparseBrickOccupancy[entry.pageIndex];
    if ((pageOccupancy.x | pageOccupancy.y) == 0u) {
        return true;
    }

    const uint3 localVoxel = uint3(
        (uint)FloorModInt(worldVoxel.x, 16),
        (uint)FloorModInt(worldVoxel.y, 16),
        (uint)FloorModInt(worldVoxel.z, 16));
    const uint3 subCoord = localVoxel >> 2u;
    const uint subIndex = subCoord.x + subCoord.y * 4u + subCoord.z * 16u;
    const uint occupancyWord = subIndex < 32u ? pageOccupancy.x : pageOccupancy.y;
    const uint occupancyBit = subIndex < 32u ? subIndex : subIndex - 32u;
    if (((occupancyWord >> occupancyBit) & 1u) == 0u) {
        return true;
    }

    const uint localIndex = SparseLocalIndex(localVoxel);
    const uint voxel = SparseBrickVoxelPool[entry.pageIndex * SPARSE_BRICK_VOXEL_COUNT + localIndex];
    const uint material = GetMaterial(voxel);
    return material == MAT_AIR || material == MAT_WATER;
}

bool BackgroundHitAllowedByExactNear(float3 rayOrigin, float3 rayDir, RayHit hit, uint layer) {
    if (layer == BACKGROUND_LAYER_NONE) {
        return true;
    }
    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    const bool sparseSurfaceRaymarchFill = (sparseNearFlags & 8u) != 0u;
    if (sparseSurfaceRaymarchFill &&
        (layer == BACKGROUND_LAYER_MID_VOXEL ||
         layer == BACKGROUND_LAYER_FAR_SVO ||
         layer == BACKGROUND_LAYER_FAR_WATER)) {
        const bool lowerLodTerrain =
            layer == BACKGROUND_LAYER_MID_VOXEL ||
            layer == BACKGROUND_LAYER_FAR_SVO;
        const float exactDistance = ExactNearDistance();
        const float surfaceOwnershipDistance = max(frame.nearOwnershipParams.w, exactDistance);
        if (lowerLodTerrain &&
            surfaceOwnershipDistance > 0.0f &&
            hit.distance <= surfaceOwnershipDistance + 64.0f) {
            const float3 hitPos = rayOrigin + rayDir * hit.distance;
            if (SparseExactResidentAirOrWaterAt(hitPos)) {
                return false;
            }
        }
        // The fullscreen raymarch runs after the raster sparse surface pass.
        // If execution reaches this function for a pixel, the exact surface did
        // not already own that pixel. Let legitimate resident lower LOD carry
        // the public frame while exact sparse terrain streams in, instead of
        // rejecting it solely because it lies inside the exact ownership radius.
        return true;
    }
    const float exactDistance = ExactNearDistance();
    const float surfaceOwnershipDistance = max(frame.nearOwnershipParams.w, exactDistance);
    if (surfaceOwnershipDistance <= 0.0f) {
        return true;
    }
    return hit.distance >= surfaceOwnershipDistance;
}

// WALKING-HOLES NOTE: do NOT add a mid-voxel residency probe anywhere in the
// RaymarchBackgroundField / water-resolver inline region. Two attempts (full
// fallback sampler, then a single-ring SampleResidentMidVoxel probe adding only
// ~12KB DXIL) both made CreateGraphicsPipelineState fail in the NVIDIA driver
// JIT (0x-7FF8FFF2 family — same driver fragility as the FixA branchless
// reshape). The pending-land water suppression is therefore driven only by the
// exact near DDA's missing-brick signal (suppressPendingLandWater threading
// below); the mid-ring leading edge is handled CPU-side by the moving-camera
// mid clipmap pump burst in main_launcher.cpp.
bool TryResolveWaterOccluderForBackgroundHit(
    float3 rayOrigin,
    float3 rayDir,
    float startDist,
    RayHit backgroundHit,
    bool hasPrecomputedWaterOccluder,
    RayHit precomputedWaterOccluder,
    out RayHit waterHit,
    bool suppressPendingLandWater = false)
{
    waterHit = precomputedWaterOccluder;
    bool hasWater = hasPrecomputedWaterOccluder;

    // WALKING-HOLES FIX: when the exact near DDA crossed a not-yet-streamed
    // brick whose analytic terrain column is dry land above sea, the analytic
    // water sheet must not own this ray. Without this gate, streaming holes at
    // the exact/mid leading edge render as navy "water holes" on land until the
    // page promotes; with it, the underlying mid/far land estimate shows
    // through instead, which visually matches the surrounding terrain.
    if (suppressPendingLandWater) {
        return false;
    }

    if (!hasWater) {
        // TANDEM (Codex root cause): this branch RE-SYNTHESIZES an analytic water
        // plane when the precomputed RaymarchFarWater occluder was false. But
        // RaymarchFarWater is the spawn-land-aware test (it self-suppresses over
        // reshaped land, 3807); its synthesis-suppression here keys on the
        // BACKGROUND hit being below sea (terrainPos.y), NOT the band, so for a
        // low-fly (~250u) ray whose far terrain hit is below sea it resurrects the
        // water plane band-independently over what is solid land at ground level.
        // That is the stubborn altitude-only far-water overlay (band-/latch-
        // independent). Trust the precomputed RaymarchFarWater verdict: if it
        // found no water, do not re-synthesize. Real water keeps the occluder
        // true and is unaffected. Removes ALU (driver-safe; shader at the PSO cliff).
        return false;
    }

    const float3 backgroundPos = rayOrigin + rayDir * backgroundHit.distance;
    // Spawn-land agreement (same rule as the probe branch above): reshaped
    // land hits inside the spawn band are not "submerged terrain" and must not
    // be overwritten by the analytic water sheet.
    if (backgroundPos.y >
        FAR_WATER_SURFACE_Y + lerp(96.0f, -8.0f, FarSpawnLandBand(backgroundPos.xz))) {
        return false;
    }

    const float waterLeadTolerance = LowerLodWaterlineTolerance(backgroundHit.distance);
    if (!hasWater ||
        waterHit.distance > backgroundHit.distance + waterLeadTolerance ||
        !BackgroundHitAllowedByExactNear(rayOrigin, rayDir, waterHit, BACKGROUND_LAYER_FAR_WATER)) {
        return false;
    }
    return true;
}

bool TryResolveDeterministicWaterBeforeBackground(
    float3 rayOrigin,
    float3 rayDir,
    RayHit backgroundHit,
    uint layer,
    out RayHit waterHit)
{
    waterHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (layer == BACKGROUND_LAYER_NONE || layer == BACKGROUND_LAYER_FAR_WATER) {
        return false;
    }
    if (rayOrigin.y < FAR_WATER_SURFACE_Y ||
        rayDir.y >= -0.015f ||
        rayDir.y < -0.92f) {
        return false;
    }

    const float waterT = (FAR_WATER_SURFACE_Y - rayOrigin.y) / rayDir.y;
    if (waterT < 32.0f || waterT > 10400.0f) {
        return false;
    }

    const float3 hitPos = rayOrigin + rayDir * waterT;
    float mountainMask, spireMask, ravineMask;
    const float terrainHeight = FarTerrainHeight(hitPos.xz, mountainMask, spireMask, ravineMask);
    // Spawn-land agreement: no deterministic water where the geometry layers
    // reshape the basin into spawn land (see FarSpawnLandBand).
    if (FarSpawnLandReshapeHeight(hitPos.xz, terrainHeight, mountainMask) >= FAR_SEA_LEVEL) {
        return false;
    }

    const float3 backgroundPos = rayOrigin + rayDir * backgroundHit.distance;
    // Spawn-land agreement: reshaped land hits are not "submerged terrain".
    if (backgroundPos.y >
        FAR_WATER_SURFACE_Y + lerp(96.0f, -8.0f, FarSpawnLandBand(backgroundPos.xz))) {
        return false;
    }

    // Coarse mid/far cells can report a vertical terrain face slightly in
    // front of the analytic sea-plane crossing even when procedural truth at
    // that plane is water. Exact sparse surfaces are resolved before this
    // fallback, so inside the public exact/surface band deterministic water may
    // own shoreline side faces, but it must not replace a clearly closer
    // terrain hit. Otherwise missing exact terrain appears as broad water until
    // the exact page streams in.
    const bool foregroundWaterlineFallback =
        (layer == BACKGROUND_LAYER_FAR_SVO ||
         layer == BACKGROUND_LAYER_MID_VOXEL ||
         layer == BACKGROUND_LAYER_MID_HEIGHT ||
         layer == BACKGROUND_LAYER_FAR_HEIGHT) &&
        waterT <= max(PublicExactSurfaceDistance() + 512.0f, 2048.0f);
    const float waterLeadTolerance = foregroundWaterlineFallback
        ? max(LowerLodWaterlineTolerance(backgroundHit.distance), 192.0f)
        : LowerLodWaterlineTolerance(backgroundHit.distance);
    if (waterT > backgroundHit.distance + waterLeadTolerance) {
        return false;
    }

    waterHit = MakeHit(float4(ShadeWaterSurface(rayDir, hitPos, waterT), 1.0f), waterT);
    return true;
}

RayHit DebugWaterlineResolverReasonHit(
    float3 rayOrigin,
    float3 rayDir,
    RayHit backgroundHit,
    uint layer)
{
    float3 color = float3(0.08f, 0.08f, 0.10f);

    if (layer == BACKGROUND_LAYER_NONE) {
        color = float3(0.10f, 0.10f, 0.10f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }
    if (layer == BACKGROUND_LAYER_FAR_WATER) {
        color = float3(0.00f, 0.85f, 1.00f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }
    if (rayOrigin.y < FAR_WATER_SURFACE_Y ||
        rayDir.y >= -0.015f ||
        rayDir.y < -0.92f) {
        // Red: this ray is not eligible for deterministic water.
        color = float3(1.0f, 0.05f, 0.04f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }

    const float waterT = (FAR_WATER_SURFACE_Y - rayOrigin.y) / rayDir.y;
    if (waterT < 32.0f || waterT > 10400.0f) {
        // Orange: water plane crossing is outside the public ray range.
        color = float3(1.0f, 0.45f, 0.02f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }

    const float3 hitPos = rayOrigin + rayDir * waterT;
    float mountainMask, spireMask, ravineMask;
    const float terrainHeight = FarTerrainHeight(hitPos.xz, mountainMask, spireMask, ravineMask);
    if (terrainHeight >= FAR_SEA_LEVEL) {
        // Yellow: shader terrain truth says this XZ is dry terrain, not water.
        color = float3(1.0f, 0.92f, 0.02f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }

    const bool foregroundWaterlineFallback =
        (layer == BACKGROUND_LAYER_FAR_SVO ||
         layer == BACKGROUND_LAYER_MID_VOXEL ||
         layer == BACKGROUND_LAYER_MID_HEIGHT ||
         layer == BACKGROUND_LAYER_FAR_HEIGHT) &&
        waterT <= max(PublicExactSurfaceDistance() + 512.0f, 2048.0f);
    if (!foregroundWaterlineFallback) {
        const float waterLeadTolerance = LowerLodWaterlineTolerance(backgroundHit.distance);
        if (waterT > backgroundHit.distance + waterLeadTolerance) {
            // Purple: water is too far behind this lower-LOD terrain hit.
            color = float3(0.70f, 0.10f, 1.0f);
            return MakeHit(float4(color, 1.0f), backgroundHit.distance);
        }
        // Green: deterministic water would be accepted through tolerance.
        color = float3(0.05f, 1.0f, 0.20f);
        return MakeHit(float4(color, 1.0f), backgroundHit.distance);
    }

    // Cyan: deterministic water would be accepted by the foreground waterline
    // fallback. If the normal owner for this pixel is still lower LOD, a later
    // path is bypassing or overwriting this resolver.
    color = float3(0.00f, 0.95f, 1.0f);
    return MakeHit(float4(color, 1.0f), backgroundHit.distance);
}

bool RenderOwnershipEnabled() {
    return frame.farFieldGridParams.w > 0.5f;
}

void RecordRenderOwnership(uint owner) {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_TOTAL], 1u);
    if (owner <= RENDER_OWNER_MISS ||
        owner == RENDER_OWNER_SURFACE ||
        owner == RENDER_OWNER_UNSAFE_NEAR_MISS ||
        owner == RENDER_OWNER_FAR_WATER ||
        owner == RENDER_OWNER_WATER_CONTEXT ||
        owner == RENDER_OWNER_VALLEY_ATMOSPHERE) {
        InterlockedAdd(RenderOwnershipStats[owner], 1u);
    }
    RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
}

void RecordRenderLodParentHeld() {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_LOD_PARENT_HELD], 1u);
    RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
}

void RecordRenderMidInteriorFallback() {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_MID_INTERIOR_FALLBACK], 1u);
    RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
}

void RecordRenderWaterContext() {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_WATER_CONTEXT], 1u);
}

void RecordFarHeightMidCoverageSample(float3 worldPos, uint preferredRing) {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    uint4 header = MidVoxelClipmapMetadata[0];
    const uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (header.x != MID_VOXEL_CLIPMAP_MAGIC || ringCount == 0u) {
        return;
    }

    const uint ring = min(preferredRing, ringCount - 1u);
    const float cellSize = MidClipmapRingCellSize(ring);
    const float brickWorldSize = max(cellSize * (float)SPARSE_BRICK_SIZE, 1.0f);
    const int3 brickCoord = int3(floor(worldPos / brickWorldSize));

    uint writeIndex = 0u;
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_COUNT], 1u, writeIndex);
    uint h = (uint)brickCoord.x * 73856093u;
    h ^= (uint)brickCoord.y * 19349663u;
    h ^= (uint)brickCoord.z * 83492791u;
    h ^= ring * 2654435761u;
    h ^= (uint)frame.frameIndex * 2246822519u;
    const uint sampleSlot = h % RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_LIST_CAPACITY;
    const uint base = RENDER_OWNER_FAR_HEIGHT_MID_SAMPLE_LIST_BASE + sampleSlot * 4u;
    RenderOwnershipStats[base + 0u] = ring;
    RenderOwnershipStats[base + 1u] = (uint)brickCoord.x;
    RenderOwnershipStats[base + 2u] = (uint)brickCoord.y;
    RenderOwnershipStats[base + 3u] = (uint)brickCoord.z;
}

void RecordFarHeightContinuityReason(float3 rayOrigin, float3 rayDir, float diagnosticTerrainT) {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_CONTINUITY], 1u);

    uint4 midHeader = MidVoxelClipmapMetadata[0];
    const uint ringCount = min(midHeader.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    bool sampledMid = false;
    uint sampledVoxel = PackVoxel(MAT_AIR, 0, 0, 0);
    if (frame.midFieldParams.x >= 0.5f &&
        midHeader.x == MID_VOXEL_CLIPMAP_MAGIC &&
        midHeader.z != 0u &&
        ringCount != 0u &&
        diagnosticTerrainT < 1e19f) {
        const float midStart = max(frame.midFieldParams.y, 1.0f);
        const float midEnd = max(frame.midFieldParams.z, midStart + 1.0f);
        if (diagnosticTerrainT <= midEnd) {
            const float ringDistance = max(diagnosticTerrainT, midStart);
            const uint preferredRing = min(
                (uint)floor(saturate((ringDistance - midStart) / max(midEnd - midStart, 1.0f)) *
                    (float)ringCount),
                ringCount - 1u);
            const float3 diagnosticPos = rayOrigin + rayDir * diagnosticTerrainT;
            RecordFarHeightMidCoverageSample(diagnosticPos, preferredRing);
            uint actualRing;
            float actualCellSize;
            sampledMid = SampleResidentMidVoxelFallback(
                diagnosticPos,
                preferredRing,
                true,
                sampledVoxel,
                actualRing,
                actualCellSize);
        }
    }
    if (!sampledMid) {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_MID_MISSING], 1u);
    } else if (GetMaterial(sampledVoxel) == MAT_AIR) {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_MID_AIR], 1u);
    } else {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_MID_SOLID], 1u);
    }

    bool farPagePresent = false;
    bool farPageInGrid = false;
    if (frame.farOwnershipParams.x > 0.5f &&
        frame.farOwnershipParams.y >= 0.999f &&
        frame.farOwnershipParams.z > 0.0f &&
        frame.farFieldParams.x > 0.5f &&
        frame.farFieldParams.y > 0.0f &&
        frame.farFieldParams.z > 0.0f &&
        diagnosticTerrainT < 1e19f) {
        const uint pageCount = (uint)frame.farFieldParams.y;
        const int pageRadius = (int)frame.farFieldGridParams.x;
        const int pageSide = (int)frame.farFieldGridParams.y;
        const float pageSize = max(frame.farFieldParams.w, 1.0f);
        const float3 diagnosticPos = rayOrigin + rayDir * diagnosticTerrainT;
        const int px = (int)floor(diagnosticPos.x / pageSize);
        const int pz = (int)floor(diagnosticPos.z / pageSize);
        if (pageCount > 0u &&
            pageRadius > 0 &&
            pageSide > 0 &&
            px >= -pageRadius &&
            px <= pageRadius &&
            pz >= -pageRadius &&
            pz <= pageRadius) {
            farPageInGrid = true;
            const uint denseIndex = (uint)((pz + pageRadius) * pageSide + (px + pageRadius));
            const uint pageIndex = FarVoxelPageIndex[denseIndex];
            farPagePresent = pageIndex != 0xFFFFFFFFu && pageIndex < pageCount;
        }
    }
    if (!farPageInGrid) {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_OUT_OF_GRID], 1u);
    }
    InterlockedAdd(
        RenderOwnershipStats[farPagePresent
            ? RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_PRESENT
            : RENDER_OWNER_FAR_HEIGHT_FAR_PAGE_MISSING],
        1u);
}

void RecordUnsafeSparseMissSample(int3 brickCoord, float distanceFromCamera) {
    if (!RenderOwnershipEnabled()) {
        return;
    }
    uint writeIndex = 0u;
    InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_UNSAFE_SAMPLE_COUNT], 1u, writeIndex);
    if (writeIndex == 0u) {
        RenderOwnershipStats[RENDER_OWNER_UNSAFE_SAMPLE_BRICK_X] = (uint)brickCoord.x;
        RenderOwnershipStats[RENDER_OWNER_UNSAFE_SAMPLE_BRICK_Y] = (uint)brickCoord.y;
        RenderOwnershipStats[RENDER_OWNER_UNSAFE_SAMPLE_BRICK_Z] = (uint)brickCoord.z;
        RenderOwnershipStats[RENDER_OWNER_UNSAFE_SAMPLE_DIST] = (uint)min(max(distanceFromCamera, 0.0f), 1000000.0f);
    }
    // A full-screen fallback can report hundreds of thousands of pixels in a
    // few dominant screen regions. First-N samples only teach the CPU about one
    // cluster and leave the rest of the visible wrong-owner terrain untouched.
    // Hash by brick so the fixed-size readback carries spatially diverse
    // render-critical exact misses.
    uint h = (uint)brickCoord.x * 73856093u;
    h ^= (uint)brickCoord.y * 19349663u;
    h ^= (uint)brickCoord.z * 83492791u;
    h ^= (uint)frame.frameIndex * 2654435761u;
    const uint sampleSlot = h % RENDER_OWNER_UNSAFE_SAMPLE_LIST_CAPACITY;
    const uint base = RENDER_OWNER_UNSAFE_SAMPLE_LIST_BASE + sampleSlot * 4u;
    RenderOwnershipStats[base + 0u] = (uint)brickCoord.x;
    RenderOwnershipStats[base + 1u] = (uint)brickCoord.y;
    RenderOwnershipStats[base + 2u] = (uint)brickCoord.z;
    RenderOwnershipStats[base + 3u] = (uint)min(max(distanceFromCamera, 0.0f), 1000000.0f);
}

uint SparseSurfaceFaceDirection(SparseSurfaceFace face) {
    return (face.payload >> 29u) & 0x7u;
}

uint SparseSurfaceFaceVoxel(SparseSurfaceFace face) {
    return face.payload & 0x0007FFFFu;
}

uint SparseSurfaceFaceWidth(SparseSurfaceFace face) {
    return ((face.payload >> 24u) & 0x1Fu) + 1u;
}

uint SparseSurfaceFaceHeight(SparseSurfaceFace face) {
    return ((face.payload >> 19u) & 0x1Fu) + 1u;
}

float3 SparseSurfaceFaceNormalFromDirection(uint direction) {
    if (direction == 0u) return float3(-1.0f, 0.0f, 0.0f);
    if (direction == 1u) return float3(1.0f, 0.0f, 0.0f);
    if (direction == 2u) return float3(0.0f, -1.0f, 0.0f);
    if (direction == 3u) return float3(0.0f, 1.0f, 0.0f);
    if (direction == 4u) return float3(0.0f, 0.0f, -1.0f);
    return float3(0.0f, 0.0f, 1.0f);
}

bool IntersectSparseSurfaceFaceForRay(
    SparseSurfaceFace face,
    float3 rayOrigin,
    float3 rayDir,
    float maxT,
    out float hitT,
    out float3 hitNormal,
    out uint hitMaterial)
{
    hitT = 0.0f;
    hitNormal = float3(0.0f, 1.0f, 0.0f);
    hitMaterial = MAT_AIR;

    const uint direction = SparseSurfaceFaceDirection(face);
    const float3 normal = SparseSurfaceFaceNormalFromDirection(direction);
    const float denom = dot(normal, rayDir);
    if (denom >= -0.0001f) {
        return false;
    }

    const float x0 = (float)face.voxelX;
    const float y0 = (float)face.voxelY;
    const float z0 = (float)face.voxelZ;
    const float width = (float)SparseSurfaceFaceWidth(face);
    const float height = (float)SparseSurfaceFaceHeight(face);
    const float x1 = x0 + (direction == 2u || direction == 3u || direction == 4u || direction == 5u ? width : 1.0f);
    const float y1 = y0 + (direction == 0u || direction == 1u || direction == 4u || direction == 5u ? height : 1.0f);
    const float z1 =
        z0 + (direction == 0u || direction == 1u ? width :
              direction == 2u || direction == 3u ? height :
              1.0f);

    float plane = x0;
    float originAxis = rayOrigin.x;
    float dirAxis = rayDir.x;
    if (direction == 1u) {
        plane = x1;
    } else if (direction == 2u) {
        plane = y0;
        originAxis = rayOrigin.y;
        dirAxis = rayDir.y;
    } else if (direction == 3u) {
        plane = y1;
        originAxis = rayOrigin.y;
        dirAxis = rayDir.y;
    } else if (direction == 4u) {
        plane = z0;
        originAxis = rayOrigin.z;
        dirAxis = rayDir.z;
    } else if (direction == 5u) {
        plane = z1;
        originAxis = rayOrigin.z;
        dirAxis = rayDir.z;
    }

    if (abs(dirAxis) < 0.000001f) {
        return false;
    }
    const float t = (plane - originAxis) / dirAxis;
    if (t <= 0.05f || t >= maxT) {
        return false;
    }

    const float3 p = rayOrigin + rayDir * t;
    const float pad = 0.02f;
    bool inside = false;
    if (direction == 0u || direction == 1u) {
        inside = p.y >= y0 - pad && p.y <= y1 + pad &&
                 p.z >= z0 - pad && p.z <= z1 + pad;
    } else if (direction == 2u || direction == 3u) {
        inside = p.x >= x0 - pad && p.x <= x1 + pad &&
                 p.z >= z0 - pad && p.z <= z1 + pad;
    } else {
        inside = p.x >= x0 - pad && p.x <= x1 + pad &&
                 p.y >= y0 - pad && p.y <= y1 + pad;
    }
    if (!inside) {
        return false;
    }

    hitT = t;
    hitNormal = normal;
    hitMaterial = GetMaterial(SparseSurfaceFaceVoxel(face));
    return hitMaterial != MAT_AIR;
}

bool ProbeSparseSurfaceRangeForRay(
    SparseSurfaceBrickRange range,
    float3 rayOrigin,
    float3 rayDir,
    inout float nearestT,
    inout float3 nearestNormal,
    inout uint nearestMaterial)
{
    bool found = false;
    const uint faceCount = min(range.faceCount, 384u);
    [loop]
    for (uint i = 0u; i < faceCount; ++i) {
        SparseSurfaceFace face = SparseSurfaceFaces[range.firstFace + i];
        float hitT;
        float3 hitNormal;
        uint hitMaterial;
        if (IntersectSparseSurfaceFaceForRay(face, rayOrigin, rayDir, nearestT, hitT, hitNormal, hitMaterial)) {
            nearestT = hitT;
            nearestNormal = hitNormal;
            nearestMaterial = hitMaterial;
            found = true;
        }
    }
    return found;
}

RayHit MakeSparseSurfaceRayHit(float3 rayOrigin, float3 rayDir, float hitT, float3 normal, uint material) {
    const float3 hitPos = rayOrigin + rayDir * hitT;
    if (frame.debugMode == 54u) {
        return MakeHit(float4(DebugMaterialColor(material), 1.0f), hitT);
    }
    if (frame.debugMode == 55u) {
        const float exactNearDistance = max(ExactNearDistance(), 0.0f);
        const float protectedSurfaceDistance = max(exactNearDistance + 768.0f, 1536.0f);
        if (exactNearDistance > 0.0f && hitT > protectedSurfaceDistance) {
            return MakeHit(float4(1.0f, 0.05f, 0.90f, 1.0f), hitT);
        }
        if (exactNearDistance > 0.0f && hitT > exactNearDistance) {
            return MakeHit(float4(1.0f, 0.46f, 0.05f, 1.0f), hitT);
        }
        return MakeHit(float4(1.0f, 0.95f, 0.05f, 1.0f), hitT);
    }
    if (frame.debugMode == 56u) {
        const float3 materialColor = DebugMaterialColor(material);
        return MakeHit(float4(saturate(materialColor * 0.86f + float3(0.95f, 0.95f, 0.95f) * 0.14f), 1.0f), hitT);
    }
    if (frame.debugMode == 63u) {
        return MakeHit(float4(saturate(normalize(normal) * 0.5f + 0.5f), 1.0f), hitT);
    }

    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
    float3 varied = BackgroundTerrainMaterialVariation(baseColor.rgb, material, hitPos, normal, hitT, 0.72f);
    const float3 lightDir = normalize(frame.sunDirection.xyz);
    const float diffuse = saturate(dot(normal, lightDir)) * 0.58f + 0.42f;
    float3 color = varied * diffuse + SkyAmbient(normal) * 0.18f;
    if (frame.cameraPosition.y <= 384.0f) {
        const float fog = saturate((hitT - 1200.0f) / 4200.0f) * 0.10f;
        color = lerp(color, SkyColor(float3(0.0f, -0.06f, 0.998f)), fog);
    }
    return MakeHit(float4(saturate(color), 1.0f), hitT);
}

bool ResolveExactSparseSurfaceBeforeBackground(
    float3 rayOrigin,
    float3 rayDir,
    RayHit backgroundHit,
    uint layer,
    out RayHit exactHit)
{
    exactHit = backgroundHit;
    if (layer != BACKGROUND_LAYER_MID_VOXEL &&
        layer != BACKGROUND_LAYER_FAR_SVO &&
        layer != BACKGROUND_LAYER_FAR_WATER) {
        return false;
    }

    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    const bool sparseNearActive = frame.sparseNearParams.x > 0.5f;
    const bool sparseSurfaceAuthoritative = (sparseNearFlags & 2u) != 0u;
    if (!sparseNearActive || !sparseSurfaceAuthoritative) {
        return false;
    }

    const float publicExactDistance = PublicExactSurfaceDistance();
    if (publicExactDistance <= 0.0f ||
        backgroundHit.distance <= 32.0f ||
        backgroundHit.distance > publicExactDistance + 64.0f) {
        return false;
    }

    float nearestT = min(backgroundHit.distance - 0.05f, publicExactDistance + 64.0f);
    if (nearestT <= 32.0f) {
        return false;
    }

    float3 nearestNormal = float3(0.0f, 1.0f, 0.0f);
    uint nearestMaterial = MAT_AIR;
    bool found = false;
    int3 lastBrick = int3(2147483647, 2147483647, 2147483647);

    [loop]
    for (uint i = 0u; i < 6u; ++i) {
        const float probeT = max(0.05f, backgroundHit.distance - (float)i * 12.0f);
        if (probeT > publicExactDistance + 64.0f) {
            continue;
        }
        const float3 probePos = rayOrigin + rayDir * probeT;
        const int3 brickCoord = int3(
            FloorDiv16((int)floor(probePos.x)),
            FloorDiv16((int)floor(probePos.y)),
            FloorDiv16((int)floor(probePos.z)));
        if (all(brickCoord == lastBrick)) {
            continue;
        }
        lastBrick = brickCoord;

        SparseSurfaceBrickRange surfaceRange;
        if (LookupSparseSurfaceRange(brickCoord, surfaceRange) && surfaceRange.faceCount > 0u) {
            found = ProbeSparseSurfaceRangeForRay(
                surfaceRange,
                rayOrigin,
                rayDir,
                nearestT,
                nearestNormal,
                nearestMaterial) || found;
        }
    }

    if (!found || nearestMaterial == MAT_AIR) {
        return false;
    }
    if (layer == BACKGROUND_LAYER_FAR_WATER && nearestMaterial == MAT_WATER) {
        return false;
    }

    exactHit = MakeSparseSurfaceRayHit(rayOrigin, rayDir, nearestT, nearestNormal, nearestMaterial);
    return true;
}

void RecordHiddenExactFallbackSampleForBackgroundHit(
    float3 rayOrigin,
    float3 rayDir,
    RayHit hit,
    uint layer)
{
    if (!RenderOwnershipEnabled()) {
        return;
    }
    if (layer != BACKGROUND_LAYER_MID_VOXEL &&
        layer != BACKGROUND_LAYER_FAR_SVO &&
        layer != BACKGROUND_LAYER_FAR_HEIGHT &&
        layer != BACKGROUND_LAYER_FAR_WATER) {
        return;
    }
    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    const bool sparseNearActive = frame.sparseNearParams.x > 0.5f;
    const bool sparseSurfaceAuthoritative = (sparseNearFlags & 2u) != 0u;
    if (!sparseNearActive || !sparseSurfaceAuthoritative) {
        return;
    }

    const float surfaceOwnershipDistance = max(frame.nearOwnershipParams.w, ExactNearDistance());
    if (surfaceOwnershipDistance <= 0.0f ||
        hit.distance < 32.0f ||
        hit.distance > surfaceOwnershipDistance) {
        return;
    }

    // Broad hidden-exact discovery is handled by the CPU foreground probe. The
    // pixel shader feedback is deliberately limited to very close fallback
    // hits; doing deterministic terrain scans here makes the fullscreen shader
    // too expensive and can stall startup.
    const float shaderFallbackFeedbackMaxDistance =
        min(surfaceOwnershipDistance, ExactNearDistance() + 160.0f);
    if (hit.distance > shaderFallbackFeedbackMaxDistance) {
        return;
    }

    const float3 hitPos = rayOrigin + rayDir * hit.distance;
    const int3 brickCoord = int3(
        FloorDiv16((int)floor(hitPos.x)),
        FloorDiv16((int)floor(hitPos.y)),
        FloorDiv16((int)floor(hitPos.z)));
    SparseSurfaceBrickRange surfaceRange;
    if (LookupSparseSurfaceRange(brickCoord, surfaceRange) && surfaceRange.faceCount > 0u) {
        return;
    }

    RecordUnsafeSparseMissSample(brickCoord, hit.distance);
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

float LowAltitudeProtectedBackgroundStartDistance() {
    const float exactDistance = ExactNearDistance();
    const float ownershipRadius = max(frame.nearOwnershipParams.w, exactDistance);
    // Low walking views need a real no-fake-terrain band around shorelines, but
    // the sparse raster ownership radius can be several kilometers. Using that
    // full radius as the fullscreen background start makes ordinary mid/far
    // voxel continuity depend on every raster surface chunk being resident.
    const float protectedBand = max(exactDistance + 768.0f, 1536.0f);
    return max(exactDistance, min(ownershipRadius, protectedBand));
}

float SurfaceAuthoritativeBackgroundStartDistance() {
    // The raster sparse-surface pass owns the editable foreground. In this mode
    // the fullscreen pass fills continuity behind the explicit ownership
    // sphere/near box. Keeping this base distance modest prevents the default
    // sparse-surface cull radius from creating a large sky gap whenever the
    // raster surface cache has not fully covered mid-distance valley walls.
    return NearBackgroundStartDistance();
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
    // The sparse surface cull sphere is intentionally larger than the exact
    // editable foreground. Using its far exit as the background start hides
    // resident mid-voxel cliffs between the exact-near radius and the cull
    // radius, which shows up as valley-atmosphere cutouts. Keep the hard
    // no-fake-terrain boundary at ExactNearDistance(); background hits are
    // still rejected by BackgroundHitAllowedByExactNear before they can own.
    startDistance = max(startDistance, ExactNearDistance() + 8.0f);
    if (frame.cameraPosition.y <= FAR_SEA_LEVEL + 220.0f) {
        startDistance = max(startDistance, LowAltitudeProtectedBackgroundStartDistance() + 8.0f);
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
    if (frame.cameraPosition.y <= FAR_SEA_LEVEL + 220.0f) {
        startDistance = max(startDistance, LowAltitudeProtectedBackgroundStartDistance() + 8.0f);
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

bool VoxelTerrainOnly() {
    const uint sparseNearFlags = (uint)frame.sparseNearParams.w;
    return (sparseNearFlags & 16u) != 0u;
}

bool DiagnosticFarTerrainWouldHit(float3 rayOrigin, float3 rayDir, float startDist, out float hitT);
float TerrainDiagnosticStartDistance();
bool TryBuildResidentMidVoxelClosureHit(float3 rayOrigin, float3 rayDir, float terrainT, out RayHit closureHit);

bool RaymarchBackgroundField(
    float3 rayOrigin,
    float3 rayDir,
    float startDist,
    bool includeSparseFarField,
    bool allowWideHeightAngles,
    out RayHit backgroundHit,
    out uint backgroundLayer,
    bool suppressPendingLandWater = false)
{
    backgroundHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    backgroundLayer = BACKGROUND_LAYER_NONE;

    const bool voxelTerrainOnly = VoxelTerrainOnly();
    RayHit waterOccluderHit;
    // Water is not a fake height proxy: SparseTerrainGenerator fills all
    // below-sea basin columns with water. Keep the sea-level occluder active
    // so deterministic water hides submerged mid/far terrain instead of
    // exposing dry sand that disappears when editable sparse pages arrive.
    const bool hasWaterOccluder = RaymarchFarWater(rayOrigin, rayDir, 32.0f, waterOccluderHit);
    const bool highAltitudeBackgroundView = rayOrigin.y > 384.0f;
    const bool lowAltitudeVoxelTerrainView = voxelTerrainOnly && !highAltitudeBackgroundView;
    const float farSvoCandidateQuality = min(frame.renderBudgetParams.z, frame.farOwnershipParams.w);
    const bool farSvoCandidateView =
        voxelTerrainOnly &&
        includeSparseFarField &&
        !highAltitudeBackgroundView &&
        rayOrigin.y > FAR_SEA_LEVEL + 24.0f &&
        rayDir.y > -0.24f &&
        rayDir.y < 0.16f &&
        farSvoCandidateQuality >= 0.35f &&
        frame.farOwnershipParams.x > 0.5f &&
        frame.farOwnershipParams.y >= 0.999f &&
        frame.farOwnershipParams.z > 0.0f &&
        frame.farFieldParams.x > 0.5f &&
        frame.farFieldParams.y > 0.0f &&
        frame.farFieldParams.z > 0.0f;
    RayHit elevatedFarSvoCandidateHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    bool hasElevatedFarSvoCandidateHit = false;
    // [perf] The elevated far-SVO candidate march now runs AFTER the mid DDA
    // (below) instead of eagerly here, so its traversal can be capped at the
    // furthest distance either of its two consumers can still accept.
    const bool preferCheapMidVoxelColumn =
        frame.renderBudgetParams.z < 0.55f || BackgroundRenderQuality() < 0.62f;
    const bool preferForegroundMidColumn =
        lowAltitudeVoxelTerrainView &&
        rayDir.y <= 0.06f;
    const bool allowMidVoxelColumnProxy = !voxelTerrainOnly || preferForegroundMidColumn;
    RayHit deferredMidInteriorHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    bool hasDeferredMidInteriorHit = false;
    if (allowMidVoxelColumnProxy &&
        (preferCheapMidVoxelColumn || preferForegroundMidColumn) &&
        RaymarchMidVoxelColumnClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, startDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    const bool skipFullMidVoxelDda =
        allowMidVoxelColumnProxy &&
        preferCheapMidVoxelColumn &&
        rayOrigin.y > 384.0f &&
        frame.renderBudgetParams.z < 0.55f;
    const float lowAltitudeVoxelContinuityStart =
        max(TerrainDiagnosticStartDistance(), max(frame.midFieldParams.y, 256.0f));
    const float midVoxelStartDist = lowAltitudeVoxelTerrainView
        ? max(startDist, lowAltitudeVoxelContinuityStart)
        : startDist;
    const bool hasMidVoxelDdaHit =
        !skipFullMidVoxelDda &&
        RaymarchMidVoxelClipmap(rayOrigin, rayDir, midVoxelStartDist, backgroundHit);
    // [perf, result-identical reorder] The elevated far-SVO candidate used to
    // march the full horizon-cone SVO BEFORE the mid DDA for every qualifying
    // ray; with the mid ring converged (midCov 1.0) the consumers below then
    // discarded almost every candidate because the mid hit is nearer. March it
    // after the mid DDA instead, capped at the furthest distance any consumer
    // can still accept:
    //   - the mid-hit handoff (below) accepts only
    //     candidate + frontBias(>=24) <= midHit.distance, and
    //   - the water-occluder replacement accepts only
    //     candidate <= waterHit.distance + frontBias(<=220).
    // When neither consumer is reachable (no mid hit AND no water occluder)
    // the candidate is never read, so the march is skipped entirely. Hits
    // nearer than the cap are found identically (the cap only seeds the same
    // nearestT pruning the traversal already applies after its first hit).
    if (farSvoCandidateView && (hasMidVoxelDdaHit || hasWaterOccluder)) {
        const float farSvoCandidateCap = max(
            hasMidVoxelDdaHit ? backgroundHit.distance : 0.0f,
            hasWaterOccluder ? waterOccluderHit.distance + 221.0f : 0.0f);
        if (RaymarchSparseFarField(
                rayOrigin,
                rayDir,
                max(startDist, frame.midFieldParams.y),
                farSvoCandidateCap,
                elevatedFarSvoCandidateHit) &&
            BackgroundHitAllowedByExactNear(rayOrigin, rayDir, elevatedFarSvoCandidateHit, BACKGROUND_LAYER_FAR_SVO)) {
            hasElevatedFarSvoCandidateHit = true;
        }
    }
    if (hasMidVoxelDdaHit) {
        const bool midInteriorFallback =
            (backgroundHit.diagnosticFlags & RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK) != 0u;
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, startDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            if (midInteriorFallback) {
                deferredMidInteriorHit = backgroundHit;
                hasDeferredMidInteriorHit = true;
            } else {
                if (hasElevatedFarSvoCandidateHit) {
                    // Resident mid voxels are the intended mid-distance
                    // mountain owner. The old handoff allowed the far SVO to
                    // replace a resident mid hit even when the far hit was
                    // hundreds of units behind it, which produced mixed
                    // coarse/sky-looking skyline ownership while all hard miss
                    // counters still passed. Treat far SVO as a true fallback
                    // or a clearly nearer occluder only.
                    const float frontBias = max(24.0f, min(backgroundHit.distance * 0.025f, 160.0f));
                    if (elevatedFarSvoCandidateHit.distance + frontBias <= backgroundHit.distance) {
                        RayHit resolvedWaterHit;
                        if (TryResolveWaterOccluderForBackgroundHit(
                                rayOrigin, rayDir, startDist, elevatedFarSvoCandidateHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                                suppressPendingLandWater)) {
                            backgroundHit = resolvedWaterHit;
                            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
                            return true;
                        }
                        backgroundHit = elevatedFarSvoCandidateHit;
                        backgroundLayer = BACKGROUND_LAYER_FAR_SVO;
                        return true;
                    }
                }
                return true;
            }
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    if (allowMidVoxelColumnProxy &&
        !preferCheapMidVoxelColumn &&
        RaymarchMidVoxelColumnClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, startDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    const float farStartDist = lowAltitudeVoxelTerrainView
        // Mid voxels are tried first. If they fail, Far SVO must begin before
        // the common low-altitude skyline crossing range (~2.0-2.2k) so it can
        // catch the entry surface instead of starting inside/behind it.
        ? max(lowAltitudeVoxelContinuityStart, 1800.0f)
        : (voxelTerrainOnly
            ? max(startDist, max(frame.midFieldParams.y, 256.0f))
            : FarLayerStartAfterBackground(startDist));
    if (lowAltitudeVoxelTerrainView &&
        includeSparseFarField &&
        rayDir.y > -0.22f &&
        rayDir.y < 0.20f) {
        float diagnosticTerrainT = 1e20f;
        if (DiagnosticFarTerrainWouldHit(rayOrigin, rayDir, lowAltitudeVoxelContinuityStart, diagnosticTerrainT)) {
            RayHit closureHit;
            if (TryBuildResidentMidVoxelClosureHit(rayOrigin, rayDir, diagnosticTerrainT, closureHit) &&
                closureHit.distance < farStartDist + 2200.0f &&
                BackgroundHitAllowedByExactNear(rayOrigin, rayDir, closureHit, BACKGROUND_LAYER_MID_VOXEL)) {
                backgroundHit = closureHit;
                backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
                return true;
            }
        }
    }
    const bool highAltitudeDownwardView = highAltitudeBackgroundView && rayDir.y < -0.35f;
    if (highAltitudeBackgroundView &&
        includeSparseFarField &&
        RaymarchSparseFarField(
            rayOrigin,
            rayDir,
            highAltitudeDownwardView ? max(startDist, 32.0f) : max(startDist, frame.midFieldParams.y),
            1e20f,
            backgroundHit)) {
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, startDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_FAR_SVO;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    if (!voxelTerrainOnly) {
        if (RaymarchMidClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
            backgroundLayer = BACKGROUND_LAYER_MID_HEIGHT;
            if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
                return true;
            }
            backgroundLayer = BACKGROUND_LAYER_NONE;
        }
    }
    // FAR HORIZON CONTINUITY (P1). Beyond the far-handoff distance the only
    // distant terrain owner in voxelTerrainOnly mode was the sparse far SVO,
    // which paints fragmented chunks with sky leaking between them -- a broken
    // horizon. Defer the far SVO for that distant low-angle band so the ray
    // falls through to the continuous far heightfield (the single tail
    // RaymarchFarTerrain call below, now enabled for voxelTerrainOnly). The far
    // SVO still owns higher-angle / nearer rays. Using the existing tail call
    // (rather than a second call site) keeps the shader compilable: a second
    // inlined RaymarchFarTerrain blows up the DXC optimizer, and [noinline]
    // fails DXC validation for the float4-carrying RayHit return.
    //
    // STARTUP-TDR SAFETY: deferral is gated on the mid-voxel ring being
    // substantially resident (midResidencyParams.y voxel coverage ~0.04 at spawn
    // -> ~1.0 once streamed); before then the far SVO keeps the horizon and the
    // far-height tail stays gated off, so no whole-screen startup far march.
    // Widened from (-0.20, 0.12) to (-0.24, 0.22): debug mode 58 at spawn showed
    // the sparse far SVO (blue) painting DETACHED chunks ABOVE the continuous
    // far-height band (orange), specifically the distant mountain-silhouette tips
    // that climb to rayDir.y ~0.12-0.20. Those tips were the dark floating blobs.
    // Extending the band lets the continuous far heightfield own the whole
    // distant silhouette (lower band + tips) so it reads as one coherent ridge
    // instead of fragmented SVO chunks with sky between them.
    const bool deferFarSvoToFarHeightHorizon =
        voxelTerrainOnly &&
        frame.midFieldParams.x > 0.5f &&
        frame.midResidencyParams.y >= 0.5f &&
        frame.midResidencyParams.w >= 1.0f &&
        rayOrigin.y <= 384.0f &&
        rayDir.y > -0.55f &&
        rayDir.y < 0.22f;
    if (!deferFarSvoToFarHeightHorizon &&
        includeSparseFarField && RaymarchSparseFarField(rayOrigin, rayDir, farStartDist, 1e20f, backgroundHit)) {
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, farStartDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_FAR_SVO;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    if (hasWaterOccluder && !deferFarSvoToFarHeightHorizon && !suppressPendingLandWater) {
        backgroundHit = waterOccluderHit;
        backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            if (hasElevatedFarSvoCandidateHit &&
                backgroundHit.distance > max(frame.midFieldParams.y + 512.0f, 1536.0f)) {
                const float frontBias = max(48.0f, min(backgroundHit.distance * 0.035f, 220.0f));
                if (elevatedFarSvoCandidateHit.distance <= backgroundHit.distance + frontBias) {
                    backgroundHit = elevatedFarSvoCandidateHit;
                    backgroundLayer = BACKGROUND_LAYER_FAR_SVO;
                }
            }
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    if (voxelTerrainOnly &&
        hasDeferredMidInteriorHit &&
        RaymarchMidVoxelColumnClipmap(rayOrigin, rayDir, startDist, backgroundHit)) {
        RayHit resolvedWaterHit;
        if (TryResolveWaterOccluderForBackgroundHit(
                rayOrigin, rayDir, startDist, backgroundHit, hasWaterOccluder, waterOccluderHit, resolvedWaterHit,
                suppressPendingLandWater)) {
            backgroundHit = resolvedWaterHit;
            backgroundLayer = BACKGROUND_LAYER_FAR_WATER;
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }
    if (hasDeferredMidInteriorHit) {
        backgroundHit = deferredMidInteriorHit;
        backgroundLayer = BACKGROUND_LAYER_MID_VOXEL;
        return true;
    }
    const bool heightAngleOk = allowWideHeightAngles
        ? (rayDir.y < 0.12f)
        : (rayDir.y > -0.28f && rayDir.y < 0.12f);
    // Mid/far sparse ownership starts are conservative visibility boundaries,
    // not proof that there is no terrain before them. Walking-height rays can
    // cross a real valley wall before the near sphere/box exit; if the fallback
    // starts after that crossing, the public view becomes a ring of sky. Probe
    // from the mid transition for low-altitude continuity while keeping the
    // high-altitude stress path behind the caller-selected boundary.
    const float midStartDist = frame.midFieldParams.x > 0.5f ? frame.midFieldParams.y : 160.0f;
    const float heightContinuityStart = highAltitudeBackgroundView
        ? max(startDist, min(farStartDist, midStartDist))
        : max(64.0f, min(max(startDist, 160.0f), midStartDist));
    // In voxelTerrainOnly mode the only ray that reaches this tail is one the far
    // SVO was deferred for (the distant low-angle horizon band) -- otherwise the
    // function already returned above. Run the continuous far heightfield from
    // the far-handoff distance to fill that band; if it misses (ray clears all
    // distant terrain) the ray is genuine sky. Single far-height call site (no
    // 2nd inline -> shader stays compilable). The far march is step-budget
    // bounded (28-64 steps) and starts thousands of units out, so it is never a
    // whole-screen startup march (and the deferral is gated on mid residency).
    float heightStart;
    bool farHeightAllowed;
    if (voxelTerrainOnly) {
        // High-altitude downward gap fill: every voxel layer (near box, mid
        // ring, far SVO) has already missed by this point. Falling through to
        // sky here turned far-SVO budget/coverage gaps into sky/ocean pits in
        // aerial views; the continuous (now reshaped) far heightfield is the
        // correct floor. Mutually exclusive with the deferral path (which
        // requires rayOrigin.y <= 384). STARTUP-TDR SAFE: the spawn camera is
        // at ground level, so this branch cannot run during the startup
        // whole-screen fallback fill.
        const bool highAltitudeVoxelGapFill =
            highAltitudeBackgroundView && rayDir.y < -0.35f;
        if (!deferFarSvoToFarHeightHorizon && !highAltitudeVoxelGapFill) {
            return false;
        }
        const float voxelFarHandoff = frame.backgroundOwnershipParams.w > 0.5f
            ? max(frame.backgroundOwnershipParams.y, frame.midFieldParams.y + 1.0f)
            : max(frame.midFieldParams.z, frame.midFieldParams.y + 1.0f);
        heightStart = deferFarSvoToFarHeightHorizon
            ? max(farStartDist, voxelFarHandoff)
            : max(startDist, 32.0f);
        // The deferral gate above already bounds rayDir.y < 0.22 for this branch,
        // so let the continuous far heightfield own the full silhouette band
        // (including the mountain tips that heightAngleOk's 0.12 ceiling rejected
        // and which the sparse SVO would otherwise paint as detached dark blobs).
        farHeightAllowed = true;
    } else {
        heightStart = highAltitudeDownwardView
            ? max(startDist, 32.0f)
            : heightContinuityStart;
        // Steep high-altitude rays that miss every voxel layer must land on the
        // continuous far heightfield (now spawn-land reshaped via
        // FarTerrainHeightVoxelized), not fall through to sky: that fall-through
        // painted the pale no-owner halo ring around the near field in aerial
        // views, and turned far-SVO budget/coverage gaps into ocean-colored pits.
        farHeightAllowed = heightAngleOk || highAltitudeDownwardView;
    }
    if (farHeightAllowed && RaymarchFarTerrain(rayOrigin, rayDir, heightStart, backgroundHit)) {
        backgroundLayer = BACKGROUND_LAYER_FAR_HEIGHT;
        if (BackgroundHitAllowedByExactNear(rayOrigin, rayDir, backgroundHit, backgroundLayer)) {
            return true;
        }
        backgroundLayer = BACKGROUND_LAYER_NONE;
    }

    return false;
}

RayHit DebugBackgroundLayerHit(RayHit hit, uint layer) {
    if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_PARENT_HELD) != 0u) {
        RecordRenderLodParentHeld();
    }
    if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK) != 0u) {
        RecordRenderMidInteriorFallback();
    }
    if (layer == BACKGROUND_LAYER_MID_VOXEL) {
        RecordRenderOwnership(RENDER_OWNER_MID_VOXEL);
    } else if (layer == BACKGROUND_LAYER_MID_HEIGHT) {
        RecordRenderOwnership(RENDER_OWNER_MID_HEIGHT);
    } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
        RecordRenderOwnership(RENDER_OWNER_FAR_SVO);
    } else if (layer == BACKGROUND_LAYER_FAR_HEIGHT) {
        RecordRenderOwnership(RENDER_OWNER_FAR_HEIGHT);
    } else if (layer == BACKGROUND_LAYER_FAR_WATER) {
        RecordRenderOwnership(RENDER_OWNER_FAR_WATER);
    } else {
        RecordRenderOwnership(RENDER_OWNER_MISS);
    }
    if (frame.debugMode == 58u) {
        hit.color.rgb = DebugOwnerLayerColor(layer);
        return hit;
    }
    if (frame.debugMode == 61u) {
        hit.color.rgb = layer == BACKGROUND_LAYER_FAR_WATER
            ? float3(0.02f, 0.88f, 1.0f)
            : float3(0.92f, 0.22f, 0.05f);
        return hit;
    }
    if (frame.debugMode == 55u) {
        if (layer == BACKGROUND_LAYER_MID_VOXEL) {
            if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK) != 0u) {
                hit.color.rgb = float3(1.0f, 0.08f, 0.02f);
            } else if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_PARENT_HELD) != 0u) {
                hit.color.rgb = float3(0.86f, 0.18f, 1.0f);
            } else {
                hit.color.rgb = float3(0.05f, 0.95f, 0.25f);
            }
        } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
            hit.color.rgb = float3(0.20f, 0.42f, 1.0f);
        } else if (layer == BACKGROUND_LAYER_FAR_WATER) {
            hit.color.rgb = float3(0.04f, 0.28f, 0.95f);
        } else if (layer == BACKGROUND_LAYER_MID_HEIGHT || layer == BACKGROUND_LAYER_FAR_HEIGHT) {
            hit.color.rgb = float3(1.0f, 0.86f, 0.08f);
        } else {
            hit.color.rgb = float3(0.12f, 0.14f, 0.16f);
        }
        return hit;
    }
    if (frame.debugMode == 68u) {
        if (layer == BACKGROUND_LAYER_MID_VOXEL) {
            if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_CLOSURE) != 0u) {
                hit.color.rgb = float3(1.0f, 0.82f, 0.02f);
            } else if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_COLUMN) != 0u) {
                hit.color.rgb = float3(0.02f, 0.90f, 1.0f);
            } else if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK) != 0u) {
                hit.color.rgb = float3(1.0f, 0.08f, 0.02f);
            } else if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_PARENT_HELD) != 0u) {
                hit.color.rgb = float3(0.86f, 0.18f, 1.0f);
            } else {
                hit.color.rgb = float3(0.05f, 0.95f, 0.25f);
            }
        } else if (layer == BACKGROUND_LAYER_FAR_SVO) {
            hit.color.rgb = float3(0.20f, 0.42f, 1.0f);
        } else if (layer == BACKGROUND_LAYER_FAR_WATER) {
            hit.color.rgb = float3(0.04f, 0.28f, 0.95f);
        } else if (layer == BACKGROUND_LAYER_MID_HEIGHT || layer == BACKGROUND_LAYER_FAR_HEIGHT) {
            hit.color.rgb = float3(1.0f, 0.86f, 0.08f);
        } else {
            hit.color.rgb = float3(0.12f, 0.14f, 0.16f);
        }
        return hit;
    }
    if (frame.debugMode == 56u) {
        if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_INTERIOR_FALLBACK) != 0u) {
            hit.color.rgb = lerp(DebugOwnerMaterialColor(hit.color.rgb, layer), float3(1.0f, 0.08f, 0.02f), 0.45f);
        } else if ((hit.diagnosticFlags & RAY_DIAGNOSTIC_MID_PARENT_HELD) != 0u) {
            hit.color.rgb = lerp(DebugOwnerMaterialColor(hit.color.rgb, layer), float3(0.86f, 0.18f, 1.0f), 0.45f);
        } else {
            hit.color.rgb = DebugOwnerMaterialColor(hit.color.rgb, layer);
        }
        return hit;
    }
    if (CameraUnderwaterForShading() &&
        frame.debugMode != 49u &&
        frame.debugMode != 50u) {
        const float underwaterFog = saturate((hit.distance - 6.0f) / 92.0f);
        const float waterColumn = saturate((hit.distance - 72.0f) / 300.0f);
        const float farLayerBoost =
            (layer == BACKGROUND_LAYER_FAR_HEIGHT || layer == BACKGROUND_LAYER_MID_HEIGHT) ? 0.13f : 0.02f;
        const float fogStrength = saturate(0.30f + underwaterFog * 0.30f + waterColumn * 0.22f + farLayerBoost);
        const float3 waterTint = lerp(float3(0.15f, 0.36f, 0.40f), float3(0.12f, 0.32f, 0.36f), waterColumn);
        hit.color.rgb = lerp(hit.color.rgb, waterTint, fogStrength);
    }
    if (frame.debugMode != 49u &&
        frame.debugMode != 50u &&
        frame.cameraPosition.y <= 384.0f) {
        const bool atmosphericBackground =
            layer == BACKGROUND_LAYER_MID_VOXEL ||
            layer == BACKGROUND_LAYER_MID_HEIGHT ||
            layer == BACKGROUND_LAYER_FAR_SVO ||
            layer == BACKGROUND_LAYER_FAR_HEIGHT;
        if (atmosphericBackground) {
            const float ownershipRadius = max(frame.nearOwnershipParams.w, ExactNearDistance());
            const float nearContext = 1.0f - saturate((hit.distance - ownershipRadius) / 2200.0f);
            const float farContext = saturate((hit.distance - frame.midFieldParams.y) /
                max(frame.midFieldParams.z - frame.midFieldParams.y, 1.0f));
            const float layerWeight =
                (layer == BACKGROUND_LAYER_FAR_SVO || layer == BACKGROUND_LAYER_FAR_HEIGHT) ? 0.24f : 0.18f;
            const float atmosphere = saturate(nearContext * layerWeight + farContext * 0.10f);
            const float3 contextSky = SkyColor(float3(0.0f, -0.06f, 0.998f));
            hit.color.rgb = lerp(hit.color.rgb, contextSky, atmosphere);
        }
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
    } else if (layer == BACKGROUND_LAYER_FAR_WATER) {
        tint = float3(0.05f, 0.32f, 1.0f);
    }
    if (frame.debugMode == 50u) {
        hit.color.rgb = tint;
    } else {
        hit.color.rgb = lerp(hit.color.rgb, tint, 0.58f);
    }
    return hit;
}

RayHit DebugBackgroundLayerHitWithExactFeedback(
    float3 rayOrigin,
    float3 rayDir,
    RayHit hit,
    uint layer)
{
    if (frame.debugMode == 70u) {
        return DebugWaterlineResolverReasonHit(rayOrigin, rayDir, hit, layer);
    }
    RayHit exactHit;
    if (ResolveExactSparseSurfaceBeforeBackground(rayOrigin, rayDir, hit, layer, exactHit)) {
        RecordRenderOwnership(RENDER_OWNER_SURFACE);
        return exactHit;
    }
    RayHit deterministicWaterHit;
    if (TryResolveDeterministicWaterBeforeBackground(
            rayOrigin,
            rayDir,
            hit,
            layer,
            deterministicWaterHit)) {
        RecordRenderOwnership(RENDER_OWNER_FAR_WATER);
        return DebugBackgroundLayerHit(deterministicWaterHit, BACKGROUND_LAYER_FAR_WATER);
    }
    RecordHiddenExactFallbackSampleForBackgroundHit(rayOrigin, rayDir, hit, layer);
    return DebugBackgroundLayerHit(hit, layer);
}

bool DiagnosticFarTerrainWouldHit(float3 rayOrigin, float3 rayDir, float startDist, out float hitT) {
    hitT = 1e20f;
    if (!VoxelTerrainOnly()) {
        return false;
    }
    if (rayDir.y > 0.42f || rayDir.y < -0.92f) {
        return false;
    }

    const float farMaxDist = 10400.0f;
    float t = max(startDist, 160.0f);
    float mountainMask, spireMask, ravineMask;
    float3 previousPos = rayOrigin + rayDir * t;
    float previousHeight = FarTerrainHeight(previousPos.xz, mountainMask, spireMask, ravineMask);
    float previousSigned = previousPos.y - previousHeight;
    float previousT = t;

    if (previousSigned <= 0.0f) {
        float originMountainMask, originSpireMask, originRavineMask;
        const float originHeight = FarTerrainHeight(
            rayOrigin.xz,
            originMountainMask,
            originSpireMask,
            originRavineMask);
        if (rayOrigin.y > originHeight) {
            hitT = t;
            return true;
        }
    }

    [loop]
    for (int i = 0; i < 112 && t < farMaxDist; ++i) {
        const bool nearSkylineProbe =
            rayOrigin.y <= 384.0f &&
            rayDir.y > -0.06f &&
            rayDir.y < 0.24f &&
            t < 3600.0f;
        const float distanceStep = nearSkylineProbe
            ? 36.0f
            : lerp(56.0f, 220.0f, saturate(t / farMaxDist));
        float stepSize = distanceStep;
        if (previousSigned > 0.0f && rayDir.y < -0.015f) {
            const float verticalStep = previousSigned / max(-rayDir.y, 0.030f);
            stepSize = clamp(verticalStep * 0.50f, nearSkylineProbe ? 12.0f : 24.0f, distanceStep);
        }
        t += stepSize;

        float3 pos = rayOrigin + rayDir * t;
        const float height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        const float signedDistance = pos.y - height;
        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            hitT = t;
            return true;
        }
        previousSigned = signedDistance;
        previousT = t;
    }
    return false;
}

bool BuildDeterministicFarTerrainContinuityHit(
    float3 rayOrigin,
    float3 rayDir,
    float diagnosticTerrainT,
    out RayHit continuityHit)
{
    continuityHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    if (!VoxelTerrainOnly() || diagnosticTerrainT >= 1e19f) {
        return false;
    }

    const float hitT = max(diagnosticTerrainT, 32.0f);
    if (hitT > 13312.0f) {
        return false;
    }

    const float3 hitPos = rayOrigin + rayDir * hitT;
    float mountainMask;
    float spireMask;
    float ravineMask;
    const float rawContinuityHeight = FarTerrainHeight(hitPos.xz, mountainMask, spireMask, ravineMask);
    // Spawn-land agreement: FarTerrainMaterial classifies the height it is
    // GIVEN, so this raw-height caller applies the reshape itself.
    const float height = FarSpawnLandReshapeHeight(hitPos.xz, rawContinuityHeight, mountainMask);

    const uint material = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
    if (material == MAT_AIR) {
        return false;
    }

    if (frame.debugMode == 54u || frame.debugMode == 56u) {
        continuityHit = MakeHit(float4(DebugMaterialColor(material), 1.0f), hitT);
        return true;
    }
    if (frame.debugMode == 59u) {
        const float cellShade = saturate(FarFallbackCellSize(hitT) / 160.0f);
        continuityHit = MakeHit(float4(0.34f, cellShade, 1.0f - cellShade * 0.55f, 1.0f), hitT);
        return true;
    }
    if (frame.debugMode == 62u) {
        continuityHit = MakeHit(float4(DebugClosureColor(hitPos), 1.0f), hitT);
        return true;
    }

    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
    baseColor.rgb = FarTerrainMaterialVariation(baseColor.rgb, material, hitPos.xz, hitPos.y, hitT);
    // PALETTE UNIFICATION (altitude patchwork): shade with the EXACT mid-voxel
    // constants, matching the far-height / far-SVO sites (the old extra-lifted
    // *0.90+0.46 / floor 0.70+0.070 was the brightest far variant and read as
    // a washed pale band against mid voxels).
    // NOTE: no xz quantization here - driver-JIT fragile, see
    // FarSvoInteriorLeafSurfaceRecovery.
    const float3 normal = DistantLodShadeNormal(FarTerrainNormal(hitPos.xz), hitT, 0.34f);
    const float ndotl = saturate(dot(normal, SkySunDirection()) * 0.62f + 0.28f);
    float3 color = baseColor.rgb * (SkyAmbient(normal) * 0.76f + ndotl * 0.34f);
    color = max(color, baseColor.rgb * 0.54f + 0.045f);
    const float gridFade = saturate((hitT - 1200.0f) / 6200.0f);
    const float grid = VoxelGridLine(hitPos.xz, FarFallbackCellSize(hitT), lerp(0.030f, 0.016f, gridFade));
    color *= lerp(1.0f, 0.970f, grid);
    const float fogFactor = saturate((hitT - 900.0f) / (10400.0f - 900.0f));
    const float horizonHaze = saturate((0.20f - abs(rayDir.y)) / 0.20f);
    if (frame.debugMode == 60u) {
        const float debugFog = saturate(fogFactor * 0.42f + horizonHaze * 0.14f);
        continuityHit = MakeHit(float4(float3(debugFog, debugFog, debugFog), 1.0f), hitT);
        return true;
    }
    color = lerp(
        color,
        SkyColor(rayDir),
        (fogFactor * 0.54f + horizonHaze * 0.32f + 0.12f) * FarHazeDowncastScale(rayDir.y));
    continuityHit = MakeHit(float4(color, 1.0f), hitT);
    return true;
}

bool DiagnosticFarTerrainSampledRangeHit(
    float3 rayOrigin,
    float3 rayDir,
    float startDist,
    float endDist,
    out float hitT)
{
    hitT = 1e20f;
    if (!VoxelTerrainOnly()) {
        return false;
    }
    if (rayDir.y > 0.42f || rayDir.y < -0.92f) {
        return false;
    }

    const float rangeStart = max(startDist, 160.0f);
    const float rangeEnd = min(max(endDist, rangeStart + 1.0f), 10400.0f);
    if (rangeEnd <= rangeStart + 1.0f) {
        return false;
    }

    [unroll]
    for (int i = 0; i < 6; ++i) {
        const float u = ((float)i + 1.0f) / 7.0f;
        const float t = lerp(rangeStart, rangeEnd, u);
        const float3 pos = rayOrigin + rayDir * t;
        float mountainMask, spireMask, ravineMask;
        const float height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        if (pos.y <= height - 0.5f) {
            hitT = t;
            return true;
        }
    }
    return false;
}

static const uint TERRAIN_SKY_REASON_LEGITIMATE_SKY = 0u;
static const uint TERRAIN_SKY_REASON_BEFORE_ALLOWED_START = 1u;
static const uint TERRAIN_SKY_REASON_MID_BRICK_MISSING = 2u;
static const uint TERRAIN_SKY_REASON_MID_SAMPLED_AIR = 3u;
static const uint TERRAIN_SKY_REASON_MID_REJECTED = 4u;
static const uint TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED = 5u;

float TerrainDiagnosticStartDistance() {
    const float midStart = frame.midFieldParams.x > 0.5f
        ? max(frame.midFieldParams.y, 1.0f)
        : 160.0f;
    return max(160.0f, min(midStart, ExactNearDistance() + 8.0f));
}

bool TryBuildResidentMidVoxelClosureHit(
    float3 rayOrigin,
    float3 rayDir,
    float terrainT,
    out RayHit closureHit)
{
    closureHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    if (terrainT >= 1e19f || frame.midFieldParams.x < 0.5f) {
        return false;
    }

    uint4 header = MidVoxelClipmapMetadata[0];
    const uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u || ringCount == 0u) {
        return false;
    }

    const float startDistance = max(frame.midFieldParams.y, 1.0f);
    const float endDistance = max(frame.midFieldParams.z, startDistance + 1.0f);
    const bool highAltitudePreMidClosure =
        rayOrigin.y > 384.0f &&
        rayDir.y > -0.92f &&
        rayDir.y <= 0.42f;
    // In broad/free-camera views the exact sparse surface is a detail layer,
    // not the only valid foreground owner. If exact misses a downward ray
    // before midStart, resident ring0 mid voxels should still be allowed to
    // carry the public pixel. Rejecting these pre-mid crossings forces the
    // analytic far-height continuity fallback even when voxel data is resident.
    const float minClosureDistance = highAltitudePreMidClosure
        ? 32.0f
        : startDistance;
    if (terrainT < minClosureDistance || terrainT > endDistance) {
        return false;
    }
    const bool allowCoarserParentFallback =
        highAltitudePreMidClosure ||
        (rayOrigin.y <= 384.0f &&
         rayDir.y > -0.58f &&
         rayDir.y < 0.18f);

    uint voxel = PackVoxel(MAT_AIR, 0, 0, 0);
    uint actualRing = 0u;
    float actualCellSize = MidClipmapRingCellSize(0u);
    uint material = MAT_AIR;
    uint closurePreferredRing = 0u;
    float closureT = terrainT;
    float3 hitPos = rayOrigin + rayDir * closureT;
    float ringDistance = max(closureT, startDistance);
    const uint probeCount = highAltitudePreMidClosure ? 4u : 1u;
    bool foundResidentSolid = false;

    [loop]
    for (uint probeIndex = 0u; probeIndex < 4u; ++probeIndex) {
        if (probeIndex >= probeCount) {
            break;
        }
        const float probeT = min(terrainT + (float)probeIndex * 12.0f, endDistance);
        const float probeRingDistance = max(probeT, startDistance);
        const uint preferredRing = min(
            (uint)floor(saturate((probeRingDistance - startDistance) / max(endDistance - startDistance, 1.0f)) *
                (float)ringCount),
            ringCount - 1u);
        const float3 probePos = rayOrigin + rayDir * probeT;
        uint probeVoxel;
        uint probeActualRing;
        float probeActualCellSize;
        if (!SampleResidentMidVoxelFallback(
                probePos,
                preferredRing,
                allowCoarserParentFallback,
                probeVoxel,
                probeActualRing,
                probeActualCellSize)) {
            continue;
        }
        const uint probeMaterial = GetMaterial(probeVoxel);
        if (probeMaterial == MAT_AIR) {
            continue;
        }
        voxel = probeVoxel;
        actualRing = probeActualRing;
        actualCellSize = probeActualCellSize;
        material = probeMaterial;
        closurePreferredRing = preferredRing;
        closureT = probeT;
        hitPos = probePos;
        ringDistance = probeRingDistance;
        foundResidentSolid = true;
        break;
    }

    if (!foundResidentSolid) {
        return false;
    }

    float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, (material + 0.5f) / 256.0f, 0);
    baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, 1.0f, 0.58f);

    const float3 shadeNormal = float3(0.0f, 1.0f, 0.0f);
    const float ndotl = saturate(dot(shadeNormal, SkySunDirection()) * 0.52f + 0.34f);
    float3 color = baseColor.rgb * (SkyAmbient(shadeNormal) * 0.78f + ndotl * 0.30f);
    color = max(color, baseColor.rgb * 0.54f + 0.045f);
    const float fogFactor = saturate((ringDistance - startDistance) / max(endDistance - startDistance, 1.0f));
    color = lerp(color, SkyColor(rayDir), fogFactor * 0.50f);

    closureHit = MakeHit(float4(color, baseColor.a), closureT);
    closureHit.diagnosticFlags |= RAY_DIAGNOSTIC_MID_CLOSURE;
    if (actualRing > closurePreferredRing) {
        closureHit.diagnosticFlags |= RAY_DIAGNOSTIC_MID_PARENT_HELD;
    }
    return true;
}

uint DiagnoseTerrainSkyReason(
    float3 rayOrigin,
    float3 rayDir,
    float backgroundAllowedStart,
    out float diagnosticTerrainT)
{
    diagnosticTerrainT = 1e20f;
    const float terrainDiagnosticStart = TerrainDiagnosticStartDistance();
    const bool hasBeforeAllowedTerrain =
        backgroundAllowedStart > terrainDiagnosticStart + 8.0f &&
        DiagnosticFarTerrainSampledRangeHit(
            rayOrigin,
            rayDir,
            terrainDiagnosticStart,
            backgroundAllowedStart,
            diagnosticTerrainT);
    if (hasBeforeAllowedTerrain) {
        return TERRAIN_SKY_REASON_BEFORE_ALLOWED_START;
    }
    if (!DiagnosticFarTerrainSampledRangeHit(
            rayOrigin,
            rayDir,
            backgroundAllowedStart,
            backgroundAllowedStart + 3200.0f,
            diagnosticTerrainT)) {
        return TERRAIN_SKY_REASON_LEGITIMATE_SKY;
    }

    if (frame.midFieldParams.x < 0.5f) {
        return TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED;
    }

    uint4 header = MidVoxelClipmapMetadata[0];
    const uint ringCount = min(header.w >> 24u, MID_CLIPMAP_MAX_SHADER_RINGS);
    if (header.x != MID_VOXEL_CLIPMAP_MAGIC || header.z == 0u || ringCount == 0u) {
        return TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED;
    }

    const float midStart = max(frame.midFieldParams.y, 1.0f);
    const float midEnd = max(frame.midFieldParams.z, midStart + 1.0f);
    if (diagnosticTerrainT < midStart || diagnosticTerrainT > midEnd) {
        return TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED;
    }

    const uint preferredRing = min(
        (uint)floor(saturate((diagnosticTerrainT - midStart) / max(midEnd - midStart, 1.0f)) *
            (float)ringCount),
        ringCount - 1u);
    const float3 diagnosticPos = rayOrigin + rayDir * diagnosticTerrainT;
    uint diagnosticVoxel;
    uint actualRing;
    float actualCellSize;
    const bool allowCoarserParentFallback =
        rayOrigin.y <= 384.0f &&
        rayDir.y > -0.58f &&
        rayDir.y < 0.18f;
    if (!SampleResidentMidVoxelFallback(
            diagnosticPos,
            preferredRing,
            allowCoarserParentFallback,
            diagnosticVoxel,
            actualRing,
            actualCellSize)) {
        return TERRAIN_SKY_REASON_MID_BRICK_MISSING;
    }
    if (actualRing > preferredRing) {
        return TERRAIN_SKY_REASON_MID_REJECTED;
    }
    if (GetMaterial(diagnosticVoxel) == MAT_AIR) {
        return TERRAIN_SKY_REASON_MID_SAMPLED_AIR;
    }

    RayHit diagnosticHit = MakeHit(float4(1.0f, 1.0f, 1.0f, 1.0f), diagnosticTerrainT);
    if (!BackgroundHitAllowedByExactNear(rayOrigin, rayDir, diagnosticHit, BACKGROUND_LAYER_MID_VOXEL)) {
        return TERRAIN_SKY_REASON_MID_REJECTED;
    }

    return TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED;
}

float3 TerrainSkyReasonColor(uint reason) {
    if (reason == TERRAIN_SKY_REASON_BEFORE_ALLOWED_START) {
        return float3(1.0f, 0.05f, 0.02f);
    }
    if (reason == TERRAIN_SKY_REASON_MID_BRICK_MISSING) {
        return float3(1.0f, 0.58f, 0.04f);
    }
    if (reason == TERRAIN_SKY_REASON_MID_SAMPLED_AIR) {
        return float3(0.02f, 0.86f, 1.0f);
    }
    if (reason == TERRAIN_SKY_REASON_MID_REJECTED) {
        return float3(1.0f, 0.02f, 0.78f);
    }
    if (reason == TERRAIN_SKY_REASON_FAR_SVO_UNAVAILABLE_OR_REJECTED) {
        return float3(0.48f, 0.16f, 1.0f);
    }
    return float3(0.18f, 0.42f, 0.95f);
}

RayHit DebugBackgroundMissHit(
    float3 rayOrigin,
    float3 rayDir,
    float startDist,
    bool nearSparseHole,
    bool runTerrainSkyReasonDebug)
{
    if (frame.cameraPosition.y < FAR_WATER_SURFACE_Y - 1.0f && rayDir.y > 0.015f) {
        RecordRenderOwnership(RENDER_OWNER_SKY);
        if (frame.debugMode == 50u) {
            return MakeHit(float4(0.18f, 0.42f, 0.95f, 1.0f), 1e20f);
        }
        const float surfaceBlend = saturate((rayDir.y - 0.015f) / 0.42f);
        float3 skyThroughWater = SkyColor(rayDir);
        const float3 shallowWaterTint = float3(0.10f, 0.28f, 0.33f);
        const float3 surfaceHaze = float3(0.19f, 0.41f, 0.46f);
        const float3 waterVolume = lerp(shallowWaterTint, surfaceHaze, surfaceBlend * 0.42f);
        skyThroughWater = lerp(skyThroughWater, waterVolume, 0.76f + surfaceBlend * 0.16f);
        skyThroughWater = min(skyThroughWater, float3(0.30f, 0.53f, 0.58f));
        return MakeHit(float4(skyThroughWater, 1.0f), 1e20f);
    }

    const bool voxelTerrainOnly = VoxelTerrainOnly();
    const bool highAltitudeHorizonSky =
        frame.cameraPosition.y > 384.0f && rayDir.y > -0.16f;
    const bool lowAltitudeVoxelMountainSilhouetteRay =
        voxelTerrainOnly && frame.cameraPosition.y <= 384.0f;
    const bool expectedSky =
        (lowAltitudeVoxelMountainSilhouetteRay ? (rayDir.y > 0.36f) : (rayDir.y > -0.01f)) ||
        highAltitudeHorizonSky;
    if (frame.debugMode == 57u && runTerrainSkyReasonDebug) {
        float reasonTerrainT = 1e20f;
        const uint reason = DiagnoseTerrainSkyReason(rayOrigin, rayDir, startDist, reasonTerrainT);
        RecordRenderOwnership(reason == TERRAIN_SKY_REASON_LEGITIMATE_SKY
            ? RENDER_OWNER_SKY
            : RENDER_OWNER_MISS);
        if (reason != TERRAIN_SKY_REASON_LEGITIMATE_SKY) {
            const float3 diagnosticPos = rayOrigin + rayDir * reasonTerrainT;
            const int3 diagnosticBrick = int3(
                FloorDiv16((int)floor(diagnosticPos.x)),
                FloorDiv16((int)floor(diagnosticPos.y)),
                FloorDiv16((int)floor(diagnosticPos.z)));
            RecordUnsafeSparseMissSample(diagnosticBrick, reasonTerrainT);
        }
        return MakeHit(float4(TerrainSkyReasonColor(reason), 1.0f), reasonTerrainT);
    }
    float diagnosticTerrainT = 1e20f;
    const bool hiddenVoxelTerrainMiss =
        DiagnosticFarTerrainWouldHit(rayOrigin, rayDir, startDist, diagnosticTerrainT);
    const bool underwaterVolume =
        CameraUnderwaterForShading() &&
        !expectedSky;
    const bool hiddenNearVoxelTerrainMiss =
        hiddenVoxelTerrainMiss &&
        nearSparseHole &&
        diagnosticTerrainT <= max(ExactNearDistance(), 224.0f) + 160.0f &&
        !underwaterVolume;
    const bool cleanExactSparseAir =
        hiddenVoxelTerrainMiss &&
        !nearSparseHole &&
        diagnosticTerrainT <= max(ExactNearDistance(), 224.0f) + 160.0f &&
        !underwaterVolume;
    const bool backgroundHiddenTerrainMiss =
        hiddenVoxelTerrainMiss &&
        !cleanExactSparseAir;
    const bool terrainFacingSparseMiss =
        (nearSparseHole || hiddenNearVoxelTerrainMiss) &&
        !underwaterVolume &&
        rayDir.y > -0.88f;
    const bool valleyAtmosphere =
        !voxelTerrainOnly &&
        !underwaterVolume &&
        !terrainFacingSparseMiss &&
        !expectedSky &&
        rayDir.y > -0.88f;
    const bool voxelOnlyAir =
        voxelTerrainOnly &&
        !underwaterVolume &&
        !terrainFacingSparseMiss;
    if (backgroundHiddenTerrainMiss && frame.debugMode != 50u) {
        RayHit closureHit;
        if (TryBuildResidentMidVoxelClosureHit(rayOrigin, rayDir, diagnosticTerrainT, closureHit)) {
            RecordRenderOwnership(RENDER_OWNER_MID_VOXEL);
            if ((closureHit.diagnosticFlags & RAY_DIAGNOSTIC_MID_PARENT_HELD) != 0u) {
                RecordRenderLodParentHeld();
            }
            return closureHit;
        }
        RayHit continuityHit;
        if (BuildDeterministicFarTerrainContinuityHit(rayOrigin, rayDir, diagnosticTerrainT, continuityHit)) {
            RecordFarHeightContinuityReason(rayOrigin, rayDir, diagnosticTerrainT);
            RecordHiddenExactFallbackSampleForBackgroundHit(
                rayOrigin,
                rayDir,
                continuityHit,
                BACKGROUND_LAYER_FAR_HEIGHT);
            return DebugBackgroundLayerHit(continuityHit, BACKGROUND_LAYER_FAR_HEIGHT);
        }
    }
    RecordRenderOwnership(terrainFacingSparseMiss
        ? RENDER_OWNER_UNSAFE_NEAR_MISS
        : (backgroundHiddenTerrainMiss
            ? RENDER_OWNER_MISS
            : ((expectedSky || voxelOnlyAir)
                ? RENDER_OWNER_SKY
                : (underwaterVolume
                    ? RENDER_OWNER_WATER_CONTEXT
                    : (valleyAtmosphere ? RENDER_OWNER_VALLEY_ATMOSPHERE : RENDER_OWNER_MISS)))));
    if (frame.debugMode == 58u) {
        if (terrainFacingSparseMiss || backgroundHiddenTerrainMiss) {
            return MakeHit(float4(1.0f, 0.05f, 0.02f, 1.0f), 1e20f);
        }
        if (underwaterVolume) {
            return MakeHit(float4(0.02f, 0.78f, 1.0f, 1.0f), 1e20f);
        }
        if (valleyAtmosphere) {
            return MakeHit(float4(0.58f, 0.70f, 0.72f, 1.0f), 1e20f);
        }
        return MakeHit(float4(0.02f, 0.05f, 0.18f, 1.0f), 1e20f);
    }
    if (frame.debugMode == 61u) {
        return MakeHit(float4(0.04f, 0.05f, 0.08f, 1.0f), 1e20f);
    }
    if (hiddenNearVoxelTerrainMiss || (frame.debugMode == 50u && backgroundHiddenTerrainMiss)) {
        const float3 diagnosticPos = rayOrigin + rayDir * diagnosticTerrainT;
        const int3 diagnosticBrick = int3(
            FloorDiv16((int)floor(diagnosticPos.x)),
            FloorDiv16((int)floor(diagnosticPos.y)),
            FloorDiv16((int)floor(diagnosticPos.z)));
        RecordUnsafeSparseMissSample(diagnosticBrick, diagnosticTerrainT);
    }
    if (frame.debugMode == 50u) {
        if (hiddenNearVoxelTerrainMiss) {
            return MakeHit(float4(1.0f, 0.02f, 0.75f, 1.0f), diagnosticTerrainT);
        }
        if (backgroundHiddenTerrainMiss) {
            const float midStart = max(frame.midFieldParams.y, 1.0f);
            const float midEnd = max(frame.midFieldParams.z, midStart + 1.0f);
            uint4 midVoxelHeader = MidVoxelClipmapMetadata[0];
            const uint midVoxelRingCount = max(midVoxelHeader.w >> 24u, 1u);
            const uint diagnosticRing = min(
                (uint)floor(saturate((diagnosticTerrainT - midStart) / max(midEnd - midStart, 1.0f)) *
                    (float)midVoxelRingCount),
                midVoxelRingCount - 1u);
            uint diagnosticVoxel;
            uint diagnosticActualRing;
            float diagnosticCellSize;
            if (SampleResidentMidVoxelFallback(
                    rayOrigin + rayDir * diagnosticTerrainT,
                    diagnosticRing,
                    false,
                    diagnosticVoxel,
                    diagnosticActualRing,
                    diagnosticCellSize)) {
                const uint diagnosticMaterial = GetMaterial(diagnosticVoxel);
                if (diagnosticMaterial != MAT_AIR) {
                    float3 diagnosticNormal;
                    const bool diagnosticTaggedSurface =
                        IsResidentMidVoxelTaggedSurface(diagnosticVoxel);
                    const bool diagnosticExposedSurface =
                        IsResidentMidVoxelExposed(
                            rayOrigin + rayDir * diagnosticTerrainT,
                            diagnosticActualRing,
                            diagnosticCellSize,
                            diagnosticTerrainT,
                            diagnosticNormal);
                    if (diagnosticTaggedSurface) {
                        return MakeHit(float4(0.05f, 1.0f, 0.10f, 1.0f), diagnosticTerrainT);
                    }
                    if (diagnosticExposedSurface) {
                        return MakeHit(float4(0.00f, 1.0f, 1.0f, 1.0f), diagnosticTerrainT);
                    }
                    return MakeHit(float4(1.0f, 0.46f, 0.0f, 1.0f), diagnosticTerrainT);
                }
                return MakeHit(float4(0.35f, 0.0f, 1.0f, 1.0f), diagnosticTerrainT);
            }
            return MakeHit(float4(1.0f, 0.05f, 0.02f, 1.0f), diagnosticTerrainT);
        }
        if (expectedSky || voxelOnlyAir) {
            return MakeHit(float4(0.18f, 0.42f, 0.95f, 1.0f), 1e20f);
        }
        if (terrainFacingSparseMiss) {
            return MakeHit(float4(1.0f, 0.02f, 0.75f, 1.0f), 1e20f);
        }
        if (underwaterVolume) {
            return MakeHit(float4(0.05f, 0.32f, 1.0f, 1.0f), 1e20f);
        }
        if (valleyAtmosphere) {
            return MakeHit(float4(0.58f, 0.70f, 0.72f, 1.0f), 1e20f);
        }
        // Pure red means the background ownership chain found no resident
        // mid/far layer for this pixel. This is intentionally harsh: it makes
        // clipmap residency gaps and fallback suppression visible in screenshots.
        // Upward sky rays are shown blue instead, so real sky is not confused
        // with missing terrain ownership.
        return MakeHit(float4(1.0f, 0.05f, 0.02f, 1.0f), 1e20f);
    }
    float3 color = SkyColor(rayDir);
    if (terrainFacingSparseMiss) {
        const float terrainGapFog = saturate((0.08f - rayDir.y) / 0.54f);
        color = lerp(float3(0.30f, 0.24f, 0.16f), float3(0.50f, 0.40f, 0.26f), terrainGapFog);
    }
    if (underwaterVolume) {
        const float depth = saturate((FAR_SEA_LEVEL - frame.cameraPosition.y) / 96.0f);
        const float volumeFog = saturate((0.12f - rayDir.y) / 0.72f);
        const float3 deepWater = float3(0.08f, 0.25f, 0.31f);
        const float3 litWater = float3(0.18f, 0.40f, 0.45f);
        color = lerp(color, lerp(litWater, deepWater, depth), 0.56f + volumeFog * 0.26f);
    }
    if (valleyAtmosphere) {
        const float valleyFog = saturate((0.04f - rayDir.y) / 0.42f);
        const float3 airColor = lerp(SkyColor(rayDir), float3(0.56f, 0.66f, 0.70f), 0.26f);
        color = lerp(color, airColor, 0.12f + valleyFog * 0.16f);
    }
    return MakeHit(float4(color, 1.0f), 1e20f);
}

bool SurfaceAuthoritativeNearTerrainMiss(float3 rayOrigin, float3 rayDir, float backgroundStart) {
    if (rayOrigin.y > 384.0f || rayDir.y >= -0.005f || rayDir.y <= -0.88f) {
        return false;
    }

    const float exactHoleDistance = max(ExactNearDistance(), 224.0f);
    const float foregroundLimit = min(
        max(exactHoleDistance + 96.0f, 384.0f),
        min(max(backgroundStart + 64.0f, 384.0f), 1024.0f));
    [loop]
    for (float terrainT = 8.0f; terrainT <= foregroundLimit; terrainT += 16.0f) {
        const float3 terrainProbe = rayOrigin + rayDir * terrainT;
        float mountainMask;
        float spireMask;
        float ravineMask;
        const float terrainHeight = FarTerrainHeight(
            terrainProbe.xz,
            mountainMask,
            spireMask,
            ravineMask);
        if (terrainProbe.y <= terrainHeight - 1.0f) {
            return terrainT < exactHoleDistance + 160.0f;
        }
    }
    return false;
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
RayHit Raymarch(float3 rayOrigin, float3 rayDir, bool runTerrainSkyReasonDebug) {
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
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, backgroundHit, backgroundLayer);
        }
        // A miss behind the surface-authoritative foreground is only an
        // unsafe near hole when the ray actually crosses expected foreground
        // terrain. Otherwise this path is ordinary sky/valley ownership and
        // should not look like a sparse residency failure.
        const bool nearSurfaceTerrainMiss =
            SurfaceAuthoritativeNearTerrainMiss(rayOrigin, rayDir, backgroundStart);
        return DebugBackgroundMissHit(
            rayOrigin,
            rayDir,
            backgroundStart,
            nearSurfaceTerrainMiss,
            runTerrainSkyReasonDebug);
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
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, farHit, farLayer);
        }
        return DebugBackgroundMissHit(rayOrigin, rayDir, 32.0f, false, runTerrainSkyReasonDebug);
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
    int3 firstSparseMissingBrickCoord = int3(0, 0, 0);
    bool firstSparseMissingTerrainAdjacent = false;
    bool firstSparseMissingLandAboveSea = false;
    bool sawLocalWater = false;
    RayHit nearWaterPlaneHit;
    const bool aboveWaterView = rayOrigin.y >= FAR_WATER_SURFACE_Y - 0.5f;
    const bool hasNearWaterPlane =
        aboveWaterView &&
        RaymarchFarWater(rayOrigin, rayDir, 32.0f, nearWaterPlaneHit);

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
            if (!sawSparseMissing) {
                float3 firstMissingPos = startPos + rayDir * dist;
                firstSparseMissingBrickCoord = int3(
                    FloorDiv16(voxelPos.x),
                    FloorDiv16(voxelPos.y),
                    FloorDiv16(voxelPos.z));
                float mountainMask;
                float spireMask;
                float ravineMask;
                const float firstMissingTerrainHeight = FarTerrainHeight(
                    firstMissingPos.xz,
                    mountainMask,
                    spireMask,
                    ravineMask);
                // TANDEM (Codex diagnosis + shape, Claude applied): the RAW
                // FarTerrainHeight misclassifies reshaped spawn-land continent
                // columns (raw below sea) as below-sea, so the near-water plane
                // AND the background-fill suppression both let FAR_WATER leak over
                // the continent at altitude (ground views never reach these
                // fallback paths, so ground is clean). This ONE source flag feeds
                // both visible water paths. CHEAP lower-bound reshape (no noise:
                // the spawn-land floor is always >= SEA+35 wherever the band is
                // significant) - one branchless FarSpawnLandBand + lerp/max, once
                // per ray. The full noise reshape (FarSpawnLandFloorBase x8) here
                // crashed the driver JIT (598x DEVICE_REMOVED); this is driver-safe.
                const float firstMissingSpawnBand =
                    FarSpawnLandBand(firstMissingPos.xz);
                const float cheapReshapedLandHeight = lerp(
                    firstMissingTerrainHeight,
                    max(firstMissingTerrainHeight, FAR_SEA_LEVEL + 35.0f),
                    firstMissingSpawnBand);
                firstSparseMissingTerrainAdjacent =
                    firstMissingPos.y <= cheapReshapedLandHeight + 24.0f &&
                    firstMissingPos.y >= cheapReshapedLandHeight - 48.0f;
                firstSparseMissingLandAboveSea =
                    cheapReshapedLandHeight > FAR_SEA_LEVEL + 4.0f;
            }
            sawSparseMissing = true;
            firstSparseMissingDist = min(firstSparseMissingDist, dist);
            float3 currentPos = startPos + rayDir * dist;
            float skipDist = DistanceToSparseBrickExit(voxelPos, currentPos, rayDir);
            dist += skipDist;
            if (dist > maxMarchDist) break;

            RestartSparseDdaAtDistance(startPos, rayDir, deltaDist, dist, voxelPos, sideDist);
            continue;
        }

        const bool skipLocalWater =
            material == MAT_WATER &&
            (aboveWaterView || rayOrigin.y < FAR_SEA_LEVEL + 8.0f);
        sawLocalWater = sawLocalWater || skipLocalWater;

        // Hit non-air voxel?
        if (material != MAT_AIR && !skipLocalWater) {
            const float hitWorldDistance = entryDist + dist;
            // WALKING-HOLES FIX: a pending (not yet streamed) dry-land brick in
            // front of the analytic sea-plane crossing means this ray tunneled
            // through a streaming hole; the water plane must not preempt the
            // real voxel hit behind it, or land holes flash navy while pages
            // promote at the walk leading edge.
            const bool pendingLandBeforeNearWater =
                sawSparseMissing &&
                firstSparseMissingTerrainAdjacent &&
                firstSparseMissingLandAboveSea &&
                (entryDist + firstSparseMissingDist) < nearWaterPlaneHit.distance;
            if (hasNearWaterPlane &&
                !pendingLandBeforeNearWater &&
                nearWaterPlaneHit.distance <= hitWorldDistance + 0.25f) {
                // The water plane can be closer than the next voxel-DDA hit
                // while a raster/extracted exact surface face still exists
                // before the plane. Route this through the same exact-surface
                // resolver used by background layers so shoreline terrain is
                // not hidden by water just because the voxel DDA steps past it.
                return DebugBackgroundLayerHitWithExactFeedback(
                    rayOrigin,
                    rayDir,
                    nearWaterPlaneHit,
                    BACKGROUND_LAYER_FAR_WATER);
            }
            // Sample material color from palette
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);
            if (frame.debugMode == 54u || frame.debugMode == 56u) {
                RecordRenderOwnership(RENDER_OWNER_NEAR);
                return MakeHit(float4(DebugMaterialColor(material), 1.0f), entryDist + dist);
            }
            const float3 hitPos = startPos + rayDir * dist;
            if (frame.debugMode == 58u) {
                RecordRenderOwnership(RENDER_OWNER_NEAR);
                return MakeHit(float4(1.0f, 0.95f, 0.05f, 1.0f), entryDist + dist);
            }
            if (frame.debugMode == 59u) {
                RecordRenderOwnership(RENDER_OWNER_NEAR);
                const float nearBand = 1.0f - saturate((entryDist + dist) / max(ExactNearDistance(), 1.0f));
                return MakeHit(float4(1.0f, nearBand, 0.0f, 1.0f), entryDist + dist);
            }
            if (frame.debugMode == 61u) {
                RecordRenderOwnership(RENDER_OWNER_NEAR);
                const float3 waterDebug = material == MAT_WATER
                    ? float3(0.02f, 0.88f, 1.0f)
                    : float3(0.12f, 0.08f, 0.05f);
                return MakeHit(float4(waterDebug, 1.0f), entryDist + dist);
            }
            if (frame.debugMode == 62u) {
                RecordRenderOwnership(RENDER_OWNER_NEAR);
                return MakeHit(float4(DebugClosureColor(hitPos), 1.0f), entryDist + dist);
            }
            baseColor.rgb = ApplyWaterlineWetTerrainTint(baseColor.rgb, material, hitPos.y, normal.y, 0.78f);

            // Simple skybox/IBL-style lighting: direct sun plus directional
            // sky/ground ambient so shaded cliffs still read in the vertical world.
            float3 lightDir = SkySunDirection();
            float ndotl = saturate(dot(normal, lightDir) * 0.85f + 0.15f);
            float3 ambient = SkyAmbient(normal) * 0.68f;

            // Add slight variant-based color variation
            uint variant = GetVariant(voxel);
            float variantNoise = (variant / 255.0f) * 0.1f - 0.05f;  // +/- 5%

            float3 finalColor = baseColor.rgb * (ambient + ndotl * 0.48f) * (1.0f + variantNoise);
            if (CameraUnderwaterForShading()) {
                const float underwaterFog = saturate((dist - 6.0f) / 88.0f);
                float fogStrength;
                const float3 waterTint = UnderwaterVolumeTint(
                    hitPos,
                    dist,
                    0.38f + underwaterFog * 0.38f,
                    fogStrength);
                finalColor = lerp(finalColor, waterTint, fogStrength);
                const float nearWaterLight = 1.0f - saturate((dist - 10.0f) / 180.0f);
                const float upwardFace = saturate(normal.y * 0.55f + 0.45f);
                finalColor += float3(0.030f, 0.070f, 0.060f) *
                    UnderwaterCaustic(hitPos) * nearWaterLight * upwardFace;
            }
            if (frame.debugMode == 50u) {
                finalColor = float3(1.0f, 0.95f, 0.05f);
            }
            if (frame.debugMode == 7u) {
                float3 sparseTint = voxelFromSparse ? float3(0.35f, 1.0f, 0.42f) : float3(1.0f, 0.38f, 0.28f);
                finalColor = lerp(finalColor, sparseTint, 0.55f);
            }

            if (frame.debugMode != 50u) {
                // Depth fog
                float fogFactor = saturate(dist / maxDist);
                if (frame.debugMode == 60u) {
                    RecordRenderOwnership(RENDER_OWNER_NEAR);
                    const float debugFog = fogFactor * 0.5f;
                    return MakeHit(float4(float3(debugFog, debugFog, debugFog), 1.0f), entryDist + dist);
                }
                float3 fogColor = SkyColor(rayDir);
                finalColor = lerp(finalColor, fogColor, fogFactor * 0.5f);
            }

            // Use material's alpha from palette (enables transparency for water, glass, etc.)
            if (sawLocalWater || material == MAT_WATER) {
                RecordRenderWaterContext();
            }
            RecordRenderOwnership(RENDER_OWNER_NEAR);
            return MakeHit(float4(finalColor, frame.debugMode == 50u ? 1.0f : baseColor.a), hitWorldDistance);
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
        const bool highAltitudeBackgroundView = rayOrigin.y > 384.0f;
        if (highAltitudeBackgroundView &&
            RaymarchBackgroundField(
                rayOrigin,
                rayDir,
                32.0f,
                true,
                true,
                backgroundHit,
                backgroundLayer)) {
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, backgroundHit, backgroundLayer);
        }
        // Missing sparse pages inside the editable/collision volume are not
        // proof that far terrain owns that ray segment. Keep the background
        // renderer behind the same transition band as the surface path so stale
        // or late near pages cannot be filled by detached coarse terrain.
        const float firstHoleStart = entryDist + firstSparseMissingDist + 24.0f;
        const float protectedHoleFillStart =
            SparseMissingPageBackgroundStartForRay(rayOrigin, rayDir, gridMin, gridMax);
        const bool lowSurfaceAuthorityView =
            sparseSurfaceAuthoritative && frame.cameraPosition.y <= FAR_SEA_LEVEL + 220.0f;
        // This branch only runs for pixels where the raster exact surface did
        // not already own the pixel and the exact sparse DDA found a missing
        // page. Treat that as a streaming hole, not proof that the protected
        // near band must remain empty. Starting the lower-LOD field at the
        // first missing page lets mid/far terrain carry the public render until
        // exact pages promote, instead of showing sky-colored cutouts in the
        // first visible frames.
        const float holeFillStart = sparseSurfaceRaymarchFill
            ? firstHoleStart
            : max(firstHoleStart, protectedHoleFillStart);
        const float firstSparseMissingWorldDist = entryDist + firstSparseMissingDist;
        // WALKING-HOLES FIX: this branch fills a streaming hole (missing exact
        // page). If the missing brick's analytic column is dry land, forbid the
        // analytic water sheet from owning the fill — the mid/far land estimate
        // shows through instead, so the hole reads as terrain, not navy water.
        const bool suppressPendingLandWaterFill =
            firstSparseMissingTerrainAdjacent &&
            firstSparseMissingLandAboveSea;
        if (RaymarchBackgroundField(
            rayOrigin,
            rayDir,
            holeFillStart,
            true,
            true,
            backgroundHit,
            backgroundLayer,
            suppressPendingLandWaterFill)) {
            const float surfaceOwnershipDistance = max(frame.nearOwnershipParams.w, ExactNearDistance());
            const bool lowerLodTerrainBeforeExactSurfaceLimit =
                backgroundLayer == BACKGROUND_LAYER_MID_VOXEL ||
                backgroundLayer == BACKGROUND_LAYER_FAR_SVO;
            if (lowSurfaceAuthorityView &&
                lowerLodTerrainBeforeExactSurfaceLimit &&
                firstSparseMissingTerrainAdjacent &&
                firstSparseMissingWorldDist <= surfaceOwnershipDistance &&
                backgroundHit.distance > firstSparseMissingWorldDist + 8.0f &&
                backgroundHit.distance <= surfaceOwnershipDistance + 512.0f) {
                RecordUnsafeSparseMissSample(firstSparseMissingBrickCoord, firstSparseMissingWorldDist);
            }
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, backgroundHit, backgroundLayer);
        }
        if (rayDir.y > -0.88f || highAltitudeBackgroundView) {
            float expectedTerrainT = 1e20f;
            if (rayDir.y < -0.005f) {
                const float exactHoleDistance = max(ExactNearDistance(), 224.0f);
                const float terrainProbeLimit = min(max(exactHoleDistance, 384.0f), 1024.0f);
                [loop]
                for (float terrainT = 8.0f; terrainT <= terrainProbeLimit; terrainT += 16.0f) {
                    const float3 terrainProbe = rayOrigin + rayDir * terrainT;
                    float mountainMask;
                    float spireMask;
                    float ravineMask;
                    const float terrainHeight = FarTerrainHeight(
                        terrainProbe.xz,
                        mountainMask,
                        spireMask,
                        ravineMask);
                    if (terrainProbe.y <= terrainHeight - 1.0f) {
                        expectedTerrainT = terrainT;
                        break;
                    }
                }
            }
            const float exactHoleDistance = max(ExactNearDistance(), 224.0f);
            const bool nearSparseHole =
                // "Unsafe near" is reserved for editable/collision foreground
                // holes. In high-altitude far-LOD views, the exact sparse
                // window is only a helper layer; missed downward samples should
                // fall through to sky/valley/background ownership instead of
                // tripping the near-terrain hole gate.
                !highAltitudeBackgroundView &&
                firstSparseMissingTerrainAdjacent &&
                firstSparseMissingWorldDist < exactHoleDistance + 96.0f &&
                expectedTerrainT < exactHoleDistance + 160.0f &&
                firstSparseMissingWorldDist <= expectedTerrainT + 32.0f &&
                expectedTerrainT <= firstSparseMissingWorldDist + 64.0f;
            if (nearSparseHole) {
                RecordUnsafeSparseMissSample(firstSparseMissingBrickCoord, firstSparseMissingWorldDist);
            }
            return DebugBackgroundMissHit(
                rayOrigin,
                rayDir,
                entryDist + firstSparseMissingDist,
                nearSparseHole,
                runTerrainSkyReasonDebug);
        }
        RecordUnsafeSparseMissSample(firstSparseMissingBrickCoord, entryDist + firstSparseMissingDist);
        return DebugUnsafeNearMissHit(rayDir);
    }

    // If the ray cleanly exits the dense editable cache, continue into the
    // far-field renderer from just beyond the cache. This preserves the earlier
    // protection against drawing far terrain through missing near chunks, while
    // avoiding a hard sky cutoff when the camera pans past the near window.
    if (entryDist + dist >= tMax - 1.0f) {
        RayHit farHit;
        uint farLayer;
        float farStart = sparseSurfaceAuthoritative
            ? max(ExactNearDistance() + 8.0f, NearBackgroundStartDistance())
            : max(tMax + 8.0f, entryDist + dist);
        if (sparseSurfaceAuthoritative && frame.cameraPosition.y <= FAR_SEA_LEVEL + 220.0f) {
            farStart = max(farStart, LowAltitudeProtectedBackgroundStartDistance() + 8.0f);
        }
        // High-altitude downward rays: the exact-near brick box ends in the air
        // above the terrain, so a clean box exit does NOT mean the exact layer
        // owns the ground beyond it. Holding the background behind the exact
        // ownership radius here painted a sky-colored no-owner halo ring around
        // the near field in aerial views (ground crossing at ~0.75-0.9x the
        // exact radius belongs to no layer). Start the background at the box
        // exit instead; the high-altitude exact window is a helper layer (same
        // rationale as the nearSparseHole gate below).
        if (rayOrigin.y > 384.0f && rayDir.y < -0.35f) {
            farStart = min(farStart, max(entryDist + dist - 64.0f, 32.0f));
        }
        if (RaymarchBackgroundField(rayOrigin, rayDir, farStart, true, true, farHit, farLayer)) {
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, farHit, farLayer);
        }
    }

    if (sparseNearActive) {
        // Sparse near-field traversal can be intentionally budget-limited by
        // the scheduler. If that budget expires while the ray is still inside
        // the editable cache, the old path returned a raw miss/sky pixel and
        // produced the visible circular moat around the camera. Hand the ray to
        // the ownership background from the point where near traversal stopped.
        RayHit budgetHit;
        uint budgetLayer;
        float budgetStart = sparseSurfaceAuthoritative
            ? (frame.cameraPosition.y <= FAR_SEA_LEVEL + 220.0f
                ? max(NearBackgroundStartDistance(), LowAltitudeProtectedBackgroundStartDistance() + 8.0f)
                : max(NearBackgroundStartDistance(), ExactNearDistance() + 8.0f))
            : max(
                NearBackgroundStartDistance(),
                entryDist + min(dist, maxMarchDist) + 8.0f);
        // Same high-altitude relaxation as the clean-exit path above: a budget
        // stop in mid-air must not leave the ground band before the exact
        // ownership radius owner-less (sky halo) in aerial views.
        if (rayOrigin.y > 384.0f && rayDir.y < -0.35f) {
            budgetStart = min(
                budgetStart,
                max(entryDist + min(dist, maxMarchDist) - 64.0f, 32.0f));
        }
        if (RaymarchBackgroundField(rayOrigin, rayDir, budgetStart, true, true, budgetHit, budgetLayer)) {
            return DebugBackgroundLayerHitWithExactFeedback(rayOrigin, rayDir, budgetHit, budgetLayer);
        }
    }

    return DebugBackgroundMissHit(
        rayOrigin,
        rayDir,
        max(NearBackgroundStartDistance(), entryDist + min(dist, maxMarchDist) + 8.0f),
        false,
        runTerrainSkyReasonDebug);
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
    if (brushRadius <= 0.0f || distToCenter < max(brushRadius * 6.0f, 16.0f)) {
        return float4(0, 0, 0, 0);
    }

    float3 brushDir = toBrush / max(distToCenter, 0.001f);
    if (dot(brushDir, rayDir) <= 0.0f) {
        // Brush is behind this pixel ray; do not let behind-camera preview
        // state leak into the visible frame.
        return float4(0, 0, 0, 0);
    }

    const float angularRadius = asin(saturate(brushRadius / max(distToCenter, 0.001f)));
    if (angularRadius > 0.16f) {
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
        float3 normal = normalize(hitPoint - brushCenter);
        float fresnel = pow(1.0f - abs(dot(normal, rayDir)), 2.0f);
        float rim = smoothstep(0.42f, 0.88f, fresnel);
        if (rim <= 0.02f) {
            return float4(0, 0, 0, 0);
        }
        float alpha = rim * 0.24f;

        return float4(baseColor, alpha);
    }
    else {  // Cube or cylinder - simple semi-transparent rendering
        return float4(baseColor, 0.10f);
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

#ifdef RAYMARCH_MID_ONLY
    // MID-ONLY pass: a separate, simpler PSO (DXC strips far/background/near via dead-code
    // elimination) that renders ONLY the mid-voxel DDA with the analytic gradient shading
    // (which is too heavy to fit the uber-shader's PSO). Drawn over the full pass with alpha
    // blending: alpha=1 where mid terrain is hit (gradient color wins), alpha=0 elsewhere
    // (full pass's far/background/sky shows through). No depth needed.
    {
        RayHit midHit;
        bool midGot = RaymarchMidVoxelClipmap(cameraPos, rayDir, 1.0f, midHit);
        if (!midGot || midHit.distance > 1.0e9f) {
            output.color = float4(0.0f, 0.0f, 0.0f, 0.0f);   // miss -> transparent
        } else {
            output.color = float4(midHit.color.rgb, 1.0f);   // mid terrain -> opaque (gradient)
        }
        return output;
    }
#endif

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

    const uint2 pixelCoord = uint2(input.position.xy);
    const bool runTerrainSkyReasonDebug =
        frame.debugMode == 57u &&
        ((pixelCoord.x & 7u) == 0u) &&
        ((pixelCoord.y & 7u) == 0u);

    // Render voxel world
    RayHit worldHit = Raymarch(cameraPos, rayDir, runTerrainSkyReasonDebug);
    float4 voxelColor = worldHit.color;
    float depthDistance = worldHit.distance;

    float avatarT;
    float4 avatarColor;
    if (RenderBlockCharacter(cameraPos, rayDir, avatarT, avatarColor) && avatarT < worldHit.distance) {
        voxelColor = frame.debugMode == 53u
            ? float4(0.20f, 1.0f, 0.80f, 1.0f)
            : avatarColor;
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
            if (frame.debugMode == 53u) {
                voxelColor = float4(1.0f, 0.20f, 0.80f, 1.0f);
            } else {
                voxelColor.rgb = lerp(voxelColor.rgb, brushPreview.rgb, brushPreview.a);
            }
        }
    }

    output.color = voxelColor;
    return output;
}
