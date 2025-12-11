#pragma once

// =============================================================================
// VENPOD Infinite Chunk Manager - Dynamic chunk loading for infinite worlds
// Loads/unloads chunks based on camera position
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>
#include <queue>
#include <vector>
#include <functional>
#include <glm/glm.hpp>
#include "Chunk.h"
#include "ChunkCoord.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// Configuration for infinite chunk manager
struct InfiniteChunkConfig {
    // ===== STREAMING DISTANCE CONFIGURATION =====
    // These create a "buffer zone" where loading/unloading happens out of sight:
    // - loadDistanceHorizontal: Where chunks START generating (beyond visible area)
    // - unloadDistanceHorizontal: Where chunks get DELETED (far beyond load area)
    // The gap between load and unload prevents thrashing when moving back and forth.

    int32_t loadDistanceHorizontal = 16;   // ±16 chunks = 33×33 = 2178 chunks loading
    int32_t unloadDistanceHorizontal = 20; // ±20 chunks before deletion (4-chunk hysteresis)

    // Vertical is fixed to terrain layers (Y=0,1) - these are mostly unused
    int32_t loadDistanceVertical = 2;      // UNUSED - fixed to TERRAIN_NUM_CHUNKS_Y
    int32_t unloadDistanceVertical = 3;    // Unload chunks beyond terrain layers

    uint32_t chunksPerFrame = 1;           // Generate 1 chunk per frame (safe, no GPU flooding)
    uint32_t maxQueuedChunks = 64;         // Queue up to 64 chunks (enough for streaming)
    uint32_t worldSeed = 12345;            // Procedural generation seed
};

// Manager for infinite voxel world
class InfiniteChunkManager {
public:
    InfiniteChunkManager() = default;
    ~InfiniteChunkManager() = default;

    // Non-copyable
    InfiniteChunkManager(const InfiniteChunkManager&) = delete;
    InfiniteChunkManager& operator=(const InfiniteChunkManager&) = delete;

    // Initialize manager (does NOT generate chunks yet)
    Result<void> Initialize(
        ID3D12Device* device,
        Graphics::DescriptorHeapManager& heapManager,
        const InfiniteChunkConfig& config = {}
    );

    void Shutdown();

    // Update chunk loading based on camera position
    // Call this every frame to load nearby chunks and unload distant ones
    // NOTE: Now uses internal command list for immediate execution (doesn't touch frame cmdList!)
    void Update(
        ID3D12Device* device,
        ID3D12CommandQueue* cmdQueue,  // CHANGED: Need queue for immediate execution
        const glm::vec3& cameraWorldPos
    );

    // Get chunk at coordinate (returns nullptr if not loaded)
    Chunk* GetChunk(const ChunkCoord& coord);
    const Chunk* GetChunk(const ChunkCoord& coord) const;

    // Get all loaded chunks for rendering
    const std::unordered_map<ChunkCoord, Chunk*>& GetLoadedChunks() const { return m_loadedChunks; }

    // Get number of loaded chunks
    size_t GetLoadedChunkCount() const { return m_loadedChunks.size(); }

    // Get generation queue size (for debugging)
    size_t GetGenerationQueueSize() const { return m_generationQueue.size(); }

    // PRIORITY 2: Get current camera chunk coordinate (for chunk scan optimization)
    ChunkCoord GetCameraChunk() const { return m_cameraChunk; }

    // Get world seed
    uint32_t GetWorldSeed() const { return m_config.worldSeed; }

    // Configuration
    const InfiniteChunkConfig& GetConfig() const { return m_config; }
    void SetLoadDistanceHorizontal(int32_t distance) { m_config.loadDistanceHorizontal = distance; }
    void SetUnloadDistanceHorizontal(int32_t distance) { m_config.unloadDistanceHorizontal = distance; }

    // Force generation of specific chunk (for testing)
    Result<void> ForceGenerateChunk(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const ChunkCoord& coord
    );

    // Get chunk size in voxels
    static constexpr uint32_t GetChunkSize() { return INFINITE_CHUNK_SIZE; }

    // CRITICAL FIX: Public method to verify generated chunks
    // Call this before UpdateActiveRegion to catch chunks that just finished
    void PollCompletedChunks();

    // Get generation pipeline
    ID3D12PipelineState* GetGenerationPSO() const { return m_generationPSO.Get(); }
    ID3D12RootSignature* GetGenerationRootSig() const { return m_generationRootSignature.Get(); }

    // CACHE FIX: Set callback to notify when chunks are unloaded
    using ChunkUnloadCallback = std::function<void(const ChunkCoord&)>;
    void SetUnloadCallback(ChunkUnloadCallback callback) { m_unloadCallback = callback; }

private:
    // Internal chunk management
    Result<void> QueueChunksAroundCamera(const ChunkCoord& cameraChunk);
    Result<void> GenerateNextChunk(ID3D12Device* device, ID3D12CommandQueue* cmdQueue);  // CHANGED: Uses internal cmdList + executes immediately
    void UnloadDistantChunks(const ChunkCoord& cameraChunk);

