#pragma once

#include "SparseTerrainGenerator.h"
#include "SparseSurfaceExtractor.h"  // SparseSurfaceFace (per-tile mesh-face cache)

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
    // Phase B: when true (and GPU mid gen is on), EDITED mid bricks are ALSO GPU-
    // generated (pristine) and their edits applied by a trivial GPU scatter
    // (CS_ApplyMidEditCellsToClipmap) instead of the expensive CPU full regen
    // (GenerateVoxelBrickPayload). The CPU only builds the cheap per-cell edit
    // overrides. Removes the edit-while-moving hitch. Env: VENPOD_MID_EDIT_BAKE.
    bool enableGpuMidEditBake = false;
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

// Phase B: one resolved mid-voxel edit override. The CPU resolves each edited mid
// cell to its final voxel via the tryEditedCellVoxel rule (cheap; reuses the same
// summary aggregation as GenerateVoxelBrickPayload); the GPU apply pass
// (CS_ApplyMidEditCellsToClipmap) scatters these into the sample pool AFTER the
// pristine gen: OutSamples[destSlot*4096 + localIndex] = voxel. 16 bytes, mirrors
// the HLSL MidEditOverride.
struct SparseMidVoxelEditOverride {
    uint32_t destSlot = 0;
    uint32_t localIndex = 0;
    uint32_t voxel = 0;
    uint32_t pad0 = 0;
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
    // Phase B: resolved edit overrides for the EDITED GPU-gen bricks in this snapshot,
    // scattered into the sample pool by CS_ApplyMidEditCellsToClipmap AFTER the gen
    // dispatch (a subset of voxelGpuGenRequests' bricks: those intersecting edits).
    std::vector<SparseMidVoxelEditOverride> voxelEditOverrides;
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
    // Per-build cap on EXPENSIVE per-tile re-emissions (cache misses). 0 = unlimited
    // (old behavior). When the budget is hit, further changed tiles reuse their cached
    // (stale, same-origin) faces this frame and are deferred to a later build — turning
    // the ~150-290ms full-frontier rebuild into bounded per-frame chunks. The deferred
    // count is reported via LastMidMeshDeferredTiles() so the caller can re-fire.
    uint32_t maxRebuildTiles = 0;
    // Per-build re-extraction TIME budget (ms). 0 = unlimited. Bounds the build spike
    // from re-extracting many expensive fine/near tiles in one frame; deferred tiles with
    // a reusable cache keep drawing stale faces (no hole), re-fired until caught up.
    float maxRebuildMs = 0.0f;
    uint32_t terraceStep = 1;
    uint32_t lodBaseMerge = 1;
    uint32_t lodMaxMerge = 4;
    bool lodEnabled = true;
    // TEST-ONLY (B1.3f-a A/B harness): force EVERY tile's LOD merge to this value,
    // overriding the camera-distance rule, so the per-BLOCK GPU extraction can be A/B'd
    // against the CPU's merged mesh in scenes where the distance rule never fires (a
    // recentering clipmap keeps fine tiles near, so mergeCells naturally stays 1). 0 =
    // OFF = the production distance-based LOD is byte-identical. >1 clamps to cellCount.
    // The CPU extractTileMesh is UNTOUCHED - it runs its normal merged path at this
    // mergeCells, producing the real CPU merged mesh the GPU must match (an honest A/B).
    uint32_t forceMergeCells = 0;
    // P1.5 incremental upload: when true, the build also tracks per-tile dirty/removed
    // coords + sets drawBatch.faces pointers so the caller can StageDirtyPayloadSnapshot
    // (upload only re-extracted tiles) instead of StageSnapshot (full re-upload).
    bool emitDirtyPayload = false;
    // PARALLEL PRE-EXTRACTION (only meaningful on the primed-dirty incremental path, i.e.
    // emitDirtyPayload && a previous build primed the GPU). When true, a serial pre-pass
    // identifies every cache-MISS tile (using the SAME LOD/childMask + meshCacheHit rules
    // as the main loop) and re-extracts them across worker threads BEFORE the main loop
    // runs. The main loop then sees those tiles as cache HITS and emits via the hit path.
    // OFF => byte-identical to the serial path (this flag gates everything).
    bool parallelExtract = false;
    // Loop 1 (FPS tail): cache computeTileLod's result per tile keyed on (camera XZ, content
    // version, residency epoch). OFF => recompute every call (byte-identical reference). When
    // ON, an editing build (stationary camera, few tiles changed) skips the per-tile LOD +
    // childResident scan that is the measured build floor. lodCacheValidate forces a recompute
    // + equality check on every call (shadow validator) and logs any mismatch.
    bool lodCache = false;
    bool lodCacheValidate = false;
    // Loop 2 (FPS tail): cap pre-extraction worker threads. The pre-pass spawns
    // min(hardware_concurrency, dirtyTiles) threads/build; on a high-logical-core CPU that
    // over-subscribes and contends (measured ~2.2x scaling, not ~14x). 0 => hardware_concurrency
    // (old behavior); a smaller cap (physical cores) can cut contention. Reversible knob.
    uint32_t preExtractMaxWorkers = 0;
    // Loop 2C (FPS tail): re-mesh only the edit-dirty cell rectangle (+halo) and splice over the
    // cached faces, instead of re-meshing the whole tile. OFF => full re-extraction (reference).
    // Validate forces a full re-extract + multiset compare per dirty tile (shadow validator).
    bool dirtyRegionExtract = false;
    bool dirtyRegionExtractValidate = false;
    // Loop D (async remesh): route tiles with a reusable same-location cache (edited / LOD-stable)
    // through the main loop's maxRebuildMs budget+deferral instead of the unbudgeted parallel
    // pre-pass. This bounds the synchronous edit-frame re-extraction to the budget (the rest defer,
    // keeping their stale faces = no hole, re-extracting over subsequent frames). New/recenter
    // tiles (no patch to hold) still pre-extract in parallel. OFF => current behavior.
    bool asyncRemesh = false;
    // Loop E: per-worker thread_local scratch + capacity-reuse assign in the extractor, to remove
    // per-tile global-heap malloc/free contention that caps parallel pre-pass scaling. Behavior-
    // identical. OFF => per-call local vector + move.
    bool extractScratch = false;
    // Loop E: work-stealing distribution for the parallel pre-pass (atomic cursor instead of
    // contiguous count-chunks) to balance the ~50x-skewed per-tile cost -> higher effective
    // parallelism on the editing burst. Behavior-identical. OFF => contiguous chunks.
    bool workStealExtract = false;
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

// Phase B1.1 (GPU mesh extraction, INPUT side only). One dirty tile's CPU->GPU
// extraction input: the stable per-tile slot, a pointer to the tile's persistent
// packedSamples grid, and the per-tile metadata a future compute shader needs to
// reproduce extractTileMesh on the GPU. Pointers are valid only until the next
// BuildMidHeightSurfaceSnapshot / tile mutation (same lifetime as the CPU draw
// batch's faces pointer). This carries NO faces and triggers NO extraction; it is
// purely the read-side projection of a dirty tile for the GPU upload path.
struct MidMeshGpuExtractDirtyTile {
    BrickCoord coord;                  // synthetic {x, ring, z} (matches dirtyBricks)
    uint32_t slot = UINT32_MAX;        // stable tile slot (m_slotByCoord)
    const uint32_t* samples = nullptr; // tile.packedSamples.data() (side*side uint32)
    uint32_t sampleCount = 0;          // side*side
    int32_t originX = 0;
    int32_t originZ = 0;
    float cellSize = 0.0f;
    uint32_t mergeCells = 0;           // LOD merge-cell size for THIS build
    uint32_t childMask = 0;            // finer-ring child residency (4 bits)
    uint64_t meshContentVersion = 0;   // per-tile content serial
    uint32_t faceCount = 0;            // CPU per-tile emitted face count (capacity bound)
    int32_t minY = 0;                  // cached per-tile height range / face AABB
    int32_t maxY = 0;
};

// Phase B1.3e (GPU edit-footprint suppression, READ-ONLY projection). One edited-cell
// XZ box in WORLD-VOXEL coordinates - byte-identical to the CPU build's internal
// `EditXzBox` (SparseClipmap.cpp extractTileMesh). The GPU mirrors the CPU's
// `cellInEditFootprint` overlap test (inclusive bounds) against these to SKIP an
// edited cell, so the live voxel raymarch owns it. Pure read of the edit overlays;
// the CPU edit path is NEVER mutated.
struct MidMeshEditXzBox {
    int32_t minX = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxZ = 0;
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
    // Non-const: maintains a per-tile mesh-face cache so an unchanged tile is not
    // re-emitted (the incremental mid-mesh — only edited/streamed/LOD-changed tiles
    // re-extract instead of the whole 1.5M-face monolith).
    bool BuildMidHeightSurfaceSnapshot(
        SparseSurfaceGpuSnapshot& outSnapshot,
        const SparseMidHeightSurfaceBuildConfig& buildConfig = {});
    // Changed tiles that were deferred (over maxRebuildTiles) in the last build. >0 means
    // the mid-mesh is mid-catch-up; the caller should re-fire the build next frame.
    uint32_t LastMidMeshDeferredTiles() const { return m_lastMidMeshDeferredTiles; }
    // Phase B1.3f-d read-only CPU-displaced telemetry: the last build's total CPU
    // extractTileMesh time + the number of tiles it (re)extracted. The GPU-extract corpus
    // harness pairs this with the per-tile GPU dispatch time to weigh CPU saved vs GPU added.
    double LastMidMeshExtractMs() const { return m_lastMidMeshExtractMs; }
    uint32_t LastMidMeshReExtractTiles() const { return m_lastMidMeshReExtractTiles; }
    // P1.5: clear exactly the tiles the GPU committed this frame (the upload is
    // per-frame BUDGETED, so a mass recenter drains over several frames). Retry-safe:
    // a failed/deferred tile stays dirty. Call only after EmitCopy succeeds.
    void AckMidMeshDirtyUpload(const std::vector<BrickCoord>& uploaded) {
        for (const BrickCoord& coord : uploaded) {
            m_midMeshDirtyCoords.erase(coord);
        }
    }
    // Full StageSnapshot fallback re-seeds the whole GPU buffer -> everything is covered.
    void AckMidMeshDirtyUploadAll() { m_midMeshDirtyCoords.clear(); }
    // True while dirty tiles remain un-uploaded -> the caller should re-fire the build
    // next frame to drain the budgeted upload backlog (no full-frame spike).
    bool HasMidMeshDirtyPayload() const { return !m_midMeshDirtyCoords.empty(); }
    // Recovery hatch: if an incremental dirty upload fails (e.g. the dirty-stage hits an
    // edge condition under a massive recenter), clearing the emitted set makes the NEXT
    // build un-primed -> it full-assembles outSnapshot.faces and the caller's full
    // StageSnapshot fallback re-seeds the entire GPU buffer from scratch (the simple path
    // that always works at startup). Costs one full-assembly frame; recovers visibleMissing
    // to 0 instead of letting resident freeze while the dirty path keeps failing.
    void ForceMidMeshFullReseed() {
        m_midMeshEmittedCoords.clear();
        m_midMeshDirtyCoords.clear();
    }
    // Phase B1.1: project the dirty tiles of the last BuildMidHeightSurfaceSnapshot
    // into per-tile GPU extraction INPUT records (slot + persistent sample pointer +
    // metadata). `dirtyCoords` is the snapshot's dirtyBricks coords (already
    // intersected with still-emitted tiles), so cost is O(dirty), not O(all tiles).
    // No extraction, no faces, no mutation - this is the read side of the GPU upload
    // path. Returns the number of records appended to `out` (a dirty coord with no
    // resident slot / empty samples is skipped). The mergeCells/childMask/faceCount
    // reflect the values cached by the build that just ran.
    uint32_t CollectMidMeshGpuExtractDirtyTiles(
        const std::vector<BrickCoord>& dirtyCoords,
        std::vector<MidMeshGpuExtractDirtyTile>& out) const;
    // Phase B1.3f-a fixture pool (DEBUG / READ-ONLY): the SAME projection as
    // CollectMidMeshGpuExtractDirtyTiles, but over EVERY resident tile with a valid mesh
    // cache (NOT just the build's dirty set). The LOD-merge increment needs a far tile with
    // mergeCells>1, and a stable far tile is not re-dirtied every build, so the per-build
    // dirty set rarely contains one. This pure read of the cached output lets the B13fa
    // fixture selector find a merged tile among ALL resident tiles. It NEVER re-extracts and
    // NEVER mutates the CPU path (same category as the dirty collector). The per-entry
    // mergeCells/childMask/sample pointer reflect the build that last extracted that tile.
    // Returns the number of records appended. O(resident tiles) - debug-only, gated by the
    // B13fa toggle in the caller.
    uint32_t CollectAllResidentMidMeshGpuExtractTiles(
        std::vector<MidMeshGpuExtractDirtyTile>& out) const;
    // Total resident mid-mesh tiles currently tracked (occupied tile slots). Used by
    // the GPU-extract instrumentation to prove dirty-scaling (uploadTiles << this).
    uint32_t MidMeshTrackedTileCount() const;
    // Phase B1.3a A/B (DEBUG / READ-ONLY): copy the persistent CPU reference mesh for
    // ONE resident tile slot - exactly the faces `extractTileMesh` left in
    // `meshCacheFaces`. The GPU top-face extraction A/Bs against this as the ground
    // truth (containment: GPU top faces must be a multiset-subset of these). This is a
    // pure read of cached output - it NEVER re-extracts and NEVER mutates the CPU path
    // (the production algorithm is untouched). Also reports the LOD/child state the
    // tile was cached under so the caller can pick flat/simple fixtures
    // (mergeCells==1, childMask==0). Returns false if the slot is not resident.
    bool GetMidMeshTileCacheFacesBySlot(
        uint32_t slot,
        std::vector<SparseSurfaceFace>& outFaces,
        uint32_t* outMergeCells = nullptr,
        uint32_t* outChildMask = nullptr,
        uint64_t* outContentVersion = nullptr) const;
    bool GetMidMeshTileCacheIdentityBySlot(
        uint32_t slot,
        BrickCoord* outCoord = nullptr,
        uint64_t* outContentVersion = nullptr,
        uint32_t* outFaceCount = nullptr) const;
    // Phase B1.3a fixture check (DEBUG / READ-ONLY): true iff any brush edit footprint
    // overlaps this tile's XZ extent. A B1.3a fixture must have NO edit footprint, since
    // the CPU `extractTileMesh` suppresses edited cells (the GPU would then emit a top
    // face the CPU dropped -> a false containment extra). Pure read of edit overlays.
    bool MidMeshTileHasEditFootprintBySlot(uint32_t slot) const;
    // Phase B1.3e edit-footprint upload (DEBUG / READ-ONLY): collect the WORLD-VOXEL edit
    // boxes whose XZ footprint overlaps this tile - the exact `editXzBoxes` the CPU build
    // tests in `cellInEditFootprint` (same coordinate space, same inclusive overlap bounds).
    // Appends at most `maxBoxes` boxes to `outBoxes` (capped to keep the per-tile GPU upload
    // bounded); the RETURN value is the TOTAL overlapping box count (so the caller can detect
    // a cap overflow when return > outBoxes.size()-startSize). Pure read of edit overlays - it
    // NEVER mutates the CPU path. Returns 0 if the slot is not resident / no edits.
    uint32_t MidMeshTileEditBoxesBySlot(
        uint32_t slot,
        std::vector<MidMeshEditXzBox>& outBoxes,
        uint32_t maxBoxes) const;
    // Phase B1.3f-c camera-distance cull parity (DEBUG / READ-ONLY): re-run the EXACT CPU
    // per-block `blockCullBounds`/`distanceCullBounds` float predicate that `extractTileMesh`
    // applied when it produced this tile's meshCacheFaces, and return the per-block cull
    // decision as a flat bit array indexed by the SHADER's blockId = bz*blockCountPerAxis+bx
    // (z-outer, x-inner; block cell-span x=bx*mergeCells, xEnd=min(cellCount,x+mergeCells)).
    // Bit set => the CPU CULLED that block (emitted nothing for it). The GPU consumes this
    // mask so its cull decision is BIT-IDENTICAL to the CPU's at any distance threshold (no
    // float-compare divergence). Uses the cull params PINNED at extraction (meshCacheCull*),
    // not the current camera, so the mask matches the cache regardless of camera motion.
    // `outMask` is sized to blockCountPerAxis^2 (one bit per uint8). Returns the number of
    // CULLED blocks (popcount); 0 if cull was off / slot not resident. Pure read - the CPU
    // cull algorithm is NEVER touched, only its recorded inputs are replayed.
    uint32_t MidMeshTileCullBlockMaskBySlot(
        uint32_t slot,
        std::vector<uint8_t>& outMask,
        uint32_t* outBlockCountPerAxis = nullptr) const;
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
    // Debug parity (public; called from the render loop one-shot): gather up to
    // `maxBricks` resident EDITED GPU-gen bricks (non-empty editOverrides) into
    // parallel arrays the mid-edit-bake parity harness consumes: a gen request per
    // brick, all their overrides (keyed by destSlot=slot), and the CPU full-regen-
    // with-edits reference per brick. Returns the brick count.
    uint32_t CollectMidEditBakeVerifyData(
        const SparseClipmapPolicy& policy,
        uint32_t maxBricks,
        std::vector<SparseMidVoxelGpuGenRequest>& outRequests,
        std::vector<SparseMidVoxelEditOverride>& outOverrides,
        std::vector<std::vector<uint32_t>>& outCpuBricks);

