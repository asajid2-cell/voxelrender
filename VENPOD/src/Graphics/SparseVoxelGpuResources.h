#pragma once

#include "RHI/DescriptorHeap.h"
#include "RHI/GPUBuffer.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Simulation/SparseVoxelTypes.h"
#include "Utils/Result.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace VENPOD::Graphics {

static constexpr uint32_t kSparseRenderOwnershipUnsafeSampleCapacity = 256u;
static constexpr uint32_t kSparseRenderOwnershipFarHeightMidSampleCapacity =
    kSparseRenderOwnershipUnsafeSampleCapacity;

struct SparseVoxelGpuConfig {
    uint32_t maxBrickPages = 24576;
    uint32_t pageTableCapacity = 65536;
    uint32_t uploadRingSlots = 3;
    uint32_t uploadBytesPerSlot = 16 * 1024 * 1024;
    uint32_t missFeedbackMaxRecords = 256;
    uint32_t midClipmapMaxTiles = 256;
    uint32_t midClipmapTileSampleSide = 33;
    uint32_t midVoxelClipmapMaxBricks = 512;
    uint32_t maxPhysicsWorkPackets = 2048;
    uint32_t maxEditDeltas = 8192;
    uint32_t maxEditDeltaRanges = 2048;
    uint32_t editDeltaRangeTableCapacity = 4096;
    uint32_t maxBrushFeedbackRecords = 8192;
};

