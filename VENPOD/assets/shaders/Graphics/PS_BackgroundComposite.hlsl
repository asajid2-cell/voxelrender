#include "../Common/SharedTypes.hlsli"

#ifndef VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR
#define VENPOD_BACKGROUND_COMPOSITE_FORCE_COLOR 0
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
    output.color.a = 1.0f;
#endif
    return output;
}
