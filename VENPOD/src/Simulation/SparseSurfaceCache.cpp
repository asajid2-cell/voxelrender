#include "SparseSurfaceCache.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

namespace VENPOD::Simulation {

namespace {

uint32_t NextPowerOfTwo(uint32_t value) {
    if (value <= 1u) {
        return 1u;
    }
    --value;
    value |= value >> 1u;
    value |= value >> 2u;
    value |= value >> 4u;
    value |= value >> 8u;
    value |= value >> 16u;
    return value + 1u;
}

bool BrickPassesVisibility(
    const BrickCoord& coord,
    const SparseSurfaceVisibilityConfig& visibility,
    bool* outUsedLookahead = nullptr)
{
    if (outUsedLookahead) {
        *outUsedLookahead = false;
    }
    if (!visibility.enabled) {
        return true;
    }

    constexpr float kBrickSize = static_cast<float>(SPARSE_BRICK_SIZE);
    constexpr float kBrickHalf = kBrickSize * 0.5f;
    constexpr float kBrickRadius = 13.856406f; // sqrt(3) * 8

    const float centerX = static_cast<float>(coord.x * SPARSE_BRICK_SIZE) + kBrickHalf;
    const float centerY = static_cast<float>(coord.y * SPARSE_BRICK_SIZE) + kBrickHalf;
    const float centerZ = static_cast<float>(coord.z * SPARSE_BRICK_SIZE) + kBrickHalf;
    const float dx = centerX - visibility.cameraX;
    const float dy = centerY - visibility.cameraY;
    const float dz = centerZ - visibility.cameraZ;
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    const float maxDistance = std::max(kBrickSize, visibility.maxDistance + visibility.padding + kBrickRadius);
    const bool insideCurrentSphere = distanceSq <= maxDistance * maxDistance;
    bool insideLookaheadSphere = false;
    if (!insideCurrentSphere && visibility.useMotionLookahead) {
        const float lookaheadDx = centerX - visibility.lookaheadCameraX;
        const float lookaheadDy = centerY - visibility.lookaheadCameraY;
        const float lookaheadDz = centerZ - visibility.lookaheadCameraZ;
        const float lookaheadDistanceSq =
            lookaheadDx * lookaheadDx +
            lookaheadDy * lookaheadDy +
            lookaheadDz * lookaheadDz;
        insideLookaheadSphere = lookaheadDistanceSq <= maxDistance * maxDistance;
    }
    if (!insideCurrentSphere && !insideLookaheadSphere) {
        return false;
    }
    if (insideLookaheadSphere) {
        if (outUsedLookahead) {
            *outUsedLookahead = true;
        }
        return true;
    }
    if (!visibility.useFrustum) {
        return true;
    }

    const float z =
        dx * visibility.forwardX +
        dy * visibility.forwardY +
        dz * visibility.forwardZ;
    if (z < -(visibility.padding + kBrickRadius)) {
        return false;
    }

    const float tanHalfY = std::tan(std::max(0.1f, visibility.fovYRadians) * 0.5f);
    const float tanHalfX = tanHalfY * std::max(0.1f, visibility.aspectRatio);
    const float x =
        dx * visibility.rightX +
        dy * visibility.rightY +
        dz * visibility.rightZ;
    const float y =
        dx * visibility.upX +
        dy * visibility.upY +
        dz * visibility.upZ;
    const float zForFrustum = std::max(0.0f, z);
    const float xLimit = zForFrustum * tanHalfX + visibility.padding + kBrickRadius;
    const float yLimit = zForFrustum * tanHalfY + visibility.padding + kBrickRadius;
    return std::abs(x) <= xLimit && std::abs(y) <= yLimit;
}

bool BrickCoordLess(const BrickCoord& lhs, const BrickCoord& rhs) {
    if (lhs.x != rhs.x) {
        return lhs.x < rhs.x;
    }
    if (lhs.y != rhs.y) {
        return lhs.y < rhs.y;
    }
    return lhs.z < rhs.z;
}

bool HasResidencyFlag(uint32_t flags, BrickResidencyFlags flag) {
    return (flags & static_cast<uint32_t>(flag)) != 0u;
}

} // namespace

uint32_t BuildSparseSurfaceDirectionMask(const std::vector<SparseSurfaceFace>& faces) {
    uint32_t mask = 0;
    for (const SparseSurfaceFace& face : faces) {
        mask |= SparseSurfaceDirectionBit(SparseSurfacePayloadDirection(face.payload));
    }
    return mask & kSparseSurfaceDirectionMaskBits;
}

void ComputeSparseSurfaceFaceBounds(
    const SparseSurfaceFace* faces,
    uint32_t faceCount,
    int32_t* outMinX,
    int32_t* outMinY,
    int32_t* outMinZ,
    int32_t* outMaxX,
    int32_t* outMaxY,
    int32_t* outMaxZ)
{
    int32_t minX = INT_MAX;
    int32_t minY = INT_MAX;
    int32_t minZ = INT_MAX;
    int32_t maxX = INT_MIN;
    int32_t maxY = INT_MIN;
    int32_t maxZ = INT_MIN;

    if (faceCount == 0u || !faces) {
        minX = minY = minZ = 0;
        maxX = maxY = maxZ = 0;
    }

    for (uint32_t i = 0; i < faceCount && faces; ++i) {
        const SparseSurfaceFace& face = faces[i];
        const int32_t width = static_cast<int32_t>(SparseSurfacePayloadWidth(face.payload));
        const int32_t height = static_cast<int32_t>(SparseSurfacePayloadHeight(face.payload));
        int32_t faceMinX = face.worldX;
        int32_t faceMinY = face.worldY;
        int32_t faceMinZ = face.worldZ;
        int32_t faceMaxX = face.worldX + 1;
        int32_t faceMaxY = face.worldY + 1;
        int32_t faceMaxZ = face.worldZ + 1;
        switch (static_cast<SparseFaceDirection>(SparseSurfacePayloadDirection(face.payload))) {
        case SparseFaceDirection::NegX:
        case SparseFaceDirection::PosX:
            faceMaxY = face.worldY + height;
            faceMaxZ = face.worldZ + width;
            break;
        case SparseFaceDirection::NegY:
        case SparseFaceDirection::PosY:
            faceMaxX = face.worldX + width;
            faceMaxZ = face.worldZ + height;
            break;
        case SparseFaceDirection::NegZ:
        case SparseFaceDirection::PosZ:
        default:
            faceMaxX = face.worldX + width;
            faceMaxY = face.worldY + height;
            break;
        }
        minX = std::min(minX, faceMinX);
        minY = std::min(minY, faceMinY);
        minZ = std::min(minZ, faceMinZ);
        maxX = std::max(maxX, faceMaxX);
        maxY = std::max(maxY, faceMaxY);
        maxZ = std::max(maxZ, faceMaxZ);
    }

    if (outMinX) *outMinX = minX;
    if (outMinY) *outMinY = minY;
    if (outMinZ) *outMinZ = minZ;
    if (outMaxX) *outMaxX = maxX;
    if (outMaxY) *outMaxY = maxY;
    if (outMaxZ) *outMaxZ = maxZ;
}

uint64_t SparseSurfaceMortonKey(const BrickCoord& coord) {
    constexpr uint32_t kBitsPerAxis = 21u;
    constexpr uint32_t kAxisMask = (1u << kBitsPerAxis) - 1u;
    constexpr int32_t kSignedBias = 1 << (kBitsPerAxis - 1u);
    const uint32_t x = static_cast<uint32_t>(coord.x + kSignedBias) & kAxisMask;
    const uint32_t y = static_cast<uint32_t>(coord.y + kSignedBias) & kAxisMask;
    const uint32_t z = static_cast<uint32_t>(coord.z + kSignedBias) & kAxisMask;

    uint64_t key = 0;
    for (uint32_t bit = 0; bit < kBitsPerAxis; ++bit) {
        key |= static_cast<uint64_t>((x >> bit) & 1u) << (bit * 3u + 0u);
        key |= static_cast<uint64_t>((y >> bit) & 1u) << (bit * 3u + 1u);
        key |= static_cast<uint64_t>((z >> bit) & 1u) << (bit * 3u + 2u);
    }
    return key;
}

void SortSparseSurfaceRecordsForClusters(std::vector<SparseSurfaceRecord>& records) {
    std::sort(records.begin(), records.end(), [](const SparseSurfaceRecord& lhs, const SparseSurfaceRecord& rhs) {
        const uint64_t lhsKey = SparseSurfaceMortonKey(lhs.coord);
        const uint64_t rhsKey = SparseSurfaceMortonKey(rhs.coord);
        if (lhsKey != rhsKey) {
            return lhsKey < rhsKey;
        }
        return BrickCoordLess(lhs.coord, rhs.coord);
    });
}

std::vector<SparseSurfaceClusterRecord> BuildSparseSurfaceClusters(
    const std::vector<SparseSurfaceRecord>& records,
    uint32_t recordsPerCluster,
    uint32_t maxClusterExtentVoxels)
{
    recordsPerCluster = std::max(1u, recordsPerCluster);
    std::vector<SparseSurfaceClusterRecord> clusters;
    clusters.reserve((records.size() + recordsPerCluster - 1u) / recordsPerCluster);

    auto flushCluster = [&](size_t first, size_t count) {
        SparseSurfaceClusterRecord cluster;
        cluster.firstRecord = static_cast<uint32_t>(first);
        cluster.recordCount = static_cast<uint32_t>(count);
        cluster.minX = INT_MAX;
        cluster.minY = INT_MAX;
        cluster.minZ = INT_MAX;
        cluster.maxX = INT_MIN;
        cluster.maxY = INT_MIN;
        cluster.maxZ = INT_MIN;
        cluster.faceCount = 0;
        uint32_t directionMask = 0;
        for (size_t i = 0; i < count; ++i) {
            const SparseSurfaceRecord& record = records[first + i];
            cluster.faceCount += record.faceCount;
            directionMask |= SparseSurfaceRecordDirectionMask(record.flags);
            cluster.minX = std::min(cluster.minX, record.minX);
            cluster.minY = std::min(cluster.minY, record.minY);
            cluster.minZ = std::min(cluster.minZ, record.minZ);
            cluster.maxX = std::max(cluster.maxX, record.maxX);
            cluster.maxY = std::max(cluster.maxY, record.maxY);
            cluster.maxZ = std::max(cluster.maxZ, record.maxZ);
        }
        cluster.flags = SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, directionMask);
        clusters.push_back(cluster);
    };

