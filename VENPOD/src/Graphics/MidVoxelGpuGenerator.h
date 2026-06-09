#pragma once

// =============================================================================
// VENPOD Phase-1 GPU mid-voxel brick generator.
//
// Owns the CS_GenerateMidVoxelBricks compute PSO and a (triple-buffered) request
// upload buffer. Given a batch of BrickGenRequest entries from the CPU pump, it
// dispatches one thread group per brick, each writing its 4096-uint brick into
// the REAL mid-voxel sample pool (SparseVoxelGpuResources::m_midVoxelClipmapSamples)
// at request.destSlot * 4096. The CPU keeps owning residency / metadata / lookup;
// only the expensive voxel SAMPLES are produced on the GPU.
//
// Binding model (mirrors MidVoxelGpuGenPoc): root 32-bit constants (b0, seed +
// requestCount), a root SRV by GPU virtual address for the request buffer (t0),
// and a root UAV by GPU virtual address for the sample pool (u0). No descriptor
// heap is required.
//
// State-transition contract: the caller is responsible for nothing extra — this
// class transitions the sample pool COPY_DEST/SRV -> UAV before the dispatch and
// back to SRV after (BeginMidVoxelGeneration / EndMidVoxelGeneration), so the
// raymarch reads a freshly-generated, SRV-state buffer.
// =============================================================================

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <d3d12.h>

#include "RHI/DX12ComputePipeline.h"
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "../Utils/Result.h"

namespace VENPOD::Graphics {

class DX12CommandQueue;

// 32-byte request, matches BrickGenRequest in CS_GenerateMidVoxelBricks.hlsl.
struct MidVoxelBrickGenRequest {
    int32_t  originX;
    int32_t  originY;
    int32_t  originZ;
    int32_t  cellSize;
    uint32_t destSlot;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(MidVoxelBrickGenRequest) == 32, "BrickGenRequest must be 32 bytes");

class MidVoxelGpuGenerator {
public:
    MidVoxelGpuGenerator() = default;

    Result<void> Initialize(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath,
        uint32_t seed);

    void Shutdown();

    bool IsValid() const { return m_pipeline.IsValid(); }

    // Transition the sample pool from its current (COPY_DEST or SRV) state into
    // UNORDERED_ACCESS, ready for the generator dispatch.
    void BeginMidVoxelGeneration(
        ID3D12GraphicsCommandList* cmdList, GPUBuffer& samplePool);

    // Transition the sample pool from UNORDERED_ACCESS back to SRV (non-pixel |
    // pixel) so the raymarch can read it. Caller must add a UAV barrier first if
    // a same-list reader needs ordering; this also emits a UAV barrier.
    void EndMidVoxelGeneration(
        ID3D12GraphicsCommandList* cmdList, GPUBuffer& samplePool);

    // Upload `requests`, bind the sample pool UAV, and dispatch one group/brick.
    // The sample pool must already be in UNORDERED_ACCESS (call Begin first), or
    // pass transitionSamplePool=true to have this method bracket the dispatch with
    // Begin/End itself. Returns false if not initialized or requests is empty.
    bool GenerateBricks(
        ID3D12GraphicsCommandList* cmdList,
        GPUBuffer& samplePool,
        const MidVoxelBrickGenRequest* requests,
        uint32_t requestCount,
        bool transitionSamplePool);

    uint32_t Seed() const { return m_seed; }

    // Dev verification: dispatch `requests` into `liveSamplePool` (the REAL
    // m_midVoxelClipmapSamples buffer), read back each request's destSlot, and
    // byte-compare against `cpuBricks[i]` (each 4096 uints). Runs on its own
    // command list + a caller-provided queue; synchronous. Returns the number of
    // matching bricks. Proves the live pool + UAV + slot-offset math end-to-end.
    uint32_t VerifyLiveSamplePool(
        ID3D12Device* device,
        DX12CommandQueue& commandQueue,
        GPUBuffer& liveSamplePool,
        const std::vector<MidVoxelBrickGenRequest>& requests,
        const std::vector<std::vector<uint32_t>>& cpuBricks);

private:
    Result<void> EnsureRequestCapacity(ID3D12Device* device, uint32_t requestCount);

    static constexpr uint32_t kRequestRingSize = 3u;  // frames in flight

    DX12ComputePipeline m_pipeline;
    ComPtr<ID3D12Device> m_device;
    std::array<UploadBuffer, kRequestRingSize> m_requestBuffers;
    std::array<uint32_t, kRequestRingSize> m_requestCapacity = { 0u, 0u, 0u };
    uint32_t m_requestRingCursor = 0u;
    uint32_t m_seed = 12345u;
};

} // namespace VENPOD::Graphics
