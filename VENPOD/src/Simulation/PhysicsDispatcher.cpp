#include "PhysicsDispatcher.h"
#include "TerrainConstants.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Simulation {

Result<void> PhysicsDispatcher::Initialize(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    Graphics::DescriptorHeapManager& heapManager,
    const std::filesystem::path& shaderPath)
{
    if (!device) {
        return Error("Device is null");
    }

    m_device = device;
    m_heapManager = &heapManager;

    // Create initialize pipeline
    auto result = CreateInitializePipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        return Error("Failed to create initialize pipeline: {}", result.error());
    }

    // Create gravity pipeline (for Phase 2B)
    result = CreateGravityPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        // Not fatal - gravity shader may not exist yet
        spdlog::warn("Gravity pipeline not created (shader may not exist yet): {}", result.error());
    }

    // Create brush pipeline (for Phase 3B)
    result = CreateBrushPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        spdlog::warn("Brush pipeline not created: {}", result.error());
    }

    // Create chunk scan pipeline (for Phase 3A)
    result = CreateChunkScanPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        spdlog::warn("Chunk scan pipeline not created: {}", result.error());
    }

    // Create prepare indirect pipeline
    result = CreatePrepareIndirectPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        spdlog::warn("Prepare indirect pipeline not created: {}", result.error());
    }

    // Create chunk-based gravity pipeline
    result = CreateGravityChunkPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        spdlog::warn("Gravity chunk pipeline not created: {}", result.error());
    }

    // Create brush raycast pipeline (NEW - GPU raycasting)
    result = CreateBrushRaycastPipeline(device, shaderCompiler, shaderPath);
    if (!result) {
        spdlog::warn("Brush raycast pipeline not created: {}", result.error());
    }

    // Create command signature for indirect dispatch
    result = CreateCommandSignature(device);
    if (!result) {
        spdlog::warn("Command signature not created: {}", result.error());
    }

    spdlog::info("PhysicsDispatcher initialized");
    return {};
}

void PhysicsDispatcher::Shutdown() {
    m_initializePipeline.Shutdown();
    m_gravityPipeline.Shutdown();
    m_brushPipeline.Shutdown();
    m_chunkScanPipeline.Shutdown();
    m_prepareIndirectPipeline.Shutdown();
    m_gravityChunkPipeline.Shutdown();
    m_brushRaycastPipeline.Shutdown();
    m_commandSignature.Reset();
    m_heapManager = nullptr;
    m_device = nullptr;
}

void PhysicsDispatcher::DispatchInitialize(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    uint32_t seed)
{
    spdlog::info("DispatchInitialize: START");

    if (!cmdList || !m_initializePipeline.IsValid()) {
        spdlog::warn("Cannot dispatch initialize: pipeline or command list invalid");
        return;
    }
    spdlog::info("DispatchInitialize: Pipeline and cmdList valid");

    // Transition write buffer to UAV state
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    spdlog::info("DispatchInitialize: Buffer transitioned to UAV");

    // Set descriptor heaps (required before using shader-visible descriptors)
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    spdlog::info("DispatchInitialize: Descriptor heaps set");

    // Bind pipeline
    m_initializePipeline.Bind(cmdList);
    spdlog::info("DispatchInitialize: Pipeline bound");

    // Set root constants (gridSize and seed)
    struct InitConstants {
        uint32_t gridSizeX;
        uint32_t gridSizeY;
        uint32_t gridSizeZ;
        uint32_t seed;
    } constants;

    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.seed = seed;
    spdlog::info("DispatchInitialize: Constants set: {}x{}x{}, seed={}",
        constants.gridSizeX, constants.gridSizeY, constants.gridSizeZ, constants.seed);

    m_initializePipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    spdlog::info("DispatchInitialize: Root constants pushed");

    // Set UAV for write buffer (use persistent shader-visible descriptor)
    spdlog::info("DispatchInitialize: Getting shader-visible UAV");

    m_initializePipeline.SetRootDescriptorTable(cmdList, 1, world.GetWriteBufferUAV().gpu);
    spdlog::info("DispatchInitialize: Root descriptor table set");

    // Dispatch compute shader
    auto dispatchSize = world.GetDispatchSize(8);
    spdlog::info("DispatchInitialize: About to dispatch: {}x{}x{}",
        dispatchSize.x, dispatchSize.y, dispatchSize.z);

    m_initializePipeline.Dispatch(cmdList, dispatchSize.x, dispatchSize.y, dispatchSize.z);
    spdlog::info("DispatchInitialize: Dispatch complete");

    // UAV barrier to ensure writes complete before next use
    auto& writeBuffer = world.GetWriteBuffer();
    ID3D12Resource* resource = writeBuffer.GetResource();
    if (resource) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = resource;
        cmdList->ResourceBarrier(1, &barrier);
    } else {
        spdlog::warn("DispatchInitialize: Write buffer resource is null!");
    }

    // CRITICAL: Copy WRITE → READ after initialization so rendering sees terrain on Frame 0
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(world.GetReadBuffer().GetResource(), world.GetWriteBuffer().GetResource());
    spdlog::info("DispatchInitialize: Copied WRITE → READ for Frame 0 rendering");

    spdlog::debug("Dispatched voxel initialization: {}x{}x{} thread groups",
        dispatchSize.x, dispatchSize.y, dispatchSize.z);
}