    size_t first = 0;
    while (first < records.size()) {
        int32_t minX = INT_MAX;
        int32_t minY = INT_MAX;
        int32_t minZ = INT_MAX;
        int32_t maxX = INT_MIN;
        int32_t maxY = INT_MIN;
        int32_t maxZ = INT_MIN;
        size_t count = 0;

        for (size_t i = first; i < records.size(); ++i) {
            const SparseSurfaceRecord& record = records[i];
            const int32_t nextMinX = std::min(minX, record.minX);
            const int32_t nextMinY = std::min(minY, record.minY);
            const int32_t nextMinZ = std::min(minZ, record.minZ);
            const int32_t nextMaxX = std::max(maxX, record.maxX);
            const int32_t nextMaxY = std::max(maxY, record.maxY);
            const int32_t nextMaxZ = std::max(maxZ, record.maxZ);
            const bool exceedsExtent =
                maxClusterExtentVoxels > 0u &&
                count > 0u &&
                (static_cast<uint32_t>(std::max(0, nextMaxX - nextMinX)) > maxClusterExtentVoxels ||
                 static_cast<uint32_t>(std::max(0, nextMaxY - nextMinY)) > maxClusterExtentVoxels ||
                 static_cast<uint32_t>(std::max(0, nextMaxZ - nextMinZ)) > maxClusterExtentVoxels);
            if (count >= recordsPerCluster || exceedsExtent) {
                break;
            }

            minX = nextMinX;
            minY = nextMinY;
            minZ = nextMinZ;
            maxX = nextMaxX;
            maxY = nextMaxY;
            maxZ = nextMaxZ;
            ++count;
        }

        if (count == 0u) {
            count = 1u;
        }
        flushCluster(first, count);
        first += count;
    }
    return clusters;
}

