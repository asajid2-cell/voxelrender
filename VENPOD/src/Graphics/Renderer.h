#pragma once

// =============================================================================
// VENPOD Renderer - Main rendering orchestrator
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include "RHI/DX12Device.h"
#include "RHI/DX12CommandQueue.h"
#include "RHI/DescriptorHeap.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/DX12GraphicsPipeline.h"
#include "../Core/Window.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Renderer configuration
struct RendererConfig {
    uint32_t cbvSrvUavDescriptorCount = 4096;
    uint32_t rtvDescriptorCount = 32;
    uint32_t dsvDescriptorCount = 8;
    std::filesystem::path shaderPath;
    bool debugShaders = false;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    // Non-copyable
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Result<void> Initialize(
        DX12Device& device,
        DX12CommandQueue& commandQueue,
        VENPOD::Window& window,
        const RendererConfig& config
    );

    void Shutdown();

    // Begin/End frame for rendering
    void BeginFrame(ID3D12GraphicsCommandList* cmdList, uint32_t frameIndex);
    void EndFrame(ID3D12GraphicsCommandList* cmdList, uint32_t frameIndex);

    // Render the fullscreen pass
    void RenderFullscreen(ID3D12GraphicsCommandList* cmdList);

    // Camera parameters for rendering
    struct CameraParams {
        float posX, posY, posZ;
        float forwardX, forwardY, forwardZ;
        float rightX, rightY, rightZ;
        float upX, upY, upZ;
        float fov;
        float aspectRatio;
    };

    // Brush preview parameters for rendering
    struct BrushPreview {
        float posX, posY, posZ;
        float radius;
        uint32_t material;
        uint32_t shape;
        bool hasValidPosition;
    };

    // Render voxels with raymarch shader (binds voxel resources)
    void RenderVoxels(
        ID3D12GraphicsCommandList* cmdList,
        const DescriptorHandle& voxelGridSRV,
        const DescriptorHandle& materialPaletteSRV,
        const DescriptorHandle& chunkOccupancySRV,  // For empty space skipping optimization
        uint32_t gridSizeX,
        uint32_t gridSizeY,
        uint32_t gridSizeZ,
        const CameraParams& camera,
        float regionOriginX,
        float regionOriginY,
        float regionOriginZ,
        const BrushPreview* brushPreview = nullptr
    );

    // Render crosshair at screen center
    void RenderCrosshair(ID3D12GraphicsCommandList* cmdList);

    // Handle window resize
    Result<void> OnResize(uint32_t width, uint32_t height);

    // Accessors
    DescriptorHeapManager& GetHeapManager() { return m_heapManager; }
    ShaderCompiler& GetShaderCompiler() { return m_shaderCompiler; }
    DX12GraphicsPipeline& GetFullscreenPipeline() { return m_fullscreenPipeline; }
    ID3D12DescriptorHeap* GetShaderVisibleHeap() const { return m_heapManager.GetShaderVisibleCbvSrvUavHeap(); }
    ID3D12Device* GetDevice() const { return m_device->GetDevice(); }

private:
    Result<void> CreateFullscreenPipeline(ID3D12Device* device);
    Result<void> CreateRTVsForSwapchain();

    // References to core systems (not owned)
    DX12Device* m_device = nullptr;
    DX12CommandQueue* m_commandQueue = nullptr;
    VENPOD::Window* m_window = nullptr;

    // Owned resources
    DescriptorHeapManager m_heapManager;
    ShaderCompiler m_shaderCompiler;

    // Fullscreen rendering pipeline
    DX12GraphicsPipeline m_fullscreenPipeline;
    CompiledShader m_fullscreenVS;
    CompiledShader m_fullscreenPS;

    // RTV handles for swapchain buffers
    DescriptorHandle m_rtvHandles[VENPOD::Window::BUFFER_COUNT];

    // Configuration
    RendererConfig m_config;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace VENPOD::Graphics