void PhysicsDispatcher::DispatchPhysics(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    float deltaTime,
    uint32_t frameIndex)
{
    if (!cmdList || !m_gravityPipeline.IsValid()) {
        // Gravity pipeline not ready yet
        return;
    }

    // PERFORMANCE FIX: GPU-side synchronization ONLY - no CPU wait needed!
    // D3D12 command queues are FIFO (First-In-First-Out), so when we do:
    //   1. UpdateActiveRegion() → ExecuteCommandLists(chunkCopyList) → Signal(fence)
    //   2. DispatchPhysics() → ExecuteCommandLists(physicsList)
    // The GPU automatically waits for (1) to complete before starting (2).
    // We don't need a CPU spin-wait - that was causing 1-5ms frame stalls!
    // The command queue serialization handles it for us.

    // Transition buffers
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Set descriptor heaps (required before using shader-visible descriptors)
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind gravity pipeline
    m_gravityPipeline.Bind(cmdList);

    // Set constants
    PhysicsConstants constants;
    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.frameIndex = frameIndex;
    constants.deltaTime = deltaTime;
    constants.gravity = m_gravity;
    constants.simulationFlags = m_simulationFlags;
    constants.padding = 0;

    m_gravityPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set SRV for read buffer and UAV for write buffer (use persistent shader-visible descriptors)
    m_gravityPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_gravityPipeline.SetRootDescriptorTable(cmdList, 2, world.GetWriteBufferUAV().gpu);

    // Dispatch
    auto dispatchSize = world.GetDispatchSize(8);
    m_gravityPipeline.Dispatch(cmdList, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    // UAV barrier
    auto& writeBuffer = world.GetWriteBuffer();
    ID3D12Resource* resource = writeBuffer.GetResource();
    if (resource) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = resource;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // NOTE: SwapBuffers is NOT called here anymore!
    // The caller must call SwapBuffers AFTER the command list executes.
    // This is because SwapBuffers is a CPU operation that changes buffer pointers,
    // but the GPU work (recorded in cmdList) hasn't completed yet.
    // If we swap here, subsequent command recording would use wrong buffers.
}

Result<void> PhysicsDispatcher::CreateInitializePipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_Initialize.hlsl";

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile initialize shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Initialize shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_Initialize";

    // Root parameters:
    // 0: Root constants (gridSize, seed)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        4   // 4 uint32s
    });

    // 1: UAV for output buffer
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_initializePipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create initialize pipeline: {}", result.error());
    }

    return {};
}

