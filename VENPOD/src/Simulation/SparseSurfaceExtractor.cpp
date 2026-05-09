#include "SparseSurfaceExtractor.h"

#include "Utils/BitPacking.h"

#include <array>
#include <algorithm>

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

uint32_t VoxelAtOrNeighbor(
    const GeneratedSparseBrick& brick,
    int32_t x,
    int32_t y,
    int32_t z,
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    const DirectionInfo& dir,
    const SparseNeighborSampler& neighborSampler)
{
    if (x >= 0 && x < SPARSE_BRICK_SIZE &&
        y >= 0 && y < SPARSE_BRICK_SIZE &&
        z >= 0 && z < SPARSE_BRICK_SIZE) {
        return LocalVoxelAt(brick, x, y, z);
    }
    return neighborSampler
        ? neighborSampler(worldX + dir.dx, worldY + dir.dy, worldZ + dir.dz)
        : 0u;
}

void LocalFromFacePlane(
    SparseFaceDirection direction,
    int32_t fixed,
    int32_t u,
    int32_t v,
    int32_t& outX,
    int32_t& outY,
    int32_t& outZ)
{
    switch (direction) {
    case SparseFaceDirection::NegX:
    case SparseFaceDirection::PosX:
        outX = fixed;
        outY = v;
        outZ = u;
        break;
    case SparseFaceDirection::NegY:
    case SparseFaceDirection::PosY:
        outX = u;
        outY = fixed;
        outZ = v;
        break;
    case SparseFaceDirection::NegZ:
    case SparseFaceDirection::PosZ:
    default:
        outX = u;
        outY = v;
        outZ = fixed;
        break;
    }
}

void AxisRangesForDirection(
    SparseFaceDirection direction,
    int32_t minX,
    int32_t minY,
    int32_t minZ,
    int32_t maxX,
    int32_t maxY,
    int32_t maxZ,
    int32_t& fixedMin,
    int32_t& fixedMax,
    int32_t& uMin,
    int32_t& uMax,
    int32_t& vMin,
    int32_t& vMax)
{
    switch (direction) {
    case SparseFaceDirection::NegX:
    case SparseFaceDirection::PosX:
        fixedMin = minX;
        fixedMax = maxX;
        uMin = minZ;
        uMax = maxZ;
        vMin = minY;
        vMax = maxY;
        break;
    case SparseFaceDirection::NegY:
    case SparseFaceDirection::PosY:
        fixedMin = minY;
        fixedMax = maxY;
        uMin = minX;
        uMax = maxX;
        vMin = minZ;
        vMax = maxZ;
        break;
    case SparseFaceDirection::NegZ:
    case SparseFaceDirection::PosZ:
    default:
        fixedMin = minZ;
        fixedMax = maxZ;
        uMin = minX;
        uMax = maxX;
        vMin = minY;
        vMax = maxY;
        break;
    }
}

} // namespace

bool SparseSurfaceExtractor::IsSolid(uint32_t voxel) {
    return !VENPOD::Utils::IsAir(voxel);
}

SparseSurfaceExtractionResult SparseSurfaceExtractor::Extract(
    const GeneratedSparseBrick& brick,
    const SparseNeighborSampler& neighborSampler)
{
    SparseSurfaceLocalRegion fullRegion;
    return ExtractRegion(brick, fullRegion, neighborSampler);
}

