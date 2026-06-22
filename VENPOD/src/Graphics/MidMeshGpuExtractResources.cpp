#include "MidMeshGpuExtractResources.h"

#include "MidMeshFaceAbCompare.h"
#include "RHI/DX12CommandQueue.h"
#include "Simulation/TerrainConstants.h"  // SEA_LEVEL_Y (mirrored into the GPU skirt rule)

#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>

namespace VENPOD::Graphics {

// B1.3e: the GPU edit-box (HLSL MidMeshEditBox) is 4x int32; the CPU MidMeshEditXzBox must
// match byte-for-byte so the upload is a straight memcpy and the shader's overlap test reads
// the same coords the CPU `cellInEditFootprint` would.
static_assert(sizeof(Simulation::MidMeshEditXzBox) == 16u,
    "MidMeshEditXzBox must be 4x int32 (16B) to match the HLSL MidMeshEditBox ABI");

namespace {

constexpr uint32_t kMaxUploadRingSlots = 3u;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0u) {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

MidMeshGpuExtractResources::~MidMeshGpuExtractResources() {
    Shutdown();
}

Result<void> MidMeshGpuExtractResources::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    const MidMeshGpuExtractConfig& config)
{
    if (!device) {
        return Error("MidMeshGpuExtractResources::Initialize - device is null");
    }
    if (config.maxTiles == 0u) {
        return Error("MidMeshGpuExtractResources::Initialize - maxTiles must be > 0");
    }
    if (config.tileSampleSide < 2u) {
        return Error("MidMeshGpuExtractResources::Initialize - tileSampleSide must be >= 2");
    }
    if (config.uploadRingSlots == 0u || config.uploadRingSlots > kMaxUploadRingSlots) {
        return Error("MidMeshGpuExtractResources::Initialize - uploadRingSlots out of range");
    }

    m_config = config;
    m_heapManager = &heapManager;
    m_samplesPerTile = config.tileSampleSide * config.tileSampleSide;
    m_sampleTileBytes = static_cast<uint64_t>(m_samplesPerTile) * sizeof(uint32_t);

    // Overflow guard: maxTiles * samplesPerTile * 4 must fit a uint64 and a D3D12
    // structured-buffer view (UINT element count).
    const uint64_t totalSampleElements =
        static_cast<uint64_t>(config.maxTiles) * static_cast<uint64_t>(m_samplesPerTile);
    if (totalSampleElements == 0u ||
        totalSampleElements > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return Error("MidMeshGpuExtractResources::Initialize - sample element count exceeds view limit");
    }
    const uint64_t sampleBufferBytes = totalSampleElements * sizeof(uint32_t);
    const uint64_t metadataBufferBytes =
        static_cast<uint64_t>(config.maxTiles) * sizeof(MidMeshGpuExtractTileMeta);

    // Persistent SAMPLE buffer: per-tile region of side*side uint32, keyed by tile slot.
    // UAV reserved for the future compute shader; B1.1 only COPIES into it.
    auto sampleResult = m_sampleBuffer.Initialize(
        device,
        sampleBufferBytes,
        BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "MidMeshGpuExtractSamples");
    if (!sampleResult) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - sample buffer: {}", sampleResult.error());
    }
    if (auto srv = m_sampleBuffer.CreateSRV(device, heapManager); !srv) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - sample SRV: {}", srv.error());
    }
    if (auto uav = m_sampleBuffer.CreateUAV(device, heapManager); !uav) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - sample UAV: {}", uav.error());
    }

    // Persistent METADATA buffer: one hard-ABI struct per tile slot.
    auto metaResult = m_metadataBuffer.Initialize(
        device,
        metadataBufferBytes,
        BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(MidMeshGpuExtractTileMeta),
        "MidMeshGpuExtractMeta");
    if (!metaResult) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - metadata buffer: {}", metaResult.error());
    }
    if (auto srv = m_metadataBuffer.CreateSRV(device, heapManager); !srv) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - metadata SRV: {}", srv.error());
    }
    if (auto uav = m_metadataBuffer.CreateUAV(device, heapManager); !uav) {
        Shutdown();
        return Error("MidMeshGpuExtractResources::Initialize - metadata UAV: {}", uav.error());
    }

    // Per-frame upload ring. Size one slot to hold a FULL recenter (all tiles dirty)
    // so the dirty path never silently drops tiles on a worst-case frame: maxTiles *
    // (sampleTileBytes + metaBytes), 256-byte aligned per the copy-placement rule.
    const uint64_t worstCaseFrameBytes =
        static_cast<uint64_t>(config.maxTiles) *
        (m_sampleTileBytes + sizeof(MidMeshGpuExtractTileMeta));
    m_uploadRingSlotBytes = AlignUp(std::max<uint64_t>(worstCaseFrameBytes, 256u), 256u);
    for (uint32_t i = 0; i < config.uploadRingSlots; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "MidMeshGpuExtractUpload%u", i);
        auto ringResult = m_uploadRing[i].Initialize(device, m_uploadRingSlotBytes, name);
        if (!ringResult) {
            Shutdown();
            return Error("MidMeshGpuExtractResources::Initialize - upload ring {}: {}", i, ringResult.error());
        }
        m_uploadRingFence[i] = 0u;
    }

    m_activeRingSlot = 0;
    m_currentFrameFenceValue = 0;
    m_sampleBufferInCopyDest = false;
    m_metadataBufferInCopyDest = false;

    m_stats = {};
    m_stats.initialized = true;
    m_stats.maxTiles = config.maxTiles;
    m_stats.sampleSide = config.tileSampleSide;
    m_stats.samplesPerTile = m_samplesPerTile;
    m_stats.sampleBufferBytes = sampleBufferBytes;
    m_stats.metadataBufferBytes = metadataBufferBytes;
    m_stats.uploadRingSlotBytes = m_uploadRingSlotBytes;
    m_stats.productionFaceCapacityPerTile =
        std::max(1u, config.productionFaceCapacityPerTile);
    m_stats.productionFaceBufferBytes = 0;
    m_stats.productionFaceCountBufferBytes = 0;
    m_stats.productionFaceStatusBufferBytes = 0;
    m_stats.productionDrawArgsBufferBytes = 0;
    m_stats.productionCommitBufferBytes = 0;
    m_stats.productionDrawCommandCount = 0;
    m_stats.productionCommittedSlots = 0;
    return {};
}

void MidMeshGpuExtractResources::Shutdown() {
    // B1.2 smoke (compute side).
    m_smokePipeline.Shutdown();
    m_smokeInputUpload.Shutdown();
    m_smokeSampleBuffer.Shutdown();
    m_smokeMetaBuffer.Shutdown();
    m_smokeFaceBuffer.Shutdown();
    m_editBoxBuffer.Shutdown();
    m_editBoxUpload.Shutdown();
    m_editBoxCapacityPerTile = 0;
    m_cullBlockBuffer.Shutdown();
    m_cullBlockUpload.Shutdown();
    m_cullBlockCapacityPerTile = 0;
    m_productionFaceBuffer.Shutdown();
    m_productionFaceCountBuffer.Shutdown();
    m_productionFaceStatusBuffer.Shutdown();
    m_productionCommitBuffer.Shutdown();
    m_productionDrawArgsBuffer.Shutdown();
    m_productionCountClearUpload.Shutdown();
    m_productionCommitUpload.Shutdown();
    m_productionFaceCapacityPerTile = 0;
    m_productionFaceBufferBytes = 0;
    m_productionFaceCountBufferBytes = 0;
    m_productionFaceStatusBufferBytes = 0;
    m_productionDrawArgsBufferBytes = 0;
    m_productionCommitBufferBytes = 0;
    m_productionDrawArgsPipeline.Shutdown();
    m_productionDrawArgsShader = {};
    // B1.3.0 readback ring: unmap the persistent maps, then release the buffers.
    for (auto& slot : m_smokeReadbackRing) {
        if (slot.facePtr) {
            slot.faceReadback.Unmap();
            slot.facePtr = nullptr;
        }
        if (slot.metaPtr) {
            slot.metaReadback.Unmap();
            slot.metaPtr = nullptr;
        }
        if (slot.countPtr) {
            slot.countReadback.Unmap();
            slot.countPtr = nullptr;
        }
        if (slot.statusPtr) {
            slot.statusReadback.Unmap();
            slot.statusPtr = nullptr;
        }
        slot.faceReadback.Shutdown();
        slot.metaReadback.Shutdown();
        slot.countReadback.Shutdown();
        slot.statusReadback.Shutdown();
        slot.fenceValue = 0;
        slot.pending = false;
        slot.tile = {};
    }
    m_smokeReadbackWriteSlot = 0;
    m_smokeReadbackReadSlot = 0;
    m_smokeReadbackForceWait = false;
    m_smokeQueryReadback.Shutdown();
    m_smokeQueryHeap.Reset();
    m_smokeCmdList.Reset();
    m_smokeCmdAllocator.Reset();
    if (m_smokeFenceEvent) {
        CloseHandle(m_smokeFenceEvent);
        m_smokeFenceEvent = nullptr;
    }
    m_smokeFence.Reset();
    m_smokeQueue.Reset();
    m_smokeFenceValue = 0;
    m_smokeSelfTestDone = false;
    m_smokeAbVerifyCount = 0;
    m_smokeReady = false;
    m_heapManager = nullptr;
    m_smokeTimestampFrequency = 0;
    m_smokeFaceCapacityPerTile = 0;
    m_debugFaceCapacityPerTile = 0;
    m_smokeFaceBufferBytes = 0;
    m_smokeControlled = {};
    m_smokeStats = {};
    // B1.3a top-face path (reuses the smoke isolated objects, so only its own state here).
    m_topFacePipeline.Shutdown();
    m_b13aReady = false;
    m_topFaceCapacityPerTile = 0;
    m_b13aAbVerifyCount = 0;
    m_b13aBuildTerraceStep = 1;
    m_b13aStats = {};

    for (auto& ring : m_uploadRing) {
        ring.Shutdown();
    }
    m_uploadRingFence = {};
    m_metadataBuffer.Shutdown();
    m_sampleBuffer.Shutdown();
    m_stats = {};
    m_samplesPerTile = 0;
    m_sampleTileBytes = 0;
    m_uploadRingSlotBytes = 0;
    m_activeRingSlot = 0;
    m_currentFrameFenceValue = 0;
    m_sampleBufferInCopyDest = false;
    m_metadataBufferInCopyDest = false;
}

void MidMeshGpuExtractResources::BeginFrame(
    uint32_t /*frameIndex*/,
    uint64_t /*completedFenceValue*/,
    uint64_t currentFrameFenceValue)
{
    if (!m_stats.initialized) {
        return;
    }
    // Advance the ring; the in-flight guard in StageDirtyTiles uses the recorded
    // fence per slot vs the completed fence to avoid clobbering live uploads.
    m_activeRingSlot = (m_activeRingSlot + 1u) % m_config.uploadRingSlots;
    m_currentFrameFenceValue = currentFrameFenceValue;
}

bool MidMeshGpuExtractResources::StageDirtyTiles(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    MidMeshGpuExtractTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    // Reset per-frame stats fields (keep the persistent sizing fields).
    m_stats.sampleUploadTiles = 0;
    m_stats.sampleUploadBytes = 0;
    m_stats.skippedNoSlotTiles = 0;
    m_stats.deferredRingFullTiles = 0;
    m_stats.uploadRingWaitSkipped = false;

    if (!m_stats.initialized || dirtyTiles.empty()) {
        return false;
    }

    const uint32_t ringSlot = m_activeRingSlot;
    UploadBuffer& upload = m_uploadRing[ringSlot];
    auto* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        m_stats.uploadRingWaitSkipped = true;
        return false;
    }

    MidMeshGpuExtractTicket ticket;
    ticket.ringSlot = ringSlot;
    ticket.sampleCopies.reserve(dirtyTiles.size());
    ticket.metaCopies.reserve(dirtyTiles.size());

    uint64_t writeOffset = 0;
    const uint32_t heightDesc = PackMidMeshHeightDesc();

    for (const Simulation::MidMeshGpuExtractDirtyTile& tile : dirtyTiles) {
        // A dirty coord that no longer resolves to a resident slot (evicted /
        // re-centered) carries no sample pointer; the accessor already skipped it,
        // but guard defensively here too.
        if (tile.samples == nullptr || tile.sampleCount == 0u || tile.slot >= m_config.maxTiles) {
            ++m_stats.skippedNoSlotTiles;
            continue;
        }
        const uint64_t tileSampleBytes =
            static_cast<uint64_t>(tile.sampleCount) * sizeof(uint32_t);
        const uint64_t needBytes = tileSampleBytes + sizeof(MidMeshGpuExtractTileMeta);
        // Ring slot full -> defer this tile (it stays dirty; re-fired next frame by
        // the existing midMeshUploadCatchup trigger). Never silently drop it.
        if (writeOffset + needBytes > m_uploadRingSlotBytes) {
            ++m_stats.deferredRingFullTiles;
            continue;
        }

        // --- samples ---
        const uint64_t sampleUploadOffset = writeOffset;
        std::memcpy(mapped + sampleUploadOffset, tile.samples, static_cast<size_t>(tileSampleBytes));
        MidMeshGpuExtractTicket::SampleCopy sc;
        sc.uploadOffset = sampleUploadOffset;
        sc.destOffset = static_cast<uint64_t>(tile.slot) * m_sampleTileBytes;
        sc.byteCount = tileSampleBytes;
        ticket.sampleCopies.push_back(sc);
        writeOffset += tileSampleBytes;

        // --- metadata (hard ABI struct) ---
        MidMeshGpuExtractTileMeta meta;
        meta.coordX = tile.coord.x;
        meta.coordRing = tile.coord.y;   // synthetic ring lives in BrickCoord.y
        meta.coordZ = tile.coord.z;
        meta.originX = tile.originX;
        meta.originZ = tile.originZ;
        std::memcpy(&meta.cellSizeBits, &tile.cellSize, sizeof(uint32_t));
        meta.mergeCells = tile.mergeCells;
        meta.childMask = tile.childMask;
        meta.meshContentVersionLo = static_cast<uint32_t>(tile.meshContentVersion & 0xFFFFFFFFull);
        meta.meshContentVersionHi = static_cast<uint32_t>(tile.meshContentVersion >> 32u);
        meta.sampleSide = m_config.tileSampleSide;
        meta.sampleStride = m_config.tileSampleSide;  // dense rows for now
        meta.haloWidth = 0u;                          // RESERVED for B1.3
        meta.heightPackDesc = heightDesc;
        meta.heightBias = MidMeshSampleHeightBias;
        meta.editFootprintCount = 0u;                 // RESERVED (full mask is later)
        // Pending output range = fixed per-slot production arena metadata. Do not borrow
        // tile.faceCount here; production extraction owns a fixed capacity and reports
        // overflow instead of truncating.
        meta.faceCapacity = std::max(1u, m_config.productionFaceCapacityPerTile);
        meta.baseFace = tile.slot * meta.faceCapacity;
        meta.faceCount = 0u;
        meta.statusOverflow = 0u;

        const uint64_t metaUploadOffset = writeOffset;
        std::memcpy(mapped + metaUploadOffset, &meta, sizeof(meta));
        MidMeshGpuExtractTicket::MetaCopy mc;
        mc.uploadOffset = metaUploadOffset;
        mc.slot = tile.slot;
        ticket.metaCopies.push_back(mc);
        writeOffset += sizeof(MidMeshGpuExtractTileMeta);

        ticket.sampleBytes += tileSampleBytes;
        ticket.metadataBytes += sizeof(MidMeshGpuExtractTileMeta);
        ++ticket.tileCount;
    }

    if (ticket.tileCount == 0u) {
        return false;
    }

    ticket.valid = true;
    ticket.fenceValue = m_currentFrameFenceValue;
    m_uploadRingFence[ringSlot] = m_currentFrameFenceValue;

    m_stats.sampleUploadTiles = ticket.tileCount;
    m_stats.sampleUploadBytes = ticket.sampleBytes + ticket.metadataBytes;

    if (outTicket) {
        *outTicket = std::move(ticket);
    }
    return true;
}