std::vector<SparseSurfaceFaceRun> BuildSparseSurfaceChangedFaceRuns(
    const SparseSurfaceFace* currentFaces,
    const SparseSurfaceFace* previousFaces,
    uint32_t faceCount)
{
    std::vector<SparseSurfaceFaceRun> runs;
    if (faceCount == 0u) {
        return runs;
    }
    if (!currentFaces || !previousFaces) {
        runs.push_back({0u, faceCount});
        return runs;
    }

    uint32_t index = 0;
    while (index < faceCount) {
        const bool same = std::memcmp(
            currentFaces + index,
            previousFaces + index,
            sizeof(SparseSurfaceFace)) == 0;
        if (same) {
            ++index;
            continue;
        }

        const uint32_t first = index;
        ++index;
        while (index < faceCount) {
            const bool nextSame = std::memcmp(
                currentFaces + index,
                previousFaces + index,
                sizeof(SparseSurfaceFace)) == 0;
            if (nextSame) {
                break;
            }
            ++index;
        }
        runs.push_back({first, index - first});
    }
    return runs;
}

void SparseSurfaceCache::BeginFrame() {
    m_stats.unitFacesGeneratedLastUpdate = 0;
    m_stats.facesGeneratedLastUpdate = 0;
    m_stats.bricksUpdatedLastFrame = 0;
    m_stats.bricksPartiallyUpdatedLastFrame = 0;
    m_stats.facesRemovedByPartialUpdatesLastFrame = 0;
    m_stats.bricksRemovedLastFrame = 0;
    m_stats.emptyFastPathBricksLastFrame = 0;
}

