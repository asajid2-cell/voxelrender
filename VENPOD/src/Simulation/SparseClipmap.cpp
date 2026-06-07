#include "SparseClipmap.h"

#include "SparseEditStore.h"
#include "TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <thread>
#include <unordered_map>

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

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

double FiniteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

float ElapsedMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<float, std::milli>(end - start).count();
}

int32_t ClampDoubleToInt32(double value) {
    value = FiniteOr(value, 0.0);
    if (value <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

int32_t FloorToInt32Clamped(double value) {
    return ClampDoubleToInt32(std::floor(FiniteOr(value, 0.0)));
}

int32_t CeilToInt32Clamped(double value) {
    return ClampDoubleToInt32(std::ceil(FiniteOr(value, 0.0)));
}

int32_t RoundToInt32Clamped(double value) {
    return ClampDoubleToInt32(std::round(FiniteOr(value, 0.0)));
}

int32_t QuantizeFloatToInt(float value, float scale) {
    return RoundToInt32Clamped(static_cast<double>(FiniteOr(value, 0.0f)) * static_cast<double>(scale));
}

uint32_t QuantizeFloatToUint(float value, float scale) {
    const int32_t quantized = QuantizeFloatToInt(value, scale);
    return quantized <= 0 ? 0u : static_cast<uint32_t>(quantized);
}

int32_t FloorToGridCoordClamped(float worldCoord, float cellSize, int32_t coordMargin = 0) {
    const double cell = std::max(1.0, static_cast<double>(FiniteOr(cellSize, 1.0f)));
    const int32_t safeMargin = std::max(0, coordMargin);
    const double maxCoord =
        std::floor((static_cast<double>(std::numeric_limits<int32_t>::max()) - cell) / cell) -
        static_cast<double>(safeMargin);
    const double minCoord =
        std::ceil((static_cast<double>(std::numeric_limits<int32_t>::min()) + cell) / cell) +
        static_cast<double>(safeMargin);
    if (minCoord > maxCoord) {
        return 0;
    }
    const double raw = std::floor(static_cast<double>(FiniteOr(worldCoord, 0.0f)) / cell);
    return ClampDoubleToInt32(std::clamp(raw, minCoord, maxCoord));
}

int32_t SaturatingAddInt32(int32_t lhs, int32_t rhs) {
    const int64_t sum = static_cast<int64_t>(lhs) + static_cast<int64_t>(rhs);
    if (sum < static_cast<int64_t>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (sum > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(sum);
}

int32_t FloorDiv2Int32(int32_t value) {
    return value >= 0 ? value / 2 : -(((-value) + 1) / 2);
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
    int32_t x = FloorToGridCoordClamped(startX, cellSize, 16);
    int32_t z = FloorToGridCoordClamped(startZ, cellSize, 16);
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
            x = SaturatingAddInt32(x, axisX.step);
            axisX.nextT += axisX.deltaT;
        }
        if (axisZ.nextT <= nextDistance + tieEpsilon) {
            z = SaturatingAddInt32(z, axisZ.step);
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

std::vector<SparseClipmapSampleRange> BuildCoalescedSampleRanges(
    const std::vector<uint32_t>& dirtySlots,
    uint32_t maxSlots,
    uint32_t dirtyStartSlot,
    uint32_t dirtyEndSlot)
{
    std::vector<SparseClipmapSampleRange> ranges;
    if (maxSlots == 0) {
        return ranges;
    }
    if (dirtyStartSlot == UINT32_MAX || dirtyStartSlot > dirtyEndSlot) {
        ranges.push_back({0u, maxSlots});
        return ranges;
    }

    std::vector<uint32_t> slots;
    slots.reserve(dirtySlots.size());
    for (uint32_t slot : dirtySlots) {
        if (slot < maxSlots) {
            slots.push_back(slot);
        }
    }
    if (slots.empty()) {
        const uint32_t startSlot = std::min(dirtyStartSlot, maxSlots);
        if (startSlot >= maxSlots) {
            return ranges;
        }
        const uint32_t slotCount = std::min(dirtyEndSlot - dirtyStartSlot + 1u, maxSlots - startSlot);
        ranges.push_back({startSlot, slotCount});
        return ranges;
    }

    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    uint32_t rangeStart = slots.front();
    uint32_t previous = rangeStart;
    for (size_t i = 1; i < slots.size(); ++i) {
        const uint32_t slot = slots[i];
        if (slot == previous + 1u) {
            previous = slot;
            continue;
        }
        ranges.push_back({rangeStart, previous - rangeStart + 1u});
        rangeStart = slot;
        previous = slot;
    }
    ranges.push_back({rangeStart, previous - rangeStart + 1u});
    return ranges;
}

uint32_t CountSampleRangeSlots(const std::vector<SparseClipmapSampleRange>& ranges) {
    uint32_t total = 0;
    for (const SparseClipmapSampleRange& range : ranges) {
        total += range.slotCount;
    }
    return total;
}

}

SparseClipmapPolicy::SparseClipmapPolicy(const SparseClipmapConfig& config)
    : m_config(config)
{
    const SparseClipmapConfig defaults;
    m_config.startDistance = FiniteOr(m_config.startDistance, defaults.startDistance);
    m_config.endDistance = FiniteOr(m_config.endDistance, defaults.endDistance);
    m_config.minCellSize = FiniteOr(m_config.minCellSize, defaults.minCellSize);
    m_config.nearExitPadding = FiniteOr(m_config.nearExitPadding, defaults.nearExitPadding);
    m_config.motionLookaheadMinSpeed =
        FiniteOr(m_config.motionLookaheadMinSpeed, defaults.motionLookaheadMinSpeed);
    m_config.pumpBudgetMs = FiniteOr(m_config.pumpBudgetMs, defaults.pumpBudgetMs);
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
    m_config.interestUpdateIntervalFrames =
        std::clamp<uint32_t>(m_config.interestUpdateIntervalFrames, 1u, 12u);
    m_config.pumpBudgetMs = std::max(0.0f, m_config.pumpBudgetMs);
    if (!m_config.parallelVoxelPump) {
        m_config.parallelVoxelPumpPersistentWorkers = false;
    }
    m_config.parallelVoxelPumpMaxWorkers =
        std::clamp<uint32_t>(m_config.parallelVoxelPumpMaxWorkers, 1u, 16u);
    m_config.parallelVoxelPumpMinBricks =
        std::clamp<uint32_t>(m_config.parallelVoxelPumpMinBricks, 2u, 256u);
    if (!m_config.asyncNoncriticalGeneration &&
        !m_config.asyncVisibleCriticalGeneration) {
        m_config.asyncNoncriticalGenerationQueueMax = 0u;
    }
    m_config.asyncNoncriticalGenerationQueueMax =
        std::clamp<uint32_t>(m_config.asyncNoncriticalGenerationQueueMax, 0u, 4096u);
    m_config.asyncNoncriticalGenerationMaxEnqueuePerFrame =
        std::clamp<uint32_t>(m_config.asyncNoncriticalGenerationMaxEnqueuePerFrame, 1u, 512u);
    m_config.asyncNoncriticalGenerationMaxApplyPerFrame =
        std::clamp<uint32_t>(m_config.asyncNoncriticalGenerationMaxApplyPerFrame, 1u, 512u);
    m_config.asyncVisibleCriticalGenerationMaxEnqueuePerFrame =
        std::clamp<uint32_t>(m_config.asyncVisibleCriticalGenerationMaxEnqueuePerFrame, 1u, 512u);
    m_config.asyncVisibleCriticalGenerationMaxApplyPerFrame =
        std::clamp<uint32_t>(m_config.asyncVisibleCriticalGenerationMaxApplyPerFrame, 1u, 512u);
}

bool SparseClipmapPolicy::IsEnabled() const {
    return m_config.enabled && m_config.endDistance > m_config.startDistance;
}

float SparseClipmapPolicy::TransitionStartAfterNearExit(float nearExitDistance) const {
    if (!IsEnabled()) {
        return m_config.endDistance;
    }
    const float finiteNearExit = std::max(0.0f, FiniteOr(nearExitDistance, 0.0f));
    return std::max(m_config.startDistance, finiteNearExit + m_config.nearExitPadding);
}

float SparseClipmapPolicy::BackgroundStartAfterNearVolumeExit(float nearVolumeExitDistance) const {
    const float finiteNearExit = std::max(0.0f, FiniteOr(nearVolumeExitDistance, 0.0f));
    if (!IsEnabled()) {
        return finiteNearExit;
    }
    return std::max(m_config.startDistance, finiteNearExit + m_config.nearExitPadding);
}

float SparseClipmapPolicy::FarLayerStartAfterBackground(float backgroundStartDistance) const {
    const float finiteBackgroundStart = std::max(0.0f, FiniteOr(backgroundStartDistance, 0.0f));
    if (!IsEnabled()) {
        return finiteBackgroundStart;
    }

    const float span = m_config.endDistance - m_config.startDistance;
    const float handoffDistance = m_config.startDistance + span * 0.62f;
    // Far layers are continuity behind the mid hierarchy, not a replacement
    // for missing mid/near data. Clamp inside the clipmap range so unusually
    // large near volumes can still push the handoff later.
    return std::max(finiteBackgroundStart, std::min(m_config.endDistance, handoffDistance));
}

float SparseClipmapPolicy::MissingNearPageBackgroundStart(
    float firstMissingDistance,
    float nearVolumeExitDistance,
    float missingPagePadding) const
{
    const float paddedMissingDistance =
        std::max(0.0f, FiniteOr(firstMissingDistance, 0.0f)) +
        std::max(0.0f, FiniteOr(missingPagePadding, 0.0f));
    return std::max(paddedMissingDistance, BackgroundStartAfterNearVolumeExit(nearVolumeExitDistance));
}

bool SparseClipmapPolicy::AllowsBackgroundForMissingNearPage(
    float firstMissingDistance,
    float nearVolumeExitDistance) const
{
    if (!std::isfinite(firstMissingDistance)) {
        return false;
    }
    return firstMissingDistance >= MissingNearPageBackgroundStart(firstMissingDistance, nearVolumeExitDistance, 0.0f);
}

bool SparseClipmapPolicy::OwnsRaySegment(
    float segmentStartDistance,
    float segmentEndDistance,
    float nearExitDistance) const
{
    if (!IsEnabled() ||
        !std::isfinite(segmentStartDistance) ||
        !std::isfinite(segmentEndDistance) ||
        segmentEndDistance <= segmentStartDistance) {
        return false;
    }

    const float transitionStart = TransitionStartAfterNearExit(nearExitDistance);
    if (transitionStart >= m_config.endDistance) {
        return false;
    }
    return segmentEndDistance >= transitionStart && segmentStartDistance <= m_config.endDistance;
}

float SparseClipmapPolicy::CellSizeForDistance(float distanceFromCamera) const {
    if (!IsEnabled() || !std::isfinite(distanceFromCamera)) {
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
    metadata.endDistance = m_config.endDistance;
    metadata.minCellSize = m_config.minCellSize;
    const float adjustedStart = BackgroundStartAfterNearVolumeExit(nearVolumeExitDistance);
    metadata.enabled = IsEnabled() && adjustedStart < metadata.endDistance;
    metadata.startDistance = metadata.enabled
        ? adjustedStart
        : std::clamp(adjustedStart, 0.0f, metadata.endDistance);
    metadata.farHandoffDistance = metadata.enabled
        ? FarLayerStartAfterBackground(metadata.startDistance)
        : metadata.startDistance;
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

SparseClipmapTileCache::~SparseClipmapTileCache() {
    StopAsyncNoncriticalVoxelGenerationWorker();
    StopPersistentVoxelPumpWorkers();
}

bool SparseClipmapTileCache::Initialize(const SparseClipmapConfig& config) {
    StopAsyncNoncriticalVoxelGenerationWorker();
    StopPersistentVoxelPumpWorkers();
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
    m_voxelBacklogFirstFrame.clear();
    m_voxelInterestSet.clear();
    m_visiblePriorityVoxelSet.clear();
    m_asyncVisibleReservations.clear();
    m_asyncNoncriticalGenerationEnqueuedLastFrame = 0;
    m_asyncNoncriticalGenerationCompletedLastFrame = 0;
    m_asyncNoncriticalGenerationAppliedLastFrame = 0;
    m_asyncNoncriticalGenerationDiscardedLastFrame = 0;
    m_asyncNoncriticalGenerationDuplicateSyncLastFrame = 0;
    m_asyncVisibleCriticalGenerationEnqueuedLastFrame = 0;
    m_asyncVisibleCriticalGenerationCompletedLastFrame = 0;
    m_asyncVisibleCriticalGenerationAppliedLastFrame = 0;
    m_asyncVisibleCriticalGenerationDiscardedLastFrame = 0;
    m_asyncVisibleCriticalGenerationDuplicateSyncLastFrame = 0;
    m_asyncVisibleReservationAppliedLastFrame = 0;
    m_asyncVisibleReservationApplyDeferredLastFrame = 0;
    m_asyncVisibleReservationApplyLimitLastFrame = UINT32_MAX;
    m_asyncNoncriticalGenerationWorkerMsLastFrame = 0.0f;
    m_asyncNoncriticalGenerationApplyMsLastFrame = 0.0f;
    m_predictedVisibleAdmissionStatsFrame = 0u;
    m_lastStatsFrame = 0;
    m_pumpBudgetHitLastFrame = 0;
    m_prunedVoxelBacklogLastFrame = 0;
    m_effectivePumpBudgetMsLastFrame = 0.0f;
    m_dirtySerial = 1;
    m_heightDirtySerial = 1;
    m_voxelDirtySerial = 1;
    m_dirtyHeightStartSlot = UINT32_MAX;
    m_dirtyHeightEndSlot = 0;
    m_dirtyVoxelStartSlot = UINT32_MAX;
    m_dirtyVoxelEndSlot = 0;
    m_dirtyHeightSlots.clear();
    m_dirtyVoxelSlots.clear();

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

void SparseClipmapTileCache::SetEditStore(const SparseEditStore* edits) {
    m_edits = edits;
}

void SparseClipmapTileCache::SetFarSvoFallbackMetadata(const SparseClipmapFarSvoFallbackMetadata& metadata) {
    m_farSvoFallbackMetadata = metadata;
}

void SparseClipmapTileCache::StartPersistentVoxelPumpWorkers(uint32_t workerCount)
{
    workerCount = std::clamp<uint32_t>(workerCount, 1u, m_config.parallelVoxelPumpMaxWorkers);
    if (workerCount <= 1u) {
        StopPersistentVoxelPumpWorkers();
        return;
    }
    if (m_persistentVoxelPumpThreads.size() == static_cast<size_t>(workerCount)) {
        return;
    }

    StopPersistentVoxelPumpWorkers();
    {
        std::lock_guard<std::mutex> lock(m_persistentVoxelPumpMutex);
        m_persistentVoxelPumpStop = false;
        m_persistentVoxelPumpActive = false;
        m_persistentVoxelPumpSlots = nullptr;
        m_persistentVoxelPumpPolicy = nullptr;
        m_persistentVoxelPumpElapsedMs = nullptr;
        m_persistentVoxelPumpColumnCounters = nullptr;
        m_persistentVoxelPumpColumnEntries = nullptr;
        m_persistentVoxelPumpUseColumnCache = false;
        m_persistentVoxelPumpNext = 0;
        m_persistentVoxelPumpRemaining = 0;
    }

    m_persistentVoxelPumpThreads.reserve(workerCount);
    for (uint32_t index = 0; index < workerCount; ++index) {
        m_persistentVoxelPumpThreads.emplace_back(
            [this, index]() { PersistentVoxelPumpWorkerLoop(index); });
    }
}

void SparseClipmapTileCache::StopPersistentVoxelPumpWorkers()
{
    {
        std::lock_guard<std::mutex> lock(m_persistentVoxelPumpMutex);
        m_persistentVoxelPumpStop = true;
        m_persistentVoxelPumpActive = false;
        m_persistentVoxelPumpSlots = nullptr;
        m_persistentVoxelPumpPolicy = nullptr;
        m_persistentVoxelPumpElapsedMs = nullptr;
        m_persistentVoxelPumpColumnCounters = nullptr;
        m_persistentVoxelPumpColumnEntries = nullptr;
        m_persistentVoxelPumpUseColumnCache = false;
        m_persistentVoxelPumpNext = 0;
        m_persistentVoxelPumpRemaining = 0;
    }
    m_persistentVoxelPumpCv.notify_all();
    for (std::thread& worker : m_persistentVoxelPumpThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_persistentVoxelPumpThreads.clear();
    {
        std::lock_guard<std::mutex> lock(m_persistentVoxelPumpMutex);
        m_persistentVoxelPumpStop = false;
    }
}

void SparseClipmapTileCache::PersistentVoxelPumpWorkerLoop(uint32_t workerIndex)
{
    std::unordered_map<uint64_t, VoxelColumnSample> workerColumnCache;
    uint64_t localBatchSerial = 0;

    for (;;) {
        size_t workIndex = 0;
        const std::vector<uint32_t>* slots = nullptr;
        const SparseClipmapPolicy* policy = nullptr;
        std::vector<float>* elapsedMs = nullptr;
        std::vector<VoxelColumnCacheCounters>* columnCounters = nullptr;
        std::vector<uint32_t>* columnEntries = nullptr;
        bool useColumnCache = false;
        uint64_t batchSerial = 0;

        {
            std::unique_lock<std::mutex> lock(m_persistentVoxelPumpMutex);
            m_persistentVoxelPumpCv.wait(lock, [this]() {
                return m_persistentVoxelPumpStop || m_persistentVoxelPumpActive;
            });
            if (m_persistentVoxelPumpStop) {
                break;
            }
            if (!m_persistentVoxelPumpSlots ||
                !m_persistentVoxelPumpPolicy ||
                !m_persistentVoxelPumpElapsedMs) {
                continue;
            }
            if (m_persistentVoxelPumpNext >= m_persistentVoxelPumpSlots->size()) {
                m_persistentVoxelPumpCv.wait(lock, [this]() {
                    return m_persistentVoxelPumpStop ||
                        !m_persistentVoxelPumpActive ||
                        (m_persistentVoxelPumpSlots &&
                         m_persistentVoxelPumpNext < m_persistentVoxelPumpSlots->size());
                });
                continue;
            }

            workIndex = m_persistentVoxelPumpNext++;
            slots = m_persistentVoxelPumpSlots;
            policy = m_persistentVoxelPumpPolicy;
            elapsedMs = m_persistentVoxelPumpElapsedMs;
            columnCounters = m_persistentVoxelPumpColumnCounters;
            columnEntries = m_persistentVoxelPumpColumnEntries;
            useColumnCache = m_persistentVoxelPumpUseColumnCache;
            batchSerial = m_persistentVoxelPumpBatchSerial;
        }

        if (localBatchSerial != batchSerial) {
            workerColumnCache.clear();
            localBatchSerial = batchSerial;
        }

        const auto generateStart = std::chrono::steady_clock::now();
        VoxelColumnCacheCounters* counters = nullptr;
        if (useColumnCache &&
            columnCounters &&
            workerIndex < columnCounters->size()) {
            counters = &(*columnCounters)[workerIndex];
        }
        GenerateVoxelBrick(
            (*slots)[workIndex],
            *policy,
            useColumnCache ? &workerColumnCache : nullptr,
            counters);
        (*elapsedMs)[workIndex] = ElapsedMs(generateStart, std::chrono::steady_clock::now());
        if (useColumnCache &&
            columnEntries &&
            workerIndex < columnEntries->size()) {
            (*columnEntries)[workerIndex] = static_cast<uint32_t>(
                std::min<size_t>(workerColumnCache.size(), std::numeric_limits<uint32_t>::max()));
        }

        {
            std::lock_guard<std::mutex> lock(m_persistentVoxelPumpMutex);
            if (m_persistentVoxelPumpRemaining > 0u) {
                --m_persistentVoxelPumpRemaining;
            }
            if (m_persistentVoxelPumpRemaining == 0u) {
                m_persistentVoxelPumpActive = false;
                m_persistentVoxelPumpSlots = nullptr;
                m_persistentVoxelPumpPolicy = nullptr;
                m_persistentVoxelPumpElapsedMs = nullptr;
                m_persistentVoxelPumpColumnCounters = nullptr;
                m_persistentVoxelPumpColumnEntries = nullptr;
                m_persistentVoxelPumpUseColumnCache = false;
                m_persistentVoxelPumpDoneCv.notify_one();
            }
        }
        m_persistentVoxelPumpCv.notify_one();
    }
}

bool SparseClipmapTileCache::GenerateVoxelBricksWithPersistentWorkers(
    const std::vector<uint32_t>& slots,
    const SparseClipmapPolicy& policy,
    bool useWorkerColumnCache,
    std::vector<float>& elapsedMs,
    std::vector<VoxelColumnCacheCounters>& workerColumnCounters,
    std::vector<uint32_t>& workerColumnEntries,
    uint32_t workerCount)
{
    if (!policy.Config().parallelVoxelPumpPersistentWorkers ||
        slots.empty() ||
        elapsedMs.size() < slots.size() ||
        workerCount <= 1u) {
        return false;
    }

    StartPersistentVoxelPumpWorkers(workerCount);
    if (m_persistentVoxelPumpThreads.empty()) {
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_persistentVoxelPumpMutex);
        m_persistentVoxelPumpDoneCv.wait(lock, [this]() {
            return !m_persistentVoxelPumpActive;
        });
        ++m_persistentVoxelPumpBatchSerial;
        m_persistentVoxelPumpSlots = &slots;
        m_persistentVoxelPumpPolicy = &policy;
        m_persistentVoxelPumpElapsedMs = &elapsedMs;
        m_persistentVoxelPumpColumnCounters = &workerColumnCounters;
        m_persistentVoxelPumpColumnEntries = &workerColumnEntries;
        m_persistentVoxelPumpUseColumnCache = useWorkerColumnCache;
        m_persistentVoxelPumpNext = 0;
        m_persistentVoxelPumpRemaining = slots.size();
        m_persistentVoxelPumpActive = true;
    }
    m_persistentVoxelPumpCv.notify_all();

    {
        std::unique_lock<std::mutex> lock(m_persistentVoxelPumpMutex);
        m_persistentVoxelPumpDoneCv.wait(lock, [this]() {
            return !m_persistentVoxelPumpActive;
        });
    }
    return true;
}

void SparseClipmapTileCache::StartAsyncNoncriticalVoxelGenerationWorkerIfNeeded()
{
    if ((!m_config.asyncNoncriticalGeneration &&
         !m_config.asyncVisibleCriticalGeneration) ||
        m_asyncNoncriticalGenerationThread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        m_asyncNoncriticalGenerationStop = false;
    }

    const SparseTerrainGenerator terrain = m_terrain;
    m_asyncNoncriticalGenerationThread = std::thread([this, terrain]() {
        SparseClipmapTileCache generator;
        generator.m_terrain = terrain;
        std::unordered_map<uint64_t, VoxelColumnSample> columnCache;
        columnCache.reserve(8192);
        VoxelColumnCacheCounters columnCounters;
        for (;;) {
            AsyncVoxelGenerationRequest request;
            {
                std::unique_lock<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
                m_asyncNoncriticalGenerationCv.wait(lock, [this]() {
                    return m_asyncNoncriticalGenerationStop ||
                        !m_asyncNoncriticalGenerationQueue.empty();
                });
                if (m_asyncNoncriticalGenerationStop) {
                    break;
                }
                request = std::move(m_asyncNoncriticalGenerationQueue.front());
                m_asyncNoncriticalGenerationQueue.pop_front();
            }

            SparseClipmapPolicy requestPolicy(request.config);
            generator.m_config = requestPolicy.Config();
            VoxelBrickPayload payload;
            payload.coord = request.coord;
            payload.slot = UINT32_MAX;
            payload.lastTouchedFrame = request.requestFrame;
            columnCounters = {};
            const auto start = std::chrono::steady_clock::now();
            generator.GenerateVoxelBrickPayload(
                payload,
                requestPolicy,
                &columnCache,
                &columnCounters);
            const float elapsedMs = ElapsedMs(start, std::chrono::steady_clock::now());
            if (columnCache.size() > 65536u) {
                columnCache.clear();
                columnCache.reserve(8192);
            }

            AsyncVoxelGenerationResult result;
            result.coord = request.coord;
            result.brick = std::move(payload);
            result.requestFrame = request.requestFrame;
            result.editRevision = request.editRevision;
            result.workerMs = elapsedMs;
            result.visibleCritical = request.visibleCritical;
            {
                std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
                if (m_asyncNoncriticalGenerationStop) {
                    break;
                }
                m_asyncNoncriticalGenerationResults.push_back(std::move(result));
            }
        }
    });
}

void SparseClipmapTileCache::StopAsyncNoncriticalVoxelGenerationWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        m_asyncNoncriticalGenerationStop = true;
    }
    m_asyncNoncriticalGenerationCv.notify_all();
    if (m_asyncNoncriticalGenerationThread.joinable()) {
        m_asyncNoncriticalGenerationThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        m_asyncNoncriticalGenerationQueue.clear();
        m_asyncNoncriticalGenerationResults.clear();
        m_asyncNoncriticalGenerationPending.clear();
        m_asyncNoncriticalGenerationStop = false;
    }
}

bool SparseClipmapTileCache::TryQueueAsyncNoncriticalVoxelGeneration(
    const SparseVoxelClipmapCoord& coord,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy)
{
    return TryQueueAsyncVoxelGeneration(coord, frameIndex, policy, false);
}

bool SparseClipmapTileCache::TryQueueAsyncVoxelGeneration(
    const SparseVoxelClipmapCoord& coord,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    bool visibleCritical,
    bool allowVisibleReservation,
    bool bypassEnqueueLimit)
{
    const SparseClipmapConfig& config = policy.Config();
    const bool coordIsVisibleCritical =
        m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end();
    const bool coordIsVisibleReservation =
        m_asyncVisibleReservations.find(coord) !=
        m_asyncVisibleReservations.end();
    const bool canUseVisibleReservation =
        visibleCritical && allowVisibleReservation && coordIsVisibleReservation;
    if ((visibleCritical && !config.asyncVisibleCriticalGeneration) ||
        (!visibleCritical && !config.asyncNoncriticalGeneration) ||
        policy.Config().asyncNoncriticalGenerationQueueMax == 0u ||
        policy.Config().sharedVoxelColumnCache ||
        (m_edits && m_edits->EditedBrickCount() != 0u) ||
        (visibleCritical
            ? (!coordIsVisibleCritical && !(allowVisibleReservation && coordIsVisibleReservation))
            : coordIsVisibleCritical || coordIsVisibleReservation) ||
        m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end() ||
        m_queuedVoxelSet.find(coord) == m_queuedVoxelSet.end() ||
        (!canUseVisibleReservation &&
         !m_voxelInterestSet.empty() &&
         m_voxelInterestSet.find(coord) == m_voxelInterestSet.end())) {
        return false;
    }

    StartAsyncNoncriticalVoxelGenerationWorkerIfNeeded();
    if (!m_asyncNoncriticalGenerationThread.joinable()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        if (m_asyncNoncriticalGenerationPending.find(coord) !=
            m_asyncNoncriticalGenerationPending.end()) {
            return true;
        }
        const uint32_t enqueuedThisFrame = visibleCritical
            ? m_asyncVisibleCriticalGenerationEnqueuedLastFrame
            : m_asyncNoncriticalGenerationEnqueuedLastFrame;
        const uint32_t enqueueLimit = visibleCritical
            ? config.asyncVisibleCriticalGenerationMaxEnqueuePerFrame
            : config.asyncNoncriticalGenerationMaxEnqueuePerFrame;
        if (!bypassEnqueueLimit && enqueuedThisFrame >= enqueueLimit) {
            return false;
        }
        if (m_asyncNoncriticalGenerationPending.size() >=
            static_cast<size_t>(config.asyncNoncriticalGenerationQueueMax)) {
            return false;
        }

        AsyncVoxelGenerationRequest request;
        request.coord = coord;
        request.config = policy.Config();
        request.requestFrame = frameIndex;
        request.editRevision = m_edits ? m_edits->RevisionSerial() : 0ull;
        request.visibleCritical = visibleCritical;
        m_asyncNoncriticalGenerationPending.insert(coord);
        m_asyncNoncriticalGenerationQueue.push_back(std::move(request));
    }
    m_asyncNoncriticalGenerationCv.notify_one();
    if (visibleCritical) {
        ++m_asyncVisibleCriticalGenerationEnqueuedLastFrame;
    } else {
        ++m_asyncNoncriticalGenerationEnqueuedLastFrame;
    }
    return true;
}

uint32_t SparseClipmapTileCache::QueueAsyncVoxelGenerationMatchingPriority(
    bool requireVisiblePriority,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy)
{
    const SparseClipmapConfig& config = policy.Config();
    const bool asyncAllowed =
        requireVisiblePriority
            ? config.asyncVisibleCriticalGeneration
            : config.asyncNoncriticalGeneration;
    if (!asyncAllowed ||
        config.asyncNoncriticalGenerationQueueMax == 0u ||
        m_voxelGenerationQueue.empty() ||
        (m_edits && m_edits->EditedBrickCount() != 0u)) {
        return 0u;
    }

    uint32_t queuedAsync = 0u;
    const uint32_t asyncEnqueueLimit = requireVisiblePriority
        ? config.asyncVisibleCriticalGenerationMaxEnqueuePerFrame
        : config.asyncNoncriticalGenerationMaxEnqueuePerFrame;
    if (requireVisiblePriority && !m_asyncVisibleReservations.empty()) {
        struct VisibleCandidate {
            SparseVoxelClipmapCoord coord;
            uint32_t deadlineFrame = UINT32_MAX;
            uint32_t sampleIndex = UINT32_MAX;
            uint32_t firstFrame = UINT32_MAX;
            uint32_t currentVisible = 0u;
        };
        std::vector<VisibleCandidate> candidates;
        candidates.reserve(m_voxelGenerationQueue.size());
        for (const SparseVoxelClipmapCoord& coord : m_voxelGenerationQueue) {
            const bool currentVisible =
                m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end();
            const auto reservationIt = m_asyncVisibleReservations.find(coord);
            const bool visibleReservation =
                reservationIt != m_asyncVisibleReservations.end();
            if (!currentVisible && !visibleReservation) {
                continue;
            }
            if (!visibleReservation &&
                !m_voxelInterestSet.empty() &&
                m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
                continue;
            }
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                continue;
            }
            VisibleCandidate candidate;
            candidate.coord = coord;
            candidate.currentVisible = currentVisible ? 1u : 0u;
            if (currentVisible) {
                candidate.deadlineFrame = frameIndex;
                candidate.sampleIndex = 0u;
                candidate.firstFrame = frameIndex;
            } else {
                candidate.deadlineFrame = reservationIt->second.deadlineFrame;
                candidate.sampleIndex = reservationIt->second.sampleIndex;
                candidate.firstFrame = reservationIt->second.firstFrame;
            }
            candidates.push_back(candidate);
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const VisibleCandidate& a, const VisibleCandidate& b) {
                if (a.currentVisible != b.currentVisible) {
                    return a.currentVisible > b.currentVisible;
                }
                if (a.deadlineFrame != b.deadlineFrame) {
                    return a.deadlineFrame < b.deadlineFrame;
                }
                if (a.sampleIndex != b.sampleIndex) {
                    return a.sampleIndex < b.sampleIndex;
                }
                if (a.firstFrame != b.firstFrame) {
                    return a.firstFrame < b.firstFrame;
                }
                if (a.coord.ring != b.coord.ring) {
                    return a.coord.ring < b.coord.ring;
                }
                if (a.coord.y != b.coord.y) {
                    return a.coord.y < b.coord.y;
                }
                if (a.coord.x != b.coord.x) {
                    return a.coord.x < b.coord.x;
                }
                return a.coord.z < b.coord.z;
            });
        std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> pendingSnapshot;
        size_t pendingCount = 0u;
        {
            std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
            pendingCount = m_asyncNoncriticalGenerationPending.size();
            pendingSnapshot = m_asyncNoncriticalGenerationPending;
        }
        const uint32_t pendingFillLimit =
            pendingCount >= static_cast<size_t>(config.asyncNoncriticalGenerationQueueMax)
                ? 0u
                : static_cast<uint32_t>(
                      std::min<size_t>(
                          static_cast<size_t>(config.asyncNoncriticalGenerationQueueMax) -
                              pendingCount,
                          static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        const uint32_t deadlineFillLimit =
            std::max(asyncEnqueueLimit, pendingFillLimit);
        for (const VisibleCandidate& candidate : candidates) {
            if (queuedAsync >= deadlineFillLimit) {
                break;
            }
            if (pendingSnapshot.find(candidate.coord) != pendingSnapshot.end()) {
                continue;
            }
            if (TryQueueAsyncVoxelGeneration(
                    candidate.coord,
                    frameIndex,
                    policy,
                    true,
                    true,
                    queuedAsync >= asyncEnqueueLimit)) {
                pendingSnapshot.insert(candidate.coord);
                ++queuedAsync;
            }
        }
        return queuedAsync;
    }
    for (auto it = m_voxelGenerationQueue.begin();
         it != m_voxelGenerationQueue.end() && queuedAsync < asyncEnqueueLimit;
         ++it) {
        const SparseVoxelClipmapCoord coord = *it;
        const bool visiblePriority =
            m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end() ||
            m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
        if (visiblePriority != requireVisiblePriority) {
            continue;
        }
        const bool visibleReservation =
            m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
        if (!visibleReservation &&
            !m_voxelInterestSet.empty() &&
            m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            continue;
        }
        if (TryQueueAsyncVoxelGeneration(
                coord,
                frameIndex,
                policy,
                requireVisiblePriority)) {
            ++queuedAsync;
        }
    }
    return queuedAsync;
}

void SparseClipmapTileCache::PrioritizeAsyncVoxelGenerationQueue()
{
    std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
    if (m_asyncNoncriticalGenerationQueue.size() <= 1u) {
        for (AsyncVoxelGenerationRequest& request : m_asyncNoncriticalGenerationQueue) {
            if (m_visiblePriorityVoxelSet.find(request.coord) != m_visiblePriorityVoxelSet.end() ||
                m_asyncVisibleReservations.find(request.coord) != m_asyncVisibleReservations.end()) {
                request.visibleCritical = true;
            }
        }
        return;
    }

    for (AsyncVoxelGenerationRequest& request : m_asyncNoncriticalGenerationQueue) {
        if (m_visiblePriorityVoxelSet.find(request.coord) != m_visiblePriorityVoxelSet.end() ||
            m_asyncVisibleReservations.find(request.coord) != m_asyncVisibleReservations.end()) {
            request.visibleCritical = true;
        }
    }

    std::stable_sort(
        m_asyncNoncriticalGenerationQueue.begin(),
        m_asyncNoncriticalGenerationQueue.end(),
        [this](const AsyncVoxelGenerationRequest& a, const AsyncVoxelGenerationRequest& b) {
            const bool aCurrentVisible =
                m_visiblePriorityVoxelSet.find(a.coord) != m_visiblePriorityVoxelSet.end();
            const bool bCurrentVisible =
                m_visiblePriorityVoxelSet.find(b.coord) != m_visiblePriorityVoxelSet.end();
            if (aCurrentVisible != bCurrentVisible) {
                return aCurrentVisible;
            }

            const auto aReservation = m_asyncVisibleReservations.find(a.coord);
            const auto bReservation = m_asyncVisibleReservations.find(b.coord);
            const bool aReserved = aReservation != m_asyncVisibleReservations.end();
            const bool bReserved = bReservation != m_asyncVisibleReservations.end();
            if (aReserved != bReserved) {
                return aReserved;
            }
            if (aReserved && bReserved) {
                if (aReservation->second.deadlineFrame != bReservation->second.deadlineFrame) {
                    return aReservation->second.deadlineFrame < bReservation->second.deadlineFrame;
                }
                if (aReservation->second.sampleIndex != bReservation->second.sampleIndex) {
                    return aReservation->second.sampleIndex < bReservation->second.sampleIndex;
                }
                if (aReservation->second.firstFrame != bReservation->second.firstFrame) {
                    return aReservation->second.firstFrame < bReservation->second.firstFrame;
                }
            }
            if (a.visibleCritical != b.visibleCritical) {
                return a.visibleCritical;
            }
            if (a.requestFrame != b.requestFrame) {
                return a.requestFrame < b.requestFrame;
            }
            if (a.coord.ring != b.coord.ring) {
                return a.coord.ring < b.coord.ring;
            }
            if (a.coord.y != b.coord.y) {
                return a.coord.y < b.coord.y;
            }
            if (a.coord.x != b.coord.x) {
                return a.coord.x < b.coord.x;
            }
            return a.coord.z < b.coord.z;
        });
}

bool SparseClipmapTileCache::RemoveQueuedVoxelCoord(const SparseVoxelClipmapCoord& coord)
{
    bool removed = false;
    for (auto it = m_voxelGenerationQueue.begin(); it != m_voxelGenerationQueue.end();) {
        if (*it == coord) {
            it = m_voxelGenerationQueue.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        m_queuedVoxelSet.erase(coord);
        m_voxelBacklogFirstFrame.erase(coord);
        m_visiblePriorityVoxelSet.erase(coord);
        m_asyncVisibleReservations.erase(coord);
    }
    return removed;
}

void SparseClipmapTileCache::PruneAsyncVisibleReservations(
    uint32_t frameIndex,
    uint32_t staleFrames)
{
    if (!m_config.asyncVisibleCriticalGeneration) {
        m_asyncVisibleReservations.clear();
        return;
    }
    for (auto it = m_asyncVisibleReservations.begin();
         it != m_asyncVisibleReservations.end();) {
        const SparseVoxelClipmapCoord coord = it->first;
        const AsyncVisibleReservation& reservation = it->second;
        const bool stale =
            staleFrames != UINT32_MAX &&
            frameIndex > reservation.lastSeenFrame &&
            frameIndex - reservation.lastSeenFrame > staleFrames;
        if (stale ||
            m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end() ||
            m_queuedVoxelSet.find(coord) == m_queuedVoxelSet.end()) {
            it = m_asyncVisibleReservations.erase(it);
            continue;
        }
        ++it;
    }
}

uint32_t SparseClipmapTileCache::ApplyAsyncNoncriticalVoxelGenerationCompletions(
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t maxNoncriticalApply,
    uint32_t maxVisibleCriticalApply,
    uint32_t maxVisibleReservationApply)
{
    m_asyncVisibleReservationAppliedLastFrame = 0u;
    m_asyncVisibleReservationApplyDeferredLastFrame = 0u;
    m_asyncVisibleReservationApplyLimitLastFrame = UINT32_MAX;
    if (!policy.Config().asyncNoncriticalGeneration &&
        !policy.Config().asyncVisibleCriticalGeneration) {
        StopAsyncNoncriticalVoxelGenerationWorker();
        RefreshStats();
        return 0u;
    }

    const uint32_t noncriticalApplyLimit = std::min<uint32_t>(
        maxNoncriticalApply == UINT32_MAX
            ? policy.Config().asyncNoncriticalGenerationMaxApplyPerFrame
            : maxNoncriticalApply,
        policy.Config().asyncNoncriticalGenerationMaxApplyPerFrame);
    const uint32_t visibleCriticalApplyLimit = std::min<uint32_t>(
        maxVisibleCriticalApply == UINT32_MAX
            ? policy.Config().asyncVisibleCriticalGenerationMaxApplyPerFrame
            : maxVisibleCriticalApply,
        policy.Config().asyncVisibleCriticalGenerationMaxApplyPerFrame);
    const uint32_t visibleReservationApplyLimit =
        maxVisibleReservationApply == UINT32_MAX
            ? UINT32_MAX
            : std::min<uint32_t>(
                  maxVisibleReservationApply,
                  policy.Config().asyncVisibleCriticalGenerationMaxApplyPerFrame);
    m_asyncVisibleReservationApplyLimitLastFrame = visibleReservationApplyLimit;
    if (noncriticalApplyLimit == 0u && visibleCriticalApplyLimit == 0u) {
        RefreshStats();
        return 0u;
    }

    const auto applyStart = std::chrono::steady_clock::now();
    uint32_t applied = 0u;
    uint32_t scanned = 0u;
    size_t maxScans = 0u;
    {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        maxScans = m_asyncNoncriticalGenerationResults.size();
    }

    uint32_t appliedNoncritical = 0u;
    uint32_t appliedVisibleCritical = 0u;
    uint32_t appliedVisibleReservation = 0u;
    while ((appliedNoncritical < noncriticalApplyLimit ||
            appliedVisibleCritical < visibleCriticalApplyLimit) &&
           scanned < static_cast<uint32_t>(maxScans)) {
        AsyncVoxelGenerationResult result;
        {
            std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
            if (m_asyncNoncriticalGenerationResults.empty()) {
                break;
            }
            result = std::move(m_asyncNoncriticalGenerationResults.front());
            m_asyncNoncriticalGenerationResults.pop_front();
        }
        ++scanned;

        const bool queuedVisibleCriticalResult = result.visibleCritical;
        const bool currentVisibleCriticalResult =
            m_visiblePriorityVoxelSet.find(result.coord) !=
                m_visiblePriorityVoxelSet.end();
        const bool currentVisibleReservationResult =
            m_asyncVisibleReservations.find(result.coord) !=
                m_asyncVisibleReservations.end();
        const bool visibleCriticalResult =
            queuedVisibleCriticalResult ||
            currentVisibleCriticalResult ||
            currentVisibleReservationResult;
        const bool visibleReservationResult =
            visibleCriticalResult &&
            !currentVisibleCriticalResult &&
            currentVisibleReservationResult;
        if ((visibleCriticalResult &&
             appliedVisibleCritical >= visibleCriticalApplyLimit) ||
            (!visibleCriticalResult &&
             appliedNoncritical >= noncriticalApplyLimit)) {
            std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
            m_asyncNoncriticalGenerationResults.push_back(std::move(result));
            continue;
        }
        if (visibleReservationResult &&
            appliedVisibleReservation >= visibleReservationApplyLimit) {
            ++m_asyncVisibleReservationApplyDeferredLastFrame;
            std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
            m_asyncNoncriticalGenerationResults.push_back(std::move(result));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
            m_asyncNoncriticalGenerationPending.erase(result.coord);
        }
        if (visibleCriticalResult) {
            ++m_asyncVisibleCriticalGenerationCompletedLastFrame;
        } else {
            ++m_asyncNoncriticalGenerationCompletedLastFrame;
        }
        m_asyncNoncriticalGenerationWorkerMsLastFrame += result.workerMs;

        const uint64_t currentEditRevision = m_edits ? m_edits->RevisionSerial() : 0ull;
        const bool staleEditRevision = result.editRevision != currentEditRevision;
        const bool editsActive = m_edits && m_edits->EditedBrickCount() != 0u;
        const bool stillVisibleCritical =
            m_visiblePriorityVoxelSet.find(result.coord) != m_visiblePriorityVoxelSet.end() ||
            m_asyncVisibleReservations.find(result.coord) !=
                m_asyncVisibleReservations.end();
        const bool noLongerInterested =
            !m_voxelInterestSet.empty() &&
            m_voxelInterestSet.find(result.coord) == m_voxelInterestSet.end() &&
            !(visibleCriticalResult && stillVisibleCritical);
        if (staleEditRevision ||
            editsActive ||
            noLongerInterested ||
            (visibleCriticalResult && !stillVisibleCritical)) {
            RemoveQueuedVoxelCoord(result.coord);
            if (visibleCriticalResult) {
                ++m_asyncVisibleCriticalGenerationDiscardedLastFrame;
            } else {
                ++m_asyncNoncriticalGenerationDiscardedLastFrame;
            }
            continue;
        }
        if (m_voxelSlotByCoord.find(result.coord) != m_voxelSlotByCoord.end()) {
            RemoveQueuedVoxelCoord(result.coord);
            if (visibleCriticalResult) {
                ++m_asyncVisibleCriticalGenerationDuplicateSyncLastFrame;
            } else {
                ++m_asyncNoncriticalGenerationDuplicateSyncLastFrame;
            }
            continue;
        }
        if (!visibleCriticalResult &&
            m_freeVoxelSlots.empty() &&
            m_voxelSlotByCoord.size() >= m_voxelBricks.size()) {
            ++m_asyncNoncriticalGenerationDiscardedLastFrame;
            continue;
        }

        const uint32_t slot = AllocateVoxelSlot(result.coord, frameIndex);
        if (slot == UINT32_MAX) {
            if (visibleCriticalResult) {
                ++m_asyncVisibleCriticalGenerationDiscardedLastFrame;
            } else {
                ++m_asyncNoncriticalGenerationDiscardedLastFrame;
            }
            continue;
        }
        result.brick.slot = slot;
        result.brick.lastTouchedFrame = frameIndex;
        m_voxelBricks[slot] = std::move(result.brick);
        m_voxelSlotByCoord[result.coord] = slot;
        RemoveQueuedVoxelCoord(result.coord);
        RecordVoxelGenerationTiming(result.coord, result.workerMs);
        ++m_backlogVoxelPumpedLastFrame;
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        MarkVoxelSlotDirty(slot);
        ++applied;
        if (visibleCriticalResult) {
            ++appliedVisibleCritical;
            ++m_asyncVisibleCriticalGenerationAppliedLastFrame;
            if (visibleReservationResult) {
                ++appliedVisibleReservation;
                ++m_asyncVisibleReservationAppliedLastFrame;
            }
        } else {
            ++appliedNoncritical;
            ++m_asyncNoncriticalGenerationAppliedLastFrame;
        }
    }
    m_asyncNoncriticalGenerationApplyMsLastFrame +=
        ElapsedMs(applyStart, std::chrono::steady_clock::now());
    RefreshStats(0u, 0u, applied, 0u);
    return applied;
}

SparseClipmapTileCache::InterestSignature SparseClipmapTileCache::BuildInterestSignature(
    float cameraX,
    float cameraY,
    float cameraZ,
    const SparseClipmapPolicy& policy,
    float forwardX,
    float forwardY,
    float forwardZ,
    float velocityX,
    float velocityY,
    float velocityZ,
    float predictionSeconds) const
{
    const SparseClipmapConfig& config = policy.Config();
    InterestSignature signature;
    const float footprintCameraQuantum = 1.0f / std::max(
        config.minCellSize,
        config.minCellSize * static_cast<float>(SPARSE_BRICK_SIZE) * 0.125f);
    const float cameraQuantum = config.footprintInterestSignature
        ? footprintCameraQuantum
        : (1.0f / std::max(1.0f, config.minCellSize * 0.5f));
    const float velocityQuantum = config.footprintInterestSignature
        ? (1.0f / std::max(config.motionLookaheadMinSpeed, config.minCellSize * 4.0f))
        : (1.0f / std::max(1.0f, config.minCellSize));
    signature.cameraX = QuantizeFloatToInt(cameraX, cameraQuantum);
    signature.cameraY = QuantizeFloatToInt(cameraY, cameraQuantum);
    signature.cameraZ = QuantizeFloatToInt(cameraZ, cameraQuantum);
    const float forwardQuantum = config.footprintInterestSignature ? 16.0f : 64.0f;
    signature.forwardX = QuantizeFloatToInt(forwardX, forwardQuantum);
    signature.forwardY = QuantizeFloatToInt(forwardY, forwardQuantum);
    signature.forwardZ = QuantizeFloatToInt(forwardZ, forwardQuantum);
    signature.velocityX = QuantizeFloatToInt(velocityX, velocityQuantum);
    signature.velocityY = QuantizeFloatToInt(velocityY, velocityQuantum);
    signature.velocityZ = QuantizeFloatToInt(velocityZ, velocityQuantum);
    signature.predictionMillis = QuantizeFloatToUint(predictionSeconds, 1000.0f);
    signature.startDistance = QuantizeFloatToUint(config.startDistance, 100.0f);
    signature.endDistance = QuantizeFloatToUint(config.endDistance, 100.0f);
    signature.minCellSize = QuantizeFloatToUint(config.minCellSize, 100.0f);
    signature.tileRadius = config.tileRadius;
    signature.tileSampleSide = config.tileSampleSide;
    signature.ringCount = config.ringCount;
    signature.heightClipmapEnabled = config.heightClipmapEnabled ? 1u : 0u;
    signature.voxelClipmapEnabled = config.voxelClipmapEnabled ? 1u : 0u;
    signature.voxelBrickRadiusXz = config.voxelBrickRadiusXz;
    signature.voxelBrickRadiusY = config.voxelBrickRadiusY;
    signature.voxelInterestCapacityPercent = config.voxelInterestCapacityPercent;
    signature.motionLookaheadMinSpeed = QuantizeFloatToUint(config.motionLookaheadMinSpeed, 100.0f);
    signature.motionLookaheadSteps = config.motionLookaheadSteps;
    signature.interestUpdateIntervalFrames = config.interestUpdateIntervalFrames;
    signature.footprintInterestSignature = config.footprintInterestSignature ? 1u : 0u;
    signature.backlogAwarePump = config.backlogAwarePump ? 1u : 0u;
    signature.pumpBudgetMs = QuantizeFloatToUint(config.pumpBudgetMs, 100.0f);
    signature.drainReuseDiagnostics = config.drainReuseDiagnostics ? 1u : 0u;
    signature.fallbackValidityClassifier = config.fallbackValidityClassifier ? 1u : 0u;
    signature.fallbackContractDiagnostics = config.fallbackContractDiagnostics ? 1u : 0u;
    signature.farSvoFallbackProof = config.farSvoFallbackProof ? 1u : 0u;
    signature.asyncNoncriticalGeneration = config.asyncNoncriticalGeneration ? 1u : 0u;
    signature.asyncVisibleCriticalGeneration = config.asyncVisibleCriticalGeneration ? 1u : 0u;
    signature.asyncNoncriticalGenerationQueueMax = config.asyncNoncriticalGenerationQueueMax;
    signature.asyncNoncriticalGenerationMaxEnqueuePerFrame =
        config.asyncNoncriticalGenerationMaxEnqueuePerFrame;
    signature.asyncNoncriticalGenerationMaxApplyPerFrame =
        config.asyncNoncriticalGenerationMaxApplyPerFrame;
    signature.asyncVisibleCriticalGenerationMaxEnqueuePerFrame =
        config.asyncVisibleCriticalGenerationMaxEnqueuePerFrame;
    signature.asyncVisibleCriticalGenerationMaxApplyPerFrame =
        config.asyncVisibleCriticalGenerationMaxApplyPerFrame;
    signature.voxelInterestDetail = config.voxelInterestDetail ? 1u : 0u;
    signature.voxelInterestSignatureReuse = config.voxelInterestSignatureReuse ? 1u : 0u;
    signature.voxelInterestSignatureReuseMaxAgeFrames =
        config.voxelInterestSignatureReuseMaxAgeFrames;
    signature.sharedVoxelColumnCache = config.sharedVoxelColumnCache ? 1u : 0u;
    signature.directVoxelFootprintColumns = config.directVoxelFootprintColumns ? 1u : 0u;
    signature.parallelWorkerColumnCache = config.parallelWorkerColumnCache ? 1u : 0u;
    signature.parallelVoxelPump = config.parallelVoxelPump ? 1u : 0u;
    signature.parallelVoxelPumpPersistentWorkers =
        config.parallelVoxelPumpPersistentWorkers ? 1u : 0u;
    signature.parallelVoxelPumpMaxWorkers = config.parallelVoxelPumpMaxWorkers;
    signature.parallelVoxelPumpMinBricks = config.parallelVoxelPumpMinBricks;
    return signature;
}

SparseClipmapTileCache::InterestSignature SparseClipmapTileCache::BuildVoxelInterestSignature(
    float cameraX,
    float cameraY,
    float cameraZ,
    const SparseClipmapPolicy& policy,
    float forwardX,
    float forwardY,
    float forwardZ,
    float velocityX,
    float velocityY,
    float velocityZ,
    float predictionSeconds) const
{
    const SparseClipmapConfig& config = policy.Config();
    InterestSignature signature = BuildInterestSignature(
        cameraX,
        cameraY,
        cameraZ,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        velocityX,
        velocityY,
        velocityZ,
        predictionSeconds);
    const float fineVoxelWorldSize = std::max(
        1.0f,
        config.minCellSize * static_cast<float>(SPARSE_BRICK_SIZE));
    const float cameraQuantum = 1.0f / std::max(1.0f, fineVoxelWorldSize * 0.5f);
    const float cameraYQuantum = 1.0f / std::max(1.0f, fineVoxelWorldSize);
    signature.cameraX = QuantizeFloatToInt(cameraX, cameraQuantum);
    signature.cameraY = QuantizeFloatToInt(cameraY, cameraYQuantum);
    signature.cameraZ = QuantizeFloatToInt(cameraZ, cameraQuantum);
    signature.forwardX = QuantizeFloatToInt(forwardX, 4.0f);
    signature.forwardY = QuantizeFloatToInt(forwardY, 4.0f);
    signature.forwardZ = QuantizeFloatToInt(forwardZ, 4.0f);
    const float velocityQuantum = 1.0f / std::max(
        fineVoxelWorldSize,
        config.motionLookaheadMinSpeed);
    signature.velocityX = QuantizeFloatToInt(velocityX, velocityQuantum);
    signature.velocityY = QuantizeFloatToInt(velocityY, velocityQuantum);
    signature.velocityZ = QuantizeFloatToInt(velocityZ, velocityQuantum);
    return signature;
}

void SparseClipmapTileCache::RefreshInterestTouchFrames(uint32_t frameIndex) {
    for (const SparseClipmapTileCoord& coord : m_interestSet) {
        auto existing = m_slotByCoord.find(coord);
        if (existing != m_slotByCoord.end()) {
            m_tiles[existing->second].record.lastTouchedFrame = frameIndex;
        }
    }
    for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
        auto existing = m_voxelSlotByCoord.find(coord);
        if (existing != m_voxelSlotByCoord.end()) {
            m_voxelBricks[existing->second].lastTouchedFrame = frameIndex;
        }
    }
}

uint32_t SparseClipmapTileCache::InvalidateEditedOverlays(
    const SparseEditStore& edits,
    const SparseClipmapPolicy& policy)
{
    if (!policy.IsEnabled() || m_voxelBricks.empty()) {
        return 0;
    }

    std::unordered_set<uint32_t> invalidatedSlots;
    edits.ForEachOverlay([&](const BrickEditOverlay& overlay) {
        int32_t editMinX = 0;
        int32_t editMinY = 0;
        int32_t editMinZ = 0;
        int32_t editMaxX = 0;
        int32_t editMaxY = 0;
        int32_t editMaxZ = 0;
        if (!TryWorldVoxelFromBrickLocal(overlay.coord.x, 0, &editMinX) ||
            !TryWorldVoxelFromBrickLocal(overlay.coord.y, 0, &editMinY) ||
            !TryWorldVoxelFromBrickLocal(overlay.coord.z, 0, &editMinZ) ||
            !TryWorldVoxelFromBrickLocal(overlay.coord.x, SPARSE_BRICK_SIZE - 1u, &editMaxX) ||
            !TryWorldVoxelFromBrickLocal(overlay.coord.y, SPARSE_BRICK_SIZE - 1u, &editMaxY) ||
            !TryWorldVoxelFromBrickLocal(overlay.coord.z, SPARSE_BRICK_SIZE - 1u, &editMaxZ)) {
            return;
        }

        for (const auto& [coord, slot] : m_voxelSlotByCoord) {
            (void)coord;
            if (slot >= m_voxelBricks.size()) {
                continue;
            }
            const VoxelBrickPayload& brick = m_voxelBricks[slot];
            if (brick.slot == UINT32_MAX || invalidatedSlots.find(slot) != invalidatedSlots.end()) {
                continue;
            }
            const int32_t brickWorldSize = std::max(1, RoundToInt32Clamped(
                static_cast<double>(std::max(1.0f, brick.cellSize)) *
                static_cast<double>(SPARSE_BRICK_SIZE)));
            const int32_t brickMinX = brick.originX;
            const int32_t brickMinY = brick.originY;
            const int32_t brickMinZ = brick.originZ;
            const int32_t brickMaxX = SaturatingAddInt32(brick.originX, brickWorldSize - 1);
            const int32_t brickMaxY = SaturatingAddInt32(brick.originY, brickWorldSize - 1);
            const int32_t brickMaxZ = SaturatingAddInt32(brick.originZ, brickWorldSize - 1);
            const bool overlaps =
                editMinX <= brickMaxX && editMaxX >= brickMinX &&
                editMinY <= brickMaxY && editMaxY >= brickMinY &&
                editMinZ <= brickMaxZ && editMaxZ >= brickMinZ;
            if (overlaps) {
                invalidatedSlots.insert(slot);
            }
        }
    });

    for (uint32_t slot : invalidatedSlots) {
        GenerateVoxelBrick(slot, policy);
        MarkVoxelSlotDirty(slot);
    }
    if (!invalidatedSlots.empty()) {
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        RefreshStats(0, 0, static_cast<uint32_t>(invalidatedSlots.size()), 0);
    }
    return static_cast<uint32_t>(invalidatedSlots.size());
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
    m_lastStatsFrame = frameIndex;
    m_prunedVoxelBacklogLastFrame = 0;
    m_newlyInterestedTilesLastFrame = 0;
    m_newlyInterestedVoxelBricksLastFrame = 0;
    m_noLongerInterestedTilesLastFrame = 0;
    m_noLongerInterestedVoxelBricksLastFrame = 0;
    m_residentInterestedTilesLastFrame = 0;
    m_residentInterestedVoxelBricksLastFrame = 0;
    m_reusedInterestedTilesLastFrame = 0;
    m_reusedInterestedVoxelBricksLastFrame = 0;
    m_voxelInterestLineMsLastFrame = 0.0f;
    m_voxelInterestAnchorMsLastFrame = 0.0f;
    m_voxelInterestSortEmitMsLastFrame = 0.0f;
    m_voxelInterestBacklogMsLastFrame = 0.0f;
    m_voxelInterestDiagnosticsMsLastFrame = 0.0f;
    m_voxelInterestCandidatesLastFrame = 0;
    m_voxelInterestCandidateAttemptsLastFrame = 0;
    m_voxelInterestCandidateDuplicateHitsLastFrame = 0;
    m_voxelInterestCandidateScoreUpdatesLastFrame = 0;
    m_voxelInterestCandidateMaxRingUniqueLastFrame = 0;
    m_voxelInterestCandidateMaxRingAttemptsLastFrame = 0;
    m_voxelInterestLineCandidateAttemptsLastFrame = 0;
    m_voxelInterestLineCandidateDuplicateHitsLastFrame = 0;
    m_voxelInterestLineCandidateScoreUpdatesLastFrame = 0;
    m_voxelInterestAnchorTerrainCandidateAttemptsLastFrame = 0;
    m_voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame = 0;
    m_voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame = 0;
    m_voxelInterestAnchorFootprintCandidateAttemptsLastFrame = 0;
    m_voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame = 0;
    m_voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame = 0;
    m_voxelInterestAnchorCameraCandidateAttemptsLastFrame = 0;
    m_voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame = 0;
    m_voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame = 0;
    m_voxelInterestEmittedLastFrame = 0;
    m_voxelInterestReusedLastFrame = 0;
    m_voxelInterestReuseAgeLastFrame = 0;
    m_backlogVoxelEnqueuedLastFrame = 0;
    m_backlogVoxelCarriedLastFrame = 0;
    m_backlogVoxelResidentSkipLastFrame = 0;
    m_visiblePriorityTaggedLastFrame = 0;
    m_visiblePriorityPrioritizedLastFrame = 0;
    m_visiblePriorityVoxelSet.clear();
    m_asyncNoncriticalGenerationEnqueuedLastFrame = 0;
    m_asyncNoncriticalGenerationCompletedLastFrame = 0;
    m_asyncNoncriticalGenerationAppliedLastFrame = 0;
    m_asyncNoncriticalGenerationDiscardedLastFrame = 0;
    m_asyncNoncriticalGenerationDuplicateSyncLastFrame = 0;
    m_asyncVisibleCriticalGenerationEnqueuedLastFrame = 0;
    m_asyncVisibleCriticalGenerationCompletedLastFrame = 0;
    m_asyncVisibleCriticalGenerationAppliedLastFrame = 0;
    m_asyncVisibleCriticalGenerationDiscardedLastFrame = 0;
    m_asyncVisibleCriticalGenerationDuplicateSyncLastFrame = 0;
    m_asyncVisibleReservationAppliedLastFrame = 0;
    m_asyncVisibleReservationApplyDeferredLastFrame = 0;
    m_asyncVisibleReservationApplyLimitLastFrame = UINT32_MAX;
    m_asyncNoncriticalGenerationWorkerMsLastFrame = 0.0f;
    m_asyncNoncriticalGenerationApplyMsLastFrame = 0.0f;
    if (!policy.IsEnabled() || m_tiles.empty()) {
        m_interestSet.clear();
        m_voxelInterestSet.clear();
        m_voxelGenerationQueue.clear();
        m_queuedVoxelSet.clear();
        m_voxelBacklogFirstFrame.clear();
        m_visiblePriorityVoxelSet.clear();
        m_asyncVisibleReservations.clear();
        m_lastInterestSignatureValid = false;
        m_lastVoxelInterestSignatureValid = false;
        m_lastInterestUpdateFrame = frameIndex;
        m_interestReusedLastFrame = 0;
        RefreshStats();
        m_stats.heightInterestAnchors = 0;
        m_stats.voxelInterestAnchors = 0;
        return;
    }

    cameraX = FiniteOr(cameraX, 0.0f);
    cameraY = FiniteOr(cameraY, 0.0f);
    cameraZ = FiniteOr(cameraZ, 0.0f);
    m_lastCameraYForStats = cameraY;
    forwardX = FiniteOr(forwardX, 0.0f);
    forwardY = FiniteOr(forwardY, 0.0f);
    forwardZ = FiniteOr(forwardZ, 0.0f);
    velocityX = FiniteOr(velocityX, 0.0f);
    velocityY = FiniteOr(velocityY, 0.0f);
    velocityZ = FiniteOr(velocityZ, 0.0f);
    predictionSeconds = std::max(0.0f, FiniteOr(predictionSeconds, 0.0f));

    const InterestSignature signature = BuildInterestSignature(
        cameraX,
        cameraY,
        cameraZ,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        velocityX,
        velocityY,
        velocityZ,
        predictionSeconds);
    const bool intervalReuse =
        m_lastInterestSignatureValid &&
        policy.Config().interestUpdateIntervalFrames > 1u &&
        frameIndex - m_lastInterestUpdateFrame < policy.Config().interestUpdateIntervalFrames;
    if (m_lastInterestSignatureValid && (signature == m_lastInterestSignature || intervalReuse)) {
        m_interestReusedLastFrame = 1;
        RefreshInterestTouchFrames(frameIndex);
        RefreshStats();
        return;
    }

    m_interestReusedLastFrame = 0;
    std::unordered_set<SparseClipmapTileCoord, SparseClipmapTileCoordHash> previousInterestSet;
    if (policy.Config().drainReuseDiagnostics) {
        previousInterestSet = m_interestSet;
    }
    m_interestSet.clear();
    m_generationQueue.clear();
    m_queuedSet.clear();
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
    const float predictedX = FiniteOr(cameraX + velocityX * predictionSeconds, cameraX);
    const float predictedY = FiniteOr(cameraY + velocityY * predictionSeconds, cameraY);
    const float predictedZ = FiniteOr(cameraZ + velocityZ * predictionSeconds, cameraZ);
    const float velocityLenXz = std::sqrt(velocityX * velocityX + velocityZ * velocityZ);
    const bool useMotionLookahead =
        predictionSeconds > 0.0f &&
        velocityLenXz >= policy.Config().motionLookaheadMinSpeed;
    uint32_t heightAnchorCount = 0;
    if (policy.Config().heightClipmapEnabled) {
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
            const int32_t centerX = FloorToGridCoordClamped(anchor.x, tileWorldSize, radius + 2);
            const int32_t centerZ = FloorToGridCoordClamped(anchor.z, tileWorldSize, radius + 2);
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
        // The screen can expose valley walls well outside the center ray. Queue
        // a bounded horizontal view fan so mid-distance height/column ownership
        // covers the visible silhouette without falling back to expensive
        // per-pixel far terrain searches.
        if (forwardLenXz > 0.001f) {
            const float rightX = forwardNormZ;
            const float rightZ = -forwardNormX;
            const float fanDistance =
                tileWorldSize * std::max(1.0f, static_cast<float>(std::max(2, radius)));
            constexpr float kViewFanHalfWidth = 0.72f;
            for (int32_t fan = -2; fan <= 2; ++fan) {
                if (fan == 0) {
                    continue;
                }
                const float fanScale = static_cast<float>(fan) * 0.5f;
                const float fanEndX =
                    cameraX + forwardNormX * fanDistance + rightX * fanDistance * kViewFanHalfWidth * fanScale;
                const float fanEndZ =
                    cameraZ + forwardNormZ * fanDistance + rightZ * fanDistance * kViewFanHalfWidth * fanScale;
                for (const SparseClipmapTileCoord& coord : BuildTileLine2D(
                         static_cast<int32_t>(ring),
                         cameraX,
                         cameraZ,
                         fanEndX,
                         fanEndZ,
                         tileWorldSize,
                         maxLineCoords)) {
                    queueHeightCoord(coord);
                }
            }
        }

        const std::vector<InterestAnchor> shellAnchors = anchors;
        for (const InterestAnchor& anchor : shellAnchors) {
            const int32_t anchorRadius = std::max(1, radius + anchor.radiusBias);
            const int32_t centerX = FloorToGridCoordClamped(anchor.x, tileWorldSize, anchorRadius + 2);
            const int32_t centerZ = FloorToGridCoordClamped(anchor.z, tileWorldSize, anchorRadius + 2);
            ++heightAnchorCount;
            for (int32_t dz = -anchorRadius; dz <= anchorRadius; ++dz) {
                for (int32_t dx = -anchorRadius; dx <= anchorRadius; ++dx) {
                    queueHeightCoord(SparseClipmapTileCoord{
                        static_cast<int32_t>(ring),
                        SaturatingAddInt32(centerX, dx),
                        SaturatingAddInt32(centerZ, dz)
                    });
                }
            }
        }
    }
    }

    if (policy.Config().drainReuseDiagnostics) {
        for (const SparseClipmapTileCoord& coord : m_interestSet) {
            const bool wasPreviouslyInterested = previousInterestSet.erase(coord) != 0u;
            const bool resident = m_slotByCoord.find(coord) != m_slotByCoord.end();
            if (!wasPreviouslyInterested) {
                ++m_newlyInterestedTilesLastFrame;
            } else if (resident) {
                ++m_reusedInterestedTilesLastFrame;
            }
            if (resident) {
                ++m_residentInterestedTilesLastFrame;
            }
        }
        m_noLongerInterestedTilesLastFrame += static_cast<uint32_t>(std::min<size_t>(
            previousInterestSet.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max() - m_noLongerInterestedTilesLastFrame)));
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
    m_lastInterestSignature = signature;
    m_lastInterestSignatureValid = true;
    m_lastInterestUpdateFrame = frameIndex;
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
    return PumpGeneration(maxTiles, maxTiles, frameIndex, policy);
}

uint32_t SparseClipmapTileCache::PumpGeneration(
    uint32_t maxHeightTiles,
    uint32_t maxVoxelBricks,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy)
{
    m_lastStatsFrame = frameIndex;
    m_pumpBudgetHitLastFrame = 0;
    m_backlogVoxelPumpedLastFrame = 0;
    m_backlogVoxelResidentSkipLastFrame = 0;
    m_generatedVoxelTimingCountLastFrame = 0;
    m_generatedVoxelMsAccumLastFrame = 0.0f;
    m_generatedVoxelMaxMsLastFrame = 0.0f;
    m_generatedVoxelBricksByRingLastFrame.fill(0u);
    m_sharedVoxelColumnHeightHitsLastFrame = 0;
    m_sharedVoxelColumnHeightMissesLastFrame = 0;
    m_sharedVoxelColumnReliefHitsLastFrame = 0;
    m_sharedVoxelColumnReliefMissesLastFrame = 0;
    m_parallelWorkerColumnCacheEntriesLastFrame = 0;
    m_parallelWorkerColumnHeightHitsLastFrame = 0;
    m_parallelWorkerColumnHeightMissesLastFrame = 0;
    m_parallelWorkerColumnReliefHitsLastFrame = 0;
    m_parallelWorkerColumnReliefMissesLastFrame = 0;
    m_parallelVoxelPumpBricksLastFrame = 0;
    m_parallelVoxelPumpWorkersLastFrame = 0;
    m_parallelVoxelPumpWallMsLastFrame = 0.0f;
    if (policy.Config().sharedVoxelColumnCache) {
        m_sharedVoxelColumnCache.clear();
        m_sharedVoxelColumnCache.reserve(std::max<size_t>(
            m_sharedVoxelColumnCache.bucket_count(),
            static_cast<size_t>(SPARSE_BRICK_SIZE) *
                static_cast<size_t>(SPARSE_BRICK_SIZE) *
                256u));
    } else if (!m_sharedVoxelColumnCache.empty()) {
        m_sharedVoxelColumnCache.clear();
    }
    m_effectivePumpBudgetMsLastFrame =
        policy.Config().backlogAwarePump ? policy.Config().pumpBudgetMs : 0.0f;
    // Phase 1 hard pump time budget: always enforced, cannot be bypassed by the
    // coverage-emergency path that zeroes pumpBudgetMs.
    const float voxelPumpHardBudgetMs = policy.Config().voxelPumpHardBudgetMs;
    const bool voxelPumpHardBudgetActive = voxelPumpHardBudgetMs > 0.0f;
    if (!policy.IsEnabled() || (maxHeightTiles == 0 && maxVoxelBricks == 0)) {
        RefreshStats();
        m_stats.pumpHeightMsLastFrame = 0.0f;
        m_stats.pumpVoxelMsLastFrame = 0.0f;
        return 0;
    }

    const auto heightPumpStart = std::chrono::steady_clock::now();
    uint32_t generated = 0;
    uint32_t evicted = 0;
    while (policy.Config().heightClipmapEnabled &&
           !m_generationQueue.empty() &&
           generated < maxHeightTiles) {
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
    const auto heightPumpEnd = std::chrono::steady_clock::now();

    const auto voxelPumpStart = std::chrono::steady_clock::now();
    uint32_t generatedVoxel = 0;
    uint32_t evictedVoxel = 0;
    const bool voxelPumpBudgetActive =
        policy.Config().backlogAwarePump &&
        policy.Config().pumpBudgetMs > 0.0f;
    const bool editOverlaysActive = m_edits && m_edits->EditedBrickCount() != 0u;
    const auto asyncGenerationPending = [this](const SparseVoxelClipmapCoord& coord) {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        return m_asyncNoncriticalGenerationPending.find(coord) !=
            m_asyncNoncriticalGenerationPending.end();
    };
    const bool parallelVoxelPumpAllowed =
        policy.Config().parallelVoxelPump &&
        !policy.Config().sharedVoxelColumnCache &&
        !editOverlaysActive &&
        maxVoxelBricks >= policy.Config().parallelVoxelPumpMinBricks;
    if (parallelVoxelPumpAllowed && !m_voxelGenerationQueue.empty()) {
        struct PendingVoxelGeneration {
            SparseVoxelClipmapCoord coord;
            uint32_t slot = UINT32_MAX;
            bool evicted = false;
        };

        const uint32_t parallelVoxelPumpLimit = voxelPumpBudgetActive
            ? std::min(maxVoxelBricks, policy.Config().parallelVoxelPumpMinBricks)
            : maxVoxelBricks;
        std::vector<PendingVoxelGeneration> pending;
        pending.reserve(parallelVoxelPumpLimit);
        while (!m_voxelGenerationQueue.empty() &&
               pending.size() < static_cast<size_t>(parallelVoxelPumpLimit)) {
            const SparseVoxelClipmapCoord coord = m_voxelGenerationQueue.front();
            m_voxelGenerationQueue.pop_front();
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);

            if (!m_voxelInterestSet.empty() && m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
                continue;
            }
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                ++m_backlogVoxelResidentSkipLastFrame;
                continue;
            }

            const bool wasFull =
                m_voxelSlotByCoord.size() >= m_voxelBricks.size() && m_freeVoxelSlots.empty();
            const uint32_t slot = AllocateVoxelSlot(coord, frameIndex);
            if (slot == UINT32_MAX) {
                break;
            }
            m_voxelBricks[slot].coord = coord;
            m_voxelBricks[slot].slot = slot;
            m_voxelBricks[slot].lastTouchedFrame = frameIndex;
            pending.push_back({coord, slot, wasFull});
        }

        if (!pending.empty()) {
            std::vector<float> elapsedMs(pending.size(), 0.0f);
            const bool useWorkerThreads =
                pending.size() >= static_cast<size_t>(policy.Config().parallelVoxelPumpMinBricks) &&
                policy.Config().parallelVoxelPumpMaxWorkers > 1u;
            const uint32_t workerCount = useWorkerThreads
                ? std::min<uint32_t>(
                    static_cast<uint32_t>(pending.size()),
                    policy.Config().parallelVoxelPumpMaxWorkers)
                : 1u;
            const bool useWorkerColumnCache =
                policy.Config().parallelWorkerColumnCache &&
                !policy.Config().sharedVoxelColumnCache &&
                !policy.Config().directVoxelFootprintColumns;
            const auto parallelStart = std::chrono::steady_clock::now();
            if (workerCount <= 1u) {
                std::unordered_map<uint64_t, VoxelColumnSample> workerColumnCache;
                VoxelColumnCacheCounters workerColumnCounters;
                std::unordered_map<uint64_t, VoxelColumnSample>* workerColumnCachePtr =
                    useWorkerColumnCache ? &workerColumnCache : nullptr;
                VoxelColumnCacheCounters* workerColumnCountersPtr =
                    useWorkerColumnCache ? &workerColumnCounters : nullptr;
                for (size_t index = 0; index < pending.size(); ++index) {
                    const auto generateStart = std::chrono::steady_clock::now();
                    GenerateVoxelBrick(
                        pending[index].slot,
                        policy,
                        workerColumnCachePtr,
                        workerColumnCountersPtr);
                    elapsedMs[index] = ElapsedMs(generateStart, std::chrono::steady_clock::now());
                }
                if (useWorkerColumnCache) {
                    m_parallelWorkerColumnCacheEntriesLastFrame += static_cast<uint32_t>(
                        std::min<size_t>(workerColumnCache.size(), std::numeric_limits<uint32_t>::max()));
                    m_parallelWorkerColumnHeightHitsLastFrame += workerColumnCounters.heightHits;
                    m_parallelWorkerColumnHeightMissesLastFrame += workerColumnCounters.heightMisses;
                    m_parallelWorkerColumnReliefHitsLastFrame += workerColumnCounters.reliefHits;
                    m_parallelWorkerColumnReliefMissesLastFrame += workerColumnCounters.reliefMisses;
                }
            } else {
                std::vector<VoxelColumnCacheCounters> workerColumnCounters(workerCount);
                std::vector<uint32_t> workerColumnEntries(workerCount, 0u);
                std::vector<uint32_t> pendingSlots;
                bool generatedWithPersistentWorkers = false;
                if (policy.Config().parallelVoxelPumpPersistentWorkers) {
                    pendingSlots.reserve(pending.size());
                    for (const PendingVoxelGeneration& item : pending) {
                        pendingSlots.push_back(item.slot);
                    }
                    generatedWithPersistentWorkers =
                        GenerateVoxelBricksWithPersistentWorkers(
                            pendingSlots,
                            policy,
                            useWorkerColumnCache,
                            elapsedMs,
                            workerColumnCounters,
                            workerColumnEntries,
                            workerCount);
                }
                if (!generatedWithPersistentWorkers) {
                    std::vector<std::thread> workers;
                    workers.reserve(workerCount);
                    for (uint32_t worker = 0u; worker < workerCount; ++worker) {
                        workers.emplace_back([this,
                                              &pending,
                                              &elapsedMs,
                                              &policy,
                                              worker,
                                              workerCount,
                                              useWorkerColumnCache,
                                              &workerColumnCounters,
                                              &workerColumnEntries]() {
                            std::unordered_map<uint64_t, VoxelColumnSample> workerColumnCache;
                            std::unordered_map<uint64_t, VoxelColumnSample>* workerColumnCachePtr =
                                useWorkerColumnCache ? &workerColumnCache : nullptr;
                            VoxelColumnCacheCounters* workerColumnCountersPtr =
                                useWorkerColumnCache ? &workerColumnCounters[worker] : nullptr;
                            const size_t begin =
                                (pending.size() * static_cast<size_t>(worker)) /
                                static_cast<size_t>(workerCount);
                            const size_t end =
                                (pending.size() * static_cast<size_t>(worker + 1u)) /
                                static_cast<size_t>(workerCount);
                            for (size_t index = begin; index < end; ++index) {
                                const auto generateStart = std::chrono::steady_clock::now();
                                GenerateVoxelBrick(
                                    pending[index].slot,
                                    policy,
                                    workerColumnCachePtr,
                                    workerColumnCountersPtr);
                                elapsedMs[index] = ElapsedMs(generateStart, std::chrono::steady_clock::now());
                            }
                            if (useWorkerColumnCache) {
                                workerColumnEntries[worker] = static_cast<uint32_t>(
                                    std::min<size_t>(
                                        workerColumnCache.size(),
                                        std::numeric_limits<uint32_t>::max()));
                            }
                        });
                    }
                    for (std::thread& worker : workers) {
                        worker.join();
                    }
                }
                if (useWorkerColumnCache) {
                    for (uint32_t worker = 0u; worker < workerCount; ++worker) {
                        m_parallelWorkerColumnCacheEntriesLastFrame += workerColumnEntries[worker];
                        m_parallelWorkerColumnHeightHitsLastFrame += workerColumnCounters[worker].heightHits;
                        m_parallelWorkerColumnHeightMissesLastFrame += workerColumnCounters[worker].heightMisses;
                        m_parallelWorkerColumnReliefHitsLastFrame += workerColumnCounters[worker].reliefHits;
                        m_parallelWorkerColumnReliefMissesLastFrame += workerColumnCounters[worker].reliefMisses;
                    }
                }
            }
            m_parallelVoxelPumpBricksLastFrame =
                static_cast<uint32_t>(
                    std::min<size_t>(pending.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            m_parallelVoxelPumpWorkersLastFrame = workerCount;
            m_parallelVoxelPumpWallMsLastFrame =
                ElapsedMs(parallelStart, std::chrono::steady_clock::now());
            if (voxelPumpBudgetActive &&
                !m_voxelGenerationQueue.empty() &&
                m_parallelVoxelPumpWallMsLastFrame >= policy.Config().pumpBudgetMs) {
                m_pumpBudgetHitLastFrame = 1;
            }

            for (size_t index = 0; index < pending.size(); ++index) {
                const PendingVoxelGeneration& item = pending[index];
                evictedVoxel += item.evicted ? 1u : 0u;
                RecordVoxelGenerationTiming(item.coord, elapsedMs[index]);
                m_voxelSlotByCoord[item.coord] = item.slot;
                ++generatedVoxel;
                ++m_backlogVoxelPumpedLastFrame;
                ++m_dirtySerial;
                ++m_voxelDirtySerial;
                MarkVoxelSlotDirty(item.slot);
            }
        }
    }
    while (!m_voxelGenerationQueue.empty() && generatedVoxel < maxVoxelBricks) {
        const SparseVoxelClipmapCoord coord = m_voxelGenerationQueue.front();
        m_voxelGenerationQueue.pop_front();
        m_queuedVoxelSet.erase(coord);
        m_voxelBacklogFirstFrame.erase(coord);

        if (!m_voxelInterestSet.empty() && m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            ++m_backlogVoxelResidentSkipLastFrame;
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
        const auto generateStart = std::chrono::steady_clock::now();
        GenerateVoxelBrick(slot, policy);
        RecordVoxelGenerationTiming(
            coord,
            ElapsedMs(generateStart, std::chrono::steady_clock::now()));
        m_voxelSlotByCoord[coord] = slot;
        ++generatedVoxel;
        ++m_backlogVoxelPumpedLastFrame;
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        MarkVoxelSlotDirty(slot);
        if (voxelPumpHardBudgetActive && !m_voxelGenerationQueue.empty()) {
            const float elapsedMs =
                ElapsedMs(voxelPumpStart, std::chrono::steady_clock::now());
            if (elapsedMs >= voxelPumpHardBudgetMs) {
                m_pumpBudgetHitLastFrame = 1;
                break;
            }
        }
        if (voxelPumpBudgetActive &&
            generatedVoxel < maxVoxelBricks &&
            !m_voxelGenerationQueue.empty()) {
            const float elapsedMs =
                ElapsedMs(voxelPumpStart, std::chrono::steady_clock::now());
            if (elapsedMs >= policy.Config().pumpBudgetMs) {
                m_pumpBudgetHitLastFrame = 1;
                break;
            }
        }
    }

    const auto voxelPumpEnd = std::chrono::steady_clock::now();

    RefreshStats(generated, evicted, generatedVoxel, evictedVoxel);
    m_stats.pumpHeightMsLastFrame = ElapsedMs(heightPumpStart, heightPumpEnd);
    m_stats.pumpVoxelMsLastFrame = ElapsedMs(voxelPumpStart, voxelPumpEnd);
    return generated + generatedVoxel;
}

uint32_t SparseClipmapTileCache::PumpGenerationSplitVisiblePriority(
    uint32_t maxHeightTiles,
    uint32_t maxVisibleVoxelBricks,
    uint32_t maxCacheVoxelBricks,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy)
{
    const uint32_t generatedHeight =
        PumpGeneration(maxHeightTiles, 0u, frameIndex, policy);
    const uint32_t evictedHeight = m_stats.evictedTilesLastFrame;
    const float heightPumpMs = m_stats.pumpHeightMsLastFrame;

    const bool asyncQueueAllowed =
        (policy.Config().asyncNoncriticalGeneration ||
         policy.Config().asyncVisibleCriticalGeneration) &&
        policy.Config().asyncNoncriticalGenerationQueueMax != 0u;
    if (!policy.IsEnabled() ||
        (maxVisibleVoxelBricks == 0u && maxCacheVoxelBricks == 0u && !asyncQueueAllowed) ||
        m_voxelGenerationQueue.empty()) {
        m_stats.pumpHeightMsLastFrame = heightPumpMs;
        m_stats.pumpVoxelMsLastFrame = 0.0f;
        return generatedHeight;
    }

    const auto voxelPumpStart = std::chrono::steady_clock::now();
    uint32_t evictedVoxel = 0u;
    const uint32_t generatedVisible =
        PumpVoxelGenerationMatchingPriority(
            true,
            maxVisibleVoxelBricks,
            frameIndex,
            policy,
            evictedVoxel);
    const uint32_t generatedCache =
        PumpVoxelGenerationMatchingPriority(
            false,
            maxCacheVoxelBricks,
            frameIndex,
            policy,
            evictedVoxel);
    const auto voxelPumpEnd = std::chrono::steady_clock::now();

    const uint32_t generatedVoxel = generatedVisible + generatedCache;
    RefreshStats(generatedHeight, evictedHeight, generatedVoxel, evictedVoxel);
    m_stats.pumpHeightMsLastFrame = heightPumpMs;
    m_stats.pumpVoxelMsLastFrame = ElapsedMs(voxelPumpStart, voxelPumpEnd);
    return generatedHeight + generatedVoxel;
}

uint32_t SparseClipmapTileCache::PumpVoxelGenerationMatchingPriority(
    bool requireVisiblePriority,
    uint32_t maxVoxelBricks,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t& evictedVoxel)
{
    if (!policy.IsEnabled() || m_voxelGenerationQueue.empty()) {
        return 0u;
    }
    const bool asyncQueueAllowed =
        (requireVisiblePriority
             ? policy.Config().asyncVisibleCriticalGeneration
             : policy.Config().asyncNoncriticalGeneration) &&
        policy.Config().asyncNoncriticalGenerationQueueMax != 0u;
    if (maxVoxelBricks == 0u && !asyncQueueAllowed) {
        return 0u;
    }

    struct PendingVoxelGeneration {
        SparseVoxelClipmapCoord coord;
        uint32_t slot = UINT32_MAX;
        bool evicted = false;
    };

    const bool editOverlaysActive = m_edits && m_edits->EditedBrickCount() != 0u;
    if (asyncQueueAllowed && !requireVisiblePriority && !editOverlaysActive) {
        QueueAsyncVoxelGenerationMatchingPriority(false, frameIndex, policy);
        if (maxVoxelBricks == 0u) {
            return 0u;
        }
    } else if (asyncQueueAllowed && requireVisiblePriority && maxVoxelBricks == 0u) {
        QueueAsyncVoxelGenerationMatchingPriority(true, frameIndex, policy);
        return 0u;
    }

    const auto asyncGenerationPending = [this](const SparseVoxelClipmapCoord& coord) {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        return m_asyncNoncriticalGenerationPending.find(coord) !=
            m_asyncNoncriticalGenerationPending.end();
    };
    const bool parallelVoxelPumpAllowed =
        policy.Config().parallelVoxelPump &&
        !policy.Config().sharedVoxelColumnCache &&
        !editOverlaysActive &&
        maxVoxelBricks >= policy.Config().parallelVoxelPumpMinBricks;
    if (parallelVoxelPumpAllowed) {
        std::vector<PendingVoxelGeneration> pending;
        pending.reserve(maxVoxelBricks);
        for (auto it = m_voxelGenerationQueue.begin();
             it != m_voxelGenerationQueue.end() &&
                 pending.size() < static_cast<size_t>(maxVoxelBricks);) {
            const SparseVoxelClipmapCoord coord = *it;
            const bool visiblePriority =
                m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end() ||
                m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
            if (visiblePriority != requireVisiblePriority) {
                ++it;
                continue;
            }
            if (!requireVisiblePriority && asyncGenerationPending(coord)) {
                ++it;
                continue;
            }

            const bool visibleReservation =
                m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
            if (!visibleReservation &&
                !m_voxelInterestSet.empty() &&
                m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
                it = m_voxelGenerationQueue.erase(it);
                m_queuedVoxelSet.erase(coord);
                m_voxelBacklogFirstFrame.erase(coord);
                m_visiblePriorityVoxelSet.erase(coord);
                continue;
            }
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                it = m_voxelGenerationQueue.erase(it);
                m_queuedVoxelSet.erase(coord);
                m_voxelBacklogFirstFrame.erase(coord);
                m_visiblePriorityVoxelSet.erase(coord);
                ++m_backlogVoxelResidentSkipLastFrame;
                continue;
            }

            const bool wasFull = m_voxelSlotByCoord.size() >= m_voxelBricks.size() && m_freeVoxelSlots.empty();
            const uint32_t slot = AllocateVoxelSlot(coord, frameIndex);
            if (slot == UINT32_MAX) {
                break;
            }

            it = m_voxelGenerationQueue.erase(it);
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);
            m_voxelBricks[slot].coord = coord;
            m_voxelBricks[slot].slot = slot;
            m_voxelBricks[slot].lastTouchedFrame = frameIndex;
            pending.push_back({coord, slot, wasFull});
        }

        if (!pending.empty()) {
            std::vector<float> elapsedMs(pending.size(), 0.0f);
            const bool useWorkerThreads =
                pending.size() >= static_cast<size_t>(policy.Config().parallelVoxelPumpMinBricks) &&
                policy.Config().parallelVoxelPumpMaxWorkers > 1u;
            const uint32_t workerCount = useWorkerThreads
                ? std::min<uint32_t>(
                    static_cast<uint32_t>(pending.size()),
                    policy.Config().parallelVoxelPumpMaxWorkers)
                : 1u;
            const bool useWorkerColumnCache =
                policy.Config().parallelWorkerColumnCache &&
                !policy.Config().sharedVoxelColumnCache &&
                !policy.Config().directVoxelFootprintColumns;
            const auto parallelStart = std::chrono::steady_clock::now();
            if (workerCount <= 1u) {
                std::unordered_map<uint64_t, VoxelColumnSample> workerColumnCache;
                VoxelColumnCacheCounters workerColumnCounters;
                std::unordered_map<uint64_t, VoxelColumnSample>* workerColumnCachePtr =
                    useWorkerColumnCache ? &workerColumnCache : nullptr;
                VoxelColumnCacheCounters* workerColumnCountersPtr =
                    useWorkerColumnCache ? &workerColumnCounters : nullptr;
                for (size_t index = 0; index < pending.size(); ++index) {
                    const auto generateStart = std::chrono::steady_clock::now();
                    GenerateVoxelBrick(
                        pending[index].slot,
                        policy,
                        workerColumnCachePtr,
                        workerColumnCountersPtr);
                    elapsedMs[index] = ElapsedMs(generateStart, std::chrono::steady_clock::now());
                }
                if (useWorkerColumnCache) {
                    m_parallelWorkerColumnCacheEntriesLastFrame += static_cast<uint32_t>(
                        std::min<size_t>(workerColumnCache.size(), std::numeric_limits<uint32_t>::max()));
                    m_parallelWorkerColumnHeightHitsLastFrame += workerColumnCounters.heightHits;
                    m_parallelWorkerColumnHeightMissesLastFrame += workerColumnCounters.heightMisses;
                    m_parallelWorkerColumnReliefHitsLastFrame += workerColumnCounters.reliefHits;
                    m_parallelWorkerColumnReliefMissesLastFrame += workerColumnCounters.reliefMisses;
                }
            } else {
                std::vector<VoxelColumnCacheCounters> workerColumnCounters(workerCount);
                std::vector<uint32_t> workerColumnEntries(workerCount, 0u);
                std::vector<uint32_t> pendingSlots;
                bool generatedWithPersistentWorkers = false;
                if (policy.Config().parallelVoxelPumpPersistentWorkers) {
                    pendingSlots.reserve(pending.size());
                    for (const PendingVoxelGeneration& item : pending) {
                        pendingSlots.push_back(item.slot);
                    }
                    generatedWithPersistentWorkers =
                        GenerateVoxelBricksWithPersistentWorkers(
                            pendingSlots,
                            policy,
                            useWorkerColumnCache,
                            elapsedMs,
                            workerColumnCounters,
                            workerColumnEntries,
                            workerCount);
                }
                if (!generatedWithPersistentWorkers) {
                    std::vector<std::thread> workers;
                    workers.reserve(workerCount);
                    for (uint32_t worker = 0u; worker < workerCount; ++worker) {
                        workers.emplace_back([this,
                                              &pending,
                                              &elapsedMs,
                                              &policy,
                                              worker,
                                              workerCount,
                                              useWorkerColumnCache,
                                              &workerColumnCounters,
                                              &workerColumnEntries]() {
                            std::unordered_map<uint64_t, VoxelColumnSample> workerColumnCache;
                            std::unordered_map<uint64_t, VoxelColumnSample>* workerColumnCachePtr =
                                useWorkerColumnCache ? &workerColumnCache : nullptr;
                            VoxelColumnCacheCounters* workerColumnCountersPtr =
                                useWorkerColumnCache ? &workerColumnCounters[worker] : nullptr;
                            const size_t begin =
                                (pending.size() * static_cast<size_t>(worker)) /
                                static_cast<size_t>(workerCount);
                            const size_t end =
                                (pending.size() * static_cast<size_t>(worker + 1u)) /
                                static_cast<size_t>(workerCount);
                            for (size_t index = begin; index < end; ++index) {
                                const auto generateStart = std::chrono::steady_clock::now();
                                GenerateVoxelBrick(
                                    pending[index].slot,
                                    policy,
                                    workerColumnCachePtr,
                                    workerColumnCountersPtr);
                                elapsedMs[index] = ElapsedMs(generateStart, std::chrono::steady_clock::now());
                            }
                            if (useWorkerColumnCache) {
                                workerColumnEntries[worker] = static_cast<uint32_t>(
                                    std::min<size_t>(
                                        workerColumnCache.size(),
                                        std::numeric_limits<uint32_t>::max()));
                            }
                        });
                    }
                    for (std::thread& worker : workers) {
                        worker.join();
                    }
                }
                if (useWorkerColumnCache) {
                    for (uint32_t worker = 0u; worker < workerCount; ++worker) {
                        m_parallelWorkerColumnCacheEntriesLastFrame += workerColumnEntries[worker];
                        m_parallelWorkerColumnHeightHitsLastFrame += workerColumnCounters[worker].heightHits;
                        m_parallelWorkerColumnHeightMissesLastFrame += workerColumnCounters[worker].heightMisses;
                        m_parallelWorkerColumnReliefHitsLastFrame += workerColumnCounters[worker].reliefHits;
                        m_parallelWorkerColumnReliefMissesLastFrame += workerColumnCounters[worker].reliefMisses;
                    }
                }
            }

            m_parallelVoxelPumpBricksLastFrame +=
                static_cast<uint32_t>(
                    std::min<size_t>(pending.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            m_parallelVoxelPumpWorkersLastFrame = std::max(m_parallelVoxelPumpWorkersLastFrame, workerCount);
            m_parallelVoxelPumpWallMsLastFrame +=
                ElapsedMs(parallelStart, std::chrono::steady_clock::now());

            uint32_t generatedVoxel = 0u;
            for (size_t index = 0; index < pending.size(); ++index) {
                const PendingVoxelGeneration& item = pending[index];
                evictedVoxel += item.evicted ? 1u : 0u;
                RecordVoxelGenerationTiming(item.coord, elapsedMs[index]);
                m_voxelSlotByCoord[item.coord] = item.slot;
                ++generatedVoxel;
                ++m_backlogVoxelPumpedLastFrame;
                ++m_dirtySerial;
                ++m_voxelDirtySerial;
                MarkVoxelSlotDirty(item.slot);
            }
            if (requireVisiblePriority) {
                QueueAsyncVoxelGenerationMatchingPriority(true, frameIndex, policy);
            }
            return generatedVoxel;
        }
    }

    uint32_t generatedVoxel = 0u;
    for (auto it = m_voxelGenerationQueue.begin();
         it != m_voxelGenerationQueue.end() && generatedVoxel < maxVoxelBricks;) {
        const SparseVoxelClipmapCoord coord = *it;
        const bool visiblePriority =
            m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end() ||
            m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
        if (visiblePriority != requireVisiblePriority) {
            ++it;
            continue;
        }
        if (!requireVisiblePriority && asyncGenerationPending(coord)) {
            ++it;
            continue;
        }

        const bool visibleReservation =
            m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
        if (!visibleReservation &&
            !m_voxelInterestSet.empty() &&
            m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            it = m_voxelGenerationQueue.erase(it);
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);
            m_visiblePriorityVoxelSet.erase(coord);
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            it = m_voxelGenerationQueue.erase(it);
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);
            m_visiblePriorityVoxelSet.erase(coord);
            ++m_backlogVoxelResidentSkipLastFrame;
            continue;
        }

        const bool wasFull = m_voxelSlotByCoord.size() >= m_voxelBricks.size() && m_freeVoxelSlots.empty();
        const uint32_t slot = AllocateVoxelSlot(coord, frameIndex);
        if (slot == UINT32_MAX) {
            break;
        }

        it = m_voxelGenerationQueue.erase(it);
        m_queuedVoxelSet.erase(coord);
        m_voxelBacklogFirstFrame.erase(coord);
        evictedVoxel += wasFull ? 1u : 0u;
        m_voxelBricks[slot].coord = coord;
        m_voxelBricks[slot].slot = slot;
        m_voxelBricks[slot].lastTouchedFrame = frameIndex;
        const auto generateStart = std::chrono::steady_clock::now();
        GenerateVoxelBrick(slot, policy);
        RecordVoxelGenerationTiming(
            coord,
            ElapsedMs(generateStart, std::chrono::steady_clock::now()));
        m_voxelSlotByCoord[coord] = slot;
        ++generatedVoxel;
        ++m_backlogVoxelPumpedLastFrame;
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        MarkVoxelSlotDirty(slot);
    }

    if (requireVisiblePriority) {
        QueueAsyncVoxelGenerationMatchingPriority(true, frameIndex, policy);
    }
    return generatedVoxel;
}

uint32_t SparseClipmapTileCache::PumpVoxelGenerationForRing(
    uint32_t ring,
    uint32_t maxVoxelBricks,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t minEvictRing)
{
    m_lastStatsFrame = frameIndex;
    if (!policy.IsEnabled() || maxVoxelBricks == 0 || m_voxelGenerationQueue.empty()) {
        return 0;
    }
    if (minEvictRing == UINT32_MAX) {
        minEvictRing = ring;
    }

    const auto voxelPumpStart = std::chrono::steady_clock::now();
    uint32_t generatedVoxel = 0;
    uint32_t evictedVoxel = 0;
    for (auto it = m_voxelGenerationQueue.begin();
         it != m_voxelGenerationQueue.end() && generatedVoxel < maxVoxelBricks;) {
        const SparseVoxelClipmapCoord coord = *it;
        if (coord.ring < 0 || static_cast<uint32_t>(coord.ring) != ring) {
            ++it;
            continue;
        }

        if (!m_voxelInterestSet.empty() && m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            it = m_voxelGenerationQueue.erase(it);
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            it = m_voxelGenerationQueue.erase(it);
            m_queuedVoxelSet.erase(coord);
            m_voxelBacklogFirstFrame.erase(coord);
            ++m_backlogVoxelResidentSkipLastFrame;
            continue;
        }

        const bool wasFull = m_voxelSlotByCoord.size() >= m_voxelBricks.size() && m_freeVoxelSlots.empty();
        const uint32_t slot = AllocateVoxelSlotForMinRing(coord, frameIndex, minEvictRing);
        if (slot == UINT32_MAX) {
            break;
        }
        it = m_voxelGenerationQueue.erase(it);
        m_queuedVoxelSet.erase(coord);
        m_voxelBacklogFirstFrame.erase(coord);
        evictedVoxel += wasFull ? 1u : 0u;
        m_voxelBricks[slot].coord = coord;
        m_voxelBricks[slot].slot = slot;
        m_voxelBricks[slot].lastTouchedFrame = frameIndex;
        const auto generateStart = std::chrono::steady_clock::now();
        GenerateVoxelBrick(slot, policy);
        RecordVoxelGenerationTiming(
            coord,
            ElapsedMs(generateStart, std::chrono::steady_clock::now()));
        m_voxelSlotByCoord[coord] = slot;
        ++generatedVoxel;
        ++m_backlogVoxelPumpedLastFrame;
        ++m_dirtySerial;
        ++m_voxelDirtySerial;
        MarkVoxelSlotDirty(slot);
    }
    const auto voxelPumpEnd = std::chrono::steady_clock::now();

    const uint32_t generatedHeight = m_stats.generatedTilesLastFrame;
    const uint32_t evictedHeight = m_stats.evictedTilesLastFrame;
    const uint32_t generatedVoxelTotal = m_stats.generatedVoxelBricksLastFrame + generatedVoxel;
    const uint32_t evictedVoxelTotal = m_stats.evictedVoxelBricksLastFrame + evictedVoxel;
    const float previousVoxelMs = m_stats.pumpVoxelMsLastFrame;
    RefreshStats(generatedHeight, evictedHeight, generatedVoxelTotal, evictedVoxelTotal);
    m_stats.pumpVoxelMsLastFrame = previousVoxelMs + ElapsedMs(voxelPumpStart, voxelPumpEnd);
    return generatedVoxel;
}

bool SparseClipmapTileCache::QueueVoxelRenderFeedbackCoord(
    const SparseVoxelClipmapCoord& coord,
    uint32_t frameIndex)
{
    if (!m_config.enabled ||
        !m_config.voxelClipmapEnabled ||
        coord.ring < 0 ||
        static_cast<uint32_t>(coord.ring) >= m_config.ringCount) {
        return false;
    }

    m_voxelInterestSet.insert(coord);

    const auto resident = m_voxelSlotByCoord.find(coord);
    if (resident != m_voxelSlotByCoord.end()) {
        if (resident->second < m_voxelBricks.size()) {
            m_voxelBricks[resident->second].lastTouchedFrame = frameIndex;
        }
        RefreshStats();
        return false;
    }

    if (m_queuedVoxelSet.insert(coord).second) {
        m_voxelGenerationQueue.push_front(coord);
        if (m_config.backlogAwarePump) {
            m_voxelBacklogFirstFrame.emplace(coord, frameIndex);
        }
        RefreshStats();
        return true;
    }

    RefreshStats();
    return false;
}

bool SparseClipmapTileCache::IsVoxelCoordResident(const SparseVoxelClipmapCoord& coord) const
{
    return m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end();
}

bool SparseClipmapTileCache::HasCoarserVoxelParentForCoord(const SparseVoxelClipmapCoord& coord) const
{
    return HasCoarserVoxelParent(coord);
}

void SparseClipmapTileCache::CollectMissingVoxelInterest(
    std::vector<SparseVoxelClipmapCoord>& outCoords,
    uint32_t maxCoords) const
{
    outCoords.clear();
    if (maxCoords == 0u) {
        return;
    }
    outCoords.reserve(std::min<size_t>(
        static_cast<size_t>(maxCoords),
        m_voxelInterestSet.size()));
    for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            continue;
        }
        outCoords.push_back(coord);
        if (outCoords.size() >= static_cast<size_t>(maxCoords)) {
            break;
        }
    }
}

uint32_t SparseClipmapTileCache::SetVisiblePriorityVoxelCoords(
    const std::vector<SparseVoxelClipmapCoord>& priorityCoords,
    uint32_t frameIndex,
    bool prioritizeQueue)
{
    m_lastStatsFrame = frameIndex;
    m_visiblePriorityTaggedLastFrame = 0;
    m_visiblePriorityPrioritizedLastFrame = 0;
    m_visiblePriorityVoxelSet.clear();
    PruneAsyncVisibleReservations(frameIndex);
    if (priorityCoords.empty()) {
        return 0u;
    }

    m_visiblePriorityVoxelSet.reserve(priorityCoords.size());
    for (const SparseVoxelClipmapCoord& coord : priorityCoords) {
        if (!m_voxelInterestSet.empty() &&
            m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            continue;
        }
        if (m_queuedVoxelSet.find(coord) == m_queuedVoxelSet.end()) {
            continue;
        }
        if (m_visiblePriorityVoxelSet.insert(coord).second) {
            ++m_visiblePriorityTaggedLastFrame;
        }
    }

    if (prioritizeQueue && m_visiblePriorityTaggedLastFrame != 0u) {
        m_visiblePriorityPrioritizedLastFrame =
            PrioritizeVoxelGenerationCoords(priorityCoords);
    }
    if (m_visiblePriorityTaggedLastFrame != 0u) {
        PrioritizeAsyncVoxelGenerationQueue();
    }

    return m_visiblePriorityPrioritizedLastFrame;
}

uint32_t SparseClipmapTileCache::QueuePredictedVisibleVoxelInterest(
    float cameraX,
    float cameraY,
    float cameraZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    float rightX,
    float rightY,
    float rightZ,
    float upX,
    float upY,
    float upZ,
    float fovYRadians,
    float aspectRatio,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t maxCoords,
    uint32_t sampleSide,
    float maxDistance,
    uint32_t deadlineFrame,
    uint32_t sampleIndex)
{
    if (maxCoords == 0u ||
        !policy.IsEnabled() ||
        !policy.Config().voxelClipmapEnabled ||
        m_voxelBricks.empty()) {
        return 0u;
    }
    (void)rightX;
    (void)rightY;
    (void)rightZ;
    (void)upX;
    (void)upY;
    (void)upZ;
    (void)fovYRadians;
    (void)aspectRatio;
    (void)sampleSide;
    (void)maxDistance;

    const SparseClipmapConfig& config = policy.Config();
    cameraX = FiniteOr(cameraX, 0.0f);
    cameraY = FiniteOr(cameraY, 0.0f);
    cameraZ = FiniteOr(cameraZ, 0.0f);
    forwardX = FiniteOr(forwardX, 0.0f);
    forwardY = FiniteOr(forwardY, 0.0f);
    forwardZ = FiniteOr(forwardZ, 0.0f);

    const auto normalize3 = [](float& x, float& y, float& z) {
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len <= 0.001f) {
            return false;
        }
        const float invLen = 1.0f / len;
        x *= invLen;
        y *= invLen;
        z *= invLen;
        return true;
    };
    if (!normalize3(forwardX, forwardY, forwardZ)) {
        return 0u;
    }

    const bool phaseDetail = true;
    if (m_predictedVisibleAdmissionStatsFrame != frameIndex) {
        m_predictedVisibleAdmissionStatsFrame = frameIndex;
        m_predictedVisibleAdmissionSamplesLastFrame = 0u;
        m_predictedVisibleAdmissionSnapshotMsLastFrame = 0.0f;
        m_predictedVisibleAdmissionRebuildMsLastFrame = 0.0f;
        m_predictedVisibleAdmissionRestoreMsLastFrame = 0.0f;
        m_predictedVisibleAdmissionQueueMsLastFrame = 0.0f;
    }
    ++m_predictedVisibleAdmissionSamplesLastFrame;

    const auto snapshotStart = phaseDetail
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    const std::deque<SparseVoxelClipmapCoord> liveVoxelGenerationQueue = m_voxelGenerationQueue;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveQueuedVoxelSet =
        m_queuedVoxelSet;
    std::unordered_map<SparseVoxelClipmapCoord, uint32_t, SparseVoxelClipmapCoordHash>
        liveVoxelBacklogFirstFrame = m_voxelBacklogFirstFrame;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveVoxelInterestSet =
        m_voxelInterestSet;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveVisiblePriorityVoxelSet =
        m_visiblePriorityVoxelSet;
    const SparseClipmapCacheStats liveStats = m_stats;
    const uint32_t liveLastStatsFrame = m_lastStatsFrame;
    const uint32_t liveVisiblePriorityTaggedLastFrame = m_visiblePriorityTaggedLastFrame;
    const uint32_t liveVisiblePriorityPrioritizedLastFrame = m_visiblePriorityPrioritizedLastFrame;
    const uint32_t liveBacklogVoxelEnqueuedLastFrame = m_backlogVoxelEnqueuedLastFrame;
    const uint32_t liveBacklogVoxelCarriedLastFrame = m_backlogVoxelCarriedLastFrame;
    const uint32_t livePrunedVoxelBacklogLastFrame = m_prunedVoxelBacklogLastFrame;
    const uint32_t liveBacklogVoxelResidentSkipLastFrame = m_backlogVoxelResidentSkipLastFrame;
    if (phaseDetail) {
        m_predictedVisibleAdmissionSnapshotMsLastFrame +=
            ElapsedMs(snapshotStart, std::chrono::steady_clock::now());
    }

    const auto predictedRebuildStart = phaseDetail
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    UpdateVoxelInterest(
        cameraX,
        cameraY,
        cameraZ,
        frameIndex,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        false);
    const std::deque<SparseVoxelClipmapCoord> predictedVoxelGenerationQueue = m_voxelGenerationQueue;
    if (phaseDetail) {
        m_predictedVisibleAdmissionRebuildMsLastFrame +=
            ElapsedMs(predictedRebuildStart, std::chrono::steady_clock::now());
    }

    const auto restoreStart = phaseDetail
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    m_voxelGenerationQueue = liveVoxelGenerationQueue;
    m_queuedVoxelSet = liveQueuedVoxelSet;
    m_voxelBacklogFirstFrame = liveVoxelBacklogFirstFrame;
    m_voxelInterestSet = liveVoxelInterestSet;
    m_visiblePriorityVoxelSet = liveVisiblePriorityVoxelSet;
    m_stats = liveStats;
    m_lastStatsFrame = liveLastStatsFrame;
    m_visiblePriorityTaggedLastFrame = liveVisiblePriorityTaggedLastFrame;
    m_visiblePriorityPrioritizedLastFrame = liveVisiblePriorityPrioritizedLastFrame;
    m_backlogVoxelEnqueuedLastFrame = liveBacklogVoxelEnqueuedLastFrame;
    m_backlogVoxelCarriedLastFrame = liveBacklogVoxelCarriedLastFrame;
    m_prunedVoxelBacklogLastFrame = livePrunedVoxelBacklogLastFrame;
    m_backlogVoxelResidentSkipLastFrame = liveBacklogVoxelResidentSkipLastFrame;
    if (phaseDetail) {
        m_predictedVisibleAdmissionRestoreMsLastFrame +=
            ElapsedMs(restoreStart, std::chrono::steady_clock::now());
    }

    const auto queueStart = phaseDetail
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    uint32_t queued = 0u;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> emitted;
    std::vector<SparseVoxelClipmapCoord> emittedOrder;
    emitted.reserve(maxCoords);
    emittedOrder.reserve(maxCoords);
    const auto queuePredictedCoord = [&](const SparseVoxelClipmapCoord& coord) {
        if (queued >= maxCoords ||
            coord.ring < 0 ||
            static_cast<uint32_t>(coord.ring) >= config.ringCount ||
            !emitted.insert(coord).second) {
            return;
        }

        if (m_visiblePriorityVoxelSet.insert(coord).second) {
            ++m_visiblePriorityTaggedLastFrame;
        }
        auto& reservation = m_asyncVisibleReservations[coord];
        const uint32_t effectiveDeadlineFrame =
            deadlineFrame != 0u ? deadlineFrame : frameIndex + 12u;
        if (reservation.firstFrame == 0u) {
            reservation.firstFrame = frameIndex;
            reservation.deadlineFrame = effectiveDeadlineFrame;
            reservation.sampleIndex = sampleIndex;
        } else {
            reservation.deadlineFrame = std::min(
                reservation.deadlineFrame,
                effectiveDeadlineFrame);
            reservation.sampleIndex = std::min(reservation.sampleIndex, sampleIndex);
        }
        reservation.lastSeenFrame = frameIndex;
        emittedOrder.push_back(coord);

        const auto resident = m_voxelSlotByCoord.find(coord);
        if (resident != m_voxelSlotByCoord.end()) {
            if (resident->second < m_voxelBricks.size()) {
                m_voxelBricks[resident->second].lastTouchedFrame = frameIndex;
            }
            return;
        }

        if (m_queuedVoxelSet.insert(coord).second) {
            m_voxelGenerationQueue.push_back(coord);
            if (config.backlogAwarePump) {
                m_voxelBacklogFirstFrame.emplace(coord, frameIndex);
            }
            ++queued;
        }
    };
    for (const SparseVoxelClipmapCoord& coord : predictedVoxelGenerationQueue) {
        queuePredictedCoord(coord);
        if (queued >= maxCoords) {
            break;
        }
    }
    if (phaseDetail) {
        m_predictedVisibleAdmissionQueueMsLastFrame +=
            ElapsedMs(queueStart, std::chrono::steady_clock::now());
    }

    if (queued != 0u) {
        (void)PrioritizeVoxelGenerationCoords(emittedOrder);
    }
    if (!emittedOrder.empty()) {
        PrioritizeAsyncVoxelGenerationQueue();
    }
    RefreshStats();
    return queued;
}

uint32_t SparseClipmapTileCache::CollectPredictedVisibleVoxelInterestForDebug(
    std::vector<SparseVoxelClipmapCoord>& outCoords,
    float cameraX,
    float cameraY,
    float cameraZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t maxCoords)
{
    outCoords.clear();
    if (maxCoords == 0u ||
        !policy.IsEnabled() ||
        !policy.Config().voxelClipmapEnabled ||
        m_voxelBricks.empty()) {
        return 0u;
    }

    cameraX = FiniteOr(cameraX, 0.0f);
    cameraY = FiniteOr(cameraY, 0.0f);
    cameraZ = FiniteOr(cameraZ, 0.0f);
    forwardX = FiniteOr(forwardX, 0.0f);
    forwardY = FiniteOr(forwardY, 0.0f);
    forwardZ = FiniteOr(forwardZ, 0.0f);
    const float forwardLen = std::sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    if (forwardLen <= 0.001f) {
        return 0u;
    }
    const float invForwardLen = 1.0f / forwardLen;
    forwardX *= invForwardLen;
    forwardY *= invForwardLen;
    forwardZ *= invForwardLen;

    const std::deque<SparseVoxelClipmapCoord> liveVoxelGenerationQueue = m_voxelGenerationQueue;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveQueuedVoxelSet =
        m_queuedVoxelSet;
    const std::unordered_map<SparseVoxelClipmapCoord, uint32_t, SparseVoxelClipmapCoordHash>
        liveVoxelBacklogFirstFrame = m_voxelBacklogFirstFrame;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveVoxelInterestSet =
        m_voxelInterestSet;
    const std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> liveVisiblePriorityVoxelSet =
        m_visiblePriorityVoxelSet;
    const SparseClipmapCacheStats liveStats = m_stats;
    const uint32_t liveLastStatsFrame = m_lastStatsFrame;
    const uint32_t liveVisiblePriorityTaggedLastFrame = m_visiblePriorityTaggedLastFrame;
    const uint32_t liveVisiblePriorityPrioritizedLastFrame = m_visiblePriorityPrioritizedLastFrame;
    const uint32_t liveBacklogVoxelEnqueuedLastFrame = m_backlogVoxelEnqueuedLastFrame;
    const uint32_t liveBacklogVoxelCarriedLastFrame = m_backlogVoxelCarriedLastFrame;
    const uint32_t livePrunedVoxelBacklogLastFrame = m_prunedVoxelBacklogLastFrame;
    const uint32_t liveBacklogVoxelResidentSkipLastFrame = m_backlogVoxelResidentSkipLastFrame;

    UpdateVoxelInterest(
        cameraX,
        cameraY,
        cameraZ,
        frameIndex,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        false);

    outCoords.reserve(std::min<size_t>(
        static_cast<size_t>(maxCoords),
        m_voxelGenerationQueue.size()));
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> emitted;
    emitted.reserve(maxCoords);
    for (const SparseVoxelClipmapCoord& coord : m_voxelGenerationQueue) {
        if (outCoords.size() >= static_cast<size_t>(maxCoords)) {
            break;
        }
        if (coord.ring < 0 ||
            static_cast<uint32_t>(coord.ring) >= policy.Config().ringCount ||
            !emitted.insert(coord).second) {
            continue;
        }
        outCoords.push_back(coord);
    }

    m_voxelGenerationQueue = liveVoxelGenerationQueue;
    m_queuedVoxelSet = liveQueuedVoxelSet;
    m_voxelBacklogFirstFrame = liveVoxelBacklogFirstFrame;
    m_voxelInterestSet = liveVoxelInterestSet;
    m_visiblePriorityVoxelSet = liveVisiblePriorityVoxelSet;
    m_stats = liveStats;
    m_lastStatsFrame = liveLastStatsFrame;
    m_visiblePriorityTaggedLastFrame = liveVisiblePriorityTaggedLastFrame;
    m_visiblePriorityPrioritizedLastFrame = liveVisiblePriorityPrioritizedLastFrame;
    m_backlogVoxelEnqueuedLastFrame = liveBacklogVoxelEnqueuedLastFrame;
    m_backlogVoxelCarriedLastFrame = liveBacklogVoxelCarriedLastFrame;
    m_prunedVoxelBacklogLastFrame = livePrunedVoxelBacklogLastFrame;
    m_backlogVoxelResidentSkipLastFrame = liveBacklogVoxelResidentSkipLastFrame;
    return static_cast<uint32_t>(std::min<size_t>(
        outCoords.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
}

uint32_t SparseClipmapTileCache::CollectPredictedVisibleVoxelInterestPureForDebug(
    std::vector<SparseVoxelClipmapCoord>& outCoords,
    float cameraX,
    float cameraY,
    float cameraZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t maxCoords,
    std::vector<SparseVoxelClipmapCoord>* outResidentTouchCoords) const
{
    (void)frameIndex;
    outCoords.clear();
    if (outResidentTouchCoords) {
        outResidentTouchCoords->clear();
    }
    if (maxCoords == 0u ||
        !policy.IsEnabled() ||
        !policy.Config().voxelClipmapEnabled ||
        m_voxelBricks.empty()) {
        return 0u;
    }

    cameraX = FiniteOr(cameraX, 0.0f);
    cameraY = FiniteOr(cameraY, 0.0f);
    cameraZ = FiniteOr(cameraZ, 0.0f);
    forwardX = FiniteOr(forwardX, 0.0f);
    forwardY = FiniteOr(forwardY, 0.0f);
    forwardZ = FiniteOr(forwardZ, 0.0f);
    const float forwardLen = std::sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    if (forwardLen <= 0.001f) {
        return 0u;
    }
    const float invForwardLen = 1.0f / forwardLen;
    forwardX *= invForwardLen;
    forwardY *= invForwardLen;
    forwardZ *= invForwardLen;

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
        int32_t radiusYBonus = 0;
    };

    const SparseClipmapConfig& config = policy.Config();
    const auto rings = policy.BuildRings();
    const int32_t radiusXz = static_cast<int32_t>(config.voxelBrickRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(config.voxelBrickRadiusY);
    const uint32_t ringCount = std::max(1u, static_cast<uint32_t>(rings.size()));
    const uint32_t maxResidentInterest = std::max<uint32_t>(
        ringCount,
        static_cast<uint32_t>(
            (static_cast<uint64_t>(m_voxelBricks.size()) *
             static_cast<uint64_t>(config.voxelInterestCapacityPercent)) / 100u));
    std::vector<uint32_t> ringQuotas(ringCount, 1u);
    uint64_t ringWeightSum = 0;
    for (uint32_t ring = 0; ring < ringCount; ++ring) {
        ringWeightSum += static_cast<uint64_t>((ringCount - ring) + ringCount);
    }
    uint32_t assignedRingQuota = 0;
    for (uint32_t ring = 0; ring < ringCount; ++ring) {
        const uint32_t weight = (ringCount - ring) + ringCount;
        ringQuotas[ring] = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(
                (static_cast<uint64_t>(maxResidentInterest) * weight) /
                std::max<uint64_t>(1ull, ringWeightSum)));
        assignedRingQuota += ringQuotas[ring];
    }
    for (uint32_t ring = 0;
         assignedRingQuota < maxResidentInterest && ring < ringCount;
         ring = (ring + 1u) % ringCount) {
        ++ringQuotas[ring];
        ++assignedRingQuota;
    }
    for (uint32_t ring = ringCount;
         assignedRingQuota > maxResidentInterest && ring > 0u;
         --ring) {
        const uint32_t idx = ring - 1u;
        if (ringQuotas[idx] > 1u) {
            --ringQuotas[idx];
            --assignedRingQuota;
        }
    }

    const float forwardNormX = forwardX;
    const float forwardNormY = forwardY;
    const float forwardNormZ = forwardZ;
    const float predictedX = cameraX;
    const float predictedY = cameraY;
    const float predictedZ = cameraZ;
    const size_t reserveHint = std::min<size_t>(
        static_cast<size_t>(maxCoords),
        static_cast<size_t>(maxResidentInterest) + m_voxelGenerationQueue.size());
    outCoords.reserve(reserveHint);
    if (outResidentTouchCoords) {
        outResidentTouchCoords->reserve(maxResidentInterest);
    }
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> queuedSet;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> interestSet;
    queuedSet.reserve(reserveHint);
    interestSet.reserve(maxResidentInterest);

    const auto compareCandidate =
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
        };

    for (uint32_t ring = 0; ring < rings.size(); ++ring) {
        const float brickWorldSize = std::max(1.0f, rings[ring].cellSize * static_cast<float>(SPARSE_BRICK_SIZE));
        std::vector<VoxelInterestAnchor> anchors;
        anchors.reserve(3u + config.motionLookaheadSteps);
        anchors.push_back({cameraX, cameraY, cameraZ, 0u, 0});
        anchors.push_back(
            {
                cameraX + forwardNormX * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                cameraY + forwardNormY * brickWorldSize * 0.5f,
                cameraZ + forwardNormZ * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                650u,
                -std::max(1, radiusXz / 2)
            });
        const int32_t predictedAnchorRadiusBias = -std::max(1, radiusXz / 2);
        const int32_t predictedAnchorRadiusXz = std::max(1, radiusXz + predictedAnchorRadiusBias);
        const int32_t cameraAnchorCenterX = FloorToGridCoordClamped(cameraX, brickWorldSize, radiusXz + 2);
        const int32_t cameraAnchorCenterY = FloorToGridCoordClamped(cameraY, brickWorldSize, radiusY + 2);
        const int32_t cameraAnchorCenterZ = FloorToGridCoordClamped(cameraZ, brickWorldSize, radiusXz + 2);
        const int32_t predictedAnchorCenterX =
            FloorToGridCoordClamped(predictedX, brickWorldSize, predictedAnchorRadiusXz + 2);
        const int32_t predictedAnchorCenterY = FloorToGridCoordClamped(predictedY, brickWorldSize, radiusY + 2);
        const int32_t predictedAnchorCenterZ =
            FloorToGridCoordClamped(predictedZ, brickWorldSize, predictedAnchorRadiusXz + 2);
        if (predictedAnchorCenterX != cameraAnchorCenterX ||
            predictedAnchorCenterY != cameraAnchorCenterY ||
            predictedAnchorCenterZ != cameraAnchorCenterZ) {
            anchors.push_back({predictedX, predictedY, predictedZ, 900u, predictedAnchorRadiusBias});
        }
        const float forwardLenXzForRingAnchor =
            std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
        if (cameraY <= 384.0f && forwardLenXzForRingAnchor > 0.001f) {
            anchors.push_back({
                cameraX + forwardNormX * rings[ring].endDistance,
                cameraY + forwardNormY * brickWorldSize * 0.5f,
                cameraZ + forwardNormZ * rings[ring].endDistance,
                8u,
                -1,
                2
            });
            if (ring == 0u) {
                const float rightX = forwardNormZ / forwardLenXzForRingAnchor;
                const float rightZ = -forwardNormX / forwardLenXzForRingAnchor;
                constexpr float kFineRingAnchorHalfWidth = 0.52f;
                constexpr float kFineRingSideMidDistance = 0.74f;
                constexpr float kFineRingSideMidHalfWidth = 0.72f;
                for (int32_t fan = -2; fan <= 2; ++fan) {
                    if (fan == 0) {
                        continue;
                    }
                    const float fanScale = static_cast<float>(fan) * 0.5f;
                    anchors.push_back({
                        cameraX + forwardNormX * rings[ring].endDistance +
                            rightX * rings[ring].endDistance * kFineRingAnchorHalfWidth * fanScale,
                        cameraY + forwardNormY * brickWorldSize * 0.5f,
                        cameraZ + forwardNormZ * rings[ring].endDistance +
                            rightZ * rings[ring].endDistance * kFineRingAnchorHalfWidth * fanScale,
                        10u + static_cast<uint32_t>(std::abs(fan)) * 3u,
                        -3,
                        2
                    });
                    const float sideMidDistance = rings[ring].endDistance * kFineRingSideMidDistance;
                    anchors.push_back({
                        cameraX + forwardNormX * sideMidDistance +
                            rightX * sideMidDistance * kFineRingSideMidHalfWidth * fanScale,
                        cameraY + forwardNormY * brickWorldSize * 0.5f,
                        cameraZ + forwardNormZ * sideMidDistance +
                            rightZ * sideMidDistance * kFineRingSideMidHalfWidth * fanScale,
                        6u + static_cast<uint32_t>(std::abs(fan)) * 2u,
                        -4,
                        2
                    });
                }
            }
        }

        std::vector<VoxelInterestCandidate> candidates;
        std::unordered_map<SparseVoxelClipmapCoord, size_t, SparseVoxelClipmapCoordHash> candidateIndexByCoord;
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
            const uint32_t score =
                baseScore +
                static_cast<uint32_t>(dx * dx * 4 + dy * dy * 9 + dz * dz * 4);
            auto existing = candidateIndexByCoord.find(coord);
            if (existing != candidateIndexByCoord.end()) {
                VoxelInterestCandidate& candidate = candidates[existing->second];
                if (score < candidate.distanceScore) {
                    candidate.dx = dx;
                    candidate.dy = dy;
                    candidate.dz = dz;
                    candidate.distanceScore = score;
                }
                return;
            }
            candidateIndexByCoord.emplace(coord, candidates.size());
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
                config.motionLookaheadSteps * 3u + 4u);
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
                const int32_t sampleX = FloorToInt32Clamped(
                    (static_cast<double>(lineCoord.x) + 0.5) * static_cast<double>(brickWorldSize));
                const int32_t sampleZ = FloorToInt32Clamped(
                    (static_cast<double>(lineCoord.z) + 0.5) * static_cast<double>(brickWorldSize));
                const float terrainY = m_terrain.HeightAt(sampleX, sampleZ);
                terrainCenterYs.push_back(FloorToGridCoordClamped(terrainY, brickWorldSize, radiusY + 2));
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
                        SaturatingAddInt32(terrainCenterY, dy),
                        lineCoord.z,
                        0,
                        dy,
                        0,
                        baseScore + 10u + static_cast<uint32_t>(i) * 2u);
                }
            }
        };

        if (cameraY > 384.0f) {
            const float highViewDistance = std::max(
                brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                rings[ring].endDistance);
            addTerrainCenterlineCandidates(
                cameraX,
                cameraZ,
                cameraX + forwardNormX * highViewDistance,
                cameraZ + forwardNormZ * highViewDistance,
                12u);
            const float highForwardLenXz =
                std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
            if (highForwardLenXz > 0.001f) {
                const float rightX = forwardNormZ / highForwardLenXz;
                const float rightZ = -forwardNormX / highForwardLenXz;
                constexpr float kHighViewFanHalfWidth = 0.42f;
                for (int32_t fan = -1; fan <= 1; fan += 2) {
                    const float fanEndX =
                        cameraX + forwardNormX * highViewDistance +
                        rightX * highViewDistance * kHighViewFanHalfWidth * static_cast<float>(fan);
                    const float fanEndZ =
                        cameraZ + forwardNormZ * highViewDistance +
                        rightZ * highViewDistance * kHighViewFanHalfWidth * static_cast<float>(fan);
                    addTerrainCenterlineCandidates(
                        cameraX,
                        cameraZ,
                        fanEndX,
                        fanEndZ,
                        18u);
                }
            }
        }
        addTerrainCenterlineCandidates(
            cameraX,
            cameraZ,
            cameraX + forwardNormX * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            cameraZ + forwardNormZ * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            45u);
        const float forwardLenXz = std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
        if (forwardLenXz > 0.001f) {
            const float rightX = forwardNormZ;
            const float rightZ = -forwardNormX;
            const float fanDistance =
                brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz));
            constexpr float kViewFanHalfWidth = 0.72f;
            for (int32_t fan = -2; fan <= 2; ++fan) {
                if (fan == 0) {
                    continue;
                }
                const float fanScale = static_cast<float>(fan) * 0.5f;
                const float fanEndX =
                    cameraX + forwardNormX * fanDistance + rightX * fanDistance * kViewFanHalfWidth * fanScale;
                const float fanEndZ =
                    cameraZ + forwardNormZ * fanDistance + rightZ * fanDistance * kViewFanHalfWidth * fanScale;
                addTerrainCenterlineCandidates(
                    cameraX,
                    cameraZ,
                    fanEndX,
                    fanEndZ,
                    35u + static_cast<uint32_t>(std::abs(fan)) * 8u);
            }
        }
        for (const VoxelInterestAnchor& anchor : anchors) {
            const int32_t anchorRadiusXz = std::max(1, radiusXz + anchor.radiusBias);
            const int32_t anchorRadiusY = std::max(1, radiusY + anchor.radiusYBonus);
            const int32_t centerX = FloorToGridCoordClamped(anchor.x, brickWorldSize, anchorRadiusXz + 2);
            const int32_t centerY = FloorToGridCoordClamped(anchor.y, brickWorldSize, anchorRadiusY + 2);
            const int32_t centerZ = FloorToGridCoordClamped(anchor.z, brickWorldSize, anchorRadiusXz + 2);
            for (int32_t dz = -anchorRadiusXz; dz <= anchorRadiusXz; ++dz) {
                for (int32_t dx = -anchorRadiusXz; dx <= anchorRadiusXz; ++dx) {
                    const int32_t brickX = SaturatingAddInt32(centerX, dx);
                    const int32_t brickZ = SaturatingAddInt32(centerZ, dz);
                    const int32_t sampleX = FloorToInt32Clamped(
                        (static_cast<double>(brickX) + 0.5) * static_cast<double>(brickWorldSize));
                    const int32_t sampleZ = FloorToInt32Clamped(
                        (static_cast<double>(brickZ) + 0.5) * static_cast<double>(brickWorldSize));
                    const float terrainY = m_terrain.HeightAt(sampleX, sampleZ);
                    const int32_t terrainCenterY = FloorToGridCoordClamped(terrainY, brickWorldSize, anchorRadiusY + 2);
                    int32_t footprintCenterY = terrainCenterY;
                    if (anchor.radiusYBonus > 0) {
                        float maxFootprintY = terrainY;
                        for (int32_t oz = -1; oz <= 1; ++oz) {
                            for (int32_t ox = -1; ox <= 1; ++ox) {
                                if (ox == 0 && oz == 0) {
                                    continue;
                                }
                                const int32_t neighborX = FloorToInt32Clamped(
                                    static_cast<double>(sampleX) +
                                    static_cast<double>(ox) * static_cast<double>(brickWorldSize));
                                const int32_t neighborZ = FloorToInt32Clamped(
                                    static_cast<double>(sampleZ) +
                                    static_cast<double>(oz) * static_cast<double>(brickWorldSize));
                                maxFootprintY = std::max(
                                    maxFootprintY,
                                    m_terrain.HeightAt(neighborX, neighborZ));
                            }
                        }
                        footprintCenterY = FloorToGridCoordClamped(
                            maxFootprintY,
                            brickWorldSize,
                            anchorRadiusY + 2);
                    }
                    const auto addVerticalBand = [&](
                        int32_t verticalCenterY,
                        uint32_t scoreBias) {
                        for (int32_t dy = -anchorRadiusY; dy <= anchorRadiusY; ++dy) {
                            addCandidate(
                                brickX,
                                SaturatingAddInt32(verticalCenterY, dy),
                                brickZ,
                                dx,
                                dy,
                                dz,
                                anchor.baseScore + scoreBias);
                        }
                    };
                    addVerticalBand(terrainCenterY, 0u);
                    if (footprintCenterY != terrainCenterY) {
                        addVerticalBand(footprintCenterY, 6u);
                    }
                    const auto yInsideVerticalBand = [&](int32_t y, int32_t verticalCenterY) {
                        return y >= SaturatingAddInt32(verticalCenterY, -anchorRadiusY) &&
                            y <= SaturatingAddInt32(verticalCenterY, anchorRadiusY);
                    };
                    for (int32_t dy = -anchorRadiusY; dy <= anchorRadiusY; ++dy) {
                        const int32_t cameraBandY = SaturatingAddInt32(centerY, dy);
                        if (
                            yInsideVerticalBand(cameraBandY, terrainCenterY) ||
                            (footprintCenterY != terrainCenterY &&
                                yInsideVerticalBand(cameraBandY, footprintCenterY))) {
                            continue;
                        }
                        addCandidate(
                            brickX,
                            cameraBandY,
                            brickZ,
                            dx,
                            dy,
                            dz,
                            anchor.baseScore + 5000u);
                    }
                }
            }
        }

        const uint32_t ringQuota = ringQuotas[ring];
        const uint32_t emitCount = std::min<uint32_t>(ringQuota, static_cast<uint32_t>(candidates.size()));
        if (emitCount < candidates.size()) {
            std::nth_element(
                candidates.begin(),
                candidates.begin() + emitCount,
                candidates.end(),
                compareCandidate);
            std::sort(
                candidates.begin(),
                candidates.begin() + emitCount,
                compareCandidate);
        } else {
            std::sort(candidates.begin(), candidates.end(), compareCandidate);
        }
        for (uint32_t i = 0; i < emitCount; ++i) {
            const SparseVoxelClipmapCoord& coord = candidates[i].coord;
            interestSet.insert(coord);
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                if (outResidentTouchCoords) {
                    outResidentTouchCoords->push_back(coord);
                }
                continue;
            }
            if (queuedSet.insert(coord).second) {
                outCoords.push_back(coord);
            }
        }
    }

    if (config.backlogAwarePump) {
        for (const SparseVoxelClipmapCoord& coord : m_voxelGenerationQueue) {
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                continue;
            }
            if (interestSet.find(coord) == interestSet.end()) {
                continue;
            }
            if (queuedSet.insert(coord).second) {
                outCoords.push_back(coord);
            }
        }
    }

    if (outCoords.size() > static_cast<size_t>(maxCoords)) {
        outCoords.resize(maxCoords);
    }
    return static_cast<uint32_t>(std::min<size_t>(
        outCoords.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
}

uint32_t SparseClipmapTileCache::QueueAsyncVisibleReservationVoxelCoords(
    const std::vector<SparseVoxelClipmapCoord>& reservationCoords,
    uint32_t frameIndex,
    const SparseClipmapPolicy& policy,
    uint32_t deadlineFrame,
    uint32_t sampleIndex,
    uint32_t staleFrames)
{
    PruneAsyncVisibleReservations(frameIndex, staleFrames);
    if (reservationCoords.empty() ||
        !policy.Config().asyncVisibleCriticalGeneration ||
        policy.Config().asyncNoncriticalGenerationQueueMax == 0u ||
        (m_edits && m_edits->EditedBrickCount() != 0u)) {
        return 0u;
    }

    const uint32_t effectiveDeadlineFrame =
        deadlineFrame != 0u ? deadlineFrame : frameIndex;
    m_asyncVisibleReservations.reserve(
        m_asyncVisibleReservations.size() + reservationCoords.size());
    for (const SparseVoxelClipmapCoord& coord : reservationCoords) {
        if (!m_voxelInterestSet.empty() &&
            m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
            continue;
        }
        if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
            continue;
        }
        if (m_queuedVoxelSet.find(coord) == m_queuedVoxelSet.end()) {
            continue;
        }
        AsyncVisibleReservation& reservation = m_asyncVisibleReservations[coord];
        if (reservation.firstFrame == 0u) {
            reservation.firstFrame = frameIndex;
            reservation.deadlineFrame = effectiveDeadlineFrame;
            reservation.sampleIndex = sampleIndex;
        } else {
            reservation.deadlineFrame = std::min(
                reservation.deadlineFrame,
                effectiveDeadlineFrame);
            reservation.sampleIndex = std::min(reservation.sampleIndex, sampleIndex);
        }
        reservation.lastSeenFrame = frameIndex;
    }

    struct ReservationCandidate {
        SparseVoxelClipmapCoord coord;
        uint32_t deadlineFrame = 0;
        uint32_t sampleIndex = 0;
        uint32_t firstFrame = 0;
    };
    std::vector<ReservationCandidate> candidates;
    candidates.reserve(m_asyncVisibleReservations.size());
    for (const auto& [coord, reservation] : m_asyncVisibleReservations) {
        candidates.push_back({
            coord,
            reservation.deadlineFrame,
            reservation.sampleIndex,
            reservation.firstFrame
        });
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const ReservationCandidate& a, const ReservationCandidate& b) {
            if (a.deadlineFrame != b.deadlineFrame) {
                return a.deadlineFrame < b.deadlineFrame;
            }
            if (a.sampleIndex != b.sampleIndex) {
                return a.sampleIndex < b.sampleIndex;
            }
            if (a.firstFrame != b.firstFrame) {
                return a.firstFrame < b.firstFrame;
            }
            if (a.coord.ring != b.coord.ring) {
                return a.coord.ring < b.coord.ring;
            }
            if (a.coord.y != b.coord.y) {
                return a.coord.y < b.coord.y;
            }
            if (a.coord.x != b.coord.x) {
                return a.coord.x < b.coord.x;
            }
            return a.coord.z < b.coord.z;
        });

    uint32_t queued = 0u;
    for (const ReservationCandidate& candidate : candidates) {
        const uint32_t enqueuedBefore =
            m_asyncVisibleCriticalGenerationEnqueuedLastFrame;
        if (TryQueueAsyncVoxelGeneration(
                candidate.coord,
                frameIndex,
                policy,
                true,
                true)) {
            queued +=
                m_asyncVisibleCriticalGenerationEnqueuedLastFrame -
                enqueuedBefore;
        }
    }
    if (!reservationCoords.empty()) {
        PrioritizeAsyncVoxelGenerationQueue();
    }
    return queued;
}

