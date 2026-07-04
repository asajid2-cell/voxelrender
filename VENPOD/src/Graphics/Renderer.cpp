#include "Renderer.h"
#include "Graphics/BackbufferCapture.h"
#include "RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace VENPOD::Graphics {

namespace {

// Must match SharedTypes.hlsli FrameConstants exactly. Keeping the CPU mirror
// in one place avoids the renderer and sparse-surface paths drifting as the
// sparse backend adds ownership metadata.
struct FrameConstantsCpu {
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
    float regionOrigin[4];        // xyz = world origin, w = unused
    float brushPosition[4];       // xyz = position, w = radius
    float brushParams[4];         // x = material, y = shape, z = hasValidPosition, w = unused
    float characterPosition[4];   // xyz = feet position, w = visible flag
    float farFieldParams[4];      // x = enabled, y = page count, z = node count, w = page size
    float renderBudgetParams[4];  // x = dense max dist, y = dense max steps, z = far quality, w = quality
    float farFieldGridParams[4];  // x = page radius, y = index side, z = root min Y, w = ownership stats flag
    float sparseNearParams[4];    // x = enabled, y = pages, z = table capacity, w = flags
    float midFieldParams[4];      // x = enabled, y = start dist, z = end dist, w = min cell size
    float surfaceParams[4];       // x = enabled, y = faces, z = ranges, w = range table capacity
    float nearOwnershipParams[4]; // xyz = sparse near owner center, w = owner radius
    float backgroundOwnershipParams[4]; // x = mid start, y = far handoff, z = mid end, w = valid
    float midResidencyParams[4];  // x/y = height/voxel coverage, z/w = resident height/voxel counts
    float farOwnershipParams[4];  // x = ready, y = upload coverage, z = page coverage, w = effective quality
    float exactNearParams[4];     // x = exact sparse voxel distance, y = world seed bits, z/w = mid voxel handoff coverage/worst ring
    float surfaceRasterParams[4]; // x = public exact sparse surface draw/resolve distance, y = mid safe distance, z = far SVO step quality gate
};

static_assert(sizeof(FrameConstantsCpu) == 368);
static_assert(std::is_standard_layout_v<FrameConstantsCpu>);
static_assert(offsetof(FrameConstantsCpu, cameraPosition) == 0u);
static_assert(offsetof(FrameConstantsCpu, cameraForward) == 16u);
static_assert(offsetof(FrameConstantsCpu, cameraRight) == 32u);
static_assert(offsetof(FrameConstantsCpu, cameraUp) == 48u);
static_assert(offsetof(FrameConstantsCpu, sunDirection) == 64u);
static_assert(offsetof(FrameConstantsCpu, gridSizeX) == 80u);
static_assert(offsetof(FrameConstantsCpu, gridSizeY) == 84u);
static_assert(offsetof(FrameConstantsCpu, gridSizeZ) == 88u);
static_assert(offsetof(FrameConstantsCpu, voxelScale) == 92u);
static_assert(offsetof(FrameConstantsCpu, viewportWidth) == 96u);
static_assert(offsetof(FrameConstantsCpu, viewportHeight) == 100u);
static_assert(offsetof(FrameConstantsCpu, frameIndex) == 104u);
static_assert(offsetof(FrameConstantsCpu, debugMode) == 108u);
static_assert(offsetof(FrameConstantsCpu, regionOrigin) == 112u);
static_assert(offsetof(FrameConstantsCpu, brushPosition) == 128u);
static_assert(offsetof(FrameConstantsCpu, brushParams) == 144u);
static_assert(offsetof(FrameConstantsCpu, characterPosition) == 160u);
static_assert(offsetof(FrameConstantsCpu, farFieldParams) == 176u);
static_assert(offsetof(FrameConstantsCpu, renderBudgetParams) == 192u);
static_assert(offsetof(FrameConstantsCpu, farFieldGridParams) == 208u);
static_assert(offsetof(FrameConstantsCpu, sparseNearParams) == 224u);
static_assert(offsetof(FrameConstantsCpu, midFieldParams) == 240u);
static_assert(offsetof(FrameConstantsCpu, surfaceParams) == 256u);
static_assert(offsetof(FrameConstantsCpu, nearOwnershipParams) == 272u);
static_assert(offsetof(FrameConstantsCpu, backgroundOwnershipParams) == 288u);
static_assert(offsetof(FrameConstantsCpu, midResidencyParams) == 304u);
static_assert(offsetof(FrameConstantsCpu, farOwnershipParams) == 320u);
static_assert(offsetof(FrameConstantsCpu, exactNearParams) == 336u);
static_assert(offsetof(FrameConstantsCpu, surfaceRasterParams) == 352u);

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

float ClampFinite(float value, float minValue, float maxValue, float fallback) {
    return std::clamp(FiniteOr(value, fallback), minValue, maxValue);
}

float NonNegativeFiniteOr(float value, float fallback) {
    return std::max(0.0f, FiniteOr(value, fallback));
}

float FloatBitsFromUint32(uint32_t value) {
    float bits = 0.0f;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float VectorLength(float x, float y, float z) {
    return std::sqrt((x * x) + (y * y) + (z * z));
}

const Renderer::BrushPreview* SelectPublicBrushPreview(
    const Renderer::BrushPreview* brushPreview,
    const Renderer::CameraParams& camera)
{
    if (!brushPreview || !brushPreview->hasValidPosition) {
        return nullptr;
    }

    constexpr float kMinBrushPreviewDistance = 16.0f;
    constexpr float kBrushPreviewDistanceScale = 6.0f;
    constexpr float kMaxBrushPreviewAngularRadius = 0.16f;
    constexpr float kMaxBrushPreviewScreenHalfHeight = 0.22f;

    const float radius = brushPreview->radius;
    if (!std::isfinite(radius) || radius <= 0.0f) {
        return nullptr;
    }

    const float toBrushX = brushPreview->posX - camera.posX;
    const float toBrushY = brushPreview->posY - camera.posY;
    const float toBrushZ = brushPreview->posZ - camera.posZ;
    const float distToCenter = VectorLength(toBrushX, toBrushY, toBrushZ);
    if (!std::isfinite(distToCenter) || distToCenter <= 0.001f) {
        return nullptr;
    }

    const float minDistance = std::max(radius * kBrushPreviewDistanceScale, kMinBrushPreviewDistance);
    if (distToCenter < minDistance) {
        return nullptr;
    }

    const float forwardLength = VectorLength(camera.forwardX, camera.forwardY, camera.forwardZ);
    if (!std::isfinite(forwardLength) || forwardLength <= 0.001f) {
        return nullptr;
    }

    const float invDist = 1.0f / distToCenter;
    const float invForwardLength = 1.0f / forwardLength;
    const float forwardDot =
        ((toBrushX * invDist) * (camera.forwardX * invForwardLength)) +
        ((toBrushY * invDist) * (camera.forwardY * invForwardLength)) +
        ((toBrushZ * invDist) * (camera.forwardZ * invForwardLength));
    if (!std::isfinite(forwardDot) || forwardDot <= 0.05f) {
        return nullptr;
    }

    const float radiusOverDistance = std::clamp(radius / distToCenter, 0.0f, 1.0f);
    const float angularRadius = std::asin(radiusOverDistance);
    if (!std::isfinite(angularRadius) || angularRadius > kMaxBrushPreviewAngularRadius) {
        return nullptr;
    }

    const float tanHalfFov = std::tan(std::clamp(camera.fov, 0.1f, 3.0f) * 0.5f);
    const float screenHalfHeight = std::tan(angularRadius) / std::max(tanHalfFov, 0.001f);
    if (!std::isfinite(screenHalfHeight) || screenHalfHeight > kMaxBrushPreviewScreenHalfHeight) {
        return nullptr;
    }

    return brushPreview;
}

void LogSuppressedBrushPreview(
    const Renderer::BrushPreview* brushPreview,
    const Renderer::CameraParams& camera,
    const char* passName)
{
    if (!brushPreview || !brushPreview->hasValidPosition || (camera.frameIndex % 30u) != 0u) {
        return;
    }

    const float toBrushX = brushPreview->posX - camera.posX;
    const float toBrushY = brushPreview->posY - camera.posY;
    const float toBrushZ = brushPreview->posZ - camera.posZ;
    const float distToCenter = VectorLength(toBrushX, toBrushY, toBrushZ);
    const float radiusOverDistance =
        (std::isfinite(distToCenter) && distToCenter > 0.001f)
            ? std::clamp(brushPreview->radius / distToCenter, 0.0f, 1.0f)
            : 1.0f;
    const float angularRadius = std::asin(radiusOverDistance);
    const float tanHalfFov = std::tan(std::clamp(camera.fov, 0.1f, 3.0f) * 0.5f);
    const float screenHalfHeight = std::tan(angularRadius) / std::max(tanHalfFov, 0.001f);
    spdlog::warn(
        "BRUSH_PREVIEW_SUPPRESSED pass={} frame={} radius={:.2f} dist={:.2f} angularRad={:.3f} screenHalfHeight={:.3f}",
        passName,
        camera.frameIndex,
        brushPreview->radius,
        distToCenter,
        angularRadius,
        screenHalfHeight);
}

}

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
    m_imguiReservedSrv = m_heapManager.AllocateShaderVisibleCbvSrvUav();
    if (!m_imguiReservedSrv.IsValid()) {
        return Error("Failed to reserve shader-visible descriptor 0 for ImGui");
    }
    if (m_imguiReservedSrv.heapIndex != 0) {
        spdlog::warn(
            "Reserved shader-visible descriptor {} for ImGui; expected index 0",
            m_imguiReservedSrv.heapIndex);
    } else {
        spdlog::info("Reserved shader-visible descriptor 0 for ImGui");
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
    result = CreateDepthBuffer();
    if (!result) {
        return Error("Failed to create depth buffer: {}", result.error());
    }
    if (UseBackgroundPassSplit()) {
        result = CreateBackgroundPassResources();
        if (!result) {
            return Error("Failed to create background pass resources: {}", result.error());
        }
    }
    if (m_config.midPassEnabled) {
        result = CreateMidPassResources();
        if (!result) {
            return Error("Failed to create mid pass resources: {}", result.error());
        }
    }

    for (uint32_t i = 0; i < VENPOD::Window::BUFFER_COUNT; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "RaymarchFrameConstants_%u", i);
        result = m_frameConstantUploads[i].Initialize(
            device.GetDevice(),
            kFrameConstantUploadBytes,
            name);
        if (!result) {
            return Error("Failed to create raymarch frame constants upload buffer: {}", result.error());
        }
        std::snprintf(name, sizeof(name), "SparseSurfaceFrameConstants_%u", i);
        result = m_sparseSurfaceConstantUploads[i].Initialize(
            device.GetDevice(),
            kFrameConstantUploadBytes,
            name);
        if (!result) {
            return Error("Failed to create sparse surface frame constants upload buffer: {}", result.error());
        }
        std::snprintf(name, sizeof(name), "OverlayFrameConstants_%u", i);
        result = m_overlayConstantUploads[i].Initialize(
            device.GetDevice(),
            kFrameConstantUploadBytes,
            name);
        if (!result) {
            return Error("Failed to create overlay frame constants upload buffer: {}", result.error());
        }
        std::snprintf(name, sizeof(name), "DagFrameConstants_%u", i);
        result = m_dagConstantUploads[i].Initialize(
            device.GetDevice(),
            kFrameConstantUploadBytes,
            name);
        if (!result) {
            return Error("Failed to create DAG frame constants upload buffer: {}", result.error());
        }
    }
    result = m_dummyRenderOwnershipUAV.Initialize(
        device.GetDevice(),
        sizeof(uint32_t) * 21u,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "DummyRenderOwnershipUAV");
    if (!result) {
        return Error("Failed to create dummy render ownership UAV buffer: {}", result.error());
    }
    result = m_dummyRenderOwnershipUAV.CreateUAV(device.GetDevice(), m_heapManager);
    if (!result) {
        return Error("Failed to create dummy render ownership UAV: {}", result.error());
    }

