#include "VoxelWorld.h"
#include "TerrainConstants.h"
#include "../Graphics/RHI/d3dx12.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "InfiniteChunkManager.h"
#include <spdlog/spdlog.h>
#include <array>
#include <filesystem>
#include <unordered_set>

namespace VENPOD::Simulation {

Result<void> VoxelWorld::Initialize(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    Graphics::DescriptorHeapManager& heapManager,
    const VoxelWorldConfig& config)
{
    if (!device) {
        return Error("Device is null");
    }

    m_config = config;
    m_heapManager = &heapManager;

    // Create ping-pong voxel buffers
    auto result = CreateVoxelBuffers(device, heapManager);
    if (!result) {
        return Error("Failed to create voxel buffers: {}", result.error());
    }

    // Create material palette texture
    result = CreateMaterialPalette(device, cmdList, heapManager);
    if (!result) {
        return Error("Failed to create material palette: {}", result.error());
    }

    // Create brush raycast result buffer (16 bytes GPU + 16 bytes CPU readback)
    result = m_brushRaycastResult.Initialize(
        device,
        16,  // 4 floats = 16 bytes (posX, posY, posZ, normalPacked)
        Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
        16,  // stride = 16 bytes (entire structure)
        "BrushRaycastResult"
    );
    if (!result) {
        return Error("Failed to create brush raycast result buffer: {}", result.error());
    }

    // Create UAV for brush raycast result
    result = m_brushRaycastResult.CreateUAV(device, heapManager);
    if (!result) {
        return Error("Failed to create UAV for brush raycast result: {}", result.error());
    }

    // Initialize CPU-side result to invalid
    m_brushRaycastCPU.posX = 0.0f;
    m_brushRaycastCPU.posY = 0.0f;
    m_brushRaycastCPU.posZ = 0.0f;
    m_brushRaycastCPU.normalPacked = 0;
    m_brushRaycastCPU.hasValidPosition = false;

    uint64_t totalMemoryMB = (GetTotalVoxels() * sizeof(uint32_t) * 2) / (1024 * 1024);
    spdlog::info("VoxelWorld initialized: {}x{}x{} grid ({} MB)",
        m_config.gridSizeX, m_config.gridSizeY, m_config.gridSizeZ, totalMemoryMB);
    spdlog::info("GPU brush raycasting enabled (16 bytes readback vs 32 MB!)");

    // ===== INITIALIZE INFINITE CHUNK SYSTEM =====
    if (m_useInfiniteChunks) {
        m_chunkManager = std::make_unique<InfiniteChunkManager>();

        InfiniteChunkConfig chunkConfig;
        chunkConfig.worldSeed = 12345;  // TODO: Make configurable via VoxelWorldConfig
        chunkConfig.chunksPerFrame = 20;  // RTX 3070 Ti (8GB) - generate 20 chunks/frame for instant world loading!

        // MAXED OUT: RTX 3070 Ti (8GB VRAM) Configuration
        // Terrain exists at Y=5-60 (chunks Y=0,1 only), so vertical range is FIXED
        // Render distance: 12 chunks horizontal = ±12 chunks = 25×25 grid = 1600 voxels = ~1.6km view distance!
        chunkConfig.renderDistanceHorizontal = 12;  // ±12 chunks = 25×25×2 = 1,250 chunks
        chunkConfig.renderDistanceVertical = 2;     // UNUSED - we use fixed Y=0,1 layers
        // With 25×25×2 = 1,250 chunks × 1 MB = 1.25 GB VRAM (well within 8GB budget!)

        // Unload distance: larger hysteresis prevents reload thrashing at boundaries
        // CRITICAL FIX: Must be > renderDistance + camera movement per frame
        // renderDistance=12 → 25×25 grid, so unload at 20+ to prevent flickering holes
        chunkConfig.unloadDistanceHorizontal = 20;  // Unload at 20 chunks (8 chunk safety buffer)
        chunkConfig.unloadDistanceVertical = 3;     // UNUSED - vertical is fixed to Y=0,1

        // Faster chunk generation for smooth exploration
        chunkConfig.maxQueuedChunks = 256;          // Queue up to 256 chunks (enough for 25×25×2 / 2)

        auto chunkResult = m_chunkManager->Initialize(device, heapManager, chunkConfig);
        if (!chunkResult) {
            return Error("Failed to initialize infinite chunk manager: {}", chunkResult.error());
        }

        // CACHE FIX: Set callback to notify VoxelWorld when chunks are unloaded
        m_chunkManager->SetUnloadCallback([this](const ChunkCoord& coord) {
            this->OnChunkUnloaded(coord);
        });

        // Create chunk copy pipeline for UpdateActiveRegion
        result = CreateChunkCopyPipeline(device);
        if (!result) {
            return Error("Failed to create chunk copy pipeline: {}", result.error());
        }

        spdlog::info("Infinite chunk system enabled ({}×{} render distance, seed: {})",
            chunkConfig.renderDistanceHorizontal,
            chunkConfig.renderDistanceVertical,
            chunkConfig.worldSeed);

        // Initialize active region center to invalid coordinates (will update on first frame)
        m_activeRegionCenter = ChunkCoord{INT32_MAX, INT32_MAX, INT32_MAX};
        m_activeRegionNeedsUpdate = true;
    }

    return {};
}

void VoxelWorld::Shutdown() {
    m_voxelBuffers[0].Shutdown();
    m_voxelBuffers[1].Shutdown();

    // ===== SHUTDOWN INFINITE CHUNK SYSTEM (NEW) =====
    if (m_chunkManager) {
        m_chunkManager->Shutdown();
        m_chunkManager.reset();
    }

    // Cleanup chunk copy pipeline
    if (m_chunkCopyConstantBuffer) {
        if (m_chunkCopyConstantBufferMappedPtr) {
            m_chunkCopyConstantBuffer->Unmap(0, nullptr);
            m_chunkCopyConstantBufferMappedPtr = nullptr;
        }
        m_chunkCopyConstantBuffer.Reset();
    }
    m_chunkCopyPSO.Reset();
    m_chunkCopyRootSignature.Reset();
    m_chunkCopyCmdList.Reset();

    // Cleanup ring buffer allocators and fence
    for (uint32_t i = 0; i < NUM_COPY_BUFFERS; ++i) {
        m_chunkCopyCmdAllocators[i].Reset();
    }
    if (m_chunkCopyFenceEvent) {
        CloseHandle(m_chunkCopyFenceEvent);
        m_chunkCopyFenceEvent = nullptr;
    }
    m_chunkCopyFence.Reset();

    // Free shader-visible descriptors for voxel buffers
    if (m_heapManager) {
        for (int i = 0; i < 2; i++) {
            if (m_shaderVisibleSRVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleSRVs[i]);
            }
            if (m_shaderVisibleUAVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleUAVs[i]);
            }
        }

        if (m_paletteSRV.IsValid()) {
            m_heapManager->FreeStagingCbvSrvUav(m_paletteSRV);
        }

        if (m_paletteShaderVisibleSRV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_paletteShaderVisibleSRV);
        }
    }

    // Cleanup brush raycast buffers
    m_brushRaycastResult.Shutdown();
    m_brushRaycastReadback.Reset();

    m_materialPalette.Reset();
    m_paletteUpload.Reset();
    m_heapManager = nullptr;
}

