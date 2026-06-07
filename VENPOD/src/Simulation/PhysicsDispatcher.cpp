#include "PhysicsDispatcher.h"
#include "SparseVoxelTypes.h"
#include "TerrainConstants.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace VENPOD::Simulation {

namespace {

constexpr float kMaxSparseDispatchRayDistance = 8192.0f;
constexpr float kMaxSparseDispatchStepDistance = 512.0f;
constexpr uint32_t kMaxSparseDispatchPhysicsPackets = 2048;
constexpr uint32_t kMaxSparseDispatchEditDeltas = 8192;
constexpr uint32_t kMaxSparseDispatchEditDeltaRanges = 2048;

bool IsFiniteVec3(const glm::vec3& value) {
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool CanFloorToInt32(float value) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(static_cast<double>(value));
    return floored >= static_cast<double>(std::numeric_limits<int32_t>::min()) &&
        floored <= static_cast<double>(std::numeric_limits<int32_t>::max());
}

bool CanFloorVec3ToInt32(const glm::vec3& value) {
    return CanFloorToInt32(value.x) &&
        CanFloorToInt32(value.y) &&
        CanFloorToInt32(value.z);
}

float ClampFinite(float value, float minValue, float maxValue, float fallback) {
    return std::clamp(std::isfinite(value) ? value : fallback, minValue, maxValue);
}

bool IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

bool TryNormalizeFinite(const glm::vec3& value, glm::vec3& out) {
    if (!IsFiniteVec3(value)) {
        return false;
    }
    const double lengthSq =
        static_cast<double>(value.x) * static_cast<double>(value.x) +
        static_cast<double>(value.y) * static_cast<double>(value.y) +
        static_cast<double>(value.z) * static_cast<double>(value.z);
    if (!std::isfinite(lengthSq) || lengthSq <= 0.00000001) {
        return false;
    }
    const double invLength = 1.0 / std::sqrt(lengthSq);
    out = glm::vec3(
        static_cast<float>(static_cast<double>(value.x) * invLength),
        static_cast<float>(static_cast<double>(value.y) * invLength),
        static_cast<float>(static_cast<double>(value.z) * invLength));
    return true;
}

glm::vec3 NormalizeFiniteOr(const glm::vec3& value, const glm::vec3& fallback) {
    glm::vec3 normalized;
    if (TryNormalizeFinite(value, normalized)) {
        return normalized;
    }
    return fallback;
}

} // namespace

