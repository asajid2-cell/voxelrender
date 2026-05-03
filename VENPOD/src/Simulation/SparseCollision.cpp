#include "SparseCollision.h"

#include "Utils/BitPacking.h"

namespace VENPOD::Simulation {

CollisionSample SparseCollisionQuery::Sample(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    uint32_t voxel = 0;
    if (m_edits && m_edits->TryGetVoxel(worldX, worldY, worldZ, &voxel)) {
        return {ClassifyVoxel(voxel), voxel, true};
    }

    voxel = m_terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
    return {ClassifyVoxel(voxel), voxel, false};
}

CollisionSampleStatus SparseCollisionQuery::ClassifyVoxel(uint32_t voxel) {
    const uint8_t material = Utils::UnpackMaterial(voxel);
    if (material == Utils::Material::Air) {
        return CollisionSampleStatus::KnownAir;
    }
    if (material == Utils::Material::Water ||
        material == Utils::Material::Lava ||
        material == Utils::Material::Oil) {
        return CollisionSampleStatus::KnownLiquid;
    }
    return CollisionSampleStatus::KnownSolid;
}

} // namespace VENPOD::Simulation

