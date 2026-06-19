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
#include <functional>
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
    // B1.3a/b TOP-FACE (+ RISER): per-tile reserved capacity for the REAL extraction. A
    // tile emits up to cellCount^2 solid top faces ((side-1)^2 ~= 1024 for side=33), each
    // possibly split for cellSize > 32, PLUS (B1.3b) up to ~2 risers per cell, each sliced
    // into 24u vertical segments + 32u chunks on a tall cliff. A stepped tile was measured
    // at ~4628+ faces (overflowing the old 4096), so size for the worst stepped tile with
    // headroom; overflow still sets the status flag (a rejected result, never an OOB write).
    uint32_t topFaceCapacityPerTile = 16384u;
    // B1.3e EDIT-FOOTPRINT: per-tile cap on the number of edited-cell XZ boxes uploaded to
    // the GPU. An edited tile in practice overlaps a handful of brush strokes; 256 is ample
    // headroom. If a tile exceeds it the dispatch is SKIPPED + reported (never a partial/OOB
    // upload), so a too-small cap surfaces as a logged overflow, not silent wrong geometry.
    uint32_t editBoxCapacityPerTile = 256u;
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
    // Verify (filled by the delayed readback ring + multiset A/B harness):
    bool verified = false;          // at least one AB_VERIFY has run this process
    uint32_t verifyMatched = 0;     // 1 if the last multiset compare passed, else 0
    uint32_t verifyMismatch = 0;    // 1 if the last multiset compare failed, else 0
    uint32_t verifyCount = 0;       // TOTAL AB_VERIFY comparisons across the run
                                    // (proves the readback RING works repeatedly).
    // Last multiset A/B result (B1.3.0):
    uint32_t abGpuFaceCount = 0;
    uint32_t abGpuUniqueFaceCount = 0;
    uint32_t abMissingCpuFaces = 0;
    uint32_t abExtraGpuFaces = 0;
    uint32_t abMultiplicityMismatches = 0;
    uint32_t abOverflow = 0;
    bool abSelfTestRun = false;     // AB_SELFTEST was executed
    bool abSelfTestPassed = false;  // AB_SELFTEST asserted the multiset logic bites
};