inline bool IsSparseVoxelGpuPowerOfTwo(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

inline bool ValidateSparseVoxelGpuConfigForStats(const SparseVoxelGpuConfig& config) {
    constexpr uint32_t kMaxUploadRingSlots = 3;
    constexpr uint32_t kMaxGpuPhysicsWorkPackets = 2048;
    constexpr uint32_t kMaxGpuEditDeltas = 8192;
    constexpr uint32_t kMaxGpuEditDeltaRanges = 2048;

    if (config.maxBrickPages == 0 ||
        config.maxBrickPages > std::numeric_limits<uint32_t>::max() / 2u ||
        !IsSparseVoxelGpuPowerOfTwo(config.pageTableCapacity) ||
        config.pageTableCapacity < config.maxBrickPages * 2u ||
        config.uploadRingSlots == 0 ||
        config.uploadRingSlots > kMaxUploadRingSlots ||
        config.uploadBytesPerSlot == 0 ||
        config.missFeedbackMaxRecords == 0 ||
        config.missFeedbackMaxRecords == std::numeric_limits<uint32_t>::max() ||
        config.midClipmapMaxTiles == 0 ||
        config.midClipmapMaxTiles > std::numeric_limits<uint32_t>::max() / 4u ||
        config.midVoxelClipmapMaxBricks == 0 ||
        config.midVoxelClipmapMaxBricks > std::numeric_limits<uint32_t>::max() / 4u ||
        config.maxPhysicsWorkPackets == 0 ||
        config.maxPhysicsWorkPackets > kMaxGpuPhysicsWorkPackets ||
        config.maxEditDeltas == 0 ||
        config.maxEditDeltas > kMaxGpuEditDeltas ||
        config.maxEditDeltaRanges == 0 ||
        config.maxEditDeltaRanges > kMaxGpuEditDeltaRanges ||
        !IsSparseVoxelGpuPowerOfTwo(config.editDeltaRangeTableCapacity) ||
        config.editDeltaRangeTableCapacity < config.maxEditDeltaRanges * 2u ||
        config.maxBrushFeedbackRecords == 0 ||
        config.maxBrushFeedbackRecords == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    return true;
}

struct SparseVoxelGpuStats {
    uint32_t maxBrickPages = 0;
    uint32_t pageTableCapacity = 0;
    uint64_t brickPoolBytes = 0;
    uint64_t pageTableBytes = 0;
    uint64_t occupancyBytes = 0;
    uint64_t pageGenerationBytes = 0;
    uint64_t uploadRingBytes = 0;
    uint64_t missFeedbackBytes = 0;
    uint64_t midClipmapMetadataBytes = 0;
    uint64_t midClipmapLookupBytes = 0;
    uint64_t midClipmapSampleBytes = 0;
    uint64_t midVoxelClipmapMetadataBytes = 0;
    uint64_t midVoxelClipmapLookupBytes = 0;
    uint64_t midVoxelClipmapSampleBytes = 0;
    uint64_t physicsWorkPacketBytes = 0;
    uint64_t physicsPacketResultBytes = 0;
    uint64_t physicsDiagnosticBytes = 0;
    uint64_t renderOwnershipBytes = 0;
    uint64_t editDeltaBytes = 0;
    uint64_t editDeltaRangeBytes = 0;
    uint64_t editDeltaRangeTableBytes = 0;
    uint64_t brushFeedbackBytes = 0;
    uint64_t totalGpuBytes = 0;
    uint32_t stagedBricksLastFrame = 0;
    uint32_t stagedPartialBrickUploadsLastFrame = 0;
    uint32_t stagedPartialCopyRangesLastFrame = 0;
    uint64_t stagedPartialVoxelBytesLastFrame = 0;
    uint32_t stagedPageEntriesLastFrame = 0;
    uint64_t stagedBytesLastFrame = 0;
    bool uploadRingOverflowLastFrame = false;
    uint32_t stagedMidClipmapTilesLastFrame = 0;
    uint32_t stagedMidVoxelClipmapBricksLastFrame = 0;
    uint64_t stagedMidClipmapBytesLastFrame = 0;
    uint32_t stagedPhysicsPacketsLastFrame = 0;
    uint64_t stagedPhysicsPacketBytesLastFrame = 0;
    bool physicsPacketUploadOverflowLastFrame = false;
    uint32_t stagedEditDeltasLastFrame = 0;
    uint32_t stagedEditDeltaRangesLastFrame = 0;
    uint32_t stagedEditDeltaRangeTableEntriesLastFrame = 0;
    uint64_t stagedEditDeltaBytesLastFrame = 0;
    bool editDeltaUploadOverflowLastFrame = false;
    uint32_t physicsGpuPacketsLastRetire = 0;
    uint32_t physicsGpuMaterialMaskLastRetire = 0;
    uint32_t physicsGpuChecksumLastRetire = 0;
    uint32_t physicsGpuFrameLastRetire = 0;
    uint32_t physicsGpuMaxPriorityLastRetire = 0;
    uint32_t physicsGpuGenerationXorLastRetire = 0;
    uint32_t physicsGpuStaleFrameDropsLastRetire = 0;
    uint32_t physicsGpuResultCountLastRetire = 0;
    uint32_t physicsGpuResultChecksumLastRetire = 0;
    int32_t physicsGpuResultFirstBrickX = 0;
    int32_t physicsGpuResultFirstBrickY = 0;
    int32_t physicsGpuResultFirstBrickZ = 0;
    uint32_t physicsGpuResultFirstGeneration = 0;
    uint32_t physicsGpuResultFirstStatus = 0;
    uint32_t physicsGpuProposalCountLastRetire = 0;
    uint32_t physicsGpuMissingBelowCountLastRetire = 0;
    uint32_t physicsGpuMalformedStatusDropsLastRetire = 0;
    uint32_t physicsGpuUnexpectedPacketDropsLastRetire = 0;
    uint32_t physicsGpuChecksumDropsLastRetire = 0;
    uint32_t missFeedbackRecordsLastRetire = 0;
    uint32_t missFeedbackFrameLastRetire = 0;
    uint32_t missFeedbackStaleFrameDropsLastRetire = 0;
    bool missFeedbackOverflowLastRetire = false;
    uint32_t brushFeedbackRecordsLastRetire = 0;
    uint32_t brushFeedbackFrameLastRetire = 0;
    uint32_t brushFeedbackQueuedFrameLastRetire = 0;
    uint32_t brushFeedbackMissingResidentLastRetire = 0;
    uint32_t brushFeedbackStaleFrameDropsLastRetire = 0;
    bool brushFeedbackOverflowLastRetire = false;
    uint32_t renderOwnerTotalPixelsLastRetire = 0;
    uint32_t renderOwnerNearPixelsLastRetire = 0;
    uint32_t renderOwnerMidVoxelPixelsLastRetire = 0;
    uint32_t renderOwnerMidHeightPixelsLastRetire = 0;
    uint32_t renderOwnerFarSvoPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightPixelsLastRetire = 0;
    uint32_t renderOwnerFarWaterPixelsLastRetire = 0;
    uint32_t renderOwnerSkyPixelsLastRetire = 0;
    uint32_t renderOwnerMissPixelsLastRetire = 0;
    uint32_t renderOwnerSurfacePixelsLastRetire = 0;
    uint32_t renderOwnerFarSurfacePixelsLastRetire = 0;
    uint32_t renderOwnerExactSurfaceFragmentsLastRetire = 0;
    uint32_t renderOwnerMidSurfaceFragmentsLastRetire = 0;
    uint32_t renderOwnerProtectedMidSurfaceFragmentsLastRetire = 0;
    uint32_t renderOwnerProtectedExactSurfaceFragmentsLastRetire = 0;
    uint32_t renderOwnerProtectedMidVoxelPixelsLastRetire = 0;
    uint32_t renderOwnerProtectedFarSvoPixelsLastRetire = 0;
    uint32_t renderOwnerProtectedFarHeightPixelsLastRetire = 0;
    uint32_t renderOwnerUnsafeNearMissPixelsLastRetire = 0;
    uint32_t renderOwnerWaterContextPixelsLastRetire = 0;
    uint32_t renderOwnerValleyAtmospherePixelsLastRetire = 0;
    uint32_t renderOwnerLodParentHeldPixelsLastRetire = 0;
    uint32_t renderOwnerUnsafeMissSampleCountLastRetire = 0;
    int32_t renderOwnerUnsafeMissSampleBrickX = 0;
    int32_t renderOwnerUnsafeMissSampleBrickY = 0;
    int32_t renderOwnerUnsafeMissSampleBrickZ = 0;
    uint32_t renderOwnerUnsafeMissSampleDistanceLastRetire = 0;
    uint32_t renderOwnerUnsafeMissSampleStoredLastRetire = 0;
    std::array<Simulation::BrickCoord, kSparseRenderOwnershipUnsafeSampleCapacity>
        renderOwnerUnsafeMissSampleBricksLastRetire{};
    std::array<uint32_t, kSparseRenderOwnershipUnsafeSampleCapacity>
        renderOwnerUnsafeMissSampleDistancesLastRetire{};
    uint32_t renderOwnerMidInteriorFallbackPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightContinuityPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightMidMissingPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightMidAirPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightMidSolidPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightFarPagePresentPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightFarPageMissingPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightFarPageOutOfGridPixelsLastRetire = 0;
    uint32_t renderOwnerFarHeightMidSampleCountLastRetire = 0;
    uint32_t renderOwnerFarHeightMidSampleStoredLastRetire = 0;
    std::array<Simulation::SparseVoxelClipmapCoord, kSparseRenderOwnershipFarHeightMidSampleCapacity>
        renderOwnerFarHeightMidSamplesLastRetire{};
    uint32_t renderOwnerFrameLastRetire = 0;
    uint32_t renderOwnerStaleFrameDropsLastRetire = 0;
    bool initialized = false;
};

struct SparseBrickVoxelCopyRange {
    uint64_t uploadOffset = 0;
    uint64_t brickPoolOffset = 0;
    uint64_t bytes = 0;
};

inline bool IsSparseVoxelGpuByteRangeInBounds(
    uint64_t offset,
    uint64_t byteCount,
    uint64_t capacityBytes)
{
    return offset <= capacityBytes && byteCount <= capacityBytes - offset;
}

inline bool IsSparseVoxelGpuCopyRangeInBounds(
    uint64_t uploadOffset,
    uint64_t destOffset,
    uint64_t byteCount,
    uint64_t uploadCapacityBytes,
    uint64_t destCapacityBytes)
{
    return IsSparseVoxelGpuByteRangeInBounds(uploadOffset, byteCount, uploadCapacityBytes) &&
        IsSparseVoxelGpuByteRangeInBounds(destOffset, byteCount, destCapacityBytes);
}

inline bool IsSparseVoxelGpuBrickCopyRangeInBounds(
    const SparseBrickVoxelCopyRange& range,
    uint64_t uploadCapacityBytes,
    uint64_t brickPoolCapacityBytes)
{
    return IsSparseVoxelGpuCopyRangeInBounds(
        range.uploadOffset,
        range.brickPoolOffset,
        range.bytes,
        uploadCapacityBytes,
        brickPoolCapacityBytes);
}

struct SparseBrickGpuUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint64_t voxelUploadOffset = 0;
    uint64_t occupancyUploadOffset = 0;
    uint64_t generationUploadOffset = 0;
    uint64_t brickPoolOffset = 0;
    uint64_t occupancyBufferOffset = 0;
    uint64_t pageGenerationBufferOffset = 0;
    uint64_t voxelBytes = 0;
    uint64_t occupancyBytes = 0;
    uint64_t generationBytes = 0;
    std::vector<SparseBrickVoxelCopyRange> voxelCopyRanges;
    Simulation::BrickCoord coord;
    uint32_t pageIndex = Simulation::INVALID_BRICK_PAGE;
    uint32_t generation = 0;
};

struct SparsePageTableGpuUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint32_t entryIndex = 0;
    uint64_t uploadOffset = 0;
    uint64_t pageTableOffset = 0;
    uint64_t bytes = 0;
};

