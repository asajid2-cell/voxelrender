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
#include <glm/glm.hpp>
#include "Chunk.h"
#include "ChunkCoord.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// Configuration for infinite chunk manager
struct InfiniteChunkConfig {
    // Cylindrical loading pattern (not spherical) to reduce VRAM usage
    int32_t renderDistanceHorizontal = 8;  // Load ±8 chunks in X/Z (17×17 = 289 chunks per layer)
    int32_t renderDistanceVertical = 2;    // Load ±2 chunks in Y (5 layers total)
    // Total: 17×17×5 = 1,445 chunks × 1 MB = 1.4 GB VRAM max

    int32_t unloadDistanceHorizontal = 10; // Unload chunks beyond 10 chunks horizontally
    int32_t unloadDistanceVertical = 4;    // Unload chunks beyond 4 chunks vertically

    uint32_t chunksPerFrame = 1;           // Generate 1-4 chunks per frame (1=smooth, 4=fast loading)
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
    void Update(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
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

    // Get world seed
    uint32_t GetWorldSeed() const { return m_config.worldSeed; }

    // Configuration
    const InfiniteChunkConfig& GetConfig() const { return m_config; }
    void SetRenderDistanceHorizontal(int32_t distance) { m_config.renderDistanceHorizontal = distance; }
    void SetRenderDistanceVertical(int32_t distance) { m_config.renderDistanceVertical = distance; }

    // Force generation of specific chunk (for testing)
    Result<void> ForceGenerateChunk(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const ChunkCoord& coord
    );

    // Get chunk size in voxels
    static constexpr uint32_t GetChunkSize() { return INFINITE_CHUNK_SIZE; }

    // Get generation pipeline
    ID3D12PipelineState* GetGenerationPSO() const { return m_generationPSO.Get(); }
    ID3D12RootSignature* GetGenerationRootSig() const { return m_generationRootSignature.Get(); }

private:
    // Internal chunk management
    Result<void> QueueChunksAroundCamera(const ChunkCoord& cameraChunk);
    Result<void> GenerateNextChunk(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
    void UnloadDistantChunks(const ChunkCoord& cameraChunk);

    // Create generation compute pipeline
    Result<void> CreateGenerationPipeline(ID3D12Device* device);

    InfiniteChunkConfig m_config;

    // Loaded chunks (hash map for O(1) access)
    std::unordered_map<ChunkCoord, Chunk*> m_loadedChunks;

    // Chunks waiting to be generated
    std::queue<ChunkCoord> m_generationQueue;

    // Last camera chunk position (to avoid redundant updates)
    ChunkCoord m_lastCameraChunk = ChunkCoord{INT32_MAX, INT32_MAX, INT32_MAX};

    // Generation compute shader pipeline
    ComPtr<ID3D12PipelineState> m_generationPSO;
    ComPtr<ID3D12RootSignature> m_generationRootSignature;

    // D3D12 device and heap manager references
    ID3D12Device* m_device = nullptr;
    Graphics::DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Simulation
