#include "ChunkManager.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Simulation {

Result<void> ChunkManager::Initialize(
    ID3D12Device* device,
    Graphics::DescriptorHeapManager& heapManager,
    uint32_t worldSizeX,
    uint32_t worldSizeY,
    uint32_t worldSizeZ)
{
    m_heapManager = &heapManager;

    // Calculate chunk counts (round up)
    m_chunkCountX = (worldSizeX + CHUNK_SIZE - 1) / CHUNK_SIZE;
    m_chunkCountY = (worldSizeY + CHUNK_SIZE - 1) / CHUNK_SIZE;
    m_chunkCountZ = (worldSizeZ + CHUNK_SIZE - 1) / CHUNK_SIZE;

    uint32_t totalChunks = GetTotalChunks();

    spdlog::info("ChunkManager: {}x{}x{} chunks ({} total) for world {}x{}x{}",
        m_chunkCountX, m_chunkCountY, m_chunkCountZ, totalChunks,
        worldSizeX, worldSizeY, worldSizeZ);

    auto result = CreateBuffers(device, heapManager);
    if (!result) {
        return Error("Failed to create chunk buffers: {}", result.error());
    }

    spdlog::info("ChunkManager initialized successfully");
    return {};
}

void ChunkManager::Shutdown() {
    if (m_heapManager) {
        if (m_chunkControlUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_chunkControlUAV);
        }
        if (m_activeListUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_activeListUAV);
        }
        if (m_activeListSRV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_activeListSRV);
        }
        if (m_activeCountUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_activeCountUAV);
        }
        if (m_indirectArgsUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_indirectArgsUAV);
        }
    }

    m_chunkControlBuffer.Shutdown();
    m_activeChunkListBuffer.Shutdown();
    m_activeChunkCountBuffer.Shutdown();
    m_indirectArgsBuffer.Shutdown();
    m_countResetBuffer.Reset();

    m_heapManager = nullptr;
}

Result<void> ChunkManager::CreateBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager) {
    uint32_t totalChunks = GetTotalChunks();

    // 1. Chunk control buffer - stores ChunkControl for each chunk
    {
        uint64_t bufferSize = static_cast<uint64_t>(totalChunks) * sizeof(ChunkControl);
        auto result = m_chunkControlBuffer.Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::UnorderedAccess,
            sizeof(ChunkControl),
            "ChunkControlBuffer"
        );
        if (!result) {
            return Error("Failed to create chunk control buffer: {}", result.error());
        }

        // Create UAV in shader-visible heap
        m_chunkControlUAV = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_chunkControlUAV.IsValid()) {
            return Error("Failed to allocate chunk control UAV");
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = totalChunks;
        uavDesc.Buffer.StructureByteStride = sizeof(ChunkControl);
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            m_chunkControlBuffer.GetResource(),
            nullptr,
            &uavDesc,
            m_chunkControlUAV.cpu
        );
    }

    // 2. Active chunk list buffer - stores indices of active chunks
    {
        uint64_t bufferSize = static_cast<uint64_t>(totalChunks) * sizeof(uint32_t);
        auto result = m_activeChunkListBuffer.Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::UnorderedAccess,
            sizeof(uint32_t),
            "ActiveChunkListBuffer"
        );
        if (!result) {
            return Error("Failed to create active chunk list buffer: {}", result.error());
        }

        m_activeListUAV = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_activeListUAV.IsValid()) {
            return Error("Failed to allocate active list UAV");
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = totalChunks;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            m_activeChunkListBuffer.GetResource(),
            nullptr,
            &uavDesc,
            m_activeListUAV.cpu
        );

        // Also create SRV for reading in physics shader
        m_activeListSRV = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_activeListSRV.IsValid()) {
            return Error("Failed to allocate active list SRV");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_UINT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = totalChunks;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(
            m_activeChunkListBuffer.GetResource(),
            &srvDesc,
            m_activeListSRV.cpu
        );
    }

    // 3. Active chunk count buffer - single uint32 for atomic append
    {
        uint64_t bufferSize = sizeof(uint32_t);
        auto result = m_activeChunkCountBuffer.Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::UnorderedAccess,
            sizeof(uint32_t),
            "ActiveChunkCountBuffer"
        );
        if (!result) {
            return Error("Failed to create active chunk count buffer: {}", result.error());
        }

        m_activeCountUAV = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_activeCountUAV.IsValid()) {
            return Error("Failed to allocate active count UAV");
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = 1;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            m_activeChunkCountBuffer.GetResource(),
            nullptr,
            &uavDesc,
            m_activeCountUAV.cpu
        );
    }

    // 4. Indirect dispatch arguments buffer
    {
        uint64_t bufferSize = sizeof(IndirectDispatchArgs);
        auto result = m_indirectArgsBuffer.Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::UnorderedAccess | Graphics::BufferUsage::IndirectArgument,
            sizeof(uint32_t),
            "IndirectArgsBuffer"
        );
        if (!result) {
            return Error("Failed to create indirect args buffer: {}", result.error());
        }

        m_indirectArgsUAV = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_indirectArgsUAV.IsValid()) {
            return Error("Failed to allocate indirect args UAV");
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = 3;  // threadGroupCountX, Y, Z
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            m_indirectArgsBuffer.GetResource(),
            nullptr,
            &uavDesc,
            m_indirectArgsUAV.cpu
        );
    }

    // 5. Create CPU-side upload buffer for count reset
    {
        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_countResetBuffer)
        );

        if (FAILED(hr)) {
            return Error("Failed to create count reset buffer");
        }

        // Initialize to zero
        uint32_t* pData = nullptr;
        D3D12_RANGE readRange = {0, 0};
        m_countResetBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
        *pData = 0;
        m_countResetBuffer->Unmap(0, nullptr);
    }

    return {};
}

