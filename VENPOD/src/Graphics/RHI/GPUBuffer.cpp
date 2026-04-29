#include "GPUBuffer.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace VENPOD::Graphics {

// =============================================================================
// GPUBuffer Implementation
// =============================================================================

GPUBuffer::~GPUBuffer() {
    Shutdown();
}

GPUBuffer::GPUBuffer(GPUBuffer&& other) noexcept
    : m_resource(std::move(other.m_resource))
    , m_sizeBytes(other.m_sizeBytes)
    , m_stride(other.m_stride)
    , m_usage(other.m_usage)
    , m_currentState(other.m_currentState)
    , m_mappedData(other.m_mappedData)
    , m_stagingSRV(other.m_stagingSRV)
    , m_stagingUAV(other.m_stagingUAV)
    , m_stagingCBV(other.m_stagingCBV)
    , m_shaderVisibleSRV(other.m_shaderVisibleSRV)
    , m_shaderVisibleUAV(other.m_shaderVisibleUAV)
    , m_heapManager(other.m_heapManager)
{
    other.m_sizeBytes = 0;
    other.m_stride = 0;
    other.m_mappedData = nullptr;
    other.m_stagingSRV.Invalidate();
    other.m_stagingUAV.Invalidate();
    other.m_stagingCBV.Invalidate();
    other.m_shaderVisibleSRV.Invalidate();
    other.m_shaderVisibleUAV.Invalidate();
    other.m_heapManager = nullptr;
}

GPUBuffer& GPUBuffer::operator=(GPUBuffer&& other) noexcept {
    if (this != &other) {
        Shutdown();

        m_resource = std::move(other.m_resource);
        m_sizeBytes = other.m_sizeBytes;
        m_stride = other.m_stride;
        m_usage = other.m_usage;
        m_currentState = other.m_currentState;
        m_mappedData = other.m_mappedData;
        m_stagingSRV = other.m_stagingSRV;
        m_stagingUAV = other.m_stagingUAV;
        m_stagingCBV = other.m_stagingCBV;
        m_shaderVisibleSRV = other.m_shaderVisibleSRV;
        m_shaderVisibleUAV = other.m_shaderVisibleUAV;
        m_heapManager = other.m_heapManager;

        other.m_sizeBytes = 0;
        other.m_stride = 0;
        other.m_mappedData = nullptr;
        other.m_stagingSRV.Invalidate();
        other.m_stagingUAV.Invalidate();
        other.m_stagingCBV.Invalidate();
        other.m_shaderVisibleSRV.Invalidate();
        other.m_shaderVisibleUAV.Invalidate();
        other.m_heapManager = nullptr;
    }
    return *this;
}

Result<void> GPUBuffer::Initialize(
    ID3D12Device* device,
    uint64_t sizeBytes,
    BufferUsage usage,
    uint32_t stride,
    const char* debugName)
{
    if (!device) {
        return Error("GPUBuffer::Initialize - device is null");
    }

    if (sizeBytes == 0) {
        return Error("GPUBuffer::Initialize - sizeBytes must be > 0");
    }

    m_sizeBytes = sizeBytes;
    m_stride = stride;
    m_usage = usage;

    // Determine heap type and initial state
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;

    if (HasFlag(usage, BufferUsage::Upload)) {
        heapType = D3D12_HEAP_TYPE_UPLOAD;
        m_currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (HasFlag(usage, BufferUsage::Readback)) {
        heapType = D3D12_HEAP_TYPE_READBACK;
        m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    } else {
        heapType = D3D12_HEAP_TYPE_DEFAULT;
        m_currentState = D3D12_RESOURCE_STATE_COMMON;
    }

    // Add UAV flag if needed
    if (HasFlag(usage, BufferUsage::UnorderedAccess)) {
        resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    CD3DX12_HEAP_PROPERTIES heapProps(heapType);
    CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes, resourceFlags);

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        m_currentState,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        return Error("GPUBuffer::Initialize - CreateCommittedResource failed: 0x{:08X}", hr);
    }

    // Set debug name
    if (debugName) {
        std::wstring wideName(debugName, debugName + strlen(debugName));
        m_resource->SetName(wideName.c_str());
    }

    // spdlog::debug("GPUBuffer created: {} ({} bytes, stride={})",
    //     debugName ? debugName : "unnamed", sizeBytes, stride);

    return {};
}

