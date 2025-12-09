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
#define STATE_LIFE_MASK  0x0F

// Frame constants passed from CPU (max 64 DWORDs total for root signature)
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

    // Brush preview (8 DWORDs)
    float4   brushPosition;      // xyz = position, w = radius
    float4   brushParams;        // x = material, y = shape, z = hasValidPosition, w = unused
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

#endif // SHARED_TYPES_HLSLI
