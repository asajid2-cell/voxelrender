#pragma once

// =============================================================================
// VENPOD Voxel World - Manages ping-pong voxel buffers for GPU simulation
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>
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

struct VoxelWorldStreamingStats {
    uint32_t copyBudget = 0;
    uint32_t chunksCopiedLastFrame = 0;
    uint32_t chunksSkippedLastFrame = 0;
    uint32_t chunksNotGeneratedLastFrame = 0;
    uint32_t chunksNotLoadedLastFrame = 0;
    uint32_t urgentVisibleChunksQueuedLastFrame = 0;
    uint32_t chunksCheckedLastFrame = 0;
    uint32_t cachedReadChunks = 0;
    uint32_t cachedWriteChunks = 0;
    uint32_t expectedVisibleChunks = 0;
    uint32_t readSlotMismatches = 0;
    uint32_t writeSlotMismatches = 0;
    uint32_t toroidalSlotCount = 0;
    uint32_t loadedChunkRecords = 0;
    uint32_t generatedChunks = 0;
    uint32_t queuedChunks = 0;
    uint32_t swapSkippedForCopyFence = 0;
    int32_t activeChunkX = 0;
    int32_t activeChunkY = 0;
    int32_t activeChunkZ = 0;
    int32_t renderMinY = 0;
    int32_t renderMaxY = 0;
    uint64_t visibleVoxelCapacity = 0;
    uint64_t loadedVoxelCapacity = 0;
    uint32_t editedChunks = 0;
    uint32_t editedVoxels = 0;
    uint32_t editsAppliedLastFrame = 0;
    uint32_t chunksWithEditsAppliedLastFrame = 0;
    int32_t targetWorldX = 0;
    int32_t targetWorldY = 0;
    int32_t targetWorldZ = 0;
    int32_t targetChunkX = 0;
    int32_t targetChunkY = 0;
    int32_t targetChunkZ = 0;
    uint32_t targetLocalX = 0;
    uint32_t targetLocalY = 0;
    uint32_t targetLocalZ = 0;
    int32_t targetNormalX = 0;
    int32_t targetNormalY = 0;
    int32_t targetNormalZ = 0;
    uint32_t targetHasPersistentEdit = 0;
    uint32_t lastEditOverlayApplied = 0;
    uint32_t brushVoxelsEvaluatedLastStroke = 0;
    uint32_t brushVoxelsRejectedLastStroke = 0;
    uint32_t persistentEditsRecordedLastStroke = 0;
    uint32_t gpuBrushFeedbackQueued = 0;
    uint32_t gpuBrushFeedbackPending = 0;
    uint32_t gpuBrushEventsAppliedLastFrame = 0;
    uint32_t gpuBrushEventsDroppedLastFrame = 0;
    uint32_t gpuBrushEventsOverflowLastFrame = 0;
    int32_t lastRecenterDeltaX = 0;
    int32_t lastRecenterDeltaY = 0;
    int32_t lastRecenterDeltaZ = 0;
    int32_t lastRecenterOldOriginX = 0;
    int32_t lastRecenterOldOriginY = 0;
    int32_t lastRecenterOldOriginZ = 0;
    int32_t lastRecenterNewOriginX = 0;
    int32_t lastRecenterNewOriginY = 0;
    int32_t lastRecenterNewOriginZ = 0;
    uint32_t lastRecenterFrame = 0;
    uint32_t lastRecenterPlayerChanged = 0;
    char lastRecenterReason[32] = "none";
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
    const Graphics::DescriptorHandle& GetReadChunkValidMaskSRV() const { return m_chunkValidMaskSRVs[m_readBufferIndex]; }
    glm::vec3 WorldToRenderLocal(const glm::vec3& worldPosition) const;
    bool RenderLocalToWorld(const glm::vec3& localPosition, glm::vec3& outWorldPosition) const;
    bool IsWorldChunkCachedForRead(const ChunkCoord& coord) const;
    bool IsWorldChunkCachedForWrite(const ChunkCoord& coord) const;
    bool IsWorldChunkCachedForReadWrite(const ChunkCoord& coord) const;
    bool IsWorldVoxelCachedForRead(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    bool IsWorldVoxelCachedForReadWrite(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    bool EnsureWorldBrushVolumeCachedForReadWrite(float worldX, float worldY, float worldZ, float radius);
    void RecordPersistentWorldBrushEdit(
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
        bool hasHitNormal);
    void RequestActiveRegionRefresh(uint32_t frames = 8);

    // Grid properties
    uint32_t GetGridSizeX() const { return m_config.gridSizeX; }
    uint32_t GetGridSizeY() const { return m_config.gridSizeY; }
    uint32_t GetGridSizeZ() const { return m_config.gridSizeZ; }
    uint32_t GetTotalVoxels() const { return m_config.gridSizeX * m_config.gridSizeY * m_config.gridSizeZ; }
    uint64_t GetTotalVoxels64() const {
        return static_cast<uint64_t>(m_config.gridSizeX) *
               static_cast<uint64_t>(m_config.gridSizeY) *
               static_cast<uint64_t>(m_config.gridSizeZ);
    }
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
    void SetChunkGenerationBudget(uint32_t chunksPerFrame);
    void PumpChunkGeneration(ID3D12Device* device, ID3D12CommandQueue* cmdQueue);

    // GPU brush raycasting (NEW - replaces CPU readback)
    // Get the GPU buffer that stores brush raycast results (16 bytes)
    Graphics::GPUBuffer& GetBrushRaycastResultBuffer() { return m_brushRaycastResult; }
    const Graphics::GPUBuffer& GetBrushRaycastResultBuffer() const { return m_brushRaycastResult; }

    // GPU ground detection raycasting (for player collision)
    Graphics::GPUBuffer& GetGroundRaycastResultBuffer() { return m_groundRaycastResult; }
    const Graphics::GPUBuffer& GetGroundRaycastResultBuffer() const { return m_groundRaycastResult; }

    // CPU readback for brush raycasting result (16 bytes only!)
    struct BrushRaycastResult {
        float posX, posY, posZ;
        uint32_t normalPacked;  // Packed normal + valid flag
        bool hasValidPosition;
    };

    // Request tiny readback of brush raycast result (16 bytes vs 32 MB!)
    void RequestBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList);
    void QueueBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList, uint32_t slotIndex);
    bool RetireBrushRaycastReadback(uint32_t slotIndex);

