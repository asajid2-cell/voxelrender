// =============================================================================
// VENPOD GPU mid-mesh extraction - SHARED DECISION MACHINERY (Phase B1.3a)
// =============================================================================
// The COMMON machinery every B1.3a-f increment reuses, ported byte-for-byte from
// the CPU AUTHORITATIVE reference (extractTileMesh / BuildMidHeightSurfaceSnapshot
// in SparseClipmap.cpp). NOTHING here may diverge from the CPU: a one-bit drift
// reads as random geometry, and the A/B containment compare would surface it as a
// false "extra GPU face". Each helper names the exact CPU function it mirrors.
//
// Covered:
//   * sample decode (Y / material)                  <- UnpackMidHeightSurfaceSample*
//   * solid / water eligibility predicate           <- IsMidHeightSurfaceSolidMaterial
//   * terrace height quantization                    <- QuantizeMidHeightSurfaceY / FloorDiv
//   * per-cell voxel hash (variant) + pack           <- PackMidHeightSurfaceVoxel / PackVoxel
//   * SparseSurfaceFace 16B payload pack             <- PackSparseSurfacePayload
//   * tile + cell world coordinates                  <- cellWorldX/Z (extractTileMesh)
//   * the 80-byte per-tile metadata ABI              <- MidMeshGpuExtractTileMeta
// =============================================================================

#ifndef VENPOD_MIDMESH_EXTRACT_COMMON_HLSLI
#define VENPOD_MIDMESH_EXTRACT_COMMON_HLSLI

// ---- SparseSurfaceFace (16B HARD ABI) -- mirrors SparseSurfaceExtractor.h ----
struct SparseSurfaceFace {
    int worldX;
    int worldY;
    int worldZ;
    uint payload;
};

// ---- Per-tile metadata (5x uint4 = 80B) -- mirrors MidMeshGpuExtractTileMeta ----
// Field order MUST match the C++ struct byte-for-byte.
struct MidMeshTileMeta {
    int coordX;
    int coordRing;
    int coordZ;
    int originX;

    int originZ;
    uint cellSizeBits;     // float cellSize reinterpreted as uint
    uint mergeCells;       // LOD merge-cell size for this build
    uint childMask;        // finer-ring child residency (4 bits)

    uint meshContentVersionLo;
    uint meshContentVersionHi;
    uint sampleSide;       // side (grid is side*side)
    uint sampleStride;     // row stride in samples (== side for now)

    uint haloWidth;        // RESERVED for B1.3b+ border/skirt samples (0 for now)
    uint heightPackDesc;   // sample bit-layout descriptor (see PackMidMeshHeightDesc)
    uint heightBias;       // 32768 (added back to the unsigned height field)
    uint editFootprintCount;

    uint baseFace;         // reserved pending face-range base (production metadata)
    uint faceCapacity;     // per-tile capacity bound
    uint faceCount;        // GPU-written actual count (top-face path InterlockedAdds here)
    uint statusOverflow;   // status / overflow flag (1 = reserved range overflowed)
};

// =============================================================================
// SAMPLE DECODE - mirrors UnpackMidHeightSurfaceSampleY / ...SampleMaterial.
//   sample.Y        = int32((sample & 0xFFFF) - 32768)
//   sample.material = uint8((sample >> 16) & 0xFF)
// heightBias is carried in the metadata (32768) so the GPU never hardcodes it.
// =============================================================================
int MidMeshDecodeSampleY(uint sample, uint heightBias) {
    return (int)(sample & 0xFFFFu) - (int)heightBias;
}

uint MidMeshDecodeSampleMaterial(uint sample) {
    return (sample >> 16u) & 0xFFu;
}

// ---- Material constants (mirror Utils::Material) ----
static const uint kMidMeshMaterialAir = 0u;
static const uint kMidMeshMaterialWater = 2u;
// State flag carried in the packed voxel (mirror Utils::StateFlags::VisualSurface).
static const uint kMidMeshStateVisualSurface = 0x10u;

