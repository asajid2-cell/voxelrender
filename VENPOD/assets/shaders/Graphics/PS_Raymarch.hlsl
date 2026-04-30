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

static const float FAR_TERRAIN_MIN_HEIGHT = -332.0f;
static const float FAR_TERRAIN_MAX_HEIGHT = 664.0f;
static const float FAR_SEA_LEVEL = -48.0f;
static const uint FAR_WORLD_SEED = 12345u;
static const bool FAR_TERRAIN_HORIZON_ENABLED = true;
static const float FAR_SVO_ROOT_CELL_SIZE = 512.0f;
static const float FAR_SVO_MIN_CELL_SIZE = 24.0f;
static const int FAR_SVO_MAX_LEVELS = 5;

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct RayHit {
    float4 color;
    float distance;
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

RayHit MakeHit(float4 color, float distance) {
    RayHit hit;
    hit.color = color;
    hit.distance = distance;
    return hit;
}

float3 SkySunDirection() {
    return normalize(float3(0.45f, 0.72f, 0.28f));
}

float3 SkyColor(float3 rayDir) {
    float up = saturate(rayDir.y * 0.5f + 0.5f);
    float horizon = pow(saturate(1.0f - abs(rayDir.y) * 1.45f), 2.0f);
    float sun = pow(saturate(dot(rayDir, SkySunDirection())), 420.0f);
    float sunBloom = pow(saturate(dot(rayDir, SkySunDirection())), 18.0f);
    float antiSun = pow(saturate(dot(rayDir, normalize(float3(-0.35f, 0.22f, -0.85f)))), 6.0f);

    float3 lowerSky = float3(0.72f, 0.84f, 0.96f);
    float3 upperSky = float3(0.22f, 0.43f, 0.78f);
    float3 horizonTint = float3(1.00f, 0.84f, 0.58f);
    float3 sky = lerp(lowerSky, upperSky, up);
    sky = lerp(sky, horizonTint, horizon * 0.42f);
    sky += float3(1.00f, 0.72f, 0.34f) * sunBloom * 0.32f;
    sky += float3(1.00f, 0.93f, 0.74f) * sun * 1.75f;
    sky += float3(0.18f, 0.24f, 0.38f) * antiSun * 0.10f;

    return saturate(sky);
}

float3 SkyAmbient(float3 normal) {
    float up = saturate(normal.y * 0.5f + 0.5f);
    float3 groundBounce = float3(0.20f, 0.16f, 0.12f);
    float3 skyBounce = float3(0.34f, 0.48f, 0.68f);
    return lerp(groundBounce, skyBounce, up);
}

float FarSmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

float FarRidged(float value, float power) {
    return pow(saturate(1.0f - abs(value)), power);
}

float FarTerrainHeight(float2 xz, out float mountainMask, out float spireMask, out float ravineMask) {
    // Very cheap far-horizon proxy. This intentionally does not try to match
    // editable near voxels exactly; it gives distant cliffs/spires a coherent
    // silhouette without running the full chunk generator per pixel.
    float n0 = sin(dot(xz, float2(0.00173f, 0.00091f)) + 2.1f);
    float n1 = sin(dot(xz, float2(-0.00077f, 0.00148f)) + 5.7f);
    float n2 = sin(dot(xz, float2(0.00320f, -0.00260f)) + 1.3f);
    float n3 = sin(dot(xz, float2(-0.00510f, 0.00430f)) + 8.4f);

    float continent = (n0 * 0.58f + n1 * 0.42f);
    mountainMask = FarSmooth01((continent + 0.20f) * 0.95f);

    float ridgeA = FarRidged(n2, 1.35f);
    float ridgeB = FarRidged(n3, 1.90f);
    float broadValley = FarRidged(sin(dot(xz, float2(0.00092f, 0.00111f)) + 0.4f), 1.2f);
    spireMask = pow(FarRidged(sin(dot(xz, float2(0.0078f, -0.0062f)) + n1), 2.0f), 3.5f) *
        (0.25f + mountainMask * 0.85f);
    ravineMask = 1.0f - smoothstep(0.02f, 0.12f, abs(sin(dot(xz, float2(0.00135f, -0.00105f)) + 2.6f)));

    float d = length(xz - float2(96.0f, 96.0f));
    float originUplift = (1.0f - FarSmooth01(d / 420.0f)) * 170.0f;

    float height = -85.0f;
    height += continent * 210.0f;
    height += ridgeA * (125.0f + mountainMask * 170.0f);
    height += ridgeB * mountainMask * 95.0f;
    height += spireMask * 360.0f;
    height += originUplift;
    height -= broadValley * (90.0f - mountainMask * 30.0f);
    height -= ravineMask * 230.0f;

    float terraceStep = lerp(10.0f, 22.0f, mountainMask);
    float terraced = floor(height / terraceStep) * terraceStep;
    height = lerp(height, terraced, 0.26f + mountainMask * 0.20f);

    return clamp(height, FAR_TERRAIN_MIN_HEIGHT, FAR_TERRAIN_MAX_HEIGHT);
}

uint FarTerrainMaterial(float2 xz, float height, float mountainMask, float spireMask, float ravineMask) {
    float materialNoise = sin(dot(xz, float2(0.013f, 0.017f)) + sin(dot(xz, float2(0.004f, -0.011f)))) * 0.5f + 0.5f;
    if (height < FAR_SEA_LEVEL + 4.0f) {
        return MAT_SAND;
    }
    if (ravineMask > 0.55f && materialNoise > 0.35f) {
        return MAT_STONE;
    }
    if (spireMask > 0.28f || height > 430.0f) {
        return MAT_STONE;
    }
    if (mountainMask > 0.70f || height > 220.0f) {
        return (materialNoise > 0.45f) ? MAT_STONE : MAT_CONCRETE;
    }
    if (materialNoise > 0.84f) {
        return MAT_CONCRETE;
    }
    return MAT_DIRT;
}

float3 FarTerrainNormal(float2 xz) {
    float mountainMaskA, spireMaskA, ravineMaskA;
    float mountainMaskB, spireMaskB, ravineMaskB;
    float hx0 = FarTerrainHeight(xz - float2(3.0f, 0.0f), mountainMaskA, spireMaskA, ravineMaskA);
    float hx1 = FarTerrainHeight(xz + float2(3.0f, 0.0f), mountainMaskB, spireMaskB, ravineMaskB);
    float hz0 = FarTerrainHeight(xz - float2(0.0f, 3.0f), mountainMaskA, spireMaskA, ravineMaskA);
    float hz1 = FarTerrainHeight(xz + float2(0.0f, 3.0f), mountainMaskB, spireMaskB, ravineMaskB);
    return normalize(float3(hx0 - hx1, 6.0f, hz0 - hz1));
}

bool FarSvoCellOccupied(float3 cellMin, float cellSize) {
    // First SVO pass: implicit node occupancy over the far procedural terrain.
    // This is intentionally read-only and visual-only. A node is occupied when
    // its world-space AABB intersects the terrain volume described by sampled
    // heightfield bounds. Empty cells can be skipped like sparse octree nodes.
    float3 cellMax = cellMin + cellSize;
    if (cellMax.y < FAR_TERRAIN_MIN_HEIGHT || cellMin.y > FAR_TERRAIN_MAX_HEIGHT + cellSize) {
        return false;
    }

    float2 c0 = cellMin.xz;
    float2 c1 = cellMin.xz + float2(cellSize, 0.0f);
    float2 c2 = cellMin.xz + float2(0.0f, cellSize);
    float2 c3 = cellMin.xz + float2(cellSize, cellSize);
    float2 cc = cellMin.xz + cellSize * 0.5f;

    float mm, sm, rm;
    float h0 = FarTerrainHeight(c0, mm, sm, rm);
    float h1 = FarTerrainHeight(c1, mm, sm, rm);
    float h2 = FarTerrainHeight(c2, mm, sm, rm);
    float h3 = FarTerrainHeight(c3, mm, sm, rm);
    float hc = FarTerrainHeight(cc, mm, sm, rm);

    float maxH = max(max(max(h0, h1), max(h2, h3)), hc) + cellSize * 0.35f;
    return cellMin.y <= maxH && cellMax.y >= FAR_TERRAIN_MIN_HEIGHT;
}

float FarSvoCellExitDistance(float3 rayOrigin, float3 rayDir, float3 cellMin, float cellSize, float currentT) {
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, cellMin, cellMin + cellSize, tMin, tMax)) {
        return currentT + cellSize;
    }
    return max(currentT + 1.0f, tMax + 0.75f);
}

