#include "SparseSurfaceGpuResources.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kMaxUploadRingSlots = 3;
constexpr uint32_t kCullStatsUintCount = 13;
constexpr uint32_t kMaxSurfaceRecordsPerCluster = 64;
constexpr uint32_t kMaxSurfaceClusterExtentVoxels = 4096;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool IsPowerOfTwo(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

bool SameBytes(const void* lhs, const void* rhs, size_t byteCount) {
    return byteCount == 0 || std::memcmp(lhs, rhs, byteCount) == 0;
}

} // namespace

SparseSurfaceGpuResources::~SparseSurfaceGpuResources() {
    Shutdown();
}

Result<void> SparseSurfaceGpuResources::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    ShaderCompiler* shaderCompiler,
    const std::filesystem::path& shaderPath,
    const SparseSurfaceGpuConfig& config)
{
    if (!device) {
        return Error("SparseSurfaceGpuResources::Initialize - device is null");
    }
    if (config.maxFaces == 0 || config.maxBrickRanges == 0 || config.maxDrawCommands == 0) {
        return Error("SparseSurfaceGpuResources::Initialize - capacities must be > 0");
    }
    if (config.uploadRingSlots == 0 || config.uploadRingSlots > kMaxUploadRingSlots) {
        return Error("SparseSurfaceGpuResources::Initialize - uploadRingSlots must be 1..{}", kMaxUploadRingSlots);
    }
    if (config.surfaceRecordsPerCluster == 0 || config.surfaceRecordsPerCluster > kMaxSurfaceRecordsPerCluster) {
        return Error("SparseSurfaceGpuResources::Initialize - surfaceRecordsPerCluster must be 1..{}", kMaxSurfaceRecordsPerCluster);
    }
    if (config.surfaceClusterMaxExtentVoxels > kMaxSurfaceClusterExtentVoxels) {
        return Error(
            "SparseSurfaceGpuResources::Initialize - surfaceClusterMaxExtentVoxels must be <= {}",
            kMaxSurfaceClusterExtentVoxels);
    }
    if (config.useFixedRangeTable && !IsPowerOfTwo(config.maxBrickRanges)) {
        return Error("SparseSurfaceGpuResources::Initialize - fixed range table capacity must be power-of-two");
    }

    Shutdown();
    m_config = config;
    m_heapManager = &heapManager;
    m_stats = {};
    m_stats.initialized = true;
    m_stats.rangeAllocatorEnabled = config.useRangeAllocator;
    m_stats.fixedRangeTableEnabled = config.useFixedRangeTable;
    m_stats.stableDrawSlotsEnabled = config.useStableDrawSlots;
    m_stats.compactStableDrawCommandsEnabled =
        config.useStableDrawSlots && config.compactStableDrawCommands;
    m_stats.gpuCullEnabled = config.useGpuCull;
    m_stats.maxFaces = config.maxFaces;
    m_stats.maxBrickRanges = config.maxBrickRanges;
    m_stats.maxDrawCommands = config.maxDrawCommands;
    m_stats.payloadCopyRegionBudget = config.maxPayloadCopyRegionsPerFrame;
    m_stats.payloadCopyFaceBudget = config.maxPayloadCopyFacesPerFrame;
    m_stats.surfaceRecordsPerCluster = config.surfaceRecordsPerCluster;
    m_stats.surfaceClusterMaxExtentVoxels = config.surfaceClusterMaxExtentVoxels;
    m_stats.surfaceClusterFastAcceptMaxRecords = config.surfaceClusterFastAcceptMaxRecords;
    m_stats.surfaceClusterFastAcceptMaxFaces = config.surfaceClusterFastAcceptMaxFaces;
    if (config.useRangeAllocator) {
        m_faceRangeAllocator.Initialize(config.maxFaces, config.rangeRetirementDelayFrames);
    }

    auto result = m_faceBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxFaces) * sizeof(Simulation::SparseSurfaceFace),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceFace),
        "SparseSurfaceFaceBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface face buffer: {}", result.error());
    }
    result = m_faceBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface face SRV: {}", result.error());
    }

    result = m_rangeBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxBrickRanges) * sizeof(Simulation::SparseSurfaceBrickRange),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceBrickRange),
        "SparseSurfaceRangeBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface range buffer: {}", result.error());
    }
    result = m_rangeBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface range SRV: {}", result.error());
    }

    result = m_drawArgsBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceDrawArgs),
        BufferUsage::IndirectArgument | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(Simulation::SparseSurfaceDrawArgs),
        "SparseSurfaceDrawArgsBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw args buffer: {}", result.error());
    }
    result = m_drawArgsBuffer.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw args UAV: {}", result.error());
    }

    result = m_surfaceRecordBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceRecord),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceRecord),
        "SparseSurfaceRecordBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface record buffer: {}", result.error());
    }
    result = m_surfaceRecordBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface record SRV: {}", result.error());
    }

    result = m_surfaceClusterBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceClusterRecord),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceClusterRecord),
        "SparseSurfaceClusterBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface cluster buffer: {}", result.error());
    }
    result = m_surfaceClusterBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface cluster SRV: {}", result.error());
    }

    result = m_drawCountBuffer.Initialize(
        device,
        sizeof(uint32_t) * kCullStatsUintCount,
        BufferUsage::IndirectArgument | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "SparseSurfaceCullStatsBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw count buffer: {}", result.error());
    }
    result = m_drawCountBuffer.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw count UAV: {}", result.error());
    }

    result = CreateVertexIdStream(device);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface vertex-id stream: {}", result.error());
    }

    for (uint32_t i = 0; i < config.uploadRingSlots; ++i) {
        result = m_uploadRing[i].Initialize(
            device,
            config.uploadBytesPerSlot,
            "SparseSurfaceUploadRing");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface upload ring: {}", result.error());
        }
        result = m_cullConstantUploads[i].Initialize(
            device,
            256u,
            "SparseSurfaceCullConstants");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull constants: {}", result.error());
        }
        result = m_cullStatsReadback[i].Initialize(
            device,
            sizeof(uint32_t) * kCullStatsUintCount,
            BufferUsage::Readback,
            sizeof(uint32_t),
            "SparseSurfaceCullStatsReadback");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull stats readback slot {}: {}", i, result.error());
        }
    }

    if (config.useGpuCull) {
        if (!shaderCompiler || shaderPath.empty()) {
            Shutdown();
            return Error("SparseSurfaceGpuResources::Initialize - GPU cull requested without shader compiler/path");
        }
        const std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparseSurfaceCullCompact.hlsl";
        auto compileResult = shaderCompiler->CompileComputeShader(csPath, L"main", config.useGpuCull);
        if (!compileResult) {
            Shutdown();
            return Error("Failed to compile sparse surface cull shader: {}", compileResult.error());
        }
        m_surfaceCullShader = compileResult.value();
        if (!m_surfaceCullShader.IsValid()) {
            Shutdown();
            return Error("Sparse surface cull shader compilation failed: {}", m_surfaceCullShader.errors);
        }
        ComputePipelineDesc cullDesc;
        cullDesc.computeShader = m_surfaceCullShader;
        cullDesc.debugName = "SparseSurfaceCullCompact";
        cullDesc.rootParams.push_back({RootParamType::ConstantBuffer, 0, 0});
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            0,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            1,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            0,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            1,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV
        });
        result = m_surfaceCullPipeline.Initialize(device, cullDesc);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull pipeline: {}", result.error());
        }
        m_stats.gpuCullEnabled = true;
    }

    return {};
}

void SparseSurfaceGpuResources::Shutdown() {
    m_faceBuffer.Shutdown();
    m_rangeBuffer.Shutdown();
    m_drawArgsBuffer.Shutdown();
    m_surfaceRecordBuffer.Shutdown();
    m_surfaceClusterBuffer.Shutdown();
    m_drawCountBuffer.Shutdown();
    m_vertexIdStream.Shutdown();
    m_indexStream.Shutdown();
    m_vertexIdStreamUpload.Shutdown();
    m_indexStreamUpload.Shutdown();
    m_vertexIdBufferView = {};
    m_indexBufferView = {};
    m_vertexIdCapacityFaces = 0;
    m_staticIaUploadPending = false;
    m_staticIaUploadComplete = false;
    m_staticIaUploadFence = 0;
    m_currentFrameFenceValue = 0;
    for (auto& readback : m_cullStatsReadback) {
        readback.Shutdown();
    }
    m_cullStatsReadbackPending = {};
    for (auto& upload : m_uploadRing) {
        upload.Shutdown();
    }
    for (auto& upload : m_cullConstantUploads) {
        upload.Shutdown();
    }
    m_surfaceCullPipeline.Shutdown();
    m_surfaceCullShader = {};
    m_heapManager = nullptr;
    m_stats = {};
    m_faceRangeAllocator.Clear();
    m_payloadResidentCoords.clear();
    m_rangeMirror.clear();
    m_drawArgsMirror.clear();
    m_surfaceRecordMirror.clear();
    m_surfaceClusterMirror.clear();
    m_drawSlotByCoord.clear();
    m_payloadFaceMirrorByCoord.clear();
    m_drawSlotOccupied.clear();
    m_freeDrawSlots.clear();
    m_uploadWriteOffset = 0;
    m_activeUploadSlot = 0;
}

