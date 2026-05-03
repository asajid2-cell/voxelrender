#include "SparseSurfaceCache.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace VENPOD::Simulation {

namespace {

constexpr uint32_t kSurfaceRangeValid = 1u;

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
    const SparseSurfaceVisibilityConfig& visibility)
{
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
    if (distanceSq > maxDistance * maxDistance) {
        return false;
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

} // namespace

void SparseSurfaceCache::BeginFrame() {
    m_stats.facesGeneratedLastUpdate = 0;
    m_stats.bricksUpdatedLastFrame = 0;
    m_stats.bricksRemovedLastFrame = 0;
}

bool SparseSurfaceCache::UpdateBrick(
    const GeneratedSparseBrick& brick,
    const SparseNeighborSampler& neighborSampler)
{
    auto extracted = SparseSurfaceExtractor::Extract(brick, neighborSampler);
    const uint32_t newFaceCount = static_cast<uint32_t>(extracted.faces.size());

    auto existing = m_facesByBrick.find(brick.coord);
    if (existing != m_facesByBrick.end()) {
        const uint32_t oldFaceCount = static_cast<uint32_t>(existing->second.size());
        m_stats.totalFaces -= oldFaceCount;
        existing->second = std::move(extracted.faces);
    } else {
        m_facesByBrick.emplace(brick.coord, std::move(extracted.faces));
        m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    }

    m_stats.totalFaces += newFaceCount;
    m_stats.facesGeneratedLastUpdate += newFaceCount;
    ++m_stats.bricksUpdatedLastFrame;
    ++m_stats.serial;
    return true;
}

bool SparseSurfaceCache::RemoveBrick(const BrickCoord& coord) {
    auto existing = m_facesByBrick.find(coord);
    if (existing == m_facesByBrick.end()) {
        return false;
    }

    m_stats.totalFaces -= static_cast<uint32_t>(existing->second.size());
    m_facesByBrick.erase(existing);
    m_stats.cachedBricks = static_cast<uint32_t>(m_facesByBrick.size());
    ++m_stats.bricksRemovedLastFrame;
    ++m_stats.serial;
    return true;
}

void SparseSurfaceCache::Clear() {
    m_facesByBrick.clear();
    m_stats = {};
}

const std::vector<SparseSurfaceFace>* SparseSurfaceCache::FindFaces(const BrickCoord& coord) const {
    auto found = m_facesByBrick.find(coord);
    return found == m_facesByBrick.end() ? nullptr : &found->second;
}

bool SparseSurfaceCache::BuildContiguousFaceList(std::vector<SparseSurfaceFace>& outFaces) const {
    outFaces.clear();
    outFaces.reserve(m_stats.totalFaces);
    for (const auto& item : m_facesByBrick) {
        outFaces.insert(outFaces.end(), item.second.begin(), item.second.end());
    }
    return outFaces.size() == m_stats.totalFaces;
}

bool SparseSurfaceCache::BuildGpuSnapshot(
    SparseSurfaceGpuSnapshot& outSnapshot,
    const SparseSurfaceVisibilityConfig* visibility) const
{
    outSnapshot.faces.clear();
    outSnapshot.ranges.clear();
    outSnapshot.candidateBricks = static_cast<uint32_t>(m_facesByBrick.size());
    outSnapshot.visibleBricks = 0;
    outSnapshot.culledBricks = 0;
    uint32_t visibleFaceCapacity = 0;
    if (visibility && visibility->enabled) {
        for (const auto& item : m_facesByBrick) {
            if (BrickPassesVisibility(item.first, *visibility)) {
                ++outSnapshot.visibleBricks;
                visibleFaceCapacity += static_cast<uint32_t>(item.second.size());
            } else {
                ++outSnapshot.culledBricks;
            }
        }
    } else {
        outSnapshot.visibleBricks = outSnapshot.candidateBricks;
        visibleFaceCapacity = m_stats.totalFaces;
    }

    outSnapshot.rangeCount = outSnapshot.visibleBricks;
    outSnapshot.rangeTableCapacity = NextPowerOfTwo(std::max(1u, outSnapshot.rangeCount * 2u));
    outSnapshot.ranges.resize(outSnapshot.rangeTableCapacity);
    outSnapshot.faces.reserve(visibleFaceCapacity);

    for (const auto& item : m_facesByBrick) {
        if (visibility && visibility->enabled && !BrickPassesVisibility(item.first, *visibility)) {
            continue;
        }

        SparseSurfaceBrickRange range;
        range.coord = item.first;
        range.firstFace = static_cast<uint32_t>(outSnapshot.faces.size());
        range.faceCount = static_cast<uint32_t>(item.second.size());
        range.flags = kSurfaceRangeValid;
        outSnapshot.faces.insert(outSnapshot.faces.end(), item.second.begin(), item.second.end());

        const uint32_t mask = outSnapshot.rangeTableCapacity - 1u;
        uint32_t slot = HashBrickCoord32(item.first) & mask;
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

    outSnapshot.serial = m_stats.serial;
    return outSnapshot.faces.size() == visibleFaceCapacity &&
        outSnapshot.rangeCount == outSnapshot.visibleBricks;
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

} // namespace VENPOD::Simulation
