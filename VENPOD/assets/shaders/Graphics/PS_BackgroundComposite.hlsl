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
    uint backgroundWidth = 1;
    uint backgroundHeight = 1;
    BackgroundColor.GetDimensions(backgroundWidth, backgroundHeight);
    uint2 texel = min(
        (uint2)floor(saturate(input.uv) * float2(backgroundWidth, backgroundHeight)),
        uint2(backgroundWidth - 1, backgroundHeight - 1));
    output.color = BackgroundColor.Load(int3(texel, 0));
    output.color.a = 1.0f;
#endif
    return output;
}
