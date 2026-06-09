#pragma once

// =============================================================================
// VENPOD Phase-0 POC: GPU mid-voxel brick generation parity harness.
//
// DEV-ONLY SCAFFOLDING. Gated by the env flag VENPOD_GPU_MIDGEN_POC in
// main_launcher.cpp; this class never runs in a normal session and is fully
// isolated from the render/sim loop.
//
// It dispatches assets/shaders/Compute/CS_GenerateMidVoxelBricks.hlsl for a set
// of mid-voxel LOD coords, reads the 4096-uint brick back, and byte-compares it
// against a CPU reference produced by replicating SparseClipmap.cpp's
// `sampleColumnCellVoxel` rule using the public SparseTerrainGenerator API
// (seeded identically to the live clipmap). Goal: prove GPU == CPU brick-for-
// brick for the procedural per-cell sample.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <vector>

#include <d3d12.h>

#include "RHI/DX12ComputePipeline.h"
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "../Simulation/SparseClipmap.h"
#include "../Utils/Result.h"

namespace VENPOD::Graphics {

class DX12CommandQueue;

class MidVoxelGpuGenPoc {
public:
    MidVoxelGpuGenPoc() = default;

    // Compile the CS, build the compute PSO, and allocate the UAV + readback
    // buffers. Safe to call once at startup behind the env flag.
    Result<void> Initialize(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath);

    void Shutdown();

    // Run the GPU-vs-CPU parity check over an internal set of test coords that
    // span the terrain classifier branches. Results are reported via spdlog.
    // The policy supplies ring cellSize + seed exactly as the live clipmap uses.
    void RunParityCheck(
        ID3D12Device* device,
        DX12CommandQueue& commandQueue,
        const Simulation::SparseClipmapPolicy& policy);

    bool IsValid() const { return m_pipeline.IsValid(); }

private:
    DX12ComputePipeline m_pipeline;
    GPUBuffer m_outSamples;   // RWStructuredBuffer<uint>, 4096 uints (UAV)
    GPUBuffer m_readback;     // CPU-visible copy of m_outSamples
    ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
};

} // namespace VENPOD::Graphics
