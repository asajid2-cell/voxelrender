#include "SparseSurfaceGpuResources.h"

#include <algorithm>
#include <cstring>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kMaxUploadRingSlots = 3;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

SparseSurfaceGpuResources::~SparseSurfaceGpuResources() {
    Shutdown();
}

Result<void> SparseSurfaceGpuResources::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    const SparseSurfaceGpuConfig& config)
{
    if (!device) {
        return Error("SparseSurfaceGpuResources::Initialize - device is null");
    }
    if (config.maxFaces == 0 || config.maxBrickRanges == 0) {
        return Error("SparseSurfaceGpuResources::Initialize - capacities must be > 0");
    }
    if (config.uploadRingSlots == 0 || config.uploadRingSlots > kMaxUploadRingSlots) {
        return Error("SparseSurfaceGpuResources::Initialize - uploadRingSlots must be 1..{}", kMaxUploadRingSlots);
    }

    Shutdown();
    m_config = config;
    m_stats = {};
    m_stats.initialized = true;
    m_stats.maxFaces = config.maxFaces;
    m_stats.maxBrickRanges = config.maxBrickRanges;

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

    for (uint32_t i = 0; i < config.uploadRingSlots; ++i) {
        result = m_uploadRing[i].Initialize(
            device,
            config.uploadBytesPerSlot,
            "SparseSurfaceUploadRing");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface upload ring: {}", result.error());
        }
    }

    return {};
}

void SparseSurfaceGpuResources::Shutdown() {
    m_faceBuffer.Shutdown();
    m_rangeBuffer.Shutdown();
    for (auto& upload : m_uploadRing) {
        upload.Shutdown();
    }
    m_stats = {};
    m_uploadWriteOffset = 0;
    m_activeUploadSlot = 0;
}

void SparseSurfaceGpuResources::BeginFrame(uint32_t frameIndex) {
    if (m_config.uploadRingSlots == 0) {
        return;
    }
    m_activeUploadSlot = frameIndex % m_config.uploadRingSlots;
    m_uploadWriteOffset = 0;
    m_stats.stagedFacesLastFrame = 0;
    m_stats.stagedRangesLastFrame = 0;
    m_stats.stagedRangeTableCapacityLastFrame = 0;
    m_stats.stagedCandidateBricksLastFrame = 0;
    m_stats.stagedVisibleBricksLastFrame = 0;
    m_stats.stagedCulledBricksLastFrame = 0;
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
        snapshot.ranges.size() > m_config.maxBrickRanges) {
        m_stats.uploadOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t faceBytes =
        static_cast<uint64_t>(snapshot.faces.size()) * sizeof(Simulation::SparseSurfaceFace);
    const uint64_t rangeBytes =
        static_cast<uint64_t>(snapshot.ranges.size()) * sizeof(Simulation::SparseSurfaceBrickRange);
    const uint64_t faceOffset = AlignUp(m_uploadWriteOffset, kUploadAlignment);
    const uint64_t rangeOffset = AlignUp(faceOffset + faceBytes, kUploadAlignment);
    const uint64_t endOffset = rangeOffset + rangeBytes;
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

    m_uploadWriteOffset = endOffset;
    m_stats.stagedFacesLastFrame = static_cast<uint32_t>(snapshot.faces.size());
    m_stats.stagedRangesLastFrame = snapshot.rangeCount;
    m_stats.stagedRangeTableCapacityLastFrame = static_cast<uint32_t>(snapshot.ranges.size());
    m_stats.stagedCandidateBricksLastFrame = snapshot.candidateBricks;
    m_stats.stagedVisibleBricksLastFrame = snapshot.visibleBricks;
    m_stats.stagedCulledBricksLastFrame = snapshot.culledBricks;
    m_stats.stagedBytesLastFrame = endOffset - faceOffset;

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->faceUploadOffset = faceOffset;
        outTicket->rangeUploadOffset = rangeOffset;
        outTicket->faceBytes = faceBytes;
        outTicket->rangeBytes = rangeBytes;
        outTicket->faceCount = static_cast<uint32_t>(snapshot.faces.size());
        outTicket->rangeCount = snapshot.rangeCount;
        outTicket->rangeTableCapacity = static_cast<uint32_t>(snapshot.ranges.size());
        outTicket->serial = snapshot.serial;
        outTicket->candidateBricks = snapshot.candidateBricks;
        outTicket->visibleBricks = snapshot.visibleBricks;
        outTicket->culledBricks = snapshot.culledBricks;
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
    if (!upload || !m_faceBuffer.GetResource() || !m_rangeBuffer.GetResource()) {
        return false;
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
    }

    m_stats.uploadedFaces = ticket.faceCount;
    m_stats.uploadedRanges = ticket.rangeCount;
    m_stats.uploadedRangeTableCapacity = ticket.rangeTableCapacity;
    m_stats.uploadedSerial = ticket.serial;
    m_stats.uploadedCandidateBricks = ticket.candidateBricks;
    m_stats.uploadedVisibleBricks = ticket.visibleBricks;
    m_stats.uploadedCulledBricks = ticket.culledBricks;
    return true;
}

} // namespace VENPOD::Graphics