Result<void> GPUBuffer::InitializeWithData(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* data,
    uint64_t sizeBytes,
    BufferUsage usage,
    uint32_t stride,
    const char* debugName)
{
    // First create the buffer
    auto result = Initialize(device, sizeBytes, usage, stride, debugName);
    if (!result) {
        return result;
    }

    // For upload buffers, we can write directly
    if (HasFlag(usage, BufferUsage::Upload)) {
        Upload(data, sizeBytes);
        return {};
    }

    // For default heap buffers, we need an upload buffer
    // This is a simplified version - in production, use a proper staging buffer manager
    ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);

    HRESULT hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)
    );

    if (FAILED(hr)) {
        return Error("GPUBuffer::InitializeWithData - Failed to create upload buffer: 0x{:08X}", hr);
    }

    // Copy data to upload buffer
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = uploadBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return Error("GPUBuffer::InitializeWithData - Failed to map upload buffer: 0x{:08X}", hr);
    }

    memcpy(mappedData, data, sizeBytes);
    uploadBuffer->Unmap(0, nullptr);

    // Transition to copy dest
    if (m_currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_resource.Get(),
            m_currentState,
            D3D12_RESOURCE_STATE_COPY_DEST
        );
        cmdList->ResourceBarrier(1, &barrier);
        m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    // Copy from upload to default buffer
    cmdList->CopyResource(m_resource.Get(), uploadBuffer.Get());

    // Note: The upload buffer will be released when this function returns.
    // The command list must be executed before the upload buffer is released.
    // In a real engine, you'd use a staging buffer manager that keeps upload buffers alive.

    return {};
}

void GPUBuffer::Shutdown() {
    // CRITICAL: Descriptors should be freed after GPU is done using them
    // The heap manager free functions handle cleanup
    if (m_heapManager) {
        if (m_stagingSRV.IsValid()) {
            m_heapManager->FreeStagingCbvSrvUav(m_stagingSRV);
        }
        if (m_stagingUAV.IsValid()) {
            m_heapManager->FreeStagingCbvSrvUav(m_stagingUAV);
        }
        if (m_stagingCBV.IsValid()) {
            m_heapManager->FreeStagingCbvSrvUav(m_stagingCBV);
        }
        if (m_shaderVisibleSRV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleSRV);
        }
        if (m_shaderVisibleUAV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleUAV);
        }
    }

    if (m_mappedData && m_resource) {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }

    m_resource.Reset();
    m_sizeBytes = 0;
    m_stride = 0;
    m_heapManager = nullptr;
}

void* GPUBuffer::Map() {
    if (!m_resource) {
        return nullptr;
    }

    if (m_mappedData) {
        return m_mappedData;  // Already mapped
    }

    // Only upload and readback buffers can be mapped
    if (!HasFlag(m_usage, BufferUsage::Upload) && !HasFlag(m_usage, BufferUsage::Readback)) {
        spdlog::error("GPUBuffer::Map - Cannot map default heap buffer");
        return nullptr;
    }

    D3D12_RANGE readRange = { 0, 0 };
    if (HasFlag(m_usage, BufferUsage::Readback)) {
        readRange.End = m_sizeBytes;
    }

    HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr)) {
        spdlog::error("GPUBuffer::Map failed: 0x{:08X}", hr);
        return nullptr;
    }

    return m_mappedData;
}

void GPUBuffer::Unmap() {
    if (!m_mappedData || !m_resource) {
        return;
    }

    D3D12_RANGE writtenRange = { 0, 0 };
    if (HasFlag(m_usage, BufferUsage::Upload)) {
        writtenRange.End = m_sizeBytes;
    }

    m_resource->Unmap(0, &writtenRange);
    m_mappedData = nullptr;
}

void GPUBuffer::Upload(const void* data, uint64_t sizeBytes, uint64_t destOffset) {
    if (!data || sizeBytes == 0) {
        return;
    }

    if (destOffset + sizeBytes > m_sizeBytes) {
        spdlog::error("GPUBuffer::Upload - Data exceeds buffer size");
        return;
    }

    void* mapped = Map();
    if (mapped) {
        memcpy(static_cast<uint8_t*>(mapped) + destOffset, data, sizeBytes);
        // Keep mapped for upload buffers (common pattern)
        if (!HasFlag(m_usage, BufferUsage::Upload)) {
            Unmap();
        }
    }
}

Result<void> GPUBuffer::CreateSRV(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    if (!device || !m_resource) {
        return Error("GPUBuffer::CreateSRV - Invalid device or resource");
    }

    m_heapManager = &heapManager;

    // Allocate in staging heap
    m_stagingSRV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_stagingSRV.IsValid()) {
        return Error("GPUBuffer::CreateSRV - Failed to allocate staging descriptor");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = GetElementCount();
    srvDesc.Buffer.StructureByteStride = m_stride;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_stagingSRV.cpu);

    // Copy to shader-visible heap
    m_shaderVisibleSRV = heapManager.CopyToShaderVisible(device, m_stagingSRV);

    return {};
}

