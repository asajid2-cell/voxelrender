#include "SparseVoxelWorld.h"

#include "Simulation/TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE = 2u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH = 4u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE = 8u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL = 16u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT = 64u;

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

int32_t SparseWorldVoxelFromLocal(int32_t brickCoord, uint8_t local) {
    return brickCoord * SPARSE_BRICK_SIZE + static_cast<int32_t>(local);
}

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

size_t ResidencyClassQueueIndex(SparseResidencyClass residencyClass) {
    return static_cast<size_t>(ResidencyRank(residencyClass));
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

int64_t QueuePriorityScore(const BrickResidentRecord& record, uint32_t currentFrame) {
    const uint32_t latestTouch = LatestPriorityTouch(record);
    const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
    const uint32_t freshness = currentFrame == 0
        ? latestTouch
        : (100000u - std::min(age, 100000u));
    return static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 1000000000ll +
           static_cast<int64_t>(freshness);
}

int64_t UploadValueScore(
    const BrickResidentRecord& record,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    const int64_t dx = static_cast<int64_t>(record.coord.x) - focus.x;
    const int64_t dy = static_cast<int64_t>(record.coord.y) - focus.y;
    const int64_t dz = static_cast<int64_t>(record.coord.z) - focus.z;
    const int64_t distancePenalty = dx * dx + dz * dz + dy * dy * 4;
    const uint32_t latestTouch = LatestPriorityTouch(record);
    const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
    const uint32_t freshness = currentFrame == 0
        ? latestTouch
        : (100000u - std::min(age, 100000u));
    return static_cast<int64_t>(ResidencyRank(record.residencyClass)) * 1000000000000ll +
           static_cast<int64_t>(freshness) * 10000ll -
           distancePenalty;
}

void SortQueuedBricksByPriority(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    uint32_t currentFrame)
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
        sorted.push_back({coord, QueuePriorityScore(record, currentFrame)});
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
    uint32_t currentFrame)
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
        sorted.push_back({coord, UploadValueScore(record, focus, currentFrame)});
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

bool SparseVoxelWorld::Initialize(const SparseVoxelWorldConfig& config) {
    m_config = config;
    m_terrain = SparseTerrainGenerator(config.seed);
    m_edits = SparseEditStore{};
    m_generated.clear();
    m_pendingSurfaceBricks.clear();
    m_knownEmptyGeneratedBricks.clear();
    m_generationQueue.clear();
    for (auto& classQueue : m_generationClassQueues) {
        classQueue.clear();
    }
    m_uploadQueue.clear();
    for (auto& classQueue : m_uploadClassQueues) {
        classQueue.clear();
    }
    m_invalidationQueue.clear();
    m_surfaceExtractionQueue.clear();
    m_surfaceExtractionQueuedSet.clear();
    for (auto& classQueue : m_surfaceClassQueues) {
        classQueue.clear();
    }
    m_generationQueuePriorityDirty = false;
    m_uploadQueuePriorityDirty = false;
    m_surfaceExtractionQueuePriorityDirty = false;
    m_queueClassStatsDirty = true;
    m_cachedGenerationQueueSize = 0;
    m_cachedUploadQueueSize = 0;
    m_cachedSurfacePendingSize = 0;
    m_generationQueueClassCounts = {};
    m_uploadQueueClassCounts = {};
    m_surfaceQueueClassCounts = {};
    m_deferredDirtyAfterUpload.clear();
    m_renderDirtyRegions.clear();
    m_surfaceDirtyRegions.clear();
    m_surfaceCache.Clear();
    m_evictedBricksLastFrame = 0;
    m_emptyRequestsSkippedLastFrame = 0;
    m_surfaceBricksExtractedLastFrame = 0;
    m_surfaceEmptyUploadsSkippedLastFrame = 0;
    m_renderDirtyVoxelsQueuedLastFrame = 0;
    m_renderDirtyFullUploadsQueuedLastFrame = 0;
    m_renderDirtyUploadDeferredLastFrame = 0;
    m_renderDirtyNonResidentLastFrame = 0;

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
    m_renderDirtyVoxelsQueuedLastFrame = 0;
    m_renderDirtyFullUploadsQueuedLastFrame = 0;
    m_renderDirtyUploadDeferredLastFrame = 0;
    m_renderDirtyNonResidentLastFrame = 0;
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

void SparseVoxelWorld::MarkUploadQueueOrderDirty() {
    m_uploadQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

void SparseVoxelWorld::MarkQueueAccountingDirty() {
    m_queueClassStatsDirty = true;
}

void SparseVoxelWorld::RebuildQueueClassStats() {
    m_generationQueueClassCounts = {};
    m_uploadQueueClassCounts = {};
    m_surfaceQueueClassCounts = {};

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

    for (const BrickCoord& coord : m_generationQueue) {
        addQueueClass(coord, m_generationQueueClassCounts);
    }
    for (const BrickCoord& coord : m_uploadQueue) {
        addQueueClass(coord, m_uploadQueueClassCounts);
    }
    for (const auto& pending : m_pendingSurfaceBricks) {
        addQueueClass(pending.first, m_surfaceQueueClassCounts);
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
    m_generationQueue.push_back(coord);
    m_generationClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_generationQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

bool SparseVoxelWorld::RemoveFirstGenerationQueueCoord(const BrickCoord& coord) {
    for (auto it = m_generationQueue.begin(); it != m_generationQueue.end(); ++it) {
        if (*it == coord) {
            m_generationQueue.erase(it);
            m_generationQueuePriorityDirty = true;
            MarkQueueAccountingDirty();
            return true;
        }
    }
    return false;
}

bool SparseVoxelWorld::RemoveFirstGenerationClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass)
{
    auto& queue = m_generationClassQueues[ResidencyClassQueueIndex(residencyClass)];
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (*it == coord) {
            queue.erase(it);
            return true;
        }
    }
    return false;
}

void SparseVoxelWorld::QueueGenerationClassAliasIfRequested(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (record.state != BrickLifecycleState::Requested) {
        return;
    }
    m_generationClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_generationQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
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

    GeneratedSparseBrick brick = m_terrain.GenerateBrick(coord);
    m_edits.ApplyToGeneratedBrick(brick);
    m_generated[coord] = brick;

    if (!m_pool.MarkGeneratedCPU(coord)) {
        return false;
    }
    if (!m_pool.QueueUpload(coord)) {
        return false;
    }

    if (!HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        m_pendingSurfaceBricks[coord] = brick;
        QueueSurfaceExtractionCoord(coord);
    }
    QueueUploadCoordBack(coord);
    if (outResidencyClass) {
        *outResidencyClass = generationRecord.residencyClass;
    }
    return true;
}

void SparseVoxelWorld::QueueUploadCoordBack(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    m_uploadQueue.push_back(coord);
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    MarkUploadQueueOrderDirty();
}

void SparseVoxelWorld::QueueUploadCoordFront(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    m_uploadQueue.push_front(coord);
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_front(coord);
    MarkUploadQueueOrderDirty();
}

bool SparseVoxelWorld::RemoveFirstUploadQueueCoord(const BrickCoord& coord) {
    for (auto it = m_uploadQueue.begin(); it != m_uploadQueue.end(); ++it) {
        if (*it == coord) {
            m_uploadQueue.erase(it);
            MarkUploadQueueOrderDirty();
            return true;
        }
    }
    return false;
}

bool SparseVoxelWorld::RemoveFirstUploadClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass)
{
    auto& queue = m_uploadClassQueues[ResidencyClassQueueIndex(residencyClass)];
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (*it == coord) {
            queue.erase(it);
            return true;
        }
    }
    return false;
}

