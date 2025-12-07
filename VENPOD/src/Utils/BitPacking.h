#pragma once

#include <cstdint>

// Header-only bit packing utilities for voxel data
// Voxel format (32-bit):
//   Bits 31-24: State (IsStatic, IsIgnited, HasMoved, Life[4])
//   Bits 23-16: Velocity (Y_Vel[3], X_Speed[3], Heading[2])
//   Bits 15-08: Variant (visual noise)
//   Bits 07-00: Material ID (0-255)

namespace VENPOD::Utils {

// Material IDs
namespace Material {
    constexpr uint8_t Air = 0;
    constexpr uint8_t Sand = 1;
    constexpr uint8_t Water = 2;
    constexpr uint8_t Stone = 3;
    constexpr uint8_t Dirt = 4;
    constexpr uint8_t Wood = 5;
    constexpr uint8_t Fire = 6;
    constexpr uint8_t Lava = 7;
    constexpr uint8_t Ice = 8;
    constexpr uint8_t Oil = 9;
    constexpr uint8_t Glass = 10;
    constexpr uint8_t Bedrock = 255;
}

// State flags
namespace StateFlags {
    constexpr uint8_t IsStatic   = 0x80;  // Bit 7
    constexpr uint8_t IsIgnited  = 0x40;  // Bit 6
    constexpr uint8_t HasMoved   = 0x20;  // Bit 5
    constexpr uint8_t LifeMask   = 0x0F;  // Bits 0-3 (life counter)
}

// Pack voxel data into 32-bit integer
inline uint32_t PackVoxel(uint8_t material, uint8_t variant, uint8_t velocity, uint8_t state) {
    return static_cast<uint32_t>(material)
         | (static_cast<uint32_t>(variant) << 8)
         | (static_cast<uint32_t>(velocity) << 16)
         | (static_cast<uint32_t>(state) << 24);
}

// Unpack material from voxel
inline uint8_t UnpackMaterial(uint32_t voxel) {
    return static_cast<uint8_t>(voxel & 0xFF);
}

// Unpack variant from voxel
inline uint8_t UnpackVariant(uint32_t voxel) {
    return static_cast<uint8_t>((voxel >> 8) & 0xFF);
}

// Unpack velocity from voxel
inline uint8_t UnpackVelocity(uint32_t voxel) {
    return static_cast<uint8_t>((voxel >> 16) & 0xFF);
}

// Unpack state from voxel
inline uint8_t UnpackState(uint32_t voxel) {
    return static_cast<uint8_t>((voxel >> 24) & 0xFF);
}

// Check if voxel is air (empty)
inline bool IsAir(uint32_t voxel) {
    return UnpackMaterial(voxel) == Material::Air;
}

// Check if voxel is static (doesn't move)
inline bool IsStatic(uint32_t voxel) {
    return (UnpackState(voxel) & StateFlags::IsStatic) != 0;
}

// Create simple voxel with just material
inline uint32_t MakeVoxel(uint8_t material) {
    return PackVoxel(material, 0, 0, 0);
}

} // namespace VENPOD::Utils