Result<void> SparseSurfaceGpuResources::CreateVertexIdStream(ID3D12Device* device) {
    if (!device) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - device is null");
    }
    if (m_config.maxFaces == 0u) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - maxFaces must be > 0");
    }
    if (m_config.maxFaces > UINT32_MAX / 6u) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - maxFaces is too large");
    }

    if (m_config.maxFaces > UINT32_MAX / 4u) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - maxFaces is too large for vertex stream");
    }

    const uint32_t vertexCount = m_config.maxFaces * 4u;
    const uint32_t indexCount = m_config.maxFaces * 6u;
    const uint64_t vertexByteCount = static_cast<uint64_t>(vertexCount) * sizeof(uint32_t);
    const uint64_t indexByteCount = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
    auto result = m_vertexIdStream.Initialize(
        device,
        vertexByteCount,
        BufferUsage::Default,
        sizeof(uint32_t),
        "SparseSurfaceVertexIdStream");
    if (!result) {
        return result;
    }
    result = m_indexStream.Initialize(
        device,
        indexByteCount,
        BufferUsage::Default,
        sizeof(uint32_t),
        "SparseSurfaceIndexStream");
    if (!result) {
        m_vertexIdStream.Shutdown();
        return result;
    }
    result = m_vertexIdStreamUpload.Initialize(
        device,
        vertexByteCount,
        "SparseSurfaceVertexIdStreamUpload");
    if (!result) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        return result;
    }
    result = m_indexStreamUpload.Initialize(
        device,
        indexByteCount,
        "SparseSurfaceIndexStreamUpload");
    if (!result) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        return result;
    }

    uint32_t* mapped = static_cast<uint32_t*>(m_vertexIdStreamUpload.GetMappedData());
    if (!mapped) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - upload buffer is not mapped");
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        mapped[i] = i;
    }
    uint32_t* mappedIndices = static_cast<uint32_t*>(m_indexStreamUpload.GetMappedData());
    if (!mappedIndices) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - index upload buffer is not mapped");
    }
    for (uint32_t face = 0; face < m_config.maxFaces; ++face) {
        const uint32_t vertexBase = face * 4u;
        const uint32_t indexBase = face * 6u;
        mappedIndices[indexBase + 0u] = vertexBase + 0u;
        mappedIndices[indexBase + 1u] = vertexBase + 1u;
        mappedIndices[indexBase + 2u] = vertexBase + 2u;
        mappedIndices[indexBase + 3u] = vertexBase + 0u;
        mappedIndices[indexBase + 4u] = vertexBase + 2u;
        mappedIndices[indexBase + 5u] = vertexBase + 3u;
    }

    m_vertexIdCapacityFaces = m_config.maxFaces;
    m_vertexIdBufferView.BufferLocation = m_vertexIdStream.GetGPUVirtualAddress();
    m_vertexIdBufferView.SizeInBytes = static_cast<UINT>(vertexByteCount);
    m_vertexIdBufferView.StrideInBytes = sizeof(uint32_t);
    m_indexBufferView.BufferLocation = m_indexStream.GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexByteCount);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_stats.iaStreamCapacityFaces = m_vertexIdCapacityFaces;
    m_stats.iaStreamVertexCount = vertexCount;
    m_stats.iaStreamIndexCount = indexCount;
    m_stats.iaStreamVertexBytes = vertexByteCount;
    m_stats.iaStreamIndexBytes = indexByteCount;
    m_stats.iaStreamGpuLocal = true;
    m_stats.iaStreamUploadPending = true;
    m_staticIaUploadPending = true;
    m_staticIaUploadComplete = false;
    m_staticIaUploadFence = 0;
    spdlog::info(
        "Sparse surface indexed IA streams created: {} faces, GPU-local vertexIds={:.2f} MB indices={:.2f} MB",
        m_vertexIdCapacityFaces,
        static_cast<double>(vertexByteCount) / (1024.0 * 1024.0),
        static_cast<double>(indexByteCount) / (1024.0 * 1024.0));
    return {};
}

void SparseSurfaceGpuResources::BeginFrame(
    uint32_t frameIndex,
    uint64_t completedFenceValue,
    uint64_t currentFrameFenceValue)
{
    m_currentFrameFenceValue = currentFrameFenceValue;
    if (m_staticIaUploadComplete &&
        !m_staticIaUploadPending &&
        m_staticIaUploadFence != 0u &&
        completedFenceValue >= m_staticIaUploadFence) {
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        m_staticIaUploadFence = 0;
    }
    m_stats.iaStreamUploadPending = m_staticIaUploadPending;
    m_stats.iaStreamUploadRetireFence = m_staticIaUploadFence;
    if (m_config.uploadRingSlots == 0) {
        return;
    }
    m_activeUploadSlot = frameIndex % m_config.uploadRingSlots;
    m_uploadWriteOffset = 0;
    if (m_config.useRangeAllocator) {
        if (currentFrameFenceValue != 0u) {
            m_faceRangeAllocator.BeginFrame(completedFenceValue, currentFrameFenceValue);
        } else {
            m_faceRangeAllocator.BeginFrame(frameIndex);
        }
        const auto& allocatorStats = m_faceRangeAllocator.GetStats();
        m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
        m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
        m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
        m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
        m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
        m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
        m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
    }
    m_stats.stagedFacesLastFrame = 0;
    m_stats.stagedRangesLastFrame = 0;
    m_stats.stagedRangeTableCapacityLastFrame = 0;
    m_stats.stagedDrawCommandsLastFrame = 0;
    m_stats.stagedRangeCopyRegionsLastFrame = 0;
    m_stats.stagedDrawCopyRegionsLastFrame = 0;
    m_stats.skippedCleanRangeSlotsLastFrame = 0;
    m_stats.skippedCleanDrawCommandsLastFrame = 0;
    m_stats.fullRangeTableUploadLastFrame = false;
    m_stats.fullDrawArgsUploadLastFrame = false;
    m_stats.activeDrawCommandsLastFrame = 0;
    m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(m_drawSlotOccupied.size());
    m_stats.stableDrawFreeSlots = static_cast<uint32_t>(m_freeDrawSlots.size());
    m_stats.inactiveStableDrawSlotsLastFrame = 0;
    m_stats.stagedSurfaceRecordsLastFrame = 0;
    m_stats.stagedSurfaceRecordCopyRegionsLastFrame = 0;
    m_stats.skippedCleanSurfaceRecordsLastFrame = 0;
    m_stats.fullSurfaceRecordUploadLastFrame = false;
    m_stats.stagedSurfaceClustersLastFrame = 0;
    m_stats.stagedSurfaceClusterCopyRegionsLastFrame = 0;
    m_stats.skippedCleanSurfaceClustersLastFrame = 0;
    m_stats.fullSurfaceClusterUploadLastFrame = false;
    m_stats.gpuCullDispatchesLastFrame = 0;
    m_stats.gpuCullCandidateRecordsLastFrame = 0;
    m_stats.gpuCullCandidateClustersLastFrame = m_stats.uploadedSurfaceClusters;
    m_stats.gpuCullMaxDrawCommands = m_config.maxDrawCommands;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
    m_stats.stagedCandidateBricksLastFrame = 0;
    m_stats.stagedVisibleBricksLastFrame = 0;
    m_stats.stagedCulledBricksLastFrame = 0;
    m_stats.stagedFaceCopyRegionsLastFrame = 0;
    m_stats.stagedPayloadPatchBricksLastFrame = 0;
    m_stats.stagedPayloadPatchFacesLastFrame = 0;
    m_stats.stagedPayloadPatchRegionsLastFrame = 0;
    m_stats.stagedDirtyPayloadBricksLastFrame = 0;
    m_stats.skippedCleanPayloadBricksLastFrame = 0;
    m_stats.deferredPayloadBricksLastFrame = 0;
    m_stats.residentPayloadBricks = static_cast<uint32_t>(m_payloadResidentCoords.size());
    m_stats.pendingDirtyBricksLastFrame = 0;
    m_stats.pendingRemovedBricksLastFrame = 0;
    m_stats.stagedBytesLastFrame = 0;
    m_stats.uploadOverflowLastFrame = false;
}

