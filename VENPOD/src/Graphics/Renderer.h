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
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/DX12GraphicsPipeline.h"
#include "../Core/Window.h"
#include "../Utils/Result.h"
#include <array>

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

struct PendingBackbufferCapture;

// Renderer configuration
struct RendererConfig {
    uint32_t cbvSrvUavDescriptorCount = 4096;
    uint32_t rtvDescriptorCount = 32;
    uint32_t dsvDescriptorCount = 8;
    std::filesystem::path shaderPath;
    bool debugShaders = false;
    bool backgroundPassEnabled = false;
    float backgroundPassScale = 1.0f;
    bool backgroundPassSurfaceRaymarchFill = false;
    bool backgroundPassClearProbe = false;
    bool backgroundPassForceColor = false;
    bool backgroundPassCompositeDebug = false;
    bool backgroundPassCompositeForceColor = false;
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

    struct BackgroundPassInfo {
        bool active = false;
        uint32_t fullWidth = 0;
        uint32_t fullHeight = 0;
        uint32_t backgroundWidth = 0;
        uint32_t backgroundHeight = 0;
        float scale = 1.0f;
        bool surfaceRaymarchFill = false;
        bool clearProbe = false;
        bool forceColor = false;
        bool compositeDebug = false;
        bool compositeForceColor = false;
    };

    BackgroundPassInfo GetBackgroundPassInfo() const;
    bool QueueBackgroundPassCapture(
        ID3D12GraphicsCommandList* cmdList,
        uint32_t frameNumber,
        const std::filesystem::path& outputPath,
        PendingBackbufferCapture& outCapture);

    // Camera parameters for rendering
    struct CameraParams {
        float posX, posY, posZ;
        float forwardX, forwardY, forwardZ;
        float rightX, rightY, rightZ;
        float upX, upY, upZ;
        float fov;
        float aspectRatio;
        float raymarchMaxDistance = 2500.0f;
        uint32_t raymarchMaxSteps = 2048;
        float farFieldQuality = 1.0f;
        float renderQuality = 1.0f;
        uint32_t frameIndex = 0;
        bool renderOwnershipStatsEnabled = false;
        bool backgroundPassSurfaceRaymarchFill = false;
        float midFieldStartDistance = 480.0f;
        float midFieldEndDistance = 4200.0f;
        float midFieldCellSize = 16.0f;
        float midFieldFarHandoffDistance = 2786.4f;
        float midFieldHeightCoverage = 0.0f;
        float midFieldVoxelCoverage = 0.0f;
        float midFieldVoxelInterestCoverage = 0.0f;
        float midFieldVoxelWorstRingCoverage = 0.0f;
        uint32_t midFieldResidentHeightTiles = 0;
        uint32_t midFieldResidentVoxelBricks = 0;
        float surfaceRasterMaxDistance = 0.0f;
        float exactNearDistance = 0.0f;
        uint32_t worldSeed = 12345u;
        uint32_t debugMode = 0;
    };

    // Brush preview parameters for rendering
    struct BrushPreview {
        float posX, posY, posZ;
        float radius;
        uint32_t material;
        uint32_t shape;
        bool hasValidPosition;
    };

    struct CharacterPreview {
        float feetX, feetY, feetZ;
        bool visible;
    };

    struct SparseFarField {
        DescriptorHandle nodeSRV;
        DescriptorHandle pageSRV;
        DescriptorHandle pageIndexSRV;
        uint32_t nodeCount = 0;
        uint32_t pageCount = 0;
        uint32_t pageIndexCount = 0;
        int32_t pageRadius = 0;
        float pageSize = 0.0f;
        float rootMinY = 0.0f;
        uint32_t seed = 12345u;
        float uploadCoverageRatio = 0.0f;
        float pageCoverageRatio = 0.0f;
        bool ready = false;
        bool enabled = false;
    };

    struct SparseNearField {
        DescriptorHandle brickPoolSRV;
        DescriptorHandle pageTableSRV;
        DescriptorHandle occupancySRV;
        DescriptorHandle pageGenerationSRV;
        DescriptorHandle midClipmapMetadataSRV;
        DescriptorHandle midClipmapLookupSRV;
        DescriptorHandle midClipmapSamplesSRV;
        DescriptorHandle midVoxelClipmapMetadataSRV;
        DescriptorHandle midVoxelClipmapLookupSRV;
        DescriptorHandle midVoxelClipmapSamplesSRV;
        DescriptorHandle surfaceFacesSRV;
        DescriptorHandle surfaceRangesSRV;
        DescriptorHandle renderOwnershipUAV;
        uint32_t maxBrickPages = 0;
        uint32_t pageTableCapacity = 0;
        uint32_t midClipmapTileCount = 0;
        uint32_t midClipmapTileSampleSide = 0;
        uint32_t midVoxelClipmapBrickCount = 0;
        uint32_t surfaceFaceCount = 0;
        uint32_t surfaceRangeCount = 0;
        uint32_t surfaceRangeTableCapacity = 0;
        uint32_t surfaceSerial = 0;
        uint32_t bindingMask = 0x3FFu;
        float ownershipCenterX = 0.0f;
        float ownershipCenterY = 0.0f;
        float ownershipCenterZ = 0.0f;
        float ownershipRadius = 0.0f;
        bool enabled = false;
        bool sparseOnly = false;
        bool surfaceAuthoritative = false;
        bool surfaceRaymarchFill = true;
        bool midClipmapEnabled = false;
        bool surfaceEnabled = false;
        bool voxelTerrainOnly = false;
        bool walkingMidVoxelDda = false;
    };

