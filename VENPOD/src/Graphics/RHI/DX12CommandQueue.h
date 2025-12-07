#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include "Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Command Queue wrapper - manages command submission and GPU synchronization
// CRITICAL: Use Flush() or WaitForFenceValue() before destroying any GPU resources
// to avoid OBJECT_DELETED_WHILE_STILL_IN_USE errors!
class DX12CommandQueue {
public:
    DX12CommandQueue() = default;
    ~DX12CommandQueue();

    DX12CommandQueue(const DX12CommandQueue&) = delete;
    DX12CommandQueue& operator=(const DX12CommandQueue&) = delete;
    DX12CommandQueue(DX12CommandQueue&&) = default;
    DX12CommandQueue& operator=(DX12CommandQueue&&) = default;

    // Initialize with a device
    Result<void> Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Cleanup
    void Shutdown();

    // Execute a command list
    void ExecuteCommandList(ID3D12CommandList* commandList);

    // Execute multiple command lists
    void ExecuteCommandLists(ID3D12CommandList* const* commandLists, uint32_t count);

    // Signal the fence from the GPU side - returns fence value to wait on
    uint64_t Signal();

    // Wait for a specific fence value (CPU blocks until GPU reaches this point)
    void WaitForFenceValue(uint64_t fenceValue);

    // Flush all pending GPU work (CPU blocks until GPU is completely idle)
    // IMPORTANT: Call this before resize, shutdown, or destroying any resources!
    void Flush();

    // Check if a fence value has been reached
    [[nodiscard]] bool IsFenceComplete(uint64_t fenceValue) const;

    // Accessors
    [[nodiscard]] ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    [[nodiscard]] ID3D12Fence* GetFence() const { return m_fence.Get(); }
    [[nodiscard]] uint64_t GetLastCompletedFenceValue() const;
    [[nodiscard]] uint64_t GetNextFenceValue() const { return m_nextFenceValue; }

private:
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12Fence> m_fence;

    HANDLE m_fenceEvent = nullptr;
    uint64_t m_nextFenceValue = 1;
};

} // namespace VENPOD::Graphics
