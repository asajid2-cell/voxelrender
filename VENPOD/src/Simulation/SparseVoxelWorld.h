#pragma once

#include "SparseBrickPool.h"
#include "SparseCollision.h"
#include "SparseEditStore.h"
#include "SparseSurfaceCache.h"
#include "SparseTerrainGenerator.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

struct SparseVoxelWorldConfig {
    uint32_t maxBrickPages = 4096;
    uint32_t pageTableCapacity = 16384;
    uint32_t seed = 12345u;
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
    uint32_t generatedBricks = 0;
    uint32_t generatedSpeculativeBricksLastFrame = 0;
    uint32_t generatedVisibleBricksLastFrame = 0;
    uint32_t generatedCollisionBricksLastFrame = 0;
    uint32_t generatedEditedBricksLastFrame = 0;
    uint32_t uploadQueuedBricks = 0;
    uint32_t uploadQueuedSpeculativeBricks = 0;
    uint32_t uploadQueuedVisibleBricks = 0;
    uint32_t uploadQueuedCollisionBricks = 0;
    uint32_t uploadQueuedEditedBricks = 0;
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
    uint32_t surfaceBricksExtractedLastFrame = 0;
    uint32_t surfaceSpeculativeBricksExtractedLastFrame = 0;
    uint32_t surfaceVisibleBricksExtractedLastFrame = 0;
    uint32_t surfaceCollisionBricksExtractedLastFrame = 0;
    uint32_t surfaceEditedBricksExtractedLastFrame = 0;
    uint32_t surfaceEmptyUploadsSkippedLastFrame = 0;
    uint32_t surfaceEmptyFastPathBricksLastFrame = 0;
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
};

struct SparseBrickUploadPacket {
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
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

class SparseVoxelWorld {
public:
    bool Initialize(const SparseVoxelWorldConfig& config = {});
    void BeginFrame();
    void SetStatsRefreshDeferred(bool deferred);
    void FlushStats();

    bool RequestBrick(const BrickCoord& coord);
    SparseBrickRequestResult RequestBrickDetailed(const BrickCoord& coord, bool allowEmptyFastPath = true);
    uint32_t PumpGeneration(uint32_t maxBricks, uint32_t currentFrame = 0);
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
    bool RequeueUploadFront(const SparseBrickUploadPacket& packet);
    bool CompleteUpload(const SparseBrickUploadPacket& packet);
    uint32_t PumpSurfaceExtraction(uint32_t maxBricks, uint32_t currentFrame = 0);
    uint32_t PumpSurfaceExtractionAround(
        uint32_t maxBricks,
        const BrickCoord& focus,
        uint32_t currentFrame = 0);
    uint32_t TrimResidentBricks(
        const BrickCoord& center,
        uint32_t keepRadiusXz,
        uint32_t keepRadiusY,
        uint32_t maxEvictions);
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
    bool TouchResidencyClass(
        const BrickCoord& coord,
        SparseResidencyClass residencyClass,
        uint32_t frameIndex);
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
    const SparseBrickPool& GetPool() const { return m_pool; }
    const SparseEditStore& GetEdits() const { return m_edits; }
    const SparseTerrainGenerator& GetTerrain() const { return m_terrain; }
    SparseSurfaceCache& GetSurfaceCache() { return m_surfaceCache; }
    const SparseSurfaceCache& GetSurfaceCache() const { return m_surfaceCache; }
    uint32_t GenerationQueueSize() const { return static_cast<uint32_t>(m_generationQueue.size()); }
    uint32_t UploadQueueSize() const { return static_cast<uint32_t>(m_uploadQueue.size()); }
    uint32_t InvalidationQueueSize() const { return static_cast<uint32_t>(m_invalidationQueue.size()); }
    uint32_t SurfaceExtractionQueueSize() const { return static_cast<uint32_t>(m_pendingSurfaceBricks.size()); }
    const std::vector<SparsePhysicsWorkPacket>& GetLastPhysicsWorkPackets() const { return m_physicsStagedPackets; }
    const std::vector<SparseEditDelta>& GetPendingGpuEditDeltas() const { return m_edits.PendingGpuDeltas(); }
    std::vector<SparseEditDelta> BuildGpuEditDeltaSnapshotForPhysicsWork(uint32_t maxDeltas) const;
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

