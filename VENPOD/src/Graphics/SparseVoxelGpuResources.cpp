#include "SparseVoxelGpuResources.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kMaxUploadRingSlots = 3;
constexpr uint32_t kSparsePhysicsDiagnosticWords = 8;
constexpr uint32_t kSparseRenderOwnershipBaseWords = 40;
constexpr uint32_t kSparseRenderOwnershipWords =
    kSparseRenderOwnershipBaseWords +
    kSparseRenderOwnershipUnsafeSampleCapacity * 4u +
    kSparseRenderOwnershipFarHeightMidSampleCapacity * 4u;
constexpr uint32_t kSparsePhysicsResultKnownStatusMask =
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW |
    Simulation::SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const uint64_t addend = alignment - 1u;
    if (value > std::numeric_limits<uint64_t>::max() - addend) {
        return std::numeric_limits<uint64_t>::max();
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool AddUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool IsWellFormedSparsePhysicsResultStatus(uint32_t status) {
    if ((status & Simulation::SPARSE_PHYSICS_PACKET_STATUS_CONSUMED) == 0u) {
        return false;
    }
    return (status & ~kSparsePhysicsResultKnownStatusMask) == 0u;
}

bool AppendAlignedUploadRange(
    uint64_t currentEnd,
    uint64_t bytes,
    uint64_t alignment,
    uint64_t* outOffset,
    uint64_t* outEnd)
{
    if (!outOffset || !outEnd) {
        return false;
    }
    if (bytes == 0) {
        *outOffset = currentEnd;
        *outEnd = currentEnd;
        return true;
    }
    const uint64_t alignedOffset = AlignUp(currentEnd, alignment);
    uint64_t endOffset = 0;
    if (!AddUint64(alignedOffset, bytes, &endOffset)) {
        return false;
    }
    *outOffset = alignedOffset;
    *outEnd = endOffset;
    return true;
}

uint32_t BuildEditDeltaRangeTableCapacity(
    size_t inputDeltaCount,
    uint32_t maxEditDeltaRanges,
    uint32_t configuredCapacity)
{
    const uint64_t estimatedRangeCount =
        std::min<uint64_t>(
            static_cast<uint64_t>(inputDeltaCount),
            static_cast<uint64_t>(maxEditDeltaRanges));
    const uint64_t targetRangeTableCapacity =
        std::max<uint64_t>(16ull, estimatedRangeCount * 4ull);
    const uint64_t clampedTarget =
        std::min<uint64_t>(targetRangeTableCapacity, configuredCapacity);

    uint32_t dynamicRangeTableCapacity = 16u;
    while (dynamicRangeTableCapacity < clampedTarget &&
           dynamicRangeTableCapacity < configuredCapacity) {
        if (dynamicRangeTableCapacity > std::numeric_limits<uint32_t>::max() / 2u) {
            dynamicRangeTableCapacity = configuredCapacity;
            break;
        }
        dynamicRangeTableCapacity <<= 1u;
    }

    return std::min(dynamicRangeTableCapacity, configuredCapacity);
}

uint32_t SparsePhysicsPacketChecksum(
    const Simulation::SparsePhysicsWorkPacket& packet,
    uint32_t frameIndex,
    uint32_t editDeltaCount,
    uint32_t editDeltaRangeCount,
    uint32_t editDeltaRangeTableCapacity,
    uint32_t pageTableCapacity)
{
    return
        static_cast<uint32_t>(packet.coord.x) ^
        (static_cast<uint32_t>(packet.coord.y) * 1664525u) ^
        (static_cast<uint32_t>(packet.coord.z) * 1013904223u) ^
        packet.packedRegionMin ^
        packet.packedRegionMax ^
        packet.materialMask ^
        packet.priority ^
        packet.generation ^
        packet.expectedPageIndex ^
        packet.expectedPageGeneration ^
        editDeltaCount ^
        editDeltaRangeCount ^
        editDeltaRangeTableCapacity ^
        frameIndex ^
        pageTableCapacity;
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
    if (!ValidateSparseVoxelGpuConfigForStats(config)) {
        return Error("SparseVoxelGpuResources::Initialize - config is outside sparse GPU runtime/stat limits");
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

    // Phase 1 GPU mid-voxel generation: the sample pool can be written by the
    // CS_GenerateMidVoxelBricks compute shader (UAV) in addition to the CPU
    // upload path (COPY_DEST). The MidVoxelGpuGenerator binds it as a root UAV by
    // GPU virtual address, so no UAV descriptor is strictly required, but we add
    // the UnorderedAccess usage so the resource is created UAV-capable.
    result = m_midVoxelClipmapSamples.Initialize(
        device,
        m_stats.midVoxelClipmapSampleBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
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
    result = m_midVoxelClipmapSamples.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse mid voxel clipmap sample UAV: {}", result.error());
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

    result = m_brushFeedback.Initialize(
        device,
        m_stats.brushFeedbackBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t) * 4u,
        "SparseBrushFeedback");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse brush feedback buffer: {}", result.error());
    }
    result = m_brushFeedback.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse brush feedback UAV: {}", result.error());
    }

    result = m_physicsWorkPackets.Initialize(
        device,
        m_stats.physicsWorkPacketBytes,
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparsePhysicsWorkPacket),
        "SparsePhysicsWorkPackets");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics packet buffer: {}", result.error());
    }
    result = m_physicsWorkPackets.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics packet SRV: {}", result.error());
    }

    result = m_editDeltas.Initialize(
        device,
        m_stats.editDeltaBytes,
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseEditDelta),
        "SparseEditDeltas");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta buffer: {}", result.error());
    }
    result = m_editDeltas.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta SRV: {}", result.error());
    }

    result = m_editDeltaRanges.Initialize(
        device,
        m_stats.editDeltaRangeBytes,
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseEditDeltaRange),
        "SparseEditDeltaRanges");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta range buffer: {}", result.error());
    }
    result = m_editDeltaRanges.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta range SRV: {}", result.error());
    }

    result = m_editDeltaRangeTable.Initialize(
        device,
        m_stats.editDeltaRangeTableBytes,
        BufferUsage::StructuredBuffer,
        sizeof(uint32_t),
        "SparseEditDeltaRangeTable");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta range table buffer: {}", result.error());
    }
    result = m_editDeltaRangeTable.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse edit delta range table SRV: {}", result.error());
    }

    result = m_physicsPacketResults.Initialize(
        device,
        m_stats.physicsPacketResultBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(Simulation::SparsePhysicsPacketResult),
        "SparsePhysicsPacketResults");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics packet result buffer: {}", result.error());
    }
    result = m_physicsPacketResults.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics packet result UAV: {}", result.error());
    }

    result = m_physicsDiagnostics.Initialize(
        device,
        m_stats.physicsDiagnosticBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "SparsePhysicsDiagnostics");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics diagnostics buffer: {}", result.error());
    }
    result = m_physicsDiagnostics.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse physics diagnostics UAV: {}", result.error());
    }

    result = m_renderOwnership.Initialize(
        device,
        m_stats.renderOwnershipBytes,
        BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "SparseRenderOwnership");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse render ownership buffer: {}", result.error());
    }
    result = m_renderOwnership.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse render ownership UAV: {}", result.error());
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
    for (uint32_t i = 0; i < kMaxUploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparseBrushFeedbackReadback_%u", i);
        result = m_brushFeedbackReadback[i].Initialize(
            device,
            m_stats.brushFeedbackBytes,
            BufferUsage::Readback,
            sizeof(uint32_t) * 4u,
            name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse brush feedback readback slot {}: {}", i, result.error());
        }
    }
    for (uint32_t i = 0; i < kMaxUploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparsePhysicsDiagnosticsReadback_%u", i);
        result = m_physicsDiagnosticsReadback[i].Initialize(
            device,
            m_stats.physicsDiagnosticBytes,
            BufferUsage::Readback,
            sizeof(uint32_t),
            name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse physics diagnostics readback slot {}: {}", i, result.error());
        }
    }
    for (uint32_t i = 0; i < kMaxUploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparsePhysicsPacketResultsReadback_%u", i);
        result = m_physicsPacketResultsReadback[i].Initialize(
            device,
            m_stats.physicsPacketResultBytes,
            BufferUsage::Readback,
            sizeof(Simulation::SparsePhysicsPacketResult),
            name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse physics packet result readback slot {}: {}", i, result.error());
        }
    }
    for (uint32_t i = 0; i < kMaxUploadRingSlots; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "SparseRenderOwnershipReadback_%u", i);
        result = m_renderOwnershipReadback[i].Initialize(
            device,
            m_stats.renderOwnershipBytes,
            BufferUsage::Readback,
            sizeof(uint32_t),
            name);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse render ownership readback slot {}: {}", i, result.error());
        }
    }

    m_stats.initialized = true;
    spdlog::info(
        "Sparse GPU resources initialized: pages={} pageTable={} brickPool={:.1f} MB occupancy={:.2f} MB midHeight={:.2f} MB midVoxel={:.2f} MB physicsPackets={:.2f} MB editDeltas={:.2f} MB editRanges={:.2f} MB editRangeTable={:.2f} MB physicsResults={:.2f} MB physicsDiag={:.3f} MB ownership={:.3f} MB missFeedback={:.2f} MB brushFeedback={:.2f} MB uploadRing={:.1f} MB total={:.1f} MB",
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
        static_cast<double>(m_stats.physicsWorkPacketBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.editDeltaBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.editDeltaRangeBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.editDeltaRangeTableBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.physicsPacketResultBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.physicsDiagnosticBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.renderOwnershipBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.missFeedbackBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.brushFeedbackBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.uploadRingBytes) / (1024.0 * 1024.0),
        static_cast<double>(m_stats.totalGpuBytes) / (1024.0 * 1024.0));

    return {};
}

void SparseVoxelGpuResources::Shutdown() {
    for (auto& upload : m_uploadRing) {
        upload.Shutdown();
    }
    for (auto& readback : m_physicsDiagnosticsReadback) {
        readback.Shutdown();
    }
    for (auto& readback : m_physicsPacketResultsReadback) {
        readback.Shutdown();
    }
    for (auto& readback : m_missFeedbackReadback) {
        readback.Shutdown();
    }
    for (auto& readback : m_brushFeedbackReadback) {
        readback.Shutdown();
    }
    for (auto& readback : m_renderOwnershipReadback) {
        readback.Shutdown();
    }
    m_renderOwnership.Shutdown();
    m_brushFeedback.Shutdown();
    m_missFeedback.Shutdown();
    m_physicsDiagnostics.Shutdown();
    m_physicsPacketResults.Shutdown();
    m_editDeltaRangeTable.Shutdown();
    m_editDeltaRanges.Shutdown();
    m_editDeltas.Shutdown();
    m_physicsWorkPackets.Shutdown();
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
    m_lastLoggedPhysicsDiagnosticFrame = UINT32_MAX;
    m_lastLoggedPhysicsDiagnosticChecksum = 0;
    m_lastLoggedPhysicsResultGeneration = UINT32_MAX;
    m_lastLoggedPhysicsResultChecksum = 0;
    m_lastRetiredPhysicsProposals.clear();
    m_physicsPacketResultsQueuedFrames.fill(UINT32_MAX);
    m_physicsPacketResultCounts.fill(0u);
    for (auto& expectedChecksums : m_physicsPacketExpectedChecksums) {
        expectedChecksums.clear();
    }
    m_pendingPhysicsPacketResultPackets.clear();
    m_physicsDiagnosticsQueuedFrames.fill(UINT32_MAX);
    m_missFeedbackQueuedFrames.fill(UINT32_MAX);
    m_brushFeedbackQueuedFrames.fill(UINT32_MAX);
    m_renderOwnershipQueuedFrames.fill(UINT32_MAX);
}

