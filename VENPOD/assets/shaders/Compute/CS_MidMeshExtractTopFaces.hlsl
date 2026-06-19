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

// Root constants (b0): the B1.3a/b/c/d controls.
cbuffer TopFaceConstants : register(b0) {
    uint gTileSlot;             // controlled tile slot (index into the metadata buffer)
    uint gDebugBaseFace;        // base of this tile's range in the DEBUG face buffer
    uint gFaceCapacityPerTile;  // per-tile debug-buffer capacity (overflow bound)
    uint gTerraceStep;          // terraceStep (build config) for height quantization
    uint gEmitRisers;           // B1.3b: 0 = top faces only (B1.3a); 1 = + risers
    uint gEmitSkirts;           // B1.3c: 0 = no border skirts; 1 = + tile-border skirts
    int  gSeaLevelY;            // B1.3c: SEA_LEVEL_Y (-48); mirrors the CPU compile-time const
    uint gApplyChildSuppression;// B1.3d: 0 = no child suppression (B1.3a-c); 1 = suppress
                                //        cells whose quadrant has a resident finer child.
    uint gApplyEditSkip;        // B1.3e: 0 = no edit skip (B1.3a-d); 1 = skip cells inside
                                //        the tile's edit footprint (whole-cell, like the CPU).
    uint gEditBoxBase;          // B1.3e: base index of this tile's edit boxes in EditBoxes.
    uint gEditBoxCount;         // B1.3e: number of edit boxes for this tile (0 = none).
    uint gEmitWater;            // B1.3f-b: 0 = SOLID-only aggregation (B1.3a-f-a; water/air
                                //        samples skipped, no all-air fill); 1 = WATER-AWARE
                                //        aggregation + water-surface tops + all-air sea-level
                                //        fill, matching the CPU emitWater path byte-for-byte.
    uint gApplyDistanceCull;    // B1.3f-c: 0 = no camera-distance cull (B1.3a-f-b; the GPU did
                                //        NOT replicate it, so those runs forced DISTANCE_CULL=0);
                                //        1 = consume the CPU's per-block cull DECISION from
                                //        CullBlockMask (NOT a re-computed float predicate). The
                                //        host fills CullBlockMask by replaying the EXACT CPU
                                //        blockCullBounds float math, so the GPU's cull decision is
                                //        BIT-IDENTICAL to the CPU's at any threshold (no float
                                //        divergence at the cull boundary - the flagged risk).
    uint gCullBlockBase;        // B1.3f-c: base index of this tile's per-block cull flags in
                                //        CullBlockMask (one uint per block, 1 = CPU culled it).
}

// =============================================================================
// B1.3d CHILD-QUADRANT SUPPRESSION - mirrors extractTileMesh's L7 finer-coverage
// suppression (SparseClipmap.cpp ~7091-7103), the no-monolith rule:
//
//   if (anyChildResident) {
//       const uint midCell  = x + (xEnd - x) / 2;   // mergeCells==1 -> == cx
//       const uint midCellZ = z + (zEnd - z) / 2;   // mergeCells==1 -> == cz
//       const uint qx = (midCell  * 2 >= cellCount) ? 1 : 0;   // cellCount = PER-AXIS count
//       const uint qz = (midCellZ * 2 >= cellCount) ? 1 : 0;
//       if (childResident[qz * 2 + qx]) continue;   // SKIP THE WHOLE CELL
//   }
//
// The CPU `continue` sits at the TOP of the footprint loop body, BEFORE the top
// face, the border skirts, AND the risers - so a suppressed cell emits NOTHING.
// Mirror that: this returns true (skip everything) when the cell's quadrant has a
// resident child. The childResident[i] bit is bit i of childMask (i = qz*2 + qx);
// childMask bit mapping: 0=(qx0,qz0) 1=(qx1,qz0) 2=(qx0,qz1) 3=(qx1,qz1), matching
// the CPU computeTileLod pack. `cellsPerRow` here is the CPU's PER-AXIS `cellCount`
// (== side - 1). For mergeCells==1, midCell==cx / midCellZ==cz exactly; for mergeCells>1
// (B1.3f-a) the caller passes the BLOCK-CENTER cell (midCell/midCellZ) so this computes
// the CPU's quadrant from the block center, generalizing the B1.3d rule unchanged.
// =============================================================================
bool MidMeshCellSuppressedByChild(uint childMask, uint cellsPerRow, uint cx, uint cz) {
    // cx/cz are the BLOCK-CENTER cell (== cx for mergeCells==1). qx/qz = the CPU quadrant.
    const uint qx = (cx * 2u >= cellsPerRow) ? 1u : 0u;
    const uint qz = (cz * 2u >= cellsPerRow) ? 1u : 0u;
    const uint quadrantBit = qz * 2u + qx;
    return (childMask & (1u << quadrantBit)) != 0u;
}