float FarSvoSuggestedStep(float3 rayOrigin, float3 rayDir, float currentT) {
    float3 pos = rayOrigin + rayDir * currentT;
    float cellSize = FAR_SVO_ROOT_CELL_SIZE;

    [unroll]
    for (int level = 0; level < FAR_SVO_MAX_LEVELS; ++level) {
        float3 cellMin = floor(pos / cellSize) * cellSize;
        if (!FarSvoCellOccupied(cellMin, cellSize)) {
            return min(FarSvoCellExitDistance(rayOrigin, rayDir, cellMin, cellSize, currentT) - currentT,
                       cellSize * 1.25f);
        }
        cellSize *= 0.5f;
    }

    return max(FAR_SVO_MIN_CELL_SIZE, cellSize);
}

bool RaymarchFarTerrain(float3 rayOrigin, float3 rayDir, float startDist, out RayHit farHit) {
    farHit = MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);

    if (!FAR_TERRAIN_HORIZON_ENABLED) {
        return false;
    }

    // Keep the fallback in the horizon band. Steep downward/upward rays should
    // show only the dense editable window or sky; otherwise the cheap heightfield
    // can look like an overhead sheet when the dense window has an air gap.
    if (rayDir.y > 0.18f || rayDir.y < -0.42f) {
        return false;
    }

    const float farMaxDist = 10400.0f;
    float t = max(startDist, 900.0f);
    float previousT = t;
    float3 previousPos = rayOrigin + rayDir * t;
    float mountainMask, spireMask, ravineMask;
    float previousHeight = FarTerrainHeight(previousPos.xz, mountainMask, spireMask, ravineMask);
    float previousSigned = previousPos.y - previousHeight;

    [loop]
    for (int i = 0; i < 96 && t < farMaxDist; ++i) {
        float svoStep = FarSvoSuggestedStep(rayOrigin, rayDir, t);
        float distanceStep = lerp(48.0f, 220.0f, saturate(t / farMaxDist));
        float stepSize = max(FAR_SVO_MIN_CELL_SIZE, max(svoStep, distanceStep));
        t += stepSize;

        float3 pos = rayOrigin + rayDir * t;
        float height = FarTerrainHeight(pos.xz, mountainMask, spireMask, ravineMask);
        float signedDistance = pos.y - height;

        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            float lo = previousT;
            float hi = t;
            [unroll]
            for (int refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5f;
                float3 midPos = rayOrigin + rayDir * mid;
                float mm, sm, rm;
                float midHeight = FarTerrainHeight(midPos.xz, mm, sm, rm);
                if (midPos.y > midHeight) {
                    lo = mid;
                } else {
                    hi = mid;
                    mountainMask = mm;
                    spireMask = sm;
                    ravineMask = rm;
                    height = midHeight;
                }
            }

            float hitT = hi;
            float3 hitPos = rayOrigin + rayDir * hitT;
            float3 normal = FarTerrainNormal(hitPos.xz);
            uint material = FarTerrainMaterial(hitPos.xz, height, mountainMask, spireMask, ravineMask);
            float u = (material + 0.5f) / 256.0f;
            float4 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0);

            float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
            float lighting = max(dot(normal, lightDir), 0.18f);
            float3 color = baseColor.rgb * lighting;

            // Extra fog hides the fact that this is a heightfield fallback, not
            // the exact editable voxel buffer.
            float fogFactor = saturate((hitT - 900.0f) / (farMaxDist - 900.0f));
            color = lerp(color, SkyColor(rayDir), fogFactor * 0.72f);
            farHit = MakeHit(float4(color, 1.0f), hitT);
            return true;
        }

        previousT = t;
        previousSigned = signedDistance;
    }

    return false;
}

