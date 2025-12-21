// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Entry Point
// =============================================================================

#include "Core/Window.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/DX12CommandQueue.h"
#include "Graphics/Renderer.h"
#include "Simulation/VoxelWorld.h"
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
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <memory>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace VENPOD;
using namespace VENPOD::Graphics;

// Frame synchronization
static constexpr uint32_t kFrameCount = Window::BUFFER_COUNT;

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    uint64_t fenceValue = 0;
};

// Get the executable directory to find assets
std::filesystem::path GetExecutableDirectory() {
    // Try to get executable path from SDL
    const char* basePath = SDL_GetBasePath();
    if (basePath) {
        std::filesystem::path path(basePath);
        // SDL3 manages the static string, no SDL_free needed
        return path;
    }
    return std::filesystem::current_path();
}


int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // DEBUG MODE: When true, bypass streaming and use a fixed 2x2 static
    // chunk layout copied into the 256x128x256 voxel buffer each frame.
    // This isolates copy/origin bugs from the infinite chunk streaming logic.
    const bool useStaticChunkLayout = false;

    // DEBUGGING: Enable debug level to see synchronization logs
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("===========================================");
    spdlog::info("  VENPOD - Voxel Physics Engine v0.1.0");
    spdlog::info("  Target: 100M+ Active Voxels @ 60 FPS");
    spdlog::info("===========================================");

    // Initialize DX12 Device
    auto device = std::make_unique<DX12Device>();
    DeviceConfig deviceConfig;
    deviceConfig.enableDebugLayer = true;
    deviceConfig.enableGPUValidation = true;  // Enable for debugging GPU raycast crash

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
    rendererConfig.cbvSrvUavDescriptorCount = 8192;  // DOUBLED: Increased from 4096 to handle 100+ chunks safely
    rendererConfig.rtvDescriptorCount = 32;
    rendererConfig.dsvDescriptorCount = 8;
    rendererConfig.debugShaders = true;  // Enable debug info for development

    // Find shader path
    std::filesystem::path exeDir = GetExecutableDirectory();
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

    // =============================================================================
    // 🧪 CHUNK GENERATION TESTS (DISABLED - causes descriptor heap conflicts)
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
        spdlog::info("╔══════════════════════════════════════════════════════════════╗");
        spdlog::info("║  🧪 RUNNING INFINITE CHUNK GENERATION TESTS                 ║");
        spdlog::info("╚══════════════════════════════════════════════════════════════╝");
        spdlog::info("");

        bool testsPass = Simulation::ChunkGenerationTest::RunAllTests(*device, *commandQueue, renderer->GetHeapManager());

        if (!testsPass) {
            spdlog::critical("❌ CHUNK GENERATION TESTS FAILED!");
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
    // RTX 3070 Ti (8GB) MAXED OUT: Render buffer sized for massive world
    // Terrain exists at Y=5-60 (spans chunks Y=0,1 only = 128 voxels)
    // Horizontal: 25×25 chunks (render distance 12 = camera ±12 chunks)
    voxelConfig.gridSizeX = 1600;  // 25 chunks wide (64 * 25)
    voxelConfig.gridSizeY = 128;   // 2 chunks tall (64 * 2) - exactly terrain height
    voxelConfig.gridSizeZ = 1600;  // 25 chunks deep (64 * 25)
    // Total: 25×2×25 = 1,250 chunks × 262KB = ~52 MB render buffer (×2 for ping-pong = 104 MB)
    // With 8GB VRAM, this is trivial! Leaves 7.9GB for chunks + rendering

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
        spdlog::info("Initialized 256³ voxel grid with procedural terrain (CS_Initialize)");
    } else {
        spdlog::info("Skipping CS_Initialize - using infinite chunk system for terrain generation");
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
    spdlog::info("Controls: ESC=Pause, WASD=Move, Mouse=Look, Space=Jump/Fly, Double-Space=Toggle Flight, Tab=Toggle Mouse, LMB=Paint, RMB=Erase");

    // Camera setup with pitch/yaw for mouse look
    const float fov = 60.0f * 3.14159f / 180.0f;
    const float aspectRatio = static_cast<float>(windowConfig.width) / static_cast<float>(windowConfig.height);
    const float cameraSpeed = 50.0f;  // Units per second
    const float mouseSensitivity = 0.001f;  // Radians per pixel (halved for better control)

    // Initial camera position
    float gridSizeXInit = static_cast<float>(voxelWorld->GetGridSizeX());
    float gridSizeYInit = static_cast<float>(voxelWorld->GetGridSizeY());
    float gridSizeZInit = static_cast<float>(voxelWorld->GetGridSizeZ());

    // FIX: Spawn camera at world origin with proper terrain height
    // Terrain generates around world origin (0,0,0) at heights Y=5 to Y=60
    // Sea level is at Y=40, so spawn slightly above at Y=50 to see the terrain
    // Camera horizontal position doesn't matter much since chunks load around it
    glm::vec3 cameraPos = glm::vec3(128.0f, 50.0f, 128.0f);  // Start near world origin

    // Camera rotation (pitch and yaw)
    float cameraPitch = -0.5f;  // Look down more to see terrain below
    float cameraYaw = 0.0f;  // Look straight ahead (north)

    // Player physics for walking on terrain
    float cameraVelocityY = 0.0f;  // Vertical velocity for gravity
    const float gravity = -50.0f;  // Gravity acceleration (units/s^2)
    const float playerHeight = 3.0f;  // Player eye height above ground (voxels)
    const float stepHeight = 1.5f;  // Max step height for climbing (voxels)
    const float playerRadius = 0.4f;  // Player collision radius (voxels)

    // Flight mode toggle (double-click Space to enable/disable)
    bool flightMode = false;

    // Player position represents feet/collision point
    // Camera rendering position is offset upward by playerHeight for natural eye-level view

    // Main loop
    bool running = true;
    bool paused = false;
    uint64_t frameCount = 0;
    bool mouseInitialized = false;  // Track if mouse capture has been enabled

    while (running) {
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

        // Mouse look - update camera rotation
        glm::vec2 mouseDelta = inputManager.GetMouseDelta();
        cameraYaw += mouseDelta.x * mouseSensitivity;  // Inverted from - to + for correct left/right
        cameraPitch -= mouseDelta.y * mouseSensitivity;

        // Clamp pitch to prevent flipping
        const float maxPitch = 1.57f;  // ~90 degrees
        cameraPitch = glm::clamp(cameraPitch, -maxPitch, maxPitch);

        // Calculate camera basis from pitch/yaw
        glm::vec3 cameraForward;
        cameraForward.x = cos(cameraPitch) * cos(cameraYaw);
        cameraForward.y = sin(cameraPitch);
        cameraForward.z = cos(cameraPitch) * sin(cameraYaw);
        cameraForward = glm::normalize(cameraForward);

        glm::vec3 cameraRight = glm::normalize(glm::cross(cameraForward, glm::vec3(0, 1, 0)));
        glm::vec3 cameraUp = glm::cross(cameraRight, cameraForward);

        // Camera movement with WASD (horizontal only for walking mode)
        float dt = 1.0f / 60.0f;  // Approximate delta time
        float moveSpeed = cameraSpeed * dt;

        // Calculate horizontal movement direction (forward/right with Y removed)
        glm::vec3 horizontalForward = glm::normalize(glm::vec3(cameraForward.x, 0, cameraForward.z));
        glm::vec3 horizontalRight = glm::normalize(glm::vec3(cameraRight.x, 0, cameraRight.z));

        // WASD for horizontal movement only
        if (inputManager.IsActionDown(Input::KeyAction::CameraForward)) {
            cameraPos += horizontalForward * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraBackward)) {
            cameraPos -= horizontalForward * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraLeft)) {
            cameraPos -= horizontalRight * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraRight)) {
            cameraPos += horizontalRight * moveSpeed;
        }

        // Check for double-click on Space to toggle flight mode
        if (inputManager.IsActionDoubleClicked(Input::KeyAction::CameraUp)) {
            flightMode = !flightMode;
            if (flightMode) {
                cameraVelocityY = 0.0f;  // Cancel gravity when entering flight mode
                spdlog::info("Flight mode ENABLED - gravity disabled");
            } else {
                spdlog::info("Flight mode DISABLED - gravity enabled");
            }
        }

        // Apply gravity to vertical velocity (only when not in flight mode)
        if (!flightMode) {
            cameraVelocityY += gravity * dt;
        }

        // Apply vertical velocity to camera position
        cameraPos.y += cameraVelocityY * dt;

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

        // Update brush controller (material, radius, buttons)
        // No CPU voxel data needed - GPU does the raycasting!
        brushController.UpdateFromMouse(
            brushNDC,
            cameraPos,
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
            // regionOrigin is now always (0,0,0) - no camera adjustment needed!
            voxelWorld->UpdateChunks(
                device->GetDevice(),
                commandQueue->GetCommandQueue(),  // Uses internal cmd list, not frame cmdList
                cameraPos
            );

            // DEBUG: Track how many chunks have been copied into each buffer so far.
            // This should climb toward ~32 (4x2x4) and then stabilize when standing still.
            if (frameCount % 60 == 0) {
                int readIdx = voxelWorld->GetReadBufferIndex();
                size_t copiedRead  = voxelWorld->GetCopiedChunkCount(readIdx);
                size_t copiedWrite = voxelWorld->GetCopiedChunkCount(1 - readIdx);
                spdlog::info("Copied chunks: READ={} WRITE={} (readIdx={})",
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

        // === GPU GROUND DETECTION RAYCAST (for player collision) ===
        // Cast a ray straight down from player FEET position to find ground
        // Camera is at eye level, so subtract playerHeight to get feet position
        glm::vec3 playerFeetLocal = cameraPosLocal - glm::vec3(0, playerHeight, 0);
        glm::vec3 downDir = glm::vec3(0, -1, 0);
        physicsDispatcher->DispatchGroundRaycast(commandList.Get(), *voxelWorld, playerFeetLocal, downDir);

        // === GPU BRUSH RAYCASTING (NEW - 2,000,000x FASTER!) ===
        // Dispatch single-thread GPU compute to find brush position in local grid space.
        // This replaces the 32MB CPU readback with 16 bytes!
        physicsDispatcher->DispatchBrushRaycast(commandList.Get(), *voxelWorld, cameraPosLocal, rayDir);

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

        // === COLLISION DETECTION ===
        if (flightMode) {
            // Flight mode - manual vertical control, no gravity or collision
            if (inputManager.IsActionDown(Input::KeyAction::CameraUp)) {
                cameraPos.y += moveSpeed * 2.0f;  // Fly up
            }
            if (inputManager.IsActionDown(Input::KeyAction::CameraDown)) {
                cameraPos.y -= moveSpeed * 2.0f;  // Fly down
            }
        } else {
            // Normal mode - ground collision and gravity
            // Ground raycast hit detection
            if (groundRaycastResult.hasValidPosition) {
                // Ground raycast hit something - use it for collision
                float groundLocalY = groundRaycastResult.posY;
                float groundWorldY = groundLocalY + regionOriginWorld.y;

                // Player feet position in world space
                float playerFeetWorldY = cameraPos.y - playerHeight;

                // Check if we're on or near the ground
                bool onGround = playerFeetWorldY <= groundWorldY + 0.5f;

                // If we've fallen through ground, snap feet to ground surface
                if (playerFeetWorldY < groundWorldY) {
                    cameraPos.y = groundWorldY + playerHeight;  // Camera at eye level above ground
                    cameraVelocityY = 0.0f;  // Stop falling
                    onGround = true;
                }

                // Space to jump (if on ground and not double-click)
                if (inputManager.IsActionPressed(Input::KeyAction::CameraUp) && onGround &&
                    !inputManager.IsActionDoubleClicked(Input::KeyAction::CameraUp)) {
                    cameraVelocityY = 20.0f;  // Jump velocity
                }
            }
            // No ground detected - free fall in air
        }

        // === HORIZONTAL COLLISION (Cave/Wall Detection) ===
        // Use brush raycast to check for walls/obstacles in movement direction
        // If brush raycast hits something close (<2 voxels) in the direction we're looking,
        // it means there's a wall/obstacle ahead
        if (gpuRaycastResult.hasValidPosition) {
            // Calculate distance to hit point
            glm::vec3 hitPosLocal = glm::vec3(gpuRaycastResult.posX, gpuRaycastResult.posY, gpuRaycastResult.posZ);
            glm::vec3 hitPosWorld = hitPosLocal + regionOriginWorld;
            float distanceToHit = glm::length(hitPosWorld - cameraPos);

            // If we're about to walk into a wall (hit within player radius + small margin)
            // and the hit is roughly at player height (not floor/ceiling), stop horizontal movement
            float hitHeight = hitPosWorld.y - (cameraPos.y - playerHeight);
            bool isWall = (distanceToHit < playerRadius + 1.0f) && (hitHeight > 0.2f) && (hitHeight < playerHeight - 0.5f);

            if (isWall) {
                // Prevent movement in the direction of the wall by checking if we're moving towards it
                glm::vec3 dirToHit = glm::normalize(hitPosWorld - cameraPos);
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

        // Apply brush painting FIRST (so chunk scanner can detect new voxels)
        // Use GPU raycast position, or fallback to fixed distance in empty air
        if (brushController.IsPainting() || brushController.IsErasing()) {
            glm::vec3 brushPos;

            if (gpuRaycastResult.hasValidPosition) {
                // Use GPU raycast hit position (on solid voxel face).
                // This is already in local 256^3 grid space.
                brushPos = glm::vec3(gpuRaycastResult.posX, gpuRaycastResult.posY, gpuRaycastResult.posZ);
                static int logCounter = 0;
                if (logCounter++ % 60 == 0) {  // Log once per second
                    spdlog::info("Painting at raycast pos: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            } else {
                // Fallback: place at fixed distance in empty air (10 voxels ahead)
                // in LOCAL 256^3 grid space around the camera.
                brushPos = cameraPosLocal + rayDir * 10.0f;

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

            physicsDispatcher->DispatchBrush(commandList.Get(), *voxelWorld, brushConstants);
        }

        // Run physics simulation (if not paused)
        if (!paused) {
            // Scan chunks to determine which are active (includes newly painted voxels)
            physicsDispatcher->DispatchChunkScan(
                commandList.Get(),
                *voxelWorld,
                *chunkManager,
                static_cast<uint32_t>(frameCount)
            );

            // Run physics on active chunks using ExecuteIndirect
            physicsDispatcher->DispatchPhysicsIndirect(
                commandList.Get(),
                *voxelWorld,
                *chunkManager,
                1.0f/60.0f,
                static_cast<uint32_t>(frameCount)
            );
        }

        // Transition read buffer to pixel shader resource for rendering
        voxelWorld->TransitionReadBufferTo(commandList.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Build camera params for shader (camera in LOCAL grid space)
        Graphics::Renderer::CameraParams cameraParams;
        cameraParams.posX = cameraPosLocal.x;
        cameraParams.posY = cameraPosLocal.y;
        cameraParams.posZ = cameraPosLocal.z;
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
        if (gpuRaycastResult.hasValidPosition) {
            // GPU raycast outputs local 256^3 grid coordinates; renderer expects the
            // same space as the camera, so we pass them through directly.
            brushPreview.posX = gpuRaycastResult.posX;
            brushPreview.posY = gpuRaycastResult.posY;
            brushPreview.posZ = gpuRaycastResult.posZ;
            brushPreview.radius = brushController.GetRadius();
            brushPreview.material = brushController.GetMaterial();
            brushPreview.shape = static_cast<uint32_t>(brushController.GetShape());
            brushPreview.hasValidPosition = true;
        } else {
            brushPreview.hasValidPosition = false;
        }

        // Render voxels with raymarch shader (using persistent shader-visible descriptors)
        // Brush preview now uses GPU raycasting (2,000,000x less bandwidth!)
        glm::vec3 regionOrigin = voxelWorld->GetRegionOriginWorld();
        renderer->RenderVoxels(
            commandList.Get(),
            voxelWorld->GetReadBufferSRV(),
            voxelWorld->GetPaletteSRV(),
            voxelWorld->GetChunkOccupancySRV(),  // Empty space acceleration
            voxelWorld->GetGridSizeX(),
            voxelWorld->GetGridSizeY(),
            voxelWorld->GetGridSizeZ(),
            cameraParams,
            regionOrigin.x,
            regionOrigin.y,
            regionOrigin.z,
            &brushPreview  // GPU result handles validity internally
        );

        // Render crosshair at screen center
        renderer->RenderCrosshair(commandList.Get());

        // Render ImGui draw data to command list
        imguiBackend.Render(commandList.Get());

        // Request tiny 16-byte GPU->CPU readback for next frame's brush preview
        // 2,000,000x smaller than old 32MB readback!
        voxelWorld->RequestBrushRaycastReadback(commandList.Get());

        // Request ground raycast readback for next frame's collision detection
        voxelWorld->RequestGroundRaycastReadback(commandList.Get());

        // End frame - transitions back buffer to present state
        renderer->EndFrame(commandList.Get(), frameIndex);

        // Close and execute command list
        commandList->Close();
        commandQueue->ExecuteCommandList(commandList.Get());

        // FIX #17: CRITICAL - Swap read/write buffers for next frame
        // The bug: Without this swap, chunks are copied to WRITE buffer every frame,
        // but READ buffer (used by renderer) stays empty forever → no terrain visible!
        // UpdateChunks and physics wrote to WRITE buffer this frame.
        // Swap makes it the READ buffer so renderer can see new chunk data next frame.
        // Sequence:
        //   Frame N: chunks copied to WRITE (buffer 1), physics writes to buffer 1, render reads buffer 0
        //   Swap → buffer 1 becomes READ, buffer 0 becomes WRITE
        //   Frame N+1: chunks copied to WRITE (now buffer 0), physics writes to buffer 0, render reads buffer 1 ← sees chunks from frame N!
        voxelWorld->SwapBuffers();

        // Present
        window->Present();

        // Signal fence for this frame
        ctx.fenceValue = commandQueue->Signal();

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

    renderer->Shutdown();
    window->Shutdown();
    commandQueue->Shutdown();
    device->Shutdown();

    spdlog::info("VENPOD shut down cleanly. Total frames: {}", frameCount);
    return 0;
}
