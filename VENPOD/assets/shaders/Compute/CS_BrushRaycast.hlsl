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
    float4 rayOrigin;       // xyz = local-space ray origin, w = unused
    float4 rayDirection;    // xyz = normalized ray direction, w = unused

    uint gridSizeX;
    uint gridSizeY;
    uint gridSizeZ;
    uint padding;

    float4 regionOrigin;    // xyz = world-space dense render-window bounds minimum
}

// Input voxel grid (read-only)
StructuredBuffer<uint> VoxelGrid : register(t0);
StructuredBuffer<uint4> ChunkValidMask : register(t1);

// Output: Brush raycast result (16 bytes total)
// Format: [hitPosX, hitPosY, hitPosZ, hitNormalPacked]
// hitNormalPacked: encodes normal vector + validity flag
RWStructuredBuffer<float4> BrushRaycastResult : register(u0);

int FloorDiv64(int value) {
    return value >= 0 ? value / 64 : -(((-value) + 63) / 64);
}

int FloorModInt(int value, int modulus) {
    int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

uint RenderSlotIndex(int3 chunkCoord, out uint3 slotCoord) {
    const int chunksX = (int)(gridSizeX / 64u);
    const int chunksY = (int)(gridSizeY / 64u);
    const int chunksZ = (int)(gridSizeZ / 64u);
    slotCoord = uint3(
        (uint)FloorModInt(chunkCoord.x, chunksX),
        (uint)FloorModInt(chunkCoord.y, chunksY),
        (uint)FloorModInt(chunkCoord.z, chunksZ));
    return slotCoord.x + slotCoord.y * (uint)chunksX + slotCoord.z * (uint)chunksX * (uint)chunksY;
}

// Safe world-space voxel access through the toroidal near-field cache.
uint GetVoxelSafe(int3 worldPos) {
    int3 worldChunk = int3(
        FloorDiv64(worldPos.x),
        FloorDiv64(worldPos.y),
        FloorDiv64(worldPos.z));
    uint3 localVoxel = uint3(
        (uint)FloorModInt(worldPos.x, 64),
        (uint)FloorModInt(worldPos.y, 64),
        (uint)FloorModInt(worldPos.z, 64));

    uint3 slotCoord;
    uint chunkIndex = RenderSlotIndex(worldChunk, slotCoord);
    uint4 slotTag = ChunkValidMask[chunkIndex];
    if (slotTag.w == 0u ||
        slotTag.x != (uint)worldChunk.x ||
        slotTag.y != (uint)worldChunk.y ||
        slotTag.z != (uint)worldChunk.z) {
        return PackVoxel(MAT_AIR, 0, 0, 0);
    }
    uint3 gridSize = uint3(gridSizeX, gridSizeY, gridSizeZ);
    uint3 bufferPos = slotCoord * 64u + localVoxel;
    uint idx = LinearIndex3D(bufferPos, gridSize);
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

    // World-space render-window bounds. The lookup itself is toroidal, but
    // bounds keep the one-thread DDA from marching outside the near field.
    float3 gridMin = regionOrigin.xyz;
    float3 gridMax = regionOrigin.xyz + float3(gridSizeX, gridSizeY, gridSizeZ);

    // Check ray-grid intersection
    float tMin, tMax;
    if (!IntersectBox(origin, dir, gridMin, gridMax, tMin, tMax)) {
        // No intersection - write invalid result
        BrushRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
        return;
    }

    // Start raymarching from grid entry point (or ray origin if inside grid)
    // Add small epsilon to ensure we're inside the grid when entering from outside
    float entryT = max(tMin, 0.0f) + 0.001f;  // Move slightly off voxel/grid boundaries
    float3 startPos = origin + dir * entryT;

    // DDA setup (same as PS_Raymarch.hlsl and CPU BrushController)
    int3 voxelPos = int3(floor(startPos));
    float3 deltaDist = abs(1.0f / dir);
    int3 step = int3(sign(dir));

    float3 sideDist;
    sideDist.x = (dir.x > 0.0f) ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = (dir.y > 0.0f) ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = (dir.z > 0.0f) ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    int3 normal = int3(0, 1, 0);  // Default normal
    float dist = 0.0f;

    const float maxDist = 4096.0f;
    const int maxSteps = 4096;

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
        float nextDist;
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                nextDist = sideDist.x;
                voxelPos.x += step.x;
                sideDist.x += deltaDist.x;
                normal = int3(-step.x, 0, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = int3(0, 0, -step.z);
                dist = nextDist;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                nextDist = sideDist.y;
                voxelPos.y += step.y;
                sideDist.y += deltaDist.y;
                normal = int3(0, -step.y, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = int3(0, 0, -step.z);
                dist = nextDist;
            }
        }

        if (dist > maxDist) {
            break;
        }
    }

    // No solid voxel hit - write invalid result
    BrushRaycastResult[0] = float4(0, 0, 0, PackNormal(int3(0, 0, 0), false));
}
