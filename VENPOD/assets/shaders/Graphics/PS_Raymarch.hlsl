// =============================================================================
// VENPOD Voxel Raymarcher Pixel Shader
// DDA algorithm for stepping through voxel grid
// =============================================================================

#include "../Common/SharedTypes.hlsli"
#include "../Common/MortonCode.hlsli"
#include "../Common/BitPacking.hlsli"

// Constant buffer
cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

// Voxel grid (read-only for rendering)
StructuredBuffer<uint> VoxelGrid : register(t0);

// Material palette
Texture1D<float4> MaterialPalette : register(t1);
SamplerState PaletteSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Sample voxel from grid
uint GetVoxel(int3 worldPos) {
    // CRITICAL FIX: Convert world position to buffer-local position
    // The render buffer is a "moving window" that follows the camera.
    // When camera moves, chunks are copied to different buffer positions,
    // so we must subtract the region origin to get the correct buffer index.
    int3 bufferPos = worldPos - int3(frame.regionOrigin.xyz);

    // Bounds check (buffer-local coordinates)
    if (bufferPos.x < 0 || bufferPos.x >= (int)frame.gridSizeX ||
        bufferPos.y < 0 || bufferPos.y >= (int)frame.gridSizeY ||
        bufferPos.z < 0 || bufferPos.z >= (int)frame.gridSizeZ) {
        return PackVoxel(MAT_AIR, 0, 0, 0);
    }

    uint3 gridSize = uint3(frame.gridSizeX, frame.gridSizeY, frame.gridSizeZ);
    uint idx = LinearIndex3D(uint3(bufferPos), gridSize);
    return VoxelGrid[idx];
}

// Box intersection test (AABB ray intersection)
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

// DDA Raymarcher
float4 Raymarch(float3 rayOrigin, float3 rayDir) {
    const float maxDist = 1024.0f;  // Increased for larger worlds
    const int maxSteps = 512;       // More steps for distant voxels

    // CRITICAL FIX: Grid bounds in WORLD coordinates (not buffer coordinates)
    // The buffer is a moving window, so grid bounds = regionOrigin + bufferSize
    float3 gridMin = frame.regionOrigin.xyz;
    float3 gridMax = frame.regionOrigin.xyz + float3(frame.gridSizeX, frame.gridSizeY, frame.gridSizeZ);

    // Find ray entry point into grid
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, gridMin, gridMax, tMin, tMax)) {
        // Ray doesn't intersect grid - show sky
        float skyFactor = saturate(rayDir.y * 0.5f + 0.5f);
        float3 skyTop = float3(0.3f, 0.5f, 0.8f);
        float3 skyBottom = float3(0.8f, 0.9f, 1.0f);
        float3 skyColor = lerp(skyBottom, skyTop, skyFactor);
        return float4(skyColor, 1.0f);
    }

    // Start raymarching from grid entry point (or ray origin if inside grid)
    float3 startPos = rayOrigin + rayDir * max(tMin, 0.0f);

    // Start position in voxel grid
    int3 voxelPos = int3(floor(startPos));

    // DDA setup
    float3 deltaDist = abs(1.0f / rayDir);
    int3 step = int3(sign(rayDir));

    float3 sideDist;
    sideDist.x = (rayDir.x > 0.0f) ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = (rayDir.y > 0.0f) ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = (rayDir.z > 0.0f) ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    float3 normal = float3(0, 1, 0);
    float dist = 0.0f;

    // DDA traversal
    for (int i = 0; i < maxSteps; i++) {
        uint voxel = GetVoxel(voxelPos);
        uint material = GetMaterial(voxel);

        // Hit non-air voxel?
        if (material != MAT_AIR) {
            // Sample material color from palette
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);

            // Simple diffuse lighting
            float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
            float ndotl = max(dot(normal, lightDir), 0.1f);  // Ambient = 0.1

            // Add slight variant-based color variation
            uint variant = GetVariant(voxel);
            float variantNoise = (variant / 255.0f) * 0.1f - 0.05f;  // +/- 5%

            float3 finalColor = baseColor.rgb * ndotl * (1.0f + variantNoise);

            // Depth fog
            float fogFactor = saturate(dist / maxDist);
            float3 fogColor = float3(0.5f, 0.6f, 0.7f);  // Sky blue
            finalColor = lerp(finalColor, fogColor, fogFactor * 0.5f);

            // Use material's alpha from palette (enables transparency for water, glass, etc.)
            return float4(finalColor, baseColor.a);
        }

        // Step to next voxel boundary
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                sideDist.x += deltaDist.x;
                voxelPos.x += step.x;
                normal = float3(-step.x, 0, 0);
                dist = sideDist.x;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = float3(0, 0, -step.z);
                dist = sideDist.z;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                sideDist.y += deltaDist.y;
                voxelPos.y += step.y;
                normal = float3(0, -step.y, 0);
                dist = sideDist.y;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = float3(0, 0, -step.z);
                dist = sideDist.z;
            }
        }

        if (dist > maxDist) break;
    }

    // Sky gradient
    float skyFactor = saturate(rayDir.y * 0.5f + 0.5f);
    float3 skyTop = float3(0.3f, 0.5f, 0.8f);
    float3 skyBottom = float3(0.8f, 0.9f, 1.0f);
    float3 skyColor = lerp(skyBottom, skyTop, skyFactor);

    return float4(skyColor, 1.0f);
}

