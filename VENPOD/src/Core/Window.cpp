#include "Window.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/DX12CommandQueue.h"
#include <spdlog/spdlog.h>

namespace VENPOD {

Window::~Window() {
    Shutdown();
}

Result<void> Window::Initialize(const WindowConfig& config, Graphics::DX12Device* device) {
    if (!device) {
        return Result<void>::Err("Invalid device pointer");
    }

    m_width = config.width;
    m_height = config.height;
    m_vsync = config.vsync;

    // Initialize SDL3 (returns true on success)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return Result<void>::Err("Failed to initialize SDL: " + std::string(SDL_GetError()));
    }

    // Create window
    uint32_t windowFlags = SDL_WINDOW_RESIZABLE;
    if (config.fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }

    m_window = SDL_CreateWindow(
        config.title.c_str(),
        static_cast<int>(m_width),
        static_cast<int>(m_height),
        windowFlags
    );

    if (!m_window) {
        return Result<void>::Err("Failed to create SDL window: " + std::string(SDL_GetError()));
    }

    spdlog::info("Window created: {}x{} ({})", m_width, m_height, config.title);
    return Result<void>::Ok();
}

Result<void> Window::InitializeSwapChain(Graphics::DX12Device* device, Graphics::DX12CommandQueue* commandQueue) {
    m_device = device;
    m_commandQueue = commandQueue;
    return CreateSwapChain(device, commandQueue);
}

void Window::Shutdown() {
    // IMPORTANT: Caller should have called WaitForGPU() before this!
    ReleaseRenderTargetViews();
    m_swapChain.Reset();

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    spdlog::info("Window shut down");
}

Result<void> Window::CreateSwapChain(Graphics::DX12Device* device, Graphics::DX12CommandQueue* commandQueue) {
    if (!device || !commandQueue) {
        return Result<void>::Err("Invalid device or command queue pointer");
    }

    // Get HWND from SDL window
    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

    if (!hwnd) {
        return Result<void>::Err("Failed to get HWND from SDL window");
    }
    m_hwnd = hwnd;

    // Describe swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = BUFFER_COUNT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = device->SupportsTearing() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = device->GetFactory()->CreateSwapChainForHwnd(
        commandQueue->GetCommandQueue(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    );

    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create swap chain");
    }

    // Disable Alt+Enter fullscreen toggle (we'll handle it ourselves)
    device->GetFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Query for IDXGISwapChain3 interface
    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        return Result<void>::Err("Failed to query IDXGISwapChain3 interface");
    }

    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    spdlog::info("Swap chain created with {} buffers (triple buffering)", BUFFER_COUNT);

    // Create render target views
    return CreateRenderTargetViews(device);
}

Result<void> Window::CreateRenderTargetViews(Graphics::DX12Device* device) {
    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = BUFFER_COUNT;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-only heap for RTVs

    HRESULT hr = device->GetDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        return Result<void>::Err("Failed to create RTV descriptor heap");
    }

    m_rtvDescriptorSize = device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create RTVs for each back buffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
        if (FAILED(hr)) {
            return Result<void>::Err("Failed to get back buffer " + std::to_string(i));
        }

        device->GetDevice()->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);

        // Set debug name for PIX/RenderDoc
        std::wstring name = L"BackBuffer" + std::to_wstring(i);
        m_backBuffers[i]->SetName(name.c_str());

        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    spdlog::info("Render target views created");
    return Result<void>::Ok();
}

void Window::ReleaseRenderTargetViews() {
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        m_backBuffers[i].Reset();
    }
    m_rtvHeap.Reset();
}

void Window::Present() {
    if (!m_swapChain) {
        return;
    }

    UINT syncInterval = m_vsync ? 1 : 0;
    UINT presentFlags = 0;

    // Allow tearing for lower latency when vsync is off
    if (!m_vsync && m_device && m_device->SupportsTearing()) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }

    m_swapChain->Present(syncInterval, presentFlags);
    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
}

uint32_t Window::GetCurrentBackBufferIndex() const {
    return m_currentBackBufferIndex;
}

ID3D12Resource* Window::GetCurrentBackBuffer() const {
    if (!m_swapChain) {
        return nullptr;
    }
    return m_backBuffers[m_currentBackBufferIndex].Get();
}

ID3D12Resource* Window::GetBackBuffer(uint32_t index) const {
    if (!m_swapChain || index >= BUFFER_COUNT) {
        return nullptr;
    }
    return m_backBuffers[index].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Window::GetCurrentRTV() const {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_currentBackBufferIndex) * m_rtvDescriptorSize;
    return rtv;
}

void Window::OnResize(uint32_t width, uint32_t height) {
    // Ignore minimization or redundant resize
    if (width == 0 || height == 0) {
        spdlog::warn("Ignoring resize to {}x{} (likely minimized)", width, height);
        return;
    }

    if (width == m_width && height == m_height) {
        return;
    }

    if (!m_device || !m_commandQueue || !m_swapChain) {
        return;
    }

    // CRITICAL: Flush GPU before resizing to avoid OBJECT_DELETED_WHILE_STILL_IN_USE
    m_commandQueue->Flush();

    ReleaseRenderTargetViews();

    HRESULT hr = m_swapChain->ResizeBuffers(
        BUFFER_COUNT,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_device->SupportsTearing() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0
    );

    if (FAILED(hr)) {
        spdlog::error("ResizeBuffers failed: 0x{:08X} for {}x{}",
                      static_cast<unsigned int>(hr), width, height);

        // Try to recover by recreating RTVs for existing buffers
        auto rtvResult = CreateRenderTargetViews(m_device);
        if (rtvResult.IsErr()) {
            spdlog::error("Failed to recreate RTVs after failed resize: {}", rtvResult.Error());
        }
        return;
    }

    m_width = width;
    m_height = height;
    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    auto rtvResult = CreateRenderTargetViews(m_device);
    if (rtvResult.IsErr()) {
        spdlog::error("Failed to recreate RTVs after resize: {}", rtvResult.Error());
    }

    spdlog::info("Window resized: {}x{}", m_width, m_height);
}

} // namespace VENPOD