Result<void> PhysicsDispatcher::CreateGravityPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_Gravity.hlsl";

    if (!std::filesystem::exists(csPath)) {
        return Error("Gravity shader not found: {}", csPath.string());
    }

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile gravity shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Gravity shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_Gravity";

    // Root parameters:
    // 0: Root constants (PhysicsConstants)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        sizeof(PhysicsConstants) / 4
    });

    // 1: SRV for input buffer (read)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register t0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // 2: UAV for output buffer (write)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_gravityPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create gravity pipeline: {}", result.error());
    }

    return {};
}

Result<void> PhysicsDispatcher::CreateBrushPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_Brush.hlsl";

    if (!std::filesystem::exists(csPath)) {
        return Error("Brush shader not found: {}", csPath.string());
    }

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile brush shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Brush shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_Brush";

    // Root parameters:
    // 0: Root constants (BrushConstants - 12 floats/uints)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        12  // 12 uint32s (sizeof(BrushConstants) / 4)
    });

    // 1: UAV for voxel buffer (read-write)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_brushPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create brush pipeline: {}", result.error());
    }

    spdlog::info("Brush pipeline created successfully");
    return {};
}

void PhysicsDispatcher::DispatchBrush(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    const Input::BrushConstants& brushConstants)
{
    if (!cmdList || !m_brushPipeline.IsValid()) {
        spdlog::warn("DispatchBrush: pipeline or cmdList invalid");
        return;
    }

    // Debug logging (can be commented out to reduce spam)
    // spdlog::info("DispatchBrush: pos=({:.1f}, {:.1f}, {:.1f}), radius={:.1f}, material={}",
    //     brushConstants.positionX, brushConstants.positionY, brushConstants.positionZ,
    //     brushConstants.radius, brushConstants.material);

    // PRIORITY 1 FIX: CORRECT PING-PONG ARCHITECTURE
    // Paint to WRITE buffer (Frame N workspace) - everything for Frame N goes there!
    //
    // Correct Timeline:
    // Frame N:
    //   1. UpdateActiveRegion: Copy NEW chunks → WRITE buffer
    //   2. DispatchBrush: Paint voxels → WRITE buffer (adds to copied chunks)
    //   3. DispatchChunkScan: Scan WRITE buffer (sees chunks + painted voxels)
    //   4. DispatchPhysicsIndirect: Read READ (Frame N-1), write WRITE (Frame N)
    //   5. SwapBuffers() - READ ↔ WRITE (WRITE becomes READ for next frame)
    //   6. Render from READ buffer (now has Frame N final state)
    //
    // Key: WRITE = Frame N workspace, READ = Frame N-1 final state (read-only)

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind brush pipeline
    m_brushPipeline.Bind(cmdList);

    // Set constants
    m_brushPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(brushConstants) / 4, &brushConstants);

    auto dispatchSize = world.GetDispatchSize(8);

    // PRIORITY 1 FIX: Paint to WRITE buffer (not READ!)
    // WRITE buffer already has chunks from UpdateActiveRegion, we add painted voxels
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_brushPipeline.SetRootDescriptorTable(cmdList, 1, world.GetWriteBufferUAV().gpu);
    m_brushPipeline.Dispatch(cmdList, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    // UAV barrier on WRITE buffer (ensures brush completes before chunk scan)
    {
        auto& writeBuffer = world.GetWriteBuffer();
        ID3D12Resource* resource = writeBuffer.GetResource();
        if (resource) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = resource;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    // CRITICAL FIX: Invalidate chunk copy cache for the painted region
    // Without this, the chunk won't be re-copied and painted voxels may be lost
    // on subsequent frames when the render buffer gets refreshed.
    //
    // NOTE: For infinite chunks, brushConstants positions are in LOCAL 256^3 grid
    // space, so we must convert them back to world voxel coordinates using the
    // region origin provided by VoxelWorld.
    if (world.IsUsingInfiniteChunks()) {
        glm::vec3 regionOriginWorld = world.GetRegionOriginWorld();
        auto toWorld = [&](float x, float y, float z) -> glm::vec3 {
            return glm::vec3(x, y, z) + regionOriginWorld;
        };

        glm::vec3 paintedWorld = toWorld(
            brushConstants.positionX,
            brushConstants.positionY,
            brushConstants.positionZ
        );

        ChunkCoord paintedChunk = ChunkCoord::FromWorldPosition(
            static_cast<int32_t>(paintedWorld.x),
            static_cast<int32_t>(paintedWorld.y),
            static_cast<int32_t>(paintedWorld.z),
            INFINITE_CHUNK_SIZE
        );
        world.InvalidateCopiedChunk(paintedChunk);

        // If brush is near chunk boundaries, invalidate adjacent chunks too
        // (brush radius can cross chunk boundaries)
        float radius = brushConstants.radius;
        if (radius > 1.0f) {
            // Check +/- X direction
            {
                glm::vec3 worldPosPlusX = toWorld(
                    brushConstants.positionX + radius,
                    brushConstants.positionY,
                    brushConstants.positionZ
                );
                world.InvalidateCopiedChunk(ChunkCoord::FromWorldPosition(
                    static_cast<int32_t>(worldPosPlusX.x),
                    static_cast<int32_t>(worldPosPlusX.y),
                    static_cast<int32_t>(worldPosPlusX.z),
                    INFINITE_CHUNK_SIZE));

                glm::vec3 worldPosMinusX = toWorld(
                    brushConstants.positionX - radius,
                    brushConstants.positionY,
                    brushConstants.positionZ
                );
                world.InvalidateCopiedChunk(ChunkCoord::FromWorldPosition(
                    static_cast<int32_t>(worldPosMinusX.x),
                    static_cast<int32_t>(worldPosMinusX.y),
                    static_cast<int32_t>(worldPosMinusX.z),
                    INFINITE_CHUNK_SIZE));
            }

            // Check +/- Z direction
            {
                glm::vec3 worldPosPlusZ = toWorld(
                    brushConstants.positionX,
                    brushConstants.positionY,
                    brushConstants.positionZ + radius
                );
                world.InvalidateCopiedChunk(ChunkCoord::FromWorldPosition(
                    static_cast<int32_t>(worldPosPlusZ.x),
                    static_cast<int32_t>(worldPosPlusZ.y),
                    static_cast<int32_t>(worldPosPlusZ.z),
                    INFINITE_CHUNK_SIZE));

                glm::vec3 worldPosMinusZ = toWorld(
                    brushConstants.positionX,
                    brushConstants.positionY,
                    brushConstants.positionZ - radius
                );
                world.InvalidateCopiedChunk(ChunkCoord::FromWorldPosition(
                    static_cast<int32_t>(worldPosMinusZ.x),
                    static_cast<int32_t>(worldPosMinusZ.y),
                    static_cast<int32_t>(worldPosMinusZ.z),
                    INFINITE_CHUNK_SIZE));
            }
        }
    }
}

Result<void> PhysicsDispatcher::CreateChunkScanPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_ChunkScanner.hlsl";

    if (!std::filesystem::exists(csPath)) {
        return Error("Chunk scanner shader not found: {}", csPath.string());
    }

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile chunk scanner shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Chunk scanner shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_ChunkScanner";

    // Root parameters:
    // 0: Root constants (ChunkScanConstants - 12 uint32s)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        sizeof(ChunkScanConstants) / 4
    });

    // 1: SRV for voxel grid (read-only)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register t0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // 2: UAV for chunk control buffer
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    // 3: UAV for active chunk list
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,  // register u1
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    // 4: UAV for active chunk count
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        2,  // register u2
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_chunkScanPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create chunk scanner pipeline: {}", result.error());
    }

    spdlog::info("Chunk scanner pipeline created successfully");
    return {};
}