Result<void> PhysicsDispatcher::Initialize(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    Graphics::DescriptorHeapManager& heapManager,
    const std::filesystem::path& shaderPath,
    const PhysicsDispatcherConfig& config)
{
    if (!device) {
        return Error("Device is null");
    }

    m_device = device;
    m_heapManager = &heapManager;

    Result<void> result = {};

    if (config.enableDenseSimulationPipelines) {
        result = CreateInitializePipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            return Error("Failed to create initialize pipeline: {}", result.error());
        }

        result = CreateGravityPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Gravity pipeline not created (shader may not exist yet): {}", result.error());
        }

        result = CreateBrushPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Brush pipeline not created: {}", result.error());
        }

        result = CreateChunkScanPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Chunk scan pipeline not created: {}", result.error());
        }

        result = CreatePrepareIndirectPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Prepare indirect pipeline not created: {}", result.error());
        }

        result = CreateGravityChunkPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Gravity chunk pipeline not created: {}", result.error());
        }
    }

    if (config.enableDenseRaycastPipelines) {
        result = CreateBrushRaycastPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Brush raycast pipeline not created: {}", result.error());
        }
    }

    if (config.enableSparseRaycastPipeline) {
        result = CreateSparseRaycastPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Sparse raycast pipeline not created: {}", result.error());
        }
    }

    if (config.enableSparseMissFeedbackPipeline) {
        result = CreateSparseMissFeedbackPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Sparse miss feedback pipeline not created: {}", result.error());
        }
    }

    if (config.enableSparseBrushFeedbackPipeline) {
        result = CreateSparseBrushFeedbackPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Sparse brush feedback pipeline not created: {}", result.error());
        }
    }

    if (config.enableSparsePhysicsPacketPipeline) {
        result = CreateSparsePhysicsPacketPipeline(device, shaderCompiler, shaderPath);
        if (!result) {
            spdlog::warn("Sparse physics packet pipeline not created: {}", result.error());
        }
    }

    if (config.enableIndirectCommandSignature) {
        result = CreateCommandSignature(device);
        if (!result) {
            spdlog::warn("Command signature not created: {}", result.error());
        }
    }

    spdlog::info(
        "PhysicsDispatcher initialized (denseSim={} denseRaycast={} sparseRaycast={} sparseFeedback={} sparseBrushFeedback={} sparsePhysicsPackets={} indirect={})",
        config.enableDenseSimulationPipelines ? 1 : 0,
        config.enableDenseRaycastPipelines ? 1 : 0,
        config.enableSparseRaycastPipeline ? 1 : 0,
        config.enableSparseMissFeedbackPipeline ? 1 : 0,
        config.enableSparseBrushFeedbackPipeline ? 1 : 0,
        config.enableSparsePhysicsPacketPipeline ? 1 : 0,
        config.enableIndirectCommandSignature ? 1 : 0);
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
    m_sparseRaycastPipeline.Shutdown();
    m_sparseMissFeedbackPipeline.Shutdown();
    m_sparseBrushFeedbackPipeline.Shutdown();
    m_sparsePhysicsPacketPipeline.Shutdown();
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

    // CRITICAL: Copy WRITE -> READ after initialization so rendering sees terrain on Frame 0
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(world.GetReadBuffer().GetResource(), world.GetWriteBuffer().GetResource());
    spdlog::info("DispatchInitialize: Copied WRITE -> READ for Frame 0 rendering");

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
    //   1. UpdateActiveRegion() -> ExecuteCommandLists(chunkCopyList) -> Signal(fence)
    //   2. DispatchPhysics() -> ExecuteCommandLists(physicsList)
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
    // 0: Root constants (brush constants plus localized dispatch start and feedback controls - 20 dwords)
    desc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        20
    });

    // 1: UAV for voxel buffer (read-write)
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,  // register u0
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    // 2: UAV for compact GPU brush edit events
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,  // register u1
        0,  // space 0
        1,  // 1 descriptor
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    // 3: UAV for compact GPU brush edit counter
    desc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        2,  // register u2
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
    //   1. UpdateActiveRegion: Copy NEW chunks -> WRITE buffer
    //   2. DispatchBrush: Paint voxels -> WRITE buffer (adds to copied chunks)
    //   3. DispatchChunkScan: Scan WRITE buffer (sees chunks + painted voxels)
    //   4. DispatchPhysicsIndirect: Read READ (Frame N-1), write WRITE (Frame N)
    //   5. SwapBuffers() - READ  WRITE (WRITE becomes READ for next frame)
    //   6. Render from READ buffer (now has Frame N final state)
    //
    // Key: WRITE = Frame N workspace, READ = Frame N-1 final state (read-only)

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Bind brush pipeline
    m_brushPipeline.Bind(cmdList);

    struct BrushDispatchConstants {
        float positionX;
        float positionY;
        float positionZ;
        float radius;
        uint32_t material;
        uint32_t mode;
        uint32_t shape;
        float strength;
        uint32_t gridSizeX;
        uint32_t gridSizeY;
        uint32_t gridSizeZ;
        uint32_t seed;
        uint32_t startX;
        uint32_t startY;
        uint32_t startZ;
        uint32_t padding;
        uint32_t recordEdits;
        uint32_t maxEditEvents;
        uint32_t feedbackFrame;
        uint32_t feedbackPadding;
    };

    const int32_t radiusCeil = static_cast<int32_t>(std::ceil(brushConstants.radius)) + 2;
    int32_t startX = std::max<int32_t>(0, static_cast<int32_t>(std::floor(brushConstants.positionX)) - radiusCeil);
    int32_t startY = std::max<int32_t>(0, static_cast<int32_t>(std::floor(brushConstants.positionY)) - radiusCeil);
    int32_t startZ = std::max<int32_t>(0, static_cast<int32_t>(std::floor(brushConstants.positionZ)) - radiusCeil);
    int32_t endX = std::min<int32_t>(static_cast<int32_t>(world.GetGridSizeX()), static_cast<int32_t>(std::ceil(brushConstants.positionX)) + radiusCeil + 1);
    int32_t endY = std::min<int32_t>(static_cast<int32_t>(world.GetGridSizeY()), static_cast<int32_t>(std::ceil(brushConstants.positionY)) + radiusCeil + 1);
    int32_t endZ = std::min<int32_t>(static_cast<int32_t>(world.GetGridSizeZ()), static_cast<int32_t>(std::ceil(brushConstants.positionZ)) + radiusCeil + 1);

    if (endX <= startX || endY <= startY || endZ <= startZ) {
        return;
    }

    BrushDispatchConstants constants = {};
    constants.positionX = brushConstants.positionX;
    constants.positionY = brushConstants.positionY;
    constants.positionZ = brushConstants.positionZ;
    constants.radius = brushConstants.radius;
    constants.material = brushConstants.material;
    constants.mode = brushConstants.mode;
    constants.shape = brushConstants.shape;
    constants.strength = brushConstants.strength;
    constants.gridSizeX = brushConstants.gridSizeX;
    constants.gridSizeY = brushConstants.gridSizeY;
    constants.gridSizeZ = brushConstants.gridSizeZ;
    constants.seed = brushConstants.seed;
    constants.startX = static_cast<uint32_t>(startX);
    constants.startY = static_cast<uint32_t>(startY);
    constants.startZ = static_cast<uint32_t>(startZ);
    constants.maxEditEvents = world.GetMaxBrushEditFeedbackEvents();

    auto dispatchSize = glm::uvec3(
        (static_cast<uint32_t>(endX - startX) + 7) / 8,
        (static_cast<uint32_t>(endY - startY) + 7) / 8,
        (static_cast<uint32_t>(endZ - startZ) + 7) / 8
    );

    const bool feedbackRecording = world.BeginBrushEditFeedback(cmdList);
    m_brushPipeline.SetRootDescriptorTable(cmdList, 2, world.GetBrushEditEventUAV().gpu);
    m_brushPipeline.SetRootDescriptorTable(cmdList, 3, world.GetBrushEditCounterUAV().gpu);

    // Paint to both buffers. In infinite streaming mode the READ buffer is the
    // authoritative visible cache, so GPU feedback records the exact visible
    // READ writes and the WRITE pass mirrors the result for local simulation.
    constants.recordEdits = feedbackRecording ? 1u : 0u;
    m_brushPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_brushPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferUAV().gpu);
    m_brushPipeline.Dispatch(cmdList, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    // UAV barrier on READ buffer
    {
        auto& readBuffer = world.GetReadBuffer();
        ID3D12Resource* resource = readBuffer.GetResource();
        if (resource) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = resource;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    constants.recordEdits = 0;
    m_brushPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_brushPipeline.SetRootDescriptorTable(cmdList, 1, world.GetWriteBufferUAV().gpu);
    m_brushPipeline.Dispatch(cmdList, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    // UAV barrier on WRITE buffer
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

    if (feedbackRecording) {
        world.QueueBrushEditFeedbackReadback(cmdList);
    }

    if (world.IsUsingInfiniteChunks()) {
        // Immediate sparse overlay prevents a later chunk copy/refill from
        // briefly replacing a freshly painted READ/WRITE slot with generated
        // terrain before async GPU edit feedback retires. The feedback path
        // still records exact shader writes into the same per-voxel keys.
        world.RecordPersistentBrushEdit(
            brushConstants.positionX,
            brushConstants.positionY,
            brushConstants.positionZ,
            brushConstants.radius,
            brushConstants.material,
            brushConstants.mode,
            brushConstants.shape,
            brushConstants.strength,
            brushConstants.seed,
            brushConstants.hitNormalX,
            brushConstants.hitNormalY,
            brushConstants.hitNormalZ,
            brushConstants.hasHitNormal != 0);
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
    uint32_t frameIndex,
    const glm::vec3& centerLocal,
    uint32_t scanBudgetChunks)
{
    if (!cmdList || !m_chunkScanPipeline.IsValid()) {
        spdlog::warn("DispatchChunkScan: pipeline or cmdList invalid");
        return;
    }

    // Infinite streaming no longer relies on a full-frame ping-pong swap. The
    // READ buffer is the currently rendered authoritative near field, while the
    // WRITE buffer is kept mirrored for copy/brush safety. Scan READ so physics
    // follows the world the player actually sees.

    // Reset active chunk count to zero before scanning
    chunkManager.ResetActiveCount(cmdList);

    world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

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

    const uint32_t fullChunksX = chunkManager.GetChunkCountX();
    const uint32_t fullChunksY = chunkManager.GetChunkCountY();
    const uint32_t fullChunksZ = chunkManager.GetChunkCountZ();

    const uint32_t requestedBudget = std::max<uint32_t>(1, scanBudgetChunks);
    const uint32_t dispatchY = std::min<uint32_t>(4, fullChunksY);
    const uint32_t xzBudget = std::max<uint32_t>(1, requestedBudget / std::max<uint32_t>(1, dispatchY));
    const uint32_t side = std::max<uint32_t>(4, static_cast<uint32_t>(std::sqrt(static_cast<float>(xzBudget))));
    const uint32_t dispatchX = std::min<uint32_t>(side, fullChunksX);
    const uint32_t dispatchZ = std::min<uint32_t>(side, fullChunksZ);

    const int32_t centerChunkX = std::clamp(
        static_cast<int32_t>(centerLocal.x / static_cast<float>(CHUNK_SIZE)),
        0,
        static_cast<int32_t>(fullChunksX) - 1);
    const int32_t centerChunkY = std::clamp(
        static_cast<int32_t>(centerLocal.y / static_cast<float>(CHUNK_SIZE)),
        0,
        static_cast<int32_t>(fullChunksY) - 1);
    const int32_t centerChunkZ = std::clamp(
        static_cast<int32_t>(centerLocal.z / static_cast<float>(CHUNK_SIZE)),
        0,
        static_cast<int32_t>(fullChunksZ) - 1);

    const int32_t maxOffsetX = static_cast<int32_t>(fullChunksX - dispatchX);
    const int32_t maxOffsetY = static_cast<int32_t>(fullChunksY - dispatchY);
    const int32_t maxOffsetZ = static_cast<int32_t>(fullChunksZ - dispatchZ);
    const int32_t offsetX = std::clamp(centerChunkX - static_cast<int32_t>(dispatchX / 2), 0, maxOffsetX);
    const int32_t offsetY = std::clamp(centerChunkY - static_cast<int32_t>(dispatchY / 2), 0, maxOffsetY);
    const int32_t offsetZ = std::clamp(centerChunkZ - static_cast<int32_t>(dispatchZ / 2), 0, maxOffsetZ);

    // Pass the offset to the shader so it knows where the scanned region starts
    constants.activeRegionOffsetX = offsetX;
    constants.activeRegionOffsetY = offsetY;
    constants.activeRegionOffsetZ = offsetZ;

    m_stats.scanBudgetChunks = requestedBudget;
    m_stats.scannedChunksLastFrame = dispatchX * dispatchY * dispatchZ;
    m_stats.skippedScanChunksLastFrame =
        requestedBudget > m_stats.scannedChunksLastFrame
            ? requestedBudget - m_stats.scannedChunksLastFrame
            : 0;
    m_stats.theoreticalChunkUniverse = fullChunksX * fullChunksY * fullChunksZ;
    m_stats.dispatchX = dispatchX;
    m_stats.dispatchY = dispatchY;
    m_stats.dispatchZ = dispatchZ;
    m_stats.offsetX = offsetX;
    m_stats.offsetY = offsetY;
    m_stats.offsetZ = offsetZ;

    // Log only once per second to avoid spam
    static int activeRegionLogThrottle = 0;
    if (++activeRegionLogThrottle % 60 == 1) {
        spdlog::debug("DispatchChunkScan: Scanning {}x{}x{} = {} physics chunks at offset ({},{},{})",
            dispatchX, dispatchY, dispatchZ,
            dispatchX * dispatchY * dispatchZ,
            offsetX, offsetY, offsetZ);
    }

    m_chunkScanPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 2, chunkManager.GetChunkControlUAV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 3, chunkManager.GetActiveListUAV().gpu);
    m_chunkScanPipeline.SetRootDescriptorTable(cmdList, 4, chunkManager.GetActiveCountUAV().gpu);

    // Dispatch the local physics window.
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
        spdlog::debug("DispatchChunkScan: Dispatched {}x{}x{} chunks (ChunkManager grid: {}x{}x{})",
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
        // In the large streaming world, the dense fallback scans the entire
        // 400M+ voxel window and can look like a crash. Skip physics until the
        // chunk-budgeted path is ready instead of issuing full-buffer work.
        (void)world;
        (void)deltaTime;
        (void)frameIndex;
        return;
    }

    // === Step 0: REMOVED REDUNDANT READ->WRITE COPY ===
    // CRITICAL FIX: The previous 64 MB READ->WRITE copy was DESTROYING newly copied chunks!
    //
    // Timeline issue (before fix):
    //   1. UpdateActiveRegion() -> copies chunks to WRITE buffer (separate command list)
    //   2. DispatchPhysicsIndirect() -> copies READ->WRITE (OVERWRITES the new chunks!)
    //   3. Physics runs on corrupted data -> crashes or rendering bugs
    //
    // The correct architecture is:
    //   - UpdateActiveRegion writes NEW chunks -> WRITE buffer
    //   - Brush paints -> WRITE buffer
    //   - ChunkScan reads WRITE buffer to find active chunks
    //   - Physics reads READ (old frame), writes WRITE (new frame) - preserves chunk data!
    //   - SwapBuffers() makes WRITE become READ for next frame
    //
    // No READ->WRITE copy is needed because:
    //   - For static grid: WRITE already has complete data from previous physics pass
    //   - For infinite chunks: UpdateActiveRegion copies chunks directly to WRITE
    //
    // This fix also avoids a redundant 64 MB copy on each physics frame.

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
    // The infinite-world dense buffers are kept as mirrored caches. Physics is
    // a local in-place update, so run the same active chunk work against both
    // buffers. This avoids the old alternating-buffer flicker where only WRITE
    // had the simulated result and SwapBuffers bounced the renderer between
    // different voxel states.
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

    auto dispatchInPlace = [&](bool readBuffer) {
        if (readBuffer) {
            world.TransitionReadBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            world.TransitionWriteBufferTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        cmdList->SetDescriptorHeaps(1, heaps);
        m_gravityChunkPipeline.Bind(cmdList);
        m_gravityChunkPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
        m_gravityChunkPipeline.SetRootDescriptorTable(cmdList, 1, chunkManager.GetActiveListSRV().gpu);
        m_gravityChunkPipeline.SetRootDescriptorTable(
            cmdList,
            2,
            readBuffer ? world.GetReadBufferUAV().gpu : world.GetWriteBufferUAV().gpu);

        cmdList->ExecuteIndirect(
            m_commandSignature.Get(),
            1,
            chunkManager.GetIndirectArgsBuffer().GetResource(),
            0,
            nullptr,
            0);

        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = readBuffer
            ? world.GetReadBuffer().GetResource()
            : world.GetWriteBuffer().GetResource();
        cmdList->ResourceBarrier(1, &uavBarrier);
    };

    dispatchInPlace(true);
    dispatchInPlace(false);

    // REMOVED DUPLICATE: SwapBuffers() is called in main loop after physics!
    // Double-swapping was causing READ and WRITE buffers to point to same buffer,
    // preventing chunk copy logic from working (cache thought all chunks were already copied).
    // world.SwapBuffers();  // <-- REMOVED - main.cpp calls this after physics

    // Log only once per second to avoid spam
    static int physicsLogThrottle = 0;
    if (++physicsLogThrottle % 60 == 1) {
        spdlog::debug("DispatchPhysicsIndirect: Indirect physics dispatch complete");
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
        float regionOriginX, regionOriginY, regionOriginZ, regionOriginW;
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
    const glm::vec3 regionOrigin = world.GetRegionOriginWorld();
    constants.regionOriginX = regionOrigin.x;
    constants.regionOriginY = regionOrigin.y;
    constants.regionOriginZ = regionOrigin.z;
    constants.regionOriginW = 0.0f;

    m_brushRaycastPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors: t0 = voxel grid SRV, t1 = valid chunk mask, u0 = result UAV
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 2, world.GetReadChunkValidMaskSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 3, world.GetBrushRaycastResultBuffer().GetShaderVisibleUAV().gpu);

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
        float regionOriginX, regionOriginY, regionOriginZ, regionOriginW;
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
    const glm::vec3 regionOrigin = world.GetRegionOriginWorld();
    constants.regionOriginX = regionOrigin.x;
    constants.regionOriginY = regionOrigin.y;
    constants.regionOriginZ = regionOrigin.z;
    constants.regionOriginW = 0.0f;

    m_brushRaycastPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);

    // Set descriptors: t0 = voxel grid SRV, t1 = valid chunk mask, u0 = GROUND result UAV
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 1, world.GetReadBufferSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 2, world.GetReadChunkValidMaskSRV().gpu);
    m_brushRaycastPipeline.SetRootDescriptorTable(cmdList, 3, world.GetGroundRaycastResultBuffer().GetShaderVisibleUAV().gpu);

    // Dispatch single thread (1x1x1)
    cmdList->Dispatch(1, 1, 1);

    // UAV barrier to ensure result is ready
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = world.GetGroundRaycastResultBuffer().GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);
}