Result<void> GPUBuffer::CreateUAV(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    if (!device || !m_resource) {
        return Error("GPUBuffer::CreateUAV - Invalid device or resource");
    }

    if (!HasFlag(m_usage, BufferUsage::UnorderedAccess)) {
        return Error("GPUBuffer::CreateUAV - Buffer was not created with UnorderedAccess flag");
    }

    m_heapManager = &heapManager;

    // Allocate in staging heap
    m_stagingUAV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_stagingUAV.IsValid()) {
        return Error("GPUBuffer::CreateUAV - Failed to allocate staging descriptor");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = GetElementCount();
    uavDesc.Buffer.StructureByteStride = m_stride;
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_stagingUAV.cpu);

    // Copy to shader-visible heap
    m_shaderVisibleUAV = heapManager.CopyToShaderVisible(device, m_stagingUAV);

    return {};
}

Result<void> GPUBuffer::CreateCBV(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    if (!device || !m_resource) {
        return Error("GPUBuffer::CreateCBV - Invalid device or resource");
    }

    m_heapManager = &heapManager;

    // Allocate in staging heap
    m_stagingCBV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_stagingCBV.IsValid()) {
        return Error("GPUBuffer::CreateCBV - Failed to allocate staging descriptor");
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = static_cast<UINT>((m_sizeBytes + 255) & ~255);  // Align to 256 bytes

    device->CreateConstantBufferView(&cbvDesc, m_stagingCBV.cpu);

    return {};
}

void GPUBuffer::TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState) {
    if (!cmdList || !m_resource || m_currentState == newState) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resource.Get(),
        m_currentState,
        newState
    );
    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = newState;
}

D3D12_GPU_VIRTUAL_ADDRESS GPUBuffer::GetGPUVirtualAddress() const {
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}

// =============================================================================
// UploadBuffer Implementation
// =============================================================================

UploadBuffer::~UploadBuffer() {
    Shutdown();
}

Result<void> UploadBuffer::Initialize(ID3D12Device* device, uint64_t sizeBytes, const char* debugName) {
    if (!device) {
        return Error("UploadBuffer::Initialize - device is null");
    }

    if (sizeBytes == 0) {
        return Error("UploadBuffer::Initialize - sizeBytes must be > 0");
    }

    m_sizeBytes = sizeBytes;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        return Error("UploadBuffer::Initialize - CreateCommittedResource failed: 0x{:08X}", hr);
    }

    // Keep mapped persistently
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr)) {
        return Error("UploadBuffer::Initialize - Map failed: 0x{:08X}", hr);
    }

    if (debugName) {
        std::wstring wideName(debugName, debugName + strlen(debugName));
        m_resource->SetName(wideName.c_str());
    }

    return {};
}

void UploadBuffer::Shutdown() {
    if (m_mappedData && m_resource) {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }
    m_resource.Reset();
    m_sizeBytes = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::GetGPUVirtualAddress() const {
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}

// =============================================================================
// ConstantBuffer Implementation
// =============================================================================

ConstantBuffer::~ConstantBuffer() {
    Shutdown();
}

Result<void> ConstantBuffer::InitializeInternal(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    uint64_t alignedSize,
    const char* debugName)
{
    if (!device) {
        return Error("ConstantBuffer::Initialize - device is null");
    }

    m_sizeBytes = alignedSize;
    m_heapManager = &heapManager;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(alignedSize);

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        return Error("ConstantBuffer::Initialize - CreateCommittedResource failed: 0x{:08X}", hr);
    }

    // Keep mapped persistently for easy updates
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr)) {
        return Error("ConstantBuffer::Initialize - Map failed: 0x{:08X}", hr);
    }

    // Create CBV
    m_stagingCBV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_stagingCBV.IsValid()) {
        return Error("ConstantBuffer::Initialize - Failed to allocate staging descriptor");
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = static_cast<UINT>(alignedSize);

    device->CreateConstantBufferView(&cbvDesc, m_stagingCBV.cpu);

    if (debugName) {
        std::wstring wideName(debugName, debugName + strlen(debugName));
        m_resource->SetName(wideName.c_str());
    }

    return {};
}

void ConstantBuffer::Shutdown() {
    if (m_heapManager && m_stagingCBV.IsValid()) {
        m_heapManager->FreeStagingCbvSrvUav(m_stagingCBV);
    }

    if (m_mappedData && m_resource) {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }

    m_resource.Reset();
    m_sizeBytes = 0;
    m_heapManager = nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS ConstantBuffer::GetGPUVirtualAddress() const {
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}

} // namespace VENPOD::Graphics
