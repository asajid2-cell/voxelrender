// =============================================================================
// VENPOD Phase-1 POC: GPU mid-voxel BATCH generation parity harness (dev-only).
// =============================================================================

#include "MidVoxelGpuGenPoc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "RHI/DX12CommandQueue.h"
#include "../Simulation/SparseVoxelTypes.h"
#include "../Simulation/TerrainConstants.h"
#include "../Utils/BitPacking.h"

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kBrickVoxelCount = 4096u;   // 16^3 == SPARSE_BRICK_VOXEL_COUNT

} // namespace

Result<void> MidVoxelGpuGenPoc::Initialize(
    ID3D12Device* device,
    ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath,
    uint32_t seed)
{
    if (!device) {
        return Error("MidVoxelGpuGenPoc: null device");
    }

    auto genResult = m_generator.Initialize(device, shaderCompiler, shaderPath, seed);
    if (!genResult) {
        return Error("MidVoxelGpuGenPoc: generator init failed: {}", genResult.error());
    }

    const uint64_t poolBytes =
        static_cast<uint64_t>(kMaxBatchSlots) * kBrickVoxelCount * sizeof(uint32_t);

    auto outResult = m_samplePool.Initialize(
        device,
        poolBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "MidVoxelGpuGenPoc_SamplePool");
    if (!outResult) {
        return Error("MidVoxelGpuGenPoc: sample pool init failed: {}", outResult.error());
    }

    auto readbackResult = m_readback.Initialize(
        device,
        poolBytes,
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

    const auto rings = policy.BuildRings();
    if (rings.empty()) {
        spdlog::error("[MIDGEN-POC] policy produced no rings (clipmap disabled?); skipping");
        return;
    }

    // Real pristine brick generator: a fresh, UNEDITED cache so the edited-overlay
    // branches in GenerateVoxelBrickPayload are inert (hasEditedOverlays == false).
    Simulation::SparseClipmapTileCache cpuCache;
    if (!cpuCache.Initialize(policy.Config())) {
        spdlog::error("[MIDGEN-POC] failed to init reference SparseClipmapTileCache; skipping");
        return;
    }

    // Test coords spanning classifier branches. {ring, x, y, z} in BRICK units.
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

    // ---- Build the BATCH of requests + the CPU reference bricks per dest slot ----
    std::vector<MidVoxelBrickGenRequest> requests;
    std::vector<std::vector<uint32_t>> cpuBricks;   // indexed by dest slot
    std::vector<const char*> slotLabels;
    requests.reserve(testCoords.size());
    cpuBricks.reserve(testCoords.size());

    for (const auto& tc : testCoords) {
        const int32_t ringIndex = tc.coord.ring;
        if (ringIndex < 0 || static_cast<size_t>(ringIndex) >= rings.size()) {
            spdlog::warn("[MIDGEN-POC] '{}': ring {} out of range; skipping", tc.label, ringIndex);
            continue;
        }
        std::vector<uint32_t> cpuBrickVec;
        int32_t originX = 0, originY = 0, originZ = 0, cellSize = 1;
        if (!cpuCache.GenerateVoxelBrickPayloadForTest(
                tc.coord, policy, cpuBrickVec, originX, originY, originZ, cellSize) ||
            cpuBrickVec.size() != kBrickVoxelCount) {
            spdlog::error("[MIDGEN-POC] '{}': real brick generation failed; skipping", tc.label);
            continue;
        }
        if (requests.size() >= kMaxBatchSlots) {
            spdlog::warn("[MIDGEN-POC] batch slot cap reached; truncating");
            break;
        }
        MidVoxelBrickGenRequest req{};
        req.originX = originX;
        req.originY = originY;
        req.originZ = originZ;
        req.cellSize = cellSize;
        req.destSlot = static_cast<uint32_t>(requests.size());  // unique slot per request
        requests.push_back(req);
        cpuBricks.push_back(std::move(cpuBrickVec));
        slotLabels.push_back(tc.label);
    }

    if (requests.empty()) {
        spdlog::error("[MIDGEN-POC] no valid test coords; skipping");
        return;
    }

    // ---- Single batch dispatch into the sample pool, then read back ----
    m_cmdAllocator->Reset();
    m_cmdList->Reset(m_cmdAllocator.Get(), nullptr);

    const bool dispatched = m_generator.GenerateBricks(
        m_cmdList.Get(),
        m_samplePool,
        requests.data(),
        static_cast<uint32_t>(requests.size()),
        /*transitionSamplePool=*/true);
    if (!dispatched) {
        spdlog::error("[MIDGEN-POC] batch dispatch failed");
        m_cmdList->Close();
        return;
    }

    m_samplePool.TransitionTo(m_cmdList.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint64_t copyBytes =
        static_cast<uint64_t>(requests.size()) * kBrickVoxelCount * sizeof(uint32_t);
    m_cmdList->CopyBufferRegion(
        m_readback.GetResource(), 0,
        m_samplePool.GetResource(), 0,
        copyBytes);

    m_cmdList->Close();
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    commandQueue.ExecuteCommandLists(lists, 1);
    const uint64_t fence = commandQueue.Signal();
    commandQueue.WaitForFenceValue(fence);

    const uint32_t* gpuPool = static_cast<const uint32_t*>(m_readback.Map());
    if (!gpuPool) {
        spdlog::error("[MIDGEN-POC] failed to map readback buffer");
        return;
    }

    uint32_t matches = 0;
    for (uint32_t slot = 0; slot < requests.size(); ++slot) {
        const uint32_t* gpuBrick = gpuPool + static_cast<size_t>(slot) * kBrickVoxelCount;
        const uint32_t* cpuBrick = cpuBricks[slot].data();
        bool match = true;
        uint32_t firstDiff = 0;
        for (uint32_t i = 0; i < kBrickVoxelCount; ++i) {
            if (gpuBrick[i] != cpuBrick[i]) {
                match = false;
                firstDiff = i;
                break;
            }
        }
        const auto& req = requests[slot];
        if (match) {
            ++matches;
            spdlog::info(
                "[MIDGEN-POC] slot={} origin=({},{},{}) cell={} match=true "
                "(all 4096 voxels equal) [{}]",
                req.destSlot, req.originX, req.originY, req.originZ, req.cellSize,
                slotLabels[slot]);
        } else {
            const uint32_t cpuV = cpuBrick[firstDiff];
            const uint32_t gpuV = gpuBrick[firstDiff];
            spdlog::error(
                "[MIDGEN-POC] slot={} origin=({},{},{}) cell={} match=FALSE "
                "firstDiffIdx={} cpu=0x{:08X}(mat={}) gpu=0x{:08X}(mat={}) [{}]",
                req.destSlot, req.originX, req.originY, req.originZ, req.cellSize,
                firstDiff,
                cpuV, static_cast<uint32_t>(Utils::UnpackMaterial(cpuV)),
                gpuV, static_cast<uint32_t>(Utils::UnpackMaterial(gpuV)),
                slotLabels[slot]);
        }
    }

    m_readback.Unmap();

    spdlog::info("[MIDGEN-POC] BATCH parity check complete: {}/{} bricks matched",
        matches, static_cast<uint32_t>(requests.size()));
    (void)device;
}

void MidVoxelGpuGenPoc::Shutdown() {
    m_cmdList.Reset();
    m_cmdAllocator.Reset();
    m_readback.Shutdown();
    m_samplePool.Shutdown();
    m_generator.Shutdown();
}

} // namespace VENPOD::Graphics