bool MidMeshGpuExtractResources::EmitCopy(
    ID3D12GraphicsCommandList* commandList,
    const MidMeshGpuExtractTicket& ticket)
{
    if (!m_stats.initialized || !commandList || !ticket.valid || ticket.tileCount == 0u) {
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        return false;
    }
    ID3D12Resource* uploadResource = m_uploadRing[ticket.ringSlot].GetResource();
    if (!uploadResource) {
        return false;
    }

    // Both persistent buffers were created in COMMON; transition to COPY_DEST for the
    // batched region copies, then back to COMMON so the (future) compute shader can
    // read them as SRV/UAV. COMMON is a promotable state for buffers so this is safe.
    m_sampleBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_metadataBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    ID3D12Resource* sampleDest = m_sampleBuffer.GetResource();
    ID3D12Resource* metaDest = m_metadataBuffer.GetResource();
    if (!sampleDest || !metaDest) {
        return false;
    }

    for (const auto& sc : ticket.sampleCopies) {
        commandList->CopyBufferRegion(
            sampleDest, sc.destOffset, uploadResource, sc.uploadOffset, sc.byteCount);
    }
    for (const auto& mc : ticket.metaCopies) {
        commandList->CopyBufferRegion(
            metaDest,
            static_cast<uint64_t>(mc.slot) * sizeof(MidMeshGpuExtractTileMeta),
            uploadResource,
            mc.uploadOffset,
            sizeof(MidMeshGpuExtractTileMeta));
    }

    m_sampleBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COMMON);
    m_metadataBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

// =============================================================================
// B1.2 SMOKE - minimal compute proof (compute side).
// =============================================================================

Result<void> MidMeshGpuExtractResources::InitializeSmoke(
    ID3D12Device* device,
    ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath,
    bool readbackForceWait,
    bool b13aEnabled)
{
    m_smokeReadbackForceWait = readbackForceWait;
    if (!m_stats.initialized) {
        return Error("InitializeSmoke - B1.1 resources not initialized");
    }
    if (!device) {
        return Error("InitializeSmoke - device is null");
    }

    m_smokeFaceCapacityPerTile = std::max(1u, m_config.smokeFaceCapacityPerTile);
    m_topFaceCapacityPerTile = std::max(1u, m_config.topFaceCapacityPerTile);
    // The ISOLATED debug face buffer + readback ring must hold the LARGER of the smoke
    // K-quad reservation and (when B1.3a is enabled) the full top-face tile reservation,
    // so both dispatch kinds reuse the same isolated buffers without overflow.
    m_debugFaceCapacityPerTile =
        b13aEnabled ? std::max(m_smokeFaceCapacityPerTile, m_topFaceCapacityPerTile)
                    : m_smokeFaceCapacityPerTile;
    // The smoke shader writes at most smokeMaxCells faces; the per-tile reserved range
    // must be able to hold them (otherwise we always overflow).
    if (m_config.smokeMaxCells > m_smokeFaceCapacityPerTile) {
        spdlog::warn(
            "[GPU_EXTRACT_SMOKE] smokeMaxCells {} > smokeFaceCapacityPerTile {}; "
            "shader will report overflow",
            m_config.smokeMaxCells, m_smokeFaceCapacityPerTile);
    }

    // Dedicated one-tile input buffers (smoke-owned). Slot index is always 0 inside
    // these; the controlled tile's samples/metadata are uploaded here each dispatch.
    if (auto r = m_smokeSampleBuffer.Initialize(
            device, m_sampleTileBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer,
            sizeof(uint32_t), "MidMeshGpuExtractSmokeSamples"); !r) {
        return Error("InitializeSmoke - smoke sample buffer: {}", r.error());
    }
    if (auto r = m_smokeMetaBuffer.Initialize(
            device, sizeof(MidMeshGpuExtractTileMeta),
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(MidMeshGpuExtractTileMeta), "MidMeshGpuExtractSmokeMeta"); !r) {
        return Error("InitializeSmoke - smoke meta buffer: {}", r.error());
    }

    // ISOLATED debug-output face buffer. One controlled tile's reserved range
    // (baseFace=0). This is NOT the production mid-mesh face buffer. Sized to the
    // larger of the smoke / B1.3a per-tile capacity so both reuse it.
    m_smokeFaceBufferBytes =
        static_cast<uint64_t>(m_debugFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace);
    if (auto r = m_smokeFaceBuffer.Initialize(
            device, m_smokeFaceBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(Simulation::SparseSurfaceFace),
            "MidMeshGpuExtractSmokeFaces"); !r) {
        return Error("InitializeSmoke - debug face buffer: {}", r.error());
    }

    // B1.3e EDIT-FOOTPRINT: dedicated per-tile edit-box buffer (SRV-read at t1) + its small
    // upload stage. One tile's boxes at a time (slot 0), sized to editBoxCapacityPerTile.
    // DIRTY-SCALED: only written when an EDITED fixture is dispatched (not every frame/tile).
    m_editBoxCapacityPerTile = std::max(1u, m_config.editBoxCapacityPerTile);
    const uint64_t editBoxBufferBytes =
        static_cast<uint64_t>(m_editBoxCapacityPerTile) * sizeof(Simulation::MidMeshEditXzBox);
    if (auto r = m_editBoxBuffer.Initialize(
            device, editBoxBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer,
            sizeof(Simulation::MidMeshEditXzBox), "MidMeshGpuExtractEditBoxes"); !r) {
        return Error("InitializeSmoke - edit-box buffer: {}", r.error());
    }
    if (auto r = m_editBoxUpload.Initialize(
            device, std::max<uint64_t>(editBoxBufferBytes, 256u),
            "MidMeshGpuExtractEditBoxUpload"); !r) {
        return Error("InitializeSmoke - edit-box upload: {}", r.error());
    }

    // B1.3f-c CAMERA-DISTANCE CULL: dedicated per-tile per-BLOCK cull-flag buffer (SRV-read at
    // t2) + its small upload stage. One tile's blocks at a time (slot 0). Worst case = one block
    // per cell at mergeCells==1 -> (tileSampleSide-1)^2 blocks. One uint per block (1 = CPU
    // culled). DIRTY-SCALED: only written when a distance-cull dispatch runs.
    const uint32_t cellsPerAxis =
        (m_config.tileSampleSide > 1u) ? (m_config.tileSampleSide - 1u) : 1u;
    m_cullBlockCapacityPerTile = std::max(1u, cellsPerAxis * cellsPerAxis);
    const uint64_t cullBlockBufferBytes =
        static_cast<uint64_t>(m_cullBlockCapacityPerTile) * sizeof(uint32_t);
    if (auto r = m_cullBlockBuffer.Initialize(
            device, cullBlockBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer,
            sizeof(uint32_t), "MidMeshGpuExtractCullBlocks"); !r) {
        return Error("InitializeSmoke - cull-block buffer: {}", r.error());
    }
    if (auto r = m_cullBlockUpload.Initialize(
            device, std::max<uint64_t>(cullBlockBufferBytes, 256u),
            "MidMeshGpuExtractCullBlockUpload"); !r) {
        return Error("InitializeSmoke - cull-block upload: {}", r.error());
    }

    // STEP 1 PRODUCTION OUTPUT: fixed per-real-slot face arena + one count per slot.
    // This is not bound for drawing yet; the B1.3 A/B readback compares each dirty
    // tile's production-format range against CPU meshCacheFaces.
    m_productionFaceCapacityPerTile =
        std::max(1u, m_config.productionFaceCapacityPerTile);
    const uint64_t totalProductionFaces =
        static_cast<uint64_t>(m_config.maxTiles) *
        static_cast<uint64_t>(m_productionFaceCapacityPerTile);
    if (totalProductionFaces == 0u ||
        totalProductionFaces > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return Error("InitializeSmoke - production face element count exceeds view limit");
    }
    m_productionFaceBufferBytes =
        totalProductionFaces * sizeof(Simulation::SparseSurfaceFace);
    m_productionFaceCountBufferBytes =
        static_cast<uint64_t>(m_config.maxTiles) * sizeof(uint32_t);
    m_productionFaceStatusBufferBytes =
        static_cast<uint64_t>(m_config.maxTiles) * sizeof(uint32_t);
    if (auto r = m_productionFaceBuffer.Initialize(
            device, m_productionFaceBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(Simulation::SparseSurfaceFace),
            "MidMeshGpuExtractProductionFaces"); !r) {
        return Error("InitializeSmoke - production face buffer: {}", r.error());
    }
    if (!m_heapManager) {
        return Error("InitializeSmoke - descriptor heap manager is null");
    }
    if (auto r = m_productionFaceBuffer.CreateSRV(device, *m_heapManager); !r) {
        return Error("InitializeSmoke - production face SRV: {}", r.error());
    }
    if (auto r = m_productionFaceCountBuffer.Initialize(
            device, m_productionFaceCountBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(uint32_t),
            "MidMeshGpuExtractProductionFaceCounts"); !r) {
        return Error("InitializeSmoke - production count buffer: {}", r.error());
    }
    if (auto r = m_productionFaceStatusBuffer.Initialize(
            device, m_productionFaceStatusBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(uint32_t),
            "MidMeshGpuExtractProductionFaceStatuses"); !r) {
        return Error("InitializeSmoke - production status buffer: {}", r.error());
    }
    m_productionCommitBufferBytes =
        static_cast<uint64_t>(m_config.maxTiles) * sizeof(uint32_t);
    if (auto r = m_productionCommitBuffer.Initialize(
            device, m_productionCommitBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer,
            sizeof(uint32_t),
            "MidMeshGpuExtractProductionCommitFlags"); !r) {
        return Error("InitializeSmoke - production commit buffer: {}", r.error());
    }
    m_productionDrawArgsBufferBytes =
        static_cast<uint64_t>(m_config.maxTiles) * sizeof(Simulation::SparseSurfaceDrawArgs);
    if (auto r = m_productionDrawArgsBuffer.Initialize(
            device, m_productionDrawArgsBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer |
                BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument,
            sizeof(Simulation::SparseSurfaceDrawArgs),
            "MidMeshGpuExtractProductionDrawArgs"); !r) {
        return Error("InitializeSmoke - production draw args buffer: {}", r.error());
    }
    if (auto r = m_productionCountClearUpload.Initialize(
            device, 256u, "MidMeshGpuExtractProductionCountClear"); !r) {
        return Error("InitializeSmoke - production count clear upload: {}", r.error());
    }
    if (auto r = m_productionCommitUpload.Initialize(
            device,
            std::max<uint64_t>(m_productionCommitBufferBytes, 256u),
            "MidMeshGpuExtractProductionCommitUpload"); !r) {
        return Error("InitializeSmoke - production commit upload: {}", r.error());
    }
    if (auto* clear = static_cast<uint32_t*>(m_productionCountClearUpload.GetMappedData())) {
        clear[0] = 0u;
    }
    m_stats.productionFaceCapacityPerTile = m_productionFaceCapacityPerTile;
    m_stats.productionFaceBufferBytes = m_productionFaceBufferBytes;
    m_stats.productionFaceCountBufferBytes = m_productionFaceCountBufferBytes;
    m_stats.productionFaceStatusBufferBytes = m_productionFaceStatusBufferBytes;
    m_stats.productionDrawArgsBufferBytes = m_productionDrawArgsBufferBytes;
    m_stats.productionCommitBufferBytes = m_productionCommitBufferBytes;
    m_stats.productionDrawCommandCount = m_config.maxTiles;

    // B1.3.0 readback RING (debug-only verify path): kSmokeReadbackSlots slots, each
    // a face + metadata readback buffer Map()'d ONCE here and kept persistently mapped
    // (the CPU pointer is held for the process lifetime; never per-frame Map/Unmap).
    // A per-slot fence value (recorded at dispatch) tracks GPU-copy completion, so a
    // slot is read only after its fence is satisfied. This makes the verify REPEATABLE
    // across frames, not once per process.
    const uint32_t readbackFaceCapacity =
        std::max(m_debugFaceCapacityPerTile, m_productionFaceCapacityPerTile);
    const uint64_t faceReadbackBytes =
        static_cast<uint64_t>(readbackFaceCapacity) * sizeof(Simulation::SparseSurfaceFace);
    for (uint32_t i = 0; i < kSmokeReadbackSlots; ++i) {
        char faceName[64];
        char metaName[64];
        char countName[64];
        char statusName[64];
        std::snprintf(faceName, sizeof(faceName), "MidMeshGpuExtractSmokeFaceReadback%u", i);
        std::snprintf(metaName, sizeof(metaName), "MidMeshGpuExtractSmokeMetaReadback%u", i);
        std::snprintf(countName, sizeof(countName), "MidMeshGpuExtractProductionCountReadback%u", i);
        std::snprintf(statusName, sizeof(statusName), "MidMeshGpuExtractProductionStatusReadback%u", i);
        SmokeReadbackSlot& slot = m_smokeReadbackRing[i];
        if (auto r = slot.faceReadback.Initialize(
                device, faceReadbackBytes,
                BufferUsage::Readback, sizeof(Simulation::SparseSurfaceFace), faceName); !r) {
            return Error("InitializeSmoke - face readback ring[{}]: {}", i, r.error());
        }
        if (auto r = slot.metaReadback.Initialize(
                device, sizeof(MidMeshGpuExtractTileMeta),
                BufferUsage::Readback, sizeof(MidMeshGpuExtractTileMeta), metaName); !r) {
            return Error("InitializeSmoke - meta readback ring[{}]: {}", i, r.error());
        }
        if (auto r = slot.countReadback.Initialize(
                device, sizeof(uint32_t),
                BufferUsage::Readback, sizeof(uint32_t),
                countName); !r) {
            return Error("InitializeSmoke - count readback ring[{}]: {}", i, r.error());
        }
        if (auto r = slot.statusReadback.Initialize(
                device, sizeof(uint32_t),
                BufferUsage::Readback, sizeof(uint32_t),
                statusName); !r) {
            return Error("InitializeSmoke - status readback ring[{}]: {}", i, r.error());
        }
        // Persistent map: Map() once, hold the pointer. A readback heap is system
        // memory; the runtime makes a completed GPU copy visible to this pointer after
        // the slot's fence is satisfied, so we never re-Map per frame.
        slot.facePtr = slot.faceReadback.Map();
        slot.metaPtr = slot.metaReadback.Map();
        slot.countPtr = slot.countReadback.Map();
        slot.statusPtr = slot.statusReadback.Map();
        if (!slot.facePtr || !slot.metaPtr || !slot.countPtr || !slot.statusPtr) {
            return Error("InitializeSmoke - readback ring[{}] persistent map failed", i);
        }
        slot.fenceValue = 0;
        slot.pending = false;
        slot.tile = {};
    }
    m_smokeReadbackWriteSlot = 0;
    m_smokeReadbackReadSlot = 0;

    // Self-contained input stage: one controlled tile's samples + metadata, so the
    // smoke dispatch never depends on the frame command list's B1.1 EmitCopy ordering.
    const uint64_t smokeUploadBytes = m_sampleTileBytes + sizeof(MidMeshGpuExtractTileMeta);
    if (auto r = m_smokeInputUpload.Initialize(
            device, std::max<uint64_t>(smokeUploadBytes, 256u),
            "MidMeshGpuExtractSmokeInputUpload"); !r) {
        return Error("InitializeSmoke - input upload: {}", r.error());
    }

    // GPU timestamps: 3 slots (dispatch begin, dispatch end == barrier begin, barrier end).
    {
        D3D12_QUERY_HEAP_DESC qhDesc = {};
        qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhDesc.Count = 3u;
        qhDesc.NodeMask = 0;
        HRESULT qhr = device->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&m_smokeQueryHeap));
        if (FAILED(qhr)) {
            spdlog::warn("[GPU_EXTRACT_SMOKE] timestamp query heap create failed: 0x{:08X}; "
                "timing disabled", static_cast<unsigned int>(qhr));
            m_smokeQueryHeap.Reset();
        } else {
            if (auto r = m_smokeQueryReadback.Initialize(
                    device, 3u * sizeof(uint64_t), BufferUsage::Readback, sizeof(uint64_t),
                    "MidMeshGpuExtractSmokeTimestamps"); !r) {
                spdlog::warn("[GPU_EXTRACT_SMOKE] timestamp readback init failed: {}; timing disabled",
                    r.error());
                m_smokeQueryHeap.Reset();
            }
        }
    }

    // Compute PSO. Root layout MUST match CS_MidMeshExtractSmoke.hlsl:
    //   b0: 4x 32-bit constants {tileSlot, debugBaseFace, maxCells, faceCapacityPerTile}
    //   t0: samples (root SRV)   u0: debug faces (root UAV)   u1: metadata (root UAV)
    const std::filesystem::path csPath =
        shaderPath / "Compute" / "CS_MidMeshExtractSmoke.hlsl";
    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", false);
    if (!compileResult || !compileResult.value().IsValid()) {
        return Error("InitializeSmoke - CS compile failed: {}",
            compileResult ? compileResult.value().errors : compileResult.error());
    }
    ComputePipelineDesc desc;
    desc.computeShader = compileResult.value();
    desc.debugName = "CS_MidMeshExtractSmoke";
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 4 }); // b0
    desc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 }); // t0 samples
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 }); // u0 faces
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 1, 0, 1 }); // u1 metadata
    if (auto r = m_smokePipeline.Initialize(device, desc); !r) {
        return Error("InitializeSmoke - PSO init: {}", r.error());
    }

    // One reusable command allocator + list for the self-contained dispatch (creating
    // a fresh pair every frame exhausts the device). Reset per dispatch.
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_smokeCmdAllocator)))) {
        return Error("InitializeSmoke - command allocator create failed");
    }
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_smokeCmdAllocator.Get(), nullptr,
            IID_PPV_ARGS(&m_smokeCmdList)))) {
        return Error("InitializeSmoke - command list create failed");
    }
    m_smokeCmdList->Close(); // start closed; RunSmokeDispatch resets it each time

    // Dedicated queue + fence + event for the isolated smoke submit (never shares the
    // main render queue's fence ring).
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_smokeQueue)))) {
            return Error("InitializeSmoke - dedicated command queue create failed");
        }
        m_smokeQueue->SetName(L"MidMeshGpuExtractSmokeQueue");
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_smokeFence)))) {
            return Error("InitializeSmoke - dedicated fence create failed");
        }
        m_smokeFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_smokeFenceEvent) {
            return Error("InitializeSmoke - fence event create failed");
        }
        m_smokeFenceValue = 0;
    }

    // The dedicated smoke queue owns the timestamp clock for this path.
    if (m_smokeQueryHeap) {
        m_smokeQueue->GetTimestampFrequency(&m_smokeTimestampFrequency);
    }

    m_smokeReady = true;
    spdlog::info(
        "[GPU_EXTRACT_SMOKE] compute side ready: debugFaceBufMB={:.2f} faceCapacityPerTile={} "
        "productionFaceBufMB={:.2f} productionCapacityPerTile={} productionStatusKB={:.1f} "
        "maxCells={} readbackRingSlots={} readbackForceWait={} "
        "(ISOLATED - production face buffer + draw path untouched)",
        static_cast<double>(m_smokeFaceBufferBytes) / (1024.0 * 1024.0),
        m_smokeFaceCapacityPerTile,
        static_cast<double>(m_productionFaceBufferBytes) / (1024.0 * 1024.0),
        m_productionFaceCapacityPerTile,
        static_cast<double>(m_productionFaceStatusBufferBytes) / 1024.0,
        m_config.smokeMaxCells,
        kSmokeReadbackSlots, m_smokeReadbackForceWait ? 1 : 0);

    // AB_SELFTEST: prove the multiset/multiplicity/containment harness actually bites
    // (catches duplicates + missing faces) BEFORE any real GPU comparison relies on it.
    // Runs once, here, independent of the GPU - a unit-style check of the harness.
    m_smokeStats.abSelfTestPassed = RunMidMeshFaceAbSelfTest();
    m_smokeStats.abSelfTestRun = true;
    m_smokeSelfTestDone = true;
    if (!m_smokeStats.abSelfTestPassed) {
        spdlog::error(
            "[GPU_EXTRACT_SMOKE] AB_SELFTEST FAILED - the multiset compare does not "
            "detect duplicates/missing; AB_VERIFY results are NOT trustworthy");
    }
    return {};
}