struct SparseMidClipmapSampleCopyRange {
    uint64_t uploadOffset = 0;
    uint64_t destinationOffset = 0;
    uint64_t bytes = 0;
};

struct SparseMidClipmapGpuUploadTicket {
    bool valid = false;
    bool uploadHeightLayer = false;
    bool uploadVoxelLayer = false;
    uint32_t ringSlot = 0;
    uint64_t metadataUploadOffset = 0;
    uint64_t lookupUploadOffset = 0;
    uint64_t samplesUploadOffset = 0;
    uint64_t samplesDestOffset = 0;
    uint64_t metadataBytes = 0;
    uint64_t lookupBytes = 0;
    uint64_t sampleBytes = 0;
    uint64_t voxelMetadataUploadOffset = 0;
    uint64_t voxelLookupUploadOffset = 0;
    uint64_t voxelSamplesUploadOffset = 0;
    uint64_t voxelSamplesDestOffset = 0;
    uint64_t voxelMetadataBytes = 0;
    uint64_t voxelLookupBytes = 0;
    uint64_t voxelSampleBytes = 0;
    std::vector<SparseMidClipmapSampleCopyRange> heightSampleCopyRanges;
    std::vector<SparseMidClipmapSampleCopyRange> voxelSampleCopyRanges;
    uint32_t tileCount = 0;
    uint32_t tileSampleSide = 0;
    uint32_t voxelBrickCount = 0;
    uint32_t snapshotSerial = 0;
};

struct SparsePhysicsPacketGpuUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint64_t uploadOffset = 0;
    uint64_t bytes = 0;
    uint32_t packetCount = 0;
};

struct SparseEditDeltaGpuUploadTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint64_t uploadOffset = 0;
    uint64_t rangeUploadOffset = 0;
    uint64_t rangeTableUploadOffset = 0;
    uint64_t bytes = 0;
    uint64_t rangeBytes = 0;
    uint64_t rangeTableBytes = 0;
    uint32_t deltaCount = 0;
    uint32_t rangeCount = 0;
    uint32_t rangeTableCapacity = 0;
    bool inputFullyRepresented = false;
};

class SparseVoxelGpuResources {
public:
    SparseVoxelGpuResources() = default;
    ~SparseVoxelGpuResources();

    SparseVoxelGpuResources(const SparseVoxelGpuResources&) = delete;
    SparseVoxelGpuResources& operator=(const SparseVoxelGpuResources&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        const SparseVoxelGpuConfig& config = {});
    void Shutdown();

    bool IsInitialized() const { return m_stats.initialized; }
    const SparseVoxelGpuStats& GetStats() const { return m_stats; }
    const std::vector<Simulation::SparsePhysicsPacketResult>& GetLastRetiredPhysicsProposals() const {
        return m_lastRetiredPhysicsProposals;
    }
    void BeginFrame(uint32_t frameIndex);
    uint64_t ActiveUploadBytesUsed() const { return m_uploadWriteOffset; }
    uint64_t ActiveUploadBytesCapacity() const;
    bool CanStageBrickUpload() const;
    bool CanStageBrickUpload(const Simulation::SparseBrickUploadPacket& packet) const;
    bool CanStagePageTableEntry() const;
    bool CanStagePageTableReset() const;
    bool CanStageMidClipmapSnapshot(
        const Simulation::SparseClipmapGpuSnapshot& snapshot,
        bool uploadHeightLayer = true,
        bool uploadVoxelLayer = true) const;
    static uint64_t EstimateMidClipmapSnapshotUploadBytes(
        const Simulation::SparseClipmapGpuSnapshot& snapshot,
        bool uploadHeightLayer = true,
        bool uploadVoxelLayer = true);
    bool CanStagePhysicsWorkPackets(
        const std::vector<Simulation::SparsePhysicsWorkPacket>& packets) const;
    bool CanStageEditDeltas(
        const std::vector<Simulation::SparseEditDelta>& deltas) const;
    bool StageBrickUpload(
        const Simulation::SparseBrickUploadPacket& packet,
        SparseBrickGpuUploadTicket* outTicket = nullptr);
    bool EmitUploadCopy(ID3D12GraphicsCommandList* commandList, const SparseBrickGpuUploadTicket& ticket);
    bool StagePageTableEntry(
        uint32_t entryIndex,
        const Simulation::BrickPageEntry& entry,
        SparsePageTableGpuUploadTicket* outTicket = nullptr);
    bool StagePageTableInvalidation(
        uint32_t entryIndex,
        SparsePageTableGpuUploadTicket* outTicket = nullptr);
    bool StagePageTableReset(SparsePageTableGpuUploadTicket* outTicket = nullptr);
    bool BeginPageTableCopyBatch(ID3D12GraphicsCommandList* commandList);
    void EndPageTableCopyBatch(ID3D12GraphicsCommandList* commandList);
    bool EmitPageTableCopy(
        ID3D12GraphicsCommandList* commandList,
        const SparsePageTableGpuUploadTicket& ticket,
        bool manageResourceState = true);
    bool StageMidClipmapSnapshot(
        const Simulation::SparseClipmapGpuSnapshot& snapshot,
        SparseMidClipmapGpuUploadTicket* outTicket = nullptr,
        bool uploadHeightLayer = true,
        bool uploadVoxelLayer = true);
    bool EmitMidClipmapCopy(ID3D12GraphicsCommandList* commandList, const SparseMidClipmapGpuUploadTicket& ticket);
    bool StagePhysicsWorkPackets(
        const std::vector<Simulation::SparsePhysicsWorkPacket>& packets,
        SparsePhysicsPacketGpuUploadTicket* outTicket = nullptr);
    bool EmitPhysicsPacketCopy(
        ID3D12GraphicsCommandList* commandList,
        const SparsePhysicsPacketGpuUploadTicket& ticket);
    bool StageEditDeltas(
        const std::vector<Simulation::SparseEditDelta>& deltas,
        SparseEditDeltaGpuUploadTicket* outTicket = nullptr);
    bool EmitEditDeltaCopy(
        ID3D12GraphicsCommandList* commandList,
        const SparseEditDeltaGpuUploadTicket& ticket);

