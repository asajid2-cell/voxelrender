// =============================================================================
// VENPOD GPU mid-mesh extraction - Phase B1.2 SMOKE (minimal compute proof).
// =============================================================================
// This is NOT the real mid-mesh extractor (that is B1.3). It proves the GPU
// compute path end to end for ONE controlled tile:
//   sample buffer (SRV) + per-tile metadata (UAV) -> compute dispatch ->
//   SparseSurfaceFace output (isolated debug UAV) -> faceCount/status write-back.
//
// It writes a TRIVIAL DETERMINISTIC face set: for the first K cells of the tile
// (row-major along the sample grid), one top-direction (direction=3) unit quad at
// the cell's world position, using the sample's unpacked Y. The point is to prove
// the samples are readable and the face write/format works - NOT to match the CPU
// mesh. The CPU side (ComputeSmokeFacesCpu) mirrors this exactly for verification.
//
// HARD ABI: the SparseSurfaceFace pack mirrors SparseSurfaceExtractor.h / the draw
// VS exactly (direction bits 29..31, width 24..28 (+1), height 19..23 (+1),
// voxel 0..18). The metadata struct mirrors MidMeshGpuExtractTileMeta (80 bytes,
// 5x uint4) byte-for-byte. A one-field drift reads as random geometry.
// =============================================================================

struct SparseSurfaceFace {
    int worldX;
    int worldY;
    int worldZ;
    uint payload;
};

// Mirrors MidMeshGpuExtractTileMeta (5x uint4 = 80 bytes). Field order MUST match.
struct MidMeshTileMeta {
    int coordX;
    int coordRing;
    int coordZ;
    int originX;

    int originZ;
    uint cellSizeBits;     // float cellSize reinterpreted as uint
    uint mergeCells;
    uint childMask;

    uint meshContentVersionLo;
    uint meshContentVersionHi;
    uint sampleSide;
    uint sampleStride;

    uint haloWidth;
    uint heightPackDesc;
    uint heightBias;       // 32768
    uint editFootprintCount;

    uint baseFace;         // reserved face range base (production metadata; unused here)
    uint faceCapacity;     // production capacity (unused here)
    uint faceCount;        // GPU-written actual count (we OVERWRITE for the smoke)
    uint statusOverflow;   // status / overflow flag (we OVERWRITE for the smoke)
};

// Root constants (b0): the smoke controls (independent of the production range fields).
cbuffer SmokeConstants : register(b0) {
    uint gTileSlot;              // controlled tile slot (index into both buffers)
    uint gDebugBaseFace;         // base of this tile's range in the DEBUG face buffer
    uint gMaxCells;              // K (cells to attempt)
    uint gFaceCapacityPerTile;   // per-tile debug-buffer capacity (overflow bound)
}

StructuredBuffer<uint>          Samples  : register(t0); // persistent per-tile sample grid
RWStructuredBuffer<SparseSurfaceFace> DebugFaces : register(u0); // ISOLATED debug output
RWStructuredBuffer<MidMeshTileMeta>   TileMeta   : register(u1); // metadata (read + write back)

static const uint kDirShift   = 29u;
static const uint kDirMask    = 0x7u;
static const uint kWidthShift = 24u;
static const uint kHeightShift= 19u;
static const uint kExtentMask = 0x1Fu;
static const uint kVoxelMask  = 0x0007FFFFu;

uint PackPayload(uint direction, uint voxel, uint width, uint height) {
    uint packedWidth  = ((width  > 0u) ? (width  - 1u) : 0u) & kExtentMask;
    uint packedHeight = ((height > 0u) ? (height - 1u) : 0u) & kExtentMask;
    return ((direction & kDirMask) << kDirShift) |
           (packedWidth  << kWidthShift) |
           (packedHeight << kHeightShift) |
           (voxel & kVoxelMask);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    const uint slot = gTileSlot;
    MidMeshTileMeta meta = TileMeta[slot];

    const uint side = meta.sampleSide;
    const uint stride = (meta.sampleStride > 0u) ? meta.sampleStride : side;
    const uint cellsPerRow = (side > 1u) ? (side - 1u) : 0u;
    const uint cellCount = cellsPerRow * cellsPerRow;

    // K cells to attempt this dispatch, clamped to the actual cell grid.
    uint kCells = min(gMaxCells, cellCount);

    // Overflow gate: stay strictly inside the reserved per-tile range. If K would
    // exceed the capacity, set the overflow status and write NOTHING.
    if (kCells > gFaceCapacityPerTile) {
        if (dispatchId.x == 0u) {
            TileMeta[slot].faceCount = 0u;
            TileMeta[slot].statusOverflow = 1u; // overflow flag
        }
        return;
    }

    // Degenerate tile (no cells): thread 0 publishes count=0 / ok, write nothing.
    if (cellsPerRow == 0u || kCells == 0u) {
        if (dispatchId.x == 0u) {
            TileMeta[slot].faceCount = 0u;
            TileMeta[slot].statusOverflow = 0u;
        }
        return;
    }

    const uint cell = dispatchId.x;
    // One face per cell (cells 0..kCells-1): a top (direction=3) unit quad at the
    // cell's world position, Y from the sample's unpacked height
    // ((sample & 0xFFFF) - bias). Deterministic; mirrored by ComputeSmokeFacesCpu.
    if (cell < kCells) {
        // cellSizeBits is the float cellSize's bit pattern. Convert NUMERICALLY to int
        // (truncate), NOT reinterpret: asint(asfloat(bits)) would hand back the float
        // bits. Mirror the CPU static_cast<int32_t>(cellSize).
        const int cellSizeInt = max(1, (int)asfloat(meta.cellSizeBits));
        const uint cx = cell % cellsPerRow;
        const uint cz = cell / cellsPerRow;
        const uint sampleIndex = cz * stride + cx;
        const uint sample = Samples[(slot * (stride * side)) + sampleIndex];
        const int unpackedY = (int)(sample & 0xFFFFu) - (int)meta.heightBias;

        SparseSurfaceFace face;
        face.worldX = meta.originX + (int)cx * cellSizeInt;
        face.worldY = unpackedY;
        face.worldZ = meta.originZ + (int)cz * cellSizeInt;
        // voxel payload = deterministic per-cell id (cell index), width=height=1,
        // direction=3 (PosY / top).
        face.payload = PackPayload(3u, cell & kVoxelMask, 1u, 1u);
        DebugFaces[gDebugBaseFace + cell] = face;
    }

    // Thread 0 writes the authoritative count + ok status (no overflow).
    if (dispatchId.x == 0u) {
        TileMeta[slot].faceCount = kCells;
        TileMeta[slot].statusOverflow = 0u;
    }
}