// ---- eligibility - mirrors IsMidHeightSurfaceSolidMaterial ----
bool MidMeshIsSolidMaterial(uint material) {
    return material != kMidMeshMaterialAir && material != kMidMeshMaterialWater;
}

bool MidMeshIsWaterMaterial(uint material) {
    return material == kMidMeshMaterialWater;
}

// =============================================================================
// TERRACE QUANTIZE - mirrors QuantizeMidHeightSurfaceY + FloorDiv.
// FloorDiv(y, step) is floor division toward negative infinity (NOT C trunc),
// then * step. Replicate floor-division exactly for negative Y.
// =============================================================================
int MidMeshFloorDiv(int a, int b) {
    // b is always >= 1 here (QuantizeMidHeightSurfaceY clamps step to >=1).
    int q = a / b;
    int r = a - q * b;
    // C division truncates toward zero; correct to floor for a negative remainder.
    if (r != 0 && ((r < 0) != (b < 0))) {
        q -= 1;
    }
    return q;
}

int MidMeshQuantizeY(int y, uint terraceStep) {
    int step = (int)max(1u, terraceStep);
    return MidMeshFloorDiv(y, step) * step;
}

// =============================================================================
// PER-CELL VOXEL HASH + PACK - mirrors PackMidHeightSurfaceVoxel + PackVoxel.
// FNV-1a over (x, y, z, 0x9E3779B9); low byte -> variant. Packed voxel:
//   material | (variant << 8) | (VisualSurface << 24)
// NOTE: the 32-bit packed voxel is later TRUNCATED to the face payload's 19-bit
// voxel field (bits 0..18), so only material (0..7) + variant (8..15) survive in
// the face; VisualSurface (bit 28) is masked away. We still compute the full pack
// for parity with the CPU's intermediate value, then mask on payload pack.
// =============================================================================
uint MidMeshPackVoxel(uint material, int x, int y, int z) {
    uint h = 2166136261u;
    h = (h ^ (uint)x) * 16777619u;
    h = (h ^ (uint)y) * 16777619u;
    h = (h ^ (uint)z) * 16777619u;
    h = (h ^ 0x9E3779B9u) * 16777619u;
    uint variant = h & 0xFFu;
    return (material & 0xFFu)
         | (variant << 8u)
         | (kMidMeshStateVisualSurface << 24u);
}

// =============================================================================
// FACE PAYLOAD PACK - mirrors PackSparseSurfacePayload (SparseSurfaceExtractor.h).
//   direction bits 29..31, width 24..28 (+1 encoded), height 19..23 (+1), voxel 0..18.
// =============================================================================
static const uint kSparseDirShift    = 29u;
static const uint kSparseDirMask      = 0x7u;
static const uint kSparseWidthShift   = 24u;
static const uint kSparseHeightShift  = 19u;
static const uint kSparseExtentMask   = 0x1Fu;
static const uint kSparseVoxelMask    = 0x0007FFFFu;

// Direction values mirror SparseFaceDirection (NegX=0..PosZ=5). PosY (top) = 3.
static const uint kSparseDirNegX = 0u;
static const uint kSparseDirPosX = 1u;
static const uint kSparseDirNegY = 2u;
static const uint kSparseDirPosY = 3u;
static const uint kSparseDirNegZ = 4u;
static const uint kSparseDirPosZ = 5u;

uint MidMeshPackPayload(uint direction, uint voxel, uint width, uint height) {
    uint packedWidth  = ((width  > 0u) ? (width  - 1u) : 0u) & kSparseExtentMask;
    uint packedHeight = ((height > 0u) ? (height - 1u) : 0u) & kSparseExtentMask;
    return ((direction & kSparseDirMask) << kSparseDirShift)
         | (packedWidth  << kSparseWidthShift)
         | (packedHeight << kSparseHeightShift)
         | (voxel & kSparseVoxelMask);
}

