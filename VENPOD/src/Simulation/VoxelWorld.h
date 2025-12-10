#pragma once

// =============================================================================
// VENPOD Voxel World - Manages ping-pong voxel buffers for GPU simulation
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_set>
#include <glm/glm.hpp>
#include "../Graphics/RHI/GPUBuffer.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"
#include "ChunkCoord.h"  // Need this before InfiniteChunkManager for unordered_set
#include "InfiniteChunkManager.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// World configuration
struct VoxelWorldConfig {
    uint32_t gridSizeX = 256;     // Default 256^3 grid
    uint32_t gridSizeY = 256;
    uint32_t gridSizeZ = 256;
    float voxelScale = 1.0f;       // World units per voxel
};

// Material properties for simulation
struct MaterialProperties {
    float density = 1.0f;
    float friction = 0.5f;
    float restitution = 0.3f;
    uint32_t flags = 0;  // Solid, liquid, gas, etc.
};

class VoxelWorld {
public:
    VoxelWorld() = default;
    ~VoxelWorld() = default;

    // Non-copyable
    VoxelWorld(const VoxelWorld&) = delete;
    VoxelWorld& operator=(const VoxelWorld&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        Graphics::DescriptorHeapManager& heapManager,
        const VoxelWorldConfig& config = {}
    );

    void Shutdown();

    // Swap ping-pong buffers (call after physics dispatch)
    void SwapBuffers();

    // Get current read/write buffers
    Graphics::GPUBuffer& GetReadBuffer() { return m_voxelBuffers[m_readBufferIndex]; }
    Graphics::GPUBuffer& GetWriteBuffer() { return m_voxelBuffers[1 - m_readBufferIndex]; }
    const Graphics::GPUBuffer& GetReadBuffer() const { return m_voxelBuffers[m_readBufferIndex]; }
    const Graphics::GPUBuffer& GetWriteBuffer() const { return m_voxelBuffers[1 - m_readBufferIndex]; }

    // Material palette texture
    ID3D12Resource* GetMaterialPalette() const { return m_materialPalette.Get(); }
    const Graphics::DescriptorHandle& GetPaletteSRV() const { return m_paletteShaderVisibleSRV; }

    // Shader-visible descriptors (for rendering/compute - already in shader-visible heap)
    const Graphics::DescriptorHandle& GetReadBufferSRV() const { return m_shaderVisibleSRVs[m_readBufferIndex]; }
    const Graphics::DescriptorHandle& GetReadBufferUAV() const { return m_shaderVisibleUAVs[m_readBufferIndex]; }
    const Graphics::DescriptorHandle& GetWriteBufferSRV() const { return m_shaderVisibleSRVs[1 - m_readBufferIndex]; }
    const Graphics::DescriptorHandle& GetWriteBufferUAV() const { return m_shaderVisibleUAVs[1 - m_readBufferIndex]; }

    // Grid properties
    uint32_t GetGridSizeX() const { return m_config.gridSizeX; }
    uint32_t GetGridSizeY() const { return m_config.gridSizeY; }
    uint32_t GetGridSizeZ() const { return m_config.gridSizeZ; }
    uint32_t GetTotalVoxels() const { return m_config.gridSizeX * m_config.gridSizeY * m_config.gridSizeZ; }
    float GetVoxelScale() const { return m_config.voxelScale; }
    glm::vec3 GetWorldSize() const {
        return glm::vec3(
            m_config.gridSizeX * m_config.voxelScale,
            m_config.gridSizeY * m_config.voxelScale,
            m_config.gridSizeZ * m_config.voxelScale
        );
    }

    // Calculate thread groups for compute dispatch
    glm::uvec3 GetDispatchSize(uint32_t threadGroupSize = 8) const {
        return glm::uvec3(
            (m_config.gridSizeX + threadGroupSize - 1) / threadGroupSize,
            (m_config.gridSizeY + threadGroupSize - 1) / threadGroupSize,
            (m_config.gridSizeZ + threadGroupSize - 1) / threadGroupSize
        );
    }