void SparseVoxelGpuResources::BeginFrame(uint32_t frameIndex) {
    if (!m_stats.initialized || m_config.uploadRingSlots == 0) {
        return;
    }

    m_activeUploadSlot = frameIndex % m_config.uploadRingSlots;
    m_uploadWriteOffset = 0;
    m_stats.stagedBricksLastFrame = 0;
    m_stats.stagedPartialBrickUploadsLastFrame = 0;
    m_stats.stagedPartialCopyRangesLastFrame = 0;
    m_stats.stagedPartialVoxelBytesLastFrame = 0;
    m_stats.stagedPageEntriesLastFrame = 0;
    m_stats.stagedBytesLastFrame = 0;
    m_stats.uploadRingOverflowLastFrame = false;
    m_stats.stagedMidClipmapTilesLastFrame = 0;
    m_stats.stagedMidVoxelClipmapBricksLastFrame = 0;
    m_stats.stagedMidClipmapBytesLastFrame = 0;
    m_stats.stagedPhysicsPacketsLastFrame = 0;
    m_stats.stagedPhysicsPacketBytesLastFrame = 0;
    m_stats.physicsPacketUploadOverflowLastFrame = false;
    m_stats.stagedEditDeltasLastFrame = 0;
    m_stats.stagedEditDeltaRangesLastFrame = 0;
    m_stats.stagedEditDeltaRangeTableEntriesLastFrame = 0;
    m_stats.stagedEditDeltaBytesLastFrame = 0;
    m_stats.editDeltaUploadOverflowLastFrame = false;
}

void SparseVoxelGpuResources::PrepareBrushFeedbackWrite(ID3D12GraphicsCommandList* commandList) {
    if (!m_stats.initialized || !commandList || !m_brushFeedback.GetResource() ||
        !m_brushFeedback.GetStagingUAV().IsValid() ||
        !m_brushFeedback.GetShaderVisibleUAV().IsValid()) {
        return;
    }

    m_brushFeedback.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t clearValues[4] = { 0u, 0u, 0u, 0u };
    commandList->ClearUnorderedAccessViewUint(
        m_brushFeedback.GetShaderVisibleUAV().gpu,
        m_brushFeedback.GetStagingUAV().cpu,
        m_brushFeedback.GetResource(),
        clearValues,
        0,
        nullptr);
}

uint64_t SparseVoxelGpuResources::ActiveUploadBytesCapacity() const {
    if (!m_stats.initialized ||
        m_activeUploadSlot >= m_config.uploadRingSlots ||
        m_activeUploadSlot >= m_uploadRing.size()) {
        return 0;
    }
    return m_uploadRing[m_activeUploadSlot].GetSize();
}

namespace {

bool CanReserveUploadRange(uint64_t currentOffset, uint64_t capacity, uint64_t bytes) {
    constexpr uint64_t kUploadAlignment = 256u;
    if (capacity == 0 || bytes == 0) {
        return false;
    }
    uint64_t uploadOffset = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(currentOffset, bytes, kUploadAlignment, &uploadOffset, &endOffset)) {
        return false;
    }
    return endOffset <= capacity;
}

struct MidClipmapUploadPlan {
    bool valid = false;
    bool uploadHeightLayer = false;
    bool uploadVoxelLayer = false;
    struct SampleRange {
        uint32_t startSlot = 0;
        uint32_t slotCount = 0;
        uint64_t sourceUintOffset = 0;
        uint64_t bytes = 0;
    };
    std::vector<SampleRange> heightSampleRanges;
    std::vector<SampleRange> voxelSampleRanges;
    uint64_t metadataBytes = 0;
    uint64_t lookupBytes = 0;
    uint64_t sampleBytes = 0;
    uint64_t voxelMetadataBytes = 0;
    uint64_t voxelLookupBytes = 0;
    uint64_t voxelSampleBytes = 0;
};

void BuildMidClipmapSampleRangePlan(
    const std::vector<Simulation::SparseClipmapSampleRange>& snapshotRanges,
    uint32_t fallbackStartSlot,
    uint32_t fallbackSlotCount,
    uint32_t maxSlots,
    uint64_t sampleStrideUints,
    uint64_t availableSampleUints,
    std::vector<MidClipmapUploadPlan::SampleRange>& outRanges,
    uint64_t& outBytes)
{
    outRanges.clear();
    outBytes = 0;
    if (maxSlots == 0 || sampleStrideUints == 0 || availableSampleUints == 0) {
        return;
    }

    uint64_t payloadUintOffset = 0;
    auto appendRange = [&](uint32_t startSlot, uint32_t slotCount) {
        if (slotCount == 0 || startSlot >= maxSlots) {
            return;
        }
        const uint32_t clampedSlotCount = std::min(slotCount, maxSlots - startSlot);
        const uint64_t rangeUints =
            static_cast<uint64_t>(clampedSlotCount) * sampleStrideUints;
        if (payloadUintOffset + rangeUints > availableSampleUints) {
            return;
        }
        MidClipmapUploadPlan::SampleRange range;
        range.startSlot = startSlot;
        range.slotCount = clampedSlotCount;
        range.sourceUintOffset = payloadUintOffset;
        range.bytes = rangeUints * sizeof(uint32_t);
        outBytes += range.bytes;
        outRanges.push_back(range);
        payloadUintOffset += rangeUints;
    };

    if (!snapshotRanges.empty()) {
        for (const Simulation::SparseClipmapSampleRange& range : snapshotRanges) {
            appendRange(range.startSlot, range.slotCount);
        }
        return;
    }
    appendRange(fallbackStartSlot, fallbackSlotCount);
}

struct SparseBrickVoxelUploadPlan {
    bool valid = false;
    uint64_t voxelBytes = 0;
    uint32_t copyRangeCount = 0;
};

SparseBrickVoxelUploadPlan BuildSparseBrickVoxelUploadPlan(
    const Simulation::SparseBrickUploadPacket& packet)
{
    SparseBrickVoxelUploadPlan plan;
    if (packet.pageIndex == Simulation::INVALID_BRICK_PAGE || packet.generation == 0) {
        return plan;
    }

    if (!packet.partialVoxelUpload) {
        plan.valid = true;
        plan.copyRangeCount = 1;
        plan.voxelBytes =
            static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
            sizeof(uint32_t);
        return plan;
    }

    const uint8_t minX = std::min(packet.dirtyMinX, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    const uint8_t minY = std::min(packet.dirtyMinY, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    const uint8_t minZ = std::min(packet.dirtyMinZ, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    const uint8_t maxX = std::min(packet.dirtyMaxX, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    const uint8_t maxY = std::min(packet.dirtyMaxY, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    const uint8_t maxZ = std::min(packet.dirtyMaxZ, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
    if (maxX < minX || maxY < minY || maxZ < minZ) {
        return plan;
    }

    const uint32_t rows =
        static_cast<uint32_t>(maxY - minY + 1u) *
        static_cast<uint32_t>(maxZ - minZ + 1u);
    const uint32_t voxelsPerRow = static_cast<uint32_t>(maxX - minX + 1u);
    plan.copyRangeCount = rows;
    plan.voxelBytes =
        static_cast<uint64_t>(rows) *
        static_cast<uint64_t>(voxelsPerRow) *
        sizeof(uint32_t);
    plan.valid = rows != 0 && voxelsPerRow != 0 && plan.voxelBytes != 0;
    return plan;
}

MidClipmapUploadPlan BuildMidClipmapUploadPlan(
    const Simulation::SparseClipmapGpuSnapshot& snapshot,
    bool uploadHeightLayer,
    bool uploadVoxelLayer)
{
    MidClipmapUploadPlan plan;
    plan.uploadHeightLayer =
        uploadHeightLayer &&
        snapshot.tileCount > 0 &&
        !snapshot.metadata.empty() &&
        !snapshot.lookup.empty() &&
        !snapshot.samples.empty();
    plan.uploadVoxelLayer =
        uploadVoxelLayer &&
        snapshot.voxelBrickCount > 0 &&
        !snapshot.voxelMetadata.empty() &&
        !snapshot.voxelLookup.empty() &&
        !snapshot.voxelSamples.empty();
    if (!plan.uploadHeightLayer && !plan.uploadVoxelLayer) {
        return plan;
    }

    const uint64_t metadataUints = plan.uploadHeightLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.metadata.size()),
            static_cast<uint64_t>(snapshot.tileCount + 1u) * 4u)
        : 0u;
    const uint64_t lookupUints = plan.uploadHeightLayer
        ? static_cast<uint64_t>(snapshot.lookup.size())
        : 0u;
    uint64_t sampleBytes = 0;
    if (plan.uploadHeightLayer) {
        const uint64_t heightSampleStride =
            static_cast<uint64_t>(snapshot.tileSampleSide) *
            static_cast<uint64_t>(snapshot.tileSampleSide);
        uint32_t fallbackStartSlot = 0;
        uint32_t fallbackSlotCount = snapshot.tileCount;
        if (snapshot.heightDirtySlotCount > 0) {
            fallbackStartSlot = std::min(snapshot.heightDirtyStartSlot, snapshot.tileCount);
            fallbackSlotCount = fallbackStartSlot < snapshot.tileCount
                ? std::min(snapshot.heightDirtySlotCount, snapshot.tileCount - fallbackStartSlot)
                : 0u;
        }
        BuildMidClipmapSampleRangePlan(
            snapshot.heightSampleRanges,
            fallbackStartSlot,
            fallbackSlotCount,
            snapshot.tileCount,
            heightSampleStride,
            static_cast<uint64_t>(snapshot.samples.size()),
            plan.heightSampleRanges,
            sampleBytes);
    }
    const uint64_t voxelMetadataUints = plan.uploadVoxelLayer
        ? std::min<uint64_t>(
            static_cast<uint64_t>(snapshot.voxelMetadata.size()),
            static_cast<uint64_t>(snapshot.voxelBrickCount + 1u) * 4u)
        : 0u;
    const uint64_t voxelLookupUints = plan.uploadVoxelLayer
        ? static_cast<uint64_t>(snapshot.voxelLookup.size())
        : 0u;
    uint64_t voxelSampleBytes = 0;
    if (plan.uploadVoxelLayer) {
        uint32_t fallbackStartSlot = 0;
        uint32_t fallbackSlotCount = snapshot.voxelBrickCount;
        if (snapshot.voxelDirtySlotCount > 0) {
            fallbackStartSlot = std::min(snapshot.voxelDirtyStartSlot, snapshot.voxelBrickCount);
            fallbackSlotCount = fallbackStartSlot < snapshot.voxelBrickCount
                ? std::min(snapshot.voxelDirtySlotCount, snapshot.voxelBrickCount - fallbackStartSlot)
                : 0u;
        }
        BuildMidClipmapSampleRangePlan(
            snapshot.voxelSampleRanges,
            fallbackStartSlot,
            fallbackSlotCount,
            snapshot.voxelBrickCount,
            static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT),
            static_cast<uint64_t>(snapshot.voxelSamples.size()),
            plan.voxelSampleRanges,
            voxelSampleBytes);
    }

    plan.metadataBytes = metadataUints * sizeof(uint32_t);
    plan.lookupBytes = lookupUints * sizeof(uint32_t);
    plan.sampleBytes = sampleBytes;
    plan.voxelMetadataBytes = voxelMetadataUints * sizeof(uint32_t);
    plan.voxelLookupBytes = voxelLookupUints * sizeof(uint32_t);
    plan.voxelSampleBytes = voxelSampleBytes;
    plan.valid = true;
    return plan;
}

} // namespace

bool SparseVoxelGpuResources::CanStageBrickUpload() const {
    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t capacity = ActiveUploadBytesCapacity();
    if (capacity == 0) {
        return false;
    }

    const uint64_t voxelBytes =
        static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) * sizeof(uint32_t);
    const uint64_t occupancyBytes = sizeof(uint32_t) * 2u;
    const uint64_t generationBytes = sizeof(uint32_t);
    uint64_t voxelOffset = 0;
    uint64_t occupancyOffset = 0;
    uint64_t generationOffset = 0;
    uint64_t afterVoxels = 0;
    uint64_t afterOccupancy = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, voxelBytes, kUploadAlignment, &voxelOffset, &afterVoxels) ||
        !AppendAlignedUploadRange(afterVoxels, occupancyBytes, kUploadAlignment, &occupancyOffset, &afterOccupancy) ||
        !AppendAlignedUploadRange(afterOccupancy, generationBytes, kUploadAlignment, &generationOffset, &endOffset)) {
        return false;
    }
    return endOffset <= capacity;
}

