#pragma once

// =============================================================================
// VENPOD Phase-1 GPU mid-voxel brick generation parity harness (dev-only).
//
// DEV-ONLY SCAFFOLDING. Gated by the env flag VENPOD_GPU_MIDGEN_POC in
// main_launcher.cpp; this class never runs in a normal session and is fully
// isolated from the render/sim loop.
//
// It exercises the REAL batch generator (MidVoxelGpuGenerator): it requests a
// BATCH of mid-voxel LOD coords into a sample pool (laid out exactly like the
// real m_midVoxelClipmapSamples: brick b -> [b*4096 .. b*4096+4095]) via a single
// Dispatch, reads each destination slot back, and byte-compares it against the
// CPU reference produced by SparseClipmap.cpp's pristine GenerateVoxelBrick-
// PayloadForTest. Goal: prove GPU == CPU brick-for-brick AT BATCH SCALE through
// the same generator the live pump uses.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <vector>

#include <d3d12.h>

#include "MidVoxelGpuGenerator.h"
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "../Simulation/SparseClipmap.h"
#include "../Utils/Result.h"

namespace VENPOD::Graphics {

class DX12CommandQueue;

class MidVoxelGpuGenPoc {
public:
    MidVoxelGpuGenPoc() = default;

    // Compile the CS, build the batch generator, and allocate the sample pool +
    // readback buffers. Safe to call once at startup behind the env flag.
    Result<void> Initialize(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath,
        uint32_t seed);

    void Shutdown();

    // Run the GPU-vs-CPU parity check over an internal set of test coords that
    // span the terrain classifier branches, dispatched as a single BATCH into the
    // sample pool. Results are reported via spdlog.
    void RunParityCheck(
        ID3D12Device* device,
        DX12CommandQueue& commandQueue,
        const Simulation::SparseClipmapPolicy& policy);

    bool IsValid() const { return m_generator.IsValid(); }

private:
    static constexpr uint32_t kMaxBatchSlots = 16u;

    MidVoxelGpuGenerator m_generator;
    GPUBuffer m_samplePool;   // RWStructuredBuffer<uint>, kMaxBatchSlots * 4096 (UAV)
    GPUBuffer m_readback;     // CPU-visible copy of m_samplePool
    ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
};

} // namespace VENPOD::Graphics