namespace {

SparseSurfaceLocalRegion ExpandedSurfaceRegion(const SparseSurfaceLocalRegion& region) {
    SparseSurfaceLocalRegion expanded;
    expanded.minX = region.minX > 0 ? static_cast<uint8_t>(region.minX - 1u) : 0u;
    expanded.minY = region.minY > 0 ? static_cast<uint8_t>(region.minY - 1u) : 0u;
    expanded.minZ = region.minZ > 0 ? static_cast<uint8_t>(region.minZ - 1u) : 0u;
    expanded.maxX = std::min<uint8_t>(SPARSE_BRICK_SIZE - 1, static_cast<uint8_t>(region.maxX + 1u));
    expanded.maxY = std::min<uint8_t>(SPARSE_BRICK_SIZE - 1, static_cast<uint8_t>(region.maxY + 1u));
    expanded.maxZ = std::min<uint8_t>(SPARSE_BRICK_SIZE - 1, static_cast<uint8_t>(region.maxZ + 1u));
    return expanded;
}

bool FaceInsideRegion(const SparseSurfaceFace& face, const BrickCoord& coord, const SparseSurfaceLocalRegion& region) {
    const int32_t baseX = coord.x * SPARSE_BRICK_SIZE;
    const int32_t baseY = coord.y * SPARSE_BRICK_SIZE;
    const int32_t baseZ = coord.z * SPARSE_BRICK_SIZE;
    int32_t minX = face.worldX - baseX;
    int32_t minY = face.worldY - baseY;
    int32_t minZ = face.worldZ - baseZ;
    int32_t maxX = minX;
    int32_t maxY = minY;
    int32_t maxZ = minZ;
    const int32_t width = static_cast<int32_t>(SparseSurfacePayloadWidth(face.payload));
    const int32_t height = static_cast<int32_t>(SparseSurfacePayloadHeight(face.payload));
    switch (static_cast<SparseFaceDirection>(SparseSurfacePayloadDirection(face.payload))) {
    case SparseFaceDirection::NegX:
    case SparseFaceDirection::PosX:
        maxY = minY + height - 1;
        maxZ = minZ + width - 1;
        break;
    case SparseFaceDirection::NegY:
    case SparseFaceDirection::PosY:
        maxX = minX + width - 1;
        maxZ = minZ + height - 1;
        break;
    case SparseFaceDirection::NegZ:
    case SparseFaceDirection::PosZ:
    default:
        maxX = minX + width - 1;
        maxY = minY + height - 1;
        break;
    }
    return minX <= region.maxX && maxX >= region.minX &&
           minY <= region.maxY && maxY >= region.minY &&
           minZ <= region.maxZ && maxZ >= region.minZ;
}

bool IsFullSurfaceRegion(const SparseSurfaceLocalRegion& region) {
    return region.minX == 0 && region.minY == 0 && region.minZ == 0 &&
           region.maxX == SPARSE_BRICK_SIZE - 1 &&
           region.maxY == SPARSE_BRICK_SIZE - 1 &&
           region.maxZ == SPARSE_BRICK_SIZE - 1;
}

uint32_t SumSurfacePrimitiveArea(const std::vector<SparseSurfaceFace>& faces) {
    uint32_t area = 0;
    for (const SparseSurfaceFace& face : faces) {
        area += SparseSurfacePayloadWidth(face.payload) * SparseSurfacePayloadHeight(face.payload);
    }
    return area;
}

} // namespace