Result<void> PhysicsDispatcher::CreateCommandSignature(ID3D12Device* device)
{
    // Create command signature for indirect dispatch
    D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(IndirectDispatchArgs);  // 3 uint32s
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &argDesc;
    sigDesc.NodeMask = 0;

    HRESULT hr = device->CreateCommandSignature(
        &sigDesc,
        nullptr,  // No root signature needed for basic dispatch
        IID_PPV_ARGS(&m_commandSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create command signature: HRESULT 0x{:08x}", static_cast<uint32_t>(hr));
    }

    spdlog::info("Command signature created for indirect dispatch");
    return {};
}

void PhysicsDispatcher::DispatchChunkScan(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    ChunkManager& chunkManager,
    uint32_t frameIndex)
{
    if (!cmdList || !m_chunkScanPipeline.IsValid()) {
        spdlog::warn("DispatchChunkScan: pipeline or cmdList invalid");
        return;
    }

    // === PERFORMANCE FIX: Scan WRITE buffer directly (no copy needed!) ===
    // DispatchBrush painted to WRITE buffer (line 436), so we scan WRITE to detect new voxels.
    // Previous frame ended with SwapBuffers → old WRITE became READ.
    // Current frame: chunks copied to WRITE, brush paints to WRITE, we scan WRITE.
    // This eliminates the redundant 64 MB WRITE→READ copy!
    //
    // Timeline: UpdateActiveRegion→WRITE, Brush→WRITE, ChunkScan→WRITE,
    //           Physics reads READ writes WRITE, SwapBuffers

    // Reset active chunk count to zero before scanning
    chunkManager.ResetActiveCount(cmdList);

    // Scan WRITE buffer (it has chunks + painted voxels from this frame)
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Transition chunk buffers to UAV for writing
    chunkManager.TransitionBuffersForCompute(cmdList);

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind chunk scan pipeline
    m_chunkScanPipeline.Bind(cmdList);

    // Set constants
    ChunkScanConstants constants;
    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.frameIndex = frameIndex;
    constants.chunkCountX = chunkManager.GetChunkCountX();
    constants.chunkCountY = chunkManager.GetChunkCountY();
    constants.chunkCountZ = chunkManager.GetChunkCountZ();
    constants.chunkSize = CHUNK_SIZE;
    constants.sleepThreshold = m_sleepThreshold;

    // FIX: ALWAYS use ChunkManager's chunk counts (16-voxel physics chunks)
    // ChunkManager is initialized with the render buffer size (1600x128x1600) divided by CHUNK_SIZE (16)
    // This gives us 100x8x100 = 80,000 physics chunks to scan.
    //
    // Previously, infinite chunks mode incorrectly used RENDER_BUFFER_CHUNKS_X/Y/Z (25x2x25)
    // which are 64-voxel STREAMING chunks - completely different coordinate system!
    //
    // The chunk scanner and physics shader MUST use the same coordinate system:
    // - Both operate on the LOCAL render buffer (not world coordinates)
    // - Both use CHUNK_SIZE=16 for physics simulation
    // - activeRegionOffset is NOT needed here since we scan the LOCAL buffer
    uint32_t dispatchX = chunkManager.GetChunkCountX();  // 100 chunks (1600/16)
    uint32_t dispatchY = chunkManager.GetChunkCountY();  // 8 chunks (128/16)
    uint32_t dispatchZ = chunkManager.GetChunkCountZ();  // 100 chunks (1600/16)

    // activeRegionOffset is set to 0 because scanner operates on LOCAL buffer coordinates
    // The world-to-local mapping is handled by UpdateActiveRegion (chunk copying)
    constants.activeRegionOffsetX = 0;
    constants.activeRegionOffsetY = 0;
    constants.activeRegionOffsetZ = 0;

    // Log only once per second to avoid spam
    static int activeRegionLogThrottle = 0;
    if (++activeRegionLogThrottle % 60 == 1) {
        spdlog::debug("DispatchChunkScan: Scanning {}×{}×{} = {} physics chunks (CHUNK_SIZE=16)",
            dispatchX, dispatchY, dispatchZ,
            dispatchX * dispatchY * dispatchZ);
    }

    m_chunkScanPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors - scan WRITE buffer directly (saves 64 MB copy!)
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 1, world.GetWriteBufferSRV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 2, chunkManager.GetChunkControlUAV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 3, chunkManager.GetActiveListUAV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 4, chunkManager.GetActiveCountUAV().gpu);

    // PRIORITY 3: Dispatch optimized 25×2×25 region for infinite chunks, full grid for static
    m_chunkScanPipeline.Dispatch(cmdList, dispatchX, dispatchY, dispatchZ);

    // UAV barrier to ensure writes complete
    D3D12_RESOURCE_BARRIER barriers[3] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = chunkManager.GetChunkControlBuffer().GetResource();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = chunkManager.GetActiveChunkListBuffer().GetResource();
    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = chunkManager.GetActiveChunkCountBuffer().GetResource();
    cmdList->ResourceBarrier(3, barriers);

    // Log only once per second to avoid spam
    static int logThrottle = 0;
    if (++logThrottle % 60 == 1) {
        spdlog::debug("DispatchChunkScan: Dispatched {}×{}×{} chunks (ChunkManager grid: {}×{}×{})",
            dispatchX, dispatchY, dispatchZ,
            chunkManager.GetChunkCountX(), chunkManager.GetChunkCountY(), chunkManager.GetChunkCountZ());
    }
}

