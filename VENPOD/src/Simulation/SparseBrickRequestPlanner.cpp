#include "SparseBrickRequestPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
            uint32_t sampleIndex = 0;
            for (float distance = 0.0f;
                 distance <= view.maxDistance;
                 distance += view.stepDistance, ++sampleIndex) {
                const Vec3 sample = Add(origin, Scale(dir, distance));
                const BrickCoord coord = BrickCoord::FromWorldVoxel(
                    static_cast<int32_t>(std::floor(sample.x)),
                    static_cast<int32_t>(std::floor(sample.y)),
                    static_cast<int32_t>(std::floor(sample.z)));
                const int32_t priority =
                    static_cast<int32_t>(sampleIndex) * 16 +
                    lateralPenalty +
                    (x == centerIndex && y == centerIndex ? -8 : 0);
                addRequest(coord, priority);
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

} // namespace VENPOD::Simulation