bool SparseVoxelGpuResources::CanStageBrickUpload(
    const Simulation::SparseBrickUploadPacket& packet) const
{
    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t capacity = ActiveUploadBytesCapacity();
    if (capacity == 0) {
        return false;
    }

    const SparseBrickVoxelUploadPlan plan = BuildSparseBrickVoxelUploadPlan(packet);
    if (!plan.valid) {
        return false;
    }

    const uint64_t occupancyBytes = sizeof(uint32_t) * 2u;
    const uint64_t generationBytes = sizeof(uint32_t);
    uint64_t voxelOffset = 0;
    uint64_t occupancyOffset = 0;
    uint64_t generationOffset = 0;
    uint64_t afterVoxels = 0;
    uint64_t afterOccupancy = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, plan.voxelBytes, kUploadAlignment, &voxelOffset, &afterVoxels) ||
        !AppendAlignedUploadRange(afterVoxels, occupancyBytes, kUploadAlignment, &occupancyOffset, &afterOccupancy) ||
        !AppendAlignedUploadRange(afterOccupancy, generationBytes, kUploadAlignment, &generationOffset, &endOffset)) {
        return false;
    }
    return endOffset <= capacity;
}

bool SparseVoxelGpuResources::CanStagePageTableEntry() const {
    return CanReserveUploadRange(
        m_uploadWriteOffset,
        ActiveUploadBytesCapacity(),
        sizeof(Simulation::BrickPageEntry));
}

bool SparseVoxelGpuResources::CanStagePageTableReset() const {
    return CanReserveUploadRange(
        m_uploadWriteOffset,
        ActiveUploadBytesCapacity(),
        m_stats.pageTableBytes);
}

bool SparseVoxelGpuResources::CanStageMidClipmapSnapshot(
    const Simulation::SparseClipmapGpuSnapshot& snapshot,
    bool uploadHeightLayer,
    bool uploadVoxelLayer) const
{
    if (!m_stats.initialized || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }
    const MidClipmapUploadPlan plan =
        BuildMidClipmapUploadPlan(snapshot, uploadHeightLayer, uploadVoxelLayer);
    if (!plan.valid) {
        return false;
    }
    if (plan.metadataBytes > m_stats.midClipmapMetadataBytes ||
        plan.lookupBytes > m_stats.midClipmapLookupBytes ||
        plan.sampleBytes > m_stats.midClipmapSampleBytes ||
        plan.voxelMetadataBytes > m_stats.midVoxelClipmapMetadataBytes ||
        plan.voxelLookupBytes > m_stats.midVoxelClipmapLookupBytes ||
        plan.voxelSampleBytes > m_stats.midVoxelClipmapSampleBytes) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    uint64_t metadataOffset = 0;
    uint64_t lookupOffset = 0;
    uint64_t samplesOffset = 0;
    uint64_t voxelMetadataOffset = 0;
    uint64_t voxelLookupOffset = 0;
    uint64_t voxelSamplesOffset = 0;
    uint64_t afterMetadata = 0;
    uint64_t afterLookup = 0;
    uint64_t afterSamples = 0;
    uint64_t afterVoxelMetadata = 0;
    uint64_t afterVoxelLookup = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, plan.metadataBytes, kUploadAlignment, &metadataOffset, &afterMetadata) ||
        !AppendAlignedUploadRange(afterMetadata, plan.lookupBytes, kUploadAlignment, &lookupOffset, &afterLookup) ||
        !AppendAlignedUploadRange(afterLookup, plan.sampleBytes, kUploadAlignment, &samplesOffset, &afterSamples) ||
        !AppendAlignedUploadRange(afterSamples, plan.voxelMetadataBytes, kUploadAlignment, &voxelMetadataOffset, &afterVoxelMetadata) ||
        !AppendAlignedUploadRange(afterVoxelMetadata, plan.voxelLookupBytes, kUploadAlignment, &voxelLookupOffset, &afterVoxelLookup) ||
        !AppendAlignedUploadRange(afterVoxelLookup, plan.voxelSampleBytes, kUploadAlignment, &voxelSamplesOffset, &endOffset)) {
        return false;
    }
    return endOffset <= ActiveUploadBytesCapacity();
}

uint64_t SparseVoxelGpuResources::EstimateMidClipmapSnapshotUploadBytes(
    const Simulation::SparseClipmapGpuSnapshot& snapshot,
    bool uploadHeightLayer,
    bool uploadVoxelLayer)
{
    const MidClipmapUploadPlan plan =
        BuildMidClipmapUploadPlan(snapshot, uploadHeightLayer, uploadVoxelLayer);
    if (!plan.valid) {
        return 0;
    }
    return plan.metadataBytes +
        plan.lookupBytes +
        plan.sampleBytes +
        plan.voxelMetadataBytes +
        plan.voxelLookupBytes +
        plan.voxelSampleBytes;
}

bool SparseVoxelGpuResources::CanStagePhysicsWorkPackets(
    const std::vector<Simulation::SparsePhysicsWorkPacket>& packets) const
{
    if (!m_stats.initialized || packets.empty() || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }
    const uint32_t packetCount = std::min<uint32_t>(
        static_cast<uint32_t>(packets.size()),
        m_config.maxPhysicsWorkPackets);
    const uint64_t bytes =
        static_cast<uint64_t>(packetCount) *
        sizeof(Simulation::SparsePhysicsWorkPacket);
    if (bytes == 0 || bytes > m_stats.physicsWorkPacketBytes) {
        return false;
    }
    return CanReserveUploadRange(m_uploadWriteOffset, ActiveUploadBytesCapacity(), bytes);
}

bool SparseVoxelGpuResources::CanStageEditDeltas(
    const std::vector<Simulation::SparseEditDelta>& deltas) const
{
    if (!m_stats.initialized || deltas.empty() || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }
    const uint32_t dynamicRangeTableCapacity =
        BuildEditDeltaRangeTableCapacity(
            deltas.size(),
            m_config.maxEditDeltaRanges,
            m_config.editDeltaRangeTableCapacity);

    const Simulation::SparseEditDeltaBatch batch =
        Simulation::BuildSparseEditDeltaBatch(
            deltas,
            m_config.maxEditDeltas,
            m_config.maxEditDeltaRanges,
            dynamicRangeTableCapacity);
    const uint64_t bytes =
        static_cast<uint64_t>(batch.deltas.size()) *
        sizeof(Simulation::SparseEditDelta);
    const uint64_t rangeBytes =
        static_cast<uint64_t>(batch.ranges.size()) *
        sizeof(Simulation::SparseEditDeltaRange);
    const uint64_t rangeTableBytes =
        static_cast<uint64_t>(batch.rangeTable.size()) *
        sizeof(uint32_t);
    if (bytes == 0 ||
        rangeBytes == 0 ||
        rangeTableBytes == 0 ||
        bytes > m_stats.editDeltaBytes ||
        rangeBytes > m_stats.editDeltaRangeBytes ||
        rangeTableBytes > m_stats.editDeltaRangeTableBytes) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    uint64_t uploadOffset = 0;
    uint64_t rangeUploadOffset = 0;
    uint64_t rangeTableUploadOffset = 0;
    uint64_t afterDeltas = 0;
    uint64_t afterRanges = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &afterDeltas) ||
        !AppendAlignedUploadRange(afterDeltas, rangeBytes, kUploadAlignment, &rangeUploadOffset, &afterRanges) ||
        !AppendAlignedUploadRange(afterRanges, rangeTableBytes, kUploadAlignment, &rangeTableUploadOffset, &endOffset)) {
        return false;
    }
    return endOffset <= ActiveUploadBytesCapacity();
}

void SparseVoxelGpuResources::PreparePhysicsDiagnosticsWrite(ID3D12GraphicsCommandList* commandList) {
    if (!m_stats.initialized || !commandList || !m_physicsDiagnostics.GetResource() ||
        !m_physicsDiagnostics.GetStagingUAV().IsValid() ||
        !m_physicsDiagnostics.GetShaderVisibleUAV().IsValid()) {
        return;
    }

    m_physicsDiagnostics.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t clearValues[4] = { 0u, 0u, 0u, 0u };
    commandList->ClearUnorderedAccessViewUint(
        m_physicsDiagnostics.GetShaderVisibleUAV().gpu,
        m_physicsDiagnostics.GetStagingUAV().cpu,
        m_physicsDiagnostics.GetResource(),
        clearValues,
        0,
        nullptr);
}

void SparseVoxelGpuResources::PreparePhysicsPacketResultsWrite(ID3D12GraphicsCommandList* commandList) {
    if (!m_stats.initialized || !commandList || !m_physicsPacketResults.GetResource() ||
        !m_physicsPacketResults.GetStagingUAV().IsValid() ||
        !m_physicsPacketResults.GetShaderVisibleUAV().IsValid()) {
        return;
    }

    m_physicsPacketResults.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t clearValues[4] = { 0u, 0u, 0u, 0u };
    commandList->ClearUnorderedAccessViewUint(
        m_physicsPacketResults.GetShaderVisibleUAV().gpu,
        m_physicsPacketResults.GetStagingUAV().cpu,
        m_physicsPacketResults.GetResource(),
        clearValues,
        0,
        nullptr);
}

void SparseVoxelGpuResources::QueuePhysicsDiagnosticsReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized || !commandList || !m_physicsDiagnostics.GetResource()) {
        return;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_physicsDiagnostics.GetResource();
    commandList->ResourceBarrier(1, &uavBarrier);

    m_physicsDiagnostics.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_physicsDiagnosticsReadback.size());
    GPUBuffer& readback = m_physicsDiagnosticsReadback[slot];
    commandList->CopyResource(readback.GetResource(), m_physicsDiagnostics.GetResource());
    m_physicsDiagnosticsQueuedFrames[slot] = frameIndex;
}

