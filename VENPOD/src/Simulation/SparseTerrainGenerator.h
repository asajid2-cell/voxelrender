#pragma once

#include "SparseVoxelTypes.h"

#include <array>
#include <cstdint>

namespace VENPOD::Simulation {

struct GeneratedSparseBrick {
    BrickCoord coord;
    std::array<uint32_t, SPARSE_BRICK_VOXEL_COUNT> voxels = {};
    uint32_t flags = 0;
    uint32_t occupancyWord0 = 0;
    uint32_t occupancyWord1 = 0;
};

class SparseTerrainGenerator {
public:
    explicit SparseTerrainGenerator(uint32_t seed = 12345u) : m_seed(seed) {}

    uint32_t Seed() const { return m_seed; }
    float HeightAt(int32_t worldX, int32_t worldZ) const;
    uint32_t SampleGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    GeneratedSparseBrick GenerateBrick(const BrickCoord& coord) const;

    static void ComputeOccupancyAndFlags(GeneratedSparseBrick& brick);

private:
    static float Smooth01(float value);
    static float Lerp(float a, float b, float t);
    static float ValueNoise2D(float x, float z, uint32_t seed);
    static uint32_t Hash3D(int32_t x, int32_t y, int32_t z, uint32_t seed);

    uint32_t m_seed = 12345u;
};

} // namespace VENPOD::Simulation