void VoxelWorld::SwapBuffers() {
    // CRITICAL FIX: Wait for pending chunk copy operations to complete on GPU
    // This prevents race condition where we swap buffers before GPU finishes copying chunks
    // Symptom: Flashes, holes, incomplete terrain when moving
    // Cause: Renderer reads from buffer while GPU is still writing to it
    if (m_chunkCopyFence && m_chunkCopyFenceValue > 0) {
        uint64_t completedValue = m_chunkCopyFence->GetCompletedValue();

        // Only wait if GPU hasn't caught up yet
        if (completedValue < m_chunkCopyFenceValue) {
            // Set event to be signaled when GPU reaches our fence value
            HRESULT hr = m_chunkCopyFence->SetEventOnCompletion(m_chunkCopyFenceValue, m_chunkCopyFenceEvent);
            if (SUCCEEDED(hr)) {
                // Wait with 100ms timeout to prevent infinite hang
                DWORD waitResult = WaitForSingleObject(m_chunkCopyFenceEvent, 100);
                if (waitResult == WAIT_TIMEOUT) {
                    spdlog::warn("SwapBuffers: GPU fence wait timeout! (completed={}, expected={})",
                        completedValue, m_chunkCopyFenceValue);
                }
            }
        }
    }

    int oldReadIndex = m_readBufferIndex;
    m_readBufferIndex = 1 - m_readBufferIndex;
    int newReadIndex = m_readBufferIndex;

    // DIAGNOSTIC: Log swap to verify it's actually happening
    static int swapCount = 0;
    if (++swapCount % 60 == 1) {  // Log once per second
        spdlog::info("SwapBuffers: {} → {} (swap #{})", oldReadIndex, newReadIndex, swapCount);
    }

    // CONVERGENT UPDATE FIX: DO NOT clear the cache!
    //
    // OLD BAD APPROACH: Clear cache → re-upload 1000+ chunks every frame → 66 GB/s flood
    // NEW CONVERGENT APPROACH: Keep cache → only upload missing chunks
    //
    // How it works:
    // Frame 1: Write to Buffer 0, cache[0] empty → upload 500 chunks
    // Frame 2: Write to Buffer 1, cache[1] empty → upload 500 chunks
    // Frame 3: Write to Buffer 0, cache[0] full → upload 0 chunks (converged!)
    // Frame 4: Write to Buffer 1, cache[1] full → upload 0 chunks (converged!)
    //
    // After 2 frames, both buffers have identical chunk sets and uploads drop to near-zero.
    // Cache invalidation happens only when:
    // 1. Camera moves (chunks outside render distance removed in UpdateChunks)
    // 2. Chunks unloaded (OnChunkUnloaded)
    // 3. Chunks painted (InvalidateCopiedChunk)
    //
    // DO NOT CLEAR: int newWriteIndex = 1 - m_readBufferIndex;
    // DO NOT CLEAR: m_copiedChunksPerBuffer[newWriteIndex].clear();
}

void VoxelWorld::TransitionReadBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state) {
    m_voxelBuffers[m_readBufferIndex].TransitionTo(cmdList, state);
}

void VoxelWorld::TransitionWriteBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state) {
    m_voxelBuffers[1 - m_readBufferIndex].TransitionTo(cmdList, state);
}

Result<void> VoxelWorld::CreateVoxelBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager) {
    uint64_t bufferSize = static_cast<uint64_t>(GetTotalVoxels()) * sizeof(uint32_t);

    // Create both ping-pong buffers with UAV support
    for (int i = 0; i < 2; i++) {
        auto result = m_voxelBuffers[i].Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
            sizeof(uint32_t),  // stride
            i == 0 ? "VoxelBuffer_A" : "VoxelBuffer_B"
        );

        if (!result) {
            return Error("Failed to create voxel buffer {}: {}", i, result.error());
        }

        // Create SRV for reading (staging heap)
        result = m_voxelBuffers[i].CreateSRV(device, heapManager);
        if (!result) {
            return Error("Failed to create SRV for buffer {}: {}", i, result.error());
        }

        // Create UAV for writing (staging heap)
        result = m_voxelBuffers[i].CreateUAV(device, heapManager);
        if (!result) {
            return Error("Failed to create UAV for buffer {}: {}", i, result.error());
        }

        // Allocate persistent shader-visible SRV
        m_shaderVisibleSRVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_shaderVisibleSRVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible SRV for voxel buffer {}", i);
        }

        // Copy the SRV descriptor from staging to shader-visible heap
        device->CopyDescriptorsSimple(1,
            m_shaderVisibleSRVs[i].cpu,
            m_voxelBuffers[i].GetStagingSRV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Allocate persistent shader-visible UAV
        m_shaderVisibleUAVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_shaderVisibleUAVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible UAV for voxel buffer {}", i);
        }

        // Copy the UAV descriptor from staging to shader-visible heap
        device->CopyDescriptorsSimple(1,
            m_shaderVisibleUAVs[i].cpu,
            m_voxelBuffers[i].GetStagingUAV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    spdlog::debug("Created persistent shader-visible descriptors for voxel buffers");
    return {};
}