void PhysicsDispatcher::DispatchSparseRaycast(
    ID3D12GraphicsCommandList* cmdList,
    VoxelWorld& world,
    const Graphics::DescriptorHandle& sparseBrickPoolSRV,
    const Graphics::DescriptorHandle& sparsePageTableSRV,
    const Graphics::DescriptorHandle& sparseOccupancySRV,
    const Graphics::DescriptorHandle& sparsePageGenerationSRV,
    uint32_t maxBrickPages,
    uint32_t pageTableCapacity,
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection,
    float maxDistance,
    bool writeGroundResult)
{
    if (!cmdList || !m_sparseRaycastPipeline.IsValid() || !m_heapManager ||
        !sparseBrickPoolSRV.IsValid() || !sparsePageTableSRV.IsValid() ||
        !sparseOccupancySRV.IsValid() || !sparsePageGenerationSRV.IsValid() ||
        maxBrickPages == 0 || pageTableCapacity == 0 ||
        !IsFiniteVec3(rayOrigin) || !CanFloorVec3ToInt32(rayOrigin) ||
        !std::isfinite(maxDistance) || maxDistance <= 0.0f) {
        return;
    }

    glm::vec3 rayDir;
    if (!TryNormalizeFinite(rayDirection, rayDir)) {
        return;
    }
    const float boundedMaxDistance =
        ClampFinite(maxDistance, 0.0f, kMaxSparseDispatchRayDistance, 192.0f);
    if (boundedMaxDistance <= 0.0f) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    m_sparseRaycastPipeline.Bind(cmdList);

    struct SparseRaycastConstants {
        float rayOriginX, rayOriginY, rayOriginZ, maxDistance;
        float rayDirX, rayDirY, rayDirZ, flags;
        uint32_t maxBrickPages;
        uint32_t pageTableCapacity;
        uint32_t maxSteps;
        uint32_t padding0;
    } constants = {};

    constants.rayOriginX = rayOrigin.x;
    constants.rayOriginY = rayOrigin.y;
    constants.rayOriginZ = rayOrigin.z;
    constants.maxDistance = boundedMaxDistance;
    constants.rayDirX = rayDir.x;
    constants.rayDirY = rayDir.y;
    constants.rayDirZ = rayDir.z;
    constants.flags = writeGroundResult ? 1.0f : 0.0f;
    constants.maxBrickPages = maxBrickPages;
    constants.pageTableCapacity = pageTableCapacity;
    constants.maxSteps = static_cast<uint32_t>(
        std::clamp(boundedMaxDistance * 3.0f + 64.0f, 64.0f, 16384.0f));

    m_sparseRaycastPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    m_sparseRaycastPipeline.SetRootDescriptorTable(cmdList, 1, sparseBrickPoolSRV.gpu);
    m_sparseRaycastPipeline.SetRootDescriptorTable(cmdList, 2, sparsePageTableSRV.gpu);
    m_sparseRaycastPipeline.SetRootDescriptorTable(cmdList, 3, sparseOccupancySRV.gpu);
    m_sparseRaycastPipeline.SetRootDescriptorTable(
        cmdList,
        4,
        sparsePageGenerationSRV.gpu);
    m_sparseRaycastPipeline.SetRootDescriptorTable(
        cmdList,
        5,
        writeGroundResult
            ? world.GetGroundRaycastResultBuffer().GetShaderVisibleUAV().gpu
            : world.GetBrushRaycastResultBuffer().GetShaderVisibleUAV().gpu);

    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = writeGroundResult
        ? world.GetGroundRaycastResultBuffer().GetResource()
        : world.GetBrushRaycastResultBuffer().GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);
}