    // Resource barriers
    void TransitionReadBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);
    void TransitionWriteBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);

    // GPU brush raycasting (NEW - replaces CPU readback)
    // Get the GPU buffer that stores brush raycast results (16 bytes)
    Graphics::GPUBuffer& GetBrushRaycastResultBuffer() { return m_brushRaycastResult; }
    const Graphics::GPUBuffer& GetBrushRaycastResultBuffer() const { return m_brushRaycastResult; }

    // CPU readback for brush raycasting result (16 bytes only!)
    struct BrushRaycastResult {
        float posX, posY, posZ;
        uint32_t normalPacked;  // Packed normal + valid flag
        bool hasValidPosition;
    };

    // Request tiny readback of brush raycast result (16 bytes vs 32 MB!)
    void RequestBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList);

    // Get CPU-side brush raycast result (updated after RequestBrushRaycastReadback)
    BrushRaycastResult GetBrushRaycastResult() const { return m_brushRaycastCPU; }

    // ===== INFINITE CHUNK SYSTEM (NEW) =====
    // Update chunk loading and active region (call every frame)
    void UpdateChunks(
        ID3D12Device* device,
        ID3D12CommandQueue* cmdQueue,  // CHANGED: Uses internal cmdList for immediate execution
        const glm::vec3& cameraPos
    );

    // Get chunk manager (for debugging/stats)
    InfiniteChunkManager* GetChunkManager() { return m_chunkManager.get(); }

    // Get chunk copy fence (for synchronization in physics dispatcher)
    ID3D12Fence* GetChunkCopyFence() const { return m_chunkCopyFence.Get(); }
    uint64_t GetChunkCopyFenceValue() const { return m_chunkCopyFenceValue; }

    // CACHE FIX: Notify VoxelWorld when chunk is unloaded (clears copy cache)
    void OnChunkUnloaded(const ChunkCoord& coord);

    // CRITICAL FIX: Invalidate cache for chunk that was modified (painted voxels)
    // Call this after painting voxels to ensure the chunk gets re-copied
    void InvalidateCopiedChunk(const ChunkCoord& coord);

    // Toggle infinite chunks on/off (for testing)
    void SetUseInfiniteChunks(bool enabled) { m_useInfiniteChunks = enabled; }
    bool IsUsingInfiniteChunks() const { return m_useInfiniteChunks && m_chunkManager != nullptr; }

    // STRESS TEST SUPPORT: Access chunk manager for testing
    InfiniteChunkManager* GetChunkManager() const { return m_chunkManager.get(); }

    // STRESS TEST SUPPORT: Get copied chunk count for cache validation
    size_t GetCopiedChunkCount(int bufferIndex) const {
        if (bufferIndex < 0 || bufferIndex >= 2) return 0;
        return m_copiedChunksPerBuffer[bufferIndex].size();
    }
    int GetReadBufferIndex() const { return m_readBufferIndex; }

    // Origin of the 256^3 render buffer in world voxel coordinates.
    // In finite 256^3 mode this remains (0,0,0) to preserve behavior.
    glm::vec3 GetRegionOriginWorld() const { return m_regionOriginWorld; }

    // DEBUG SUPPORT: Copy a fixed 2x2 layout of infinite chunks at coordinates
    // (0,0,0), (1,0,0), (0,0,1), (1,0,1) into the WRITE 256x128x256 buffer.
    // This uses the same chunk copy pipeline as UpdateActiveRegion but bypasses
    // streaming logic entirely so copy/origin issues can be debugged in isolation.
    void CopyStatic2x2Chunks(ID3D12CommandQueue* cmdQueue);