bool SparseVoxelGpuResources::RetirePhysicsDiagnostics(uint32_t frameIndex) {
    m_stats.physicsGpuPacketsLastRetire = 0;
    m_stats.physicsGpuMaterialMaskLastRetire = 0;
    m_stats.physicsGpuChecksumLastRetire = 0;
    m_stats.physicsGpuFrameLastRetire = 0;
    m_stats.physicsGpuMaxPriorityLastRetire = 0;
    m_stats.physicsGpuGenerationXorLastRetire = 0;
    m_stats.physicsGpuStaleFrameDropsLastRetire = 0;
    if (!m_stats.initialized) {
        return false;
    }

    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_physicsDiagnosticsReadback.size());
    const uint32_t queuedFrame = m_physicsDiagnosticsQueuedFrames[slot];
    if (queuedFrame == UINT32_MAX || frameIndex < queuedFrame + static_cast<uint32_t>(m_physicsDiagnosticsReadback.size())) {
        return false;
    }
    GPUBuffer& readback = m_physicsDiagnosticsReadback[slot];
    const uint32_t* mapped = static_cast<const uint32_t*>(readback.Map());
    if (!mapped) {
        return false;
    }

    const uint32_t payloadFrame = mapped[3];
    m_physicsDiagnosticsQueuedFrames[slot] = UINT32_MAX;
    if (payloadFrame != queuedFrame) {
        m_stats.physicsGpuStaleFrameDropsLastRetire = 1;
        spdlog::warn(
            "Sparse physics diagnostics dropped stale readback payload: retireFrame={} queuedFrame={} payloadFrame={} packets={}",
            frameIndex,
            queuedFrame,
            payloadFrame,
            mapped[0]);
        return false;
    }

    m_stats.physicsGpuPacketsLastRetire = mapped[0];
    m_stats.physicsGpuMaterialMaskLastRetire = mapped[1];
    m_stats.physicsGpuChecksumLastRetire = mapped[2];
    m_stats.physicsGpuFrameLastRetire = payloadFrame;
    m_stats.physicsGpuMaxPriorityLastRetire = mapped[4];
    m_stats.physicsGpuGenerationXorLastRetire = mapped[5];
    if (m_stats.physicsGpuPacketsLastRetire > 0 &&
        (m_stats.physicsGpuFrameLastRetire != m_lastLoggedPhysicsDiagnosticFrame ||
         m_stats.physicsGpuChecksumLastRetire != m_lastLoggedPhysicsDiagnosticChecksum)) {
        m_lastLoggedPhysicsDiagnosticFrame = m_stats.physicsGpuFrameLastRetire;
        m_lastLoggedPhysicsDiagnosticChecksum = m_stats.physicsGpuChecksumLastRetire;
        spdlog::info(
            "PERF_SPARSE_PHYSICS_GPU_READBACK retireFrame={} packets={} mask=0x{:X} checksum={} shaderFrame={} maxPri={} genXor={}",
            frameIndex,
            m_stats.physicsGpuPacketsLastRetire,
            m_stats.physicsGpuMaterialMaskLastRetire,
            m_stats.physicsGpuChecksumLastRetire,
            m_stats.physicsGpuFrameLastRetire,
            m_stats.physicsGpuMaxPriorityLastRetire,
            m_stats.physicsGpuGenerationXorLastRetire);
    }
    return true;
}

void SparseVoxelGpuResources::QueuePhysicsPacketResultsReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized || !commandList || !m_physicsPacketResults.GetResource()) {
        return;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_physicsPacketResults.GetResource();
    commandList->ResourceBarrier(1, &uavBarrier);

    m_physicsPacketResults.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_physicsPacketResultsReadback.size());
    GPUBuffer& readback = m_physicsPacketResultsReadback[slot];
    commandList->CopyResource(readback.GetResource(), m_physicsPacketResults.GetResource());
    m_physicsPacketResultsQueuedFrames[slot] = frameIndex;
    m_physicsPacketResultCounts[slot] =
        std::min(m_stats.stagedPhysicsPacketsLastFrame, m_config.maxPhysicsWorkPackets);
    std::vector<uint32_t>& expectedChecksums = m_physicsPacketExpectedChecksums[slot];
    expectedChecksums.clear();
    expectedChecksums.reserve(m_physicsPacketResultCounts[slot]);
    const uint32_t packetChecksumCount =
        std::min<uint32_t>(
            m_physicsPacketResultCounts[slot],
            static_cast<uint32_t>(m_pendingPhysicsPacketResultPackets.size()));
    for (uint32_t i = 0; i < packetChecksumCount; ++i) {
        expectedChecksums.push_back(
            SparsePhysicsPacketChecksum(
                m_pendingPhysicsPacketResultPackets[i],
                frameIndex,
                m_stats.stagedEditDeltasLastFrame,
                m_stats.stagedEditDeltaRangesLastFrame,
                m_stats.stagedEditDeltaRangeTableEntriesLastFrame,
                m_stats.pageTableCapacity));
    }
    m_pendingPhysicsPacketResultPackets.clear();
}

bool SparseVoxelGpuResources::RetirePhysicsPacketResults(uint32_t frameIndex) {
    m_lastRetiredPhysicsProposals.clear();
    m_stats.physicsGpuResultCountLastRetire = 0;
    m_stats.physicsGpuResultChecksumLastRetire = 0;
    m_stats.physicsGpuResultFirstBrickX = 0;
    m_stats.physicsGpuResultFirstBrickY = 0;
    m_stats.physicsGpuResultFirstBrickZ = 0;
    m_stats.physicsGpuResultFirstGeneration = 0;
    m_stats.physicsGpuResultFirstStatus = 0;
    m_stats.physicsGpuProposalCountLastRetire = 0;
    m_stats.physicsGpuMissingBelowCountLastRetire = 0;
    m_stats.physicsGpuMalformedStatusDropsLastRetire = 0;
    m_stats.physicsGpuUnexpectedPacketDropsLastRetire = 0;
    m_stats.physicsGpuChecksumDropsLastRetire = 0;
    if (!m_stats.initialized) {
        return false;
    }

    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_physicsPacketResultsReadback.size());
    const uint32_t queuedFrame = m_physicsPacketResultsQueuedFrames[slot];
    if (queuedFrame == UINT32_MAX || frameIndex < queuedFrame + static_cast<uint32_t>(m_physicsPacketResultsReadback.size())) {
        return false;
    }
    GPUBuffer& readback = m_physicsPacketResultsReadback[slot];
    const auto* mapped = static_cast<const Simulation::SparsePhysicsPacketResult*>(readback.Map());
    if (!mapped) {
        return false;
    }
    const uint32_t queuedResultCount =
        std::min(m_physicsPacketResultCounts[slot], m_config.maxPhysicsWorkPackets);
    const std::vector<uint32_t>& expectedChecksums = m_physicsPacketExpectedChecksums[slot];
    m_physicsPacketResultsQueuedFrames[slot] = UINT32_MAX;
    m_physicsPacketResultCounts[slot] = 0u;

    const uint32_t resultCount = queuedResultCount;
    uint32_t checksum = 0;
    uint32_t validResults = 0;
    uint32_t proposals = 0;
    uint32_t missingBelow = 0;
    uint32_t malformedStatusDrops = 0;
    uint32_t unexpectedPacketDrops = 0;
    uint32_t checksumDrops = 0;
    uint32_t firstValidResultIndex = UINT32_MAX;
    uint32_t firstProposalResultIndex = UINT32_MAX;
    for (uint32_t i = 0; i < resultCount; ++i) {
        if (mapped[i].status == 0) {
            continue;
        }
        if (!IsWellFormedSparsePhysicsResultStatus(mapped[i].status)) {
            ++malformedStatusDrops;
            continue;
        }
        if (mapped[i].packetIndex != i || mapped[i].packetIndex >= expectedChecksums.size()) {
            ++unexpectedPacketDrops;
            continue;
        }
        if (mapped[i].checksum != expectedChecksums[mapped[i].packetIndex]) {
            ++checksumDrops;
            continue;
        }
        if (firstValidResultIndex == UINT32_MAX) {
            firstValidResultIndex = i;
        }
        ++validResults;
        if ((mapped[i].status & Simulation::SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL) != 0u) {
            if (firstProposalResultIndex == UINT32_MAX) {
                firstProposalResultIndex = i;
            }
            ++proposals;
            m_lastRetiredPhysicsProposals.push_back(mapped[i]);
        }
        if ((mapped[i].status & Simulation::SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW) != 0u) {
            ++missingBelow;
        }
        checksum += mapped[i].checksum ^ mapped[i].generation ^ mapped[i].packetIndex;
    }
    m_stats.physicsGpuResultCountLastRetire = validResults;
    m_stats.physicsGpuResultChecksumLastRetire = checksum;
    m_stats.physicsGpuProposalCountLastRetire = proposals;
    m_stats.physicsGpuMissingBelowCountLastRetire = missingBelow;
    m_stats.physicsGpuMalformedStatusDropsLastRetire = malformedStatusDrops;
    m_stats.physicsGpuUnexpectedPacketDropsLastRetire = unexpectedPacketDrops;
    m_stats.physicsGpuChecksumDropsLastRetire = checksumDrops;
    if (unexpectedPacketDrops > 0 || checksumDrops > 0) {
        spdlog::warn(
            "Sparse physics result dropped stale or mismatched readback rows: retireFrame={} queuedFrame={} unexpectedPacketDrops={} checksumDrops={} expectedChecksums={}",
            frameIndex,
            queuedFrame,
            unexpectedPacketDrops,
            checksumDrops,
            expectedChecksums.size());
    }
    if (validResults > 0 && firstValidResultIndex != UINT32_MAX) {
        const uint32_t diagnosticResultIndex =
            firstProposalResultIndex != UINT32_MAX ? firstProposalResultIndex : firstValidResultIndex;
        const auto& firstResult = mapped[diagnosticResultIndex];
        m_stats.physicsGpuResultFirstBrickX = firstResult.coord.x;
        m_stats.physicsGpuResultFirstBrickY = firstResult.coord.y;
        m_stats.physicsGpuResultFirstBrickZ = firstResult.coord.z;
        m_stats.physicsGpuResultFirstGeneration = firstResult.generation;
        m_stats.physicsGpuResultFirstStatus = firstResult.status;
    }
    if (validResults > 0 &&
        (m_stats.physicsGpuResultFirstGeneration != m_lastLoggedPhysicsResultGeneration ||
         m_stats.physicsGpuResultChecksumLastRetire != m_lastLoggedPhysicsResultChecksum)) {
        m_lastLoggedPhysicsResultGeneration = m_stats.physicsGpuResultFirstGeneration;
        m_lastLoggedPhysicsResultChecksum = m_stats.physicsGpuResultChecksumLastRetire;
        spdlog::info(
            "PERF_SPARSE_PHYSICS_GPU_RESULT retireFrame={} results={} proposals={} missingBelow={} checksum={} firstBrick={},{},{} firstGen={} firstStatus={}",
            frameIndex,
            m_stats.physicsGpuResultCountLastRetire,
            m_stats.physicsGpuProposalCountLastRetire,
            m_stats.physicsGpuMissingBelowCountLastRetire,
            m_stats.physicsGpuResultChecksumLastRetire,
            m_stats.physicsGpuResultFirstBrickX,
            m_stats.physicsGpuResultFirstBrickY,
            m_stats.physicsGpuResultFirstBrickZ,
            m_stats.physicsGpuResultFirstGeneration,
            m_stats.physicsGpuResultFirstStatus);
    }
    m_physicsPacketExpectedChecksums[slot].clear();
    return true;
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
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_missFeedbackReadback.size());
    GPUBuffer& readback = m_missFeedbackReadback[slot];
    commandList->CopyResource(readback.GetResource(), m_missFeedback.GetResource());
    m_missFeedbackQueuedFrames[slot] = frameIndex;
}