    // Render voxels with raymarch shader (binds voxel resources)
    void RenderVoxels(
        ID3D12GraphicsCommandList* cmdList,
        const DescriptorHandle& voxelGridSRV,
        const DescriptorHandle& chunkValidMaskSRV,
        const DescriptorHandle& materialPaletteSRV,
        uint32_t gridSizeX,
        uint32_t gridSizeY,
        uint32_t gridSizeZ,
        const CameraParams& camera,
        float regionOriginX,
        float regionOriginY,
        float regionOriginZ,
        const BrushPreview* brushPreview = nullptr,
        const CharacterPreview* characterPreview = nullptr,
        const SparseFarField* sparseFarField = nullptr,
        const SparseNearField* sparseNearField = nullptr
    );

    void RenderSparseSurfaceFaces(
        ID3D12GraphicsCommandList* cmdList,
        const DescriptorHandle& surfaceFacesSRV,
        const DescriptorHandle& materialPaletteSRV,
        uint32_t surfaceFaceCount,
        const CameraParams& camera,
        ID3D12Resource* indirectDrawArgs = nullptr,
        uint32_t indirectDrawCommandCount = 0,
        ID3D12Resource* indirectDrawCount = nullptr,
        const D3D12_VERTEX_BUFFER_VIEW* surfaceVertexIdView = nullptr,
        const D3D12_INDEX_BUFFER_VIEW* surfaceIndexView = nullptr,
        uint32_t surfaceVertexIdCapacityFaces = 0,
        const DescriptorHandle* surfaceRecordsSRV = nullptr,
        const DescriptorHandle* surfaceClustersSRV = nullptr,
        const DescriptorHandle* renderOwnershipUAV = nullptr
    );

    void RenderOverlays(
        ID3D12GraphicsCommandList* cmdList,
        const DescriptorHandle& materialPaletteSRV,
        const CameraParams& camera,
        const BrushPreview* brushPreview = nullptr,
        const CharacterPreview* characterPreview = nullptr
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
    Result<void> CreateSparseSurfacePipeline(ID3D12Device* device);
    Result<void> CreateOverlayPipeline(ID3D12Device* device);
    Result<void> CreateBackgroundCompositePipeline(ID3D12Device* device);
    Result<void> CreateSparseSurfaceDrawCommandSignature(ID3D12Device* device);
    Result<void> CreateRTVsForSwapchain();
    Result<void> CreateDepthBuffer();
    Result<void> CreateBackgroundPassResources();
    void DestroyBackgroundPassResources();
    bool UseBackgroundPassSplit() const;
    void SetMainRenderTarget(ID3D12GraphicsCommandList* cmdList);
    void SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList, uint32_t width, uint32_t height);

    // References to core systems (not owned)
    DX12Device* m_device = nullptr;
    DX12CommandQueue* m_commandQueue = nullptr;
    VENPOD::Window* m_window = nullptr;

    // Owned resources
    DescriptorHeapManager m_heapManager;
    ShaderCompiler m_shaderCompiler;
    DescriptorHandle m_imguiReservedSrv;

    // Fullscreen rendering pipeline
    DX12GraphicsPipeline m_fullscreenPipeline;
    DX12GraphicsPipeline m_sparseSurfacePipeline;
    DX12GraphicsPipeline m_overlayPipeline;
    DX12GraphicsPipeline m_backgroundCompositePipeline;
    ComPtr<ID3D12CommandSignature> m_sparseSurfaceDrawSignature;
    CompiledShader m_fullscreenVS;
    CompiledShader m_fullscreenPS;
    CompiledShader m_sparseSurfaceVS;
    CompiledShader m_sparseSurfacePS;
    CompiledShader m_overlayPS;
    CompiledShader m_backgroundCompositePS;
    std::array<UploadBuffer, VENPOD::Window::BUFFER_COUNT> m_frameConstantUploads;
    std::array<UploadBuffer, VENPOD::Window::BUFFER_COUNT> m_sparseSurfaceConstantUploads;
    std::array<UploadBuffer, VENPOD::Window::BUFFER_COUNT> m_overlayConstantUploads;
    GPUBuffer m_dummyRenderOwnershipUAV;
    uint32_t m_currentFrameIndex = 0;
    static constexpr uint64_t kFrameConstantUploadBytes = 512;

    // RTV handles for swapchain buffers
    DescriptorHandle m_rtvHandles[VENPOD::Window::BUFFER_COUNT];
    DescriptorHandle m_dsvHandle;
    ComPtr<ID3D12Resource> m_depthBuffer;
    DescriptorHandle m_backgroundPassRtv;
    DescriptorHandle m_backgroundPassDsv;
    DescriptorHandle m_backgroundPassStagingSrv;
    DescriptorHandle m_backgroundPassSrv;
    ComPtr<ID3D12Resource> m_backgroundPassColor;
    ComPtr<ID3D12Resource> m_backgroundPassDepth;
    D3D12_RESOURCE_STATES m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t m_backgroundPassWidth = 0;
    uint32_t m_backgroundPassHeight = 0;
    bool m_backgroundPassSurfaceRaymarchFillLastFrame = false;

    // Configuration
    RendererConfig m_config;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace VENPOD::Graphics