bool SparseSurfaceGpuResources::StageSnapshot(
    const Simulation::SparseSurfaceGpuSnapshot& snapshot,
    SparseSurfaceUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }
    if (snapshot.faces.size() > m_config.maxFaces ||
        snapshot.ranges.size() > m_config.maxBrickRanges ||
        snapshot.drawArgs.size() > m_config.maxDrawCommands ||
        snapshot.surfaceRecords.size() > m_config.maxDrawCommands) {
        m_stats.uploadOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    if (m_config.useRangeAllocator) {
        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> liveCoords;
        liveCoords.reserve(snapshot.brickFaceCounts.size());
        for (const auto& item : snapshot.brickFaceCounts) {
            liveCoords.insert(item.coord);
        }
        m_faceRangeAllocator.ReleaseNotIn(liveCoords);

        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> dirtyCoords;
        dirtyCoords.reserve(snapshot.dirtyBricks.size());
        for (const auto& item : snapshot.dirtyBricks) {
            dirtyCoords.insert(item.coord);
        }

        SparseSurfaceUploadTicket ticket;
        ticket.valid = true;
        ticket.ringSlot = m_activeUploadSlot;
        ticket.serial = snapshot.serial;
        ticket.candidateBricks = snapshot.candidateBricks;
        ticket.visibleBricks = snapshot.visibleBricks;
        ticket.culledBricks = snapshot.culledBricks;
        ticket.removedBricks.reserve(snapshot.removedBricks.size());
        for (const auto& removed : snapshot.removedBricks) {
            if (removed.serial <= snapshot.serial) {
                ticket.removedBricks.push_back(removed.coord);
            }
        }

        std::vector<Simulation::SparseSurfaceBrickRange> remappedRanges(snapshot.ranges.size());
        const bool compactStableDrawCommands =
            m_config.useStableDrawSlots && m_config.compactStableDrawCommands;
        std::vector<Simulation::SparseSurfaceDrawArgs> remappedDrawArgs;
        std::vector<Simulation::SparseSurfaceDrawArgs> stableSlotDrawArgs;
        std::vector<Simulation::SparseSurfaceRecord> remappedSurfaceRecords;
        remappedSurfaceRecords.reserve(snapshot.drawBatches.size());
        if (!m_config.useStableDrawSlots || compactStableDrawCommands) {
            remappedDrawArgs.reserve(snapshot.drawBatches.size());
        }
        auto nextDrawSlotByCoord = m_drawSlotByCoord;
        auto nextDrawSlotOccupied = m_drawSlotOccupied;
        auto nextFreeDrawSlots = m_freeDrawSlots;
        if (m_config.useStableDrawSlots) {
            for (const Simulation::BrickCoord& coord : ticket.removedBricks) {
                auto slotIt = nextDrawSlotByCoord.find(coord);
                if (slotIt == nextDrawSlotByCoord.end()) {
                    continue;
                }
                const uint32_t slot = slotIt->second;
                nextDrawSlotByCoord.erase(slotIt);
                if (slot < nextDrawSlotOccupied.size() && nextDrawSlotOccupied[slot] != 0u) {
                    nextDrawSlotOccupied[slot] = 0u;
                    nextFreeDrawSlots.push_back(slot);
                }
            }
            if (compactStableDrawCommands) {
                stableSlotDrawArgs.resize(nextDrawSlotOccupied.size());
            } else {
                remappedDrawArgs.resize(nextDrawSlotOccupied.size());
            }
        }

        const uint64_t stageStartOffset = AlignUp(m_uploadWriteOffset, 4u);
        uint64_t writeOffset = stageStartOffset;
        uint32_t drawableFaceCount = 0;
        uint32_t copiedPayloadFaceCount = 0;
        uint32_t copiedPayloadBrickCount = 0;
        uint32_t patchedPayloadFaceCount = 0;
        uint32_t patchedPayloadBrickCount = 0;
        uint32_t patchedPayloadRegionCount = 0;
        uint32_t skippedCleanPayloadBrickCount = 0;
        uint32_t deferredPayloadBrickCount = 0;
        std::unordered_map<
            Simulation::BrickCoord,
            Simulation::SparseSurfaceFaceAllocation,
            Simulation::BrickCoordHash> visibleAllocations;
        visibleAllocations.reserve(snapshot.drawBatches.size());

        auto appendDraw = [&](const Simulation::BrickCoord& coord, const Simulation::SparseSurfaceFaceAllocation& allocation) -> bool {
            Simulation::SparseSurfaceDrawArgs args;
            args.indexCountPerInstance = allocation.faceCount * 6u;
            args.instanceCount = 1u;
            args.startIndexLocation = allocation.firstFace * 6u;
            args.baseVertexLocation = 0;
            args.startInstanceLocation = 0u;
            if (m_config.useStableDrawSlots) {
                auto slotIt = nextDrawSlotByCoord.find(coord);
                uint32_t slot = UINT32_MAX;
                if (slotIt != nextDrawSlotByCoord.end()) {
                    slot = slotIt->second;
                } else if (!nextFreeDrawSlots.empty()) {
                    slot = nextFreeDrawSlots.back();
                    nextFreeDrawSlots.pop_back();
                    nextDrawSlotByCoord.emplace(coord, slot);
                    if (slot >= nextDrawSlotOccupied.size()) {
                        return false;
                    }
                    nextDrawSlotOccupied[slot] = 1u;
                } else {
                    if (nextDrawSlotOccupied.size() >= m_config.maxDrawCommands) {
                        return false;
                    }
                    slot = static_cast<uint32_t>(nextDrawSlotOccupied.size());
                    nextDrawSlotByCoord.emplace(coord, slot);
                    nextDrawSlotOccupied.push_back(1u);
                    if (compactStableDrawCommands) {
                        stableSlotDrawArgs.resize(nextDrawSlotOccupied.size());
                    } else {
                        remappedDrawArgs.resize(nextDrawSlotOccupied.size());
                    }
                }
                if (compactStableDrawCommands) {
                    if (slot >= stableSlotDrawArgs.size()) {
                        stableSlotDrawArgs.resize(slot + 1u);
                    }
                    stableSlotDrawArgs[slot] = args;
                } else {
                    if (slot >= remappedDrawArgs.size()) {
                        remappedDrawArgs.resize(slot + 1u);
                    }
                    remappedDrawArgs[slot] = args;
                }
                return true;
            }
            remappedDrawArgs.push_back(args);
            return true;
        };

        auto queuePayloadMirrorUpdate = [&](
            const Simulation::SparseSurfaceDrawBatch& batch)
        {
            SparseSurfacePayloadMirrorUpdate update;
            update.coord = batch.coord;
            if (batch.faceCount > 0u &&
                static_cast<uint64_t>(batch.firstFace + batch.faceCount) <= snapshot.faces.size()) {
                update.faces.assign(
                    snapshot.faces.begin() + batch.firstFace,
                    snapshot.faces.begin() + batch.firstFace + batch.faceCount);
            }
            ticket.payloadMirrorUpdates.push_back(std::move(update));
        };

        auto stageFaceCopy = [&](
            const Simulation::SparseSurfaceFaceAllocation& allocation,
            uint32_t sourceFirstFace,
            uint32_t destFirstFace,
            uint32_t faceCount) -> bool
        {
            if (faceCount == 0u) {
                return true;
            }
            const uint64_t faceBytes =
                static_cast<uint64_t>(faceCount) * sizeof(Simulation::SparseSurfaceFace);
            const uint64_t uploadOffset = AlignUp(writeOffset, 4u);
            const uint64_t endOffset = uploadOffset + faceBytes;
            if (endOffset > upload.GetSize() ||
                static_cast<uint64_t>(sourceFirstFace + faceCount) > snapshot.faces.size() ||
                destFirstFace < allocation.firstFace ||
                destFirstFace + faceCount > allocation.firstFace + allocation.faceCount) {
                return false;
            }
            std::memcpy(
                mapped + uploadOffset,
                snapshot.faces.data() + sourceFirstFace,
                static_cast<size_t>(faceBytes));
            ticket.faceCopyRegions.push_back({
                uploadOffset,
                destFirstFace,
                faceCount
            });
            writeOffset = endOffset;
            return true;
        };

        for (const Simulation::SparseSurfaceDrawBatch& batch : snapshot.drawBatches) {
            Simulation::SparseSurfaceFaceAllocation previousAllocation;
            const bool hadAllocation =
                m_faceRangeAllocator.TryGet(batch.coord, &previousAllocation);
            const bool payloadResident =
                m_payloadResidentCoords.find(batch.coord) != m_payloadResidentCoords.end();
            const bool dirtyPayload = dirtyCoords.find(batch.coord) != dirtyCoords.end();
            const bool allocationChanged =
                !hadAllocation || previousAllocation.faceCount != batch.faceCount;
            const bool needsPayloadUpload =
                batch.faceCount > 0u && (!payloadResident || dirtyPayload || allocationChanged);

            const bool regionBudgetAvailable =
                m_config.maxPayloadCopyRegionsPerFrame == 0u ||
                copiedPayloadBrickCount < m_config.maxPayloadCopyRegionsPerFrame;
            const bool faceBudgetAvailable =
                m_config.maxPayloadCopyFacesPerFrame == 0u ||
                copiedPayloadFaceCount + batch.faceCount <= m_config.maxPayloadCopyFacesPerFrame ||
                copiedPayloadBrickCount == 0u;
            const bool canUploadPayload = !needsPayloadUpload || (regionBudgetAvailable && faceBudgetAvailable);

            if (needsPayloadUpload) {
                if (!canUploadPayload) {
                    if (hadAllocation && payloadResident) {
                        visibleAllocations.emplace(batch.coord, previousAllocation);
                        if (!appendDraw(batch.coord, previousAllocation)) {
                            m_stats.uploadOverflowLastFrame = true;
                            return false;
                        }
                        drawableFaceCount += previousAllocation.faceCount;
                    }
                    ++deferredPayloadBrickCount;
                    continue;
                }

                Simulation::SparseSurfaceFaceAllocation allocation;
                if (!m_faceRangeAllocator.AllocateOrResize(batch.coord, batch.faceCount, &allocation)) {
                    m_stats.uploadOverflowLastFrame = true;
                    const auto& allocatorStats = m_faceRangeAllocator.GetStats();
                    m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
                    return false;
                }

                bool uploadedPayload = false;
                if (hadAllocation &&
                    payloadResident &&
                    dirtyPayload &&
                    !allocationChanged &&
                    allocation.firstFace == previousAllocation.firstFace &&
                    allocation.faceCount == previousAllocation.faceCount) {
                    auto mirrorIt = m_payloadFaceMirrorByCoord.find(batch.coord);
                    const bool mirrorUsable =
                        mirrorIt != m_payloadFaceMirrorByCoord.end() &&
                        mirrorIt->second.size() == batch.faceCount &&
                        static_cast<uint64_t>(batch.firstFace + batch.faceCount) <= snapshot.faces.size();
                    const auto runs = Simulation::BuildSparseSurfaceChangedFaceRuns(
                        mirrorUsable ? snapshot.faces.data() + batch.firstFace : nullptr,
                        mirrorUsable ? mirrorIt->second.data() : nullptr,
                        batch.faceCount);
                    uint32_t runFaceCount = 0;
                    for (const auto& run : runs) {
                        runFaceCount += run.faceCount;
                    }
                    const uint32_t remainingRegionBudget =
                        m_config.maxPayloadCopyRegionsPerFrame == 0u
                            ? UINT_MAX
                            : m_config.maxPayloadCopyRegionsPerFrame -
                                std::min<uint32_t>(
                                    m_config.maxPayloadCopyRegionsPerFrame,
                                    static_cast<uint32_t>(ticket.faceCopyRegions.size()));
                    const uint32_t remainingFaceBudget =
                        m_config.maxPayloadCopyFacesPerFrame == 0u
                            ? UINT_MAX
                            : m_config.maxPayloadCopyFacesPerFrame - copiedPayloadFaceCount;
                    const bool canPatch =
                        !runs.empty() &&
                        runs.size() <= remainingRegionBudget &&
                        runFaceCount <= remainingFaceBudget;
                    if (runs.empty()) {
                        uploadedPayload = true;
                    } else if (canPatch) {
                        for (const auto& run : runs) {
                            if (!stageFaceCopy(
                                    allocation,
                                    batch.firstFace + run.firstFace,
                                    allocation.firstFace + run.firstFace,
                                    run.faceCount)) {
                                m_stats.uploadOverflowLastFrame = true;
                                const auto& allocatorStats = m_faceRangeAllocator.GetStats();
                                m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
                                return false;
                            }
                        }
                        copiedPayloadFaceCount += runFaceCount;
                        ++copiedPayloadBrickCount;
                        patchedPayloadFaceCount += runFaceCount;
                        ++patchedPayloadBrickCount;
                        patchedPayloadRegionCount += static_cast<uint32_t>(runs.size());
                        uploadedPayload = true;
                    }
                }

                if (!uploadedPayload) {
                    if (!stageFaceCopy(
                            allocation,
                            batch.firstFace,
                            allocation.firstFace,
                            batch.faceCount)) {
                        m_stats.uploadOverflowLastFrame = true;
                        const auto& allocatorStats = m_faceRangeAllocator.GetStats();
                        m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
                        return false;
                    }
                    copiedPayloadFaceCount += batch.faceCount;
                    ++copiedPayloadBrickCount;
                }
                ticket.uploadedPayloadBricks.push_back(batch.coord);
                queuePayloadMirrorUpdate(batch);
                visibleAllocations.emplace(batch.coord, allocation);
                if (!appendDraw(batch.coord, allocation)) {
                    m_stats.uploadOverflowLastFrame = true;
                    return false;
                }
                drawableFaceCount += allocation.faceCount;
            } else {
                visibleAllocations.emplace(batch.coord, previousAllocation);
                if (!appendDraw(batch.coord, previousAllocation)) {
                    m_stats.uploadOverflowLastFrame = true;
                    return false;
                }
                drawableFaceCount += previousAllocation.faceCount;
                ++skippedCleanPayloadBrickCount;
            }
        }

        struct SnapshotFaceSpan {
            uint32_t firstFace = 0;
            uint32_t faceCount = 0;
        };
        std::unordered_map<Simulation::BrickCoord, SnapshotFaceSpan, Simulation::BrickCoordHash> snapshotFaceSpans;
        snapshotFaceSpans.reserve(snapshot.drawBatches.size());
        for (const Simulation::SparseSurfaceDrawBatch& batch : snapshot.drawBatches) {
            snapshotFaceSpans.emplace(batch.coord, SnapshotFaceSpan{batch.firstFace, batch.faceCount});
        }

        for (size_t i = 0; i < snapshot.ranges.size(); ++i) {
            const Simulation::SparseSurfaceBrickRange& source = snapshot.ranges[i];
            if (source.flags == 0u) {
                continue;
            }
            Simulation::SparseSurfaceBrickRange range = source;
            auto allocationIt = visibleAllocations.find(source.coord);
            if (allocationIt != visibleAllocations.end()) {
                range.firstFace = allocationIt->second.firstFace;
                range.faceCount = allocationIt->second.faceCount;
            } else {
                range.firstFace = 0u;
                range.faceCount = 0u;
                if (source.faceCount == 0u && dirtyCoords.find(source.coord) != dirtyCoords.end()) {
                    ticket.uploadedPayloadBricks.push_back(source.coord);
                }
            }
            remappedRanges[i] = range;

            if (range.flags != 0u && range.faceCount > 0u) {
                Simulation::SparseSurfaceRecord record;
                record.coord = range.coord;
                record.firstFace = range.firstFace;
                record.faceCount = range.faceCount;
                record.flags = range.flags;
                record.generation = snapshot.serial;
                auto spanIt = snapshotFaceSpans.find(range.coord);
                if (spanIt != snapshotFaceSpans.end() &&
                    static_cast<uint64_t>(spanIt->second.firstFace + spanIt->second.faceCount) <= snapshot.faces.size()) {
                    Simulation::ComputeSparseSurfaceFaceBounds(
                        snapshot.faces.data() + spanIt->second.firstFace,
                        spanIt->second.faceCount,
                        &record.minX,
                        &record.minY,
                        &record.minZ,
                        &record.maxX,
                        &record.maxY,
                        &record.maxZ);
                } else {
                    const int32_t minX = range.coord.x * Simulation::SPARSE_BRICK_SIZE;
                    const int32_t minY = range.coord.y * Simulation::SPARSE_BRICK_SIZE;
                    const int32_t minZ = range.coord.z * Simulation::SPARSE_BRICK_SIZE;
                    record.minX = minX;
                    record.minY = minY;
                    record.minZ = minZ;
                    record.maxX = minX + Simulation::SPARSE_BRICK_SIZE;
                    record.maxY = minY + Simulation::SPARSE_BRICK_SIZE;
                    record.maxZ = minZ + Simulation::SPARSE_BRICK_SIZE;
                }
                remappedSurfaceRecords.push_back(record);
            }
        }

        uint32_t inactiveStableDrawSlots = 0;
        if (compactStableDrawCommands) {
            remappedDrawArgs.clear();
            remappedDrawArgs.reserve(snapshot.drawBatches.size());
            for (size_t slot = 0; slot < nextDrawSlotOccupied.size(); ++slot) {
                if (nextDrawSlotOccupied[slot] == 0u) {
                    ++inactiveStableDrawSlots;
                    continue;
                }
                const Simulation::SparseSurfaceDrawArgs& args = slot < stableSlotDrawArgs.size()
                    ? stableSlotDrawArgs[slot]
                    : Simulation::SparseSurfaceDrawArgs{};
                if (args.indexCountPerInstance == 0u || args.instanceCount == 0u) {
                    ++inactiveStableDrawSlots;
                    continue;
                }
                remappedDrawArgs.push_back(args);
            }
        }

        Simulation::SortSparseSurfaceRecordsForClusters(remappedSurfaceRecords);
        std::vector<Simulation::SparseSurfaceClusterRecord> remappedSurfaceClusters =
            Simulation::BuildSparseSurfaceClusters(
                remappedSurfaceRecords,
                m_config.surfaceRecordsPerCluster,
                m_config.surfaceClusterMaxExtentVoxels);

        std::vector<Simulation::SparseSurfaceBrickRange> publishedRanges;
        if (m_config.useFixedRangeTable) {
            if (snapshot.rangeCount * 2u > m_config.maxBrickRanges) {
                m_stats.uploadOverflowLastFrame = true;
                const auto& allocatorStats = m_faceRangeAllocator.GetStats();
                m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
                return false;
            }
            publishedRanges.resize(m_config.maxBrickRanges);
            const uint32_t mask = m_config.maxBrickRanges - 1u;
            for (const Simulation::SparseSurfaceBrickRange& source : remappedRanges) {
                if (source.flags == 0u) {
                    continue;
                }
                uint32_t slot = Simulation::HashBrickCoord32(source.coord) & mask;
                bool inserted = false;
                for (uint32_t probe = 0; probe < m_config.maxBrickRanges; ++probe) {
                    Simulation::SparseSurfaceBrickRange& tableEntry = publishedRanges[slot];
                    if (tableEntry.flags == 0u) {
                        tableEntry = source;
                        inserted = true;
                        break;
                    }
                    slot = (slot + 1u) & mask;
                }
                if (!inserted) {
                    m_stats.uploadOverflowLastFrame = true;
                    return false;
                }
            }
        } else {
            publishedRanges = std::move(remappedRanges);
        }

        const bool fullRangeUpload = m_rangeMirror.empty() && !publishedRanges.empty();
        const bool fullDrawUpload = m_drawArgsMirror.empty() && !remappedDrawArgs.empty();
        const bool fullSurfaceRecordUpload = m_surfaceRecordMirror.empty() && !remappedSurfaceRecords.empty();
        const bool fullSurfaceClusterUpload = m_surfaceClusterMirror.empty() && !remappedSurfaceClusters.empty();
        uint32_t skippedCleanRangeSlots = 0;
        uint32_t skippedCleanDrawCommands = 0;
        uint32_t skippedCleanSurfaceRecords = 0;
        uint32_t skippedCleanSurfaceClusters = 0;

        auto stageChangedBlocks = [&](
            const auto& source,
            const auto& mirror,
            bool forceFull,
            uint64_t elementSize,
            uint64_t destBaseOffset,
            uint64_t& inOutWriteOffset,
            std::vector<SparseSurfaceBufferCopyRegion>& outRegions,
            uint32_t& outSkippedCleanElements) -> bool
        {
            const uint32_t elementCount = static_cast<uint32_t>(source.size());
            uint32_t index = 0;
            while (index < elementCount) {
                bool changed = forceFull;
                if (!changed && index >= mirror.size()) {
                    changed = true;
                }
                if (!changed) {
                    changed = !SameBytes(
                        &source[index],
                        &mirror[index],
                        static_cast<size_t>(elementSize));
                }
                if (!changed) {
                    ++outSkippedCleanElements;
                    ++index;
                    continue;
                }

                const uint32_t start = index;
                ++index;
                while (index < elementCount) {
                    if (!forceFull &&
                        index < mirror.size() &&
                        SameBytes(
                            &source[index],
                            &mirror[index],
                            static_cast<size_t>(elementSize))) {
                        break;
                    }
                    ++index;
                }

                const uint32_t count = index - start;
                const uint64_t byteCount = static_cast<uint64_t>(count) * elementSize;
                const uint64_t uploadOffset = AlignUp(inOutWriteOffset, 4u);
                const uint64_t endOffset = uploadOffset + byteCount;
                if (endOffset > upload.GetSize()) {
                    return false;
                }
                std::memcpy(
                    mapped + uploadOffset,
                    source.data() + start,
                    static_cast<size_t>(byteCount));
                outRegions.push_back({
                    uploadOffset,
                    destBaseOffset + static_cast<uint64_t>(start) * elementSize,
                    byteCount
                });
                inOutWriteOffset = endOffset;
            }
            return true;
        };

        uint64_t metadataWriteOffset = AlignUp(writeOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                publishedRanges,
                m_rangeMirror,
                fullRangeUpload,
                sizeof(Simulation::SparseSurfaceBrickRange),
                0u,
                metadataWriteOffset,
                ticket.rangeCopyRegions,
                skippedCleanRangeSlots)) {
            m_stats.uploadOverflowLastFrame = true;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
            return false;
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedDrawArgs,
                m_drawArgsMirror,
                fullDrawUpload,
                sizeof(Simulation::SparseSurfaceDrawArgs),
                0u,
                metadataWriteOffset,
                ticket.drawArgsCopyRegions,
                skippedCleanDrawCommands)) {
            m_stats.uploadOverflowLastFrame = true;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
                m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
            return false;
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedSurfaceRecords,
                m_surfaceRecordMirror,
                fullSurfaceRecordUpload,
                sizeof(Simulation::SparseSurfaceRecord),
                0u,
                metadataWriteOffset,
                ticket.surfaceRecordCopyRegions,
                skippedCleanSurfaceRecords)) {
            m_stats.uploadOverflowLastFrame = true;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
            return false;
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedSurfaceClusters,
                m_surfaceClusterMirror,
                fullSurfaceClusterUpload,
                sizeof(Simulation::SparseSurfaceClusterRecord),
                0u,
                metadataWriteOffset,
                ticket.surfaceClusterCopyRegions,
                skippedCleanSurfaceClusters)) {
            m_stats.uploadOverflowLastFrame = true;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
            return false;
        }
        const uint64_t endOffset = metadataWriteOffset;
        const uint64_t rangeBytes = 0;
        const uint64_t drawArgsBytes = 0;
        const uint64_t surfaceRecordBytes = 0;
        const uint64_t rangeOffset = 0;
        const uint64_t drawArgsOffset = 0;
        const uint64_t surfaceRecordOffset = 0;
        if (endOffset > upload.GetSize()) {
            m_stats.uploadOverflowLastFrame = true;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
            return false;
        }
        ticket.rangeMirrorAfterCopy = std::move(publishedRanges);
        ticket.drawArgsMirrorAfterCopy = std::move(remappedDrawArgs);
        ticket.surfaceRecordMirrorAfterCopy = std::move(remappedSurfaceRecords);
        ticket.surfaceClusterMirrorAfterCopy = std::move(remappedSurfaceClusters);
        ticket.drawSlotByCoordAfterCopy = std::move(nextDrawSlotByCoord);
        ticket.drawSlotOccupiedAfterCopy = std::move(nextDrawSlotOccupied);
        ticket.freeDrawSlotsAfterCopy = std::move(nextFreeDrawSlots);

        m_uploadWriteOffset = endOffset;
        m_stats.stagedFacesLastFrame = drawableFaceCount;
        m_stats.stagedRangesLastFrame = snapshot.rangeCount;
        m_stats.stagedRangeTableCapacityLastFrame = static_cast<uint32_t>(ticket.rangeMirrorAfterCopy.size());
        m_stats.stagedDrawCommandsLastFrame = static_cast<uint32_t>(ticket.drawArgsMirrorAfterCopy.size());
        m_stats.activeDrawCommandsLastFrame = static_cast<uint32_t>(snapshot.drawBatches.size());
        m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(ticket.drawSlotOccupiedAfterCopy.size());
        m_stats.stableDrawFreeSlots = static_cast<uint32_t>(ticket.freeDrawSlotsAfterCopy.size());
        m_stats.inactiveStableDrawSlotsLastFrame = inactiveStableDrawSlots;
        m_stats.stagedSurfaceRecordsLastFrame = static_cast<uint32_t>(ticket.surfaceRecordMirrorAfterCopy.size());
        m_stats.stagedSurfaceClustersLastFrame = static_cast<uint32_t>(ticket.surfaceClusterMirrorAfterCopy.size());
        m_stats.stagedRangeCopyRegionsLastFrame = static_cast<uint32_t>(ticket.rangeCopyRegions.size());
        m_stats.stagedDrawCopyRegionsLastFrame = static_cast<uint32_t>(ticket.drawArgsCopyRegions.size());
        m_stats.stagedSurfaceRecordCopyRegionsLastFrame =
            static_cast<uint32_t>(ticket.surfaceRecordCopyRegions.size());
        m_stats.stagedSurfaceClusterCopyRegionsLastFrame =
            static_cast<uint32_t>(ticket.surfaceClusterCopyRegions.size());
        m_stats.skippedCleanRangeSlotsLastFrame = skippedCleanRangeSlots;
        m_stats.skippedCleanDrawCommandsLastFrame = skippedCleanDrawCommands;
        m_stats.skippedCleanSurfaceRecordsLastFrame = skippedCleanSurfaceRecords;
        m_stats.skippedCleanSurfaceClustersLastFrame = skippedCleanSurfaceClusters;
        m_stats.fullRangeTableUploadLastFrame = fullRangeUpload;
        m_stats.fullDrawArgsUploadLastFrame = fullDrawUpload;
        m_stats.fullSurfaceRecordUploadLastFrame = fullSurfaceRecordUpload;
        m_stats.fullSurfaceClusterUploadLastFrame = fullSurfaceClusterUpload;
        m_stats.stagedCandidateBricksLastFrame = snapshot.candidateBricks;
        m_stats.stagedVisibleBricksLastFrame = snapshot.visibleBricks;
        m_stats.stagedCulledBricksLastFrame = snapshot.culledBricks;
        m_stats.stagedFaceCopyRegionsLastFrame = static_cast<uint32_t>(ticket.faceCopyRegions.size());
        m_stats.stagedPayloadPatchBricksLastFrame = patchedPayloadBrickCount;
        m_stats.stagedPayloadPatchFacesLastFrame = patchedPayloadFaceCount;
        m_stats.stagedPayloadPatchRegionsLastFrame = patchedPayloadRegionCount;
        m_stats.stagedDirtyPayloadBricksLastFrame = copiedPayloadBrickCount;
        m_stats.skippedCleanPayloadBricksLastFrame = skippedCleanPayloadBrickCount;
        m_stats.deferredPayloadBricksLastFrame = deferredPayloadBrickCount;
        m_stats.pendingDirtyBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
        m_stats.pendingRemovedBricksLastFrame = static_cast<uint32_t>(snapshot.removedBricks.size());
        m_stats.stagedBytesLastFrame = endOffset - stageStartOffset;
        const auto& allocatorStats = m_faceRangeAllocator.GetStats();
        m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
        m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
        m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
        m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
        m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
        m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
        m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;

        ticket.rangeUploadOffset = rangeOffset;
        ticket.drawArgsUploadOffset = drawArgsOffset;
        ticket.surfaceRecordUploadOffset = surfaceRecordOffset;
        ticket.rangeBytes = rangeBytes;
        ticket.drawArgsBytes = drawArgsBytes;
        ticket.surfaceRecordBytes = surfaceRecordBytes;
        ticket.faceCount = drawableFaceCount;
        ticket.rangeCount = snapshot.rangeCount;
        ticket.rangeTableCapacity = static_cast<uint32_t>(ticket.rangeMirrorAfterCopy.size());
        ticket.drawCommandCount = static_cast<uint32_t>(ticket.drawArgsMirrorAfterCopy.size());
        ticket.activeDrawCommandCount = static_cast<uint32_t>(snapshot.drawBatches.size());
        ticket.deferredPayloadBricks = deferredPayloadBrickCount;

        if (outTicket) {
            *outTicket = std::move(ticket);
        }
        return true;
    }

    const uint64_t faceBytes =
        static_cast<uint64_t>(snapshot.faces.size()) * sizeof(Simulation::SparseSurfaceFace);
    const uint64_t rangeBytes =
        static_cast<uint64_t>(snapshot.ranges.size()) * sizeof(Simulation::SparseSurfaceBrickRange);
    const uint64_t drawArgsBytes =
        static_cast<uint64_t>(snapshot.drawArgs.size()) * sizeof(Simulation::SparseSurfaceDrawArgs);
    std::vector<Simulation::SparseSurfaceRecord> fallbackSurfaceRecords = snapshot.surfaceRecords;
    Simulation::SortSparseSurfaceRecordsForClusters(fallbackSurfaceRecords);
    const uint64_t surfaceRecordBytes =
        static_cast<uint64_t>(fallbackSurfaceRecords.size()) * sizeof(Simulation::SparseSurfaceRecord);
    std::vector<Simulation::SparseSurfaceClusterRecord> fallbackSurfaceClusters =
        Simulation::BuildSparseSurfaceClusters(
            fallbackSurfaceRecords,
            m_config.surfaceRecordsPerCluster,
            m_config.surfaceClusterMaxExtentVoxels);
    const uint64_t surfaceClusterBytes =
        static_cast<uint64_t>(fallbackSurfaceClusters.size()) * sizeof(Simulation::SparseSurfaceClusterRecord);
    const uint64_t faceOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t rangeOffset = AlignUp(faceOffset + faceBytes, kUploadAlignment);
    const uint64_t drawArgsOffset = AlignUp(rangeOffset + rangeBytes, kUploadAlignment);
    const uint64_t surfaceRecordOffset = AlignUp(drawArgsOffset + drawArgsBytes, kUploadAlignment);
    const uint64_t surfaceClusterOffset = AlignUp(surfaceRecordOffset + surfaceRecordBytes, kUploadAlignment);
    const uint64_t endOffset = surfaceClusterOffset + surfaceClusterBytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadOverflowLastFrame = true;
        return false;
    }

    if (faceBytes > 0) {
        std::memcpy(mapped + faceOffset, snapshot.faces.data(), static_cast<size_t>(faceBytes));
    }
    if (rangeBytes > 0) {
        std::memcpy(mapped + rangeOffset, snapshot.ranges.data(), static_cast<size_t>(rangeBytes));
    }
    if (drawArgsBytes > 0) {
        std::memcpy(mapped + drawArgsOffset, snapshot.drawArgs.data(), static_cast<size_t>(drawArgsBytes));
    }
    if (surfaceRecordBytes > 0) {
        std::memcpy(
            mapped + surfaceRecordOffset,
            fallbackSurfaceRecords.data(),
            static_cast<size_t>(surfaceRecordBytes));
    }
    if (surfaceClusterBytes > 0) {
        std::memcpy(
            mapped + surfaceClusterOffset,
            fallbackSurfaceClusters.data(),
            static_cast<size_t>(surfaceClusterBytes));
    }
    m_uploadWriteOffset = endOffset;
    m_stats.stagedFacesLastFrame = static_cast<uint32_t>(snapshot.faces.size());
    m_stats.stagedRangesLastFrame = snapshot.rangeCount;
    m_stats.stagedRangeTableCapacityLastFrame = static_cast<uint32_t>(snapshot.ranges.size());
    m_stats.stagedDrawCommandsLastFrame = snapshot.drawCommandCount;
    m_stats.stagedSurfaceRecordsLastFrame = static_cast<uint32_t>(fallbackSurfaceRecords.size());
    m_stats.stagedSurfaceClustersLastFrame = static_cast<uint32_t>(fallbackSurfaceClusters.size());
    m_stats.stagedRangeCopyRegionsLastFrame = rangeBytes > 0 ? 1u : 0u;
    m_stats.stagedDrawCopyRegionsLastFrame = drawArgsBytes > 0 ? 1u : 0u;
    m_stats.stagedSurfaceRecordCopyRegionsLastFrame = surfaceRecordBytes > 0 ? 1u : 0u;
    m_stats.stagedSurfaceClusterCopyRegionsLastFrame = surfaceClusterBytes > 0 ? 1u : 0u;
    m_stats.fullRangeTableUploadLastFrame = true;
    m_stats.fullDrawArgsUploadLastFrame = true;
    m_stats.fullSurfaceRecordUploadLastFrame = true;
    m_stats.fullSurfaceClusterUploadLastFrame = true;
    m_stats.stagedCandidateBricksLastFrame = snapshot.candidateBricks;
    m_stats.stagedVisibleBricksLastFrame = snapshot.visibleBricks;
    m_stats.stagedCulledBricksLastFrame = snapshot.culledBricks;
    m_stats.stagedBytesLastFrame = endOffset - faceOffset;
    m_stats.stagedDirtyPayloadBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
    m_stats.pendingDirtyBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
    m_stats.pendingRemovedBricksLastFrame = static_cast<uint32_t>(snapshot.removedBricks.size());

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->faceUploadOffset = faceOffset;
        outTicket->rangeUploadOffset = rangeOffset;
        outTicket->drawArgsUploadOffset = drawArgsOffset;
        outTicket->surfaceRecordUploadOffset = surfaceRecordOffset;
        outTicket->faceBytes = faceBytes;
        outTicket->rangeBytes = rangeBytes;
        outTicket->drawArgsBytes = drawArgsBytes;
        outTicket->surfaceRecordBytes = surfaceRecordBytes;
        if (surfaceClusterBytes > 0) {
            outTicket->surfaceClusterCopyRegions.push_back({
                surfaceClusterOffset,
                0u,
                surfaceClusterBytes
            });
        }
        outTicket->faceCount = static_cast<uint32_t>(snapshot.faces.size());
        outTicket->rangeCount = snapshot.rangeCount;
        outTicket->rangeTableCapacity = static_cast<uint32_t>(snapshot.ranges.size());
        outTicket->drawCommandCount = snapshot.drawCommandCount;
        outTicket->activeDrawCommandCount = snapshot.drawCommandCount;
        outTicket->serial = snapshot.serial;
        outTicket->candidateBricks = snapshot.candidateBricks;
        outTicket->visibleBricks = snapshot.visibleBricks;
        outTicket->culledBricks = snapshot.culledBricks;
        outTicket->rangeMirrorAfterCopy = snapshot.ranges;
        outTicket->drawArgsMirrorAfterCopy = snapshot.drawArgs;
        outTicket->surfaceRecordMirrorAfterCopy = std::move(fallbackSurfaceRecords);
        outTicket->surfaceClusterMirrorAfterCopy = std::move(fallbackSurfaceClusters);
        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> visibleDirtyCoords;
        visibleDirtyCoords.reserve(snapshot.dirtyBricks.size());
        for (const auto& dirty : snapshot.dirtyBricks) {
            if (dirty.serial <= snapshot.serial) {
                visibleDirtyCoords.insert(dirty.coord);
            }
        }
        outTicket->uploadedPayloadBricks.reserve(snapshot.drawBatches.size());
        for (const auto& batch : snapshot.drawBatches) {
            if (visibleDirtyCoords.find(batch.coord) != visibleDirtyCoords.end()) {
                outTicket->uploadedPayloadBricks.push_back(batch.coord);
            }
        }
        for (const auto& range : snapshot.ranges) {
            if (range.flags != 0u &&
                range.faceCount == 0u &&
                visibleDirtyCoords.find(range.coord) != visibleDirtyCoords.end()) {
                outTicket->uploadedPayloadBricks.push_back(range.coord);
            }
        }
        outTicket->removedBricks.reserve(snapshot.removedBricks.size());
        for (const auto& removed : snapshot.removedBricks) {
            if (removed.serial <= snapshot.serial) {
                outTicket->removedBricks.push_back(removed.coord);
            }
        }
    }
    return true;
}