void PhysicsDispatcher::DispatchSparseMissFeedback(
    ID3D12GraphicsCommandList* cmdList,
    const Graphics::DescriptorHandle& sparsePageTableSRV,
    const Graphics::DescriptorHandle& sparseMissFeedbackUAV,
    uint32_t maxBrickPages,
    uint32_t pageTableCapacity,
    const glm::vec3& cameraOrigin,
    const glm::vec3& cameraForward,
    const glm::vec3& cameraRight,
    const glm::vec3& cameraUp,
    float verticalFovRadians,
    float aspectRatio,
    float maxDistance,
    float stepDistance,
    uint32_t rayGrid,
    uint32_t maxRecords,
    uint32_t frameIndex)
{
    if (!cmdList || !m_sparseMissFeedbackPipeline.IsValid() || !m_heapManager ||
        !sparsePageTableSRV.IsValid() || !sparseMissFeedbackUAV.IsValid() ||
        maxBrickPages == 0 || pageTableCapacity == 0 || maxRecords == 0 ||
        rayGrid == 0 || !IsFiniteVec3(cameraOrigin) ||
        !CanFloorVec3ToInt32(cameraOrigin) ||
        !std::isfinite(maxDistance) || maxDistance <= 0.0f) {
        return;
    }

    const glm::vec3 boundedForward = NormalizeFiniteOr(cameraForward, {0.0f, 0.0f, 1.0f});
    const glm::vec3 boundedRight = NormalizeFiniteOr(cameraRight, {1.0f, 0.0f, 0.0f});
    const glm::vec3 boundedUp = NormalizeFiniteOr(cameraUp, {0.0f, 1.0f, 0.0f});
    const float boundedMaxDistance =
        ClampFinite(maxDistance, 0.0f, kMaxSparseDispatchRayDistance, 192.0f);
    const float boundedStepDistance =
        ClampFinite(stepDistance, 1.0f, kMaxSparseDispatchStepDistance, 16.0f);
    if (boundedMaxDistance <= 0.0f) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    m_sparseMissFeedbackPipeline.Bind(cmdList);

    struct SparseMissFeedbackConstants {
        float cameraOriginX, cameraOriginY, cameraOriginZ, maxDistance;
        float cameraForwardX, cameraForwardY, cameraForwardZ, stepDistance;
        float cameraRightX, cameraRightY, cameraRightZ, tanHalfFov;
        float cameraUpX, cameraUpY, cameraUpZ, aspectRatio;
        uint32_t maxBrickPages;
        uint32_t pageTableCapacity;
        uint32_t rayGrid;
        uint32_t maxRecords;
        uint32_t maxSteps;
        uint32_t frameIndex;
        uint32_t padding0;
        uint32_t padding1;
    } constants = {};

    constants.cameraOriginX = cameraOrigin.x;
    constants.cameraOriginY = cameraOrigin.y;
    constants.cameraOriginZ = cameraOrigin.z;
    constants.maxDistance = boundedMaxDistance;
    constants.cameraForwardX = boundedForward.x;
    constants.cameraForwardY = boundedForward.y;
    constants.cameraForwardZ = boundedForward.z;
    constants.stepDistance = boundedStepDistance;
    constants.cameraRightX = boundedRight.x;
    constants.cameraRightY = boundedRight.y;
    constants.cameraRightZ = boundedRight.z;
    constants.tanHalfFov =
        std::tan(ClampFinite(verticalFovRadians, 0.1f, 2.8f, 1.04719755f) * 0.5f);
    constants.cameraUpX = boundedUp.x;
    constants.cameraUpY = boundedUp.y;
    constants.cameraUpZ = boundedUp.z;
    constants.aspectRatio = ClampFinite(aspectRatio, 0.25f, 4.0f, 1.7777778f);
    constants.maxBrickPages = maxBrickPages;
    constants.pageTableCapacity = pageTableCapacity;
    constants.rayGrid = std::clamp(rayGrid, 1u, 16u);
    constants.maxRecords = maxRecords;
    constants.maxSteps = static_cast<uint32_t>(std::clamp(
        constants.maxDistance / constants.stepDistance + 1.0f,
        1.0f,
        4096.0f));
    constants.frameIndex = frameIndex;

    m_sparseMissFeedbackPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    m_sparseMissFeedbackPipeline.SetRootDescriptorTable(cmdList, 1, sparsePageTableSRV.gpu);
    m_sparseMissFeedbackPipeline.SetRootDescriptorTable(cmdList, 2, sparseMissFeedbackUAV.gpu);
    const uint32_t groupCount =
        std::max(1u, (constants.rayGrid + 7u) / 8u);
    cmdList->Dispatch(groupCount, groupCount, 1);
}

