// =============================================================================
// VENPOD Brush Raycast Compute Shader
// GPU-side DDA raycasting for brush preview positioning
// Replaces expensive CPU-side raycasting with GPU compute
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"

// Brush raycast input
cbuffer BrushRaycastConstants : register(b0) {
    float4 rayOrigin;       // xyz = camera position, w = unused
    float4 rayDirection;    // xyz = normalized ray direction, w = unused

    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint padding;
}

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGrid : register(t0);

// Output: Brush raycast result (16 bytes total)
// Format: [hitPosX, hitPosY, hitPosZ, hitNormalPacked]
// hitNormalPacked: encodes normal vector + validity flag
RWStructuredBuffer<float4> BrushRaycastResult : register(u0);

// Safe voxel access
uint GetVoxelSafe(int3 pos) {
    if (pos.x < 0 || pos.x >= (int)gridSizeX ||
        pos.y < 0 || pos.y >= (int)gridSizeY ||
        pos.z < 0 || pos.z >= (int)gridSizeZ) {
        return PackVoxel(MAT_AIR, 0, 0, 0);  // Out of bounds = air
    }

    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint idx = LinearIndex3D(uint3(pos), gridSize);
    return VoxelGrid[idx];
}

// Ray-AABB intersection
bool IntersectBox(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tMin, out float tMax) {
    float3 invDir = 1.0f / rayDir;
    float3 t0 = (boxMin - rayOrigin) * invDir;
    float3 t1 = (boxMax - rayOrigin) * invDir;

    float3 tNear = min(t0, t1);
    float3 tFar = max(t0, t1);

    tMin = max(max(tNear.x, tNear.y), tNear.z);
    tMax = min(min(tFar.x, tFar.y), tFar.z);

    return tMax >= tMin && tMax >= 0.0f;
}

// Pack normal vector into float (x=-1/0/+1, y=-1/0/+1, z=-1/0/+1, valid flag)
float PackNormal(int3 normal, bool valid) {
    // Encode: bits 0-1 = X (-1,0,1 -> 0,1,2), bits 2-3 = Y, bits 4-5 = Z, bit 6 = valid
    uint packed = 0;
    packed |= (uint)(normal.x + 1);          // bits 0-1
    packed |= (uint)(normal.y + 1) << 2;     // bits 2-3
    packed |= (uint)(normal.z + 1) << 4;     // bits 4-5
    packed |= (valid ? 1u : 0u) << 6;        // bit 6
    return asfloat(packed);
}

// Main compute shader - runs ONCE per frame (1 thread)
[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Only one thread does the raycast
    if (DTid.x != 0 || DTid.y != 0 || DTid.z != 0) {
        return;
    }

    float3 origin = rayOrigin.xyz;
    float3 dir = normalize(rayDirection.xyz);

    // Grid bounds
    float3 gridMin = float3(0.0f, 0.0f, 0.0f);
    float3 gridMax = float3(gridSizeX, gridSizeY, gridSizeZ);

    // Check ray-grid intersection
    float tMin, tMax;
    if (!IntersectBox(origin, dir, gridMin, gridMax, tMin, tMax)) {
        // No intersection - write invalid result
        BrushRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
        return;
    }

    // Start raymarching from grid entry point (or ray origin if inside grid)
    // Add small epsilon to ensure we're inside the grid when entering from outside
    float entryT = max(tMin, 0.0f);
    if (tMin > 0.0f) entryT += 0.001f;  // Move slightly inside grid boundary
    float3 startPos = origin + dir * entryT;

    // DDA setup (same as PS_Raymarch.hlsl and CPU BrushController)
    // Add small epsilon to floor to avoid precision issues at boundaries
    int3 voxelPos = int3(floor(startPos + float3(1e-4f, 1e-4f, 1e-4f)));
    float3 deltaDist = abs(1.0f / dir);
    int3 step = int3(sign(dir));

    float3 sideDist;
    sideDist.x = (dir.x > 0.0f) ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = (dir.y > 0.0f) ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = (dir.z > 0.0f) ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    int3 normal = int3(0, 1, 0);  // Default normal
    float dist = 0.0f;

    const float maxDist = 1024.0f;
    const int maxSteps = 512;

    // DDA traversal - find first solid voxel
    for (int i = 0; i < maxSteps; ++i) {
        uint voxel = GetVoxelSafe(voxelPos);
        uint material = GetMaterial(voxel);

        // Hit non-air voxel?
        if (material != MAT_AIR) {
            // Calculate brush position (adjacent voxel on the face we hit)
            float3 brushPos = float3(voxelPos) + float3(normal) + float3(0.5f, 0.5f, 0.5f);

            // Write result: position + packed normal (with valid=true)
            BrushRaycastResult[0] = float4(brushPos, PackNormal(normal, true));
            return;
        }

        // Step to next voxel boundary
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                sideDist.x += deltaDist.x;
                voxelPos.x += step.x;
                normal = int3(-step.x, 0, 0);
                dist = sideDist.x;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = int3(0, 0, -step.z);
                dist = sideDist.z;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                sideDist.y += deltaDist.y;
                voxelPos.y += step.y;
                normal = int3(0, -step.y, 0);
                dist = sideDist.y;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = int3(0, 0, -step.z);
                dist = sideDist.z;
            }
        }

        if (dist > maxDist) {
            break;
        }
    }

    // No solid voxel hit - write invalid result
    BrushRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
}
