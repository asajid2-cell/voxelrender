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

// State flags (in high byte, bits 31-24)
#define STATE_IS_STATIC  0x80  // Bit 31: Frozen/immovable
#define STATE_IS_IGNITED 0x40  // Bit 30: On fire (for oil napalm)
#define STATE_HAS_MOVED  0x20  // Bit 29: Moved this frame
#define STATE_SETTLED    0x10  // Bit 28: Liquid at equilibrium (skip physics)
#define STATE_LIFE_MASK  0x0F  // Bits 27-24: Life counter (0-15)

// =============================================================================
// VELOCITY ENCODING (bits 23-16)
// =============================================================================
// New layout for smooth fluid physics:
//   Bits 23-21: X velocity (signed 3-bit: -4 to +3)
//   Bits 20-18: Z velocity (signed 3-bit: -4 to +3)
//   Bits 17-16: Y velocity (signed 2-bit: -2 to +1, mostly for gravity)
//
// This allows liquids to have momentum and move smoothly instead of
// teleporting 1 voxel per frame.

#define VEL_X_SHIFT 21
#define VEL_X_MASK  0x07  // 3 bits
#define VEL_Z_SHIFT 18
#define VEL_Z_MASK  0x07  // 3 bits
#define VEL_Y_SHIFT 16
#define VEL_Y_MASK  0x03  // 2 bits

// Velocity byte mask (all velocity bits)
#define VEL_BYTE_MASK 0x00FF0000

// Helper: Extract signed velocity from voxel
// Returns int3 with x,y,z velocities
#ifdef __HLSL_VERSION
inline int3 GetVelocity(uint voxel) {
    uint velByte = (voxel >> 16) & 0xFF;

    // Extract and sign-extend each component
    int vx = (int)((velByte >> 5) & VEL_X_MASK);
    int vz = (int)((velByte >> 2) & VEL_Z_MASK);
    int vy = (int)(velByte & VEL_Y_MASK);

    // Sign extend: if high bit set, subtract range to make negative
    if (vx >= 4) vx -= 8;  // 4,5,6,7 -> -4,-3,-2,-1
    if (vz >= 4) vz -= 8;
    if (vy >= 2) vy -= 4;  // 2,3 -> -2,-1

    return int3(vx, vy, vz);
}

// Helper: Pack velocity into voxel
// vel components should be clamped to valid ranges before calling
inline uint SetVelocity(uint voxel, int3 vel) {
    // Clamp to valid ranges
    int vx = clamp(vel.x, -4, 3);
    int vy = clamp(vel.y, -2, 1);
    int vz = clamp(vel.z, -4, 3);

    // Convert to unsigned (wrap negatives)
    uint ux = (uint)(vx & 0x7);  // -4 to 3 -> 4,5,6,7,0,1,2,3
    uint uy = (uint)(vy & 0x3);  // -2 to 1 -> 2,3,0,1
    uint uz = (uint)(vz & 0x7);

    // Pack into velocity byte
    uint velByte = (ux << 5) | (uz << 2) | uy;

    // Clear old velocity, set new
    return (voxel & ~VEL_BYTE_MASK) | (velByte << 16);
}

// Helper: Check if voxel has significant velocity
inline bool HasVelocity(uint voxel) {
    int3 vel = GetVelocity(voxel);
    return (abs(vel.x) > 0 || abs(vel.y) > 0 || abs(vel.z) > 0);
}

// Helper: Apply drag to velocity (reduces by ~12.5% per frame)
inline int3 ApplyDrag(int3 vel) {
    // Horizontal drag (water resistance)
    vel.x = (vel.x * 7) / 8;
    vel.z = (vel.z * 7) / 8;
    // No drag on Y (gravity handles it)
    return vel;
}
#endif

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

    // Region origin for infinite world (4 DWORDs) - CRITICAL FOR CAMERA MOVEMENT!
    // This is the world-space origin of the render buffer's (0,0,0) voxel
    // Shader MUST subtract this from world coords to get buffer-local coords
    float4   regionOrigin;       // xyz = world origin, w = unused

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