void PhysicsDispatcher::DispatchSparseBrushFeedback(
    ID3D12GraphicsCommandList* cmdList,
    const Graphics::DescriptorHandle& sparseBrickPoolSRV,
    const Graphics::DescriptorHandle& sparsePageTableSRV,
    const Graphics::DescriptorHandle& sparseOccupancySRV,
    const Graphics::DescriptorHandle& sparsePageGenerationSRV,
    const Graphics::DescriptorHandle& sparseBrushFeedbackUAV,
    uint32_t maxBrickPages,
    uint32_t pageTableCapacity,
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal,
    uint32_t maxRecords,
    uint32_t frameIndex)
{
    if (!cmdList || !m_sparseBrushFeedbackPipeline.IsValid() || !m_heapManager ||
        !sparseBrickPoolSRV.IsValid() || !sparsePageTableSRV.IsValid() ||
        !sparseOccupancySRV.IsValid() || !sparsePageGenerationSRV.IsValid() ||
        !sparseBrushFeedbackUAV.IsValid() || maxBrickPages == 0 ||
        pageTableCapacity == 0 || maxRecords == 0 || radius <= 0.0f) {
        return;
    }

    SparseBrushVoxelBounds brushBounds;
    if (!TryBuildSparseBrushVoxelBounds(
            worldPositionX,
            worldPositionY,
            worldPositionZ,
            radius,
            strength,
            &brushBounds)) {
        return;
    }

    const uint32_t volumeX = static_cast<uint32_t>(brushBounds.endX - brushBounds.startX);
    const uint32_t volumeY = static_cast<uint32_t>(brushBounds.endY - brushBounds.startY);
    const uint32_t volumeZ = static_cast<uint32_t>(brushBounds.endZ - brushBounds.startZ);
    if (volumeX == 0 || volumeY == 0 || volumeZ == 0) {
        return;
    }

    struct SparseBrushFeedbackConstants {
        float positionX, positionY, positionZ, radius;
        uint32_t material, mode, shape;
        float strength;
        int32_t startX, startY, startZ;
        uint32_t volumeX;
        uint32_t volumeY, volumeZ, seed, maxRecords;
        int32_t hitNormalX, hitNormalY, hitNormalZ;
        uint32_t hasHitNormal;
        uint32_t maxBrickPages, pageTableCapacity, frameIndex, padding0;
    } constants = {};

    constants.positionX = worldPositionX;
    constants.positionY = worldPositionY;
    constants.positionZ = worldPositionZ;
    constants.radius = brushBounds.radius;
    constants.material = material;
    constants.mode = mode;
    constants.shape = shape;
    constants.strength = brushBounds.strength;
    constants.startX = brushBounds.startX;
    constants.startY = brushBounds.startY;
    constants.startZ = brushBounds.startZ;
    constants.volumeX = volumeX;
    constants.volumeY = volumeY;
    constants.volumeZ = volumeZ;
    constants.seed = seed;
    constants.maxRecords = maxRecords;
    constants.hitNormalX = hitNormalX;
    constants.hitNormalY = hitNormalY;
    constants.hitNormalZ = hitNormalZ;
    constants.hasHitNormal = hasHitNormal ? 1u : 0u;
    constants.maxBrickPages = maxBrickPages;
    constants.pageTableCapacity = pageTableCapacity;
    constants.frameIndex = frameIndex;

    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    m_sparseBrushFeedbackPipeline.Bind(cmdList);
    m_sparseBrushFeedbackPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    m_sparseBrushFeedbackPipeline.SetRootDescriptorTable(cmdList, 1, sparseBrickPoolSRV.gpu);
    m_sparseBrushFeedbackPipeline.SetRootDescriptorTable(cmdList, 2, sparsePageTableSRV.gpu);
    m_sparseBrushFeedbackPipeline.SetRootDescriptorTable(cmdList, 3, sparseOccupancySRV.gpu);
    m_sparseBrushFeedbackPipeline.SetRootDescriptorTable(cmdList, 4, sparsePageGenerationSRV.gpu);
    m_sparseBrushFeedbackPipeline.SetRootDescriptorTable(cmdList, 5, sparseBrushFeedbackUAV.gpu);
    cmdList->Dispatch(
        (volumeX + 7u) / 8u,
        (volumeY + 7u) / 8u,
        (volumeZ + 7u) / 8u);
}

