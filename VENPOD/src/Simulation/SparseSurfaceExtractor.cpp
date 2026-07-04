#include "SparseSurfaceExtractor.h"

#include "Utils/BitPacking.h"

#include <array>
#include <algorithm>
#include <limits>

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

bool BrickHasFlag(const GeneratedSparseBrick& brick, BrickResidencyFlags flag) {
    return (brick.flags & static_cast<uint32_t>(flag)) != 0u;
}

bool IsFullBrickRegion(
    int32_t minX,
    int32_t minY,
    int32_t minZ,
    int32_t maxX,
    int32_t maxY,
    int32_t maxZ)
{
    return minX == 0 &&
        minY == 0 &&
        minZ == 0 &&
        maxX == SPARSE_BRICK_SIZE - 1 &&
        maxY == SPARSE_BRICK_SIZE - 1 &&
        maxZ == SPARSE_BRICK_SIZE - 1;
}

bool TryStepWorldVoxel(int32_t value, int32_t step, int32_t& out) {
    const int64_t stepped = static_cast<int64_t>(value) + static_cast<int64_t>(step);
    if (stepped < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        stepped > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(stepped);
    return true;
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
    if (!neighborSampler) {
        return 0u;
    }
    int32_t neighborX = 0;
    int32_t neighborY = 0;
    int32_t neighborZ = 0;
    if (!TryStepWorldVoxel(worldX, dir.dx, neighborX) ||
        !TryStepWorldVoxel(worldY, dir.dy, neighborY) ||
        !TryStepWorldVoxel(worldZ, dir.dz, neighborZ)) {
        return 0u;
    }
    return neighborSampler(neighborX, neighborY, neighborZ);
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
    const uint8_t material = VENPOD::Utils::UnpackMaterial(voxel);
    return material != VENPOD::Utils::Material::Air &&
        material != VENPOD::Utils::Material::Water;
}

bool IsSolidMaterial(uint8_t material) {
    return material != VENPOD::Utils::Material::Air &&
        material != VENPOD::Utils::Material::Water;
}

bool IsWater(uint32_t voxel) {
    return VENPOD::Utils::UnpackMaterial(voxel) == VENPOD::Utils::Material::Water;
}

bool IsAir(uint32_t voxel) {
    return VENPOD::Utils::UnpackMaterial(voxel) == VENPOD::Utils::Material::Air;
}

bool IsRenderableSurfaceFaceMaterials(
    uint8_t material,
    uint8_t neighborMaterial,
    SparseFaceDirection direction)
{
    if (IsSolidMaterial(material)) {
        if (neighborMaterial == VENPOD::Utils::Material::Air) {
            return true;
        }
        return neighborMaterial == VENPOD::Utils::Material::Water &&
            direction != SparseFaceDirection::PosY;
    }

    if (material == VENPOD::Utils::Material::Water) {
        // ALL water-vs-air boundaries emit (not just top/bottom). Top/bottom-only
        // was correct for flat sea sheets but rendered painted/sloped water (a
        // stream down a hillside) as scattered horizontal specks -- every side
        // boundary was invisible. Water-vs-water stays suppressed (no interior
        // faces), water-vs-solid stays suppressed (banks own those pixels).
        return neighborMaterial == VENPOD::Utils::Material::Air;
    }

    return false;
}

bool IsRenderableSurfaceFace(
    uint32_t voxel,
    uint32_t neighborVoxel,
    SparseFaceDirection direction)
{
    if (SparseSurfaceExtractor::IsSolid(voxel)) {
        if (IsAir(neighborVoxel)) {
            return true;
        }
        // Suppress solid top faces directly under water, but keep side/bottom
        // banks so sparse terrain remains continuous where water coverage is
        // not a complete screen-space owner.
        return IsWater(neighborVoxel) && direction != SparseFaceDirection::PosY;
    }

    if (IsWater(voxel)) {
        // All water-vs-air boundaries (see IsRenderableSurfaceFaceMaterials).
        return IsAir(neighborVoxel);
    }

    return false;
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

    const int32_t minX = std::max<int32_t>(0, std::min<int32_t>(region.minX, SPARSE_BRICK_SIZE - 1));
    const int32_t minY = std::max<int32_t>(0, std::min<int32_t>(region.minY, SPARSE_BRICK_SIZE - 1));
    const int32_t minZ = std::max<int32_t>(0, std::min<int32_t>(region.minZ, SPARSE_BRICK_SIZE - 1));
    const int32_t maxX = std::max<int32_t>(minX, std::min<int32_t>(region.maxX, SPARSE_BRICK_SIZE - 1));
    const int32_t maxY = std::max<int32_t>(minY, std::min<int32_t>(region.maxY, SPARSE_BRICK_SIZE - 1));
    const int32_t maxZ = std::max<int32_t>(minZ, std::min<int32_t>(region.maxZ, SPARSE_BRICK_SIZE - 1));
    int32_t baseX = 0;
    int32_t baseY = 0;
    int32_t baseZ = 0;
    int32_t regionMaxWorldX = 0;
    int32_t regionMaxWorldY = 0;
    int32_t regionMaxWorldZ = 0;
    if (!TryWorldVoxelFromBrickLocal(brick.coord.x, static_cast<uint8_t>(minX), &baseX) ||
        !TryWorldVoxelFromBrickLocal(brick.coord.y, static_cast<uint8_t>(minY), &baseY) ||
        !TryWorldVoxelFromBrickLocal(brick.coord.z, static_cast<uint8_t>(minZ), &baseZ) ||
        !TryWorldVoxelFromBrickLocal(brick.coord.x, static_cast<uint8_t>(maxX), &regionMaxWorldX) ||
        !TryWorldVoxelFromBrickLocal(brick.coord.y, static_cast<uint8_t>(maxY), &regionMaxWorldY) ||
        !TryWorldVoxelFromBrickLocal(brick.coord.z, static_cast<uint8_t>(maxZ), &regionMaxWorldZ)) {
        return result;
    }
    baseX -= minX;
    baseY -= minY;
    baseZ -= minZ;

    const bool fullSolidBrick =
        IsFullBrickRegion(minX, minY, minZ, maxX, maxY, maxZ) &&
        BrickHasFlag(brick, BrickResidencyFlags::Solid);
    if (fullSolidBrick) {
        result.stats.solidVoxels = SPARSE_BRICK_VOXEL_COUNT;
    } else {
        for (int32_t z = minZ; z <= maxZ; ++z) {
            for (int32_t y = minY; y <= maxY; ++y) {
                for (int32_t x = minX; x <= maxX; ++x) {
                    if (IsSolidMaterial(VENPOD::Utils::UnpackMaterial(LocalVoxelAt(brick, x, y, z)))) {
                        ++result.stats.solidVoxels;
                    }
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
        if (fullSolidBrick) {
            switch (dir.direction) {
            case SparseFaceDirection::NegX:
                fixedMin = fixedMax = 0;
                break;
            case SparseFaceDirection::PosX:
                fixedMin = fixedMax = SPARSE_BRICK_SIZE - 1;
                break;
            case SparseFaceDirection::NegY:
                fixedMin = fixedMax = 0;
                break;
            case SparseFaceDirection::PosY:
                fixedMin = fixedMax = SPARSE_BRICK_SIZE - 1;
                break;
            case SparseFaceDirection::NegZ:
                fixedMin = fixedMax = 0;
                break;
            case SparseFaceDirection::PosZ:
            default:
                fixedMin = fixedMax = SPARSE_BRICK_SIZE - 1;
                break;
            }
        }

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
                    const uint8_t material = VENPOD::Utils::UnpackMaterial(voxel);
                    if (!IsSolidMaterial(material) &&
                        material != VENPOD::Utils::Material::Water) {
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
                    const uint8_t neighborMaterial = VENPOD::Utils::UnpackMaterial(neighborVoxel);
                    if (!IsRenderableSurfaceFaceMaterials(material, neighborMaterial, dir.direction)) {
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

                    if (dir.direction == SparseFaceDirection::PosY &&
                        IsWater(SparseSurfacePayloadVoxel(payload)) &&
                        face.worldY < std::numeric_limits<int32_t>::max()) {
                        SparseSurfaceFace underside = face;
                        underside.worldY = face.worldY + 1;
                        underside.payload = PackSparseSurfacePayload(
                            static_cast<uint32_t>(SparseFaceDirection::NegY),
                            SparseSurfacePayloadVoxel(payload),
                            static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height));
                        result.faces.push_back(underside);
                        result.stats.exposedFaces += static_cast<uint32_t>(width * height);
                        result.stats.facesByDirection[static_cast<size_t>(SparseFaceDirection::NegY)] +=
                            static_cast<uint32_t>(width * height);
                    }
                }
            }
        }
    }

    return result;
}

} // namespace VENPOD::Simulation