bool SparseSurfaceCache::UpdateBrick(
    const GeneratedSparseBrick& brick,
    const SparseNeighborSampler& neighborSampler)
{
    auto existing = m_facesByBrick.find(brick.coord);
    m_knownBricks.insert(brick.coord);
    if (HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        ++m_stats.emptyFastPathBricksLastFrame;
        if (existing == m_facesByBrick.end()) {
            m_unitFacesByBrick.erase(brick.coord);
            m_removedBrickSerials.erase(brick.coord);
            m_dirtyBrickSerials.erase(brick.coord);
            RefreshKnownStats();
            RefreshPendingGpuStats();
            return true;
        }

        m_stats.totalFaces -= static_cast<uint32_t>(existing->second.size());
        auto unitIt = m_unitFacesByBrick.find(brick.coord);
        if (unitIt != m_unitFacesByBrick.end()) {
            m_stats.totalUnitFaces -= unitIt->second;
            m_unitFacesByBrick.erase(unitIt);
        }
        m_facesByBrick.erase(existing);
        m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
        ++m_stats.bricksRemovedLastFrame;
        ++m_stats.serial;
        m_dirtyBrickSerials.erase(brick.coord);
        m_removedBrickSerials[brick.coord] = m_stats.serial;
        RefreshKnownStats();
        RefreshPendingGpuStats();
        return true;
    }

    auto extracted = SparseSurfaceExtractor::Extract(brick, neighborSampler);
    const uint32_t newFaceCount = static_cast<uint32_t>(extracted.faces.size());

    if (newFaceCount == 0u) {
        if (existing == m_facesByBrick.end()) {
            m_unitFacesByBrick.erase(brick.coord);
            m_removedBrickSerials.erase(brick.coord);
            m_dirtyBrickSerials.erase(brick.coord);
            RefreshKnownStats();
            RefreshPendingGpuStats();
            return true;
        }

        m_stats.totalFaces -= static_cast<uint32_t>(existing->second.size());
        auto unitIt = m_unitFacesByBrick.find(brick.coord);
        if (unitIt != m_unitFacesByBrick.end()) {
            m_stats.totalUnitFaces -= unitIt->second;
            m_unitFacesByBrick.erase(unitIt);
        }
        m_facesByBrick.erase(existing);
        m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
        ++m_stats.bricksRemovedLastFrame;
        ++m_stats.serial;
        m_dirtyBrickSerials.erase(brick.coord);
        m_removedBrickSerials[brick.coord] = m_stats.serial;
        RefreshKnownStats();
        RefreshPendingGpuStats();
        return true;
    }

    if (existing != m_facesByBrick.end()) {
        const uint32_t oldFaceCount = static_cast<uint32_t>(existing->second.size());
        const auto oldUnitIt = m_unitFacesByBrick.find(brick.coord);
        const uint32_t oldUnitFaceCount =
            oldUnitIt != m_unitFacesByBrick.end() ? oldUnitIt->second : SumSurfacePrimitiveArea(existing->second);
        m_stats.totalFaces -= oldFaceCount;
        m_stats.totalUnitFaces -= oldUnitFaceCount;
        existing->second = std::move(extracted.faces);
    } else {
        m_facesByBrick.emplace(brick.coord, std::move(extracted.faces));
        m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    }

    m_stats.totalFaces += newFaceCount;
    m_stats.totalUnitFaces += extracted.stats.exposedFaces;
    m_unitFacesByBrick[brick.coord] = extracted.stats.exposedFaces;
    m_stats.unitFacesGeneratedLastUpdate += extracted.stats.exposedFaces;
    m_stats.facesGeneratedLastUpdate += newFaceCount;
    ++m_stats.bricksUpdatedLastFrame;
    ++m_stats.serial;
    m_dirtyBrickSerials[brick.coord] = m_stats.serial;
    m_removedBrickSerials.erase(brick.coord);
    RefreshKnownStats();
    RefreshPendingGpuStats();
    return true;
}

bool SparseSurfaceCache::UpdateBrickRegion(
    const GeneratedSparseBrick& brick,
    const SparseSurfaceLocalRegion& region,
    const SparseNeighborSampler& neighborSampler)
{
    if (HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty) ||
        IsFullSurfaceRegion(region) ||
        m_facesByBrick.find(brick.coord) == m_facesByBrick.end()) {
        return UpdateBrick(brick, neighborSampler);
    }

    const SparseSurfaceLocalRegion expanded = ExpandedSurfaceRegion(region);
    auto extracted = SparseSurfaceExtractor::ExtractRegion(brick, expanded, neighborSampler);
    auto existing = m_facesByBrick.find(brick.coord);
    if (existing == m_facesByBrick.end()) {
        return UpdateBrick(brick, neighborSampler);
    }

    std::vector<SparseSurfaceFace>& faces = existing->second;
    for (const SparseSurfaceFace& face : faces) {
        const uint32_t area =
            SparseSurfacePayloadWidth(face.payload) *
            SparseSurfacePayloadHeight(face.payload);
        if (area > 1u && FaceInsideRegion(face, brick.coord, expanded)) {
            return UpdateBrick(brick, neighborSampler);
        }
    }

    const uint32_t oldFaceCount = static_cast<uint32_t>(faces.size());
    const auto oldUnitIt = m_unitFacesByBrick.find(brick.coord);
    const uint32_t oldUnitFaceCount =
        oldUnitIt != m_unitFacesByBrick.end() ? oldUnitIt->second : SumSurfacePrimitiveArea(faces);
    auto newEnd = std::remove_if(
        faces.begin(),
        faces.end(),
        [&](const SparseSurfaceFace& face) {
            return FaceInsideRegion(face, brick.coord, expanded);
        });
    const uint32_t removedFaces = static_cast<uint32_t>(std::distance(newEnd, faces.end()));
    faces.erase(newEnd, faces.end());
    faces.insert(
        faces.end(),
        std::make_move_iterator(extracted.faces.begin()),
        std::make_move_iterator(extracted.faces.end()));

    const uint32_t newFaceCount = static_cast<uint32_t>(faces.size());
    const uint32_t newUnitFaceCount = SumSurfacePrimitiveArea(faces);
    m_stats.totalFaces -= oldFaceCount;
    m_stats.totalFaces += newFaceCount;
    m_stats.totalUnitFaces -= oldUnitFaceCount;
    m_stats.totalUnitFaces += newUnitFaceCount;
    m_unitFacesByBrick[brick.coord] = newUnitFaceCount;
    m_stats.unitFacesGeneratedLastUpdate += extracted.stats.exposedFaces;
    m_stats.facesGeneratedLastUpdate +=
        newFaceCount >= oldFaceCount - removedFaces
            ? static_cast<uint32_t>(newFaceCount - (oldFaceCount - removedFaces))
            : 0u;
    m_stats.facesRemovedByPartialUpdatesLastFrame += removedFaces;
    ++m_stats.bricksUpdatedLastFrame;
    ++m_stats.bricksPartiallyUpdatedLastFrame;
    ++m_stats.serial;
    if (newFaceCount == 0u) {
        m_facesByBrick.erase(existing);
        m_unitFacesByBrick.erase(brick.coord);
        m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
        ++m_stats.bricksRemovedLastFrame;
        m_dirtyBrickSerials.erase(brick.coord);
        m_removedBrickSerials[brick.coord] = m_stats.serial;
    } else {
        m_dirtyBrickSerials[brick.coord] = m_stats.serial;
        m_removedBrickSerials.erase(brick.coord);
    }
    m_knownBricks.insert(brick.coord);
    RefreshKnownStats();
    RefreshPendingGpuStats();
    return true;
}

