#include "SparseClipmap.h"

#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VENPOD::Simulation {

namespace {

uint32_t NextPowerOfTwo(uint32_t value) {
    if (value <= 1u) {
        return 1u;
    }
    --value;
    value |= value >> 1u;
    value |= value >> 2u;
    value |= value >> 4u;
    value |= value >> 8u;
    value |= value >> 16u;
    return value + 1u;
}

uint32_t HashClipmapTileCoord(const SparseClipmapTileCoord& coord) {
    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(coord.ring)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.z)) * 16777619u;
    return hash;
}

uint32_t HashVoxelClipmapCoord(const SparseVoxelClipmapCoord& coord) {
    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(coord.ring)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.y)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.z)) * 16777619u;
    return hash;
}

struct GridDdaAxis {
    int32_t step = 0;
    float nextT = std::numeric_limits<float>::infinity();
    float deltaT = std::numeric_limits<float>::infinity();
};

GridDdaAxis BuildGridDdaAxis(float origin, float direction, float cellSize, int32_t cellCoord) {
    GridDdaAxis axis;
    if (std::abs(direction) <= 0.00001f) {
        return axis;
    }
    axis.step = direction > 0.0f ? 1 : -1;
    const float boundary = axis.step > 0
        ? static_cast<float>(cellCoord + 1) * cellSize
        : static_cast<float>(cellCoord) * cellSize;
    axis.nextT = (boundary - origin) / direction;
    if (!std::isfinite(axis.nextT) || axis.nextT < 0.0f) {
        axis.nextT = 0.0f;
    }
    axis.deltaT = cellSize / std::abs(direction);
    return axis;
}

std::vector<SparseClipmapTileCoord> BuildTileLine2D(
    int32_t ring,
    float startX,
    float startZ,
    float endX,
    float endZ,
    float cellSize,
    uint32_t maxCoords)
{
    std::vector<SparseClipmapTileCoord> coords;
    if (maxCoords == 0 || cellSize <= 0.0f) {
        return coords;
    }

    const float dx = endX - startX;
    const float dz = endZ - startZ;
    const float length = std::sqrt(dx * dx + dz * dz);
    int32_t x = static_cast<int32_t>(std::floor(startX / cellSize));
    int32_t z = static_cast<int32_t>(std::floor(startZ / cellSize));
    coords.push_back({ring, x, z});
    if (!std::isfinite(length) || length <= 0.0001f || maxCoords == 1u) {
        return coords;
    }

    const float invLength = 1.0f / length;
    const float dirX = dx * invLength;
    const float dirZ = dz * invLength;
    GridDdaAxis axisX = BuildGridDdaAxis(startX, dirX, cellSize, x);
    GridDdaAxis axisZ = BuildGridDdaAxis(startZ, dirZ, cellSize, z);
    float distance = 0.0f;
    const uint32_t maxDdaSteps = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(length / cellSize)) * 4u + 8u,
        1u,
        std::max<uint32_t>(maxCoords * 4u, 8u));
    for (uint32_t stepIndex = 0;
         distance <= length && stepIndex < maxDdaSteps && coords.size() < maxCoords;
         ++stepIndex) {
        const float nextDistance = std::min(axisX.nextT, axisZ.nextT);
        if (!std::isfinite(nextDistance) || nextDistance > length) {
            break;
        }
        const float tieEpsilon = 0.0005f;
        if (axisX.nextT <= nextDistance + tieEpsilon) {
            x += axisX.step;
            axisX.nextT += axisX.deltaT;
        }
        if (axisZ.nextT <= nextDistance + tieEpsilon) {
            z += axisZ.step;
            axisZ.nextT += axisZ.deltaT;
        }
        distance = std::max(nextDistance + 0.0001f, distance + 0.0001f);
        const SparseClipmapTileCoord coord{ring, x, z};
        if (coords.empty() || !(coords.back() == coord)) {
            coords.push_back(coord);
        }
    }
    return coords;
}

}

SparseClipmapPolicy::SparseClipmapPolicy(const SparseClipmapConfig& config)
    : m_config(config)
{
    m_config.startDistance = std::max(0.0f, m_config.startDistance);
    m_config.endDistance = std::max(m_config.startDistance + 1.0f, m_config.endDistance);
    m_config.minCellSize = std::max(4.0f, m_config.minCellSize);
    m_config.nearExitPadding = std::max(0.0f, m_config.nearExitPadding);
    m_config.ringCount = std::clamp<uint32_t>(m_config.ringCount, 1u, 8u);
    m_config.tileRadius = std::clamp<uint32_t>(m_config.tileRadius, 1u, 8u);
    m_config.tileSampleSide = std::clamp<uint32_t>(m_config.tileSampleSide, 9u, 65u);
    m_config.maxTiles = std::max(m_config.maxTiles, m_config.ringCount);
    m_config.voxelBrickRadiusXz = std::clamp<uint32_t>(m_config.voxelBrickRadiusXz, 1u, 8u);
    m_config.voxelBrickRadiusY = std::clamp<uint32_t>(m_config.voxelBrickRadiusY, 0u, 4u);
    m_config.maxVoxelBricks = std::max(m_config.maxVoxelBricks, m_config.ringCount);
    m_config.voxelInterestCapacityPercent = std::clamp<uint32_t>(
        m_config.voxelInterestCapacityPercent,
        25u,
        100u);
    m_config.motionLookaheadMinSpeed = std::max(0.0f, m_config.motionLookaheadMinSpeed);
    m_config.motionLookaheadSteps = std::clamp<uint32_t>(m_config.motionLookaheadSteps, 1u, 8u);
}

bool SparseClipmapPolicy::IsEnabled() const {
    return m_config.enabled && m_config.endDistance > m_config.startDistance;
}

float SparseClipmapPolicy::TransitionStartAfterNearExit(float nearExitDistance) const {
    if (!IsEnabled()) {
        return m_config.endDistance;
    }
    return std::max(m_config.startDistance, nearExitDistance + m_config.nearExitPadding);
}

float SparseClipmapPolicy::BackgroundStartAfterNearVolumeExit(float nearVolumeExitDistance) const {
    if (!IsEnabled()) {
        return std::max(0.0f, nearVolumeExitDistance);
    }
    return std::max(m_config.startDistance, nearVolumeExitDistance + m_config.nearExitPadding);
}

float SparseClipmapPolicy::FarLayerStartAfterBackground(float backgroundStartDistance) const {
    if (!IsEnabled()) {
        return std::max(0.0f, backgroundStartDistance);
    }

    const float span = m_config.endDistance - m_config.startDistance;
    const float handoffDistance = m_config.startDistance + span * 0.62f;
    // Far layers are continuity behind the mid hierarchy, not a replacement
    // for missing mid/near data. Clamp inside the clipmap range so unusually
    // large near volumes can still push the handoff later.
    return std::max(backgroundStartDistance, std::min(m_config.endDistance, handoffDistance));
}