    // Create generation compute pipeline
    Result<void> CreateGenerationPipeline(ID3D12Device* device);

    InfiniteChunkConfig m_config;

    // Loaded chunks (hash map for O(1) access)
    std::unordered_map<ChunkCoord, Chunk*> m_loadedChunks;

    // Chunks waiting to be generated - PRIORITY QUEUE (nearest to camera first)
    // This ensures chunks near the player generate before distant ones!
    struct ChunkPriorityEntry {
        ChunkCoord coord;
        int32_t distanceSquared;  // Distance from camera (lower = higher priority)

        // Priority queue is max-heap, so use > for min-heap behavior (nearest first)
        bool operator<(const ChunkPriorityEntry& other) const {
            return distanceSquared > other.distanceSquared;  // Invert for min-heap
        }
    };
    std::priority_queue<ChunkPriorityEntry> m_generationQueue;

    // Last camera chunk position (to avoid redundant updates)
    ChunkCoord m_lastCameraChunk = ChunkCoord{INT32_MAX, INT32_MAX, INT32_MAX};

    // FIX #1: Rate limiting for stationary camera - prevents infinite generation loops
    uint32_t m_stationaryFrameCount = 0;

    // FIX #9: Startup phase tracking - prevents unloading during initial chunk generation
    bool m_startupPhase = true;
    uint32_t m_chunksGeneratedSinceStartup = 0;

    // PRIORITY 2: Current camera chunk (for chunk scan optimization)
    ChunkCoord m_cameraChunk = ChunkCoord{0, 0, 0};

    // Generation compute shader pipeline
    ComPtr<ID3D12PipelineState> m_generationPSO;
    ComPtr<ID3D12RootSignature> m_generationRootSignature;

    // OPTIMIZATION: Reusable constant buffer for chunk generation (instead of creating one per chunk!)
    ComPtr<ID3D12Resource> m_sharedConstantBuffer;
    void* m_sharedConstantBufferMappedPtr = nullptr;  // Persistent mapping

    // CRITICAL FIX: Dedicated command allocator + list for chunk generation
    // We CANNOT reuse the frame's command list because:
    // 1. Chunks need immediate execution (can't wait until end of frame)
    // 2. Multiple chunks per frame would flood the command list
    // 3. Synchronization issues - we read buffers before GPU completes

    // RING BUFFER FIX: Use 3 allocators to prevent reuse while GPU is executing
    static constexpr uint32_t NUM_FRAME_BUFFERS = 3;
    ComPtr<ID3D12CommandAllocator> m_chunkCmdAllocators[NUM_FRAME_BUFFERS];
    ComPtr<ID3D12GraphicsCommandList> m_chunkCmdList;
    uint32_t m_currentAllocatorIndex = 0;

    // GPU FENCE: Track chunk generation completion
    ComPtr<ID3D12Fence> m_chunkFence;
    uint64_t m_chunkFenceValue = 0;
    uint64_t m_allocatorFenceValues[NUM_FRAME_BUFFERS] = {0, 0, 0};  // Track when each allocator was last used
    uint32_t m_allocatorSkipCounts[NUM_FRAME_BUFFERS] = {0, 0, 0};   // Per-allocator skip counters
    HANDLE m_chunkFenceEvent = nullptr;

    // FIX #1: Track per-chunk fence values to verify when generation completes
    std::unordered_map<ChunkCoord, uint64_t> m_chunkGenerationFences;

    // FIX: Track re-queue attempts to prevent infinite loops when allocators are busy
    std::unordered_map<ChunkCoord, uint32_t> m_chunkRequeueCount;

    // FIX #16/#19: Deferred chunk deletion to prevent GPU accessing freed buffers
    // The bug: Chunks (including GPU buffers) were freed immediately on unload, but GPU
    // might still be using them → OBJECT_DELETED_WHILE_STILL_IN_USE crash
    // Solution: Queue entire chunk for deferred delete, only free after GPU finishes using it
    struct DeferredChunkDelete {
        Chunk* chunk;            // Chunk to delete (includes buffers AND descriptors)
        uint64_t fenceValue;     // Delete when GPU reaches this fence value
    };
    std::vector<DeferredChunkDelete> m_deferredChunkDeletes;

    // Process deferred chunk deletions (call each frame)
    void ProcessDeferredChunkDeletes();

    // Helper to verify completed chunks and mark them as Generated
    void VerifyGeneratedChunks();

    // D3D12 device and heap manager references
    ID3D12Device* m_device = nullptr;
    Graphics::DescriptorHeapManager* m_heapManager = nullptr;

    // CACHE FIX: Callback to notify when chunks are unloaded
    ChunkUnloadCallback m_unloadCallback;
};

} // namespace VENPOD::Simulation
