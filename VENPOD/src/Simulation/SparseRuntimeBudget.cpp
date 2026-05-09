#include "SparseRuntimeBudget.h"

#include <algorithm>
#include <cmath>

namespace VENPOD::Simulation {

SparseFramePressure SparseRuntimeBudgetScheduler::BuildFramePressure(
    const SparseFramePressureInput& input)
{
    SparseFramePressure pressure;
    const float smoothedFrameMs =
        std::isfinite(input.smoothedFrameMs) ? std::max(0.0f, input.smoothedFrameMs) : 16.67f;
    const float predictedFrameMs =
        std::isfinite(input.predictedFrameMs) ? std::max(0.0f, input.predictedFrameMs) : smoothedFrameMs;
    const float gpuFrameMs =
        std::isfinite(input.gpuFrameMs) ? std::max(0.0f, input.gpuFrameMs) : 0.0f;
    const float gpuRaymarchMs =
        std::isfinite(input.gpuRaymarchMs) ? std::max(0.0f, input.gpuRaymarchMs) : 0.0f;
    const float previousDebtMs =
        std::isfinite(input.previousDebtMs) ? std::max(0.0f, input.previousDebtMs) : 0.0f;
    const float frameBudgetMs =
        std::isfinite(input.frameBudgetMs) ? std::max(1.0f, input.frameBudgetMs) : 16.67f;
    const float maxDebtMs =
        std::isfinite(input.maxDebtMs) ? std::max(0.0f, input.maxDebtMs) : 8.0f;

    pressure.schedulerPressureMs = std::max(smoothedFrameMs, predictedFrameMs);
    pressure.gpuPressureMs = std::max(gpuFrameMs, gpuRaymarchMs);
    pressure.combinedPressureMs =
        std::max(pressure.schedulerPressureMs, pressure.gpuPressureMs);
    const float pressureExcessMs = pressure.combinedPressureMs - frameBudgetMs;
    const float debtDelta = pressureExcessMs > 0.0f
        ? pressureExcessMs * 0.28f
        : pressureExcessMs * 0.10f;
    pressure.debtMs = std::clamp(previousDebtMs + debtDelta, 0.0f, maxDebtMs);
    pressure.budgetPressureMs = pressure.combinedPressureMs + pressure.debtMs;
    return pressure;
}

SparseFramePrediction SparseRuntimeBudgetScheduler::BuildFramePrediction(
    const SparseFramePredictionInput& input)
{
    SparseFramePrediction prediction;
    const float previousPredictedFrameMs =
        std::isfinite(input.previousPredictedFrameMs)
            ? std::max(0.0f, input.previousPredictedFrameMs)
            : 16.67f;
    const float rawFrameMs =
        std::isfinite(input.rawFrameMs) ? std::max(0.0f, input.rawFrameMs) : 16.67f;
    const float gpuFrameMs =
        std::isfinite(input.gpuFrameMs) ? std::max(0.0f, input.gpuFrameMs) : 0.0f;
    const float chunkUpdateMs =
        std::isfinite(input.chunkUpdateMs) ? std::max(0.0f, input.chunkUpdateMs) : 0.0f;
    const float physicsSubmitMs =
        std::isfinite(input.physicsSubmitMs) ? std::max(0.0f, input.physicsSubmitMs) : 0.0f;
    const float brushSubmitMs =
        std::isfinite(input.brushSubmitMs) ? std::max(0.0f, input.brushSubmitMs) : 0.0f;
    const float presentMs =
        std::isfinite(input.presentMs) ? std::max(0.0f, input.presentMs) : 0.0f;
    const float historyWeight =
        std::isfinite(input.historyWeight)
            ? std::clamp(input.historyWeight, 0.0f, 1.0f)
            : 0.82f;

    prediction.predictedWorkMs = std::max(
        rawFrameMs,
        gpuFrameMs + chunkUpdateMs + physicsSubmitMs + brushSubmitMs + presentMs);
    prediction.predictedFrameMs =
        previousPredictedFrameMs * historyWeight +
        prediction.predictedWorkMs * (1.0f - historyWeight);
    return prediction;
}

SparseOwnershipPressure SparseRuntimeBudgetScheduler::BuildOwnershipPressure(
    const SparseOwnershipPressureInput& input)
{
    SparseOwnershipPressure pressure;
    pressure.updatedCatchupFrames = input.currentCatchupFrames;
    if (input.frameIndex < input.readyFrame) {
        pressure.active = pressure.updatedCatchupFrames > 0;
        return pressure;
    }

    pressure.terrainDeficitPercent =
        input.terrainPercent < input.minTerrainPercent
            ? input.minTerrainPercent - input.terrainPercent
            : 0u;
    pressure.missExcessPercent =
        input.missPercent > input.maxMissPercent
            ? input.missPercent - input.maxMissPercent
            : 0u;
    pressure.unsafeNearMissExcessPercent =
        input.unsafeNearMissPercent > input.maxUnsafeNearMissPercent
            ? input.unsafeNearMissPercent - input.maxUnsafeNearMissPercent
            : 0u;

    if (pressure.terrainDeficitPercent == 0u &&
        pressure.missExcessPercent == 0u &&
        pressure.unsafeNearMissExcessPercent == 0u) {
        pressure.active = pressure.updatedCatchupFrames > 0;
        return pressure;
    }

    pressure.triggered = true;
    pressure.level = 1u;
    if (pressure.missExcessPercent >= 10u ||
        pressure.unsafeNearMissExcessPercent >= 4u ||
        pressure.terrainDeficitPercent >= 12u) {
        pressure.level = 2u;
    }
    if (pressure.missExcessPercent >= 24u ||
        pressure.unsafeNearMissExcessPercent >= 10u ||
        pressure.terrainDeficitPercent >= 28u) {
        pressure.level = 3u;
    }

    const uint32_t holdMultiplier = 1u + pressure.level / 2u;
    const uint32_t requestedHold =
        input.holdFrames > UINT32_MAX / holdMultiplier
            ? UINT32_MAX
            : input.holdFrames * holdMultiplier;
    pressure.updatedCatchupFrames =
        std::max(input.currentCatchupFrames, std::max(1u, requestedHold));
    pressure.active = pressure.updatedCatchupFrames > 0;
    return pressure;
}

SparseMissFeedbackPlan SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(
    const SparseMissFeedbackPlanInput& input)
{
    SparseMissFeedbackPlan plan;
    plan.rayGrid = std::clamp(input.baseRayGrid, 1u, 8u);
    plan.distance = std::max(16u, input.baseDistance);
    plan.stride = std::max(4u, input.baseStride);
    plan.maxRecords = std::max(1u, input.maxRecords);

    if (!input.enabled) {
        return plan;
    }

    const uint32_t interval = std::max(1u, input.baseInterval);
    const bool intervalDue = interval <= 1u || (input.frameIndex % interval) == 0u;
    const bool unsafeNearMissObserved = input.unsafeNearMissPercent > 0u;
    const bool feedbackReadbackUnhealthy =
        input.staleReadbackDrops > 0u || input.overflowLastRetire;
    const bool highOwnershipPressure = input.ownershipPressureLevel >= 2u;
    const bool severeOwnershipPressure = input.ownershipPressureLevel >= 3u;
    const bool pendingBacklog =
        input.pendingRecords >= std::max(1u, plan.maxRecords / 2u);

    plan.urgent = unsafeNearMissObserved || feedbackReadbackUnhealthy || highOwnershipPressure;
    plan.dispatch =
        intervalDue ||
        plan.urgent ||
        (input.ownershipPressureLevel > 0u && pendingBacklog);

    if (plan.urgent) {
        plan.rayGrid = std::max(plan.rayGrid, 7u);
        plan.distance = std::max(plan.distance, 512u);
        plan.stride = std::min(plan.stride, 8u);
    }
    if (severeOwnershipPressure || input.unsafeNearMissPercent >= 4u) {
        plan.rayGrid = 8u;
        plan.distance = std::max(plan.distance, 768u);
        plan.stride = 4u;
    }

    return plan;
}

SparseRuntimeBudgetDecision SparseRuntimeBudgetScheduler::Evaluate(
    const SparseRuntimeBudgetInput& input)
{
    const uint32_t pageDivisor = std::max(1u, input.maxBrickPages / 512u);
    const uint64_t stagedByteLimit =
        input.uploadRingBytes == 0 ? 0 : input.uploadRingBytes / pageDivisor;
    const bool uploadPressure =
        input.uploadRingOverflow ||
        (stagedByteLimit > 0 && input.stagedBytesLastFrame > stagedByteLimit);

    SparseRuntimeBudgetDecision decision;
    const bool readyPublishBacklog =
        input.pagePublishReadyQueued > 0 ||
        input.pagePublishEditedQueued > 0 ||
        input.pagePublishMaxReadyFrameLag >= 2u;
    const bool fencePublishBacklog =
        input.pagePublishWaitingFence >= 8u ||
        input.pagePublishWaitingFence > input.pagePublishReadyQueued + input.pagePublishWaitingFrame;
    if (input.lastRawFrameMs > 30.0f ||
        input.combinedSchedulerPressureMs > 21.0f ||
        uploadPressure) {
        decision.scale = 0.35f;
        decision.pressureClass = SparseRuntimePressureClass::Severe;
    } else if (input.combinedSchedulerPressureMs > 19.0f) {
        decision.scale = 0.55f;
        decision.pressureClass = SparseRuntimePressureClass::High;
    } else if (input.combinedSchedulerPressureMs > 17.0f) {
        decision.scale = 0.75f;
        decision.pressureClass = SparseRuntimePressureClass::Moderate;
    } else if (input.hasQueueBacklog && input.combinedSchedulerPressureMs < 14.5f) {
        decision.scale = 1.35f;
        decision.pressureClass = SparseRuntimePressureClass::BacklogHeadroom;
    } else {
        decision.scale = 1.0f;
        decision.pressureClass = SparseRuntimePressureClass::Idle;
    }
    decision.backgroundScale = decision.scale;
    decision.protectedScale = decision.scale;
    decision.hasProtectedBacklog =
        input.visibleMissPressure ||
        input.ownershipPressureLevel > 0 ||
        input.urgentQueuedBricks > 0 ||
        input.visibleQueuedBricks > 0 ||
        readyPublishBacklog ||
        input.physicsHotCandidateBricks > 0;
    decision.trimSpeculativeFirst =
        decision.pressureClass == SparseRuntimePressureClass::High ||
        decision.pressureClass == SparseRuntimePressureClass::Severe ||
        fencePublishBacklog ||
        input.speculativeQueuedBricks > input.urgentQueuedBricks + input.visibleQueuedBricks;

    if (decision.hasProtectedBacklog) {
        switch (decision.pressureClass) {
            case SparseRuntimePressureClass::Severe:
                decision.protectedScale = std::max(decision.protectedScale, 1.00f);
                decision.backgroundScale = std::min(decision.backgroundScale, 0.30f);
                break;
            case SparseRuntimePressureClass::High:
                decision.protectedScale = std::max(decision.protectedScale, 1.00f);
                decision.backgroundScale = std::min(decision.backgroundScale, 0.45f);
                break;
            case SparseRuntimePressureClass::Moderate:
                decision.protectedScale = std::max(decision.protectedScale, 1.15f);
                decision.backgroundScale = std::min(decision.backgroundScale, 0.70f);
                break;
            case SparseRuntimePressureClass::BacklogHeadroom:
                decision.protectedScale = std::max(decision.protectedScale, 1.50f);
                break;
            case SparseRuntimePressureClass::Idle:
            default:
                decision.protectedScale = std::max(decision.protectedScale, 1.15f);
                break;
        }
    }
    if (input.visibleMissPressure) {
        // Ownership misses are not ordinary backlog: they mean the renderer is
        // showing sky where the near sparse field should already own the view.
        // Keep collision/visible residency lanes moving even if frame pressure
        // is high, and make speculative/background work absorb the degradation.
        const uint32_t level = std::clamp(input.ownershipPressureLevel, 1u, 3u);
        const float protectedBoost = 1.15f + 0.20f * static_cast<float>(level);
        const float backgroundCap = level >= 3u ? 0.15f : (level == 2u ? 0.20f : 0.25f);
        decision.protectedScale = std::max(decision.protectedScale, protectedBoost);
        decision.backgroundScale = std::min(decision.backgroundScale, backgroundCap);
        decision.trimSpeculativeFirst = true;
    }
    if (readyPublishBacklog) {
        // A ready page-table publish is the cheapest way to convert already
        // uploaded brick payload into visible terrain. Treat it as protected
        // continuity work, especially when entries have been ready for multiple
        // frames.
        float publishBoost = 1.20f;
        if (input.pagePublishEditedQueued > 0) {
            publishBoost = std::max(publishBoost, 1.35f);
        }
        if (input.pagePublishMaxReadyFrameLag >= 4u) {
            publishBoost = std::max(publishBoost, 1.55f);
        } else if (input.pagePublishMaxReadyFrameLag >= 2u) {
            publishBoost = std::max(publishBoost, 1.35f);
        }
        decision.protectedScale = std::max(decision.protectedScale, publishBoost);
        if (decision.pressureClass == SparseRuntimePressureClass::High ||
            decision.pressureClass == SparseRuntimePressureClass::Severe) {
            decision.backgroundScale = std::min(decision.backgroundScale, 0.45f);
            decision.trimSpeculativeFirst = true;
        }
    }
    if (fencePublishBacklog) {
        // Fence-waiting publishes mean the direct queue has a backlog of brick
        // copies that cannot be made visible yet. Do not keep feeding
        // speculative uploads ahead of that visibility contract.
        decision.backgroundScale = std::min(decision.backgroundScale, 0.70f);
        decision.trimSpeculativeFirst = true;
    }
    return decision;
}

uint32_t SparseRuntimeBudgetScheduler::ScaleBudget(
    uint32_t budget,
    float scale,
    uint32_t minIfNonZero)
{
    if (budget == 0) {
        return 0;
    }
    const float safeScale = std::isfinite(scale) ? std::max(0.0f, scale) : 1.0f;
    const uint32_t scaled =
        static_cast<uint32_t>(std::floor(static_cast<float>(budget) * safeScale + 0.5f));
    return std::max(minIfNonZero, scaled);
}

uint32_t SparseRuntimeBudgetScheduler::BuildProcessingBudget(
    uint32_t baseBudget,
    uint32_t queuedWork,
    bool protectedBacklog,
    const SparseRuntimeBudgetDecision& runtimeDecision,
    uint32_t minIfQueued,
    uint32_t maxMultiplier)
{
    if (baseBudget == 0) {
        return 0;
    }

    const float laneScale = protectedBacklog
        ? runtimeDecision.protectedScale
        : runtimeDecision.backgroundScale;
    uint32_t budget = ScaleBudget(
        baseBudget,
        laneScale,
        queuedWork > 0 ? minIfQueued : 0u);
    if (queuedWork == 0 || maxMultiplier <= 1u) {
        return budget;
    }

    const uint32_t maxBudget = std::max(baseBudget, baseBudget * maxMultiplier);
    if (budget >= maxBudget) {
        return budget;
    }

    const bool hasHeadroom =
        runtimeDecision.pressureClass == SparseRuntimePressureClass::BacklogHeadroom;
    const bool idleWithLargeBacklog =
        runtimeDecision.pressureClass == SparseRuntimePressureClass::Idle &&
        queuedWork > baseBudget * 16u;
    if (!hasHeadroom && !idleWithLargeBacklog) {
        return budget;
    }

    const uint32_t backlogDivisor = hasHeadroom ? 64u : 128u;
    const uint32_t catchup = std::min(
        maxBudget - budget,
        queuedWork / backlogDivisor);
    return std::min(maxBudget, budget + catchup);
}

uint32_t SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
    uint32_t currentBudget,
    uint32_t queuedEdited,
    const SparseRuntimeBudgetDecision& runtimeDecision,
    uint32_t absoluteMaxBudget)
{
    if (currentBudget == 0 || queuedEdited == 0) {
        return currentBudget;
    }

    uint32_t multiplier = 4u;
    switch (runtimeDecision.pressureClass) {
        case SparseRuntimePressureClass::Severe:
            multiplier = 2u;
            break;
        case SparseRuntimePressureClass::High:
            multiplier = 3u;
            break;
        case SparseRuntimePressureClass::Moderate:
            multiplier = 4u;
            break;
        case SparseRuntimePressureClass::BacklogHeadroom:
            multiplier = 8u;
            break;
        case SparseRuntimePressureClass::Idle:
        default:
            multiplier = 6u;
            break;
    }

    const uint32_t maxByMultiplier =
        currentBudget > UINT32_MAX / multiplier ? UINT32_MAX : currentBudget * multiplier;
    const uint32_t cappedMax = std::max(
        currentBudget,
        std::min(absoluteMaxBudget == 0 ? maxByMultiplier : absoluteMaxBudget, maxByMultiplier));
    const uint32_t requested = std::min(queuedEdited, cappedMax);
    return std::max(currentBudget, requested);
}

