#pragma once

#include <cstdint>

// Header-only Morton code implementation
// Used to map 3D coordinates to 1D indices with good cache locality

namespace VENPOD::Utils {

// Spread bits for Morton encoding (interleave zeros)
inline uint32_t SpreadBits3D(uint32_t x) {
    x &= 0x000003FF;                  // Keep only 10 bits
    x = (x | (x << 16)) & 0x030000FF;
    x = (x | (x <<  8)) & 0x0300F00F;
    x = (x | (x <<  4)) & 0x030C30C3;
    x = (x | (x <<  2)) & 0x09249249;
    return x;
}

// Compact bits for Morton decoding (remove interleaved zeros)
inline uint32_t CompactBits3D(uint32_t x) {
    x &= 0x09249249;
    x = (x | (x >>  2)) & 0x030C30C3;
    x = (x | (x >>  4)) & 0x0300F00F;
    x = (x | (x >>  8)) & 0x030000FF;
    x = (x | (x >> 16)) & 0x000003FF;
    return x;
}

// Encode 3D coordinates to 1D Morton index
// Supports coordinates up to 1024 (10 bits each)
inline uint32_t EncodeMorton3D(uint32_t x, uint32_t y, uint32_t z) {
    return SpreadBits3D(x) | (SpreadBits3D(y) << 1) | (SpreadBits3D(z) << 2);
}

// Decode 1D Morton index to 3D coordinates
inline void DecodeMorton3D(uint32_t morton, uint32_t& x, uint32_t& y, uint32_t& z) {
    x = CompactBits3D(morton);
    y = CompactBits3D(morton >> 1);
    z = CompactBits3D(morton >> 2);
}

} // namespace VENPOD::Utils
