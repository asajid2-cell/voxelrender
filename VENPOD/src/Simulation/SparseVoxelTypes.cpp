#include "SparseVoxelTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VENPOD::Simulation {

int32_t FloorDiv(int32_t value, int32_t divisor) {
    if (divisor <= 0) {
        throw std::invalid_argument("FloorDiv requires a positive divisor");
    }
    const int64_t wideValue = static_cast<int64_t>(value);
    const int64_t wideDivisor = static_cast<int64_t>(divisor);
    return static_cast<int32_t>(
        wideValue >= 0
            ? wideValue / wideDivisor
            : -(((-wideValue) + wideDivisor - 1) / wideDivisor));
}

uint32_t FloorMod(int32_t value, uint32_t divisor) {
    if (divisor == 0) {
        throw std::invalid_argument("FloorMod requires a non-zero divisor");
    }
    const int64_t signedDivisor = static_cast<int64_t>(divisor);
    int64_t result = static_cast<int64_t>(value) % signedDivisor;
    if (result < 0) {
        result += signedDivisor;
    }
    return static_cast<uint32_t>(result);
}

bool TryWorldVoxelFromBrickLocal(int32_t brickCoord, uint8_t local, int32_t* outWorldVoxel) {
    if (!outWorldVoxel) {
        return false;
    }
    const int64_t worldVoxel =
        static_cast<int64_t>(brickCoord) * static_cast<int64_t>(SPARSE_BRICK_SIZE) +
        static_cast<int64_t>(local);
    if (worldVoxel < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        worldVoxel > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *outWorldVoxel = static_cast<int32_t>(worldVoxel);
    return true;
}

namespace {

bool TryFloorToInt32(float value, int32_t* out) {
    if (!out || !std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(static_cast<double>(value));
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *out = static_cast<int32_t>(floored);
    return true;
}

bool TryCeilToInt32(float value, int32_t* out) {
    if (!out || !std::isfinite(value)) {
        return false;
    }
    const double ceiled = std::ceil(static_cast<double>(value));
    if (ceiled < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        ceiled > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *out = static_cast<int32_t>(ceiled);
    return true;
}

} // namespace

bool TryBuildSparseBrushVoxelBounds(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    float strength,
    SparseBrushVoxelBounds* outBounds)
{
    if (!outBounds ||
        !std::isfinite(worldPositionX) ||
        !std::isfinite(worldPositionY) ||
        !std::isfinite(worldPositionZ) ||
        !std::isfinite(radius) ||
        !std::isfinite(strength) ||
        radius <= 0.0f) {
        return false;
    }

    const float boundedRadius = std::min(radius, SPARSE_MAX_BRUSH_RADIUS);
    const float boundedStrength = std::clamp(strength, 0.0f, 1.0f);
    const int64_t radiusCeil =
        static_cast<int64_t>(std::ceil(static_cast<double>(boundedRadius))) + 2;

    int32_t centerX = 0;
    int32_t centerY = 0;
    int32_t centerZ = 0;
    int32_t ceilX = 0;
    int32_t ceilY = 0;
    int32_t ceilZ = 0;
    if (!TryFloorToInt32(worldPositionX, &centerX) ||
        !TryFloorToInt32(worldPositionY, &centerY) ||
        !TryFloorToInt32(worldPositionZ, &centerZ) ||
        !TryCeilToInt32(worldPositionX, &ceilX) ||
        !TryCeilToInt32(worldPositionY, &ceilY) ||
        !TryCeilToInt32(worldPositionZ, &ceilZ)) {
        return false;
    }

    const int64_t startX = static_cast<int64_t>(centerX) - radiusCeil;
    const int64_t startY = static_cast<int64_t>(centerY) - radiusCeil;
    const int64_t startZ = static_cast<int64_t>(centerZ) - radiusCeil;
    const int64_t endX = static_cast<int64_t>(ceilX) + radiusCeil + 1;
    const int64_t endY = static_cast<int64_t>(ceilY) + radiusCeil + 1;
    const int64_t endZ = static_cast<int64_t>(ceilZ) + radiusCeil + 1;
    const int64_t minInt = static_cast<int64_t>(std::numeric_limits<int32_t>::min());
    const int64_t maxInt = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    if (startX < minInt || startY < minInt || startZ < minInt ||
        endX > maxInt || endY > maxInt || endZ > maxInt ||
        endX <= startX || endY <= startY || endZ <= startZ) {
        return false;
    }

    const uint64_t countX = static_cast<uint64_t>(endX - startX);
    const uint64_t countY = static_cast<uint64_t>(endY - startY);
    const uint64_t countZ = static_cast<uint64_t>(endZ - startZ);
    if (countX > SPARSE_MAX_BRUSH_VOXELS ||
        countY > SPARSE_MAX_BRUSH_VOXELS / countX ||
        countZ > SPARSE_MAX_BRUSH_VOXELS / (countX * countY)) {
        return false;
    }

    outBounds->startX = static_cast<int32_t>(startX);
    outBounds->startY = static_cast<int32_t>(startY);
    outBounds->startZ = static_cast<int32_t>(startZ);
    outBounds->endX = static_cast<int32_t>(endX);
    outBounds->endY = static_cast<int32_t>(endY);
    outBounds->endZ = static_cast<int32_t>(endZ);
    outBounds->radius = boundedRadius;
    outBounds->strength = boundedStrength;
    return true;
}

BrickCoord BrickCoord::FromWorldVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) {
    return {
        FloorDiv(worldX, SPARSE_BRICK_SIZE),
        FloorDiv(worldY, SPARSE_BRICK_SIZE),
        FloorDiv(worldZ, SPARSE_BRICK_SIZE)
    };
}

LocalVoxelCoord LocalVoxelFromWorld(int32_t worldX, int32_t worldY, int32_t worldZ) {
    return {
        static_cast<uint8_t>(FloorMod(worldX, SPARSE_BRICK_SIZE)),
        static_cast<uint8_t>(FloorMod(worldY, SPARSE_BRICK_SIZE)),
        static_cast<uint8_t>(FloorMod(worldZ, SPARSE_BRICK_SIZE))
    };
}

uint16_t LocalVoxelIndex(LocalVoxelCoord local) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(local.x) +
        static_cast<uint16_t>(local.y) * SPARSE_BRICK_SIZE +
        static_cast<uint16_t>(local.z) * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE);
}

