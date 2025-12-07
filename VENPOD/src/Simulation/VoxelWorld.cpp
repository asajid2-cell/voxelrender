#include "VoxelWorld.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <array>

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

    return {};
}

void VoxelWorld::Shutdown() {
    m_voxelBuffers[0].Shutdown();
    m_voxelBuffers[1].Shutdown();

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
    m_readBufferIndex = 1 - m_readBufferIndex;
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

} // namespace VENPOD::Simulation