uint32_t SparseClipmapTileCache::CountAsyncVisibleReservationMatches(
    const std::vector<SparseVoxelClipmapCoord>& coords) const
{
    uint32_t matches = 0u;
    for (const SparseVoxelClipmapCoord& coord : coords) {
        if (m_asyncVisibleReservations.find(coord) !=
            m_asyncVisibleReservations.end()) {
            ++matches;
        }
    }
    return matches;
}

uint32_t SparseClipmapTileCache::CountVisiblePriorityMatches(
    const std::vector<SparseVoxelClipmapCoord>& coords) const
{
    uint32_t matches = 0u;
    for (const SparseVoxelClipmapCoord& coord : coords) {
        if (m_visiblePriorityVoxelSet.find(coord) !=
            m_visiblePriorityVoxelSet.end()) {
            ++matches;
        }
    }
    return matches;
}

uint32_t SparseClipmapTileCache::PrioritizeVoxelGenerationCoords(
    const std::vector<SparseVoxelClipmapCoord>& priorityCoords)
{
    if (priorityCoords.empty() || m_voxelGenerationQueue.size() <= 1) {
        return 0u;
    }

    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> prioritySet;
    prioritySet.reserve(priorityCoords.size());
    for (const SparseVoxelClipmapCoord& coord : priorityCoords) {
        prioritySet.insert(coord);
    }

    std::deque<SparseVoxelClipmapCoord> priorityQueue;
    std::deque<SparseVoxelClipmapCoord> remainingQueue;
    uint32_t prioritized = 0u;
    for (const SparseVoxelClipmapCoord& coord : m_voxelGenerationQueue) {
        if (prioritySet.find(coord) != prioritySet.end() &&
            m_voxelSlotByCoord.find(coord) == m_voxelSlotByCoord.end()) {
            priorityQueue.push_back(coord);
            ++prioritized;
        } else {
            remainingQueue.push_back(coord);
        }
    }

    if (prioritized == 0u) {
        return 0u;
    }

    priorityQueue.insert(
        priorityQueue.end(),
        std::make_move_iterator(remainingQueue.begin()),
        std::make_move_iterator(remainingQueue.end()));
    m_voxelGenerationQueue.swap(priorityQueue);
    return prioritized;
}