float SparseClipmapPolicy::MissingNearPageBackgroundStart(
    float firstMissingDistance,
    float nearVolumeExitDistance,
    float missingPagePadding) const
{
    const float paddedMissingDistance =
        std::max(0.0f, firstMissingDistance) + std::max(0.0f, missingPagePadding);
    return std::max(paddedMissingDistance, BackgroundStartAfterNearVolumeExit(nearVolumeExitDistance));
}

bool SparseClipmapPolicy::AllowsBackgroundForMissingNearPage(
    float firstMissingDistance,
    float nearVolumeExitDistance) const
{
    return firstMissingDistance >= MissingNearPageBackgroundStart(firstMissingDistance, nearVolumeExitDistance, 0.0f);
}

bool SparseClipmapPolicy::OwnsRaySegment(
    float segmentStartDistance,
    float segmentEndDistance,
    float nearExitDistance) const
{
    if (!IsEnabled() || segmentEndDistance <= segmentStartDistance) {
        return false;
    }

    const float transitionStart = TransitionStartAfterNearExit(nearExitDistance);
    return segmentEndDistance >= transitionStart && segmentStartDistance <= m_config.endDistance;
}

float SparseClipmapPolicy::CellSizeForDistance(float distanceFromCamera) const {
    if (!IsEnabled()) {
        return m_config.minCellSize;
    }

    const float span = std::max(1.0f, m_config.endDistance - m_config.startDistance);
    const float t = std::clamp((distanceFromCamera - m_config.startDistance) / span, 0.0f, 0.9999f);
    const uint32_t ring = std::min(
        static_cast<uint32_t>(std::floor(t * static_cast<float>(m_config.ringCount))),
        m_config.ringCount - 1u);
    return m_config.minCellSize * static_cast<float>(1u << ring);
}

std::vector<SparseClipmapRing> SparseClipmapPolicy::BuildRings() const {
    std::vector<SparseClipmapRing> rings;
    if (!IsEnabled()) {
        return rings;
    }

    rings.reserve(m_config.ringCount);
    const float span = m_config.endDistance - m_config.startDistance;
    for (uint32_t ring = 0; ring < m_config.ringCount; ++ring) {
        const float startT = static_cast<float>(ring) / static_cast<float>(m_config.ringCount);
        const float endT = static_cast<float>(ring + 1u) / static_cast<float>(m_config.ringCount);
        rings.push_back({
            m_config.startDistance + span * startT,
            m_config.startDistance + span * endT,
            m_config.minCellSize * static_cast<float>(1u << ring)
        });
    }
    return rings;
}

SparseClipmapTransitionMetadata SparseClipmapPolicy::BuildTransitionMetadata() const {
    SparseClipmapTransitionMetadata metadata;
    metadata.startDistance = m_config.startDistance;
    metadata.endDistance = m_config.endDistance;
    metadata.minCellSize = m_config.minCellSize;
    metadata.enabled = IsEnabled();
    metadata.farHandoffDistance = metadata.enabled
        ? FarLayerStartAfterBackground(m_config.startDistance)
        : std::max(0.0f, m_config.startDistance);
    return metadata;
}

SparseClipmapTransitionMetadata SparseClipmapPolicy::BuildTransitionMetadataAfterNearExit(
    float nearVolumeExitDistance) const
{
    SparseClipmapTransitionMetadata metadata;
    metadata.startDistance = BackgroundStartAfterNearVolumeExit(nearVolumeExitDistance);
    metadata.endDistance = m_config.endDistance;
    metadata.minCellSize = m_config.minCellSize;
    metadata.enabled = IsEnabled();
    metadata.farHandoffDistance = metadata.enabled
        ? FarLayerStartAfterBackground(metadata.startDistance)
        : std::max(0.0f, metadata.startDistance);
    return metadata;
}

size_t SparseClipmapTileCoordHash::operator()(const SparseClipmapTileCoord& coord) const noexcept {
    return static_cast<size_t>(HashClipmapTileCoord(coord));
}

SparseClipmapResidencyMetadata BuildClipmapResidencyMetadata(const SparseClipmapCacheStats& stats) {
    SparseClipmapResidencyMetadata metadata;
    const uint32_t heightCovered =
        stats.interestedTiles > stats.missingInterestedTiles
            ? stats.interestedTiles - stats.missingInterestedTiles
            : 0u;
    const uint32_t voxelCovered =
        stats.interestedVoxelBricks > stats.missingInterestedVoxelBricks
            ? stats.interestedVoxelBricks - stats.missingInterestedVoxelBricks
            : 0u;
    metadata.heightCoverageRatio = stats.interestedTiles > 0
        ? std::clamp(static_cast<float>(heightCovered) / static_cast<float>(stats.interestedTiles), 0.0f, 1.0f)
        : 0.0f;
    metadata.voxelCoverageRatio = stats.interestedVoxelBricks > 0
        ? std::clamp(static_cast<float>(voxelCovered) / static_cast<float>(stats.interestedVoxelBricks), 0.0f, 1.0f)
        : 0.0f;
    metadata.residentHeightTiles = stats.residentTiles;
    metadata.residentVoxelBricks = stats.residentVoxelBricks;
    return metadata;
}

size_t SparseVoxelClipmapCoordHash::operator()(const SparseVoxelClipmapCoord& coord) const noexcept {
    return static_cast<size_t>(HashVoxelClipmapCoord(coord));
}

bool SparseClipmapTileCache::Initialize(const SparseClipmapConfig& config) {
    m_config = SparseClipmapPolicy(config).Config();
    m_terrain = SparseTerrainGenerator(m_config.seed);
    m_tiles.clear();
    m_tiles.resize(m_config.maxTiles);
    m_freeSlots.clear();
    m_slotByCoord.clear();
    m_generationQueue.clear();
    m_queuedSet.clear();
    m_interestSet.clear();
    m_voxelBricks.clear();
    m_voxelBricks.resize(m_config.maxVoxelBricks);
    m_freeVoxelSlots.clear();
    m_voxelSlotByCoord.clear();
    m_voxelGenerationQueue.clear();
    m_queuedVoxelSet.clear();
    m_voxelInterestSet.clear();
    m_dirtySerial = 1;
    m_heightDirtySerial = 1;
    m_voxelDirtySerial = 1;
    m_dirtyHeightStartSlot = UINT32_MAX;
    m_dirtyHeightEndSlot = 0;
    m_dirtyVoxelStartSlot = UINT32_MAX;
    m_dirtyVoxelEndSlot = 0;

    for (uint32_t slot = 0; slot < m_config.maxTiles; ++slot) {
        m_tiles[slot].record.slot = UINT32_MAX;
        m_tiles[slot].packedSamples.clear();
        m_freeSlots.push_back(m_config.maxTiles - 1u - slot);
    }
    for (uint32_t slot = 0; slot < m_config.maxVoxelBricks; ++slot) {
        m_voxelBricks[slot].slot = UINT32_MAX;
        m_voxelBricks[slot].voxels.clear();
        m_freeVoxelSlots.push_back(m_config.maxVoxelBricks - 1u - slot);
    }

    RefreshStats();
    return true;
}