void ChunkManager::TransitionBuffersForCompute(ID3D12GraphicsCommandList* cmdList) {
    D3D12_RESOURCE_BARRIER barriers[4] = {};
    int barrierCount = 0;

    auto addBarrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before != after) {
            barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        }
    };

    // Transition all chunk buffers to UAV state for compute shader access
    addBarrier(m_chunkControlBuffer.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    addBarrier(m_activeChunkListBuffer.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    addBarrier(m_activeChunkCountBuffer.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    addBarrier(m_indirectArgsBuffer.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (barrierCount > 0) {
        cmdList->ResourceBarrier(barrierCount, barriers);
    }
}

void ChunkManager::TransitionBuffersForIndirect(ID3D12GraphicsCommandList* cmdList) {
    // Transition indirect args buffer for indirect dispatch
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_indirectArgsBuffer.GetResource(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
    );
    cmdList->ResourceBarrier(1, &barrier);
}

void ChunkManager::ResetActiveCount(ID3D12GraphicsCommandList* cmdList) {
    // Copy zero from upload buffer to active count buffer
    cmdList->CopyBufferRegion(
        m_activeChunkCountBuffer.GetResource(),
        0,
        m_countResetBuffer.Get(),
        0,
        sizeof(uint32_t)
    );

    // Barrier to ensure copy completes before compute shader reads
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_activeChunkCountBuffer.GetResource(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    cmdList->ResourceBarrier(1, &barrier);
}

void ChunkManager::MarkAllChunksActive(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;

    uint32_t totalChunks = GetTotalChunks();

    // Write total chunk count to active count buffer using Upload
    m_activeChunkCountBuffer.Upload(&totalChunks, sizeof(uint32_t));

    // Fill active chunk list with all chunk indices (0, 1, 2, ...)
    std::vector<uint32_t> allChunkIndices(totalChunks);
    for (uint32_t i = 0; i < totalChunks; ++i) {
        allChunkIndices[i] = i;
    }
    m_activeChunkListBuffer.Upload(allChunkIndices.data(), totalChunks * sizeof(uint32_t));

    // Transition buffers to be ready for indirect dispatch
    TransitionBuffersForIndirect(cmdList);

    spdlog::info("MarkAllChunksActive: All {} chunks marked as active for physics", totalChunks);
}

} // namespace VENPOD::Simulation
