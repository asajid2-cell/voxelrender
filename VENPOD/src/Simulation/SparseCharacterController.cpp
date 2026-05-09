#include "SparseCharacterController.h"

#include <algorithm>
#include <cmath>

namespace VENPOD::Simulation {

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
    result.eyeX = request.targetBody.eyeX;
    result.eyeY = request.targetBody.eyeY;
    result.eyeZ = request.targetBody.eyeZ;

    const float deltaX = request.targetBody.eyeX - request.startBody.eyeX;
    const float deltaZ = request.targetBody.eyeZ - request.startBody.eyeZ;
    const float horizontalDistance = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
    if (horizontalDistance < 0.0001f) {
        return result;
    }

    const uint32_t sweepSteps = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(horizontalDistance / 0.25f)),
        1,
        static_cast<int>(std::max(1u, request.maxSweepSteps))));

    SparseCharacterBody sweepStart = request.startBody;
    sweepStart.eyeY = request.targetBody.eyeY;
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

    if (request.allowStepUp && request.verticalVelocity <= 0.0f) {
        SparseCharacterBody elevatedTarget = request.targetBody;
        elevatedTarget.eyeY += request.targetBody.stepHeight;
        const SparseCollisionVolumeResult elevatedBody =
            world.TestCollisionAabb(MakeSparseCharacterBodyAabb(elevatedTarget));
        result.sampledVoxels += elevatedBody.sampledVoxels;
        result.solidVoxels += elevatedBody.solidVoxels;
        result.liquidVoxels += elevatedBody.liquidVoxels;

        if (!elevatedBody.blocked) {
            const float currentFeetY = request.targetBody.eyeY - request.targetBody.height;
            const SparseCollisionAabb stepFootProbe = MakeSparseCharacterFootprintAabb(
                elevatedTarget,
                currentFeetY + request.targetBody.stepHeight);
            const SparseCollisionSupportResult stepSupport =
                world.FindCollisionSupportBelow(stepFootProbe, request.targetBody.stepHeight + 0.75f);
            result.sampledVoxels += stepSupport.sampledVoxels;
            result.solidVoxels += stepSupport.solidVoxels;
            result.liquidVoxels += stepSupport.liquidVoxels;

            if (stepSupport.found) {
                const float supportFeetY = static_cast<float>(stepSupport.supportY) + 1.0f;
                const bool withinStep =
                    supportFeetY >= currentFeetY - 0.1f &&
                    supportFeetY <= currentFeetY + request.targetBody.stepHeight + 0.35f;
                if (withinStep) {
                    result.eyeX = request.targetBody.eyeX;
                    result.eyeY = supportFeetY + request.targetBody.height;
                    result.eyeZ = request.targetBody.eyeZ;
                    result.steppedUp = true;
                    return result;
                }
            }
        }
    }

    SparseCharacterBody xOnly = request.targetBody;
    xOnly.eyeZ = sweepStart.eyeZ;
    SparseCharacterBody zOnly = request.targetBody;
    zOnly.eyeX = sweepStart.eyeX;
    const SparseCollisionVolumeResult xOnlyVolume =
        world.TestCollisionAabb(MakeSparseCharacterBodyAabb(xOnly));
    const SparseCollisionVolumeResult zOnlyVolume =
        world.TestCollisionAabb(MakeSparseCharacterBodyAabb(zOnly));
    result.sampledVoxels += xOnlyVolume.sampledVoxels + zOnlyVolume.sampledVoxels;
    result.solidVoxels += xOnlyVolume.solidVoxels + zOnlyVolume.solidVoxels;
    result.liquidVoxels += xOnlyVolume.liquidVoxels + zOnlyVolume.liquidVoxels;

    if (!xOnlyVolume.blocked && zOnlyVolume.blocked) {
        result.eyeX = request.targetBody.eyeX;
        result.eyeZ = sweepStart.eyeZ;
    } else if (xOnlyVolume.blocked && !zOnlyVolume.blocked) {
        result.eyeX = sweepStart.eyeX;
        result.eyeZ = request.targetBody.eyeZ;
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
    result.eyeY = request.targetBody.eyeY;
    result.verticalVelocity = request.verticalVelocity;

    const float deltaY = request.targetBody.eyeY - request.startBody.eyeY;
    if (std::abs(deltaY) < 0.0001f) {
        return result;
    }

    const float travelDistance = std::abs(deltaY);
    const uint32_t sweepSteps = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(travelDistance / 0.25f)),
        1,
        static_cast<int>(std::max(1u, request.maxSweepSteps))));

    if (deltaY < 0.0f) {
        const float startFeetY = request.startBody.eyeY - request.startBody.height;
        const float targetFeetY = request.targetBody.eyeY - request.targetBody.height;
        const SparseCollisionAabb supportProbe =
            MakeSparseCharacterFootprintAabb(request.startBody, startFeetY + 0.25f);
        const SparseCollisionSupportResult support =
            world.FindCollisionSupportBelow(supportProbe, (startFeetY - targetFeetY) + 1.0f);
        result.sampledVoxels += support.sampledVoxels;
        result.solidVoxels += support.solidVoxels;
        result.liquidVoxels += support.liquidVoxels;

        if (support.found) {
            const float supportFeetY = static_cast<float>(support.supportY) + 1.0f;
            if (supportFeetY >= targetFeetY - 0.001f && supportFeetY <= startFeetY + 0.25f) {
                result.blocked = true;
                result.landed = true;
                result.eyeY = supportFeetY + request.targetBody.height;
                result.verticalVelocity = 0.0f;
                const float denominator = std::max(0.0001f, startFeetY - targetFeetY);
                result.safeFraction = std::clamp((startFeetY - supportFeetY) / denominator, 0.0f, 1.0f);
                return result;
            }
        }
    }

    const SparseCollisionSweepResult sweep = world.SweepCollisionAabb(
        MakeSparseCharacterBodyAabb(request.startBody),
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
    result.eyeY = request.startBody.eyeY + deltaY * sweep.safeFraction;
    result.verticalVelocity = 0.0f;
    return result;
}

SparseCharacterGroundResult ResolveSparseCharacterGrounding(
    const SparseVoxelWorld& world,
    const SparseCharacterGroundRequest& request) {
    SparseCharacterGroundResult result;
    result.eyeY = request.body.eyeY;
    result.verticalVelocity = request.verticalVelocity;

    const float feetY = request.body.eyeY - request.body.height;
    const float snapUp = std::max(0.0f, request.maxSnapUp);
    const float snapDown = std::max(0.0f, request.maxSnapDown);
    if (snapUp <= 0.0f && snapDown <= 0.0f) {
        return result;
    }

    const SparseCollisionAabb supportProbe =
        MakeSparseCharacterFootprintAabb(request.body, feetY + snapUp);
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
    result.snapped = std::abs(result.eyeY - request.body.eyeY) > 0.001f;
    return result;
}

} // namespace VENPOD::Simulation
