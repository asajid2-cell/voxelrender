#include "SparseVoxelTypes.h"

#include <stdexcept>

namespace VENPOD::Simulation {

int32_t FloorDiv(int32_t value, int32_t divisor) {
    if (divisor <= 0) {
        throw std::invalid_argument("FloorDiv requires a positive divisor");
    }
    return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
}

uint32_t FloorMod(int32_t value, uint32_t divisor) {
    if (divisor == 0) {
        throw std::invalid_argument("FloorMod requires a non-zero divisor");
    }
    const int32_t signedDivisor = static_cast<int32_t>(divisor);
    int32_t result = value % signedDivisor;
    if (result < 0) {
        result += signedDivisor;
    }
    return static_cast<uint32_t>(result);
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
