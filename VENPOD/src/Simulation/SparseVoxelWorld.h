#pragma once

#include "SparseBrickPool.h"
#include "SparseCollision.h"
#include "SparseEditStore.h"
#include "SparseSurfaceCache.h"
#include "SparseTerrainGenerator.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

struct SparseVoxelWorldConfig {
    uint32_t maxBrickPages = 32768;
    uint32_t pageTableCapacity = 131072;
    uint32_t seed = 12345u;
    bool directExactGeneration = false;
    bool asyncExactGeneration = false;
    bool asyncExactGenerationVisible = false;
    bool asyncExactGenerationPrefetchLane = false;
    // When true, the async edit gate is PER-COORD (a brick whose dependency
    // neighborhood is edit-free generates async even while edits exist elsewhere).
    // When false, the legacy GLOBAL gate (any edit -> all gen sync). Toggle for A/B
    // + a safety switch back to the conservative behavior.
    bool asyncExactGenerationPerCoordEditGate = true;
    uint32_t asyncExactGenerationQueueMax = 256;
    uint32_t asyncExactGenerationMaxEnqueuePerFrame = 0;
    uint32_t asyncExactGenerationMaxApplyPerFrame = 32;
    uint32_t asyncExactGenerationMaxLowPriorityApplyPerFrame = 0;
    bool parallelExactGeneration = false;
    bool parallelExactGenerationPersistentWorkers = false;
    uint32_t parallelExactGenerationMaxWorkers = 4;
    uint32_t parallelExactGenerationMinBricks = 8;
    // When true, the fork-join parallel exact-generation blocks (PumpGeneration /
    // PumpGenerationAround) are allowed to run WHILE edits exist. Workers still
    // generate only PRISTINE bricks (GenerateExactBrickForConfig never reads
    // m_edits); the per-voxel edit overlay is applied SERIALLY on the main thread
    // (ApplyToGeneratedBrick) in the post-join apply loop, before the payload is
    // stored. Without this, one edit globally degrades exact generation to single-
    // threaded for the rest of the session (the "editing halves fps and never
    // recovers" regression). false restores the old globally-edit-disabled behavior
    // for A/B. Async exact + surface-extraction paths are NOT covered (they are not
    // yet edit-aware) and keep their own EditedBrickCount gates.
    bool parallelExactGenerationEditAware = true;
    bool incrementalPressureTrim = false;
    uint32_t incrementalPressureTrimScanBudget = 32768;
    bool surfaceBuriedSolidFastPath = false;
    bool surfaceClassValueSortCache = false;
    bool surfaceClassPartialValueSort = false;
    // PumpGenerationAround fully re-sorts every generation-class queue (and the
    // legacy queue) twice per frame, even though only a small prefix (the per-frame
    // brick budget) is ever popped. When set, the in-loop value/ticket sorts are
    // replaced by their partial-sort variants bounded to the remaining budget, which
    // produces a byte-identical consumed prefix while skipping the cost of fully
    // ordering the hundreds-to-thousands of bricks in the queue tail during the
    // moving-convergence transient. Pure CPU-prep reduction; no streaming-order change.
    bool generationClassPartialValueSort = true;
    bool surfaceStrictTimeBudget = false;
    bool parallelSurfaceExtraction = false;
    bool parallelSurfaceExtractionTimeBudgeted = false;
    uint32_t parallelSurfaceExtractionMaxWorkers = 4;
    uint32_t parallelSurfaceExtractionMinBricks = 4;
    uint32_t parallelSurfaceExtractionMaxBatch = 32;
    // Fire-and-forget async surface extraction (meshing) on persistent worker threads.
    // Unlike parallelSurfaceExtraction (fork-join: main thread blocks each frame), this
    // enqueues coords to a worker pool and applies finished meshes off the critical path,
    // so the frame never waits on meshing. Best-available render shows coarser terrain
    // until a coord's mesh lands.
    bool asyncSurfaceExtraction = false;
    // Default worker count for the async surface mesher. Raised 2->8: the exact-surface
    // near-detail extraction is the moving-into-fresh-terrain throughput producer and runs
    // entirely off the render thread, so more workers raise meshing throughput during the
    // convergence transient without adding main-thread cost (16 logical cores available).
    uint32_t asyncSurfaceExtractionMaxWorkers = 8;
    uint32_t asyncSurfaceExtractionQueueMax = 4096;
    uint32_t asyncSurfaceExtractionMaxApplyPerFrame = 256;
    bool persistentTerrainColumnCache = false;
    uint32_t terrainColumnCacheMaxEntries = 131072;
    bool streamingLaneQueuePriority = false;
    bool streamingTicketScheduler = false;
    bool streamingTicketProtectedScheduling = false;
    bool streamingTicketStageDemandAccounting = false;
    bool streamingTicketGenerationOwnershipQueues = false;
    bool streamingTicketGenerationOwnershipReservations = false;
    uint32_t streamingTicketGenerationOwnershipReservationMax = 64;
    bool streamingTicketGenerationOwnershipShareScheduler = false;
    uint32_t streamingTicketGenerationOwnershipSharePublicMin = 48;
    uint32_t streamingTicketGenerationOwnershipShareVisibleMax = 32;
    uint32_t streamingTicketGenerationOwnershipSharePrefetchMin = 32;
    uint32_t streamingTicketGenerationOwnershipShareVisibleDebtGate = 160;
    bool streamingTicketLowPriorityDownstreamDeferral = false;
    uint32_t streamingTicketLowPriorityDownstreamPromoteMax = 16;
    bool prefetchLaneSpeculativeClass = false;
};

enum class SparseBrickRequestResult : uint8_t {
    Rejected,
    AlreadyResident,
    Allocated,
    SkippedKnownEmpty
};