Result<void> VoxelWorld::CreateMaterialPalette(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    Graphics::DescriptorHeapManager& heapManager)
{
    // Default material palette colors (256 entries)
    // Format: RGBA float4
    std::array<float, 256 * 4> paletteData = {};

    // Define some default materials
    auto setMaterial = [&](int id, float r, float g, float b, float a = 1.0f) {
        paletteData[id * 4 + 0] = r;
        paletteData[id * 4 + 1] = g;
        paletteData[id * 4 + 2] = b;
        paletteData[id * 4 + 3] = a;
    };

    // MAT_AIR (0) - Transparent
    setMaterial(0, 0.0f, 0.0f, 0.0f, 0.0f);

    // MAT_SAND (1) - Sandy beige
    setMaterial(1, 0.76f, 0.70f, 0.50f);

    // MAT_WATER (2) - Blue transparent
    setMaterial(2, 0.2f, 0.4f, 0.8f, 0.7f);

    // MAT_STONE (3) - Gray
    setMaterial(3, 0.5f, 0.5f, 0.5f);

    // MAT_DIRT (4) - Brown
    setMaterial(4, 0.55f, 0.35f, 0.2f);

    // MAT_WOOD (5) - Wood brown
    setMaterial(5, 0.6f, 0.4f, 0.2f);

    // MAT_FIRE (6) - Orange/yellow
    setMaterial(6, 1.0f, 0.6f, 0.1f);

    // MAT_LAVA (7) - Red/orange
    setMaterial(7, 1.0f, 0.3f, 0.0f);

    // MAT_ICE (8) - Light blue
    setMaterial(8, 0.7f, 0.85f, 0.95f, 0.8f);

    // MAT_OIL (9) - Dark purple/black
    setMaterial(9, 0.15f, 0.1f, 0.2f, 0.9f);

    // MAT_GLASS (10) - Transparent white
    setMaterial(10, 0.9f, 0.95f, 1.0f, 0.3f);

    // MAT_SMOKE (11) - Gray semi-transparent
    setMaterial(11, 0.3f, 0.3f, 0.35f, 0.4f);

    // MAT_ACID (12) - Toxic green semi-transparent
    setMaterial(12, 0.2f, 0.9f, 0.2f, 0.6f);

    // MAT_HONEY (13) - Golden amber
    setMaterial(13, 0.95f, 0.75f, 0.2f, 0.8f);

    // MAT_CONCRETE (14) - Gray (starts flowing, hardens to stone-like)
    setMaterial(14, 0.6f, 0.6f, 0.65f, 1.0f);

    // MAT_GUNPOWDER (15) - Dark gray/black powder
    setMaterial(15, 0.2f, 0.2f, 0.25f, 1.0f);

    // MAT_CRYSTAL (16) - Purple crystalline
    setMaterial(16, 0.7f, 0.3f, 0.9f, 0.7f);

    // MAT_STEAM (17) - White/light gray transparent
    setMaterial(17, 0.9f, 0.95f, 1.0f, 0.3f);

    // MAT_BEDROCK (255) - Dark gray
    setMaterial(255, 0.2f, 0.2f, 0.2f);

    // Create 1D texture for palette
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    texDesc.Width = 256;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_materialPalette)
    );

    if (FAILED(hr)) {
        return Error("Failed to create material palette texture: 0x{:08X}", hr);
    }
    m_materialPalette->SetName(L"MaterialPalette");

    // Create upload buffer
    uint64_t uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_paletteUpload)
    );

    if (FAILED(hr)) {
        return Error("Failed to create palette upload buffer: 0x{:08X}", hr);
    }

    // Copy data to upload buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSize;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, nullptr);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 0};
    hr = m_paletteUpload->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return Error("Failed to map palette upload buffer: 0x{:08X}", hr);
    }

    memcpy(mappedData, paletteData.data(), 256 * 4 * sizeof(float));
    m_paletteUpload->Unmap(0, nullptr);

    // Copy from upload to texture
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_paletteUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_materialPalette.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition to shader resource state
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_materialPalette.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    cmdList->ResourceBarrier(1, &barrier);

    // Create SRV in staging heap
    m_paletteSRV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_paletteSRV.IsValid()) {
        return Error("Failed to allocate palette SRV descriptor");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture1D.MostDetailedMip = 0;
    srvDesc.Texture1D.MipLevels = 1;

    device->CreateShaderResourceView(m_materialPalette.Get(), &srvDesc, m_paletteSRV.cpu);

    // Allocate shader-visible SRV for palette
    m_paletteShaderVisibleSRV = heapManager.AllocateShaderVisibleCbvSrvUav();
    if (!m_paletteShaderVisibleSRV.IsValid()) {
        return Error("Failed to allocate shader-visible SRV for material palette");
    }

    // Copy descriptor to shader-visible heap
    device->CopyDescriptorsSimple(1,
        m_paletteShaderVisibleSRV.cpu,
        m_paletteSRV.cpu,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    spdlog::debug("Material palette created with 256 colors (shader-visible descriptor allocated)");
    return {};
}

void VoxelWorld::RequestBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;

    ID3D12Device* device = nullptr;
    m_brushRaycastResult.GetResource()->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    // Create tiny 16-byte readback buffer if it doesn't exist
    if (!m_brushRaycastReadback) {
        constexpr uint64_t bufferSize = 16;  // 4 floats = 16 bytes (vs 32 MB!)

        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_brushRaycastReadback)
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create brush raycast readback buffer: 0x{:08X}", hr);
            device->Release();
            return;
        }

        m_brushRaycastReadback->SetName(L"BrushRaycastReadback");
        spdlog::debug("Created brush raycast readback buffer (16 bytes - 2,000,000x smaller!)");
    }

    device->Release();

    // Transition brush result buffer to copy source
    D3D12_RESOURCE_STATES currentState = m_brushRaycastResult.GetCurrentState();
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            currentState,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Copy tiny 16-byte GPU buffer to CPU readback buffer
    cmdList->CopyResource(m_brushRaycastReadback.Get(), m_brushRaycastResult.GetResource());

    // Transition back to UAV state for next raycast
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            currentState
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Map and read the result immediately (safe because readback is tiny)
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 16};
    HRESULT hr = m_brushRaycastReadback->Map(0, &readRange, &mappedData);
    if (SUCCEEDED(hr)) {
        float* data = static_cast<float*>(mappedData);
        m_brushRaycastCPU.posX = data[0];
        m_brushRaycastCPU.posY = data[1];
        m_brushRaycastCPU.posZ = data[2];

        // Unpack normal and validity flag
        uint32_t packed = *reinterpret_cast<uint32_t*>(&data[3]);
        m_brushRaycastCPU.normalPacked = packed;
        m_brushRaycastCPU.hasValidPosition = (packed >> 6) & 1;

        D3D12_RANGE writeRange = {0, 0};
        m_brushRaycastReadback->Unmap(0, &writeRange);
    }
}

