#pragma once

#include "SparseTerrainGenerator.h"

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Simulation {

struct SparseClipmapConfig {
    bool enabled = true;
    float startDistance = 480.0f;
    float endDistance = 4200.0f;
    float minCellSize = 16.0f;
    float nearExitPadding = 8.0f;
    uint32_t ringCount = 4;
    uint32_t tileRadius = 2;
    uint32_t tileSampleSide = 33;
    uint32_t maxTiles = 128;
    bool voxelClipmapEnabled = true;
    uint32_t voxelBrickRadiusXz = 2;
    uint32_t voxelBrickRadiusY = 1;
    uint32_t maxVoxelBricks = 128;
    uint32_t seed = 12345u;
};

struct SparseClipmapRing {
    float startDistance = 0.0f;
    float endDistance = 0.0f;
    float cellSize = 0.0f;
};

class SparseClipmapPolicy {
public:
    explicit SparseClipmapPolicy(const SparseClipmapConfig& config = {});

    const SparseClipmapConfig& Config() const { return m_config; }
    bool IsEnabled() const;
    float TransitionStartAfterNearExit(float nearExitDistance) const;
    bool OwnsRaySegment(float segmentStartDistance, float segmentEndDistance, float nearExitDistance) const;
    float CellSizeForDistance(float distanceFromCamera) const;
    std::vector<SparseClipmapRing> BuildRings() const;

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

struct SparseClipmapGpuSnapshot {
    std::vector<uint32_t> metadata;
    std::vector<uint32_t> lookup;
    std::vector<uint32_t> samples;
    std::vector<uint32_t> voxelMetadata;
    std::vector<uint32_t> voxelLookup;
    std::vector<uint32_t> voxelSamples;
    uint32_t tileCount = 0;
    uint32_t tileSampleSide = 0;
    uint32_t lookupCapacity = 0;
    uint32_t voxelBrickCount = 0;
    uint32_t voxelLookupCapacity = 0;
    uint32_t voxelDirtyStartSlot = 0;
    uint32_t voxelDirtySlotCount = 0;
    uint32_t frameIndex = 0;
};

struct SparseClipmapCacheStats {
    uint32_t residentTiles = 0;
    uint32_t queuedTiles = 0;
    uint32_t generatedTilesLastFrame = 0;
    uint32_t evictedTilesLastFrame = 0;
    uint32_t dirtySerial = 0;
    uint32_t snapshotTiles = 0;
    uint32_t residentVoxelBricks = 0;
    uint32_t queuedVoxelBricks = 0;
    uint32_t generatedVoxelBricksLastFrame = 0;
    uint32_t evictedVoxelBricksLastFrame = 0;
};

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

class SparseClipmapTileCache {
public:
    bool Initialize(const SparseClipmapConfig& config);
    void UpdateInterest(
        float cameraX,
        float cameraY,
        float cameraZ,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    uint32_t PumpGeneration(uint32_t maxTiles, uint32_t frameIndex, const SparseClipmapPolicy& policy);
    bool BuildGpuSnapshot(SparseClipmapGpuSnapshot& outSnapshot) const;

    const SparseClipmapCacheStats& GetStats() const { return m_stats; }
    uint32_t DirtySerial() const { return m_dirtySerial; }
    uint32_t HeightDirtySerial() const { return m_heightDirtySerial; }
    uint32_t VoxelDirtySerial() const { return m_voxelDirtySerial; }
    void ClearVoxelDirtyRange();

private:
    struct TilePayload {
        SparseClipmapTileRecord record;
        std::vector<uint32_t> packedSamples;
    };

    uint32_t AllocateSlot(const SparseClipmapTileCoord& coord, uint32_t frameIndex);
    void GenerateTile(uint32_t slot, const SparseClipmapPolicy& policy);
    uint32_t PackSample(int32_t worldX, int32_t worldZ, float height) const;
    uint32_t AllocateVoxelSlot(const SparseVoxelClipmapCoord& coord, uint32_t frameIndex);
    void GenerateVoxelBrick(uint32_t slot, const SparseClipmapPolicy& policy);
    void UpdateVoxelInterest(
        float cameraX,
        float cameraY,
        float cameraZ,
        uint32_t frameIndex,
        const SparseClipmapPolicy& policy);
    void MarkVoxelSlotDirty(uint32_t slot);
    void RefreshStats(
        uint32_t generatedLastFrame = 0,
        uint32_t evictedLastFrame = 0,
        uint32_t generatedVoxelLastFrame = 0,
        uint32_t evictedVoxelLastFrame = 0);

    SparseClipmapConfig m_config;
    SparseTerrainGenerator m_terrain;
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
        std::vector<uint32_t> voxels;
    };
    std::vector<VoxelBrickPayload> m_voxelBricks;
    std::vector<uint32_t> m_freeVoxelSlots;
    std::unordered_map<SparseVoxelClipmapCoord, uint32_t, SparseVoxelClipmapCoordHash> m_voxelSlotByCoord;
    std::deque<SparseVoxelClipmapCoord> m_voxelGenerationQueue;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> m_queuedVoxelSet;
    std::unordered_set<SparseVoxelClipmapCoord, SparseVoxelClipmapCoordHash> m_voxelInterestSet;
    SparseClipmapCacheStats m_stats;
    uint32_t m_dirtySerial = 0;
    uint32_t m_heightDirtySerial = 0;
    uint32_t m_voxelDirtySerial = 0;
    uint32_t m_dirtyVoxelStartSlot = UINT32_MAX;
    uint32_t m_dirtyVoxelEndSlot = 0;
};

} // namespace VENPOD::Simulation