void SparseClipmapTileCache::RecordVoxelGenerationTiming(
    const SparseVoxelClipmapCoord& coord,
    float elapsedMs)
{
    elapsedMs = std::max(0.0f, FiniteOr(elapsedMs, 0.0f));
    m_generatedVoxelMsAccumLastFrame += elapsedMs;
    m_generatedVoxelMaxMsLastFrame = std::max(m_generatedVoxelMaxMsLastFrame, elapsedMs);
    ++m_generatedVoxelTimingCountLastFrame;
    if (coord.ring >= 0 &&
        static_cast<uint32_t>(coord.ring) < SPARSE_CLIPMAP_MAX_STATS_RINGS) {
        ++m_generatedVoxelBricksByRingLastFrame[static_cast<uint32_t>(coord.ring)];
    }
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
    tile.record.originX =
        FloorToInt32Clamped(static_cast<double>(tile.record.coord.x) * static_cast<double>(tileWorldSize));
    tile.record.originZ =
        FloorToInt32Clamped(static_cast<double>(tile.record.coord.z) * static_cast<double>(tileWorldSize));
    tile.packedSamples.resize(static_cast<size_t>(side) * static_cast<size_t>(side));

    for (uint32_t z = 0; z < side; ++z) {
        for (uint32_t x = 0; x < side; ++x) {
            const int32_t worldX = RoundToInt32Clamped(
                static_cast<double>(tile.record.originX) +
                static_cast<double>(x) * static_cast<double>(ring.cellSize));
            const int32_t worldZ = RoundToInt32Clamped(
                static_cast<double>(tile.record.originZ) +
                static_cast<double>(z) * static_cast<double>(ring.cellSize));
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
    float predictionSeconds,
    bool allowSignatureReuse)
{
    m_lastStatsFrame = frameIndex;
    if (!policy.IsEnabled() || !policy.Config().voxelClipmapEnabled || m_voxelBricks.empty()) {
        m_voxelInterestSet.clear();
        m_voxelGenerationQueue.clear();
        m_queuedVoxelSet.clear();
        m_voxelBacklogFirstFrame.clear();
        m_visiblePriorityVoxelSet.clear();
        m_lastVoxelInterestSignatureValid = false;
        RefreshStats();
        m_stats.voxelInterestAnchors = 0;
        return;
    }

    cameraX = FiniteOr(cameraX, 0.0f);
    cameraY = FiniteOr(cameraY, 0.0f);
    cameraZ = FiniteOr(cameraZ, 0.0f);
    forwardX = FiniteOr(forwardX, 0.0f);
    forwardY = FiniteOr(forwardY, 0.0f);
    forwardZ = FiniteOr(forwardZ, 0.0f);
    velocityX = FiniteOr(velocityX, 0.0f);
    velocityY = FiniteOr(velocityY, 0.0f);
    velocityZ = FiniteOr(velocityZ, 0.0f);
    predictionSeconds = std::max(0.0f, FiniteOr(predictionSeconds, 0.0f));

    const bool backlogAwarePump = policy.Config().backlogAwarePump;
    const bool voxelInterestDetail = policy.Config().voxelInterestDetail;
    const auto rings = policy.BuildRings();
    const int32_t radiusXz = static_cast<int32_t>(policy.Config().voxelBrickRadiusXz);
    const int32_t radiusY = static_cast<int32_t>(policy.Config().voxelBrickRadiusY);
    const uint32_t ringCount = std::max(1u, static_cast<uint32_t>(rings.size()));
    const uint32_t maxResidentInterest = std::max<uint32_t>(
        ringCount,
        static_cast<uint32_t>(
            (static_cast<uint64_t>(m_voxelBricks.size()) *
             static_cast<uint64_t>(policy.Config().voxelInterestCapacityPercent)) / 100u));
    std::vector<uint32_t> ringQuotas(ringCount, 1u);
    uint64_t ringWeightSum = 0;
    for (uint32_t ring = 0; ring < ringCount; ++ring) {
        // Keep a near-ring priority, but do not starve the outer rings. Those
        // rings carry the mountain silhouettes, and visible gaps there read as
        // broken world coverage rather than a graceful far LOD transition.
        ringWeightSum += static_cast<uint64_t>((ringCount - ring) + ringCount);
    }
    uint32_t assignedRingQuota = 0;
    for (uint32_t ring = 0; ring < ringCount; ++ring) {
        const uint32_t weight = (ringCount - ring) + ringCount;
        ringQuotas[ring] = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(
                (static_cast<uint64_t>(maxResidentInterest) * weight) /
                std::max<uint64_t>(1ull, ringWeightSum)));
        assignedRingQuota += ringQuotas[ring];
    }
    for (uint32_t ring = 0;
         assignedRingQuota < maxResidentInterest && ring < ringCount;
         ring = (ring + 1u) % ringCount) {
        ++ringQuotas[ring];
        ++assignedRingQuota;
    }
    for (uint32_t ring = ringCount;
         assignedRingQuota > maxResidentInterest && ring > 0u;
         --ring) {
        const uint32_t idx = ring - 1u;
        if (ringQuotas[idx] > 1u) {
            --ringQuotas[idx];
            --assignedRingQuota;
        }
    }

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
        int32_t radiusYBonus = 0;
    };

    const float forwardLen = std::sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    const float invForwardLen = forwardLen > 0.001f ? 1.0f / forwardLen : 0.0f;
    const float forwardNormX = forwardX * invForwardLen;
    const float forwardNormY = forwardY * invForwardLen;
    const float forwardNormZ = forwardZ * invForwardLen;
    const float predictedX = FiniteOr(cameraX + velocityX * predictionSeconds, cameraX);
    const float predictedY = FiniteOr(cameraY + velocityY * predictionSeconds, cameraY);
    const float predictedZ = FiniteOr(cameraZ + velocityZ * predictionSeconds, cameraZ);
    const float velocityLen = std::sqrt(velocityX * velocityX + velocityY * velocityY + velocityZ * velocityZ);
    const bool useMotionLookahead =
        predictionSeconds > 0.0f &&
        velocityLen >= policy.Config().motionLookaheadMinSpeed;
    uint32_t voxelAnchorCount = 0;

    const InterestSignature voxelInterestSignature = BuildVoxelInterestSignature(
        cameraX,
        cameraY,
        cameraZ,
        policy,
        forwardX,
        forwardY,
        forwardZ,
        velocityX,
        velocityY,
        velocityZ,
        predictionSeconds);
    const uint32_t voxelInterestReuseAge =
        frameIndex >= m_lastVoxelInterestBuildFrame
            ? frameIndex - m_lastVoxelInterestBuildFrame
            : 0u;
    const auto staticVoxelInterestSignatureMatches = [](
        InterestSignature a,
        InterestSignature b) {
        a.cameraX = b.cameraX;
        a.cameraY = b.cameraY;
        a.cameraZ = b.cameraZ;
        a.forwardX = b.forwardX;
        a.forwardY = b.forwardY;
        a.forwardZ = b.forwardZ;
        a.velocityX = b.velocityX;
        a.velocityY = b.velocityY;
        a.velocityZ = b.velocityZ;
        a.predictionMillis = b.predictionMillis;
        return a == b;
    };
    const bool voxelInterestReuseConfigCompatible =
        m_lastVoxelInterestSignatureValid &&
        staticVoxelInterestSignatureMatches(
            voxelInterestSignature,
            m_lastVoxelInterestSignature);
    const bool voxelInterestSignatureReuse =
        allowSignatureReuse &&
        policy.Config().voxelInterestSignatureReuse &&
        m_lastVoxelInterestSignatureValid &&
        !m_voxelInterestSet.empty() &&
        voxelInterestReuseConfigCompatible &&
        voxelInterestReuseAge <= policy.Config().voxelInterestSignatureReuseMaxAgeFrames;
    if (voxelInterestSignatureReuse) {
        m_voxelInterestReusedLastFrame = 1u;
        m_voxelInterestReuseAgeLastFrame = voxelInterestReuseAge;

        std::deque<SparseVoxelClipmapCoord> retainedQueue;
        std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> retainedQueuedSet;
        retainedQueue.swap(m_voxelGenerationQueue);
        m_queuedVoxelSet.clear();
        for (const SparseVoxelClipmapCoord& coord : retainedQueue) {
            if (m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
                m_voxelBacklogFirstFrame.erase(coord);
                ++m_prunedVoxelBacklogLastFrame;
                continue;
            }
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                m_voxelBacklogFirstFrame.erase(coord);
                ++m_backlogVoxelResidentSkipLastFrame;
                continue;
            }
            if (retainedQueuedSet.insert(coord).second) {
                m_voxelGenerationQueue.push_back(coord);
            }
        }
        for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
            auto resident = m_voxelSlotByCoord.find(coord);
            if (resident != m_voxelSlotByCoord.end()) {
                m_voxelBricks[resident->second].lastTouchedFrame = frameIndex;
                ++m_reusedInterestedVoxelBricksLastFrame;
                ++m_residentInterestedVoxelBricksLastFrame;
                continue;
            }
            if (retainedQueuedSet.insert(coord).second) {
                m_voxelGenerationQueue.push_back(coord);
                ++m_backlogVoxelCarriedLastFrame;
                if (policy.Config().backlogAwarePump) {
                    m_voxelBacklogFirstFrame.emplace(coord, frameIndex);
                }
            }
        }
        m_queuedVoxelSet = std::move(retainedQueuedSet);
        RefreshStats();
        m_stats.voxelInterestAnchors = 0u;
        return;
    }

    std::deque<SparseVoxelClipmapCoord> previousVoxelQueue;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> previousVoxelInterestSet;
    if (policy.Config().drainReuseDiagnostics) {
        previousVoxelInterestSet = m_voxelInterestSet;
    }
    if (backlogAwarePump) {
        previousVoxelQueue.swap(m_voxelGenerationQueue);
        m_queuedVoxelSet.clear();
    }
    m_voxelInterestSet.clear();
    if (!backlogAwarePump) {
        m_voxelGenerationQueue.clear();
        m_queuedVoxelSet.clear();
        m_voxelBacklogFirstFrame.clear();
    }
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
        const int32_t predictedAnchorRadiusBias = -std::max(1, radiusXz / 2);
        const int32_t predictedAnchorRadiusXz = std::max(1, radiusXz + predictedAnchorRadiusBias);
        const int32_t cameraAnchorCenterX = FloorToGridCoordClamped(cameraX, brickWorldSize, radiusXz + 2);
        const int32_t cameraAnchorCenterY = FloorToGridCoordClamped(cameraY, brickWorldSize, radiusY + 2);
        const int32_t cameraAnchorCenterZ = FloorToGridCoordClamped(cameraZ, brickWorldSize, radiusXz + 2);
        const int32_t predictedAnchorCenterX =
            FloorToGridCoordClamped(predictedX, brickWorldSize, predictedAnchorRadiusXz + 2);
        const int32_t predictedAnchorCenterY = FloorToGridCoordClamped(predictedY, brickWorldSize, radiusY + 2);
        const int32_t predictedAnchorCenterZ =
            FloorToGridCoordClamped(predictedZ, brickWorldSize, predictedAnchorRadiusXz + 2);
        if (predictedAnchorCenterX != cameraAnchorCenterX ||
            predictedAnchorCenterY != cameraAnchorCenterY ||
            predictedAnchorCenterZ != cameraAnchorCenterZ) {
            anchors.push_back({predictedX, predictedY, predictedZ, 900u, predictedAnchorRadiusBias});
        }
        const float forwardLenXzForRingAnchor =
            std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
        if (cameraY <= 384.0f && forwardLenXzForRingAnchor > 0.001f) {
            anchors.push_back({
                cameraX + forwardNormX * rings[ring].endDistance,
                cameraY + forwardNormY * brickWorldSize * 0.5f,
                cameraZ + forwardNormZ * rings[ring].endDistance,
                8u,
                -1,
                2
            });
            if (ring == 0u) {
                const float rightX = forwardNormZ / forwardLenXzForRingAnchor;
                const float rightZ = -forwardNormX / forwardLenXzForRingAnchor;
                constexpr float kFineRingAnchorHalfWidth = 0.52f;
                constexpr float kFineRingSideMidDistance = 0.74f;
                constexpr float kFineRingSideMidHalfWidth = 0.72f;
                for (int32_t fan = -2; fan <= 2; ++fan) {
                    if (fan == 0) {
                        continue;
                    }
                    const float fanScale = static_cast<float>(fan) * 0.5f;
                    anchors.push_back({
                        cameraX + forwardNormX * rings[ring].endDistance +
                            rightX * rings[ring].endDistance * kFineRingAnchorHalfWidth * fanScale,
                        cameraY + forwardNormY * brickWorldSize * 0.5f,
                        cameraZ + forwardNormZ * rings[ring].endDistance +
                            rightZ * rings[ring].endDistance * kFineRingAnchorHalfWidth * fanScale,
                        10u + static_cast<uint32_t>(std::abs(fan)) * 3u,
                        -3,
                        2
                    });
                    const float sideMidDistance = rings[ring].endDistance * kFineRingSideMidDistance;
                    anchors.push_back({
                        cameraX + forwardNormX * sideMidDistance +
                            rightX * sideMidDistance * kFineRingSideMidHalfWidth * fanScale,
                        cameraY + forwardNormY * brickWorldSize * 0.5f,
                        cameraZ + forwardNormZ * sideMidDistance +
                            rightZ * sideMidDistance * kFineRingSideMidHalfWidth * fanScale,
                        6u + static_cast<uint32_t>(std::abs(fan)) * 2u,
                        -4,
                        2
                    });
                }
            }
        }
        enum class VoxelInterestCandidateSource : uint8_t {
            Line,
            AnchorTerrain,
            AnchorFootprint,
            AnchorCamera
        };
        std::vector<VoxelInterestCandidate> candidates;
        std::unordered_map<SparseVoxelClipmapCoord, size_t, SparseVoxelClipmapCoordHash> candidateIndexByCoord;
        const size_t estimatedCandidateCapacity =
            static_cast<size_t>(radiusXz * 2 + 1) *
            static_cast<size_t>(radiusY * 2 + 1) *
            static_cast<size_t>(radiusXz * 2 + 1) *
            anchors.size() *
            2u;
        candidates.reserve(estimatedCandidateCapacity);
        candidateIndexByCoord.reserve(estimatedCandidateCapacity);
        uint32_t ringCandidateAttempts = 0;
        std::unordered_map<uint64_t, float> terrainHeightByBrickCoord;
        terrainHeightByBrickCoord.reserve(
            static_cast<size_t>(radiusXz * 2 + 1) *
            static_cast<size_t>(radiusXz * 2 + 1) *
            std::max<size_t>(anchors.size(), 8u) *
            2u);

        const auto terrainCacheKey = [](int32_t x, int32_t z) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) ^
                static_cast<uint32_t>(z);
        };
        const auto terrainHeightForBrick = [&](
            int32_t brickX,
            int32_t brickZ) {
            const uint64_t key = terrainCacheKey(brickX, brickZ);
            const auto cached = terrainHeightByBrickCoord.find(key);
            if (cached != terrainHeightByBrickCoord.end()) {
                return cached->second;
            }
            const int32_t sampleX = FloorToInt32Clamped(
                (static_cast<double>(brickX) + 0.5) * static_cast<double>(brickWorldSize));
            const int32_t sampleZ = FloorToInt32Clamped(
                (static_cast<double>(brickZ) + 0.5) * static_cast<double>(brickWorldSize));
            const float terrainY = m_terrain.HeightAt(sampleX, sampleZ);
            terrainHeightByBrickCoord.emplace(key, terrainY);
            return terrainY;
        };
        const auto terrainCenterYForBrick = [&](
            int32_t brickX,
            int32_t brickZ,
            int32_t coordMargin) {
            return FloorToGridCoordClamped(
                terrainHeightForBrick(brickX, brickZ),
                brickWorldSize,
                coordMargin);
        };

        const auto noteCandidateAttempt = [&](VoxelInterestCandidateSource source) {
            switch (source) {
            case VoxelInterestCandidateSource::Line:
                ++m_voxelInterestLineCandidateAttemptsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorTerrain:
                ++m_voxelInterestAnchorTerrainCandidateAttemptsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorFootprint:
                ++m_voxelInterestAnchorFootprintCandidateAttemptsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorCamera:
                ++m_voxelInterestAnchorCameraCandidateAttemptsLastFrame;
                break;
            }
        };
        const auto noteCandidateDuplicate = [&](VoxelInterestCandidateSource source) {
            switch (source) {
            case VoxelInterestCandidateSource::Line:
                ++m_voxelInterestLineCandidateDuplicateHitsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorTerrain:
                ++m_voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorFootprint:
                ++m_voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorCamera:
                ++m_voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame;
                break;
            }
        };
        const auto noteCandidateScoreUpdate = [&](VoxelInterestCandidateSource source) {
            switch (source) {
            case VoxelInterestCandidateSource::Line:
                ++m_voxelInterestLineCandidateScoreUpdatesLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorTerrain:
                ++m_voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorFootprint:
                ++m_voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame;
                break;
            case VoxelInterestCandidateSource::AnchorCamera:
                ++m_voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame;
                break;
            }
        };

        const auto addCandidate = [&](
            int32_t x,
            int32_t y,
            int32_t z,
            int32_t dx,
            int32_t dy,
            int32_t dz,
            uint32_t baseScore,
            VoxelInterestCandidateSource source) {
            const SparseVoxelClipmapCoord coord{
                static_cast<int32_t>(ring),
                x,
                y,
                z
            };
            const uint32_t score =
                baseScore +
                static_cast<uint32_t>(dx * dx * 4 + dy * dy * 9 + dz * dz * 4);
            if (voxelInterestDetail) {
                ++ringCandidateAttempts;
                ++m_voxelInterestCandidateAttemptsLastFrame;
                noteCandidateAttempt(source);
            }
            auto existing = candidateIndexByCoord.find(coord);
            if (existing != candidateIndexByCoord.end()) {
                if (voxelInterestDetail) {
                    ++m_voxelInterestCandidateDuplicateHitsLastFrame;
                    noteCandidateDuplicate(source);
                }
                VoxelInterestCandidate& candidate = candidates[existing->second];
                if (score < candidate.distanceScore) {
                    candidate.dx = dx;
                    candidate.dy = dy;
                    candidate.dz = dz;
                    candidate.distanceScore = score;
                    if (voxelInterestDetail) {
                        ++m_voxelInterestCandidateScoreUpdatesLastFrame;
                        noteCandidateScoreUpdate(source);
                    }
                }
                return;
            }
            candidateIndexByCoord.emplace(coord, candidates.size());
            candidates.push_back(VoxelInterestCandidate{coord, dx, dy, dz, score});
        };
        const auto addTerrainCenterlineCandidates = [&](
            float startX,
            float startZ,
            float endX,
            float endZ,
            uint32_t baseScore) {
            const auto lineStart = voxelInterestDetail
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
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
                terrainCenterYs.push_back(terrainCenterYForBrick(
                    lineCoord.x,
                    lineCoord.z,
                    radiusY + 2));
            }
            for (size_t i = 0; i < lineCoords.size(); ++i) {
                addCandidate(
                    lineCoords[i].x,
                    terrainCenterYs[i],
                    lineCoords[i].z,
                    0,
                    0,
                    0,
                    baseScore + static_cast<uint32_t>(i) * 2u,
                    VoxelInterestCandidateSource::Line);
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
                        SaturatingAddInt32(terrainCenterY, dy),
                        lineCoord.z,
                        0,
                        dy,
                        0,
                    baseScore + 10u + static_cast<uint32_t>(i) * 2u,
                    VoxelInterestCandidateSource::Line);
                }
            }
            if (voxelInterestDetail) {
                m_voxelInterestLineMsLastFrame +=
                    ElapsedMs(lineStart, std::chrono::steady_clock::now());
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
        if (cameraY > 384.0f) {
            const float highViewDistance = std::max(
                brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
                rings[ring].endDistance);
            addTerrainCenterlineCandidates(
                cameraX,
                cameraZ,
                cameraX + forwardNormX * highViewDistance,
                cameraZ + forwardNormZ * highViewDistance,
                12u);
            const float highForwardLenXz =
                std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
            if (highForwardLenXz > 0.001f) {
                const float rightX = forwardNormZ / highForwardLenXz;
                const float rightZ = -forwardNormX / highForwardLenXz;
                constexpr float kHighViewFanHalfWidth = 0.42f;
                for (int32_t fan = -1; fan <= 1; fan += 2) {
                    const float fanEndX =
                        cameraX + forwardNormX * highViewDistance +
                        rightX * highViewDistance * kHighViewFanHalfWidth * static_cast<float>(fan);
                    const float fanEndZ =
                        cameraZ + forwardNormZ * highViewDistance +
                        rightZ * highViewDistance * kHighViewFanHalfWidth * static_cast<float>(fan);
                    addTerrainCenterlineCandidates(
                        cameraX,
                        cameraZ,
                        fanEndX,
                        fanEndZ,
                        18u);
                }
            }
        }
        addTerrainCenterlineCandidates(
            cameraX,
            cameraZ,
            cameraX + forwardNormX * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            cameraZ + forwardNormZ * brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz)),
            45u);
        // Add a small horizontal view fan at terrain height. The existing local
        // shell can spend the full quota near the camera, while the centerline
        // alone misses visible side valleys. These fan lines are bounded by the
        // same line-coordinate cap and resident-interest quota as every other
        // mid-voxel target.
        const float forwardLenXz = std::sqrt(forwardNormX * forwardNormX + forwardNormZ * forwardNormZ);
        if (forwardLenXz > 0.001f) {
            const float rightX = forwardNormZ;
            const float rightZ = -forwardNormX;
            const float fanDistance =
                brickWorldSize * static_cast<float>(std::max<int32_t>(2, radiusXz));
            constexpr float kViewFanHalfWidth = 0.72f;
            for (int32_t fan = -2; fan <= 2; ++fan) {
                if (fan == 0) {
                    continue;
                }
                const float fanScale = static_cast<float>(fan) * 0.5f;
                const float fanEndX =
                    cameraX + forwardNormX * fanDistance + rightX * fanDistance * kViewFanHalfWidth * fanScale;
                const float fanEndZ =
                    cameraZ + forwardNormZ * fanDistance + rightZ * fanDistance * kViewFanHalfWidth * fanScale;
                addTerrainCenterlineCandidates(
                    cameraX,
                    cameraZ,
                    fanEndX,
                    fanEndZ,
                    35u + static_cast<uint32_t>(std::abs(fan)) * 8u);
            }
        }
        for (const VoxelInterestAnchor& anchor : anchors) {
            const auto anchorStart = voxelInterestDetail
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const int32_t anchorRadiusXz = std::max(1, radiusXz + anchor.radiusBias);
            const int32_t anchorRadiusY = std::max(1, radiusY + anchor.radiusYBonus);
            const int32_t centerX = FloorToGridCoordClamped(anchor.x, brickWorldSize, anchorRadiusXz + 2);
            const int32_t centerY = FloorToGridCoordClamped(anchor.y, brickWorldSize, anchorRadiusY + 2);
            const int32_t centerZ = FloorToGridCoordClamped(anchor.z, brickWorldSize, anchorRadiusXz + 2);
            ++voxelAnchorCount;
            for (int32_t dz = -anchorRadiusXz; dz <= anchorRadiusXz; ++dz) {
                for (int32_t dx = -anchorRadiusXz; dx <= anchorRadiusXz; ++dx) {
                    const int32_t brickX = SaturatingAddInt32(centerX, dx);
                    const int32_t brickZ = SaturatingAddInt32(centerZ, dz);
                    const float terrainY = terrainHeightForBrick(brickX, brickZ);
                    const int32_t terrainCenterY = FloorToGridCoordClamped(terrainY, brickWorldSize, anchorRadiusY + 2);
                    int32_t footprintCenterY = terrainCenterY;
                    if (anchor.radiusYBonus > 0) {
                        float maxFootprintY = terrainY;
                        for (int32_t oz = -1; oz <= 1; ++oz) {
                            for (int32_t ox = -1; ox <= 1; ++ox) {
                                if (ox == 0 && oz == 0) {
                                    continue;
                                }
                                maxFootprintY = std::max(
                                    maxFootprintY,
                                    terrainHeightForBrick(
                                        SaturatingAddInt32(brickX, ox),
                                        SaturatingAddInt32(brickZ, oz)));
                            }
                        }
                        footprintCenterY = FloorToGridCoordClamped(
                            maxFootprintY,
                            brickWorldSize,
                            anchorRadiusY + 2);
                    }
                    const auto addVerticalBand = [&](
                        int32_t verticalCenterY,
                        uint32_t scoreBias,
                        VoxelInterestCandidateSource source) {
                        for (int32_t dy = -anchorRadiusY; dy <= anchorRadiusY; ++dy) {
                            // The mid voxel clipmap is primarily distant terrain
                            // context. Anchor the vertical interest around generated
                            // terrain height, not camera height, so high/flying cameras
                            // stream terrain below them instead of empty air.
                            addCandidate(
                                brickX,
                                SaturatingAddInt32(verticalCenterY, dy),
                                brickZ,
                                dx,
                                dy,
                                dz,
                                anchor.baseScore + scoreBias,
                                source);
                        }
                    };
                    addVerticalBand(
                        terrainCenterY,
                        0u,
                        VoxelInterestCandidateSource::AnchorTerrain);
                    if (footprintCenterY != terrainCenterY) {
                        addVerticalBand(
                            footprintCenterY,
                            6u,
                            VoxelInterestCandidateSource::AnchorFootprint);
                    }
                    const auto yInsideVerticalBand = [&](int32_t y, int32_t verticalCenterY) {
                        return y >= SaturatingAddInt32(verticalCenterY, -anchorRadiusY) &&
                            y <= SaturatingAddInt32(verticalCenterY, anchorRadiusY);
                    };
                    for (int32_t dy = -anchorRadiusY; dy <= anchorRadiusY; ++dy) {
                        const int32_t cameraBandY = SaturatingAddInt32(centerY, dy);
                        if (
                            yInsideVerticalBand(cameraBandY, terrainCenterY) ||
                            (footprintCenterY != terrainCenterY &&
                                yInsideVerticalBand(cameraBandY, footprintCenterY))) {
                            continue;
                        }
                        // Keep a lower-priority camera-height band for cases where
                        // the camera is inside tall/vertical formations.
                        addCandidate(
                            brickX,
                            cameraBandY,
                            brickZ,
                            dx,
                            dy,
                            dz,
                            anchor.baseScore + 5000u,
                            VoxelInterestCandidateSource::AnchorCamera);
                    }
                }
            }
            if (voxelInterestDetail) {
                m_voxelInterestAnchorMsLastFrame +=
                    ElapsedMs(anchorStart, std::chrono::steady_clock::now());
            }
        }
        const auto compareCandidate =
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
        };

        const uint32_t ringQuota = ringQuotas[ring];
        const uint32_t emitCount = std::min<uint32_t>(ringQuota, static_cast<uint32_t>(candidates.size()));
        const auto sortEmitStart = voxelInterestDetail
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (voxelInterestDetail) {
            const uint32_t remainingCandidateCount =
                std::numeric_limits<uint32_t>::max() - m_voxelInterestCandidatesLastFrame;
            m_voxelInterestCandidatesLastFrame += static_cast<uint32_t>(
                std::min<size_t>(candidates.size(), remainingCandidateCount));
            m_voxelInterestEmittedLastFrame += emitCount;
        }
        if (emitCount < candidates.size()) {
            std::nth_element(
                candidates.begin(),
                candidates.begin() + emitCount,
                candidates.end(),
                compareCandidate);
            std::sort(
                candidates.begin(),
                candidates.begin() + emitCount,
                compareCandidate);
        } else {
            std::sort(candidates.begin(), candidates.end(), compareCandidate);
        }
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
                ++m_backlogVoxelEnqueuedLastFrame;
                if (backlogAwarePump) {
                    m_voxelBacklogFirstFrame.emplace(coord, frameIndex);
                }
            }
        }
        if (voxelInterestDetail) {
            m_voxelInterestSortEmitMsLastFrame +=
                ElapsedMs(sortEmitStart, std::chrono::steady_clock::now());
        }
        if (voxelInterestDetail) {
            m_voxelInterestCandidateMaxRingUniqueLastFrame = std::max(
                m_voxelInterestCandidateMaxRingUniqueLastFrame,
                static_cast<uint32_t>(std::min<size_t>(
                    candidates.size(),
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max()))));
            m_voxelInterestCandidateMaxRingAttemptsLastFrame = std::max(
                m_voxelInterestCandidateMaxRingAttemptsLastFrame,
                ringCandidateAttempts);
        }
    }

    if (backlogAwarePump) {
        const auto backlogStart = voxelInterestDetail
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        for (const SparseVoxelClipmapCoord& coord : previousVoxelQueue) {
            if (m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end()) {
                m_voxelBacklogFirstFrame.erase(coord);
                continue;
            }
            if (m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) {
                m_voxelBacklogFirstFrame.erase(coord);
                ++m_prunedVoxelBacklogLastFrame;
                continue;
            }
            if (m_queuedVoxelSet.insert(coord).second) {
                m_voxelGenerationQueue.push_back(coord);
                ++m_backlogVoxelCarriedLastFrame;
                m_voxelBacklogFirstFrame.emplace(coord, frameIndex);
            }
        }
        if (voxelInterestDetail) {
            m_voxelInterestBacklogMsLastFrame +=
                ElapsedMs(backlogStart, std::chrono::steady_clock::now());
        }
    }

    if (policy.Config().drainReuseDiagnostics) {
        const auto diagnosticsStart = voxelInterestDetail
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
            const bool wasPreviouslyInterested = previousVoxelInterestSet.erase(coord) != 0u;
            const bool resident = m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end();
            if (!wasPreviouslyInterested) {
                ++m_newlyInterestedVoxelBricksLastFrame;
            } else if (resident) {
                ++m_reusedInterestedVoxelBricksLastFrame;
            }
            if (resident) {
                ++m_residentInterestedVoxelBricksLastFrame;
            }
        }
        m_noLongerInterestedVoxelBricksLastFrame += static_cast<uint32_t>(std::min<size_t>(
            previousVoxelInterestSet.size(),
            static_cast<size_t>(
                std::numeric_limits<uint32_t>::max() - m_noLongerInterestedVoxelBricksLastFrame)));
        if (voxelInterestDetail) {
            m_voxelInterestDiagnosticsMsLastFrame +=
                ElapsedMs(diagnosticsStart, std::chrono::steady_clock::now());
        }
    }

    RefreshStats();
    m_stats.voxelInterestAnchors = voxelAnchorCount;
    if (allowSignatureReuse) {
        m_lastVoxelInterestSignature = voxelInterestSignature;
        m_lastVoxelInterestSignatureValid = true;
        m_lastVoxelInterestBuildFrame = frameIndex;
    }
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

    VoxelBrickPayload& brick = m_voxelBricks[bestSlot];
    m_voxelSlotByCoord.erase(brick.coord);
    brick.coord = coord;
    brick.slot = bestSlot;
    brick.lastTouchedFrame = frameIndex;
    brick.cellSize = 16.0f;
    brick.originX = 0;
    brick.originY = 0;
    brick.originZ = 0;
    brick.nonAirSamples = 0;
    brick.surfaceSamples = 0;
    ++m_dirtySerial;
    ++m_voxelDirtySerial;
    MarkVoxelSlotDirty(bestSlot);
    return bestSlot;
}

