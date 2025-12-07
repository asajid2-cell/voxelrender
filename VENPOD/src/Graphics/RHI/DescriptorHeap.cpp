#include "DescriptorHeap.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace VENPOD::Graphics {

// =============================================================================
// DescriptorHeap Implementation
// =============================================================================

DescriptorHeap::DescriptorHeap(DescriptorHeap&& other) noexcept
    : m_heap(std::move(other.m_heap))
    , m_cpuStart(other.m_cpuStart)
    , m_gpuStart(other.m_gpuStart)
    , m_type(other.m_type)
    , m_descriptorSize(other.m_descriptorSize)
    , m_capacity(other.m_capacity)
    , m_allocatedCount(other.m_allocatedCount)
    , m_shaderVisible(other.m_shaderVisible)
    , m_freeList(std::move(other.m_freeList))
{
    other.m_cpuStart = {};
    other.m_gpuStart = {};
    other.m_descriptorSize = 0;
    other.m_capacity = 0;
    other.m_allocatedCount = 0;
}

DescriptorHeap& DescriptorHeap::operator=(DescriptorHeap&& other) noexcept {
    if (this != &other) {
        m_heap = std::move(other.m_heap);
        m_cpuStart = other.m_cpuStart;
        m_gpuStart = other.m_gpuStart;
        m_type = other.m_type;
        m_descriptorSize = other.m_descriptorSize;
        m_capacity = other.m_capacity;
        m_allocatedCount = other.m_allocatedCount;
        m_shaderVisible = other.m_shaderVisible;
        m_freeList = std::move(other.m_freeList);

        other.m_cpuStart = {};
        other.m_gpuStart = {};
        other.m_descriptorSize = 0;
        other.m_capacity = 0;
        other.m_allocatedCount = 0;
    }
    return *this;
}

Result<void> DescriptorHeap::Initialize(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t numDescriptors,
    bool shaderVisible,
    const char* debugName)
{
    if (!device) {
        return Error("DescriptorHeap::Initialize - device is null");
    }

    if (numDescriptors == 0) {
        return Error("DescriptorHeap::Initialize - numDescriptors must be > 0");
    }

    // Only CBV/SRV/UAV and SAMPLER heaps can be shader-visible
    if (shaderVisible && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        return Error("DescriptorHeap::Initialize - Only CBV_SRV_UAV and SAMPLER heaps can be shader visible");
    }

    m_type = type;
    m_capacity = numDescriptors;
    m_shaderVisible = shaderVisible;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = numDescriptors;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        return Error("DescriptorHeap::Initialize - CreateDescriptorHeap failed: 0x{:08X}", hr);
    }

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) {
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
    }

    // Set debug name
    if (debugName) {
        std::wstring wideName(debugName, debugName + strlen(debugName));
        m_heap->SetName(wideName.c_str());
    }

    // Initialize free list (all indices available)
    m_freeList.reserve(numDescriptors);
    for (uint32_t i = numDescriptors; i > 0; --i) {
        m_freeList.push_back(i - 1);
    }

    m_allocatedCount = 0;

    const char* typeStr = "Unknown";
    switch (type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: typeStr = "CBV_SRV_UAV"; break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: typeStr = "SAMPLER"; break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: typeStr = "RTV"; break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: typeStr = "DSV"; break;
        default: break;
    }

    spdlog::debug("DescriptorHeap created: {} ({} descriptors, {})",
        debugName ? debugName : "unnamed",
        numDescriptors,
        shaderVisible ? "shader-visible" : "CPU-only");

    return {};
}

void DescriptorHeap::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_heap.Reset();
    m_freeList.clear();
    m_cpuStart = {};
    m_gpuStart = {};
    m_capacity = 0;
    m_allocatedCount = 0;
}

DescriptorHandle DescriptorHeap::Allocate() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_freeList.empty()) {
        spdlog::error("DescriptorHeap::Allocate - heap is full ({} descriptors)", m_capacity);
        return {};
    }

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    m_allocatedCount++;

    DescriptorHandle handle;
    handle.heapIndex = index;
    handle.cpu.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpu.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
    }

    return handle;
}

void DescriptorHeap::Free(DescriptorHandle& handle) {
    if (!handle.IsValid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (handle.heapIndex >= m_capacity) {
        spdlog::error("DescriptorHeap::Free - invalid index {}", handle.heapIndex);
        return;
    }

    m_freeList.push_back(handle.heapIndex);
    m_allocatedCount--;
    handle.Invalidate();
}

DescriptorHandle DescriptorHeap::GetHandle(uint32_t index) const {
    if (index >= m_capacity) {
        return {};
    }

    DescriptorHandle handle;
    handle.heapIndex = index;
    handle.cpu.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpu.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
    }

    return handle;
}

