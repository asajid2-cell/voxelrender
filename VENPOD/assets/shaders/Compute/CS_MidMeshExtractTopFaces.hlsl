// =============================================================================
// VENPOD GPU mid-mesh extraction - Phase B1.3a TOP FACES + B1.3b RISERS
//                                  (containment-proven).
// =============================================================================
// B1.3a (gEmitRisers==0): for a controlled FLAT/SIMPLE tile (mergeCells==1,
// childMask==0, no edit footprint) this emits the CPU's TOP faces (direction = PosY)
// and NOTHING else - so the GPU output is a multiset-SUBSET of the CPU tile's
// meshCacheFaces.
//
// B1.3b (gEmitRisers==1): EXTENDS the same path to ALSO emit the CPU's RISER side
// faces - the vertical quads filling the height gap between a cell and its right/
// forward neighbor (TILE-INTERIOR addressing only; halo/border skirts are B1.3c).
// Ported byte-for-byte from extractTileMesh's right/forward-neighbor riser blocks +
// emitRiser (24u segment slicing for tall risers) + addRiser (the -1 boundary shift)
// + addFace (the 32-unit split chunking). The GPU set is still proven a multiset-
// SUBSET of the CPU mesh by MidMeshFaceAbCompare in CONTAINMENT mode (extraGpuFaces
// MUST be 0); the CPU keeps MORE (border skirts, child-suppression, water, LOD) so
// missingCpuFaces stays > 0 but SMALLER than B1.3a (risers are now covered).
//
// This is ISOLATED debug output - production geometry + the draw path are untouched.
//
// PARITY with the CPU extractTileMesh (mergeCells==1 path), all from the shared
// MidMeshExtractCommon.hlsli machinery:
//   * one cell per sample-cell (cx,cz) in [0, cellCount)^2, cellCount = side-1.
//   * aggregateSamples(x,z,x+1,z+1) for mergeCells==1 is the SINGLE sample (cx,cz).
//   * SOLID cell -> top quad: world (cellWorldX, quantize(decodeY), cellWorldZ),
//     width = height = cellSize, direction = PosY, voxel = PackVoxel(material, ...).
//   * RIGHT/FORWARD neighbor (cx+1,cz)/(cx,cz+1) is a real IN-TILE sample for every
//     interior cell in the mergeCells==1 path (cx+1,cz+1 in [1,cellCount]=side-1), so
//     NO halo is needed; the height gap to a present neighbor emits a riser.
//   * WATER / all-AIR footprints are NOT emitted here (their emission depends on the
//     emitWater + sea-level fill path); deferring them keeps the GPU set a strict subset.
//     The riser neighbor-presence test mirrors this: only SOLID neighbors are "present"
//     here, so a riser against a water/air neighbor is deferred too (a smaller subset).
//   * per-block distance/frustum cull is camera-dependent and NOT replicated; the
//     fixture is chosen near-camera + the A/B is containment, so a culled CPU cell only
//     ever makes the GPU a smaller subset (never an extra). If a culled cell DID make
//     the GPU emit an extra, the containment compare surfaces it (it is not hidden).
//
// Append model: a per-tile InterlockedAdd counter (TileMeta[slot].faceCount) hands
// each emitted sub-face a dense write slot inside the reserved range. Overflow past
// gFaceCapacityPerTile sets statusOverflow and the face is not written (the A/B
// harness fails a non-zero overflow, so a truncated result is never trusted).
// =============================================================================

#include "MidMeshExtractCommon.hlsli"

// Root constants (b0): the B1.3a/b/c controls.
cbuffer TopFaceConstants : register(b0) {
    uint gTileSlot;             // controlled tile slot (index into the metadata buffer)
    uint gDebugBaseFace;        // base of this tile's range in the DEBUG face buffer
    uint gFaceCapacityPerTile;  // per-tile debug-buffer capacity (overflow bound)
    uint gTerraceStep;          // terraceStep (build config) for height quantization
    uint gEmitRisers;           // B1.3b: 0 = top faces only (B1.3a); 1 = + risers
    uint gEmitSkirts;           // B1.3c: 0 = no border skirts; 1 = + tile-border skirts
    int  gSeaLevelY;            // B1.3c: SEA_LEVEL_Y (-48); mirrors the CPU compile-time const
}

