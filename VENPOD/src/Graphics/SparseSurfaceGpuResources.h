#pragma once

#include "RHI/DescriptorHeap.h"
#include "RHI/DX12ComputePipeline.h"
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "Simulation/SparseSurfaceCache.h"
#include "Simulation/SparseSurfaceRangeAllocator.h"
#include "Utils/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Graphics {

struct SparseSurfaceGpuConfig {
    uint32_t maxFaces = 1u << 20;
    uint32_t maxBrickRanges = 16384;
    uint32_t maxDrawCommands = 16384;
    uint32_t uploadRingSlots = 3;
    uint32_t uploadBytesPerSlot = 8 * 1024 * 1024;
    uint32_t maxPayloadCopyRegionsPerFrame = 128;
    uint32_t maxPayloadCopyFacesPerFrame = 256 * 1024;
    uint32_t rangeRetirementDelayFrames = 3;
    uint32_t surfaceRecordsPerCluster = 32;
    uint32_t surfaceClusterMaxExtentVoxels = 128;
    uint32_t surfaceClusterFastAcceptMaxRecords = 8;
    uint32_t surfaceClusterFastAcceptMaxFaces = 2048;
    bool useRangeAllocator = true;
    bool useFixedRangeTable = true;
    bool useStableDrawSlots = true;
    bool compactStableDrawCommands = true;
    bool useGpuCull = true;
};

struct SparseSurfaceGpuStats {
    bool initialized = false;
    bool rangeAllocatorEnabled = false;
    uint32_t maxFaces = 0;
    uint32_t maxBrickRanges = 0;
    uint32_t maxDrawCommands = 0;
    uint32_t uploadedFaces = 0;
    uint32_t uploadedRanges = 0;
    uint32_t uploadedRangeTableCapacity = 0;
    uint32_t uploadedDrawCommands = 0;
    uint32_t uploadedActiveDrawCommands = 0;
    uint32_t uploadedSerial = 0;
    uint32_t uploadedCandidateBricks = 0;
    uint32_t uploadedVisibleBricks = 0;
    uint32_t uploadedCulledBricks = 0;
    uint32_t stagedFacesLastFrame = 0;
    uint32_t stagedRangesLastFrame = 0;
    uint32_t stagedRangeTableCapacityLastFrame = 0;
    uint32_t stagedDrawCommandsLastFrame = 0;
    uint32_t stagedRangeCopyRegionsLastFrame = 0;
    uint32_t stagedDrawCopyRegionsLastFrame = 0;
    uint32_t skippedCleanRangeSlotsLastFrame = 0;
    uint32_t skippedCleanDrawCommandsLastFrame = 0;
    bool fullRangeTableUploadLastFrame = false;
    bool fullDrawArgsUploadLastFrame = false;
    bool fixedRangeTableEnabled = false;
    bool stableDrawSlotsEnabled = false;
    bool compactStableDrawCommandsEnabled = false;
    bool gpuCullEnabled = false;
    uint32_t iaStreamCapacityFaces = 0;
    uint32_t iaStreamVertexCount = 0;
    uint32_t iaStreamIndexCount = 0;
    uint64_t iaStreamVertexBytes = 0;
    uint64_t iaStreamIndexBytes = 0;
    bool iaStreamGpuLocal = false;
    bool iaStreamUploadPending = false;
    uint64_t iaStreamUploadRetireFence = 0;
    uint32_t activeDrawCommandsLastFrame = 0;
    uint32_t stableDrawSlotCapacity = 0;
    uint32_t stableDrawFreeSlots = 0;
    uint32_t inactiveStableDrawSlotsLastFrame = 0;
    uint32_t uploadedSurfaceRecords = 0;
    uint32_t stagedSurfaceRecordsLastFrame = 0;
    uint32_t stagedSurfaceRecordCopyRegionsLastFrame = 0;
    uint32_t skippedCleanSurfaceRecordsLastFrame = 0;
    bool fullSurfaceRecordUploadLastFrame = false;
    uint32_t gpuCullDispatchesLastFrame = 0;
    uint32_t gpuCullCandidateRecordsLastFrame = 0;
    uint32_t uploadedSurfaceClusters = 0;
    uint32_t stagedSurfaceClustersLastFrame = 0;
    uint32_t stagedSurfaceClusterCopyRegionsLastFrame = 0;
    uint32_t skippedCleanSurfaceClustersLastFrame = 0;
    bool fullSurfaceClusterUploadLastFrame = false;
    uint32_t gpuCullCandidateClustersLastFrame = 0;
    uint32_t gpuCullRejectedClusters = 0;
    uint32_t gpuCullFastAcceptedClusterRecords = 0;
    uint32_t gpuCullAcceptedClusterDraws = 0;
    uint32_t gpuCullAcceptedRecordDraws = 0;
    uint32_t gpuCullMaxDrawCommands = 0;
    uint32_t gpuCullAcceptedDraws = 0;
    uint32_t gpuCullRejectedInvalid = 0;
    uint32_t gpuCullRejectedDistance = 0;
    uint32_t gpuCullRejectedFrustum = 0;
    uint32_t gpuCullRejectedBackface = 0;
    uint32_t gpuCullOverflow = 0;
    uint32_t gpuCullStatsReadbacksQueued = 0;
    uint32_t gpuCullStatsReadbacksRetired = 0;
    uint32_t gpuCullStatsReadbackPending = 0;
    bool gpuCullStatsValid = false;
    uint32_t stagedCandidateBricksLastFrame = 0;
    uint32_t stagedVisibleBricksLastFrame = 0;
    uint32_t stagedCulledBricksLastFrame = 0;
    uint32_t stagedFaceCopyRegionsLastFrame = 0;
    uint32_t stagedPayloadPatchBricksLastFrame = 0;
    uint32_t stagedPayloadPatchFacesLastFrame = 0;
    uint32_t stagedPayloadPatchRegionsLastFrame = 0;
    uint32_t stagedDirtyPayloadBricksLastFrame = 0;
    uint32_t skippedCleanPayloadBricksLastFrame = 0;
    uint32_t deferredPayloadBricksLastFrame = 0;
    uint32_t payloadCopyRegionBudget = 0;
    uint32_t payloadCopyFaceBudget = 0;
    uint32_t surfaceRecordsPerCluster = 0;
    uint32_t surfaceClusterMaxExtentVoxels = 0;
    uint32_t surfaceClusterFastAcceptMaxRecords = 0;
    uint32_t surfaceClusterFastAcceptMaxFaces = 0;
    uint32_t residentPayloadBricks = 0;
    uint32_t pendingDirtyBricksLastFrame = 0;
    uint32_t pendingRemovedBricksLastFrame = 0;
    uint32_t allocatedFaceRanges = 0;
    uint32_t allocatedFaceCapacity = 0;
    uint32_t freeFaceRanges = 0;
    uint32_t largestFreeFaceRange = 0;
    uint32_t pendingRetiredFaceRanges = 0;
    uint32_t pendingRetiredFaceCapacity = 0;
    uint32_t faceRangeAllocationFailures = 0;
    uint64_t stagedBytesLastFrame = 0;
    bool uploadOverflowLastFrame = false;
};