glm::vec3 VoxelWorld::UpdateChunks(
    ID3D12Device* device,
    ID3D12CommandQueue* cmdQueue,  // CHANGED: Pass queue instead of cmdList
    const glm::vec3& cameraPos)
{
    if (!m_chunkManager || !m_useInfiniteChunks) {
        return glm::vec3(0.0f, 0.0f, 0.0f);  // Chunk system disabled, no origin shift
    }

    // Update chunk loading/unloading based on camera position
    m_chunkManager->Update(device, cmdQueue, cameraPos);

    // Calculate which chunk the camera is currently in (HORIZONTAL ONLY)
    // FIX: Terrain is independent of camera Y - always at Y=0-64 chunk range
    ChunkCoord cameraChunk = ChunkCoord::FromWorldPosition(
        static_cast<int32_t>(std::floor(cameraPos.x)),
        0,  // FIX: Always use Y=0 as reference chunk (terrain layers are Y=0,1)
        static_cast<int32_t>(std::floor(cameraPos.z)),
        INFINITE_CHUNK_SIZE
    );

    // Update active region when camera moves OR when new chunks finish generating
    static ChunkCoord lastProcessedChunk = {INT32_MAX, INT32_MAX, INT32_MAX};
    static size_t lastGeneratedCount = 0;

    bool chunkChanged = (cameraChunk != lastProcessedChunk);

    // FIX: Count GENERATED chunks only (not just queued/submitted chunks)
    // GetLoadedChunkCount() includes chunks that are queued but not generated yet,
    // so we need to manually count chunks in Generated or Dirty state
    size_t currentGeneratedCount = 0;
    for (const auto& [coord, chunk] : m_chunkManager->GetLoadedChunks()) {
        if (chunk && chunk->IsGenerated()) {
            currentGeneratedCount++;
        }
    }
    bool newChunksGenerated = (currentGeneratedCount != lastGeneratedCount);

    // ============================================================================
    // CRITICAL FIX: Update activeRegionCenter with HYSTERESIS to prevent constant stuttering!
    //
    // Problem: Updating m_activeRegionCenter every chunk boundary (every 64 voxels) causes:
    // 1. RegionOrigin shifts by 64 voxels
    // 2. Cache invalidation forces re-copying ALL 1,250 chunks
    // 3. For 2-3 frames, buffer has mismatched data/origin → flickering and visual jump
    // 4. This happens every 64 voxels → constant stuttering
    //
    // Solution: Only shift the render buffer when camera gets FAR from center (e.g., 8+ chunks).
    // This reduces buffer shifts from every 64 voxels to every ~512 voxels (8 chunks * 64).
    // ============================================================================
    bool bufferNeedsShift = false;
    if (chunkChanged) {
        // Calculate distance from camera to current buffer center
        int dx = abs(cameraChunk.x - m_activeRegionCenter.x);
        int dz = abs(cameraChunk.z - m_activeRegionCenter.z);

        // Only shift buffer if camera is more than 8 chunks from current center
        // (12 chunk radius - 4 chunk margin = 8 chunk threshold)
        constexpr int SHIFT_THRESHOLD = 8;
        if (dx > SHIFT_THRESHOLD || dz > SHIFT_THRESHOLD) {
            m_activeRegionCenter = cameraChunk;
            bufferNeedsShift = true;
            spdlog::info("Buffer center shifted from chunk [{},{},{}] to [{},{},{}] (distance: dx={}, dz={})",
                m_activeRegionCenter.x - (cameraChunk.x - m_activeRegionCenter.x),
                m_activeRegionCenter.y,
                m_activeRegionCenter.z - (cameraChunk.z - m_activeRegionCenter.z),
                cameraChunk.x, cameraChunk.y, cameraChunk.z,
                dx, dz);
        }
    }

    // ============================================================================
    // CRITICAL FIX: DISABLE ORIGIN SHIFTING COMPLETELY!
    //
    // Setting regionOrigin to (0,0,0) means the shader receives absolute world coordinates.
    // The buffer still contains chunks relative to the camera, but we need the shader to
    // convert world pos → buffer pos using the camera chunk coordinate, not regionOrigin.
    //
    // This prevents ALL teleportation because the coordinate system never resets!
    // ============================================================================
    m_regionOriginWorld = glm::vec3(0.0f, 0.0f, 0.0f);  // ALWAYS zero - no shifting!
    glm::vec3 originShiftDelta = glm::vec3(0.0f, 0.0f, 0.0f);  // No shift ever

    // Log when region origin shifts (only when buffer actually shifts, not every chunk change)
    if (bufferNeedsShift) {
        spdlog::info("Region origin shifted to world voxel ({},{},{}) - Cache invalidated",
            static_cast<int>(m_regionOriginWorld.x),
            static_cast<int>(m_regionOriginWorld.y),
            static_cast<int>(m_regionOriginWorld.z));
    }

    // DIAGNOSTIC: Log camera position and chunk periodically
    static int diagFrameCount = 0;
    static bool firstFrameLogged = false;
    if (++diagFrameCount >= 60 || !firstFrameLogged) {
        // Calculate what the shader will compute
        glm::vec3 bufferPos = cameraPos - m_regionOriginWorld;
        spdlog::info("[CAMERA DIAG] Pos=({:.1f},{:.1f},{:.1f}) Chunk=[{},{},{}] RegionOrigin=({:.0f},{:.0f},{:.0f}) BufferPos=({:.1f},{:.1f},{:.1f})",
            cameraPos.x, cameraPos.y, cameraPos.z,
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            m_regionOriginWorld.x, m_regionOriginWorld.y, m_regionOriginWorld.z,
            bufferPos.x, bufferPos.y, bufferPos.z);
        diagFrameCount = 0;
        firstFrameLogged = true;
    }

    // Update if buffer needs shift OR if new chunks were generated
    if (bufferNeedsShift || newChunksGenerated) {
        // CRITICAL FIX: When buffer center shifts, buffer layout changes!
        // All cached chunk positions become INVALID because destX/Y/Z calculations
        // depend on m_activeRegionCenter. Example:
        //   Frame 1 (camera chunk 0): chunk (5,0,5) → destX = (5-0+12)*64 = 1088
        //   Frame 2 (camera chunk 8): chunk (5,0,5) → destX = (5-8+12)*64 = 576
        // Cache says "chunk already copied" but it's at WRONG offset (1088 not 576)!
        // This causes persistent flickering holes in terrain.
        //
        // NOTE: With hysteresis, this only happens when camera moves 8+ chunks from
        // current buffer center (every ~512 voxels), not every chunk boundary (64 voxels).
        if (bufferNeedsShift) {
            m_copiedChunksPerBuffer[0].clear();
            m_copiedChunksPerBuffer[1].clear();

            // FIX #2: Reset frame counter to trigger aggressive chunk copying
            // After cache invalidation, we need to refill BOTH buffers ASAP to prevent
            // holes and floating chunks. This flag is checked in UpdateActiveRegion to
            // temporarily disable the dual-buffer skip optimization.
            m_framesAfterCacheInvalidation = 0;
        }

        lastProcessedChunk = cameraChunk;
        lastGeneratedCount = currentGeneratedCount;

        // CRITICAL FIX: Poll for completed chunks RIGHT BEFORE UpdateActiveRegion
        // This catches chunks that JUST finished generating in this frame's Update() call
        // Without this, UpdateActiveRegion won't see newly completed chunks until next frame
        m_chunkManager->PollCompletedChunks();

        UpdateActiveRegion(device, cmdQueue, bufferNeedsShift);

        if (bufferNeedsShift) {
            spdlog::debug("Camera at chunk [{},{},{}] - updating active region (buffer shifted)",
                cameraChunk.x, cameraChunk.y, cameraChunk.z);
        }
        // if (newChunksGenerated) {
        //     spdlog::debug("New chunks generated ({} total GENERATED, {} total loaded) - updating active region",
        //         currentGeneratedCount, m_chunkManager->GetLoadedChunkCount());
        // }
    }

    // Log chunk changes for debugging
    static ChunkCoord lastLoggedChunk = {INT32_MAX, INT32_MAX, INT32_MAX};
    if (cameraChunk != lastLoggedChunk) {
        const auto& loadedChunks = m_chunkManager->GetLoadedChunks();
        spdlog::debug("Camera entered chunk [{},{},{}] - {} chunks loaded",
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            loadedChunks.size());
        lastLoggedChunk = cameraChunk;
    }

    // CRITICAL FIX: Return origin shift delta so caller can adjust camera!
    // If regionOrigin shifted by (+64, 0, +64), caller must adjust camera by (-64, 0, -64)
    // to prevent teleportation.
    if (glm::length(originShiftDelta) > 0.01f) {
        spdlog::info("Origin shifted by ({:.1f}, {:.1f}, {:.1f}) - caller must adjust camera by ({:.1f}, {:.1f}, {:.1f})",
            originShiftDelta.x, originShiftDelta.y, originShiftDelta.z,
            -originShiftDelta.x, -originShiftDelta.y, -originShiftDelta.z);
    }
    return originShiftDelta;
}

void VoxelWorld::OnChunkUnloaded(const ChunkCoord& coord) {
    // CACHE FIX: Clear this chunk from BOTH buffer caches when it's unloaded
    // Without this, if the chunk gets reloaded later, UpdateActiveRegion thinks
    // it's already in the render buffer and skips copying it → invisible chunk!
    for (int i = 0; i < 2; ++i) {
        auto it = m_copiedChunksPerBuffer[i].find(coord);
        if (it != m_copiedChunksPerBuffer[i].end()) {
            m_copiedChunksPerBuffer[i].erase(it);
        }
    }
    spdlog::debug("Cleared chunk [{},{},{}] from copy caches on unload",
        coord.x, coord.y, coord.z);
}