Result<void> PhysicsDispatcher::CreatePrepareIndirectPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_PrepareIndirect.hlsl";

    if (!std::filesystem::exists(csPath)) {
        return Error("Prepare indirect shader not found: {}", csPath.string());
    }

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile prepare indirect shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Prepare indirect shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_PrepareIndirect";

    // Root parameters:
    // 0: UAV for active count (u0)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    // 1: UAV for indirect args (u1)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,  // register u1
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_prepareIndirectPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create prepare indirect pipeline: {}", result.error());
    }

    spdlog::info("Prepare indirect pipeline created successfully");
    return {};
}

Result<void> PhysicsDispatcher::CreateGravityChunkPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_GravityChunk.hlsl";

    if (!std::filesystem::exists(csPath)) {
        return Error("Gravity chunk shader not found: {}", csPath.string());
    }

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile gravity chunk shader: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("Gravity chunk shader compilation failed: {}", compiledShader.errors);
    }

    Graphics::ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_GravityChunk";

    // Root parameters:
    // 0: Root constants (PhysicsChunkConstants - 12 uint32s)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        sizeof(PhysicsChunkConstants) / 4
    });

    // 1: SRV for active chunk list (t0)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register t0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // 2: UAV for voxel buffer (u0) - single buffer for in-place updates
    // Shader reads and writes the same buffer (no separate SRV needed)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_gravityChunkPipeline.Initialize(device, desc);
    if (!result) {
        return Error("Failed to create gravity chunk pipeline: {}", result.error());
    }

    spdlog::info("Gravity chunk pipeline created successfully");
    return {};
}