uint32_t SparseClipmapTileCache::AllocateVoxelSlotForMinRing(
    const SparseVoxelClipmapCoord& coord,
    uint32_t frameIndex,
    uint32_t minEvictRing)
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
        if (brick.coord.ring < 0 || static_cast<uint32_t>(brick.coord.ring) < minEvictRing) {
            continue;
        }
        if (brick.lastTouchedFrame < oldestFrame) {
            oldestFrame = brick.lastTouchedFrame;
            bestSlot = slot;
        }
    }
    if (bestSlot == UINT32_MAX) {
        return UINT32_MAX;
    }

    VoxelBrickPayload& brick = m_voxelBricks[bestSlot];
    m_voxelSlotByCoord.erase(brick.coord);
    brick.coord = coord;
    brick.slot = bestSlot;
    brick.lastTouchedFrame = frameIndex;
    brick.cellSize = 16.0f;
    brick.originX = 0;
    brick.originY = 0;
    brick.originZ = 0;
    brick.nonAirSamples = 0;
    brick.surfaceSamples = 0;
    ++m_dirtySerial;
    ++m_voxelDirtySerial;
    MarkVoxelSlotDirty(bestSlot);
    return bestSlot;
}

void SparseClipmapTileCache::GenerateVoxelBrick(
    uint32_t slot,
    const SparseClipmapPolicy& policy,
    std::unordered_map<uint64_t, VoxelColumnSample>* externalColumnCache,
    VoxelColumnCacheCounters* externalColumnCacheCounters)
{
    if (slot >= m_voxelBricks.size()) {
        return;
    }
    GenerateVoxelBrickPayload(
        m_voxelBricks[slot],
        policy,
        externalColumnCache,
        externalColumnCacheCounters);
}

