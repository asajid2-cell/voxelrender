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

    struct ScenicSpawn {
        bool found = false;
        int32_t worldX = 96;
        int32_t worldZ = 96;
        int32_t groundY = 0;
        float eyeY = 236.0f;
        float yaw = 0.0f;
        float pitch = -0.22f;
        float score = 0.0f;
        float forwardClearance = 0.0f;
        float localRelief = 0.0f;
    };

    uint32_t Seed() const { return m_seed; }
    float HeightAt(int32_t worldX, int32_t worldZ) const;
    ScenicSpawn FindScenicSpawn(
        int32_t originX,
        int32_t originZ,
        float playerHeight,
        int32_t searchRadius = 448,
        int32_t sampleSpacing = 32) const;
    bool IsDefinitelyEmptyBrick(const BrickCoord& coord, float verticalSafetyMargin = 64.0f) const;
    uint32_t SampleGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const;
    uint32_t SampleGeneratedSurfaceVoxel(
        int32_t worldX,
        int32_t worldY,
        int32_t worldZ,
        int32_t sampleStep) const;
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
