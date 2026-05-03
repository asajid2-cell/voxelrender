// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Entry Point
// =============================================================================

#include "Core/Window.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/DX12CommandQueue.h"
#include "Graphics/Renderer.h"
#include "Graphics/FarVoxelOctree.h"
#include "Graphics/SparseSurfaceGpuResources.h"
#include "Graphics/SparseVoxelGpuResources.h"
#include "Graphics/VoxelRenderBackend.h"
#include "Simulation/VoxelWorld.h"
#include "Simulation/TerrainConstants.h"
#include "Simulation/PhysicsDispatcher.h"
#include "Simulation/ChunkManager.h"
#include "Simulation/ChunkGenerationTest.h"  // INFINITE CHUNK TEST HARNESS
#include "Simulation/ChunkStressTest.h"      // STRESS TESTING FRAMEWORK
#include "Simulation/SparseBrickRequestPlanner.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Input/InputManager.h"
#include "Input/BrushController.h"
#include "UI/ImGuiBackend.h"
#include "UI/MaterialPalette.h"
#include "UI/BrushPanel.h"
#include "UI/PauseMenu.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <memory>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <deque>
#include <unordered_set>
#include <vector>

using namespace VENPOD;
using namespace VENPOD::Graphics;

// Frame synchronization
static constexpr uint32_t kFrameCount = Window::BUFFER_COUNT;
static constexpr uint64_t kFrameStageTraceLimit = 512;
static constexpr float kBrushDefaultAimDistance = 384.0f;
static constexpr float kBrushMaxInteractionDistance = 4096.0f;
static constexpr float kBrushStrokePullSpeed = 250.0f;

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    uint64_t fenceValue = 0;
};

static constexpr uint32_t kGpuTimestampCount = 4;

struct GpuTimingStats {
    bool valid = false;
    double frameMs = 0.0;
    double preRenderMs = 0.0;
    double raymarchMs = 0.0;
    double uiAndReadbackMs = 0.0;
};

static bool ReadGpuTiming(
    ID3D12Resource* readbackBuffer,
    uint64_t timestampFrequency,
    uint32_t frameIndex,
    GpuTimingStats& stats)
{
    if (!readbackBuffer || timestampFrequency == 0) {
        return false;
    }

    const uint64_t baseByte = static_cast<uint64_t>(frameIndex) * kGpuTimestampCount * sizeof(uint64_t);
    const D3D12_RANGE readRange{
        static_cast<SIZE_T>(baseByte),
        static_cast<SIZE_T>(baseByte + kGpuTimestampCount * sizeof(uint64_t))
    };

    uint8_t* mapped = nullptr;
    HRESULT hr = readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) {
        return false;
    }

    const uint64_t* timestamps = reinterpret_cast<const uint64_t*>(mapped + baseByte);
    const uint64_t t0 = timestamps[0];
    const uint64_t t1 = timestamps[1];
    const uint64_t t2 = timestamps[2];
    const uint64_t t3 = timestamps[3];

    const D3D12_RANGE writtenRange{0, 0};
    readbackBuffer->Unmap(0, &writtenRange);

    if (t0 == 0 || t1 < t0 || t2 < t1 || t3 < t2) {
        return false;
    }

    const double tickToMs = 1000.0 / static_cast<double>(timestampFrequency);
    stats.valid = true;
    stats.preRenderMs = static_cast<double>(t1 - t0) * tickToMs;
    stats.raymarchMs = static_cast<double>(t2 - t1) * tickToMs;
    stats.uiAndReadbackMs = static_cast<double>(t3 - t2) * tickToMs;
    stats.frameMs = static_cast<double>(t3 - t0) * tickToMs;
    return true;
}

struct GroundQueryMetadata {
    bool valid = false;
    glm::vec3 regionOriginWorld{0.0f};
    glm::vec3 feetWorld{0.0f};
};

struct BrushQueryMetadata {
    bool valid = false;
    glm::vec3 regionOriginWorld{0.0f};
    glm::vec3 originWorld{0.0f};
    glm::vec3 directionWorld{0.0f, 0.0f, 1.0f};
};

struct BuildStrokeState {
    bool active = false;
    float rayDistance = kBrushDefaultAimDistance;
    bool closeRampActive = false;
    float closeRampHorizontalDistance = 0.0f;
    glm::vec3 closeRampDirection{0.0f, 0.0f, 1.0f};
    bool hasLastBrushWorldPosition = false;
    glm::vec3 lastBrushWorldPosition{0.0f};
    bool hasLastBrushRayDirection = false;
    glm::vec3 lastBrushRayDirection{0.0f, 0.0f, 1.0f};
    uint32_t lastBrushMode = UINT32_MAX;
    uint32_t sweepStampsLastFrame = 0;
    bool hasStableAimDistance = false;
    float stableAimDistance = kBrushDefaultAimDistance;
    glm::vec3 stableAimWorldPosition{0.0f};
    bool hasPreviewWorldPosition = false;
    glm::vec3 previewWorldPosition{0.0f};
};

static glm::ivec3 DecodePackedNormal(uint32_t packedNormal, bool& valid) {
    valid = ((packedNormal >> 6) & 0x1u) != 0;
    return glm::ivec3(
        static_cast<int32_t>(packedNormal & 0x3u) - 1,
        static_cast<int32_t>((packedNormal >> 2) & 0x3u) - 1,
        static_cast<int32_t>((packedNormal >> 4) & 0x3u) - 1
    );
}

static uint32_t PackNormalForReadback(const glm::ivec3& normal, bool valid) {
    uint32_t packed = 0;
    packed |= static_cast<uint32_t>(std::clamp(normal.x + 1, 0, 3));
    packed |= static_cast<uint32_t>(std::clamp(normal.y + 1, 0, 3)) << 2;
    packed |= static_cast<uint32_t>(std::clamp(normal.z + 1, 0, 3)) << 4;
    packed |= (valid ? 1u : 0u) << 6;
    return packed;
}

static glm::vec3 ApplyCloseTraversalBrushFallback(
    const glm::vec3& rawBrushLocal,
    const glm::vec3& cameraLocal,
    const glm::vec3& cameraForward,
    float playerHeight,
    float playerRadius,
    float brushRadius,
    float dt,
    bool buildStroke,
    BuildStrokeState& strokeState,
    bool* adjustedOut = nullptr)
{
    if (adjustedOut) {
        *adjustedOut = false;
    }
    if (!buildStroke) {
        strokeState.closeRampActive = false;
        return rawBrushLocal;
    }

    const glm::vec3 feetLocal = cameraLocal - glm::vec3(0.0f, playerHeight, 0.0f);
    glm::vec3 flatToBrush(rawBrushLocal.x - feetLocal.x, 0.0f, rawBrushLocal.z - feetLocal.z);
    float horizontalDistance = glm::length(flatToBrush);
    const float eyeDistance = glm::length(rawBrushLocal - cameraLocal);

    glm::vec3 flatForward(cameraForward.x, 0.0f, cameraForward.z);
    if (glm::length(flatForward) < 0.001f) {
        flatForward = glm::vec3(0.0f, 0.0f, 1.0f);
    } else {
        flatForward = glm::normalize(flatForward);
    }

    glm::vec3 rampDirection = horizontalDistance > 0.001f ? flatToBrush / horizontalDistance : flatForward;
    if (glm::dot(rampDirection, flatForward) < -0.1f) {
        return rawBrushLocal;
    }

    const float closeEyeStart = std::max(10.0f, brushRadius * 2.35f + playerRadius);
    const float closeRampStart = std::max(7.0f, brushRadius * 1.95f + playerRadius);
    const float finishDistance = std::max(0.65f, playerRadius * 0.65f);
    if (eyeDistance > closeEyeStart && !strokeState.closeRampActive) {
        return rawBrushLocal;
    }

    const float forwardDot = glm::dot(rampDirection, flatForward);
    if (forwardDot < 0.35f && horizontalDistance > brushRadius * 1.5f && !strokeState.closeRampActive) {
        return rawBrushLocal;
    }

    if (!strokeState.closeRampActive) {
        strokeState.closeRampActive = true;
        strokeState.closeRampDirection = rampDirection;
        // Start only when the brush is actually close to the camera. Horizontal
        // distance alone is misleading when looking sharply up/down at cliffs.
        strokeState.closeRampHorizontalDistance = std::clamp(
            std::max(horizontalDistance, closeRampStart),
            finishDistance,
            closeRampStart);
    } else if (glm::length(strokeState.closeRampDirection) < 0.001f) {
        strokeState.closeRampDirection = rampDirection;
    }

    const float rampAdvancePerSecond = std::max(150.0f, brushRadius * 22.0f);
    const float rampAdvance = std::max(0.30f, rampAdvancePerSecond * std::max(dt, 0.0f));
    strokeState.closeRampHorizontalDistance = std::max(
        finishDistance,
        strokeState.closeRampHorizontalDistance - rampAdvance);

    glm::vec3 adjusted = rawBrushLocal;
    const glm::vec3 rampDir = glm::normalize(strokeState.closeRampDirection);
    adjusted.x = feetLocal.x + rampDir.x * strokeState.closeRampHorizontalDistance;
    adjusted.z = feetLocal.z + rampDir.z * strokeState.closeRampHorizontalDistance;

    // Move the close phase along a shallow walkable ramp. The last brush centers
    // land at foot level, which makes held painting finish as a traversable path
    // instead of a face-height blocker.
    const float nearCenterY = feetLocal.y - std::max(0.35f, brushRadius * 0.35f);
    constexpr float kTraversalRampSlope = 0.14f;  // Roughly 1 voxel up per 7 voxels forward.
    adjusted.y = nearCenterY + strokeState.closeRampHorizontalDistance * kTraversalRampSlope;

    if (adjustedOut && glm::length(adjusted - rawBrushLocal) > 0.001f) {
        *adjustedOut = true;
    }
    return adjusted;
}

// Get the executable directory to find assets (Sandbox version)
static std::filesystem::path GetExecutableDirectorySandbox() {
    // Try to get executable path from SDL
    const char* basePath = SDL_GetBasePath();
    if (basePath) {
        std::filesystem::path path(basePath);
        // SDL3 manages the static string, no SDL_free needed
        return path;
    }
    return std::filesystem::current_path();
}

static uint32_t ReadUIntEnv(const char* name, uint32_t fallbackValue) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallbackValue;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed > UINT32_MAX) {
        return fallbackValue;
    }
    return static_cast<uint32_t>(parsed);
}


