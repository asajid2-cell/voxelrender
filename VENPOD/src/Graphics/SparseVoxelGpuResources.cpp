#include "SparseVoxelGpuResources.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kMaxUploadRingSlots = 3;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

}

SparseVoxelGpuResources::~SparseVoxelGpuResources() {
    Shutdown();
}

Result<void> SparseVoxelGpuResources::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    const SparseVoxelGpuConfig& config)
{
    if (!device) {
        return Error("SparseVoxelGpuResources::Initialize - device is null");
    }
    if (config.maxBrickPages == 0) {
        return Error("SparseVoxelGpuResources::Initialize - maxBrickPages must be > 0");
    }
    if (!IsPowerOfTwo(config.pageTableCapacity)) {
        return Error("SparseVoxelGpuResources::Initialize - pageTableCapacity must be a power of two");
    }
    if (config.pageTableCapacity < config.maxBrickPages * 2u) {
        return Error("SparseVoxelGpuResources::Initialize - pageTableCapacity must be at least 2x maxBrickPages");
    }
    if (config.uploadRingSlots == 0 || config.uploadRingSlots > kMaxUploadRingSlots) {
        return Error("SparseVoxelGpuResources::Initialize - uploadRingSlots must be 1..{}", kMaxUploadRingSlots);
    }
    if (config.missFeedbackMaxRecords == 0) {
        return Error("SparseVoxelGpuResources::Initialize - missFeedbackMaxRecords must be > 0");
    }

    Shutdown();
    m_config = config;
    m_stats = ComputeStats(config);

    auto result = m_brickPool.Initialize(
        device,
        m_stats.brickPoolBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "SparseBrickVoxelPool");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse brick voxel pool: {}", result.error());
    }
    result = m_brickPool.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse brick pool SRV: {}", result.error());
    }
    result = m_brickPool.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse brick pool UAV: {}", result.error());
    }

    result = m_pageTable.Initialize(
        device,
        m_stats.pageTableBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(Simulation::BrickPageEntry),
        "SparseBrickPageTable");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse page table: {}", result.error());
    }
    result = m_pageTable.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse page table SRV: {}", result.error());
    }
    result = m_pageTable.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse page table UAV: {}", result.error());
    }

    result = m_occupancy.Initialize(
        device,
        m_stats.occupancyBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t) * 2u,
        "SparseBrickOccupancy");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse occupancy buffer: {}", result.error());
    }
    result = m_occupancy.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse occupancy SRV: {}", result.error());
    }
    result = m_occupancy.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse occupancy UAV: {}", result.error());
    }

    result = m_pageGeneration.Initialize(
        device,
        m_stats.pageGenerationBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t),
        "SparseBrickPageGenerations");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse page generation buffer: {}", result.error());
    }
    result = m_pageGeneration.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse page generation SRV: {}", result.error());
    }

    result = m_midClipmapMetadata.Initialize(
        device,
        m_stats.midClipmapMetadataBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t) * 4u,
        "SparseMidClipmapMetadata");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap metadata buffer: {}", result.error());
    }
    result = m_midClipmapMetadata.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap metadata SRV: {}", result.error());
    }

    result = m_midClipmapLookup.Initialize(
        device,
        m_stats.midClipmapLookupBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t) * 4u,
        "SparseMidClipmapLookup");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap lookup buffer: {}", result.error());
    }
    result = m_midClipmapLookup.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap lookup SRV: {}", result.error());
    }

    result = m_midClipmapSamples.Initialize(
        device,
        m_stats.midClipmapSampleBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t),
        "SparseMidClipmapSamples");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap sample buffer: {}", result.error());
    }
    result = m_midClipmapSamples.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid clipmap sample SRV: {}", result.error());
    }

    result = m_midVoxelClipmapMetadata.Initialize(
        device,
        m_stats.midVoxelClipmapMetadataBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t) * 4u,
        "SparseMidVoxelClipmapMetadata");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap metadata buffer: {}", result.error());
    }
    result = m_midVoxelClipmapMetadata.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap metadata SRV: {}", result.error());
    }

    result = m_midVoxelClipmapLookup.Initialize(
        device,
        m_stats.midVoxelClipmapLookupBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t) * 4u,
        "SparseMidVoxelClipmapLookup");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap lookup buffer: {}", result.error());
    }
    result = m_midVoxelClipmapLookup.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap lookup SRV: {}", result.error());
    }

    result = m_midVoxelClipmapSamples.Initialize(
        device,
        m_stats.midVoxelClipmapSampleBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t),
        "SparseMidVoxelClipmapSamples");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap sample buffer: {}", result.error());
    }
    result = m_midVoxelClipmapSamples.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap sample SRV: {}", result.error());
    }

    result = m_missFeedback.Initialize(
        device,
        m_stats.missFeedbackBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t) * 4u,
        "SparseMissFeedback");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse miss feedback buffer: {}", result.error());
    }
    result = m_missFeedback.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse miss feedback UAV: {}", result.error());
    }

    for (uint32_t i = 0; i < config.uploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparseBrickUploadRing_%u", i);
        result = m_uploadRing[i].Initialize(device, config.uploadBytesPerSlot, name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse upload ring slot {}: {}", i, result.error());
        }
    }
    for (uint32_t i = 0; i < kMaxUploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparseMissFeedbackReadback_%u", i);
        result = m_missFeedbackReadback[i].Initialize(
            device,
            m_stats.missFeedbackBytes,
            BufferUsage::Readback,
            sizeof(uint32_t) * 4u,
            name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse miss feedback readback slot {}: {}", i, result.error());
        }
    }

    m_stats.initialized = true;
    spdlog::info(
        "Sparse GPU resources initialized: pages={} pageTable={} brickPool={:.1f} MB occupancy={:.2f} MB midHeight={:.2f} MB midVoxel={:.2f} MB feedback={:.2f} MB uploadRing={:.1f} MB total={:.1f} MB",
        m_stats.maxBrickPages,
        m_stats.pageTableCapacity,
        static_cast<double>(m_stats.brickPoolBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.occupancyBytes) / (1024.0 * 1024.0),
        static_cast<double>(
            m_stats.midClipmapMetadataBytes +
            m_stats.midClipmapLookupBytes +
            m_stats.midClipmapSampleBytes) / (1024.0 * 1024.0),
        static_cast<double>(
            m_stats.midVoxelClipmapMetadataBytes +
            m_stats.midVoxelClipmapLookupBytes +
            m_stats.midVoxelClipmapSampleBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.missFeedbackBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.uploadRingBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.totalGpuBytes) / (1024.0 * 1024.0));

    return {};
}