// DDA Raymarcher
RayHit Raymarch(float3 rayOrigin, float3 rayDir) {
    // Must cover the diagonal of the moving render window. Keep this generous:
    // shortening it can make startup look like a black/empty screen while chunks
    // are visible but beyond the ray budget.
    const float maxDist = 2500.0f;
    const int maxSteps = 2048;

    // CRITICAL FIX: Grid bounds in WORLD coordinates (not buffer coordinates)
    // The buffer is a moving window, so grid bounds = regionOrigin + bufferSize
    float3 gridMin = frame.regionOrigin.xyz;
    float3 gridMax = frame.regionOrigin.xyz + float3(frame.gridSizeX, frame.gridSizeY, frame.gridSizeZ);

    // Find ray entry point into grid
    float tMin, tMax;
    if (!IntersectBox(rayOrigin, rayDir, gridMin, gridMax, tMin, tMax)) {
        RayHit farHit;
        if (RaymarchFarTerrain(rayOrigin, rayDir, 32.0f, farHit)) {
            return farHit;
        }
        return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
    }

    // Start raymarching from grid entry point (or ray origin if inside grid).
    // Clamp traversal to the box exit so sky/horizon rays stop as soon as they
    // leave the render volume instead of burning the full maxDist budget.
    const float rayEpsilon = 0.001f;
    float entryDist = max(tMin, 0.0f) + rayEpsilon;
    float maxMarchDist = min(maxDist, max(tMax - entryDist, 0.0f));
    float3 startPos = rayOrigin + rayDir * entryDist;

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

            // Simple skybox/IBL-style lighting: direct sun plus directional
            // sky/ground ambient so shaded cliffs still read in the vertical world.
            float3 lightDir = SkySunDirection();
            float ndotl = saturate(dot(normal, lightDir));
            float3 ambient = SkyAmbient(normal) * 0.35f;

            // Add slight variant-based color variation
            uint variant = GetVariant(voxel);
            float variantNoise = (variant / 255.0f) * 0.1f - 0.05f;  // +/- 5%

            float3 finalColor = baseColor.rgb * (ambient + ndotl * 0.86f) * (1.0f + variantNoise);

            // Depth fog
            float fogFactor = saturate(dist / maxDist);
            float3 fogColor = SkyColor(rayDir);
            finalColor = lerp(finalColor, fogColor, fogFactor * 0.5f);

            // Use material's alpha from palette (enables transparency for water, glass, etc.)
            return MakeHit(float4(finalColor, baseColor.a), entryDist + dist);
        }

        // Step to next voxel boundary
        float nextDist;
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                nextDist = sideDist.x;
                voxelPos.x += step.x;
                sideDist.x += deltaDist.x;
                normal = float3(-step.x, 0, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = float3(0, 0, -step.z);
                dist = nextDist;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                nextDist = sideDist.y;
                voxelPos.y += step.y;
                sideDist.y += deltaDist.y;
                normal = float3(0, -step.y, 0);
                dist = nextDist;
            } else {
                nextDist = sideDist.z;
                voxelPos.z += step.z;
                sideDist.z += deltaDist.z;
                normal = float3(0, 0, -step.z);
                dist = nextDist;
            }
        }

        if (dist > maxMarchDist) break;
    }

    // Do not draw the far proxy through the near editable volume. If a ray
    // traversed the dense window and found no voxel, showing the proxy behind it
    // makes transient missing chunks look like strange overhead sheets/spikes.
    // Far terrain is only used when the ray misses the dense AABB entirely.
    return MakeHit(float4(SkyColor(rayDir), 1.0f), 1e20f);
}