void SparseVoxelWorld::QueueUploadClassAliasIfUploadQueued(const BrickCoord& coord) {
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    if (record.state != BrickLifecycleState::UploadQueued) {
        return;
    }
    m_uploadClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
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
    }
    m_surfaceExtractionQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

bool SparseVoxelWorld::RemoveFirstSurfaceQueueCoord(const BrickCoord& coord) {
    for (auto it = m_surfaceExtractionQueue.begin(); it != m_surfaceExtractionQueue.end(); ++it) {
        if (*it == coord) {
            m_surfaceExtractionQueue.erase(it);
            m_surfaceExtractionQueuedSet.erase(coord);
            m_surfaceExtractionQueuePriorityDirty = true;
            MarkQueueAccountingDirty();
            return true;
        }
    }
    return false;
}

bool SparseVoxelWorld::RemoveFirstSurfaceClassQueueCoord(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass)
{
    auto& queue = m_surfaceClassQueues[ResidencyClassQueueIndex(residencyClass)];
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (*it == coord) {
            queue.erase(it);
            return true;
        }
    }
    return false;
}

void SparseVoxelWorld::QueueSurfaceClassAliasIfPending(const BrickCoord& coord) {
    if (m_pendingSurfaceBricks.find(coord) == m_pendingSurfaceBricks.end()) {
        return;
    }
    BrickResidentRecord record;
    if (!m_pool.GetRecord(coord, &record)) {
        return;
    }
    m_surfaceClassQueues[ResidencyClassQueueIndex(record.residencyClass)].push_back(coord);
    m_surfaceExtractionQueuePriorityDirty = true;
    MarkQueueAccountingDirty();
}