void SparseVoxelGpuResources::Shutdown() {
    for (auto& upload : m_uploadRing) {
        upload.Shutdown();
    }
    for (auto& readback : m_missFeedbackReadback) {
        readback.Shutdown();
    }
    m_missFeedback.Shutdown();
    m_midVoxelClipmapSamples.Shutdown();
    m_midVoxelClipmapLookup.Shutdown();
    m_midVoxelClipmapMetadata.Shutdown();
    m_midClipmapSamples.Shutdown();
    m_midClipmapLookup.Shutdown();
    m_midClipmapMetadata.Shutdown();
    m_pageGeneration.Shutdown();
    m_occupancy.Shutdown();
    m_pageTable.Shutdown();
    m_brickPool.Shutdown();
    m_stats = {};
    m_activeUploadSlot = 0;
    m_uploadWriteOffset = 0;
}

void SparseVoxelGpuResources::BeginFrame(uint32_t frameIndex) {
    if (!m_stats.initialized || m_config.uploadRingSlots == 0) {
        return;
    }

    m_activeUploadSlot = frameIndex % m_config.uploadRingSlots;
    m_uploadWriteOffset = 0;
    m_stats.stagedBricksLastFrame = 0;
    m_stats.stagedPageEntriesLastFrame = 0;
    m_stats.stagedBytesLastFrame = 0;
    m_stats.uploadRingOverflowLastFrame = false;
    m_stats.stagedMidClipmapTilesLastFrame = 0;
    m_stats.stagedMidVoxelClipmapBricksLastFrame = 0;
    m_stats.stagedMidClipmapBytesLastFrame = 0;
}