void VoxelWorld::InvalidateCopiedChunk(const ChunkCoord& coord) {
    // CRITICAL FIX: Remove chunk from BOTH buffer caches when it's modified
    // This forces UpdateActiveRegion to re-copy the chunk with the painted voxels
    // Both buffers need invalidation because the modification affects both
    for (int i = 0; i < 2; ++i) {
        auto it = m_copiedChunksPerBuffer[i].find(coord);
        if (it != m_copiedChunksPerBuffer[i].end()) {
            m_copiedChunksPerBuffer[i].erase(it);
        }
    }
    // spdlog::debug("Invalidated chunk [{},{},{}] from copy caches (modified)",
    //     coord.x, coord.y, coord.z);
}

Result<void> VoxelWorld::CreateChunkCopyPipeline(ID3D12Device* device) {
    // ===== COMPILE SHADER =====
    Graphics::ShaderCompiler compiler;
    auto initResult = compiler.Initialize();
    if (!initResult) {
        return Error("Failed to initialize shader compiler: {}", initResult.error());
    }

    std::filesystem::path shaderPath = "assets/shaders/Compute/CS_CopyChunkToBuffer.hlsl";
    auto compileResult = compiler.CompileComputeShader(shaderPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile CS_CopyChunkToBuffer.hlsl: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("CS_CopyChunkToBuffer.hlsl compilation failed: {}", compiledShader.errors);
    }

    // ===== CREATE ROOT SIGNATURE =====
    // Root parameter 0: CBV (CopyChunkConstants at b0)
    // Root parameter 1: SRV (ChunkVoxelInput at t0)
    // Root parameter 2: UAV (RenderBufferOutput at u0)

    D3D12_ROOT_PARAMETER1 rootParams[3] = {};

    // Parameter 0: CBV
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: SRV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 2: UAV
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 3;
    rootSigDesc.Desc_1_1.pParameters = rootParams;
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
        return Error("Failed to serialize chunk copy root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_chunkCopyRootSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create chunk copy root signature");
    }

    // ===== CREATE PIPELINE STATE =====
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_chunkCopyRootSignature.Get();
    psoDesc.CS = compiledShader.GetBytecode();
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_chunkCopyPSO));
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy pipeline state");
    }

    m_chunkCopyPSO->SetName(L"CS_CopyChunkToBuffer_PSO");
    m_chunkCopyRootSignature->SetName(L"CS_CopyChunkToBuffer_RootSig");

    // ===== CREATE SHARED CONSTANT BUFFER =====
    // Persistent mapped buffer for copy parameters (32 bytes)
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(32);

    hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_chunkCopyConstantBuffer)
    );

    if (FAILED(hr)) {
        return Error("Failed to create chunk copy constant buffer");
    }

    m_chunkCopyConstantBuffer->SetName(L"ChunkCopyConstantBuffer");

    // Persistent mapping
    D3D12_RANGE readRange = {0, 0};
    hr = m_chunkCopyConstantBuffer->Map(0, &readRange, &m_chunkCopyConstantBufferMappedPtr);
    if (FAILED(hr)) {
        return Error("Failed to map chunk copy constant buffer");
    }

    // ===== RING BUFFER FIX: Create 3 command allocators for chunk copy =====
    for (uint32_t i = 0; i < NUM_COPY_BUFFERS; ++i) {
        hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_chunkCopyCmdAllocators[i])
        );
        if (FAILED(hr)) {
            return Error("Failed to create chunk copy command allocator {}", i);
        }
        m_copyAllocatorFenceValues[i] = 0;
    }

    // Create command list
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_chunkCopyCmdAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_chunkCopyCmdList)
    );
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy command list");
    }

    // Create GPU fence for synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_chunkCopyFence));
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy fence");
    }

    m_chunkCopyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_chunkCopyFenceEvent) {
        return Error("Failed to create chunk copy fence event");
    }

    m_chunkCopyFenceValue = 0;
    m_currentCopyAllocatorIndex = 0;

    // Close command list (ready for Reset() in UpdateActiveRegion)
    m_chunkCopyCmdList->Close();

    spdlog::info("Chunk copy pipeline created successfully (ring buffer with {} allocators)", NUM_COPY_BUFFERS);
    return {};
}

