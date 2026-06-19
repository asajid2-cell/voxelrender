#pragma once

// =============================================================================
// VENPOD GPU mid-mesh extraction - Phase B1.1 (INPUT side only)
// =============================================================================
// SHADOW infrastructure for the render-only GPU mesh-extraction pipeline
// (GPU_VISUAL_EXTRACTION_PLAN.md, sub-step B1.1). This stands up ONLY the
// CPU->GPU INPUT side so a later compute shader (B1.3) can read it:
//
//   * a PERSISTENT per-tile height-SAMPLE buffer (side*side uint32 per tile slot),
//   * a PERSISTENT per-tile METADATA buffer (one hard-ABI struct per slot),
//   * a small per-frame UPLOAD RING for the dirty sample + metadata copies.
//
// It performs NO compute dispatch, NO face output, and NO readback. It uploads
// ONLY the tiles that went dirty in the build that just ran (the same dirty set
// the CPU incremental path tracks), keyed by the cache's stable tile slot, so
// per-frame upload scales with the DIRTY count, not the resident tile count.
//
// The metadata struct is a HARD ABI: explicit fixed-width fields, uint4-friendly,
// static_assert-pinned, mirroring the SparseSurfaceFace ABI discipline so a future
// HLSL struct must match it byte-for-byte.
// =============================================================================

#include "RHI/DescriptorHeap.h"
#include "RHI/DX12ComputePipeline.h"
#include "RHI/GPUBuffer.h"
#include "RHI/ShaderCompiler.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseSurfaceExtractor.h"
#include "Utils/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace VENPOD::Graphics {

class DX12CommandQueue;

// -----------------------------------------------------------------------------
// Per-tile GPU extraction metadata - HARD ABI (CPU <-> future HLSL compute).
// -----------------------------------------------------------------------------
// 80 bytes = 5x uint4 (16-byte aligned, uint4-friendly). EXACTLY mirrors what a
// future CS_MidMeshExtract must read; a one-field drift reads as random geometry.
// Keep this layout byte-identical with the HLSL struct that lands in B1.2/B1.3.
//
// Height packing descriptor (do NOT invent - this records how the CPU packs each
// sample uint32 today, from PackMidHeightSurfaceVoxel / UnpackMidHeightSurfaceSampleY
// / UnpackMidHeightSurfaceSampleMaterial in SparseClipmap.cpp):
//   sample.Y       = int32((sample & 0xFFFF) - 32768)   -> bits 0..15, bias 32768
//   sample.material = uint8((sample >> 16) & 0xFF)        -> bits 16..23
//   bits 24..31    = unused / reserved
// The descriptor fields below carry those shifts/masks/bias explicitly so the GPU
// side never has to hardcode them.
struct MidMeshGpuExtractTileMeta {
    // uint4[0]: synthetic tile coord {x, ring, z} + originX
    int32_t coordX = 0;
    int32_t coordRing = 0;
    int32_t coordZ = 0;
    int32_t originX = 0;
    // uint4[1]: originZ + cellSize(bits) + LOD merge-cells + finer-child mask
    int32_t originZ = 0;
    uint32_t cellSizeBits = 0;   // float cellSize reinterpreted as uint32
    uint32_t mergeCells = 0;     // camera-distance LOD merge-cell size for this build
    uint32_t childMask = 0;      // finer-ring child residency (4 bits)
    // uint4[2]: per-tile content serial (64-bit lo/hi) + sample side + sample stride
    uint32_t meshContentVersionLo = 0;
    uint32_t meshContentVersionHi = 0;
    uint32_t sampleSide = 0;     // side (grid is side*side)
    uint32_t sampleStride = 0;   // row stride in samples (== side for now)
    // uint4[3]: halo width (reserved=0) + height/material packing descriptor + edits
    uint32_t haloWidth = 0;      // RESERVED for B1.3 border/skirt samples (0 for now)
    // heightPackDesc packs the sample bit layout (see header above):
    //   bits 0..4   = heightShift   (0)
    //   bits 5..9   = heightBits    (16)
    //   bits 10..14 = materialShift (16)
    //   bits 15..19 = materialBits  (8)
    uint32_t heightPackDesc = 0;
    uint32_t heightBias = 0;        // 32768 (added back to the unsigned height field)
    uint32_t editFootprintCount = 0; // RESERVED: per-tile edited-cell count (full mask later)
    // uint4[4]: pending output range (metadata only for B1.1) + status/overflow
    uint32_t baseFace = 0;          // reserved pending face range base (no faces written)
    uint32_t faceCapacity = 0;      // CPU per-tile face count + margin (capacity bound)
    uint32_t faceCount = 0;         // GPU-written actual count (0 in B1.1, no shader)
    uint32_t statusOverflow = 0;    // status / overflow flag (0 in B1.1)
};

