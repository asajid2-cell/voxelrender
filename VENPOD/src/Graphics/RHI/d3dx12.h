// Minimal D3DX12 helper subset
// These are convenience wrappers around D3D12 structures
#pragma once

#include <d3d12.h>

// Helper for D3D12_RANGE
struct CD3DX12_RANGE {
    CD3DX12_RANGE() = default;
    CD3DX12_RANGE(SIZE_T begin, SIZE_T end) {
        Begin = begin;
        End = end;
    }
    D3D12_RANGE ToRange() const { return D3D12_RANGE{Begin, End}; }
    SIZE_T Begin = 0;
    SIZE_T End = 0;
};

// Helper for D3D12_HEAP_PROPERTIES
struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {
    CD3DX12_HEAP_PROPERTIES() = default;
    explicit CD3DX12_HEAP_PROPERTIES(const D3D12_HEAP_PROPERTIES& o) : D3D12_HEAP_PROPERTIES(o) {}
    CD3DX12_HEAP_PROPERTIES(
        D3D12_HEAP_TYPE type,
        UINT creationNodeMask = 1,
        UINT nodeMask = 1,
        D3D12_CPU_PAGE_PROPERTY cpuPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL memoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN)
    {
        Type = type;
        CPUPageProperty = cpuPageProperty;
        MemoryPoolPreference = memoryPoolPreference;
        CreationNodeMask = creationNodeMask;
        VisibleNodeMask = nodeMask;
    }
};

// Helper for D3D12_RESOURCE_DESC
struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC {
    CD3DX12_RESOURCE_DESC() = default;
    explicit CD3DX12_RESOURCE_DESC(const D3D12_RESOURCE_DESC& o) : D3D12_RESOURCE_DESC(o) {}

    static CD3DX12_RESOURCE_DESC Buffer(
        UINT64 width,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        UINT64 alignment = 0)
    {
        CD3DX12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = alignment;
        desc.Width = width;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    }

    static CD3DX12_RESOURCE_DESC Tex2D(
        DXGI_FORMAT format,
        UINT64 width,
        UINT height,
        UINT16 arraySize = 1,
        UINT16 mipLevels = 1,
        UINT sampleCount = 1,
        UINT sampleQuality = 0,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        CD3DX12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = arraySize;
        desc.MipLevels = mipLevels;
        desc.Format = format;
        desc.SampleDesc.Count = sampleCount;
        desc.SampleDesc.Quality = sampleQuality;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        return desc;
    }

    static CD3DX12_RESOURCE_DESC Tex3D(
        DXGI_FORMAT format,
        UINT64 width,
        UINT height,
        UINT16 depth,
        UINT16 mipLevels = 1,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        CD3DX12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = depth;
        desc.MipLevels = mipLevels;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        return desc;
    }
};

// Helper for D3D12_RESOURCE_BARRIER
struct CD3DX12_RESOURCE_BARRIER {
    static D3D12_RESOURCE_BARRIER Transition(
        ID3D12Resource* pResource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE)
    {
        D3D12_RESOURCE_BARRIER result = {};
        result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        result.Flags = flags;
        result.Transition.pResource = pResource;
        result.Transition.StateBefore = stateBefore;
        result.Transition.StateAfter = stateAfter;
        result.Transition.Subresource = subresource;
        return result;
    }

    static D3D12_RESOURCE_BARRIER UAV(ID3D12Resource* pResource) {
        D3D12_RESOURCE_BARRIER result = {};
        result.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        result.UAV.pResource = pResource;
        return result;
    }

    static D3D12_RESOURCE_BARRIER Aliasing(
        ID3D12Resource* pResourceBefore,
        ID3D12Resource* pResourceAfter)
    {
        D3D12_RESOURCE_BARRIER result = {};
        result.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        result.Aliasing.pResourceBefore = pResourceBefore;
        result.Aliasing.pResourceAfter = pResourceAfter;
        return result;
    }
};

// Helper for D3D12_CPU_DESCRIPTOR_HANDLE
struct CD3DX12_CPU_DESCRIPTOR_HANDLE : public D3D12_CPU_DESCRIPTOR_HANDLE {
    CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
    explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE handle) : D3D12_CPU_DESCRIPTOR_HANDLE(handle) {}
    CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE handle, INT offsetInDescriptors, UINT descriptorSize) {
        ptr = handle.ptr + static_cast<SIZE_T>(offsetInDescriptors) * descriptorSize;
    }
    CD3DX12_CPU_DESCRIPTOR_HANDLE& Offset(INT offsetInDescriptors, UINT descriptorSize) {
        ptr += static_cast<SIZE_T>(offsetInDescriptors) * descriptorSize;
        return *this;
    }
};

// Helper for D3D12_GPU_DESCRIPTOR_HANDLE
struct CD3DX12_GPU_DESCRIPTOR_HANDLE : public D3D12_GPU_DESCRIPTOR_HANDLE {
    CD3DX12_GPU_DESCRIPTOR_HANDLE() = default;
    explicit CD3DX12_GPU_DESCRIPTOR_HANDLE(D3D12_GPU_DESCRIPTOR_HANDLE handle) : D3D12_GPU_DESCRIPTOR_HANDLE(handle) {}
    CD3DX12_GPU_DESCRIPTOR_HANDLE(D3D12_GPU_DESCRIPTOR_HANDLE handle, INT offsetInDescriptors, UINT descriptorSize) {
        ptr = handle.ptr + static_cast<UINT64>(offsetInDescriptors) * descriptorSize;
    }
    CD3DX12_GPU_DESCRIPTOR_HANDLE& Offset(INT offsetInDescriptors, UINT descriptorSize) {
        ptr += static_cast<UINT64>(offsetInDescriptors) * descriptorSize;
        return *this;
    }
};
