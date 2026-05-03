#pragma once

#include "RHI/DescriptorHeap.h"
#include "RHI/GPUBuffer.h"
#include "Simulation/SparseSurfaceCache.h"
#include "Utils/Result.h"

#include <array>
#include <cstdint>

namespace VENPOD::Graphics {

struct SparseSurfaceGpuConfig {
    uint32_t maxFaces = 1u << 20;
    uint32_t maxBrickRanges = 16384;
    uint32_t uploadRingSlots = 3;
    uint32_t uploadBytesPerSlot = 8 * 1024 * 1024;
};

struct SparseSurfaceGpuStats {
    bool initialized = false;
    uint32_t maxFaces = 0;
    uint32_t maxBrickRanges = 0;
    uint32_t uploadedFaces = 0;
    uint32_t uploadedRanges = 0;
    uint32_t uploadedRangeTableCapacity = 0;
    uint32_t uploadedSerial = 0;
    uint32_t uploadedCandidateBricks = 0;
    uint32_t uploadedVisibleBricks = 0;
    uint32_t uploadedCulledBricks = 0;
    uint32_t stagedFacesLastFrame = 0;
    uint32_t stagedRangesLastFrame = 0;
    uint32_t stagedRangeTableCapacityLastFrame = 0;
    uint32_t stagedCandidateBricksLastFrame = 0;
    uint32_t stagedVisibleBricksLastFrame = 0;
    uint32_t stagedCulledBricksLastFrame = 0;
    uint64_t stagedBytesLastFrame = 0;
    bool uploadOverflowLastFrame = false;
};

struct SparseSurfaceUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint64_t faceUploadOffset = 0;
    uint64_t rangeUploadOffset = 0;
    uint64_t faceBytes = 0;
    uint64_t rangeBytes = 0;
    uint32_t faceCount = 0;
    uint32_t rangeCount = 0;
    uint32_t rangeTableCapacity = 0;
    uint32_t serial = 0;
    uint32_t candidateBricks = 0;
    uint32_t visibleBricks = 0;
    uint32_t culledBricks = 0;
};

class SparseSurfaceGpuResources {
public:
    SparseSurfaceGpuResources() = default;
    ~SparseSurfaceGpuResources();

    SparseSurfaceGpuResources(const SparseSurfaceGpuResources&) = delete;
    SparseSurfaceGpuResources& operator=(const SparseSurfaceGpuResources&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        const SparseSurfaceGpuConfig& config = {});
    void Shutdown();

    void BeginFrame(uint32_t frameIndex);
    bool StageSnapshot(
        const Simulation::SparseSurfaceGpuSnapshot& snapshot,
        SparseSurfaceUploadTicket* outTicket = nullptr);
    bool EmitCopy(ID3D12GraphicsCommandList* commandList, const SparseSurfaceUploadTicket& ticket);

    bool IsInitialized() const { return m_stats.initialized; }
    const SparseSurfaceGpuStats& GetStats() const { return m_stats; }
    const DescriptorHandle& FaceBufferSRV() const { return m_faceBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& RangeBufferSRV() const { return m_rangeBuffer.GetShaderVisibleSRV(); }

private:
    SparseSurfaceGpuConfig m_config;
    SparseSurfaceGpuStats m_stats;
    GPUBuffer m_faceBuffer;
    GPUBuffer m_rangeBuffer;
    std::array<UploadBuffer, 3> m_uploadRing;
    uint32_t m_activeUploadSlot = 0;
    uint64_t m_uploadWriteOffset = 0;
};

} // namespace VENPOD::Graphics