struct SparseVoxelWorldStats {
    uint32_t requestedBricks = 0;
    uint32_t generationQueuedBricks = 0;
    uint32_t generationQueuedSpeculativeBricks = 0;
    uint32_t generationQueuedVisibleBricks = 0;
    uint32_t generationQueuedCollisionBricks = 0;
    uint32_t generationQueuedEditedBricks = 0;
    uint32_t generationQueuedCacheLaneBricks = 0;
    uint32_t generationQueuedPrefetchLaneBricks = 0;
    uint32_t generationQueuedRepairLaneBricks = 0;
    uint32_t generationQueuedVisibleLaneBricks = 0;
    uint32_t generationQueuedPublicCriticalLaneBricks = 0;
    uint32_t generationPendingOwnershipPublicCritical = 0;
    uint32_t generationPendingOwnershipSampledVisible = 0;
    uint32_t generationPendingOwnershipHiddenRepair = 0;
    uint32_t generationPendingOwnershipCache = 0;
    uint32_t generationPendingOwnershipPrefetch = 0;
    uint32_t generationPendingOwnershipFallbackValid = 0;
    uint32_t generationPendingOwnershipUnknownCritical = 0;
    uint32_t generatedBricks = 0;
    uint32_t generatedSpeculativeBricksLastFrame = 0;
    uint32_t generatedVisibleBricksLastFrame = 0;
    uint32_t generatedCollisionBricksLastFrame = 0;
    uint32_t generatedEditedBricksLastFrame = 0;
    uint32_t generatedCacheLaneBricksLastFrame = 0;
    uint32_t generatedPrefetchLaneBricksLastFrame = 0;
    uint32_t generatedRepairLaneBricksLastFrame = 0;
    uint32_t generatedVisibleLaneBricksLastFrame = 0;
    uint32_t generatedPublicCriticalLaneBricksLastFrame = 0;
    uint32_t deferredGeneratedDownstreamPending = 0;
    uint32_t deferredGeneratedDownstreamPendingCache = 0;
    uint32_t deferredGeneratedDownstreamPendingPrefetch = 0;
    uint32_t deferredGeneratedDownstreamPendingRepair = 0;
    uint32_t deferredGeneratedDownstreamPendingVisible = 0;
    uint32_t deferredGeneratedDownstreamPendingPublicCritical = 0;
    uint32_t deferredGeneratedDownstreamPromotedLastFrame = 0;
    uint32_t deferredGeneratedDownstreamStaleLastFrame = 0;
    uint32_t asyncExactGenerationEnabled = 0;
    uint32_t asyncExactGenerationQueueDepth = 0;
    uint32_t asyncExactGenerationResultDepth = 0;
    uint32_t asyncExactGenerationPending = 0;
    uint32_t asyncExactGenerationEnqueuedLastFrame = 0;
    uint32_t asyncExactGenerationCompletedLastFrame = 0;
    uint32_t asyncExactGenerationAppliedLastFrame = 0;
    uint32_t asyncExactGenerationDeferredLowPriorityApplyLastFrame = 0;
    uint32_t asyncExactGenerationDiscardedLastFrame = 0;
    uint32_t asyncExactGenerationSyncFallbackLastFrame = 0;
    uint32_t asyncExactGenEditGateGlobalWouldSyncLastFrame = 0;
    uint32_t asyncExactGenEditGatePerCoordAsyncLastFrame = 0;
    uint32_t asyncExactGenEditStaleAtCompletionLastFrame = 0;
    uint32_t asyncExactGenerationOldestAge = 0;
    uint32_t asyncExactGenerationEnqueuedCacheLaneLastFrame = 0;
    uint32_t asyncExactGenerationEnqueuedPrefetchLaneLastFrame = 0;
    uint32_t asyncExactGenerationEnqueuedRepairLaneLastFrame = 0;
    uint32_t asyncExactGenerationEnqueuedVisibleLaneLastFrame = 0;
    uint32_t asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame = 0;
    uint32_t asyncExactGenerationAppliedCacheLaneLastFrame = 0;
    uint32_t asyncExactGenerationAppliedPrefetchLaneLastFrame = 0;
    uint32_t asyncExactGenerationAppliedRepairLaneLastFrame = 0;
    uint32_t asyncExactGenerationAppliedVisibleLaneLastFrame = 0;
    uint32_t asyncExactGenerationAppliedPublicCriticalLaneLastFrame = 0;
    float asyncExactGenerationWorkerMsLastFrame = 0.0f;
    float asyncExactGenerationApplyMsLastFrame = 0.0f;
    uint32_t parallelExactGenerationActive = 0;
    uint32_t parallelExactGenerationBricksLastFrame = 0;
    uint32_t parallelExactGenerationWorkersLastFrame = 0;
    float parallelExactGenerationWallMsLastFrame = 0.0f;
    float persistentExactGenerationWaitMsLastFrame = 0.0f;
    uint32_t uploadQueuedBricks = 0;
    uint32_t uploadQueuedSpeculativeBricks = 0;
    uint32_t uploadQueuedVisibleBricks = 0;
    uint32_t uploadQueuedCollisionBricks = 0;
    uint32_t uploadQueuedEditedBricks = 0;
    uint32_t uploadQueuedCacheLaneBricks = 0;
    uint32_t uploadQueuedPrefetchLaneBricks = 0;
    uint32_t uploadQueuedRepairLaneBricks = 0;
    uint32_t uploadQueuedVisibleLaneBricks = 0;
    uint32_t uploadQueuedPublicCriticalLaneBricks = 0;
    uint32_t uploadedSpeculativeBricksLastFrame = 0;
    uint32_t uploadedVisibleBricksLastFrame = 0;
    uint32_t uploadedCollisionBricksLastFrame = 0;
    uint32_t uploadedEditedBricksLastFrame = 0;
    uint32_t residentBricks = 0;
    uint32_t freePages = 0;
    uint32_t residentSpeculativeBricks = 0;
    uint32_t residentVisibleBricks = 0;
    uint32_t residentCollisionBricks = 0;
    uint32_t residentEditedBricks = 0;
    uint32_t evictionQueuedBricks = 0;
    uint32_t evictedBricksLastFrame = 0;
    uint32_t emptyRequestsSkippedLastFrame = 0;
    uint32_t knownEmptyGeneratedBricks = 0;
    uint32_t editedBricks = 0;
    uint32_t editedVoxels = 0;
    uint32_t renderDirtyBricks = 0;
    uint32_t renderDirtyRegionVoxels = 0;
    uint32_t renderDirtyVoxelsQueuedLastFrame = 0;
    uint32_t renderDirtyFullUploadsQueuedLastFrame = 0;
    uint32_t renderDirtyUploadDeferredLastFrame = 0;
    uint32_t renderDirtyNonResidentLastFrame = 0;
    uint32_t brushVoxelsEvaluatedLastStroke = 0;
    uint32_t brushVoxelsEditedLastStroke = 0;
    uint32_t brushBricksTouchedLastStroke = 0;
    uint32_t brushBricksQueuedLastStroke = 0;
    uint32_t surfaceCachedBricks = 0;
    uint32_t surfaceUnitFaces = 0;
    uint32_t surfaceFaces = 0;
    uint32_t residentRenderableBricks = 0;
    uint32_t residentRenderableMissingSurfaces = 0;
    uint32_t surfaceUnitFacesGeneratedLastFrame = 0;
    uint32_t surfaceFacesGeneratedLastFrame = 0;
    uint32_t surfaceBricksUpdatedLastFrame = 0;
    uint32_t surfaceBricksPartiallyUpdatedLastFrame = 0;
    uint32_t surfaceFacesRemovedByPartialUpdatesLastFrame = 0;
    uint32_t surfaceBricksRemovedLastFrame = 0;
    uint32_t surfacePendingGpuDirtyBricks = 0;
    uint32_t surfacePendingGpuRemovedBricks = 0;
    uint32_t surfaceExtractionQueuedBricks = 0;
    uint32_t surfaceQueuedSpeculativeBricks = 0;
    uint32_t surfaceQueuedVisibleBricks = 0;
    uint32_t surfaceQueuedCollisionBricks = 0;
    uint32_t surfaceQueuedEditedBricks = 0;
    uint32_t surfaceQueuedCacheLaneBricks = 0;
    uint32_t surfaceQueuedPrefetchLaneBricks = 0;
    uint32_t surfaceQueuedRepairLaneBricks = 0;
    uint32_t surfaceQueuedVisibleLaneBricks = 0;
    uint32_t surfaceQueuedPublicCriticalLaneBricks = 0;
    uint32_t streamingLaneQueuePriorityActive = 0;
    uint32_t streamingTicketSchedulerActive = 0;
    uint32_t streamingTicketProtectedSchedulingActive = 0;
    uint32_t streamingTicketProtectedSortsLastFrame = 0;
    uint32_t streamingTicketActive = 0;
    uint32_t streamingTicketCompletedLastFrame = 0;
    uint32_t streamingTicketOwnershipPublicCritical = 0;
    uint32_t streamingTicketOwnershipSampledVisible = 0;
    uint32_t streamingTicketOwnershipHiddenRepair = 0;
    uint32_t streamingTicketOwnershipCache = 0;
    uint32_t streamingTicketOwnershipPrefetch = 0;
    uint32_t streamingTicketOwnershipFallbackValid = 0;
    uint32_t streamingTicketOwnershipUnknownCritical = 0;
    uint32_t streamingTicketPendingCpu = 0;
    uint32_t streamingTicketPendingUpload = 0;
    uint32_t streamingTicketPendingSurface = 0;
    uint32_t streamingTicketPendingPublish = 0;
    uint32_t streamingTicketRequiredCpu = 0;
    uint32_t streamingTicketRequiredUpload = 0;
    uint32_t streamingTicketRequiredSurface = 0;
    uint32_t streamingTicketRequiredPublish = 0;
    uint32_t streamingTicketOldestAge = 0;
    uint32_t surfaceBricksExtractedLastFrame = 0;
    uint32_t surfaceSpeculativeBricksExtractedLastFrame = 0;
    uint32_t surfaceVisibleBricksExtractedLastFrame = 0;
    uint32_t surfaceCollisionBricksExtractedLastFrame = 0;
    uint32_t surfaceEditedBricksExtractedLastFrame = 0;
    uint32_t surfaceEmptyUploadsSkippedLastFrame = 0;
    uint32_t surfaceEmptyFastPathBricksLastFrame = 0;
    uint32_t surfaceBuriedSolidFastPathBricksLastFrame = 0;
    uint32_t surfaceClassValueSortCallsLastFrame = 0;
    uint32_t surfaceClassValueSortCacheHitsLastFrame = 0;
    uint32_t surfaceStrictTimeBudgetUnsortedPopsLastFrame = 0;
    uint32_t parallelSurfaceExtractionActive = 0;
    uint32_t parallelSurfaceExtractionBricksLastFrame = 0;
    uint32_t parallelSurfaceExtractionWorkersLastFrame = 0;
    float parallelSurfaceExtractionWallMsLastFrame = 0.0f;
    float surfaceExtractionWaitMsLastFrame = 0.0f;
    uint32_t terrainColumnCachePersistentActive = 0;
    uint32_t terrainColumnCacheEntries = 0;
    uint32_t terrainColumnCacheMaxEntries = 0;
    uint32_t terrainColumnCacheClearedLastFrame = 0;
    uint32_t terrainColumnCacheHeightHitsLastFrame = 0;
    uint32_t terrainColumnCacheHeightMissesLastFrame = 0;
    uint32_t terrainColumnCacheReliefHitsLastFrame = 0;
    uint32_t terrainColumnCacheReliefMissesLastFrame = 0;
    uint32_t surfaceSerial = 0;
    uint32_t physicsCandidateBricks = 0;
    uint32_t physicsHotCandidateBricks = 0;
    uint32_t physicsWarmCandidateBricks = 0;
    uint32_t physicsWorkPacketsLastFrame = 0;
    uint32_t physicsHotWorkPacketsLastFrame = 0;
    uint32_t physicsWarmWorkPacketsLastFrame = 0;
    uint32_t physicsDirtyRegionVoxelsLastFrame = 0;
    uint32_t physicsProcessedBricksLastFrame = 0;
    uint32_t physicsMovedVoxelsLastFrame = 0;
    uint32_t physicsSkippedVoxelsLastFrame = 0;
    uint32_t physicsSupportBricksRequestedLastFrame = 0;
    uint32_t physicsGpuProcessedProposalsLastFrame = 0;
    uint32_t physicsGpuAppliedMovesLastFrame = 0;
    uint32_t physicsGpuRejectedProposalsLastFrame = 0;
    uint32_t trimScanCallsLastFrame = 0;
    uint32_t trimRecordsScannedLastFrame = 0;
    uint32_t trimCandidatesLastFrame = 0;
    uint32_t replacementScanCallsLastFrame = 0;
    uint32_t replacementRecordsScannedLastFrame = 0;
    uint32_t replacementCandidatesLastFrame = 0;
};