SparseRequestBudgetDecision SparseRuntimeBudgetScheduler::BuildRequestBudgets(
    uint32_t speculativeBudget,
    uint32_t visibleBudget,
    uint32_t collisionBudget,
    uint32_t totalBudget,
    const SparseRuntimeBudgetDecision& runtimeDecision)
{
    SparseRequestBudgetDecision decision;
    decision.speculative = ScaleBudget(speculativeBudget, runtimeDecision.backgroundScale, 1u);
    decision.visible = ScaleBudget(visibleBudget, runtimeDecision.protectedScale, 1u);
    decision.collision = ScaleBudget(collisionBudget, runtimeDecision.protectedScale, 1u);
    const uint32_t baseTotal = ScaleBudget(totalBudget, runtimeDecision.scale, 1u);
    decision.total = std::max(
        baseTotal,
        std::max(1u, decision.speculative + decision.visible));
    decision.protectedHardTotal = decision.total + decision.collision;
    return decision;
}

SparseUploadBudgetDecision SparseRuntimeBudgetScheduler::BuildUploadBudgets(
    uint32_t totalUploadBudget,
    uint32_t queuedSpeculative,
    uint32_t queuedVisible,
    uint32_t queuedCollision,
    uint32_t queuedEdited,
    const SparseRuntimeBudgetDecision& runtimeDecision)
{
    SparseUploadBudgetDecision decision;
    decision.total = totalUploadBudget;
    if (totalUploadBudget == 0) {
        return decision;
    }

    uint32_t remaining = totalUploadBudget;
    const auto reserve = [&remaining](uint32_t queued, uint32_t desired) {
        const uint32_t amount = std::min(std::min(queued, desired), remaining);
        remaining -= amount;
        return amount;
    };

    decision.hasProtectedBacklog = queuedEdited > 0 || queuedCollision > 0;
    const uint32_t protectedDesired = ScaleBudget(
        std::max(queuedEdited + queuedCollision, 1u),
        runtimeDecision.protectedScale,
        decision.hasProtectedBacklog ? 1u : 0u);
    const uint32_t editedDesired = queuedEdited;
    decision.edited = reserve(queuedEdited, editedDesired);

    const uint32_t collisionDesired = protectedDesired > decision.edited
        ? protectedDesired - decision.edited
        : queuedCollision;
    decision.collision = reserve(queuedCollision, std::max(collisionDesired, queuedCollision));
    decision.protectedTotal = decision.edited + decision.collision;

    const bool allowBackground =
        !decision.hasProtectedBacklog ||
        remaining > 0 ||
        runtimeDecision.pressureClass == SparseRuntimePressureClass::Idle ||
        runtimeDecision.pressureClass == SparseRuntimePressureClass::BacklogHeadroom;
    if (allowBackground && remaining > 0) {
        const uint32_t visibleDesired = ScaleBudget(queuedVisible, runtimeDecision.scale, 0u);
        decision.visible = reserve(queuedVisible, std::max(visibleDesired, queuedVisible));
    }
    if (allowBackground && remaining > 0) {
        const bool onlySpeculativeBacklog =
            queuedSpeculative > 0 &&
            queuedVisible == 0 &&
            queuedCollision == 0 &&
            queuedEdited == 0;
        if (!runtimeDecision.trimSpeculativeFirst || onlySpeculativeBacklog) {
            uint32_t speculativeDesired =
                ScaleBudget(queuedSpeculative, runtimeDecision.backgroundScale, 0u);
            const bool hardPressure =
                runtimeDecision.pressureClass == SparseRuntimePressureClass::High ||
                runtimeDecision.pressureClass == SparseRuntimePressureClass::Severe;
            if (runtimeDecision.trimSpeculativeFirst && hardPressure) {
                const uint32_t trickleBudget = std::max(1u, totalUploadBudget / 4u);
                speculativeDesired = std::min(speculativeDesired, trickleBudget);
            }
            decision.speculative = reserve(
                queuedSpeculative,
                std::max(speculativeDesired, onlySpeculativeBacklog ? 1u : 0u));
        }
    }
    decision.backgroundTotal = decision.visible + decision.speculative;
    return decision;
}