void SparseVoxelGpuResources::PrepareMissFeedbackWrite(ID3D12GraphicsCommandList* commandList) {
    if (!m_stats.initialized || !commandList) {
        return;
    }
    m_missFeedback.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void SparseVoxelGpuResources::QueueMissFeedbackReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized || !commandList) {
        return;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_missFeedback.GetResource();
    commandList->ResourceBarrier(1, &uavBarrier);

    m_missFeedback.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    GPUBuffer& readback = m_missFeedbackReadback[frameIndex % m_missFeedbackReadback.size()];
    commandList->CopyResource(readback.GetResource(), m_missFeedback.GetResource());
}

bool SparseVoxelGpuResources::RetireMissFeedback(
    uint32_t frameIndex,
    std::vector<Simulation::BrickCoord>& outMissingBricks)
{
    m_stats.missFeedbackRecordsLastRetire = 0;
    if (!m_stats.initialized) {
        return false;
    }

    GPUBuffer& readback = m_missFeedbackReadback[frameIndex % m_missFeedbackReadback.size()];
    const uint32_t* mapped = static_cast<const uint32_t*>(readback.Map());
    if (!mapped) {
        return false;
    }

    const uint32_t reported = mapped[0];
    const uint32_t count = std::min(reported, m_config.missFeedbackMaxRecords);
    outMissingBricks.reserve(outMissingBricks.size() + count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* record = mapped + (static_cast<size_t>(i + 1u) * 4u);
        outMissingBricks.push_back({
            static_cast<int32_t>(record[0]),
            static_cast<int32_t>(record[1]),
            static_cast<int32_t>(record[2])
        });
    }
    m_stats.missFeedbackRecordsLastRetire = count;
    return true;
}

bool SparseVoxelGpuResources::StageBrickUpload(
    const Simulation::SparseBrickUploadPacket& packet,
    SparseBrickGpuUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || packet.pageIndex == Simulation::INVALID_BRICK_PAGE || packet.generation == 0) {
        return false;
    }
    if (m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t voxelBytes =
        static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) * sizeof(uint32_t);
    const uint64_t occupancyBytes = sizeof(uint32_t) * 2u;
    const uint64_t voxelOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t occupancyOffset = AlignUp(voxelOffset + voxelBytes, kUploadAlignment);
    const uint64_t generationOffset = AlignUp(occupancyOffset + occupancyBytes, kUploadAlignment);
    const uint64_t generationBytes = sizeof(uint32_t);
    const uint64_t endOffset = generationOffset + generationBytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    std::memcpy(mapped + voxelOffset, packet.brick.voxels.data(), static_cast<size_t>(voxelBytes));
    uint32_t occupancyWords[2] = {
        packet.brick.occupancyWord0,
        packet.brick.occupancyWord1
    };
    std::memcpy(mapped + occupancyOffset, occupancyWords, sizeof(occupancyWords));
    std::memcpy(mapped + generationOffset, &packet.generation, sizeof(packet.generation));

    m_uploadWriteOffset = endOffset;
    ++m_stats.stagedBricksLastFrame;
    m_stats.stagedBytesLastFrame += endOffset - voxelOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->voxelUploadOffset = voxelOffset;
        outTicket->occupancyUploadOffset = occupancyOffset;
        outTicket->generationUploadOffset = generationOffset;
        outTicket->brickPoolOffset =
            static_cast<uint64_t>(packet.pageIndex) *
            static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
            sizeof(uint32_t);
        outTicket->occupancyBufferOffset =
            static_cast<uint64_t>(packet.pageIndex) * sizeof(uint32_t) * 2u;
        outTicket->pageGenerationBufferOffset =
            static_cast<uint64_t>(packet.pageIndex) * sizeof(uint32_t);
        outTicket->voxelBytes = voxelBytes;
        outTicket->occupancyBytes = occupancyBytes;
        outTicket->generationBytes = generationBytes;
        outTicket->coord = packet.coord;
        outTicket->pageIndex = packet.pageIndex;
        outTicket->generation = packet.generation;
    }
    return true;
}