void PhysicsDispatcher::DispatchPhysicsIndirect(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    ChunkManager& chunkManager,
    float deltaTime,
    uint32_t frameIndex)
{
    if (!cmdList || !m_gravityChunkPipeline.IsValid() || !m_prepareIndirectPipeline.IsValid() || !m_commandSignature) {
        // Fall back to regular dispatch if indirect isn't ready
        DispatchPhysics(cmdList, world, deltaTime, frameIndex);
        return;
    }

    // === Step 0: REMOVED REDUNDANT READ→WRITE COPY ===
    // CRITICAL FIX: The previous 64 MB READ→WRITE copy was DESTROYING newly copied chunks!
    //
    // Timeline issue (before fix):
    //   1. UpdateActiveRegion() → copies chunks to WRITE buffer (separate command list)
    //   2. DispatchPhysicsIndirect() → copies READ→WRITE (OVERWRITES the new chunks!)
    //   3. Physics runs on corrupted data → crashes or rendering bugs
    //
    // The correct architecture is:
    //   - UpdateActiveRegion writes NEW chunks → WRITE buffer
    //   - Brush paints → WRITE buffer
    //   - ChunkScan reads WRITE buffer to find active chunks
    //   - Physics reads READ (old frame), writes WRITE (new frame) - preserves chunk data!
    //   - SwapBuffers() makes WRITE become READ for next frame
    //
    // No READ→WRITE copy is needed because:
    //   - For static grid: WRITE already has complete data from previous physics pass
    //   - For infinite chunks: UpdateActiveRegion copies chunks directly to WRITE
    //
    // This fix also saves 64 MB/frame × 60 FPS = 3.84 GB/s bandwidth!

    // === Step 1: Prepare indirect dispatch arguments ===
    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind prepare indirect pipeline
    m_prepareIndirectPipeline.Bind(cmdList);

    // Set descriptors for prepare indirect
    m_prepareIndirectPipeline.SetRootDescriptorTable(cmdList, 0, chunkManager.GetActiveCountUAV().gpu);
    m_prepareIndirectPipeline.SetRootDescriptorTable(cmdList, 1, chunkManager.GetIndirectArgsUAV().gpu);

    // Dispatch single thread to prepare args
    m_prepareIndirectPipeline.Dispatch(cmdList, 1, 1, 1);

    // UAV barrier on indirect args
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = chunkManager.GetIndirectArgsBuffer().GetResource();
    cmdList->ResourceBarrier(1, &barrier);

    // Transition indirect args buffer for indirect dispatch
    chunkManager.TransitionBuffersForIndirect(cmdList);

    // === Step 2: Execute indirect dispatch for chunk-based physics ===
    // CRITICAL FIX: For infinite chunks, read from WRITE buffer (where chunks are)!
    // UpdateActiveRegion copies chunks to WRITE. Physics must read from WRITE to see them.
    // This is an in-place update (read and write same buffer) but works because:
    // - Each voxel is processed by exactly one thread (chunk-based dispatch)
    // - Static voxels just copy themselves in-place (no change)
    // - Moving voxels use atomic-like behavior (only one can claim destination)
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Re-set descriptor heaps (in case they were changed)
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind gravity chunk pipeline
    m_gravityChunkPipeline.Bind(cmdList);

    // Set constants
    PhysicsChunkConstants constants;
    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.frameIndex = frameIndex;
    constants.deltaTime = deltaTime;
    constants.gravity = m_gravity;
    constants.simulationFlags = m_simulationFlags;
    constants.chunkSize = CHUNK_SIZE;
    constants.chunkCountX = chunkManager.GetChunkCountX();
    constants.chunkCountY = chunkManager.GetChunkCountY();
    constants.chunkCountZ = chunkManager.GetChunkCountZ();
    constants.padding = 0;

    m_gravityChunkPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors - single buffer for in-place physics updates
    m_gravityChunkPipeline.SetRootDescriptorTable(cmdList, 1, chunkManager.GetActiveListSRV().gpu);  // Active chunk list SRV (t0)
    m_gravityChunkPipeline.SetRootDescriptorTable(cmdList, 2, world.GetWriteBufferUAV().gpu);        // Voxel buffer UAV (u0) - read AND write

    // Execute indirect dispatch
    cmdList->ExecuteIndirect(
        m_commandSignature.Get(),
        1,  // Max command count
        chunkManager.GetIndirectArgsBuffer().GetResource(),
        0,  // Offset to arguments
        nullptr,  // No count buffer
        0
    );

    // UAV barrier on output
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = world.GetWriteBuffer().GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);

    // REMOVED DUPLICATE: SwapBuffers() is called in main loop after physics!
    // Double-swapping was causing READ and WRITE buffers to point to same buffer,
    // preventing chunk copy logic from working (cache thought all chunks were already copied).
    // world.SwapBuffers();  // <-- REMOVED - main.cpp calls this after physics

    // Log only once per second to avoid spam
    static int physicsLogThrottle = 0;
    if (++physicsLogThrottle % 60 == 1) {
        spdlog::debug("DispatchPhysicsIndirect: Indirect physics dispatch complete (60 FPS)");
    }
}