int RunSandbox(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // DEBUG MODE: When true, bypass streaming and use a fixed 2x2 static
    // chunk layout copied into the 256x128x256 voxel buffer each frame.
    // This isolates copy/origin bugs from the infinite chunk streaming logic.
    const bool useStaticChunkLayout = std::getenv("VENPOD_STATIC_CHUNKS") != nullptr;
    const bool disablePhysics = std::getenv("VENPOD_DISABLE_PHYSICS") != nullptr;
    const bool enableInfinitePhysics = std::getenv("VENPOD_DISABLE_INFINITE_PHYSICS") == nullptr;
    const bool enableDiagnostics = std::getenv("VENPOD_DIAGNOSTICS") != nullptr;
    const bool enableRuntimeLog = enableDiagnostics || std::getenv("VENPOD_LOG_FILE") != nullptr;
    const bool enableD3DDebug = std::getenv("VENPOD_D3D_DEBUG") != nullptr;
    const bool allowInternalTestModes = std::getenv("VENPOD_ENABLE_TEST_MODES") != nullptr;
    const bool enableBoundaryTest =
        allowInternalTestModes && std::getenv("VENPOD_BOUNDARY_TEST") != nullptr;
    const bool enableFarSVO = std::getenv("VENPOD_DISABLE_FAR_SVO") == nullptr;
    const bool highDensityDenseWindow = std::getenv("VENPOD_HIGH_DENSITY") != nullptr;
    const bool lowMemoryDenseWindow =
        std::getenv("VENPOD_LOW_MEMORY_DENSE") != nullptr && !highDensityDenseWindow;
    const bool allowExperimentalSparse = std::getenv("VENPOD_ENABLE_EXPERIMENTAL_SPARSE") != nullptr;
    const VoxelRenderBackend environmentRenderBackend = RequestedVoxelRenderBackendFromEnvironment();
    const VoxelRenderBackend requestedRenderBackend =
        (environmentRenderBackend == VoxelRenderBackend::SparseBrick && !allowExperimentalSparse)
            ? VoxelRenderBackend::DenseLegacy
            : environmentRenderBackend;
    const VoxelRenderBackend activeRenderBackend = VoxelRenderBackend::DenseLegacy;
    const bool sparseBackendRequested = requestedRenderBackend == VoxelRenderBackend::SparseBrick;
    const bool enableSparseRaymarch = sparseBackendRequested && std::getenv("VENPOD_SPARSE_RAYMARCH") != nullptr;
    const bool enableSparseOnlyRaymarch = enableSparseRaymarch && std::getenv("VENPOD_SPARSE_ONLY") != nullptr;
    const bool enableSparseNearBinding = ReadUIntEnv("VENPOD_SPARSE_BIND_NEAR", 1u) != 0u;
    const uint32_t sparseNearBindingMask = ReadUIntEnv("VENPOD_SPARSE_BIND_MASK", 0xFFFu);
    const uint32_t sparseRaymarchWindowVoxels =
        std::max(64u, ReadUIntEnv("VENPOD_SPARSE_RAY_WINDOW", 64u));
    const bool enableUnsafeSparseFullRaymarch =
        ReadUIntEnv("VENPOD_SPARSE_FULL_RAYMARCH", 0u) != 0u;
    const bool sparseRuntimeTestMode =
        enableSparseOnlyRaymarch && std::getenv("VENPOD_SPARSE_LEGACY_RUNTIME") == nullptr;
    const bool disableRuntimePhysics = disablePhysics || sparseRuntimeTestMode;
    uint32_t sparseRaymarchDebugMode = enableSparseRaymarch
        ? ReadUIntEnv("VENPOD_SPARSE_DEBUG_MODE", 0u)
        : 0u;
    if (enableSparseOnlyRaymarch &&
        !enableUnsafeSparseFullRaymarch &&
        sparseRaymarchDebugMode == 0u) {
        // The temporary full-screen sparse DDA is intentionally gated while the
        // brick renderer is being refactored; more than one sparse sample per
        // pixel can still saturate the GPU on startup. Mode 45 keeps sparse
        // resource binding and first-sample validation active without freezing
        // the machine.
        sparseRaymarchDebugMode = 45u;
    }

    if (enableRuntimeLog) {
        auto logPath = GetExecutableDirectorySandbox() / "venpod_runtime.log";
        auto fileLogger = spdlog::basic_logger_mt("venpod_file", logPath.string(), true);
        spdlog::set_default_logger(fileLogger);
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("  Log path: {}", logPath.string());
    }

    spdlog::set_level(enableDiagnostics ? spdlog::level::debug :
        (enableRuntimeLog ? spdlog::level::info : spdlog::level::warn));
    spdlog::info("===========================================");
    spdlog::info("  VENPOD - Voxel Physics Engine v0.1.0");
    spdlog::info("  Target: 100M+ Active Voxels @ 60 FPS");
    spdlog::info("  Static chunks: {} | Physics disabled: {} | Infinite physics: {} | Diagnostics: {} | Boundary test: {} | Far SVO: {}",
        useStaticChunkLayout ? "yes" : "no",
        disableRuntimePhysics ? "yes" : "no",
        enableInfinitePhysics ? "yes" : "no",
        enableDiagnostics ? "yes" : "no",
        enableBoundaryTest ? "yes" : "no",
        enableFarSVO ? "yes" : "no");
    spdlog::info("  Render backend requested: {} | active: {}{}",
        ToString(requestedRenderBackend),
        ToString(activeRenderBackend),
        sparseBackendRequested ? " (sparse backend scaffold active; dense renderer still displays final frame)" : "");
    if (environmentRenderBackend == VoxelRenderBackend::SparseBrick && !allowExperimentalSparse) {
        spdlog::warn("  Ignoring VENPOD_RENDER_BACKEND=sparse because VENPOD_ENABLE_EXPERIMENTAL_SPARSE is not set");
    }
    if (!allowInternalTestModes && std::getenv("VENPOD_BOUNDARY_TEST") != nullptr) {
        spdlog::warn("  Ignoring VENPOD_BOUNDARY_TEST because VENPOD_ENABLE_TEST_MODES is not set");
    }
    spdlog::info("  Sparse raymarch visual path: {}", enableSparseRaymarch ? "enabled" : "disabled");
    if (enableSparseRaymarch) {
        spdlog::info("  Sparse raymarch debug mode: {} | sparse only: {}",
            sparseRaymarchDebugMode,
            enableSparseOnlyRaymarch ? "yes" : "no");
        spdlog::info("  Sparse runtime test mode: {}{}",
            sparseRuntimeTestMode ? "enabled" : "disabled",
            sparseRuntimeTestMode ? " (legacy dense streaming bypassed)" : "");
    }
    spdlog::info("===========================================");

    // Initialize DX12 Device
    auto device = std::make_unique<DX12Device>();
    DeviceConfig deviceConfig;
    deviceConfig.enableDebugLayer = enableD3DDebug;
    deviceConfig.enableGPUValidation = enableD3DDebug;

    auto deviceResult = device->Initialize(deviceConfig);
    if (deviceResult.IsErr()) {
        spdlog::critical("Failed to initialize DX12 device: {}", deviceResult.Error());
        return 1;
    }

    // Initialize Command Queue
    auto commandQueue = std::make_unique<DX12CommandQueue>();
    auto queueResult = commandQueue->Initialize(device->GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    if (queueResult.IsErr()) {
        spdlog::critical("Failed to initialize command queue: {}", queueResult.Error());
        return 1;
    }

    // =============================================================================
    // Continue with normal initialization
    // =============================================================================

    // Initialize Window
    auto window = std::make_unique<Window>();
    WindowConfig windowConfig;
    windowConfig.title = "VENPOD - Voxel Physics Engine";
    windowConfig.width = 1920;
    windowConfig.height = 1080;
    windowConfig.vsync = true;

    auto windowResult = window->Initialize(windowConfig, device.get());
    if (windowResult.IsErr()) {
        spdlog::critical("Failed to initialize window: {}", windowResult.Error());
        return 1;
    }

    auto swapChainResult = window->InitializeSwapChain(device.get(), commandQueue.get());
    if (swapChainResult.IsErr()) {
        spdlog::critical("Failed to initialize swap chain: {}", swapChainResult.Error());
        return 1;
    }

    // Initialize Renderer
    auto renderer = std::make_unique<Renderer>();
    RendererConfig rendererConfig;
    rendererConfig.cbvSrvUavDescriptorCount = 32768;  // Larger stream window needs room for chunk SRV/UAV descriptors.
    rendererConfig.rtvDescriptorCount = 32;
    rendererConfig.dsvDescriptorCount = 8;
    rendererConfig.debugShaders = enableDiagnostics;

    // Find shader path
    std::filesystem::path exeDir = GetExecutableDirectorySandbox();
    std::filesystem::path shaderPath = exeDir / "assets" / "shaders";

    // Try a few common locations
    if (!std::filesystem::exists(shaderPath)) {
        shaderPath = exeDir.parent_path() / "assets" / "shaders";
    }
    if (!std::filesystem::exists(shaderPath)) {
        shaderPath = std::filesystem::current_path() / "assets" / "shaders";
    }
    if (!std::filesystem::exists(shaderPath)) {
        // Try relative to source (for development)
        shaderPath = std::filesystem::current_path().parent_path() / "assets" / "shaders";
    }

    spdlog::info("Shader path: {}", shaderPath.string());
    rendererConfig.shaderPath = shaderPath;

    auto rendererResult = renderer->Initialize(*device, *commandQueue, *window, rendererConfig);
    if (!rendererResult) {
        spdlog::critical("Failed to initialize renderer: {}", rendererResult.error());
        return 1;
    }

    SparseVoxelGpuResources sparseGpuResources;
    SparseSurfaceGpuResources sparseSurfaceGpuResources;
    Simulation::SparseVoxelWorld sparseVoxelWorld;
    bool sparseVoxelWorldReady = false;
    Simulation::SparseVoxelWorldConfig sparseWorldConfig;
    sparseWorldConfig.maxBrickPages = ReadUIntEnv("VENPOD_SPARSE_MAX_PAGES", sparseWorldConfig.maxBrickPages);
    sparseWorldConfig.pageTableCapacity = ReadUIntEnv("VENPOD_SPARSE_PAGE_TABLE", sparseWorldConfig.pageTableCapacity);
    sparseWorldConfig.seed = ReadUIntEnv("VENPOD_SPARSE_SEED", sparseWorldConfig.seed);
    const uint32_t sparseGenerationBudget = ReadUIntEnv("VENPOD_SPARSE_GENERATION_BUDGET", 4u);
    const uint32_t sparseUploadBudget = ReadUIntEnv("VENPOD_SPARSE_UPLOAD_BUDGET", 8u);
    const uint32_t sparseFeedbackGenerationBudget =
        ReadUIntEnv("VENPOD_SPARSE_FEEDBACK_GENERATION_BUDGET", std::max(8u, sparseGenerationBudget));
    const uint32_t sparseFeedbackUploadBudget =
        ReadUIntEnv("VENPOD_SPARSE_FEEDBACK_UPLOAD_BUDGET", std::max(16u, sparseUploadBudget));
    const uint32_t sparseBootstrapGenerationBudget =
        ReadUIntEnv("VENPOD_SPARSE_BOOTSTRAP_GENERATION_BUDGET", 16u);
    const uint32_t sparseBootstrapUploadBudget =
        ReadUIntEnv("VENPOD_SPARSE_BOOTSTRAP_UPLOAD_BUDGET", 24u);
    const uint32_t sparseBootstrapResidentTarget = std::min(
        ReadUIntEnv("VENPOD_SPARSE_BOOTSTRAP_RESIDENT_TARGET", std::min(192u, sparseWorldConfig.maxBrickPages / 2u)),
        sparseWorldConfig.maxBrickPages);
    const uint32_t sparseRequestRadiusXz = ReadUIntEnv("VENPOD_SPARSE_REQUEST_RADIUS_XZ", 2u);
    const uint32_t sparseRequestRadiusY = ReadUIntEnv("VENPOD_SPARSE_REQUEST_RADIUS_Y", 1u);
    const uint32_t sparseTrimRadiusXz = ReadUIntEnv(
        "VENPOD_SPARSE_TRIM_RADIUS_XZ",
        sparseRequestRadiusXz + ReadUIntEnv("VENPOD_SPARSE_PREFETCH_BRICKS", 2u) + 3u);
    const uint32_t sparseTrimRadiusY = ReadUIntEnv(
        "VENPOD_SPARSE_TRIM_RADIUS_Y",
        sparseRequestRadiusY + 2u);
    const uint32_t sparseTrimBudget = ReadUIntEnv("VENPOD_SPARSE_TRIM_BUDGET", 8u);
    const uint32_t sparsePressureTrimBudget =
        ReadUIntEnv("VENPOD_SPARSE_PRESSURE_TRIM_BUDGET", sparseTrimBudget);
    const uint32_t sparseInvalidationBudget = ReadUIntEnv("VENPOD_SPARSE_INVALIDATION_BUDGET", 16u);
    const uint32_t sparsePageTablePublishBudget =
        ReadUIntEnv("VENPOD_SPARSE_PAGE_TABLE_PUBLISH_BUDGET", sparseInvalidationBudget);
    const uint32_t sparseCollisionShellRadiusXz = ReadUIntEnv("VENPOD_SPARSE_COLLISION_SHELL_XZ", 1u);
    const uint32_t sparseCollisionShellRadiusY = ReadUIntEnv("VENPOD_SPARSE_COLLISION_SHELL_Y", 1u);
    const uint32_t sparseNewRequestBudget = ReadUIntEnv("VENPOD_SPARSE_NEW_REQUEST_BUDGET", 16u);
    const uint32_t sparseTotalRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_TOTAL_REQUEST_BUDGET", sparseNewRequestBudget * 2u);
    const uint32_t sparseSpeculativeRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_SPECULATIVE_REQUEST_BUDGET", std::max(1u, sparseNewRequestBudget / 2u));
    const uint32_t sparseSpeculativeBackpressureGenQueue =
        ReadUIntEnv("VENPOD_SPARSE_SPEC_BACKPRESSURE_GEN_QUEUE",
            std::max(32u, sparseWorldConfig.maxBrickPages / 8u));
    const uint32_t sparseSpeculativeBackpressureMissPending =
        ReadUIntEnv("VENPOD_SPARSE_SPEC_BACKPRESSURE_MISS_PENDING", 32u);
    const uint32_t sparseVisibleRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_VISIBLE_REQUEST_BUDGET", sparseNewRequestBudget);
    const uint32_t sparseCollisionRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_COLLISION_REQUEST_BUDGET", sparseNewRequestBudget);
    const uint32_t sparseReplacementBudget =
        ReadUIntEnv("VENPOD_SPARSE_REPLACEMENT_BUDGET", std::max(1u, sparseVisibleRequestBudget));
    const uint32_t sparseMinFreePages = ReadUIntEnv(
        "VENPOD_SPARSE_MIN_FREE_PAGES",
        std::max(16u, sparseWorldConfig.maxBrickPages / 8u));
    const uint32_t sparseTrimStartResident = std::min(
        ReadUIntEnv(
            "VENPOD_SPARSE_TRIM_START_RESIDENT",
            sparseWorldConfig.maxBrickPages > sparseMinFreePages
                ? sparseWorldConfig.maxBrickPages - sparseMinFreePages
                : sparseWorldConfig.maxBrickPages),
        sparseWorldConfig.maxBrickPages);
    const uint32_t sparseRayPrefetchDistance = ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_DISTANCE", 192u);
    const uint32_t sparseRayPrefetchStride = std::max(4u, ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_STRIDE", 16u));
    const uint32_t sparseRayPrefetchMaxRequests = ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS", 16u);
    const uint32_t sparseViewPrefetchRayGrid = ReadUIntEnv("VENPOD_SPARSE_VIEW_PREFETCH_RAYS", 3u);
    const uint32_t sparsePredictivePrefetchMs = ReadUIntEnv("VENPOD_SPARSE_PREDICTIVE_PREFETCH_MS", 250u);
    const bool enableSparseMissFeedback =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK", 0u) != 0u;
    const uint32_t sparseMissFeedbackMaxRecords = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_RECORDS", 256u);
    const uint32_t sparseMissFeedbackRayGrid = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_RAYS", 5u);
    const uint32_t sparseMissFeedbackDistance = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_DISTANCE", 256u);
    const uint32_t sparseMissFeedbackStride = std::max(4u, ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_STRIDE", 16u));
    const uint32_t sparseMissFeedbackInterval = std::max(1u, ReadUIntEnv(
        "VENPOD_SPARSE_MISS_FEEDBACK_INTERVAL",
        30u));
    const bool enableSparseSurfaceUpload =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_UPLOAD", 1u) != 0u;
    const bool enableSparseSurfaceRaster =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_RASTER", 1u) != 0u;
    uint32_t sparseSurfaceUploadedSerial = 0;
    uint32_t sparseSurfaceUploadRetriesLastFrame = 0;
    uint32_t sparseSurfaceRasterFacesLastFrame = 0;
    Simulation::SparseClipmapConfig sparseClipmapConfig;
    sparseClipmapConfig.enabled =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_MID_CLIPMAP", 1u) != 0u;
    sparseClipmapConfig.startDistance =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_START", 520u));
    sparseClipmapConfig.endDistance =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_END", 4200u));
    sparseClipmapConfig.minCellSize =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_CELL", 16u));
    sparseClipmapConfig.nearExitPadding =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_NEAR_PADDING", 12u));
    sparseClipmapConfig.ringCount = ReadUIntEnv("VENPOD_SPARSE_MID_RINGS", 4u);
    sparseClipmapConfig.tileRadius = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_RADIUS", 2u);
    sparseClipmapConfig.tileSampleSide = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_SIDE", 33u);
    sparseClipmapConfig.maxTiles = ReadUIntEnv("VENPOD_SPARSE_MID_MAX_TILES", 128u);
    sparseClipmapConfig.voxelClipmapEnabled =
        ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_CLIPMAP", 1u) != 0u;
    sparseClipmapConfig.voxelBrickRadiusXz = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ", 2u);
    sparseClipmapConfig.voxelBrickRadiusY = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_RADIUS_Y", 1u);
    sparseClipmapConfig.maxVoxelBricks = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS", 128u);
    sparseClipmapConfig.seed = sparseWorldConfig.seed;
    const uint32_t sparseMidClipmapTileBudget = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_BUDGET", 4u);
    Simulation::SparseClipmapPolicy sparseClipmapPolicy(sparseClipmapConfig);
    Simulation::SparseClipmapTileCache sparseClipmapTileCache;
    bool sparseClipmapTileCacheReady = false;
    uint32_t sparseMidClipmapUploadedHeightSerial = 0;
    uint32_t sparseMidClipmapUploadedVoxelSerial = 0;
    uint32_t sparseMidClipmapUploadRetriesLastFrame = 0;
    if (sparseBackendRequested) {
        sparseClipmapTileCacheReady = sparseClipmapTileCache.Initialize(sparseClipmapPolicy.Config());
        spdlog::info(
            "Sparse mid clipmap {}: start={:.0f} end={:.0f} cell={:.0f} rings={} tileRadius={} tileSide={} maxTiles={} budget={}",
            sparseClipmapPolicy.IsEnabled() ? "enabled" : "disabled",
            sparseClipmapPolicy.Config().startDistance,
            sparseClipmapPolicy.Config().endDistance,
            sparseClipmapPolicy.Config().minCellSize,
            sparseClipmapPolicy.Config().ringCount,
            sparseClipmapPolicy.Config().tileRadius,
            sparseClipmapPolicy.Config().tileSampleSide,
            sparseClipmapPolicy.Config().maxTiles,
            sparseMidClipmapTileBudget);
    }
    Simulation::SparseBrickRequestPlanner sparseRequestPlanner({
        sparseRequestRadiusXz,
        sparseRequestRadiusY,
        ReadUIntEnv("VENPOD_SPARSE_PREFETCH_BRICKS", 2u),
        ReadUIntEnv("VENPOD_SPARSE_MAX_REQUESTS", 128u)
    });
    Simulation::BrickCoord lastSparseRequestCenter{
        INT32_MIN,
        INT32_MIN,
        INT32_MIN
    };
    bool sparseGpuPageTableResetPending = false;

    if (sparseBackendRequested) {
        SparseVoxelGpuConfig sparseConfig;
        sparseConfig.maxBrickPages = sparseWorldConfig.maxBrickPages;
        sparseConfig.pageTableCapacity = sparseWorldConfig.pageTableCapacity;
        sparseConfig.uploadBytesPerSlot = ReadUIntEnv("VENPOD_SPARSE_UPLOAD_SLOT_BYTES", sparseConfig.uploadBytesPerSlot);
        sparseConfig.missFeedbackMaxRecords = sparseMissFeedbackMaxRecords;
        sparseConfig.midClipmapMaxTiles = sparseClipmapPolicy.Config().maxTiles;
        sparseConfig.midClipmapTileSampleSide = sparseClipmapPolicy.Config().tileSampleSide;
        sparseConfig.midVoxelClipmapMaxBricks = sparseClipmapPolicy.Config().maxVoxelBricks;

        sparseVoxelWorldReady = sparseVoxelWorld.Initialize(sparseWorldConfig);
        if (!sparseVoxelWorldReady) {
            spdlog::error("Sparse CPU world initialization failed");
        } else {
            spdlog::info(
                "Sparse CPU world scaffold initialized: pages={} table={} genBudget={} uploadBudget={} requestRadius={}x{}",
                sparseWorldConfig.maxBrickPages,
                sparseWorldConfig.pageTableCapacity,
                sparseGenerationBudget,
                sparseUploadBudget,
                sparseRequestRadiusXz,
                sparseRequestRadiusY);
        }

        auto sparseGpuResult = sparseGpuResources.Initialize(
            device->GetDevice(),
            renderer->GetHeapManager(),
            sparseConfig);
        if (!sparseGpuResult) {
            spdlog::error("Sparse GPU resource initialization failed: {}", sparseGpuResult.error());
        } else {
            spdlog::info("Sparse backend GPU resource scaffold is active; rendering still uses dense legacy fallback");
            sparseGpuPageTableResetPending = true;
        }

        if (enableSparseSurfaceUpload) {
            SparseSurfaceGpuConfig surfaceConfig;
            surfaceConfig.maxFaces = ReadUIntEnv("VENPOD_SPARSE_SURFACE_MAX_FACES", surfaceConfig.maxFaces);
            surfaceConfig.maxBrickRanges = ReadUIntEnv("VENPOD_SPARSE_SURFACE_MAX_RANGES", surfaceConfig.maxBrickRanges);
            surfaceConfig.uploadBytesPerSlot =
                ReadUIntEnv("VENPOD_SPARSE_SURFACE_UPLOAD_SLOT_BYTES", surfaceConfig.uploadBytesPerSlot);
            auto surfaceGpuResult = sparseSurfaceGpuResources.Initialize(
                device->GetDevice(),
                renderer->GetHeapManager(),
                surfaceConfig);
            if (!surfaceGpuResult) {
                spdlog::error("Sparse surface GPU resource initialization failed: {}", surfaceGpuResult.error());
            } else {
                spdlog::info(
                    "Sparse surface GPU buffers initialized: faces={} ranges={} uploadSlotMB={:.2f}",
                    surfaceConfig.maxFaces,
                    surfaceConfig.maxBrickRanges,
                    static_cast<double>(surfaceConfig.uploadBytesPerSlot) / (1024.0 * 1024.0));
            }
        } else if (sparseBackendRequested) {
            spdlog::info("Sparse surface GPU upload disabled by VENPOD_SPARSE_SURFACE_UPLOAD=0");
        }
        spdlog::info("Sparse surface raster path: {}", enableSparseSurfaceRaster ? "enabled" : "disabled");
    }

    FarVoxelOctree farVoxelOctree;
    Renderer::SparseFarField sparseFarField = {};
    if (enableFarSVO) {
        FarVoxelOctreeConfig farConfig;
        farVoxelOctree.BeginAsyncLoad(farConfig);
        spdlog::info("Far sparse voxel octree async load started");
    }

    // =============================================================================
    // CHUNK GENERATION TESTS (DISABLED - causes descriptor heap conflicts)
    // =============================================================================
    // CRITICAL FIX: Tests allocate/free descriptors that get recycled by main app,
    // causing descriptor handle collisions (TEXTURE2D vs BUFFER mismatch errors).
    // The tests work fine in isolation but pollute the heap for production use.
    //
    // To re-enable tests (for development only), set runTests = true
    // =============================================================================
    bool runTests = false;  // Disabled by default to avoid descriptor conflicts

    if (runTests) {
        spdlog::info("\n");
        spdlog::info("RUNNING INFINITE CHUNK GENERATION TESTS");
        spdlog::info("");

        bool testsPass = Simulation::ChunkGenerationTest::RunAllTests(*device, *commandQueue, renderer->GetHeapManager());

        if (!testsPass) {
            spdlog::critical("CHUNK GENERATION TESTS FAILED!");
            spdlog::critical("   Fix the issues above before proceeding.");
            spdlog::critical("   Press ENTER to exit...");
            std::cin.get();
            return 1;
        }

        spdlog::info("");
        spdlog::info("All chunk tests passed! Continuing with stress tests...");
        spdlog::info("");
        // TEST-ONLY MODE: Exit after generation tests to avoid descriptor reuse
        // interfering with the main game runtime.
        return 0;
    } else {
        spdlog::info("Skipping chunk generation tests (disabled to prevent descriptor conflicts)");
    }

    // =============================================================================
    // STRESS TESTS (Optional - run with command line flag --stress-test)
    // =============================================================================
    bool runStressTests = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stress-test") == 0 || strcmp(argv[i], "-s") == 0) {
            runStressTests = true;
            break;
        }
    }

    if (runStressTests) {
        spdlog::info("");
        spdlog::info("========================================================");
        spdlog::info("       RUNNING STRESS TESTS (--stress-test flag)");
        spdlog::info("========================================================");
        spdlog::info("");

        Simulation::StressTestConfig stressConfig;
        stressConfig.intensity = 2;        // Normal intensity
        stressConfig.maxDurationMs = 30000; // 30 second max per test
        stressConfig.cycleIterations = 100; // Reduced for faster testing
        stressConfig.verbose = true;

        bool stressTestsPass = Simulation::ChunkStressTest::RunAllStressTests(
            *device, *commandQueue, renderer->GetHeapManager(), stressConfig);

        if (!stressTestsPass) {
            spdlog::warn("Some stress tests failed - check logs above");
            spdlog::info("Press ENTER to continue anyway, or Ctrl+C to exit...");
            std::cin.get();
        } else {
            spdlog::info("All stress tests passed!");
        }
    } else {
        spdlog::info("Skipping stress tests (use --stress-test or -s flag to run)");
    }

    spdlog::info("");
    spdlog::info("Continuing with normal initialization...");
    spdlog::info("");
    // =============================================================================

    // Create per-frame resources (triple buffering) - MUST BE BEFORE VOXEL INIT
    FrameContext frameContexts[kFrameCount];
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        HRESULT hr = device->GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&frameContexts[i].commandAllocator)
        );
        if (FAILED(hr)) {
            spdlog::critical("Failed to create command allocator for frame {}", i);
            return 1;
        }
    }

    ComPtr<ID3D12QueryHeap> gpuTimestampHeap;
    ComPtr<ID3D12Resource> gpuTimestampReadback;
    uint64_t gpuTimestampFrequency = 0;
    GpuTimingStats gpuTiming = {};
    {
        HRESULT freqHr = commandQueue->GetCommandQueue()->GetTimestampFrequency(&gpuTimestampFrequency);
        if (FAILED(freqHr) || gpuTimestampFrequency == 0) {
            spdlog::warn("GPU timestamp frequency unavailable; GPU timing overlay disabled");
        } else {
            D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
            queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queryHeapDesc.Count = kFrameCount * kGpuTimestampCount;
            queryHeapDesc.NodeMask = 0;
            HRESULT queryHr = device->GetDevice()->CreateQueryHeap(
                &queryHeapDesc,
                IID_PPV_ARGS(&gpuTimestampHeap));
            if (FAILED(queryHr)) {
                spdlog::warn("Failed to create GPU timestamp query heap: 0x{:08X}", static_cast<unsigned int>(queryHr));
                gpuTimestampFrequency = 0;
            } else {
                D3D12_HEAP_PROPERTIES heapProps = {};
                heapProps.Type = D3D12_HEAP_TYPE_READBACK;
                heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                heapProps.CreationNodeMask = 1;
                heapProps.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC bufferDesc = {};
                bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufferDesc.Alignment = 0;
                bufferDesc.Width = static_cast<UINT64>(kFrameCount) * kGpuTimestampCount * sizeof(uint64_t);
                bufferDesc.Height = 1;
                bufferDesc.DepthOrArraySize = 1;
                bufferDesc.MipLevels = 1;
                bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
                bufferDesc.SampleDesc.Count = 1;
                bufferDesc.SampleDesc.Quality = 0;
                bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

                HRESULT bufferHr = device->GetDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &bufferDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&gpuTimestampReadback));
                if (FAILED(bufferHr)) {
                    spdlog::warn("Failed to create GPU timestamp readback buffer: 0x{:08X}", static_cast<unsigned int>(bufferHr));
                    gpuTimestampHeap.Reset();
                    gpuTimestampFrequency = 0;
                } else {
                    gpuTimestampHeap->SetName(L"VENPOD_GPU_TimestampHeap");
                    gpuTimestampReadback->SetName(L"VENPOD_GPU_TimestampReadback");
                    spdlog::info("GPU timestamp timing enabled: {} Hz", gpuTimestampFrequency);
                }
            }
        }
    }

    // Initialize VoxelWorld
    auto voxelWorld = std::make_unique<Simulation::VoxelWorld>();
    Simulation::VoxelWorldConfig voxelConfig;
    // The dense streaming/copy path assumes the TerrainConstants render-window
    // dimensions. Those constants are currently a temporary dev harness size;
    // high effective render distance moves to sparse bricks and clipmaps.
    voxelConfig.gridSizeX = Simulation::RENDER_BUFFER_VOXELS_X;
    voxelConfig.gridSizeY = Simulation::RENDER_BUFFER_VOXELS_Y;
    voxelConfig.gridSizeZ = Simulation::RENDER_BUFFER_VOXELS_Z;
    if (lowMemoryDenseWindow) {
        voxelConfig.gridSizeX = ReadUIntEnv("VENPOD_DENSE_GRID_X", 832u);
        voxelConfig.gridSizeY = ReadUIntEnv("VENPOD_DENSE_GRID_Y", 320u);
        voxelConfig.gridSizeZ = ReadUIntEnv("VENPOD_DENSE_GRID_Z", 832u);
        spdlog::warn(
            "Low-memory dense profile: {}x{}x{}; dense copy constants still expect {}x{}x{}, so visual coverage is partial",
            voxelConfig.gridSizeX,
            voxelConfig.gridSizeY,
            voxelConfig.gridSizeZ,
            Simulation::RENDER_BUFFER_VOXELS_X,
            Simulation::RENDER_BUFFER_VOXELS_Y,
            Simulation::RENDER_BUFFER_VOXELS_Z);
    }
    if (sparseRuntimeTestMode) {
        voxelWorld->SetUseInfiniteChunks(false);
        voxelConfig.gridSizeX = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_X", 512u);
        voxelConfig.gridSizeY = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_Y", 384u);
        voxelConfig.gridSizeZ = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_Z", 512u);
        spdlog::info(
            "Sparse runtime test mode: dense VoxelWorld reduced to {}x{}x{} and infinite chunk streaming disabled",
            voxelConfig.gridSizeX,
            voxelConfig.gridSizeY,
            voxelConfig.gridSizeZ);
    }

    // Need a one-time command list for upload
    ComPtr<ID3D12GraphicsCommandList> initCommandList;
    HRESULT initHr = device->GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        frameContexts[0].commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&initCommandList)
    );
    if (FAILED(initHr)) {
        spdlog::critical("Failed to create initialization command list");
        return 1;
    }

    auto voxelResult = voxelWorld->Initialize(
        device->GetDevice(),
        initCommandList.Get(),
        renderer->GetHeapManager(),
        voxelConfig
    );
    if (!voxelResult) {
        spdlog::critical("Failed to initialize voxel world: {}", voxelResult.error());
        return 1;
    }

    // Initialize ChunkManager for sparse chunk-based optimization
    auto chunkManager = std::make_unique<Simulation::ChunkManager>();
    auto chunkResult = chunkManager->Initialize(
        device->GetDevice(),
        renderer->GetHeapManager(),
        voxelConfig.gridSizeX,
        voxelConfig.gridSizeY,
        voxelConfig.gridSizeZ
    );
    if (!chunkResult) {
        spdlog::critical("Failed to initialize chunk manager: {}", chunkResult.error());
        return 1;
    }

    // Initialize PhysicsDispatcher
    auto physicsDispatcher = std::make_unique<Simulation::PhysicsDispatcher>();
    auto physicsResult = physicsDispatcher->Initialize(
        device->GetDevice(),
        renderer->GetShaderCompiler(),
        renderer->GetHeapManager(),
        shaderPath
    );
    if (!physicsResult) {
        spdlog::critical("Failed to initialize physics dispatcher: {}", physicsResult.error());
        return 1;
    }

    // Initialize voxels with test pattern (ONLY if NOT using infinite chunks)
    // When using infinite chunks, the chunk system manages terrain generation
    if (!voxelWorld->IsUsingInfiniteChunks() && !sparseRuntimeTestMode) {
        physicsDispatcher->DispatchInitialize(initCommandList.Get(), *voxelWorld, 12345);

        // CRITICAL: Swap buffers so the initialized data becomes the "read" buffer
        // DispatchInitialize writes to the WRITE buffer, so we need to swap
        // to make that data available as the READ buffer for rendering
        voxelWorld->SwapBuffers();
        spdlog::info("Initialized 256^3 voxel grid with procedural terrain (CS_Initialize)");
    } else if (sparseRuntimeTestMode) {
        spdlog::info("Sparse runtime test mode: skipping dense CS_Initialize; sparse pages are authoritative");
    } else {
        // Infinite streaming now gates dense-buffer reads through a tiny
        // per-chunk valid mask. Startup only needs the masks cleared on the
        // first active-region fill; clearing both multi-GB voxel buffers here
        // caused a black-screen-scale launch stall on 8 GB GPUs.
        spdlog::info("Infinite chunk mode: skipping dense voxel-buffer clear; chunk-valid masks gate startup reads");
    }

    initCommandList->Close();
    commandQueue->ExecuteCommandList(initCommandList.Get());
    commandQueue->Flush();  // Wait for initialization to complete

    // =============================================================================
    // STATIC 2x2 CHUNK LAYOUT (DEBUG MODE)
    // Pre-generate four fixed infinite chunks at coordinates (0,0,0), (1,0,0),
    // (0,0,1), (1,0,1) using the infinite chunk manager inside VoxelWorld.
    // These will be copied into the 256x128x256 voxel buffer each frame when
    // useStaticChunkLayout is enabled, bypassing streaming logic entirely.
    // =============================================================================
    if (useStaticChunkLayout) {
        auto* infiniteChunkManager = voxelWorld->GetChunkManager();
        if (!infiniteChunkManager) {
            spdlog::critical("Static chunk layout enabled but VoxelWorld has no InfiniteChunkManager");
            return 1;
        }

        ComPtr<ID3D12CommandAllocator> staticCmdAllocator;
        HRESULT staticHr = device->GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&staticCmdAllocator)
        );
        if (FAILED(staticHr)) {
            spdlog::critical("Failed to create command allocator for static chunk layout");
            return 1;
        }

        ComPtr<ID3D12GraphicsCommandList> staticCmdList;
        staticHr = device->GetDevice()->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            staticCmdAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&staticCmdList)
        );
        if (FAILED(staticHr)) {
            spdlog::critical("Failed to create command list for static chunk layout");
            return 1;
        }

        Simulation::ChunkCoord staticCoords[] = {
            {0, 0, 0},
            {1, 0, 0},
            {0, 0, 1},
            {1, 0, 1},
        };

        for (const auto& coord : staticCoords) {
            auto genResult = infiniteChunkManager->ForceGenerateChunk(
                device->GetDevice(),
                staticCmdList.Get(),
                coord
            );
            if (!genResult) {
                spdlog::critical("Static chunk layout: failed to generate chunk [{},{},{}]: {}",
                    coord.x, coord.y, coord.z, genResult.error());
                return 1;
            }
        }

        staticCmdList->Close();
        ID3D12CommandList* staticLists[] = { staticCmdList.Get() };
        commandQueue->GetCommandQueue()->ExecuteCommandLists(1, staticLists);
        commandQueue->Flush();

        spdlog::info("Static chunk layout: pre-generated 2x2 chunk patch at origin");

        // Treat world as a static 256^3 grid for the rest of the runtime so physics
        // and chunk scan use the non-infinite path while we debug copy/origin.
        voxelWorld->SetUseInfiniteChunks(false);
    }

    // Create command list
    ComPtr<ID3D12GraphicsCommandList> commandList;
    HRESULT hr = device->GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        frameContexts[0].commandAllocator.Get(),
        nullptr,  // No initial PSO
        IID_PPV_ARGS(&commandList)
    );
    if (FAILED(hr)) {
        spdlog::critical("Failed to create command list");
        return 1;
    }
    commandList->Close();  // Start closed, will be reset each frame

    // Initialize Input Manager with window reference
    Input::InputManager inputManager;
    inputManager.Initialize(windowConfig.width, windowConfig.height, window->GetSDLWindow());

    // Initialize Brush Controller
    Input::BrushController brushController;
    brushController.Initialize();
    brushController.SetGridBounds(
        static_cast<float>(voxelWorld->GetGridSizeX()),
        static_cast<float>(voxelWorld->GetGridSizeY()),
        static_cast<float>(voxelWorld->GetGridSizeZ())
    );

    // =============================================================================
    // Initialize ImGui UI
    // =============================================================================
    UI::ImGuiBackend imguiBackend;
    if (!imguiBackend.Initialize(
        window->GetSDLWindow(),
        device->GetDevice(),
        kFrameCount,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        renderer->GetShaderVisibleHeap()
    )) {
        spdlog::critical("Failed to initialize ImGui");
        return 1;
    }

    UI::MaterialPalette materialPalette;
    materialPalette.Initialize();

    UI::BrushPanel brushPanel;
    brushPanel.Initialize();

    UI::PauseMenu pauseMenu;
    pauseMenu.Initialize();

    spdlog::info("Initialization complete. Entering main loop...");
    spdlog::info("Controls: P=Pause/Menu, ESC=Pause/Menu, WASD=Move, Mouse=Look, Space=Jump/Fly, Double-Space=Toggle Flight, V=Perspective, LMB=Paint, RMB=Erase");

    // Camera setup with pitch/yaw for mouse look
    const float fov = 60.0f * 3.14159f / 180.0f;
    const float aspectRatio = static_cast<float>(windowConfig.width) / static_cast<float>(windowConfig.height);
    const float cameraSpeed = 50.0f;  // Units per second
    const float mouseSensitivity = 0.001f;  // Radians per pixel (halved for better control)

    // Spawn above a seeded scenic mesa near origin. The terrain generator has
    // an explicit origin uplift so this starts inside the vertical render window
    // instead of above an empty lowland slice.
    glm::vec3 cameraPos = glm::vec3(96.0f, 236.0f, 96.0f);

    // Camera rotation (pitch and yaw)
    float cameraPitch = -0.3f;  // Slight downward look to see terrain below
    float cameraYaw = 0.0f;  // Look straight ahead (north)

    // Player physics for walking on terrain
    float cameraVelocityY = 0.0f;  // Vertical velocity for gravity
    const float gravity = -50.0f;  // Gravity acceleration (units/s^2)
    const float playerHeight = 6.0f;  // Player eye height above ground (voxels)
    const float stepHeight = 2.5f;  // Max step height for climbing (voxels)
    const float playerRadius = 0.75f;  // Player collision radius (voxels)

    // Flight mode toggle (double-click Space to enable/disable)
    bool flightMode = false;
    bool thirdPersonMode = false;
    const float thirdPersonDistance = 20.0f;
    const float thirdPersonHeight = 8.0f;

    // Terrain ready flag - don't apply gravity until ground detection works
    // This prevents the camera from falling through the world during startup
    // before chunks have been generated
    bool terrainReady = sparseRuntimeTestMode || (sparseBackendRequested && sparseVoxelWorldReady);

    if (sparseBackendRequested && sparseVoxelWorldReady) {
        const float spawnProbeY = std::max(cameraPos.y + 256.0f, 720.0f);
        const auto spawnGround = sparseVoxelWorld.Raycast(
            cameraPos.x,
            spawnProbeY,
            cameraPos.z,
            0.0f,
            -1.0f,
            0.0f,
            1400.0f);
        if (spawnGround.hit) {
            cameraPos.y = static_cast<float>(spawnGround.voxelY) + 1.0f + playerHeight;
            cameraVelocityY = 0.0f;
            spdlog::info(
                "Sparse spawn placed on generated terrain at world=({:.1f},{:.1f},{:.1f}) groundY={}",
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                spawnGround.voxelY);
        } else {
            spdlog::warn(
                "Sparse spawn probe found no ground at xz=({:.1f},{:.1f}); keeping default Y {:.1f}",
                cameraPos.x,
                cameraPos.z,
                cameraPos.y);
        }
    }

    // Player position represents feet/collision point
    // Camera rendering position is offset upward by playerHeight for natural eye-level view

    // Main loop
    bool running = true;
    bool paused = false;
    uint64_t frameCount = 0;
    const uint32_t exitAfterFrames = ReadUIntEnv("VENPOD_EXIT_AFTER_FRAMES", 0u);
    const bool traceFrameStages = std::getenv("VENPOD_TRACE_FRAME_STAGES") != nullptr;
    bool mouseInitialized = false;  // Track if mouse capture has been enabled
    uint64_t lastFrameCounter = SDL_GetPerformanceCounter();
    const double performanceFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    float smoothedFrameMs = 16.67f;
    float lastRawFrameMs = 16.67f;
    uint64_t physicsDispatchCount = 0;
    uint64_t physicsBudgetSkipCount = 0;
    uint32_t currentCopyBudget = voxelWorld->GetMaxChunkCopiesPerFrame();
    const uint32_t denseGenerationMax = ReadUIntEnv(
        "VENPOD_DENSE_GENERATION_MAX",
        highDensityDenseWindow ? 12u : 8u);
    uint32_t currentGenerationBudget = std::min<uint32_t>(3u, denseGenerationMax);
    const float denseRaymarchDefaultDistance = static_cast<float>(ReadUIntEnv(
        "VENPOD_RAYMARCH_MAX_DISTANCE",
        lowMemoryDenseWindow ? 1400u : 3000u));
    const uint32_t denseRaymarchDefaultSteps = ReadUIntEnv(
        "VENPOD_RAYMARCH_MAX_STEPS",
        lowMemoryDenseWindow ? 1152u : 2048u);
    const float sparseRaymarchDefaultDistance = static_cast<float>(ReadUIntEnv(
        "VENPOD_SPARSE_RAYMARCH_MAX_DISTANCE",
        64u));
    const uint32_t sparseRaymarchDefaultSteps = ReadUIntEnv(
        "VENPOD_SPARSE_RAYMARCH_MAX_STEPS",
        16u);
    const float sparseRaymarchMaxScale = std::max(
        0.10f,
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_RAYMARCH_MAX_SCALE_PERCENT", 100u)) / 100.0f);
    float currentRaymarchMaxDistance = enableSparseRaymarch
        ? sparseRaymarchDefaultDistance
        : denseRaymarchDefaultDistance;
    uint32_t currentRaymarchMaxSteps = enableSparseRaymarch
        ? sparseRaymarchDefaultSteps
        : denseRaymarchDefaultSteps;
    float currentFarFieldQuality = 1.0f;
    float currentRenderQuality = 1.0f;
    float perfFenceWaitMs = 0.0f;
    float perfChunkUpdateMs = 0.0f;
    float perfPhysicsSubmitMs = 0.0f;
    float perfBrushSubmitMs = 0.0f;
    float perfRenderSubmitMs = 0.0f;
    float perfPresentMs = 0.0f;
    float schedulerPredictedFrameMs = 16.67f;
    bool hasCompletedGroundQuery = false;
    bool hasCompletedBrushQuery = false;
    glm::vec3 completedGroundQueryRegionOriginWorld(0.0f);
    glm::vec3 completedGroundQueryFeetWorld(0.0f);
    glm::vec3 completedBrushQueryRegionOriginWorld(0.0f);
    glm::vec3 completedBrushQueryOriginWorld(0.0f);
    glm::vec3 completedBrushQueryDirectionWorld(0.0f, 0.0f, 1.0f);
    glm::vec3 nextGroundQueryRegionOriginWorld(0.0f);
    glm::vec3 nextGroundQueryFeetWorld(0.0f);
    glm::vec3 nextBrushQueryRegionOriginWorld(0.0f);
    glm::vec3 nextBrushQueryOriginWorld(0.0f);
    glm::vec3 nextBrushQueryDirectionWorld(0.0f, 0.0f, 1.0f);
    std::array<GroundQueryMetadata, kFrameCount> groundQueryMetadata = {};
    std::array<BrushQueryMetadata, kFrameCount> brushQueryMetadata = {};
    BuildStrokeState buildStrokeState;
    glm::vec3 physicsDirtyCenterWorld = cameraPos;
    uint32_t physicsDirtyFramesRemaining = 0;
    uint64_t physicsDirtyEvents = 0;
    glm::vec3 lastPhysicsSchedulerCameraWorld = cameraPos;
    glm::vec3 lastSparseResidencyCameraWorld = cameraPos;
    glm::vec3 lastBoundaryTestCameraWorld = cameraPos;
    std::vector<Simulation::BrickCoord> sparseMissFeedbackPending;
    uint32_t sparseMissFeedbackConsumedLastFrame = 0;
    uint32_t sparseSpeculativeRequestsLastFrame = 0;
    uint32_t sparseVisibleRequestsLastFrame = 0;
    uint32_t sparseCollisionRequestsLastFrame = 0;
    uint32_t sparsePressureTrimLastFrame = 0;
    uint32_t sparseReplacementEvictionsLastFrame = 0;
    uint32_t sparseSpeculativeBackpressureSkipsLastFrame = 0;
    uint32_t sparseDistanceTrimSkippedLastFrame = 0;
    uint32_t sparseUploadRequeuesLastFrame = 0;
    uint32_t sparseInvalidationRequeuesLastFrame = 0;
    uint32_t sparsePageTablePublishRetriesLastFrame = 0;
    uint32_t sparseGenerationBudgetLastFrame = 0;
    uint32_t sparseUploadBudgetLastFrame = 0;
    uint32_t sparseMidClipmapBudgetLastFrame = 0;
    float sparseRuntimeBudgetScale = 1.0f;
    float sparseRaymarchBudgetScale = 1.0f;
    std::deque<uint32_t> sparsePendingPageTablePublishes;
    float boundaryTestElapsedSeconds = 0.0f;

    auto setPauseMenuOpen = [&](bool open) {
        if (open) {
            pauseMenu.Show();
        } else {
            pauseMenu.Hide();
        }
        paused = open;
        inputManager.SetMouseCaptured(!open);
        spdlog::info("Pause menu {}", open ? "opened" : "closed");
    };

    while (running) {
        uint64_t currentFrameCounter = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(
            static_cast<double>(currentFrameCounter - lastFrameCounter) / performanceFrequency);
        lastFrameCounter = currentFrameCounter;
        lastRawFrameMs = dt * 1000.0f;
        smoothedFrameMs = smoothedFrameMs * 0.92f + lastRawFrameMs * 0.08f;
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 30.0f);
        const auto ticksToMs = [performanceFrequency](uint64_t ticks) {
            return static_cast<float>(static_cast<double>(ticks) * 1000.0 / performanceFrequency);
        };
        perfFenceWaitMs = 0.0f;
        perfChunkUpdateMs = 0.0f;
        perfPhysicsSubmitMs = 0.0f;
        perfBrushSubmitMs = 0.0f;
        perfRenderSubmitMs = 0.0f;

        if (enableFarSVO && !sparseFarField.enabled && farVoxelOctree.IsAsyncPending()) {
            if (farVoxelOctree.TryFinalizeAsyncUpload(device->GetDevice(), renderer->GetHeapManager())) {
                const auto& farStats = farVoxelOctree.GetStats();
                sparseFarField.nodeSRV = farVoxelOctree.GetNodeSRV();
                sparseFarField.pageSRV = farVoxelOctree.GetPageSRV();
                sparseFarField.pageIndexSRV = farVoxelOctree.GetPageIndexSRV();
                sparseFarField.nodeCount = farStats.nodeCount;
                sparseFarField.pageCount = farStats.pageCount;
                sparseFarField.pageIndexCount = farStats.pageIndexCount;
                sparseFarField.pageRadius = farStats.pageRadius;
                sparseFarField.pageSize = farStats.pageSize;
                sparseFarField.rootMinY = farStats.rootMinY;
                sparseFarField.enabled = true;
                spdlog::info(
                    "Far sparse voxel octree async ready: {} pages, {} nodes, source={}, cpu={:.1f} ms, gpuUpload={:.1f} ms",
                    farStats.pageCount,
                    farStats.nodeCount,
                    farStats.loadedFromCache ? "cache" : "build",
                    farStats.cpuBuildMs,
                    farStats.gpuUploadMs);
            }
        }

        const auto& previousStreamingStats = voxelWorld->GetStreamingStats();
        const bool streamingStillFilling =
            previousStreamingStats.expectedVisibleChunks == 0 ||
            previousStreamingStats.cachedReadChunks < previousStreamingStats.expectedVisibleChunks ||
            previousStreamingStats.cachedWriteChunks < previousStreamingStats.expectedVisibleChunks;
        const bool sourceStillFilling =
            previousStreamingStats.expectedVisibleChunks == 0 ||
            previousStreamingStats.generatedChunks < previousStreamingStats.expectedVisibleChunks ||
            previousStreamingStats.queuedChunks > 0 ||
            previousStreamingStats.chunksNotLoadedLastFrame > 0;

        const float schedulerPressureMs = std::max(smoothedFrameMs, schedulerPredictedFrameMs);
        const float gpuSchedulerPressureMs = gpuTiming.valid
            ? std::max(static_cast<float>(gpuTiming.frameMs), static_cast<float>(gpuTiming.raymarchMs))
            : 0.0f;
        const float combinedSchedulerPressureMs =
            std::max(schedulerPressureMs, gpuSchedulerPressureMs);

        uint32_t targetCopyBudget = 40;
        uint32_t targetGenerationBudget = 3;
        float targetFarFieldQuality = 1.0f;
        if (lastRawFrameMs > 30.0f || combinedSchedulerPressureMs > 21.0f) {
            targetCopyBudget = 8;
            targetGenerationBudget = 0;
            targetFarFieldQuality = 0.45f;
        } else if (lastRawFrameMs > 24.0f || combinedSchedulerPressureMs > 19.0f) {
            targetCopyBudget = 12;
            targetGenerationBudget = 1;
            targetFarFieldQuality = 0.60f;
        } else if (combinedSchedulerPressureMs > 18.0f) {
            targetCopyBudget = 16;
            targetGenerationBudget = 1;
            targetFarFieldQuality = 0.75f;
        } else if (combinedSchedulerPressureMs > 17.0f) {
            targetCopyBudget = 24;
            targetGenerationBudget = 3;
            targetFarFieldQuality = 0.88f;
        } else if (streamingStillFilling) {
            targetCopyBudget = 48;
            targetGenerationBudget = 8;
            targetFarFieldQuality = 0.92f;
        } else {
            targetCopyBudget = 40;
            targetGenerationBudget = 6;
            targetFarFieldQuality = 1.0f;
        }
        if (sourceStillFilling) {
            // Do not starve visible chunk generation because of one bad frame.
            // Generation now batches the frame's work into one command-list
            // submission, so keeping the source queue moving is cheaper than
            // letting missing visible pages linger for seconds.
            const uint32_t backlogBoost =
                previousStreamingStats.queuedChunks > 1500 ? 10u :
                previousStreamingStats.queuedChunks > 700 ? 8u :
                6u;
            targetGenerationBudget = std::max<uint32_t>(targetGenerationBudget, backlogBoost);
            if (streamingStillFilling) {
                targetCopyBudget = std::max<uint32_t>(targetCopyBudget, 24u);
            }
        }
        if (previousStreamingStats.readSlotMismatches > 0 ||
            previousStreamingStats.chunksNotLoadedLastFrame > 0 ||
            previousStreamingStats.chunksNotGeneratedLastFrame > 0) {
            targetCopyBudget = std::max<uint32_t>(targetCopyBudget, 56u);
            targetGenerationBudget = std::max<uint32_t>(targetGenerationBudget, 24u);
        }
        targetGenerationBudget = std::min<uint32_t>(targetGenerationBudget, denseGenerationMax);

        if (streamingStillFilling || sourceStillFilling) {
            currentCopyBudget = std::max(currentCopyBudget, std::min<uint32_t>(targetCopyBudget, 56u));
            currentGenerationBudget = std::max(currentGenerationBudget, targetGenerationBudget);
        } else if (targetCopyBudget < currentCopyBudget) {
            currentCopyBudget = targetCopyBudget;
        } else if (targetCopyBudget > currentCopyBudget && (frameCount % 8 == 0)) {
            currentCopyBudget = std::min(targetCopyBudget, currentCopyBudget + 2u);
        }
        if (!streamingStillFilling && !sourceStillFilling && targetGenerationBudget < currentGenerationBudget) {
            currentGenerationBudget = targetGenerationBudget;
        } else if (!streamingStillFilling && !sourceStillFilling && targetGenerationBudget > currentGenerationBudget && (frameCount % 12 == 0)) {
            currentGenerationBudget = std::min(targetGenerationBudget, currentGenerationBudget + 2u);
        }
        if (enableSparseRaymarch) {
            float targetSparseRaymarchScale = sparseRaymarchMaxScale;
            if (gpuTiming.valid && combinedSchedulerPressureMs > 19.0f) {
                targetSparseRaymarchScale = std::min(targetSparseRaymarchScale, 0.45f);
            } else if (gpuTiming.valid && combinedSchedulerPressureMs > 17.0f) {
                targetSparseRaymarchScale = std::min(targetSparseRaymarchScale, 0.62f);
            } else if (gpuTiming.valid && combinedSchedulerPressureMs > 15.5f) {
                targetSparseRaymarchScale = std::min(targetSparseRaymarchScale, 0.78f);
            }
            const float sparseScaleStep = targetSparseRaymarchScale < sparseRaymarchBudgetScale ? 0.22f : 0.035f;
            sparseRaymarchBudgetScale += (targetSparseRaymarchScale - sparseRaymarchBudgetScale) * sparseScaleStep;
            sparseRaymarchBudgetScale = std::clamp(sparseRaymarchBudgetScale, 0.20f, sparseRaymarchMaxScale);

            currentRaymarchMaxDistance = std::max(32.0f, sparseRaymarchDefaultDistance * sparseRaymarchBudgetScale);
            currentRaymarchMaxSteps = std::max<uint32_t>(
                4u,
                static_cast<uint32_t>(std::floor(static_cast<float>(sparseRaymarchDefaultSteps) * sparseRaymarchBudgetScale + 0.5f)));
        } else {
            currentRaymarchMaxDistance = denseRaymarchDefaultDistance;
            currentRaymarchMaxSteps = denseRaymarchDefaultSteps;
        }
        currentFarFieldQuality += (targetFarFieldQuality - currentFarFieldQuality) * 0.08f;
        currentRenderQuality = 1.0f;
        voxelWorld->SetMaxChunkCopiesPerFrame(currentCopyBudget);
        const uint32_t trailingGenerationBudget = currentGenerationBudget;
        voxelWorld->SetChunkGenerationBudget(0);

        sparseRuntimeBudgetScale = 1.0f;
        if (sparseBackendRequested && sparseVoxelWorldReady) {
            const auto& sparseStatsForScheduler = sparseVoxelWorld.GetStats();
            const auto& sparseGpuStatsForScheduler = sparseGpuResources.GetStats();
            const bool sparseQueueBacklog =
                sparseStatsForScheduler.generationQueuedBricks > 0 ||
                sparseStatsForScheduler.uploadQueuedBricks > 0 ||
                !sparseMissFeedbackPending.empty() ||
                sparseClipmapTileCache.GetStats().queuedTiles > 0 ||
                sparseClipmapTileCache.GetStats().queuedVoxelBricks > 0;
            const bool sparseUploadPressure =
                sparseGpuStatsForScheduler.uploadRingOverflowLastFrame ||
                sparseGpuStatsForScheduler.stagedBytesLastFrame >
                    (sparseGpuStatsForScheduler.uploadRingBytes / std::max(1u, sparseWorldConfig.maxBrickPages / 512u));
            if (lastRawFrameMs > 30.0f || combinedSchedulerPressureMs > 21.0f || sparseUploadPressure) {
                sparseRuntimeBudgetScale = 0.35f;
            } else if (combinedSchedulerPressureMs > 19.0f) {
                sparseRuntimeBudgetScale = 0.55f;
            } else if (combinedSchedulerPressureMs > 17.0f) {
                sparseRuntimeBudgetScale = 0.75f;
            } else if (sparseQueueBacklog && combinedSchedulerPressureMs < 14.5f) {
                sparseRuntimeBudgetScale = 1.35f;
            }
        }

        auto scaleRuntimeBudget = [](uint32_t budget, float scale, uint32_t minIfNonZero = 0u) -> uint32_t {
            if (budget == 0) {
                return 0;
            }
            const uint32_t scaled = static_cast<uint32_t>(std::floor(static_cast<float>(budget) * scale + 0.5f));
            return std::max(minIfNonZero, scaled);
        };

        // Process SDL events FIRST to update mouse/keyboard state
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Pass event to ImGui FIRST (captures mouse/keyboard when hovering UI)
            imguiBackend.ProcessEvent(event);

            // Then pass event to input manager
            inputManager.ProcessEvent(event);

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        setPauseMenuOpen(!pauseMenu.IsVisible());
                    }
                    break;

                case SDL_EVENT_WINDOW_RESIZED: {
                    uint32_t newWidth = static_cast<uint32_t>(event.window.data1);
                    uint32_t newHeight = static_cast<uint32_t>(event.window.data2);

                    // Wait for GPU before resize
                    commandQueue->Flush();

                    window->OnResize(newWidth, newHeight);
                    renderer->OnResize(newWidth, newHeight);
                    inputManager.OnResize(newWidth, newHeight);
                    break;
                }

                default:
                    break;
            }
        }

        if (!running) break;

        // Enable mouse capture on first frame (after window has focus)
        if (!mouseInitialized) {
            inputManager.SetMouseCaptured(true);
            mouseInitialized = true;
        }

        // Begin input frame AFTER processing events
        inputManager.BeginFrame();

        // Handle input actions
        if (inputManager.IsActionPressed(Input::KeyAction::TogglePause)) {
            setPauseMenuOpen(!pauseMenu.IsVisible());
        }
        const bool gameplayInputEnabled = !pauseMenu.IsVisible();
        if (gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::MaterialNext)) {
            brushController.NextMaterial();
            spdlog::info("Material: {}", brushController.GetMaterial());
        }
        if (gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::MaterialPrev)) {
            brushController.PrevMaterial();
            spdlog::info("Material: {}", brushController.GetMaterial());
        }
        if (gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::BrushIncrease)) {
            brushController.IncreaseRadius();
            spdlog::info("Brush radius: {:.1f}", brushController.GetRadius());
        }
        if (gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::BrushDecrease)) {
            brushController.DecreaseRadius();
            spdlog::info("Brush radius: {:.1f}", brushController.GetRadius());
        }
        if (gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::TogglePerspective)) {
            thirdPersonMode = !thirdPersonMode;
            spdlog::info("Perspective: {}", thirdPersonMode ? "third-person" : "first-person");
        }

        const bool jumpPressed = gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::CameraUp);
        const bool flightTogglePressed = gameplayInputEnabled && inputManager.IsActionDoubleClicked(Input::KeyAction::CameraUp);

        // Mouse look - update camera rotation
        glm::vec2 mouseDelta = gameplayInputEnabled ? inputManager.GetMouseDelta() : glm::vec2(0.0f);
        cameraYaw += mouseDelta.x * mouseSensitivity;  // Inverted from - to + for correct left/right
        cameraPitch -= mouseDelta.y * mouseSensitivity;

        // Clamp pitch to prevent flipping
        const float maxPitch = 1.57f;  // ~90 degrees
        cameraPitch = glm::clamp(cameraPitch, -maxPitch, maxPitch);

        // Check for double-click on Space to toggle flight mode before applying
        // any movement. Streaming/recentering later in the frame must see the
        // final intended player world position, not a pre-flight-input position.
        if (flightTogglePressed) {
            flightMode = !flightMode;
            if (flightMode) {
                cameraVelocityY = 0.0f;  // Cancel gravity when entering flight mode
                spdlog::info("Flight mode ENABLED - gravity disabled");
            } else {
                spdlog::info("Flight mode DISABLED - gravity enabled");
            }
        }

        // Calculate camera basis from pitch/yaw
        glm::vec3 cameraForward;
        cameraForward.x = cos(cameraPitch) * cos(cameraYaw);
        cameraForward.y = sin(cameraPitch);
        cameraForward.z = cos(cameraPitch) * sin(cameraYaw);
        cameraForward = glm::normalize(cameraForward);

        glm::vec3 cameraRight = glm::cross(cameraForward, glm::vec3(0, 1, 0));
        if (glm::length(cameraRight) < 0.0001f) {
            cameraRight = glm::vec3(-std::sin(cameraYaw), 0.0f, std::cos(cameraYaw));
        } else {
            cameraRight = glm::normalize(cameraRight);
        }
        glm::vec3 cameraUp = glm::normalize(glm::cross(cameraRight, cameraForward));

        // Camera movement with WASD (horizontal only for walking mode)
        float moveSpeed = cameraSpeed * dt;

        // Calculate horizontal movement direction (forward/right with Y removed)
        glm::vec3 horizontalForward(std::cos(cameraYaw), 0.0f, std::sin(cameraYaw));
        glm::vec3 horizontalRight(-std::sin(cameraYaw), 0.0f, std::cos(cameraYaw));

        glm::vec3 moveDirection(0.0f);

        // WASD for horizontal movement only
        if (gameplayInputEnabled && inputManager.IsActionDown(Input::KeyAction::CameraForward)) {
            moveDirection += horizontalForward;
        }
        if (gameplayInputEnabled && inputManager.IsActionDown(Input::KeyAction::CameraBackward)) {
            moveDirection -= horizontalForward;
        }
        if (gameplayInputEnabled && inputManager.IsActionDown(Input::KeyAction::CameraLeft)) {
            moveDirection -= horizontalRight;
        }
        if (gameplayInputEnabled && inputManager.IsActionDown(Input::KeyAction::CameraRight)) {
            moveDirection += horizontalRight;
        }
        if (glm::length(moveDirection) > 0.001f) {
            cameraPos += glm::normalize(moveDirection) * moveSpeed;
        }

        if (gameplayInputEnabled && flightMode && !enableBoundaryTest) {
            if (inputManager.IsActionDown(Input::KeyAction::CameraUp)) {
                cameraPos.y += moveSpeed * 2.0f;
            }
            if (inputManager.IsActionDown(Input::KeyAction::CameraDown)) {
                cameraPos.y -= moveSpeed * 2.0f;
            }
        }

        if (enableBoundaryTest && !paused) {
            flightMode = true;
            terrainReady = true;
            cameraVelocityY = 0.0f;

            boundaryTestElapsedSeconds += dt;
            constexpr float boundaryTestSpeed = 180.0f;
            constexpr float boundaryTestPhaseSeconds = 4.0f;
            const uint32_t phase = static_cast<uint32_t>(boundaryTestElapsedSeconds / boundaryTestPhaseSeconds) % 6;
            glm::vec3 scriptedDirection(0.0f);
            if (phase == 0) scriptedDirection = glm::vec3(1, 0, 0);
            if (phase == 1) scriptedDirection = glm::vec3(-1, 0, 0);
            if (phase == 2) scriptedDirection = glm::vec3(0, 0, 1);
            if (phase == 3) scriptedDirection = glm::vec3(0, 0, -1);
            if (phase == 4) scriptedDirection = glm::vec3(0, 1, 0);
            if (phase == 5) scriptedDirection = glm::vec3(0, -1, 0);

            cameraPos += scriptedDirection * boundaryTestSpeed * dt;
            const float observedStep = glm::length(cameraPos - lastBoundaryTestCameraWorld);
            const float maxExpectedStep = boundaryTestSpeed * dt * 1.75f + 2.0f;
            if (frameCount > 0 && observedStep > maxExpectedStep) {
                spdlog::error("BOUNDARY_TEST discontinuity: step={:.3f} expected<={:.3f} prev=({:.2f},{:.2f},{:.2f}) now=({:.2f},{:.2f},{:.2f}) phase={}",
                    observedStep,
                    maxExpectedStep,
                    lastBoundaryTestCameraWorld.x,
                    lastBoundaryTestCameraWorld.y,
                    lastBoundaryTestCameraWorld.z,
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    phase);
            }
            if (frameCount % 30 == 0) {
                spdlog::info("BOUNDARY_TEST phase={} cameraWorld=({:.2f},{:.2f},{:.2f})",
                    phase,
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z);
            }
            lastBoundaryTestCameraWorld = cameraPos;
        }

        if (sparseBackendRequested && sparseVoxelWorldReady) {
            const Simulation::BrickCoord sparseCenter =
                Simulation::BrickCoord::FromWorldVoxel(
                    static_cast<int32_t>(std::floor(cameraPos.x)),
                    static_cast<int32_t>(std::floor(cameraPos.y - playerHeight)),
                    static_cast<int32_t>(std::floor(cameraPos.z)));

            uint32_t sparseNewRequestsThisFrame = 0;
            uint32_t sparseSpeculativeRequestsThisFrame = 0;
            uint32_t sparseVisibleRequestsThisFrame = 0;
            uint32_t sparseCollisionRequestsThisFrame = 0;
            const uint32_t sparseResidencyFrame =
                static_cast<uint32_t>(std::min<uint64_t>(
                    frameCount,
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
            sparsePressureTrimLastFrame = 0;
            sparseReplacementEvictionsLastFrame = 0;
            sparseSpeculativeBackpressureSkipsLastFrame = 0;
            sparseDistanceTrimSkippedLastFrame = 0;
            sparseGenerationBudgetLastFrame = 0;
            sparseMidClipmapBudgetLastFrame = 0;
            if (sparsePressureTrimBudget > 0 &&
                !sparseMissFeedbackPending.empty() &&
                sparseVoxelWorld.GetStats().freePages <= sparseMinFreePages) {
                sparsePressureTrimLastFrame = sparseVoxelWorld.TrimResidentBricks(
                    sparseCenter,
                    sparseTrimRadiusXz,
                    sparseTrimRadiusY,
                    sparsePressureTrimBudget);
            }
            auto requestSparseBrick = [&](
                const Simulation::BrickCoord& coord,
                bool urgent,
                Simulation::SparseResidencyClass residencyClass = Simulation::SparseResidencyClass::Speculative) {
                if (sparseVoxelWorld.GetPool().TryGetPage(coord)) {
                    sparseVoxelWorld.TouchResidencyClass(coord, residencyClass, sparseResidencyFrame);
                    return true;
                }
                const auto& stats = sparseVoxelWorld.GetStats();
                const uint32_t minFreePages = urgent ? std::min(4u, sparseMinFreePages) : sparseMinFreePages;
                if (residencyClass == Simulation::SparseResidencyClass::Speculative &&
                    (stats.generationQueuedBricks >= sparseSpeculativeBackpressureGenQueue ||
                     sparseMissFeedbackPending.size() >= sparseSpeculativeBackpressureMissPending ||
                     stats.freePages <= sparseMinFreePages)) {
                    ++sparseSpeculativeBackpressureSkipsLastFrame;
                    return false;
                }
                uint32_t* classCounter = &sparseSpeculativeRequestsThisFrame;
                uint32_t classBudget = sparseSpeculativeRequestBudget;
                if (residencyClass == Simulation::SparseResidencyClass::Visible) {
                    classCounter = &sparseVisibleRequestsThisFrame;
                    classBudget = sparseVisibleRequestBudget;
                } else if (residencyClass == Simulation::SparseResidencyClass::Collision ||
                           residencyClass == Simulation::SparseResidencyClass::Edited) {
                    classCounter = &sparseCollisionRequestsThisFrame;
                    classBudget = sparseCollisionRequestBudget;
                }
                bool madeReplacementRoom = false;
                if (stats.freePages <= minFreePages &&
                    residencyClass != Simulation::SparseResidencyClass::Speculative &&
                    sparseReplacementEvictionsLastFrame < sparseReplacementBudget) {
                    const uint32_t evicted = sparseVoxelWorld.EvictLowerPriorityForRequest(
                        sparseCenter,
                        residencyClass,
                        std::max(1u, sparseCollisionShellRadiusXz),
                        sparseCollisionShellRadiusY,
                        1u,
                        sparseResidencyFrame);
                    sparseReplacementEvictionsLastFrame += evicted;
                    madeReplacementRoom = evicted > 0;
                }
                const auto& statsAfterReplacement = sparseVoxelWorld.GetStats();
                if (statsAfterReplacement.freePages == 0 ||
                    (statsAfterReplacement.freePages <= minFreePages && !madeReplacementRoom) ||
                    sparseNewRequestsThisFrame >= sparseTotalRequestBudget ||
                    *classCounter >= classBudget) {
                    return false;
                }
                if (!sparseVoxelWorld.RequestBrick(coord)) {
                    return false;
                }
                sparseVoxelWorld.TouchResidencyClass(coord, residencyClass, sparseResidencyFrame);
                ++sparseNewRequestsThisFrame;
                ++(*classCounter);
                return true;
            };

            sparseMissFeedbackConsumedLastFrame = 0;
            if (!sparseMissFeedbackPending.empty()) {
                std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> consumedThisFrame;
                size_t readIndex = 0;
                size_t writeIndex = 0;
                for (; readIndex < sparseMissFeedbackPending.size(); ++readIndex) {
                    const Simulation::BrickCoord coord = sparseMissFeedbackPending[readIndex];
                    if (!consumedThisFrame.insert(coord).second) {
                        ++sparseMissFeedbackConsumedLastFrame;
                        continue;
                    }
                    if (!requestSparseBrick(coord, true, Simulation::SparseResidencyClass::Visible)) {
                        break;
                    }
                    ++sparseMissFeedbackConsumedLastFrame;
                }
                for (; readIndex < sparseMissFeedbackPending.size(); ++readIndex) {
                    sparseMissFeedbackPending[writeIndex++] = sparseMissFeedbackPending[readIndex];
                }
                sparseMissFeedbackPending.resize(writeIndex);
            }

            if (sparseCenter != lastSparseRequestCenter) {
                const int32_t sparseForwardX = cameraForward.x > 0.35f ? 1 : (cameraForward.x < -0.35f ? -1 : 0);
                const int32_t sparseForwardY = cameraForward.y > 0.45f ? 1 : (cameraForward.y < -0.45f ? -1 : 0);
                const int32_t sparseForwardZ = cameraForward.z > 0.35f ? 1 : (cameraForward.z < -0.35f ? -1 : 0);
                const auto sparseRequests = sparseRequestPlanner.Plan(
                    sparseCenter,
                    sparseForwardX,
                    sparseForwardY,
                    sparseForwardZ);
                for (const auto& request : sparseRequests) {
                    requestSparseBrick(request.coord, true, Simulation::SparseResidencyClass::Visible);
                }
                lastSparseRequestCenter = sparseCenter;
            }

            const int32_t collisionRadiusXz = static_cast<int32_t>(sparseCollisionShellRadiusXz);
            const int32_t collisionRadiusY = static_cast<int32_t>(sparseCollisionShellRadiusY);
                for (int32_t dz = -collisionRadiusXz; dz <= collisionRadiusXz; ++dz) {
                for (int32_t dy = -collisionRadiusY; dy <= collisionRadiusY; ++dy) {
                    for (int32_t dx = -collisionRadiusXz; dx <= collisionRadiusXz; ++dx) {
                        requestSparseBrick(
                            {sparseCenter.x + dx, sparseCenter.y + dy, sparseCenter.z + dz},
                            true,
                            Simulation::SparseResidencyClass::Collision);
                    }
                }
            }

            if (sparseRayPrefetchDistance > 0 && sparseRayPrefetchMaxRequests > 0) {
                const glm::vec3 sparseCameraDelta = cameraPos - lastSparseResidencyCameraWorld;
                const float sparseCameraSpeed = glm::length(sparseCameraDelta) / std::max(dt, 0.001f);
                const bool usePredictivePrefetch =
                    sparsePredictivePrefetchMs > 0 &&
                    sparseCameraSpeed > 12.0f &&
                    sparseRayPrefetchMaxRequests >= 4u;
                const uint32_t currentViewBudget = usePredictivePrefetch
                    ? std::max(1u, (sparseRayPrefetchMaxRequests * 2u) / 3u)
                    : sparseRayPrefetchMaxRequests;
                const uint32_t predictiveViewBudget = usePredictivePrefetch
                    ? std::max(1u, sparseRayPrefetchMaxRequests - currentViewBudget)
                    : 0u;

                Simulation::SparseViewConeConfig viewPrefetch{};
                viewPrefetch.originX = cameraPos.x;
                viewPrefetch.originY = cameraPos.y;
                viewPrefetch.originZ = cameraPos.z;
                viewPrefetch.forwardX = cameraForward.x;
                viewPrefetch.forwardY = cameraForward.y;
                viewPrefetch.forwardZ = cameraForward.z;
                viewPrefetch.rightX = cameraRight.x;
                viewPrefetch.rightY = cameraRight.y;
                viewPrefetch.rightZ = cameraRight.z;
                viewPrefetch.upX = cameraUp.x;
                viewPrefetch.upY = cameraUp.y;
                viewPrefetch.upZ = cameraUp.z;
                viewPrefetch.verticalFovRadians = fov;
                viewPrefetch.aspectRatio = aspectRatio;
                viewPrefetch.maxDistance = static_cast<float>(sparseRayPrefetchDistance);
                viewPrefetch.stepDistance = static_cast<float>(sparseRayPrefetchStride);
                viewPrefetch.rayGrid = sparseViewPrefetchRayGrid;
                viewPrefetch.maxRequests = currentViewBudget;
                for (const auto& request : sparseRequestPlanner.PlanViewCone(viewPrefetch)) {
                    requestSparseBrick(request.coord, false, Simulation::SparseResidencyClass::Speculative);
                }

                if (usePredictivePrefetch && predictiveViewBudget > 0) {
                    const float predictionSeconds =
                        static_cast<float>(sparsePredictivePrefetchMs) * 0.001f;
                    const glm::vec3 predictedCameraPos =
                        cameraPos + sparseCameraDelta / std::max(dt, 0.001f) * predictionSeconds;
                    viewPrefetch.originX = predictedCameraPos.x;
                    viewPrefetch.originY = predictedCameraPos.y;
                    viewPrefetch.originZ = predictedCameraPos.z;
                    viewPrefetch.maxRequests = predictiveViewBudget;
                    for (const auto& request : sparseRequestPlanner.PlanViewCone(viewPrefetch)) {
                        requestSparseBrick(request.coord, false, Simulation::SparseResidencyClass::Speculative);
                    }
                }
            }
            lastSparseResidencyCameraWorld = cameraPos;
            sparseSpeculativeRequestsLastFrame = sparseSpeculativeRequestsThisFrame;
            sparseVisibleRequestsLastFrame = sparseVisibleRequestsThisFrame;
            sparseCollisionRequestsLastFrame = sparseCollisionRequestsThisFrame;

            const auto& sparseStatsBeforeGeneration = sparseVoxelWorld.GetStats();
            const bool sparseFeedbackPressure =
                sparseReplacementEvictionsLastFrame > 0 ||
                sparseMissFeedbackConsumedLastFrame > 0 ||
                !sparseMissFeedbackPending.empty();
            uint32_t sparseGenerationBudgetThisFrame =
                sparseStatsBeforeGeneration.residentBricks < sparseBootstrapResidentTarget
                    ? std::max(sparseGenerationBudget, sparseBootstrapGenerationBudget)
                    : sparseGenerationBudget;
            if (sparseFeedbackPressure) {
                sparseGenerationBudgetThisFrame =
                    std::max(sparseGenerationBudgetThisFrame, sparseFeedbackGenerationBudget);
            }
            sparseGenerationBudgetThisFrame =
                scaleRuntimeBudget(sparseGenerationBudgetThisFrame, sparseRuntimeBudgetScale, 1u);
            sparseGenerationBudgetLastFrame = sparseGenerationBudgetThisFrame;
            sparseVoxelWorld.PumpGeneration(sparseGenerationBudgetThisFrame, sparseResidencyFrame);
            if (sparseClipmapTileCacheReady && sparseClipmapPolicy.IsEnabled()) {
                sparseClipmapTileCache.UpdateInterest(
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    sparseResidencyFrame,
                    sparseClipmapPolicy);
                const uint32_t sparseMidClipmapBudgetThisFrame =
                    scaleRuntimeBudget(sparseMidClipmapTileBudget, sparseRuntimeBudgetScale, 1u);
                sparseMidClipmapBudgetLastFrame = sparseMidClipmapBudgetThisFrame;
                sparseClipmapTileCache.PumpGeneration(
                    sparseMidClipmapBudgetThisFrame,
                    sparseResidencyFrame,
                    sparseClipmapPolicy);
            }
            const auto& sparseStatsBeforeDistanceTrim = sparseVoxelWorld.GetStats();
            if (sparseStatsBeforeDistanceTrim.residentBricks >= sparseTrimStartResident ||
                sparseStatsBeforeDistanceTrim.freePages <= sparseMinFreePages) {
                sparseVoxelWorld.TrimResidentBricks(
                    sparseCenter,
                    sparseTrimRadiusXz,
                    sparseTrimRadiusY,
                    sparseTrimBudget);
            } else {
                sparseDistanceTrimSkippedLastFrame = 1;
            }
        }

        // Walking physics must not advance downward into a streamed page that is
        // not resident yet. Otherwise fast movement can outrun chunk upload, miss
        // the ground raycast for a few frames, fall through the hole, and then
        // crash/teleport when terrain catches up.
        bool supportChunkReadyForWalking = true;
        const bool sparseCollisionAuthoritative = sparseBackendRequested && sparseVoxelWorldReady;
        if (!flightMode && terrainReady && sparseCollisionAuthoritative) {
            // Sparse collision samples generated terrain plus persistent edits
            // directly in world space. It is not tied to render-page residency,
            // so walking does not fall through just because dense chunks or GPU
            // sparse pages are still streaming.
            supportChunkReadyForWalking = true;
        } else if (!flightMode && terrainReady && voxelWorld && voxelWorld->IsUsingInfiniteChunks()) {
            const int32_t supportX = static_cast<int32_t>(std::floor(cameraPos.x));
            const int32_t supportY = static_cast<int32_t>(std::floor(cameraPos.y - playerHeight - 1.0f));
            const int32_t supportZ = static_cast<int32_t>(std::floor(cameraPos.z));
            auto queueSupportFootprint = [&](const glm::vec3& worldCenter, int32_t priorityBase) {
                auto* chunkManager = voxelWorld->GetChunkManager();
                if (!chunkManager) {
                    return;
                }
                const auto centerChunk = Simulation::ChunkCoord::FromWorldPosition(
                    static_cast<int32_t>(std::floor(worldCenter.x)),
                    supportY,
                    static_cast<int32_t>(std::floor(worldCenter.z)),
                    Simulation::CHUNK_SIZE_VOXELS);
                for (int32_t dz = -1; dz <= 1; ++dz) {
                    for (int32_t dx = -1; dx <= 1; ++dx) {
                        const int32_t distance = dx * dx + dz * dz;
                        chunkManager->QueueUrgentChunk(
                            Simulation::ChunkCoord{centerChunk.x + dx, centerChunk.y, centerChunk.z + dz},
                            priorityBase + distance);
                    }
                }
            };

            queueSupportFootprint(cameraPos, -3'000'000);
            if (glm::length(moveDirection) > 0.001f) {
                const glm::vec3 walkingDirection = glm::normalize(moveDirection);
                queueSupportFootprint(cameraPos + walkingDirection * static_cast<float>(Simulation::CHUNK_SIZE_VOXELS * 2), -2'900'000);
            }

            supportChunkReadyForWalking = voxelWorld->IsWorldVoxelCachedForRead(supportX, supportY, supportZ);
            if (!supportChunkReadyForWalking && cameraVelocityY < 0.0f) {
                cameraVelocityY = 0.0f;
            }
        }

        // Apply gravity to vertical velocity (only when not in flight mode AND terrain is ready)
        // During startup, terrain might not be generated yet - disable gravity until
        // ground detection works to prevent falling through the world
        if (gameplayInputEnabled && !flightMode && terrainReady && supportChunkReadyForWalking) {
            cameraVelocityY += gravity * dt;
        }

        // Apply vertical velocity to camera position (only if terrain ready or flying)
        if (gameplayInputEnabled && ((terrainReady && (supportChunkReadyForWalking || cameraVelocityY >= 0.0f)) || flightMode)) {
            cameraPos.y += cameraVelocityY * dt;
        }

        // Calculate ray for GPU brush raycasting
        // When mouse is captured (FPS mode), always use screen center for crosshair
        // When mouse is free, use actual mouse position
        glm::vec2 brushNDC = inputManager.IsMouseCaptured()
            ? glm::vec2(0.0f, 0.0f)  // Screen center (crosshair position)
            : inputManager.GetMouseNDC();  // Actual mouse cursor

        float tanHalfFov = std::tan(fov * 0.5f);
        glm::vec3 rayDir = glm::normalize(
            cameraForward +
            cameraRight * brushNDC.x * tanHalfFov * aspectRatio +
            cameraUp * brushNDC.y * tanHalfFov
        );
        glm::vec3 brushRayOriginWorld = cameraPos;

        // Update brush controller (material, radius, buttons)
        // No CPU voxel data needed - GPU does the raycasting!
        brushController.UpdateFromMouse(
            brushNDC,
            brushRayOriginWorld,
            cameraForward,
            cameraRight,
            cameraUp,
            fov,
            aspectRatio,
            gameplayInputEnabled && inputManager.IsMouseButtonDown(Input::MouseButton::Left),
            gameplayInputEnabled && inputManager.IsMouseButtonDown(Input::MouseButton::Right),
            gameplayInputEnabled ? inputManager.GetScrollDelta() : 0.0f,
            nullptr,  // No CPU voxel data (GPU raycasting now!)
            0
        );

        // Get current frame context
        uint32_t frameIndex = window->GetCurrentBackBufferIndex();
        FrameContext& ctx = frameContexts[frameIndex];
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info(
                "FRAME_STAGE {} wait-start backBuffer={} fence={}",
                frameCount,
                frameIndex,
                ctx.fenceValue);
        }

        // Wait for this frame's previous work to complete
        uint64_t perfPhaseStart = SDL_GetPerformanceCounter();
        commandQueue->WaitForFenceValue(ctx.fenceValue);
        perfFenceWaitMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info(
                "FRAME_STAGE {} wait-done {:.2f}ms completedFence={}",
                frameCount,
                perfFenceWaitMs,
                commandQueue->GetLastCompletedFenceValue());
        }
        if (gpuTimestampReadback && gpuTimestampFrequency != 0) {
            ReadGpuTiming(gpuTimestampReadback.Get(), gpuTimestampFrequency, frameIndex, gpuTiming);
        }
        voxelWorld->RetireBrushEditFeedback(commandQueue->GetLastCompletedFenceValue());
        if (voxelWorld->RetireGroundRaycastReadback(frameIndex) && groundQueryMetadata[frameIndex].valid) {
            completedGroundQueryRegionOriginWorld = groundQueryMetadata[frameIndex].regionOriginWorld;
            completedGroundQueryFeetWorld = groundQueryMetadata[frameIndex].feetWorld;
            hasCompletedGroundQuery = true;
        }
        if (voxelWorld->RetireBrushRaycastReadback(frameIndex) && brushQueryMetadata[frameIndex].valid) {
            completedBrushQueryRegionOriginWorld = brushQueryMetadata[frameIndex].regionOriginWorld;
            completedBrushQueryOriginWorld = brushQueryMetadata[frameIndex].originWorld;
            completedBrushQueryDirectionWorld = brushQueryMetadata[frameIndex].directionWorld;
            hasCompletedBrushQuery = true;
        }
        if (enableSparseMissFeedback && sparseGpuResources.IsInitialized()) {
            sparseGpuResources.RetireMissFeedback(frameIndex, sparseMissFeedbackPending);
            if (!sparseMissFeedbackPending.empty()) {
                std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> uniqueFeedback;
                size_t writeIndex = 0;
                for (const Simulation::BrickCoord& coord : sparseMissFeedbackPending) {
                    if (uniqueFeedback.insert(coord).second) {
                        sparseMissFeedbackPending[writeIndex++] = coord;
                    }
                }
                sparseMissFeedbackPending.resize(writeIndex);
            }
            if (sparseMissFeedbackPending.size() > sparseMissFeedbackMaxRecords * 2ull) {
                const size_t keepCount = static_cast<size_t>(sparseMissFeedbackMaxRecords) * 2u;
                sparseMissFeedbackPending.erase(
                    sparseMissFeedbackPending.begin(),
                    sparseMissFeedbackPending.begin() + (sparseMissFeedbackPending.size() - keepCount));
            }
        }

        // Reset command allocator and command list
        ctx.commandAllocator->Reset();
        commandList->Reset(ctx.commandAllocator.Get(), nullptr);
        const uint32_t gpuTimestampBase = frameIndex * kGpuTimestampCount;
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 0);
        }

        if (sparseBackendRequested && sparseVoxelWorldReady) {
            sparseVoxelWorld.BeginFrame();
            sparseGpuResources.BeginFrame(frameIndex);
            if (sparseSurfaceGpuResources.IsInitialized()) {
                sparseSurfaceGpuResources.BeginFrame(frameIndex);
            }
            sparseSurfaceRasterFacesLastFrame = 0;
            sparseUploadRequeuesLastFrame = 0;
            sparseInvalidationRequeuesLastFrame = 0;
            sparsePageTablePublishRetriesLastFrame = 0;
            sparseMidClipmapUploadRetriesLastFrame = 0;
            sparseSurfaceUploadRetriesLastFrame = 0;
            sparseUploadBudgetLastFrame = 0;
            if (sparseGpuPageTableResetPending) {
                SparsePageTableGpuUploadTicket resetTicket;
                if (sparseGpuResources.StagePageTableReset(&resetTicket) &&
                    sparseGpuResources.EmitPageTableCopy(commandList.Get(), resetTicket)) {
                    sparseGpuPageTableResetPending = false;
                    spdlog::info("Sparse GPU page table reset uploaded");
                } else {
                    spdlog::warn("Sparse GPU page table reset upload failed; sparse visual path remains unsafe");
                }
            }
            for (uint32_t invalidationIndex = 0;
                 invalidationIndex < sparseInvalidationBudget;
                 ++invalidationIndex) {
                Simulation::SparsePageInvalidationPacket invalidation;
                if (!sparseVoxelWorld.PopNextInvalidation(&invalidation)) {
                    break;
                }

                SparsePageTableGpuUploadTicket invalidationTicket;
                if (!sparseGpuResources.StagePageTableInvalidation(
                        invalidation.entryIndex,
                        &invalidationTicket) ||
                    !sparseGpuResources.EmitPageTableCopy(commandList.Get(), invalidationTicket)) {
                    sparseVoxelWorld.RequeueInvalidationFront(invalidation);
                    ++sparseInvalidationRequeuesLastFrame;
                    spdlog::warn(
                        "Sparse page-table invalidation upload failed for ({},{},{}) slot={} page={} generation={}",
                        invalidation.coord.x,
                        invalidation.coord.y,
                        invalidation.coord.z,
                        invalidation.entryIndex,
                        invalidation.pageIndex,
                        invalidation.generation);
                    break;
                }
            }
            for (uint32_t publishIndex = 0;
                 publishIndex < sparsePageTablePublishBudget && !sparsePendingPageTablePublishes.empty();
                 ++publishIndex) {
                const uint32_t entryIndex = sparsePendingPageTablePublishes.front();
                sparsePendingPageTablePublishes.pop_front();
                const auto& sparsePageTable = sparseVoxelWorld.GetPool().PageTable();
                if (entryIndex >= sparsePageTable.Entries().size()) {
                    continue;
                }

                SparsePageTableGpuUploadTicket pageTableTicket;
                if (!sparseGpuResources.StagePageTableEntry(
                        entryIndex,
                        sparsePageTable.Entries()[entryIndex],
                        &pageTableTicket) ||
                    !sparseGpuResources.EmitPageTableCopy(commandList.Get(), pageTableTicket)) {
                    sparsePendingPageTablePublishes.push_front(entryIndex);
                    ++sparsePageTablePublishRetriesLastFrame;
                    break;
                }
            }
            if (sparseClipmapTileCacheReady &&
                sparseClipmapPolicy.IsEnabled()) {
                const bool uploadHeightClipmap =
                    sparseClipmapTileCache.HeightDirtySerial() != sparseMidClipmapUploadedHeightSerial;
                const bool uploadVoxelClipmap =
                    sparseClipmapTileCache.VoxelDirtySerial() != sparseMidClipmapUploadedVoxelSerial;
                if (!uploadHeightClipmap && !uploadVoxelClipmap) {
                    // Both clipmap layers already match the CPU cache.
                } else {
                Simulation::SparseClipmapGpuSnapshot midSnapshot;
                if (sparseClipmapTileCache.BuildGpuSnapshot(midSnapshot)) {
                    SparseMidClipmapGpuUploadTicket midTicket;
                    if (sparseGpuResources.StageMidClipmapSnapshot(
                            midSnapshot,
                            &midTicket,
                            uploadHeightClipmap,
                            uploadVoxelClipmap) &&
                        sparseGpuResources.EmitMidClipmapCopy(commandList.Get(), midTicket)) {
                        if (uploadHeightClipmap) {
                            sparseMidClipmapUploadedHeightSerial = sparseClipmapTileCache.HeightDirtySerial();
                        }
                        if (uploadVoxelClipmap) {
                            sparseMidClipmapUploadedVoxelSerial = sparseClipmapTileCache.VoxelDirtySerial();
                            sparseClipmapTileCache.ClearVoxelDirtyRange();
                        }
                    } else {
                        ++sparseMidClipmapUploadRetriesLastFrame;
                    }
                }
                }
            }
            const auto& sparseStatsBeforeUpload = sparseVoxelWorld.GetStats();
            const bool sparseFeedbackUploadPressure =
                sparseReplacementEvictionsLastFrame > 0 ||
                sparseMissFeedbackConsumedLastFrame > 0 ||
                !sparseMissFeedbackPending.empty();
            uint32_t sparseUploadBudgetThisFrame =
                sparseStatsBeforeUpload.residentBricks < sparseBootstrapResidentTarget
                    ? std::max(sparseUploadBudget, sparseBootstrapUploadBudget)
                    : sparseUploadBudget;
            if (sparseFeedbackUploadPressure) {
                sparseUploadBudgetThisFrame =
                    std::max(sparseUploadBudgetThisFrame, sparseFeedbackUploadBudget);
            }
            sparseUploadBudgetThisFrame =
                scaleRuntimeBudget(sparseUploadBudgetThisFrame, sparseRuntimeBudgetScale, 1u);
            sparseUploadBudgetLastFrame = sparseUploadBudgetThisFrame;
            for (uint32_t uploadIndex = 0; uploadIndex < sparseUploadBudgetThisFrame; ++uploadIndex) {
                Simulation::SparseBrickUploadPacket packet;
                if (!sparseVoxelWorld.PopNextUpload(&packet)) {
                    break;
                }
                SparseBrickGpuUploadTicket uploadTicket;
                if (!sparseGpuResources.StageBrickUpload(packet, &uploadTicket)) {
                    if (sparseVoxelWorld.RequeueUploadFront(packet)) {
                        ++sparseUploadRequeuesLastFrame;
                    }
                    spdlog::warn(
                        "Sparse brick upload staging failed for ({},{},{}) page={} generation={}",
                        packet.coord.x,
                        packet.coord.y,
                        packet.coord.z,
                        packet.pageIndex,
                        packet.generation);
                    break;
                }
                if (!sparseGpuResources.EmitUploadCopy(commandList.Get(), uploadTicket)) {
                    if (sparseVoxelWorld.RequeueUploadFront(packet)) {
                        ++sparseUploadRequeuesLastFrame;
                    }
                    spdlog::warn(
                        "Sparse brick upload copy failed for ({},{},{}) page={} generation={}",
                        packet.coord.x,
                        packet.coord.y,
                        packet.coord.z,
                        packet.pageIndex,
                        packet.generation);
                    break;
                }
                // The CPU table is published after brick and occupancy copies are
                // queued. Then the matching GPU page-table entry is staged and
                // copied last, so shader lookup cannot point at an uncopied page.
                if (!sparseVoxelWorld.CompleteUpload(packet)) {
                    if (sparseVoxelWorld.RequeueUploadFront(packet)) {
                        ++sparseUploadRequeuesLastFrame;
                    }
                    spdlog::warn(
                        "Sparse brick publish failed for ({},{},{}) page={} generation={}",
                        packet.coord.x,
                        packet.coord.y,
                        packet.coord.z,
                        packet.pageIndex,
                        packet.generation);
                    break;
                }

                uint32_t pageTableEntryIndex = UINT32_MAX;
                const auto& sparsePageTable = sparseVoxelWorld.GetPool().PageTable();
                if (!sparsePageTable.TryGetEntryIndex(packet.coord, &pageTableEntryIndex) ||
                    pageTableEntryIndex >= sparsePageTable.Entries().size()) {
                    spdlog::warn(
                        "Sparse page-table slot lookup failed for ({},{},{})",
                        packet.coord.x,
                        packet.coord.y,
                        packet.coord.z);
                    break;
                }

                // Publish the page-table entry on a later command list. This
                // keeps the renderer from seeing a BrickCoord until the payload,
                // occupancy, and generation copies are safely ordered ahead of
                // a subsequent frame's draw.
                sparsePendingPageTablePublishes.push_back(pageTableEntryIndex);
            }

            if (sparseSurfaceGpuResources.IsInitialized()) {
                const uint32_t surfaceSerial = sparseVoxelWorld.GetSurfaceCache().GetStats().serial;
                if (surfaceSerial != sparseSurfaceUploadedSerial) {
                    Simulation::SparseSurfaceGpuSnapshot surfaceSnapshot;
                    if (sparseVoxelWorld.GetSurfaceCache().BuildGpuSnapshot(surfaceSnapshot)) {
                        SparseSurfaceUploadTicket surfaceTicket;
                        if (sparseSurfaceGpuResources.StageSnapshot(surfaceSnapshot, &surfaceTicket) &&
                            sparseSurfaceGpuResources.EmitCopy(commandList.Get(), surfaceTicket)) {
                            sparseSurfaceUploadedSerial = surfaceSnapshot.serial;
                        } else {
                            ++sparseSurfaceUploadRetriesLastFrame;
                        }
                    } else {
                        ++sparseSurfaceUploadRetriesLastFrame;
                        spdlog::warn("Sparse surface GPU snapshot build failed");
                    }
                }
            }
        }

        // ===== UPDATE VOXEL DATA SOURCE =====
        // In static layout mode, copy a fixed 2x2 patch of pre-generated chunks
        // into the 256x128x256 buffer each frame. This bypasses streaming so we
        // can validate copy/origin and rendering in isolation. Otherwise, use the
        // normal infinite chunk streaming path.
        if (useStaticChunkLayout) {
            perfPhaseStart = SDL_GetPerformanceCounter();
            voxelWorld->CopyStatic2x2Chunks(commandQueue->GetCommandQueue());
            perfChunkUpdateMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        } else if (!sparseRuntimeTestMode && voxelWorld->IsUsingInfiniteChunks()) {
            perfPhaseStart = SDL_GetPerformanceCounter();
            const glm::vec3 cameraBeforeChunkUpdate = cameraPos;
            const glm::vec3 oldRegionOrigin = voxelWorld->GetRegionOriginWorld();
            const glm::vec3 recenterDelta = voxelWorld->UpdateChunks(
                device->GetDevice(),
                commandQueue->GetCommandQueue(),  // Uses internal cmd list, not frame cmdList
                cameraPos
            );
            if (glm::length(recenterDelta) > 0.01f) {
                const glm::vec3 newRegionOrigin = voxelWorld->GetRegionOriginWorld();
                const bool cameraChanged =
                    glm::length(cameraPos - cameraBeforeChunkUpdate) > 0.001f;
                if (cameraChanged) {
                    spdlog::error("RECENTER INVARIANT VIOLATION: camera world mutated during render-window update before=({:.3f},{:.3f},{:.3f}) after=({:.3f},{:.3f},{:.3f})",
                        cameraBeforeChunkUpdate.x, cameraBeforeChunkUpdate.y, cameraBeforeChunkUpdate.z,
                        cameraPos.x, cameraPos.y, cameraPos.z);
                }
                if (enableDiagnostics) {
                    spdlog::debug("RECENTER_INVARIANT oldOrigin=({:.0f},{:.0f},{:.0f}) newOrigin=({:.0f},{:.0f},{:.0f}) delta=({:.0f},{:.0f},{:.0f}) cameraBefore=({:.2f},{:.2f},{:.2f}) cameraAfter=({:.2f},{:.2f},{:.2f}) cameraLocalBefore=({:.2f},{:.2f},{:.2f}) cameraLocalAfter=({:.2f},{:.2f},{:.2f}) changed={}",
                        oldRegionOrigin.x, oldRegionOrigin.y, oldRegionOrigin.z,
                        newRegionOrigin.x, newRegionOrigin.y, newRegionOrigin.z,
                        recenterDelta.x, recenterDelta.y, recenterDelta.z,
                        cameraBeforeChunkUpdate.x, cameraBeforeChunkUpdate.y, cameraBeforeChunkUpdate.z,
                        cameraPos.x, cameraPos.y, cameraPos.z,
                        cameraBeforeChunkUpdate.x - oldRegionOrigin.x,
                        cameraBeforeChunkUpdate.y - oldRegionOrigin.y,
                        cameraBeforeChunkUpdate.z - oldRegionOrigin.z,
                        cameraPos.x - newRegionOrigin.x,
                        cameraPos.y - newRegionOrigin.y,
                        cameraPos.z - newRegionOrigin.z,
                        cameraChanged ? 1 : 0);
                }
            }

            // DEBUG: Track how many chunks have been copied into each buffer so far.
            // This should climb toward ~32 (4x2x4) and then stabilize when standing still.
            if (frameCount % 60 == 0) {
                int readIdx = voxelWorld->GetReadBufferIndex();
                size_t copiedRead  = voxelWorld->GetCopiedChunkCount(readIdx);
                size_t copiedWrite = voxelWorld->GetCopiedChunkCount(1 - readIdx);
                spdlog::debug("Copied chunks: READ={} WRITE={} (readIdx={})",
                    copiedRead, copiedWrite, readIdx);
            }
            perfChunkUpdateMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        }

        // Convert camera position into local 256^3-buffer coordinates. In static
        // 2x2 layout mode, the grid is fixed at world origin so local==world.
        // With streaming enabled, the voxel grid is a moving window around the
        // camera, so we subtract the region origin.
        glm::vec3 regionOriginWorld;
        glm::vec3 cameraPosLocal;
        if (useStaticChunkLayout) {
            regionOriginWorld = glm::vec3(0.0f);
            cameraPosLocal = cameraPos;
        } else if (sparseRuntimeTestMode) {
            regionOriginWorld = glm::floor(cameraPos - glm::vec3(
                static_cast<float>(voxelWorld->GetGridSizeX()) * 0.5f,
                static_cast<float>(voxelWorld->GetGridSizeY()) * 0.5f,
                static_cast<float>(voxelWorld->GetGridSizeZ()) * 0.5f));
            cameraPosLocal = cameraPos - regionOriginWorld;
        } else {
            regionOriginWorld = voxelWorld->GetRegionOriginWorld();
            cameraPosLocal = voxelWorld->WorldToRenderLocal(cameraPos);
        }

        nextGroundQueryRegionOriginWorld = regionOriginWorld;
        nextGroundQueryFeetWorld = cameraPos - glm::vec3(0, playerHeight, 0);
        nextBrushQueryRegionOriginWorld = regionOriginWorld;
        brushRayOriginWorld = cameraPos;
        glm::vec3 brushRayOriginLocal = cameraPosLocal;
        nextBrushQueryOriginWorld = brushRayOriginWorld;
        nextBrushQueryDirectionWorld = rayDir;

        // === GPU GROUND DETECTION RAYCAST (for player collision) ===
        // Cast a ray straight down from player FEET position to find ground
        // Camera is at eye level, so subtract playerHeight to get feet position
        glm::vec3 downDir = glm::vec3(0, -1, 0);
        if (sparseRuntimeTestMode && sparseGpuResources.IsInitialized()) {
            const auto& sparseStats = sparseGpuResources.GetStats();
            physicsDispatcher->DispatchSparseRaycast(
                commandList.Get(),
                *voxelWorld,
                sparseGpuResources.BrickPoolSRV(),
                sparseGpuResources.PageTableSRV(),
                sparseGpuResources.OccupancySRV(),
                sparseStats.maxBrickPages,
                sparseStats.pageTableCapacity,
                cameraPos - glm::vec3(0, playerHeight - 2.0f, 0),
                downDir,
                512.0f,
                true);
        } else {
            physicsDispatcher->DispatchGroundRaycast(commandList.Get(), *voxelWorld, cameraPos - glm::vec3(0, playerHeight, 0), downDir);
        }

        // === GPU BRUSH RAYCASTING ===
        // Target from the camera/crosshair. Traversal-friendly placement should
        // be handled as brush policy, not by moving the raw ray origin into the
        // collision body; doing that made empty-air strokes resolve underfoot.
        if (sparseRuntimeTestMode && sparseGpuResources.IsInitialized()) {
            const auto& sparseStats = sparseGpuResources.GetStats();
            physicsDispatcher->DispatchSparseRaycast(
                commandList.Get(),
                *voxelWorld,
                sparseGpuResources.BrickPoolSRV(),
                sparseGpuResources.PageTableSRV(),
                sparseGpuResources.OccupancySRV(),
                sparseStats.maxBrickPages,
                sparseStats.pageTableCapacity,
                brushRayOriginWorld,
                rayDir,
                kBrushMaxInteractionDistance,
                false);
        } else {
            physicsDispatcher->DispatchBrushRaycast(commandList.Get(), *voxelWorld, brushRayOriginWorld, rayDir);
        }

        // Begin frame - transitions back buffer, sets render target, viewport, etc.
        renderer->BeginFrame(commandList.Get(), frameIndex);

        // =============================================================================
        // Render ImGui UI (Pause Menu System)
        // =============================================================================
        imguiBackend.NewFrame();

        // Render pause menu and all UI panels (only when pause menu is open)
        pauseMenu.Render(paused, frameCount, cameraPos, materialPalette, brushPanel, brushController);

        // Debug logging removed to reduce spam

        // Get GPU raycast results (16 bytes from previous frame)
        auto gpuRaycastResult = voxelWorld->GetBrushRaycastResult();
        auto groundRaycastResult = voxelWorld->GetGroundRaycastResult();
        const bool sparseGroundAuthoritative = sparseBackendRequested && sparseVoxelWorldReady;
        if (sparseGroundAuthoritative) {
            const glm::vec3 feetWorld = cameraPos - glm::vec3(0.0f, playerHeight, 0.0f);
            const auto sparseGround = sparseVoxelWorld.Raycast(
                feetWorld.x,
                feetWorld.y + 2.0f,
                feetWorld.z,
                0.0f,
                -1.0f,
                0.0f,
                512.0f);
            if (sparseGround.hit) {
                groundRaycastResult.posX = static_cast<float>(sparseGround.voxelX) + 0.5f;
                groundRaycastResult.posY = static_cast<float>(sparseGround.voxelY) + 1.0f;
                groundRaycastResult.posZ = static_cast<float>(sparseGround.voxelZ) + 0.5f;
                groundRaycastResult.normalPacked = PackNormalForReadback(glm::ivec3(0, 1, 0), true);
                groundRaycastResult.hasValidPosition = true;
                completedGroundQueryRegionOriginWorld = regionOriginWorld;
                completedGroundQueryFeetWorld = feetWorld;
                hasCompletedGroundQuery = true;
            } else {
                groundRaycastResult.hasValidPosition = false;
                hasCompletedGroundQuery = false;
            }
        } else if (!hasCompletedGroundQuery || !groundRaycastResult.hasValidPosition) {
            groundRaycastResult.hasValidPosition = false;
        }

        glm::vec3 brushHitWorld(0.0f);
        glm::ivec3 brushHitNormal(0);
        bool brushHitNormalValid = false;
        bool brushHitValid = false;
        bool brushHitTracksCurrentRay = false;
        if (hasCompletedBrushQuery && gpuRaycastResult.hasValidPosition) {
            brushHitNormal = DecodePackedNormal(gpuRaycastResult.normalPacked, brushHitNormalValid);
            brushHitWorld = glm::vec3(
                gpuRaycastResult.posX,
                gpuRaycastResult.posY,
                gpuRaycastResult.posZ
            );

            float hitDistance = glm::length(brushHitWorld - completedBrushQueryOriginWorld);
            glm::vec3 brushHitCurrentLocal = voxelWorld->WorldToRenderLocal(brushHitWorld);
            glm::vec3 queryHitDelta = brushHitWorld - completedBrushQueryOriginWorld;
            float queryRayDistance = glm::dot(queryHitDelta, completedBrushQueryDirectionWorld);
            glm::vec3 queryRayClosest = completedBrushQueryOriginWorld + completedBrushQueryDirectionWorld * queryRayDistance;
            float queryRayLateralError = glm::length(brushHitWorld - queryRayClosest);
            float queryRayTolerance = std::max(brushController.GetRadius() * 1.5f, queryRayDistance * 0.04f + 2.0f);
            const float minBrushHitDistance = 1.0f;
            brushHitValid =
                hitDistance > minBrushHitDistance &&
                hitDistance < kBrushMaxInteractionDistance &&
                queryRayDistance > minBrushHitDistance &&
                queryRayLateralError <= queryRayTolerance &&
                brushHitCurrentLocal.x >= 0.0f && brushHitCurrentLocal.x < voxelWorld->GetGridSizeX() &&
                brushHitCurrentLocal.y >= 0.0f && brushHitCurrentLocal.y < voxelWorld->GetGridSizeY() &&
                brushHitCurrentLocal.z >= 0.0f && brushHitCurrentLocal.z < voxelWorld->GetGridSizeZ();
            if (brushHitValid) {
                buildStrokeState.hasStableAimDistance = true;
                buildStrokeState.stableAimDistance = std::clamp(hitDistance, 4.0f, kBrushMaxInteractionDistance);
                buildStrokeState.stableAimWorldPosition = brushHitWorld;
                const glm::vec3 currentHitDelta = brushHitWorld - cameraPos;
                const float currentRayDistance = glm::dot(currentHitDelta, rayDir);
                const glm::vec3 currentRayClosest = cameraPos + rayDir * currentRayDistance;
                const float currentRayLateralError = glm::length(brushHitWorld - currentRayClosest);
                const float currentRayTolerance = std::max(brushController.GetRadius() * 1.5f, currentRayDistance * 0.05f + 3.0f);
                brushHitTracksCurrentRay =
                    currentRayDistance > minBrushHitDistance &&
                    currentRayLateralError <= currentRayTolerance;
            }
        }
        if (enableSparseOnlyRaymarch && sparseVoxelWorldReady && !brushHitValid) {
            const auto sparseHit = sparseVoxelWorld.Raycast(
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                rayDir.x,
                rayDir.y,
                rayDir.z,
                kBrushMaxInteractionDistance);
            if (sparseHit.hit && sparseHit.distance > 1.0f && sparseHit.distance < kBrushMaxInteractionDistance) {
                brushHitWorld = glm::vec3(
                    static_cast<float>(sparseHit.voxelX) + 0.5f,
                    static_cast<float>(sparseHit.voxelY) + 0.5f,
                    static_cast<float>(sparseHit.voxelZ) + 0.5f);
                brushHitNormal = glm::ivec3(sparseHit.normalX, sparseHit.normalY, sparseHit.normalZ);
                brushHitNormalValid =
                    sparseHit.normalX != 0 ||
                    sparseHit.normalY != 0 ||
                    sparseHit.normalZ != 0;
                brushHitValid = true;
                brushHitTracksCurrentRay = true;
                buildStrokeState.hasStableAimDistance = true;
                buildStrokeState.stableAimDistance = std::clamp(sparseHit.distance, 4.0f, kBrushMaxInteractionDistance);
                buildStrokeState.stableAimWorldPosition = brushHitWorld;
            }
        }
        if (brushHitValid) {
            voxelWorld->UpdateTargetVoxelDebug(
                static_cast<int32_t>(std::floor(brushHitWorld.x)),
                static_cast<int32_t>(std::floor(brushHitWorld.y)),
                static_cast<int32_t>(std::floor(brushHitWorld.z)),
                brushHitNormalValid ? brushHitNormal.x : 0,
                brushHitNormalValid ? brushHitNormal.y : 0,
                brushHitNormalValid ? brushHitNormal.z : 0);
        }

        const bool brushInputActive = brushController.IsPainting() || brushController.IsErasing();
        const float responsiveAimDistance = brushHitValid
            ? buildStrokeState.stableAimDistance
            : (buildStrokeState.hasStableAimDistance ? buildStrokeState.stableAimDistance : kBrushDefaultAimDistance);
        glm::vec3 responsiveAimWorld = (brushHitValid && brushHitTracksCurrentRay)
            ? brushHitWorld
            : cameraPos + rayDir * responsiveAimDistance;
        if (!brushHitValid && !brushInputActive && buildStrokeState.hasStableAimDistance) {
            // While just previewing, keep a stable distance but update direction
            // every frame. This makes the brush feel attached to the crosshair
            // even when async GPU hit readback is one or two frames behind.
            responsiveAimWorld = cameraPos + rayDir * buildStrokeState.stableAimDistance;
        }
        buildStrokeState.stableAimWorldPosition = responsiveAimWorld;

        // === COLLISION DETECTION ===
        if (gameplayInputEnabled && !flightMode) {
            // Normal mode - ground collision and gravity
            // Ground raycast hit detection
            if (hasCompletedGroundQuery && groundRaycastResult.hasValidPosition) {
                bool groundNormalValid = false;
                const glm::ivec3 groundNormal = DecodePackedNormal(groundRaycastResult.normalPacked, groundNormalValid);
                glm::vec3 groundHitWorld = glm::vec3(
                    groundRaycastResult.posX,
                    groundRaycastResult.posY,
                    groundRaycastResult.posZ
                );

                glm::vec2 groundXZ(groundHitWorld.x, groundHitWorld.z);
                glm::vec2 lastFeetXZ(completedGroundQueryFeetWorld.x, completedGroundQueryFeetWorld.z);
                glm::vec2 currentFeetXZ(cameraPos.x, cameraPos.z);
                bool groundResultMatchesQuery = glm::length(groundXZ - lastFeetXZ) < 2.0f;
                bool queryStillRelevant =
                    glm::length(currentFeetXZ - lastFeetXZ) < 6.0f &&
                    std::abs((cameraPos.y - playerHeight) - completedGroundQueryFeetWorld.y) < 8.0f;
                bool groundNormalLooksWalkable = groundNormalValid && groundNormal.y > 0;

                if (groundResultMatchesQuery && queryStillRelevant && groundNormalLooksWalkable) {
                    // Terrain is ready! Ground detection found solid ground.
                    if (!terrainReady) {
                        terrainReady = true;
                        spdlog::info("Terrain ready - gravity enabled");
                    }

                    // Ground raycast hit something - use it for collision
                    float groundWorldY = groundHitWorld.y;

                    // Player feet position in world space
                    float playerFeetWorldY = cameraPos.y - playerHeight;
                    float groundDelta = playerFeetWorldY - groundWorldY;

                    bool onGround = false;

                    if (groundDelta < -0.05f) {
                        float climbHeight = -groundDelta;
                        if (climbHeight <= stepHeight + 0.25f) {
                            cameraPos.y = groundWorldY + playerHeight;
                            cameraVelocityY = 0.0f;
                            onGround = true;
                        } else {
                            // Large upward corrections are usually stale
                            // raycast data, steep walls, ceilings, or a render
                            // recenter/cache-refill edge. Do not mutate X/Z or
                            // snap Y here; that created visible teleports.
                            static uint32_t rejectedGroundSnapCount = 0;
                            if (++rejectedGroundSnapCount % 60 == 1) {
                                spdlog::debug("Rejected large ground snap: climbHeight={:.2f} groundWorld=({:.2f},{:.2f},{:.2f}) feetY={:.2f}",
                                    climbHeight,
                                    groundHitWorld.x,
                                    groundHitWorld.y,
                                    groundHitWorld.z,
                                    playerFeetWorldY);
                            }
                        }
                    } else if (cameraVelocityY <= 0.0f && groundDelta <= stepHeight) {
                        cameraPos.y = groundWorldY + playerHeight;
                        cameraVelocityY = 0.0f;
                        onGround = true;
                    }

                    // Space to jump (if on ground and not double-click)
                    if (jumpPressed && onGround && !flightTogglePressed) {
                        cameraVelocityY = 20.0f;  // Jump velocity
                    }
                }
            }
            // No ground detected - terrain not ready yet or in air above terrain
        }

        cameraPosLocal = (useStaticChunkLayout || sparseRuntimeTestMode)
            ? cameraPos - regionOriginWorld
            : voxelWorld->WorldToRenderLocal(cameraPos);

        // === HORIZONTAL COLLISION (Cave/Wall Detection) ===
        // Use brush raycast to check for walls/obstacles in movement direction
        // If brush raycast hits something close (<2 voxels) in the direction we're looking,
        // it means there's a wall/obstacle ahead
        if (brushHitValid) {
            // Calculate distance to hit point
            glm::vec3 hitPosWorld = brushHitWorld;
            const glm::vec3 currentBrushOriginWorld = cameraPos;
            float distanceToHit = glm::length(hitPosWorld - currentBrushOriginWorld);

            // If we're about to walk into a wall (hit within player radius + small margin)
            // and the hit is roughly at player height (not floor/ceiling), stop horizontal movement
            float hitHeight = hitPosWorld.y - (cameraPos.y - playerHeight);
            bool isWall = (distanceToHit < playerRadius + 1.0f) && (hitHeight > 0.2f) && (hitHeight < playerHeight - 0.5f);

            if (isWall) {
                // Prevent movement in the direction of the wall by checking if we're moving towards it
                glm::vec3 dirToHit = glm::normalize(hitPosWorld - currentBrushOriginWorld);
                glm::vec3 horizontalMoveDir = glm::vec3(cameraForward.x, 0, cameraForward.z);

                // If we're moving towards the wall, reduce movement
                if (glm::length(horizontalMoveDir) > 0.01f) {
                    horizontalMoveDir = glm::normalize(horizontalMoveDir);
                    float dotProduct = glm::dot(horizontalMoveDir, glm::vec3(dirToHit.x, 0, dirToHit.z));

                    // Moving towards wall - slide along it
                    if (dotProduct > 0.5f) {
                        // Simple slide: remove component of movement towards wall
                        glm::vec3 slideDir = horizontalMoveDir - dirToHit * dotProduct;
                        if (glm::length(slideDir) > 0.01f) {
                            // Apply reduced movement (sliding along wall)
                            slideDir = glm::normalize(slideDir);
                        }
                    }
                }
            }
        }

        // =====================================================
        // PHYSICS FIRST, THEN BRUSH (same as sand simulator)
        // =====================================================
        // Order matters! Physics copies READ->WRITE (via shader), then brush paints on top.
        // If brush was first, physics would overwrite painted voxels.

        glm::vec3 brushPlacementWorld = responsiveAimWorld;
        bool brushPlacementPreviewValid = true;
        bool brushPlacementCloseRamp = false;

        // Run physics simulation only after the streamed render window is
        // populated. Scanning the full vertical buffer while chunks are still
        // sparse can stall startup; traversal responsiveness is more important
        // than simulating incomplete terrain.
        bool physicsRanThisFrame = false;
        bool physicsSkippedForBudget = false;
        const auto& prePhysicsStats = voxelWorld->GetStreamingStats();
        const bool infinitePhysicsAllowed =
            !voxelWorld->IsUsingInfiniteChunks() || enableInfinitePhysics;
        const uint32_t criticalPhysicsCoverage =
            prePhysicsStats.expectedVisibleChunks == 0
                ? 0
                : (prePhysicsStats.expectedVisibleChunks * 3u) / 4u;
        const bool streamingReadyForPhysics =
            disableRuntimePhysics ||
            !voxelWorld->IsUsingInfiniteChunks() ||
            (prePhysicsStats.expectedVisibleChunks > 0 &&
             prePhysicsStats.cachedReadChunks >= criticalPhysicsCoverage &&
             prePhysicsStats.cachedWriteChunks >= criticalPhysicsCoverage);
        const bool hasPhysicsDirtyRegion = physicsDirtyFramesRemaining > 0;
        const float physicsCameraSpeed =
            glm::length(cameraPos - lastPhysicsSchedulerCameraWorld) / std::max(dt, 0.001f);
        lastPhysicsSchedulerCameraWorld = cameraPos;
        const uint32_t physicsInterval =
            hasPhysicsDirtyRegion ? 1u :
            (!streamingReadyForPhysics || smoothedFrameMs > 17.2f) ? 8u :
            (physicsCameraSpeed > 80.0f ? 4u : 6u);
        const uint32_t physicsScanBudgetChunks =
            hasPhysicsDirtyRegion ? 64u :
            (smoothedFrameMs > 17.2f ? 64u : 128u);
        glm::vec3 physicsScanCenterLocal = cameraPosLocal;
        if (hasPhysicsDirtyRegion) {
            physicsScanCenterLocal = physicsDirtyCenterWorld - voxelWorld->GetRegionOriginWorld();
        }
        const bool physicsDueThisFrame = (frameCount % physicsInterval) == 0;
        perfPhaseStart = SDL_GetPerformanceCounter();
        if (!paused && !disableRuntimePhysics && infinitePhysicsAllowed && streamingReadyForPhysics && physicsDueThisFrame) {
            // Scan chunks to determine which are active
            physicsDispatcher->DispatchChunkScan(
                commandList.Get(),
                *voxelWorld,
                *chunkManager,
                static_cast<uint32_t>(frameCount),
                physicsScanCenterLocal,
                physicsScanBudgetChunks
            );

            // Run physics on active chunks using ExecuteIndirect
            physicsDispatcher->DispatchPhysicsIndirect(
                commandList.Get(),
                *voxelWorld,
                *chunkManager,
                1.0f/60.0f,
                static_cast<uint32_t>(frameCount)
            );
            physicsRanThisFrame = true;
            physicsDispatchCount++;
        } else if (!paused && !disableRuntimePhysics) {
            physicsSkippedForBudget = true;
            physicsBudgetSkipCount++;
        }
        perfPhysicsSubmitMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);

        // Apply brush painting AFTER physics (so brush changes aren't overwritten)
        // Use GPU raycast position, or fallback to fixed distance in empty air
        perfPhaseStart = SDL_GetPerformanceCounter();
        if (brushController.IsPainting() || brushController.IsErasing()) {
            glm::vec3 brushPos;
            glm::vec3 intendedBrushWorld = responsiveAimWorld;
            const bool buildStroke = brushController.IsPainting() && !brushController.IsErasing();
            if (!buildStroke) {
                buildStrokeState.active = false;
                buildStrokeState.rayDistance = kBrushDefaultAimDistance;
                buildStrokeState.closeRampActive = false;
                buildStrokeState.closeRampHorizontalDistance = 0.0f;
                buildStrokeState.hasLastBrushRayDirection = false;
            }

            if (brushHitValid && brushHitTracksCurrentRay) {
                if (buildStroke) {
                    // Start held build strokes at the hit point, then actively
                    // pull the brush back along the current line of sight. If
                    // we reset to hitDistance every frame, the stroke only
                    // advances when the ray hits the new blob, which feels
                    // like a slow buffer/paint lag on long shots.
                    const float hitDistance = glm::length(brushHitWorld - cameraPos);
                    const float pullStep = std::max(
                        kBrushStrokePullSpeed,
                        brushController.GetRadius() * 38.0f) * std::max(dt, 0.0f);
                    if (!buildStrokeState.active) {
                        buildStrokeState.rayDistance = std::clamp(
                            hitDistance,
                            4.0f,
                            kBrushMaxInteractionDistance);
                    } else {
                        const float currentDistance = std::min(buildStrokeState.rayDistance, hitDistance);
                        buildStrokeState.rayDistance = std::clamp(
                            currentDistance - pullStep,
                            4.0f,
                            kBrushMaxInteractionDistance);
                    }
                    buildStrokeState.active = true;
                    intendedBrushWorld = cameraPos + rayDir * buildStrokeState.rayDistance;
                    brushPos = sparseRuntimeTestMode
                        ? intendedBrushWorld - regionOriginWorld
                        : voxelWorld->WorldToRenderLocal(intendedBrushWorld);
                } else {
                    // Use GPU raycast hit position (on solid voxel face).
                    // Convert the previous-frame world hit into the current toroidal render slot.
                    intendedBrushWorld = brushHitWorld;
                    brushPos = sparseRuntimeTestMode
                        ? brushHitWorld - regionOriginWorld
                        : voxelWorld->WorldToRenderLocal(brushHitWorld);
                }
                static int logCounter = 0;
                if (enableDiagnostics && logCounter++ % 60 == 0) {
                    spdlog::debug("Painting at raycast pos: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            } else {
                // Fallback: keep build strokes at their current working
                // distance so turning left/right follows the line of sight
                // instead of teleporting the brush back in front of the player.
                const float fallbackDistance = buildStroke
                    ? (buildStrokeState.active ? buildStrokeState.rayDistance : kBrushDefaultAimDistance)
                    : 12.0f;
                if (buildStroke) {
                    buildStrokeState.active = true;
                    const float pullStep = std::max(
                        kBrushStrokePullSpeed,
                        brushController.GetRadius() * 38.0f) * std::max(dt, 0.0f);
                    buildStrokeState.rayDistance = std::clamp(
                        fallbackDistance - pullStep,
                        4.0f,
                        kBrushMaxInteractionDistance);
                }
                const glm::vec3 fallbackWorld = buildStroke
                    ? cameraPos + rayDir * buildStrokeState.rayDistance
                    : cameraPos + rayDir * fallbackDistance;
                intendedBrushWorld = fallbackWorld;
                brushPos = (useStaticChunkLayout || sparseRuntimeTestMode)
                    ? fallbackWorld - regionOriginWorld
                    : voxelWorld->WorldToRenderLocal(fallbackWorld);

                // Clamp to grid bounds
                brushPos = glm::clamp(brushPos,
                    glm::vec3(0.5f),
                    glm::vec3(voxelWorld->GetGridSizeX() - 0.5f,
                             voxelWorld->GetGridSizeY() - 0.5f,
                             voxelWorld->GetGridSizeZ() - 0.5f));

                static int logCounter = 0;
                if (enableDiagnostics && logCounter++ % 60 == 0) {
                    spdlog::debug("Painting in air at: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            }

            bool closeRampAdjusted = false;
            const float intendedBrushEyeDistance = glm::length(intendedBrushWorld - cameraPos);
            const float closeRampWorldEligibility =
                std::max(16.0f, brushController.GetRadius() * 3.0f + playerHeight + playerRadius);
            if (buildStroke && intendedBrushEyeDistance <= closeRampWorldEligibility) {
                brushPos = ApplyCloseTraversalBrushFallback(
                    brushPos,
                    cameraPosLocal,
                    cameraForward,
                    playerHeight,
                    playerRadius,
                    brushController.GetRadius(),
                    dt,
                    buildStroke,
                    buildStrokeState,
                    &closeRampAdjusted);
            } else {
                buildStrokeState.closeRampActive = false;
                buildStrokeState.closeRampHorizontalDistance = 0.0f;
            }

            brushPos = glm::clamp(brushPos,
                glm::vec3(0.5f),
                glm::vec3(voxelWorld->GetGridSizeX() - 0.5f,
                         voxelWorld->GetGridSizeY() - 0.5f,
                         voxelWorld->GetGridSizeZ() - 0.5f));

            brushPlacementWorld = intendedBrushWorld;
            if (closeRampAdjusted && sparseRuntimeTestMode) {
                brushPlacementWorld = brushPos + regionOriginWorld;
            } else if (closeRampAdjusted && !useStaticChunkLayout) {
                glm::vec3 mappedWorld(0.0f);
                if (voxelWorld->RenderLocalToWorld(brushPos, mappedWorld)) {
                    brushPlacementWorld = mappedWorld;
                } else {
                    brushPlacementPreviewValid = false;
                }
            } else if (useStaticChunkLayout) {
                brushPlacementWorld = brushPos + regionOriginWorld;
            }
            brushPlacementCloseRamp = closeRampAdjusted;
            if (closeRampAdjusted) {
                static int rampLogCounter = 0;
                if (enableDiagnostics && rampLogCounter++ % 60 == 0) {
                    spdlog::debug("Close traversal brush ramp: local=({:.1f},{:.1f},{:.1f}) world=({:.1f},{:.1f},{:.1f})",
                        brushPos.x, brushPos.y, brushPos.z,
                        brushPlacementWorld.x, brushPlacementWorld.y, brushPlacementWorld.z);
                }
            }

            Input::BrushConstants brushConstants;
            brushConstants.positionX = brushPos.x;
            brushConstants.positionY = brushPos.y;
            brushConstants.positionZ = brushPos.z;
            brushConstants.radius = brushController.GetRadius();
            brushConstants.material = brushController.IsErasing() ? 0 : brushController.GetMaterial();
            brushConstants.mode = static_cast<uint32_t>(brushController.IsErasing() ? Input::BrushMode::Erase : brushController.GetMode());
            brushConstants.shape = static_cast<uint32_t>(brushController.GetShape());
            brushConstants.strength = 1.0f;
            brushConstants.gridSizeX = voxelWorld->GetGridSizeX();
            brushConstants.gridSizeY = voxelWorld->GetGridSizeY();
            brushConstants.gridSizeZ = voxelWorld->GetGridSizeZ();
            brushConstants.seed = static_cast<uint32_t>(frameCount);
            const bool brushNormalUsable = brushHitValid && brushHitTracksCurrentRay && brushHitNormalValid;
            brushConstants.hitNormalX = brushNormalUsable ? brushHitNormal.x : 0;
            brushConstants.hitNormalY = brushNormalUsable ? brushHitNormal.y : 0;
            brushConstants.hitNormalZ = brushNormalUsable ? brushHitNormal.z : 0;
            brushConstants.hasHitNormal = brushNormalUsable ? 1u : 0u;

            const uint32_t currentBrushKey =
                (brushConstants.mode & 0xffu) |
                ((brushConstants.shape & 0xffu) << 8) |
                ((brushConstants.material & 0xffffu) << 16);
            if (buildStrokeState.hasLastBrushWorldPosition &&
                buildStrokeState.lastBrushMode != currentBrushKey) {
                buildStrokeState.hasLastBrushWorldPosition = false;
                buildStrokeState.hasLastBrushRayDirection = false;
            }
            if (buildStrokeState.hasLastBrushWorldPosition) {
                const glm::vec3 toLastBrush = buildStrokeState.lastBrushWorldPosition - cameraPos;
                const float lastAlongCurrentRay = glm::dot(toLastBrush, rayDir);
                const glm::vec3 lastClosestOnCurrentRay = cameraPos + rayDir * lastAlongCurrentRay;
                const float lastLateralError = glm::length(buildStrokeState.lastBrushWorldPosition - lastClosestOnCurrentRay);
                const float rayDirectionDot = buildStrokeState.hasLastBrushRayDirection
                    ? glm::dot(glm::normalize(buildStrokeState.lastBrushRayDirection), rayDir)
                    : 1.0f;
                const float maxTrustedLateralError = std::max(
                    brushController.GetRadius() * 6.0f,
                    std::max(32.0f, std::max(lastAlongCurrentRay, 0.0f) * 0.12f));

                // A tiny angular mouse change at long brush distances can be
                // tens of voxels laterally. Do not reset continuity for that,
                // or held painting becomes a dotted line. Only break the sweep
                // when the camera ray makes an abrupt turn, or when the old
                // point is badly off the current ray and the ray direction also
                // changed enough that interpolating would draw a cross-space rod.
                if (lastAlongCurrentRay < -brushController.GetRadius() ||
                    rayDirectionDot < 0.82f ||
                    (lastLateralError > maxTrustedLateralError && rayDirectionDot < 0.94f)) {
                    buildStrokeState.hasLastBrushWorldPosition = false;
                    buildStrokeState.hasLastBrushRayDirection = false;
                }
            }

            std::array<glm::vec3, 192> stampWorldPositions = {};
            uint32_t stampCount = 0;
            const float brushRadius = std::max(brushController.GetRadius(), 1.0f);
            const float stampSpacing = std::max(1.0f, brushRadius * 0.45f);
            if (buildStrokeState.hasLastBrushWorldPosition) {
                const glm::vec3 segment = brushPlacementWorld - buildStrokeState.lastBrushWorldPosition;
                const float segmentLength = glm::length(segment);
                constexpr float kMaxTrustedBrushSweepDistance = 640.0f;
                if (segmentLength > 0.01f && segmentLength <= kMaxTrustedBrushSweepDistance) {
                    stampCount = std::clamp<uint32_t>(
                        static_cast<uint32_t>(std::ceil(segmentLength / stampSpacing)),
                        1u,
                        static_cast<uint32_t>(stampWorldPositions.size()));
                    for (uint32_t i = 0; i < stampCount; ++i) {
                        const float t = static_cast<float>(i + 1) / static_cast<float>(stampCount);
                        stampWorldPositions[i] = buildStrokeState.lastBrushWorldPosition + segment * t;
                    }
                } else {
                    stampWorldPositions[0] = brushPlacementWorld;
                    stampCount = 1;
                }
            } else {
                stampWorldPositions[0] = brushPlacementWorld;
                stampCount = 1;
            }

            uint32_t submittedBrushStamps = 0;
            bool submittedAnyBrushStamp = false;
            uint32_t acceptedBrushStamps = 0;
            bool acceptedAnyBrushStamp = false;
            glm::vec3 lastSubmittedBrushWorldPosition = buildStrokeState.hasLastBrushWorldPosition
                ? buildStrokeState.lastBrushWorldPosition
                : brushPlacementWorld;
            auto recordSparseBrushStamp = [&](const glm::vec3& worldPosition, uint32_t stampSeed) {
                if (!sparseBackendRequested || !sparseVoxelWorldReady) {
                    return;
                }
                sparseVoxelWorld.ApplyBrushEdit(
                    worldPosition.x,
                    worldPosition.y,
                    worldPosition.z,
                    brushConstants.radius,
                    brushConstants.material,
                    brushConstants.mode,
                    brushConstants.shape,
                    brushConstants.strength,
                    stampSeed,
                    brushConstants.hitNormalX,
                    brushConstants.hitNormalY,
                    brushConstants.hitNormalZ,
                    brushConstants.hasHitNormal != 0,
                    true);
            };
            for (uint32_t stampIndex = 0; stampIndex < stampCount; ++stampIndex) {
                const uint32_t stampSeed = static_cast<uint32_t>(frameCount + stampIndex);
                if (sparseRuntimeTestMode) {
                    recordSparseBrushStamp(stampWorldPositions[stampIndex], stampSeed);
                    ++acceptedBrushStamps;
                    acceptedAnyBrushStamp = true;
                    lastSubmittedBrushWorldPosition = stampWorldPositions[stampIndex];
                    continue;
                }
                if (!useStaticChunkLayout && voxelWorld->IsUsingInfiniteChunks()) {
                    if (!voxelWorld->EnsureWorldBrushVolumeCachedForReadWrite(
                            stampWorldPositions[stampIndex].x,
                            stampWorldPositions[stampIndex].y,
                            stampWorldPositions[stampIndex].z,
                            brushRadius)) {
                        voxelWorld->RecordPersistentWorldBrushEdit(
                            stampWorldPositions[stampIndex].x,
                            stampWorldPositions[stampIndex].y,
                            stampWorldPositions[stampIndex].z,
                            brushConstants.radius,
                            brushConstants.material,
                            brushConstants.mode,
                            brushConstants.shape,
                            brushConstants.strength,
                            static_cast<uint32_t>(frameCount + stampIndex),
                            brushConstants.hitNormalX,
                            brushConstants.hitNormalY,
                            brushConstants.hitNormalZ,
                            brushConstants.hasHitNormal != 0);
                        recordSparseBrushStamp(stampWorldPositions[stampIndex], stampSeed);
                        ++acceptedBrushStamps;
                        acceptedAnyBrushStamp = true;
                        lastSubmittedBrushWorldPosition = stampWorldPositions[stampIndex];
                        continue;
                    }
                }

                glm::vec3 stampLocal = useStaticChunkLayout
                    ? stampWorldPositions[stampIndex]
                    : voxelWorld->WorldToRenderLocal(stampWorldPositions[stampIndex]);
                if (stampLocal.x < 0.5f || stampLocal.y < 0.5f || stampLocal.z < 0.5f ||
                    stampLocal.x > static_cast<float>(voxelWorld->GetGridSizeX()) - 0.5f ||
                    stampLocal.y > static_cast<float>(voxelWorld->GetGridSizeY()) - 0.5f ||
                    stampLocal.z > static_cast<float>(voxelWorld->GetGridSizeZ()) - 0.5f) {
                    if (buildStroke) {
                        voxelWorld->RecordPersistentWorldBrushEdit(
                            stampWorldPositions[stampIndex].x,
                            stampWorldPositions[stampIndex].y,
                            stampWorldPositions[stampIndex].z,
                            brushConstants.radius,
                            brushConstants.material,
                            brushConstants.mode,
                            brushConstants.shape,
                            brushConstants.strength,
                            static_cast<uint32_t>(frameCount + stampIndex),
                            brushConstants.hitNormalX,
                            brushConstants.hitNormalY,
                            brushConstants.hitNormalZ,
                            brushConstants.hasHitNormal != 0);
                        recordSparseBrushStamp(stampWorldPositions[stampIndex], stampSeed);
                        ++acceptedBrushStamps;
                        acceptedAnyBrushStamp = true;
                        lastSubmittedBrushWorldPosition = stampWorldPositions[stampIndex];
                        continue;
                    }
                    continue;
                }

                brushConstants.positionX = stampLocal.x;
                brushConstants.positionY = stampLocal.y;
                brushConstants.positionZ = stampLocal.z;
                brushConstants.seed = stampSeed;
                physicsDispatcher->DispatchBrush(commandList.Get(), *voxelWorld, brushConstants);
                recordSparseBrushStamp(stampWorldPositions[stampIndex], stampSeed);
                ++submittedBrushStamps;
                submittedAnyBrushStamp = true;
                ++acceptedBrushStamps;
                acceptedAnyBrushStamp = true;
                lastSubmittedBrushWorldPosition = stampWorldPositions[stampIndex];
            }
            if (acceptedAnyBrushStamp || !buildStrokeState.hasLastBrushWorldPosition) {
                buildStrokeState.hasLastBrushWorldPosition = acceptedAnyBrushStamp;
                buildStrokeState.lastBrushWorldPosition = lastSubmittedBrushWorldPosition;
            }
            if (acceptedAnyBrushStamp) {
                buildStrokeState.hasLastBrushRayDirection = true;
                buildStrokeState.lastBrushRayDirection = rayDir;
            }
            buildStrokeState.lastBrushMode = currentBrushKey;
            buildStrokeState.sweepStampsLastFrame = acceptedBrushStamps;
        } else {
            buildStrokeState.active = false;
            buildStrokeState.rayDistance = kBrushDefaultAimDistance;
            buildStrokeState.closeRampActive = false;
            buildStrokeState.closeRampHorizontalDistance = 0.0f;
            buildStrokeState.hasLastBrushWorldPosition = false;
            buildStrokeState.hasLastBrushRayDirection = false;
            buildStrokeState.lastBrushMode = UINT32_MAX;
            buildStrokeState.sweepStampsLastFrame = 0;
        }
        if (brushPlacementPreviewValid) {
            if (!buildStrokeState.hasPreviewWorldPosition) {
                buildStrokeState.previewWorldPosition = brushPlacementWorld;
                buildStrokeState.hasPreviewWorldPosition = true;
            } else {
                const float previewBlend = std::clamp(dt * 22.0f, 0.28f, 1.0f);
                const float previewJump = glm::length(brushPlacementWorld - buildStrokeState.previewWorldPosition);
                if (previewJump > 96.0f) {
                    buildStrokeState.previewWorldPosition = brushPlacementWorld;
                } else {
                    buildStrokeState.previewWorldPosition =
                        buildStrokeState.previewWorldPosition +
                        (brushPlacementWorld - buildStrokeState.previewWorldPosition) * previewBlend;
                }
            }
        } else {
            buildStrokeState.hasPreviewWorldPosition = false;
        }
        perfBrushSubmitMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        const auto& postBrushStreamingStats = voxelWorld->GetStreamingStats();
        if ((brushController.IsPainting() || brushController.IsErasing()) && brushPlacementPreviewValid) {
            physicsDirtyCenterWorld = brushPlacementWorld;
            physicsDirtyFramesRemaining = std::max<uint32_t>(physicsDirtyFramesRemaining, 90u);
            ++physicsDirtyEvents;
        } else if (postBrushStreamingStats.gpuBrushEventsAppliedLastFrame > 0 ||
                   postBrushStreamingStats.editsAppliedLastFrame > 0) {
            physicsDirtyCenterWorld = cameraPos;
            physicsDirtyFramesRemaining = std::max<uint32_t>(physicsDirtyFramesRemaining, 45u);
            ++physicsDirtyEvents;
        } else if (physicsDirtyFramesRemaining > 0) {
            --physicsDirtyFramesRemaining;
        }
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 1);
        }

        // Transition read buffer to pixel shader resource for rendering
        voxelWorld->TransitionReadBufferTo(commandList.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Build camera params for shader (camera in WORLD coordinates)
        // The shader uses regionOrigin to convert world->buffer coordinates internally
        glm::vec3 renderCameraPos = cameraPos;
        if (thirdPersonMode) {
            renderCameraPos = cameraPos - cameraForward * thirdPersonDistance + glm::vec3(0.0f, thirdPersonHeight, 0.0f);
        }
        glm::vec3 playerFeetWorld = cameraPos - glm::vec3(0.0f, playerHeight, 0.0f);

        Graphics::Renderer::CameraParams cameraParams;
        cameraParams.posX = renderCameraPos.x;
        cameraParams.posY = renderCameraPos.y;
        cameraParams.posZ = renderCameraPos.z;
        cameraParams.forwardX = cameraForward.x;
        cameraParams.forwardY = cameraForward.y;
        cameraParams.forwardZ = cameraForward.z;
        cameraParams.rightX = cameraRight.x;
        cameraParams.rightY = cameraRight.y;
        cameraParams.rightZ = cameraRight.z;
        cameraParams.upX = cameraUp.x;
        cameraParams.upY = cameraUp.y;
        cameraParams.upZ = cameraUp.z;
        cameraParams.fov = fov;
        cameraParams.aspectRatio = aspectRatio;
        cameraParams.raymarchMaxDistance = currentRaymarchMaxDistance;
        cameraParams.raymarchMaxSteps = currentRaymarchMaxSteps;
        cameraParams.farFieldQuality = currentFarFieldQuality;
        cameraParams.renderQuality = currentRenderQuality;
        cameraParams.midFieldStartDistance = sparseClipmapPolicy.Config().startDistance;
        cameraParams.midFieldEndDistance = sparseClipmapPolicy.Config().endDistance;
        cameraParams.midFieldCellSize = sparseClipmapPolicy.Config().minCellSize;
        cameraParams.debugMode = sparseRaymarchDebugMode;

        // Build brush preview params from GPU raycast result (NEW!)
        Graphics::Renderer::BrushPreview brushPreview = {};
        if (brushPlacementPreviewValid || brushHitValid) {
            const glm::vec3 previewWorld = brushPlacementPreviewValid
                ? (buildStrokeState.hasPreviewWorldPosition ? buildStrokeState.previewWorldPosition : brushPlacementWorld)
                : brushHitWorld;
            brushPreview.posX = previewWorld.x;
            brushPreview.posY = previewWorld.y;
            brushPreview.posZ = previewWorld.z;
            brushPreview.radius = brushController.GetRadius();
            brushPreview.material = brushController.GetMaterial();
            brushPreview.shape = static_cast<uint32_t>(brushController.GetShape());
            brushPreview.hasValidPosition = true;
        } else {
            brushPreview.hasValidPosition = false;
        }

        Graphics::Renderer::CharacterPreview characterPreview = {};
        characterPreview.feetX = playerFeetWorld.x;
        characterPreview.feetY = playerFeetWorld.y;
        characterPreview.feetZ = playerFeetWorld.z;
        characterPreview.visible = thirdPersonMode;

        // Render voxels with raymarch shader (using persistent shader-visible descriptors)
        // Brush preview now uses GPU raycasting (2,000,000x less bandwidth!)
        glm::vec3 regionOrigin = sparseRuntimeTestMode ? regionOriginWorld : voxelWorld->GetRegionOriginWorld();
        if (enableDiagnostics && frameCount % 60 == 0) {
            spdlog::debug(
                "[FRAME_DIAG] frame={} camWorld=({:.2f},{:.2f},{:.2f}) camLocal=({:.2f},{:.2f},{:.2f}) forward=({:.3f},{:.3f},{:.3f}) regionOrigin=({:.0f},{:.0f},{:.0f}) groundValid={} groundLocal=({:.2f},{:.2f},{:.2f}) brushValid={} brushLocal=({:.2f},{:.2f},{:.2f})",
                frameCount,
                cameraPos.x, cameraPos.y, cameraPos.z,
                cameraPosLocal.x, cameraPosLocal.y, cameraPosLocal.z,
                cameraForward.x, cameraForward.y, cameraForward.z,
                regionOrigin.x, regionOrigin.y, regionOrigin.z,
                groundRaycastResult.hasValidPosition ? 1 : 0,
                groundRaycastResult.posX, groundRaycastResult.posY, groundRaycastResult.posZ,
                brushHitValid ? 1 : 0,
                gpuRaycastResult.posX, gpuRaycastResult.posY, gpuRaycastResult.posZ);
        }
        perfPhaseStart = SDL_GetPerformanceCounter();
        Graphics::Renderer::SparseNearField sparseNearField = {};
        if (sparseBackendRequested && sparseGpuResources.IsInitialized()) {
            const auto& sparseStats = sparseGpuResources.GetStats();
            sparseNearField.brickPoolSRV = sparseGpuResources.BrickPoolSRV();
            sparseNearField.pageTableSRV = sparseGpuResources.PageTableSRV();
            sparseNearField.occupancySRV = sparseGpuResources.OccupancySRV();
            sparseNearField.pageGenerationSRV = sparseGpuResources.PageGenerationSRV();
            sparseNearField.midClipmapMetadataSRV = sparseGpuResources.MidClipmapMetadataSRV();
            sparseNearField.midClipmapLookupSRV = sparseGpuResources.MidClipmapLookupSRV();
            sparseNearField.midClipmapSamplesSRV = sparseGpuResources.MidClipmapSamplesSRV();
            sparseNearField.midVoxelClipmapMetadataSRV = sparseGpuResources.MidVoxelClipmapMetadataSRV();
            sparseNearField.midVoxelClipmapLookupSRV = sparseGpuResources.MidVoxelClipmapLookupSRV();
            sparseNearField.midVoxelClipmapSamplesSRV = sparseGpuResources.MidVoxelClipmapSamplesSRV();
            if (sparseSurfaceGpuResources.IsInitialized()) {
                const auto& sparseSurfaceStats = sparseSurfaceGpuResources.GetStats();
                sparseNearField.surfaceFacesSRV = sparseSurfaceGpuResources.FaceBufferSRV();
                sparseNearField.surfaceRangesSRV = sparseSurfaceGpuResources.RangeBufferSRV();
                sparseNearField.surfaceFaceCount = sparseSurfaceStats.uploadedFaces;
                sparseNearField.surfaceRangeCount = sparseSurfaceStats.uploadedRanges;
                sparseNearField.surfaceRangeTableCapacity = sparseSurfaceStats.uploadedRangeTableCapacity;
                sparseNearField.surfaceSerial = sparseSurfaceStats.uploadedSerial;
                sparseNearField.surfaceEnabled = sparseSurfaceStats.uploadedSerial != 0;
            }
            sparseNearField.maxBrickPages = sparseStats.maxBrickPages;
            sparseNearField.pageTableCapacity = sparseStats.pageTableCapacity;
            sparseNearField.midClipmapTileCount = sparseClipmapTileCache.GetStats().snapshotTiles;
            sparseNearField.midClipmapTileSampleSide = sparseClipmapPolicy.Config().tileSampleSide;
            sparseNearField.midVoxelClipmapBrickCount = sparseClipmapTileCache.GetStats().residentVoxelBricks;
            sparseNearField.bindingMask = sparseNearBindingMask;
            sparseNearField.enabled = enableSparseRaymarch && enableSparseNearBinding;
            sparseNearField.sparseOnly = enableSparseOnlyRaymarch;
            sparseNearField.midClipmapEnabled =
                sparseClipmapPolicy.IsEnabled() &&
                sparseClipmapTileCacheReady &&
                (sparseMidClipmapUploadedHeightSerial != 0 ||
                 sparseMidClipmapUploadedVoxelSerial != 0);
        }
        const bool dispatchSparseMissFeedbackThisFrame =
            enableSparseMissFeedback &&
            (sparseMissFeedbackInterval <= 1u ||
             (frameCount % sparseMissFeedbackInterval) == 0u);
        if (dispatchSparseMissFeedbackThisFrame &&
            enableSparseRaymarch &&
            sparseGpuResources.IsInitialized() &&
            sparseNearField.pageTableSRV.IsValid() &&
            sparseGpuResources.MissFeedbackUAV().IsValid()) {
            sparseGpuResources.PrepareMissFeedbackWrite(commandList.Get());
            physicsDispatcher->DispatchSparseMissFeedback(
                commandList.Get(),
                sparseNearField.pageTableSRV,
                sparseGpuResources.MissFeedbackUAV(),
                sparseNearField.maxBrickPages,
                sparseNearField.pageTableCapacity,
                renderCameraPos,
                cameraForward,
                cameraRight,
                cameraUp,
                fov,
                aspectRatio,
                static_cast<float>(sparseMissFeedbackDistance),
                static_cast<float>(sparseMissFeedbackStride),
                sparseMissFeedbackRayGrid,
                sparseMissFeedbackMaxRecords,
                static_cast<uint32_t>(frameCount));
            sparseGpuResources.QueueMissFeedbackReadback(commandList.Get(), frameIndex);
        }
        uint32_t renderGridSizeX = voxelWorld->GetGridSizeX();
        uint32_t renderGridSizeY = voxelWorld->GetGridSizeY();
        uint32_t renderGridSizeZ = voxelWorld->GetGridSizeZ();
        glm::vec3 renderRegionOrigin = regionOrigin;
        if (sparseBackendRequested && enableSparseOnlyRaymarch) {
            const auto alignDown16 = [](float value) {
                return std::floor(value / 16.0f) * 16.0f;
            };
            renderGridSizeX = sparseRaymarchWindowVoxels;
            renderGridSizeY = sparseRaymarchWindowVoxels;
            renderGridSizeZ = sparseRaymarchWindowVoxels;
            const float halfWindow = static_cast<float>(sparseRaymarchWindowVoxels) * 0.5f;
            renderRegionOrigin.x = alignDown16(renderCameraPos.x - halfWindow);
            renderRegionOrigin.y = alignDown16(renderCameraPos.y - halfWindow);
            renderRegionOrigin.z = alignDown16(renderCameraPos.z - halfWindow);
        }

        renderer->RenderVoxels(
            commandList.Get(),
            voxelWorld->GetReadBufferSRV(),
            voxelWorld->GetReadChunkValidMaskSRV(),
            voxelWorld->GetPaletteSRV(),
            renderGridSizeX,
            renderGridSizeY,
            renderGridSizeZ,
            cameraParams,
            renderRegionOrigin.x,
            renderRegionOrigin.y,
            renderRegionOrigin.z,
            &brushPreview,
            &characterPreview,
            &sparseFarField,
            &sparseNearField
        );
        if (enableSparseSurfaceRaster &&
            sparseNearField.surfaceEnabled &&
            sparseNearField.surfaceFaceCount > 0) {
            renderer->RenderSparseSurfaceFaces(
                commandList.Get(),
                sparseNearField.surfaceFacesSRV,
                voxelWorld->GetPaletteSRV(),
                sparseNearField.surfaceFaceCount,
                cameraParams);
            sparseSurfaceRasterFacesLastFrame = sparseNearField.surfaceFaceCount;
        }
        perfRenderSubmitMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 2);
        }

        // Always-on performance counters for the public tech demo. These are
        // intentionally cheap CPU-side counters: frame time, screen pixels,
        // represented voxel capacity, streaming progress, and simulation cadence.
        const auto& streamingStats = voxelWorld->GetStreamingStats();
        const uint64_t pixelCount =
            static_cast<uint64_t>(window->GetWidth()) *
            static_cast<uint64_t>(window->GetHeight());
        const float instantFps = lastRawFrameMs > 0.001f ? (1000.0f / lastRawFrameMs) : 0.0f;
        const float smoothedFps = smoothedFrameMs > 0.001f ? (1000.0f / smoothedFrameMs) : 0.0f;
        const auto playerChunk = Simulation::ChunkCoord::FromWorldPosition(
            static_cast<int32_t>(std::floor(cameraPos.x)),
            static_cast<int32_t>(std::floor(cameraPos.y)),
            static_cast<int32_t>(std::floor(cameraPos.z)),
            Simulation::CHUNK_SIZE_VOXELS);
        const auto playerFeetChunk = Simulation::ChunkCoord::FromWorldPosition(
            static_cast<int32_t>(std::floor(playerFeetWorld.x)),
            static_cast<int32_t>(std::floor(playerFeetWorld.y)),
            static_cast<int32_t>(std::floor(playerFeetWorld.z)),
            Simulation::CHUNK_SIZE_VOXELS);
        const uint32_t playerLocalVoxelX = Simulation::ChunkCoord::LocalCoord(
            static_cast<int32_t>(std::floor(playerFeetWorld.x)),
            Simulation::CHUNK_SIZE_VOXELS);
        const uint32_t playerLocalVoxelY = Simulation::ChunkCoord::LocalCoord(
            static_cast<int32_t>(std::floor(playerFeetWorld.y)),
            Simulation::CHUNK_SIZE_VOXELS);
        const uint32_t playerLocalVoxelZ = Simulation::ChunkCoord::LocalCoord(
            static_cast<int32_t>(std::floor(playerFeetWorld.z)),
            Simulation::CHUNK_SIZE_VOXELS);
        const glm::vec3 playerFeetRenderLocal =
            useStaticChunkLayout ? playerFeetWorld : voxelWorld->WorldToRenderLocal(playerFeetWorld);
        const glm::vec3 characterPreviewWorld(
            characterPreview.feetX,
            characterPreview.feetY,
            characterPreview.feetZ);
        const float terrainHeightAtPlayer =
            (hasCompletedGroundQuery && groundRaycastResult.hasValidPosition)
                ? groundRaycastResult.posY
                : -9999.0f;
        const auto& physicsStats = physicsDispatcher->GetStats();

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGuiWindowFlags metricsFlags =
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings;
        if (!pauseMenu.IsVisible()) {
            metricsFlags |=
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav;
        }
        if (ImGui::Begin("VENPOD Metrics", nullptr, metricsFlags)) {
            ImGui::Text("Backend %s%s",
                ToString(activeRenderBackend),
                sparseBackendRequested ? " | sparse requested, dense fallback active" : "");
            if (sparseBackendRequested) {
                const auto& sparseStats = sparseGpuResources.GetStats();
                ImGui::Text("Sparse raymarch visual %s | debug %u | only %u",
                    enableSparseRaymarch ? "on" : "off",
                    sparseRaymarchDebugMode,
                    enableSparseOnlyRaymarch ? 1u : 0u);
                ImGui::Text("Sparse mid clipmap %s | %.0f..%.0f | cell %.0f",
                    sparseClipmapPolicy.IsEnabled() ? "on" : "off",
                    sparseClipmapPolicy.Config().startDistance,
                    sparseClipmapPolicy.Config().endDistance,
                    sparseClipmapPolicy.Config().minCellSize);
                const auto& midStats = sparseClipmapTileCache.GetStats();
                ImGui::Text("Sparse mid cache tiles %u resident / %u queued | gen %u evict %u | upload %u retry %u",
                    midStats.residentTiles,
                    midStats.queuedTiles,
                    midStats.generatedTilesLastFrame,
                    midStats.evictedTilesLastFrame,
                    sparseStats.stagedMidClipmapTilesLastFrame,
                    sparseMidClipmapUploadRetriesLastFrame);
                ImGui::Text("Sparse mid voxel bricks %u resident / %u queued | gen %u evict %u | upload %u",
                    midStats.residentVoxelBricks,
                    midStats.queuedVoxelBricks,
                    midStats.generatedVoxelBricksLastFrame,
                    midStats.evictedVoxelBricksLastFrame,
                    sparseStats.stagedMidVoxelClipmapBricksLastFrame);
                ImGui::Text("Sparse GPU %s | pages %u | table %u | pool %.1f MB",
                    sparseStats.initialized ? "ready" : "unavailable",
                    sparseStats.maxBrickPages,
                    sparseStats.pageTableCapacity,
                    static_cast<double>(sparseStats.brickPoolBytes) / (1024.0 * 1024.0));
                ImGui::Text("Sparse mid GPU height %.2f MB | voxel %.2f MB | staged %.2f MB",
                    static_cast<double>(
                        sparseStats.midClipmapMetadataBytes +
                        sparseStats.midClipmapLookupBytes +
                        sparseStats.midClipmapSampleBytes) / (1024.0 * 1024.0),
                    static_cast<double>(
                        sparseStats.midVoxelClipmapMetadataBytes +
                        sparseStats.midVoxelClipmapLookupBytes +
                        sparseStats.midVoxelClipmapSampleBytes) / (1024.0 * 1024.0),
                    static_cast<double>(sparseStats.stagedMidClipmapBytesLastFrame) / (1024.0 * 1024.0));
                ImGui::Text("Sparse upload staged %u bricks + %u page entries / %.2f MB | overflow %u",
                    sparseStats.stagedBricksLastFrame,
                    sparseStats.stagedPageEntriesLastFrame,
                    static_cast<double>(sparseStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                    sparseStats.uploadRingOverflowLastFrame ? 1u : 0u);
                ImGui::Text("Sparse miss feedback %u retired | %zu pending | %u consumed",
                    sparseStats.missFeedbackRecordsLastRetire,
                    sparseMissFeedbackPending.size(),
                    sparseMissFeedbackConsumedLastFrame);
                ImGui::Text("Sparse requests spec/vis/coll %u/%u/%u | total budget %u",
                    sparseSpeculativeRequestsLastFrame,
                    sparseVisibleRequestsLastFrame,
                    sparseCollisionRequestsLastFrame,
                    sparseTotalRequestBudget);
                ImGui::Text("Sparse runtime scale %.2f | gen/upload/mid budgets %u/%u/%u",
                    sparseRuntimeBudgetScale,
                    sparseGenerationBudgetLastFrame,
                    sparseUploadBudgetLastFrame,
                    sparseMidClipmapBudgetLastFrame);
                ImGui::Text("Sparse ray budget scale %.2f | %.0f voxels / %u steps",
                    sparseRaymarchBudgetScale,
                    currentRaymarchMaxDistance,
                    currentRaymarchMaxSteps);
                ImGui::Text("Sparse speculative backpressure skips %u | genQ>%u miss>%u",
                    sparseSpeculativeBackpressureSkipsLastFrame,
                    sparseSpeculativeBackpressureGenQueue,
                    sparseSpeculativeBackpressureMissPending);
                ImGui::Text("Sparse pressure trim %u | budget %u",
                    sparsePressureTrimLastFrame,
                    sparsePressureTrimBudget);
                ImGui::Text("Sparse distance trim %s | start %u | keep %u/%u",
                    sparseDistanceTrimSkippedLastFrame ? "deferred" : "eligible",
                    sparseTrimStartResident,
                    sparseTrimRadiusXz,
                    sparseTrimRadiusY);
                ImGui::Text("Sparse replacement evict %u | budget %u",
                    sparseReplacementEvictionsLastFrame,
                    sparseReplacementBudget);
                ImGui::Text("Sparse retry upload %u | invalidation %u",
                    sparseUploadRequeuesLastFrame,
                    sparseInvalidationRequeuesLastFrame);
                ImGui::Text("Sparse page-table publish pending %zu | retries %u",
                    sparsePendingPageTablePublishes.size(),
                    sparsePageTablePublishRetriesLastFrame);
                const auto& sparseWorldStats = sparseVoxelWorld.GetStats();
                ImGui::Text("Sparse world %s | resident %u | tracked %u | queued gen %u | staged %u | queued upload %u | free %u",
                    sparseVoxelWorldReady ? "ready" : "off",
                    sparseWorldStats.residentBricks,
                    sparseWorldStats.requestedBricks,
                    sparseWorldStats.generationQueuedBricks,
                    sparseWorldStats.generatedBricks,
                    sparseWorldStats.uploadQueuedBricks,
                    sparseWorldStats.freePages);
                ImGui::Text("Sparse resident classes spec/vis/coll/edit %u/%u/%u/%u",
                    sparseWorldStats.residentSpeculativeBricks,
                    sparseWorldStats.residentVisibleBricks,
                    sparseWorldStats.residentCollisionBricks,
                    sparseWorldStats.residentEditedBricks);
                ImGui::Text("Sparse eviction last %u | queued invalid %u | trim %u/%u",
                    sparseWorldStats.evictedBricksLastFrame,
                    sparseWorldStats.evictionQueuedBricks,
                    sparseTrimRadiusXz,
                    sparseTrimRadiusY);
                ImGui::Text("Sparse brush eval %u | edited %u | bricks %u | uploads %u",
                    sparseWorldStats.brushVoxelsEvaluatedLastStroke,
                    sparseWorldStats.brushVoxelsEditedLastStroke,
                    sparseWorldStats.brushBricksTouchedLastStroke,
                    sparseWorldStats.brushBricksQueuedLastStroke);
            }
            ImGui::Text("FPS %.1f avg / %.1f now", smoothedFps, instantFps);
            ImGui::Text("Frame %.2f ms avg / %.2f ms now", smoothedFrameMs, lastRawFrameMs);
            ImGui::Separator();
            ImGui::Text("Pixels %ux%u = %llu",
                window->GetWidth(),
                window->GetHeight(),
                static_cast<unsigned long long>(pixelCount));
            ImGui::Text("Render voxels %ux%ux%u = %llu",
                voxelWorld->GetGridSizeX(),
                voxelWorld->GetGridSizeY(),
                voxelWorld->GetGridSizeZ(),
                static_cast<unsigned long long>(streamingStats.visibleVoxelCapacity));
            ImGui::Text("Generated chunks %u / records %u / queued %u",
                streamingStats.generatedChunks,
                streamingStats.loadedChunkRecords,
                streamingStats.queuedChunks);
            ImGui::Text("Generated voxels %llu",
                static_cast<unsigned long long>(streamingStats.loadedVoxelCapacity));
            ImGui::Text("Far SVO %s | pages %u | nodes %u | index %u | page %.0f | coverage %.0f",
                sparseFarField.enabled ? "on" : (farVoxelOctree.IsAsyncPending() ? "loading" : "off"),
                sparseFarField.pageCount,
                sparseFarField.nodeCount,
                sparseFarField.pageIndexCount,
                sparseFarField.pageSize,
                farVoxelOctree.GetStats().coveredWorldSize);
            ImGui::Text("World pos %.1f %.1f %.1f | chunk %d %d %d",
                cameraPos.x, cameraPos.y, cameraPos.z,
                playerChunk.x, playerChunk.y, playerChunk.z);
            ImGui::Text("Camera world %.1f %.1f %.1f | render camera %.1f %.1f %.1f",
                cameraPos.x, cameraPos.y, cameraPos.z,
                renderCameraPos.x, renderCameraPos.y, renderCameraPos.z);
            ImGui::Text("Player feet world %.1f %.1f %.1f | local %.1f %.1f %.1f",
                playerFeetWorld.x, playerFeetWorld.y, playerFeetWorld.z,
                playerFeetRenderLocal.x, playerFeetRenderLocal.y, playerFeetRenderLocal.z);
            ImGui::Text("Character world %.1f %.1f %.1f | visible %u",
                characterPreviewWorld.x,
                characterPreviewWorld.y,
                characterPreviewWorld.z,
                characterPreview.visible ? 1u : 0u);
            ImGui::Text("Feet chunk %d %d %d | local voxel %u %u %u",
                playerFeetChunk.x,
                playerFeetChunk.y,
                playerFeetChunk.z,
                playerLocalVoxelX,
                playerLocalVoxelY,
                playerLocalVoxelZ);
            ImGui::Text("Render origin %.0f %.0f %.0f | eye local %.1f %.1f %.1f",
                regionOrigin.x,
                regionOrigin.y,
                regionOrigin.z,
                cameraPosLocal.x,
                cameraPosLocal.y,
                cameraPosLocal.z);
            ImGui::Text("Last recenter d %d %d %d | old %d %d %d -> new %d %d %d",
                streamingStats.lastRecenterDeltaX,
                streamingStats.lastRecenterDeltaY,
                streamingStats.lastRecenterDeltaZ,
                streamingStats.lastRecenterOldOriginX,
                streamingStats.lastRecenterOldOriginY,
                streamingStats.lastRecenterOldOriginZ,
                streamingStats.lastRecenterNewOriginX,
                streamingStats.lastRecenterNewOriginY,
                streamingStats.lastRecenterNewOriginZ);
            ImGui::Text("Recenter reason %s | player changed %u | count %u",
                streamingStats.lastRecenterReason,
                streamingStats.lastRecenterPlayerChanged,
                streamingStats.lastRecenterFrame);
            if (terrainHeightAtPlayer > -9000.0f) {
                ImGui::Text("Terrain Y %.1f | render Y %d..%d | active center %d",
                    terrainHeightAtPlayer,
                    streamingStats.renderMinY,
                    streamingStats.renderMaxY,
                    streamingStats.activeChunkY);
            } else {
                ImGui::Text("Terrain Y pending | render Y %d..%d | active center %d",
                    streamingStats.renderMinY,
                    streamingStats.renderMaxY,
                    streamingStats.activeChunkY);
            }
            ImGui::Text("Visible chunks READ %u WRITE %u / %u",
                streamingStats.cachedReadChunks,
                streamingStats.cachedWriteChunks,
                streamingStats.expectedVisibleChunks);
            ImGui::Text("Toroidal page slots %u | misses R %u W %u",
                streamingStats.toroidalSlotCount,
                streamingStats.readSlotMismatches,
                streamingStats.writeSlotMismatches);
            ImGui::Text("Chunk copies %u / budget %u",
                streamingStats.chunksCopiedLastFrame,
                streamingStats.copyBudget);
            ImGui::Text("Missing gen %u load %u urgent %u checked %u",
                streamingStats.chunksNotGeneratedLastFrame,
                streamingStats.chunksNotLoadedLastFrame,
                streamingStats.urgentVisibleChunksQueuedLastFrame,
                streamingStats.chunksCheckedLastFrame);
            ImGui::Text("Edits chunks %u voxels %u | applied %u voxels / %u chunks",
                streamingStats.editedChunks,
                streamingStats.editedVoxels,
                streamingStats.editsAppliedLastFrame,
                streamingStats.chunksWithEditsAppliedLastFrame);
            ImGui::Text("Target voxel %d %d %d | chunk %d %d %d | local %u %u %u | %s",
                streamingStats.targetWorldX,
                streamingStats.targetWorldY,
                streamingStats.targetWorldZ,
                streamingStats.targetChunkX,
                streamingStats.targetChunkY,
                streamingStats.targetChunkZ,
                streamingStats.targetLocalX,
                streamingStats.targetLocalY,
                streamingStats.targetLocalZ,
                streamingStats.targetHasPersistentEdit ? "edited" : "generated");
            ImGui::Text("Hit %s | normal %d %d %d | brush r %.1f mode %u mat %u",
                brushHitValid ? "valid" : "invalid",
                streamingStats.targetNormalX,
                streamingStats.targetNormalY,
                streamingStats.targetNormalZ,
                brushController.GetRadius(),
                static_cast<uint32_t>(brushController.IsErasing() ? Input::BrushMode::Erase : brushController.GetMode()),
                brushController.IsErasing() ? 0u : brushController.GetMaterial());
            ImGui::Text("Brush eval %u reject %u recorded %u",
                streamingStats.brushVoxelsEvaluatedLastStroke,
                streamingStats.brushVoxelsRejectedLastStroke,
                streamingStats.persistentEditsRecordedLastStroke);
            ImGui::Text("Brush placement %s | close ramp %s | world %.1f %.1f %.1f",
                brushPlacementPreviewValid ? "active" : "preview",
                brushPlacementCloseRamp ? "on" : "off",
                brushPlacementPreviewValid ? brushPlacementWorld.x : brushHitWorld.x,
                brushPlacementPreviewValid ? brushPlacementWorld.y : brushHitWorld.y,
                brushPlacementPreviewValid ? brushPlacementWorld.z : brushHitWorld.z);
            ImGui::Text("Brush sweep stamps %u | stroke cache %s",
                buildStrokeState.sweepStampsLastFrame,
                buildStrokeState.hasLastBrushWorldPosition ? "active" : "idle");
            ImGui::Text("GPU edit feedback queued %u pending %u applied %u drop %u overflow %u",
                streamingStats.gpuBrushFeedbackQueued,
                streamingStats.gpuBrushFeedbackPending,
                streamingStats.gpuBrushEventsAppliedLastFrame,
                streamingStats.gpuBrushEventsDroppedLastFrame,
                streamingStats.gpuBrushEventsOverflowLastFrame);
            ImGui::Text("Physics %s interval %u dispatches %llu skips %llu dirty %u events %llu",
                physicsRanThisFrame ? "ran" : (physicsSkippedForBudget ? "budget skip" : "idle"),
                physicsInterval,
                static_cast<unsigned long long>(physicsDispatchCount),
                static_cast<unsigned long long>(physicsBudgetSkipCount),
                physicsDirtyFramesRemaining,
                static_cast<unsigned long long>(physicsDirtyEvents));
            ImGui::Text("Physics scan %u/%u chunks skip %u universe %u region %ux%ux%u @ %d %d %d",
                physicsStats.scannedChunksLastFrame,
                physicsStats.scanBudgetChunks,
                physicsStats.skippedScanChunksLastFrame,
                physicsStats.theoreticalChunkUniverse,
                physicsStats.dispatchX,
                physicsStats.dispatchY,
                physicsStats.dispatchZ,
                physicsStats.offsetX,
                physicsStats.offsetY,
                physicsStats.offsetZ);
            ImGui::Text("Copy budget %u | generation budget %u | copy-fence skips %u",
                currentCopyBudget,
                currentGenerationBudget,
                streamingStats.swapSkippedForCopyFence);
            ImGui::Text("Raymarch budget dense %.0f voxels / %u steps | far %.2f | quality %.2f",
                currentRaymarchMaxDistance,
                currentRaymarchMaxSteps,
                currentFarFieldQuality,
                currentRenderQuality);
            ImGui::Text("CPU phases wait %.2f chunk %.2f phys %.2f brush %.2f render %.2f present %.2f ms",
                perfFenceWaitMs,
                perfChunkUpdateMs,
                perfPhysicsSubmitMs,
                perfBrushSubmitMs,
                perfRenderSubmitMs,
                perfPresentMs);
            ImGui::Text("GPU frame %.2f ms | pre-render %.2f | raymarch %.2f | ui/readback %.2f",
                gpuTiming.valid ? gpuTiming.frameMs : 0.0,
                gpuTiming.valid ? gpuTiming.preRenderMs : 0.0,
                gpuTiming.valid ? gpuTiming.raymarchMs : 0.0,
                gpuTiming.valid ? gpuTiming.uiAndReadbackMs : 0.0);
            ImGui::Text("Scheduler predicted %.2f ms | pressure %.2f ms",
                schedulerPredictedFrameMs,
                schedulerPressureMs);
        }
        ImGui::End();

        if (enableRuntimeLog && (frameCount % 120 == 0)) {
            spdlog::info(
                "PERF frame={} fps={:.1f}/{:.1f} ms={:.2f}/{:.2f} wait={:.2f} chunk={:.2f} phys={:.2f} brush={:.2f} render={:.2f} present={:.2f} gpu={:.2f}/{:.2f}/{:.2f}/{:.2f} sched={:.2f} copy={}/{} genBudget={} generated={} records={} queue={} cached={}/{}/{} pageMiss={}/{} missingGen={} missingLoad={} urgent={} skipped={} checked={} physicsScan={}/{} skip={} universe={} dirty={} farQ={:.2f}",
                frameCount,
                smoothedFps,
                instantFps,
                smoothedFrameMs,
                lastRawFrameMs,
                perfFenceWaitMs,
                perfChunkUpdateMs,
                perfPhysicsSubmitMs,
                perfBrushSubmitMs,
                perfRenderSubmitMs,
                perfPresentMs,
                gpuTiming.valid ? gpuTiming.frameMs : 0.0,
                gpuTiming.valid ? gpuTiming.preRenderMs : 0.0,
                gpuTiming.valid ? gpuTiming.raymarchMs : 0.0,
                gpuTiming.valid ? gpuTiming.uiAndReadbackMs : 0.0,
                schedulerPredictedFrameMs,
                streamingStats.chunksCopiedLastFrame,
                streamingStats.copyBudget,
                currentGenerationBudget,
                streamingStats.generatedChunks,
                streamingStats.loadedChunkRecords,
                streamingStats.queuedChunks,
                streamingStats.cachedReadChunks,
                streamingStats.cachedWriteChunks,
                streamingStats.expectedVisibleChunks,
                streamingStats.readSlotMismatches,
                streamingStats.writeSlotMismatches,
                streamingStats.chunksNotGeneratedLastFrame,
                streamingStats.chunksNotLoadedLastFrame,
                streamingStats.urgentVisibleChunksQueuedLastFrame,
                streamingStats.chunksSkippedLastFrame,
                streamingStats.chunksCheckedLastFrame,
                physicsStats.scannedChunksLastFrame,
                physicsStats.scanBudgetChunks,
                physicsStats.skippedScanChunksLastFrame,
                physicsStats.theoreticalChunkUniverse,
                physicsDirtyFramesRemaining,
                currentFarFieldQuality);
            if (sparseBackendRequested && sparseVoxelWorldReady) {
                const auto& sparseWorldStats = sparseVoxelWorld.GetStats();
                const auto& sparseGpuStats = sparseGpuResources.GetStats();
                const auto& midStats = sparseClipmapTileCache.GetStats();
                spdlog::info(
                    "PERF_SPARSE frame={} runtimeTest={} resident={} tracked={} class={}/{}/{}/{} genQueued={} staged={} uploadQueued={} free={} evictLast={} invalidQueued={} edits={}/{} surface={}/{} surfUpd={} surfRm={} surfGenFaces={} surfSerial={} brushEval={} brushEdit={} brushBricks={} brushUploads={} gpuStaged={} pageEntries={} uploadMB={:.2f} overflow={} missRetired={} missPending={} missConsumed={} reqSpec={} reqVis={} reqColl={} scale={:.2f} rayScale={:.2f} rayBudget={:.0f}/{} budgetGen={} budgetUpload={} budgetMid={} specSkip={} pressureTrim={} distTrimSkip={} trimStart={} replaceEvict={} retryUpload={} retryInvalid={} publishPending={} publishRetry={} midClip={} midStart={} midEnd={} midTiles={}/{} midGen={} midUpload={} midRetry={} midVoxels={}/{} midVoxelGen={} midVoxelUpload={} midVoxelEvict={} midBytesMB={:.2f} midSerial={}",
                    frameCount,
                    sparseRuntimeTestMode ? 1 : 0,
                    sparseWorldStats.residentBricks,
                    sparseWorldStats.requestedBricks,
                    sparseWorldStats.residentSpeculativeBricks,
                    sparseWorldStats.residentVisibleBricks,
                    sparseWorldStats.residentCollisionBricks,
                    sparseWorldStats.residentEditedBricks,
                    sparseWorldStats.generationQueuedBricks,
                    sparseWorldStats.generatedBricks,
                    sparseWorldStats.uploadQueuedBricks,
                    sparseWorldStats.freePages,
                    sparseWorldStats.evictedBricksLastFrame,
                    sparseWorldStats.evictionQueuedBricks,
                    sparseWorldStats.editedBricks,
                    sparseWorldStats.editedVoxels,
                    sparseWorldStats.surfaceCachedBricks,
                    sparseWorldStats.surfaceFaces,
                    sparseWorldStats.surfaceBricksUpdatedLastFrame,
                    sparseWorldStats.surfaceBricksRemovedLastFrame,
                    sparseWorldStats.surfaceFacesGeneratedLastFrame,
                    sparseWorldStats.surfaceSerial,
                    sparseWorldStats.brushVoxelsEvaluatedLastStroke,
                    sparseWorldStats.brushVoxelsEditedLastStroke,
                    sparseWorldStats.brushBricksTouchedLastStroke,
                    sparseWorldStats.brushBricksQueuedLastStroke,
                    sparseGpuStats.stagedBricksLastFrame,
                    sparseGpuStats.stagedPageEntriesLastFrame,
                    static_cast<double>(sparseGpuStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                    sparseGpuStats.uploadRingOverflowLastFrame ? 1 : 0,
                    sparseGpuStats.missFeedbackRecordsLastRetire,
                    sparseMissFeedbackPending.size(),
                    sparseMissFeedbackConsumedLastFrame,
                    sparseSpeculativeRequestsLastFrame,
                    sparseVisibleRequestsLastFrame,
                    sparseCollisionRequestsLastFrame,
                    sparseRuntimeBudgetScale,
                    sparseRaymarchBudgetScale,
                    currentRaymarchMaxDistance,
                    currentRaymarchMaxSteps,
                    sparseGenerationBudgetLastFrame,
                    sparseUploadBudgetLastFrame,
                    sparseMidClipmapBudgetLastFrame,
                    sparseSpeculativeBackpressureSkipsLastFrame,
                    sparsePressureTrimLastFrame,
                    sparseDistanceTrimSkippedLastFrame,
                    sparseTrimStartResident,
                    sparseReplacementEvictionsLastFrame,
                    sparseUploadRequeuesLastFrame,
                    sparseInvalidationRequeuesLastFrame,
                    sparsePendingPageTablePublishes.size(),
                    sparsePageTablePublishRetriesLastFrame,
                    sparseClipmapPolicy.IsEnabled() ? 1 : 0,
                    static_cast<uint32_t>(sparseClipmapPolicy.Config().startDistance),
                    static_cast<uint32_t>(sparseClipmapPolicy.Config().endDistance),
                    midStats.residentTiles,
                    midStats.queuedTiles,
                    midStats.generatedTilesLastFrame,
                    sparseGpuStats.stagedMidClipmapTilesLastFrame,
                    sparseMidClipmapUploadRetriesLastFrame,
                    midStats.residentVoxelBricks,
                    midStats.queuedVoxelBricks,
                    midStats.generatedVoxelBricksLastFrame,
                    sparseGpuStats.stagedMidVoxelClipmapBricksLastFrame,
                    midStats.evictedVoxelBricksLastFrame,
                    static_cast<double>(sparseGpuStats.stagedMidClipmapBytesLastFrame) / (1024.0 * 1024.0),
                    std::max(sparseMidClipmapUploadedHeightSerial, sparseMidClipmapUploadedVoxelSerial));
                if (sparseSurfaceGpuResources.IsInitialized()) {
                    const auto& sparseSurfaceGpuStats = sparseSurfaceGpuResources.GetStats();
                    spdlog::info(
                        "PERF_SPARSE_SURFACE frame={} cpuBricks={} cpuFaces={} cpuSerial={} gpuFaces={} gpuRanges={} gpuRangeTable={} gpuSerial={} stagedFaces={} stagedRanges={} stagedRangeTable={} stagedMB={:.2f} rasterFaces={} retry={} overflow={}",
                        frameCount,
                        sparseWorldStats.surfaceCachedBricks,
                        sparseWorldStats.surfaceFaces,
                        sparseWorldStats.surfaceSerial,
                        sparseSurfaceGpuStats.uploadedFaces,
                        sparseSurfaceGpuStats.uploadedRanges,
                        sparseSurfaceGpuStats.uploadedRangeTableCapacity,
                        sparseSurfaceGpuStats.uploadedSerial,
                        sparseSurfaceGpuStats.stagedFacesLastFrame,
                        sparseSurfaceGpuStats.stagedRangesLastFrame,
                        sparseSurfaceGpuStats.stagedRangeTableCapacityLastFrame,
                        static_cast<double>(sparseSurfaceGpuStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                        sparseSurfaceRasterFacesLastFrame,
                        sparseSurfaceUploadRetriesLastFrame,
                        sparseSurfaceGpuStats.uploadOverflowLastFrame ? 1 : 0);
                }
            }
        }

        ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        const ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f);
        const ImU32 crosshairColor = IM_COL32(225, 240, 255, 210);
        foregroundDrawList->AddLine(
            ImVec2(center.x - 8.0f, center.y),
            ImVec2(center.x + 8.0f, center.y),
            crosshairColor,
            1.5f);
        foregroundDrawList->AddLine(
            ImVec2(center.x, center.y - 8.0f),
            ImVec2(center.x, center.y + 8.0f),
            crosshairColor,
            1.5f);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} imgui-built", frameCount);
        }

        // Render ImGui draw data to command list
        imguiBackend.Render(commandList.Get());
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} imgui-rendered", frameCount);
        }

        // Queue tiny GPU->CPU raycast copies into a per-frame readback slot.
        // The slot is only mapped after this same frame index's fence completes,
        // so brush targeting/collision never consumes an in-flight GPU write.
        voxelWorld->QueueBrushRaycastReadback(commandList.Get(), frameIndex);
        voxelWorld->QueueGroundRaycastReadback(commandList.Get(), frameIndex);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} readbacks-queued", frameCount);
        }
        brushQueryMetadata[frameIndex] = BrushQueryMetadata{
            true,
            nextBrushQueryRegionOriginWorld,
            nextBrushQueryOriginWorld,
            nextBrushQueryDirectionWorld
        };
        groundQueryMetadata[frameIndex] = GroundQueryMetadata{
            true,
            nextGroundQueryRegionOriginWorld,
            nextGroundQueryFeetWorld
        };

        // End frame - transitions back buffer to present state
        renderer->EndFrame(commandList.Get(), frameIndex);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} render-end", frameCount);
        }
        if (gpuTimestampHeap && gpuTimestampReadback) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 3);
            commandList->ResolveQueryData(
                gpuTimestampHeap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                gpuTimestampBase,
                kGpuTimestampCount,
                gpuTimestampReadback.Get(),
                static_cast<UINT64>(gpuTimestampBase) * sizeof(uint64_t));
        }
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} timestamp-resolved", frameCount);
        }

        // Close and execute command list
        commandList->Close();
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} commandlist-closed", frameCount);
        }
        commandQueue->ExecuteCommandList(commandList.Get());
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} commandlist-executed", frameCount);
        }

        // FIX #17: CRITICAL - Swap read/write buffers for next frame
        // The bug: Without this swap, chunks are copied to WRITE buffer every frame,
        // but READ buffer (used by renderer) stays empty forever -> no terrain visible!
        // UpdateChunks and physics wrote to WRITE buffer this frame.
        // Swap makes it the READ buffer so renderer can see new chunk data next frame.
        // Sequence:
        //   Frame N: chunks copied to WRITE (buffer 1), physics writes to buffer 1, render reads buffer 0
        //   Swap -> buffer 1 becomes READ, buffer 0 becomes WRITE
        //   Frame N+1: chunks copied to WRITE (now buffer 0), physics writes to buffer 0, render reads buffer 1
        voxelWorld->SwapBuffers();
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} buffers-swapped", frameCount);
        }

        // Present
        perfPhaseStart = SDL_GetPerformanceCounter();
        window->Present();
        perfPresentMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} presented {:.2f}ms", frameCount, perfPresentMs);
        }

        // Signal fence for this frame
        ctx.fenceValue = commandQueue->Signal();
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} signaled fence {}", frameCount, ctx.fenceValue);
        }
        voxelWorld->NotifyBrushEditFeedbackFence(ctx.fenceValue);

        // Submit background chunk generation after the frame has been queued.
        // This keeps generation from sitting in front of the render pass on the
        // direct queue while still letting it use trailing GPU time.
        voxelWorld->SetChunkGenerationBudget(trailingGenerationBudget);
        voxelWorld->PumpChunkGeneration(device->GetDevice(), commandQueue->GetCommandQueue());
        {
            const float gpuFrameMs = gpuTiming.valid ? static_cast<float>(gpuTiming.frameMs) : 0.0f;
            const float predictedWorkMs = std::max(
                lastRawFrameMs,
                gpuFrameMs + perfChunkUpdateMs + perfPhysicsSubmitMs + perfBrushSubmitMs + perfPresentMs);
            schedulerPredictedFrameMs =
                schedulerPredictedFrameMs * 0.82f + predictedWorkMs * 0.18f;
        }

        // End input frame
        inputManager.EndFrame();

        frameCount++;
        if (traceFrameStages && frameCount <= kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} complete", frameCount - 1);
        }
        if (exitAfterFrames > 0u && frameCount >= exitAfterFrames) {
            spdlog::info("VENPOD_EXIT_AFTER_FRAMES reached: {}", exitAfterFrames);
            running = false;
        }

        // Log FPS every 100 frames
        // if (frameCount % 100 == 0) {
        //     spdlog::debug("Frame {}", frameCount);
        // }
    }

    spdlog::info("Shutting down...");

    // CRITICAL: Wait for all GPU work to complete before cleanup
    // This prevents OBJECT_DELETED_WHILE_STILL_IN_USE errors
    commandQueue->Flush();

    // Release resources in reverse order
    commandList.Reset();
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        frameContexts[i].commandAllocator.Reset();
    }

    physicsDispatcher->Shutdown();
    chunkManager->Shutdown();
    voxelWorld->Shutdown();
    farVoxelOctree.Shutdown();
    sparseSurfaceGpuResources.Shutdown();
    sparseGpuResources.Shutdown();

    renderer->Shutdown();
    window->Shutdown();
    commandQueue->Shutdown();
    device->Shutdown();

    spdlog::info("VENPOD shut down cleanly. Total frames: {}", frameCount);
    return 0;
}