void VoxelWorld::CopyStatic2x2Chunks(ID3D12CommandQueue* cmdQueue) {
    if (!m_chunkManager || !cmdQueue) {
        return;
    }

    // Ensure copy pipeline is available
    if (!m_chunkCopyPSO || !m_chunkCopyRootSignature || !m_chunkCopyConstantBuffer || !m_chunkCopyFence) {
        spdlog::warn("CopyStatic2x2Chunks: chunk copy pipeline not initialized");
        return;
    }

    // ===== STEP 1: Acquire a copy allocator from the ring buffer =====
    uint32_t allocatorIndex = m_currentCopyAllocatorIndex;
    uint64_t allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
    uint32_t triesRemaining = NUM_COPY_BUFFERS;

    while (allocatorFenceValue > 0 &&
           m_chunkCopyFence->GetCompletedValue() < allocatorFenceValue &&
           triesRemaining > 0) {
        allocatorIndex = (allocatorIndex + 1) % NUM_COPY_BUFFERS;
        allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
        --triesRemaining;

        if (triesRemaining == 0) {
            static uint32_t skipFrameCount = 0;
            if (++skipFrameCount % 60 == 1) {  // Log once per second
                spdlog::warn("CopyStatic2x2Chunks: all {} copy allocators busy, skipping copy ({} times)",
                    NUM_COPY_BUFFERS, skipFrameCount);
            }
            return;
        }
    }

    HRESULT hr = m_chunkCopyCmdAllocators[allocatorIndex]->Reset();
    if (FAILED(hr)) {
        spdlog::error("CopyStatic2x2Chunks: failed to reset cmd allocator {} (HRESULT={:#x})",
            allocatorIndex, static_cast<uint32_t>(hr));
        return;
    }

    hr = m_chunkCopyCmdList->Reset(m_chunkCopyCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        spdlog::error("CopyStatic2x2Chunks: failed to reset cmd list (HRESULT={:#x})",
            static_cast<uint32_t>(hr));
        return;
    }

    m_chunkCopyCmdList->SetPipelineState(m_chunkCopyPSO.Get());
    m_chunkCopyCmdList->SetComputeRootSignature(m_chunkCopyRootSignature.Get());

    int32_t chunksCopied = 0;
    bool writeBufferTransitioned = false;

    constexpr int32_t chunkSize = static_cast<int32_t>(INFINITE_CHUNK_SIZE);

    struct StaticEntry {
        ChunkCoord coord;
        int32_t destX;
        int32_t destY;
        int32_t destZ;
    };

    StaticEntry entries[4] = {
        { ChunkCoord{0, 0, 0}, 0,          0, 0          },
        { ChunkCoord{1, 0, 0}, chunkSize,  0, 0          },
        { ChunkCoord{0, 0, 1}, 0,          0, chunkSize  },
        { ChunkCoord{1, 0, 1}, chunkSize,  0, chunkSize  },
    };

    for (const auto& entry : entries) {
        Chunk* chunk = m_chunkManager->GetChunk(entry.coord);
        if (!chunk || !chunk->IsGenerated()) {
            continue;
        }

        // Bounds check against 256x128x256 grid
        if (entry.destX < 0 || entry.destY < 0 || entry.destZ < 0) {
            continue;
        }
        if (entry.destX + chunkSize > static_cast<int32_t>(m_config.gridSizeX) ||
            entry.destY + chunkSize > static_cast<int32_t>(m_config.gridSizeY) ||
            entry.destZ + chunkSize > static_cast<int32_t>(m_config.gridSizeZ)) {
            continue;
        }

        struct CopyChunkConstants {
            uint32_t destOffsetX;
            uint32_t destOffsetY;
            uint32_t destOffsetZ;
            uint32_t chunkSize;
            uint32_t destGridSizeX;
            uint32_t destGridSizeY;
            uint32_t destGridSizeZ;
            uint32_t padding;
        };

        CopyChunkConstants constants;
        constants.destOffsetX = static_cast<uint32_t>(entry.destX);
        constants.destOffsetY = static_cast<uint32_t>(entry.destY);
        constants.destOffsetZ = static_cast<uint32_t>(entry.destZ);
        constants.chunkSize = INFINITE_CHUNK_SIZE;
        constants.destGridSizeX = m_config.gridSizeX;
        constants.destGridSizeY = m_config.gridSizeY;
        constants.destGridSizeZ = m_config.gridSizeZ;
        constants.padding = 0;

        memcpy(m_chunkCopyConstantBufferMappedPtr, &constants, sizeof(CopyChunkConstants));

        if (!writeBufferTransitioned) {
            TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            writeBufferTransitioned = true;
        }

        // Bind constants and resources
        m_chunkCopyCmdList->SetComputeRootConstantBufferView(0, m_chunkCopyConstantBuffer->GetGPUVirtualAddress());

        // Ensure chunk buffer is in SRV state before reading
        chunk->TransitionBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());
        m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetWriteBuffer().GetGPUVirtualAddress());

        // Dispatch 8x8x8 groups for 64^3 chunk
        m_chunkCopyCmdList->Dispatch(8, 8, 8);

        // UAV barrier between copies on WRITE buffer
        D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(GetWriteBuffer().GetResource());
        m_chunkCopyCmdList->ResourceBarrier(1, &uavBarrier);

        ++chunksCopied;
    }

    if (chunksCopied > 0) {
        // Transition WRITE buffer back to SRV for rendering after swap
        TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_chunkCopyCmdList->Close();
        ID3D12CommandList* lists[] = { m_chunkCopyCmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        m_chunkCopyFenceValue++;
        cmdQueue->Signal(m_chunkCopyFence.Get(), m_chunkCopyFenceValue);
        m_copyAllocatorFenceValues[allocatorIndex] = m_chunkCopyFenceValue;

        m_currentCopyAllocatorIndex = (m_currentCopyAllocatorIndex + 1) % NUM_COPY_BUFFERS;

        spdlog::debug("CopyStatic2x2Chunks: Copied {} chunks into WRITE buffer", chunksCopied);
    } else {
        // No chunks copied: clear fence tracking for this allocator and just close list
        m_copyAllocatorFenceValues[allocatorIndex] = 0;
        m_chunkCopyCmdList->Close();
    }
}

