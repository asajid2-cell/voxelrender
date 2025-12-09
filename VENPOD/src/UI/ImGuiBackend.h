#pragma once

// =============================================================================
// VENPOD ImGui Backend - SDL3 + DX12 Integration
// =============================================================================

#include <d3d12.h>
#include <SDL3/SDL.h>

namespace VENPOD::UI {

class ImGuiBackend {
public:
    ImGuiBackend() = default;
    ~ImGuiBackend() = default;

    // Non-copyable
    ImGuiBackend(const ImGuiBackend&) = delete;
    ImGuiBackend& operator=(const ImGuiBackend&) = delete;

    // Initialize ImGui with SDL3 and DX12
    // Must be called after device and window are initialized
    bool Initialize(
        SDL_Window* window,
        ID3D12Device* device,
        uint32_t numFramesInFlight,
        DXGI_FORMAT rtvFormat,
        ID3D12DescriptorHeap* srvHeap
    );

    // Shutdown ImGui
    void Shutdown();

    // Begin a new ImGui frame (call before rendering UI)
    void NewFrame();

    // Render ImGui draw data to command list
    void Render(ID3D12GraphicsCommandList* commandList);

    // Process SDL3 event for ImGui input
    bool ProcessEvent(const SDL_Event& event);

private:
    bool m_initialized = false;
};

} // namespace VENPOD::UI