void SparseClipmapTileCache::UpdateInterest(
    float cameraX,
    float cameraY,
    float cameraZ,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    float forwardX,
    float forwardY,
    float forwardZ,
    float velocityX,
    float velocityY,
    float velocityZ,
    float predictionSeconds)
{
    if (!policy.IsEnabled() || m_tiles.empty()) {
        m_interestSet.clear();
        m_voxelInterestSet.clear();
        RefreshStats();
        m_stats.heightInterestAnchors = 0;
        m_stats.voxelInterestAnchors = 0;
        return;
    }

    m_interestSet.clear();
    const auto rings = policy.BuildRings();
    const int32_t radius = static_cast<int32_t>(policy.Config().tileRadius);
    const float tileCells = static_cast<float>(policy.Config().tileSampleSide - 1u);

    struct InterestAnchor {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        int32_t radiusBias = 0;
    };
    const float forwardLenXz = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
    const float invForwardLenXz = forwardLenXz > 0.001f ? 1.0f / forwardLenXz : 0.0f;
    const float forwardNormX = forwardX * invForwardLenXz;
    const float forwardNormZ = forwardZ * invForwardLenXz;
    const float predictedX = cameraX + velocityX * std::max(0.0f, predictionSeconds);
    const float predictedY = cameraY + velocityY * std::max(0.0f, predictionSeconds);
    const float predictedZ = cameraZ + velocityZ * std::max(0.0f, predictionSeconds);
    const float velocityLenXz = std::sqrt(velocityX * velocityX + velocityZ * velocityZ);
    const bool useMotionLookahead =
        predictionSeconds > 0.0f &&
        velocityLenXz >= policy.Config().motionLookaheadMinSpeed;
    uint32_t heightAnchorCount = 0;
    for (uint32_t ring = 0; ring < rings.size(); ++ring) {
        const float tileWorldSize = std::max(1.0f, rings[ring].cellSize * tileCells);
        std::vector<InterestAnchor> anchors;
        anchors.reserve(3u + policy.Config().motionLookaheadSteps);
        anchors.push_back({cameraX, cameraY, cameraZ, 0});
        if (useMotionLookahead) {
            const uint32_t steps = std::max(1u, policy.Config().motionLookaheadSteps);
            for (uint32_t step = 1u; step <= steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                anchors.push_back({
                    cameraX + (predictedX - cameraX) * t,
                    cameraY + (predictedY - cameraY) * t,
                    cameraZ + (predictedZ - cameraZ) * t,
                    -radius
                });
            }
        }
        anchors.push_back(
            {
                cameraX + forwardNormX * tileWorldSize * std::max(1.0f, static_cast<float>(radius)),
                cameraY + forwardY * tileWorldSize * 0.25f,
                cameraZ + forwardNormZ * tileWorldSize * std::max(1.0f, static_cast<float>(radius)),
                -std::max(1, radius / 2)
            });
        anchors.push_back({predictedX, predictedY, predictedZ, -std::max(1, radius / 2)});

        const auto queueHeightCoord = [&](const SparseClipmapTileCoord& coord) {
            m_interestSet.insert(coord);

            auto existing = m_slotByCoord.find(coord);
            if (existing != m_slotByCoord.end()) {
                m_tiles[existing->second].record.lastTouchedFrame = frameIndex;
                return;
            }

            if (m_queuedSet.insert(coord).second) {
                m_generationQueue.push_back(coord);
            }
        };

        // Queue current/motion/forward centerline tiles before wider shells.
        // With small per-frame clipmap budgets this gives the streamer a
        // continuous visual breadcrumb path instead of spending the whole
        // budget on the local shell and only discovering the far predicted
        // tile later.
        for (const InterestAnchor& anchor : anchors) {
            const int32_t centerX = static_cast<int32_t>(std::floor(anchor.x / tileWorldSize));
            const int32_t centerZ = static_cast<int32_t>(std::floor(anchor.z / tileWorldSize));
            queueHeightCoord(SparseClipmapTileCoord{
                static_cast<int32_t>(ring),
                centerX,
                centerZ
            });
        }
        const uint32_t maxLineCoords = std::max<uint32_t>(
            2u,
            policy.Config().motionLookaheadSteps * 3u + 4u);
        if (useMotionLookahead) {
            for (const SparseClipmapTileCoord& coord : BuildTileLine2D(
                     static_cast<int32_t>(ring),
                     cameraX,
                     cameraZ,
                     predictedX,
                     predictedZ,
                     tileWorldSize,
                     maxLineCoords)) {
                queueHeightCoord(coord);
            }
        }
        const float forwardAnchorX =
            cameraX + forwardNormX * tileWorldSize * std::max(1.0f, static_cast<float>(radius));
        const float forwardAnchorZ =
            cameraZ + forwardNormZ * tileWorldSize * std::max(1.0f, static_cast<float>(radius));
        for (const SparseClipmapTileCoord& coord : BuildTileLine2D(
                 static_cast<int32_t>(ring),
                 cameraX,
                 cameraZ,
                 forwardAnchorX,
                 forwardAnchorZ,
                 tileWorldSize,
                 maxLineCoords)) {
            queueHeightCoord(coord);
        }

        const std::vector<InterestAnchor> shellAnchors = anchors;
        for (const InterestAnchor& anchor : shellAnchors) {
            const int32_t anchorRadius = std::max(1, radius + anchor.radiusBias);
            const int32_t centerX = static_cast<int32_t>(std::floor(anchor.x / tileWorldSize));
            const int32_t centerZ = static_cast<int32_t>(std::floor(anchor.z / tileWorldSize));
            ++heightAnchorCount;
            for (int32_t dz = -anchorRadius; dz <= anchorRadius; ++dz) {
                for (int32_t dx = -anchorRadius; dx <= anchorRadius; ++dx) {
                    queueHeightCoord(SparseClipmapTileCoord{
                        static_cast<int32_t>(ring),
                        centerX + dx,
                        centerZ + dz
                    });
                }
            }
        }
    }

    RefreshStats();
    m_stats.heightInterestAnchors = heightAnchorCount;
    UpdateVoxelInterest(
        cameraX,
        cameraY,
        cameraZ,
        frameIndex,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        velocityX,
        velocityY,
        velocityZ,
        predictionSeconds);
}

