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
#include <limits>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VENPOD::Graphics {

struct SparseSurfaceIaStreamSizing {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
};

inline bool TryBuildSparseSurfaceIaStreamSizing(
    uint32_t maxFaces,
    SparseSurfaceIaStreamSizing& outSizing)
{
    outSizing = {};
    constexpr uint32_t verticesPerFace = 4u;
    constexpr uint32_t indicesPerFace = 6u;
    constexpr uint32_t elementBytes = static_cast<uint32_t>(sizeof(uint32_t));
    constexpr uint32_t maxViewBytes = std::numeric_limits<uint32_t>::max();

    if (maxFaces == 0u) {
        return false;
    }
    if (maxFaces > std::numeric_limits<uint32_t>::max() / indicesPerFace ||
        maxFaces > std::numeric_limits<uint32_t>::max() / verticesPerFace) {
        return false;
    }
    if (maxFaces > maxViewBytes / (verticesPerFace * elementBytes) ||
        maxFaces > maxViewBytes / (indicesPerFace * elementBytes)) {
        return false;
    }

    outSizing.vertexCount = maxFaces * verticesPerFace;
    outSizing.indexCount = maxFaces * indicesPerFace;
    outSizing.vertexBytes = static_cast<uint64_t>(outSizing.vertexCount) * elementBytes;
    outSizing.indexBytes = static_cast<uint64_t>(outSizing.indexCount) * elementBytes;
    return true;
}

struct SparseSurfaceGpuConfig {
    // Aggressive variable-dt movement can keep more than 3.3M exact sparse
    // surface faces visible while small dirty/removal updates fragment the
    // range allocator. Keep enough contiguous slack for atomic old->new
    // surface replacement instead of exposing allocation-overflow frames.
    uint32_t maxFaces = 1u << 23;
    uint32_t maxBrickRanges = 32768;
    uint32_t maxDrawCommands = 65535;
    uint32_t uploadRingSlots = 3;
    uint32_t uploadBytesPerSlot = 32 * 1024 * 1024;
    uint32_t maxPayloadCopyRegionsPerFrame = 512;
    uint32_t maxPayloadCopyFacesPerFrame = 1024 * 1024;
    uint32_t rangeRetirementDelayFrames = 3;
    uint32_t surfaceRecordsPerCluster = 32;
    uint32_t surfaceClusterMaxExtentVoxels = 128;
    // Cluster fast-accept can expose partial exact-surface groups as detached
    // foreground bands. Keep GPU culling per-record unless a safer grouped
    // visibility contract is added.
    uint32_t surfaceClusterFastAcceptMaxRecords = 0;
    uint32_t surfaceClusterFastAcceptMaxFaces = 0;
    bool useRangeAllocator = true;
    bool useFixedRangeTable = true;
    bool useStableDrawSlots = true;
    bool compactStableDrawCommands = true;
    bool useGpuCull = true;
    bool incrementalMetadataAdds = false;
};