SparseFrameUploadPlan SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(
    const SparseFrameUploadPlanInput& input)
{
    SparseFrameUploadPlan plan;
    plan.brickBudgets = input.brickBudgets;
    if (input.uploadBytesCapacity <= input.uploadBytesAlreadyUsed) {
        plan.byteLimitedDefers =
            (input.pageTableResetPending ? 1u : 0u) +
            input.invalidationQueued +
            input.publishQueued +
            (input.midClipmapDirty ? 1u : 0u) +
            plan.brickBudgets.total;
        return plan;
    }

    uint64_t remaining = input.uploadBytesCapacity - input.uploadBytesAlreadyUsed;
    const auto reserveBytes = [&remaining, &plan](uint64_t bytes) {
        if (bytes == 0 || bytes > remaining) {
            ++plan.byteLimitedDefers;
            return false;
        }
        remaining -= bytes;
        plan.reservedBytes += bytes;
        plan.remainingBytes = remaining;
        return true;
    };

    if (input.pageTableResetPending) {
        plan.allowPageTableReset = reserveBytes(input.pageTableResetBytes);
    }

    const uint32_t invalidationTarget =
        std::min(input.invalidationQueued, input.invalidationBudget);
    for (uint32_t i = 0; i < invalidationTarget; ++i) {
        if (!reserveBytes(input.pageTableEntryBytes)) {
            break;
        }
        ++plan.invalidationBudget;
    }

    const auto reservePublishes = [&]() {
        const uint32_t publishTarget = std::min(input.publishQueued, input.publishBudget);
        for (uint32_t i = plan.publishBudget; i < publishTarget; ++i) {
            if (!reserveBytes(input.pageTableEntryBytes)) {
                break;
            }
            ++plan.publishBudget;
        }
    };

    if (input.publishProtectedBacklog) {
        reservePublishes();
    }

    const auto reserveBrickClass = [&](uint32_t& classBudget) {
        uint32_t reserved = 0;
        for (uint32_t i = 0; i < classBudget; ++i) {
            if (!reserveBytes(input.brickUploadBytes)) {
                break;
            }
            ++reserved;
        }
        classBudget = reserved;
    };

    reserveBrickClass(plan.brickBudgets.edited);

    // Visible brick payloads are part of the protected visual contract when
    // the near field is behind. Page-table publishes can catch up on the next
    // frame, but a skipped visible payload keeps terrain absent longer.
    if (input.protectedBacklog) {
        uint32_t minimumCollision = std::min(plan.brickBudgets.collision, 1u);
        reserveBrickClass(minimumCollision);
        plan.brickBudgets.collision = minimumCollision;
        reserveBrickClass(plan.brickBudgets.visible);
        uint32_t remainingCollision =
            input.brickBudgets.collision > plan.brickBudgets.collision
                ? input.brickBudgets.collision - plan.brickBudgets.collision
                : 0u;
        reserveBrickClass(remainingCollision);
        plan.brickBudgets.collision += remainingCollision;
    } else {
        reserveBrickClass(plan.brickBudgets.collision);
    }
    plan.brickBudgets.protectedTotal =
        plan.brickBudgets.edited + plan.brickBudgets.collision;

    if (!input.publishProtectedBacklog) {
        reservePublishes();
    }

    // Mid clipmaps are not speculative decoration once sparse surface
    // authoritative rendering is active: they are the continuity bridge between
    // resident near-field bricks and far SVO/procedural terrain. Protected brick
    // uploads still reserve first, but a small protected/edit backlog must not
    // starve the mid layer forever.
    if (input.midClipmapDirty) {
        plan.allowMidClipmap = reserveBytes(input.midClipmapSnapshotBytes);
    }

    if (!input.protectedBacklog) {
        reserveBrickClass(plan.brickBudgets.visible);
    }
    if (!input.protectedBacklog) {
        reserveBrickClass(plan.brickBudgets.speculative);
    } else {
        plan.brickBudgets.speculative = 0;
    }
    plan.brickBudgets.backgroundTotal =
        plan.brickBudgets.visible + plan.brickBudgets.speculative;
    plan.brickBudgets.total =
        plan.brickBudgets.protectedTotal + plan.brickBudgets.backgroundTotal;

    plan.remainingBytes = remaining;
    return plan;
}