bool SparseVoxelWorld::ExtractSurfaceCoord(const BrickCoord& coord) {
    auto pendingIt = m_pendingSurfaceBricks.find(coord);
    if (pendingIt == m_pendingSurfaceBricks.end()) {
        m_surfaceExtractionQueuedSet.erase(coord);
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
        MarkQueueAccountingDirty();
        return false;
    }

    GeneratedSparseBrick brick = std::move(pendingIt->second);
    m_pendingSurfaceBricks.erase(pendingIt);
    m_surfaceExtractionQueuedSet.erase(coord);
    MarkQueueAccountingDirty();
    auto neighborSampler = [this](int32_t worldX, int32_t worldY, int32_t worldZ) {
        return SampleEditedOrGeneratedVoxel(worldX, worldY, worldZ);
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
    return true;
}

bool SparseVoxelWorld::RequestBrick(const BrickCoord& coord) {
    return RequestBrickDetailed(coord) != SparseBrickRequestResult::Rejected;
}

SparseBrickRequestResult SparseVoxelWorld::RequestBrickDetailed(
    const BrickCoord& coord,
    bool allowEmptyFastPath)
{
    if (m_pool.TryGetPage(coord)) {
        return SparseBrickRequestResult::AlreadyResident;
    }

    if (allowEmptyFastPath && !m_edits.HasOverlay(coord)) {
        if (m_knownEmptyGeneratedBricks.find(coord) != m_knownEmptyGeneratedBricks.end() ||
            m_terrain.IsDefinitelyEmptyBrick(coord)) {
            m_knownEmptyGeneratedBricks.insert(coord);
            ++m_emptyRequestsSkippedLastFrame;
            RefreshStats();
            return SparseBrickRequestResult::SkippedKnownEmpty;
        }
    }

    const uint32_t page = m_pool.AllocatePage(coord);
    if (page == INVALID_BRICK_PAGE) {
        return SparseBrickRequestResult::Rejected;
    }

    QueueGenerationCoordBack(coord);
    RefreshStats();
    return SparseBrickRequestResult::Allocated;
}

uint32_t SparseVoxelWorld::PumpGeneration(uint32_t maxBricks, uint32_t currentFrame) {
    uint32_t generated = 0;
    if (m_generationQueuePriorityDirty) {
        SortQueuedBricksByPriority(m_generationQueue, m_pool, currentFrame);
        m_generationQueuePriorityDirty = false;
    }

    BrickCoord coord{};
    while (generated < maxBricks &&
           PopFrontQueuedBrick(m_generationQueue, m_pool, &coord)) {

        BrickResidentRecord generationRecord;
        if (m_pool.GetRecord(coord, &generationRecord)) {
            RemoveFirstGenerationClassQueueCoord(coord, generationRecord.residencyClass);
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
    }

    RefreshStats();
    return generated;
}

uint32_t SparseVoxelWorld::PumpGenerationAround(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    uint32_t generated = 0;
    m_generationQueuePriorityDirty = false;

    BrickCoord coord{};
    const std::array<SparseResidencyClass, 4> classOrder{
        SparseResidencyClass::Edited,
        SparseResidencyClass::Collision,
        SparseResidencyClass::Visible,
        SparseResidencyClass::Speculative
    };
    for (SparseResidencyClass residencyClass : classOrder) {
        auto& classQueue = m_generationClassQueues[ResidencyClassQueueIndex(residencyClass)];
        SortQueuedBricksByValue(classQueue, m_pool, focus, currentFrame);
        while (generated < maxBricks &&
               PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
            BrickResidentRecord record;
            if (!m_pool.GetRecord(coord, &record)) {
                continue;
            }
            if (record.residencyClass != residencyClass) {
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
            } else {
                RemoveFirstGenerationQueueCoord(coord);
            }
        }
        if (generated >= maxBricks) {
            break;
        }
    }

    if (generated < maxBricks) {
        SortQueuedBricksByValue(m_generationQueue, m_pool, focus, currentFrame);
        while (generated < maxBricks &&
               PopFrontQueuedBrick(m_generationQueue, m_pool, &coord)) {
            BrickResidentRecord record;
            if (m_pool.GetRecord(coord, &record)) {
                RemoveFirstGenerationClassQueueCoord(coord, record.residencyClass);
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
        }
    }

    RefreshStats();
    return generated;
}

bool SparseVoxelWorld::PopNextUpload(SparseBrickUploadPacket* outPacket, uint32_t currentFrame) {
    if (!outPacket) {
        return false;
    }

    while (!m_uploadQueue.empty()) {
        if (m_uploadQueuePriorityDirty) {
            SortQueuedBricksByPriority(m_uploadQueue, m_pool, currentFrame);
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

    auto& classQueue = m_uploadClassQueues[ResidencyClassQueueIndex(residencyClass)];
    if (m_uploadQueuePriorityDirty) {
        SortQueuedBricksByPriority(classQueue, m_pool, currentFrame);
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

    auto& classQueue = m_uploadClassQueues[ResidencyClassQueueIndex(residencyClass)];
    SortQueuedBricksByValue(classQueue, m_pool, focus, currentFrame);
    BrickCoord coord{};
    while (PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        if (record.residencyClass != residencyClass) {
            continue;
        }

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
        const bool needsUploadSurfaceRefresh =
            emptyBrick ||
            !existingSurface ||
            m_surfaceDirtyRegions.find(packet.coord) != m_surfaceDirtyRegions.end();
        if ((!emptyBrick || existingSurface) && needsUploadSurfaceRefresh) {
            m_pendingSurfaceBricks[packet.coord] = packet.brick;
            QueueSurfaceExtractionCoord(packet.coord);
        } else if (!emptyBrick && existingSurface) {
            m_pendingSurfaceBricks.erase(packet.coord);
            MarkQueueAccountingDirty();
        } else {
            m_pendingSurfaceBricks.erase(packet.coord);
            m_surfaceDirtyRegions.erase(packet.coord);
            MarkQueueAccountingDirty();
            ++m_surfaceEmptyUploadsSkippedLastFrame;
        }
        m_generated.erase(packet.coord);
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

uint32_t SparseVoxelWorld::PumpSurfaceExtraction(uint32_t maxBricks, uint32_t currentFrame) {
    uint32_t extracted = 0;
    if (m_surfaceExtractionQueuePriorityDirty) {
        SortQueuedBricksByPriority(m_surfaceExtractionQueue, m_pool, currentFrame);
        m_surfaceExtractionQueuePriorityDirty = false;
    }
    BrickCoord coord{};
    while (extracted < maxBricks &&
           PopFrontQueuedBrick(m_surfaceExtractionQueue, m_pool, &coord)) {
        BrickResidentRecord surfaceRecord;
        if (m_pool.GetRecord(coord, &surfaceRecord)) {
            RemoveFirstSurfaceClassQueueCoord(coord, surfaceRecord.residencyClass);
        }
        if (ExtractSurfaceCoord(coord)) {
            ++extracted;
        }
    }

    m_surfaceBricksExtractedLastFrame = extracted;
    RefreshStats();
    return extracted;
}

uint32_t SparseVoxelWorld::PumpSurfaceExtractionAround(
    uint32_t maxBricks,
    const BrickCoord& focus,
    uint32_t currentFrame)
{
    uint32_t extracted = 0;
    SortQueuedBricksByValue(m_surfaceExtractionQueue, m_pool, focus, currentFrame);
    m_surfaceExtractionQueuePriorityDirty = false;

    BrickCoord coord{};
    const std::array<SparseResidencyClass, 4> classOrder{
        SparseResidencyClass::Edited,
        SparseResidencyClass::Collision,
        SparseResidencyClass::Visible,
        SparseResidencyClass::Speculative
    };
    for (SparseResidencyClass residencyClass : classOrder) {
        auto& classQueue = m_surfaceClassQueues[ResidencyClassQueueIndex(residencyClass)];
        SortQueuedBricksByValue(classQueue, m_pool, focus, currentFrame);
        while (extracted < maxBricks &&
               PopFrontQueuedBrick(classQueue, m_pool, &coord)) {
            BrickResidentRecord record;
            if (!m_pool.GetRecord(coord, &record)) {
                continue;
            }
            if (record.residencyClass != residencyClass) {
                continue;
            }
            if (ExtractSurfaceCoord(coord)) {
                RemoveFirstSurfaceQueueCoord(coord);
                ++extracted;
            } else {
                RemoveFirstSurfaceQueueCoord(coord);
            }
        }
        if (extracted >= maxBricks) {
            break;
        }
    }

    m_surfaceBricksExtractedLastFrame = extracted;
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
        int32_t score = 0;
    };

    std::vector<Candidate> candidates;
    const int32_t radiusXz = static_cast<int32_t>(keepRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(keepRadiusY);
    for (const auto& record : m_pool.Records()) {
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            record.hasPersistentEdits ||
            record.physicsActive) {
            continue;
        }

        const int32_t dx = record.coord.x - center.x;
        const int32_t dy = record.coord.y - center.y;
        const int32_t dz = record.coord.z - center.z;
        if (std::abs(dx) <= radiusXz &&
            std::abs(dy) <= radiusY &&
            std::abs(dz) <= radiusXz) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const int32_t score =
            dx * dx + dz * dz + dy * dy * 4 -
            ResidencyRetentionScore(record.residencyClass);
        candidates.push_back({record.coord, record.pageIndex, record.generation, entryIndex, score});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
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
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
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
    const int32_t radiusXz = static_cast<int32_t>(keepRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(keepRadiusY);
    for (const auto& record : m_pool.Records()) {
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            record.hasPersistentEdits ||
            record.physicsActive ||
            record.residencyClass == SparseResidencyClass::Collision ||
            record.residencyClass == SparseResidencyClass::Edited) {
            continue;
        }

        const int32_t dx = record.coord.x - center.x;
        const int32_t dy = record.coord.y - center.y;
        const int32_t dz = record.coord.z - center.z;
        if (std::abs(dx) <= radiusXz &&
            std::abs(dy) <= radiusY &&
            std::abs(dz) <= radiusXz) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const uint32_t cappedAge = std::min(age, 100000u);
        const int32_t distanceScore = dx * dx + dz * dz + dy * dy * 4;
        const int64_t classBias = record.residencyClass == SparseResidencyClass::Speculative
            ? 1'000'000ll
            : 0ll;
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            classBias +
                static_cast<int64_t>(distanceScore) +
                static_cast<int64_t>(cappedAge) * 32
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
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
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
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
    const int32_t radiusXz = static_cast<int32_t>(keepRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(keepRadiusY);
    for (const auto& record : m_pool.Records()) {
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.hasPersistentEdits ||
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

        const int32_t dx = record.coord.x - center.x;
        const int32_t dy = record.coord.y - center.y;
        const int32_t dz = record.coord.z - center.z;
        if (std::abs(dx) <= radiusXz &&
            std::abs(dy) <= radiusY &&
            std::abs(dz) <= radiusXz) {
            continue;
        }

        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const int64_t distanceScore =
            static_cast<int64_t>(dx) * dx +
            static_cast<int64_t>(dz) * dz +
            static_cast<int64_t>(dy) * dy * 4ll;
        const int64_t classBias = record.residencyClass == SparseResidencyClass::Speculative
            ? 2'000'000ll
            : 500'000ll;
        uint32_t entryIndex = UINT32_MAX;
        m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex);
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            record.residencyClass,
            classBias + distanceScore + static_cast<int64_t>(std::min(age, 100000u)) * 64ll
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
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
    const int32_t keepXz = static_cast<int32_t>(hardKeepRadiusXz);
    const int32_t keepY = static_cast<int32_t>(hardKeepRadiusY);
    const uint8_t requestRank = ResidencyRank(requestClass);
    for (const auto& record : m_pool.Records()) {
        if (record.pageIndex == INVALID_BRICK_PAGE ||
            record.state != BrickLifecycleState::Resident ||
            record.hasPersistentEdits ||
            record.physicsActive ||
            ResidencyRank(record.residencyClass) > requestRank) {
            continue;
        }

        const int32_t dx = record.coord.x - center.x;
        const int32_t dy = record.coord.y - center.y;
        const int32_t dz = record.coord.z - center.z;
        if (std::abs(dx) <= keepXz &&
            std::abs(dy) <= keepY &&
            std::abs(dz) <= keepXz) {
            continue;
        }

        uint32_t entryIndex = UINT32_MAX;
        if (!m_pool.PageTable().TryGetEntryIndex(record.coord, &entryIndex)) {
            continue;
        }

        const int32_t classPenalty = static_cast<int32_t>(ResidencyRank(record.residencyClass)) * 100000;
        const int32_t distanceScore = dx * dx + dz * dz + dy * dy * 4;
        const uint32_t latestTouch = LatestPriorityTouch(record);
        const uint32_t age = currentFrame > latestTouch ? currentFrame - latestTouch : 0u;
        const uint32_t cappedAge = std::min(age, 100000u);
        candidates.push_back({
            record.coord,
            record.pageIndex,
            record.generation,
            entryIndex,
            static_cast<int64_t>(distanceScore) +
                static_cast<int64_t>(cappedAge) * 64 -
                static_cast<int64_t>(classPenalty)
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
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
        m_pendingSurfaceBricks.erase(candidate.coord);
        m_surfaceExtractionQueuedSet.erase(candidate.coord);
        MarkQueueAccountingDirty();
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
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
    m_surfaceExtractionQueuePriorityDirty = true;
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    RefreshStats();
    return true;
}

bool SparseVoxelWorld::TouchResidencyClass(
    const BrickCoord& coord,
    SparseResidencyClass residencyClass,
    uint32_t frameIndex)
{
    if (!m_pool.TouchResidencyClass(coord, residencyClass, frameIndex)) {
        return false;
    }
    m_generationQueuePriorityDirty = true;
    MarkUploadQueueOrderDirty();
    m_surfaceExtractionQueuePriorityDirty = true;
    QueueGenerationClassAliasIfRequested(coord);
    QueueUploadClassAliasIfUploadQueued(coord);
    QueueSurfaceClassAliasIfPending(coord);
    RefreshStats();
    return true;
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
    const SparseRenderDirtyRegion& region)
{
    auto [regionIt, insertedRegion] = m_renderDirtyRegions.emplace(coord, region);
    if (!insertedRegion) {
        MergeSparseRegion(regionIt->second, region);
    }
    m_renderDirtyVoxelsQueuedLastFrame += SparseRegionVoxelCount(region);
    QueueSurfaceDirtyRegionNoStats(coord, region);
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

        GeneratedSparseBrick brick = m_terrain.GenerateBrick(dirtyCoord);
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
        queueOne({coord.x - 1, coord.y, coord.z}, SparseSurfaceLocalRegion{
            SPARSE_BRICK_SIZE - 1, region.minY, region.minZ,
            SPARSE_BRICK_SIZE - 1, region.maxY, region.maxZ}, true);
    }
    if (region.maxX == SPARSE_BRICK_SIZE - 1) {
        queueOne({coord.x + 1, coord.y, coord.z}, SparseSurfaceLocalRegion{
            0, region.minY, region.minZ,
            0, region.maxY, region.maxZ}, true);
    }
    if (region.minY == 0) {
        queueOne({coord.x, coord.y - 1, coord.z}, SparseSurfaceLocalRegion{
            region.minX, SPARSE_BRICK_SIZE - 1, region.minZ,
            region.maxX, SPARSE_BRICK_SIZE - 1, region.maxZ}, true);
    }
    if (region.maxY == SPARSE_BRICK_SIZE - 1) {
        queueOne({coord.x, coord.y + 1, coord.z}, SparseSurfaceLocalRegion{
            region.minX, 0, region.minZ,
            region.maxX, 0, region.maxZ}, true);
    }
    if (region.minZ == 0) {
        queueOne({coord.x, coord.y, coord.z - 1}, SparseSurfaceLocalRegion{
            region.minX, region.minY, SPARSE_BRICK_SIZE - 1,
            region.maxX, region.maxY, SPARSE_BRICK_SIZE - 1}, true);
    }
    if (region.maxZ == SPARSE_BRICK_SIZE - 1) {
        queueOne({coord.x, coord.y, coord.z + 1}, SparseSurfaceLocalRegion{
            region.minX, region.minY, 0,
            region.maxX, region.maxY, 0}, true);
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
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            // Removing support only wakes the immediately supported column
            // above the edit. Queue exact voxels instead of full bricks so
            // erase/brush strokes do not turn into broad local scans.
            QueuePhysicsVoxelNoStats(
                worldX + dx,
                worldY + 1,
                worldZ + dz,
                SparsePhysicsPriority::Hot);
        }
    }
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
    m_physicsStagedPackets.reserve(maxPackets);
    m_physicsHotWorkPacketsLastFrame = 0;
    m_physicsWarmWorkPacketsLastFrame = 0;
    m_physicsDirtyRegionVoxelsLastFrame = 0;
    ++m_physicsWorkGeneration;

    SparsePhysicsWorkPacket packet;
    while (m_physicsStagedPackets.size() < maxPackets && PopNextPhysicsWorkPacket(&packet)) {
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

        const BrickCoord belowCoord{packet.coord.x, packet.coord.y - 1, packet.coord.z};
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
        if (regionMin.y == 0) {
            addCoord(BrickCoord{packet.coord.x, packet.coord.y - 1, packet.coord.z});
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
    if (maxVoxelMoves == 0 || m_physicsStagedPackets.empty()) {
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
            candidates.push_back(Candidate{
                coord.x * SPARSE_BRICK_SIZE + static_cast<int32_t>(local.x),
                coord.y * SPARSE_BRICK_SIZE + static_cast<int32_t>(local.y),
                coord.z * SPARSE_BRICK_SIZE + static_cast<int32_t>(local.z),
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

            const int32_t belowY = candidate.y - 1;
            uint32_t belowVoxel = 0;
            if (!m_edits.TryGetVoxel(candidate.x, belowY, candidate.z, &belowVoxel)) {
                belowVoxel = m_terrain.SampleGeneratedVoxel(candidate.x, belowY, candidate.z);
            }
            if (Utils::UnpackMaterial(belowVoxel) != Utils::Material::Air) {
                continue;
            }

            m_edits.SetVoxel(candidate.x, candidate.y, candidate.z, Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));
            m_edits.SetVoxel(candidate.x, belowY, candidate.z, candidate.voxel);
            const BrickCoord fromCoord = BrickCoord::FromWorldVoxel(candidate.x, candidate.y, candidate.z);
            const BrickCoord toCoord = BrickCoord::FromWorldVoxel(candidate.x, belowY, candidate.z);
            markTouchedRenderVoxel(candidate.x, candidate.y, candidate.z);
            markTouchedRenderVoxel(candidate.x, belowY, candidate.z);
            m_knownEmptyGeneratedBricks.erase(fromCoord);
            m_knownEmptyGeneratedBricks.erase(toCoord);
            ++moved;
            requeueSourceBrick = true;
            QueuePhysicsVoxelNoStats(candidate.x, belowY, candidate.z, SparsePhysicsPriority::Hot);
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
        m_surfaceExtractionQueuePriorityDirty = true;
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
    if (maxVoxelMoves == 0 || proposals.empty()) {
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
    auto requeueProposalSource = [this](const SparsePhysicsPacketResult& proposal) {
        const LocalVoxelCoord sourceLocal = UnpackPhysicsRegionPoint(proposal.packedSourceLocal);
        SparsePhysicsDirtyRegion deferredRegion;
        deferredRegion.minX = deferredRegion.maxX = sourceLocal.x;
        deferredRegion.minY = deferredRegion.maxY = sourceLocal.y;
        deferredRegion.minZ = deferredRegion.maxZ = sourceLocal.z;
        QueuePhysicsRegionNoStats(proposal.coord, deferredRegion, SparsePhysicsPriority::Hot);
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

        const bool hasExpectedPage =
            proposal.expectedPageIndex != INVALID_BRICK_PAGE &&
            proposal.expectedPageGeneration != 0u;
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
        const int32_t sourceX = SparseWorldVoxelFromLocal(proposal.coord.x, sourceLocal.x);
        const int32_t sourceY = SparseWorldVoxelFromLocal(proposal.coord.y, sourceLocal.y);
        const int32_t sourceZ = SparseWorldVoxelFromLocal(proposal.coord.z, sourceLocal.z);
        const int32_t destinationX =
            SparseWorldVoxelFromLocal(proposal.destinationCoord.x, destinationLocal.x);
        const int32_t destinationY =
            SparseWorldVoxelFromLocal(proposal.destinationCoord.y, destinationLocal.y);
        const int32_t destinationZ =
            SparseWorldVoxelFromLocal(proposal.destinationCoord.z, destinationLocal.z);
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
            if ((proposal.sourceRevision != 0u &&
                 currentSourceRevision > proposal.sourceRevision) ||
                (proposal.destinationRevision != 0u &&
                 currentDestinationRevision > proposal.destinationRevision)) {
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
        if (Utils::UnpackMaterial(currentDestinationVoxel) != Utils::Material::Air) {
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
        m_surfaceExtractionQueuePriorityDirty = true;
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
    m_surfaceExtractionQueuePriorityDirty = true;
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
    if (commit) {
        m_stats.brushVoxelsEvaluatedLastStroke = 0;
        m_stats.brushVoxelsEditedLastStroke = 0;
        m_stats.brushBricksTouchedLastStroke = 0;
        m_stats.brushBricksQueuedLastStroke = 0;
    }

    if (radius <= 0.0f) {
        if (commit) {
            RefreshStats();
        }
        return 0;
    }

    const int32_t radiusCeil = static_cast<int32_t>(std::ceil(radius)) + 2;
    const int32_t centerX = static_cast<int32_t>(std::floor(worldPositionX));
    const int32_t centerY = static_cast<int32_t>(std::floor(worldPositionY));
    const int32_t centerZ = static_cast<int32_t>(std::floor(worldPositionZ));
    const int32_t startX = centerX - radiusCeil;
    const int32_t startY = centerY - radiusCeil;
    const int32_t startZ = centerZ - radiusCeil;
    const int32_t endX = static_cast<int32_t>(std::ceil(worldPositionX)) + radiusCeil + 1;
    const int32_t endY = static_cast<int32_t>(std::ceil(worldPositionY)) + radiusCeil + 1;
    const int32_t endZ = static_cast<int32_t>(std::ceil(worldPositionZ)) + radiusCeil + 1;

    std::unordered_set<BrickCoord, BrickCoordHash> touchedBricks;
    std::unordered_map<BrickCoord, SparsePhysicsDirtyRegion, BrickCoordHash> touchedPhysicsRegions;
    uint32_t edited = 0;
    uint32_t evaluated = 0;

    const float normalX = static_cast<float>(hitNormalX);
    const float normalY = static_cast<float>(hitNormalY);
    const float normalZ = static_cast<float>(hitNormalZ);

    for (int32_t z = startZ; z < endZ; ++z) {
        for (int32_t y = startY; y < endY; ++y) {
            for (int32_t x = startX; x < endX; ++x) {
                ++evaluated;

                const float sdf = BrushSdf(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f,
                    static_cast<float>(z) + 0.5f,
                    worldPositionX,
                    worldPositionY,
                    worldPositionZ,
                    radius,
                    shape);
                if (sdf > 0.5f || y <= TERRAIN_MIN_Y + 5) {
                    continue;
                }

                const uint8_t variant = HashVoxelVariant(x, y, z, seed);
                if (strength < 1.0f && sdf > -0.5f) {
                    const float edgeFactor = std::clamp(1.0f - sdf / 0.5f, 0.0f, 1.0f);
                    const float probability = edgeFactor * strength;
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
                    currentVoxel = m_terrain.SampleGeneratedVoxel(x, y, z);
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

                if (newVoxel == currentVoxel) {
                    continue;
                }

                const BrickCoord editCoord = BrickCoord::FromWorldVoxel(x, y, z);
                const LocalVoxelCoord editLocal = LocalVoxelFromWorld(x, y, z);
                if (commit) {
                    m_edits.SetVoxel(x, y, z, newVoxel);
                }
                if (outDeltas) {
                    outDeltas->push_back({
                        editCoord,
                        PackSparseEditLocal(editLocal),
                        newVoxel,
                        commit
                            ? m_edits.GetOverlayRevision(editCoord)
                            : m_edits.GetOverlayRevision(editCoord) + 1u
                    });
                }
                if (commit && Utils::UnpackMaterial(newVoxel) == Utils::Material::Air) {
                    WakePhysicsSupportNeighborhoodNoStats(x, y, z);
                }
                touchedBricks.insert(editCoord);
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
            if (QueueRegeneratedUploadForExistingPage(coord, &renderRegion)) {
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
    if (maxDistance <= 0.0f) {
        return hit;
    }

    const float dirLength = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
    if (dirLength <= 0.00001f) {
        return hit;
    }
    dirX /= dirLength;
    dirY /= dirLength;
    dirZ /= dirLength;

    int32_t voxelX = static_cast<int32_t>(std::floor(originX));
    int32_t voxelY = static_cast<int32_t>(std::floor(originY));
    int32_t voxelZ = static_cast<int32_t>(std::floor(originZ));

    const int32_t stepX = dirX > 0.0f ? 1 : (dirX < 0.0f ? -1 : 0);
    const int32_t stepY = dirY > 0.0f ? 1 : (dirY < 0.0f ? -1 : 0);
    const int32_t stepZ = dirZ > 0.0f ? 1 : (dirZ < 0.0f ? -1 : 0);

    const float inf = std::numeric_limits<float>::infinity();
    const float tDeltaX = stepX != 0 ? std::abs(1.0f / dirX) : inf;
    const float tDeltaY = stepY != 0 ? std::abs(1.0f / dirY) : inf;
    const float tDeltaZ = stepZ != 0 ? std::abs(1.0f / dirZ) : inf;

    auto firstBoundaryT = [](float origin, float dir, int32_t voxel, int32_t step) {
        if (step > 0) {
            return (static_cast<float>(voxel + 1) - origin) / dir;
        }
        if (step < 0) {
            return (origin - static_cast<float>(voxel)) / -dir;
        }
        return std::numeric_limits<float>::infinity();
    };

    float tMaxX = firstBoundaryT(originX, dirX, voxelX, stepX);
    float tMaxY = firstBoundaryT(originY, dirY, voxelY, stepY);
    float tMaxZ = firstBoundaryT(originZ, dirZ, voxelZ, stepZ);
    tMaxX = std::max(tMaxX, 0.0f);
    tMaxY = std::max(tMaxY, 0.0f);
    tMaxZ = std::max(tMaxZ, 0.0f);

    SparseCollisionQuery query(m_terrain, &m_edits);
    float traveled = 0.0f;
    int32_t normalX = 0;
    int32_t normalY = 0;
    int32_t normalZ = 0;
    const uint32_t maxSteps = static_cast<uint32_t>(std::ceil(maxDistance * 3.0f)) + 16u;
    for (uint32_t step = 0; step < maxSteps && traveled <= maxDistance; ++step) {
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
            hit.distance = traveled;
            hit.voxel = sample.voxel;
            hit.fromEdit = sample.fromEdit;
            return hit;
        }

        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            voxelX += stepX;
            traveled = tMaxX;
            tMaxX += tDeltaX;
            normalX = -stepX;
            normalY = 0;
            normalZ = 0;
        } else if (tMaxY <= tMaxZ) {
            voxelY += stepY;
            traveled = tMaxY;
            tMaxY += tDeltaY;
            normalX = 0;
            normalY = -stepY;
            normalZ = 0;
        } else {
            voxelZ += stepZ;
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
    m_stats.generatedBricks = static_cast<uint32_t>(m_generated.size());
    m_stats.generatedSpeculativeBricksLastFrame = m_generatedSpeculativeBricksLastFrame;
    m_stats.generatedVisibleBricksLastFrame = m_generatedVisibleBricksLastFrame;
    m_stats.generatedCollisionBricksLastFrame = m_generatedCollisionBricksLastFrame;
    m_stats.generatedEditedBricksLastFrame = m_generatedEditedBricksLastFrame;
    m_stats.uploadQueuedBricks = static_cast<uint32_t>(m_uploadQueue.size());
    m_stats.uploadQueuedSpeculativeBricks = m_uploadQueueClassCounts.speculative;
    m_stats.uploadQueuedVisibleBricks = m_uploadQueueClassCounts.visible;
    m_stats.uploadQueuedCollisionBricks = m_uploadQueueClassCounts.collision;
    m_stats.uploadQueuedEditedBricks = m_uploadQueueClassCounts.edited;
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
    m_stats.surfaceBricksExtractedLastFrame = m_surfaceBricksExtractedLastFrame;
    m_stats.surfaceSpeculativeBricksExtractedLastFrame = m_surfaceSpeculativeBricksExtractedLastFrame;
    m_stats.surfaceVisibleBricksExtractedLastFrame = m_surfaceVisibleBricksExtractedLastFrame;
    m_stats.surfaceCollisionBricksExtractedLastFrame = m_surfaceCollisionBricksExtractedLastFrame;
    m_stats.surfaceEditedBricksExtractedLastFrame = m_surfaceEditedBricksExtractedLastFrame;
    m_stats.surfaceEmptyUploadsSkippedLastFrame = m_surfaceEmptyUploadsSkippedLastFrame;
    m_stats.surfaceEmptyFastPathBricksLastFrame = m_surfaceCache.GetStats().emptyFastPathBricksLastFrame;
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
    uint32_t editedVoxel = 0;
    if (m_edits.TryGetVoxel(worldX, worldY, worldZ, &editedVoxel)) {
        return editedVoxel;
    }
    return m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
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
    const SparseRenderDirtyRegion* dirtyRegion)
{
    if (dirtyRegion) {
        QueueRenderDirtyRegionNoStats(coord, *dirtyRegion);
    } else if (m_renderDirtyRegions.find(coord) == m_renderDirtyRegions.end()) {
        SparseRenderDirtyRegion fullRegion;
        QueueRenderDirtyRegionNoStats(coord, fullRegion);
    }

    if (!m_pool.TryGetPage(coord)) {
        ++m_renderDirtyNonResidentLastFrame;
        return false;
    }

    m_pool.MarkHasPersistentEdits(coord);
    MarkResidencyClass(coord, SparseResidencyClass::Edited);

    const BrickLifecycleState state = m_pool.GetState(coord);
    if (state == BrickLifecycleState::Requested || state == BrickLifecycleState::GeneratingCPU) {
        return false;
    }

    if (state == BrickLifecycleState::UploadingGPU) {
        m_deferredDirtyAfterUpload[coord] = true;
        ++m_renderDirtyUploadDeferredLastFrame;
        return false;
    }

    if (state == BrickLifecycleState::EvictQueued || state == BrickLifecycleState::Evicted ||
        state == BrickLifecycleState::Missing) {
        return false;
    }

    GeneratedSparseBrick brick = m_terrain.GenerateBrick(coord);
    m_edits.ApplyToGeneratedBrick(brick);
    m_generated[coord] = brick;

    if (state == BrickLifecycleState::UploadQueued) {
        return false;
    }

    if (state == BrickLifecycleState::Resident) {
        if (!m_pool.MarkDirty(coord)) {
            return false;
        }
    }

    if (!m_pool.QueueUpload(coord)) {
        return false;
    }
    QueueUploadCoordBack(coord);
    ++m_renderDirtyFullUploadsQueuedLastFrame;
    return true;
}

} // namespace VENPOD::Simulation