    void RefreshStats();
    void MarkQueueAccountingDirty();
    void RebuildQueueClassStats();
    void QueueGenerationCoordBack(const BrickCoord& coord);
    bool RemoveFirstGenerationQueueCoord(const BrickCoord& coord);
    bool RemoveFirstGenerationClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    void QueueGenerationClassAliasIfRequested(const BrickCoord& coord);
    bool GenerateQueuedBrick(const BrickCoord& coord, SparseResidencyClass* outResidencyClass);
    bool QueuePhysicsCandidateNoStats(const BrickCoord& coord);
    bool QueuePhysicsRegionNoStats(
        const BrickCoord& coord,
        const SparsePhysicsDirtyRegion& region,
        SparsePhysicsPriority priority);
    void QueueRenderDirtyRegionNoStats(
        const BrickCoord& coord,
        const SparseRenderDirtyRegion& region);
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
    bool PopNextPhysicsWorkPacket(SparsePhysicsWorkPacket* outPacket);
    uint32_t BuildPhysicsWorkBatch(uint32_t maxPackets);
    void RequeuePhysicsWorkPacketNoStats(const SparsePhysicsWorkPacket& packet);
    void QueueUploadCoordBack(const BrickCoord& coord);
    void QueueUploadCoordFront(const BrickCoord& coord);
    bool RemoveFirstUploadQueueCoord(const BrickCoord& coord);
    bool RemoveFirstUploadClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    void QueueUploadClassAliasIfUploadQueued(const BrickCoord& coord);
    void QueueSurfaceExtractionCoord(const BrickCoord& coord);
    bool RemoveFirstSurfaceQueueCoord(const BrickCoord& coord);
    bool RemoveFirstSurfaceClassQueueCoord(const BrickCoord& coord, SparseResidencyClass residencyClass);
    void QueueSurfaceClassAliasIfPending(const BrickCoord& coord);
    bool ExtractSurfaceCoord(const BrickCoord& coord);
    void MarkUploadQueueOrderDirty();
    bool QueueRegeneratedUploadForExistingPage(
        const BrickCoord& coord,
        const SparseRenderDirtyRegion* dirtyRegion = nullptr);
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

    SparseVoxelWorldConfig m_config;
    SparseBrickPool m_pool;
    SparseTerrainGenerator m_terrain;
    SparseEditStore m_edits;
    SparseSurfaceCache m_surfaceCache;
    SparseVoxelWorldStats m_stats;

    std::deque<BrickCoord> m_generationQueue;
    std::array<std::deque<BrickCoord>, 4> m_generationClassQueues;
    std::deque<BrickCoord> m_uploadQueue;
    std::array<std::deque<BrickCoord>, 4> m_uploadClassQueues;
    std::deque<SparsePageInvalidationPacket> m_invalidationQueue;
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash> m_generated;
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash> m_pendingSurfaceBricks;
    std::unordered_map<BrickCoord, bool, BrickCoordHash> m_deferredDirtyAfterUpload;
    std::unordered_map<BrickCoord, SparseRenderDirtyRegion, BrickCoordHash> m_renderDirtyRegions;
    std::unordered_map<BrickCoord, SparseSurfaceLocalRegion, BrickCoordHash> m_surfaceDirtyRegions;
    std::unordered_map<BrickCoord, SparsePhysicsDirtyRegion, BrickCoordHash> m_physicsDirtyRegions;
    std::unordered_map<BrickCoord, SparsePhysicsPriority, BrickCoordHash> m_physicsQueuedPriorities;
    std::unordered_set<BrickCoord, BrickCoordHash> m_knownEmptyGeneratedBricks;
    std::unordered_set<BrickCoord, BrickCoordHash> m_surfaceExtractionQueuedSet;
    std::deque<BrickCoord> m_surfaceExtractionQueue;
    std::array<std::deque<BrickCoord>, 4> m_surfaceClassQueues;
    bool m_generationQueuePriorityDirty = false;
    bool m_uploadQueuePriorityDirty = false;
    bool m_surfaceExtractionQueuePriorityDirty = false;
    bool m_queueClassStatsDirty = true;
    uint32_t m_cachedGenerationQueueSize = 0;
    uint32_t m_cachedUploadQueueSize = 0;
    uint32_t m_cachedSurfacePendingSize = 0;
    QueueClassCounts m_generationQueueClassCounts;
    QueueClassCounts m_uploadQueueClassCounts;
    QueueClassCounts m_surfaceQueueClassCounts;
    std::deque<BrickCoord> m_physicsHotQueue;
    std::deque<BrickCoord> m_physicsWarmQueue;
    std::vector<SparsePhysicsWorkPacket> m_physicsStagedPackets;
    uint32_t m_evictedBricksLastFrame = 0;
    uint32_t m_emptyRequestsSkippedLastFrame = 0;
    uint32_t m_generatedSpeculativeBricksLastFrame = 0;
    uint32_t m_generatedVisibleBricksLastFrame = 0;
    uint32_t m_generatedCollisionBricksLastFrame = 0;
    uint32_t m_generatedEditedBricksLastFrame = 0;
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
    uint32_t m_physicsWorkGeneration = 0;
    bool m_statsRefreshDeferred = false;
    bool m_statsRefreshPending = false;
};

} // namespace VENPOD::Simulation