// -----------------------------------------------------------------------------
// B1.3a TOP-FACE: per-dispatch result of the REAL top-face extraction + its
// CONTAINMENT A/B against the CPU tile's meshCacheFaces (the ground-truth mesh).
// -----------------------------------------------------------------------------
struct MidMeshGpuExtractB13aStats {
    bool dispatched = false;        // a B1.3a/b/c/d top-face dispatch ran this frame
    bool emitRisers = false;        // B1.3b: this dispatch also emitted neighbor risers
    bool emitSkirts = false;        // B1.3c: this dispatch also emitted tile-border skirts
    bool applyChildSuppression = false; // B1.3d: child-quadrant suppression was active
    bool applyEditSkip = false;     // B1.3e: edit-footprint suppression was active
    bool emitWater = false;         // B1.3f-b: water-aware aggregation + all-air fill active
    bool applyDistanceCull = false; // B1.3f-c: camera-distance cull (CPU-decision mask) was active
    bool equalityMode = false;      // B1.3d: A/B ran in EQUALITY mode (not containment)
    uint32_t gpuCulledBlocks = 0;   // B1.3f-c: blocks the CPU mask flagged as culled (the GPU skipped)
    uint32_t gpuSkirtFaces = 0;     // B1.3c: GPU faces that are border-skirt side quads
    uint32_t gpuSuppressedCells = 0;// B1.3d: cells the GPU skipped due to child suppression
    uint32_t gpuEditSkippedCells = 0;// B1.3e: cells the GPU skipped due to the edit footprint
    uint32_t gpuWaterTopFaces = 0;  // B1.3f-b: GPU PosY top faces with the Water material
    uint32_t gpuAirFillFaces = 0;   // B1.3f-b: GPU faces from all-air sea-level fill blocks
    uint32_t editBoxCount = 0;      // B1.3e: edit boxes uploaded for this fixture
    uint32_t editBoxOverflow = 0;   // B1.3e: 1 if the tile exceeded editBoxCapacityPerTile
    uint64_t editBoxBytes = 0;      // B1.3e: edit-box bytes uploaded this dispatch
    uint32_t tileSlot = UINT32_MAX; // controlled (flat/simple) fixture slot
    uint32_t fixtureMergeCells = 0; // the fixture's mergeCells (must be 1)
    uint32_t fixtureChildMask = 0;  // the fixture's childMask (0 for B1.3a-c; !=0 for B1.3d)
    uint32_t cpuRefFaceCount = 0;   // CPU meshCacheFaces size (full mesh: tops+risers+skirts)
    uint32_t gpuFaceCount = 0;      // GPU-written top-face count (read back; UINT32_MAX=unread)
    uint32_t gpuStatusOverflow = 0; // GPU status (0 ok / 1 reserved-range overflow)
    uint64_t dispatchedVersion = 0; // meshContentVersion at dispatch
    bool commitGateStale = false;   // dispatched serial != current serial -> discard
    // Timing (microseconds):
    double dispatchGpuUs = 0.0;
    double barrierGpuUs = 0.0;
    double cpuSubmitUs = 0.0;
    // CONTAINMENT A/B (filled by the delayed readback ring + multiset harness):
    bool verified = false;          // at least one AB_VERIFY(contain) has run
    uint32_t verifyCount = 0;       // TOTAL containment comparisons (ring repeatability)
    uint32_t verifyMatched = 0;     // 1 if the last containment compare passed
    uint32_t verifyMismatch = 0;    // 1 if the last containment compare failed
    // Last containment result (the proof: extraGpuFaces==0 => GPU-top subset of CPU):
    uint32_t abGpuFaceCount = 0;
    uint32_t abGpuUniqueFaceCount = 0;
    uint32_t abMissingCpuFaces = 0;     // CPU faces the GPU did not emit (ALLOWED in contain)
    uint32_t abExtraGpuFaces = 0;       // GPU faces not in CPU (MUST be 0)
    uint32_t abMultiplicityMismatches = 0;
    uint32_t abOverflow = 0;
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
    // `readbackForceWait` (default false) gates the explicit CPU fence WAIT in the
    // readback poll: when true, the ISOLATED correctness path blocks until each slot's
    // fence completes (same-cycle compare). MUST be false for any timing run.
    // `b13aEnabled` sizes the ISOLATED debug face buffer + readback ring to also hold a
    // full B1.3a top-face tile (topFaceCapacityPerTile, larger than the smoke K), so the
    // B1.3a path can reuse this same isolated infrastructure.
    Result<void> InitializeSmoke(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath,
        bool readbackForceWait = false,
        bool b13aEnabled = false);
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

    // Delayed, DEBUG-ONLY readback via the fence-tracked RING. Consumes the OLDEST
    // pending ring slot whose copy fence has completed: reads that slot's persistently
    // mapped face + metadata pointers, recomputes the CPU reference for THAT dispatch,
    // and runs the multiset A/B harness -> AB_VERIFY. Works REPEATEDLY across frames
    // (one comparison per dispatch, a couple frames behind), NOT once per process.
    // Non-blocking by default (skips a slot whose fence is not yet done); the gated
    // explicit wait (set in InitializeSmoke) is the only blocking path and is OFF for
    // timing runs. The pipeline FUNCTIONS without ever calling this - pure validation.
    // Returns true if a comparison ran this call.
    bool PollSmokeReadback(uint64_t completedFenceValue);

    const MidMeshGpuExtractSmokeStats& GetSmokeStats() const { return m_smokeStats; }

