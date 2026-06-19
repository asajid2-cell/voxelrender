#include "MidMeshGpuExtractResources.h"

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
    m_smokeFaceReadback.Shutdown();
    m_smokeMetaReadback.Shutdown();
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
    m_smokeVerifyDone = false;
    m_smokeReady = false;
    m_smokeTimestampFrequency = 0;
    m_smokeFaceCapacityPerTile = 0;
    m_smokeFaceBufferBytes = 0;
    m_smokeControlled = {};
    m_smokeStats = {};

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
    const std::filesystem::path& shaderPath)
{
    if (!m_stats.initialized) {
        return Error("InitializeSmoke - B1.1 resources not initialized");
    }
    if (!device) {
        return Error("InitializeSmoke - device is null");
    }

    m_smokeFaceCapacityPerTile = std::max(1u, m_config.smokeFaceCapacityPerTile);
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
    // (baseFace=0). This is NOT the production mid-mesh face buffer.
    m_smokeFaceBufferBytes =
        static_cast<uint64_t>(m_smokeFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace);
    if (auto r = m_smokeFaceBuffer.Initialize(
            device, m_smokeFaceBufferBytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
            sizeof(Simulation::SparseSurfaceFace),
            "MidMeshGpuExtractSmokeFaces"); !r) {
        return Error("InitializeSmoke - debug face buffer: {}", r.error());
    }

    // Readback buffers (debug-only verify path): one controlled tile's worth.
    if (auto r = m_smokeFaceReadback.Initialize(
            device,
            static_cast<uint64_t>(m_smokeFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace),
            BufferUsage::Readback, sizeof(Simulation::SparseSurfaceFace),
            "MidMeshGpuExtractSmokeFaceReadback"); !r) {
        return Error("InitializeSmoke - face readback: {}", r.error());
    }
    if (auto r = m_smokeMetaReadback.Initialize(
            device, sizeof(MidMeshGpuExtractTileMeta),
            BufferUsage::Readback, sizeof(MidMeshGpuExtractTileMeta),
            "MidMeshGpuExtractSmokeMetaReadback"); !r) {
        return Error("InitializeSmoke - meta readback: {}", r.error());
    }

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
        "maxCells={} (ISOLATED - production face buffer + draw path untouched)",
        static_cast<double>(m_smokeFaceBufferBytes) / (1024.0 * 1024.0),
        m_smokeFaceCapacityPerTile, m_config.smokeMaxCells);
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

    // Reuse the persistent command allocator + list (created once in InitializeSmoke);
    // the previous dispatch's fence was already waited, so it is safe to reset.
    if (!m_smokeCmdAllocator || !m_smokeCmdList) {
        spdlog::error("[GPU_EXTRACT_SMOKE] command objects missing");
        return false;
    }
    if (FAILED(m_smokeCmdAllocator->Reset()) ||
        FAILED(m_smokeCmdList->Reset(m_smokeCmdAllocator.Get(), nullptr))) {
        spdlog::error("[GPU_EXTRACT_SMOKE] command list reset failed");
        return false;
    }
    ID3D12GraphicsCommandList* const list = m_smokeCmdList.Get();

    const bool haveTimestamps = (m_smokeQueryHeap != nullptr);
    // Debug-only verify readback: this RHI reliably maps a readback resource only ONCE
    // per process, so we capture the GPU->CPU verify on the FIRST dispatch only (one
    // authoritative SMOKE_VERIFY proving the GPU wrote the expected faces). After that
    // the dispatch + commit gate + GPU timestamps still run every frame; only the
    // (optional, debug-only) readback copy + map is skipped. The pipeline never depends
    // on the readback, so this is purely a validation-budget decision.
    const bool recordReadback = !m_smokeVerifyDone;

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

    // Copy the controlled tile's debug faces + metadata into the readback buffers as
    // part of THIS submission, so the (delayed, debug-only) verify only needs to map.
    // Recorded ONLY until the first authoritative SMOKE_VERIFY is captured (this RHI
    // reliably maps a readback resource once per process); after that the dispatch +
    // commit gate + timestamps still run every frame. The pipeline never needs this.
    if (recordReadback) {
        m_smokeFaceBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_smokeMetaBuffer.TransitionTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(
            m_smokeFaceReadback.GetResource(), 0,
            m_smokeFaceBuffer.GetResource(), 0,
            static_cast<uint64_t>(m_smokeFaceCapacityPerTile) * sizeof(Simulation::SparseSurfaceFace));
        list->CopyBufferRegion(
            m_smokeMetaReadback.GetResource(), 0,
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

    // ---- commit gate (validate; nothing is drawn) ----
    // (a) GPU work completed -> wait this dispatch's fence (the per-slot fence guard).
    if (m_smokeFence->GetCompletedValue() < fence) {
        m_smokeFence->SetEventOnCompletion(fence, m_smokeFenceEvent);
        WaitForSingleObject(m_smokeFenceEvent, INFINITE);
    }
    ctl.fenceValue = fence;
    (void)commandQueue; // main render queue intentionally untouched by the smoke path

    // (b) serial still matches what we dispatched -> usable; else discard-stale.
    const bool stale = (currentTileVersionForSlot != ctl.version);

    // Resolve timestamps now that the fence is complete (every frame).
    double dispatchUs = 0.0, barrierUs = 0.0;
    if (haveTimestamps && m_smokeTimestampFrequency != 0u) {
        if (const auto* ts = static_cast<const uint64_t*>(m_smokeQueryReadback.Map())) {
            const double toUs = 1.0e6 / static_cast<double>(m_smokeTimestampFrequency);
            dispatchUs = (ts[1] >= ts[0]) ? static_cast<double>(ts[1] - ts[0]) * toUs : 0.0;
            barrierUs = (ts[2] >= ts[1]) ? static_cast<double>(ts[2] - ts[1]) * toUs : 0.0;
            m_smokeQueryReadback.Unmap();
        }
    }
    const double cpuSubmitUs =
        std::chrono::duration<double, std::micro>(cpuSubmitStop - cpuSubmitStart).count();

    // Record stats.
    m_smokeStats = {};
    m_smokeStats.dispatched = true;
    m_smokeStats.tileSlot = ctl.slot;
    m_smokeStats.controlledTiles = 1u;
    m_smokeStats.expectedFaceCount = ctl.expectedCount;
    m_smokeStats.gpuFaceCount = UINT32_MAX;   // unread until PollSmokeReadback
    m_smokeStats.gpuStatusOverflow = 0u;
    m_smokeStats.dispatchedVersion = ctl.version;
    m_smokeStats.commitGatePassed = !stale;   // fence already waited (done) + serial check
    m_smokeStats.commitGateStale = stale;
    m_smokeStats.dispatchGpuUs = dispatchUs;
    m_smokeStats.barrierGpuUs = barrierUs;
    m_smokeStats.cpuSubmitUs = cpuSubmitUs;
    m_smokeStats.verified = false;
    m_smokeStats.verifyMatched = 0;
    m_smokeStats.verifyMismatch = 0;

    m_smokeControlled = std::move(ctl);

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
    (void)completedFenceValue; // smoke uses its own dedicated fence (waited in dispatch)
    // One authoritative verify: the readback copy is only RECORDED on the first dispatch
    // (recordReadback == !m_smokeVerifyDone), so only poll while that holds. The smoke
    // dispatch already waited its dedicated fence, so the readback data is ready.
    if (!m_smokeReady || !m_smokeControlled.valid || m_smokeVerifyDone) {
        return false;
    }

    // Read the GPU-written metadata faceCount/status for the controlled tile (the smoke
    // dispatch fence is complete, so this first-map of the readback resource is coherent).
    uint32_t gpuFaceCount = 0;
    uint32_t gpuStatus = 0;
    if (const auto* meta =
            static_cast<const MidMeshGpuExtractTileMeta*>(m_smokeMetaReadback.Map())) {
        gpuFaceCount = meta->faceCount;
        gpuStatus = meta->statusOverflow;
        m_smokeMetaReadback.Unmap();
    }

    // Recompute the deterministic expected faces CPU-side from the SAME samples.
    std::vector<Simulation::SparseSurfaceFace> expected;
    ComputeSmokeFacesCpu(m_smokeControlled, expected);
    const uint32_t expectedCount = static_cast<uint32_t>(expected.size());

    // Compare the GPU faces (deterministic cell-index order) against CPU.
    uint32_t matched = 0;
    uint32_t mismatch = 0;
    if (const auto* gpuFaces =
            static_cast<const Simulation::SparseSurfaceFace*>(m_smokeFaceReadback.Map())) {
        const uint32_t cmpCount = std::min(expectedCount, m_smokeFaceCapacityPerTile);
        for (uint32_t i = 0; i < cmpCount; ++i) {
            const auto& g = gpuFaces[i];
            const auto& e = expected[i];
            if (g.worldX == e.worldX && g.worldY == e.worldY &&
                g.worldZ == e.worldZ && g.payload == e.payload) {
                ++matched;
            } else {
                ++mismatch;
                if (mismatch <= 4u) {
                    spdlog::warn(
                        "[SMOKE_VERIFY] face[{}] MISMATCH gpu=({},{},{},0x{:08X}) "
                        "cpu=({},{},{},0x{:08X})",
                        i, g.worldX, g.worldY, g.worldZ, g.payload,
                        e.worldX, e.worldY, e.worldZ, e.payload);
                }
            }
        }
        m_smokeFaceReadback.Unmap();
    }

    m_smokeStats.gpuFaceCount = gpuFaceCount;
    m_smokeStats.gpuStatusOverflow = gpuStatus;
    m_smokeStats.verified = true;
    m_smokeStats.verifyMatched = matched;
    m_smokeStats.verifyMismatch = mismatch;
    // One authoritative verify captured; stop recording further readback copies (this
    // RHI reliably maps a readback resource once, and the proof is now established).
    m_smokeVerifyDone = true;

    const bool countOk = (gpuFaceCount == expectedCount);
    spdlog::info(
        "SMOKE_VERIFY ok={} mismatch={} count={} expected={} gpuStatus={} countMatch={}",
        matched, mismatch, gpuFaceCount, expectedCount, gpuStatus,
        countOk ? 1 : 0);
    return true;
}

} // namespace VENPOD::Graphics
