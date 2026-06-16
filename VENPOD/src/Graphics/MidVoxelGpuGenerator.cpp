// =============================================================================
// VENPOD Phase-1 GPU mid-voxel brick generator (implementation).
// =============================================================================

#include "MidVoxelGpuGenerator.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

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

// Root 32-bit constants (b0) for the apply pass: mirrors ApplyParams.misc.
struct ApplyParamsConstants {
    uint32_t overrideCount;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
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

    // Phase B: edit-override apply pipeline (CS_ApplyMidEditCellsToClipmap). Optional:
    // if it fails to compile/init, gen still works (edits just won't bake into mid),
    // so we warn and continue rather than failing the whole generator.
    const std::filesystem::path applyCsPath =
        shaderPath / "Compute" / "CS_ApplyMidEditCellsToClipmap.hlsl";
    auto applyCompile = shaderCompiler.CompileComputeShader(applyCsPath, L"main", true);
    if (applyCompile && applyCompile.value().IsValid()) {
        ComputePipelineDesc applyDesc;
        applyDesc.computeShader = applyCompile.value();
        applyDesc.debugName = "CS_ApplyMidEditCellsToClipmap";
        // b0: 4x 32-bit constants (ApplyParamsConstants).
        applyDesc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 4 });
        // t0: root SRV (override buffer). u0: root UAV (sample pool).
        applyDesc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 });
        applyDesc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 });
        auto applyPipe = m_applyPipeline.Initialize(device, applyDesc);
        if (!applyPipe) {
            spdlog::warn("[MIDGEN] apply pipeline init failed: {} (edits will not bake to mid)",
                applyPipe.error());
        }
    } else {
        spdlog::warn("[MIDGEN] apply CS compile failed (edits will not bake to mid): {}",
            applyCompile ? applyCompile.value().errors : applyCompile.error());
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

bool MidVoxelGpuGenerator::ApplyEditOverrides(
    ID3D12GraphicsCommandList* cmdList,
    GPUBuffer& samplePool,
    const MidVoxelEditOverride* overrides,
    uint32_t overrideCount)
{
    if (!m_applyPipeline.IsValid() || !cmdList || !overrides || overrideCount == 0u) {
        return false;
    }
    if (!m_device) {
        return false;
    }

    // Cycle the override ring (independent of the request ring; both can be in flight).
    m_overrideRingCursor = (m_overrideRingCursor + 1u) % kRequestRingSize;
    const uint32_t idx = m_overrideRingCursor;

    if (m_overrideCapacity[idx] < overrideCount) {
        uint32_t newCap = std::max(overrideCount, 256u);
        newCap = ((newCap + 255u) / 256u) * 256u;
        m_overrideBuffers[idx].Shutdown();
        auto result = m_overrideBuffers[idx].Initialize(
            m_device.Get(),
            static_cast<uint64_t>(newCap) * sizeof(MidVoxelEditOverride),
            "MidVoxelGpuGen_Overrides");
        if (!result) {
            m_overrideCapacity[idx] = 0u;
            spdlog::error("[MIDGEN] override buffer init failed: {}", result.error());
            return false;
        }
        m_overrideCapacity[idx] = newCap;
    }

    void* mapped = m_overrideBuffers[idx].GetMappedData();
    if (!mapped) {
        return false;
    }
    std::memcpy(
        mapped, overrides,
        static_cast<size_t>(overrideCount) * sizeof(MidVoxelEditOverride));

    // Gen -> apply ordering: make the pristine sample writes visible before the
    // override overwrite (both target the same UAV; without this the writes could
    // reorder and the gen would clobber the override).
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = samplePool.GetResource();
    cmdList->ResourceBarrier(1, &uavBarrier);

    m_applyPipeline.Bind(cmdList);

    ApplyParamsConstants constants{};
    constants.overrideCount = overrideCount;
    constants.pad0 = 0u;
    constants.pad1 = 0u;
    constants.pad2 = 0u;
    m_applyPipeline.SetRoot32BitConstants(
        cmdList, 0, sizeof(constants) / 4u, &constants);

    // t0: override buffer as a root SRV. u0: sample pool as a root UAV.
    cmdList->SetComputeRootShaderResourceView(
        1, m_overrideBuffers[idx].GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(
        2, samplePool.GetGPUVirtualAddress());

    // One thread per override (64/group).
    const uint32_t groupCount = (overrideCount + 63u) / 64u;
    m_applyPipeline.Dispatch(cmdList, groupCount, 1, 1);
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

uint32_t MidVoxelGpuGenerator::VerifyEditedBricksAgainstCpu(
    ID3D12Device* device,
    DX12CommandQueue& commandQueue,
    GPUBuffer& liveSamplePool,
    const std::vector<MidVoxelBrickGenRequest>& requests,
    const std::vector<MidVoxelEditOverride>& overrides,
    const std::vector<std::vector<uint32_t>>& cpuBricks)
{
    if (!IsValid() || !device || requests.empty() ||
        requests.size() != cpuBricks.size()) {
        spdlog::error("[MIDBAKE-VERIFY] invalid args (reqs={}, cpu={})",
            requests.size(), cpuBricks.size());
        return 0u;
    }

    const uint64_t readbackBytes =
        static_cast<uint64_t>(requests.size()) * kBrickVoxelCount * sizeof(uint32_t);
    GPUBuffer rbPristine, rbBaked;
    if (!rbPristine.Initialize(device, readbackBytes, BufferUsage::Readback,
            sizeof(uint32_t), "MidEditBake_VerifyPristine") ||
        !rbBaked.Initialize(device, readbackBytes, BufferUsage::Readback,
            sizeof(uint32_t), "MidEditBake_VerifyBaked")) {
        spdlog::error("[MIDBAKE-VERIFY] readback init failed");
        return 0u;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
            IID_PPV_ARGS(&list)))) {
        spdlog::error("[MIDBAKE-VERIFY] cmd list create failed");
        return 0u;
    }

    auto copySlots = [&](GPUBuffer& dst) {
        for (uint32_t i = 0; i < requests.size(); ++i) {
            const uint64_t srcOffset =
                static_cast<uint64_t>(requests[i].destSlot) * kBrickVoxelCount * sizeof(uint32_t);
            const uint64_t dstOffset =
                static_cast<uint64_t>(i) * kBrickVoxelCount * sizeof(uint32_t);
            list->CopyBufferRegion(
                dst.GetResource(), dstOffset,
                liveSamplePool.GetResource(), srcOffset,
                static_cast<uint64_t>(kBrickVoxelCount) * sizeof(uint32_t));
        }
    };
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = liveSamplePool.GetResource();

    // Pass 1: gen ONLY -> read back the PRISTINE base (what the GPU generator
    // produces with no edits). Pass 2: re-gen + APPLY overrides -> read back the
    // BAKED result. Comparing baked-vs-(pristine+intended-overrides) isolates the
    // APPLY pass from the pre-existing GPU-pristine-vs-CPU-pristine gap (the shipped
    // engine already renders non-edited mid bricks from the GPU pristine, so the
    // correct reference for the bake is the GPU pristine + our edits, NOT a CPU regen).
    BeginMidVoxelGeneration(list.Get(), liveSamplePool);  // SRV->UAV
    if (!GenerateBricks(list.Get(), liveSamplePool, requests.data(),
            static_cast<uint32_t>(requests.size()), /*transitionSamplePool=*/false)) {
        spdlog::error("[MIDBAKE-VERIFY] pristine gen dispatch failed");
        list->Close();
        return 0u;
    }
    list->ResourceBarrier(1, &uavBarrier);
    liveSamplePool.TransitionTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    copySlots(rbPristine);
    liveSamplePool.TransitionTo(list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Pass 2: gen again (idempotent) then apply.
    GenerateBricks(list.Get(), liveSamplePool, requests.data(),
        static_cast<uint32_t>(requests.size()), /*transitionSamplePool=*/false);
    if (!overrides.empty() && m_applyPipeline.IsValid()) {
        ApplyEditOverrides(list.Get(), liveSamplePool, overrides.data(),
            static_cast<uint32_t>(overrides.size()));
    }
    list->ResourceBarrier(1, &uavBarrier);
    liveSamplePool.TransitionTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    copySlots(rbBaked);
    liveSamplePool.TransitionTo(list.Get(), kSamplePoolSrvState);

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    commandQueue.ExecuteCommandLists(lists, 1);
    const uint64_t fence = commandQueue.Signal();
    commandQueue.WaitForFenceValue(fence);

    const uint32_t* pristine = static_cast<const uint32_t*>(rbPristine.Map());
    const uint32_t* baked = static_cast<const uint32_t*>(rbBaked.Map());
    if (!pristine || !baked) {
        spdlog::error("[MIDBAKE-VERIFY] readback map failed");
        return 0u;
    }
    // Per-brick override map: localIndex -> intended voxel (last write wins, matching
    // the GPU scatter where each override is a unique cell).
    constexpr uint32_t kVisualSurfaceBit = 0x10u << 24u;  // StateFlags::VisualSurface
    constexpr uint32_t kMask = ~kVisualSurfaceBit;
    uint32_t correctBricks = 0;
    uint32_t totalOverrideCells = 0, overrideWrong = 0;
    uint32_t pristineChanged = 0;       // non-override cells that the apply altered (MUST be 0)
    uint32_t cpuMaskedMatchCells = 0, cpuTotalCells = 0;  // pre-existing GPU-vs-CPU pristine gap (info)
    for (uint32_t i = 0; i < requests.size(); ++i) {
        std::unordered_map<uint32_t, uint32_t> ovMap;
        for (const auto& ov : overrides) {
            if (ov.destSlot == requests[i].destSlot) {
                ovMap[ov.localIndex] = ov.voxel;
            }
        }
        const uint32_t* pr = pristine + static_cast<size_t>(i) * kBrickVoxelCount;
        const uint32_t* bk = baked + static_cast<size_t>(i) * kBrickVoxelCount;
        const uint32_t* cpu = (cpuBricks[i].size() == kBrickVoxelCount) ? cpuBricks[i].data() : nullptr;
        uint32_t ovWrong = 0, prChanged = 0;
        uint32_t firstOvWrong = UINT32_MAX, firstPrChanged = UINT32_MAX;
        for (uint32_t v = 0; v < kBrickVoxelCount; ++v) {
            auto it = ovMap.find(v);
            if (it != ovMap.end()) {
                ++totalOverrideCells;
                if (bk[v] != it->second) { ++ovWrong; if (firstOvWrong==UINT32_MAX) firstOvWrong=v; }
            } else {
                if (bk[v] != pr[v]) { ++prChanged; if (firstPrChanged==UINT32_MAX) firstPrChanged=v; }
            }
            if (cpu) { ++cpuTotalCells; if ((bk[v] & kMask) == (cpu[v] & kMask)) ++cpuMaskedMatchCells; }
        }
        overrideWrong += ovWrong;
        pristineChanged += prChanged;
        const auto& r = requests[i];
        if (ovWrong == 0 && prChanged == 0) {
            ++correctBricks;
            spdlog::info("[MIDBAKE-VERIFY] slot={} origin=({},{},{}) cell={} APPLY-CORRECT ({} overrides applied, pristine untouched)",
                r.destSlot, r.originX, r.originY, r.originZ, r.cellSize, static_cast<uint32_t>(ovMap.size()));
        } else {
            spdlog::error("[MIDBAKE-VERIFY] slot={} origin=({},{},{}) cell={} APPLY-WRONG: overrideWrong={} (firstIdx={} got=0x{:08X} want=0x{:08X}) pristineChanged={} (firstIdx={})",
                r.destSlot, r.originX, r.originY, r.originZ, r.cellSize,
                ovWrong, firstOvWrong, firstOvWrong!=UINT32_MAX?bk[firstOvWrong]:0u,
                firstOvWrong!=UINT32_MAX?ovMap[firstOvWrong]:0u, prChanged, firstPrChanged);
        }
    }
    rbPristine.Unmap();
    rbBaked.Unmap();
    spdlog::info(
        "[MIDBAKE-VERIFY] APPLY parity: correctBricks={}/{} | overrideCellsWrong={}/{} | pristineCellsAltered={} (MUST be 0) || info: baked-vs-CPU masked-cell-match={}/{} ({}% pre-existing GPU/CPU pristine gap)",
        correctBricks, static_cast<uint32_t>(requests.size()),
        overrideWrong, totalOverrideCells, pristineChanged,
        cpuMaskedMatchCells, cpuTotalCells,
        cpuTotalCells ? (100u * cpuMaskedMatchCells / cpuTotalCells) : 0u);
    return correctBricks;
}

void MidVoxelGpuGenerator::Shutdown() {
    for (auto& buf : m_requestBuffers) {
        buf.Shutdown();
    }
    m_requestCapacity = { 0u, 0u, 0u };
    m_requestRingCursor = 0u;
    for (auto& buf : m_overrideBuffers) {
        buf.Shutdown();
    }
    m_overrideCapacity = { 0u, 0u, 0u };
    m_overrideRingCursor = 0u;
    m_applyPipeline.Shutdown();
    m_pipeline.Shutdown();
    m_device.Reset();
}

} // namespace VENPOD::Graphics