struct SparseBrickUploadPacket {
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
    SparseStreamingLane streamingLane = SparseStreamingLane::Cache;
    bool partialVoxelUpload = false;
    uint16_t voxelStartIndex = 0;
    uint16_t voxelCount = SPARSE_BRICK_VOXEL_COUNT;
    uint8_t dirtyMinX = 0;
    uint8_t dirtyMinY = 0;
    uint8_t dirtyMinZ = 0;
    uint8_t dirtyMaxX = SPARSE_BRICK_SIZE - 1;
    uint8_t dirtyMaxY = SPARSE_BRICK_SIZE - 1;
    uint8_t dirtyMaxZ = SPARSE_BRICK_SIZE - 1;
    GeneratedSparseBrick brick;
};

struct SparsePageInvalidationPacket {
    BrickCoord coord;
    uint32_t entryIndex = UINT32_MAX;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
};

struct SparseRaycastHit {
    bool hit = false;
    int32_t voxelX = 0;
    int32_t voxelY = 0;
    int32_t voxelZ = 0;
    int32_t normalX = 0;
    int32_t normalY = 0;
    int32_t normalZ = 0;
    float distance = 0.0f;
    uint32_t voxel = 0;
    bool fromEdit = false;
};

struct SparsePhysicsWorkPacket {
    BrickCoord coord;
    uint32_t packedRegionMin = 0;
    uint32_t packedRegionMax = 0;
    uint32_t materialMask = 0;
    uint32_t priority = 0;
    uint32_t generation = 0;
    uint32_t expectedPageIndex = INVALID_BRICK_PAGE;
    uint32_t expectedPageGeneration = 0;
};

constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_CONSUMED = 1u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE = 2u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH = 4u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE = 8u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL = 16u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW = 32u;
constexpr uint32_t SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT = 64u;

struct SparsePhysicsPacketResult {
    BrickCoord coord;
    uint32_t packetIndex = 0;
    BrickCoord destinationCoord;
    uint32_t destinationFlags = 0;
    uint32_t generation = 0;
    uint32_t materialMask = 0;
    uint32_t checksum = 0;
    uint32_t status = 0;
    uint32_t expectedPageIndex = INVALID_BRICK_PAGE;
    uint32_t expectedPageGeneration = 0;
    uint32_t packedSourceLocal = 0;
    uint32_t packedDestinationLocal = 0;
    uint32_t sourceVoxel = 0;
    uint32_t destinationVoxel = 0;
    uint32_t sourceRevision = 0;
    uint32_t destinationRevision = 0;
};

enum class SparseRenderReadinessState : uint8_t {
    Missing,
    Requested,
    GeneratingCPU,
    GeneratedCPU,
    UploadQueued,
    UploadingGPU,
    ResidentEmpty,
    ResidentMissingSurface,
    ReadyToRender,
    DirtyCPU,
    DirtyGPU,
    EvictQueued,
    Evicted
};

struct SparseRenderReadinessStats {
    uint32_t totalTracked = 0;
    uint32_t missing = 0;
    uint32_t requested = 0;
    uint32_t generatingCPU = 0;
    uint32_t generatedCPU = 0;
    uint32_t uploadQueued = 0;
    uint32_t uploadingGPU = 0;
    uint32_t residentEmpty = 0;
    uint32_t residentMissingSurface = 0;
    uint32_t readyToRender = 0;
    uint32_t dirtyCPU = 0;
    uint32_t dirtyGPU = 0;
    uint32_t evictQueued = 0;
    uint32_t evicted = 0;
};

const char* ToString(SparseRenderReadinessState state);

class SparseVoxelWorld {
public:
    SparseVoxelWorld() = default;
    ~SparseVoxelWorld();
    SparseVoxelWorld(const SparseVoxelWorld&) = delete;
    SparseVoxelWorld& operator=(const SparseVoxelWorld&) = delete;

    bool Initialize(const SparseVoxelWorldConfig& config = {});
    void BeginFrame();
    void SetStatsRefreshDeferred(bool deferred);
    void FlushStats();
    // Engine opt-in: RefreshStats() is fired ~84x/frame from gen/upload/evict bookkeeping
    // (a top profiled hot spot during edits). With this on, the full stats refresh runs
    // at most once per stats frame; OFF for tests/isolated use (every call refreshes).
    void SetStatsRefreshOncePerFrame(bool enable) { m_statsRefreshOncePerFrame = enable; }
    void SetStatsFrame(uint64_t frame) { m_statsFrameHint = frame; }

