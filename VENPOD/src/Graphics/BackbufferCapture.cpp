#include "Graphics/BackbufferCapture.h"

#include "Graphics/RHI/d3dx12.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace VENPOD::Graphics {

bool WriteBackbufferBmp(const PendingBackbufferCapture& capture) {
    if (!capture.readback || capture.width == 0 || capture.height == 0 || capture.rowPitch == 0) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(capture.rowPitch) * capture.height};
    HRESULT hr = capture.readback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped) {
        spdlog::error("Capture failed to map readback for frame {}: 0x{:08X}",
            capture.frame,
            static_cast<uint32_t>(hr));
        return false;
    }

    std::filesystem::create_directories(capture.path.parent_path());
    std::ofstream out(capture.path, std::ios::binary);
    if (!out) {
        const D3D12_RANGE writeRange{0, 0};
        capture.readback->Unmap(0, &writeRange);
        spdlog::error("Capture failed to open output {}", capture.path.string());
        return false;
    }

    const uint32_t dstRowBytes = ((capture.width * 3u + 3u) / 4u) * 4u;
    const uint32_t imageBytes = dstRowBytes * capture.height;
    const uint32_t fileHeaderBytes = 14u;
    const uint32_t infoHeaderBytes = 40u;
    const uint32_t pixelOffset = fileHeaderBytes + infoHeaderBytes;
    const uint32_t fileBytes = pixelOffset + imageBytes;

    auto writeU16 = [&out](uint16_t value) {
        out.put(static_cast<char>(value & 0xFFu));
        out.put(static_cast<char>((value >> 8u) & 0xFFu));
    };
    auto writeU32 = [&out](uint32_t value) {
        out.put(static_cast<char>(value & 0xFFu));
        out.put(static_cast<char>((value >> 8u) & 0xFFu));
        out.put(static_cast<char>((value >> 16u) & 0xFFu));
        out.put(static_cast<char>((value >> 24u) & 0xFFu));
    };

    writeU16(0x4D42u);
    writeU32(fileBytes);
    writeU16(0u);
    writeU16(0u);
    writeU32(pixelOffset);
    writeU32(infoHeaderBytes);
    writeU32(capture.width);
    writeU32(capture.height);
    writeU16(1u);
    writeU16(24u);
    writeU32(0u);
    writeU32(imageBytes);
    writeU32(2835u);
    writeU32(2835u);
    writeU32(0u);
    writeU32(0u);

    const auto* src = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> row(dstRowBytes, 0u);
    for (uint32_t y = 0; y < capture.height; ++y) {
        const uint32_t srcY = capture.height - 1u - y;
        const uint8_t* srcRow = src + static_cast<size_t>(srcY) * capture.rowPitch;
        for (uint32_t x = 0; x < capture.width; ++x) {
            row[x * 3u + 0u] = srcRow[x * 4u + 0u];
            row[x * 3u + 1u] = srcRow[x * 4u + 1u];
            row[x * 3u + 2u] = srcRow[x * 4u + 2u];
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    const D3D12_RANGE writeRange{0, 0};
    capture.readback->Unmap(0, &writeRange);
    spdlog::info("CAPTURE_BACKBUFFER frame={} path={}", capture.frame, capture.path.string());
    return true;
}

bool QueueBackbufferCapture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    uint32_t frameNumber,
    const std::filesystem::path& outputPath,
    PendingBackbufferCapture& outCapture)
{
    if (!device || !commandList || !backBuffer) {
        return false;
    }

    D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        spdlog::error("Capture failed to create readback buffer: 0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toRenderTarget);

    outCapture.readback = readback;
    outCapture.width = static_cast<uint32_t>(desc.Width);
    outCapture.height = desc.Height;
    outCapture.rowPitch = footprint.Footprint.RowPitch;
    outCapture.frame = frameNumber;
    std::ostringstream name;
    name << "engine_frame_" << std::setw(4) << std::setfill('0') << frameNumber << ".bmp";
    outCapture.path = outputPath / name.str();
    return true;
}

} // namespace VENPOD::Graphics
