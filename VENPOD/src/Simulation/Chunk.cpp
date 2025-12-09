#include "Chunk.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <format>

namespace VENPOD::Simulation {

// Constant buffer structure (MUST MATCH CS_GenerateChunk.hlsl!)
struct alignas(16) ChunkConstants {
    int32_t chunkWorldOffsetX;
    int32_t chunkWorldOffsetY;
    int32_t chunkWorldOffsetZ;
    uint32_t worldSeed;
    uint32_t chunkSize;
    uint32_t padding[3];  // Pad to 32 bytes (2×16-byte alignment)
};

Chunk::Chunk(Chunk&& other) noexcept
    : m_coord(other.m_coord)
    , m_state(other.m_state)
    , m_voxelBuffer(std::move(other.m_voxelBuffer))
    , m_voxelSRV(other.m_voxelSRV)
    , m_voxelUAV(other.m_voxelUAV)
    , m_heapManager(other.m_heapManager)
{
    other.m_state = ChunkState::Ungenerated;
    other.m_voxelSRV.Invalidate();
    other.m_voxelUAV.Invalidate();
    other.m_heapManager = nullptr;
}

Chunk& Chunk::operator=(Chunk&& other) noexcept {
    if (this != &other) {
        Shutdown();

        m_coord = other.m_coord;
        m_state = other.m_state;
        m_voxelBuffer = std::move(other.m_voxelBuffer);
        m_voxelSRV = other.m_voxelSRV;
        m_voxelUAV = other.m_voxelUAV;
        m_heapManager = other.m_heapManager;

        other.m_state = ChunkState::Ungenerated;
        other.m_voxelSRV.Invalidate();
        other.m_voxelUAV.Invalidate();
        other.m_heapManager = nullptr;
    }
    return *this;
}

Result<void> Chunk::Initialize(
    ID3D12Device* device,
    Graphics::DescriptorHeapManager& heapManager,
    const ChunkCoord& coord,
    const char* debugNamePrefix)
{
    if (!device) {
        return Error("Chunk::Initialize - device is null");
    }

    m_coord = coord;
    m_heapManager = &heapManager;
    m_state = ChunkState::Ungenerated;

    // Create debug name
    std::string bufferName = std::format("{}[{},{},{}]_VoxelBuffer",
        debugNamePrefix, coord.x, coord.y, coord.z);

    // ===== STEP 1: Create voxel buffer (64³ × 4 bytes = 1,048,576 bytes) =====
    auto result = m_voxelBuffer.Initialize(
        device,
        GetBufferSize(),  // 1 MB
        Graphics::BufferUsage::UnorderedAccess,
        sizeof(uint32_t),  // 4-byte stride (packed voxel)
        bufferName.c_str()
    );

    if (!result) {
        return Error("Failed to create chunk voxel buffer: {}", result.error());
    }

    // ===== STEP 2: Create shader-visible UAV for compute write =====
    m_voxelUAV = heapManager.AllocateShaderVisibleCbvSrvUav();
    if (!m_voxelUAV.IsValid()) {
        return Error("Failed to allocate voxel UAV");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = GetVoxelCount();  // 262,144 voxels
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    device->CreateUnorderedAccessView(
        m_voxelBuffer.GetResource(),
        nullptr,
        &uavDesc,
        m_voxelUAV.cpu
    );

    // ===== STEP 3: Create shader-visible SRV for rendering =====
    m_voxelSRV = heapManager.AllocateShaderVisibleCbvSrvUav();
    if (!m_voxelSRV.IsValid()) {
        return Error("Failed to allocate voxel SRV");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = GetVoxelCount();
    srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(
        m_voxelBuffer.GetResource(),
        &srvDesc,
        m_voxelSRV.cpu
    );

    spdlog::debug("Chunk[{},{},{}] initialized - {} MB",
        coord.x, coord.y, coord.z,
        GetBufferSize() / (1024.0f * 1024.0f));

    return {};
}

void Chunk::Shutdown() {
    if (m_heapManager) {
        if (m_voxelSRV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_voxelSRV);
        }
        if (m_voxelUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_voxelUAV);
        }
    }

    m_voxelBuffer.Shutdown();
    m_state = ChunkState::Ungenerated;
    m_heapManager = nullptr;
}

Result<void> Chunk::Generate(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* generationPSO,
    ID3D12RootSignature* rootSignature,
    uint32_t worldSeed)
{
    if (!device || !cmdList || !generationPSO || !rootSignature) {
        return Error("Chunk::Generate - null parameters");
    }

    if (m_state == ChunkState::Generated || m_state == ChunkState::Generating) {
        return {};  // Already generated, skip
    }

    m_state = ChunkState::Generating;

    // ===== STEP 1: Calculate chunk world offset (coord × 64) =====
    int32_t worldOffsetX, worldOffsetY, worldOffsetZ;
    GetWorldOrigin(worldOffsetX, worldOffsetY, worldOffsetZ);

    // ===== STEP 2: Prepare constant buffer data =====
    ChunkConstants constants;
    constants.chunkWorldOffsetX = worldOffsetX;
    constants.chunkWorldOffsetY = worldOffsetY;
    constants.chunkWorldOffsetZ = worldOffsetZ;
    constants.worldSeed = worldSeed;
    constants.chunkSize = INFINITE_CHUNK_SIZE;
    constants.padding[0] = 0;
    constants.padding[1] = 0;
    constants.padding[2] = 0;

    // ===== STEP 3: Create temporary upload buffer for constants =====
    // DX12 requires constant buffers to be 256-byte aligned
    constexpr uint64_t CB_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;  // 256
    uint64_t alignedSize = (sizeof(ChunkConstants) + CB_ALIGNMENT - 1) & ~(CB_ALIGNMENT - 1);

    // Create upload heap buffer
    D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(alignedSize);

    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer;
    HRESULT hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&constantBuffer)
    );

    if (FAILED(hr)) {
        m_state = ChunkState::Ungenerated;
        return Error("Failed to create constant buffer for chunk generation");
    }

    // Map and upload constants
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 0};
    constantBuffer->Map(0, &readRange, &mappedData);
    memcpy(mappedData, &constants, sizeof(ChunkConstants));
    constantBuffer->Unmap(0, nullptr);

    // ===== STEP 4: Transition voxel buffer to UAV state =====
    D3D12_RESOURCE_BARRIER barrierToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
        m_voxelBuffer.GetResource(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    cmdList->ResourceBarrier(1, &barrierToUAV);

    // ===== STEP 5: Bind compute pipeline and root signature =====
    cmdList->SetPipelineState(generationPSO);
    cmdList->SetComputeRootSignature(rootSignature);

    // ===== STEP 6: Set root parameters =====
    // Root parameter 0: Constant Buffer View (CBV)
    cmdList->SetComputeRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());

    // Root parameter 1: Unordered Access View (UAV) for ChunkVoxelOutput
    cmdList->SetComputeRootUnorderedAccessView(1, m_voxelBuffer.GetGPUVirtualAddress());

    // ===== STEP 7: Dispatch compute shader =====
    // 8×8×8 thread groups (each group has 8×8×8 threads = 512 threads)
    // Total: 8³ groups × 512 threads = 262,144 threads = 64³ voxels ✅
    cmdList->Dispatch(8, 8, 8);

    // ===== STEP 8: UAV barrier (ensure compute completes before next use) =====
    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_voxelBuffer.GetResource());
    cmdList->ResourceBarrier(1, &uavBarrier);

    // ===== STEP 9: Mark as generated =====
    m_state = ChunkState::Generated;

    spdlog::info("Chunk[{},{},{}] generated - world offset ({},{},{})",
        m_coord.x, m_coord.y, m_coord.z,
        worldOffsetX, worldOffsetY, worldOffsetZ);

    return {};
}

} // namespace VENPOD::Simulation
