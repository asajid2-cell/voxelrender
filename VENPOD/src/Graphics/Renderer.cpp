#include "Renderer.h"
#include "RHI/d3dx12.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

Result<void> Renderer::Initialize(
    DX12Device& device,
    DX12CommandQueue& commandQueue,
    VENPOD::Window& window,
    const RendererConfig& config)
{
    m_device = &device;
    m_commandQueue = &commandQueue;
    m_window = &window;
    m_config = config;
    m_width = window.GetWidth();
    m_height = window.GetHeight();

    // Initialize descriptor heap manager
    auto result = m_heapManager.Initialize(
        device.GetDevice(),
        config.cbvSrvUavDescriptorCount,
        config.rtvDescriptorCount,
        config.dsvDescriptorCount
    );
    if (!result) {
        return Error("Failed to initialize descriptor heap manager: {}", result.error());
    }

    // Initialize shader compiler
    result = m_shaderCompiler.Initialize();
    if (!result) {
        return Error("Failed to initialize shader compiler: {}", result.error());
    }

    // Set shader include path
    m_shaderCompiler.SetIncludePath(config.shaderPath);

    // Create RTVs for swapchain
    result = CreateRTVsForSwapchain();
    if (!result) {
        return Error("Failed to create RTVs: {}", result.error());
    }

    // Create fullscreen pipeline
    result = CreateFullscreenPipeline(device.GetDevice());
    if (!result) {
        return Error("Failed to create fullscreen pipeline: {}", result.error());
    }

    spdlog::info("Renderer initialized ({}x{})", m_width, m_height);
    return {};
}

void Renderer::Shutdown() {
    // Free RTV handles
    for (auto& handle : m_rtvHandles) {
        if (handle.IsValid()) {
            m_heapManager.FreeRtv(handle);
        }
    }

    m_fullscreenPipeline.Shutdown();
    m_shaderCompiler.Shutdown();
    m_heapManager.Shutdown();

    m_device = nullptr;
    m_commandQueue = nullptr;
    m_window = nullptr;
}

void Renderer::BeginFrame(ID3D12GraphicsCommandList* cmdList, uint32_t frameIndex) {
    if (!cmdList || !m_window) return;

    // Get current back buffer
    ID3D12Resource* backBuffer = m_window->GetBackBuffer(frameIndex);
    if (!backBuffer) return;

    // Transition to render target
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    cmdList->ResourceBarrier(1, &barrier);

    // Set render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHandles[frameIndex].cpu;
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Set viewport and scissor
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(m_width);
    scissor.bottom = static_cast<LONG>(m_height);
    cmdList->RSSetScissorRects(1, &scissor);

    // Clear render target
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
}

void Renderer::EndFrame(ID3D12GraphicsCommandList* cmdList, uint32_t frameIndex) {
    if (!cmdList || !m_window) return;

    // Get current back buffer
    ID3D12Resource* backBuffer = m_window->GetBackBuffer(frameIndex);
    if (!backBuffer) return;

    // Transition to present
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT
    );
    cmdList->ResourceBarrier(1, &barrier);
}