SparsePhysicsBudgetDecision SparseRuntimeBudgetScheduler::BuildPhysicsBudgets(
    uint32_t baseBrickBudget,
    uint32_t baseMoveBudget,
    uint32_t queuedBricks,
    uint32_t hotQueuedBricks,
    const SparseRuntimeBudgetDecision& runtimeDecision)
{
    SparsePhysicsBudgetDecision decision;
    decision.protectedBacklog = hotQueuedBricks > 0;
    decision.brickBudget = BuildProcessingBudget(
        baseBrickBudget,
        queuedBricks,
        decision.protectedBacklog,
        runtimeDecision,
        queuedBricks > 0 ? 1u : 0u,
        4u);
    const float moveScale = decision.protectedBacklog
        ? runtimeDecision.protectedScale
        : runtimeDecision.backgroundScale;
    decision.moveBudget = ScaleBudget(
        baseMoveBudget,
        moveScale,
        queuedBricks > 0 ? 16u : 0u);

    if (queuedBricks > baseBrickBudget &&
        (runtimeDecision.pressureClass == SparseRuntimePressureClass::Idle ||
         runtimeDecision.pressureClass == SparseRuntimePressureClass::BacklogHeadroom)) {
        const uint32_t maxMoveBudget = std::max(baseMoveBudget, baseMoveBudget * 4u);
        const uint32_t catchup =
            std::min(maxMoveBudget - std::min(decision.moveBudget, maxMoveBudget),
                     (queuedBricks / std::max(1u, baseBrickBudget)) * 32u);
        decision.moveBudget = std::min(maxMoveBudget, decision.moveBudget + catchup);
    }
    return decision;
}

