#include "MidMeshGpuExtractResources.h"

#include "MidMeshFaceAbCompare.h"
#include "RHI/DX12CommandQueue.h"

#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace VENPOD::Graphics {

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
    return {};
}

void MidMeshGpuExtractResources::Shutdown() {
    // B1.2 smoke (compute side).
    m_smokePipeline.Shutdown();
    m_smokeInputUpload.Shutdown();
    m_smokeSampleBuffer.Shutdown();
    m_smokeMetaBuffer.Shutdown();
    m_smokeFaceBuffer.Shutdown();
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
        slot.faceReadback.Shutdown();
        slot.metaReadback.Shutdown();
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
        // Pending output range = metadata ONLY for B1.1: record a per-tile reserved
        // range (CPU face count + margin). No allocation logic is exercised, no faces
        // are written. baseFace is the tile's slot region in a hypothetical per-slot
        // face arena (slot * capacity) - a stable, non-overlapping reservation so a
        // pending range can never overwrite a resident one. faceCount stays 0 (no CS).
        meta.faceCapacity = tile.faceCount + m_config.faceCapacityMargin;
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

    // B1.3.0 readback RING (debug-only verify path): kSmokeReadbackSlots slots, each
    // a face + metadata readback buffer Map()'d ONCE here and kept persistently mapped
    // (the CPU pointer is held for the process lifetime; never per-frame Map/Unmap).
    // A per-slot fence value (recorded at dispatch) tracks GPU-copy completion, so a
    // slot is read only after its fence is satisfied. This makes the verify REPEATABLE
    // across frames, not once per process.
    const uint64_t faceReadbackBytes =
        static_cast<uint64_t>(m_debugFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace);
    for (uint32_t i = 0; i < kSmokeReadbackSlots; ++i) {
        char faceName[64];
        char metaName[64];
        std::snprintf(faceName, sizeof(faceName), "MidMeshGpuExtractSmokeFaceReadback%u", i);
        std::snprintf(metaName, sizeof(metaName), "MidMeshGpuExtractSmokeMetaReadback%u", i);
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
        // Persistent map: Map() once, hold the pointer. A readback heap is system
        // memory; the runtime makes a completed GPU copy visible to this pointer after
        // the slot's fence is satisfied, so we never re-Map per frame.
        slot.facePtr = slot.faceReadback.Map();
        slot.metaPtr = slot.metaReadback.Map();
        if (!slot.facePtr || !slot.metaPtr) {
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
        "maxCells={} readbackRingSlots={} readbackForceWait={} "
        "(ISOLATED - production face buffer + draw path untouched)",
        static_cast<double>(m_smokeFaceBufferBytes) / (1024.0 * 1024.0),
        m_smokeFaceCapacityPerTile, m_config.smokeMaxCells,
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
    //   b0: 5x 32-bit constants {tileSlot, debugBaseFace, faceCapacityPerTile,
    //       terraceStep, emitRisers}  (emitRisers: 0 = B1.3a top only, 1 = B1.3b + risers)
    //   t0: samples (root SRV)   u0: debug faces (root UAV)   u1: metadata (root UAV)
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
    desc.rootParams.push_back({ RootParamType::Constants32Bit, 0, 0, 5 }); // b0
    desc.rootParams.push_back({ RootParamType::ShaderResource, 0, 0, 1 }); // t0 samples
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 0, 0, 1 }); // u0 faces
    desc.rootParams.push_back({ RootParamType::UnorderedAccess, 1, 0, 1 }); // u1 metadata
    if (auto r = m_topFacePipeline.Initialize(device, desc); !r) {
        return Error("InitializeB13aTopFace - PSO init: {}", r.error());
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

bool MidMeshGpuExtractResources::RunB13aTopFaceDispatch(
    ID3D12Device* device,
    const Simulation::MidMeshGpuExtractDirtyTile& fixture,
    const std::vector<Simulation::SparseSurfaceFace>& cpuReferenceFaces,
    uint32_t terraceStep,
    uint64_t currentTileVersionForSlot,
    bool emitRisers)
{
    if (!m_b13aReady || !device) {
        return false;
    }
    if (fixture.slot >= m_config.maxTiles || fixture.samples == nullptr ||
        fixture.sampleCount == 0u) {
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
    ctl.slot = 0u;       // the dedicated smoke buffers always use slot 0 / baseFace 0
    ctl.baseFace = 0u;
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
        meta.editFootprintCount = 0u;
        meta.faceCapacity = m_topFaceCapacityPerTile;
        meta.baseFace = 0u;
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

    m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_topFacePipeline.Bind(list);
    const uint32_t consts[5] = {
        ctl.slot, ctl.baseFace, m_topFaceCapacityPerTile, m_b13aBuildTerraceStep,
        emitRisers ? 1u : 0u
    };
    m_topFacePipeline.SetRoot32BitConstants(list, 0, 5, consts);
    list->SetComputeRootShaderResourceView(1, m_smokeSampleBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, m_smokeFaceBuffer.GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, m_smokeMetaBuffer.GetGPUVirtualAddress());

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

    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = m_smokeFaceBuffer.GetResource();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = m_smokeMetaBuffer.GetResource();
    list->ResourceBarrier(2, uavBarriers);
    if (haveTimestamps) {
        list->EndQuery(m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    }

    // Copy this dispatch's faces + metadata into the current ring slot (persistently
    // mapped, fence-tracked) so a later poll reads it back. Copy the FULL top-face
    // capacity (the poll clamps to the GPU-written faceCount).
    m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(
        ringSlot.faceReadback.GetResource(), 0,
        m_smokeFaceBuffer.GetResource(), 0,
        static_cast<uint64_t>(m_topFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace));
    list->CopyBufferRegion(
        ringSlot.metaReadback.GetResource(), 0,
        m_smokeMetaBuffer.GetResource(), 0,
        sizeof(MidMeshGpuExtractTileMeta));
    if (haveTimestamps && m_smokeQueryReadback.GetResource()) {
        list->ResolveQueryData(
            m_smokeQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3,
            m_smokeQueryReadback.GetResource(), 0);
    }

    m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);
    m_smokeSampleBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COMMON);

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

    spdlog::info(
        "GPU_EXTRACT_B13A mode={} fixtureSlot={} mergeCells={} childMask={} cpuRefFaces={} "
        "dispatchGpuUs={:.2f} barrierGpuUs={:.2f} cpuSubmitUs={:.2f} commitGate={} "
        "dispatchedVersion={} currentVersion={}",
        emitRisers ? "b13b" : "b13a",
        fixture.slot, fixture.mergeCells, fixture.childMask,
        m_b13aStats.cpuRefFaceCount, dispatchUs, barrierUs, cpuSubmitUs,
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
        gpuFaceCount = meta->faceCount;       // the per-tile append counter's final value
        gpuStatus = meta->statusOverflow;
    }

    // Gather the GPU top faces the shader actually wrote (clamped to capacity; on overflow
    // the result is rejected by the harness so we read nothing meaningful).
    std::vector<Simulation::SparseSurfaceFace> gpuFaces;
    if (slot.facePtr) {
        const auto* faces = static_cast<const Simulation::SparseSurfaceFace*>(slot.facePtr);
        const uint32_t emitted =
            (gpuStatus == 0u) ? std::min(gpuFaceCount, m_topFaceCapacityPerTile) : 0u;
        gpuFaces.assign(faces, faces + emitted);
    }

    // B1.3b: a GPU RISER is any emitted face whose packed direction is not PosY (the top).
    // Count them so the gate can require riser faces > 0 (the increment really added risers).
    const bool emitRisers = slot.tile.emitRisers;
    uint32_t gpuRiserFaces = 0u;
    for (const auto& f : gpuFaces) {
        if (Simulation::SparseSurfacePayloadDirection(f.payload) !=
            static_cast<uint32_t>(Simulation::SparseFaceDirection::PosY)) {
            ++gpuRiserFaces;
        }
    }

    // The CPU reference = this dispatch's captured FULL meshCacheFaces (tops + risers +
    // skirts). CONTAINMENT: every GPU face (top + B1.3b risers) must appear in the CPU mesh
    // with >= its multiplicity, and extraGpuFaces MUST be 0 (a GPU face absent from the CPU
    // mesh is a bug). missingCpuFaces is EXPECTED (the CPU still has border skirts / water /
    // LOD the GPU does not emit) but is SMALLER than B1.3a once risers are covered.
    const std::vector<Simulation::SparseSurfaceFace>& cpuFaces = slot.tile.cpuReferenceFaces;
    const MidMeshFaceAbResult ab = CompareMidMeshFacesMultiset(
        gpuFaces, cpuFaces, MidMeshFaceAbMode::Containment, gpuStatus, 8u);
    LogMidMeshFaceAbResult(emitRisers ? "b13b" : "b13a", ab);

    slot.pending = false;
    slot.tile = {};
    m_smokeReadbackReadSlot = (m_smokeReadbackReadSlot + 1u) % kSmokeReadbackSlots;

    ++m_b13aAbVerifyCount;
    m_b13aStats.emitRisers = emitRisers;
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
    // gpuRiserFaces > 0 is part of the B1.3b gate (the increment really added risers).
    spdlog::info(
        "B13A_VERIFY mode={} abVerifyCount={} match={} containSubset={} gpuFaces={} "
        "gpuRiserFaces={} cpuMeshFaces={} extraGpuFaces={} missingCpuFaces={} "
        "multiplicityMismatches={} gpuStatus={}",
        emitRisers ? "b13b" : "b13a",
        m_b13aAbVerifyCount, ab.match ? 1 : 0,
        (ab.extraGpuFaces == 0u && gpuStatus == 0u) ? 1 : 0,
        gpuFaceCount, gpuRiserFaces, ab.cpuFaceCount,
        ab.extraGpuFaces, ab.missingCpuFaces, ab.multiplicityMismatches, gpuStatus);
    return true;
}

} // namespace VENPOD::Graphics