static_assert(sizeof(MidMeshGpuExtractTileMeta) == 80,
    "MidMeshGpuExtractTileMeta ABI = 5x uint4 (80 bytes); HLSL must match byte-for-byte");
static_assert(sizeof(MidMeshGpuExtractTileMeta) % 16u == 0u,
    "metadata struct must be uint4-aligned");
static_assert(alignof(MidMeshGpuExtractTileMeta) <= 16u,
    "metadata struct must not exceed uint4 alignment");

// Height packing descriptor field accessors (CPU mirror of the future HLSL decode).
inline constexpr uint32_t MidMeshSampleHeightShift = 0u;
inline constexpr uint32_t MidMeshSampleHeightBits = 16u;
inline constexpr uint32_t MidMeshSampleMaterialShift = 16u;
inline constexpr uint32_t MidMeshSampleMaterialBits = 8u;
inline constexpr uint32_t MidMeshSampleHeightBias = 32768u;

inline uint32_t PackMidMeshHeightDesc() {
    return (MidMeshSampleHeightShift & 0x1Fu) |
        ((MidMeshSampleHeightBits & 0x1Fu) << 5u) |
        ((MidMeshSampleMaterialShift & 0x1Fu) << 10u) |
        ((MidMeshSampleMaterialBits & 0x1Fu) << 15u);
}

struct MidMeshGpuExtractConfig {
    uint32_t maxTiles = 128;        // resident tile-slot capacity (cache m_config.maxTiles)
    uint32_t tileSampleSide = 33;   // side; per-tile region = side*side uint32
    uint32_t uploadRingSlots = 3;   // per-frame upload-ring depth (matches the present ring)
    uint32_t faceCapacityMargin = 64; // extra faces reserved past the CPU count (B1.1: bookkeeping only)
    // B1.2 SMOKE: a fixed per-slot reserved face range for the ISOLATED debug-output
    // face buffer (NOT the production mid-mesh face buffer). Uniform per-slot so
    // baseFace = slot * smokeFaceCapacityPerTile is non-overlapping + stable. The smoke
    // CS writes only the first smokeMaxCells top quads (<= this capacity).
    uint32_t smokeFaceCapacityPerTile = 256u;
    uint32_t smokeMaxCells = 16u;   // K: top quads the smoke shader writes per tile
};

struct MidMeshGpuExtractStats {
    bool initialized = false;
    uint32_t maxTiles = 0;
    uint32_t sampleSide = 0;
    uint32_t samplesPerTile = 0;
    uint64_t sampleBufferBytes = 0;
    uint64_t metadataBufferBytes = 0;
    uint64_t uploadRingSlotBytes = 0;
    // Per-frame (last upload):
    uint32_t sampleUploadTiles = 0;     // tiles whose samples+metadata were copied
    uint64_t sampleUploadBytes = 0;     // samples + metadata bytes copied this frame
    uint32_t skippedNoSlotTiles = 0;    // dirty coords with no resident slot (evicted)
    uint32_t deferredRingFullTiles = 0; // dirty tiles deferred because the ring slot was full
    bool uploadRingWaitSkipped = false; // active ring slot still in flight -> upload skipped
};

// -----------------------------------------------------------------------------
// B1.2 SMOKE: per-dispatch result + a captured snapshot of the ONE controlled tile
// so the CPU can recompute the deterministic face pattern the shader should produce.
// -----------------------------------------------------------------------------
struct MidMeshGpuExtractSmokeStats {
    bool dispatched = false;        // a smoke dispatch was recorded this run
    uint32_t tileSlot = UINT32_MAX; // controlled tile's slot
    uint32_t controlledTiles = 0;   // controlled tile count (1 for B1.2)
    uint32_t expectedFaceCount = 0; // CPU-predicted face count for the controlled tile
    uint32_t gpuFaceCount = 0;      // GPU-written faceCount (read back; UINT32_MAX = unread)
    uint32_t gpuStatusOverflow = 0; // GPU-written status (0 ok / 1 overflow)
    // Commit gate (no-hole publication logic; nothing is actually drawn):
    bool commitGatePassed = false;  // fence done AND meshContentVersion still matches
    bool commitGateStale = false;   // dispatched serial != current tile serial -> discard
    uint64_t dispatchedVersion = 0; // meshContentVersion captured at dispatch
    // Timing (microseconds):
    double dispatchGpuUs = 0.0;
    double barrierGpuUs = 0.0;
    double cpuSubmitUs = 0.0;
    // Verify (filled by the delayed readback):
    bool verified = false;
    uint32_t verifyMatched = 0;     // faces that matched the CPU recompute
    uint32_t verifyMismatch = 0;
};

