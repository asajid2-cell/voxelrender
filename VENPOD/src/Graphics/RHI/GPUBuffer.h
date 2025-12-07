#pragma once

// =============================================================================
// VENPOD GPU Buffer - Structured buffer wrapper for DX12
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "DescriptorHeap.h"
#include "d3dx12.h"
#include "../../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Buffer usage flags
enum class BufferUsage : uint32_t {
    Default = 0,                    // GPU read/write
    Upload = 1,                     // CPU -> GPU upload
    Readback = 2,                   // GPU -> CPU readback
    StructuredBuffer = 4,           // Structured buffer (SRV/UAV)
    ConstantBuffer = 8,             // Constant buffer (CBV)
    IndirectArgument = 16,          // Indirect dispatch/draw arguments
    UnorderedAccess = 32,           // UAV access
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool HasFlag(BufferUsage flags, BufferUsage flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

// GPU Buffer wrapper
class GPUBuffer {
public:
    GPUBuffer() = default;
    ~GPUBuffer();

    // Non-copyable
    GPUBuffer(const GPUBuffer&) = delete;
    GPUBuffer& operator=(const GPUBuffer&) = delete;

    // Movable
    GPUBuffer(GPUBuffer&& other) noexcept;
    GPUBuffer& operator=(GPUBuffer&& other) noexcept;

    // Create a buffer
    Result<void> Initialize(
        ID3D12Device* device,
        uint64_t sizeBytes,
        BufferUsage usage,
        uint32_t stride = 0,            // For structured buffers
        const char* debugName = nullptr
    );

    // Create a buffer with initial data
    Result<void> InitializeWithData(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const void* data,
        uint64_t sizeBytes,
        BufferUsage usage,
        uint32_t stride = 0,
        const char* debugName = nullptr
    );

    void Shutdown();

    // Map for CPU access (upload/readback buffers only)
    void* Map();
    void Unmap();

    // Upload data (requires upload buffer or staging)
    void Upload(const void* data, uint64_t sizeBytes, uint64_t destOffset = 0);

    // Create views in descriptor heap
    Result<void> CreateSRV(ID3D12Device* device, DescriptorHeapManager& heapManager);
    Result<void> CreateUAV(ID3D12Device* device, DescriptorHeapManager& heapManager);
    Result<void> CreateCBV(ID3D12Device* device, DescriptorHeapManager& heapManager);

    // Resource barriers
    void TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

    // Accessors
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
    uint64_t GetSize() const { return m_sizeBytes; }
    uint32_t GetStride() const { return m_stride; }
    uint32_t GetElementCount() const { return m_stride > 0 ? static_cast<uint32_t>(m_sizeBytes / m_stride) : 0; }
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    BufferUsage GetUsage() const { return m_usage; }

    // Descriptor handles
    const DescriptorHandle& GetStagingSRV() const { return m_stagingSRV; }
    const DescriptorHandle& GetStagingUAV() const { return m_stagingUAV; }
    const DescriptorHandle& GetStagingCBV() const { return m_stagingCBV; }
    const DescriptorHandle& GetShaderVisibleSRV() const { return m_shaderVisibleSRV; }
    const DescriptorHandle& GetShaderVisibleUAV() const { return m_shaderVisibleUAV; }

    bool HasSRV() const { return m_stagingSRV.IsValid(); }
    bool HasUAV() const { return m_stagingUAV.IsValid(); }
    bool HasCBV() const { return m_stagingCBV.IsValid(); }

private:
    ComPtr<ID3D12Resource> m_resource;
    uint64_t m_sizeBytes = 0;
    uint32_t m_stride = 0;
    BufferUsage m_usage = BufferUsage::Default;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
    void* m_mappedData = nullptr;

    // Staging descriptors (CPU-only, persistent)
    DescriptorHandle m_stagingSRV;
    DescriptorHandle m_stagingUAV;
    DescriptorHandle m_stagingCBV;

    // Shader-visible descriptors (copied from staging when needed)
    DescriptorHandle m_shaderVisibleSRV;
    DescriptorHandle m_shaderVisibleUAV;

    // Track which heap manager owns our descriptors for cleanup
    DescriptorHeapManager* m_heapManager = nullptr;
};

// Upload buffer for CPU -> GPU transfers
class UploadBuffer {
public:
    UploadBuffer() = default;
    ~UploadBuffer();

    Result<void> Initialize(ID3D12Device* device, uint64_t sizeBytes, const char* debugName = nullptr);
    void Shutdown();

    // Get mapped pointer for writing
    void* GetMappedData() const { return m_mappedData; }
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
    uint64_t GetSize() const { return m_sizeBytes; }

private:
    ComPtr<ID3D12Resource> m_resource;
    void* m_mappedData = nullptr;
    uint64_t m_sizeBytes = 0;
};

// Constant buffer with automatic alignment
class ConstantBuffer {
public:
    ConstantBuffer() = default;
    ~ConstantBuffer();

    template<typename T>
    Result<void> Initialize(ID3D12Device* device, DescriptorHeapManager& heapManager, const char* debugName = nullptr) {
        // Constant buffers must be 256-byte aligned
        constexpr uint64_t alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        uint64_t alignedSize = (sizeof(T) + alignment - 1) & ~(alignment - 1);
        return InitializeInternal(device, heapManager, alignedSize, debugName);
    }

    void Shutdown();

    // Update constant buffer data
    template<typename T>
    void Update(const T& data) {
        if (m_mappedData) {
            memcpy(m_mappedData, &data, sizeof(T));
        }
    }

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
    const DescriptorHandle& GetStagingCBV() const { return m_stagingCBV; }

private:
    Result<void> InitializeInternal(ID3D12Device* device, DescriptorHeapManager& heapManager, uint64_t alignedSize, const char* debugName);

    ComPtr<ID3D12Resource> m_resource;
    void* m_mappedData = nullptr;
    uint64_t m_sizeBytes = 0;
    DescriptorHandle m_stagingCBV;
    DescriptorHeapManager* m_heapManager = nullptr;
};

} // namespace VENPOD::Graphics
