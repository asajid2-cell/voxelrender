#pragma once

#include "SparseVoxelTypes.h"

#include <cstdint>
#include <vector>

namespace VENPOD::Simulation {

struct SparseBrickRequestPlannerConfig {
    uint32_t radiusXz = 2;
    uint32_t radiusY = 1;
    uint32_t forwardPrefetchBricks = 2;
    uint32_t maxRequests = 128;
};

struct SparseBrickRequest {
    BrickCoord coord;
    int32_t priority = 0;
};

struct SparseViewConeConfig {
    float originX = 0.0f;
    float originY = 0.0f;
    float originZ = 0.0f;
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    float forwardZ = 1.0f;
    float rightX = 1.0f;
    float rightY = 0.0f;
    float rightZ = 0.0f;
    float upX = 0.0f;
    float upY = 1.0f;
    float upZ = 0.0f;
    float verticalFovRadians = 1.04719755f;
    float aspectRatio = 1.7777778f;
    float maxDistance = 192.0f;
    float stepDistance = 16.0f;
    uint32_t rayGrid = 3;
    uint32_t maxRequests = 64;
};

class SparseBrickRequestPlanner {
public:
    explicit SparseBrickRequestPlanner(SparseBrickRequestPlannerConfig config = {});

    const SparseBrickRequestPlannerConfig& GetConfig() const { return m_config; }
    void SetConfig(const SparseBrickRequestPlannerConfig& config);

    std::vector<SparseBrickRequest> Plan(
        const BrickCoord& center,
        int32_t forwardX,
        int32_t forwardY,
        int32_t forwardZ) const;

    std::vector<SparseBrickRequest> PlanViewCone(const SparseViewConeConfig& view) const;

private:
    SparseBrickRequestPlannerConfig m_config;
};

} // namespace VENPOD::Simulation
