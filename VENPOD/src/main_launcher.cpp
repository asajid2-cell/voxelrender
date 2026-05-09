// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Entry Point
// =============================================================================

#include "Core/Window.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/DX12CommandQueue.h"
#include "Graphics/BackbufferCapture.h"
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
#include "Simulation/SparseCharacterController.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparsePagePublishQueue.h"
#include "Simulation/SparseRuntimeBudget.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Input/InputManager.h"
#include "Input/BrushController.h"
#include "UI/ImGuiBackend.h"
#include "UI/MaterialPalette.h"
#include "UI/BrushPanel.h"
#include "UI/PauseMenu.h"
#include "Utils/BitPacking.h"
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
#include <limits>
#include <string>
#include <unordered_map>
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

static constexpr uint32_t kGpuTimestampCount = 7;

struct GpuTimingStats {
    bool valid = false;
    double frameMs = 0.0;
    double sparseUploadMs = 0.0;
    double preRenderMs = 0.0;
    double raymarchMs = 0.0;
    double overlayMs = 0.0;
    double sparseSurfaceMs = 0.0;
    double uiAndReadbackMs = 0.0;
};

static bool ReadGpuTiming(
    ID3D12Resource* readbackBuffer,
    uint64_t timestampFrequency,
    uint32_t frameIndex,
    GpuTimingStats& stats)
{
    stats = {};
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
    const uint64_t t4 = timestamps[4];
    const uint64_t t5 = timestamps[5];
    const uint64_t t6 = timestamps[6];

    const D3D12_RANGE writtenRange{0, 0};
    readbackBuffer->Unmap(0, &writtenRange);

    if (t0 == 0 || t1 < t0 || t2 < t1 || t3 < t2 || t4 < t3 || t5 < t4 || t6 < t5) {
        return false;
    }

    const double tickToMs = 1000.0 / static_cast<double>(timestampFrequency);
    stats.valid = true;
    stats.sparseUploadMs = static_cast<double>(t1 - t0) * tickToMs;
    stats.preRenderMs = static_cast<double>(t2 - t1) * tickToMs;
    stats.sparseSurfaceMs = static_cast<double>(t3 - t2) * tickToMs;
    stats.raymarchMs = static_cast<double>(t4 - t3) * tickToMs;
    stats.overlayMs = static_cast<double>(t5 - t4) * tickToMs;
    stats.uiAndReadbackMs = static_cast<double>(t6 - t5) * tickToMs;
    stats.frameMs = static_cast<double>(t6 - t0) * tickToMs;
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

static std::filesystem::path ResolveSandboxUserPath(const char* pathValue) {
    if (!pathValue || pathValue[0] == '\0') {
        return {};
    }

    std::filesystem::path path(pathValue);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    std::error_code ec;
    std::filesystem::path base = std::filesystem::current_path(ec);
    if (ec) {
        base = GetExecutableDirectorySandbox();
    }

    if (base.filename() == "bin" && base.parent_path().filename() == "build") {
        base = base.parent_path().parent_path();
    }

    return (base / path).lexically_normal();
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
    const bool hideUiForCapture =
        std::getenv("VENPOD_CAPTURE_HIDE_UI") != nullptr ||
        std::getenv("VENPOD_HIDE_UI") != nullptr;
    const bool enableRuntimeLog = enableDiagnostics || std::getenv("VENPOD_LOG_FILE") != nullptr;
    const bool enableD3DDebug = std::getenv("VENPOD_D3D_DEBUG") != nullptr;
    const bool allowInternalTestModes = std::getenv("VENPOD_ENABLE_TEST_MODES") != nullptr;
    const bool enableBoundaryTest =
        allowInternalTestModes && std::getenv("VENPOD_BOUNDARY_TEST") != nullptr;
    const bool enableFarSVO =
        ReadUIntEnv("VENPOD_ENABLE_FAR_SVO", 1u) != 0u &&
        std::getenv("VENPOD_DISABLE_FAR_SVO") == nullptr;
    const float farSvoFinalizeMaxFrameMs = static_cast<float>(
        ReadUIntEnv("VENPOD_FAR_SVO_FINALIZE_MAX_MS", 12u));
    const uint64_t farSvoFinalizeUploadBudgetBytes =
        static_cast<uint64_t>(std::max(1u, ReadUIntEnv("VENPOD_FAR_SVO_UPLOAD_BUDGET_MB", 2u))) *
        1024ull * 1024ull;
    const uint64_t farSvoTrickleUploadBudgetBytes =
        static_cast<uint64_t>(
            std::max(64u, ReadUIntEnv("VENPOD_FAR_SVO_TRICKLE_UPLOAD_KB", 512u))) *
        1024ull;
    const float farSvoUploadTargetMs = static_cast<float>(
        std::max(250u, ReadUIntEnv("VENPOD_FAR_SVO_UPLOAD_TARGET_US", 1250u))) /
        1000.0f;
    const bool highDensityDenseWindow = std::getenv("VENPOD_HIGH_DENSITY") != nullptr;
    const bool lowMemoryDenseWindow =
        std::getenv("VENPOD_LOW_MEMORY_DENSE") != nullptr && !highDensityDenseWindow;
    const bool allowExperimentalSparse = std::getenv("VENPOD_ENABLE_EXPERIMENTAL_SPARSE") != nullptr;
    const VoxelRenderBackend environmentRenderBackend = RequestedVoxelRenderBackendFromEnvironment();
    const VoxelRenderBackend requestedRenderBackend =
        (environmentRenderBackend == VoxelRenderBackend::SparseBrick && !allowExperimentalSparse)
            ? VoxelRenderBackend::DenseLegacy
            : environmentRenderBackend;
    const bool sparseBackendRequested = requestedRenderBackend == VoxelRenderBackend::SparseBrick;
    const bool requireSparsePipeReady =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_REQUIRE_PIPE_READY", 0u) != 0u;
    const uint32_t sparsePipeReadyFrame =
        std::max(1u, ReadUIntEnv("VENPOD_SPARSE_PIPE_READY_FRAME", 180u));
    const bool enableSparseRaymarch = sparseBackendRequested && std::getenv("VENPOD_SPARSE_RAYMARCH") != nullptr;
    const bool enableSparseOnlyRaymarch = enableSparseRaymarch && std::getenv("VENPOD_SPARSE_ONLY") != nullptr;
    const bool enableSparseSurfaceAuthoritative =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_SURFACE_AUTHORITATIVE", enableSparseOnlyRaymarch ? 1u : 0u) != 0u;
    const VoxelRenderBackend activeRenderBackend =
        enableSparseSurfaceAuthoritative ? VoxelRenderBackend::SparseBrick : VoxelRenderBackend::DenseLegacy;
    const bool enableSparseNearBinding = ReadUIntEnv("VENPOD_SPARSE_BIND_NEAR", 1u) != 0u;
    const uint32_t sparseNearBindingMask = ReadUIntEnv("VENPOD_SPARSE_BIND_MASK", 0xFFFu);
    const uint32_t sparseRaymarchWindowVoxels =
        std::max(64u, ReadUIntEnv("VENPOD_SPARSE_RAY_WINDOW", 64u));
    const bool enableUnsafeSparseFullRaymarch =
        ReadUIntEnv("VENPOD_SPARSE_FULL_RAYMARCH", 0u) != 0u;
    const bool sparseRuntimeTestMode =
        enableSparseOnlyRaymarch && std::getenv("VENPOD_SPARSE_LEGACY_RUNTIME") == nullptr;
    const bool enableSparsePoolValidation =
        sparseRuntimeTestMode &&
        ReadUIntEnv("VENPOD_SPARSE_VALIDATE_POOL", 0u) != 0u;
    const bool enableSparseGpuRaycast =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST", sparseRuntimeTestMode ? 0u : 1u) != 0u;
    const bool enableSparseLocalPhysics =
        sparseRuntimeTestMode &&
        !disablePhysics &&
        ReadUIntEnv("VENPOD_ENABLE_SPARSE_PHYSICS", 1u) != 0u;
    const bool requestSparsePhysicsGpu =
        enableSparseLocalPhysics &&
        ReadUIntEnv("VENPOD_SPARSE_PHYSICS_GPU", 0u) != 0u;
    const bool enableSparsePhysicsPacketUpload =
        enableSparseLocalPhysics &&
        ReadUIntEnv(
            "VENPOD_SPARSE_PHYSICS_PACKET_UPLOAD",
            requestSparsePhysicsGpu ? 1u : 0u) != 0u;
    const bool enableSparsePhysicsGpu =
        requestSparsePhysicsGpu &&
        enableSparsePhysicsPacketUpload;
    const bool enableSparsePhysicsGpuApply =
        enableSparsePhysicsGpu &&
        ReadUIntEnv("VENPOD_SPARSE_PHYSICS_GPU_APPLY", 0u) != 0u;
    const bool disableRuntimePhysics = disablePhysics || sparseRuntimeTestMode;
    const bool enableSparseBodyCollision =
        sparseRuntimeTestMode &&
        ReadUIntEnv("VENPOD_SPARSE_BODY_COLLISION", 1u) != 0u;
    const char* sparseEditFileEnv = std::getenv("VENPOD_SPARSE_EDIT_FILE");
    const std::filesystem::path sparseEditFilePath = ResolveSandboxUserPath(sparseEditFileEnv);
    const bool enableSparseEditFile = !sparseEditFilePath.empty();
    uint32_t sparseRaymarchDebugMode = enableSparseRaymarch
        ? ReadUIntEnv("VENPOD_SPARSE_DEBUG_MODE", 0u)
        : 0u;
    if (enableSparseOnlyRaymarch &&
        !enableSparseSurfaceAuthoritative &&
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
    spdlog::info("  Static chunks: {} | Legacy dense physics disabled: {} | Infinite physics: {} | Diagnostics: {} | Boundary test: {} | Far SVO: {}",
        useStaticChunkLayout ? "yes" : "no",
        disableRuntimePhysics ? "yes" : "no",
        enableInfinitePhysics ? "yes" : "no",
        enableDiagnostics ? "yes" : "no",
        enableBoundaryTest ? "yes" : "no",
        enableFarSVO ? "yes" : "no");
    spdlog::info("  Render backend requested: {} | active: {}{}",
        ToString(requestedRenderBackend),
        ToString(activeRenderBackend),
        sparseBackendRequested
            ? (enableSparseSurfaceAuthoritative
                ? " (sparse surface authoritative near-field enabled)"
                : " (sparse backend scaffold active; dense renderer still displays final frame)")
            : "");
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
        spdlog::info("  Sparse surface authoritative near field: {}",
            enableSparseSurfaceAuthoritative ? "enabled" : "disabled");
        spdlog::info("  Sparse runtime test mode: {}{}",
            sparseRuntimeTestMode ? "enabled" : "disabled",
            sparseRuntimeTestMode ? " (legacy dense streaming bypassed)" : "");
        spdlog::info("  Sparse raycast owner: {}",
            (sparseRuntimeTestMode && !enableSparseGpuRaycast) ? "CPU sparse world" : "GPU sparse/dense readback");
        spdlog::info("  Sparse local physics: {}",
            enableSparseLocalPhysics ? "enabled" : "disabled");
        spdlog::info("  Sparse physics packet upload scaffold: {}",
            enableSparsePhysicsPacketUpload ? "enabled" : "disabled");
        spdlog::info("  Sparse physics GPU packet pipeline: {}",
            enableSparsePhysicsGpu ? "enabled" : "disabled");
        spdlog::info("  Sparse physics GPU proposal apply: {}",
            enableSparsePhysicsGpuApply ? "enabled" : "disabled");
        spdlog::info("  Sparse body collision: {}",
            enableSparseBodyCollision ? "enabled" : "disabled");
        spdlog::info("  Sparse pool invariant validation: {}",
            enableSparsePoolValidation ? "enabled" : "disabled");
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
    const uint32_t sparseGenerationBudget = ReadUIntEnv("VENPOD_SPARSE_GENERATION_BUDGET", 6u);
    const uint32_t sparseUploadBudget = ReadUIntEnv("VENPOD_SPARSE_UPLOAD_BUDGET", 12u);
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
        ReadUIntEnv(
            "VENPOD_SPARSE_PAGE_TABLE_PUBLISH_BUDGET",
            std::max(256u, std::max(sparseInvalidationBudget, sparseUploadBudget * 8u)));
    const uint32_t sparseCollisionShellRadiusXz = ReadUIntEnv("VENPOD_SPARSE_COLLISION_SHELL_XZ", 1u);
    const uint32_t sparseCollisionShellRadiusY = ReadUIntEnv("VENPOD_SPARSE_COLLISION_SHELL_Y", 1u);
    const uint32_t sparseNewRequestBudget = ReadUIntEnv("VENPOD_SPARSE_NEW_REQUEST_BUDGET", 24u);
    const uint32_t sparseTotalRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_TOTAL_REQUEST_BUDGET", sparseNewRequestBudget * 2u + 16u);
    const uint32_t sparseSpeculativeRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_SPECULATIVE_REQUEST_BUDGET", std::max(1u, sparseNewRequestBudget / 2u));
    const uint32_t sparseFastRequestSpeed =
        ReadUIntEnv("VENPOD_SPARSE_FAST_REQUEST_SPEED", 96u);
    const uint32_t sparseFastRequestMaxScale =
        std::max(1u, std::min(6u, ReadUIntEnv("VENPOD_SPARSE_FAST_REQUEST_MAX_SCALE", 4u)));
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
    const uint32_t sparseRayPrefetchDistance = ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_DISTANCE", 384u);
    const uint32_t sparseRayPrefetchStride = std::max(4u, ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_STRIDE", 16u));
    const uint32_t sparseRayPrefetchMaxRequests = ReadUIntEnv("VENPOD_SPARSE_RAY_PREFETCH_MAX_REQUESTS", 32u);
    const uint32_t sparseViewPrefetchRayGrid = ReadUIntEnv("VENPOD_SPARSE_VIEW_PREFETCH_RAYS", 5u);
    const uint32_t sparsePredictivePrefetchMs = ReadUIntEnv("VENPOD_SPARSE_PREDICTIVE_PREFETCH_MS", 450u);
    const uint32_t sparseMotionVisibleMinSpeed =
        ReadUIntEnv("VENPOD_SPARSE_MOTION_VISIBLE_MIN_SPEED", 64u);
    const uint32_t sparseMotionVisibleMaxRequests =
        ReadUIntEnv("VENPOD_SPARSE_MOTION_VISIBLE_MAX_REQUESTS", std::max(24u, sparseVisibleRequestBudget));
    const bool enableSparseHierarchicalRequests =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_HIERARCHICAL_REQUESTS", 1u) != 0u;
    const bool enableSparseStressRequests =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_STRESS_REQUESTS", 0u) != 0u;
    const uint32_t sparseStressRadiusXz = ReadUIntEnv("VENPOD_SPARSE_STRESS_RADIUS_XZ", 5u);
    const uint32_t sparseStressRadiusY = ReadUIntEnv("VENPOD_SPARSE_STRESS_RADIUS_Y", 2u);
    const uint32_t sparseStressBudget = ReadUIntEnv("VENPOD_SPARSE_STRESS_BUDGET", 96u);
    const bool enableSparseStressCamera =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_STRESS_CAMERA", 0u) != 0u;
    const float sparseStressCameraRadius =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_STRESS_CAMERA_RADIUS", 640u));
    const float sparseStressCameraHeight =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_STRESS_CAMERA_HEIGHT", 220u));
    const float sparseStressCameraBaseHeight =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_STRESS_CAMERA_BASE_HEIGHT", 260u));
    const float sparseStressCameraSpeed =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_STRESS_CAMERA_SPEED", 90u));
    const bool enableSparseMissFeedback =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK", 1u) != 0u;
    const uint32_t sparseMissFeedbackMaxRecords = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_RECORDS", 256u);
    const bool enableSparseBrushFeedback =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_BRUSH_FEEDBACK", 0u) != 0u;
    const bool enableSparseBrushFeedbackApply =
        enableSparseBrushFeedback && ReadUIntEnv("VENPOD_SPARSE_BRUSH_FEEDBACK_APPLY", 0u) != 0u;
    const bool enableSparseBrushFeedbackAuthoritative =
        enableSparseBrushFeedbackApply &&
        ReadUIntEnv("VENPOD_SPARSE_BRUSH_FEEDBACK_AUTHORITATIVE", 0u) != 0u;
    const bool enableSparseBrushFeedbackDiagnosticSeed =
        enableSparseBrushFeedback &&
        ReadUIntEnv("VENPOD_SPARSE_BRUSH_FEEDBACK_DIAGNOSTIC_SEED", 0u) != 0u;
    const uint32_t sparseBrushFeedbackMaxRecords =
        ReadUIntEnv("VENPOD_SPARSE_BRUSH_FEEDBACK_RECORDS", 8192u);
    const uint32_t sparseMissFeedbackRayGrid = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_RAYS", 5u);
    const uint32_t sparseMissFeedbackDistance = ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_DISTANCE", 256u);
    const uint32_t sparseMissFeedbackStride = std::max(4u, ReadUIntEnv("VENPOD_SPARSE_MISS_FEEDBACK_STRIDE", 16u));
    const uint32_t sparseMissFeedbackInterval = std::max(1u, ReadUIntEnv(
        "VENPOD_SPARSE_MISS_FEEDBACK_INTERVAL",
        30u));
    const bool requireSparseGpuRaycastHealth =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_REQUIRE_GPU_RAYCAST_HEALTH", 0u) != 0u;
    const uint32_t sparseGpuRaycastHealthReadyFrame = std::max(
        1u,
        ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST_HEALTH_READY_FRAME", 120u));
    const uint32_t sparseGpuRaycastMinAccepted =
        ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST_MIN_ACCEPTED", 1u);
    const uint32_t sparseGpuRaycastMaxFallbackPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST_MAX_FALLBACK_PCT", 95u));
    const bool enableSparseGpuRaycastStrict =
        enableSparseGpuRaycast &&
        ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST_STRICT", 0u) != 0u;
    const bool requireSparseOwnershipQuality =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_REQUIRE_OWNERSHIP_QUALITY", 0u) != 0u;
    const bool requireSparseOwnershipStability =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_REQUIRE_OWNERSHIP_STABILITY", 0u) != 0u;
    const bool enableSparseSurfaceDiagnosticSeed =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_SURFACE_DIAGNOSTIC_SEED", 0u) != 0u;
    const bool enableSparseGpuRaycastDiagnosticSeed =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_GPU_RAYCAST_DIAGNOSTIC_SEED", 0u) != 0u;
    const bool requireSparseSurfaceFragments =
        sparseBackendRequested &&
        ReadUIntEnv("VENPOD_SPARSE_REQUIRE_SURFACE_FRAGMENTS", 0u) != 0u;
    const bool enableSparseRenderOwnershipStats =
        sparseBackendRequested &&
        (requireSparseOwnershipQuality ||
         requireSparseOwnershipStability ||
         requireSparseSurfaceFragments ||
         ReadUIntEnv("VENPOD_SPARSE_RENDER_OWNERSHIP", 1u) != 0u);
    const uint32_t sparseRenderOwnershipInterval = std::max(
        1u,
        ReadUIntEnv("VENPOD_SPARSE_RENDER_OWNERSHIP_INTERVAL", 60u));
    const uint32_t sparseOwnershipQualityReadyFrame = std::max(
        sparsePipeReadyFrame,
        ReadUIntEnv("VENPOD_SPARSE_OWNERSHIP_QUALITY_READY_FRAME", sparsePipeReadyFrame));
    const uint32_t sparseOwnershipMinTerrainPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_MIN_TERRAIN_PIXELS_PCT", 35u));
    const uint32_t sparseOwnershipMaxMissPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_MAX_MISS_PIXELS_PCT", 15u));
    const uint32_t sparseOwnershipMaxUnsafeNearMissPct =
        std::min(
            100u,
            ReadUIntEnv(
                "VENPOD_SPARSE_MAX_UNSAFE_NEAR_MISS_PIXELS_PCT",
                std::max(1u, sparseOwnershipMaxMissPct / 3u)));
    const uint32_t sparseOwnershipCatchupMissPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_CATCHUP_MISS_PIXELS_PCT", 18u));
    const uint32_t sparseOwnershipCatchupTerrainPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_CATCHUP_MIN_TERRAIN_PIXELS_PCT", 32u));
    const uint32_t sparseOwnershipCatchupHoldFrames =
        std::max(1u, ReadUIntEnv("VENPOD_SPARSE_CATCHUP_HOLD_FRAMES", 36u));
    const uint32_t sparseOwnershipCatchupReadyFrame =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_READY_FRAME", 12u);
    const uint32_t sparseCatchupVisibleRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_VISIBLE_REQUEST_BUDGET", std::max(48u, sparseNewRequestBudget * 2u));
    const uint32_t sparseCatchupCollisionRequestBudget =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_COLLISION_REQUEST_BUDGET", std::max(32u, sparseNewRequestBudget));
    const uint32_t sparseCatchupGenerationBudget =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_GENERATION_BUDGET", std::max(24u, sparseFeedbackGenerationBudget));
    const uint32_t sparseCatchupUploadBudget =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_UPLOAD_BUDGET", std::max(32u, sparseFeedbackUploadBudget));
    const uint32_t sparseCatchupSurfaceBudget =
        ReadUIntEnv("VENPOD_SPARSE_CATCHUP_SURFACE_BUDGET", std::max(32u, sparseUploadBudget));
    const uint32_t sparseOwnershipStabilityReadyFrame = std::max(
        sparseOwnershipQualityReadyFrame,
        ReadUIntEnv("VENPOD_SPARSE_OWNERSHIP_STABILITY_READY_FRAME", sparseOwnershipQualityReadyFrame));
    const uint32_t sparseOwnershipMaxTerrainDeltaPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_MAX_TERRAIN_DELTA_PCT", 25u));
    const uint32_t sparseOwnershipMaxMissDeltaPct =
        std::min(100u, ReadUIntEnv("VENPOD_SPARSE_MAX_MISS_DELTA_PCT", 12u));
    const uint32_t sparseSurfaceFragmentsReadyFrame = std::max(
        sparsePipeReadyFrame,
        ReadUIntEnv("VENPOD_SPARSE_SURFACE_FRAGMENTS_READY_FRAME", sparsePipeReadyFrame));
    const uint32_t sparseMinSurfaceFragments =
        ReadUIntEnv("VENPOD_SPARSE_MIN_SURFACE_FRAGMENTS", 512u);
    const bool enableSparseSurfaceUpload =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_UPLOAD", 1u) != 0u;
    const bool enableSparseSurfaceRaster =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_RASTER", 1u) != 0u;
    const bool enableSparseSurfaceRaymarchFill =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_RAYMARCH_FILL", 0u) != 0u;
    // Stable default: keep raster surfaces to a world-space near ownership
    // radius. Camera-shaped snapshots caused yaw-dependent holes/flicker, but
    // drawing every cached resident surface lets stale/off-axis near geometry
    // overlay the raymarched mid/far world. Distance-only near culling gives
    // the raster layer a stable contract while clipmaps/SVO own the horizon.
    const bool enableSparseSurfaceStableNearCull =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_STABLE_NEAR_CULL", 1u) != 0u;
    // Sparse surfaces use an input-assembler vertex-id stream, so D3D draw
    // offsets now have real vertex-buffer semantics. This lets GPU culling
    // compact indirect draw commands without losing each brick's face base.
    const bool enableSparseSurfaceIndirect =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_INDIRECT", 1u) != 0u;
    const bool enableSparseSurfaceCulling =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_SURFACE_CULLING", 1u) != 0u;
    const uint32_t sparseSurfaceExtractionBudget =
        ReadUIntEnv("VENPOD_SPARSE_SURFACE_EXTRACTION_BUDGET", std::max(4u, sparseUploadBudget));
    const uint32_t sparsePhysicsBrickBudget =
        ReadUIntEnv("VENPOD_SPARSE_PHYSICS_BRICK_BUDGET", 8u);
    const uint32_t sparsePhysicsMoveBudget =
        ReadUIntEnv("VENPOD_SPARSE_PHYSICS_MOVE_BUDGET", 256u);
    const bool enableSparsePhysicsDiagnosticSeed =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_SEED", 0u) != 0u;
    const bool enableSparsePhysicsDiagnosticFluidSeed =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_PHYSICS_DIAGNOSTIC_FLUID_SEED", 0u) != 0u;
    const uint32_t sparseSurfaceCullInterval =
        std::max(1u, ReadUIntEnv("VENPOD_SPARSE_SURFACE_CULL_INTERVAL", 6u));
    const float sparseSurfaceCullDistance =
        static_cast<float>(ReadUIntEnv(
            "VENPOD_SPARSE_SURFACE_CULL_DISTANCE",
            enableSparseSurfaceStableNearCull ? 1280u : 4200u));
    const float sparseSurfaceCullPadding =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_SURFACE_CULL_PADDING", 192u));
    const float sparseSurfaceCullMotionMinSpeed =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_SURFACE_CULL_MOTION_MIN_SPEED", 64u));
    const uint32_t sparseSurfaceCullTurnDegrees =
        std::min(90u, std::max(1u, ReadUIntEnv("VENPOD_SPARSE_SURFACE_CULL_TURN_DEGREES", 8u)));
    const float sparseSurfaceCullTurnDot =
        std::cos(static_cast<float>(sparseSurfaceCullTurnDegrees) * 0.017453292519943295f);
    uint32_t sparseSurfaceUploadedSerial = 0;
    bool sparseSurfaceUploadedCullValid = false;
    uint64_t sparseSurfaceLastCullFrame = 0;
    glm::vec3 sparseSurfaceUploadedCullForward{0.0f, 0.0f, 1.0f};
    Simulation::BrickCoord sparseSurfaceUploadedCullCenter{
        INT32_MIN,
        INT32_MIN,
        INT32_MIN
    };
    Simulation::BrickCoord sparseSurfaceUploadedCullLookaheadCenter{
        INT32_MIN,
        INT32_MIN,
        INT32_MIN
    };
    uint32_t sparseSurfaceDeferredPayloadsLastUpload = 0;
    uint32_t sparseSurfaceUploadRetriesLastFrame = 0;
    uint32_t sparseSurfaceRasterFacesLastFrame = 0;
    uint32_t sparseSurfaceExtractionBudgetLastFrame = 0;
    uint32_t sparseSurfaceLookaheadVisibleLastUpload = 0;
    Simulation::SparseClipmapConfig sparseClipmapConfig;
    sparseClipmapConfig.enabled =
        sparseBackendRequested && ReadUIntEnv("VENPOD_SPARSE_MID_CLIPMAP", 1u) != 0u;
    sparseClipmapConfig.startDistance =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_START", 384u));
    sparseClipmapConfig.endDistance =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_END", 6400u));
    sparseClipmapConfig.minCellSize =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_CELL", 16u));
    sparseClipmapConfig.nearExitPadding =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_NEAR_PADDING", 12u));
    sparseClipmapConfig.ringCount = ReadUIntEnv("VENPOD_SPARSE_MID_RINGS", 4u);
    sparseClipmapConfig.tileRadius = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_RADIUS", 3u);
    sparseClipmapConfig.tileSampleSide = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_SIDE", 33u);
    sparseClipmapConfig.maxTiles = ReadUIntEnv("VENPOD_SPARSE_MID_MAX_TILES", 256u);
    // The coarse 3D mid-voxel clipmap now has a narrow shader ownership
    // contract: it only owns moderate-angle mid-range rays, requires tagged
    // exposed voxels, and treats missing neighbors as unknown instead of air.
    // Keep the old env override so it can still be disabled for A/B testing.
    sparseClipmapConfig.voxelClipmapEnabled =
        ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_CLIPMAP", 1u) != 0u;
    sparseClipmapConfig.voxelBrickRadiusXz = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_RADIUS_XZ", 3u);
    sparseClipmapConfig.voxelBrickRadiusY = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_RADIUS_Y", 1u);
    sparseClipmapConfig.maxVoxelBricks = ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_MAX_BRICKS", 512u);
    sparseClipmapConfig.voxelInterestCapacityPercent =
        ReadUIntEnv("VENPOD_SPARSE_MID_VOXEL_INTEREST_PCT", 100u);
    sparseClipmapConfig.motionLookaheadMinSpeed =
        static_cast<float>(ReadUIntEnv("VENPOD_SPARSE_MID_MOTION_MIN_SPEED", 64u));
    sparseClipmapConfig.motionLookaheadSteps =
        ReadUIntEnv("VENPOD_SPARSE_MID_MOTION_STEPS", 3u);
    sparseClipmapConfig.seed = sparseWorldConfig.seed;
    const uint32_t sparseMidClipmapTileBudget = ReadUIntEnv("VENPOD_SPARSE_MID_TILE_BUDGET", 6u);
    Simulation::SparseClipmapPolicy sparseClipmapPolicy(sparseClipmapConfig);
    Simulation::SparseClipmapTileCache sparseClipmapTileCache;
    bool sparseClipmapTileCacheReady = false;
    uint32_t sparseMidClipmapUploadedHeightSerial = 0;
    uint32_t sparseMidClipmapUploadedVoxelSerial = 0;
    uint32_t sparseMidClipmapUploadRetriesLastFrame = 0;
    if (sparseBackendRequested) {
        sparseClipmapTileCacheReady = sparseClipmapTileCache.Initialize(sparseClipmapPolicy.Config());
        spdlog::info(
            "Sparse mid clipmap {}: start={:.0f} end={:.0f} cell={:.0f} rings={} tileRadius={} tileSide={} maxTiles={} voxelSlots={} voxelInterest={}pct motion={}x@{:.0f} budget={}",
            sparseClipmapPolicy.IsEnabled() ? "enabled" : "disabled",
            sparseClipmapPolicy.Config().startDistance,
            sparseClipmapPolicy.Config().endDistance,
            sparseClipmapPolicy.Config().minCellSize,
            sparseClipmapPolicy.Config().ringCount,
            sparseClipmapPolicy.Config().tileRadius,
            sparseClipmapPolicy.Config().tileSampleSide,
            sparseClipmapPolicy.Config().maxTiles,
            sparseClipmapPolicy.Config().maxVoxelBricks,
            sparseClipmapPolicy.Config().voxelInterestCapacityPercent,
            sparseClipmapPolicy.Config().motionLookaheadSteps,
            sparseClipmapPolicy.Config().motionLookaheadMinSpeed,
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
        sparseConfig.maxBrushFeedbackRecords = sparseBrushFeedbackMaxRecords;
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
            if (enableSparseEditFile) {
                std::error_code editFileExistsError;
                const bool editFileExists = std::filesystem::exists(sparseEditFilePath, editFileExistsError);
                if (editFileExistsError) {
                    spdlog::warn(
                        "Sparse edit persistence path could not be checked: {} ({})",
                        sparseEditFilePath.string(),
                        editFileExistsError.message());
                } else if (editFileExists) {
                    if (sparseVoxelWorld.LoadEditsFromFile(sparseEditFilePath, true)) {
                        spdlog::info(
                            "Loaded sparse edit overlays from {} (bricks={} voxels={})",
                            sparseEditFilePath.string(),
                            sparseVoxelWorld.GetEdits().EditedBrickCount(),
                            sparseVoxelWorld.GetEdits().EditedVoxelCount());
                    } else {
                        spdlog::warn("Failed to load sparse edit overlays from {}", sparseEditFilePath.string());
                    }
                } else {
                    spdlog::info("Sparse edit persistence path has no existing file: {}", sparseEditFilePath.string());
                }
            }
        }

        auto sparseGpuResult = sparseGpuResources.Initialize(
            device->GetDevice(),
            renderer->GetHeapManager(),
            sparseConfig);
        if (!sparseGpuResult) {
            spdlog::error("Sparse GPU resource initialization failed: {}", sparseGpuResult.error());
        } else {
            spdlog::info(
                "Sparse backend GPU resources initialized: page-table path active{}",
                enableSparseSurfaceAuthoritative ? " with surface-authoritative raster near field" : "");
            sparseGpuPageTableResetPending = true;
        }

        if (enableSparseSurfaceUpload) {
            SparseSurfaceGpuConfig surfaceConfig;
            surfaceConfig.maxFaces = ReadUIntEnv("VENPOD_SPARSE_SURFACE_MAX_FACES", surfaceConfig.maxFaces);
            surfaceConfig.maxBrickRanges = ReadUIntEnv("VENPOD_SPARSE_SURFACE_MAX_RANGES", surfaceConfig.maxBrickRanges);
            surfaceConfig.maxDrawCommands =
                ReadUIntEnv("VENPOD_SPARSE_SURFACE_MAX_DRAW_COMMANDS", surfaceConfig.maxDrawCommands);
            surfaceConfig.useRangeAllocator =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_RANGE_ALLOCATOR",
                    1u) != 0u;
            surfaceConfig.useFixedRangeTable =
                ReadUIntEnv("VENPOD_SPARSE_SURFACE_FIXED_RANGE_TABLE", surfaceConfig.useFixedRangeTable ? 1u : 0u) != 0u;
            surfaceConfig.useStableDrawSlots =
                ReadUIntEnv("VENPOD_SPARSE_SURFACE_STABLE_DRAW_SLOTS", surfaceConfig.useStableDrawSlots ? 1u : 0u) != 0u;
            surfaceConfig.compactStableDrawCommands =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_COMPACT_STABLE_DRAWS",
                    surfaceConfig.compactStableDrawCommands ? 1u : 0u) != 0u;
            surfaceConfig.useGpuCull =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_GPU_CULL",
                    (enableSparseSurfaceIndirect && enableSparseSurfaceCulling) ? 1u : 0u) != 0u;
            surfaceConfig.uploadBytesPerSlot =
                ReadUIntEnv("VENPOD_SPARSE_SURFACE_UPLOAD_SLOT_BYTES", surfaceConfig.uploadBytesPerSlot);
            surfaceConfig.maxPayloadCopyRegionsPerFrame =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_COPY_REGION_BUDGET",
                    surfaceConfig.maxPayloadCopyRegionsPerFrame);
            surfaceConfig.maxPayloadCopyFacesPerFrame =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_COPY_FACE_BUDGET",
                    surfaceConfig.maxPayloadCopyFacesPerFrame);
            surfaceConfig.rangeRetirementDelayFrames =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_RANGE_RETIRE_FRAMES",
                    surfaceConfig.rangeRetirementDelayFrames);
            surfaceConfig.surfaceRecordsPerCluster =
                std::min(
                    64u,
                    std::max(
                        1u,
                        ReadUIntEnv(
                            "VENPOD_SPARSE_SURFACE_RECORDS_PER_CLUSTER",
                            surfaceConfig.surfaceRecordsPerCluster)));
            surfaceConfig.surfaceClusterMaxExtentVoxels =
                std::min(
                    4096u,
                    ReadUIntEnv(
                        "VENPOD_SPARSE_SURFACE_CLUSTER_MAX_EXTENT",
                        surfaceConfig.surfaceClusterMaxExtentVoxels));
            surfaceConfig.surfaceClusterFastAcceptMaxRecords =
                std::min(
                    64u,
                    ReadUIntEnv(
                        "VENPOD_SPARSE_SURFACE_CLUSTER_FAST_RECORDS",
                        surfaceConfig.surfaceClusterFastAcceptMaxRecords));
            surfaceConfig.surfaceClusterFastAcceptMaxFaces =
                ReadUIntEnv(
                    "VENPOD_SPARSE_SURFACE_CLUSTER_FAST_FACES",
                    surfaceConfig.surfaceClusterFastAcceptMaxFaces);
            auto surfaceGpuResult = sparseSurfaceGpuResources.Initialize(
                device->GetDevice(),
                renderer->GetHeapManager(),
                &renderer->GetShaderCompiler(),
                shaderPath,
                surfaceConfig);
            if (!surfaceGpuResult) {
                spdlog::error("Sparse surface GPU resource initialization failed: {}", surfaceGpuResult.error());
            } else {
                const auto& surfaceGpuStats = sparseSurfaceGpuResources.GetStats();
                spdlog::info(
                    "Sparse surface GPU buffers initialized: faces={} ranges={} drawCommands={} rangeAllocator={} fixedRangeTable={} stableDrawSlots={} compactStableDraws={} gpuCull={} clusterRecords={} clusterExtent={} clusterFast={}/{} retireFrames={} uploadSlotMB={:.2f} copyBudget={}/{} iaFaces={} iaVerts={} iaIndices={} iaMB={:.2f}/{:.2f} iaGpuLocal={} iaUploadPending={}",
                    surfaceConfig.maxFaces,
                    surfaceConfig.maxBrickRanges,
                    surfaceConfig.maxDrawCommands,
                    surfaceConfig.useRangeAllocator ? "enabled" : "disabled",
                    surfaceConfig.useFixedRangeTable ? "enabled" : "disabled",
                    surfaceConfig.useStableDrawSlots ? "enabled" : "disabled",
                    surfaceConfig.compactStableDrawCommands ? "enabled" : "disabled",
                    surfaceConfig.useGpuCull ? "enabled" : "disabled",
                    surfaceConfig.surfaceRecordsPerCluster,
                    surfaceConfig.surfaceClusterMaxExtentVoxels,
                    surfaceConfig.surfaceClusterFastAcceptMaxRecords,
                    surfaceConfig.surfaceClusterFastAcceptMaxFaces,
                    surfaceConfig.rangeRetirementDelayFrames,
                    static_cast<double>(surfaceConfig.uploadBytesPerSlot) / (1024.0 * 1024.0),
                    surfaceConfig.maxPayloadCopyRegionsPerFrame,
                    surfaceConfig.maxPayloadCopyFacesPerFrame,
                    surfaceGpuStats.iaStreamCapacityFaces,
                    surfaceGpuStats.iaStreamVertexCount,
                    surfaceGpuStats.iaStreamIndexCount,
                    static_cast<double>(surfaceGpuStats.iaStreamVertexBytes) / (1024.0 * 1024.0),
                    static_cast<double>(surfaceGpuStats.iaStreamIndexBytes) / (1024.0 * 1024.0),
                    surfaceGpuStats.iaStreamGpuLocal ? "yes" : "no",
                    surfaceGpuStats.iaStreamUploadPending ? "yes" : "no");
            }
        } else if (sparseBackendRequested) {
            spdlog::info("Sparse surface GPU upload disabled by VENPOD_SPARSE_SURFACE_UPLOAD=0");
        }
        spdlog::info(
            "Sparse surface raster path: {} | stableNearCull={} | CPU frustum culling: {} dist={:.0f} padding={:.0f} interval={}",
            enableSparseSurfaceRaster ? "enabled" : "disabled",
            enableSparseSurfaceStableNearCull ? "enabled" : "disabled",
            enableSparseSurfaceCulling ? "enabled" : "disabled",
            sparseSurfaceCullDistance,
            sparseSurfaceCullPadding,
            sparseSurfaceCullInterval);
        spdlog::info(
            "Sparse surface indirect draw path: {}",
            enableSparseSurfaceIndirect ? "enabled" : "disabled");
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
        // In sparse surface-authoritative mode the dense VoxelWorld is only a
        // compatibility owner for legacy descriptor/result buffers and the
        // material palette. Terrain, brush edits, collision, and rendering are
        // owned by sparse bricks. Keep this dense shim intentionally tiny so
        // sparse testing does not pay hundreds of MB of irrelevant startup
        // allocation before the sparse path even begins streaming.
        const uint32_t sparseCompatGridDefault = enableSparseSurfaceAuthoritative ? 64u : 512u;
        const uint32_t sparseCompatGridYDefault = enableSparseSurfaceAuthoritative ? 64u : 384u;
        voxelConfig.gridSizeX = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_X", sparseCompatGridDefault);
        voxelConfig.gridSizeY = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_Y", sparseCompatGridYDefault);
        voxelConfig.gridSizeZ = ReadUIntEnv("VENPOD_SPARSE_TEST_GRID_Z", sparseCompatGridDefault);
        voxelConfig.enableRaycastResultBuffers = enableSparseGpuRaycast;
        voxelConfig.enableBrushEditFeedbackBuffers = false;
        spdlog::info(
            "Sparse runtime test mode: dense compatibility VoxelWorld {}x{}x{} and infinite chunk streaming disabled{}",
            voxelConfig.gridSizeX,
            voxelConfig.gridSizeY,
            voxelConfig.gridSizeZ,
            enableSparseSurfaceAuthoritative ? " (surface-authoritative sparse owner)" : "");
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
    Simulation::PhysicsDispatcherConfig physicsDispatcherConfig;
    physicsDispatcherConfig.enableSparseRaycastPipeline = enableSparseGpuRaycast;
    physicsDispatcherConfig.enableSparseMissFeedbackPipeline = enableSparseMissFeedback;
    physicsDispatcherConfig.enableSparseBrushFeedbackPipeline = enableSparseBrushFeedback;
    physicsDispatcherConfig.enableSparsePhysicsPacketPipeline = enableSparsePhysicsGpu;
    if (sparseRuntimeTestMode) {
        physicsDispatcherConfig.enableDenseSimulationPipelines = false;
        physicsDispatcherConfig.enableDenseRaycastPipelines = false;
        physicsDispatcherConfig.enableIndirectCommandSignature = false;
    }
    auto physicsResult = physicsDispatcher->Initialize(
        device->GetDevice(),
        renderer->GetShaderCompiler(),
        renderer->GetHeapManager(),
        shaderPath,
        physicsDispatcherConfig
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
        const auto scenicSpawn = sparseVoxelWorld.GetTerrain().FindScenicSpawn(
            96,
            96,
            playerHeight,
            static_cast<int32_t>(ReadUIntEnv("VENPOD_SPARSE_SPAWN_SEARCH_RADIUS", 448u)),
            static_cast<int32_t>(ReadUIntEnv("VENPOD_SPARSE_SPAWN_SAMPLE_SPACING", 32u)));
        if (scenicSpawn.found) {
            cameraPos = glm::vec3(
                static_cast<float>(scenicSpawn.worldX) + 0.5f,
                scenicSpawn.eyeY,
                static_cast<float>(scenicSpawn.worldZ) + 0.5f);
            cameraYaw = scenicSpawn.yaw;
            cameraPitch = scenicSpawn.pitch;
            cameraVelocityY = 0.0f;
            spdlog::info(
                "Sparse scenic spawn world=({:.1f},{:.1f},{:.1f}) groundY={} yaw={:.2f} pitch={:.2f} score={:.2f} forwardClearance={:.1f} localRelief={:.1f}",
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                scenicSpawn.groundY,
                cameraYaw,
                cameraPitch,
                scenicSpawn.score,
                scenicSpawn.forwardClearance,
                scenicSpawn.localRelief);
        } else {
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
                spdlog::warn(
                    "Sparse scenic spawn failed; fallback raycast spawn world=({:.1f},{:.1f},{:.1f}) groundY={}",
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    spawnGround.voxelY);
            } else {
                cameraPos.y = scenicSpawn.eyeY;
                spdlog::warn(
                    "Sparse scenic spawn and fallback probe found no validated ground; using generator fallback world=({:.1f},{:.1f},{:.1f}) groundY={}",
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    scenicSpawn.groundY);
            }
        }

        const uint32_t startupPrewarmRadiusXz =
            ReadUIntEnv("VENPOD_SPARSE_STARTUP_PREWARM_RADIUS_XZ", 2u);
        const uint32_t startupPrewarmRadiusY =
            ReadUIntEnv("VENPOD_SPARSE_STARTUP_PREWARM_RADIUS_Y", 1u);
        const uint32_t startupPrewarmGenerationBudget =
            ReadUIntEnv("VENPOD_SPARSE_STARTUP_PREWARM_GENERATION", 96u);
        const uint32_t startupPrewarmSurfaceBudget =
            ReadUIntEnv("VENPOD_SPARSE_STARTUP_PREWARM_SURFACE", 96u);
        if (startupPrewarmGenerationBudget > 0u || startupPrewarmSurfaceBudget > 0u) {
            const auto centerCoord = Simulation::BrickCoord::FromWorldVoxel(
                static_cast<int32_t>(std::floor(cameraPos.x)),
                static_cast<int32_t>(std::floor(cameraPos.y - playerHeight)),
                static_cast<int32_t>(std::floor(cameraPos.z)));
            std::vector<Simulation::SparseBrickRequest> startupRequests;
            startupRequests.reserve(
                (startupPrewarmRadiusXz * 2u + 1u) *
                (startupPrewarmRadiusXz * 2u + 1u) *
                (startupPrewarmRadiusY * 2u + 1u));
            for (int32_t dy = -static_cast<int32_t>(startupPrewarmRadiusY);
                 dy <= static_cast<int32_t>(startupPrewarmRadiusY);
                 ++dy) {
                for (int32_t dz = -static_cast<int32_t>(startupPrewarmRadiusXz);
                     dz <= static_cast<int32_t>(startupPrewarmRadiusXz);
                     ++dz) {
                    for (int32_t dx = -static_cast<int32_t>(startupPrewarmRadiusXz);
                         dx <= static_cast<int32_t>(startupPrewarmRadiusXz);
                         ++dx) {
                        Simulation::SparseBrickRequest request;
                        request.coord = {
                            centerCoord.x + dx,
                            centerCoord.y + dy,
                            centerCoord.z + dz
                        };
                        request.residencyClass =
                            (std::abs(dx) <= 1 && std::abs(dy) <= 1 && std::abs(dz) <= 1)
                                ? Simulation::SparseResidencyClass::Collision
                                : Simulation::SparseResidencyClass::Visible;
                        request.urgent = request.residencyClass == Simulation::SparseResidencyClass::Collision;
                        request.priority = 4096 -
                            (std::abs(dx) * 16 + std::abs(dz) * 16 + std::abs(dy) * 24);
                        startupRequests.push_back(request);
                    }
                }
            }
            std::sort(
                startupRequests.begin(),
                startupRequests.end(),
                [](const auto& lhs, const auto& rhs) {
                    if (lhs.priority != rhs.priority) {
                        return lhs.priority > rhs.priority;
                    }
                    return lhs.coord < rhs.coord;
                });
            uint32_t acceptedStartupRequests = 0;
            for (const auto& request : startupRequests) {
                const auto result = sparseVoxelWorld.RequestBrickDetailed(request.coord);
                if (result == Simulation::SparseBrickRequestResult::Allocated ||
                    result == Simulation::SparseBrickRequestResult::AlreadyResident) {
                    sparseVoxelWorld.TouchResidencyClass(request.coord, request.residencyClass, 0u);
                    ++acceptedStartupRequests;
                }
            }
            const uint32_t generatedStartup = sparseVoxelWorld.PumpGenerationAround(
                startupPrewarmGenerationBudget,
                centerCoord,
                0u);
            const uint32_t extractedStartup = sparseVoxelWorld.PumpSurfaceExtractionAround(
                startupPrewarmSurfaceBudget,
                centerCoord,
                0u);
            sparseVoxelWorld.FlushStats();
            const auto& startupStats = sparseVoxelWorld.GetStats();
            spdlog::info(
                "Sparse startup prewarm center={} {} {} radius={}x{} accepted={} generated={} surfaces={} cachedSurface={}/{} genQ={} uploadQ={} surfQ={}",
                centerCoord.x,
                centerCoord.y,
                centerCoord.z,
                startupPrewarmRadiusXz,
                startupPrewarmRadiusY,
                acceptedStartupRequests,
                generatedStartup,
                extractedStartup,
                startupStats.surfaceCachedBricks,
                startupStats.surfaceFaces,
                startupStats.generationQueuedBricks,
                startupStats.uploadQueuedBricks,
                startupStats.surfaceExtractionQueuedBricks);
        }
    }

    // Player position represents feet/collision point
    // Camera rendering position is offset upward by playerHeight for natural eye-level view

    // Main loop
    bool running = true;
    bool paused = false;
    uint64_t frameCount = 0;
    uint64_t lastFarSvoUploadProgressBytes = UINT64_MAX;
    const uint32_t exitAfterFrames = ReadUIntEnv("VENPOD_EXIT_AFTER_FRAMES", 0u);
    BackbufferCaptureConfig backbufferCapture = {};
    if (const char* captureDir = std::getenv("VENPOD_CAPTURE_DIR");
        captureDir && captureDir[0] != '\0') {
        backbufferCapture.enabled = true;
        backbufferCapture.outputDir = captureDir;
        backbufferCapture.startFrame = ReadUIntEnv("VENPOD_CAPTURE_START_FRAME", 120u);
        backbufferCapture.intervalFrames = std::max(1u, ReadUIntEnv("VENPOD_CAPTURE_INTERVAL_FRAMES", 30u));
        backbufferCapture.count = std::max(1u, ReadUIntEnv("VENPOD_CAPTURE_COUNT", 8u));
        std::filesystem::create_directories(backbufferCapture.outputDir);
        spdlog::info(
            "Backbuffer capture enabled: dir={} start={} interval={} count={}",
            backbufferCapture.outputDir.string(),
            backbufferCapture.startFrame,
            backbufferCapture.intervalFrames,
            backbufferCapture.count);
    }
    const bool traceFrameStages = std::getenv("VENPOD_TRACE_FRAME_STAGES") != nullptr;
    bool mouseInitialized = false;  // Track if mouse capture has been enabled
    uint64_t lastFrameCounter = SDL_GetPerformanceCounter();
    const double performanceFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    float smoothedFrameMs = 16.67f;
    float lastRawFrameMs = 16.67f;
    uint64_t physicsDispatchCount = 0;
    uint32_t backbufferCapturesQueued = 0;
    std::vector<PendingBackbufferCapture> pendingBackbufferCaptures;
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
    uint32_t sparseBackgroundQualityTier = 0;
    uint64_t farSvoUploadBudgetLastFrame = 0;
    uint32_t farSvoUploadPressureTier = 0;
    float farSvoUploadScale = 1.0f;
    float farSvoUploadMsLastFrame = 0.0f;
    float farSvoUploadMsSmoothed = 0.0f;
    float perfFenceWaitMs = 0.0f;
    float perfChunkUpdateMs = 0.0f;
    float perfPhysicsSubmitMs = 0.0f;
    float perfBrushSubmitMs = 0.0f;
    float perfRenderSubmitMs = 0.0f;
    float perfPresentMs = 0.0f;
    float schedulerPredictedFrameMs = 16.67f;
    float schedulerFrameDebtMs = 0.0f;
    float schedulerBudgetPressureMs = 16.67f;
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
    bool sparseMissFeedbackUrgentLastFrame = false;
    uint32_t sparseMissFeedbackRayGridLastFrame = 0;
    uint32_t sparseMissFeedbackDistanceLastFrame = 0;
    uint32_t sparseMissFeedbackStrideLastFrame = 0;
    uint32_t sparseSpeculativeRequestsLastFrame = 0;
    uint32_t sparseVisibleRequestsLastFrame = 0;
    uint32_t sparseCollisionRequestsLastFrame = 0;
    uint32_t sparseSpeculativeRequestBudgetLastFrame = 0;
    uint32_t sparseVisibleRequestBudgetLastFrame = 0;
    uint32_t sparseCollisionRequestBudgetLastFrame = 0;
    uint32_t sparseTotalRequestBudgetLastFrame = 0;
    uint32_t sparseFastRequestScaleLastFrame = 1;
    uint32_t sparseProtectedRequestOverageLastFrame = 0;
    uint32_t sparseBrushIntentActiveLastFrame = 0;
    uint32_t sparseBrushCollisionReserveLastFrame = 0;
    uint32_t sparseBrushCollisionMaxLastFrame = 0;
    uint32_t sparseRequestFreePageSkipsLastFrame = 0;
    uint32_t sparseRequestClassBudgetSkipsLastFrame = 0;
    uint32_t sparseRequestTotalBudgetSkipsLastFrame = 0;
    uint32_t sparseRequestRejectedSkipsLastFrame = 0;
    uint32_t sparseRequestKnownEmptySkipsLastFrame = 0;
    uint32_t sparseStressRequestsLastFrame = 0;
    uint32_t sparseStressAcceptedLastFrame = 0;
    uint32_t sparseStressCursor = 0;
    uint32_t sparsePressureTrimLastFrame = 0;
    uint32_t sparseReplacementEvictionsLastFrame = 0;
    uint32_t sparseSpeculativeBackpressureSkipsLastFrame = 0;
    uint32_t sparseDistanceTrimSkippedLastFrame = 0;
    constexpr uint32_t kBackendPipeCpuWorld = 1u << 0;
    constexpr uint32_t kBackendPipeGpuResources = 1u << 1;
    constexpr uint32_t kBackendPipeRaymarch = 1u << 2;
    constexpr uint32_t kBackendPipeNearBinding = 1u << 3;
    constexpr uint32_t kBackendPipeSurfaceGpu = 1u << 4;
    constexpr uint32_t kBackendPipeSurfaceRaster = 1u << 5;
    constexpr uint32_t kBackendPipeSurfaceAuthoritative = 1u << 6;
    constexpr uint32_t kBackendPipeMidClipmap = 1u << 7;
    constexpr uint32_t kBackendPipeFarSvo = 1u << 8;
    constexpr uint32_t kBackendPipeOwnership = 1u << 9;
    constexpr uint32_t kBackendPipeCollision = 1u << 10;
    constexpr uint32_t kBackendPipePhysics = 1u << 11;
    const auto sparsePipeMaskNames = [&](uint32_t mask) {
        std::string names;
        const auto appendName = [&](uint32_t bit, const char* name) {
            if ((mask & bit) == 0u) {
                return;
            }
            if (!names.empty()) {
                names += ",";
            }
            names += name;
        };
        appendName(kBackendPipeCpuWorld, "cpu-world");
        appendName(kBackendPipeGpuResources, "gpu-resources");
        appendName(kBackendPipeRaymarch, "raymarch");
        appendName(kBackendPipeNearBinding, "near-binding");
        appendName(kBackendPipeSurfaceGpu, "surface-gpu");
        appendName(kBackendPipeSurfaceRaster, "surface-raster");
        appendName(kBackendPipeSurfaceAuthoritative, "surface-authoritative");
        appendName(kBackendPipeMidClipmap, "mid-clipmap");
        appendName(kBackendPipeFarSvo, "far-svo");
        appendName(kBackendPipeOwnership, "ownership");
        appendName(kBackendPipeCollision, "collision");
        appendName(kBackendPipePhysics, "physics");
        return names.empty() ? std::string("none") : names;
    };
    uint32_t sparseBackendConfiguredMaskLastFrame = 0;
    uint32_t sparseBackendActiveMaskLastFrame = 0;
    uint32_t sparseBackendWarnMaskLastFrame = 0;
    uint64_t sparseBackendLastWarnFrame = 0;
    bool sparseBackendPipeReadyFailed = false;
    bool sparseBackendPipeReadyObserved = false;
    uint32_t sparseBackendPipeReadyFailureMask = 0;
    bool sparseOwnershipQualityFailed = false;
    bool sparseOwnershipQualityObserved = false;
    uint32_t sparseOwnershipQualityFrame = 0;
    uint32_t sparseOwnershipQualityTerrainPct = 0;
    uint32_t sparseOwnershipQualityMissPct = 0;
    uint32_t sparseOwnershipQualityUnsafeNearMissPct = 0;
    bool sparseOwnershipStabilityFailed = false;
    bool sparseOwnershipStabilityPrimed = false;
    bool sparseOwnershipStabilityObserved = false;
    uint32_t sparseOwnershipStabilityFrame = 0;
    uint32_t sparseOwnershipStabilityFailurePreviousFrame = 0;
    uint32_t sparseOwnershipStabilityPreviousFrame = 0;
    uint32_t sparseOwnershipStabilityPreviousTerrainPct = 0;
    uint32_t sparseOwnershipStabilityPreviousMissPct = 0;
    uint32_t sparseOwnershipStabilityTerrainDeltaPct = 0;
    uint32_t sparseOwnershipStabilityMissDeltaPct = 0;
    bool sparseSurfaceDiagnosticSeedQueued = false;
    uint32_t sparseSurfaceDiagnosticSeededVoxels = 0;
    uint32_t sparseSurfaceDiagnosticSeededBricks = 0;
    bool sparseSurfaceFragmentsFailed = false;
    bool sparseSurfaceFragmentsObserved = false;
    uint32_t sparseSurfaceFragmentsFrame = 0;
    uint32_t sparseSurfaceFragmentsLastRetire = 0;
    uint32_t sparseSurfaceOwnedPixelsLastRetire = 0;
    bool sparseGpuRaycastDiagnosticSeedQueued = false;
    uint32_t sparseGpuRaycastDiagnosticSeededVoxels = 0;
    uint32_t sparseGpuRaycastDiagnosticSeededBricks = 0;
    uint32_t sparseGpuRaycastAcceptedLastFrame = 0;
    uint32_t sparseGpuRaycastRejectedLastFrame = 0;
    uint32_t sparseGpuRaycastMissLastFrame = 0;
    uint32_t sparseGpuRaycastFallbackLastFrame = 0;
    uint32_t sparseGpuRaycastAcceptedSinceReady = 0;
    uint32_t sparseGpuRaycastRejectedSinceReady = 0;
    uint32_t sparseGpuRaycastMissSinceReady = 0;
    uint32_t sparseGpuRaycastFallbackSinceReady = 0;
    uint32_t sparseBrushStrokeDeltasLastFrame = 0;
    uint32_t sparseBrushStrokeDeltaBricksLastFrame = 0;
    uint32_t sparseBrushStrokeDeltaMismatchesLastFrame = 0;
    uint32_t sparseBrushFeedbackQueuedLastFrame = 0;
    uint32_t sparseBrushFeedbackRetiredLastFrame = 0;
    uint32_t sparseBrushFeedbackAppliedLastFrame = 0;
    uint32_t sparseBrushFeedbackOverflowLastFrame = 0;
    uint32_t sparseBrushFeedbackMissingResidentLastFrame = 0;
    uint32_t sparseBrushFeedbackCpuFallbackLastFrame = 0;
    uint32_t sparseBrushFeedbackMissingResidentHintsLastFrame = 0;
    uint32_t sparseBrushFeedbackMissingResidentRequestsLastFrame = 0;
    uint32_t sparseBrushFeedbackParityExpectedLastFrame = 0;
    uint32_t sparseBrushFeedbackParityMatchedLastFrame = 0;
    uint32_t sparseBrushFeedbackParityMissingLastFrame = 0;
    uint32_t sparseBrushFeedbackParityUnexpectedLastFrame = 0;
    uint32_t sparseBrushFeedbackParityValueMismatchLastFrame = 0;
    bool sparseBrushFeedbackParityExpectsMissingResident = false;
    bool sparseBrushFeedbackDiagnosticQueued = false;
    bool sparseBrushFeedbackParityPending = false;
    bool sparseBrushFeedbackParityObserved = false;
    bool sparseBrushFeedbackParityFailed = false;
    uint32_t sparseBrushFeedbackParityExpectedFrame = 0;
    uint32_t sparseBrushFeedbackParityFailureFrame = 0;
    uint32_t sparseBrushFeedbackDiagnosticStage = 0;
    uint32_t sparseBrushFeedbackDiagnosticCasesPassed = 0;
    uint32_t sparseBrushFeedbackDiagnosticNextFrame = 90;
    bool sparseBrushFeedbackDiagnosticSuitePassed = false;
    bool sparseBrushFeedbackDiagnosticCenterPinned = false;
    glm::vec3 sparseBrushFeedbackDiagnosticCenter{0.0f};
    std::string sparseBrushFeedbackParityLabel;
    std::vector<Simulation::SparseBrushFeedbackRecord> sparseBrushFeedbackParityExpected;
    bool sparseGpuRaycastHealthObserved = false;
    bool sparseGpuRaycastHealthFailed = false;
    uint32_t sparseGpuRaycastHealthFailureFrame = 0;
    uint32_t sparseGpuRaycastFallbackPctAtFailure = 0;
    uint32_t sparseBodyCollisionBlockedLastFrame = 0;
    uint32_t sparseBodyCollisionStepUpsLastFrame = 0;
    uint32_t sparseBodyCollisionGroundedLastFrame = 0;
    uint32_t sparseBodyCollisionGroundSnapsLastFrame = 0;
    uint32_t sparseBodyCollisionVerticalBlockedLastFrame = 0;
    uint32_t sparseBodyCollisionLandedLastFrame = 0;
    uint32_t sparseBodyCollisionCeilingLastFrame = 0;
    uint32_t sparseBodyCollisionSampledLastFrame = 0;
    uint32_t sparseBodyCollisionSolidLastFrame = 0;
    uint32_t sparseBodyCollisionLiquidLastFrame = 0;
    float sparseBodyCollisionSafeFractionLastFrame = 1.0f;
    uint32_t sparseUploadRequeuesLastFrame = 0;
    uint32_t sparseInvalidationRequeuesLastFrame = 0;
    uint32_t sparsePageTablePublishRetriesLastFrame = 0;
    uint32_t sparsePageTablePublishStaleDropsLastFrame = 0;
    uint32_t sparseEditedPageTablePublishesQueuedLastFrame = 0;
    uint32_t sparseEditedPageTablePublishesPublishedLastFrame = 0;
    uint32_t sparseEditedPageTablePublishPromotionsLastFrame = 0;
    uint32_t sparseUploadRingBudgetDefersLastFrame = 0;
    uint64_t sparseUploadRingUsedBytesLastFrame = 0;
    uint64_t sparseUploadRingCapacityBytesLastFrame = 0;
    uint64_t sparseFrameUploadReservedBytesLastFrame = 0;
    uint64_t sparseFrameUploadRemainingBytesLastFrame = 0;
    uint32_t sparseFrameUploadPlanDefersLastFrame = 0;
    uint32_t sparseValueSelectedUploadsLastFrame = 0;
    bool sparsePhysicsDiagnosticSeedQueued = false;
    uint32_t sparseGenerationBudgetLastFrame = 0;
    uint32_t sparseUploadBudgetLastFrame = 0;
    uint32_t sparseMidClipmapBudgetLastFrame = 0;
    uint32_t sparsePhysicsBrickBudgetLastFrame = sparsePhysicsBrickBudget;
    uint32_t sparsePhysicsMoveBudgetLastFrame = sparsePhysicsMoveBudget;
    float sparseRuntimeBudgetScale = 1.0f;
    float sparseProtectedRuntimeBudgetScale = 1.0f;
    float sparseBackgroundRuntimeBudgetScale = 1.0f;
    Simulation::SparseRuntimePressureClass sparseRuntimePressureClass =
        Simulation::SparseRuntimePressureClass::Idle;
    uint32_t sparseProtectedBacklogLastFrame = 0;
    uint32_t sparseTrimSpeculativeFirstLastFrame = 0;
    uint32_t sparseOwnershipTerrainPctLastRetire = 0;
    uint32_t sparseOwnershipMissPctLastRetire = 0;
    uint32_t sparseOwnershipUnsafeNearMissPctLastRetire = 0;
    uint32_t sparseResidencyCatchupFramesRemaining = 0;
    uint32_t sparseResidencyCatchupLastFrame = 0;
    uint32_t sparseOwnershipPressureLevelActive = 0;
    uint32_t sparseOwnershipPressureLevelLastRetire = 0;
    uint32_t sparseOwnershipPressureTerrainDeficitLastRetire = 0;
    uint32_t sparseOwnershipPressureMissExcessLastRetire = 0;
    uint32_t sparseOwnershipPressureUnsafeNearMissExcessLastRetire = 0;
    float sparseOwnershipMidVoxelPixelShareLastRetire = 0.0f;
    float sparseOwnershipFarHeightPixelShareLastRetire = 0.0f;
    float sparseOwnershipSkyPixelShareLastRetire = 0.0f;
    float sparseOwnershipBackgroundPixelShareLastRetire = 1.0f;
    float sparseRaymarchBudgetScale = 1.0f;
    Simulation::SparsePagePublishQueue sparsePagePublishQueue;
    uint32_t sparseBrushFeedbackLastAppliedFrame = 0;
    std::deque<Simulation::BrickCoord> sparseBrushFeedbackMissingResidentRequestQueue;
    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash>
        sparseBrushFeedbackMissingResidentRequestSet;
    struct SparseBrushFeedbackPendingStroke {
        uint32_t frame = 0;
        float worldX = 0.0f;
        float worldY = 0.0f;
        float worldZ = 0.0f;
        float radius = 0.0f;
        uint32_t material = 0;
        uint32_t mode = 0;
        uint32_t shape = 0;
        float strength = 1.0f;
        uint32_t seed = 0;
        int32_t hitNormalX = 0;
        int32_t hitNormalY = 0;
        int32_t hitNormalZ = 0;
        bool hasHitNormal = false;
    };
    std::deque<SparseBrushFeedbackPendingStroke> sparseBrushFeedbackPendingStrokes;
    std::array<char, 512> sparseEditUiPathBuffer = {};
    {
        const std::filesystem::path initialSparseEditUiPath =
            enableSparseEditFile
                ? sparseEditFilePath
                : ResolveSandboxUserPath("saves/review-edits.vsed");
        const std::string initialSparseEditUiPathText = initialSparseEditUiPath.string();
        const size_t copyLength = std::min(
            initialSparseEditUiPathText.size(),
            sparseEditUiPathBuffer.size() - 1u);
        std::memcpy(sparseEditUiPathBuffer.data(), initialSparseEditUiPathText.data(), copyLength);
    }
    std::string sparseEditUiStatus =
        enableSparseEditFile
            ? "autosave enabled for shutdown"
            : "manual save/load ready";
    auto enqueueSparsePageTablePublish = [&](
        uint32_t entryIndex,
        const Simulation::BrickCoord& coord,
        uint32_t pageIndex,
        uint32_t generation,
        Simulation::SparseResidencyClass residencyClass) {
        const uint32_t readyFrame =
            static_cast<uint32_t>(std::min<uint64_t>(
                frameCount + 1u,
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
        const uint64_t readyFenceValue = commandQueue
            ? commandQueue->GetNextFenceValue()
            : 0u;
        const Simulation::SparsePagePublishQueueEvent event =
            sparsePagePublishQueue.Enqueue(
                entryIndex,
                coord,
                pageIndex,
                generation,
                readyFrame,
                readyFenceValue,
                residencyClass);
        if (event == Simulation::SparsePagePublishQueueEvent::QueuedEdited) {
            ++sparseEditedPageTablePublishesQueuedLastFrame;
        } else if (event == Simulation::SparsePagePublishQueueEvent::PromotedEdited) {
            ++sparseEditedPageTablePublishesQueuedLastFrame;
            ++sparseEditedPageTablePublishPromotionsLastFrame;
        }
    };
    float boundaryTestElapsedSeconds = 0.0f;
    float sparseStressCameraElapsedSeconds = 0.0f;
    glm::vec3 sparseStressCameraOrigin = cameraPos;

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

        if (enableFarSVO &&
            !sparseFarField.enabled &&
            (farVoxelOctree.IsAsyncPending() || farVoxelOctree.IsGpuUploadPending())) {
            const bool legacyDenseStreamingBlocksFarFinalize =
                !enableSparseSurfaceAuthoritative &&
                (streamingStillFilling || sourceStillFilling);
            const bool farFinalizeFrameIsCheap =
                frameCount > 2 &&
                smoothedFrameMs <= farSvoFinalizeMaxFrameMs &&
                schedulerPredictedFrameMs <= farSvoFinalizeMaxFrameMs &&
                !legacyDenseStreamingBlocksFarFinalize;
            const bool farFinalizeCanTrickle =
                frameCount > 20 &&
                enableSparseSurfaceAuthoritative &&
                !legacyDenseStreamingBlocksFarFinalize;
            const auto& farStatsBeforeBudget = farVoxelOctree.GetStats();
            Simulation::SparseFarUploadBudgetInput farUploadBudgetInput;
            farUploadBudgetInput.fullBudgetBytes = farSvoFinalizeUploadBudgetBytes;
            farUploadBudgetInput.trickleBudgetBytes = farSvoTrickleUploadBudgetBytes;
            farUploadBudgetInput.uploadedBytes = farStatsBeforeBudget.gpuUploadBytesUploaded;
            farUploadBudgetInput.totalBytes =
                farStatsBeforeBudget.gpuUploadBytesTotal > 0
                    ? farStatsBeforeBudget.gpuUploadBytesTotal
                    : (farVoxelOctree.IsAsyncPending() ? 1ull : 0ull);
            farUploadBudgetInput.combinedPressureMs = schedulerBudgetPressureMs;
            farUploadBudgetInput.predictedFrameMs = schedulerPredictedFrameMs;
            farUploadBudgetInput.lastUploadMs = farSvoUploadMsLastFrame;
            farUploadBudgetInput.smoothedUploadMs = farSvoUploadMsSmoothed;
            farUploadBudgetInput.targetUploadMs = farSvoUploadTargetMs;
            farUploadBudgetInput.cheapFrame = farFinalizeFrameIsCheap;
            farUploadBudgetInput.canTrickle = farFinalizeCanTrickle;
            farUploadBudgetInput.visibleMissPressure =
                sparseOwnershipPressureLevelActive > 0 || !sparseMissFeedbackPending.empty();
            const Simulation::SparseFarUploadBudgetDecision farUploadBudget =
                Simulation::SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadBudgetInput);
            const uint64_t farFinalizeBudgetThisFrame = farUploadBudget.budgetBytes;
            farSvoUploadBudgetLastFrame = farFinalizeBudgetThisFrame;
            farSvoUploadScale = farUploadBudget.uploadScale;
            farSvoUploadPressureTier = farUploadBudget.pressureTier;
            if (farFinalizeBudgetThisFrame > 0) {
                const double farUploadAccumBefore = farVoxelOctree.GetStats().gpuUploadMs;
                if (farVoxelOctree.TryFinalizeAsyncUpload(
                        device->GetDevice(),
                        renderer->GetHeapManager(),
                        farFinalizeBudgetThisFrame)) {
                    const auto& farStats = farVoxelOctree.GetStats();
                    farSvoUploadMsLastFrame = static_cast<float>(
                        std::max(0.0, farStats.gpuUploadMs - farUploadAccumBefore));
                    farSvoUploadMsSmoothed =
                        farSvoUploadMsSmoothed * 0.82f + farSvoUploadMsLastFrame * 0.18f;
                    sparseFarField.nodeSRV = farVoxelOctree.GetNodeSRV();
                    sparseFarField.pageSRV = farVoxelOctree.GetPageSRV();
                    sparseFarField.pageIndexSRV = farVoxelOctree.GetPageIndexSRV();
                    sparseFarField.nodeCount = farStats.nodeCount;
                    sparseFarField.pageCount = farStats.pageCount;
                    sparseFarField.pageIndexCount = farStats.pageIndexCount;
                    sparseFarField.pageRadius = farStats.pageRadius;
                    sparseFarField.pageSize = farStats.pageSize;
                    sparseFarField.rootMinY = farStats.rootMinY;
                    const auto farResidency =
                        Graphics::BuildFarVoxelOctreeResidencyMetadata(farStats, farVoxelOctree.IsValid());
                    sparseFarField.uploadCoverageRatio = farResidency.uploadCoverageRatio;
                    sparseFarField.pageCoverageRatio = farResidency.pageCoverageRatio;
                    sparseFarField.ready = farResidency.ready;
                    sparseFarField.enabled = true;
                    spdlog::info(
                        "Far sparse voxel octree async ready: {} pages, {} nodes, source={}, cpu={:.1f} ms, gpuUpload={:.1f} ms, uploaded={:.2f}/{:.2f} MB, coverage upload/page={:.2f}/{:.2f}",
                        farStats.pageCount,
                        farStats.nodeCount,
                        farStats.loadedFromCache ? "cache" : "build",
                        farStats.cpuBuildMs,
                        farStats.gpuUploadMs,
                        static_cast<double>(farStats.gpuUploadBytesUploaded) / (1024.0 * 1024.0),
                        static_cast<double>(farStats.gpuUploadBytesTotal) / (1024.0 * 1024.0),
                        sparseFarField.uploadCoverageRatio,
                        sparseFarField.pageCoverageRatio);
                }
                else if (farVoxelOctree.IsGpuUploadPending()) {
                    const auto& farStats = farVoxelOctree.GetStats();
                    farSvoUploadMsLastFrame = static_cast<float>(
                        std::max(0.0, farStats.gpuUploadMs - farUploadAccumBefore));
                    farSvoUploadMsSmoothed =
                        farSvoUploadMsSmoothed * 0.82f + farSvoUploadMsLastFrame * 0.18f;
                    if (farStats.gpuUploadBytesUploaded != lastFarSvoUploadProgressBytes ||
                        (frameCount % 30u) == 0u) {
                        lastFarSvoUploadProgressBytes = farStats.gpuUploadBytesUploaded;
                        spdlog::info(
                            "Far sparse voxel octree upload progress: frame={} stage={} stageUploaded={:.2f}/{:.2f} MB totalUploaded={:.2f}/{:.2f} MB budget={:.2f} MB tier={} uploadMs={:.2f}/{:.2f} accumGpu={:.2f} ms",
                            frameCount,
                            farVoxelOctree.GetGpuUploadStageName(),
                            static_cast<double>(farStats.gpuUploadStageBytesUploaded) / (1024.0 * 1024.0),
                            static_cast<double>(farStats.gpuUploadStageBytesTotal) / (1024.0 * 1024.0),
                            static_cast<double>(farStats.gpuUploadBytesUploaded) / (1024.0 * 1024.0),
                            static_cast<double>(farStats.gpuUploadBytesTotal) / (1024.0 * 1024.0),
                            static_cast<double>(farFinalizeBudgetThisFrame) / (1024.0 * 1024.0),
                            farSvoUploadPressureTier,
                            farSvoUploadMsLastFrame,
                            farSvoUploadMsSmoothed,
                            farStats.gpuUploadMs);
                    }
                }
            } else if ((frameCount % 120u) == 0u) {
                const auto& farStats = farVoxelOctree.GetStats();
                spdlog::info(
                    "Far sparse voxel octree upload deferred: frame={} smoothed={:.2f} predicted={:.2f} streaming={} source={} threshold={:.2f} stage={} uploaded={:.2f}/{:.2f} MB tier={} uploadMs={:.2f}/{:.2f}",
                    frameCount,
                    smoothedFrameMs,
                    schedulerPredictedFrameMs,
                    legacyDenseStreamingBlocksFarFinalize ? 1 : 0,
                    sourceStillFilling ? 1 : 0,
                    farSvoFinalizeMaxFrameMs,
                    farVoxelOctree.GetGpuUploadStageName(),
                    static_cast<double>(farStats.gpuUploadBytesUploaded) / (1024.0 * 1024.0),
                    static_cast<double>(farStats.gpuUploadBytesTotal) / (1024.0 * 1024.0),
                    farSvoUploadPressureTier,
                    farSvoUploadMsLastFrame,
                    farSvoUploadMsSmoothed);
            }
        } else {
            farSvoUploadBudgetLastFrame = 0;
            farSvoUploadMsLastFrame = 0.0f;
            farSvoUploadMsSmoothed *= 0.96f;
        }

        Simulation::SparseFramePressureInput framePressureInput;
        framePressureInput.smoothedFrameMs = smoothedFrameMs;
        framePressureInput.predictedFrameMs = schedulerPredictedFrameMs;
        framePressureInput.gpuFrameMs = gpuTiming.valid ? static_cast<float>(gpuTiming.frameMs) : 0.0f;
        framePressureInput.gpuRaymarchMs = gpuTiming.valid ? static_cast<float>(gpuTiming.raymarchMs) : 0.0f;
        framePressureInput.previousDebtMs = schedulerFrameDebtMs;
        const Simulation::SparseFramePressure framePressure =
            Simulation::SparseRuntimeBudgetScheduler::BuildFramePressure(framePressureInput);
        const float schedulerPressureMs = framePressure.schedulerPressureMs;
        schedulerFrameDebtMs = framePressure.debtMs;
        schedulerBudgetPressureMs = framePressure.budgetPressureMs;

        uint32_t targetCopyBudget = 40;
        uint32_t targetGenerationBudget = 3;
        float targetFarFieldQuality = 1.0f;
        if (lastRawFrameMs > 30.0f || schedulerBudgetPressureMs > 21.0f) {
            targetCopyBudget = 8;
            targetGenerationBudget = 0;
            targetFarFieldQuality = 0.45f;
        } else if (lastRawFrameMs > 24.0f || schedulerBudgetPressureMs > 19.0f) {
            targetCopyBudget = 12;
            targetGenerationBudget = 1;
            targetFarFieldQuality = 0.60f;
        } else if (schedulerBudgetPressureMs > 18.0f) {
            targetCopyBudget = 16;
            targetGenerationBudget = 1;
            targetFarFieldQuality = 0.75f;
        } else if (schedulerBudgetPressureMs > 17.0f) {
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
            const auto sparseMidResidencyForBudget =
                Simulation::BuildClipmapResidencyMetadata(sparseClipmapTileCache.GetStats());
            Simulation::SparseBackgroundRenderBudgetInput backgroundBudgetInput;
            backgroundBudgetInput.combinedPressureMs = schedulerBudgetPressureMs;
            backgroundBudgetInput.gpuRaymarchMs =
                gpuTiming.valid ? static_cast<float>(gpuTiming.raymarchMs) : 0.0f;
            backgroundBudgetInput.frameBudgetMs = 16.67f;
            backgroundBudgetInput.previousRaymarchScale = sparseRaymarchBudgetScale;
            backgroundBudgetInput.previousRenderQuality = currentRenderQuality;
            uint32_t backgroundOwnershipPressureLevel = sparseOwnershipPressureLevelActive;
            const uint32_t feedbackScaleForBackground = std::max(1u, sparseMissFeedbackMaxRecords);
            if (sparseMissFeedbackPending.size() >= feedbackScaleForBackground) {
                backgroundOwnershipPressureLevel = std::max(backgroundOwnershipPressureLevel, 3u);
            } else if (sparseMissFeedbackPending.size() >= std::max<size_t>(1u, feedbackScaleForBackground / 2u)) {
                backgroundOwnershipPressureLevel = std::max(backgroundOwnershipPressureLevel, 2u);
            } else if (!sparseMissFeedbackPending.empty()) {
                backgroundOwnershipPressureLevel = std::max(backgroundOwnershipPressureLevel, 1u);
            }
            backgroundBudgetInput.ownershipPressureLevel =
                std::min<uint32_t>(3u, backgroundOwnershipPressureLevel);
            backgroundBudgetInput.midHeightCoverage =
                sparseMidResidencyForBudget.heightCoverageRatio;
            backgroundBudgetInput.midVoxelCoverage =
                sparseMidResidencyForBudget.voxelCoverageRatio;
            backgroundBudgetInput.midVoxelPixelShare =
                sparseOwnershipMidVoxelPixelShareLastRetire;
            backgroundBudgetInput.farHeightPixelShare =
                sparseOwnershipFarHeightPixelShareLastRetire;
            backgroundBudgetInput.skyPixelShare =
                sparseOwnershipSkyPixelShareLastRetire;
            backgroundBudgetInput.backgroundPixelShare =
                sparseOwnershipBackgroundPixelShareLastRetire;
            backgroundBudgetInput.farSvoReady =
                sparseFarField.enabled && sparseFarField.ready;
            const Simulation::SparseBackgroundRenderBudgetDecision backgroundBudget =
                Simulation::SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(
                    backgroundBudgetInput);

            sparseRaymarchBudgetScale = std::clamp(
                std::min(backgroundBudget.raymarchScale, sparseRaymarchMaxScale),
                0.20f,
                sparseRaymarchMaxScale);
            sparseBackgroundQualityTier = backgroundBudget.qualityTier;

            currentRaymarchMaxDistance = std::max(
                32.0f,
                sparseRaymarchDefaultDistance * sparseRaymarchBudgetScale);
            currentRaymarchMaxSteps = std::max<uint32_t>(
                4u,
                static_cast<uint32_t>(std::floor(
                    static_cast<float>(sparseRaymarchDefaultSteps) *
                    sparseRaymarchBudgetScale +
                    0.5f)));
            currentRenderQuality = backgroundBudget.renderQuality;
            targetFarFieldQuality = std::min(
                targetFarFieldQuality,
                backgroundBudget.farFieldQuality);
        } else {
            currentRaymarchMaxDistance = denseRaymarchDefaultDistance;
            currentRaymarchMaxSteps = denseRaymarchDefaultSteps;
            currentRenderQuality = 1.0f;
            sparseBackgroundQualityTier = 0;
        }
        currentFarFieldQuality += (targetFarFieldQuality - currentFarFieldQuality) * 0.08f;
        voxelWorld->SetMaxChunkCopiesPerFrame(currentCopyBudget);
        const uint32_t trailingGenerationBudget = currentGenerationBudget;
        voxelWorld->SetChunkGenerationBudget(0);
        const auto computeSparseEffectiveOwnershipPressureLevel = [&]() -> uint32_t {
            uint32_t level = sparseOwnershipPressureLevelActive;
            const uint32_t feedbackScale = std::max(1u, sparseMissFeedbackMaxRecords);
            uint32_t feedbackLevel = 0;
            if (sparseMissFeedbackPending.size() >= feedbackScale) {
                feedbackLevel = 3;
            } else if (sparseMissFeedbackPending.size() >= std::max<size_t>(1u, feedbackScale / 2u)) {
                feedbackLevel = 2;
            } else if (!sparseMissFeedbackPending.empty()) {
                feedbackLevel = 1;
            }
            level = std::max(level, feedbackLevel);
            return std::min<uint32_t>(3u, level);
        };

        sparseRuntimeBudgetScale = 1.0f;
        sparseProtectedRuntimeBudgetScale = 1.0f;
        sparseBackgroundRuntimeBudgetScale = 1.0f;
        sparseRuntimePressureClass = Simulation::SparseRuntimePressureClass::Idle;
        sparseProtectedBacklogLastFrame = 0;
        sparseTrimSpeculativeFirstLastFrame = 0;
        if (sparseBackendRequested && sparseVoxelWorldReady) {
            const auto& sparseStatsForScheduler = sparseVoxelWorld.GetStats();
            const auto& sparseGpuStatsForScheduler = sparseGpuResources.GetStats();
            const uint32_t sparseSchedulerFrameIndex = static_cast<uint32_t>(std::min<uint64_t>(
                frameCount,
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
            const uint64_t sparseSchedulerCompletedFence =
                commandQueue ? commandQueue->GetLastCompletedFenceValue() : 0u;
            const Simulation::SparsePagePublishQueueStats sparsePublishStatsForScheduler =
                sparsePagePublishQueue.GetStats(
                    sparseSchedulerFrameIndex,
                    sparseSchedulerCompletedFence);
            Simulation::SparseRuntimeBudgetInput sparseBudgetInput;
            sparseBudgetInput.lastRawFrameMs = lastRawFrameMs;
            sparseBudgetInput.combinedSchedulerPressureMs = schedulerBudgetPressureMs;
            sparseBudgetInput.hasQueueBacklog =
                sparseStatsForScheduler.generationQueuedBricks > 0 ||
                sparseStatsForScheduler.uploadQueuedBricks > 0 ||
                !sparsePagePublishQueue.Empty() ||
                !sparseMissFeedbackPending.empty() ||
                sparseClipmapTileCache.GetStats().queuedTiles > 0 ||
                sparseClipmapTileCache.GetStats().queuedVoxelBricks > 0;
            sparseBudgetInput.uploadRingOverflow =
                sparseGpuStatsForScheduler.uploadRingOverflowLastFrame ||
                sparseUploadRingBudgetDefersLastFrame > 0;
            sparseBudgetInput.stagedBytesLastFrame = sparseGpuStatsForScheduler.stagedBytesLastFrame;
            sparseBudgetInput.uploadRingBytes = sparseGpuStatsForScheduler.uploadRingBytes;
            sparseBudgetInput.maxBrickPages = sparseWorldConfig.maxBrickPages;
            sparseBudgetInput.urgentQueuedBricks =
                sparseStatsForScheduler.generationQueuedCollisionBricks +
                sparseStatsForScheduler.generationQueuedEditedBricks +
                sparseStatsForScheduler.uploadQueuedCollisionBricks +
                sparseStatsForScheduler.uploadQueuedEditedBricks +
                sparseStatsForScheduler.surfaceQueuedCollisionBricks +
                sparseStatsForScheduler.surfaceQueuedEditedBricks;
            sparseBudgetInput.visibleQueuedBricks =
                sparseStatsForScheduler.generationQueuedVisibleBricks +
                sparseStatsForScheduler.uploadQueuedVisibleBricks +
                sparseStatsForScheduler.surfaceQueuedVisibleBricks;
            sparseBudgetInput.speculativeQueuedBricks =
                sparseStatsForScheduler.generationQueuedSpeculativeBricks +
                sparseStatsForScheduler.uploadQueuedSpeculativeBricks +
                sparseStatsForScheduler.surfaceQueuedSpeculativeBricks;
            sparseBudgetInput.surfaceQueuedBricks =
                sparseStatsForScheduler.surfaceExtractionQueuedBricks;
            sparseBudgetInput.physicsHotCandidateBricks =
                sparseStatsForScheduler.physicsHotCandidateBricks;
            sparseBudgetInput.pagePublishReadyQueued =
                static_cast<uint32_t>(std::min<size_t>(
                    sparsePublishStatsForScheduler.ready,
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            sparseBudgetInput.pagePublishWaitingFrame =
                static_cast<uint32_t>(std::min<size_t>(
                    sparsePublishStatsForScheduler.waitingFrame,
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            sparseBudgetInput.pagePublishWaitingFence =
                static_cast<uint32_t>(std::min<size_t>(
                    sparsePublishStatsForScheduler.waitingFence,
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            sparseBudgetInput.pagePublishEditedQueued =
                static_cast<uint32_t>(std::min<size_t>(
                    sparsePublishStatsForScheduler.edited,
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            sparseBudgetInput.pagePublishMaxReadyFrameLag =
                sparsePublishStatsForScheduler.maxReadyFrameLag;
            sparseBudgetInput.visibleMissPressure =
                sparseResidencyCatchupFramesRemaining > 0 ||
                computeSparseEffectiveOwnershipPressureLevel() > 0;
            sparseBudgetInput.ownershipPressureLevel =
                computeSparseEffectiveOwnershipPressureLevel();
            const Simulation::SparseRuntimeBudgetDecision sparseBudgetDecision =
                Simulation::SparseRuntimeBudgetScheduler::Evaluate(sparseBudgetInput);
            sparseRuntimeBudgetScale = sparseBudgetDecision.scale;
            sparseProtectedRuntimeBudgetScale = sparseBudgetDecision.protectedScale;
            sparseBackgroundRuntimeBudgetScale = sparseBudgetDecision.backgroundScale;
            sparseRuntimePressureClass = sparseBudgetDecision.pressureClass;
            sparseProtectedBacklogLastFrame = sparseBudgetDecision.hasProtectedBacklog ? 1u : 0u;
            sparseTrimSpeculativeFirstLastFrame = sparseBudgetDecision.trimSpeculativeFirst ? 1u : 0u;
        }

        auto evaluateSparseBudgetFromStats =
            [&](const Simulation::SparseVoxelWorldStats& sparseStatsForScheduler) {
                const auto& sparseGpuStatsForScheduler = sparseGpuResources.GetStats();
                const uint32_t sparseSchedulerFrameIndex = static_cast<uint32_t>(std::min<uint64_t>(
                    frameCount,
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
                const uint64_t sparseSchedulerCompletedFence =
                    commandQueue ? commandQueue->GetLastCompletedFenceValue() : 0u;
                const Simulation::SparsePagePublishQueueStats sparsePublishStatsForScheduler =
                    sparsePagePublishQueue.GetStats(
                        sparseSchedulerFrameIndex,
                        sparseSchedulerCompletedFence);
                Simulation::SparseRuntimeBudgetInput sparseBudgetInput;
                sparseBudgetInput.lastRawFrameMs = lastRawFrameMs;
                sparseBudgetInput.combinedSchedulerPressureMs = schedulerBudgetPressureMs;
                sparseBudgetInput.hasQueueBacklog =
                    sparseStatsForScheduler.generationQueuedBricks > 0 ||
                    sparseStatsForScheduler.uploadQueuedBricks > 0 ||
                    !sparsePagePublishQueue.Empty() ||
                    !sparseMissFeedbackPending.empty() ||
                    sparseClipmapTileCache.GetStats().queuedTiles > 0 ||
                    sparseClipmapTileCache.GetStats().queuedVoxelBricks > 0;
                sparseBudgetInput.uploadRingOverflow =
                    sparseGpuStatsForScheduler.uploadRingOverflowLastFrame ||
                    sparseUploadRingBudgetDefersLastFrame > 0;
                sparseBudgetInput.stagedBytesLastFrame = sparseGpuStatsForScheduler.stagedBytesLastFrame;
                sparseBudgetInput.uploadRingBytes = sparseGpuStatsForScheduler.uploadRingBytes;
                sparseBudgetInput.maxBrickPages = sparseWorldConfig.maxBrickPages;
                sparseBudgetInput.urgentQueuedBricks =
                    sparseStatsForScheduler.generationQueuedCollisionBricks +
                    sparseStatsForScheduler.generationQueuedEditedBricks +
                    sparseStatsForScheduler.uploadQueuedCollisionBricks +
                    sparseStatsForScheduler.uploadQueuedEditedBricks +
                    sparseStatsForScheduler.surfaceQueuedCollisionBricks +
                    sparseStatsForScheduler.surfaceQueuedEditedBricks;
                sparseBudgetInput.visibleQueuedBricks =
                    sparseStatsForScheduler.generationQueuedVisibleBricks +
                    sparseStatsForScheduler.uploadQueuedVisibleBricks +
                    sparseStatsForScheduler.surfaceQueuedVisibleBricks;
                sparseBudgetInput.speculativeQueuedBricks =
                    sparseStatsForScheduler.generationQueuedSpeculativeBricks +
                    sparseStatsForScheduler.uploadQueuedSpeculativeBricks +
                    sparseStatsForScheduler.surfaceQueuedSpeculativeBricks;
                sparseBudgetInput.surfaceQueuedBricks =
                    sparseStatsForScheduler.surfaceExtractionQueuedBricks;
                sparseBudgetInput.physicsHotCandidateBricks =
                    sparseStatsForScheduler.physicsHotCandidateBricks;
                sparseBudgetInput.pagePublishReadyQueued =
                    static_cast<uint32_t>(std::min<size_t>(
                        sparsePublishStatsForScheduler.ready,
                        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                sparseBudgetInput.pagePublishWaitingFrame =
                    static_cast<uint32_t>(std::min<size_t>(
                        sparsePublishStatsForScheduler.waitingFrame,
                        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                sparseBudgetInput.pagePublishWaitingFence =
                    static_cast<uint32_t>(std::min<size_t>(
                        sparsePublishStatsForScheduler.waitingFence,
                        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                sparseBudgetInput.pagePublishEditedQueued =
                    static_cast<uint32_t>(std::min<size_t>(
                        sparsePublishStatsForScheduler.edited,
                        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                sparseBudgetInput.pagePublishMaxReadyFrameLag =
                    sparsePublishStatsForScheduler.maxReadyFrameLag;
                sparseBudgetInput.visibleMissPressure =
                    sparseResidencyCatchupFramesRemaining > 0 ||
                    computeSparseEffectiveOwnershipPressureLevel() > 0;
                sparseBudgetInput.ownershipPressureLevel =
                    computeSparseEffectiveOwnershipPressureLevel();
                return Simulation::SparseRuntimeBudgetScheduler::Evaluate(sparseBudgetInput);
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

        const bool sparseStressCameraActive =
            enableSparseStressCamera && !enableBoundaryTest && !pauseMenu.IsVisible();
        const bool jumpPressed = gameplayInputEnabled && inputManager.IsActionPressed(Input::KeyAction::CameraUp);
        const bool flightTogglePressed = gameplayInputEnabled && inputManager.IsActionDoubleClicked(Input::KeyAction::CameraUp);

        // Mouse look - update camera rotation
        glm::vec2 mouseDelta = gameplayInputEnabled && !sparseStressCameraActive
            ? inputManager.GetMouseDelta()
            : glm::vec2(0.0f);
        cameraYaw += mouseDelta.x * mouseSensitivity;  // Inverted from - to + for correct left/right
        cameraPitch -= mouseDelta.y * mouseSensitivity;

        // Clamp pitch to prevent flipping
        const float maxPitch = 1.57f;  // ~90 degrees
        cameraPitch = glm::clamp(cameraPitch, -maxPitch, maxPitch);
        if (sparseStressCameraActive) {
            flightMode = true;
            terrainReady = true;
            cameraVelocityY = 0.0f;
            sparseStressCameraElapsedSeconds += dt;

            const float angularSpeed = glm::radians(std::max(1.0f, sparseStressCameraSpeed));
            const float angle = sparseStressCameraElapsedSeconds * angularSpeed;
            const float radius = std::max(32.0f, sparseStressCameraRadius);
            const float verticalOffset =
                std::sin(angle * 0.43f) * sparseStressCameraHeight +
                std::sin(angle * 1.37f) * sparseStressCameraHeight * 0.25f;
            cameraPos = sparseStressCameraOrigin + glm::vec3(
                std::cos(angle) * radius,
                sparseStressCameraBaseHeight + verticalOffset,
                std::sin(angle) * radius);

            const glm::vec3 lookTarget =
                sparseStressCameraOrigin +
                glm::vec3(
                    std::sin(angle * 0.29f) * radius * 0.18f,
                    std::sin(angle * 0.61f) * sparseStressCameraHeight * 0.20f,
                    std::cos(angle * 0.37f) * radius * 0.18f);
            const glm::vec3 lookDir = glm::normalize(lookTarget - cameraPos);
            cameraYaw = std::atan2(lookDir.z, lookDir.x);
            cameraPitch = std::asin(glm::clamp(lookDir.y, -0.98f, 0.98f));
            if (frameCount % 60 == 0) {
                spdlog::info(
                    "SPARSE_STRESS_CAMERA t={:.2f} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f} pitch={:.2f}",
                    sparseStressCameraElapsedSeconds,
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    cameraYaw,
                    cameraPitch);
            }
        }

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
        const glm::vec3 cameraPosBeforeInputMovement = cameraPos;

        // Calculate horizontal movement direction (forward/right with Y removed)
        glm::vec3 horizontalForward(std::cos(cameraYaw), 0.0f, std::sin(cameraYaw));
        glm::vec3 horizontalRight(-std::sin(cameraYaw), 0.0f, std::cos(cameraYaw));

        glm::vec3 moveDirection(0.0f);

        // WASD for horizontal movement only
        if (gameplayInputEnabled && !sparseStressCameraActive && inputManager.IsActionDown(Input::KeyAction::CameraForward)) {
            moveDirection += horizontalForward;
        }
        if (gameplayInputEnabled && !sparseStressCameraActive && inputManager.IsActionDown(Input::KeyAction::CameraBackward)) {
            moveDirection -= horizontalForward;
        }
        if (gameplayInputEnabled && !sparseStressCameraActive && inputManager.IsActionDown(Input::KeyAction::CameraLeft)) {
            moveDirection -= horizontalRight;
        }
        if (gameplayInputEnabled && !sparseStressCameraActive && inputManager.IsActionDown(Input::KeyAction::CameraRight)) {
            moveDirection += horizontalRight;
        }
        if (glm::length(moveDirection) > 0.001f) {
            cameraPos += glm::normalize(moveDirection) * moveSpeed;
        }

        if (gameplayInputEnabled && flightMode && !enableBoundaryTest && !sparseStressCameraActive) {
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
            sparseVoxelWorld.BeginFrame();
            sparseResidencyCatchupLastFrame = sparseResidencyCatchupFramesRemaining > 0 ? 1u : 0u;
            const Simulation::BrickCoord sparseCenter =
                Simulation::BrickCoord::FromWorldVoxel(
                    static_cast<int32_t>(std::floor(cameraPos.x)),
                    static_cast<int32_t>(std::floor(cameraPos.y - playerHeight)),
                    static_cast<int32_t>(std::floor(cameraPos.z)));

            uint32_t sparseNewRequestsThisFrame = 0;
            uint32_t sparseSpeculativeRequestsThisFrame = 0;
            uint32_t sparseVisibleRequestsThisFrame = 0;
            uint32_t sparseCollisionRequestsThisFrame = 0;
            uint32_t sparseRequestFreePageSkipsThisFrame = 0;
            uint32_t sparseRequestClassBudgetSkipsThisFrame = 0;
            uint32_t sparseRequestTotalBudgetSkipsThisFrame = 0;
            uint32_t sparseRequestRejectedSkipsThisFrame = 0;
            uint32_t sparseRequestKnownEmptySkipsThisFrame = 0;
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
            sparseBrushIntentActiveLastFrame = 0;
            sparseBrushCollisionReserveLastFrame = 0;
            sparseBrushCollisionMaxLastFrame = 0;
            const glm::vec3 sparseAdmissionCameraDelta = cameraPos - lastSparseResidencyCameraWorld;
            const float sparseAdmissionSpeed =
                dt > 0.001f ? glm::length(sparseAdmissionCameraDelta) / dt : 0.0f;
            uint32_t sparseFastRequestScaleThisFrame = 1;
            if (sparseFastRequestSpeed > 0u && sparseAdmissionSpeed > static_cast<float>(sparseFastRequestSpeed)) {
                sparseFastRequestScaleThisFrame = std::min<uint32_t>(
                    sparseFastRequestMaxScale,
                    1u + static_cast<uint32_t>(
                        std::floor(sparseAdmissionSpeed / static_cast<float>(sparseFastRequestSpeed))));
            }
            if (flightMode && sparseAdmissionSpeed > 24.0f) {
                sparseFastRequestScaleThisFrame = std::max<uint32_t>(sparseFastRequestScaleThisFrame, 2u);
            }
            sparseFastRequestScaleLastFrame = sparseFastRequestScaleThisFrame;
            Simulation::SparseRuntimeBudgetDecision sparseAdmissionRuntimeDecision{};
            sparseAdmissionRuntimeDecision.scale = sparseRuntimeBudgetScale;
            sparseAdmissionRuntimeDecision.protectedScale = sparseProtectedRuntimeBudgetScale;
            sparseAdmissionRuntimeDecision.backgroundScale = sparseBackgroundRuntimeBudgetScale;
            const uint32_t sparseEffectiveOwnershipPressureLevelThisFrame =
                computeSparseEffectiveOwnershipPressureLevel();
            const bool sparseResidencyCatchupActive =
                sparseResidencyCatchupFramesRemaining > 0 ||
                sparseEffectiveOwnershipPressureLevelThisFrame > 0;
            const uint32_t sparseOwnershipCatchupLevel =
                sparseResidencyCatchupActive
                    ? std::clamp<uint32_t>(sparseEffectiveOwnershipPressureLevelThisFrame, 1u, 3u)
                    : 0u;
            const uint32_t sparseOwnershipBudgetMultiplier =
                sparseOwnershipCatchupLevel == 0u ? 1u : (sparseOwnershipCatchupLevel + 1u);
            const uint32_t sparseVisibleAdmissionBudget =
                std::max(
                    sparseVisibleRequestBudget * sparseFastRequestScaleThisFrame,
                    sparseResidencyCatchupActive
                        ? sparseCatchupVisibleRequestBudget * sparseOwnershipBudgetMultiplier
                        : 0u);
            const uint32_t sparseCollisionAdmissionBudget =
                std::max(
                    sparseCollisionRequestBudget * sparseFastRequestScaleThisFrame,
                    sparseResidencyCatchupActive
                        ? sparseCatchupCollisionRequestBudget * sparseOwnershipBudgetMultiplier
                        : 0u);
            const uint32_t sparseSpeculativeAdmissionBudget =
                sparseResidencyCatchupActive
                    ? std::max(1u, sparseSpeculativeRequestBudget / 2u)
                    : sparseSpeculativeRequestBudget * std::max(1u, sparseFastRequestScaleThisFrame / 2u);
            const uint32_t sparseTotalAdmissionBudget =
                sparseTotalRequestBudget +
                (sparseFastRequestScaleThisFrame - 1u) *
                    (sparseVisibleRequestBudget + sparseCollisionRequestBudget) +
                (sparseResidencyCatchupActive
                    ? (sparseCatchupVisibleRequestBudget + sparseCatchupCollisionRequestBudget) *
                        sparseOwnershipBudgetMultiplier
                    : 0u);
            const Simulation::SparseRequestBudgetDecision sparseRequestBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildRequestBudgets(
                    sparseSpeculativeAdmissionBudget,
                    sparseVisibleAdmissionBudget,
                    sparseCollisionAdmissionBudget,
                    sparseTotalAdmissionBudget,
                    sparseAdmissionRuntimeDecision);
            uint32_t sparseProtectedRequestOverageThisFrame = 0;
            if (sparsePressureTrimBudget > 0 &&
                (sparseVoxelWorld.GetStats().freePages <= sparseMinFreePages ||
                 sparseVoxelWorld.GenerationQueueSize() >= sparseSpeculativeBackpressureGenQueue ||
                 !sparseMissFeedbackPending.empty())) {
                sparsePressureTrimLastFrame += sparseVoxelWorld.TrimQueuedBackgroundBricks(
                    sparseCenter,
                    sparseTrimRadiusXz,
                    sparseTrimRadiusY,
                    sparsePressureTrimBudget,
                    sparseResidencyFrame);
                if (sparseVoxelWorld.GetStats().freePages <= sparseMinFreePages ||
                    !sparseMissFeedbackPending.empty()) {
                    sparsePressureTrimLastFrame += sparseTrimSpeculativeFirstLastFrame
                        ? sparseVoxelWorld.TrimBackgroundResidentBricks(
                        sparseCenter,
                        sparseTrimRadiusXz,
                        sparseTrimRadiusY,
                        sparsePressureTrimBudget,
                        sparseResidencyFrame)
                        : sparseVoxelWorld.TrimResidentBricks(
                        sparseCenter,
                        sparseTrimRadiusXz,
                        sparseTrimRadiusY,
                        sparsePressureTrimBudget);
                }
            }
            sparseVoxelWorld.SetStatsRefreshDeferred(true);
            auto requestSparseBrick = [&](
                const Simulation::BrickCoord& coord,
                bool urgent,
                Simulation::SparseResidencyClass residencyClass = Simulation::SparseResidencyClass::Speculative) {
                if (sparseVoxelWorld.GetPool().TryGetPage(coord)) {
                    sparseVoxelWorld.TouchResidencyClass(coord, residencyClass, sparseResidencyFrame);
                    return true;
                }
                const bool protectedRequestClass =
                    residencyClass == Simulation::SparseResidencyClass::Visible ||
                    residencyClass == Simulation::SparseResidencyClass::Collision ||
                    residencyClass == Simulation::SparseResidencyClass::Edited;
                const uint32_t minFreePages = urgent ? std::min(4u, sparseMinFreePages) : sparseMinFreePages;
                const uint32_t freePages = sparseVoxelWorld.GetPool().FreePageCount();
                if (residencyClass == Simulation::SparseResidencyClass::Speculative &&
                    (sparseVoxelWorld.GenerationQueueSize() >= sparseSpeculativeBackpressureGenQueue ||
                     sparseMissFeedbackPending.size() >= sparseSpeculativeBackpressureMissPending ||
                     freePages <= sparseMinFreePages)) {
                    ++sparseSpeculativeBackpressureSkipsLastFrame;
                    return false;
                }
                uint32_t* classCounter = &sparseSpeculativeRequestsThisFrame;
                uint32_t classBudget = sparseRequestBudgetThisFrame.speculative;
                if (residencyClass == Simulation::SparseResidencyClass::Visible) {
                    classCounter = &sparseVisibleRequestsThisFrame;
                    classBudget = sparseRequestBudgetThisFrame.visible;
                } else if (protectedRequestClass) {
                    classCounter = &sparseCollisionRequestsThisFrame;
                    classBudget = sparseRequestBudgetThisFrame.collision;
                }
                bool madeReplacementRoom = false;
                if (freePages <= minFreePages &&
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
                const uint32_t freePagesAfterReplacement = sparseVoxelWorld.GetPool().FreePageCount();
                const uint32_t effectiveTotalRequestBudget = protectedRequestClass
                    ? sparseRequestBudgetThisFrame.protectedHardTotal
                    : sparseRequestBudgetThisFrame.total;
                if (freePagesAfterReplacement == 0 ||
                    (freePagesAfterReplacement <= minFreePages && !madeReplacementRoom)) {
                    ++sparseRequestFreePageSkipsThisFrame;
                    return false;
                }
                if (sparseNewRequestsThisFrame >= effectiveTotalRequestBudget) {
                    ++sparseRequestTotalBudgetSkipsThisFrame;
                    return false;
                }
                if (*classCounter >= classBudget) {
                    ++sparseRequestClassBudgetSkipsThisFrame;
                    return false;
                }
                const auto requestResult = sparseVoxelWorld.RequestBrickDetailed(coord);
                if (requestResult == Simulation::SparseBrickRequestResult::Rejected) {
                    ++sparseRequestRejectedSkipsThisFrame;
                    return false;
                }
                if (requestResult == Simulation::SparseBrickRequestResult::SkippedKnownEmpty) {
                    ++sparseRequestKnownEmptySkipsThisFrame;
                    return false;
                }
                sparseVoxelWorld.TouchResidencyClass(coord, residencyClass, sparseResidencyFrame);
                if (protectedRequestClass &&
                    sparseNewRequestsThisFrame >= sparseRequestBudgetThisFrame.total) {
                    ++sparseProtectedRequestOverageThisFrame;
                }
                ++sparseNewRequestsThisFrame;
                ++(*classCounter);
                return true;
            };

            const uint32_t sparseBrushFeedbackMissingRequestBudget =
                std::min<uint32_t>(16u, std::max(1u, sparseRequestBudgetThisFrame.collision));
            for (uint32_t i = 0;
                 i < sparseBrushFeedbackMissingRequestBudget &&
                 !sparseBrushFeedbackMissingResidentRequestQueue.empty();
                 ++i) {
                const Simulation::BrickCoord missingBrick =
                    sparseBrushFeedbackMissingResidentRequestQueue.front();
                sparseBrushFeedbackMissingResidentRequestQueue.pop_front();
                sparseBrushFeedbackMissingResidentRequestSet.erase(missingBrick);
                if (requestSparseBrick(
                        missingBrick,
                        true,
                        Simulation::SparseResidencyClass::Edited)) {
                    ++sparseBrushFeedbackMissingResidentRequestsLastFrame;
                } else {
                    sparseVoxelWorld.MarkResidencyClass(
                        missingBrick,
                        Simulation::SparseResidencyClass::Edited);
                }
            }

            if (enableSparseSurfaceDiagnosticSeed && !sparseSurfaceDiagnosticSeedQueued) {
                const glm::vec3 seedCenterWorld =
                    cameraPos + cameraForward * 24.0f + glm::vec3(0.0f, 3.0f, 0.0f);
                const int32_t centerX = static_cast<int32_t>(std::floor(seedCenterWorld.x));
                const int32_t centerY = static_cast<int32_t>(std::floor(seedCenterWorld.y));
                const int32_t centerZ = static_cast<int32_t>(std::floor(seedCenterWorld.z));
                const uint32_t diagnosticVoxel =
                    Utils::PackVoxel(Utils::Material::Stone, 0, 0, Utils::StateFlags::IsStatic);
                std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> seededBricks;
                uint32_t seededVoxels = 0;
                for (int32_t dz = -2; dz <= 2; ++dz) {
                    for (int32_t dy = -4; dy <= 4; ++dy) {
                        for (int32_t dx = -4; dx <= 4; ++dx) {
                            const int32_t worldX = centerX + dx;
                            const int32_t worldY = centerY + dy;
                            const int32_t worldZ = centerZ + dz;
                            sparseVoxelWorld.SetEditedVoxel(worldX, worldY, worldZ, diagnosticVoxel);
                            seededBricks.insert(Simulation::BrickCoord::FromWorldVoxel(worldX, worldY, worldZ));
                            ++seededVoxels;
                        }
                    }
                }
                uint32_t requestedSeedBricks = 0;
                for (const Simulation::BrickCoord& coord : seededBricks) {
                    if (requestSparseBrick(coord, true, Simulation::SparseResidencyClass::Edited)) {
                        ++requestedSeedBricks;
                    } else {
                        sparseVoxelWorld.MarkResidencyClass(coord, Simulation::SparseResidencyClass::Edited);
                    }
                }
                sparseSurfaceDiagnosticSeedQueued = true;
                sparseSurfaceDiagnosticSeededVoxels = seededVoxels;
                sparseSurfaceDiagnosticSeededBricks = static_cast<uint32_t>(seededBricks.size());
                spdlog::info(
                    "Sparse surface diagnostic seed queued center={} {} {} voxels={} bricks={} requested={}",
                    centerX,
                    centerY,
                    centerZ,
                    seededVoxels,
                    seededBricks.size(),
                    requestedSeedBricks);
            }
            if (enableSparseGpuRaycastDiagnosticSeed && !sparseGpuRaycastDiagnosticSeedQueued) {
                const glm::vec3 seedCenterWorld = cameraPos + cameraForward * 48.0f;
                const int32_t centerX = static_cast<int32_t>(std::floor(seedCenterWorld.x));
                const int32_t centerY = static_cast<int32_t>(std::floor(seedCenterWorld.y));
                const int32_t centerZ = static_cast<int32_t>(std::floor(seedCenterWorld.z));
                const uint32_t diagnosticVoxel =
                    Utils::PackVoxel(Utils::Material::Stone, 0, 0, Utils::StateFlags::IsStatic);
                std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> seededBricks;
                uint32_t seededVoxels = 0;
                for (int32_t dz = -3; dz <= 3; ++dz) {
                    for (int32_t dy = -3; dy <= 3; ++dy) {
                        for (int32_t dx = -3; dx <= 3; ++dx) {
                            const int32_t worldX = centerX + dx;
                            const int32_t worldY = centerY + dy;
                            const int32_t worldZ = centerZ + dz;
                            sparseVoxelWorld.SetEditedVoxel(worldX, worldY, worldZ, diagnosticVoxel);
                            seededBricks.insert(Simulation::BrickCoord::FromWorldVoxel(worldX, worldY, worldZ));
                            ++seededVoxels;
                        }
                    }
                }
                uint32_t requestedSeedBricks = 0;
                for (const Simulation::BrickCoord& coord : seededBricks) {
                    if (requestSparseBrick(coord, true, Simulation::SparseResidencyClass::Edited)) {
                        ++requestedSeedBricks;
                    } else {
                        sparseVoxelWorld.MarkResidencyClass(coord, Simulation::SparseResidencyClass::Edited);
                    }
                }
                sparseGpuRaycastDiagnosticSeedQueued = true;
                sparseGpuRaycastDiagnosticSeededVoxels = seededVoxels;
                sparseGpuRaycastDiagnosticSeededBricks = static_cast<uint32_t>(seededBricks.size());
                spdlog::info(
                    "Sparse GPU raycast diagnostic seed queued center={} {} {} voxels={} bricks={} requested={}",
                    centerX,
                    centerY,
                    centerZ,
                    seededVoxels,
                    seededBricks.size(),
                    requestedSeedBricks);
            }

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

            if (enableSparseHierarchicalRequests) {
                const glm::vec3 sparseCameraDelta = cameraPos - lastSparseResidencyCameraWorld;
                const glm::vec3 sparseCameraVelocity =
                    dt > 0.001f ? sparseCameraDelta / dt : glm::vec3(0.0f);

                Simulation::SparseHierarchicalRequestConfig hierarchy{};
                hierarchy.center = sparseCenter;
                hierarchy.cameraX = cameraPos.x;
                hierarchy.cameraY = cameraPos.y;
                hierarchy.cameraZ = cameraPos.z;
                hierarchy.velocityX = sparseCameraVelocity.x;
                hierarchy.velocityY = sparseCameraVelocity.y;
                hierarchy.velocityZ = sparseCameraVelocity.z;
                hierarchy.forwardX = cameraForward.x;
                hierarchy.forwardY = cameraForward.y;
                hierarchy.forwardZ = cameraForward.z;
                hierarchy.rightX = cameraRight.x;
                hierarchy.rightY = cameraRight.y;
                hierarchy.rightZ = cameraRight.z;
                hierarchy.upX = cameraUp.x;
                hierarchy.upY = cameraUp.y;
                hierarchy.upZ = cameraUp.z;
                hierarchy.verticalFovRadians = fov;
                hierarchy.aspectRatio = aspectRatio;
                hierarchy.visibleDistance = static_cast<float>(std::max(64u, sparseRayPrefetchDistance));
                hierarchy.speculativeDistance = static_cast<float>(std::max(
                    sparseRayPrefetchDistance * 2u,
                    sparseRayPrefetchDistance + sparseRayPrefetchStride));
                hierarchy.stepDistance = static_cast<float>(sparseRayPrefetchStride);
                const uint32_t sparseFastRadiusBonus =
                    sparseFastRequestScaleThisFrame > 1u ? sparseFastRequestScaleThisFrame - 1u : 0u;
                hierarchy.visibleDistance += static_cast<float>(sparseFastRadiusBonus * sparseRayPrefetchStride * 2u);
                hierarchy.speculativeDistance += static_cast<float>(sparseFastRadiusBonus * sparseRayPrefetchStride * 4u);
                hierarchy.predictionSeconds = std::max(
                    static_cast<float>(sparsePredictivePrefetchMs) * 0.001f,
                    sparseFastRequestScaleThisFrame > 1u ? 0.45f : 0.0f);
                hierarchy.collisionBodyHeight = playerHeight;
                hierarchy.collisionBodyRadius = playerRadius;
                hierarchy.collisionStepHeight = stepHeight;
                hierarchy.collisionSupportDrop = std::max(4.0f, std::abs(cameraVelocityY) * 0.20f + 2.0f);
                hierarchy.collisionRadiusXz = sparseCollisionShellRadiusXz;
                hierarchy.collisionRadiusY = sparseCollisionShellRadiusY;
                hierarchy.collisionPredictionBricks = 2u + std::min<uint32_t>(3u, sparseFastRadiusBonus);
                hierarchy.collisionMaxIntentSamples = 16u + sparseFastRadiusBonus * 8u;
                hierarchy.nearVisibleRadiusXz =
                    std::max(2u + std::min<uint32_t>(3u, sparseFastRadiusBonus),
                        sparseCollisionShellRadiusXz + 1u);
                hierarchy.nearVisibleRadiusY =
                    std::max(1u + std::min<uint32_t>(2u, sparseFastRadiusBonus / 2u),
                        sparseCollisionShellRadiusY);
                hierarchy.maxNearVisibleRequests = std::max<uint32_t>(
                    8u + sparseFastRadiusBonus * 8u,
                    sparseRequestBudgetThisFrame.visible / 2u);
                hierarchy.motionVisibleMinSpeed = static_cast<float>(
                    flightMode ? std::min<uint32_t>(24u, sparseMotionVisibleMinSpeed) : sparseMotionVisibleMinSpeed);
                hierarchy.motionVisibleRadiusXz =
                    std::max(hierarchy.nearVisibleRadiusXz, 2u + std::min<uint32_t>(3u, sparseFastRadiusBonus));
                hierarchy.motionVisibleRadiusY =
                    std::max(hierarchy.nearVisibleRadiusY, 1u + std::min<uint32_t>(2u, sparseFastRadiusBonus / 2u));
                hierarchy.maxMotionVisibleRequests = std::min<uint32_t>(
                    sparseMotionVisibleMaxRequests + sparseFastRadiusBonus * 12u,
                    std::max<uint32_t>(hierarchy.maxNearVisibleRequests, sparseRequestBudgetThisFrame.visible));
                hierarchy.ownershipPressureLevel = sparseEffectiveOwnershipPressureLevelThisFrame;
                hierarchy.maxOwnershipRecoveryRequests =
                    sparseEffectiveOwnershipPressureLevelThisFrame > 0
                        ? std::min<uint32_t>(
                              sparseRequestBudgetThisFrame.visible,
                              16u + sparseEffectiveOwnershipPressureLevelThisFrame * 24u)
                        : 0u;
                if ((brushController.IsPainting() || brushController.IsErasing()) &&
                    buildStrokeState.hasLastBrushWorldPosition) {
                    const glm::vec3 brushIntentEnd = buildStrokeState.hasPreviewWorldPosition
                        ? buildStrokeState.previewWorldPosition
                        : buildStrokeState.stableAimWorldPosition;
                    hierarchy.brushIntentValid = true;
                    hierarchy.brushStartX = buildStrokeState.lastBrushWorldPosition.x;
                    hierarchy.brushStartY = buildStrokeState.lastBrushWorldPosition.y;
                    hierarchy.brushStartZ = buildStrokeState.lastBrushWorldPosition.z;
                    hierarchy.brushEndX = brushIntentEnd.x;
                    hierarchy.brushEndY = brushIntentEnd.y;
                    hierarchy.brushEndZ = brushIntentEnd.z;
                    hierarchy.brushRadius = brushController.GetRadius();
                    hierarchy.maxBrushCollisionRequests = std::min<uint32_t>(
                        48u,
                        std::max<uint32_t>(8u, sparseRequestBudgetThisFrame.collision / 3u));
                    hierarchy.reservedBrushCollisionRequests = std::min<uint32_t>(
                        hierarchy.maxBrushCollisionRequests,
                        std::max<uint32_t>(8u, sparseRequestBudgetThisFrame.collision / 4u));
                    sparseBrushIntentActiveLastFrame = 1;
                    sparseBrushCollisionMaxLastFrame = hierarchy.maxBrushCollisionRequests;
                    sparseBrushCollisionReserveLastFrame = hierarchy.reservedBrushCollisionRequests;
                }
                hierarchy.visibleRayGrid = sparseViewPrefetchRayGrid;
                hierarchy.speculativeRayGrid = sparseViewPrefetchRayGrid;
                hierarchy.maxCollisionRequests = sparseRequestBudgetThisFrame.collision;
                hierarchy.maxVisibleRequests = sparseRequestBudgetThisFrame.visible;
                hierarchy.maxSpeculativeRequests = sparseRequestBudgetThisFrame.speculative;
                hierarchy.maxRequests = sparseRequestBudgetThisFrame.protectedHardTotal;

                for (const auto& request : sparseRequestPlanner.PlanHierarchical(hierarchy)) {
                    requestSparseBrick(request.coord, request.urgent, request.residencyClass);
                }
                lastSparseRequestCenter = sparseCenter;
            } else {
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
            }
            sparseStressRequestsLastFrame = 0;
            sparseStressAcceptedLastFrame = 0;
            if (enableSparseStressRequests && sparseStressBudget > 0) {
                Simulation::SparseStressRequestConfig stress{};
                stress.center = sparseCenter;
                stress.radiusXz = sparseStressRadiusXz;
                stress.radiusY = sparseStressRadiusY;
                stress.maxRequests = sparseStressBudget;
                stress.cursor = sparseStressCursor;
                stress.includeCollisionCore = true;
                const auto stressRequests = sparseRequestPlanner.PlanStressVolume(stress);
                sparseStressRequestsLastFrame = static_cast<uint32_t>(stressRequests.size());
                sparseStressCursor += std::max<uint32_t>(1u, sparseStressBudget);
                for (const auto& request : stressRequests) {
                    if (requestSparseBrick(request.coord, request.urgent, request.residencyClass)) {
                        ++sparseStressAcceptedLastFrame;
                    }
                }
            }
            lastSparseResidencyCameraWorld = cameraPos;
            sparseSpeculativeRequestsLastFrame = sparseSpeculativeRequestsThisFrame;
            sparseVisibleRequestsLastFrame = sparseVisibleRequestsThisFrame;
            sparseCollisionRequestsLastFrame = sparseCollisionRequestsThisFrame;
            sparseSpeculativeRequestBudgetLastFrame = sparseRequestBudgetThisFrame.speculative;
            sparseVisibleRequestBudgetLastFrame = sparseRequestBudgetThisFrame.visible;
            sparseCollisionRequestBudgetLastFrame = sparseRequestBudgetThisFrame.collision;
            sparseTotalRequestBudgetLastFrame = sparseRequestBudgetThisFrame.total;
            sparseProtectedRequestOverageLastFrame = sparseProtectedRequestOverageThisFrame;
            sparseRequestFreePageSkipsLastFrame = sparseRequestFreePageSkipsThisFrame;
            sparseRequestClassBudgetSkipsLastFrame = sparseRequestClassBudgetSkipsThisFrame;
            sparseRequestTotalBudgetSkipsLastFrame = sparseRequestTotalBudgetSkipsThisFrame;
            sparseRequestRejectedSkipsLastFrame = sparseRequestRejectedSkipsThisFrame;
            sparseRequestKnownEmptySkipsLastFrame = sparseRequestKnownEmptySkipsThisFrame;
            sparseVoxelWorld.SetStatsRefreshDeferred(false);
            sparseVoxelWorld.FlushStats();

            const auto& sparseStatsBeforeGeneration = sparseVoxelWorld.GetStats();
            const Simulation::SparseRuntimeBudgetDecision sparseGenerationDecision =
                evaluateSparseBudgetFromStats(sparseStatsBeforeGeneration);
            sparseRuntimeBudgetScale = sparseGenerationDecision.scale;
            sparseProtectedRuntimeBudgetScale = sparseGenerationDecision.protectedScale;
            sparseBackgroundRuntimeBudgetScale = sparseGenerationDecision.backgroundScale;
            sparseRuntimePressureClass = sparseGenerationDecision.pressureClass;
            sparseProtectedBacklogLastFrame = sparseGenerationDecision.hasProtectedBacklog ? 1u : 0u;
            sparseTrimSpeculativeFirstLastFrame = sparseGenerationDecision.trimSpeculativeFirst ? 1u : 0u;
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
            const uint32_t sparseGenerationOwnershipPressureLevel =
                computeSparseEffectiveOwnershipPressureLevel();
            const bool sparseGenerationCatchupActive =
                sparseResidencyCatchupFramesRemaining > 0 ||
                sparseGenerationOwnershipPressureLevel > 0;
            if (sparseGenerationCatchupActive) {
                const uint32_t ownershipGenerationMultiplier =
                    std::clamp<uint32_t>(sparseGenerationOwnershipPressureLevel, 1u, 3u) + 1u;
                sparseGenerationBudgetThisFrame =
                    std::max(
                        sparseGenerationBudgetThisFrame,
                        sparseCatchupGenerationBudget * ownershipGenerationMultiplier);
            }
            const bool sparseProtectedGenerationBacklog =
                sparseStatsBeforeGeneration.generationQueuedVisibleBricks > 0 ||
                sparseStatsBeforeGeneration.generationQueuedCollisionBricks > 0 ||
                sparseStatsBeforeGeneration.generationQueuedEditedBricks > 0 ||
                sparseStatsBeforeGeneration.physicsHotCandidateBricks > 0;
            sparseGenerationBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildProcessingBudget(
                    sparseGenerationBudgetThisFrame,
                    sparseStatsBeforeGeneration.generationQueuedBricks,
                    sparseProtectedGenerationBacklog,
                    sparseGenerationDecision,
                    1u,
                    sparseGenerationCatchupActive ? 8u : 4u);
            sparseGenerationBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
                    sparseGenerationBudgetThisFrame,
                    sparseStatsBeforeGeneration.generationQueuedEditedBricks,
                    sparseGenerationDecision,
                    std::max(32u, sparseGenerationBudget * 8u));
            sparseGenerationBudgetLastFrame = sparseGenerationBudgetThisFrame;
            sparseVoxelWorld.PumpGenerationAround(
                sparseGenerationBudgetThisFrame,
                sparseCenter,
                sparseResidencyFrame);
            if (sparseClipmapTileCacheReady && sparseClipmapPolicy.IsEnabled()) {
                const glm::vec3 sparseClipmapVelocity =
                    dt > 0.001f
                        ? (cameraPos - cameraPosBeforeInputMovement) / dt
                        : glm::vec3(0.0f);
                sparseClipmapTileCache.UpdateInterest(
                    cameraPos.x,
                    cameraPos.y,
                    cameraPos.z,
                    sparseResidencyFrame,
                    sparseClipmapPolicy,
                    cameraForward.x,
                    cameraForward.y,
                    cameraForward.z,
                    sparseClipmapVelocity.x,
                    sparseClipmapVelocity.y,
                    sparseClipmapVelocity.z,
                    std::max(0.25f, static_cast<float>(sparsePredictivePrefetchMs) * 0.001f));
                const auto& sparseMidClipmapStatsForBudget = sparseClipmapTileCache.GetStats();
                const uint32_t sparseMidClipmapBudgetThisFrame =
                    Simulation::SparseRuntimeBudgetScheduler::BuildProcessingBudget(
                        sparseMidClipmapTileBudget,
                        sparseMidClipmapStatsForBudget.queuedTiles +
                            sparseMidClipmapStatsForBudget.queuedVoxelBricks,
                        false,
                        sparseGenerationDecision,
                        1u,
                        4u);
                sparseMidClipmapBudgetLastFrame = sparseMidClipmapBudgetThisFrame;
                sparseClipmapTileCache.PumpGeneration(
                    sparseMidClipmapBudgetThisFrame,
                    sparseResidencyFrame,
                    sparseClipmapPolicy);
            }
            const auto& sparseStatsBeforeDistanceTrim = sparseVoxelWorld.GetStats();
            if (sparseStatsBeforeDistanceTrim.residentBricks >= sparseTrimStartResident ||
                sparseStatsBeforeDistanceTrim.freePages <= sparseMinFreePages) {
                if (sparseTrimSpeculativeFirstLastFrame) {
                    sparseVoxelWorld.TrimBackgroundResidentBricks(
                        sparseCenter,
                        sparseTrimRadiusXz,
                        sparseTrimRadiusY,
                        sparseTrimBudget,
                        sparseResidencyFrame);
                } else {
                    sparseVoxelWorld.TrimResidentBricks(
                        sparseCenter,
                        sparseTrimRadiusXz,
                        sparseTrimRadiusY,
                        sparseTrimBudget);
                }
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

        sparseBodyCollisionBlockedLastFrame = 0;
        sparseBodyCollisionStepUpsLastFrame = 0;
        sparseBodyCollisionGroundedLastFrame = 0;
        sparseBodyCollisionGroundSnapsLastFrame = 0;
        sparseBodyCollisionVerticalBlockedLastFrame = 0;
        sparseBodyCollisionLandedLastFrame = 0;
        sparseBodyCollisionCeilingLastFrame = 0;
        sparseBodyCollisionSampledLastFrame = 0;
        sparseBodyCollisionSolidLastFrame = 0;
        sparseBodyCollisionLiquidLastFrame = 0;
        sparseBodyCollisionSafeFractionLastFrame = 1.0f;

        // Apply gravity to vertical velocity (only when not in flight mode AND terrain is ready)
        // During startup, terrain might not be generated yet - disable gravity until
        // ground detection works to prevent falling through the world
        if (gameplayInputEnabled && !flightMode && terrainReady && supportChunkReadyForWalking) {
            cameraVelocityY += gravity * dt;
        }

        // Apply vertical velocity to camera position (only if terrain ready or flying)
        const glm::vec3 cameraPosBeforeVerticalMovement = cameraPos;
        if (gameplayInputEnabled && ((terrainReady && (supportChunkReadyForWalking || cameraVelocityY >= 0.0f)) || flightMode)) {
            cameraPos.y += cameraVelocityY * dt;
        }

        if (enableSparseBodyCollision &&
            gameplayInputEnabled &&
            !flightMode &&
            terrainReady &&
            sparseCollisionAuthoritative) {
            Simulation::SparseCharacterVerticalMoveRequest verticalRequest;
            verticalRequest.startBody = Simulation::SparseCharacterBody{
                cameraPosBeforeVerticalMovement.x,
                cameraPosBeforeVerticalMovement.y,
                cameraPosBeforeVerticalMovement.z,
                playerHeight,
                playerRadius,
                stepHeight
            };
            verticalRequest.targetBody = Simulation::SparseCharacterBody{
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                playerHeight,
                playerRadius,
                stepHeight
            };
            verticalRequest.verticalVelocity = cameraVelocityY;
            verticalRequest.maxSweepSteps = 24;
            const Simulation::SparseCharacterVerticalMoveResult verticalResult =
                Simulation::ResolveSparseCharacterVerticalMove(sparseVoxelWorld, verticalRequest);
            sparseBodyCollisionVerticalBlockedLastFrame = verticalResult.blocked ? 1u : 0u;
            sparseBodyCollisionLandedLastFrame = verticalResult.landed ? 1u : 0u;
            sparseBodyCollisionCeilingLastFrame = verticalResult.hitCeiling ? 1u : 0u;
            sparseBodyCollisionSampledLastFrame += verticalResult.sampledVoxels;
            sparseBodyCollisionSolidLastFrame += verticalResult.solidVoxels;
            sparseBodyCollisionLiquidLastFrame += verticalResult.liquidVoxels;
            if (verticalResult.blocked) {
                cameraPos.y = verticalResult.eyeY;
                cameraVelocityY = verticalResult.verticalVelocity;
            }

            Simulation::SparseCharacterMoveRequest moveRequest;
            moveRequest.startBody = Simulation::SparseCharacterBody{
                cameraPosBeforeInputMovement.x,
                cameraPos.y,
                cameraPosBeforeInputMovement.z,
                playerHeight,
                playerRadius,
                stepHeight
            };
            moveRequest.targetBody = Simulation::SparseCharacterBody{
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                playerHeight,
                playerRadius,
                stepHeight
            };
            moveRequest.verticalVelocity = cameraVelocityY;
            moveRequest.allowStepUp = true;
            moveRequest.maxSweepSteps = 16;

            const Simulation::SparseCharacterMoveResult moveResult =
                Simulation::ResolveSparseCharacterHorizontalMove(sparseVoxelWorld, moveRequest);
            sparseBodyCollisionBlockedLastFrame = moveResult.blocked ? 1u : 0u;
            sparseBodyCollisionStepUpsLastFrame = moveResult.steppedUp ? 1u : 0u;
            sparseBodyCollisionSafeFractionLastFrame = moveResult.safeFraction;
            sparseBodyCollisionSampledLastFrame = moveResult.sampledVoxels;
            sparseBodyCollisionSolidLastFrame = moveResult.solidVoxels;
            sparseBodyCollisionLiquidLastFrame = moveResult.liquidVoxels;
            if (moveResult.blocked || moveResult.steppedUp) {
                cameraPos.x = moveResult.eyeX;
                cameraPos.y = moveResult.eyeY;
                cameraPos.z = moveResult.eyeZ;
                if (moveResult.steppedUp) {
                    cameraVelocityY = 0.0f;
                }
            }

            Simulation::SparseCharacterGroundRequest groundRequest;
            groundRequest.body = Simulation::SparseCharacterBody{
                cameraPos.x,
                cameraPos.y,
                cameraPos.z,
                playerHeight,
                playerRadius,
                stepHeight
            };
            groundRequest.verticalVelocity = cameraVelocityY;
            groundRequest.maxSnapUp = stepHeight + 0.25f;
            groundRequest.maxSnapDown = cameraVelocityY <= 0.0f
                ? std::max(1.0f, -cameraVelocityY * dt + 1.0f)
                : 0.0f;
            groundRequest.liquidsSupport = false;
            const Simulation::SparseCharacterGroundResult groundResult =
                Simulation::ResolveSparseCharacterGrounding(sparseVoxelWorld, groundRequest);
            sparseBodyCollisionGroundedLastFrame = groundResult.grounded ? 1u : 0u;
            sparseBodyCollisionGroundSnapsLastFrame = groundResult.snapped ? 1u : 0u;
            sparseBodyCollisionSampledLastFrame += groundResult.sampledVoxels;
            sparseBodyCollisionSolidLastFrame += groundResult.solidVoxels;
            sparseBodyCollisionLiquidLastFrame += groundResult.liquidVoxels;
            if (groundResult.grounded && cameraVelocityY <= 0.0f) {
                cameraPos.y = groundResult.eyeY;
                cameraVelocityY = groundResult.verticalVelocity;
                if (jumpPressed && !flightTogglePressed) {
                    cameraVelocityY = 20.0f;
                }
            }
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
        const bool sparseCpuRaycastAuthoritative =
            sparseRuntimeTestMode && sparseVoxelWorldReady && !enableSparseGpuRaycast;
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
        sparseBrushFeedbackRetiredLastFrame = 0;
        sparseBrushFeedbackAppliedLastFrame = 0;
        sparseBrushFeedbackOverflowLastFrame = 0;
        sparseBrushFeedbackMissingResidentLastFrame = 0;
        sparseBrushFeedbackCpuFallbackLastFrame = 0;
        sparseBrushFeedbackMissingResidentHintsLastFrame = 0;
        sparseBrushFeedbackMissingResidentRequestsLastFrame = 0;
        voxelWorld->RetireBrushEditFeedback(commandQueue->GetLastCompletedFenceValue());
        sparseSurfaceGpuResources.RetireGpuCullStatsReadback(frameIndex);
        if (sparseCpuRaycastAuthoritative) {
            hasCompletedGroundQuery = false;
            hasCompletedBrushQuery = false;
            groundQueryMetadata[frameIndex].valid = false;
            brushQueryMetadata[frameIndex].valid = false;
        } else if (voxelWorld->RetireGroundRaycastReadback(frameIndex) && groundQueryMetadata[frameIndex].valid) {
            completedGroundQueryRegionOriginWorld = groundQueryMetadata[frameIndex].regionOriginWorld;
            completedGroundQueryFeetWorld = groundQueryMetadata[frameIndex].feetWorld;
            hasCompletedGroundQuery = true;
        }
        if (!sparseCpuRaycastAuthoritative &&
            voxelWorld->RetireBrushRaycastReadback(frameIndex) &&
            brushQueryMetadata[frameIndex].valid) {
            completedBrushQueryRegionOriginWorld = brushQueryMetadata[frameIndex].regionOriginWorld;
            completedBrushQueryOriginWorld = brushQueryMetadata[frameIndex].originWorld;
            completedBrushQueryDirectionWorld = brushQueryMetadata[frameIndex].directionWorld;
            hasCompletedBrushQuery = true;
        }
        if (enableSparseMissFeedback && sparseGpuResources.IsInitialized()) {
            sparseGpuResources.RetireMissFeedback(
                static_cast<uint32_t>(frameCount),
                sparseMissFeedbackPending);
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
        if (enableSparseBrushFeedback && sparseGpuResources.IsInitialized()) {
            std::vector<Simulation::SparseBrushFeedbackRecord> feedbackRecords;
            if (sparseGpuResources.RetireBrushFeedback(
                    static_cast<uint32_t>(frameCount),
                    feedbackRecords)) {
                std::vector<Simulation::SparseBrushFeedbackRecord> editFeedbackRecords;
                editFeedbackRecords.reserve(feedbackRecords.size());
                std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> missingHintBricks;
                for (const auto& record : feedbackRecords) {
                    if (Simulation::IsSparseBrushFeedbackMissingResident(record)) {
                        ++sparseBrushFeedbackMissingResidentHintsLastFrame;
                        missingHintBricks.insert(Simulation::BrickCoord::FromWorldVoxel(
                            record.worldX,
                            record.worldY,
                            record.worldZ));
                    } else {
                        editFeedbackRecords.push_back(record);
                    }
                }
                const uint32_t retiredBrushFeedbackFrame =
                    sparseGpuResources.GetStats().brushFeedbackFrameLastRetire;
                const bool retiredBrushFeedbackFrameAlreadyApplied =
                    retiredBrushFeedbackFrame != 0u &&
                    retiredBrushFeedbackFrame == sparseBrushFeedbackLastAppliedFrame;
                auto popPendingAuthoritativeBrushStrokesForFrame =
                    [&](uint32_t producerFrame, std::vector<SparseBrushFeedbackPendingStroke>& outStrokes) {
                        if (producerFrame == 0u) {
                            return;
                        }
                        while (!sparseBrushFeedbackPendingStrokes.empty() &&
                               sparseBrushFeedbackPendingStrokes.front().frame + 180u < producerFrame) {
                            sparseBrushFeedbackPendingStrokes.pop_front();
                        }
                        for (auto it = sparseBrushFeedbackPendingStrokes.begin();
                             it != sparseBrushFeedbackPendingStrokes.end();) {
                            if (it->frame == producerFrame) {
                                outStrokes.push_back(*it);
                                it = sparseBrushFeedbackPendingStrokes.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    };
                if (enableSparseBrushFeedbackApply &&
                    !retiredBrushFeedbackFrameAlreadyApplied &&
                    sparseBrushFeedbackMissingResidentHintsLastFrame != 0u) {
                    for (const Simulation::BrickCoord& missingBrick : missingHintBricks) {
                        if (sparseBrushFeedbackMissingResidentRequestSet.insert(missingBrick).second) {
                            sparseBrushFeedbackMissingResidentRequestQueue.push_back(missingBrick);
                        }
                    }
                }
                sparseBrushFeedbackRetiredLastFrame =
                    static_cast<uint32_t>(editFeedbackRecords.size());
                sparseBrushFeedbackOverflowLastFrame =
                    sparseGpuResources.GetStats().brushFeedbackOverflowLastRetire ? 1u : 0u;
                sparseBrushFeedbackMissingResidentLastFrame =
                    sparseGpuResources.GetStats().brushFeedbackMissingResidentLastRetire;
                if (sparseBrushFeedbackParityPending &&
                    !sparseBrushFeedbackParityObserved &&
                    retiredBrushFeedbackFrame == sparseBrushFeedbackParityExpectedFrame) {
                    auto makeFeedbackKey = [](int32_t x, int32_t y, int32_t z) {
                        return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
                    };
                    std::unordered_map<std::string, uint32_t> expectedByVoxel;
                    expectedByVoxel.reserve(sparseBrushFeedbackParityExpected.size());
                    for (const auto& expected : sparseBrushFeedbackParityExpected) {
                        expectedByVoxel.emplace(
                            makeFeedbackKey(expected.worldX, expected.worldY, expected.worldZ),
                            expected.voxel);
                    }

                    uint32_t matched = 0;
                    uint32_t valueMismatches = 0;
                    uint32_t unexpected = 0;
                    std::unordered_set<std::string> seenGpuVoxels;
                    seenGpuVoxels.reserve(editFeedbackRecords.size());
                    for (const auto& record : editFeedbackRecords) {
                        const std::string key = makeFeedbackKey(record.worldX, record.worldY, record.worldZ);
                        if (!seenGpuVoxels.insert(key).second) {
                            ++unexpected;
                            continue;
                        }
                        const auto expectedIt = expectedByVoxel.find(key);
                        if (expectedIt == expectedByVoxel.end()) {
                            ++unexpected;
                            continue;
                        }
                        if (expectedIt->second == record.voxel) {
                            ++matched;
                        } else {
                            ++valueMismatches;
                        }
                    }
                    uint32_t missing = 0;
                    for (const auto& expected : sparseBrushFeedbackParityExpected) {
                        const std::string key =
                            makeFeedbackKey(expected.worldX, expected.worldY, expected.worldZ);
                        if (seenGpuVoxels.find(key) == seenGpuVoxels.end()) {
                            ++missing;
                        }
                    }

                    sparseBrushFeedbackParityExpectedLastFrame =
                        static_cast<uint32_t>(sparseBrushFeedbackParityExpected.size());
                    sparseBrushFeedbackParityMatchedLastFrame = matched;
                    sparseBrushFeedbackParityMissingLastFrame = missing;
                    sparseBrushFeedbackParityUnexpectedLastFrame = unexpected;
                    sparseBrushFeedbackParityValueMismatchLastFrame = valueMismatches;
                    sparseBrushFeedbackParityObserved = true;
                    sparseBrushFeedbackParityPending = false;
                    const bool missingResidentFailed =
                        sparseBrushFeedbackParityExpectsMissingResident
                            ? sparseBrushFeedbackMissingResidentLastFrame == 0
                            : sparseBrushFeedbackMissingResidentLastFrame != 0;
                    const bool parityFailed =
                        sparseBrushFeedbackOverflowLastFrame != 0 ||
                        missingResidentFailed ||
                        missing != 0 ||
                        unexpected != 0 ||
                        valueMismatches != 0 ||
                        matched != sparseBrushFeedbackParityExpectedLastFrame;
                    sparseBrushFeedbackParityFailed = parityFailed;
                    sparseBrushFeedbackParityFailureFrame =
                        parityFailed ? static_cast<uint32_t>(frameCount) : sparseBrushFeedbackParityFailureFrame;
                    if (parityFailed) {
                        spdlog::error(
                            "SPARSE_BRUSH_FEEDBACK parity failed at frame {} case={} expected={} gpu={} matched={} missing={} unexpected={} valueMismatch={} overflow={} missingResident={}",
                            frameCount,
                            sparseBrushFeedbackParityLabel,
                            sparseBrushFeedbackParityExpected.size(),
                            editFeedbackRecords.size(),
                            matched,
                            missing,
                            unexpected,
                            valueMismatches,
                            sparseBrushFeedbackOverflowLastFrame,
                            sparseBrushFeedbackMissingResidentLastFrame);
                    } else {
                        ++sparseBrushFeedbackDiagnosticCasesPassed;
                        sparseBrushFeedbackDiagnosticNextFrame = static_cast<uint32_t>(frameCount + 30u);
                        spdlog::info(
                            "SPARSE_BRUSH_FEEDBACK parity observed at frame {} case={} expected={} gpu={} matched={} missingResident={}",
                            frameCount,
                            sparseBrushFeedbackParityLabel,
                            sparseBrushFeedbackParityExpected.size(),
                            editFeedbackRecords.size(),
                            matched,
                            sparseBrushFeedbackMissingResidentLastFrame);
                        if (enableSparseBrushFeedbackDiagnosticSeed &&
                            sparseBrushFeedbackDiagnosticCasesPassed >= 7u &&
                            !sparseBrushFeedbackDiagnosticSuitePassed) {
                            sparseBrushFeedbackDiagnosticSuitePassed = true;
                            spdlog::info(
                                "SPARSE_BRUSH_FEEDBACK diagnostic suite passed cases={}",
                                sparseBrushFeedbackDiagnosticCasesPassed);
                        }
                    }
                }
                if (enableSparseBrushFeedbackApply &&
                    !retiredBrushFeedbackFrameAlreadyApplied &&
                    sparseBrushFeedbackMissingResidentLastFrame == 0u) {
                    std::vector<SparseBrushFeedbackPendingStroke> completedAuthoritativeStrokes;
                    if (enableSparseBrushFeedbackAuthoritative) {
                        popPendingAuthoritativeBrushStrokesForFrame(
                            retiredBrushFeedbackFrame,
                            completedAuthoritativeStrokes);
                    }
                    for (const auto& record : editFeedbackRecords) {
                        sparseVoxelWorld.SetEditedVoxel(
                            record.worldX,
                            record.worldY,
                            record.worldZ,
                            record.voxel);
                    }
                    sparseBrushFeedbackAppliedLastFrame =
                        static_cast<uint32_t>(editFeedbackRecords.size());
                    if (!editFeedbackRecords.empty()) {
                        spdlog::info(
                            "SPARSE_BRUSH_FEEDBACK GPU apply at frame {} records={} authoritative={} completedStrokes={}",
                            frameCount,
                            editFeedbackRecords.size(),
                            enableSparseBrushFeedbackAuthoritative ? 1 : 0,
                            completedAuthoritativeStrokes.size());
                    }
                    if (retiredBrushFeedbackFrame != 0u &&
                        (!editFeedbackRecords.empty() || !completedAuthoritativeStrokes.empty())) {
                        sparseBrushFeedbackLastAppliedFrame = retiredBrushFeedbackFrame;
                    }
                } else if (enableSparseBrushFeedbackApply &&
                    !retiredBrushFeedbackFrameAlreadyApplied &&
                    (sparseBrushFeedbackMissingResidentLastFrame != 0u ||
                     sparseBrushFeedbackOverflowLastFrame != 0u)) {
                    std::vector<SparseBrushFeedbackPendingStroke> fallbackStrokes;
                    if (enableSparseBrushFeedbackAuthoritative) {
                        popPendingAuthoritativeBrushStrokesForFrame(
                            retiredBrushFeedbackFrame,
                            fallbackStrokes);
                        for (const auto& stroke : fallbackStrokes) {
                            sparseVoxelWorld.ApplyBrushEdit(
                                stroke.worldX,
                                stroke.worldY,
                                stroke.worldZ,
                                stroke.radius,
                                stroke.material,
                                stroke.mode,
                                stroke.shape,
                                stroke.strength,
                                stroke.seed,
                                stroke.hitNormalX,
                                stroke.hitNormalY,
                                stroke.hitNormalZ,
                                stroke.hasHitNormal,
                                true,
                                nullptr);
                        }
                    }
                    sparseBrushFeedbackCpuFallbackLastFrame = 1u;
                    if (retiredBrushFeedbackFrame != 0u) {
                        sparseBrushFeedbackLastAppliedFrame = retiredBrushFeedbackFrame;
                    }
                    spdlog::info(
                        "SPARSE_BRUSH_FEEDBACK CPU fallback at frame {} records={} missingResident={} hints={} queuedBricks={} brickRequests={} authoritative={} fallbackStrokes={}",
                        frameCount,
                        editFeedbackRecords.size(),
                        sparseBrushFeedbackMissingResidentLastFrame,
                        sparseBrushFeedbackMissingResidentHintsLastFrame,
                        missingHintBricks.size(),
                        sparseBrushFeedbackMissingResidentRequestsLastFrame,
                        enableSparseBrushFeedbackAuthoritative ? 1 : 0,
                        fallbackStrokes.size());
                }
            }
        }
        if (enableSparseRenderOwnershipStats && sparseGpuResources.IsInitialized()) {
            const bool retiredOwnershipThisFrame =
                sparseGpuResources.RetireRenderOwnership(static_cast<uint32_t>(frameCount));
            const auto& ownershipStats = sparseGpuResources.GetStats();
            const bool hasRetiredOwnershipSample =
                ownershipStats.renderOwnerTotalPixelsLastRetire > 0;
            if (hasRetiredOwnershipSample) {
                const uint64_t totalPixels = ownershipStats.renderOwnerTotalPixelsLastRetire;
                // `surface` is emitted by the rasterized sparse-surface pass.
                // That pass can overdraw, so it is a fragment counter, not a
                // final pixel-owner counter. Keep it in logs as a useful
                // diagnostic, but do not include it in the terrain quality /
                // stability gate where every other owner is counted once per
                // fullscreen raymarch pixel. Including it made the terrain
                // percentage saturate at 100% and then fail when surface
                // overdraw changed at chunk/boundary transitions.
                const uint64_t terrainPixelsRaw =
                    static_cast<uint64_t>(ownershipStats.renderOwnerNearPixelsLastRetire) +
                    static_cast<uint64_t>(ownershipStats.renderOwnerMidVoxelPixelsLastRetire) +
                    static_cast<uint64_t>(ownershipStats.renderOwnerMidHeightPixelsLastRetire) +
                    static_cast<uint64_t>(ownershipStats.renderOwnerFarSvoPixelsLastRetire) +
                    static_cast<uint64_t>(ownershipStats.renderOwnerFarHeightPixelsLastRetire);
                const uint64_t terrainPixels = std::min(totalPixels, terrainPixelsRaw);
                const uint64_t skyPixels = std::min<uint64_t>(
                    totalPixels,
                    ownershipStats.renderOwnerSkyPixelsLastRetire);
                const uint64_t ownershipTestPixels = std::max<uint64_t>(1ull, totalPixels - skyPixels);
                const uint32_t terrainPct =
                    static_cast<uint32_t>((terrainPixels * 100ull) / ownershipTestPixels);
                const uint32_t missPct =
                    static_cast<uint32_t>(
                        (static_cast<uint64_t>(ownershipStats.renderOwnerMissPixelsLastRetire) * 100ull) /
                        ownershipTestPixels);
                const uint32_t unsafeNearMissPct =
                    static_cast<uint32_t>(
                        (static_cast<uint64_t>(ownershipStats.renderOwnerUnsafeNearMissPixelsLastRetire) * 100ull) /
                        ownershipTestPixels);
                const float invTotalPixels =
                    1.0f / static_cast<float>(std::max<uint64_t>(1ull, totalPixels));
                sparseOwnershipTerrainPctLastRetire = terrainPct;
                sparseOwnershipMissPctLastRetire = missPct;
                sparseOwnershipUnsafeNearMissPctLastRetire = unsafeNearMissPct;
                sparseSurfaceFragmentsLastRetire = ownershipStats.renderOwnerSurfacePixelsLastRetire;
                const uint64_t screenPixelsAtRetire =
                    static_cast<uint64_t>(window->GetWidth()) *
                    static_cast<uint64_t>(window->GetHeight());
                sparseSurfaceOwnedPixelsLastRetire =
                    static_cast<uint32_t>(
                        screenPixelsAtRetire > totalPixels
                            ? std::min<uint64_t>(
                                  screenPixelsAtRetire - totalPixels,
                                  static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
                            : 0ull);
                sparseOwnershipBackgroundPixelShareLastRetire =
                    screenPixelsAtRetire > 0
                        ? std::clamp(
                              static_cast<float>(totalPixels) /
                                  static_cast<float>(screenPixelsAtRetire),
                              0.0f,
                              1.0f)
                        : 1.0f;
                if (enableRuntimeLog && retiredOwnershipThisFrame) {
                    spdlog::info(
                        "PERF_RENDER_COMPOSITION frame={} screen={} backgroundPixels={} surfaceOwnedPixels={} surfaceFragments={} overdrawRatio={:.2f}",
                        ownershipStats.renderOwnerFrameLastRetire,
                        screenPixelsAtRetire,
                        totalPixels,
                        sparseSurfaceOwnedPixelsLastRetire,
                        ownershipStats.renderOwnerSurfacePixelsLastRetire,
                        sparseSurfaceOwnedPixelsLastRetire > 0
                            ? static_cast<double>(ownershipStats.renderOwnerSurfacePixelsLastRetire) /
                                  static_cast<double>(sparseSurfaceOwnedPixelsLastRetire)
                            : 0.0);
                }
                sparseOwnershipMidVoxelPixelShareLastRetire =
                    static_cast<float>(ownershipStats.renderOwnerMidVoxelPixelsLastRetire) * invTotalPixels;
                sparseOwnershipFarHeightPixelShareLastRetire =
                    static_cast<float>(ownershipStats.renderOwnerFarHeightPixelsLastRetire) * invTotalPixels;
                sparseOwnershipSkyPixelShareLastRetire =
                    static_cast<float>(ownershipStats.renderOwnerSkyPixelsLastRetire) * invTotalPixels;
                Simulation::SparseOwnershipPressureInput ownershipPressureInput;
                ownershipPressureInput.frameIndex = ownershipStats.renderOwnerFrameLastRetire;
                ownershipPressureInput.readyFrame = sparseOwnershipCatchupReadyFrame;
                ownershipPressureInput.terrainPercent = terrainPct;
                ownershipPressureInput.missPercent = missPct;
                ownershipPressureInput.unsafeNearMissPercent = unsafeNearMissPct;
                ownershipPressureInput.minTerrainPercent = sparseOwnershipCatchupTerrainPct;
                ownershipPressureInput.maxMissPercent = sparseOwnershipCatchupMissPct;
                ownershipPressureInput.maxUnsafeNearMissPercent =
                    std::max(1u, sparseOwnershipCatchupMissPct / 3u);
                ownershipPressureInput.holdFrames = sparseOwnershipCatchupHoldFrames;
                ownershipPressureInput.currentCatchupFrames = sparseResidencyCatchupFramesRemaining;
                const Simulation::SparseOwnershipPressure ownershipPressure =
                    Simulation::SparseRuntimeBudgetScheduler::BuildOwnershipPressure(
                        ownershipPressureInput);
                sparseResidencyCatchupFramesRemaining =
                    ownershipPressure.updatedCatchupFrames;
                sparseOwnershipPressureTerrainDeficitLastRetire =
                    ownershipPressure.terrainDeficitPercent;
                sparseOwnershipPressureMissExcessLastRetire =
                    ownershipPressure.missExcessPercent;
                sparseOwnershipPressureUnsafeNearMissExcessLastRetire =
                    ownershipPressure.unsafeNearMissExcessPercent;
                sparseOwnershipPressureLevelLastRetire = ownershipPressure.level;
                if (ownershipPressure.triggered) {
                    sparseOwnershipPressureLevelActive =
                        std::max(sparseOwnershipPressureLevelActive, ownershipPressure.level);
                } else if (!ownershipPressure.active) {
                    sparseOwnershipPressureLevelActive = 0;
                }
                if (requireSparseOwnershipQuality &&
                    ownershipStats.renderOwnerFrameLastRetire >= sparseOwnershipQualityReadyFrame) {
                    sparseOwnershipQualityObserved = true;
                    if (terrainPct < sparseOwnershipMinTerrainPct ||
                        missPct > sparseOwnershipMaxMissPct ||
                        unsafeNearMissPct > sparseOwnershipMaxUnsafeNearMissPct) {
                        sparseOwnershipQualityFailed = true;
                        sparseOwnershipQualityFrame = ownershipStats.renderOwnerFrameLastRetire;
                        sparseOwnershipQualityTerrainPct = terrainPct;
                        sparseOwnershipQualityMissPct = missPct;
                        sparseOwnershipQualityUnsafeNearMissPct = unsafeNearMissPct;
                        spdlog::critical(
                            "SPARSE_RENDER_OWNERSHIP quality failed sampleFrame={} terrain={}%% min={}%% miss={}%% maxMiss={}%% unsafeNearMiss={}%% maxUnsafe={}%% total={} surfaceFragments={} midVoxel={} midHeight={} farSvo={} sky={} miss={} unsafeNearMiss={}",
                            ownershipStats.renderOwnerFrameLastRetire,
                            terrainPct,
                            sparseOwnershipMinTerrainPct,
                            missPct,
                            sparseOwnershipMaxMissPct,
                            unsafeNearMissPct,
                            sparseOwnershipMaxUnsafeNearMissPct,
                            ownershipStats.renderOwnerTotalPixelsLastRetire,
                            ownershipStats.renderOwnerSurfacePixelsLastRetire,
                            ownershipStats.renderOwnerMidVoxelPixelsLastRetire,
                            ownershipStats.renderOwnerMidHeightPixelsLastRetire,
                            ownershipStats.renderOwnerFarSvoPixelsLastRetire,
                            ownershipStats.renderOwnerSkyPixelsLastRetire,
                            ownershipStats.renderOwnerMissPixelsLastRetire,
                            ownershipStats.renderOwnerUnsafeNearMissPixelsLastRetire);
                        running = false;
                    }
                }
                if (requireSparseSurfaceFragments &&
                    ownershipStats.renderOwnerFrameLastRetire >= sparseSurfaceFragmentsReadyFrame) {
                    if (ownershipStats.renderOwnerSurfacePixelsLastRetire >= sparseMinSurfaceFragments) {
                        sparseSurfaceFragmentsObserved = true;
                        sparseSurfaceFragmentsFrame = ownershipStats.renderOwnerFrameLastRetire;
                    } else {
                        sparseSurfaceFragmentsFailed = true;
                        sparseSurfaceFragmentsFrame = ownershipStats.renderOwnerFrameLastRetire;
                        spdlog::critical(
                            "SPARSE_SURFACE_FRAGMENTS failed sampleFrame={} surfaceFragments={} min={} seedQueued={} seedVoxels={} seedBricks={} total={} near={} midVoxel={} midHeight={} farSvo={} sky={} miss={} unsafeNearMiss={}",
                            ownershipStats.renderOwnerFrameLastRetire,
                            ownershipStats.renderOwnerSurfacePixelsLastRetire,
                            sparseMinSurfaceFragments,
                            sparseSurfaceDiagnosticSeedQueued ? 1 : 0,
                            sparseSurfaceDiagnosticSeededVoxels,
                            sparseSurfaceDiagnosticSeededBricks,
                            ownershipStats.renderOwnerTotalPixelsLastRetire,
                            ownershipStats.renderOwnerNearPixelsLastRetire,
                            ownershipStats.renderOwnerMidVoxelPixelsLastRetire,
                            ownershipStats.renderOwnerMidHeightPixelsLastRetire,
                            ownershipStats.renderOwnerFarSvoPixelsLastRetire,
                            ownershipStats.renderOwnerSkyPixelsLastRetire,
                            ownershipStats.renderOwnerMissPixelsLastRetire,
                            ownershipStats.renderOwnerUnsafeNearMissPixelsLastRetire);
                        running = false;
                    }
                }
                if (requireSparseOwnershipStability &&
                    ownershipStats.renderOwnerFrameLastRetire >= sparseOwnershipStabilityReadyFrame) {
                    if (sparseOwnershipStabilityPrimed) {
                        const auto absDiff = [](uint32_t a, uint32_t b) -> uint32_t {
                            return a > b ? (a - b) : (b - a);
                        };
                        const uint32_t terrainDelta =
                            absDiff(terrainPct, sparseOwnershipStabilityPreviousTerrainPct);
                        const uint32_t missDelta =
                            absDiff(missPct, sparseOwnershipStabilityPreviousMissPct);
                        sparseOwnershipStabilityObserved = true;
                        if (terrainDelta > sparseOwnershipMaxTerrainDeltaPct ||
                            missDelta > sparseOwnershipMaxMissDeltaPct) {
                            sparseOwnershipStabilityFailed = true;
                            sparseOwnershipStabilityFrame = ownershipStats.renderOwnerFrameLastRetire;
                            sparseOwnershipStabilityFailurePreviousFrame = sparseOwnershipStabilityPreviousFrame;
                            sparseOwnershipStabilityTerrainDeltaPct = terrainDelta;
                            sparseOwnershipStabilityMissDeltaPct = missDelta;
                            spdlog::critical(
                                "SPARSE_RENDER_OWNERSHIP stability failed sampleFrame={} previousFrame={} terrain={}%% previousTerrain={}%% delta={}%% maxDelta={}%% miss={}%% previousMiss={}%% delta={}%% maxDelta={}%% unsafeNearMiss={}%% total={} surfaceFragments={} midVoxel={} midHeight={} farSvo={} sky={} miss={} unsafeNearMiss={}",
                                ownershipStats.renderOwnerFrameLastRetire,
                                sparseOwnershipStabilityPreviousFrame,
                                terrainPct,
                                sparseOwnershipStabilityPreviousTerrainPct,
                                terrainDelta,
                                sparseOwnershipMaxTerrainDeltaPct,
                                missPct,
                                sparseOwnershipStabilityPreviousMissPct,
                                missDelta,
                                sparseOwnershipMaxMissDeltaPct,
                                unsafeNearMissPct,
                                ownershipStats.renderOwnerTotalPixelsLastRetire,
                                ownershipStats.renderOwnerSurfacePixelsLastRetire,
                                ownershipStats.renderOwnerMidVoxelPixelsLastRetire,
                                ownershipStats.renderOwnerMidHeightPixelsLastRetire,
                                ownershipStats.renderOwnerFarSvoPixelsLastRetire,
                                ownershipStats.renderOwnerSkyPixelsLastRetire,
                                ownershipStats.renderOwnerMissPixelsLastRetire,
                                ownershipStats.renderOwnerUnsafeNearMissPixelsLastRetire);
                            running = false;
                        }
                    }
                    sparseOwnershipStabilityPrimed = true;
                    sparseOwnershipStabilityPreviousFrame = ownershipStats.renderOwnerFrameLastRetire;
                    sparseOwnershipStabilityPreviousTerrainPct = terrainPct;
                    sparseOwnershipStabilityPreviousMissPct = missPct;
                }
            }
        }
        bool physicsRanThisFrame = false;
        bool physicsSkippedForBudget = false;
        if (enableSparsePhysicsGpu && sparseGpuResources.IsInitialized()) {
            sparseGpuResources.RetirePhysicsDiagnostics(static_cast<uint32_t>(frameCount));
            if (sparseGpuResources.RetirePhysicsPacketResults(static_cast<uint32_t>(frameCount)) &&
                enableSparsePhysicsGpuApply &&
                sparseVoxelWorldReady) {
                const uint32_t appliedGpuMoves = sparseVoxelWorld.ApplyGpuPhysicsProposals(
                    sparseGpuResources.GetLastRetiredPhysicsProposals(),
                    sparsePhysicsMoveBudget,
                    true);
                if (appliedGpuMoves > 0) {
                    physicsRanThisFrame = true;
                    ++physicsDispatchCount;
                }
            }
        }

        // Reset command allocator and command list
        ctx.commandAllocator->Reset();
        commandList->Reset(ctx.commandAllocator.Get(), nullptr);
        const uint32_t gpuTimestampBase = frameIndex * kGpuTimestampCount;
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 0);
        }
        if (enableFarSVO && farVoxelOctree.HasPendingGpuUploadCopies()) {
            farVoxelOctree.EmitPendingGpuUploadCopies(commandList.Get());
        }

        if (sparseBackendRequested && sparseVoxelWorldReady) {
            sparseGpuResources.BeginFrame(frameIndex);
            if (sparseSurfaceGpuResources.IsInitialized()) {
                sparseSurfaceGpuResources.BeginFrame(
                    frameIndex,
                    commandQueue->GetLastCompletedFenceValue(),
                    commandQueue->GetNextFenceValue());
            }
            sparseSurfaceRasterFacesLastFrame = 0;
            sparseUploadRequeuesLastFrame = 0;
            sparseInvalidationRequeuesLastFrame = 0;
            sparsePageTablePublishRetriesLastFrame = 0;
            sparsePageTablePublishStaleDropsLastFrame = 0;
            sparseEditedPageTablePublishesQueuedLastFrame = 0;
            sparseEditedPageTablePublishesPublishedLastFrame = 0;
            sparseEditedPageTablePublishPromotionsLastFrame = 0;
            sparseUploadRingBudgetDefersLastFrame = 0;
            sparseUploadRingUsedBytesLastFrame = 0;
            sparseUploadRingCapacityBytesLastFrame = sparseGpuResources.ActiveUploadBytesCapacity();
            sparseFrameUploadReservedBytesLastFrame = 0;
            sparseFrameUploadRemainingBytesLastFrame = sparseUploadRingCapacityBytesLastFrame;
            sparseFrameUploadPlanDefersLastFrame = 0;
            sparseValueSelectedUploadsLastFrame = 0;
            sparseMidClipmapUploadRetriesLastFrame = 0;
            sparseSurfaceUploadRetriesLastFrame = 0;
            sparseUploadBudgetLastFrame = 0;
            sparseSurfaceExtractionBudgetLastFrame = 0;
            const auto& sparseStatsBeforeUpload = sparseVoxelWorld.GetStats();
            const auto& sparseGpuStatsForUploadPlan = sparseGpuResources.GetStats();
            const bool uploadHeightClipmapPending =
                sparseClipmapTileCacheReady &&
                sparseClipmapPolicy.IsEnabled() &&
                sparseClipmapTileCache.HeightDirtySerial() != sparseMidClipmapUploadedHeightSerial;
            const bool uploadVoxelClipmapPending =
                sparseClipmapTileCacheReady &&
                sparseClipmapPolicy.IsEnabled() &&
                sparseClipmapTileCache.VoxelDirtySerial() != sparseMidClipmapUploadedVoxelSerial;
            const bool sparseMidClipmapUploadPending =
                uploadHeightClipmapPending || uploadVoxelClipmapPending;
            Simulation::SparseClipmapGpuSnapshot sparseMidClipmapSnapshotForUpload;
            const bool sparseMidClipmapSnapshotReadyForUpload =
                sparseMidClipmapUploadPending &&
                sparseClipmapTileCache.BuildGpuSnapshot(sparseMidClipmapSnapshotForUpload);
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
            const uint32_t sparseUploadOwnershipPressureLevel =
                computeSparseEffectiveOwnershipPressureLevel();
            const bool sparseUploadCatchupActive =
                sparseResidencyCatchupFramesRemaining > 0 ||
                sparseUploadOwnershipPressureLevel > 0;
            if (sparseUploadCatchupActive) {
                const uint32_t ownershipUploadMultiplier =
                    std::clamp<uint32_t>(sparseUploadOwnershipPressureLevel, 1u, 3u) + 1u;
                sparseUploadBudgetThisFrame =
                    std::max(
                        sparseUploadBudgetThisFrame,
                        sparseCatchupUploadBudget * ownershipUploadMultiplier);
            }
            const bool sparseProtectedUploadBacklog =
                sparseStatsBeforeUpload.uploadQueuedVisibleBricks > 0 ||
                sparseStatsBeforeUpload.uploadQueuedCollisionBricks > 0 ||
                sparseStatsBeforeUpload.uploadQueuedEditedBricks > 0 ||
                sparseStatsBeforeUpload.generationQueuedVisibleBricks > 0 ||
                sparseStatsBeforeUpload.generationQueuedCollisionBricks > 0 ||
                sparseStatsBeforeUpload.generationQueuedEditedBricks > 0;
            const Simulation::SparseRuntimeBudgetDecision sparseUploadRuntimeDecision =
                evaluateSparseBudgetFromStats(sparseStatsBeforeUpload);
            sparseUploadBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildProcessingBudget(
                    sparseUploadBudgetThisFrame,
                    sparseStatsBeforeUpload.uploadQueuedBricks,
                    sparseProtectedUploadBacklog,
                    sparseUploadRuntimeDecision,
                    1u,
                    sparseUploadCatchupActive ? 8u : 4u);
            sparseUploadBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
                    sparseUploadBudgetThisFrame,
                    sparseStatsBeforeUpload.uploadQueuedEditedBricks,
                    sparseUploadRuntimeDecision,
                    std::max(32u, sparseUploadBudget * 8u));
            sparseUploadBudgetLastFrame = sparseUploadBudgetThisFrame;
            const Simulation::SparseUploadBudgetDecision sparseUploadClassBudget =
                Simulation::SparseRuntimeBudgetScheduler::BuildUploadBudgets(
                    sparseUploadBudgetThisFrame,
                    sparseStatsBeforeUpload.uploadQueuedSpeculativeBricks,
                    sparseStatsBeforeUpload.uploadQueuedVisibleBricks,
                    sparseStatsBeforeUpload.uploadQueuedCollisionBricks,
                    sparseStatsBeforeUpload.uploadQueuedEditedBricks,
                    sparseUploadRuntimeDecision);
            const uint64_t sparseEstimatedBrickUploadBytes =
                static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) * sizeof(uint32_t) +
                512u;
            const uint64_t sparseEstimatedPageEntryUploadBytes = 256u;
            const uint64_t sparseEstimatedMidClipmapBytes =
                sparseMidClipmapSnapshotReadyForUpload
                    ? SparseVoxelGpuResources::EstimateMidClipmapSnapshotUploadBytes(
                          sparseMidClipmapSnapshotForUpload,
                          uploadHeightClipmapPending,
                          uploadVoxelClipmapPending)
                    : 0u;
            const uint32_t sparseFrameIndexU32 = static_cast<uint32_t>(std::min<uint64_t>(
                frameCount,
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
            const uint64_t sparseCompletedFenceForUpload =
                commandQueue ? commandQueue->GetLastCompletedFenceValue() : 0u;
            const Simulation::SparsePagePublishQueueStats sparsePublishStatsForUpload =
                sparsePagePublishQueue.GetStats(
                    sparseFrameIndexU32,
                    sparseCompletedFenceForUpload);
            Simulation::SparseFrameUploadPlanInput sparseUploadPlanInput{};
            sparseUploadPlanInput.uploadBytesCapacity = sparseGpuResources.ActiveUploadBytesCapacity();
            sparseUploadPlanInput.uploadBytesAlreadyUsed = sparseGpuResources.ActiveUploadBytesUsed();
            sparseUploadPlanInput.pageTableResetBytes = sparseGpuStatsForUploadPlan.pageTableBytes;
            sparseUploadPlanInput.pageTableEntryBytes = sparseEstimatedPageEntryUploadBytes;
            sparseUploadPlanInput.brickUploadBytes = sparseEstimatedBrickUploadBytes;
            sparseUploadPlanInput.midClipmapSnapshotBytes = sparseEstimatedMidClipmapBytes;
            sparseUploadPlanInput.pageTableResetPending = sparseGpuPageTableResetPending;
            sparseUploadPlanInput.midClipmapDirty =
                sparseMidClipmapUploadPending && sparseMidClipmapSnapshotReadyForUpload;
            sparseUploadPlanInput.protectedBacklog = sparseProtectedUploadBacklog;
            sparseUploadPlanInput.publishProtectedBacklog =
                sparsePublishStatsForUpload.ready > 0 ||
                sparsePublishStatsForUpload.edited > 0 ||
                sparsePublishStatsForUpload.maxReadyFrameLag >= 2u;
            sparseUploadPlanInput.invalidationQueued = sparseVoxelWorld.InvalidationQueueSize();
            sparseUploadPlanInput.publishQueued =
                static_cast<uint32_t>(std::min<size_t>(
                    sparsePublishStatsForUpload.ready,
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
            sparseUploadPlanInput.invalidationBudget = sparseInvalidationBudget;
            sparseUploadPlanInput.publishBudget = sparsePageTablePublishBudget;
            sparseUploadPlanInput.brickBudgets = sparseUploadClassBudget;
            const Simulation::SparseFrameUploadPlan sparseFrameUploadPlan =
                Simulation::SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(sparseUploadPlanInput);
            const uint32_t sparsePagePublishesEligibleThisFrame =
                sparseUploadPlanInput.publishQueued;
            sparseFrameUploadReservedBytesLastFrame = sparseFrameUploadPlan.reservedBytes;
            sparseFrameUploadRemainingBytesLastFrame = sparseFrameUploadPlan.remainingBytes;
            sparseFrameUploadPlanDefersLastFrame = sparseFrameUploadPlan.byteLimitedDefers;
            sparseUploadRingBudgetDefersLastFrame += sparseFrameUploadPlan.byteLimitedDefers;
            const Simulation::BrickCoord sparseUploadFocus =
                Simulation::BrickCoord::FromWorldVoxel(
                    static_cast<int32_t>(std::floor(cameraPos.x)),
                    static_cast<int32_t>(std::floor(cameraPos.y - playerHeight)),
                    static_cast<int32_t>(std::floor(cameraPos.z)));
            if (sparseGpuPageTableResetPending) {
                SparsePageTableGpuUploadTicket resetTicket;
                if (!sparseFrameUploadPlan.allowPageTableReset) {
                    // The frame upload planner already counted this defer.
                } else if (!sparseGpuResources.CanStagePageTableReset()) {
                    ++sparseUploadRingBudgetDefersLastFrame;
                } else if (sparseGpuResources.StagePageTableReset(&resetTicket) &&
                    sparseGpuResources.EmitPageTableCopy(commandList.Get(), resetTicket)) {
                    sparseGpuPageTableResetPending = false;
                    spdlog::info("Sparse GPU page table reset uploaded");
                } else {
                    spdlog::warn("Sparse GPU page table reset upload failed; sparse visual path remains unsafe");
                }
            }
            for (uint32_t invalidationIndex = 0;
                 invalidationIndex < sparseFrameUploadPlan.invalidationBudget;
                 ++invalidationIndex) {
                if (!sparseGpuResources.CanStagePageTableEntry()) {
                    ++sparseUploadRingBudgetDefersLastFrame;
                    break;
                }
                Simulation::SparsePageInvalidationPacket invalidation;
                if (!sparseVoxelWorld.PopNextInvalidation(&invalidation)) {
                    break;
                }

                bool skipDelayedInvalidation = false;
                const auto& sparseCpuPageEntries = sparseVoxelWorld.GetPool().PageTable().Entries();
                Simulation::SparseDelayedInvalidationInput invalidationInput{};
                invalidationInput.cpuEntries = sparseCpuPageEntries.data();
                invalidationInput.cpuEntryCount = sparseCpuPageEntries.size();
                invalidationInput.entryIndex = invalidation.entryIndex;
                invalidationInput.coord = invalidation.coord;
                invalidationInput.pageIndex = invalidation.pageIndex;
                invalidationInput.generation = invalidation.generation;
                invalidationInput.replacementPublishPending =
                    sparsePagePublishQueue.ContainsEntry(invalidation.entryIndex);
                skipDelayedInvalidation =
                    Simulation::DecideSparseDelayedInvalidation(invalidationInput) ==
                    Simulation::SparseDelayedInvalidationDecision::SkipAlreadyReplaced;
                if (skipDelayedInvalidation) {
                    continue;
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
            uint32_t sparseUploadsPlannedThisFrame = 0;
            bool sparseUploadStoppedByCapacity = false;
            auto uploadOneSparseBrick = [&](bool requireClass, Simulation::SparseResidencyClass residencyClass) -> bool {
                if (sparseUploadsPlannedThisFrame >= sparseUploadBudgetThisFrame) {
                    return false;
                }
                Simulation::SparseBrickUploadPacket packet;
                const bool hasPacket = requireClass
                    ? sparseVoxelWorld.PopBestUploadForClass(
                        &packet,
                        residencyClass,
                        sparseUploadFocus,
                        sparseFrameIndexU32)
                    : sparseVoxelWorld.PopNextUpload(&packet, sparseFrameIndexU32);
                if (!hasPacket) {
                    return false;
                }
                if (requireClass) {
                    ++sparseValueSelectedUploadsLastFrame;
                }
                if (!sparseGpuResources.CanStageBrickUpload(packet)) {
                    if (sparseVoxelWorld.RequeueUploadFront(packet)) {
                        ++sparseUploadRequeuesLastFrame;
                    }
                    ++sparseUploadRingBudgetDefersLastFrame;
                    sparseUploadStoppedByCapacity = true;
                    return false;
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
                    sparseUploadStoppedByCapacity = true;
                    return false;
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
                    sparseUploadStoppedByCapacity = true;
                    return false;
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
                    sparseUploadStoppedByCapacity = true;
                    return false;
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
                    sparseUploadStoppedByCapacity = true;
                    return false;
                }

                // Publish the page-table entry on a later command list. This
                // keeps the renderer from seeing a BrickCoord until the payload,
                // occupancy, and generation copies are safely ordered ahead of
                // a subsequent frame's draw.
                enqueueSparsePageTablePublish(
                    pageTableEntryIndex,
                    packet.coord,
                    packet.pageIndex,
                    packet.generation,
                    packet.residencyClass);
                ++sparseUploadsPlannedThisFrame;
                return true;
            };
            auto pumpUploadClass = [&](Simulation::SparseResidencyClass residencyClass, uint32_t classBudget) {
                for (uint32_t i = 0;
                     i < classBudget &&
                     sparseUploadsPlannedThisFrame < sparseUploadBudgetThisFrame &&
                     !sparseUploadStoppedByCapacity;
                     ++i) {
                    if (!uploadOneSparseBrick(true, residencyClass)) {
                        break;
                    }
                }
            };

            sparseVoxelWorld.SetStatsRefreshDeferred(true);
            pumpUploadClass(
                Simulation::SparseResidencyClass::Edited,
                sparseFrameUploadPlan.brickBudgets.edited);
            pumpUploadClass(
                Simulation::SparseResidencyClass::Collision,
                sparseFrameUploadPlan.brickBudgets.collision);
            if (sparseFrameUploadPlan.brickBudgets.backgroundTotal > 0) {
                pumpUploadClass(
                    Simulation::SparseResidencyClass::Visible,
                    sparseFrameUploadPlan.brickBudgets.visible);
                pumpUploadClass(
                    Simulation::SparseResidencyClass::Speculative,
                    sparseFrameUploadPlan.brickBudgets.speculative);
            }
            while (sparseUploadsPlannedThisFrame < sparseFrameUploadPlan.brickBudgets.total &&
                   !sparseUploadStoppedByCapacity &&
                   uploadOneSparseBrick(false, Simulation::SparseResidencyClass::Speculative)) {
            }
            for (uint32_t publishIndex = 0;
                 publishIndex < sparseFrameUploadPlan.publishBudget &&
                 publishIndex < sparsePagePublishesEligibleThisFrame &&
                 !sparsePagePublishQueue.Empty();
                 ++publishIndex) {
                if (!sparseGpuResources.CanStagePageTableEntry()) {
                    ++sparseUploadRingBudgetDefersLastFrame;
                    break;
                }
                Simulation::SparsePendingPageTablePublish pendingPublish;
                if (!sparsePagePublishQueue.PopReady(
                        sparseFrameIndexU32,
                        sparseCompletedFenceForUpload,
                        &pendingPublish)) {
                    break;
                }
                const uint32_t entryIndex = pendingPublish.entryIndex;
                const auto& sparsePageTable = sparseVoxelWorld.GetPool().PageTable();
                if (entryIndex >= sparsePageTable.Entries().size()) {
                    continue;
                }
                const Simulation::BrickPageEntry& pageTableEntry = sparsePageTable.Entries()[entryIndex];
                if (pageTableEntry.pageIndex == Simulation::INVALID_BRICK_PAGE ||
                    pageTableEntry.generation == 0u) {
                    ++sparsePageTablePublishStaleDropsLastFrame;
                    continue;
                }
                if (!(pageTableEntry.coord == pendingPublish.coord) ||
                    pageTableEntry.pageIndex != pendingPublish.pageIndex ||
                    pageTableEntry.generation != pendingPublish.generation) {
                    ++sparsePageTablePublishStaleDropsLastFrame;
                    continue;
                }

                SparsePageTableGpuUploadTicket pageTableTicket;
                if (!sparseGpuResources.StagePageTableEntry(
                        entryIndex,
                        pageTableEntry,
                        &pageTableTicket) ||
                    !sparseGpuResources.EmitPageTableCopy(commandList.Get(), pageTableTicket)) {
                    sparsePagePublishQueue.RequeueFront(pendingPublish);
                    ++sparsePageTablePublishRetriesLastFrame;
                    break;
                }
                if (pendingPublish.residencyClass == Simulation::SparseResidencyClass::Edited) {
                    ++sparseEditedPageTablePublishesPublishedLastFrame;
                }
            }
            if (sparseClipmapTileCacheReady &&
                sparseClipmapPolicy.IsEnabled()) {
                if (!uploadHeightClipmapPending && !uploadVoxelClipmapPending) {
                    // Both clipmap layers already match the CPU cache.
                } else if (!sparseMidClipmapSnapshotReadyForUpload) {
                    ++sparseMidClipmapUploadRetriesLastFrame;
                } else if (!sparseFrameUploadPlan.allowMidClipmap) {
                    ++sparseMidClipmapUploadRetriesLastFrame;
                } else {
                    SparseMidClipmapGpuUploadTicket midTicket;
                    if (!sparseGpuResources.CanStageMidClipmapSnapshot(
                            sparseMidClipmapSnapshotForUpload,
                            uploadHeightClipmapPending,
                            uploadVoxelClipmapPending)) {
                        ++sparseUploadRingBudgetDefersLastFrame;
                        ++sparseMidClipmapUploadRetriesLastFrame;
                    } else if (sparseGpuResources.StageMidClipmapSnapshot(
                            sparseMidClipmapSnapshotForUpload,
                            &midTicket,
                            uploadHeightClipmapPending,
                            uploadVoxelClipmapPending) &&
                        sparseGpuResources.EmitMidClipmapCopy(commandList.Get(), midTicket)) {
                        if (uploadHeightClipmapPending) {
                            sparseMidClipmapUploadedHeightSerial = sparseClipmapTileCache.HeightDirtySerial();
                            sparseClipmapTileCache.ClearHeightDirtyRange();
                        }
                        if (uploadVoxelClipmapPending) {
                            sparseMidClipmapUploadedVoxelSerial = sparseClipmapTileCache.VoxelDirtySerial();
                            sparseClipmapTileCache.ClearVoxelDirtyRange();
                        }
                    } else {
                        ++sparseMidClipmapUploadRetriesLastFrame;
                    }
                }
            }
            sparseVoxelWorld.SetStatsRefreshDeferred(false);
            sparseVoxelWorld.FlushStats();
            sparseUploadRingUsedBytesLastFrame = sparseGpuResources.ActiveUploadBytesUsed();
            const auto& sparseStatsBeforeSurface = sparseVoxelWorld.GetStats();
            const bool sparseProtectedSurfaceBacklog =
                sparseStatsBeforeSurface.surfaceQueuedVisibleBricks > 0 ||
                sparseStatsBeforeSurface.surfaceQueuedCollisionBricks > 0 ||
                sparseStatsBeforeSurface.surfaceQueuedEditedBricks > 0;
            const Simulation::SparseRuntimeBudgetDecision sparseSurfaceRuntimeDecision =
                evaluateSparseBudgetFromStats(sparseStatsBeforeSurface);
            const uint32_t sparseSurfaceOwnershipPressureLevel =
                computeSparseEffectiveOwnershipPressureLevel();
            const bool sparseSurfaceCatchupActive =
                sparseResidencyCatchupFramesRemaining > 0 ||
                sparseSurfaceOwnershipPressureLevel > 0;
            const uint32_t surfaceExtractionBaseBudget =
                sparseSurfaceCatchupActive
                    ? std::max(
                        sparseSurfaceExtractionBudget,
                        sparseCatchupSurfaceBudget *
                            (std::clamp<uint32_t>(sparseSurfaceOwnershipPressureLevel, 1u, 3u) + 1u))
                    : sparseSurfaceExtractionBudget;
            const uint32_t surfaceExtractionBudgetThisFrame =
                Simulation::SparseRuntimeBudgetScheduler::BuildProcessingBudget(
                    surfaceExtractionBaseBudget,
                    sparseStatsBeforeSurface.surfaceExtractionQueuedBricks,
                    sparseProtectedSurfaceBacklog,
                    sparseSurfaceRuntimeDecision,
                    1u,
                    sparseSurfaceCatchupActive ? 8u : 4u);
            sparseSurfaceExtractionBudgetLastFrame = surfaceExtractionBudgetThisFrame;
            sparseVoxelWorld.PumpSurfaceExtractionAround(
                surfaceExtractionBudgetThisFrame,
                sparseUploadFocus,
                static_cast<uint32_t>(frameCount));

            if (sparseSurfaceGpuResources.IsInitialized()) {
                const bool useGpuSurfaceCulling = sparseSurfaceGpuResources.IsGpuCullEnabled();
                const uint32_t surfaceSerial = sparseVoxelWorld.GetSurfaceCache().GetStats().serial;
                const Simulation::BrickCoord surfaceCullCenter =
                    Simulation::BrickCoord::FromWorldVoxel(
                        static_cast<int32_t>(std::floor(cameraPos.x)),
                        static_cast<int32_t>(std::floor(cameraPos.y)),
                        static_cast<int32_t>(std::floor(cameraPos.z)));
                const bool cullCenterChanged =
                    !sparseSurfaceUploadedCullValid ||
                    surfaceCullCenter != sparseSurfaceUploadedCullCenter;
                const glm::vec3 normalizedCullForward = glm::normalize(cameraForward);
                const bool useCpuSurfaceSnapshotCulling =
                    (enableSparseSurfaceStableNearCull || enableSparseSurfaceCulling) &&
                    !useGpuSurfaceCulling;
                const glm::vec3 surfaceCullVelocity =
                    dt > 0.001f
                        ? (cameraPos - cameraPosBeforeInputMovement) / dt
                        : glm::vec3(0.0f);
                const float surfaceCullSpeed = glm::length(surfaceCullVelocity);
                const float surfaceCullPredictionSeconds = std::max(
                    0.25f,
                    static_cast<float>(sparsePredictivePrefetchMs) * 0.001f);
                const glm::vec3 surfaceCullLookahead =
                    cameraPos + surfaceCullVelocity * surfaceCullPredictionSeconds;
                const bool surfaceCullLookaheadActive =
                    surfaceCullSpeed >= sparseSurfaceCullMotionMinSpeed;
                const Simulation::BrickCoord surfaceCullLookaheadCenter =
                    Simulation::BrickCoord::FromWorldVoxel(
                        static_cast<int32_t>(std::floor(surfaceCullLookahead.x)),
                        static_cast<int32_t>(std::floor(surfaceCullLookahead.y)),
                        static_cast<int32_t>(std::floor(surfaceCullLookahead.z)));
                const bool cullDirectionChanged =
                    enableSparseSurfaceCulling &&
                    (!sparseSurfaceUploadedCullValid ||
                     glm::dot(normalizedCullForward, sparseSurfaceUploadedCullForward) < sparseSurfaceCullTurnDot);
                const bool cullLookaheadChanged =
                    useCpuSurfaceSnapshotCulling &&
                    surfaceCullLookaheadActive &&
                    (!sparseSurfaceUploadedCullValid ||
                     surfaceCullLookaheadCenter != sparseSurfaceUploadedCullLookaheadCenter);
                const bool cullIntervalExpired =
                    useCpuSurfaceSnapshotCulling &&
                    (frameCount - sparseSurfaceLastCullFrame) >= sparseSurfaceCullInterval;
                const bool needsSurfaceUpload = useGpuSurfaceCulling
                    ? (surfaceSerial != sparseSurfaceUploadedSerial ||
                       sparseSurfaceDeferredPayloadsLastUpload > 0u)
                    : (surfaceSerial != sparseSurfaceUploadedSerial ||
                       sparseSurfaceDeferredPayloadsLastUpload > 0u ||
                       cullCenterChanged ||
                       cullLookaheadChanged ||
                       cullDirectionChanged ||
                       cullIntervalExpired);
                if (needsSurfaceUpload) {
                    Simulation::SparseSurfaceGpuSnapshot surfaceSnapshot;
                    Simulation::SparseSurfaceVisibilityConfig surfaceVisibility;
                    surfaceVisibility.enabled = useCpuSurfaceSnapshotCulling;
                    surfaceVisibility.useFrustum =
                        enableSparseSurfaceCulling && !enableSparseSurfaceStableNearCull;
                    surfaceVisibility.cameraX = cameraPos.x;
                    surfaceVisibility.cameraY = cameraPos.y;
                    surfaceVisibility.cameraZ = cameraPos.z;
                    surfaceVisibility.forwardX = cameraForward.x;
                    surfaceVisibility.forwardY = cameraForward.y;
                    surfaceVisibility.forwardZ = cameraForward.z;
                    surfaceVisibility.rightX = cameraRight.x;
                    surfaceVisibility.rightY = cameraRight.y;
                    surfaceVisibility.rightZ = cameraRight.z;
                    surfaceVisibility.upX = cameraUp.x;
                    surfaceVisibility.upY = cameraUp.y;
                    surfaceVisibility.upZ = cameraUp.z;
                    surfaceVisibility.fovYRadians = fov;
                    surfaceVisibility.aspectRatio = aspectRatio;
                    surfaceVisibility.maxDistance = sparseSurfaceCullDistance;
                    surfaceVisibility.padding = sparseSurfaceCullPadding;
                    surfaceVisibility.useMotionLookahead =
                        surfaceCullLookaheadActive;
                    surfaceVisibility.lookaheadCameraX = surfaceCullLookahead.x;
                    surfaceVisibility.lookaheadCameraY = surfaceCullLookahead.y;
                    surfaceVisibility.lookaheadCameraZ = surfaceCullLookahead.z;
                    if (sparseVoxelWorld.GetSurfaceCache().BuildGpuSnapshot(
                            surfaceSnapshot,
                            useCpuSurfaceSnapshotCulling ? &surfaceVisibility : nullptr)) {
                        SparseSurfaceUploadTicket surfaceTicket;
                        if (sparseSurfaceGpuResources.StageSnapshot(surfaceSnapshot, &surfaceTicket) &&
                            sparseSurfaceGpuResources.EmitCopy(commandList.Get(), surfaceTicket)) {
                            sparseVoxelWorld.GetSurfaceCache().MarkGpuUploadComplete(
                                surfaceTicket.serial,
                                surfaceTicket.uploadedPayloadBricks,
                                surfaceTicket.removedBricks);
                            sparseSurfaceUploadedSerial = surfaceSnapshot.serial;
                            sparseSurfaceUploadedCullCenter = surfaceCullCenter;
                            sparseSurfaceUploadedCullLookaheadCenter = surfaceCullLookaheadCenter;
                            sparseSurfaceUploadedCullForward = normalizedCullForward;
                            sparseSurfaceUploadedCullValid = true;
                            sparseSurfaceLastCullFrame = frameCount;
                            sparseSurfaceDeferredPayloadsLastUpload = surfaceTicket.deferredPayloadBricks;
                            sparseSurfaceLookaheadVisibleLastUpload =
                                surfaceSnapshot.lookaheadVisibleBricks;
                        } else {
                            ++sparseSurfaceUploadRetriesLastFrame;
                        }
                    } else {
                        ++sparseSurfaceUploadRetriesLastFrame;
                        spdlog::warn("Sparse surface GPU snapshot build failed");
                    }
                }
            }
            if (sparseResidencyCatchupFramesRemaining > 0) {
                --sparseResidencyCatchupFramesRemaining;
                if (sparseResidencyCatchupFramesRemaining == 0) {
                    sparseOwnershipPressureLevelActive = 0;
                }
            }
        }
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 1);
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
        if (sparseRuntimeTestMode && sparseGpuResources.IsInitialized() && !sparseCpuRaycastAuthoritative) {
            const auto& sparseStats = sparseGpuResources.GetStats();
            physicsDispatcher->DispatchSparseRaycast(
                commandList.Get(),
                *voxelWorld,
                sparseGpuResources.BrickPoolSRV(),
                sparseGpuResources.PageTableSRV(),
                sparseGpuResources.OccupancySRV(),
                sparseGpuResources.PageGenerationSRV(),
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
        if (sparseRuntimeTestMode && sparseGpuResources.IsInitialized() && !sparseCpuRaycastAuthoritative) {
            const auto& sparseStats = sparseGpuResources.GetStats();
            physicsDispatcher->DispatchSparseRaycast(
                commandList.Get(),
                *voxelWorld,
                sparseGpuResources.BrickPoolSRV(),
                sparseGpuResources.PageTableSRV(),
                sparseGpuResources.OccupancySRV(),
                sparseGpuResources.PageGenerationSRV(),
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

        // Render pause menu and all UI panels (only when pause menu is open).
        // Capture smokes can suppress UI so visual gates inspect the renderer, not ImGui.
        if (!hideUiForCapture) {
            pauseMenu.Render(paused, frameCount, cameraPos, materialPalette, brushPanel, brushController);
        }

        // Debug logging removed to reduce spam

        // Get GPU raycast results (16 bytes from previous frame)
        auto gpuRaycastResult = voxelWorld->GetBrushRaycastResult();
        auto groundRaycastResult = voxelWorld->GetGroundRaycastResult();
        sparseGpuRaycastAcceptedLastFrame = 0;
        sparseGpuRaycastRejectedLastFrame = 0;
        sparseGpuRaycastMissLastFrame = 0;
        sparseGpuRaycastFallbackLastFrame = 0;
        sparseBrushStrokeDeltasLastFrame = 0;
        sparseBrushStrokeDeltaBricksLastFrame = 0;
        sparseBrushStrokeDeltaMismatchesLastFrame = 0;
        sparseBrushFeedbackQueuedLastFrame = 0;
        const bool sparseGpuBrushRaycastOwner =
            sparseVoxelWorldReady && enableSparseGpuRaycast;
        const bool sparseGroundAuthoritative = sparseBackendRequested && sparseVoxelWorldReady;
        if (sparseGroundAuthoritative) {
            const glm::vec3 feetWorld = cameraPos - glm::vec3(0.0f, playerHeight, 0.0f);
            const Simulation::SparseCollisionAabb footProbe{
                feetWorld.x - playerRadius,
                feetWorld.y + 0.25f,
                feetWorld.z - playerRadius,
                feetWorld.x + playerRadius,
                feetWorld.y + 0.35f,
                feetWorld.z + playerRadius
            };
            const auto sparseGround = sparseVoxelWorld.FindCollisionSupportBelow(footProbe, 512.0f);
            if (sparseGround.found) {
                groundRaycastResult.posX = static_cast<float>(sparseGround.supportX) + 0.5f;
                groundRaycastResult.posY = static_cast<float>(sparseGround.supportY) + 1.0f;
                groundRaycastResult.posZ = static_cast<float>(sparseGround.supportZ) + 0.5f;
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
        if (!sparseCpuRaycastAuthoritative &&
            hasCompletedBrushQuery &&
            gpuRaycastResult.hasValidPosition) {
            brushHitNormal = DecodePackedNormal(gpuRaycastResult.normalPacked, brushHitNormalValid);
            brushHitWorld = glm::vec3(
                gpuRaycastResult.posX,
                gpuRaycastResult.posY,
                gpuRaycastResult.posZ
            );

            float hitDistance = glm::length(brushHitWorld - completedBrushQueryOriginWorld);
            glm::vec3 brushHitCurrentLocal = sparseRuntimeTestMode
                ? brushHitWorld - regionOriginWorld
                : voxelWorld->WorldToRenderLocal(brushHitWorld);
            glm::vec3 queryHitDelta = brushHitWorld - completedBrushQueryOriginWorld;
            float queryRayDistance = glm::dot(queryHitDelta, completedBrushQueryDirectionWorld);
            glm::vec3 queryRayClosest = completedBrushQueryOriginWorld + completedBrushQueryDirectionWorld * queryRayDistance;
            float queryRayLateralError = glm::length(brushHitWorld - queryRayClosest);
            float queryRayTolerance = std::max(brushController.GetRadius() * 1.5f, queryRayDistance * 0.04f + 2.0f);
            const float minBrushHitDistance = 1.0f;
            const bool brushHitInsideActiveVolume =
                sparseRuntimeTestMode ||
                (brushHitCurrentLocal.x >= 0.0f && brushHitCurrentLocal.x < voxelWorld->GetGridSizeX() &&
                 brushHitCurrentLocal.y >= 0.0f && brushHitCurrentLocal.y < voxelWorld->GetGridSizeY() &&
                 brushHitCurrentLocal.z >= 0.0f && brushHitCurrentLocal.z < voxelWorld->GetGridSizeZ());
            brushHitValid =
                hitDistance > minBrushHitDistance &&
                hitDistance < kBrushMaxInteractionDistance &&
                queryRayDistance > minBrushHitDistance &&
                queryRayLateralError <= queryRayTolerance &&
                brushHitInsideActiveVolume;
            if (brushHitValid) {
                if (sparseGpuBrushRaycastOwner) {
                    sparseGpuRaycastAcceptedLastFrame = 1;
                }
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
            } else if (sparseGpuBrushRaycastOwner) {
                sparseGpuRaycastRejectedLastFrame = 1;
            }
        } else if (sparseGpuBrushRaycastOwner) {
            sparseGpuRaycastMissLastFrame = 1;
        }
        if (enableSparseOnlyRaymarch && sparseVoxelWorldReady && !brushHitValid && !enableSparseGpuRaycastStrict) {
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
                if (sparseGpuBrushRaycastOwner) {
                    sparseGpuRaycastFallbackLastFrame = 1;
                }
            }
        }
        if (sparseGpuBrushRaycastOwner && frameCount >= sparseGpuRaycastHealthReadyFrame) {
            sparseGpuRaycastAcceptedSinceReady += sparseGpuRaycastAcceptedLastFrame;
            sparseGpuRaycastRejectedSinceReady += sparseGpuRaycastRejectedLastFrame;
            sparseGpuRaycastMissSinceReady += sparseGpuRaycastMissLastFrame;
            sparseGpuRaycastFallbackSinceReady += sparseGpuRaycastFallbackLastFrame;
            const uint32_t gpuRaycastAuthoritativeSamples =
                sparseGpuRaycastAcceptedSinceReady + sparseGpuRaycastFallbackSinceReady;
            const uint32_t fallbackPct =
                gpuRaycastAuthoritativeSamples == 0u
                    ? 100u
                    : static_cast<uint32_t>(
                        (static_cast<uint64_t>(sparseGpuRaycastFallbackSinceReady) * 100ull) /
                        std::max<uint32_t>(1u, gpuRaycastAuthoritativeSamples));
            if (sparseGpuRaycastAcceptedSinceReady >= sparseGpuRaycastMinAccepted &&
                fallbackPct <= sparseGpuRaycastMaxFallbackPct) {
                if (!sparseGpuRaycastHealthObserved) {
                    spdlog::info(
                        "SPARSE_GPU_RAYCAST health observed at frame {}: accepted={} fallback={} rejected={} miss={} fallbackPct={} maxFallbackPct={} minAccepted={}",
                        frameCount,
                        sparseGpuRaycastAcceptedSinceReady,
                        sparseGpuRaycastFallbackSinceReady,
                        sparseGpuRaycastRejectedSinceReady,
                        sparseGpuRaycastMissSinceReady,
                        fallbackPct,
                        sparseGpuRaycastMaxFallbackPct,
                        sparseGpuRaycastMinAccepted);
                }
                sparseGpuRaycastHealthObserved = true;
            }
            if (requireSparseGpuRaycastHealth &&
                frameCount >= sparseGpuRaycastHealthReadyFrame + 120u &&
                !sparseGpuRaycastHealthObserved &&
                !sparseGpuRaycastHealthFailed) {
                sparseGpuRaycastHealthFailed = true;
                sparseGpuRaycastHealthFailureFrame = static_cast<uint32_t>(frameCount);
                sparseGpuRaycastFallbackPctAtFailure = fallbackPct;
                spdlog::critical(
                    "SPARSE_GPU_RAYCAST health failed at frame {}: accepted={} fallback={} rejected={} miss={} fallbackPct={} maxFallbackPct={} minAccepted={}",
                    frameCount,
                    sparseGpuRaycastAcceptedSinceReady,
                    sparseGpuRaycastFallbackSinceReady,
                    sparseGpuRaycastRejectedSinceReady,
                    sparseGpuRaycastMissSinceReady,
                    fallbackPct,
                    sparseGpuRaycastMaxFallbackPct,
                    sparseGpuRaycastMinAccepted);
                running = false;
            }
        }
        if (brushHitValid && !(enableSparseBodyCollision && sparseGroundAuthoritative)) {
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
        if (gameplayInputEnabled &&
            !flightMode &&
            !(enableSparseBodyCollision && sparseGroundAuthoritative)) {
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
        bool brushPlacementPreviewValid =
            gameplayInputEnabled && brushHitValid && brushHitTracksCurrentRay;
        bool brushPlacementCloseRamp = false;

        // Run physics simulation only after the streamed render window is
        // populated. Scanning the full vertical buffer while chunks are still
        // sparse can stall startup; traversal responsiveness is more important
        // than simulating incomplete terrain.
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
        uint32_t sparsePhysicsFrameBrickBudget = sparsePhysicsBrickBudget;
        uint32_t sparsePhysicsFrameMoveBudget = sparsePhysicsMoveBudget;
        if (enableSparseLocalPhysics && sparseVoxelWorldReady) {
            const auto& preSparsePhysicsStats = sparseVoxelWorld.GetStats();
            Simulation::SparseRuntimeBudgetDecision sparsePhysicsRuntimeDecision{};
            sparsePhysicsRuntimeDecision.scale = sparseRuntimeBudgetScale;
            sparsePhysicsRuntimeDecision.protectedScale = sparseProtectedRuntimeBudgetScale;
            sparsePhysicsRuntimeDecision.backgroundScale = sparseBackgroundRuntimeBudgetScale;
            sparsePhysicsRuntimeDecision.pressureClass = sparseRuntimePressureClass;
            const Simulation::SparsePhysicsBudgetDecision sparsePhysicsBudgets =
                Simulation::SparseRuntimeBudgetScheduler::BuildPhysicsBudgets(
                    sparsePhysicsBrickBudget,
                    sparsePhysicsMoveBudget,
                    preSparsePhysicsStats.physicsCandidateBricks,
                    preSparsePhysicsStats.physicsHotCandidateBricks +
                        (physicsDirtyFramesRemaining > 0 ? 1u : 0u),
                    sparsePhysicsRuntimeDecision);
            sparsePhysicsFrameBrickBudget = sparsePhysicsBudgets.brickBudget;
            sparsePhysicsFrameMoveBudget = sparsePhysicsBudgets.moveBudget;
        }
        sparsePhysicsBrickBudgetLastFrame = sparsePhysicsFrameBrickBudget;
        sparsePhysicsMoveBudgetLastFrame = sparsePhysicsFrameMoveBudget;
        const bool physicsDueThisFrame = (frameCount % physicsInterval) == 0;
        perfPhaseStart = SDL_GetPerformanceCounter();
        if (!paused && enableSparseLocalPhysics && sparseVoxelWorldReady && physicsDueThisFrame) {
            if (enableSparsePhysicsDiagnosticSeed && !sparsePhysicsDiagnosticSeedQueued) {
                const int32_t seedX = static_cast<int32_t>(std::floor(cameraPos.x));
                const int32_t seedY = static_cast<int32_t>(std::floor(cameraPos.y + 64.0f));
                const int32_t seedZ = static_cast<int32_t>(std::floor(cameraPos.z));
                if (enableSparsePhysicsDiagnosticFluidSeed) {
                    sparseVoxelWorld.SetEditedVoxel(
                        seedX,
                        seedY - 1,
                        seedZ,
                        Utils::PackVoxel(Utils::Material::Stone, 0, 0, Utils::StateFlags::IsStatic));
                    sparseVoxelWorld.SetEditedVoxel(
                        seedX + 1,
                        seedY,
                        seedZ,
                        Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));
                    sparseVoxelWorld.SetEditedVoxel(
                        seedX,
                        seedY,
                        seedZ,
                        Utils::PackVoxel(Utils::Material::Water, 0, 0, 0));
                } else {
                    sparseVoxelWorld.SetEditedVoxel(
                        seedX,
                        seedY,
                        seedZ,
                        Utils::PackVoxel(Utils::Material::Sand, 0, 0, 0));
                }
                sparsePhysicsDiagnosticSeedQueued = true;
                spdlog::info(
                    "Sparse physics diagnostic {}seed queued at world voxel {},{},{}",
                    enableSparsePhysicsDiagnosticFluidSeed ? "fluid " : "",
                    seedX,
                    seedY,
                    seedZ);
            }
            sparseVoxelWorld.StageLocalPhysicsWork(sparsePhysicsFrameBrickBudget);
            if (enableSparsePhysicsPacketUpload && sparseGpuResources.IsInitialized()) {
                SparsePhysicsPacketGpuUploadTicket physicsPacketTicket;
                SparseEditDeltaGpuUploadTicket editDeltaTicket;
                uint32_t uploadedEditDeltaCount = 0;
                uint32_t uploadedEditDeltaRangeCount = 0;
                uint32_t uploadedEditDeltaRangeTableCapacity = 0;
                std::vector<Simulation::SparseEditDelta> physicsEditDeltaSnapshot;
                if (enableSparsePhysicsGpu) {
                    const auto& pendingEditDeltas = sparseVoxelWorld.GetPendingGpuEditDeltas();
                    const std::vector<Simulation::SparseEditDelta>* editDeltasForUpload = &pendingEditDeltas;
                    bool editDeltasSourceIsPendingQueue = true;
                    if (enableSparsePhysicsGpuApply) {
                        physicsEditDeltaSnapshot =
                            sparseVoxelWorld.BuildGpuEditDeltaSnapshotForPhysicsWork(8192u);
                        editDeltasForUpload = &physicsEditDeltaSnapshot;
                        editDeltasSourceIsPendingQueue = false;
                        // GPU-apply physics samples edit overlays from a per-work snapshot.
                        // The global pending queue is only a non-apply upload source, so
                        // clear it here to avoid retaining stale historical brush deltas.
                        sparseVoxelWorld.ClearPendingGpuEditDeltas();
                    }
                    const size_t editDeltaUploadSourceCount = editDeltasForUpload->size();
                    if (!editDeltasForUpload->empty() &&
                        sparseGpuResources.CanStageEditDeltas(*editDeltasForUpload) &&
                        sparseGpuResources.StageEditDeltas(*editDeltasForUpload, &editDeltaTicket)) {
                        if (!sparseGpuResources.EmitEditDeltaCopy(commandList.Get(), editDeltaTicket)) {
                            spdlog::warn(
                                "Sparse edit-delta GPU upload emit failed for {} deltas",
                                editDeltaTicket.deltaCount);
                        } else {
                            uploadedEditDeltaCount = editDeltaTicket.deltaCount;
                            uploadedEditDeltaRangeCount = editDeltaTicket.rangeCount;
                            uploadedEditDeltaRangeTableCapacity = editDeltaTicket.rangeTableCapacity;
                            if (editDeltasSourceIsPendingQueue &&
                                uploadedEditDeltaCount >= editDeltaUploadSourceCount) {
                                sparseVoxelWorld.ClearPendingGpuEditDeltas();
                            }
                        }
                    } else if (!editDeltasForUpload->empty()) {
                        ++sparseUploadRingBudgetDefersLastFrame;
                    }
                }
                const auto& physicsWorkPackets = sparseVoxelWorld.GetLastPhysicsWorkPackets();
                if (!physicsWorkPackets.empty() &&
                    sparseGpuResources.CanStagePhysicsWorkPackets(physicsWorkPackets) &&
                    sparseGpuResources.StagePhysicsWorkPackets(
                        physicsWorkPackets,
                        &physicsPacketTicket)) {
                    if (!sparseGpuResources.EmitPhysicsPacketCopy(commandList.Get(), physicsPacketTicket)) {
                        spdlog::warn(
                            "Sparse physics packet GPU upload emit failed for {} packets",
                            physicsPacketTicket.packetCount);
                    } else if (enableSparsePhysicsGpu) {
                        sparseGpuResources.PreparePhysicsPacketResultsWrite(commandList.Get());
                        sparseGpuResources.PreparePhysicsDiagnosticsWrite(commandList.Get());
                        physicsDispatcher->DispatchSparsePhysicsPackets(
                            commandList.Get(),
                            sparseGpuResources.PhysicsWorkPacketsSRV(),
                            sparseGpuResources.PageTableSRV(),
                            sparseGpuResources.BrickPoolSRV(),
                            sparseGpuResources.EditDeltasSRV(),
                            sparseGpuResources.EditDeltaRangesSRV(),
                            sparseGpuResources.EditDeltaRangeTableSRV(),
                            sparseGpuResources.PhysicsPacketResultsUAV(),
                            sparseGpuResources.PhysicsDiagnosticsUAV(),
                            sparseGpuResources.GetStats().pageTableCapacity,
                            physicsPacketTicket.packetCount,
                            uploadedEditDeltaCount,
                            uploadedEditDeltaRangeCount,
                            uploadedEditDeltaRangeTableCapacity,
                            static_cast<uint32_t>(frameCount));
                        sparseGpuResources.QueuePhysicsPacketResultsReadback(
                            commandList.Get(),
                            static_cast<uint32_t>(frameCount));
                        sparseGpuResources.QueuePhysicsDiagnosticsReadback(
                            commandList.Get(),
                            static_cast<uint32_t>(frameCount));
                    }
                } else if (!physicsWorkPackets.empty()) {
                    ++sparseUploadRingBudgetDefersLastFrame;
                }
            }
            if (!enableSparsePhysicsGpuApply) {
                const uint32_t sparsePhysicsMoved = sparseVoxelWorld.ExecuteStagedLocalPhysics(
                    sparsePhysicsFrameMoveBudget,
                    true);
                if (sparsePhysicsMoved > 0) {
                    physicsRanThisFrame = true;
                    physicsDispatchCount++;
                }
            }
        } else if (!paused && !disableRuntimePhysics && infinitePhysicsAllowed && streamingReadyForPhysics && physicsDueThisFrame) {
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
            brushPlacementPreviewValid = gameplayInputEnabled;
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
            uint32_t sparseBrushStrokeDeltasThisFrame = 0;
            uint32_t sparseBrushStrokeDeltaMismatchesThisFrame = 0;
            std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> sparseBrushStrokeDeltaBricks;
            const bool sparseBrushFeedbackRecording =
                enableSparseBrushFeedback &&
                sparseGpuResources.IsInitialized() &&
                sparseGpuResources.BrushFeedbackUAV().IsValid();
            if (sparseBrushFeedbackRecording) {
                sparseGpuResources.PrepareBrushFeedbackWrite(commandList.Get());
            }
            glm::vec3 lastSubmittedBrushWorldPosition = buildStrokeState.hasLastBrushWorldPosition
                ? buildStrokeState.lastBrushWorldPosition
                : brushPlacementWorld;
            auto recordSparseBrushStamp = [&](const glm::vec3& worldPosition, uint32_t stampSeed) {
                if (!sparseBackendRequested || !sparseVoxelWorldReady) {
                    return;
                }
                std::vector<Simulation::SparseEditDelta> sparseBrushDeltas;
                const bool useGpuAuthoritativeBrush =
                    sparseBrushFeedbackRecording && enableSparseBrushFeedbackAuthoritative;
                const uint32_t sparseEditedVoxels = useGpuAuthoritativeBrush
                    ? sparseVoxelWorld.PreviewBrushEdit(
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
                        &sparseBrushDeltas)
                    : sparseVoxelWorld.ApplyBrushEdit(
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
                        true,
                        &sparseBrushDeltas);
                sparseBrushStrokeDeltasThisFrame += static_cast<uint32_t>(sparseBrushDeltas.size());
                if (sparseBrushDeltas.size() != sparseEditedVoxels) {
                    ++sparseBrushStrokeDeltaMismatchesThisFrame;
                }
                for (const auto& delta : sparseBrushDeltas) {
                    sparseBrushStrokeDeltaBricks.insert(delta.coord);
                }
                if (sparseBrushFeedbackRecording) {
                    if (useGpuAuthoritativeBrush && sparseEditedVoxels != 0u) {
                        sparseBrushFeedbackPendingStrokes.push_back({
                            static_cast<uint32_t>(frameCount),
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
                            brushConstants.hasHitNormal != 0
                        });
                    }
                    const auto& gpuStats = sparseGpuResources.GetStats();
                    physicsDispatcher->DispatchSparseBrushFeedback(
                        commandList.Get(),
                        sparseGpuResources.BrickPoolSRV(),
                        sparseGpuResources.PageTableSRV(),
                        sparseGpuResources.OccupancySRV(),
                        sparseGpuResources.PageGenerationSRV(),
                        sparseGpuResources.BrushFeedbackUAV(),
                        gpuStats.maxBrickPages,
                        gpuStats.pageTableCapacity,
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
                        sparseBrushFeedbackMaxRecords,
                        static_cast<uint32_t>(frameCount));
                    ++sparseBrushFeedbackQueuedLastFrame;
                }
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
            sparseBrushStrokeDeltasLastFrame = sparseBrushStrokeDeltasThisFrame;
            sparseBrushStrokeDeltaBricksLastFrame =
                static_cast<uint32_t>(sparseBrushStrokeDeltaBricks.size());
            sparseBrushStrokeDeltaMismatchesLastFrame =
                sparseBrushStrokeDeltaMismatchesThisFrame;
            if (sparseBrushFeedbackRecording && sparseBrushFeedbackQueuedLastFrame > 0) {
                sparseGpuResources.QueueBrushFeedbackReadback(
                    commandList.Get(),
                    static_cast<uint32_t>(frameCount));
            }
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
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 2);
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
        cameraParams.frameIndex = static_cast<uint32_t>(frameCount);
        cameraParams.renderOwnershipStatsEnabled =
            enableSparseRenderOwnershipStats &&
            sparseGpuResources.IsInitialized() &&
            (sparseRenderOwnershipInterval <= 1u ||
             (frameCount % sparseRenderOwnershipInterval) == 0u);
        const auto sparseTransitionMetadata = sparseClipmapPolicy.BuildTransitionMetadata();
        cameraParams.midFieldStartDistance = sparseTransitionMetadata.startDistance;
        cameraParams.midFieldEndDistance = sparseTransitionMetadata.endDistance;
        cameraParams.midFieldCellSize = sparseTransitionMetadata.minCellSize;
        cameraParams.midFieldFarHandoffDistance = sparseTransitionMetadata.farHandoffDistance;
        const auto sparseMidResidencyMetadata =
            Simulation::BuildClipmapResidencyMetadata(sparseClipmapTileCache.GetStats());
        cameraParams.midFieldHeightCoverage = sparseMidResidencyMetadata.heightCoverageRatio;
        cameraParams.midFieldVoxelCoverage = sparseMidResidencyMetadata.voxelCoverageRatio;
        cameraParams.midFieldResidentHeightTiles = sparseMidResidencyMetadata.residentHeightTiles;
        cameraParams.midFieldResidentVoxelBricks = sparseMidResidencyMetadata.residentVoxelBricks;
        cameraParams.debugMode = sparseRaymarchDebugMode;

        // Build brush preview params from GPU raycast result (NEW!)
        Graphics::Renderer::BrushPreview brushPreview = {};
        if (!hideUiForCapture && (brushPlacementPreviewValid || brushHitValid)) {
            const glm::vec3 previewWorld = brushPlacementPreviewValid
                ? (buildStrokeState.hasPreviewWorldPosition ? buildStrokeState.previewWorldPosition : brushPlacementWorld)
                : brushHitWorld;
            const float previewRadius = brushController.GetRadius();
            const glm::vec3 cameraToPreview = previewWorld - renderCameraPos;
            const float previewDistance = glm::length(cameraToPreview);
            const bool previewFinite =
                std::isfinite(previewWorld.x) &&
                std::isfinite(previewWorld.y) &&
                std::isfinite(previewWorld.z) &&
                std::isfinite(previewDistance) &&
                std::isfinite(previewRadius);
            const bool previewInFront =
                previewDistance > 0.001f &&
                glm::dot(cameraToPreview / previewDistance, cameraForward) > 0.05f;
            const bool previewTooClose =
                previewDistance < std::max(previewRadius * 3.75f, 12.0f);
            if (previewFinite && previewRadius > 0.0f && previewInFront && !previewTooClose) {
                brushPreview.posX = previewWorld.x;
                brushPreview.posY = previewWorld.y;
                brushPreview.posZ = previewWorld.z;
                brushPreview.radius = previewRadius;
                brushPreview.material = brushController.GetMaterial();
                brushPreview.shape = static_cast<uint32_t>(brushController.GetShape());
                brushPreview.hasValidPosition = true;
            } else {
                brushPreview.hasValidPosition = false;
            }
        } else {
            brushPreview.hasValidPosition = false;
        }

        Graphics::Renderer::CharacterPreview characterPreview = {};
        characterPreview.feetX = playerFeetWorld.x;
        characterPreview.feetY = playerFeetWorld.y;
        characterPreview.feetZ = playerFeetWorld.z;
        characterPreview.visible = !hideUiForCapture && thirdPersonMode;

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
        uint32_t sparseBackendConfiguredMaskThisFrame = 0;
        uint32_t sparseBackendActiveMaskThisFrame = 0;
        uint32_t sparseBackendWarnMaskThisFrame = 0;
        if (sparseBackendRequested) {
            sparseBackendConfiguredMaskThisFrame |= kBackendPipeCpuWorld;
            if (sparseVoxelWorldReady) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeCpuWorld;
            }
            sparseBackendConfiguredMaskThisFrame |= kBackendPipeGpuResources;
            if (sparseGpuResources.IsInitialized()) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeGpuResources;
            }
            if (enableSparseRaymarch) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeRaymarch;
            }
            if (enableSparseNearBinding) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeNearBinding;
            }
            if (enableSparseSurfaceUpload) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeSurfaceGpu;
            }
            if (enableSparseSurfaceRaster) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeSurfaceRaster;
            }
            if (enableSparseSurfaceAuthoritative) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeSurfaceAuthoritative;
            }
            if (sparseClipmapPolicy.IsEnabled()) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeMidClipmap;
            }
            if (enableFarSVO) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeFarSvo;
            }
            if (enableSparseRenderOwnershipStats) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeOwnership;
            }
            if (enableSparseBodyCollision) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipeCollision;
            }
            if (enableSparseLocalPhysics) {
                sparseBackendConfiguredMaskThisFrame |= kBackendPipePhysics;
            }
        }
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
            sparseNearField.renderOwnershipUAV = sparseGpuResources.RenderOwnershipUAV();
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
            sparseNearField.surfaceAuthoritative =
                enableSparseSurfaceAuthoritative &&
                enableSparseSurfaceRaster;
            sparseNearField.surfaceRaymarchFill =
                enableSparseSurfaceRaymarchFill;
            sparseNearField.ownershipCenterX = renderCameraPos.x;
            sparseNearField.ownershipCenterY = renderCameraPos.y;
            sparseNearField.ownershipCenterZ = renderCameraPos.z;
            sparseNearField.ownershipRadius = enableSparseSurfaceStableNearCull
                ? sparseSurfaceCullDistance
                : std::max(cameraParams.midFieldStartDistance, 896.0f);
            sparseNearField.midClipmapEnabled =
                sparseClipmapPolicy.IsEnabled() &&
                sparseClipmapTileCacheReady &&
                (sparseMidClipmapUploadedHeightSerial != 0 ||
                 sparseMidClipmapUploadedVoxelSerial != 0);
            if (sparseNearField.enabled) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeRaymarch;
            }
            if (sparseNearField.enabled && sparseNearField.brickPoolSRV.IsValid() &&
                sparseNearField.pageTableSRV.IsValid() &&
                sparseNearField.occupancySRV.IsValid() &&
                sparseNearField.pageGenerationSRV.IsValid()) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeNearBinding;
            }
            if (sparseSurfaceGpuResources.IsInitialized()) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeSurfaceGpu;
            }
            if (sparseNearField.surfaceAuthoritative) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeSurfaceAuthoritative;
            }
            if (sparseNearField.midClipmapEnabled) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeMidClipmap;
            }
            if (sparseFarField.enabled) {
                sparseBackendActiveMaskThisFrame |= kBackendPipeFarSvo;
            }
        }
        Simulation::SparseMissFeedbackPlanInput missFeedbackPlanInput{};
        missFeedbackPlanInput.enabled = enableSparseMissFeedback;
        missFeedbackPlanInput.frameIndex = static_cast<uint32_t>(frameCount);
        missFeedbackPlanInput.baseInterval = sparseMissFeedbackInterval;
        missFeedbackPlanInput.baseRayGrid = sparseMissFeedbackRayGrid;
        missFeedbackPlanInput.baseDistance = sparseMissFeedbackDistance;
        missFeedbackPlanInput.baseStride = sparseMissFeedbackStride;
        missFeedbackPlanInput.maxRecords = sparseMissFeedbackMaxRecords;
        missFeedbackPlanInput.unsafeNearMissPercent = sparseOwnershipUnsafeNearMissPctLastRetire;
        missFeedbackPlanInput.ownershipPressureLevel = computeSparseEffectiveOwnershipPressureLevel();
        missFeedbackPlanInput.staleReadbackDrops =
            sparseGpuResources.GetStats().missFeedbackStaleFrameDropsLastRetire;
        missFeedbackPlanInput.overflowLastRetire =
            sparseGpuResources.GetStats().missFeedbackOverflowLastRetire;
        missFeedbackPlanInput.pendingRecords =
            static_cast<uint32_t>(std::min<size_t>(
                sparseMissFeedbackPending.size(),
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        const Simulation::SparseMissFeedbackPlan missFeedbackPlan =
            Simulation::SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missFeedbackPlanInput);
        sparseMissFeedbackUrgentLastFrame = missFeedbackPlan.urgent;
        sparseMissFeedbackRayGridLastFrame = missFeedbackPlan.rayGrid;
        sparseMissFeedbackDistanceLastFrame = missFeedbackPlan.distance;
        sparseMissFeedbackStrideLastFrame = missFeedbackPlan.stride;
        if (missFeedbackPlan.dispatch &&
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
                static_cast<float>(missFeedbackPlan.distance),
                static_cast<float>(missFeedbackPlan.stride),
                missFeedbackPlan.rayGrid,
                missFeedbackPlan.maxRecords,
                static_cast<uint32_t>(frameCount));
            sparseGpuResources.QueueMissFeedbackReadback(
                commandList.Get(),
                static_cast<uint32_t>(frameCount));
        }
        if (enableSparseBrushFeedbackDiagnosticSeed &&
            !sparseBrushFeedbackParityPending &&
            sparseBrushFeedbackDiagnosticStage < 8u &&
            frameCount >= sparseBrushFeedbackDiagnosticNextFrame &&
            sparseGpuResources.IsInitialized() &&
            sparseNearField.brickPoolSRV.IsValid() &&
            sparseNearField.pageTableSRV.IsValid() &&
            sparseNearField.occupancySRV.IsValid() &&
            sparseNearField.pageGenerationSRV.IsValid() &&
            sparseGpuResources.BrushFeedbackUAV().IsValid()) {
            if (!sparseBrushFeedbackDiagnosticCenterPinned) {
                sparseBrushFeedbackDiagnosticCenter = glm::vec3(
                    cameraPos.x,
                    cameraPos.y - playerHeight - 1.0f,
                    cameraPos.z);
                sparseBrushFeedbackDiagnosticCenterPinned = true;
            }
            const glm::vec3 diagnosticCenter = sparseBrushFeedbackDiagnosticCenter;
            struct SparseBrushFeedbackDiagnosticCase {
                const char* label = "";
                uint32_t mode = 0;
                uint32_t material = 0;
                bool centerOnBrickBoundary = false;
                bool negativeResident = false;
                bool prepareOnly = false;
                bool missingResidentOnly = false;
            };
            const std::array<SparseBrushFeedbackDiagnosticCase, 8> diagnosticCases = {{
                { "replace-solid", 2u, Utils::Material::Glass, false, false, false, false },
                { "erase-edited-solid", 1u, Utils::Material::Air, false, false, false, false },
                { "paint-edited-air", 0u, Utils::Material::Sand, false, false, false, false },
                { "reject-solid-paint", 0u, Utils::Material::Sand, false, false, false, false },
                { "replace-brick-boundary", 2u, Utils::Material::Glass, true, false, false, false },
                { "prepare-negative-resident", 3u, Utils::Material::Stone, false, true, true, false },
                { "replace-negative-resident", 2u, Utils::Material::Glass, false, true, false, false },
                { "report-missing-nonresident", 2u, Utils::Material::Glass, false, false, false, true },
            }};
            const SparseBrushFeedbackDiagnosticCase& diagnosticCase =
                diagnosticCases[sparseBrushFeedbackDiagnosticStage];
            glm::vec3 caseCenter = diagnosticCenter;
            if (diagnosticCase.centerOnBrickBoundary) {
                caseCenter.x =
                    std::floor(caseCenter.x / static_cast<float>(Simulation::SPARSE_BRICK_SIZE)) *
                    static_cast<float>(Simulation::SPARSE_BRICK_SIZE) +
                    static_cast<float>(Simulation::SPARSE_BRICK_SIZE) - 0.5f;
            }
            if (diagnosticCase.negativeResident) {
                caseCenter.x = -16.5f;
                caseCenter.z = -16.5f;
            }
            if (diagnosticCase.missingResidentOnly) {
                caseCenter = glm::vec3(32768.5f, caseCenter.y, 32768.5f);
            }
            std::vector<Simulation::SparseEditDelta> expectedDeltas;
            if (!diagnosticCase.missingResidentOnly) {
                sparseVoxelWorld.PreviewBrushEdit(
                    caseCenter.x,
                    caseCenter.y,
                    caseCenter.z,
                    1.5f,
                    diagnosticCase.material,
                    diagnosticCase.mode,
                    0u,
                    1.0f,
                    static_cast<uint32_t>(frameCount),
                    0,
                    1,
                    0,
                    false,
                    &expectedDeltas);
                const bool diagnosticCommitsCpuBeforeGpu =
                    diagnosticCase.prepareOnly || !enableSparseBrushFeedbackAuthoritative;
                if (diagnosticCommitsCpuBeforeGpu) {
                    sparseVoxelWorld.ApplyBrushEdit(
                    caseCenter.x,
                    caseCenter.y,
                    caseCenter.z,
                    1.5f,
                    diagnosticCase.material,
                    diagnosticCase.mode,
                    0u,
                    1.0f,
                    static_cast<uint32_t>(frameCount),
                    0,
                    1,
                    0,
                    false,
                    diagnosticCase.prepareOnly,
                    nullptr);
                }
            }
            if (diagnosticCase.prepareOnly) {
                sparseBrushFeedbackDiagnosticNextFrame = static_cast<uint32_t>(frameCount + 60u);
                ++sparseBrushFeedbackDiagnosticStage;
                spdlog::info(
                    "Sparse brush feedback diagnostic prepared case={} center={:.1f},{:.1f},{:.1f} edits={} waitUntilFrame={}",
                    diagnosticCase.label,
                    caseCenter.x,
                    caseCenter.y,
                    caseCenter.z,
                    expectedDeltas.size(),
                    sparseBrushFeedbackDiagnosticNextFrame);
            } else {
            sparseBrushFeedbackParityExpected.clear();
            sparseBrushFeedbackParityExpected.reserve(expectedDeltas.size());
            for (const auto& delta : expectedDeltas) {
                const Simulation::LocalVoxelCoord local =
                    Simulation::UnpackSparseEditLocal(delta.packedLocal);
                sparseBrushFeedbackParityExpected.push_back({
                    delta.coord.x * Simulation::SPARSE_BRICK_SIZE + static_cast<int32_t>(local.x),
                    delta.coord.y * Simulation::SPARSE_BRICK_SIZE + static_cast<int32_t>(local.y),
                    delta.coord.z * Simulation::SPARSE_BRICK_SIZE + static_cast<int32_t>(local.z),
                    delta.voxel
                });
            }
            sparseBrushFeedbackParityPending = !sparseBrushFeedbackParityExpected.empty();
            if (sparseBrushFeedbackDiagnosticStage == 3u) {
                sparseBrushFeedbackParityPending = true;
            }
            if (diagnosticCase.missingResidentOnly) {
                sparseBrushFeedbackParityPending = true;
            }
            sparseBrushFeedbackParityObserved = false;
            sparseBrushFeedbackParityFailed = false;
            sparseBrushFeedbackParityExpectsMissingResident = diagnosticCase.missingResidentOnly;
            sparseBrushFeedbackParityExpectedFrame = static_cast<uint32_t>(frameCount);
            sparseBrushFeedbackParityLabel = diagnosticCase.label;
            sparseBrushFeedbackParityExpectedLastFrame =
                static_cast<uint32_t>(sparseBrushFeedbackParityExpected.size());
            sparseBrushFeedbackParityMatchedLastFrame = 0;
            sparseBrushFeedbackParityMissingLastFrame = 0;
            sparseBrushFeedbackParityUnexpectedLastFrame = 0;
            sparseBrushFeedbackParityValueMismatchLastFrame = 0;
            sparseGpuResources.PrepareBrushFeedbackWrite(commandList.Get());
            if (enableSparseBrushFeedbackAuthoritative &&
                !diagnosticCase.missingResidentOnly &&
                !sparseBrushFeedbackParityExpected.empty()) {
                sparseBrushFeedbackPendingStrokes.push_back({
                    static_cast<uint32_t>(frameCount),
                    caseCenter.x,
                    caseCenter.y,
                    caseCenter.z,
                    1.5f,
                    diagnosticCase.material,
                    diagnosticCase.mode,
                    0u,
                    1.0f,
                    static_cast<uint32_t>(frameCount),
                    0,
                    1,
                    0,
                    false
                });
            }
            physicsDispatcher->DispatchSparseBrushFeedback(
                commandList.Get(),
                sparseNearField.brickPoolSRV,
                sparseNearField.pageTableSRV,
                sparseNearField.occupancySRV,
                sparseNearField.pageGenerationSRV,
                sparseGpuResources.BrushFeedbackUAV(),
                sparseNearField.maxBrickPages,
                sparseNearField.pageTableCapacity,
                caseCenter.x,
                caseCenter.y,
                caseCenter.z,
                1.5f,
                diagnosticCase.material,
                diagnosticCase.mode,
                0u,
                1.0f,
                static_cast<uint32_t>(frameCount),
                0,
                1,
                0,
                false,
                sparseBrushFeedbackMaxRecords,
                static_cast<uint32_t>(frameCount));
            sparseGpuResources.QueueBrushFeedbackReadback(
                commandList.Get(),
                static_cast<uint32_t>(frameCount));
            sparseBrushFeedbackDiagnosticQueued = true;
            ++sparseBrushFeedbackDiagnosticStage;
            ++sparseBrushFeedbackQueuedLastFrame;
            spdlog::info(
                "Sparse brush feedback diagnostic queued case={} center={:.1f},{:.1f},{:.1f} expected={}",
                diagnosticCase.label,
                caseCenter.x,
                caseCenter.y,
                caseCenter.z,
                sparseBrushFeedbackParityExpected.size());
            }
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
        if (cameraParams.renderOwnershipStatsEnabled) {
            sparseGpuResources.PrepareRenderOwnershipWrite(commandList.Get());
            sparseBackendActiveMaskThisFrame |= kBackendPipeOwnership;
        }

        const auto renderSparseSurfaceLayer = [&]() {
            if (!enableSparseSurfaceRaster ||
                !sparseNearField.surfaceEnabled ||
                sparseNearField.surfaceFaceCount == 0) {
                return;
            }
            sparseBackendActiveMaskThisFrame |= kBackendPipeSurfaceRaster;
            const auto& sparseSurfaceStats = sparseSurfaceGpuResources.GetStats();
            const bool useSparseSurfaceGpuCull =
                sparseSurfaceGpuResources.IsGpuCullEnabled() &&
                sparseSurfaceStats.uploadedSurfaceRecords > 0;
            bool sparseSurfaceGpuCullReady = false;
            if (useSparseSurfaceGpuCull) {
                sparseSurfaceGpuCullReady = sparseSurfaceGpuResources.DispatchGpuCull(
                    commandList.Get(),
                    cameraParams.posX,
                    cameraParams.posY,
                    cameraParams.posZ,
                    cameraParams.forwardX,
                    cameraParams.forwardY,
                    cameraParams.forwardZ,
                    cameraParams.rightX,
                    cameraParams.rightY,
                    cameraParams.rightZ,
                    cameraParams.upX,
                    cameraParams.upY,
                    cameraParams.upZ,
                    fov,
                    aspectRatio,
                    sparseSurfaceCullDistance,
                    sparseSurfaceCullPadding);
            }
            const bool useSparseSurfaceIndirect =
                enableSparseSurfaceIndirect &&
                sparseSurfaceStats.uploadedDrawCommands > 0 &&
                sparseSurfaceGpuResources.DrawArgsResource() != nullptr;
            renderer->RenderSparseSurfaceFaces(
                commandList.Get(),
                sparseNearField.surfaceFacesSRV,
                voxelWorld->GetPaletteSRV(),
                sparseNearField.surfaceFaceCount,
                cameraParams,
                useSparseSurfaceIndirect ? sparseSurfaceGpuResources.DrawArgsResource() : nullptr,
                useSparseSurfaceIndirect
                    ? (sparseSurfaceGpuCullReady ? sparseSurfaceStats.maxDrawCommands : sparseSurfaceStats.uploadedDrawCommands)
                    : 0u,
                (useSparseSurfaceIndirect && sparseSurfaceGpuCullReady)
                    ? sparseSurfaceGpuResources.DrawCountResource()
                    : nullptr,
                &sparseSurfaceGpuResources.VertexIdBufferView(),
                &sparseSurfaceGpuResources.IndexBufferView(),
                sparseSurfaceGpuResources.VertexIdCapacityFaces(),
                &sparseSurfaceGpuResources.SurfaceRecordSRV(),
                &sparseSurfaceGpuResources.SurfaceClusterSRV(),
                cameraParams.renderOwnershipStatsEnabled ? &sparseNearField.renderOwnershipUAV : nullptr);
            if (sparseSurfaceGpuCullReady) {
                sparseSurfaceGpuResources.QueueGpuCullStatsReadback(commandList.Get(), frameIndex);
            }
            sparseSurfaceRasterFacesLastFrame = sparseNearField.surfaceFaceCount;
        };

        // Sparse surfaces are the near-field owner. Draw them first so they
        // write depth/stencil; the fullscreen background raymarch runs only for
        // pixels not already owned by sparse raster surfaces.
        renderSparseSurfaceLayer();
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 3);
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
            sparseNearField.surfaceAuthoritative ? nullptr : &brushPreview,
            sparseNearField.surfaceAuthoritative ? nullptr : &characterPreview,
            &sparseFarField,
            &sparseNearField
        );
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 4);
        }
        if (sparseNearField.surfaceAuthoritative) {
            renderer->RenderOverlays(
                commandList.Get(),
                voxelWorld->GetPaletteSRV(),
                cameraParams,
                &brushPreview,
                &characterPreview);
        }
        if (gpuTimestampHeap) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 5);
        }
        if (enableSparseBodyCollision && sparseBackendRequested && sparseVoxelWorldReady) {
            sparseBackendActiveMaskThisFrame |= kBackendPipeCollision;
        }
        if (enableSparseLocalPhysics && sparseBackendRequested && sparseVoxelWorldReady) {
            sparseBackendActiveMaskThisFrame |= kBackendPipePhysics;
        }
        if (frameCount > 90) {
            const uint32_t inactiveConfigured =
                sparseBackendConfiguredMaskThisFrame & ~sparseBackendActiveMaskThisFrame;
            const uint32_t criticalInactive =
                inactiveConfigured &
                (kBackendPipeCpuWorld |
                 kBackendPipeGpuResources |
                 kBackendPipeRaymarch |
                 kBackendPipeNearBinding |
                 kBackendPipeSurfaceGpu |
                 kBackendPipeSurfaceRaster |
                 kBackendPipeSurfaceAuthoritative |
                 kBackendPipeMidClipmap |
                 kBackendPipeFarSvo |
                 kBackendPipeCollision |
                 kBackendPipePhysics);
            sparseBackendWarnMaskThisFrame = criticalInactive;
            if (requireSparsePipeReady &&
                criticalInactive == 0u &&
                frameCount >= sparsePipeReadyFrame) {
                sparseBackendPipeReadyObserved = true;
            }
            if (criticalInactive != 0u &&
                enableRuntimeLog &&
                (sparseBackendLastWarnFrame == 0 ||
                 frameCount - sparseBackendLastWarnFrame >= 240u)) {
                sparseBackendLastWarnFrame = frameCount;
                spdlog::warn(
                    "SPARSE_BACKEND_PIPE inactive configured bits frame={} configured=0x{:X} active=0x{:X} warn=0x{:X} missing=[{}] surfaceFaces={} midSerial={} farSvo={} physics={} collision={}",
                    frameCount,
                    sparseBackendConfiguredMaskThisFrame,
                    sparseBackendActiveMaskThisFrame,
                    criticalInactive,
                    sparsePipeMaskNames(criticalInactive),
                    sparseNearField.surfaceFaceCount,
                    std::max(sparseMidClipmapUploadedHeightSerial, sparseMidClipmapUploadedVoxelSerial),
                    sparseFarField.enabled ? 1 : 0,
                    enableSparseLocalPhysics ? 1 : 0,
                    enableSparseBodyCollision ? 1 : 0);
            }
            if (requireSparsePipeReady &&
                criticalInactive != 0u &&
                frameCount >= sparsePipeReadyFrame) {
                sparseBackendPipeReadyFailed = true;
                sparseBackendPipeReadyFailureMask = criticalInactive;
                spdlog::critical(
                    "SPARSE_BACKEND_PIPE readiness failed at frame {}: configured=0x{:X} active=0x{:X} missing=0x{:X} [{}]. This is a smoke-test failure, not a recoverable visual fallback.",
                    frameCount,
                    sparseBackendConfiguredMaskThisFrame,
                    sparseBackendActiveMaskThisFrame,
                    criticalInactive,
                    sparsePipeMaskNames(criticalInactive));
                running = false;
            }
        }
        sparseBackendConfiguredMaskLastFrame = sparseBackendConfiguredMaskThisFrame;
        sparseBackendActiveMaskLastFrame = sparseBackendActiveMaskThisFrame;
        sparseBackendWarnMaskLastFrame = sparseBackendWarnMaskThisFrame;
        if (cameraParams.renderOwnershipStatsEnabled) {
            sparseGpuResources.QueueRenderOwnershipReadback(
                commandList.Get(),
                static_cast<uint32_t>(frameCount));
        }
        perfRenderSubmitMs = ticksToMs(SDL_GetPerformanceCounter() - perfPhaseStart);
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

        if (!hideUiForCapture) {
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
                sparseBackendRequested
                    ? (enableSparseSurfaceAuthoritative
                        ? " | sparse surface authoritative"
                        : " | sparse requested, dense fallback active")
                    : "");
            if (sparseBackendRequested) {
                const auto& sparseStats = sparseGpuResources.GetStats();
                ImGui::Text("Sparse raymarch visual %s | debug %u | only %u",
                    enableSparseRaymarch ? "on" : "off",
                    sparseRaymarchDebugMode,
                    enableSparseOnlyRaymarch ? 1u : 0u);
                const auto midResidencyOverlay =
                    Simulation::BuildClipmapResidencyMetadata(sparseClipmapTileCache.GetStats());
                ImGui::Text("Sparse mid clipmap %s | %.0f..%.0f | cell %.0f | coverage h/v %.2f/%.2f",
                    sparseClipmapPolicy.IsEnabled() ? "on" : "off",
                    sparseClipmapPolicy.Config().startDistance,
                    sparseClipmapPolicy.Config().endDistance,
                    sparseClipmapPolicy.Config().minCellSize,
                    midResidencyOverlay.heightCoverageRatio,
                    midResidencyOverlay.voxelCoverageRatio);
                const auto& midStats = sparseClipmapTileCache.GetStats();
                const uint32_t midHeightCovered =
                    midStats.interestedTiles > midStats.missingInterestedTiles
                        ? midStats.interestedTiles - midStats.missingInterestedTiles
                        : 0u;
                const uint32_t midVoxelCovered =
                    midStats.interestedVoxelBricks > midStats.missingInterestedVoxelBricks
                        ? midStats.interestedVoxelBricks - midStats.missingInterestedVoxelBricks
                        : 0u;
                ImGui::Text("Sparse mid cache tiles %u resident / %u queued | interest %u/%u | gen %u evict %u | upload %u retry %u",
                    midStats.residentTiles,
                    midStats.queuedTiles,
                    midHeightCovered,
                    midStats.interestedTiles,
                    midStats.generatedTilesLastFrame,
                    midStats.evictedTilesLastFrame,
                    sparseStats.stagedMidClipmapTilesLastFrame,
                    sparseMidClipmapUploadRetriesLastFrame);
                ImGui::Text("Sparse mid voxel bricks %u resident / %u queued | interest %u/%u | gen %u evict %u | upload %u",
                    midStats.residentVoxelBricks,
                    midStats.queuedVoxelBricks,
                    midVoxelCovered,
                    midStats.interestedVoxelBricks,
                    midStats.generatedVoxelBricksLastFrame,
                    midStats.evictedVoxelBricksLastFrame,
                    sparseStats.stagedMidVoxelClipmapBricksLastFrame);
                ImGui::Text("Sparse mid interest anchors height %u | voxel %u",
                    midStats.heightInterestAnchors,
                    midStats.voxelInterestAnchors);
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
                ImGui::Text("Sparse upload staged %u bricks (%u partial %u ranges %.2f KB) + %u page entries / %.2f MB | overflow %u",
                    sparseStats.stagedBricksLastFrame,
                    sparseStats.stagedPartialBrickUploadsLastFrame,
                    sparseStats.stagedPartialCopyRangesLastFrame,
                    static_cast<double>(sparseStats.stagedPartialVoxelBytesLastFrame) / 1024.0,
                    sparseStats.stagedPageEntriesLastFrame,
                    static_cast<double>(sparseStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                    sparseStats.uploadRingOverflowLastFrame ? 1u : 0u);
                ImGui::Text("Sparse upload ring used %.2f / %.2f MB | byte defers %u",
                    static_cast<double>(sparseUploadRingUsedBytesLastFrame) / (1024.0 * 1024.0),
                    static_cast<double>(sparseUploadRingCapacityBytesLastFrame) / (1024.0 * 1024.0),
                    sparseUploadRingBudgetDefersLastFrame);
                ImGui::Text("Sparse upload plan reserved %.2f MB | remaining %.2f MB | plan defers %u",
                    static_cast<double>(sparseFrameUploadReservedBytesLastFrame) / (1024.0 * 1024.0),
                    static_cast<double>(sparseFrameUploadRemainingBytesLastFrame) / (1024.0 * 1024.0),
                    sparseFrameUploadPlanDefersLastFrame);
                ImGui::Text("Sparse upload selection value-picked %u focused class bricks",
                    sparseValueSelectedUploadsLastFrame);
                ImGui::Text("Sparse miss feedback f%u %u retired | %zu pending | %u consumed | stale %u overflow %u | %s grid %u dist %u stride %u",
                    sparseStats.missFeedbackFrameLastRetire,
                    sparseStats.missFeedbackRecordsLastRetire,
                    sparseMissFeedbackPending.size(),
                    sparseMissFeedbackConsumedLastFrame,
                    sparseStats.missFeedbackStaleFrameDropsLastRetire,
                    sparseStats.missFeedbackOverflowLastRetire ? 1u : 0u,
                    sparseMissFeedbackUrgentLastFrame ? "urgent" : "normal",
                    sparseMissFeedbackRayGridLastFrame,
                    sparseMissFeedbackDistanceLastFrame,
                    sparseMissFeedbackStrideLastFrame);
                ImGui::Text("Sparse requests spec/vis/coll %u/%u/%u | budgets %u/%u/%u total %u | fast x%u | protected over %u",
                    sparseSpeculativeRequestsLastFrame,
                    sparseVisibleRequestsLastFrame,
                    sparseCollisionRequestsLastFrame,
                    sparseSpeculativeRequestBudgetLastFrame,
                    sparseVisibleRequestBudgetLastFrame,
                    sparseCollisionRequestBudgetLastFrame,
                    sparseTotalRequestBudgetLastFrame,
                    sparseFastRequestScaleLastFrame,
                    sparseProtectedRequestOverageLastFrame);
                ImGui::Text("Sparse brush residency intent %u | reserve/max %u/%u",
                    sparseBrushIntentActiveLastFrame,
                    sparseBrushCollisionReserveLastFrame,
                    sparseBrushCollisionMaxLastFrame);
                ImGui::Text("Sparse request skips free/class/total/reject/empty %u/%u/%u/%u/%u",
                    sparseRequestFreePageSkipsLastFrame,
                    sparseRequestClassBudgetSkipsLastFrame,
                    sparseRequestTotalBudgetSkipsLastFrame,
                    sparseRequestRejectedSkipsLastFrame,
                    sparseRequestKnownEmptySkipsLastFrame);
                ImGui::Text("Sparse body collision %s | hblock %u step %u vblock %u land %u ceil %u grounded %u snap %u | safe %.2f | sampled %u solid %u liquid %u",
                    enableSparseBodyCollision ? "on" : "off",
                    sparseBodyCollisionBlockedLastFrame,
                    sparseBodyCollisionStepUpsLastFrame,
                    sparseBodyCollisionVerticalBlockedLastFrame,
                    sparseBodyCollisionLandedLastFrame,
                    sparseBodyCollisionCeilingLastFrame,
                    sparseBodyCollisionGroundedLastFrame,
                    sparseBodyCollisionGroundSnapsLastFrame,
                    sparseBodyCollisionSafeFractionLastFrame,
                    sparseBodyCollisionSampledLastFrame,
                    sparseBodyCollisionSolidLastFrame,
                    sparseBodyCollisionLiquidLastFrame);
                ImGui::Text("Sparse stress %s | planned %u accepted %u | radius %u/%u budget %u",
                    enableSparseStressRequests ? "on" : "off",
                    sparseStressRequestsLastFrame,
                    sparseStressAcceptedLastFrame,
                    sparseStressRadiusXz,
                    sparseStressRadiusY,
                    sparseStressBudget);
                ImGui::Text("Sparse surface authoritative %s",
                    enableSparseSurfaceAuthoritative ? "on" : "off");
                ImGui::Text("Sparse runtime scale %.2f protected %.2f bg %.2f | protQ %u trimSpec %u | gen/upload/mid budgets %u/%u/%u",
                    sparseRuntimeBudgetScale,
                    sparseProtectedRuntimeBudgetScale,
                    sparseBackgroundRuntimeBudgetScale,
                    sparseProtectedBacklogLastFrame,
                    sparseTrimSpeculativeFirstLastFrame,
                    sparseGenerationBudgetLastFrame,
                    sparseUploadBudgetLastFrame,
                    sparseMidClipmapBudgetLastFrame);
                const uint32_t sparseEffectiveOwnershipPressureLevelForUi =
                    computeSparseEffectiveOwnershipPressureLevel();
                ImGui::Text("Sparse ownership pressure terrain %u%% miss %u%% unsafe %u%% | retire level %u effective %u active %u | catchup %u | def/excess %u/%u/%u",
                    sparseOwnershipTerrainPctLastRetire,
                    sparseOwnershipMissPctLastRetire,
                    sparseOwnershipUnsafeNearMissPctLastRetire,
                    sparseOwnershipPressureLevelActive,
                    sparseEffectiveOwnershipPressureLevelForUi,
                    sparseResidencyCatchupLastFrame,
                    sparseResidencyCatchupFramesRemaining,
                    sparseOwnershipPressureTerrainDeficitLastRetire,
                    sparseOwnershipPressureMissExcessLastRetire,
                    sparseOwnershipPressureUnsafeNearMissExcessLastRetire);
                const auto& sparseWorldStats = sparseVoxelWorld.GetStats();
                ImGui::Text("Sparse surface extract queued %u | extracted %u / budget %u | empty skip %u fast %u",
                    sparseWorldStats.surfaceExtractionQueuedBricks,
                    sparseWorldStats.surfaceBricksExtractedLastFrame,
                    sparseSurfaceExtractionBudgetLastFrame,
                    sparseWorldStats.surfaceEmptyUploadsSkippedLastFrame,
                    sparseWorldStats.surfaceEmptyFastPathBricksLastFrame);
                ImGui::Text("Sparse surface dirty partial %u | faces rm/gen %u/%u",
                    sparseWorldStats.surfaceBricksPartiallyUpdatedLastFrame,
                    sparseWorldStats.surfaceFacesRemovedByPartialUpdatesLastFrame,
                    sparseWorldStats.surfaceFacesGeneratedLastFrame);
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
                ImGui::Text("Sparse empty request skips %u | known empty %u",
                    sparseWorldStats.emptyRequestsSkippedLastFrame,
                    sparseWorldStats.knownEmptyGeneratedBricks);
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
                const uint32_t overlayFrameU32 = static_cast<uint32_t>(std::min<uint64_t>(
                    frameCount,
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
                const Simulation::SparsePagePublishQueueStats overlayPublishStats =
                    sparsePagePublishQueue.GetStats(
                        overlayFrameU32,
                        commandQueue ? commandQueue->GetLastCompletedFenceValue() : 0u);
                ImGui::Text("Sparse page publish total/ready/frame/fence %zu/%zu/%zu/%zu | retry %u stale %u",
                    overlayPublishStats.total,
                    overlayPublishStats.ready,
                    overlayPublishStats.waitingFrame,
                    overlayPublishStats.waitingFence,
                    sparsePageTablePublishRetriesLastFrame,
                    sparsePageTablePublishStaleDropsLastFrame);
                ImGui::Text("Sparse edited page publish q/pub/promote %u/%u/%u",
                    sparseEditedPageTablePublishesQueuedLastFrame,
                    sparseEditedPageTablePublishesPublishedLastFrame,
                    sparseEditedPageTablePublishPromotionsLastFrame);
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
                ImGui::Text("Sparse queue classes gen %u/%u/%u/%u | upload %u/%u/%u/%u | surf %u/%u/%u/%u",
                    sparseWorldStats.generationQueuedSpeculativeBricks,
                    sparseWorldStats.generationQueuedVisibleBricks,
                    sparseWorldStats.generationQueuedCollisionBricks,
                    sparseWorldStats.generationQueuedEditedBricks,
                    sparseWorldStats.uploadQueuedSpeculativeBricks,
                    sparseWorldStats.uploadQueuedVisibleBricks,
                    sparseWorldStats.uploadQueuedCollisionBricks,
                    sparseWorldStats.uploadQueuedEditedBricks,
                    sparseWorldStats.surfaceQueuedSpeculativeBricks,
                    sparseWorldStats.surfaceQueuedVisibleBricks,
                    sparseWorldStats.surfaceQueuedCollisionBricks,
                    sparseWorldStats.surfaceQueuedEditedBricks);
                ImGui::Text("Sparse processed classes gen %u/%u/%u/%u | upload %u/%u/%u/%u | surf %u/%u/%u/%u",
                    sparseWorldStats.generatedSpeculativeBricksLastFrame,
                    sparseWorldStats.generatedVisibleBricksLastFrame,
                    sparseWorldStats.generatedCollisionBricksLastFrame,
                    sparseWorldStats.generatedEditedBricksLastFrame,
                    sparseWorldStats.uploadedSpeculativeBricksLastFrame,
                    sparseWorldStats.uploadedVisibleBricksLastFrame,
                    sparseWorldStats.uploadedCollisionBricksLastFrame,
                    sparseWorldStats.uploadedEditedBricksLastFrame,
                    sparseWorldStats.surfaceSpeculativeBricksExtractedLastFrame,
                    sparseWorldStats.surfaceVisibleBricksExtractedLastFrame,
                    sparseWorldStats.surfaceCollisionBricksExtractedLastFrame,
                    sparseWorldStats.surfaceEditedBricksExtractedLastFrame);
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
                ImGui::Text("Sparse edit deltas GPU %u deltas / %u ranges / table %u / %.2f KB | pending %zu | overflow %u",
                    sparseStats.stagedEditDeltasLastFrame,
                    sparseStats.stagedEditDeltaRangesLastFrame,
                    sparseStats.stagedEditDeltaRangeTableEntriesLastFrame,
                    static_cast<double>(sparseStats.stagedEditDeltaBytesLastFrame) / 1024.0,
                    sparseVoxelWorld.GetPendingGpuEditDeltas().size(),
                    sparseStats.editDeltaUploadOverflowLastFrame ? 1u : 0u);
                ImGui::Text("Sparse edit persistence: %s", sparseEditUiStatus.c_str());
                if (pauseMenu.IsVisible()) {
                    ImGui::PushItemWidth(420.0f);
                    ImGui::InputText(
                        "##SparseEditPersistencePath",
                        sparseEditUiPathBuffer.data(),
                        sparseEditUiPathBuffer.size());
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    if (ImGui::Button("Save Edits")) {
                        const std::filesystem::path uiPath =
                            ResolveSandboxUserPath(sparseEditUiPathBuffer.data());
                        if (uiPath.empty()) {
                            sparseEditUiStatus = "save failed: empty path";
                        } else if (sparseVoxelWorld.SaveEditsToFile(uiPath)) {
                            sparseEditUiStatus =
                                "saved " +
                                std::to_string(sparseVoxelWorld.GetEdits().EditedVoxelCount()) +
                                " voxels to " +
                                uiPath.string();
                            spdlog::info("Saved sparse edit overlays from UI to {}", uiPath.string());
                        } else {
                            sparseEditUiStatus = "save failed: " + uiPath.string();
                            spdlog::warn("Failed to save sparse edit overlays from UI to {}", uiPath.string());
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Load Edits")) {
                        const std::filesystem::path uiPath =
                            ResolveSandboxUserPath(sparseEditUiPathBuffer.data());
                        if (uiPath.empty()) {
                            sparseEditUiStatus = "load failed: empty path";
                        } else if (sparseVoxelWorld.LoadEditsFromFile(uiPath, true)) {
                            sparseEditUiStatus =
                                "loaded " +
                                std::to_string(sparseVoxelWorld.GetEdits().EditedVoxelCount()) +
                                " voxels from " +
                                uiPath.string();
                            spdlog::info("Loaded sparse edit overlays from UI from {}", uiPath.string());
                        } else {
                            sparseEditUiStatus = "load failed: " + uiPath.string();
                            spdlog::warn("Failed to load sparse edit overlays from UI from {}", uiPath.string());
                        }
                    }
                }
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
            const auto& farStatsOverlay = farVoxelOctree.GetStats();
            const char* farSvoState =
                sparseFarField.enabled ? "on" :
                (farVoxelOctree.IsGpuUploadPending() ? "uploading" :
                    (farVoxelOctree.IsAsyncPending() ? "loading" : "off"));
            ImGui::Text("Far SVO %s | pages %u | nodes %u | index %u | page %.0f | world %.0f | cov upload/page %.2f/%.2f",
                farSvoState,
                sparseFarField.pageCount,
                sparseFarField.nodeCount,
                sparseFarField.pageIndexCount,
                sparseFarField.pageSize,
                farStatsOverlay.coveredWorldSize,
                sparseFarField.uploadCoverageRatio,
                sparseFarField.pageCoverageRatio);
            ImGui::Text("Far SVO upload %s | %.2f / %.2f MB | budget %.2f MB tier %u scale %.2f | upload %.2f/%.2f ms",
                farVoxelOctree.GetGpuUploadStageName(),
                static_cast<double>(farStatsOverlay.gpuUploadBytesUploaded) / (1024.0 * 1024.0),
                static_cast<double>(farStatsOverlay.gpuUploadBytesTotal) / (1024.0 * 1024.0),
                static_cast<double>(farSvoUploadBudgetLastFrame) / (1024.0 * 1024.0),
                farSvoUploadPressureTier,
                farSvoUploadScale,
                farSvoUploadMsLastFrame,
                farSvoUploadMsSmoothed);
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
            if (sparseBackendRequested) {
                ImGui::Text("Sparse GPU raycast %s | frame a/r/m/f %u/%u/%u/%u | ready totals %u/%u/%u/%u",
                    enableSparseGpuRaycast ? "on" : "off",
                    sparseGpuRaycastAcceptedLastFrame,
                    sparseGpuRaycastRejectedLastFrame,
                    sparseGpuRaycastMissLastFrame,
                    sparseGpuRaycastFallbackLastFrame,
                    sparseGpuRaycastAcceptedSinceReady,
                    sparseGpuRaycastRejectedSinceReady,
                    sparseGpuRaycastMissSinceReady,
                    sparseGpuRaycastFallbackSinceReady);
                ImGui::Text("Sparse brush deltas %u | bricks %u | mismatches %u",
                    sparseBrushStrokeDeltasLastFrame,
                    sparseBrushStrokeDeltaBricksLastFrame,
                    sparseBrushStrokeDeltaMismatchesLastFrame);
                const char* sparseBrushFeedbackMode =
                    enableSparseBrushFeedback
                        ? (enableSparseBrushFeedbackAuthoritative
                            ? "authoritative"
                            : (enableSparseBrushFeedbackApply ? "apply" : "observe"))
                        : "off";
                ImGui::Text("Sparse brush GPU feedback %s | q %u rb %u apply %u overflow %u stale %u missResident %u cpuFallback %u pending %zu",
                    sparseBrushFeedbackMode,
                    sparseBrushFeedbackQueuedLastFrame,
                    sparseBrushFeedbackRetiredLastFrame,
                    sparseBrushFeedbackAppliedLastFrame,
                    sparseBrushFeedbackOverflowLastFrame,
                    sparseGpuResources.GetStats().brushFeedbackStaleFrameDropsLastRetire,
                    sparseBrushFeedbackMissingResidentLastFrame,
                    sparseBrushFeedbackCpuFallbackLastFrame,
                    sparseBrushFeedbackPendingStrokes.size());
                ImGui::Text("Sparse brush missing feedback hints %u | brick requests %u",
                    sparseBrushFeedbackMissingResidentHintsLastFrame,
                    sparseBrushFeedbackMissingResidentRequestsLastFrame);
                ImGui::Text("Sparse brush parity %s | exp %u match %u miss %u extra %u val %u",
                    sparseBrushFeedbackParityFailed ? "failed" :
                        (sparseBrushFeedbackParityObserved ? "ok" :
                            (sparseBrushFeedbackParityPending ? "pending" : "idle")),
                    sparseBrushFeedbackParityExpectedLastFrame,
                    sparseBrushFeedbackParityMatchedLastFrame,
                    sparseBrushFeedbackParityMissingLastFrame,
                    sparseBrushFeedbackParityUnexpectedLastFrame,
                    sparseBrushFeedbackParityValueMismatchLastFrame);
            }
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
            if (sparseBackendRequested && sparseVoxelWorldReady) {
                const auto& sparseWorldStats = sparseVoxelWorld.GetStats();
                const auto& sparseGpuStats = sparseGpuResources.GetStats();
                ImGui::Text("Sparse physics %s queue %u hot/warm %u/%u packets %u h/w %u/%u regionVox %u support %u gpu %u/%.2fKB rb %u/res %u prop %u missBelow %u apply %u/%u reject %u mask 0x%X processed %u moved %u skipped %u budget %u/%u",
                    enableSparseLocalPhysics ? "on" : "off",
                    sparseWorldStats.physicsCandidateBricks,
                    sparseWorldStats.physicsHotCandidateBricks,
                    sparseWorldStats.physicsWarmCandidateBricks,
                    sparseWorldStats.physicsWorkPacketsLastFrame,
                    sparseWorldStats.physicsHotWorkPacketsLastFrame,
                    sparseWorldStats.physicsWarmWorkPacketsLastFrame,
                    sparseWorldStats.physicsDirtyRegionVoxelsLastFrame,
                    sparseWorldStats.physicsSupportBricksRequestedLastFrame,
                    sparseGpuStats.stagedPhysicsPacketsLastFrame,
                    static_cast<double>(sparseGpuStats.stagedPhysicsPacketBytesLastFrame) / 1024.0,
                    sparseGpuStats.physicsGpuPacketsLastRetire,
                    sparseGpuStats.physicsGpuResultCountLastRetire,
                    sparseGpuStats.physicsGpuProposalCountLastRetire,
                    sparseGpuStats.physicsGpuMissingBelowCountLastRetire,
                    sparseWorldStats.physicsGpuAppliedMovesLastFrame,
                    sparseWorldStats.physicsGpuProcessedProposalsLastFrame,
                    sparseWorldStats.physicsGpuRejectedProposalsLastFrame,
                    sparseGpuStats.physicsGpuMaterialMaskLastRetire,
                    sparseWorldStats.physicsProcessedBricksLastFrame,
                    sparseWorldStats.physicsMovedVoxelsLastFrame,
                    sparseWorldStats.physicsSkippedVoxelsLastFrame,
                    sparsePhysicsBrickBudgetLastFrame,
                    sparsePhysicsMoveBudgetLastFrame);
                ImGui::Text("Sparse render dirty bricks %u vox %u | queued %u fullUp %u defer %u nonres %u",
                    sparseWorldStats.renderDirtyBricks,
                    sparseWorldStats.renderDirtyRegionVoxels,
                    sparseWorldStats.renderDirtyVoxelsQueuedLastFrame,
                    sparseWorldStats.renderDirtyFullUploadsQueuedLastFrame,
                    sparseWorldStats.renderDirtyUploadDeferredLastFrame,
                    sparseWorldStats.renderDirtyNonResidentLastFrame);
            }
            ImGui::Text("Copy budget %u | generation budget %u | copy-fence skips %u",
                currentCopyBudget,
                currentGenerationBudget,
                streamingStats.swapSkippedForCopyFence);
            ImGui::Text("Raymarch budget %.0f voxels / %u steps | far %.2f | bg quality %.2f | tier %u",
                currentRaymarchMaxDistance,
                currentRaymarchMaxSteps,
                currentFarFieldQuality,
                currentRenderQuality,
                sparseBackgroundQualityTier);
            if (sparseBackendRequested) {
                ImGui::Text("Sparse backend pipe cfg 0x%03X | active 0x%03X | warn 0x%03X",
                    sparseBackendConfiguredMaskLastFrame,
                    sparseBackendActiveMaskLastFrame,
                    sparseBackendWarnMaskLastFrame);
                const auto overlayTransitionMetadata = sparseClipmapPolicy.BuildTransitionMetadata();
                const auto overlayMidResidency =
                    Simulation::BuildClipmapResidencyMetadata(sparseClipmapTileCache.GetStats());
                ImGui::Text("Sparse render ownership near surface %s | mid %.0f..%.0f far>=%.0f cov %.2f/%.2f | mid tiles r%u/%u vox r%u/%u | cull %.0f+%.0f",
                    enableSparseSurfaceAuthoritative ? "auth" : "blend",
                    overlayTransitionMetadata.startDistance,
                    overlayTransitionMetadata.endDistance,
                    overlayTransitionMetadata.farHandoffDistance,
                    overlayMidResidency.heightCoverageRatio,
                    overlayMidResidency.voxelCoverageRatio,
                    sparseClipmapPolicy.Config().tileRadius,
                    sparseClipmapPolicy.Config().maxTiles,
                    sparseClipmapPolicy.Config().voxelBrickRadiusXz,
                    sparseClipmapPolicy.Config().maxVoxelBricks,
                    sparseSurfaceCullDistance,
                    sparseSurfaceCullPadding);
                if (sparseGpuResources.IsInitialized()) {
                    const auto& sparseGpuStats = sparseGpuResources.GetStats();
                    ImGui::Text("Sparse pixel ownership sample f%u bg %u | surfOwned %u frag %u | near %u midV %u midH %u farSVO %u farH %u sky %u miss %u unsafe %u",
                        sparseGpuStats.renderOwnerFrameLastRetire,
                        sparseGpuStats.renderOwnerTotalPixelsLastRetire,
                        sparseSurfaceOwnedPixelsLastRetire,
                        sparseGpuStats.renderOwnerSurfacePixelsLastRetire,
                        sparseGpuStats.renderOwnerNearPixelsLastRetire,
                        sparseGpuStats.renderOwnerMidVoxelPixelsLastRetire,
                        sparseGpuStats.renderOwnerMidHeightPixelsLastRetire,
                        sparseGpuStats.renderOwnerFarSvoPixelsLastRetire,
                        sparseGpuStats.renderOwnerFarHeightPixelsLastRetire,
                        sparseGpuStats.renderOwnerSkyPixelsLastRetire,
                        sparseGpuStats.renderOwnerMissPixelsLastRetire,
                        sparseGpuStats.renderOwnerUnsafeNearMissPixelsLastRetire);
                    ImGui::Text("Sparse ownership mix bg %.0f%% midV %.0f%% farH %.0f%% sky %.0f%%",
                        sparseOwnershipBackgroundPixelShareLastRetire * 100.0f,
                        sparseOwnershipMidVoxelPixelShareLastRetire * 100.0f,
                        sparseOwnershipFarHeightPixelShareLastRetire * 100.0f,
                        sparseOwnershipSkyPixelShareLastRetire * 100.0f);
                }
            }
            if (sparseSurfaceGpuResources.IsInitialized()) {
                const auto& surfaceGpuStats = sparseSurfaceGpuResources.GetStats();
                const auto& surfaceCacheStats = sparseVoxelWorld.GetSurfaceCache().GetStats();
                ImGui::Text("Surface GPU dirty %u removed %u | copied %u skipped %u defer %u resident %u retired %u/%u regions %u patch %u/%u/%u MB %.2f",
                    surfaceCacheStats.pendingGpuDirtyBricks,
                    surfaceCacheStats.pendingGpuRemovedBricks,
                    surfaceGpuStats.stagedDirtyPayloadBricksLastFrame,
                    surfaceGpuStats.skippedCleanPayloadBricksLastFrame,
                    surfaceGpuStats.deferredPayloadBricksLastFrame,
                    surfaceGpuStats.residentPayloadBricks,
                    surfaceGpuStats.pendingRetiredFaceRanges,
                    surfaceGpuStats.pendingRetiredFaceCapacity,
                    surfaceGpuStats.stagedFaceCopyRegionsLastFrame,
                    surfaceGpuStats.stagedPayloadPatchBricksLastFrame,
                    surfaceGpuStats.stagedPayloadPatchFacesLastFrame,
                    surfaceGpuStats.stagedPayloadPatchRegionsLastFrame,
                    static_cast<double>(surfaceGpuStats.stagedBytesLastFrame) / (1024.0 * 1024.0));
                ImGui::Text("Surface metadata range %u%s %s skip %u | draw %u%s skip %u | rec %u%s skip %u",
                    surfaceGpuStats.stagedRangeCopyRegionsLastFrame,
                    surfaceGpuStats.fullRangeTableUploadLastFrame ? " full" : " inc",
                    surfaceGpuStats.fixedRangeTableEnabled ? "fixed" : "dyn",
                    surfaceGpuStats.skippedCleanRangeSlotsLastFrame,
                    surfaceGpuStats.stagedDrawCopyRegionsLastFrame,
                    surfaceGpuStats.fullDrawArgsUploadLastFrame ? " full" : " inc",
                    surfaceGpuStats.skippedCleanDrawCommandsLastFrame,
                    surfaceGpuStats.stagedSurfaceRecordCopyRegionsLastFrame,
                    surfaceGpuStats.fullSurfaceRecordUploadLastFrame ? " full" : " inc",
                    surfaceGpuStats.skippedCleanSurfaceRecordsLastFrame);
                ImGui::Text("Surface draw slots %s%s resident %u active %u staged %u high %u free %u inactive %u",
                    surfaceGpuStats.stableDrawSlotsEnabled ? "stable" : "compact",
                    surfaceGpuStats.compactStableDrawCommandsEnabled ? "+compact" : "",
                    surfaceGpuStats.uploadedDrawCommands,
                    surfaceGpuStats.uploadedActiveDrawCommands,
                    surfaceGpuStats.activeDrawCommandsLastFrame,
                    surfaceGpuStats.stableDrawSlotCapacity,
                    surfaceGpuStats.stableDrawFreeSlots,
                    surfaceGpuStats.inactiveStableDrawSlotsLastFrame);
                ImGui::Text("Surface IA stream %s faces %u | verts %u indices %u | MB %.2f/%.2f | upload %s",
                    surfaceGpuStats.iaStreamGpuLocal ? "gpu" : "upload",
                    surfaceGpuStats.iaStreamCapacityFaces,
                    surfaceGpuStats.iaStreamVertexCount,
                    surfaceGpuStats.iaStreamIndexCount,
                    static_cast<double>(surfaceGpuStats.iaStreamVertexBytes) / (1024.0 * 1024.0),
                    static_cast<double>(surfaceGpuStats.iaStreamIndexBytes) / (1024.0 * 1024.0),
                    surfaceGpuStats.iaStreamUploadPending ? "pending" : "resident");
                ImGui::Text("Surface GPU cull %s records %u clusters %u x%u extent %u fast %u/%u dispatch %u cand %u/%u maxDraw %u",
                    surfaceGpuStats.gpuCullEnabled ? "on" : "off",
                    surfaceGpuStats.uploadedSurfaceRecords,
                    surfaceGpuStats.uploadedSurfaceClusters,
                    surfaceGpuStats.surfaceRecordsPerCluster,
                    surfaceGpuStats.surfaceClusterMaxExtentVoxels,
                    surfaceGpuStats.surfaceClusterFastAcceptMaxRecords,
                    surfaceGpuStats.surfaceClusterFastAcceptMaxFaces,
                    surfaceGpuStats.gpuCullDispatchesLastFrame,
                    surfaceGpuStats.gpuCullCandidateRecordsLastFrame,
                    surfaceGpuStats.gpuCullCandidateClustersLastFrame,
                    surfaceGpuStats.gpuCullMaxDrawCommands);
                ImGui::Text("Surface GPU cull stats %s accepted %u cluster/record %u/%u fast %u reject inv/dist/frust/back/cluster %u/%u/%u/%u/%u overflow %u rb %u/%u pending %u",
                    surfaceGpuStats.gpuCullStatsValid ? "valid" : "pending",
                    surfaceGpuStats.gpuCullAcceptedDraws,
                    surfaceGpuStats.gpuCullAcceptedClusterDraws,
                    surfaceGpuStats.gpuCullAcceptedRecordDraws,
                    surfaceGpuStats.gpuCullFastAcceptedClusterRecords,
                    surfaceGpuStats.gpuCullRejectedInvalid,
                    surfaceGpuStats.gpuCullRejectedDistance,
                    surfaceGpuStats.gpuCullRejectedFrustum,
                    surfaceGpuStats.gpuCullRejectedBackface,
                    surfaceGpuStats.gpuCullRejectedClusters,
                    surfaceGpuStats.gpuCullOverflow,
                    surfaceGpuStats.gpuCullStatsReadbacksRetired,
                    surfaceGpuStats.gpuCullStatsReadbacksQueued,
                    surfaceGpuStats.gpuCullStatsReadbackPending);
            }
            ImGui::Text("CPU phases wait %.2f chunk %.2f phys %.2f brush %.2f render %.2f present %.2f ms",
                perfFenceWaitMs,
                perfChunkUpdateMs,
                perfPhysicsSubmitMs,
                perfBrushSubmitMs,
                perfRenderSubmitMs,
                perfPresentMs);
            ImGui::Text("GPU frame %.2f ms | upload %.2f | pre %.2f | surface %.2f | ray %.2f | overlay %.2f | ui/readback %.2f",
                gpuTiming.valid ? gpuTiming.frameMs : 0.0,
                gpuTiming.valid ? gpuTiming.sparseUploadMs : 0.0,
                gpuTiming.valid ? gpuTiming.preRenderMs : 0.0,
                gpuTiming.valid ? gpuTiming.sparseSurfaceMs : 0.0,
                gpuTiming.valid ? gpuTiming.raymarchMs : 0.0,
                gpuTiming.valid ? gpuTiming.overlayMs : 0.0,
                gpuTiming.valid ? gpuTiming.uiAndReadbackMs : 0.0);
            ImGui::Text("Scheduler predicted %.2f ms | pressure %.2f ms | budget %.2f debt %.2f",
                schedulerPredictedFrameMs,
                schedulerPressureMs,
                schedulerBudgetPressureMs,
                schedulerFrameDebtMs);
        }
        ImGui::End();
        }

        if (enableRuntimeLog && (frameCount % 120 == 0)) {
            const auto& farStatsLog = farVoxelOctree.GetStats();
            const char* farSvoLogState =
                sparseFarField.enabled ? "on" :
                (farVoxelOctree.IsGpuUploadPending() ? "uploading" :
                    (farVoxelOctree.IsAsyncPending() ? "loading" : "off"));
            spdlog::info(
                "PERF frame={} fps={:.1f}/{:.1f} ms={:.2f}/{:.2f} wait={:.2f} chunk={:.2f} phys={:.2f} brush={:.2f} render={:.2f} present={:.2f} gpuValid={} gpu=frame/upload/pre/surface/ray/overlay/ui:{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f} sched={:.2f}/{:.2f}/{:.2f} copy={}/{} genBudget={} generated={} records={} queue={} cached={}/{}/{} pageMiss={}/{} missingGen={} missingLoad={} urgent={} skipped={} checked={} physicsScan={}/{} skip={} universe={} dirty={} farQ={:.2f} farSvo={} farStage={} farCov={:.2f}/{:.2f} farUploadMB={:.2f}/{:.2f} farBudgetMB={:.2f} farTier={} farUploadMs={:.2f}/{:.2f} farGpuAccumMs={:.2f}",
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
                gpuTiming.valid ? 1 : 0,
                gpuTiming.valid ? gpuTiming.frameMs : 0.0,
                gpuTiming.valid ? gpuTiming.sparseUploadMs : 0.0,
                gpuTiming.valid ? gpuTiming.preRenderMs : 0.0,
                gpuTiming.valid ? gpuTiming.sparseSurfaceMs : 0.0,
                gpuTiming.valid ? gpuTiming.raymarchMs : 0.0,
                gpuTiming.valid ? gpuTiming.overlayMs : 0.0,
                gpuTiming.valid ? gpuTiming.uiAndReadbackMs : 0.0,
                schedulerPredictedFrameMs,
                schedulerBudgetPressureMs,
                schedulerFrameDebtMs,
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
                currentFarFieldQuality,
                farSvoLogState,
                farVoxelOctree.GetGpuUploadStageName(),
                sparseFarField.uploadCoverageRatio,
                sparseFarField.pageCoverageRatio,
                static_cast<double>(farStatsLog.gpuUploadBytesUploaded) / (1024.0 * 1024.0),
                static_cast<double>(farStatsLog.gpuUploadBytesTotal) / (1024.0 * 1024.0),
                static_cast<double>(farSvoUploadBudgetLastFrame) / (1024.0 * 1024.0),
                farSvoUploadPressureTier,
                farSvoUploadMsLastFrame,
                farSvoUploadMsSmoothed,
                farStatsLog.gpuUploadMs);
            if (sparseBackendRequested && sparseVoxelWorldReady) {
                spdlog::info(
                    "PERF_BACKEND_PIPE frame={} configured=0x{:X} active=0x{:X} warn=0x{:X} cpu={} gpu={} ray={} near={} surfaceGpu={} surfaceRaster={} surfaceAuth={} mid={} far={} own={} coll={} phys={}",
                    frameCount,
                    sparseBackendConfiguredMaskLastFrame,
                    sparseBackendActiveMaskLastFrame,
                    sparseBackendWarnMaskLastFrame,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeCpuWorld) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeGpuResources) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeRaymarch) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeNearBinding) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeSurfaceGpu) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeSurfaceRaster) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeSurfaceAuthoritative) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeMidClipmap) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeFarSvo) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeOwnership) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipeCollision) ? 1 : 0,
                    (sparseBackendActiveMaskLastFrame & kBackendPipePhysics) ? 1 : 0);
                if (enableSparsePoolValidation) {
                    const Simulation::SparseBrickPoolValidationResult validation =
                        sparseVoxelWorld.GetPool().ValidateInvariants();
                    if (!validation.ok) {
                        spdlog::error(
                            "SPARSE_POOL_INVARIANT frame={} active={} free={} pageEntries={} residentErrors={} freeErrors={} pageTableErrors={} missingPublishedPageEntries={}",
                            frameCount,
                            validation.activeRecords,
                            validation.freePages,
                            validation.pageTableEntries,
                            validation.residentMapErrors,
                            validation.freeListErrors,
                            validation.pageTableErrors,
                            validation.missingPublishedPageTableEntries);
                    }
                }
                const auto& sparseWorldStats = sparseVoxelWorld.GetStats();
                const auto& sparseGpuStats = sparseGpuResources.GetStats();
                const auto& midStats = sparseClipmapTileCache.GetStats();
                const uint32_t midHeightCoveredLog =
                    midStats.interestedTiles > midStats.missingInterestedTiles
                        ? midStats.interestedTiles - midStats.missingInterestedTiles
                        : 0u;
                const uint32_t midVoxelCoveredLog =
                    midStats.interestedVoxelBricks > midStats.missingInterestedVoxelBricks
                        ? midStats.interestedVoxelBricks - midStats.missingInterestedVoxelBricks
                        : 0u;
                const auto midResidencyLog =
                    Simulation::BuildClipmapResidencyMetadata(midStats);
                const uint32_t logFrameU32 = static_cast<uint32_t>(std::min<uint64_t>(
                    frameCount,
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
                const Simulation::SparsePagePublishQueueStats logPublishStats =
                    sparsePagePublishQueue.GetStats(
                        logFrameU32,
                        commandQueue ? commandQueue->GetLastCompletedFenceValue() : 0u);
                spdlog::info(
                    "PERF_SPARSE frame={} runtimeTest={} surfaceAuth={} resident={} tracked={} class={}/{}/{}/{} qgen={}/{}/{}/{} genQueued={} pgen={}/{}/{}/{} staged={} qup={}/{}/{}/{} uploadQueued={} pup={}/{}/{}/{} qsurf={}/{}/{}/{} psurf={}/{}/{}/{} free={} evictLast={} invalidQueued={} emptyReqSkip={} knownEmpty={} edits={}/{} rDirty={}/{} rDirtyQ={} rDirtyUpload={}/{}/{} surface={}/{} unitFaces={} renderable={}/{} surfUpd={} surfPartial={}/{} surfRm={} surfGen={}/{} surfExtract={}/{}/{} surfEmptySkip={} surfEmptyFast={} surfSerial={} brushEval={} brushEdit={} brushBricks={} brushUploads={} brushDelta={}/{} brushDeltaMismatch={} brushGpuFb={}/{}/{}/{} brushGpuFbMiss={} brushGpuFbFallback={} gpuStaged={} gpuPartial={}/{}/{:.2f}KB pageEntries={} uploadMB={:.2f} uploadRingMB={:.2f}/{:.2f} uploadByteDefers={} overflow={} missRetired={} missPending={} missConsumed={} reqSpec={} reqVis={} reqColl={} reqBudget={}/{}/{}/{} protOver={} reqSkip={}/{}/{}/{}/{} bodyColl={}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{:.2f} stress={}/{} scale={:.2f}/{:.2f}/{:.2f} protQ={} trimSpec={} rayScale={:.2f} bgQuality={:.2f} bgTier={} rayBudget={:.0f}/{} budgetGen={} budgetUpload={} budgetMid={} specSkip={} pressureTrim={} distTrimSkip={} trimStart={} replaceEvict={} retryUpload={} retryInvalid={} publishPending={} publishReady={} publishWait={}/{} publishEdited={} publishLag={} publishRetry={} publishStale={} midClip={} midStart={} midEnd={} midCov={:.2f}/{:.2f} midTiles={}/{} midInterest={}/{} midAnchors={}/{} midGen={} midUpload={} midRetry={} midVoxels={}/{} midVoxInterest={}/{} midVoxelGen={} midVoxelUpload={} midVoxelEvict={} midBytesMB={:.2f} midSerial={}",
                    frameCount,
                    sparseRuntimeTestMode ? 1 : 0,
                    enableSparseSurfaceAuthoritative ? 1 : 0,
                    sparseWorldStats.residentBricks,
                    sparseWorldStats.requestedBricks,
                    sparseWorldStats.residentSpeculativeBricks,
                    sparseWorldStats.residentVisibleBricks,
                    sparseWorldStats.residentCollisionBricks,
                    sparseWorldStats.residentEditedBricks,
                    sparseWorldStats.generationQueuedSpeculativeBricks,
                    sparseWorldStats.generationQueuedVisibleBricks,
                    sparseWorldStats.generationQueuedCollisionBricks,
                    sparseWorldStats.generationQueuedEditedBricks,
                    sparseWorldStats.generationQueuedBricks,
                    sparseWorldStats.generatedSpeculativeBricksLastFrame,
                    sparseWorldStats.generatedVisibleBricksLastFrame,
                    sparseWorldStats.generatedCollisionBricksLastFrame,
                    sparseWorldStats.generatedEditedBricksLastFrame,
                    sparseWorldStats.generatedBricks,
                    sparseWorldStats.uploadQueuedSpeculativeBricks,
                    sparseWorldStats.uploadQueuedVisibleBricks,
                    sparseWorldStats.uploadQueuedCollisionBricks,
                    sparseWorldStats.uploadQueuedEditedBricks,
                    sparseWorldStats.uploadQueuedBricks,
                    sparseWorldStats.uploadedSpeculativeBricksLastFrame,
                    sparseWorldStats.uploadedVisibleBricksLastFrame,
                    sparseWorldStats.uploadedCollisionBricksLastFrame,
                    sparseWorldStats.uploadedEditedBricksLastFrame,
                    sparseWorldStats.surfaceQueuedSpeculativeBricks,
                    sparseWorldStats.surfaceQueuedVisibleBricks,
                    sparseWorldStats.surfaceQueuedCollisionBricks,
                    sparseWorldStats.surfaceQueuedEditedBricks,
                    sparseWorldStats.surfaceSpeculativeBricksExtractedLastFrame,
                    sparseWorldStats.surfaceVisibleBricksExtractedLastFrame,
                    sparseWorldStats.surfaceCollisionBricksExtractedLastFrame,
                    sparseWorldStats.surfaceEditedBricksExtractedLastFrame,
                    sparseWorldStats.freePages,
                    sparseWorldStats.evictedBricksLastFrame,
                    sparseWorldStats.evictionQueuedBricks,
                    sparseWorldStats.emptyRequestsSkippedLastFrame,
                    sparseWorldStats.knownEmptyGeneratedBricks,
                    sparseWorldStats.editedBricks,
                    sparseWorldStats.editedVoxels,
                    sparseWorldStats.renderDirtyBricks,
                    sparseWorldStats.renderDirtyRegionVoxels,
                    sparseWorldStats.renderDirtyVoxelsQueuedLastFrame,
                    sparseWorldStats.renderDirtyFullUploadsQueuedLastFrame,
                    sparseWorldStats.renderDirtyUploadDeferredLastFrame,
                    sparseWorldStats.renderDirtyNonResidentLastFrame,
                    sparseWorldStats.surfaceCachedBricks,
                    sparseWorldStats.surfaceFaces,
                    sparseWorldStats.surfaceUnitFaces,
                    sparseWorldStats.residentRenderableBricks,
                    sparseWorldStats.residentRenderableMissingSurfaces,
                    sparseWorldStats.surfaceBricksUpdatedLastFrame,
                    sparseWorldStats.surfaceBricksPartiallyUpdatedLastFrame,
                    sparseWorldStats.surfaceFacesRemovedByPartialUpdatesLastFrame,
                    sparseWorldStats.surfaceBricksRemovedLastFrame,
                    sparseWorldStats.surfaceUnitFacesGeneratedLastFrame,
                    sparseWorldStats.surfaceFacesGeneratedLastFrame,
                    sparseWorldStats.surfaceBricksExtractedLastFrame,
                    sparseWorldStats.surfaceExtractionQueuedBricks,
                    sparseSurfaceExtractionBudgetLastFrame,
                    sparseWorldStats.surfaceEmptyUploadsSkippedLastFrame,
                    sparseWorldStats.surfaceEmptyFastPathBricksLastFrame,
                    sparseWorldStats.surfaceSerial,
                    sparseWorldStats.brushVoxelsEvaluatedLastStroke,
                    sparseWorldStats.brushVoxelsEditedLastStroke,
                    sparseWorldStats.brushBricksTouchedLastStroke,
                    sparseWorldStats.brushBricksQueuedLastStroke,
                    sparseBrushStrokeDeltasLastFrame,
                    sparseBrushStrokeDeltaBricksLastFrame,
                    sparseBrushStrokeDeltaMismatchesLastFrame,
                    sparseBrushFeedbackQueuedLastFrame,
                    sparseBrushFeedbackRetiredLastFrame,
                    sparseBrushFeedbackAppliedLastFrame,
                    sparseBrushFeedbackOverflowLastFrame,
                    sparseBrushFeedbackMissingResidentLastFrame,
                    sparseBrushFeedbackCpuFallbackLastFrame,
                    sparseGpuStats.stagedBricksLastFrame,
                    sparseGpuStats.stagedPartialBrickUploadsLastFrame,
                    sparseGpuStats.stagedPartialCopyRangesLastFrame,
                    static_cast<double>(sparseGpuStats.stagedPartialVoxelBytesLastFrame) / 1024.0,
                    sparseGpuStats.stagedPageEntriesLastFrame,
                    static_cast<double>(sparseGpuStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                    static_cast<double>(sparseUploadRingUsedBytesLastFrame) / (1024.0 * 1024.0),
                    static_cast<double>(sparseUploadRingCapacityBytesLastFrame) / (1024.0 * 1024.0),
                    sparseUploadRingBudgetDefersLastFrame,
                    sparseGpuStats.uploadRingOverflowLastFrame ? 1 : 0,
                    sparseGpuStats.missFeedbackRecordsLastRetire,
                    sparseMissFeedbackPending.size(),
                    sparseMissFeedbackConsumedLastFrame,
                    sparseSpeculativeRequestsLastFrame,
                    sparseVisibleRequestsLastFrame,
                    sparseCollisionRequestsLastFrame,
                    sparseSpeculativeRequestBudgetLastFrame,
                    sparseVisibleRequestBudgetLastFrame,
                    sparseCollisionRequestBudgetLastFrame,
                    sparseTotalRequestBudgetLastFrame,
                    sparseProtectedRequestOverageLastFrame,
                    sparseRequestFreePageSkipsLastFrame,
                    sparseRequestClassBudgetSkipsLastFrame,
                    sparseRequestTotalBudgetSkipsLastFrame,
                    sparseRequestRejectedSkipsLastFrame,
                    sparseRequestKnownEmptySkipsLastFrame,
                    sparseBodyCollisionBlockedLastFrame,
                    sparseBodyCollisionStepUpsLastFrame,
                    sparseBodyCollisionVerticalBlockedLastFrame,
                    sparseBodyCollisionLandedLastFrame,
                    sparseBodyCollisionCeilingLastFrame,
                    sparseBodyCollisionGroundedLastFrame,
                    sparseBodyCollisionGroundSnapsLastFrame,
                    sparseBodyCollisionSampledLastFrame,
                    sparseBodyCollisionSolidLastFrame,
                    sparseBodyCollisionLiquidLastFrame,
                    sparseBodyCollisionSafeFractionLastFrame,
                    sparseStressRequestsLastFrame,
                    sparseStressAcceptedLastFrame,
                    sparseRuntimeBudgetScale,
                    sparseProtectedRuntimeBudgetScale,
                    sparseBackgroundRuntimeBudgetScale,
                    sparseProtectedBacklogLastFrame,
                    sparseTrimSpeculativeFirstLastFrame,
                    sparseRaymarchBudgetScale,
                    currentRenderQuality,
                    sparseBackgroundQualityTier,
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
                    logPublishStats.total,
                    logPublishStats.ready,
                    logPublishStats.waitingFrame,
                    logPublishStats.waitingFence,
                    logPublishStats.edited,
                    logPublishStats.maxReadyFrameLag,
                    sparsePageTablePublishRetriesLastFrame,
                    sparsePageTablePublishStaleDropsLastFrame,
                    sparseClipmapPolicy.IsEnabled() ? 1 : 0,
                    static_cast<uint32_t>(sparseClipmapPolicy.Config().startDistance),
                    static_cast<uint32_t>(sparseClipmapPolicy.Config().endDistance),
                    midResidencyLog.heightCoverageRatio,
                    midResidencyLog.voxelCoverageRatio,
                    midStats.residentTiles,
                    midStats.queuedTiles,
                    midHeightCoveredLog,
                    midStats.interestedTiles,
                    midStats.heightInterestAnchors,
                    midStats.voxelInterestAnchors,
                    midStats.generatedTilesLastFrame,
                    sparseGpuStats.stagedMidClipmapTilesLastFrame,
                    sparseMidClipmapUploadRetriesLastFrame,
                    midStats.residentVoxelBricks,
                    midStats.queuedVoxelBricks,
                    midVoxelCoveredLog,
                    midStats.interestedVoxelBricks,
                    midStats.generatedVoxelBricksLastFrame,
                    sparseGpuStats.stagedMidVoxelClipmapBricksLastFrame,
                    midStats.evictedVoxelBricksLastFrame,
                    static_cast<double>(sparseGpuStats.stagedMidClipmapBytesLastFrame) / (1024.0 * 1024.0),
                    std::max(sparseMidClipmapUploadedHeightSerial, sparseMidClipmapUploadedVoxelSerial));
                spdlog::info(
                    "PERF_SPARSE_OWNERSHIP_PRESSURE frame={} terrainPct={} missPct={} unsafeNearMissPct={} level={} effectiveLevel={} catchupActive={} catchupFrames={} deficit={} excess={} unsafeExcess={} pendingMiss={} feedback={}/{}/{} urgent={} readyFrame={} thresholdMiss={} thresholdTerrain={} catchupBudgetReq={}/{} gen={} upload={} surface={}",
                    frameCount,
                    sparseOwnershipTerrainPctLastRetire,
                    sparseOwnershipMissPctLastRetire,
                    sparseOwnershipUnsafeNearMissPctLastRetire,
                    sparseOwnershipPressureLevelActive,
                    computeSparseEffectiveOwnershipPressureLevel(),
                    sparseResidencyCatchupLastFrame,
                    sparseResidencyCatchupFramesRemaining,
                    sparseOwnershipPressureTerrainDeficitLastRetire,
                    sparseOwnershipPressureMissExcessLastRetire,
                    sparseOwnershipPressureUnsafeNearMissExcessLastRetire,
                    sparseMissFeedbackPending.size(),
                    sparseMissFeedbackRayGridLastFrame,
                    sparseMissFeedbackDistanceLastFrame,
                    sparseMissFeedbackStrideLastFrame,
                    sparseMissFeedbackUrgentLastFrame ? 1 : 0,
                    sparseOwnershipCatchupReadyFrame,
                    sparseOwnershipCatchupMissPct,
                    sparseOwnershipCatchupTerrainPct,
                    sparseCatchupVisibleRequestBudget,
                    sparseCatchupCollisionRequestBudget,
                    sparseCatchupGenerationBudget,
                    sparseCatchupUploadBudget,
                    sparseCatchupSurfaceBudget);
                if (sparseEditedPageTablePublishesQueuedLastFrame != 0u ||
                    sparseEditedPageTablePublishesPublishedLastFrame != 0u ||
                    sparseEditedPageTablePublishPromotionsLastFrame != 0u) {
                    spdlog::info(
                        "PERF_SPARSE_EDIT_PUBLISH frame={} queued={} published={} promoted={} pending={} ready={} wait={}/{} retry={} stale={}",
                        frameCount,
                        sparseEditedPageTablePublishesQueuedLastFrame,
                        sparseEditedPageTablePublishesPublishedLastFrame,
                        sparseEditedPageTablePublishPromotionsLastFrame,
                        logPublishStats.total,
                        logPublishStats.ready,
                        logPublishStats.waitingFrame,
                        logPublishStats.waitingFence,
                        sparsePageTablePublishRetriesLastFrame,
                        sparsePageTablePublishStaleDropsLastFrame);
                }
                if (enableSparseLocalPhysics ||
                    sparseWorldStats.physicsCandidateBricks != 0 ||
                    sparseWorldStats.physicsMovedVoxelsLastFrame != 0) {
                    spdlog::info(
                        "PERF_SPARSE_PHYSICS frame={} enabled={} queue={} hot={} warm={} packets={} hotPackets={} warmPackets={} regionVoxels={} supportReq={} gpuPackets={} gpuKB={:.2f} editDeltas={} editRanges={} editTable={} editKB={:.2f} editOverflow={} gpuOverflow={} gpuRbPackets={} gpuResults={} gpuProposals={} gpuMissingBelow={} gpuApply={}/{} gpuReject={} gpuMask=0x{:X} gpuChecksum={} gpuRbFrame={} gpuMaxPri={} gpuGenXor={} gpuResultChecksum={} processed={} moved={} skipped={} budget={}/{}",
                        frameCount,
                        enableSparseLocalPhysics ? 1 : 0,
                        sparseWorldStats.physicsCandidateBricks,
                        sparseWorldStats.physicsHotCandidateBricks,
                        sparseWorldStats.physicsWarmCandidateBricks,
                        sparseWorldStats.physicsWorkPacketsLastFrame,
                        sparseWorldStats.physicsHotWorkPacketsLastFrame,
                        sparseWorldStats.physicsWarmWorkPacketsLastFrame,
                        sparseWorldStats.physicsDirtyRegionVoxelsLastFrame,
                        sparseWorldStats.physicsSupportBricksRequestedLastFrame,
                        sparseGpuStats.stagedPhysicsPacketsLastFrame,
                        static_cast<double>(sparseGpuStats.stagedPhysicsPacketBytesLastFrame) / 1024.0,
                        sparseGpuStats.stagedEditDeltasLastFrame,
                        sparseGpuStats.stagedEditDeltaRangesLastFrame,
                        sparseGpuStats.stagedEditDeltaRangeTableEntriesLastFrame,
                        static_cast<double>(sparseGpuStats.stagedEditDeltaBytesLastFrame) / 1024.0,
                        sparseGpuStats.editDeltaUploadOverflowLastFrame ? 1 : 0,
                        sparseGpuStats.physicsPacketUploadOverflowLastFrame ? 1 : 0,
                        sparseGpuStats.physicsGpuPacketsLastRetire,
                        sparseGpuStats.physicsGpuResultCountLastRetire,
                        sparseGpuStats.physicsGpuProposalCountLastRetire,
                        sparseGpuStats.physicsGpuMissingBelowCountLastRetire,
                        sparseWorldStats.physicsGpuAppliedMovesLastFrame,
                        sparseWorldStats.physicsGpuProcessedProposalsLastFrame,
                        sparseWorldStats.physicsGpuRejectedProposalsLastFrame,
                        sparseGpuStats.physicsGpuMaterialMaskLastRetire,
                        sparseGpuStats.physicsGpuChecksumLastRetire,
                        sparseGpuStats.physicsGpuFrameLastRetire,
                        sparseGpuStats.physicsGpuMaxPriorityLastRetire,
                        sparseGpuStats.physicsGpuGenerationXorLastRetire,
                        sparseGpuStats.physicsGpuResultChecksumLastRetire,
                        sparseWorldStats.physicsProcessedBricksLastFrame,
                        sparseWorldStats.physicsMovedVoxelsLastFrame,
                        sparseWorldStats.physicsSkippedVoxelsLastFrame,
                        sparsePhysicsBrickBudgetLastFrame,
                        sparsePhysicsMoveBudgetLastFrame);
                }
                if (sparseSurfaceGpuResources.IsInitialized()) {
                    const auto& sparseSurfaceGpuStats = sparseSurfaceGpuResources.GetStats();
                    spdlog::info(
                        "PERF_SPARSE_SURFACE frame={} cpuBricks={} cpuFaces={} cpuUnitFaces={} faceRatio={:.2f} cpuSerial={} pendingDirty={} pendingRemoved={} gpuFaces={} gpuRanges={} gpuRangeTable={} gpuDrawCmds={} gpuActiveDraw={} gpuRecords={} gpuClusters={} clusterSize={} clusterExtent={} clusterFast={}/{} iaFaces={} iaMB={:.2f}/{:.2f} iaGpu={} iaUpload={} iaFence={} gpuCull={} gpuCullDispatch={} gpuCullCand={}/{} gpuCullAccepted={} gpuCullClusterDraws={} gpuCullRecordDraws={} gpuCullFast={} gpuCullReject={}/{}/{}/{}/{} gpuCullOverflow={} gpuCullRB={}/{}:{} gpuSerial={} stagedFaces={} stagedRanges={} stagedRangeTable={} stagedDrawCmds={} stagedActiveDraw={} stagedRecords={} stagedClusters={} drawSlots={}/{} stableDraw={} compactDraw={} inactiveSlots={} copyRegions={} dirtyCopied={} cleanSkipped={} deferred={} patch={}/{}/{} residentPayload={} copyBudget={}/{} metaRange={}{} fixed={} rangeSkip={} metaDraw={}{} drawSkip={} metaRec={}{} recSkip={} metaCluster={}{} clusterSkip={} pendingSnapDirty={} pendingSnapRemoved={} stagedMB={:.2f} cand={} vis={} cull={} look={} alloc={} allocCap={} freeRanges={} largestFree={} retirePending={}/{} allocFail={} rasterFaces={} retry={} overflow={}",
                        frameCount,
                        sparseWorldStats.surfaceCachedBricks,
                        sparseWorldStats.surfaceFaces,
                        sparseWorldStats.surfaceUnitFaces,
                        sparseWorldStats.surfaceUnitFaces == 0u
                            ? 1.0
                            : static_cast<double>(sparseWorldStats.surfaceFaces) /
                                static_cast<double>(sparseWorldStats.surfaceUnitFaces),
                        sparseWorldStats.surfaceSerial,
                        sparseWorldStats.surfacePendingGpuDirtyBricks,
                        sparseWorldStats.surfacePendingGpuRemovedBricks,
                        sparseSurfaceGpuStats.uploadedFaces,
                        sparseSurfaceGpuStats.uploadedRanges,
                        sparseSurfaceGpuStats.uploadedRangeTableCapacity,
                        sparseSurfaceGpuStats.uploadedDrawCommands,
                        sparseSurfaceGpuStats.uploadedActiveDrawCommands,
                        sparseSurfaceGpuStats.uploadedSurfaceRecords,
                        sparseSurfaceGpuStats.uploadedSurfaceClusters,
                        sparseSurfaceGpuStats.surfaceRecordsPerCluster,
                        sparseSurfaceGpuStats.surfaceClusterMaxExtentVoxels,
                        sparseSurfaceGpuStats.surfaceClusterFastAcceptMaxRecords,
                        sparseSurfaceGpuStats.surfaceClusterFastAcceptMaxFaces,
                        sparseSurfaceGpuStats.iaStreamCapacityFaces,
                        static_cast<double>(sparseSurfaceGpuStats.iaStreamVertexBytes) / (1024.0 * 1024.0),
                        static_cast<double>(sparseSurfaceGpuStats.iaStreamIndexBytes) / (1024.0 * 1024.0),
                        sparseSurfaceGpuStats.iaStreamGpuLocal ? 1 : 0,
                        sparseSurfaceGpuStats.iaStreamUploadPending ? 1 : 0,
                        sparseSurfaceGpuStats.iaStreamUploadRetireFence,
                        sparseSurfaceGpuStats.gpuCullEnabled ? 1 : 0,
                        sparseSurfaceGpuStats.gpuCullDispatchesLastFrame,
                        sparseSurfaceGpuStats.gpuCullCandidateRecordsLastFrame,
                        sparseSurfaceGpuStats.gpuCullCandidateClustersLastFrame,
                        sparseSurfaceGpuStats.gpuCullAcceptedDraws,
                        sparseSurfaceGpuStats.gpuCullAcceptedClusterDraws,
                        sparseSurfaceGpuStats.gpuCullAcceptedRecordDraws,
                        sparseSurfaceGpuStats.gpuCullFastAcceptedClusterRecords,
                        sparseSurfaceGpuStats.gpuCullRejectedInvalid,
                        sparseSurfaceGpuStats.gpuCullRejectedDistance,
                        sparseSurfaceGpuStats.gpuCullRejectedFrustum,
                        sparseSurfaceGpuStats.gpuCullRejectedBackface,
                        sparseSurfaceGpuStats.gpuCullRejectedClusters,
                        sparseSurfaceGpuStats.gpuCullOverflow,
                        sparseSurfaceGpuStats.gpuCullStatsReadbacksRetired,
                        sparseSurfaceGpuStats.gpuCullStatsReadbacksQueued,
                        sparseSurfaceGpuStats.gpuCullStatsReadbackPending,
                        sparseSurfaceGpuStats.uploadedSerial,
                        sparseSurfaceGpuStats.stagedFacesLastFrame,
                        sparseSurfaceGpuStats.stagedRangesLastFrame,
                        sparseSurfaceGpuStats.stagedRangeTableCapacityLastFrame,
                        sparseSurfaceGpuStats.stagedDrawCommandsLastFrame,
                        sparseSurfaceGpuStats.activeDrawCommandsLastFrame,
                        sparseSurfaceGpuStats.stagedSurfaceRecordsLastFrame,
                        sparseSurfaceGpuStats.stagedSurfaceClustersLastFrame,
                        sparseSurfaceGpuStats.stableDrawSlotCapacity,
                        sparseSurfaceGpuStats.stableDrawFreeSlots,
                        sparseSurfaceGpuStats.stableDrawSlotsEnabled ? 1 : 0,
                        sparseSurfaceGpuStats.compactStableDrawCommandsEnabled ? 1 : 0,
                        sparseSurfaceGpuStats.inactiveStableDrawSlotsLastFrame,
                        sparseSurfaceGpuStats.stagedFaceCopyRegionsLastFrame,
                        sparseSurfaceGpuStats.stagedDirtyPayloadBricksLastFrame,
                        sparseSurfaceGpuStats.skippedCleanPayloadBricksLastFrame,
                        sparseSurfaceGpuStats.deferredPayloadBricksLastFrame,
                        sparseSurfaceGpuStats.stagedPayloadPatchBricksLastFrame,
                        sparseSurfaceGpuStats.stagedPayloadPatchFacesLastFrame,
                        sparseSurfaceGpuStats.stagedPayloadPatchRegionsLastFrame,
                        sparseSurfaceGpuStats.residentPayloadBricks,
                        sparseSurfaceGpuStats.payloadCopyRegionBudget,
                        sparseSurfaceGpuStats.payloadCopyFaceBudget,
                        sparseSurfaceGpuStats.stagedRangeCopyRegionsLastFrame,
                        sparseSurfaceGpuStats.fullRangeTableUploadLastFrame ? "F" : "I",
                        sparseSurfaceGpuStats.fixedRangeTableEnabled ? 1 : 0,
                        sparseSurfaceGpuStats.skippedCleanRangeSlotsLastFrame,
                        sparseSurfaceGpuStats.stagedDrawCopyRegionsLastFrame,
                        sparseSurfaceGpuStats.fullDrawArgsUploadLastFrame ? "F" : "I",
                        sparseSurfaceGpuStats.skippedCleanDrawCommandsLastFrame,
                        sparseSurfaceGpuStats.stagedSurfaceRecordCopyRegionsLastFrame,
                        sparseSurfaceGpuStats.fullSurfaceRecordUploadLastFrame ? "F" : "I",
                        sparseSurfaceGpuStats.skippedCleanSurfaceRecordsLastFrame,
                        sparseSurfaceGpuStats.stagedSurfaceClusterCopyRegionsLastFrame,
                        sparseSurfaceGpuStats.fullSurfaceClusterUploadLastFrame ? "F" : "I",
                        sparseSurfaceGpuStats.skippedCleanSurfaceClustersLastFrame,
                        sparseSurfaceGpuStats.pendingDirtyBricksLastFrame,
                        sparseSurfaceGpuStats.pendingRemovedBricksLastFrame,
                        static_cast<double>(sparseSurfaceGpuStats.stagedBytesLastFrame) / (1024.0 * 1024.0),
                        sparseSurfaceGpuStats.uploadedCandidateBricks,
                        sparseSurfaceGpuStats.uploadedVisibleBricks,
                        sparseSurfaceGpuStats.uploadedCulledBricks,
                        sparseSurfaceLookaheadVisibleLastUpload,
                        sparseSurfaceGpuStats.allocatedFaceRanges,
                        sparseSurfaceGpuStats.allocatedFaceCapacity,
                        sparseSurfaceGpuStats.freeFaceRanges,
                        sparseSurfaceGpuStats.largestFreeFaceRange,
                        sparseSurfaceGpuStats.pendingRetiredFaceRanges,
                        sparseSurfaceGpuStats.pendingRetiredFaceCapacity,
                        sparseSurfaceGpuStats.faceRangeAllocationFailures,
                        sparseSurfaceRasterFacesLastFrame,
                        sparseSurfaceUploadRetriesLastFrame,
                        sparseSurfaceGpuStats.uploadOverflowLastFrame ? 1 : 0);
                }
                if (sparseFastRequestScaleLastFrame > 1u) {
                spdlog::info(
                    "PERF_SPARSE_FAST_REQUEST frame={} scale={} speedThreshold={} spec/vis/coll={} / {} / {} total={} brushIntent={} brushReserve={}/{} skips={}/{}/{}",
                    frameCount,
                    sparseFastRequestScaleLastFrame,
                    sparseFastRequestSpeed,
                        sparseSpeculativeRequestBudgetLastFrame,
                        sparseVisibleRequestBudgetLastFrame,
                        sparseCollisionRequestBudgetLastFrame,
                        sparseTotalRequestBudgetLastFrame,
                        sparseBrushIntentActiveLastFrame,
                        sparseBrushCollisionReserveLastFrame,
                        sparseBrushCollisionMaxLastFrame,
                    sparseRequestFreePageSkipsLastFrame,
                    sparseRequestClassBudgetSkipsLastFrame,
                    sparseRequestTotalBudgetSkipsLastFrame);
                if (enableSparseGpuRaycast) {
                    const uint32_t gpuRaycastAuthoritativeSamples =
                        sparseGpuRaycastAcceptedSinceReady + sparseGpuRaycastFallbackSinceReady;
                    const uint32_t fallbackPct =
                        gpuRaycastAuthoritativeSamples == 0u
                            ? 100u
                            : static_cast<uint32_t>(
                                (static_cast<uint64_t>(sparseGpuRaycastFallbackSinceReady) * 100ull) /
                                std::max<uint32_t>(1u, gpuRaycastAuthoritativeSamples));
                    spdlog::info(
                        "PERF_SPARSE_GPU_RAYCAST frame={} frameA/R/M/F={}/{}/{}/{} readyA/R/M/F={}/{}/{}/{} fallbackPct={} health={}",
                        frameCount,
                        sparseGpuRaycastAcceptedLastFrame,
                        sparseGpuRaycastRejectedLastFrame,
                        sparseGpuRaycastMissLastFrame,
                        sparseGpuRaycastFallbackLastFrame,
                        sparseGpuRaycastAcceptedSinceReady,
                        sparseGpuRaycastRejectedSinceReady,
                        sparseGpuRaycastMissSinceReady,
                        sparseGpuRaycastFallbackSinceReady,
                        fallbackPct,
                        sparseGpuRaycastHealthObserved ? 1 : 0);
                }
            }
        }
        }

        if (!hideUiForCapture) {
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
        }
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} imgui-built", frameCount);
        }

        // Render ImGui draw data to command list
        imguiBackend.Render(commandList.Get());
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} imgui-rendered", frameCount);
        }

        if (sparseCpuRaycastAuthoritative) {
            brushQueryMetadata[frameIndex] = {};
            groundQueryMetadata[frameIndex] = {};
            if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
                spdlog::info("FRAME_STAGE {} raycast-readbacks-skipped sparse-cpu-authoritative", frameCount);
            }
        } else {
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
        }

        if (backbufferCapture.enabled &&
            backbufferCapturesQueued < backbufferCapture.count &&
            frameCount >= backbufferCapture.startFrame &&
            ((frameCount - backbufferCapture.startFrame) % backbufferCapture.intervalFrames) == 0u) {
            PendingBackbufferCapture capture = {};
            if (QueueBackbufferCapture(
                    device->GetDevice(),
                    commandList.Get(),
                    window->GetBackBuffer(frameIndex),
                    static_cast<uint32_t>(frameCount),
                    backbufferCapture.outputDir,
                    capture)) {
                pendingBackbufferCaptures.push_back(std::move(capture));
                ++backbufferCapturesQueued;
            }
        }

        // End frame - transitions back buffer to present state
        renderer->EndFrame(commandList.Get(), frameIndex);
        if (traceFrameStages && frameCount < kFrameStageTraceLimit) {
            spdlog::info("FRAME_STAGE {} render-end", frameCount);
        }
        if (gpuTimestampHeap && gpuTimestampReadback) {
            commandList->EndQuery(gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuTimestampBase + 6);
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
        if (!pendingBackbufferCaptures.empty()) {
            commandQueue->WaitForFenceValue(ctx.fenceValue);
            for (const PendingBackbufferCapture& capture : pendingBackbufferCaptures) {
                WriteBackbufferBmp(capture);
            }
            pendingBackbufferCaptures.clear();
        }
        voxelWorld->NotifyBrushEditFeedbackFence(ctx.fenceValue);

        // Submit background chunk generation after the frame has been queued.
        // This keeps generation from sitting in front of the render pass on the
        // direct queue while still letting it use trailing GPU time.
        voxelWorld->SetChunkGenerationBudget(trailingGenerationBudget);
        voxelWorld->PumpChunkGeneration(device->GetDevice(), commandQueue->GetCommandQueue());
        {
            Simulation::SparseFramePredictionInput predictionInput;
            predictionInput.previousPredictedFrameMs = schedulerPredictedFrameMs;
            predictionInput.rawFrameMs = lastRawFrameMs;
            predictionInput.gpuFrameMs = gpuTiming.valid ? static_cast<float>(gpuTiming.frameMs) : 0.0f;
            predictionInput.chunkUpdateMs = perfChunkUpdateMs;
            predictionInput.physicsSubmitMs = perfPhysicsSubmitMs;
            predictionInput.brushSubmitMs = perfBrushSubmitMs;
            predictionInput.presentMs = perfPresentMs;
            const Simulation::SparseFramePrediction prediction =
                Simulation::SparseRuntimeBudgetScheduler::BuildFramePrediction(predictionInput);
            schedulerPredictedFrameMs = prediction.predictedFrameMs;
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

    if (enableSparseEditFile && sparseVoxelWorldReady) {
        if (sparseVoxelWorld.SaveEditsToFile(sparseEditFilePath)) {
            spdlog::info(
                "Saved sparse edit overlays to {} (bricks={} voxels={})",
                sparseEditFilePath.string(),
                sparseVoxelWorld.GetEdits().EditedBrickCount(),
                sparseVoxelWorld.GetEdits().EditedVoxelCount());
        } else {
            spdlog::warn("Failed to save sparse edit overlays to {}", sparseEditFilePath.string());
        }
    }

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

    if (sparseBackendPipeReadyFailed) {
        spdlog::critical(
            "VENPOD sparse backend readiness smoke failed. missingMask=0x{:X} [{}]",
            sparseBackendPipeReadyFailureMask,
            sparsePipeMaskNames(sparseBackendPipeReadyFailureMask));
        return 2;
    }
    if (requireSparsePipeReady && !sparseBackendPipeReadyObserved) {
        spdlog::critical(
            "VENPOD sparse backend readiness smoke did not observe a post-ready clean sample. readyFrame={} totalFrames={}",
            sparsePipeReadyFrame,
            frameCount);
        return 4;
    }
    if (sparseGpuRaycastHealthFailed) {
        spdlog::critical(
            "VENPOD sparse GPU raycast health smoke failed. sampleFrame={} accepted={} fallback={} rejected={} miss={} fallbackPct={} maxFallbackPct={} minAccepted={}",
            sparseGpuRaycastHealthFailureFrame,
            sparseGpuRaycastAcceptedSinceReady,
            sparseGpuRaycastFallbackSinceReady,
            sparseGpuRaycastRejectedSinceReady,
            sparseGpuRaycastMissSinceReady,
            sparseGpuRaycastFallbackPctAtFailure,
            sparseGpuRaycastMaxFallbackPct,
            sparseGpuRaycastMinAccepted);
        return 12;
    }
    if (requireSparseGpuRaycastHealth && !sparseGpuRaycastHealthObserved) {
        const uint32_t gpuRaycastAuthoritativeSamples =
            sparseGpuRaycastAcceptedSinceReady + sparseGpuRaycastFallbackSinceReady;
        const uint32_t fallbackPct =
            gpuRaycastAuthoritativeSamples == 0u
                ? 100u
                : static_cast<uint32_t>(
                    (static_cast<uint64_t>(sparseGpuRaycastFallbackSinceReady) * 100ull) /
                    std::max<uint32_t>(1u, gpuRaycastAuthoritativeSamples));
        spdlog::critical(
            "VENPOD sparse GPU raycast health smoke did not observe enough usable GPU hits. readyFrame={} totalFrames={} accepted={} fallback={} rejected={} miss={} fallbackPct={} maxFallbackPct={} minAccepted={}",
            sparseGpuRaycastHealthReadyFrame,
            frameCount,
            sparseGpuRaycastAcceptedSinceReady,
            sparseGpuRaycastFallbackSinceReady,
            sparseGpuRaycastRejectedSinceReady,
            sparseGpuRaycastMissSinceReady,
            fallbackPct,
            sparseGpuRaycastMaxFallbackPct,
            sparseGpuRaycastMinAccepted);
        return 13;
    }
    if (sparseOwnershipQualityFailed) {
        spdlog::critical(
            "VENPOD sparse render ownership quality smoke failed. sampleFrame={} terrain={}%% miss={}%% unsafeNearMiss={}%%",
            sparseOwnershipQualityFrame,
            sparseOwnershipQualityTerrainPct,
            sparseOwnershipQualityMissPct,
            sparseOwnershipQualityUnsafeNearMissPct);
        return 3;
    }
    if (sparseOwnershipStabilityFailed) {
        spdlog::critical(
            "VENPOD sparse render ownership stability smoke failed. sampleFrame={} previousFrame={} terrainDelta={}%% missDelta={}%%",
            sparseOwnershipStabilityFrame,
            sparseOwnershipStabilityFailurePreviousFrame,
            sparseOwnershipStabilityTerrainDeltaPct,
            sparseOwnershipStabilityMissDeltaPct);
        return 6;
    }
    if (sparseSurfaceFragmentsFailed) {
        spdlog::critical(
            "VENPOD sparse surface-fragment smoke failed. sampleFrame={} fragments={} min={}",
            sparseSurfaceFragmentsFrame,
            sparseSurfaceFragmentsLastRetire,
            sparseMinSurfaceFragments);
        return 8;
    }
    if (requireSparseOwnershipQuality && !sparseOwnershipQualityObserved) {
        spdlog::critical(
            "VENPOD sparse render ownership quality smoke did not observe a post-ready ownership sample. readyFrame={} totalFrames={}",
            sparseOwnershipQualityReadyFrame,
            frameCount);
        return 5;
    }
    if (requireSparseOwnershipStability && !sparseOwnershipStabilityObserved) {
        spdlog::critical(
            "VENPOD sparse render ownership stability smoke did not observe two post-ready ownership samples. readyFrame={} totalFrames={}",
            sparseOwnershipStabilityReadyFrame,
            frameCount);
        return 7;
    }
    if (requireSparseSurfaceFragments && !sparseSurfaceFragmentsObserved) {
        spdlog::critical(
            "VENPOD sparse surface-fragment smoke did not observe a post-ready surface sample. readyFrame={} totalFrames={} lastFragments={}",
            sparseSurfaceFragmentsReadyFrame,
            frameCount,
            sparseSurfaceFragmentsLastRetire);
        return 9;
    }

    spdlog::info("VENPOD shut down cleanly. Total frames: {}", frameCount);
    return 0;
}