uint32_t SparseClipmapTileCache::AllocateSlot(
    const SparseClipmapTileCoord& coord,
    uint32_t frameIndex)
{
    if (!m_freeSlots.empty()) {
        const uint32_t slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        return slot;
    }

    uint32_t bestSlot = UINT32_MAX;
    uint32_t oldestFrame = std::numeric_limits<uint32_t>::max();
    for (uint32_t slot = 0; slot < m_tiles.size(); ++slot) {
        const SparseClipmapTileRecord& record = m_tiles[slot].record;
        if (record.slot == UINT32_MAX) {
            bestSlot = slot;
            break;
        }
        if (record.lastTouchedFrame < oldestFrame) {
            oldestFrame = record.lastTouchedFrame;
            bestSlot = slot;
        }
    }

    if (bestSlot == UINT32_MAX) {
        return UINT32_MAX;
    }

    const SparseClipmapTileCoord oldCoord = m_tiles[bestSlot].record.coord;
    m_slotByCoord.erase(oldCoord);
    m_tiles[bestSlot].packedSamples.clear();
    m_tiles[bestSlot].record = {};
    m_tiles[bestSlot].record.slot = bestSlot;
    m_tiles[bestSlot].record.coord = coord;
    m_tiles[bestSlot].record.lastTouchedFrame = frameIndex;
    ++m_dirtySerial;
    ++m_heightDirtySerial;
    MarkHeightSlotDirty(bestSlot);
    return bestSlot;
}

uint32_t SparseClipmapTileCache::PumpGeneration(
    uint32_t maxTiles,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy)
{
    if (!policy.IsEnabled() || maxTiles == 0) {
        RefreshStats();
        return 0;
    }

    uint32_t generated = 0;
    uint32_t evicted = 0;
    while (!m_generationQueue.empty() && generated < maxTiles) {
        const SparseClipmapTileCoord coord = m_generationQueue.front();
        m_generationQueue.pop_front();
        m_queuedSet.erase(coord);

        if (!m_interestSet.empty() && m_interestSet.find(coord) == m_interestSet.end()) {
            continue;
        }

        if (m_slotByCoord.find(coord) != m_slotByCoord.end()) {
            continue;
        }

        const bool wasFull = m_slotByCoord.size() >= m_tiles.size() && m_freeSlots.empty();
        const uint32_t slot = AllocateSlot(coord, frameIndex);
        if (slot == UINT32_MAX) {
            break;
        }
        evicted += wasFull ? 1u : 0u;
        m_tiles[slot].record.coord = coord;
        m_tiles[slot].record.slot = slot;
        m_tiles[slot].record.lastTouchedFrame = frameIndex;
        GenerateTile(slot, policy);
        m_slotByCoord[coord] = slot;
        ++generated;
        ++m_dirtySerial;
        ++m_heightDirtySerial;
        MarkHeightSlotDirty(slot);
    }

    uint32_t generatedVoxel = 0;
    uint32_t evictedVoxel = 0;
    while (!m_voxelGenerationQueue.empty() && generatedVoxel < maxTiles) {
        const SparseVoxelClipmapCoord coord = m_voxelGenerationQueue.front();
        m_voxelGenerationQueue.pop_front();
        m_queuedVoxelSet.erase(coord);

        if (!m_voxelInterestSet.empty() && m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            continue;
        }

        const bool wasFull = m_voxelSlotByCoord.size() >= m_voxelBricks.size() && m_freeVoxelSlots.empty();
        const uint32_t slot = AllocateVoxelSlot(coord, frameIndex);
        if (slot == UINT32_MAX) {
            break;
        }
        evictedVoxel += wasFull ? 1u : 0u;
        m_voxelBricks[slot].coord = coord;
        m_voxelBricks[slot].slot = slot;
        m_voxelBricks[slot].lastTouchedFrame = frameIndex;
        GenerateVoxelBrick(slot, policy);
        m_voxelSlotByCoord[coord] = slot;
        ++generatedVoxel;
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        MarkVoxelSlotDirty(slot);
    }

    RefreshStats(generated, evicted, generatedVoxel, evictedVoxel);
    return generated + generatedVoxel;
}

void SparseClipmapTileCache::GenerateTile(uint32_t slot, const SparseClipmapPolicy& policy) {
    if (slot >= m_tiles.size()) {
        return;
    }

    const auto rings = policy.BuildRings();
    TilePayload& tile = m_tiles[slot];
    if (tile.record.coord.ring < 0 ||
        static_cast<uint32_t>(tile.record.coord.ring) >= rings.size()) {
        return;
    }

    const SparseClipmapRing& ring = rings[static_cast<uint32_t>(tile.record.coord.ring)];
    const uint32_t side = policy.Config().tileSampleSide;
    const float tileWorldSize = ring.cellSize * static_cast<float>(side - 1u);
    tile.record.cellSize = ring.cellSize;
    tile.record.originX = static_cast<int32_t>(std::floor(static_cast<float>(tile.record.coord.x) * tileWorldSize));
    tile.record.originZ = static_cast<int32_t>(std::floor(static_cast<float>(tile.record.coord.z) * tileWorldSize));
    tile.packedSamples.resize(static_cast<size_t>(side) * static_cast<size_t>(side));

    for (uint32_t z = 0; z < side; ++z) {
        for (uint32_t x = 0; x < side; ++x) {
            const int32_t worldX = static_cast<int32_t>(std::lround(
                static_cast<float>(tile.record.originX) + static_cast<float>(x) * ring.cellSize));
            const int32_t worldZ = static_cast<int32_t>(std::lround(
                static_cast<float>(tile.record.originZ) + static_cast<float>(z) * ring.cellSize));
            const float height = m_terrain.HeightAt(worldX, worldZ);
            tile.packedSamples[x + z * side] = PackSample(worldX, worldZ, height);
        }
    }
}