StructuredBuffer<uint>                Samples       : register(t0); // per-tile sample grid
StructuredBuffer<MidMeshEditBox>      EditBoxes     : register(t1); // B1.3e: per-tile edit boxes
StructuredBuffer<uint>                CullBlockMask : register(t2); // B1.3f-c: per-block CPU cull flag
RWStructuredBuffer<SparseSurfaceFace> DebugFaces    : register(u0); // ISOLATED debug output
RWStructuredBuffer<MidMeshTileMeta>   TileMeta      : register(u1); // metadata (faceCount UAV)

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

// =============================================================================
// B1.3f-a AGGREGATE SAMPLES (LOD MERGE) - faithful port of extractTileMesh's
// `aggregateSamples` lambda (SparseClipmap.cpp), WATER-OFF path. The B1.3f-a run
// forces VENPOD_SPARSE_MID_MESH_WATER=0, so the CPU `aggregateSamples` skips every
// water sample (`if (!buildConfig.emitWater && material==Water) continue;`) and
// also never runs the all-air fill (`if (!buildConfig.emitWater) continue;`). With
// water OFF the CPU's water counters stay 0, the shoal-height fix (which only fires
// when waterCount>0) never triggers, and `block.present` becomes exactly
// `block.solid`. So the GPU's solid-only aggregation reproduces the CPU block EXACTLY
// for this run, mergeCells>=1, while staying a clean subset under any future water-on
// path (water tops are deferred, never an extra).
//
// The aggregation rule the GPU MUST match byte-for-byte (the #1 risk of this
// increment):
//   * clamp the block span to the tile: x0 in [0,side-1], x1 in [x0+1, side] (same z).
//   * iterate samples in the CPU's order (z OUTER, x INNER) so the tie-break material
//     matches: the LAST solid sample at the running max height wins (the CPU updates on
//     `h >= block.height`, i.e. >= not >, so a later equal-height sample overwrites the
//     material).
//   * height = the MAXIMUM quantized height over the block's SOLID samples (any-solid-
//     wins-MAX; the shoal MIN-height override is a water-mixed rule, inert when water is
//     off). material = that winning sample's material.
//   * present == solid: the block is present iff it has >=1 solid sample (water/air
//     samples are skipped). A block with no solid sample returns present=false (the CPU
//     `!block.present` -> `continue` under water-off, emitting nothing).
// For mergeCells==1 a 1x1 block is the single sample (x0..x0+1) x (z0..z0+1), so this
// reduces EXACTLY to the old single-sample decode (byte-identical to B1.3e).
// =============================================================================
bool MidMeshAggregateSolidBlock(
    uint slot, uint stride, uint side,
    uint x0, uint z0, uint x1, uint z1,
    uint heightBias, uint terraceStep,
    out int outHeight, out uint outMaterial)
{
    outHeight = 0;
    outMaterial = 0u;
    // Clamp exactly like the CPU aggregateSamples.
    x0 = min(x0, side - 1u);
    z0 = min(z0, side - 1u);
    x1 = max(x0 + 1u, min(x1, side));
    z1 = max(z0 + 1u, min(z1, side));
    bool present = false;
    int blockHeight = 0;
    uint blockMaterial = 0u;
    for (uint z = z0; z < z1; ++z) {
        for (uint x = x0; x < x1; ++x) {
            const uint sampleIndex = MidMeshSampleIndex(slot, stride, side, x, z);
            const uint sample = Samples[sampleIndex];
            const uint material = MidMeshDecodeSampleMaterial(sample);
            // WATER-OFF: skip water (and air) samples - mirrors the CPU's
            // `!emitWater && Water -> continue` plus `!solid && !water -> continue`.
            if (!MidMeshIsSolidMaterial(material)) {
                continue;
            }
            const int h = MidMeshQuantizeY(MidMeshDecodeSampleY(sample, heightBias), terraceStep);
            // any-solid-wins MAX: update on `!present || h >= blockHeight` (>= so the
            // last sample at the max height owns the material - matches the CPU `>=`).
            if (!present || h >= blockHeight) {
                present = true;
                blockHeight = h;
                blockMaterial = material;
            }
        }
    }
    if (!present) {
        return false;
    }
    outHeight = blockHeight;
    outMaterial = blockMaterial;
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    const uint slot = gTileSlot;
    MidMeshTileMeta meta = TileMeta[slot];

    const uint side = meta.sampleSide;
    const uint stride = (meta.sampleStride > 0u) ? meta.sampleStride : side;
    // cellsPerAxis == the CPU extractTileMesh's `cellCount` (PER-AXIS cell count = side-1).
    // Earlier increments named this `cellsPerRow`; we keep that meaning but rename to
    // disambiguate from the CPU's `cellCount` (which is per-axis, NOT total). The B1.3d
    // child-suppression helper expects this PER-AXIS count too.
    const uint cellsPerAxis = (side > 1u) ? (side - 1u) : 0u;

    // ---- B1.3f-a LOD MERGE: per-BLOCK loop geometry (mirrors extractTileMesh) ----
    // mergeCells==1 -> blockCountPerAxis==cellsPerAxis -> one block per cell (the exact
    // B1.3a-e mapping, byte-identical). mergeCells>1 -> coarse blocks, each spanning up to
    // mergeCells x mergeCells cells, clamped at the tile's far/upper edge by `min`.
    const uint mergeCells = max(1u, meta.mergeCells);
    const uint blockCountPerAxis =
        (cellsPerAxis > 0u) ? ((cellsPerAxis + mergeCells - 1u) / mergeCells) : 0u;
    const uint blockCount = blockCountPerAxis * blockCountPerAxis;

    // Thread 0 initializes the per-tile face counter + ok status for this dispatch.
    // (The host zeroes the counter pre-dispatch too, but make the shader self-consistent.)
    if (dispatchId.x == 0u) {
        TileMeta[slot].faceCount = 0u;
        TileMeta[slot].statusOverflow = 0u;
    }
    // A device-scope barrier so every thread sees the zeroed counter before appending.
    AllMemoryBarrierWithGroupSync();

    if (cellsPerAxis == 0u || blockCountPerAxis == 0u) {
        return; // degenerate tile - nothing to emit.
    }

    // ONE THREAD PER BLOCK (B1.3f-a). For mergeCells==1 this is one thread per cell, so the
    // host's cellCount^2 dispatch still covers all blocks (blockCount <= cellsPerAxis^2).
    const uint blockId = dispatchId.x;
    if (blockId >= blockCount) {
        return; // padding thread (or a coarse tile where blockCount < dispatched cellCount^2).
    }

    // Block grid index (bx, bz) in [0, blockCountPerAxis)^2. The block's CELL span is
    // [x, xEnd) x [z, zEnd), x = bx*mergeCells, xEnd = min(cellsPerAxis, x+mergeCells) -
    // exactly the CPU's `for (x=0; x<cellCount; x+=mergeCells) xEnd=min(cellCount,x+mergeCells)`.
    const uint bx = blockId % blockCountPerAxis;
    const uint bz = blockId / blockCountPerAxis;
    const uint x = bx * mergeCells;
    const uint z = bz * mergeCells;
    const uint xEnd = min(cellsPerAxis, x + mergeCells);
    const uint zEnd = min(cellsPerAxis, z + mergeCells);

    // ---- B1.3f-c CAMERA-DISTANCE CULL (block space) ----
    // Mirrors extractTileMesh's per-block `blockCullBounds` `continue` (SparseClipmap.cpp ~7088),
    // which sits AFTER aggregateSamples + the all-air fill but BEFORE child suppression + emission.
    // A culled block emits NOTHING regardless of where the skip sits (each gate independently
    // emits nothing), so applying it here is equivalent. CRITICAL DETERMINISM: we do NOT recompute
    // the float distance predicate on the GPU (CPU sqrt is correctly-rounded, HLSL sqrt is only
    // 1-ULP and dxc may contract a*b+c into an FMA -> a block at the exact threshold could cull on
    // one side and not the other). Instead the host replayed the EXACT CPU blockCullBounds float
    // math and handed us the DECISION in CullBlockMask, so the GPU consumes the CPU's bit-identical
    // verdict. blockId == bz*blockCountPerAxis + bx, matching the host mask layout exactly.
    if (gApplyDistanceCull != 0u) {
        if (CullBlockMask[gCullBlockBase + blockId] != 0u) {
            return; // whole-block skip (matches the CPU `continue`)
        }
    }

    const int cellSizeIntEarly = MidMeshCellSizeInt(meta.cellSizeBits);

    // ---- B1.3e EDIT-FOOTPRINT SUPPRESSION (block space) ----
    // Mirrors the CPU's `cellInEditFootprint` `continue` at the VERY TOP of the BLOCK loop
    // body - BEFORE aggregateSamples, the all-air fill, distance cull, child suppression,
    // top, skirts, AND risers. The CPU box is the BLOCK's world box:
    // (cellWorldX(x),cellWorldZ(z)) .. (cellWorldX(xEnd),cellWorldZ(zEnd)). For mergeCells==1
    // x..xEnd is one cell (cx..cx+1), identical to B1.3e. A block overlapping any edit box
    // emits NOTHING. When gApplyEditSkip==0 (B1.3a-d) this is inert; gEditBoxCount==0 no-op.
    if (gApplyEditSkip != 0u && gEditBoxCount != 0u) {
        const int editX0 = MidMeshCellWorldX(meta.originX, x, cellSizeIntEarly);
        const int editZ0 = MidMeshCellWorldZ(meta.originZ, z, cellSizeIntEarly);
        const int editX1 = MidMeshCellWorldX(meta.originX, xEnd, cellSizeIntEarly);
        const int editZ1 = MidMeshCellWorldZ(meta.originZ, zEnd, cellSizeIntEarly);
        if (MidMeshCellInEditFootprint(EditBoxes, gEditBoxBase, gEditBoxCount,
                                       editX0, editZ0, editX1, editZ1)) {
            return; // whole-block skip (matches the CPU `continue`)
        }
    }

    // ---- B1.3d CHILD-QUADRANT SUPPRESSION (block center) ----
    // Mirrors the CPU's L7 finer-coverage suppression `continue`. For mergeCells>1 the CPU
    // uses the BLOCK CENTER cell, not a single cell: midCell = x + (xEnd-x)/2,
    // midCellZ = z + (zEnd-z)/2, qx = (midCell*2 >= cellCount)?1:0 (cellCount == cellsPerAxis).
    // MidMeshCellSuppressedByChild takes a (cell, cellsPerAxis) pair and computes the same
    // qx/qz from `cell*2 >= cellsPerAxis`, so passing the block-center cell reproduces it
    // exactly (for mergeCells==1, midCell==cx, byte-identical to B1.3d). The CPU only applies
    // it when anyChildResident; childMask != 0 is that condition.
    if (gApplyChildSuppression != 0u && meta.childMask != 0u) {
        const uint midCell = x + (xEnd - x) / 2u;
        const uint midCellZ = z + (zEnd - z) / 2u;
        // MidMeshCellSuppressedByChild(childMask, cellsPerAxis, midCell, midCellZ) -> the
        // CPU qx=(midCell*2>=cellCount), qz=(midCellZ*2>=cellCount), bit qz*2+qx.
        if (MidMeshCellSuppressedByChild(meta.childMask, cellsPerAxis, midCell, midCellZ)) {
            return; // whole-block skip (matches the CPU continue)
        }
    }

    const int cellSizeInt = cellSizeIntEarly;
    const uint heightBias = meta.heightBias;
    const uint terraceStep = gTerraceStep;

    // ---- this block's aggregated surface (aggregateSamples(x,z,xEnd,zEnd)) ----
    // For mergeCells==1 this is the single sample (x,z), byte-identical to B1.3a-e.
    // B1.3f-b WATER-AWARE: gEmitWater==0 reduces to the B1.3f-a solid-only result
    // EXACTLY (water/air samples skipped, block.solid). gEmitWater==1 lets water
    // samples participate (water-only blocks present + the shoal min-height override).
    const bool emitWater = (gEmitWater != 0u);
    MidMeshSurfaceBlock block = MidMeshAggregateBlock(
        Samples, slot, stride, side, x, z, xEnd, zEnd,
        heightBias, terraceStep, emitWater);
    if (!block.present) {
        // ALL-AIR FOOTPRINT FILL (mirrors extractTileMesh ~7065-7083): a block whose
        // samples are all Air emits NOTHING under water-off (the CPU `continue`). With
        // water ON, fill it with a sea-level WATER top so the hole seals AND the block
        // becomes `present`, so adjacent land seals risers against it. (The fill is
        // applied ONLY to the CURRENT block, never to the neighbor probes below - exactly
        // like the CPU, where the fill sits in the loop body, not inside aggregateSamples.)
        if (!emitWater) {
            return; // WATER-OFF: no top, neighbors emit no riser toward this block.
        }
        block.present = true;
        block.solid = false;
        block.water = true;
        block.height = MidMeshQuantizeY(gSeaLevelY, terraceStep);
        block.material = kMidMeshMaterialWater;
    }
    const int height = block.height;
    const uint material = block.material;

    const int worldX = MidMeshCellWorldX(meta.originX, x, cellSizeInt);
    const int worldZ = MidMeshCellWorldZ(meta.originZ, z, cellSizeInt);
    // width/depth = the BLOCK's world span = (xEnd-x)*cellSize / (zEnd-z)*cellSize. For
    // mergeCells==1 (xEnd-x==1) this is cellSize, identical to B1.3a-e. A merged block is
    // LARGER (up to mergeCells*cellSize), so a top quad > 32u splits into addFace chunks.
    const uint width = (xEnd - x) * (uint)cellSizeInt;
    const uint depth = (zEnd - z) * (uint)cellSizeInt;

    // ---- TOP FACE (B1.3a) ----
    // voxel = PackMidHeightSurfaceVoxel(material, worldX, height, worldZ), then masked
    // into the 19-bit payload field by MidMeshPackPayload (matches PackSparseSurfacePayload).
    const uint voxel = MidMeshPackVoxel(material, worldX, height, worldZ);
    // addFace SPLIT-LIMIT chunking parity: a top quad wider/deeper than 32u is emitted as
    // MULTIPLE sub-faces. For PosY: worldX += wOff, worldZ += hOff.
    MidMeshAddFace(slot, kSparseDirPosY, worldX, height, worldZ, width, depth, voxel);

    // ---- TILE-BORDER SKIRTS (B1.3c) ----
    // CPU order: top face -> border skirts -> risers. The skirt is SOLID-only: the CPU
    // guards it with `if (!block.water)` (SparseClipmap.cpp ~7128). Under water-off every
    // present block is solid so the guard is implicit; under water-on a water block (incl.
    // the all-air sea-level fill) emits NO skirt, so we must mirror the explicit guard.
    // Border-edge predicates mirror the CPU's BLOCK rule: x==0, xEnd>=cellCount, z==0,
    // zEnd>=cellCount (cellCount == cellsPerAxis). Skirts use the top face's voxel + span.
    if (gEmitSkirts != 0u && !block.water) {
        const bool atNegX = (x == 0u);
        const bool atPosX = (xEnd >= cellsPerAxis);
        const bool atNegZ = (z == 0u);
        const bool atPosZ = (zEnd >= cellsPerAxis);
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
    // Only when enabled. NEIGHBOR = the ADJACENT BLOCK (not the next cell): the CPU reads
    // rightBlock = aggregateSamples(xEnd, z, xEnd+mergeCells, zEnd), forwardBlock =
    // aggregateSamples(x, zEnd, xEnd, zEnd+mergeCells). aggregateSamples CLAMPS to the tile,
    // so a border block sees itself as its own neighbor (equal heights -> no riser crosses a
    // tile boundary; the skirts seal those). For mergeCells==1 xEnd==cx+1 etc., identical to
    // B1.3b. The CPU rises on `present && height != neighborHeight` (present == solid under
    // water-off), using the higher block's material when current is higher, else the
    // neighbor's material.
    if (gEmitRisers == 0u) {
        return;
    }

    // B1.3f-b: the neighbor probes are WATER-AWARE too (gEmitWater carried), but the CPU
    // does NOT apply the all-air fill to the neighbor (the fill is in the loop body, not in
    // aggregateSamples). So an all-air neighbor probe returns !present and emits no riser
    // toward it here; that area's riser comes from the all-air-filled block's own iteration.
    // The CPU riser test is `neighbor.present && height != neighbor.height` (present, not
    // solid) - water blocks (present) now participate, identical to the CPU.

    // RIGHT NEIGHBOR BLOCK: aggregateSamples(xEnd, z, xEnd+mergeCells, zEnd).
    {
        MidMeshSurfaceBlock rightBlock = MidMeshAggregateBlock(
            Samples, slot, stride, side,
            xEnd, z, xEnd + mergeCells, zEnd,
            heightBias, terraceStep, emitWater);
        if (rightBlock.present && height != rightBlock.height) {
            const int lowTopY = min(height, rightBlock.height) + 1;
            const uint riserHeight = (uint)abs(height - rightBlock.height);
            const bool currentHigher = (height > rightBlock.height);
            // boundary = (worldX + width, worldZ); span = depth (the block's Z span);
            // dir PosX if current higher.
            MidMeshEmitRiser(
                slot,
                currentHigher ? kSparseDirPosX : kSparseDirNegX,
                worldX + (int)width,
                worldZ,
                lowTopY,
                depth,
                riserHeight,
                currentHigher ? material : rightBlock.material);
        }
    }

    // FORWARD NEIGHBOR BLOCK: aggregateSamples(x, zEnd, xEnd, zEnd+mergeCells).
    {
        MidMeshSurfaceBlock forwardBlock = MidMeshAggregateBlock(
            Samples, slot, stride, side,
            x, zEnd, xEnd, zEnd + mergeCells,
            heightBias, terraceStep, emitWater);
        if (forwardBlock.present && height != forwardBlock.height) {
            const int lowTopY = min(height, forwardBlock.height) + 1;
            const uint riserHeight = (uint)abs(height - forwardBlock.height);
            const bool currentHigher = (height > forwardBlock.height);
            // boundary = (worldX, worldZ + depth); span = width (the block's X span);
            // dir PosZ if current higher.
            MidMeshEmitRiser(
                slot,
                currentHigher ? kSparseDirPosZ : kSparseDirNegZ,
                worldX,
                worldZ + (int)depth,
                lowTopY,
                width,
                riserHeight,
                currentHigher ? material : forwardBlock.material);
        }
    }
}