void PhysicsDispatcher::DispatchBrushRaycast(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection)
{
    if (!cmdList || !m_brushRaycastPipeline.IsValid()) {
        return;
    }

    // Transition voxel read buffer to SRV state for compute shader read
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind brush raycast pipeline
    m_brushRaycastPipeline.Bind(cmdList);

    // Brush raycast constants (16 DWORDs = 64 bytes)
    struct BrushRaycastConstants {
        float rayOriginX, rayOriginY, rayOriginZ, rayOriginW;
        float rayDirX, rayDirY, rayDirZ, rayDirW;
        uint32_t gridSizeX, gridSizeY, gridSizeZ, padding;
    } constants = {};

    constants.rayOriginX = rayOrigin.x;
    constants.rayOriginY = rayOrigin.y;
    constants.rayOriginZ = rayOrigin.z;
    constants.rayOriginW = 0.0f;

    constants.rayDirX = rayDirection.x;
    constants.rayDirY = rayDirection.y;
    constants.rayDirZ = rayDirection.z;
    constants.rayDirW = 0.0f;

    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.padding = 0;

    m_brushRaycastPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors: t0 = voxel grid SRV, u0 = result UAV
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 2, world.GetBrushRaycastResultBuffer().GetShaderVisibleUAV().gpu);

    // Dispatch single thread (1x1x1)
    cmdList->Dispatch(1, 1, 1);

    // UAV barrier to ensure result is ready
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = world.GetBrushRaycastResultBuffer().GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);
}