void SparseClipmapTileCache::UpdateVoxelInterest(
    float cameraX,
    float cameraY,
    float cameraZ,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    float forwardX,
    float forwardY,
    float forwardZ,
    float velocityX,
    float velocityY,
    float velocityZ,
    float predictionSeconds)
{
    if (!policy.IsEnabled() || !policy.Config().voxelClipmapEnabled || m_voxelBricks.empty()) {
        m_voxelInterestSet.clear();
        RefreshStats();
        m_stats.voxelInterestAnchors = 0;
        return;
    }

    m_voxelInterestSet.clear();
    const auto rings = policy.BuildRings();
    const int32_t radiusXz = static_cast<int32_t>(policy.Config().voxelBrickRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(policy.Config().voxelBrickRadiusY);
    const uint32_t ringCount = std::max(1u, static_cast<uint32_t>(rings.size()));
    const uint32_t maxResidentInterest = std::max<uint32_t>(
        ringCount,
        static_cast<uint32_t>(
            (static_cast<uint64_t>(m_voxelBricks.size()) *
             static_cast<uint64_t>(policy.Config().voxelInterestCapacityPercent)) / 100u));
    const uint32_t baseQuotaPerRing = std::max(1u, maxResidentInterest / ringCount);
    uint32_t quotaRemainder = maxResidentInterest - baseQuotaPerRing * ringCount;

    struct VoxelInterestCandidate {
        SparseVoxelClipmapCoord coord;
        int32_t dx = 0;
        int32_t dy = 0;
        int32_t dz = 0;
        uint32_t distanceScore = 0;
    };
    struct VoxelInterestAnchor {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32_t baseScore = 0;
        int32_t radiusBias = 0;
    };

    const float forwardLen = std::sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    const float invForwardLen = forwardLen > 0.001f ? 1.0f / forwardLen : 0.0f;
    const float forwardNormX = forwardX * invForwardLen;
    const float forwardNormY = forwardY * invForwardLen;
    const float forwardNormZ = forwardZ * invForwardLen;
    const float predictedX = cameraX + velocityX * std::max(0.0f, predictionSeconds);
    const float predictedY = cameraY + velocityY * std::max(0.0f, predictionSeconds);
    const float predictedZ = cameraZ + velocityZ * std::max(0.0f, predictionSeconds);
    const float velocityLen = std::sqrt(velocityX * velocityX + velocityY * velocityY + velocityZ * velocityZ);
    const bool useMotionLookahead =
        predictionSeconds > 0.0f &&
        velocityLen >= policy.Config().motionLookaheadMinSpeed;
    uint32_t voxelAnchorCount = 0;

    for (uint32_t ring = 0; ring < rings.size(); ++ring) {
        const float brickWorldSize = std::max(1.0f, rings[ring].cellSize * static_cast<float>(SPARSE_BRICK_SIZE));
        std::vector<VoxelInterestAnchor> anchors;
        anchors.reserve(3u + policy.Config().motionLookaheadSteps);
        anchors.push_back({cameraX, cameraY, cameraZ, 0u, 0});
        if (useMotionLookahead) {
            const uint32_t steps = std::max(1u, policy.Config().motionLookaheadSteps);
            for (uint32_t step = 1u; step <= steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                anchors.push_back({
                    cameraX + (predictedX - cameraX) * t,
                    cameraY + (predictedY - cameraY) * t,
                    cameraZ + (predictedZ - cameraZ) * t,
                    120u + step * 25u,
                    -radiusXz
                });
            }
        }
        anchors.push_back(
            {
                cameraX + forwardNormX * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                cameraY + forwardNormY * brickWorldSize * 0.5f,
                cameraZ + forwardNormZ * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                650u,
                -std::max(1, radiusXz / 2)
            });
        anchors.push_back({predictedX, predictedY, predictedZ, 900u, -std::max(1, radiusXz / 2)});
        std::vector<VoxelInterestCandidate> candidates;
        std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> candidateSet;
        candidates.reserve(
            static_cast<size_t>(radiusXz * 2 + 1) *
            static_cast<size_t>(radiusY * 2 + 1) *
            static_cast<size_t>(radiusXz * 2 + 1) *
            anchors.size() *
            2u);

        const auto addCandidate = [&](
            int32_t x,
            int32_t y,
            int32_t z,
            int32_t dx,
            int32_t dy,
            int32_t dz,
            uint32_t baseScore) {
            const SparseVoxelClipmapCoord coord{
                static_cast<int32_t>(ring),
                x,
                y,
                z
            };
            if (!candidateSet.insert(coord).second) {
                return;
            }
            const uint32_t score =
                baseScore +
                static_cast<uint32_t>(dx * dx * 4 + dy * dy * 9 + dz * dz * 4);
            candidates.push_back(VoxelInterestCandidate{coord, dx, dy, dz, score});
        };
        const auto addTerrainCenterlineCandidates = [&](
            float startX,
            float startZ,
            float endX,
            float endZ,
            uint32_t baseScore) {
            const uint32_t maxLineCoords = std::max<uint32_t>(
                2u,
                policy.Config().motionLookaheadSteps * 3u + 4u);
            const std::vector<SparseClipmapTileCoord> lineCoords = BuildTileLine2D(
                static_cast<int32_t>(ring),
                startX,
                startZ,
                endX,
                endZ,
                brickWorldSize,
                maxLineCoords);
            std::vector<int32_t> terrainCenterYs;
            terrainCenterYs.reserve(lineCoords.size());
            for (const SparseClipmapTileCoord& lineCoord : lineCoords) {
                const int32_t sampleX = static_cast<int32_t>(std::floor(
                    (static_cast<float>(lineCoord.x) + 0.5f) * brickWorldSize));
                const int32_t sampleZ = static_cast<int32_t>(std::floor(
                    (static_cast<float>(lineCoord.z) + 0.5f) * brickWorldSize));
                const float terrainY = m_terrain.HeightAt(sampleX, sampleZ);
                terrainCenterYs.push_back(static_cast<int32_t>(std::floor(terrainY / brickWorldSize)));
            }
            for (size_t i = 0; i < lineCoords.size(); ++i) {
                addCandidate(
                    lineCoords[i].x,
                    terrainCenterYs[i],
                    lineCoords[i].z,
                    0,
                    0,
                    0,
                    baseScore + static_cast<uint32_t>(i) * 2u);
            }
            for (size_t i = 0; i < lineCoords.size(); ++i) {
                const SparseClipmapTileCoord& lineCoord = lineCoords[i];
                const int32_t terrainCenterY = terrainCenterYs[i];
                for (int32_t dy = -radiusY; dy <= radiusY; ++dy) {
                    if (dy == 0) {
                        continue;
                    }
                    addCandidate(
                        lineCoord.x,
                        terrainCenterY + dy,
                        lineCoord.z,
                        0,
                        dy,
                        0,
                        baseScore + 10u + static_cast<uint32_t>(i) * 2u);
                }
            }
        };

        if (useMotionLookahead) {
            addTerrainCenterlineCandidates(
                cameraX,
                cameraZ,
                predictedX,
                predictedZ,
                20u);
        }
        addTerrainCenterlineCandidates(
            cameraX,
            cameraZ,
            cameraX + forwardNormX * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            cameraZ + forwardNormZ * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            45u);

        for (const VoxelInterestAnchor& anchor : anchors) {
            const int32_t anchorRadiusXz = std::max(1, radiusXz + anchor.radiusBias);
            const int32_t centerX = static_cast<int32_t>(std::floor(anchor.x / brickWorldSize));
            const int32_t centerY = static_cast<int32_t>(std::floor(anchor.y / brickWorldSize));
            const int32_t centerZ = static_cast<int32_t>(std::floor(anchor.z / brickWorldSize));
            ++voxelAnchorCount;
            for (int32_t dz = -anchorRadiusXz; dz <= anchorRadiusXz; ++dz) {
                for (int32_t dx = -anchorRadiusXz; dx <= anchorRadiusXz; ++dx) {
                    const int32_t brickX = centerX + dx;
                    const int32_t brickZ = centerZ + dz;
                    const int32_t sampleX = static_cast<int32_t>(std::floor(
                        (static_cast<float>(brickX) + 0.5f) * brickWorldSize));
                    const int32_t sampleZ = static_cast<int32_t>(std::floor(
                        (static_cast<float>(brickZ) + 0.5f) * brickWorldSize));
                    const float terrainY = m_terrain.HeightAt(sampleX, sampleZ);
                    const int32_t terrainCenterY = static_cast<int32_t>(std::floor(terrainY / brickWorldSize));
                    for (int32_t dy = -radiusY; dy <= radiusY; ++dy) {
                        // The mid voxel clipmap is primarily distant terrain
                        // context. Anchor the vertical interest around generated
                        // terrain height, not camera height, so high/flying cameras
                        // stream terrain below them instead of empty air.
                        addCandidate(
                            brickX,
                            terrainCenterY + dy,
                            brickZ,
                            dx,
                            dy,
                            dz,
                            anchor.baseScore);

                        // Keep a lower-priority camera-height band for cases where
                        // the camera is inside tall/vertical formations. This should
                        // not steal quota from terrain-surface bricks unless there
                        // is spare capacity in the ring.
                        addCandidate(
                            brickX,
                            centerY + dy,
                            brickZ,
                            dx,
                            dy,
                            dz,
                            anchor.baseScore + 5000u);
                    }
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const VoxelInterestCandidate& a, const VoxelInterestCandidate& b) {
                if (a.distanceScore != b.distanceScore) {
                    return a.distanceScore < b.distanceScore;
                }
                if (a.dy != b.dy) {
                    return std::abs(a.dy) < std::abs(b.dy);
                }
                if (a.dz != b.dz) {
                    return std::abs(a.dz) < std::abs(b.dz);
                }
                return std::abs(a.dx) < std::abs(b.dx);
            });

        const uint32_t ringQuota =
            baseQuotaPerRing + ((ring < quotaRemainder) ? 1u : 0u);
        const uint32_t emitCount = std::min<uint32_t>(ringQuota, static_cast<uint32_t>(candidates.size()));
        for (uint32_t i = 0; i < emitCount; ++i) {
            const SparseVoxelClipmapCoord& coord = candidates[i].coord;
            m_voxelInterestSet.insert(coord);

            auto existing = m_voxelSlotByCoord.find(coord);
            if (existing != m_voxelSlotByCoord.end()) {
                m_voxelBricks[existing->second].lastTouchedFrame = frameIndex;
                continue;
            }
            if (m_queuedVoxelSet.insert(coord).second) {
                m_voxelGenerationQueue.push_back(coord);
            }
        }
    }

    RefreshStats();
    m_stats.voxelInterestAnchors = voxelAnchorCount;
}

uint32_t SparseClipmapTileCache::AllocateVoxelSlot(
    const SparseVoxelClipmapCoord& coord,
    uint32_t frameIndex)
{
    if (!m_freeVoxelSlots.empty()) {
        const uint32_t slot = m_freeVoxelSlots.back();
        m_freeVoxelSlots.pop_back();
        return slot;
    }

    uint32_t bestSlot = UINT32_MAX;
    uint32_t oldestFrame = std::numeric_limits<uint32_t>::max();
    for (uint32_t slot = 0; slot < m_voxelBricks.size(); ++slot) {
        const VoxelBrickPayload& brick = m_voxelBricks[slot];
        if (brick.slot == UINT32_MAX) {
            bestSlot = slot;
            break;
        }
        if (brick.lastTouchedFrame < oldestFrame) {
            oldestFrame = brick.lastTouchedFrame;
            bestSlot = slot;
        }
    }
    if (bestSlot == UINT32_MAX) {
        return UINT32_MAX;
    }

    m_voxelSlotByCoord.erase(m_voxelBricks[bestSlot].coord);
    m_voxelBricks[bestSlot] = {};
    m_voxelBricks[bestSlot].slot = bestSlot;
    m_voxelBricks[bestSlot].coord = coord;
    m_voxelBricks[bestSlot].lastTouchedFrame = frameIndex;
    ++m_dirtySerial;
    ++m_voxelDirtySerial;
    MarkVoxelSlotDirty(bestSlot);
    return bestSlot;
}

void SparseClipmapTileCache::GenerateVoxelBrick(uint32_t slot, const SparseClipmapPolicy& policy) {
    if (slot >= m_voxelBricks.size()) {
        return;
    }

    const auto rings = policy.BuildRings();
    VoxelBrickPayload& brick = m_voxelBricks[slot];
    if (brick.coord.ring < 0 || static_cast<uint32_t>(brick.coord.ring) >= rings.size()) {
        return;
    }

    const SparseClipmapRing& ring = rings[static_cast<uint32_t>(brick.coord.ring)];
    const int32_t brickWorldSize = static_cast<int32_t>(std::lround(
        ring.cellSize * static_cast<float>(SPARSE_BRICK_SIZE)));
    brick.cellSize = ring.cellSize;
    brick.originX = brick.coord.x * brickWorldSize;
    brick.originY = brick.coord.y * brickWorldSize;
    brick.originZ = brick.coord.z * brickWorldSize;
    brick.voxels.resize(SPARSE_BRICK_VOXEL_COUNT);
    const int32_t sampleStep = std::max(1, static_cast<int32_t>(std::lround(ring.cellSize)));

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const int32_t worldX = brick.originX + static_cast<int32_t>(
                    std::lround((static_cast<float>(x) + 0.5f) * ring.cellSize));
                const int32_t worldY = brick.originY + static_cast<int32_t>(
                    std::lround((static_cast<float>(y) + 0.5f) * ring.cellSize));
                const int32_t worldZ = brick.originZ + static_cast<int32_t>(
                    std::lround((static_cast<float>(z) + 0.5f) * ring.cellSize));
                uint32_t voxel = m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
                const uint32_t surfaceVoxel = m_terrain.SampleGeneratedSurfaceVoxel(
                    worldX,
                    worldY,
                    worldZ,
                    sampleStep);
                if (Utils::UnpackMaterial(surfaceVoxel) != Utils::Material::Air) {
                    voxel |= static_cast<uint32_t>(Utils::StateFlags::VisualSurface) << 24u;
                }
                brick.voxels[LocalVoxelIndex({x, y, z})] = voxel;
            }
        }
    }
}

