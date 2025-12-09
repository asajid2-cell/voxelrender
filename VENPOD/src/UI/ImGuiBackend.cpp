#include "ImGuiBackend.h"
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_dx12.h>
#include <spdlog/spdlog.h>

namespace VENPOD::UI {

bool ImGuiBackend::Initialize(
    SDL_Window* window,
    ID3D12Device* device,
    uint32_t numFramesInFlight,
    DXGI_FORMAT rtvFormat,
    ID3D12DescriptorHeap* srvHeap
) {
    if (m_initialized) {
        spdlog::warn("ImGuiBackend already initialized");
        return true;
    }

    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard navigation

    // Setup ImGui style
    ImGui::StyleColorsDark();

    // Initialize SDL3 backend
    if (!ImGui_ImplSDL3_InitForD3D(window)) {
        spdlog::error("Failed to initialize ImGui SDL3 backend");
        return false;
    }

    // Initialize DX12 backend
    // ImGui needs one SRV descriptor from the shader-visible heap
    // We'll use the first descriptor in the heap for ImGui's font texture
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();

    if (!ImGui_ImplDX12_Init(
        device,
        numFramesInFlight,
        rtvFormat,
        srvHeap,
        cpuHandle,
        gpuHandle
    )) {
        spdlog::error("Failed to initialize ImGui DX12 backend");
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    m_initialized = true;
    spdlog::info("ImGui initialized successfully");
    return true;
}

void ImGuiBackend::Shutdown() {
    if (!m_initialized) {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
    spdlog::info("ImGui shut down");
}

void ImGuiBackend::NewFrame() {
    if (!m_initialized) {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiBackend::Render(ID3D12GraphicsCommandList* commandList) {
    if (!m_initialized) {
        return;
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

bool ImGuiBackend::ProcessEvent(const SDL_Event& event) {
    if (!m_initialized) {
        return false;
    }

    return ImGui_ImplSDL3_ProcessEvent(&event);
}

} // namespace VENPOD::UI