// Per-frame upload ticket: the staged copy regions for this frame's dirty tiles.
// EmitCopy() consumes it on the command list. Kept separate from Stage so a caller
// can decide not to emit (B1.1 always emits when staged).
struct MidMeshGpuExtractTicket {
    bool valid = false;
    uint32_t ringSlot = 0;
    uint32_t tileCount = 0;
    uint64_t sampleBytes = 0;
    uint64_t metadataBytes = 0;
    // CopyBufferRegion descriptors into the persistent default buffers.
    struct SampleCopy {
        uint64_t uploadOffset = 0;  // byte offset in the ring upload buffer
        uint64_t destOffset = 0;    // byte offset in the persistent sample buffer
        uint64_t byteCount = 0;
    };
    struct MetaCopy {
        uint64_t uploadOffset = 0;  // byte offset in the ring upload buffer
        uint32_t slot = 0;          // dest slot in the metadata buffer
    };
    std::vector<SampleCopy> sampleCopies;
    std::vector<MetaCopy> metaCopies;
    uint64_t fenceValue = 0;        // fence this frame's copies are submitted under
};

class MidMeshGpuExtractResources {
public:
    MidMeshGpuExtractResources() = default;
    ~MidMeshGpuExtractResources();

    MidMeshGpuExtractResources(const MidMeshGpuExtractResources&) = delete;
    MidMeshGpuExtractResources& operator=(const MidMeshGpuExtractResources&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        const MidMeshGpuExtractConfig& config);
    void Shutdown();

    // Advance the upload ring. completedFenceValue = last GPU-completed fence;
    // currentFrameFenceValue = the fence this frame's work will signal. Used to
    // avoid overwriting a ring slot whose previous copy has not yet completed.
    void BeginFrame(uint32_t frameIndex, uint64_t completedFenceValue, uint64_t currentFrameFenceValue);

    // Stage the dirty tiles' samples + metadata into the active ring upload buffer
    // and build the copy ticket. Returns false (and stats reflect why) if there is
    // nothing to upload or the ring slot is still in flight.
    bool StageDirtyTiles(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        MidMeshGpuExtractTicket* outTicket = nullptr);

    // Emit the staged CopyBufferRegion commands (samples + metadata) on the list.
    bool EmitCopy(ID3D12GraphicsCommandList* commandList, const MidMeshGpuExtractTicket& ticket);