void PhysicsDispatcher::DispatchGroundRaycast(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection)
{
    if (!cmdList || !m_brushRaycastPipeline.IsValid()) {
        return;
    }

    // Transition voxel read buffer to SRV state for compute shader read
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind brush raycast pipeline (reuse same shader, different output buffer)
    m_brushRaycastPipeline.Bind(cmdList);

    // Ground raycast constants (same structure as brush raycast)
    struct GroundRaycastConstants {
        float rayOriginX, rayOriginY, rayOriginZ, rayOriginW;
        float rayDirX, rayDirY, rayDirZ, rayDirW;
        uint32_t gridSizeX, gridSizeY, gridSizeZ, padding;
    } constants = {};

    constants.rayOriginX = rayOrigin.x;
    constants.rayOriginY = rayOrigin.y;
    constants.rayOriginZ = rayOrigin.z;
    constants.rayOriginW = 0.0f;

    constants.rayDirX = rayDirection.x;
    constants.rayDirY = rayDirection.y;
    constants.rayDirZ = rayDirection.z;
    constants.rayDirW = 0.0f;

    constants.gridSizeX = world.GetGridSizeX();
    constants.gridSizeY = world.GetGridSizeY();
    constants.gridSizeZ = world.GetGridSizeZ();
    constants.padding = 0;

    m_brushRaycastPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors: t0 = voxel grid SRV, u0 = GROUND result UAV (different from brush!)
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 2, world.GetGroundRaycastResultBuffer().GetShaderVisibleUAV().gpu);

    // Dispatch single thread (1x1x1)
    cmdList->Dispatch(1, 1, 1);

    // UAV barrier to ensure result is ready
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = world.GetGroundRaycastResultBuffer().GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);
}

Result<void> PhysicsDispatcher::CreateBrushRaycastPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_BrushRaycast.hlsl";

    auto csResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!csResult) {
        return Error("Failed to compile CS_BrushRaycast.hlsl: {}", csResult.error());
    }

    Graphics::CompiledShader cs = csResult.value();
    if (!cs.IsValid()) {
        return Error("CS_BrushRaycast shader compilation failed: {}", cs.errors);
    }

    // Root signature:
    // b0: BrushRaycastConstants (12 DWORDs)
    // t0: VoxelGrid SRV (descriptor table)
    // u0: BrushRaycastResult UAV (descriptor table)
    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "BrushRaycastPipeline";

    // b0: Brush raycast constants (inline)
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        12  // 12 uint32s (BrushRaycastConstants)
    });

    // t0: Voxel grid SRV
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register t0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });

    // u0: Brush raycast result UAV
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_brushRaycastPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create brush raycast pipeline: {}", result.error());
    }

    spdlog::info("Brush raycast pipeline created successfully (GPU raycasting enabled)");
    return {};
}

} // namespace VENPOD::Simulation
