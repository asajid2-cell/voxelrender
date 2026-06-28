#include "Graphics/RHI/DX12CommandQueue.h"
#include "Graphics/RHI/DX12ComputePipeline.h"
#include "Graphics/RHI/DX12Device.h"
#include "Graphics/RHI/GPUBuffer.h"
#include "Graphics/RHI/ShaderCompiler.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace {

using namespace VENPOD::Graphics;

constexpr uint32_t kFaceCellCount = 384u;
constexpr uint32_t kFacesPerCell = 2u;
constexpr uint32_t kFaceCount = kFaceCellCount * kFaceCellCount * kFacesPerCell;
constexpr uint32_t kFaceStride = 16u;
constexpr uint64_t kFaceBufferBytes = static_cast<uint64_t>(kFaceCount) * kFaceStride;
constexpr uint32_t kFaceCellSizeVoxels = 28u;
constexpr float kFaceCellSize = static_cast<float>(kFaceCellSizeVoxels);
constexpr float kFarHeightfieldMaxDistance = 10400.0f;
constexpr float kFarHeightfieldOwnerMaxDistance = 11000.0f;

struct FarFace {
    int32_t worldX;
    int32_t worldY;
    int32_t worldZ;
    uint32_t payload;
};
static_assert(sizeof(FarFace) == kFaceStride);

struct GenerateConstants {
    uint32_t faceCount;
    uint32_t cellCount;
    uint32_t baseGridCellSize;
    uint32_t worldSeed;
    uint32_t originXBits;
    uint32_t originZBits;
    uint32_t farHandoffBits;
    uint32_t ownerMaxDistanceBits;
    uint32_t cameraXBits;
    uint32_t cameraYBits;
    uint32_t cameraZBits;
    uint32_t pad0;
};
static_assert(sizeof(GenerateConstants) == 48);

struct Options {
    std::filesystem::path shaderRoot = "assets/shaders";
    std::filesystem::path outCsv = "build/tools/far_owner_gpu_faces.csv";
    float cameraX = 192.0f;
    float cameraY = 0.0f;
    float cameraZ = 224.0f;
    float forwardX = 0.0f;
    float forwardZ = 1.0f;
    float farHandoff = 5677.28f;
    uint32_t seed = 12345u;
};