    // =========================================================================
    // B1.3a TOP-FACE - the FIRST real meshing increment (gated by
    // VENPOD_MIDMESH_GPU_EXTRACT_B13A; requires the smoke compute side ready).
    // =========================================================================
    // Compile the top-face PSO (CS_MidMeshExtractTopFaces.hlsl). Reuses the smoke
    // path's isolated dedicated queue/fence, readback ring, timestamps, command list,
    // and debug face/sample/meta buffers; only the PSO + root layout differ. Call AFTER
    // InitializeSmoke(). No-op (returns ok) if B1.3a is not enabled.
    Result<void> InitializeB13aTopFace(
        ID3D12Device* device,
        ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath);
    bool B13aReady() const { return m_b13aReady; }

    // Pick the FIRST flat/simple B1.3a fixture from `dirtyTiles`: a resident-slot dirty
    // tile with mergeCells==1 AND childMask==0 (no resident finer children -> no
    // suppression). `hasEditFootprint` is queried by the caller per-candidate slot (a
    // fixture must have NO edit footprint, since the CPU suppresses edited cells). Returns
    // the picked tile's index in `dirtyTiles`, or UINT32_MAX if none qualifies.
    static uint32_t SelectB13aFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint);

    // B1.3b fixture: the SAME flat/simple constraints as SelectB13aFixture (mergeCells==1,
    // childMask==0, no edit footprint) PLUS real STEPPED height variation - at least two
    // adjacent in-tile sample-cells that are both SOLID with DIFFERENT quantized heights, so
    // the riser path actually emits (otherwise a perfectly flat tile yields zero risers and
    // the "riser faces > 0" gate could never be met on it). `sampleSide` / `terraceStep`
    // mirror the build so the height-variation scan matches the GPU's quantization. Returns
    // the picked tile's index in `dirtyTiles`, or UINT32_MAX if none qualifies.
    static uint32_t SelectB13bFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide,
        uint32_t terraceStep);

    // B1.3c fixture: the SAME flat/simple constraints as SelectB13aFixture (mergeCells==1,
    // childMask==0, no edit footprint) PLUS at least one BORDER cell (cx/cz on the tile edge)
    // that is SOLID - the exact condition under which the CPU's tile-border SKIRT emits, so
    // the "skirt faces > 0" gate can be met. (The CPU emits a skirt on every border SOLID
    // footprint, independent of any height step, so a stepped interior is NOT required - a
    // solid edge cell is sufficient.) `sampleSide`/`terraceStep` mirror the build so the
    // solid/quantize scan matches the GPU. Returns the picked tile's index, or UINT32_MAX.
    static uint32_t SelectB13cFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide,
        uint32_t terraceStep);

    // B1.3d fixture: the FIRST face-REMOVING increment. Constraints INVERT the childMask
    // requirement of the earlier increments - it REQUIRES childMask != 0 (at least one
    // resident finer child quadrant) so the suppression rule actually FIRES, while still
    // mergeCells==1, no edit footprint, full sample grid. (The earlier increments required
    // childMask==0 to avoid suppression; B1.3d needs it present.) `sampleSide` is used to
    // validate the grid; `terraceStep` is unused here (a quadrant either has a resident
    // child or it does not, independent of height). Returns the picked tile's index, or
    // UINT32_MAX. The CPU mesh for such a tile is { tops + risers + skirts } MINUS the
    // suppressed child quadrants, so GPU(top+riser+skirt+suppression) can A/B EQUAL to it.
    static uint32_t SelectB13dFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide);

    // B1.3e fixture: an EDITED tile - one the edit footprint overlapped (the predicate
    // `hasEditFootprint(slot)` is TRUE). This INVERTS the earlier increments, which all
    // REQUIRED no edit footprint. To isolate the edit skip as the only removed-face source,
    // it prefers mergeCells==1 AND childMask==0 (so neither merge-blocking nor child
    // suppression is in play - only the edit skip removes cells). Still a full sample grid.
    // The CPU mesh for such a tile is { tops + risers + skirts } MINUS the edited cells, so
    // GPU(top+riser+skirt+editSkip) can A/B EQUAL to it. Returns the picked tile's index, or
    // UINT32_MAX. `hasEditFootprint` is the read-only edit-overlap predicate.
    static uint32_t SelectB13eFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide);

    // B1.3f-a fixture: the LOD-MERGE increment. INVERTS the mergeCells==1 constraint of every
    // prior increment - it REQUIRES mergeCells > 1 (a far/LOD-merged tile, where the CPU
    // aggregates mergeCells x mergeCells sample cells into one coarse BLOCK). To isolate LOD
    // as the only new variable, it prefers childMask==0 AND no edit footprint (so neither
    // child suppression nor the edit skip removes faces - the only difference vs the full CPU
    // mesh is the block merge itself). A full sample grid is required. The CPU mesh for such a
    // tile is the merged (coarse) tops+risers+skirts, so GPU(per-BLOCK extraction) A/Bs to
    // FULL EQUALITY against it (run with water+cull off). Returns the picked tile's index, or
    // UINT32_MAX. `sampleSide` validates the grid (height/material come from aggregation).
    static uint32_t SelectB13faFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide);

    // B1.3f-b fixture: the WATER + ALL-AIR-FILL increment. It REQUIRES a tile that bears
    // WATER and/or AIR samples (so the water-surface tops and/or the all-air sea-level fill
    // actually emit - otherwise a pure-land tile exercises neither new path and the "water
    // faces > 0" / "air-fill faces > 0" gate could never be met). To isolate water/air-fill
    // as the only new variable it prefers childMask==0 AND no edit footprint. mergeCells is
    // NOT constrained (a water tile at any LOD is valid; the caller may demand mergeCells>1
    // for the merged-water sub-fixture via `requireMerged`). A full sample grid is required.
    // The CPU mesh for such a tile is the water-aware tops/risers + skirts (solid-only) + the
    // all-air fill, so GPU(water-aware extraction) A/Bs to FULL EQUALITY against it (water ON,
    // cull off). Returns the picked tile's index in `dirtyTiles`, or UINT32_MAX. `sampleSide`
    // validates the grid + bounds the water/air sample scan; `requireMerged` demands
    // mergeCells>1 (the merged-water sub-fixture) when true.
    // `requireWater` (default false): when true, ONLY a tile bearing >=1 WATER sample qualifies
    // (so a water-surface top is guaranteed to emit); when false, a tile bearing WATER OR AIR
    // qualifies (the all-air sea-level fill also exercises the new path). The caller tries
    // requireWater=true first so the report exercises an actual water surface when one exists.
    static uint32_t SelectB13fbFixture(
        const std::vector<Simulation::MidMeshGpuExtractDirtyTile>& dirtyTiles,
        const std::function<bool(uint32_t /*slot*/)>& hasEditFootprint,
        uint32_t sampleSide,
        bool requireMerged,
        bool requireWater = false);

    // Run ONE top-face extraction for the picked fixture tile. Uploads the tile's
    // samples+metadata (faceCount zeroed) into the dedicated smoke buffers, dispatches
    // CS_MidMeshExtractTopFaces over the tile's cell grid (each eligible solid cell
    // appends a PosY top quad via the per-tile counter), copies the result into the
    // readback ring, and captures `cpuReferenceFaces` (the tile's FULL meshCacheFaces,
    // passed in by the caller via the read-only accessor) so PollB13aReadback can A/B
    // against it. Runs on the isolated smoke queue + fence (production untouched).
    // Returns true if a dispatch was issued.
    // `emitRisers` (B1.3b): when true the dispatch also emits the CPU's right/forward
    // neighbor RISER side faces (tile-interior addressing). `emitSkirts` (B1.3c): when
    // true the dispatch ALSO emits the CPU's tile-border SKIRT faces (the self-contained
    // outward border quads that seal tile seams - no halo needed; mirrors the CPU's
    // x==0/xEnd>=cellCount/z==0/zEnd>=cellCount skirt rule using SEA_LEVEL_Y + terraceStep).
    // The containment A/B then proves GPU(top+risers+skirts) is still a multiset-subset of
    // the CPU mesh; the AB_VERIFY label becomes b13c. Both false = B1.3a top-faces only.
    // `applyChildSuppression` (B1.3d): when true, the shader applies the CPU's
    // child-quadrant suppression (a cell whose quadrant has a resident finer child emits
    // NOTHING). `equalityMode` (B1.3d): when true, the delayed poll A/Bs in EQUALITY mode
    // (gpuFaceCount==cpuFaceCount, missing==0 AND extra==0) instead of containment - the
    // suppression increment proves FULL equality, since with suppression the GPU emits the
    // exact same set the CPU does (run with water+cull off so the only variable is the
    // suppression). Defaults false keep the B1.3a-c containment behavior unchanged.
    // `applyEditSkip` (B1.3e): when true the shader skips cells inside the tile's edit
    // footprint (whole-cell, mirroring the CPU `cellInEditFootprint`), and `editBoxes` carries
    // the tile's WORLD-VOXEL edit boxes (from MidMeshTileEditBoxesBySlot) to upload + bind. An
    // empty `editBoxes` with applyEditSkip true is inert (no cell is in an empty footprint).
    // The poll then A/Bs in EQUALITY mode (run with water+cull off): the GPU emits the exact
    // CPU set minus the edited cells, so missing==0 AND extra==0.
    // `emitWater` (B1.3f-b): when true the shader runs the WATER-AWARE aggregation (water
    // samples participate, the shoal min-height override, a water-only block) AND emits the
    // all-air sea-level fill, matching the CPU emitWater path. The poll then A/Bs in EQUALITY
    // mode with water ON (run with cull off so water + air-fill is the only new variable):
    // the GPU emits the exact CPU set, missing==0 AND extra==0. Default false keeps the
    // B1.3a-f-a solid-only behavior byte-identical (water/air samples skipped, no fill).
    // `applyDistanceCull` (B1.3f-c): when true the shader skips every block the CPU CULLED,
    // consuming the per-block decision in `cullBlockMask` (the host-replayed CPU blockCullBounds
    // verdict, one uint per block, 1 = culled). The mask is laid out by blockId = bz*blockCountPerAxis
    // + bx (z-outer, x-inner), matching the shader. Because the GPU consumes the CPU's exact
    // float decision (never recomputes it), the cull is BIT-IDENTICAL at any distance threshold -
    // the boundary divergence risk is eliminated. The poll then A/Bs in EQUALITY mode WITH cull ON
    // (run via VENPOD_SPARSE_MID_MESH_DISTANCE_CULL=1): the GPU emits the exact CPU set, the cull
    // removed blocks on both sides identically, so missing==0 AND extra==0. An empty mask with
    // applyDistanceCull true is inert (no block flagged). Default false keeps B1.3a-f-b behavior.
    bool RunB13aTopFaceDispatch(
        ID3D12Device* device,
        const Simulation::MidMeshGpuExtractDirtyTile& fixture,
        const std::vector<Simulation::SparseSurfaceFace>& cpuReferenceFaces,
        uint32_t terraceStep,
        uint64_t currentTileVersionForSlot,
        bool emitRisers = false,
        bool emitSkirts = false,
        bool applyChildSuppression = false,
        bool equalityMode = false,
        bool applyEditSkip = false,
        const std::vector<Simulation::MidMeshEditXzBox>& editBoxes = {},
        bool emitWater = false,
        bool applyDistanceCull = false,
        const std::vector<uint8_t>& cullBlockMask = {});

    // Delayed, DEBUG-ONLY containment A/B via the fence-tracked ring (same FIFO/ring as
    // the smoke poll, non-blocking by default). Reads the GPU top faces + status for the
    // oldest completed dispatch and compares them against that dispatch's captured CPU
    // reference mesh in CONTAINMENT mode -> AB_VERIFY label=b13a mode=contain. Proves
    // GPU-top is a multiset-subset of the CPU mesh (extraGpuFaces==0). Returns true if a
    // comparison ran this call.
    bool PollB13aReadback();

    const MidMeshGpuExtractB13aStats& GetB13aStats() const { return m_b13aStats; }

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
        // B1.3a: this dispatch is a real top-face extraction A/B'd in CONTAINMENT mode
        // against the CPU's FULL meshCacheFaces (captured below), not the deterministic
        // smoke pattern. When set, the poll uses cpuReferenceFaces + Containment.
        bool b13aTopFace = false;
        bool emitRisers = false; // B1.3b: this dispatch also emitted neighbor risers
        bool emitSkirts = false; // B1.3c: this dispatch also emitted tile-border skirts
        bool applyChildSuppression = false; // B1.3d: child-quadrant suppression was active
        bool applyEditSkip = false; // B1.3e: edit-footprint suppression was active
        bool emitWater = false; // B1.3f-b: water-aware aggregation + all-air fill was active
        bool applyDistanceCull = false; // B1.3f-c: camera-distance cull (CPU-decision mask) active
        bool equalityMode = false; // B1.3d: poll A/Bs in EQUALITY mode (not containment)
        uint32_t mergeCells = 1u; // B1.3f-a: the fixture's LOD merge-cell size (block span)
        uint32_t childMask = 0u; // B1.3d: the fixture's childMask (for suppressed-cell count)
        uint32_t culledBlocks = 0u; // B1.3f-c: count of blocks the CPU cull mask flagged (for the log)
        std::vector<Simulation::MidMeshEditXzBox> editBoxes; // B1.3e: uploaded edit boxes (poll mirror)
        std::vector<Simulation::SparseSurfaceFace> cpuReferenceFaces; // tile meshCacheFaces
    };

    // Recompute the deterministic smoke faces CPU-side (mirror of the HLSL). Static so
    // the test/verify path and the doc stay in lockstep with the shader.
    static void ComputeSmokeFacesCpu(
        const SmokeControlledTile& tile,
        std::vector<Simulation::SparseSurfaceFace>& outFaces);

    // B1.3c: recompute ONLY the tile-border SKIRT faces CPU-side for the captured fixture
    // (mirror of the HLSL MidMeshEmitBorderSkirts + the CPU extractTileMesh skirt rule). Used
    // by PollB13aReadback to COUNT how many of the GPU's emitted faces are border skirts (the
    // "skirt faces > 0" gate), drift-free: identical FNV voxel hash / quantize / addRiser split
    // as the shader, so a skirt face matches the GPU's exactly. `terraceStep`/`seaLevelY` mirror
    // the build. Does NOT include tops or interior risers - only the border skirts.
    static void ComputeB13cSkirtFacesCpu(
        const SmokeControlledTile& tile,
        uint32_t terraceStep,
        int32_t seaLevelY,
        std::vector<Simulation::SparseSurfaceFace>& outSkirtFaces);

    // B1.3.0: one slot of the fence-tracked readback RING. Each slot owns a face +
    // metadata readback buffer that is Map()'d ONCE at init and kept persistently
    // mapped (never per-frame Map/Unmap); the recorded fence value tells the poll
    // when the GPU copy into this slot has completed, and the captured controlled
    // tile lets the poll recompute the CPU reference for THAT dispatch (delayed).
    struct SmokeReadbackSlot {
        GPUBuffer faceReadback;     // CPU-visible copy of the dispatch's debug faces
        GPUBuffer metaReadback;     // CPU-visible copy of the dispatch's metadata
        void* facePtr = nullptr;    // persistent mapped pointer into faceReadback
        void* metaPtr = nullptr;    // persistent mapped pointer into metaReadback
        uint64_t fenceValue = 0;    // smoke-fence value the copy into this slot signals
        bool pending = false;       // copy recorded, awaiting fence + a poll
        SmokeControlledTile tile;   // controlled tile snapshot for this dispatch
    };

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
    // B1.3e EDIT-FOOTPRINT: dedicated per-tile edit-box buffer (StructuredBuffer<MidMeshEditBox>,
    // SRV-read at t1) + its own small upload stage. DIRTY-SCALED: written ONLY when an edited
    // fixture is dispatched (not every tile / every frame); a non-edited dispatch leaves it
    // untouched (gEditBoxCount=0 -> the shader never reads it). Sized to editBoxCapacityPerTile.
    GPUBuffer m_editBoxBuffer;       // one tile's edit boxes (SRV-read at t1)
    UploadBuffer m_editBoxUpload;    // one tile's edit-box stage
    uint32_t m_editBoxCapacityPerTile = 0;
    // B1.3f-c CAMERA-DISTANCE CULL: dedicated per-tile per-BLOCK cull-flag buffer
    // (StructuredBuffer<uint>, SRV-read at t2) + its own small upload stage. The host fills it
    // with the CPU's bit-exact per-block cull DECISION (one uint per block, 1 = CPU culled it),
    // so the GPU consumes the CPU's verdict instead of recomputing a float predicate (no
    // boundary divergence). DIRTY-SCALED: written ONLY when a distance-cull dispatch runs.
    GPUBuffer m_cullBlockBuffer;     // one tile's per-block cull flags (SRV-read at t2)
    UploadBuffer m_cullBlockUpload;  // one tile's cull-flag stage
    uint32_t m_cullBlockCapacityPerTile = 0; // max blocks/tile == (tileSampleSide-1)^2
    // B1.3.0: fence-tracked, persistently-mapped readback RING (replaces B1.2's
    // single once-per-process readback). 3 slots so a dispatch's copy can be read
    // a frame or two later, after its fence is naturally satisfied - no per-frame
    // blocking wait on the normal path.
    static constexpr uint32_t kSmokeReadbackSlots = 3u;
    std::array<SmokeReadbackSlot, kSmokeReadbackSlots> m_smokeReadbackRing;
    uint32_t m_smokeReadbackWriteSlot = 0; // next slot a dispatch writes into
    uint32_t m_smokeReadbackReadSlot = 0;  // next slot the poll consumes (FIFO)
    // Gated explicit CPU fence WAIT: OFF by default. When ON (env
    // VENPOD_MIDMESH_GPU_EXTRACT_READBACK_WAIT=1) the poll blocks until the slot's
    // fence completes, guaranteeing a same-cycle compare in the ISOLATED correctness
    // path. NEVER on for timing runs and never on the production runtime path.
    bool m_smokeReadbackForceWait = false;
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
    bool m_smokeSelfTestDone = false; // AB_SELFTEST has been run once this process
    uint32_t m_smokeAbVerifyCount = 0; // total AB_VERIFY comparisons (ring repeatability)
    uint64_t m_smokeTimestampFrequency = 0;
    uint32_t m_smokeFaceCapacityPerTile = 0;
    // The ISOLATED debug face buffer + readback ring are sized to this (= max of the
    // smoke K capacity and, when B1.3a is enabled, the top-face capacity) so BOTH the
    // smoke and B1.3a dispatches fit in the shared isolated buffers.
    uint32_t m_debugFaceCapacityPerTile = 0;
    uint64_t m_smokeFaceBufferBytes = 0;
    SmokeControlledTile m_smokeControlled;       // last dispatch's controlled tile
    MidMeshGpuExtractSmokeStats m_smokeStats;

    // ---- B1.3a TOP-FACE (real meshing increment) ----
    // Reuses the smoke path's isolated queue/fence/readback-ring/timestamps/command list
    // and the dedicated debug sample/meta/face buffers; only the PSO + root layout + the
    // CPU-reference + containment comparison differ.
    bool m_b13aReady = false;
    DX12ComputePipeline m_topFacePipeline;
    uint32_t m_topFaceCapacityPerTile = 0;
    uint32_t m_b13aAbVerifyCount = 0;    // total containment comparisons (ring repeatability)
    uint32_t m_b13aBuildTerraceStep = 1; // terraceStep the CPU build used (height quantize)
    MidMeshGpuExtractB13aStats m_b13aStats;
};

} // namespace VENPOD::Graphics
