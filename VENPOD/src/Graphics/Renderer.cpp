#include "Renderer.h"
#include "Graphics/BackbufferCapture.h"
#include "RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <type_traits>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kFarHeightfieldCellCount = 384u;
constexpr uint32_t kFarHeightfieldFacesPerCell = 2u; // top + one camera-facing top curtain
constexpr uint32_t kFarHeightfieldFaceCount =
    kFarHeightfieldCellCount * kFarHeightfieldCellCount * kFarHeightfieldFacesPerCell;
constexpr uint32_t kFarHeightfieldVertexCount =
    kFarHeightfieldFaceCount * 4u;
constexpr uint32_t kFarHeightfieldIndexCount =
    kFarHeightfieldFaceCount * 6u;
constexpr uint32_t kFarHeightfieldFaceStride = 16u;
constexpr uint64_t kFarHeightfieldFaceBufferBytes =
    static_cast<uint64_t>(kFarHeightfieldFaceCount) * kFarHeightfieldFaceStride;
constexpr uint32_t kFarHeightfieldCellSizeVoxels = 28u;
constexpr float kFarHeightfieldCellSize = static_cast<float>(kFarHeightfieldCellSizeVoxels);
constexpr float kFarHeightfieldMaxDistance = 10400.0f;
constexpr float kFarHeightfieldCpuProbeSurfaceY = 260.0f;
constexpr float kFarHeightfieldOwnerMaxDistance = 11000.0f;
constexpr uint32_t kFarMaxHeightCacheLeafCellCount = 1366u;
constexpr uint32_t kFarMaxHeightCacheMipLevels = 12u;
constexpr uint32_t kFarMaxHeightCacheElementCount = 2488550u;
constexpr uint32_t kFarMaxHeightCacheStride = sizeof(float);
constexpr uint64_t kFarMaxHeightCacheBufferBytes =
    static_cast<uint64_t>(kFarMaxHeightCacheElementCount) * kFarMaxHeightCacheStride;
constexpr uint32_t kFarMaxHeightCacheCellSizeVoxels = 16u;
constexpr float kFarMaxHeightCacheCellSize = static_cast<float>(kFarMaxHeightCacheCellSizeVoxels);
constexpr float kFarMaxHeightCacheHeightPad = 64.0f;
constexpr uint32_t kFarMaxHeightScreenMaskTileWidth = 8u;
constexpr float kFarMaxHeightScreenMaskDilationPixels = 4.0f;
constexpr uint32_t kFarMaxHeightScreenMaskProjectMipLevel = 3u;

struct FarHeightfieldFaceCpu {
    int32_t worldX;
    int32_t worldY;
    int32_t worldZ;
    uint32_t payload;
};

static_assert(sizeof(FarHeightfieldFaceCpu) == kFarHeightfieldFaceStride);

struct FarHeightfieldGenerateConstants {
    uint32_t faceCount;
    uint32_t cellCount;
    uint32_t baseGridCellSize;
    uint32_t worldSeed;
    uint32_t originXBits;
    uint32_t originZBits;
    uint32_t farHandoffBits;
    uint32_t ownerMaxDistanceBits;
    uint32_t cameraXBits;
    uint32_t cameraYBits;
    uint32_t cameraZBits;
    uint32_t pad0;
};
static_assert(sizeof(FarHeightfieldGenerateConstants) == 48);

struct FarMaxHeightCacheGenerateConstants {
    uint32_t level = 0;
    uint32_t srcOffset = 0;
    uint32_t dstOffset = 0;
    uint32_t worldSeed = 0;
    uint32_t srcWidth = 1;
    uint32_t srcHeight = 1;
    uint32_t dstWidth = 1;
    uint32_t dstHeight = 1;
    uint32_t originXBits = 0;
    uint32_t originZBits = 0;
    uint32_t leafCellSizeBits = 0;
    uint32_t heightPadBits = 0;
    uint32_t cameraXBits = 0;
    uint32_t cameraYBits = 0;
    uint32_t cameraZBits = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(FarMaxHeightCacheGenerateConstants) == 64);

struct FarMaxHeightScreenMaskConstants {
    uint32_t passIndex = 0;
    uint32_t tileCount = 0;
    uint32_t tileWidth = kFarMaxHeightScreenMaskTileWidth;
    uint32_t dilationPixelsBits = 0;
    uint32_t projectOffset = 0;
    uint32_t projectSide = 1;
    uint32_t projectCellSizeBits = 0;
    uint32_t reserved0 = 0;
};
static_assert(sizeof(FarMaxHeightScreenMaskConstants) == 32);

struct BackgroundHorizonTileMaskConstants {
    uint32_t fullWidth = 1;
    uint32_t fullHeight = 1;
    uint32_t tileSize = 8;
    uint32_t thresholdBits = 0;
    uint32_t y0 = 320;
    uint32_t y1 = 480;
    uint32_t frameIndex = 0;
    uint32_t selectorMode = 0;
};
static_assert(sizeof(BackgroundHorizonTileMaskConstants) == 32);

void ComputeFarMaxHeightCacheMipInfo(
    uint32_t level,
    uint32_t& outOffset,
    uint32_t& outSide,
    float& outCellSize)
{
    outOffset = 0u;
    outSide = kFarMaxHeightCacheLeafCellCount;
    outCellSize = kFarMaxHeightCacheCellSize;
    uint32_t side = kFarMaxHeightCacheLeafCellCount;
    for (uint32_t mip = 0; mip < level && mip + 1u < kFarMaxHeightCacheMipLevels; ++mip) {
        outOffset += side * side;
        side = std::max(1u, (side + 1u) / 2u);
        outCellSize *= 2.0f;
    }
    outSide = side;
}

void ComputeFarMaxHeightCacheOrigin(float cameraX, float cameraZ, float& originX, float& originZ) {
    const float extent =
        static_cast<float>(kFarMaxHeightCacheLeafCellCount) * kFarMaxHeightCacheCellSize;
    const float halfExtent = extent * 0.5f;
    originX = std::floor((cameraX - halfExtent) / kFarMaxHeightCacheCellSize) *
        kFarMaxHeightCacheCellSize;
    originZ = std::floor((cameraZ - halfExtent) / kFarMaxHeightCacheCellSize) *
        kFarMaxHeightCacheCellSize;
}

uint32_t PackSparseSurfacePayload(uint32_t direction, uint32_t voxel, uint32_t width, uint32_t height) {
    return ((direction & 0x7u) << 29u) |
        (((width - 1u) & 0x1Fu) << 24u) |
        (((height - 1u) & 0x1Fu) << 19u) |
        (voxel & 0x0007FFFFu);
}

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
    float farMaxHeightCacheParams[4]; // x = enabled, y/z = origin x/z, w = leaf cell size
    float farMaxHeightCacheParams2[4]; // x = leaf side, y = mip levels, z = height pad, w = horizon state
    float temporalParams[4];      // x = temporal active, y = cameraStatic, z = tile phase count N, w = tile size px
    // Stage 2b motion reprojection: previous-frame camera basis so the compute reproject can
    // reconstruct prev-frame world hits (prevCamPos + prevRayDir(prevUV)*prevDist) and project them
    // into the current camera. w channels mirror the current camera (fov, aspect).
    float prevCameraPosition[4];  // xyz = prev position, w = prev fov
    float prevCameraForward[4];   // xyz = prev forward, w = prev aspect
    float prevCameraRight[4];     // xyz = prev right
    float prevCameraUp[4];        // xyz = prev up
};

static_assert(sizeof(FrameConstantsCpu) == 480);
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
static_assert(offsetof(FrameConstantsCpu, farMaxHeightCacheParams) == 368u);
static_assert(offsetof(FrameConstantsCpu, farMaxHeightCacheParams2) == 384u);
static_assert(offsetof(FrameConstantsCpu, temporalParams) == 400u);
static_assert(offsetof(FrameConstantsCpu, prevCameraPosition) == 416u);
static_assert(offsetof(FrameConstantsCpu, prevCameraForward) == 432u);
static_assert(offsetof(FrameConstantsCpu, prevCameraRight) == 448u);
static_assert(offsetof(FrameConstantsCpu, prevCameraUp) == 464u);

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