void SparseClipmapTileCache::GenerateVoxelBrickPayload(
    VoxelBrickPayload& brick,
    const SparseClipmapPolicy& policy,
    std::unordered_map<uint64_t, VoxelColumnSample>* externalColumnCache,
    VoxelColumnCacheCounters* externalColumnCacheCounters)
{

    const auto rings = policy.BuildRings();
    if (brick.coord.ring < 0 || static_cast<uint32_t>(brick.coord.ring) >= rings.size()) {
        return;
    }

    const SparseClipmapRing& ring = rings[static_cast<uint32_t>(brick.coord.ring)];
    const int32_t brickWorldSize = std::max(1, RoundToInt32Clamped(
        static_cast<double>(ring.cellSize) * static_cast<double>(SPARSE_BRICK_SIZE)));
    brick.cellSize = ring.cellSize;
    brick.originX = FloorToInt32Clamped(
        static_cast<double>(brick.coord.x) * static_cast<double>(brickWorldSize));
    brick.originY = FloorToInt32Clamped(
        static_cast<double>(brick.coord.y) * static_cast<double>(brickWorldSize));
    brick.originZ = FloorToInt32Clamped(
        static_cast<double>(brick.coord.z) * static_cast<double>(brickWorldSize));
    brick.voxels.resize(SPARSE_BRICK_VOXEL_COUNT);
    brick.nonAirSamples = 0;
    brick.surfaceSamples = 0;
    const int32_t sampleStep = std::max(1, RoundToInt32Clamped(ring.cellSize));

    constexpr uint32_t airVoxel = Utils::Material::Air;
    if (brick.originY > TERRAIN_MAX_Y && brick.originY > SEA_LEVEL_Y) {
        std::fill(brick.voxels.begin(), brick.voxels.end(), airVoxel);
        return;
    }
    constexpr uint32_t haloSide = SPARSE_BRICK_SIZE + 2u;

    using ColumnSample = VoxelColumnSample;
    struct EditedCellSummary {
        uint32_t solidVoxel = Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
        uint32_t solidCount = 0;
        uint32_t airCount = 0;
        bool foundSolid = false;
        bool foundAir = false;
    };
    std::unordered_map<uint32_t, EditedCellSummary> editedCellSummaries;
    constexpr int32_t editHaloSide = SPARSE_BRICK_SIZE + 2;
    auto editCellIndex = [&](int32_t worldCoord, int32_t origin) {
        const double cellSize = std::max(1.0, static_cast<double>(ring.cellSize));
        const int32_t raw = FloorToInt32Clamped(
            (static_cast<double>(worldCoord) - static_cast<double>(origin)) / cellSize) + 1;
        return std::clamp(raw, 0, editHaloSide - 1);
    };
    auto editCellKey = [](int32_t x, int32_t y, int32_t z) {
        return static_cast<uint32_t>(
            x +
            y * editHaloSide +
            z * editHaloSide * editHaloSide);
    };
    auto columnKey = [](int32_t worldX, int32_t worldZ) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(worldX)) << 32u) |
            static_cast<uint32_t>(worldZ);
    };
    std::unordered_map<uint64_t, ColumnSample> localColumnCache;
    const bool useGlobalSharedColumnCache =
        policy.Config().sharedVoxelColumnCache && externalColumnCache == nullptr;
    const bool useExternalColumnCache = externalColumnCache != nullptr;
    auto& columnCache = useGlobalSharedColumnCache
        ? m_sharedVoxelColumnCache
        : (useExternalColumnCache ? *externalColumnCache : localColumnCache);
    const uint32_t coarseFootprintSamplesPerCell = ring.cellSize > 1.5f ? 25u : 0u;
    const size_t localColumnReserve =
        static_cast<size_t>(SPARSE_BRICK_SIZE + 2u) *
            static_cast<size_t>(SPARSE_BRICK_SIZE + 2u) +
        static_cast<size_t>(SPARSE_BRICK_SIZE) *
            static_cast<size_t>(SPARSE_BRICK_SIZE) *
            static_cast<size_t>(9u + coarseFootprintSamplesPerCell);
    if (!useGlobalSharedColumnCache && !useExternalColumnCache) {
        columnCache.reserve(localColumnReserve);
    } else if (columnCache.bucket_count() == 0u) {
        columnCache.reserve(localColumnReserve);
    }
    auto buildColumn = [this,
                        &columnCache,
                        &columnKey,
                        useGlobalSharedColumnCache,
                        useExternalColumnCache,
                        externalColumnCacheCounters](int32_t worldX, int32_t worldZ) {
        const uint64_t key = columnKey(worldX, worldZ);
        auto cached = columnCache.find(key);
        if (cached != columnCache.end()) {
            if (useGlobalSharedColumnCache) {
                ++m_sharedVoxelColumnHeightHitsLastFrame;
            } else if (useExternalColumnCache && externalColumnCacheCounters) {
                ++externalColumnCacheCounters->heightHits;
            }
            return cached->second;
        }
        if (useGlobalSharedColumnCache) {
            ++m_sharedVoxelColumnHeightMissesLastFrame;
        } else if (useExternalColumnCache && externalColumnCacheCounters) {
            ++externalColumnCacheCounters->heightMisses;
        }
        ColumnSample column;
        column.worldX = worldX;
        column.worldZ = worldZ;
        column.height = m_terrain.HeightAt(worldX, worldZ);
        columnCache.emplace(key, column);
        return column;
    };
    const bool useDirectFootprintColumns =
        policy.Config().directVoxelFootprintColumns &&
        !useGlobalSharedColumnCache &&
        !useExternalColumnCache;
    auto buildDirectColumn = [this](int32_t worldX, int32_t worldZ) {
        ColumnSample column;
        column.worldX = worldX;
        column.worldZ = worldZ;
        column.height = m_terrain.HeightAt(worldX, worldZ);
        return column;
    };
    auto ensureColumnRelief = [this,
                               &columnCache,
                               &columnKey,
                               useGlobalSharedColumnCache,
                               useExternalColumnCache,
                               externalColumnCacheCounters](ColumnSample& column) {
        if (column.reliefValid) {
            return;
        }
        const uint64_t key = columnKey(column.worldX, column.worldZ);
        auto cached = columnCache.find(key);
        if (cached != columnCache.end() && cached->second.reliefValid) {
            if (useGlobalSharedColumnCache) {
                ++m_sharedVoxelColumnReliefHitsLastFrame;
            } else if (useExternalColumnCache && externalColumnCacheCounters) {
                ++externalColumnCacheCounters->reliefHits;
            }
            column.relief = cached->second.relief;
            column.reliefValid = true;
            return;
        }
        if (useGlobalSharedColumnCache) {
            ++m_sharedVoxelColumnReliefMissesLastFrame;
        } else if (useExternalColumnCache && externalColumnCacheCounters) {
            ++externalColumnCacheCounters->reliefMisses;
        }

        column.relief =
            m_terrain.SurfaceReliefAtWithCenter(column.worldX, column.worldZ, column.height, 4);
        column.reliefValid = true;
        if (cached != columnCache.end()) {
            cached->second = column;
        } else {
            columnCache.emplace(key, column);
        }
    };
    auto tryEditedCellVoxel = [&, this](
        int32_t minWorldX,
        int32_t maxWorldX,
        int32_t minWorldY,
        int32_t maxWorldY,
        int32_t minWorldZ,
        int32_t maxWorldZ,
        uint32_t* outVoxel) {
        if (!outVoxel || editedCellSummaries.empty()) {
            return false;
        }
        if (minWorldX > maxWorldX) {
            std::swap(minWorldX, maxWorldX);
        }
        if (minWorldY > maxWorldY) {
            std::swap(minWorldY, maxWorldY);
        }
        if (minWorldZ > maxWorldZ) {
            std::swap(minWorldZ, maxWorldZ);
        }

        const int32_t minCellX = editCellIndex(minWorldX, brick.originX);
        const int32_t maxCellX = editCellIndex(maxWorldX, brick.originX);
        const int32_t minCellY = editCellIndex(minWorldY, brick.originY);
        const int32_t maxCellY = editCellIndex(maxWorldY, brick.originY);
        const int32_t minCellZ = editCellIndex(minWorldZ, brick.originZ);
        const int32_t maxCellZ = editCellIndex(maxWorldZ, brick.originZ);
        bool foundAir = false;
        bool foundSolid = false;
        uint32_t airCount = 0;
        uint32_t solidCount = 0;
        uint32_t solidVoxel = Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
        for (int32_t z = minCellZ; z <= maxCellZ; ++z) {
            for (int32_t y = minCellY; y <= maxCellY; ++y) {
                for (int32_t x = minCellX; x <= maxCellX; ++x) {
                    auto summaryIt = editedCellSummaries.find(editCellKey(x, y, z));
                    if (summaryIt == editedCellSummaries.end()) {
                        continue;
                    }
                    const EditedCellSummary& summary = summaryIt->second;
                    airCount += summary.airCount;
                    solidCount += summary.solidCount;
                    if (summary.foundSolid) {
                        solidVoxel = summary.solidVoxel;
                        foundSolid = true;
                        break;
                    }
                    foundAir = foundAir || summary.foundAir;
                }
                if (foundSolid) {
                    break;
                }
            }
            if (foundSolid) {
                break;
            }
        }

        if (foundSolid) {
            *outVoxel = solidVoxel;
            return true;
        }
        // Coarse mid-clipmap cells cover many authoritative voxels. A single
        // erased voxel must not collapse the entire generated cell to air; the
        // exact sparse page/surface layer owns that local edit. Solid edits are
        // still allowed to punch through so additions remain visible. Once a
        // brush produces a real cluster of AIR edits in a coarse cell, the
        // generated mid context should stop showing stale procedural terrain.
        const uint32_t coarseAirClusterThreshold = std::max<uint32_t>(
            8u,
            static_cast<uint32_t>(std::ceil(std::max(1.0f, ring.cellSize))));
        const bool localAirEdit = foundAir && ring.cellSize <= 1.5f;
        const bool clusteredCoarseAirEdit =
            foundAir &&
            ring.cellSize > 1.5f &&
            solidCount == 0u &&
            airCount >= coarseAirClusterThreshold;
        if (localAirEdit || clusteredCoarseAirEdit) {
            *outVoxel = Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
            return true;
        }
        return false;
    };
    auto addEditedVoxelToCell = [&](int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
        const uint32_t key = editCellKey(
            editCellIndex(worldX, brick.originX),
            editCellIndex(worldY, brick.originY),
            editCellIndex(worldZ, brick.originZ));
        EditedCellSummary& summary = editedCellSummaries[key];
        if (Utils::UnpackMaterial(packedVoxel) == Utils::Material::Air) {
            summary.foundAir = true;
            ++summary.airCount;
        } else {
            summary.solidVoxel = packedVoxel;
            summary.foundSolid = true;
            ++summary.solidCount;
        }
    };
    const bool hasEditedOverlays = m_edits && m_edits->EditedBrickCount() != 0u;
    auto sampleColumnVoxel = [this, &ensureColumnRelief](
        ColumnSample& column,
        int32_t worldY) {
        if (worldY > TERRAIN_MIN_Y + 2 &&
            static_cast<float>(worldY) <= column.height &&
            !column.reliefValid) {
            ensureColumnRelief(column);
        }
        return m_terrain.SampleGeneratedVoxelWithColumn(
            column.worldX,
            worldY,
            column.worldZ,
            column.height,
            column.relief);
    };
    auto classifyColumnCellMaterial = [&tryEditedCellVoxel](
        const ColumnSample& column,
        int32_t minWorldX,
        int32_t maxWorldX,
        int32_t minWorldY,
        int32_t maxWorldY,
        int32_t minWorldZ,
        int32_t maxWorldZ) {
        uint32_t editedVoxel = 0;
        if (tryEditedCellVoxel(
                minWorldX,
                maxWorldX,
                minWorldY,
                maxWorldY,
                minWorldZ,
                maxWorldZ,
                &editedVoxel)) {
            return Utils::UnpackMaterial(editedVoxel);
        }
        if (maxWorldY <= TERRAIN_MIN_Y + 2) {
            return Utils::Material::Bedrock;
        }
        const int32_t terrainTopY = FloorToInt32Clamped(column.height);
        if (column.height < static_cast<float>(SEA_LEVEL_Y) &&
            minWorldY <= SEA_LEVEL_Y &&
            maxWorldY > terrainTopY) {
            return Utils::Material::Water;
        }
        if (static_cast<float>(minWorldY) <= column.height) {
            return Utils::Material::Stone;
        }
        return Utils::Material::Air;
    };
    auto sampleColumnCellVoxel = [this, &sampleColumnVoxel, &tryEditedCellVoxel](
        ColumnSample& column,
        int32_t minWorldX,
        int32_t maxWorldX,
        int32_t minWorldY,
        int32_t maxWorldY,
        int32_t minWorldZ,
        int32_t maxWorldZ,
        int32_t preferredWorldY) {
        uint32_t editedVoxel = 0;
        if (tryEditedCellVoxel(
                minWorldX,
                maxWorldX,
                minWorldY,
                maxWorldY,
                minWorldZ,
                maxWorldZ,
                &editedVoxel)) {
            return editedVoxel;
        }
        if (maxWorldY <= TERRAIN_MIN_Y + 2) {
            const int32_t sampleY = std::clamp(
                preferredWorldY,
                minWorldY,
                maxWorldY);
            return m_terrain.SampleGeneratedVoxelWithColumn(
                column.worldX,
                sampleY,
                column.worldZ,
                column.height,
                column.relief);
        }

        const int32_t terrainTopY = FloorToInt32Clamped(column.height);
        const bool submergedColumn = column.height < static_cast<float>(SEA_LEVEL_Y);
        const bool overlapsWater =
            submergedColumn &&
            minWorldY <= SEA_LEVEL_Y &&
            maxWorldY > terrainTopY;
        if (overlapsWater) {
            const int32_t waterMinY = std::max(minWorldY, SaturatingAddInt32(terrainTopY, 1));
            const int32_t waterMaxY = std::min(maxWorldY, SEA_LEVEL_Y);
            const int32_t sampleY = std::clamp(preferredWorldY, waterMinY, waterMaxY);
            return m_terrain.SampleGeneratedVoxelWithColumn(
                column.worldX,
                sampleY,
                column.worldZ,
                column.height,
                column.relief);
        }

        if (static_cast<float>(minWorldY) <= column.height) {
            const int32_t solidMaxY = std::min(maxWorldY, terrainTopY);
            const bool cellContainsTerrainTop = maxWorldY >= terrainTopY;
            const int32_t representativeY = cellContainsTerrainTop
                ? terrainTopY
                : preferredWorldY;
            const int32_t sampleY = std::clamp(representativeY, minWorldY, solidMaxY);
            return sampleColumnVoxel(column, sampleY);
        }

        return Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
    };
    auto isSurfaceNeighbor = [](uint8_t material, uint8_t neighborMaterial) {
        if (neighborMaterial == Utils::Material::Air) {
            return true;
        }
        if (material == Utils::Material::Water && neighborMaterial != Utils::Material::Water) {
            return true;
        }
        return false;
    };

    int32_t worldXByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldXMinByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldXMaxByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldYByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldYMinByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldYMaxByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldZByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldZMinByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldZMaxByLocal[SPARSE_BRICK_SIZE] = {};
    for (uint8_t i = 0; i < SPARSE_BRICK_SIZE; ++i) {
        const int32_t sampleOffset = RoundToInt32Clamped(
            (static_cast<double>(i) + 0.5) * static_cast<double>(ring.cellSize));
        worldXByLocal[i] = SaturatingAddInt32(brick.originX, sampleOffset);
        worldYByLocal[i] = SaturatingAddInt32(brick.originY, sampleOffset);
        worldZByLocal[i] = SaturatingAddInt32(brick.originZ, sampleOffset);
        const int32_t cellMinOffset = FloorToInt32Clamped(
            static_cast<double>(i) * static_cast<double>(ring.cellSize));
        const int32_t cellMaxOffset = SaturatingAddInt32(
            CeilToInt32Clamped(static_cast<double>(i + 1u) * static_cast<double>(ring.cellSize)),
            -1);
        worldXMinByLocal[i] = SaturatingAddInt32(brick.originX, cellMinOffset);
        worldXMaxByLocal[i] = std::max(
            worldXMinByLocal[i],
            SaturatingAddInt32(brick.originX, cellMaxOffset));
        worldYMinByLocal[i] = SaturatingAddInt32(brick.originY, cellMinOffset);
        worldYMaxByLocal[i] = std::max(
            worldYMinByLocal[i],
            SaturatingAddInt32(brick.originY, cellMaxOffset));
        worldZMinByLocal[i] = SaturatingAddInt32(brick.originZ, cellMinOffset);
        worldZMaxByLocal[i] = std::max(
            worldZMinByLocal[i],
            SaturatingAddInt32(brick.originZ, cellMaxOffset));
    }

    int32_t worldXByHalo[haloSide] = {};
    int32_t worldXMinByHalo[haloSide] = {};
    int32_t worldXMaxByHalo[haloSide] = {};
    int32_t worldZByHalo[haloSide] = {};
    int32_t worldZMinByHalo[haloSide] = {};
    int32_t worldZMaxByHalo[haloSide] = {};
    worldXByHalo[0] = SaturatingAddInt32(worldXByLocal[0], -sampleStep);
    worldZByHalo[0] = SaturatingAddInt32(worldZByLocal[0], -sampleStep);
    worldXMinByHalo[0] = SaturatingAddInt32(worldXMinByLocal[0], -sampleStep);
    worldXMaxByHalo[0] = SaturatingAddInt32(worldXMinByLocal[0], -1);
    worldZMinByHalo[0] = SaturatingAddInt32(worldZMinByLocal[0], -sampleStep);
    worldZMaxByHalo[0] = SaturatingAddInt32(worldZMinByLocal[0], -1);
    for (uint8_t i = 0; i < SPARSE_BRICK_SIZE; ++i) {
        worldXByHalo[i + 1u] = worldXByLocal[i];
        worldZByHalo[i + 1u] = worldZByLocal[i];
        worldXMinByHalo[i + 1u] = worldXMinByLocal[i];
        worldXMaxByHalo[i + 1u] = worldXMaxByLocal[i];
        worldZMinByHalo[i + 1u] = worldZMinByLocal[i];
        worldZMaxByHalo[i + 1u] = worldZMaxByLocal[i];
    }
    worldXByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldXByLocal[SPARSE_BRICK_SIZE - 1u], sampleStep);
    worldZByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldZByLocal[SPARSE_BRICK_SIZE - 1u], sampleStep);
    worldXMinByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldXMaxByLocal[SPARSE_BRICK_SIZE - 1u], 1);
    worldXMaxByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldXMaxByLocal[SPARSE_BRICK_SIZE - 1u], sampleStep);
    worldZMinByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldZMaxByLocal[SPARSE_BRICK_SIZE - 1u], 1);
    worldZMaxByHalo[haloSide - 1u] =
        SaturatingAddInt32(worldZMaxByLocal[SPARSE_BRICK_SIZE - 1u], sampleStep);

    auto buildEditedCellSummaries = [&]() {
        if (!hasEditedOverlays) {
            return;
        }
        const int32_t minEditX = worldXMinByHalo[0];
        const int32_t maxEditX = worldXMaxByHalo[haloSide - 1u];
        const int32_t minEditY = SaturatingAddInt32(worldYMinByLocal[0], -sampleStep);
        const int32_t maxEditY = SaturatingAddInt32(worldYMaxByLocal[SPARSE_BRICK_SIZE - 1u], sampleStep);
        const int32_t minEditZ = worldZMinByHalo[0];
        const int32_t maxEditZ = worldZMaxByHalo[haloSide - 1u];

        m_edits->ForEachOverlay([&](const BrickEditOverlay& overlay) {
            int32_t overlayMinX = 0;
            int32_t overlayMinY = 0;
            int32_t overlayMinZ = 0;
            int32_t overlayMaxX = 0;
            int32_t overlayMaxY = 0;
            int32_t overlayMaxZ = 0;
            if (!TryWorldVoxelFromBrickLocal(overlay.coord.x, 0, &overlayMinX) ||
                !TryWorldVoxelFromBrickLocal(overlay.coord.y, 0, &overlayMinY) ||
                !TryWorldVoxelFromBrickLocal(overlay.coord.z, 0, &overlayMinZ) ||
                !TryWorldVoxelFromBrickLocal(overlay.coord.x, SPARSE_BRICK_SIZE - 1u, &overlayMaxX) ||
                !TryWorldVoxelFromBrickLocal(overlay.coord.y, SPARSE_BRICK_SIZE - 1u, &overlayMaxY) ||
                !TryWorldVoxelFromBrickLocal(overlay.coord.z, SPARSE_BRICK_SIZE - 1u, &overlayMaxZ)) {
                return;
            }
            const bool overlaps =
                overlayMinX <= maxEditX && overlayMaxX >= minEditX &&
                overlayMinY <= maxEditY && overlayMaxY >= minEditY &&
                overlayMinZ <= maxEditZ && overlayMaxZ >= minEditZ;
            if (!overlaps) {
                return;
            }

            for (const auto& [localIndex, packedVoxel] : overlay.voxels) {
                const LocalVoxelCoord local = LocalVoxelFromIndex(localIndex);
                int32_t worldX = 0;
                int32_t worldY = 0;
                int32_t worldZ = 0;
                if (!TryWorldVoxelFromBrickLocal(overlay.coord.x, local.x, &worldX) ||
                    !TryWorldVoxelFromBrickLocal(overlay.coord.y, local.y, &worldY) ||
                    !TryWorldVoxelFromBrickLocal(overlay.coord.z, local.z, &worldZ)) {
                    continue;
                }
                if (worldX < minEditX || worldX > maxEditX ||
                    worldY < minEditY || worldY > maxEditY ||
                    worldZ < minEditZ || worldZ > maxEditZ) {
                    continue;
                }
                addEditedVoxelToCell(worldX, worldY, worldZ, packedVoxel);
            }
        });
    };
    buildEditedCellSummaries();

    ColumnSample columns[haloSide][haloSide] = {};
    float minColumnHeight = static_cast<float>(TERRAIN_MAX_Y);
    float maxColumnHeight = static_cast<float>(TERRAIN_MIN_Y);
    for (uint32_t z = 0; z < haloSide; ++z) {
        for (uint32_t x = 0; x < haloSide; ++x) {
            columns[z][x] = buildColumn(worldXByHalo[x], worldZByHalo[z]);
            minColumnHeight = std::min(minColumnHeight, columns[z][x].height);
            maxColumnHeight = std::max(maxColumnHeight, columns[z][x].height);
        }
    }

    const int32_t minWorldY = worldYByLocal[0];
    if (editedCellSummaries.empty() &&
        minWorldY > SEA_LEVEL_Y &&
        static_cast<float>(minWorldY) > maxColumnHeight) {
        std::fill(brick.voxels.begin(), brick.voxels.end(), airVoxel);
        return;
    }

    const int32_t surfaceBandDepth = std::max(sampleStep * 2, 2);
    const bool hasEditedCells = !editedCellSummaries.empty();
    const int32_t maxWorldY = worldYMaxByLocal[SPARSE_BRICK_SIZE - 1u];
    if (!hasEditedCells &&
        maxWorldY > TERRAIN_MIN_Y + 2 &&
        static_cast<float>(SaturatingAddInt32(maxWorldY, surfaceBandDepth)) < minColumnHeight) {
        const uint32_t stoneVoxel =
            Utils::PackVoxel(Utils::Material::Stone, 0, 0, Utils::StateFlags::IsStatic);
        std::fill(brick.voxels.begin(), brick.voxels.end(), stoneVoxel);
        brick.nonAirSamples = SPARSE_BRICK_VOXEL_COUNT;
        return;
    }

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
            ColumnSample& centerColumn = columns[z + 1u][x + 1u];
            ColumnSample& posXColumn = columns[z + 1u][x + 2u];
            ColumnSample& negXColumn = columns[z + 1u][x];
            ColumnSample& posZColumn = columns[z + 2u][x + 1u];
            ColumnSample& negZColumn = columns[z][x + 1u];
            ColumnSample cellFootprintColumns[25] = {};
            const bool sampleCoarseCellCorners = ring.cellSize > 1.5f;
            if (sampleCoarseCellCorners) {
                uint32_t footprintIndex = 0u;
                for (uint32_t sampleZIndex = 0u; sampleZIndex < 5u; ++sampleZIndex) {
                    const int32_t sampleZ = worldZMinByLocal[z] +
                        static_cast<int32_t>(
                            (static_cast<int64_t>(worldZMaxByLocal[z] - worldZMinByLocal[z]) *
                             static_cast<int64_t>(sampleZIndex) + 2) / 4);
                    for (uint32_t sampleXIndex = 0u; sampleXIndex < 5u; ++sampleXIndex) {
                        const int32_t sampleX = worldXMinByLocal[x] +
                            static_cast<int32_t>(
                                (static_cast<int64_t>(worldXMaxByLocal[x] - worldXMinByLocal[x]) *
                                 static_cast<int64_t>(sampleXIndex) + 2) / 4);
                        cellFootprintColumns[footprintIndex++] = useDirectFootprintColumns
                            ? buildDirectColumn(sampleX, sampleZ)
                            : buildColumn(sampleX, sampleZ);
                    }
                }
            }
            ColumnSample* footprintColumns[] = {
                &centerColumn,
                &posXColumn,
                &negXColumn,
                &posZColumn,
                &negZColumn,
                &columns[z][x],
                &columns[z][x + 2u],
                &columns[z + 2u][x],
                &columns[z + 2u][x + 2u],
                sampleCoarseCellCorners ? &cellFootprintColumns[0] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[1] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[2] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[3] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[4] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[5] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[6] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[7] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[8] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[9] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[10] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[11] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[12] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[13] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[14] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[15] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[16] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[17] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[18] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[19] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[20] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[21] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[22] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[23] : nullptr,
                sampleCoarseCellCorners ? &cellFootprintColumns[24] : nullptr,
            };
            const float minHorizontalColumnHeight = std::min(
                std::min(std::min(centerColumn.height, posXColumn.height), negXColumn.height),
                std::min(posZColumn.height, negZColumn.height));
            ColumnSample* maxFootprintColumn = &centerColumn;
            float maxFootprintColumnHeight = centerColumn.height;
            for (ColumnSample* candidate : footprintColumns) {
                if (candidate && candidate->height > maxFootprintColumnHeight) {
                    maxFootprintColumn = candidate;
                    maxFootprintColumnHeight = candidate->height;
                }
            }

            for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
                const int32_t worldY = worldYByLocal[y];
                const int32_t cellMinWorldY = worldYMinByLocal[y];
                const int32_t cellMaxWorldY = worldYMaxByLocal[y];

                if (!hasEditedCells &&
                    cellMaxWorldY > TERRAIN_MIN_Y + 2 &&
                    static_cast<float>(SaturatingAddInt32(cellMaxWorldY, surfaceBandDepth)) <
                        minHorizontalColumnHeight) {
                    brick.voxels[LocalVoxelIndex({x, y, z})] =
                        Utils::PackVoxel(Utils::Material::Stone, 0, 0, Utils::StateFlags::IsStatic);
                    ++brick.nonAirSamples;
                    continue;
                }

                uint32_t voxel = sampleColumnCellVoxel(
                    centerColumn,
                    worldXMinByLocal[x],
                    worldXMaxByLocal[x],
                    cellMinWorldY,
                    cellMaxWorldY,
                    worldZMinByLocal[z],
                    worldZMaxByLocal[z],
                    worldY);
                uint8_t material = Utils::UnpackMaterial(voxel);
                bool clusteredCoarseAirEditForCell = false;
                if (hasEditedCells && ring.cellSize > 1.5f) {
                    const uint32_t cellKey = editCellKey(
                        static_cast<int32_t>(x) + 1,
                        static_cast<int32_t>(y) + 1,
                        static_cast<int32_t>(z) + 1);
                    auto summaryIt = editedCellSummaries.find(cellKey);
                    if (summaryIt != editedCellSummaries.end()) {
                        const uint32_t coarseAirClusterThreshold = std::max<uint32_t>(
                            8u,
                            static_cast<uint32_t>(std::ceil(std::max(1.0f, ring.cellSize))));
                        const EditedCellSummary& summary = summaryIt->second;
                        clusteredCoarseAirEditForCell =
                            summary.foundAir &&
                            summary.solidCount == 0u &&
                            summary.airCount >= coarseAirClusterThreshold;
                    }
                }
                if (!clusteredCoarseAirEditForCell &&
                    material == Utils::Material::Air &&
                    ring.cellSize > 1.5f) {
                    // Coarse mid-voxel cells cover an area, not a single terrain
                    // column. Center-only sampling punched holes through steep
                    // slopes and spires even when the resident brick was fully
                    // generated. Treat generated cells as occupied if any
                    // footprint column intersects the cell volume.
                    const bool footprintTouchesTerrain =
                        static_cast<float>(cellMinWorldY) <= maxFootprintColumnHeight;
                    if (footprintTouchesTerrain) {
                        voxel = sampleColumnCellVoxel(
                            *maxFootprintColumn,
                            worldXMinByLocal[x],
                            worldXMaxByLocal[x],
                            cellMinWorldY,
                            cellMaxWorldY,
                            worldZMinByLocal[z],
                            worldZMaxByLocal[z],
                            std::min(worldY, FloorToInt32Clamped(maxFootprintColumnHeight)));
                        material = Utils::UnpackMaterial(voxel);
                    }
                }
                // Preserve generated shoreline land in mixed coarse cells.
                // The water volume case is already handled in sampleColumnCellVoxel()
                // when the sampled column is actually submerged. Converting sand to
                // water just because a neighboring footprint column dips below sea
                // level makes the lower LOD draw water over land until exact sparse
                // surface pages stream in.
                if (!hasEditedCells &&
                    ring.cellSize > 1.5f &&
                    material == Utils::Material::Stone) {
                    const int32_t terrainTopY = FloorToInt32Clamped(centerColumn.height);
                    const bool nearGeneratedSurfaceSkin =
                        cellMaxWorldY >= SaturatingAddInt32(terrainTopY, -surfaceBandDepth) &&
                        cellMinWorldY <= SaturatingAddInt32(terrainTopY, std::max(1, sampleStep));
                    if (nearGeneratedSurfaceSkin &&
                        centerColumn.height < 160.0f) {
                        if (!centerColumn.reliefValid) {
                            ensureColumnRelief(centerColumn);
                        }
                        if (centerColumn.relief < 10.0f) {
                            const uint8_t variant = static_cast<uint8_t>(
                                Utils::UnpackVariant(voxel));
                            voxel = Utils::PackVoxel(
                                Utils::Material::Dirt,
                                variant,
                                0,
                                Utils::StateFlags::IsStatic);
                            material = Utils::Material::Dirt;
                        }
                    }
                }
                if (material != Utils::Material::Air) {
                    const int32_t terrainTopY = FloorToInt32Clamped(centerColumn.height);
                    const int32_t proceduralSurfaceY =
                        material == Utils::Material::Water ? SEA_LEVEL_Y : terrainTopY;
                    const bool nearProceduralSurface =
                        cellMaxWorldY >= SaturatingAddInt32(proceduralSurfaceY, -surfaceBandDepth) &&
                        cellMinWorldY <= proceduralSurfaceY;
                    const bool coarseSlopeEnvelopeSurface =
                        ring.cellSize > 1.5f &&
                        (maxFootprintColumnHeight - minHorizontalColumnHeight) >=
                            static_cast<float>(std::max(2, sampleStep)) * 0.75f &&
                        static_cast<float>(cellMinWorldY) <= maxFootprintColumnHeight &&
                        static_cast<float>(cellMaxWorldY) >= minHorizontalColumnHeight;
                    const uint8_t neighborMaterials[] = {
                        classifyColumnCellMaterial(
                            posXColumn,
                            worldXMinByHalo[x + 2u],
                            worldXMaxByHalo[x + 2u],
                            cellMinWorldY,
                            cellMaxWorldY,
                            worldZMinByLocal[z],
                            worldZMaxByLocal[z]),
                        classifyColumnCellMaterial(
                            negXColumn,
                            worldXMinByHalo[x],
                            worldXMaxByHalo[x],
                            cellMinWorldY,
                            cellMaxWorldY,
                            worldZMinByLocal[z],
                            worldZMaxByLocal[z]),
                        classifyColumnCellMaterial(
                            centerColumn,
                            worldXMinByLocal[x],
                            worldXMaxByLocal[x],
                            SaturatingAddInt32(cellMinWorldY, sampleStep),
                            SaturatingAddInt32(cellMaxWorldY, sampleStep),
                            worldZMinByLocal[z],
                            worldZMaxByLocal[z]),
                        classifyColumnCellMaterial(
                            centerColumn,
                            worldXMinByLocal[x],
                            worldXMaxByLocal[x],
                            SaturatingAddInt32(cellMinWorldY, -sampleStep),
                            SaturatingAddInt32(cellMaxWorldY, -sampleStep),
                            worldZMinByLocal[z],
                            worldZMaxByLocal[z]),
                        classifyColumnCellMaterial(
                            posZColumn,
                            worldXMinByLocal[x],
                            worldXMaxByLocal[x],
                            cellMinWorldY,
                            cellMaxWorldY,
                            worldZMinByHalo[z + 2u],
                            worldZMaxByHalo[z + 2u]),
                        classifyColumnCellMaterial(
                            negZColumn,
                            worldXMinByLocal[x],
                            worldXMaxByLocal[x],
                            cellMinWorldY,
                            cellMaxWorldY,
                            worldZMinByHalo[z],
                            worldZMaxByHalo[z]),
                    };
                    bool visualSurface = nearProceduralSurface || coarseSlopeEnvelopeSurface;
                    for (uint8_t neighborMaterial : neighborMaterials) {
                        if (isSurfaceNeighbor(material, neighborMaterial)) {
                            visualSurface = true;
                            break;
                        }
                    }
                    if (visualSurface) {
                        voxel |= static_cast<uint32_t>(Utils::StateFlags::VisualSurface) << 24u;
                        ++brick.surfaceSamples;
                    }
                    ++brick.nonAirSamples;
                } else {
                    voxel = airVoxel;
                }
                brick.voxels[LocalVoxelIndex({x, y, z})] = voxel;
            }
        }
    }
}

