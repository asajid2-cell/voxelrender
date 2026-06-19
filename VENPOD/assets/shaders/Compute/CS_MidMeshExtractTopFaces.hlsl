// =============================================================================
// VENPOD GPU mid-mesh extraction - Phase B1.3a: TOP FACES (containment-proven).
// =============================================================================
// The FIRST real meshing increment. For a controlled FLAT/SIMPLE tile
// (mergeCells==1, childMask==0, no edit footprint) this emits the CPU's TOP faces
// (direction = PosY) and NOTHING else (no risers, no skirts) - so the GPU output is
// a multiset-SUBSET of the CPU tile's meshCacheFaces. Proven by MidMeshFaceAbCompare
// in CONTAINMENT mode: every GPU top face must appear in the CPU mesh; extraGpuFaces
// must be 0. This is ISOLATED debug output - production geometry + the draw path are
// untouched.
//
// PARITY with the CPU extractTileMesh (mergeCells==1 path), all from the shared
// MidMeshExtractCommon.hlsli machinery:
//   * one cell per sample-cell (cx,cz) in [0, cellCount)^2, cellCount = side-1.
//   * aggregateSamples(x,z,x+1,z+1) for mergeCells==1 is the SINGLE sample (cx,cz).
//   * SOLID cell -> top quad: world (cellWorldX, quantize(decodeY), cellWorldZ),
//     width = height = cellSize, direction = PosY, voxel = PackVoxel(material, ...).
//   * WATER / all-AIR footprints are NOT emitted here (their emission depends on the
//     emitWater + sea-level fill path); deferring them keeps GPU-top a strict subset.
//   * per-block distance/frustum cull is camera-dependent and NOT replicated; the
//     fixture is chosen near-camera + the A/B is containment, so a culled CPU cell only
//     ever makes the GPU a smaller subset (never an extra). If a culled cell DID make
//     the GPU emit an extra, the containment compare surfaces it (it is not hidden).
//
// Append model: a per-tile InterlockedAdd counter (TileMeta[slot].faceCount) hands
// each eligible cell a dense write slot inside the reserved range. Overflow past
// gFaceCapacityPerTile sets statusOverflow and the face is not written (the A/B
// harness fails a non-zero overflow, so a truncated result is never trusted).
// =============================================================================

#include "MidMeshExtractCommon.hlsli"

// Root constants (b0): the B1.3a controls.
cbuffer TopFaceConstants : register(b0) {
    uint gTileSlot;             // controlled tile slot (index into the metadata buffer)
    uint gDebugBaseFace;        // base of this tile's range in the DEBUG face buffer
    uint gFaceCapacityPerTile;  // per-tile debug-buffer capacity (overflow bound)
    uint gTerraceStep;          // terraceStep (build config) for height quantization
}

StructuredBuffer<uint>                Samples    : register(t0); // per-tile sample grid
RWStructuredBuffer<SparseSurfaceFace> DebugFaces : register(u0); // ISOLATED debug output
RWStructuredBuffer<MidMeshTileMeta>   TileMeta   : register(u1); // metadata (faceCount UAV)

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    const uint slot = gTileSlot;
    MidMeshTileMeta meta = TileMeta[slot];

    const uint side = meta.sampleSide;
    const uint stride = (meta.sampleStride > 0u) ? meta.sampleStride : side;
    const uint cellsPerRow = (side > 1u) ? (side - 1u) : 0u;
    const uint cellCount = cellsPerRow * cellsPerRow;

    // Thread 0 initializes the per-tile face counter + ok status for this dispatch.
    // (The host zeroes the counter pre-dispatch too, but make the shader self-consistent.)
    if (dispatchId.x == 0u) {
        TileMeta[slot].faceCount = 0u;
        TileMeta[slot].statusOverflow = 0u;
    }
    // A device-scope barrier so every thread sees the zeroed counter before appending.
    AllMemoryBarrierWithGroupSync();

    if (cellsPerRow == 0u) {
        return; // degenerate tile - nothing to emit.
    }

    const uint cell = dispatchId.x;
    if (cell >= cellCount) {
        return; // padding thread.
    }

    // B1.3a is the mergeCells==1 fixture path only. A non-1 merge tile would aggregate
    // a block; that is a later increment. Guard so a misrouted tile emits NOTHING (a
    // smaller subset) instead of wrong geometry.
    if (meta.mergeCells != 1u) {
        return;
    }

    const uint cx = cell % cellsPerRow;
    const uint cz = cell / cellsPerRow;
    const uint sampleIndex = MidMeshSampleIndex(slot, stride, side, cx, cz);
    const uint sample = Samples[sampleIndex];

    const uint material = MidMeshDecodeSampleMaterial(sample);
    // B1.3a emits SOLID top faces only (subset-safe). Water / all-air emission is a
    // later increment (it depends on emitWater + the sea-level fill rule).
    if (!MidMeshIsSolidMaterial(material)) {
        return;
    }

    const int cellSizeInt = MidMeshCellSizeInt(meta.cellSizeBits);
    const int rawY = MidMeshDecodeSampleY(sample, meta.heightBias);
    const int height = MidMeshQuantizeY(rawY, gTerraceStep);
    const int worldX = MidMeshCellWorldX(meta.originX, cx, cellSizeInt);
    const int worldZ = MidMeshCellWorldZ(meta.originZ, cz, cellSizeInt);

    // voxel = PackMidHeightSurfaceVoxel(material, worldX, height, worldZ), then masked
    // into the 19-bit payload field by MidMeshPackPayload (matches PackSparseSurfacePayload).
    const uint voxel = MidMeshPackVoxel(material, worldX, height, worldZ);
    // width = depth = cellSize for a single-cell footprint (xEnd-x == 1 -> *cellSize).
    const uint width = (uint)cellSizeInt;
    const uint depth = (uint)cellSizeInt;

    // addFace SPLIT-LIMIT chunking parity (CPU addFace, SparseClipmap.cpp): a top quad
    // wider/deeper than the 32-unit packed-extent limit is emitted as MULTIPLE sub-faces
    // at offset positions, NOT one big face. Replicate exactly so the GPU face(s) match the
    // CPU's chunked faces 1:1. For PosY: worldX += wOff, worldZ += hOff; wChunk/hChunk are
    // the per-sub-quad packed extents. For cellSize <= 32 this is a single face (no split).
    const uint splitLimit = kSparseExtentMask + 1u; // 32
    for (uint hOff = 0u; hOff < depth; hOff += splitLimit) {
        const uint hChunk = min(splitLimit, depth - hOff);
        for (uint wOff = 0u; wOff < width; wOff += splitLimit) {
            const uint wChunk = min(splitLimit, width - wOff);

            // Claim a dense slot in the reserved range via the per-tile counter.
            uint writeIndex;
            InterlockedAdd(TileMeta[slot].faceCount, 1u, writeIndex);
            if (writeIndex >= gFaceCapacityPerTile) {
                // Overflow: stay strictly inside the reserved per-tile range. Flag it and
                // write nothing (the A/B harness fails on a non-zero status, so a truncated
                // result is never trusted).
                TileMeta[slot].statusOverflow = 1u;
                continue;
            }

            SparseSurfaceFace face;
            // PosY: the chunk offsets advance worldX (by wOff) and worldZ (by hOff).
            face.worldX = worldX + (int)wOff;
            face.worldY = height;
            face.worldZ = worldZ + (int)hOff;
            face.payload = MidMeshPackPayload(kSparseDirPosY, voxel, wChunk, hChunk);
            DebugFaces[gDebugBaseFace + writeIndex] = face;
        }
    }
}
