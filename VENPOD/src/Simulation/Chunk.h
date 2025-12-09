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
    Ungenerated,    // Chunk allocated but not generated yet
    Generating,     // Currently being generated on GPU
    Generated,      // Generation complete, ready for rendering
    Dirty           // Needs physics update or regeneration
};

// Individual chunk in infinite world
class Chunk {
public:
    Chunk() = default;
    ~Chunk() = default;

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
    Result<void> Generate(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12PipelineState* generationPSO,
        ID3D12RootSignature* rootSignature,
        uint32_t worldSeed
    );

    // Mark chunk as needing physics update
    void MarkDirty() { m_state = ChunkState::Dirty; }

    // Getters
    const ChunkCoord& GetCoord() const { return m_coord; }
    ChunkState GetState() const { return m_state; }
    bool IsGenerated() const { return m_state == ChunkState::Generated || m_state == ChunkState::Dirty; }
    bool IsDirty() const { return m_state == ChunkState::Dirty; }

    // Get world origin position (in voxel coordinates)
    void GetWorldOrigin(int32_t& outX, int32_t& outY, int32_t& outZ) const {
        m_coord.GetWorldOrigin(outX, outY, outZ, INFINITE_CHUNK_SIZE);
    }

    // GPU buffer access
    Graphics::GPUBuffer& GetVoxelBuffer() { return m_voxelBuffer; }
    const Graphics::GPUBuffer& GetVoxelBuffer() const { return m_voxelBuffer; }

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

    Graphics::DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Simulation
