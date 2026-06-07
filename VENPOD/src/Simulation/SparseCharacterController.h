#pragma once

#include "SparseVoxelWorld.h"

#include <cstdint>

namespace VENPOD::Simulation {

struct SparseCharacterBody {
    float eyeX = 0.0f;
    float eyeY = 0.0f;
    float eyeZ = 0.0f;
    float height = 6.0f;
    float radius = 0.75f;
    float stepHeight = 2.5f;
};

struct SparseCharacterMoveRequest {
    SparseCharacterBody startBody{};
    SparseCharacterBody targetBody{};
    float verticalVelocity = 0.0f;
    bool allowStepUp = true;
    bool liquidsSupport = false;
    uint32_t maxSweepSteps = 16;
};

struct SparseCharacterMoveResult {
    float eyeX = 0.0f;
    float eyeY = 0.0f;
    float eyeZ = 0.0f;
    bool blocked = false;
    bool steppedUp = false;
    float safeFraction = 1.0f;
    uint32_t sampledVoxels = 0;
    uint32_t solidVoxels = 0;
    uint32_t liquidVoxels = 0;
};

struct SparseCharacterVerticalMoveRequest {
    SparseCharacterBody startBody{};
    SparseCharacterBody targetBody{};
    float verticalVelocity = 0.0f;
    bool liquidsSupport = false;
    uint32_t maxSweepSteps = 16;
};

struct SparseCharacterVerticalMoveResult {
    float eyeY = 0.0f;
    float verticalVelocity = 0.0f;
    bool blocked = false;
    bool landed = false;
    bool hitCeiling = false;
    float safeFraction = 1.0f;
    uint32_t sampledVoxels = 0;
    uint32_t solidVoxels = 0;
    uint32_t liquidVoxels = 0;
};

struct SparseCharacterGroundRequest {
    SparseCharacterBody body{};
    float verticalVelocity = 0.0f;
    float maxSnapUp = 0.25f;
    float maxSnapDown = 1.0f;
    bool liquidsSupport = false;
};

struct SparseCharacterGroundResult {
    float eyeY = 0.0f;
    float verticalVelocity = 0.0f;
    bool grounded = false;
    bool snapped = false;
    int32_t supportX = 0;
    int32_t supportY = 0;
    int32_t supportZ = 0;
    uint32_t sampledVoxels = 0;
    uint32_t solidVoxels = 0;
    uint32_t liquidVoxels = 0;
};

SparseCollisionAabb MakeSparseCharacterBodyAabb(const SparseCharacterBody& body);
SparseCollisionAabb MakeSparseCharacterFootprintAabb(const SparseCharacterBody& body, float feetY);
SparseCharacterMoveResult ResolveSparseCharacterHorizontalMove(
    const SparseVoxelWorld& world,
    const SparseCharacterMoveRequest& request);
SparseCharacterVerticalMoveResult ResolveSparseCharacterVerticalMove(
    const SparseVoxelWorld& world,
    const SparseCharacterVerticalMoveRequest& request);
SparseCharacterGroundResult ResolveSparseCharacterGrounding(
    const SparseVoxelWorld& world,
    const SparseCharacterGroundRequest& request);

} // namespace VENPOD::Simulation