    const DescriptorHandle& BrickPoolSRV() const { return m_brickPool.GetShaderVisibleSRV(); }
    const DescriptorHandle& BrickPoolUAV() const { return m_brickPool.GetShaderVisibleUAV(); }
    const DescriptorHandle& PageTableSRV() const { return m_pageTable.GetShaderVisibleSRV(); }
    const DescriptorHandle& PageTableUAV() const { return m_pageTable.GetShaderVisibleUAV(); }
    const DescriptorHandle& OccupancySRV() const { return m_occupancy.GetShaderVisibleSRV(); }
    const DescriptorHandle& OccupancyUAV() const { return m_occupancy.GetShaderVisibleUAV(); }
    const DescriptorHandle& PageGenerationSRV() const { return m_pageGeneration.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidClipmapMetadataSRV() const { return m_midClipmapMetadata.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidClipmapLookupSRV() const { return m_midClipmapLookup.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidClipmapSamplesSRV() const { return m_midClipmapSamples.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidVoxelClipmapMetadataSRV() const { return m_midVoxelClipmapMetadata.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidVoxelClipmapLookupSRV() const { return m_midVoxelClipmapLookup.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidVoxelClipmapSamplesSRV() const { return m_midVoxelClipmapSamples.GetShaderVisibleSRV(); }
    const DescriptorHandle& MidVoxelClipmapSamplesUAV() const { return m_midVoxelClipmapSamples.GetShaderVisibleUAV(); }
    // Phase 1 GPU mid-voxel generation: direct access to the sample pool buffer so
    // the MidVoxelGpuGenerator can transition it UAV<->SRV and bind it as a root
    // UAV by GPU virtual address.
    GPUBuffer& MidVoxelClipmapSamplesBuffer() { return m_midVoxelClipmapSamples; }
    const DescriptorHandle& PhysicsWorkPacketsSRV() const { return m_physicsWorkPackets.GetShaderVisibleSRV(); }
    const DescriptorHandle& EditDeltasSRV() const { return m_editDeltas.GetShaderVisibleSRV(); }
    const DescriptorHandle& EditDeltaRangesSRV() const { return m_editDeltaRanges.GetShaderVisibleSRV(); }
    const DescriptorHandle& EditDeltaRangeTableSRV() const { return m_editDeltaRangeTable.GetShaderVisibleSRV(); }
    const DescriptorHandle& PhysicsPacketResultsUAV() const { return m_physicsPacketResults.GetShaderVisibleUAV(); }
    const DescriptorHandle& PhysicsDiagnosticsUAV() const { return m_physicsDiagnostics.GetShaderVisibleUAV(); }
    const DescriptorHandle& MissFeedbackUAV() const { return m_missFeedback.GetShaderVisibleUAV(); }
    const DescriptorHandle& BrushFeedbackUAV() const { return m_brushFeedback.GetShaderVisibleUAV(); }
    const DescriptorHandle& RenderOwnershipUAV() const { return m_renderOwnership.GetShaderVisibleUAV(); }

    // Live edit-overlay bake: transition the brick pool + occupancy to UAV so a
    // compute pass can write edits straight into them, then back to the raymarch's
    // shader-resource state (with UAV barriers). Call Begin before the dispatch and
    // End after, with the bake as the last writer to the pool before the raymarch.
    void BeginEditDeltaBakeWrite(ID3D12GraphicsCommandList* commandList);
    void EndEditDeltaBakeWrite(ID3D12GraphicsCommandList* commandList);

    void PrepareMissFeedbackWrite(ID3D12GraphicsCommandList* commandList);
    void QueueMissFeedbackReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetireMissFeedback(uint32_t frameIndex, std::vector<Simulation::BrickCoord>& outMissingBricks);
    void PrepareBrushFeedbackWrite(ID3D12GraphicsCommandList* commandList);
    void QueueBrushFeedbackReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetireBrushFeedback(
        uint32_t frameIndex,
        std::vector<Simulation::SparseBrushFeedbackRecord>& outRecords);
    void PrepareRenderOwnershipWrite(ID3D12GraphicsCommandList* commandList);
    void QueueRenderOwnershipReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetireRenderOwnership(uint32_t frameIndex);
    void PreparePhysicsDiagnosticsWrite(ID3D12GraphicsCommandList* commandList);
    void QueuePhysicsDiagnosticsReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetirePhysicsDiagnostics(uint32_t frameIndex);
    void PreparePhysicsPacketResultsWrite(ID3D12GraphicsCommandList* commandList);
    void QueuePhysicsPacketResultsReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetirePhysicsPacketResults(uint32_t frameIndex);

    static SparseVoxelGpuStats ComputeStats(const SparseVoxelGpuConfig& config);
    static bool IsPowerOfTwo(uint32_t value);

private:
    SparseVoxelGpuConfig m_config;
    SparseVoxelGpuStats m_stats;

    GPUBuffer m_brickPool;
    GPUBuffer m_pageTable;
    GPUBuffer m_occupancy;
    GPUBuffer m_pageGeneration;
    GPUBuffer m_midClipmapMetadata;
    GPUBuffer m_midClipmapLookup;
    GPUBuffer m_midClipmapSamples;
    GPUBuffer m_midVoxelClipmapMetadata;
    GPUBuffer m_midVoxelClipmapLookup;
    GPUBuffer m_midVoxelClipmapSamples;
    GPUBuffer m_physicsWorkPackets;
    GPUBuffer m_editDeltas;
    GPUBuffer m_editDeltaRanges;
    GPUBuffer m_editDeltaRangeTable;
    GPUBuffer m_physicsPacketResults;
    GPUBuffer m_physicsDiagnostics;
    GPUBuffer m_missFeedback;
    GPUBuffer m_brushFeedback;
    GPUBuffer m_renderOwnership;
    std::vector<Simulation::SparsePhysicsPacketResult> m_lastRetiredPhysicsProposals;
    std::array<GPUBuffer, 3> m_physicsPacketResultsReadback;
    std::array<GPUBuffer, 3> m_physicsDiagnosticsReadback;
    std::array<GPUBuffer, 3> m_missFeedbackReadback;
    std::array<GPUBuffer, 3> m_brushFeedbackReadback;
    std::array<GPUBuffer, 3> m_renderOwnershipReadback;
    std::array<uint32_t, 3> m_physicsPacketResultsQueuedFrames = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    std::array<uint32_t, 3> m_physicsPacketResultCounts = {
        0u, 0u, 0u
    };
    std::array<std::vector<uint32_t>, 3> m_physicsPacketExpectedChecksums;
    std::vector<Simulation::SparsePhysicsWorkPacket> m_pendingPhysicsPacketResultPackets;
    std::array<uint32_t, 3> m_physicsDiagnosticsQueuedFrames = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    std::array<uint32_t, 3> m_missFeedbackQueuedFrames = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    std::array<uint32_t, 3> m_brushFeedbackQueuedFrames = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    std::array<uint32_t, 3> m_renderOwnershipQueuedFrames = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    std::array<UploadBuffer, 3> m_uploadRing;
    uint32_t m_activeUploadSlot = 0;
    uint64_t m_uploadWriteOffset = 0;
    uint32_t m_lastLoggedPhysicsDiagnosticFrame = UINT32_MAX;
    uint32_t m_lastLoggedPhysicsDiagnosticChecksum = 0;
    uint32_t m_lastLoggedPhysicsResultGeneration = UINT32_MAX;
    uint32_t m_lastLoggedPhysicsResultChecksum = 0;
};

} // namespace VENPOD::Graphics
