#include "SparseBrickRequestPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace VENPOD::Simulation {

namespace {

constexpr uint32_t kMaxPlannerRequests = 4096;
constexpr uint32_t kMaxPlannerRadiusXz = 64;
constexpr uint32_t kMaxPlannerRadiusY = 32;
constexpr uint32_t kMaxForwardPrefetchBricks = 256;
constexpr float kMaxViewConeDistance = 8192.0f;
constexpr float kMaxViewConeStepDistance = 512.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

float Length(Vec3 v) {
    return static_cast<float>(std::sqrt(
        static_cast<double>(v.x) * static_cast<double>(v.x) +
        static_cast<double>(v.y) * static_cast<double>(v.y) +
        static_cast<double>(v.z) * static_cast<double>(v.z)));
}

Vec3 NormalizeOr(Vec3 v, Vec3 fallback) {
    const float length = Length(v);
    if (!std::isfinite(length) || length <= 0.00001f) {
        return fallback;
    }
    return {v.x / length, v.y / length, v.z / length};
}

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

float ClampFinite(float value, float minValue, float maxValue, float fallback) {
    return std::clamp(FiniteOr(value, fallback), minValue, maxValue);
}

float BrickCenterWorldFallback(int32_t brickCoord) {
    return static_cast<float>(
        static_cast<double>(brickCoord) * static_cast<double>(SPARSE_BRICK_SIZE) +
        static_cast<double>(SPARSE_BRICK_SIZE) * 0.5);
}

bool TryFloorToInt32(float value, int32_t& out) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(static_cast<double>(value));
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(floored);
    return true;
}