bool SparseSurfaceCache::RemoveBrick(const BrickCoord& coord) {
    auto existing = m_facesByBrick.find(coord);
    if (existing == m_facesByBrick.end()) {
        const size_t erasedKnown = m_knownBricks.erase(coord);
        if (erasedKnown == 0u) {
            return false;
        }
        m_unitFacesByBrick.erase(coord);
        m_dirtyBrickSerials.erase(coord);
        m_removedBrickSerials.erase(coord);
        RefreshKnownStats();
        RefreshPendingGpuStats();
        return true;
    }

    m_stats.totalFaces -= static_cast<uint32_t>(existing->second.size());
    auto unitIt = m_unitFacesByBrick.find(coord);
    if (unitIt != m_unitFacesByBrick.end()) {
        m_stats.totalUnitFaces -= unitIt->second;
        m_unitFacesByBrick.erase(unitIt);
    }
    m_facesByBrick.erase(existing);
    m_knownBricks.erase(coord);
    m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    ++m_stats.bricksRemovedLastFrame;
    ++m_stats.serial;
    m_dirtyBrickSerials.erase(coord);
    m_removedBrickSerials[coord] = m_stats.serial;
    RefreshKnownStats();
    RefreshPendingGpuStats();
    return true;
}

void SparseSurfaceCache::Clear() {
    m_facesByBrick.clear();
    m_unitFacesByBrick.clear();
    m_knownBricks.clear();
    m_dirtyBrickSerials.clear();
    m_removedBrickSerials.clear();
    m_stats = {};
}

const std::vector<SparseSurfaceFace>* SparseSurfaceCache::FindFaces(const BrickCoord& coord) const {
    auto found = m_facesByBrick.find(coord);
    return found == m_facesByBrick.end() ? nullptr : &found->second;
}

bool SparseSurfaceCache::IsSurfaceKnown(const BrickCoord& coord) const {
    return m_knownBricks.find(coord) != m_knownBricks.end();
}

bool SparseSurfaceCache::BuildContiguousFaceList(std::vector<SparseSurfaceFace>& outFaces) const {
    outFaces.clear();
    outFaces.reserve(m_stats.totalFaces);
    std::vector<BrickCoord> orderedCoords;
    orderedCoords.reserve(m_facesByBrick.size());
    for (const auto& item : m_facesByBrick) {
        orderedCoords.push_back(item.first);
    }
    std::sort(orderedCoords.begin(), orderedCoords.end(), BrickCoordLess);
    for (const BrickCoord& coord : orderedCoords) {
        auto found = m_facesByBrick.find(coord);
        if (found == m_facesByBrick.end()) {
            continue;
        }
        outFaces.insert(outFaces.end(), found->second.begin(), found->second.end());
    }
    return outFaces.size() == m_stats.totalFaces;
}

