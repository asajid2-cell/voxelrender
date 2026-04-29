#pragma once

// =============================================================================
// VENPOD Chunk - Individual 64³ voxel region for infinite world
// Each chunk holds its own voxel buffer and generation state
// =============================================================================

#include <d3d12.h>
#include <cstdint>
#include "ChunkCoord.h"
#include "../Graphics/RHI/GPUBuffer.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

namespace VENPOD::Simulation {

// Chunk size in voxels (must match shader constant)
static constexpr uint32_t INFINITE_CHUNK_SIZE = 64;

// Chunk generation state
enum class ChunkState {
    Ungenerated,         // Chunk allocated but not generated yet
    GenerationSubmitted, // GPU commands submitted, waiting for completion
    Generated,           // Generation complete, ready for rendering
    Dirty                // Needs physics update or regeneration
};

// Individual chunk in infinite world
class Chunk {
public:
    Chunk() = default;

    // CRITICAL FIX: Explicit destructor to ensure descriptors are always freed
    // This prevents descriptor leaks when Initialize() fails partway through
    // or when chunks are deleted without calling Shutdown() explicitly
    ~Chunk() { Shutdown(); }

    // Non-copyable
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    // Movable
    Chunk(Chunk&& other) noexcept;
    Chunk& operator=(Chunk&& other) noexcept;

    // Initialize chunk (allocate GPU buffer)
    Result<void> Initialize(
        ID3D12Device* device,
        Graphics::DescriptorHeapManager& heapManager,
        const ChunkCoord& coord,
        const char* debugNamePrefix = "Chunk"
    );

    void Shutdown();

    // Generate chunk using compute shader
    // This dispatches CS_GenerateChunk.hlsl with world offset
    // OPTIMIZATION: Uses shared constant buffer instead of creating one per chunk
    Result<void> Generate(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12PipelineState* generationPSO,
        ID3D12RootSignature* rootSignature,
        ID3D12Resource* sharedConstantBuffer,
        void* sharedConstantBufferMappedPtr,
        uint32_t worldSeed
    );

    // Mark chunk as needing physics update
    void MarkDirty() { m_state = ChunkState::Dirty; }

    // Mark chunk as generated (called after GPU fence signals completion)
    void MarkGenerated() { m_state = ChunkState::Generated; }

    // Mark chunk as submitted (GPU work in flight)
    void MarkSubmitted() { m_state = ChunkState::GenerationSubmitted; }

    // FIX #16: Invalidate descriptors without freeing (used for deferred descriptor freeing)
    // This prevents Shutdown() from freeing descriptors that will be freed later
    void InvalidateDescriptors() {
        m_voxelSRV.Invalidate();
        m_voxelUAV.Invalidate();
    }

    // Getters
    const ChunkCoord& GetCoord() const { return m_coord; }
    ChunkState GetState() const { return m_state; }
    bool IsGenerated() const { return m_state == ChunkState::Generated || m_state == ChunkState::Dirty; }
    bool IsSubmitted() const { return m_state == ChunkState::GenerationSubmitted; }
    bool IsDirty() const { return m_state == ChunkState::Dirty; }

    // Get world origin position (in voxel coordinates)
    void GetWorldOrigin(int32_t& outX, int32_t& outY, int32_t& outZ) const {
        m_coord.GetWorldOrigin(outX, outY, outZ, INFINITE_CHUNK_SIZE);
    }

    // GPU buffer access
    Graphics::GPUBuffer& GetVoxelBuffer() { return m_voxelBuffer; }
    const Graphics::GPUBuffer& GetVoxelBuffer() const { return m_voxelBuffer; }

    // Transition chunk buffer to specified state (updates internal tracking)
    void TransitionBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

    // Shader-visible descriptors for rendering/compute
    const Graphics::DescriptorHandle& GetVoxelSRV() const { return m_voxelSRV; }
    const Graphics::DescriptorHandle& GetVoxelUAV() const { return m_voxelUAV; }

    // Calculate total voxel count
    static constexpr uint32_t GetVoxelCount() {
        return INFINITE_CHUNK_SIZE * INFINITE_CHUNK_SIZE * INFINITE_CHUNK_SIZE;
    }

    // Calculate buffer size in bytes (4 bytes per voxel = 1 MB per chunk)
    static constexpr uint64_t GetBufferSize() {
        return static_cast<uint64_t>(GetVoxelCount()) * 4;  // 64³ * 4 = 1,048,576 bytes
    }

private:
    ChunkCoord m_coord;                   // Position in chunk grid
    ChunkState m_state = ChunkState::Ungenerated;

    // GPU voxel buffer (64³ voxels = 1 MB)
    Graphics::GPUBuffer m_voxelBuffer;

    // Shader-visible descriptors (for rendering and compute access)
    Graphics::DescriptorHandle m_voxelSRV;  // For reading in shaders
    Graphics::DescriptorHandle m_voxelUAV;  // For writing in compute shaders

    // Resource state tracking (prevents invalid transitions that cause device removal)
    D3D12_RESOURCE_STATES m_currentVoxelState = D3D12_RESOURCE_STATE_COMMON;

    Graphics::DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Simulation
