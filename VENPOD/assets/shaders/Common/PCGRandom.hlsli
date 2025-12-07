// =============================================================================
// VENPOD PCG Random - GPU-friendly pseudo-random number generator
// Based on PCG (Permuted Congruential Generator) by Melissa O'Neill
// =============================================================================

#ifndef PCG_RANDOM_HLSLI
#define PCG_RANDOM_HLSLI

// PCG hash function - generates random uint from seed
uint PCGHash(uint seed) {
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Generate random uint from 2D coordinates and frame
uint Random2D(uint2 coord, uint frame) {
    uint seed = coord.x + coord.y * 65536u + frame * 4294967291u;
    return PCGHash(seed);
}

// Generate random uint from 3D coordinates and frame
uint Random3D(uint3 coord, uint frame) {
    uint seed = coord.x + coord.y * 256u + coord.z * 65536u + frame * 16777213u;
    return PCGHash(seed);
}

// Generate random float in [0, 1)
float RandomFloat(uint random) {
    return (float)(random & 0xFFFFFF) / 16777216.0f;
}

// Generate random float in [min, max)
float RandomRange(uint random, float minVal, float maxVal) {
    return minVal + RandomFloat(random) * (maxVal - minVal);
}

// Generate random int in [0, max)
uint RandomInt(uint random, uint maxVal) {
    return random % maxVal;
}

// Generate random bool with given probability [0, 1]
bool RandomBool(uint random, float probability) {
    return RandomFloat(random) < probability;
}

// Generate random direction (0=N, 1=E, 2=S, 3=W)
uint RandomDirection(uint random) {
    return random & 0x3;
}

// Generate random direction favoring diagonal movement
// Returns offset: -1, 0, or +1
int RandomDispersion(uint random) {
    uint r = random & 0x3;
    if (r == 0) return -1;
    if (r == 1) return 1;
    return 0;  // 50% chance of no lateral movement
}

// Noise functions for initialization
float ValueNoise3D(float3 p) {
    float3 pi = floor(p);
    float3 pf = frac(p);

    // Smooth interpolation
    float3 w = pf * pf * (3.0 - 2.0 * pf);

    // Hash corners
    float n000 = RandomFloat(PCGHash((uint)pi.x + (uint)pi.y * 57u + (uint)pi.z * 113u));
    float n100 = RandomFloat(PCGHash((uint)pi.x + 1u + (uint)pi.y * 57u + (uint)pi.z * 113u));
    float n010 = RandomFloat(PCGHash((uint)pi.x + ((uint)pi.y + 1u) * 57u + (uint)pi.z * 113u));
    float n110 = RandomFloat(PCGHash((uint)pi.x + 1u + ((uint)pi.y + 1u) * 57u + (uint)pi.z * 113u));
    float n001 = RandomFloat(PCGHash((uint)pi.x + (uint)pi.y * 57u + ((uint)pi.z + 1u) * 113u));
    float n101 = RandomFloat(PCGHash((uint)pi.x + 1u + (uint)pi.y * 57u + ((uint)pi.z + 1u) * 113u));
    float n011 = RandomFloat(PCGHash((uint)pi.x + ((uint)pi.y + 1u) * 57u + ((uint)pi.z + 1u) * 113u));
    float n111 = RandomFloat(PCGHash((uint)pi.x + 1u + ((uint)pi.y + 1u) * 57u + ((uint)pi.z + 1u) * 113u));

    // Trilinear interpolation
    float nx00 = lerp(n000, n100, w.x);
    float nx10 = lerp(n010, n110, w.x);
    float nx01 = lerp(n001, n101, w.x);
    float nx11 = lerp(n011, n111, w.x);

    float nxy0 = lerp(nx00, nx10, w.y);
    float nxy1 = lerp(nx01, nx11, w.y);

    return lerp(nxy0, nxy1, w.z);
}

// Simple FBM noise
float FBMNoise3D(float3 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * ValueNoise3D(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    return value;
}

#endif // PCG_RANDOM_HLSLI