bool SparseSurfaceGpuResources::EmitCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparseSurfaceUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        return false;
    }
    ID3D12Resource* upload = m_uploadRing[ticket.ringSlot].GetResource();
    if (!upload ||
        !m_faceBuffer.GetResource() ||
        !m_rangeBuffer.GetResource() ||
        !m_drawArgsBuffer.GetResource() ||
        !m_surfaceRecordBuffer.GetResource() ||
        !m_surfaceClusterBuffer.GetResource()) {
        return false;
    }

    if (m_staticIaUploadPending) {
        ID3D12Resource* vertexUpload = m_vertexIdStreamUpload.GetResource();
        ID3D12Resource* indexUpload = m_indexStreamUpload.GetResource();
        ID3D12Resource* vertexBuffer = m_vertexIdStream.GetResource();
        ID3D12Resource* indexBuffer = m_indexStream.GetResource();
        if (!vertexUpload || !indexUpload || !vertexBuffer || !indexBuffer) {
            return false;
        }

        m_vertexIdStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            vertexBuffer,
            0,
            vertexUpload,
            0,
            m_stats.iaStreamVertexBytes);
        m_vertexIdStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        m_indexStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            indexBuffer,
            0,
            indexUpload,
            0,
            m_stats.iaStreamIndexBytes);
        m_indexStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDEX_BUFFER);

        m_staticIaUploadPending = false;
        m_staticIaUploadComplete = true;
        m_staticIaUploadFence = m_currentFrameFenceValue;
        m_stats.iaStreamUploadPending = false;
        m_stats.iaStreamUploadRetireFence = m_staticIaUploadFence;
    }

    if (ticket.faceBytes > 0) {
        m_faceBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_faceBuffer.GetResource(),
            0,
            upload,
            ticket.faceUploadOffset,
            ticket.faceBytes);
        m_faceBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.faceCopyRegions.empty()) {
        m_faceBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceFaceCopyRegion& region : ticket.faceCopyRegions) {
            if (region.faceCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_faceBuffer.GetResource(),
                static_cast<uint64_t>(region.destFirstFace) * sizeof(Simulation::SparseSurfaceFace),
                upload,
                region.uploadOffset,
                static_cast<uint64_t>(region.faceCount) * sizeof(Simulation::SparseSurfaceFace));
        }
        m_faceBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (ticket.rangeBytes > 0) {
        m_rangeBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_rangeBuffer.GetResource(),
            0,
            upload,
            ticket.rangeUploadOffset,
            ticket.rangeBytes);
        m_rangeBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.rangeCopyRegions.empty()) {
        m_rangeBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.rangeCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_rangeBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_rangeBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (ticket.drawArgsBytes > 0) {
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_drawArgsBuffer.GetResource(),
            0,
            upload,
            ticket.drawArgsUploadOffset,
            ticket.drawArgsBytes);
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    } else if (!ticket.drawArgsCopyRegions.empty()) {
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.drawArgsCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_drawArgsBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    if (ticket.surfaceRecordBytes > 0) {
        m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_surfaceRecordBuffer.GetResource(),
            0,
            upload,
            ticket.surfaceRecordUploadOffset,
            ticket.surfaceRecordBytes);
        m_surfaceRecordBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.surfaceRecordCopyRegions.empty()) {
        m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.surfaceRecordCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_surfaceRecordBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_surfaceRecordBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (!ticket.surfaceClusterCopyRegions.empty()) {
        m_surfaceClusterBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.surfaceClusterCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_surfaceClusterBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_surfaceClusterBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    for (const Simulation::BrickCoord& coord : ticket.removedBricks) {
        m_payloadResidentCoords.erase(coord);
        m_payloadFaceMirrorByCoord.erase(coord);
    }
    for (const Simulation::BrickCoord& coord : ticket.uploadedPayloadBricks) {
        m_payloadResidentCoords.insert(coord);
    }
    for (const SparseSurfacePayloadMirrorUpdate& update : ticket.payloadMirrorUpdates) {
        if (update.faces.empty()) {
            m_payloadFaceMirrorByCoord.erase(update.coord);
        } else {
            m_payloadFaceMirrorByCoord[update.coord] = update.faces;
        }
    }
    m_rangeMirror = ticket.rangeMirrorAfterCopy;
    m_drawArgsMirror = ticket.drawArgsMirrorAfterCopy;
    m_surfaceRecordMirror = ticket.surfaceRecordMirrorAfterCopy;
    m_surfaceClusterMirror = ticket.surfaceClusterMirrorAfterCopy;
    if (m_config.useStableDrawSlots) {
        m_drawSlotByCoord = ticket.drawSlotByCoordAfterCopy;
        m_drawSlotOccupied = ticket.drawSlotOccupiedAfterCopy;
        m_freeDrawSlots = ticket.freeDrawSlotsAfterCopy;
    }

    m_stats.uploadedFaces = ticket.faceCount;
    m_stats.uploadedRanges = ticket.rangeCount;
    m_stats.uploadedRangeTableCapacity = ticket.rangeTableCapacity;
    m_stats.uploadedDrawCommands = ticket.drawCommandCount;
    m_stats.uploadedActiveDrawCommands = ticket.activeDrawCommandCount;
    m_stats.uploadedSurfaceRecords = static_cast<uint32_t>(m_surfaceRecordMirror.size());
    m_stats.uploadedSurfaceClusters = static_cast<uint32_t>(m_surfaceClusterMirror.size());
    m_stats.uploadedSerial = ticket.serial;
    m_stats.uploadedCandidateBricks = ticket.candidateBricks;
    m_stats.uploadedVisibleBricks = ticket.visibleBricks;
    m_stats.uploadedCulledBricks = ticket.culledBricks;
    m_stats.residentPayloadBricks = static_cast<uint32_t>(m_payloadResidentCoords.size());
    m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(m_drawSlotOccupied.size());
    m_stats.stableDrawFreeSlots = static_cast<uint32_t>(m_freeDrawSlots.size());
    return true;
}

bool SparseSurfaceGpuResources::DispatchGpuCull(
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
    float padding)
{
    if (!m_stats.initialized ||
        !commandList ||
        !m_config.useGpuCull ||
        !m_surfaceCullPipeline.IsValid() ||
        !m_surfaceRecordBuffer.GetResource() ||
        !m_surfaceClusterBuffer.GetResource() ||
        !m_drawArgsBuffer.GetResource() ||
        !m_drawCountBuffer.GetResource() ||
        !m_surfaceRecordBuffer.GetShaderVisibleSRV().IsValid() ||
        !m_surfaceClusterBuffer.GetShaderVisibleSRV().IsValid() ||
        !m_drawArgsBuffer.GetShaderVisibleUAV().IsValid() ||
        !m_drawCountBuffer.GetShaderVisibleUAV().IsValid()) {
        return false;
    }

    const uint32_t recordCount = m_stats.uploadedSurfaceRecords;
    const uint32_t clusterCount = m_stats.uploadedSurfaceClusters;
    m_stats.gpuCullCandidateRecordsLastFrame = recordCount;
    m_stats.gpuCullCandidateClustersLastFrame = clusterCount;
    m_stats.gpuCullMaxDrawCommands = m_config.maxDrawCommands;
    if (m_activeUploadSlot >= m_cullConstantUploads.size()) {
        return false;
    }

    struct CullConstants {
        float cameraPosition[4];
        float cameraForward[4];
        float cameraRight[4];
        float cameraUp[4];
        float params[4];
        float clusterParams[4];
    } constants = {};
    constants.cameraPosition[0] = cameraX;
    constants.cameraPosition[1] = cameraY;
    constants.cameraPosition[2] = cameraZ;
    constants.cameraPosition[3] = fovYRadians;
    constants.cameraForward[0] = forwardX;
    constants.cameraForward[1] = forwardY;
    constants.cameraForward[2] = forwardZ;
    constants.cameraForward[3] = aspectRatio;
    constants.cameraRight[0] = rightX;
    constants.cameraRight[1] = rightY;
    constants.cameraRight[2] = rightZ;
    constants.cameraRight[3] = 0.0f;
    constants.cameraUp[0] = upX;
    constants.cameraUp[1] = upY;
    constants.cameraUp[2] = upZ;
    constants.cameraUp[3] = 0.0f;
    constants.params[0] = static_cast<float>(recordCount);
    constants.params[1] = static_cast<float>(m_config.maxDrawCommands);
    constants.params[2] = maxDistance;
    constants.params[3] = padding;
    constants.clusterParams[0] = static_cast<float>(m_config.surfaceClusterFastAcceptMaxRecords);
    constants.clusterParams[1] = static_cast<float>(m_config.surfaceClusterFastAcceptMaxFaces);
    static_assert(sizeof(CullConstants) <= 256u);
    if (void* mapped = m_cullConstantUploads[m_activeUploadSlot].GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }

    if (!m_heapManager) {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_surfaceClusterBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT clearValues[4] = {0u, 0u, 0u, 0u};
    commandList->ClearUnorderedAccessViewUint(
        m_drawCountBuffer.GetShaderVisibleUAV().gpu,
        m_drawCountBuffer.GetStagingUAV().cpu,
        m_drawCountBuffer.GetResource(),
        clearValues,
        0,
        nullptr);

    D3D12_RESOURCE_BARRIER clearStatsBarrier = {};
    clearStatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    clearStatsBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    clearStatsBarrier.UAV.pResource = m_drawCountBuffer.GetResource();
    commandList->ResourceBarrier(1, &clearStatsBarrier);

    if (recordCount > 0u && clusterCount > 0u) {
        m_surfaceCullPipeline.Bind(commandList);
        m_surfaceCullPipeline.SetRootConstantBufferView(
            commandList,
            0,
            m_cullConstantUploads[m_activeUploadSlot].GetGPUVirtualAddress());
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            1,
            m_surfaceRecordBuffer.GetShaderVisibleSRV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            2,
            m_surfaceClusterBuffer.GetShaderVisibleSRV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            3,
            m_drawArgsBuffer.GetShaderVisibleUAV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            4,
            m_drawCountBuffer.GetShaderVisibleUAV().gpu);

        const uint32_t groupCount = clusterCount;
        m_surfaceCullPipeline.Dispatch(commandList, groupCount, 1u, 1u);
        m_stats.gpuCullDispatchesLastFrame = 1u;
    }

    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[0].UAV.pResource = m_drawArgsBuffer.GetResource();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[1].UAV.pResource = m_drawCountBuffer.GetResource();
    commandList->ResourceBarrier(2, uavBarriers);

    m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    return true;
}

void SparseSurfaceGpuResources::QueueGpuCullStatsReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized ||
        !commandList ||
        !m_config.useGpuCull ||
        !m_drawCountBuffer.GetResource()) {
        return;
    }
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_cullStatsReadback.size());
    if (m_cullStatsReadbackPending[slot] || !m_cullStatsReadback[slot].GetResource()) {
        return;
    }

    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(
        m_cullStatsReadback[slot].GetResource(),
        0,
        m_drawCountBuffer.GetResource(),
        0,
        sizeof(uint32_t) * kCullStatsUintCount);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_cullStatsReadbackPending[slot] = true;
    ++m_stats.gpuCullStatsReadbacksQueued;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
}