uint32_t Uint32BitsFromFloat(float value) {
    uint32_t bits = 0u;
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

    spdlog::info("RENDERER_INIT_STAGE begin CreateRTVsForSwapchain");
    result = CreateRTVsForSwapchain();
    if (!result) {
        return Error("Failed to create RTVs: {}", result.error());
    }
    spdlog::info("RENDERER_INIT_STAGE end CreateRTVsForSwapchain");
    spdlog::info("RENDERER_INIT_STAGE begin CreateDepthBuffer");
    result = CreateDepthBuffer();
    if (!result) {
        return Error("Failed to create depth buffer: {}", result.error());
    }
    spdlog::info("RENDERER_INIT_STAGE end CreateDepthBuffer");
    if (UseBackgroundPassSplit()) {
        spdlog::info("RENDERER_INIT_STAGE begin CreateBackgroundPassResources");
        result = CreateBackgroundPassResources();
        if (!result) {
            return Error("Failed to create background pass resources: {}", result.error());
        }
        spdlog::info("RENDERER_INIT_STAGE end CreateBackgroundPassResources");
    }
    if (UseTemporalReproject()) {
        spdlog::info("RENDERER_INIT_STAGE begin CreateTemporalResources");
        result = CreateTemporalResources();
        if (!result) {
            return Error("Failed to create temporal resources: {}", result.error());
        }
        spdlog::info("RENDERER_INIT_STAGE end CreateTemporalResources");
    }
    spdlog::info("RENDERER_INIT_STAGE begin CreateFarMaxHeightNoHitMaskResources");
    result = CreateFarMaxHeightNoHitMaskResources();
    if (!result) {
        return Error("Failed to create far max-height no-hit mask resources: {}", result.error());
    }
    spdlog::info("RENDERER_INIT_STAGE end CreateFarMaxHeightNoHitMaskResources");
    if (m_config.midPassEnabled) {
        spdlog::info("RENDERER_INIT_STAGE begin CreateMidPassResources");
        result = CreateMidPassResources();
        if (!result) {
            return Error("Failed to create mid pass resources: {}", result.error());
        }
        spdlog::info("RENDERER_INIT_STAGE end CreateMidPassResources");
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
        std::snprintf(name, sizeof(name), "FarHeightfieldFrameConstants_%u", i);
        result = m_farHeightfieldConstantUploads[i].Initialize(
            device.GetDevice(),
            kFrameConstantUploadBytes,
            name);
        if (!result) {
            return Error("Failed to create far heightfield frame constants upload buffer: {}", result.error());
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
    if (m_config.farHeightfieldOwnerEnabled) {
        result = CreateFarHeightfieldOwnerResources(device.GetDevice());
        if (!result) {
            return Error("Failed to create far heightfield owner resources: {}", result.error());
        }
    }
    if (m_config.farMaxHeightCacheEnabled) {
        result = CreateFarMaxHeightCacheResources(device.GetDevice());
        if (!result) {
            return Error("Failed to create far max-height cache resources: {}", result.error());
        }
    }
    if (m_config.farMaxHeightNoHitMaskEnabled) {
        result = CreateFarMaxHeightNoHitMaskPipeline(device.GetDevice());
        if (!result) {
            return Error("Failed to create far max-height no-hit mask pipeline: {}", result.error());
        }
    }
    if (m_config.backgroundPassHorizonTileMask) {
        result = CreateBackgroundHorizonTileMaskPipeline(device.GetDevice());
        if (!result) {
            return Error("Failed to create background horizon tile mask pipeline: {}", result.error());
        }
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
    for (auto& upload : m_farHeightfieldConstantUploads) {
        upload.Shutdown();
    }
    for (auto& upload : m_overlayConstantUploads) {
        upload.Shutdown();
    }
    m_dummyRenderOwnershipUAV.Shutdown();
    m_farHeightfieldFaces.Shutdown();
    m_farMaxHeightCache.Shutdown();
    m_farMaxHeightCacheValid = false;
    m_farMaxHeightCacheKey = {};
    m_farHeightfieldFaceReadback.Shutdown();
    m_farHeightfieldFaceUpload.Shutdown();
    m_farHeightfieldVertexIdUpload.Shutdown();
    m_farHeightfieldIndexUpload.Shutdown();
    m_farHeightfieldVertexIdView = {};
    m_farHeightfieldIndexView = {};
    m_farHeightfieldGeneratePipeline.Shutdown();
    m_farMaxHeightCacheGeneratePipeline.Shutdown();
    m_farMaxHeightNoHitMaskPipeline.Shutdown();
    m_backgroundHorizonTileMaskPipeline.Shutdown();
    m_farSkyOwnerPipeline.Shutdown();
    DestroyBackgroundPassResources();
    DestroyMidPassResources();
    DestroyFarMaxHeightNoHitMaskResources();

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
    if (!m_config.backgroundPassEnabled ||
        !std::isfinite(m_config.backgroundPassScale) ||
        m_config.backgroundPassScale <= 0.0f) {
        return false;
    }
    // Temporal mode runs the split full-res (scale ~1.0) and shares the main depth-stencil; the
    // legacy downscale path only activates below native (scale < 0.999).
    if (m_config.backgroundPassTemporal) {
        return true;
    }
    return m_config.backgroundPassScale < 0.999f;
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
    ++m_farHeightfieldFrameSerial;
    TryRetireFarHeightfieldOwnerFaceCsv();
    TryRetireFarMaxHeightHorizonCsv();

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
    const SparseNearField* sparseNearField,
    ID3D12QueryHeap* gpuTimingHeap,
    uint32_t gpuTimingBaseQuery,
    uint32_t farMaxHeightCacheEndQueryOffset,
    uint32_t farMaxHeightNoHitMaskEndQueryOffset,
    uint32_t farSkyOwnerBeginQueryOffset,
    uint32_t farSkyOwnerEndQueryOffset,
    uint32_t backgroundCoreEndQueryOffset)
{
    if (!cmdList) return;

    const auto markOptionalGpuTiming = [&](uint32_t queryOffset) {
        if (gpuTimingHeap && queryOffset != 0xFFFFFFFFu) {
            cmdList->EndQuery(
                gpuTimingHeap,
                D3D12_QUERY_TYPE_TIMESTAMP,
                gpuTimingBaseQuery + queryOffset);
        }
    };

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
    m_backgroundPassSurfaceRaymarchFillLastFrame = backgroundPassSurfaceRaymarchFillThisFrame;
    // Temporal full-res: render the raymarch into the separate bg color RT but bind the MAIN
    // depth-stencil (already carrying the mesh stencil written this frame) so early-stencil culls
    // the raymarch to background pixels only -- matching the default pass instead of marching the
    // whole screen. Requires scale 1.0 (bg RT == main RT size).
    const bool backgroundPassShareMainDepth =
        useBackgroundPassSplit && m_config.backgroundPassTemporal && m_dsvHandle.IsValid() &&
        m_backgroundPassWidth == m_width && m_backgroundPassHeight == m_height;
    // Temporal 3-MRT path: the raymarch writes color+distance+meta history (cur buffer) instead of the
    // single R8 bg color, then composites History[cur].color. Falls back to the 1b-i single-RT split if
    // resources/PSO aren't ready.
    const bool useTemporalPath =
        backgroundPassShareMainDepth &&
        m_temporalColor[0].resource && m_temporalColor[1].resource &&
        m_temporalDistance[0].resource && m_temporalMeta[0].resource &&
        m_temporalRaymarchPipeline.GetPSO() != nullptr &&
        m_temporalWidth == m_width && m_temporalHeight == m_height;
    const bool useTemporalMotionReproject = useTemporalPath && UseTemporalMotionReproject();
    // Static tile refresh keeps one persistent history target so discarded pixels retain their last
    // marched value. Motion reproject remains ping-pong because compute reads prev and writes cur.
    const uint32_t temporalCur = useTemporalMotionReproject
        ? static_cast<uint32_t>(m_temporalFrameCounter & 1ull)
        : 0u;
    const uint32_t temporalPrev = useTemporalMotionReproject
        ? static_cast<uint32_t>((m_temporalFrameCounter + 1ull) & 1ull)
        : 0u;
    const bool temporalReprojectReady =
        useTemporalMotionReproject && m_temporalReprojectDepthPipeline.IsValid() &&
        m_temporalReprojectColorPipeline.IsValid() && m_temporalMarchMask.resource &&
        m_temporalMeta[temporalCur].resource && m_temporalPrevCamValid;
    auto transitionTemporal = [&](TemporalTarget& t, D3D12_RESOURCE_STATES to) {
        if (t.resource && t.state != to) {
            D3D12_RESOURCE_BARRIER b =
                CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), t.state, to);
            cmdList->ResourceBarrier(1, &b);
            t.state = to;
        }
    };
    if (useTemporalPath) {
        if (useTemporalMotionReproject) {
            // Stage 2b: compute writes reuse pixels to [cur], then the raymarch fills mask=1 pixels.
            transitionTemporal(m_temporalColor[temporalCur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transitionTemporal(m_temporalColor[temporalPrev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transitionTemporal(m_temporalMarchMask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transitionTemporal(m_temporalMeta[temporalCur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            // Stage 2a: same target persists across frames. No clear; discarded tiles keep history.
            transitionTemporal(m_temporalColor[temporalCur], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        SetViewportAndScissor(cmdList, m_temporalWidth, m_temporalHeight);
    } else if (useBackgroundPassSplit) {
        if (m_backgroundPassColorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                m_backgroundPassColor.Get(),
                m_backgroundPassColorState,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList->ResourceBarrier(1, &barrier);
            m_backgroundPassColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE backgroundRtv = m_backgroundPassRtv.cpu;
        D3D12_CPU_DESCRIPTOR_HANDLE backgroundDsv =
            backgroundPassShareMainDepth ? m_dsvHandle.cpu : m_backgroundPassDsv.cpu;
        cmdList->OMSetRenderTargets(1, &backgroundRtv, FALSE, &backgroundDsv);
        SetViewportAndScissor(cmdList, m_backgroundPassWidth, m_backgroundPassHeight);
        const float defaultClearColor[] = { 0.42f, 0.55f, 0.74f, 1.0f };
        const float probeClearColor[] = { 1.0f, 0.0f, 1.0f, 1.0f };
        const float forceClearColor[] = { 0.0f, 0.95f, 0.28f, 1.0f };
        const float* clearColor = forceBackgroundPassColor
            ? forceClearColor
            : (m_config.backgroundPassClearProbe ? probeClearColor : defaultClearColor);
        cmdList->ClearRenderTargetView(backgroundRtv, clearColor, 0, nullptr);
        // When sharing the main depth-stencil, do NOT clear it -- that stencil holds the mesh
        // ownership the raymarch early-tests against; clearing would re-march the whole screen.
        if (!backgroundPassShareMainDepth) {
            cmdList->ClearDepthStencilView(
                backgroundDsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                1.0f,
                0,
                0,
                nullptr);
        }
    }

    // Bind the fullscreen raymarch pipeline. For the temporal path the bind is DEFERRED to after the
    // compute reproject dispatch below (the compute binds its own PSO/root sig, so the graphics PSO +
    // CBV are re-bound post-compute, just before the SRV tables + draw).
    if (!useTemporalPath) {
        m_fullscreenPipeline.Bind(cmdList);
    }
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
    constants.surfaceRasterParams[3] =
        m_config.farSkyOwnerEnabled
            ? ClampFinite(m_config.farSkyOwnerMinY, -0.20f, 0.42f, 0.06f)
            : (m_config.raymarchAggressiveSkyPso
                ? ClampFinite(m_config.raymarchAggressiveSkyMinY, -0.20f, 0.42f, 0.06f)
                : 0.0f);
    if (m_config.farMaxHeightCacheEnabled && m_farMaxHeightCache.GetResource() != nullptr) {
        float cacheOriginX = 0.0f;
        float cacheOriginZ = 0.0f;
        ComputeFarMaxHeightCacheOrigin(cameraPosX, cameraPosZ, cacheOriginX, cacheOriginZ);
        constants.farMaxHeightCacheParams[0] = 1.0f;
        constants.farMaxHeightCacheParams[1] = cacheOriginX;
        constants.farMaxHeightCacheParams[2] = cacheOriginZ;
        constants.farMaxHeightCacheParams[3] = kFarMaxHeightCacheCellSize;
        constants.farMaxHeightCacheParams2[0] = static_cast<float>(kFarMaxHeightCacheLeafCellCount);
        constants.farMaxHeightCacheParams2[1] = static_cast<float>(kFarMaxHeightCacheMipLevels);
        constants.farMaxHeightCacheParams2[2] = kFarMaxHeightCacheHeightPad;
        const bool farHorizonReady =
            m_config.farMaxHeightNoHitMaskEnabled &&
            m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().IsValid();
        constants.farMaxHeightCacheParams2[3] =
            farHorizonReady
                ? ((m_config.farSkyOwnerEnabled && m_config.farSkyOwnerHorizonOnly) ? 2.0f : 1.0f)
                : 0.0f;
    }

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

    // Temporal Stage 2a: detect a (near-)static camera. Same-UV tile reuse is only valid when both
    // position and orientation barely changed; under motion we mark cameraStatic=0 so the temporal PS
    // marches every background pixel (no stale reuse => no holes/ghosting). cameraPosition/Forward are
    // already filled above.
    bool temporalCameraStatic = false;
    if (useTemporalPath) {
        const float dpx = constants.cameraPosition[0] - m_temporalLastCamPos[0];
        const float dpy = constants.cameraPosition[1] - m_temporalLastCamPos[1];
        const float dpz = constants.cameraPosition[2] - m_temporalLastCamPos[2];
        const float posMove2 = dpx * dpx + dpy * dpy + dpz * dpz;
        const float fdot = constants.cameraForward[0] * m_temporalLastCamFwd[0] +
                           constants.cameraForward[1] * m_temporalLastCamFwd[1] +
                           constants.cameraForward[2] * m_temporalLastCamFwd[2];
        // <0.1u movement AND <~0.57deg rotation since last frame.
        temporalCameraStatic = m_temporalLastCamValid && posMove2 < 0.01f && fdot > 0.99995f;

        // Stage 2b: publish the PREVIOUS frame's full camera basis (before overwriting it) so the
        // compute reproject can map prev world hits into this frame. First frame: prev = current
        // (zero motion => reproject identity, safe).
        const bool havePrev = m_temporalPrevCamValid;
        for (int i = 0; i < 3; ++i) {
            constants.prevCameraPosition[i] = havePrev ? m_temporalLastCamPos[i] : constants.cameraPosition[i];
            constants.prevCameraForward[i]  = havePrev ? m_temporalLastCamFwd[i] : constants.cameraForward[i];
            constants.prevCameraRight[i]    = havePrev ? m_temporalLastCamRight[i] : constants.cameraRight[i];
            constants.prevCameraUp[i]       = havePrev ? m_temporalLastCamUp[i] : constants.cameraUp[i];
        }
        constants.prevCameraPosition[3] = havePrev ? m_temporalLastFov : constants.cameraPosition[3];
        constants.prevCameraForward[3]  = havePrev ? m_temporalLastAspect : constants.cameraForward[3];
        constants.prevCameraRight[3] = 0.0f;
        constants.prevCameraUp[3] = 0.0f;

        m_temporalLastCamPos[0] = constants.cameraPosition[0];
        m_temporalLastCamPos[1] = constants.cameraPosition[1];
        m_temporalLastCamPos[2] = constants.cameraPosition[2];
        m_temporalLastCamFwd[0] = constants.cameraForward[0];
        m_temporalLastCamFwd[1] = constants.cameraForward[1];
        m_temporalLastCamFwd[2] = constants.cameraForward[2];
        m_temporalLastCamRight[0] = constants.cameraRight[0];
        m_temporalLastCamRight[1] = constants.cameraRight[1];
        m_temporalLastCamRight[2] = constants.cameraRight[2];
        m_temporalLastCamUp[0] = constants.cameraUp[0];
        m_temporalLastCamUp[1] = constants.cameraUp[1];
        m_temporalLastCamUp[2] = constants.cameraUp[2];
        m_temporalLastFov = constants.cameraPosition[3];
        m_temporalLastAspect = constants.cameraForward[3];
        m_temporalLastCamValid = true;
        m_temporalPrevCamValid = true;
    }
    constants.temporalParams[0] = useTemporalPath ? 1.0f : 0.0f;
    constants.temporalParams[1] = temporalCameraStatic ? 1.0f : 0.0f;
    constants.temporalParams[2] = static_cast<float>(
        std::clamp(
            m_config.raymarchMaskedBandDiag && !useTemporalPath
                ? m_config.raymarchMaskedBandDiagPhases
                : m_config.backgroundPassTemporalStaticPhases,
            1u,
            16u));
    constants.temporalParams[3] = static_cast<float>(
        std::clamp(
            m_config.raymarchMaskedBandDiag && !useTemporalPath
                ? m_config.raymarchMaskedBandDiagTileSize
                : m_config.backgroundPassTemporalTileSize,
            4u,
            64u));

    static_assert(sizeof(constants) <= kFrameConstantUploadBytes);
    UploadBuffer& frameConstantsUpload = m_frameConstantUploads[m_currentFrameIndex];
    if (void* mapped = frameConstantsUpload.GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }
    cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());

    GenerateFarMaxHeightCache(cmdList, &constants);
    markOptionalGpuTiming(farMaxHeightCacheEndQueryOffset);
    GenerateFarMaxHeightNoHitMask(cmdList, frameConstantsUpload.GetGPUVirtualAddress());
    markOptionalGpuTiming(farMaxHeightNoHitMaskEndQueryOffset);

    if (!useTemporalPath) {
        // The far max-height/no-hit products are generated with compute PSOs.
        // Re-establish the fullscreen graphics PSO before binding graphics root
        // descriptors and issuing the background draw; otherwise the no-hit path
        // can leave the clear color in the background target and record no
        // ownership samples.
        m_fullscreenPipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    }

    const bool farHeightfieldOwnerReady =
        m_config.farHeightfieldOwnerEnabled &&
        m_sparseSurfacePipeline.GetPSO() != nullptr &&
        m_farHeightfieldFaces.GetResource() != nullptr &&
        m_farHeightfieldFaceUpload.GetResource() != nullptr &&
        m_farHeightfieldVertexIdView.BufferLocation != 0u &&
        m_farHeightfieldIndexView.BufferLocation != 0u &&
        materialPaletteSRV.IsValid() &&
        (!useBackgroundPassSplit || backgroundPassShareMainDepth);
    if (farHeightfieldOwnerReady) {
        DrawFarHeightfieldOwner(
            cmdList,
            frameConstantsUpload.GetGPUVirtualAddress(),
            &constants,
            materialPaletteSRV);

        if (!useTemporalPath) {
            if (useBackgroundPassSplit) {
                D3D12_CPU_DESCRIPTOR_HANDLE backgroundRtv = m_backgroundPassRtv.cpu;
                D3D12_CPU_DESCRIPTOR_HANDLE backgroundDsv =
                    backgroundPassShareMainDepth ? m_dsvHandle.cpu : m_backgroundPassDsv.cpu;
                cmdList->OMSetRenderTargets(1, &backgroundRtv, FALSE, &backgroundDsv);
                SetViewportAndScissor(cmdList, m_backgroundPassWidth, m_backgroundPassHeight);
            } else {
                SetMainRenderTarget(cmdList);
            }
            m_fullscreenPipeline.Bind(cmdList);
            cmdList->OMSetStencilRef(0);
            cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
        }
    }

    const bool farSkyOwnerReady =
        m_config.farSkyOwnerEnabled &&
        (!useBackgroundPassSplit || backgroundPassShareMainDepth) &&
        m_farSkyOwnerPipeline.GetPSO() != nullptr &&
        m_farSkyOwnerPipeline.GetRootSignature() != nullptr;
    markOptionalGpuTiming(farSkyOwnerBeginQueryOffset);
    if (farSkyOwnerReady) {
        SetMainRenderTarget(cmdList);
        SetViewportAndScissor(cmdList, m_width, m_height);
        m_farSkyOwnerPipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(
            1,
            m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().IsValid()
                ? m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().gpu
                : voxelGridSRV.gpu);
        cmdList->DrawInstanced(3, 1, 0, 0);

        m_fullscreenPipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    }
    markOptionalGpuTiming(farSkyOwnerEndQueryOffset);

    if (useTemporalMotionReproject) {
        // ---- Stage 2b: temporal motion reprojection (compute), then re-bind the raymarch state ----
        // Clear the march mask to 1 (march every pixel) -- this is the hole-safe default, so the first
        // frame (no prev camera) and any pixel the reproject doesn't confidently reuse get marched.
        const UINT maskClear[4] = { 1u, 1u, 1u, 1u };
        cmdList->ClearUnorderedAccessViewUint(
            m_temporalMarchMask.uav.gpu, m_temporalMarchMask.stagingUav.cpu,
            m_temporalMarchMask.resource.Get(), maskClear, 0, nullptr);
        // ScatterDepth (reuses m_temporalMeta[cur]) cleared to 0xFFFFFFFF = empty (nearest-wins).
        const UINT depthClear[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        if (temporalReprojectReady) {
            cmdList->ClearUnorderedAccessViewUint(
                m_temporalMeta[temporalCur].uav.gpu, m_temporalMeta[temporalCur].stagingUav.cpu,
                m_temporalMeta[temporalCur].resource.Get(), depthClear, 0, nullptr);
        }
        D3D12_RESOURCE_BARRIER clearBarriers[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_temporalMarchMask.resource.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_temporalMeta[temporalCur].resource.Get()) };
        cmdList->ResourceBarrier(temporalReprojectReady ? 2u : 1u, clearBarriers);

        if (temporalReprojectReady) {
            const uint32_t gx = (m_temporalWidth + 7u) / 8u;
            const uint32_t gy = (m_temporalHeight + 7u) / 8u;
            const auto cbv = frameConstantsUpload.GetGPUVirtualAddress();
            // Pass 1 (depth): InterlockedMin the nearest reprojected distance per current pixel.
            m_temporalReprojectDepthPipeline.Bind(cmdList);
            cmdList->SetComputeRootConstantBufferView(0, cbv);
            cmdList->SetComputeRootDescriptorTable(1, m_temporalColor[temporalPrev].srv.gpu);
            cmdList->SetComputeRootDescriptorTable(2, m_temporalMeta[temporalCur].uav.gpu);  // u0 ScatterDepth
            m_temporalReprojectDepthPipeline.Dispatch(cmdList, gx, gy, 1u);
            D3D12_RESOURCE_BARRIER depthDone =
                CD3DX12_RESOURCE_BARRIER::UAV(m_temporalMeta[temporalCur].resource.Get());
            cmdList->ResourceBarrier(1, &depthDone);
            // Pass 2 (color): the winning prev sample writes color into [cur] + clears its march bit.
            m_temporalReprojectColorPipeline.Bind(cmdList);
            cmdList->SetComputeRootConstantBufferView(0, cbv);
            cmdList->SetComputeRootDescriptorTable(1, m_temporalColor[temporalPrev].srv.gpu);
            cmdList->SetComputeRootDescriptorTable(2, m_temporalMeta[temporalCur].uav.gpu);   // u0 ScatterDepth (read)
            cmdList->SetComputeRootDescriptorTable(3, m_temporalColor[temporalCur].uav.gpu);  // u1 HistoryCur
            cmdList->SetComputeRootDescriptorTable(4, m_temporalMarchMask.uav.gpu);           // u2 MarchMask
            m_temporalReprojectColorPipeline.Dispatch(cmdList, gx, gy, 1u);
        }

        // Make the compute's [cur] + mask writes visible, then transition for the raymarch: [cur] -> RTV
        // (march pixels), mask -> SRV (the PS mask-gate at t18 / table 20).
        D3D12_RESOURCE_BARRIER postCompute[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_temporalColor[temporalCur].resource.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_temporalMarchMask.resource.Get()) };
        cmdList->ResourceBarrier(2, postCompute);
        transitionTemporal(m_temporalColor[temporalCur], D3D12_RESOURCE_STATE_RENDER_TARGET);
        transitionTemporal(m_temporalMarchMask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (useTemporalPath) {
        // Establish the temporal raymarch state. In motion mode this rebinds graphics after compute;
        // in static mode it is the first temporal graphics bind.
        ID3D12DescriptorHeap* graphicsHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, graphicsHeaps);
        D3D12_CPU_DESCRIPTOR_HANDLE tcRtv = m_temporalColor[temporalCur].rtv.cpu;
        D3D12_CPU_DESCRIPTOR_HANDLE tcDsv = m_dsvHandle.cpu;
        cmdList->OMSetRenderTargets(1, &tcRtv, FALSE, &tcDsv);
        SetViewportAndScissor(cmdList, m_temporalWidth, m_temporalHeight);
        m_temporalRaymarchPipeline.Bind(cmdList);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
    }

    const uint32_t sparseBindingMask = sparseNearField ? sparseNearField->bindingMask : 0u;
    auto bindRaymarchRootDescriptors = [&]() {
        cmdList->SetGraphicsRootConstantBufferView(0, frameConstantsUpload.GetGPUVirtualAddress());
        // Use persistent shader-visible descriptors directly (no per-frame copy needed)
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
        cmdList->SetGraphicsRootDescriptorTable(
            20,
            (m_config.farMaxHeightCacheEnabled && m_farMaxHeightCache.GetShaderVisibleSRV().IsValid())
                ? m_farMaxHeightCache.GetShaderVisibleSRV().gpu
                : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            21,
            m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().IsValid()
                ? m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().gpu
                : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            22,
            m_backgroundHorizonTileMask.srv.IsValid()
                ? m_backgroundHorizonTileMask.srv.gpu
                : (m_backgroundPassSrv.IsValid() ? m_backgroundPassSrv.gpu : voxelGridSRV.gpu));
    if (useTemporalMotionReproject) {
        // t18 (table 23): the reproject march/reuse mask the temporal PS samples at top-of-main.
        cmdList->SetGraphicsRootDescriptorTable(23, m_temporalMarchMask.srv.gpu);
    }
    };
    bindRaymarchRootDescriptors();

    // Draw fullscreen triangle. The force-color probe intentionally leaves the
    // lower-resolution target at a known clear color to test RTV/SRV/composite
    // plumbing without touching PS_Raymarch.
    if (!forceBackgroundPassColor) {
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
    markOptionalGpuTiming(backgroundCoreEndQueryOffset);

    if (useTemporalPath) {
        // Composite History[cur].color -> main RT (stencil EQUAL 0, same path as the bg split). The
        // distance/meta MRTs stay as RTV history for next frame's reproject (Stage 2 reads them as SRV).
        transitionTemporal(m_temporalColor[temporalCur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        SetMainRenderTarget(cmdList);
        m_backgroundCompositePipeline.Bind(cmdList);
        ID3D12DescriptorHeap* compositeHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, compositeHeaps);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootDescriptorTable(0, m_temporalColor[temporalCur].srv.gpu);
        cmdList->DrawInstanced(3, 1, 0, 0);
        ++m_temporalFrameCounter;
    } else if (useBackgroundPassSplit) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_backgroundPassColor.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (renderOwnershipEnabled ||
            (m_config.backgroundPassHorizonRepair && m_config.backgroundPassHorizonTileMask)) {
            GenerateBackgroundHorizonTileMask(
                cmdList,
                sparseNearField,
                camera.frameIndex);
        }

        SetMainRenderTarget(cmdList);
        m_backgroundCompositePipeline.Bind(cmdList);
        ID3D12DescriptorHeap* compositeHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
        cmdList->SetDescriptorHeaps(1, compositeHeaps);
        cmdList->OMSetStencilRef(0);
        cmdList->SetGraphicsRootDescriptorTable(0, m_backgroundPassSrv.gpu);
        cmdList->DrawInstanced(3, 1, 0, 0);

        const uint32_t repairY0 = std::min(m_height, m_config.backgroundPassHorizonRepairY0);
        const uint32_t repairY1 = std::min(m_height, m_config.backgroundPassHorizonRepairY1);
        const bool useTileRepair =
            m_config.backgroundPassHorizonRepair &&
            m_config.backgroundPassHorizonTileMask &&
            m_backgroundHorizonTileMask.srv.IsValid();
        if (m_config.backgroundPassHorizonRepair &&
            repairY0 < repairY1 &&
            (!m_config.backgroundPassHorizonTileMask || useTileRepair)) {
            m_fullscreenPipeline.Bind(cmdList);
            ID3D12DescriptorHeap* repairHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
            cmdList->SetDescriptorHeaps(1, repairHeaps);
            cmdList->OMSetStencilRef(0);
            bindRaymarchRootDescriptors();
            cmdList->SetGraphicsRootDescriptorTable(21, m_backgroundPassSrv.gpu);

            D3D12_VIEWPORT repairViewport = {};
            repairViewport.Width = static_cast<float>(std::max(1u, m_width));
            repairViewport.Height = static_cast<float>(std::max(1u, m_height));
            repairViewport.MinDepth = 0.0f;
            repairViewport.MaxDepth = 1.0f;
            cmdList->RSSetViewports(1, &repairViewport);
            D3D12_RECT repairScissor = {};
            repairScissor.left = 0;
            repairScissor.top = static_cast<LONG>(
                useTileRepair ? std::max(repairY0, m_backgroundPassHeight) : repairY0);
            repairScissor.right = static_cast<LONG>(m_width);
            repairScissor.bottom = static_cast<LONG>(repairY1);
            cmdList->RSSetScissorRects(1, &repairScissor);
            cmdList->DrawInstanced(3, 1, 0, 0);
            SetViewportAndScissor(cmdList, m_width, m_height);
        }
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
        cmdList->SetGraphicsRootDescriptorTable(
            20,
            (m_config.farMaxHeightCacheEnabled && m_farMaxHeightCache.GetShaderVisibleSRV().IsValid())
                ? m_farMaxHeightCache.GetShaderVisibleSRV().gpu
                : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            21,
            m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().IsValid()
                ? m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().gpu
                : voxelGridSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(
            22,
            m_backgroundHorizonTileMask.srv.IsValid()
                ? m_backgroundHorizonTileMask.srv.gpu
                : (m_backgroundPassSrv.IsValid() ? m_backgroundPassSrv.gpu : voxelGridSRV.gpu));

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
    constants.surfaceRasterParams[3] =
        m_config.farSkyOwnerEnabled
            ? ClampFinite(m_config.farSkyOwnerMinY, -0.20f, 0.42f, 0.06f)
            : (m_config.raymarchAggressiveSkyPso
                ? ClampFinite(m_config.raymarchAggressiveSkyMinY, -0.20f, 0.42f, 0.06f)
                : 0.0f);
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
    if (UseTemporalReproject()) {
        result = CreateTemporalResources();
        if (!result) {
            return result;
        }
    }
    result = CreateFarMaxHeightNoHitMaskResources();
    if (!result) {
        return result;
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

    spdlog::info("RENDERER_INIT_STAGE begin CreateFullscreenPipeline");
    auto vsResult = m_shaderCompiler.CompileVertexShader(vsPath, L"main", m_config.debugShaders);
    if (!vsResult) {
        return Error("Failed to compile vertex shader: {}", vsResult.error());
    }
    m_fullscreenVS = vsResult.value();
    if (!m_fullscreenVS.IsValid()) {
        return Error("Vertex shader compilation failed: {}", m_fullscreenVS.errors);
    }
    spdlog::info("RENDERER_INIT_STAGE fullscreen VS ready bytes={}", m_fullscreenVS.bytecode.size());

    const bool useBackgroundOnlyFullscreenShader =
        m_config.raymarchBackgroundOnlyPso || m_config.backgroundPassTemporal;
    spdlog::info(
        "RENDERER_INIT_STAGE begin fullscreen PS compile backgroundOnly={} temporal={} debugShaders={}",
        useBackgroundOnlyFullscreenShader ? 1 : 0,
        m_config.backgroundPassTemporal ? 1 : 0,
        m_config.debugShaders ? 1 : 0);
    auto psResult = [&]() {
        if (useBackgroundOnlyFullscreenShader) {
            ShaderCompileOptions psOptions;
            psOptions.entryPoint = L"main";
            psOptions.target = L"ps_6_0";
            psOptions.debugInfo = false;
            psOptions.optimizationLevel3 = true;
            psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_ONLY=1");
            if (m_config.raymarchBackgroundOnlyPso) {
                psOptions.defines.push_back(L"RAYMARCH_FAR_TERRAIN_WORK_STATS=1");
            }
            if (m_config.farMaxHeightNoHitMaskEnabled) {
                psOptions.defines.push_back(L"RAYMARCH_FAR_MAX_HEIGHT_NO_HIT_MASK=1");
            }
            if (m_config.raymarchFarMaxHeightDdaEnabled) {
                psOptions.defines.push_back(L"RAYMARCH_FAR_MAX_HEIGHT_DDA=1");
            }
            if (m_config.raymarchFastSkyPso) {
                psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_FAST_SKY=1");
            }
            if (m_config.raymarchAggressiveSkyPso) {
                psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_AGGRESSIVE_SKY=1");
            }
            if (m_config.raymarchMaskedBandDiag) {
                psOptions.defines.push_back(L"RAYMARCH_MASKED_BAND_DIAG=1");
            }
            if (m_config.backgroundPassHorizonRepair) {
                psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_EDGE_REPAIR=1");
                if (m_config.backgroundPassHorizonTileMask) {
                    psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_TILE_REPAIR=1");
                    psOptions.defines.push_back(
                        L"RAYMARCH_BACKGROUND_TILE_REPAIR_SIZE=" +
                        std::to_wstring(std::max(1u, m_config.backgroundPassHorizonTileSize)));
                }
            }
            if (m_config.raymarchFastTerrainDiagnostics) {
                psOptions.defines.push_back(L"RAYMARCH_FAST_TERRAIN_DIAGNOSTICS=1");
            }
            if (m_config.raymarchProbeSkipWater) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_WATER=1");
            }
            if (m_config.raymarchProbeSkipMidDda) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_MID_DDA=1");
            }
            if (m_config.raymarchProbeSkipFarSvo) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_FAR_SVO=1");
            }
            if (m_config.raymarchProbeSkipFarHeight) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_FAR_HEIGHT=1");
            }
            if (m_config.raymarchProbeSkipFarTail) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_FAR_TAIL=1");
            }
            if (m_config.raymarchProbeSkipTerrainDiag) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_TERRAIN_DIAG=1");
            }
            if (m_config.raymarchProbeSkipClosureDiag) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_CLOSURE_DIAG=1");
            }
            if (m_config.raymarchProbeSkipMissDiag) {
                psOptions.defines.push_back(L"RAYMARCH_PROBE_SKIP_MISS_DIAG=1");
            }
            return m_shaderCompiler.CompileFromFile(psPath, psOptions);
        }
        return m_shaderCompiler.CompilePixelShader(psPath, L"main", m_config.debugShaders);
    }();
    if (!psResult) {
        return Error("Failed to compile pixel shader: {}", psResult.error());
    }
    m_fullscreenPS = psResult.value();
    if (!m_fullscreenPS.IsValid()) {
        return Error("Pixel shader compilation failed: {}", m_fullscreenPS.errors);
    }
    spdlog::info("RENDERER_INIT_STAGE fullscreen PS ready bytes={}", m_fullscreenPS.bytecode.size());
    if (m_config.raymarchBackgroundOnlyPso) {
        spdlog::info(
            "Raymarch background-only PSO specialization: fastSky={} aggressiveSky={} maskedBandDiag={} maskedBandTile={} maskedBandPhases={} fastTerrainDiag={} farMaxHeightDda={} probeSkipWater={} probeSkipMidDda={} probeSkipFarSvo={} probeSkipFarHeight={} probeSkipFarTail={} probeSkipTerrainDiag={} probeSkipClosureDiag={} probeSkipMissDiag={}",
            m_config.raymarchFastSkyPso ? 1 : 0,
            m_config.raymarchAggressiveSkyPso ? 1 : 0,
            m_config.raymarchMaskedBandDiag ? 1 : 0,
            std::clamp(m_config.raymarchMaskedBandDiagTileSize, 4u, 64u),
            std::clamp(m_config.raymarchMaskedBandDiagPhases, 1u, 16u),
            m_config.raymarchFastTerrainDiagnostics ? 1 : 0,
            m_config.raymarchFarMaxHeightDdaEnabled ? 1 : 0,
            m_config.raymarchProbeSkipWater ? 1 : 0,
            m_config.raymarchProbeSkipMidDda ? 1 : 0,
            m_config.raymarchProbeSkipFarSvo ? 1 : 0,
            m_config.raymarchProbeSkipFarHeight ? 1 : 0,
            m_config.raymarchProbeSkipFarTail ? 1 : 0,
            m_config.raymarchProbeSkipTerrainDiag ? 1 : 0,
            m_config.raymarchProbeSkipClosureDiag ? 1 : 0,
            m_config.raymarchProbeSkipMissDiag ? 1 : 0);
    }

    // Create pipeline
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_fullscreenVS;
    pipelineDesc.pixelShader = m_fullscreenPS;
    pipelineDesc.debugName =
        m_config.raymarchBackgroundOnlyPso ? "FullscreenBackgroundOnlyPipeline" : "FullscreenPipeline";

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
    // t19: Reserved for the conservative far max-height cache consumer. The
    // initial GPU product is generated behind an env flag; the rejected
    // uber-shader consumer is intentionally not compiled.
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        19,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t20: Optional far-height horizon. A compute pass writes one conservative
    // screen-space horizon Y per 8px X tile; pixels above it can skip only the
    // final far-height tail. The pixel shader still runs all earlier background
    // owners.
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        20,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t21: Optional background horizon repair tile mask. Generated from the
    // low-res background pass and consumed only by the experimental repair PSO.
    pipelineDesc.rootParams.push_back({
        RootParamType::DescriptorTable,
        21,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
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

    spdlog::info(
        "RENDERER_INIT_STAGE begin fullscreen PSO create rootParams={} rtvFormats={} dsvFormat={}",
        pipelineDesc.rootParams.size(),
        pipelineDesc.rtvFormats.size(),
        static_cast<uint32_t>(pipelineDesc.dsvFormat));
    auto result = m_fullscreenPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create fullscreen pipeline: {}", result.error());
    }

    spdlog::info("RENDERER_INIT_STAGE fullscreen PSO ready");
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

    if (m_config.backgroundPassTemporal) {
        auto temporalResult = CreateTemporalRaymarchPipeline(device, pipelineDesc);
        if (!temporalResult) {
            return Error("Failed to create temporal raymarch pipeline: {}", temporalResult.error());
        }
    }

    if (m_config.farSkyOwnerEnabled) {
        auto farSkyOwnerResult = CreateFarSkyOwnerPipeline(device);
        if (!farSkyOwnerResult) {
            return Error("Failed to create far sky owner pipeline: {}", farSkyOwnerResult.error());
        }
    }

    return {};
}

Result<void> Renderer::CreateFarSkyOwnerPipeline(ID3D12Device* device) {
    if (!device) {
        return Error("Far sky owner pipeline: null device");
    }

    const std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_FarSkyOwner.hlsl";
    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile far sky owner pixel shader: {}", psResult.error());
    }
    m_farSkyOwnerPS = psResult.value();
    if (!m_farSkyOwnerPS.IsValid()) {
        return Error("Far sky owner pixel shader compilation failed: {}", m_farSkyOwnerPS.errors);
    }

    GraphicsPipelineDesc desc;
    desc.vertexShader = m_fullscreenVS;
    desc.pixelShader = m_farSkyOwnerPS;
    desc.debugName = "FarSkyOwnerPipeline";
    desc.rootParams.push_back({
        RootParamType::ConstantBuffer,
        0,
        0,
        D3D12_SHADER_VISIBILITY_ALL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        0
    });
    desc.rootParams.push_back({
        RootParamType::DescriptorTable,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    desc.rtvFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.inputLayout.clear();
    desc.depthEnable = false;
    desc.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.stencilEnable = true;
    desc.stencilReadMask = 0xFFu;
    desc.stencilWriteMask = 0xFFu;
    desc.frontStencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    desc.frontStencilPassOp = D3D12_STENCIL_OP_INCR_SAT;
    desc.frontStencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.frontStencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.cullMode = D3D12_CULL_MODE_NONE;

    auto result = m_farSkyOwnerPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create far sky owner pipeline: {}", result.error());
    }
    spdlog::info("Far sky owner pipeline created successfully");
    return {};
}

Result<void> Renderer::CreateTemporalRaymarchPipeline(
    ID3D12Device* device, GraphicsPipelineDesc fullscreenDesc) {
    // SINGLE-RT temporal history at RGBA16F. A 3-MRT variant tripped the uber-shader PSO JIT cliff
    // (E_OUTOFMEMORY); keeping ONE float4 SV_Target0 output (same shape as the working default) avoids
    // it. The default RAYMARCH_TEMPORAL variant adds only a cheap top-of-main tile-Bayer discard
    // (no extra SRV root table). The opt-in reproject variant samples TemporalMarchMask at t18.
    std::filesystem::path psPath = m_config.shaderPath / "Graphics" / "PS_Raymarch.hlsl";
    ShaderCompileOptions psOptions;
    psOptions.entryPoint = L"main";
    psOptions.target = L"ps_6_0";
    psOptions.debugInfo = m_config.debugShaders;
    psOptions.optimizationLevel3 = true;
    psOptions.defines.push_back(L"RAYMARCH_TEMPORAL=1");
    if (UseTemporalMotionReproject()) {
        psOptions.defines.push_back(L"RAYMARCH_TEMPORAL_REPROJECT=1");
    } else {
        // Static temporal renders only background pixels through the split pass. Keep this variant off
        // the full near/brush/avatar uber-shader so cold startup can compile and verify the path.
        psOptions.defines.push_back(L"RAYMARCH_BACKGROUND_ONLY=1");
    }
    auto psResult = m_shaderCompiler.CompileFromFile(psPath, psOptions);
    if (!psResult) {
        return Error("Failed to compile temporal raymarch pixel shader: {}", psResult.error());
    }
    m_temporalRaymarchPS = psResult.value();
    if (!m_temporalRaymarchPS.IsValid()) {
        return Error("Temporal raymarch pixel shader compilation failed: {}", m_temporalRaymarchPS.errors);
    }
    fullscreenDesc.vertexShader = m_fullscreenVS;
    fullscreenDesc.pixelShader = m_temporalRaymarchPS;
    fullscreenDesc.blendEnable = false;
    fullscreenDesc.rtvFormats.clear();
    fullscreenDesc.rtvFormats.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);  // RT0 history color (a = distance, Stage 2b)
    if (UseTemporalMotionReproject()) {
        // Stage 2b: t18 = TemporalMarchMask SRV. Index 23 after the default tables.
        fullscreenDesc.rootParams.push_back({
            RootParamType::DescriptorTable, 18, 0, D3D12_SHADER_VISIBILITY_PIXEL, 1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV });
    }
    fullscreenDesc.debugName = "TemporalRaymarchPipeline";

    auto result = m_temporalRaymarchPipeline.Initialize(device, fullscreenDesc);
    if (!result) {
        return Error("Failed to create temporal raymarch pipeline: {}", result.error());
    }
    spdlog::info("Temporal raymarch pipeline created successfully (single-RT RGBA16F)");

    if (UseTemporalMotionReproject()) {
        if (auto reproj = CreateTemporalReprojectPipeline(device); !reproj) {
            return reproj;
        }
    }
    return {};
}

Result<void> Renderer::CreateTemporalReprojectPipeline(ID3D12Device* device) {
    // Stage 2b: 2-pass depth-resolved forward-scatter (CS_TemporalReproject). Pass 1 InterlockedMins the
    // nearest reprojected distance per current pixel (overlap/disocclusion resolve); pass 2 writes the
    // winning prev color + the reuse mask. Same source, compiled twice via REPROJECT_COLOR_PASS.
    std::filesystem::path csPath = m_config.shaderPath / "Compute" / "CS_TemporalReproject.hlsl";

    // Pass 1 (depth): b0 FrameConstants, t0 HistoryPrev, u0 ScatterDepth.
    {
        ShaderCompileOptions o;
        o.entryPoint = L"main";
        o.target = L"cs_6_0";
        o.debugInfo = m_config.debugShaders;
        o.optimizationLevel3 = true;
        auto cs = m_shaderCompiler.CompileFromFile(csPath, o);
        if (!cs) return Error("Failed to compile temporal reproject DEPTH shader: {}", cs.error());
        m_temporalReprojectDepthCS = cs.value();
        if (!m_temporalReprojectDepthCS.IsValid()) {
            return Error("Temporal reproject DEPTH shader compile failed: {}", m_temporalReprojectDepthCS.errors);
        }
        ComputePipelineDesc desc;
        desc.computeShader = m_temporalReprojectDepthCS;
        desc.debugName = "TemporalReprojectDepthPipeline";
        desc.rootParams.push_back({ RootParamType::ConstantBuffer, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // b0
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // t0 HistoryPrev
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u0 ScatterDepth
        if (auto r = m_temporalReprojectDepthPipeline.Initialize(device, desc); !r) {
            return Error("Failed to create temporal reproject depth pipeline: {}", r.error());
        }
    }
    // Pass 2 (color): b0, t0 HistoryPrev, u0 ScatterDepth, u1 HistoryCur, u2 MarchMask.
    {
        ShaderCompileOptions o;
        o.entryPoint = L"main";
        o.target = L"cs_6_0";
        o.debugInfo = m_config.debugShaders;
        o.optimizationLevel3 = true;
        o.defines.push_back(L"REPROJECT_COLOR_PASS=1");
        auto cs = m_shaderCompiler.CompileFromFile(csPath, o);
        if (!cs) return Error("Failed to compile temporal reproject COLOR shader: {}", cs.error());
        m_temporalReprojectColorCS = cs.value();
        if (!m_temporalReprojectColorCS.IsValid()) {
            return Error("Temporal reproject COLOR shader compile failed: {}", m_temporalReprojectColorCS.errors);
        }
        ComputePipelineDesc desc;
        desc.computeShader = m_temporalReprojectColorCS;
        desc.debugName = "TemporalReprojectColorPipeline";
        desc.rootParams.push_back({ RootParamType::ConstantBuffer, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // b0
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // t0 HistoryPrev
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u0 ScatterDepth
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u1 HistoryCur
        desc.rootParams.push_back({ RootParamType::DescriptorTable, 2, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u2 MarchMask
        if (auto r = m_temporalReprojectColorPipeline.Initialize(device, desc); !r) {
            return Error("Failed to create temporal reproject color pipeline: {}", r.error());
        }
    }
    spdlog::info("Temporal reproject compute pipelines created successfully (2-pass depth-resolved)");
    return {};
}

Result<void> Renderer::CreateFarHeightfieldOwnerResources(ID3D12Device* device) {
    if (!device) {
        return Error("Far heightfield owner: null device");
    }

    spdlog::info("Far heightfield owner init: creating face buffer");
    auto bufferResult = m_farHeightfieldFaces.Initialize(
        device,
        kFarHeightfieldFaceBufferBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        kFarHeightfieldFaceStride,
        "FarHeightfieldFaces");
    if (!bufferResult) {
        return Error("Far heightfield owner face buffer init failed: {}", bufferResult.error());
    }
    if (auto srv = m_farHeightfieldFaces.CreateSRV(device, m_heapManager); !srv) {
        return Error("Far heightfield owner face SRV init failed: {}", srv.error());
    }
    if (auto uav = m_farHeightfieldFaces.CreateUAV(device, m_heapManager); !uav) {
        return Error("Far heightfield owner face UAV init failed: {}", uav.error());
    }

    if (!m_config.farHeightfieldOwnerFaceCsvPath.empty()) {
        auto readbackResult = m_farHeightfieldFaceReadback.Initialize(
            device,
            kFarHeightfieldFaceBufferBytes,
            BufferUsage::Readback,
            kFarHeightfieldFaceStride,
            "FarHeightfieldFaceReadback");
        if (!readbackResult) {
            return Error("Far heightfield owner face readback init failed: {}", readbackResult.error());
        }
    }

    if (m_config.farHeightfieldOwnerGpuGenerate) {
        const std::filesystem::path csPath =
            m_config.shaderPath / "Compute" / "CS_GenerateFarHeightfieldFaces.hlsl";
        auto compileResult = m_shaderCompiler.CompileComputeShader(csPath, L"main", true);
        if (!compileResult) {
            return Error("Far heightfield owner generate CS compile failed: {}", compileResult.error());
        }
        m_farHeightfieldGenerateCS = compileResult.value();
        if (!m_farHeightfieldGenerateCS.IsValid()) {
            return Error(
                "Far heightfield owner generate CS invalid: {}",
                m_farHeightfieldGenerateCS.errors);
        }

        ComputePipelineDesc desc;
        desc.computeShader = m_farHeightfieldGenerateCS;
        desc.debugName = "CS_GenerateFarHeightfieldFaces";
        desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 12 });
        desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 });
        if (auto pipeResult = m_farHeightfieldGeneratePipeline.Initialize(device, desc); !pipeResult) {
            return Error("Far heightfield owner generate pipeline init failed: {}", pipeResult.error());
        }
    }

    auto uploadResult = m_farHeightfieldFaceUpload.Initialize(
        device,
        kFarHeightfieldFaceBufferBytes,
        "FarHeightfieldFaceUpload");
    if (!uploadResult) {
        return Error("Far heightfield owner face upload init failed: {}", uploadResult.error());
    }

    spdlog::info("Far heightfield owner init: creating IA streams");
    const uint64_t vertexByteCount =
        static_cast<uint64_t>(kFarHeightfieldVertexCount) * sizeof(uint32_t);
    auto vertexResult = m_farHeightfieldVertexIdUpload.Initialize(
        device,
        vertexByteCount,
        "FarHeightfieldVertexIds");
    if (!vertexResult) {
        return Error("Far heightfield owner vertex stream init failed: {}", vertexResult.error());
    }
    if (auto* mapped = static_cast<uint32_t*>(m_farHeightfieldVertexIdUpload.GetMappedData())) {
        for (uint32_t i = 0; i < kFarHeightfieldVertexCount; ++i) {
            mapped[i] = i;
        }
    } else {
        return Error("Far heightfield owner vertex stream map is null");
    }
    m_farHeightfieldVertexIdView.BufferLocation =
        m_farHeightfieldVertexIdUpload.GetGPUVirtualAddress();
    m_farHeightfieldVertexIdView.SizeInBytes = static_cast<UINT>(vertexByteCount);
    m_farHeightfieldVertexIdView.StrideInBytes = sizeof(uint32_t);

    const uint64_t indexByteCount =
        static_cast<uint64_t>(kFarHeightfieldIndexCount) * sizeof(uint32_t);
    auto indexResult = m_farHeightfieldIndexUpload.Initialize(
        device,
        indexByteCount,
        "FarHeightfieldIndices");
    if (!indexResult) {
        return Error("Far heightfield owner index stream init failed: {}", indexResult.error());
    }
    if (auto* mappedIndices = static_cast<uint32_t*>(m_farHeightfieldIndexUpload.GetMappedData())) {
        for (uint32_t face = 0; face < kFarHeightfieldFaceCount; ++face) {
            const uint32_t vertexBase = face * 4u;
            const uint32_t indexBase = face * 6u;
            mappedIndices[indexBase + 0u] = vertexBase + 0u;
            mappedIndices[indexBase + 1u] = vertexBase + 1u;
            mappedIndices[indexBase + 2u] = vertexBase + 2u;
            mappedIndices[indexBase + 3u] = vertexBase + 0u;
            mappedIndices[indexBase + 4u] = vertexBase + 2u;
            mappedIndices[indexBase + 5u] = vertexBase + 3u;
        }
    } else {
        return Error("Far heightfield owner index stream map is null");
    }
    m_farHeightfieldIndexView.BufferLocation =
        m_farHeightfieldIndexUpload.GetGPUVirtualAddress();
    m_farHeightfieldIndexView.SizeInBytes = static_cast<UINT>(indexByteCount);
    m_farHeightfieldIndexView.Format = DXGI_FORMAT_R32_UINT;

    spdlog::info(
        "Far heightfield owner created: cells={} faces={} indices={} generation={}",
        kFarHeightfieldCellCount,
        kFarHeightfieldFaceCount,
        kFarHeightfieldIndexCount,
        m_config.farHeightfieldOwnerGpuGenerate ? "gpu_far_height_voxelized" : "cpu_probe");
    return {};
}

Result<void> Renderer::CreateFarMaxHeightCacheResources(ID3D12Device* device) {
    if (!device) {
        return Error("Far max-height cache: null device");
    }
    m_farMaxHeightCacheValid = false;
    m_farMaxHeightCacheKey = {};

    spdlog::info(
        "Far max-height cache init: cells={} levels={} elements={} bytes={}",
        kFarMaxHeightCacheLeafCellCount,
        kFarMaxHeightCacheMipLevels,
        kFarMaxHeightCacheElementCount,
        kFarMaxHeightCacheBufferBytes);
    auto bufferResult = m_farMaxHeightCache.Initialize(
        device,
        kFarMaxHeightCacheBufferBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        kFarMaxHeightCacheStride,
        "FarMaxHeightCache");
    if (!bufferResult) {
        return Error("Far max-height cache buffer init failed: {}", bufferResult.error());
    }
    if (auto srv = m_farMaxHeightCache.CreateSRV(device, m_heapManager); !srv) {
        return Error("Far max-height cache SRV init failed: {}", srv.error());
    }
    if (auto uav = m_farMaxHeightCache.CreateUAV(device, m_heapManager); !uav) {
        return Error("Far max-height cache UAV init failed: {}", uav.error());
    }

    const std::filesystem::path csPath =
        m_config.shaderPath / "Compute" / "CS_GenerateFarMaxHeightCache.hlsl";
    auto compileResult = m_shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Far max-height cache generate CS compile failed: {}", compileResult.error());
    }
    m_farMaxHeightCacheGenerateCS = compileResult.value();
    if (!m_farMaxHeightCacheGenerateCS.IsValid()) {
        return Error(
            "Far max-height cache generate CS invalid: {}",
            m_farMaxHeightCacheGenerateCS.errors);
    }

    ComputePipelineDesc desc;
    desc.computeShader = m_farMaxHeightCacheGenerateCS;
    desc.debugName = "CS_GenerateFarMaxHeightCache";
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 16 });
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 });
    if (auto pipeResult = m_farMaxHeightCacheGeneratePipeline.Initialize(device, desc); !pipeResult) {
        return Error("Far max-height cache generate pipeline init failed: {}", pipeResult.error());
    }

    spdlog::info("Far max-height cache created: generation=gpu_voxelized_mip");
    return {};
}

Result<void> Renderer::CreateFarMaxHeightNoHitMaskPipeline(ID3D12Device* device) {
    if (!device) {
        return Error("Far max-height no-hit mask: null device");
    }

    const std::filesystem::path csPath =
        m_config.shaderPath / "Compute" / "CS_FarMaxHeightNoHitMask.hlsl";
    auto compileResult = m_shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Far max-height no-hit mask CS compile failed: {}", compileResult.error());
    }
    m_farMaxHeightNoHitMaskCS = compileResult.value();
    if (!m_farMaxHeightNoHitMaskCS.IsValid()) {
        return Error(
            "Far max-height no-hit mask CS invalid: {}",
            m_farMaxHeightNoHitMaskCS.errors);
    }

    ComputePipelineDesc desc;
    desc.computeShader = m_farMaxHeightNoHitMaskCS;
    desc.debugName = "CS_FarMaxHeightNoHitMask";
    desc.rootParams.push_back({ RootParamType::ConstantBuffer, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // b0
    desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // t0 cache
    desc.rootParams.push_back({ RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u0 mask
    desc.rootParams.push_back({ RootParamType::DescriptorTable, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u1 horizon
    desc.rootParams.push_back(
        { RootParamType::Constants32Bit, 1, 0, sizeof(FarMaxHeightScreenMaskConstants) / sizeof(uint32_t) }); // b1 pass params
    if (auto pipeResult = m_farMaxHeightNoHitMaskPipeline.Initialize(device, desc); !pipeResult) {
        return Error("Far max-height no-hit mask pipeline init failed: {}", pipeResult.error());
    }
    spdlog::info("Far max-height no-hit mask pipeline created");
    return {};
}

Result<void> Renderer::CreateBackgroundHorizonTileMaskPipeline(ID3D12Device* device) {
    if (!device) {
        return Error("Background horizon tile mask: null device");
    }

    const std::filesystem::path csPath =
        m_config.shaderPath / "Compute" / "CS_BackgroundHorizonTileMask.hlsl";
    auto compileResult = m_shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Background horizon tile mask CS compile failed: {}", compileResult.error());
    }
    m_backgroundHorizonTileMaskCS = compileResult.value();
    if (!m_backgroundHorizonTileMaskCS.IsValid()) {
        return Error(
            "Background horizon tile mask CS invalid: {}",
            m_backgroundHorizonTileMaskCS.errors);
    }

    ComputePipelineDesc desc;
    desc.computeShader = m_backgroundHorizonTileMaskCS;
    desc.debugName = "CS_BackgroundHorizonTileMask";
    desc.rootParams.push_back(
        { RootParamType::Constants32Bit, 0, 0, sizeof(BackgroundHorizonTileMaskConstants) / sizeof(uint32_t) }); // b0 params
    desc.rootParams.push_back(
        { RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV }); // t0 background color
    desc.rootParams.push_back(
        { RootParamType::DescriptorTable, 0, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u0 render ownership stats
    desc.rootParams.push_back(
        { RootParamType::DescriptorTable, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u1 tile mask
    desc.rootParams.push_back(
        { RootParamType::DescriptorTable, 2, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u2 compact tile list
    desc.rootParams.push_back(
        { RootParamType::DescriptorTable, 3, 0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV }); // u3 non-indexed draw args words
    if (auto pipeResult = m_backgroundHorizonTileMaskPipeline.Initialize(device, desc); !pipeResult) {
        return Error("Background horizon tile mask pipeline init failed: {}", pipeResult.error());
    }
    spdlog::info("Background horizon tile mask pipeline created");
    return {};
}

void Renderer::GenerateFarMaxHeightCache(
    ID3D12GraphicsCommandList* cmdList,
    const void* frameConstantsCpu)
{
    if (!cmdList ||
        !frameConstantsCpu ||
        !m_config.farMaxHeightCacheEnabled ||
        !m_farMaxHeightCache.GetResource() ||
        !m_farMaxHeightCacheGeneratePipeline.IsValid()) {
        return;
    }

    const FrameConstantsCpu& constants =
        *static_cast<const FrameConstantsCpu*>(frameConstantsCpu);
    float originX = 0.0f;
    float originZ = 0.0f;
    ComputeFarMaxHeightCacheOrigin(
        constants.cameraPosition[0],
        constants.cameraPosition[2],
        originX,
        originZ);

    const uint32_t seed = Uint32BitsFromFloat(constants.exactNearParams[1]);
    const float cacheExtent =
        static_cast<float>(kFarMaxHeightCacheLeafCellCount) * kFarMaxHeightCacheCellSize;
    const float canonicalCameraX = originX + cacheExtent * 0.5f + kFarMaxHeightCacheCellSize * 0.5f;
    const float canonicalCameraZ = originZ + cacheExtent * 0.5f + kFarMaxHeightCacheCellSize * 0.5f;
    const std::array<uint32_t, 3> cacheKey = {
        Uint32BitsFromFloat(originX),
        Uint32BitsFromFloat(originZ),
        seed,
    };
    if (m_farMaxHeightCacheValid && m_farMaxHeightCacheKey == cacheKey) {
        return;
    }

    std::array<uint32_t, kFarMaxHeightCacheMipLevels> widths{};
    std::array<uint32_t, kFarMaxHeightCacheMipLevels> heights{};
    std::array<uint32_t, kFarMaxHeightCacheMipLevels> offsets{};
    uint32_t width = kFarMaxHeightCacheLeafCellCount;
    uint32_t height = kFarMaxHeightCacheLeafCellCount;
    uint32_t offset = 0;
    for (uint32_t level = 0; level < kFarMaxHeightCacheMipLevels; ++level) {
        widths[level] = width;
        heights[level] = height;
        offsets[level] = offset;
        offset += width * height;
        width = std::max(1u, (width + 1u) / 2u);
        height = std::max(1u, (height + 1u) / 2u);
    }

    m_farMaxHeightCache.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_farMaxHeightCacheGeneratePipeline.Bind(cmdList);

    for (uint32_t level = 0; level < kFarMaxHeightCacheMipLevels; ++level) {
        FarMaxHeightCacheGenerateConstants gen{};
        gen.level = level;
        gen.srcOffset = level == 0 ? 0u : offsets[level - 1u];
        gen.dstOffset = offsets[level];
        gen.worldSeed = seed;
        gen.srcWidth = level == 0 ? widths[level] : widths[level - 1u];
        gen.srcHeight = level == 0 ? heights[level] : heights[level - 1u];
        gen.dstWidth = widths[level];
        gen.dstHeight = heights[level];
        gen.originXBits = Uint32BitsFromFloat(originX);
        gen.originZBits = Uint32BitsFromFloat(originZ);
        gen.leafCellSizeBits = Uint32BitsFromFloat(kFarMaxHeightCacheCellSize);
        gen.heightPadBits = Uint32BitsFromFloat(kFarMaxHeightCacheHeightPad);
        gen.cameraXBits = Uint32BitsFromFloat(canonicalCameraX);
        gen.cameraYBits = Uint32BitsFromFloat(constants.cameraPosition[1]);
        gen.cameraZBits = Uint32BitsFromFloat(canonicalCameraZ);

        m_farMaxHeightCacheGeneratePipeline.SetRoot32BitConstants(
            cmdList,
            0,
            sizeof(gen) / sizeof(uint32_t),
            &gen);
        cmdList->SetComputeRootUnorderedAccessView(
            1,
            m_farMaxHeightCache.GetGPUVirtualAddress());
        const uint32_t dstCount = gen.dstWidth * gen.dstHeight;
        m_farMaxHeightCacheGeneratePipeline.Dispatch(
            cmdList,
            (dstCount + 127u) / 128u,
            1u,
            1u);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_farMaxHeightCache.GetResource();
        cmdList->ResourceBarrier(1, &barrier);
    }

    m_farMaxHeightCache.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_farMaxHeightCacheKey = cacheKey;
    m_farMaxHeightCacheValid = true;
}

void Renderer::GenerateFarMaxHeightNoHitMask(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    if (!cmdList ||
        !m_config.farMaxHeightNoHitMaskEnabled ||
        !m_farMaxHeightNoHitMaskPipeline.IsValid() ||
        !m_farMaxHeightCache.GetResource() ||
        !m_farMaxHeightCache.GetShaderVisibleSRV().IsValid() ||
        !m_farMaxHeightNoHitMask.resource ||
        !m_farMaxHeightNoHitMask.uav.IsValid() ||
        !m_farMaxHeightScreenHorizon.GetResource() ||
        !m_farMaxHeightScreenHorizon.GetShaderVisibleSRV().IsValid() ||
        !m_farMaxHeightScreenHorizon.GetShaderVisibleUAV().IsValid() ||
        m_farMaxHeightScreenHorizonTileCount == 0u) {
        return;
    }

    if (m_farMaxHeightNoHitMask.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_farMaxHeightNoHitMask.resource.Get(),
            m_farMaxHeightNoHitMask.state,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &barrier);
        m_farMaxHeightNoHitMask.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    m_farMaxHeightCache.TransitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_farMaxHeightScreenHorizon.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_farMaxHeightNoHitMaskPipeline.Bind(cmdList);
    cmdList->SetComputeRootConstantBufferView(0, frameConstants);
    cmdList->SetComputeRootDescriptorTable(1, m_farMaxHeightCache.GetShaderVisibleSRV().gpu);
    cmdList->SetComputeRootDescriptorTable(2, m_farMaxHeightNoHitMask.uav.gpu);
    cmdList->SetComputeRootDescriptorTable(3, m_farMaxHeightScreenHorizon.GetShaderVisibleUAV().gpu);

    FarMaxHeightScreenMaskConstants params{};
    params.tileCount = m_farMaxHeightScreenHorizonTileCount;
    params.tileWidth = kFarMaxHeightScreenMaskTileWidth;
    const float maskDilationPixels = ClampFinite(
        m_config.farMaxHeightScreenMaskDilationPixels,
        0.0f,
        32.0f,
        kFarMaxHeightScreenMaskDilationPixels);
    const uint32_t maskProjectMip = std::min(
        m_config.farMaxHeightScreenMaskMipLevel,
        kFarMaxHeightCacheMipLevels - 1u);
    params.dilationPixelsBits = Uint32BitsFromFloat(maskDilationPixels);
    float projectCellSize = 0.0f;
    ComputeFarMaxHeightCacheMipInfo(
        maskProjectMip,
        params.projectOffset,
        params.projectSide,
        projectCellSize);
    params.projectCellSizeBits = Uint32BitsFromFloat(projectCellSize);

    params.passIndex = 0u;
    m_farMaxHeightNoHitMaskPipeline.SetRoot32BitConstants(
        cmdList,
        4,
        sizeof(params) / sizeof(uint32_t),
        &params);
    m_farMaxHeightNoHitMaskPipeline.Dispatch(
        cmdList,
        (m_farMaxHeightScreenHorizonTileCount + 7u) / 8u,
        1u,
        1u);

    D3D12_RESOURCE_BARRIER horizonBarrier = CD3DX12_RESOURCE_BARRIER::UAV(
        m_farMaxHeightScreenHorizon.GetResource());
    cmdList->ResourceBarrier(1, &horizonBarrier);

    params.passIndex = 1u;
    m_farMaxHeightNoHitMaskPipeline.SetRoot32BitConstants(
        cmdList,
        4,
        sizeof(params) / sizeof(uint32_t),
        &params);
    m_farMaxHeightNoHitMaskPipeline.Dispatch(
        cmdList,
        (params.projectSide + 7u) / 8u,
        (params.projectSide + 7u) / 8u,
        1u);

    cmdList->ResourceBarrier(1, &horizonBarrier);
    m_farMaxHeightCache.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_farMaxHeightScreenHorizon.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    QueueFarMaxHeightHorizonCsvReadback(cmdList);
}

void Renderer::GenerateBackgroundHorizonTileMask(
    ID3D12GraphicsCommandList* cmdList,
    const SparseNearField* sparseNearField,
    uint32_t frameIndex)
{
    if (!cmdList ||
        !m_config.backgroundPassHorizonTileMask ||
        !UseBackgroundPassSplit() ||
        !m_backgroundHorizonTileMaskPipeline.IsValid() ||
        !m_backgroundPassColor.Get() ||
        !m_backgroundPassSrv.IsValid() ||
        !m_backgroundHorizonTileMask.resource ||
        !m_backgroundHorizonTileMask.uav.IsValid() ||
        !m_backgroundHorizonTileList.GetResource() ||
        !m_backgroundHorizonTileList.GetShaderVisibleUAV().IsValid() ||
        !m_backgroundHorizonTileDrawArgs.GetResource() ||
        !m_backgroundHorizonTileDrawArgs.GetShaderVisibleUAV().IsValid()) {
        return;
    }

    BackgroundHorizonTileMaskConstants params{};
    params.fullWidth = std::max(1u, m_width);
    params.fullHeight = std::max(1u, m_height);
    params.tileSize = std::clamp(m_config.backgroundPassHorizonTileSize, 4u, 32u);
    params.selectorMode = std::min(m_config.backgroundPassHorizonTileSelector, 1u);
    const float thresholdFallback = params.selectorMode == 1u ? 0.00225f : 0.28f;
    params.thresholdBits = Uint32BitsFromFloat(
        ClampFinite(m_config.backgroundPassHorizonTileThreshold, 0.0f, 1.0f, thresholdFallback));
    params.y0 = std::min(params.fullHeight, m_config.backgroundPassHorizonTileY0);
    params.y1 = std::min(params.fullHeight, std::max(m_config.backgroundPassHorizonTileY1, params.y0));
    params.frameIndex = frameIndex;

    if (m_backgroundHorizonTileMask.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER maskToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            m_backgroundHorizonTileMask.resource.Get(),
            m_backgroundHorizonTileMask.state,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &maskToUav);
        m_backgroundHorizonTileMask.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    m_backgroundHorizonTileList.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_backgroundHorizonTileDrawArgs.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT drawArgsClear[4] = { 0u, 0u, 0u, 0u };
    cmdList->ClearUnorderedAccessViewUint(
        m_backgroundHorizonTileDrawArgs.GetShaderVisibleUAV().gpu,
        m_backgroundHorizonTileDrawArgs.GetStagingUAV().cpu,
        m_backgroundHorizonTileDrawArgs.GetResource(),
        drawArgsClear,
        0,
        nullptr);

    D3D12_RESOURCE_BARRIER preComputeBarrier = {};
    preComputeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    preComputeBarrier.UAV.pResource = nullptr;
    cmdList->ResourceBarrier(1, &preComputeBarrier);

    ID3D12DescriptorHeap* computeHeaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, computeHeaps);
    m_backgroundHorizonTileMaskPipeline.Bind(cmdList);
    m_backgroundHorizonTileMaskPipeline.SetRoot32BitConstants(
        cmdList,
        0,
        sizeof(params) / sizeof(uint32_t),
        &params);
    cmdList->SetComputeRootDescriptorTable(1, m_backgroundPassSrv.gpu);
    cmdList->SetComputeRootDescriptorTable(
        2,
        (sparseNearField && sparseNearField->renderOwnershipUAV.IsValid())
            ? sparseNearField->renderOwnershipUAV.gpu
            : m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);
    cmdList->SetComputeRootDescriptorTable(3, m_backgroundHorizonTileMask.uav.gpu);
    cmdList->SetComputeRootDescriptorTable(4, m_backgroundHorizonTileList.GetShaderVisibleUAV().gpu);
    cmdList->SetComputeRootDescriptorTable(5, m_backgroundHorizonTileDrawArgs.GetShaderVisibleUAV().gpu);
    m_backgroundHorizonTileMaskPipeline.Dispatch(
        cmdList,
        ((params.fullWidth + params.tileSize - 1u) / params.tileSize + 7u) / 8u,
        ((params.fullHeight + params.tileSize - 1u) / params.tileSize + 7u) / 8u,
        1u);

    D3D12_RESOURCE_BARRIER postComputeBarrier = {};
    postComputeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    postComputeBarrier.UAV.pResource = nullptr;
    cmdList->ResourceBarrier(1, &postComputeBarrier);
    D3D12_RESOURCE_BARRIER maskToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        m_backgroundHorizonTileMask.resource.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &maskToSrv);
    m_backgroundHorizonTileMask.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_backgroundHorizonTileList.TransitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_backgroundHorizonTileDrawArgs.TransitionTo(cmdList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

void Renderer::TryRetireFarHeightfieldOwnerFaceCsv() {
    if (!m_farHeightfieldOwnerFaceCsvReadbackPending ||
        m_farHeightfieldOwnerFaceCsvWritten ||
        m_config.farHeightfieldOwnerFaceCsvPath.empty()) {
        return;
    }
    if (m_farHeightfieldFrameSerial <= m_farHeightfieldOwnerFaceCsvQueuedSerial) {
        return;
    }
    if (!m_farHeightfieldFaceReadback.GetResource()) {
        spdlog::warn(
            "FAR_OWNER_FACE_CSV_WRITE_FAILED path={} reason=no_readback_buffer",
            m_config.farHeightfieldOwnerFaceCsvPath.string());
        m_farHeightfieldOwnerFaceCsvReadbackPending = false;
        m_farHeightfieldOwnerFaceCsvWritten = true;
        return;
    }

    if (m_commandQueue) {
        m_commandQueue->Flush();
    }

    const auto* faces = static_cast<const FarHeightfieldFaceCpu*>(m_farHeightfieldFaceReadback.Map());
    if (!faces) {
        spdlog::warn(
            "FAR_OWNER_FACE_CSV_WRITE_FAILED path={} reason=map_failed",
            m_config.farHeightfieldOwnerFaceCsvPath.string());
        m_farHeightfieldOwnerFaceCsvReadbackPending = false;
        m_farHeightfieldOwnerFaceCsvWritten = true;
        return;
    }

    const std::filesystem::path& path = m_config.farHeightfieldOwnerFaceCsvPath;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            spdlog::warn(
                "FAR_OWNER_FACE_CSV_PARENT_CREATE_FAILED path={} error={}",
                parent.string(),
                ec.message());
        }
    }

    std::ofstream csv(path, std::ios::out | std::ios::trunc);
    uint32_t activeFaces = 0;
    if (!csv) {
        spdlog::warn(
            "FAR_OWNER_FACE_CSV_WRITE_FAILED path={} reason=open_failed",
            path.string());
    } else {
        csv << "# cameraX=" << m_farHeightfieldOwnerFaceCsvCamera[0]
            << " cameraY=" << m_farHeightfieldOwnerFaceCsvCamera[1]
            << " cameraZ=" << m_farHeightfieldOwnerFaceCsvCamera[2]
            << " faces=" << kFarHeightfieldFaceCount
            << " cellSize=" << kFarHeightfieldCellSizeVoxels
            << " originX=" << m_farHeightfieldOwnerFaceCsvOrigin[0]
            << " originZ=" << m_farHeightfieldOwnerFaceCsvOrigin[1]
            << " generation=gpu_far_height_voxelized\n";
        csv << "worldX,worldY,worldZ,payload\n";
        for (uint32_t index = 0; index < kFarHeightfieldFaceCount; ++index) {
            activeFaces += faces[index].payload != 0u ? 1u : 0u;
            csv << faces[index].worldX << ','
                << faces[index].worldY << ','
                << faces[index].worldZ << ','
                << faces[index].payload << '\n';
        }
        csv.close();
        if (!csv) {
            spdlog::warn(
                "FAR_OWNER_FACE_CSV_WRITE_INCOMPLETE path={}",
                path.string());
        } else {
            spdlog::info(
                "FAR_OWNER_FACE_CSV path={} faces={} activeFaces={} camera=({:.2f},{:.2f},{:.2f}) generation=gpu_far_height_voxelized",
                path.string(),
                kFarHeightfieldFaceCount,
                activeFaces,
                m_farHeightfieldOwnerFaceCsvCamera[0],
                m_farHeightfieldOwnerFaceCsvCamera[1],
                m_farHeightfieldOwnerFaceCsvCamera[2]);
        }
    }

    m_farHeightfieldFaceReadback.Unmap();
    m_farHeightfieldOwnerFaceCsvReadbackPending = false;
    m_farHeightfieldOwnerFaceCsvWritten = true;
}

bool Renderer::QueueFarHeightfieldOwnerFaceCsvReadback(
    ID3D12GraphicsCommandList* cmdList,
    const float* cameraPosition,
    float originX,
    float originZ)
{
    if (!cmdList ||
        !cameraPosition ||
        m_farHeightfieldOwnerFaceCsvWritten ||
        m_farHeightfieldOwnerFaceCsvReadbackPending ||
        m_config.farHeightfieldOwnerFaceCsvPath.empty()) {
        return false;
    }
    if (!m_farHeightfieldFaces.GetResource() || !m_farHeightfieldFaceReadback.GetResource()) {
        spdlog::warn(
            "FAR_OWNER_FACE_CSV_SKIPPED path={} reason=no_readback_resource generation=gpu_far_height_voxelized",
            m_config.farHeightfieldOwnerFaceCsvPath.string());
        m_farHeightfieldOwnerFaceCsvWritten = true;
        return false;
    }

    m_farHeightfieldFaces.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyBufferRegion(
        m_farHeightfieldFaceReadback.GetResource(),
        0,
        m_farHeightfieldFaces.GetResource(),
        0,
        kFarHeightfieldFaceBufferBytes);
    m_farHeightfieldOwnerFaceCsvReadbackPending = true;
    m_farHeightfieldOwnerFaceCsvQueuedSerial = m_farHeightfieldFrameSerial;
    m_farHeightfieldOwnerFaceCsvCamera[0] = cameraPosition[0];
    m_farHeightfieldOwnerFaceCsvCamera[1] = cameraPosition[1];
    m_farHeightfieldOwnerFaceCsvCamera[2] = cameraPosition[2];
    m_farHeightfieldOwnerFaceCsvOrigin[0] = originX;
    m_farHeightfieldOwnerFaceCsvOrigin[1] = originZ;
    spdlog::info(
        "FAR_OWNER_FACE_CSV_QUEUED path={} faces={} generation=gpu_far_height_voxelized",
        m_config.farHeightfieldOwnerFaceCsvPath.string(),
        kFarHeightfieldFaceCount);
    return true;
}

void Renderer::TryRetireFarMaxHeightHorizonCsv() {
    if (!m_farMaxHeightHorizonCsvReadbackPending ||
        m_farMaxHeightHorizonCsvWritten ||
        m_config.farMaxHeightHorizonCsvPath.empty()) {
        return;
    }
    if (m_farHeightfieldFrameSerial <= m_farMaxHeightHorizonCsvQueuedSerial) {
        return;
    }
    if (!m_farMaxHeightHorizonReadback.GetResource()) {
        spdlog::warn(
            "FAR_MAX_HEIGHT_HORIZON_CSV_WRITE_FAILED path={} reason=no_readback_buffer",
            m_config.farMaxHeightHorizonCsvPath.string());
        m_farMaxHeightHorizonCsvReadbackPending = false;
        m_farMaxHeightHorizonCsvWritten = true;
        return;
    }

    if (m_commandQueue) {
        m_commandQueue->Flush();
    }

    const auto* horizon = static_cast<const uint32_t*>(m_farMaxHeightHorizonReadback.Map());
    if (!horizon) {
        spdlog::warn(
            "FAR_MAX_HEIGHT_HORIZON_CSV_WRITE_FAILED path={} reason=map_failed",
            m_config.farMaxHeightHorizonCsvPath.string());
        m_farMaxHeightHorizonCsvReadbackPending = false;
        m_farMaxHeightHorizonCsvWritten = true;
        return;
    }

    const std::filesystem::path& path = m_config.farMaxHeightHorizonCsvPath;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            spdlog::warn(
                "FAR_MAX_HEIGHT_HORIZON_CSV_PARENT_CREATE_FAILED path={} error={}",
                parent.string(),
                ec.message());
        }
    }

    std::ofstream csv(path, std::ios::out | std::ios::trunc);
    uint32_t validTiles = 0;
    if (!csv) {
        spdlog::warn(
            "FAR_MAX_HEIGHT_HORIZON_CSV_WRITE_FAILED path={} reason=open_failed",
            path.string());
    } else {
        csv << "# width=" << m_width
            << " height=" << m_height
            << " tileWidth=" << kFarMaxHeightScreenMaskTileWidth
            << " tileCount=" << m_farMaxHeightScreenHorizonTileCount
            << " empty=4294967295\n";
        csv << "tile,horizonY\n";
        for (uint32_t tile = 0; tile < m_farMaxHeightScreenHorizonTileCount; ++tile) {
            validTiles += horizon[tile] != 0xffffffffu ? 1u : 0u;
            csv << tile << ',' << horizon[tile] << '\n';
        }
        csv.close();
        if (!csv) {
            spdlog::warn(
                "FAR_MAX_HEIGHT_HORIZON_CSV_WRITE_INCOMPLETE path={}",
                path.string());
        } else {
            spdlog::info(
                "FAR_MAX_HEIGHT_HORIZON_CSV path={} tiles={} validTiles={} tileWidth={}",
                path.string(),
                m_farMaxHeightScreenHorizonTileCount,
                validTiles,
                kFarMaxHeightScreenMaskTileWidth);
        }
    }

    m_farMaxHeightHorizonReadback.Unmap();
    m_farMaxHeightHorizonCsvReadbackPending = false;
    m_farMaxHeightHorizonCsvWritten = true;
}

bool Renderer::QueueFarMaxHeightHorizonCsvReadback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList ||
        m_farMaxHeightHorizonCsvWritten ||
        m_farMaxHeightHorizonCsvReadbackPending ||
        m_config.farMaxHeightHorizonCsvPath.empty()) {
        return false;
    }
    if (!m_farMaxHeightScreenHorizon.GetResource() ||
        !m_farMaxHeightHorizonReadback.GetResource() ||
        m_farMaxHeightScreenHorizonTileCount == 0u) {
        spdlog::warn(
            "FAR_MAX_HEIGHT_HORIZON_CSV_SKIPPED path={} reason=no_readback_resource",
            m_config.farMaxHeightHorizonCsvPath.string());
        m_farMaxHeightHorizonCsvWritten = true;
        return false;
    }

    m_farMaxHeightScreenHorizon.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyBufferRegion(
        m_farMaxHeightHorizonReadback.GetResource(),
        0,
        m_farMaxHeightScreenHorizon.GetResource(),
        0,
        static_cast<uint64_t>(m_farMaxHeightScreenHorizonTileCount) * sizeof(uint32_t));
    m_farMaxHeightHorizonCsvReadbackPending = true;
    m_farMaxHeightHorizonCsvQueuedSerial = m_farHeightfieldFrameSerial;
    m_farMaxHeightScreenHorizon.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    spdlog::info(
        "FAR_MAX_HEIGHT_HORIZON_CSV_QUEUED path={} tiles={}",
        m_config.farMaxHeightHorizonCsvPath.string(),
        m_farMaxHeightScreenHorizonTileCount);
    return true;
}

void Renderer::DrawFarHeightfieldOwner(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants,
    const void* frameConstantsCpu,
    const DescriptorHandle& materialPaletteSRV)
{
    if (!cmdList || !frameConstantsCpu || !materialPaletteSRV.IsValid() || !m_dsvHandle.IsValid()) {
        return;
    }
    (void)frameConstants;

    const FrameConstantsCpu& baseConstants =
        *static_cast<const FrameConstantsCpu*>(frameConstantsCpu);
    float forwardX = baseConstants.cameraForward[0];
    float forwardZ = baseConstants.cameraForward[2];
    const float forwardLen2 = forwardX * forwardX + forwardZ * forwardZ;
    if (forwardLen2 > 1.0e-5f) {
        const float invLen = 1.0f / std::sqrt(forwardLen2);
        forwardX *= invLen;
        forwardZ *= invLen;
    } else {
        forwardX = 0.0f;
        forwardZ = 1.0f;
    }

    const float extent = static_cast<float>(kFarHeightfieldCellCount) * kFarHeightfieldCellSize;
    const float halfExtent = extent * 0.5f;
    const float forwardShift = std::max(0.0f, kFarHeightfieldMaxDistance - halfExtent);
    const float centerX = baseConstants.cameraPosition[0] + forwardX * forwardShift;
    const float centerZ = baseConstants.cameraPosition[2] + forwardZ * forwardShift;
    const float originX = std::floor((centerX - halfExtent) / kFarHeightfieldCellSize) *
        kFarHeightfieldCellSize;
    const float originZ = std::floor((centerZ - halfExtent) / kFarHeightfieldCellSize) *
        kFarHeightfieldCellSize;

    const bool useGpuGenerate =
        m_config.farHeightfieldOwnerGpuGenerate &&
        m_farHeightfieldGeneratePipeline.IsValid();
    if (useGpuGenerate) {
        FarHeightfieldGenerateConstants gen{};
        gen.faceCount = kFarHeightfieldFaceCount;
        gen.cellCount = kFarHeightfieldCellCount;
        gen.baseGridCellSize = kFarHeightfieldCellSizeVoxels;
        gen.worldSeed = Uint32BitsFromFloat(baseConstants.exactNearParams[1]);
        gen.originXBits = Uint32BitsFromFloat(originX);
        gen.originZBits = Uint32BitsFromFloat(originZ);
        gen.farHandoffBits = Uint32BitsFromFloat(baseConstants.backgroundOwnershipParams[1]);
        gen.ownerMaxDistanceBits = Uint32BitsFromFloat(kFarHeightfieldOwnerMaxDistance);
        gen.cameraXBits = Uint32BitsFromFloat(baseConstants.cameraPosition[0]);
        gen.cameraYBits = Uint32BitsFromFloat(baseConstants.cameraPosition[1]);
        gen.cameraZBits = Uint32BitsFromFloat(baseConstants.cameraPosition[2]);

        m_farHeightfieldFaces.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_farHeightfieldGeneratePipeline.Bind(cmdList);
        m_farHeightfieldGeneratePipeline.SetRoot32BitConstants(
            cmdList,
            0,
            sizeof(gen) / sizeof(uint32_t),
            &gen);
        cmdList->SetComputeRootUnorderedAccessView(
            1,
            m_farHeightfieldFaces.GetGPUVirtualAddress());
        m_farHeightfieldGeneratePipeline.Dispatch(
            cmdList,
            (kFarHeightfieldFaceCount + 127u) / 128u,
            1u,
            1u);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_farHeightfieldFaces.GetResource();
        cmdList->ResourceBarrier(1, &barrier);
        QueueFarHeightfieldOwnerFaceCsvReadback(cmdList, baseConstants.cameraPosition, originX, originZ);
        m_farHeightfieldFaces.TransitionTo(
            cmdList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        if (auto* mappedFaces =
                static_cast<FarHeightfieldFaceCpu*>(m_farHeightfieldFaceUpload.GetMappedData())) {
            // F1 mechanism probe only: set VENPOD_FAR_HEIGHT_OWNER_GPU_GEN=1 for the
            // production-aligned generator path.
            const uint32_t payload = PackSparseSurfacePayload(
                3u,
                4u,
                kFarHeightfieldCellSizeVoxels,
                kFarHeightfieldCellSizeVoxels);

            for (uint32_t index = 0; index < kFarHeightfieldFaceCount; ++index) {
                const uint32_t slot = index % kFarHeightfieldFacesPerCell;
                const uint32_t cellIndex = index / kFarHeightfieldFacesPerCell;
                if (slot != 0u) {
                    mappedFaces[index].worldX = 0;
                    mappedFaces[index].worldY = 0;
                    mappedFaces[index].worldZ = 0;
                    mappedFaces[index].payload = 0u;
                    continue;
                }
                const uint32_t cellX = cellIndex % kFarHeightfieldCellCount;
                const uint32_t cellZ = cellIndex / kFarHeightfieldCellCount;
                mappedFaces[index].worldX = static_cast<int32_t>(
                    std::floor(originX + static_cast<float>(cellX) * kFarHeightfieldCellSize));
                mappedFaces[index].worldY = static_cast<int32_t>(kFarHeightfieldCpuProbeSurfaceY) - 1;
                mappedFaces[index].worldZ = static_cast<int32_t>(
                    std::floor(originZ + static_cast<float>(cellZ) * kFarHeightfieldCellSize));
                mappedFaces[index].payload = payload;
            }

            if (!m_farHeightfieldOwnerFaceCsvWritten &&
                !m_config.farHeightfieldOwnerFaceCsvPath.empty()) {
                std::ofstream csv(m_config.farHeightfieldOwnerFaceCsvPath, std::ios::out | std::ios::trunc);
                if (!csv) {
                    spdlog::warn(
                        "FAR_OWNER_FACE_CSV_WRITE_FAILED path={}",
                        m_config.farHeightfieldOwnerFaceCsvPath.string());
                    m_farHeightfieldOwnerFaceCsvWritten = true;
                } else {
                    csv << "# cameraX=" << baseConstants.cameraPosition[0]
                        << " cameraY=" << baseConstants.cameraPosition[1]
                        << " cameraZ=" << baseConstants.cameraPosition[2]
                        << " faces=" << kFarHeightfieldFaceCount
                        << " cellSize=" << kFarHeightfieldCellSizeVoxels
                        << " originX=" << originX
                        << " originZ=" << originZ
                        << " generation=cpu_probe\n";
                    csv << "worldX,worldY,worldZ,payload\n";
                    for (uint32_t index = 0; index < kFarHeightfieldFaceCount; ++index) {
                        csv << mappedFaces[index].worldX << ','
                            << mappedFaces[index].worldY << ','
                            << mappedFaces[index].worldZ << ','
                            << mappedFaces[index].payload << '\n';
                    }
                    csv.close();
                    if (!csv) {
                        spdlog::warn(
                            "FAR_OWNER_FACE_CSV_WRITE_INCOMPLETE path={}",
                            m_config.farHeightfieldOwnerFaceCsvPath.string());
                    } else {
                        spdlog::info(
                            "FAR_OWNER_FACE_CSV path={} faces={} camera=({:.2f},{:.2f},{:.2f}) generation=cpu_probe",
                            m_config.farHeightfieldOwnerFaceCsvPath.string(),
                            kFarHeightfieldFaceCount,
                            baseConstants.cameraPosition[0],
                            baseConstants.cameraPosition[1],
                            baseConstants.cameraPosition[2]);
                    }
                    m_farHeightfieldOwnerFaceCsvWritten = true;
                }
            }
        }

        m_farHeightfieldFaces.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyBufferRegion(
            m_farHeightfieldFaces.GetResource(),
            0,
            m_farHeightfieldFaceUpload.GetResource(),
            0,
            kFarHeightfieldFaceBufferBytes);
        m_farHeightfieldFaces.TransitionTo(
            cmdList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    FrameConstantsCpu farConstants =
        baseConstants;
    farConstants.nearOwnershipParams[3] = kFarHeightfieldOwnerMaxDistance;
    farConstants.surfaceRasterParams[0] = kFarHeightfieldOwnerMaxDistance;
    farConstants.surfaceParams[0] = 1.0f;
    farConstants.surfaceParams[1] = static_cast<float>(kFarHeightfieldFaceCount);
    farConstants.farFieldGridParams[3] = 0.0f;
    farConstants.sparseNearParams[0] = 0.0f;
    farConstants.sparseNearParams[1] = 0.0f;
    farConstants.sparseNearParams[2] = 0.0f;

    static_assert(sizeof(farConstants) <= kFrameConstantUploadBytes);
    UploadBuffer& upload = m_farHeightfieldConstantUploads[m_currentFrameIndex];
    if (void* mapped = upload.GetMappedData()) {
        std::memcpy(mapped, &farConstants, sizeof(farConstants));
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager.GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    auto bindFarSurface = [&]() {
        cmdList->OMSetStencilRef(1);
        cmdList->IASetVertexBuffers(0, 1, &m_farHeightfieldVertexIdView);
        cmdList->IASetIndexBuffer(&m_farHeightfieldIndexView);
        cmdList->SetGraphicsRootConstantBufferView(0, upload.GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(1, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(2, materialPaletteSRV.gpu);
        cmdList->SetGraphicsRootDescriptorTable(3, m_dummyRenderOwnershipUAV.GetShaderVisibleUAV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(4, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(5, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(6, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(7, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(8, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
        cmdList->SetGraphicsRootDescriptorTable(9, m_farHeightfieldFaces.GetShaderVisibleSRV().gpu);
    };
    auto drawFarSurface = [&]() {
        cmdList->DrawIndexedInstanced(kFarHeightfieldIndexCount, 1u, 0u, 0, 0u);
    };

    if (m_config.sparseSurfaceDepthPrepass &&
        m_sparseSurfaceDepthPrepassPipeline.GetPSO() != nullptr) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHandle.cpu;
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
        SetViewportAndScissor(cmdList, m_width, m_height);
        m_sparseSurfaceDepthPrepassPipeline.Bind(cmdList);
        bindFarSurface();
        drawFarSurface();
    }

    SetMainRenderTarget(cmdList);
    m_sparseSurfacePipeline.Bind(cmdList);
    bindFarSurface();
    drawFarSurface();
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
    if (m_config.backgroundPassEdgeAwareComposite) {
        psOptions.defines.push_back(L"VENPOD_BACKGROUND_COMPOSITE_EDGE_AWARE=1");
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
    if (m_backgroundHorizonTileMask.srv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_backgroundHorizonTileMask.srv);
    }
    if (m_backgroundHorizonTileMask.stagingSrv.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_backgroundHorizonTileMask.stagingSrv);
    }
    if (m_backgroundHorizonTileMask.uav.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_backgroundHorizonTileMask.uav);
    }
    if (m_backgroundHorizonTileMask.stagingUav.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_backgroundHorizonTileMask.stagingUav);
    }
    m_backgroundHorizonTileMask.resource.Reset();
    m_backgroundHorizonTileMask = TemporalTarget{};
    m_backgroundHorizonTileMaskWidth = 0;
    m_backgroundHorizonTileMaskHeight = 0;
    m_backgroundHorizonTileList.Shutdown();
    m_backgroundHorizonTileDrawArgs.Shutdown();
    m_backgroundHorizonTileListCapacity = 0;
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
    m_backgroundPassColor.Reset();
    m_backgroundPassDepth.Reset();
    m_backgroundPassColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_backgroundPassWidth = 0;
    m_backgroundPassHeight = 0;
}

Result<void> Renderer::CreateTemporalTarget(
    ID3D12Device* device, TemporalTarget& target, DXGI_FORMAT format, const wchar_t* name) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        format,
        static_cast<UINT64>(m_temporalWidth),
        static_cast<UINT>(m_temporalHeight),
        1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // No optimized clear value: these targets are fully written by the compute reproject (reuse pixels)
    // + the raymarch PS (marched pixels), never RTV-cleared, so the no-clear temporal invariant holds.
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&target.resource));
    if (FAILED(hr)) {
        return Error("Failed to create temporal target: 0x{:08X}", hr);
    }
    target.resource->SetName(name);
    target.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    target.rtv = m_heapManager.AllocateRtv();
    if (!target.rtv.IsValid()) return Error("Failed to allocate temporal RTV");
    device->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv.cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    target.stagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
    if (!target.stagingSrv.IsValid()) return Error("Failed to allocate temporal staging SRV");
    device->CreateShaderResourceView(target.resource.Get(), &srvDesc, target.stagingSrv.cpu);
    target.srv = m_heapManager.CopyToShaderVisible(device, target.stagingSrv);
    if (!target.srv.IsValid()) return Error("Failed to allocate temporal shader-visible SRV");
    device->CreateShaderResourceView(target.resource.Get(), &srvDesc, target.srv.cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    target.stagingUav = m_heapManager.AllocateStagingCbvSrvUav();
    if (!target.stagingUav.IsValid()) return Error("Failed to allocate temporal staging UAV");
    device->CreateUnorderedAccessView(target.resource.Get(), nullptr, &uavDesc, target.stagingUav.cpu);
    target.uav = m_heapManager.CopyToShaderVisible(device, target.stagingUav);
    if (!target.uav.IsValid()) return Error("Failed to allocate temporal shader-visible UAV");
    device->CreateUnorderedAccessView(target.resource.Get(), nullptr, &uavDesc, target.uav.cpu);
    return {};
}

Result<void> Renderer::CreateTemporalResources() {
    if (!UseTemporalReproject()) {
        DestroyTemporalResources();
        return {};
    }
    if (!m_device) {
        return Error("Device not initialized");
    }
    ID3D12Device* device = m_device->GetDevice();
    if (!device) {
        return Error("D3D12 device not initialized");
    }
    DestroyTemporalResources();
    m_temporalWidth = std::max(1u, m_width);
    m_temporalHeight = std::max(1u, m_height);
    for (int i = 0; i < 2; ++i) {
        if (auto r = CreateTemporalTarget(device, m_temporalColor[i], DXGI_FORMAT_R16G16B16A16_FLOAT,
                i == 0 ? L"TemporalColor0" : L"TemporalColor1"); !r) return r;
        if (auto r = CreateTemporalTarget(device, m_temporalDistance[i], DXGI_FORMAT_R32_FLOAT,
                i == 0 ? L"TemporalDistance0" : L"TemporalDistance1"); !r) return r;
        if (auto r = CreateTemporalTarget(device, m_temporalMeta[i], DXGI_FORMAT_R32_UINT,
                i == 0 ? L"TemporalMeta0" : L"TemporalMeta1"); !r) return r;
    }
    if (auto r = CreateTemporalTarget(device, m_temporalMarchMask, DXGI_FORMAT_R8_UINT,
            L"TemporalMarchMask"); !r) return r;
    return {};
}

void Renderer::DestroyTemporalResources() {
    auto reset = [&](TemporalTarget& t) {
        if (t.srv.IsValid()) m_heapManager.FreeShaderVisibleCbvSrvUav(t.srv);
        if (t.stagingSrv.IsValid()) m_heapManager.FreeStagingCbvSrvUav(t.stagingSrv);
        if (t.uav.IsValid()) m_heapManager.FreeShaderVisibleCbvSrvUav(t.uav);
        if (t.stagingUav.IsValid()) m_heapManager.FreeStagingCbvSrvUav(t.stagingUav);
        if (t.rtv.IsValid()) m_heapManager.FreeRtv(t.rtv);
        t.resource.Reset();
        t = TemporalTarget{};
    };
    for (int i = 0; i < 2; ++i) {
        reset(m_temporalColor[i]);
        reset(m_temporalDistance[i]);
        reset(m_temporalMeta[i]);
    }
    reset(m_temporalMarchMask);
    m_temporalWidth = 0;
    m_temporalHeight = 0;
}

Result<void> Renderer::CreateFarMaxHeightNoHitMaskResources() {
    if (!m_config.farMaxHeightNoHitMaskEnabled) {
        DestroyFarMaxHeightNoHitMaskResources();
        return {};
    }
    if (!m_device) {
        return Error("Device not initialized");
    }
    ID3D12Device* device = m_device->GetDevice();
    if (!device) {
        return Error("D3D12 device not initialized");
    }

    DestroyFarMaxHeightNoHitMaskResources();
    m_farMaxHeightNoHitMaskWidth = std::max(1u, m_width);
    m_farMaxHeightNoHitMaskHeight = std::max(1u, m_height);
    m_farMaxHeightScreenHorizonTileCount =
        (m_farMaxHeightNoHitMaskWidth + kFarMaxHeightScreenMaskTileWidth - 1u) /
        kFarMaxHeightScreenMaskTileWidth;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16_UINT,
        static_cast<UINT64>(m_farMaxHeightNoHitMaskWidth),
        static_cast<UINT>(m_farMaxHeightNoHitMaskHeight),
        1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&m_farMaxHeightNoHitMask.resource));
    if (FAILED(hr)) {
        return Error("Failed to create far max-height no-hit mask: 0x{:08X}", hr);
    }
    m_farMaxHeightNoHitMask.resource->SetName(L"FarMaxHeightNoHitMask");
    m_farMaxHeightNoHitMask.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UINT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_farMaxHeightNoHitMask.stagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
    if (!m_farMaxHeightNoHitMask.stagingSrv.IsValid()) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to allocate far max-height no-hit staging SRV");
    }
    device->CreateShaderResourceView(
        m_farMaxHeightNoHitMask.resource.Get(),
        &srvDesc,
        m_farMaxHeightNoHitMask.stagingSrv.cpu);
    m_farMaxHeightNoHitMask.srv =
        m_heapManager.CopyToShaderVisible(device, m_farMaxHeightNoHitMask.stagingSrv);
    if (!m_farMaxHeightNoHitMask.srv.IsValid()) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to allocate far max-height no-hit shader-visible SRV");
    }
    device->CreateShaderResourceView(
        m_farMaxHeightNoHitMask.resource.Get(),
        &srvDesc,
        m_farMaxHeightNoHitMask.srv.cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R16_UINT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_farMaxHeightNoHitMask.stagingUav = m_heapManager.AllocateStagingCbvSrvUav();
    if (!m_farMaxHeightNoHitMask.stagingUav.IsValid()) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to allocate far max-height no-hit staging UAV");
    }
    device->CreateUnorderedAccessView(
        m_farMaxHeightNoHitMask.resource.Get(),
        nullptr,
        &uavDesc,
        m_farMaxHeightNoHitMask.stagingUav.cpu);
    m_farMaxHeightNoHitMask.uav =
        m_heapManager.CopyToShaderVisible(device, m_farMaxHeightNoHitMask.stagingUav);
    if (!m_farMaxHeightNoHitMask.uav.IsValid()) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to allocate far max-height no-hit shader-visible UAV");
    }
    device->CreateUnorderedAccessView(
        m_farMaxHeightNoHitMask.resource.Get(),
        nullptr,
        &uavDesc,
        m_farMaxHeightNoHitMask.uav.cpu);

    const uint64_t horizonBytes =
        static_cast<uint64_t>(std::max(1u, m_farMaxHeightScreenHorizonTileCount)) * sizeof(uint32_t);
    auto horizonResult = m_farMaxHeightScreenHorizon.Initialize(
        device,
        horizonBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "FarMaxHeightScreenHorizon");
    if (!horizonResult) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to create far max-height screen horizon buffer: {}", horizonResult.error());
    }
    if (auto horizonUav = m_farMaxHeightScreenHorizon.CreateUAV(device, m_heapManager); !horizonUav) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to create far max-height screen horizon UAV: {}", horizonUav.error());
    }
    if (auto horizonSrv = m_farMaxHeightScreenHorizon.CreateSRV(device, m_heapManager); !horizonSrv) {
        DestroyFarMaxHeightNoHitMaskResources();
        return Error("Failed to create far max-height screen horizon SRV: {}", horizonSrv.error());
    }
    if (!m_config.farMaxHeightHorizonCsvPath.empty()) {
        auto readbackResult = m_farMaxHeightHorizonReadback.Initialize(
            device,
            horizonBytes,
            BufferUsage::Readback,
            sizeof(uint32_t),
            "FarMaxHeightScreenHorizonReadback");
        if (!readbackResult) {
            DestroyFarMaxHeightNoHitMaskResources();
            return Error("Failed to create far max-height horizon readback: {}", readbackResult.error());
        }
    }
    return {};
}

void Renderer::DestroyFarMaxHeightNoHitMaskResources() {
    if (m_farMaxHeightNoHitMask.srv.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_farMaxHeightNoHitMask.srv);
    }
    if (m_farMaxHeightNoHitMask.stagingSrv.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_farMaxHeightNoHitMask.stagingSrv);
    }
    if (m_farMaxHeightNoHitMask.uav.IsValid()) {
        m_heapManager.FreeShaderVisibleCbvSrvUav(m_farMaxHeightNoHitMask.uav);
    }
    if (m_farMaxHeightNoHitMask.stagingUav.IsValid()) {
        m_heapManager.FreeStagingCbvSrvUav(m_farMaxHeightNoHitMask.stagingUav);
    }
    if (m_farMaxHeightNoHitMask.rtv.IsValid()) {
        m_heapManager.FreeRtv(m_farMaxHeightNoHitMask.rtv);
    }
    m_farMaxHeightNoHitMask.resource.Reset();
    m_farMaxHeightNoHitMask = TemporalTarget{};
    m_farMaxHeightNoHitMaskWidth = 0;
    m_farMaxHeightNoHitMaskHeight = 0;
    m_farMaxHeightScreenHorizon.Shutdown();
    m_farMaxHeightHorizonReadback.Shutdown();
    m_farMaxHeightHorizonCsvWritten = false;
    m_farMaxHeightHorizonCsvReadbackPending = false;
    m_farMaxHeightHorizonCsvQueuedSerial = 0;
    m_farMaxHeightScreenHorizonTileCount = 0;
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

    if (m_config.backgroundPassHorizonTileMask) {
        const uint32_t tileSize = std::max(1u, std::clamp(m_config.backgroundPassHorizonTileSize, 4u, 32u));
        m_backgroundHorizonTileMaskWidth = (std::max(1u, m_width) + tileSize - 1u) / tileSize;
        m_backgroundHorizonTileMaskHeight = (std::max(1u, m_height) + tileSize - 1u) / tileSize;
        auto maskDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_UINT,
            static_cast<UINT64>(m_backgroundHorizonTileMaskWidth),
            static_cast<UINT>(m_backgroundHorizonTileMaskHeight),
            1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &maskDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&m_backgroundHorizonTileMask.resource));
        if (FAILED(hr)) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile mask: 0x{:08X}", hr);
        }
        m_backgroundHorizonTileMask.resource->SetName(L"BackgroundHorizonTileMask");
        m_backgroundHorizonTileMask.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC maskSrvDesc = {};
        maskSrvDesc.Format = DXGI_FORMAT_R32_UINT;
        maskSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maskSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maskSrvDesc.Texture2D.MipLevels = 1;
        m_backgroundHorizonTileMask.stagingSrv = m_heapManager.AllocateStagingCbvSrvUav();
        if (!m_backgroundHorizonTileMask.stagingSrv.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background horizon tile mask staging SRV");
        }
        device->CreateShaderResourceView(
            m_backgroundHorizonTileMask.resource.Get(),
            &maskSrvDesc,
            m_backgroundHorizonTileMask.stagingSrv.cpu);
        m_backgroundHorizonTileMask.srv =
            m_heapManager.CopyToShaderVisible(device, m_backgroundHorizonTileMask.stagingSrv);
        if (!m_backgroundHorizonTileMask.srv.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background horizon tile mask shader-visible SRV");
        }
        device->CreateShaderResourceView(
            m_backgroundHorizonTileMask.resource.Get(),
            &maskSrvDesc,
            m_backgroundHorizonTileMask.srv.cpu);

        D3D12_UNORDERED_ACCESS_VIEW_DESC maskUavDesc = {};
        maskUavDesc.Format = DXGI_FORMAT_R32_UINT;
        maskUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_backgroundHorizonTileMask.stagingUav = m_heapManager.AllocateStagingCbvSrvUav();
        if (!m_backgroundHorizonTileMask.stagingUav.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background horizon tile mask staging UAV");
        }
        device->CreateUnorderedAccessView(
            m_backgroundHorizonTileMask.resource.Get(),
            nullptr,
            &maskUavDesc,
            m_backgroundHorizonTileMask.stagingUav.cpu);
        m_backgroundHorizonTileMask.uav =
            m_heapManager.CopyToShaderVisible(device, m_backgroundHorizonTileMask.stagingUav);
        if (!m_backgroundHorizonTileMask.uav.IsValid()) {
            DestroyBackgroundPassResources();
            return Error("Failed to allocate background horizon tile mask shader-visible UAV");
        }
        device->CreateUnorderedAccessView(
            m_backgroundHorizonTileMask.resource.Get(),
            nullptr,
            &maskUavDesc,
            m_backgroundHorizonTileMask.uav.cpu);

        const uint32_t tileCapacity =
            std::max(1u, m_backgroundHorizonTileMaskWidth * m_backgroundHorizonTileMaskHeight);
        m_backgroundHorizonTileListCapacity = tileCapacity;
        auto tileListResult = m_backgroundHorizonTileList.Initialize(
            device,
            static_cast<uint64_t>(tileCapacity) * sizeof(uint32_t) * 2ull,
            BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(uint32_t) * 2u,
            "BackgroundHorizonTileList");
        if (!tileListResult) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile list: {}", tileListResult.error());
        }
        if (auto srvResult = m_backgroundHorizonTileList.CreateSRV(device, m_heapManager); !srvResult) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile list SRV: {}", srvResult.error());
        }
        if (auto uavResult = m_backgroundHorizonTileList.CreateUAV(device, m_heapManager); !uavResult) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile list UAV: {}", uavResult.error());
        }

        auto drawArgsResult = m_backgroundHorizonTileDrawArgs.Initialize(
            device,
            sizeof(uint32_t) * 4ull,
            BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument,
            sizeof(uint32_t),
            "BackgroundHorizonTileDrawArgs");
        if (!drawArgsResult) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile draw args: {}", drawArgsResult.error());
        }
        if (auto uavResult = m_backgroundHorizonTileDrawArgs.CreateUAV(device, m_heapManager); !uavResult) {
            DestroyBackgroundPassResources();
            return Error("Failed to create background horizon tile draw args UAV: {}", uavResult.error());
        }
    }

    spdlog::info(
        "Background pass resources created: {}x{} scale={:.3f} main={}x{} srvIndex={} stagingSrvIndex={} horizonMask={}x{} horizonTileListCapacity={}",
        m_backgroundPassWidth,
        m_backgroundPassHeight,
        scale,
        m_width,
        m_height,
        m_backgroundPassSrv.heapIndex,
        m_backgroundPassStagingSrv.heapIndex,
        m_backgroundHorizonTileMaskWidth,
        m_backgroundHorizonTileMaskHeight,
        m_backgroundHorizonTileListCapacity);
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