bool IntersectAvatarBox(float3 localOrigin, float3 localDir, float3 boxMin, float3 boxMax, out float tNear, out float3 normal) {
    float3 invDir = 1.0f / localDir;
    float3 t0 = (boxMin - localOrigin) * invDir;
    float3 t1 = (boxMax - localOrigin) * invDir;
    float3 tMin3 = min(t0, t1);
    float3 tMax3 = max(t0, t1);

    tNear = max(max(tMin3.x, tMin3.y), tMin3.z);
    float tFar = min(min(tMax3.x, tMax3.y), tMax3.z);
    if (tFar < max(tNear, 0.0f)) {
        return false;
    }

    if (tNear < 0.0f) {
        tNear = tFar;
    }

    float3 hit = localOrigin + localDir * tNear;
    float3 dMin = abs(hit - boxMin);
    float3 dMax = abs(hit - boxMax);
    float best = dMin.x;
    normal = float3(-1, 0, 0);
    if (dMax.x < best) { best = dMax.x; normal = float3(1, 0, 0); }
    if (dMin.y < best) { best = dMin.y; normal = float3(0, -1, 0); }
    if (dMax.y < best) { best = dMax.y; normal = float3(0, 1, 0); }
    if (dMin.z < best) { best = dMin.z; normal = float3(0, 0, -1); }
    if (dMax.z < best) { normal = float3(0, 0, 1); }
    return true;
}

