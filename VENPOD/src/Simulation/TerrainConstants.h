#pragma once

// =============================================================================
// VENPOD Terrain Constants
// Single source of truth for terrain generation bounds
// =============================================================================

#include <cstdint>

namespace VENPOD::Simulation {

// ===== CONCEPTUAL TERRAIN HEIGHT BOUNDS =====
// These MUST match the values in CS_GenerateChunk.hlsl.
// The renderer does not allocate this entire height at once. Instead, it keeps
// a moving 3D render window around the player and streams chunk Y layers.
constexpr int32_t TERRAIN_MIN_Y = -332;   // Deep ravine/cavern floor target
constexpr int32_t TERRAIN_MAX_Y = 664;    // Needle spire / extreme peak target
constexpr int32_t SEA_LEVEL_Y = -48;      // Low basin water, not a global ground plane

// ===== CHUNK COORDINATES =====
// Floor-divide Y bounds into 64-voxel chunks. Negative Y matters here:
// -332 lives in chunk -6, 664 lives in chunk 10.
constexpr int32_t TERRAIN_CHUNK_MIN_Y = -6;
constexpr int32_t TERRAIN_CHUNK_MAX_Y = 10;
constexpr int32_t TERRAIN_TOTAL_CHUNKS_Y = TERRAIN_CHUNK_MAX_Y - TERRAIN_CHUNK_MIN_Y + 1;

// ===== INFINITE CHUNK STREAMING DISTANCES =====
// The key to seamless infinite worlds: load chunks BEFORE they're visible,
// unload them AFTER they're out of view. This creates a "buffer zone" where
// loading/unloading happens invisibly to the player.
//
// Visual diagram (top-down view, player at center):
//
//     UNLOAD_DISTANCE (10 chunks) - chunks deleted here
//     |
//     |   LOAD_DISTANCE (8 chunks) - chunks start generating here
//     |   |
//     |   |   RENDER_DISTANCE (5 chunks) - dense voxel window
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

// RENDER_DISTANCE: What the legacy dense GPU voxel buffer can hold and render.
// This is intentionally a temporary dev harness size while the sparse brick
// renderer becomes authoritative. The long-term target remains huge effective
// worlds through sparse pages/clipmaps, not brute-force dense allocation.
constexpr int32_t RENDER_DISTANCE_HORIZONTAL = 5;  // +/-5 chunks dense/editable view
constexpr int32_t RENDER_DISTANCE_VERTICAL_BELOW = 3;
constexpr int32_t RENDER_DISTANCE_VERTICAL_ABOVE = 1;
constexpr int32_t RENDER_BUFFER_CHUNKS_Y =
    RENDER_DISTANCE_VERTICAL_BELOW + 1 + RENDER_DISTANCE_VERTICAL_ABOVE;  // 5

// LOAD_DISTANCE: Where we START loading chunks (must be > RENDER_DISTANCE)
// Chunks at this distance are loading in the background, ready when needed
// 3-chunk buffer means chunks have travel time to generate without flooding VRAM.
constexpr int32_t LOAD_DISTANCE_HORIZONTAL = 8;  // +/-8 chunks load buffer
constexpr int32_t LOAD_DISTANCE_VERTICAL_BELOW = 4;
constexpr int32_t LOAD_DISTANCE_VERTICAL_ABOVE = 2;

// UNLOAD_DISTANCE: Where we DELETE chunks (must be > LOAD_DISTANCE)
// Large gap prevents thrashing at boundaries when camera moves back and forth
// 2-chunk hysteresis prevents constant load/unload cycles.
constexpr int32_t UNLOAD_DISTANCE_HORIZONTAL = 10;  // +/-10 chunks before deletion
constexpr int32_t UNLOAD_DISTANCE_VERTICAL_BELOW = 5;
constexpr int32_t UNLOAD_DISTANCE_VERTICAL_ABOVE = 3;

// ===== RENDER BUFFER SIZE =====
// Buffer only needs to fit RENDER_DISTANCE (visible area)
// LOAD_DISTANCE chunks exist in memory but aren't copied to render buffer
constexpr int32_t RENDER_BUFFER_CHUNKS_X = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 11
constexpr int32_t RENDER_BUFFER_CHUNKS_Z = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 11

constexpr int32_t RENDER_BUFFER_VOXELS_X = RENDER_BUFFER_CHUNKS_X * CHUNK_SIZE_VOXELS;  // 704
constexpr int32_t RENDER_BUFFER_VOXELS_Y = RENDER_BUFFER_CHUNKS_Y * CHUNK_SIZE_VOXELS;  // 320
constexpr int32_t RENDER_BUFFER_VOXELS_Z = RENDER_BUFFER_CHUNKS_Z * CHUNK_SIZE_VOXELS;  // 704

// Clamp a vertical streaming/render-window center so the requested below/above
// chunk span still intersects the generated terrain range. This must never be
// applied to player/camera world position; it is only a content streaming policy.
constexpr int32_t ClampVerticalChunkCenter(
    int32_t rawCenterY,
    int32_t chunksBelow,
    int32_t chunksAbove)
{
    const int32_t strictMinCenter = TERRAIN_CHUNK_MIN_Y + chunksBelow;
    const int32_t strictMaxCenter = TERRAIN_CHUNK_MAX_Y - chunksAbove;
    const int32_t minCenter = (strictMinCenter <= strictMaxCenter) ? strictMinCenter : TERRAIN_CHUNK_MIN_Y;
    const int32_t maxCenter = (strictMinCenter <= strictMaxCenter) ? strictMaxCenter : TERRAIN_CHUNK_MAX_Y;

    return rawCenterY < minCenter ? minCenter :
        (rawCenterY > maxCenter ? maxCenter : rawCenterY);
}

// ===== VRAM BUDGET =====
// Temporary dense render window: 11x5x11 = 605 chunks = 158M voxels per
// ping-pong buffer. The production scalability path is the sparse brick
// renderer, where only occupied/editable pages are resident.

// ===== VALIDATION =====
static_assert(RENDER_BUFFER_CHUNKS_Y == 5, "Expected a 5-chunk vertical dev render window");
static_assert(LOAD_DISTANCE_VERTICAL_BELOW >= RENDER_DISTANCE_VERTICAL_BELOW,
    "Vertical load window must cover the render window below the player");
static_assert(LOAD_DISTANCE_VERTICAL_ABOVE >= RENDER_DISTANCE_VERTICAL_ABOVE,
    "Vertical load window must cover the render window above the player");
static_assert(TERRAIN_CHUNK_MIN_Y < 0, "Vertical world should support negative terrain");
static_assert(SEA_LEVEL_Y > TERRAIN_MIN_Y && SEA_LEVEL_Y < TERRAIN_MAX_Y,
    "Sea level outside terrain bounds");

} // namespace VENPOD::Simulation