void Renderer::RenderFullscreen(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;

    // Bind fullscreen pipeline (sets root signature, PSO, and topology)
    m_fullscreenPipeline.Bind(cmdList);

    // Draw fullscreen triangle (3 vertices, no vertex buffer needed)
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void Renderer::RenderVoxels(
    ID3D12GraphicsCommandList* cmdList,
    const DescriptorHandle& voxelGridSRV,
    const DescriptorHandle& materialPaletteSRV,
    const DescriptorHandle& chunkOccupancySRV,
    uint32_t gridSizeX,
    uint32_t gridSizeY,
    uint32_t gridSizeZ,
    const CameraParams& camera,
    float regionOriginX,
    float regionOriginY,
    float regionOriginZ,
    const BrushPreview* brushPreview)
{
    if (!cmdList) return;

    // Set descriptor heaps (required before using shader-visible descriptors)
    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind fullscreen pipeline
    m_fullscreenPipeline.Bind(cmdList);

    // Create frame constants on stack (will be passed as root constants)
    // Must match SharedTypes.hlsli FrameConstants exactly - 40 DWORDs (added 4 for regionOrigin)
    struct FrameConstants {
        float cameraPosition[4];      // xyz = pos, w = fov
        float cameraForward[4];       // xyz = forward, w = aspectRatio
        float cameraRight[4];         // xyz = right, w = unused
        float cameraUp[4];            // xyz = up, w = unused
        float sunDirection[4];        // xyz = direction, w = intensity
        uint32_t gridSizeX;
        uint32_t gridSizeY;
        uint32_t gridSizeZ;
        float voxelScale;
        float viewportWidth;
        float viewportHeight;
        uint32_t frameIndex;
        uint32_t debugMode;
        float regionOrigin[4];        // xyz = world origin, w = unused - CRITICAL FOR INFINITE WORLD!
        float brushPosition[4];       // xyz = position, w = radius
        float brushParams[4];         // x = material, y = shape, z = hasValidPosition, w = unused
    } constants = {};

    // Fill in camera data
    constants.cameraPosition[0] = camera.posX;
    constants.cameraPosition[1] = camera.posY;
    constants.cameraPosition[2] = camera.posZ;
    constants.cameraPosition[3] = camera.fov;

    constants.cameraForward[0] = camera.forwardX;
    constants.cameraForward[1] = camera.forwardY;
    constants.cameraForward[2] = camera.forwardZ;
    constants.cameraForward[3] = camera.aspectRatio;

    constants.cameraRight[0] = camera.rightX;
    constants.cameraRight[1] = camera.rightY;
    constants.cameraRight[2] = camera.rightZ;
    constants.cameraRight[3] = 0.0f;

    constants.cameraUp[0] = camera.upX;
    constants.cameraUp[1] = camera.upY;
    constants.cameraUp[2] = camera.upZ;
    constants.cameraUp[3] = 0.0f;

    // Sun direction (default lighting)
    constants.sunDirection[0] = 0.5f;
    constants.sunDirection[1] = 1.0f;
    constants.sunDirection[2] = 0.3f;
    constants.sunDirection[3] = 1.0f;

    // Fill in grid dimensions
    constants.gridSizeX = gridSizeX;
    constants.gridSizeY = gridSizeY;
    constants.gridSizeZ = gridSizeZ;
    constants.voxelScale = 1.0f;
    constants.viewportWidth = static_cast<float>(m_width);
    constants.viewportHeight = static_cast<float>(m_height);
    constants.frameIndex = 0;
    constants.debugMode = 0;

    // CRITICAL FIX: Fill in region origin for infinite world
    // Shader MUST subtract this from world coords to sample correct buffer location
    constants.regionOrigin[0] = regionOriginX;
    constants.regionOrigin[1] = regionOriginY;
    constants.regionOrigin[2] = regionOriginZ;
    constants.regionOrigin[3] = 0.0f;  // unused

    // Fill in brush preview data (if provided)
    if (brushPreview && brushPreview->hasValidPosition) {
        constants.brushPosition[0] = brushPreview->posX;
        constants.brushPosition[1] = brushPreview->posY;
        constants.brushPosition[2] = brushPreview->posZ;
        constants.brushPosition[3] = brushPreview->radius;
        constants.brushParams[0] = static_cast<float>(brushPreview->material);
        constants.brushParams[1] = static_cast<float>(brushPreview->shape);
        constants.brushParams[2] = 1.0f;  // hasValidPosition = true
        constants.brushParams[3] = 0.0f;
    } else {
        constants.brushPosition[0] = 0.0f;
        constants.brushPosition[1] = 0.0f;
        constants.brushPosition[2] = 0.0f;
        constants.brushPosition[3] = 0.0f;
        constants.brushParams[0] = 0.0f;
        constants.brushParams[1] = 0.0f;
        constants.brushParams[2] = 0.0f;  // hasValidPosition = false
        constants.brushParams[3] = 0.0f;
    }

    // Set root constants (b0)
    cmdList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

    // Use persistent shader-visible descriptors directly (no per-frame copy needed)
    cmdList->SetGraphicsRootDescriptorTable(1, voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);

    // Bind chunk occupancy texture for empty space skipping (t2 in shader)
    if (chunkOccupancySRV.IsValid()) {
        cmdList->SetGraphicsRootDescriptorTable(3, chunkOccupancySRV.gpu);
    }

    // Draw fullscreen triangle
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void Renderer::RenderCrosshair(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;

    // Crosshair dimensions
    const uint32_t crosshairSize = 10;  // Length of each line from center
    const uint32_t crosshairThickness = 2;  // Thickness in pixels

    // Calculate center position
    uint32_t centerX = m_width / 2;
    uint32_t centerY = m_height / 2;

    // Bind fullscreen pipeline (for simple rendering)
    m_fullscreenPipeline.Bind(cmdList);

    // Set viewport to full screen
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    // Draw horizontal line (left and right of center)
    D3D12_RECT horizontalScissor = {};
    horizontalScissor.left = centerX - crosshairSize;
    horizontalScissor.right = centerX + crosshairSize;
    horizontalScissor.top = centerY - crosshairThickness / 2;
    horizontalScissor.bottom = centerY + crosshairThickness / 2;
    cmdList->RSSetScissorRects(1, &horizontalScissor);

    // Draw white color using a simple fullscreen pass
    // We'll use the fullscreen pipeline with modified constants to render white
    struct CrosshairConstants {
        float color[4];  // RGBA
        uint32_t padding[24];  // Padding to match FrameConstants size (28 DWORDs)
    } crosshairData = {};
    crosshairData.color[0] = 1.0f;  // R
    crosshairData.color[1] = 1.0f;  // G
    crosshairData.color[2] = 1.0f;  // B
    crosshairData.color[3] = 1.0f;  // A

    cmdList->SetGraphicsRoot32BitConstants(0, 4, &crosshairData, 0);
    cmdList->DrawInstanced(3, 1, 0, 0);

    // Draw vertical line (top and bottom of center)
    D3D12_RECT verticalScissor = {};
    verticalScissor.left = centerX - crosshairThickness / 2;
    verticalScissor.right = centerX + crosshairThickness / 2;
    verticalScissor.top = centerY - crosshairSize;
    verticalScissor.bottom = centerY + crosshairSize;
    cmdList->RSSetScissorRects(1, &verticalScissor);

    cmdList->DrawInstanced(3, 1, 0, 0);

    // Restore full scissor rect
    D3D12_RECT fullScissor = {};
    fullScissor.right = static_cast<LONG>(m_width);
    fullScissor.bottom = static_cast<LONG>(m_height);
    cmdList->RSSetScissorRects(1, &fullScissor);
}

Result<void> Renderer::OnResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    // Recreate RTVs after window resize
    return CreateRTVsForSwapchain();
}

Result<void> Renderer::CreateFullscreenPipeline(ID3D12Device* device) {
    // Compile shaders
    std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_Fullscreen.hlsl";
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_Raymarch.hlsl";

    auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
    if (!vsResult) {
        return Error("Failed to compile vertex shader: {}", vsResult.error());
    }
    m_fullscreenVS = vsResult.value();
    if (!m_fullscreenVS.IsValid()) {
        return Error("Vertex shader compilation failed: {}", m_fullscreenVS.errors);
    }

    auto psResult = m_shaderCompiler.CompilePixelShader(psPath, L"main", m_config.debugShaders);
    if (!psResult) {
        return Error("Failed to compile pixel shader: {}", psResult.error());
    }
    m_fullscreenPS = psResult.value();
    if (!m_fullscreenPS.IsValid()) {
        return Error("Pixel shader compilation failed: {}", m_fullscreenPS.errors);
    }

    // Create pipeline
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_fullscreenVS;
    pipelineDesc.pixelShader = m_fullscreenPS;
    pipelineDesc.debugName = "FullscreenPipeline";

    // Root signature parameters (for voxel rendering)
    // b0: FrameConstants (inline 32-bit constants)
    // Layout: camPos(4) + camFwd(4) + camRight(4) + camUp(4) + sunDir(4) + grid(4) + viewport(4) + regionOrigin(4) + brushPos(4) + brushParams(4) = 40 DWORDs
    RootParameter frameConstantsParam;
    frameConstantsParam.type = RootParamType::Constants32Bit;
    frameConstantsParam.shaderRegister = 0;  // register b0
    frameConstantsParam.registerSpace = 0;   // space 0
    frameConstantsParam.visibility = D3D12_SHADER_VISIBILITY_ALL;
    frameConstantsParam.num32BitValues = 40;  // sizeof(FrameConstants) / 4 - UPDATED for regionOrigin!
    pipelineDesc.rootParams.push_back(frameConstantsParam);

    // t0: VoxelGrid SRV (descriptor table for structured buffer)
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,  // register t0
        0,  // space 0
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,  // numDescriptors
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t1: MaterialPalette texture (descriptor table)
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        1,  // register t1
        0,  // space 0
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,  // numDescriptors
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t2: ChunkOccupancy 3D texture (for empty space skipping acceleration)
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        2,  // register t2
        0,  // space 0
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,  // numDescriptors
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // Static sampler s0
    pipelineDesc.staticSamplers.push_back({
        0,  // register s0
        0,  // space 0
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });

    // Render target format (swapchain format)
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);

    // No input layout needed for fullscreen triangle
    pipelineDesc.inputLayout.clear();

    // No depth testing for fullscreen pass
    pipelineDesc.depthEnable = false;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;

    auto result = m_fullscreenPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create fullscreen pipeline: {}", result.error());
    }

    spdlog::info("Fullscreen pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateRTVsForSwapchain() {
    if (!m_device || !m_window) {
        return Error("Device or window not initialized");
    }

    ID3D12Device* device = m_device->GetDevice();
    uint32_t rtvDescriptorSize = m_heapManager.GetRtvDescriptorSize();

    // Free old RTVs
    for (auto& handle : m_rtvHandles) {
        if (handle.IsValid()) {
            m_heapManager.FreeRtv(handle);
        }
    }

    // Create new RTVs for each back buffer
    for (uint32_t i = 0; i < VENPOD::Window::BUFFER_COUNT; i++) {
        ID3D12Resource* backBuffer = m_window->GetBackBuffer(i);
        if (!backBuffer) {
            return Error("Back buffer {} is null", i);
        }

        m_rtvHandles[i] = m_heapManager.AllocateRtv();
        if (!m_rtvHandles[i].IsValid()) {
            return Error("Failed to allocate RTV for back buffer {}", i);
        }

        device->CreateRenderTargetView(backBuffer, nullptr, m_rtvHandles[i].cpu);
    }

    spdlog::debug("Created {} RTVs for swapchain", VENPOD::Window::BUFFER_COUNT);
    return {};
}

} // namespace VENPOD::Graphics
