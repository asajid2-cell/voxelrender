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
#include "Input/InputManager.h"
#include "Input/BrushController.h"
#include <spdlog/spdlog.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <memory>
#include <filesystem>
#include <cmath>

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

    spdlog::set_level(spdlog::level::info);
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
    rendererConfig.cbvSrvUavDescriptorCount = 4096;
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
    voxelConfig.gridSizeX = 256;  // Larger world for more playable area
    voxelConfig.gridSizeY = 128;  // Keep height smaller for faster sim
    voxelConfig.gridSizeZ = 256;

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

    // Initialize voxels with test pattern
    physicsDispatcher->DispatchInitialize(initCommandList.Get(), *voxelWorld, 12345);

    // CRITICAL: Swap buffers so the initialized data becomes the "read" buffer
    // DispatchInitialize writes to the WRITE buffer, so we need to swap
    // to make that data available as the READ buffer for rendering
    voxelWorld->SwapBuffers();

    initCommandList->Close();
    commandQueue->ExecuteCommandList(initCommandList.Get());
    commandQueue->Flush();  // Wait for initialization to complete

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

    spdlog::info("Initialization complete. Entering main loop...");
    spdlog::info("Controls: WASD=Move, Mouse=Look, Space/Shift=Up/Down, Tab=Toggle Mouse, LMB=Paint, RMB=Erase, Q/E=Material, P=Pause");

    // Camera setup with pitch/yaw for mouse look
    const float fov = 60.0f * 3.14159f / 180.0f;
    const float aspectRatio = static_cast<float>(windowConfig.width) / static_cast<float>(windowConfig.height);
    const float cameraSpeed = 50.0f;  // Units per second
    const float mouseSensitivity = 0.001f;  // Radians per pixel (halved for better control)

    // Initial camera position
    float gridSizeXInit = static_cast<float>(voxelWorld->GetGridSizeX());
    float gridSizeYInit = static_cast<float>(voxelWorld->GetGridSizeY());
    float gridSizeZInit = static_cast<float>(voxelWorld->GetGridSizeZ());
    glm::vec3 cameraPos = glm::vec3(gridSizeXInit * 1.2f, gridSizeYInit * 0.7f, gridSizeZInit * 1.2f);

    // Camera rotation (pitch and yaw)
    float cameraPitch = -0.3f;  // Look down slightly
    float cameraYaw = -2.356f;  // Look toward center (approximately -3*PI/4)

    // Main loop
    bool running = true;
    bool paused = false;
    uint64_t frameCount = 0;
    bool mouseInitialized = false;  // Track if mouse capture has been enabled

    while (running) {
        // Process SDL events FIRST to update mouse/keyboard state
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Pass event to input manager
            inputManager.ProcessEvent(event);

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        running = false;
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

        // Camera movement with WASD + Space/Shift
        float dt = 1.0f / 60.0f;  // Approximate delta time
        float moveSpeed = cameraSpeed * dt;
        if (inputManager.IsActionDown(Input::KeyAction::CameraForward)) {
            cameraPos += cameraForward * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraBackward)) {
            cameraPos -= cameraForward * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraLeft)) {
            cameraPos -= cameraRight * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraRight)) {
            cameraPos += cameraRight * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraUp)) {
            cameraPos += glm::vec3(0, 1, 0) * moveSpeed;
        }
        if (inputManager.IsActionDown(Input::KeyAction::CameraDown)) {
            cameraPos -= glm::vec3(0, 1, 0) * moveSpeed;
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

        // === GPU BRUSH RAYCASTING (NEW - 2,000,000x FASTER!) ===
        // Dispatch single-thread GPU compute to find brush position
        // This replaces the 32MB CPU readback with 16 bytes!
        physicsDispatcher->DispatchBrushRaycast(commandList.Get(), *voxelWorld, cameraPos, rayDir);

        // Begin frame - transitions back buffer, sets render target, viewport, etc.
        renderer->BeginFrame(commandList.Get(), frameIndex);

        // Debug logging removed to reduce spam

        // Get GPU raycast result (16 bytes from previous frame)
        auto gpuRaycastResult = voxelWorld->GetBrushRaycastResult();

        // Apply brush painting FIRST (so chunk scanner can detect new voxels)
        // Use GPU raycast position, or fallback to fixed distance in empty air
        if (brushController.IsPainting() || brushController.IsErasing()) {
            glm::vec3 brushPos;

            if (gpuRaycastResult.hasValidPosition) {
                // Use GPU raycast hit position (on solid voxel face)
                brushPos = glm::vec3(gpuRaycastResult.posX, gpuRaycastResult.posY, gpuRaycastResult.posZ);
                static int logCounter = 0;
                if (logCounter++ % 60 == 0) {  // Log once per second
                    spdlog::info("Painting at raycast pos: ({:.1f}, {:.1f}, {:.1f}), material={}",
                        brushPos.x, brushPos.y, brushPos.z, brushController.GetMaterial());
                }
            } else {
                // Fallback: place at fixed distance in empty air (10 voxels ahead)
                brushPos = cameraPos + rayDir * 10.0f;

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
            brushConstants.mode = static_cast<uint32_t>(brushController.GetMode());
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

        // Build camera params for shader
        Graphics::Renderer::CameraParams cameraParams;
        cameraParams.posX = cameraPos.x;
        cameraParams.posY = cameraPos.y;
        cameraParams.posZ = cameraPos.z;
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
        renderer->RenderVoxels(
            commandList.Get(),
            voxelWorld->GetReadBufferSRV(),
            voxelWorld->GetPaletteSRV(),
            voxelWorld->GetGridSizeX(),
            voxelWorld->GetGridSizeY(),
            voxelWorld->GetGridSizeZ(),
            cameraParams,
            &brushPreview  // GPU result handles validity internally
        );

        // Render crosshair at screen center
        renderer->RenderCrosshair(commandList.Get());

        // Request tiny 16-byte GPU->CPU readback for next frame's brush preview
        // 2,000,000x smaller than old 32MB readback!
        voxelWorld->RequestBrushRaycastReadback(commandList.Get());

        // End frame - transitions back buffer to present state
        renderer->EndFrame(commandList.Get(), frameIndex);

        // Close and execute command list
        commandList->Close();
        commandQueue->ExecuteCommandList(commandList.Get());

        // Present
        window->Present();

        // Signal fence for this frame
        ctx.fenceValue = commandQueue->Signal();

        // End input frame
        inputManager.EndFrame();

        frameCount++;

        // Log FPS every 100 frames
        if (frameCount % 100 == 0) {
            spdlog::debug("Frame {}", frameCount);
        }
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