void MidMeshGpuExtractResources::ComputeSmokeFacesCpu(
    const SmokeControlledTile& tile,
    std::vector<Simulation::SparseSurfaceFace>& outFaces)
{
    outFaces.clear();
    if (!tile.valid || tile.sampleSide < 2u) {
        return;
    }
    const uint32_t side = tile.sampleSide;
    const uint32_t stride = side;
    const uint32_t cellsPerRow = side - 1u;
    const uint32_t cellCount = cellsPerRow * cellsPerRow;
    const uint32_t kCells = std::min(tile.expectedCount, cellCount);
    const int32_t cellSizeInt = std::max(1, tile.cellSizeInt);
    outFaces.reserve(kCells);
    for (uint32_t cell = 0; cell < kCells; ++cell) {
        const uint32_t cx = cell % cellsPerRow;
        const uint32_t cz = cell / cellsPerRow;
        const uint32_t sampleIndex = cz * stride + cx;
        const uint32_t sample =
            (sampleIndex < tile.samples.size()) ? tile.samples[sampleIndex] : 0u;
        const int32_t unpackedY =
            static_cast<int32_t>(sample & 0xFFFFu) -
            static_cast<int32_t>(MidMeshSampleHeightBias);
        Simulation::SparseSurfaceFace face;
        face.worldX = tile.originX + static_cast<int32_t>(cx) * cellSizeInt;
        face.worldY = unpackedY;
        face.worldZ = tile.originZ + static_cast<int32_t>(cz) * cellSizeInt;
        face.payload = Simulation::PackSparseSurfacePayload(
            3u, cell & Simulation::kSparseSurfaceVoxelPayloadMask, 1u, 1u);
        outFaces.push_back(face);
    }
}

void MidMeshGpuExtractResources::ComputeB13cSkirtFacesCpu(
    const SmokeControlledTile& tile,
    uint32_t terraceStep,
    int32_t seaLevelY,
    std::vector<Simulation::SparseSurfaceFace>& outSkirtFaces)
{
    outSkirtFaces.clear();
    if (!tile.valid || tile.sampleSide < 2u) {
        return;
    }
    const uint32_t side = tile.sampleSide;
    const uint32_t stride = side;
    const uint32_t cellsPerRow = side - 1u;
    if (cellsPerRow == 0u || tile.samples.size() < static_cast<size_t>(side) * side) {
        return;
    }
    const int32_t cellSizeInt = std::max(1, tile.cellSizeInt);
    const int32_t step = std::max(1, static_cast<int32_t>(terraceStep));
    const uint32_t splitLimit = 32u; // kSparseExtentMask + 1 (must match the shader addFace)

    // Exact mirrors of the shared HLSLI helpers (a one-bit drift would miscount skirts).
    auto decodeMaterial = [](uint32_t s) -> uint32_t { return (s >> 16u) & 0xFFu; };
    auto decodeY = [](uint32_t s) -> int32_t {
        return static_cast<int32_t>(s & 0xFFFFu) -
            static_cast<int32_t>(MidMeshSampleHeightBias);
    };
    auto isSolid = [](uint32_t m) -> bool { return m != 0u /*Air*/ && m != 2u /*Water*/; };
    auto floorDiv = [](int32_t a, int32_t b) -> int32_t {
        int32_t q = a / b;
        int32_t r = a - q * b;
        if (r != 0 && ((r < 0) != (b < 0))) { q -= 1; }
        return q;
    };
    auto quantY = [&](int32_t y) -> int32_t { return floorDiv(y, step) * step; };
    auto isWater = [](uint32_t m) -> bool { return m == 2u; };
    const bool emitWater = tile.emitWater;
    // B1.3f-a/b: SKIRT-ONLY block aggregation mirror of the shader MidMeshAggregateBlock.
    // Skirts emit on SOLID blocks only (`!block.water`), so this returns `present` ONLY for a
    // block whose aggregated result is SOLID (a water-only / all-air block emits no skirt and
    // is reported !present here). It must therefore match the SOLID-block height/material the
    // shader would use, INCLUDING the shoal min-height override when water is on (a mixed
    // water+solid block is solid but its top sits at the LOWEST solid sample, not the MAX).
    auto aggregateBlock = [&](uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1,
                              bool& present, int32_t& outHeight, uint32_t& outMaterial) {
        present = false; outHeight = 0; outMaterial = 0u;
        x0 = std::min(x0, side - 1u);
        z0 = std::min(z0, side - 1u);
        x1 = std::max(x0 + 1u, std::min(x1, side));
        z1 = std::max(z0 + 1u, std::min(z1, side));
        bool solid = false;
        uint32_t waterCount = 0u;
        int32_t minSolidHeight = 0; uint32_t minSolidMaterial = 0u; bool haveMinSolid = false;
        for (uint32_t zz = z0; zz < z1; ++zz) {
            for (uint32_t xx = x0; xx < x1; ++xx) {
                const uint32_t ss = tile.samples[zz * stride + xx];
                const uint32_t mm = decodeMaterial(ss);
                if (!emitWater && isWater(mm)) { continue; }
                const bool sSolid = isSolid(mm);
                const bool sWater = isWater(mm);
                if (!sSolid && !sWater) { continue; }
                const int32_t hh = quantY(decodeY(ss));
                if (sSolid) {
                    if (!haveMinSolid || hh < minSolidHeight) {
                        minSolidHeight = hh; minSolidMaterial = mm; haveMinSolid = true;
                    }
                    if (!present || !solid || hh >= outHeight) {
                        present = true; solid = true; outHeight = hh; outMaterial = mm;
                    }
                } else {
                    ++waterCount;
                    if (!present) { present = true; /* water block; solid stays false */
                        outHeight = hh; outMaterial = mm; }
                }
            }
        }
        // Shoal override (mixed water+solid -> hug the lowest solid).
        if (present && solid && waterCount > 0u && haveMinSolid) {
            outHeight = minSolidHeight; outMaterial = minSolidMaterial;
        }
        // Skirts are SOLID-only: report !present for a water-only block so no skirt is counted.
        if (present && !solid) { present = false; outHeight = 0; outMaterial = 0u; }
    };
    // FNV-1a over (x,y,z,0x9E3779B9); low byte = variant. Mirrors MidMeshPackVoxel.
    auto packVoxel = [](uint32_t material, int32_t x, int32_t y, int32_t z) -> uint32_t {
        uint32_t h = 2166136261u;
        h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
        h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
        h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
        h = (h ^ 0x9E3779B9u) * 16777619u;
        const uint32_t variant = h & 0xFFu;
        return (material & 0xFFu) | (variant << 8u) | (0x10u << 24u);
    };

    // addFace split-chunked emission (mirrors MidMeshAddFace / the CPU addFace) for ONE
    // riser-direction quad. direction is NegX/PosX/NegZ/PosZ here (skirts are never PosY).
    auto addFace = [&](uint32_t direction, int32_t baseX, int32_t baseY, int32_t baseZ,
                       uint32_t width, uint32_t height, uint32_t voxel) {
        if (width == 0u || height == 0u) { return; }
        for (uint32_t hOff = 0u; hOff < height; hOff += splitLimit) {
            const uint32_t hChunk = std::min(splitLimit, height - hOff);
            for (uint32_t wOff = 0u; wOff < width; wOff += splitLimit) {
                const uint32_t wChunk = std::min(splitLimit, width - wOff);
                Simulation::SparseSurfaceFace face;
                face.worldX = baseX;
                face.worldY = baseY;
                face.worldZ = baseZ;
                const auto NegX = static_cast<uint32_t>(Simulation::SparseFaceDirection::NegX);
                const auto PosX = static_cast<uint32_t>(Simulation::SparseFaceDirection::PosX);
                const auto NegZ = static_cast<uint32_t>(Simulation::SparseFaceDirection::NegZ);
                if (direction == NegX || direction == PosX) {
                    face.worldY = baseY + static_cast<int32_t>(hOff);
                    face.worldZ = baseZ + static_cast<int32_t>(wOff);
                } else { // NegZ / PosZ (skirts are never PosY here)
                    (void)NegZ;
                    face.worldX = baseX + static_cast<int32_t>(wOff);
                    face.worldY = baseY + static_cast<int32_t>(hOff);
                }
                face.payload =
                    Simulation::PackSparseSurfacePayload(direction, voxel, wChunk, hChunk);
                outSkirtFaces.push_back(face);
            }
        }
    };
    // addRiser -1 inward shift for PosX/PosZ (mirrors MidMeshAddRiser / the CPU addRiser).
    auto addRiser = [&](uint32_t direction, int32_t boundaryX, int32_t boundaryZ,
                        int32_t lowTopY, uint32_t span, uint32_t height, uint32_t voxel) {
        if (height == 0u) { return; }
        const auto PosX = static_cast<uint32_t>(Simulation::SparseFaceDirection::PosX);
        const auto PosZ = static_cast<uint32_t>(Simulation::SparseFaceDirection::PosZ);
        int32_t faceX = boundaryX;
        int32_t faceZ = boundaryZ;
        if (direction == PosX) { faceX = boundaryX - 1; }
        else if (direction == PosZ) { faceZ = boundaryZ - 1; }
        addFace(direction, faceX, lowTopY, faceZ, span, height, voxel);
    };

    const auto NegX = static_cast<uint32_t>(Simulation::SparseFaceDirection::NegX);
    const auto PosX = static_cast<uint32_t>(Simulation::SparseFaceDirection::PosX);
    const auto NegZ = static_cast<uint32_t>(Simulation::SparseFaceDirection::NegZ);
    const auto PosZ = static_cast<uint32_t>(Simulation::SparseFaceDirection::PosZ);

    // B1.3f-a: iterate BLOCKS (mergeCells>=1), exactly like the shader. mergeCells==1 -> one
    // block per cell (byte-identical to the prior B1.3c skirt mirror). cellsPerRow here is the
    // CPU's PER-AXIS cellCount (== side-1). Border predicates use the BLOCK edge.
    const uint32_t mergeCells = std::max(1u, tile.mergeCells);
    const uint32_t blockCountPerAxis = (cellsPerRow + mergeCells - 1u) / mergeCells;
    for (uint32_t bz = 0; bz < blockCountPerAxis; ++bz) {
        const uint32_t z = bz * mergeCells;
        const uint32_t zEnd = std::min(cellsPerRow, z + mergeCells);
        const bool atNegZ = (z == 0u);
        const bool atPosZ = (zEnd >= cellsPerRow);
        for (uint32_t bx = 0; bx < blockCountPerAxis; ++bx) {
            const uint32_t x = bx * mergeCells;
            const uint32_t xEnd = std::min(cellsPerRow, x + mergeCells);
            const bool atNegX = (x == 0u);
            const bool atPosX = (xEnd >= cellsPerRow);
            if (!atNegX && !atPosX && !atNegZ && !atPosZ) {
                continue; // interior block: no skirt
            }
            bool present = false; int32_t height = 0; uint32_t material = 0u;
            aggregateBlock(x, z, xEnd, zEnd, present, height, material);
            if (!present) {
                continue; // SOLID-only (mirrors !block.water on the GPU's solid-only path)
            }
            const int32_t worldX = tile.originX + static_cast<int32_t>(x) * cellSizeInt;
            const int32_t worldZ = tile.originZ + static_cast<int32_t>(z) * cellSizeInt;
            const uint32_t width = (xEnd - x) * static_cast<uint32_t>(cellSizeInt);
            const uint32_t depth = (zEnd - z) * static_cast<uint32_t>(cellSizeInt);
            const uint32_t voxel = packVoxel(material, worldX, height, worldZ);

            const uint32_t skirtDepth = std::max(8u, terraceStep * 6u);
            int32_t skirtLowTopY = height + 1 - static_cast<int32_t>(skirtDepth);
            if (height > seaLevelY && skirtLowTopY > seaLevelY - 2) {
                skirtLowTopY = seaLevelY - 2;
            }
            const uint32_t skirtHeight =
                static_cast<uint32_t>(std::max(1, height + 1 - skirtLowTopY));

            if (atNegX) { addRiser(NegX, worldX, worldZ, skirtLowTopY, depth, skirtHeight, voxel); }
            if (atPosX) {
                addRiser(PosX, worldX + static_cast<int32_t>(width), worldZ,
                         skirtLowTopY, depth, skirtHeight, voxel);
            }
            if (atNegZ) { addRiser(NegZ, worldX, worldZ, skirtLowTopY, width, skirtHeight, voxel); }
            if (atPosZ) {
                addRiser(PosZ, worldX, worldZ + static_cast<int32_t>(depth),
                         skirtLowTopY, width, skirtHeight, voxel);
            }
        }
    }
}