SparseBackgroundRenderBudgetDecision SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(
    const SparseBackgroundRenderBudgetInput& input)
{
    SparseBackgroundRenderBudgetDecision decision;
    const float combinedPressureMs =
        std::isfinite(input.combinedPressureMs) ? std::max(0.0f, input.combinedPressureMs) : 16.67f;
    const float gpuRaymarchMs =
        std::isfinite(input.gpuRaymarchMs) ? std::max(0.0f, input.gpuRaymarchMs) : 0.0f;
    const float frameBudgetMs =
        std::isfinite(input.frameBudgetMs) ? std::max(1.0f, input.frameBudgetMs) : 16.67f;
    const float previousRaymarchScale =
        std::isfinite(input.previousRaymarchScale)
            ? std::clamp(input.previousRaymarchScale, 0.20f, 1.35f)
            : 1.0f;
    const float previousRenderQuality =
        std::isfinite(input.previousRenderQuality)
            ? std::clamp(input.previousRenderQuality, 0.25f, 1.0f)
            : 1.0f;
    const float midCoverage = std::max(
        std::isfinite(input.midHeightCoverage) ? std::clamp(input.midHeightCoverage, 0.0f, 1.0f) : 0.0f,
        std::isfinite(input.midVoxelCoverage) ? std::clamp(input.midVoxelCoverage, 0.0f, 1.0f) : 0.0f);
    const float midVoxelPixelShare =
        std::isfinite(input.midVoxelPixelShare) ? std::clamp(input.midVoxelPixelShare, 0.0f, 1.0f) : 0.0f;
    const float farHeightPixelShare =
        std::isfinite(input.farHeightPixelShare) ? std::clamp(input.farHeightPixelShare, 0.0f, 1.0f) : 0.0f;
    const float skyPixelShare =
        std::isfinite(input.skyPixelShare) ? std::clamp(input.skyPixelShare, 0.0f, 1.0f) : 0.0f;
    const float backgroundPixelShare =
        std::isfinite(input.backgroundPixelShare)
            ? std::clamp(input.backgroundPixelShare, 0.0f, 1.0f)
            : 1.0f;
    const float expensiveBackgroundPixelShare =
        std::clamp(midVoxelPixelShare + farHeightPixelShare, 0.0f, 1.0f);
    const float expensiveScreenPixelShare =
        std::clamp(expensiveBackgroundPixelShare * backgroundPixelShare, 0.0f, 1.0f);

    const bool gpuTimingAvailable = gpuRaymarchMs > 0.001f;
    const bool gpuHardPressure = gpuRaymarchMs > frameBudgetMs * 0.92f;
    const bool gpuModeratePressure = gpuRaymarchMs > frameBudgetMs * 0.70f;
    const bool gpuSoftPressure = gpuRaymarchMs > frameBudgetMs * 0.55f;
    const bool ownershipDrivenBackgroundCost =
        gpuTimingAvailable &&
        expensiveScreenPixelShare > 0.34f &&
        expensiveBackgroundPixelShare > 0.52f &&
        skyPixelShare < 0.72f &&
        gpuRaymarchMs > frameBudgetMs * 0.34f;
    const bool farHeightDominantCost =
        gpuTimingAvailable &&
        backgroundPixelShare > 0.42f &&
        farHeightPixelShare > 0.42f &&
        gpuRaymarchMs > frameBudgetMs * 0.30f;
    const bool midVoxelDominantCost =
        gpuTimingAvailable &&
        backgroundPixelShare > 0.38f &&
        midVoxelPixelShare > 0.24f &&
        gpuRaymarchMs > frameBudgetMs * 0.32f;
    const bool schedulerHardPressure =
        combinedPressureMs > frameBudgetMs + 4.0f &&
        (!gpuTimingAvailable || gpuRaymarchMs > frameBudgetMs * 0.45f);
    const bool schedulerModeratePressure =
        combinedPressureMs > frameBudgetMs + 1.0f &&
        (!gpuTimingAvailable || gpuRaymarchMs > frameBudgetMs * 0.38f);
    const bool residencyCatchup = input.ownershipPressureLevel > 0;

    float targetRayScale = 1.0f;
    float targetRenderQuality = 1.0f;
    float targetFarQuality = 1.0f;
    uint32_t tier = 0;

    if (gpuHardPressure || schedulerHardPressure) {
        targetRayScale = 0.52f;
        targetRenderQuality = 0.58f;
        targetFarQuality = 0.50f;
        tier = 3;
    } else if (gpuModeratePressure || schedulerModeratePressure) {
        targetRayScale = 0.72f;
        targetRenderQuality = 0.74f;
        targetFarQuality = 0.68f;
        tier = 2;
    } else if ((!gpuTimingAvailable && combinedPressureMs > frameBudgetMs * 0.92f) || gpuSoftPressure) {
        targetRayScale = 0.88f;
        targetRenderQuality = 0.88f;
        targetFarQuality = 0.84f;
        tier = 1;
    }

    if (ownershipDrivenBackgroundCost) {
        // Ownership mix tells us which layer is making the fullscreen pass
        // expensive before the total frame time fully collapses. Prefer a
        // shallow quality downshift over shrinking the world or starving near
        // residency.
        targetRayScale = std::min(targetRayScale, 0.88f);
        targetRenderQuality = std::min(targetRenderQuality, 0.88f);
        targetFarQuality = std::min(targetFarQuality, 0.84f);
        tier = std::max<uint32_t>(tier, 1u);
    }
    if (farHeightDominantCost) {
        targetRenderQuality = std::min(targetRenderQuality, 0.84f);
        targetFarQuality = std::min(targetFarQuality, 0.78f);
        tier = std::max<uint32_t>(tier, 1u);
    }
    if (midVoxelDominantCost) {
        targetRenderQuality = std::min(targetRenderQuality, 0.86f);
        targetFarQuality = std::min(targetFarQuality, 0.82f);
        tier = std::max<uint32_t>(tier, 1u);
    }

    if (midCoverage < 0.35f && !input.farSvoReady) {
        // When continuity layers are still warming up, avoid dropping the
        // background into a low-quality sky/miss state. Request queues should
        // absorb the pressure before the visual ownership contract collapses.
        targetRenderQuality = std::max(targetRenderQuality, 0.74f);
        targetFarQuality = std::max(targetFarQuality, 0.68f);
    }
    if (residencyCatchup) {
        const float catchupFloor = 0.70f + 0.06f * static_cast<float>(
            std::clamp(input.ownershipPressureLevel, 1u, 3u) - 1u);
        targetRayScale = std::max(targetRayScale, catchupFloor);
        targetRenderQuality = std::max(targetRenderQuality, catchupFloor);
        targetFarQuality = std::max(targetFarQuality, 0.62f);
    }

    const auto smoothToward = [](float previous, float target, float downStep, float upStep) {
        const float step = target < previous ? downStep : upStep;
        return std::clamp(previous + (target - previous) * step, 0.20f, 1.35f);
    };

    decision.raymarchScale = smoothToward(previousRaymarchScale, targetRayScale, 0.30f, 0.045f);
    decision.renderQuality = std::clamp(
        smoothToward(previousRenderQuality, targetRenderQuality, 0.26f, 0.055f),
        0.25f,
        1.0f);
    decision.farFieldQuality = std::clamp(targetFarQuality, 0.25f, 1.0f);
    decision.qualityTier = tier;
    return decision;
}

