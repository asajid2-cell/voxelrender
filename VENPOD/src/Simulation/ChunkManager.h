#pragma once

// =============================================================================
// VENPOD Chunk Manager - Sparse chunk-based physics optimization
// Tracks active/sleeping chunks to skip idle regions in simulation
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "../Graphics/RHI/GPUBuffer.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// Chunk size in voxels (must be power of 2)
static constexpr uint32_t CHUNK_SIZE = 16;

// Per-chunk control data (GPU-side structure)
// MUST MATCH SharedTypes.hlsli ChunkControl!
struct alignas(16) ChunkControl {
    uint32_t isActive;       // 0 or 1
    uint32_t sleepTimer;     // Frames since last movement
    uint32_t particleCount;  // Debugging metric
    uint32_t padding;        // Align to 16 bytes
};

// Indirect dispatch arguments (for GPU-driven dispatch)
struct IndirectDispatchArgs {
    uint32_t threadGroupCountX;
    uint32_t threadGroupCountY;
    uint32_t threadGroupCountZ;
};

class ChunkManager {
public:
    ChunkManager() = default;
    ~ChunkManager() = default;

    // Non-copyable
    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        Graphics::DescriptorHeapManager& heapManager,
        uint32_t worldSizeX,
        uint32_t worldSizeY,
        uint32_t worldSizeZ
    );

    void Shutdown();

    // Get chunk counts
    uint32_t GetChunkCountX() const { return m_chunkCountX; }
    uint32_t GetChunkCountY() const { return m_chunkCountY; }
    uint32_t GetChunkCountZ() const { return m_chunkCountZ; }
    uint32_t GetTotalChunks() const { return m_chunkCountX * m_chunkCountY * m_chunkCountZ; }

    // Get active chunk list for sparse dispatch
    Graphics::GPUBuffer& GetChunkControlBuffer() { return m_chunkControlBuffer; }
    Graphics::GPUBuffer& GetActiveChunkListBuffer() { return m_activeChunkListBuffer; }
    Graphics::GPUBuffer& GetActiveChunkCountBuffer() { return m_activeChunkCountBuffer; }
    Graphics::GPUBuffer& GetIndirectArgsBuffer() { return m_indirectArgsBuffer; }

    // Shader-visible descriptors
    const Graphics::DescriptorHandle& GetChunkControlUAV() const { return m_chunkControlUAV; }
    const Graphics::DescriptorHandle& GetActiveListUAV() const { return m_activeListUAV; }
    const Graphics::DescriptorHandle& GetActiveListSRV() const { return m_activeListSRV; }
    const Graphics::DescriptorHandle& GetActiveCountUAV() const { return m_activeCountUAV; }
    const Graphics::DescriptorHandle& GetIndirectArgsUAV() const { return m_indirectArgsUAV; }

    // Calculate chunk index from voxel position
    static uint32_t GetChunkIndex(uint32_t voxelX, uint32_t voxelY, uint32_t voxelZ,
                                   uint32_t chunkCountX, uint32_t chunkCountY) {
        uint32_t chunkX = voxelX / CHUNK_SIZE;
        uint32_t chunkY = voxelY / CHUNK_SIZE;
        uint32_t chunkZ = voxelZ / CHUNK_SIZE;
        return chunkX + chunkY * chunkCountX + chunkZ * chunkCountX * chunkCountY;
    }

    // Calculate dispatch size for chunk-based compute
    glm::uvec3 GetChunkDispatchSize(uint32_t threadGroupSize = 4) const {
        return glm::uvec3(
            (m_chunkCountX + threadGroupSize - 1) / threadGroupSize,
            (m_chunkCountY + threadGroupSize - 1) / threadGroupSize,
            (m_chunkCountZ + threadGroupSize - 1) / threadGroupSize
        );
    }

    // Resource barriers
    void TransitionBuffersForCompute(ID3D12GraphicsCommandList* cmdList);
    void TransitionBuffersForIndirect(ID3D12GraphicsCommandList* cmdList);

    // Reset active chunk count (call at start of chunk scan)
    void ResetActiveCount(ID3D12GraphicsCommandList* cmdList);

    // TEMPORARY: Mark all chunks as active (for debugging brush issues)
    void MarkAllChunksActive(ID3D12GraphicsCommandList* cmdList);

private:
    Result<void> CreateBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager);

    uint32_t m_chunkCountX = 0;
    uint32_t m_chunkCountY = 0;
    uint32_t m_chunkCountZ = 0;

    // Chunk control buffer - one ChunkControl per chunk
    Graphics::GPUBuffer m_chunkControlBuffer;
    Graphics::DescriptorHandle m_chunkControlUAV;

    // Active chunk list - indices of chunks that need simulation
    Graphics::GPUBuffer m_activeChunkListBuffer;
    Graphics::DescriptorHandle m_activeListUAV;
    Graphics::DescriptorHandle m_activeListSRV;

    // Active chunk count - single uint32 for indirect dispatch
    Graphics::GPUBuffer m_activeChunkCountBuffer;
    Graphics::DescriptorHandle m_activeCountUAV;

    // Indirect dispatch arguments buffer
    Graphics::GPUBuffer m_indirectArgsBuffer;
    Graphics::DescriptorHandle m_indirectArgsUAV;

    // CPU-side staging for count reset
    ComPtr<ID3D12Resource> m_countResetBuffer;

    Graphics::DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Simulation