// =============================================================================
// TILE / CELL WORLD COORDS - mirrors extractTileMesh's cellWorldX / cellWorldZ
// (originX/Z + cell*cellSize). The CPU saturates the int32 add; cell indices here
// are bounded by cellCount (<= ~64), so cell*cellSize stays well inside int32 and
// a plain add matches the saturating add. cellSizeBits is the float cellSize's bit
// pattern: convert NUMERICALLY (truncate the float), matching the CPU which derives
// the integer cell size from the float record.cellSize.
// =============================================================================
int MidMeshCellSizeInt(uint cellSizeBits) {
    return max(1, (int)asfloat(cellSizeBits));
}

int MidMeshCellWorldX(int originX, uint cellIndexX, int cellSizeInt) {
    return originX + (int)cellIndexX * cellSizeInt;
}

int MidMeshCellWorldZ(int originZ, uint cellIndexZ, int cellSizeInt) {
    return originZ + (int)cellIndexZ * cellSizeInt;
}

// Linear sample index into a tile's row-major (stride x side) sample grid.
// slot * (stride * side) is the tile's base; cz * stride + cx is the in-tile offset.
uint MidMeshSampleIndex(uint slot, uint stride, uint side, uint cx, uint cz) {
    return slot * (stride * side) + (cz * stride + cx);
}

// =============================================================================
// EDIT-FOOTPRINT BOX (B1.3e) - mirrors extractTileMesh's internal `EditXzBox`
// (SparseClipmap.cpp). WORLD-VOXEL coordinates; 4x int32 = 16B, byte-identical to
// the C++ MidMeshEditXzBox the cache uploads.
// =============================================================================
struct MidMeshEditBox {
    int minX;
    int minZ;
    int maxX;
    int maxZ;
};

