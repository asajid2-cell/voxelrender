// =============================================================================
// VENPOD Phase-0 POC: GPU mid-voxel brick generation parity harness (dev-only).
// =============================================================================

#include "MidVoxelGpuGenPoc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "RHI/DX12CommandQueue.h"
#include "../Simulation/SparseTerrainGenerator.h"
#include "../Simulation/SparseVoxelTypes.h"
#include "../Simulation/TerrainConstants.h"
#include "../Utils/BitPacking.h"

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kBrickVoxelCount = 4096u;   // 16^3 == SPARSE_BRICK_VOXEL_COUNT

// Constant buffer payload mirroring CS_GenerateMidVoxelBricks.hlsl `GenParams`.
struct GenParams {
    int32_t  originX;
    int32_t  originY;
    int32_t  originZ;
    int32_t  cellSize;
    uint32_t seed;
    uint32_t brickCount;
    uint32_t pad0;
    uint32_t pad1;
};

} // namespace

Result<void> MidVoxelGpuGenPoc::Initialize(
    ID3D12Device* device,
    ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    if (!device) {
        return Error("MidVoxelGpuGenPoc: null device");
    }

    const std::filesystem::path csPath =
        shaderPath / "Compute" / "CS_GenerateMidVoxelBricks.hlsl";

    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", true);
    if (!compileResult) {
        return Error("MidVoxelGpuGenPoc: failed to compile CS: {}", compileResult.error());
    }
    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("MidVoxelGpuGenPoc: CS compilation failed: {}", compiledShader.errors);
    }

    ComputePipelineDesc desc;
    desc.computeShader = compiledShader;
    desc.debugName = "CS_GenerateMidVoxelBricks";
    // b0: 8x 32-bit constants (GenParams).
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 8 });
    // u0: root UAV (bound by GPU virtual address; no descriptor heap needed).
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 });

    auto pipeResult = m_pipeline.Initialize(device, desc);
    if (!pipeResult) {
        return Error("MidVoxelGpuGenPoc: pipeline init failed: {}", pipeResult.error());
    }

    auto outResult = m_outSamples.Initialize(
        device,
        static_cast<uint64_t>(kBrickVoxelCount) * sizeof(uint32_t),
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "MidVoxelGpuGenPoc_OutSamples");
    if (!outResult) {
        return Error("MidVoxelGpuGenPoc: out buffer init failed: {}", outResult.error());
    }

    auto readbackResult = m_readback.Initialize(
        device,
        static_cast<uint64_t>(kBrickVoxelCount) * sizeof(uint32_t),
        BufferUsage::Readback,
        sizeof(uint32_t),
        "MidVoxelGpuGenPoc_Readback");
    if (!readbackResult) {
        return Error("MidVoxelGpuGenPoc: readback buffer init failed: {}", readbackResult.error());
    }

    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocator));
    if (FAILED(hr)) {
        return Error("MidVoxelGpuGenPoc: CreateCommandAllocator failed: 0x{:08X}",
            static_cast<uint32_t>(hr));
    }
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocator.Get(), nullptr,
        IID_PPV_ARGS(&m_cmdList));
    if (FAILED(hr)) {
        return Error("MidVoxelGpuGenPoc: CreateCommandList failed: 0x{:08X}",
            static_cast<uint32_t>(hr));
    }
    m_cmdList->Close();

    return {};
}

