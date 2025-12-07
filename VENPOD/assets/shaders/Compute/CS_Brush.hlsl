// =============================================================================
// VENPOD Brush Compute Shader
// Paints voxels using SDF shapes (sphere, cube, cylinder)
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"
#include "../Common/PCGRandom.hlsli"

// Brush modes
#define BRUSH_MODE_PAINT   0
#define BRUSH_MODE_ERASE   1
#define BRUSH_MODE_REPLACE 2
#define BRUSH_MODE_FILL    3

// Brush shapes
#define BRUSH_SHAPE_SPHERE   0
#define BRUSH_SHAPE_CUBE     1
#define BRUSH_SHAPE_CYLINDER 2

// Root constants
cbuffer BrushConstants : register(b0) {
    float positionX;
    float positionY;
    float positionZ;
    float radius;

    uint material;
    uint mode;
    uint shape;
    float strength;

    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint seed;
};

// Voxel buffer (read-write)
RWStructuredBuffer<uint> VoxelBuffer : register(u0);

// Signed distance functions
float SDFSphere(float3 p, float3 center, float r) {
    return length(p - center) - r;
}

float SDFBox(float3 p, float3 center, float3 halfExtents) {
    float3 d = abs(p - center) - halfExtents;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

float SDFCylinder(float3 p, float3 center, float r, float h) {
    float3 d = p - center;
    float2 xz = float2(d.x, d.z);
    float2 dh = abs(float2(length(xz), d.y)) - float2(r, h);
    return min(max(dh.x, dh.y), 0.0) + length(max(dh, 0.0));
}

// Get SDF value based on brush shape
float GetBrushSDF(float3 p, float3 center, float r, uint brushShape) {
    switch (brushShape) {
        case BRUSH_SHAPE_SPHERE:
            return SDFSphere(p, center, r);
        case BRUSH_SHAPE_CUBE:
            return SDFBox(p, center, float3(r, r, r));
        case BRUSH_SHAPE_CYLINDER:
            return SDFCylinder(p, center, r, r);
        default:
            return SDFSphere(p, center, r);
    }
}

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= gridSizeX || DTid.y >= gridSizeY || DTid.z >= gridSizeZ) {
        return;
    }

    // Calculate brush center
    float3 brushCenter = float3(positionX, positionY, positionZ);

    // Get voxel position (center of voxel)
    float3 voxelPos = float3(DTid) + 0.5;

    // Calculate SDF distance
    float dist = GetBrushSDF(voxelPos, brushCenter, radius, shape);

    // Only affect voxels inside the brush
    if (dist > 0.5) {
        return;
    }

    // Calculate linear index
    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint index = LinearIndex3D(DTid, gridSize);

    // Read current voxel
    uint currentVoxel = VoxelBuffer[index];
    uint currentMaterial = GetMaterial(currentVoxel);

    // Generate random variant for new voxels
    uint random = Random3D(DTid, seed);
    uint variant = random & 0xFF;

    // Apply brush based on mode
    uint newVoxel = currentVoxel;

    switch (mode) {
        case BRUSH_MODE_PAINT:
            // Only paint in air or if forcing
            if (currentMaterial == MAT_AIR) {
                newVoxel = PackVoxel(material, variant, 0, 0);
            }
            break;

        case BRUSH_MODE_ERASE:
            // Set to air (material = 0)
            if (currentMaterial != MAT_AIR && currentMaterial != MAT_BEDROCK) {
                newVoxel = PackVoxel(MAT_AIR, 0, 0, 0);
            }
            break;

        case BRUSH_MODE_REPLACE:
            // Replace any non-air, non-bedrock voxel
            if (currentMaterial != MAT_AIR && currentMaterial != MAT_BEDROCK) {
                newVoxel = PackVoxel(material, variant, 0, 0);
            }
            break;

        case BRUSH_MODE_FILL:
            // Fill all voxels (including air, but not bedrock)
            if (currentMaterial != MAT_BEDROCK) {
                newVoxel = PackVoxel(material, variant, 0, 0);
            }
            break;
    }

    // Apply strength-based edge softening
    // Voxels at the edge of the brush have lower probability of being affected
    if (strength < 1.0 && dist > -0.5) {
        float edgeFactor = saturate(1.0 - dist / 0.5);
        float probability = edgeFactor * strength;
        if (RandomFloat(random ^ seed) > probability) {
            return;  // Don't modify this voxel
        }
    }

    // Write result
    VoxelBuffer[index] = newVoxel;
}
