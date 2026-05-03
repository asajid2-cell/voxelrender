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

uint32_t LatestPriorityTouch(const BrickResidentRecord& record) {
    uint32_t latest = record.lastTouchedFrame;
    latest = std::max(latest, record.lastSpeculativeFrame);
    latest = std::max(latest, record.lastVisibleFrame);
    latest = std::max(latest, record.lastCollisionFrame);
    latest = std::max(latest, record.lastEditedFrame);
    return latest;
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

bool PopHighestPriorityQueuedBrick(
    std::deque<BrickCoord>& queue,
    const SparseBrickPool& pool,
    BrickCoord* outCoord,
    uint32_t currentFrame)
{
    if (!outCoord || queue.empty()) {
        return false;
    }

    auto bestIt = queue.end();
    int64_t bestScore = std::numeric_limits<int64_t>::min();
    for (auto it = queue.begin(); it != queue.end();) {
        BrickResidentRecord record;
        if (!pool.GetRecord(*it, &record)) {
            it = queue.erase(it);
            continue;
        }

        const int64_t score = QueuePriorityScore(record, currentFrame);
        if (bestIt == queue.end() || score > bestScore) {
            bestIt = it;
            bestScore = score;
        }
        ++it;
    }

    if (bestIt == queue.end()) {
        return false;
    }

    *outCoord = *bestIt;
    queue.erase(bestIt);
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
    m_generationQueue.clear();
    m_uploadQueue.clear();
    m_invalidationQueue.clear();
    m_deferredDirtyAfterUpload.clear();
    m_surfaceCache.Clear();
    m_evictedBricksLastFrame = 0;

    if (!m_pool.Initialize(config.maxBrickPages, config.pageTableCapacity)) {
        return false;
    }

    RefreshStats();
    return true;
}

void SparseVoxelWorld::BeginFrame() {
    m_evictedBricksLastFrame = 0;
    m_surfaceCache.BeginFrame();
    RefreshStats();
}

bool SparseVoxelWorld::RequestBrick(const BrickCoord& coord) {
    if (m_pool.TryGetPage(coord)) {
        return true;
    }

    const uint32_t page = m_pool.AllocatePage(coord);
    if (page == INVALID_BRICK_PAGE) {
        return false;
    }

    m_generationQueue.push_back(coord);
    RefreshStats();
    return true;
}

uint32_t SparseVoxelWorld::PumpGeneration(uint32_t maxBricks, uint32_t currentFrame) {
    uint32_t generated = 0;

    BrickCoord coord{};
    while (generated < maxBricks &&
           PopHighestPriorityQueuedBrick(m_generationQueue, m_pool, &coord, currentFrame)) {

        if (!m_pool.MarkGeneratingCPU(coord)) {
            continue;
        }

        GeneratedSparseBrick brick = m_terrain.GenerateBrick(coord);
        m_edits.ApplyToGeneratedBrick(brick);
        m_generated[coord] = brick;

        if (!m_pool.MarkGeneratedCPU(coord)) {
            continue;
        }
        if (!m_pool.QueueUpload(coord)) {
            continue;
        }

        m_uploadQueue.push_back(coord);
        ++generated;
    }

    RefreshStats();
    return generated;
}

bool SparseVoxelWorld::PopNextUpload(SparseBrickUploadPacket* outPacket) {
    if (!outPacket) {
        return false;
    }

    while (!m_uploadQueue.empty()) {
        BrickCoord coord{};
        if (!PopHighestPriorityQueuedBrick(m_uploadQueue, m_pool, &coord, 0)) {
            break;
        }

        auto generatedIt = m_generated.find(coord);
        if (generatedIt == m_generated.end()) {
            continue;
        }

        BrickResidentRecord record;
        if (!m_pool.GetRecord(coord, &record)) {
            continue;
        }
        if (!m_pool.BeginUpload(coord)) {
            continue;
        }

        outPacket->coord = coord;
        outPacket->pageIndex = record.pageIndex;
        outPacket->generation = record.generation;
        outPacket->brick = generatedIt->second;
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
    m_uploadQueue.push_front(packet.coord);
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
        m_surfaceCache.UpdateBrick(
            packet.brick,
            [this](int32_t worldX, int32_t worldY, int32_t worldZ) {
                return SampleEditedOrGeneratedVoxel(worldX, worldY, worldZ);
            });
        m_generated.erase(packet.coord);
        auto deferredIt = m_deferredDirtyAfterUpload.find(packet.coord);
        if (deferredIt != m_deferredDirtyAfterUpload.end()) {
            m_deferredDirtyAfterUpload.erase(deferredIt);
            QueueRegeneratedUploadForExistingPage(packet.coord);
        }
    }

    RefreshStats();
    return published;
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
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
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
        m_surfaceCache.RemoveBrick(candidate.coord);
        m_generated.erase(candidate.coord);
        m_deferredDirtyAfterUpload.erase(candidate.coord);
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
    RefreshStats();
    return true;
}

void SparseVoxelWorld::SetEditedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
    m_edits.SetVoxel(worldX, worldY, worldZ, packedVoxel);
    const BrickCoord coord = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    m_pool.MarkResidencyClass(coord, SparseResidencyClass::Edited);
    QueueRegeneratedUploadForExistingPage(coord);
    RefreshStats();
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
    bool requestRenderBricks)
{
    m_stats.brushVoxelsEvaluatedLastStroke = 0;
    m_stats.brushVoxelsEditedLastStroke = 0;
    m_stats.brushBricksTouchedLastStroke = 0;
    m_stats.brushBricksQueuedLastStroke = 0;

    if (radius <= 0.0f) {
        RefreshStats();
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

                m_edits.SetVoxel(x, y, z, newVoxel);
                touchedBricks.insert(BrickCoord::FromWorldVoxel(x, y, z));
                ++edited;
            }
        }
    }

    uint32_t queued = 0;
    for (const BrickCoord& coord : touchedBricks) {
        if (requestRenderBricks) {
            RequestBrick(coord);
        }
        m_pool.MarkResidencyClass(coord, SparseResidencyClass::Edited);
        if (QueueRegeneratedUploadForExistingPage(coord)) {
            ++queued;
        }
    }

    m_stats.brushVoxelsEvaluatedLastStroke = evaluated;
    m_stats.brushVoxelsEditedLastStroke = edited;
    m_stats.brushBricksTouchedLastStroke = static_cast<uint32_t>(touchedBricks.size());
    m_stats.brushBricksQueuedLastStroke = queued;
    RefreshStats();
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
    m_stats.requestedBricks = m_pool.ResidentCount();
    m_stats.generationQueuedBricks = static_cast<uint32_t>(m_generationQueue.size());
    m_stats.generatedBricks = static_cast<uint32_t>(m_generated.size());
    m_stats.uploadQueuedBricks = static_cast<uint32_t>(m_uploadQueue.size());
    m_stats.residentBricks = 0;
    m_stats.freePages = m_pool.FreePageCount();
    m_stats.residentSpeculativeBricks = 0;
    m_stats.residentVisibleBricks = 0;
    m_stats.residentCollisionBricks = 0;
    m_stats.residentEditedBricks = 0;
    m_stats.evictionQueuedBricks = static_cast<uint32_t>(m_invalidationQueue.size());
    m_stats.evictedBricksLastFrame = m_evictedBricksLastFrame;
    m_stats.editedBricks = static_cast<uint32_t>(m_edits.EditedBrickCount());
    m_stats.editedVoxels = static_cast<uint32_t>(m_edits.EditedVoxelCount());
    m_stats.surfaceCachedBricks = m_surfaceCache.GetStats().cachedBricks;
    m_stats.surfaceFaces = m_surfaceCache.GetStats().totalFaces;
    m_stats.surfaceFacesGeneratedLastFrame = m_surfaceCache.GetStats().facesGeneratedLastUpdate;
    m_stats.surfaceBricksUpdatedLastFrame = m_surfaceCache.GetStats().bricksUpdatedLastFrame;
    m_stats.surfaceBricksRemovedLastFrame = m_surfaceCache.GetStats().bricksRemovedLastFrame;
    m_stats.surfaceSerial = m_surfaceCache.GetStats().serial;

    // The pool's public count tracks allocated records. Count only published
    // resident records by probing the page table through TryGetResidentPage.
    for (const auto& entry : m_pool.PageTable().Entries()) {
        if (entry.pageIndex != INVALID_BRICK_PAGE &&
            entry.pageIndex != INVALID_BRICK_PAGE - 1u) {
            ++m_stats.residentBricks;
        }
    }
    for (const auto& record : m_pool.Records()) {
        if (record.state != BrickLifecycleState::Resident) {
            continue;
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

bool SparseVoxelWorld::QueueRegeneratedUploadForExistingPage(const BrickCoord& coord) {
    if (!m_pool.TryGetPage(coord)) {
        return false;
    }

    m_pool.MarkHasPersistentEdits(coord);
    m_pool.MarkResidencyClass(coord, SparseResidencyClass::Edited);

    const BrickLifecycleState state = m_pool.GetState(coord);
    if (state == BrickLifecycleState::Requested || state == BrickLifecycleState::GeneratingCPU) {
        return false;
    }

    if (state == BrickLifecycleState::UploadingGPU) {
        m_deferredDirtyAfterUpload[coord] = true;
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
    m_uploadQueue.push_back(coord);
    return true;
}

} // namespace VENPOD::Simulation
