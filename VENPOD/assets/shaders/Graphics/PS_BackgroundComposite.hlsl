#include "../Common/SharedTypes.hlsli"

#ifndef VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR
#define VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR 0
#endif
#ifndef VENPOD_BACKGROUND_COMPOSITE_EDGE_AWARE
#define VENPOD_BACKGROUND_COMPOSITE_EDGE_AWARE 0
#endif

Texture2D<float4> BackgroundColor : register(t0);
SamplerState BackgroundSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_Target;
};

PSOutput main(PSInput input) {
    PSOutput output;
#if VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR
    output.color = float4(0.0f, 0.95f, 1.0f, 1.0f);
#else
    // Linear filter the low-res (0.3-scale) background when upscaling to the main
    // RT. The previous Load() point-sampled, which hard-upscaled ~576x324 -> 1080p
    // with nearest-neighbor = the blocky "static-TV" mid/far in perf mode. The
    // linear BackgroundSampler (s0) was declared but unused; use it.
    output.color = BackgroundColor.SampleLevel(BackgroundSampler, saturate(input.uv), 0.0f);
#if VENPOD_BACKGROUND_COMPOSITE_EDGE_AWARE
    // Horizon-only anti-bleed: the failing 0.25-scale captures had sparse dark
    // terrain bleeding upward into sky/horizon pixels. In the horizon band,
    // choose the brightest local low-res sample across a 4-neighbor footprint
    // when contrast is high. This is composite-only, so it preserves the
    // low-invocation performance path and is cheap to reject if visual gates fail.
    if (input.uv.y > 0.30f && input.uv.y < 0.45f) {
        uint texW;
        uint texH;
        BackgroundColor.GetDimensions(texW, texH);
        const float2 texel = 1.0f / max(float2((float)texW, (float)texH), 1.0f);
        float4 best = output.color;
        float bestLum = dot(best.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        const float4 sx0 = BackgroundColor.SampleLevel(
            BackgroundSampler, saturate(input.uv + float2(-texel.x, 0.0f)), 0.0f);
        const float4 sx1 = BackgroundColor.SampleLevel(
            BackgroundSampler, saturate(input.uv + float2(texel.x, 0.0f)), 0.0f);
        const float4 sy0 = BackgroundColor.SampleLevel(
            BackgroundSampler, saturate(input.uv + float2(0.0f, -texel.y)), 0.0f);
        const float4 sy1 = BackgroundColor.SampleLevel(
            BackgroundSampler, saturate(input.uv + float2(0.0f, texel.y)), 0.0f);
        const float4 samples[4] = { sx0, sx1, sy0, sy1 };
        [unroll]
        for (uint i = 0u; i < 4u; ++i) {
            const float lum = dot(samples[i].rgb, float3(0.2126f, 0.7152f, 0.0722f));
            if (lum > bestLum) {
                bestLum = lum;
                best = samples[i];
            }
        }
        const float centerLum = dot(output.color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (bestLum - centerLum > 0.05f) {
            output.color.rgb = best.rgb;
        }
    }
#endif
    output.color.a = 1.0f;
#endif
    return output;
}