uint32_t SparseClipmapTileCache::PackSample(int32_t worldX, int32_t worldZ, float height) const {
    const int32_t roundedHeight = static_cast<int32_t>(std::lround(height));
    const uint32_t biasedHeight = static_cast<uint32_t>(
        std::clamp(roundedHeight + 32768, 0, 65535));
    const uint32_t voxel = m_terrain.SampleGeneratedVoxel(worldX, roundedHeight, worldZ);
    const uint32_t material = static_cast<uint32_t>(Utils::UnpackMaterial(voxel)) & 0xFFu;
    return biasedHeight | (material << 16);
}

bool SparseClipmapTileCache::BuildGpuSnapshot(SparseClipmapGpuSnapshot& outSnapshot) const {
    outSnapshot = {};
    if (m_tiles.empty()) {
        return false;
    }

    const uint32_t side = m_config.tileSampleSide;
    const uint32_t sampleCountPerTile = side * side;
    const uint32_t maxTiles = static_cast<uint32_t>(m_tiles.size());
    const uint32_t lookupCapacity = NextPowerOfTwo(std::max(16u, maxTiles * 4u));
    const uint32_t maxVoxelBricks = static_cast<uint32_t>(m_voxelBricks.size());
    const uint32_t voxelLookupCapacity = NextPowerOfTwo(std::max(16u, maxVoxelBricks * 4u));
    outSnapshot.metadata.assign(static_cast<size_t>(maxTiles + 1u) * 4u, 0u);
    outSnapshot.lookup.assign(static_cast<size_t>(lookupCapacity) * 4u, 0u);
    outSnapshot.samples.assign(static_cast<size_t>(maxTiles) * sampleCountPerTile, 0u);
    outSnapshot.voxelMetadata.assign(static_cast<size_t>(maxVoxelBricks + 1u) * 4u, 0u);
    outSnapshot.voxelLookup.assign(static_cast<size_t>(voxelLookupCapacity) * 4u, 0u);
    outSnapshot.voxelSamples.assign(static_cast<size_t>(maxVoxelBricks) * SPARSE_BRICK_VOXEL_COUNT, 0u);
    outSnapshot.metadata[0] = 0x56434C50u; // "VCLP"
    outSnapshot.metadata[1] = side;
    outSnapshot.metadata[2] = maxTiles;
    outSnapshot.metadata[3] = (lookupCapacity & 0x00FFFFFFu) |
        ((m_config.ringCount & 0xFFu) << 24u);
    outSnapshot.voxelMetadata[0] = 0x56435658u; // "VCVX"
    outSnapshot.voxelMetadata[1] = SPARSE_BRICK_SIZE;
    outSnapshot.voxelMetadata[2] = maxVoxelBricks;
    outSnapshot.voxelMetadata[3] = (voxelLookupCapacity & 0x00FFFFFFu) |
        ((m_config.ringCount & 0xFFu) << 24u);

    uint32_t residentHeightEntries = 0;
    uint32_t maxUsedTileSlot = 0;
    for (uint32_t tileSlot = 0; tileSlot < static_cast<uint32_t>(m_tiles.size()); ++tileSlot) {
        const TilePayload& tile = m_tiles[tileSlot];
        if (tile.record.slot == UINT32_MAX || tile.packedSamples.empty()) {
            continue;
        }
        if (tileSlot >= maxTiles) {
            break;
        }
        maxUsedTileSlot = std::max(maxUsedTileSlot, tileSlot);
        ++residentHeightEntries;

        const size_t metadataBase = static_cast<size_t>(tileSlot + 1u) * 4u;
        outSnapshot.metadata[metadataBase + 0u] = static_cast<uint32_t>(tile.record.originX);
        outSnapshot.metadata[metadataBase + 1u] = static_cast<uint32_t>(tile.record.originZ);
        outSnapshot.metadata[metadataBase + 2u] = static_cast<uint32_t>(tile.record.coord.ring & 0xFF) |
            (static_cast<uint32_t>(tile.record.cellSize) << 8);
        outSnapshot.metadata[metadataBase + 3u] = 1u;

        const size_t sampleBase = static_cast<size_t>(tileSlot) * sampleCountPerTile;
        std::copy(
            tile.packedSamples.begin(),
            tile.packedSamples.end(),
            outSnapshot.samples.begin() + sampleBase);

        const uint32_t lookupMask = lookupCapacity - 1u;
        uint32_t lookupSlot = HashClipmapTileCoord(tile.record.coord) & lookupMask;
        for (uint32_t probe = 0; probe < lookupCapacity; ++probe) {
            const size_t lookupBase = static_cast<size_t>(lookupSlot) * 4u;
            if (outSnapshot.lookup[lookupBase + 3u] == 0u) {
                outSnapshot.lookup[lookupBase + 0u] = static_cast<uint32_t>(tile.record.coord.ring);
                outSnapshot.lookup[lookupBase + 1u] = static_cast<uint32_t>(tile.record.coord.x);
                outSnapshot.lookup[lookupBase + 2u] = static_cast<uint32_t>(tile.record.coord.z);
                outSnapshot.lookup[lookupBase + 3u] = tileSlot + 1u;
                break;
            }
            lookupSlot = (lookupSlot + 1u) & lookupMask;
        }
    }

    outSnapshot.tileCount = residentHeightEntries == 0 ? 0u : maxUsedTileSlot + 1u;
    outSnapshot.tileSampleSide = side;
    outSnapshot.lookupCapacity = lookupCapacity;
    if (m_dirtyHeightStartSlot != UINT32_MAX && m_dirtyHeightStartSlot <= m_dirtyHeightEndSlot) {
        outSnapshot.heightDirtyStartSlot = m_dirtyHeightStartSlot;
        outSnapshot.heightDirtySlotCount = m_dirtyHeightEndSlot - m_dirtyHeightStartSlot + 1u;
    }

    uint32_t residentVoxelEntries = 0;
    uint32_t maxUsedVoxelSlot = 0;
    for (uint32_t voxelSlot = 0; voxelSlot < static_cast<uint32_t>(m_voxelBricks.size()); ++voxelSlot) {
        const VoxelBrickPayload& brick = m_voxelBricks[voxelSlot];
        if (brick.slot == UINT32_MAX || brick.voxels.empty()) {
            continue;
        }
        if (voxelSlot >= maxVoxelBricks) {
            break;
        }
        maxUsedVoxelSlot = std::max(maxUsedVoxelSlot, voxelSlot);
        ++residentVoxelEntries;

        const size_t metadataBase = static_cast<size_t>(voxelSlot + 1u) * 4u;
        outSnapshot.voxelMetadata[metadataBase + 0u] = static_cast<uint32_t>(brick.originX);
        outSnapshot.voxelMetadata[metadataBase + 1u] = static_cast<uint32_t>(brick.originY);
        outSnapshot.voxelMetadata[metadataBase + 2u] = static_cast<uint32_t>(brick.originZ);
        outSnapshot.voxelMetadata[metadataBase + 3u] = static_cast<uint32_t>(brick.coord.ring & 0xFF) |
            (static_cast<uint32_t>(brick.cellSize) << 8);

        const size_t sampleBase = static_cast<size_t>(voxelSlot) * SPARSE_BRICK_VOXEL_COUNT;
        std::copy(
            brick.voxels.begin(),
            brick.voxels.end(),
            outSnapshot.voxelSamples.begin() + sampleBase);

        const uint32_t lookupMask = voxelLookupCapacity - 1u;
        uint32_t lookupSlot = HashVoxelClipmapCoord(brick.coord) & lookupMask;
        for (uint32_t probe = 0; probe < voxelLookupCapacity; ++probe) {
            const size_t lookupBase = static_cast<size_t>(lookupSlot) * 4u;
            if (outSnapshot.voxelLookup[lookupBase + 3u] == 0u) {
                outSnapshot.voxelLookup[lookupBase + 0u] = static_cast<uint32_t>(brick.coord.x);
                outSnapshot.voxelLookup[lookupBase + 1u] = static_cast<uint32_t>(brick.coord.y);
                outSnapshot.voxelLookup[lookupBase + 2u] = static_cast<uint32_t>(brick.coord.z);
                outSnapshot.voxelLookup[lookupBase + 3u] =
                    ((static_cast<uint32_t>(brick.coord.ring) & 0xFFu) << 24u) |
                    ((voxelSlot + 1u) & 0x00FFFFFFu);
                break;
            }
            lookupSlot = (lookupSlot + 1u) & lookupMask;
        }
    }
    outSnapshot.voxelBrickCount = residentVoxelEntries == 0 ? 0u : maxUsedVoxelSlot + 1u;
    outSnapshot.voxelLookupCapacity = voxelLookupCapacity;
    if (m_dirtyVoxelStartSlot != UINT32_MAX && m_dirtyVoxelStartSlot <= m_dirtyVoxelEndSlot) {
        outSnapshot.voxelDirtyStartSlot = m_dirtyVoxelStartSlot;
        outSnapshot.voxelDirtySlotCount = m_dirtyVoxelEndSlot - m_dirtyVoxelStartSlot + 1u;
    }
    outSnapshot.voxelMetadata[2] = outSnapshot.voxelBrickCount;
    outSnapshot.frameIndex = m_dirtySerial;
    outSnapshot.metadata[2] = outSnapshot.tileCount;
    return residentHeightEntries > 0 || residentVoxelEntries > 0;
}