    bool RequestBrick(const BrickCoord& coord);
    SparseBrickRequestResult RequestBrickDetailed(const BrickCoord& coord, bool allowEmptyFastPath = true);
    bool TrySkipKnownEmptyRequest(const BrickCoord& coord);
    uint32_t PumpGeneration(uint32_t maxBricks, uint32_t currentFrame = 0);
    uint32_t PumpGenerationForCoord(const BrickCoord& coord);
    uint32_t PumpGenerationForCoordsParallel(
        const std::vector<BrickCoord>& coords,
        uint32_t maxBricks,
        uint32_t maxWorkers,
        float* outWallMs = nullptr);
    uint32_t PumpGenerationAround(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame = 0);
    bool PopNextUpload(SparseBrickUploadPacket* outPacket, uint32_t currentFrame = 0);
    bool PopNextUploadForClass(
        SparseBrickUploadPacket* outPacket,
        SparseResidencyClass residencyClass,
        uint32_t currentFrame = 0);
    bool PopBestUploadForClass(
        SparseBrickUploadPacket* outPacket,
        SparseResidencyClass residencyClass,
        const BrickCoord& focus,
        uint32_t currentFrame = 0);
    bool PopBestUploadForOwnershipCritical(
        SparseBrickUploadPacket* outPacket,
        bool ownershipCritical,
        const BrickCoord& focus,
        uint32_t currentFrame = 0);
    bool PopUploadForCoord(SparseBrickUploadPacket* outPacket, const BrickCoord& coord);
    bool RequeueUploadFront(const SparseBrickUploadPacket& packet);
    bool CompleteUpload(const SparseBrickUploadPacket& packet);
    bool MarkGpuPageTablePublished(const BrickCoord& coord, uint32_t pageIndex, uint32_t generation);
    uint32_t PumpSurfaceExtraction(uint32_t maxBricks, uint32_t currentFrame = 0);
    bool PumpSurfaceExtractionForCoord(const BrickCoord& coord);
    uint32_t PumpSurfaceExtractionForCoords(
        const std::vector<BrickCoord>& coords,
        uint32_t maxBricks);
    uint32_t PumpSurfaceExtractionAround(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame = 0);
    uint32_t PumpSurfaceExtractionAroundTimed(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame,
        double maxMilliseconds);
    uint32_t PumpSurfaceExtractionAroundTimedForClass(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame,
        double maxMilliseconds,
        SparseResidencyClass residencyClass);
    uint32_t PumpSurfaceExtractionAroundTimedForOwnershipCritical(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame,
        double maxMilliseconds,
        bool ownershipCritical);
    uint32_t TrimResidentBricks(
        const BrickCoord& center,
        uint32_t keepRadiusXz,
        uint32_t keepRadiusY,
        uint32_t maxEvictions);
    // View-following eviction: evict resident bricks NOT touched/wanted within the
    // last `staleFrames` frames (lastTouchedFrame < currentFrame - staleFrames), i.e.
    // terrain the camera has flown past. Keys off recency, NOT the sticky residency
    // class (Visible is promote-only and never demotes, so it cannot be used to
    // detect "no longer visible") and NOT a camera radius (which evicts in-view far
    // surface -> holes). Collision/Edited/persistent-edit/physics bricks are always
    // kept. Incremental cursor scan bounded by scanBudget. This is what bounds the
    // resident set to the current view so the pool recovers after a long flight.
    uint32_t TrimStaleResidentBricks(
        uint32_t currentFrame,
        uint32_t staleFrames,
        uint32_t maxEvictions,
        uint32_t scanBudget);
    uint32_t TrimBackgroundResidentBricks(
        const BrickCoord& center,
        uint32_t keepRadiusXz,
        uint32_t keepRadiusY,
        uint32_t maxEvictions,
        uint32_t currentFrame = 0);
    uint32_t TrimQueuedBackgroundBricks(
        const BrickCoord& center,
        uint32_t keepRadiusXz,
        uint32_t keepRadiusY,
        uint32_t maxEvictions,
        uint32_t currentFrame = 0);
    uint32_t EvictQueuedLowerPriorityForRequest(
        const BrickCoord& center,
        SparseResidencyClass requestClass,
        uint32_t hardKeepRadiusXz,
        uint32_t hardKeepRadiusY,
        uint32_t maxEvictions,
        uint32_t currentFrame = 0);
    uint32_t EvictLowerPriorityForRequest(
        const BrickCoord& center,
        SparseResidencyClass requestClass,
        uint32_t hardKeepRadiusXz,
        uint32_t hardKeepRadiusY,
        uint32_t maxEvictions,
        uint32_t currentFrame = 0);
    bool PopNextInvalidation(SparsePageInvalidationPacket* outPacket);
    void RequeueInvalidationFront(const SparsePageInvalidationPacket& packet);
    bool MarkResidencyClass(const BrickCoord& coord, SparseResidencyClass residencyClass);
    bool MarkStreamingLane(const BrickCoord& coord, SparseStreamingLane lane);
    bool TouchResidencyClass(
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchResidencyClassWithStreamingLane(
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        SparseStreamingLane streamingLane,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchStreamingLane(
        const BrickCoord& coord,
        SparseStreamingLane lane,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchResidencyClassKnownPage(
        uint32_t pageIndex,
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchResidencyClassWithStreamingLaneKnownPage(
        uint32_t pageIndex,
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        SparseStreamingLane streamingLane,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchStreamingLaneKnownPage(
        uint32_t pageIndex,
        const BrickCoord& coord,
        SparseStreamingLane lane,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchResidentRetention(
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    bool TouchResidentRetentionKnownPage(
        uint32_t pageIndex,
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex,
        int32_t queuePriority = 0);
    void QueuePhysicsCandidate(const BrickCoord& coord);
    uint32_t StageLocalPhysicsWork(uint32_t maxBricks);
    uint32_t RequeueLastPhysicsWorkPackets();
    uint32_t ExecuteStagedLocalPhysics(uint32_t maxVoxelMoves, bool requestRenderBricks = true);
    uint32_t ApplyGpuPhysicsProposals(
        const std::vector<SparsePhysicsPacketResult>& proposals,
        uint32_t maxVoxelMoves,
        bool requestRenderBricks = true);
    uint32_t StepLocalPhysics(uint32_t maxBricks, uint32_t maxVoxelMoves, bool requestRenderBricks = true);

    void SetEditedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel);
    uint32_t ApplyEditedVoxels(const std::vector<SparseBrushFeedbackRecord>& records);
    uint32_t ApplyBrushEdit(
        float worldPositionX,
        float worldPositionY,
        float worldPositionZ,
        float radius,
        uint32_t material,
        uint32_t mode,
        uint32_t shape,
        float strength,
        uint32_t seed,
        int32_t hitNormalX = 0,
        int32_t hitNormalY = 0,
        int32_t hitNormalZ = 0,
        bool hasHitNormal = false,
        bool requestRenderBricks = true,
        std::vector<SparseEditDelta>* outDeltas = nullptr);
    uint32_t PreviewBrushEdit(
        float worldPositionX,
        float worldPositionY,
        float worldPositionZ,
        float radius,
        uint32_t material,
        uint32_t mode,
        uint32_t shape,
        float strength,
        uint32_t seed,
        int32_t hitNormalX = 0,
        int32_t hitNormalY = 0,
        int32_t hitNormalZ = 0,
        bool hasHitNormal = false,
        std::vector<SparseEditDelta>* outDeltas = nullptr);
    // Drains the deferred edited-brick regen+upload queue (see
    // QueueRegeneratedUploadForExistingPage): each entry is a full 16^3 brick
    // rebuild + edit composite, far too expensive to run inline per brush stamp.
    uint32_t PumpRegeneratedEditUploads(uint32_t maxBricks);
    CollisionSampleStatus SampleCollisionStatus(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    SparseCollisionVolumeResult TestCollisionAabb(
        const SparseCollisionAabb& aabb,
        bool liquidsBlock = false) const;
    SparseCollisionSweepResult SweepCollisionAabb(
        const SparseCollisionAabb& aabb,
        float deltaX,
        float deltaY,
        float deltaZ,
        uint32_t steps,
        bool liquidsBlock = false) const;
    SparseCollisionSupportResult FindCollisionSupportBelow(
        const SparseCollisionAabb& footprintAabb,
        float maxDrop,
        bool liquidsSupport = false) const;
    SparseRaycastHit Raycast(
        float originX,
        float originY,
        float originZ,
        float dirX,
        float dirY,
        float dirZ,
        float maxDistance) const;

    const SparseVoxelWorldStats& GetStats() const { return m_stats; }
    SparseRenderReadinessState GetRenderReadinessState(const BrickCoord& coord) const;
    SparseRenderReadinessState GetRenderReadinessStateKnownPage(
        uint32_t pageIndex,
        const BrickCoord& coord) const;
    SparseRenderReadinessStats BuildRenderReadinessStats() const;
    const SparseBrickPool& GetPool() const { return m_pool; }
    const SparseEditStore& GetEdits() const { return m_edits; }
    const SparseTerrainGenerator& GetTerrain() const { return m_terrain; }
    SparseSurfaceCache& GetSurfaceCache() { return m_surfaceCache; }
    const SparseSurfaceCache& GetSurfaceCache() const { return m_surfaceCache; }
    uint32_t GenerationQueueSize() const { return static_cast<uint32_t>(m_generationQueue.size()); }
    uint32_t UploadQueueSize() const { return static_cast<uint32_t>(m_uploadQueue.size()); }
    uint32_t InvalidationQueueSize() const { return static_cast<uint32_t>(m_invalidationQueue.size()); }
    uint32_t SurfaceExtractionQueueSize() const { return static_cast<uint32_t>(m_pendingSurfaceBricks.size()); }
    uint32_t GenerationClassQueueSize() const;
    uint32_t UploadClassQueueSize() const;
    uint32_t SurfaceClassQueueSize() const;
    const std::vector<SparsePhysicsWorkPacket>& GetLastPhysicsWorkPackets() const { return m_physicsStagedPackets; }
    const std::vector<SparseEditDelta>& GetPendingGpuEditDeltas() const { return m_edits.PendingGpuDeltas(); }
    std::vector<SparseEditDelta> BuildGpuEditDeltaSnapshotForPhysicsWork(uint32_t maxDeltas) const;
    // Edited bricks most-recently-touched first, capped at maxDeltas: the live
    // render edit-overlay bake covers what the player is actively editing; older
    // edits fall through to the durable pool once regen bakes them.
    std::vector<SparseEditDelta> BuildGpuEditDeltaSnapshotForRender(uint32_t maxDeltas) const;
    void ClearPendingGpuEditDeltas(uint32_t consumedCount) { m_edits.ClearPendingGpuDeltas(consumedCount); }
    void ClearPendingGpuEditDeltas() { m_edits.ClearPendingGpuDeltas(); }
    bool SaveEditsToFile(const std::filesystem::path& path);
    bool LoadEditsFromFile(const std::filesystem::path& path, bool requestRenderBricks = true);

private:
    enum class SparsePhysicsPriority : uint8_t {
        Warm,
        Hot
    };

    struct SparsePhysicsDirtyRegion {
        uint8_t minX = 0;
        uint8_t minY = 0;
        uint8_t minZ = 0;
        uint8_t maxX = SPARSE_BRICK_SIZE - 1;
        uint8_t maxY = SPARSE_BRICK_SIZE - 1;
        uint8_t maxZ = SPARSE_BRICK_SIZE - 1;
    };

    struct SparseRenderDirtyRegion {
        uint8_t minX = 0;
        uint8_t minY = 0;
        uint8_t minZ = 0;
        uint8_t maxX = SPARSE_BRICK_SIZE - 1;
        uint8_t maxY = SPARSE_BRICK_SIZE - 1;
        uint8_t maxZ = SPARSE_BRICK_SIZE - 1;
    };

    struct QueueClassCounts {
        uint32_t speculative = 0;
        uint32_t visible = 0;
        uint32_t collision = 0;
        uint32_t edited = 0;
    };

    struct QueueLaneCounts {
        uint32_t cache = 0;
        uint32_t prefetch = 0;
        uint32_t repair = 0;
        uint32_t visible = 0;
        uint32_t publicCritical = 0;
    };

    enum class StreamingTicketOwnership : uint8_t {
        Cache,
        Prefetch,
        SampledVisible,
        PublicCritical,
        HiddenRepair,
        FallbackValid,
        UnknownCritical
    };

    static constexpr size_t kStreamingTicketOwnershipCount = 7;
    static constexpr size_t kStreamingTicketStageCount = 4;
    using StreamingTicketOwnershipCounts =
        std::array<uint32_t, kStreamingTicketOwnershipCount>;
    using StreamingTicketStageOwnershipCounts =
        std::array<StreamingTicketOwnershipCounts, kStreamingTicketStageCount>;

    struct StreamingWorkTicket {
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        SparseStreamingLane streamingLane = SparseStreamingLane::Cache;
        StreamingTicketOwnership ownership = StreamingTicketOwnership::Cache;
        uint32_t requiredStages = 0;
        uint32_t completedStages = 0;
        uint32_t requestFrame = 0;
        uint32_t lastTouchedFrame = 0;
        uint32_t lastUpdatedFrame = 0;
        uint64_t editRevision = 0;
    };

    struct GenerationOwnershipWorklistEntry {
        StreamingTicketOwnership ownership = StreamingTicketOwnership::Cache;
        size_t index = 0;
    };

    struct TerrainSurfaceColumnKey {
        int32_t x = 0;
        int32_t z = 0;

        bool operator==(const TerrainSurfaceColumnKey& other) const {
            return x == other.x && z == other.z;
        }
    };

    struct TerrainSurfaceColumnKeyHash {
        size_t operator()(const TerrainSurfaceColumnKey& key) const noexcept;
    };

    struct TerrainSurfaceColumnCacheEntry {
        float height = 0.0f;
        float relief = 0.0f;
        int32_t reliefSampleOffset = 0;
        bool reliefValid = false;
    };
    using TerrainSurfaceColumnCache =
        std::unordered_map<TerrainSurfaceColumnKey, TerrainSurfaceColumnCacheEntry, TerrainSurfaceColumnKeyHash>;

    struct TerrainColumnCacheFrameStats {
        uint32_t heightHits = 0;
        uint32_t heightMisses = 0;
        uint32_t reliefHits = 0;
        uint32_t reliefMisses = 0;
    };

    struct AsyncExactGenerationRequest {
        BrickCoord coord;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        SparseStreamingLane streamingLane = SparseStreamingLane::Cache;
        uint32_t requestFrame = 0;
        uint64_t editRevision = 0;
    };

    struct AsyncExactGenerationResult {
        BrickCoord coord;
        GeneratedSparseBrick brick;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        SparseStreamingLane streamingLane = SparseStreamingLane::Cache;
        uint32_t requestFrame = 0;
        uint64_t editRevision = 0;
        float workerMs = 0.0f;
    };

    struct AsyncSurfaceExtractionRequest {
        BrickCoord coord;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        GeneratedSparseBrick brick;
    };

    struct AsyncSurfaceExtractionResult {
        BrickCoord coord;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        GeneratedSparseBrick brick;
        SparseSurfaceExtractionResult faces;
        float workerMs = 0.0f;
    };

    void RefreshStats();
    void MarkQueueAccountingDirty();
    void RebuildQueueClassStats();
    void QueueGenerationCoordBack(const BrickCoord& coord);
    bool RemoveFirstGenerationQueueCoord(const BrickCoord& coord);
    bool RemoveFirstGenerationClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    bool RemoveFirstGenerationOwnershipQueueCoord(const BrickCoord& coord);
    void QueueGenerationClassAliasIfRequested(const BrickCoord& coord);
    void QueueGenerationOwnershipAliasIfRequested(const BrickCoord& coord);
    bool PopGenerationOwnershipWorklistCandidate(
        StreamingTicketOwnership ownership,
        BrickCoord* outCoord,
        BrickResidentRecord* outRecord);
    uint32_t PumpGenerationOwnershipQuota(
        StreamingTicketOwnership ownership,
        uint32_t maxBricks,
        uint32_t currentFrame,
        uint32_t* outProcessed);
    uint32_t PumpGenerationOwnershipReservations(
        uint32_t maxBricks,
        uint32_t currentFrame,
        uint32_t* outProcessed);
    uint32_t PumpGenerationOwnershipShares(
        uint32_t maxBricks,
        uint32_t currentFrame,
        uint32_t* outProcessed);
    bool GenerateQueuedBrick(const BrickCoord& coord, SparseResidencyClass* outResidencyClass);
    bool ApplyGeneratedBrickPayload(
        const BrickCoord& coord,
        const GeneratedSparseBrick& brick,
        SparseResidencyClass* outResidencyClass);
    bool ShouldDeferGeneratedDownstream(
        const BrickResidentRecord& record,
        const GeneratedSparseBrick& brick) const;
    bool QueueDeferredGeneratedDownstream(const BrickCoord& coord);
    uint32_t PromoteDeferredGeneratedDownstream(uint32_t maxBricks, uint32_t currentFrame);
    uint32_t PromoteDeferredGeneratedDownstreamForOwnership(
        bool ownershipCritical,
        uint32_t maxBricks,
        uint32_t currentFrame);
    bool PromoteDeferredGeneratedDownstreamForCoord(const BrickCoord& coord, uint32_t currentFrame);
    bool PromoteDeferredGeneratedDownstreamIfCritical(const BrickCoord& coord, uint32_t currentFrame);
    bool PromoteDeferredGeneratedDownstreamCoordInternal(const BrickCoord& coord, uint32_t currentFrame);
    GeneratedSparseBrick GenerateBrickWithCachedTerrainColumns(const BrickCoord& coord);
    GeneratedSparseBrick GenerateExactBrickForConfig(
        const SparseTerrainGenerator& terrain,
        const BrickCoord& coord,
        TerrainSurfaceColumnCache& columnCache,
        TerrainColumnCacheFrameStats* columnStats = nullptr) const;
    static GeneratedSparseBrick GenerateBrickWithTerrainColumnCache(
        const SparseTerrainGenerator& terrain,
        const BrickCoord& coord,
        TerrainSurfaceColumnCache& columnCache,
        TerrainColumnCacheFrameStats* columnStats = nullptr);
    bool GenerateExactBricksWithPersistentWorkers(
        const SparseTerrainGenerator& terrain,
        const std::vector<BrickCoord>& coords,
        std::vector<GeneratedSparseBrick>& bricks,
        uint32_t workerCount);
    void StartPersistentExactGenerationWorkers(uint32_t workerCount);
    void StopPersistentExactGenerationWorkers();
    void PersistentExactGenerationWorkerLoop();
    bool TryQueueAsyncExactGeneration(
        const BrickCoord& coord,
        const BrickResidentRecord& record,
        uint32_t currentFrame);
    uint32_t ApplyAsyncExactGenerationCompletions(uint32_t currentFrame);
    void StartAsyncExactGenerationWorkerIfNeeded();
    void StopAsyncExactGenerationWorker();
    // Per-coord replacement for the old global "any edit -> all gen sync" gate. An exact
    // brick's content depends ONLY on its own coord's edit overlay (ApplyToGeneratedBrick),
    // so a brick whose dependency neighborhood has no edits can generate async even while
    // edits exist elsewhere. The neighborhood is inflated by kAsyncExactGenEditHaloBricks
    // (conservative margin; own-coord is the proven-minimal). False positive = extra sync
    // (safe); false negative = stale data (forbidden) -> stay conservative.
    static constexpr int32_t kAsyncExactGenEditHaloBricks = 1;
    bool EditOverlapsExactGenDependency(const BrickCoord& coord) const;
    uint64_t MaxEditRevisionInExactGenDependency(const BrickCoord& coord) const;
    bool TryQueueAsyncSurfaceExtraction(const BrickCoord& coord);
    uint32_t ApplyAsyncSurfaceExtractionCompletions();
    void StartAsyncSurfaceExtractionWorkerIfNeeded();
    void StopAsyncSurfaceExtractionWorker();
    bool QueuePhysicsCandidateNoStats(const BrickCoord& coord);
    bool QueuePhysicsRegionNoStats(
        const BrickCoord& coord,
        const SparsePhysicsDirtyRegion& region,
        SparsePhysicsPriority priority);
    void QueueRenderDirtyRegionNoStats(
        const BrickCoord& coord,
        const SparseRenderDirtyRegion& region,
        bool queueSurfaceDirty = true);
    void QueueSurfaceDirtyRegionNoStats(
        const BrickCoord& coord,
        const SparseRenderDirtyRegion& region);
    void QueueRenderDirtyVoxelNoStats(int32_t worldX, int32_t worldY, int32_t worldZ);
    bool QueuePhysicsVoxelNoStats(
        int32_t worldX,
        int32_t worldY,
        int32_t worldZ,
        SparsePhysicsPriority priority);
    void WakePhysicsSupportNeighborhoodNoStats(int32_t worldX, int32_t worldY, int32_t worldZ);
    float CachedTerrainHeightAt(int32_t worldX, int32_t worldZ);
    float CachedTerrainReliefAt(int32_t worldX, int32_t worldZ, int32_t sampleOffset);
    bool PopNextPhysicsWorkPacket(SparsePhysicsWorkPacket* outPacket);
    uint32_t BuildPhysicsWorkBatch(uint32_t maxPackets);
    void RequeuePhysicsWorkPacketNoStats(const SparsePhysicsWorkPacket& packet);
    void QueueUploadCoordBack(const BrickCoord& coord);
    void QueueUploadCoordFront(const BrickCoord& coord);
    bool RemoveFirstUploadQueueCoord(const BrickCoord& coord);
    bool RemoveFirstUploadClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    void QueueUploadClassAliasIfUploadQueued(const BrickCoord& coord);
    void QueueSurfaceExtractionCoord(const BrickCoord& coord);
    void MarkSurfaceQueueOrderDirty();
    bool RemoveFirstSurfaceQueueCoord(const BrickCoord& coord);
    bool RemoveFirstSurfaceClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    void QueueSurfaceClassAliasIfPending(const BrickCoord& coord);
    bool PruneSurfaceExtractionQueuesIfNoPending();
    bool CanUseBuriedSolidSurfaceFastPath(const BrickCoord& coord, const GeneratedSparseBrick& brick) const;
    bool MarkBuriedSolidSurfaceKnownEmpty(const BrickCoord& coord);
    bool ExtractSurfaceCoord(const BrickCoord& coord);
    bool ExtractOrQueueSurfaceCoord(const BrickCoord& coord);
    struct SurfaceExtractionBatchItem {
        BrickCoord coord;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        GeneratedSparseBrick brick;
    };
    bool CanUseParallelSurfaceExtractionBatch(uint32_t maxBricks) const;
    uint32_t ExtractSurfaceBatchNoEdit(std::vector<SurfaceExtractionBatchItem>& pending);
    StreamingTicketOwnership ClassifyStreamingTicketOwnership(const BrickResidentRecord& record) const;
    bool IsStreamingOwnershipCritical(const BrickResidentRecord& record) const;
    void TouchStreamingTicket(
        const BrickCoord& coord,
        const BrickResidentRecord& record,
        uint32_t requiredStages,
        uint32_t completedStages = 0);
    void UpdateStreamingTicketFromRecord(const BrickCoord& coord, const BrickResidentRecord& record);
    void MarkStreamingTicketStagesCompleted(const BrickCoord& coord, uint32_t completedStages);
    void RemoveStreamingTicket(const BrickCoord& coord);
    static size_t StreamingTicketOwnershipIndex(StreamingTicketOwnership ownership);
    static size_t StreamingTicketStageIndex(uint32_t stageBit);
    void AddStreamingTicketPendingStageDemand(const StreamingWorkTicket& ticket);
    void RemoveStreamingTicketPendingStageDemand(const StreamingWorkTicket& ticket);
    int64_t StreamingTicketOwnershipScore(StreamingTicketOwnership ownership) const;
    int64_t StreamingTicketStageScore(const StreamingWorkTicket& ticket, uint32_t stageBit) const;
    void SortQueueByStreamingTickets(
        std::deque<BrickCoord>& queue,
        uint32_t stageBit,
        const BrickCoord* focus,
        uint32_t currentFrame,
        bool valueSort,
        size_t frontCount = 0);
    void MarkUploadQueueOrderDirty();
    bool QueueRegeneratedUploadForExistingPage(
        const BrickCoord& coord,
        const SparseRenderDirtyRegion* dirtyRegion = nullptr,
        bool surfaceGeometryDirty = true);
    void AnnotateRenderDirtyUploadRange(SparseBrickUploadPacket* packet) const;
    uint32_t SampleEditedOrGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    uint32_t EvaluateBrushEdit(
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
        std::vector<SparseEditDelta>* outDeltas);

    std::deque<BrickCoord> m_pendingRegenUploadQueue;
    std::unordered_set<BrickCoord, BrickCoordHash> m_pendingRegenUploadSet;

    SparseVoxelWorldConfig m_config;
    SparseBrickPool m_pool;
    SparseTerrainGenerator m_terrain;
    SparseEditStore m_edits;
    SparseSurfaceCache m_surfaceCache;
    SparseVoxelWorldStats m_stats;

    std::deque<BrickCoord> m_generationQueue;
    std::array<std::deque<BrickCoord>, 4> m_generationClassQueues;
    std::array<std::vector<BrickCoord>, kStreamingTicketOwnershipCount> m_generationOwnershipWorklists;
    std::unordered_map<BrickCoord, GenerationOwnershipWorklistEntry, BrickCoordHash>
        m_generationOwnershipWorklistEntries;
    std::deque<BrickCoord> m_uploadQueue;
    std::array<std::deque<BrickCoord>, 4> m_uploadClassQueues;
    std::array<std::deque<BrickCoord>, 2> m_uploadOwnershipQueues;
    std::deque<SparsePageInvalidationPacket> m_invalidationQueue;
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash> m_generated;
    std::deque<BrickCoord> m_deferredGeneratedDownstreamQueue;
    std::unordered_set<BrickCoord, BrickCoordHash> m_deferredGeneratedDownstreamSet;
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash> m_pendingSurfaceBricks;
    std::unordered_map<BrickCoord, bool, BrickCoordHash> m_deferredDirtyAfterUpload;
    std::unordered_map<BrickCoord, SparseRenderDirtyRegion, BrickCoordHash> m_renderDirtyRegions;
    std::unordered_map<BrickCoord, SparseSurfaceLocalRegion, BrickCoordHash> m_surfaceDirtyRegions;
    TerrainSurfaceColumnCache m_surfaceTerrainColumnCache;
    TerrainColumnCacheFrameStats m_terrainColumnCacheFrameStats;
    uint32_t m_terrainColumnCacheClearedLastFrame = 0;
    std::unordered_map<BrickCoord, SparsePhysicsDirtyRegion, BrickCoordHash> m_physicsDirtyRegions;
    std::unordered_map<BrickCoord, SparsePhysicsPriority, BrickCoordHash> m_physicsQueuedPriorities;
    std::unordered_set<BrickCoord, BrickCoordHash> m_knownEmptyGeneratedBricks;
    std::unordered_set<BrickCoord, BrickCoordHash> m_surfaceExtractionQueuedSet;
    std::deque<BrickCoord> m_surfaceExtractionQueue;
    std::array<std::deque<BrickCoord>, 4> m_surfaceClassQueues;
    std::array<std::deque<BrickCoord>, 2> m_surfaceOwnershipQueues;
    bool m_generationQueuePriorityDirty = false;
    bool m_uploadQueuePriorityDirty = false;
    bool m_surfaceExtractionQueuePriorityDirty = false;
    std::array<bool, 4> m_uploadClassValueSortValid{};
    std::array<BrickCoord, 4> m_uploadClassValueSortFocus{};
    std::array<uint32_t, 4> m_uploadClassValueSortFrame{};
    std::array<bool, 4> m_surfaceClassValueSortValid{};
    std::array<BrickCoord, 4> m_surfaceClassValueSortFocus{};
    bool m_queueClassStatsDirty = true;
    uint32_t m_cachedGenerationQueueSize = 0;
    uint32_t m_cachedUploadQueueSize = 0;
    uint32_t m_cachedSurfacePendingSize = 0;
    uint32_t m_deferredGeneratedDownstreamPromotedFrame = 0xFFFFFFFFu;
    uint32_t m_deferredGeneratedDownstreamPromotedLastFrame = 0;
    uint32_t m_deferredGeneratedDownstreamStaleLastFrame = 0;
    QueueClassCounts m_generationQueueClassCounts;
    QueueClassCounts m_uploadQueueClassCounts;
    QueueClassCounts m_surfaceQueueClassCounts;
    QueueLaneCounts m_generationQueueLaneCounts;
    QueueLaneCounts m_uploadQueueLaneCounts;
    QueueLaneCounts m_surfaceQueueLaneCounts;
    std::deque<BrickCoord> m_physicsHotQueue;
    std::deque<BrickCoord> m_physicsWarmQueue;
    std::vector<SparsePhysicsWorkPacket> m_physicsStagedPackets;
    uint32_t m_evictedBricksLastFrame = 0;
    uint32_t m_emptyRequestsSkippedLastFrame = 0;
    uint32_t m_generatedSpeculativeBricksLastFrame = 0;
    uint32_t m_generatedVisibleBricksLastFrame = 0;
    uint32_t m_generatedCollisionBricksLastFrame = 0;
    uint32_t m_generatedEditedBricksLastFrame = 0;
    uint32_t m_generatedCacheLaneBricksLastFrame = 0;
    uint32_t m_generatedPrefetchLaneBricksLastFrame = 0;
    uint32_t m_generatedRepairLaneBricksLastFrame = 0;
    uint32_t m_generatedVisibleLaneBricksLastFrame = 0;
    uint32_t m_generatedPublicCriticalLaneBricksLastFrame = 0;
    uint32_t m_asyncExactGenerationEnqueuedLastFrame = 0;
    uint32_t m_asyncExactGenerationCompletedLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedLastFrame = 0;
    uint32_t m_asyncExactGenerationDeferredLowPriorityApplyLastFrame = 0;
    uint32_t m_asyncExactGenerationDiscardedLastFrame = 0;
    uint32_t m_asyncExactGenerationSyncFallbackLastFrame = 0;
    // Per-coord edit-gate instrumentation (the edit-aware async relaxation).
    uint32_t m_asyncExactGenEditGateGlobalWouldSyncLastFrame = 0;  // old global gate would have synced
    uint32_t m_asyncExactGenEditGatePerCoordSyncLastFrame = 0;     // per-coord gate kept sync (real overlap)
    uint32_t m_asyncExactGenEditGatePerCoordAsyncLastFrame = 0;    // per-coord gate unlocked async
    uint32_t m_asyncExactGenEditStaleAtCompletionLastFrame = 0;    // edit landed during gen -> regen sync
    uint32_t m_asyncExactGenerationEnqueuedCacheLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationEnqueuedPrefetchLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationEnqueuedRepairLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationEnqueuedVisibleLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationEnqueuedPublicCriticalLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedCacheLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedPrefetchLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedRepairLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedVisibleLaneLastFrame = 0;
    uint32_t m_asyncExactGenerationAppliedPublicCriticalLaneLastFrame = 0;
    float m_asyncExactGenerationWorkerMsLastFrame = 0.0f;
    float m_asyncExactGenerationApplyMsLastFrame = 0.0f;
    uint32_t m_parallelExactGenerationBricksLastFrame = 0;
    uint32_t m_parallelExactGenerationWorkersLastFrame = 0;
    float m_parallelExactGenerationWallMsLastFrame = 0.0f;
    float m_persistentExactGenerationWaitMsLastFrame = 0.0f;
    uint32_t m_uploadedSpeculativeBricksLastFrame = 0;
    uint32_t m_uploadedVisibleBricksLastFrame = 0;
    uint32_t m_uploadedCollisionBricksLastFrame = 0;
    uint32_t m_uploadedEditedBricksLastFrame = 0;
    uint32_t m_surfaceBricksExtractedLastFrame = 0;
    uint32_t m_surfaceSpeculativeBricksExtractedLastFrame = 0;
    uint32_t m_surfaceVisibleBricksExtractedLastFrame = 0;
    uint32_t m_surfaceCollisionBricksExtractedLastFrame = 0;
    uint32_t m_surfaceEditedBricksExtractedLastFrame = 0;
    uint32_t m_surfaceEmptyUploadsSkippedLastFrame = 0;
    uint32_t m_surfaceBuriedSolidFastPathBricksLastFrame = 0;
    uint32_t m_surfaceClassValueSortCallsLastFrame = 0;
    uint32_t m_surfaceClassValueSortCacheHitsLastFrame = 0;
    uint32_t m_surfaceStrictTimeBudgetUnsortedPopsLastFrame = 0;
    uint32_t m_parallelSurfaceExtractionBricksLastFrame = 0;
    uint32_t m_parallelSurfaceExtractionWorkersLastFrame = 0;
    float m_parallelSurfaceExtractionWallMsLastFrame = 0.0f;
    float m_surfaceExtractionWaitMsLastFrame = 0.0f;
    std::unordered_map<BrickCoord, StreamingWorkTicket, BrickCoordHash> m_streamingTickets;
    StreamingTicketStageOwnershipCounts m_streamingTicketPendingStageOwnershipCounts{};
    uint32_t m_streamingTicketCompletedLastFrame = 0;
    uint32_t m_streamingTicketProtectedSortsLastFrame = 0;
    uint32_t m_renderDirtyVoxelsQueuedLastFrame = 0;
    uint32_t m_renderDirtyFullUploadsQueuedLastFrame = 0;
    uint32_t m_renderDirtyUploadDeferredLastFrame = 0;
    uint32_t m_renderDirtyNonResidentLastFrame = 0;
    uint32_t m_physicsProcessedBricksLastFrame = 0;
    uint32_t m_physicsWorkPacketsLastFrame = 0;
    uint32_t m_physicsHotWorkPacketsLastFrame = 0;
    uint32_t m_physicsWarmWorkPacketsLastFrame = 0;
    uint32_t m_physicsDirtyRegionVoxelsLastFrame = 0;
    uint32_t m_physicsMovedVoxelsLastFrame = 0;
    uint32_t m_physicsSkippedVoxelsLastFrame = 0;
    uint32_t m_physicsSupportBricksRequestedLastFrame = 0;
    uint32_t m_physicsGpuProcessedProposalsLastFrame = 0;
    uint32_t m_physicsGpuAppliedMovesLastFrame = 0;
    uint32_t m_physicsGpuRejectedProposalsLastFrame = 0;
    uint32_t m_trimScanCallsLastFrame = 0;
    uint32_t m_trimRecordsScannedLastFrame = 0;
    uint32_t m_trimCandidatesLastFrame = 0;
    uint32_t m_replacementScanCallsLastFrame = 0;
    uint32_t m_replacementRecordsScannedLastFrame = 0;
    uint32_t m_replacementCandidatesLastFrame = 0;
    size_t m_trimResidentCursor = 0;
    size_t m_trimStaleResidentCursor = 0;
    size_t m_trimBackgroundResidentCursor = 0;
    size_t m_trimQueuedBackgroundCursor = 0;
    uint32_t m_physicsWorkGeneration = 0;
    bool m_statsRefreshDeferred = false;
    bool m_statsRefreshPending = false;
    bool m_statsRefreshOncePerFrame = false;
    uint64_t m_statsFrameHint = 0;
    uint64_t m_lastFullStatsFrame = 0xFFFFFFFFFFFFFFFFull;
    std::thread m_asyncExactGenerationThread;
    std::mutex m_asyncExactGenerationMutex;
    std::condition_variable m_asyncExactGenerationCv;
    std::deque<AsyncExactGenerationRequest> m_asyncExactGenerationQueue;
    std::deque<AsyncExactGenerationResult> m_asyncExactGenerationResults;
    std::unordered_set<BrickCoord, BrickCoordHash> m_asyncExactGenerationPending;
    bool m_asyncExactGenerationStop = false;
    uint32_t m_asyncExactGenerationStatsFrame = 0;
    // Fire-and-forget async surface extraction (meshing) worker pool.
    std::vector<std::thread> m_asyncSurfaceExtractionThreads;
    std::mutex m_asyncSurfaceExtractionMutex;
    std::condition_variable m_asyncSurfaceExtractionCv;
    std::deque<AsyncSurfaceExtractionRequest> m_asyncSurfaceExtractionQueue;
    std::deque<AsyncSurfaceExtractionResult> m_asyncSurfaceExtractionResults;
    std::unordered_set<BrickCoord, BrickCoordHash> m_asyncSurfaceExtractionPending;
    bool m_asyncSurfaceExtractionStop = false;
    uint32_t m_asyncSurfaceExtractionEnqueuedLastFrame = 0;
    uint32_t m_asyncSurfaceExtractionAppliedLastFrame = 0;
    uint32_t m_asyncSurfaceExtractionDiscardedLastFrame = 0;
    float m_asyncSurfaceExtractionWorkerMsLastFrame = 0.0f;
    std::vector<std::thread> m_persistentExactGenerationThreads;
    std::mutex m_persistentExactGenerationMutex;
    std::condition_variable m_persistentExactGenerationCv;
    std::condition_variable m_persistentExactGenerationDoneCv;
    const SparseTerrainGenerator* m_persistentExactGenerationTerrain = nullptr;
    const std::vector<BrickCoord>* m_persistentExactGenerationCoords = nullptr;
    std::vector<GeneratedSparseBrick>* m_persistentExactGenerationBricks = nullptr;
    size_t m_persistentExactGenerationNext = 0;
    size_t m_persistentExactGenerationRemaining = 0;
    bool m_persistentExactGenerationActive = false;
    bool m_persistentExactGenerationStop = false;
};

} // namespace VENPOD::Simulation