    // Get CPU-side brush raycast result (updated after RequestBrushRaycastReadback)
    BrushRaycastResult GetBrushRaycastResult() const { return m_brushRaycastCPU; }

    // Ground detection raycast (separate from brush)
    struct GroundRaycastResult {
        float posX, posY, posZ;
        uint32_t normalPacked;
        bool hasValidPosition;
    };

    // Request ground raycast readback (16 bytes)
    void RequestGroundRaycastReadback(ID3D12GraphicsCommandList* cmdList);
    void QueueGroundRaycastReadback(ID3D12GraphicsCommandList* cmdList, uint32_t slotIndex);
    bool RetireGroundRaycastReadback(uint32_t slotIndex);

    // Get CPU-side ground raycast result
    GroundRaycastResult GetGroundRaycastResult() const { return m_groundRaycastCPU; }

    // ===== INFINITE CHUNK SYSTEM (NEW) =====
    // Update chunk loading and active region (call every frame)
    // RETURNS: Origin shift delta for diagnostics only. Player/camera world
    //          position must remain stable when the render window recenters.
    glm::vec3 UpdateChunks(
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

    // Persistent edit overlay: records a brush in world/chunk coordinates so
    // painted traversal edits survive render-window recentering and chunk reloads.
    void RecordPersistentBrushEdit(
        float localPositionX,
        float localPositionY,
        float localPositionZ,
        float radius,
        uint32_t material,
        uint32_t mode,
        uint32_t shape,
        float strength,
        uint32_t seed,
        int32_t hitNormalX = 0,
        int32_t hitNormalY = 0,
        int32_t hitNormalZ = 0,
        bool hasHitNormal = false
    );
    bool BeginBrushEditFeedback(ID3D12GraphicsCommandList* cmdList);
    void QueueBrushEditFeedbackReadback(ID3D12GraphicsCommandList* cmdList);
    void NotifyBrushEditFeedbackFence(uint64_t fenceValue);
    void RetireBrushEditFeedback(uint64_t completedFenceValue);
    const Graphics::DescriptorHandle& GetBrushEditEventUAV() const { return m_brushEditEventBuffer.GetShaderVisibleUAV(); }
    const Graphics::DescriptorHandle& GetBrushEditCounterUAV() const { return m_brushEditCounterBuffer.GetShaderVisibleUAV(); }
    uint32_t GetMaxBrushEditFeedbackEvents() const { return MAX_BRUSH_EDIT_FEEDBACK_EVENTS; }

    bool HasPersistentEditAtWorldVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    void UpdateTargetVoxelDebug(
        int32_t worldX,
        int32_t worldY,
        int32_t worldZ,
        int32_t normalX = 0,
        int32_t normalY = 0,
        int32_t normalZ = 0
    );
    static bool RunEditOverlayCoordinateSelfTest();

    // Toggle infinite chunks on/off (for testing)
    void SetUseInfiniteChunks(bool enabled) { m_useInfiniteChunks = enabled; }
    bool IsUsingInfiniteChunks() const { return m_useInfiniteChunks && m_chunkManager != nullptr; }

    // STRESS TEST SUPPORT: Access chunk manager for testing
    InfiniteChunkManager* GetChunkManager() const { return m_chunkManager.get(); }

    const VoxelWorldStreamingStats& GetStreamingStats() const { return m_streamingStats; }
    void SetMaxChunkCopiesPerFrame(uint32_t maxCopies) {
        m_maxChunkCopiesPerFrame = (maxCopies == 0) ? 1 : maxCopies;
    }
    uint32_t GetMaxChunkCopiesPerFrame() const { return m_maxChunkCopiesPerFrame; }

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
    Result<void> CreateEditApplyPipeline(ID3D12Device* device);
    Result<void> CreateBrushEditFeedbackBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager);
    Result<void> EnsureEditUploadCapacity(ID3D12Device* device, uint32_t uploadSlot, uint32_t entryCount);