bool SparseSurfaceCache::BuildGpuSnapshot(
    SparseSurfaceGpuSnapshot& outSnapshot,
    const SparseSurfaceVisibilityConfig* visibility) const
{
    outSnapshot.faces.clear();
    outSnapshot.ranges.clear();
    outSnapshot.drawArgs.clear();
    outSnapshot.drawBatches.clear();
    outSnapshot.surfaceRecords.clear();
    outSnapshot.brickFaceCounts.clear();
    outSnapshot.dirtyBricks.clear();
    outSnapshot.removedBricks.clear();
    outSnapshot.candidateBricks = static_cast<uint32_t>(m_facesByBrick.size());
    outSnapshot.visibleBricks = 0;
    outSnapshot.culledBricks = 0;
    outSnapshot.lookaheadVisibleBricks = 0;
    outSnapshot.drawCommandCount = 0;
    uint32_t visibleFaceCapacity = 0;

    struct SnapshotCandidate {
        BrickCoord coord;
        const std::vector<SparseSurfaceFace>* faces = nullptr;
        bool visible = true;
    };
    std::vector<SnapshotCandidate> candidates;
    candidates.reserve(m_facesByBrick.size());
    for (const auto& item : m_facesByBrick) {
        bool isVisible = true;
        if (visibility && visibility->enabled) {
            bool usedLookahead = false;
            isVisible = BrickPassesVisibility(item.first, *visibility, &usedLookahead);
            if (isVisible) {
                ++outSnapshot.visibleBricks;
                outSnapshot.lookaheadVisibleBricks += usedLookahead ? 1u : 0u;
                visibleFaceCapacity += static_cast<uint32_t>(item.second.size());
            } else {
                ++outSnapshot.culledBricks;
            }
        } else {
            ++outSnapshot.visibleBricks;
            visibleFaceCapacity += static_cast<uint32_t>(item.second.size());
        }
        candidates.push_back({item.first, &item.second, isVisible});
    }

    outSnapshot.rangeCount = outSnapshot.visibleBricks;
    outSnapshot.rangeTableCapacity = NextPowerOfTwo(std::max(1u, outSnapshot.rangeCount * 2u));
    outSnapshot.ranges.resize(outSnapshot.rangeTableCapacity);
    outSnapshot.faces.reserve(visibleFaceCapacity);
    outSnapshot.drawArgs.reserve(outSnapshot.visibleBricks);
    outSnapshot.drawBatches.reserve(outSnapshot.visibleBricks);
    outSnapshot.surfaceRecords.reserve(outSnapshot.visibleBricks);
    outSnapshot.brickFaceCounts.reserve(outSnapshot.candidateBricks);
    outSnapshot.dirtyBricks.reserve(m_dirtyBrickSerials.size());
    outSnapshot.removedBricks.reserve(m_removedBrickSerials.size());

    std::vector<BrickCoord> dirtyCoords;
    dirtyCoords.reserve(m_dirtyBrickSerials.size());
    for (const auto& item : m_dirtyBrickSerials) {
        dirtyCoords.push_back(item.first);
    }
    std::sort(dirtyCoords.begin(), dirtyCoords.end(), BrickCoordLess);
    for (const BrickCoord& coord : dirtyCoords) {
        auto found = m_dirtyBrickSerials.find(coord);
        if (found != m_dirtyBrickSerials.end()) {
            outSnapshot.dirtyBricks.push_back({found->first, found->second});
        }
    }

    std::vector<BrickCoord> removedCoords;
    removedCoords.reserve(m_removedBrickSerials.size());
    for (const auto& item : m_removedBrickSerials) {
        removedCoords.push_back(item.first);
    }
    std::sort(removedCoords.begin(), removedCoords.end(), BrickCoordLess);
    for (const BrickCoord& coord : removedCoords) {
        auto found = m_removedBrickSerials.find(coord);
        if (found != m_removedBrickSerials.end()) {
            outSnapshot.removedBricks.push_back({found->first, found->second});
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const SnapshotCandidate& lhs, const SnapshotCandidate& rhs) {
            return BrickCoordLess(lhs.coord, rhs.coord);
        });

    for (const SnapshotCandidate& candidate : candidates) {
        if (!candidate.faces) {
            continue;
        }
        const BrickCoord& coord = candidate.coord;
        const auto& faces = *candidate.faces;
        outSnapshot.brickFaceCounts.push_back({
            coord,
            static_cast<uint32_t>(faces.size())
        });
        if (!candidate.visible) {
            continue;
        }

        const uint32_t faceCount = static_cast<uint32_t>(faces.size());
        SparseSurfaceBrickRange range;
        range.coord = coord;
        range.firstFace = static_cast<uint32_t>(outSnapshot.faces.size());
        range.faceCount = faceCount;
        const uint32_t directionMask = BuildSparseSurfaceDirectionMask(faces);
        range.flags = SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, directionMask);
        outSnapshot.faces.insert(outSnapshot.faces.end(), faces.begin(), faces.end());

        SparseSurfaceRecord record;
        record.coord = coord;
        record.firstFace = range.firstFace;
        record.faceCount = faceCount;
        record.flags = faceCount > 0u
            ? SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, directionMask)
            : 0u;
        record.generation = m_stats.serial;
        ComputeSparseSurfaceFaceBounds(
            faces.data(),
            faceCount,
            &record.minX,
            &record.minY,
            &record.minZ,
            &record.maxX,
            &record.maxY,
            &record.maxZ);
        outSnapshot.surfaceRecords.push_back(record);

        if (faceCount > 0u) {
            SparseSurfaceDrawArgs args;
            args.indexCountPerInstance = faceCount * 6u;
            args.instanceCount = 1u;
            args.startIndexLocation = range.firstFace * 6u;
            args.baseVertexLocation = 0;
            args.startInstanceLocation = 0u;
            outSnapshot.drawArgs.push_back(args);

            SparseSurfaceDrawBatch batch;
            batch.coord = coord;
            batch.firstFace = range.firstFace;
            batch.faceCount = faceCount;
            outSnapshot.drawBatches.push_back(batch);
        }

        const uint32_t mask = outSnapshot.rangeTableCapacity - 1u;
        uint32_t slot = HashBrickCoord32(coord) & mask;
        bool inserted = false;
        for (uint32_t probe = 0; probe < outSnapshot.rangeTableCapacity; ++probe) {
            SparseSurfaceBrickRange& tableEntry = outSnapshot.ranges[slot];
            if (tableEntry.flags == 0u) {
                tableEntry = range;
                inserted = true;
                break;
            }
            slot = (slot + 1u) & mask;
        }
        if (!inserted) {
            return false;
        }
    }

    outSnapshot.drawCommandCount = static_cast<uint32_t>(outSnapshot.drawArgs.size());
    outSnapshot.serial = m_stats.serial;
    return outSnapshot.faces.size() == visibleFaceCapacity &&
        outSnapshot.rangeCount == outSnapshot.visibleBricks &&
        outSnapshot.drawArgs.size() == outSnapshot.drawBatches.size() &&
        outSnapshot.surfaceRecords.size() == outSnapshot.visibleBricks &&
        outSnapshot.brickFaceCounts.size() == outSnapshot.candidateBricks;
}

