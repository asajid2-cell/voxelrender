// =============================================================================
// VENPOD Background Temporal Accumulation (TAA lane increment 2)
//
// Drawn fullscreen into the still-RENDER_TARGET background color buffer right
// after the background raymarch, with SrcAlpha/InvSrcAlpha blending:
//   out = alpha * reprojectedHistory + (1 - alpha) * freshBackground
// The renderer then copies the blended result into the history buffer, closing
// the accumulation loop.
//
// Reprojection is rotation-exact / translation-approximate: each pixel's world
// DIRECTION is looked up in the previous frame's camera basis, treating the
// background as distant. That matches the measured shimmer (subpixel
// re-aliasing of the mid/far band under camera motion); a depth-aware
// reprojection is the follow-up increment if translation ghosting shows.
//
// Ray convention mirrors PS_Raymarch exactly:
//   ndc = uv * 2 - 1; ndc.y = -ndc.y
//   dir = normalize(fwd + right * ndc.x * tan(fov/2) * aspect
//                       + up    * ndc.y * tan(fov/2))
// =============================================================================

Texture2D<float4> HistoryColor : register(t0);
SamplerState HistorySampler : register(s0);

cbuffer TemporalConstants : register(b0) {
    float4 currPosFov;      // xyz = camera position, w = fov (fed to tan(fov*0.5) verbatim)
    float4 currFwdAspect;   // xyz = forward, w = aspect ratio
    float4 currRight;       // xyz = right
    float4 currUp;          // xyz = up
    float4 prevPosFov;
    float4 prevFwdAspect;
    float4 prevRight;
    float4 prevUp;
    float4 blendParams;     // x = history weight (0 disables), yzw unused
};

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    // Reconstruct this pixel's world direction with the current camera.
    float2 ndc = input.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float tanHalfFov = tan(currPosFov.w * 0.5f);
    float3 dir = normalize(
        currFwdAspect.xyz +
        currRight.xyz * ndc.x * tanHalfFov * currFwdAspect.w +
        currUp.xyz * ndc.y * tanHalfFov);

    // Where did the previous frame's camera see this direction?
    float prevZ = dot(dir, prevFwdAspect.xyz);
    if (prevZ <= 1e-4f) {
        // Behind the previous camera: no usable history.
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float prevTanHalfFov = tan(prevPosFov.w * 0.5f);
    float prevNdcX = dot(dir, prevRight.xyz) / (prevZ * prevTanHalfFov * prevFwdAspect.w);
    float prevNdcY = dot(dir, prevUp.xyz) / (prevZ * prevTanHalfFov);
    float2 prevUv = float2(prevNdcX * 0.5f + 0.5f, -prevNdcY * 0.5f + 0.5f);
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f)) {
        // Reprojected off-screen (screen edge revealed by rotation): keep fresh.
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float4 history = HistoryColor.SampleLevel(HistorySampler, prevUv, 0.0f);
    return float4(history.rgb, blendParams.x);
}