bool TryStepBrickCoord(int32_t value, int32_t step, int32_t& out) {
    const int64_t stepped = static_cast<int64_t>(value) + static_cast<int64_t>(step);
    if (stepped < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        stepped > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(stepped);
    return true;
}

bool TryOffsetBrickCoord(const BrickCoord& center, int32_t dx, int32_t dy, int32_t dz, BrickCoord& out) {
    return TryStepBrickCoord(center.x, dx, out.x) &&
        TryStepBrickCoord(center.y, dy, out.y) &&
        TryStepBrickCoord(center.z, dz, out.z);
}

int32_t ClampPriority(int64_t value) {
    return static_cast<int32_t>(std::clamp<int64_t>(
        value,
        static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

int64_t AbsDiffInt64(int32_t a, int32_t b) {
    const int64_t diff = static_cast<int64_t>(a) - static_cast<int64_t>(b);
    return diff < 0 ? -diff : diff;
}

Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Scale(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

struct BrickRayDdaAxis {
    int32_t step = 0;
    float nextT = std::numeric_limits<float>::infinity();
    float deltaT = std::numeric_limits<float>::infinity();
};

BrickRayDdaAxis BuildBrickRayDdaAxis(float origin, float direction, int32_t brickCoord) {
    BrickRayDdaAxis axis;
    if (std::abs(direction) <= 0.00001f) {
        return axis;
    }

    axis.step = direction > 0.0f ? 1 : -1;
    const double brickSize = static_cast<double>(SPARSE_BRICK_SIZE);
    const double boundary = axis.step > 0
        ? (static_cast<double>(brickCoord) + 1.0) * brickSize
        : static_cast<double>(brickCoord) * brickSize;
    axis.nextT = static_cast<float>(
        (boundary - static_cast<double>(origin)) / static_cast<double>(direction));
    if (!std::isfinite(axis.nextT) || axis.nextT < 0.0f) {
        axis.nextT = 0.0f;
    }
    axis.deltaT = static_cast<float>(brickSize / std::abs(static_cast<double>(direction)));
    return axis;
}

std::vector<BrickCoord> BuildBrickLineDda(Vec3 start, Vec3 end, uint32_t maxCenters) {
    std::vector<BrickCoord> centers;
    if (maxCenters == 0 ||
        !std::isfinite(start.x) ||
        !std::isfinite(start.y) ||
        !std::isfinite(start.z) ||
        !std::isfinite(end.x) ||
        !std::isfinite(end.y) ||
        !std::isfinite(end.z)) {
        return centers;
    }

    const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = Length(delta);
    const Vec3 dir = NormalizeOr(delta, {0.0f, 0.0f, 1.0f});
    int32_t startVoxelX = 0;
    int32_t startVoxelY = 0;
    int32_t startVoxelZ = 0;
    if (!TryFloorToInt32(start.x, startVoxelX) ||
        !TryFloorToInt32(start.y, startVoxelY) ||
        !TryFloorToInt32(start.z, startVoxelZ)) {
        return centers;
    }
    BrickCoord coord = BrickCoord::FromWorldVoxel(startVoxelX, startVoxelY, startVoxelZ);
    centers.push_back(coord);
    if (!std::isfinite(length) || length <= 0.0001f || maxCenters == 1) {
        return centers;
    }

    BrickRayDdaAxis axisX = BuildBrickRayDdaAxis(start.x, dir.x, coord.x);
    BrickRayDdaAxis axisY = BuildBrickRayDdaAxis(start.y, dir.y, coord.y);
    BrickRayDdaAxis axisZ = BuildBrickRayDdaAxis(start.z, dir.z, coord.z);
    float distance = 0.0f;
    const uint32_t maxDdaSteps = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(length / static_cast<float>(SPARSE_BRICK_SIZE))) * 4u + 8u,
        1u,
        std::max<uint32_t>(maxCenters * 4u, 8u));
    for (uint32_t stepIndex = 0;
         distance <= length && stepIndex < maxDdaSteps && centers.size() < maxCenters;
         ++stepIndex) {
        const float nextDistance = std::min(axisX.nextT, std::min(axisY.nextT, axisZ.nextT));
        if (!std::isfinite(nextDistance) || nextDistance > length) {
            break;
        }

        const float tieEpsilon = 0.0005f;
        if (axisX.nextT <= nextDistance + tieEpsilon) {
            if (!TryStepBrickCoord(coord.x, axisX.step, coord.x)) {
                break;
            }
            axisX.nextT += axisX.deltaT;
        }
        if (axisY.nextT <= nextDistance + tieEpsilon) {
            if (!TryStepBrickCoord(coord.y, axisY.step, coord.y)) {
                break;
            }
            axisY.nextT += axisY.deltaT;
        }
        if (axisZ.nextT <= nextDistance + tieEpsilon) {
            if (!TryStepBrickCoord(coord.z, axisZ.step, coord.z)) {
                break;
            }
            axisZ.nextT += axisZ.deltaT;
        }
        distance = std::max(nextDistance + 0.0001f, distance + 0.0001f);
        if (centers.empty() || centers.back() != coord) {
            centers.push_back(coord);
        }
    }
    return centers;
}

bool TryWorldRangeToBrickRange(
    float minXWorld,
    float minYWorld,
    float minZWorld,
    float maxXWorld,
    float maxYWorld,
    float maxZWorld,
    BrickCoord& minBrick,
    BrickCoord& maxBrick)
{
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;
    int32_t maxZ = 0;
    if (!TryFloorToInt32(minXWorld, minX) ||
        !TryFloorToInt32(minYWorld, minY) ||
        !TryFloorToInt32(minZWorld, minZ) ||
        !TryFloorToInt32(maxXWorld, maxX) ||
        !TryFloorToInt32(maxYWorld, maxY) ||
        !TryFloorToInt32(maxZWorld, maxZ)) {
        return false;
    }

    minBrick = BrickCoord::FromWorldVoxel(minX, minY, minZ);
    maxBrick = BrickCoord::FromWorldVoxel(maxX, maxY, maxZ);
    return maxBrick.x >= minBrick.x && maxBrick.y >= minBrick.y && maxBrick.z >= minBrick.z;
}

int32_t ResidencyPriorityBase(SparseResidencyClass residencyClass) {
    switch (residencyClass) {
        case SparseResidencyClass::Edited:
            return -3'000'000;
        case SparseResidencyClass::Collision:
            return -2'000'000;
        case SparseResidencyClass::Visible:
            return -1'000'000;
        case SparseResidencyClass::Speculative:
        default:
            return 0;
    }
}

uint8_t ResidencyRankValue(SparseResidencyClass residencyClass) {
    return static_cast<uint8_t>(residencyClass);
}

} // namespace

SparseBrickRequestPlanner::SparseBrickRequestPlanner(SparseBrickRequestPlannerConfig config)
    : m_config(config) {}

void SparseBrickRequestPlanner::SetConfig(const SparseBrickRequestPlannerConfig& config) {
    m_config = config;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::Plan(
    const BrickCoord& center,
    int32_t forwardX,
    int32_t forwardY,
    int32_t forwardZ) const
{
    std::vector<SparseBrickRequest> requests;
    const uint32_t requestLimit = std::min(m_config.maxRequests, kMaxPlannerRequests);
    requests.reserve(requestLimit);
    std::unordered_set<BrickCoord, BrickCoordHash> seen;

    const auto addRequest = [&](BrickCoord coord, int32_t priority) {
        if (seen.insert(coord).second) {
            requests.push_back({coord, priority});
            return;
        }
        for (SparseBrickRequest& request : requests) {
            if (request.coord == coord && priority < request.priority) {
                request.priority = priority;
                return;
            }
        }
    };

    const int32_t rx = static_cast<int32_t>(std::min(m_config.radiusXz, kMaxPlannerRadiusXz));
    const int32_t ry = static_cast<int32_t>(std::min(m_config.radiusY, kMaxPlannerRadiusY));

    for (int32_t y = -ry; y <= ry; ++y) {
        for (int32_t z = -rx; z <= rx; ++z) {
            for (int32_t x = -rx; x <= rx; ++x) {
                const int64_t distanceScore =
                    static_cast<int64_t>(x) * x +
                    static_cast<int64_t>(z) * z +
                    static_cast<int64_t>(y) * y * 2;
                const int64_t forwardScore =
                    static_cast<int64_t>(x) * forwardX +
                    static_cast<int64_t>(y) * forwardY +
                    static_cast<int64_t>(z) * forwardZ;
                const int32_t priority = ClampPriority(distanceScore * 16 - forwardScore * 3);
                BrickCoord requestCoord;
                if (TryOffsetBrickCoord(center, x, y, z, requestCoord)) {
                    addRequest(requestCoord, priority);
                }
            }
        }
    }

    const int32_t directionLength =
        (forwardX != 0 ? 1 : 0) +
        (forwardY != 0 ? 1 : 0) +
        (forwardZ != 0 ? 1 : 0);
    if (directionLength > 0) {
        const uint32_t forwardPrefetch =
            std::min(m_config.forwardPrefetchBricks, kMaxForwardPrefetchBricks);
        for (uint32_t step = 1; step <= forwardPrefetch; ++step) {
            const int32_t forwardShellOffset =
                static_cast<int32_t>(step + static_cast<uint32_t>(rx));
            BrickCoord ahead;
            if (!TryOffsetBrickCoord(
                    center,
                    forwardX * forwardShellOffset,
                    forwardY * static_cast<int32_t>(step),
                    forwardZ * forwardShellOffset,
                    ahead)) {
                continue;
            }
            const int32_t basePriority = 4 + static_cast<int32_t>(step) * 12;
            for (int32_t y = -ry; y <= ry; ++y) {
                for (int32_t z = -1; z <= 1; ++z) {
                    for (int32_t x = -1; x <= 1; ++x) {
                        BrickCoord requestCoord;
                        if (TryOffsetBrickCoord(ahead, x, y, z, requestCoord)) {
                            addRequest(
                                requestCoord,
                                ClampPriority(
                                    static_cast<int64_t>(basePriority) +
                                    static_cast<int64_t>(x) * x +
                                    static_cast<int64_t>(y) * y +
                                    static_cast<int64_t>(z) * z));
                        }
                    }
                }
            }
        }
    }

    std::sort(requests.begin(), requests.end(), [](const SparseBrickRequest& a, const SparseBrickRequest& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.coord < b.coord;
    });

    if (requests.size() > requestLimit) {
        requests.resize(requestLimit);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanViewCone(
    const SparseViewConeConfig& view) const
{
    std::vector<SparseBrickRequest> requests;
    const uint32_t requestLimit = std::min(view.maxRequests, kMaxPlannerRequests);
    requests.reserve(requestLimit);
    if (view.maxRequests == 0 ||
        !std::isfinite(view.originX) ||
        !std::isfinite(view.originY) ||
        !std::isfinite(view.originZ) ||
        !std::isfinite(view.maxDistance) ||
        view.maxDistance <= 0.0f ||
        !std::isfinite(view.stepDistance) ||
        view.stepDistance <= 0.0f ||
        view.rayGrid == 0) {
        return requests;
    }

    std::unordered_map<BrickCoord, size_t, BrickCoordHash> indexByCoord;
    indexByCoord.reserve(requestLimit);
    const auto addRequest = [&](BrickCoord coord, int32_t priority) {
        auto found = indexByCoord.find(coord);
        if (found == indexByCoord.end()) {
            indexByCoord.emplace(coord, requests.size());
            requests.push_back({coord, priority, SparseResidencyClass::Speculative, false, SparseBrickRequestSource::ViewCone});
            return;
        }

        SparseBrickRequest& request = requests[found->second];
        if (priority < request.priority) {
            request.priority = priority;
        }
    };

    const Vec3 origin{view.originX, view.originY, view.originZ};
    const Vec3 forward = NormalizeOr(
        {view.forwardX, view.forwardY, view.forwardZ},
        {0.0f, 0.0f, 1.0f});
    const Vec3 right = NormalizeOr(
        {view.rightX, view.rightY, view.rightZ},
        {1.0f, 0.0f, 0.0f});
    const Vec3 up = NormalizeOr(
        {view.upX, view.upY, view.upZ},
        {0.0f, 1.0f, 0.0f});

    const float fov = ClampFinite(view.verticalFovRadians, 0.1f, 2.8f, 1.04719755f);
    const float aspect = ClampFinite(view.aspectRatio, 0.25f, 4.0f, 1.7777778f);
    const float maxDistance = ClampFinite(view.maxDistance, 0.0f, kMaxViewConeDistance, 192.0f);
    const float stepDistance = ClampFinite(view.stepDistance, 1.0f, kMaxViewConeStepDistance, 16.0f);
    const float tanHalfFov = std::tan(fov * 0.5f);
    const uint32_t rayGrid = std::clamp<uint32_t>(view.rayGrid, 1u, 7u);
    const float center = static_cast<float>(rayGrid - 1u) * 0.5f;
    const float invCenter = center > 0.0f ? 1.0f / center : 0.0f;
    const uint32_t centerIndex = rayGrid / 2u;
    const float priorityStepDistance = stepDistance;
    const int32_t coverageRadiusXz = static_cast<int32_t>(
        std::min<uint32_t>(view.coverageRadiusXz, 2u));
    const int32_t coverageRadiusY = static_cast<int32_t>(
        std::min<uint32_t>(view.coverageRadiusY, 2u));

    const auto addCoverage = [&](
        BrickCoord centerCoord,
        float distance,
        int32_t lateralPenalty,
        bool centerRay,
        uint32_t ddaStepIndex) {
        const int32_t depthPriority =
            static_cast<int32_t>(std::floor(distance / priorityStepDistance)) * 16 +
            static_cast<int32_t>(ddaStepIndex);
        const int32_t priority =
            depthPriority +
            lateralPenalty +
            (centerRay ? -8 : 0);
        for (int32_t dz = -coverageRadiusXz; dz <= coverageRadiusXz; ++dz) {
            for (int32_t dy = -coverageRadiusY; dy <= coverageRadiusY; ++dy) {
                for (int32_t dx = -coverageRadiusXz; dx <= coverageRadiusXz; ++dx) {
                    const int32_t shellDistance =
                        std::abs(dx) + std::abs(dz) + std::abs(dy);
                    const int32_t shellPenalty = shellDistance == 0
                        ? 0
                        : 96 +
                            (dx * dx + dz * dz) * 8 +
                            dy * dy * 14 +
                            shellDistance * 3;
                    BrickCoord coverageCoord;
                    if (TryOffsetBrickCoord(centerCoord, dx, dy, dz, coverageCoord)) {
                        addRequest(
                            coverageCoord,
                            ClampPriority(
                                static_cast<int64_t>(priority) +
                                static_cast<int64_t>(shellPenalty)));
                    }
                }
            }
        }
    };

    for (uint32_t y = 0; y < rayGrid; ++y) {
        for (uint32_t x = 0; x < rayGrid; ++x) {
            const float ndcX = center > 0.0f ? (static_cast<float>(x) - center) * invCenter : 0.0f;
            const float ndcY = center > 0.0f ? (static_cast<float>(y) - center) * invCenter : 0.0f;
            const Vec3 dir = NormalizeOr(
                Add(
                    Add(forward, Scale(right, ndcX * tanHalfFov * aspect)),
                    Scale(up, ndcY * tanHalfFov)),
                forward);

            const int32_t lateralPenalty =
                static_cast<int32_t>((std::abs(ndcX) + std::abs(ndcY)) * 12.0f);
            int32_t originVoxelX = 0;
            int32_t originVoxelY = 0;
            int32_t originVoxelZ = 0;
            if (!TryFloorToInt32(origin.x, originVoxelX) ||
                !TryFloorToInt32(origin.y, originVoxelY) ||
                !TryFloorToInt32(origin.z, originVoxelZ)) {
                continue;
            }
            BrickCoord coord = BrickCoord::FromWorldVoxel(
                originVoxelX,
                originVoxelY,
                originVoxelZ);
            BrickRayDdaAxis axisX = BuildBrickRayDdaAxis(origin.x, dir.x, coord.x);
            BrickRayDdaAxis axisY = BuildBrickRayDdaAxis(origin.y, dir.y, coord.y);
            BrickRayDdaAxis axisZ = BuildBrickRayDdaAxis(origin.z, dir.z, coord.z);

            // Traverse the brick grid crossed by each view ray instead of
            // sampling isolated points along the ray. Fixed-distance samples
            // can miss oblique brick crossings, which creates dotted residency
            // demand and visible holes during fast yaw/pitch motion.
            float distance = 0.0f;
            uint32_t ddaStepIndex = 0;
            const uint32_t maxDdaSteps = std::clamp<uint32_t>(
                static_cast<uint32_t>(std::ceil(maxDistance / static_cast<float>(SPARSE_BRICK_SIZE))) * 4u + 8u,
                1u,
                1024u);
            while (distance <= maxDistance && ddaStepIndex < maxDdaSteps) {
                addCoverage(
                    coord,
                    distance,
                    lateralPenalty,
                    x == centerIndex && y == centerIndex,
                    ddaStepIndex);

                const float nextDistance = std::min(axisX.nextT, std::min(axisY.nextT, axisZ.nextT));
                if (!std::isfinite(nextDistance) || nextDistance > maxDistance) {
                    break;
                }

                const float tieEpsilon = 0.0005f;
                if (axisX.nextT <= nextDistance + tieEpsilon) {
                    if (!TryStepBrickCoord(coord.x, axisX.step, coord.x)) {
                        break;
                    }
                    axisX.nextT += axisX.deltaT;
                }
                if (axisY.nextT <= nextDistance + tieEpsilon) {
                    if (!TryStepBrickCoord(coord.y, axisY.step, coord.y)) {
                        break;
                    }
                    axisY.nextT += axisY.deltaT;
                }
                if (axisZ.nextT <= nextDistance + tieEpsilon) {
                    if (!TryStepBrickCoord(coord.z, axisZ.step, coord.z)) {
                        break;
                    }
                    axisZ.nextT += axisZ.deltaT;
                }
                distance = std::max(nextDistance + 0.0001f, distance + 0.0001f);
                ++ddaStepIndex;
            }
        }
    }

    std::sort(requests.begin(), requests.end(), [](const SparseBrickRequest& a, const SparseBrickRequest& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.coord < b.coord;
    });

    if (requests.size() > requestLimit) {
        requests.resize(requestLimit);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanCollisionResidency(
    const SparseCollisionResidencyConfig& config) const
{
    std::vector<SparseBrickRequest> requests;
    if (config.maxRequests == 0) {
        return requests;
    }
    const uint32_t requestLimit = std::min(config.maxRequests, kMaxPlannerRequests);
    requests.reserve(requestLimit);

    std::unordered_map<BrickCoord, size_t, BrickCoordHash> indexByCoord;
    const auto addRequest = [&](
        BrickCoord coord,
        int32_t priority,
        bool urgent) -> bool {
        auto found = indexByCoord.find(coord);
        if (found == indexByCoord.end()) {
            indexByCoord.emplace(coord, requests.size());
            requests.push_back({
                coord,
                priority,
                SparseResidencyClass::Collision,
                urgent,
                SparseBrickRequestSource::Collision
            });
            return true;
        }

        SparseBrickRequest& existing = requests[found->second];
        existing.urgent = urgent || existing.urgent;
        if (priority < existing.priority) {
            existing.priority = priority;
        }
        return false;
    };

    const Vec3 velocity{
        FiniteOr(config.velocityX, 0.0f),
        FiniteOr(config.velocityY, 0.0f),
        FiniteOr(config.velocityZ, 0.0f)
    };
    const float velocityLength = Length(velocity);
    const Vec3 velocityDir = NormalizeOr(velocity, {0.0f, 0.0f, 0.0f});
    const uint32_t predictionBricks =
        velocityLength > 0.01f ? std::min(config.predictionBricks, kMaxForwardPrefetchBricks) : 0u;
    const int32_t shellRadiusXz =
        static_cast<int32_t>(std::min(config.shellRadiusXz, kMaxPlannerRadiusXz));
    const int32_t shellRadiusY =
        static_cast<int32_t>(std::min(config.shellRadiusY, kMaxPlannerRadiusY));
    uint32_t added = 0;

    const bool hasBrushIntent =
        config.brushIntentValid &&
        config.maxBrushRequests > 0 &&
        config.maxRequests > 0 &&
        std::isfinite(config.brushStartX) &&
        std::isfinite(config.brushStartY) &&
        std::isfinite(config.brushStartZ) &&
        std::isfinite(config.brushEndX) &&
        std::isfinite(config.brushEndY) &&
        std::isfinite(config.brushEndZ) &&
        std::isfinite(config.brushRadius) &&
        config.brushRadius > 0.0f;
    const uint32_t brushReserve = hasBrushIntent
        ? std::min(
            requestLimit,
            std::min(config.maxBrushRequests, config.reservedBrushRequests))
        : 0u;
    const uint32_t bodyLimit = brushReserve < requestLimit
        ? requestLimit - brushReserve
        : std::min(requestLimit, 1u);

    const float bodyRadius = ClampFinite(config.bodyRadius, 0.1f, 16.0f, 0.75f);
    const float bodyHeight = ClampFinite(config.bodyHeight, 1.0f, 128.0f, 6.0f);
    const float stepHeight = ClampFinite(config.stepHeight, 0.0f, 64.0f, 2.5f);
    const float supportDrop = ClampFinite(config.supportDrop, 0.0f, 128.0f, 4.0f);
    const float predictionSeconds = ClampFinite(config.predictionSeconds, 0.0f, 8.0f, 0.25f);
    const float predictedDistance = std::isfinite(velocityLength) ? velocityLength * predictionSeconds : 0.0f;
    const uint32_t distanceSamples = static_cast<uint32_t>(
        std::ceil(predictedDistance / (static_cast<float>(SPARSE_BRICK_SIZE) * 0.5f))) + 1u;
    const uint32_t bodySamples = std::clamp<uint32_t>(
        std::max(predictionBricks + 1u, distanceSamples),
        1u,
        std::max(1u, std::min(config.maxIntentSamples, kMaxForwardPrefetchBricks)));

    for (uint32_t sampleIndex = 0;
         sampleIndex < bodySamples && added < bodyLimit;
         ++sampleIndex) {
        const float fraction = bodySamples > 1u
            ? static_cast<float>(sampleIndex) / static_cast<float>(bodySamples - 1u)
            : 0.0f;
        const float predictedSeconds = predictionSeconds * fraction;
        const Vec3 eye{
            FiniteOr(config.cameraX, BrickCenterWorldFallback(config.center.x)) + velocity.x * predictedSeconds,
            FiniteOr(config.cameraY, BrickCenterWorldFallback(config.center.y)) + velocity.y * predictedSeconds,
            FiniteOr(config.cameraZ, BrickCenterWorldFallback(config.center.z)) + velocity.z * predictedSeconds
        };
        const float minXWorld = eye.x - bodyRadius;
        const float maxXWorld = eye.x + bodyRadius;
        const float minZWorld = eye.z - bodyRadius;
        const float maxZWorld = eye.z + bodyRadius;
        const float feetY = eye.y - bodyHeight;
        const float minYWorld = feetY - supportDrop;
        const float maxYWorld = eye.y - 0.35f + stepHeight;

        BrickCoord minBrick;
        BrickCoord maxBrick;
        if (!TryWorldRangeToBrickRange(
                minXWorld, minYWorld, minZWorld,
                maxXWorld, maxYWorld, maxZWorld,
                minBrick, maxBrick)) {
            continue;
        }
        for (int32_t z = minBrick.z; z <= maxBrick.z && added < bodyLimit; ++z) {
            for (int32_t y = minBrick.y; y <= maxBrick.y && added < bodyLimit; ++y) {
                for (int32_t x = minBrick.x; x <= maxBrick.x && added < bodyLimit; ++x) {
                    const int32_t priority = ClampPriority(
                        static_cast<int64_t>(sampleIndex) * 48 +
                        AbsDiffInt64(x, config.center.x) * 3 +
                        AbsDiffInt64(y, config.center.y) * 5 +
                        AbsDiffInt64(z, config.center.z) * 3);
                    if (addRequest({x, y, z}, priority, true)) {
                        ++added;
                    }
                }
            }
        }
    }

    if (hasBrushIntent && added < requestLimit) {
        const Vec3 brushStart{config.brushStartX, config.brushStartY, config.brushStartZ};
        const Vec3 brushEnd{config.brushEndX, config.brushEndY, config.brushEndZ};
        const Vec3 brushDelta{
            brushEnd.x - brushStart.x,
            brushEnd.y - brushStart.y,
            brushEnd.z - brushStart.z
        };
        const float brushLength = Length(brushDelta);
        const float brushRadius = std::clamp(config.brushRadius, 0.25f, 64.0f);
        const uint32_t maxBrushCenters = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil(brushLength / static_cast<float>(SPARSE_BRICK_SIZE))) * 3u + 2u,
            1u,
            std::max(1u, config.maxBrushRequests));
        const std::vector<BrickCoord> brushCenters =
            BuildBrickLineDda(brushStart, brushEnd, maxBrushCenters);
        const int32_t brushRadiusBricks = static_cast<int32_t>(
            std::clamp<uint32_t>(
                static_cast<uint32_t>(std::ceil(brushRadius / static_cast<float>(SPARSE_BRICK_SIZE))),
                0u,
                4u));
        uint32_t brushAdded = 0;
        for (uint32_t sampleIndex = 0;
             sampleIndex < brushCenters.size() &&
             brushAdded < config.maxBrushRequests &&
             added < requestLimit;
             ++sampleIndex) {
            const BrickCoord centerBrick = brushCenters[sampleIndex];
            const int32_t priority = ClampPriority(
                static_cast<int64_t>(sampleIndex) * 24 +
                AbsDiffInt64(centerBrick.x, config.center.x) * 2 +
                AbsDiffInt64(centerBrick.y, config.center.y) * 3 +
                AbsDiffInt64(centerBrick.z, config.center.z) * 2);
            if (addRequest(centerBrick, priority, true)) {
                ++brushAdded;
                ++added;
            }
        }
        for (uint32_t sampleIndex = 0;
             sampleIndex < brushCenters.size() &&
             brushAdded < config.maxBrushRequests &&
             added < requestLimit;
             ++sampleIndex) {
            const BrickCoord centerBrick = brushCenters[sampleIndex];
            for (int32_t z = -brushRadiusBricks;
                 z <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < requestLimit;
                 ++z) {
                for (int32_t y = -brushRadiusBricks;
                     y <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < requestLimit;
                     ++y) {
                    for (int32_t x = -brushRadiusBricks;
                         x <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < requestLimit;
                         ++x) {
                        if (x == 0 && y == 0 && z == 0) {
                            continue;
                        }
                        const int32_t priority = ClampPriority(
                            8 +
                            static_cast<int64_t>(sampleIndex) * 24 +
                            static_cast<int64_t>(x * x + z * z) * 3 +
                            static_cast<int64_t>(y * y) * 4 +
                            AbsDiffInt64(centerBrick.x, config.center.x) * 2 +
                            AbsDiffInt64(centerBrick.y, config.center.y) * 3 +
                            AbsDiffInt64(centerBrick.z, config.center.z) * 2);
                        BrickCoord requestCoord;
                        if (TryOffsetBrickCoord(centerBrick, x, y, z, requestCoord) &&
                            addRequest(requestCoord, priority, true)) {
                            ++brushAdded;
                            ++added;
                        }
                    }
                }
            }
        }
    }

    for (uint32_t step = 0; step <= predictionBricks && added < requestLimit; ++step) {
        BrickCoord shellCenter;
        if (!TryOffsetBrickCoord(
                config.center,
                static_cast<int32_t>(std::round(velocityDir.x * static_cast<float>(step))),
                static_cast<int32_t>(std::round(velocityDir.y * static_cast<float>(step))),
                static_cast<int32_t>(std::round(velocityDir.z * static_cast<float>(step))),
                shellCenter)) {
            continue;
        }
        for (int32_t dz = -shellRadiusXz; dz <= shellRadiusXz && added < requestLimit; ++dz) {
            for (int32_t dy = -shellRadiusY; dy <= shellRadiusY && added < requestLimit; ++dy) {
                for (int32_t dx = -shellRadiusXz; dx <= shellRadiusXz && added < requestLimit; ++dx) {
                    const int32_t distanceScore = dx * dx + dz * dz + dy * dy * 2;
                    BrickCoord requestCoord;
                    if (TryOffsetBrickCoord(shellCenter, dx, dy, dz, requestCoord) &&
                        addRequest(
                            requestCoord,
                            ClampPriority(static_cast<int64_t>(step) * 64 + distanceScore),
                            true)) {
                        ++added;
                    }
                }
            }
        }
    }

    std::sort(requests.begin(), requests.end(), [](const SparseBrickRequest& a, const SparseBrickRequest& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.coord < b.coord;
    });
    if (requests.size() > requestLimit) {
        requests.resize(requestLimit);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanHierarchical(
    const SparseHierarchicalRequestConfig& config) const
{
    std::vector<SparseBrickRequest> requests;
    if (config.maxRequests == 0) {
        return requests;
    }
    const uint32_t requestLimit = std::min(config.maxRequests, kMaxPlannerRequests);
    requests.reserve(requestLimit);

    const float cameraX = FiniteOr(config.cameraX, BrickCenterWorldFallback(config.center.x));
    const float cameraY = FiniteOr(config.cameraY, BrickCenterWorldFallback(config.center.y));
    const float cameraZ = FiniteOr(config.cameraZ, BrickCenterWorldFallback(config.center.z));
    const float velocityX = FiniteOr(config.velocityX, 0.0f);
    const float velocityY = FiniteOr(config.velocityY, 0.0f);
    const float velocityZ = FiniteOr(config.velocityZ, 0.0f);
    const float predictionSeconds = ClampFinite(config.predictionSeconds, 0.0f, 8.0f, 0.25f);
    const float collisionBodyHeight = ClampFinite(config.collisionBodyHeight, 1.0f, 128.0f, 6.0f);
    const float visibleDistance = ClampFinite(config.visibleDistance, 0.0f, kMaxViewConeDistance, 256.0f);
    const float speculativeDistance = ClampFinite(
        config.speculativeDistance,
        visibleDistance,
        kMaxViewConeDistance,
        std::max(visibleDistance, 768.0f));
    const float stepDistance = ClampFinite(config.stepDistance, 1.0f, kMaxViewConeStepDistance, 16.0f);

    std::unordered_map<BrickCoord, size_t, BrickCoordHash> indexByCoord;
    const auto addRequest = [&](
        BrickCoord coord,
        int32_t priority,
        SparseResidencyClass residencyClass,
        bool urgent,
        SparseBrickRequestSource source) -> bool {
        priority = ClampPriority(
            static_cast<int64_t>(priority) +
            static_cast<int64_t>(ResidencyPriorityBase(residencyClass)));
        auto found = indexByCoord.find(coord);
        if (found == indexByCoord.end()) {
            indexByCoord.emplace(coord, requests.size());
            requests.push_back({coord, priority, residencyClass, urgent, source});
            return true;
        }

        SparseBrickRequest& existing = requests[found->second];
        if (ResidencyRankValue(residencyClass) > ResidencyRankValue(existing.residencyClass)) {
            existing.residencyClass = residencyClass;
            existing.urgent = urgent || existing.urgent;
            existing.source = source;
        }
        if (priority < existing.priority) {
            existing.priority = priority;
            existing.source = source;
        }
        return false;
    };

    SparseCollisionResidencyConfig collision{};
    collision.center = config.center;
    collision.cameraX = cameraX;
    collision.cameraY = cameraY;
    collision.cameraZ = cameraZ;
    collision.velocityX = velocityX;
    collision.velocityY = velocityY;
    collision.velocityZ = velocityZ;
    collision.predictionSeconds = predictionSeconds;
    collision.bodyHeight = collisionBodyHeight;
    collision.bodyRadius = config.collisionBodyRadius;
    collision.stepHeight = config.collisionStepHeight;
    collision.supportDrop = config.collisionSupportDrop;
    collision.brushIntentValid = config.brushIntentValid;
    collision.brushStartX = config.brushStartX;
    collision.brushStartY = config.brushStartY;
    collision.brushStartZ = config.brushStartZ;
    collision.brushEndX = config.brushEndX;
    collision.brushEndY = config.brushEndY;
    collision.brushEndZ = config.brushEndZ;
    collision.brushRadius = config.brushRadius;
    collision.shellRadiusXz = config.collisionRadiusXz;
    collision.shellRadiusY = config.collisionRadiusY;
    collision.predictionBricks = config.collisionPredictionBricks;
    collision.maxIntentSamples = config.collisionMaxIntentSamples;
    collision.maxBrushRequests = config.maxBrushCollisionRequests;
    collision.reservedBrushRequests = config.reservedBrushCollisionRequests;
    collision.maxRequests = std::min(config.maxCollisionRequests, requestLimit);
    for (const SparseBrickRequest& request : PlanCollisionResidency(collision)) {
        addRequest(
            request.coord,
            request.priority,
            SparseResidencyClass::Collision,
            request.urgent,
            SparseBrickRequestSource::Collision);
    }

    const int32_t nearVisibleRadiusXz =
        static_cast<int32_t>(std::min(config.nearVisibleRadiusXz, kMaxPlannerRadiusXz));
    const int32_t nearVisibleRadiusY =
        static_cast<int32_t>(std::min(config.nearVisibleRadiusY, kMaxPlannerRadiusY));
    struct NearVisibleCandidate {
        BrickCoord coord;
        int32_t priority = 0;
    };
    std::vector<NearVisibleCandidate> nearVisibleCandidates;
    nearVisibleCandidates.reserve(
        static_cast<size_t>(nearVisibleRadiusXz * 2 + 1) *
        static_cast<size_t>(nearVisibleRadiusY * 2 + 1) *
        static_cast<size_t>(nearVisibleRadiusXz * 2 + 1));
    for (int32_t dy = -nearVisibleRadiusY;
         dy <= nearVisibleRadiusY;
         ++dy) {
        for (int32_t dz = -nearVisibleRadiusXz;
             dz <= nearVisibleRadiusXz;
             ++dz) {
            for (int32_t dx = -nearVisibleRadiusXz;
                 dx <= nearVisibleRadiusXz;
                 ++dx) {
                const int32_t planarDistance = dx * dx + dz * dz;
                const int32_t verticalDistance = dy * dy;
                const int32_t priority = ClampPriority(
                    static_cast<int64_t>(planarDistance) * 6 +
                    static_cast<int64_t>(verticalDistance) * 11 +
                    std::abs(dx) + std::abs(dz));
                BrickCoord coord;
                if (TryOffsetBrickCoord(config.center, dx, dy, dz, coord)) {
                    nearVisibleCandidates.push_back({coord, priority});
                }
            }
        }
    }
    std::sort(
        nearVisibleCandidates.begin(),
        nearVisibleCandidates.end(),
        [](const NearVisibleCandidate& a, const NearVisibleCandidate& b) {
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            return a.coord < b.coord;
        });
    uint32_t nearVisibleAdded = 0;
    for (const NearVisibleCandidate& candidate : nearVisibleCandidates) {
        if (nearVisibleAdded >= std::min(config.maxNearVisibleRequests, requestLimit)) {
            break;
        }
        if (addRequest(
                candidate.coord,
                candidate.priority,
                SparseResidencyClass::Visible,
                true,
                SparseBrickRequestSource::NearVisible)) {
            ++nearVisibleAdded;
        }
    }

    const Vec3 velocity{velocityX, velocityY, velocityZ};
    const float velocityLength = Length(velocity);
    const uint32_t maxMotionVisibleRequests =
        std::min(config.maxMotionVisibleRequests, requestLimit);
    if (config.maxMotionVisibleRequests > 0 &&
        velocityLength >= ClampFinite(config.motionVisibleMinSpeed, 0.0f, 4096.0f, 64.0f) &&
        predictionSeconds > 0.0f) {
        const Vec3 velocityDir = NormalizeOr(velocity, {0.0f, 0.0f, 0.0f});
        const float predictedDistance = std::min(velocityLength * predictionSeconds, kMaxViewConeDistance);
        const uint32_t centerSamples = std::clamp<uint32_t>(
            static_cast<uint32_t>(
                std::ceil(predictedDistance / (static_cast<float>(SPARSE_BRICK_SIZE) * 0.75f))) + 1u,
            1u,
            std::max(1u, maxMotionVisibleRequests));
        const int32_t radiusXz =
            static_cast<int32_t>(std::min(config.motionVisibleRadiusXz, kMaxPlannerRadiusXz));
        const int32_t radiusY =
            static_cast<int32_t>(std::min(config.motionVisibleRadiusY, kMaxPlannerRadiusY));
        uint32_t motionVisibleAdded = 0;
        std::vector<BrickCoord> motionCenters;
        motionCenters.reserve(std::max<uint32_t>(centerSamples, maxMotionVisibleRequests));
        const Vec3 motionStart{
            cameraX,
            cameraY - collisionBodyHeight,
            cameraZ
        };
        const Vec3 motionEnd{
            FiniteOr(cameraX + velocityDir.x * predictedDistance, cameraX),
            FiniteOr(cameraY - collisionBodyHeight + velocityDir.y * predictedDistance, cameraY - collisionBodyHeight),
            FiniteOr(cameraZ + velocityDir.z * predictedDistance, cameraZ)
        };
        motionCenters = BuildBrickLineDda(
            motionStart,
            motionEnd,
            std::max<uint32_t>(centerSamples, maxMotionVisibleRequests));
        for (uint32_t sampleIndex = 0; sampleIndex < motionCenters.size(); ++sampleIndex) {
            const BrickCoord sampleCenter = motionCenters[sampleIndex];
            const int32_t priority = ClampPriority(
                -50'000 + static_cast<int64_t>(sampleIndex) * 2);
            if (motionVisibleAdded < maxMotionVisibleRequests &&
                addRequest(
                    sampleCenter,
                    priority,
                    SparseResidencyClass::Visible,
                    true,
                    SparseBrickRequestSource::MotionVisible)) {
                ++motionVisibleAdded;
            }
        }

        for (uint32_t sampleIndex = 0;
             sampleIndex < motionCenters.size() &&
             motionVisibleAdded < maxMotionVisibleRequests;
             ++sampleIndex) {
            const BrickCoord sampleCenter = motionCenters[sampleIndex];
            for (int32_t dz = -radiusXz;
                 dz <= radiusXz && motionVisibleAdded < maxMotionVisibleRequests;
                 ++dz) {
                for (int32_t dy = -radiusY;
                     dy <= radiusY && motionVisibleAdded < maxMotionVisibleRequests;
                     ++dy) {
                    for (int32_t dx = -radiusXz;
                         dx <= radiusXz && motionVisibleAdded < maxMotionVisibleRequests;
                         ++dx) {
                        const int32_t priority = ClampPriority(
                            -40'000 +
                            static_cast<int64_t>(sampleIndex) * 4 +
                            static_cast<int64_t>(dx * dx + dz * dz) * 4 +
                            static_cast<int64_t>(dy * dy) * 9);
                        BrickCoord requestCoord;
                        if (TryOffsetBrickCoord(sampleCenter, dx, dy, dz, requestCoord) &&
                            addRequest(
                                requestCoord,
                                priority,
                                SparseResidencyClass::Visible,
                                true,
                                SparseBrickRequestSource::MotionVisible)) {
                            ++motionVisibleAdded;
                        }
                    }
                }
            }
        }
    }

    const uint32_t ownershipPressureLevel = std::min<uint32_t>(3u, config.ownershipPressureLevel);
    if (ownershipPressureLevel > 0 && config.maxOwnershipRecoveryRequests > 0) {
        SparseViewConeConfig recoveryView;
        recoveryView.originX = cameraX;
        recoveryView.originY = cameraY;
        recoveryView.originZ = cameraZ;
        recoveryView.forwardX = config.forwardX;
        recoveryView.forwardY = config.forwardY;
        recoveryView.forwardZ = config.forwardZ;
        recoveryView.rightX = config.rightX;
        recoveryView.rightY = config.rightY;
        recoveryView.rightZ = config.rightZ;
        recoveryView.upX = config.upX;
        recoveryView.upY = config.upY;
        recoveryView.upZ = config.upZ;
        recoveryView.verticalFovRadians = config.verticalFovRadians;
        recoveryView.aspectRatio = config.aspectRatio;
        recoveryView.maxDistance = std::max(
            visibleDistance,
            visibleDistance + static_cast<float>(ownershipPressureLevel) * stepDistance * 3.0f);
        recoveryView.stepDistance = std::max(
            static_cast<float>(SPARSE_BRICK_SIZE) * 0.5f,
            stepDistance * 0.5f);
        recoveryView.rayGrid = std::max<uint32_t>(
            config.visibleRayGrid,
            std::min<uint32_t>(7u, config.visibleRayGrid + ownershipPressureLevel * 2u));
        recoveryView.coverageRadiusXz = ownershipPressureLevel >= 2 ? 1u : 0u;
        recoveryView.coverageRadiusY = ownershipPressureLevel >= 3 ? 1u : 0u;
        recoveryView.maxRequests = std::min(config.maxOwnershipRecoveryRequests, requestLimit);
        for (const SparseBrickRequest& request : PlanViewCone(recoveryView)) {
            const int32_t priority = ClampPriority(
                -200'000 +
                request.priority -
                static_cast<int64_t>(ownershipPressureLevel) * 10'000);
            addRequest(
                request.coord,
                priority,
                SparseResidencyClass::Visible,
                true,
                SparseBrickRequestSource::OwnershipRecovery);
        }
    }

    SparseViewConeConfig visibleView;
    visibleView.originX = cameraX;
    visibleView.originY = cameraY;
    visibleView.originZ = cameraZ;
    visibleView.forwardX = config.forwardX;
    visibleView.forwardY = config.forwardY;
    visibleView.forwardZ = config.forwardZ;
    visibleView.rightX = config.rightX;
    visibleView.rightY = config.rightY;
    visibleView.rightZ = config.rightZ;
    visibleView.upX = config.upX;
    visibleView.upY = config.upY;
    visibleView.upZ = config.upZ;
    visibleView.verticalFovRadians = config.verticalFovRadians;
    visibleView.aspectRatio = config.aspectRatio;
    visibleView.maxDistance = visibleDistance;
    visibleView.stepDistance = stepDistance;
    visibleView.rayGrid = config.visibleRayGrid;
    visibleView.coverageRadiusXz = 1u;
    visibleView.coverageRadiusY = 1u;
    visibleView.maxRequests = std::min(config.maxVisibleRequests, requestLimit);
    for (const SparseBrickRequest& request : PlanViewCone(visibleView)) {
        addRequest(
            request.coord,
            request.priority,
            SparseResidencyClass::Visible,
            true,
            SparseBrickRequestSource::ViewCone);
    }

    SparseViewConeConfig speculativeView = visibleView;
    speculativeView.originX = FiniteOr(cameraX + velocityX * predictionSeconds, cameraX);
    speculativeView.originY = FiniteOr(cameraY + velocityY * predictionSeconds, cameraY);
    speculativeView.originZ = FiniteOr(cameraZ + velocityZ * predictionSeconds, cameraZ);
    speculativeView.maxDistance = std::max(speculativeDistance, visibleDistance);
    speculativeView.rayGrid = config.speculativeRayGrid;
    speculativeView.coverageRadiusXz = 0u;
    speculativeView.coverageRadiusY = 0u;
    speculativeView.maxRequests = std::min(config.maxSpeculativeRequests, requestLimit);
    for (const SparseBrickRequest& request : PlanViewCone(speculativeView)) {
        addRequest(
            request.coord,
            request.priority,
            SparseResidencyClass::Speculative,
            false,
            SparseBrickRequestSource::SpeculativeView);
    }

    std::sort(requests.begin(), requests.end(), [](const SparseBrickRequest& a, const SparseBrickRequest& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.residencyClass != b.residencyClass) {
            return ResidencyRankValue(a.residencyClass) > ResidencyRankValue(b.residencyClass);
        }
        return a.coord < b.coord;
    });

    if (requests.size() > requestLimit) {
        requests.resize(requestLimit);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanStressVolume(
    const SparseStressRequestConfig& config) const
{
    std::vector<SparseBrickRequest> requests;
    if (config.maxRequests == 0) {
        return requests;
    }
    const uint32_t requestLimit = std::min(config.maxRequests, kMaxPlannerRequests);

    struct Candidate {
        BrickCoord coord;
        int32_t priority = 0;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        bool urgent = false;
        SparseBrickRequestSource source = SparseBrickRequestSource::Stress;
    };

    std::vector<Candidate> candidates;
    const int32_t radiusXz = static_cast<int32_t>(std::min(config.radiusXz, kMaxPlannerRadiusXz));
    const int32_t radiusY = static_cast<int32_t>(std::min(config.radiusY, kMaxPlannerRadiusY));
    const size_t candidateReserve =
        static_cast<size_t>(radiusXz * 2 + 1) *
        static_cast<size_t>(radiusXz * 2 + 1) *
        static_cast<size_t>(radiusY * 2 + 1);
    candidates.reserve(candidateReserve);

    for (int32_t dy = -radiusY; dy <= radiusY; ++dy) {
        for (int32_t dz = -radiusXz; dz <= radiusXz; ++dz) {
            for (int32_t dx = -radiusXz; dx <= radiusXz; ++dx) {
                const int32_t planarDistance = dx * dx + dz * dz;
                const int32_t verticalDistance = dy * dy;
                const bool collisionCore =
                    config.includeCollisionCore &&
                    std::abs(dx) <= 1 &&
                    std::abs(dy) <= 1 &&
                    std::abs(dz) <= 1;
                const bool visibleBand =
                    !collisionCore &&
                    planarDistance <= 9 &&
                    std::abs(dy) <= std::max<int32_t>(1, radiusY / 2);
                const SparseResidencyClass residencyClass = collisionCore
                    ? SparseResidencyClass::Collision
                    : (visibleBand ? SparseResidencyClass::Visible : SparseResidencyClass::Speculative);
                const int32_t shellScore = ClampPriority(
                    static_cast<int64_t>(planarDistance) * 8 +
                    static_cast<int64_t>(verticalDistance) * 13 +
                    std::abs(dx) + std::abs(dz));
                BrickCoord candidateCoord;
                if (TryOffsetBrickCoord(config.center, dx, dy, dz, candidateCoord)) {
                    candidates.push_back({
                        candidateCoord,
                        ClampPriority(static_cast<int64_t>(ResidencyPriorityBase(residencyClass)) + shellScore),
                        residencyClass,
                        residencyClass != SparseResidencyClass::Speculative,
                        SparseBrickRequestSource::Stress
                    });
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.coord < b.coord;
    });

    const size_t takeCount = std::min<size_t>(requestLimit, candidates.size());
    requests.reserve(takeCount);
    if (takeCount == 0) {
        return requests;
    }

    const size_t start = candidates.empty() ? 0 : (static_cast<size_t>(config.cursor) % candidates.size());
    std::unordered_set<BrickCoord, BrickCoordHash> seen;
    for (size_t emitted = 0; emitted < takeCount && emitted < candidates.size(); ++emitted) {
        const Candidate& candidate = candidates[(start + emitted) % candidates.size()];
        if (!seen.insert(candidate.coord).second) {
            continue;
        }
        requests.push_back({
            candidate.coord,
            candidate.priority,
            candidate.residencyClass,
            candidate.urgent,
            candidate.source
        });
    }

    std::sort(requests.begin(), requests.end(), [](const SparseBrickRequest& a, const SparseBrickRequest& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.residencyClass != b.residencyClass) {
            return ResidencyRankValue(a.residencyClass) > ResidencyRankValue(b.residencyClass);
        }
        return a.coord < b.coord;
    });

    return requests;
}

} // namespace VENPOD::Simulation