bool SparseSurfaceGpuResources::RetireGpuCullStatsReadback(uint32_t frameIndex)
{
    if (!m_stats.initialized || m_cullStatsReadback.empty()) {
        return false;
    }
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_cullStatsReadback.size());
    if (!m_cullStatsReadbackPending[slot] || !m_cullStatsReadback[slot].GetResource()) {
        return false;
    }

    const uint32_t* mapped = static_cast<const uint32_t*>(m_cullStatsReadback[slot].Map());
    if (!mapped) {
        return false;
    }
    m_stats.gpuCullAcceptedDraws = mapped[0];
    m_stats.gpuCullRejectedInvalid = mapped[1];
    m_stats.gpuCullRejectedDistance = mapped[2];
    m_stats.gpuCullRejectedFrustum = mapped[3];
    m_stats.gpuCullOverflow = mapped[4];
    m_stats.gpuCullCandidateRecordsLastFrame = mapped[5];
    m_stats.gpuCullMaxDrawCommands = mapped[6];
    m_stats.gpuCullRejectedClusters = mapped[7];
    m_stats.gpuCullFastAcceptedClusterRecords = mapped[8];
    m_stats.gpuCullAcceptedClusterDraws = mapped[9];
    m_stats.gpuCullAcceptedRecordDraws = mapped[10];
    m_stats.gpuCullRejectedBackface = mapped[11];
    m_stats.gpuCullStatsValid = true;
    m_cullStatsReadback[slot].Unmap();
    m_cullStatsReadbackPending[slot] = false;
    ++m_stats.gpuCullStatsReadbacksRetired;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
    return true;
}

} // namespace VENPOD::Graphics
