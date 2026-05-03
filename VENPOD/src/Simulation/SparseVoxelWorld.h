#pragma once

#include "SparseBrickPool.h"
#include "SparseCollision.h"
#include "SparseEditStore.h"
#include "SparseSurfaceCache.h"
#include "SparseTerrainGenerator.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace VENPOD::Simulation {

struct SparseVoxelWorldConfig {
    uint32_t maxBrickPages = 4096;
    uint32_t pageTableCapacity = 16384;
    uint32_t seed = 12345u;
};

struct SparseVoxelWorldStats {
    uint32_t requestedBricks = 0;
    uint32_t generationQueuedBricks = 0;
    uint32_t generatedBricks = 0;
    uint32_t uploadQueuedBricks = 0;
    uint32_t residentBricks = 0;
    uint32_t freePages = 0;
    uint32_t residentSpeculativeBricks = 0;
    uint32_t residentVisibleBricks = 0;
    uint32_t residentCollisionBricks = 0;
    uint32_t residentEditedBricks = 0;
    uint32_t evictionQueuedBricks = 0;
    uint32_t evictedBricksLastFrame = 0;
    uint32_t editedBricks = 0;
    uint32_t editedVoxels = 0;
    uint32_t brushVoxelsEvaluatedLastStroke = 0;
    uint32_t brushVoxelsEditedLastStroke = 0;
    uint32_t brushBricksTouchedLastStroke = 0;
    uint32_t brushBricksQueuedLastStroke = 0;
    uint32_t surfaceCachedBricks = 0;
    uint32_t surfaceFaces = 0;
    uint32_t surfaceFacesGeneratedLastFrame = 0;
    uint32_t surfaceBricksUpdatedLastFrame = 0;
    uint32_t surfaceBricksRemovedLastFrame = 0;
    uint32_t surfaceSerial = 0;
};

struct SparseBrickUploadPacket {
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
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

class SparseVoxelWorld {
public:
    bool Initialize(const SparseVoxelWorldConfig& config = {});
    void BeginFrame();

    bool RequestBrick(const BrickCoord& coord);
    uint32_t PumpGeneration(uint32_t maxBricks, uint32_t currentFrame = 0);
    bool PopNextUpload(SparseBrickUploadPacket* outPacket);
    bool RequeueUploadFront(const SparseBrickUploadPacket& packet);
    bool CompleteUpload(const SparseBrickUploadPacket& packet);
    uint32_t TrimResidentBricks(
        const BrickCoord& center,
        uint32_t keepRadiusXz,
        uint32_t keepRadiusY,
        uint32_t maxEvictions);
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
        bool requestRenderBricks = true);
    CollisionSampleStatus SampleCollisionStatus(int32_t worldX, int32_t worldY, int32_t worldZ) const;
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
    const SparseSurfaceCache& GetSurfaceCache() const { return m_surfaceCache; }

private:
    void RefreshStats();
    bool QueueRegeneratedUploadForExistingPage(const BrickCoord& coord);
    uint32_t SampleEditedOrGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const;

    SparseVoxelWorldConfig m_config;
    SparseBrickPool m_pool;
    SparseTerrainGenerator m_terrain;
    SparseEditStore m_edits;
    SparseSurfaceCache m_surfaceCache;
    SparseVoxelWorldStats m_stats;

    std::deque<BrickCoord> m_generationQueue;
    std::deque<BrickCoord> m_uploadQueue;
    std::deque<SparsePageInvalidationPacket> m_invalidationQueue;
    std::unordered_map<BrickCoord, GeneratedSparseBrick, BrickCoordHash> m_generated;
    std::unordered_map<BrickCoord, bool, BrickCoordHash> m_deferredDirtyAfterUpload;
    uint32_t m_evictedBricksLastFrame = 0;
};

} // namespace VENPOD::Simulation
