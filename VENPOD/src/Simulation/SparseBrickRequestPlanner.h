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

enum class SparseBrickRequestSource : uint8_t {
    Generic = 0,
    ViewCone = 1,
    Collision = 2,
    NearVisible = 3,
    MotionVisible = 4,
    OwnershipRecovery = 5,
    SpeculativeView = 6,
    Stress = 7
};

static constexpr uint32_t kSparseBrickRequestSourceCount = 8u;

struct SparseBrickRequest {
    BrickCoord coord;
    int32_t priority = 0;
    SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
    bool urgent = false;
    SparseBrickRequestSource source = SparseBrickRequestSource::Generic;
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
    uint32_t coverageRadiusXz = 0;
    uint32_t coverageRadiusY = 0;
    uint32_t maxRequests = 64;
};

struct SparseCollisionResidencyConfig {
    BrickCoord center;
    float cameraX = 0.0f;
    float cameraY = 0.0f;
    float cameraZ = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float velocityZ = 0.0f;
    float predictionSeconds = 0.25f;
    float bodyHeight = 6.0f;
    float bodyRadius = 0.75f;
    float stepHeight = 2.5f;
    float supportDrop = 4.0f;
    bool brushIntentValid = false;
    float brushStartX = 0.0f;
    float brushStartY = 0.0f;
    float brushStartZ = 0.0f;
    float brushEndX = 0.0f;
    float brushEndY = 0.0f;
    float brushEndZ = 0.0f;
    float brushRadius = 5.0f;
    uint32_t shellRadiusXz = 1;
    uint32_t shellRadiusY = 1;
    uint32_t predictionBricks = 2;
    uint32_t maxIntentSamples = 16;
    uint32_t maxBrushRequests = 32;
    uint32_t reservedBrushRequests = 12;
    uint32_t maxRequests = 64;
};

struct SparseHierarchicalRequestConfig {
    BrickCoord center;
    float cameraX = 0.0f;
    float cameraY = 0.0f;
    float cameraZ = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float velocityZ = 0.0f;
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
    float visibleDistance = 256.0f;
    float speculativeDistance = 768.0f;
    float stepDistance = 16.0f;
    float predictionSeconds = 0.25f;
    float collisionBodyHeight = 6.0f;
    float collisionBodyRadius = 0.75f;
    float collisionStepHeight = 2.5f;
    float collisionSupportDrop = 4.0f;
    bool brushIntentValid = false;
    float brushStartX = 0.0f;
    float brushStartY = 0.0f;
    float brushStartZ = 0.0f;
    float brushEndX = 0.0f;
    float brushEndY = 0.0f;
    float brushEndZ = 0.0f;
    float brushRadius = 5.0f;
    uint32_t collisionRadiusXz = 1;
    uint32_t collisionRadiusY = 1;
    uint32_t collisionPredictionBricks = 2;
    uint32_t collisionMaxIntentSamples = 16;
    uint32_t maxBrushCollisionRequests = 32;
    uint32_t reservedBrushCollisionRequests = 12;
    uint32_t nearVisibleRadiusXz = 2;
    uint32_t nearVisibleRadiusY = 1;
    uint32_t maxNearVisibleRequests = 32;
    float motionVisibleMinSpeed = 64.0f;
    uint32_t motionVisibleRadiusXz = 2;
    uint32_t motionVisibleRadiusY = 1;
    uint32_t maxMotionVisibleRequests = 48;
    uint32_t ownershipPressureLevel = 0;
    uint32_t maxOwnershipRecoveryRequests = 0;
    uint32_t visibleRayGrid = 3;
    uint32_t speculativeRayGrid = 3;
    uint32_t maxCollisionRequests = 64;
    uint32_t maxVisibleRequests = 96;
    uint32_t maxSpeculativeRequests = 96;
    uint32_t maxRequests = 192;
};

struct SparseStressRequestConfig {
    BrickCoord center;
    uint32_t radiusXz = 5;
    uint32_t radiusY = 2;
    uint32_t maxRequests = 128;
    uint32_t cursor = 0;
    bool includeCollisionCore = true;
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
    std::vector<SparseBrickRequest> PlanCollisionResidency(const SparseCollisionResidencyConfig& config) const;
    std::vector<SparseBrickRequest> PlanHierarchical(const SparseHierarchicalRequestConfig& config) const;
    std::vector<SparseBrickRequest> PlanStressVolume(const SparseStressRequestConfig& config) const;

private:
    SparseBrickRequestPlannerConfig m_config;
};

} // namespace VENPOD::Simulation