    bool IsInitialized() const { return m_stats.initialized; }
    const MidMeshGpuExtractStats& GetStats() const { return m_stats; }
    const DescriptorHandle& SampleBufferSRV() const { return m_sampleBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& MetadataBufferSRV() const { return m_metadataBuffer.GetShaderVisibleSRV(); }

    // =========================================================================
    // B1.2 SMOKE - minimal compute proof (gated by VENPOD_MIDMESH_GPU_EXTRACT_SMOKE)
    // =========================================================================
    // Stands up the compute side: an ISOLATED debug-output SparseSurfaceFace UAV
    // buffer + the smoke compute PSO. NEVER touches the production mid-mesh face
    // buffer or the draw path. Must be called after Initialize().
    Result<void> InitializeSmoke(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath);
    bool SmokeReady() const { return m_smokeReady; }

    // Run ONE smoke dispatch for the FIRST controlled (resident-slot) dirty tile in
    // `dirtyTiles`: the smoke CS reads that tile's metadata + samples from the
    // persistent buffers (already uploaded by EmitCopy on `commandQueue`'s timeline)
    // and writes K deterministic top quads into the isolated debug face buffer's
    // reserved range. Runs on its own command list + fence (the per-slot fence
    // pattern), times the dispatch+barrier with GPU timestamps, and validates the
    // commit gate (fence done AND serial unchanged). Records into m_smokeStats and
    // captures the controlled tile so a later PollSmokeReadback() can verify it.
    // Returns true if a dispatch was issued.
    bool RunSmokeDispatch(
        ID3D12Device* device,
        DX12CommandQueue& commandQueue,
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        uint64_t currentTileVersionForSlot);

    // Delayed, DEBUG-ONLY readback of the last dispatch's controlled tile: copies the
    // debug face buffer's reserved range + the metadata faceCount/status back, then
    // recomputes the SAME deterministic pattern CPU-side and compares. Non-blocking:
    // only runs once the dispatch fence has completed. Returns true once a verify ran.
    // The pipeline FUNCTIONS without ever calling this - it is pure validation.
    bool PollSmokeReadback(uint64_t completedFenceValue);

    const MidMeshGpuExtractSmokeStats& GetSmokeStats() const { return m_smokeStats; }

private:
    // B1.2: CPU snapshot of the controlled tile, captured at dispatch, so the readback
    // can recompute the exact deterministic faces the shader was asked to produce.
    struct SmokeControlledTile {
        bool valid = false;
        uint32_t slot = UINT32_MAX;
        uint32_t baseFace = 0;          // reserved range base in the debug face buffer
        uint32_t expectedCount = 0;     // K (clamped to capacity)
        uint64_t version = 0;           // meshContentVersion at dispatch
        uint64_t fenceValue = 0;        // fence this dispatch signaled
        int32_t originX = 0;
        int32_t originZ = 0;
        int32_t cellSizeInt = 0;
        uint32_t sampleSide = 0;
        std::vector<uint32_t> samples;  // copy of the tile's packed samples
    };

    // Recompute the deterministic smoke faces CPU-side (mirror of the HLSL). Static so
    // the test/verify path and the doc stay in lockstep with the shader.
    static void ComputeSmokeFacesCpu(
        const SmokeControlledTile& tile,
        std::vector<Simulation::SparseSurfaceFace>& outFaces);

    MidMeshGpuExtractConfig m_config;
    MidMeshGpuExtractStats m_stats;

    GPUBuffer m_sampleBuffer;    // persistent, maxTiles * side*side uint32
    GPUBuffer m_metadataBuffer;  // persistent, maxTiles * MidMeshGpuExtractTileMeta
    std::array<UploadBuffer, 3> m_uploadRing;
    std::array<uint64_t, 3> m_uploadRingFence = {};

    uint32_t m_samplesPerTile = 0;
    uint64_t m_sampleTileBytes = 0;     // side*side*4
    uint64_t m_uploadRingSlotBytes = 0; // capacity of one ring slot
    uint32_t m_activeRingSlot = 0;
    uint64_t m_currentFrameFenceValue = 0;
    bool m_sampleBufferInCopyDest = false;
    bool m_metadataBufferInCopyDest = false;

    // ---- B1.2 SMOKE (compute side) ----
    bool m_smokeReady = false;
    DX12ComputePipeline m_smokePipeline;
    UploadBuffer m_smokeInputUpload; // one controlled tile's samples + metadata stage
    // DEDICATED, smoke-OWNED input buffers (one tile / one slot). Kept separate from the
    // B1.1 persistent sample/metadata buffers so the smoke's own command list never
    // shares DX12 state tracking with the frame command list's B1.1 EmitCopy (that
    // cross-list coupling silently corrupted reads on frames after the first).
    GPUBuffer m_smokeSampleBuffer;   // one tile's samples (StructuredBuffer, SRV-read)
    GPUBuffer m_smokeMetaBuffer;     // one slot's metadata (RWStructuredBuffer, UAV)
    GPUBuffer m_smokeFaceBuffer;     // ISOLATED RWStructuredBuffer<SparseSurfaceFace> (UAV)
    GPUBuffer m_smokeFaceReadback;   // CPU-visible copy of the controlled tile's range
    GPUBuffer m_smokeMetaReadback;   // CPU-visible copy of the controlled tile's metadata
    ComPtr<ID3D12QueryHeap> m_smokeQueryHeap;   // 3 timestamps: dispatch begin/end + barrier end
    GPUBuffer m_smokeQueryReadback;             // CPU-visible timestamp resolve target
    ComPtr<ID3D12CommandAllocator> m_smokeCmdAllocator; // reused per dispatch (created once)
    ComPtr<ID3D12GraphicsCommandList> m_smokeCmdList;   // reused per dispatch (created once)
    // DEDICATED queue + fence for the smoke submit. The smoke path runs every frame, so
    // it must NOT share the main render queue's fence/value counter (that desynchronized
    // the main per-frame fence ring and caused a device removal). This queue is touched
    // ONLY by the smoke dispatch, fully isolating its Signal/Wait.
    ComPtr<ID3D12CommandQueue> m_smokeQueue;
    ComPtr<ID3D12Fence> m_smokeFence;
    void* m_smokeFenceEvent = nullptr;
    uint64_t m_smokeFenceValue = 0;
    bool m_smokeVerifyDone = false; // one authoritative SMOKE_VERIFY has been captured
    uint64_t m_smokeTimestampFrequency = 0;
    uint32_t m_smokeFaceCapacityPerTile = 0;
    uint64_t m_smokeFaceBufferBytes = 0;
    SmokeControlledTile m_smokeControlled;       // last dispatch's controlled tile
    MidMeshGpuExtractSmokeStats m_smokeStats;
};

} // namespace VENPOD::Graphics