    // Create fullscreen pipeline
    result = CreateFullscreenPipeline(device.GetDevice());
    if (!result) {
        return Error("Failed to create fullscreen pipeline: {}", result.error());
    }
    // P2 editable-SVDAG raymarch pipeline (separate pass; off the uber-shader).
    result = CreateDagRaymarchPipeline(device.GetDevice());
    if (!result) {
        return Error("Failed to create DAG raymarch pipeline: {}", result.error());
    }
    result = CreateSparseSurfacePipeline(device.GetDevice());
    if (!result) {
        return Error("Failed to create sparse surface pipeline: {}", result.error());
    }
    result = CreateOverlayPipeline(device.GetDevice());
    if (!result) {
        return Error("Failed to create overlay pipeline: {}", result.error());
    }
    if (UseBackgroundPassSplit()) {
        result = CreateBackgroundCompositePipeline(device.GetDevice());
        if (!result) {
            return Error("Failed to create background composite pipeline: {}", result.error());
        }
        result = CreateBackgroundTemporalPipeline(device.GetDevice());
        if (!result) {
            return Error("Failed to create background temporal pipeline: {}", result.error());
        }
    }
    if (m_config.midPassEnabled) {
        result = CreateMidCompositePipeline(device.GetDevice());
        if (!result) {
            return Error("Failed to create mid composite pipeline: {}", result.error());
        }
    }
    result = CreateSparseSurfaceDrawCommandSignature(device.GetDevice());
    if (!result) {
        return Error("Failed to create sparse surface draw command signature: {}", result.error());
    }
    spdlog::info("Renderer initialized ({}x{})", m_width, m_height);
    return {};
}

void Renderer::Shutdown() {
    for (auto& upload : m_frameConstantUploads) {
        upload.Shutdown();
    }
    for (auto& upload : m_sparseSurfaceConstantUploads) {
        upload.Shutdown();
    }
    for (auto& upload : m_overlayConstantUploads) {
        upload.Shutdown();
    }
    m_dummyRenderOwnershipUAV.Shutdown();
    DestroyBackgroundPassResources();
    DestroyMidPassResources();

    // Free RTV handles
    for (auto& handle : m_rtvHandles) {
        if (handle.IsValid()) {
            m_heapManager.FreeRtv(handle);
        }
    }
    if (m_dsvHandle.IsValid()) {
        m_heapManager.FreeDsv(m_dsvHandle);
    }
    m_depthBuffer.Reset();

    m_fullscreenPipeline.Shutdown();
    m_sparseSurfacePipeline.Shutdown();
    m_sparseSurfaceDepthPrepassPipeline.Shutdown();
    m_overlayPipeline.Shutdown();
    m_backgroundCompositePipeline.Shutdown();
    m_midCompositePipeline.Shutdown();
    m_sparseSurfaceDrawSignature.Reset();
    m_shaderCompiler.Shutdown();
    if (m_imguiReservedSrv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_imguiReservedSrv);
    }
    m_heapManager.Shutdown();

    m_device = nullptr;
    m_commandQueue = nullptr;
    m_window = nullptr;
}

bool Renderer::UseBackgroundPassSplit() const {
    return m_config.backgroundPassEnabled &&
        std::isfinite(m_config.backgroundPassScale) &&
        m_config.backgroundPassScale > 0.0f &&
        m_config.backgroundPassScale < 0.999f;
}

Renderer::BackgroundPassInfo Renderer::GetBackgroundPassInfo() const {
    BackgroundPassInfo info = {};
    info.active = UseBackgroundPassSplit() && m_backgroundPassColor.Get() != nullptr;
    info.fullWidth = m_width;
    info.fullHeight = m_height;
    info.backgroundWidth = info.active ? m_backgroundPassWidth : m_width;
    info.backgroundHeight = info.active ? m_backgroundPassHeight : m_height;
    info.scale = m_config.backgroundPassScale;
    info.surfaceRaymarchFill = m_backgroundPassSurfaceRaymarchFillLastFrame;
    info.clearProbe = m_config.backgroundPassClearProbe;
    info.forceColor = m_config.backgroundPassForceColor;
    info.compositeDebug = m_config.backgroundPassCompositeDebug;
    info.compositeForceColor = m_config.backgroundPassCompositeForceColor;
    info.foregroundMask = m_config.backgroundPassForegroundMask;
    return info;
}

bool Renderer::QueueBackgroundPassCapture(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t frameNumber,
    const std::filesystem::path& outputPath,
    PendingBackbufferCapture& outCapture)
{
    if (!cmdList || !m_device || !UseBackgroundPassSplit() || !m_backgroundPassColor.Get()) {
        return false;
    }
    return QueueTextureCapture(
        m_device->GetDevice(),
        cmdList,
        m_backgroundPassColor.Get(),
        m_backgroundPassColorState,
        m_backgroundPassColorState,
        frameNumber,
        outputPath,
        "background_pass_frame",
        outCapture);
}

void Renderer::SetViewportAndScissor(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t width,
    uint32_t height)
{
    if (!cmdList) {
        return;
    }
    const uint32_t safeWidth = std::max(1u, width);
    const uint32_t safeHeight = std::max(1u, height);
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(safeWidth);
    viewport.Height = static_cast<float>(safeHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(safeWidth);
    scissor.bottom = static_cast<LONG>(safeHeight);
    cmdList->RSSetScissorRects(1, &scissor);
}

void Renderer::SetMainRenderTarget(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList || !m_window) {
        return;
    }
    const uint32_t frameIndex = m_currentFrameIndex % VENPOD::Window::BUFFER_COUNT;
    if (!m_rtvHandles[frameIndex].IsValid()) {
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHandles[frameIndex].cpu;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHandle.cpu;
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, m_dsvHandle.IsValid() ? &dsvHandle : nullptr);
    SetViewportAndScissor(cmdList, m_width, m_height);
}

