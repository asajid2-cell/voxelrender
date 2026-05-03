#include "SparseSurfaceExtractor.h"

#include "Utils/BitPacking.h"

#include <array>

namespace VENPOD::Simulation {

namespace {

struct DirectionInfo {
    int32_t dx;
    int32_t dy;
    int32_t dz;
    SparseFaceDirection direction;
};

constexpr std::array<DirectionInfo, 6> kDirections = {{
    {-1, 0, 0, SparseFaceDirection::NegX},
    {1, 0, 0, SparseFaceDirection::PosX},
    {0, -1, 0, SparseFaceDirection::NegY},
    {0, 1, 0, SparseFaceDirection::PosY},
    {0, 0, -1, SparseFaceDirection::NegZ},
    {0, 0, 1, SparseFaceDirection::PosZ},
}};

uint32_t LocalVoxelAt(const GeneratedSparseBrick& brick, int32_t x, int32_t y, int32_t z) {
    const auto index =
        static_cast<uint32_t>(x) +
        static_cast<uint32_t>(y) * SPARSE_BRICK_SIZE +
        static_cast<uint32_t>(z) * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
    return brick.voxels[index];
}

} // namespace

bool SparseSurfaceExtractor::IsSolid(uint32_t voxel) {
    return !VENPOD::Utils::IsAir(voxel);
}

SparseSurfaceExtractionResult SparseSurfaceExtractor::Extract(
    const GeneratedSparseBrick& brick,
    const SparseNeighborSampler& neighborSampler)
{
    SparseSurfaceExtractionResult result;
    result.faces.reserve(1024);

    const int32_t baseX = brick.coord.x * SPARSE_BRICK_SIZE;
    const int32_t baseY = brick.coord.y * SPARSE_BRICK_SIZE;
    const int32_t baseZ = brick.coord.z * SPARSE_BRICK_SIZE;

    for (int32_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (int32_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (int32_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const uint32_t voxel = LocalVoxelAt(brick, x, y, z);
                if (!IsSolid(voxel)) {
                    continue;
                }

                ++result.stats.solidVoxels;
                const int32_t worldX = baseX + x;
                const int32_t worldY = baseY + y;
                const int32_t worldZ = baseZ + z;

                for (uint32_t dirIndex = 0; dirIndex < kDirections.size(); ++dirIndex) {
                    const DirectionInfo& dir = kDirections[dirIndex];
                    const int32_t nx = x + dir.dx;
                    const int32_t ny = y + dir.dy;
                    const int32_t nz = z + dir.dz;

                    uint32_t neighborVoxel = 0;
                    if (nx >= 0 && nx < SPARSE_BRICK_SIZE &&
                        ny >= 0 && ny < SPARSE_BRICK_SIZE &&
                        nz >= 0 && nz < SPARSE_BRICK_SIZE) {
                        neighborVoxel = LocalVoxelAt(brick, nx, ny, nz);
                    } else if (neighborSampler) {
                        neighborVoxel = neighborSampler(
                            worldX + dir.dx,
                            worldY + dir.dy,
                            worldZ + dir.dz);
                    }

                    if (IsSolid(neighborVoxel)) {
                        continue;
                    }

                    SparseSurfaceFace face;
                    face.worldX = worldX;
                    face.worldY = worldY;
                    face.worldZ = worldZ;
                    face.direction = static_cast<uint32_t>(dir.direction);
                    face.voxel = voxel;
                    result.faces.push_back(face);
                    ++result.stats.exposedFaces;
                    ++result.stats.facesByDirection[dirIndex];
                }
            }
        }
    }

    return result;
}

} // namespace VENPOD::Simulation