SparseFarUploadBudgetDecision SparseRuntimeBudgetScheduler::BuildFarUploadBudget(
    const SparseFarUploadBudgetInput& input)
{
    SparseFarUploadBudgetDecision decision;
    const uint64_t remainingBytes =
        input.totalBytes > input.uploadedBytes ? input.totalBytes - input.uploadedBytes : 0ull;
    if (remainingBytes == 0) {
        decision.deferred = true;
        return decision;
    }

    const uint64_t fullBudget = input.fullBudgetBytes;
    const uint64_t trickleBudget = input.trickleBudgetBytes;
    if (fullBudget == 0 && trickleBudget == 0) {
        decision.deferred = true;
        return decision;
    }

    const float combinedPressureMs =
        std::isfinite(input.combinedPressureMs) ? std::max(0.0f, input.combinedPressureMs) : 16.67f;
    const float predictedFrameMs =
        std::isfinite(input.predictedFrameMs) ? std::max(0.0f, input.predictedFrameMs) : combinedPressureMs;
    const float lastUploadMs =
        std::isfinite(input.lastUploadMs) ? std::max(0.0f, input.lastUploadMs) : 0.0f;
    const float smoothedUploadMs =
        std::isfinite(input.smoothedUploadMs) ? std::max(0.0f, input.smoothedUploadMs) : lastUploadMs;
    const float targetUploadMs =
        std::isfinite(input.targetUploadMs) ? std::clamp(input.targetUploadMs, 0.25f, 4.0f) : 1.25f;

    const float uploadPressure = std::max(lastUploadMs, smoothedUploadMs);
    const bool uploadHardPressure = uploadPressure > targetUploadMs * 3.2f;
    const bool uploadModeratePressure = uploadPressure > targetUploadMs * 1.7f;
    const bool frameHardPressure =
        combinedPressureMs > 21.0f ||
        predictedFrameMs > 21.0f;
    const bool frameModeratePressure =
        combinedPressureMs > 18.5f ||
        predictedFrameMs > 18.5f;

    uint64_t desiredBudget = 0;
    if (input.cheapFrame && !frameModeratePressure && !uploadModeratePressure) {
        desiredBudget = fullBudget;
        decision.uploadScale = 1.0f;
        decision.pressureTier = 0u;
    } else if (input.canTrickle) {
        desiredBudget = trickleBudget;
        decision.uploadScale = 1.0f;
        decision.pressureTier = 1u;
        if (uploadHardPressure) {
            desiredBudget = std::max<uint64_t>(64ull * 1024ull, trickleBudget / 4ull);
            decision.uploadScale = 0.25f;
            decision.pressureTier = 3u;
        } else if (uploadModeratePressure) {
            desiredBudget = std::max<uint64_t>(128ull * 1024ull, trickleBudget / 2ull);
            decision.uploadScale = 0.50f;
            decision.pressureTier = 2u;
        } else if (frameHardPressure || frameModeratePressure) {
            desiredBudget = trickleBudget;
            decision.uploadScale = frameHardPressure ? 0.70f : 1.0f;
            decision.pressureTier = frameHardPressure ? 2u : 1u;
        } else if (combinedPressureMs < 14.5f && predictedFrameMs < 15.5f) {
            desiredBudget = std::min(fullBudget, std::max(trickleBudget, trickleBudget * 2ull));
            decision.uploadScale = 1.35f;
            decision.pressureTier = 0u;
        }
    } else {
        decision.deferred = true;
        decision.pressureTier = frameHardPressure || uploadHardPressure ? 3u : 2u;
        return decision;
    }

    if (input.visibleMissPressure && desiredBudget > trickleBudget) {
        // Visible near-field catch-up owns the frame. Keep far continuity
        // moving, but never let it compete with missing close terrain.
        desiredBudget = trickleBudget;
        decision.uploadScale = std::min(decision.uploadScale, 1.0f);
        decision.pressureTier = std::max(decision.pressureTier, 1u);
    }

    decision.budgetBytes = std::min(remainingBytes, desiredBudget);
    decision.deferred = decision.budgetBytes == 0;
    return decision;
}

} // namespace VENPOD::Simulation
