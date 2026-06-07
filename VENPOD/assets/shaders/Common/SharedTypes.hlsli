// =============================================================================
// VENPOD Shared Types - C++/HLSL Compatible Structures
// =============================================================================

#ifndef SHARED_TYPES_HLSLI
#define SHARED_TYPES_HLSLI

// Voxel bit layout (32-bit uint):
//   Bits 31-24: State (IsStatic, IsIgnited, HasMoved, Life[4])
//   Bits 23-16: Velocity (Y_Vel[3], X_Speed[3], Heading[2])
//   Bits 15-08: Variant (visual noise)
//   Bits 07-00: Material ID (0-255)

// Material IDs
#define MAT_AIR     0
#define MAT_SAND    1
#define MAT_WATER   2
#define MAT_STONE   3
#define MAT_DIRT    4
#define MAT_WOOD    5
#define MAT_FIRE    6
#define MAT_LAVA    7
#define MAT_ICE     8
#define MAT_OIL     9
#define MAT_GLASS      10
#define MAT_SMOKE      11
#define MAT_ACID       12
#define MAT_HONEY      13
#define MAT_CONCRETE   14
#define MAT_GUNPOWDER  15
#define MAT_CRYSTAL    16
#define MAT_STEAM      17
#define MAT_BEDROCK    255

// State flags (in high byte)
#define STATE_IS_STATIC  0x80
#define STATE_IS_IGNITED 0x40
#define STATE_HAS_MOVED  0x20
#define STATE_VISUAL_SURFACE 0x10
#define STATE_LIFE_MASK  0x0F

// Frame constants passed from CPU through a CBV. This used to be root
// constants; keep C++'s FrameConstantsCpu mirror in Renderer.cpp synchronized
// with this exact layout.
struct FrameConstants {
    // Camera (20 floats = 20 DWORDs)
    float4   cameraPosition;     // xyz = position, w = fov
    float4   cameraForward;      // xyz = forward direction, w = aspectRatio
    float4   cameraRight;        // xyz = right direction, w = unused
    float4   cameraUp;           // xyz = up direction, w = unused
    float4   sunDirection;       // xyz = direction, w = intensity

    // Grid dimensions (4 DWORDs)
    uint     gridSizeX;
    uint     gridSizeY;
    uint     gridSizeZ;
    float    voxelScale;

    // Viewport (4 DWORDs)
    float    viewportWidth;
    float    viewportHeight;
    uint     frameIndex;
    uint     debugMode;

    // Region origin for infinite world (4 DWORDs) - CRITICAL FOR CAMERA MOVEMENT!
    // This is the world-space origin of the render buffer's (0,0,0) voxel
    // Shader MUST subtract this from world coords to get buffer-local coords
    float4   regionOrigin;       // xyz = world origin, w = unused

    // Brush preview (8 DWORDs)
    float4   brushPosition;      // xyz = position, w = radius
    float4   brushParams;        // x = material, y = shape, z = hasValidPosition, w = unused

    // Third-person block character (4 DWORDs)
    float4   characterPosition;  // xyz = feet position, w = visible flag

    // Sparse visual far field (4 DWORDs)
    // x = enabled, y = page count, z = node count, w = page size
    float4   farFieldParams;

    // Adaptive render budget (4 DWORDs)
    // x = dense max ray distance, y = dense max DDA steps, z = far-field quality, w = overall quality
    float4   renderBudgetParams;

    // Far-field page index metadata (4 DWORDs)
    // x = page radius, y = page-index side length, z = root min Y, w = ownership stats flag
    float4   farFieldGridParams;

    // Sparse near-field metadata (4 DWORDs)
    // x = enabled, y = max brick pages, z = page-table capacity, w = flags
    // flags bit 0 = sparse-only, no dense fallback for missing pages
    // flags bit 1 = surface-authoritative near field; fullscreen pass renders background only
    // flags bit 2 = mid voxel clipmap is resident and may own background rays
    // flags bit 4 = voxel terrain only; disable procedural height/water fallback terrain
    float4   sparseNearParams;

      // Mid-field procedural clipmap metadata (4 DWORDs)
      // x = enabled, y = start distance, z = end distance, w = minimum cell size
      float4   midFieldParams;

      // Sparse extracted surface hierarchy metadata (4 DWORDs)
      // x = enabled, y = face count, z = live range count, w = range hash-table capacity
      float4   surfaceParams;

