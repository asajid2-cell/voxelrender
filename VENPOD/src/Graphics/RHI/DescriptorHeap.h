#pragma once

// =============================================================================
// VENPOD Descriptor Heap Manager
// Implements Staging Heap Pattern to avoid COPY_DESCRIPTORS_WRITE_ONLY errors
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <mutex>
#include "../../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Handle to a descriptor in a heap
struct DescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    uint32_t heapIndex = UINT32_MAX;

    bool IsValid() const { return heapIndex != UINT32_MAX; }
    void Invalidate() { heapIndex = UINT32_MAX; cpu = {}; gpu = {}; }
};

// Single descriptor heap wrapper
class DescriptorHeap {
public:
    DescriptorHeap() = default;
    ~DescriptorHeap() = default;

    // Non-copyable
    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    // Movable
    DescriptorHeap(DescriptorHeap&& other) noexcept;
    DescriptorHeap& operator=(DescriptorHeap&& other) noexcept;

    Result<void> Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t numDescriptors,
        bool shaderVisible,
        const char* debugName = nullptr
    );

    void Shutdown();

    // Allocate a single descriptor
    DescriptorHandle Allocate();

    // Free a descriptor (returns it to free list)
    void Free(DescriptorHandle& handle);

    // Get handle at specific index
    DescriptorHandle GetHandle(uint32_t index) const;

    // Accessors
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    uint32_t GetDescriptorSize() const { return m_descriptorSize; }
    uint32_t GetCapacity() const { return m_capacity; }
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }
    bool IsShaderVisible() const { return m_shaderVisible; }
    D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
    D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint32_t m_descriptorSize = 0;
    uint32_t m_capacity = 0;
    uint32_t m_allocatedCount = 0;
    bool m_shaderVisible = false;

    // Simple free list using indices
    std::vector<uint32_t> m_freeList;
    std::mutex m_mutex;
};

// =============================================================================
// Descriptor Heap Manager
// Manages staging (CPU-only) and shader-visible heaps with safe copying
// CRITICAL: Avoids "800 Frame" Time Bomb - COPY_DESCRIPTORS_WRITE_ONLY_DESCRIPTOR
// =============================================================================
class DescriptorHeapManager {
public:
    DescriptorHeapManager() = default;
    ~DescriptorHeapManager() = default;

    // Non-copyable, non-movable
    DescriptorHeapManager(const DescriptorHeapManager&) = delete;
    DescriptorHeapManager& operator=(const DescriptorHeapManager&) = delete;

    Result<void> Initialize(ID3D12Device* device, uint32_t cbvSrvUavCount, uint32_t rtvCount, uint32_t dsvCount);
    void Shutdown();

    // Allocate in STAGING heap (CPU-only, persistent)
    // Use this for CBVs/SRVs/UAVs that need to be copied to shader-visible heap
    DescriptorHandle AllocateStagingCbvSrvUav();

    // Allocate directly in shader-visible heap (for ImGui, etc.)
    // WARNING: Limited capacity, use sparingly
    DescriptorHandle AllocateShaderVisibleCbvSrvUav();

    // Copy from staging to shader-visible heap
    // CRITICAL: This is how you make descriptors usable by shaders
    DescriptorHandle CopyToShaderVisible(ID3D12Device* device, const DescriptorHandle& stagingHandle);

    // Allocate RTV (always CPU-only)
    DescriptorHandle AllocateRtv();

    // Allocate DSV (always CPU-only)
    DescriptorHandle AllocateDsv();

    // Free descriptors
    void FreeStagingCbvSrvUav(DescriptorHandle& handle);
    void FreeShaderVisibleCbvSrvUav(DescriptorHandle& handle);
    void FreeRtv(DescriptorHandle& handle);
    void FreeDsv(DescriptorHandle& handle);

    // Accessors for binding heaps to command lists
    ID3D12DescriptorHeap* GetShaderVisibleCbvSrvUavHeap() const { return m_shaderVisibleCbvSrvUav.GetHeap(); }
    ID3D12DescriptorHeap* GetStagingCbvSrvUavHeap() const { return m_stagingCbvSrvUav.GetHeap(); }
    ID3D12DescriptorHeap* GetRtvHeap() const { return m_rtvHeap.GetHeap(); }
    ID3D12DescriptorHeap* GetDsvHeap() const { return m_dsvHeap.GetHeap(); }

    uint32_t GetCbvSrvUavDescriptorSize() const { return m_shaderVisibleCbvSrvUav.GetDescriptorSize(); }
    uint32_t GetRtvDescriptorSize() const { return m_rtvHeap.GetDescriptorSize(); }
    uint32_t GetDsvDescriptorSize() const { return m_dsvHeap.GetDescriptorSize(); }

    // STRESS TEST SUPPORT: Get allocated descriptor counts
    uint32_t GetShaderVisibleCbvSrvUavAllocatedCount() const { return m_shaderVisibleCbvSrvUav.GetAllocatedCount(); }
    uint32_t GetStagingCbvSrvUavAllocatedCount() const { return m_stagingCbvSrvUav.GetAllocatedCount(); }

private:
    // Staging heap (CPU-only) - persistent descriptors
    DescriptorHeap m_stagingCbvSrvUav;

    // Shader-visible heap - transient descriptors
    DescriptorHeap m_shaderVisibleCbvSrvUav;

    // RTV heap (always CPU-only)
    DescriptorHeap m_rtvHeap;

    // DSV heap (always CPU-only)
    DescriptorHeap m_dsvHeap;

    ID3D12Device* m_device = nullptr;
};

} // namespace VENPOD::Graphics