void SparseSurfaceCache::MarkGpuUploadComplete(
    uint32_t completedSerial,
    const std::vector<BrickCoord>& uploadedPayloadBricks,
    const std::vector<BrickCoord>& removedBricks)
{
    for (const BrickCoord& coord : uploadedPayloadBricks) {
        auto dirtyIt = m_dirtyBrickSerials.find(coord);
        if (dirtyIt != m_dirtyBrickSerials.end() && dirtyIt->second <= completedSerial) {
            m_dirtyBrickSerials.erase(dirtyIt);
        }
    }

    for (const BrickCoord& coord : removedBricks) {
        auto removedIt = m_removedBrickSerials.find(coord);
        if (removedIt != m_removedBrickSerials.end() && removedIt->second <= completedSerial) {
            m_removedBrickSerials.erase(removedIt);
        }
    }

    RefreshPendingGpuStats();
}

bool SparseSurfaceCache::TryLookupRangeInSnapshot(
    const SparseSurfaceGpuSnapshot& snapshot,
    const BrickCoord& coord,
    SparseSurfaceBrickRange* outRange)
{
    if (snapshot.rangeTableCapacity == 0 ||
        snapshot.ranges.size() != snapshot.rangeTableCapacity ||
        (snapshot.rangeTableCapacity & (snapshot.rangeTableCapacity - 1u)) != 0u) {
        return false;
    }

    const uint32_t mask = snapshot.rangeTableCapacity - 1u;
    uint32_t slot = HashBrickCoord32(coord) & mask;
    for (uint32_t probe = 0; probe < snapshot.rangeTableCapacity; ++probe) {
        const SparseSurfaceBrickRange& entry = snapshot.ranges[slot];
        if (entry.flags == 0u) {
            return false;
        }
        if (entry.coord == coord) {
            if (outRange) {
                *outRange = entry;
            }
            return true;
        }
        slot = (slot + 1u) & mask;
    }
    return false;
}

void SparseSurfaceCache::RefreshPendingGpuStats() {
    m_stats.pendingGpuDirtyBricks = static_cast<uint32_t>(m_dirtyBrickSerials.size());
    m_stats.pendingGpuRemovedBricks = static_cast<uint32_t>(m_removedBrickSerials.size());
}

void SparseSurfaceCache::RefreshKnownStats() {
    m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    m_stats.knownBricks = static_cast<uint32_t>(m_knownBricks.size());
    m_stats.knownEmptySurfaceBricks =
        m_stats.knownBricks >= m_stats.cachedBricks
            ? m_stats.knownBricks - m_stats.cachedBricks
            : 0u;
}

} // namespace VENPOD::Simulation