void MidVoxelGpuGenPoc::RunParityCheck(
    ID3D12Device* device,
    DX12CommandQueue& commandQueue,
    const Simulation::SparseClipmapPolicy& policy)
{
    if (!IsValid()) {
        spdlog::error("[MIDGEN-POC] harness not initialized; skipping parity check");
        return;
    }

    const uint32_t seed = policy.Config().seed;
    const auto rings = policy.BuildRings();
    if (rings.empty()) {
        spdlog::error("[MIDGEN-POC] policy produced no rings (clipmap disabled?); skipping");
        return;
    }

    // Real pristine brick generator: a fresh, UNEDITED cache so the edited-overlay
    // branches in GenerateVoxelBrickPayload are inert (hasEditedOverlays == false).
    // GPU output is compared against this real brick (incl. VisualSurface bits).
    Simulation::SparseClipmapTileCache cpuCache;
    if (!cpuCache.Initialize(policy.Config())) {
        spdlog::error("[MIDGEN-POC] failed to init reference SparseClipmapTileCache; skipping");
        return;
    }

    // Test coords spanning classifier branches. {ring, x, y, z} in BRICK units.
    // origin = Floor(coord * brickWorldSize), brickWorldSize = cellSize * 16.
    // For the default config (minCellSize=16) ring 0 -> cellSize 16, brick spans
    // 256 world units; brick y-index k covers world Y [256k, 256k+255].
    struct TestCoord {
        Simulation::SparseVoxelClipmapCoord coord;
        const char* label;
    };
    const std::array<TestCoord, 6> testCoords = {{
        { { 0,  0,  0,  0 }, "near-origin flat (world Y 0..255)" },
        { { 0,  2,  1,  2 }, "mountainous (off-origin, mid altitude)" },
        { { 0,  0, -1,  0 }, "shoreline near sea level (world Y -256..-1)" },
        { { 0,  6, -1,  0 }, "submerged shelf (far X, below sea level band)" },
        { { 0, 18,  2, 14 }, "far backdrop (distant silhouette ridge)" },
        { { 0,  0, -2,  0 }, "bedrock floor (very low Y, -512..-257)" },
    }};

    uint32_t matches = 0;
    for (const auto& tc : testCoords) {
        const int32_t ringIndex = tc.coord.ring;
        if (ringIndex < 0 || static_cast<size_t>(ringIndex) >= rings.size()) {
            spdlog::warn("[MIDGEN-POC] '{}': ring {} out of range; skipping", tc.label, ringIndex);
            continue;
        }
        // ---- CPU reference: the REAL pristine resident brick ----
        std::vector<uint32_t> cpuBrickVec;
        int32_t originX = 0;
        int32_t originY = 0;
        int32_t originZ = 0;
        int32_t cellSize = 1;
        if (!cpuCache.GenerateVoxelBrickPayloadForTest(
                tc.coord, policy, cpuBrickVec, originX, originY, originZ, cellSize) ||
            cpuBrickVec.size() != kBrickVoxelCount) {
            spdlog::error("[MIDGEN-POC] '{}': real brick generation failed; skipping", tc.label);
            continue;
        }
        const uint32_t* cpuBrick = cpuBrickVec.data();

        // ---- GPU dispatch ----
        m_cmdAllocator->Reset();
        m_cmdList->Reset(m_cmdAllocator.Get(), nullptr);

        m_outSamples.TransitionTo(m_cmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_pipeline.Bind(m_cmdList.Get());

        GenParams params{};
        params.originX = originX;
        params.originY = originY;
        params.originZ = originZ;
        params.cellSize = cellSize;
        params.seed = seed;
        params.brickCount = 1u;
        params.pad0 = 0u;
        params.pad1 = 0u;
        m_pipeline.SetRoot32BitConstants(
            m_cmdList.Get(), 0, sizeof(params) / 4u, &params);

        // Root UAV bound directly by GPU virtual address (root param index 1).
        m_cmdList->SetComputeRootUnorderedAccessView(
            1, m_outSamples.GetGPUVirtualAddress());

        // 4096 voxels / 64 threads per group -> 1 group (shader strides anyway).
        m_pipeline.Dispatch(m_cmdList.Get(), 1, 1, 1);

        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = m_outSamples.GetResource();
        m_cmdList->ResourceBarrier(1, &uavBarrier);

        m_outSamples.TransitionTo(m_cmdList.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_cmdList->CopyBufferRegion(
            m_readback.GetResource(), 0,
            m_outSamples.GetResource(), 0,
            static_cast<uint64_t>(kBrickVoxelCount) * sizeof(uint32_t));

        m_cmdList->Close();
        ID3D12CommandList* lists[] = { m_cmdList.Get() };
        commandQueue.ExecuteCommandLists(lists, 1);
        const uint64_t fence = commandQueue.Signal();
        commandQueue.WaitForFenceValue(fence);

        // ---- Readback + byte compare ----
        const uint32_t* gpuBrick = static_cast<const uint32_t*>(m_readback.Map());
        if (!gpuBrick) {
            spdlog::error("[MIDGEN-POC] '{}': failed to map readback buffer", tc.label);
            continue;
        }

        bool match = true;
        uint32_t firstDiff = 0;
        for (uint32_t i = 0; i < kBrickVoxelCount; ++i) {
            if (gpuBrick[i] != cpuBrick[i]) {
                match = false;
                firstDiff = i;
                break;
            }
        }

        if (match) {
            ++matches;
            spdlog::info(
                "[MIDGEN-POC] coord(ring={},x={},y={},z={}) origin=({},{},{}) cell={} "
                "match=true  ({}) [{}]",
                tc.coord.ring, tc.coord.x, tc.coord.y, tc.coord.z,
                originX, originY, originZ, cellSize, "all 4096 voxels equal", tc.label);
        } else {
            const uint32_t cpuV = cpuBrick[firstDiff];
            const uint32_t gpuV = gpuBrick[firstDiff];
            spdlog::error(
                "[MIDGEN-POC] coord(ring={},x={},y={},z={}) origin=({},{},{}) cell={} "
                "match=FALSE firstDiffIdx={} cpu=0x{:08X}(mat={}) gpu=0x{:08X}(mat={}) [{}]",
                tc.coord.ring, tc.coord.x, tc.coord.y, tc.coord.z,
                originX, originY, originZ, cellSize,
                firstDiff,
                cpuV, static_cast<uint32_t>(Utils::UnpackMaterial(cpuV)),
                gpuV, static_cast<uint32_t>(Utils::UnpackMaterial(gpuV)),
                tc.label);
        }

        m_readback.Unmap();
    }

    spdlog::info("[MIDGEN-POC] parity check complete: {}/{} bricks matched",
        matches, static_cast<uint32_t>(testCoords.size()));
    (void)device;
}

void MidVoxelGpuGenPoc::Shutdown() {
    m_cmdList.Reset();
    m_cmdAllocator.Reset();
    m_readback.Shutdown();
    m_outSamples.Shutdown();
    m_pipeline.Shutdown();
}

} // namespace VENPOD::Graphics
