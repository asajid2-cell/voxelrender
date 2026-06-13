#pragma once

#include "SparseTerrainGenerator.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

class SparseEditStore;
struct SparseSurfaceGpuSnapshot;

inline constexpr uint32_t SPARSE_CLIPMAP_MAX_STATS_RINGS = 8u;

struct SparseClipmapConfig {
    bool enabled = true;
    float startDistance = 480.0f;
    float endDistance = 4200.0f;
    float minCellSize = 16.0f;
    float nearExitPadding = 8.0f;
    uint32_t ringCount = 4;
    // Ring cell-size growth per ring. 2.0 = original doubling (cells 4/8/16/32 ->
    // the visible mid coarsens to 16-32u jaggy blocks). A gentler factor (~1.4)
    // with more rings keeps the visible mid much finer (4/6/8/11..) for ~the same
    // brick budget -> closer to near-voxel fidelity (TANDEM mid-quality work).
    float ringGrowthFactor = 2.0f;
    bool heightClipmapEnabled = true;
    uint32_t tileRadius = 2;
    uint32_t tileSampleSide = 33;
    uint32_t maxTiles = 128;
    bool voxelClipmapEnabled = true;
    uint32_t voxelBrickRadiusXz = 2;
    uint32_t voxelBrickRadiusY = 1;
    uint32_t maxVoxelBricks = 512;
    uint32_t voxelInterestCapacityPercent = 75;
    float motionLookaheadMinSpeed = 64.0f;
    uint32_t motionLookaheadSteps = 3;
    uint32_t interestUpdateIntervalFrames = 1;
    bool footprintInterestSignature = false;
    bool backlogAwarePump = false;
    float pumpBudgetMs = 0.0f;
    // Phase 1 (stable 30): hard, unbypassable per-pump TIME budget (ms). Unlike
    // pumpBudgetMs (which the coverage-emergency path sets to 0 to catch up ->
    // 200ms+ freezes in dense terrain), this is ALWAYS enforced regardless of
    // coverage. A time budget (not a count cap) is required because per-brick cost
    // varies ~20x (cheap flat terrain vs dense geometry). Trades brief lower-LOD lag
    // in dense regions for stable frame time. 0 = disabled.
    float voxelPumpHardBudgetMs = 0.0f;
    bool drainReuseDiagnostics = false;
    bool fallbackValidityClassifier = false;
    bool fallbackContractDiagnostics = false;
    bool farSvoFallbackProof = false;
    bool asyncNoncriticalGeneration = false;
    bool asyncVisibleCriticalGeneration = false;
    uint32_t asyncNoncriticalGenerationQueueMax = 256;
    uint32_t asyncNoncriticalGenerationMaxEnqueuePerFrame = 16;
    uint32_t asyncNoncriticalGenerationMaxApplyPerFrame = 16;
    uint32_t asyncVisibleCriticalGenerationMaxEnqueuePerFrame = 16;
    uint32_t asyncVisibleCriticalGenerationMaxApplyPerFrame = 16;
    bool voxelInterestDetail = false;
    bool voxelInterestSignatureReuse = false;
    uint32_t voxelInterestSignatureReuseMaxAgeFrames = 1;
    // Frame-budgeted (incremental) voxel interest rebuild. When the camera crosses
    // a footprint cell the signature reuse fast path is bypassed and the full
    // per-ring candidate scan runs, spiking 'clip' to 20-75ms. Spreading that
    // rebuild over a few frames (process only N rings per cross frame, carry the
    // remaining rings' coords from the prior interest set) converts the one-frame
    // spike into N small frames. This NEVER drops coverage: the camera moved at
    // most ~1 cell, so the carried-over rings are a strict superset-minus-edge of
    // the correct set and stay fully resident during the spread. 0 == disabled
    // (atomic full rebuild every cross, the legacy behaviour).
    uint32_t voxelInterestRebuildRingsPerFrame = 0;
    bool sharedVoxelColumnCache = false;
    bool directVoxelFootprintColumns = false;
    bool parallelWorkerColumnCache = false;
    bool parallelVoxelPump = false;
    bool parallelVoxelPumpPersistentWorkers = false;
    uint32_t parallelVoxelPumpMaxWorkers = 4;
    uint32_t parallelVoxelPumpMinBricks = 8;
    // Parallel HEIGHT tile pump. GenerateTile runs ~tileSampleSide^2 heavy HeightAt
    // evaluations per tile; a cell-cross bursts many tiles and they generate
    // single-threaded in one frame (measured ~6-25ms, the dominant clip-section pump
    // spike). Fanning the per-tile HeightAt work across workers is coverage-neutral
    // (same tiles, same budget). Independent of parallelVoxelPump so the height win
    // ships without changing voxel-pump behaviour. Reuses parallelVoxelPumpMaxWorkers
    // for worker count. Height tiles are far more expensive than voxel bricks (~3ms/tile)
    // so the serial-fallback threshold is its OWN small value below -- even the measured
    // 4-6 tile cross burst is worth fanning across workers.
    bool parallelHeightPump = true;
    uint32_t parallelHeightPumpMinTiles = 2;
    // Phase 1: when true, the mid-voxel clipmap fills its sample pool via the GPU
    // compute shader (CS_GenerateMidVoxelBricks) instead of CPU GenerateVoxel-
    // BrickPayload. The CPU pump still owns residency/metadata/lookup; only the
    // expensive per-voxel SAMPLES move to the GPU. Edited bricks fall back to the
    // CPU fill (v1). Env: VENPOD_SPARSE_MID_CLIPMAP_GPU_GENERATION.
    bool enableGpuMidVoxelGeneration = false;
    uint32_t seed = 12345u;
};

struct SparseClipmapRing {
    float startDistance = 0.0f;
    float endDistance = 0.0f;
    float cellSize = 0.0f;
};

struct SparseClipmapTransitionMetadata {
    float startDistance = 0.0f;
    float endDistance = 0.0f;
    float farHandoffDistance = 0.0f;
    float minCellSize = 0.0f;
    bool enabled = false;
};

class SparseClipmapPolicy {
public:
    explicit SparseClipmapPolicy(const SparseClipmapConfig& config = {});

    const SparseClipmapConfig& Config() const { return m_config; }
    bool IsEnabled() const;
    float TransitionStartAfterNearExit(float nearExitDistance) const;
    float BackgroundStartAfterNearVolumeExit(float nearVolumeExitDistance) const;
    float FarLayerStartAfterBackground(float backgroundStartDistance) const;
    float MissingNearPageBackgroundStart(
        float firstMissingDistance,
        float nearVolumeExitDistance,
        float missingPagePadding = 24.0f) const;
    bool AllowsBackgroundForMissingNearPage(float firstMissingDistance, float nearVolumeExitDistance) const;
    bool OwnsRaySegment(float segmentStartDistance, float segmentEndDistance, float nearExitDistance) const;
    float CellSizeForDistance(float distanceFromCamera) const;
    std::vector<SparseClipmapRing> BuildRings() const;
    SparseClipmapTransitionMetadata BuildTransitionMetadata() const;
    SparseClipmapTransitionMetadata BuildTransitionMetadataAfterNearExit(float nearVolumeExitDistance) const;

private:
    SparseClipmapConfig m_config;
};

struct SparseClipmapTileCoord {
    int32_t ring = 0;
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const SparseClipmapTileCoord& other) const {
        return ring == other.ring && x == other.x && z == other.z;
    }
};

