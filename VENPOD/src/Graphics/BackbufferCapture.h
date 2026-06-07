#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

namespace VENPOD::Graphics {

struct BackbufferCaptureConfig {
    bool enabled = false;
    std::filesystem::path outputDir;
    uint32_t startFrame = 120;
    uint32_t intervalFrames = 30;
    uint32_t count = 8;
};

struct PendingBackbufferCapture {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    uint32_t frame = 0;
    std::filesystem::path path;
};

bool QueueBackbufferCapture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    uint32_t frameNumber,
    const std::filesystem::path& outputPath,
    PendingBackbufferCapture& outCapture);

bool QueueTextureCapture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES sourceState,
    D3D12_RESOURCE_STATES restoreState,
    uint32_t frameNumber,
    const std::filesystem::path& outputPath,
    const char* filePrefix,
    PendingBackbufferCapture& outCapture);

bool WriteBackbufferBmp(const PendingBackbufferCapture& capture);

} // namespace VENPOD::Graphics