uint32_t SparseClipmapTileCache::PackSample(int32_t worldX, int32_t worldZ, float height) const {
    int32_t roundedHeight = FloorToInt32Clamped(height);
    uint32_t material = 0u;
    if (height < static_cast<float>(SEA_LEVEL_Y)) {
        roundedHeight = SEA_LEVEL_Y;
        material = static_cast<uint32_t>(Utils::Material::Water);
    } else {
        const float relief = m_terrain.SurfaceReliefAtWithCenter(worldX, worldZ, height, 4);
        const uint32_t voxel =
            m_terrain.SampleGeneratedVoxelWithColumn(worldX, roundedHeight, worldZ, height, relief);
        material = static_cast<uint32_t>(Utils::UnpackMaterial(voxel)) & 0xFFu;
    }
    const uint32_t biasedHeight = static_cast<uint32_t>(
        std::clamp<int64_t>(static_cast<int64_t>(roundedHeight) + 32768ll, 0ll, 65535ll));
    return biasedHeight | (material << 16);
}

bool SparseClipmapTileCache::BuildGpuSnapshot(
    SparseClipmapGpuSnapshot& outSnapshot,
    bool includeHeightLayer,
    bool includeVoxelLayer) const
{
    outSnapshot = {};
    if (!includeHeightLayer && !includeVoxelLayer) {
        return false;
    }
    if ((includeHeightLayer && m_tiles.empty()) ||
        (includeVoxelLayer && m_voxelBricks.empty())) {
        return false;
    }

    const uint32_t side = m_config.tileSampleSide;
    const uint32_t sampleCountPerTile = side * side;
    const uint32_t maxTiles = static_cast<uint32_t>(m_tiles.size());
    const uint32_t lookupCapacity = NextPowerOfTwo(std::max(16u, maxTiles * 4u));
    const uint32_t maxVoxelBricks = static_cast<uint32_t>(m_voxelBricks.size());
    const uint32_t voxelLookupCapacity = NextPowerOfTwo(std::max(16u, maxVoxelBricks * 4u));
    if (includeHeightLayer) {
        outSnapshot.heightSampleRanges = BuildCoalescedSampleRanges(
            m_dirtyHeightSlots,
            maxTiles,
            m_dirtyHeightStartSlot,
            m_dirtyHeightEndSlot);
        const uint32_t heightPayloadSlotCount =
            CountSampleRangeSlots(outSnapshot.heightSampleRanges);
        outSnapshot.metadata.assign(static_cast<size_t>(maxTiles + 1u) * 4u, 0u);
        outSnapshot.lookup.assign(static_cast<size_t>(lookupCapacity) * 4u, 0u);
        outSnapshot.samples.assign(static_cast<size_t>(heightPayloadSlotCount) * sampleCountPerTile, 0u);
        outSnapshot.metadata[0] = 0x56434C50u; // "VCLP"
        outSnapshot.metadata[1] = side;
        outSnapshot.metadata[2] = maxTiles;
        outSnapshot.metadata[3] = (lookupCapacity & 0x00FFFFFFu) |
            ((m_config.ringCount & 0xFFu) << 24u);
        outSnapshot.heightSamplePayloadStartSlot = outSnapshot.heightSampleRanges.empty()
            ? 0u
            : outSnapshot.heightSampleRanges.front().startSlot;
    }
    if (includeVoxelLayer) {
        outSnapshot.voxelSampleRanges = BuildCoalescedSampleRanges(
            m_dirtyVoxelSlots,
            maxVoxelBricks,
            m_dirtyVoxelStartSlot,
            m_dirtyVoxelEndSlot);
        const uint32_t voxelPayloadSlotCount =
            CountSampleRangeSlots(outSnapshot.voxelSampleRanges);
        outSnapshot.voxelMetadata.assign(static_cast<size_t>(maxVoxelBricks + 1u) * 4u, 0u);
        outSnapshot.voxelLookup.assign(static_cast<size_t>(voxelLookupCapacity) * 4u, 0u);
        outSnapshot.voxelSamples.assign(
            static_cast<size_t>(voxelPayloadSlotCount) * SPARSE_BRICK_VOXEL_COUNT,
            0u);
        outSnapshot.voxelMetadata[0] = 0x56435658u; // "VCVX"
        outSnapshot.voxelMetadata[1] = SPARSE_BRICK_SIZE;
        outSnapshot.voxelMetadata[2] = maxVoxelBricks;
        outSnapshot.voxelMetadata[3] = (voxelLookupCapacity & 0x00FFFFFFu) |
            ((m_config.ringCount & 0xFFu) << 24u);
        outSnapshot.voxelSamplePayloadStartSlot = outSnapshot.voxelSampleRanges.empty()
            ? 0u
            : outSnapshot.voxelSampleRanges.front().startSlot;
    }

    uint32_t residentHeightEntries = 0;
    uint32_t maxUsedTileSlot = 0;
    if (includeHeightLayer) {
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

            uint32_t payloadSlotBase = 0;
            for (const SparseClipmapSampleRange& range : outSnapshot.heightSampleRanges) {
                if (tileSlot >= range.startSlot && tileSlot < range.startSlot + range.slotCount) {
                    const uint32_t payloadSlot = payloadSlotBase + (tileSlot - range.startSlot);
                    const size_t sampleBase = static_cast<size_t>(payloadSlot) * sampleCountPerTile;
                    if (sampleBase + tile.packedSamples.size() <= outSnapshot.samples.size()) {
                        std::copy(
                            tile.packedSamples.begin(),
                            tile.packedSamples.end(),
                            outSnapshot.samples.begin() + sampleBase);
                    }
                    break;
                }
                payloadSlotBase += range.slotCount;
            }

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
    }

    outSnapshot.tileCount = residentHeightEntries == 0 ? 0u : maxUsedTileSlot + 1u;
    outSnapshot.tileSampleSide = side;
    outSnapshot.lookupCapacity = lookupCapacity;
    if (includeHeightLayer &&
        m_dirtyHeightStartSlot != UINT32_MAX &&
        m_dirtyHeightStartSlot <= m_dirtyHeightEndSlot) {
        outSnapshot.heightDirtyStartSlot = m_dirtyHeightStartSlot;
        outSnapshot.heightDirtySlotCount = m_dirtyHeightEndSlot - m_dirtyHeightStartSlot + 1u;
    }

    uint32_t residentVoxelEntries = 0;
    uint32_t maxUsedVoxelSlot = 0;
    if (includeVoxelLayer) {
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

            uint32_t payloadSlotBase = 0;
            for (const SparseClipmapSampleRange& range : outSnapshot.voxelSampleRanges) {
                if (voxelSlot >= range.startSlot && voxelSlot < range.startSlot + range.slotCount) {
                    const uint32_t payloadSlot = payloadSlotBase + (voxelSlot - range.startSlot);
                    const size_t sampleBase = static_cast<size_t>(payloadSlot) * SPARSE_BRICK_VOXEL_COUNT;
                    if (sampleBase + brick.voxels.size() <= outSnapshot.voxelSamples.size()) {
                        std::copy(
                            brick.voxels.begin(),
                            brick.voxels.end(),
                            outSnapshot.voxelSamples.begin() + sampleBase);
                    }
                    break;
                }
                payloadSlotBase += range.slotCount;
            }

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
    }
    outSnapshot.voxelBrickCount = residentVoxelEntries == 0 ? 0u : maxUsedVoxelSlot + 1u;
    outSnapshot.voxelLookupCapacity = voxelLookupCapacity;
    if (includeVoxelLayer &&
        m_dirtyVoxelStartSlot != UINT32_MAX &&
        m_dirtyVoxelStartSlot <= m_dirtyVoxelEndSlot) {
        outSnapshot.voxelDirtyStartSlot = m_dirtyVoxelStartSlot;
        outSnapshot.voxelDirtySlotCount = m_dirtyVoxelEndSlot - m_dirtyVoxelStartSlot + 1u;
    }
    if (includeVoxelLayer) {
        outSnapshot.voxelMetadata[2] = outSnapshot.voxelBrickCount;
    }
    outSnapshot.frameIndex = m_dirtySerial;
    if (includeHeightLayer) {
        outSnapshot.metadata[2] = outSnapshot.tileCount;
    }
    return residentHeightEntries > 0 || residentVoxelEntries > 0;
}