struct SparseClipmapTileCoordHash {
    size_t operator()(const SparseClipmapTileCoord& coord) const noexcept;
};

struct SparseClipmapTileRecord {
    SparseClipmapTileCoord coord;
    int32_t originX = 0;
    int32_t originZ = 0;
    float cellSize = 16.0f;
    uint32_t slot = UINT32_MAX;
    uint32_t lastTouchedFrame = 0;
};

struct SparseClipmapSampleRange {
    uint32_t startSlot = 0;
    uint32_t slotCount = 0;
};

// Phase 1: a single GPU mid-voxel brick generation request. Mirrors the HLSL
// BrickGenRequest / C++ MidVoxelBrickGenRequest (32 bytes). The CPU pump decides
// origin/cellSize/destSlot; the GPU CS fills the brick's voxel samples.
struct SparseMidVoxelGpuGenRequest {
    int32_t originX = 0;
    int32_t originY = 0;
    int32_t originZ = 0;
    int32_t cellSize = 1;
    uint32_t destSlot = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};

struct SparseClipmapGpuSnapshot {
    std::vector<uint32_t> metadata;
    std::vector<uint32_t> lookup;
    std::vector<uint32_t> samples;
    std::vector<uint32_t> voxelMetadata;
    std::vector<uint32_t> voxelLookup;
    std::vector<uint32_t> voxelSamples;
    std::vector<SparseClipmapSampleRange> heightSampleRanges;
    std::vector<SparseClipmapSampleRange> voxelSampleRanges;
    // Phase 1: when GPU mid-voxel generation is enabled, the resident bricks whose
    // samples must be (re)generated on the GPU this snapshot. voxelSamples is left
    // empty for these (the CPU never packs them); the dispatch fills the pool.
    std::vector<SparseMidVoxelGpuGenRequest> voxelGpuGenRequests;
    uint32_t tileCount = 0;
    uint32_t tileSampleSide = 0;
    uint32_t lookupCapacity = 0;
    uint32_t heightSamplePayloadStartSlot = 0;
    uint32_t heightDirtyStartSlot = 0;
    uint32_t heightDirtySlotCount = 0;
    uint32_t voxelBrickCount = 0;
    uint32_t voxelLookupCapacity = 0;
    uint32_t voxelSamplePayloadStartSlot = 0;
    uint32_t voxelDirtyStartSlot = 0;
    uint32_t voxelDirtySlotCount = 0;
    uint32_t frameIndex = 0;
};

struct SparseMidHeightSurfaceBuildConfig {
    uint32_t maxFaces = 0;
    uint32_t maxTiles = 0;
    uint32_t terraceStep = 1;
    uint32_t lodBaseMerge = 1;
    uint32_t lodMaxMerge = 4;
    bool lodEnabled = true;
    bool emitWater = true;
    bool distanceCull = true;
    bool frustumCull = false;
    float cameraX = 0.0f;
    float cameraY = 0.0f;
    float cameraZ = 0.0f;
    // Camera height ABOVE LOCAL TERRAIN (not world Y): drives the altitude-corrected
    // min-distance so the mesh meets the near raster's actual ground footprint.
    float cameraHeightAboveTerrain = 0.0f;
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    float forwardZ = 1.0f;
    float rightX = 1.0f;
    float rightY = 0.0f;
    float rightZ = 0.0f;
    float upX = 0.0f;
    float upY = 1.0f;
    float upZ = 0.0f;
    float fovYRadians = 1.04719755f;
    float aspectRatio = 1.7777778f;
    float minDistance = 0.0f;
    float maxDistance = 9000.0f;
    float cullPadding = 128.0f;
};

