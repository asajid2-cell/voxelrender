#pragma once

#include <cstdint>
#include <limits>

namespace VENPOD::Simulation {

struct SparseRuntimeBudgetInput {
    float lastRawFrameMs = 16.67f;
    float combinedSchedulerPressureMs = 16.67f;
    bool hasQueueBacklog = false;
    bool uploadRingOverflow = false;
    uint64_t stagedBytesLastFrame = 0;
    uint64_t uploadRingBytes = 0;
    uint32_t maxBrickPages = 1;
    uint32_t urgentQueuedBricks = 0;
    uint32_t visibleQueuedBricks = 0;
    uint32_t speculativeQueuedBricks = 0;
    uint32_t surfaceQueuedBricks = 0;
    uint32_t physicsHotCandidateBricks = 0;
    uint32_t pagePublishReadyQueued = 0;
    uint32_t pagePublishWaitingFrame = 0;
    uint32_t pagePublishWaitingFence = 0;
    uint32_t pagePublishEditedQueued = 0;
    uint32_t pagePublishMaxReadyFrameLag = 0;
    bool visibleMissPressure = false;
    uint32_t ownershipPressureLevel = 0;
};

struct SparseReadyPublishPressureInput {
    bool enabled = false;
    uint64_t pending = 0;
    uint32_t oldestAge = 0;
};

struct SparsePrePublishSurfaceBudgetInput {
    bool pageTableSurfaceReadyGateEnabled = false;
    uint32_t pagePublishesEligible = 0;
    bool pagePublishQueueEmpty = true;

    uint32_t baseExtractBudget = 0;
    uint32_t terrainCriticalExtractBudget = 0;
    uint32_t startupExtractBudget = 0;
    uint32_t postOpenExtractBudget = 0;

    uint32_t baseHiddenCriticalBudget = 0;
    uint32_t startupHiddenCriticalBudget = 0;
    uint32_t postOpenHiddenCriticalBudget = 0;
    uint32_t hiddenTrackedBudget = 0;
    uint32_t generalBudget = std::numeric_limits<uint32_t>::max();
    uint32_t editGeneralBudget = std::numeric_limits<uint32_t>::max();

    float baseMaxMs = 0.0f;
    float startupMaxMs = 0.0f;
    float postOpenMaxMs = 0.0f;

    uint64_t frameIndex = 0;
    uint64_t lastEditFrame = 0;
    uint32_t editIdleFrames = 0;
    uint32_t postEditGeneralSpillFrames = 0;
    uint32_t postEditGeneralBudget = std::numeric_limits<uint32_t>::max();
    float postEditGeneralSpillPressureMs = 0.0f;
    float lastRawFrameMs = 0.0f;
    float combinedSchedulerPressureMs = 0.0f;
    float sameFrameClipmapPrepMs = 0.0f;
    float stackedWorkClipmapPrepThresholdMs = 0.0f;
    uint32_t stackedWorkGeneralBudget = std::numeric_limits<uint32_t>::max();