bool SparseVoxelGpuResources::RetireMissFeedback(
    uint32_t frameIndex,
    std::vector<Simulation::BrickCoord>& outMissingBricks)
{
    m_stats.missFeedbackRecordsLastRetire = 0;
    m_stats.missFeedbackFrameLastRetire = 0;
    m_stats.missFeedbackStaleFrameDropsLastRetire = 0;
    m_stats.missFeedbackOverflowLastRetire = false;
    if (!m_stats.initialized) {
        return false;
    }

    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_missFeedbackReadback.size());
    const uint32_t queuedFrame = m_missFeedbackQueuedFrames[slot];
    if (queuedFrame == UINT32_MAX ||
        frameIndex < queuedFrame + static_cast<uint32_t>(m_missFeedbackReadback.size())) {
        return false;
    }
    GPUBuffer& readback = m_missFeedbackReadback[slot];
    const uint32_t* mapped = static_cast<const uint32_t*>(readback.Map());
    if (!mapped) {
        return false;
    }
    m_missFeedbackQueuedFrames[slot] = UINT32_MAX;

    const uint32_t reported = mapped[0];
    const uint32_t reportedFrame = mapped[1];
    m_stats.missFeedbackFrameLastRetire = reportedFrame;
    if (reportedFrame != queuedFrame) {
        m_stats.missFeedbackStaleFrameDropsLastRetire = 1;
        spdlog::warn(
            "Sparse miss feedback dropped stale readback payload: retireFrame={} queuedFrame={} payloadFrame={} reported={}",
            frameIndex,
            queuedFrame,
            reportedFrame,
            reported);
        return false;
    }

    const uint32_t count = std::min(reported, m_config.missFeedbackMaxRecords);
    m_stats.missFeedbackOverflowLastRetire = reported > m_config.missFeedbackMaxRecords;
    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> uniqueThisRetire;
    uniqueThisRetire.reserve(count);
    outMissingBricks.reserve(outMissingBricks.size() + count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* record = mapped + (static_cast<size_t>(i + 1u) * 4u);
        Simulation::BrickCoord coord{
            static_cast<int32_t>(record[0]),
            static_cast<int32_t>(record[1]),
            static_cast<int32_t>(record[2])
        };
        if (uniqueThisRetire.insert(coord).second) {
            outMissingBricks.push_back(coord);
        }
    }
    m_stats.missFeedbackRecordsLastRetire = static_cast<uint32_t>(uniqueThisRetire.size());
    return true;
}

void SparseVoxelGpuResources::QueueBrushFeedbackReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized || !commandList || !m_brushFeedback.GetResource()) {
        return;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_brushFeedback.GetResource();
    commandList->ResourceBarrier(1, &uavBarrier);

    m_brushFeedback.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_brushFeedbackReadback.size());
    GPUBuffer& readback = m_brushFeedbackReadback[slot];
    commandList->CopyResource(readback.GetResource(), m_brushFeedback.GetResource());
    m_brushFeedbackQueuedFrames[slot] = frameIndex;
}

bool SparseVoxelGpuResources::RetireBrushFeedback(
    uint32_t frameIndex,
    std::vector<Simulation::SparseBrushFeedbackRecord>& outRecords)
{
    m_stats.brushFeedbackRecordsLastRetire = 0;
    m_stats.brushFeedbackFrameLastRetire = 0;
    m_stats.brushFeedbackQueuedFrameLastRetire = 0;
    m_stats.brushFeedbackMissingResidentLastRetire = 0;
    m_stats.brushFeedbackStaleFrameDropsLastRetire = 0;
    m_stats.brushFeedbackOverflowLastRetire = false;
    if (!m_stats.initialized) {
        return false;
    }

    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_brushFeedbackReadback.size());
    const uint32_t queuedFrame = m_brushFeedbackQueuedFrames[slot];
    if (queuedFrame == UINT32_MAX ||
        frameIndex < queuedFrame + static_cast<uint32_t>(m_brushFeedbackReadback.size())) {
        return false;
    }
    GPUBuffer& readback = m_brushFeedbackReadback[slot];
    const uint32_t* mapped = static_cast<const uint32_t*>(readback.Map());
    if (!mapped) {
        return false;
    }
    m_brushFeedbackQueuedFrames[slot] = UINT32_MAX;
    m_stats.brushFeedbackQueuedFrameLastRetire = queuedFrame;

    const uint32_t reported = mapped[0];
    const uint32_t count = std::min(reported, m_config.maxBrushFeedbackRecords);
    m_stats.brushFeedbackFrameLastRetire = mapped[1];
    if (m_stats.brushFeedbackFrameLastRetire != queuedFrame) {
        m_stats.brushFeedbackStaleFrameDropsLastRetire = 1;
        spdlog::warn(
            "Sparse brush feedback dropped stale readback payload: retireFrame={} queuedFrame={} payloadFrame={} reported={}",
            frameIndex,
            queuedFrame,
            m_stats.brushFeedbackFrameLastRetire,
            reported);
        return false;
    }
    m_stats.brushFeedbackOverflowLastRetire =
        Simulation::SparseBrushFeedbackPayloadOverflowed(
            reported,
            m_config.maxBrushFeedbackRecords,
            mapped[2]);
    m_stats.brushFeedbackMissingResidentLastRetire = mapped[3];
    outRecords.reserve(outRecords.size() + count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* record = mapped + (static_cast<size_t>(i + 1u) * 4u);
        outRecords.push_back({
            static_cast<int32_t>(record[0]),
            static_cast<int32_t>(record[1]),
            static_cast<int32_t>(record[2]),
            record[3]
        });
    }
    m_stats.brushFeedbackRecordsLastRetire = count;
    return true;
}

void SparseVoxelGpuResources::PrepareRenderOwnershipWrite(ID3D12GraphicsCommandList* commandList) {
    if (!m_stats.initialized || !commandList || !m_renderOwnership.GetResource() ||
        !m_renderOwnership.GetStagingUAV().IsValid() ||
        !m_renderOwnership.GetShaderVisibleUAV().IsValid()) {
        return;
    }

    m_renderOwnership.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t clearValues[4] = { 0u, 0u, 0u, 0u };
    commandList->ClearUnorderedAccessViewUint(
        m_renderOwnership.GetShaderVisibleUAV().gpu,
        m_renderOwnership.GetStagingUAV().cpu,
        m_renderOwnership.GetResource(),
        clearValues,
        0,
        nullptr);
}

void SparseVoxelGpuResources::QueueRenderOwnershipReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized || !commandList || !m_renderOwnership.GetResource()) {
        return;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_renderOwnership.GetResource();
    commandList->ResourceBarrier(1, &uavBarrier);

    m_renderOwnership.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_renderOwnershipReadback.size());
    GPUBuffer& readback = m_renderOwnershipReadback[slot];
    commandList->CopyResource(readback.GetResource(), m_renderOwnership.GetResource());
    m_renderOwnershipQueuedFrames[slot] = frameIndex;
}

