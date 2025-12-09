#pragma once

// =============================================================================
// VENPOD Chunk Coordinate - Identifies chunks in infinite 3D grid
// Used as hash map key for dynamic chunk loading/unloading
// =============================================================================

#include <cstdint>
#include <functional>

namespace VENPOD::Simulation {

// Chunk coordinate in infinite grid space
// Example: ChunkCoord{0,0,0} = world origin, ChunkCoord{1,0,0} = +64 voxels in X
struct ChunkCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    // Constructors
    ChunkCoord() = default;
    ChunkCoord(int32_t x_, int32_t y_, int32_t z_) : x(x_), y(y_), z(z_) {}

    // Equality comparison (required for hash map key)
    bool operator==(const ChunkCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }

    bool operator!=(const ChunkCoord& other) const {
        return !(*this == other);
    }

    // Less-than operator (for std::map if needed)
    bool operator<(const ChunkCoord& other) const {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }

    // Calculate world position of chunk origin (in voxel coordinates)
    // For CHUNK_SIZE=64: ChunkCoord{1,0,0} → world position {64,0,0}
    inline void GetWorldOrigin(int32_t& outX, int32_t& outY, int32_t& outZ, uint32_t chunkSize) const {
        outX = x * static_cast<int32_t>(chunkSize);
        outY = y * static_cast<int32_t>(chunkSize);
        outZ = z * static_cast<int32_t>(chunkSize);
    }

    // Hash function for std::unordered_map
    // Uses FNV-1a hash algorithm for good distribution
    size_t Hash() const {
        size_t hash = 2166136261u;
        hash = (hash ^ static_cast<size_t>(x)) * 16777619u;
        hash = (hash ^ static_cast<size_t>(y)) * 16777619u;
        hash = (hash ^ static_cast<size_t>(z)) * 16777619u;
        return hash;
    }

    // Convert world voxel position to chunk coordinate
    // For CHUNK_SIZE=64: voxel {65,0,0} → ChunkCoord{1,0,0}
    // For negative coords: voxel {-1,0,0} → ChunkCoord{-1,0,0} (NOT chunk 0!)
    static ChunkCoord FromWorldPosition(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t chunkSize) {
        // Floor division that correctly handles negative coordinates
        auto FloorDiv = [](int32_t a, int32_t b) -> int32_t {
            return (a >= 0) ? (a / b) : ((a - b + 1) / b);
        };

        int32_t chunkSizeSigned = static_cast<int32_t>(chunkSize);
        return ChunkCoord{
            FloorDiv(worldX, chunkSizeSigned),
            FloorDiv(worldY, chunkSizeSigned),
            FloorDiv(worldZ, chunkSizeSigned)
        };
    }

    // Calculate distance squared to another chunk (for unloading distant chunks)
    int32_t DistanceSquared(const ChunkCoord& other) const {
        int32_t dx = x - other.x;
        int32_t dy = y - other.y;
        int32_t dz = z - other.z;
        return dx * dx + dy * dy + dz * dz;
    }
};

} // namespace VENPOD::Simulation

// Hash specialization for std::unordered_map<ChunkCoord, ...>
namespace std {
    template<>
    struct hash<VENPOD::Simulation::ChunkCoord> {
        size_t operator()(const VENPOD::Simulation::ChunkCoord& coord) const noexcept {
            return coord.Hash();
        }
    };
}
