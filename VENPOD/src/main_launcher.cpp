// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Entry Point
// =============================================================================

#include "Core/Window.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/DX12CommandQueue.h"
#include "Graphics/Renderer.h"
#include "Graphics/FarVoxelOctree.h"
#include "Simulation/VoxelWorld.h"
#include "Simulation/TerrainConstants.h"
#include "Simulation/PhysicsDispatcher.h"
#include "Simulation/ChunkManager.h"
#include "Simulation/ChunkGenerationTest.h"  // INFINITE CHUNK TEST HARNESS
#include "Simulation/ChunkStressTest.h"      // STRESS TESTING FRAMEWORK
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
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <array>

using namespace VENPOD;
using namespace VENPOD::Graphics;

// Frame synchronization
static constexpr uint32_t kFrameCount = Window::BUFFER_COUNT;

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    uint64_t fenceValue = 0;
};

struct GroundQueryMetadata {
    bool valid = false;
    glm::vec3 regionOriginWorld{0.0f};
    glm::vec3 feetWorld{0.0f};
};

struct BrushQueryMetadata {
    bool valid = false;
    glm::vec3 regionOriginWorld{0.0f};
    glm::vec3 originWorld{0.0f};
};

struct BuildStrokeState {
    bool active = false;
    float rayDistance = 64.0f;
};

static glm::ivec3 DecodePackedNormal(uint32_t packedNormal, bool& valid) {
    valid = ((packedNormal >> 6) & 0x1u) != 0;
    return glm::ivec3(
        static_cast<int32_t>(packedNormal & 0x3u) - 1,
        static_cast<int32_t>((packedNormal >> 2) & 0x3u) - 1,
        static_cast<int32_t>((packedNormal >> 4) & 0x3u) - 1
    );
}