SparseSurfaceExtractionResult SparseSurfaceExtractor::ExtractRegion(
    const GeneratedSparseBrick& brick,
    const SparseSurfaceLocalRegion& region,
    const SparseNeighborSampler& neighborSampler)
{
    SparseSurfaceExtractionResult result;
    result.faces.reserve(256);

    const int32_t baseX = brick.coord.x * SPARSE_BRICK_SIZE;
    const int32_t baseY = brick.coord.y * SPARSE_BRICK_SIZE;
    const int32_t baseZ = brick.coord.z * SPARSE_BRICK_SIZE;
    const int32_t minX = std::max<int32_t>(0, std::min<int32_t>(region.minX, SPARSE_BRICK_SIZE - 1));
    const int32_t minY = std::max<int32_t>(0, std::min<int32_t>(region.minY, SPARSE_BRICK_SIZE - 1));
    const int32_t minZ = std::max<int32_t>(0, std::min<int32_t>(region.minZ, SPARSE_BRICK_SIZE - 1));
    const int32_t maxX = std::max<int32_t>(minX, std::min<int32_t>(region.maxX, SPARSE_BRICK_SIZE - 1));
    const int32_t maxY = std::max<int32_t>(minY, std::min<int32_t>(region.maxY, SPARSE_BRICK_SIZE - 1));
    const int32_t maxZ = std::max<int32_t>(minZ, std::min<int32_t>(region.maxZ, SPARSE_BRICK_SIZE - 1));

    for (int32_t z = minZ; z <= maxZ; ++z) {
        for (int32_t y = minY; y <= maxY; ++y) {
            for (int32_t x = minX; x <= maxX; ++x) {
                if (IsSolid(LocalVoxelAt(brick, x, y, z))) {
                    ++result.stats.solidVoxels;
                }
            }
        }
    }

    uint32_t plane[SPARSE_BRICK_SIZE][SPARSE_BRICK_SIZE] = {};
    bool consumed[SPARSE_BRICK_SIZE][SPARSE_BRICK_SIZE] = {};

    for (uint32_t dirIndex = 0; dirIndex < kDirections.size(); ++dirIndex) {
        const DirectionInfo& dir = kDirections[dirIndex];
        int32_t fixedMin = 0;
        int32_t fixedMax = 0;
        int32_t uMin = 0;
        int32_t uMax = 0;
        int32_t vMin = 0;
        int32_t vMax = 0;
        AxisRangesForDirection(
            dir.direction,
            minX,
            minY,
            minZ,
            maxX,
            maxY,
            maxZ,
            fixedMin,
            fixedMax,
            uMin,
            uMax,
            vMin,
            vMax);

        for (int32_t fixed = fixedMin; fixed <= fixedMax; ++fixed) {
            for (auto& row : plane) {
                std::fill(std::begin(row), std::end(row), 0u);
            }
            for (auto& row : consumed) {
                std::fill(std::begin(row), std::end(row), false);
            }

            for (int32_t v = vMin; v <= vMax; ++v) {
                for (int32_t u = uMin; u <= uMax; ++u) {
                    int32_t x = 0;
                    int32_t y = 0;
                    int32_t z = 0;
                    LocalFromFacePlane(dir.direction, fixed, u, v, x, y, z);
                    const uint32_t voxel = LocalVoxelAt(brick, x, y, z);
                    if (!IsSolid(voxel)) {
                        continue;
                    }

                    const int32_t worldX = baseX + x;
                    const int32_t worldY = baseY + y;
                    const int32_t worldZ = baseZ + z;
                    const uint32_t neighborVoxel = VoxelAtOrNeighbor(
                        brick,
                        x + dir.dx,
                        y + dir.dy,
                        z + dir.dz,
                        worldX,
                        worldY,
                        worldZ,
                        dir,
                        neighborSampler);
                    if (IsSolid(neighborVoxel)) {
                        continue;
                    }

                    plane[v][u] = PackSparseSurfacePayload(static_cast<uint32_t>(dir.direction), voxel);
                    ++result.stats.exposedFaces;
                    ++result.stats.facesByDirection[dirIndex];
                }
            }

            for (int32_t v = vMin; v <= vMax; ++v) {
                for (int32_t u = uMin; u <= uMax; ++u) {
                    const uint32_t payload = plane[v][u];
                    if (payload == 0u || consumed[v][u]) {
                        continue;
                    }

                    int32_t width = 1;
                    while (u + width <= uMax &&
                           !consumed[v][u + width] &&
                           plane[v][u + width] == payload) {
                        ++width;
                    }

                    int32_t height = 1;
                    bool canGrow = true;
                    while (v + height <= vMax && canGrow) {
                        for (int32_t du = 0; du < width; ++du) {
                            if (consumed[v + height][u + du] ||
                                plane[v + height][u + du] != payload) {
                                canGrow = false;
                                break;
                            }
                        }
                        if (canGrow) {
                            ++height;
                        }
                    }

                    for (int32_t dv = 0; dv < height; ++dv) {
                        for (int32_t du = 0; du < width; ++du) {
                            consumed[v + dv][u + du] = true;
                        }
                    }

                    int32_t x = 0;
                    int32_t y = 0;
                    int32_t z = 0;
                    LocalFromFacePlane(dir.direction, fixed, u, v, x, y, z);
                    SparseSurfaceFace face;
                    face.worldX = baseX + x;
                    face.worldY = baseY + y;
                    face.worldZ = baseZ + z;
                    face.payload = PackSparseSurfacePayload(
                        static_cast<uint32_t>(dir.direction),
                        SparseSurfacePayloadVoxel(payload),
                        static_cast<uint32_t>(width),
                        static_cast<uint32_t>(height));
                    result.faces.push_back(face);
                }
            }
        }
    }

    return result;
}

} // namespace VENPOD::Simulation