bool SparseVoxelGpuResources::StagePageTableEntry(
    uint32_t entryIndex,
    const Simulation::BrickPageEntry& entry,
    SparsePageTableGpuUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || entryIndex >= m_config.pageTableCapacity) {
        return false;
    }
    if (entry.pageIndex == Simulation::INVALID_BRICK_PAGE || entry.generation == 0) {
        return false;
    }
    if (m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t bytes = sizeof(Simulation::BrickPageEntry);
    const uint64_t uploadOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t endOffset = uploadOffset + bytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    std::memcpy(mapped + uploadOffset, &entry, sizeof(entry));
    m_uploadWriteOffset = endOffset;
    ++m_stats.stagedPageEntriesLastFrame;
    m_stats.stagedBytesLastFrame += endOffset - uploadOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->entryIndex = entryIndex;
        outTicket->uploadOffset = uploadOffset;
        outTicket->pageTableOffset =
            static_cast<uint64_t>(entryIndex) * sizeof(Simulation::BrickPageEntry);
        outTicket->bytes = bytes;
    }
    return true;
}

bool SparseVoxelGpuResources::StagePageTableInvalidation(
    uint32_t entryIndex,
    SparsePageTableGpuUploadTicket* outTicket)
{
    if (entryIndex >= m_config.pageTableCapacity) {
        if (outTicket) {
            *outTicket = {};
        }
        return false;
    }

    Simulation::BrickPageEntry invalidEntry = {};
    invalidEntry.pageIndex = Simulation::INVALID_BRICK_PAGE;
    invalidEntry.generation = 0;

    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t bytes = sizeof(Simulation::BrickPageEntry);
    const uint64_t uploadOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t endOffset = uploadOffset + bytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    std::memcpy(mapped + uploadOffset, &invalidEntry, sizeof(invalidEntry));
    m_uploadWriteOffset = endOffset;
    ++m_stats.stagedPageEntriesLastFrame;
    m_stats.stagedBytesLastFrame += endOffset - uploadOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->entryIndex = entryIndex;
        outTicket->uploadOffset = uploadOffset;
        outTicket->pageTableOffset =
            static_cast<uint64_t>(entryIndex) * sizeof(Simulation::BrickPageEntry);
        outTicket->bytes = bytes;
    }
    return true;
}

bool SparseVoxelGpuResources::StagePageTableReset(SparsePageTableGpuUploadTicket* outTicket) {
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t bytes = m_stats.pageTableBytes;
    const uint64_t uploadOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t endOffset = uploadOffset + bytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    Simulation::BrickPageEntry invalidEntry = {};
    invalidEntry.pageIndex = Simulation::INVALID_BRICK_PAGE;
    auto* entries = reinterpret_cast<Simulation::BrickPageEntry*>(mapped + uploadOffset);
    for (uint32_t i = 0; i < m_config.pageTableCapacity; ++i) {
        entries[i] = invalidEntry;
    }

    m_uploadWriteOffset = endOffset;
    m_stats.stagedPageEntriesLastFrame += m_config.pageTableCapacity;
    m_stats.stagedBytesLastFrame += endOffset - uploadOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->entryIndex = 0;
        outTicket->uploadOffset = uploadOffset;
        outTicket->pageTableOffset = 0;
        outTicket->bytes = bytes;
    }
    return true;
}

