#pragma once

#include "SparseTerrainGenerator.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace VENPOD::Simulation {

enum class SparseFaceDirection : uint32_t {
    NegX = 0,
    PosX = 1,
    NegY = 2,
    PosY = 3,
    NegZ = 4,
    PosZ = 5
};

struct SparseSurfaceFace {
    int32_t worldX = 0;
    int32_t worldY = 0;
    int32_t worldZ = 0;
    uint32_t payload = 0;
};

static_assert(sizeof(SparseSurfaceFace) == 16);

constexpr uint32_t kSparseSurfaceDirectionShift = 29u;
constexpr uint32_t kSparseSurfaceDirectionMask = 0x7u;
constexpr uint32_t kSparseSurfaceQuadWidthShift = 24u;
constexpr uint32_t kSparseSurfaceQuadHeightShift = 19u;
constexpr uint32_t kSparseSurfaceQuadExtentMask = 0x1Fu;
constexpr uint32_t kSparseSurfaceVoxelPayloadMask = 0x0007FFFFu;

inline uint32_t PackSparseSurfacePayload(uint32_t direction, uint32_t voxel, uint32_t width = 1u, uint32_t height = 1u) {
    const uint32_t packedWidth = (width > 0u ? width - 1u : 0u) & kSparseSurfaceQuadExtentMask;
    const uint32_t packedHeight = (height > 0u ? height - 1u : 0u) & kSparseSurfaceQuadExtentMask;
    return ((direction & kSparseSurfaceDirectionMask) << kSparseSurfaceDirectionShift) |
        (packedWidth << kSparseSurfaceQuadWidthShift) |
        (packedHeight << kSparseSurfaceQuadHeightShift) |
        (voxel & kSparseSurfaceVoxelPayloadMask);
}

inline uint32_t SparseSurfacePayloadDirection(uint32_t payload) {
    return (payload >> kSparseSurfaceDirectionShift) & kSparseSurfaceDirectionMask;
}

inline uint32_t SparseSurfacePayloadWidth(uint32_t payload) {
    return ((payload >> kSparseSurfaceQuadWidthShift) & kSparseSurfaceQuadExtentMask) + 1u;
}

inline uint32_t SparseSurfacePayloadHeight(uint32_t payload) {
    return ((payload >> kSparseSurfaceQuadHeightShift) & kSparseSurfaceQuadExtentMask) + 1u;
}

inline uint32_t SparseSurfacePayloadVoxel(uint32_t payload) {
    return payload & kSparseSurfaceVoxelPayloadMask;
}

// ===== SparseSurfaceFace HARD ABI (CPU <-> GPU) =====
// The 16-byte face is the exact contract the draw shader (VS_SparseSurface.hlsl) and the
// upcoming GPU extraction compute shader both consume. The payload bit layout MUST stay
// byte-identical across C++ and HLSL - a one-bit drift reads as "random geometry bugs".
// These asserts pin the layout at compile time; the HLSL side mirrors the same shifts/masks
// (FaceDirection >>29 &0x7, FaceWidth ((>>24)&0x1F)+1, FaceHeight ((>>19)&0x1F)+1,
// FaceVoxel &0x0007FFFF). Keep all three (this header, the VS, the extraction CS) in sync.
static_assert(sizeof(SparseSurfaceFace) == 16, "face ABI is 3x int32 + 1x uint32");
static_assert(kSparseSurfaceDirectionShift == 29u && kSparseSurfaceDirectionMask == 0x7u,
    "direction = bits 29..31");
static_assert(kSparseSurfaceQuadWidthShift == 24u && kSparseSurfaceQuadHeightShift == 19u &&
    kSparseSurfaceQuadExtentMask == 0x1Fu, "width = bits 24..28, height = bits 19..23 (5 each)");
static_assert(kSparseSurfaceVoxelPayloadMask == 0x0007FFFFu, "voxel = bits 0..18 (19 bits)");
// Fields are mutually exclusive and tile the full 32-bit payload (no overlap, no gap).
static_assert(
    ((kSparseSurfaceDirectionMask << kSparseSurfaceDirectionShift) |
     (kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadWidthShift) |
     (kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadHeightShift) |
     kSparseSurfaceVoxelPayloadMask) == 0xFFFFFFFFu,
    "face payload fields must tile all 32 bits exactly");
static_assert(
    ((kSparseSurfaceDirectionMask << kSparseSurfaceDirectionShift) &
     (kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadWidthShift)) == 0u &&
    ((kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadWidthShift) &
     (kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadHeightShift)) == 0u &&
    ((kSparseSurfaceQuadExtentMask << kSparseSurfaceQuadHeightShift) &
     kSparseSurfaceVoxelPayloadMask) == 0u,
    "face payload fields must not overlap");

struct SparseSurfaceStats {
    uint32_t solidVoxels = 0;
    uint32_t exposedFaces = 0;
    std::array<uint32_t, 6> facesByDirection = {};
};

using SparseNeighborSampler =
    std::function<uint32_t(int32_t worldX, int32_t worldY, int32_t worldZ)>;

struct SparseSurfaceLocalRegion {
    uint8_t minX = 0;
    uint8_t minY = 0;
    uint8_t minZ = 0;
    uint8_t maxX = SPARSE_BRICK_SIZE - 1;
    uint8_t maxY = SPARSE_BRICK_SIZE - 1;
    uint8_t maxZ = SPARSE_BRICK_SIZE - 1;
};

struct SparseSurfaceExtractionResult {
    std::vector<SparseSurfaceFace> faces;
    SparseSurfaceStats stats;
};

class SparseSurfaceExtractor {
public:
    static SparseSurfaceExtractionResult Extract(
        const GeneratedSparseBrick& brick,
        const SparseNeighborSampler& neighborSampler = {});
    static SparseSurfaceExtractionResult ExtractRegion(
        const GeneratedSparseBrick& brick,
        const SparseSurfaceLocalRegion& region,
        const SparseNeighborSampler& neighborSampler = {});

    static bool IsSolid(uint32_t voxel);
};

} // namespace VENPOD::Simulation