    bool terrainCriticalPublishOvertime = false;
    bool hiddenExactStartupPublishOvertime = false;
    bool hiddenExactRuntimePublishPriority = false;
    bool startupSurfaceCatchup = false;
    bool postOpenSurfaceCatchup = false;
};

struct SparsePrePublishSurfaceBudgetDecision {
    bool enabled = false;
    bool editActive = false;
    bool postEditGeneralSpillActive = false;
    bool splitGeneralByOwnership = false;
    bool skipGeneralSurfaceStage = false;
    bool stackedWorkGeneralCapActive = false;
    uint32_t totalBudget = 0;
    uint32_t hiddenCriticalBudget = 0;
    uint32_t hiddenTrackedBudget = 0;
    uint32_t generalBudget = 0;
    uint32_t generalCriticalBudget = 0;
    uint32_t generalNonCriticalBudget = 0;
    float maxMs = 0.0f;
};

struct SparseSurfaceWorkRouteInput {
    bool routingEnabled = false;
    bool asyncExtractionEnabled = false;
    bool editActive = false;
    // Per-coord edit gate: async extraction rejects only bricks whose local edit
    // dependency overlaps an edit. With it on, edit-window routing is safe (the
    // stroke floods hundreds of ordinary visible bricks whose meshing is not
    // edit-dependent; only blob bricks stay inline).
    bool asyncPerCoordEditGate = false;
    bool previousRouteGeneralToAsync = false;
    uint32_t asyncQueueDepth = 0;
    uint32_t asyncResultDepth = 0;
    uint32_t asyncBacklogLimit = 0;
    uint64_t surfaceReadyPublishPending = 0;
    uint32_t surfaceReadyPublishOldestAge = 0;
    uint64_t surfaceReadyPublishPendingLimit = 0;
    uint32_t surfaceReadyPublishOldestAgeLimit = 0;
    // Main page-table publish queue depth (SparsePagePublishQueue). This is the
    // counter that actually blew up in the Loop 84 Probe A failure (553 vs ~61).
    uint64_t pagePublishBacklog = 0;
    uint32_t pagePublishBacklogLimit = 0;
};

struct SparseSurfaceWorkRouteDecision {
    bool routeGeneralToAsync = false;
    bool asyncBacklogSaturated = false;
    bool publishBacklogSaturated = false;
};

struct SparseRuntimeWorkloadSnapshot {
    float lastRawFrameMs = 16.67f;
    float combinedSchedulerPressureMs = 16.67f;
    bool pagePublishQueueEmpty = true;
    bool missFeedbackPending = false;
    bool uploadRingOverflowLastFrame = false;
    uint32_t uploadRingBudgetDefersLastFrame = 0;
    uint64_t stagedBytesLastFrame = 0;
    uint64_t uploadRingBytes = 0;
    uint32_t maxBrickPages = 1;
    uint64_t clipmapQueuedHeightTiles = 0;
    uint64_t clipmapQueuedVoxelBricks = 0;
    uint64_t generationQueuedBricks = 0;
    uint64_t generationQueuedSpeculativeBricks = 0;
    uint64_t generationQueuedVisibleBricks = 0;
    uint64_t generationQueuedCollisionBricks = 0;
    uint64_t generationQueuedEditedBricks = 0;
    uint64_t uploadQueuedBricks = 0;
    uint64_t uploadQueuedSpeculativeBricks = 0;
    uint64_t uploadQueuedVisibleBricks = 0;
    uint64_t uploadQueuedCollisionBricks = 0;
    uint64_t uploadQueuedEditedBricks = 0;
    uint64_t surfaceQueuedSpeculativeBricks = 0;
    uint64_t surfaceQueuedVisibleBricks = 0;
    uint64_t surfaceQueuedCollisionBricks = 0;
    uint64_t surfaceQueuedEditedBricks = 0;
    uint64_t surfaceExtractionQueuedBricks = 0;
    uint64_t physicsHotCandidateBricks = 0;
    uint64_t pagePublishReadyQueued = 0;
    uint64_t pagePublishWaitingFrame = 0;
    uint64_t pagePublishWaitingFence = 0;
    uint64_t pagePublishEditedQueued = 0;
    uint32_t pagePublishMaxReadyFrameLag = 0;
    bool residencyCatchupActive = false;
    uint32_t ownershipPressureLevel = 0;
    SparseReadyPublishPressureInput readySurfacePublish{};
};

enum class SparseRuntimePressureClass : uint8_t {
    Idle,
    BacklogHeadroom,
    Moderate,
    High,
    Severe
};

struct SparseRuntimeBudgetDecision {
    float scale = 1.0f;
    float protectedScale = 1.0f;
    float backgroundScale = 1.0f;
    SparseRuntimePressureClass pressureClass = SparseRuntimePressureClass::Idle;
    bool hasProtectedBacklog = false;
    bool trimSpeculativeFirst = false;
};

struct SparseFramePressureInput {
    float smoothedFrameMs = 16.67f;
    float predictedFrameMs = 16.67f;
    float gpuFrameMs = 0.0f;
    float gpuRaymarchMs = 0.0f;
    float previousDebtMs = 0.0f;
    float frameBudgetMs = 16.67f;
    float maxDebtMs = 8.0f;
};

struct SparseFramePressure {
    float schedulerPressureMs = 16.67f;
    float gpuPressureMs = 0.0f;
    float combinedPressureMs = 16.67f;
    float debtMs = 0.0f;
    float budgetPressureMs = 16.67f;
};

struct SparseFramePredictionInput {
    float previousPredictedFrameMs = 16.67f;
    float rawFrameMs = 16.67f;
    float gpuFrameMs = 0.0f;
    float chunkUpdateMs = 0.0f;
    float physicsSubmitMs = 0.0f;
    float brushSubmitMs = 0.0f;
    float presentMs = 0.0f;
    float historyWeight = 0.82f;
};

struct SparseFramePrediction {
    float predictedWorkMs = 16.67f;
    float predictedFrameMs = 16.67f;
};

struct SparseOwnershipPressureInput {
    uint32_t frameIndex = 0;
    uint32_t readyFrame = 0;
    uint32_t terrainPercent = 0;
    uint32_t voxelTerrainPercent = 0;
    uint32_t valleyAtmospherePercent = 0;
    uint32_t missPercent = 0;
    uint32_t unsafeNearMissPercent = 0;
    uint32_t minTerrainPercent = 0;
    uint32_t minVoxelTerrainPercent = 0;
    uint32_t maxValleyAtmospherePercent = 0;
    uint32_t maxMissPercent = 0;
    uint32_t maxUnsafeNearMissPercent = 0;
    uint32_t holdFrames = 1;
    uint32_t currentCatchupFrames = 0;
};

struct SparseOwnershipPressure {
    bool triggered = false;
    bool active = false;
    uint32_t level = 0;
    uint32_t updatedCatchupFrames = 0;
    uint32_t terrainDeficitPercent = 0;
    uint32_t voxelTerrainDeficitPercent = 0;
    uint32_t valleyAtmosphereExcessPercent = 0;
    uint32_t missExcessPercent = 0;
    uint32_t unsafeNearMissExcessPercent = 0;
};

struct SparseMissFeedbackPlanInput {
    bool enabled = false;
    uint32_t frameIndex = 0;
    uint32_t baseInterval = 1;
    uint32_t baseRayGrid = 5;
    uint32_t baseDistance = 256;
    uint32_t baseStride = 16;
    uint32_t maxRecords = 256;
    uint32_t unsafeNearMissPercent = 0;
    uint32_t valleyAtmospherePercent = 0;
    uint32_t maxValleyAtmospherePercent = 8;
    bool allowValleyAtmosphereFeedback = true;
    uint32_t ownershipPressureLevel = 0;
    uint32_t pendingRecords = 0;
    uint32_t staleReadbackDrops = 0;
    bool overflowLastRetire = false;
};

struct SparseMissFeedbackPlan {
    bool dispatch = false;
    bool urgent = false;
    uint32_t rayGrid = 5;
    uint32_t distance = 256;
    uint32_t stride = 16;
    uint32_t maxRecords = 256;
};

struct SparseRequestBudgetDecision {
    uint32_t speculative = 0;
    uint32_t visible = 0;
    uint32_t collision = 0;
    uint32_t total = 0;
    uint32_t protectedHardTotal = 0;
};

struct SparseUploadBudgetDecision {
    uint32_t speculative = 0;
    uint32_t visible = 0;
    uint32_t collision = 0;
    uint32_t edited = 0;
    uint32_t total = 0;
    uint32_t protectedTotal = 0;
    uint32_t backgroundTotal = 0;
    bool hasProtectedBacklog = false;
};

struct SparseFrameUploadPlanInput {
    uint64_t uploadBytesCapacity = 0;
    uint64_t uploadBytesAlreadyUsed = 0;
    uint64_t pageTableResetBytes = 0;
    uint64_t pageTableEntryBytes = 256;
    uint64_t brickUploadBytes = 0;
    uint64_t midClipmapSnapshotBytes = 0;
    bool pageTableResetPending = false;
    bool midClipmapDirty = false;
    bool protectedBacklog = false;
    bool publishProtectedBacklog = false;
    uint32_t invalidationQueued = 0;
    uint32_t publishQueued = 0;
    uint32_t invalidationBudget = 0;
    uint32_t publishBudget = 0;
    SparseUploadBudgetDecision brickBudgets{};
};

struct SparseFrameUploadPlan {
    bool allowPageTableReset = false;
    uint32_t invalidationBudget = 0;
    uint32_t publishBudget = 0;
    bool allowMidClipmap = false;
    SparseUploadBudgetDecision brickBudgets{};
    uint64_t reservedBytes = 0;
    uint64_t remainingBytes = 0;
    uint32_t byteLimitedDefers = 0;
};

struct SparsePhysicsBudgetDecision {
    uint32_t brickBudget = 0;
    uint32_t moveBudget = 0;
    bool protectedBacklog = false;
};

struct SparseBackgroundRenderBudgetInput {
    float combinedPressureMs = 16.67f;
    float gpuRaymarchMs = 0.0f;
    float frameBudgetMs = 16.67f;
    float previousRaymarchScale = 1.0f;
    float previousRenderQuality = 1.0f;
    uint32_t ownershipPressureLevel = 0;
    float midHeightCoverage = 0.0f;
    float midVoxelCoverage = 0.0f;
    float midVoxelPixelShare = 0.0f;
    float farSvoPixelShare = 0.0f;
    float farHeightPixelShare = 0.0f;
    float skyPixelShare = 0.0f;
    float backgroundPixelShare = 1.0f;
    bool farSvoReady = false;
};

struct SparseBackgroundRenderBudgetDecision {
    float raymarchScale = 1.0f;
    float farFieldQuality = 1.0f;
    float renderQuality = 1.0f;
    uint32_t qualityTier = 0;
    bool preserveFarFieldQuality = false;
};

struct SparseFarUploadBudgetInput {
    uint64_t fullBudgetBytes = 0;
    uint64_t trickleBudgetBytes = 0;
    uint64_t uploadedBytes = 0;
    uint64_t totalBytes = 0;
    float combinedPressureMs = 16.67f;
    float predictedFrameMs = 16.67f;
    float lastUploadMs = 0.0f;
    float smoothedUploadMs = 0.0f;
    float targetUploadMs = 1.25f;
    bool cheapFrame = false;
    bool canTrickle = false;
    bool visibleMissPressure = false;
    bool readinessDeadline = false;
};

struct SparseFarUploadBudgetDecision {
    uint64_t budgetBytes = 0;
    float uploadScale = 1.0f;
    uint32_t pressureTier = 0;
    bool deferred = false;
};

class SparseRuntimeBudgetScheduler {
public:
    static SparseFramePressure BuildFramePressure(const SparseFramePressureInput& input);
    static SparseFramePrediction BuildFramePrediction(const SparseFramePredictionInput& input);
    static SparseOwnershipPressure BuildOwnershipPressure(const SparseOwnershipPressureInput& input);
    static SparseMissFeedbackPlan BuildMissFeedbackPlan(const SparseMissFeedbackPlanInput& input);
    static SparseRuntimeBudgetInput BuildRuntimeBudgetInput(
        const SparseRuntimeWorkloadSnapshot& snapshot);
    static SparsePrePublishSurfaceBudgetDecision BuildPrePublishSurfaceBudget(
        const SparsePrePublishSurfaceBudgetInput& input);
    static SparseSurfaceWorkRouteDecision BuildSurfaceWorkRoute(
        const SparseSurfaceWorkRouteInput& input);
    static SparseRuntimeBudgetDecision Evaluate(const SparseRuntimeBudgetInput& input);
    static uint32_t ScaleBudget(uint32_t budget, float scale, uint32_t minIfNonZero = 0u);
    static uint32_t BuildProcessingBudget(
        uint32_t baseBudget,
        uint32_t queuedWork,
        bool protectedBacklog,
        const SparseRuntimeBudgetDecision& runtimeDecision,
        uint32_t minIfQueued = 1u,
        uint32_t maxMultiplier = 4u);
    static uint32_t BuildEditedCatchupBudget(
        uint32_t currentBudget,
        uint32_t queuedEdited,
        const SparseRuntimeBudgetDecision& runtimeDecision,
        uint32_t absoluteMaxBudget);
    static SparseRequestBudgetDecision BuildRequestBudgets(
        uint32_t speculativeBudget,
        uint32_t visibleBudget,
        uint32_t collisionBudget,
        uint32_t totalBudget,
        const SparseRuntimeBudgetDecision& runtimeDecision);
    static SparseUploadBudgetDecision BuildUploadBudgets(
        uint32_t totalUploadBudget,
        uint32_t queuedSpeculative,
        uint32_t queuedVisible,
        uint32_t queuedCollision,
        uint32_t queuedEdited,
        const SparseRuntimeBudgetDecision& runtimeDecision);
    static SparseFrameUploadPlan BuildFrameUploadPlan(const SparseFrameUploadPlanInput& input);
    static SparsePhysicsBudgetDecision BuildPhysicsBudgets(
        uint32_t baseBrickBudget,
        uint32_t baseMoveBudget,
        uint32_t queuedBricks,
        uint32_t hotQueuedBricks,
        const SparseRuntimeBudgetDecision& runtimeDecision);
    static SparseBackgroundRenderBudgetDecision BuildBackgroundRenderBudget(
        const SparseBackgroundRenderBudgetInput& input);
    static SparseFarUploadBudgetDecision BuildFarUploadBudget(
        const SparseFarUploadBudgetInput& input);
};

} // namespace VENPOD::Simulation