bool SparseVoxelGpuResources::StageMidClipmapSnapshot(
    const Simulation::SparseClipmapGpuSnapshot& snapshot,
    SparseMidClipmapGpuUploadTicket* outTicket,
    bool uploadHeightLayer,
    bool uploadVoxelLayer)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || (!uploadHeightLayer && !uploadVoxelLayer)) {
        return false;
    }
    uploadHeightLayer =
        uploadHeightLayer &&
        snapshot.tileCount > 0 &&
        !snapshot.metadata.empty() &&
        !snapshot.lookup.empty() &&
        !snapshot.samples.empty();
    uploadVoxelLayer =
        uploadVoxelLayer &&
        snapshot.voxelBrickCount > 0 &&
        !snapshot.voxelMetadata.empty() &&
        !snapshot.voxelLookup.empty() &&
        !snapshot.voxelSamples.empty();
    if (!uploadHeightLayer && !uploadVoxelLayer) {
        return false;
    }
    if (m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    const uint64_t metadataUints = uploadHeightLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.metadata.size()),
            static_cast<uint64_t>(snapshot.tileCount + 1u) * 4u)
        : 0u;
    const uint64_t lookupUints = uploadHeightLayer
        ? static_cast<uint64_t>(snapshot.lookup.size())
        : 0u;
    const uint64_t sampleUints = uploadHeightLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.samples.size()),
            static_cast<uint64_t>(snapshot.tileCount) *
                static_cast<uint64_t>(snapshot.tileSampleSide) *
                static_cast<uint64_t>(snapshot.tileSampleSide))
        : 0u;
    const uint64_t voxelMetadataUints = uploadVoxelLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.voxelMetadata.size()),
            static_cast<uint64_t>(snapshot.voxelBrickCount + 1u) * 4u)
        : 0u;
    const uint64_t voxelLookupUints = uploadVoxelLayer
        ? static_cast<uint64_t>(snapshot.voxelLookup.size())
        : 0u;
    uint32_t voxelSampleStartSlot = 0;
    uint32_t voxelSampleSlotCount = uploadVoxelLayer ? snapshot.voxelBrickCount : 0u;
    if (uploadVoxelLayer && snapshot.voxelDirtySlotCount > 0) {
        voxelSampleStartSlot = std::min(snapshot.voxelDirtyStartSlot, snapshot.voxelBrickCount);
        const uint32_t maxDirtyCount = snapshot.voxelBrickCount - voxelSampleStartSlot;
        voxelSampleSlotCount = std::min(snapshot.voxelDirtySlotCount, maxDirtyCount);
    }
    const uint64_t voxelSampleUints = uploadVoxelLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.voxelSamples.size()),
            static_cast<uint64_t>(voxelSampleSlotCount) *
                static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT))
        : 0u;
    const uint64_t metadataBytes = metadataUints * sizeof(uint32_t);
    const uint64_t lookupBytes = lookupUints * sizeof(uint32_t);
    const uint64_t sampleBytes = sampleUints * sizeof(uint32_t);
    const uint64_t voxelMetadataBytes = voxelMetadataUints * sizeof(uint32_t);
    const uint64_t voxelLookupBytes = voxelLookupUints * sizeof(uint32_t);
    const uint64_t voxelSampleBytes = voxelSampleUints * sizeof(uint32_t);
    if (metadataBytes > m_stats.midClipmapMetadataBytes ||
        lookupBytes > m_stats.midClipmapLookupBytes ||
        sampleBytes > m_stats.midClipmapSampleBytes ||
        voxelMetadataBytes > m_stats.midVoxelClipmapMetadataBytes ||
        voxelLookupBytes > m_stats.midVoxelClipmapLookupBytes ||
        voxelSampleBytes > m_stats.midVoxelClipmapSampleBytes) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t metadataOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t lookupOffset = AlignUp(metadataOffset + metadataBytes, kUploadAlignment);
    const uint64_t samplesOffset = AlignUp(lookupOffset + lookupBytes, kUploadAlignment);
    const uint64_t voxelMetadataOffset = AlignUp(samplesOffset + sampleBytes, kUploadAlignment);
    const uint64_t voxelLookupOffset = AlignUp(voxelMetadataOffset + voxelMetadataBytes, kUploadAlignment);
    const uint64_t voxelSamplesOffset = AlignUp(voxelLookupOffset + voxelLookupBytes, kUploadAlignment);
    const uint64_t endOffset = voxelSamplesOffset + voxelSampleBytes;
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    if (metadataBytes > 0) {
        std::memcpy(mapped + metadataOffset, snapshot.metadata.data(), static_cast<size_t>(metadataBytes));
        std::memcpy(mapped + lookupOffset, snapshot.lookup.data(), static_cast<size_t>(lookupBytes));
        std::memcpy(mapped + samplesOffset, snapshot.samples.data(), static_cast<size_t>(sampleBytes));
    }
    if (voxelMetadataBytes > 0) {
        std::memcpy(mapped + voxelMetadataOffset, snapshot.voxelMetadata.data(), static_cast<size_t>(voxelMetadataBytes));
        std::memcpy(mapped + voxelLookupOffset, snapshot.voxelLookup.data(), static_cast<size_t>(voxelLookupBytes));
        const size_t voxelSampleSourceOffset =
            static_cast<size_t>(voxelSampleStartSlot) *
            static_cast<size_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT);
        std::memcpy(
            mapped + voxelSamplesOffset,
            snapshot.voxelSamples.data() + voxelSampleSourceOffset,
            static_cast<size_t>(voxelSampleBytes));
    }
    m_uploadWriteOffset = endOffset;
    m_stats.stagedBytesLastFrame += endOffset - metadataOffset;
    m_stats.stagedMidClipmapTilesLastFrame = uploadHeightLayer ? snapshot.tileCount : 0u;
    m_stats.stagedMidVoxelClipmapBricksLastFrame = uploadVoxelLayer ? voxelSampleSlotCount : 0u;
    m_stats.stagedMidClipmapBytesLastFrame =
        metadataBytes + lookupBytes + sampleBytes +
        voxelMetadataBytes + voxelLookupBytes + voxelSampleBytes;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->uploadHeightLayer = uploadHeightLayer;
        outTicket->uploadVoxelLayer = uploadVoxelLayer;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->metadataUploadOffset = metadataOffset;
        outTicket->lookupUploadOffset = lookupOffset;
        outTicket->samplesUploadOffset = samplesOffset;
        outTicket->metadataBytes = metadataBytes;
        outTicket->lookupBytes = lookupBytes;
        outTicket->sampleBytes = sampleBytes;
        outTicket->voxelMetadataUploadOffset = voxelMetadataOffset;
        outTicket->voxelLookupUploadOffset = voxelLookupOffset;
        outTicket->voxelSamplesUploadOffset = voxelSamplesOffset;
        outTicket->voxelSamplesDestOffset =
            static_cast<uint64_t>(voxelSampleStartSlot) *
            static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
            sizeof(uint32_t);
        outTicket->voxelMetadataBytes = voxelMetadataBytes;
        outTicket->voxelLookupBytes = voxelLookupBytes;
        outTicket->voxelSampleBytes = voxelSampleBytes;
        outTicket->tileCount = snapshot.tileCount;
        outTicket->tileSampleSide = snapshot.tileSampleSide;
        outTicket->voxelBrickCount = snapshot.voxelBrickCount;
        outTicket->snapshotSerial = snapshot.frameIndex;
    }
    return true;
}

