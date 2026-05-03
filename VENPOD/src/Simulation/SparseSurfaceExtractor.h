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
    uint32_t direction = static_cast<uint32_t>(SparseFaceDirection::PosY);
    uint32_t voxel = 0;
};

static_assert(sizeof(SparseSurfaceFace) == 20);

struct SparseSurfaceStats {
    uint32_t solidVoxels = 0;
    uint32_t exposedFaces = 0;
    std::array<uint32_t, 6> facesByDirection = {};
};

using SparseNeighborSampler =
    std::function<uint32_t(int32_t worldX, int32_t worldY, int32_t worldZ)>;

struct SparseSurfaceExtractionResult {
    std::vector<SparseSurfaceFace> faces;
    SparseSurfaceStats stats;
};

class SparseSurfaceExtractor {
public:
    static SparseSurfaceExtractionResult Extract(
        const GeneratedSparseBrick& brick,
        const SparseNeighborSampler& neighborSampler = {});

    static bool IsSolid(uint32_t voxel);
};

} // namespace VENPOD::Simulation