    // Update active region by copying nearby chunks into 256^3 render buffer
    void UpdateActiveRegion(ID3D12Device* device, ID3D12CommandQueue* cmdQueue, bool chunkChanged = false);

    struct CopiedChunkTarget {
        ChunkCoord coord;
        int32_t destX = 0;
        int32_t destY = 0;
        int32_t destZ = 0;
    };

    struct ChunkEditOverlay {
        std::unordered_map<uint32_t, uint32_t> voxels;
    };

    struct GpuEditEntry {
        uint32_t targetIndex = 0;
        uint32_t packedVoxel = 0;
    };

    struct GpuBrushEditEvent {
        uint32_t localRenderIndex = 0;
        uint32_t packedVoxel = 0;
    };

    void StorePersistentVoxelEdit(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel);
    bool TryGetPersistentVoxelEdit(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t* outVoxel = nullptr) const;
    void RefreshPersistentEditStats();
    void ResetBrushFeedbackStats();
    uint32_t ApplyPersistentEditsForCopiedChunks(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        uint32_t uploadSlot,
        const std::vector<CopiedChunkTarget>& copiedChunks,
        Graphics::GPUBuffer& targetBuffer
    );

    VoxelWorldConfig m_config;

    // Ping-pong voxel buffers (read from one, write to other)
    Graphics::GPUBuffer m_voxelBuffers[2];
    uint32_t m_readBufferIndex = 0;

    // Shader-visible descriptors for voxel buffers (persistent, no per-frame copying)
    Graphics::DescriptorHandle m_shaderVisibleSRVs[2];  // SRV for each buffer
    Graphics::DescriptorHandle m_shaderVisibleUAVs[2];  // UAV for each buffer
    Graphics::GPUBuffer m_chunkValidMasks[2];
    Graphics::DescriptorHandle m_chunkValidMaskSRVs[2];
    Graphics::DescriptorHandle m_chunkValidMaskUAVs[2];
    std::array<std::vector<ChunkCoord>, 2> m_chunkSlotWorldCoords;

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

    // GPU ground detection raycasting (for player collision)
    Graphics::GPUBuffer m_groundRaycastResult;  // 16-byte buffer for ground raycast
    ComPtr<ID3D12Resource> m_groundRaycastReadback;  // 16-byte CPU readback
    GroundRaycastResult m_groundRaycastCPU;  // CPU copy of result

    static constexpr uint32_t RAYCAST_READBACK_SLOTS = 3;
    std::array<ComPtr<ID3D12Resource>, RAYCAST_READBACK_SLOTS> m_brushRaycastReadbackSlots;
    std::array<ComPtr<ID3D12Resource>, RAYCAST_READBACK_SLOTS> m_groundRaycastReadbackSlots;
    std::array<bool, RAYCAST_READBACK_SLOTS> m_brushRaycastReadbackReady = {};
    std::array<bool, RAYCAST_READBACK_SLOTS> m_groundRaycastReadbackReady = {};

    static constexpr uint32_t MAX_BRUSH_EDIT_FEEDBACK_EVENTS = 131072;
    static constexpr uint32_t BRUSH_EDIT_FEEDBACK_READBACK_SLOTS = 4;

    struct BrushEditFeedbackSlot {
        Graphics::GPUBuffer eventReadback;
        Graphics::GPUBuffer counterReadback;
        bool pending = false;
        uint64_t fenceValue = 0;
        uint32_t bufferIndex = 0;
        glm::vec3 regionOriginWorld = glm::vec3(0.0f);
    };