private:
    Result<void> CreateVoxelBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager);
    Result<void> CreateMaterialPalette(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, Graphics::DescriptorHeapManager& heapManager);
    Result<void> CreateChunkCopyPipeline(ID3D12Device* device);

    // Update active region by copying nearby chunks into 256^3 render buffer
    void UpdateActiveRegion(ID3D12Device* device, ID3D12CommandQueue* cmdQueue, bool chunkChanged = false);

    VoxelWorldConfig m_config;

    // Ping-pong voxel buffers (read from one, write to other)
    Graphics::GPUBuffer m_voxelBuffers[2];
    uint32_t m_readBufferIndex = 0;

    // Shader-visible descriptors for voxel buffers (persistent, no per-frame copying)
    Graphics::DescriptorHandle m_shaderVisibleSRVs[2];  // SRV for each buffer
    Graphics::DescriptorHandle m_shaderVisibleUAVs[2];  // UAV for each buffer

    // Material palette (256 colors)
    ComPtr<ID3D12Resource> m_materialPalette;
    ComPtr<ID3D12Resource> m_paletteUpload;
    Graphics::DescriptorHandle m_paletteSRV;  // Staging SRV
    Graphics::DescriptorHandle m_paletteShaderVisibleSRV;  // Shader-visible SRV

    // Heap manager reference for cleanup
    Graphics::DescriptorHeapManager* m_heapManager = nullptr;

    // GPU brush raycasting (NEW - 16 bytes vs 32 MB!)
    Graphics::GPUBuffer m_brushRaycastResult;  // 16-byte buffer for raycast result
    ComPtr<ID3D12Resource> m_brushRaycastReadback;  // 16-byte CPU readback
    BrushRaycastResult m_brushRaycastCPU;  // CPU copy of result

    // ===== INFINITE CHUNK SYSTEM (NEW) =====
    std::unique_ptr<InfiniteChunkManager> m_chunkManager;
    bool m_useInfiniteChunks = true;  // Toggle for testing (set false to use old 256^3 system)

    // Active region tracking (which chunk is at center of render buffer)
    ChunkCoord m_activeRegionCenter;
    bool m_activeRegionNeedsUpdate = true;

    // World-space origin (in voxel coordinates) that maps to (0,0,0) in the 256^3 buffer
    // when using infinite chunks. This is derived from m_activeRegionCenter and the
    // chunk layout in UpdateActiveRegion. In non-infinite mode it stays at (0,0,0).
    glm::vec3 m_regionOriginWorld = glm::vec3(0.0f);

    // PERFORMANCE OPTIMIZATION: Double-buffered chunk tracking
    // Each buffer has its own cache of which chunks are already copied
    // This prevents re-copying ALL chunks every frame (only copy missing/changed ones)
    std::unordered_set<ChunkCoord> m_copiedChunksPerBuffer[2];

    // Cache invalidation tracking - used to boost chunk copy speed after cache clear
    // When cache is invalidated (camera moves to new chunk), we need to aggressively
    // refill BOTH buffers to prevent holes/missing chunks during the refill period
    int32_t m_framesAfterCacheInvalidation = 0;

    // Chunk copy pipeline (for UpdateActiveRegion)
    ComPtr<ID3D12PipelineState> m_chunkCopyPSO;
    ComPtr<ID3D12RootSignature> m_chunkCopyRootSignature;
    ComPtr<ID3D12Resource> m_chunkCopyConstantBuffer;
    void* m_chunkCopyConstantBufferMappedPtr = nullptr;

    // RING BUFFER FIX: Use 3 allocators for chunk copy to prevent reuse while GPU executing
    static constexpr uint32_t NUM_COPY_BUFFERS = 3;
    ComPtr<ID3D12CommandAllocator> m_chunkCopyCmdAllocators[NUM_COPY_BUFFERS];
    ComPtr<ID3D12GraphicsCommandList> m_chunkCopyCmdList;
    uint32_t m_currentCopyAllocatorIndex = 0;

    // GPU FENCE: Track chunk copy completion
    ComPtr<ID3D12Fence> m_chunkCopyFence;
    uint64_t m_chunkCopyFenceValue = 0;
    uint64_t m_copyAllocatorFenceValues[NUM_COPY_BUFFERS] = {0, 0, 0};
    HANDLE m_chunkCopyFenceEvent = nullptr;
};

} // namespace VENPOD::Simulation