void Renderer::BeginFrame(ID3D12GraphicsCommandList* cmdList, uint32_t frameIndex) {
    if (!cmdList || !m_window) return;
    m_currentFrameIndex = frameIndex % VENPOD::Window::BUFFER_COUNT;

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
    SetMainRenderTarget(cmdList);

    // Clear render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHandles[frameIndex].cpu;
    const float clearColor[] = { 0.42f, 0.55f, 0.74f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    if (m_dsvHandle.IsValid()) {
        cmdList->ClearDepthStencilView(
            m_dsvHandle.cpu,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
    }
    if (m_config.backgroundPassForegroundMask &&
        UseBackgroundPassSplit() &&
        m_backgroundPassDsv.IsValid()) {
        cmdList->ClearDepthStencilView(
            m_backgroundPassDsv.cpu,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
    }

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
    const DescriptorHandle& chunkValidMaskSRV,
    const DescriptorHandle& materialPaletteSRV,
    uint32_t gridSizeX,
    uint32_t gridSizeY,
    uint32_t gridSizeZ,
    const CameraParams& camera,
    float regionOriginX,
    float regionOriginY,
    float regionOriginZ,
    const BrushPreview* brushPreview,
    const CharacterPreview* characterPreview,
    const SparseFarField* sparseFarField,
    const SparseNearField* sparseNearField)
{
    if (!cmdList) return;

    // Set descriptor heaps (required before using shader-visible descriptors)
    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    const bool useBackgroundPassSplit =
        UseBackgroundPassSplit() &&
        m_backgroundPassColor.Get() &&
        m_backgroundPassDepth.Get() &&
        m_backgroundPassRtv.IsValid() &&
        m_backgroundPassDsv.IsValid() &&
        m_backgroundPassSrv.IsValid() &&
        m_backgroundCompositePipeline.GetPSO() != nullptr &&
        m_backgroundCompositePipeline.GetRootSignature() != nullptr &&
        m_backgroundPassWidth > 0 &&
        m_backgroundPassHeight > 0;
    const bool forceBackgroundPassColor =
        useBackgroundPassSplit && m_config.backgroundPassForceColor;
    const bool backgroundPassSurfaceRaymarchFillThisFrame =
        useBackgroundPassSplit &&
        (m_config.backgroundPassSurfaceRaymarchFill || camera.backgroundPassSurfaceRaymarchFill);
    const bool backgroundPassForegroundMaskThisFrame =
        useBackgroundPassSplit &&
        m_config.backgroundPassForegroundMask &&
        m_sparseSurfaceDepthPrepassPipeline.GetPSO() != nullptr;
    m_backgroundPassSurfaceRaymarchFillLastFrame = backgroundPassSurfaceRaymarchFillThisFrame;
    if (useBackgroundPassSplit) {
        if (m_backgroundPassColorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassColor.Get(),
                m_backgroundPassColorState,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList->ResourceBarrier(1, &barrier);
            m_backgroundPassColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE backgroundRtv = m_backgroundPassRtv.cpu;
        D3D12_CPU_DESCRIPTOR_HANDLE backgroundDsv = m_backgroundPassDsv.cpu;
        cmdList->OMSetRenderTargets(1, &backgroundRtv, FALSE, &backgroundDsv);
        SetViewportAndScissor(cmdList, m_backgroundPassWidth, m_backgroundPassHeight);
        const float defaultClearColor[] = { 0.42f, 0.55f, 0.74f, 1.0f };
        const float probeClearColor[] = { 1.0f, 0.0f, 1.0f, 1.0f };
        const float forceClearColor[] = { 0.0f, 0.95f, 0.28f, 1.0f };
        const float* clearColor = forceBackgroundPassColor
            ? forceClearColor
            : (m_config.backgroundPassClearProbe ? probeClearColor : defaultClearColor);
        cmdList->ClearRenderTargetView(backgroundRtv, clearColor, 0, nullptr);
        if (backgroundPassForegroundMaskThisFrame) {
            cmdList->ClearDepthStencilView(
                backgroundDsv,
                D3D12_CLEAR_FLAG_DEPTH,
                1.0f,
                0,
                0,
                nullptr);
        } else {
            cmdList->ClearDepthStencilView(
                backgroundDsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                1.0f,
                0,
                0,
                nullptr);
        }
    }

    // Bind fullscreen pipeline
    m_fullscreenPipeline.Bind(cmdList);
    cmdList->OMSetStencilRef(0);

    FrameConstantsCpu constants = {};

    const float cameraPosX = FiniteOr(camera.posX, 0.0f);
    const float cameraPosY = FiniteOr(camera.posY, 0.0f);
    const float cameraPosZ = FiniteOr(camera.posZ, 0.0f);

    // Fill in camera data
    constants.cameraPosition[0] = cameraPosX;
    constants.cameraPosition[1] = cameraPosY;
    constants.cameraPosition[2] = cameraPosZ;
    constants.cameraPosition[3] = ClampFinite(camera.fov, 1.0f, 175.0f, 75.0f);

    constants.cameraForward[0] = FiniteOr(camera.forwardX, 0.0f);
    constants.cameraForward[1] = FiniteOr(camera.forwardY, 0.0f);
    constants.cameraForward[2] = FiniteOr(camera.forwardZ, 1.0f);
    constants.cameraForward[3] = std::max(0.001f, FiniteOr(camera.aspectRatio, 1.0f));

    constants.cameraRight[0] = FiniteOr(camera.rightX, 1.0f);
    constants.cameraRight[1] = FiniteOr(camera.rightY, 0.0f);
    constants.cameraRight[2] = FiniteOr(camera.rightZ, 0.0f);
    constants.cameraRight[3] = 0.0f;

    constants.cameraUp[0] = FiniteOr(camera.upX, 0.0f);
    constants.cameraUp[1] = FiniteOr(camera.upY, 1.0f);
    constants.cameraUp[2] = FiniteOr(camera.upZ, 0.0f);
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
    constants.viewportWidth = static_cast<float>(useBackgroundPassSplit ? m_backgroundPassWidth : m_width);
    constants.viewportHeight = static_cast<float>(useBackgroundPassSplit ? m_backgroundPassHeight : m_height);
    constants.frameIndex = camera.frameIndex;
    constants.debugMode = camera.debugMode;

    // CRITICAL FIX: Fill in region origin for infinite world
    // Shader MUST subtract this from world coords to sample correct buffer location
    constants.regionOrigin[0] = FiniteOr(regionOriginX, 0.0f);
    constants.regionOrigin[1] = FiniteOr(regionOriginY, 0.0f);
    constants.regionOrigin[2] = FiniteOr(regionOriginZ, 0.0f);
    constants.regionOrigin[3] = 0.0f;  // unused

    // Fill in brush preview data only when its projection is small enough to
    // be an edit affordance rather than a scene-covering dome.
    const BrushPreview* visibleBrushPreview = SelectPublicBrushPreview(brushPreview, camera);
    if (!visibleBrushPreview) {
        LogSuppressedBrushPreview(brushPreview, camera, "raymarch");
    }
    if (visibleBrushPreview) {
        constants.brushPosition[0] = visibleBrushPreview->posX;
        constants.brushPosition[1] = visibleBrushPreview->posY;
        constants.brushPosition[2] = visibleBrushPreview->posZ;
        constants.brushPosition[3] = visibleBrushPreview->radius;
        constants.brushParams[0] = static_cast<float>(visibleBrushPreview->material);
        constants.brushParams[1] = static_cast<float>(visibleBrushPreview->shape);
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

    if (characterPreview && characterPreview->visible) {
        constants.characterPosition[0] = characterPreview->feetX;
        constants.characterPosition[1] = characterPreview->feetY;
        constants.characterPosition[2] = characterPreview->feetZ;
        constants.characterPosition[3] = 1.0f;
    } else {
        constants.characterPosition[0] = 0.0f;
        constants.characterPosition[1] = 0.0f;
        constants.characterPosition[2] = 0.0f;
        constants.characterPosition[3] = 0.0f;
    }

    const bool farFieldEnabled =
        sparseFarField &&
        sparseFarField->enabled &&
        sparseFarField->nodeSRV.IsValid() &&
        sparseFarField->pageSRV.IsValid() &&
        sparseFarField->pageIndexSRV.IsValid() &&
        sparseFarField->nodeCount > 0 &&
        sparseFarField->pageCount > 0 &&
        sparseFarField->pageIndexCount > 0 &&
        sparseFarField->pageRadius > 0;
    constants.farFieldParams[0] = farFieldEnabled ? 1.0f : 0.0f;
    constants.farFieldParams[1] = farFieldEnabled ? static_cast<float>(sparseFarField->pageCount) : 0.0f;
    constants.farFieldParams[2] = farFieldEnabled ? static_cast<float>(sparseFarField->nodeCount) : 0.0f;
    constants.farFieldParams[3] = farFieldEnabled ? NonNegativeFiniteOr(sparseFarField->pageSize, 0.0f) : 0.0f;

    const float raymarchMaxDistance = FiniteOr(camera.raymarchMaxDistance, 2500.0f);
    constants.renderBudgetParams[0] = raymarchMaxDistance > 0.0f ? raymarchMaxDistance : 2500.0f;
    constants.renderBudgetParams[1] = static_cast<float>(camera.raymarchMaxSteps > 0 ? camera.raymarchMaxSteps : 2048);
    // Far height/clipmap fallback is independent from the experimental far-SVO
    // buffers. Do not zero this when far-SVO is disabled, or the renderer loses
    // all non-SVO horizon continuity and reports most pixels as misses.
    constants.renderBudgetParams[2] = ClampFinite(camera.farFieldQuality, 0.0f, 1.0f, 1.0f);
    constants.renderBudgetParams[3] = ClampFinite(camera.renderQuality, 0.0f, 1.0f, 1.0f);
    constants.farFieldGridParams[0] = farFieldEnabled ? static_cast<float>(sparseFarField->pageRadius) : 0.0f;
    constants.farFieldGridParams[1] = farFieldEnabled ? static_cast<float>(sparseFarField->pageRadius * 2 + 1) : 0.0f;
    constants.farFieldGridParams[2] = farFieldEnabled ? FiniteOr(sparseFarField->rootMinY, 0.0f) : 0.0f;
    const bool renderOwnershipEnabled =
        camera.renderOwnershipStatsEnabled &&
        sparseNearField &&
        sparseNearField->renderOwnershipUAV.IsValid();
    constants.farFieldGridParams[3] = renderOwnershipEnabled ? 1.0f : 0.0f;
    const float farUploadCoverage = farFieldEnabled
        ? ClampFinite(sparseFarField->uploadCoverageRatio, 0.0f, 1.0f, 0.0f)
        : 0.0f;
    const float farPageCoverage = farFieldEnabled
        ? ClampFinite(sparseFarField->pageCoverageRatio, 0.0f, 1.0f, 0.0f)
        : 0.0f;
    const bool farReady =
        farFieldEnabled &&
        sparseFarField->ready &&
        farUploadCoverage >= 0.999f &&
        farPageCoverage > 0.0f;
    constants.farOwnershipParams[0] = farReady ? 1.0f : 0.0f;
    constants.farOwnershipParams[1] = farUploadCoverage;
    constants.farOwnershipParams[2] = farPageCoverage;
    constants.farOwnershipParams[3] = ClampFinite(camera.farFieldQuality, 0.0f, 1.0f, 1.0f);
    constants.exactNearParams[0] = NonNegativeFiniteOr(camera.exactNearDistance, 0.0f);
    constants.exactNearParams[1] = FloatBitsFromUint32(camera.worldSeed);
    constants.exactNearParams[2] = ClampFinite(camera.midFieldVoxelInterestCoverage, 0.0f, 1.0f, 0.0f);
    constants.exactNearParams[3] = ClampFinite(camera.midFieldVoxelWorstRingCoverage, 0.0f, 1.0f, 0.0f);
    constants.surfaceRasterParams[0] = NonNegativeFiniteOr(camera.surfaceRasterMaxDistance, 0.0f);
    // y = L3 motion guard: streamed-mid safe distance (0 = off). See CameraParams.
    constants.surfaceRasterParams[1] = NonNegativeFiniteOr(camera.midStreamSafeDistance, 0.0f);
    constants.surfaceRasterParams[2] = FiniteOr(camera.farSvoStepQualityGate, 0.92f);
    constants.surfaceRasterParams[3] = 0.0f;

    const bool sparseNearEnabled =
        sparseNearField &&
        sparseNearField->enabled &&
        sparseNearField->brickPoolSRV.IsValid() &&
        sparseNearField->pageTableSRV.IsValid() &&
        sparseNearField->occupancySRV.IsValid() &&
        sparseNearField->pageGenerationSRV.IsValid() &&
        sparseNearField->maxBrickPages > 0 &&
        sparseNearField->pageTableCapacity > 0;
    constants.sparseNearParams[0] = sparseNearEnabled ? 1.0f : 0.0f;
    constants.sparseNearParams[1] = sparseNearEnabled ? static_cast<float>(sparseNearField->maxBrickPages) : 0.0f;
    constants.sparseNearParams[2] = sparseNearEnabled ? static_cast<float>(sparseNearField->pageTableCapacity) : 0.0f;
    uint32_t sparseNearFlags = 0u;
    if (sparseNearEnabled && sparseNearField->sparseOnly) {
        sparseNearFlags |= 1u;
    }
    if (sparseNearEnabled && sparseNearField->surfaceAuthoritative) {
        sparseNearFlags |= 2u;
    }
    if (sparseNearEnabled &&
        sparseNearField->midClipmapEnabled &&
        sparseNearField->midVoxelClipmapBrickCount > 0) {
        sparseNearFlags |= 4u;
    }
    if (sparseNearEnabled && sparseNearField->surfaceRaymarchFill) {
        sparseNearFlags |= 8u;
    }
    if (sparseNearField && sparseNearField->voxelTerrainOnly) {
        sparseNearFlags |= 16u;
    }
    if (sparseNearEnabled && sparseNearField->walkingMidVoxelDda) {
        sparseNearFlags |= 32u;
    }
    if (useBackgroundPassSplit && !backgroundPassSurfaceRaymarchFillThisFrame) {
        sparseNearFlags &= ~8u;
    }
    constants.sparseNearParams[3] = static_cast<float>(sparseNearFlags);
    const float safeMidStartDistance = NonNegativeFiniteOr(camera.midFieldStartDistance, 480.0f);
    const float safeMidEndDistance =
        std::max(safeMidStartDistance + 1.0f, FiniteOr(camera.midFieldEndDistance, safeMidStartDistance + 1.0f));
    const float safeMidCellSize = std::max(4.0f, FiniteOr(camera.midFieldCellSize, 16.0f));
    const bool midClipmapEnabled =
        sparseNearEnabled &&
        sparseNearField->midClipmapEnabled &&
        sparseNearField->midClipmapMetadataSRV.IsValid() &&
        sparseNearField->midClipmapLookupSRV.IsValid() &&
        sparseNearField->midClipmapSamplesSRV.IsValid() &&
        sparseNearField->midVoxelClipmapMetadataSRV.IsValid() &&
        sparseNearField->midVoxelClipmapLookupSRV.IsValid() &&
        sparseNearField->midVoxelClipmapSamplesSRV.IsValid() &&
        sparseNearField->midClipmapTileCount > 0 &&
        sparseNearField->midClipmapTileSampleSide > 0 &&
        safeMidEndDistance > safeMidStartDistance;
    const bool surfaceEnabled =
        sparseNearEnabled &&
        sparseNearField->surfaceEnabled &&
        sparseNearField->surfaceFacesSRV.IsValid() &&
        sparseNearField->surfaceRangesSRV.IsValid() &&
        sparseNearField->surfaceRangeCount > 0;
    constants.midFieldParams[0] = midClipmapEnabled ? 1.0f : 0.0f;
    constants.midFieldParams[1] = midClipmapEnabled ? safeMidStartDistance : 0.0f;
    constants.midFieldParams[2] = midClipmapEnabled ? safeMidEndDistance : 0.0f;
    constants.midFieldParams[3] = midClipmapEnabled ? safeMidCellSize : 0.0f;
    constants.surfaceParams[0] = surfaceEnabled ? 1.0f : 0.0f;
    constants.surfaceParams[1] = surfaceEnabled ? static_cast<float>(sparseNearField->surfaceFaceCount) : 0.0f;
    constants.surfaceParams[2] = surfaceEnabled ? static_cast<float>(sparseNearField->surfaceRangeCount) : 0.0f;
    constants.surfaceParams[3] = surfaceEnabled ? static_cast<float>(sparseNearField->surfaceRangeTableCapacity) : 0.0f;
    constants.nearOwnershipParams[0] =
        sparseNearEnabled ? FiniteOr(sparseNearField->ownershipCenterX, cameraPosX) : cameraPosX;
    constants.nearOwnershipParams[1] =
        sparseNearEnabled ? FiniteOr(sparseNearField->ownershipCenterY, cameraPosY) : cameraPosY;
    constants.nearOwnershipParams[2] =
        sparseNearEnabled ? FiniteOr(sparseNearField->ownershipCenterZ, cameraPosZ) : cameraPosZ;
    constants.nearOwnershipParams[3] =
        sparseNearEnabled ? NonNegativeFiniteOr(sparseNearField->ownershipRadius, 0.0f) : 0.0f;
    const float fallbackFarHandoffDistance =
        safeMidStartDistance + (safeMidEndDistance - safeMidStartDistance) * 0.62f;
    const float requestedFarHandoffDistance = FiniteOr(camera.midFieldFarHandoffDistance, fallbackFarHandoffDistance);
    const float farHandoffDistance = requestedFarHandoffDistance > 0.0f
        ? std::clamp(requestedFarHandoffDistance, safeMidStartDistance, safeMidEndDistance)
        : fallbackFarHandoffDistance;
    constants.backgroundOwnershipParams[0] = midClipmapEnabled ? safeMidStartDistance : 0.0f;
    constants.backgroundOwnershipParams[1] = midClipmapEnabled ? farHandoffDistance : 0.0f;
    constants.backgroundOwnershipParams[2] = midClipmapEnabled ? safeMidEndDistance : 0.0f;
    constants.backgroundOwnershipParams[3] = midClipmapEnabled ? 1.0f : 0.0f;
    constants.midResidencyParams[0] =
        midClipmapEnabled ? ClampFinite(camera.midFieldHeightCoverage, 0.0f, 1.0f, 0.0f) : 0.0f;
    // World-consistency latch: shader gates (FarSpawnLandBand, the far-SVO
    // horizon deferral) key on voxel coverage >= 0.5 only to hide the
    // pre-residency startup flash. A transient mid-flight coverage dip (fast
    // high-altitude flight) must NOT flip those gates off - doing so re-renders
    // the entire far field as the unreshaped flooded basin (navy discs / blue
    // lakes) until streaming catches up. Once coverage has been good this
    // session, hold the published signal at the gate threshold; the live
    // 0.04-usability checks keep their behavior via the unlatched lower range
    // and the still-live resident counts in z/w.
    {
        const float liveVoxelCoverage =
            midClipmapEnabled ? ClampFinite(camera.midFieldVoxelCoverage, 0.0f, 1.0f, 0.0f) : 0.0f;
        // TANDEM fix (altitude flooded-world bug): the spawn-land reshape gate
        // (FarSpawnLandBand) keys on this latched signal. The latch used to arm
        // ONLY on voxel coverage >= 0.5, but when the camera starts/flies at
        // altitude the mid-voxel bricks stay sparse and voxel coverage never
        // reaches 0.5 -> the latch never arms -> the reshape stays OFF -> the far
        // water/deterministic-water layers render the UN-reshaped flooded basin
        // (ground-truth: a ground walk is a clean continent, a fly-from-altitude
        // is flooded). HEIGHT coverage is reliably ~1.0 at altitude and is the
        // correct "world is defined" signal for an analytic reshape, so arm on
        // either. The render is held until ~frame 120 so no startup flash.
        const float liveHeightCoverage =
            midClipmapEnabled ? ClampFinite(camera.midFieldHeightCoverage, 0.0f, 1.0f, 0.0f) : 0.0f;
        static bool s_midResidencyEverGood = false;
        if (liveVoxelCoverage >= 0.5f || liveHeightCoverage >= 0.5f) {
            s_midResidencyEverGood = true;
        }
        constants.midResidencyParams[1] = (s_midResidencyEverGood && midClipmapEnabled)
            ? std::max(liveVoxelCoverage, 0.51f)
            : liveVoxelCoverage;
    }
    constants.midResidencyParams[2] = midClipmapEnabled ? static_cast<float>(camera.midFieldResidentHeightTiles) : 0.0f;
    constants.midResidencyParams[3] = midClipmapEnabled ? static_cast<float>(camera.midFieldResidentVoxelBricks) : 0.0f;

    static_assert(sizeof(constants) <= kFrameConstantUploadBytes);
    UploadBuffer& frameConstantsUpload = m_frameConstantUploads[m_currentFrameIndex];
    if (void* mapped = frameConstantsUpload.GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }
    cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());

    // Use persistent shader-visible descriptors directly (no per-frame copy needed)
    cmdList->SetGraphicsRootDescriptorTable(1, voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(3, farFieldEnabled ? sparseFarField->nodeSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(4, farFieldEnabled ? sparseFarField->pageSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(5, farFieldEnabled ? sparseFarField->pageIndexSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(6, chunkValidMaskSRV.IsValid() ? chunkValidMaskSRV.gpu : voxelGridSRV.gpu);
    const uint32_t sparseBindingMask = sparseNearField ? sparseNearField->bindingMask : 0u;
    cmdList->SetGraphicsRootDescriptorTable(7, (sparseNearEnabled && (sparseBindingMask & (1u << 0))) ? sparseNearField->brickPoolSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(8, (sparseNearEnabled && (sparseBindingMask & (1u << 1))) ? sparseNearField->pageTableSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(9, (sparseNearEnabled && (sparseBindingMask & (1u << 2))) ? sparseNearField->occupancySRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(10, (sparseNearEnabled && (sparseBindingMask & (1u << 3))) ? sparseNearField->pageGenerationSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(11, (midClipmapEnabled && (sparseBindingMask & (1u << 4))) ? sparseNearField->midClipmapMetadataSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(12, (midClipmapEnabled && (sparseBindingMask & (1u << 5))) ? sparseNearField->midClipmapLookupSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(13, (midClipmapEnabled && (sparseBindingMask & (1u << 6))) ? sparseNearField->midClipmapSamplesSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(14, (midClipmapEnabled && (sparseBindingMask & (1u << 7))) ? sparseNearField->midVoxelClipmapMetadataSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(15, (midClipmapEnabled && (sparseBindingMask & (1u << 8))) ? sparseNearField->midVoxelClipmapLookupSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(16, (midClipmapEnabled && (sparseBindingMask & (1u << 9))) ? sparseNearField->midVoxelClipmapSamplesSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(17, (surfaceEnabled && (sparseBindingMask & (1u << 10))) ? sparseNearField->surfaceFacesSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(18, (surfaceEnabled && (sparseBindingMask & (1u << 11))) ? sparseNearField->surfaceRangesSRV.gpu : voxelGridSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        19,
        renderOwnershipEnabled
            ? sparseNearField->renderOwnershipUAV.gpu
            : m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);

    // Draw fullscreen triangle. The force-color probe intentionally leaves the
    // lower-resolution target at a known clear color to test RTV/SRV/composite
    // plumbing without touching PS_Raymarch.
    if (!forceBackgroundPassColor) {
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    if (useBackgroundPassSplit) {
        // TAA lane increment 2 (env VENPOD_BG_TEMPORAL, default off): while the
        // low-res background is still the bound render target, alpha-blend last
        // frame's accumulated background over it, reprojected through the
        // previous camera basis (rotation-exact; background treated as distant).
        // The history copy below then snapshots the blended result, closing the
        // accumulation loop. Skipped until one frame of history exists.
        if (m_backgroundTemporalEnabled &&
            m_backgroundPassHistory.Get() &&
            m_backgroundTemporalPipeline.GetPSO() != nullptr &&
            m_backgroundHistoryValid &&
            m_backgroundPrevCameraValid &&
            !forceBackgroundPassColor) {
            m_backgroundTemporalPipeline.Bind(cmdList);
            float temporalConstants[36];
            std::memcpy(temporalConstants + 0, constants.cameraPosition, 4 * sizeof(float));
            std::memcpy(temporalConstants + 4, constants.cameraForward, 4 * sizeof(float));
            std::memcpy(temporalConstants + 8, constants.cameraRight, 4 * sizeof(float));
            std::memcpy(temporalConstants + 12, constants.cameraUp, 4 * sizeof(float));
            std::memcpy(temporalConstants + 16, m_backgroundPrevCamera, 16 * sizeof(float));
            temporalConstants[32] = m_backgroundTemporalBlend;
            temporalConstants[33] = 0.0f;
            temporalConstants[34] = 0.0f;
            temporalConstants[35] = 0.0f;
            cmdList->SetGraphicsRootDescriptorTable(0, m_backgroundPassHistorySrv.gpu);
            cmdList->SetGraphicsRoot32BitConstants(1, 36, temporalConstants, 0);
            cmdList->DrawInstanced(3, 1, 0, 0);
        }

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_backgroundPassColor.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // Snapshot the (blended) background into the history buffer for next
        // frame's reprojection.
        if (m_backgroundTemporalEnabled && m_backgroundPassHistory.Get()) {
            D3D12_RESOURCE_BARRIER toCopy[2];
            toCopy[0] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassHistory.Get(),
                m_backgroundPassHistoryState,
                D3D12_RESOURCE_STATE_COPY_DEST);
            toCopy[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassColor.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmdList->ResourceBarrier(2, toCopy);
            cmdList->CopyResource(m_backgroundPassHistory.Get(), m_backgroundPassColor.Get());
            D3D12_RESOURCE_BARRIER toRead[2];
            toRead[0] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassHistory.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            toRead[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassColor.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(2, toRead);
            m_backgroundPassHistoryState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_backgroundHistoryValid = true;
        }

        // Cache this frame's camera basis (same packing the shader consumed)
        // for next frame's reprojection.
        if (m_backgroundTemporalEnabled) {
            std::memcpy(m_backgroundPrevCamera + 0, constants.cameraPosition, 4 * sizeof(float));
            std::memcpy(m_backgroundPrevCamera + 4, constants.cameraForward, 4 * sizeof(float));
            std::memcpy(m_backgroundPrevCamera + 8, constants.cameraRight, 4 * sizeof(float));
            std::memcpy(m_backgroundPrevCamera + 12, constants.cameraUp, 4 * sizeof(float));
            m_backgroundPrevCameraValid = true;
        }

        SetMainRenderTarget(cmdList);
        m_backgroundCompositePipeline.Bind(cmdList);
        ID3D12DescriptorHeap* compositeHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, compositeHeaps);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootDescriptorTable(0, m_backgroundPassSrv.gpu);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    // Mid-only raymarch overlay. The full raymarch pass has now drawn (and, in
    // the background-pass-split case, composited) to the MAIN render target.
    // This second pipeline re-shades the same fullscreen triangle with the
    // RAYMARCH_MID_ONLY variant (alpha=1 on a mid-terrain hit, alpha=0 on a
    // miss) and alpha-over blends it on top, so mid terrain gets the heavy
    // analytic-gradient shading while the full pass keeps near/far/sky.
    const bool useMidPassSplit =
        m_config.midPassEnabled &&
        m_midPassPipeline.GetPSO() != nullptr &&
        m_midCompositePipeline.GetPSO() != nullptr &&
        m_midCompositePipeline.GetRootSignature() != nullptr &&
        m_midPassColor.Get() != nullptr &&
        m_midPassRtv.IsValid() &&
        m_midPassSrv.IsValid() &&
        m_midPassWidth > 0 &&
        m_midPassHeight > 0;
    if (useMidPassSplit) {
        // (i) Render the mid-only raymarch into the LOW-RES color target. The
        // target encodes mid coverage in alpha (1 = mid hit, 0 = miss). It has
        // no depth/stencil (the mid PSO disables both); ownership against the
        // near surface is enforced at composite time via the main-RT stencil.
        if (m_midPassColorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                m_midPassColor.Get(),
                m_midPassColorState,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList->ResourceBarrier(1, &barrier);
            m_midPassColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE midRtv = m_midPassRtv.cpu;
        cmdList->OMSetRenderTargets(1, &midRtv, FALSE, nullptr);
        SetViewportAndScissor(cmdList, m_midPassWidth, m_midPassHeight);
        const float midClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(midRtv, midClearColor, 0, nullptr);

        m_midPassPipeline.Bind(cmdList);
        ID3D12DescriptorHeap* midHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, midHeaps);
        cmdList->OMSetStencilRef(0);

        // Re-set the exact same root bindings used by the fullscreen draw above.
        // The mid pipeline shares the fullscreen root signature layout.
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(1, voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(3, farFieldEnabled ? sparseFarField->nodeSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(4, farFieldEnabled ? sparseFarField->pageSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(5, farFieldEnabled ? sparseFarField->pageIndexSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(6, chunkValidMaskSRV.IsValid() ? chunkValidMaskSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(7, (sparseNearEnabled && (sparseBindingMask & (1u << 0))) ? sparseNearField->brickPoolSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(8, (sparseNearEnabled && (sparseBindingMask & (1u << 1))) ? sparseNearField->pageTableSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(9, (sparseNearEnabled && (sparseBindingMask & (1u << 2))) ? sparseNearField->occupancySRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(10, (sparseNearEnabled && (sparseBindingMask & (1u << 3))) ? sparseNearField->pageGenerationSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(11, (midClipmapEnabled && (sparseBindingMask & (1u << 4))) ? sparseNearField->midClipmapMetadataSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(12, (midClipmapEnabled && (sparseBindingMask & (1u << 5))) ? sparseNearField->midClipmapLookupSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(13, (midClipmapEnabled && (sparseBindingMask & (1u << 6))) ? sparseNearField->midClipmapSamplesSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(14, (midClipmapEnabled && (sparseBindingMask & (1u << 7))) ? sparseNearField->midVoxelClipmapMetadataSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(15, (midClipmapEnabled && (sparseBindingMask & (1u << 8))) ? sparseNearField->midVoxelClipmapLookupSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(16, (midClipmapEnabled && (sparseBindingMask & (1u << 9))) ? sparseNearField->midVoxelClipmapSamplesSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(17, (surfaceEnabled && (sparseBindingMask & (1u << 10))) ? sparseNearField->surfaceFacesSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(18, (surfaceEnabled && (sparseBindingMask & (1u << 11))) ? sparseNearField->surfaceRangesSRV.gpu : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            19,
            renderOwnershipEnabled
                ? sparseNearField->renderOwnershipUAV.gpu
                : m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);

        cmdList->DrawInstanced(3, 1, 0, 0);

        // (ii) Upscale-composite the low-res mid target over the MAIN render
        // target with bilinear filtering and alpha-over blending. The composite
        // PSO's stencil EQUAL ref 0 test restricts writes to pixels the near
        // surface did not own, mirroring the background composite.
        D3D12_RESOURCE_BARRIER midBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_midPassColor.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &midBarrier);
        m_midPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        SetMainRenderTarget(cmdList);
        m_midCompositePipeline.Bind(cmdList);
        ID3D12DescriptorHeap* midCompositeHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, midCompositeHeaps);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootDescriptorTable(0, m_midPassSrv.gpu);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
}

void Renderer::RenderSparseSurfaceFaces(
    ID3D12GraphicsCommandList* cmdList,
    const DescriptorHandle& surfaceFacesSRV,
    const DescriptorHandle& materialPaletteSRV,
    uint32_t surfaceFaceCount,
    const CameraParams& camera,
    ID3D12Resource* indirectDrawArgs,
    uint32_t indirectDrawCommandCount,
    ID3D12Resource* indirectDrawCount,
    const D3D12_VERTEX_BUFFER_VIEW* surfaceVertexIdView,
    const D3D12_INDEX_BUFFER_VIEW* surfaceIndexView,
    uint32_t surfaceVertexIdCapacityFaces,
    const DescriptorHandle* surfaceRecordsSRV,
    const DescriptorHandle* surfaceClustersSRV,
    const DescriptorHandle* renderOwnershipUAV,
    const DescriptorHandle* sparseBrickPoolSRV,
    const DescriptorHandle* sparsePageTableSRV,
    const DescriptorHandle* sparseOccupancySRV,
    const DescriptorHandle* sparsePageGenerationSRV,
    uint32_t sparseMaxBrickPages,
    uint32_t sparsePageTableCapacity)
{
    if (!cmdList || surfaceFaceCount == 0 || !surfaceFacesSRV.IsValid() || !materialPaletteSRV.IsValid()) {
        return;
    }
    if (!surfaceVertexIdView ||
        !surfaceIndexView ||
        surfaceVertexIdView->BufferLocation == 0u ||
        surfaceIndexView->BufferLocation == 0u ||
        surfaceVertexIdCapacityFaces == 0u) {
        return;
    }
    const uint32_t drawableFaceCount = std::min(surfaceFaceCount, surfaceVertexIdCapacityFaces);
    if (drawableFaceCount == 0u) {
        return;
    }
    const bool drawBackgroundForegroundMask =
        m_config.backgroundPassForegroundMask &&
        UseBackgroundPassSplit() &&
        m_backgroundPassDepth.Get() != nullptr &&
        m_backgroundPassDsv.IsValid() &&
        m_backgroundPassWidth > 0u &&
        m_backgroundPassHeight > 0u &&
        m_sparseSurfaceDepthPrepassPipeline.GetPSO() != nullptr;

    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    m_sparseSurfacePipeline.Bind(cmdList);
    cmdList->OMSetStencilRef(1);
    cmdList->IASetVertexBuffers(0, 1, surfaceVertexIdView);
    cmdList->IASetIndexBuffer(surfaceIndexView);

    FrameConstantsCpu constants = {};

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
    constants.cameraUp[0] = camera.upX;
    constants.cameraUp[1] = camera.upY;
    constants.cameraUp[2] = camera.upZ;
    constants.sunDirection[0] = 0.5f;
    constants.sunDirection[1] = 1.0f;
    constants.sunDirection[2] = 0.3f;
    constants.sunDirection[3] = 1.0f;
    constants.viewportWidth = static_cast<float>(m_width);
    constants.viewportHeight = static_cast<float>(m_height);
    constants.renderBudgetParams[0] = camera.raymarchMaxDistance;
    constants.renderBudgetParams[1] = static_cast<float>(camera.raymarchMaxSteps);
    constants.renderBudgetParams[2] = camera.farFieldQuality;
    constants.renderBudgetParams[3] = camera.renderQuality;
    constants.frameIndex = camera.frameIndex;
    constants.debugMode = camera.debugMode;
    constants.farFieldGridParams[3] =
        (camera.renderOwnershipStatsEnabled && renderOwnershipUAV && renderOwnershipUAV->IsValid())
            ? 1.0f
            : 0.0f;
    constants.surfaceParams[0] = 1.0f;
    constants.surfaceParams[1] = static_cast<float>(drawableFaceCount);
    constants.exactNearParams[0] = NonNegativeFiniteOr(camera.exactNearDistance, 0.0f);
    constants.nearOwnershipParams[3] = camera.surfaceRasterMaxDistance;
    constants.surfaceRasterParams[0] = NonNegativeFiniteOr(camera.surfaceRasterMaxDistance, 0.0f);
    constants.surfaceRasterParams[1] = 0.0f;
    constants.surfaceRasterParams[2] = FiniteOr(camera.farSvoStepQualityGate, 0.92f);
    const bool sparseMaterialLookupEnabled =
        sparseBrickPoolSRV && sparseBrickPoolSRV->IsValid() &&
        sparsePageTableSRV && sparsePageTableSRV->IsValid() &&
        sparseOccupancySRV && sparseOccupancySRV->IsValid() &&
        sparsePageGenerationSRV && sparsePageGenerationSRV->IsValid() &&
        sparseMaxBrickPages > 0u &&
        sparsePageTableCapacity > 0u;
    constants.sparseNearParams[0] = sparseMaterialLookupEnabled ? 1.0f : 0.0f;
    constants.sparseNearParams[1] = sparseMaterialLookupEnabled ? static_cast<float>(sparseMaxBrickPages) : 0.0f;
    constants.sparseNearParams[2] = sparseMaterialLookupEnabled ? static_cast<float>(sparsePageTableCapacity) : 0.0f;

    static_assert(sizeof(constants) <= kFrameConstantUploadBytes);
    UploadBuffer& frameConstantsUpload = m_sparseSurfaceConstantUploads[m_currentFrameIndex];
    if (void* mapped = frameConstantsUpload.GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }

    cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        3,
        (camera.renderOwnershipStatsEnabled && renderOwnershipUAV && renderOwnershipUAV->IsValid())
            ? renderOwnershipUAV->gpu
            : m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        4,
        (surfaceRecordsSRV && surfaceRecordsSRV->IsValid())
            ? surfaceRecordsSRV->gpu
            : surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        5,
        (surfaceClustersSRV && surfaceClustersSRV->IsValid())
            ? surfaceClustersSRV->gpu
            : surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        6,
        (sparseMaterialLookupEnabled && sparseBrickPoolSRV)
            ? sparseBrickPoolSRV->gpu
            : surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        7,
        (sparseMaterialLookupEnabled && sparsePageTableSRV)
            ? sparsePageTableSRV->gpu
            : surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        8,
        (sparseMaterialLookupEnabled && sparseOccupancySRV)
            ? sparseOccupancySRV->gpu
            : surfaceFacesSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(
        9,
        (sparseMaterialLookupEnabled && sparsePageGenerationSRV)
            ? sparsePageGenerationSRV->gpu
            : surfaceFacesSRV.gpu);

    auto drawSurfaceStreams = [&]() {
        if (indirectDrawArgs && indirectDrawCommandCount > 0 && m_sparseSurfaceDrawSignature) {
            cmdList->ExecuteIndirect(
                m_sparseSurfaceDrawSignature.Get(),
                indirectDrawCommandCount,
                indirectDrawArgs,
                0,
                indirectDrawCount,
                0);
        } else {
            cmdList->DrawIndexedInstanced(drawableFaceCount * 6u, 1u, 0u, 0, 0u);
        }
    };
    auto bindSurfaceDescriptors = [&]() {
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(1, surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            3,
            (camera.renderOwnershipStatsEnabled && renderOwnershipUAV && renderOwnershipUAV->IsValid())
                ? renderOwnershipUAV->gpu
                : m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            4,
            (surfaceRecordsSRV && surfaceRecordsSRV->IsValid())
                ? surfaceRecordsSRV->gpu
                : surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            5,
            (surfaceClustersSRV && surfaceClustersSRV->IsValid())
                ? surfaceClustersSRV->gpu
                : surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            6,
            (sparseMaterialLookupEnabled && sparseBrickPoolSRV)
                ? sparseBrickPoolSRV->gpu
                : surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            7,
            (sparseMaterialLookupEnabled && sparsePageTableSRV)
                ? sparsePageTableSRV->gpu
                : surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            8,
            (sparseMaterialLookupEnabled && sparseOccupancySRV)
                ? sparseOccupancySRV->gpu
                : surfaceFacesSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            9,
            (sparseMaterialLookupEnabled && sparsePageGenerationSRV)
                ? sparsePageGenerationSRV->gpu
                : surfaceFacesSRV.gpu);
    };

    if (m_config.sparseSurfaceDepthPrepass &&
        m_sparseSurfaceDepthPrepassPipeline.GetPSO() != nullptr &&
        m_dsvHandle.IsValid()) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHandle.cpu;
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
        m_sparseSurfaceDepthPrepassPipeline.Bind(cmdList);
        // Safe to draw the shaded pass after this because stencil writes are REPLACE->1.
        cmdList->OMSetStencilRef(1);
        bindSurfaceDescriptors();
        drawSurfaceStreams();

        SetMainRenderTarget(cmdList);
        m_sparseSurfacePipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(1);
        bindSurfaceDescriptors();
    }
    if (indirectDrawArgs && indirectDrawCommandCount > 0 && m_sparseSurfaceDrawSignature) {
        cmdList->ExecuteIndirect(
            m_sparseSurfaceDrawSignature.Get(),
            indirectDrawCommandCount,
            indirectDrawArgs,
            0,
            indirectDrawCount,
            0);
    } else {
        cmdList->DrawIndexedInstanced(drawableFaceCount * 6u, 1u, 0u, 0, 0u);
    }

    if (drawBackgroundForegroundMask) {
        D3D12_CPU_DESCRIPTOR_HANDLE backgroundDsv = m_backgroundPassDsv.cpu;
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &backgroundDsv);
        SetViewportAndScissor(cmdList, m_backgroundPassWidth, m_backgroundPassHeight);
        m_sparseSurfaceDepthPrepassPipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(1);
        bindSurfaceDescriptors();
        drawSurfaceStreams();
        SetMainRenderTarget(cmdList);
    }
}

void Renderer::RenderOverlays(
    ID3D12GraphicsCommandList* cmdList,
    const DescriptorHandle& materialPaletteSRV,
    const CameraParams& camera,
    const BrushPreview* brushPreview,
    const CharacterPreview* characterPreview)
{
    if (!cmdList || !materialPaletteSRV.IsValid()) {
        return;
    }
    const BrushPreview* visibleBrushPreview = SelectPublicBrushPreview(brushPreview, camera);
    if (!visibleBrushPreview) {
        LogSuppressedBrushPreview(brushPreview, camera, "overlay");
    }
    const bool hasBrush = visibleBrushPreview != nullptr;
    const bool hasCharacter = characterPreview && characterPreview->visible;
    if (!hasBrush && !hasCharacter) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    m_overlayPipeline.Bind(cmdList);

    FrameConstantsCpu constants = {};
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
    constants.viewportWidth = static_cast<float>(m_width);
    constants.viewportHeight = static_cast<float>(m_height);
    constants.frameIndex = camera.frameIndex;
    constants.debugMode = camera.debugMode;

    if (hasBrush) {
        constants.brushPosition[0] = visibleBrushPreview->posX;
        constants.brushPosition[1] = visibleBrushPreview->posY;
        constants.brushPosition[2] = visibleBrushPreview->posZ;
        constants.brushPosition[3] = visibleBrushPreview->radius;
        constants.brushParams[0] = static_cast<float>(visibleBrushPreview->material);
        constants.brushParams[1] = static_cast<float>(visibleBrushPreview->shape);
        constants.brushParams[2] = 1.0f;
    }
    if (hasCharacter) {
        constants.characterPosition[0] = characterPreview->feetX;
        constants.characterPosition[1] = characterPreview->feetY;
        constants.characterPosition[2] = characterPreview->feetZ;
        constants.characterPosition[3] = 1.0f;
    }

    static_assert(sizeof(constants) <= kFrameConstantUploadBytes);
    UploadBuffer& frameConstantsUpload = m_overlayConstantUploads[m_currentFrameIndex];
    if (void* mapped = frameConstantsUpload.GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }

    cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, materialPaletteSRV.gpu);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void Renderer::RenderCrosshair(ID3D12GraphicsCommandList* cmdList) {
    (void)cmdList;
    // Crosshair is drawn by ImGui in the app layer. This legacy function used
    // the voxel raymarch pipeline with partial constants in scissored strips,
    // which produced world-space scanline artifacts if called.
}

Result<void> Renderer::OnResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    auto result = CreateRTVsForSwapchain();
    if (!result) {
        return result;
    }
    result = CreateDepthBuffer();
    if (!result) {
        return result;
    }
    if (UseBackgroundPassSplit()) {
        result = CreateBackgroundPassResources();
        if (!result) {
            return result;
        }
    }
    if (m_config.midPassEnabled) {
        result = CreateMidPassResources();
        if (!result) {
            return result;
        }
    }
    return {};
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
    // b0: FrameConstants CBV. This used to be 56 DWORD root constants, which
    // left too little room for sparse near-field descriptor tables.
    RootParameter frameConstantsParam;
    frameConstantsParam.type = RootParamType::ConstantBuffer;
    frameConstantsParam.shaderRegister = 0;  // register b0
    frameConstantsParam.registerSpace = 0;   // space 0
    frameConstantsParam.visibility = D3D12_SHADER_VISIBILITY_ALL;
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

    // t2: Far voxel octree nodes
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        2,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t3: Far voxel octree pages
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        3,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t4: Far voxel octree page index grid
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        4,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t5: Dense render-window chunk-valid mask
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        5,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t6: Sparse near-field brick voxel pool
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        6,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t7: Sparse near-field page table
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        7,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t8: Sparse near-field occupancy metadata
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        8,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t9: Sparse near-field physical page generations
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        9,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t10: Sparse mid-field clipmap tile metadata
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        10,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t11: Sparse mid-field clipmap tile lookup hash table
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        11,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t12: Sparse mid-field clipmap height/material samples
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        12,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t13: Sparse mid-field coarse voxel brick metadata
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        13,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t14: Sparse mid-field coarse voxel brick lookup
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        14,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t15: Sparse mid-field coarse voxel brick samples
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        15,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t16: Sparse surface-extracted faces
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        16,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // t17: Sparse surface face ranges by brick
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        17,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // u0: Optional render ownership counters. This is sampled diagnostic
    // instrumentation, not part of the renderer's correctness path.
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
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

    // Sparse raster surfaces are the foreground owner and write depth/stencil
    // first. The fullscreen raymarch is now a background/color pass only: it
    // shades pixels whose stencil is still zero, but does not publish depth
    // ownership because it no longer writes SV_Depth.
    pipelineDesc.depthEnable = true;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipelineDesc.stencilEnable = true;
    pipelineDesc.stencilReadMask = 0xFFu;
    pipelineDesc.stencilWriteMask = 0x00u;
    pipelineDesc.frontStencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pipelineDesc.frontStencilPassOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // Sparse extracted faces are emitted with outward CCW winding in the
    // engine's world-space convention. Tell D3D about that contract so
    // fixed-function backface culling can reject hidden faces before raster.
    pipelineDesc.frontCounterClockwise = true;
    // The sparse-surface VS already rejects back-facing extracted faces using
    // the packed face normal and SV_ClipDistance. Keep fixed-function culling
    // disabled here: the generated quads are reconstructed in shader space,
    // and a winding/clip convention mismatch can drop valid exact faces before
    // they write the foreground stencil mask. When that happens, the later
    // fullscreen mid/far raymarch owns pixels even though exact surface draw
    // records exist and are closer.
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;

    auto result = m_fullscreenPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create fullscreen pipeline: {}", result.error());
    }

    spdlog::info("Fullscreen pipeline created successfully");

    // Optionally build a second "mid-only" raymarch overlay pipeline that reuses
    // this exact pipeline description (root signature, depth/stencil, RTV) but
    // swaps in the RAYMARCH_MID_ONLY shader variant and alpha-over blending. It
    // is composited over the full raymarch pass at draw time.
    if (m_config.midPassEnabled) {
        auto midResult = CreateMidPassPipeline(device, pipelineDesc);
        if (!midResult) {
            return Error("Failed to create mid pass pipeline: {}", midResult.error());
        }
    }

    return {};
}

Result<void> Renderer::CreateMidPassPipeline(ID3D12Device* device, GraphicsPipelineDesc fullscreenDesc) {
    // Compile the mid-only variant of PS_Raymarch (RAYMARCH_MID_ONLY=1). This is
    // the small analytic-gradient shading path that does not fit the full
    // uber-shader PSO, so it gets its own pipeline.
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_Raymarch.hlsl";

    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    psOptions.defines.push_back(L"RAYMARCH_MID_ONLY=1");
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile mid pass pixel shader: {}", psResult.error());
    }
    m_midPassPS = psResult.value();
    if (!m_midPassPS.IsValid()) {
        return Error("Mid pass pixel shader compilation failed: {}", m_midPassPS.errors);
    }

    // Reuse the fullscreen pipeline description verbatim (same root params,
    // sampler, RTV format, depth/stencil, cull) and only swap the pixel shader
    // and enable alpha-over blending so the mid terrain composites over the
    // already-drawn full raymarch pass.
    fullscreenDesc.vertexShader = m_fullscreenVS;
    fullscreenDesc.pixelShader = m_midPassPS;
    // The low-res mid pass renders into its own cleared (transparent) color
    // target, NOT over the main RT, so it does not alpha-blend here and does
    // not need a depth/stencil test. The alpha-over composite happens later in
    // m_midCompositePipeline (which performs the stencil==0 ownership test at
    // full resolution). Disabling depth/stencil here means the low-res pass
    // needs no matching-resolution DSV.
    fullscreenDesc.blendEnable = false;
    fullscreenDesc.depthEnable = false;
    fullscreenDesc.stencilEnable = false;
    fullscreenDesc.debugName = "MidPassPipeline";

    auto result = m_midPassPipeline.Initialize(device, fullscreenDesc);
    if (!result) {
        return Error("Failed to create mid pass pipeline: {}", result.error());
    }

    spdlog::info("Mid pass pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateDagRaymarchPipeline(ID3D12Device* device) {
    // Compile the separate DAG raymarch pixel shader (reuses the fullscreen VS).
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_DagRaymarch.hlsl";
    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile DAG raymarch pixel shader: {}", psResult.error());
    }
    m_dagRaymarchPS = psResult.value();
    if (!m_dagRaymarchPS.IsValid()) {
        return Error("DAG raymarch pixel shader compilation failed: {}", m_dagRaymarchPS.errors);
    }

    GraphicsPipelineDesc desc;
    desc.vertexShader = m_fullscreenVS;
    desc.pixelShader = m_dagRaymarchPS;

    // b0: FrameConstants CBV
    RootParameter cbv;
    cbv.type = RootParamType::ConstantBuffer;
    cbv.shaderRegister = 0;
    cbv.registerSpace = 0;
    cbv.visibility = D3D12_SHADER_VISIBILITY_ALL;
    desc.rootParams.push_back(cbv);
    // t0 DagNodes, t1 DagChildPointers, t2 DagPages, t3 DagPageIndex
    for (uint32_t reg = 0; reg < 4; ++reg) {
        desc.rootParams.push_back({
            RootParamType::DescriptorTable,
            reg,
            0,
            D3D12_SHADER_VISIBILITY_PIXEL,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
    }

    desc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.inputLayout.clear();
    // Mirror the fullscreen pass's stencil ownership exactly (paint only where the near
    // mesh left stencil == 0), and add alpha blending so the DAG overwrites the existing
    // mid/far background where a ray hits (alpha 1) and leaves it where it misses (alpha 0).
    desc.depthEnable = true;
    desc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.depthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.stencilEnable = true;
    desc.stencilReadMask = 0xFFu;
    desc.stencilWriteMask = 0x00u;
    desc.frontStencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    desc.frontStencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.blendEnable = true;
    desc.cullMode = D3D12_CULL_MODE_NONE;
    desc.debugName = "DagRaymarchPipeline";

    auto result = m_dagRaymarchPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create DAG raymarch pipeline: {}", result.error());
    }
    spdlog::info("DAG raymarch pipeline created successfully");
    return {};
}

void Renderer::RenderDagRaymarch(
    ID3D12GraphicsCommandList* cmdList,
    const DescriptorHandle& dagNodeSRV,
    const DescriptorHandle& dagChildPtrSRV,
    const DescriptorHandle& dagPageSRV,
    const DescriptorHandle& dagPageIndexSRV,
    const CameraParams& camera,
    uint32_t dagPageCount,
    uint32_t dagNodeCount,
    float pageSize,
    int32_t pageRadius,
    float rootMinY)
{
    if (!cmdList) return;
    if (!dagNodeSRV.IsValid() || !dagChildPtrSRV.IsValid() ||
        !dagPageSRV.IsValid() || !dagPageIndexSRV.IsValid()) {
        return;
    }
    if (dagNodeCount == 0u || dagPageCount == 0u || pageSize <= 0.0f) {
        return;
    }
    if (m_dagRaymarchPipeline.GetPSO() == nullptr) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    m_dagRaymarchPipeline.Bind(cmdList);
    cmdList->OMSetStencilRef(0);

    FrameConstantsCpu constants = {};
    constants.cameraPosition[0] = FiniteOr(camera.posX, 0.0f);
    constants.cameraPosition[1] = FiniteOr(camera.posY, 0.0f);
    constants.cameraPosition[2] = FiniteOr(camera.posZ, 0.0f);
    constants.cameraPosition[3] = ClampFinite(camera.fov, 1.0f, 175.0f, 75.0f);
    constants.cameraForward[0] = FiniteOr(camera.forwardX, 0.0f);
    constants.cameraForward[1] = FiniteOr(camera.forwardY, 0.0f);
    constants.cameraForward[2] = FiniteOr(camera.forwardZ, 1.0f);
    constants.cameraForward[3] = std::max(0.001f, FiniteOr(camera.aspectRatio, 1.0f));
    constants.cameraRight[0] = FiniteOr(camera.rightX, 1.0f);
    constants.cameraRight[1] = FiniteOr(camera.rightY, 0.0f);
    constants.cameraRight[2] = FiniteOr(camera.rightZ, 0.0f);
    constants.cameraUp[0] = FiniteOr(camera.upX, 0.0f);
    constants.cameraUp[1] = FiniteOr(camera.upY, 1.0f);
    constants.cameraUp[2] = FiniteOr(camera.upZ, 0.0f);
    constants.sunDirection[0] = 0.5f;
    constants.sunDirection[1] = 1.0f;
    constants.sunDirection[2] = 0.3f;
    constants.sunDirection[3] = 1.0f;
    constants.viewportWidth = static_cast<float>(m_width);
    constants.viewportHeight = static_cast<float>(m_height);
    constants.frameIndex = camera.frameIndex;
    constants.debugMode = camera.debugMode;
    constants.farFieldParams[0] = 1.0f;
    constants.farFieldParams[1] = static_cast<float>(dagPageCount);
    constants.farFieldParams[2] = static_cast<float>(dagNodeCount);
    constants.farFieldParams[3] = pageSize;
    constants.farFieldGridParams[0] = static_cast<float>(pageRadius);
    constants.farFieldGridParams[1] = static_cast<float>(pageRadius * 2 + 1);
    constants.farFieldGridParams[2] = rootMinY;
    constants.farFieldGridParams[3] = 0.0f;

    UploadBuffer& frameConstantsUpload = m_dagConstantUploads[m_currentFrameIndex];
    if (void* mapped = frameConstantsUpload.GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }
    cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, dagNodeSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(2, dagChildPtrSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(3, dagPageSRV.gpu);
    cmdList->SetGraphicsRootDescriptorTable(4, dagPageIndexSRV.gpu);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

Result<void> Renderer::CreateSparseSurfacePipeline(ID3D12Device* device) {
    std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_SparseSurface.hlsl";
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_SparseSurface.hlsl";
    std::filesystem::path earlyDepthPsPath =
        m_config.shaderPath / "Graphics" / "PS_SparseSurfaceEarlyDepth.hlsl";
    std::filesystem::path depthPrepassPsPath =
        m_config.shaderPath / "Graphics" / "PS_SparseSurfaceDepthPrepass.hlsl";

    auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
    if (!vsResult) {
        return Error("Failed to compile sparse surface vertex shader: {}", vsResult.error());
    }
    m_sparseSurfaceVS = vsResult.value();
    if (!m_sparseSurfaceVS.IsValid()) {
        return Error("Sparse surface vertex shader compilation failed: {}", m_sparseSurfaceVS.errors);
    }

    auto psResult = m_shaderCompiler.CompilePixelShader(psPath, L"main", m_config.debugShaders);
    if (!psResult) {
        return Error("Failed to compile sparse surface pixel shader: {}", psResult.error());
    }
    m_sparseSurfacePS = psResult.value();
    if (!m_sparseSurfacePS.IsValid()) {
        return Error("Sparse surface pixel shader compilation failed: {}", m_sparseSurfacePS.errors);
    }

    if (m_config.sparseSurfaceDepthPrepass) {
        auto earlyDepthPsResult =
            m_shaderCompiler.CompilePixelShader(earlyDepthPsPath, L"main", m_config.debugShaders);
        if (!earlyDepthPsResult) {
            return Error("Failed to compile sparse surface early-depth pixel shader: {}", earlyDepthPsResult.error());
        }
        m_sparseSurfaceEarlyDepthPS = earlyDepthPsResult.value();
        if (!m_sparseSurfaceEarlyDepthPS.IsValid()) {
            return Error(
                "Sparse surface early-depth pixel shader compilation failed: {}",
                m_sparseSurfaceEarlyDepthPS.errors);
        }

        auto depthPrepassPsResult =
            m_shaderCompiler.CompilePixelShader(depthPrepassPsPath, L"main", m_config.debugShaders);
        if (!depthPrepassPsResult) {
            return Error("Failed to compile sparse surface depth-prepass pixel shader: {}", depthPrepassPsResult.error());
        }
        m_sparseSurfaceDepthPrepassPS = depthPrepassPsResult.value();
        if (!m_sparseSurfaceDepthPrepassPS.IsValid()) {
            return Error(
                "Sparse surface depth-prepass pixel shader compilation failed: {}",
                m_sparseSurfaceDepthPrepassPS.errors);
        }
    }

    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_sparseSurfaceVS;
    pipelineDesc.pixelShader = m_sparseSurfacePS;
    pipelineDesc.debugName = "SparseSurfacePipeline";

    pipelineDesc.rootParams.push_back({
        RootParamType::ConstantBuffer,
        0,
        0,
        D3D12_SHADER_VISIBILITY_ALL
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_VERTEX,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        1,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        2,
        0,
        D3D12_SHADER_VISIBILITY_VERTEX,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        3,
        0,
        D3D12_SHADER_VISIBILITY_VERTEX,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        6,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        7,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        8,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        9,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.staticSamplers.push_back({
        0,
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_INPUT_ELEMENT_DESC faceVertexElement = {};
    faceVertexElement.SemanticName = "FACEVERTEX";
    faceVertexElement.SemanticIndex = 0;
    faceVertexElement.Format = DXGI_FORMAT_R32_UINT;
    faceVertexElement.InputSlot = 0;
    faceVertexElement.AlignedByteOffset = 0;
    faceVertexElement.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    faceVertexElement.InstanceDataStepRate = 0;
    pipelineDesc.inputLayout.push_back(faceVertexElement);
    pipelineDesc.depthEnable = true;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipelineDesc.stencilEnable = true;
    pipelineDesc.stencilReadMask = 0xFFu;
    pipelineDesc.stencilWriteMask = 0xFFu;
    pipelineDesc.frontStencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipelineDesc.frontStencilPassOp = D3D12_STENCIL_OP_REPLACE;
    pipelineDesc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pipelineDesc.frontCounterClockwise = true;
    pipelineDesc.cullMode = D3D12_CULL_MODE_BACK;

    if (m_config.sparseSurfaceDepthPrepass) {
        GraphicsPipelineDesc depthPrepassDesc = pipelineDesc;
        depthPrepassDesc.pixelShader = m_sparseSurfaceDepthPrepassPS;
        depthPrepassDesc.debugName = "SparseSurfaceDepthPrepassPipeline";
        depthPrepassDesc.rtvFormats.clear();
        auto depthPrepassResult =
            m_sparseSurfaceDepthPrepassPipeline.Initialize(device, depthPrepassDesc);
        if (!depthPrepassResult) {
            return Error("Failed to create sparse surface depth-prepass pipeline: {}", depthPrepassResult.error());
        }
        spdlog::info("Sparse surface depth pre-pass pipeline created successfully");

        pipelineDesc.pixelShader = m_sparseSurfaceEarlyDepthPS;
        pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    }

    auto result = m_sparseSurfacePipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create sparse surface pipeline: {}", result.error());
    }
    spdlog::info("Sparse surface pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateOverlayPipeline(ID3D12Device* device) {
    std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_Fullscreen.hlsl";
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_Overlay.hlsl";

    CompiledShader overlayVS = m_fullscreenVS;
    if (!overlayVS.IsValid()) {
        auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
        if (!vsResult) {
            return Error("Failed to compile overlay vertex shader: {}", vsResult.error());
        }
        overlayVS = vsResult.value();
    }

    auto psResult = m_shaderCompiler.CompilePixelShader(psPath, L"main", m_config.debugShaders);
    if (!psResult) {
        return Error("Failed to compile overlay pixel shader: {}", psResult.error());
    }
    m_overlayPS = psResult.value();
    if (!m_overlayPS.IsValid()) {
        return Error("Overlay pixel shader compilation failed: {}", m_overlayPS.errors);
    }

    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = overlayVS;
    pipelineDesc.pixelShader = m_overlayPS;
    pipelineDesc.debugName = "OverlayPipeline";
    pipelineDesc.rootParams.push_back({
        RootParamType::ConstantBuffer,
        0,
        0,
        D3D12_SHADER_VISIBILITY_ALL
    });
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        1,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.staticSamplers.push_back({
        0,
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    pipelineDesc.inputLayout.clear();
    pipelineDesc.depthEnable = false;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.stencilEnable = false;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.blendEnable = true;

    auto result = m_overlayPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create overlay pipeline: {}", result.error());
    }

    spdlog::info("Overlay pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateBackgroundCompositePipeline(ID3D12Device* device) {
    if (!UseBackgroundPassSplit()) {
        return {};
    }
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_BackgroundComposite.hlsl";

    CompiledShader compositeVS = m_fullscreenVS;
    if (!compositeVS.IsValid()) {
        std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_Fullscreen.hlsl";
        auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
        if (!vsResult) {
            return Error("Failed to compile background composite vertex shader: {}", vsResult.error());
        }
        compositeVS = vsResult.value();
    }

    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    if (m_config.backgroundPassCompositeForceColor) {
        psOptions.defines.push_back(L"VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR=1");
    }
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile background composite pixel shader: {}", psResult.error());
    }
    m_backgroundCompositePS = psResult.value();
    if (!m_backgroundCompositePS.IsValid()) {
        return Error("Background composite pixel shader compilation failed: {}", m_backgroundCompositePS.errors);
    }

    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = compositeVS;
    pipelineDesc.pixelShader = m_backgroundCompositePS;
    pipelineDesc.debugName = "BackgroundCompositePipeline";
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.staticSamplers.push_back({
        0,
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    pipelineDesc.inputLayout.clear();
    pipelineDesc.depthEnable = false;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipelineDesc.stencilEnable = !m_config.backgroundPassCompositeDebug;
    pipelineDesc.stencilReadMask = 0xFFu;
    pipelineDesc.stencilWriteMask = 0x00u;
    pipelineDesc.frontStencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pipelineDesc.frontStencilPassOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;

    auto result = m_backgroundCompositePipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create background composite pipeline: {}", result.error());
    }

    spdlog::info(
        "Background composite pipeline created successfully forceColor={}",
        m_config.backgroundPassCompositeForceColor ? 1 : 0);
    return {};
}

Result<void> Renderer::CreateBackgroundTemporalPipeline(ID3D12Device* device) {
    // TAA lane increment 2 (env VENPOD_BG_TEMPORAL, default off): reprojected
    // history blend over the fresh background. Env is read here directly so the
    // pipeline exists regardless of Create*Resources ordering.
    const char* bgTemporalEnv = std::getenv("VENPOD_BG_TEMPORAL");
    const bool temporalEnabled =
        bgTemporalEnv != nullptr && bgTemporalEnv[0] != '0' && bgTemporalEnv[0] != '\0';
    if (!UseBackgroundPassSplit() || !temporalEnabled) {
        return {};
    }

    if (const char* blendEnv = std::getenv("VENPOD_BG_TEMPORAL_BLEND")) {
        const float parsed = static_cast<float>(std::atof(blendEnv));
        if (parsed > 0.0f && parsed <= 0.95f) {
            m_backgroundTemporalBlend = parsed;
        }
    }

    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_BackgroundTemporal.hlsl";

    CompiledShader temporalVS = m_fullscreenVS;
    if (!temporalVS.IsValid()) {
        std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_Fullscreen.hlsl";
        auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
        if (!vsResult) {
            return Error("Failed to compile background temporal vertex shader: {}", vsResult.error());
        }
        temporalVS = vsResult.value();
    }

    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile background temporal pixel shader: {}", psResult.error());
    }
    m_backgroundTemporalPS = psResult.value();
    if (!m_backgroundTemporalPS.IsValid()) {
        return Error("Background temporal pixel shader compilation failed: {}", m_backgroundTemporalPS.errors);
    }

    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = temporalVS;
    pipelineDesc.pixelShader = m_backgroundTemporalPS;
    pipelineDesc.debugName = "BackgroundTemporalPipeline";
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    RootParameter temporalConstants{};
    temporalConstants.type = RootParamType::Constants32Bit;
    temporalConstants.shaderRegister = 0;
    temporalConstants.visibility = D3D12_SHADER_VISIBILITY_PIXEL;
    temporalConstants.num32BitValues = 36;
    pipelineDesc.rootParams.push_back(temporalConstants);
    pipelineDesc.staticSamplers.push_back({
        0,
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    pipelineDesc.inputLayout.clear();
    // Blends into the background color target while its DSV is still bound:
    // depth/stencil fully disabled, but the DSV format must match.
    pipelineDesc.blendEnable = true;
    pipelineDesc.depthEnable = false;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipelineDesc.stencilEnable = false;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;

    auto result = m_backgroundTemporalPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create background temporal pipeline: {}", result.error());
    }

    spdlog::info(
        "Background temporal pipeline created successfully blend={:.2f}",
        m_backgroundTemporalBlend);
    return {};
}

Result<void> Renderer::CreateMidCompositePipeline(ID3D12Device* device) {
    if (!m_config.midPassEnabled) {
        return {};
    }
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_MidComposite.hlsl";

    CompiledShader compositeVS = m_fullscreenVS;
    if (!compositeVS.IsValid()) {
        std::filesystem::path vsPath = m_config.shaderPath / "Graphics" / "VS_Fullscreen.hlsl";
        auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
        if (!vsResult) {
            return Error("Failed to compile mid composite vertex shader: {}", vsResult.error());
        }
        compositeVS = vsResult.value();
    }

    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile mid composite pixel shader: {}", psResult.error());
    }
    m_midCompositePS = psResult.value();
    if (!m_midCompositePS.IsValid()) {
        return Error("Mid composite pixel shader compilation failed: {}", m_midCompositePS.errors);
    }

    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = compositeVS;
    pipelineDesc.pixelShader = m_midCompositePS;
    pipelineDesc.debugName = "MidCompositePipeline";
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.staticSamplers.push_back({
        0,
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_SHADER_VISIBILITY_PIXEL
    });
    pipelineDesc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    pipelineDesc.inputLayout.clear();
    // The upscaled mid composite only writes where the near surface did not own
    // the pixel (stencil == 0), mirroring the fullscreen/background composite,
    // and alpha-over blends so mid coverage (alpha) composites over the full
    // pass already in the main RT.
    pipelineDesc.depthEnable = true;
    pipelineDesc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipelineDesc.stencilEnable = true;
    pipelineDesc.stencilReadMask = 0xFFu;
    pipelineDesc.stencilWriteMask = 0x00u;
    pipelineDesc.frontStencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pipelineDesc.frontStencilPassOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipelineDesc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.blendEnable = true;

    auto result = m_midCompositePipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create mid composite pipeline: {}", result.error());
    }

    spdlog::info("Mid composite pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateSparseSurfaceDrawCommandSignature(ID3D12Device* device) {
    if (!device) {
        return Error("Renderer::CreateSparseSurfaceDrawCommandSignature - device is null");
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(uint32_t) * 5u;
    desc.NumArgumentDescs = 1u;
    desc.pArgumentDescs = &argument;
    desc.NodeMask = 0u;

    HRESULT hr = device->CreateCommandSignature(
        &desc,
        nullptr,
        IID_PPV_ARGS(&m_sparseSurfaceDrawSignature));
    if (FAILED(hr)) {
        return Error("Failed to create sparse surface draw command signature: HRESULT 0x{:08x}",
            static_cast<uint32_t>(hr));
    }

    spdlog::info("Sparse surface indirect draw command signature created");
    return {};
}

Result<void> Renderer::CreateRTVsForSwapchain() {
    if (!m_device || !m_window) {
        return Error("Device or window not initialized");
    }

    ID3D12Device* device = m_device->GetDevice();
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

Result<void> Renderer::CreateDepthBuffer() {
    if (!m_device || !m_window) {
        return Error("Device or window not initialized");
    }

    ID3D12Device* device = m_device->GetDevice();
    if (!m_dsvHandle.IsValid()) {
        m_dsvHandle = m_heapManager.AllocateDsv();
        if (!m_dsvHandle.IsValid()) {
            return Error("Failed to allocate sparse surface DSV");
        }
    }

    m_depthBuffer.Reset();

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        static_cast<UINT64>(std::max(1u, m_width)),
        static_cast<UINT>(std::max(1u, m_height)),
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&m_depthBuffer));
    if (FAILED(hr)) {
        return Error("Failed to create sparse surface depth buffer: 0x{:08X}", hr);
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsvHandle.cpu);
    spdlog::debug("Created sparse surface depth/stencil buffer {}x{}", m_width, m_height);
    return {};
}

void Renderer::DestroyBackgroundPassResources() {
    if (m_backgroundPassSrv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_backgroundPassSrv);
    }
    if (m_backgroundPassStagingSrv.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_backgroundPassStagingSrv);
    }
    if (m_backgroundPassRtv.IsValid()) {
        m_heapManager.FreeRtv(m_backgroundPassRtv);
    }
    if (m_backgroundPassDsv.IsValid()) {
        m_heapManager.FreeDsv(m_backgroundPassDsv);
    }
    if (m_backgroundPassHistorySrv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_backgroundPassHistorySrv);
    }
    if (m_backgroundPassHistoryStagingSrv.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_backgroundPassHistoryStagingSrv);
    }
    m_backgroundPassHistory.Reset();
    m_backgroundPassHistoryState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_backgroundHistoryValid = false;
    m_backgroundPrevCameraValid = false;
    m_backgroundPassColor.Reset();
    m_backgroundPassDepth.Reset();
    m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_backgroundPassWidth = 0;
    m_backgroundPassHeight = 0;
}

Result<void> Renderer::CreateBackgroundPassResources() {
    if (!UseBackgroundPassSplit()) {
        DestroyBackgroundPassResources();
        return {};
    }
    if (!m_device) {
        return Error("Device not initialized");
    }
    ID3D12Device* device = m_device->GetDevice();
    if (!device) {
        return Error("D3D12 device not initialized");
    }

    DestroyBackgroundPassResources();

    const float scale = std::clamp(m_config.backgroundPassScale, 0.25f, 1.0f);
    m_backgroundPassWidth = std::max(
        1u,
        static_cast<uint32_t>(std::lround(static_cast<float>(std::max(1u, m_width)) * scale)));
    m_backgroundPassHeight = std::max(
        1u,
        static_cast<uint32_t>(std::lround(static_cast<float>(std::max(1u, m_height)) * scale)));

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE colorClear = {};
    colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorClear.Color[0] = 0.42f;
    colorClear.Color[1] = 0.55f;
    colorClear.Color[2] = 0.74f;
    colorClear.Color[3] = 1.0f;
    auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<UINT64>(m_backgroundPassWidth),
        static_cast<UINT>(m_backgroundPassHeight),
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &colorDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &colorClear,
        IID_PPV_ARGS(&m_backgroundPassColor));
    if (FAILED(hr)) {
        DestroyBackgroundPassResources();
        return Error("Failed to create background pass color target: 0x{:08X}", hr);
    }
    m_backgroundPassColor->SetName(L"BackgroundPassColor");
    m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    m_backgroundPassRtv = m_heapManager.AllocateRtv();
    if (!m_backgroundPassRtv.IsValid()) {
        DestroyBackgroundPassResources();
        return Error("Failed to allocate background pass RTV");
    }
    device->CreateRenderTargetView(m_backgroundPassColor.Get(), nullptr, m_backgroundPassRtv.cpu);

    m_backgroundPassStagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
    if (!m_backgroundPassStagingSrv.IsValid()) {
        DestroyBackgroundPassResources();
        return Error("Failed to allocate background pass staging SRV");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(m_backgroundPassColor.Get(), &srvDesc, m_backgroundPassStagingSrv.cpu);
    m_backgroundPassSrv = m_heapManager.CopyToShaderVisible(device, m_backgroundPassStagingSrv);
    if (!m_backgroundPassSrv.IsValid()) {
        DestroyBackgroundPassResources();
        return Error("Failed to allocate background pass shader-visible SRV");
    }
    device->CreateShaderResourceView(m_backgroundPassColor.Get(), &srvDesc, m_backgroundPassSrv.cpu);

    // Temporal history (TAA lane increment 1, env-gated, default off): same
    // desc as the color target so CopyResource is legal; the increment-2 blend
    // pass renders into it, so keep the RENDER_TARGET flag.
    const char* bgTemporalEnv = std::getenv("VENPOD_BG_TEMPORAL");
    m_backgroundTemporalEnabled =
        bgTemporalEnv != nullptr && bgTemporalEnv[0] != '0' && bgTemporalEnv[0] != '\0';
    if (m_backgroundTemporalEnabled) {
        hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &colorDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &colorClear,
            IID_PPV_ARGS(&m_backgroundPassHistory));
        if (FAILED(hr)) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background temporal history: 0x{:08X}", hr);
        }
        m_backgroundPassHistory->SetName(L"BackgroundPassHistory");
        m_backgroundPassHistoryState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_backgroundPassHistoryStagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
        if (!m_backgroundPassHistoryStagingSrv.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background history staging SRV");
        }
        device->CreateShaderResourceView(
            m_backgroundPassHistory.Get(), &srvDesc, m_backgroundPassHistoryStagingSrv.cpu);
        m_backgroundPassHistorySrv =
            m_heapManager.CopyToShaderVisible(device, m_backgroundPassHistoryStagingSrv);
        if (!m_backgroundPassHistorySrv.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background history shader-visible SRV");
        }
        device->CreateShaderResourceView(
            m_backgroundPassHistory.Get(), &srvDesc, m_backgroundPassHistorySrv.cpu);
    }

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;
    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        static_cast<UINT64>(m_backgroundPassWidth),
        static_cast<UINT>(m_backgroundPassHeight),
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthClear,
        IID_PPV_ARGS(&m_backgroundPassDepth));
    if (FAILED(hr)) {
        DestroyBackgroundPassResources();
        return Error("Failed to create background pass depth target: 0x{:08X}", hr);
    }
    m_backgroundPassDepth->SetName(L"BackgroundPassDepth");

    m_backgroundPassDsv = m_heapManager.AllocateDsv();
    if (!m_backgroundPassDsv.IsValid()) {
        DestroyBackgroundPassResources();
        return Error("Failed to allocate background pass DSV");
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_backgroundPassDepth.Get(), &dsvDesc, m_backgroundPassDsv.cpu);

    spdlog::info(
        "Background pass resources created: {}x{} scale={:.3f} main={}x{} srvIndex={} stagingSrvIndex={}",
        m_backgroundPassWidth,
        m_backgroundPassHeight,
        scale,
        m_width,
        m_height,
        m_backgroundPassSrv.heapIndex,
        m_backgroundPassStagingSrv.heapIndex);
    return {};
}

void Renderer::DestroyMidPassResources() {
    if (m_midPassSrv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_midPassSrv);
    }
    if (m_midPassStagingSrv.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_midPassStagingSrv);
    }
    if (m_midPassRtv.IsValid()) {
        m_heapManager.FreeRtv(m_midPassRtv);
    }
    m_midPassColor.Reset();
    m_midPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_midPassWidth = 0;
    m_midPassHeight = 0;
}

Result<void> Renderer::CreateMidPassResources() {
    if (!m_config.midPassEnabled) {
        DestroyMidPassResources();
        return {};
    }
    if (!m_device) {
        return Error("Device not initialized");
    }
    ID3D12Device* device = m_device->GetDevice();
    if (!device) {
        return Error("D3D12 device not initialized");
    }

    DestroyMidPassResources();

    // Allow >1.0 = SSAA supersample of the mid pass (diagnostic + quality lever).
    const float scale = std::clamp(m_config.midPassScale, 0.25f, 2.0f);
    m_midPassWidth = std::max(
        1u,
        static_cast<uint32_t>(std::lround(static_cast<float>(std::max(1u, m_width)) * scale)));
    m_midPassHeight = std::max(
        1u,
        static_cast<uint32_t>(std::lround(static_cast<float>(std::max(1u, m_height)) * scale)));

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE colorClear = {};
    colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorClear.Color[0] = 0.0f;
    colorClear.Color[1] = 0.0f;
    colorClear.Color[2] = 0.0f;
    colorClear.Color[3] = 0.0f;
    auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<UINT64>(m_midPassWidth),
        static_cast<UINT>(m_midPassHeight),
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &colorDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &colorClear,
        IID_PPV_ARGS(&m_midPassColor));
    if (FAILED(hr)) {
        DestroyMidPassResources();
        return Error("Failed to create mid pass color target: 0x{:08X}", hr);
    }
    m_midPassColor->SetName(L"MidPassColor");
    m_midPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    m_midPassRtv = m_heapManager.AllocateRtv();
    if (!m_midPassRtv.IsValid()) {
        DestroyMidPassResources();
        return Error("Failed to allocate mid pass RTV");
    }
    device->CreateRenderTargetView(m_midPassColor.Get(), nullptr, m_midPassRtv.cpu);

    m_midPassStagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
    if (!m_midPassStagingSrv.IsValid()) {
        DestroyMidPassResources();
        return Error("Failed to allocate mid pass staging SRV");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(m_midPassColor.Get(), &srvDesc, m_midPassStagingSrv.cpu);
    m_midPassSrv = m_heapManager.CopyToShaderVisible(device, m_midPassStagingSrv);
    if (!m_midPassSrv.IsValid()) {
        DestroyMidPassResources();
        return Error("Failed to allocate mid pass shader-visible SRV");
    }
    device->CreateShaderResourceView(m_midPassColor.Get(), &srvDesc, m_midPassSrv.cpu);

    spdlog::info(
        "Mid pass resources created: {}x{} scale={:.3f} main={}x{} srvIndex={} stagingSrvIndex={}",
        m_midPassWidth,
        m_midPassHeight,
        scale,
        m_width,
        m_height,
        m_midPassSrv.heapIndex,
        m_midPassStagingSrv.heapIndex);
    return {};
}


} // namespace VENPOD::Graphics
