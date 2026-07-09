#include "SparseSurfaceCache.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
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

bool TryBrickBaseWorld(const BrickCoord& coord, int32_t* outBaseX, int32_t* outBaseY, int32_t* outBaseZ) {
    int32_t baseX = 0;
    int32_t baseY = 0;
    int32_t baseZ = 0;
    if (!TryWorldVoxelFromBrickLocal(coord.x, 0, &baseX) ||
        !TryWorldVoxelFromBrickLocal(coord.y, 0, &baseY) ||
        !TryWorldVoxelFromBrickLocal(coord.z, 0, &baseZ)) {
        return false;
    }
    if (outBaseX) *outBaseX = baseX;
    if (outBaseY) *outBaseY = baseY;
    if (outBaseZ) *outBaseZ = baseZ;
    return true;
}

int32_t SaturatingAddInt32(int32_t value, int32_t delta) {
    const int64_t sum = static_cast<int64_t>(value) + static_cast<int64_t>(delta);
    if (sum < static_cast<int64_t>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (sum > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(sum);
}

uint32_t SaturatingAddUint32(uint32_t lhs, uint32_t rhs) {
    const uint64_t sum = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs);
    if (sum > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(sum);
}

uint32_t SaturatingSizeToUint32(size_t value) {
    if (value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(value);
}

size_t SparseSurfaceClusterReserveCount(size_t recordCount, uint32_t recordsPerCluster) {
    const size_t stride = static_cast<size_t>(std::max(1u, recordsPerCluster));
    return (recordCount / stride) + ((recordCount % stride) != 0u ? 1u : 0u);
}

bool SparseSurfaceExtentExceedsLimit(int32_t minValue, int32_t maxValue, uint32_t maxExtent) {
    if (maxExtent == 0u || maxValue <= minValue) {
        return false;
    }
    const uint64_t extent =
        static_cast<uint64_t>(static_cast<int64_t>(maxValue) - static_cast<int64_t>(minValue));
    return extent > static_cast<uint64_t>(maxExtent);
}

uint32_t SparseSurfaceMortonAxisBits(int32_t value) {
    constexpr uint32_t kBitsPerAxis = 21u;
    constexpr uint64_t kAxisMask = (uint64_t{1} << kBitsPerAxis) - 1u;
    constexpr int64_t kSignedBias = int64_t{1} << (kBitsPerAxis - 1u);
    const int64_t biased = static_cast<int64_t>(value) + kSignedBias;
    return static_cast<uint32_t>(static_cast<uint64_t>(biased) & kAxisMask);
}

bool IsFinite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
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

    int32_t baseX = 0;
    int32_t baseY = 0;
    int32_t baseZ = 0;
    if (!TryBrickBaseWorld(coord, &baseX, &baseY, &baseZ)) {
        return false;
    }
    const float centerX = static_cast<float>(baseX) + kBrickHalf;
    const float centerY = static_cast<float>(baseY) + kBrickHalf;
    const float centerZ = static_cast<float>(baseZ) + kBrickHalf;
    if (!IsFinite3(visibility.cameraX, visibility.cameraY, visibility.cameraZ)) {
        return true;
    }
    const float dx = centerX - visibility.cameraX;
    const float dy = centerY - visibility.cameraY;
    const float dz = centerZ - visibility.cameraZ;
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSq)) {
        return true;
    }
    const float padding = std::clamp(FiniteOr(visibility.padding, 0.0f), 0.0f, 8192.0f);
    const float maxDistanceInput = std::clamp(FiniteOr(visibility.maxDistance, 2500.0f), kBrickSize, 1000000.0f);
    const float maxDistance = std::max(kBrickSize, maxDistanceInput + padding + kBrickRadius);
    const bool insideCurrentSphere = distanceSq <= maxDistance * maxDistance;
    bool insideLookaheadSphere = false;
    if (!insideCurrentSphere && visibility.useMotionLookahead) {
        if (!IsFinite3(
                visibility.lookaheadCameraX,
                visibility.lookaheadCameraY,
                visibility.lookaheadCameraZ)) {
            return true;
        }
        const float lookaheadDx = centerX - visibility.lookaheadCameraX;
        const float lookaheadDy = centerY - visibility.lookaheadCameraY;
        const float lookaheadDz = centerZ - visibility.lookaheadCameraZ;
        const float lookaheadDistanceSq =
            lookaheadDx * lookaheadDx +
            lookaheadDy * lookaheadDy +
            lookaheadDz * lookaheadDz;
        if (!std::isfinite(lookaheadDistanceSq)) {
            return true;
        }
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
    if (!IsFinite3(visibility.forwardX, visibility.forwardY, visibility.forwardZ) ||
        !IsFinite3(visibility.rightX, visibility.rightY, visibility.rightZ) ||
        !IsFinite3(visibility.upX, visibility.upY, visibility.upZ)) {
        return true;
    }

    const float z =
        dx * visibility.forwardX +
        dy * visibility.forwardY +
        dz * visibility.forwardZ;
    if (!std::isfinite(z)) {
        return true;
    }
    if (z < -(padding + kBrickRadius)) {
        return false;
    }

    const float fovY = std::clamp(FiniteOr(visibility.fovYRadians, 1.04719755f), 0.1f, 3.0f);
    const float aspect = std::clamp(FiniteOr(visibility.aspectRatio, 1.7777778f), 0.1f, 8.0f);
    const float tanHalfY = std::tan(fovY * 0.5f);
    const float tanHalfX = tanHalfY * aspect;
    const float x =
        dx * visibility.rightX +
        dy * visibility.rightY +
        dz * visibility.rightZ;
    const float y =
        dx * visibility.upX +
        dy * visibility.upY +
        dz * visibility.upZ;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(tanHalfX) || !std::isfinite(tanHalfY)) {
        return true;
    }
    const float zForFrustum = std::max(0.0f, z);
    const float xLimit = zForFrustum * tanHalfX + padding + kBrickRadius;
    const float yLimit = zForFrustum * tanHalfY + padding + kBrickRadius;
    if (!std::isfinite(xLimit) || !std::isfinite(yLimit)) {
        return true;
    }
    return std::abs(x) <= xLimit && std::abs(y) <= yLimit;
}

