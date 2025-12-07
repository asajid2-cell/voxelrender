// =============================================================================
// VENPOD Morton Code - 3D <-> 1D encoding for voxel grid addressing
// =============================================================================

#ifndef MORTON_CODE_HLSLI
#define MORTON_CODE_HLSLI

// Expand bits for Morton encoding
// Spreads bits so each bit has 2 zero bits between it
// e.g., 0b11111111 -> 0b001001001001001001001001
uint ExpandBits(uint v) {
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

// Compact bits for Morton decoding
// Reverses ExpandBits operation
uint CompactBits(uint v) {
    v &= 0x49249249u;
    v = (v ^ (v >> 2))  & 0xC30C30C3u;
    v = (v ^ (v >> 4))  & 0x0F00F00Fu;
    v = (v ^ (v >> 8))  & 0xFF0000FFu;
    v = (v ^ (v >> 16)) & 0x0000FFFFu;
    return v;
}

// Encode 3D coordinates to 1D Morton code
// Max 10 bits per component (0-1023)
uint EncodeMorton3(uint3 coord) {
    return ExpandBits(coord.x) | (ExpandBits(coord.y) << 1) | (ExpandBits(coord.z) << 2);
}

// Decode 1D Morton code to 3D coordinates
uint3 DecodeMorton3(uint code) {
    return uint3(
        CompactBits(code),
        CompactBits(code >> 1),
        CompactBits(code >> 2)
    );
}

// Simple linear 3D index (non-Morton)
// Use when cache coherence is less important than simplicity
uint LinearIndex3D(uint3 coord, uint3 gridSize) {
    return coord.x + coord.y * gridSize.x + coord.z * gridSize.x * gridSize.y;
}

// Convert linear index back to 3D coordinates
uint3 LinearToCoord3D(uint index, uint3 gridSize) {
    uint z = index / (gridSize.x * gridSize.y);
    uint remainder = index % (gridSize.x * gridSize.y);
    uint y = remainder / gridSize.x;
    uint x = remainder % gridSize.x;
    return uint3(x, y, z);
}

// Check if coordinates are within grid bounds
bool IsInBounds(int3 coord, uint3 gridSize) {
    return coord.x >= 0 && coord.x < (int)gridSize.x &&
           coord.y >= 0 && coord.y < (int)gridSize.y &&
           coord.z >= 0 && coord.z < (int)gridSize.z;
}

#endif // MORTON_CODE_HLSLI