bool MidMeshGpuExtractResources::RunSmokeDispatch(
    ID3D12Device* device,
    DX12CommandQueue& commandQueue,
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    uint64_t currentTileVersionForSlot)
{
    if (!m_smokeReady || !device) {
        return false;
    }

    // Pick the FIRST controlled tile: a dirty tile with a resident slot + samples.
    const Simulation::MidMeshGpuExtractDirtyTile* picked = nullptr;
    for (const auto& t : dirtyTiles) {
        if (t.slot < m_config.maxTiles && t.samples != nullptr && t.sampleCount > 0u) {
            picked = &t;
            break;
        }
    }
    if (!picked) {
        return false;
    }

    // Capture the controlled tile so the delayed readback can recompute its faces.
    SmokeControlledTile ctl;
    ctl.valid = true;
    ctl.slot = picked->slot;
    ctl.baseFace = picked->slot * m_smokeFaceCapacityPerTile;
    ctl.version = picked->meshContentVersion;
    ctl.originX = picked->originX;
    ctl.originZ = picked->originZ;
    ctl.cellSizeInt = std::max(1, static_cast<int32_t>(picked->cellSize));
    ctl.sampleSide = m_config.tileSampleSide;
    const uint32_t cellsPerRow =
        (m_config.tileSampleSide > 1u) ? (m_config.tileSampleSide - 1u) : 0u;
    const uint32_t cellCount = cellsPerRow * cellsPerRow;
    ctl.expectedCount = std::min(m_config.smokeMaxCells, cellCount);
    ctl.samples.assign(picked->samples, picked->samples + picked->sampleCount);

    // Reuse the persistent command allocator + list (created once in InitializeSmoke).
    // Resetting the allocator requires the PREVIOUS dispatch (which recorded into it) to
    // be GPU-complete. Wait that prior fence value - by the next frame it is essentially
    // always already signaled, so this is non-blocking in steady state and is NOT a wait
    // on the CURRENT dispatch's result (the ring + delayed poll handle that). This keeps
    // the readback REPEATABLE without a per-frame blocking wait on fresh GPU output.
    if (!m_smokeCmdAllocator || !m_smokeCmdList) {
        spdlog::error("[GPU_EXTRACT_SMOKE] command objects missing");
        return false;
    }
    const uint64_t prevFence = m_smokeFenceValue;
    if (prevFence != 0u && m_smokeFence->GetCompletedValue() < prevFence) {
        m_smokeFence->SetEventOnCompletion(prevFence, m_smokeFenceEvent);
        WaitForSingleObject(m_smokeFenceEvent, INFINITE);
    }
    if (FAILED(m_smokeCmdAllocator->Reset()) ||
        FAILED(m_smokeCmdList->Reset(m_smokeCmdAllocator.Get(), nullptr))) {
        spdlog::error("[GPU_EXTRACT_SMOKE] command list reset failed");
        return false;
    }
    ID3D12GraphicsCommandList* const list = m_smokeCmdList.Get();

    const bool haveTimestamps = (m_smokeQueryHeap != nullptr);
    // B1.3.0: the readback copy is recorded EVERY dispatch into the next ring slot
    // (persistently mapped, fence-tracked), so the verify is repeatable - not once.
    const uint32_t writeSlot = m_smokeReadbackWriteSlot;
    SmokeReadbackSlot& ringSlot = m_smokeReadbackRing[writeSlot];
    const bool recordReadback = true;

    // The dedicated smoke input buffers always use slot 0 / baseFace 0.
    ctl.baseFace = 0u;
    ctl.slot = 0u;

    // ---- self-contained input upload into the DEDICATED smoke buffers ----
    // Stage the controlled tile's samples + metadata, then copy into the smoke-owned
    // sample/metadata buffers on THIS command list. These buffers are touched ONLY by
    // the smoke list, so there is no cross-list DX12 state-tracking hazard with the
    // frame command list's B1.1 EmitCopy.
    if (auto* mapped = static_cast<uint8_t*>(m_smokeInputUpload.GetMappedData())) {
        const uint64_t tileSampleBytes =
            static_cast<uint64_t>(picked->sampleCount) * sizeof(uint32_t);
        std::memcpy(mapped, picked->samples, static_cast<size_t>(tileSampleBytes));

        MidMeshGpuExtractTileMeta meta;
        meta.coordX = picked->coord.x;
        meta.coordRing = picked->coord.y;
        meta.coordZ = picked->coord.z;
        meta.originX = picked->originX;
        meta.originZ = picked->originZ;
        std::memcpy(&meta.cellSizeBits, &picked->cellSize, sizeof(uint32_t));
        meta.mergeCells = picked->mergeCells;
        meta.childMask = picked->childMask;
        meta.meshContentVersionLo =
            static_cast<uint32_t>(picked->meshContentVersion & 0xFFFFFFFFull);
        meta.meshContentVersionHi =
            static_cast<uint32_t>(picked->meshContentVersion >> 32u);
        meta.sampleSide = m_config.tileSampleSide;
        meta.sampleStride = m_config.tileSampleSide;
        meta.haloWidth = 0u;
        meta.heightPackDesc = PackMidMeshHeightDesc();
        meta.heightBias = MidMeshSampleHeightBias;
        meta.editFootprintCount = 0u;
        // Production range fields are unused by the smoke CS (it uses its own root
        // constants for the ISOLATED debug buffer range). faceCount/status are the
        // fields the smoke CS write-backs overwrite.
        meta.faceCapacity = m_smokeFaceCapacityPerTile;
        meta.baseFace = 0u;
        meta.faceCount = 0u;
        meta.statusOverflow = 0u;
        std::memcpy(mapped + tileSampleBytes, &meta, sizeof(meta));

        ID3D12Resource* uploadRes = m_smokeInputUpload.GetResource();
        m_smokeSampleBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(
            m_smokeSampleBuffer.GetResource(), 0, uploadRes, 0, tileSampleBytes);
        list->CopyBufferRegion(
            m_smokeMetaBuffer.GetResource(), 0,
            uploadRes, tileSampleBytes, sizeof(MidMeshGpuExtractTileMeta));
        m_smokeSampleBuffer.TransitionTo(
            list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Debug face buffer -> UAV (metadata already UAV from the upload-back transition).
    m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_smokePipeline.Bind(list);
    const uint32_t consts[4] = {
        ctl.slot, ctl.baseFace, m_config.smokeMaxCells, m_smokeFaceCapacityPerTile
    };
    m_smokePipeline.SetRoot32BitConstants(list, 0, 4, consts);
    list->SetComputeRootShaderResourceView(1, m_smokeSampleBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, m_smokeFaceBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, m_smokeMetaBuffer.GetGPUVirtualAddress());

    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); // dispatch begin
    }
    // One controlled tile: enough thread groups for smokeMaxCells cells (64/group).
    const uint32_t groups = (m_config.smokeMaxCells + 63u) / 64u;
    m_smokePipeline.Dispatch(list, std::max(1u, groups), 1, 1);
    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); // dispatch end
    }

    // UAV barrier so the face/metadata writes are visible before the readback copy.
    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = m_smokeFaceBuffer.GetResource();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = m_smokeMetaBuffer.GetResource();
    list->ResourceBarrier(2, uavBarriers);
    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2); // barrier end
    }

    // B1.3.0: copy this dispatch's debug faces + metadata into the CURRENT RING SLOT's
    // persistently-mapped readback buffers as part of THIS submission. Recorded EVERY
    // dispatch (not once per process), so a later PollSmokeReadback reads a fresh result
    // each time the ring cycles - proving repeatable readback. The slot's fence value
    // (recorded below) tells the poll when this copy is GPU-complete.
    if (recordReadback) {
        m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(
            ringSlot.faceReadback.GetResource(), 0,
            m_smokeFaceBuffer.GetResource(), 0,
            static_cast<uint64_t>(m_smokeFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace));
        list->CopyBufferRegion(
            ringSlot.metaReadback.GetResource(), 0,
            m_smokeMetaBuffer.GetResource(), 0,
            sizeof(MidMeshGpuExtractTileMeta));
    }
    // Timestamp resolve runs EVERY frame (required per-frame GPU timing).
    if (haveTimestamps && m_smokeQueryReadback.GetResource()) {
        list->ResolveQueryData(
            m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3,
            m_smokeQueryReadback.GetResource(), 0);
    }

    // Restore the smoke buffers to COMMON for the next dispatch.
    m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeSampleBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);

    // CPU submit cost = the Close + Execute + Signal span (NOT the GPU-completion wait).
    // Submit on the DEDICATED smoke queue + fence so the main render queue's per-frame
    // fence ring is never perturbed.
    const auto cpuSubmitStart = std::chrono::steady_clock::now();
    list->Close();
    ID3D12CommandList* lists[] = { list };
    m_smokeQueue->ExecuteCommandLists(1, lists);
    const uint64_t fence = ++m_smokeFenceValue;
    m_smokeQueue->Signal(m_smokeFence.Get(), fence);
    const auto cpuSubmitStop = std::chrono::steady_clock::now();

    ctl.fenceValue = fence;
    (void)commandQueue; // main render queue intentionally untouched by the smoke path

    // ---- record this dispatch into the ring slot (NO blocking wait on its fence) ----
    // The slot's fence value tells PollSmokeReadback when the copy is GPU-complete; the
    // poll reads it a frame or two later, by which point the fence is naturally done.
    // This is the change that makes the readback REPEATABLE without a per-frame stall.
    ringSlot.fenceValue = fence;
    ringSlot.pending = true;
    ringSlot.tile = ctl;
    m_smokeReadbackWriteSlot = (writeSlot + 1u) % kSmokeReadbackSlots;

    // ---- commit gate (validate; nothing is drawn) ----
    // serial still matches what we dispatched -> usable; else discard-stale. (The GPU
    // completion half of the gate is proven by the slot fence at poll time, not a wait.)
    const bool stale = (currentTileVersionForSlot != ctl.version);

    // Resolve timestamps NON-BLOCKINGLY: read the per-query readback only if a prior
    // dispatch's fence is already complete (the prevFence wait above guarantees the
    // previous resolve is visible). We never block on THIS dispatch for timing.
    double dispatchUs = 0.0, barrierUs = 0.0;
    const bool timingReady =
        (m_smokeFence->GetCompletedValue() >= fence) ||
        (prevFence != 0u && m_smokeFence->GetCompletedValue() >= prevFence);
    if (haveTimestamps && m_smokeTimestampFrequency != 0u && timingReady) {
        if (const auto* ts = static_cast<const uint64_t*>(m_smokeQueryReadback.Map())) {
            const double toUs = 1.0e6 / static_cast<double>(m_smokeTimestampFrequency);
            dispatchUs = (ts[1] >= ts[0]) ? static_cast<double>(ts[1] - ts[0]) * toUs : 0.0;
            barrierUs = (ts[2] >= ts[1]) ? static_cast<double>(ts[2] - ts[1]) * toUs : 0.0;
            m_smokeQueryReadback.Unmap();
        }
    }
    const double cpuSubmitUs =
        std::chrono::duration<double, std::micro>(cpuSubmitStop - cpuSubmitStart).count();

    // Record stats. Preserve the CUMULATIVE / persistent verify fields (the ring poll
    // owns them) across this per-dispatch refresh, so AB_VERIFY repeatability + the
    // self-test result are not wiped every frame.
    const uint32_t keepVerifyCount = m_smokeStats.verifyCount;
    const bool keepVerified = m_smokeStats.verified;
    const bool keepSelfTestRun = m_smokeStats.abSelfTestRun;
    const bool keepSelfTestPassed = m_smokeStats.abSelfTestPassed;
    m_smokeStats = {};
    m_smokeStats.dispatched = true;
    m_smokeStats.tileSlot = ctl.slot;
    m_smokeStats.controlledTiles = 1u;
    m_smokeStats.expectedFaceCount = ctl.expectedCount;
    m_smokeStats.gpuFaceCount = UINT32_MAX;   // unread until PollSmokeReadback
    m_smokeStats.gpuStatusOverflow = 0u;
    m_smokeStats.dispatchedVersion = ctl.version;
    m_smokeStats.commitGatePassed = !stale;   // serial check (fence proven at poll time)
    m_smokeStats.commitGateStale = stale;
    m_smokeStats.dispatchGpuUs = dispatchUs;
    m_smokeStats.barrierGpuUs = barrierUs;
    m_smokeStats.cpuSubmitUs = cpuSubmitUs;
    m_smokeStats.verified = keepVerified;
    m_smokeStats.verifyMatched = 0;
    m_smokeStats.verifyMismatch = 0;
    m_smokeStats.verifyCount = keepVerifyCount;
    m_smokeStats.abSelfTestRun = keepSelfTestRun;
    m_smokeStats.abSelfTestPassed = keepSelfTestPassed;

    m_smokeControlled = ctl;

    spdlog::info(
        "GPU_EXTRACT_SMOKE slot={} controlledTiles={} expectedFaces={} dispatchGpuUs={:.2f} "
        "barrierGpuUs={:.2f} cpuSubmitUs={:.2f} commitGate={} dispatchedVersion={} currentVersion={}",
        m_smokeStats.tileSlot, m_smokeStats.controlledTiles, m_smokeStats.expectedFaceCount,
        m_smokeStats.dispatchGpuUs, m_smokeStats.barrierGpuUs, m_smokeStats.cpuSubmitUs,
        stale ? "DISCARD_STALE" : "PASS",
        m_smokeStats.dispatchedVersion, currentTileVersionForSlot);
    return true;
}

bool MidMeshGpuExtractResources::PollSmokeReadback(uint64_t completedFenceValue) {
    (void)completedFenceValue; // smoke uses its own dedicated fence (the ring tracks it)
    if (!m_smokeReady) {
        return false;
    }

    // B1.3.0: consume the OLDEST pending ring slot whose copy fence has completed (FIFO).
    // The poll runs a couple frames behind the dispatch, so the slot's fence is normally
    // already satisfied with NO wait. The gated explicit wait (m_smokeReadbackForceWait,
    // OFF for timing) is the only blocking path; it forces a same-cycle compare in the
    // isolated correctness path. This is what makes the verify REPEATABLE - one compare
    // per cycled slot, not once per process.
    SmokeReadbackSlot& slot = m_smokeReadbackRing[m_smokeReadbackReadSlot];
    if (!slot.pending || !slot.tile.valid) {
        return false;
    }
    const uint64_t completed = m_smokeFence->GetCompletedValue();
    if (completed < slot.fenceValue) {
        if (!m_smokeReadbackForceWait) {
            return false; // not done yet; try again next poll (non-blocking)
        }
        // GATED correctness path ONLY (never on for timing): block until the slot's
        // fence completes so the persistent readback pointer holds this dispatch's data.
        if (m_smokeFenceEvent) {
            m_smokeFence->SetEventOnCompletion(slot.fenceValue, m_smokeFenceEvent);
            WaitForSingleObject(m_smokeFenceEvent, INFINITE);
        }
    }

    // Fence satisfied: the persistently-mapped pointers now hold this dispatch's copy.
    uint32_t gpuFaceCount = 0;
    uint32_t gpuStatus = 0;
    if (slot.metaPtr) {
        const auto* meta = static_cast<const MidMeshGpuExtractTileMeta*>(slot.metaPtr);
        gpuFaceCount = meta->faceCount;
        gpuStatus = meta->statusOverflow;
    }

    // Recompute the deterministic CPU reference for THIS slot's dispatch (its captured
    // tile snapshot - the same `ComputeSmokeFacesCpu` mirror), then gather the GPU faces
    // the shader actually wrote (the first gpuFaceCount, clamped to capacity).
    std::vector<Simulation::SparseSurfaceFace> cpuFaces;
    ComputeSmokeFacesCpu(slot.tile, cpuFaces);

    std::vector<Simulation::SparseSurfaceFace> gpuFaces;
    if (slot.facePtr) {
        const auto* faces = static_cast<const Simulation::SparseSurfaceFace*>(slot.facePtr);
        const uint32_t emitted =
            (gpuStatus == 0u) ? std::min(gpuFaceCount, m_smokeFaceCapacityPerTile) : 0u;
        gpuFaces.assign(faces, faces + emitted);
    }

    // Multiset A/B: the smoke is deterministic and complete, so use full EQUALITY
    // (later subset-convergence increments will pass Containment instead). The harness
    // canonicalizes by sorting raw 16-byte payloads, keeps duplicates, and reports the
    // multiset diff - so a duplicated GPU face would FAIL even though counts "look" ok.
    const MidMeshFaceAbResult ab = CompareMidMeshFacesMultiset(
        gpuFaces, cpuFaces, MidMeshFaceAbMode::Equal, gpuStatus, 4u);
    LogMidMeshFaceAbResult("smoke", ab);

    // Mark the slot consumed and advance the FIFO read cursor.
    slot.pending = false;
    slot.tile = {};
    m_smokeReadbackReadSlot = (m_smokeReadbackReadSlot + 1u) % kSmokeReadbackSlots;

    ++m_smokeAbVerifyCount;
    m_smokeStats.gpuFaceCount = gpuFaceCount;
    m_smokeStats.gpuStatusOverflow = gpuStatus;
    m_smokeStats.verified = true;
    m_smokeStats.verifyMatched = ab.match ? 1u : 0u;
    m_smokeStats.verifyMismatch = ab.match ? 0u : 1u;
    m_smokeStats.verifyCount = m_smokeAbVerifyCount;
    m_smokeStats.abGpuFaceCount = ab.gpuFaceCount;
    m_smokeStats.abGpuUniqueFaceCount = ab.gpuUniqueFaceCount;
    m_smokeStats.abMissingCpuFaces = ab.missingCpuFaces;
    m_smokeStats.abExtraGpuFaces = ab.extraGpuFaces;
    m_smokeStats.abMultiplicityMismatches = ab.multiplicityMismatches;
    m_smokeStats.abOverflow = ab.overflow;

    // Keep the legacy SMOKE_VERIFY line too (count/expected) for continuity with B1.2.
    spdlog::info(
        "SMOKE_VERIFY abVerifyCount={} match={} gpuCount={} cpuCount={} gpuStatus={}",
        m_smokeAbVerifyCount, ab.match ? 1 : 0, gpuFaceCount,
        static_cast<uint32_t>(cpuFaces.size()), gpuStatus);
    return true;
}

