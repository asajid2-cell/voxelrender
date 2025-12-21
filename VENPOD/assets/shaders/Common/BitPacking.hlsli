// =============================================================================
// VENPOD Bit Packing - Voxel data pack/unpack utilities
// =============================================================================

#ifndef BIT_PACKING_HLSLI
#define BIT_PACKING_HLSLI

#include "SharedTypes.hlsli"

// Voxel bit layout (32-bit uint):
//   Bits 31-24: State (IsStatic, IsIgnited, HasMoved, Life[4])
//   Bits 23-16: Velocity (Y_Vel[3], X_Speed[3], Heading[2])
//   Bits 15-08: Variant (visual noise)
//   Bits 07-00: Material ID (0-255)

// Extract material ID from voxel data
uint GetMaterial(uint voxel) {
    return voxel & 0xFF;
}

// Extract variant from voxel data
uint GetVariant(uint voxel) {
    return (voxel >> 8) & 0xFF;
}

// Extract velocity byte from voxel data (raw packed bits)
uint GetVelocityByte(uint voxel) {
    return (voxel >> 16) & 0xFF;
}

// Extract state byte from voxel data
uint GetState(uint voxel) {
    return (voxel >> 24) & 0xFF;
}

// Set material ID in voxel data
uint SetMaterial(uint voxel, uint material) {
    return (voxel & 0xFFFFFF00) | (material & 0xFF);
}

// Set variant in voxel data
uint SetVariant(uint voxel, uint variant) {
    return (voxel & 0xFFFF00FF) | ((variant & 0xFF) << 8);
}

// Set velocity in voxel data (expects packed byte)
uint SetVelocityByte(uint voxel, uint velocity) {
    return (voxel & 0xFF00FFFF) | ((velocity & 0xFF) << 16);
}

// Set state in voxel data
uint SetState(uint voxel, uint state) {
    return (voxel & 0x00FFFFFF) | ((state & 0xFF) << 24);
}

// Create a voxel from components
uint PackVoxel(uint material, uint variant, uint velocity, uint state) {
    return (material & 0xFF) |
           ((variant & 0xFF) << 8) |
           ((velocity & 0xFF) << 16) |
           ((state & 0xFF) << 24);
}

// State flag helpers
bool IsStatic(uint voxel) {
    return (GetState(voxel) & STATE_IS_STATIC) != 0;
}

bool IsIgnited(uint voxel) {
    return (GetState(voxel) & STATE_IS_IGNITED) != 0;
}

bool HasMoved(uint voxel) {
    return (GetState(voxel) & STATE_HAS_MOVED) != 0;
}

uint GetLife(uint voxel) {
    return GetState(voxel) & STATE_LIFE_MASK;
}

uint SetHasMoved(uint voxel, bool moved) {
    uint state = GetState(voxel);
    if (moved) {
        state |= STATE_HAS_MOVED;
    } else {
        state &= ~STATE_HAS_MOVED;
    }
    return SetState(voxel, state);
}

// Velocity component extraction (3+3+2 = 8 bits)
// Y_Vel: bits 7-5 (signed, -4 to +3)
// X_Speed: bits 4-2 (unsigned, 0 to 7)
// Heading: bits 1-0 (0=N, 1=E, 2=S, 3=W)
int GetYVelocity(uint voxel) {
    int vel = (int)((GetVelocityByte(voxel) >> 5) & 0x7);
    // Sign extend from 3 bits
    if (vel >= 4) vel -= 8;
    return vel;
}

uint GetXSpeed(uint voxel) {
    return (GetVelocityByte(voxel) >> 2) & 0x7;
}

uint GetHeading(uint voxel) {
    return GetVelocityByte(voxel) & 0x3;
}

uint PackVelocity(int yVel, uint xSpeed, uint heading) {
    uint y = ((uint)yVel) & 0x7;
    return (y << 5) | ((xSpeed & 0x7) << 2) | (heading & 0x3);
}

// Material type checks
bool IsAir(uint voxel) {
    return GetMaterial(voxel) == MAT_AIR;
}

bool IsSolid(uint voxel) {
    uint mat = GetMaterial(voxel);
    return mat == MAT_STONE || mat == MAT_BEDROCK || mat == MAT_GLASS;
}

bool IsLiquid(uint voxel) {
    uint mat = GetMaterial(voxel);
    return mat == MAT_WATER || mat == MAT_LAVA || mat == MAT_OIL ||
           mat == MAT_ACID || mat == MAT_HONEY || mat == MAT_CONCRETE;
}

bool IsPowder(uint voxel) {
    uint mat = GetMaterial(voxel);
    return mat == MAT_SAND || mat == MAT_DIRT;
}

bool CanFall(uint voxel) {
    return !IsAir(voxel) && !IsSolid(voxel) && !IsStatic(voxel);
}

#endif // BIT_PACKING_HLSLI
