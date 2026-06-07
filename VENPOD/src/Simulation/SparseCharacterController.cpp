#include "SparseCharacterController.h"

#include <algorithm>
#include <cmath>

namespace VENPOD::Simulation {

namespace {

constexpr float kDefaultCharacterHeight = 6.0f;
constexpr float kDefaultCharacterRadius = 0.75f;
constexpr float kDefaultCharacterStepHeight = 2.5f;
constexpr float kMinCharacterHeight = 0.5f;
constexpr float kMaxCharacterHeight = 128.0f;
constexpr float kMinCharacterRadius = 0.05f;
constexpr float kMaxCharacterRadius = 16.0f;
constexpr float kMaxCharacterStepHeight = 16.0f;
constexpr uint32_t kMaxCharacterSweepSteps = 1024;

float ClampFinite(float value, float minValue, float maxValue, float fallback) {
    return std::clamp(std::isfinite(value) ? value : fallback, minValue, maxValue);
}

bool TrySanitizeBody(const SparseCharacterBody& in, SparseCharacterBody& out) {
    if (!std::isfinite(in.eyeX) ||
        !std::isfinite(in.eyeY) ||
        !std::isfinite(in.eyeZ)) {
        return false;
    }

    out = in;
    out.height = ClampFinite(in.height, kMinCharacterHeight, kMaxCharacterHeight, kDefaultCharacterHeight);
    out.radius = ClampFinite(in.radius, kMinCharacterRadius, kMaxCharacterRadius, kDefaultCharacterRadius);
    out.stepHeight = ClampFinite(in.stepHeight, 0.0f, kMaxCharacterStepHeight, kDefaultCharacterStepHeight);
    return true;
}

uint32_t BuildSweepSteps(float distance, uint32_t maxSweepSteps) {
    if (!std::isfinite(distance) || distance <= 0.0f) {
        return 1u;
    }
    const uint32_t stepLimit = std::clamp(maxSweepSteps, 1u, kMaxCharacterSweepSteps);
    const double rawSteps = std::ceil(static_cast<double>(distance) / 0.25);
    if (!std::isfinite(rawSteps) || rawSteps <= 1.0) {
        return 1u;
    }
    if (rawSteps >= static_cast<double>(stepLimit)) {
        return stepLimit;
    }
    return static_cast<uint32_t>(rawSteps);
}

} // namespace

SparseCollisionAabb MakeSparseCharacterBodyAabb(const SparseCharacterBody& body) {
    const float feetY = body.eyeY - body.height;
    return SparseCollisionAabb{
        body.eyeX - body.radius,
        feetY + 0.65f,
        body.eyeZ - body.radius,
        body.eyeX + body.radius,
        body.eyeY - 0.35f,
        body.eyeZ + body.radius
    };
}

SparseCollisionAabb MakeSparseCharacterFootprintAabb(
    const SparseCharacterBody& body,
    float feetY) {
    return SparseCollisionAabb{
        body.eyeX - body.radius,
        feetY + 0.25f,
        body.eyeZ - body.radius,
        body.eyeX + body.radius,
        feetY + 0.35f,
        body.eyeZ + body.radius
    };
}

SparseCharacterMoveResult ResolveSparseCharacterHorizontalMove(
    const SparseVoxelWorld& world,
    const SparseCharacterMoveRequest& request) {
    SparseCharacterMoveResult result;
    SparseCharacterBody startBody;
    SparseCharacterBody targetBody;
    const bool startValid = TrySanitizeBody(request.startBody, startBody);
    if (!startValid || !TrySanitizeBody(request.targetBody, targetBody)) {
        if (startValid) {
            result.eyeX = startBody.eyeX;
            result.eyeY = startBody.eyeY;
            result.eyeZ = startBody.eyeZ;
        }
        result.blocked = true;
        result.safeFraction = 0.0f;
        return result;
    }

    result.eyeX = targetBody.eyeX;
    result.eyeY = targetBody.eyeY;
    result.eyeZ = targetBody.eyeZ;

    const float verticalVelocity = std::isfinite(request.verticalVelocity)
        ? request.verticalVelocity
        : 0.0f;
    const float deltaX = targetBody.eyeX - startBody.eyeX;
    const float deltaZ = targetBody.eyeZ - startBody.eyeZ;
    const float horizontalDistance = static_cast<float>(std::sqrt(
        static_cast<double>(deltaX) * static_cast<double>(deltaX) +
        static_cast<double>(deltaZ) * static_cast<double>(deltaZ)));
    if (horizontalDistance < 0.0001f) {
        return result;
    }

    const uint32_t sweepSteps = BuildSweepSteps(horizontalDistance, request.maxSweepSteps);

    SparseCharacterBody sweepStart = startBody;
    sweepStart.eyeY = targetBody.eyeY;
    const SparseCollisionSweepResult sweep = world.SweepCollisionAabb(
        MakeSparseCharacterBodyAabb(sweepStart),
        deltaX,
        0.0f,
        deltaZ,
        sweepSteps);
    result.safeFraction = sweep.safeFraction;
    result.sampledVoxels += sweep.hitVolume.sampledVoxels;
    result.solidVoxels += sweep.hitVolume.solidVoxels;
    result.liquidVoxels += sweep.hitVolume.liquidVoxels;

    if (!sweep.blocked) {
        return result;
    }

    result.blocked = true;

    if (request.allowStepUp && verticalVelocity <= 0.0f) {
        SparseCharacterBody elevatedTarget = targetBody;
        elevatedTarget.eyeY += targetBody.stepHeight;
        const SparseCollisionVolumeResult elevatedBody =
            world.TestCollisionAabb(MakeSparseCharacterBodyAabb(elevatedTarget));
        result.sampledVoxels += elevatedBody.sampledVoxels;
        result.solidVoxels += elevatedBody.solidVoxels;
        result.liquidVoxels += elevatedBody.liquidVoxels;

        if (!elevatedBody.blocked) {
            const float currentFeetY = targetBody.eyeY - targetBody.height;
            const SparseCollisionAabb stepFootProbe = MakeSparseCharacterFootprintAabb(
                elevatedTarget,
                currentFeetY + targetBody.stepHeight);
            const SparseCollisionSupportResult stepSupport =
                world.FindCollisionSupportBelow(
                    stepFootProbe,
                    targetBody.stepHeight + 0.75f,
                    request.liquidsSupport);
            result.sampledVoxels += stepSupport.sampledVoxels;
            result.solidVoxels += stepSupport.solidVoxels;
            result.liquidVoxels += stepSupport.liquidVoxels;

            if (stepSupport.found) {
                const float supportFeetY = static_cast<float>(stepSupport.supportY) + 1.0f;
                const bool withinStep =
                    supportFeetY >= currentFeetY - 0.1f &&
                    supportFeetY <= currentFeetY + targetBody.stepHeight + 0.35f;
                if (withinStep) {
                    result.eyeX = targetBody.eyeX;
                    result.eyeY = supportFeetY + targetBody.height;
                    result.eyeZ = targetBody.eyeZ;
                    result.steppedUp = true;
                    return result;
                }
            }
        }
    }

    SparseCharacterBody xOnly = targetBody;
    xOnly.eyeZ = sweepStart.eyeZ;
    SparseCharacterBody zOnly = targetBody;
    zOnly.eyeX = sweepStart.eyeX;
    const SparseCollisionVolumeResult xOnlyVolume =
        world.TestCollisionAabb(MakeSparseCharacterBodyAabb(xOnly));
    const SparseCollisionVolumeResult zOnlyVolume =
        world.TestCollisionAabb(MakeSparseCharacterBodyAabb(zOnly));
    result.sampledVoxels += xOnlyVolume.sampledVoxels + zOnlyVolume.sampledVoxels;
    result.solidVoxels += xOnlyVolume.solidVoxels + zOnlyVolume.solidVoxels;
    result.liquidVoxels += xOnlyVolume.liquidVoxels + zOnlyVolume.liquidVoxels;

    if (!xOnlyVolume.blocked && zOnlyVolume.blocked) {
        result.eyeX = targetBody.eyeX;
        result.eyeZ = sweepStart.eyeZ;
    } else if (xOnlyVolume.blocked && !zOnlyVolume.blocked) {
        result.eyeX = sweepStart.eyeX;
        result.eyeZ = targetBody.eyeZ;
    } else {
        result.eyeX = sweepStart.eyeX + deltaX * sweep.safeFraction;
        result.eyeZ = sweepStart.eyeZ + deltaZ * sweep.safeFraction;
    }
    return result;
}

SparseCharacterVerticalMoveResult ResolveSparseCharacterVerticalMove(
    const SparseVoxelWorld& world,
    const SparseCharacterVerticalMoveRequest& request) {
    SparseCharacterVerticalMoveResult result;
    SparseCharacterBody startBody;
    SparseCharacterBody targetBody;
    const bool startValid = TrySanitizeBody(request.startBody, startBody);
    if (!startValid || !TrySanitizeBody(request.targetBody, targetBody)) {
        if (startValid) {
            result.eyeY = startBody.eyeY;
        }
        result.verticalVelocity = 0.0f;
        result.blocked = true;
        result.safeFraction = 0.0f;
        return result;
    }

    result.eyeY = targetBody.eyeY;
    result.verticalVelocity = std::isfinite(request.verticalVelocity)
        ? request.verticalVelocity
        : 0.0f;

    const float deltaY = targetBody.eyeY - startBody.eyeY;
    if (std::abs(deltaY) < 0.0001f) {
        return result;
    }

    const float travelDistance = std::abs(deltaY);
    const uint32_t sweepSteps = BuildSweepSteps(travelDistance, request.maxSweepSteps);

    if (deltaY < 0.0f) {
        const float startFeetY = startBody.eyeY - startBody.height;
        const float targetFeetY = targetBody.eyeY - targetBody.height;
        const SparseCollisionAabb supportProbe =
            MakeSparseCharacterFootprintAabb(startBody, startFeetY + 0.25f);
        const SparseCollisionSupportResult support =
            world.FindCollisionSupportBelow(
                supportProbe,
                (startFeetY - targetFeetY) + 1.0f,
                request.liquidsSupport);
        result.sampledVoxels += support.sampledVoxels;
        result.solidVoxels += support.solidVoxels;
        result.liquidVoxels += support.liquidVoxels;

        if (support.found) {
            const float supportFeetY = static_cast<float>(support.supportY) + 1.0f;
            if (supportFeetY >= targetFeetY - 0.001f && supportFeetY <= startFeetY + 0.25f) {
                result.blocked = true;
                result.landed = true;
                result.eyeY = supportFeetY + targetBody.height;
                result.verticalVelocity = 0.0f;
                const float denominator = std::max(0.0001f, startFeetY - targetFeetY);
                result.safeFraction = std::clamp((startFeetY - supportFeetY) / denominator, 0.0f, 1.0f);
                return result;
            }
        }
    }

    const SparseCollisionSweepResult sweep = world.SweepCollisionAabb(
        MakeSparseCharacterBodyAabb(startBody),
        0.0f,
        deltaY,
        0.0f,
        sweepSteps);
    result.sampledVoxels += sweep.hitVolume.sampledVoxels;
    result.solidVoxels += sweep.hitVolume.solidVoxels;
    result.liquidVoxels += sweep.hitVolume.liquidVoxels;
    result.safeFraction = sweep.safeFraction;
    if (!sweep.blocked) {
        return result;
    }

    result.blocked = true;
    result.hitCeiling = deltaY > 0.0f;
    result.eyeY = startBody.eyeY + deltaY * sweep.safeFraction;
    result.verticalVelocity = 0.0f;
    return result;
}

SparseCharacterGroundResult ResolveSparseCharacterGrounding(
    const SparseVoxelWorld& world,
    const SparseCharacterGroundRequest& request) {
    SparseCharacterGroundResult result;
    SparseCharacterBody body;
    if (!TrySanitizeBody(request.body, body)) {
        result.verticalVelocity = 0.0f;
        return result;
    }

    result.eyeY = body.eyeY;
    result.verticalVelocity = std::isfinite(request.verticalVelocity)
        ? request.verticalVelocity
        : 0.0f;

    const float feetY = body.eyeY - body.height;
    const float snapUp = ClampFinite(request.maxSnapUp, 0.0f, kMaxCharacterStepHeight, 0.25f);
    const float snapDown = ClampFinite(request.maxSnapDown, 0.0f, kMaxCharacterHeight, 1.0f);
    if (snapUp <= 0.0f && snapDown <= 0.0f) {
        return result;
    }

    const SparseCollisionAabb supportProbe =
        MakeSparseCharacterFootprintAabb(body, feetY + snapUp);
    const SparseCollisionSupportResult support =
        world.FindCollisionSupportBelow(supportProbe, snapUp + snapDown, request.liquidsSupport);
    result.sampledVoxels = support.sampledVoxels;
    result.solidVoxels = support.solidVoxels;
    result.liquidVoxels = support.liquidVoxels;

    if (!support.found) {
        return result;
    }

    const float supportFeetY = static_cast<float>(support.supportY) + 1.0f;
    const bool withinVerticalRange =
        supportFeetY >= feetY - snapDown - 0.001f &&
        supportFeetY <= feetY + snapUp + 0.001f;
    if (!withinVerticalRange) {
        return result;
    }

    result.grounded = true;
    result.supportX = support.supportX;
    result.supportY = support.supportY;
    result.supportZ = support.supportZ;
    result.eyeY = supportFeetY + request.body.height;
    result.verticalVelocity = 0.0f;
    result.snapped = std::abs(result.eyeY - body.eyeY) > 0.001f;
    return result;
}

} // namespace VENPOD::Simulation