// =============================================================================
// B1.3a TOP-FACE - the first real meshing increment (containment-proven).
// =============================================================================

Result<void> MidMeshGpuExtractResources::InitializeB13aTopFace(
    ID3D12Device* device,
    ShaderCompiler& shaderCompiler,
    const std::filesystem::path& shaderPath)
{
    if (!m_smokeReady) {
        return Error("InitializeB13aTopFace - smoke compute side not ready");
    }
    if (!device) {
        return Error("InitializeB13aTopFace - device is null");
    }
    // The isolated debug face buffer + readback ring must have been sized for the
    // top-face capacity (InitializeSmoke called with b13aEnabled=true).
    if (m_debugFaceCapacityPerTile < m_topFaceCapacityPerTile) {
        return Error(
            "InitializeB13aTopFace - debug face buffer capacity {} < topFaceCapacityPerTile {} "
            "(InitializeSmoke must be called with b13aEnabled=true)",
            m_debugFaceCapacityPerTile, m_topFaceCapacityPerTile);
    }

    // Top-face PSO. Root layout MUST match CS_MidMeshExtractTopFaces.hlsl:
    //   b0: 11x 32-bit constants {tileSlot, debugBaseFace, faceCapacityPerTile,
    //       terraceStep, emitRisers, emitSkirts, seaLevelY, applyChildSuppression,
    //       applyEditSkip, editBoxBase, editBoxCount}
    //       (emitRisers: 0 = B1.3a top only, 1 = B1.3b + risers;
    //        emitSkirts: 0 = no border skirts, 1 = B1.3c + tile-border skirts;
    //        seaLevelY: SEA_LEVEL_Y, passed so the GPU never hardcodes it;
    //        applyChildSuppression: 0 = B1.3a-c (no suppression), 1 = B1.3d child-quadrant
    //        suppression - a cell whose quadrant has a resident finer child emits nothing;
    //        applyEditSkip: 0 = B1.3a-d, 1 = B1.3e edit-footprint skip - a cell inside the
    //        tile's edit footprint emits nothing; editBoxBase/Count index this tile's boxes)
    //   t0: samples (root SRV)   u0: debug faces (root UAV)   u1: metadata (root UAV)
    //   t1: edit boxes (root SRV; B1.3e)
    const std::filesystem::path csPath =
        shaderPath / "Compute" / "CS_MidMeshExtractTopFaces.hlsl";
    auto compileResult = shaderCompiler.CompileComputeShader(csPath, L"main", false);
    if (!compileResult || !compileResult.value().IsValid()) {
        return Error("InitializeB13aTopFace - CS compile failed: {}",
            compileResult ? compileResult.value().errors : compileResult.error());
    }
    ComputePipelineDesc desc;
    desc.computeShader = compileResult.value();
    desc.debugName = "CS_MidMeshExtractTopFaces";
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 15 }); // b0 (STEP 1: +gOutputSlot)
    desc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 }); // t0 samples
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 }); // u0 faces
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 1, 0, 1 }); // u1 metadata
    desc.rootParams.push_back({ RootParamType::ShaderResource, 1, 0, 1 }); // t1 edit boxes
    desc.rootParams.push_back({ RootParamType::ShaderResource, 2, 0, 1 }); // t2 cull-block mask (B1.3f-c)
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 2, 0, 1 }); // u2 production counts
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 3, 0, 1 }); // u3 production statuses
    if (auto r = m_topFacePipeline.Initialize(device, desc); !r) {
        return Error("InitializeB13aTopFace - PSO init: {}", r.error());
    }

    const std::filesystem::path drawArgsCsPath =
        shaderPath / "Compute" / "CS_MidMeshBuildProductionDrawArgs.hlsl";
    auto drawArgsCompile = shaderCompiler.CompileComputeShader(drawArgsCsPath, L"main", false);
    if (!drawArgsCompile || !drawArgsCompile.value().IsValid()) {
        return Error("InitializeB13aTopFace - production draw-args CS compile failed: {}",
            drawArgsCompile ? drawArgsCompile.value().errors : drawArgsCompile.error());
    }
    m_productionDrawArgsShader = drawArgsCompile.value();
    ComputePipelineDesc drawArgsDesc;
    drawArgsDesc.computeShader = m_productionDrawArgsShader;
    drawArgsDesc.debugName = "CS_MidMeshBuildProductionDrawArgs";
    drawArgsDesc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 4 }); // b0
    drawArgsDesc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 }); // t0 counts
    drawArgsDesc.rootParams.push_back({ RootParamType::ShaderResource, 1, 0, 1 }); // t1 statuses
    drawArgsDesc.rootParams.push_back({ RootParamType::ShaderResource, 2, 0, 1 }); // t2 commit flags
    drawArgsDesc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 }); // u0 draw args
    if (auto r = m_productionDrawArgsPipeline.Initialize(device, drawArgsDesc); !r) {
        return Error("InitializeB13aTopFace - production draw-args PSO init: {}", r.error());
    }

    m_b13aReady = true;
    spdlog::info(
        "[GPU_EXTRACT_B13A] top-face compute ready: topFaceCapacityPerTile={} "
        "debugFaceCapacityPerTile={} (ISOLATED - production face buffer + draw path untouched, "
        "containment A/B vs CPU meshCacheFaces)",
        m_topFaceCapacityPerTile, m_debugFaceCapacityPerTile);
    return {};
}

uint32_t MidMeshGpuExtractResources::SelectB13aFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint)
{
    // A clean containment fixture is FLAT/SIMPLE: a resident-slot dirty tile with
    //   * mergeCells == 1   (single-cell footprints; the only path the B1.3a CS handles)
    //   * childMask  == 0   (no resident finer children -> no L7 quadrant suppression)
    //   * NO edit footprint (the CPU suppresses edited cells; the GPU would over-emit)
    // For such a tile the CPU mesh is { top faces } (+ border skirts/risers), so GPU-top
    // is a strict multiset-subset.
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells != 1u || t.childMask != 0u) {
            continue;
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue;
        }
        return i;
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13bFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide,
    uint32_t terraceStep)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    const int32_t step = std::max(1, static_cast<int32_t>(terraceStep));

    // Mirror the shared HLSLI decode/quantize exactly so the scan agrees with the GPU.
    auto decodeMaterial = [](uint32_t sample) -> uint32_t {
        return (sample >> 16u) & 0xFFu;
    };
    auto isSolid = [](uint32_t material) -> bool {
        return material != 0u /*Air*/ && material != 2u /*Water*/;
    };
    auto floorDiv = [](int32_t a, int32_t b) -> int32_t {
        int32_t q = a / b;
        int32_t r = a - q * b;
        if (r != 0 && ((r < 0) != (b < 0))) {
            q -= 1;
        }
        return q;
    };
    auto quantY = [&](uint32_t sample) -> int32_t {
        const int32_t y = static_cast<int32_t>(sample & 0xFFFFu) -
            static_cast<int32_t>(MidMeshSampleHeightBias);
        return floorDiv(y, step) * step;
    };

    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells != 1u || t.childMask != 0u) {
            continue;
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue;
        }
        if (t.sampleCount < static_cast<uint32_t>(side) * side) {
            continue; // need the full grid for the neighbor scan
        }
        // STEPPED check: scan for two right/forward-adjacent solid cells with different
        // quantized heights (the exact condition that makes the riser path emit). The cell
        // grid is [0, side-1)^2; neighbor (cx+1,cz)/(cx,cz+1) is in-tile (cx+1 <= side-1).
        const uint32_t cellsPerRow = side - 1u;
        bool stepped = false;
        for (uint32_t cz = 0; cz < cellsPerRow && !stepped; ++cz) {
            for (uint32_t cx = 0; cx < cellsPerRow && !stepped; ++cx) {
                const uint32_t s = t.samples[cz * side + cx];
                if (!isSolid(decodeMaterial(s))) {
                    continue;
                }
                const int32_t h = quantY(s);
                const uint32_t sr = t.samples[cz * side + (cx + 1u)];
                if (isSolid(decodeMaterial(sr)) && quantY(sr) != h) {
                    stepped = true;
                    break;
                }
                const uint32_t sf = t.samples[(cz + 1u) * side + cx];
                if (isSolid(decodeMaterial(sf)) && quantY(sf) != h) {
                    stepped = true;
                    break;
                }
            }
        }
        if (stepped) {
            return i;
        }
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13cFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide,
    uint32_t terraceStep)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    (void)terraceStep; // not needed: a border SOLID cell emits a skirt regardless of height.

    // Mirror the shared HLSLI material decode + solid predicate exactly.
    auto decodeMaterial = [](uint32_t sample) -> uint32_t {
        return (sample >> 16u) & 0xFFu;
    };
    auto isSolid = [](uint32_t material) -> bool {
        return material != 0u /*Air*/ && material != 2u /*Water*/;
    };

    const uint32_t cellsPerRow = side - 1u;
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells != 1u || t.childMask != 0u) {
            continue;
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue;
        }
        if (t.sampleCount < static_cast<uint32_t>(side) * side) {
            continue; // need the full grid to scan border cells
        }
        // BORDER-SOLID check: the CPU emits a skirt on every SOLID footprint whose cell sits
        // on a tile edge (cx==0 || cx==cellsPerRow-1 || cz==0 || cz==cellsPerRow-1). Scan
        // the perimeter; if ANY border cell is solid, the GPU skirt path will emit (> 0).
        bool borderSolid = false;
        for (uint32_t cz = 0; cz < cellsPerRow && !borderSolid; ++cz) {
            const bool zEdge = (cz == 0u) || (cz + 1u >= cellsPerRow);
            for (uint32_t cx = 0; cx < cellsPerRow && !borderSolid; ++cx) {
                const bool xEdge = (cx == 0u) || (cx + 1u >= cellsPerRow);
                if (!xEdge && !zEdge) {
                    continue; // interior cell: no skirt
                }
                if (isSolid(decodeMaterial(t.samples[cz * side + cx]))) {
                    borderSolid = true;
                }
            }
        }
        if (borderSolid) {
            return i;
        }
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13dFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    // B1.3d INVERTS the childMask constraint: it REQUIRES childMask != 0 so the
    // child-quadrant suppression rule actually fires (otherwise the increment removes
    // nothing and the "suppression removed faces" gate could never be met). Still
    // mergeCells==1 (the only path the CS handles), no edit footprint (the CPU suppresses
    // edited cells), and a full sample grid. No height/solid scan is needed - suppression
    // depends only on which quadrant has a resident child, not on the heights.
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells != 1u || t.childMask == 0u) {
            continue; // REQUIRE a resident child quadrant (childMask != 0)
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue;
        }
        if (t.sampleCount < static_cast<uint32_t>(side) * side) {
            continue;
        }
        return i;
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13eFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    // B1.3e REQUIRES an EDITED tile (hasEditFootprint(slot) == TRUE) - the inverse of the
    // earlier increments. To isolate the edit skip as the ONLY removed-face source, prefer
    // mergeCells==1 (the only path the CS handles) AND childMask==0 (no child suppression in
    // play, so the only difference vs the full CPU mesh is the edited cells). A full sample
    // grid is needed for the cell scan/parity. No height scan is required - whether a cell is
    // edited depends only on the edit boxes, not the heights.
    if (!hasEditFootprint) {
        return UINT32_MAX; // no way to know which tiles are edited
    }
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells != 1u || t.childMask != 0u) {
            continue; // isolate the edit skip (no merge-block, no child suppression)
        }
        if (t.sampleCount < static_cast<uint32_t>(side) * side) {
            continue;
        }
        if (!hasEditFootprint(t.slot)) {
            continue; // REQUIRE an edit footprint (an edit overlapped this tile)
        }
        return i;
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13faFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    // B1.3f-a REQUIRES mergeCells > 1 (a far/LOD-merged tile) - the inverse of every earlier
    // increment (all required mergeCells==1). To isolate LOD as the only new variable, prefer
    // childMask==0 (no child suppression in play) AND no edit footprint (the CPU suppresses
    // edited cells). A full sample grid is needed for the per-block aggregation. No height
    // scan is required - the merge always changes the block geometry, so a clean merged tile
    // is sufficient to prove the per-BLOCK path; the merged-quad gate (gpuFaceCount much lower
    // than an unmerged tile) is checked by the caller from the A/B metrics.
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.mergeCells <= 1u || t.childMask != 0u) {
            continue; // REQUIRE a LOD-merged block (mergeCells>1), isolate from child suppression
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue; // no edit footprint (isolate the edit skip out)
        }
        if (t.sampleCount < static_cast<uint32_t>(side) * side) {
            continue;
        }
        return i;
    }
    return UINT32_MAX;
}

uint32_t MidMeshGpuExtractResources::SelectB13fbFixture(
    const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
    const std::function<bool(uint32_t)>& hasEditFootprint,
    uint32_t sampleSide,
    bool requireMerged,
    bool requireWater)
{
    if (sampleSide < 2u) {
        return UINT32_MAX;
    }
    const uint32_t side = sampleSide;
    const uint32_t needed = side * side;
    // B1.3f-b REQUIRES a tile that bears WATER and/or AIR samples (so a water top and/or the
    // all-air sea-level fill actually emit). Scan the tile's samples for any Water (material
    // 2) or Air (material 0). To isolate water/air-fill as the only new variable, prefer
    // childMask==0 + no edit footprint. `requireMerged` demands a LOD-merged tile (mergeCells>1)
    // for the merged-water sub-fixture; otherwise mergeCells is unconstrained.
    for (uint32_t i = 0; i < dirtyTiles.size(); ++i) {
        const auto& t = dirtyTiles[i];
        if (t.slot == UINT32_MAX || t.samples == nullptr || t.sampleCount == 0u) {
            continue;
        }
        if (t.childMask != 0u) {
            continue; // isolate from child suppression
        }
        if (requireMerged ? (t.mergeCells <= 1u) : false) {
            continue; // merged-water sub-fixture demands a LOD-merged tile
        }
        if (hasEditFootprint && hasEditFootprint(t.slot)) {
            continue; // no edit footprint (isolate the edit skip out)
        }
        if (t.sampleCount < needed) {
            continue;
        }
        // Scan for at least one WATER (and, when not requireWater, AIR) sample (material
        // decode mirrors UnpackMidHeightSurfaceSampleMaterial: (sample >> 16) & 0xFF).
        bool bearsWater = false;
        bool bearsAir = false;
        for (uint32_t s = 0u; s < needed; ++s) {
            const uint32_t material = (t.samples[s] >> 16u) & 0xFFu;
            if (material == 2u /*Water*/) { bearsWater = true; if (requireWater) { break; } }
            else if (material == 0u /*Air*/) { bearsAir = true; }
        }
        const bool qualifies = requireWater ? bearsWater : (bearsWater || bearsAir);
        if (!qualifies) {
            continue;
        }
        return i;
    }
    return UINT32_MAX;
}