struct SparseClipmapCacheStats {
    uint32_t residentTiles = 0;
    uint32_t queuedTiles = 0;
    uint32_t interestedTiles = 0;
    uint32_t missingInterestedTiles = 0;
    uint32_t generatedTilesLastFrame = 0;
    uint32_t evictedTilesLastFrame = 0;
    uint32_t dirtySerial = 0;
    uint32_t snapshotTiles = 0;
    uint32_t residentVoxelBricks = 0;
    uint32_t residentVoxelNonAirSamples = 0;
    uint32_t residentVoxelSurfaceSamples = 0;
    uint32_t queuedVoxelBricks = 0;
    uint32_t interestedVoxelBricks = 0;
    uint32_t missingInterestedVoxelBricks = 0;
    uint32_t voxelRingCount = 0;
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> residentVoxelBricksByRing{};
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> queuedVoxelBricksByRing{};
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> interestedVoxelBricksByRing{};
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> missingInterestedVoxelBricksByRing{};
    uint32_t generatedVoxelBricksLastFrame = 0;
    uint32_t evictedVoxelBricksLastFrame = 0;
    uint32_t heightInterestAnchors = 0;
    uint32_t voxelInterestAnchors = 0;
    uint32_t interestReusedLastFrame = 0;
    uint32_t voxelInterestRingsRebuiltLastFrame = 0;
    uint32_t voxelInterestBudgetedRebuildsLastFrame = 0;
    float pumpHeightMsLastFrame = 0.0f;
    float pumpVoxelMsLastFrame = 0.0f;
    uint32_t backlogAwarePumpActive = 0;
    float pumpBudgetMs = 0.0f;
    uint32_t pumpBudgetHitLastFrame = 0;
    uint32_t backlogHeightBricks = 0;
    uint32_t backlogVoxelBricks = 0;
    uint32_t backlogVoxelOldestAge = 0;
    uint32_t backlogVoxelMaxAge = 0;
    uint32_t prunedVoxelBacklogLastFrame = 0;
    uint32_t visibleCriticalMissingVoxelBricks = 0;
    uint32_t nonCriticalMissingVoxelBricks = 0;
    uint32_t newlyInterestedTilesLastFrame = 0;
    uint32_t newlyInterestedVoxelBricksLastFrame = 0;
    uint32_t noLongerInterestedTilesLastFrame = 0;
    uint32_t noLongerInterestedVoxelBricksLastFrame = 0;
    uint32_t residentInterestedTiles = 0;
    uint32_t residentInterestedVoxelBricks = 0;
    uint32_t reusedInterestedTilesLastFrame = 0;
    uint32_t reusedInterestedVoxelBricksLastFrame = 0;
    float voxelInterestLineMsLastFrame = 0.0f;
    float voxelInterestAnchorMsLastFrame = 0.0f;
    float voxelInterestSortEmitMsLastFrame = 0.0f;
    float voxelInterestBacklogMsLastFrame = 0.0f;
    float voxelInterestDiagnosticsMsLastFrame = 0.0f;
    uint32_t voxelInterestCandidatesLastFrame = 0;
    uint32_t voxelInterestCandidateAttemptsLastFrame = 0;
    uint32_t voxelInterestCandidateDuplicateHitsLastFrame = 0;
    uint32_t voxelInterestCandidateScoreUpdatesLastFrame = 0;
    uint32_t voxelInterestCandidateMaxRingUniqueLastFrame = 0;
    uint32_t voxelInterestCandidateMaxRingAttemptsLastFrame = 0;
    uint32_t voxelInterestLineCandidateAttemptsLastFrame = 0;
    uint32_t voxelInterestLineCandidateDuplicateHitsLastFrame = 0;
    uint32_t voxelInterestLineCandidateScoreUpdatesLastFrame = 0;
    uint32_t voxelInterestAnchorTerrainCandidateAttemptsLastFrame = 0;
    uint32_t voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame = 0;
    uint32_t voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame = 0;
    uint32_t voxelInterestAnchorFootprintCandidateAttemptsLastFrame = 0;
    uint32_t voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame = 0;
    uint32_t voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame = 0;
    uint32_t voxelInterestAnchorCameraCandidateAttemptsLastFrame = 0;
    uint32_t voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame = 0;
    uint32_t voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame = 0;
    uint32_t voxelInterestEmittedLastFrame = 0;
    uint32_t voxelInterestReusedLastFrame = 0;
    uint32_t voxelInterestReuseAgeLastFrame = 0;
    uint32_t backlogVoxelEnqueuedLastFrame = 0;
    uint32_t backlogVoxelCarriedLastFrame = 0;
    uint32_t backlogVoxelPumpedLastFrame = 0;
    uint32_t backlogVoxelResidentSkipLastFrame = 0;
    uint32_t backlogVoxelAge0To30 = 0;
    uint32_t backlogVoxelAge31To90 = 0;
    uint32_t backlogVoxelAge91To180 = 0;
    uint32_t backlogVoxelAge181Plus = 0;
    float generateVoxelAvgMsLastFrame = 0.0f;
    float generateVoxelMaxMsLastFrame = 0.0f;
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> generatedVoxelBricksByRingLastFrame{};
    uint32_t fallbackValidityClassifierActive = 0;
    uint32_t asyncNoncriticalGenerationActive = 0;
    uint32_t asyncNoncriticalGenerationQueueDepth = 0;
    uint32_t asyncNoncriticalGenerationResultDepth = 0;
    uint32_t asyncNoncriticalGenerationPending = 0;
    uint32_t asyncNoncriticalGenerationEnqueuedLastFrame = 0;
    uint32_t asyncNoncriticalGenerationCompletedLastFrame = 0;
    uint32_t asyncNoncriticalGenerationAppliedLastFrame = 0;
    uint32_t asyncNoncriticalGenerationDiscardedLastFrame = 0;
    uint32_t asyncNoncriticalGenerationDuplicateSyncLastFrame = 0;
    uint32_t asyncVisibleCriticalGenerationEnqueuedLastFrame = 0;
    uint32_t asyncVisibleCriticalGenerationCompletedLastFrame = 0;
    uint32_t asyncVisibleCriticalGenerationAppliedLastFrame = 0;
    uint32_t asyncVisibleCriticalGenerationDiscardedLastFrame = 0;
    uint32_t asyncVisibleCriticalGenerationDuplicateSyncLastFrame = 0;
    uint32_t asyncVisibleReservationAppliedLastFrame = 0;
    uint32_t asyncVisibleReservationApplyDeferredLastFrame = 0;
    uint32_t asyncVisibleReservationApplyLimitLastFrame = UINT32_MAX;
    float asyncNoncriticalGenerationWorkerMsLastFrame = 0.0f;
    float asyncNoncriticalGenerationApplyMsLastFrame = 0.0f;
    uint32_t predictedVisibleAdmissionSamplesLastFrame = 0;
    float predictedVisibleAdmissionSnapshotMsLastFrame = 0.0f;
    float predictedVisibleAdmissionRebuildMsLastFrame = 0.0f;
    float predictedVisibleAdmissionRestoreMsLastFrame = 0.0f;
    float predictedVisibleAdmissionQueueMsLastFrame = 0.0f;
    uint32_t missingFallbackValidVoxelBricks = 0;
    uint32_t missingFallbackInvalidVoxelBricks = 0;
    uint32_t missingFallbackUnknownVoxelBricks = 0;
    uint32_t highAltitudeCurrentInterestVoxelBricks = 0;
    uint32_t highAltitudeFallbackValidVoxelBricks = 0;
    uint32_t highAltitudeFallbackInvalidVoxelBricks = 0;
    uint32_t highAltitudeFallbackUnknownVoxelBricks = 0;
    uint32_t finerLodFallbackAvailableVoxelBricks = 0;
    uint32_t lowerLodFallbackAvailableVoxelBricks = 0;
    uint32_t farSvoFallbackAvailableVoxelBricks = 0;
    uint32_t waterFallbackAvailableVoxelBricks = 0;
    uint32_t skyFallbackAvailableVoxelBricks = 0;
    uint32_t oldResidentFallbackAvailableVoxelBricks = 0;
    uint32_t fallbackRejectNoLowerLod = 0;
    uint32_t fallbackRejectFarSvoOutOfDomain = 0;
    uint32_t fallbackRejectShorelineMixedCell = 0;
    uint32_t fallbackRejectNearCamera = 0;
    uint32_t fallbackRejectScreenCritical = 0;
    uint32_t fallbackRejectUnknownOwner = 0;
    uint32_t asyncEligibleVoxelBricks = 0;
    uint32_t syncRequiredVoxelBricks = 0;
    uint32_t fallbackContractDiagnosticsActive = 0;
    uint32_t farSvoFallbackProofActive = 0;
    uint32_t contractCpuProvableValid = 0;
    uint32_t contractCpuProvableInvalid = 0;
    uint32_t contractCpuUnknown = 0;
    uint32_t contractShaderOnlyUnknown = 0;
    uint32_t contractFinerValid = 0;
    uint32_t contractFinerInvalid = 0;
    uint32_t contractFinerUnknown = 0;
    uint32_t contractCoarserValid = 0;
    uint32_t contractCoarserRejectedHighAlt = 0;
    uint32_t contractCoarserRejectedRayAngle = 0;
    uint32_t contractCoarserRejectedError = 0;
    uint32_t contractCoarserMissing = 0;
    uint32_t contractFarSvoDomainValid = 0;
    uint32_t contractFarSvoDomainInvalid = 0;
    uint32_t contractFarSvoMaterialValid = 0;
    uint32_t contractFarSvoMaterialUnknown = 0;
    uint32_t contractFarSvoRejected = 0;
    uint32_t contractWaterValid = 0;
    uint32_t contractWaterRejectedShoreline = 0;
    uint32_t contractWaterUnknown = 0;
    uint32_t contractSkyValid = 0;
    uint32_t contractSkyUnknown = 0;
    uint32_t contractPreviousResidentValid = 0;
    uint32_t contractPreviousResidentStale = 0;
    uint32_t contractPublicReadinessRejected = 0;
    uint32_t contractEditStampRejected = 0;
    uint32_t contractMixedOwnerUnknown = 0;
    uint32_t contractNearCameraRejected = 0;
    uint32_t contractScreenCriticalRejected = 0;
    uint32_t contractCoverageEmergencyRejected = 0;
    uint32_t contractHighAltRejected = 0;
    uint32_t contractValidButNotDeferredReason = 0;
    uint32_t contractInvalidReasonTop1 = 0;
    uint32_t contractUnknownReasonTop1 = 0;
    uint32_t sharedVoxelColumnCacheActive = 0;
    uint32_t sharedVoxelColumnCacheEntries = 0;
    uint32_t sharedVoxelColumnHeightHitsLastFrame = 0;
    uint32_t sharedVoxelColumnHeightMissesLastFrame = 0;
    uint32_t sharedVoxelColumnReliefHitsLastFrame = 0;
    uint32_t sharedVoxelColumnReliefMissesLastFrame = 0;
    uint32_t directVoxelFootprintColumnsActive = 0;
    uint32_t parallelWorkerColumnCacheActive = 0;
    uint32_t parallelWorkerColumnCacheEntries = 0;
    uint32_t parallelWorkerColumnHeightHitsLastFrame = 0;
    uint32_t parallelWorkerColumnHeightMissesLastFrame = 0;
    uint32_t parallelWorkerColumnReliefHitsLastFrame = 0;
    uint32_t parallelWorkerColumnReliefMissesLastFrame = 0;
    uint32_t parallelVoxelPumpActive = 0;
    uint32_t parallelVoxelPumpBricksLastFrame = 0;
    uint32_t parallelVoxelPumpWorkersLastFrame = 0;
    float parallelVoxelPumpWallMsLastFrame = 0.0f;
    uint32_t visiblePriorityVoxelBricks = 0;
    uint32_t cachePriorityVoxelBricks = 0;
    uint32_t queuedVisiblePriorityVoxelBricks = 0;
    uint32_t queuedCachePriorityVoxelBricks = 0;
    uint32_t visiblePriorityBacklogMaxAge = 0;
    uint32_t cachePriorityBacklogMaxAge = 0;
    uint32_t visiblePriorityTaggedLastFrame = 0;
    uint32_t visiblePriorityPrioritizedLastFrame = 0;
    uint32_t asyncVisibleReservations = 0;
    uint32_t asyncVisibleReservationsDue = 0;
    uint32_t asyncVisibleReservationsOverdue = 0;
    uint32_t asyncVisibleReservationBacklogMaxAge = 0;
};

