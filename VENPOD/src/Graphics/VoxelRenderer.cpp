#include "VoxelRenderer.h"
#include "RHI/d3dx12.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

Result<void> VoxelRenderer::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    const VoxelRendererConfig& config)
{
    if (!device) {
        return Error("VoxelRenderer::Initialize - device is null");
    }

    m_config = config;
    m_heapManager = &heapManager;

    // NOTE: Voxel buffers removed - VoxelWorld manages them now (saves 32MB+ GPU memory)

    // Create material palette
    auto result = CreateMaterialPalette(device, heapManager);
    if (!result) {
        return Error("Failed to create material palette: {}", result.error());
    }

    // Create frame constants buffer
    result = m_frameConstantBuffer.Initialize<FrameConstants>(device, heapManager, "FrameConstants");
    if (!result) {
        return Error("Failed to create frame constants buffer: {}", result.error());
    }

    uint64_t totalVoxels = GetTotalVoxels();
    uint64_t bufferSizeMB = (totalVoxels * sizeof(uint32_t)) / (1024 * 1024);

    spdlog::info("VoxelRenderer initialized: {}x{}x{} = {} voxels ({} MB per buffer)",
        config.gridSizeX, config.gridSizeY, config.gridSizeZ,
        totalVoxels, bufferSizeMB);

    return {};
}

void VoxelRenderer::Shutdown() {
    // Free palette SRV
    if (m_heapManager && m_paletteSRV.IsValid()) {
        m_heapManager->FreeStagingCbvSrvUav(m_paletteSRV);
    }

    m_frameConstantBuffer.Shutdown();
    m_materialPalette.Reset();
    // NOTE: Voxel buffers removed - VoxelWorld manages them now
    m_heapManager = nullptr;
}

void VoxelRenderer::UpdateFrameConstants(const FrameConstants& constants) {
    m_frameConstantBuffer.Update(constants);
}

void VoxelRenderer::Render(ID3D12GraphicsCommandList* cmdList, DX12GraphicsPipeline& pipeline) {
    if (!cmdList) return;

    // Bind pipeline (sets root signature, PSO, and topology)
    pipeline.Bind(cmdList);

    // Bind constant buffer (b0)
    cmdList->SetGraphicsRootConstantBufferView(0, m_frameConstantBuffer.GetGPUVirtualAddress());

    // NOTE: Voxel buffer binding removed - caller binds VoxelWorld's READ buffer directly
    // See main.cpp: renderer->RenderVoxels() which passes voxelWorld->GetReadBufferSRV()

    // Bind material palette (t1)
    if (m_paletteSRV.IsValid()) {
        // For now, we need to copy the staging SRV to shader-visible
        // This is done during initialization, so we just bind it
        // Note: In a full implementation, we'd have a shader-visible copy
    }

    // Draw fullscreen triangle
    cmdList->DrawInstanced(3, 1, 0, 0);
}

Result<void> VoxelRenderer::CreateMaterialPalette(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    // Create a 1D texture for material colors (256 entries)
    constexpr uint32_t paletteSize = 256;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    texDesc.Width = paletteSize;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_materialPalette)
    );

    if (FAILED(hr)) {
        return Error("Failed to create material palette texture: 0x{:08X}", hr);
    }

    m_materialPalette->SetName(L"MaterialPalette");

    // Create SRV for palette
    m_paletteSRV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_paletteSRV.IsValid()) {
        return Error("Failed to allocate SRV for material palette");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture1D.MostDetailedMip = 0;
    srvDesc.Texture1D.MipLevels = 1;

    device->CreateShaderResourceView(m_materialPalette.Get(), &srvDesc, m_paletteSRV.cpu);

    // TODO: Initialize palette with default material colors via upload buffer
    // For now, the palette will be black (uninitialized)

    spdlog::debug("Created material palette texture ({} entries)", paletteSize);
    return {};
}

} // namespace VENPOD::Graphics