bool SparseVoxelGpuResources::RetireRenderOwnership(uint32_t frameIndex) {
    m_stats.renderOwnerStaleFrameDropsLastRetire = 0;
    if (!m_stats.initialized) {
        return false;
    }

    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_renderOwnershipReadback.size());
    const uint32_t queuedFrame = m_renderOwnershipQueuedFrames[slot];
    if (queuedFrame == UINT32_MAX || frameIndex < queuedFrame + static_cast<uint32_t>(m_renderOwnershipReadback.size())) {
        return false;
    }

    GPUBuffer& readback = m_renderOwnershipReadback[slot];
    const uint32_t* mapped = static_cast<const uint32_t*>(readback.Map());
    if (!mapped) {
        return false;
    }

    const uint32_t payloadFrame = mapped[8];
    m_renderOwnershipQueuedFrames[slot] = UINT32_MAX;
    if (payloadFrame != queuedFrame) {
        m_stats.renderOwnerStaleFrameDropsLastRetire = 1;
        spdlog::warn(
            "Sparse render ownership dropped stale readback payload: retireFrame={} queuedFrame={} payloadFrame={} total={}",
            frameIndex,
            queuedFrame,
            payloadFrame,
            mapped[0]);
        return false;
    }

    m_stats.renderOwnerTotalPixelsLastRetire = mapped[0];
    m_stats.renderOwnerNearPixelsLastRetire = mapped[1];
    m_stats.renderOwnerMidVoxelPixelsLastRetire = mapped[2];
    m_stats.renderOwnerMidHeightPixelsLastRetire = mapped[3];
    m_stats.renderOwnerFarSvoPixelsLastRetire = mapped[4];
    m_stats.renderOwnerFarHeightPixelsLastRetire = mapped[5];
    m_stats.renderOwnerSkyPixelsLastRetire = mapped[6];
    m_stats.renderOwnerMissPixelsLastRetire = mapped[7];
    m_stats.renderOwnerSurfacePixelsLastRetire = mapped[9];
    m_stats.renderOwnerUnsafeNearMissPixelsLastRetire = mapped[10];
    m_stats.renderOwnerFarWaterPixelsLastRetire = mapped[11];
    m_stats.renderOwnerWaterContextPixelsLastRetire = mapped[12];
    m_stats.renderOwnerValleyAtmospherePixelsLastRetire = mapped[13];
    m_stats.renderOwnerLodParentHeldPixelsLastRetire = mapped[14];
    m_stats.renderOwnerUnsafeMissSampleCountLastRetire = mapped[15];
    m_stats.renderOwnerUnsafeMissSampleBrickX = static_cast<int32_t>(mapped[16]);
    m_stats.renderOwnerUnsafeMissSampleBrickY = static_cast<int32_t>(mapped[17]);
    m_stats.renderOwnerUnsafeMissSampleBrickZ = static_cast<int32_t>(mapped[18]);
    m_stats.renderOwnerUnsafeMissSampleDistanceLastRetire = mapped[19];
    m_stats.renderOwnerMidInteriorFallbackPixelsLastRetire = mapped[20];
    m_stats.renderOwnerFarSurfacePixelsLastRetire = mapped[21];
    m_stats.renderOwnerFarHeightContinuityPixelsLastRetire = mapped[22];
    m_stats.renderOwnerFarHeightMidMissingPixelsLastRetire = mapped[23];
    m_stats.renderOwnerFarHeightMidAirPixelsLastRetire = mapped[24];
    m_stats.renderOwnerFarHeightMidSolidPixelsLastRetire = mapped[25];
    m_stats.renderOwnerFarHeightFarPagePresentPixelsLastRetire = mapped[26];
    m_stats.renderOwnerFarHeightFarPageMissingPixelsLastRetire = mapped[27];
    m_stats.renderOwnerFarHeightFarPageOutOfGridPixelsLastRetire = mapped[28];
    m_stats.renderOwnerFarHeightMidSampleCountLastRetire = mapped[29];
    m_stats.renderOwnerUnsafeMissSampleStoredLastRetire = 0;
    for (uint32_t sampleSlot = 0;
         sampleSlot < kSparseRenderOwnershipUnsafeSampleCapacity &&
         m_stats.renderOwnerUnsafeMissSampleStoredLastRetire < kSparseRenderOwnershipUnsafeSampleCapacity;
         ++sampleSlot) {
        const uint32_t base = kSparseRenderOwnershipBaseWords + sampleSlot * 4u;
        const uint32_t distance = mapped[base + 3u];
        if (distance == 0u && mapped[base + 0u] == 0u && mapped[base + 1u] == 0u && mapped[base + 2u] == 0u) {
            continue;
        }
        const uint32_t outIndex = m_stats.renderOwnerUnsafeMissSampleStoredLastRetire++;
        m_stats.renderOwnerUnsafeMissSampleBricksLastRetire[outIndex] = {
            static_cast<int32_t>(mapped[base + 0u]),
            static_cast<int32_t>(mapped[base + 1u]),
            static_cast<int32_t>(mapped[base + 2u])
        };
        m_stats.renderOwnerUnsafeMissSampleDistancesLastRetire[outIndex] = distance;
    }
    const uint32_t farHeightMidSampleBase =
        kSparseRenderOwnershipBaseWords + kSparseRenderOwnershipUnsafeSampleCapacity * 4u;
    m_stats.renderOwnerFarHeightMidSampleStoredLastRetire = 0;
    for (uint32_t sampleSlot = 0;
         sampleSlot < kSparseRenderOwnershipFarHeightMidSampleCapacity &&
         m_stats.renderOwnerFarHeightMidSampleStoredLastRetire < kSparseRenderOwnershipFarHeightMidSampleCapacity;
         ++sampleSlot) {
        const uint32_t base = farHeightMidSampleBase + sampleSlot * 4u;
        if (mapped[base + 0u] == 0u &&
            mapped[base + 1u] == 0u &&
            mapped[base + 2u] == 0u &&
            mapped[base + 3u] == 0u) {
            continue;
        }
        const uint32_t outIndex = m_stats.renderOwnerFarHeightMidSampleStoredLastRetire++;
        m_stats.renderOwnerFarHeightMidSamplesLastRetire[outIndex] = {
            static_cast<int32_t>(mapped[base + 0u]),
            static_cast<int32_t>(mapped[base + 1u]),
            static_cast<int32_t>(mapped[base + 2u]),
            static_cast<int32_t>(mapped[base + 3u])
        };
    }
    for (uint32_t i = m_stats.renderOwnerUnsafeMissSampleStoredLastRetire;
         i < kSparseRenderOwnershipUnsafeSampleCapacity;
         ++i) {
        m_stats.renderOwnerUnsafeMissSampleBricksLastRetire[i] = {};
        m_stats.renderOwnerUnsafeMissSampleDistancesLastRetire[i] = 0u;
    }
    for (uint32_t i = m_stats.renderOwnerFarHeightMidSampleStoredLastRetire;
         i < kSparseRenderOwnershipFarHeightMidSampleCapacity;
         ++i) {
        m_stats.renderOwnerFarHeightMidSamplesLastRetire[i] = {};
    }
    m_stats.renderOwnerFrameLastRetire = payloadFrame;
    if (m_stats.renderOwnerTotalPixelsLastRetire > 0) {
        spdlog::info(
            "PERF_RENDER_OWNERSHIP retireFrame={} shaderFrame={} total={} near={} surfaceFragments={} farSurface={} midVoxel={} midVoxelInteriorFallback={} midHeight={} farSvo={} farHeight={} farWater={} waterContext={} valleyAtmosphere={} sky={} miss={} unsafeNearMiss={} lodParentHeld={} parentHeldUntilChildrenReady={} unsafeSample={}/{},{},{} dist={} farHeightReason=continuity/midMissing/midAir/midSolid/farPagePresent/farPageMissing/farPageOutGrid/midSamples/stored:{}/{}/{}/{}/{}/{}/{}/{}/{}",
            frameIndex,
            m_stats.renderOwnerFrameLastRetire,
            m_stats.renderOwnerTotalPixelsLastRetire,
            m_stats.renderOwnerNearPixelsLastRetire,
            m_stats.renderOwnerSurfacePixelsLastRetire,
            m_stats.renderOwnerFarSurfacePixelsLastRetire,
            m_stats.renderOwnerMidVoxelPixelsLastRetire,
            m_stats.renderOwnerMidInteriorFallbackPixelsLastRetire,
            m_stats.renderOwnerMidHeightPixelsLastRetire,
            m_stats.renderOwnerFarSvoPixelsLastRetire,
            m_stats.renderOwnerFarHeightPixelsLastRetire,
            m_stats.renderOwnerFarWaterPixelsLastRetire,
            m_stats.renderOwnerWaterContextPixelsLastRetire,
            m_stats.renderOwnerValleyAtmospherePixelsLastRetire,
            m_stats.renderOwnerSkyPixelsLastRetire,
            m_stats.renderOwnerMissPixelsLastRetire,
            m_stats.renderOwnerUnsafeNearMissPixelsLastRetire,
            m_stats.renderOwnerLodParentHeldPixelsLastRetire,
            m_stats.renderOwnerLodParentHeldPixelsLastRetire,
            m_stats.renderOwnerUnsafeMissSampleCountLastRetire,
            m_stats.renderOwnerUnsafeMissSampleBrickX,
            m_stats.renderOwnerUnsafeMissSampleBrickY,
            m_stats.renderOwnerUnsafeMissSampleBrickZ,
            m_stats.renderOwnerUnsafeMissSampleDistanceLastRetire,
            m_stats.renderOwnerFarHeightContinuityPixelsLastRetire,
            m_stats.renderOwnerFarHeightMidMissingPixelsLastRetire,
            m_stats.renderOwnerFarHeightMidAirPixelsLastRetire,
            m_stats.renderOwnerFarHeightMidSolidPixelsLastRetire,
            m_stats.renderOwnerFarHeightFarPagePresentPixelsLastRetire,
            m_stats.renderOwnerFarHeightFarPageMissingPixelsLastRetire,
            m_stats.renderOwnerFarHeightFarPageOutOfGridPixelsLastRetire,
            m_stats.renderOwnerFarHeightMidSampleCountLastRetire,
            m_stats.renderOwnerFarHeightMidSampleStoredLastRetire);
    }
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
    struct LocalCopyRange {
        uint16_t localIndex = 0;
        uint16_t voxelCount = 0;
    };
    std::vector<LocalCopyRange> localRanges;
    if (packet.partialVoxelUpload) {
        const uint8_t minX = std::min(packet.dirtyMinX, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        const uint8_t minY = std::min(packet.dirtyMinY, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        const uint8_t minZ = std::min(packet.dirtyMinZ, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        const uint8_t maxX = std::min(packet.dirtyMaxX, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        const uint8_t maxY = std::min(packet.dirtyMaxY, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        const uint8_t maxZ = std::min(packet.dirtyMaxZ, static_cast<uint8_t>(Simulation::SPARSE_BRICK_SIZE - 1));
        if (maxX < minX || maxY < minY || maxZ < minZ) {
            return false;
        }
        localRanges.reserve(
            static_cast<size_t>(maxY - minY + 1u) *
            static_cast<size_t>(maxZ - minZ + 1u));
        for (uint8_t z = minZ; z <= maxZ; ++z) {
            for (uint8_t y = minY; y <= maxY; ++y) {
                localRanges.push_back({
                    Simulation::LocalVoxelIndex({minX, y, z}),
                    static_cast<uint16_t>(maxX - minX + 1u)
                });
            }
        }
    } else {
        localRanges.push_back({0, Simulation::SPARSE_BRICK_VOXEL_COUNT});
    }

    uint64_t voxelBytes = 0;
    for (const LocalCopyRange& range : localRanges) {
        if (range.voxelCount == 0 ||
            static_cast<uint32_t>(range.localIndex) + range.voxelCount >
                Simulation::SPARSE_BRICK_VOXEL_COUNT) {
            return false;
        }
        voxelBytes += static_cast<uint64_t>(range.voxelCount) * sizeof(uint32_t);
    }
    if (voxelBytes == 0) {
        return false;
    }

    const uint64_t generationBytes = sizeof(uint32_t);
    const uint64_t occupancyBytes = sizeof(uint32_t) * 2u;
    uint64_t voxelOffset = 0;
    uint64_t occupancyOffset = 0;
    uint64_t generationOffset = 0;
    uint64_t afterVoxels = 0;
    uint64_t afterOccupancy = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, voxelBytes, kUploadAlignment, &voxelOffset, &afterVoxels) ||
        !AppendAlignedUploadRange(afterVoxels, occupancyBytes, kUploadAlignment, &occupancyOffset, &afterOccupancy) ||
        !AppendAlignedUploadRange(afterOccupancy, generationBytes, kUploadAlignment, &generationOffset, &endOffset)) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    std::vector<SparseBrickVoxelCopyRange> copyRanges;
    copyRanges.reserve(localRanges.size());
    uint64_t nextVoxelUploadOffset = voxelOffset;
    const uint64_t pageBaseOffset =
        static_cast<uint64_t>(packet.pageIndex) *
        static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
        sizeof(uint32_t);
    for (const LocalCopyRange& range : localRanges) {
        const uint64_t rangeBytes = static_cast<uint64_t>(range.voxelCount) * sizeof(uint32_t);
        std::memcpy(
            mapped + nextVoxelUploadOffset,
            packet.brick.voxels.data() + range.localIndex,
            static_cast<size_t>(rangeBytes));
        copyRanges.push_back({
            nextVoxelUploadOffset,
            pageBaseOffset + static_cast<uint64_t>(range.localIndex) * sizeof(uint32_t),
            rangeBytes
        });
        if (!AddUint64(nextVoxelUploadOffset, rangeBytes, &nextVoxelUploadOffset)) {
            m_stats.uploadRingOverflowLastFrame = true;
            return false;
        }
    }
    uint32_t occupancyWords[2] = {
        packet.brick.occupancyWord0,
        packet.brick.occupancyWord1
    };
    std::memcpy(mapped + occupancyOffset, occupancyWords, sizeof(occupancyWords));
    std::memcpy(mapped + generationOffset, &packet.generation, sizeof(packet.generation));

    m_uploadWriteOffset = endOffset;
    ++m_stats.stagedBricksLastFrame;
    if (packet.partialVoxelUpload) {
        ++m_stats.stagedPartialBrickUploadsLastFrame;
        m_stats.stagedPartialCopyRangesLastFrame += static_cast<uint32_t>(copyRanges.size());
        m_stats.stagedPartialVoxelBytesLastFrame += voxelBytes;
    }
    m_stats.stagedBytesLastFrame += endOffset - voxelOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->voxelUploadOffset = voxelOffset;
        outTicket->occupancyUploadOffset = occupancyOffset;
        outTicket->generationUploadOffset = generationOffset;
        outTicket->brickPoolOffset = copyRanges.empty() ? pageBaseOffset : copyRanges.front().brickPoolOffset;
        outTicket->occupancyBufferOffset =
            static_cast<uint64_t>(packet.pageIndex) * sizeof(uint32_t) * 2u;
        outTicket->pageGenerationBufferOffset =
            static_cast<uint64_t>(packet.pageIndex) * sizeof(uint32_t);
        outTicket->voxelBytes = voxelBytes;
        outTicket->occupancyBytes = occupancyBytes;
        outTicket->generationBytes = generationBytes;
        outTicket->voxelCopyRanges = std::move(copyRanges);
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
    uint64_t uploadOffset = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &endOffset) ||
        endOffset > upload.GetSize()) {
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
    // CPU-side sparse page-table removal leaves an open-addressing tombstone,
    // not an empty slot. The shader lookup also has to see a tombstone here;
    // writing INVALID would prematurely terminate the probe chain and make
    // later resident bricks in that chain disappear on the GPU.
    invalidEntry.pageIndex = Simulation::INVALID_BRICK_PAGE - 1u;
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
    uint64_t uploadOffset = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &endOffset) ||
        endOffset > upload.GetSize()) {
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
    uint64_t uploadOffset = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &endOffset) ||
        endOffset > upload.GetSize()) {
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

    const MidClipmapUploadPlan plan =
        BuildMidClipmapUploadPlan(snapshot, uploadHeightLayer, uploadVoxelLayer);
    if (!plan.valid) {
        return false;
    }
    uploadHeightLayer = plan.uploadHeightLayer;
    uploadVoxelLayer = plan.uploadVoxelLayer;

    if (plan.metadataBytes > m_stats.midClipmapMetadataBytes ||
        plan.lookupBytes > m_stats.midClipmapLookupBytes ||
        plan.sampleBytes > m_stats.midClipmapSampleBytes ||
        plan.voxelMetadataBytes > m_stats.midVoxelClipmapMetadataBytes ||
        plan.voxelLookupBytes > m_stats.midVoxelClipmapLookupBytes ||
        plan.voxelSampleBytes > m_stats.midVoxelClipmapSampleBytes) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    uint64_t metadataOffset = 0;
    uint64_t lookupOffset = 0;
    uint64_t samplesOffset = 0;
    uint64_t voxelMetadataOffset = 0;
    uint64_t voxelLookupOffset = 0;
    uint64_t voxelSamplesOffset = 0;
    uint64_t afterMetadata = 0;
    uint64_t afterLookup = 0;
    uint64_t afterSamples = 0;
    uint64_t afterVoxelMetadata = 0;
    uint64_t afterVoxelLookup = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, plan.metadataBytes, kUploadAlignment, &metadataOffset, &afterMetadata) ||
        !AppendAlignedUploadRange(afterMetadata, plan.lookupBytes, kUploadAlignment, &lookupOffset, &afterLookup) ||
        !AppendAlignedUploadRange(afterLookup, plan.sampleBytes, kUploadAlignment, &samplesOffset, &afterSamples) ||
        !AppendAlignedUploadRange(afterSamples, plan.voxelMetadataBytes, kUploadAlignment, &voxelMetadataOffset, &afterVoxelMetadata) ||
        !AppendAlignedUploadRange(afterVoxelMetadata, plan.voxelLookupBytes, kUploadAlignment, &voxelLookupOffset, &afterVoxelLookup) ||
        !AppendAlignedUploadRange(afterVoxelLookup, plan.voxelSampleBytes, kUploadAlignment, &voxelSamplesOffset, &endOffset)) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    if (plan.metadataBytes > 0) {
        std::memcpy(mapped + metadataOffset, snapshot.metadata.data(), static_cast<size_t>(plan.metadataBytes));
        std::memcpy(mapped + lookupOffset, snapshot.lookup.data(), static_cast<size_t>(plan.lookupBytes));
        std::memcpy(
            mapped + samplesOffset,
            snapshot.samples.data(),
            static_cast<size_t>(plan.sampleBytes));
    }
    if (plan.voxelMetadataBytes > 0) {
        std::memcpy(mapped + voxelMetadataOffset, snapshot.voxelMetadata.data(), static_cast<size_t>(plan.voxelMetadataBytes));
        std::memcpy(mapped + voxelLookupOffset, snapshot.voxelLookup.data(), static_cast<size_t>(plan.voxelLookupBytes));
        std::memcpy(
            mapped + voxelSamplesOffset,
            snapshot.voxelSamples.data(),
            static_cast<size_t>(plan.voxelSampleBytes));
    }
    m_uploadWriteOffset = endOffset;
    m_stats.stagedBytesLastFrame += endOffset - metadataOffset;
    m_stats.stagedMidClipmapTilesLastFrame = uploadHeightLayer ? snapshot.tileCount : 0u;
    uint32_t stagedVoxelSampleSlots = 0;
    for (const MidClipmapUploadPlan::SampleRange& range : plan.voxelSampleRanges) {
        stagedVoxelSampleSlots += range.slotCount;
    }
    m_stats.stagedMidVoxelClipmapBricksLastFrame = uploadVoxelLayer ? stagedVoxelSampleSlots : 0u;
    m_stats.stagedMidClipmapBytesLastFrame =
        plan.metadataBytes + plan.lookupBytes + plan.sampleBytes +
        plan.voxelMetadataBytes + plan.voxelLookupBytes + plan.voxelSampleBytes;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->uploadHeightLayer = uploadHeightLayer;
        outTicket->uploadVoxelLayer = uploadVoxelLayer;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->metadataUploadOffset = metadataOffset;
        outTicket->lookupUploadOffset = lookupOffset;
        outTicket->samplesUploadOffset = samplesOffset;
        outTicket->samplesDestOffset = 0;
        outTicket->heightSampleCopyRanges.clear();
        for (const MidClipmapUploadPlan::SampleRange& range : plan.heightSampleRanges) {
            SparseMidClipmapSampleCopyRange copyRange;
            copyRange.uploadOffset = samplesOffset + range.sourceUintOffset * sizeof(uint32_t);
            copyRange.destinationOffset =
                static_cast<uint64_t>(range.startSlot) *
                static_cast<uint64_t>(snapshot.tileSampleSide) *
                static_cast<uint64_t>(snapshot.tileSampleSide) *
                sizeof(uint32_t);
            copyRange.bytes = range.bytes;
            if (outTicket->heightSampleCopyRanges.empty()) {
                outTicket->samplesDestOffset = copyRange.destinationOffset;
            }
            outTicket->heightSampleCopyRanges.push_back(copyRange);
        }
        outTicket->metadataBytes = plan.metadataBytes;
        outTicket->lookupBytes = plan.lookupBytes;
        outTicket->sampleBytes = plan.sampleBytes;
        outTicket->voxelMetadataUploadOffset = voxelMetadataOffset;
        outTicket->voxelLookupUploadOffset = voxelLookupOffset;
        outTicket->voxelSamplesUploadOffset = voxelSamplesOffset;
        outTicket->voxelSamplesDestOffset = 0;
        outTicket->voxelSampleCopyRanges.clear();
        for (const MidClipmapUploadPlan::SampleRange& range : plan.voxelSampleRanges) {
            SparseMidClipmapSampleCopyRange copyRange;
            copyRange.uploadOffset = voxelSamplesOffset + range.sourceUintOffset * sizeof(uint32_t);
            copyRange.destinationOffset =
                static_cast<uint64_t>(range.startSlot) *
                static_cast<uint64_t>(Simulation::SPARSE_BRICK_VOXEL_COUNT) *
                sizeof(uint32_t);
            copyRange.bytes = range.bytes;
            if (outTicket->voxelSampleCopyRanges.empty()) {
                outTicket->voxelSamplesDestOffset = copyRange.destinationOffset;
            }
            outTicket->voxelSampleCopyRanges.push_back(copyRange);
        }
        outTicket->voxelMetadataBytes = plan.voxelMetadataBytes;
        outTicket->voxelLookupBytes = plan.voxelLookupBytes;
        outTicket->voxelSampleBytes = plan.voxelSampleBytes;
        outTicket->tileCount = snapshot.tileCount;
        outTicket->tileSampleSide = snapshot.tileSampleSide;
        outTicket->voxelBrickCount = snapshot.voxelBrickCount;
        outTicket->snapshotSerial = snapshot.frameIndex;
    }
    return true;
}