bool SparseVoxelGpuResources::EmitPageTableCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparsePageTableGpuUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        return false;
    }

    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource || !m_pageTable.GetResource()) {
        return false;
    }

    m_pageTable.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_pageTable.GetResource(),
        ticket.pageTableOffset,
        uploadResource,
        ticket.uploadOffset,
        ticket.bytes);
    m_pageTable.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool SparseVoxelGpuResources::EmitMidClipmapCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparseMidClipmapGpuUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        return false;
    }

    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource) {
        return false;
    }
    if ((ticket.uploadHeightLayer &&
            (!m_midClipmapMetadata.GetResource() ||
             !m_midClipmapLookup.GetResource() ||
             !m_midClipmapSamples.GetResource())) ||
        (ticket.uploadVoxelLayer &&
            (!m_midVoxelClipmapMetadata.GetResource() ||
             !m_midVoxelClipmapLookup.GetResource() ||
             !m_midVoxelClipmapSamples.GetResource()))) {
        return false;
    }
    if (ticket.uploadHeightLayer) {
        if (ticket.metadataBytes > m_midClipmapMetadata.GetSize() ||
            ticket.lookupBytes > m_midClipmapLookup.GetSize() ||
            ticket.sampleBytes > m_midClipmapSamples.GetSize()) {
            return false;
        }
    }
    if (ticket.uploadVoxelLayer) {
        if (ticket.voxelMetadataBytes > m_midVoxelClipmapMetadata.GetSize() ||
            ticket.voxelLookupBytes > m_midVoxelClipmapLookup.GetSize() ||
            ticket.voxelSampleBytes > m_midVoxelClipmapSamples.GetSize()) {
            return false;
        }
    }

    if (ticket.uploadHeightLayer) {
        m_midClipmapMetadata.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        m_midClipmapLookup.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        m_midClipmapSamples.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_midClipmapMetadata.GetResource(),
            0,
            uploadResource,
            ticket.metadataUploadOffset,
            ticket.metadataBytes);
        commandList->CopyBufferRegion(
            m_midClipmapLookup.GetResource(),
            0,
            uploadResource,
            ticket.lookupUploadOffset,
            ticket.lookupBytes);
        commandList->CopyBufferRegion(
            m_midClipmapSamples.GetResource(),
            0,
            uploadResource,
            ticket.samplesUploadOffset,
            ticket.sampleBytes);
        m_midClipmapMetadata.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_midClipmapLookup.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_midClipmapSamples.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (ticket.uploadVoxelLayer) {
        m_midVoxelClipmapMetadata.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        m_midVoxelClipmapLookup.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        m_midVoxelClipmapSamples.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_midVoxelClipmapMetadata.GetResource(),
            0,
            uploadResource,
            ticket.voxelMetadataUploadOffset,
            ticket.voxelMetadataBytes);
        commandList->CopyBufferRegion(
            m_midVoxelClipmapLookup.GetResource(),
            0,
            uploadResource,
            ticket.voxelLookupUploadOffset,
            ticket.voxelLookupBytes);
        commandList->CopyBufferRegion(
            m_midVoxelClipmapSamples.GetResource(),
            ticket.voxelSamplesDestOffset,
            uploadResource,
            ticket.voxelSamplesUploadOffset,
            ticket.voxelSampleBytes);
        m_midVoxelClipmapMetadata.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_midVoxelClipmapLookup.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_midVoxelClipmapSamples.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    return true;
}