uint32_t FloatBits(float value) {
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool ParseFloatArg(const char* text, float& out) {
    if (!text) {
        return false;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool ParseUintArg(const char* text, uint32_t& out) {
    if (!text) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || value > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--out") {
            if (const char* value = needValue("--out")) {
                options.outCsv = value;
            } else {
                return false;
            }
        } else if (arg == "--shader-root") {
            if (const char* value = needValue("--shader-root")) {
                options.shaderRoot = value;
            } else {
                return false;
            }
        } else if (arg == "--camera-x") {
            if (!ParseFloatArg(needValue("--camera-x"), options.cameraX)) return false;
        } else if (arg == "--camera-y") {
            if (!ParseFloatArg(needValue("--camera-y"), options.cameraY)) return false;
        } else if (arg == "--camera-z") {
            if (!ParseFloatArg(needValue("--camera-z"), options.cameraZ)) return false;
        } else if (arg == "--forward-x") {
            if (!ParseFloatArg(needValue("--forward-x"), options.forwardX)) return false;
        } else if (arg == "--forward-z") {
            if (!ParseFloatArg(needValue("--forward-z"), options.forwardZ)) return false;
        } else if (arg == "--far-handoff") {
            if (!ParseFloatArg(needValue("--far-handoff"), options.farHandoff)) return false;
        } else if (arg == "--seed") {
            if (!ParseUintArg(needValue("--seed"), options.seed)) return false;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            return false;
        }
    }
    return !options.outCsv.empty() && !options.shaderRoot.empty();
}

void PrintUsage() {
    std::cerr
        << "usage: far_owner_gpu_dump --out <faces.csv> [--shader-root assets/shaders] "
        << "[--camera-x 192] [--camera-y 0] [--camera-z 224] "
        << "[--forward-x 0] [--forward-z 1] [--far-handoff 5677.28] [--seed 12345]\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }

    DX12Device device;
    DeviceConfig deviceConfig{};
    auto deviceResult = device.Initialize(deviceConfig);
    if (!deviceResult) {
        std::cerr << "device init failed: " << deviceResult.Error() << "\n";
        return 1;
    }

    DX12CommandQueue queue;
    auto queueResult = queue.Initialize(device.GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    if (!queueResult) {
        std::cerr << "queue init failed: " << queueResult.Error() << "\n";
        return 1;
    }

    ShaderCompiler compiler;
    auto compilerResult = compiler.Initialize();
    if (!compilerResult) {
        std::cerr << "shader compiler init failed: " << compilerResult.error() << "\n";
        return 1;
    }
    compiler.SetIncludePath(options.shaderRoot);

    const std::filesystem::path csPath =
        options.shaderRoot / "Compute" / "CS_GenerateFarHeightfieldFaces.hlsl";
    auto csResult = compiler.CompileComputeShader(csPath, L"main", false);
    if (!csResult) {
        std::cerr << "CS compile failed: " << csResult.error() << "\n";
        return 1;
    }

    ComputePipelineDesc pipeDesc;
    pipeDesc.computeShader = csResult.value();
    pipeDesc.debugName = "FarOwnerGpuDump_Generate";
    pipeDesc.rootParams.push_back({RootParamType::Constants32Bit, 0, 0, 12});
    pipeDesc.rootParams.push_back({RootParamType::UnorderedAccess, 0, 0, 1});

    DX12ComputePipeline pipeline;
    auto pipeResult = pipeline.Initialize(device.GetDevice(), pipeDesc);
    if (!pipeResult) {
        std::cerr << "pipeline init failed: " << pipeResult.error() << "\n";
        return 1;
    }

    GPUBuffer faces;
    auto facesResult = faces.Initialize(
        device.GetDevice(),
        kFaceBufferBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        kFaceStride,
        "FarOwnerGpuDump_Faces");
    if (!facesResult) {
        std::cerr << "faces init failed: " << facesResult.error() << "\n";
        return 1;
    }

    GPUBuffer readback;
    auto readbackResult = readback.Initialize(
        device.GetDevice(),
        kFaceBufferBytes,
        BufferUsage::Readback,
        kFaceStride,
        "FarOwnerGpuDump_Readback");
    if (!readbackResult) {
        std::cerr << "readback init failed: " << readbackResult.error() << "\n";
        return 1;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = device.GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        std::cerr << "CreateCommandAllocator failed: 0x" << std::hex << hr << "\n";
        return 1;
    }

    ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = device.GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) {
        std::cerr << "CreateCommandList failed: 0x" << std::hex << hr << "\n";
        return 1;
    }

    float forwardX = options.forwardX;
    float forwardZ = options.forwardZ;
    const float forwardLen2 = forwardX * forwardX + forwardZ * forwardZ;
    if (forwardLen2 > 1.0e-5f) {
        const float invLen = 1.0f / std::sqrt(forwardLen2);
        forwardX *= invLen;
        forwardZ *= invLen;
    } else {
        forwardX = 0.0f;
        forwardZ = 1.0f;
    }

    const float extent = static_cast<float>(kFaceCellCount) * kFaceCellSize;
    const float halfExtent = extent * 0.5f;
    const float forwardShift = std::max(0.0f, kFarHeightfieldMaxDistance - halfExtent);
    const float centerX = options.cameraX + forwardX * forwardShift;
    const float centerZ = options.cameraZ + forwardZ * forwardShift;
    const float originX = std::floor((centerX - halfExtent) / kFaceCellSize) * kFaceCellSize;
    const float originZ = std::floor((centerZ - halfExtent) / kFaceCellSize) * kFaceCellSize;

    GenerateConstants constants{};
    constants.faceCount = kFaceCount;
    constants.cellCount = kFaceCellCount;
    constants.baseGridCellSize = kFaceCellSizeVoxels;
    constants.worldSeed = options.seed;
    constants.originXBits = FloatBits(originX);
    constants.originZBits = FloatBits(originZ);
    constants.farHandoffBits = FloatBits(options.farHandoff);
    constants.ownerMaxDistanceBits = FloatBits(kFarHeightfieldOwnerMaxDistance);
    constants.cameraXBits = FloatBits(options.cameraX);
    constants.cameraYBits = FloatBits(options.cameraY);
    constants.cameraZBits = FloatBits(options.cameraZ);

    faces.TransitionTo(commandList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pipeline.Bind(commandList.Get());
    pipeline.SetRoot32BitConstants(commandList.Get(), 0, 12, &constants);
    commandList->SetComputeRootUnorderedAccessView(1, faces.GetGPUVirtualAddress());
    pipeline.Dispatch(commandList.Get(), (kFaceCount + 127u) / 128u, 1u, 1u);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = faces.GetResource();
    commandList->ResourceBarrier(1, &barrier);

    faces.TransitionTo(commandList.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readback.GetResource(), 0, faces.GetResource(), 0, kFaceBufferBytes);

    hr = commandList->Close();
    if (FAILED(hr)) {
        std::cerr << "command list close failed: 0x" << std::hex << hr << "\n";
        return 1;
    }

    ID3D12CommandList* lists[] = {commandList.Get()};
    queue.ExecuteCommandLists(lists, 1);
    const uint64_t fence = queue.Signal();
    queue.WaitForFenceValue(fence);

    const auto* mappedFaces = static_cast<const FarFace*>(readback.Map());
    if (!mappedFaces) {
        std::cerr << "readback map failed\n";
        return 1;
    }

    if (const std::filesystem::path parent = options.outCsv.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream csv(options.outCsv, std::ios::out | std::ios::trunc);
    if (!csv) {
        std::cerr << "failed to open output CSV: " << options.outCsv.string() << "\n";
        return 1;
    }

    uint32_t activeFaces = 0;
    csv << "# cameraX=" << options.cameraX
        << " cameraY=" << options.cameraY
        << " cameraZ=" << options.cameraZ
        << " faces=" << kFaceCount
        << " cellSize=" << kFaceCellSizeVoxels
        << " originX=" << originX
        << " originZ=" << originZ
        << " generation=gpu_far_height_voxelized_standalone\n";
    csv << "worldX,worldY,worldZ,payload\n";
    for (uint32_t i = 0; i < kFaceCount; ++i) {
        activeFaces += mappedFaces[i].payload != 0u ? 1u : 0u;
        csv << mappedFaces[i].worldX << ','
            << mappedFaces[i].worldY << ','
            << mappedFaces[i].worldZ << ','
            << mappedFaces[i].payload << '\n';
    }
    csv.close();
    readback.Unmap();

    queue.Flush();
    std::cout << "FAR_OWNER_GPU_DUMP faces=" << kFaceCount
              << " activeFaces=" << activeFaces
              << " out=" << options.outCsv.string()
              << " origin=(" << originX << "," << originZ << ")"
              << "\n";
    return csv ? 0 : 1;
}