// =============================================================================
// DescriptorHeapManager Implementation
// =============================================================================

Result<void> DescriptorHeapManager::Initialize(
    ID3D12Device* device,
    uint32_t cbvSrvUavCount,
    uint32_t rtvCount,
    uint32_t dsvCount)
{
    if (!device) {
        return Error("DescriptorHeapManager::Initialize - device is null");
    }

    m_device = device;

    // Create staging heap (CPU-only, for creating descriptors)
    // This is where we create CBVs/SRVs/UAVs persistently
    auto result = m_stagingCbvSrvUav.Initialize(
        device,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        cbvSrvUavCount,
        false,  // NOT shader-visible
        "StagingCbvSrvUav"
    );
    if (!result) {
        return Error("Failed to create staging CBV_SRV_UAV heap: {}", result.error());
    }

    // Create shader-visible heap (for GPU access)
    // This is where we copy descriptors when they need to be used by shaders
    result = m_shaderVisibleCbvSrvUav.Initialize(
        device,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        cbvSrvUavCount,
        true,   // Shader-visible
        "ShaderVisibleCbvSrvUav"
    );
    if (!result) {
        return Error("Failed to create shader-visible CBV_SRV_UAV heap: {}", result.error());
    }

    // Create RTV heap (always CPU-only)
    result = m_rtvHeap.Initialize(
        device,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        rtvCount,
        false,
        "RtvHeap"
    );
    if (!result) {
        return Error("Failed to create RTV heap: {}", result.error());
    }

    // Create DSV heap (always CPU-only)
    result = m_dsvHeap.Initialize(
        device,
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        dsvCount,
        false,
        "DsvHeap"
    );
    if (!result) {
        return Error("Failed to create DSV heap: {}", result.error());
    }

    spdlog::info("DescriptorHeapManager initialized: CBV/SRV/UAV={}, RTV={}, DSV={}",
        cbvSrvUavCount, rtvCount, dsvCount);

    return {};
}

void DescriptorHeapManager::Shutdown() {
    m_stagingCbvSrvUav.Shutdown();
    m_shaderVisibleCbvSrvUav.Shutdown();
    m_rtvHeap.Shutdown();
    m_dsvHeap.Shutdown();
    m_device = nullptr;
}

DescriptorHandle DescriptorHeapManager::AllocateStagingCbvSrvUav() {
    return m_stagingCbvSrvUav.Allocate();
}

DescriptorHandle DescriptorHeapManager::AllocateShaderVisibleCbvSrvUav() {
    return m_shaderVisibleCbvSrvUav.Allocate();
}

DescriptorHandle DescriptorHeapManager::CopyToShaderVisible(
    ID3D12Device* device,
    const DescriptorHandle& stagingHandle)
{
    if (!stagingHandle.IsValid()) {
        spdlog::error("CopyToShaderVisible - invalid staging handle");
        return {};
    }

    // Allocate in shader-visible heap
    DescriptorHandle shaderVisibleHandle = m_shaderVisibleCbvSrvUav.Allocate();
    if (!shaderVisibleHandle.IsValid()) {
        spdlog::error("CopyToShaderVisible - failed to allocate shader-visible descriptor");
        return {};
    }

    // Copy from staging to shader-visible
    // CRITICAL: This is the safe way to make descriptors usable by shaders
    device->CopyDescriptorsSimple(
        1,
        shaderVisibleHandle.cpu,
        stagingHandle.cpu,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );

    return shaderVisibleHandle;
}

DescriptorHandle DescriptorHeapManager::AllocateRtv() {
    return m_rtvHeap.Allocate();
}

DescriptorHandle DescriptorHeapManager::AllocateDsv() {
    return m_dsvHeap.Allocate();
}

void DescriptorHeapManager::FreeStagingCbvSrvUav(DescriptorHandle& handle) {
    m_stagingCbvSrvUav.Free(handle);
}

void DescriptorHeapManager::FreeShaderVisibleCbvSrvUav(DescriptorHandle& handle) {
    m_shaderVisibleCbvSrvUav.Free(handle);
}

void DescriptorHeapManager::FreeRtv(DescriptorHandle& handle) {
    m_rtvHeap.Free(handle);
}

void DescriptorHeapManager::FreeDsv(DescriptorHandle& handle) {
    m_dsvHeap.Free(handle);
}

} // namespace VENPOD::Graphics