StructuredBuffer<uint>                Samples    : register(t0); // per-tile sample grid
RWStructuredBuffer<SparseSurfaceFace> DebugFaces : register(u0); // ISOLATED debug output
RWStructuredBuffer<MidMeshTileMeta>   TileMeta   : register(u1); // metadata (faceCount UAV)

// =============================================================================
// APPEND ONE FACE - claims a dense slot from the per-tile counter, bounds-checks
// against the reserved range, writes into the ISOLATED debug buffer. Overflow sets
// the status flag and writes nothing (stay strictly inside the reserved range).
// Returns true on success (or benign skip), false on overflow (caller may stop).
// =============================================================================
bool MidMeshAppendFace(uint slot, SparseSurfaceFace face) {
    uint writeIndex;
    InterlockedAdd(TileMeta[slot].faceCount, 1u, writeIndex);
    if (writeIndex >= gFaceCapacityPerTile) {
        TileMeta[slot].statusOverflow = 1u;
        return false;
    }
    DebugFaces[gDebugBaseFace + writeIndex] = face;
    return true;
}

// =============================================================================
// addFace SPLIT-LIMIT chunking (CPU addFace, SparseClipmap.cpp). A quad wider/taller
// than the 32-unit packed-extent limit is emitted as MULTIPLE sub-faces at offset
// positions. The per-direction offset assignment MUST match the CPU switch exactly:
//   NegX/PosX : worldY += hOff, worldZ += wOff   (X-facing risers)
//   NegY/PosY : worldX += wOff, worldZ += hOff   (top/bottom quads)
//   NegZ/PosZ : worldX += wOff, worldY += hOff   (Z-facing risers)
// (worldX/worldY/worldZ here are the CPU addFace's base coords; for X-dirs worldX is
// constant across chunks, for Z-dirs worldZ is constant - mirrors the CPU switch.)
// =============================================================================
bool MidMeshAddFace(uint slot, uint direction,
                    int baseX, int baseY, int baseZ,
                    uint width, uint height, uint voxel) {
    if (width == 0u || height == 0u) {
        return true;
    }
    const uint splitLimit = kSparseExtentMask + 1u; // 32
    for (uint hOff = 0u; hOff < height; hOff += splitLimit) {
        const uint hChunk = min(splitLimit, height - hOff);
        for (uint wOff = 0u; wOff < width; wOff += splitLimit) {
            const uint wChunk = min(splitLimit, width - wOff);

            SparseSurfaceFace face;
            face.worldX = baseX;
            face.worldY = baseY;
            face.worldZ = baseZ;
            if (direction == kSparseDirNegX || direction == kSparseDirPosX) {
                face.worldY = baseY + (int)hOff;
                face.worldZ = baseZ + (int)wOff;
            } else if (direction == kSparseDirNegY || direction == kSparseDirPosY) {
                face.worldX = baseX + (int)wOff;
                face.worldZ = baseZ + (int)hOff;
            } else { // NegZ / PosZ
                face.worldX = baseX + (int)wOff;
                face.worldY = baseY + (int)hOff;
            }
            face.payload = MidMeshPackPayload(direction, voxel, wChunk, hChunk);
            if (!MidMeshAppendFace(slot, face)) {
                return false;
            }
        }
    }
    return true;
}

// =============================================================================
// addRiser (CPU SparseClipmap.cpp): a PosX riser's face X is boundaryX-1; a PosZ
// riser's face Z is boundaryZ-1 (the side quad sits one unit inside the high cell).
// NegX/NegZ keep the boundary coord. Then it is a normal split-chunked addFace.
// height==0 emits nothing (the CPU early-outs identically).
// =============================================================================
bool MidMeshAddRiser(uint slot, uint direction,
                     int boundaryX, int boundaryZ, int lowTopY,
                     uint span, uint height, uint voxel) {
    if (height == 0u) {
        return true;
    }
    int faceX = boundaryX;
    int faceZ = boundaryZ;
    if (direction == kSparseDirPosX) {
        faceX = boundaryX - 1;
    } else if (direction == kSparseDirPosZ) {
        faceZ = boundaryZ - 1;
    }
    // addFace(direction, faceX, lowTopY, faceZ, span=width, height, voxel).
    return MidMeshAddFace(slot, direction, faceX, lowTopY, faceZ, span, height, voxel);
}

