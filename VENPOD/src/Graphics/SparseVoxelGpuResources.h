#pragma once

#include "RHI/DescriptorHeap.h"
#include "RHI/GPUBuffer.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Simulation/SparseVoxelTypes.h"
#include "Utils/Result.h"

#include <array>
#include <cstdint>
#include <vector>

namespace VENPOD::Graphics {

struct SparseVoxelGpuConfig {
    uint32_t maxBrickPages = 4096;
    uint32_t pageTableCapacity = 16384;
    uint32_t uploadRingSlots = 3;
    uint32_t uploadBytesPerSlot = 4 * 1024 * 1024;
    uint32_t missFeedbackMaxRecords = 256;
    uint32_t midClipmapMaxTiles = 128;
    uint32_t midClipmapTileSampleSide = 33;
    uint32_t midVoxelClipmapMaxBricks = 128;
};

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
    uint64_t totalGpuBytes = 0;
    uint32_t stagedBricksLastFrame = 0;
    uint32_t stagedPageEntriesLastFrame = 0;
    uint64_t stagedBytesLastFrame = 0;
    bool uploadRingOverflowLastFrame = false;
    uint32_t stagedMidClipmapTilesLastFrame = 0;
    uint32_t stagedMidVoxelClipmapBricksLastFrame = 0;
    uint64_t stagedMidClipmapBytesLastFrame = 0;
    uint32_t missFeedbackRecordsLastRetire = 0;
    bool initialized = false;
};

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

struct SparseMidClipmapGpuUploadTicket {
    bool valid = false;
    bool uploadHeightLayer = false;
    bool uploadVoxelLayer = false;
    uint32_t ringSlot = 0;
    uint64_t metadataUploadOffset = 0;
    uint64_t lookupUploadOffset = 0;
    uint64_t samplesUploadOffset = 0;
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
    uint32_t tileCount = 0;
    uint32_t tileSampleSide = 0;
    uint32_t voxelBrickCount = 0;
    uint32_t snapshotSerial = 0;
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
    void BeginFrame(uint32_t frameIndex);
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
    bool EmitPageTableCopy(ID3D12GraphicsCommandList* commandList, const SparsePageTableGpuUploadTicket& ticket);
    bool StageMidClipmapSnapshot(
        const Simulation::SparseClipmapGpuSnapshot& snapshot,
        SparseMidClipmapGpuUploadTicket* outTicket = nullptr,
        bool uploadHeightLayer = true,
        bool uploadVoxelLayer = true);
    bool EmitMidClipmapCopy(ID3D12GraphicsCommandList* commandList, const SparseMidClipmapGpuUploadTicket& ticket);

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
    const DescriptorHandle& MissFeedbackUAV() const { return m_missFeedback.GetShaderVisibleUAV(); }

    void PrepareMissFeedbackWrite(ID3D12GraphicsCommandList* commandList);
    void QueueMissFeedbackReadback(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex);
    bool RetireMissFeedback(uint32_t frameIndex, std::vector<Simulation::BrickCoord>& outMissingBricks);

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
    GPUBuffer m_missFeedback;
    std::array<GPUBuffer, 3> m_missFeedbackReadback;
    std::array<UploadBuffer, 3> m_uploadRing;
    uint32_t m_activeUploadSlot = 0;
    uint64_t m_uploadWriteOffset = 0;
};

} // namespace VENPOD::Graphics
