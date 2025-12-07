#pragma once

// =============================================================================
// VENPOD Voxel World - Manages ping-pong voxel buffers for GPU simulation
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <glm/glm.hpp>
#include "../Graphics/RHI/GPUBuffer.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

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

private:
    Result<void> CreateVoxelBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager);
    Result<void> CreateMaterialPalette(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, Graphics::DescriptorHeapManager& heapManager);

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
};

} // namespace VENPOD::Simulation
