#include "DX12CommandQueue.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

DX12CommandQueue::~DX12CommandQueue() {
    Shutdown();
}

Result<void> DX12CommandQueue::Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
    if (!device) {
        return Result<void>::Err("Invalid device pointer");
    }

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = type;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create command queue");
    }

    // Create fence for GPU-CPU synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create fence");
    }

    // Create event for fence signaling
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        return Result<void>::Err("Failed to create fence event");
    }

    const char* typeStr = "Unknown";
    switch (type) {
        case D3D12_COMMAND_LIST_TYPE_DIRECT: typeStr = "Direct (Graphics)"; break;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: typeStr = "Compute"; break;
        case D3D12_COMMAND_LIST_TYPE_COPY: typeStr = "Copy"; break;
        default: break;
    }
    spdlog::info("Command Queue initialized ({})", typeStr);

    return Result<void>::Ok();
}

void DX12CommandQueue::Shutdown() {
    // CRITICAL: Ensure all GPU work is complete before cleanup
    // This prevents OBJECT_DELETED_WHILE_STILL_IN_USE errors
    if (m_commandQueue && m_fence) {
        Flush();
    }

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_fence.Reset();
    m_commandQueue.Reset();
}

void DX12CommandQueue::ExecuteCommandList(ID3D12CommandList* commandList) {
    if (!m_commandQueue || !commandList) {
        return;
    }
    ID3D12CommandList* const commandLists[] = { commandList };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
}

void DX12CommandQueue::ExecuteCommandLists(ID3D12CommandList* const* commandLists, uint32_t count) {
    if (!m_commandQueue || !commandLists || count == 0) {
        return;
    }
    m_commandQueue->ExecuteCommandLists(count, commandLists);
}

uint64_t DX12CommandQueue::Signal() {
    if (!m_commandQueue || !m_fence) {
        return 0;
    }

    uint64_t fenceValue = m_nextFenceValue++;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceValue);
    if (FAILED(hr)) {
        spdlog::error("Failed to signal command queue fence: 0x{:08X}",
                      static_cast<unsigned int>(hr));
        return 0;
    }
    return fenceValue;
}

void DX12CommandQueue::WaitForFenceValue(uint64_t fenceValue) {
    if (fenceValue == 0 || !m_fence || !m_fenceEvent) {
        return;
    }

    // Already completed?
    if (IsFenceComplete(fenceValue)) {
        return;
    }

    // Schedule an event when the fence reaches the specified value
    HRESULT hr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
    if (FAILED(hr)) {
        spdlog::error("Failed to set fence completion event: 0x{:08X}",
                      static_cast<unsigned int>(hr));
        return;
    }

    // Block CPU until GPU signals the fence
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void DX12CommandQueue::Flush() {
    // Signal and wait - ensures GPU is completely idle
    if (!m_commandQueue || !m_fence) {
        return;
    }
    uint64_t fenceValue = Signal();
    WaitForFenceValue(fenceValue);
}

bool DX12CommandQueue::IsFenceComplete(uint64_t fenceValue) const {
    if (!m_fence) {
        return true;
    }
    return m_fence->GetCompletedValue() >= fenceValue;
}

uint64_t DX12CommandQueue::GetLastCompletedFenceValue() const {
    if (!m_fence) {
        return 0;
    }
    return m_fence->GetCompletedValue();
}

} // namespace VENPOD::Graphics