void VoxelWorld::UpdateActiveRegion(ID3D12Device* /*device*/, ID3D12CommandQueue* cmdQueue, bool chunkChanged) {
    if (!m_chunkManager || !cmdQueue) {
        return;
    }

    // ===== STEP 1: Determine which chunks to copy into 512×256×512 render buffer =====
    // FIX #18: The buffer represents 8×4×8 = 256 chunks (each 64³) - increased from 4×2×4 = 32
    // We center on the camera's chunk and copy nearby chunks

    const auto& loadedChunks = m_chunkManager->GetLoadedChunks();
    if (loadedChunks.empty()) {
        return;  // No chunks loaded yet
    }

    // Camera is at m_activeRegionCenter chunk coordinate
    // For a 256×128×256 buffer with 64³ chunks, we can fit 4×2×4 chunks
    // Center on camera chunk: copy chunks in range dx,dz ∈ [-1..2], dy ∈ [-1..0]

    // ===== STEP 2: Ring buffer - get next allocator =====
    uint32_t allocatorIndex = m_currentCopyAllocatorIndex;

    // FIX #5: Check if this allocator is still busy
    // IMPROVED: Try other allocators before giving up
    uint64_t allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
    uint32_t triesRemaining = NUM_COPY_BUFFERS;
    while (allocatorFenceValue > 0 && m_chunkCopyFence->GetCompletedValue() < allocatorFenceValue && triesRemaining > 0) {
        // This allocator is busy, try next one in ring buffer
        allocatorIndex = (allocatorIndex + 1) % NUM_COPY_BUFFERS;
        allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
        triesRemaining--;

        if (triesRemaining == 0) {
            // All allocators are busy - skip this frame but log less frequently
            static uint32_t skipFrameCount = 0;
            if (++skipFrameCount % 60 == 1) {  // Log once per second
                spdlog::warn("All {} copy allocators busy, skipping chunk copy ({} times)",
                    NUM_COPY_BUFFERS, skipFrameCount);
            }
            return;  // Skip chunk copy this frame
        }
    }

    // NOW safe to reset this allocator (GPU has finished with it)
    // FIX #7: Check HRESULT - if Reset() fails, skip chunk copy this frame
    HRESULT hr = m_chunkCopyCmdAllocators[allocatorIndex]->Reset();
    if (FAILED(hr)) {
        spdlog::error("Failed to reset chunk copy cmd allocator {} (HRESULT={:#x}), skipping chunk copy",
            allocatorIndex, static_cast<uint32_t>(hr));
        return;  // Skip chunk copy this frame, will retry next frame
    }

    hr = m_chunkCopyCmdList->Reset(m_chunkCopyCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        spdlog::error("Failed to reset chunk copy cmd list (HRESULT={:#x}), skipping chunk copy",
            static_cast<uint32_t>(hr));
        return;  // Skip chunk copy this frame, will retry next frame
    }

    // FIX #6: Copy chunks to WRITE buffer (not READ)!
    // This prevents race condition where chunk copy and physics both write to same buffer
    // Architecture: UpdateChunks() writes NEW chunks → WRITE, then physics simulates on READ → WRITE
    //
    // NOTE: We defer the buffer transition until we know we'll actually copy chunks.
    // If we transition here but then copy 0 chunks, we'd update the CPU state tracking
    // without executing any GPU commands, causing state desync. See "deferred transition" below.

    // Set pipeline and root signature
    m_chunkCopyCmdList->SetPipelineState(m_chunkCopyPSO.Get());
    m_chunkCopyCmdList->SetComputeRootSignature(m_chunkCopyRootSignature.Get());

    // ===== STEP 3: Copy chunks within 9×2×9 grid centered on camera =====
    int32_t chunksCopied = 0;
    int32_t chunksSkipped = 0;
    int32_t chunksNotGenerated = 0;  // DEBUG: Track chunks that exist but aren't generated yet
    bool writeBufferTransitioned = false;  // Deferred transition flag
    bool readBufferTransitioned = false;   // NEW: Track READ buffer transition too

    // FIX: Always copy all chunks immediately (1250 max for 25×25×2 region with RTX 3070 Ti)
    // The old "10 chunks/frame when stationary" was way too conservative and caused:
    // 1. Chunks only appearing in front (loop order meant only first 10 got copied)
    // 2. Terrain not being persistent (moving before buffer fully populated)
    // 3. Random regeneration (different chunks getting priority as you move)
    // At 60 FPS, 1250 chunks in 1 frame is still trivial for RTX 3070 Ti (~1-2ms total)
    int32_t maxChunksPerFrame = 1250;  // Always copy entire region immediately

    // FIX: Copy range for 9×2×9 render buffer (576×128×576 / 64³ = 9×2×9)
    // Horizontal: dx,dz ∈ [-4..4] = 9 chunks × 64 = 576 voxels ✓ (relative to camera)
    // Vertical:   y ∈ [0..1]      = 2 chunks × 64 = 128 voxels ✓ (ABSOLUTE coordinates)
    //             Terrain at Y=5-60 spans chunk Y=0 (0-63) and Y=1 (64-127)

    int32_t chunksNotLoaded = 0;  // DEBUG: Count chunks not in loadedChunks map
    int32_t chunksChecked = 0;    // DEBUG: Total chunks checked
    int32_t chunksOutOfBounds = 0; // DEBUG: Chunks that failed bounds check

    for (int32_t dz = -RENDER_DISTANCE_HORIZONTAL; dz <= RENDER_DISTANCE_HORIZONTAL; ++dz) {
        for (int32_t y = TERRAIN_CHUNK_MIN_Y; y < TERRAIN_CHUNK_MIN_Y + TERRAIN_NUM_CHUNKS_Y; ++y) {
            for (int32_t dx = -RENDER_DISTANCE_HORIZONTAL; dx <= RENDER_DISTANCE_HORIZONTAL; ++dx) {
                chunksChecked++;

                // CRITICAL FIX: Stop if we've copied enough this frame
                if (chunksCopied >= maxChunksPerFrame) {
                    goto done_copying;  // Break out of all loops
                }
                ChunkCoord chunkCoord = {
                    m_activeRegionCenter.x + dx,
                    y,  // FIX: Use absolute Y coordinate (not relative to camera)
                    m_activeRegionCenter.z + dz
                };

                // Check if chunk is loaded
                auto it = loadedChunks.find(chunkCoord);
                if (it == loadedChunks.end()) {
                    chunksNotLoaded++;
                    continue;  // Chunk not loaded at all
                }
                if (!it->second->IsGenerated()) {
                    chunksNotGenerated++;
                    continue;  // Chunk exists but generation not complete yet
                }

                // CRITICAL FIX: Check if chunk is missing from EITHER buffer
                // Ping-pong rendering requires BOTH buffers to have the same chunks!
                // Without this, chunks appear/disappear every frame (checkerboard holes)
                int writeBufferIndex = 1 - m_readBufferIndex;
                int readBufferIndex = m_readBufferIndex;

                bool inWriteBuffer = (m_copiedChunksPerBuffer[writeBufferIndex].find(chunkCoord) != m_copiedChunksPerBuffer[writeBufferIndex].end());
                bool inReadBuffer = (m_copiedChunksPerBuffer[readBufferIndex].find(chunkCoord) != m_copiedChunksPerBuffer[readBufferIndex].end());

                // FIX #2 OPTIMIZATION: During cache refill period (first 3 frames after invalidation),
                // ALWAYS copy chunks to WRITE buffer even if they're in READ buffer.
                // This aggressively refills BOTH buffers to eliminate holes/floating chunks.
                // After 3 frames, revert to normal optimization (skip if in both buffers).
                bool shouldSkip = false;
                if (m_framesAfterCacheInvalidation >= 3) {
                    // Normal mode: skip only if in BOTH buffers
                    shouldSkip = (inWriteBuffer && inReadBuffer);
                } else {
                    // Aggressive refill mode: skip only if already in WRITE buffer
                    // (READ buffer will get synced when we swap at end of frame)
                    shouldSkip = inWriteBuffer;
                }

                if (shouldSkip) {
                    chunksSkipped++;
                    continue;
                }

                // If missing from write buffer (or during refill period), we need to copy

                Chunk* chunk = it->second;

                // Calculate destination offset in render buffer
                // Horizontal: dx ∈ [-RENDER_DISTANCE..+RENDER_DISTANCE] → 25 chunks (12*2+1)
                // Vertical:   y ∈ [TERRAIN_CHUNK_MIN_Y..TERRAIN_CHUNK_MIN_Y+2) → 2 chunks
                int32_t destX = (dx + RENDER_DISTANCE_HORIZONTAL) * INFINITE_CHUNK_SIZE;
                int32_t destY = (y - TERRAIN_CHUNK_MIN_Y) * INFINITE_CHUNK_SIZE;  // Offset to buffer Y=0
                int32_t destZ = (dz + RENDER_DISTANCE_HORIZONTAL) * INFINITE_CHUNK_SIZE;

                // DIAGNOSTIC: Enable to debug chunk copy issues
                static int copyDebugCount = 0;
                if (copyDebugCount < 20) {
                    spdlog::info("[CHUNK_COPY] Chunk coord [{},{},{}] dx={} dz={} → buffer dest [{},{},{}]",
                        chunkCoord.x, chunkCoord.y, chunkCoord.z, dx, dz, destX, destY, destZ);
                    copyDebugCount++;
                }

                // Skip if out of bounds
                if (destX < 0 || destY < 0 || destZ < 0 ||
                    destX + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeX) ||
                    destY + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeY) ||
                    destZ + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeZ)) {
                    chunksOutOfBounds++;
                    continue;
                }

                // ===== STEP 4: Update constant buffer =====
                struct CopyChunkConstants {
                    uint32_t destOffsetX;
                    uint32_t destOffsetY;
                    uint32_t destOffsetZ;
                    uint32_t chunkSize;
                    uint32_t destGridSizeX;
                    uint32_t destGridSizeY;
                    uint32_t destGridSizeZ;
                    uint32_t padding;
                };

                CopyChunkConstants constants;
                constants.destOffsetX = static_cast<uint32_t>(destX);
                constants.destOffsetY = static_cast<uint32_t>(destY);
                constants.destOffsetZ = static_cast<uint32_t>(destZ);
                constants.chunkSize = INFINITE_CHUNK_SIZE;
                constants.destGridSizeX = m_config.gridSizeX;
                constants.destGridSizeY = m_config.gridSizeY;
                constants.destGridSizeZ = m_config.gridSizeZ;
                constants.padding = 0;

                memcpy(m_chunkCopyConstantBufferMappedPtr, &constants, sizeof(CopyChunkConstants));

                // ===== DEFERRED TRANSITION: Only transition WRITE buffer when we have chunks to copy =====
                // This fixes a critical bug where we'd update CPU state tracking but not execute any GPU commands.
                // If we transition but then exit early (0 chunks copied), the CPU thinks the buffer is in UAV
                // but the GPU never saw that transition. This causes invalid state transitions later.
                if (!writeBufferTransitioned) {
                    TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    writeBufferTransitioned = true;
                }

                // ===== STEP 5: Bind resources and dispatch =====
                // Root parameter 0: CBV
                m_chunkCopyCmdList->SetComputeRootConstantBufferView(0, m_chunkCopyConstantBuffer->GetGPUVirtualAddress());

                // CRITICAL FIX: Transition chunk buffer to SRV state before reading!
                // After generation, chunks are left in UAV state. We MUST transition to
                // NON_PIXEL_SHADER_RESOURCE before using as SRV input for copy shader.
                // Failing to do this causes undefined behavior and GPU crashes.
                chunk->TransitionBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                // Root parameter 1: SRV (chunk input)
                m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());

                // CRITICAL FIX: Copy to BOTH buffers if needed (prevents ping-pong holes)
                // We dispatch once per missing buffer to keep chunks synchronized

                if (!inWriteBuffer) {
                    // Root parameter 2: UAV (WRITE buffer output)
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetWriteBuffer().GetGPUVirtualAddress());

                    // Dispatch 8×8×8 thread groups for 64³ chunk
                    m_chunkCopyCmdList->Dispatch(8, 8, 8);

                    // UAV barrier between chunk copies
                    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(GetWriteBuffer().GetResource());
                    m_chunkCopyCmdList->ResourceBarrier(1, &uavBarrier);

                    // Mark as copied in WRITE buffer's cache
                    m_copiedChunksPerBuffer[writeBufferIndex].insert(chunkCoord);
                    chunksCopied++;
                }

                if (!inReadBuffer) {
                    // Also copy to READ buffer to keep them in sync!

                    // Transition READ buffer to UAV state (only once per frame)
                    if (!readBufferTransitioned) {
                        TransitionReadBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        readBufferTransitioned = true;
                    }

                    // Re-bind constants (they're the same - just different target buffer)
                    m_chunkCopyCmdList->SetComputeRootConstantBufferView(0, m_chunkCopyConstantBuffer->GetGPUVirtualAddress());
                    m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());

                    // Root parameter 2: UAV (READ buffer output)
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetReadBuffer().GetGPUVirtualAddress());

                    // Dispatch 8×8×8 thread groups for 64³ chunk
                    m_chunkCopyCmdList->Dispatch(8, 8, 8);

                    // UAV barrier between chunk copies
                    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(GetReadBuffer().GetResource());
                    m_chunkCopyCmdList->ResourceBarrier(1, &uavBarrier);

                    // Mark as copied in READ buffer's cache
                    m_copiedChunksPerBuffer[readBufferIndex].insert(chunkCoord);
                    chunksCopied++;
                }
            }
        }
    }
    done_copying:  // Label for early exit when max chunks reached

    // DEBUG: Log copy statistics
    static int debugFrameCount = 0;
    static int lastCopied = 0;
    debugFrameCount++;

    int writeBufferIndex = 1 - m_readBufferIndex;
    int readBufferIndex = m_readBufferIndex;
    size_t cachedInWriteBuffer = m_copiedChunksPerBuffer[writeBufferIndex].size();
    size_t cachedInReadBuffer = m_copiedChunksPerBuffer[readBufferIndex].size();

    // CRITICAL: Detect if we're copying every frame (bandwidth flood!)
    bool bandwidthFlood = (chunksCopied > 500);  // More than 500 chunks per frame = flood
    bool significantChange = (chunksCopied > 0 && chunksCopied != lastCopied);
    bool starvation = (chunksCopied == 0 && chunksNotGenerated > 0);

    if (bandwidthFlood || significantChange || starvation || debugFrameCount % 60 == 1) {
        spdlog::info("Copy: {} copied, {} skipped | READ[{}]={} WRITE[{}]={} | {}",
            chunksCopied, chunksSkipped,
            readBufferIndex, cachedInReadBuffer, writeBufferIndex, cachedInWriteBuffer,
            bandwidthFlood ? "!!! BANDWIDTH FLOOD !!!" : (chunksCopied == 0 ? "CONVERGED" : "filling"));
        lastCopied = chunksCopied;
    }

    // ===== STEP 6: Close and execute (ONLY if we copied something new) =====
    if (chunksCopied > 0) {
        // CRITICAL FIX: Transition WRITE buffer from UAV to SRV state
        // After SwapBuffers(), this buffer becomes READ buffer for rendering
        // It MUST be in NON_PIXEL_SHADER_RESOURCE state for raymarching shader
        //
        // BUG FIX: Use TransitionWriteBufferTo() instead of manual barrier!
        // Manual barriers don't update GPUBuffer::m_currentState, causing state desync.
        // Next call to TransitionWriteBufferTo(UAV) would think buffer is still in UAV
        // when GPU actually has it in SRV - this causes GPU crashes!
        TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_chunkCopyCmdList->Close();
        ID3D12CommandList* lists[] = { m_chunkCopyCmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        // ===== SIGNAL FENCE: Track when this copy operation completes =====
        m_chunkCopyFenceValue++;
        cmdQueue->Signal(m_chunkCopyFence.Get(), m_chunkCopyFenceValue);
        m_copyAllocatorFenceValues[allocatorIndex] = m_chunkCopyFenceValue;

        // NO BUFFER SWAP here! We write directly to WRITE buffer.
        // Chunks will appear after physics swaps buffers at end of frame.
        // This prevents race condition: UpdateChunks→WRITE, Physics reads READ writes WRITE, then swap.

        // PERFORMANCE FIX: Only log at debug level (info logs cause lag on Windows)
        // spdlog::debug("UpdateActiveRegion: Copied {} NEW chunks ({} skipped, {} not generated) to WRITE buffer",
        //     chunksCopied, chunksSkipped, chunksNotGenerated);

        // CRITICAL FIX: Only advance allocator index AFTER successful execution!
        // This ensures we don't skip allocators when no chunks are copied.
        m_currentCopyAllocatorIndex = (m_currentCopyAllocatorIndex + 1) % NUM_COPY_BUFFERS;
    } else {
        // CRITICAL FIX: If no chunks copied, we didn't execute or signal fence!
        // Clear the fence value for this allocator so next frame can use it.
        // We do NOT advance m_currentCopyAllocatorIndex - same allocator will be reused.
        m_copyAllocatorFenceValues[allocatorIndex] = 0;

        // No new chunks to copy - just close the command list without executing
        m_chunkCopyCmdList->Close();
        if (chunksSkipped > 0 || chunksNotGenerated > 0) {
            spdlog::debug("UpdateActiveRegion: No chunks copied ({} skipped, {} not generated yet)",
                chunksSkipped, chunksNotGenerated);
        }
    }

    // NOTE: We do NOT wait for GPU completion here!
    // The ring buffer (3 allocators) ensures we won't reuse this allocator
    // for at least 2 more UpdateActiveRegion calls. GPU has plenty of time to complete.

    // FIX #2: Increment frame counter for cache refill tracking
    // After 3 frames, aggressive copying mode is disabled (normal dual-buffer optimization resumes)
    if (m_framesAfterCacheInvalidation < 3) {
        m_framesAfterCacheInvalidation++;
        if (m_framesAfterCacheInvalidation == 3) {
            spdlog::info("Cache refill complete - reverting to normal dual-buffer optimization");
        }
    }
}

} // namespace VENPOD::Simulation