bool SparseVoxelGpuResources::StagePhysicsWorkPackets(
    const std::vector<Simulation::SparsePhysicsWorkPacket>& packets,
    SparsePhysicsPacketGpuUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    m_pendingPhysicsPacketResultPackets.clear();
    if (!m_stats.initialized || packets.empty()) {
        return false;
    }
    if (m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    const uint32_t packetCount = std::min<uint32_t>(
        static_cast<uint32_t>(packets.size()),
        m_config.maxPhysicsWorkPackets);
    if (packetCount < packets.size()) {
        m_stats.physicsPacketUploadOverflowLastFrame = true;
    }

    const uint64_t bytes =
        static_cast<uint64_t>(packetCount) *
        sizeof(Simulation::SparsePhysicsWorkPacket);
    if (bytes == 0 || bytes > m_stats.physicsWorkPacketBytes) {
        m_stats.physicsPacketUploadOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    uint64_t uploadOffset = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &endOffset) ||
        endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        m_stats.physicsPacketUploadOverflowLastFrame = true;
        return false;
    }

    std::memcpy(mapped + uploadOffset, packets.data(), static_cast<size_t>(bytes));
    m_pendingPhysicsPacketResultPackets.assign(packets.begin(), packets.begin() + packetCount);
    m_uploadWriteOffset = endOffset;
    m_stats.stagedBytesLastFrame += endOffset - uploadOffset;
    m_stats.stagedPhysicsPacketsLastFrame = packetCount;
    m_stats.stagedPhysicsPacketBytesLastFrame = bytes;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->uploadOffset = uploadOffset;
        outTicket->bytes = bytes;
        outTicket->packetCount = packetCount;
    }
    return true;
}

bool SparseVoxelGpuResources::StageEditDeltas(
    const std::vector<Simulation::SparseEditDelta>& deltas,
    SparseEditDeltaGpuUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || deltas.empty()) {
        return false;
    }
    if (m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }

    const uint32_t dynamicRangeTableCapacity =
        BuildEditDeltaRangeTableCapacity(
            deltas.size(),
            m_config.maxEditDeltaRanges,
            m_config.editDeltaRangeTableCapacity);

    const Simulation::SparseEditDeltaBatch batch =
        Simulation::BuildSparseEditDeltaBatch(
            deltas,
            m_config.maxEditDeltas,
            m_config.maxEditDeltaRanges,
            dynamicRangeTableCapacity);
    if (batch.truncated) {
        m_stats.editDeltaUploadOverflowLastFrame = true;
    }
    const uint32_t deltaCount = static_cast<uint32_t>(batch.deltas.size());
    const uint32_t rangeCount = static_cast<uint32_t>(batch.ranges.size());
    const uint32_t rangeTableCapacity = static_cast<uint32_t>(batch.rangeTable.size());
    const uint64_t bytes =
        static_cast<uint64_t>(deltaCount) *
        sizeof(Simulation::SparseEditDelta);
    const uint64_t rangeBytes =
        static_cast<uint64_t>(rangeCount) *
        sizeof(Simulation::SparseEditDeltaRange);
    const uint64_t rangeTableBytes =
        static_cast<uint64_t>(rangeTableCapacity) *
        sizeof(uint32_t);
    if (bytes == 0 || rangeBytes == 0 ||
        rangeTableBytes == 0 ||
        bytes > m_stats.editDeltaBytes ||
        rangeBytes > m_stats.editDeltaRangeBytes ||
        rangeTableBytes > m_stats.editDeltaRangeTableBytes) {
        m_stats.editDeltaUploadOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    uint64_t uploadOffset = 0;
    uint64_t rangeUploadOffset = 0;
    uint64_t rangeTableUploadOffset = 0;
    uint64_t afterDeltas = 0;
    uint64_t afterRanges = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, bytes, kUploadAlignment, &uploadOffset, &afterDeltas) ||
        !AppendAlignedUploadRange(afterDeltas, rangeBytes, kUploadAlignment, &rangeUploadOffset, &afterRanges) ||
        !AppendAlignedUploadRange(afterRanges, rangeTableBytes, kUploadAlignment, &rangeTableUploadOffset, &endOffset)) {
        m_stats.uploadRingOverflowLastFrame = true;
        m_stats.editDeltaUploadOverflowLastFrame = true;
        return false;
    }
    if (endOffset > upload.GetSize()) {
        m_stats.uploadRingOverflowLastFrame = true;
        m_stats.editDeltaUploadOverflowLastFrame = true;
        return false;
    }

    std::memcpy(mapped + uploadOffset, batch.deltas.data(), static_cast<size_t>(bytes));
    std::memcpy(mapped + rangeUploadOffset, batch.ranges.data(), static_cast<size_t>(rangeBytes));
    std::memcpy(mapped + rangeTableUploadOffset, batch.rangeTable.data(), static_cast<size_t>(rangeTableBytes));
    m_uploadWriteOffset = endOffset;
    m_stats.stagedBytesLastFrame += endOffset - uploadOffset;
    m_stats.stagedEditDeltasLastFrame = deltaCount;
    m_stats.stagedEditDeltaRangesLastFrame = rangeCount;
    m_stats.stagedEditDeltaRangeTableEntriesLastFrame = rangeTableCapacity;
    m_stats.stagedEditDeltaBytesLastFrame = bytes + rangeBytes + rangeTableBytes;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->uploadOffset = uploadOffset;
        outTicket->rangeUploadOffset = rangeUploadOffset;
        outTicket->rangeTableUploadOffset = rangeTableUploadOffset;
        outTicket->bytes = bytes;
        outTicket->rangeBytes = rangeBytes;
        outTicket->rangeTableBytes = rangeTableBytes;
        outTicket->deltaCount = deltaCount;
        outTicket->rangeCount = rangeCount;
        outTicket->rangeTableCapacity = rangeTableCapacity;
        outTicket->inputFullyRepresented = !batch.truncated;
    }
    return true;
}

bool SparseVoxelGpuResources::BeginPageTableCopyBatch(ID3D12GraphicsCommandList* commandList)
{
    if (!m_stats.initialized || !commandList || !m_pageTable.GetResource()) {
        return false;
    }
    m_pageTable.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    return true;
}

