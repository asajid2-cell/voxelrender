#pragma once

// =============================================================================
// VENPOD Terrain Constants
// Single source of truth for terrain generation bounds
// =============================================================================

#include <cstdint>

namespace VENPOD::Simulation {

// ===== TERRAIN HEIGHT BOUNDS =====
// These MUST match the values in CS_GenerateChunk.hlsl
constexpr int32_t TERRAIN_MIN_Y = 5;    // Minimum terrain height (bedrock/ocean floor)
constexpr int32_t TERRAIN_MAX_Y = 60;   // Maximum terrain height (mountain peaks)
constexpr int32_t SEA_LEVEL_Y = 40;     // Sea level for water/air boundary

// ===== CHUNK COORDINATES =====
// Terrain spans these chunk Y coordinates (64-voxel chunks)
constexpr int32_t TERRAIN_CHUNK_MIN_Y = TERRAIN_MIN_Y / 64;    // = 0
constexpr int32_t TERRAIN_CHUNK_MAX_Y = TERRAIN_MAX_Y / 64;    // = 0 (60/64 = 0)
constexpr int32_t TERRAIN_NUM_CHUNKS_Y = 2;  // Always load chunks Y=0 and Y=1

// Note: TERRAIN_MAX_Y = 60, which is in chunk Y=0 (0-63)
// But we need chunk Y=1 (64-127) for vertical space above terrain
// Total terrain vertical span: Y=0-127 (chunks 0 and 1)

// ===== RENDER BUFFER SIZE =====
// Based on render distance and terrain chunk count
// RTX 3070 Ti (8GB) MAXED OUT - 25×25×2 = 1,250 chunks = ~1.25GB VRAM for chunks
constexpr int32_t RENDER_DISTANCE_HORIZONTAL = 12;  // ±12 chunks = 25×25 grid = 1.6km view!
constexpr int32_t RENDER_BUFFER_CHUNKS_X = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 25
constexpr int32_t RENDER_BUFFER_CHUNKS_Y = TERRAIN_NUM_CHUNKS_Y;  // 2
constexpr int32_t RENDER_BUFFER_CHUNKS_Z = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 25

constexpr int32_t CHUNK_SIZE_VOXELS = 64;
constexpr int32_t RENDER_BUFFER_VOXELS_X = RENDER_BUFFER_CHUNKS_X * CHUNK_SIZE_VOXELS;  // 1600
constexpr int32_t RENDER_BUFFER_VOXELS_Y = RENDER_BUFFER_CHUNKS_Y * CHUNK_SIZE_VOXELS;  // 128
constexpr int32_t RENDER_BUFFER_VOXELS_Z = RENDER_BUFFER_CHUNKS_Z * CHUNK_SIZE_VOXELS;  // 1600

// ===== VALIDATION =====
static_assert(TERRAIN_MAX_Y < 128, "Terrain exceeds 2-chunk vertical span");
static_assert(TERRAIN_MIN_Y >= 0, "Terrain below world origin");
static_assert(SEA_LEVEL_Y > TERRAIN_MIN_Y && SEA_LEVEL_Y < TERRAIN_MAX_Y,
    "Sea level outside terrain bounds");

} // namespace VENPOD::Simulation
