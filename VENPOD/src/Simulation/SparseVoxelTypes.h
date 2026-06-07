#pragma once

#include <cstdint>
#include <functional>

namespace VENPOD::Simulation {

constexpr int32_t SPARSE_BRICK_SIZE = 16;
constexpr int32_t SPARSE_BRICK_VOXEL_COUNT =
    SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
constexpr uint32_t INVALID_BRICK_PAGE = 0xFFFFFFFFu;
constexpr float SPARSE_MAX_BRUSH_RADIUS = 32.0f;
constexpr uint64_t SPARSE_MAX_BRUSH_VOXELS = 512000;

int32_t FloorDiv(int32_t value, int32_t divisor);
uint32_t FloorMod(int32_t value, uint32_t divisor);
bool TryWorldVoxelFromBrickLocal(int32_t brickCoord, uint8_t local, int32_t* outWorldVoxel);

struct SparseBrushVoxelBounds {
    int32_t startX = 0;
    int32_t startY = 0;
    int32_t startZ = 0;
    int32_t endX = 0;
    int32_t endY = 0;
    int32_t endZ = 0;
    float radius = 0.0f;
    float strength = 1.0f;
};

bool TryBuildSparseBrushVoxelBounds(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    float strength,
    SparseBrushVoxelBounds* outBounds);

struct BrickCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const BrickCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }

    bool operator!=(const BrickCoord& other) const {
        return !(*this == other);
    }

    bool operator<(const BrickCoord& other) const {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }

    static BrickCoord FromWorldVoxel(int32_t worldX, int32_t worldY, int32_t worldZ);
};

struct LocalVoxelCoord {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t z = 0;

    bool operator==(const LocalVoxelCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

LocalVoxelCoord LocalVoxelFromWorld(int32_t worldX, int32_t worldY, int32_t worldZ);
uint16_t LocalVoxelIndex(LocalVoxelCoord local);
LocalVoxelCoord LocalVoxelFromIndex(uint16_t index);

enum class BrickResidencyFlags : uint32_t {
    None = 0,
    Empty = 1u << 0,
    Solid = 1u << 1,
    Homogeneous = 1u << 2,
    HasWater = 1u << 3,
    HasEdits = 1u << 4,
    PhysicsLive = 1u << 5
};

enum class BrickLifecycleState : uint8_t {
    Missing,
    Requested,
    GeneratingCPU,
    GeneratedCPU,
    UploadQueued,
    UploadingGPU,
    Resident,
    DirtyCPU,
    DirtyGPU,
    EvictQueued,
    Evicted
};

enum class SparseResidencyClass : uint8_t {
    Speculative = 0,
    Visible = 1,
    Collision = 2,
    Edited = 3
};

enum class SparseStreamingLane : uint8_t {
    Cache = 0,
    Prefetch = 1,
    Repair = 2,
    Visible = 3,
    PublicCritical = 4
};

const char* ToString(BrickLifecycleState state);
bool IsValidLifecycleTransition(BrickLifecycleState from, BrickLifecycleState to);

struct BrickPageEntry {
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    uint32_t flags = 0;
    uint32_t occupancyWord0 = 0;
    uint32_t occupancyWord1 = 0;
};

struct BrickResidentRecord {
    BrickCoord coord;
    uint32_t pageIndex = INVALID_BRICK_PAGE;
    uint32_t generation = 0;
    BrickLifecycleState state = BrickLifecycleState::Missing;
    uint32_t lastTouchedFrame = 0;
    uint32_t lastSpeculativeFrame = 0;
    uint32_t lastVisibleFrame = 0;
    uint32_t lastCollisionFrame = 0;
    uint32_t lastEditedFrame = 0;
    uint32_t lastUploadedFrame = 0;
    int32_t queuePriority = 0;
    SparseResidencyClass residencyClass = SparseResidencyClass::Speculative;
    SparseStreamingLane streamingLane = SparseStreamingLane::Cache;
    bool dirtyCpu = false;
    bool dirtyGpu = false;
    bool gpuPageTablePublished = false;
    bool hasPersistentEdits = false;
    bool physicsActive = false;
};

struct BrickCoordHash {
    size_t operator()(const BrickCoord& coord) const noexcept;
};

uint32_t HashBrickCoord32(const BrickCoord& coord);

} // namespace VENPOD::Simulation