struct SparseClipmapResidencyMetadata {
    float heightCoverageRatio = 0.0f;
    float voxelCoverageRatio = 0.0f;
    uint32_t residentHeightTiles = 0;
    uint32_t residentVoxelBricks = 0;
};

SparseClipmapResidencyMetadata BuildClipmapResidencyMetadata(const SparseClipmapCacheStats& stats);

struct SparseVoxelClipmapCoord {
    int32_t ring = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const SparseVoxelClipmapCoord& other) const {
        return ring == other.ring && x == other.x && y == other.y && z == other.z;
    }
};

struct SparseVoxelClipmapCoordHash {
    size_t operator()(const SparseVoxelClipmapCoord& coord) const noexcept;
};

struct SparseClipmapFarSvoFallbackMetadata {
    bool ready = false;
    int32_t pageRadius = 0;
    float pageSize = 0.0f;
    float rootMinY = 0.0f;
    float pageCoverageRatio = 0.0f;
};

class SparseClipmapTileCache {
public:
    ~SparseClipmapTileCache();

    bool Initialize(const SparseClipmapConfig& config);
    void UpdateInterest(
        float cameraX,
        float cameraY,
        float cameraZ,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        float forwardX = 0.0f,
        float forwardY = 0.0f,
        float forwardZ = 0.0f,
        float velocityX = 0.0f,
        float velocityY = 0.0f,
        float velocityZ = 0.0f,
        float predictionSeconds = 0.0f);
    uint32_t PumpGeneration(uint32_t maxTiles, uint32_t frameIndex, const SparseClipmapPolicy& policy);
    uint32_t PumpGeneration(
        uint32_t maxHeightTiles,
        uint32_t maxVoxelBricks,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    uint32_t PumpGenerationSplitVisiblePriority(
        uint32_t maxHeightTiles,
        uint32_t maxVisibleVoxelBricks,
        uint32_t maxCacheVoxelBricks,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    uint32_t PumpVoxelGenerationForRing(
        uint32_t ring,
        uint32_t maxVoxelBricks,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        uint32_t minEvictRing = UINT32_MAX);
    bool QueueVoxelRenderFeedbackCoord(
        const SparseVoxelClipmapCoord& coord,
        uint32_t frameIndex);
    bool IsVoxelCoordResident(const SparseVoxelClipmapCoord& coord) const;
    bool HasCoarserVoxelParentForCoord(const SparseVoxelClipmapCoord& coord) const;
    void CollectMissingVoxelInterest(
        std::vector<SparseVoxelClipmapCoord>& outCoords,
        uint32_t maxCoords = UINT32_MAX) const;
    uint32_t SetVisiblePriorityVoxelCoords(
        const std::vector<SparseVoxelClipmapCoord>& priorityCoords,
        uint32_t frameIndex,
        bool prioritizeQueue = true);
    uint32_t QueuePredictedVisibleVoxelInterest(
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
        uint32_t deadlineFrame = 0u,
        uint32_t sampleIndex = 0u);
    uint32_t CollectPredictedVisibleVoxelInterestForDebug(
        std::vector<SparseVoxelClipmapCoord>& outCoords,
        float cameraX,
        float cameraY,
        float cameraZ,
        float forwardX,
        float forwardY,
        float forwardZ,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        uint32_t maxCoords);
    uint32_t CollectPredictedVisibleVoxelInterestPureForDebug(
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
        std::vector<SparseVoxelClipmapCoord>* outResidentTouchCoords = nullptr) const;
    uint32_t QueueAsyncVisibleReservationVoxelCoords(
        const std::vector<SparseVoxelClipmapCoord>& reservationCoords,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        uint32_t deadlineFrame = 0u,
        uint32_t sampleIndex = 0u,
        uint32_t staleFrames = 24u);
    uint32_t CountVisiblePriorityMatches(
        const std::vector<SparseVoxelClipmapCoord>& coords) const;
    uint32_t CountAsyncVisibleReservationMatches(
        const std::vector<SparseVoxelClipmapCoord>& coords) const;
    uint32_t PrioritizeVoxelGenerationCoords(
        const std::vector<SparseVoxelClipmapCoord>& priorityCoords);
    uint32_t ApplyAsyncNoncriticalVoxelGenerationCompletions(
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        uint32_t maxNoncriticalApply = UINT32_MAX,
        uint32_t maxVisibleCriticalApply = UINT32_MAX,
        uint32_t maxVisibleReservationApply = UINT32_MAX);
    bool BuildGpuSnapshot(
        SparseClipmapGpuSnapshot& outSnapshot,
        bool includeHeightLayer = true,
        bool includeVoxelLayer = true) const;
    // Mark the GPU-gen sample slots emitted in `snapshot.voxelGpuGenRequests` as uploaded
    // so they are not re-dispatched on the next snapshot. Call ONLY after the upload +
    // compute dispatch for this snapshot actually succeed (so a rejected upload retries).
    void MarkVoxelGpuSamplesUploaded(const SparseClipmapGpuSnapshot& snapshot);
    bool BuildMidHeightSurfaceSnapshot(
        SparseSurfaceGpuSnapshot& outSnapshot,
        const SparseMidHeightSurfaceBuildConfig& buildConfig = {}) const;
    void SetEditStore(const SparseEditStore* edits);
    void SetFarSvoFallbackMetadata(const SparseClipmapFarSvoFallbackMetadata& metadata);
    // sinceRevision: only overlays touched after this global edit revision are
    // processed (0 = all, e.g. after load). Pass the previously-seen
    // RevisionSerial so continuous painting costs O(new strokes), not
    // O(every overlay ever made) — the edit-hitch fix. Invalidated bricks are
    // QUEUED; call PumpEditedBrickRegens each frame to drain them on a budget.
    uint32_t InvalidateEditedOverlays(
        const SparseEditStore& edits,
        const SparseClipmapPolicy& policy,
        uint64_t sinceRevision = 0);
    // Budgeted regeneration of edit-invalidated mid bricks (a full CPU brick
    // rebuild is ~2.5ms; draining a couple per frame keeps editing hitch-free
    // while the mid trails the stroke by a few invisible frames).
    uint32_t PumpEditedBrickRegens(const SparseClipmapPolicy& policy, uint32_t maxBricks);
    // Height-tile counterpart: queue resident height tiles whose XZ footprint is
    // touched by overlays changed since `sinceRevision`. These feed the mid-mesh
    // raster, the layer that was drawing stale procedural ground over carves.
    uint32_t InvalidateEditedHeightTiles(
        const SparseEditStore& edits,
        const SparseClipmapPolicy& policy,
        uint64_t sinceRevision = 0);
    // Drain a budget of edit-invalidated height tiles, regenerating each with the
    // edit-aware sampler. Bumps m_heightDirtySerial at most every few frames while
    // draining (and once on drain) so the full mid-mesh rebuild is coalesced.
    uint32_t PumpEditedHeightTileRegens(const SparseClipmapPolicy& policy, uint32_t maxTiles);

    // DEV-ONLY (MidVoxelGpuGenPoc parity harness): generate the REAL pristine
    // procedural brick for a coord on this (unedited) cache and return its packed
    // voxels + origin/cellSize. Wraps the private GenerateVoxelBrickPayload; the
    // VoxelBrickPayload struct is private so results are returned via out-params.
    // Edited-overlay branches are inert here because a test cache has no edits.
    bool GenerateVoxelBrickPayloadForTest(
        const SparseVoxelClipmapCoord& coord,
        const SparseClipmapPolicy& policy,
        std::vector<uint32_t>& outVoxels,
        int32_t& outOriginX,
        int32_t& outOriginY,
        int32_t& outOriginZ,
        int32_t& outCellSize);

    const SparseClipmapCacheStats& GetStats() const { return m_stats; }
    uint32_t DirtySerial() const { return m_dirtySerial; }
    uint32_t HeightDirtySerial() const { return m_heightDirtySerial; }
    uint32_t VoxelDirtySerial() const { return m_voxelDirtySerial; }
    // L3 motion guard: XZ distance from the camera to the nearest INTERESTED height
    // tile that is not yet resident (FLT_MAX when full coverage). Fed to the renderer
    // per frame so the shader can suppress the bare far-water fallback beyond the
    // streamed region (it painted sea over un-streamed dry mountains at speed).
    float NearestMissingHeightTileDistance(
        float cameraX,
        float cameraZ,
        const SparseClipmapPolicy& policy) const;
    void ClearHeightDirtyRange();
    void ClearVoxelDirtyRange();

private:
    struct InterestSignature {
        int32_t cameraX = 0;
        int32_t cameraY = 0;
        int32_t cameraZ = 0;
        int32_t forwardX = 0;
        int32_t forwardY = 0;
        int32_t forwardZ = 0;
        int32_t velocityX = 0;
        int32_t velocityY = 0;
        int32_t velocityZ = 0;
        uint32_t predictionMillis = 0;
        uint32_t startDistance = 0;
        uint32_t endDistance = 0;
        uint32_t minCellSize = 0;
        uint32_t tileRadius = 0;
        uint32_t tileSampleSide = 0;
        uint32_t ringCount = 0;
        uint32_t heightClipmapEnabled = 0;
        uint32_t voxelClipmapEnabled = 0;
        uint32_t voxelBrickRadiusXz = 0;
        uint32_t voxelBrickRadiusY = 0;
        uint32_t voxelInterestCapacityPercent = 0;
        uint32_t motionLookaheadMinSpeed = 0;
        uint32_t motionLookaheadSteps = 0;
        uint32_t interestUpdateIntervalFrames = 1;
        uint32_t footprintInterestSignature = 0;
        uint32_t backlogAwarePump = 0;
        uint32_t pumpBudgetMs = 0;
        uint32_t drainReuseDiagnostics = 0;
        uint32_t fallbackValidityClassifier = 0;
        uint32_t fallbackContractDiagnostics = 0;
        uint32_t farSvoFallbackProof = 0;
        uint32_t asyncNoncriticalGeneration = 0;
        uint32_t asyncVisibleCriticalGeneration = 0;
        uint32_t asyncNoncriticalGenerationQueueMax = 0;
        uint32_t asyncNoncriticalGenerationMaxEnqueuePerFrame = 0;
        uint32_t asyncNoncriticalGenerationMaxApplyPerFrame = 0;
        uint32_t asyncVisibleCriticalGenerationMaxEnqueuePerFrame = 0;
        uint32_t asyncVisibleCriticalGenerationMaxApplyPerFrame = 0;
        uint32_t voxelInterestDetail = 0;
        uint32_t voxelInterestSignatureReuse = 0;
        uint32_t voxelInterestSignatureReuseMaxAgeFrames = 0;
        uint32_t sharedVoxelColumnCache = 0;
        uint32_t directVoxelFootprintColumns = 0;
        uint32_t parallelWorkerColumnCache = 0;
        uint32_t parallelVoxelPump = 0;
        uint32_t parallelVoxelPumpPersistentWorkers = 0;
        uint32_t parallelVoxelPumpMaxWorkers = 0;
        uint32_t parallelVoxelPumpMinBricks = 0;

        bool operator==(const InterestSignature& other) const {
            return cameraX == other.cameraX &&
                cameraY == other.cameraY &&
                cameraZ == other.cameraZ &&
                forwardX == other.forwardX &&
                forwardY == other.forwardY &&
                forwardZ == other.forwardZ &&
                velocityX == other.velocityX &&
                velocityY == other.velocityY &&
                velocityZ == other.velocityZ &&
                predictionMillis == other.predictionMillis &&
                startDistance == other.startDistance &&
                endDistance == other.endDistance &&
                minCellSize == other.minCellSize &&
                tileRadius == other.tileRadius &&
                tileSampleSide == other.tileSampleSide &&
                ringCount == other.ringCount &&
                heightClipmapEnabled == other.heightClipmapEnabled &&
                voxelClipmapEnabled == other.voxelClipmapEnabled &&
                voxelBrickRadiusXz == other.voxelBrickRadiusXz &&
                voxelBrickRadiusY == other.voxelBrickRadiusY &&
                voxelInterestCapacityPercent == other.voxelInterestCapacityPercent &&
                motionLookaheadMinSpeed == other.motionLookaheadMinSpeed &&
                motionLookaheadSteps == other.motionLookaheadSteps &&
                interestUpdateIntervalFrames == other.interestUpdateIntervalFrames &&
                footprintInterestSignature == other.footprintInterestSignature &&
                backlogAwarePump == other.backlogAwarePump &&
                pumpBudgetMs == other.pumpBudgetMs &&
                drainReuseDiagnostics == other.drainReuseDiagnostics &&
                fallbackValidityClassifier == other.fallbackValidityClassifier &&
                fallbackContractDiagnostics == other.fallbackContractDiagnostics &&
                farSvoFallbackProof == other.farSvoFallbackProof &&
                asyncNoncriticalGeneration == other.asyncNoncriticalGeneration &&
                asyncVisibleCriticalGeneration == other.asyncVisibleCriticalGeneration &&
                asyncNoncriticalGenerationQueueMax == other.asyncNoncriticalGenerationQueueMax &&
                asyncNoncriticalGenerationMaxEnqueuePerFrame ==
                    other.asyncNoncriticalGenerationMaxEnqueuePerFrame &&
                asyncNoncriticalGenerationMaxApplyPerFrame ==
                    other.asyncNoncriticalGenerationMaxApplyPerFrame &&
                asyncVisibleCriticalGenerationMaxEnqueuePerFrame ==
                    other.asyncVisibleCriticalGenerationMaxEnqueuePerFrame &&
                asyncVisibleCriticalGenerationMaxApplyPerFrame ==
                    other.asyncVisibleCriticalGenerationMaxApplyPerFrame &&
                voxelInterestDetail == other.voxelInterestDetail &&
                voxelInterestSignatureReuse == other.voxelInterestSignatureReuse &&
                voxelInterestSignatureReuseMaxAgeFrames == other.voxelInterestSignatureReuseMaxAgeFrames &&
                sharedVoxelColumnCache == other.sharedVoxelColumnCache &&
                directVoxelFootprintColumns == other.directVoxelFootprintColumns &&
                parallelWorkerColumnCache == other.parallelWorkerColumnCache &&
                parallelVoxelPump == other.parallelVoxelPump &&
                parallelVoxelPumpPersistentWorkers == other.parallelVoxelPumpPersistentWorkers &&
                parallelVoxelPumpMaxWorkers == other.parallelVoxelPumpMaxWorkers &&
                parallelVoxelPumpMinBricks == other.parallelVoxelPumpMinBricks;
        }
    };

    struct TilePayload {
        SparseClipmapTileRecord record;
        std::vector<uint32_t> packedSamples;
    };

    struct VoxelColumnSample {
        int32_t worldX = 0;
        int32_t worldZ = 0;
        float height = 0.0f;
        float relief = 0.0f;
        bool reliefValid = false;
    };

    struct VoxelColumnCacheCounters {
        uint32_t heightHits = 0;
        uint32_t heightMisses = 0;
        uint32_t reliefHits = 0;
        uint32_t reliefMisses = 0;
    };

    struct VoxelBrickPayload;

    uint32_t AllocateSlot(const SparseClipmapTileCoord& coord, uint32_t frameIndex);
    void GenerateTile(uint32_t slot, const SparseClipmapPolicy& policy);
    uint32_t PackSample(int32_t worldX, int32_t worldZ, float height) const;
    uint32_t AllocateVoxelSlot(const SparseVoxelClipmapCoord& coord, uint32_t frameIndex);
    uint32_t AllocateVoxelSlotForMinRing(
        const SparseVoxelClipmapCoord& coord,
        uint32_t frameIndex,
        uint32_t minEvictRing);
    void GenerateVoxelBrick(
        uint32_t slot,
        const SparseClipmapPolicy& policy,
        std::unordered_map<uint64_t, VoxelColumnSample>* externalColumnCache = nullptr,
        VoxelColumnCacheCounters* externalColumnCacheCounters = nullptr);
    void GenerateVoxelBrickPayload(
        VoxelBrickPayload& brick,
        const SparseClipmapPolicy& policy,
        std::unordered_map<uint64_t, VoxelColumnSample>* externalColumnCache = nullptr,
        VoxelColumnCacheCounters* externalColumnCacheCounters = nullptr);
    bool GenerateVoxelBricksWithPersistentWorkers(
        const std::vector<uint32_t>& slots,
        const SparseClipmapPolicy& policy,
        bool useWorkerColumnCache,
        std::vector<float>& elapsedMs,
        std::vector<VoxelColumnCacheCounters>& workerColumnCounters,
        std::vector<uint32_t>& workerColumnEntries,
        uint32_t workerCount);
    void StartPersistentVoxelPumpWorkers(uint32_t workerCount);
    void StopPersistentVoxelPumpWorkers();
    void PersistentVoxelPumpWorkerLoop(uint32_t workerIndex);
    void StartAsyncNoncriticalVoxelGenerationWorkerIfNeeded();
    void StopAsyncNoncriticalVoxelGenerationWorker();
    bool TryQueueAsyncNoncriticalVoxelGeneration(
        const SparseVoxelClipmapCoord& coord,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    bool TryQueueAsyncVoxelGeneration(
        const SparseVoxelClipmapCoord& coord,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        bool visibleCritical,
        bool allowVisibleReservation = false,
        bool bypassEnqueueLimit = false);
    uint32_t QueueAsyncVoxelGenerationMatchingPriority(
        bool requireVisiblePriority,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    void PrioritizeAsyncVoxelGenerationQueue();
    bool RemoveQueuedVoxelCoord(const SparseVoxelClipmapCoord& coord);
    void PruneAsyncVisibleReservations(uint32_t frameIndex, uint32_t staleFrames = 24u);
    void UpdateVoxelInterest(
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
        bool allowSignatureReuse = true);
    InterestSignature BuildInterestSignature(
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
        float predictionSeconds) const;
    InterestSignature BuildVoxelInterestSignature(
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
        float predictionSeconds) const;
    void RefreshInterestTouchFrames(uint32_t frameIndex);
    void MarkVoxelSlotDirty(uint32_t slot);
    void MarkHeightSlotDirty(uint32_t slot);
    void RefreshStats(
        uint32_t generatedLastFrame = 0,
        uint32_t evictedLastFrame = 0,
        uint32_t generatedVoxelLastFrame = 0,
        uint32_t evictedVoxelLastFrame = 0);
    void RecordVoxelGenerationTiming(const SparseVoxelClipmapCoord& coord, float elapsedMs);
    uint32_t PumpVoxelGenerationMatchingPriority(
        bool requireVisiblePriority,
        uint32_t maxVoxelBricks,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy,
        uint32_t& evictedVoxel);
    bool HasCompleteFinerVoxelCoverage(const SparseVoxelClipmapCoord& coord) const;
    bool HasCoarserVoxelParent(const SparseVoxelClipmapCoord& coord) const;
    bool MissingBrickWithinFarSvoDomain(const SparseVoxelClipmapCoord& coord) const;

    SparseClipmapConfig m_config;
    SparseClipmapFarSvoFallbackMetadata m_farSvoFallbackMetadata;
    SparseTerrainGenerator m_terrain;
    const SparseEditStore* m_edits = nullptr;
    std::vector<TilePayload> m_tiles;
    std::vector<uint32_t> m_freeSlots;
    std::unordered_map<SparseClipmapTileCoord, uint32_t, SparseClipmapTileCoordHash> m_slotByCoord;
    std::deque<SparseClipmapTileCoord> m_generationQueue;
    std::unordered_set<SparseClipmapTileCoord, SparseClipmapTileCoordHash> m_queuedSet;
    std::unordered_set<SparseClipmapTileCoord, SparseClipmapTileCoordHash> m_interestSet;
    struct VoxelBrickPayload {
        SparseVoxelClipmapCoord coord;
        uint32_t slot = UINT32_MAX;
        uint32_t lastTouchedFrame = 0;
        float cellSize = 16.0f;
        int32_t originX = 0;
        int32_t originY = 0;
        int32_t originZ = 0;
        uint32_t nonAirSamples = 0;
        uint32_t surfaceSamples = 0;
        // Phase 1: true if this brick's voxel SAMPLES are produced on the GPU
        // (CS_GenerateMidVoxelBricks). When set, `voxels` is left empty (the CPU
        // never pays for the per-voxel fill) and BuildGpuSnapshot skips copying
        // samples for this slot, emitting a GPU gen request instead.
        bool gpuGenerated = false;
        // PERF: true once this brick's GPU samples have been (re)generated via a
        // voxelGpuGenRequest. GPU gen is DETERMINISTIC from the brick's coord, and the
        // sample pool is indexed by `slot`, so an unchanged brick keeps valid samples and
        // must NOT be re-dispatched. Without this, BuildGpuSnapshot re-emitted a gen
        // request for ALL ~12k resident bricks on every metadata upload -> full re-dispatch
        // every frame during streaming, starving new-terrain gen and dropping fps. Reset to
        // false in MarkVoxelSlotDirty (any change/slot-reuse) so the brick re-generates.
        bool gpuGenSamplesUploaded = false;
        std::vector<uint32_t> voxels;
    };
    struct AsyncVoxelGenerationRequest {
        SparseVoxelClipmapCoord coord;
        SparseClipmapConfig config;
        uint32_t requestFrame = 0;
        uint64_t editRevision = 0;
        bool visibleCritical = false;
    };
    struct AsyncVoxelGenerationResult {
        SparseVoxelClipmapCoord coord;
        VoxelBrickPayload brick;
        uint32_t requestFrame = 0;
        uint64_t editRevision = 0;
        float workerMs = 0.0f;
        bool visibleCritical = false;
    };
    std::vector<VoxelBrickPayload> m_voxelBricks;
    std::vector<uint32_t> m_freeVoxelSlots;
    std::unordered_map<SparseVoxelClipmapCoord, uint32_t, SparseVoxelClipmapCoordHash> m_voxelSlotByCoord;
    std::deque<SparseVoxelClipmapCoord> m_voxelGenerationQueue;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> m_queuedVoxelSet;
    std::unordered_map<SparseVoxelClipmapCoord, uint32_t, SparseVoxelClipmapCoordHash> m_voxelBacklogFirstFrame;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> m_voxelInterestSet;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> m_visiblePriorityVoxelSet;
    struct AsyncVisibleReservation {
        uint32_t firstFrame = 0;
        uint32_t lastSeenFrame = 0;
        uint32_t deadlineFrame = 0;
        uint32_t sampleIndex = 0;
    };
    std::unordered_map<SparseVoxelClipmapCoord, AsyncVisibleReservation, SparseVoxelClipmapCoordHash>
        m_asyncVisibleReservations;
    std::unordered_map<uint64_t, VoxelColumnSample> m_sharedVoxelColumnCache;
    InterestSignature m_lastInterestSignature;
    bool m_lastInterestSignatureValid = false;
    InterestSignature m_lastVoxelInterestSignature;
    bool m_lastVoxelInterestSignatureValid = false;
    uint32_t m_lastVoxelInterestBuildFrame = 0;
    // Frame-budgeted voxel interest rebuild cursor: the next ring index to refresh
    // on a footprint-cell cross. While >0 the spread is mid-flight (some rings are
    // still carried from the prior set) so the signature must NOT be committed yet.
    uint32_t m_voxelInterestRebuildRingCursor = 0;
    bool m_voxelInterestRebuildInProgress = false;
    uint32_t m_voxelInterestBudgetedRebuildsLastFrame = 0;
    uint32_t m_voxelInterestRingsRebuiltLastFrame = 0;
    uint32_t m_lastInterestUpdateFrame = 0;
    uint32_t m_lastStatsFrame = 0;
    float m_lastCameraYForStats = 0.0f;
    uint32_t m_interestReusedLastFrame = 0;
    uint32_t m_pumpBudgetHitLastFrame = 0;
    uint32_t m_prunedVoxelBacklogLastFrame = 0;
    uint32_t m_newlyInterestedTilesLastFrame = 0;
    uint32_t m_newlyInterestedVoxelBricksLastFrame = 0;
    uint32_t m_noLongerInterestedTilesLastFrame = 0;
    uint32_t m_noLongerInterestedVoxelBricksLastFrame = 0;
    uint32_t m_residentInterestedTilesLastFrame = 0;
    uint32_t m_residentInterestedVoxelBricksLastFrame = 0;
    uint32_t m_reusedInterestedTilesLastFrame = 0;
    uint32_t m_reusedInterestedVoxelBricksLastFrame = 0;
    float m_voxelInterestLineMsLastFrame = 0.0f;
    float m_voxelInterestAnchorMsLastFrame = 0.0f;
    float m_voxelInterestSortEmitMsLastFrame = 0.0f;
    float m_voxelInterestBacklogMsLastFrame = 0.0f;
    float m_voxelInterestDiagnosticsMsLastFrame = 0.0f;
    uint32_t m_voxelInterestCandidatesLastFrame = 0;
    uint32_t m_voxelInterestCandidateAttemptsLastFrame = 0;
    uint32_t m_voxelInterestCandidateDuplicateHitsLastFrame = 0;
    uint32_t m_voxelInterestCandidateScoreUpdatesLastFrame = 0;
    uint32_t m_voxelInterestCandidateMaxRingUniqueLastFrame = 0;
    uint32_t m_voxelInterestCandidateMaxRingAttemptsLastFrame = 0;
    uint32_t m_voxelInterestLineCandidateAttemptsLastFrame = 0;
    uint32_t m_voxelInterestLineCandidateDuplicateHitsLastFrame = 0;
    uint32_t m_voxelInterestLineCandidateScoreUpdatesLastFrame = 0;
    uint32_t m_voxelInterestAnchorTerrainCandidateAttemptsLastFrame = 0;
    uint32_t m_voxelInterestAnchorTerrainCandidateDuplicateHitsLastFrame = 0;
    uint32_t m_voxelInterestAnchorTerrainCandidateScoreUpdatesLastFrame = 0;
    uint32_t m_voxelInterestAnchorFootprintCandidateAttemptsLastFrame = 0;
    uint32_t m_voxelInterestAnchorFootprintCandidateDuplicateHitsLastFrame = 0;
    uint32_t m_voxelInterestAnchorFootprintCandidateScoreUpdatesLastFrame = 0;
    uint32_t m_voxelInterestAnchorCameraCandidateAttemptsLastFrame = 0;
    uint32_t m_voxelInterestAnchorCameraCandidateDuplicateHitsLastFrame = 0;
    uint32_t m_voxelInterestAnchorCameraCandidateScoreUpdatesLastFrame = 0;
    uint32_t m_voxelInterestEmittedLastFrame = 0;
    uint32_t m_voxelInterestReusedLastFrame = 0;
    uint32_t m_voxelInterestReuseAgeLastFrame = 0;
    uint32_t m_backlogVoxelEnqueuedLastFrame = 0;
    uint32_t m_backlogVoxelCarriedLastFrame = 0;
    uint32_t m_backlogVoxelPumpedLastFrame = 0;
    uint32_t m_backlogVoxelResidentSkipLastFrame = 0;
    uint32_t m_generatedVoxelTimingCountLastFrame = 0;
    float m_generatedVoxelMsAccumLastFrame = 0.0f;
    float m_generatedVoxelMaxMsLastFrame = 0.0f;
    std::array<uint32_t, SPARSE_CLIPMAP_MAX_STATS_RINGS> m_generatedVoxelBricksByRingLastFrame{};
    uint32_t m_sharedVoxelColumnHeightHitsLastFrame = 0;
    uint32_t m_sharedVoxelColumnHeightMissesLastFrame = 0;
    uint32_t m_sharedVoxelColumnReliefHitsLastFrame = 0;
    uint32_t m_sharedVoxelColumnReliefMissesLastFrame = 0;
    uint32_t m_parallelWorkerColumnCacheEntriesLastFrame = 0;
    uint32_t m_parallelWorkerColumnHeightHitsLastFrame = 0;
    uint32_t m_parallelWorkerColumnHeightMissesLastFrame = 0;
    uint32_t m_parallelWorkerColumnReliefHitsLastFrame = 0;
    uint32_t m_parallelWorkerColumnReliefMissesLastFrame = 0;
    uint32_t m_parallelVoxelPumpBricksLastFrame = 0;
    uint32_t m_parallelVoxelPumpWorkersLastFrame = 0;
    float m_parallelVoxelPumpWallMsLastFrame = 0.0f;
    uint32_t m_asyncNoncriticalGenerationEnqueuedLastFrame = 0;
    uint32_t m_asyncNoncriticalGenerationCompletedLastFrame = 0;
    uint32_t m_asyncNoncriticalGenerationAppliedLastFrame = 0;
    uint32_t m_asyncNoncriticalGenerationDiscardedLastFrame = 0;
    uint32_t m_asyncNoncriticalGenerationDuplicateSyncLastFrame = 0;
    uint32_t m_asyncVisibleCriticalGenerationEnqueuedLastFrame = 0;
    uint32_t m_asyncVisibleCriticalGenerationCompletedLastFrame = 0;
    uint32_t m_asyncVisibleCriticalGenerationAppliedLastFrame = 0;
    uint32_t m_asyncVisibleCriticalGenerationDiscardedLastFrame = 0;
    uint32_t m_asyncVisibleCriticalGenerationDuplicateSyncLastFrame = 0;
    uint32_t m_asyncVisibleReservationAppliedLastFrame = 0;
    uint32_t m_asyncVisibleReservationApplyDeferredLastFrame = 0;
    uint32_t m_asyncVisibleReservationApplyLimitLastFrame = UINT32_MAX;
    float m_asyncNoncriticalGenerationWorkerMsLastFrame = 0.0f;
    float m_asyncNoncriticalGenerationApplyMsLastFrame = 0.0f;
    uint32_t m_predictedVisibleAdmissionSamplesLastFrame = 0;
    uint32_t m_predictedVisibleAdmissionStatsFrame = 0;
    float m_predictedVisibleAdmissionSnapshotMsLastFrame = 0.0f;
    float m_predictedVisibleAdmissionRebuildMsLastFrame = 0.0f;
    float m_predictedVisibleAdmissionRestoreMsLastFrame = 0.0f;
    float m_predictedVisibleAdmissionQueueMsLastFrame = 0.0f;
    std::thread m_asyncNoncriticalGenerationThread;
    std::mutex m_asyncNoncriticalGenerationMutex;
    std::condition_variable m_asyncNoncriticalGenerationCv;
    std::deque<AsyncVoxelGenerationRequest> m_asyncNoncriticalGenerationQueue;
    std::deque<AsyncVoxelGenerationResult> m_asyncNoncriticalGenerationResults;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash>
        m_asyncNoncriticalGenerationPending;
    bool m_asyncNoncriticalGenerationStop = false;
    std::vector<std::thread> m_persistentVoxelPumpThreads;
    std::mutex m_persistentVoxelPumpMutex;
    std::condition_variable m_persistentVoxelPumpCv;
    std::condition_variable m_persistentVoxelPumpDoneCv;
    bool m_persistentVoxelPumpStop = false;
    bool m_persistentVoxelPumpActive = false;
    const std::vector<uint32_t>* m_persistentVoxelPumpSlots = nullptr;
    const SparseClipmapPolicy* m_persistentVoxelPumpPolicy = nullptr;
    std::vector<float>* m_persistentVoxelPumpElapsedMs = nullptr;
    std::vector<VoxelColumnCacheCounters>* m_persistentVoxelPumpColumnCounters = nullptr;
    std::vector<uint32_t>* m_persistentVoxelPumpColumnEntries = nullptr;
    bool m_persistentVoxelPumpUseColumnCache = false;
    size_t m_persistentVoxelPumpNext = 0;
    size_t m_persistentVoxelPumpRemaining = 0;
    uint64_t m_persistentVoxelPumpBatchSerial = 0;
    uint32_t m_visiblePriorityTaggedLastFrame = 0;
    uint32_t m_visiblePriorityPrioritizedLastFrame = 0;
    float m_effectivePumpBudgetMsLastFrame = 0.0f;
    SparseClipmapCacheStats m_stats;
    uint32_t m_dirtySerial = 0;
    uint32_t m_heightDirtySerial = 0;
    uint32_t m_voxelDirtySerial = 0;
    uint32_t m_dirtyHeightStartSlot = UINT32_MAX;
    uint32_t m_dirtyHeightEndSlot = 0;
    uint32_t m_dirtyVoxelStartSlot = UINT32_MAX;
    uint32_t m_dirtyVoxelEndSlot = 0;
    std::vector<uint32_t> m_dirtyHeightSlots;
    std::vector<uint32_t> m_dirtyVoxelSlots;
    // Edit-invalidated bricks awaiting their budgeted regeneration (see
    // PumpEditedBrickRegens). The set mirrors the deque for O(1) dedup.
    std::deque<uint32_t> m_editRegenQueue;
    std::unordered_set<uint32_t> m_editRegenQueued;
    // Edit-invalidated HEIGHT tiles awaiting budgeted regeneration. The mid-mesh
    // raster reads only these tiles' top-surface samples, so a carve stays
    // visually hidden behind stale procedural ground until its tile regenerates
    // (with the edit-aware sampler) and the mesh rebuilds. Separate from the
    // voxel-brick queue because tiles are 2D (ring,x,z) and drive the mesh.
    std::deque<uint32_t> m_editHeightTileQueue;
    std::unordered_set<uint32_t> m_editHeightTileQueued;
    // Coalesce the full-snapshot mid-mesh rebuild: bumping m_heightDirtySerial
    // every drained tile would rebuild+upload the whole mesh every frame of a
    // stroke (the 2fps trap). Instead bump at most every N frames while draining,
    // and once when the queue empties.
    uint32_t m_editHeightFramesSinceSerialBump = 0;
    // Cached world AABBs of all edit overlays (rebuilt on edit-revision change);
    // backs the per-brick GPU-gen eligibility test so one edit no longer drops
    // ALL brick generation to the CPU path.
    struct OverlayAabb {
        int32_t minX, minY, minZ, maxX, maxY, maxZ;
    };
    mutable std::vector<OverlayAabb> m_overlayAabbCache;
    mutable uint64_t m_overlayAabbCacheRevision = ~0ull;
    bool BrickIntersectsEditOverlays(
        int32_t originX, int32_t originY, int32_t originZ,
        int32_t worldSize, int32_t halo) const;
};

} // namespace VENPOD::Simulation