void PhysicsDispatcher::DispatchSparsePhysicsPackets(
    ID3D12GraphicsCommandList* cmdList,
    const Graphics::DescriptorHandle& sparsePhysicsPacketSRV,
    const Graphics::DescriptorHandle& sparsePageTableSRV,
    const Graphics::DescriptorHandle& sparseBrickPoolSRV,
    const Graphics::DescriptorHandle& sparseEditDeltaSRV,
    const Graphics::DescriptorHandle& sparseEditDeltaRangeSRV,
    const Graphics::DescriptorHandle& sparseEditDeltaRangeTableSRV,
    const Graphics::DescriptorHandle& sparsePhysicsPacketResultUAV,
    const Graphics::DescriptorHandle& sparsePhysicsDiagnosticsUAV,
    uint32_t pageTableCapacity,
    uint32_t packetCount,
    uint32_t editDeltaCount,
    uint32_t editDeltaRangeCount,
    uint32_t editDeltaRangeTableCapacity,
    uint32_t frameIndex)
{
    if (!cmdList || !m_sparsePhysicsPacketPipeline.IsValid() || !m_heapManager ||
        !sparsePhysicsPacketSRV.IsValid() || !sparsePageTableSRV.IsValid() ||
        !sparseBrickPoolSRV.IsValid() || !sparseEditDeltaSRV.IsValid() ||
        !sparseEditDeltaRangeSRV.IsValid() ||
        !sparseEditDeltaRangeTableSRV.IsValid() ||
        !sparsePhysicsPacketResultUAV.IsValid() ||
        !sparsePhysicsDiagnosticsUAV.IsValid() ||
        packetCount == 0 ||
        packetCount > kMaxSparseDispatchPhysicsPackets ||
        pageTableCapacity == 0 ||
        !IsPowerOfTwo(pageTableCapacity) ||
        editDeltaCount > kMaxSparseDispatchEditDeltas ||
        editDeltaRangeCount > kMaxSparseDispatchEditDeltaRanges ||
        editDeltaRangeCount > editDeltaCount ||
        (editDeltaRangeTableCapacity > 0 && !IsPowerOfTwo(editDeltaRangeTableCapacity)) ||
        (editDeltaRangeCount > 0 && editDeltaRangeTableCapacity < editDeltaRangeCount)) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    m_sparsePhysicsPacketPipeline.Bind(cmdList);

    struct SparsePhysicsPacketConstants {
        uint32_t packetCount;
        uint32_t frameIndex;
        uint32_t pageTableCapacity;
        uint32_t editDeltaCount;
        uint32_t editDeltaRangeCount;
        uint32_t editDeltaRangeTableCapacity;
        uint32_t padding1;
        uint32_t padding2;
    } constants = {};

    constants.packetCount = packetCount;
    constants.frameIndex = frameIndex;
    constants.pageTableCapacity = pageTableCapacity;
    constants.editDeltaCount = editDeltaCount;
    constants.editDeltaRangeCount = editDeltaRangeCount;
    constants.editDeltaRangeTableCapacity = editDeltaRangeTableCapacity;

    m_sparsePhysicsPacketPipeline.SetRoot32BitConstants(cmdList, 0, sizeof(constants) / 4, &constants);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 1, sparsePhysicsPacketSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 2, sparsePageTableSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 3, sparseBrickPoolSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 4, sparseEditDeltaSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 5, sparseEditDeltaRangeSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 6, sparseEditDeltaRangeTableSRV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 7, sparsePhysicsPacketResultUAV.gpu);
    m_sparsePhysicsPacketPipeline.SetRootDescriptorTable(cmdList, 8, sparsePhysicsDiagnosticsUAV.gpu);
    const uint32_t groupsX = (packetCount + 63u) / 64u;
    m_sparsePhysicsPacketPipeline.Dispatch(cmdList, groupsX, 1, 1);
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
    // b0: BrushRaycastConstants (16 DWORDs)
    // t0: VoxelGrid SRV (descriptor table)
    // t1: ChunkValidMask SRV (descriptor table)
    // u0: BrushRaycastResult UAV (descriptor table)
    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "BrushRaycastPipeline";

    // b0: Brush raycast constants (inline)
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,  // register b0
        0,  // space 0
        16  // 16 uint32s (BrushRaycastConstants)
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
        1,  // register t1
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

