#include "DX12Device.h"
#include <spdlog/spdlog.h>
#include <dxgidebug.h>

namespace VENPOD::Graphics {

Result<void> DX12Device::Initialize(const DeviceConfig& config) {
    spdlog::info("Initializing DX12 Device...");

    // Enable debug layer if requested
    // When debug layer is active, also enable DRED for device-removed diagnostics
    bool debugLayerEnabled = false;
    if (config.enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            spdlog::info("D3D12 Debug Layer enabled");

            ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1))) {
                // Explicitly set GPU-based validation state
                debugController1->SetEnableGPUBasedValidation(
                    config.enableGPUValidation ? TRUE : FALSE);
                debugController1->SetEnableSynchronizedCommandQueueValidation(FALSE);
                if (config.enableGPUValidation) {
                    spdlog::info("GPU-based validation enabled");
                } else {
                    spdlog::info("GPU-based validation disabled");
                }
            }
            debugLayerEnabled = true;
        } else {
            spdlog::warn("Failed to enable D3D12 Debug Layer, continuing without it");
        }
    }

    if (debugLayerEnabled) {
        EnableDRED();
    }

    // Create DXGI Factory
    auto factoryResult = CreateFactory();
    if (factoryResult.IsErr()) {
        return Result<void>::Err(factoryResult.Error());
    }

    // Select adapter (GPU)
    auto adapterResult = SelectAdapter();
    if (adapterResult.IsErr()) {
        return Result<void>::Err(adapterResult.Error());
    }

    // Create device
    auto deviceResult = CreateDevice(config.minFeatureLevel);
    if (deviceResult.IsErr()) {
        return Result<void>::Err(deviceResult.Error());
    }

    // Check for tearing support
    CheckTearingSupport();

    spdlog::info("DX12 Device initialized successfully");
    return Result<void>::Ok();
}

void DX12Device::Shutdown() {
    // IMPORTANT: Caller must ensure GPU is idle before calling this!
    // Use WaitForGPU() on your command queue before shutdown.
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
    m_dedicatedVideoMemoryBytes = 0;

    spdlog::info("DX12 Device shut down");
}

Result<void> DX12Device::CreateFactory() {
    // Note: Don't use DXGI_CREATE_FACTORY_DEBUG to avoid runtime breaks
    UINT dxgiFactoryFlags = 0;

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create DXGI Factory");
    }

    return Result<void>::Ok();
}

Result<void> DX12Device::SelectAdapter() {
    ComPtr<IDXGIAdapter1> adapter;

    // Enumerate adapters preferring high-performance GPUs
    for (UINT adapterIndex = 0;
         SUCCEEDED(m_factory->EnumAdapterByGpuPreference(
             adapterIndex,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)));
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        // Check if adapter supports D3D12
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            m_adapter = adapter;
            m_dedicatedVideoMemoryBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);

            // Log adapter info
            char adapterName[128];
            size_t converted;
            wcstombs_s(&converted, adapterName, sizeof(adapterName), desc.Description, _TRUNCATE);
            spdlog::info("Selected GPU: {}", adapterName);
            spdlog::info("  Dedicated Video Memory: {} MB", desc.DedicatedVideoMemory / (1024 * 1024));

            return Result<void>::Ok();
        }
    }

    return Result<void>::Err("No compatible GPU adapter found");
}

Result<void> DX12Device::CreateDevice(D3D_FEATURE_LEVEL minFeatureLevel) {
    HRESULT hr = D3D12CreateDevice(
        m_adapter.Get(),
        minFeatureLevel,
        IID_PPV_ARGS(&m_device)
    );

    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create D3D12 device");
    }

    // Configure info queue for debug builds
    // Only break on corruption to avoid stopping on every validation error
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_device.As(&infoQueue))) {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

        // Filter out noisy info messages
        D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumSeverities = _countof(severities);
        filter.DenyList.pSeverityList = severities;
        infoQueue->PushStorageFilter(&filter);
    }

    return Result<void>::Ok();
}

void DX12Device::CheckTearingSupport() {
    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;

    if (SUCCEEDED(m_factory.As(&factory5))) {
        HRESULT hr = factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing)
        );

        m_supportsTearing = SUCCEEDED(hr) && allowTearing;

        if (m_supportsTearing) {
            spdlog::info("Variable refresh rate (tearing) supported");
        }
    }
}

Result<DX12Device::VideoMemoryInfo> DX12Device::QueryVideoMemoryInfo() const {
    VideoMemoryInfo out{};

    if (!m_adapter) {
        return Result<VideoMemoryInfo>::Err("DX12Device::QueryVideoMemoryInfo: adapter is null");
    }

    ComPtr<IDXGIAdapter3> adapter3;
    HRESULT hr = m_adapter.As(&adapter3);
    if (FAILED(hr) || !adapter3) {
        return Result<VideoMemoryInfo>::Err("DX12Device::QueryVideoMemoryInfo: IDXGIAdapter3 not available");
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(hr)) {
        return Result<VideoMemoryInfo>::Err("DX12Device::QueryVideoMemoryInfo: query failed");
    }

    out.currentUsageBytes = static_cast<uint64_t>(info.CurrentUsage);
    out.budgetBytes = static_cast<uint64_t>(info.Budget);
    out.availableForReservationBytes = static_cast<uint64_t>(info.AvailableForReservation);

    return Result<VideoMemoryInfo>::Ok(out);
}

void DX12Device::EnableDRED() {
    // DRED = Device Removed Extended Data
    // Provides breadcrumbs and page fault info for debugging device-removed errors
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        spdlog::info("DX12 DRED (auto-breadcrumbs + page fault reporting) enabled");
    } else {
        spdlog::warn("DX12 DRED settings interface not available");
    }
}

} // namespace VENPOD::Graphics
