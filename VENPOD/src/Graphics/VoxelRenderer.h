#pragma once

// =============================================================================
// VENPOD Voxel Renderer - GPU-driven voxel rendering with DDA raymarching
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <glm/glm.hpp>
#include "Renderer.h"
#include "RHI/GPUBuffer.h"
#include "RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Frame constants passed to shaders (must match SharedTypes.hlsli)
struct FrameConstants {
    glm::mat4 invViewProjection;
    glm::vec4 cameraPosition;     // xyz = position, w = unused
    glm::vec4 sunDirection;       // xyz = direction, w = intensity
    glm::vec4 timeParams;         // x = time, y = deltaTime, z = frameIndex, w = unused
    glm::vec4 worldBounds;        // xyz = gridSize, w = voxelScale
    glm::uvec4 debugParams;       // x = debugMode, y = clipPlane, z,w = unused
};

// Voxel renderer configuration
struct VoxelRendererConfig {
    uint32_t gridSizeX = 256;
    uint32_t gridSizeY = 256;
    uint32_t gridSizeZ = 256;
    float voxelScale = 1.0f;
};

class VoxelRenderer {
public:
    VoxelRenderer() = default;
    ~VoxelRenderer() = default;

    // Non-copyable
    VoxelRenderer(const VoxelRenderer&) = delete;
    VoxelRenderer& operator=(const VoxelRenderer&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        const VoxelRendererConfig& config
    );

    void Shutdown();

    // Update frame constants
    void UpdateFrameConstants(const FrameConstants& constants);

    // Render voxels (binds resources and issues draw)
    void Render(ID3D12GraphicsCommandList* cmdList, DX12GraphicsPipeline& pipeline);

    // Accessors
    GPUBuffer& GetVoxelBufferA() { return m_voxelBufferA; }
    GPUBuffer& GetVoxelBufferB() { return m_voxelBufferB; }
    ConstantBuffer& GetFrameConstantBuffer() { return m_frameConstantBuffer; }

    uint32_t GetGridSizeX() const { return m_config.gridSizeX; }
    uint32_t GetGridSizeY() const { return m_config.gridSizeY; }
    uint32_t GetGridSizeZ() const { return m_config.gridSizeZ; }
    uint64_t GetTotalVoxels() const {
        return static_cast<uint64_t>(m_config.gridSizeX) *
               static_cast<uint64_t>(m_config.gridSizeY) *
               static_cast<uint64_t>(m_config.gridSizeZ);
    }

private:
    Result<void> CreateVoxelBuffers(ID3D12Device* device, DescriptorHeapManager& heapManager);
    Result<void> CreateMaterialPalette(ID3D12Device* device, DescriptorHeapManager& heapManager);

    // Ping-pong voxel buffers (for physics simulation)
    GPUBuffer m_voxelBufferA;  // Current state
    GPUBuffer m_voxelBufferB;  // Next state

    // Material palette texture
    ComPtr<ID3D12Resource> m_materialPalette;
    DescriptorHandle m_paletteSRV;

    // Frame constants buffer
    ConstantBuffer m_frameConstantBuffer;

    // Configuration
    VoxelRendererConfig m_config;

    // Track which buffer is current
    bool m_useBufferA = true;

    DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Graphics
