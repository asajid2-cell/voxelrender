#pragma once

// =============================================================================
// VENPOD Terrain Constants
// Single source of truth for terrain generation bounds
// =============================================================================

#include <cstdint>

namespace VENPOD::Simulation {

// ===== TERRAIN HEIGHT BOUNDS =====
// These MUST match the values in CS_GenerateChunk.hlsl
constexpr int32_t TERRAIN_MIN_Y = 4;     // Minimum terrain height (deep valley/ocean floor)
constexpr int32_t TERRAIN_MAX_Y = 124;   // Maximum terrain height within the 2-chunk render span
constexpr int32_t SEA_LEVEL_Y = 16;      // Baseline for rivers and basin lakes

// ===== CHUNK COORDINATES =====
// Terrain spans these chunk Y coordinates (64-voxel chunks)
constexpr int32_t TERRAIN_CHUNK_MIN_Y = TERRAIN_MIN_Y / 64;    // = 0
constexpr int32_t TERRAIN_CHUNK_MAX_Y = TERRAIN_MAX_Y / 64;    // = 1
constexpr int32_t TERRAIN_NUM_CHUNKS_Y = 2;  // Always load chunks Y=0 and Y=1

// Terrain uses the full Y=0-127 render span, so chunks Y=0 and Y=1 must
// both be generated even when the camera is far above or below sea level.

// ===== INFINITE CHUNK STREAMING DISTANCES =====
// The key to seamless infinite worlds: load chunks BEFORE they're visible,
// unload them AFTER they're out of view. This creates a "buffer zone" where
// loading/unloading happens invisibly to the player.
//
// Visual diagram (top-down view, player at center):
//
//     UNLOAD_DISTANCE (20 chunks) - chunks deleted here
//     |
//     |   LOAD_DISTANCE (16 chunks) - chunks start generating here
//     |   |
//     |   |   RENDER_DISTANCE (12 chunks) - what you can see
//     |   |   |
//     v   v   v
//   +-------------------------------------------+
//   |               UNLOAD ZONE                 |
//   |   +-----------------------------------+   |
//   |   |          LOAD BUFFER              |   |
//   |   |   +---------------------------+   |   |
//   |   |   |                           |   |   |
//   |   |   |      VISIBLE AREA         |   |   |
//   |   |   |        (player)           |   |   |
//   |   |   |                           |   |   |
//   |   |   +---------------------------+   |   |
//   |   |          LOAD BUFFER              |   |
//   |   +-----------------------------------+   |
//   |               UNLOAD ZONE                 |
//   +-------------------------------------------+
//
// When moving: chunks in LOAD BUFFER are already generated and waiting.
// You never see chunks pop in because they're ready before entering view.

constexpr int32_t CHUNK_SIZE_VOXELS = 64;

// RENDER_DISTANCE: What the GPU buffer can hold and render
// This determines the visible world size: (2*12+1) * 64 = 1600 voxels = 1.6km
constexpr int32_t RENDER_DISTANCE_HORIZONTAL = 12;  // +/-12 chunks visible

// LOAD_DISTANCE: Where we START loading chunks (must be > RENDER_DISTANCE)
// Chunks at this distance are loading in the background, ready when needed
// 4-chunk buffer means chunks have ~4 chunks of travel time to generate
constexpr int32_t LOAD_DISTANCE_HORIZONTAL = 16;  // +/-16 chunks = 33x33 = 2178 chunks loading

// UNLOAD_DISTANCE: Where we DELETE chunks (must be > LOAD_DISTANCE)
// Large gap prevents thrashing at boundaries when camera moves back and forth
// 4-chunk hysteresis prevents constant load/unload cycles
constexpr int32_t UNLOAD_DISTANCE_HORIZONTAL = 20;  // +/-20 chunks before deletion

// ===== RENDER BUFFER SIZE =====
// Buffer only needs to fit RENDER_DISTANCE (visible area)
// LOAD_DISTANCE chunks exist in memory but aren't copied to render buffer
constexpr int32_t RENDER_BUFFER_CHUNKS_X = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 25
constexpr int32_t RENDER_BUFFER_CHUNKS_Y = TERRAIN_NUM_CHUNKS_Y;  // 2
constexpr int32_t RENDER_BUFFER_CHUNKS_Z = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 25

constexpr int32_t RENDER_BUFFER_VOXELS_X = RENDER_BUFFER_CHUNKS_X * CHUNK_SIZE_VOXELS;  // 1600
constexpr int32_t RENDER_BUFFER_VOXELS_Y = RENDER_BUFFER_CHUNKS_Y * CHUNK_SIZE_VOXELS;  // 128
constexpr int32_t RENDER_BUFFER_VOXELS_Z = RENDER_BUFFER_CHUNKS_Z * CHUNK_SIZE_VOXELS;  // 1600

// ===== VRAM BUDGET =====
// With LOAD_DISTANCE=16: 33x33x2 = 2,178 chunks x 1 MB = ~2.2 GB for chunks
// With RENDER_DISTANCE=12: 25x25x2 = 1,250 chunks in render buffer
// Both fit comfortably in RTX 3070 Ti's 8GB VRAM

// ===== VALIDATION =====
static_assert(TERRAIN_MAX_Y < 128, "Terrain exceeds 2-chunk vertical span");
static_assert(TERRAIN_MIN_Y >= 0, "Terrain below world origin");
static_assert(SEA_LEVEL_Y > TERRAIN_MIN_Y && SEA_LEVEL_Y < TERRAIN_MAX_Y,
    "Sea level outside terrain bounds");

} // namespace VENPOD::Simulation