bool SparseVoxelGpuResources::EmitUploadCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparseBrickGpuUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        return false;
    }

    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource || !m_brickPool.GetResource() || !m_occupancy.GetResource() ||
        !m_pageGeneration.GetResource()) {
        return false;
    }

    m_brickPool.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_occupancy.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_pageGeneration.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyBufferRegion(
        m_brickPool.GetResource(),
        ticket.brickPoolOffset,
        uploadResource,
        ticket.voxelUploadOffset,
        ticket.voxelBytes);
    commandList->CopyBufferRegion(
        m_occupancy.GetResource(),
        ticket.occupancyBufferOffset,
        uploadResource,
        ticket.occupancyUploadOffset,
        ticket.occupancyBytes);
    commandList->CopyBufferRegion(
        m_pageGeneration.GetResource(),
        ticket.pageGenerationBufferOffset,
        uploadResource,
        ticket.generationUploadOffset,
        ticket.generationBytes);

    m_brickPool.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_occupancy.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_pageGeneration.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

SparseVoxelGpuStats SparseVoxelGpuResources::ComputeStats(const SparseVoxelGpuConfig& config) {
    SparseVoxelGpuStats stats;
    stats.maxBrickPages = config.maxBrickPages;
    stats.pageTableCapacity = config.pageTableCapacity;
    stats.brickPoolBytes =
        static_cast<uint64_t>(config.maxBrickPages) *
        static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
        sizeof(uint32_t);
    stats.pageTableBytes =
        static_cast<uint64_t>(config.pageTableCapacity) *
        sizeof(Simulation::BrickPageEntry);
    stats.occupancyBytes =
        static_cast<uint64_t>(config.maxBrickPages) *
        sizeof(uint32_t) * 2u;
    stats.pageGenerationBytes =
        static_cast<uint64_t>(config.maxBrickPages) *
        sizeof(uint32_t);
    const uint32_t midTileSide = std::clamp(config.midClipmapTileSampleSide, 9u, 65u);
    const uint32_t midMaxTiles = std::max(1u, config.midClipmapMaxTiles);
    stats.midClipmapMetadataBytes =
        static_cast<uint64_t>(midMaxTiles + 1u) *
        sizeof(uint32_t) * 4u;
    uint32_t midLookupCapacity = 16u;
    while (midLookupCapacity < midMaxTiles * 4u) {
        midLookupCapacity <<= 1u;
    }
    stats.midClipmapLookupBytes =
        static_cast<uint64_t>(midLookupCapacity) *
        sizeof(uint32_t) * 4u;
    stats.midClipmapSampleBytes =
        static_cast<uint64_t>(midMaxTiles) *
        static_cast<uint64_t>(midTileSide) *
        static_cast<uint64_t>(midTileSide) *
        sizeof(uint32_t);
    const uint32_t midMaxVoxelBricks = std::max(1u, config.midVoxelClipmapMaxBricks);
    uint32_t midVoxelLookupCapacity = 16u;
    while (midVoxelLookupCapacity < midMaxVoxelBricks * 4u) {
        midVoxelLookupCapacity <<= 1u;
    }
    stats.midVoxelClipmapMetadataBytes =
        static_cast<uint64_t>(midMaxVoxelBricks + 1u) *
        sizeof(uint32_t) * 4u;
    stats.midVoxelClipmapLookupBytes =
        static_cast<uint64_t>(midVoxelLookupCapacity) *
        sizeof(uint32_t) * 4u;
    stats.midVoxelClipmapSampleBytes =
        static_cast<uint64_t>(midMaxVoxelBricks) *
        static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
        sizeof(uint32_t);
    stats.missFeedbackBytes =
        static_cast<uint64_t>(config.missFeedbackMaxRecords + 1u) *
        sizeof(uint32_t) * 4u;
    stats.uploadRingBytes =
        static_cast<uint64_t>(std::min(config.uploadRingSlots, kMaxUploadRingSlots)) *
        static_cast<uint64_t>(config.uploadBytesPerSlot);
    stats.totalGpuBytes =
        stats.brickPoolBytes +
        stats.pageTableBytes +
        stats.occupancyBytes +
        stats.pageGenerationBytes +
        stats.midClipmapMetadataBytes +
        stats.midClipmapLookupBytes +
        stats.midClipmapSampleBytes +
        stats.midVoxelClipmapMetadataBytes +
        stats.midVoxelClipmapLookupBytes +
        stats.midVoxelClipmapSampleBytes +
        stats.missFeedbackBytes +
        stats.uploadRingBytes;
    return stats;
}

bool SparseVoxelGpuResources::IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

} // namespace VENPOD::Graphics