inline bool IsSparseSurfaceGpuPowerOfTwo(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

inline bool ValidateSparseSurfaceGpuConfigForStats(const SparseSurfaceGpuConfig& config) {
    constexpr uint32_t maxUploadRingSlots = 3u;
    constexpr uint32_t maxUploadBytesPerSlot = 256u * 1024u * 1024u;
    constexpr uint32_t maxSurfaceFaces = 1u << 23;
    constexpr uint32_t maxSurfaceBrickRanges = 1u << 20;
    constexpr uint32_t maxSurfaceRecordsPerCluster = 64u;
    constexpr uint32_t maxSurfaceClusterExtentVoxels = 4096u;
    constexpr uint32_t maxGpuCullDispatchGroups = 65535u;

    SparseSurfaceIaStreamSizing ignoredSizing;
    if (!TryBuildSparseSurfaceIaStreamSizing(config.maxFaces, ignoredSizing)) {
        return false;
    }
    if (config.maxFaces > maxSurfaceFaces) {
        return false;
    }
    if (config.maxBrickRanges == 0u || config.maxDrawCommands == 0u) {
        return false;
    }
    if (config.maxBrickRanges > maxSurfaceBrickRanges) {
        return false;
    }
    if (config.maxDrawCommands > maxGpuCullDispatchGroups) {
        return false;
    }
    if (config.useFixedRangeTable &&
        config.maxBrickRanges > config.maxDrawCommands * 2u) {
        return false;
    }
    if (config.uploadRingSlots == 0u || config.uploadRingSlots > maxUploadRingSlots) {
        return false;
    }
    if (config.uploadBytesPerSlot == 0u) {
        return false;
    }
    if (config.uploadBytesPerSlot > maxUploadBytesPerSlot) {
        return false;
    }
    if (config.useFixedRangeTable && !IsSparseSurfaceGpuPowerOfTwo(config.maxBrickRanges)) {
        return false;
    }
    if (config.surfaceRecordsPerCluster == 0u ||
        config.surfaceRecordsPerCluster > maxSurfaceRecordsPerCluster) {
        return false;
    }
    if (config.surfaceClusterMaxExtentVoxels > maxSurfaceClusterExtentVoxels) {
        return false;
    }
    if (config.surfaceClusterFastAcceptMaxRecords > config.surfaceRecordsPerCluster) {
        return false;
    }
    if (config.surfaceClusterFastAcceptMaxFaces > config.maxFaces) {
        return false;
    }
    if (config.maxPayloadCopyRegionsPerFrame != 0u &&
        config.maxPayloadCopyRegionsPerFrame > config.maxDrawCommands) {
        return false;
    }
    if (config.maxPayloadCopyFacesPerFrame != 0u &&
        config.maxPayloadCopyFacesPerFrame > config.maxFaces) {
        return false;
    }
    return true;
}

inline bool IsSparseSurfaceCullStatsReadbackRetirable(
    bool pending,
    uint32_t queuedFrame,
    uint32_t retireFrame)
{
    return pending && queuedFrame != retireFrame;
}

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
    bool incrementalMetadataAddsEnabled = false;
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

struct SparseSurfaceGpuBrickDebugInfo {
    bool payloadResident = false;
    bool surfaceRecordPresent = false;
    bool drawSlotPresent = false;
    bool rangePresent = false;
    uint32_t payloadFaceCount = 0;
    uint32_t surfaceRecordFaceCount = 0;
    uint32_t surfaceRecordFirstFace = 0;
    uint32_t surfaceRecordFlags = 0;
    uint32_t surfaceRecordIndex = 0;
    uint32_t drawSlot = 0;
    uint32_t rangeFirstFace = 0;
    uint32_t rangeFaceCount = 0;
    uint32_t rangeFlags = 0;
};

struct SparseSurfaceGpuCullDebugInfo {
    bool hasRecord = false;
    bool hasCluster = false;
    uint32_t surfaceRecordIndex = 0;
    uint32_t clusterIndex = 0;
    uint32_t clusterClass = 0;
    uint32_t recordClass = 0;
    bool clusterRejected = false;
    bool recordRejected = false;
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

inline bool IsSparseSurfaceGpuByteRangeInBounds(
    uint64_t offset,
    uint64_t byteCount,
    uint64_t capacityBytes)
{
    return offset <= capacityBytes && byteCount <= capacityBytes - offset;
}

inline bool IsSparseSurfaceGpuFaceCopyRegionInBounds(
    const SparseSurfaceFaceCopyRegion& region,
    uint64_t uploadCapacityBytes,
    uint64_t faceCapacityBytes)
{
    const uint64_t faceSize = sizeof(Simulation::SparseSurfaceFace);
    if (region.faceCount > std::numeric_limits<uint64_t>::max() / faceSize ||
        region.destFirstFace > std::numeric_limits<uint64_t>::max() / faceSize) {
        return false;
    }
    const uint64_t byteCount = static_cast<uint64_t>(region.faceCount) * faceSize;
    const uint64_t destOffset = static_cast<uint64_t>(region.destFirstFace) * faceSize;
    return IsSparseSurfaceGpuByteRangeInBounds(region.uploadOffset, byteCount, uploadCapacityBytes) &&
        IsSparseSurfaceGpuByteRangeInBounds(destOffset, byteCount, faceCapacityBytes);
}

inline bool IsSparseSurfaceGpuBufferCopyRegionInBounds(
    const SparseSurfaceBufferCopyRegion& region,
    uint64_t uploadCapacityBytes,
    uint64_t destCapacityBytes)
{
    return IsSparseSurfaceGpuByteRangeInBounds(region.uploadOffset, region.byteCount, uploadCapacityBytes) &&
        IsSparseSurfaceGpuByteRangeInBounds(region.destOffset, region.byteCount, destCapacityBytes);
}

struct SparseSurfacePayloadMirrorUpdate {
    Simulation::BrickCoord coord;
    std::vector<Simulation::SparseSurfaceFace> faces;
};

struct SparseSurfaceRangeMirrorPatch {
    uint32_t index = 0;
    Simulation::SparseSurfaceBrickRange value;
};

struct SparseSurfaceDrawArgsMirrorPatch {
    uint32_t index = 0;
    Simulation::SparseSurfaceDrawArgs value;
};

struct SparseSurfaceRecordMirrorPatch {
    uint32_t index = 0;
    Simulation::SparseSurfaceRecord value;
};

struct SparseSurfaceClusterMirrorPatch {
    uint32_t index = 0;
    Simulation::SparseSurfaceClusterRecord value;
};

struct SparseSurfaceDrawSlotRetire {
    Simulation::BrickCoord coord;
    uint32_t slot = std::numeric_limits<uint32_t>::max();
};

struct SparseSurfaceDrawSlotAssign {
    Simulation::BrickCoord coord;
    uint32_t slot = std::numeric_limits<uint32_t>::max();
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
    bool payloadOnly = false;
    bool incrementalMetadataPatches = false;
    bool hasUploadWriteOffsetRollback = false;
    uint64_t uploadWriteOffsetBeforeStage = 0;
    std::vector<Simulation::BrickCoord> uploadedPayloadBricks;
    std::vector<Simulation::BrickCoord> removedBricks;
    std::vector<SparseSurfacePayloadMirrorUpdate> payloadMirrorUpdates;
    std::vector<SparseSurfaceFaceCopyRegion> faceCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> rangeCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> drawArgsCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> surfaceRecordCopyRegions;
    std::vector<SparseSurfaceBufferCopyRegion> surfaceClusterCopyRegions;
    std::vector<SparseSurfaceRangeMirrorPatch> rangeMirrorPatches;
    std::vector<SparseSurfaceDrawArgsMirrorPatch> drawArgsMirrorPatches;
    std::vector<SparseSurfaceRecordMirrorPatch> surfaceRecordMirrorPatches;
    std::vector<SparseSurfaceClusterMirrorPatch> surfaceClusterMirrorPatches;
    std::vector<SparseSurfaceDrawSlotRetire> drawSlotRetires;
    std::vector<SparseSurfaceDrawSlotAssign> drawSlotAssignments;
    uint32_t drawArgsMirrorSizeAfterPatch = 0;
    uint32_t drawSlotOccupiedSizeAfterPatch = 0;
    uint32_t surfaceRecordMirrorSizeAfterPatch = 0;
    uint32_t surfaceClusterMirrorSizeAfterPatch = 0;
    std::vector<Simulation::SparseSurfaceBrickRange> rangeMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceDrawArgs> drawArgsMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceRecord> surfaceRecordMirrorAfterCopy;
    std::vector<Simulation::SparseSurfaceClusterRecord> surfaceClusterMirrorAfterCopy;
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> drawSlotByCoordAfterCopy;
    std::vector<uint8_t> drawSlotOccupiedAfterCopy;
    std::vector<uint32_t> freeDrawSlotsAfterCopy;
    bool hasRangeAllocatorRollback = false;
    Simulation::SparseSurfaceRangeAllocator rangeAllocatorBeforeStage;
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
        uint64_t currentFrameFenceValue = 0,
        uint64_t telemetryFrameIndex = 0);
    bool StageSnapshot(
        const Simulation::SparseSurfaceGpuSnapshot& snapshot,
        SparseSurfaceUploadTicket* outTicket = nullptr);
    bool StageDirtyPayloadSnapshot(
        const Simulation::SparseSurfaceGpuSnapshot& snapshot,
        SparseSurfaceUploadTicket* outTicket = nullptr);
    bool EmitCopy(ID3D12GraphicsCommandList* commandList, const SparseSurfaceUploadTicket& ticket);
    bool BuildFallbackDrawArgsExcluding(
        ID3D12GraphicsCommandList* commandList,
        const std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash>& excludedCoords,
        uint32_t* outExcludedDrawSlots = nullptr,
        uint32_t* outCommandCount = nullptr);
    bool CopyFixedSlotFacesIntoCompactRanges(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* fixedSlotFaceBuffer,
        uint32_t fixedSlotFaceCapacity,
        const std::vector<std::pair<uint32_t, Simulation::BrickCoord>>& slotCoords,
        uint32_t* outCopiedTiles = nullptr,
        uint32_t* outCopiedFaces = nullptr);
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
    // Why the last StageDirtyPayloadSnapshot rejected (or "accepted"). Lets the mid-mesh
    // caller log exactly which precondition forced a full-StageSnapshot fallback.
    const char* LastDirtyStageRejectReason() const { return m_lastDirtyStageRejectReason; }
    const DescriptorHandle& FaceBufferSRV() const { return m_faceBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& RangeBufferSRV() const { return m_rangeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& SurfaceRecordSRV() const { return m_surfaceRecordBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& SurfaceClusterSRV() const { return m_surfaceClusterBuffer.GetShaderVisibleSRV(); }
    ID3D12Resource* DrawArgsResource() const { return m_drawArgsBuffer.GetResource(); }
    ID3D12Resource* FallbackDrawArgsResource() const { return m_fallbackDrawArgsBuffer.GetResource(); }
    ID3D12Resource* DrawCountResource() const { return m_drawCountBuffer.GetResource(); }
    const D3D12_VERTEX_BUFFER_VIEW& VertexIdBufferView() const { return m_vertexIdBufferView; }
    const D3D12_INDEX_BUFFER_VIEW& IndexBufferView() const { return m_indexBufferView; }
    uint32_t VertexIdCapacityFaces() const { return m_vertexIdCapacityFaces; }
    bool IsGpuCullEnabled() const { return m_config.useGpuCull && m_surfaceCullPipeline.IsValid(); }
    bool TryGetBrickDebugInfo(
        const Simulation::BrickCoord& coord,
        SparseSurfaceGpuBrickDebugInfo* outInfo = nullptr) const;
    bool TryClassifyBrickGpuCull(
        const Simulation::BrickCoord& coord,
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
        float padding,
        SparseSurfaceGpuCullDebugInfo* outInfo) const;

private:
    Result<void> CreateVertexIdStream(ID3D12Device* device);
    void RebuildSurfaceRecordLookup();

    SparseSurfaceGpuConfig m_config;
    SparseSurfaceGpuStats m_stats;
    const char* m_lastDirtyStageRejectReason = "none";
    GPUBuffer m_faceBuffer;
    GPUBuffer m_rangeBuffer;
    GPUBuffer m_drawArgsBuffer;
    GPUBuffer m_fallbackDrawArgsBuffer;
    GPUBuffer m_surfaceRecordBuffer;
    GPUBuffer m_surfaceClusterBuffer;
    GPUBuffer m_drawCountBuffer;
    std::array<GPUBuffer, 3> m_cullStatsReadback;
    std::array<bool, 3> m_cullStatsReadbackPending = {};
    std::array<uint32_t, 3> m_cullStatsReadbackQueuedFrames = {};
    std::array<UploadBuffer, 3> m_uploadRing;
    UploadBuffer m_fallbackDrawArgsUpload;
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
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> m_surfaceRecordIndexByCoord;
    std::vector<uint32_t> m_surfaceRecordClusterIndex;
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> m_drawSlotByCoord;
    std::unordered_map<
        Simulation::BrickCoord,
        std::vector<Simulation::SparseSurfaceFace>,
        Simulation::BrickCoordHash> m_payloadFaceMirrorByCoord;
    std::vector<uint8_t> m_drawSlotOccupied;
    std::vector<uint32_t> m_freeDrawSlots;
    uint64_t m_currentTelemetryFrameIndex = 0;
    uint32_t m_activeUploadSlot = 0;
    uint64_t m_uploadWriteOffset = 0;
};

} // namespace VENPOD::Graphics