    const SparseClipmapCacheStats& GetStats() const { return m_stats; }
    uint32_t DirtySerial() const { return m_dirtySerial; }
    uint32_t HeightDirtySerial() const { return m_heightDirtySerial; }
    uint32_t VoxelDirtySerial() const { return m_voxelDirtySerial; }
    // Engine main loop opts in: RefreshStats' heavy telemetry aggregation runs at most
    // once per stats frame instead of on every (hundreds of) internal calls. Leave OFF
    // for tests/isolated use so every RefreshStats() yields a complete snapshot.
    void SetStatsHeavyRefreshOncePerFrame(bool enable) { m_statsHeavyRefreshOncePerFrame = enable; }
    // Opt-in refresh for telemetry consumers. Coverage-critical stats remain current
    // without this; the heavy diagnostics sweep runs only when logs/overlays need it.
    void RefreshStatsForTelemetry();
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
        // Incremental mid-mesh: this tile's emitted surface faces, cached with the
        // key they were built at. A build reuses them unless the key changed
        // (content regen, camera-distance LOD, finer-ring child residency, or the
        // tile slot was re-centered to a new coord). contentVersion is bumped by
        // GenerateTile, so edits/streaming/regen invalidate only the affected tile.
        uint64_t meshContentVersion = 0;
        std::vector<SparseSurfaceFace> meshCacheFaces;
        uint64_t meshCacheContentVersion = UINT64_MAX;
        uint32_t meshCacheMergeCells = 0xFFFFFFFFu;
        uint32_t meshCacheChildMask = 0xFFFFFFFFu;
        uint32_t meshCacheBuildVersion = 0xFFFFFFFFu;
        int32_t meshCacheOriginX = INT32_MIN;
        int32_t meshCacheOriginZ = INT32_MIN;
        int32_t meshCacheRing = -1;
        bool meshCacheValid = false;
        // Cached per-tile height range (the LOD slope-scan result). Depends only on the
        // tile's samples (content), not the camera, so it's recomputed only when content
        // changes - the slope scan ran on EVERY tile EVERY build (the per-tile-loop floor).
        int32_t meshCacheRangeMinY = 0;
        int32_t meshCacheRangeMaxY = 0;
        uint64_t meshCacheRangeVersion = UINT64_MAX;
        // Cached direction mask + face AABB. Both are O(faceCount) scans of the tile's
        // emitted faces and depend ONLY on those faces, yet ran for EVERY tile EVERY build
        // (251 tiles x ~4600 faces = ~1.1M face-iterations/build x2). Computed once when the
        // faces are (re)extracted, reused for the life of meshCacheFaces.
        uint32_t meshCacheDirectionMask = 0;
        int32_t meshCacheMinX = 0;
        int32_t meshCacheMinY = 0;
        int32_t meshCacheMinZ = 0;
        int32_t meshCacheMaxX = 0;
        int32_t meshCacheMaxY = 0;
        int32_t meshCacheMaxZ = 0;
        // Phase B1.3f-c (GPU camera-distance cull parity, READ-ONLY annotation): record the
        // EXACT distance-cull parameters the build used when it extracted meshCacheFaces, so a
        // gated read-only accessor (MidMeshTileCullBlockMaskBySlot) can re-run the IDENTICAL
        // CPU blockCullBounds/distanceCullBounds float predicate per block and hand the GPU the
        // CPU's bit-exact cull decision (the cache is NOT camera-keyed, so the CURRENT camera
        // could differ - these pin the decision to the build that produced these faces). Pure
        // bookkeeping: these record values the build already computed; they NEVER change the
        // cull decision, the geometry, or extractTileMesh's algorithm (parallel to the other
        // meshCache* fields). Default = cull off (no faces removed) until a build records them.
        bool meshCacheCullDistanceEnabled = false;
        float meshCacheCullCameraX = 0.0f;
        float meshCacheCullCameraZ = 0.0f;
        float meshCacheCullMinDistance = 0.0f;
        float meshCacheCullMaxDistance = 0.0f;
        float meshCacheCullPadding = 0.0f;
        // Loop 1 (FPS tail): computeTileLod result cache. computeTileLod runs per tile in BOTH
        // the pre-extract pre-pass AND the main emit loop (~2x m_tiles.size() calls/build), and
        // its childResident finer-suppression scan does up to 4 m_slotByCoord hash lookups/tile.
        // That per-tile loop is the measured mid-mesh build floor (buildMs 14-34ms while extract
        // and assembly are ~0). The result depends ONLY on (camera XZ, content version, child
        // residency epoch); caching it lets an editing build (stationary camera) skip the scan,
        // and within a single build the second call reuses the first. Pure memoization: the
        // cached values are bit-identical to a recompute, so geometry/LOD/extractTileMesh are
        // unchanged. A shadow validator (debug) recomputes and compares to catch any drift.
        bool lodCacheValid = false;
        float lodCacheCameraX = 0.0f;
        float lodCacheCameraZ = 0.0f;
        uint64_t lodCacheContentVersion = UINT64_MAX;
        uint64_t lodCacheResidencyEpoch = UINT64_MAX;
        uint32_t lodCacheCellSize = 0;
        uint32_t lodCacheMergeCells = 0;
        uint32_t lodCacheChildMask = 0;
        bool lodCacheAnyChildResident = false;
        bool lodCacheChildResident[4] = { false, false, false, false };
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
    // Phase B: resolve the CURRENT edit overlay into brick.editOverrides (one entry
    // per edited OWNED mid cell, via the tryEditedCellVoxel single-cell rule). Cheap
    // (O(edited voxels in brick)); does NO procedural sampling. Called from the GPU-
    // gen edited-brick branch instead of the full CPU GenerateVoxelBrickPayload regen.
    void BuildMidEditOverridesForBrick(
        VoxelBrickPayload& brick, const SparseClipmapRing& ring);
    // Debug parity: CPU-reference payload for the brick at `slot`, generated with
    // GPU mid gen + edit bake FORCED OFF (the full CPU regen WITH the current edit
    // overlay). The mid-edit-bake parity harness compares this against the GPU
    // gen+bake result for the same slot. Returns false if the slot is not resident.
    bool GenerateEditedBrickCpuReferenceForSlot(
        uint32_t slot, const SparseClipmapPolicy& policy,
        std::vector<uint32_t>& outVoxels);
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
    // Loop 1 (FPS tail): bumped on every m_slotByCoord mutation (tile insert/erase/clear/
    // recenter). A tile's computeTileLod childResident scan reads child residency from
    // m_slotByCoord, so this epoch is the invalidation signal for the per-tile LOD cache:
    // unchanged epoch + same camera + same content version => the cached LOD is still exact.
    uint64_t m_midMeshResidencyEpoch = 0;
    uint64_t m_midMeshLodCacheMismatches = 0;  // shadow-validator drift count (must stay 0)
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
        // Phase B: resolved (localIndex, voxel) edit overrides for this brick when it
        // is a GPU-generated EDITED brick. Rebuilt by GenerateVoxelBrickPayload from
        // the CURRENT edit overlay on each (re)generation (so an erased voxel reverts
        // to the pristine base because no override is emitted for it); consumed by
        // BuildGpuSnapshot, applied on GPU after the pristine gen. Empty otherwise.
        std::vector<std::pair<uint32_t, uint32_t>> editOverrides;
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
    // P1.5 incremental mid-mesh upload state (emitDirtyPayload builds only).
    // m_midMeshDirtyCoords: synthetic tile coords whose faces changed but have not yet
    // been acknowledged as uploaded (persists across frames until AckMidMeshDirtyUpload).
    // m_midMeshEmittedCoords: synthetic coords emitted by the previous build, to derive
    // removed (evicted/re-centered) tiles. m_midMeshIncrementalPrimed: a full StageSnapshot
    // has populated the GPU mirrors, so dirty uploads are now valid.
    std::unordered_set<BrickCoord, BrickCoordHash> m_midMeshDirtyCoords;
    std::unordered_set<BrickCoord, BrickCoordHash> m_midMeshEmittedCoords;
    // No-hole budget telemetry (and the seed of a real dirty worklist): tiles whose
    // re-extraction was deferred by the time budget, keyed to the build counter at which
    // they were first deferred, so we can report how long any tile has been drawing stale
    // faces (stale age) and how many are pending. A tile leaves the set when it actually
    // re-extracts. m_midMeshBuildCounter ticks once per build that runs (during a drain the
    // catchup forces a build every frame, so build age ~= frame age = edit-lag frames).
    std::unordered_map<BrickCoord, uint64_t, BrickCoordHash> m_midMeshTileDeferredSince;
    uint64_t m_midMeshBuildCounter = 0;
    uint32_t m_lastMidMeshMaxStaleAge = 0;
    uint32_t m_lastMidMeshPendingCount = 0;
    // Phase B1.3f-d (read-only telemetry): the last build's CPU extract attribution, so the
    // GPU-extract corpus harness can report the CPU extractTileMesh cost DISPLACED (CPU saved vs
    // GPU added) without touching the production extractor. Pure bookkeeping set at build end.
    double m_lastMidMeshExtractMs = 0.0;
    uint32_t m_lastMidMeshReExtractTiles = 0;
    uint32_t m_lastInterestUpdateFrame = 0;
    uint32_t m_lastStatsFrame = 0;
    // RefreshStats' heavy aggregation (iterating up to 16384 resident voxel bricks +
    // the generation queue) is TELEMETRY-ONLY but ran on all ~20 RefreshStats() calls
    // per frame (~14% of frame CPU, the top profiled hot spot). Recompute it at most
    // once per stats frame; UINT32_MAX so the first call each frame always runs it.
    uint32_t m_lastFullStatsFrame = 0xFFFFFFFFu;
    uint32_t m_lastCoverageStatsFrame = 0xFFFFFFFFu;
    bool m_statsHeavyRefreshOncePerFrame = false;
    bool m_statsHeavyTelemetryRequestedThisFrame = false;
    uint32_t m_lastMidMeshDeferredTiles = 0;
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
