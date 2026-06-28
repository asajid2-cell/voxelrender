#include "../Common/SharedTypes.hlsli"

ConstantBuffer<FrameConstants> frame : register(b0);
StructuredBuffer<uint> FarScreenHorizonY : register(t0);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float FarSkySmooth01(float value) {
    value = saturate(value);
    return value * value * (3.0f - 2.0f * value);
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
    const float horizonAirBand = saturate((0.30f - abs(rayDir.y)) / 0.30f);
    const float3 horizonAir = float3(0.80f, 0.84f, 0.88f);
    sky = lerp(sky, horizonAir, horizonAirBand * horizonAirBand * 0.38f);
    sky += float3(1.00f, 0.72f, 0.34f) * sunBloom * 0.32f;
    sky += float3(1.00f, 0.93f, 0.74f) * sun * 1.75f;
    sky += float3(0.18f, 0.24f, 0.38f) * antiSun * 0.10f;

    return saturate(sky);
}

float3 ComputeRayDir(float2 uv) {
    const float3 forward = frame.cameraForward.xyz;
    const float3 right = frame.cameraRight.xyz;
    const float3 up = frame.cameraUp.xyz;
    const float fov = frame.cameraPosition.w;
    const float aspectRatio = frame.cameraForward.w;

    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    const float tanHalfFov = tan(fov * 0.5f);
    return normalize(
        forward +
        right * ndc.x * tanHalfFov * aspectRatio +
        up * ndc.y * tanHalfFov);
}

bool FarSkyOwnerBaseEligible() {
    const uint sparseNearFlags = (uint)round(frame.sparseNearParams.w);
    const bool sparseSurfaceAuthoritative =
        frame.sparseNearParams.x > 0.5f && ((sparseNearFlags & 2u) != 0u);
    const bool voxelTerrainOnly = ((sparseNearFlags & 16u) != 0u);
    const bool aboveWater = frame.cameraPosition.y > -49.0f;

    return
        frame.debugMode == 0u &&
        aboveWater &&
        sparseSurfaceAuthoritative &&
        voxelTerrainOnly &&
        frame.cameraPosition.y <= 384.0f;
}

bool FarSkyOwnerClassifies(float3 rayDir) {
    const float minY = frame.surfaceRasterParams.w;
    return
        FarSkyOwnerBaseEligible() &&
        rayDir.y > minY;
}

bool FarSkyOwnerClassifiesHorizon(float3 rayDir, float2 pixel, float minY) {
    const bool horizonReady = frame.farMaxHeightCacheParams2.w > 0.5f;
    if (!horizonReady ||
        !FarSkyOwnerBaseEligible() ||
        rayDir.y <= minY) {
        return false;
    }

    static const uint FAR_HORIZON_EMPTY = 0xffffffffu;
    static const uint FAR_HORIZON_TILE_WIDTH = 8u;
    static const uint FAR_HORIZON_OWNER_BAND_PIXELS = 2u;
    const uint tileCount = ((uint)frame.viewportWidth + FAR_HORIZON_TILE_WIDTH - 1u) / FAR_HORIZON_TILE_WIDTH;
    if (tileCount == 0u) {
        return false;
    }

    const uint tile = min(tileCount - 1u, (uint)pixel.x / FAR_HORIZON_TILE_WIDTH);
    const uint horizonY = FarScreenHorizonY[tile];
    return
        horizonY != FAR_HORIZON_EMPTY &&
        (uint)pixel.y + FAR_HORIZON_OWNER_BAND_PIXELS < horizonY;
}

float4 main(VSOutput input) : SV_Target {
    const float3 rayDir = ComputeRayDir(input.uv);
    const float horizonState = frame.farMaxHeightCacheParams2.w;
    const bool horizonMode = horizonState > 0.5f;
    const bool horizonOnlyMode = horizonState > 1.5f;
    // Default mode preserves the prior union of raw minY and projected horizon ownership.
    // Horizon-only mode is a conservative validation path: skip the flat threshold owner so
    // anything not proven by the projected max-height horizon falls back to the raymarch.
    const float conservativeHorizonMinY = 0.006f;
    const bool ownsPixel = horizonMode
        ? ((!horizonOnlyMode && FarSkyOwnerClassifies(rayDir)) ||
           FarSkyOwnerClassifiesHorizon(rayDir, input.position.xy, conservativeHorizonMinY))
        : FarSkyOwnerClassifies(rayDir);
    if (!ownsPixel) {
        discard;
    }
    return float4(SkyColor(rayDir), 1.0f);
}