LocalVoxelCoord LocalVoxelFromIndex(uint16_t index) {
    return {
        static_cast<uint8_t>(index % SPARSE_BRICK_SIZE),
        static_cast<uint8_t>((index / SPARSE_BRICK_SIZE) % SPARSE_BRICK_SIZE),
        static_cast<uint8_t>(index / (SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE))
    };
}

const char* ToString(BrickLifecycleState state) {
    switch (state) {
        case BrickLifecycleState::Missing: return "Missing";
        case BrickLifecycleState::Requested: return "Requested";
        case BrickLifecycleState::GeneratingCPU: return "GeneratingCPU";
        case BrickLifecycleState::GeneratedCPU: return "GeneratedCPU";
        case BrickLifecycleState::UploadQueued: return "UploadQueued";
        case BrickLifecycleState::UploadingGPU: return "UploadingGPU";
        case BrickLifecycleState::Resident: return "Resident";
        case BrickLifecycleState::DirtyCPU: return "DirtyCPU";
        case BrickLifecycleState::DirtyGPU: return "DirtyGPU";
        case BrickLifecycleState::EvictQueued: return "EvictQueued";
        case BrickLifecycleState::Evicted: return "Evicted";
        default: return "Unknown";
    }
}

bool IsValidLifecycleTransition(BrickLifecycleState from, BrickLifecycleState to) {
    if (from == to) {
        return true;
    }

    switch (from) {
        case BrickLifecycleState::Missing:
            return to == BrickLifecycleState::Requested;
        case BrickLifecycleState::Requested:
            return to == BrickLifecycleState::GeneratingCPU ||
                   to == BrickLifecycleState::GeneratedCPU ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::GeneratingCPU:
            return to == BrickLifecycleState::GeneratedCPU ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::GeneratedCPU:
            return to == BrickLifecycleState::UploadQueued ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::UploadQueued:
            return to == BrickLifecycleState::UploadingGPU ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::UploadingGPU:
            return to == BrickLifecycleState::Resident ||
                   to == BrickLifecycleState::UploadQueued ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::Resident:
            return to == BrickLifecycleState::DirtyCPU ||
                   to == BrickLifecycleState::DirtyGPU ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::DirtyCPU:
            return to == BrickLifecycleState::UploadQueued ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::DirtyGPU:
            return to == BrickLifecycleState::UploadQueued ||
                   to == BrickLifecycleState::Resident ||
                   to == BrickLifecycleState::EvictQueued;
        case BrickLifecycleState::EvictQueued:
            return to == BrickLifecycleState::Evicted;
        case BrickLifecycleState::Evicted:
            return to == BrickLifecycleState::Requested ||
                   to == BrickLifecycleState::Missing;
        default:
            return false;
    }
}

uint32_t HashBrickCoord32(const BrickCoord& coord) {
    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(coord.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.y)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.z)) * 16777619u;
    return hash;
}

size_t BrickCoordHash::operator()(const BrickCoord& coord) const noexcept {
    return static_cast<size_t>(HashBrickCoord32(coord));
}

} // namespace VENPOD::Simulation
