#include "InfiniteChunkManager.h"
#include "../Graphics/RHI/d3dx12.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "../Graphics/RHI/DX12ComputePipeline.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_set>

namespace VENPOD::Simulation {

Result<void> InfiniteChunkManager::Initialize(
    ID3D12Device* device,
    Graphics::DescriptorHeapManager& heapManager,
    const InfiniteChunkConfig& config)
{
    if (!device) {
        return Error("InfiniteChunkManager::Initialize - device is null");
    }

    m_device = device;
    m_heapManager = &heapManager;
    m_config = config;

    // Create generation compute pipeline
    auto result = CreateGenerationPipeline(device);
    if (!result) {
        return Error("Failed to create generation pipeline: {}", result.error());
    }

    spdlog::info("InfiniteChunkManager initialized - render distance: {}×{} (horiz×vert), seed: {}",
        m_config.renderDistanceHorizontal,
        m_config.renderDistanceVertical,
        m_config.worldSeed);

    return {};
}

void InfiniteChunkManager::Shutdown() {
    // Free all loaded chunks
    for (auto& [coord, chunk] : m_loadedChunks) {
        if (chunk) {
            chunk->Shutdown();
            delete chunk;
        }
    }
    m_loadedChunks.clear();

    // Clear generation queue
    while (!m_generationQueue.empty()) {
        m_generationQueue.pop();
    }

    m_generationPSO.Reset();
    m_generationRootSignature.Reset();

    m_device = nullptr;
    m_heapManager = nullptr;

    spdlog::info("InfiniteChunkManager shut down");
}

void InfiniteChunkManager::Update(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const glm::vec3& cameraWorldPos)
{
    if (!device || !cmdList || !m_heapManager) {
        return;
    }

    // ===== STEP 1: Calculate camera's chunk coordinate =====
    ChunkCoord cameraChunk = ChunkCoord::FromWorldPosition(
        static_cast<int32_t>(cameraWorldPos.x),
        static_cast<int32_t>(cameraWorldPos.y),
        static_cast<int32_t>(cameraWorldPos.z),
        INFINITE_CHUNK_SIZE
    );

    // Only update if camera moved to different chunk (avoid redundant work)
    if (cameraChunk == m_lastCameraChunk) {
        // Still generate queued chunks even if camera hasn't moved
        if (!m_generationQueue.empty()) {
            GenerateNextChunk(device, cmdList);
        }
        return;
    }

    m_lastCameraChunk = cameraChunk;

    spdlog::debug("Camera chunk: [{},{},{}] - world pos: ({:.1f},{:.1f},{:.1f})",
        cameraChunk.x, cameraChunk.y, cameraChunk.z,
        cameraWorldPos.x, cameraWorldPos.y, cameraWorldPos.z);

    // ===== STEP 2: Queue chunks within cylindrical render distance =====
    auto queueResult = QueueChunksAroundCamera(cameraChunk);
    if (!queueResult) {
        spdlog::warn("Failed to queue chunks: {}", queueResult.error());
    }

    // ===== STEP 3: Generate N chunks per frame (avoid lag) =====
    for (uint32_t i = 0; i < m_config.chunksPerFrame; ++i) {
        if (m_generationQueue.empty()) {
            break;
        }
        GenerateNextChunk(device, cmdList);
    }

    // ===== STEP 4: Unload distant chunks =====
    UnloadDistantChunks(cameraChunk);

    spdlog::debug("Chunks loaded: {}, queued: {}",
        m_loadedChunks.size(),
        m_generationQueue.size());
}

Chunk* InfiniteChunkManager::GetChunk(const ChunkCoord& coord) {
    auto it = m_loadedChunks.find(coord);
    return (it != m_loadedChunks.end()) ? it->second : nullptr;
}

const Chunk* InfiniteChunkManager::GetChunk(const ChunkCoord& coord) const {
    auto it = m_loadedChunks.find(coord);
    return (it != m_loadedChunks.end()) ? it->second : nullptr;
}

Result<void> InfiniteChunkManager::ForceGenerateChunk(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const ChunkCoord& coord)
{
    if (!device || !cmdList) {
        return Error("ForceGenerateChunk - null parameters");
    }

    // Check if already loaded
    if (GetChunk(coord) != nullptr) {
        return {};  // Already generated
    }

    // Create chunk
    Chunk* chunk = new Chunk();
    auto result = chunk->Initialize(device, *m_heapManager, coord, "InfiniteChunk");
    if (!result) {
        delete chunk;
        return Error("Failed to initialize chunk: {}", result.error());
    }

    // Generate chunk
    result = chunk->Generate(
        device,
        cmdList,
        m_generationPSO.Get(),
        m_generationRootSignature.Get(),
        m_config.worldSeed
    );

    if (!result) {
        chunk->Shutdown();
        delete chunk;
        return Error("Failed to generate chunk: {}", result.error());
    }

    // Add to loaded chunks
    m_loadedChunks[coord] = chunk;

    spdlog::info("Force-generated chunk [{},{},{}]", coord.x, coord.y, coord.z);
    return {};
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

Result<void> InfiniteChunkManager::QueueChunksAroundCamera(const ChunkCoord& cameraChunk) {
    // Use unordered_set to avoid duplicate queue entries
    std::unordered_set<ChunkCoord> chunksToLoad;

    // ===== CYLINDRICAL LOADING PATTERN (Horizontal × Vertical) =====
    // Iterate through cylindrical volume centered on camera chunk
    for (int32_t dy = -m_config.renderDistanceVertical; dy <= m_config.renderDistanceVertical; ++dy) {
        for (int32_t dx = -m_config.renderDistanceHorizontal; dx <= m_config.renderDistanceHorizontal; ++dx) {
            for (int32_t dz = -m_config.renderDistanceHorizontal; dz <= m_config.renderDistanceHorizontal; ++dz) {
                // OPTIMIZATION: Use circular horizontal pattern (not square)
                // Only load chunks within horizontal radius
                int32_t horizontalDistSq = dx * dx + dz * dz;
                int32_t maxHorizDistSq = m_config.renderDistanceHorizontal * m_config.renderDistanceHorizontal;

                if (horizontalDistSq > maxHorizDistSq) {
                    continue;  // Skip corner chunks (outside circular radius)
                }

                ChunkCoord coord = {
                    cameraChunk.x + dx,
                    cameraChunk.y + dy,
                    cameraChunk.z + dz
                };

                // Check if already loaded
                if (m_loadedChunks.find(coord) != m_loadedChunks.end()) {
                    continue;  // Already loaded
                }

                chunksToLoad.insert(coord);
            }
        }
    }

    // Add new chunks to generation queue
    for (const auto& coord : chunksToLoad) {
        m_generationQueue.push(coord);
    }

    spdlog::debug("Queued {} new chunks for generation", chunksToLoad.size());
    return {};
}

Result<void> InfiniteChunkManager::GenerateNextChunk(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList)
{
    if (m_generationQueue.empty()) {
        return {};
    }

    ChunkCoord coord = m_generationQueue.front();
    m_generationQueue.pop();

    // Skip if already loaded (could have been queued multiple times)
    if (m_loadedChunks.find(coord) != m_loadedChunks.end()) {
        return {};
    }

    // ===== CREATE CHUNK =====
    Chunk* chunk = new Chunk();
    auto result = chunk->Initialize(device, *m_heapManager, coord, "InfiniteChunk");
    if (!result) {
        delete chunk;
        return Error("Failed to initialize chunk [{},{},{}]: {}",
            coord.x, coord.y, coord.z, result.error());
    }

    // ===== GENERATE CHUNK (Calls Chunk::Generate from Checkpoint 3) =====
    result = chunk->Generate(
        device,
        cmdList,
        m_generationPSO.Get(),
        m_generationRootSignature.Get(),
        m_config.worldSeed
    );

    if (!result) {
        chunk->Shutdown();
        delete chunk;
        return Error("Failed to generate chunk [{},{},{}]: {}",
            coord.x, coord.y, coord.z, result.error());
    }

    // ===== ADD TO LOADED CHUNKS MAP =====
    m_loadedChunks[coord] = chunk;

    spdlog::debug("Generated chunk [{},{},{}] - {} chunks loaded",
        coord.x, coord.y, coord.z, m_loadedChunks.size());

    return {};
}

void InfiniteChunkManager::UnloadDistantChunks(const ChunkCoord& cameraChunk) {
    // Iterate and unload chunks beyond unload distance
    for (auto it = m_loadedChunks.begin(); it != m_loadedChunks.end(); ) {
        const ChunkCoord& coord = it->first;

        // Calculate distance from camera chunk (separate horizontal/vertical)
        int32_t dx = std::abs(coord.x - cameraChunk.x);
        int32_t dy = std::abs(coord.y - cameraChunk.y);
        int32_t dz = std::abs(coord.z - cameraChunk.z);

        // Calculate horizontal distance (XZ plane)
        int32_t horizontalDistSq = dx * dx + dz * dz;
        int32_t maxHorizDistSq = m_config.unloadDistanceHorizontal * m_config.unloadDistanceHorizontal;

        // Unload if beyond horizontal OR vertical distance
        bool beyondHorizontal = horizontalDistSq > maxHorizDistSq;
        bool beyondVertical = dy > m_config.unloadDistanceVertical;

        if (beyondHorizontal || beyondVertical) {
            // Free GPU memory
            if (it->second) {
                it->second->Shutdown();
                delete it->second;
            }

            spdlog::debug("Unloaded chunk [{},{},{}] - distance: horiz²={}, vert={}",
                coord.x, coord.y, coord.z, horizontalDistSq, dy);

            it = m_loadedChunks.erase(it);
        } else {
            ++it;
        }
    }
}

Result<void> InfiniteChunkManager::CreateGenerationPipeline(ID3D12Device* device) {
    // ===== COMPILE SHADER =====
    Graphics::ShaderCompiler compiler;
    auto initResult = compiler.Initialize();
    if (!initResult) {
        return Error("Failed to initialize shader compiler: {}", initResult.error());
    }

    std::filesystem::path shaderPath = "assets/shaders/Compute/CS_GenerateChunk.hlsl";
    auto compileResult = compiler.CompileComputeShader(shaderPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile CS_GenerateChunk.hlsl: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("CS_GenerateChunk.hlsl compilation failed: {}", compiledShader.errors);
    }

    // ===== CREATE ROOT SIGNATURE =====
    // Root parameter 0: CBV (ChunkConstants at b0)
    // Root parameter 1: UAV (ChunkVoxelOutput at u0)

    CD3DX12_ROOT_PARAMETER1 rootParams[2];
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[1].InitAsUnorderedAccessView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(2, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
        return Error("Failed to serialize root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_generationRootSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create root signature");
    }

    // ===== CREATE PIPELINE STATE =====
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_generationRootSignature.Get();
    psoDesc.CS = compiledShader.GetBytecode();
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_generationPSO));
    if (FAILED(hr)) {
        return Error("Failed to create compute pipeline state");
    }

    m_generationPSO->SetName(L"CS_GenerateChunk_PSO");
    m_generationRootSignature->SetName(L"CS_GenerateChunk_RootSig");

    spdlog::info("Generation pipeline created successfully");
    return {};
}

} // namespace VENPOD::Simulation