Result<void> PhysicsDispatcher::CreateSparseRaycastPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparseRaycast.hlsl";

    auto csResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!csResult) {
        return Error("Failed to compile CS_SparseRaycast.hlsl: {}", csResult.error());
    }

    Graphics::CompiledShader cs = csResult.value();
    if (!cs.IsValid()) {
        return Error("CS_SparseRaycast shader compilation failed: {}", cs.errors);
    }

    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "SparseRaycastPipeline";

    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,
        0,
        12
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        2,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        3,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_sparseRaycastPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create sparse raycast pipeline: {}", result.error());
    }

    spdlog::info("Sparse raycast pipeline created successfully");
    return {};
}

Result<void> PhysicsDispatcher::CreateSparseMissFeedbackPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparseMissFeedback.hlsl";

    auto csResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!csResult) {
        return Error("Failed to compile CS_SparseMissFeedback.hlsl: {}", csResult.error());
    }

    Graphics::CompiledShader cs = csResult.value();
    if (!cs.IsValid()) {
        return Error("CS_SparseMissFeedback shader compilation failed: {}", cs.errors);
    }

    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "SparseMissFeedbackPipeline";

    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,
        0,
        24
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_sparseMissFeedbackPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create sparse miss feedback pipeline: {}", result.error());
    }

    spdlog::info("Sparse miss feedback pipeline created successfully");
    return {};
}

