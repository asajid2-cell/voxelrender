#include "SparseBrickRequestPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace VENPOD::Simulation {

namespace {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

float Length(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 NormalizeOr(Vec3 v, Vec3 fallback) {
    const float length = Length(v);
    if (!std::isfinite(length) || length <= 0.00001f) {
        return fallback;
    }
    return {v.x / length, v.y / length, v.z / length};
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
    const float brickSize = static_cast<float>(SPARSE_BRICK_SIZE);
    const float boundary = axis.step > 0
        ? static_cast<float>(brickCoord + 1) * brickSize
        : static_cast<float>(brickCoord) * brickSize;
    axis.nextT = (boundary - origin) / direction;
    if (!std::isfinite(axis.nextT) || axis.nextT < 0.0f) {
        axis.nextT = 0.0f;
    }
    axis.deltaT = brickSize / std::abs(direction);
    return axis;
}

std::vector<BrickCoord> BuildBrickLineDda(Vec3 start, Vec3 end, uint32_t maxCenters) {
    std::vector<BrickCoord> centers;
    if (maxCenters == 0) {
        return centers;
    }

    const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = Length(delta);
    const Vec3 dir = NormalizeOr(delta, {0.0f, 0.0f, 1.0f});
    BrickCoord coord = BrickCoord::FromWorldVoxel(
        static_cast<int32_t>(std::floor(start.x)),
        static_cast<int32_t>(std::floor(start.y)),
        static_cast<int32_t>(std::floor(start.z)));
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
            coord.x += axisX.step;
            axisX.nextT += axisX.deltaT;
        }
        if (axisY.nextT <= nextDistance + tieEpsilon) {
            coord.y += axisY.step;
            axisY.nextT += axisY.deltaT;
        }
        if (axisZ.nextT <= nextDistance + tieEpsilon) {
            coord.z += axisZ.step;
            axisZ.nextT += axisZ.deltaT;
        }
        distance = std::max(nextDistance + 0.0001f, distance + 0.0001f);
        if (centers.empty() || centers.back() != coord) {
            centers.push_back(coord);
        }
    }
    return centers;
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
    requests.reserve(m_config.maxRequests);
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

    const int32_t rx = static_cast<int32_t>(m_config.radiusXz);
    const int32_t ry = static_cast<int32_t>(m_config.radiusY);

    for (int32_t y = -ry; y <= ry; ++y) {
        for (int32_t z = -rx; z <= rx; ++z) {
            for (int32_t x = -rx; x <= rx; ++x) {
                const int32_t distanceScore = x * x + z * z + y * y * 2;
                const int32_t forwardScore = x * forwardX + y * forwardY + z * forwardZ;
                const int32_t priority = distanceScore * 16 - forwardScore * 3;
                addRequest({center.x + x, center.y + y, center.z + z}, priority);
            }
        }
    }

