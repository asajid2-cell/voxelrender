// =============================================================================
// VENPOD Phase-1 GPU mid-voxel brick generator (implementation).
// =============================================================================

#include "MidVoxelGpuGenerator.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "RHI/DX12CommandQueue.h"

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kBrickVoxelCount = 4096u;

constexpr D3D12_RESOURCE_STATES kSamplePoolSrvState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Root 32-bit constants (b0): mirrors GenParams.misc { seed, requestCount, 0, 0 }.
struct GenParamsConstants {
    uint32_t seed;
    uint32_t requestCount;
    uint32_t pad0;
    uint32_t pad1;
};

} // namespace

Result<void> MidVoxelGpuGenerator::Initialize(
    ID3D12Device* device,
    ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath,
    uint32_t seed)
{
    if (!device) {
        return Error("MidVoxelGpuGenerator: null device");
    }
    m_device = device;
    m_seed = seed;

    const std::filesystem::path csPath =
        shaderPath / "Compute" / "CS_GenerateMidVoxelBricks.hlsl";

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("MidVoxelGpuGenerator: failed to compile CS: {}", compileResult.error());
    }
    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("MidVoxelGpuGenerator: CS compilation failed: {}", compiledShader.errors);
    }

    ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_GenerateMidVoxelBricks_Batch";
    // b0: 4x 32-bit constants (GenParamsConstants).
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 4 });
    // t0: root SRV (request buffer, bound by GPU virtual address).
    desc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 });
    // u0: root UAV (sample pool, bound by GPU virtual address).
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 });

    auto pipeResult = m_pipeline.Initialize(device, desc);
    if (!pipeResult) {
        return Error("MidVoxelGpuGenerator: pipeline init failed: {}", pipeResult.error());
    }

    return {};
}

Result<void> MidVoxelGpuGenerator::EnsureRequestCapacity(
    ID3D12Device* device, uint32_t requestCount)
{
    const uint32_t idx = m_requestRingCursor;
    if (m_requestCapacity[idx] >= requestCount && requestCount > 0u) {
        return {};
    }
    // Grow with headroom to avoid frequent reallocation.
    uint32_t newCap = std::max(requestCount, 64u);
    newCap = ((newCap + 63u) / 64u) * 64u;
    m_requestBuffers[idx].Shutdown();
    auto result = m_requestBuffers[idx].Initialize(
        device,
        static_cast<uint64_t>(newCap) * sizeof(MidVoxelBrickGenRequest),
        "MidVoxelGpuGen_Requests");
    if (!result) {
        m_requestCapacity[idx] = 0u;
        return Error("MidVoxelGpuGenerator: request buffer init failed: {}", result.error());
    }
    m_requestCapacity[idx] = newCap;
    return {};
}

void MidVoxelGpuGenerator::BeginMidVoxelGeneration(
    ID3D12GraphicsCommandList* cmdList, GPUBuffer& samplePool)
{
    samplePool.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void MidVoxelGpuGenerator::EndMidVoxelGeneration(
    ID3D12GraphicsCommandList* cmdList, GPUBuffer& samplePool)
{
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = samplePool.GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);
    samplePool.TransitionTo(cmdList, kSamplePoolSrvState);
}

bool MidVoxelGpuGenerator::GenerateBricks(
    ID3D12GraphicsCommandList* cmdList,
    GPUBuffer& samplePool,
    const MidVoxelBrickGenRequest* requests,
    uint32_t requestCount,
    bool transitionSamplePool)
{
    if (!IsValid() || !cmdList || !requests || requestCount == 0u) {
        return false;
    }
    if (!m_device) {
        return false;
    }

    // Cycle the request ring so we never overwrite a buffer still referenced by a
    // dispatch in flight (kRequestRingSize == frames in flight).
    m_requestRingCursor = (m_requestRingCursor + 1u) % kRequestRingSize;
    const uint32_t idx = m_requestRingCursor;

    auto capResult = EnsureRequestCapacity(m_device.Get(), requestCount);
    if (!capResult) {
        spdlog::error("[MIDGEN] {}", capResult.error());
        return false;
    }

    void* mapped = m_requestBuffers[idx].GetMappedData();
    if (!mapped) {
        return false;
    }
    std::memcpy(
        mapped, requests,
        static_cast<size_t>(requestCount) * sizeof(MidVoxelBrickGenRequest));

    if (transitionSamplePool) {
        BeginMidVoxelGeneration(cmdList, samplePool);
    }

    m_pipeline.Bind(cmdList);

    GenParamsConstants constants{};
    constants.seed = m_seed;
    constants.requestCount = requestCount;
    constants.pad0 = 0u;
    constants.pad1 = 0u;
    m_pipeline.SetRoot32BitConstants(
        cmdList, 0, sizeof(constants) / 4u, &constants);

    // t0: request buffer as a root SRV (by GPU virtual address).
    cmdList->SetComputeRootShaderResourceView(
        1, m_requestBuffers[idx].GetGPUVirtualAddress());
    // u0: sample pool as a root UAV (by GPU virtual address).
    cmdList->SetComputeRootUnorderedAccessView(
        2, samplePool.GetGPUVirtualAddress());

    // One thread group per brick.
    m_pipeline.Dispatch(cmdList, requestCount, 1, 1);

    if (transitionSamplePool) {
        EndMidVoxelGeneration(cmdList, samplePool);
    }
    return true;
}