struct SparseSurfaceFaceCopyRegion {
    uint64_t uploadOffset = 0;
    uint32_t destFirstFace = 0;
    uint32_t faceCount = 0;
};

struct SparseSurfaceBufferCopyRegion {
    uint64_t uploadOffset = 0;
    uint64_t destOffset = 0;
    uint64_t byteCount = 0;
};

struct SparseSurfacePayloadMirrorUpdate {
    Simulation::BrickCoord coord;
    std::vector<Simulation::SparseSurfaceFace> faces;
};

struct SparseSurfaceUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint64_t faceUploadOffset = 0;
    uint64_t rangeUploadOffset = 0;
    uint64_t drawArgsUploadOffset = 0;
    uint64_t surfaceRecordUploadOffset = 0;
    uint64_t faceBytes = 0;
    uint64_t rangeBytes = 0;
    uint64_t drawArgsBytes = 0;
    uint64_t surfaceRecordBytes = 0;
    uint32_t faceCount = 0;
    uint32_t rangeCount = 0;
    uint32_t rangeTableCapacity = 0;
    uint32_t drawCommandCount = 0;
    uint32_t activeDrawCommandCount = 0;
    uint32_t serial = 0;
    uint32_t candidateBricks = 0;
    uint32_t visibleBricks = 0;
    uint32_t culledBricks = 0;
    uint32_t deferredPayloadBricks = 0;
    std::vector<Simulation::BrickCoord> uploadedPayloadBricks;
    std::vector<Simulation::BrickCoord> removedBricks;
    std::vector<SparseSurfacePayloadMirrorUpdate> payloadMirrorUpdates;
    std::vector<SparseSurfaceFaceCopyRegion> faceCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> rangeCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> drawArgsCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> surfaceRecordCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> surfaceClusterCopyRegions;
    std::vector<Simulation::SparseSurfaceBrickRange> rangeMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceDrawArgs> drawArgsMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceRecord> surfaceRecordMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceClusterRecord> surfaceClusterMirrorAfterCopy;
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> drawSlotByCoordAfterCopy;
    std::vector<uint8_t> drawSlotOccupiedAfterCopy;
    std::vector<uint32_t> freeDrawSlotsAfterCopy;
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
        ShaderCompiler* shaderCompiler = nullptr,
        const std::filesystem::path& shaderPath = {},
        const SparseSurfaceGpuConfig& config = {});
    void Shutdown();

    void BeginFrame(
        uint32_t frameIndex,
        uint64_t completedFenceValue = 0,
        uint64_t currentFrameFenceValue = 0);
    bool StageSnapshot(
        const Simulation::SparseSurfaceGpuSnapshot& snapshot,
        SparseSurfaceUploadTicket* outTicket = nullptr);
    bool EmitCopy(ID3D12GraphicsCommandList* commandList, const SparseSurfaceUploadTicket& ticket);
    bool DispatchGpuCull(
        ID3D12GraphicsCommandList* commandList,
        float cameraX,
        float cameraY,
        float cameraZ,
        float forwardX,
        float forwardY,
        float forwardZ,
        float rightX,
        float rightY,
        float rightZ,
        float upX,
        float upY,
        float upZ,
        float fovYRadians,
        float aspectRatio,
        float maxDistance,
        float padding);
    void QueueGpuCullStatsReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetireGpuCullStatsReadback(uint32_t frameIndex);

    bool IsInitialized() const { return m_stats.initialized; }
    const SparseSurfaceGpuStats& GetStats() const { return m_stats; }
    const DescriptorHandle& FaceBufferSRV() const { return m_faceBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& RangeBufferSRV() const { return m_rangeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& SurfaceRecordSRV() const { return m_surfaceRecordBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& SurfaceClusterSRV() const { return m_surfaceClusterBuffer.GetShaderVisibleSRV(); }
    ID3D12Resource* DrawArgsResource() const { return m_drawArgsBuffer.GetResource(); }
    ID3D12Resource* DrawCountResource() const { return m_drawCountBuffer.GetResource(); }
    const D3D12_VERTEX_BUFFER_VIEW& VertexIdBufferView() const { return m_vertexIdBufferView; }
    const D3D12_INDEX_BUFFER_VIEW& IndexBufferView() const { return m_indexBufferView; }
    uint32_t VertexIdCapacityFaces() const { return m_vertexIdCapacityFaces; }
    bool IsGpuCullEnabled() const { return m_config.useGpuCull && m_surfaceCullPipeline.IsValid(); }

private:
    Result<void> CreateVertexIdStream(ID3D12Device* device);

    SparseSurfaceGpuConfig m_config;
    SparseSurfaceGpuStats m_stats;
    GPUBuffer m_faceBuffer;
    GPUBuffer m_rangeBuffer;
    GPUBuffer m_drawArgsBuffer;
    GPUBuffer m_surfaceRecordBuffer;
    GPUBuffer m_surfaceClusterBuffer;
    GPUBuffer m_drawCountBuffer;
    std::array<GPUBuffer, 3> m_cullStatsReadback;
    std::array<bool, 3> m_cullStatsReadbackPending = {};
    std::array<UploadBuffer, 3> m_uploadRing;
    std::array<UploadBuffer, 3> m_cullConstantUploads;
    GPUBuffer m_vertexIdStream;
    GPUBuffer m_indexStream;
    UploadBuffer m_vertexIdStreamUpload;
    UploadBuffer m_indexStreamUpload;
    D3D12_VERTEX_BUFFER_VIEW m_vertexIdBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    uint32_t m_vertexIdCapacityFaces = 0;
    bool m_staticIaUploadPending = false;
    bool m_staticIaUploadComplete = false;
    uint64_t m_staticIaUploadFence = 0;
    uint64_t m_currentFrameFenceValue = 0;
    DX12ComputePipeline m_surfaceCullPipeline;
    CompiledShader m_surfaceCullShader;
    DescriptorHeapManager* m_heapManager = nullptr;
    Simulation::SparseSurfaceRangeAllocator m_faceRangeAllocator;
    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> m_payloadResidentCoords;
    std::vector<Simulation::SparseSurfaceBrickRange> m_rangeMirror;
    std::vector<Simulation::SparseSurfaceDrawArgs> m_drawArgsMirror;
    std::vector<Simulation::SparseSurfaceRecord> m_surfaceRecordMirror;
    std::vector<Simulation::SparseSurfaceClusterRecord> m_surfaceClusterMirror;
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> m_drawSlotByCoord;
    std::unordered_map<
        Simulation::BrickCoord,
        std::vector<Simulation::SparseSurfaceFace>,
        Simulation::BrickCoordHash> m_payloadFaceMirrorByCoord;
    std::vector<uint8_t> m_drawSlotOccupied;
    std::vector<uint32_t> m_freeDrawSlots;
    uint32_t m_activeUploadSlot = 0;
    uint64_t m_uploadWriteOffset = 0;
};

} // namespace VENPOD::Graphics