void SparseClipmapTileCache::ClearHeightDirtyRange() {
    m_dirtyHeightStartSlot = UINT32_MAX;
    m_dirtyHeightEndSlot = 0;
    m_dirtyHeightSlots.clear();
}

void SparseClipmapTileCache::ClearVoxelDirtyRange() {
    m_dirtyVoxelStartSlot = UINT32_MAX;
    m_dirtyVoxelEndSlot = 0;
    m_dirtyVoxelSlots.clear();
}

void SparseClipmapTileCache::MarkHeightSlotDirty(uint32_t slot) {
    if (slot == UINT32_MAX || slot >= m_tiles.size()) {
        return;
    }
    m_dirtyHeightStartSlot = std::min(m_dirtyHeightStartSlot, slot);
    m_dirtyHeightEndSlot = std::max(m_dirtyHeightEndSlot, slot);
    if (std::find(m_dirtyHeightSlots.begin(), m_dirtyHeightSlots.end(), slot) == m_dirtyHeightSlots.end()) {
        m_dirtyHeightSlots.push_back(slot);
    }
}

void SparseClipmapTileCache::MarkVoxelSlotDirty(uint32_t slot) {
    if (slot == UINT32_MAX || slot >= m_voxelBricks.size()) {
        return;
    }
    m_dirtyVoxelStartSlot = std::min(m_dirtyVoxelStartSlot, slot);
    m_dirtyVoxelEndSlot = std::max(m_dirtyVoxelEndSlot, slot);
    if (std::find(m_dirtyVoxelSlots.begin(), m_dirtyVoxelSlots.end(), slot) == m_dirtyVoxelSlots.end()) {
        m_dirtyVoxelSlots.push_back(slot);
    }
}