// Render brush preview as semi-transparent overlay
float4 RenderBrushPreview(float3 rayOrigin, float3 rayDir, float3 brushCenter, float brushRadius, uint brushShape, float3 baseColor) {
    // Safety check: Don't render if camera is too close to or inside the brush
    float distToCenter = length(rayOrigin - brushCenter);
    if (distToCenter < brushRadius * 1.5f) {
        // Camera is too close - don't render to avoid visual glitches
        return float4(0, 0, 0, 0);
    }

    // Ray-sphere intersection for brush preview
    float3 oc = rayOrigin - brushCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - brushRadius * brushRadius;
    float discriminant = b * b - c;

    if (discriminant < 0.0f) {
        return float4(0, 0, 0, 0);  // No intersection
    }

    float t = -b - sqrt(discriminant);
    if (t < 0.0f) t = -b + sqrt(discriminant);  // Inside sphere
    if (t < 0.0f) return float4(0, 0, 0, 0);    // Behind camera

    // Don't render if intersection is too close (less than 2 voxels away)
    if (t < 2.0f) {
        return float4(0, 0, 0, 0);
    }

    float3 hitPoint = rayOrigin + rayDir * t;

    // For sphere shape, use distance-based alpha
    if (brushShape == 0) {  // Sphere
        float dist = length(hitPoint - brushCenter);
        float normalizedDist = dist / brushRadius;

        // Create wireframe effect - more opaque at edges
        float edgeFactor = abs(normalizedDist - 0.95f) < 0.05f ? 0.6f : 0.15f;

        // Fresnel-like effect for better visibility
        float3 normal = normalize(hitPoint - brushCenter);
        float fresnel = pow(1.0f - abs(dot(normal, rayDir)), 2.0f);
        float alpha = lerp(edgeFactor, 0.4f, fresnel);

        return float4(baseColor, alpha);
    }
    else {  // Cube or cylinder - simple semi-transparent rendering
        return float4(baseColor, 0.25f);
    }
}

float4 main(PSInput input) : SV_Target {
    // Camera data from constant buffer
    float3 cameraPos = frame.cameraPosition.xyz;
    float3 forward = frame.cameraForward.xyz;
    float3 right = frame.cameraRight.xyz;
    float3 up = frame.cameraUp.xyz;
    float fov = frame.cameraPosition.w;
    float aspectRatio = frame.cameraForward.w;

    // Ray direction from UV
    float2 ndc = input.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;  // Flip Y

    float tanHalfFov = tan(fov * 0.5f);

    float3 rayDir = normalize(
        forward +
        right * ndc.x * tanHalfFov * aspectRatio +
        up * ndc.y * tanHalfFov
    );

    // Render voxel world
    float4 voxelColor = Raymarch(cameraPos, rayDir);

    // Render brush preview overlay if valid position
    if (frame.brushParams.z > 0.5f) {  // hasValidPosition
        float3 brushPos = frame.brushPosition.xyz;
        float brushRadius = frame.brushPosition.w;
        uint brushMaterial = (uint)frame.brushParams.x;
        uint brushShape = (uint)frame.brushParams.y;

        // Get material color for preview
        float u = (float(brushMaterial) + 0.5f) / 256.0f;
        float3 materialColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0).rgb;

        // Render semi-transparent brush preview
        float4 brushPreview = RenderBrushPreview(cameraPos, rayDir, brushPos, brushRadius, brushShape, materialColor);

        // Alpha blend preview over voxel color
        if (brushPreview.a > 0.0f) {
            voxelColor.rgb = lerp(voxelColor.rgb, brushPreview.rgb, brushPreview.a);
        }
    }

    return voxelColor;
}