Result<void> PhysicsDispatcher::CreateSparseBrushFeedbackPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparseBrushFeedback.hlsl";

    auto csResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!csResult) {
        return Error("Failed to compile CS_SparseBrushFeedback.hlsl: {}", csResult.error());
    }

    Graphics::CompiledShader cs = csResult.value();
    if (!cs.IsValid()) {
        return Error("CS_SparseBrushFeedback shader compilation failed: {}", cs.errors);
    }

    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "SparseBrushFeedbackPipeline";

    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,
        0,
        24
    });
    for (uint32_t srvRegister = 0; srvRegister < 4; ++srvRegister) {
        pipelineDesc.rootParams.push_back({
            Graphics::RootParamType::DescriptorTable,
            srvRegister,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
    }
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_sparseBrushFeedbackPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create sparse brush feedback pipeline: {}", result.error());
    }

    spdlog::info("Sparse brush feedback pipeline created successfully");
    return {};
}

Result<void> PhysicsDispatcher::CreateSparsePhysicsPacketPipeline(
    ID3D12Device* device,
    Graphics::ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparsePhysicsPackets.hlsl";

    auto csResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!csResult) {
        return Error("Failed to compile CS_SparsePhysicsPackets.hlsl: {}", csResult.error());
    }

    Graphics::CompiledShader cs = csResult.value();
    if (!cs.IsValid()) {
        return Error("CS_SparsePhysicsPackets shader compilation failed: {}", cs.errors);
    }

    Graphics::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.debugName = "SparsePhysicsPacketPipeline";

    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::Constants32Bit,
        0,
        0,
        8
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t2: sparse brick voxel pool, used for packet-local proposal validation.
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        2,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t3: same-frame sparse edit deltas overlaid on resident brick samples.
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        3,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t4: compact per-brick ranges into t3 edit deltas.
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        4,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    // t5: hash table mapping brickCoord to edit-delta range index.
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        5,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        0,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });
    pipelineDesc.rootParams.push_back({
        Graphics::RootParamType::DescriptorTable,
        1,
        0,
        1,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV
    });

    auto result = m_sparsePhysicsPacketPipeline.Initialize(device, pipelineDesc);
    if (!result) {
        return Error("Failed to create sparse physics packet pipeline: {}", result.error());
    }

    spdlog::info("Sparse physics packet pipeline created successfully");
    return {};
}

} // namespace VENPOD::Simulation