void SparseClipmapTileCache::ClearHeightDirtyRange() {
    m_dirtyHeightStartSlot = UINT32_MAX;
    m_dirtyHeightEndSlot = 0;
}

void SparseClipmapTileCache::ClearVoxelDirtyRange() {
    m_dirtyVoxelStartSlot = UINT32_MAX;
    m_dirtyVoxelEndSlot = 0;
}

void SparseClipmapTileCache::MarkHeightSlotDirty(uint32_t slot) {
    if (slot == UINT32_MAX || slot >= m_tiles.size()) {
        return;
    }
    m_dirtyHeightStartSlot = std::min(m_dirtyHeightStartSlot, slot);
    m_dirtyHeightEndSlot = std::max(m_dirtyHeightEndSlot, slot);
}

void SparseClipmapTileCache::MarkVoxelSlotDirty(uint32_t slot) {
    if (slot == UINT32_MAX || slot >= m_voxelBricks.size()) {
        return;
    }
    m_dirtyVoxelStartSlot = std::min(m_dirtyVoxelStartSlot, slot);
    m_dirtyVoxelEndSlot = std::max(m_dirtyVoxelEndSlot, slot);
}

void SparseClipmapTileCache::RefreshStats(
    uint32_t generatedLastFrame,
    uint32_t evictedLastFrame,
    uint32_t generatedVoxelLastFrame,
    uint32_t evictedVoxelLastFrame)
{
    m_stats.residentTiles = static_cast<uint32_t>(m_slotByCoord.size());
    m_stats.queuedTiles = static_cast<uint32_t>(m_generationQueue.size());
    m_stats.interestedTiles = static_cast<uint32_t>(m_interestSet.size());
    m_stats.missingInterestedTiles = 0;
    for (const SparseClipmapTileCoord& coord : m_interestSet) {
        if (m_slotByCoord.find(coord) == m_slotByCoord.end()) {
            ++m_stats.missingInterestedTiles;
        }
    }
    m_stats.generatedTilesLastFrame = generatedLastFrame;
    m_stats.evictedTilesLastFrame = evictedLastFrame;
    m_stats.dirtySerial = m_dirtySerial;
    m_stats.snapshotTiles = m_stats.residentTiles;
    m_stats.residentVoxelBricks = static_cast<uint32_t>(m_voxelSlotByCoord.size());
    m_stats.queuedVoxelBricks = static_cast<uint32_t>(m_voxelGenerationQueue.size());
    m_stats.interestedVoxelBricks = static_cast<uint32_t>(m_voxelInterestSet.size());
    m_stats.missingInterestedVoxelBricks = 0;
    for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
        if (m_voxelSlotByCoord.find(coord) == m_voxelSlotByCoord.end()) {
            ++m_stats.missingInterestedVoxelBricks;
        }
    }
    m_stats.generatedVoxelBricksLastFrame = generatedVoxelLastFrame;
    m_stats.evictedVoxelBricksLastFrame = evictedVoxelLastFrame;
}

} // namespace VENPOD::Simulation
