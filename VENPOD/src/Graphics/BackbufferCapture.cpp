#include "Graphics/BackbufferCapture.h"

#include "Graphics/RHI/d3dx12.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

namespace VENPOD::Graphics {

namespace {

bool AddUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool MulUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || (b != 0 && a > std::numeric_limits<uint64_t>::max() / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool FitsSizeT(uint64_t value) {
    return value <= static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max());
}

bool FitsBmpU32(uint64_t value) {
    return value <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
}

} // namespace

bool WriteBackbufferBmp(const PendingBackbufferCapture& capture) {
    if (!capture.readback || capture.width == 0 || capture.height == 0 || capture.rowPitch == 0) {
        return false;
    }

    uint64_t sourceRowBytes = 0;
    uint64_t readbackBytes = 0;
    uint64_t bmpPayloadRowBytes = 0;
    uint64_t imageBytes64 = 0;
    uint64_t fileBytes64 = 0;
    constexpr uint64_t fileHeaderBytes = 14u;
    constexpr uint64_t infoHeaderBytes = 40u;
    constexpr uint64_t pixelOffset = fileHeaderBytes + infoHeaderBytes;
    uint64_t lastRowOffset = 0;
    if (!MulUint64(capture.width, 4u, &sourceRowBytes) ||
        capture.rowPitch < sourceRowBytes ||
        !MulUint64(capture.rowPitch, capture.height - 1u, &lastRowOffset) ||
        !AddUint64(lastRowOffset, sourceRowBytes, &readbackBytes) ||
        !AddUint64(static_cast<uint64_t>(capture.width) * 3u, 3u, &bmpPayloadRowBytes)) {
        spdlog::error("Capture rejected invalid dimensions for frame {}: {}x{} rowPitch={}",
            capture.frame,
            capture.width,
            capture.height,
            capture.rowPitch);
        return false;
    }
    bmpPayloadRowBytes = (bmpPayloadRowBytes / 4u) * 4u;
    if (!MulUint64(bmpPayloadRowBytes, capture.height, &imageBytes64) ||
        !AddUint64(pixelOffset, imageBytes64, &fileBytes64) ||
        !FitsBmpU32(bmpPayloadRowBytes) ||
        !FitsBmpU32(imageBytes64) ||
        !FitsBmpU32(fileBytes64) ||
        !FitsSizeT(readbackBytes)) {
        spdlog::error("Capture rejected oversized BMP/readback for frame {}: {}x{} rowPitch={}",
            capture.frame,
            capture.width,
            capture.height,
            capture.rowPitch);
        return false;
    }

    const D3D12_RESOURCE_DESC readbackDesc = capture.readback->GetDesc();
    if (readbackDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        readbackDesc.Width < readbackBytes) {
        spdlog::error("Capture rejected undersized readback for frame {}: need={} buffer={}",
            capture.frame,
            readbackBytes,
            readbackDesc.Width);
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackBytes)};
    HRESULT hr = capture.readback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped) {
        spdlog::error("Capture failed to map readback for frame {}: 0x{:08X}",
            capture.frame,
            static_cast<uint32_t>(hr));
        return false;
    }

    std::error_code createDirError;
    std::filesystem::create_directories(capture.path.parent_path(), createDirError);
    if (createDirError) {
        const D3D12_RANGE writeRange{0, 0};
        capture.readback->Unmap(0, &writeRange);
        spdlog::error("Capture failed to create output directory {}: {}",
            capture.path.parent_path().string(),
            createDirError.message());
        return false;
    }

    std::ofstream out(capture.path, std::ios::binary);
    if (!out) {
        const D3D12_RANGE writeRange{0, 0};
        capture.readback->Unmap(0, &writeRange);
        spdlog::error("Capture failed to open output {}", capture.path.string());
        return false;
    }

    const uint32_t dstRowBytes = static_cast<uint32_t>(bmpPayloadRowBytes);
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    const uint32_t fileBytes = static_cast<uint32_t>(fileBytes64);

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
    writeU32(static_cast<uint32_t>(pixelOffset));
    writeU32(static_cast<uint32_t>(infoHeaderBytes));
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
            const size_t dstOffset = static_cast<size_t>(x) * 3u;
            const size_t srcOffset = static_cast<size_t>(x) * 4u;
            // Swapchain is R8G8B8A8, while 24-bit BMP rows are BGR.
            row[dstOffset + 0u] = srcRow[srcOffset + 2u];
            row[dstOffset + 1u] = srcRow[srcOffset + 1u];
            row[dstOffset + 2u] = srcRow[srcOffset + 0u];
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    const D3D12_RANGE writeRange{0, 0};
    capture.readback->Unmap(0, &writeRange);
    if (!out) {
        spdlog::error("Capture failed while writing output {}", capture.path.string());
        return false;
    }
    spdlog::info("CAPTURE_BACKBUFFER frame={} path={}", capture.frame, capture.path.string());
    return true;
}

bool QueueTextureCapture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES sourceState,
    D3D12_RESOURCE_STATES restoreState,
    uint32_t frameNumber,
    const std::filesystem::path& outputPath,
    const char* filePrefix,
    PendingBackbufferCapture& outCapture)
{
    if (!device || !commandList || !texture) {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.Width == 0 ||
        desc.Width > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
        desc.Height == 0 ||
        desc.Height > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("Capture rejected unsupported texture: dim={} format={} size={}x{}",
            static_cast<uint32_t>(desc.Dimension),
            static_cast<uint32_t>(desc.Format),
            desc.Width,
            desc.Height);
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);
    if (numRows == 0 ||
        footprint.Footprint.RowPitch == 0 ||
        footprint.Footprint.RowPitch > std::numeric_limits<uint32_t>::max() ||
        totalBytes == 0 ||
        totalBytes > static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max())) {
        spdlog::error("Capture rejected invalid copy footprint: rows={} rowPitch={} totalBytes={}",
            numRows,
            footprint.Footprint.RowPitch,
            totalBytes);
        return false;
    }

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
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            texture,
            sourceState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1, &toCopy);
    }
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (restoreState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            restoreState);
        commandList->ResourceBarrier(1, &restore);
    }

    outCapture.readback = readback;
    outCapture.width = static_cast<uint32_t>(desc.Width);
    outCapture.height = desc.Height;
    outCapture.rowPitch = footprint.Footprint.RowPitch;
    outCapture.frame = frameNumber;
    std::ostringstream name;
    name << (filePrefix && filePrefix[0] != '\0' ? filePrefix : "texture_frame")
         << "_" << std::setw(4) << std::setfill('0') << frameNumber << ".bmp";
    outCapture.path = outputPath / name.str();
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
    return QueueTextureCapture(
        device,
        commandList,
        backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        frameNumber,
        outputPath,
        "engine_frame",
        outCapture);
}

} // namespace VENPOD::Graphics