bool HasHorizontalNeighborCoverage(
    const BrickCoord& coord,
    const std::unordered_set<BrickCoord, BrickCoordHash>& knownBricks)
{
    const BrickCoord neighbors[] = {
        {SaturatingAddInt32(coord.x, -1), coord.y, coord.z},
        {SaturatingAddInt32(coord.x, 1), coord.y, coord.z},
        {coord.x, coord.y, SaturatingAddInt32(coord.z, -1)},
        {coord.x, coord.y, SaturatingAddInt32(coord.z, 1)},
    };
    for (const BrickCoord& neighbor : neighbors) {
        if (neighbor == coord || knownBricks.find(neighbor) == knownBricks.end()) {
            return false;
        }
    }
    return true;
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

uint32_t BuildSparseSurfaceDirectionMask(const SparseSurfaceFace* faces, uint32_t faceCount) {
    uint32_t mask = 0;
    if (!faces) {
        return 0u;
    }
    for (uint32_t index = 0; index < faceCount; ++index) {
        mask |= SparseSurfaceDirectionBit(SparseSurfacePayloadDirection(faces[index].payload));
    }
    return mask & kSparseSurfaceDirectionMaskBits;
}

uint32_t BuildSparseSurfaceDirectionMask(const std::vector<SparseSurfaceFace>& faces) {
    const uint32_t faceCount = SaturatingSizeToUint32(faces.size());
    const SparseSurfaceFace* faceData = faces.empty() ? nullptr : faces.data();
    const uint32_t mask = BuildSparseSurfaceDirectionMask(faceData, faceCount);
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
        int32_t faceMaxX = SaturatingAddInt32(face.worldX, 1);
        int32_t faceMaxY = SaturatingAddInt32(face.worldY, 1);
        int32_t faceMaxZ = SaturatingAddInt32(face.worldZ, 1);
        switch (static_cast<SparseFaceDirection>(SparseSurfacePayloadDirection(face.payload))) {
        case SparseFaceDirection::NegX:
        case SparseFaceDirection::PosX:
            faceMaxY = SaturatingAddInt32(face.worldY, height);
            faceMaxZ = SaturatingAddInt32(face.worldZ, width);
            break;
        case SparseFaceDirection::NegY:
        case SparseFaceDirection::PosY:
            faceMaxX = SaturatingAddInt32(face.worldX, width);
            faceMaxZ = SaturatingAddInt32(face.worldZ, height);
            break;
        case SparseFaceDirection::NegZ:
        case SparseFaceDirection::PosZ:
        default:
            faceMaxX = SaturatingAddInt32(face.worldX, width);
            faceMaxY = SaturatingAddInt32(face.worldY, height);
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
    const uint32_t x = SparseSurfaceMortonAxisBits(coord.x);
    const uint32_t y = SparseSurfaceMortonAxisBits(coord.y);
    const uint32_t z = SparseSurfaceMortonAxisBits(coord.z);

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
    const size_t reserveCount = SparseSurfaceClusterReserveCount(records.size(), recordsPerCluster);
    if (reserveCount <= clusters.max_size()) {
        clusters.reserve(reserveCount);
    }

    auto flushCluster = [&](size_t first, size_t count) {
        SparseSurfaceClusterRecord cluster;
        cluster.firstRecord = SaturatingSizeToUint32(first);
        cluster.recordCount = SaturatingSizeToUint32(count);
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
            cluster.faceCount = SaturatingAddUint32(cluster.faceCount, record.faceCount);
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
                (SparseSurfaceExtentExceedsLimit(nextMinX, nextMaxX, maxClusterExtentVoxels) ||
                 SparseSurfaceExtentExceedsLimit(nextMinY, nextMaxY, maxClusterExtentVoxels) ||
                 SparseSurfaceExtentExceedsLimit(nextMinZ, nextMaxZ, maxClusterExtentVoxels));
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
    int32_t baseX = 0;
    int32_t baseY = 0;
    int32_t baseZ = 0;
    if (!TryBrickBaseWorld(coord, &baseX, &baseY, &baseZ)) {
        return false;
    }
    const int64_t minX64 = static_cast<int64_t>(face.worldX) - static_cast<int64_t>(baseX);
    const int64_t minY64 = static_cast<int64_t>(face.worldY) - static_cast<int64_t>(baseY);
    const int64_t minZ64 = static_cast<int64_t>(face.worldZ) - static_cast<int64_t>(baseZ);
    if (minX64 < std::numeric_limits<int32_t>::min() ||
        minX64 > std::numeric_limits<int32_t>::max() ||
        minY64 < std::numeric_limits<int32_t>::min() ||
        minY64 > std::numeric_limits<int32_t>::max() ||
        minZ64 < std::numeric_limits<int32_t>::min() ||
        minZ64 > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    int32_t minX = static_cast<int32_t>(minX64);
    int32_t minY = static_cast<int32_t>(minY64);
    int32_t minZ = static_cast<int32_t>(minZ64);
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
    if (HasResidencyFlag(brick.flags, BrickResidencyFlags::Empty)) {
        SparseSurfaceExtractionResult emptyExtraction;
        return UpdateBrickWithExtractedFaces(brick, std::move(emptyExtraction));
    }

    auto extracted = SparseSurfaceExtractor::Extract(brick, neighborSampler);
    return UpdateBrickWithExtractedFaces(brick, std::move(extracted));
}

bool SparseSurfaceCache::UpdateBrickWithExtractedFaces(
    const GeneratedSparseBrick& brick,
    SparseSurfaceExtractionResult&& extracted)
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

bool SparseSurfaceCache::MarkKnownEmptySurface(const BrickCoord& coord) {
    m_knownBricks.insert(coord);
    auto existing = m_facesByBrick.find(coord);
    if (existing == m_facesByBrick.end()) {
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
    m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    ++m_stats.bricksRemovedLastFrame;
    ++m_stats.serial;
    m_dirtyBrickSerials.erase(coord);
    m_removedBrickSerials[coord] = m_stats.serial;
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
    const SparseSurfaceVisibilityConfig* visibility,
    bool includeSurfaceRecords,
    bool includeFacePayloads) const
{
    outSnapshot.faces.clear();
    outSnapshot.ranges.clear();
    outSnapshot.drawArgs.clear();
    outSnapshot.drawBatches.clear();
    outSnapshot.surfaceRecords.clear();
    outSnapshot.brickFaceCounts.clear();
    outSnapshot.dirtyBricks.clear();
    outSnapshot.removedBricks.clear();
    outSnapshot.deferredDirtyBricks = 0;
    outSnapshot.candidateBricks = static_cast<uint32_t>(m_facesByBrick.size());
    outSnapshot.visibleBricks = 0;
    outSnapshot.visibleFaceCount = 0;
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
            if (isVisible && visibility->requireHorizontalNeighborCoverage) {
                isVisible = HasHorizontalNeighborCoverage(item.first, m_knownBricks);
                if (!isVisible) {
                    usedLookahead = false;
                }
            }
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
    outSnapshot.visibleFaceCount = visibleFaceCapacity;
    outSnapshot.rangeTableCapacity = NextPowerOfTwo(std::max(1u, outSnapshot.rangeCount * 2u));
    outSnapshot.ranges.resize(outSnapshot.rangeTableCapacity);
    if (includeFacePayloads) {
        outSnapshot.faces.reserve(visibleFaceCapacity);
    }
    outSnapshot.drawArgs.reserve(outSnapshot.visibleBricks);
    outSnapshot.drawBatches.reserve(outSnapshot.visibleBricks);
    if (includeSurfaceRecords) {
        outSnapshot.surfaceRecords.reserve(outSnapshot.visibleBricks);
    }
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

    uint32_t nextLogicalFirstFace = 0;
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
        range.firstFace = includeFacePayloads
            ? static_cast<uint32_t>(outSnapshot.faces.size())
            : nextLogicalFirstFace;
        range.faceCount = faceCount;
        const uint32_t directionMask = BuildSparseSurfaceDirectionMask(faces);
        range.flags = SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, directionMask);
        const auto dirtySerialIt = m_dirtyBrickSerials.find(coord);
        const bool debugDirtySurface = dirtySerialIt != m_dirtyBrickSerials.end();
        if (includeFacePayloads) {
            outSnapshot.faces.insert(outSnapshot.faces.end(), faces.begin(), faces.end());
        }

        if (includeSurfaceRecords) {
            SparseSurfaceRecord record;
            record.coord = coord;
            record.firstFace = range.firstFace;
            record.faceCount = faceCount;
            record.flags = faceCount > 0u
                ? SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, directionMask)
                : 0u;
            if (debugDirtySurface && record.flags != 0u) {
                record.flags |= kSparseSurfaceDebugWavePixel;
            }
            record.generation = debugDirtySurface ? dirtySerialIt->second : m_stats.serial;
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
        }

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
            batch.faces = faces.data();
            batch.firstFace = range.firstFace;
            batch.faceCount = faceCount;
            outSnapshot.drawBatches.push_back(batch);
        }
        nextLogicalFirstFace += faceCount;

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
    return (!includeFacePayloads || outSnapshot.faces.size() == visibleFaceCapacity) &&
        outSnapshot.visibleFaceCount == visibleFaceCapacity &&
        outSnapshot.rangeCount == outSnapshot.visibleBricks &&
        outSnapshot.drawArgs.size() == outSnapshot.drawBatches.size() &&
        (!includeSurfaceRecords || outSnapshot.surfaceRecords.size() == outSnapshot.visibleBricks) &&
        outSnapshot.brickFaceCounts.size() == outSnapshot.candidateBricks;
}

bool SparseSurfaceCache::BuildDirtyPayloadGpuSnapshot(
    SparseSurfaceGpuSnapshot& outSnapshot,
    uint32_t maxDirtyBricks) const
{
    outSnapshot.faces.clear();
    outSnapshot.ranges.clear();
    outSnapshot.drawArgs.clear();
    outSnapshot.drawBatches.clear();
    outSnapshot.surfaceRecords.clear();
    outSnapshot.brickFaceCounts.clear();
    outSnapshot.dirtyBricks.clear();
    outSnapshot.removedBricks.clear();
    outSnapshot.deferredDirtyBricks = 0;
    outSnapshot.rangeCount = 0;
    outSnapshot.rangeTableCapacity = 0;
    outSnapshot.drawCommandCount = 0;
    outSnapshot.serial = m_stats.serial;
    outSnapshot.candidateBricks = static_cast<uint32_t>(m_facesByBrick.size());
    outSnapshot.visibleBricks = 0;
    outSnapshot.visibleFaceCount = 0;
    outSnapshot.culledBricks = 0;
    outSnapshot.lookaheadVisibleBricks = 0;

    if (m_dirtyBrickSerials.empty() && m_removedBrickSerials.empty()) {
        return false;
    }

    std::vector<BrickCoord> dirtyCoords;
    dirtyCoords.reserve(m_dirtyBrickSerials.size());
    for (const auto& item : m_dirtyBrickSerials) {
        dirtyCoords.push_back(item.first);
    }
    std::sort(dirtyCoords.begin(), dirtyCoords.end(), BrickCoordLess);
    if (maxDirtyBricks > 0u && dirtyCoords.size() > maxDirtyBricks) {
        outSnapshot.deferredDirtyBricks =
            static_cast<uint32_t>(dirtyCoords.size() - maxDirtyBricks);
        dirtyCoords.resize(maxDirtyBricks);
    }

    outSnapshot.dirtyBricks.reserve(dirtyCoords.size());
    outSnapshot.drawBatches.reserve(dirtyCoords.size());
    for (const BrickCoord& coord : dirtyCoords) {
        auto dirtyIt = m_dirtyBrickSerials.find(coord);
        if (dirtyIt == m_dirtyBrickSerials.end()) {
            continue;
        }
        auto facesIt = m_facesByBrick.find(coord);
        if (facesIt == m_facesByBrick.end()) {
            outSnapshot.dirtyBricks.push_back({coord, dirtyIt->second});
            continue;
        }
        const auto& faces = facesIt->second;
        outSnapshot.dirtyBricks.push_back({coord, dirtyIt->second});
        outSnapshot.visibleBricks += 1u;
        outSnapshot.visibleFaceCount += static_cast<uint32_t>(faces.size());
        if (!faces.empty()) {
            SparseSurfaceDrawBatch batch;
            batch.coord = coord;
            batch.faces = faces.data();
            batch.firstFace = 0u;
            batch.faceCount = static_cast<uint32_t>(faces.size());
            outSnapshot.drawBatches.push_back(batch);
        }
    }
    std::vector<BrickCoord> removedCoords;
    removedCoords.reserve(m_removedBrickSerials.size());
    for (const auto& item : m_removedBrickSerials) {
        removedCoords.push_back(item.first);
    }
    std::sort(removedCoords.begin(), removedCoords.end(), BrickCoordLess);
    outSnapshot.removedBricks.reserve(removedCoords.size());
    for (const BrickCoord& coord : removedCoords) {
        auto removedIt = m_removedBrickSerials.find(coord);
        if (removedIt != m_removedBrickSerials.end()) {
            outSnapshot.removedBricks.push_back({coord, removedIt->second});
        }
    }
    outSnapshot.drawCommandCount = static_cast<uint32_t>(outSnapshot.drawBatches.size());
    outSnapshot.rangeCount = outSnapshot.visibleBricks;
    return !outSnapshot.dirtyBricks.empty() || !outSnapshot.removedBricks.empty();
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
        if (entry.coord == coord && (entry.flags & kSparseSurfaceRangeValid) != 0u) {
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
