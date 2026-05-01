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
//     UNLOAD_DISTANCE (18 chunks) - chunks deleted here
//     |
//     |   LOAD_DISTANCE (14 chunks) - chunks start generating here
//     |   |
//     |   |   RENDER_DISTANCE (9 chunks) - dense voxel window
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

// RENDER_DISTANCE: What the dense GPU voxel buffer can hold and render.
// Horizontal dense distance stays bounded so the renderer can afford a tall
// moving Y window. Longer distance should come from a future explicit far-LOD
// renderer, not from expanding this dense editable buffer.
constexpr int32_t RENDER_DISTANCE_HORIZONTAL = 9;  // +/-9 chunks dense/editable view
constexpr int32_t RENDER_DISTANCE_VERTICAL_BELOW = 4;
constexpr int32_t RENDER_DISTANCE_VERTICAL_ABOVE = 2;
constexpr int32_t RENDER_BUFFER_CHUNKS_Y =
    RENDER_DISTANCE_VERTICAL_BELOW + 1 + RENDER_DISTANCE_VERTICAL_ABOVE;  // 7

// LOAD_DISTANCE: Where we START loading chunks (must be > RENDER_DISTANCE)
// Chunks at this distance are loading in the background, ready when needed
// 4-chunk buffer means chunks have ~4 chunks of travel time to generate
constexpr int32_t LOAD_DISTANCE_HORIZONTAL = 14;  // +/-14 chunks load buffer
constexpr int32_t LOAD_DISTANCE_VERTICAL_BELOW = 5;
constexpr int32_t LOAD_DISTANCE_VERTICAL_ABOVE = 3;

// UNLOAD_DISTANCE: Where we DELETE chunks (must be > LOAD_DISTANCE)
// Large gap prevents thrashing at boundaries when camera moves back and forth
// 4-chunk hysteresis prevents constant load/unload cycles
constexpr int32_t UNLOAD_DISTANCE_HORIZONTAL = 18;  // +/-18 chunks before deletion
constexpr int32_t UNLOAD_DISTANCE_VERTICAL_BELOW = 6;
constexpr int32_t UNLOAD_DISTANCE_VERTICAL_ABOVE = 4;

// ===== RENDER BUFFER SIZE =====
// Buffer only needs to fit RENDER_DISTANCE (visible area)
// LOAD_DISTANCE chunks exist in memory but aren't copied to render buffer
constexpr int32_t RENDER_BUFFER_CHUNKS_X = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 19
constexpr int32_t RENDER_BUFFER_CHUNKS_Z = (RENDER_DISTANCE_HORIZONTAL * 2 + 1);  // 19

constexpr int32_t RENDER_BUFFER_VOXELS_X = RENDER_BUFFER_CHUNKS_X * CHUNK_SIZE_VOXELS;  // 1216
constexpr int32_t RENDER_BUFFER_VOXELS_Y = RENDER_BUFFER_CHUNKS_Y * CHUNK_SIZE_VOXELS;  // 448
constexpr int32_t RENDER_BUFFER_VOXELS_Z = RENDER_BUFFER_CHUNKS_Z * CHUNK_SIZE_VOXELS;  // 1216

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
// Render window: 19x7x19 = 2,527 chunks = 662M voxels per ping-pong buffer.
// This is a high default for the current dense editable representation. Startup
// must stream it incrementally and queue visible chunks first; do not flood the
// full load cube at launch.

// ===== VALIDATION =====
static_assert(RENDER_BUFFER_CHUNKS_Y == 7, "Expected a 7-chunk vertical render window");
static_assert(LOAD_DISTANCE_VERTICAL_BELOW >= RENDER_DISTANCE_VERTICAL_BELOW,
    "Vertical load window must cover the render window below the player");
static_assert(LOAD_DISTANCE_VERTICAL_ABOVE >= RENDER_DISTANCE_VERTICAL_ABOVE,
    "Vertical load window must cover the render window above the player");
static_assert(TERRAIN_CHUNK_MIN_Y < 0, "Vertical world should support negative terrain");
static_assert(SEA_LEVEL_Y > TERRAIN_MIN_Y && SEA_LEVEL_Y < TERRAIN_MAX_Y,
    "Sea level outside terrain bounds");

} // namespace VENPOD::Simulation