bool SparseClipmapTileCache::HasCompleteFinerVoxelCoverage(const SparseVoxelClipmapCoord& coord) const {
    if (coord.ring <= 0) {
        return false;
    }

    const int32_t childRing = coord.ring - 1;
    const int32_t baseX = SaturatingAddInt32(coord.x, coord.x);
    const int32_t baseY = SaturatingAddInt32(coord.y, coord.y);
    const int32_t baseZ = SaturatingAddInt32(coord.z, coord.z);
    for (int32_t dz = 0; dz <= 1; ++dz) {
        for (int32_t dy = 0; dy <= 1; ++dy) {
            for (int32_t dx = 0; dx <= 1; ++dx) {
                const SparseVoxelClipmapCoord child{
                    childRing,
                    SaturatingAddInt32(baseX, dx),
                    SaturatingAddInt32(baseY, dy),
                    SaturatingAddInt32(baseZ, dz)
                };
                if (m_voxelSlotByCoord.find(child) == m_voxelSlotByCoord.end()) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool SparseClipmapTileCache::HasCoarserVoxelParent(const SparseVoxelClipmapCoord& coord) const {
    if (coord.ring < 0 ||
        static_cast<uint32_t>(coord.ring + 1) >= std::max<uint32_t>(1u, m_config.ringCount)) {
        return false;
    }

    const SparseVoxelClipmapCoord parent{
        coord.ring + 1,
        FloorDiv2Int32(coord.x),
        FloorDiv2Int32(coord.y),
        FloorDiv2Int32(coord.z)
    };
    return m_voxelSlotByCoord.find(parent) != m_voxelSlotByCoord.end();
}

bool SparseClipmapTileCache::MissingBrickWithinFarSvoDomain(const SparseVoxelClipmapCoord& coord) const {
    if (!m_farSvoFallbackMetadata.ready ||
        m_farSvoFallbackMetadata.pageRadius <= 0 ||
        !std::isfinite(m_farSvoFallbackMetadata.pageSize) ||
        m_farSvoFallbackMetadata.pageSize <= 0.0f ||
        !std::isfinite(m_farSvoFallbackMetadata.rootMinY) ||
        m_farSvoFallbackMetadata.pageCoverageRatio < 0.999f ||
        coord.ring < 0 ||
        static_cast<uint32_t>(coord.ring) >= std::max<uint32_t>(1u, m_config.ringCount)) {
        return false;
    }

    const double cellSize =
        std::max(1.0, static_cast<double>(m_config.minCellSize)) *
        static_cast<double>(uint64_t{1} << std::min<uint32_t>(static_cast<uint32_t>(coord.ring), 30u));
    const double brickWorldSize = cellSize * static_cast<double>(SPARSE_BRICK_SIZE);
    const double minX = static_cast<double>(coord.x) * brickWorldSize;
    const double minY = static_cast<double>(coord.y) * brickWorldSize;
    const double minZ = static_cast<double>(coord.z) * brickWorldSize;
    const double maxX = (static_cast<double>(coord.x) + 1.0) * brickWorldSize;
    const double maxY = (static_cast<double>(coord.y) + 1.0) * brickWorldSize;
    const double maxZ = (static_cast<double>(coord.z) + 1.0) * brickWorldSize;

    const double pageSize = static_cast<double>(m_farSvoFallbackMetadata.pageSize);
    const double radius = static_cast<double>(m_farSvoFallbackMetadata.pageRadius);
    const double domainMinX = -radius * pageSize;
    const double domainMaxX = (radius + 1.0) * pageSize;
    const double domainMinZ = domainMinX;
    const double domainMaxZ = domainMaxX;
    const double domainMinY = static_cast<double>(m_farSvoFallbackMetadata.rootMinY);
    const double domainMaxY = domainMinY + pageSize;

    return minX >= domainMinX &&
        maxX <= domainMaxX &&
        minZ >= domainMinZ &&
        maxZ <= domainMaxZ &&
        minY >= domainMinY &&
        maxY <= domainMaxY;
}

void SparseClipmapTileCache::RefreshStats(
    uint32_t generatedLastFrame,
    uint32_t evictedLastFrame,
    uint32_t generatedVoxelLastFrame,
    uint32_t evictedVoxelLastFrame)
{
    PruneAsyncVisibleReservations(m_lastStatsFrame);
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
    m_stats.residentVoxelNonAirSamples = 0;
    m_stats.residentVoxelSurfaceSamples = 0;
    m_stats.voxelRingCount = std::min<uint32_t>(
        static_cast<uint32_t>(m_config.ringCount),
        SPARSE_CLIPMAP_MAX_STATS_RINGS);
    m_stats.residentVoxelBricksByRing.fill(0u);
    m_stats.queuedVoxelBricksByRing.fill(0u);
    m_stats.interestedVoxelBricksByRing.fill(0u);
    m_stats.missingInterestedVoxelBricksByRing.fill(0u);
    for (const auto& [coord, slot] : m_voxelSlotByCoord) {
        if (coord.ring >= 0 &&
            static_cast<uint32_t>(coord.ring) < SPARSE_CLIPMAP_MAX_STATS_RINGS) {
            ++m_stats.residentVoxelBricksByRing[static_cast<uint32_t>(coord.ring)];
        }
        if (slot < m_voxelBricks.size()) {
            m_stats.residentVoxelNonAirSamples += m_voxelBricks[slot].nonAirSamples;
            m_stats.residentVoxelSurfaceSamples += m_voxelBricks[slot].surfaceSamples;
        }
    }
    m_stats.queuedVoxelBricks = static_cast<uint32_t>(m_voxelGenerationQueue.size());
    m_stats.backlogHeightBricks = m_stats.queuedTiles;
    m_stats.backlogVoxelBricks = m_stats.queuedVoxelBricks;
    m_stats.backlogVoxelOldestAge = 0;
    m_stats.backlogVoxelMaxAge = 0;
    m_stats.backlogVoxelAge0To30 = 0;
    m_stats.backlogVoxelAge31To90 = 0;
    m_stats.backlogVoxelAge91To180 = 0;
    m_stats.backlogVoxelAge181Plus = 0;
    m_stats.visiblePriorityVoxelBricks = 0;
    m_stats.cachePriorityVoxelBricks = 0;
    m_stats.queuedVisiblePriorityVoxelBricks = 0;
    m_stats.queuedCachePriorityVoxelBricks = 0;
    m_stats.visiblePriorityBacklogMaxAge = 0;
    m_stats.cachePriorityBacklogMaxAge = 0;
    m_stats.asyncVisibleReservations = static_cast<uint32_t>(
        std::min<size_t>(
            m_asyncVisibleReservations.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    m_stats.asyncVisibleReservationsDue = 0u;
    m_stats.asyncVisibleReservationsOverdue = 0u;
    m_stats.asyncVisibleReservationBacklogMaxAge = 0u;
    for (const auto& [coord, reservation] : m_asyncVisibleReservations) {
        (void)coord;
        if (reservation.deadlineFrame <= m_lastStatsFrame) {
            ++m_stats.asyncVisibleReservationsDue;
        }
        if (reservation.deadlineFrame < m_lastStatsFrame) {
            ++m_stats.asyncVisibleReservationsOverdue;
        }
        if (m_lastStatsFrame >= reservation.firstFrame) {
            m_stats.asyncVisibleReservationBacklogMaxAge =
                std::max(
                    m_stats.asyncVisibleReservationBacklogMaxAge,
                    m_lastStatsFrame - reservation.firstFrame);
        }
    }
    for (auto it = m_visiblePriorityVoxelSet.begin();
         it != m_visiblePriorityVoxelSet.end();) {
        const SparseVoxelClipmapCoord coord = *it;
        if ((!m_voxelInterestSet.empty() && m_voxelInterestSet.find(coord) == m_voxelInterestSet.end()) ||
            m_voxelSlotByCoord.find(coord) != m_voxelSlotByCoord.end() ||
            m_queuedVoxelSet.find(coord) == m_queuedVoxelSet.end()) {
            it = m_visiblePriorityVoxelSet.erase(it);
            continue;
        }
        ++m_stats.visiblePriorityVoxelBricks;
        ++it;
    }
    for (const SparseVoxelClipmapCoord& coord : m_voxelGenerationQueue) {
        if (coord.ring >= 0 &&
            static_cast<uint32_t>(coord.ring) < SPARSE_CLIPMAP_MAX_STATS_RINGS) {
            ++m_stats.queuedVoxelBricksByRing[static_cast<uint32_t>(coord.ring)];
        }
        const bool visiblePriority =
            m_visiblePriorityVoxelSet.find(coord) != m_visiblePriorityVoxelSet.end() ||
            m_asyncVisibleReservations.find(coord) != m_asyncVisibleReservations.end();
        if (visiblePriority) {
            ++m_stats.queuedVisiblePriorityVoxelBricks;
        } else {
            ++m_stats.queuedCachePriorityVoxelBricks;
        }
        const auto firstFrameIt = m_voxelBacklogFirstFrame.find(coord);
        if (firstFrameIt != m_voxelBacklogFirstFrame.end() &&
            m_lastStatsFrame >= firstFrameIt->second) {
            const uint32_t age = m_lastStatsFrame - firstFrameIt->second;
            m_stats.backlogVoxelMaxAge = std::max(m_stats.backlogVoxelMaxAge, age);
            if (visiblePriority) {
                m_stats.visiblePriorityBacklogMaxAge =
                    std::max(m_stats.visiblePriorityBacklogMaxAge, age);
            } else {
                m_stats.cachePriorityBacklogMaxAge =
                    std::max(m_stats.cachePriorityBacklogMaxAge, age);
            }
            if (age <= 30u) {
                ++m_stats.backlogVoxelAge0To30;
            } else if (age <= 90u) {
                ++m_stats.backlogVoxelAge31To90;
            } else if (age <= 180u) {
                ++m_stats.backlogVoxelAge91To180;
            } else {
                ++m_stats.backlogVoxelAge181Plus;
            }
        }
    }
    m_stats.backlogVoxelOldestAge = m_stats.backlogVoxelMaxAge;
    m_stats.interestedVoxelBricks = static_cast<uint32_t>(m_voxelInterestSet.size());
    m_stats.missingInterestedVoxelBricks = 0;
    m_stats.visiblePriorityTaggedLastFrame = m_visiblePriorityTaggedLastFrame;
    m_stats.visiblePriorityPrioritizedLastFrame = m_visiblePriorityPrioritizedLastFrame;
    m_stats.fallbackValidityClassifierActive = m_config.fallbackValidityClassifier ? 1u : 0u;
    m_stats.fallbackContractDiagnosticsActive = m_config.fallbackContractDiagnostics ? 1u : 0u;
    m_stats.farSvoFallbackProofActive = m_config.farSvoFallbackProof ? 1u : 0u;
    m_stats.asyncNoncriticalGenerationActive =
        (m_config.asyncNoncriticalGeneration ||
         m_config.asyncVisibleCriticalGeneration) ? 1u : 0u;
    if (m_config.asyncNoncriticalGeneration ||
        m_config.asyncVisibleCriticalGeneration) {
        std::lock_guard<std::mutex> lock(m_asyncNoncriticalGenerationMutex);
        m_stats.asyncNoncriticalGenerationQueueDepth =
            static_cast<uint32_t>(std::min<size_t>(
                m_asyncNoncriticalGenerationQueue.size(),
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        m_stats.asyncNoncriticalGenerationResultDepth =
            static_cast<uint32_t>(std::min<size_t>(
                m_asyncNoncriticalGenerationResults.size(),
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        m_stats.asyncNoncriticalGenerationPending =
            static_cast<uint32_t>(std::min<size_t>(
                m_asyncNoncriticalGenerationPending.size(),
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    } else {
        m_stats.asyncNoncriticalGenerationQueueDepth = 0u;
        m_stats.asyncNoncriticalGenerationResultDepth = 0u;
        m_stats.asyncNoncriticalGenerationPending = 0u;
    }
    m_stats.asyncNoncriticalGenerationEnqueuedLastFrame =
        m_asyncNoncriticalGenerationEnqueuedLastFrame;
    m_stats.asyncNoncriticalGenerationCompletedLastFrame =
        m_asyncNoncriticalGenerationCompletedLastFrame;
    m_stats.asyncNoncriticalGenerationAppliedLastFrame =
        m_asyncNoncriticalGenerationAppliedLastFrame;
    m_stats.asyncNoncriticalGenerationDiscardedLastFrame =
        m_asyncNoncriticalGenerationDiscardedLastFrame;
    m_stats.asyncNoncriticalGenerationDuplicateSyncLastFrame =
        m_asyncNoncriticalGenerationDuplicateSyncLastFrame;
    m_stats.asyncVisibleCriticalGenerationEnqueuedLastFrame =
        m_asyncVisibleCriticalGenerationEnqueuedLastFrame;
    m_stats.asyncVisibleCriticalGenerationCompletedLastFrame =
        m_asyncVisibleCriticalGenerationCompletedLastFrame;
    m_stats.asyncVisibleCriticalGenerationAppliedLastFrame =
        m_asyncVisibleCriticalGenerationAppliedLastFrame;
    m_stats.asyncVisibleCriticalGenerationDiscardedLastFrame =
        m_asyncVisibleCriticalGenerationDiscardedLastFrame;
    m_stats.asyncVisibleCriticalGenerationDuplicateSyncLastFrame =
        m_asyncVisibleCriticalGenerationDuplicateSyncLastFrame;
    m_stats.asyncVisibleReservationAppliedLastFrame =
        m_asyncVisibleReservationAppliedLastFrame;
    m_stats.asyncVisibleReservationApplyDeferredLastFrame =
        m_asyncVisibleReservationApplyDeferredLastFrame;
    m_stats.asyncVisibleReservationApplyLimitLastFrame =
        m_asyncVisibleReservationApplyLimitLastFrame;
    m_stats.asyncNoncriticalGenerationWorkerMsLastFrame =
        m_asyncNoncriticalGenerationWorkerMsLastFrame;
    m_stats.asyncNoncriticalGenerationApplyMsLastFrame =
        m_asyncNoncriticalGenerationApplyMsLastFrame;
    m_stats.predictedVisibleAdmissionSamplesLastFrame =
        m_predictedVisibleAdmissionSamplesLastFrame;
    m_stats.predictedVisibleAdmissionSnapshotMsLastFrame =
        m_predictedVisibleAdmissionSnapshotMsLastFrame;
    m_stats.predictedVisibleAdmissionRebuildMsLastFrame =
        m_predictedVisibleAdmissionRebuildMsLastFrame;
    m_stats.predictedVisibleAdmissionRestoreMsLastFrame =
        m_predictedVisibleAdmissionRestoreMsLastFrame;
    m_stats.predictedVisibleAdmissionQueueMsLastFrame =
        m_predictedVisibleAdmissionQueueMsLastFrame;
    m_stats.missingFallbackValidVoxelBricks = 0;
    m_stats.missingFallbackInvalidVoxelBricks = 0;
    m_stats.missingFallbackUnknownVoxelBricks = 0;
    m_stats.highAltitudeCurrentInterestVoxelBricks =
        (m_config.fallbackValidityClassifier && m_lastCameraYForStats > 384.0f)
            ? m_stats.interestedVoxelBricks
            : 0u;
    m_stats.highAltitudeFallbackValidVoxelBricks = 0;
    m_stats.highAltitudeFallbackInvalidVoxelBricks = 0;
    m_stats.highAltitudeFallbackUnknownVoxelBricks = 0;
    m_stats.finerLodFallbackAvailableVoxelBricks = 0;
    m_stats.lowerLodFallbackAvailableVoxelBricks = 0;
    m_stats.farSvoFallbackAvailableVoxelBricks = 0;
    m_stats.waterFallbackAvailableVoxelBricks = 0;
    m_stats.skyFallbackAvailableVoxelBricks = 0;
    m_stats.oldResidentFallbackAvailableVoxelBricks = 0;
    m_stats.fallbackRejectNoLowerLod = 0;
    m_stats.fallbackRejectFarSvoOutOfDomain = 0;
    m_stats.fallbackRejectShorelineMixedCell = 0;
    m_stats.fallbackRejectNearCamera = 0;
    m_stats.fallbackRejectScreenCritical = 0;
    m_stats.fallbackRejectUnknownOwner = 0;
    m_stats.asyncEligibleVoxelBricks = 0;
    m_stats.syncRequiredVoxelBricks = 0;
    m_stats.contractCpuProvableValid = 0;
    m_stats.contractCpuProvableInvalid = 0;
    m_stats.contractCpuUnknown = 0;
    m_stats.contractShaderOnlyUnknown = 0;
    m_stats.contractFinerValid = 0;
    m_stats.contractFinerInvalid = 0;
    m_stats.contractFinerUnknown = 0;
    m_stats.contractCoarserValid = 0;
    m_stats.contractCoarserRejectedHighAlt = 0;
    m_stats.contractCoarserRejectedRayAngle = 0;
    m_stats.contractCoarserRejectedError = 0;
    m_stats.contractCoarserMissing = 0;
    m_stats.contractFarSvoDomainValid = 0;
    m_stats.contractFarSvoDomainInvalid = 0;
    m_stats.contractFarSvoMaterialValid = 0;
    m_stats.contractFarSvoMaterialUnknown = 0;
    m_stats.contractFarSvoRejected = 0;
    m_stats.contractWaterValid = 0;
    m_stats.contractWaterRejectedShoreline = 0;
    m_stats.contractWaterUnknown = 0;
    m_stats.contractSkyValid = 0;
    m_stats.contractSkyUnknown = 0;
    m_stats.contractPreviousResidentValid = 0;
    m_stats.contractPreviousResidentStale = 0;
    m_stats.contractPublicReadinessRejected = 0;
    m_stats.contractEditStampRejected = 0;
    m_stats.contractMixedOwnerUnknown = 0;
    m_stats.contractNearCameraRejected = 0;
    m_stats.contractScreenCriticalRejected = 0;
    m_stats.contractCoverageEmergencyRejected = 0;
    m_stats.contractHighAltRejected = 0;
    m_stats.contractValidButNotDeferredReason = 0;
    m_stats.contractInvalidReasonTop1 = 0;
    m_stats.contractUnknownReasonTop1 = 0;
    const bool classifyFallback = m_config.fallbackValidityClassifier;
    const bool diagnoseContract = m_config.fallbackContractDiagnostics;
    const bool diagnoseFarSvo = diagnoseContract && m_config.farSvoFallbackProof;
    const bool highAltitudeFallbackView = m_lastCameraYForStats > 384.0f;
    for (const SparseVoxelClipmapCoord& coord : m_voxelInterestSet) {
        if (coord.ring >= 0 &&
            static_cast<uint32_t>(coord.ring) < SPARSE_CLIPMAP_MAX_STATS_RINGS) {
            ++m_stats.interestedVoxelBricksByRing[static_cast<uint32_t>(coord.ring)];
        }
        if (m_voxelSlotByCoord.find(coord) == m_voxelSlotByCoord.end()) {
            ++m_stats.missingInterestedVoxelBricks;
            if (coord.ring >= 0 &&
                static_cast<uint32_t>(coord.ring) < SPARSE_CLIPMAP_MAX_STATS_RINGS) {
                ++m_stats.missingInterestedVoxelBricksByRing[static_cast<uint32_t>(coord.ring)];
            }
            if (classifyFallback) {
                const bool completeFinerCoverage = HasCompleteFinerVoxelCoverage(coord);
                const bool coarserParentAvailable = HasCoarserVoxelParent(coord);
                const bool farSvoDomainAvailable =
                    diagnoseFarSvo && MissingBrickWithinFarSvoDomain(coord);
                if (completeFinerCoverage) {
                    ++m_stats.finerLodFallbackAvailableVoxelBricks;
                    ++m_stats.missingFallbackValidVoxelBricks;
                    ++m_stats.asyncEligibleVoxelBricks;
                    if (diagnoseContract) {
                        ++m_stats.contractCpuProvableValid;
                        ++m_stats.contractFinerValid;
                    }
                    if (highAltitudeFallbackView) {
                        ++m_stats.highAltitudeFallbackValidVoxelBricks;
                    }
                } else {
                    const bool screenCritical =
                        coord.ring >= 0 && static_cast<uint32_t>(coord.ring) < 2u;
                    if (coarserParentAvailable) {
                        ++m_stats.lowerLodFallbackAvailableVoxelBricks;
                        if (diagnoseContract) {
                            if (highAltitudeFallbackView) {
                                ++m_stats.contractCoarserRejectedHighAlt;
                            } else {
                                ++m_stats.contractCoarserRejectedRayAngle;
                            }
                        }
                    } else {
                        ++m_stats.fallbackRejectNoLowerLod;
                        if (diagnoseContract) {
                            ++m_stats.contractCoarserMissing;
                        }
                    }

                    if (diagnoseContract) {
                        ++m_stats.contractFinerInvalid;
                        if (diagnoseFarSvo) {
                            if (farSvoDomainAvailable) {
                                ++m_stats.farSvoFallbackAvailableVoxelBricks;
                                ++m_stats.contractFarSvoDomainValid;
                                ++m_stats.contractFarSvoMaterialUnknown;
                            } else {
                                ++m_stats.fallbackRejectFarSvoOutOfDomain;
                                ++m_stats.contractFarSvoDomainInvalid;
                                ++m_stats.contractFarSvoRejected;
                            }
                        }
                    }
                    if (screenCritical) {
                        ++m_stats.missingFallbackInvalidVoxelBricks;
                        ++m_stats.fallbackRejectScreenCritical;
                        if (diagnoseContract) {
                            ++m_stats.contractCpuProvableInvalid;
                            ++m_stats.contractScreenCriticalRejected;
                        }
                        if (coord.ring == 0) {
                            ++m_stats.fallbackRejectNearCamera;
                            if (diagnoseContract) {
                                ++m_stats.contractNearCameraRejected;
                            }
                        }
                        if (highAltitudeFallbackView) {
                            ++m_stats.highAltitudeFallbackInvalidVoxelBricks;
                            if (diagnoseContract) {
                                ++m_stats.contractHighAltRejected;
                            }
                        }
                    } else {
                        // Coarser mid-voxel parents are ray- and view-dependent
                        // in PS_Raymarch, and high-alt views explicitly reject
                        // them. Far-SVO/water/sky validity is also decided per
                        // ray in shader space, so CPU cannot safely defer this
                        // brick unless finer resident coverage already proved it.
                        ++m_stats.missingFallbackUnknownVoxelBricks;
                        ++m_stats.fallbackRejectUnknownOwner;
                        if (diagnoseContract) {
                            ++m_stats.contractCpuUnknown;
                            ++m_stats.contractShaderOnlyUnknown;
                            ++m_stats.contractMixedOwnerUnknown;
                            ++m_stats.contractWaterUnknown;
                            ++m_stats.contractSkyUnknown;
                            if (highAltitudeFallbackView) {
                                ++m_stats.contractHighAltRejected;
                            }
                        }
                        if (highAltitudeFallbackView) {
                            ++m_stats.highAltitudeFallbackUnknownVoxelBricks;
                        }
                    }
                    ++m_stats.syncRequiredVoxelBricks;
                }
            }
        }
    }
    m_stats.cachePriorityVoxelBricks =
        m_stats.missingInterestedVoxelBricks > m_stats.visiblePriorityVoxelBricks
            ? m_stats.missingInterestedVoxelBricks - m_stats.visiblePriorityVoxelBricks
            : 0u;
    if (diagnoseContract) {
        if (m_stats.contractNearCameraRejected > 0u) {
            m_stats.contractInvalidReasonTop1 = 1u;
        } else if (m_stats.contractScreenCriticalRejected > 0u) {
            m_stats.contractInvalidReasonTop1 = 2u;
        } else if (m_stats.contractHighAltRejected > 0u) {
            m_stats.contractInvalidReasonTop1 = 3u;
        } else if (m_stats.contractFarSvoDomainInvalid > 0u) {
            m_stats.contractInvalidReasonTop1 = 4u;
        } else if (m_stats.contractCoarserMissing > 0u) {
            m_stats.contractInvalidReasonTop1 = 5u;
        }

        if (m_stats.contractFarSvoMaterialUnknown > 0u) {
            m_stats.contractUnknownReasonTop1 = 1u;
        } else if (m_stats.contractMixedOwnerUnknown > 0u) {
            m_stats.contractUnknownReasonTop1 = 2u;
        } else if (m_stats.contractCoarserRejectedRayAngle > 0u) {
            m_stats.contractUnknownReasonTop1 = 3u;
        } else if (m_stats.contractWaterUnknown > 0u) {
            m_stats.contractUnknownReasonTop1 = 4u;
        } else if (m_stats.contractSkyUnknown > 0u) {
            m_stats.contractUnknownReasonTop1 = 5u;
        }
        if (m_stats.contractCpuProvableValid > 0u &&
            m_stats.asyncNoncriticalGenerationActive == 0u) {
            m_stats.contractValidButNotDeferredReason = 1u;
        }
    }
    m_stats.generatedVoxelBricksLastFrame = generatedVoxelLastFrame;
    m_stats.evictedVoxelBricksLastFrame = evictedVoxelLastFrame;
    m_stats.generatedVoxelBricksByRingLastFrame = m_generatedVoxelBricksByRingLastFrame;
    m_stats.sharedVoxelColumnCacheActive = m_config.sharedVoxelColumnCache ? 1u : 0u;
    m_stats.sharedVoxelColumnCacheEntries = static_cast<uint32_t>(
        std::min<size_t>(
            m_sharedVoxelColumnCache.size(),
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    m_stats.sharedVoxelColumnHeightHitsLastFrame = m_sharedVoxelColumnHeightHitsLastFrame;
    m_stats.sharedVoxelColumnHeightMissesLastFrame = m_sharedVoxelColumnHeightMissesLastFrame;
    m_stats.sharedVoxelColumnReliefHitsLastFrame = m_sharedVoxelColumnReliefHitsLastFrame;
    m_stats.sharedVoxelColumnReliefMissesLastFrame = m_sharedVoxelColumnReliefMissesLastFrame;
    m_stats.directVoxelFootprintColumnsActive = m_config.directVoxelFootprintColumns ? 1u : 0u;
    m_stats.parallelWorkerColumnCacheActive = m_config.parallelWorkerColumnCache ? 1u : 0u;
    m_stats.parallelWorkerColumnCacheEntries = m_parallelWorkerColumnCacheEntriesLastFrame;
    m_stats.parallelWorkerColumnHeightHitsLastFrame = m_parallelWorkerColumnHeightHitsLastFrame;
    m_stats.parallelWorkerColumnHeightMissesLastFrame = m_parallelWorkerColumnHeightMissesLastFrame;
    m_stats.parallelWorkerColumnReliefHitsLastFrame = m_parallelWorkerColumnReliefHitsLastFrame;
    m_stats.parallelWorkerColumnReliefMissesLastFrame = m_parallelWorkerColumnReliefMissesLastFrame;
    m_stats.parallelVoxelPumpActive = m_parallelVoxelPumpBricksLastFrame != 0u ? 1u : 0u;
    m_stats.parallelVoxelPumpBricksLastFrame = m_parallelVoxelPumpBricksLastFrame;
    m_stats.parallelVoxelPumpWorkersLastFrame = m_parallelVoxelPumpWorkersLastFrame;
    m_stats.parallelVoxelPumpWallMsLastFrame = m_parallelVoxelPumpWallMsLastFrame;
    m_stats.interestReusedLastFrame = m_interestReusedLastFrame;
    m_stats.backlogAwarePumpActive = m_config.backlogAwarePump ? 1u : 0u;
    m_stats.pumpBudgetMs = m_effectivePumpBudgetMsLastFrame;
    m_stats.pumpBudgetHitLastFrame = m_pumpBudgetHitLastFrame;
    m_stats.prunedVoxelBacklogLastFrame = m_prunedVoxelBacklogLastFrame;
    m_stats.newlyInterestedTilesLastFrame = m_newlyInterestedTilesLastFrame;
    m_stats.newlyInterestedVoxelBricksLastFrame = m_newlyInterestedVoxelBricksLastFrame;
    m_stats.noLongerInterestedTilesLastFrame = m_noLongerInterestedTilesLastFrame;
    m_stats.noLongerInterestedVoxelBricksLastFrame = m_noLongerInterestedVoxelBricksLastFrame;
    m_stats.residentInterestedTiles = m_residentInterestedTilesLastFrame;
    m_stats.residentInterestedVoxelBricks = m_residentInterestedVoxelBricksLastFrame;
    m_stats.reusedInterestedTilesLastFrame = m_reusedInterestedTilesLastFrame;
    m_stats.reusedInterestedVoxelBricksLastFrame = m_reusedInterestedVoxelBricksLastFrame;
    m_stats.voxelInterestLineMsLastFrame = m_voxelInterestLineMsLastFrame;
    m_stats.voxelInterestAnchorMsLastFrame = m_voxelInterestAnchorMsLastFrame;
    m_stats.voxelInterestSortEmitMsLastFrame = m_voxelInterestSortEmitMsLastFrame;
    m_stats.voxelInterestBacklogMsLastFrame = m_voxelInterestBacklogMsLastFrame;
    m_stats.voxelInterestDiagnosticsMsLastFrame = m_voxelInterestDiagnosticsMsLastFrame;
    m_stats.voxelInterestCandidatesLastFrame = m_voxelInterestCandidatesLastFrame;
    m_stats.voxelInterestCandidateAttemptsLastFrame = m_voxelInterestCandidateAttemptsLastFrame;
    m_stats.voxelInterestCandidateDuplicateHitsLastFrame = m_voxelInterestCandidateDuplicateHitsLastFrame;
    m_stats.voxelInterestCandidateScoreUpdatesLastFrame = m_voxelInterestCandidateScoreUpdatesLastFrame;
    m_stats.voxelInterestCandidateMaxRingUniqueLastFrame = m_voxelInterestCandidateMaxRingUniqueLastFrame;
    m_stats.voxelInterestCandidateMaxRingAttemptsLastFrame = m_voxelInterestCandidateMaxRingAttemptsLastFrame;
    m_stats.voxelInterestLineCandidateAttemptsLastFrame = m_voxelInterestLineCandidateAttemptsLastFrame;
    m_stats.voxelInterestLineCandidateDuplicateHitsLastFrame = m_voxelInterestLineCandidateDuplicateHitsLastFrame;
    m_stats.voxelInterestLineCandidateScoreUpdatesLastFrame = m_voxelInterestLineCandidateScoreUpdatesLastFrame;
    m_stats.voxelInterestAnchorTerrainCandidateAttemptsLastFrame =
        m_voxelInterestAnchorTerrainCandidateAttemptsLastFrame;
    m_stats.voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame =
        m_voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame;
    m_stats.voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame =
        m_voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame;
    m_stats.voxelInterestAnchorFootprintCandidateAttemptsLastFrame =
        m_voxelInterestAnchorFootprintCandidateAttemptsLastFrame;
    m_stats.voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame =
        m_voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame;
    m_stats.voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame =
        m_voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame;
    m_stats.voxelInterestAnchorCameraCandidateAttemptsLastFrame =
        m_voxelInterestAnchorCameraCandidateAttemptsLastFrame;
    m_stats.voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame =
        m_voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame;
    m_stats.voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame =
        m_voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame;
    m_stats.voxelInterestEmittedLastFrame = m_voxelInterestEmittedLastFrame;
    m_stats.voxelInterestReusedLastFrame = m_voxelInterestReusedLastFrame;
    m_stats.voxelInterestReuseAgeLastFrame = m_voxelInterestReuseAgeLastFrame;
    m_stats.backlogVoxelEnqueuedLastFrame = m_backlogVoxelEnqueuedLastFrame;
    m_stats.backlogVoxelCarriedLastFrame = m_backlogVoxelCarriedLastFrame;
    m_stats.backlogVoxelPumpedLastFrame = m_backlogVoxelPumpedLastFrame;
    m_stats.backlogVoxelResidentSkipLastFrame = m_backlogVoxelResidentSkipLastFrame;
    m_stats.generateVoxelAvgMsLastFrame =
        m_generatedVoxelTimingCountLastFrame > 0u
            ? m_generatedVoxelMsAccumLastFrame / static_cast<float>(m_generatedVoxelTimingCountLastFrame)
            : 0.0f;
    m_stats.generateVoxelMaxMsLastFrame = m_generatedVoxelMaxMsLastFrame;
    uint32_t criticalMissing = 0;
    const uint32_t criticalRingCount = std::min<uint32_t>(2u, m_stats.voxelRingCount);
    for (uint32_t ring = 0u; ring < criticalRingCount; ++ring) {
        criticalMissing += m_stats.missingInterestedVoxelBricksByRing[ring];
    }
    m_stats.visibleCriticalMissingVoxelBricks = criticalMissing;
    m_stats.nonCriticalMissingVoxelBricks =
        m_stats.missingInterestedVoxelBricks > criticalMissing
            ? m_stats.missingInterestedVoxelBricks - criticalMissing
            : 0u;
}

} // namespace VENPOD::Simulation