    const int32_t directionLength =
        (forwardX != 0 ? 1 : 0) +
        (forwardY != 0 ? 1 : 0) +
        (forwardZ != 0 ? 1 : 0);
    if (directionLength > 0) {
        for (uint32_t step = 1; step <= m_config.forwardPrefetchBricks; ++step) {
            const BrickCoord ahead{
                center.x + forwardX * static_cast<int32_t>(step + m_config.radiusXz),
                center.y + forwardY * static_cast<int32_t>(step),
                center.z + forwardZ * static_cast<int32_t>(step + m_config.radiusXz)
            };
            const int32_t basePriority = 4 + static_cast<int32_t>(step) * 12;
            for (int32_t y = -ry; y <= ry; ++y) {
                for (int32_t z = -1; z <= 1; ++z) {
                    for (int32_t x = -1; x <= 1; ++x) {
                        addRequest({ahead.x + x, ahead.y + y, ahead.z + z}, basePriority + x * x + y * y + z * z);
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

    if (requests.size() > m_config.maxRequests) {
        requests.resize(m_config.maxRequests);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanViewCone(
    const SparseViewConeConfig& view) const
{
    std::vector<SparseBrickRequest> requests;
    requests.reserve(view.maxRequests);
    if (view.maxRequests == 0 ||
        view.maxDistance <= 0.0f ||
        view.stepDistance <= 0.0f ||
        view.rayGrid == 0) {
        return requests;
    }

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

    const float fov = std::clamp(view.verticalFovRadians, 0.1f, 2.8f);
    const float aspect = std::clamp(view.aspectRatio, 0.25f, 4.0f);
    const float tanHalfFov = std::tan(fov * 0.5f);
    const uint32_t rayGrid = std::clamp<uint32_t>(view.rayGrid, 1u, 7u);
    const float center = static_cast<float>(rayGrid - 1u) * 0.5f;
    const float invCenter = center > 0.0f ? 1.0f / center : 0.0f;
    const uint32_t centerIndex = rayGrid / 2u;
    const float priorityStepDistance = std::max(view.stepDistance, 1.0f);
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
                    addRequest(
                        {centerCoord.x + dx, centerCoord.y + dy, centerCoord.z + dz},
                        priority + shellPenalty);
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
            BrickCoord coord = BrickCoord::FromWorldVoxel(
                static_cast<int32_t>(std::floor(origin.x)),
                static_cast<int32_t>(std::floor(origin.y)),
                static_cast<int32_t>(std::floor(origin.z)));
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
                static_cast<uint32_t>(std::ceil(view.maxDistance / static_cast<float>(SPARSE_BRICK_SIZE))) * 4u + 8u,
                1u,
                1024u);
            while (distance <= view.maxDistance && ddaStepIndex < maxDdaSteps) {
                addCoverage(
                    coord,
                    distance,
                    lateralPenalty,
                    x == centerIndex && y == centerIndex,
                    ddaStepIndex);

                const float nextDistance = std::min(axisX.nextT, std::min(axisY.nextT, axisZ.nextT));
                if (!std::isfinite(nextDistance) || nextDistance > view.maxDistance) {
                    break;
                }

                const float tieEpsilon = 0.0005f;
                if (axisX.nextT <= nextDistance + tieEpsilon) {
                    coord.x += axisX.step;
                    axisX.nextT += axisX.deltaT;
                }
                if (axisY.nextT <= nextDistance + tieEpsilon) {
                    coord.y += axisY.step;
                    axisY.nextT += axisY.deltaT;
                }
                if (axisZ.nextT <= nextDistance + tieEpsilon) {
                    coord.z += axisZ.step;
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

    if (requests.size() > view.maxRequests) {
        requests.resize(view.maxRequests);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanCollisionResidency(
    const SparseCollisionResidencyConfig& config) const
{
    std::vector<SparseBrickRequest> requests;
    requests.reserve(config.maxRequests);
    if (config.maxRequests == 0) {
        return requests;
    }

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
                urgent
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

    const Vec3 velocity{config.velocityX, config.velocityY, config.velocityZ};
    const float velocityLength = Length(velocity);
    const Vec3 velocityDir = NormalizeOr(velocity, {0.0f, 0.0f, 0.0f});
    const uint32_t predictionBricks = velocityLength > 0.01f ? config.predictionBricks : 0u;
    const int32_t shellRadiusXz = static_cast<int32_t>(config.shellRadiusXz);
    const int32_t shellRadiusY = static_cast<int32_t>(config.shellRadiusY);
    uint32_t added = 0;

    const bool hasBrushIntent =
        config.brushIntentValid &&
        config.maxBrushRequests > 0 &&
        config.maxRequests > 0;
    const uint32_t brushReserve = hasBrushIntent
        ? std::min(
            config.maxRequests,
            std::min(config.maxBrushRequests, config.reservedBrushRequests))
        : 0u;
    const uint32_t bodyLimit = brushReserve < config.maxRequests
        ? config.maxRequests - brushReserve
        : std::min(config.maxRequests, 1u);

    const float bodyRadius = std::max(0.1f, config.bodyRadius);
    const float bodyHeight = std::max(1.0f, config.bodyHeight);
    const float stepHeight = std::max(0.0f, config.stepHeight);
    const float supportDrop = std::max(0.0f, config.supportDrop);
    const float predictedDistance = velocityLength * std::max(0.0f, config.predictionSeconds);
    const uint32_t distanceSamples = static_cast<uint32_t>(
        std::ceil(predictedDistance / (static_cast<float>(SPARSE_BRICK_SIZE) * 0.5f))) + 1u;
    const uint32_t bodySamples = std::clamp<uint32_t>(
        std::max(predictionBricks + 1u, distanceSamples),
        1u,
        std::max(1u, config.maxIntentSamples));

    for (uint32_t sampleIndex = 0;
         sampleIndex < bodySamples && added < bodyLimit;
         ++sampleIndex) {
        const float fraction = bodySamples > 1u
            ? static_cast<float>(sampleIndex) / static_cast<float>(bodySamples - 1u)
            : 0.0f;
        const float predictedSeconds = std::max(0.0f, config.predictionSeconds) * fraction;
        const Vec3 eye{
            config.cameraX + config.velocityX * predictedSeconds,
            config.cameraY + config.velocityY * predictedSeconds,
            config.cameraZ + config.velocityZ * predictedSeconds
        };
        const float minXWorld = eye.x - bodyRadius;
        const float maxXWorld = eye.x + bodyRadius;
        const float minZWorld = eye.z - bodyRadius;
        const float maxZWorld = eye.z + bodyRadius;
        const float feetY = eye.y - bodyHeight;
        const float minYWorld = feetY - supportDrop;
        const float maxYWorld = eye.y - 0.35f + stepHeight;

        const BrickCoord minBrick = BrickCoord::FromWorldVoxel(
            static_cast<int32_t>(std::floor(minXWorld)),
            static_cast<int32_t>(std::floor(minYWorld)),
            static_cast<int32_t>(std::floor(minZWorld)));
        const BrickCoord maxBrick = BrickCoord::FromWorldVoxel(
            static_cast<int32_t>(std::floor(maxXWorld)),
            static_cast<int32_t>(std::floor(maxYWorld)),
            static_cast<int32_t>(std::floor(maxZWorld)));
        for (int32_t z = minBrick.z; z <= maxBrick.z && added < bodyLimit; ++z) {
            for (int32_t y = minBrick.y; y <= maxBrick.y && added < bodyLimit; ++y) {
                for (int32_t x = minBrick.x; x <= maxBrick.x && added < bodyLimit; ++x) {
                    const int32_t priority =
                        static_cast<int32_t>(sampleIndex) * 48 +
                        std::abs(x - config.center.x) * 3 +
                        std::abs(y - config.center.y) * 5 +
                        std::abs(z - config.center.z) * 3;
                    if (addRequest({x, y, z}, priority, true)) {
                        ++added;
                    }
                }
            }
        }
    }

    if (hasBrushIntent && added < config.maxRequests) {
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
             added < config.maxRequests;
             ++sampleIndex) {
            const BrickCoord centerBrick = brushCenters[sampleIndex];
            const int32_t priority =
                static_cast<int32_t>(sampleIndex) * 24 +
                std::abs(centerBrick.x - config.center.x) * 2 +
                std::abs(centerBrick.y - config.center.y) * 3 +
                std::abs(centerBrick.z - config.center.z) * 2;
            if (addRequest(centerBrick, priority, true)) {
                ++brushAdded;
                ++added;
            }
        }
        for (uint32_t sampleIndex = 0;
             sampleIndex < brushCenters.size() &&
             brushAdded < config.maxBrushRequests &&
             added < config.maxRequests;
             ++sampleIndex) {
            const BrickCoord centerBrick = brushCenters[sampleIndex];
            for (int32_t z = -brushRadiusBricks;
                 z <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < config.maxRequests;
                 ++z) {
                for (int32_t y = -brushRadiusBricks;
                     y <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < config.maxRequests;
                     ++y) {
                    for (int32_t x = -brushRadiusBricks;
                         x <= brushRadiusBricks && brushAdded < config.maxBrushRequests && added < config.maxRequests;
                         ++x) {
                        if (x == 0 && y == 0 && z == 0) {
                            continue;
                        }
                        const int32_t priority =
                            8 +
                            static_cast<int32_t>(sampleIndex) * 24 +
                            (x * x + z * z) * 3 +
                            y * y * 4 +
                            std::abs(centerBrick.x - config.center.x) * 2 +
                            std::abs(centerBrick.y - config.center.y) * 3 +
                            std::abs(centerBrick.z - config.center.z) * 2;
                        if (addRequest(
                                {centerBrick.x + x, centerBrick.y + y, centerBrick.z + z},
                                priority,
                                true)) {
                            ++brushAdded;
                            ++added;
                        }
                    }
                }
            }
        }
    }

    for (uint32_t step = 0; step <= predictionBricks && added < config.maxRequests; ++step) {
        const BrickCoord shellCenter{
            config.center.x + static_cast<int32_t>(std::round(velocityDir.x * static_cast<float>(step))),
            config.center.y + static_cast<int32_t>(std::round(velocityDir.y * static_cast<float>(step))),
            config.center.z + static_cast<int32_t>(std::round(velocityDir.z * static_cast<float>(step)))
        };
        for (int32_t dz = -shellRadiusXz; dz <= shellRadiusXz && added < config.maxRequests; ++dz) {
            for (int32_t dy = -shellRadiusY; dy <= shellRadiusY && added < config.maxRequests; ++dy) {
                for (int32_t dx = -shellRadiusXz; dx <= shellRadiusXz && added < config.maxRequests; ++dx) {
                    const int32_t distanceScore = dx * dx + dz * dz + dy * dy * 2;
                    if (addRequest(
                            {shellCenter.x + dx, shellCenter.y + dy, shellCenter.z + dz},
                            static_cast<int32_t>(step) * 64 + distanceScore,
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
    if (requests.size() > config.maxRequests) {
        requests.resize(config.maxRequests);
    }
    return requests;
}

std::vector<SparseBrickRequest> SparseBrickRequestPlanner::PlanHierarchical(
    const SparseHierarchicalRequestConfig& config) const
{
    std::vector<SparseBrickRequest> requests;
    requests.reserve(config.maxRequests);
    if (config.maxRequests == 0) {
        return requests;
    }

    std::unordered_map<BrickCoord, size_t, BrickCoordHash> indexByCoord;
    const auto addRequest = [&](
        BrickCoord coord,
        int32_t priority,
        SparseResidencyClass residencyClass,
        bool urgent) -> bool {
        priority += ResidencyPriorityBase(residencyClass);
        auto found = indexByCoord.find(coord);
        if (found == indexByCoord.end()) {
            indexByCoord.emplace(coord, requests.size());
            requests.push_back({coord, priority, residencyClass, urgent});
            return true;
        }

        SparseBrickRequest& existing = requests[found->second];
        if (ResidencyRankValue(residencyClass) > ResidencyRankValue(existing.residencyClass)) {
            existing.residencyClass = residencyClass;
            existing.urgent = urgent || existing.urgent;
        }
        if (priority < existing.priority) {
            existing.priority = priority;
        }
        return false;
    };

    SparseCollisionResidencyConfig collision{};
    collision.center = config.center;
    collision.cameraX = config.cameraX;
    collision.cameraY = config.cameraY;
    collision.cameraZ = config.cameraZ;
    collision.velocityX = config.velocityX;
    collision.velocityY = config.velocityY;
    collision.velocityZ = config.velocityZ;
    collision.predictionSeconds = config.predictionSeconds;
    collision.bodyHeight = config.collisionBodyHeight;
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
    collision.maxRequests = config.maxCollisionRequests;
    for (const SparseBrickRequest& request : PlanCollisionResidency(collision)) {
        addRequest(
            request.coord,
            request.priority,
            SparseResidencyClass::Collision,
            request.urgent);
    }

    const int32_t nearVisibleRadiusXz = static_cast<int32_t>(config.nearVisibleRadiusXz);
    const int32_t nearVisibleRadiusY = static_cast<int32_t>(config.nearVisibleRadiusY);
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
                const int32_t priority =
                    planarDistance * 6 +
                    verticalDistance * 11 +
                    std::abs(dx) + std::abs(dz);
                nearVisibleCandidates.push_back({
                    {config.center.x + dx, config.center.y + dy, config.center.z + dz},
                    priority
                });
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
        if (nearVisibleAdded >= config.maxNearVisibleRequests) {
            break;
        }
        if (addRequest(
                candidate.coord,
                candidate.priority,
                SparseResidencyClass::Visible,
                true)) {
            ++nearVisibleAdded;
        }
    }

    const Vec3 velocity{config.velocityX, config.velocityY, config.velocityZ};
    const float velocityLength = Length(velocity);
    if (config.maxMotionVisibleRequests > 0 &&
        velocityLength >= std::max(0.0f, config.motionVisibleMinSpeed) &&
        config.predictionSeconds > 0.0f) {
        const Vec3 velocityDir = NormalizeOr(velocity, {0.0f, 0.0f, 0.0f});
        const float predictedDistance = velocityLength * std::max(0.0f, config.predictionSeconds);
        const uint32_t centerSamples = std::clamp<uint32_t>(
            static_cast<uint32_t>(
                std::ceil(predictedDistance / (static_cast<float>(SPARSE_BRICK_SIZE) * 0.75f))) + 1u,
            1u,
            std::max(1u, config.maxMotionVisibleRequests));
        const int32_t radiusXz = static_cast<int32_t>(config.motionVisibleRadiusXz);
        const int32_t radiusY = static_cast<int32_t>(config.motionVisibleRadiusY);
        uint32_t motionVisibleAdded = 0;
        std::vector<BrickCoord> motionCenters;
        motionCenters.reserve(std::max<uint32_t>(centerSamples, config.maxMotionVisibleRequests));
        const Vec3 motionStart{
            config.cameraX,
            config.cameraY - config.collisionBodyHeight,
            config.cameraZ
        };
        const Vec3 motionEnd{
            config.cameraX + velocityDir.x * predictedDistance,
            config.cameraY - config.collisionBodyHeight + velocityDir.y * predictedDistance,
            config.cameraZ + velocityDir.z * predictedDistance
        };
        motionCenters = BuildBrickLineDda(
            motionStart,
            motionEnd,
            std::max<uint32_t>(centerSamples, config.maxMotionVisibleRequests));
        for (uint32_t sampleIndex = 0; sampleIndex < motionCenters.size(); ++sampleIndex) {
            const BrickCoord sampleCenter = motionCenters[sampleIndex];
            const int32_t priority =
                -50'000 +
                static_cast<int32_t>(sampleIndex) * 2;
            if (motionVisibleAdded < config.maxMotionVisibleRequests &&
                addRequest(
                    sampleCenter,
                    priority,
                    SparseResidencyClass::Visible,
                    true)) {
                ++motionVisibleAdded;
            }
        }

        for (uint32_t sampleIndex = 0;
             sampleIndex < motionCenters.size() &&
             motionVisibleAdded < config.maxMotionVisibleRequests;
             ++sampleIndex) {
            const BrickCoord sampleCenter = motionCenters[sampleIndex];
            for (int32_t dz = -radiusXz;
                 dz <= radiusXz && motionVisibleAdded < config.maxMotionVisibleRequests;
                 ++dz) {
                for (int32_t dy = -radiusY;
                     dy <= radiusY && motionVisibleAdded < config.maxMotionVisibleRequests;
                     ++dy) {
                    for (int32_t dx = -radiusXz;
                         dx <= radiusXz && motionVisibleAdded < config.maxMotionVisibleRequests;
                         ++dx) {
                        const int32_t priority =
                            -40'000 +
                            static_cast<int32_t>(sampleIndex) * 4 +
                            (dx * dx + dz * dz) * 4 +
                            dy * dy * 9;
                        if (addRequest(
                                {sampleCenter.x + dx, sampleCenter.y + dy, sampleCenter.z + dz},
                                priority,
                                SparseResidencyClass::Visible,
                                true)) {
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
        recoveryView.originX = config.cameraX;
        recoveryView.originY = config.cameraY;
        recoveryView.originZ = config.cameraZ;
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
            config.visibleDistance,
            config.visibleDistance + static_cast<float>(ownershipPressureLevel) * config.stepDistance * 3.0f);
        recoveryView.stepDistance = std::max(
            static_cast<float>(SPARSE_BRICK_SIZE) * 0.5f,
            config.stepDistance * 0.5f);
        recoveryView.rayGrid = std::max<uint32_t>(
            config.visibleRayGrid,
            std::min<uint32_t>(7u, config.visibleRayGrid + ownershipPressureLevel * 2u));
        recoveryView.coverageRadiusXz = ownershipPressureLevel >= 2 ? 1u : 0u;
        recoveryView.coverageRadiusY = ownershipPressureLevel >= 3 ? 1u : 0u;
        recoveryView.maxRequests = config.maxOwnershipRecoveryRequests;
        for (const SparseBrickRequest& request : PlanViewCone(recoveryView)) {
            const int32_t priority =
                -200'000 +
                request.priority -
                static_cast<int32_t>(ownershipPressureLevel) * 10'000;
            addRequest(
                request.coord,
                priority,
                SparseResidencyClass::Visible,
                true);
        }
    }

    SparseViewConeConfig visibleView;
    visibleView.originX = config.cameraX;
    visibleView.originY = config.cameraY;
    visibleView.originZ = config.cameraZ;
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
    visibleView.maxDistance = config.visibleDistance;
    visibleView.stepDistance = config.stepDistance;
    visibleView.rayGrid = config.visibleRayGrid;
    visibleView.coverageRadiusXz = 1u;
    visibleView.coverageRadiusY = 1u;
    visibleView.maxRequests = config.maxVisibleRequests;
    for (const SparseBrickRequest& request : PlanViewCone(visibleView)) {
        addRequest(request.coord, request.priority, SparseResidencyClass::Visible, true);
    }

    SparseViewConeConfig speculativeView = visibleView;
    const float predictionSeconds = std::max(0.0f, config.predictionSeconds);
    speculativeView.originX = config.cameraX + config.velocityX * predictionSeconds;
    speculativeView.originY = config.cameraY + config.velocityY * predictionSeconds;
    speculativeView.originZ = config.cameraZ + config.velocityZ * predictionSeconds;
    speculativeView.maxDistance = std::max(config.speculativeDistance, config.visibleDistance);
    speculativeView.rayGrid = config.speculativeRayGrid;
    speculativeView.coverageRadiusXz = 0u;
    speculativeView.coverageRadiusY = 0u;
    speculativeView.maxRequests = config.maxSpeculativeRequests;
    for (const SparseBrickRequest& request : PlanViewCone(speculativeView)) {
        addRequest(request.coord, request.priority, SparseResidencyClass::Speculative, false);
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

    if (requests.size() > config.maxRequests) {
        requests.resize(config.maxRequests);
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

    struct Candidate {
        BrickCoord coord;
        int32_t priority = 0;
        SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
        bool urgent = false;
    };

    std::vector<Candidate> candidates;
    const int32_t radiusXz = static_cast<int32_t>(config.radiusXz);
    const int32_t radiusY = static_cast<int32_t>(config.radiusY);
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
                const int32_t shellScore =
                    planarDistance * 8 +
                    verticalDistance * 13 +
                    std::abs(dx) + std::abs(dz);
                candidates.push_back({
                    {config.center.x + dx, config.center.y + dy, config.center.z + dz},
                    ResidencyPriorityBase(residencyClass) + shellScore,
                    residencyClass,
                    residencyClass != SparseResidencyClass::Speculative
                });
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.coord < b.coord;
    });

    const size_t takeCount = std::min<size_t>(config.maxRequests, candidates.size());
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
            candidate.urgent
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