uint32_t MidVoxelGpuGenerator::VerifyLiveSamplePool(
    ID3D12Device* device,
    DX12CommandQueue& commandQueue,
    GPUBuffer& liveSamplePool,
    const std::vector<MidVoxelBrickGenRequest>& requests,
    const std::vector<std::vector<uint32_t>>& cpuBricks)
{
    if (!IsValid() || !device || requests.empty() ||
        requests.size() != cpuBricks.size()) {
        spdlog::error("[MIDGEN-VERIFY] invalid args");
        return 0u;
    }

    const uint64_t readbackBytes =
        static_cast<uint64_t>(requests.size()) * kBrickVoxelCount * sizeof(uint32_t);
    GPUBuffer readback;
    if (!readback.Initialize(device, readbackBytes, BufferUsage::Readback,
            sizeof(uint32_t), "MidVoxelGpuGen_VerifyReadback")) {
        spdlog::error("[MIDGEN-VERIFY] readback init failed");
        return 0u;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
            IID_PPV_ARGS(&list)))) {
        spdlog::error("[MIDGEN-VERIFY] cmd list create failed");
        return 0u;
    }

    // Dispatch the batch into the REAL live pool (brackets the SRV<->UAV
    // transition itself), then copy each destSlot's brick to the readback buffer.
    const bool dispatched = GenerateBricks(
        list.Get(), liveSamplePool, requests.data(),
        static_cast<uint32_t>(requests.size()), /*transitionSamplePool=*/true);
    if (!dispatched) {
        spdlog::error("[MIDGEN-VERIFY] dispatch failed");
        list->Close();
        return 0u;
    }

    liveSamplePool.TransitionTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (uint32_t i = 0; i < requests.size(); ++i) {
        const uint64_t srcOffset =
            static_cast<uint64_t>(requests[i].destSlot) * kBrickVoxelCount * sizeof(uint32_t);
        const uint64_t dstOffset =
            static_cast<uint64_t>(i) * kBrickVoxelCount * sizeof(uint32_t);
        list->CopyBufferRegion(
            readback.GetResource(), dstOffset,
            liveSamplePool.GetResource(), srcOffset,
            static_cast<uint64_t>(kBrickVoxelCount) * sizeof(uint32_t));
    }
    // Restore the live pool to SRV so the renderer can read it.
    liveSamplePool.TransitionTo(list.Get(), kSamplePoolSrvState);

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    commandQueue.ExecuteCommandLists(lists, 1);
    const uint64_t fence = commandQueue.Signal();
    commandQueue.WaitForFenceValue(fence);

    const uint32_t* gpuData = static_cast<const uint32_t*>(readback.Map());
    if (!gpuData) {
        spdlog::error("[MIDGEN-VERIFY] readback map failed");
        return 0u;
    }
    uint32_t matches = 0;
    for (uint32_t i = 0; i < requests.size(); ++i) {
        const uint32_t* gpu = gpuData + static_cast<size_t>(i) * kBrickVoxelCount;
        const uint32_t* cpu = cpuBricks[i].data();
        bool match = (cpuBricks[i].size() == kBrickVoxelCount);
        uint32_t firstDiff = 0;
        for (uint32_t v = 0; match && v < kBrickVoxelCount; ++v) {
            if (gpu[v] != cpu[v]) { match = false; firstDiff = v; }
        }
        const auto& r = requests[i];
        if (match) {
            ++matches;
            spdlog::info(
                "[MIDGEN-VERIFY] slot={} origin=({},{},{}) cell={} match=true (live pool)",
                r.destSlot, r.originX, r.originY, r.originZ, r.cellSize);
        } else {
            spdlog::error(
                "[MIDGEN-VERIFY] slot={} origin=({},{},{}) cell={} match=FALSE firstDiff={} "
                "cpu=0x{:08X} gpu=0x{:08X}",
                r.destSlot, r.originX, r.originY, r.originZ, r.cellSize,
                firstDiff, cpu[firstDiff], gpu[firstDiff]);
        }
    }
    readback.Unmap();
    spdlog::info("[MIDGEN-VERIFY] LIVE sample pool parity: {}/{} bricks matched",
        matches, static_cast<uint32_t>(requests.size()));
    return matches;
}

void MidVoxelGpuGenerator::Shutdown() {
    for (auto& buf : m_requestBuffers) {
        buf.Shutdown();
    }
    m_requestCapacity = { 0u, 0u, 0u };
    m_requestRingCursor = 0u;
    m_pipeline.Shutdown();
    m_device.Reset();
}

} // namespace VENPOD::Graphics