bool MidMeshGpuExtractResources::RunB13aTopFaceDispatch(
    ID3D12Device* device,
    const Simulation::MidMeshGpuExtractDirtyTile& fixture,
    const std::vector<Simulation::SparseSurfaceFace>& cpuReferenceFaces,
    uint32_t terraceStep,
    uint64_t currentTileVersionForSlot,
    bool emitRisers,
    bool emitSkirts,
    bool applyChildSuppression,
    bool equalityMode,
    bool applyEditSkip,
    const std::vector<Simulation::MidMeshEditXzBox>& editBoxes,
    bool emitWater,
    bool applyDistanceCull,
    const std::vector<uint8_t>& cullBlockMask)
{
    return RunB13aTopFaceDispatchInternal(
        device, fixture, cpuReferenceFaces, terraceStep, currentTileVersionForSlot,
        emitRisers, emitSkirts, applyChildSuppression, equalityMode, applyEditSkip,
        editBoxes, emitWater, applyDistanceCull, cullBlockMask, false);
}

bool MidMeshGpuExtractResources::RunB13aTopFaceDispatchProduction(
    ID3D12Device* device,
    const Simulation::MidMeshGpuExtractDirtyTile& fixture,
    const std::vector<Simulation::SparseSurfaceFace>& cpuReferenceFaces,
    uint32_t terraceStep,
    uint64_t currentTileVersionForSlot,
    bool emitRisers,
    bool emitSkirts,
    bool applyChildSuppression,
    bool equalityMode,
    bool applyEditSkip,
    const std::vector<Simulation::MidMeshEditXzBox>& editBoxes,
    bool emitWater,
    bool applyDistanceCull,
    const std::vector<uint8_t>& cullBlockMask)
{
    return RunB13aTopFaceDispatchInternal(
        device, fixture, cpuReferenceFaces, terraceStep, currentTileVersionForSlot,
        emitRisers, emitSkirts, applyChildSuppression, equalityMode, applyEditSkip,
        editBoxes, emitWater, applyDistanceCull, cullBlockMask, true);
}

bool MidMeshGpuExtractResources::UpdateProductionDrawArgs(
    ID3D12GraphicsCommandList* commandList,
    const std::vector<uint32_t>& committedSlots)
{
    if (!commandList ||
        !m_b13aReady ||
        !m_productionDrawArgsPipeline.IsValid() ||
        !m_productionFaceCountBuffer.GetResource() ||
        !m_productionFaceStatusBuffer.GetResource() ||
        !m_productionCommitBuffer.GetResource() ||
        !m_productionDrawArgsBuffer.GetResource() ||
        m_config.maxTiles == 0u ||
        m_productionFaceCapacityPerTile == 0u) {
        return false;
    }

    std::vector<uint32_t> commitFlags(m_config.maxTiles, 0u);
    uint32_t committedCount = 0u;
    for (const uint32_t slot : committedSlots) {
        if (slot < m_config.maxTiles && commitFlags[slot] == 0u) {
            commitFlags[slot] = 1u;
            ++committedCount;
        }
    }

    const uint64_t commitBytes =
        static_cast<uint64_t>(commitFlags.size()) * sizeof(uint32_t);
    if (commitBytes == 0u || commitBytes > m_productionCommitUpload.GetSize()) {
        return false;
    }
    if (auto* mapped = static_cast<uint8_t*>(m_productionCommitUpload.GetMappedData())) {
        std::memcpy(mapped, commitFlags.data(), static_cast<size_t>(commitBytes));
    } else {
        return false;
    }

    m_productionCommitBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_productionCommitBuffer.GetResource(), 0,
        m_productionCommitUpload.GetResource(), 0,
        commitBytes);
    m_productionCommitBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_productionFaceCountBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_productionFaceStatusBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_productionDrawArgsBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint32_t consts[4] = {
        m_config.maxTiles,
        m_productionFaceCapacityPerTile,
        0u,
        0u
    };
    m_productionDrawArgsPipeline.Bind(commandList);
    m_productionDrawArgsPipeline.SetRoot32BitConstants(commandList, 0, 4, consts);
    commandList->SetComputeRootShaderResourceView(
        1, m_productionFaceCountBuffer.GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        2, m_productionFaceStatusBuffer.GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        3, m_productionCommitBuffer.GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        4, m_productionDrawArgsBuffer.GetGPUVirtualAddress());
    m_productionDrawArgsPipeline.Dispatch(
        commandList, (m_config.maxTiles + 63u) / 64u, 1u, 1u);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = m_productionDrawArgsBuffer.GetResource();
    commandList->ResourceBarrier(1, &barrier);

    m_productionDrawArgsBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_productionFaceBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_stats.productionCommittedSlots = committedCount;
    m_stats.productionDrawCommandCount = m_config.maxTiles;
    return true;
}

void MidMeshGpuExtractResources::TransitionProductionFaceBuffer(
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES state)
{
    if (commandList && m_productionFaceBuffer.GetResource()) {
        m_productionFaceBuffer.TransitionTo(commandList, state);
    }
}