void SparseVoxelGpuResources::EndPageTableCopyBatch(ID3D12GraphicsCommandList* commandList)
{
    if (!m_stats.initialized || !commandList || !m_pageTable.GetResource()) {
        return;
    }
    m_pageTable.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool SparseVoxelGpuResources::EmitPageTableCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparsePageTableGpuUploadTicket& ticket,
    bool manageResourceState)
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
    if (!IsSparseVoxelGpuCopyRangeInBounds(
            ticket.uploadOffset,
            ticket.pageTableOffset,
            ticket.bytes,
            m_uploadRing[ticket.ringSlot].GetSize(),
            m_pageTable.GetSize())) {
        spdlog::warn("SparseVoxelGpuResources::EmitPageTableCopy rejected out-of-bounds copy ticket");
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    if (manageResourceState) {
        m_pageTable.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    commandList->CopyBufferRegion(
        m_pageTable.GetResource(),
        ticket.pageTableOffset,
        uploadResource,
        ticket.uploadOffset,
        ticket.bytes);
    if (manageResourceState) {
        m_pageTable.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    return true;
}

bool SparseVoxelGpuResources::EmitPhysicsPacketCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparsePhysicsPacketGpuUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots ||
        ticket.bytes > m_stats.physicsWorkPacketBytes) {
        return false;
    }

    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource || !m_physicsWorkPackets.GetResource()) {
        return false;
    }
    if (!IsSparseVoxelGpuCopyRangeInBounds(
            ticket.uploadOffset,
            0u,
            ticket.bytes,
            m_uploadRing[ticket.ringSlot].GetSize(),
            m_physicsWorkPackets.GetSize())) {
        spdlog::warn("SparseVoxelGpuResources::EmitPhysicsPacketCopy rejected out-of-bounds copy ticket");
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    m_physicsWorkPackets.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_physicsWorkPackets.GetResource(),
        0,
        uploadResource,
        ticket.uploadOffset,
        ticket.bytes);
    m_physicsWorkPackets.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool SparseVoxelGpuResources::EmitEditDeltaCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparseEditDeltaGpuUploadTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots ||
        ticket.bytes > m_stats.editDeltaBytes ||
        ticket.rangeBytes > m_stats.editDeltaRangeBytes ||
        ticket.rangeTableBytes > m_stats.editDeltaRangeTableBytes) {
        return false;
    }

    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource ||
        !m_editDeltas.GetResource() ||
        !m_editDeltaRanges.GetResource() ||
        !m_editDeltaRangeTable.GetResource()) {
        return false;
    }
    const uint64_t uploadSize = m_uploadRing[ticket.ringSlot].GetSize();
    if (!IsSparseVoxelGpuCopyRangeInBounds(
            ticket.uploadOffset,
            0u,
            ticket.bytes,
            uploadSize,
            m_editDeltas.GetSize()) ||
        !IsSparseVoxelGpuCopyRangeInBounds(
            ticket.rangeUploadOffset,
            0u,
            ticket.rangeBytes,
            uploadSize,
            m_editDeltaRanges.GetSize()) ||
        !IsSparseVoxelGpuCopyRangeInBounds(
            ticket.rangeTableUploadOffset,
            0u,
            ticket.rangeTableBytes,
            uploadSize,
            m_editDeltaRangeTable.GetSize())) {
        spdlog::warn("SparseVoxelGpuResources::EmitEditDeltaCopy rejected out-of-bounds copy ticket");
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    m_editDeltas.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_editDeltas.GetResource(),
        0,
        uploadResource,
        ticket.uploadOffset,
        ticket.bytes);
    m_editDeltas.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_editDeltaRanges.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_editDeltaRanges.GetResource(),
        0,
        uploadResource,
        ticket.rangeUploadOffset,
        ticket.rangeBytes);
    m_editDeltaRanges.TransitionTo(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_editDeltaRangeTable.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_editDeltaRangeTable.GetResource(),
        0,
        uploadResource,
        ticket.rangeTableUploadOffset,
        ticket.rangeTableBytes);
    m_editDeltaRangeTable.TransitionTo(
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
        const uint64_t uploadSize = m_uploadRing[ticket.ringSlot].GetSize();
        bool heightSampleRangesValid = ticket.sampleBytes == 0;
        uint64_t heightSampleRangeBytes = 0;
        for (const SparseMidClipmapSampleCopyRange& range : ticket.heightSampleCopyRanges) {
            heightSampleRangeBytes += range.bytes;
            if (!IsSparseVoxelGpuCopyRangeInBounds(
                    range.uploadOffset,
                    range.destinationOffset,
                    range.bytes,
                    uploadSize,
                    m_midClipmapSamples.GetSize())) {
                heightSampleRangesValid = false;
                break;
            }
        }
        heightSampleRangesValid =
            heightSampleRangesValid ||
            (!ticket.heightSampleCopyRanges.empty() && heightSampleRangeBytes == ticket.sampleBytes);
        if (!IsSparseVoxelGpuCopyRangeInBounds(
                ticket.metadataUploadOffset,
                0u,
                ticket.metadataBytes,
                uploadSize,
                m_midClipmapMetadata.GetSize()) ||
            !IsSparseVoxelGpuCopyRangeInBounds(
                ticket.lookupUploadOffset,
                0u,
                ticket.lookupBytes,
                uploadSize,
                m_midClipmapLookup.GetSize()) ||
            !heightSampleRangesValid) {
            spdlog::warn("SparseVoxelGpuResources::EmitMidClipmapCopy rejected out-of-bounds height copy ticket");
            m_stats.uploadRingOverflowLastFrame = true;
            return false;
        }
    }
    if (ticket.uploadVoxelLayer) {
        const uint64_t uploadSize = m_uploadRing[ticket.ringSlot].GetSize();
        bool voxelSampleRangesValid = ticket.voxelSampleBytes == 0;
        uint64_t voxelSampleRangeBytes = 0;
        for (const SparseMidClipmapSampleCopyRange& range : ticket.voxelSampleCopyRanges) {
            voxelSampleRangeBytes += range.bytes;
            if (!IsSparseVoxelGpuCopyRangeInBounds(
                    range.uploadOffset,
                    range.destinationOffset,
                    range.bytes,
                    uploadSize,
                    m_midVoxelClipmapSamples.GetSize())) {
                voxelSampleRangesValid = false;
                break;
            }
        }
        voxelSampleRangesValid =
            voxelSampleRangesValid ||
            (!ticket.voxelSampleCopyRanges.empty() && voxelSampleRangeBytes == ticket.voxelSampleBytes);
        if (!IsSparseVoxelGpuCopyRangeInBounds(
                ticket.voxelMetadataUploadOffset,
                0u,
                ticket.voxelMetadataBytes,
                uploadSize,
                m_midVoxelClipmapMetadata.GetSize()) ||
            !IsSparseVoxelGpuCopyRangeInBounds(
                ticket.voxelLookupUploadOffset,
                0u,
                ticket.voxelLookupBytes,
                uploadSize,
                m_midVoxelClipmapLookup.GetSize()) ||
            !voxelSampleRangesValid) {
            spdlog::warn("SparseVoxelGpuResources::EmitMidClipmapCopy rejected out-of-bounds voxel copy ticket");
            m_stats.uploadRingOverflowLastFrame = true;
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
        for (const SparseMidClipmapSampleCopyRange& range : ticket.heightSampleCopyRanges) {
            commandList->CopyBufferRegion(
                m_midClipmapSamples.GetResource(),
                range.destinationOffset,
                uploadResource,
                range.uploadOffset,
                range.bytes);
        }
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
        for (const SparseMidClipmapSampleCopyRange& range : ticket.voxelSampleCopyRanges) {
            commandList->CopyBufferRegion(
                m_midVoxelClipmapSamples.GetResource(),
                range.destinationOffset,
                uploadResource,
                range.uploadOffset,
                range.bytes);
        }
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
    const uint64_t uploadSize = m_uploadRing[ticket.ringSlot].GetSize();
    if (!ticket.voxelCopyRanges.empty()) {
        for (const SparseBrickVoxelCopyRange& range : ticket.voxelCopyRanges) {
            if (!IsSparseVoxelGpuBrickCopyRangeInBounds(range, uploadSize, m_brickPool.GetSize())) {
                spdlog::warn("SparseVoxelGpuResources::EmitUploadCopy rejected out-of-bounds partial brick copy ticket");
                m_stats.uploadRingOverflowLastFrame = true;
                return false;
            }
        }
    } else if (!IsSparseVoxelGpuCopyRangeInBounds(
            ticket.voxelUploadOffset,
            ticket.brickPoolOffset,
            ticket.voxelBytes,
            uploadSize,
            m_brickPool.GetSize())) {
        spdlog::warn("SparseVoxelGpuResources::EmitUploadCopy rejected out-of-bounds brick copy ticket");
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }
    if (!IsSparseVoxelGpuCopyRangeInBounds(
            ticket.occupancyUploadOffset,
            ticket.occupancyBufferOffset,
            ticket.occupancyBytes,
            uploadSize,
            m_occupancy.GetSize()) ||
        !IsSparseVoxelGpuCopyRangeInBounds(
            ticket.generationUploadOffset,
            ticket.pageGenerationBufferOffset,
            ticket.generationBytes,
            uploadSize,
            m_pageGeneration.GetSize())) {
        spdlog::warn("SparseVoxelGpuResources::EmitUploadCopy rejected out-of-bounds brick metadata copy ticket");
        m_stats.uploadRingOverflowLastFrame = true;
        return false;
    }

    m_brickPool.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_occupancy.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_pageGeneration.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    if (!ticket.voxelCopyRanges.empty()) {
        for (const SparseBrickVoxelCopyRange& range : ticket.voxelCopyRanges) {
            if (range.bytes == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_brickPool.GetResource(),
                range.brickPoolOffset,
                uploadResource,
                range.uploadOffset,
                range.bytes);
        }
    } else {
        commandList->CopyBufferRegion(
            m_brickPool.GetResource(),
            ticket.brickPoolOffset,
            uploadResource,
            ticket.voxelUploadOffset,
            ticket.voxelBytes);
    }
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
    if (!ValidateSparseVoxelGpuConfigForStats(config)) {
        return stats;
    }
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
    stats.brushFeedbackBytes =
        static_cast<uint64_t>(config.maxBrushFeedbackRecords + 1u) *
        sizeof(uint32_t) * 4u;
    stats.physicsWorkPacketBytes =
        static_cast<uint64_t>(config.maxPhysicsWorkPackets) *
        sizeof(Simulation::SparsePhysicsWorkPacket);
    stats.physicsPacketResultBytes =
        static_cast<uint64_t>(config.maxPhysicsWorkPackets) *
        sizeof(Simulation::SparsePhysicsPacketResult);
    stats.physicsDiagnosticBytes =
        static_cast<uint64_t>(kSparsePhysicsDiagnosticWords) * sizeof(uint32_t);
    stats.renderOwnershipBytes =
        static_cast<uint64_t>(kSparseRenderOwnershipWords) * sizeof(uint32_t);
    stats.editDeltaBytes =
        static_cast<uint64_t>(config.maxEditDeltas) *
        sizeof(Simulation::SparseEditDelta);
    stats.editDeltaRangeBytes =
        static_cast<uint64_t>(config.maxEditDeltaRanges) *
        sizeof(Simulation::SparseEditDeltaRange);
    stats.editDeltaRangeTableBytes =
        static_cast<uint64_t>(config.editDeltaRangeTableCapacity) *
        sizeof(uint32_t);
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
        stats.physicsWorkPacketBytes +
        stats.physicsPacketResultBytes +
        stats.physicsDiagnosticBytes +
        stats.renderOwnershipBytes +
        stats.editDeltaBytes +
        stats.editDeltaRangeBytes +
        stats.editDeltaRangeTableBytes +
        stats.missFeedbackBytes +
        stats.brushFeedbackBytes +
        stats.uploadRingBytes;
    return stats;
}

bool SparseVoxelGpuResources::IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

} // namespace VENPOD::Graphics