void TestAvatarPart(
    float3 localOrigin,
    float3 localDir,
    float3 boxMin,
    float3 boxMax,
    float3 color,
    inout float nearestT,
    inout float3 nearestNormal,
    inout float3 nearestColor)
{
    float t;
    float3 normal;
    if (IntersectAvatarBox(localOrigin, localDir, boxMin, boxMax, t, normal) && t < nearestT) {
        nearestT = t;
        nearestNormal = normal;
        nearestColor = color;
    }
}

bool RenderBlockCharacter(float3 rayOrigin, float3 rayDir, out float tHit, out float4 color) {
    tHit = 1e20f;
    color = float4(0, 0, 0, 0);

    if (frame.characterPosition.w < 0.5f) {
        return false;
    }

    float3 feet = frame.characterPosition.xyz;
    float3 forward = normalize(float3(frame.cameraForward.x, 0.0f, frame.cameraForward.z));
    if (length(forward) < 0.001f) {
        forward = float3(0, 0, 1);
    }
    float3 right = normalize(float3(frame.cameraRight.x, 0.0f, frame.cameraRight.z));

    float3 rel = rayOrigin - feet;
    float3 localOrigin = float3(dot(rel, right), rel.y, dot(rel, forward));
    float3 localDir = normalize(float3(dot(rayDir, right), rayDir.y, dot(rayDir, forward)));

    float3 nearestNormal = float3(0, 1, 0);
    float3 nearestColor = float3(0.2f, 0.45f, 0.95f);

    // CC0 blocky-character style: head, torso, arms, and legs as simple cuboids.
    TestAvatarPart(localOrigin, localDir, float3(-0.65f, 0.00f, -0.35f), float3(-0.08f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.08f, 0.00f, -0.35f), float3( 0.65f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-0.95f, 2.70f, -0.42f), float3( 0.95f, 5.35f, 0.42f), float3(0.18f, 0.55f, 0.95f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.55f, 2.45f, -0.34f), float3(-0.98f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.98f, 2.45f, -0.34f), float3( 1.55f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), tHit, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.05f, 5.25f, -0.58f), float3( 1.05f, 7.20f, 0.58f), float3(0.86f, 0.64f, 0.42f), tHit, nearestNormal, nearestColor);

    if (tHit >= 1e19f) {
        return false;
    }

    float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
    float lighting = max(dot(nearestNormal, lightDir), 0.25f);
    color = float4(nearestColor * lighting, 1.0f);
    return true;
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
    RayHit worldHit = Raymarch(cameraPos, rayDir);
    float4 voxelColor = worldHit.color;

    float avatarT;
    float4 avatarColor;
    if (RenderBlockCharacter(cameraPos, rayDir, avatarT, avatarColor) && avatarT < worldHit.distance) {
        voxelColor = avatarColor;
    }

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
