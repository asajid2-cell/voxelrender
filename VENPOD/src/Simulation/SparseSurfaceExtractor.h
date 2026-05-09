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
