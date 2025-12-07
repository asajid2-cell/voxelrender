#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <memory>
#include <cstdint>
#include "Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Configuration for device initialization
struct DeviceConfig {
    bool enableDebugLayer = true;
    bool enableGPUValidation = false;
    D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_12_0;
};

// Core DX12 Device wrapper - manages the low-level GPU interface
class DX12Device {
public:
    struct VideoMemoryInfo {
        uint64_t currentUsageBytes = 0;
        uint64_t budgetBytes = 0;
        uint64_t availableForReservationBytes = 0;
    };

    DX12Device() = default;
    ~DX12Device() = default;

    // Disable copy, allow move
    DX12Device(const DX12Device&) = delete;
    DX12Device& operator=(const DX12Device&) = delete;
    DX12Device(DX12Device&&) = default;
    DX12Device& operator=(DX12Device&&) = default;

    // Initialize the device with given configuration
    Result<void> Initialize(const DeviceConfig& config = {});

    // Cleanup - IMPORTANT: Call WaitForGPU() before this to avoid
    // OBJECT_DELETED_WHILE_STILL_IN_USE errors!
    void Shutdown();

    // Accessors
    [[nodiscard]] ID3D12Device* GetDevice() const { return m_device.Get(); }
    [[nodiscard]] IDXGIFactory6* GetFactory() const { return m_factory.Get(); }
    [[nodiscard]] IDXGIAdapter1* GetAdapter() const { return m_adapter.Get(); }

    // Approximate dedicated video memory reported by the selected adapter (bytes)
    [[nodiscard]] uint64_t GetDedicatedVideoMemoryBytes() const { return m_dedicatedVideoMemoryBytes; }

    // Query current DXGI video memory usage and budget
    [[nodiscard]] Result<VideoMemoryInfo> QueryVideoMemoryInfo() const;

    // Check for tearing support (for variable refresh rate displays)
    [[nodiscard]] bool SupportsTearing() const { return m_supportsTearing; }

private:
    Result<void> CreateFactory();
    Result<void> SelectAdapter();
    Result<void> CreateDevice(D3D_FEATURE_LEVEL minFeatureLevel);
    void CheckTearingSupport();
    void EnableDRED();

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;

    uint64_t m_dedicatedVideoMemoryBytes = 0;
    bool m_supportsTearing = false;
};

} // namespace VENPOD::Graphics