bool MidMeshGpuExtractResources::QueueWaitForProduction(ID3D12CommandQueue* commandQueue) const
{
    if (!commandQueue || !m_smokeFence || m_smokeFenceValue == 0u) {
        return true;
    }
    const HRESULT hr = commandQueue->Wait(m_smokeFence.Get(), m_smokeFenceValue);
    if (FAILED(hr)) {
        spdlog::error(
            "MidMeshGpuExtractResources::QueueWaitForProduction failed: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

uint64_t MidMeshGpuExtractResources::ProductionQueueCompletedFenceValue() const
{
    return m_smokeFence ? m_smokeFence->GetCompletedValue() : 0u;
}

bool MidMeshGpuExtractResources::ProductionQueueIdle() const
{
    if (!m_smokeFence || m_smokeFenceValue == 0u) {
        return true;
    }
    return m_smokeFence->GetCompletedValue() >= m_smokeFenceValue;
}

bool MidMeshGpuExtractResources::RunB13aTopFaceDispatchInternal(
    ID3D12Device* device,
    const Simulation::MidMeshGpuExtractDirtyTile& fixture,
    const std::vector<Simulation::SparseSurfaceFace>& cpuReferenceFaces,
    uint32_t terraceStep,
    uint64_t currentTileVersionForSlot,
    bool emitRisers,
    bool emitSkirts,
    bool applyChildSuppression,
    bool equalityMode,
    bool applyEditSkip,
    const std::vector<Simulation::MidMeshEditXzBox>& editBoxes,
    bool emitWater,
    bool applyDistanceCull,
    const std::vector<uint8_t>& cullBlockMask,
    bool productionOutput)
{
    if (!m_b13aReady || !device) {
        return false;
    }
    if (fixture.slot >= m_config.maxTiles || fixture.samples == nullptr ||
        fixture.sampleCount == 0u) {
        return false;
    }
    // B1.3e: the edit-box list is capped at editBoxCapacityPerTile (the caller already caps
    // its fetch). If somehow more arrived, REJECT the dispatch (report it) rather than upload
    // a truncated footprint that would under-suppress and surface as a false extra. An empty
    // list with applyEditSkip true is benign (no cell is inside an empty footprint).
    const uint32_t editBoxCount = static_cast<uint32_t>(editBoxes.size());
    const bool editBoxOverflow = (editBoxCount > m_editBoxCapacityPerTile);
    if (applyEditSkip && editBoxOverflow) {
        spdlog::warn(
            "[GPU_EXTRACT_B13E] tile slot {} has {} edit boxes > capacity {}; dispatch SKIPPED "
            "(raise editBoxCapacityPerTile)",
            fixture.slot, editBoxCount, m_editBoxCapacityPerTile);
        m_b13aStats.editBoxOverflow = 1u;
        m_b13aStats.editBoxCount = editBoxCount;
        return false;
    }
    // B1.3f-c: the per-block cull mask is bounded by m_cullBlockCapacityPerTile (worst-case one
    // block per cell). If a larger mask arrives (should never - the host sizes it to the same
    // bound), REJECT the dispatch rather than upload a truncated mask that would under-cull and
    // surface as a false extra. An empty mask with applyDistanceCull true is benign (no block
    // flagged). A non-cull dispatch leaves the cull buffer untouched (gApplyDistanceCull=0).
    const uint32_t cullBlockCount = static_cast<uint32_t>(cullBlockMask.size());
    if (applyDistanceCull && cullBlockCount > m_cullBlockCapacityPerTile) {
        spdlog::warn(
            "[GPU_EXTRACT_B13FC] tile slot {} cull mask {} blocks > capacity {}; dispatch SKIPPED",
            fixture.slot, cullBlockCount, m_cullBlockCapacityPerTile);
        return false;
    }
    if (!m_smokeCmdAllocator || !m_smokeCmdList) {
        spdlog::error("[GPU_EXTRACT_B13A] command objects missing");
        return false;
    }
    m_b13aBuildTerraceStep = std::max(1u, terraceStep);

    // Capture the controlled fixture + its FULL CPU reference mesh so the delayed poll
    // can A/B in CONTAINMENT mode.
    SmokeControlledTile ctl;
    ctl.valid = true;
    ctl.b13aTopFace = true;
    ctl.emitRisers = emitRisers; // B1.3b: poll labels b13b + the riser path was active
    ctl.emitSkirts = emitSkirts; // B1.3c: poll labels b13c + the skirt path was active
    ctl.applyChildSuppression = applyChildSuppression; // B1.3d: suppression was active
    ctl.applyEditSkip = applyEditSkip; // B1.3e: edit-footprint suppression was active
    ctl.emitWater = emitWater; // B1.3f-b: water-aware aggregation + all-air fill was active
    ctl.applyDistanceCull = applyDistanceCull; // B1.3f-c: camera-distance cull (CPU mask) was active
    // B1.3f-c: count the blocks the CPU mask flagged as culled (for the log + the "cull removed
    // blocks" gate). 0 when the cull is off or no block was beyond the threshold.
    {
        uint32_t culled = 0u;
        for (const uint8_t f : cullBlockMask) { culled += (f != 0u) ? 1u : 0u; }
        ctl.culledBlocks = culled;
    }
    ctl.equalityMode = equalityMode; // B1.3d: poll A/Bs in EQUALITY mode
    ctl.productionOutput = productionOutput;
    ctl.outputSlot = productionOutput ? fixture.slot : 0u;
    ctl.mergeCells = std::max(1u, fixture.mergeCells); // B1.3f-a: block span for the poll mirrors
    ctl.childMask = fixture.childMask; // B1.3d: for the suppressed-cell count
    ctl.editBoxes = editBoxes; // B1.3e: for the edit-skipped-cell count (poll mirror)
    ctl.slot = 0u;       // the dedicated smoke input buffers always use metadata/sample slot 0
    ctl.baseFace = productionOutput
        ? ctl.outputSlot * m_productionFaceCapacityPerTile
        : 0u;
    ctl.version = fixture.meshContentVersion;
    ctl.originX = fixture.originX;
    ctl.originZ = fixture.originZ;
    ctl.cellSizeInt = std::max(1, static_cast<int32_t>(fixture.cellSize));
    ctl.sampleSide = m_config.tileSampleSide;
    ctl.expectedCount = 0u; // variable (counter-driven); filled by the GPU faceCount
    ctl.samples.assign(fixture.samples, fixture.samples + fixture.sampleCount);
    ctl.cpuReferenceFaces = cpuReferenceFaces;

    // Reuse the persistent allocator/list. Wait the PREVIOUS dispatch's fence so the
    // allocator can be reset (non-blocking in steady state; NOT a wait on this dispatch).
    const uint64_t prevFence = m_smokeFenceValue;
    if (prevFence != 0u && m_smokeFence->GetCompletedValue() < prevFence) {
        m_smokeFence->SetEventOnCompletion(prevFence, m_smokeFenceEvent);
        WaitForSingleObject(m_smokeFenceEvent, INFINITE);
    }
    if (FAILED(m_smokeCmdAllocator->Reset()) ||
        FAILED(m_smokeCmdList->Reset(m_smokeCmdAllocator.Get(), nullptr))) {
        spdlog::error("[GPU_EXTRACT_B13A] command list reset failed");
        return false;
    }
    ID3D12GraphicsCommandList* const list = m_smokeCmdList.Get();
    const bool haveTimestamps = (m_smokeQueryHeap != nullptr);

    const uint32_t writeSlot = m_smokeReadbackWriteSlot;
    SmokeReadbackSlot& ringSlot = m_smokeReadbackRing[writeSlot];

    // ---- self-contained input upload into the DEDICATED smoke buffers ----
    if (auto* mapped = static_cast<uint8_t*>(m_smokeInputUpload.GetMappedData())) {
        const uint64_t tileSampleBytes =
            static_cast<uint64_t>(fixture.sampleCount) * sizeof(uint32_t);
        std::memcpy(mapped, fixture.samples, static_cast<size_t>(tileSampleBytes));

        MidMeshGpuExtractTileMeta meta;
        meta.coordX = fixture.coord.x;
        meta.coordRing = fixture.coord.y;
        meta.coordZ = fixture.coord.z;
        meta.originX = fixture.originX;
        meta.originZ = fixture.originZ;
        std::memcpy(&meta.cellSizeBits, &fixture.cellSize, sizeof(uint32_t));
        meta.mergeCells = fixture.mergeCells;   // == 1 for a B1.3a fixture
        meta.childMask = fixture.childMask;     // == 0 for a B1.3a fixture
        meta.meshContentVersionLo =
            static_cast<uint32_t>(fixture.meshContentVersion & 0xFFFFFFFFull);
        meta.meshContentVersionHi =
            static_cast<uint32_t>(fixture.meshContentVersion >> 32u);
        meta.sampleSide = m_config.tileSampleSide;
        meta.sampleStride = m_config.tileSampleSide;
        meta.haloWidth = 0u;
        meta.heightPackDesc = PackMidMeshHeightDesc();
        meta.heightBias = MidMeshSampleHeightBias;
        // B1.3e: record the per-tile edit-box count in the reserved metadata field (0 when
        // not edit-skipping). Carried for diagnostics; the shader uses the gEditBoxCount
        // root constant for the loop bound.
        meta.editFootprintCount = applyEditSkip ? editBoxCount : 0u;
        meta.faceCapacity = productionOutput
            ? m_productionFaceCapacityPerTile
            : m_topFaceCapacityPerTile;
        meta.baseFace = ctl.baseFace;
        // faceCount is the per-tile append COUNTER: start at 0 so InterlockedAdd hands out
        // dense slots [0..count). statusOverflow starts clean.
        meta.faceCount = 0u;
        meta.statusOverflow = 0u;
        std::memcpy(mapped + tileSampleBytes, &meta, sizeof(meta));

        ID3D12Resource* uploadRes = m_smokeInputUpload.GetResource();
        m_smokeSampleBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(
            m_smokeSampleBuffer.GetResource(), 0, uploadRes, 0, tileSampleBytes);
        list->CopyBufferRegion(
            m_smokeMetaBuffer.GetResource(), 0,
            uploadRes, tileSampleBytes, sizeof(MidMeshGpuExtractTileMeta));
        m_smokeSampleBuffer.TransitionTo(
            list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // B1.3e: DIRTY-SCALED edit-box upload. ONLY when this is an edit-skip dispatch with at
    // least one box (a non-edited dispatch leaves m_editBoxBuffer untouched -> zero extra
    // upload, and the shader sees gEditBoxCount=0 so it never reads it). One tile's boxes.
    uint64_t editBoxBytesUploaded = 0u;
    if (applyEditSkip && editBoxCount > 0u) {
        if (auto* mappedBoxes = static_cast<uint8_t*>(m_editBoxUpload.GetMappedData())) {
            const uint64_t boxBytes =
                static_cast<uint64_t>(editBoxCount) * sizeof(Simulation::MidMeshEditXzBox);
            std::memcpy(mappedBoxes, editBoxes.data(), static_cast<size_t>(boxBytes));
            m_editBoxBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(
                m_editBoxBuffer.GetResource(), 0, m_editBoxUpload.GetResource(), 0, boxBytes);
            m_editBoxBuffer.TransitionTo(
                list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            editBoxBytesUploaded = boxBytes;
        }
    }

    // B1.3f-c: DIRTY-SCALED per-block cull-mask upload. ONLY when this is a distance-cull
    // dispatch with >=1 block flag (a non-cull dispatch leaves m_cullBlockBuffer untouched ->
    // zero extra upload, and the shader sees gApplyDistanceCull=0 so it never reads it). The
    // host already packed the CPU's per-block verdict (1 = culled) into cullBlockMask, indexed by
    // blockId = bz*blockCountPerAxis + bx. We widen each uint8 flag to uint32 for the StructuredBuffer.
    uint64_t cullBlockBytesUploaded = 0u;
    if (applyDistanceCull && cullBlockCount > 0u) {
        if (auto* mappedCull = static_cast<uint32_t*>(m_cullBlockUpload.GetMappedData())) {
            for (uint32_t i = 0u; i < cullBlockCount; ++i) {
                mappedCull[i] = (cullBlockMask[i] != 0u) ? 1u : 0u;
            }
            const uint64_t cullBytes = static_cast<uint64_t>(cullBlockCount) * sizeof(uint32_t);
            m_cullBlockBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(
                m_cullBlockBuffer.GetResource(), 0, m_cullBlockUpload.GetResource(), 0, cullBytes);
            m_cullBlockBuffer.TransitionTo(
                list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cullBlockBytesUploaded = cullBytes;
        }
    }

    GPUBuffer& outputFaceBuffer = productionOutput ? m_productionFaceBuffer : m_smokeFaceBuffer;
    const uint32_t outputFaceCapacity =
        productionOutput ? m_productionFaceCapacityPerTile : m_topFaceCapacityPerTile;
    const uint64_t countOffset = static_cast<uint64_t>(ctl.outputSlot) * sizeof(uint32_t);
    m_productionFaceCountBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
    m_productionFaceStatusBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(
        m_productionFaceCountBuffer.GetResource(), countOffset,
        m_productionCountClearUpload.GetResource(), 0, sizeof(uint32_t));
    list->CopyBufferRegion(
        m_productionFaceStatusBuffer.GetResource(), countOffset,
        m_productionCountClearUpload.GetResource(), 0, sizeof(uint32_t));
    m_productionFaceCountBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_productionFaceStatusBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outputFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_topFacePipeline.Bind(list);
    // b0: {tileSlot, debugBaseFace, faceCapacityPerTile, terraceStep, emitRisers,
    //      emitSkirts, seaLevelY, applyChildSuppression}. seaLevelY mirrors the CPU
    //      compile-time SEA_LEVEL_Y so the GPU skirt rule never hardcodes it; bit-cast the
    //      signed int into the uint slot. applyChildSuppression gates the B1.3d cell skip.
    uint32_t seaLevelBits;
    const int32_t seaLevelY = Simulation::SEA_LEVEL_Y;
    std::memcpy(&seaLevelBits, &seaLevelY, sizeof(uint32_t));
    // B1.3e: gEditBoxBase=0 (the dedicated edit-box buffer holds this ONE tile's boxes at
    // base 0); gEditBoxCount=0 when not edit-skipping so the shader's edit loop never runs.
    // B1.3f-b: gEmitWater gates the water-aware aggregation + all-air sea-level fill.
    // B1.3f-c: gApplyDistanceCull gates the per-block cull skip; gCullBlockBase=0 (the dedicated
    // cull buffer holds this ONE tile's per-block flags at base 0). gApplyDistanceCull=0 when not
    // culling so the shader never reads CullBlockMask.
    const uint32_t consts[15] = {
        ctl.slot, ctl.baseFace, outputFaceCapacity, m_b13aBuildTerraceStep,
        emitRisers ? 1u : 0u,
        emitSkirts ? 1u : 0u,
        seaLevelBits,
        applyChildSuppression ? 1u : 0u,
        applyEditSkip ? 1u : 0u,
        0u,                                       // gEditBoxBase
        applyEditSkip ? editBoxCount : 0u,        // gEditBoxCount
        emitWater ? 1u : 0u,                      // gEmitWater (B1.3f-b)
        applyDistanceCull ? 1u : 0u,              // gApplyDistanceCull (B1.3f-c)
        0u,                                       // gCullBlockBase (B1.3f-c)
        ctl.outputSlot                            // gOutputSlot (STEP 1 production count slot)
    };
    m_topFacePipeline.SetRoot32BitConstants(list, 0, 15, consts);
    list->SetComputeRootShaderResourceView(1, m_smokeSampleBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, outputFaceBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, m_smokeMetaBuffer.GetGPUVirtualAddress());
    // B1.3e: t1 edit boxes (root index 4). Always bind a valid SRV address (even when
    // gEditBoxCount==0 the binding must be valid); the buffer holds this tile's boxes.
    list->SetComputeRootShaderResourceView(4, m_editBoxBuffer.GetGPUVirtualAddress());
    // B1.3f-c: t2 cull-block mask (root index 5). Always bind a valid SRV address (even when
    // gApplyDistanceCull==0 the binding must be valid); the buffer holds this tile's flags.
    list->SetComputeRootShaderResourceView(5, m_cullBlockBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(6, m_productionFaceCountBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(7, m_productionFaceStatusBuffer.GetGPUVirtualAddress());

    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }
    // One thread per cell: cellCount^2 cells, 64 threads/group.
    const uint32_t side = m_config.tileSampleSide;
    const uint32_t cellsPerRow = (side > 1u) ? (side - 1u) : 0u;
    const uint32_t cellCount = cellsPerRow * cellsPerRow;
    const uint32_t groups = (cellCount + 63u) / 64u;
    m_topFacePipeline.Dispatch(list, std::max(1u, groups), 1, 1);
    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    }

    D3D12_RESOURCE_BARRIER uavBarriers[4] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = outputFaceBuffer.GetResource();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = m_smokeMetaBuffer.GetResource();
    uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[2].UAV.pResource = m_productionFaceCountBuffer.GetResource();
    uavBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[3].UAV.pResource = m_productionFaceStatusBuffer.GetResource();
    list->ResourceBarrier(4, uavBarriers);
    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    }

    // Copy this dispatch's faces + metadata into the current ring slot (persistently
    // mapped, fence-tracked) so a later poll reads it back. Copy the FULL top-face
    // capacity (the poll clamps to the GPU-written faceCount).
    outputFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_productionFaceCountBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_productionFaceStatusBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(
        ringSlot.faceReadback.GetResource(), 0,
        outputFaceBuffer.GetResource(), static_cast<uint64_t>(ctl.baseFace) * sizeof(Simulation::SparseSurfaceFace),
        static_cast<uint64_t>(outputFaceCapacity) * sizeof(Simulation::SparseSurfaceFace));
    list->CopyBufferRegion(
        ringSlot.metaReadback.GetResource(), 0,
        m_smokeMetaBuffer.GetResource(), 0,
        sizeof(MidMeshGpuExtractTileMeta));
    list->CopyBufferRegion(
        ringSlot.countReadback.GetResource(), 0,
        m_productionFaceCountBuffer.GetResource(), countOffset,
        sizeof(uint32_t));
    list->CopyBufferRegion(
        ringSlot.statusReadback.GetResource(), 0,
        m_productionFaceStatusBuffer.GetResource(), countOffset,
        sizeof(uint32_t));
    if (haveTimestamps && m_smokeQueryReadback.GetResource()) {
        list->ResolveQueryData(
            m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3,
            m_smokeQueryReadback.GetResource(), 0);
    }

    outputFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_productionFaceCountBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_productionFaceStatusBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeSampleBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    // B1.3e: restore the edit-box buffer to COMMON for the next dispatch (only if it was
    // transitioned this dispatch).
    if (editBoxBytesUploaded > 0u) {
        m_editBoxBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    }
    // B1.3f-c: same for the cull-block buffer.
    if (cullBlockBytesUploaded > 0u) {
        m_cullBlockBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    }

    const auto cpuSubmitStart = std::chrono::steady_clock::now();
    list->Close();
    ID3D12CommandList* lists[] = { list };
    m_smokeQueue->ExecuteCommandLists(1, lists);
    const uint64_t fence = ++m_smokeFenceValue;
    m_smokeQueue->Signal(m_smokeFence.Get(), fence);
    const auto cpuSubmitStop = std::chrono::steady_clock::now();

    ctl.fenceValue = fence;

    // Record into the ring (no blocking wait on its fence; the poll reads it later).
    ringSlot.fenceValue = fence;
    ringSlot.pending = true;
    ringSlot.tile = ctl;
    m_smokeReadbackWriteSlot = (writeSlot + 1u) % kSmokeReadbackSlots;

    const bool stale = (currentTileVersionForSlot != ctl.version);

    double dispatchUs = 0.0, barrierUs = 0.0;
    const bool timingReady =
        (m_smokeFence->GetCompletedValue() >= fence) ||
        (prevFence != 0u && m_smokeFence->GetCompletedValue() >= prevFence);
    if (haveTimestamps && m_smokeTimestampFrequency != 0u && timingReady) {
        if (const auto* ts = static_cast<const uint64_t*>(m_smokeQueryReadback.Map())) {
            const double toUs = 1.0e6 / static_cast<double>(m_smokeTimestampFrequency);
            dispatchUs = (ts[1] >= ts[0]) ? static_cast<double>(ts[1] - ts[0]) * toUs : 0.0;
            barrierUs = (ts[2] >= ts[1]) ? static_cast<double>(ts[2] - ts[1]) * toUs : 0.0;
            m_smokeQueryReadback.Unmap();
        }
    }
    const double cpuSubmitUs =
        std::chrono::duration<double, std::micro>(cpuSubmitStop - cpuSubmitStart).count();

    // Preserve cumulative verify fields across the per-dispatch stats refresh.
    const uint32_t keepVerifyCount = m_b13aStats.verifyCount;
    const bool keepVerified = m_b13aStats.verified;
    m_b13aStats = {};
    m_b13aStats.dispatched = true;
    m_b13aStats.emitRisers = emitRisers;
    m_b13aStats.emitSkirts = emitSkirts;
    m_b13aStats.applyChildSuppression = applyChildSuppression;
    m_b13aStats.applyEditSkip = applyEditSkip;
    m_b13aStats.editBoxCount = applyEditSkip ? editBoxCount : 0u;
    m_b13aStats.editBoxOverflow = 0u; // overflow already early-returned above
    m_b13aStats.editBoxBytes = editBoxBytesUploaded;
    m_b13aStats.emitWater = emitWater;
    m_b13aStats.equalityMode = equalityMode;
    m_b13aStats.productionOutput = productionOutput;
    m_b13aStats.tileSlot = fixture.slot;        // report the REAL cache slot (fixture)
    m_b13aStats.fixtureMergeCells = fixture.mergeCells;
    m_b13aStats.fixtureChildMask = fixture.childMask;
    m_b13aStats.cpuRefFaceCount = static_cast<uint32_t>(cpuReferenceFaces.size());
    m_b13aStats.gpuFaceCount = UINT32_MAX;        // unread until PollB13aReadback
    m_b13aStats.dispatchedVersion = ctl.version;
    m_b13aStats.commitGateStale = stale;
    m_b13aStats.dispatchGpuUs = dispatchUs;
    m_b13aStats.barrierGpuUs = barrierUs;
    m_b13aStats.cpuSubmitUs = cpuSubmitUs;
    m_b13aStats.verified = keepVerified;
    m_b13aStats.verifyCount = keepVerifyCount;

    // Label: b13fb if the WATER + all-air-fill path is on (this increment owns its own corpus
    // label so the stage can tell increments apart - the prior code logged it under b13c).
    // Else b13e if edit skip is on (a face-REMOVING source), else b13d if child suppression,
    // else b13c if skirts, else b13b if risers, else b13a (each a superset of the prior).
    const char* const modeLabel = emitWater
        ? "b13fb"
        : (applyEditSkip
               ? "b13e"
               : (applyChildSuppression
                      ? "b13d"
                      : (emitSkirts ? "b13c" : (emitRisers ? "b13b" : "b13a"))));
    spdlog::info(
        "GPU_EXTRACT_B13A mode={} abMode={} productionOutput={} fixtureSlot={} mergeCells={} childMask={} cpuRefFaces={} "
        "editBoxes={} editBoxBytes={} dispatchGpuUs={:.2f} barrierGpuUs={:.2f} cpuSubmitUs={:.2f} "
        "commitGate={} dispatchedVersion={} currentVersion={}",
        modeLabel, equalityMode ? "equal" : "contain", productionOutput ? 1 : 0,
        fixture.slot, fixture.mergeCells, fixture.childMask,
        m_b13aStats.cpuRefFaceCount, m_b13aStats.editBoxCount, m_b13aStats.editBoxBytes,
        dispatchUs, barrierUs, cpuSubmitUs,
        stale ? "DISCARD_STALE" : "PASS",
        ctl.version, currentTileVersionForSlot);
    return true;
}

bool MidMeshGpuExtractResources::PollB13aReadback() {
    if (!m_b13aReady) {
        return false;
    }
    // FIFO-consume the oldest pending ring slot whose copy fence has completed. Only act
    // on B1.3a dispatches here (the smoke poll owns equality dispatches); a non-B1.3a slot
    // at the read cursor is left for PollSmokeReadback. (In a B1.3a run only B1.3a
    // dispatches are recorded, so this is the only consumer.)
    SmokeReadbackSlot& slot = m_smokeReadbackRing[m_smokeReadbackReadSlot];
    if (!slot.pending || !slot.tile.valid || !slot.tile.b13aTopFace) {
        return false;
    }
    const uint64_t completed = m_smokeFence->GetCompletedValue();
    if (completed < slot.fenceValue) {
        if (!m_smokeReadbackForceWait) {
            return false; // not done yet (non-blocking)
        }
        if (m_smokeFenceEvent) {
            m_smokeFence->SetEventOnCompletion(slot.fenceValue, m_smokeFenceEvent);
            WaitForSingleObject(m_smokeFenceEvent, INFINITE);
        }
    }

    uint32_t gpuFaceCount = 0;
    uint32_t gpuStatus = 0;
    if (slot.metaPtr) {
        const auto* meta = static_cast<const MidMeshGpuExtractTileMeta*>(slot.metaPtr);
        gpuStatus = meta->statusOverflow;
    }
    if (slot.countPtr) {
        gpuFaceCount = *static_cast<const uint32_t*>(slot.countPtr);
    }
    if (slot.statusPtr) {
        gpuStatus = std::max(gpuStatus, *static_cast<const uint32_t*>(slot.statusPtr));
    }
    const bool productionOutput = slot.tile.productionOutput;

    // Gather the GPU top faces the shader actually wrote (clamped to capacity; on overflow
    // the result is rejected by the harness so we read nothing meaningful).
    std::vector<Simulation::SparseSurfaceFace> gpuFaces;
    if (slot.facePtr) {
        const auto* faces = static_cast<const Simulation::SparseSurfaceFace*>(slot.facePtr);
        const uint32_t faceCapacity =
            productionOutput ? m_productionFaceCapacityPerTile : m_topFaceCapacityPerTile;
        const uint32_t emitted =
            (gpuStatus == 0u) ? std::min(gpuFaceCount, faceCapacity) : 0u;
        gpuFaces.assign(faces, faces + emitted);
    }

    // B1.3b: a GPU RISER is any emitted face whose packed direction is not PosY (the top).
    // Count them so the gate can require riser faces > 0 (the increment really added risers).
    const bool emitRisers = slot.tile.emitRisers;
    const bool emitSkirts = slot.tile.emitSkirts;
    const bool applyChildSuppression = slot.tile.applyChildSuppression;
    const bool applyEditSkip = slot.tile.applyEditSkip;
    const bool emitWater = slot.tile.emitWater;
    const bool applyDistanceCull = slot.tile.applyDistanceCull;
    const uint32_t culledBlocks = slot.tile.culledBlocks; // B1.3f-c: blocks the CPU mask flagged
    const bool equalityMode = slot.tile.equalityMode;
    uint32_t gpuRiserFaces = 0u;
    // B1.3f-b: count GPU faces with the Water material (low byte of the packed voxel ==
    // Material::Water == 2). This is the "water faces emitted" gate signal: it covers BOTH
    // water-surface tops AND the all-air sea-level fill tops/risers (all carry the Water
    // material). 0 when water is off (the solid-only path never emits a Water face).
    uint32_t gpuWaterFaces = 0u;
    // B1.3f-a: track the MAX top-face quad world span so the LOD-merge run can show that
    // merged quads are physically LARGER than a single cell (a 1x1 cell quad spans cellSize;
    // a merged kxk block spans up to k*cellSize, capped per sub-face by the 32u split limit).
    // PayloadWidth/Height are the +1-decoded packed extents (PosY: width=X span, height=Z span).
    uint32_t gpuMaxTopQuadW = 0u, gpuMaxTopQuadH = 0u;
    for (const auto& f : gpuFaces) {
        if (Simulation::SparseSurfacePayloadDirection(f.payload) !=
            static_cast<uint32_t>(Simulation::SparseFaceDirection::PosY)) {
            ++gpuRiserFaces;
        } else {
            gpuMaxTopQuadW = std::max(gpuMaxTopQuadW, Simulation::SparseSurfacePayloadWidth(f.payload));
            gpuMaxTopQuadH = std::max(gpuMaxTopQuadH, Simulation::SparseSurfacePayloadHeight(f.payload));
        }
        // B1.3f-b: Water material is the low byte of the packed voxel field (== 2).
        if ((Simulation::SparseSurfacePayloadVoxel(f.payload) & 0xFFu) == 2u /*Water*/) {
            ++gpuWaterFaces;
        }
    }

    // B1.3f-b: COUNT the BLOCKS the GPU all-air-FILLED (a block whose samples are ALL Air,
    // filled with a sea-level water top - SparseClipmap.cpp ~7065-7083). Recompute the
    // water-aware block aggregation CPU-side (drift-free mirror of MidMeshAggregateBlock) and
    // count blocks that are !present (all-air -> filled). > 0 confirms the new all-air-fill
    // path actually fired on this fixture. 0 when water is off (no fill) or no all-air block.
    uint32_t gpuAirFillFaces = 0u;
    if (emitWater && slot.tile.sampleSide > 1u &&
        slot.tile.samples.size() >=
            static_cast<size_t>(slot.tile.sampleSide) * slot.tile.sampleSide) {
        const uint32_t side = slot.tile.sampleSide;
        const uint32_t stride = side;
        const uint32_t cellsPerAxis = side - 1u;
        const int32_t step = std::max(1, static_cast<int32_t>(m_b13aBuildTerraceStep));
        const uint32_t mergeCells = std::max(1u, slot.tile.mergeCells);
        const uint32_t blockCountPerAxis = (cellsPerAxis + mergeCells - 1u) / mergeCells;
        auto decodeMaterial = [](uint32_t s) -> uint32_t { return (s >> 16u) & 0xFFu; };
        auto isSolid = [](uint32_t m) -> bool { return m != 0u && m != 2u; };
        auto isWater = [](uint32_t m) -> bool { return m == 2u; };
        for (uint32_t bz = 0u; bz < blockCountPerAxis; ++bz) {
            const uint32_t z = bz * mergeCells;
            const uint32_t zEnd = std::min(cellsPerAxis, z + mergeCells);
            for (uint32_t bx = 0u; bx < blockCountPerAxis; ++bx) {
                const uint32_t x = bx * mergeCells;
                const uint32_t xEnd = std::min(cellsPerAxis, x + mergeCells);
                // Clamp identical to the shader/CPU aggregate.
                uint32_t x0 = std::min(x, side - 1u);
                uint32_t z0 = std::min(z, side - 1u);
                uint32_t x1 = std::max(x0 + 1u, std::min(xEnd, side));
                uint32_t z1 = std::max(z0 + 1u, std::min(zEnd, side));
                bool present = false;
                for (uint32_t zz = z0; zz < z1 && !present; ++zz) {
                    for (uint32_t xx = x0; xx < x1; ++xx) {
                        const uint32_t m = decodeMaterial(slot.tile.samples[zz * stride + xx]);
                        if (isSolid(m) || isWater(m)) { present = true; break; }
                    }
                }
                if (!present) {
                    // This block is all-air -> the GPU fills it with ONE sea-level water top
                    // quad, split into 32u chunks for a merged block (same as the shader's
                    // MidMeshAddFace). Count the sub-faces the fill emits.
                    const int32_t cellSizeInt = std::max(1, slot.tile.cellSizeInt);
                    const uint32_t width = (xEnd - x) * static_cast<uint32_t>(cellSizeInt);
                    const uint32_t depth = (zEnd - z) * static_cast<uint32_t>(cellSizeInt);
                    const uint32_t wChunks = (width + 31u) / 32u;
                    const uint32_t hChunks = (depth + 31u) / 32u;
                    gpuAirFillFaces += std::max(1u, wChunks) * std::max(1u, hChunks);
                }
                (void)step;
            }
        }
    }

    // B1.3c: COUNT the GPU faces that are TILE-BORDER SKIRTS, drift-free. Recompute the exact
    // skirt face set CPU-side (same FNV voxel hash / quantize / addRiser split as the shader),
    // build a multiset, and count how many GPU faces appear in it. This is the "skirt faces > 0"
    // gate signal - it isolates the new B1.3c contribution from the B1.3b interior risers (both
    // share riser DIRECTIONS, so direction alone cannot tell them apart). 0 when skirts are off.
    uint32_t gpuSkirtFaces = 0u;
    if (emitSkirts) {
        std::vector<Simulation::SparseSurfaceFace> skirtFaces;
        ComputeB13cSkirtFacesCpu(slot.tile, m_b13aBuildTerraceStep, Simulation::SEA_LEVEL_Y,
                                 skirtFaces);
        // Multiset key = the raw 16-byte face (memcmp), matching the AB harness canonical form.
        auto faceLess = [](const Simulation::SparseSurfaceFace& a,
                           const Simulation::SparseSurfaceFace& b) {
            return std::memcmp(&a, &b, sizeof(Simulation::SparseSurfaceFace)) < 0;
        };
        std::multiset<Simulation::SparseSurfaceFace, decltype(faceLess)> skirtSet(faceLess);
        for (const auto& f : skirtFaces) { skirtSet.insert(f); }
        for (const auto& f : gpuFaces) {
            auto it = skirtSet.find(f);
            if (it != skirtSet.end()) {
                ++gpuSkirtFaces;
                skirtSet.erase(it); // each GPU skirt consumes one CPU skirt slot (multiset)
            }
        }
    }

    // B1.3d: COUNT the cells the GPU SUPPRESSED (whole-cell skip) because their quadrant has
    // a resident finer child. This mirrors the shader's MidMeshCellSuppressedByChild exactly
    // (mergeCells==1 -> midCell==cx / midCellZ==cz; qx/qz from cellsPerRow; childMask bit
    // qz*2+qx). It is the "suppression removed faces" signal: > 0 confirms the new B1.3d code
    // path actually fired on this fixture. 0 when suppression is off.
    // B1.3f-a: count is over BLOCKS now (mergeCells>=1) using the BLOCK-CENTER quadrant rule
    // (midCell = x + (xEnd-x)/2), mirroring the shader exactly. mergeCells==1 -> one block
    // per cell -> byte-identical to the prior B1.3d count.
    uint32_t gpuSuppressedCells = 0u;
    if (applyChildSuppression && slot.tile.childMask != 0u && slot.tile.sampleSide > 1u) {
        const uint32_t cellsPerAxis = slot.tile.sampleSide - 1u;
        const uint32_t mergeCells = std::max(1u, slot.tile.mergeCells);
        const uint32_t blockCountPerAxis = (cellsPerAxis + mergeCells - 1u) / mergeCells;
        for (uint32_t bz = 0u; bz < blockCountPerAxis; ++bz) {
            const uint32_t z = bz * mergeCells;
            const uint32_t zEnd = std::min(cellsPerAxis, z + mergeCells);
            const uint32_t midCellZ = z + (zEnd - z) / 2u;
            const uint32_t qz = (midCellZ * 2u >= cellsPerAxis) ? 1u : 0u;
            for (uint32_t bx = 0u; bx < blockCountPerAxis; ++bx) {
                const uint32_t x = bx * mergeCells;
                const uint32_t xEnd = std::min(cellsPerAxis, x + mergeCells);
                const uint32_t midCell = x + (xEnd - x) / 2u;
                const uint32_t qx = (midCell * 2u >= cellsPerAxis) ? 1u : 0u;
                if ((slot.tile.childMask & (1u << (qz * 2u + qx))) != 0u) {
                    ++gpuSuppressedCells;
                }
            }
        }
    }

    // B1.3e: COUNT the cells the GPU EDIT-SKIPPED (whole-cell skip) because their world box
    // overlaps an edit box. Mirrors the shader's MidMeshCellInEditFootprint exactly (same
    // world-box corners cellWorldX/Z(cx..cx+1), same inclusive AABB overlap). > 0 confirms the
    // new B1.3e edit-skip path actually fired on this fixture. 0 when the edit skip is off or
    // no box is uploaded.
    // B1.3f-a: count is over BLOCKS now (mergeCells>=1) using the BLOCK world box
    // (cellWorldX/Z(x..xEnd)), mirroring the shader exactly. mergeCells==1 -> one block per
    // cell -> byte-identical to the prior B1.3e count.
    uint32_t gpuEditSkippedCells = 0u;
    const uint32_t editBoxCountForStats =
        static_cast<uint32_t>(slot.tile.editBoxes.size()); // capture before slot.tile is cleared
    const uint32_t mergeCellsForStats = slot.tile.mergeCells; // B1.3f-a: capture before clear
    if (applyEditSkip && !slot.tile.editBoxes.empty() && slot.tile.sampleSide > 1u) {
        const uint32_t cellsPerAxis = slot.tile.sampleSide - 1u;
        const int32_t cellSizeInt = std::max(1, slot.tile.cellSizeInt);
        const uint32_t mergeCells = std::max(1u, slot.tile.mergeCells);
        const uint32_t blockCountPerAxis = (cellsPerAxis + mergeCells - 1u) / mergeCells;
        for (uint32_t bz = 0u; bz < blockCountPerAxis; ++bz) {
            const uint32_t z = bz * mergeCells;
            const uint32_t zEnd = std::min(cellsPerAxis, z + mergeCells);
            const int32_t z0 = slot.tile.originZ + static_cast<int32_t>(z) * cellSizeInt;
            const int32_t z1 = slot.tile.originZ + static_cast<int32_t>(zEnd) * cellSizeInt;
            for (uint32_t bx = 0u; bx < blockCountPerAxis; ++bx) {
                const uint32_t x = bx * mergeCells;
                const uint32_t xEnd = std::min(cellsPerAxis, x + mergeCells);
                const int32_t x0 = slot.tile.originX + static_cast<int32_t>(x) * cellSizeInt;
                const int32_t x1 = slot.tile.originX + static_cast<int32_t>(xEnd) * cellSizeInt;
                for (const auto& b : slot.tile.editBoxes) {
                    if (b.minX <= x1 && b.maxX >= x0 && b.minZ <= z1 && b.maxZ >= z0) {
                        ++gpuEditSkippedCells;
                        break;
                    }
                }
            }
        }
    }

    // The CPU reference = this dispatch's captured FULL meshCacheFaces. For B1.3a-c
    // (Containment): every GPU face must appear in the CPU mesh, extraGpuFaces==0;
    // missingCpuFaces is EXPECTED (CPU has water/LOD the GPU defers). For B1.3d
    // (Equality, run with water+cull off so the only variable is suppression): the GPU now
    // emits the EXACT same set the CPU does - tops+risers+skirts MINUS the child-suppressed
    // quadrants - so missingCpuFaces MUST be 0 AND extraGpuFaces MUST be 0. A suppression
    // that removes too much -> missingCpuFaces>0; too little -> extraGpuFaces>0.
    const std::vector<Simulation::SparseSurfaceFace>& cpuFaces = slot.tile.cpuReferenceFaces;
    const MidMeshFaceAbMode abMode =
        equalityMode ? MidMeshFaceAbMode::Equal : MidMeshFaceAbMode::Containment;
    const MidMeshFaceAbResult ab = CompareMidMeshFacesMultiset(
        gpuFaces, cpuFaces, abMode, gpuStatus, 8u);
    // Label: b13fc if the camera-distance CULL was on (its own corpus label - the new increment).
    // Else b13fb if WATER + all-air-fill, else b13e if edit skip, else b13d if child suppression,
    // else b13c/b/a. (Cull takes the top label since it can stack on water + everything below.)
    const char* const modeLabel = applyDistanceCull
        ? "b13fc"
        : (emitWater
               ? "b13fb"
               : (applyEditSkip
                      ? "b13e"
                      : (applyChildSuppression
                             ? "b13d"
                             : (emitSkirts ? "b13c" : (emitRisers ? "b13b" : "b13a")))));
    LogMidMeshFaceAbResult(modeLabel, ab);

    slot.pending = false;
    slot.tile = {};
    m_smokeReadbackReadSlot = (m_smokeReadbackReadSlot + 1u) % kSmokeReadbackSlots;

    ++m_b13aAbVerifyCount;
    m_b13aStats.emitRisers = emitRisers;
    m_b13aStats.emitSkirts = emitSkirts;
    m_b13aStats.applyChildSuppression = applyChildSuppression;
    m_b13aStats.applyEditSkip = applyEditSkip;
    m_b13aStats.emitWater = emitWater;
    m_b13aStats.applyDistanceCull = applyDistanceCull;
    m_b13aStats.productionOutput = productionOutput;
    m_b13aStats.gpuCulledBlocks = culledBlocks;
    m_b13aStats.equalityMode = equalityMode;
    m_b13aStats.gpuSkirtFaces = gpuSkirtFaces;
    m_b13aStats.gpuSuppressedCells = gpuSuppressedCells;
    m_b13aStats.gpuEditSkippedCells = gpuEditSkippedCells;
    m_b13aStats.gpuWaterTopFaces = gpuWaterFaces;
    m_b13aStats.gpuAirFillFaces = gpuAirFillFaces;
    m_b13aStats.editBoxCount = editBoxCountForStats;
    m_b13aStats.gpuFaceCount = gpuFaceCount;
    m_b13aStats.gpuStatusOverflow = gpuStatus;
    m_b13aStats.verified = true;
    m_b13aStats.verifyCount = m_b13aAbVerifyCount;
    m_b13aStats.verifyMatched = ab.match ? 1u : 0u;
    m_b13aStats.verifyMismatch = ab.match ? 0u : 1u;
    m_b13aStats.abGpuFaceCount = ab.gpuFaceCount;
    m_b13aStats.abGpuUniqueFaceCount = ab.gpuUniqueFaceCount;
    m_b13aStats.abMissingCpuFaces = ab.missingCpuFaces;
    m_b13aStats.abExtraGpuFaces = ab.extraGpuFaces;
    m_b13aStats.abMultiplicityMismatches = ab.multiplicityMismatches;
    m_b13aStats.abOverflow = ab.overflow;

    // Use ab.cpuFaceCount (a captured value), NOT cpuFaces.size() - cpuFaces aliases
    // slot.tile.cpuReferenceFaces which was just cleared by `slot.tile = {}` above.
    // gpuRiserFaces > 0 is part of the B1.3b gate (the increment really added risers);
    // gpuSuppressedCells > 0 is the B1.3d gate (suppression actually removed faces).
    // For EQUALITY mode the gate is missing==0 AND extra==0 (gpuFaces==cpuMeshFaces).
    spdlog::info(
        "B13A_VERIFY mode={} abMode={} productionOutput={} abVerifyCount={} match={} equalityHeld={} containSubset={} "
        "mergeCells={} emitWater={} applyDistanceCull={} culledBlocks={} gpuFaces={} gpuRiserFaces={} gpuSkirtFaces={} "
        "gpuWaterFaces={} gpuAirFillFaces={} maxTopQuadW={} maxTopQuadH={} "
        "gpuSuppressedCells={} gpuEditSkippedCells={} "
        "editBoxes={} cpuMeshFaces={} extraGpuFaces={} missingCpuFaces={} multiplicityMismatches={} "
        "gpuStatus={}",
        modeLabel, equalityMode ? "equal" : "contain", productionOutput ? 1 : 0,
        m_b13aAbVerifyCount, ab.match ? 1 : 0,
        (equalityMode && ab.missingCpuFaces == 0u && ab.extraGpuFaces == 0u &&
         ab.multiplicityMismatches == 0u && gpuStatus == 0u) ? 1 : 0,
        (ab.extraGpuFaces == 0u && gpuStatus == 0u) ? 1 : 0,
        mergeCellsForStats, emitWater ? 1 : 0, applyDistanceCull ? 1 : 0, culledBlocks,
        gpuFaceCount, gpuRiserFaces, gpuSkirtFaces,
        gpuWaterFaces, gpuAirFillFaces, gpuMaxTopQuadW, gpuMaxTopQuadH,
        gpuSuppressedCells, gpuEditSkippedCells,
        editBoxCountForStats, ab.cpuFaceCount,
        ab.extraGpuFaces, ab.missingCpuFaces, ab.multiplicityMismatches, gpuStatus);
    return true;
}

} // namespace VENPOD::Graphics