    Graphics::GPUBuffer m_brushEditEventBuffer;
    Graphics::GPUBuffer m_brushEditCounterBuffer;
    Graphics::GPUBuffer m_brushEditCounterResetUpload;
    std::array<BrushEditFeedbackSlot, BRUSH_EDIT_FEEDBACK_READBACK_SLOTS> m_brushEditFeedbackSlots;
    uint32_t m_nextBrushEditFeedbackSlot = 0;
    int32_t m_activeBrushEditFeedbackSlot = -1;
    bool m_brushEditFeedbackAvailable = false;

    // ===== INFINITE CHUNK SYSTEM (NEW) =====
    std::unique_ptr<InfiniteChunkManager> m_chunkManager;
    bool m_useInfiniteChunks = true;  // Toggle for testing (set false to use old 256^3 system)

    // Active region tracking (which chunk is at center of render buffer)
    ChunkCoord m_activeRegionCenter = ChunkCoord(INT32_MAX, INT32_MAX, INT32_MAX);  // Sentinel for "not initialized"
    ChunkCoord m_lastRenderCameraChunk = ChunkCoord(INT32_MAX, INT32_MAX, INT32_MAX);
    bool m_activeRegionNeedsUpdate = true;
    bool m_firstUpdateDone = false;  // Flag to track first UpdateChunks call

    // World-space origin (in voxel coordinates) that maps to (0,0,0) in the 256^3 buffer
    // when using infinite chunks. This is derived from m_activeRegionCenter and the
    // chunk layout in UpdateActiveRegion. In non-infinite mode it stays at (0,0,0).
    glm::vec3 m_regionOriginWorld = glm::vec3(0.0f);

    // PERFORMANCE OPTIMIZATION: Double-buffered chunk tracking
    // Each buffer has its own cache of which chunks are already copied
    // This prevents re-copying ALL chunks every frame (only copy missing/changed ones)
    std::unordered_set<ChunkCoord> m_copiedChunksPerBuffer[2];

    // PAINTED CHUNK PROTECTION: Track chunks that have been modified by painting
    // These chunks should NEVER be re-copied from source because:
    // - Source chunks only have original generated terrain
    // - Painted voxels exist only in the render buffer
    // - Re-copying would overwrite user's painted content
    // Chunks stay in this set until they leave the render distance (far enough that
    // the user won't notice if their paint is lost when they return)
    std::unordered_set<ChunkCoord> m_modifiedChunks;

    // Persistent sparse user edits keyed by stable world chunk coordinate.
    // Each overlay stores only touched local voxels, not a dense 64^3 array.
    std::unordered_map<ChunkCoord, ChunkEditOverlay> m_editOverlays;
    uint32_t m_totalEditedVoxels = 0;

    // Cache invalidation tracking - used to boost chunk copy speed after cache clear
    // When cache is invalidated (camera moves to new chunk), we need to aggressively
    // refill BOTH buffers to prevent holes/missing chunks during the refill period
    int32_t m_framesAfterCacheInvalidation = 0;
    uint32_t m_forcedActiveRegionUpdateFrames = 0;
    uint32_t m_maxChunkCopiesPerFrame = 24;
    VoxelWorldStreamingStats m_streamingStats = {};

    // Buffer stability tracking - prevent swaps during refill to avoid visual artifacts.
    // When false, SwapBuffers() keeps presenting the current read buffer while
    // UpdateActiveRegion fills the new window center-out.
    bool m_buffersStable = true;

    // RING BUFFER FIX: Use 3 allocators for chunk copy to prevent reuse while GPU executing
    static constexpr uint32_t NUM_COPY_BUFFERS = 3;
    static constexpr uint32_t NUM_EDIT_UPLOAD_SLOTS = NUM_COPY_BUFFERS * 2;

    // Chunk copy pipeline (for UpdateActiveRegion)
    ComPtr<ID3D12PipelineState> m_chunkCopyPSO;
    ComPtr<ID3D12RootSignature> m_chunkCopyRootSignature;
    ComPtr<ID3D12Resource> m_chunkCopyConstantBuffer;
    void* m_chunkCopyConstantBufferMappedPtr = nullptr;

    // Persistent edit overlay apply pipeline.
    ComPtr<ID3D12PipelineState> m_editApplyPSO;
    ComPtr<ID3D12RootSignature> m_editApplyRootSignature;
    ComPtr<ID3D12Resource> m_editUploadBuffers[NUM_EDIT_UPLOAD_SLOTS];
    void* m_editUploadMappedPtrs[NUM_EDIT_UPLOAD_SLOTS] = {};
    uint32_t m_editUploadCapacities[NUM_EDIT_UPLOAD_SLOTS] = {};

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