      // Sparse near-field ownership metadata (4 DWORDs)
      // xyz = world-space ownership center, w = ownership radius in voxels.
      // Background/mid/far layers may not draw before the ray exits this
      // volume when the sparse surface layer is authoritative.
      float4   nearOwnershipParams;

      // Background ownership transition metadata (4 DWORDs)
      // x = mid clipmap start, y = far handoff distance, z = mid clipmap end,
      // w = metadata valid flag. CPU policy owns these distances; shaders may
      // clamp per-ray starts later but should not invent transition fractions.
      float4   backgroundOwnershipParams;

      // Mid-layer residency coverage metadata (4 DWORDs)
      // x = height tile coverage ratio, y = voxel brick coverage ratio,
      // z = resident height tile count, w = resident voxel brick count.
      // This lets shader ownership decisions distinguish a deliberately empty
      // layer from a layer that is still streaming.
      float4   midResidencyParams;

      // Far-layer ownership metadata (4 DWORDs)
      // x = SVO ready flag, y = SVO upload coverage ratio, z = SVO page
      // coverage ratio, w = effective far quality. Far layers may provide
      // continuity only when this metadata says they are resident enough.
      float4   farOwnershipParams;

      // Exact near-field contract metadata (4 DWORDs)
      // x = distance from camera inside which background/proxy layers may not
      // satisfy terrain/water ownership. Missing exact sparse voxels must stay
      // visible to diagnostics instead of being hidden by fallback heightfields.
      float4   exactNearParams;      // x = exact sparse voxel distance, y = world seed bits, z/w = mid voxel handoff coverage/worst ring

      // Public exact-surface drawing contract (4 DWORDs)
      // x = maximum distance where the public frame may draw exact sparse
      // surface hits. This is intentionally separate from the wider sparse
      // ownership/feedback radius so lower LOD can remain coherent until exact
      // terrain is ready for a deliberate foreground promotion.
      float4   surfaceRasterParams;
  };

// Chunk control structure for sparse optimization
struct ChunkControl {
    uint isActive;       // 0 or 1
    uint sleepTimer;     // Frames since last movement
    uint particleCount;  // Debugging metric
    uint padding;        // Align to 16 bytes
};

// Brush input for painting
struct BrushInput {
    float4 rayOrigin;
    float4 rayDirection;
    float4 params;       // x = radius, y = material, z = strength, w = mode
};

// Sparse local physics packet ABI. Keep this layout matched with
// Simulation::SparsePhysicsWorkPacket on the CPU side.
#define SPARSE_PHYSICS_MATERIAL_SAND  0x1
#define SPARSE_PHYSICS_MATERIAL_WATER 0x2
#define SPARSE_PHYSICS_MATERIAL_LAVA  0x4

struct SparsePhysicsWorkPacket {
    int3  brickCoord;
    uint  packedRegionMin; // local x/y/z packed into low three bytes
    uint  packedRegionMax; // local x/y/z packed into low three bytes
    uint  materialMask;
    uint  priority;        // 0 = warm, 1 = hot
    uint  generation;
    uint  expectedPageIndex;
    uint  expectedPageGeneration;
};

struct SparseEditDelta {
    int3  brickCoord;
    uint  packedLocal;     // local x/y/z packed into low three bytes
    uint  voxel;
    uint  revision;
};

struct SparseEditDeltaRange {
    int3  brickCoord;
    uint  firstDelta;
    uint  deltaCount;
    uint  latestRevision;
};

struct BrickPageEntry {
    int3  coord;
    uint  pageIndex;
    uint  generation;
    uint  flags;
    uint  occupancyWord0;
    uint  occupancyWord1;
};

struct SparsePhysicsPacketResult {
    int3  brickCoord;
    uint  packetIndex;
    int3  destinationBrickCoord;
    uint  destinationFlags;
    uint  generation;
    uint  materialMask;
    uint  checksum;
    uint  status;          // 1 = consumed by validation shader
    uint  expectedPageIndex;
    uint  expectedPageGeneration;
    uint  packedSourceLocal;
    uint  packedDestinationLocal;
    uint  sourceVoxel;
    uint  destinationVoxel;
    uint  sourceRevision;
    uint  destinationRevision;
};

#endif // SHARED_TYPES_HLSLI
