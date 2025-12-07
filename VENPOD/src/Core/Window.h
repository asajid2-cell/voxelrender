#pragma once

#include <SDL3/SDL.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include "Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {
    class DX12Device;
    class DX12CommandQueue;
}

namespace VENPOD {

struct WindowConfig {
    std::string title = "VENPOD - Voxel Physics Engine";
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool fullscreen = false;
    bool vsync = true;
};

// Window wrapper with SDL3 and DX12 swapchain
// Triple-buffered for optimal frame pipelining
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Initialize window (must call InitializeSwapChain after command queue is created)
    Result<void> Initialize(const WindowConfig& config, Graphics::DX12Device* device);

    // Complete swapchain initialization (must be called after command queue is created)
    Result<void> InitializeSwapChain(Graphics::DX12Device* device, Graphics::DX12CommandQueue* commandQueue);

    // Cleanup - call after WaitForGPU()!
    void Shutdown();

    // Frame management
    void Present();

    [[nodiscard]] uint32_t GetCurrentBackBufferIndex() const;
    [[nodiscard]] ID3D12Resource* GetCurrentBackBuffer() const;
    [[nodiscard]] ID3D12Resource* GetBackBuffer(uint32_t index) const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    // Window properties
    [[nodiscard]] uint32_t GetWidth() const { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_height; }
    [[nodiscard]] float GetAspectRatio() const { return static_cast<float>(m_width) / static_cast<float>(m_height); }
    [[nodiscard]] SDL_Window* GetSDLWindow() const { return m_window; }
    [[nodiscard]] HWND GetHWND() const { return m_hwnd; }
    [[nodiscard]] IDXGISwapChain3* GetSwapChain() const { return m_swapChain.Get(); }
    [[nodiscard]] bool IsVSyncEnabled() const { return m_vsync; }

    void SetVSync(bool enabled) { m_vsync = enabled; }

    // Resize handling - IMPORTANT: Flushes GPU internally
    void OnResize(uint32_t width, uint32_t height);

    static constexpr uint32_t BUFFER_COUNT = 3;  // Triple buffering

private:
    Result<void> CreateSwapChain(Graphics::DX12Device* device, Graphics::DX12CommandQueue* commandQueue);
    Result<void> CreateRenderTargetViews(Graphics::DX12Device* device);
    void ReleaseRenderTargetViews();

    SDL_Window* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_vsync = true;
    HWND m_hwnd = nullptr;

    // Stored for resize operations
    Graphics::DX12Device* m_device = nullptr;
    Graphics::DX12CommandQueue* m_commandQueue = nullptr;

    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_backBuffers[BUFFER_COUNT];
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t m_rtvDescriptorSize = 0;

    uint32_t m_currentBackBufferIndex = 0;
};

} // namespace VENPOD