static glm::vec3 ApplyCloseTraversalBrushFallback(
    const glm::vec3& rawBrushLocal,
    const glm::vec3& cameraLocal,
    const glm::vec3& cameraForward,
    float playerHeight,
    float playerRadius,
    float brushRadius,
    bool buildStroke,
    bool* adjustedOut = nullptr)
{
    if (adjustedOut) {
        *adjustedOut = false;
    }
    if (!buildStroke) {
        return rawBrushLocal;
    }

    const glm::vec3 feetLocal = cameraLocal - glm::vec3(0.0f, playerHeight, 0.0f);
    glm::vec3 flatToBrush(rawBrushLocal.x - feetLocal.x, 0.0f, rawBrushLocal.z - feetLocal.z);
    float horizontalDistance = glm::length(flatToBrush);

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

    const float closeRampStart = std::max(56.0f, brushRadius * 11.0f);
    const float nearSnapDistance = std::max(1.0f, std::min(playerRadius + brushRadius * 0.25f, brushRadius * 0.45f));
    if (horizontalDistance > closeRampStart) {
        return rawBrushLocal;
    }

    const float forwardDot = glm::dot(rampDirection, flatForward);
    if (forwardDot < 0.35f && horizontalDistance > brushRadius * 1.5f) {
        return rawBrushLocal;
    }

    glm::vec3 adjusted = rawBrushLocal;
    if (horizontalDistance < nearSnapDistance) {
        // Finish the traversal stroke under/slightly ahead of the player so
        // the last painted blobs become walkable instead of face blockers.
        adjusted.x = feetLocal.x + flatForward.x * 0.75f;
        adjusted.z = feetLocal.z + flatForward.z * 0.75f;
        horizontalDistance = 0.0f;
    }

    // Cap the brush center to a shallow walkable ramp as it approaches the
    // player. This reaches foot level at the end instead of stopping early.
    const float nearCenterY = feetLocal.y - std::max(0.35f, brushRadius * 0.35f);
    constexpr float kTraversalRampSlope = 0.14f;  // Roughly 1 voxel up per 7 voxels forward.
    const float rampY = nearCenterY + horizontalDistance * kTraversalRampSlope;
    if (rampY < adjusted.y) {
        adjusted.y = rampY;
    }

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


int RunSandbox(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // DEBUG MODE: When true, bypass streaming and use a fixed 2x2 static
    // chunk layout copied into the 256x128x256 voxel buffer each frame.
    // This isolates copy/origin bugs from the infinite chunk streaming logic.
    const bool useStaticChunkLayout = std::getenv("VENPOD_STATIC_CHUNKS") != nullptr;
    const bool disablePhysics = std::getenv("VENPOD_DISABLE_PHYSICS") != nullptr;
    const bool enableInfinitePhysics = std::getenv("VENPOD_ENABLE_INFINITE_PHYSICS") != nullptr;
    const bool enableDiagnostics = std::getenv("VENPOD_DIAGNOSTICS") != nullptr;
    const bool enableRuntimeLog = enableDiagnostics || std::getenv("VENPOD_LOG_FILE") != nullptr;
    const bool enableD3DDebug = std::getenv("VENPOD_D3D_DEBUG") != nullptr;
    const bool enableBoundaryTest = std::getenv("VENPOD_BOUNDARY_TEST") != nullptr;
    const bool enableFarSVO = std::getenv("VENPOD_DISABLE_FAR_SVO") == nullptr;

    if (enableRuntimeLog) {
        auto logPath = GetExecutableDirectorySandbox() / "venpod_runtime.log";
        auto fileLogger = spdlog::basic_logger_mt("venpod_file", logPath.string(), true);
        spdlog::set_default_logger(fileLogger);
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("  Log path: {}", logPath.string());
    }

    spdlog::set_level(enableDiagnostics ? spdlog::level::debug : spdlog::level::warn);
    spdlog::info("===========================================");
    spdlog::info("  VENPOD - Voxel Physics Engine v0.1.0");
    spdlog::info("  Target: 100M+ Active Voxels @ 60 FPS");
    spdlog::info("  Static chunks: {} | Physics disabled: {} | Infinite physics: {} | Diagnostics: {} | Boundary test: {} | Far SVO: {}",
        useStaticChunkLayout ? "yes" : "no",
        disablePhysics ? "yes" : "no",
        enableInfinitePhysics ? "yes" : "no",
        enableDiagnostics ? "yes" : "no",
        enableBoundaryTest ? "yes" : "no",
        enableFarSVO ? "yes" : "no");
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

    FarVoxelOctree farVoxelOctree;
    Renderer::SparseFarField sparseFarField = {};
    if (enableFarSVO) {
        FarVoxelOctreeConfig farConfig;
        auto farResult = farVoxelOctree.Initialize(
            device->GetDevice(),
            renderer->GetHeapManager(),
            farConfig);
        if (!farResult) {
            spdlog::warn("Far sparse voxel octree disabled: {}", farResult.error());
        } else {
            const auto& farStats = farVoxelOctree.GetStats();
            sparseFarField.nodeSRV = farVoxelOctree.GetNodeSRV();
            sparseFarField.pageSRV = farVoxelOctree.GetPageSRV();
            sparseFarField.nodeCount = farStats.nodeCount;
            sparseFarField.pageCount = farStats.pageCount;
            sparseFarField.pageSize = farStats.pageSize;
            sparseFarField.enabled = true;
        }
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

    // Initialize VoxelWorld
    auto voxelWorld = std::make_unique<Simulation::VoxelWorld>();
    Simulation::VoxelWorldConfig voxelConfig;
    // Moving 3D render window: 15x7x15 chunks. This preserves the large-world
    // feel while giving extreme terrain enough above-camera headroom to avoid
    // visible clipping planes at spawn. Longer distance needs a future far-LOD
    // pass rather than expanding the dense editable buffer horizontally.
    voxelConfig.gridSizeX = Simulation::RENDER_BUFFER_VOXELS_X;
    voxelConfig.gridSizeY = Simulation::RENDER_BUFFER_VOXELS_Y;
    voxelConfig.gridSizeZ = Simulation::RENDER_BUFFER_VOXELS_Z;

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
    if (!voxelWorld->IsUsingInfiniteChunks()) {
        physicsDispatcher->DispatchInitialize(initCommandList.Get(), *voxelWorld, 12345);

        // CRITICAL: Swap buffers so the initialized data becomes the "read" buffer
        // DispatchInitialize writes to the WRITE buffer, so we need to swap
        // to make that data available as the READ buffer for rendering
        voxelWorld->SwapBuffers();
        spdlog::info("Initialized 256^3 voxel grid with procedural terrain (CS_Initialize)");
    } else {
        // CRITICAL FIX: Clear both voxel buffers to air (0) before using infinite chunks!
        // Without this, uninitialized GPU memory contains garbage that the raymarcher
        // interprets as random terrain, causing terrain to appear in wrong locations.
        spdlog::info("Clearing voxel buffers to air for infinite chunk mode...");

        // Transition both buffers to UAV state for clearing
        voxelWorld->TransitionReadBufferTo(initCommandList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        voxelWorld->TransitionWriteBufferTo(initCommandList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Clear both buffers using ClearUnorderedAccessViewUint
        // This sets all voxels to 0 (MAT_AIR)
        UINT clearValues[4] = {0, 0, 0, 0};
        auto& heapManager = renderer->GetHeapManager();
        ID3D12DescriptorHeap* heaps[] = { heapManager.GetShaderVisibleCbvSrvUavHeap() };
        initCommandList->SetDescriptorHeaps(1, heaps);

        // Clear READ buffer
        initCommandList->ClearUnorderedAccessViewUint(
            voxelWorld->GetReadBufferUAV().gpu,
            voxelWorld->GetReadBufferUAV().cpu,
            voxelWorld->GetReadBuffer().GetResource(),
            clearValues,
            0, nullptr
        );

        // Clear WRITE buffer
        initCommandList->ClearUnorderedAccessViewUint(
            voxelWorld->GetWriteBufferUAV().gpu,
            voxelWorld->GetWriteBufferUAV().cpu,
            voxelWorld->GetWriteBuffer().GetResource(),
            clearValues,
            0, nullptr
        );

        // UAV barriers to ensure clears complete
        D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
        uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[0].UAV.pResource = voxelWorld->GetReadBuffer().GetResource();
        uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[1].UAV.pResource = voxelWorld->GetWriteBuffer().GetResource();
        initCommandList->ResourceBarrier(2, uavBarriers);

        spdlog::info("Both voxel buffers cleared to air - ready for infinite chunk streaming");
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
    spdlog::info("Controls: ESC=Pause, WASD=Move, Mouse=Look, Space=Jump/Fly, Double-Space=Toggle Flight, V=Perspective, Tab=Toggle Mouse, LMB=Paint, RMB=Erase");

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
    bool terrainReady = false;

    // Player position represents feet/collision point
    // Camera rendering position is offset upward by playerHeight for natural eye-level view

    // Main loop
    bool running = true;
    bool paused = false;
    uint64_t frameCount = 0;
    bool mouseInitialized = false;  // Track if mouse capture has been enabled
    uint64_t lastFrameCounter = SDL_GetPerformanceCounter();
    const double performanceFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    float smoothedFrameMs = 16.67f;
    float lastRawFrameMs = 16.67f;
    uint64_t physicsDispatchCount = 0;
    uint64_t physicsBudgetSkipCount = 0;
    uint32_t currentCopyBudget = voxelWorld->GetMaxChunkCopiesPerFrame();
    bool hasCompletedGroundQuery = false;
    bool hasCompletedBrushQuery = false;
    glm::vec3 completedGroundQueryRegionOriginWorld(0.0f);
    glm::vec3 completedGroundQueryFeetWorld(0.0f);
    glm::vec3 completedBrushQueryRegionOriginWorld(0.0f);
    glm::vec3 completedBrushQueryOriginWorld(0.0f);
    glm::vec3 nextGroundQueryRegionOriginWorld(0.0f);
    glm::vec3 nextGroundQueryFeetWorld(0.0f);
    glm::vec3 nextBrushQueryRegionOriginWorld(0.0f);
    glm::vec3 nextBrushQueryOriginWorld(0.0f);
    std::array<GroundQueryMetadata, kFrameCount> groundQueryMetadata = {};
    std::array<BrushQueryMetadata, kFrameCount> brushQueryMetadata = {};
    BuildStrokeState buildStrokeState;
    glm::vec3 lastBoundaryTestCameraWorld = cameraPos;
    float boundaryTestElapsedSeconds = 0.0f;

    while (running) {
        uint64_t currentFrameCounter = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(
            static_cast<double>(currentFrameCounter - lastFrameCounter) / performanceFrequency);
        lastFrameCounter = currentFrameCounter;
        lastRawFrameMs = dt * 1000.0f;
        smoothedFrameMs = smoothedFrameMs * 0.92f + lastRawFrameMs * 0.08f;
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 30.0f);

        const auto& previousStreamingStats = voxelWorld->GetStreamingStats();
        const bool streamingStillFilling =
            previousStreamingStats.expectedVisibleChunks == 0 ||
            previousStreamingStats.cachedReadChunks < previousStreamingStats.expectedVisibleChunks ||
            previousStreamingStats.cachedWriteChunks < previousStreamingStats.expectedVisibleChunks;

        if (smoothedFrameMs > 19.0f) {
            currentCopyBudget = 12;
        } else if (smoothedFrameMs > 18.0f) {
            currentCopyBudget = 16;
        } else if (smoothedFrameMs > 17.0f) {
            currentCopyBudget = 24;
        } else if (streamingStillFilling) {
            currentCopyBudget = 48;
        } else {
            currentCopyBudget = 40;
        }
        voxelWorld->SetMaxChunkCopiesPerFrame(currentCopyBudget);

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
                        // Toggle pause menu instead of quitting
                        pauseMenu.Toggle();
                        // Release/capture mouse based on pause menu state
                        inputManager.SetMouseCaptured(!pauseMenu.IsVisible());
                    }
                    else if (event.key.key == SDLK_TAB) {
                        // Toggle mouse capture
                        inputManager.SetMouseCaptured(!inputManager.IsMouseCaptured());
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
            paused = !paused;
            spdlog::info("Simulation {}", paused ? "paused" : "resumed");
        }
        if (inputManager.IsActionPressed(Input::KeyAction::MaterialNext)) {
            brushController.NextMaterial();
            spdlog::info("Material: {}", brushController.GetMaterial());
        }
        if (inputManager.IsActionPressed(Input::KeyAction::MaterialPrev)) {
            brushController.PrevMaterial();
            spdlog::info("Material: {}", brushController.GetMaterial());
        }
        if (inputManager.IsActionPressed(Input::KeyAction::BrushIncrease)) {
            brushController.IncreaseRadius();
            spdlog::info("Brush radius: {:.1f}", brushController.GetRadius());
        }
        if (inputManager.IsActionPressed(Input::KeyAction::BrushDecrease)) {
            brushController.DecreaseRadius();
            spdlog::info("Brush radius: {:.1f}", brushController.GetRadius());
        }
        if (inputManager.IsActionPressed(Input::KeyAction::TogglePerspective)) {
            thirdPersonMode = !thirdPersonMode;
            spdlog::info("Perspective: {}", thirdPersonMode ? "third-person" : "first-person");
        }

        const bool jumpPressed = inputManager.IsActionPressed(Input::KeyAction::CameraUp);
        const bool flightTogglePressed = inputManager.IsActionDoubleClicked(Input::KeyAction::CameraUp);

        // Mouse look - update camera rotation
        glm::vec2 mouseDelta = inputManager.GetMouseDelta();
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
        if (inputManager.IsActionDown(Input::KeyAction::CameraForward)) {
            moveDirection += horizontalForward;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraBackward)) {
            moveDirection -= horizontalForward;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraLeft)) {
            moveDirection -= horizontalRight;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraRight)) {
            moveDirection += horizontalRight;
        }
        if (glm::length(moveDirection) > 0.001f) {
            cameraPos += glm::normalize(moveDirection) * moveSpeed;
        }

        if (flightMode && !enableBoundaryTest) {
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

        // Apply gravity to vertical velocity (only when not in flight mode AND terrain is ready)
        // During startup, terrain might not be generated yet - disable gravity until
        // ground detection works to prevent falling through the world
        if (!flightMode && terrainReady) {
            cameraVelocityY += gravity * dt;
        }

        // Apply vertical velocity to camera position (only if terrain ready or flying)
        if (terrainReady || flightMode) {
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
            inputManager.IsMouseButtonDown(Input::MouseButton::Left),
            inputManager.IsMouseButtonDown(Input::MouseButton::Right),
            inputManager.GetScrollDelta(),
            nullptr,  // No CPU voxel data (GPU raycasting now!)
            0
        );

        // Get current frame context
        uint32_t frameIndex = window->GetCurrentBackBufferIndex();
        FrameContext& ctx = frameContexts[frameIndex];

        // Wait for this frame's previous work to complete
        commandQueue->WaitForFenceValue(ctx.fenceValue);
        voxelWorld->RetireBrushEditFeedback(commandQueue->GetLastCompletedFenceValue());
        if (voxelWorld->RetireGroundRaycastReadback(frameIndex) && groundQueryMetadata[frameIndex].valid) {
            completedGroundQueryRegionOriginWorld = groundQueryMetadata[frameIndex].regionOriginWorld;
            completedGroundQueryFeetWorld = groundQueryMetadata[frameIndex].feetWorld;
            hasCompletedGroundQuery = true;
        }
        if (voxelWorld->RetireBrushRaycastReadback(frameIndex) && brushQueryMetadata[frameIndex].valid) {
            completedBrushQueryRegionOriginWorld = brushQueryMetadata[frameIndex].regionOriginWorld;
            completedBrushQueryOriginWorld = brushQueryMetadata[frameIndex].originWorld;
            hasCompletedBrushQuery = true;
        }

        // Reset command allocator and command list
        ctx.commandAllocator->Reset();
        commandList->Reset(ctx.commandAllocator.Get(), nullptr);

        // ===== UPDATE VOXEL DATA SOURCE =====
        // In static layout mode, copy a fixed 2x2 patch of pre-generated chunks
        // into the 256x128x256 buffer each frame. This bypasses streaming so we
        // can validate copy/origin and rendering in isolation. Otherwise, use the
        // normal infinite chunk streaming path.
        if (useStaticChunkLayout) {
            voxelWorld->CopyStatic2x2Chunks(commandQueue->GetCommandQueue());
        } else if (voxelWorld->IsUsingInfiniteChunks()) {
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
                spdlog::info("RECENTER_INVARIANT oldOrigin=({:.0f},{:.0f},{:.0f}) newOrigin=({:.0f},{:.0f},{:.0f}) delta=({:.0f},{:.0f},{:.0f}) cameraBefore=({:.2f},{:.2f},{:.2f}) cameraAfter=({:.2f},{:.2f},{:.2f}) cameraLocalBefore=({:.2f},{:.2f},{:.2f}) cameraLocalAfter=({:.2f},{:.2f},{:.2f}) changed={}",
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

            // DEBUG: Track how many chunks have been copied into each buffer so far.
            // This should climb toward ~32 (4x2x4) and then stabilize when standing still.
            if (frameCount % 60 == 0) {
                int readIdx = voxelWorld->GetReadBufferIndex();
                size_t copiedRead  = voxelWorld->GetCopiedChunkCount(readIdx);
                size_t copiedWrite = voxelWorld->GetCopiedChunkCount(1 - readIdx);
                spdlog::debug("Copied chunks: READ={} WRITE={} (readIdx={})",
                    copiedRead, copiedWrite, readIdx);
            }
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
        } else {
            regionOriginWorld = voxelWorld->GetRegionOriginWorld();
            cameraPosLocal = cameraPos - regionOriginWorld;
        }

        nextGroundQueryRegionOriginWorld = regionOriginWorld;
        nextGroundQueryFeetWorld = cameraPos - glm::vec3(0, playerHeight, 0);
        nextBrushQueryRegionOriginWorld = regionOriginWorld;
        brushRayOriginWorld = cameraPos;
        glm::vec3 brushRayOriginLocal = cameraPosLocal;
        nextBrushQueryOriginWorld = brushRayOriginWorld;

        // === GPU GROUND DETECTION RAYCAST (for player collision) ===
        // Cast a ray straight down from player FEET position to find ground
        // Camera is at eye level, so subtract playerHeight to get feet position
        glm::vec3 playerFeetLocal = cameraPosLocal - glm::vec3(0, playerHeight, 0);
        glm::vec3 downDir = glm::vec3(0, -1, 0);
        physicsDispatcher->DispatchGroundRaycast(commandList.Get(), *voxelWorld, playerFeetLocal, downDir);

        // === GPU BRUSH RAYCASTING ===
        // Target from the camera/crosshair. Traversal-friendly placement should
        // be handled as brush policy, not by moving the raw ray origin into the
        // collision body; doing that made empty-air strokes resolve underfoot.
        physicsDispatcher->DispatchBrushRaycast(commandList.Get(), *voxelWorld, brushRayOriginLocal, rayDir);

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

        glm::vec3 brushHitWorld(0.0f);
        glm::ivec3 brushHitNormal(0);
        bool brushHitNormalValid = false;
        bool brushHitValid = false;
        if (hasCompletedBrushQuery && gpuRaycastResult.hasValidPosition) {
            brushHitNormal = DecodePackedNormal(gpuRaycastResult.normalPacked, brushHitNormalValid);
            brushHitWorld = glm::vec3(
                gpuRaycastResult.posX,
                gpuRaycastResult.posY,
                gpuRaycastResult.posZ
            ) + completedBrushQueryRegionOriginWorld;

            float hitDistance = glm::length(brushHitWorld - completedBrushQueryOriginWorld);
            glm::vec3 brushHitCurrentLocal = brushHitWorld - regionOriginWorld;
            glm::vec3 currentHitDelta = brushHitWorld - cameraPos;
            float currentRayDistance = glm::dot(currentHitDelta, rayDir);
            glm::vec3 currentRayClosest = cameraPos + rayDir * currentRayDistance;
            float currentRayLateralError = glm::length(brushHitWorld - currentRayClosest);
            float currentRayTolerance = std::max(brushController.GetRadius() * 1.25f, currentRayDistance * 0.08f + 2.0f);
            const float minBrushHitDistance = 1.0f;
            brushHitValid =
                hitDistance > minBrushHitDistance &&
                hitDistance < 768.0f &&
                currentRayDistance > minBrushHitDistance &&
                currentRayLateralError <= currentRayTolerance &&
                brushHitCurrentLocal.x >= 0.0f && brushHitCurrentLocal.x < voxelWorld->GetGridSizeX() &&
                brushHitCurrentLocal.y >= 0.0f && brushHitCurrentLocal.y < voxelWorld->GetGridSizeY() &&
                brushHitCurrentLocal.z >= 0.0f && brushHitCurrentLocal.z < voxelWorld->GetGridSizeZ();
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

        // === COLLISION DETECTION ===
        if (!flightMode) {
            // Normal mode - ground collision and gravity
            // Ground raycast hit detection
            if (hasCompletedGroundQuery && groundRaycastResult.hasValidPosition) {
                bool groundNormalValid = false;
                const glm::ivec3 groundNormal = DecodePackedNormal(groundRaycastResult.normalPacked, groundNormalValid);
                glm::vec3 groundHitWorld = glm::vec3(
                    groundRaycastResult.posX,
                    groundRaycastResult.posY,
                    groundRaycastResult.posZ
                ) + completedGroundQueryRegionOriginWorld;

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

        cameraPosLocal = useStaticChunkLayout ? cameraPos : cameraPos - regionOriginWorld;

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

        glm::vec3 brushPlacementWorld = brushHitWorld;
        bool brushPlacementPreviewValid = false;
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
        const bool streamingReadyForPhysics =
            disablePhysics ||
            !voxelWorld->IsUsingInfiniteChunks() ||
            (prePhysicsStats.expectedVisibleChunks > 0 &&
             prePhysicsStats.cachedReadChunks >= prePhysicsStats.expectedVisibleChunks &&
             prePhysicsStats.cachedWriteChunks >= prePhysicsStats.expectedVisibleChunks &&
             prePhysicsStats.queuedChunks == 0);
        const uint32_t physicsInterval = (!streamingReadyForPhysics || smoothedFrameMs > 17.2f) ? 4u : 1u;
        const uint32_t physicsScanBudgetChunks = smoothedFrameMs > 17.2f ? 256u : 512u;
        const bool physicsDueThisFrame = (frameCount % physicsInterval) == 0;
        if (!paused && !disablePhysics && infinitePhysicsAllowed && streamingReadyForPhysics && physicsDueThisFrame) {
            // Scan chunks to determine which are active
            physicsDispatcher->DispatchChunkScan(
                commandList.Get(),
                *voxelWorld,
                *chunkManager,
                static_cast<uint32_t>(frameCount),
                cameraPosLocal,
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
        } else if (!paused && !disablePhysics) {
            physicsSkippedForBudget = true;
            physicsBudgetSkipCount++;
        }

        // Apply brush painting AFTER physics (so brush changes aren't overwritten)
        // Use GPU raycast position, or fallback to fixed distance in empty air
        if (brushController.IsPainting() || brushController.IsErasing()) {
            glm::vec3 brushPos;
            const bool buildStroke = brushController.IsPainting() && !brushController.IsErasing();
            if (!buildStroke) {
                buildStrokeState.active = false;
                buildStrokeState.rayDistance = 64.0f;
            }

            if (brushHitValid) {
                // Use GPU raycast hit position (on solid voxel face).
                // Convert the previous-frame world hit into the current render buffer.
                brushPos = brushHitWorld - regionOriginWorld;
                if (buildStroke) {
                    buildStrokeState.active = true;
                    buildStrokeState.rayDistance = std::clamp(glm::length(brushHitWorld - cameraPos), 4.0f, 768.0f);
                }
                static int logCounter = 0;
                if (logCounter++ % 60 == 0) {  // Log once per second
                    spdlog::info("Painting at raycast pos: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            } else {
                // Fallback: keep build strokes at their current working
                // distance so turning left/right follows the line of sight
                // instead of teleporting the brush back in front of the player.
                const float fallbackDistance = buildStroke
                    ? (buildStrokeState.active ? buildStrokeState.rayDistance : 64.0f)
                    : 12.0f;
                if (buildStroke) {
                    buildStrokeState.active = true;
                    buildStrokeState.rayDistance = fallbackDistance;
                }
                brushPos = cameraPosLocal + rayDir * fallbackDistance;

                // Clamp to grid bounds
                brushPos = glm::clamp(brushPos,
                    glm::vec3(0.5f),
                    glm::vec3(voxelWorld->GetGridSizeX() - 0.5f,
                             voxelWorld->GetGridSizeY() - 0.5f,
                             voxelWorld->GetGridSizeZ() - 0.5f));

                static int logCounter = 0;
                if (logCounter++ % 60 == 0) {  // Log once per second
                    spdlog::info("Painting in air at: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            }

            bool closeRampAdjusted = false;
            brushPos = ApplyCloseTraversalBrushFallback(
                brushPos,
                cameraPosLocal,
                cameraForward,
                playerHeight,
                playerRadius,
                brushController.GetRadius(),
                buildStroke,
                &closeRampAdjusted);

            brushPos = glm::clamp(brushPos,
                glm::vec3(0.5f),
                glm::vec3(voxelWorld->GetGridSizeX() - 0.5f,
                         voxelWorld->GetGridSizeY() - 0.5f,
                         voxelWorld->GetGridSizeZ() - 0.5f));

            brushPlacementWorld = brushPos + regionOriginWorld;
            brushPlacementPreviewValid = true;
            brushPlacementCloseRamp = closeRampAdjusted;
            if (closeRampAdjusted) {
                static int rampLogCounter = 0;
                if (rampLogCounter++ % 60 == 0) {
                    spdlog::info("Close traversal brush ramp: local=({:.1f},{:.1f},{:.1f}) world=({:.1f},{:.1f},{:.1f})",
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
            brushConstants.hitNormalX = brushHitNormalValid ? brushHitNormal.x : 0;
            brushConstants.hitNormalY = brushHitNormalValid ? brushHitNormal.y : 0;
            brushConstants.hitNormalZ = brushHitNormalValid ? brushHitNormal.z : 0;
            brushConstants.hasHitNormal = brushHitNormalValid ? 1u : 0u;

            physicsDispatcher->DispatchBrush(commandList.Get(), *voxelWorld, brushConstants);
        } else {
            buildStrokeState.active = false;
            buildStrokeState.rayDistance = 64.0f;
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

        // Build brush preview params from GPU raycast result (NEW!)
        Graphics::Renderer::BrushPreview brushPreview = {};
        if (brushPlacementPreviewValid || brushHitValid) {
            const glm::vec3 previewWorld = brushPlacementPreviewValid ? brushPlacementWorld : brushHitWorld;
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
        glm::vec3 regionOrigin = voxelWorld->GetRegionOriginWorld();
        if (enableDiagnostics && frameCount % 60 == 0) {
            spdlog::info(
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
        renderer->RenderVoxels(
            commandList.Get(),
            voxelWorld->GetReadBufferSRV(),
            voxelWorld->GetPaletteSRV(),
            voxelWorld->GetGridSizeX(),
            voxelWorld->GetGridSizeY(),
            voxelWorld->GetGridSizeZ(),
            cameraParams,
            regionOrigin.x,
            regionOrigin.y,
            regionOrigin.z,
            &brushPreview,
            &characterPreview,
            &sparseFarField
        );

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
        const glm::vec3 playerFeetRenderLocal = playerFeetWorld - regionOrigin;
        const glm::vec3 characterPreviewWorld(
            characterPreview.feetX,
            characterPreview.feetY,
            characterPreview.feetZ);
        const float terrainHeightAtPlayer =
            (hasCompletedGroundQuery && groundRaycastResult.hasValidPosition)
                ? (groundRaycastResult.posY + completedGroundQueryRegionOriginWorld.y)
                : -9999.0f;

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGuiWindowFlags metricsFlags =
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("VENPOD Metrics", nullptr, metricsFlags)) {
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
            ImGui::Text("Far SVO %s | pages %u | nodes %u | page %.0f | coverage %.0f",
                sparseFarField.enabled ? "on" : "off",
                sparseFarField.pageCount,
                sparseFarField.nodeCount,
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
            ImGui::Text("Chunk copies %u / budget %u",
                streamingStats.chunksCopiedLastFrame,
                streamingStats.copyBudget);
            ImGui::Text("Missing gen %u load %u checked %u",
                streamingStats.chunksNotGeneratedLastFrame,
                streamingStats.chunksNotLoadedLastFrame,
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
            ImGui::Text("GPU edit feedback queued %u pending %u applied %u drop %u overflow %u",
                streamingStats.gpuBrushFeedbackQueued,
                streamingStats.gpuBrushFeedbackPending,
                streamingStats.gpuBrushEventsAppliedLastFrame,
                streamingStats.gpuBrushEventsDroppedLastFrame,
                streamingStats.gpuBrushEventsOverflowLastFrame);
            ImGui::Text("Physics %s interval %u dispatches %llu skips %llu",
                physicsRanThisFrame ? "ran" : (physicsSkippedForBudget ? "budget skip" : "idle"),
                physicsInterval,
                static_cast<unsigned long long>(physicsDispatchCount),
                static_cast<unsigned long long>(physicsBudgetSkipCount));
            const auto& physicsStats = physicsDispatcher->GetStats();
            ImGui::Text("Physics scan %u/%u chunks skip %u region %ux%ux%u @ %d %d %d",
                physicsStats.scannedChunksLastFrame,
                physicsStats.scanBudgetChunks,
                physicsStats.skippedScanChunksLastFrame,
                physicsStats.dispatchX,
                physicsStats.dispatchY,
                physicsStats.dispatchZ,
                physicsStats.offsetX,
                physicsStats.offsetY,
                physicsStats.offsetZ);
            ImGui::Text("Copy-fence swap skips %u", streamingStats.swapSkippedForCopyFence);
            ImGui::Text("Raymarch budget dense 2500 voxels / 2048 steps");
        }
        ImGui::End();

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

        // Render ImGui draw data to command list
        imguiBackend.Render(commandList.Get());

        // Queue tiny GPU->CPU raycast copies into a per-frame readback slot.
        // The slot is only mapped after this same frame index's fence completes,
        // so brush targeting/collision never consumes an in-flight GPU write.
        voxelWorld->QueueBrushRaycastReadback(commandList.Get(), frameIndex);
        voxelWorld->QueueGroundRaycastReadback(commandList.Get(), frameIndex);
        brushQueryMetadata[frameIndex] = BrushQueryMetadata{
            true,
            nextBrushQueryRegionOriginWorld,
            nextBrushQueryOriginWorld
        };
        groundQueryMetadata[frameIndex] = GroundQueryMetadata{
            true,
            nextGroundQueryRegionOriginWorld,
            nextGroundQueryFeetWorld
        };

        // End frame - transitions back buffer to present state
        renderer->EndFrame(commandList.Get(), frameIndex);

        // Close and execute command list
        commandList->Close();
        commandQueue->ExecuteCommandList(commandList.Get());

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

        // Present
        window->Present();

        // Signal fence for this frame
        ctx.fenceValue = commandQueue->Signal();
        voxelWorld->NotifyBrushEditFeedbackFence(ctx.fenceValue);

        // End input frame
        inputManager.EndFrame();

        frameCount++;

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

    renderer->Shutdown();
    window->Shutdown();
    commandQueue->Shutdown();
    device->Shutdown();

    spdlog::info("VENPOD shut down cleanly. Total frames: {}", frameCount);
    return 0;
}