// =============================================================================
// cellInEditFootprint (B1.3e) - mirrors SparseClipmap.cpp byte-for-byte:
//   for (const EditXzBox& b : editXzBoxes)
//       if (b.minX <= x1 && b.maxX >= x0 && b.minZ <= z1 && b.maxZ >= z0) return true;
// The cell's world box is (x0,z0)=(cellWorldX(x),cellWorldZ(z)) and
// (x1,z1)=(cellWorldX(xEnd),cellWorldZ(zEnd)). For mergeCells==1, xEnd=x+1 so the
// cell box is [worldX, worldX+cellSize] x [worldZ, worldZ+cellSize] (INCLUSIVE of
// the next cell's start corner - the same width=cellSize the CPU passes). The
// overlap test uses INCLUSIVE bounds on BOTH sides (a standard AABB overlap). A
// cell that overlaps ANY edit box is skipped WHOLE (no top, no riser, no skirt) so
// the live voxel raymarch owns that area, identical to the CPU `continue`.
// =============================================================================
bool MidMeshCellInEditFootprint(
    StructuredBuffer<MidMeshEditBox> editBoxes,
    uint editBoxBase, uint editBoxCount,
    int x0, int z0, int x1, int z1)
{
    for (uint i = 0u; i < editBoxCount; ++i) {
        MidMeshEditBox b = editBoxes[editBoxBase + i];
        if (b.minX <= x1 && b.maxX >= x0 && b.minZ <= z1 && b.maxZ >= z0) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// SURFACE BLOCK (B1.3f-b) - mirrors the CPU `SurfaceBlock` (SparseClipmap.cpp):
//   bool present; bool solid; bool water; int height; uint material; (Air default).
// Carries the full WATER-AWARE aggregation result so the GPU can reproduce the
// CPU's water-mixed block decision (a water-only block, a shoal min-height
// override, the all-air water fill) exactly. The B1.3f-a SOLID-only aggregate was
// a degenerate case of this (emitWater==0 -> water samples skipped -> block.solid).
// =============================================================================
struct MidMeshSurfaceBlock {
    bool present;
    bool solid;
    bool water;
    int  height;
    uint material;
};

// =============================================================================
// B1.3f-b WATER-AWARE AGGREGATE - faithful port of extractTileMesh's
// `aggregateSamples` lambda (SparseClipmap.cpp ~6982-7048), the FULL water path.
// Byte-for-byte parity (the #1 risk of this increment):
//   * clamp the block span to the tile (same as the solid-only path).
//   * iterate z OUTER, x INNER (the CPU order) so the >= tie-break material matches.
//   * emitWater==0 -> skip every Water sample (CPU `!emitWater && Water -> continue`),
//     then skip air (`!solid && !water -> continue`) - reduces to the solid-only
//     B1.3f-a result EXACTLY (water counters stay 0, the shoal override never fires).
//   * emitWater==1 -> Water samples participate:
//       SOLID sample: ++solidCount; track minSolidHeight/minSolidMaterial; update the
//         block to SOLID on (!present || !solid || h >= height) (>= keeps the last
//         max-height solid's material; flips a water-present block to solid via !solid).
//       WATER sample: ++waterCount; set the block to WATER ONLY when !present (the very
//         first sample being water seeds a water block; a later solid then takes over).
//         A water sample NEVER overrides an already-present block (matches the CPU).
//   * SHOAL MIN-HEIGHT OVERRIDE: if (present && solid && waterCount>0 && minSolidHeight
//     valid) -> height = minSolidHeight, material = minSolidMaterial (a mixed water+solid
//     footprint hugs the water instead of floating at the solid MAX). Inert when
//     waterCount==0 (pure land keeps MAX) - so it never fires with water off.
// The CPU's `waterHeight`/`waterMaterial`/`solidCount` locals are write-only (never read
// after the loop), so they are intentionally not modeled; only waterCount + the minSolid*
// pair drive the result.
// =============================================================================
MidMeshSurfaceBlock MidMeshAggregateBlock(
    StructuredBuffer<uint> samples,
    uint slot, uint stride, uint side,
    uint x0, uint z0, uint x1, uint z1,
    uint heightBias, uint terraceStep, bool emitWater)
{
    MidMeshSurfaceBlock block;
    block.present = false;
    block.solid = false;
    block.water = false;
    block.height = 0;
    block.material = kMidMeshMaterialAir;

    // Clamp exactly like the CPU aggregateSamples.
    x0 = min(x0, side - 1u);
    z0 = min(z0, side - 1u);
    x1 = max(x0 + 1u, min(x1, side));
    z1 = max(z0 + 1u, min(z1, side));

    uint waterCount = 0u;
    int  minSolidHeight = 0x7FFFFFFF; // std::numeric_limits<int32_t>::max()
    uint minSolidMaterial = 0u;
    bool haveMinSolid = false;

    for (uint z = z0; z < z1; ++z) {
        for (uint x = x0; x < x1; ++x) {
            const uint sampleIndex = MidMeshSampleIndex(slot, stride, side, x, z);
            const uint sample = samples[sampleIndex];
            const uint material = MidMeshDecodeSampleMaterial(sample);
            // emitWater==0: skip Water (CPU `!emitWater && Water -> continue`).
            if (!emitWater && MidMeshIsWaterMaterial(material)) {
                continue;
            }
            const bool solid = MidMeshIsSolidMaterial(material);
            const bool water = MidMeshIsWaterMaterial(material);
            // Air (neither solid nor water) is skipped (CPU `!solid && !water -> continue`).
            if (!solid && !water) {
                continue;
            }
            const int h = MidMeshQuantizeY(MidMeshDecodeSampleY(sample, heightBias), terraceStep);
            if (solid) {
                if (!haveMinSolid || h < minSolidHeight) {
                    minSolidHeight = h;
                    minSolidMaterial = material;
                    haveMinSolid = true;
                }
                if (!block.present || !block.solid || h >= block.height) {
                    block.present = true;
                    block.solid = true;
                    block.water = false;
                    block.height = h;
                    block.material = material;
                }
            } else {
                ++waterCount;
                if (!block.present) {
                    block.present = true;
                    block.water = true;
                    block.height = h;
                    block.material = material;
                }
            }
        }
    }
    // SHOAL-HEIGHT FIX: mixed (water+solid) footprints hug the lowest solid sample.
    if (block.present && block.solid && waterCount > 0u && haveMinSolid) {
        block.height = minSolidHeight;
        block.material = minSolidMaterial;
    }
    return block;
}

#endif // VENPOD_MIDMESH_EXTRACT_COMMON_HLSLI
