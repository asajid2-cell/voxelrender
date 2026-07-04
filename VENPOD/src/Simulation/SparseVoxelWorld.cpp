#include "SparseVoxelWorld.h"

#include "Simulation/HeightAtAttribution.h"
#include "Simulation/TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

namespace {

constexpr uint32_t SPARSE_BRUSH_MODE_PAINT = 0;
constexpr uint32_t SPARSE_BRUSH_MODE_ERASE = 1;
constexpr uint32_t SPARSE_BRUSH_MODE_REPLACE = 2;
constexpr uint32_t SPARSE_BRUSH_MODE_FILL = 3;
constexpr uint32_t SPARSE_BRUSH_SHAPE_CUBE = 1;
constexpr uint32_t SPARSE_BRUSH_SHAPE_CYLINDER = 2;
constexpr uint32_t SPARSE_PHYSICS_MATERIAL_SAND = 1u << 0u;
constexpr uint32_t SPARSE_PHYSICS_MATERIAL_WATER = 1u << 1u;
constexpr uint32_t SPARSE_PHYSICS_MATERIAL_LAVA = 1u << 2u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_KNOWN_MASK =
    SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
    SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
    SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
    SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE |
    SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
    SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW |
    SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;

float WaitTicksToMs(uint64_t startTicks) {
    const uint64_t endTicks = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0u || endTicks < startTicks) {
        return 0.0f;
    }
    return static_cast<float>(
        (static_cast<double>(endTicks - startTicks) * 1000.0) /
        static_cast<double>(frequency));
}
constexpr float kMaxSparseRaycastDistance = 8192.0f;

enum class SparseSurfaceOccupancyClass : uint8_t {
    Air = 0,
    Solid = 1,
    Water = 2
};

SparseSurfaceOccupancyClass SurfaceOccupancyClassForVoxel(uint32_t voxel) {
    const uint8_t material = VENPOD::Utils::UnpackMaterial(voxel);
    if (material == VENPOD::Utils::Material::Air) {
        return SparseSurfaceOccupancyClass::Air;
    }
    if (material == VENPOD::Utils::Material::Water) {
        return SparseSurfaceOccupancyClass::Water;
    }
    return SparseSurfaceOccupancyClass::Solid;
}
constexpr uint32_t kMaxSparseRaycastSteps = 32768;
constexpr uint32_t kMaxSparseLocalPhysicsWorkPackets = 2048;
constexpr uint32_t kStreamingTicketStageCpuGenerated = 1u << 0u;
constexpr uint32_t kStreamingTicketStageGpuUploaded = 1u << 1u;
constexpr uint32_t kStreamingTicketStageSurfaceReady = 1u << 2u;
constexpr uint32_t kStreamingTicketStagePagePublished = 1u << 3u;

template <typename Candidate, typename Compare>
void SortBestEvictionCandidates(
    std::vector<Candidate>& candidates,
    uint32_t maxCandidates,
    Compare compare)
{
    if (candidates.empty()) {
        return;
    }

    static const bool usePartialSort = []() {
        const char* env = std::getenv("VENPOD_SPARSE_EVICTION_PARTIAL_SORT");
        return env == nullptr || std::atoi(env) != 0;
    }();
    if (!usePartialSort) {
        std::sort(candidates.begin(), candidates.end(), compare);
        return;
    }

    const size_t limit = std::min<size_t>(
        static_cast<size_t>(std::max(1u, maxCandidates)),
        candidates.size());
    if (limit < candidates.size()) {
        std::partial_sort(candidates.begin(), candidates.begin() + limit, candidates.end(), compare);
        candidates.resize(limit);
    } else {
        std::sort(candidates.begin(), candidates.end(), compare);
    }
}

struct SparseWorldVoxelKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const SparseWorldVoxelKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct SparseWorldVoxelKeyHash {
    size_t operator()(const SparseWorldVoxelKey& key) const noexcept {
        uint32_t hash = 2166136261u;
        hash = (hash ^ static_cast<uint32_t>(key.x)) * 16777619u;
        hash = (hash ^ static_cast<uint32_t>(key.y)) * 16777619u;
        hash = (hash ^ static_cast<uint32_t>(key.z)) * 16777619u;
        return static_cast<size_t>(hash);
    }
};

uint8_t HashVoxelVariant(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t hash = 2166136261u ^ seed;
    hash = (hash ^ static_cast<uint32_t>(x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(y)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(z)) * 16777619u;
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return static_cast<uint8_t>(hash & 0xFFu);
}

bool BrushFeedbackVoxelAlreadyApplied(uint32_t currentVoxel, uint32_t feedbackVoxel) {
    if (currentVoxel == feedbackVoxel) {
        return true;
    }

    // GPU brush feedback can lag one or two edit-delta uploads. During a held
    // brush, the GPU may keep proposing the same material with a new random
    // variant even though the CPU authoritative world already contains the
    // visible result. Treat material-equivalent feedback as a no-op so repeated
    // stamps do not churn sparse pages, surface extraction, and mid-clipmap
    // invalidation.
    return Utils::UnpackMaterial(currentVoxel) == Utils::UnpackMaterial(feedbackVoxel);
}

uint32_t PackPhysicsRegionPoint(uint8_t x, uint8_t y, uint8_t z) {
    return static_cast<uint32_t>(x) |
           (static_cast<uint32_t>(y) << 8u) |
           (static_cast<uint32_t>(z) << 16u);
}

LocalVoxelCoord UnpackPhysicsRegionPoint(uint32_t packed) {
    return LocalVoxelCoord{
        static_cast<uint8_t>(packed & 0xFFu),
        static_cast<uint8_t>((packed >> 8u) & 0xFFu),
        static_cast<uint8_t>((packed >> 16u) & 0xFFu)};
}

bool IsValidPhysicsLocal(LocalVoxelCoord local) {
    return local.x < SPARSE_BRICK_SIZE &&
           local.y < SPARSE_BRICK_SIZE &&
           local.z < SPARSE_BRICK_SIZE;
}

bool TryFloorToInt32(float value, int32_t* out) {
    if (!out || !std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(static_cast<double>(value));
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *out = static_cast<int32_t>(floored);
    return true;
}

bool TryStepInt32(int32_t value, int32_t step, int32_t* out) {
    if (!out) {
        return false;
    }
    const int64_t stepped = static_cast<int64_t>(value) + static_cast<int64_t>(step);
    if (stepped < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        stepped > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *out = static_cast<int32_t>(stepped);
    return true;
}

bool TryOffsetBrickCoord(const BrickCoord& coord, int32_t dx, int32_t dy, int32_t dz, BrickCoord* out) {
    if (!out) {
        return false;
    }
    return TryStepInt32(coord.x, dx, &out->x) &&
           TryStepInt32(coord.y, dy, &out->y) &&
           TryStepInt32(coord.z, dz, &out->z);
}

struct SurfaceWorkerColumnKey {
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const SurfaceWorkerColumnKey& other) const noexcept {
        return x == other.x && z == other.z;
    }
};

struct SurfaceWorkerColumnKeyHash {
    size_t operator()(const SurfaceWorkerColumnKey& key) const noexcept {
        uint64_t h = 1469598103934665603ull;
        h ^= static_cast<uint32_t>(key.x);
        h *= 1099511628211ull;
        h ^= static_cast<uint32_t>(key.z);
        h *= 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

struct SurfaceWorkerColumnCacheEntry {
    float height = 0.0f;
    float relief = 0.0f;
    int32_t reliefSampleOffset = 0;
    bool reliefValid = false;
};

using SurfaceWorkerColumnCache =
    std::unordered_map<SurfaceWorkerColumnKey, SurfaceWorkerColumnCacheEntry, SurfaceWorkerColumnKeyHash>;

SparseSurfaceExtractionResult ExtractSurfaceNoEditWithTerrain(
    const SparseTerrainGenerator& terrain,
    const GeneratedSparseBrick& brick,
    SurfaceWorkerColumnCache& columnCache)
{
    HEIGHTAT_SCOPE("ExtractSurfaceNoEditWithTerrain");
    const auto cachedHeightAt = [&terrain, &columnCache](int32_t worldX, int32_t worldZ) {
        const SurfaceWorkerColumnKey key{worldX, worldZ};
        auto columnIt = columnCache.find(key);
        if (columnIt != columnCache.end()) {
            return columnIt->second.height;
        }

        SurfaceWorkerColumnCacheEntry entry;
        entry.height = terrain.HeightAt(worldX, worldZ);
        auto [insertedIt, inserted] = columnCache.emplace(key, entry);
        (void)inserted;
        return insertedIt->second.height;
    };
    const auto cachedReliefAt = [
        &columnCache,
        &cachedHeightAt](int32_t worldX, int32_t worldZ, int32_t sampleOffset) {
        const int32_t offset = std::max(1, sampleOffset);
        const SurfaceWorkerColumnKey key{worldX, worldZ};
        auto columnIt = columnCache.find(key);
        if (columnIt == columnCache.end()) {
            SurfaceWorkerColumnCacheEntry entry;
            entry.height = cachedHeightAt(worldX, worldZ);
            columnIt = columnCache.emplace(key, entry).first;
        }
        if (columnIt->second.reliefValid && columnIt->second.reliefSampleOffset == offset) {
            return columnIt->second.relief;
        }

        int32_t xMinus = worldX;
        int32_t xPlus = worldX;
        int32_t zMinus = worldZ;
        int32_t zPlus = worldZ;
        (void)TryStepInt32(worldX, -offset, &xMinus);
        (void)TryStepInt32(worldX, offset, &xPlus);
        (void)TryStepInt32(worldZ, -offset, &zMinus);
        (void)TryStepInt32(worldZ, offset, &zPlus);

        const float center = columnIt->second.height;
        float localMin = center;
        float localMax = center;
        const float samples[] = {
            cachedHeightAt(xMinus, worldZ),
            cachedHeightAt(xPlus, worldZ),
            cachedHeightAt(worldX, zMinus),
            cachedHeightAt(worldX, zPlus),
        };
        for (float height : samples) {
            localMin = std::min(localMin, height);
            localMax = std::max(localMax, height);
        }

        columnIt->second.relief = localMax - localMin;
        columnIt->second.reliefSampleOffset = offset;
        columnIt->second.reliefValid = true;
        return columnIt->second.relief;
    };

    constexpr int32_t kSurfaceHaloColumnSize = SPARSE_BRICK_SIZE + 2;
    int32_t brickWorldMinX = 0;
    int32_t brickWorldMinZ = 0;
    bool haloCacheValid =
        TryWorldVoxelFromBrickLocal(brick.coord.x, 0, &brickWorldMinX) &&
        TryWorldVoxelFromBrickLocal(brick.coord.z, 0, &brickWorldMinZ);
    int32_t haloMinX = 0;
    int32_t haloMinZ = 0;
    if (haloCacheValid &&
        (!TryStepInt32(brickWorldMinX, -1, &haloMinX) ||
         !TryStepInt32(brickWorldMinZ, -1, &haloMinZ))) {
        haloCacheValid = false;
    }

    std::array<float, kSurfaceHaloColumnSize * kSurfaceHaloColumnSize> haloHeights = {};
    std::array<float, kSurfaceHaloColumnSize * kSurfaceHaloColumnSize> haloRelief = {};
    if (haloCacheValid) {
        for (int32_t z = 0; z < kSurfaceHaloColumnSize; ++z) {
            for (int32_t x = 0; x < kSurfaceHaloColumnSize; ++x) {
                const int32_t worldX = haloMinX + x;
                const int32_t worldZ = haloMinZ + z;
                const size_t index = static_cast<size_t>(x + z * kSurfaceHaloColumnSize);
                haloHeights[index] = cachedHeightAt(worldX, worldZ);
                haloRelief[index] = cachedReliefAt(worldX, worldZ, 4);
            }
        }
    }

    auto neighborSampler = [
        &terrain,
        haloCacheValid,
        haloMinX,
        haloMinZ,
        &haloHeights,
        &haloRelief](int32_t worldX, int32_t worldY, int32_t worldZ) {
        if (haloCacheValid &&
            worldX >= haloMinX &&
            worldX < haloMinX + kSurfaceHaloColumnSize &&
            worldZ >= haloMinZ &&
            worldZ < haloMinZ + kSurfaceHaloColumnSize) {
            const int32_t localX = worldX - haloMinX;
            const int32_t localZ = worldZ - haloMinZ;
            const size_t index = static_cast<size_t>(localX + localZ * kSurfaceHaloColumnSize);
            return terrain.SampleGeneratedVoxelWithColumn(
                worldX,
                worldY,
                worldZ,
                haloHeights[index],
                haloRelief[index]);
        }
        return terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
    };

    return SparseSurfaceExtractor::Extract(brick, neighborSampler);
}

int64_t BrickCoordDelta(int32_t coord, int32_t center) {
    return static_cast<int64_t>(coord) - static_cast<int64_t>(center);
}

uint64_t AbsInt64ToUint64(int64_t value) {
    if (value >= 0) {
        return static_cast<uint64_t>(value);
    }
    return static_cast<uint64_t>(-(value + 1)) + 1u;
}

bool WithinKeepRadius(int64_t dx, int64_t dy, int64_t dz, uint32_t keepRadiusXz, uint32_t keepRadiusY) {
    return AbsInt64ToUint64(dx) <= static_cast<uint64_t>(keepRadiusXz) &&
           AbsInt64ToUint64(dy) <= static_cast<uint64_t>(keepRadiusY) &&
           AbsInt64ToUint64(dz) <= static_cast<uint64_t>(keepRadiusXz);
}

uint64_t SaturatingWeightedSquare(uint64_t value, uint64_t weight) {
    constexpr uint64_t kMaxScore = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (value == 0 || weight == 0) {
        return 0;
    }
    if (value > kMaxScore / value) {
        return kMaxScore;
    }
    const uint64_t square = value * value;
    if (square > kMaxScore / weight) {
        return kMaxScore;
    }
    return square * weight;
}

uint64_t SaturatingAddScore(uint64_t a, uint64_t b) {
    constexpr uint64_t kMaxScore = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (a > kMaxScore - b) {
        return kMaxScore;
    }
    return a + b;
}

int64_t SparseBrickDistanceScore(int64_t dx, int64_t dy, int64_t dz) {
    constexpr uint64_t kMaxScore = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t score = SaturatingWeightedSquare(AbsInt64ToUint64(dx), 1u);
    score = SaturatingAddScore(score, SaturatingWeightedSquare(AbsInt64ToUint64(dz), 1u));
    score = SaturatingAddScore(score, SaturatingWeightedSquare(AbsInt64ToUint64(dy), 4u));
    return score >= kMaxScore
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(score);
}

int64_t SaturatingAddInt64(int64_t a, int64_t b) {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
        return std::numeric_limits<int64_t>::max();
    }
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return std::numeric_limits<int64_t>::min();
    }
    return a + b;
}

bool GpuPhysicsEditRevisionsMatch(
    bool proposalUsedEditDelta,
    uint32_t proposalSourceRevision,
    uint32_t proposalDestinationRevision,
    uint32_t currentSourceRevision,
    uint32_t currentDestinationRevision)
{
    if (!proposalUsedEditDelta) {
        return true;
    }
    if (proposalSourceRevision == 0u && proposalDestinationRevision == 0u) {
        return false;
    }
    if (proposalSourceRevision != 0u && currentSourceRevision != proposalSourceRevision) {
        return false;
    }
    if (proposalDestinationRevision != 0u &&
        currentDestinationRevision != proposalDestinationRevision) {
        return false;
    }
    return true;
}

bool GpuPhysicsProposalStatusWellFormed(uint32_t status) {
    if ((status & SPARSE_PHYSICS_PACKET_STATUS_CONSUMED) == 0u) {
        return false;
    }
    return (status & ~SPARSE_PHYSICS_PACKET_STATUS_KNOWN_MASK) == 0u;
}

template <typename TRegion>
uint32_t SparseRegionVoxelCount(const TRegion& region) {
    const uint32_t widthX =
        region.maxX >= region.minX ? static_cast<uint32_t>(region.maxX - region.minX + 1u) : 0u;
    const uint32_t widthY =
        region.maxY >= region.minY ? static_cast<uint32_t>(region.maxY - region.minY + 1u) : 0u;
    const uint32_t widthZ =
        region.maxZ >= region.minZ ? static_cast<uint32_t>(region.maxZ - region.minZ + 1u) : 0u;
    return widthX * widthY * widthZ;
}

template <typename TRegion>
void MergeSparseRegion(TRegion& existing, const TRegion& incoming) {
    existing.minX = std::min(existing.minX, incoming.minX);
    existing.minY = std::min(existing.minY, incoming.minY);
    existing.minZ = std::min(existing.minZ, incoming.minZ);
    existing.maxX = std::max(existing.maxX, incoming.maxX);
    existing.maxY = std::max(existing.maxY, incoming.maxY);
    existing.maxZ = std::max(existing.maxZ, incoming.maxZ);
}

bool PhysicsMaterialAllowed(uint8_t material, uint32_t materialMask) {
    if (material == Utils::Material::Sand) {
        return (materialMask & SPARSE_PHYSICS_MATERIAL_SAND) != 0u;
    }
    if (material == Utils::Material::Water) {
        return (materialMask & SPARSE_PHYSICS_MATERIAL_WATER) != 0u;
    }
    if (material == Utils::Material::Lava) {
        return (materialMask & SPARSE_PHYSICS_MATERIAL_LAVA) != 0u;
    }
    return false;
}

int32_t ResidencyRetentionScore(SparseResidencyClass residencyClass) {
    switch (residencyClass) {
        case SparseResidencyClass::Edited:
            return 12000;
        case SparseResidencyClass::Collision:
            return 8000;
        case SparseResidencyClass::Visible:
            return 4000;
        case SparseResidencyClass::Speculative:
        default:
            return 0;
    }
}

uint8_t ResidencyRank(SparseResidencyClass residencyClass) {
    return static_cast<uint8_t>(residencyClass);
}

uint8_t StreamingLaneRank(SparseStreamingLane lane) {
    switch (lane) {
        case SparseStreamingLane::PublicCritical:
            return 4;
        case SparseStreamingLane::Visible:
            return 3;
        case SparseStreamingLane::Repair:
            return 2;
        case SparseStreamingLane::Prefetch:
            return 1;
        case SparseStreamingLane::Cache:
        default:
            return 0;
    }
}

size_t ResidencyClassQueueIndex(SparseResidencyClass residencyClass) {
    return static_cast<size_t>(ResidencyRank(residencyClass));
}

size_t OwnershipCriticalQueueIndex(bool ownershipCritical) {
    return ownershipCritical ? 1u : 0u;
}

void IncrementResidencyClassCounter(
    SparseResidencyClass residencyClass,
    uint32_t& speculative,
    uint32_t& visible,
    uint32_t& collision,
    uint32_t& edited)
{
    switch (residencyClass) {
        case SparseResidencyClass::Edited:
            ++edited;
            break;
        case SparseResidencyClass::Collision:
            ++collision;
            break;
        case SparseResidencyClass::Visible:
            ++visible;
            break;
        case SparseResidencyClass::Speculative:
        default:
            ++speculative;
            break;
    }
}

void IncrementStreamingLaneCounter(
    SparseStreamingLane lane,
    uint32_t& cache,
    uint32_t& prefetch,
    uint32_t& repair,
    uint32_t& visible,
    uint32_t& publicCritical)
{
    switch (lane) {
        case SparseStreamingLane::PublicCritical:
            ++publicCritical;
            break;
        case SparseStreamingLane::Visible:
            ++visible;
            break;
        case SparseStreamingLane::Repair:
            ++repair;
            break;
        case SparseStreamingLane::Prefetch:
            ++prefetch;
            break;
        case SparseStreamingLane::Cache:
        default:
            ++cache;
            break;
    }
}

uint32_t LatestPriorityTouch(const BrickResidentRecord& record) {
    uint32_t latest = record.lastTouchedFrame;
    latest = std::max(latest, record.lastSpeculativeFrame);
    latest = std::max(latest, record.lastVisibleFrame);
    latest = std::max(latest, record.lastCollisionFrame);
    latest = std::max(latest, record.lastEditedFrame);
    return latest;
}

bool HasResidencyFlag(uint32_t flags, BrickResidencyFlags flag) {
    return (flags & static_cast<uint32_t>(flag)) != 0u;
}

int64_t QueuePriorityScore(
    const BrickResidentRecord& record,
    uint32_t currentFrame,
    bool streamingLanePriority)
{
    const uint32_t latestTouch = LatestPriorityTouch(record);
    const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
    const uint32_t freshness = currentFrame == 0
        ? latestTouch
        : (100000u - std::min(age, 100000u));
    const int64_t requestPriority = std::clamp<int64_t>(
        static_cast<int64_t>(record.queuePriority),
        -500000ll,
        500000ll);
    const int64_t lanePriority = streamingLanePriority
        ? static_cast<int64_t>(StreamingLaneRank(record.streamingLane)) * 200000000000ll
        : 0ll;
    return static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 1000000000000ll +
           lanePriority +
           static_cast<int64_t>(freshness) * 1000000ll +
           requestPriority;
}

int64_t UploadValueScore(
    const BrickResidentRecord& record,
    const BrickCoord& focus,
    uint32_t currentFrame,
    bool streamingLanePriority)
{
    const int64_t dx = BrickCoordDelta(record.coord.x, focus.x);
    const int64_t dy = BrickCoordDelta(record.coord.y, focus.y);
    const int64_t dz = BrickCoordDelta(record.coord.z, focus.z);
    const int64_t distancePenalty = SparseBrickDistanceScore(dx, dy, dz);
    const uint32_t latestTouch = LatestPriorityTouch(record);
    const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
    const uint32_t freshness = currentFrame == 0
        ? latestTouch
        : (100000u - std::min(age, 100000u));
    const int64_t lanePriority = streamingLanePriority
        ? static_cast<int64_t>(StreamingLaneRank(record.streamingLane)) * 200000000000ll
        : 0ll;
    return static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 1000000000000ll +
           lanePriority +
           static_cast<int64_t>(freshness) * 10000ll -
           distancePenalty;
}

void SortQueuedBricksByPriority(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    uint32_t currentFrame,
    bool streamingLanePriority)
{
    if (queue.size() <= 1) {
        return;
    }

    struct QueuedBrick {
        BrickCoord coord;
        int64_t score = 0;
    };

    std::vector<QueuedBrick> sorted;
    sorted.reserve(queue.size());
    std::unordered_set<BrickCoord, BrickCoordHash> seen;
    for (const BrickCoord& coord : queue) {
        if (!seen.insert(coord).second) {
            continue;
        }
        BrickResidentRecord record;
        if (!pool.GetRecord(coord, &record)) {
            continue;
        }
        sorted.push_back({coord, QueuePriorityScore(record, currentFrame, streamingLanePriority)});
    }

    std::sort(sorted.begin(), sorted.end(), [](const QueuedBrick& a, const QueuedBrick& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    queue.clear();
    for (const QueuedBrick& queued : sorted) {
        queue.push_back(queued.coord);
    }
}

void SortQueuedBricksByValue(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    const BrickCoord& focus,
    uint32_t currentFrame,
    bool streamingLanePriority)
{
    if (queue.size() <= 1) {
        return;
    }

    struct QueuedBrick {
        BrickCoord coord;
        int64_t score = 0;
    };

    std::vector<QueuedBrick> sorted;
    sorted.reserve(queue.size());
    std::unordered_set<BrickCoord, BrickCoordHash> seen;
    for (const BrickCoord& coord : queue) {
        if (!seen.insert(coord).second) {
            continue;
        }
        BrickResidentRecord record;
        if (!pool.GetRecord(coord, &record)) {
            continue;
        }
        sorted.push_back({coord, UploadValueScore(record, focus, currentFrame, streamingLanePriority)});
    }

    std::sort(sorted.begin(), sorted.end(), [](const QueuedBrick& a, const QueuedBrick& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    queue.clear();
    for (const QueuedBrick& queued : sorted) {
        queue.push_back(queued.coord);
    }
}

void PartialSortQueuedBricksByValue(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    const BrickCoord& focus,
    uint32_t currentFrame,
    size_t frontCount,
    bool streamingLanePriority)
{
    if (queue.size() <= 1 || frontCount == 0) {
        return;
    }

    struct QueuedBrick {
        BrickCoord coord;
        int64_t score = 0;
    };

    std::vector<QueuedBrick> sorted;
    sorted.reserve(queue.size());
    std::unordered_set<BrickCoord, BrickCoordHash> seen;
    for (const BrickCoord& coord : queue) {
        if (!seen.insert(coord).second) {
            continue;
        }
        BrickResidentRecord record;
        if (!pool.GetRecord(coord, &record)) {
            continue;
        }
        sorted.push_back({coord, UploadValueScore(record, focus, currentFrame, streamingLanePriority)});
    }

    const auto better = [](const QueuedBrick& a, const QueuedBrick& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    };

    const size_t selectedCount = std::min(frontCount, sorted.size());
    if (selectedCount == sorted.size()) {
        std::sort(sorted.begin(), sorted.end(), better);
    } else if (selectedCount > 0) {
        const auto selectedEnd = sorted.begin() + static_cast<std::ptrdiff_t>(selectedCount);
        std::nth_element(sorted.begin(), selectedEnd, sorted.end(), better);
        std::sort(sorted.begin(), selectedEnd, better);
    }

    queue.clear();
    for (const QueuedBrick& queued : sorted) {
        queue.push_back(queued.coord);
    }
}

bool PopFrontQueuedBrick(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    BrickCoord* outCoord)
{
    if (!outCoord) {
        return false;
    }

    while (!queue.empty()) {
        const BrickCoord coord = queue.front();
        queue.pop_front();
        BrickResidentRecord record;
        if (!pool.GetRecord(coord, &record)) {
            continue;
        }
        *outCoord = coord;
        return true;
    }
    return false;
}

bool RemoveAllQueuedCoord(std::deque<BrickCoord>& queue, const BrickCoord& coord) {
    const auto oldSize = queue.size();
    queue.erase(std::remove(queue.begin(), queue.end(), coord), queue.end());
    return queue.size() != oldSize;
}

template <size_t N>
bool RemoveAllClassQueueCoord(
    std::array<std::deque<BrickCoord>, N>& queues,
    const BrickCoord& coord)
{
    bool removed = false;
    for (auto& queue : queues) {
        removed = RemoveAllQueuedCoord(queue, coord) || removed;
    }
    return removed;
}

bool BuildUploadPacketForCoord(
    const BrickCoord& coord,
    SparseBrickPool& pool,
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash>& generated,
    SparseBrickUploadPacket* outPacket,
    SparseResidencyClass* outResidencyClass)
{
    auto generatedIt = generated.find(coord);
    if (generatedIt == generated.end()) {
        return false;
    }

    BrickResidentRecord record;
    if (!pool.GetRecord(coord, &record)) {
        return false;
    }
    if (!pool.BeginUpload(coord)) {
        return false;
    }

    outPacket->coord = coord;
    outPacket->pageIndex = record.pageIndex;
    outPacket->generation = record.generation;
    outPacket->residencyClass = record.residencyClass;
    outPacket->streamingLane = record.streamingLane;
    outPacket->partialVoxelUpload = false;
    outPacket->voxelStartIndex = 0;
    outPacket->voxelCount = SPARSE_BRICK_VOXEL_COUNT;
    outPacket->dirtyMinX = 0;
    outPacket->dirtyMinY = 0;
    outPacket->dirtyMinZ = 0;
    outPacket->dirtyMaxX = SPARSE_BRICK_SIZE - 1;
    outPacket->dirtyMaxY = SPARSE_BRICK_SIZE - 1;
    outPacket->dirtyMaxZ = SPARSE_BRICK_SIZE - 1;
    outPacket->brick = generatedIt->second;
    if (outResidencyClass) {
        *outResidencyClass = record.residencyClass;
    }
    return true;
}

float BrushSdf(
    float x,
    float y,
    float z,
    float centerX,
    float centerY,
    float centerZ,
    float radius,
    uint32_t shape)
{
    const float px = x - centerX;
    const float py = y - centerY;
    const float pz = z - centerZ;

    if (shape == SPARSE_BRUSH_SHAPE_CUBE) {
        const float dx = std::abs(px) - radius;
        const float dy = std::abs(py) - radius;
        const float dz = std::abs(pz) - radius;
        const float outsideX = std::max(dx, 0.0f);
        const float outsideY = std::max(dy, 0.0f);
        const float outsideZ = std::max(dz, 0.0f);
        const float outside = std::sqrt(outsideX * outsideX + outsideY * outsideY + outsideZ * outsideZ);
        const float inside = std::min(std::max(dx, std::max(dy, dz)), 0.0f);
        return outside + inside;
    }

    if (shape == SPARSE_BRUSH_SHAPE_CYLINDER) {
        const float horizontal = std::sqrt(px * px + pz * pz);
        const float dx = horizontal - radius;
        const float dy = std::abs(py) - radius;
        const float outsideX = std::max(dx, 0.0f);
        const float outsideY = std::max(dy, 0.0f);
        return std::min(std::max(dx, dy), 0.0f) + std::sqrt(outsideX * outsideX + outsideY * outsideY);
    }

    return std::sqrt(px * px + py * py + pz * pz) - radius;
}

} // namespace

size_t SparseVoxelWorld::TerrainSurfaceColumnKeyHash::operator()(
    const TerrainSurfaceColumnKey& key) const noexcept
{
    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(key.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(key.z)) * 16777619u;
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return static_cast<size_t>(hash);
}

SparseVoxelWorld::~SparseVoxelWorld() {
    StopPersistentExactGenerationWorkers();
    StopAsyncExactGenerationWorker();
    StopAsyncSurfaceExtractionWorker();
}

bool SparseVoxelWorld::Initialize(const SparseVoxelWorldConfig& config) {
    StopPersistentExactGenerationWorkers();
    StopAsyncExactGenerationWorker();
    StopAsyncSurfaceExtractionWorker();
    m_config = config;
    m_config.parallelExactGenerationMaxWorkers =
        std::clamp<uint32_t>(m_config.parallelExactGenerationMaxWorkers, 1u, 16u);
    m_config.parallelExactGenerationMinBricks =
        std::clamp<uint32_t>(m_config.parallelExactGenerationMinBricks, 2u, 256u);
    m_config.parallelSurfaceExtractionMaxWorkers =
        std::clamp<uint32_t>(m_config.parallelSurfaceExtractionMaxWorkers, 1u, 16u);
    m_config.parallelSurfaceExtractionMinBricks =
        std::clamp<uint32_t>(m_config.parallelSurfaceExtractionMinBricks, 2u, 256u);
    m_config.parallelSurfaceExtractionMaxBatch =
        std::clamp<uint32_t>(m_config.parallelSurfaceExtractionMaxBatch, 1u, 256u);
    m_config.terrainColumnCacheMaxEntries =
        std::clamp<uint32_t>(m_config.terrainColumnCacheMaxEntries, 4096u, 1048576u);
    m_config.incrementalPressureTrimScanBudget =
        std::clamp<uint32_t>(m_config.incrementalPressureTrimScanBudget, 1024u, 262144u);
    m_terrain = SparseTerrainGenerator(config.seed);
    m_edits = SparseEditStore{};
    m_generated.clear();
    m_deferredGeneratedDownstreamQueue.clear();
    m_deferredGeneratedDownstreamSet.clear();
    m_pendingSurfaceBricks.clear();
    m_knownEmptyGeneratedBricks.clear();
    m_generationQueue.clear();
    for (auto& classQueue : m_generationClassQueues) {
        classQueue.clear();
    }
    for (auto& ownershipWorklist : m_generationOwnershipWorklists) {
        ownershipWorklist.clear();
    }
    m_generationOwnershipWorklistEntries.clear();
    m_uploadQueue.clear();
    for (auto& classQueue : m_uploadClassQueues) {
        classQueue.clear();
    }
    for (auto& ownershipQueue : m_uploadOwnershipQueues) {
        ownershipQueue.clear();
    }
    m_invalidationQueue.clear();
    m_surfaceExtractionQueue.clear();
    m_surfaceExtractionQueuedSet.clear();
    for (auto& classQueue : m_surfaceClassQueues) {
        classQueue.clear();
    }
    for (auto& ownershipQueue : m_surfaceOwnershipQueues) {
        ownershipQueue.clear();
    }
    m_generationQueuePriorityDirty = false;
    m_uploadQueuePriorityDirty = false;
    m_surfaceExtractionQueuePriorityDirty = false;
    m_deferredGeneratedDownstreamPromotedFrame = 0xFFFFFFFFu;
    m_uploadClassValueSortValid.fill(false);
    m_uploadClassValueSortFocus = {};
    m_uploadClassValueSortFrame = {};
    m_surfaceClassValueSortValid.fill(false);
    m_surfaceClassValueSortFocus = {};
    m_queueClassStatsDirty = true;
    m_cachedGenerationQueueSize = 0;
    m_cachedUploadQueueSize = 0;
    m_cachedSurfacePendingSize = 0;
    m_generationQueueClassCounts = {};
    m_uploadQueueClassCounts = {};
    m_generationQueueLaneCounts = {};
    m_uploadQueueLaneCounts = {};
    m_surfaceQueueClassCounts = {};
    m_surfaceQueueLaneCounts = {};
    m_deferredDirtyAfterUpload.clear();
    m_renderDirtyRegions.clear();
    m_surfaceDirtyRegions.clear();
    m_streamingTickets.clear();
    m_streamingTicketPendingStageOwnershipCounts = {};
    m_streamingTicketCompletedLastFrame = 0;
    m_streamingTicketProtectedSortsLastFrame = 0;
    m_surfaceTerrainColumnCache.clear();
    m_surfaceTerrainColumnCache.reserve(std::min<uint32_t>(
        m_config.terrainColumnCacheMaxEntries,
        32768u));
    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        m_asyncExactGenerationQueue.clear();
        m_asyncExactGenerationResults.clear();
        m_asyncExactGenerationPending.clear();
        m_asyncExactGenerationStop = false;
    }
    m_asyncExactGenerationEnqueuedLastFrame = 0;
    m_asyncExactGenerationCompletedLastFrame = 0;
    m_asyncExactGenerationAppliedLastFrame = 0;
    m_asyncExactGenerationDeferredLowPriorityApplyLastFrame = 0;
    m_asyncExactGenerationDiscardedLastFrame = 0;
    m_asyncExactGenerationSyncFallbackLastFrame = 0;
    m_asyncExactGenEditGateGlobalWouldSyncLastFrame = 0;
    m_asyncExactGenEditGatePerCoordSyncLastFrame = 0;
    m_asyncExactGenEditGatePerCoordAsyncLastFrame = 0;
    m_asyncExactGenEditStaleAtCompletionLastFrame = 0;
    m_asyncExactGenerationEnqueuedCacheLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedPrefetchLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedRepairLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedVisibleLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame = 0;
    m_asyncExactGenerationAppliedCacheLaneLastFrame = 0;
    m_asyncExactGenerationAppliedPrefetchLaneLastFrame = 0;
    m_asyncExactGenerationAppliedRepairLaneLastFrame = 0;
    m_asyncExactGenerationAppliedVisibleLaneLastFrame = 0;
    m_asyncExactGenerationAppliedPublicCriticalLaneLastFrame = 0;
    m_asyncExactGenerationWorkerMsLastFrame = 0.0f;
    m_asyncExactGenerationApplyMsLastFrame = 0.0f;
    m_asyncExactGenerationStatsFrame = 0;
    m_parallelExactGenerationBricksLastFrame = 0;
    m_parallelExactGenerationWorkersLastFrame = 0;
    m_parallelExactGenerationWallMsLastFrame = 0.0f;
    m_persistentExactGenerationWaitMsLastFrame = 0.0f;
    m_surfaceCache.Clear();
    m_evictedBricksLastFrame = 0;
    m_emptyRequestsSkippedLastFrame = 0;
    m_surfaceBricksExtractedLastFrame = 0;
    m_surfaceEmptyUploadsSkippedLastFrame = 0;
    m_surfaceBuriedSolidFastPathBricksLastFrame = 0;
    m_surfaceClassValueSortCallsLastFrame = 0;
    m_surfaceClassValueSortCacheHitsLastFrame = 0;
    m_surfaceStrictTimeBudgetUnsortedPopsLastFrame = 0;
    m_surfaceInlineExtractionBricksLastFrame = 0;
    m_surfaceInlineExtractionMsLastFrame = 0.0f;
    m_parallelSurfaceExtractionBricksLastFrame = 0;
    m_parallelSurfaceExtractionWorkersLastFrame = 0;
    m_parallelSurfaceExtractionWallMsLastFrame = 0.0f;
    m_surfaceExtractionWaitMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionEnqueuedLastFrame = 0;
    m_asyncSurfaceExtractionAppliedLastFrame = 0;
    m_asyncSurfaceExtractionDiscardedLastFrame = 0;
    m_asyncSurfaceExtractionRequeuedLastFrame = 0;
    m_asyncSurfaceExtractionWorkerMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionEnqueueMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionRejectedLastFrame = 0;
    m_streamingTicketCompletedLastFrame = 0;
    m_streamingTicketProtectedSortsLastFrame = 0;
    m_deferredGeneratedDownstreamPromotedLastFrame = 0;
    m_deferredGeneratedDownstreamStaleLastFrame = 0;
    m_renderDirtyVoxelsQueuedLastFrame = 0;
    m_renderDirtyFullUploadsQueuedLastFrame = 0;
    m_renderDirtyUploadDeferredLastFrame = 0;
    m_renderDirtyNonResidentLastFrame = 0;
    m_trimScanCallsLastFrame = 0;
    m_trimRecordsScannedLastFrame = 0;
    m_trimCandidatesLastFrame = 0;
    m_replacementScanCallsLastFrame = 0;
    m_replacementRecordsScannedLastFrame = 0;
    m_replacementCandidatesLastFrame = 0;
    m_trimResidentCursor = 0;
    m_trimBackgroundResidentCursor = 0;
    m_trimQueuedBackgroundCursor = 0;

    if (!m_pool.Initialize(config.maxBrickPages, config.pageTableCapacity)) {
        return false;
    }

    RefreshStats();
    return true;
}

void SparseVoxelWorld::BeginFrame() {
    m_evictedBricksLastFrame = 0;
    m_emptyRequestsSkippedLastFrame = 0;
    m_generatedSpeculativeBricksLastFrame = 0;
    m_generatedVisibleBricksLastFrame = 0;
    m_generatedCollisionBricksLastFrame = 0;
    m_generatedEditedBricksLastFrame = 0;
    m_generatedCacheLaneBricksLastFrame = 0;
    m_generatedPrefetchLaneBricksLastFrame = 0;
    m_generatedRepairLaneBricksLastFrame = 0;
    m_generatedVisibleLaneBricksLastFrame = 0;
    m_generatedPublicCriticalLaneBricksLastFrame = 0;
    m_deferredGeneratedDownstreamPromotedLastFrame = 0;
    m_deferredGeneratedDownstreamStaleLastFrame = 0;
    m_asyncExactGenerationEnqueuedLastFrame = 0;
    m_asyncExactGenerationCompletedLastFrame = 0;
    m_asyncExactGenerationAppliedLastFrame = 0;
    m_asyncExactGenerationDeferredLowPriorityApplyLastFrame = 0;
    m_asyncExactGenerationDiscardedLastFrame = 0;
    m_asyncExactGenerationSyncFallbackLastFrame = 0;
    m_asyncExactGenEditGateGlobalWouldSyncLastFrame = 0;
    m_asyncExactGenEditGatePerCoordSyncLastFrame = 0;
    m_asyncExactGenEditGatePerCoordAsyncLastFrame = 0;
    m_asyncExactGenEditStaleAtCompletionLastFrame = 0;
    m_asyncExactGenerationEnqueuedCacheLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedPrefetchLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedRepairLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedVisibleLaneLastFrame = 0;
    m_asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame = 0;
    m_asyncExactGenerationAppliedCacheLaneLastFrame = 0;
    m_asyncExactGenerationAppliedPrefetchLaneLastFrame = 0;
    m_asyncExactGenerationAppliedRepairLaneLastFrame = 0;
    m_asyncExactGenerationAppliedVisibleLaneLastFrame = 0;
    m_asyncExactGenerationAppliedPublicCriticalLaneLastFrame = 0;
    m_asyncExactGenerationWorkerMsLastFrame = 0.0f;
    m_asyncExactGenerationApplyMsLastFrame = 0.0f;
    m_persistentExactGenerationWaitMsLastFrame = 0.0f;
    m_uploadedSpeculativeBricksLastFrame = 0;
    m_uploadedVisibleBricksLastFrame = 0;
    m_uploadedCollisionBricksLastFrame = 0;
    m_uploadedEditedBricksLastFrame = 0;
    m_surfaceBricksExtractedLastFrame = 0;
    m_surfaceSpeculativeBricksExtractedLastFrame = 0;
    m_surfaceVisibleBricksExtractedLastFrame = 0;
    m_surfaceCollisionBricksExtractedLastFrame = 0;
    m_surfaceEditedBricksExtractedLastFrame = 0;
    m_surfaceEmptyUploadsSkippedLastFrame = 0;
    m_surfaceBuriedSolidFastPathBricksLastFrame = 0;
    m_surfaceClassValueSortCallsLastFrame = 0;
    m_surfaceClassValueSortCacheHitsLastFrame = 0;
    m_surfaceStrictTimeBudgetUnsortedPopsLastFrame = 0;
    m_surfaceInlineExtractionBricksLastFrame = 0;
    m_surfaceInlineExtractionMsLastFrame = 0.0f;
    m_parallelSurfaceExtractionBricksLastFrame = 0;
    m_parallelSurfaceExtractionWorkersLastFrame = 0;
    m_parallelSurfaceExtractionWallMsLastFrame = 0.0f;
    m_surfaceExtractionWaitMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionEnqueuedLastFrame = 0;
    m_asyncSurfaceExtractionAppliedLastFrame = 0;
    m_asyncSurfaceExtractionDiscardedLastFrame = 0;
    m_asyncSurfaceExtractionRequeuedLastFrame = 0;
    m_asyncSurfaceExtractionWorkerMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionEnqueueMsLastFrame = 0.0f;
    m_asyncSurfaceExtractionRejectedLastFrame = 0;
    m_terrainColumnCacheFrameStats = {};
    m_terrainColumnCacheClearedLastFrame = 0;
    if (!m_config.persistentTerrainColumnCache) {
        m_surfaceTerrainColumnCache.clear();
        m_terrainColumnCacheClearedLastFrame = 1;
    } else if (m_surfaceTerrainColumnCache.size() >
        static_cast<size_t>(m_config.terrainColumnCacheMaxEntries)) {
        m_surfaceTerrainColumnCache.clear();
        m_terrainColumnCacheClearedLastFrame = 1;
    }
    m_renderDirtyVoxelsQueuedLastFrame = 0;
    m_renderDirtyFullUploadsQueuedLastFrame = 0;
    m_renderDirtyUploadDeferredLastFrame = 0;
    m_renderDirtyNonResidentLastFrame = 0;
    m_trimScanCallsLastFrame = 0;
    m_trimRecordsScannedLastFrame = 0;
    m_trimCandidatesLastFrame = 0;
    m_replacementScanCallsLastFrame = 0;
    m_replacementRecordsScannedLastFrame = 0;
    m_replacementCandidatesLastFrame = 0;
    m_surfaceCache.BeginFrame();
    RefreshStats();
}

void SparseVoxelWorld::SetStatsRefreshDeferred(bool deferred) {
    if (m_statsRefreshDeferred == deferred) {
        return;
    }
    m_statsRefreshDeferred = deferred;
    if (!m_statsRefreshDeferred && m_statsRefreshPending) {
        FlushStats();
    }
}

void SparseVoxelWorld::FlushStats() {
    const bool wasDeferred = m_statsRefreshDeferred;
    m_statsRefreshDeferred = false;
    m_statsRefreshPending = false;
    RefreshStats();
    m_statsRefreshDeferred = wasDeferred;
}

uint32_t SparseVoxelWorld::GenerationClassQueueSize() const {
    uint32_t total = 0;
    for (const auto& queue : m_generationClassQueues) {
        total += static_cast<uint32_t>(queue.size());
    }
    return total;
}

uint32_t SparseVoxelWorld::UploadClassQueueSize() const {
    uint32_t total = 0;
    for (const auto& queue : m_uploadClassQueues) {
        total += static_cast<uint32_t>(queue.size());
    }
    return total;
}

uint32_t SparseVoxelWorld::SurfaceClassQueueSize() const {
    uint32_t total = 0;
    for (const auto& queue : m_surfaceClassQueues) {
        total += static_cast<uint32_t>(queue.size());
    }
    return total;
}

void SparseVoxelWorld::MarkUploadQueueOrderDirty() {
    m_uploadQueuePriorityDirty = true;
    m_uploadClassValueSortValid.fill(false);
    MarkQueueAccountingDirty();
}

void SparseVoxelWorld::MarkSurfaceQueueOrderDirty() {
    m_surfaceExtractionQueuePriorityDirty = true;
    m_surfaceClassValueSortValid.fill(false);
    MarkQueueAccountingDirty();
}

void SparseVoxelWorld::MarkQueueAccountingDirty() {
    m_queueClassStatsDirty = true;
}

void SparseVoxelWorld::RebuildQueueClassStats() {
    m_generationQueueClassCounts = {};
    m_uploadQueueClassCounts = {};
    m_surfaceQueueClassCounts = {};
    m_generationQueueLaneCounts = {};
    m_uploadQueueLaneCounts = {};
    m_surfaceQueueLaneCounts = {};

    const auto addQueueClass = [this](const BrickCoord& coord, QueueClassCounts& counts) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            return;
        }
        switch (record.residencyClass) {
            case SparseResidencyClass::Edited:
                ++counts.edited;
                break;
            case SparseResidencyClass::Collision:
                ++counts.collision;
                break;
            case SparseResidencyClass::Visible:
                ++counts.visible;
                break;
            case SparseResidencyClass::Speculative:
            default:
                ++counts.speculative;
                break;
        }
    };

    const auto addQueueLane = [this](const BrickCoord& coord, QueueLaneCounts& counts) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            return;
        }
        switch (record.streamingLane) {
            case SparseStreamingLane::PublicCritical:
                ++counts.publicCritical;
                break;
            case SparseStreamingLane::Visible:
                ++counts.visible;
                break;
            case SparseStreamingLane::Repair:
                ++counts.repair;
                break;
            case SparseStreamingLane::Prefetch:
                ++counts.prefetch;
                break;
            case SparseStreamingLane::Cache:
            default:
                ++counts.cache;
                break;
        }
    };

    for (const BrickCoord& coord : m_generationQueue) {
        addQueueClass(coord, m_generationQueueClassCounts);
        addQueueLane(coord, m_generationQueueLaneCounts);
    }
    for (const BrickCoord& coord : m_uploadQueue) {
        addQueueClass(coord, m_uploadQueueClassCounts);
        addQueueLane(coord, m_uploadQueueLaneCounts);
    }
    for (const auto& pending : m_pendingSurfaceBricks) {
        addQueueClass(pending.first, m_surfaceQueueClassCounts);
        addQueueLane(pending.first, m_surfaceQueueLaneCounts);
    }

    m_cachedGenerationQueueSize = static_cast<uint32_t>(m_generationQueue.size());
    m_cachedUploadQueueSize = static_cast<uint32_t>(m_uploadQueue.size());
    m_cachedSurfacePendingSize = static_cast<uint32_t>(m_pendingSurfaceBricks.size());
    m_queueClassStatsDirty = false;
}

void SparseVoxelWorld::QueueGenerationCoordBack(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    RemoveAllQueuedCoord(m_generationQueue, coord);
    RemoveAllClassQueueCoord(m_generationClassQueues, coord);
    if (m_config.streamingTicketGenerationOwnershipQueues) {
        RemoveFirstGenerationOwnershipQueueCoord(coord);
    }
    m_generationQueue.push_back(coord);
    m_generationClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    if (m_config.streamingTicketGenerationOwnershipQueues) {
        QueueGenerationOwnershipAliasIfRequested(coord);
    }
    m_generationQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

bool SparseVoxelWorld::RemoveFirstGenerationQueueCoord(const BrickCoord& coord) {
    if (RemoveAllQueuedCoord(m_generationQueue, coord)) {
        if (m_config.streamingTicketGenerationOwnershipQueues) {
            RemoveFirstGenerationOwnershipQueueCoord(coord);
        }
        m_generationQueuePriorityDirty = true;
        MarkQueueAccountingDirty();
        return true;
    }
    return false;
}

bool SparseVoxelWorld::RemoveFirstGenerationClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass)
{
    return RemoveAllClassQueueCoord(m_generationClassQueues, coord);
}

bool SparseVoxelWorld::RemoveFirstGenerationOwnershipQueueCoord(const BrickCoord& coord)
{
    if (!m_config.streamingTicketGenerationOwnershipQueues) {
        return false;
    }
    auto entryIt = m_generationOwnershipWorklistEntries.find(coord);
    if (entryIt == m_generationOwnershipWorklistEntries.end()) {
        return false;
    }

    const GenerationOwnershipWorklistEntry entry = entryIt->second;
    const size_t ownershipIndex = StreamingTicketOwnershipIndex(entry.ownership);
    std::vector<BrickCoord>& worklist = m_generationOwnershipWorklists[ownershipIndex];
    if (entry.index < worklist.size() && worklist[entry.index] == coord) {
        const BrickCoord movedCoord = worklist.back();
        worklist[entry.index] = movedCoord;
        worklist.pop_back();
        if (!(movedCoord == coord)) {
            auto movedIt = m_generationOwnershipWorklistEntries.find(movedCoord);
            if (movedIt != m_generationOwnershipWorklistEntries.end()) {
                movedIt->second.index = entry.index;
            }
        }
    } else {
        auto fallbackIt = std::find(worklist.begin(), worklist.end(), coord);
        if (fallbackIt != worklist.end()) {
            const size_t index = static_cast<size_t>(fallbackIt - worklist.begin());
            const BrickCoord movedCoord = worklist.back();
            worklist[index] = movedCoord;
            worklist.pop_back();
            if (!(movedCoord == coord)) {
                auto movedIt = m_generationOwnershipWorklistEntries.find(movedCoord);
                if (movedIt != m_generationOwnershipWorklistEntries.end()) {
                    movedIt->second.index = index;
                }
            }
        }
    }
    m_generationOwnershipWorklistEntries.erase(entryIt);
    return true;
}

void SparseVoxelWorld::QueueGenerationClassAliasIfRequested(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (record.state != BrickLifecycleState::Requested) {
        return;
    }
    RemoveAllClassQueueCoord(m_generationClassQueues, coord);
    if (m_config.streamingTicketGenerationOwnershipQueues) {
        RemoveFirstGenerationOwnershipQueueCoord(coord);
    }
    m_generationClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    if (m_config.streamingTicketGenerationOwnershipQueues) {
        QueueGenerationOwnershipAliasIfRequested(coord);
    }
    m_generationQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

void SparseVoxelWorld::QueueGenerationOwnershipAliasIfRequested(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (record.state != BrickLifecycleState::Requested) {
        return;
    }
    if (!m_config.streamingTicketGenerationOwnershipQueues) {
        return;
    }
    RemoveFirstGenerationOwnershipQueueCoord(coord);
    const StreamingTicketOwnership ownership = ClassifyStreamingTicketOwnership(record);
    const size_t ownershipIndex = StreamingTicketOwnershipIndex(ownership);
    std::vector<BrickCoord>& worklist = m_generationOwnershipWorklists[ownershipIndex];
    m_generationOwnershipWorklistEntries[coord] = {ownership, worklist.size()};
    worklist.push_back(coord);
    m_generationQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

bool SparseVoxelWorld::PopGenerationOwnershipWorklistCandidate(
    StreamingTicketOwnership ownership,
    BrickCoord* outCoord,
    BrickResidentRecord* outRecord)
{
    if (!m_config.streamingTicketGenerationOwnershipQueues || !outCoord || !outRecord) {
        return false;
    }

    std::vector<BrickCoord>& worklist = m_generationOwnershipWorklists[StreamingTicketOwnershipIndex(ownership)];
    while (!worklist.empty()) {
        const BrickCoord coord = worklist.back();
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record) ||
            record.state != BrickLifecycleState::Requested) {
            RemoveFirstGenerationOwnershipQueueCoord(coord);
            continue;
        }
        const StreamingTicketOwnership currentOwnership = ClassifyStreamingTicketOwnership(record);
        if (currentOwnership != ownership) {
            QueueGenerationOwnershipAliasIfRequested(coord);
            continue;
        }
        *outCoord = coord;
        *outRecord = record;
        return true;
    }
    return false;
}

uint32_t SparseVoxelWorld::PumpGenerationOwnershipQuota(
    StreamingTicketOwnership ownership,
    uint32_t maxBricks,
    uint32_t currentFrame,
    uint32_t* outProcessed)
{
    if (outProcessed) {
        *outProcessed = 0;
    }
    if (maxBricks == 0u || !m_config.streamingTicketGenerationOwnershipQueues) {
        return 0;
    }

    uint32_t generated = 0;
    uint32_t processed = 0;
    BrickCoord coord{};
    BrickResidentRecord record;
    while (processed < maxBricks &&
           PopGenerationOwnershipWorklistCandidate(ownership, &coord, &record)) {
        RemoveFirstGenerationQueueCoord(coord);
        RemoveFirstGenerationClassQueueCoord(coord, record.residencyClass);
        if (TryQueueAsyncExactGeneration(coord, record, currentFrame)) {
            ++processed;
            continue;
        }

        SparseResidencyClass generatedClass = SparseResidencyClass::Speculative;
        if (GenerateQueuedBrick(coord, &generatedClass)) {
            IncrementResidencyClassCounter(
                generatedClass,
                m_generatedSpeculativeBricksLastFrame,
                m_generatedVisibleBricksLastFrame,
                m_generatedCollisionBricksLastFrame,
                m_generatedEditedBricksLastFrame);
            ++generated;
        }
        ++processed;
    }

    if (outProcessed) {
        *outProcessed = processed;
    }
    return generated;
}

uint32_t SparseVoxelWorld::PumpGenerationOwnershipReservations(
    uint32_t maxBricks,
    uint32_t currentFrame,
    uint32_t* outProcessed)
{
    if (outProcessed) {
        *outProcessed = 0;
    }
    if (!m_config.streamingTicketGenerationOwnershipReservations ||
        !m_config.streamingTicketGenerationOwnershipQueues ||
        m_config.streamingTicketGenerationOwnershipReservationMax == 0u ||
        maxBricks == 0u) {
        return 0;
    }

    const uint32_t reservationMax =
        std::min(maxBricks, m_config.streamingTicketGenerationOwnershipReservationMax);

    constexpr std::array<StreamingTicketOwnership, 3> kCriticalOwnershipOrder{
        StreamingTicketOwnership::PublicCritical,
        StreamingTicketOwnership::UnknownCritical,
        StreamingTicketOwnership::SampledVisible,
    };

    uint32_t generated = 0;
    uint32_t processed = 0;
    for (StreamingTicketOwnership ownership : kCriticalOwnershipOrder) {
        uint32_t ownershipProcessed = 0;
        generated += PumpGenerationOwnershipQuota(
            ownership,
            reservationMax - processed,
            currentFrame,
            &ownershipProcessed);
        processed += ownershipProcessed;
        if (processed >= reservationMax) {
            break;
        }
    }

    if (outProcessed) {
        *outProcessed = processed;
    }
    return generated;
}

uint32_t SparseVoxelWorld::PumpGenerationOwnershipShares(
    uint32_t maxBricks,
    uint32_t currentFrame,
    uint32_t* outProcessed)
{
    if (outProcessed) {
        *outProcessed = 0;
    }
    if (!m_config.streamingTicketGenerationOwnershipShareScheduler ||
        !m_config.streamingTicketGenerationOwnershipQueues ||
        maxBricks == 0u) {
        return 0;
    }

    auto pump = [this, currentFrame](
        StreamingTicketOwnership ownership,
        uint32_t quota,
        uint32_t remaining,
        uint32_t& processed) {
        const uint32_t allowed = std::min(quota, remaining);
        if (allowed == 0u) {
            return 0u;
        }
        uint32_t ownershipProcessed = 0;
        const uint32_t generated = PumpGenerationOwnershipQuota(
            ownership,
            allowed,
            currentFrame,
            &ownershipProcessed);
        processed += ownershipProcessed;
        return generated;
    };

    uint32_t generated = 0;
    uint32_t processed = 0;
    const uint32_t publicTarget =
        m_config.streamingTicketGenerationOwnershipSharePublicMin;
    const uint32_t visibleTarget =
        m_config.streamingTicketGenerationOwnershipShareVisibleMax;
    const uint32_t prefetchTarget =
        m_config.streamingTicketGenerationOwnershipSharePrefetchMin;
    const uint32_t targetTotal = publicTarget + visibleTarget + prefetchTarget;
    if (targetTotal == 0u) {
        return 0;
    }

    uint32_t publicQuota = publicTarget;
    uint32_t visibleQuota = visibleTarget;
    uint32_t prefetchQuota = prefetchTarget;
    if (maxBricks < targetTotal) {
        publicQuota = static_cast<uint32_t>(
            (static_cast<uint64_t>(maxBricks) * publicTarget +
             static_cast<uint64_t>(targetTotal - 1u)) /
            targetTotal);
        publicQuota = std::min(publicQuota, maxBricks);
        const uint32_t afterPublic = maxBricks - publicQuota;
        visibleQuota = static_cast<uint32_t>(
            (static_cast<uint64_t>(maxBricks) * visibleTarget) /
            targetTotal);
        visibleQuota = std::min(visibleQuota, afterPublic);
        prefetchQuota = maxBricks - publicQuota - visibleQuota;
    }

    const uint32_t visibleDebtGate =
        m_config.streamingTicketGenerationOwnershipShareVisibleDebtGate;
    if (visibleDebtGate > 0u) {
        const size_t visibleDebtSize =
            m_generationOwnershipWorklists[StreamingTicketOwnershipIndex(
                StreamingTicketOwnership::UnknownCritical)].size() +
            m_generationOwnershipWorklists[StreamingTicketOwnershipIndex(
                StreamingTicketOwnership::SampledVisible)].size();
        const uint32_t visibleDebt = static_cast<uint32_t>(
            std::min<size_t>(
                visibleDebtSize,
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        if (visibleDebt > visibleDebtGate) {
            visibleQuota = std::min(maxBricks - publicQuota, visibleQuota + prefetchQuota);
            prefetchQuota = 0u;
        }
    }

    generated += pump(
        StreamingTicketOwnership::PublicCritical,
        publicQuota,
        maxBricks - processed,
        processed);

    uint32_t visibleRemaining = std::min(
        visibleQuota,
        maxBricks - processed);
    const StreamingTicketOwnership visibleOrder[] = {
        StreamingTicketOwnership::UnknownCritical,
        StreamingTicketOwnership::SampledVisible,
    };
    for (StreamingTicketOwnership ownership : visibleOrder) {
        const uint32_t before = processed;
        generated += pump(ownership, visibleRemaining, maxBricks - processed, processed);
        const uint32_t consumed = processed - before;
        visibleRemaining = consumed >= visibleRemaining ? 0u : visibleRemaining - consumed;
        if (visibleRemaining == 0u || processed >= maxBricks) {
            break;
        }
    }

    uint32_t lowPriorityRemaining = std::min(
        prefetchQuota,
        maxBricks - processed);
    const StreamingTicketOwnership lowPriorityOrder[] = {
        StreamingTicketOwnership::HiddenRepair,
        StreamingTicketOwnership::Prefetch,
        StreamingTicketOwnership::Cache,
    };
    for (StreamingTicketOwnership ownership : lowPriorityOrder) {
        const uint32_t before = processed;
        generated += pump(ownership, lowPriorityRemaining, maxBricks - processed, processed);
        const uint32_t consumed = processed - before;
        lowPriorityRemaining =
            consumed >= lowPriorityRemaining ? 0u : lowPriorityRemaining - consumed;
        if (lowPriorityRemaining == 0u || processed >= maxBricks) {
            break;
        }
    }

    while (processed < maxBricks) {
        const uint32_t before = processed;
        generated += pump(
            StreamingTicketOwnership::HiddenRepair,
            maxBricks - processed,
            maxBricks - processed,
            processed);
        generated += pump(
            StreamingTicketOwnership::Prefetch,
            maxBricks - processed,
            maxBricks - processed,
            processed);
        generated += pump(
            StreamingTicketOwnership::Cache,
            maxBricks - processed,
            maxBricks - processed,
            processed);
        generated += pump(
            StreamingTicketOwnership::PublicCritical,
            maxBricks - processed,
            maxBricks - processed,
            processed);
        for (StreamingTicketOwnership ownership : visibleOrder) {
            generated += pump(
                ownership,
                maxBricks - processed,
                maxBricks - processed,
                processed);
        }
        if (processed == before) {
            break;
        }
    }

    if (outProcessed) {
        *outProcessed = processed;
    }
    return generated;
}

bool SparseVoxelWorld::GenerateQueuedBrick(
    const BrickCoord& coord,
    SparseResidencyClass* outResidencyClass)
{
    BrickResidentRecord generationRecord;
    if (!m_pool.GetRecord(coord, &generationRecord)) {
        return false;
    }
    if (!m_pool.MarkGeneratingCPU(coord)) {
        return false;
    }

    GeneratedSparseBrick brick = GenerateBrickWithCachedTerrainColumns(coord);
    m_edits.ApplyToGeneratedBrick(brick);
    return ApplyGeneratedBrickPayload(coord, brick, outResidencyClass);
}

bool SparseVoxelWorld::ShouldDeferGeneratedDownstream(
    const BrickResidentRecord& record,
    const GeneratedSparseBrick& brick) const
{
    if (!m_config.streamingTicketLowPriorityDownstreamDeferral) {
        return false;
    }
    if (HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        return false;
    }
    return record.streamingLane == SparseStreamingLane::Cache ||
           record.streamingLane == SparseStreamingLane::Prefetch ||
           record.streamingLane == SparseStreamingLane::Repair;
}

bool SparseVoxelWorld::QueueDeferredGeneratedDownstream(const BrickCoord& coord)
{
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record) ||
        record.state != BrickLifecycleState::GeneratedCPU ||
        m_generated.find(coord) == m_generated.end()) {
        return false;
    }
    if (m_deferredGeneratedDownstreamSet.insert(coord).second) {
        m_deferredGeneratedDownstreamQueue.push_back(coord);
    }
    MarkQueueAccountingDirty();
    return true;
}

uint32_t SparseVoxelWorld::PromoteDeferredGeneratedDownstream(
    uint32_t maxBricks,
    uint32_t currentFrame)
{
    if (!m_config.streamingTicketLowPriorityDownstreamDeferral ||
        maxBricks == 0u ||
        m_deferredGeneratedDownstreamQueue.empty()) {
        return 0;
    }

    uint32_t promoted = 0;
    const size_t originalCount = m_deferredGeneratedDownstreamQueue.size();
    size_t scanned = 0;
    while (promoted < maxBricks &&
           scanned < originalCount &&
           !m_deferredGeneratedDownstreamQueue.empty()) {
        const BrickCoord coord = m_deferredGeneratedDownstreamQueue.front();
        m_deferredGeneratedDownstreamQueue.pop_front();
        m_deferredGeneratedDownstreamSet.erase(coord);
        ++scanned;

        if (PromoteDeferredGeneratedDownstreamCoordInternal(coord, currentFrame)) {
            ++promoted;
        }
    }

    if (promoted > 0u) {
        MarkQueueAccountingDirty();
    }
    return promoted;
}

uint32_t SparseVoxelWorld::PromoteDeferredGeneratedDownstreamForOwnership(
    bool ownershipCritical,
    uint32_t maxBricks,
    uint32_t currentFrame)
{
    if (!m_config.streamingTicketLowPriorityDownstreamDeferral ||
        maxBricks == 0u ||
        m_deferredGeneratedDownstreamQueue.empty()) {
        return 0;
    }

    uint32_t promoted = 0;
    const size_t originalCount = m_deferredGeneratedDownstreamQueue.size();
    size_t scanned = 0;
    while (promoted < maxBricks &&
           scanned < originalCount &&
           !m_deferredGeneratedDownstreamQueue.empty()) {
        const BrickCoord coord = m_deferredGeneratedDownstreamQueue.front();
        m_deferredGeneratedDownstreamQueue.pop_front();
        ++scanned;

        BrickResidentRecord record;
        auto generatedIt = m_generated.find(coord);
        if (!m_pool.GetRecord(coord, &record) ||
            record.state != BrickLifecycleState::GeneratedCPU ||
            generatedIt == m_generated.end()) {
            m_deferredGeneratedDownstreamSet.erase(coord);
            ++m_deferredGeneratedDownstreamStaleLastFrame;
            continue;
        }

        if (IsStreamingOwnershipCritical(record) != ownershipCritical) {
            m_deferredGeneratedDownstreamQueue.push_back(coord);
            continue;
        }

        m_deferredGeneratedDownstreamSet.erase(coord);
        if (PromoteDeferredGeneratedDownstreamCoordInternal(coord, currentFrame)) {
            ++promoted;
        }
    }

    if (promoted > 0u) {
        MarkQueueAccountingDirty();
    }
    return promoted;
}

bool SparseVoxelWorld::PromoteDeferredGeneratedDownstreamForCoord(
    const BrickCoord& coord,
    uint32_t currentFrame)
{
    if (!m_config.streamingTicketLowPriorityDownstreamDeferral ||
        m_deferredGeneratedDownstreamSet.find(coord) == m_deferredGeneratedDownstreamSet.end()) {
        return false;
    }
    m_deferredGeneratedDownstreamSet.erase(coord);
    RemoveAllQueuedCoord(m_deferredGeneratedDownstreamQueue, coord);
    const bool promoted = PromoteDeferredGeneratedDownstreamCoordInternal(coord, currentFrame);
    if (promoted) {
        MarkQueueAccountingDirty();
    }
    return promoted;
}

bool SparseVoxelWorld::PromoteDeferredGeneratedDownstreamIfCritical(
    const BrickCoord& coord,
    uint32_t currentFrame)
{
    if (!m_config.streamingTicketLowPriorityDownstreamDeferral ||
        m_deferredGeneratedDownstreamSet.find(coord) == m_deferredGeneratedDownstreamSet.end()) {
        return false;
    }

    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record) ||
        record.state != BrickLifecycleState::GeneratedCPU ||
        !IsStreamingOwnershipCritical(record)) {
        return false;
    }
    return PromoteDeferredGeneratedDownstreamForCoord(coord, currentFrame);
}

bool SparseVoxelWorld::PromoteDeferredGeneratedDownstreamCoordInternal(
    const BrickCoord& coord,
    uint32_t currentFrame)
{
    (void)currentFrame;
    BrickResidentRecord record;
    auto generatedIt = m_generated.find(coord);
    if (!m_pool.GetRecord(coord, &record) ||
        record.state != BrickLifecycleState::GeneratedCPU ||
        generatedIt == m_generated.end()) {
        ++m_deferredGeneratedDownstreamStaleLastFrame;
        return false;
    }
    if (!m_pool.QueueUpload(coord)) {
        ++m_deferredGeneratedDownstreamStaleLastFrame;
        return false;
    }

    const GeneratedSparseBrick& brick = generatedIt->second;
    uint32_t requiredTicketStages =
        kStreamingTicketStageCpuGenerated |
        kStreamingTicketStageGpuUploaded |
        kStreamingTicketStagePagePublished;
    uint32_t completedTicketStages = kStreamingTicketStageCpuGenerated;
    if (CanUseBuriedSolidSurfaceFastPath(coord, brick)) {
        MarkBuriedSolidSurfaceKnownEmpty(coord);
    } else if (!HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        requiredTicketStages |= kStreamingTicketStageSurfaceReady;
        m_pendingSurfaceBricks[coord] = brick;
        QueueSurfaceExtractionCoord(coord);
    } else {
        completedTicketStages |= kStreamingTicketStageSurfaceReady;
    }
    TouchStreamingTicket(coord, record, requiredTicketStages, completedTicketStages);
    QueueUploadCoordBack(coord);
    ++m_deferredGeneratedDownstreamPromotedLastFrame;
    return true;
}

bool SparseVoxelWorld::ApplyGeneratedBrickPayload(
    const BrickCoord& coord,
    const GeneratedSparseBrick& brick,
    SparseResidencyClass* outResidencyClass)
{
    BrickResidentRecord generationRecord;
    if (!m_pool.GetRecord(coord, &generationRecord)) {
        return false;
    }
    const SparseStreamingLane generatedLane = generationRecord.streamingLane;

    m_generated[coord] = brick;
    if (!m_pool.MarkGeneratedCPU(coord)) {
        return false;
    }
    if (ShouldDeferGeneratedDownstream(generationRecord, brick)) {
        TouchStreamingTicket(
            coord,
            generationRecord,
            kStreamingTicketStageCpuGenerated,
            kStreamingTicketStageCpuGenerated);
        QueueDeferredGeneratedDownstream(coord);
        if (outResidencyClass) {
            *outResidencyClass = generationRecord.residencyClass;
        }
        IncrementStreamingLaneCounter(
            generatedLane,
            m_generatedCacheLaneBricksLastFrame,
            m_generatedPrefetchLaneBricksLastFrame,
            m_generatedRepairLaneBricksLastFrame,
            m_generatedVisibleLaneBricksLastFrame,
            m_generatedPublicCriticalLaneBricksLastFrame);
        MarkQueueAccountingDirty();
        return true;
    }
    if (!m_pool.QueueUpload(coord)) {
        return false;
    }

    uint32_t requiredTicketStages =
        kStreamingTicketStageCpuGenerated |
        kStreamingTicketStageGpuUploaded |
        kStreamingTicketStagePagePublished;
    uint32_t completedTicketStages = kStreamingTicketStageCpuGenerated;
    if (CanUseBuriedSolidSurfaceFastPath(coord, brick)) {
        MarkBuriedSolidSurfaceKnownEmpty(coord);
    } else if (!HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        requiredTicketStages |= kStreamingTicketStageSurfaceReady;
        m_pendingSurfaceBricks[coord] = brick;
        QueueSurfaceExtractionCoord(coord);
    } else {
        completedTicketStages |= kStreamingTicketStageSurfaceReady;
    }
    TouchStreamingTicket(coord, generationRecord, requiredTicketStages, completedTicketStages);
    QueueUploadCoordBack(coord);
    if (outResidencyClass) {
        *outResidencyClass = generationRecord.residencyClass;
    }
    IncrementStreamingLaneCounter(
        generatedLane,
        m_generatedCacheLaneBricksLastFrame,
        m_generatedPrefetchLaneBricksLastFrame,
        m_generatedRepairLaneBricksLastFrame,
        m_generatedVisibleLaneBricksLastFrame,
        m_generatedPublicCriticalLaneBricksLastFrame);
    MarkQueueAccountingDirty();
    return true;
}

void SparseVoxelWorld::StartAsyncExactGenerationWorkerIfNeeded()
{
    if (!m_config.asyncExactGeneration || m_asyncExactGenerationThread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        m_asyncExactGenerationStop = false;
    }

    const SparseTerrainGenerator terrain = m_terrain;
    m_asyncExactGenerationThread = std::thread([this, terrain]() {
        TerrainSurfaceColumnCache workerColumnCache;
        workerColumnCache.reserve(8192);
        for (;;) {
            AsyncExactGenerationRequest request;
            {
                std::unique_lock<std::mutex> lock(m_asyncExactGenerationMutex);
                m_asyncExactGenerationCv.wait(lock, [this]() {
                    return m_asyncExactGenerationStop || !m_asyncExactGenerationQueue.empty();
                });
                if (m_asyncExactGenerationStop) {
                    break;
                }
                request = m_asyncExactGenerationQueue.front();
                m_asyncExactGenerationQueue.pop_front();
            }

            const auto start = std::chrono::steady_clock::now();
            GeneratedSparseBrick brick =
                GenerateExactBrickForConfig(terrain, request.coord, workerColumnCache);
            const auto elapsed = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (workerColumnCache.size() > 65536u) {
                workerColumnCache.clear();
                workerColumnCache.reserve(8192);
            }

            AsyncExactGenerationResult result;
            result.coord = request.coord;
            result.brick = std::move(brick);
            result.residencyClass = request.residencyClass;
            result.streamingLane = request.streamingLane;
            result.requestFrame = request.requestFrame;
            result.editRevision = request.editRevision;
            result.workerMs = elapsed;
            {
                std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
                if (m_asyncExactGenerationStop) {
                    break;
                }
                m_asyncExactGenerationResults.push_back(std::move(result));
            }
        }
    });
}

void SparseVoxelWorld::StopAsyncExactGenerationWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        m_asyncExactGenerationStop = true;
    }
    m_asyncExactGenerationCv.notify_all();
    if (m_asyncExactGenerationThread.joinable()) {
        m_asyncExactGenerationThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        m_asyncExactGenerationQueue.clear();
        m_asyncExactGenerationResults.clear();
        m_asyncExactGenerationPending.clear();
        m_asyncExactGenerationStop = false;
    }
}

void SparseVoxelWorld::StartAsyncSurfaceExtractionWorkerIfNeeded()
{
    if (!m_config.asyncSurfaceExtraction || !m_asyncSurfaceExtractionThreads.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        m_asyncSurfaceExtractionStop = false;
    }
    const uint32_t workerCount = std::max(1u, m_config.asyncSurfaceExtractionMaxWorkers);
    const SparseTerrainGenerator terrain = m_terrain;
    m_asyncSurfaceExtractionThreads.reserve(workerCount);
    for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
        m_asyncSurfaceExtractionThreads.emplace_back([this, terrain]() {
            SurfaceWorkerColumnCache workerColumnCache;
            workerColumnCache.reserve(8192);
            for (;;) {
                AsyncSurfaceExtractionRequest request;
                {
                    std::unique_lock<std::mutex> lock(m_asyncSurfaceExtractionMutex);
                    m_asyncSurfaceExtractionCv.wait(lock, [this]() {
                        return m_asyncSurfaceExtractionStop ||
                               !m_asyncSurfaceExtractionQueue.empty();
                    });
                    if (m_asyncSurfaceExtractionStop) {
                        break;
                    }
                    request = std::move(m_asyncSurfaceExtractionQueue.front());
                    m_asyncSurfaceExtractionQueue.pop_front();
                }

                const auto start = std::chrono::steady_clock::now();
                SparseSurfaceExtractionResult faces =
                    ExtractSurfaceNoEditWithTerrain(terrain, request.brick, workerColumnCache);
                const float elapsed = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (workerColumnCache.size() > 65536u) {
                    workerColumnCache.clear();
                    workerColumnCache.reserve(8192);
                }

                AsyncSurfaceExtractionResult result;
                result.coord = request.coord;
                result.residencyClass = request.residencyClass;
                result.pageIndex = request.pageIndex;
                result.generation = request.generation;
                result.editDependencyRevision = request.editDependencyRevision;
                result.brick = std::move(request.brick);
                result.faces = std::move(faces);
                result.workerMs = elapsed;
                {
                    std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
                    if (m_asyncSurfaceExtractionStop) {
                        break;
                    }
                    m_asyncSurfaceExtractionResults.push_back(std::move(result));
                }
            }
        });
    }
}

void SparseVoxelWorld::StopAsyncSurfaceExtractionWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        m_asyncSurfaceExtractionStop = true;
    }
    m_asyncSurfaceExtractionCv.notify_all();
    for (std::thread& worker : m_asyncSurfaceExtractionThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_asyncSurfaceExtractionThreads.clear();
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        m_asyncSurfaceExtractionQueue.clear();
        m_asyncSurfaceExtractionResults.clear();
        m_asyncSurfaceExtractionPending.clear();
        m_asyncSurfaceExtractionStop = false;
    }
}

void SparseVoxelWorld::StartPersistentExactGenerationWorkers(uint32_t workerCount)
{
    workerCount = std::clamp<uint32_t>(workerCount, 1u, m_config.parallelExactGenerationMaxWorkers);
    if (workerCount <= 1u) {
        StopPersistentExactGenerationWorkers();
        return;
    }
    if (m_persistentExactGenerationThreads.size() == static_cast<size_t>(workerCount)) {
        return;
    }

    StopPersistentExactGenerationWorkers();
    {
        std::lock_guard<std::mutex> lock(m_persistentExactGenerationMutex);
        m_persistentExactGenerationStop = false;
        m_persistentExactGenerationActive = false;
        m_persistentExactGenerationTerrain = nullptr;
        m_persistentExactGenerationCoords = nullptr;
        m_persistentExactGenerationBricks = nullptr;
        m_persistentExactGenerationNext = 0;
        m_persistentExactGenerationRemaining = 0;
    }
    m_persistentExactGenerationThreads.reserve(workerCount);
    for (uint32_t index = 0; index < workerCount; ++index) {
        m_persistentExactGenerationThreads.emplace_back(
            [this]() { PersistentExactGenerationWorkerLoop(); });
    }
}

void SparseVoxelWorld::StopPersistentExactGenerationWorkers()
{
    {
        std::lock_guard<std::mutex> lock(m_persistentExactGenerationMutex);
        m_persistentExactGenerationStop = true;
        m_persistentExactGenerationActive = false;
        m_persistentExactGenerationTerrain = nullptr;
        m_persistentExactGenerationCoords = nullptr;
        m_persistentExactGenerationBricks = nullptr;
        m_persistentExactGenerationNext = 0;
        m_persistentExactGenerationRemaining = 0;
    }
    m_persistentExactGenerationCv.notify_all();
    m_persistentExactGenerationDoneCv.notify_all();
    for (std::thread& worker : m_persistentExactGenerationThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_persistentExactGenerationThreads.clear();
    {
        std::lock_guard<std::mutex> lock(m_persistentExactGenerationMutex);
        m_persistentExactGenerationStop = false;
    }
}

void SparseVoxelWorld::PersistentExactGenerationWorkerLoop()
{
    TerrainSurfaceColumnCache columnCache;
    columnCache.reserve(8192);
    for (;;) {
        size_t jobIndex = 0;
        const SparseTerrainGenerator* terrain = nullptr;
        const std::vector<BrickCoord>* coords = nullptr;
        std::vector<GeneratedSparseBrick>* bricks = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_persistentExactGenerationMutex);
            m_persistentExactGenerationCv.wait(lock, [this]() {
                return m_persistentExactGenerationStop ||
                       (m_persistentExactGenerationActive &&
                        m_persistentExactGenerationNext <
                            (m_persistentExactGenerationCoords
                                ? m_persistentExactGenerationCoords->size()
                                : 0u));
            });
            if (m_persistentExactGenerationStop) {
                break;
            }
            terrain = m_persistentExactGenerationTerrain;
            coords = m_persistentExactGenerationCoords;
            bricks = m_persistentExactGenerationBricks;
            if (!terrain || !coords || !bricks ||
                m_persistentExactGenerationNext >= coords->size()) {
                continue;
            }
            jobIndex = m_persistentExactGenerationNext++;
        }

        const BrickCoord coord = (*coords)[jobIndex];
        (*bricks)[jobIndex] = GenerateExactBrickForConfig(*terrain, coord, columnCache);
        if (columnCache.size() > 65536u) {
            columnCache.clear();
            columnCache.reserve(8192);
        }

        {
            std::lock_guard<std::mutex> lock(m_persistentExactGenerationMutex);
            if (m_persistentExactGenerationRemaining > 0u) {
                --m_persistentExactGenerationRemaining;
            }
            if (m_persistentExactGenerationRemaining == 0u) {
                m_persistentExactGenerationActive = false;
                m_persistentExactGenerationTerrain = nullptr;
                m_persistentExactGenerationCoords = nullptr;
                m_persistentExactGenerationBricks = nullptr;
                m_persistentExactGenerationDoneCv.notify_one();
            }
        }
        m_persistentExactGenerationCv.notify_one();
    }
}

bool SparseVoxelWorld::GenerateExactBricksWithPersistentWorkers(
    const SparseTerrainGenerator& terrain,
    const std::vector<BrickCoord>& coords,
    std::vector<GeneratedSparseBrick>& bricks,
    uint32_t workerCount)
{
    if (!m_config.parallelExactGenerationPersistentWorkers ||
        coords.empty() ||
        bricks.size() < coords.size() ||
        workerCount <= 1u) {
        return false;
    }

    StartPersistentExactGenerationWorkers(workerCount);
    if (m_persistentExactGenerationThreads.empty()) {
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_persistentExactGenerationMutex);
        const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
        m_persistentExactGenerationDoneCv.wait(lock, [this]() {
            return !m_persistentExactGenerationActive;
        });
        m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
        m_persistentExactGenerationTerrain = &terrain;
        m_persistentExactGenerationCoords = &coords;
        m_persistentExactGenerationBricks = &bricks;
        m_persistentExactGenerationNext = 0;
        m_persistentExactGenerationRemaining = coords.size();
        m_persistentExactGenerationActive = true;
    }
    m_persistentExactGenerationCv.notify_all();

    {
        std::unique_lock<std::mutex> lock(m_persistentExactGenerationMutex);
        const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
        m_persistentExactGenerationDoneCv.wait(lock, [this]() {
            return !m_persistentExactGenerationActive;
        });
        m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
    }
    return true;
}

bool SparseVoxelWorld::EditOverlapsExactGenDependency(const BrickCoord& coord) const {
    if (m_edits.EditedBrickCount() == 0u) {
        return false;  // fast path: nothing edited anywhere
    }
    const int32_t h = kAsyncExactGenEditHaloBricks;
    for (int32_t dz = -h; dz <= h; ++dz) {
        for (int32_t dy = -h; dy <= h; ++dy) {
            for (int32_t dx = -h; dx <= h; ++dx) {
                if (m_edits.HasOverlay({coord.x + dx, coord.y + dy, coord.z + dz})) {
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t SparseVoxelWorld::MaxEditRevisionInExactGenDependency(const BrickCoord& coord) const {
    if (m_edits.EditedBrickCount() == 0u) {
        return 0;
    }
    uint64_t maxRev = 0;
    const int32_t h = kAsyncExactGenEditHaloBricks;
    for (int32_t dz = -h; dz <= h; ++dz) {
        for (int32_t dy = -h; dy <= h; ++dy) {
            for (int32_t dx = -h; dx <= h; ++dx) {
                maxRev = std::max(
                    maxRev,
                    m_edits.OverlayGlobalRevision({coord.x + dx, coord.y + dy, coord.z + dz}));
            }
        }
    }
    return maxRev;
}

bool SparseVoxelWorld::TryQueueAsyncExactGeneration(
    const BrickCoord& coord,
    const BrickResidentRecord& record,
    uint32_t currentFrame)
{
    const bool lowPriorityStreamingLane =
        record.streamingLane == SparseStreamingLane::Cache ||
        record.streamingLane == SparseStreamingLane::Prefetch ||
        record.streamingLane == SparseStreamingLane::Repair;
    const bool publicStreamingLane =
        record.streamingLane == SparseStreamingLane::Visible ||
        record.streamingLane == SparseStreamingLane::PublicCritical;
    const bool prefetchLaneAsyncAllowed =
        m_config.asyncExactGenerationPrefetchLane &&
        lowPriorityStreamingLane;
    if (!m_config.asyncExactGeneration ||
        m_config.asyncExactGenerationQueueMax == 0u ||
        record.state != BrickLifecycleState::Requested ||
        record.residencyClass == SparseResidencyClass::Edited ||
        record.residencyClass == SparseResidencyClass::Collision ||
        (publicStreamingLane && !m_config.asyncExactGenerationVisible) ||
        (record.residencyClass == SparseResidencyClass::Visible &&
         !m_config.asyncExactGenerationVisible &&
         !prefetchLaneAsyncAllowed) ||
        (m_config.asyncExactGenerationPerCoordEditGate
             ? EditOverlapsExactGenDependency(coord)
             : m_edits.EditedBrickCount() != 0u)) {
        return false;
    }

    StartAsyncExactGenerationWorkerIfNeeded();

    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        if (m_asyncExactGenerationPending.find(coord) != m_asyncExactGenerationPending.end()) {
            return true;
        }
        if (m_config.asyncExactGenerationMaxEnqueuePerFrame > 0u &&
            m_asyncExactGenerationEnqueuedLastFrame >= m_config.asyncExactGenerationMaxEnqueuePerFrame) {
            return false;
        }
        if (m_asyncExactGenerationPending.size() >= m_config.asyncExactGenerationQueueMax) {
            return false;
        }
    }

    if (!m_pool.MarkGeneratingCPU(coord)) {
        return false;
    }

    // This brick is committing to async despite passing all gates. If edits exist
    // anywhere, the OLD global gate would have forced it synchronous; the per-coord
    // gate let it go async because its own dependency neighborhood is edit-free.
    if (m_edits.EditedBrickCount() != 0u) {
        ++m_asyncExactGenEditGateGlobalWouldSyncLastFrame;
        ++m_asyncExactGenEditGatePerCoordAsyncLastFrame;
    }

    AsyncExactGenerationRequest request;
    request.coord = coord;
    request.residencyClass = record.residencyClass;
    request.streamingLane = record.streamingLane;
    request.requestFrame = currentFrame;
    request.editRevision = m_edits.RevisionSerial();
    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        if (!m_asyncExactGenerationPending.insert(coord).second) {
            return true;
        }
        m_asyncExactGenerationQueue.push_back(request);
    }
    m_asyncExactGenerationCv.notify_one();
    ++m_asyncExactGenerationEnqueuedLastFrame;
    switch (record.streamingLane) {
        case SparseStreamingLane::PublicCritical:
            ++m_asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame;
            break;
        case SparseStreamingLane::Visible:
            ++m_asyncExactGenerationEnqueuedVisibleLaneLastFrame;
            break;
        case SparseStreamingLane::Repair:
            ++m_asyncExactGenerationEnqueuedRepairLaneLastFrame;
            break;
        case SparseStreamingLane::Prefetch:
            ++m_asyncExactGenerationEnqueuedPrefetchLaneLastFrame;
            break;
        case SparseStreamingLane::Cache:
        default:
            ++m_asyncExactGenerationEnqueuedCacheLaneLastFrame;
            break;
    }
    MarkQueueAccountingDirty();
    return true;
}

uint32_t SparseVoxelWorld::ApplyAsyncExactGenerationCompletions(uint32_t currentFrame)
{
    if (!m_config.asyncExactGeneration) {
        return 0;
    }

    m_asyncExactGenerationStatsFrame = currentFrame;
    const uint32_t maxApply = std::max(1u, m_config.asyncExactGenerationMaxApplyPerFrame);
    const uint32_t maxLowPriorityApply =
        m_config.asyncExactGenerationMaxLowPriorityApplyPerFrame;
    const bool lowPriorityApplyLimited = maxLowPriorityApply > 0u;
    uint32_t applied = 0;
    uint32_t lowPriorityApplied = 0;
    const auto start = std::chrono::steady_clock::now();
    size_t maxScans = 0;
    {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        maxScans = m_asyncExactGenerationResults.size();
    }

    size_t scanned = 0;
    while (applied < maxApply && scanned < maxScans) {
        AsyncExactGenerationResult result;
        {
            std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
            if (m_asyncExactGenerationResults.empty()) {
                break;
            }
            result = std::move(m_asyncExactGenerationResults.front());
            m_asyncExactGenerationResults.pop_front();
        }
        ++scanned;

        const bool lowPriorityResult =
            result.streamingLane == SparseStreamingLane::Cache ||
            result.streamingLane == SparseStreamingLane::Prefetch ||
            result.streamingLane == SparseStreamingLane::Repair;
        if (lowPriorityApplyLimited &&
            lowPriorityResult &&
            lowPriorityApplied >= maxLowPriorityApply) {
            std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
            m_asyncExactGenerationResults.push_back(std::move(result));
            ++m_asyncExactGenerationDeferredLowPriorityApplyLastFrame;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
            m_asyncExactGenerationPending.erase(result.coord);
        }

        ++m_asyncExactGenerationCompletedLastFrame;
        m_asyncExactGenerationWorkerMsLastFrame += result.workerMs;

        BrickResidentRecord record;
        if (!m_pool.GetRecord(result.coord, &record) ||
            record.state != BrickLifecycleState::GeneratingCPU) {
            ++m_asyncExactGenerationDiscardedLastFrame;
            continue;
        }

        GeneratedSparseBrick brick = result.brick;
        SparseResidencyClass generatedClass = result.residencyClass;
        // Per-coord stale check: the worker generated against edit-free terrain at
        // dispatch (request.editRevision). If an edit has since landed on this brick's
        // dependency neighborhood (max overlay revision there now exceeds the dispatch
        // epoch), the async payload is stale -> regenerate synchronously + apply edits.
        // An edit ELSEWHERE in the world no longer triggers a needless regen (the old
        // global gate's waste). HasOverlay(coord) is a final belt-and-suspenders guard
        // for the brick's own overlay regardless of the halo epoch arithmetic.
        const bool staleAtCompletion =
            m_config.asyncExactGenerationPerCoordEditGate
                ? (m_edits.HasOverlay(result.coord) ||
                   MaxEditRevisionInExactGenDependency(result.coord) > result.editRevision)
                : (m_edits.EditedBrickCount() != 0u ||
                   m_edits.RevisionSerial() != result.editRevision);
        if (staleAtCompletion) {
            brick = GenerateBrickWithCachedTerrainColumns(result.coord);
            m_edits.ApplyToGeneratedBrick(brick);
            ++m_asyncExactGenerationSyncFallbackLastFrame;
            ++m_asyncExactGenEditStaleAtCompletionLastFrame;
        }

        if (ApplyGeneratedBrickPayload(result.coord, brick, &generatedClass)) {
            IncrementResidencyClassCounter(
                generatedClass,
                m_generatedSpeculativeBricksLastFrame,
                m_generatedVisibleBricksLastFrame,
                m_generatedCollisionBricksLastFrame,
                m_generatedEditedBricksLastFrame);
            switch (result.streamingLane) {
                case SparseStreamingLane::PublicCritical:
                    ++m_asyncExactGenerationAppliedPublicCriticalLaneLastFrame;
                    break;
                case SparseStreamingLane::Visible:
                    ++m_asyncExactGenerationAppliedVisibleLaneLastFrame;
                    break;
                case SparseStreamingLane::Repair:
                    ++m_asyncExactGenerationAppliedRepairLaneLastFrame;
                    break;
                case SparseStreamingLane::Prefetch:
                    ++m_asyncExactGenerationAppliedPrefetchLaneLastFrame;
                    break;
                case SparseStreamingLane::Cache:
                default:
                    ++m_asyncExactGenerationAppliedCacheLaneLastFrame;
                    break;
            }
            ++m_asyncExactGenerationAppliedLastFrame;
            if (lowPriorityResult) {
                ++lowPriorityApplied;
            }
            ++applied;
        } else {
            ++m_asyncExactGenerationDiscardedLastFrame;
        }
    }

    m_asyncExactGenerationApplyMsLastFrame += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (applied != 0u ||
        m_asyncExactGenerationCompletedLastFrame != 0u ||
        m_asyncExactGenerationDeferredLowPriorityApplyLastFrame != 0u) {
        RefreshStats();
    }
    return applied;
}

GeneratedSparseBrick SparseVoxelWorld::GenerateBrickWithTerrainColumnCache(
    const SparseTerrainGenerator& terrain,
    const BrickCoord& coord,
    TerrainSurfaceColumnCache& columnCache,
    TerrainColumnCacheFrameStats* columnStats)
{
    GeneratedSparseBrick brick;
    brick.coord = coord;
    brick.voxels.fill(Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));

    int32_t worldXByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldYByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldZByLocal[SPARSE_BRICK_SIZE] = {};
    for (uint8_t i = 0; i < SPARSE_BRICK_SIZE; ++i) {
        if (!TryWorldVoxelFromBrickLocal(coord.x, i, &worldXByLocal[i]) ||
            !TryWorldVoxelFromBrickLocal(coord.y, i, &worldYByLocal[i]) ||
            !TryWorldVoxelFromBrickLocal(coord.z, i, &worldZByLocal[i])) {
            SparseTerrainGenerator::ComputeOccupancyAndFlags(brick);
            return brick;
        }
    }

    std::array<float, SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE> heightByColumn = {};
    std::array<float, SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE> reliefByColumn = {};
    const int32_t minWorldY = worldYByLocal[0];
    const int32_t maxWorldY = worldYByLocal[SPARSE_BRICK_SIZE - 1];
    const auto cachedHeightAt = [
        &terrain,
        &columnCache,
        columnStats](int32_t worldX, int32_t worldZ) {
        const TerrainSurfaceColumnKey key{worldX, worldZ};
        auto columnIt = columnCache.find(key);
        if (columnIt != columnCache.end()) {
            if (columnStats) {
                ++columnStats->heightHits;
            }
            return columnIt->second.height;
        }

        if (columnStats) {
            ++columnStats->heightMisses;
        }
        TerrainSurfaceColumnCacheEntry entry;
        entry.height = terrain.HeightAt(worldX, worldZ);
        auto [insertedIt, inserted] = columnCache.emplace(key, entry);
        (void)inserted;
        return insertedIt->second.height;
    };
    const auto cachedReliefAt =
        [&terrain, &columnCache, &cachedHeightAt, columnStats](
            int32_t worldX,
            int32_t worldZ,
            int32_t sampleOffset)
    {
        const int32_t offset = std::max(1, sampleOffset);
        const TerrainSurfaceColumnKey key{worldX, worldZ};
        auto columnIt = columnCache.find(key);
        if (columnIt == columnCache.end()) {
            if (columnStats) {
                ++columnStats->heightMisses;
            }
            TerrainSurfaceColumnCacheEntry entry;
            entry.height = terrain.HeightAt(worldX, worldZ);
            columnIt = columnCache.emplace(key, entry).first;
        }
        if (columnIt->second.reliefValid && columnIt->second.reliefSampleOffset == offset) {
            if (columnStats) {
                ++columnStats->reliefHits;
            }
            return columnIt->second.relief;
        }

        if (columnStats) {
            ++columnStats->reliefMisses;
        }
        int32_t xMinus = worldX;
        int32_t xPlus = worldX;
        int32_t zMinus = worldZ;
        int32_t zPlus = worldZ;
        (void)TryStepInt32(worldX, -offset, &xMinus);
        (void)TryStepInt32(worldX, offset, &xPlus);
        (void)TryStepInt32(worldZ, -offset, &zMinus);
        (void)TryStepInt32(worldZ, offset, &zPlus);

        const float center = columnIt->second.height;
        float localMin = center;
        float localMax = center;
        const float samples[] = {
            cachedHeightAt(xMinus, worldZ),
            cachedHeightAt(xPlus, worldZ),
            cachedHeightAt(worldX, zMinus),
            cachedHeightAt(worldX, zPlus),
        };
        for (float height : samples) {
            localMin = std::min(localMin, height);
            localMax = std::max(localMax, height);
        }

        columnIt->second.relief = localMax - localMin;
        columnIt->second.reliefSampleOffset = offset;
        columnIt->second.reliefValid = true;
        return columnIt->second.relief;
    };
    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
            const int32_t worldX = worldXByLocal[x];
            const int32_t worldZ = worldZByLocal[z];
            const size_t index = static_cast<size_t>(x) +
                static_cast<size_t>(z) * SPARSE_BRICK_SIZE;
            const float height = cachedHeightAt(worldX, worldZ);
            heightByColumn[index] = height;
            if (maxWorldY > TERRAIN_MIN_Y + 2 &&
                static_cast<float>(minWorldY) <= height) {
                reliefByColumn[index] = cachedReliefAt(worldX, worldZ, 4);
            }
        }
    }

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
            const size_t columnIndex = static_cast<size_t>(x) +
                static_cast<size_t>(z) * SPARSE_BRICK_SIZE;
            const float height = heightByColumn[columnIndex];
            int32_t maxNonAirY = TERRAIN_MIN_Y + 2;
            int32_t solidMaxY = 0;
            if (TryFloorToInt32(height, &solidMaxY)) {
                maxNonAirY = std::max(maxNonAirY, solidMaxY);
            }
            if (height < static_cast<float>(SEA_LEVEL_Y - 2)) {
                maxNonAirY = std::max(maxNonAirY, SEA_LEVEL_Y);
            }
            if (maxNonAirY < minWorldY) {
                continue;
            }

            const int32_t clampedMaxNonAirY = std::min(maxNonAirY, maxWorldY);
            const int32_t worldX = worldXByLocal[x];
            const int32_t worldZ = worldZByLocal[z];
            for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
                const int32_t worldY = worldYByLocal[y];
                if (worldY > clampedMaxNonAirY) {
                    break;
                }
                brick.voxels[LocalVoxelIndex({x, y, z})] =
                    terrain.SampleGeneratedVoxelWithColumn(
                        worldX,
                        worldY,
                        worldZ,
                        height,
                        reliefByColumn[columnIndex]);
            }
        }
    }

    SparseTerrainGenerator::ComputeOccupancyAndFlags(brick);
    return brick;
}

GeneratedSparseBrick SparseVoxelWorld::GenerateExactBrickForConfig(
    const SparseTerrainGenerator& terrain,
    const BrickCoord& coord,
    TerrainSurfaceColumnCache& columnCache,
    TerrainColumnCacheFrameStats* columnStats) const
{
    if (m_config.directExactGeneration) {
        return terrain.GenerateBrick(coord);
    }
    return GenerateBrickWithTerrainColumnCache(terrain, coord, columnCache, columnStats);
}

GeneratedSparseBrick SparseVoxelWorld::GenerateBrickWithCachedTerrainColumns(const BrickCoord& coord) {
    TerrainColumnCacheFrameStats* columnStats = m_config.persistentTerrainColumnCache
        ? &m_terrainColumnCacheFrameStats
        : nullptr;
    return GenerateExactBrickForConfig(
        m_terrain,
        coord,
        m_surfaceTerrainColumnCache,
        columnStats);
}

void SparseVoxelWorld::QueueUploadCoordBack(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    RemoveAllQueuedCoord(m_uploadQueue, coord);
    RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
    RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
    m_uploadQueue.push_back(coord);
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_uploadOwnershipQueues[OwnershipCriticalQueueIndex(IsStreamingOwnershipCritical(record))].push_back(coord);
    MarkUploadQueueOrderDirty();
}

void SparseVoxelWorld::QueueUploadCoordFront(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    RemoveAllQueuedCoord(m_uploadQueue, coord);
    RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
    RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
    m_uploadQueue.push_front(coord);
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_front(coord);
    m_uploadOwnershipQueues[OwnershipCriticalQueueIndex(IsStreamingOwnershipCritical(record))].push_front(coord);
    MarkUploadQueueOrderDirty();
}

bool SparseVoxelWorld::RemoveFirstUploadQueueCoord(const BrickCoord& coord) {
    if (RemoveAllQueuedCoord(m_uploadQueue, coord)) {
        RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
        MarkUploadQueueOrderDirty();
        return true;
    }
    RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
    return false;
}

bool SparseVoxelWorld::RemoveFirstUploadClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass)
{
    return RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
}

void SparseVoxelWorld::QueueUploadClassAliasIfUploadQueued(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (record.state != BrickLifecycleState::UploadQueued) {
        return;
    }
    RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
    RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_uploadOwnershipQueues[OwnershipCriticalQueueIndex(IsStreamingOwnershipCritical(record))].push_back(coord);
    MarkUploadQueueOrderDirty();
}

void SparseVoxelWorld::QueueSurfaceExtractionCoord(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (m_surfaceExtractionQueuedSet.insert(coord).second) {
        m_surfaceExtractionQueue.push_back(coord);
        m_surfaceClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
        m_surfaceOwnershipQueues[OwnershipCriticalQueueIndex(IsStreamingOwnershipCritical(record))].push_back(coord);
    }
    MarkSurfaceQueueOrderDirty();
}

bool SparseVoxelWorld::RemoveFirstSurfaceQueueCoord(const BrickCoord& coord) {
    if (RemoveAllQueuedCoord(m_surfaceExtractionQueue, coord)) {
        RemoveAllClassQueueCoord(m_surfaceOwnershipQueues, coord);
        m_surfaceExtractionQueuedSet.erase(coord);
        MarkSurfaceQueueOrderDirty();
        return true;
    }
    RemoveAllClassQueueCoord(m_surfaceOwnershipQueues, coord);
    return false;
}

bool SparseVoxelWorld::RemoveFirstSurfaceClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass)
{
    if (!RemoveAllClassQueueCoord(m_surfaceClassQueues, coord)) {
        return false;
    }
    MarkSurfaceQueueOrderDirty();
    return true;
}

void SparseVoxelWorld::QueueSurfaceClassAliasIfPending(const BrickCoord& coord) {
    if (m_pendingSurfaceBricks.find(coord) == m_pendingSurfaceBricks.end()) {
        return;
    }
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
    RemoveAllClassQueueCoord(m_surfaceOwnershipQueues, coord);
    m_surfaceClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_surfaceOwnershipQueues[OwnershipCriticalQueueIndex(IsStreamingOwnershipCritical(record))].push_back(coord);
    MarkSurfaceQueueOrderDirty();
}

bool SparseVoxelWorld::PruneSurfaceExtractionQueuesIfNoPending() {
    if (!m_pendingSurfaceBricks.empty()) {
        return false;
    }

    bool hadQueuedWork =
        !m_surfaceExtractionQueue.empty() ||
        !m_surfaceExtractionQueuedSet.empty();
    for (const auto& classQueue : m_surfaceClassQueues) {
        hadQueuedWork = hadQueuedWork || !classQueue.empty();
    }
    for (const auto& ownershipQueue : m_surfaceOwnershipQueues) {
        hadQueuedWork = hadQueuedWork || !ownershipQueue.empty();
    }
    if (!hadQueuedWork) {
        return false;
    }

    m_surfaceExtractionQueue.clear();
    m_surfaceExtractionQueuedSet.clear();
    for (auto& classQueue : m_surfaceClassQueues) {
        classQueue.clear();
    }
    for (auto& ownershipQueue : m_surfaceOwnershipQueues) {
        ownershipQueue.clear();
    }
    m_surfaceExtractionQueuePriorityDirty = false;
    m_surfaceClassValueSortValid.fill(false);
    MarkQueueAccountingDirty();
    return true;
}

bool SparseVoxelWorld::ExtractSurfaceCoord(const BrickCoord& coord) {
    HEIGHTAT_SCOPE("ExtractSurfaceCoord");
    auto pendingIt = m_pendingSurfaceBricks.find(coord);
    if (pendingIt == m_pendingSurfaceBricks.end()) {
        RemoveFirstSurfaceQueueCoord(coord);
        RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
        m_surfaceExtractionQueuedSet.erase(coord);
        MarkQueueAccountingDirty();
        return false;
    }
    BrickResidentRecord surfaceRecord;
    const bool hasSurfaceRecord = m_pool.GetRecord(coord, &surfaceRecord);
    if (!hasSurfaceRecord ||
        (surfaceRecord.state != BrickLifecycleState::UploadQueued &&
         surfaceRecord.state != BrickLifecycleState::UploadingGPU &&
         surfaceRecord.state != BrickLifecycleState::Resident &&
         surfaceRecord.state != BrickLifecycleState::DirtyCPU &&
         surfaceRecord.state != BrickLifecycleState::DirtyGPU)) {
        m_pendingSurfaceBricks.erase(pendingIt);
        m_surfaceDirtyRegions.erase(coord);
        m_surfaceExtractionQueuedSet.erase(coord);
        RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
        RemoveAllClassQueueCoord(m_surfaceOwnershipQueues, coord);
        MarkQueueAccountingDirty();
        return false;
    }

    const auto inlineExtractionStart = std::chrono::steady_clock::now();
    GeneratedSparseBrick brick = std::move(pendingIt->second);
    m_pendingSurfaceBricks.erase(pendingIt);
    m_surfaceExtractionQueuedSet.erase(coord);
    MarkQueueAccountingDirty();
    constexpr int32_t kSurfaceHaloColumnSize = SPARSE_BRICK_SIZE + 2;
    int32_t brickWorldMinX = 0;
    int32_t brickWorldMinZ = 0;
    bool haloCacheValid =
        TryWorldVoxelFromBrickLocal(coord.x, 0, &brickWorldMinX) &&
        TryWorldVoxelFromBrickLocal(coord.z, 0, &brickWorldMinZ);
    int32_t haloMinX = 0;
    int32_t haloMinZ = 0;
    if (haloCacheValid &&
        (!TryStepInt32(brickWorldMinX, -1, &haloMinX) ||
         !TryStepInt32(brickWorldMinZ, -1, &haloMinZ))) {
        haloCacheValid = false;
    }
    std::array<float, kSurfaceHaloColumnSize * kSurfaceHaloColumnSize> haloHeights = {};
    std::array<float, kSurfaceHaloColumnSize * kSurfaceHaloColumnSize> haloRelief = {};
    if (haloCacheValid) {
        for (int32_t z = 0; z < kSurfaceHaloColumnSize; ++z) {
            for (int32_t x = 0; x < kSurfaceHaloColumnSize; ++x) {
                const int32_t worldX = haloMinX + x;
                const int32_t worldZ = haloMinZ + z;
                const size_t index = static_cast<size_t>(x + z * kSurfaceHaloColumnSize);
                haloHeights[index] = CachedTerrainHeightAt(worldX, worldZ);
                haloRelief[index] = CachedTerrainReliefAt(worldX, worldZ, 4);
            }
        }
    }
    bool haloMayContainEdits = false;
    for (int32_t dz = -1; dz <= 1 && !haloMayContainEdits; ++dz) {
        for (int32_t dy = -1; dy <= 1 && !haloMayContainEdits; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                BrickCoord neighborCoord;
                if (TryOffsetBrickCoord(coord, dx, dy, dz, &neighborCoord) &&
                    m_edits.HasOverlay(neighborCoord)) {
                    haloMayContainEdits = true;
                    break;
                }
            }
        }
    }
    auto neighborSampler = [this,
                            haloCacheValid,
                            haloMayContainEdits,
                            haloMinX,
                            haloMinZ,
                            &haloHeights,
                            &haloRelief](int32_t worldX, int32_t worldY, int32_t worldZ) {
        if (haloMayContainEdits) {
            uint32_t editedVoxel = 0;
            if (m_edits.TryGetVoxel(worldX, worldY, worldZ, &editedVoxel)) {
                return editedVoxel;
            }
        }
        if (haloCacheValid &&
            worldX >= haloMinX &&
            worldX < haloMinX + kSurfaceHaloColumnSize &&
            worldZ >= haloMinZ &&
            worldZ < haloMinZ + kSurfaceHaloColumnSize) {
            const int32_t localX = worldX - haloMinX;
            const int32_t localZ = worldZ - haloMinZ;
            const size_t index = static_cast<size_t>(localX + localZ * kSurfaceHaloColumnSize);
            return m_terrain.SampleGeneratedVoxelWithColumn(
                worldX,
                worldY,
                worldZ,
                haloHeights[index],
                haloRelief[index]);
        }
        return m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
    };
    auto surfaceDirtyIt = m_surfaceDirtyRegions.find(coord);
    if (surfaceDirtyIt != m_surfaceDirtyRegions.end()) {
        const SparseSurfaceLocalRegion dirtyRegion = surfaceDirtyIt->second;
        m_surfaceDirtyRegions.erase(surfaceDirtyIt);
        m_surfaceCache.UpdateBrickRegion(brick, dirtyRegion, neighborSampler);
    } else {
        m_surfaceCache.UpdateBrick(brick, neighborSampler);
    }
    if (hasSurfaceRecord) {
        IncrementResidencyClassCounter(
            surfaceRecord.residencyClass,
            m_surfaceSpeculativeBricksExtractedLastFrame,
            m_surfaceVisibleBricksExtractedLastFrame,
            m_surfaceCollisionBricksExtractedLastFrame,
            m_surfaceEditedBricksExtractedLastFrame);
    }
    MarkStreamingTicketStagesCompleted(coord, kStreamingTicketStageSurfaceReady);
    m_surfaceInlineExtractionMsLastFrame +=
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - inlineExtractionStart).count();
    ++m_surfaceInlineExtractionBricksLastFrame;
    return true;
}

bool SparseVoxelWorld::CanUseParallelSurfaceExtractionBatch(uint32_t maxBricks) const {
    return
        m_config.parallelSurfaceExtraction &&
        m_edits.EditedBrickCount() == 0u &&
        m_surfaceDirtyRegions.empty() &&
        maxBricks >= m_config.parallelSurfaceExtractionMinBricks &&
        m_pendingSurfaceBricks.size() >= static_cast<size_t>(m_config.parallelSurfaceExtractionMinBricks);
}

uint32_t SparseVoxelWorld::ExtractSurfaceBatchNoEdit(
    std::vector<SurfaceExtractionBatchItem>& pending,
    SparseSurfaceExtractionClassCounts* outClassCounts) {
    if (outClassCounts) {
        *outClassCounts = {};
    }
    if (pending.empty()) {
        return 0;
    }

    const SparseTerrainGenerator terrain = m_terrain;
    std::vector<SparseSurfaceExtractionResult> results(pending.size());
    const uint32_t workerCount = std::min<uint32_t>(
        static_cast<uint32_t>(pending.size()),
        m_config.parallelSurfaceExtractionMaxWorkers);
    std::vector<SurfaceWorkerColumnCache> workerColumnCaches(workerCount);
    for (SurfaceWorkerColumnCache& cache : workerColumnCaches) {
        cache.reserve(8192);
    }

    const auto parallelStart = std::chrono::steady_clock::now();
    if (workerCount <= 1u) {
        for (size_t index = 0; index < pending.size(); ++index) {
            results[index] = ExtractSurfaceNoEditWithTerrain(terrain, pending[index].brick, workerColumnCaches[0]);
        }
    } else {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        const size_t chunkSize =
            (pending.size() + static_cast<size_t>(workerCount) - 1u) /
            static_cast<size_t>(workerCount);
        for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
            const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
            const size_t end = std::min(pending.size(), begin + chunkSize);
            workers.emplace_back([
                &terrain,
                &pending,
                &results,
                &workerColumnCaches,
                workerIndex,
                begin,
                end]() {
                for (size_t index = begin; index < end; ++index) {
                    results[index] = ExtractSurfaceNoEditWithTerrain(
                        terrain,
                        pending[index].brick,
                        workerColumnCaches[workerIndex]);
                }
            });
        }
        const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
        for (std::thread& worker : workers) {
            worker.join();
        }
        m_surfaceExtractionWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
    }

    const uint32_t batchBricks = static_cast<uint32_t>(
        std::min<size_t>(pending.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    m_parallelSurfaceExtractionBricksLastFrame =
        std::min<uint32_t>(
            std::numeric_limits<uint32_t>::max() - m_parallelSurfaceExtractionBricksLastFrame,
            batchBricks) +
        m_parallelSurfaceExtractionBricksLastFrame;
    m_parallelSurfaceExtractionWorkersLastFrame =
        std::max(m_parallelSurfaceExtractionWorkersLastFrame, workerCount);
    m_parallelSurfaceExtractionWallMsLastFrame +=
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - parallelStart).count();

    uint32_t extracted = 0;
    for (size_t index = 0; index < pending.size(); ++index) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(pending[index].coord, &record)) {
            continue;
        }
        if (m_surfaceCache.UpdateBrickWithExtractedFaces(
                pending[index].brick,
                std::move(results[index]))) {
            IncrementResidencyClassCounter(
                pending[index].residencyClass,
                m_surfaceSpeculativeBricksExtractedLastFrame,
                m_surfaceVisibleBricksExtractedLastFrame,
                m_surfaceCollisionBricksExtractedLastFrame,
                m_surfaceEditedBricksExtractedLastFrame);
            if (outClassCounts) {
                switch (pending[index].residencyClass) {
                case SparseResidencyClass::Edited:
                    ++outClassCounts->edited;
                    break;
                case SparseResidencyClass::Collision:
                    ++outClassCounts->collision;
                    break;
                case SparseResidencyClass::Visible:
                    ++outClassCounts->visible;
                    break;
                case SparseResidencyClass::Speculative:
                    ++outClassCounts->speculative;
                    break;
                }
            }
            MarkStreamingTicketStagesCompleted(pending[index].coord, kStreamingTicketStageSurfaceReady);
            ++extracted;
        }
    }

    return extracted;
}

SparseVoxelWorld::StreamingTicketOwnership SparseVoxelWorld::ClassifyStreamingTicketOwnership(
    const BrickResidentRecord& record) const
{
    switch (record.streamingLane) {
        case SparseStreamingLane::PublicCritical:
            return StreamingTicketOwnership::PublicCritical;
        case SparseStreamingLane::Visible:
            return StreamingTicketOwnership::SampledVisible;
        case SparseStreamingLane::Repair:
            return StreamingTicketOwnership::HiddenRepair;
        case SparseStreamingLane::Prefetch:
            return StreamingTicketOwnership::Prefetch;
        case SparseStreamingLane::Cache:
        default:
            break;
    }

    if (record.residencyClass == SparseResidencyClass::Visible ||
        record.residencyClass == SparseResidencyClass::Collision ||
        record.residencyClass == SparseResidencyClass::Edited) {
        return StreamingTicketOwnership::UnknownCritical;
    }
    return StreamingTicketOwnership::Cache;
}

bool SparseVoxelWorld::IsStreamingOwnershipCritical(const BrickResidentRecord& record) const
{
    switch (ClassifyStreamingTicketOwnership(record)) {
        case StreamingTicketOwnership::PublicCritical:
        case StreamingTicketOwnership::UnknownCritical:
        case StreamingTicketOwnership::SampledVisible:
            return true;
        case StreamingTicketOwnership::HiddenRepair:
        case StreamingTicketOwnership::FallbackValid:
        case StreamingTicketOwnership::Prefetch:
        case StreamingTicketOwnership::Cache:
        default:
            return false;
    }
}

size_t SparseVoxelWorld::StreamingTicketOwnershipIndex(StreamingTicketOwnership ownership)
{
    const size_t index = static_cast<size_t>(ownership);
    return index < kStreamingTicketOwnershipCount ? index : 0u;
}

size_t SparseVoxelWorld::StreamingTicketStageIndex(uint32_t stageBit)
{
    switch (stageBit) {
        case kStreamingTicketStageCpuGenerated:
            return 0u;
        case kStreamingTicketStageGpuUploaded:
            return 1u;
        case kStreamingTicketStageSurfaceReady:
            return 2u;
        case kStreamingTicketStagePagePublished:
            return 3u;
        default:
            return kStreamingTicketStageCount;
    }
}

void SparseVoxelWorld::AddStreamingTicketPendingStageDemand(const StreamingWorkTicket& ticket)
{
    if (!m_config.streamingTicketStageDemandAccounting) {
        return;
    }
    const uint32_t pendingStages = ticket.requiredStages & ~ticket.completedStages;
    const size_t ownershipIndex = StreamingTicketOwnershipIndex(ticket.ownership);
    const uint32_t stageBits[] = {
        kStreamingTicketStageCpuGenerated,
        kStreamingTicketStageGpuUploaded,
        kStreamingTicketStageSurfaceReady,
        kStreamingTicketStagePagePublished,
    };
    for (uint32_t stageBit : stageBits) {
        if ((pendingStages & stageBit) == 0u) {
            continue;
        }
        const size_t stageIndex = StreamingTicketStageIndex(stageBit);
        if (stageIndex < kStreamingTicketStageCount) {
            ++m_streamingTicketPendingStageOwnershipCounts[stageIndex][ownershipIndex];
        }
    }
}

void SparseVoxelWorld::RemoveStreamingTicketPendingStageDemand(const StreamingWorkTicket& ticket)
{
    if (!m_config.streamingTicketStageDemandAccounting) {
        return;
    }
    const uint32_t pendingStages = ticket.requiredStages & ~ticket.completedStages;
    const size_t ownershipIndex = StreamingTicketOwnershipIndex(ticket.ownership);
    const uint32_t stageBits[] = {
        kStreamingTicketStageCpuGenerated,
        kStreamingTicketStageGpuUploaded,
        kStreamingTicketStageSurfaceReady,
        kStreamingTicketStagePagePublished,
    };
    for (uint32_t stageBit : stageBits) {
        if ((pendingStages & stageBit) == 0u) {
            continue;
        }
        const size_t stageIndex = StreamingTicketStageIndex(stageBit);
        if (stageIndex >= kStreamingTicketStageCount) {
            continue;
        }
        uint32_t& count = m_streamingTicketPendingStageOwnershipCounts[stageIndex][ownershipIndex];
        if (count > 0u) {
            --count;
        }
    }
}

void SparseVoxelWorld::TouchStreamingTicket(
    const BrickCoord& coord,
    const BrickResidentRecord& record,
    uint32_t requiredStages,
    uint32_t completedStages)
{
    if (!m_config.streamingTicketScheduler) {
        return;
    }

    auto [ticketIt, inserted] = m_streamingTickets.try_emplace(coord);
    StreamingWorkTicket& ticket = ticketIt->second;
    if (!inserted) {
        RemoveStreamingTicketPendingStageDemand(ticket);
    }
    if (ticket.requestFrame == 0u) {
        ticket.requestFrame = record.lastTouchedFrame;
    }
    ticket.residencyClass = record.residencyClass;
    ticket.streamingLane = record.streamingLane;
    ticket.ownership = ClassifyStreamingTicketOwnership(record);
    ticket.requiredStages |= requiredStages;
    ticket.completedStages |= completedStages;
    ticket.lastTouchedFrame = std::max(ticket.lastTouchedFrame, record.lastTouchedFrame);
    ticket.lastUpdatedFrame = std::max(ticket.lastUpdatedFrame, record.lastTouchedFrame);
    ticket.editRevision = m_edits.RevisionSerial();

    if ((ticket.requiredStages & ~ticket.completedStages) == 0u &&
        ticket.requiredStages != 0u) {
        m_streamingTickets.erase(ticketIt);
        ++m_streamingTicketCompletedLastFrame;
    } else {
        AddStreamingTicketPendingStageDemand(ticket);
    }
}

void SparseVoxelWorld::UpdateStreamingTicketFromRecord(
    const BrickCoord& coord,
    const BrickResidentRecord& record)
{
    if (!m_config.streamingTicketScheduler) {
        return;
    }

    auto ticketIt = m_streamingTickets.find(coord);
    if (ticketIt == m_streamingTickets.end()) {
        return;
    }
    RemoveStreamingTicketPendingStageDemand(ticketIt->second);
    ticketIt->second.residencyClass = record.residencyClass;
    ticketIt->second.streamingLane = record.streamingLane;
    ticketIt->second.ownership = ClassifyStreamingTicketOwnership(record);
    ticketIt->second.lastTouchedFrame =
        std::max(ticketIt->second.lastTouchedFrame, record.lastTouchedFrame);
    ticketIt->second.lastUpdatedFrame =
        std::max(ticketIt->second.lastUpdatedFrame, record.lastTouchedFrame);
    AddStreamingTicketPendingStageDemand(ticketIt->second);
}

void SparseVoxelWorld::MarkStreamingTicketStagesCompleted(
    const BrickCoord& coord,
    uint32_t completedStages)
{
    if (!m_config.streamingTicketScheduler || completedStages == 0u) {
        return;
    }

    auto ticketIt = m_streamingTickets.find(coord);
    if (ticketIt == m_streamingTickets.end()) {
        return;
    }
    RemoveStreamingTicketPendingStageDemand(ticketIt->second);
    ticketIt->second.completedStages |= completedStages;
    if ((ticketIt->second.requiredStages & ~ticketIt->second.completedStages) == 0u &&
        ticketIt->second.requiredStages != 0u) {
        m_streamingTickets.erase(ticketIt);
        ++m_streamingTicketCompletedLastFrame;
    } else {
        AddStreamingTicketPendingStageDemand(ticketIt->second);
    }
}

void SparseVoxelWorld::RemoveStreamingTicket(const BrickCoord& coord)
{
    if (!m_config.streamingTicketScheduler) {
        return;
    }
    auto ticketIt = m_streamingTickets.find(coord);
    if (ticketIt == m_streamingTickets.end()) {
        return;
    }
    RemoveStreamingTicketPendingStageDemand(ticketIt->second);
    m_streamingTickets.erase(ticketIt);
}

int64_t SparseVoxelWorld::StreamingTicketOwnershipScore(StreamingTicketOwnership ownership) const
{
    switch (ownership) {
        case StreamingTicketOwnership::PublicCritical:
            return 700000000000000ll;
        case StreamingTicketOwnership::UnknownCritical:
            return 650000000000000ll;
        case StreamingTicketOwnership::SampledVisible:
            return 600000000000000ll;
        case StreamingTicketOwnership::HiddenRepair:
            return 400000000000000ll;
        case StreamingTicketOwnership::FallbackValid:
            return 300000000000000ll;
        case StreamingTicketOwnership::Prefetch:
            return 200000000000000ll;
        case StreamingTicketOwnership::Cache:
        default:
            return 100000000000000ll;
    }
}

int64_t SparseVoxelWorld::StreamingTicketStageScore(
    const StreamingWorkTicket& ticket,
    uint32_t stageBit) const
{
    if (stageBit == 0u) {
        return 0ll;
    }
    const bool required = (ticket.requiredStages & stageBit) != 0u;
    const bool completed = (ticket.completedStages & stageBit) != 0u;
    if (required && !completed) {
        return 50000000000000ll;
    }
    if (required) {
        return 1000000000000ll;
    }
    return 0ll;
}

void SparseVoxelWorld::SortQueueByStreamingTickets(
    std::deque<BrickCoord>& queue,
    uint32_t stageBit,
    const BrickCoord* focus,
    uint32_t currentFrame,
    bool valueSort,
    size_t frontCount)
{
    if (!m_config.streamingTicketProtectedScheduling || queue.size() <= 1) {
        return;
    }

    struct QueuedBrick {
        BrickCoord coord;
        int64_t ticketScore = 0;
        int64_t baseScore = 0;
    };

    std::vector<QueuedBrick> sorted;
    sorted.reserve(queue.size());
    std::unordered_set<BrickCoord, BrickCoordHash> seen;
    bool hasProtectedTicket = false;
    for (const BrickCoord& coord : queue) {
        if (!seen.insert(coord).second) {
            continue;
        }
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        int64_t ticketScore = 0ll;
        auto ticketIt = m_streamingTickets.find(coord);
        if (ticketIt != m_streamingTickets.end()) {
            ticketScore =
                StreamingTicketOwnershipScore(ticketIt->second.ownership) +
                StreamingTicketStageScore(ticketIt->second, stageBit);
            switch (ticketIt->second.ownership) {
                case StreamingTicketOwnership::PublicCritical:
                case StreamingTicketOwnership::UnknownCritical:
                case StreamingTicketOwnership::SampledVisible:
                case StreamingTicketOwnership::HiddenRepair:
                case StreamingTicketOwnership::FallbackValid:
                    hasProtectedTicket = true;
                    break;
                case StreamingTicketOwnership::Prefetch:
                case StreamingTicketOwnership::Cache:
                default:
                    break;
            }
            if (currentFrame >= ticketIt->second.requestFrame) {
                ticketScore += static_cast<int64_t>(
                    std::min<uint32_t>(currentFrame - ticketIt->second.requestFrame, 100000u)) *
                    1000000ll;
            }
        }
        const int64_t baseScore =
            valueSort && focus
                ? UploadValueScore(
                    record,
                    *focus,
                    currentFrame,
                    m_config.streamingLaneQueuePriority)
                : QueuePriorityScore(
                    record,
                    currentFrame,
                    m_config.streamingLaneQueuePriority);
        sorted.push_back({coord, ticketScore, baseScore});
    }

    if (!hasProtectedTicket) {
        return;
    }

    const auto better = [](const QueuedBrick& a, const QueuedBrick& b) {
        if (a.ticketScore != b.ticketScore) {
            return a.ticketScore > b.ticketScore;
        }
        if (a.baseScore != b.baseScore) {
            return a.baseScore > b.baseScore;
        }
        return a.coord < b.coord;
    };

    const size_t selectedCount = frontCount == 0
        ? sorted.size()
        : std::min(frontCount, sorted.size());
    if (selectedCount == sorted.size()) {
        std::sort(sorted.begin(), sorted.end(), better);
    } else if (selectedCount > 0) {
        std::partial_sort(sorted.begin(), sorted.begin() + selectedCount, sorted.end(), better);
    }

    queue.clear();
    for (const QueuedBrick& queued : sorted) {
        queue.push_back(queued.coord);
    }
    ++m_streamingTicketProtectedSortsLastFrame;
}

bool SparseVoxelWorld::RequestBrick(const BrickCoord& coord) {
    return RequestBrickDetailed(coord) != SparseBrickRequestResult::Rejected;
}

bool SparseVoxelWorld::TrySkipKnownEmptyRequest(const BrickCoord& coord)
{
    if (m_edits.HasOverlay(coord)) {
        return false;
    }
    if (m_knownEmptyGeneratedBricks.find(coord) == m_knownEmptyGeneratedBricks.end() &&
        !m_terrain.IsDefinitelyEmptyBrick(coord)) {
        return false;
    }
    m_knownEmptyGeneratedBricks.insert(coord);
    ++m_emptyRequestsSkippedLastFrame;
    RefreshStats();
    return true;
}

SparseBrickRequestResult SparseVoxelWorld::RequestBrickDetailed(
    const BrickCoord& coord,
    bool allowEmptyFastPath)
{
    if (m_pool.TryGetPage(coord)) {
        return SparseBrickRequestResult::AlreadyResident;
    }

    if (allowEmptyFastPath && TrySkipKnownEmptyRequest(coord)) {
        return SparseBrickRequestResult::SkippedKnownEmpty;
    }

    const uint32_t page = m_pool.AllocatePage(coord);
    if (page == INVALID_BRICK_PAGE) {
        return SparseBrickRequestResult::Rejected;
    }

    QueueGenerationCoordBack(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        TouchStreamingTicket(coord, record, kStreamingTicketStageCpuGenerated);
    }
    RefreshStats();
    return SparseBrickRequestResult::Allocated;
}

uint32_t SparseVoxelWorld::PumpGeneration(uint32_t maxBricks, uint32_t currentFrame) {
    ApplyAsyncExactGenerationCompletions(currentFrame);
    ApplyAsyncSurfaceExtractionCompletions();
    m_parallelExactGenerationBricksLastFrame = 0;
    m_parallelExactGenerationWorkersLastFrame = 0;
    m_parallelExactGenerationWallMsLastFrame = 0.0f;
    m_persistentExactGenerationWaitMsLastFrame = 0.0f;
    uint32_t generated = 0;
    uint32_t processed = 0;
    uint32_t ownershipProcessed = 0;
    if (m_config.streamingTicketGenerationOwnershipShareScheduler) {
        generated += PumpGenerationOwnershipShares(maxBricks, currentFrame, &ownershipProcessed);
    } else {
        generated += PumpGenerationOwnershipReservations(maxBricks, currentFrame, &ownershipProcessed);
    }
    processed += ownershipProcessed;
    if (processed < maxBricks && m_generationQueuePriorityDirty) {
        SortQueuedBricksByPriority(
            m_generationQueue,
            m_pool,
            currentFrame,
            m_config.streamingLaneQueuePriority);
        SortQueueByStreamingTickets(
            m_generationQueue,
            kStreamingTicketStageCpuGenerated,
            nullptr,
            currentFrame,
            false);
        m_generationQueuePriorityDirty = false;
    }

    const bool parallelGenerationAllowed =
        m_config.parallelExactGeneration &&
        maxBricks >= m_config.parallelExactGenerationMinBricks &&
        m_generationQueue.size() >= static_cast<size_t>(m_config.parallelExactGenerationMinBricks) &&
        (m_config.parallelExactGenerationEditAware || m_edits.EditedBrickCount() == 0u);
    if (processed < maxBricks && parallelGenerationAllowed) {
        struct PendingExactGeneration {
            BrickCoord coord;
            SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        };

        std::vector<PendingExactGeneration> pending;
        pending.reserve(maxBricks);
        BrickCoord coord{};
        while (processed < maxBricks &&
               pending.size() < static_cast<size_t>(maxBricks) &&
               PopFrontQueuedBrick(m_generationQueue, m_pool, &coord)) {
            BrickResidentRecord generationRecord;
            if (!m_pool.GetRecord(coord, &generationRecord)) {
                continue;
            }
            RemoveFirstGenerationClassQueueCoord(coord, generationRecord.residencyClass);
            if (TryQueueAsyncExactGeneration(coord, generationRecord, currentFrame)) {
                ++processed;
                continue;
            }
            if (!m_pool.MarkGeneratingCPU(coord)) {
                continue;
            }
            pending.push_back({coord, generationRecord.residencyClass});
            ++processed;
        }

        if (!pending.empty()) {
            const SparseTerrainGenerator terrain = m_terrain;
            std::vector<GeneratedSparseBrick> bricks(pending.size());
            std::vector<float> elapsedMs(pending.size(), 0.0f);
            std::vector<BrickCoord> persistentWorkerCoords;
            const bool useWorkerThreads =
                pending.size() >= static_cast<size_t>(m_config.parallelExactGenerationMinBricks) &&
                m_config.parallelExactGenerationMaxWorkers > 1u;
            const uint32_t workerCount = useWorkerThreads
                ? std::min<uint32_t>(
                    static_cast<uint32_t>(pending.size()),
                    m_config.parallelExactGenerationMaxWorkers)
                : 1u;
            std::vector<TerrainSurfaceColumnCache> workerColumnCaches(workerCount);
            for (TerrainSurfaceColumnCache& cache : workerColumnCaches) {
                cache.reserve(8192);
            }
            const auto parallelStart = std::chrono::steady_clock::now();
            if (workerCount <= 1u) {
                for (size_t index = 0; index < pending.size(); ++index) {
                    const auto generateStart = std::chrono::steady_clock::now();
                    bricks[index] = GenerateExactBrickForConfig(
                        terrain,
                        pending[index].coord,
                        workerColumnCaches[0]);
                    elapsedMs[index] = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - generateStart).count();
                }
            } else if (m_config.parallelExactGenerationPersistentWorkers) {
                persistentWorkerCoords.reserve(pending.size());
                for (const PendingExactGeneration& item : pending) {
                    persistentWorkerCoords.push_back(item.coord);
                }
                if (!GenerateExactBricksWithPersistentWorkers(
                        terrain,
                        persistentWorkerCoords,
                        bricks,
                        workerCount)) {
                    std::vector<std::thread> workers;
                    workers.reserve(workerCount);
                    const size_t chunkSize =
                        (pending.size() + static_cast<size_t>(workerCount) - 1u) /
                        static_cast<size_t>(workerCount);
                    for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
                        const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
                        const size_t end = std::min(pending.size(), begin + chunkSize);
                        workers.emplace_back([
                            this,
                            &terrain,
                            &pending,
                            &bricks,
                            &elapsedMs,
                            &workerColumnCaches,
                            workerIndex,
                            begin,
                            end]() {
                            for (size_t index = begin; index < end; ++index) {
                                const auto generateStart = std::chrono::steady_clock::now();
                                bricks[index] = GenerateExactBrickForConfig(
                                    terrain,
                                    pending[index].coord,
                                    workerColumnCaches[workerIndex]);
                                elapsedMs[index] = std::chrono::duration<float, std::milli>(
                                    std::chrono::steady_clock::now() - generateStart).count();
                            }
                        });
                    }
                    const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
                    for (std::thread& worker : workers) {
                        worker.join();
                    }
                    m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
                }
            } else {
                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                const size_t chunkSize =
                    (pending.size() + static_cast<size_t>(workerCount) - 1u) /
                    static_cast<size_t>(workerCount);
                for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
                    const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
                    const size_t end = std::min(pending.size(), begin + chunkSize);
                    workers.emplace_back([
                        this,
                        &terrain,
                        &pending,
                        &bricks,
                        &elapsedMs,
                        &workerColumnCaches,
                        workerIndex,
                        begin,
                        end]() {
                        for (size_t index = begin; index < end; ++index) {
                            const auto generateStart = std::chrono::steady_clock::now();
                            bricks[index] = GenerateExactBrickForConfig(
                                terrain,
                                pending[index].coord,
                                workerColumnCaches[workerIndex]);
                            elapsedMs[index] = std::chrono::duration<float, std::milli>(
                                std::chrono::steady_clock::now() - generateStart).count();
                        }
                    });
                }
                const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
                for (std::thread& worker : workers) {
                    worker.join();
                }
                m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
            }
            (void)elapsedMs;
            m_parallelExactGenerationBricksLastFrame =
                static_cast<uint32_t>(
                    std::min<size_t>(pending.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            m_parallelExactGenerationWorkersLastFrame = workerCount;
            m_parallelExactGenerationWallMsLastFrame =
                std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - parallelStart).count();

            for (size_t index = 0; index < pending.size(); ++index) {
                // Workers generate PRISTINE bricks (no m_edits access); apply the edit
                // overlay serially here on the main thread before storing the payload.
                // No-op when the brick has no edits, so this is correct whether or not
                // parallelExactGenerationEditAware is on.
                m_edits.ApplyToGeneratedBrick(bricks[index]);
                SparseResidencyClass generatedClass = pending[index].residencyClass;
                if (ApplyGeneratedBrickPayload(pending[index].coord, bricks[index], &generatedClass)) {
                    IncrementResidencyClassCounter(
                        generatedClass,
                        m_generatedSpeculativeBricksLastFrame,
                        m_generatedVisibleBricksLastFrame,
                        m_generatedCollisionBricksLastFrame,
                        m_generatedEditedBricksLastFrame);
                    ++generated;
                }
            }
        }
    }

    BrickCoord coord{};
    while (processed < maxBricks &&
           PopFrontQueuedBrick(m_generationQueue, m_pool, &coord)) {

        BrickResidentRecord generationRecord;
        if (m_pool.GetRecord(coord, &generationRecord)) {
            RemoveFirstGenerationClassQueueCoord(coord, generationRecord.residencyClass);
        }
        if (TryQueueAsyncExactGeneration(coord, generationRecord, currentFrame)) {
            ++processed;
            continue;
        }
        SparseResidencyClass generatedClass = SparseResidencyClass::Speculative;
        if (GenerateQueuedBrick(coord, &generatedClass)) {
            IncrementResidencyClassCounter(
                generatedClass,
                m_generatedSpeculativeBricksLastFrame,
                m_generatedVisibleBricksLastFrame,
                m_generatedCollisionBricksLastFrame,
                m_generatedEditedBricksLastFrame);
            ++generated;
            ++processed;
        }
    }

    RefreshStats();
    return generated;
}

uint32_t SparseVoxelWorld::PumpGenerationForCoord(const BrickCoord& coord)
{
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record) ||
        record.state != BrickLifecycleState::Requested) {
        return 0;
    }

    RemoveFirstGenerationQueueCoord(coord);
    RemoveFirstGenerationClassQueueCoord(coord, record.residencyClass);
    SparseResidencyClass generatedClass = SparseResidencyClass::Speculative;
    const bool generated = GenerateQueuedBrick(coord, &generatedClass);
    if (generated) {
        IncrementResidencyClassCounter(
            generatedClass,
            m_generatedSpeculativeBricksLastFrame,
            m_generatedVisibleBricksLastFrame,
            m_generatedCollisionBricksLastFrame,
            m_generatedEditedBricksLastFrame);
    }
    RefreshStats();
    return generated ? 1u : 0u;
}

uint32_t SparseVoxelWorld::PumpGenerationForCoordsParallel(
    const std::vector<BrickCoord>& coords,
    uint32_t maxBricks,
    uint32_t maxWorkers,
    float* outWallMs)
{
    if (outWallMs) {
        *outWallMs = 0.0f;
    }
    if (coords.empty() || maxBricks == 0u) {
        return 0u;
    }

    struct PendingExactGeneration {
        BrickCoord coord;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
    };

    std::vector<PendingExactGeneration> pending;
    pending.reserve(std::min<size_t>(coords.size(), static_cast<size_t>(maxBricks)));
    for (const BrickCoord& coord : coords) {
        if (pending.size() >= static_cast<size_t>(maxBricks)) {
            break;
        }

        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record) ||
            record.state != BrickLifecycleState::Requested) {
            continue;
        }

        RemoveFirstGenerationQueueCoord(coord);
        RemoveFirstGenerationClassQueueCoord(coord, record.residencyClass);
        if (!m_pool.MarkGeneratingCPU(coord)) {
            continue;
        }

        pending.push_back({coord, record.residencyClass});
    }

    if (pending.empty()) {
        RefreshStats();
        return 0u;
    }

    const SparseTerrainGenerator terrain = m_terrain;
    std::vector<GeneratedSparseBrick> bricks(pending.size());
    std::vector<BrickCoord> persistentWorkerCoords;
    const uint32_t workerLimit = std::max(1u, maxWorkers);
    const uint32_t workerCount =
        std::min<uint32_t>(static_cast<uint32_t>(pending.size()), workerLimit);
    std::vector<TerrainSurfaceColumnCache> workerColumnCaches(workerCount);
    for (TerrainSurfaceColumnCache& cache : workerColumnCaches) {
        cache.reserve(8192);
    }

    const auto parallelStart = std::chrono::steady_clock::now();
    if (workerCount <= 1u) {
        for (size_t index = 0; index < pending.size(); ++index) {
            bricks[index] = GenerateExactBrickForConfig(
                terrain,
                pending[index].coord,
                workerColumnCaches[0]);
        }
    } else if (m_config.parallelExactGenerationPersistentWorkers) {
        persistentWorkerCoords.reserve(pending.size());
        for (const PendingExactGeneration& item : pending) {
            persistentWorkerCoords.push_back(item.coord);
        }
        if (!GenerateExactBricksWithPersistentWorkers(
                terrain,
                persistentWorkerCoords,
                bricks,
                workerCount)) {
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            const size_t chunkSize =
                (pending.size() + static_cast<size_t>(workerCount) - 1u) /
                static_cast<size_t>(workerCount);
            for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
                const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
                const size_t end = std::min(pending.size(), begin + chunkSize);
                if (begin >= end) {
                    continue;
                }
                workers.emplace_back([
                    this,
                    &terrain,
                    &pending,
                    &bricks,
                    &workerColumnCaches,
                    workerIndex,
                    begin,
                    end]() {
                    for (size_t index = begin; index < end; ++index) {
                        bricks[index] = GenerateExactBrickForConfig(
                            terrain,
                            pending[index].coord,
                            workerColumnCaches[workerIndex]);
                    }
                });
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
        }
    } else {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        const size_t chunkSize =
            (pending.size() + static_cast<size_t>(workerCount) - 1u) /
            static_cast<size_t>(workerCount);
        for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
            const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
            const size_t end = std::min(pending.size(), begin + chunkSize);
            if (begin >= end) {
                continue;
            }
            workers.emplace_back([
                this,
                &terrain,
                &pending,
                &bricks,
                &workerColumnCaches,
                workerIndex,
                begin,
                end]() {
                for (size_t index = begin; index < end; ++index) {
                    bricks[index] = GenerateExactBrickForConfig(
                        terrain,
                        pending[index].coord,
                        workerColumnCaches[workerIndex]);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    const float wallMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - parallelStart).count();
    if (outWallMs) {
        *outWallMs = wallMs;
    }

    uint32_t generated = 0;
    for (size_t index = 0; index < pending.size(); ++index) {
        m_edits.ApplyToGeneratedBrick(bricks[index]);
        SparseResidencyClass generatedClass = pending[index].residencyClass;
        if (ApplyGeneratedBrickPayload(pending[index].coord, bricks[index], &generatedClass)) {
            IncrementResidencyClassCounter(
                generatedClass,
                m_generatedSpeculativeBricksLastFrame,
                m_generatedVisibleBricksLastFrame,
                m_generatedCollisionBricksLastFrame,
                m_generatedEditedBricksLastFrame);
            ++generated;
        }
    }

    RefreshStats();
    return generated;
}

uint32_t SparseVoxelWorld::PumpGenerationAround(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    ApplyAsyncExactGenerationCompletions(currentFrame);
    ApplyAsyncSurfaceExtractionCompletions();
    m_parallelExactGenerationBricksLastFrame = 0;
    m_parallelExactGenerationWorkersLastFrame = 0;
    m_parallelExactGenerationWallMsLastFrame = 0.0f;
    m_persistentExactGenerationWaitMsLastFrame = 0.0f;
    uint32_t generated = 0;
    uint32_t processed = 0;
    m_generationQueuePriorityDirty = false;
    uint32_t ownershipProcessed = 0;
    if (m_config.streamingTicketGenerationOwnershipShareScheduler) {
        generated += PumpGenerationOwnershipShares(maxBricks, currentFrame, &ownershipProcessed);
    } else {
        generated += PumpGenerationOwnershipReservations(maxBricks, currentFrame, &ownershipProcessed);
    }
    processed += ownershipProcessed;

    BrickCoord coord{};
    const std::array<SparseResidencyClass, 4> classOrder{
        SparseResidencyClass::Edited,
        SparseResidencyClass::Collision,
        SparseResidencyClass::Visible,
        SparseResidencyClass::Speculative
    };
    const bool parallelGenerationAllowed =
        m_config.parallelExactGeneration &&
        maxBricks >= m_config.parallelExactGenerationMinBricks &&
        (m_config.parallelExactGenerationEditAware || m_edits.EditedBrickCount() == 0u);
    if (processed < maxBricks && parallelGenerationAllowed) {
        struct PendingExactGeneration {
            BrickCoord coord;
            SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        };

        std::vector<PendingExactGeneration> pending;
        pending.reserve(maxBricks);
        for (SparseResidencyClass residencyClass : classOrder) {
            auto& classQueue = m_generationClassQueues[ResidencyClassQueueIndex(residencyClass)];
            if (m_config.generationClassPartialValueSort) {
                const size_t remainingBudget =
                    std::max<size_t>(1u, static_cast<size_t>(maxBricks - processed));
                PartialSortQueuedBricksByValue(
                    classQueue,
                    m_pool,
                    focus,
                    currentFrame,
                    remainingBudget,
                    m_config.streamingLaneQueuePriority);
                SortQueueByStreamingTickets(
                    classQueue,
                    kStreamingTicketStageCpuGenerated,
                    &focus,
                    currentFrame,
                    true,
                    remainingBudget);
            } else {
                SortQueuedBricksByValue(
                    classQueue,
                    m_pool,
                    focus,
                    currentFrame,
                    m_config.streamingLaneQueuePriority);
                SortQueueByStreamingTickets(
                    classQueue,
                    kStreamingTicketStageCpuGenerated,
                    &focus,
                    currentFrame,
                    true);
            }
            while (processed < maxBricks &&
                   pending.size() < static_cast<size_t>(maxBricks) &&
                   PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
                BrickResidentRecord record;
                if (!m_pool.GetRecord(coord, &record)) {
                    continue;
                }
                if (record.residencyClass != residencyClass) {
                    continue;
                }
                RemoveAllClassQueueCoord(m_generationClassQueues, coord);
                RemoveFirstGenerationQueueCoord(coord);
                if (TryQueueAsyncExactGeneration(coord, record, currentFrame)) {
                    ++processed;
                    continue;
                }
                if (!m_pool.MarkGeneratingCPU(coord)) {
                    ++processed;
                    continue;
                }
                pending.push_back({coord, record.residencyClass});
                ++processed;
            }
            if (processed >= maxBricks) {
                break;
            }
        }

        if (!pending.empty()) {
            const SparseTerrainGenerator terrain = m_terrain;
            std::vector<GeneratedSparseBrick> bricks(pending.size());
            std::vector<BrickCoord> persistentWorkerCoords;
            const bool useWorkerThreads =
                pending.size() >= static_cast<size_t>(m_config.parallelExactGenerationMinBricks) &&
                m_config.parallelExactGenerationMaxWorkers > 1u;
            const uint32_t workerCount = useWorkerThreads
                ? std::min<uint32_t>(
                    static_cast<uint32_t>(pending.size()),
                    m_config.parallelExactGenerationMaxWorkers)
                : 1u;
            std::vector<TerrainSurfaceColumnCache> workerColumnCaches(workerCount);
            for (TerrainSurfaceColumnCache& cache : workerColumnCaches) {
                cache.reserve(8192);
            }
            const auto parallelStart = std::chrono::steady_clock::now();
            if (workerCount <= 1u) {
                for (size_t index = 0; index < pending.size(); ++index) {
                    bricks[index] = GenerateExactBrickForConfig(
                        terrain,
                        pending[index].coord,
                        workerColumnCaches[0]);
                }
            } else if (m_config.parallelExactGenerationPersistentWorkers) {
                persistentWorkerCoords.reserve(pending.size());
                for (const PendingExactGeneration& item : pending) {
                    persistentWorkerCoords.push_back(item.coord);
                }
                if (!GenerateExactBricksWithPersistentWorkers(
                        terrain,
                        persistentWorkerCoords,
                        bricks,
                        workerCount)) {
                    std::vector<std::thread> workers;
                    workers.reserve(workerCount);
                    const size_t chunkSize =
                        (pending.size() + static_cast<size_t>(workerCount) - 1u) /
                        static_cast<size_t>(workerCount);
                    for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
                        const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
                        const size_t end = std::min(pending.size(), begin + chunkSize);
                        workers.emplace_back([
                            this,
                            &terrain,
                            &pending,
                            &bricks,
                            &workerColumnCaches,
                            workerIndex,
                            begin,
                            end]() {
                            for (size_t index = begin; index < end; ++index) {
                                bricks[index] = GenerateExactBrickForConfig(
                                    terrain,
                                    pending[index].coord,
                                    workerColumnCaches[workerIndex]);
                            }
                        });
                    }
                    const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
                    for (std::thread& worker : workers) {
                        worker.join();
                    }
                    m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
                }
            } else {
                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                const size_t chunkSize =
                    (pending.size() + static_cast<size_t>(workerCount) - 1u) /
                    static_cast<size_t>(workerCount);
                for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex) {
                    const size_t begin = static_cast<size_t>(workerIndex) * chunkSize;
                    const size_t end = std::min(pending.size(), begin + chunkSize);
                    workers.emplace_back([
                        this,
                        &terrain,
                        &pending,
                        &bricks,
                        &workerColumnCaches,
                        workerIndex,
                        begin,
                        end]() {
                        for (size_t index = begin; index < end; ++index) {
                            bricks[index] = GenerateExactBrickForConfig(
                                terrain,
                                pending[index].coord,
                                workerColumnCaches[workerIndex]);
                        }
                    });
                }
                const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
                for (std::thread& worker : workers) {
                    worker.join();
                }
                m_persistentExactGenerationWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
            }
            m_parallelExactGenerationBricksLastFrame =
                static_cast<uint32_t>(
                    std::min<size_t>(pending.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            m_parallelExactGenerationWorkersLastFrame = workerCount;
            m_parallelExactGenerationWallMsLastFrame =
                std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - parallelStart).count();

            for (size_t index = 0; index < pending.size(); ++index) {
                // Workers generate PRISTINE bricks (no m_edits access); apply the edit
                // overlay serially here on the main thread before storing the payload.
                // No-op when the brick has no edits, so this is correct whether or not
                // parallelExactGenerationEditAware is on.
                m_edits.ApplyToGeneratedBrick(bricks[index]);
                SparseResidencyClass generatedClass = pending[index].residencyClass;
                if (ApplyGeneratedBrickPayload(pending[index].coord, bricks[index], &generatedClass)) {
                    IncrementResidencyClassCounter(
                        generatedClass,
                        m_generatedSpeculativeBricksLastFrame,
                        m_generatedVisibleBricksLastFrame,
                        m_generatedCollisionBricksLastFrame,
                        m_generatedEditedBricksLastFrame);
                    ++generated;
                }
            }
        }
    }

    if (processed < maxBricks) {
        for (SparseResidencyClass residencyClass : classOrder) {
            auto& classQueue = m_generationClassQueues[ResidencyClassQueueIndex(residencyClass)];
            if (m_config.generationClassPartialValueSort) {
                const size_t remainingBudget =
                    std::max<size_t>(1u, static_cast<size_t>(maxBricks - processed));
                PartialSortQueuedBricksByValue(
                    classQueue,
                    m_pool,
                    focus,
                    currentFrame,
                    remainingBudget,
                    m_config.streamingLaneQueuePriority);
                SortQueueByStreamingTickets(
                    classQueue,
                    kStreamingTicketStageCpuGenerated,
                    &focus,
                    currentFrame,
                    true,
                    remainingBudget);
            } else {
                SortQueuedBricksByValue(
                    classQueue,
                    m_pool,
                    focus,
                    currentFrame,
                    m_config.streamingLaneQueuePriority);
                SortQueueByStreamingTickets(
                    classQueue,
                    kStreamingTicketStageCpuGenerated,
                    &focus,
                    currentFrame,
                    true);
            }
            while (processed < maxBricks &&
                   PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
                BrickResidentRecord record;
                if (!m_pool.GetRecord(coord, &record)) {
                    continue;
                }
                if (record.residencyClass != residencyClass) {
                    continue;
                }
                RemoveAllClassQueueCoord(m_generationClassQueues, coord);
                if (TryQueueAsyncExactGeneration(coord, record, currentFrame)) {
                    RemoveFirstGenerationQueueCoord(coord);
                    ++processed;
                    continue;
                }
                SparseResidencyClass generatedClass = SparseResidencyClass::Speculative;
                if (GenerateQueuedBrick(coord, &generatedClass)) {
                    RemoveFirstGenerationQueueCoord(coord);
                    IncrementResidencyClassCounter(
                        generatedClass,
                        m_generatedSpeculativeBricksLastFrame,
                        m_generatedVisibleBricksLastFrame,
                        m_generatedCollisionBricksLastFrame,
                        m_generatedEditedBricksLastFrame);
                    ++generated;
                    ++processed;
                } else {
                    RemoveFirstGenerationQueueCoord(coord);
                    ++processed;
                }
            }
            if (processed >= maxBricks) {
                break;
            }
        }
    }

    if (processed < maxBricks) {
        if (m_config.generationClassPartialValueSort) {
            const size_t remainingBudget =
                std::max<size_t>(1u, static_cast<size_t>(maxBricks - processed));
            PartialSortQueuedBricksByValue(
                m_generationQueue,
                m_pool,
                focus,
                currentFrame,
                remainingBudget,
                m_config.streamingLaneQueuePriority);
            SortQueueByStreamingTickets(
                m_generationQueue,
                kStreamingTicketStageCpuGenerated,
                &focus,
                currentFrame,
                true,
                remainingBudget);
        } else {
            SortQueuedBricksByValue(
                m_generationQueue,
                m_pool,
                focus,
                currentFrame,
                m_config.streamingLaneQueuePriority);
            SortQueueByStreamingTickets(
                m_generationQueue,
                kStreamingTicketStageCpuGenerated,
                &focus,
                currentFrame,
                true);
        }
        while (processed < maxBricks &&
               PopFrontQueuedBrick(m_generationQueue, m_pool, &coord)) {
            BrickResidentRecord record;
            if (m_pool.GetRecord(coord, &record)) {
                RemoveFirstGenerationClassQueueCoord(coord, record.residencyClass);
            }
            if (TryQueueAsyncExactGeneration(coord, record, currentFrame)) {
                ++processed;
                continue;
            }
            SparseResidencyClass generatedClass = SparseResidencyClass::Speculative;
            if (GenerateQueuedBrick(coord, &generatedClass)) {
                IncrementResidencyClassCounter(
                    generatedClass,
                    m_generatedSpeculativeBricksLastFrame,
                    m_generatedVisibleBricksLastFrame,
                    m_generatedCollisionBricksLastFrame,
                    m_generatedEditedBricksLastFrame);
                ++generated;
                ++processed;
            } else {
                ++processed;
            }
        }
    }

    RefreshStats();
    return generated;
}

bool SparseVoxelWorld::PopNextUpload(SparseBrickUploadPacket* outPacket, uint32_t currentFrame) {
    if (!outPacket) {
        return false;
    }
    if (m_config.streamingTicketLowPriorityDownstreamDeferral &&
        m_deferredGeneratedDownstreamPromotedFrame != currentFrame) {
        PromoteDeferredGeneratedDownstream(
            m_config.streamingTicketLowPriorityDownstreamPromoteMax,
            currentFrame);
        m_deferredGeneratedDownstreamPromotedFrame = currentFrame;
    }

    while (!m_uploadQueue.empty()) {
        if (m_uploadQueuePriorityDirty) {
            SortQueuedBricksByPriority(
                m_uploadQueue,
                m_pool,
                currentFrame,
                m_config.streamingLaneQueuePriority);
            SortQueueByStreamingTickets(
                m_uploadQueue,
                kStreamingTicketStageGpuUploaded,
                nullptr,
                currentFrame,
                false);
            m_uploadQueuePriorityDirty = false;
        }
        BrickCoord coord{};
        if (!PopFrontQueuedBrick(m_uploadQueue, m_pool, &coord)) {
            break;
        }

        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
        RemoveFirstUploadClassQueueCoord(coord, record.residencyClass);

        SparseResidencyClass uploadedClass = record.residencyClass;
        if (!BuildUploadPacketForCoord(coord, m_pool, m_generated, outPacket, &uploadedClass)) {
            continue;
        }
        AnnotateRenderDirtyUploadRange(outPacket);

        IncrementResidencyClassCounter(
            uploadedClass,
            m_uploadedSpeculativeBricksLastFrame,
            m_uploadedVisibleBricksLastFrame,
            m_uploadedCollisionBricksLastFrame,
            m_uploadedEditedBricksLastFrame);
        RefreshStats();
        return true;
    }

    RefreshStats();
    return false;
}

bool SparseVoxelWorld::PopNextUploadForClass(
    SparseBrickUploadPacket* outPacket,
    SparseResidencyClass residencyClass,
    uint32_t currentFrame)
{
    if (!outPacket) {
        return false;
    }
    if (m_config.streamingTicketLowPriorityDownstreamDeferral &&
        m_deferredGeneratedDownstreamPromotedFrame != currentFrame) {
        PromoteDeferredGeneratedDownstream(
            m_config.streamingTicketLowPriorityDownstreamPromoteMax,
            currentFrame);
        m_deferredGeneratedDownstreamPromotedFrame = currentFrame;
    }

    auto& classQueue = m_uploadClassQueues[ResidencyClassQueueIndex(residencyClass)];
    if (m_uploadQueuePriorityDirty) {
        SortQueuedBricksByPriority(
            classQueue,
            m_pool,
            currentFrame,
            m_config.streamingLaneQueuePriority);
        SortQueueByStreamingTickets(
            classQueue,
            kStreamingTicketStageGpuUploaded,
            nullptr,
            currentFrame,
            false);
    }

    BrickCoord coord{};
    while (PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        if (record.residencyClass != residencyClass) {
            continue;
        }
        RemoveAllClassQueueCoord(m_uploadClassQueues, coord);

        SparseResidencyClass uploadedClass = record.residencyClass;
        if (!BuildUploadPacketForCoord(coord, m_pool, m_generated, outPacket, &uploadedClass)) {
            RemoveFirstUploadQueueCoord(coord);
            continue;
        }
        AnnotateRenderDirtyUploadRange(outPacket);
        RemoveFirstUploadQueueCoord(coord);

        IncrementResidencyClassCounter(
            uploadedClass,
            m_uploadedSpeculativeBricksLastFrame,
            m_uploadedVisibleBricksLastFrame,
            m_uploadedCollisionBricksLastFrame,
            m_uploadedEditedBricksLastFrame);
        RefreshStats();
        return true;
    }

    RefreshStats();
    return false;
}

bool SparseVoxelWorld::PopBestUploadForClass(
    SparseBrickUploadPacket* outPacket,
    SparseResidencyClass residencyClass,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    if (!outPacket) {
        return false;
    }
    if (m_config.streamingTicketLowPriorityDownstreamDeferral &&
        m_deferredGeneratedDownstreamPromotedFrame != currentFrame) {
        PromoteDeferredGeneratedDownstream(
            m_config.streamingTicketLowPriorityDownstreamPromoteMax,
            currentFrame);
        m_deferredGeneratedDownstreamPromotedFrame = currentFrame;
    }

    auto& classQueue = m_uploadClassQueues[ResidencyClassQueueIndex(residencyClass)];
    const size_t classIndex = ResidencyClassQueueIndex(residencyClass);
    const bool valueSortValid =
        m_uploadClassValueSortValid[classIndex] &&
        m_uploadClassValueSortFrame[classIndex] == currentFrame &&
        m_uploadClassValueSortFocus[classIndex] == focus;
    if (!valueSortValid) {
        SortQueuedBricksByValue(
            classQueue,
            m_pool,
            focus,
            currentFrame,
            m_config.streamingLaneQueuePriority);
        m_uploadClassValueSortValid[classIndex] = true;
        m_uploadClassValueSortFrame[classIndex] = currentFrame;
        m_uploadClassValueSortFocus[classIndex] = focus;
    }
    SortQueueByStreamingTickets(
        classQueue,
        kStreamingTicketStageGpuUploaded,
        &focus,
        currentFrame,
        true);
    BrickCoord coord{};
    while (PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        if (record.residencyClass != residencyClass) {
            continue;
        }
        RemoveAllClassQueueCoord(m_uploadClassQueues, coord);

        SparseResidencyClass uploadedClass = record.residencyClass;
        if (!BuildUploadPacketForCoord(coord, m_pool, m_generated, outPacket, &uploadedClass)) {
            RemoveFirstUploadQueueCoord(coord);
            continue;
        }
        AnnotateRenderDirtyUploadRange(outPacket);
        RemoveFirstUploadQueueCoord(coord);

        IncrementResidencyClassCounter(
            uploadedClass,
            m_uploadedSpeculativeBricksLastFrame,
            m_uploadedVisibleBricksLastFrame,
            m_uploadedCollisionBricksLastFrame,
            m_uploadedEditedBricksLastFrame);
        RefreshStats();
        return true;
    }

    RefreshStats();
    return false;
}

bool SparseVoxelWorld::PopBestUploadForOwnershipCritical(
    SparseBrickUploadPacket* outPacket,
    bool ownershipCritical,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    if (!outPacket) {
        return false;
    }
    if (m_config.streamingTicketLowPriorityDownstreamDeferral) {
        PromoteDeferredGeneratedDownstreamForOwnership(
            ownershipCritical,
            m_config.streamingTicketLowPriorityDownstreamPromoteMax,
            currentFrame);
    }

    auto& ownershipQueue = m_uploadOwnershipQueues[OwnershipCriticalQueueIndex(ownershipCritical)];
    SortQueuedBricksByValue(
        ownershipQueue,
        m_pool,
        focus,
        currentFrame,
        m_config.streamingLaneQueuePriority);

    BrickCoord bestCoord{};
    while (PopFrontQueuedBrick(ownershipQueue, m_pool, &bestCoord)) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(bestCoord, &record)) {
            RemoveFirstUploadQueueCoord(bestCoord);
            RemoveAllClassQueueCoord(m_uploadClassQueues, bestCoord);
            continue;
        }
        if (IsStreamingOwnershipCritical(record) != ownershipCritical) {
            continue;
        }
        RemoveFirstUploadQueueCoord(bestCoord);
        RemoveAllClassQueueCoord(m_uploadClassQueues, bestCoord);

        SparseResidencyClass uploadedClass = record.residencyClass;
        if (!BuildUploadPacketForCoord(bestCoord, m_pool, m_generated, outPacket, &uploadedClass)) {
            continue;
        }
        AnnotateRenderDirtyUploadRange(outPacket);

        IncrementResidencyClassCounter(
            uploadedClass,
            m_uploadedSpeculativeBricksLastFrame,
            m_uploadedVisibleBricksLastFrame,
            m_uploadedCollisionBricksLastFrame,
            m_uploadedEditedBricksLastFrame);
        RefreshStats();
        return true;
    }

    RefreshStats();
    return false;
}

bool SparseVoxelWorld::PopUploadForCoord(SparseBrickUploadPacket* outPacket, const BrickCoord& coord) {
    if (!outPacket) {
        return false;
    }

    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        RefreshStats();
        return false;
    }
    if (record.state == BrickLifecycleState::GeneratedCPU &&
        PromoteDeferredGeneratedDownstreamForCoord(coord, 0u)) {
        (void)m_pool.GetRecord(coord, &record);
    }
    if (record.state != BrickLifecycleState::UploadQueued) {
        RefreshStats();
        return false;
    }

    SparseResidencyClass uploadedClass = record.residencyClass;
    if (!BuildUploadPacketForCoord(coord, m_pool, m_generated, outPacket, &uploadedClass)) {
        RemoveFirstUploadQueueCoord(coord);
        RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
        RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);
        RefreshStats();
        return false;
    }
    AnnotateRenderDirtyUploadRange(outPacket);
    RemoveFirstUploadQueueCoord(coord);
    RemoveAllClassQueueCoord(m_uploadClassQueues, coord);
    RemoveAllClassQueueCoord(m_uploadOwnershipQueues, coord);

    IncrementResidencyClassCounter(
        uploadedClass,
        m_uploadedSpeculativeBricksLastFrame,
        m_uploadedVisibleBricksLastFrame,
        m_uploadedCollisionBricksLastFrame,
        m_uploadedEditedBricksLastFrame);
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::RequeueUploadFront(const SparseBrickUploadPacket& packet) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(packet.coord, &record)) {
        RefreshStats();
        return false;
    }
    if (record.pageIndex != packet.pageIndex ||
        record.generation != packet.generation ||
        record.state != BrickLifecycleState::UploadingGPU) {
        RefreshStats();
        return false;
    }
    if (m_generated.find(packet.coord) == m_generated.end()) {
        m_generated[packet.coord] = packet.brick;
    }
    if (!m_pool.AbortUpload(packet.coord)) {
        RefreshStats();
        return false;
    }
    QueueUploadCoordFront(packet.coord);
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::CompleteUpload(const SparseBrickUploadPacket& packet) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(packet.coord, &record)) {
        return false;
    }
    if (record.pageIndex != packet.pageIndex || record.generation != packet.generation) {
        return false;
    }

    const bool published = m_pool.PublishResident(
        packet.coord,
        packet.brick.flags,
        packet.brick.occupancyWord0,
        packet.brick.occupancyWord1);
    if (published) {
        const bool emptyBrick = HasResidencyFlag(packet.brick.flags, BrickResidencyFlags::Empty);
        const bool existingSurface = m_surfaceCache.FindFaces(packet.coord) != nullptr;
        const bool knownSurface = m_surfaceCache.IsSurfaceKnown(packet.coord);
        const bool buriedSolidFastPath =
            CanUseBuriedSolidSurfaceFastPath(packet.coord, packet.brick);
        if (buriedSolidFastPath) {
            MarkBuriedSolidSurfaceKnownEmpty(packet.coord);
        }
        const bool needsUploadSurfaceRefresh =
            emptyBrick ||
            (!knownSurface && !buriedSolidFastPath) ||
            m_surfaceDirtyRegions.find(packet.coord) != m_surfaceDirtyRegions.end();
        if ((!emptyBrick || existingSurface) && needsUploadSurfaceRefresh) {
            BrickResidentRecord refreshedRecord;
            if (m_pool.GetRecord(packet.coord, &refreshedRecord)) {
                TouchStreamingTicket(
                    packet.coord,
                    refreshedRecord,
                    kStreamingTicketStageSurfaceReady);
            }
            m_pendingSurfaceBricks[packet.coord] = packet.brick;
            QueueSurfaceExtractionCoord(packet.coord);
        } else if (!emptyBrick && existingSurface) {
            m_pendingSurfaceBricks.erase(packet.coord);
            MarkStreamingTicketStagesCompleted(packet.coord, kStreamingTicketStageSurfaceReady);
            MarkQueueAccountingDirty();
        } else {
            m_pendingSurfaceBricks.erase(packet.coord);
            m_surfaceDirtyRegions.erase(packet.coord);
            MarkStreamingTicketStagesCompleted(packet.coord, kStreamingTicketStageSurfaceReady);
            MarkQueueAccountingDirty();
            ++m_surfaceEmptyUploadsSkippedLastFrame;
        }
        MarkStreamingTicketStagesCompleted(packet.coord, kStreamingTicketStageGpuUploaded);
        m_generated.erase(packet.coord);
        m_deferredGeneratedDownstreamSet.erase(packet.coord);
        auto deferredIt = m_deferredDirtyAfterUpload.find(packet.coord);
        if (deferredIt != m_deferredDirtyAfterUpload.end()) {
            m_deferredDirtyAfterUpload.erase(deferredIt);
            QueueRegeneratedUploadForExistingPage(packet.coord);
        } else {
            m_renderDirtyRegions.erase(packet.coord);
        }
    }

    RefreshStats();
    return published;
}

bool SparseVoxelWorld::MarkGpuPageTablePublished(
    const BrickCoord& coord,
    uint32_t pageIndex,
    uint32_t generation)
{
    const bool marked = m_pool.MarkGpuPageTablePublished(coord, pageIndex, generation);
    if (marked) {
        MarkStreamingTicketStagesCompleted(coord, kStreamingTicketStagePagePublished);
        RefreshStats();
    }
    return marked;
}

bool SparseVoxelWorld::CanUseBuriedSolidSurfaceFastPath(
    const BrickCoord& coord,
    const GeneratedSparseBrick& brick) const
{
    if (!m_config.surfaceBuriedSolidFastPath ||
        !HasResidencyFlag(brick.flags, BrickResidencyFlags::Solid) ||
        HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty) ||
        HasResidencyFlag(brick.flags, BrickResidencyFlags::HasWater)) {
        return false;
    }

    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                BrickCoord neighborCoord;
                if (TryOffsetBrickCoord(coord, dx, dy, dz, &neighborCoord) &&
                    m_edits.HasOverlay(neighborCoord)) {
                    return false;
                }
            }
        }
    }

    return m_terrain.IsDefinitelyBuriedSolidBrick(coord);
}

bool SparseVoxelWorld::MarkBuriedSolidSurfaceKnownEmpty(const BrickCoord& coord) {
    m_pendingSurfaceBricks.erase(coord);
    m_surfaceDirtyRegions.erase(coord);
    m_surfaceExtractionQueuedSet.erase(coord);
    RemoveFirstSurfaceQueueCoord(coord);
    RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
    const bool marked = m_surfaceCache.MarkKnownEmptySurface(coord);
    if (marked) {
        ++m_surfaceBuriedSolidFastPathBricksLastFrame;
        MarkQueueAccountingDirty();
    }
    MarkStreamingTicketStagesCompleted(coord, kStreamingTicketStageSurfaceReady);
    return marked;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtraction(uint32_t maxBricks, uint32_t currentFrame) {
    uint32_t extracted = 0;
    if (maxBricks == 0 || m_pendingSurfaceBricks.empty()) {
        PruneSurfaceExtractionQueuesIfNoPending();
        m_surfaceBricksExtractedLastFrame = 0;
        RefreshStats();
        return 0;
    }
    if (m_surfaceExtractionQueuePriorityDirty) {
        SortQueuedBricksByPriority(
            m_surfaceExtractionQueue,
            m_pool,
            currentFrame,
            m_config.streamingLaneQueuePriority);
        SortQueueByStreamingTickets(
            m_surfaceExtractionQueue,
            kStreamingTicketStageSurfaceReady,
            nullptr,
            currentFrame,
            false);
        m_surfaceExtractionQueuePriorityDirty = false;
    }
    BrickCoord coord{};
    while (extracted < maxBricks &&
           PopFrontQueuedBrick(m_surfaceExtractionQueue, m_pool, &coord)) {
        BrickResidentRecord surfaceRecord;
        if (m_pool.GetRecord(coord, &surfaceRecord)) {
            RemoveFirstSurfaceClassQueueCoord(coord, surfaceRecord.residencyClass);
        }
        if (ExtractOrQueueSurfaceCoord(coord)) {
            ++extracted;
        }
    }

    m_surfaceBricksExtractedLastFrame = extracted;
    RefreshStats();
    return extracted;
}

static bool IsSurfaceExtractableState(BrickLifecycleState state) {
    return state == BrickLifecycleState::UploadQueued ||
           state == BrickLifecycleState::UploadingGPU ||
           state == BrickLifecycleState::Resident ||
           state == BrickLifecycleState::DirtyCPU ||
           state == BrickLifecycleState::DirtyGPU;
}

bool SparseVoxelWorld::TryQueueAsyncSurfaceExtraction(const BrickCoord& coord) {
    const auto enqueueStart = std::chrono::steady_clock::now();
    const auto rejectAsync = [this]() {
        ++m_asyncSurfaceExtractionRejectedLastFrame;
        return false;
    };
    // Only the no-edit meshing path is safe to run off-thread (the worker uses the
    // terrain generator directly, with no edit overlay sampling). With the per-coord
    // edit gate enabled, coords whose 3x3x3 dependency neighborhood has no edit
    // overlay may still use the worker while unrelated edits exist elsewhere.
    auto pendingIt = m_pendingSurfaceBricks.find(coord);
    if (pendingIt == m_pendingSurfaceBricks.end()) {
        return rejectAsync();
    }
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record) || !IsSurfaceExtractableState(record.state)) {
        return rejectAsync();  // let the caller's inline path clean up the stale pending brick
    }
    if (m_surfaceDirtyRegions.find(coord) != m_surfaceDirtyRegions.end()) {
        return rejectAsync();
    }
    if (m_config.asyncSurfaceExtractionPerCoordEditGate) {
        if (EditOverlapsExactGenDependency(coord)) {
            return rejectAsync();
        }
    } else if (m_edits.EditedBrickCount() != 0u) {
        return rejectAsync();
    }
    const uint64_t editDependencyRevision =
        m_config.asyncSurfaceExtractionPerCoordEditGate
            ? MaxEditRevisionInExactGenDependency(coord)
            : 0u;
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        if (m_asyncSurfaceExtractionPending.find(coord) !=
            m_asyncSurfaceExtractionPending.end()) {
            // A duplicate pending brick can be a newer generation or an edit/dirty
            // refresh for this coord. Keep it on the synchronous path instead of
            // deleting the only fresh source while an older async mesh is in flight.
            return rejectAsync();
        }
        if (m_asyncSurfaceExtractionPending.size() >=
            static_cast<size_t>(m_config.asyncSurfaceExtractionQueueMax)) {
            return rejectAsync();  // backpressure: fall back to inline extraction this frame
        }
    }

    StartAsyncSurfaceExtractionWorkerIfNeeded();

    AsyncSurfaceExtractionRequest request;
    request.coord = coord;
    request.residencyClass = record.residencyClass;
    request.pageIndex = record.pageIndex;
    request.generation = record.generation;
    request.editDependencyRevision = editDependencyRevision;
    request.brick = std::move(pendingIt->second);
    m_pendingSurfaceBricks.erase(pendingIt);
    m_surfaceExtractionQueuedSet.erase(coord);
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        m_asyncSurfaceExtractionPending.insert(coord);
        m_asyncSurfaceExtractionQueue.push_back(std::move(request));
    }
    m_asyncSurfaceExtractionCv.notify_one();
    ++m_asyncSurfaceExtractionEnqueuedLastFrame;
    m_asyncSurfaceExtractionEnqueueMsLastFrame +=
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - enqueueStart).count();
    MarkQueueAccountingDirty();
    return true;
}

uint32_t SparseVoxelWorld::ApplyAsyncSurfaceExtractionCompletions() {
    if (!m_config.asyncSurfaceExtraction) {
        return 0;
    }
    const uint32_t maxApply = std::max(1u, m_config.asyncSurfaceExtractionMaxApplyPerFrame);
    uint32_t applied = 0;
    while (applied < maxApply) {
        AsyncSurfaceExtractionResult result;
        {
            const uint64_t waitStartTicks = SDL_GetPerformanceCounter();
            std::unique_lock<std::mutex> lock(m_asyncSurfaceExtractionMutex);
            m_surfaceExtractionWaitMsLastFrame += WaitTicksToMs(waitStartTicks);
            m_stats.surfaceExtractionWaitMsLastFrame = m_surfaceExtractionWaitMsLastFrame;
            if (m_asyncSurfaceExtractionResults.empty()) {
                break;
            }
            result = std::move(m_asyncSurfaceExtractionResults.front());
            m_asyncSurfaceExtractionResults.pop_front();
            m_asyncSurfaceExtractionPending.erase(result.coord);
        }
        m_asyncSurfaceExtractionWorkerMsLastFrame += result.workerMs;

        BrickResidentRecord record;
        if (!m_pool.GetRecord(result.coord, &record) ||
            !IsSurfaceExtractableState(record.state) ||
            record.pageIndex != result.pageIndex ||
            record.generation != result.generation) {
            ++m_asyncSurfaceExtractionDiscardedLastFrame;
            continue;  // brick evicted/changed while meshing; drop the stale mesh
        }
        const bool dirtyWhileMeshing =
            m_surfaceDirtyRegions.find(result.coord) != m_surfaceDirtyRegions.end();
        const bool editDependencyChanged =
            m_config.asyncSurfaceExtractionPerCoordEditGate &&
            MaxEditRevisionInExactGenDependency(result.coord) != result.editDependencyRevision;
        if (dirtyWhileMeshing || editDependencyChanged) {
            if (m_pendingSurfaceBricks.find(result.coord) == m_pendingSurfaceBricks.end()) {
                m_pendingSurfaceBricks[result.coord] = std::move(result.brick);
                QueueSurfaceExtractionCoord(result.coord);
                TouchStreamingTicket(
                    result.coord,
                    record,
                    kStreamingTicketStageSurfaceReady);
                ++m_asyncSurfaceExtractionRequeuedLastFrame;
            }
            ++m_asyncSurfaceExtractionDiscardedLastFrame;
            continue;  // surface dependency changed while the no-edit worker was meshing
        }
        if (m_surfaceCache.UpdateBrickWithExtractedFaces(
                result.brick,
                std::move(result.faces))) {
            IncrementResidencyClassCounter(
                result.residencyClass,
                m_surfaceSpeculativeBricksExtractedLastFrame,
                m_surfaceVisibleBricksExtractedLastFrame,
                m_surfaceCollisionBricksExtractedLastFrame,
                m_surfaceEditedBricksExtractedLastFrame);
            MarkStreamingTicketStagesCompleted(result.coord, kStreamingTicketStageSurfaceReady);
            ++m_surfaceBricksExtractedLastFrame;
            ++m_asyncSurfaceExtractionAppliedLastFrame;
            ++applied;
        }
    }
    if (applied > 0) {
        RefreshStats();
    }
    return applied;
}

// Route a coord to the async surface mesher when eligible (non-edited terrain), else
// extract inline. Used by ALL the per-frame surface pump loops so meshing leaves the
// main thread regardless of which pump drives it.
bool SparseVoxelWorld::ExtractOrQueueSurfaceCoord(const BrickCoord& coord) {
    if (m_config.asyncSurfaceExtraction &&
        TryQueueAsyncSurfaceExtraction(coord)) {
        return true;
    }
    return ExtractSurfaceCoord(coord);
}

bool SparseVoxelWorld::PumpSurfaceExtractionForCoord(const BrickCoord& coord) {
    if (m_pendingSurfaceBricks.find(coord) == m_pendingSurfaceBricks.end()) {
        return false;
    }

    BrickResidentRecord surfaceRecord;
    if (m_pool.GetRecord(coord, &surfaceRecord)) {
        RemoveFirstSurfaceClassQueueCoord(coord, surfaceRecord.residencyClass);
    }
    RemoveFirstSurfaceQueueCoord(coord);

    // Fire-and-forget async meshing for non-edited terrain: enqueue and apply later
    // off the critical path. Falls back to inline extraction if not eligible/queue full.
    if (m_config.asyncSurfaceExtraction &&
        TryQueueAsyncSurfaceExtraction(coord)) {
        RefreshStats();
        return true;
    }

    if (!ExtractSurfaceCoord(coord)) {
        RefreshStats();
        return false;
    }

    ++m_surfaceBricksExtractedLastFrame;
    RefreshStats();
    return true;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionForCoords(
    const std::vector<BrickCoord>& coords,
    uint32_t maxBricks)
{
    if (maxBricks == 0 || coords.empty() || m_pendingSurfaceBricks.empty()) {
        PruneSurfaceExtractionQueuesIfNoPending();
        RefreshStats();
        return 0;
    }

    // Async surface extraction supersedes the fork-join batch: route every coord
    // through PumpSurfaceExtractionForCoord, which enqueues to the worker pool.
    if (m_config.asyncSurfaceExtraction || !CanUseParallelSurfaceExtractionBatch(maxBricks)) {
        uint32_t extracted = 0;
        for (const BrickCoord& coord : coords) {
            if (extracted >= maxBricks) {
                break;
            }
            if (PumpSurfaceExtractionForCoord(coord)) {
                ++extracted;
            }
        }
        return extracted;
    }

    std::vector<SurfaceExtractionBatchItem> pending;
    const uint32_t parallelMaxBricks =
        m_config.parallelSurfaceExtractionTimeBudgeted
            ? std::min(maxBricks, m_config.parallelSurfaceExtractionMaxBatch)
            : maxBricks;
    pending.reserve(std::min<size_t>(coords.size(), static_cast<size_t>(parallelMaxBricks)));
    for (const BrickCoord& coord : coords) {
        if (pending.size() >= static_cast<size_t>(parallelMaxBricks)) {
            break;
        }
        if (m_surfaceCache.IsSurfaceKnown(coord)) {
            continue;
        }
        auto pendingIt = m_pendingSurfaceBricks.find(coord);
        if (pendingIt == m_pendingSurfaceBricks.end()) {
            continue;
        }
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            RemoveFirstSurfaceQueueCoord(coord);
            RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
            m_pendingSurfaceBricks.erase(pendingIt);
            m_surfaceExtractionQueuedSet.erase(coord);
            continue;
        }
        if (record.state != BrickLifecycleState::UploadQueued &&
            record.state != BrickLifecycleState::UploadingGPU &&
            record.state != BrickLifecycleState::Resident &&
            record.state != BrickLifecycleState::DirtyCPU &&
            record.state != BrickLifecycleState::DirtyGPU) {
            RemoveFirstSurfaceQueueCoord(coord);
            RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
            m_pendingSurfaceBricks.erase(pendingIt);
            m_surfaceDirtyRegions.erase(coord);
            m_surfaceExtractionQueuedSet.erase(coord);
            continue;
        }

        RemoveFirstSurfaceQueueCoord(coord);
        RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
        SurfaceExtractionBatchItem work;
        work.coord = coord;
        work.residencyClass = record.residencyClass;
        work.brick = std::move(pendingIt->second);
        pending.push_back(std::move(work));
        m_pendingSurfaceBricks.erase(pendingIt);
        m_surfaceExtractionQueuedSet.erase(coord);
    }

    if (pending.empty()) {
        RefreshStats();
        return 0;
    }

    MarkQueueAccountingDirty();
    const uint32_t extracted = ExtractSurfaceBatchNoEdit(pending);
    m_surfaceBricksExtractedLastFrame += extracted;
    RefreshStats();
    return extracted;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionAround(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    return PumpSurfaceExtractionAroundTimed(maxBricks, focus, currentFrame, 0.0);
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionAroundTimed(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame,
    double maxMilliseconds,
    SparseSurfaceExtractionClassCounts* outClassCounts)
{
    if (outClassCounts) {
        *outClassCounts = {};
    }
    uint32_t extracted = 0;
    if (maxBricks == 0 || m_pendingSurfaceBricks.empty()) {
        PruneSurfaceExtractionQueuesIfNoPending();
        m_surfaceBricksExtractedLastFrame = 0;
        RefreshStats();
        return 0;
    }
    const bool hasTimeLimit = maxMilliseconds > 0.0;
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeLimitReached = [&]() {
        if (!hasTimeLimit) {
            return false;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsedMs >= maxMilliseconds;
    };
    BrickCoord coord{};
    const std::array<SparseResidencyClass, 4> classOrder{
        SparseResidencyClass::Edited,
        SparseResidencyClass::Collision,
        SparseResidencyClass::Visible,
        SparseResidencyClass::Speculative
    };
    // Surface work route: when the scheduler routed general catch-up to async, skip
    // the blocking fork-join batch so the per-coord loop below feeds the worker pool.
    // Only Visible/Speculative coords route to async there; Edited/Collision stay
    // synchronous so their surface-ready page publishes are not delayed (Codex
    // cross-check: render readiness needs published page + known surface).
    const bool routePreferAsync = SurfaceWorkRoutePreferAsyncActive();
    const bool parallelSurfaceAllowed =
        !routePreferAsync &&
        CanUseParallelSurfaceExtractionBatch(maxBricks);
    if (parallelSurfaceAllowed) {
        std::vector<SurfaceExtractionBatchItem> pending;
        const uint32_t parallelMaxBricks =
            (m_config.parallelSurfaceExtractionTimeBudgeted && hasTimeLimit)
                ? std::min(maxBricks, m_config.parallelSurfaceExtractionMaxBatch)
                : maxBricks;
        pending.reserve(parallelMaxBricks);
        for (SparseResidencyClass residencyClass : classOrder) {
            auto& classQueue = m_surfaceClassQueues[ResidencyClassQueueIndex(residencyClass)];
            SortQueuedBricksByValue(
                classQueue,
                m_pool,
                focus,
                currentFrame,
                m_config.streamingLaneQueuePriority);
            SortQueueByStreamingTickets(
                classQueue,
                kStreamingTicketStageSurfaceReady,
                &focus,
                currentFrame,
                true);
            while (pending.size() < static_cast<size_t>(parallelMaxBricks) &&
                   !timeLimitReached() &&
                   PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
                BrickResidentRecord record;
                if (!m_pool.GetRecord(coord, &record)) {
                    continue;
                }
                if (record.residencyClass != residencyClass) {
                    continue;
                }
                RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
                RemoveFirstSurfaceQueueCoord(coord);
                auto pendingIt = m_pendingSurfaceBricks.find(coord);
                if (pendingIt == m_pendingSurfaceBricks.end()) {
                    m_surfaceExtractionQueuedSet.erase(coord);
                    continue;
                }
                if (record.state != BrickLifecycleState::UploadQueued &&
                    record.state != BrickLifecycleState::UploadingGPU &&
                    record.state != BrickLifecycleState::Resident &&
                    record.state != BrickLifecycleState::DirtyCPU &&
                    record.state != BrickLifecycleState::DirtyGPU) {
                    m_pendingSurfaceBricks.erase(pendingIt);
                    m_surfaceDirtyRegions.erase(coord);
                    m_surfaceExtractionQueuedSet.erase(coord);
                    continue;
                }

                SurfaceExtractionBatchItem work;
                work.coord = coord;
                work.residencyClass = record.residencyClass;
                work.brick = std::move(pendingIt->second);
                pending.push_back(std::move(work));
                m_pendingSurfaceBricks.erase(pendingIt);
                m_surfaceExtractionQueuedSet.erase(coord);
            }
            if (pending.size() >= static_cast<size_t>(parallelMaxBricks) || timeLimitReached()) {
                break;
            }
        }

        if (!pending.empty()) {
            MarkQueueAccountingDirty();
            extracted = ExtractSurfaceBatchNoEdit(pending, outClassCounts);

            m_surfaceBricksExtractedLastFrame = extracted;
            RefreshStats();
            return extracted;
        }
    }

    for (SparseResidencyClass residencyClass : classOrder) {
        auto& classQueue = m_surfaceClassQueues[ResidencyClassQueueIndex(residencyClass)];
        const size_t classIndex = ResidencyClassQueueIndex(residencyClass);
        const bool strictTimeBudgetUnsorted =
            m_config.surfaceStrictTimeBudget && hasTimeLimit;
        if (strictTimeBudgetUnsorted) {
            m_surfaceClassValueSortValid[classIndex] = false;
        } else if (m_config.surfaceClassPartialValueSort) {
            const size_t remainingBudget = static_cast<size_t>(maxBricks - extracted);
            PartialSortQueuedBricksByValue(
                classQueue,
                m_pool,
                focus,
                currentFrame,
                std::max<size_t>(1u, remainingBudget),
                m_config.streamingLaneQueuePriority);
            SortQueueByStreamingTickets(
                classQueue,
                kStreamingTicketStageSurfaceReady,
                &focus,
                currentFrame,
                true,
                std::max<size_t>(1u, remainingBudget));
            ++m_surfaceClassValueSortCallsLastFrame;
            m_surfaceClassValueSortValid[classIndex] = false;
        } else {
            const bool valueSortValid =
                m_config.surfaceClassValueSortCache &&
                m_surfaceClassValueSortValid[classIndex] &&
                m_surfaceClassValueSortFocus[classIndex] == focus;
            if (valueSortValid) {
                ++m_surfaceClassValueSortCacheHitsLastFrame;
            } else {
                SortQueuedBricksByValue(
                    classQueue,
                    m_pool,
                    focus,
                    currentFrame,
                    m_config.streamingLaneQueuePriority);
                ++m_surfaceClassValueSortCallsLastFrame;
                if (m_config.surfaceClassValueSortCache) {
                    m_surfaceClassValueSortValid[classIndex] = true;
                    m_surfaceClassValueSortFocus[classIndex] = focus;
                }
            }
        }
        SortQueueByStreamingTickets(
            classQueue,
            kStreamingTicketStageSurfaceReady,
            &focus,
            currentFrame,
            true);
        while (extracted < maxBricks &&
               !timeLimitReached() &&
               PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
            if (strictTimeBudgetUnsorted) {
                ++m_surfaceStrictTimeBudgetUnsortedPopsLastFrame;
            }
            BrickResidentRecord record;
            if (!m_pool.GetRecord(coord, &record)) {
                continue;
            }
            if (record.residencyClass != residencyClass) {
                continue;
            }
            RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
            const bool classRoutable =
                residencyClass == SparseResidencyClass::Visible ||
                residencyClass == SparseResidencyClass::Speculative;
            const bool handledCoord = (routePreferAsync && !classRoutable)
                ? ExtractSurfaceCoord(coord)
                : ExtractOrQueueSurfaceCoord(coord);
            if (handledCoord) {
                RemoveFirstSurfaceQueueCoord(coord);
                ++extracted;
                if (outClassCounts) {
                    switch (record.residencyClass) {
                    case SparseResidencyClass::Edited:
                        ++outClassCounts->edited;
                        break;
                    case SparseResidencyClass::Collision:
                        ++outClassCounts->collision;
                        break;
                    case SparseResidencyClass::Visible:
                        ++outClassCounts->visible;
                        break;
                    case SparseResidencyClass::Speculative:
                        ++outClassCounts->speculative;
                        break;
                    }
                }
            } else {
                RemoveFirstSurfaceQueueCoord(coord);
            }
        }
        if (extracted >= maxBricks || timeLimitReached()) {
            break;
        }
    }

    m_surfaceBricksExtractedLastFrame = extracted;
    RefreshStats();
    return extracted;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionAroundTimedForClass(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame,
    double maxMilliseconds,
    SparseResidencyClass residencyClass)
{
    uint32_t extracted = 0;
    if (maxBricks == 0 || m_pendingSurfaceBricks.empty()) {
        PruneSurfaceExtractionQueuesIfNoPending();
        RefreshStats();
        return 0;
    }

    const bool hasTimeLimit = maxMilliseconds > 0.0;
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeLimitReached = [&]() {
        if (!hasTimeLimit) {
            return false;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsedMs >= maxMilliseconds;
    };

    auto& classQueue = m_surfaceClassQueues[ResidencyClassQueueIndex(residencyClass)];
    const size_t classIndex = ResidencyClassQueueIndex(residencyClass);
    const bool strictTimeBudgetUnsorted =
        m_config.surfaceStrictTimeBudget && hasTimeLimit;
    if (strictTimeBudgetUnsorted) {
        m_surfaceClassValueSortValid[classIndex] = false;
    } else if (m_config.surfaceClassPartialValueSort) {
        PartialSortQueuedBricksByValue(
            classQueue,
            m_pool,
            focus,
            currentFrame,
            std::max<size_t>(1u, maxBricks),
            m_config.streamingLaneQueuePriority);
        SortQueueByStreamingTickets(
            classQueue,
            kStreamingTicketStageSurfaceReady,
            &focus,
            currentFrame,
            true,
            std::max<size_t>(1u, maxBricks));
        ++m_surfaceClassValueSortCallsLastFrame;
        m_surfaceClassValueSortValid[classIndex] = false;
    } else {
        const bool valueSortValid =
            m_config.surfaceClassValueSortCache &&
            m_surfaceClassValueSortValid[classIndex] &&
            m_surfaceClassValueSortFocus[classIndex] == focus;
        if (valueSortValid) {
            ++m_surfaceClassValueSortCacheHitsLastFrame;
        } else {
            SortQueuedBricksByValue(
                classQueue,
                m_pool,
                focus,
                currentFrame,
                m_config.streamingLaneQueuePriority);
            ++m_surfaceClassValueSortCallsLastFrame;
            if (m_config.surfaceClassValueSortCache) {
                m_surfaceClassValueSortValid[classIndex] = true;
                m_surfaceClassValueSortFocus[classIndex] = focus;
            }
        }
    }
    SortQueueByStreamingTickets(
        classQueue,
        kStreamingTicketStageSurfaceReady,
        &focus,
        currentFrame,
        true);

    BrickCoord coord{};
    // Surface work route: routed frames prefer per-coord async enqueue over the
    // blocking fork-join batch, but ONLY for Visible/Speculative classes;
    // Edited/Collision class pumps keep today's batch/serial behavior so their
    // surface-ready page publishes are not delayed behind async meshing latency.
    const bool routePreferAsync =
        SurfaceWorkRoutePreferAsyncActive() &&
        (residencyClass == SparseResidencyClass::Visible ||
         residencyClass == SparseResidencyClass::Speculative);
    if (!routePreferAsync &&
        CanUseParallelSurfaceExtractionBatch(maxBricks)) {
        std::vector<SurfaceExtractionBatchItem> pending;
        const uint32_t parallelMaxBricks =
            (m_config.parallelSurfaceExtractionTimeBudgeted && hasTimeLimit)
                ? std::min(maxBricks, m_config.parallelSurfaceExtractionMaxBatch)
                : maxBricks;
        pending.reserve(parallelMaxBricks);
        while (pending.size() < static_cast<size_t>(parallelMaxBricks) &&
               !timeLimitReached() &&
               PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
            if (strictTimeBudgetUnsorted) {
                ++m_surfaceStrictTimeBudgetUnsortedPopsLastFrame;
            }
            BrickResidentRecord record;
            if (!m_pool.GetRecord(coord, &record)) {
                RemoveFirstSurfaceQueueCoord(coord);
                RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
                continue;
            }
            if (record.residencyClass != residencyClass) {
                continue;
            }
            RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
            RemoveFirstSurfaceQueueCoord(coord);

            auto pendingIt = m_pendingSurfaceBricks.find(coord);
            if (pendingIt == m_pendingSurfaceBricks.end()) {
                m_surfaceExtractionQueuedSet.erase(coord);
                continue;
            }
            if (record.state != BrickLifecycleState::UploadQueued &&
                record.state != BrickLifecycleState::UploadingGPU &&
                record.state != BrickLifecycleState::Resident &&
                record.state != BrickLifecycleState::DirtyCPU &&
                record.state != BrickLifecycleState::DirtyGPU) {
                m_pendingSurfaceBricks.erase(pendingIt);
                m_surfaceDirtyRegions.erase(coord);
                m_surfaceExtractionQueuedSet.erase(coord);
                continue;
            }

            SurfaceExtractionBatchItem work;
            work.coord = coord;
            work.residencyClass = record.residencyClass;
            work.brick = std::move(pendingIt->second);
            pending.push_back(std::move(work));
            m_pendingSurfaceBricks.erase(pendingIt);
            m_surfaceExtractionQueuedSet.erase(coord);
        }
        if (!pending.empty()) {
            MarkQueueAccountingDirty();
            extracted = ExtractSurfaceBatchNoEdit(pending);
            m_surfaceBricksExtractedLastFrame += extracted;
            RefreshStats();
            return extracted;
        }
    }

    while (extracted < maxBricks &&
           !timeLimitReached() &&
           PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
        if (strictTimeBudgetUnsorted) {
            ++m_surfaceStrictTimeBudgetUnsortedPopsLastFrame;
        }
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        if (record.residencyClass != residencyClass) {
            continue;
        }
        RemoveAllClassQueueCoord(m_surfaceClassQueues, coord);
        if (ExtractOrQueueSurfaceCoord(coord)) {
            RemoveFirstSurfaceQueueCoord(coord);
            ++extracted;
        } else {
            RemoveFirstSurfaceQueueCoord(coord);
        }
    }

    m_surfaceBricksExtractedLastFrame += extracted;
    RefreshStats();
    return extracted;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionAroundTimedForOwnershipCritical(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame,
    double maxMilliseconds,
    bool ownershipCritical)
{
    uint32_t extracted = 0;
    if (maxBricks == 0 || m_pendingSurfaceBricks.empty()) {
        PruneSurfaceExtractionQueuesIfNoPending();
        RefreshStats();
        return 0;
    }

    const bool hasTimeLimit = maxMilliseconds > 0.0;
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeLimitReached = [&]() {
        if (!hasTimeLimit) {
            return false;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsedMs >= maxMilliseconds;
    };

    auto& ownershipQueue = m_surfaceOwnershipQueues[OwnershipCriticalQueueIndex(ownershipCritical)];
    SortQueuedBricksByValue(
        ownershipQueue,
        m_pool,
        focus,
        currentFrame,
        m_config.streamingLaneQueuePriority);

    BrickCoord bestCoord{};
    // Surface work route: only the NON-critical ownership lane routes to async;
    // ownership-critical repair keeps its synchronous batch/inline paths.
    const bool routeToAsync = !ownershipCritical && SurfaceWorkRoutePreferAsyncActive();
    if (!routeToAsync && CanUseParallelSurfaceExtractionBatch(maxBricks)) {
        std::vector<SurfaceExtractionBatchItem> pending;
        const uint32_t parallelMaxBricks =
            (m_config.parallelSurfaceExtractionTimeBudgeted && hasTimeLimit)
                ? std::min(maxBricks, m_config.parallelSurfaceExtractionMaxBatch)
                : maxBricks;
        pending.reserve(parallelMaxBricks);
        while (pending.size() < static_cast<size_t>(parallelMaxBricks) &&
               !timeLimitReached() &&
               PopFrontQueuedBrick(ownershipQueue, m_pool, &bestCoord)) {
            BrickResidentRecord record;
            if (!m_pool.GetRecord(bestCoord, &record)) {
                RemoveFirstSurfaceQueueCoord(bestCoord);
                RemoveAllClassQueueCoord(m_surfaceClassQueues, bestCoord);
                continue;
            }
            if (IsStreamingOwnershipCritical(record) != ownershipCritical) {
                continue;
            }
            RemoveFirstSurfaceQueueCoord(bestCoord);
            RemoveAllClassQueueCoord(m_surfaceClassQueues, bestCoord);

            auto pendingIt = m_pendingSurfaceBricks.find(bestCoord);
            if (pendingIt == m_pendingSurfaceBricks.end()) {
                m_surfaceExtractionQueuedSet.erase(bestCoord);
                continue;
            }
            if (record.state != BrickLifecycleState::UploadQueued &&
                record.state != BrickLifecycleState::UploadingGPU &&
                record.state != BrickLifecycleState::Resident &&
                record.state != BrickLifecycleState::DirtyCPU &&
                record.state != BrickLifecycleState::DirtyGPU) {
                m_pendingSurfaceBricks.erase(pendingIt);
                m_surfaceDirtyRegions.erase(bestCoord);
                m_surfaceExtractionQueuedSet.erase(bestCoord);
                continue;
            }

            SurfaceExtractionBatchItem work;
            work.coord = bestCoord;
            work.residencyClass = record.residencyClass;
            work.brick = std::move(pendingIt->second);
            pending.push_back(std::move(work));
            m_pendingSurfaceBricks.erase(pendingIt);
            m_surfaceExtractionQueuedSet.erase(bestCoord);
        }
        if (!pending.empty()) {
            MarkQueueAccountingDirty();
            extracted = ExtractSurfaceBatchNoEdit(pending);
            m_surfaceBricksExtractedLastFrame += extracted;
            RefreshStats();
            return extracted;
        }
    }

    while (extracted < maxBricks &&
           !timeLimitReached() &&
           PopFrontQueuedBrick(ownershipQueue, m_pool, &bestCoord)) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(bestCoord, &record)) {
            RemoveFirstSurfaceQueueCoord(bestCoord);
            RemoveAllClassQueueCoord(m_surfaceClassQueues, bestCoord);
            continue;
        }
        if (IsStreamingOwnershipCritical(record) != ownershipCritical) {
            continue;
        }
        RemoveFirstSurfaceQueueCoord(bestCoord);
        RemoveAllClassQueueCoord(m_surfaceClassQueues, bestCoord);
        const bool coordRoutable =
            routeToAsync &&
            (record.residencyClass == SparseResidencyClass::Visible ||
             record.residencyClass == SparseResidencyClass::Speculative);
        const bool handled = coordRoutable
            ? ExtractOrQueueSurfaceCoord(bestCoord)
            : ExtractSurfaceCoord(bestCoord);
        if (handled) {
            ++extracted;
        }
    }

    m_surfaceBricksExtractedLastFrame += extracted;
    RefreshStats();
    return extracted;
}

uint32_t SparseVoxelWorld::TrimResidentBricks(
    const BrickCoord& center,
    uint32_t keepRadiusXz,
    uint32_t keepRadiusY,
    uint32_t maxEvictions)
{
    m_evictedBricksLastFrame = 0;
    if (maxEvictions == 0) {
        RefreshStats();
        return 0;
    }

    struct Candidate {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
        int64_t score = 0;
    };

    std::vector<Candidate> candidates;
    uint32_t recordsScanned = 0;
    const auto& records = m_pool.Records();
    const size_t recordCount = records.size();
    const bool incrementalScan =
        m_config.incrementalPressureTrim &&
        recordCount > static_cast<size_t>(m_config.incrementalPressureTrimScanBudget);
    const size_t scanCount = incrementalScan
        ? std::min<size_t>(recordCount, static_cast<size_t>(m_config.incrementalPressureTrimScanBudget))
        : recordCount;
    const size_t startIndex = incrementalScan && recordCount > 0
        ? m_trimResidentCursor % recordCount
        : 0u;
    for (size_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
        const auto& record = records[incrementalScan ? (startIndex + scanIndex) % recordCount : scanIndex];
        ++recordsScanned;
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive) {
            continue;
        }

        const int64_t dx = BrickCoordDelta(record.coord.x, center.x);
        const int64_t dy = BrickCoordDelta(record.coord.y, center.y);
        const int64_t dz = BrickCoordDelta(record.coord.z, center.z);
        if (WithinKeepRadius(dx, dy, dz, keepRadiusXz, keepRadiusY)) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const int64_t score = SaturatingAddInt64(
            SparseBrickDistanceScore(dx, dy, dz),
            -static_cast<int64_t>(ResidencyRetentionScore(record.residencyClass)));
        candidates.push_back({record.coord, record.pageIndex, record.generation, entryIndex, score});
    }
    if (incrementalScan && recordCount > 0) {
        m_trimResidentCursor = (startIndex + scanCount) % recordCount;
    } else {
        m_trimResidentCursor = 0;
    }

    ++m_trimScanCallsLastFrame;
    m_trimRecordsScannedLastFrame += recordsScanned;
    m_trimCandidatesLastFrame += static_cast<uint32_t>(std::min<size_t>(
        candidates.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    SortBestEvictionCandidates(candidates, maxEvictions, [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    uint32_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(candidate.coord)) {
            continue;
        }
        RemoveStreamingTicket(candidate.coord);
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredGeneratedDownstreamSet.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
        m_renderDirtyRegions.erase(candidate.coord);
        m_surfaceDirtyRegions.erase(candidate.coord);
        m_invalidationQueue.push_back({
            candidate.coord,
            candidate.entryIndex,
            candidate.pageIndex,
            candidate.generation
        });
        ++evicted;
    }

    m_evictedBricksLastFrame = evicted;
    RefreshStats();
    return evicted;
}

uint32_t SparseVoxelWorld::TrimStaleResidentBricks(
    uint32_t currentFrame,
    uint32_t staleFrames,
    uint32_t maxEvictions,
    uint32_t scanBudget)
{
    if (maxEvictions == 0 || currentFrame <= staleFrames) {
        return 0;
    }
    const uint32_t staleBeforeFrame = currentFrame - staleFrames;

    struct StaleTarget {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
    };
    std::vector<StaleTarget> targets;

    const auto& records = m_pool.Records();
    const size_t recordCount = records.size();
    if (recordCount == 0) {
        return 0;
    }
    const size_t scanCount = (scanBudget == 0)
        ? recordCount
        : std::min<size_t>(recordCount, static_cast<size_t>(scanBudget));
    const size_t startIndex = m_trimStaleResidentCursor % recordCount;
    for (size_t i = 0; i < scanCount && targets.size() < maxEvictions; ++i) {
        const auto& record = records[(startIndex + i) % recordCount];
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive ||
            record.residencyClass == SparseResidencyClass::Collision ||
            record.residencyClass == SparseResidencyClass::Edited) {
            continue;
        }
        // Recency, not the sticky class: a brick touched/wanted within the window is
        // still in (or near) the active view; only terrain not wanted for staleFrames
        // (flown past) is shed. The window absorbs budget-skip / 1-frame-timing slack,
        // so we never evict a brick that is about to be requested again -> no churn.
        if (record.lastTouchedFrame >= staleBeforeFrame) {
            continue;
        }
        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }
        targets.push_back({record.coord, record.pageIndex, record.generation, entryIndex});
    }
    m_trimStaleResidentCursor = (startIndex + scanCount) % recordCount;

    uint32_t evicted = 0;
    for (const StaleTarget& target : targets) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(target.coord)) {
            continue;
        }
        RemoveStreamingTicket(target.coord);
        m_pendingSurfaceBricks.erase(target.coord);
        m_surfaceExtractionQueuedSet.erase(target.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(target.coord);
        m_generated.erase(target.coord);
        m_deferredGeneratedDownstreamSet.erase(target.coord);
        m_deferredDirtyAfterUpload.erase(target.coord);
        m_renderDirtyRegions.erase(target.coord);
        m_surfaceDirtyRegions.erase(target.coord);
        m_invalidationQueue.push_back({
            target.coord,
            target.entryIndex,
            target.pageIndex,
            target.generation
        });
        ++evicted;
    }

    if (evicted != 0) {
        m_evictedBricksLastFrame += evicted;
        RefreshStats();
    }
    return evicted;
}

uint32_t SparseVoxelWorld::TrimBackgroundResidentBricks(
    const BrickCoord& center,
    uint32_t keepRadiusXz,
    uint32_t keepRadiusY,
    uint32_t maxEvictions,
    uint32_t currentFrame)
{
    m_evictedBricksLastFrame = 0;
    if (maxEvictions == 0) {
        RefreshStats();
        return 0;
    }

    struct Candidate {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
        int64_t score = 0;
    };

    std::vector<Candidate> candidates;
    uint32_t recordsScanned = 0;
    const auto& records = m_pool.Records();
    const size_t recordCount = records.size();
    const bool incrementalScan =
        m_config.incrementalPressureTrim &&
        recordCount > static_cast<size_t>(m_config.incrementalPressureTrimScanBudget);
    const size_t scanCount = incrementalScan
        ? std::min<size_t>(recordCount, static_cast<size_t>(m_config.incrementalPressureTrimScanBudget))
        : recordCount;
    const size_t startIndex = incrementalScan && recordCount > 0
        ? m_trimBackgroundResidentCursor % recordCount
        : 0u;
    for (size_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
        const auto& record = records[incrementalScan ? (startIndex + scanIndex) % recordCount : scanIndex];
        ++recordsScanned;
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive ||
            record.residencyClass == SparseResidencyClass::Collision ||
            record.residencyClass == SparseResidencyClass::Edited) {
            continue;
        }

        const int64_t dx = BrickCoordDelta(record.coord.x, center.x);
        const int64_t dy = BrickCoordDelta(record.coord.y, center.y);
        const int64_t dz = BrickCoordDelta(record.coord.z, center.z);
        if (WithinKeepRadius(dx, dy, dz, keepRadiusXz, keepRadiusY)) {
            continue;
        }
        if (record.residencyClass == SparseResidencyClass::Visible &&
            currentFrame != 0u &&
            record.lastVisibleFrame == currentFrame) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const uint32_t cappedAge = std::min(age, 100000u);
        const int64_t distanceScore = SparseBrickDistanceScore(dx, dy, dz);
        const int64_t classBias = record.residencyClass == SparseResidencyClass::Speculative
            ? 1'000'000ll
            : 0ll;
        const int64_t ageScore = static_cast<int64_t>(cappedAge) * 32ll;
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            SaturatingAddInt64(
                SaturatingAddInt64(classBias, distanceScore),
                ageScore)
        });
    }
    if (incrementalScan && recordCount > 0) {
        m_trimBackgroundResidentCursor = (startIndex + scanCount) % recordCount;
    } else {
        m_trimBackgroundResidentCursor = 0;
    }

    ++m_trimScanCallsLastFrame;
    m_trimRecordsScannedLastFrame += recordsScanned;
    m_trimCandidatesLastFrame += static_cast<uint32_t>(std::min<size_t>(
        candidates.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    SortBestEvictionCandidates(candidates, maxEvictions, [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    uint32_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(candidate.coord)) {
            continue;
        }
        RemoveStreamingTicket(candidate.coord);
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredGeneratedDownstreamSet.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
        m_renderDirtyRegions.erase(candidate.coord);
        m_surfaceDirtyRegions.erase(candidate.coord);
        m_invalidationQueue.push_back({
            candidate.coord,
            candidate.entryIndex,
            candidate.pageIndex,
            candidate.generation
        });
        ++evicted;
    }

    m_evictedBricksLastFrame = evicted;
    RefreshStats();
    return evicted;
}

uint32_t SparseVoxelWorld::TrimQueuedBackgroundBricks(
    const BrickCoord& center,
    uint32_t keepRadiusXz,
    uint32_t keepRadiusY,
    uint32_t maxEvictions,
    uint32_t currentFrame)
{
    m_evictedBricksLastFrame = 0;
    if (maxEvictions == 0) {
        RefreshStats();
        return 0;
    }

    struct Candidate {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        int64_t score = 0;
    };

    std::vector<Candidate> candidates;
    uint32_t recordsScanned = 0;
    const auto& records = m_pool.Records();
    const size_t recordCount = records.size();
    const bool incrementalScan =
        m_config.incrementalPressureTrim &&
        recordCount > static_cast<size_t>(m_config.incrementalPressureTrimScanBudget);
    const size_t scanCount = incrementalScan
        ? std::min<size_t>(recordCount, static_cast<size_t>(m_config.incrementalPressureTrimScanBudget))
        : recordCount;
    const size_t startIndex = incrementalScan && recordCount > 0
        ? m_trimQueuedBackgroundCursor % recordCount
        : 0u;
    for (size_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
        const auto& record = records[incrementalScan ? (startIndex + scanIndex) % recordCount : scanIndex];
        ++recordsScanned;
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive ||
            record.residencyClass == SparseResidencyClass::Collision ||
            record.residencyClass == SparseResidencyClass::Edited) {
            continue;
        }

        const bool queuedState =
            record.state == BrickLifecycleState::Requested ||
            record.state == BrickLifecycleState::GeneratedCPU ||
            record.state == BrickLifecycleState::UploadQueued;
        if (!queuedState) {
            continue;
        }

        const int64_t dx = BrickCoordDelta(record.coord.x, center.x);
        const int64_t dy = BrickCoordDelta(record.coord.y, center.y);
        const int64_t dz = BrickCoordDelta(record.coord.z, center.z);
        if (WithinKeepRadius(dx, dy, dz, keepRadiusXz, keepRadiusY)) {
            continue;
        }

        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const int64_t distanceScore = SparseBrickDistanceScore(dx, dy, dz);
        const int64_t classBias = record.residencyClass == SparseResidencyClass::Speculative
            ? 2'000'000ll
            : 500'000ll;
        const int64_t ageScore = static_cast<int64_t>(std::min(age, 100000u)) * 64ll;
        uint32_t entryIndex = UINT32_MAX;
        m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex);
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            record.residencyClass,
            SaturatingAddInt64(
                SaturatingAddInt64(classBias, distanceScore),
                ageScore)
        });
    }
    if (incrementalScan && recordCount > 0) {
        m_trimQueuedBackgroundCursor = (startIndex + scanCount) % recordCount;
    } else {
        m_trimQueuedBackgroundCursor = 0;
    }

    ++m_trimScanCallsLastFrame;
    m_trimRecordsScannedLastFrame += recordsScanned;
    m_trimCandidatesLastFrame += static_cast<uint32_t>(std::min<size_t>(
        candidates.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    SortBestEvictionCandidates(candidates, maxEvictions, [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    uint32_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(candidate.coord)) {
            continue;
        }

        RemoveStreamingTicket(candidate.coord);
        RemoveFirstGenerationQueueCoord(candidate.coord);
        RemoveFirstGenerationClassQueueCoord(candidate.coord, candidate.residencyClass);
        RemoveFirstUploadQueueCoord(candidate.coord);
        RemoveFirstUploadClassQueueCoord(candidate.coord, candidate.residencyClass);
        RemoveFirstSurfaceQueueCoord(candidate.coord);
        RemoveFirstSurfaceClassQueueCoord(candidate.coord, candidate.residencyClass);
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredGeneratedDownstreamSet.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
        m_renderDirtyRegions.erase(candidate.coord);
        m_surfaceDirtyRegions.erase(candidate.coord);
        if (candidate.entryIndex != UINT32_MAX) {
            m_invalidationQueue.push_back({
                candidate.coord,
                candidate.entryIndex,
                candidate.pageIndex,
                candidate.generation
            });
        }
        MarkQueueAccountingDirty();
        ++evicted;
    }

    m_evictedBricksLastFrame = evicted;
    RefreshStats();
    return evicted;
}

uint32_t SparseVoxelWorld::EvictQueuedLowerPriorityForRequest(
    const BrickCoord& center,
    SparseResidencyClass requestClass,
    uint32_t hardKeepRadiusXz,
    uint32_t hardKeepRadiusY,
    uint32_t maxEvictions,
    uint32_t currentFrame)
{
    if (maxEvictions == 0 || requestClass == SparseResidencyClass::Speculative) {
        RefreshStats();
        return 0;
    }

    struct Candidate {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        int64_t score = 0;
    };

    std::vector<Candidate> candidates;
    const uint8_t requestRank = ResidencyRank(requestClass);
    uint32_t recordsScanned = 0;
    for (const auto& record : m_pool.Records()) {
        ++recordsScanned;
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive ||
            ResidencyRank(record.residencyClass) > requestRank) {
            continue;
        }

        const bool queuedState =
            record.state == BrickLifecycleState::Requested ||
            record.state == BrickLifecycleState::GeneratedCPU ||
            record.state == BrickLifecycleState::UploadQueued;
        if (!queuedState) {
            continue;
        }

        const int64_t dx = BrickCoordDelta(record.coord.x, center.x);
        const int64_t dy = BrickCoordDelta(record.coord.y, center.y);
        const int64_t dz = BrickCoordDelta(record.coord.z, center.z);
        if (WithinKeepRadius(dx, dy, dz, hardKeepRadiusXz, hardKeepRadiusY)) {
            continue;
        }
        if (record.residencyClass == SparseResidencyClass::Visible &&
            currentFrame != 0u &&
            record.lastVisibleFrame == currentFrame) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex);

        const int64_t classPenalty = static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 100000ll;
        const int64_t distanceScore = SparseBrickDistanceScore(dx, dy, dz);
        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const uint32_t cappedAge = std::min(age, 100000u);
        const int64_t ageScore = static_cast<int64_t>(cappedAge) * 64ll;
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            record.residencyClass,
            SaturatingAddInt64(
                SaturatingAddInt64(distanceScore, ageScore),
                -classPenalty)
        });
    }

    ++m_replacementScanCallsLastFrame;
    m_replacementRecordsScannedLastFrame += recordsScanned;
    m_replacementCandidatesLastFrame += static_cast<uint32_t>(std::min<size_t>(
        candidates.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    SortBestEvictionCandidates(candidates, maxEvictions, [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    uint32_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(candidate.coord)) {
            continue;
        }

        RemoveStreamingTicket(candidate.coord);
        RemoveFirstGenerationQueueCoord(candidate.coord);
        RemoveFirstGenerationClassQueueCoord(candidate.coord, candidate.residencyClass);
        RemoveFirstUploadQueueCoord(candidate.coord);
        RemoveFirstUploadClassQueueCoord(candidate.coord, candidate.residencyClass);
        RemoveFirstSurfaceQueueCoord(candidate.coord);
        RemoveFirstSurfaceClassQueueCoord(candidate.coord, candidate.residencyClass);
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredGeneratedDownstreamSet.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
        m_renderDirtyRegions.erase(candidate.coord);
        m_surfaceDirtyRegions.erase(candidate.coord);
        if (candidate.entryIndex != UINT32_MAX) {
            m_invalidationQueue.push_back({
                candidate.coord,
                candidate.entryIndex,
                candidate.pageIndex,
                candidate.generation
            });
        }
        MarkQueueAccountingDirty();
        ++evicted;
    }

    if (evicted > 0) {
        m_evictedBricksLastFrame += evicted;
    }
    RefreshStats();
    return evicted;
}

uint32_t SparseVoxelWorld::EvictLowerPriorityForRequest(
    const BrickCoord& center,
    SparseResidencyClass requestClass,
    uint32_t hardKeepRadiusXz,
    uint32_t hardKeepRadiusY,
    uint32_t maxEvictions,
    uint32_t currentFrame)
{
    if (maxEvictions == 0 || requestClass == SparseResidencyClass::Speculative) {
        RefreshStats();
        return 0;
    }

    struct Candidate {
        BrickCoord coord;
        uint32_t pageIndex = INVALID_BRICK_PAGE;
        uint32_t generation = 0;
        uint32_t entryIndex = UINT32_MAX;
        int64_t score = 0;
    };

    std::vector<Candidate> candidates;
    const uint8_t requestRank = ResidencyRank(requestClass);
    uint32_t recordsScanned = 0;
    for (const auto& record : m_pool.Records()) {
        ++recordsScanned;
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            (!m_config.evictPersistentEditedBricks && record.hasPersistentEdits) ||
            record.physicsActive ||
            ResidencyRank(record.residencyClass) > requestRank) {
            continue;
        }

        const int64_t dx = BrickCoordDelta(record.coord.x, center.x);
        const int64_t dy = BrickCoordDelta(record.coord.y, center.y);
        const int64_t dz = BrickCoordDelta(record.coord.z, center.z);
        if (WithinKeepRadius(dx, dy, dz, hardKeepRadiusXz, hardKeepRadiusY)) {
            continue;
        }
        if (record.residencyClass == SparseResidencyClass::Visible &&
            currentFrame != 0u &&
            record.lastVisibleFrame == currentFrame) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const int64_t classPenalty = static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 100000ll;
        const int64_t distanceScore = SparseBrickDistanceScore(dx, dy, dz);
        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const uint32_t cappedAge = std::min(age, 100000u);
        const int64_t ageScore = static_cast<int64_t>(cappedAge) * 64ll;
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            SaturatingAddInt64(
                SaturatingAddInt64(distanceScore, ageScore),
                -classPenalty)
        });
    }

    ++m_replacementScanCallsLastFrame;
    m_replacementRecordsScannedLastFrame += recordsScanned;
    m_replacementCandidatesLastFrame += static_cast<uint32_t>(std::min<size_t>(
        candidates.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    SortBestEvictionCandidates(candidates, maxEvictions, [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.coord < b.coord;
    });

    uint32_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (evicted >= maxEvictions) {
            break;
        }
        if (!m_pool.Evict(candidate.coord)) {
            continue;
        }
        RemoveStreamingTicket(candidate.coord);
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredGeneratedDownstreamSet.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
        m_renderDirtyRegions.erase(candidate.coord);
        m_surfaceDirtyRegions.erase(candidate.coord);
        m_invalidationQueue.push_back({
            candidate.coord,
            candidate.entryIndex,
            candidate.pageIndex,
            candidate.generation
        });
        ++evicted;
    }

    if (evicted > 0) {
        m_evictedBricksLastFrame += evicted;
    }
    RefreshStats();
    return evicted;
}

bool SparseVoxelWorld::PopNextInvalidation(SparsePageInvalidationPacket* outPacket) {
    if (!outPacket || m_invalidationQueue.empty()) {
        return false;
    }
    *outPacket = m_invalidationQueue.front();
    m_invalidationQueue.pop_front();
    RefreshStats();
    return true;
}

void SparseVoxelWorld::RequeueInvalidationFront(const SparsePageInvalidationPacket& packet) {
    m_invalidationQueue.push_front(packet);
    RefreshStats();
}

bool SparseVoxelWorld::MarkResidencyClass(const BrickCoord& coord, SparseResidencyClass residencyClass) {
    if (!m_pool.MarkResidencyClass(coord, residencyClass)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, 0u);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::MarkStreamingLane(const BrickCoord& coord, SparseStreamingLane lane) {
    if (!m_pool.MarkStreamingLane(coord, lane)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, 0u);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidencyClass(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    if (!m_pool.TouchResidencyClass(coord, residencyClass, frameIndex, queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidencyClassWithStreamingLane(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    SparseStreamingLane streamingLane,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    const SparseResidencyClass effectiveResidencyClass =
        m_config.prefetchLaneSpeculativeClass &&
        residencyClass == SparseResidencyClass::Visible &&
        streamingLane == SparseStreamingLane::Prefetch
            ? SparseResidencyClass::Speculative
            : residencyClass;
    if (!m_pool.TouchResidencyClassWithStreamingLane(
            coord,
            effectiveResidencyClass,
            streamingLane,
            frameIndex,
            queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchStreamingLane(
    const BrickCoord& coord,
    SparseStreamingLane lane,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    if (!m_pool.TouchStreamingLane(coord, lane, frameIndex, queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidencyClassKnownPage(
    uint32_t pageIndex,
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    if (!m_pool.TouchResidencyClassKnownPage(pageIndex, coord, residencyClass, frameIndex, queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidencyClassWithStreamingLaneKnownPage(
    uint32_t pageIndex,
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    SparseStreamingLane streamingLane,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    const SparseResidencyClass effectiveResidencyClass =
        m_config.prefetchLaneSpeculativeClass &&
        residencyClass == SparseResidencyClass::Visible &&
        streamingLane == SparseStreamingLane::Prefetch
            ? SparseResidencyClass::Speculative
            : residencyClass;
    if (!m_pool.TouchResidencyClassWithStreamingLaneKnownPage(
            pageIndex,
            coord,
            effectiveResidencyClass,
            streamingLane,
            frameIndex,
            queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchStreamingLaneKnownPage(
    uint32_t pageIndex,
    const BrickCoord& coord,
    SparseStreamingLane lane,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    if (!m_pool.TouchStreamingLaneKnownPage(
            pageIndex,
            coord,
            lane,
            frameIndex,
            queuePriority)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    BrickResidentRecord record;
    if (m_pool.GetRecord(coord, &record)) {
        UpdateStreamingTicketFromRecord(coord, record);
        PromoteDeferredGeneratedDownstreamIfCritical(coord, frameIndex);
    }
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidentRetention(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    return m_pool.TouchResidencyClass(coord, residencyClass, frameIndex, queuePriority);
}

bool SparseVoxelWorld::TouchResidentRetentionKnownPage(
    uint32_t pageIndex,
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex,
    int32_t queuePriority)
{
    return m_pool.TouchResidencyClassKnownPage(pageIndex, coord, residencyClass, frameIndex, queuePriority);
}

bool SparseVoxelWorld::QueuePhysicsCandidateNoStats(const BrickCoord& coord) {
    SparsePhysicsDirtyRegion fullRegion;
    return QueuePhysicsRegionNoStats(coord, fullRegion, SparsePhysicsPriority::Warm);
}

bool SparseVoxelWorld::QueuePhysicsRegionNoStats(
    const BrickCoord& coord,
    const SparsePhysicsDirtyRegion& region,
    SparsePhysicsPriority priority)
{
    auto [regionIt, insertedRegion] = m_physicsDirtyRegions.emplace(coord, region);
    if (!insertedRegion) {
        MergeSparseRegion(regionIt->second, region);
    }

    auto priorityIt = m_physicsQueuedPriorities.find(coord);
    if (priorityIt == m_physicsQueuedPriorities.end()) {
        m_physicsQueuedPriorities[coord] = priority;
        if (priority == SparsePhysicsPriority::Hot) {
            m_physicsHotQueue.push_back(coord);
        } else {
            m_physicsWarmQueue.push_back(coord);
        }
        return true;
    }
    if (priorityIt->second == SparsePhysicsPriority::Warm &&
        priority == SparsePhysicsPriority::Hot) {
        priorityIt->second = SparsePhysicsPriority::Hot;
        m_physicsHotQueue.push_back(coord);
    }
    return false;
}

void SparseVoxelWorld::QueueRenderDirtyRegionNoStats(
    const BrickCoord& coord,
    const SparseRenderDirtyRegion& region,
    bool queueSurfaceDirty)
{
    auto [regionIt, insertedRegion] = m_renderDirtyRegions.emplace(coord, region);
    if (!insertedRegion) {
        MergeSparseRegion(regionIt->second, region);
    }
    m_renderDirtyVoxelsQueuedLastFrame += SparseRegionVoxelCount(region);
    if (queueSurfaceDirty) {
        QueueSurfaceDirtyRegionNoStats(coord, region);
    }
}

void SparseVoxelWorld::QueueSurfaceDirtyRegionNoStats(
    const BrickCoord& coord,
    const SparseRenderDirtyRegion& region)
{
    const auto queueOne = [this](
        const BrickCoord& dirtyCoord,
        const SparseSurfaceLocalRegion& dirtyRegion,
        bool queueResidentSurface) {
        auto [surfaceRegionIt, inserted] = m_surfaceDirtyRegions.emplace(dirtyCoord, dirtyRegion);
        if (!inserted) {
            MergeSparseRegion(surfaceRegionIt->second, dirtyRegion);
        }

        if (!queueResidentSurface || !m_pool.IsResident(dirtyCoord)) {
            return;
        }

        // Loop 98 NOTE: stroke coalescing (skip the per-dab regen here, refresh
        // once at extraction) was A/B-REFUTED — it moved the regen cost inside
        // the budgeted extraction window, throttling the publish drain further
        // (p50 14->15.2, >16.7 frames 104->189, queue peak 1103->1389). Profile
        // with HEIGHTAT_SCOPE before the next attempt at this cost.
        GeneratedSparseBrick brick = GenerateBrickWithCachedTerrainColumns(dirtyCoord);
        m_edits.ApplyToGeneratedBrick(brick);
        m_pendingSurfaceBricks[dirtyCoord] = std::move(brick);
        QueueSurfaceExtractionCoord(dirtyCoord);
    };

    SparseSurfaceLocalRegion primary{
        region.minX,
        region.minY,
        region.minZ,
        region.maxX,
        region.maxY,
        region.maxZ};
    // The edited brick itself is queued after its voxel payload upload
    // completes. Adjacent boundary bricks do not need a voxel upload, but their
    // exposed faces can change, so they are queued immediately if resident.
    queueOne(coord, primary, false);

    if (region.minX == 0) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, -1, 0, 0, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                SPARSE_BRICK_SIZE - 1, region.minY, region.minZ,
                SPARSE_BRICK_SIZE - 1, region.maxY, region.maxZ}, true);
        }
    }
    if (region.maxX == SPARSE_BRICK_SIZE - 1) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, 1, 0, 0, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                0, region.minY, region.minZ,
                0, region.maxY, region.maxZ}, true);
        }
    }
    if (region.minY == 0) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, 0, -1, 0, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                region.minX, SPARSE_BRICK_SIZE - 1, region.minZ,
                region.maxX, SPARSE_BRICK_SIZE - 1, region.maxZ}, true);
        }
    }
    if (region.maxY == SPARSE_BRICK_SIZE - 1) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, 0, 1, 0, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                region.minX, 0, region.minZ,
                region.maxX, 0, region.maxZ}, true);
        }
    }
    if (region.minZ == 0) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, 0, 0, -1, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                region.minX, region.minY, SPARSE_BRICK_SIZE - 1,
                region.maxX, region.maxY, SPARSE_BRICK_SIZE - 1}, true);
        }
    }
    if (region.maxZ == SPARSE_BRICK_SIZE - 1) {
        BrickCoord neighbor;
        if (TryOffsetBrickCoord(coord, 0, 0, 1, &neighbor)) {
            queueOne(neighbor, SparseSurfaceLocalRegion{
                region.minX, region.minY, 0,
                region.maxX, region.maxY, 0}, true);
        }
    }
}

void SparseVoxelWorld::QueueRenderDirtyVoxelNoStats(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ)
{
    const BrickCoord coord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    const LocalVoxelCoord local = LocalVoxelFromWorld(worldX, worldY, worldZ);
    SparseRenderDirtyRegion region;
    region.minX = region.maxX = local.x;
    region.minY = region.maxY = local.y;
    region.minZ = region.maxZ = local.z;
    QueueRenderDirtyRegionNoStats(coord, region);
}

bool SparseVoxelWorld::QueuePhysicsVoxelNoStats(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    SparsePhysicsPriority priority)
{
    const BrickCoord coord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    const LocalVoxelCoord local = LocalVoxelFromWorld(worldX, worldY, worldZ);
    SparsePhysicsDirtyRegion region;
    region.minX = region.maxX = local.x;
    region.minY = region.maxY = local.y;
    region.minZ = region.maxZ = local.z;
    return QueuePhysicsRegionNoStats(coord, region, priority);
}

void SparseVoxelWorld::WakePhysicsSupportNeighborhoodNoStats(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ)
{
    int32_t supportY = 0;
    if (!TryStepInt32(worldY, 1, &supportY)) {
        return;
    }
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            int32_t supportX = 0;
            int32_t supportZ = 0;
            if (!TryStepInt32(worldX, dx, &supportX) ||
                !TryStepInt32(worldZ, dz, &supportZ)) {
                continue;
            }
            // Removing support only wakes the immediately supported column
            // above the edit. Queue exact voxels instead of full bricks so
            // erase/brush strokes do not turn into broad local scans.
            QueuePhysicsVoxelNoStats(
                supportX,
                supportY,
                supportZ,
                SparsePhysicsPriority::Hot);
        }
    }
}

float SparseVoxelWorld::CachedTerrainHeightAt(int32_t worldX, int32_t worldZ) {
    const bool collectCacheStats = m_config.persistentTerrainColumnCache;
    const TerrainSurfaceColumnKey key{worldX, worldZ};
    auto columnIt = m_surfaceTerrainColumnCache.find(key);
    if (columnIt != m_surfaceTerrainColumnCache.end()) {
        if (collectCacheStats) {
            ++m_terrainColumnCacheFrameStats.heightHits;
        }
        return columnIt->second.height;
    }

    if (collectCacheStats) {
        ++m_terrainColumnCacheFrameStats.heightMisses;
    }
    TerrainSurfaceColumnCacheEntry entry;
    entry.height = m_terrain.HeightAt(worldX, worldZ);
    auto [insertedIt, inserted] = m_surfaceTerrainColumnCache.emplace(key, entry);
    (void)inserted;
    return insertedIt->second.height;
}

float SparseVoxelWorld::CachedTerrainReliefAt(
    int32_t worldX,
    int32_t worldZ,
    int32_t sampleOffset)
{
    const bool collectCacheStats = m_config.persistentTerrainColumnCache;
    const int32_t offset = std::max(1, sampleOffset);
    const TerrainSurfaceColumnKey key{worldX, worldZ};
    auto columnIt = m_surfaceTerrainColumnCache.find(key);
    if (columnIt == m_surfaceTerrainColumnCache.end()) {
        if (collectCacheStats) {
            ++m_terrainColumnCacheFrameStats.heightMisses;
        }
        TerrainSurfaceColumnCacheEntry entry;
        entry.height = m_terrain.HeightAt(worldX, worldZ);
        columnIt = m_surfaceTerrainColumnCache.emplace(key, entry).first;
    }
    if (columnIt->second.reliefValid && columnIt->second.reliefSampleOffset == offset) {
        if (collectCacheStats) {
            ++m_terrainColumnCacheFrameStats.reliefHits;
        }
        return columnIt->second.relief;
    }

    if (collectCacheStats) {
        ++m_terrainColumnCacheFrameStats.reliefMisses;
    }
    int32_t xMinus = worldX;
    int32_t xPlus = worldX;
    int32_t zMinus = worldZ;
    int32_t zPlus = worldZ;
    (void)TryStepInt32(worldX, -offset, &xMinus);
    (void)TryStepInt32(worldX, offset, &xPlus);
    (void)TryStepInt32(worldZ, -offset, &zMinus);
    (void)TryStepInt32(worldZ, offset, &zPlus);

    const float center = columnIt->second.height;
    float localMin = center;
    float localMax = center;
    const float samples[] = {
        CachedTerrainHeightAt(xMinus, worldZ),
        CachedTerrainHeightAt(xPlus, worldZ),
        CachedTerrainHeightAt(worldX, zMinus),
        CachedTerrainHeightAt(worldX, zPlus),
    };
    for (float height : samples) {
        localMin = std::min(localMin, height);
        localMax = std::max(localMax, height);
    }

    columnIt->second.relief = localMax - localMin;
    columnIt->second.reliefSampleOffset = offset;
    columnIt->second.reliefValid = true;
    return columnIt->second.relief;
}

bool SparseVoxelWorld::PopNextPhysicsWorkPacket(SparsePhysicsWorkPacket* outPacket) {
    auto buildPacket = [&](const BrickCoord& coord, SparsePhysicsPriority priority) {
        SparsePhysicsDirtyRegion region;
        auto regionIt = m_physicsDirtyRegions.find(coord);
        if (regionIt != m_physicsDirtyRegions.end()) {
            region = regionIt->second;
            m_physicsDirtyRegions.erase(regionIt);
        }
        outPacket->coord = coord;
        outPacket->packedRegionMin = PackPhysicsRegionPoint(region.minX, region.minY, region.minZ);
        outPacket->packedRegionMax = PackPhysicsRegionPoint(region.maxX, region.maxY, region.maxZ);
        outPacket->materialMask =
            SPARSE_PHYSICS_MATERIAL_SAND |
            SPARSE_PHYSICS_MATERIAL_WATER |
            SPARSE_PHYSICS_MATERIAL_LAVA;
        outPacket->priority = priority == SparsePhysicsPriority::Hot ? 1u : 0u;
        outPacket->generation = m_physicsWorkGeneration;
        BrickResidentRecord record;
        if (m_pool.GetRecord(coord, &record)) {
            outPacket->expectedPageIndex = record.pageIndex;
            outPacket->expectedPageGeneration = record.generation;
        } else {
            outPacket->expectedPageIndex = INVALID_BRICK_PAGE;
            outPacket->expectedPageGeneration = 0;
        }
    };

    while (!m_physicsHotQueue.empty()) {
        const BrickCoord coord = m_physicsHotQueue.front();
        m_physicsHotQueue.pop_front();
        auto priorityIt = m_physicsQueuedPriorities.find(coord);
        if (priorityIt != m_physicsQueuedPriorities.end() &&
            priorityIt->second == SparsePhysicsPriority::Hot) {
            m_physicsQueuedPriorities.erase(priorityIt);
            buildPacket(coord, SparsePhysicsPriority::Hot);
            return true;
        }
    }

    while (!m_physicsWarmQueue.empty()) {
        const BrickCoord coord = m_physicsWarmQueue.front();
        m_physicsWarmQueue.pop_front();
        auto priorityIt = m_physicsQueuedPriorities.find(coord);
        if (priorityIt != m_physicsQueuedPriorities.end() &&
            priorityIt->second == SparsePhysicsPriority::Warm) {
            m_physicsQueuedPriorities.erase(priorityIt);
            buildPacket(coord, SparsePhysicsPriority::Warm);
            return true;
        }
    }
    return false;
}

uint32_t SparseVoxelWorld::BuildPhysicsWorkBatch(uint32_t maxPackets) {
    m_physicsStagedPackets.clear();
    const uint32_t cappedMaxPackets = std::min(maxPackets, kMaxSparseLocalPhysicsWorkPackets);
    m_physicsStagedPackets.reserve(cappedMaxPackets);
    m_physicsHotWorkPacketsLastFrame = 0;
    m_physicsWarmWorkPacketsLastFrame = 0;
    m_physicsDirtyRegionVoxelsLastFrame = 0;
    ++m_physicsWorkGeneration;

    SparsePhysicsWorkPacket packet;
    while (m_physicsStagedPackets.size() < cappedMaxPackets && PopNextPhysicsWorkPacket(&packet)) {
        m_physicsStagedPackets.push_back(packet);
        if (packet.priority != 0u) {
            ++m_physicsHotWorkPacketsLastFrame;
        } else {
            ++m_physicsWarmWorkPacketsLastFrame;
        }
        SparsePhysicsDirtyRegion countedRegion;
        const LocalVoxelCoord regionMin = UnpackPhysicsRegionPoint(packet.packedRegionMin);
        const LocalVoxelCoord regionMax = UnpackPhysicsRegionPoint(packet.packedRegionMax);
        countedRegion.minX = regionMin.x;
        countedRegion.minY = regionMin.y;
        countedRegion.minZ = regionMin.z;
        countedRegion.maxX = regionMax.x;
        countedRegion.maxY = regionMax.y;
        countedRegion.maxZ = regionMax.z;
        m_physicsDirtyRegionVoxelsLastFrame += SparseRegionVoxelCount(countedRegion);
    }
    return static_cast<uint32_t>(m_physicsStagedPackets.size());
}

void SparseVoxelWorld::RequeuePhysicsWorkPacketNoStats(const SparsePhysicsWorkPacket& packet) {
    SparsePhysicsDirtyRegion region;
    const LocalVoxelCoord regionMin = UnpackPhysicsRegionPoint(packet.packedRegionMin);
    const LocalVoxelCoord regionMax = UnpackPhysicsRegionPoint(packet.packedRegionMax);
    region.minX = regionMin.x;
    region.minY = regionMin.y;
    region.minZ = regionMin.z;
    region.maxX = regionMax.x;
    region.maxY = regionMax.y;
    region.maxZ = regionMax.z;
    QueuePhysicsRegionNoStats(
        packet.coord,
        region,
        packet.priority != 0u ? SparsePhysicsPriority::Hot : SparsePhysicsPriority::Warm);
}

void SparseVoxelWorld::QueuePhysicsCandidate(const BrickCoord& coord) {
    QueuePhysicsCandidateNoStats(coord);
    RefreshStats();
}

uint32_t SparseVoxelWorld::StepLocalPhysics(
    uint32_t maxBricks,
    uint32_t maxVoxelMoves,
    bool requestRenderBricks)
{
    StageLocalPhysicsWork(maxBricks);
    return ExecuteStagedLocalPhysics(maxVoxelMoves, requestRenderBricks);
}

uint32_t SparseVoxelWorld::StageLocalPhysicsWork(uint32_t maxBricks) {
    m_physicsProcessedBricksLastFrame = 0;
    m_physicsWorkPacketsLastFrame = 0;
    m_physicsMovedVoxelsLastFrame = 0;
    m_physicsSkippedVoxelsLastFrame = 0;
    m_physicsSupportBricksRequestedLastFrame = 0;
    m_physicsHotWorkPacketsLastFrame = 0;
    m_physicsWarmWorkPacketsLastFrame = 0;
    m_physicsDirtyRegionVoxelsLastFrame = 0;

    m_physicsStagedPackets.clear();
    if (maxBricks == 0) {
        RefreshStats();
        return 0;
    }

    const uint32_t stagedPackets = BuildPhysicsWorkBatch(maxBricks);
    uint32_t supportRequests = 0;
    const bool wasStatsDeferred = m_statsRefreshDeferred;
    if (!wasStatsDeferred) {
        SetStatsRefreshDeferred(true);
    }
    for (const SparsePhysicsWorkPacket& packet : m_physicsStagedPackets) {
        const LocalVoxelCoord regionMin = UnpackPhysicsRegionPoint(packet.packedRegionMin);
        if (regionMin.y != 0) {
            continue;
        }

        BrickCoord belowCoord;
        if (!TryOffsetBrickCoord(packet.coord, 0, -1, 0, &belowCoord)) {
            continue;
        }
        const SparseBrickRequestResult requestResult = RequestBrickDetailed(belowCoord, false);
        if (requestResult != SparseBrickRequestResult::Rejected) {
            MarkResidencyClass(belowCoord, SparseResidencyClass::Collision);
            ++supportRequests;
        }
    }
    if (!wasStatsDeferred) {
        SetStatsRefreshDeferred(false);
    }
    m_physicsWorkPacketsLastFrame = stagedPackets;
    m_physicsSupportBricksRequestedLastFrame = supportRequests;
    RefreshStats();
    return stagedPackets;
}

uint32_t SparseVoxelWorld::RequeueLastPhysicsWorkPackets() {
    uint32_t requeued = 0;
    for (const SparsePhysicsWorkPacket& packet : m_physicsStagedPackets) {
        RequeuePhysicsWorkPacketNoStats(packet);
        ++requeued;
    }
    RefreshStats();
    return requeued;
}

std::vector<SparseEditDelta> SparseVoxelWorld::BuildGpuEditDeltaSnapshotForRender(
    uint32_t maxDeltas) const
{
    // Order edited bricks by recency (latest global revision first) so the
    // capped snapshot always carries the bricks the player is actively editing.
    std::vector<std::pair<uint64_t, BrickCoord>> byRecency;
    byRecency.reserve(m_edits.EditedBrickCount());
    m_edits.ForEachOverlay([&](const BrickEditOverlay& overlay) {
        if (!overlay.voxels.empty()) {
            byRecency.emplace_back(overlay.lastGlobalRevision, overlay.coord);
        }
    });
    std::sort(byRecency.begin(), byRecency.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<BrickCoord> coords;
    coords.reserve(byRecency.size());
    for (const auto& entry : byRecency) {
        coords.push_back(entry.second);
    }
    // BuildDeltaSnapshotForBricks stops at maxDeltas, so the most-recent bricks
    // (front of coords) get in first; older ones spill to the durable pool.
    return m_edits.BuildDeltaSnapshotForBricks(coords, maxDeltas);
}

std::vector<SparseEditDelta> SparseVoxelWorld::BuildGpuEditDeltaSnapshotForPhysicsWork(
    uint32_t maxDeltas) const
{
    std::vector<BrickCoord> coords;
    coords.reserve(m_physicsStagedPackets.size() * 2u);
    std::unordered_set<BrickCoord, BrickCoordHash> uniqueCoords;
    uniqueCoords.reserve(m_physicsStagedPackets.size() * 2u);

    auto addCoord = [&](const BrickCoord& coord) {
        if (uniqueCoords.insert(coord).second) {
            coords.push_back(coord);
        }
    };

    for (const SparsePhysicsWorkPacket& packet : m_physicsStagedPackets) {
        addCoord(packet.coord);
        const LocalVoxelCoord regionMin = UnpackPhysicsRegionPoint(packet.packedRegionMin);
        const LocalVoxelCoord regionMax = UnpackPhysicsRegionPoint(packet.packedRegionMax);
        if (regionMin.y == 0) {
            BrickCoord belowCoord;
            if (TryOffsetBrickCoord(packet.coord, 0, -1, 0, &belowCoord)) {
                addCoord(belowCoord);
            }
        }
        if (regionMin.x == 0) {
            BrickCoord lateralCoord;
            if (TryOffsetBrickCoord(packet.coord, -1, 0, 0, &lateralCoord)) {
                addCoord(lateralCoord);
            }
        }
        if (regionMax.x >= SPARSE_BRICK_SIZE - 1) {
            BrickCoord lateralCoord;
            if (TryOffsetBrickCoord(packet.coord, 1, 0, 0, &lateralCoord)) {
                addCoord(lateralCoord);
            }
        }
        if (regionMin.z == 0) {
            BrickCoord lateralCoord;
            if (TryOffsetBrickCoord(packet.coord, 0, 0, -1, &lateralCoord)) {
                addCoord(lateralCoord);
            }
        }
        if (regionMax.z >= SPARSE_BRICK_SIZE - 1) {
            BrickCoord lateralCoord;
            if (TryOffsetBrickCoord(packet.coord, 0, 0, 1, &lateralCoord)) {
                addCoord(lateralCoord);
            }
        }
    }

    return m_edits.BuildDeltaSnapshotForBricks(coords, maxDeltas);
}

uint32_t SparseVoxelWorld::ExecuteStagedLocalPhysics(
    uint32_t maxVoxelMoves,
    bool requestRenderBricks)
{
    m_physicsGpuProcessedProposalsLastFrame = 0;
    m_physicsGpuAppliedMovesLastFrame = 0;
    m_physicsGpuRejectedProposalsLastFrame = 0;
    m_physicsProcessedBricksLastFrame = 0;
    m_physicsMovedVoxelsLastFrame = 0;
    m_physicsSkippedVoxelsLastFrame = 0;
    if (m_physicsStagedPackets.empty()) {
        RefreshStats();
        return 0;
    }
    if (maxVoxelMoves == 0) {
        RequeueLastPhysicsWorkPackets();
        return 0;
    }

    std::unordered_map<BrickCoord, SparseRenderDirtyRegion, BrickCoordHash> touchedRenderRegions;
    auto markTouchedRenderVoxel = [&touchedRenderRegions](int32_t worldX, int32_t worldY, int32_t worldZ) {
        const BrickCoord dirtyCoord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
        const LocalVoxelCoord dirtyLocal = LocalVoxelFromWorld(worldX, worldY, worldZ);
        SparseRenderDirtyRegion pointRegion;
        pointRegion.minX = pointRegion.maxX = dirtyLocal.x;
        pointRegion.minY = pointRegion.maxY = dirtyLocal.y;
        pointRegion.minZ = pointRegion.maxZ = dirtyLocal.z;
        auto [regionIt, insertedRegion] = touchedRenderRegions.emplace(dirtyCoord, pointRegion);
        if (!insertedRegion) {
            MergeSparseRegion(regionIt->second, pointRegion);
        }
    };
    uint32_t moved = 0;
    uint32_t processed = 0;

    const uint32_t stagedPackets = static_cast<uint32_t>(m_physicsStagedPackets.size());
    for (uint32_t packetIndex = 0; packetIndex < stagedPackets; ++packetIndex) {
        const SparsePhysicsWorkPacket& packet = m_physicsStagedPackets[packetIndex];
        if (moved >= maxVoxelMoves) {
            RequeuePhysicsWorkPacketNoStats(packet);
            continue;
        }

        const BrickCoord coord = packet.coord;
        SparsePhysicsDirtyRegion region;
        const LocalVoxelCoord regionMin = UnpackPhysicsRegionPoint(packet.packedRegionMin);
        const LocalVoxelCoord regionMax = UnpackPhysicsRegionPoint(packet.packedRegionMax);
        region.minX = regionMin.x;
        region.minY = regionMin.y;
        region.minZ = regionMin.z;
        region.maxX = regionMax.x;
        region.maxY = regionMax.y;
        region.maxZ = regionMax.z;
        ++processed;

        struct Candidate {
            int32_t x;
            int32_t y;
            int32_t z;
            uint32_t voxel;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);
        m_edits.ForEachVoxelInBrick(coord, [&](uint16_t localIndex, uint32_t packedVoxel) {
            const uint8_t material = Utils::UnpackMaterial(packedVoxel);
            if (!PhysicsMaterialAllowed(material, packet.materialMask)) {
                return;
            }
            const LocalVoxelCoord local = LocalVoxelFromIndex(localIndex);
            if (local.x < region.minX || local.x > region.maxX ||
                local.y < region.minY || local.y > region.maxY ||
                local.z < region.minZ || local.z > region.maxZ) {
                ++m_physicsSkippedVoxelsLastFrame;
                return;
            }
            int32_t worldX = 0;
            int32_t worldY = 0;
            int32_t worldZ = 0;
            if (!TryWorldVoxelFromBrickLocal(coord.x, local.x, &worldX) ||
                !TryWorldVoxelFromBrickLocal(coord.y, local.y, &worldY) ||
                !TryWorldVoxelFromBrickLocal(coord.z, local.z, &worldZ)) {
                ++m_physicsSkippedVoxelsLastFrame;
                return;
            }
            candidates.push_back(Candidate{
                worldX,
                worldY,
                worldZ,
                packedVoxel});
        });

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.y < b.y;
        });

        bool requeueSourceBrick = false;
        for (const Candidate& candidate : candidates) {
            if (moved >= maxVoxelMoves) {
                requeueSourceBrick = true;
                break;
            }

            uint32_t currentVoxel = 0;
            if (!m_edits.TryGetVoxel(candidate.x, candidate.y, candidate.z, &currentVoxel) ||
                currentVoxel != candidate.voxel) {
                ++m_physicsSkippedVoxelsLastFrame;
                continue;
            }

            int32_t belowY = 0;
            if (!TryStepInt32(candidate.y, -1, &belowY)) {
                ++m_physicsSkippedVoxelsLastFrame;
                continue;
            }
            const auto sampleWorldVoxel = [this](int32_t sx, int32_t sy, int32_t sz) {
                uint32_t sampled = 0;
                if (!m_edits.TryGetVoxel(sx, sy, sz, &sampled)) {
                    sampled = m_terrain.SampleGeneratedVoxel(sx, sy, sz);
                }
                return sampled;
            };
            // Movement rule (Loop 92): FALL if air below; else granular materials
            // SLUMP into a diagonal-below air cell (piles form instead of columns);
            // else water SPREADS laterally into an air cell that itself has
            // support below (fills basins one layer, cannot creep across open
            // air, so a spill is bounded by the receiving surface).
            int32_t destX = candidate.x;
            int32_t destY = candidate.y;
            int32_t destZ = candidate.z;
            bool haveDest = false;
            if (Utils::UnpackMaterial(sampleWorldVoxel(candidate.x, belowY, candidate.z)) ==
                Utils::Material::Air) {
                destY = belowY;
                haveDest = true;
            } else {
                // Deterministic but position-varied direction order to avoid a
                // global drift bias.
                static constexpr int32_t kDirX[4] = {1, -1, 0, 0};
                static constexpr int32_t kDirZ[4] = {0, 0, 1, -1};
                const uint32_t rot = static_cast<uint32_t>(
                    (candidate.x ^ candidate.z ^ candidate.y) & 3);
                const uint8_t material = Utils::UnpackMaterial(candidate.voxel);
                for (uint32_t d = 0; d < 4u && !haveDest; ++d) {
                    const uint32_t dir = (d + rot) & 3u;
                    int32_t sideX = 0;
                    int32_t sideZ = 0;
                    if (!TryStepInt32(candidate.x, kDirX[dir], &sideX) ||
                        !TryStepInt32(candidate.z, kDirZ[dir], &sideZ)) {
                        continue;
                    }
                    const bool sideAir =
                        Utils::UnpackMaterial(sampleWorldVoxel(sideX, candidate.y, sideZ)) ==
                        Utils::Material::Air;
                    if (!sideAir) {
                        continue;
                    }
                    const bool sideBelowAir =
                        Utils::UnpackMaterial(sampleWorldVoxel(sideX, belowY, sideZ)) ==
                        Utils::Material::Air;
                    if (sideBelowAir) {
                        // Diagonal slump/flow: drop into the neighboring column.
                        destX = sideX;
                        destY = belowY;
                        destZ = sideZ;
                        haveDest = true;
                    } else if ((material == Utils::Material::Water ||
                                material == Utils::Material::Lava) &&
                               Utils::UnpackMaterial(sampleWorldVoxel(
                                   candidate.x, belowY, candidate.z)) == material) {
                        // Liquid lateral spread, ONLY while sitting on the same
                        // liquid (a stack levelling out). A lone surface voxel
                        // never moves sideways, so flat spread cannot oscillate:
                        // after levelling, every liquid voxel rests on solid or
                        // is buried, and both states are stable.
                        destX = sideX;
                        destY = candidate.y;
                        destZ = sideZ;
                        haveDest = true;
                    }
                }
            }
            if (!haveDest) {
                continue;
            }

            m_edits.SetVoxel(candidate.x, candidate.y, candidate.z, Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));
            m_edits.SetVoxel(destX, destY, destZ, candidate.voxel);
            const BrickCoord fromCoord = BrickCoord::FromWorldVoxel(candidate.x, candidate.y, candidate.z);
            const BrickCoord toCoord = BrickCoord::FromWorldVoxel(destX, destY, destZ);
            markTouchedRenderVoxel(candidate.x, candidate.y, candidate.z);
            markTouchedRenderVoxel(destX, destY, destZ);
            m_knownEmptyGeneratedBricks.erase(fromCoord);
            m_knownEmptyGeneratedBricks.erase(toCoord);
            ++moved;
            requeueSourceBrick = true;
            QueuePhysicsVoxelNoStats(destX, destY, destZ, SparsePhysicsPriority::Hot);
            // Cross-brick support wake (Loop 93): vacating (x,y,z) removes the
            // support under the voxel ABOVE it. The source-brick requeue only
            // re-examines this brick's region, so a tall stack spanning brick
            // boundaries froze at the seam (user repro: floating blob tops with
            // fallen streaks below). Wake the neighborhood like erase does.
            WakePhysicsSupportNeighborhoodNoStats(candidate.x, candidate.y, candidate.z);
        }

        if (requeueSourceBrick) {
            QueuePhysicsRegionNoStats(coord, region, SparsePhysicsPriority::Hot);
        }
    }

    uint32_t queuedUploads = 0;
    for (const auto& [touchedCoord, renderRegion] : touchedRenderRegions) {
        if (requestRenderBricks) {
            RequestBrick(touchedCoord);
        }
        MarkResidencyClass(touchedCoord, SparseResidencyClass::Edited);
        if (QueueRegeneratedUploadForExistingPage(touchedCoord, &renderRegion)) {
            ++queuedUploads;
        }
    }
    if (!touchedRenderRegions.empty()) {
        m_generationQueuePriorityDirty = true;
        MarkUploadQueueOrderDirty();
        MarkSurfaceQueueOrderDirty();
    }

    m_physicsProcessedBricksLastFrame = processed;
    m_physicsWorkPacketsLastFrame = stagedPackets;
    m_physicsMovedVoxelsLastFrame = moved;
    (void)queuedUploads;
    RefreshStats();
    return moved;
}

uint32_t SparseVoxelWorld::ApplyGpuPhysicsProposals(
    const std::vector<SparsePhysicsPacketResult>& proposals,
    uint32_t maxVoxelMoves,
    bool requestRenderBricks)
{
    m_physicsProcessedBricksLastFrame = 0;
    m_physicsMovedVoxelsLastFrame = 0;
    m_physicsSkippedVoxelsLastFrame = 0;
    m_physicsGpuProcessedProposalsLastFrame = 0;
    m_physicsGpuAppliedMovesLastFrame = 0;
    m_physicsGpuRejectedProposalsLastFrame = 0;
    auto requeueProposalSource = [this](const SparsePhysicsPacketResult& proposal) {
        const LocalVoxelCoord sourceLocal = UnpackPhysicsRegionPoint(proposal.packedSourceLocal);
        SparsePhysicsDirtyRegion deferredRegion;
        if (IsValidPhysicsLocal(sourceLocal)) {
            deferredRegion.minX = deferredRegion.maxX = sourceLocal.x;
            deferredRegion.minY = deferredRegion.maxY = sourceLocal.y;
            deferredRegion.minZ = deferredRegion.maxZ = sourceLocal.z;
        }
        QueuePhysicsRegionNoStats(proposal.coord, deferredRegion, SparsePhysicsPriority::Hot);
    };
    if (proposals.empty()) {
        RefreshStats();
        return 0;
    }
    if (maxVoxelMoves == 0) {
        for (const SparsePhysicsPacketResult& proposal : proposals) {
            if ((proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL) != 0u) {
                requeueProposalSource(proposal);
            }
        }
        RefreshStats();
        return 0;
    }

    std::unordered_map<BrickCoord, SparseRenderDirtyRegion, BrickCoordHash> touchedRenderRegions;
    auto markTouchedRenderVoxel = [&touchedRenderRegions](int32_t worldX, int32_t worldY, int32_t worldZ) {
        const BrickCoord dirtyCoord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
        const LocalVoxelCoord dirtyLocal = LocalVoxelFromWorld(worldX, worldY, worldZ);
        SparseRenderDirtyRegion pointRegion;
        pointRegion.minX = pointRegion.maxX = dirtyLocal.x;
        pointRegion.minY = pointRegion.maxY = dirtyLocal.y;
        pointRegion.minZ = pointRegion.maxZ = dirtyLocal.z;
        auto [regionIt, insertedRegion] = touchedRenderRegions.emplace(dirtyCoord, pointRegion);
        if (!insertedRegion) {
            MergeSparseRegion(regionIt->second, pointRegion);
        }
    };
    uint32_t moved = 0;
    uint32_t processed = 0;
    std::unordered_set<SparseWorldVoxelKey, SparseWorldVoxelKeyHash> claimedVoxels;
    claimedVoxels.reserve(proposals.size() * 2u);
    for (const SparsePhysicsPacketResult& proposal : proposals) {
        if ((proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL) == 0u) {
            continue;
        }
        if (moved >= maxVoxelMoves) {
            requeueProposalSource(proposal);
            continue;
        }
        ++m_physicsGpuProcessedProposalsLastFrame;

        if (!GpuPhysicsProposalStatusWellFormed(proposal.status)) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }

        if (proposal.generation == 0u) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }

        const bool hasExpectedPage =
            proposal.expectedPageIndex != INVALID_BRICK_PAGE &&
            proposal.expectedPageGeneration != 0u;
        const bool statusHasExpectedPage =
            (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE) != 0u;
        const bool statusPageMatch =
            (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH) != 0u;
        const bool statusPageStale =
            (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE) != 0u;
        const bool claimsExpectedPageValidation =
            statusHasExpectedPage || statusPageMatch || statusPageStale;
        if ((claimsExpectedPageValidation && !hasExpectedPage) ||
            ((statusPageMatch || statusPageStale) && !statusHasExpectedPage) ||
            (statusPageMatch && statusPageStale)) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }
        if (hasExpectedPage) {
            uint32_t currentPageIndex = INVALID_BRICK_PAGE;
            if (!m_pool.PageTable().TryLookupExactGeneration(
                    proposal.coord,
                    proposal.expectedPageGeneration,
                    &currentPageIndex,
                    nullptr) ||
                currentPageIndex != proposal.expectedPageIndex ||
                (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE) != 0u ||
                ((proposal.status & SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE) != 0u &&
                 (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH) == 0u)) {
                ++m_physicsSkippedVoxelsLastFrame;
                ++m_physicsGpuRejectedProposalsLastFrame;
                requeueProposalSource(proposal);
                continue;
            }
        }

        const LocalVoxelCoord sourceLocal = UnpackPhysicsRegionPoint(proposal.packedSourceLocal);
        const LocalVoxelCoord destinationLocal =
            UnpackPhysicsRegionPoint(proposal.packedDestinationLocal);
        if (!IsValidPhysicsLocal(sourceLocal) || !IsValidPhysicsLocal(destinationLocal)) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }
        int32_t sourceX = 0;
        int32_t sourceY = 0;
        int32_t sourceZ = 0;
        int32_t destinationX = 0;
        int32_t destinationY = 0;
        int32_t destinationZ = 0;
        if (!TryWorldVoxelFromBrickLocal(proposal.coord.x, sourceLocal.x, &sourceX) ||
            !TryWorldVoxelFromBrickLocal(proposal.coord.y, sourceLocal.y, &sourceY) ||
            !TryWorldVoxelFromBrickLocal(proposal.coord.z, sourceLocal.z, &sourceZ) ||
            !TryWorldVoxelFromBrickLocal(proposal.destinationCoord.x, destinationLocal.x, &destinationX) ||
            !TryWorldVoxelFromBrickLocal(proposal.destinationCoord.y, destinationLocal.y, &destinationY) ||
            !TryWorldVoxelFromBrickLocal(proposal.destinationCoord.z, destinationLocal.z, &destinationZ)) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }
        const SparseWorldVoxelKey sourceKey{sourceX, sourceY, sourceZ};
        const SparseWorldVoxelKey destinationKey{destinationX, destinationY, destinationZ};
        if (claimedVoxels.find(sourceKey) != claimedVoxels.end() ||
            claimedVoxels.find(destinationKey) != claimedVoxels.end()) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }
        const bool proposalUsedEditDelta =
            (proposal.status & SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT) != 0u;
        if (proposalUsedEditDelta) {
            const uint32_t currentSourceRevision = m_edits.GetOverlayRevision(proposal.coord);
            const uint32_t currentDestinationRevision =
                m_edits.GetOverlayRevision(proposal.destinationCoord);
            if (!GpuPhysicsEditRevisionsMatch(
                    proposalUsedEditDelta,
                    proposal.sourceRevision,
                    proposal.destinationRevision,
                    currentSourceRevision,
                    currentDestinationRevision)) {
                ++m_physicsSkippedVoxelsLastFrame;
                ++m_physicsGpuRejectedProposalsLastFrame;
                requeueProposalSource(proposal);
                continue;
            }
        }
        ++processed;

        uint32_t currentSourceVoxel = 0;
        if (!m_edits.TryGetVoxel(sourceX, sourceY, sourceZ, &currentSourceVoxel)) {
            currentSourceVoxel = m_terrain.SampleGeneratedVoxel(sourceX, sourceY, sourceZ);
        }
        if (currentSourceVoxel != proposal.sourceVoxel ||
            !PhysicsMaterialAllowed(Utils::UnpackMaterial(currentSourceVoxel), proposal.materialMask)) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }

        uint32_t currentDestinationVoxel = 0;
        if (!m_edits.TryGetVoxel(destinationX, destinationY, destinationZ, &currentDestinationVoxel)) {
            currentDestinationVoxel =
                m_terrain.SampleGeneratedVoxel(destinationX, destinationY, destinationZ);
        }
        if (currentDestinationVoxel != proposal.destinationVoxel ||
            Utils::UnpackMaterial(currentDestinationVoxel) != Utils::Material::Air) {
            ++m_physicsSkippedVoxelsLastFrame;
            ++m_physicsGpuRejectedProposalsLastFrame;
            requeueProposalSource(proposal);
            continue;
        }

        m_edits.SetVoxel(sourceX, sourceY, sourceZ, Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));
        m_edits.SetVoxel(destinationX, destinationY, destinationZ, proposal.sourceVoxel);
        markTouchedRenderVoxel(sourceX, sourceY, sourceZ);
        markTouchedRenderVoxel(destinationX, destinationY, destinationZ);
        claimedVoxels.insert(sourceKey);
        claimedVoxels.insert(destinationKey);
        m_knownEmptyGeneratedBricks.erase(proposal.coord);
        m_knownEmptyGeneratedBricks.erase(proposal.destinationCoord);
        QueuePhysicsVoxelNoStats(destinationX, destinationY, destinationZ, SparsePhysicsPriority::Hot);
        ++moved;
    }

    uint32_t queuedUploads = 0;
    for (const auto& [touchedCoord, renderRegion] : touchedRenderRegions) {
        if (requestRenderBricks) {
            RequestBrick(touchedCoord);
        }
        MarkResidencyClass(touchedCoord, SparseResidencyClass::Edited);
        if (QueueRegeneratedUploadForExistingPage(touchedCoord, &renderRegion)) {
            ++queuedUploads;
        }
    }
    if (!touchedRenderRegions.empty()) {
        m_generationQueuePriorityDirty = true;
        MarkUploadQueueOrderDirty();
        MarkSurfaceQueueOrderDirty();
    }

    m_physicsProcessedBricksLastFrame = processed;
    m_physicsMovedVoxelsLastFrame = moved;
    m_physicsGpuAppliedMovesLastFrame = moved;
    (void)queuedUploads;
    RefreshStats();
    return moved;
}

void SparseVoxelWorld::SetEditedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
    m_edits.SetVoxel(worldX, worldY, worldZ, packedVoxel);
    const BrickCoord coord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    m_knownEmptyGeneratedBricks.erase(coord);
    MarkResidencyClass(coord, SparseResidencyClass::Edited);
    QueuePhysicsVoxelNoStats(worldX, worldY, worldZ, SparsePhysicsPriority::Hot);
    if (Utils::UnpackMaterial(packedVoxel) == Utils::Material::Air) {
        WakePhysicsSupportNeighborhoodNoStats(worldX, worldY, worldZ);
    }
    const LocalVoxelCoord local = LocalVoxelFromWorld(worldX, worldY, worldZ);
    SparseRenderDirtyRegion renderRegion;
    renderRegion.minX = renderRegion.maxX = local.x;
    renderRegion.minY = renderRegion.maxY = local.y;
    renderRegion.minZ = renderRegion.maxZ = local.z;
    QueueRegeneratedUploadForExistingPage(coord, &renderRegion);
    RefreshStats();
}

uint32_t SparseVoxelWorld::ApplyEditedVoxels(const std::vector<SparseBrushFeedbackRecord>& records) {
    if (records.empty()) {
        return 0;
    }

    std::unordered_map<BrickCoord, SparseRenderDirtyRegion, BrickCoordHash> touchedRenderRegions;
    std::unordered_map<BrickCoord, SparsePhysicsDirtyRegion, BrickCoordHash> touchedPhysicsRegions;
    uint32_t evaluated = 0;
    uint32_t applied = 0;
    for (const SparseBrushFeedbackRecord& record : records) {
        if (IsSparseBrushFeedbackMissingResident(record)) {
            continue;
        }
        ++evaluated;

        uint32_t currentVoxel = 0;
        if (!m_edits.TryGetVoxel(record.worldX, record.worldY, record.worldZ, &currentVoxel)) {
            currentVoxel = m_terrain.SampleGeneratedVoxel(record.worldX, record.worldY, record.worldZ);
        }
        if (BrushFeedbackVoxelAlreadyApplied(currentVoxel, record.voxel)) {
            continue;
        }

        m_edits.SetVoxel(record.worldX, record.worldY, record.worldZ, record.voxel);
        const BrickCoord coord = BrickCoord::FromWorldVoxel(record.worldX, record.worldY, record.worldZ);
        const LocalVoxelCoord local = LocalVoxelFromWorld(record.worldX, record.worldY, record.worldZ);
        m_knownEmptyGeneratedBricks.erase(coord);

        SparseRenderDirtyRegion renderRegion;
        renderRegion.minX = renderRegion.maxX = local.x;
        renderRegion.minY = renderRegion.maxY = local.y;
        renderRegion.minZ = renderRegion.maxZ = local.z;
        auto [renderIt, insertedRender] = touchedRenderRegions.emplace(coord, renderRegion);
        if (!insertedRender) {
            MergeSparseRegion(renderIt->second, renderRegion);
        }

        SparsePhysicsDirtyRegion physicsRegion;
        physicsRegion.minX = physicsRegion.maxX = local.x;
        physicsRegion.minY = physicsRegion.maxY = local.y;
        physicsRegion.minZ = physicsRegion.maxZ = local.z;
        auto [physicsIt, insertedPhysics] = touchedPhysicsRegions.emplace(coord, physicsRegion);
        if (!insertedPhysics) {
            MergeSparseRegion(physicsIt->second, physicsRegion);
        }

        if (Utils::UnpackMaterial(record.voxel) == Utils::Material::Air) {
            WakePhysicsSupportNeighborhoodNoStats(record.worldX, record.worldY, record.worldZ);
        }
        ++applied;
    }

    m_stats.brushVoxelsEvaluatedLastStroke = evaluated;
    m_stats.brushVoxelsEditedLastStroke = applied;
    if (applied == 0) {
        RefreshStats();
        return 0;
    }

    uint32_t queued = 0;
    for (const auto& [coord, renderRegion] : touchedRenderRegions) {
        auto physicsIt = touchedPhysicsRegions.find(coord);
        if (physicsIt != touchedPhysicsRegions.end()) {
            QueuePhysicsRegionNoStats(coord, physicsIt->second, SparsePhysicsPriority::Hot);
        } else {
            QueuePhysicsCandidateNoStats(coord);
        }
        if (QueueRegeneratedUploadForExistingPage(coord, &renderRegion)) {
            ++queued;
        }
    }

    m_stats.brushBricksTouchedLastStroke = static_cast<uint32_t>(touchedRenderRegions.size());
    m_stats.brushBricksQueuedLastStroke = queued;
    RefreshStats();
    return applied;
}

bool SparseVoxelWorld::SaveEditsToFile(const std::filesystem::path& path) {
    return m_edits.SaveToFile(path);
}

bool SparseVoxelWorld::LoadEditsFromFile(const std::filesystem::path& path, bool requestRenderBricks) {
    if (!m_edits.LoadFromFile(path)) {
        return false;
    }

    m_edits.ForEachOverlay([&](const BrickEditOverlay& overlay) {
        if (overlay.voxels.empty()) {
            return;
        }

        const BrickCoord coord = overlay.coord;
        m_knownEmptyGeneratedBricks.erase(coord);
        if (requestRenderBricks) {
            RequestBrickDetailed(coord, false);
        }
        MarkResidencyClass(coord, SparseResidencyClass::Edited);

        SparseRenderDirtyRegion fullRenderRegion;
        QueueRenderDirtyRegionNoStats(coord, fullRenderRegion);
        QueueSurfaceDirtyRegionNoStats(coord, fullRenderRegion);
        QueuePhysicsRegionNoStats(
            coord,
            SparsePhysicsDirtyRegion{},
            SparsePhysicsPriority::Hot);

        auto generatedIt = m_generated.find(coord);
        if (generatedIt != m_generated.end()) {
            m_edits.ApplyToGeneratedBrick(generatedIt->second);
        }
        QueueRegeneratedUploadForExistingPage(coord, &fullRenderRegion);
    });

    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    MarkSurfaceQueueOrderDirty();
    RefreshStats();
    return true;
}

uint32_t SparseVoxelWorld::ApplyBrushEdit(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal,
    bool requestRenderBricks,
    std::vector<SparseEditDelta>* outDeltas)
{
    return EvaluateBrushEdit(
        worldPositionX,
        worldPositionY,
        worldPositionZ,
        radius,
        material,
        mode,
        shape,
        strength,
        seed,
        hitNormalX,
        hitNormalY,
        hitNormalZ,
        hasHitNormal,
        true,
        requestRenderBricks,
        outDeltas);
}

uint32_t SparseVoxelWorld::PreviewBrushEdit(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal,
    std::vector<SparseEditDelta>* outDeltas)
{
    return EvaluateBrushEdit(
        worldPositionX,
        worldPositionY,
        worldPositionZ,
        radius,
        material,
        mode,
        shape,
        strength,
        seed,
        hitNormalX,
        hitNormalY,
        hitNormalZ,
        hasHitNormal,
        false,
        false,
        outDeltas);
}

uint32_t SparseVoxelWorld::EvaluateBrushEdit(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal,
    bool commit,
    bool requestRenderBricks,
    std::vector<SparseEditDelta>* outDeltas)
{
    HEIGHTAT_SCOPE("EvaluateBrushEdit");
    if (commit) {
        m_stats.brushVoxelsEvaluatedLastStroke = 0;
        m_stats.brushVoxelsEditedLastStroke = 0;
        m_stats.brushBricksTouchedLastStroke = 0;
        m_stats.brushBricksQueuedLastStroke = 0;
    }

    SparseBrushVoxelBounds brushBounds;
    if (!TryBuildSparseBrushVoxelBounds(
            worldPositionX,
            worldPositionY,
            worldPositionZ,
            radius,
            strength,
            &brushBounds)) {
        if (commit) {
            RefreshStats();
        }
        return 0;
    }

    std::unordered_set<BrickCoord, BrickCoordHash> touchedBricks;
    std::unordered_map<BrickCoord, SparsePhysicsDirtyRegion, BrickCoordHash> touchedPhysicsRegions;
    std::unordered_map<BrickCoord, bool, BrickCoordHash> touchedSurfaceGeometryDirty;
    uint32_t edited = 0;
    uint32_t evaluated = 0;

    const float normalX = static_cast<float>(hitNormalX);
    const float normalY = static_cast<float>(hitNormalY);
    const float normalZ = static_cast<float>(hitNormalZ);

    // The generated-terrain sample needs HeightAt/SurfaceReliefAt, expensive
    // multi-octave noise that depends ONLY on (x,z). The brush loops x,y,z so it is
    // computed at most once per column via the FRAME-PERSISTENT member column cache
    // (CachedTerrainHeightAt/ReliefAt) below. That member cache is shared across the
    // brush's ~9.5 preview/apply calls per frame (a per-call-local cache recomputed
    // the same columns every call - the dominant CPU edit cost).
    for (int32_t z = brushBounds.startZ; z < brushBounds.endZ; ++z) {
        for (int32_t y = brushBounds.startY; y < brushBounds.endY; ++y) {
            for (int32_t x = brushBounds.startX; x < brushBounds.endX; ++x) {
                ++evaluated;

                const float sdf = BrushSdf(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f,
                    static_cast<float>(z) + 0.5f,
                    worldPositionX,
                    worldPositionY,
                    worldPositionZ,
                    brushBounds.radius,
                    shape);
                if (sdf > 0.5f || y <= TERRAIN_MIN_Y + 5) {
                    continue;
                }

                const uint8_t variant = HashVoxelVariant(x, y, z, seed);
                if (brushBounds.strength < 1.0f && sdf > -0.5f) {
                    const float edgeFactor = std::clamp(1.0f - sdf / 0.5f, 0.0f, 1.0f);
                    const float probability = edgeFactor * brushBounds.strength;
                    if ((static_cast<float>(variant) / 255.0f) > probability) {
                        continue;
                    }
                }

                if (hasHitNormal) {
                    const float faceSide =
                        (static_cast<float>(x) + 0.5f - worldPositionX) * normalX +
                        (static_cast<float>(y) + 0.5f - worldPositionY) * normalY +
                        (static_cast<float>(z) + 0.5f - worldPositionZ) * normalZ;
                    if (mode == SPARSE_BRUSH_MODE_PAINT && faceSide < -0.35f) {
                        continue;
                    }
                    if ((mode == SPARSE_BRUSH_MODE_ERASE || mode == SPARSE_BRUSH_MODE_REPLACE) && faceSide > 0.65f) {
                        continue;
                    }
                }

                uint32_t currentVoxel = 0;
                if (!m_edits.TryGetVoxel(x, y, z, &currentVoxel)) {
                    const float colHeight = CachedTerrainHeightAt(x, z);
                    const float colRelief = CachedTerrainReliefAt(x, z, 4);
                    currentVoxel =
                        m_terrain.SampleGeneratedVoxelWithColumn(x, y, z, colHeight, colRelief);
                }

                const uint8_t currentMaterial = Utils::UnpackMaterial(currentVoxel);
                if (currentMaterial == Utils::Material::Bedrock) {
                    continue;
                }

                uint32_t newVoxel = currentVoxel;
                if (mode == SPARSE_BRUSH_MODE_PAINT) {
                    if (currentMaterial != Utils::Material::Air) {
                        continue;
                    }
                    newVoxel = Utils::PackVoxel(
                        static_cast<uint8_t>(material & 0xFFu),
                        variant,
                        0,
                        Utils::StateFlags::IsStatic);
                } else if (mode == SPARSE_BRUSH_MODE_ERASE) {
                    if (currentMaterial == Utils::Material::Air) {
                        continue;
                    }
                    newVoxel = Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
                } else if (mode == SPARSE_BRUSH_MODE_REPLACE) {
                    if (currentMaterial == Utils::Material::Air) {
                        continue;
                    }
                    newVoxel = Utils::PackVoxel(
                        static_cast<uint8_t>(material & 0xFFu),
                        variant,
                        0,
                        Utils::StateFlags::IsStatic);
                } else if (mode == SPARSE_BRUSH_MODE_FILL) {
                    newVoxel = Utils::PackVoxel(
                        static_cast<uint8_t>(material & 0xFFu),
                        variant,
                        0,
                        Utils::StateFlags::IsStatic);
                } else {
                    continue;
                }

                if (BrushFeedbackVoxelAlreadyApplied(currentVoxel, newVoxel)) {
                    continue;
                }

                const BrickCoord editCoord = BrickCoord::FromWorldVoxel(x, y, z);
                const LocalVoxelCoord editLocal = LocalVoxelFromWorld(x, y, z);
                const bool surfaceGeometryChanged =
                    SurfaceOccupancyClassForVoxel(currentVoxel) !=
                    SurfaceOccupancyClassForVoxel(newVoxel);
                if (commit) {
                    m_edits.SetVoxel(x, y, z, newVoxel);
                }
                if (outDeltas) {
                    const uint32_t currentRevision = m_edits.GetOverlayRevision(editCoord);
                    const uint32_t deltaRevision =
                        commit || currentRevision != std::numeric_limits<uint32_t>::max()
                            ? currentRevision + (commit ? 0u : 1u)
                            : 1u;
                    outDeltas->push_back({
                        editCoord,
                        PackSparseEditLocal(editLocal),
                        newVoxel,
                        deltaRevision
                    });
                }
                if (commit && Utils::UnpackMaterial(newVoxel) == Utils::Material::Air) {
                    WakePhysicsSupportNeighborhoodNoStats(x, y, z);
                }
                touchedBricks.insert(editCoord);
                auto [surfaceDirtyIt, insertedSurfaceDirty] =
                    touchedSurfaceGeometryDirty.emplace(editCoord, surfaceGeometryChanged);
                if (!insertedSurfaceDirty) {
                    surfaceDirtyIt->second = surfaceDirtyIt->second || surfaceGeometryChanged;
                }
                SparsePhysicsDirtyRegion pointRegion;
                pointRegion.minX = pointRegion.maxX = editLocal.x;
                pointRegion.minY = pointRegion.maxY = editLocal.y;
                pointRegion.minZ = pointRegion.maxZ = editLocal.z;
                auto [regionIt, insertedRegion] = touchedPhysicsRegions.emplace(editCoord, pointRegion);
                if (!insertedRegion) {
                    MergeSparseRegion(regionIt->second, pointRegion);
                }
                ++edited;
            }
        }
    }

    uint32_t queued = 0;
    if (commit && requestRenderBricks &&
        mode == SPARSE_BRUSH_MODE_ERASE && edited != 0u) {
        // An erase EXPOSES bricks it does not touch: fully-interior neighbors
        // (never surface-bearing, never resident) become the new pit floor/walls.
        // Nothing requested them — the near raymarch reads their non-resident
        // pages as air and tunnels to sky (the fresh-dab blue/black void) until
        // per-frame ray feedback discovers them layer by layer. We know the dab
        // volume right here: request every brick overlapping the dab bounds plus
        // a one-voxel shell (<=~27 coords, already-resident ones short-circuit).
        const BrickCoord shellMin = BrickCoord::FromWorldVoxel(
            brushBounds.startX - 1, brushBounds.startY - 1, brushBounds.startZ - 1);
        const BrickCoord shellMax = BrickCoord::FromWorldVoxel(
            brushBounds.endX, brushBounds.endY, brushBounds.endZ);
        for (int32_t bz = shellMin.z; bz <= shellMax.z; ++bz) {
            for (int32_t by = shellMin.y; by <= shellMax.y; ++by) {
                for (int32_t bx = shellMin.x; bx <= shellMax.x; ++bx) {
                    const BrickCoord coord{bx, by, bz};
                    if (touchedBricks.find(coord) == touchedBricks.end()) {
                        RequestBrick(coord);
                    }
                }
            }
        }
    }
    if (commit) {
        for (const BrickCoord& coord : touchedBricks) {
            if (requestRenderBricks) {
                RequestBrick(coord);
            }
            m_knownEmptyGeneratedBricks.erase(coord);
            MarkResidencyClass(coord, SparseResidencyClass::Edited);
            auto regionIt = touchedPhysicsRegions.find(coord);
            if (regionIt != touchedPhysicsRegions.end()) {
                QueuePhysicsRegionNoStats(coord, regionIt->second, SparsePhysicsPriority::Hot);
            } else {
                QueuePhysicsCandidateNoStats(coord);
            }
            SparseRenderDirtyRegion renderRegion;
            if (regionIt != touchedPhysicsRegions.end()) {
                renderRegion.minX = regionIt->second.minX;
                renderRegion.minY = regionIt->second.minY;
                renderRegion.minZ = regionIt->second.minZ;
                renderRegion.maxX = regionIt->second.maxX;
                renderRegion.maxY = regionIt->second.maxY;
                renderRegion.maxZ = regionIt->second.maxZ;
            }
            const auto surfaceDirtyIt = touchedSurfaceGeometryDirty.find(coord);
            const bool surfaceGeometryDirty =
                surfaceDirtyIt == touchedSurfaceGeometryDirty.end() || surfaceDirtyIt->second;
            if (QueueRegeneratedUploadForExistingPage(coord, &renderRegion, surfaceGeometryDirty)) {
                ++queued;
            }
        }
    }

    if (commit) {
        m_stats.brushVoxelsEvaluatedLastStroke = evaluated;
        m_stats.brushVoxelsEditedLastStroke = edited;
        m_stats.brushBricksTouchedLastStroke = static_cast<uint32_t>(touchedBricks.size());
        m_stats.brushBricksQueuedLastStroke = queued;
        RefreshStats();
    }
    return edited;
}

CollisionSampleStatus SparseVoxelWorld::SampleCollisionStatus(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ) const
{
    SparseCollisionQuery query(m_terrain, &m_edits);
    return query.Sample(worldX, worldY, worldZ).status;
}

SparseCollisionVolumeResult SparseVoxelWorld::TestCollisionAabb(
    const SparseCollisionAabb& aabb,
    bool liquidsBlock) const
{
    SparseCollisionQuery query(m_terrain, &m_edits);
    return query.TestAabb(aabb, liquidsBlock);
}

SparseCollisionSweepResult SparseVoxelWorld::SweepCollisionAabb(
    const SparseCollisionAabb& aabb,
    float deltaX,
    float deltaY,
    float deltaZ,
    uint32_t steps,
    bool liquidsBlock) const
{
    SparseCollisionQuery query(m_terrain, &m_edits);
    return query.SweepAabb(aabb, deltaX, deltaY, deltaZ, steps, liquidsBlock);
}

SparseCollisionSupportResult SparseVoxelWorld::FindCollisionSupportBelow(
    const SparseCollisionAabb& footprintAabb,
    float maxDrop,
    bool liquidsSupport) const
{
    SparseCollisionQuery query(m_terrain, &m_edits);
    return query.FindSupportBelow(footprintAabb, maxDrop, liquidsSupport);
}

SparseRaycastHit SparseVoxelWorld::Raycast(
    float originX,
    float originY,
    float originZ,
    float dirX,
    float dirY,
    float dirZ,
    float maxDistance) const
{
    SparseRaycastHit hit;
    if (!std::isfinite(originX) ||
        !std::isfinite(originY) ||
        !std::isfinite(originZ) ||
        !std::isfinite(dirX) ||
        !std::isfinite(dirY) ||
        !std::isfinite(dirZ) ||
        !std::isfinite(maxDistance) ||
        maxDistance <= 0.0f) {
        return hit;
    }

    const double dirLength = std::sqrt(
        static_cast<double>(dirX) * static_cast<double>(dirX) +
        static_cast<double>(dirY) * static_cast<double>(dirY) +
        static_cast<double>(dirZ) * static_cast<double>(dirZ));
    if (!std::isfinite(dirLength) || dirLength <= 0.00001) {
        return hit;
    }
    dirX = static_cast<float>(static_cast<double>(dirX) / dirLength);
    dirY = static_cast<float>(static_cast<double>(dirY) / dirLength);
    dirZ = static_cast<float>(static_cast<double>(dirZ) / dirLength);
    const float rayMaxDistance = std::min(maxDistance, kMaxSparseRaycastDistance);

    int32_t voxelX = 0;
    int32_t voxelY = 0;
    int32_t voxelZ = 0;
    if (!TryFloorToInt32(originX, &voxelX) ||
        !TryFloorToInt32(originY, &voxelY) ||
        !TryFloorToInt32(originZ, &voxelZ)) {
        return hit;
    }

    const int32_t stepX = dirX > 0.0f ? 1 : (dirX < 0.0f ? -1 : 0);
    const int32_t stepY = dirY > 0.0f ? 1 : (dirY < 0.0f ? -1 : 0);
    const int32_t stepZ = dirZ > 0.0f ? 1 : (dirZ < 0.0f ? -1 : 0);

    const double inf = std::numeric_limits<double>::infinity();
    const double tDeltaX = stepX != 0 ? std::abs(1.0 / static_cast<double>(dirX)) : inf;
    const double tDeltaY = stepY != 0 ? std::abs(1.0 / static_cast<double>(dirY)) : inf;
    const double tDeltaZ = stepZ != 0 ? std::abs(1.0 / static_cast<double>(dirZ)) : inf;

    auto firstBoundaryT = [](float origin, float dir, int32_t voxel, int32_t step) {
        if (step > 0) {
            return (static_cast<double>(voxel) + 1.0 - static_cast<double>(origin)) /
                static_cast<double>(dir);
        }
        if (step < 0) {
            return (static_cast<double>(origin) - static_cast<double>(voxel)) /
                -static_cast<double>(dir);
        }
        return std::numeric_limits<double>::infinity();
    };

    double tMaxX = firstBoundaryT(originX, dirX, voxelX, stepX);
    double tMaxY = firstBoundaryT(originY, dirY, voxelY, stepY);
    double tMaxZ = firstBoundaryT(originZ, dirZ, voxelZ, stepZ);
    tMaxX = std::max(tMaxX, 0.0);
    tMaxY = std::max(tMaxY, 0.0);
    tMaxZ = std::max(tMaxZ, 0.0);

    SparseCollisionQuery query(m_terrain, &m_edits);
    double traveled = 0.0;
    int32_t normalX = 0;
    int32_t normalY = 0;
    int32_t normalZ = 0;
    const uint32_t maxSteps = std::min<uint32_t>(
        kMaxSparseRaycastSteps,
        static_cast<uint32_t>(std::ceil(static_cast<double>(rayMaxDistance) * 3.0)) + 16u);
    for (uint32_t step = 0; step < maxSteps && traveled <= static_cast<double>(rayMaxDistance); ++step) {
        const CollisionSample sample = query.Sample(voxelX, voxelY, voxelZ);
        if (sample.status == CollisionSampleStatus::KnownSolid ||
            sample.status == CollisionSampleStatus::KnownLiquid ||
            sample.status == CollisionSampleStatus::UnknownBlocked) {
            hit.hit = true;
            hit.voxelX = voxelX;
            hit.voxelY = voxelY;
            hit.voxelZ = voxelZ;
            hit.normalX = normalX;
            hit.normalY = normalY;
            hit.normalZ = normalZ;
            hit.distance = static_cast<float>(std::min(traveled, static_cast<double>(rayMaxDistance)));
            hit.voxel = sample.voxel;
            hit.fromEdit = sample.fromEdit;
            return hit;
        }

        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            if (!TryStepInt32(voxelX, stepX, &voxelX)) {
                return hit;
            }
            traveled = tMaxX;
            tMaxX += tDeltaX;
            normalX = -stepX;
            normalY = 0;
            normalZ = 0;
        } else if (tMaxY <= tMaxZ) {
            if (!TryStepInt32(voxelY, stepY, &voxelY)) {
                return hit;
            }
            traveled = tMaxY;
            tMaxY += tDeltaY;
            normalX = 0;
            normalY = -stepY;
            normalZ = 0;
        } else {
            if (!TryStepInt32(voxelZ, stepZ, &voxelZ)) {
                return hit;
            }
            traveled = tMaxZ;
            tMaxZ += tDeltaZ;
            normalX = 0;
            normalY = 0;
            normalZ = -stepZ;
        }
    }

    return hit;
}

void SparseVoxelWorld::RefreshStats() {
    if (m_statsRefreshDeferred) {
        m_statsRefreshPending = true;
        return;
    }
    m_statsRefreshPending = false;
    // Cheap fields callers may read right after a mutation stay fresh every call.
    m_stats.requestedBricks = m_pool.ResidentCount();
    m_stats.generationQueuedBricks = static_cast<uint32_t>(m_generationQueue.size());
    m_stats.uploadQueuedBricks = static_cast<uint32_t>(m_uploadQueue.size());
    m_stats.generatedBricks = static_cast<uint32_t>(m_generated.size());
    m_stats.surfaceBricksExtractedLastFrame = m_surfaceBricksExtractedLastFrame;
    m_stats.surfaceSpeculativeBricksExtractedLastFrame =
        m_surfaceSpeculativeBricksExtractedLastFrame;
    m_stats.surfaceVisibleBricksExtractedLastFrame =
        m_surfaceVisibleBricksExtractedLastFrame;
    m_stats.surfaceCollisionBricksExtractedLastFrame =
        m_surfaceCollisionBricksExtractedLastFrame;
    m_stats.surfaceEditedBricksExtractedLastFrame =
        m_surfaceEditedBricksExtractedLastFrame;
    m_stats.surfaceClassValueSortCallsLastFrame =
        m_surfaceClassValueSortCallsLastFrame;
    m_stats.surfaceClassValueSortCacheHitsLastFrame =
        m_surfaceClassValueSortCacheHitsLastFrame;
    m_stats.surfaceStrictTimeBudgetUnsortedPopsLastFrame =
        m_surfaceStrictTimeBudgetUnsortedPopsLastFrame;
    m_stats.surfaceInlineExtractionBricksLastFrame =
        m_surfaceInlineExtractionBricksLastFrame;
    m_stats.surfaceInlineExtractionMsLastFrame =
        m_surfaceInlineExtractionMsLastFrame;
    m_stats.parallelSurfaceExtractionActive =
        m_parallelSurfaceExtractionBricksLastFrame != 0u ? 1u : 0u;
    m_stats.parallelSurfaceExtractionBricksLastFrame =
        m_parallelSurfaceExtractionBricksLastFrame;
    m_stats.parallelSurfaceExtractionWorkersLastFrame =
        m_parallelSurfaceExtractionWorkersLastFrame;
    m_stats.parallelSurfaceExtractionWallMsLastFrame =
        m_parallelSurfaceExtractionWallMsLastFrame;
    m_stats.surfaceExtractionWaitMsLastFrame =
        m_surfaceExtractionWaitMsLastFrame;
    m_stats.asyncSurfaceExtractionEnabled =
        m_config.asyncSurfaceExtraction ? 1u : 0u;
    m_stats.asyncSurfaceExtractionEnqueuedLastFrame =
        m_asyncSurfaceExtractionEnqueuedLastFrame;
    m_stats.asyncSurfaceExtractionAppliedLastFrame =
        m_asyncSurfaceExtractionAppliedLastFrame;
    m_stats.asyncSurfaceExtractionDiscardedLastFrame =
        m_asyncSurfaceExtractionDiscardedLastFrame;
    m_stats.asyncSurfaceExtractionRequeuedLastFrame =
        m_asyncSurfaceExtractionRequeuedLastFrame;
    m_stats.asyncSurfaceExtractionWorkerMsLastFrame =
        m_asyncSurfaceExtractionWorkerMsLastFrame;
    m_stats.asyncSurfaceExtractionEnqueueMsLastFrame =
        m_asyncSurfaceExtractionEnqueueMsLastFrame;
    m_stats.asyncSurfaceExtractionRejectedLastFrame =
        m_asyncSurfaceExtractionRejectedLastFrame;
    // The rest is telemetry-grade aggregation fired ~84x/frame; collapse it to once per
    // stats frame (engine opt-in; OFF for tests so every call yields a full snapshot).
    if (m_statsRefreshOncePerFrame && m_lastFullStatsFrame == m_statsFrameHint) {
        return;
    }
    m_lastFullStatsFrame = m_statsFrameHint;
    if (m_queueClassStatsDirty ||
        m_cachedGenerationQueueSize != static_cast<uint32_t>(m_generationQueue.size()) ||
        m_cachedUploadQueueSize != static_cast<uint32_t>(m_uploadQueue.size()) ||
        m_cachedSurfacePendingSize != static_cast<uint32_t>(m_pendingSurfaceBricks.size())) {
        RebuildQueueClassStats();
    }

    m_stats.requestedBricks = m_pool.ResidentCount();
    m_stats.generationQueuedBricks = static_cast<uint32_t>(m_generationQueue.size());
    m_stats.generationQueuedSpeculativeBricks = m_generationQueueClassCounts.speculative;
    m_stats.generationQueuedVisibleBricks = m_generationQueueClassCounts.visible;
    m_stats.generationQueuedCollisionBricks = m_generationQueueClassCounts.collision;
    m_stats.generationQueuedEditedBricks = m_generationQueueClassCounts.edited;
    m_stats.generationQueuedCacheLaneBricks = m_generationQueueLaneCounts.cache;
    m_stats.generationQueuedPrefetchLaneBricks = m_generationQueueLaneCounts.prefetch;
    m_stats.generationQueuedRepairLaneBricks = m_generationQueueLaneCounts.repair;
    m_stats.generationQueuedVisibleLaneBricks = m_generationQueueLaneCounts.visible;
    m_stats.generationQueuedPublicCriticalLaneBricks = m_generationQueueLaneCounts.publicCritical;
    m_stats.generationPendingOwnershipPublicCritical = 0u;
    m_stats.generationPendingOwnershipSampledVisible = 0u;
    m_stats.generationPendingOwnershipHiddenRepair = 0u;
    m_stats.generationPendingOwnershipCache = 0u;
    m_stats.generationPendingOwnershipPrefetch = 0u;
    m_stats.generationPendingOwnershipFallbackValid = 0u;
    m_stats.generationPendingOwnershipUnknownCritical = 0u;
    m_stats.generatedBricks = static_cast<uint32_t>(m_generated.size());
    m_stats.generatedSpeculativeBricksLastFrame = m_generatedSpeculativeBricksLastFrame;
    m_stats.generatedVisibleBricksLastFrame = m_generatedVisibleBricksLastFrame;
    m_stats.generatedCollisionBricksLastFrame = m_generatedCollisionBricksLastFrame;
    m_stats.generatedEditedBricksLastFrame = m_generatedEditedBricksLastFrame;
    m_stats.generatedCacheLaneBricksLastFrame = m_generatedCacheLaneBricksLastFrame;
    m_stats.generatedPrefetchLaneBricksLastFrame = m_generatedPrefetchLaneBricksLastFrame;
    m_stats.generatedRepairLaneBricksLastFrame = m_generatedRepairLaneBricksLastFrame;
    m_stats.generatedVisibleLaneBricksLastFrame = m_generatedVisibleLaneBricksLastFrame;
    m_stats.generatedPublicCriticalLaneBricksLastFrame =
        m_generatedPublicCriticalLaneBricksLastFrame;
    m_stats.deferredGeneratedDownstreamPending = static_cast<uint32_t>(std::min<size_t>(
        m_deferredGeneratedDownstreamSet.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    m_stats.deferredGeneratedDownstreamPendingCache = 0u;
    m_stats.deferredGeneratedDownstreamPendingPrefetch = 0u;
    m_stats.deferredGeneratedDownstreamPendingRepair = 0u;
    m_stats.deferredGeneratedDownstreamPendingVisible = 0u;
    m_stats.deferredGeneratedDownstreamPendingPublicCritical = 0u;
    for (const BrickCoord& coord : m_deferredGeneratedDownstreamSet) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record) ||
            record.state != BrickLifecycleState::GeneratedCPU ||
            m_generated.find(coord) == m_generated.end()) {
            continue;
        }
        IncrementStreamingLaneCounter(
            record.streamingLane,
            m_stats.deferredGeneratedDownstreamPendingCache,
            m_stats.deferredGeneratedDownstreamPendingPrefetch,
            m_stats.deferredGeneratedDownstreamPendingRepair,
            m_stats.deferredGeneratedDownstreamPendingVisible,
            m_stats.deferredGeneratedDownstreamPendingPublicCritical);
    }
    m_stats.deferredGeneratedDownstreamPromotedLastFrame =
        m_deferredGeneratedDownstreamPromotedLastFrame;
    m_stats.deferredGeneratedDownstreamStaleLastFrame =
        m_deferredGeneratedDownstreamStaleLastFrame;
    uint32_t asyncQueueDepth = 0;
    uint32_t asyncResultDepth = 0;
    uint32_t asyncPending = 0;
    uint32_t asyncOldestAge = 0;
    if (m_config.asyncExactGeneration) {
        std::lock_guard<std::mutex> lock(m_asyncExactGenerationMutex);
        asyncQueueDepth = static_cast<uint32_t>(std::min<size_t>(
            m_asyncExactGenerationQueue.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        asyncResultDepth = static_cast<uint32_t>(std::min<size_t>(
            m_asyncExactGenerationResults.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        asyncPending = static_cast<uint32_t>(std::min<size_t>(
            m_asyncExactGenerationPending.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        auto updateOldest = [&](uint32_t requestFrame) {
            if (m_asyncExactGenerationStatsFrame >= requestFrame) {
                asyncOldestAge = std::max(asyncOldestAge, m_asyncExactGenerationStatsFrame - requestFrame);
            }
        };
        for (const AsyncExactGenerationRequest& request : m_asyncExactGenerationQueue) {
            updateOldest(request.requestFrame);
        }
        for (const AsyncExactGenerationResult& result : m_asyncExactGenerationResults) {
            updateOldest(result.requestFrame);
        }
    }
    m_stats.asyncExactGenerationEnabled = m_config.asyncExactGeneration ? 1u : 0u;
    m_stats.asyncExactGenerationQueueDepth = asyncQueueDepth;
    m_stats.asyncExactGenerationResultDepth = asyncResultDepth;
    m_stats.asyncExactGenerationPending = asyncPending;
    m_stats.asyncExactGenerationEnqueuedLastFrame = m_asyncExactGenerationEnqueuedLastFrame;
    m_stats.asyncExactGenerationCompletedLastFrame = m_asyncExactGenerationCompletedLastFrame;
    m_stats.asyncExactGenerationAppliedLastFrame = m_asyncExactGenerationAppliedLastFrame;
    m_stats.asyncExactGenerationDeferredLowPriorityApplyLastFrame =
        m_asyncExactGenerationDeferredLowPriorityApplyLastFrame;
    m_stats.asyncExactGenerationDiscardedLastFrame = m_asyncExactGenerationDiscardedLastFrame;
    m_stats.asyncExactGenerationSyncFallbackLastFrame = m_asyncExactGenerationSyncFallbackLastFrame;
    m_stats.asyncExactGenEditGateGlobalWouldSyncLastFrame = m_asyncExactGenEditGateGlobalWouldSyncLastFrame;
    m_stats.asyncExactGenEditGatePerCoordAsyncLastFrame = m_asyncExactGenEditGatePerCoordAsyncLastFrame;
    m_stats.asyncExactGenEditStaleAtCompletionLastFrame = m_asyncExactGenEditStaleAtCompletionLastFrame;
    m_stats.asyncExactGenerationOldestAge = asyncOldestAge;
    m_stats.asyncExactGenerationEnqueuedCacheLaneLastFrame =
        m_asyncExactGenerationEnqueuedCacheLaneLastFrame;
    m_stats.asyncExactGenerationEnqueuedPrefetchLaneLastFrame =
        m_asyncExactGenerationEnqueuedPrefetchLaneLastFrame;
    m_stats.asyncExactGenerationEnqueuedRepairLaneLastFrame =
        m_asyncExactGenerationEnqueuedRepairLaneLastFrame;
    m_stats.asyncExactGenerationEnqueuedVisibleLaneLastFrame =
        m_asyncExactGenerationEnqueuedVisibleLaneLastFrame;
    m_stats.asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame =
        m_asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame;
    m_stats.asyncExactGenerationAppliedCacheLaneLastFrame =
        m_asyncExactGenerationAppliedCacheLaneLastFrame;
    m_stats.asyncExactGenerationAppliedPrefetchLaneLastFrame =
        m_asyncExactGenerationAppliedPrefetchLaneLastFrame;
    m_stats.asyncExactGenerationAppliedRepairLaneLastFrame =
        m_asyncExactGenerationAppliedRepairLaneLastFrame;
    m_stats.asyncExactGenerationAppliedVisibleLaneLastFrame =
        m_asyncExactGenerationAppliedVisibleLaneLastFrame;
    m_stats.asyncExactGenerationAppliedPublicCriticalLaneLastFrame =
        m_asyncExactGenerationAppliedPublicCriticalLaneLastFrame;
    m_stats.asyncExactGenerationWorkerMsLastFrame = m_asyncExactGenerationWorkerMsLastFrame;
    m_stats.asyncExactGenerationApplyMsLastFrame = m_asyncExactGenerationApplyMsLastFrame;
    m_stats.parallelExactGenerationActive = m_parallelExactGenerationBricksLastFrame != 0u ? 1u : 0u;
    m_stats.parallelExactGenerationBricksLastFrame = m_parallelExactGenerationBricksLastFrame;
    m_stats.parallelExactGenerationWorkersLastFrame = m_parallelExactGenerationWorkersLastFrame;
    m_stats.parallelExactGenerationWallMsLastFrame = m_parallelExactGenerationWallMsLastFrame;
    m_stats.persistentExactGenerationWaitMsLastFrame =
        m_persistentExactGenerationWaitMsLastFrame;
    m_stats.uploadQueuedBricks = static_cast<uint32_t>(m_uploadQueue.size());
    m_stats.uploadQueuedSpeculativeBricks = m_uploadQueueClassCounts.speculative;
    m_stats.uploadQueuedVisibleBricks = m_uploadQueueClassCounts.visible;
    m_stats.uploadQueuedCollisionBricks = m_uploadQueueClassCounts.collision;
    m_stats.uploadQueuedEditedBricks = m_uploadQueueClassCounts.edited;
    m_stats.uploadQueuedCacheLaneBricks = m_uploadQueueLaneCounts.cache;
    m_stats.uploadQueuedPrefetchLaneBricks = m_uploadQueueLaneCounts.prefetch;
    m_stats.uploadQueuedRepairLaneBricks = m_uploadQueueLaneCounts.repair;
    m_stats.uploadQueuedVisibleLaneBricks = m_uploadQueueLaneCounts.visible;
    m_stats.uploadQueuedPublicCriticalLaneBricks = m_uploadQueueLaneCounts.publicCritical;
    m_stats.uploadedSpeculativeBricksLastFrame = m_uploadedSpeculativeBricksLastFrame;
    m_stats.uploadedVisibleBricksLastFrame = m_uploadedVisibleBricksLastFrame;
    m_stats.uploadedCollisionBricksLastFrame = m_uploadedCollisionBricksLastFrame;
    m_stats.uploadedEditedBricksLastFrame = m_uploadedEditedBricksLastFrame;
    m_stats.residentBricks = 0;
    m_stats.freePages = m_pool.FreePageCount();
    m_stats.residentSpeculativeBricks = 0;
    m_stats.residentVisibleBricks = 0;
    m_stats.residentCollisionBricks = 0;
    m_stats.residentEditedBricks = 0;
    m_stats.residentRenderableBricks = 0;
    m_stats.residentRenderableMissingSurfaces = 0;
    m_stats.evictionQueuedBricks = static_cast<uint32_t>(m_invalidationQueue.size());
    m_stats.evictedBricksLastFrame = m_evictedBricksLastFrame;
    m_stats.emptyRequestsSkippedLastFrame = m_emptyRequestsSkippedLastFrame;
    m_stats.knownEmptyGeneratedBricks = static_cast<uint32_t>(m_knownEmptyGeneratedBricks.size());
    m_stats.editedBricks = static_cast<uint32_t>(m_edits.EditedBrickCount());
    m_stats.editedVoxels = static_cast<uint32_t>(m_edits.EditedVoxelCount());
    m_stats.renderDirtyBricks = static_cast<uint32_t>(m_renderDirtyRegions.size());
    m_stats.renderDirtyRegionVoxels = 0;
    for (const auto& dirtyRegion : m_renderDirtyRegions) {
        m_stats.renderDirtyRegionVoxels += SparseRegionVoxelCount(dirtyRegion.second);
    }
    m_stats.renderDirtyVoxelsQueuedLastFrame = m_renderDirtyVoxelsQueuedLastFrame;
    m_stats.renderDirtyFullUploadsQueuedLastFrame = m_renderDirtyFullUploadsQueuedLastFrame;
    m_stats.renderDirtyUploadDeferredLastFrame = m_renderDirtyUploadDeferredLastFrame;
    m_stats.renderDirtyNonResidentLastFrame = m_renderDirtyNonResidentLastFrame;
    m_stats.surfaceCachedBricks = m_surfaceCache.GetStats().cachedBricks;
    m_stats.surfaceUnitFaces = m_surfaceCache.GetStats().totalUnitFaces;
    m_stats.surfaceFaces = m_surfaceCache.GetStats().totalFaces;
    m_stats.surfaceUnitFacesGeneratedLastFrame = m_surfaceCache.GetStats().unitFacesGeneratedLastUpdate;
    m_stats.surfaceFacesGeneratedLastFrame = m_surfaceCache.GetStats().facesGeneratedLastUpdate;
    m_stats.surfaceBricksUpdatedLastFrame = m_surfaceCache.GetStats().bricksUpdatedLastFrame;
    m_stats.surfaceBricksPartiallyUpdatedLastFrame =
        m_surfaceCache.GetStats().bricksPartiallyUpdatedLastFrame;
    m_stats.surfaceFacesRemovedByPartialUpdatesLastFrame =
        m_surfaceCache.GetStats().facesRemovedByPartialUpdatesLastFrame;
    m_stats.surfaceBricksRemovedLastFrame = m_surfaceCache.GetStats().bricksRemovedLastFrame;
    m_stats.surfacePendingGpuDirtyBricks = m_surfaceCache.GetStats().pendingGpuDirtyBricks;
    m_stats.surfacePendingGpuRemovedBricks = m_surfaceCache.GetStats().pendingGpuRemovedBricks;
    m_stats.surfaceExtractionQueuedBricks = static_cast<uint32_t>(m_pendingSurfaceBricks.size());
    m_stats.surfaceQueuedSpeculativeBricks = m_surfaceQueueClassCounts.speculative;
    m_stats.surfaceQueuedVisibleBricks = m_surfaceQueueClassCounts.visible;
    m_stats.surfaceQueuedCollisionBricks = m_surfaceQueueClassCounts.collision;
    m_stats.surfaceQueuedEditedBricks = m_surfaceQueueClassCounts.edited;
    m_stats.surfaceQueuedCacheLaneBricks = m_surfaceQueueLaneCounts.cache;
    m_stats.surfaceQueuedPrefetchLaneBricks = m_surfaceQueueLaneCounts.prefetch;
    m_stats.surfaceQueuedRepairLaneBricks = m_surfaceQueueLaneCounts.repair;
    m_stats.surfaceQueuedVisibleLaneBricks = m_surfaceQueueLaneCounts.visible;
    m_stats.surfaceQueuedPublicCriticalLaneBricks = m_surfaceQueueLaneCounts.publicCritical;
    m_stats.streamingLaneQueuePriorityActive = m_config.streamingLaneQueuePriority ? 1u : 0u;
    m_stats.streamingTicketSchedulerActive = m_config.streamingTicketScheduler ? 1u : 0u;
    m_stats.streamingTicketProtectedSchedulingActive =
        m_config.streamingTicketProtectedScheduling ? 1u : 0u;
    m_stats.streamingTicketProtectedSortsLastFrame =
        m_config.streamingTicketProtectedScheduling
            ? m_streamingTicketProtectedSortsLastFrame
            : 0u;
    m_stats.streamingTicketCompletedLastFrame = m_config.streamingTicketScheduler
        ? m_streamingTicketCompletedLastFrame
        : 0u;
    m_stats.streamingTicketActive = 0u;
    m_stats.streamingTicketOwnershipPublicCritical = 0u;
    m_stats.streamingTicketOwnershipSampledVisible = 0u;
    m_stats.streamingTicketOwnershipHiddenRepair = 0u;
    m_stats.streamingTicketOwnershipCache = 0u;
    m_stats.streamingTicketOwnershipPrefetch = 0u;
    m_stats.streamingTicketOwnershipFallbackValid = 0u;
    m_stats.streamingTicketOwnershipUnknownCritical = 0u;
    m_stats.streamingTicketPendingCpu = 0u;
    m_stats.streamingTicketPendingUpload = 0u;
    m_stats.streamingTicketPendingSurface = 0u;
    m_stats.streamingTicketPendingPublish = 0u;
    m_stats.streamingTicketRequiredCpu = 0u;
    m_stats.streamingTicketRequiredUpload = 0u;
    m_stats.streamingTicketRequiredSurface = 0u;
    m_stats.streamingTicketRequiredPublish = 0u;
    m_stats.streamingTicketOldestAge = 0u;
    if (m_config.streamingTicketScheduler) {
        uint32_t statsFrame = 0u;
        uint32_t oldestRequestFrame = UINT32_MAX;
        m_stats.streamingTicketActive = static_cast<uint32_t>(std::min<size_t>(
            m_streamingTickets.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        if (m_config.streamingTicketStageDemandAccounting) {
            const auto pendingOwnershipCount = [&](uint32_t stageBit, StreamingTicketOwnership ownership) {
                const size_t stageIndex = StreamingTicketStageIndex(stageBit);
                if (stageIndex >= kStreamingTicketStageCount) {
                    return 0u;
                }
                return m_streamingTicketPendingStageOwnershipCounts[stageIndex]
                    [StreamingTicketOwnershipIndex(ownership)];
            };
            const auto pendingStageTotal = [&](uint32_t stageBit) {
                const size_t stageIndex = StreamingTicketStageIndex(stageBit);
                if (stageIndex >= kStreamingTicketStageCount) {
                    return 0u;
                }
                uint32_t total = 0u;
                for (uint32_t count : m_streamingTicketPendingStageOwnershipCounts[stageIndex]) {
                    total += count;
                }
                return total;
            };
            m_stats.generationPendingOwnershipPublicCritical =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::PublicCritical);
            m_stats.generationPendingOwnershipSampledVisible =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::SampledVisible);
            m_stats.generationPendingOwnershipHiddenRepair =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::HiddenRepair);
            m_stats.generationPendingOwnershipCache =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::Cache);
            m_stats.generationPendingOwnershipPrefetch =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::Prefetch);
            m_stats.generationPendingOwnershipFallbackValid =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::FallbackValid);
            m_stats.generationPendingOwnershipUnknownCritical =
                pendingOwnershipCount(kStreamingTicketStageCpuGenerated, StreamingTicketOwnership::UnknownCritical);
            m_stats.streamingTicketPendingCpu = pendingStageTotal(kStreamingTicketStageCpuGenerated);
            m_stats.streamingTicketPendingUpload = pendingStageTotal(kStreamingTicketStageGpuUploaded);
            m_stats.streamingTicketPendingSurface = pendingStageTotal(kStreamingTicketStageSurfaceReady);
            m_stats.streamingTicketPendingPublish = pendingStageTotal(kStreamingTicketStagePagePublished);
        }
        for (const auto& ticketEntry : m_streamingTickets) {
            const StreamingWorkTicket& ticket = ticketEntry.second;
            statsFrame = std::max(statsFrame, ticket.lastUpdatedFrame);
            oldestRequestFrame = std::min(oldestRequestFrame, ticket.requestFrame);
            switch (ticket.ownership) {
                case StreamingTicketOwnership::PublicCritical:
                    ++m_stats.streamingTicketOwnershipPublicCritical;
                    break;
                case StreamingTicketOwnership::SampledVisible:
                    ++m_stats.streamingTicketOwnershipSampledVisible;
                    break;
                case StreamingTicketOwnership::HiddenRepair:
                    ++m_stats.streamingTicketOwnershipHiddenRepair;
                    break;
                case StreamingTicketOwnership::Prefetch:
                    ++m_stats.streamingTicketOwnershipPrefetch;
                    break;
                case StreamingTicketOwnership::FallbackValid:
                    ++m_stats.streamingTicketOwnershipFallbackValid;
                    break;
                case StreamingTicketOwnership::UnknownCritical:
                    ++m_stats.streamingTicketOwnershipUnknownCritical;
                    break;
                case StreamingTicketOwnership::Cache:
                default:
                    ++m_stats.streamingTicketOwnershipCache;
                    break;
            }
            const uint32_t pendingStages = ticket.requiredStages & ~ticket.completedStages;
            if (!m_config.streamingTicketStageDemandAccounting) {
                if ((pendingStages & kStreamingTicketStageCpuGenerated) != 0u) {
                    switch (ticket.ownership) {
                        case StreamingTicketOwnership::PublicCritical:
                            ++m_stats.generationPendingOwnershipPublicCritical;
                            break;
                        case StreamingTicketOwnership::SampledVisible:
                            ++m_stats.generationPendingOwnershipSampledVisible;
                            break;
                        case StreamingTicketOwnership::HiddenRepair:
                            ++m_stats.generationPendingOwnershipHiddenRepair;
                            break;
                        case StreamingTicketOwnership::Prefetch:
                            ++m_stats.generationPendingOwnershipPrefetch;
                            break;
                        case StreamingTicketOwnership::FallbackValid:
                            ++m_stats.generationPendingOwnershipFallbackValid;
                            break;
                        case StreamingTicketOwnership::UnknownCritical:
                            ++m_stats.generationPendingOwnershipUnknownCritical;
                            break;
                        case StreamingTicketOwnership::Cache:
                        default:
                            ++m_stats.generationPendingOwnershipCache;
                            break;
                    }
                }
                if ((pendingStages & kStreamingTicketStageCpuGenerated) != 0u) {
                    ++m_stats.streamingTicketPendingCpu;
                }
                if ((pendingStages & kStreamingTicketStageGpuUploaded) != 0u) {
                    ++m_stats.streamingTicketPendingUpload;
                }
                if ((pendingStages & kStreamingTicketStageSurfaceReady) != 0u) {
                    ++m_stats.streamingTicketPendingSurface;
                }
                if ((pendingStages & kStreamingTicketStagePagePublished) != 0u) {
                    ++m_stats.streamingTicketPendingPublish;
                }
            }
            if ((ticket.requiredStages & kStreamingTicketStageCpuGenerated) != 0u) {
                ++m_stats.streamingTicketRequiredCpu;
            }
            if ((ticket.requiredStages & kStreamingTicketStageGpuUploaded) != 0u) {
                ++m_stats.streamingTicketRequiredUpload;
            }
            if ((ticket.requiredStages & kStreamingTicketStageSurfaceReady) != 0u) {
                ++m_stats.streamingTicketRequiredSurface;
            }
            if ((ticket.requiredStages & kStreamingTicketStagePagePublished) != 0u) {
                ++m_stats.streamingTicketRequiredPublish;
            }
        }
        if (oldestRequestFrame != UINT32_MAX && statsFrame >= oldestRequestFrame) {
            m_stats.streamingTicketOldestAge = statsFrame - oldestRequestFrame;
        }
    }
    m_stats.surfaceBricksExtractedLastFrame = m_surfaceBricksExtractedLastFrame;
    m_stats.surfaceSpeculativeBricksExtractedLastFrame = m_surfaceSpeculativeBricksExtractedLastFrame;
    m_stats.surfaceVisibleBricksExtractedLastFrame = m_surfaceVisibleBricksExtractedLastFrame;
    m_stats.surfaceCollisionBricksExtractedLastFrame = m_surfaceCollisionBricksExtractedLastFrame;
    m_stats.surfaceEditedBricksExtractedLastFrame = m_surfaceEditedBricksExtractedLastFrame;
    m_stats.surfaceEmptyUploadsSkippedLastFrame = m_surfaceEmptyUploadsSkippedLastFrame;
    m_stats.surfaceEmptyFastPathBricksLastFrame = m_surfaceCache.GetStats().emptyFastPathBricksLastFrame;
    m_stats.surfaceBuriedSolidFastPathBricksLastFrame = m_surfaceBuriedSolidFastPathBricksLastFrame;
    m_stats.surfaceClassValueSortCallsLastFrame = m_surfaceClassValueSortCallsLastFrame;
    m_stats.surfaceClassValueSortCacheHitsLastFrame = m_surfaceClassValueSortCacheHitsLastFrame;
    m_stats.surfaceStrictTimeBudgetUnsortedPopsLastFrame =
        m_surfaceStrictTimeBudgetUnsortedPopsLastFrame;
    m_stats.surfaceInlineExtractionBricksLastFrame =
        m_surfaceInlineExtractionBricksLastFrame;
    m_stats.surfaceInlineExtractionMsLastFrame =
        m_surfaceInlineExtractionMsLastFrame;
    m_stats.parallelSurfaceExtractionActive =
        m_parallelSurfaceExtractionBricksLastFrame != 0u ? 1u : 0u;
    m_stats.parallelSurfaceExtractionBricksLastFrame = m_parallelSurfaceExtractionBricksLastFrame;
    m_stats.parallelSurfaceExtractionWorkersLastFrame = m_parallelSurfaceExtractionWorkersLastFrame;
    m_stats.parallelSurfaceExtractionWallMsLastFrame = m_parallelSurfaceExtractionWallMsLastFrame;
    m_stats.surfaceExtractionWaitMsLastFrame = m_surfaceExtractionWaitMsLastFrame;
    m_stats.asyncSurfaceExtractionEnabled = m_config.asyncSurfaceExtraction ? 1u : 0u;
    {
        std::lock_guard<std::mutex> lock(m_asyncSurfaceExtractionMutex);
        m_stats.asyncSurfaceExtractionQueueDepth = static_cast<uint32_t>(std::min<size_t>(
            m_asyncSurfaceExtractionQueue.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        m_stats.asyncSurfaceExtractionResultDepth = static_cast<uint32_t>(std::min<size_t>(
            m_asyncSurfaceExtractionResults.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        m_stats.asyncSurfaceExtractionPending = static_cast<uint32_t>(std::min<size_t>(
            m_asyncSurfaceExtractionPending.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    }
    m_stats.asyncSurfaceExtractionEnqueuedLastFrame = m_asyncSurfaceExtractionEnqueuedLastFrame;
    m_stats.asyncSurfaceExtractionAppliedLastFrame = m_asyncSurfaceExtractionAppliedLastFrame;
    m_stats.asyncSurfaceExtractionDiscardedLastFrame = m_asyncSurfaceExtractionDiscardedLastFrame;
    m_stats.asyncSurfaceExtractionRequeuedLastFrame = m_asyncSurfaceExtractionRequeuedLastFrame;
    m_stats.asyncSurfaceExtractionWorkerMsLastFrame = m_asyncSurfaceExtractionWorkerMsLastFrame;
    m_stats.asyncSurfaceExtractionEnqueueMsLastFrame =
        m_asyncSurfaceExtractionEnqueueMsLastFrame;
    m_stats.asyncSurfaceExtractionRejectedLastFrame =
        m_asyncSurfaceExtractionRejectedLastFrame;
    m_stats.terrainColumnCachePersistentActive = m_config.persistentTerrainColumnCache ? 1u : 0u;
    m_stats.terrainColumnCacheEntries = static_cast<uint32_t>(std::min<size_t>(
        m_surfaceTerrainColumnCache.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    m_stats.terrainColumnCacheMaxEntries = m_config.terrainColumnCacheMaxEntries;
    m_stats.terrainColumnCacheClearedLastFrame = m_terrainColumnCacheClearedLastFrame;
    m_stats.terrainColumnCacheHeightHitsLastFrame = m_terrainColumnCacheFrameStats.heightHits;
    m_stats.terrainColumnCacheHeightMissesLastFrame = m_terrainColumnCacheFrameStats.heightMisses;
    m_stats.terrainColumnCacheReliefHitsLastFrame = m_terrainColumnCacheFrameStats.reliefHits;
    m_stats.terrainColumnCacheReliefMissesLastFrame = m_terrainColumnCacheFrameStats.reliefMisses;
    m_stats.surfaceSerial = m_surfaceCache.GetStats().serial;
    m_stats.physicsCandidateBricks = static_cast<uint32_t>(m_physicsQueuedPriorities.size());
    m_stats.physicsHotCandidateBricks = 0;
    m_stats.physicsWarmCandidateBricks = 0;
    for (const auto& queued : m_physicsQueuedPriorities) {
        if (queued.second == SparsePhysicsPriority::Hot) {
            ++m_stats.physicsHotCandidateBricks;
        } else {
            ++m_stats.physicsWarmCandidateBricks;
        }
    }
    m_stats.physicsWorkPacketsLastFrame = m_physicsWorkPacketsLastFrame;
    m_stats.physicsHotWorkPacketsLastFrame = m_physicsHotWorkPacketsLastFrame;
    m_stats.physicsWarmWorkPacketsLastFrame = m_physicsWarmWorkPacketsLastFrame;
    m_stats.physicsDirtyRegionVoxelsLastFrame = m_physicsDirtyRegionVoxelsLastFrame;
    m_stats.physicsProcessedBricksLastFrame = m_physicsProcessedBricksLastFrame;
    m_stats.physicsMovedVoxelsLastFrame = m_physicsMovedVoxelsLastFrame;
    m_stats.physicsSkippedVoxelsLastFrame = m_physicsSkippedVoxelsLastFrame;
    m_stats.physicsSupportBricksRequestedLastFrame = m_physicsSupportBricksRequestedLastFrame;
    m_stats.physicsGpuProcessedProposalsLastFrame = m_physicsGpuProcessedProposalsLastFrame;
    m_stats.physicsGpuAppliedMovesLastFrame = m_physicsGpuAppliedMovesLastFrame;
    m_stats.physicsGpuRejectedProposalsLastFrame = m_physicsGpuRejectedProposalsLastFrame;
    m_stats.trimScanCallsLastFrame = m_trimScanCallsLastFrame;
    m_stats.trimRecordsScannedLastFrame = m_trimRecordsScannedLastFrame;
    m_stats.trimCandidatesLastFrame = m_trimCandidatesLastFrame;
    m_stats.replacementScanCallsLastFrame = m_replacementScanCallsLastFrame;
    m_stats.replacementRecordsScannedLastFrame = m_replacementRecordsScannedLastFrame;
    m_stats.replacementCandidatesLastFrame = m_replacementCandidatesLastFrame;

    for (const auto& record : m_pool.Records()) {
        if (record.state != BrickLifecycleState::Resident) {
            continue;
        }
        ++m_stats.residentBricks;
        uint32_t residentFlags = 0;
        if (m_pool.PageTable().TryLookupExactGeneration(
                record.coord,
                record.generation,
                nullptr,
                &residentFlags) &&
            record.gpuPageTablePublished &&
            (residentFlags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) == 0u) {
            ++m_stats.residentRenderableBricks;
            if (!m_surfaceCache.IsSurfaceKnown(record.coord) &&
                m_pendingSurfaceBricks.find(record.coord) == m_pendingSurfaceBricks.end()) {
                ++m_stats.residentRenderableMissingSurfaces;
            }
        }
        switch (record.residencyClass) {
            case SparseResidencyClass::Edited:
                ++m_stats.residentEditedBricks;
                break;
            case SparseResidencyClass::Collision:
                ++m_stats.residentCollisionBricks;
                break;
            case SparseResidencyClass::Visible:
                ++m_stats.residentVisibleBricks;
                break;
            case SparseResidencyClass::Speculative:
            default:
                ++m_stats.residentSpeculativeBricks;
                break;
        }
    }
}

uint32_t SparseVoxelWorld::SampleEditedOrGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    HEIGHTAT_SCOPE("SampleEditedOrGeneratedVoxel");
    uint32_t editedVoxel = 0;
    if (m_edits.TryGetVoxel(worldX, worldY, worldZ, &editedVoxel)) {
        return editedVoxel;
    }
    return m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
}

const char* ToString(SparseRenderReadinessState state) {
    switch (state) {
        case SparseRenderReadinessState::Missing: return "Missing";
        case SparseRenderReadinessState::Requested: return "Requested";
        case SparseRenderReadinessState::GeneratingCPU: return "GeneratingCPU";
        case SparseRenderReadinessState::GeneratedCPU: return "GeneratedCPU";
        case SparseRenderReadinessState::UploadQueued: return "UploadQueued";
        case SparseRenderReadinessState::UploadingGPU: return "UploadingGPU";
        case SparseRenderReadinessState::ResidentEmpty: return "ResidentEmpty";
        case SparseRenderReadinessState::ResidentMissingSurface: return "ResidentMissingSurface";
        case SparseRenderReadinessState::ReadyToRender: return "ReadyToRender";
        case SparseRenderReadinessState::DirtyCPU: return "DirtyCPU";
        case SparseRenderReadinessState::DirtyGPU: return "DirtyGPU";
        case SparseRenderReadinessState::EvictQueued: return "EvictQueued";
        case SparseRenderReadinessState::Evicted: return "Evicted";
        default: return "Unknown";
    }
}

SparseRenderReadinessState SparseVoxelWorld::GetRenderReadinessState(const BrickCoord& coord) const {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record) || record.pageIndex == INVALID_BRICK_PAGE) {
        return SparseRenderReadinessState::Missing;
    }

    switch (record.state) {
        case BrickLifecycleState::Requested:
            return SparseRenderReadinessState::Requested;
        case BrickLifecycleState::GeneratingCPU:
            return SparseRenderReadinessState::GeneratingCPU;
        case BrickLifecycleState::GeneratedCPU:
            return SparseRenderReadinessState::GeneratedCPU;
        case BrickLifecycleState::UploadQueued:
            return SparseRenderReadinessState::UploadQueued;
        case BrickLifecycleState::UploadingGPU:
            return SparseRenderReadinessState::UploadingGPU;
        case BrickLifecycleState::DirtyCPU:
            return SparseRenderReadinessState::DirtyCPU;
        case BrickLifecycleState::DirtyGPU:
            return SparseRenderReadinessState::DirtyGPU;
        case BrickLifecycleState::EvictQueued:
            return SparseRenderReadinessState::EvictQueued;
        case BrickLifecycleState::Evicted:
            return SparseRenderReadinessState::Evicted;
        case BrickLifecycleState::Resident:
            break;
        case BrickLifecycleState::Missing:
        default:
            return SparseRenderReadinessState::Missing;
    }

    uint32_t residentFlags = 0;
    if (!m_pool.PageTable().TryLookupExactGeneration(
            record.coord,
            record.generation,
            nullptr,
            &residentFlags)) {
        return SparseRenderReadinessState::UploadingGPU;
    }
    if (!record.gpuPageTablePublished) {
        return SparseRenderReadinessState::UploadingGPU;
    }
    if ((residentFlags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) != 0u) {
        return SparseRenderReadinessState::ResidentEmpty;
    }
    if (!m_surfaceCache.IsSurfaceKnown(record.coord)) {
        return SparseRenderReadinessState::ResidentMissingSurface;
    }
    return SparseRenderReadinessState::ReadyToRender;
}

SparseRenderReadinessState SparseVoxelWorld::GetRenderReadinessStateKnownPage(
    uint32_t pageIndex,
    const BrickCoord& coord) const
{
    const auto& records = m_pool.Records();
    if (pageIndex >= records.size()) {
        return SparseRenderReadinessState::Missing;
    }

    const BrickResidentRecord& record = records[pageIndex];
    if (record.pageIndex != pageIndex ||
        record.coord != coord ||
        record.pageIndex == INVALID_BRICK_PAGE) {
        return SparseRenderReadinessState::Missing;
    }

    switch (record.state) {
        case BrickLifecycleState::Requested:
            return SparseRenderReadinessState::Requested;
        case BrickLifecycleState::GeneratingCPU:
            return SparseRenderReadinessState::GeneratingCPU;
        case BrickLifecycleState::GeneratedCPU:
            return SparseRenderReadinessState::GeneratedCPU;
        case BrickLifecycleState::UploadQueued:
            return SparseRenderReadinessState::UploadQueued;
        case BrickLifecycleState::UploadingGPU:
            return SparseRenderReadinessState::UploadingGPU;
        case BrickLifecycleState::DirtyCPU:
            return SparseRenderReadinessState::DirtyCPU;
        case BrickLifecycleState::DirtyGPU:
            return SparseRenderReadinessState::DirtyGPU;
        case BrickLifecycleState::EvictQueued:
            return SparseRenderReadinessState::EvictQueued;
        case BrickLifecycleState::Evicted:
            return SparseRenderReadinessState::Evicted;
        case BrickLifecycleState::Resident:
            break;
        case BrickLifecycleState::Missing:
        default:
            return SparseRenderReadinessState::Missing;
    }

    uint32_t residentFlags = 0;
    if (!m_pool.PageTable().TryLookupExactGeneration(
            record.coord,
            record.generation,
            nullptr,
            &residentFlags)) {
        return SparseRenderReadinessState::UploadingGPU;
    }
    if (!record.gpuPageTablePublished) {
        return SparseRenderReadinessState::UploadingGPU;
    }
    if ((residentFlags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) != 0u) {
        return SparseRenderReadinessState::ResidentEmpty;
    }
    if (!m_surfaceCache.IsSurfaceKnown(record.coord)) {
        return SparseRenderReadinessState::ResidentMissingSurface;
    }
    return SparseRenderReadinessState::ReadyToRender;
}

SparseRenderReadinessStats SparseVoxelWorld::BuildRenderReadinessStats() const {
    SparseRenderReadinessStats stats;
    for (const auto& record : m_pool.Records()) {
        if (record.state == BrickLifecycleState::Missing ||
            record.pageIndex == INVALID_BRICK_PAGE) {
            continue;
        }
        ++stats.totalTracked;
        SparseRenderReadinessState state = SparseRenderReadinessState::Missing;
        switch (record.state) {
            case BrickLifecycleState::Requested:
                state = SparseRenderReadinessState::Requested;
                break;
            case BrickLifecycleState::GeneratingCPU:
                state = SparseRenderReadinessState::GeneratingCPU;
                break;
            case BrickLifecycleState::GeneratedCPU:
                state = SparseRenderReadinessState::GeneratedCPU;
                break;
            case BrickLifecycleState::UploadQueued:
                state = SparseRenderReadinessState::UploadQueued;
                break;
            case BrickLifecycleState::UploadingGPU:
                state = SparseRenderReadinessState::UploadingGPU;
                break;
            case BrickLifecycleState::Resident: {
                uint32_t residentFlags = 0;
                if (!m_pool.PageTable().TryLookupExactGeneration(
                        record.coord,
                        record.generation,
                        nullptr,
                        &residentFlags)) {
                    state = SparseRenderReadinessState::UploadingGPU;
                } else if (!record.gpuPageTablePublished) {
                    state = SparseRenderReadinessState::UploadingGPU;
                } else if ((residentFlags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) != 0u) {
                    state = SparseRenderReadinessState::ResidentEmpty;
                } else if (!m_surfaceCache.IsSurfaceKnown(record.coord)) {
                    state = SparseRenderReadinessState::ResidentMissingSurface;
                } else {
                    state = SparseRenderReadinessState::ReadyToRender;
                }
                break;
            }
            case BrickLifecycleState::DirtyCPU:
                state = SparseRenderReadinessState::DirtyCPU;
                break;
            case BrickLifecycleState::DirtyGPU:
                state = SparseRenderReadinessState::DirtyGPU;
                break;
            case BrickLifecycleState::EvictQueued:
                state = SparseRenderReadinessState::EvictQueued;
                break;
            case BrickLifecycleState::Evicted:
                state = SparseRenderReadinessState::Evicted;
                break;
            case BrickLifecycleState::Missing:
            default:
                state = SparseRenderReadinessState::Missing;
                break;
        }

        switch (state) {
            case SparseRenderReadinessState::Missing:
                ++stats.missing;
                break;
            case SparseRenderReadinessState::Requested:
                ++stats.requested;
                break;
            case SparseRenderReadinessState::GeneratingCPU:
                ++stats.generatingCPU;
                break;
            case SparseRenderReadinessState::GeneratedCPU:
                ++stats.generatedCPU;
                break;
            case SparseRenderReadinessState::UploadQueued:
                ++stats.uploadQueued;
                break;
            case SparseRenderReadinessState::UploadingGPU:
                ++stats.uploadingGPU;
                break;
            case SparseRenderReadinessState::ResidentEmpty:
                ++stats.residentEmpty;
                break;
            case SparseRenderReadinessState::ResidentMissingSurface:
                ++stats.residentMissingSurface;
                break;
            case SparseRenderReadinessState::ReadyToRender:
                ++stats.readyToRender;
                break;
            case SparseRenderReadinessState::DirtyCPU:
                ++stats.dirtyCPU;
                break;
            case SparseRenderReadinessState::DirtyGPU:
                ++stats.dirtyGPU;
                break;
            case SparseRenderReadinessState::EvictQueued:
                ++stats.evictQueued;
                break;
            case SparseRenderReadinessState::Evicted:
                ++stats.evicted;
                break;
        }
    }
    return stats;
}

void SparseVoxelWorld::AnnotateRenderDirtyUploadRange(SparseBrickUploadPacket* packet) const {
    if (!packet || packet->pageIndex == INVALID_BRICK_PAGE || packet->generation == 0) {
        return;
    }

    packet->partialVoxelUpload = false;
    packet->voxelStartIndex = 0;
    packet->voxelCount = SPARSE_BRICK_VOXEL_COUNT;
    packet->dirtyMinX = 0;
    packet->dirtyMinY = 0;
    packet->dirtyMinZ = 0;
    packet->dirtyMaxX = SPARSE_BRICK_SIZE - 1;
    packet->dirtyMaxY = SPARSE_BRICK_SIZE - 1;
    packet->dirtyMaxZ = SPARSE_BRICK_SIZE - 1;

    auto dirtyIt = m_renderDirtyRegions.find(packet->coord);
    if (dirtyIt == m_renderDirtyRegions.end()) {
        return;
    }

    uint32_t residentPage = INVALID_BRICK_PAGE;
    if (!m_pool.PageTable().TryLookupExactGeneration(
            packet->coord,
            packet->generation,
            &residentPage,
            nullptr) ||
        residentPage != packet->pageIndex) {
        return;
    }

    const SparseRenderDirtyRegion& region = dirtyIt->second;
    const uint16_t startIndex = LocalVoxelIndex({region.minX, region.minY, region.minZ});
    const uint16_t endIndex = LocalVoxelIndex({region.maxX, region.maxY, region.maxZ});
    if (endIndex < startIndex) {
        return;
    }

    const uint32_t count = static_cast<uint32_t>(endIndex - startIndex) + 1u;
    if (count == 0 || count >= SPARSE_BRICK_VOXEL_COUNT) {
        return;
    }

    packet->partialVoxelUpload = true;
    packet->voxelStartIndex = startIndex;
    packet->voxelCount = static_cast<uint16_t>(count);
    packet->dirtyMinX = region.minX;
    packet->dirtyMinY = region.minY;
    packet->dirtyMinZ = region.minZ;
    packet->dirtyMaxX = region.maxX;
    packet->dirtyMaxY = region.maxY;
    packet->dirtyMaxZ = region.maxZ;
}

bool SparseVoxelWorld::QueueRegeneratedUploadForExistingPage(
    const BrickCoord& coord,
    const SparseRenderDirtyRegion* dirtyRegion,
    bool surfaceGeometryDirty)
{
    if (dirtyRegion) {
        QueueRenderDirtyRegionNoStats(coord, *dirtyRegion, surfaceGeometryDirty);
    } else if (m_renderDirtyRegions.find(coord) == m_renderDirtyRegions.end()) {
        SparseRenderDirtyRegion fullRegion;
        QueueRenderDirtyRegionNoStats(coord, fullRegion, surfaceGeometryDirty);
    }

    if (!m_pool.TryGetPage(coord)) {
        ++m_renderDirtyNonResidentLastFrame;
        return false;
    }

    m_pool.MarkHasPersistentEdits(coord);
    MarkResidencyClass(coord, SparseResidencyClass::Edited);

    // The full 16^3 regen + edit composite is DEFERRED to a per-frame budget
    // (PumpRegeneratedEditUploads). Running it inline was a measured ~31ms/frame
    // while painting interactively: every stamp re-built every touched brick on
    // the main thread. Lifecycle checks happen at drain time - the brick's state
    // when the budgeted regen actually runs is what matters.
    if (m_pendingRegenUploadSet.insert(coord).second) {
        m_pendingRegenUploadQueue.push_back(coord);
    }
    return true;
}

uint32_t SparseVoxelWorld::PumpRegeneratedEditUploads(uint32_t maxBricks)
{
    if (m_pendingRegenUploadQueue.empty()) {
        return 0;
    }
    // Catch-up: a fast stroke must not let the visible-edit lag grow unbounded.
    if (m_pendingRegenUploadQueue.size() > 24) {
        maxBricks = std::max(maxBricks * 2u, 8u);
    }
    uint32_t pumped = 0;
    while (pumped < maxBricks && !m_pendingRegenUploadQueue.empty()) {
        const BrickCoord coord = m_pendingRegenUploadQueue.front();
        m_pendingRegenUploadQueue.pop_front();
        m_pendingRegenUploadSet.erase(coord);

        if (!m_pool.TryGetPage(coord)) {
            ++m_renderDirtyNonResidentLastFrame;
            continue;
        }
        const BrickLifecycleState state = m_pool.GetState(coord);
        if (state == BrickLifecycleState::Requested || state == BrickLifecycleState::GeneratingCPU) {
            continue;
        }
        if (state == BrickLifecycleState::UploadingGPU) {
            m_deferredDirtyAfterUpload[coord] = true;
            ++m_renderDirtyUploadDeferredLastFrame;
            continue;
        }
        if (state == BrickLifecycleState::EvictQueued || state == BrickLifecycleState::Evicted ||
            state == BrickLifecycleState::Missing) {
            continue;
        }

        // The procedural base of a brick never changes when it is edited - only
        // the painted/erased voxels do. So if we already hold the generated brick
        // (it is resident/cached), skip the full 16^3 procedural regen entirely and
        // just (re)apply the edit overlay in place. ApplyToGeneratedBrick SETS the
        // edited voxels (idempotent) and recomputes occupancy/flags, so re-applying
        // onto an already-edited cached brick is correct. This drops the
        // ~2.5ms/brick procedural regen that dominated the per-stroke 'clip' cost
        // (the source of both the edit hitch and, via its budget, the skip-voxel
        // latency). Fall back to a full regen only when the brick is not cached.
        auto generatedIt = m_generated.find(coord);
        if (generatedIt != m_generated.end()) {
            m_edits.ApplyToGeneratedBrick(generatedIt->second);
        } else {
            GeneratedSparseBrick brick = GenerateBrickWithCachedTerrainColumns(coord);
            m_edits.ApplyToGeneratedBrick(brick);
            m_generated[coord] = brick;
        }
        ++pumped;

        if (state == BrickLifecycleState::UploadQueued) {
            continue;
        }
        if (state == BrickLifecycleState::Resident) {
            if (!m_pool.MarkDirty(coord)) {
                continue;
            }
        }
        if (!m_pool.QueueUpload(coord)) {
            continue;
        }
        QueueUploadCoordBack(coord);
        ++m_renderDirtyFullUploadsQueuedLastFrame;
    }
    return pumped;
}

} // namespace VENPOD::Simulation