// =============================================================================
// TILE-BORDER SKIRTS (CPU extractTileMesh, SparseClipmap.cpp ~7120-7170). B1.3c.
// SELF-CONTAINED - reads NO neighbor-tile samples; this is the deliberate SUBSTITUTE
// for a halo. aggregateSamples CLAMPS to the tile, so a border footprint sees itself
// as its own neighbor (equal heights -> no riser ever crosses a tile boundary). The
// CPU seals those seams with fixed-depth OUTWARD skirts on border SOLID footprints,
// using ONLY this block's own height/material/voxel + terraceStep + SEA_LEVEL_Y. We
// MIRROR it byte-for-byte (so haloWidth stays 0):
//   * SOLID blocks only (!block.water): water tops sit at the uniform sea level so
//     water-water borders cannot crack, and a water skirt is a visible dark wall.
//   * skirtDepth = max(8, terraceStep*6); skirtLowTopY = height + 1 - skirtDepth,
//     extended DOWN to SEA_LEVEL_Y - 2 whenever the block sits above sea level (so a
//     border ledge always meets the water instead of leaving a black cut).
//   * skirtHeight = max(1, height + 1 - skirtLowTopY).
//   * one addRiser per border edge (NegX@x==0, PosX@xEnd>=cellCount, NegZ@z==0,
//     PosZ@zEnd>=cellCount), span = depth (X edges) / width (Z edges), the SAME voxel
//     as the top face (NOT re-hashed per segment - this is a plain addRiser, not the
//     cliff-sliced emitRiser). The PosX/PosZ boundary coords are worldX+width /
//     worldZ+depth (addRiser applies the -1 inward shift), NegX/NegZ keep worldX/worldZ.
// `width`/`depth` are this block's footprint extent; `atNegX/atPosX/atNegZ/atPosZ` are
// the border-edge predicates the caller computes from the cell index vs cellCount.
// =============================================================================
bool MidMeshEmitBorderSkirts(uint slot,
                             int worldX, int worldZ, int height,
                             uint width, uint depth, uint voxel,
                             uint terraceStep, int seaLevelY,
                             bool atNegX, bool atPosX, bool atNegZ, bool atPosZ) {
    // skirtDepth = max(8u, terraceStep * 6u).
    const uint skirtDepth = max(8u, terraceStep * 6u);
    int skirtLowTopY = height + 1 - (int)skirtDepth;
    // height > SEA_LEVEL_Y && skirtLowTopY > SEA_LEVEL_Y - 2  ->  clamp to SEA_LEVEL_Y - 2.
    if (height > seaLevelY && skirtLowTopY > seaLevelY - 2) {
        skirtLowTopY = seaLevelY - 2;
    }
    const uint skirtHeight = (uint)max(1, height + 1 - skirtLowTopY);

    if (atNegX) {
        // addRiser(NegX, worldX, worldZ, skirtLowTopY, span=depth, skirtHeight, voxel)
        if (!MidMeshAddRiser(slot, kSparseDirNegX, worldX, worldZ, skirtLowTopY,
                             depth, skirtHeight, voxel)) {
            return false;
        }
    }
    if (atPosX) {
        // boundary X = worldX + width (addRiser applies the -1 inward shift for PosX).
        if (!MidMeshAddRiser(slot, kSparseDirPosX, worldX + (int)width, worldZ, skirtLowTopY,
                             depth, skirtHeight, voxel)) {
            return false;
        }
    }
    if (atNegZ) {
        // addRiser(NegZ, worldX, worldZ, skirtLowTopY, span=width, skirtHeight, voxel)
        if (!MidMeshAddRiser(slot, kSparseDirNegZ, worldX, worldZ, skirtLowTopY,
                             width, skirtHeight, voxel)) {
            return false;
        }
    }
    if (atPosZ) {
        // boundary Z = worldZ + depth (addRiser applies the -1 inward shift for PosZ).
        if (!MidMeshAddRiser(slot, kSparseDirPosZ, worldX, worldZ + (int)depth, skirtLowTopY,
                             width, skirtHeight, voxel)) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// emitRiser (CPU SparseClipmap.cpp CLIFF-RISER SLICING): short risers (<=32) emit a
// single riser whose voxel is hashed at lowTopY+riserHeight. Tall risers slice into
// 24u vertical segments, each hashed at its own segment top (segLowTopY+segHeight) so
// the cliff reads as stratified rock. riserX/riserZ are the BOUNDARY coords (the
// voxel hash uses them, NOT the addRiser -1 face coord - mirrors the CPU exactly).
// =============================================================================
bool MidMeshEmitRiser(uint slot, uint direction,
                      int riserX, int riserZ, int lowTopY,
                      uint span, uint riserHeight, uint riserMaterial) {
    if (riserHeight <= 32u) {
        const uint riserVoxel = MidMeshPackVoxel(
            riserMaterial, riserX, lowTopY + (int)riserHeight, riserZ);
        return MidMeshAddRiser(slot, direction, riserX, riserZ, lowTopY, span, riserHeight, riserVoxel);
    }
    uint emitted = 0u;
    while (emitted < riserHeight) {
        const uint segHeight = min(24u, riserHeight - emitted);
        const int segLowTopY = lowTopY + (int)emitted;
        const uint segVoxel = MidMeshPackVoxel(
            riserMaterial, riserX, segLowTopY + (int)segHeight, riserZ);
        if (!MidMeshAddRiser(slot, direction, riserX, riserZ, segLowTopY, span, segHeight, segVoxel)) {
            return false;
        }
        emitted += segHeight;
    }
    return true;
}

// Decode one IN-TILE sample-cell into a present/solid SurfaceBlock for the
// mergeCells==1 path: aggregateSamples(cx,cz,cx+1,cz+1) is the single sample (cx,cz).
// Returns whether the cell is a SOLID block (the only "present" kind the GPU subset
// emits here - water/air are deferred, matching the top-face eligibility) and fills
// the quantized height + material.
bool MidMeshSolidCellBlock(uint slot, uint stride, uint side, uint cx, uint cz,
                           uint heightBias, uint terraceStep,
                           out int outHeight, out uint outMaterial) {
    outHeight = 0;
    outMaterial = 0u;
    const uint sampleIndex = MidMeshSampleIndex(slot, stride, side, cx, cz);
    const uint sample = Samples[sampleIndex];
    const uint material = MidMeshDecodeSampleMaterial(sample);
    if (!MidMeshIsSolidMaterial(material)) {
        return false;
    }
    outMaterial = material;
    outHeight = MidMeshQuantizeY(MidMeshDecodeSampleY(sample, heightBias), terraceStep);
    return true;
}

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

    const int cellSizeInt = MidMeshCellSizeInt(meta.cellSizeBits);
    const uint heightBias = meta.heightBias;
    const uint terraceStep = gTerraceStep;

    // ---- this cell's block (aggregateSamples(cx,cz,cx+1,cz+1) == sample (cx,cz)) ----
    int height;
    uint material;
    if (!MidMeshSolidCellBlock(slot, stride, side, cx, cz, heightBias, terraceStep,
                               height, material)) {
        // B1.3a/b emit SOLID footprints only (subset-safe). Water / all-air emission is a
        // later increment (it depends on emitWater + the sea-level fill rule). A non-solid
        // cell also emits no risers toward its neighbors (matching the CPU subset deferral).
        return;
    }

    const int worldX = MidMeshCellWorldX(meta.originX, cx, cellSizeInt);
    const int worldZ = MidMeshCellWorldZ(meta.originZ, cz, cellSizeInt);
    // width = depth = cellSize for a single-cell footprint (xEnd-x == 1 -> *cellSize).
    const uint width = (uint)cellSizeInt;
    const uint depth = (uint)cellSizeInt;

    // ---- TOP FACE (B1.3a) ----
    // voxel = PackMidHeightSurfaceVoxel(material, worldX, height, worldZ), then masked
    // into the 19-bit payload field by MidMeshPackPayload (matches PackSparseSurfacePayload).
    const uint voxel = MidMeshPackVoxel(material, worldX, height, worldZ);
    // addFace SPLIT-LIMIT chunking parity: a top quad wider/deeper than 32u is emitted as
    // MULTIPLE sub-faces. For PosY: worldX += wOff, worldZ += hOff. cellSize<=32 -> 1 face.
    MidMeshAddFace(slot, kSparseDirPosY, worldX, height, worldZ, width, depth, voxel);

    // ---- TILE-BORDER SKIRTS (B1.3c) ----
    // CPU order: top face -> border skirts -> risers. The skirt is SOLID-only; this path
    // already early-returned for non-solid cells (MidMeshSolidCellBlock above), so block
    // is always solid here -> the CPU's `!block.water` guard is satisfied implicitly.
    // Border-edge predicates mirror the CPU mergeCells==1 case: x==0 (cx==0), xEnd>=cellCount
    // (cx==cellsPerRow-1), z==0 (cz==0), zEnd>=cellCount (cz==cellsPerRow-1). cellsPerRow is
    // the CPU's cellCount (== side-1). Skirts use the SAME voxel as the top face.
    if (gEmitSkirts != 0u) {
        const bool atNegX = (cx == 0u);
        const bool atPosX = (cx + 1u >= cellsPerRow);
        const bool atNegZ = (cz == 0u);
        const bool atPosZ = (cz + 1u >= cellsPerRow);
        if (atNegX || atPosX || atNegZ || atPosZ) {
            if (!MidMeshEmitBorderSkirts(slot, worldX, worldZ, height,
                                         width, depth, voxel,
                                         terraceStep, gSeaLevelY,
                                         atNegX, atPosX, atNegZ, atPosZ)) {
                return; // overflow -> status set; stop emitting for this thread.
            }
        }
    }

    // ---- RISERS (B1.3b) ----
    // Only when enabled. INTERIOR addressing: the right (cx+1,cz) / forward (cx,cz+1)
    // neighbor sample is in-tile for every interior cell of the mergeCells==1 path. The
    // CPU's border SKIRTS (x==0 / xEnd>=cellCount etc.) are a SEPARATE path (B1.3c) and
    // are NOT emitted here, so the GPU set stays a strict subset (skirts remain missing).
    if (gEmitRisers == 0u) {
        return;
    }

    // RIGHT NEIGHBOR (cx+1, cz): rightBlock = aggregateSamples(xEnd,z,xEnd+1,zEnd) for
    // mergeCells==1 == sample (cx+1,cz). Present-and-different-height -> riser between them.
    if (cx + 1u < side) { // cx in [0,cellCount)=[0,side-1) so this is always true; guard anyway
        int rHeight;
        uint rMaterial;
        if (MidMeshSolidCellBlock(slot, stride, side, cx + 1u, cz, heightBias, terraceStep,
                                  rHeight, rMaterial) &&
            height != rHeight) {
            const int lowTopY = min(height, rHeight) + 1;
            const uint riserHeight = (uint)abs(height - rHeight);
            const bool currentHigher = (height > rHeight);
            // boundary = (worldX + width, worldZ); span = depth; dir PosX if current higher.
            MidMeshEmitRiser(
                slot,
                currentHigher ? kSparseDirPosX : kSparseDirNegX,
                worldX + (int)width,
                worldZ,
                lowTopY,
                depth,
                riserHeight,
                currentHigher ? material : rMaterial);
        }
    }

    // FORWARD NEIGHBOR (cx, cz+1): forwardBlock = aggregateSamples(x,zEnd,xEnd,zEnd+1) for
    // mergeCells==1 == sample (cx,cz+1). Present-and-different-height -> riser between them.
    if (cz + 1u < side) {
        int fHeight;
        uint fMaterial;
        if (MidMeshSolidCellBlock(slot, stride, side, cx, cz + 1u, heightBias, terraceStep,
                                  fHeight, fMaterial) &&
            height != fHeight) {
            const int lowTopY = min(height, fHeight) + 1;
            const uint riserHeight = (uint)abs(height - fHeight);
            const bool currentHigher = (height > fHeight);
            // boundary = (worldX, worldZ + depth); span = width; dir PosZ if current higher.
            MidMeshEmitRiser(
                slot,
                currentHigher ? kSparseDirPosZ : kSparseDirNegZ,
                worldX,
                worldZ + (int)depth,
                lowTopY,
                width,
                riserHeight,
                currentHigher ? material : fMaterial);
        }
    }
}
