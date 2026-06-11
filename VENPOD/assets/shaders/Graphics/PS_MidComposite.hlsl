#include "../Common/SharedTypes.hlsli"

// Upscale-composite for the low-resolution "mid" raymarch pass. The mid pass
// renders into a smaller R8G8B8A8 target where alpha encodes mid-terrain
// coverage (alpha = 1 on a mid hit, alpha = 0 on a miss). This shader samples
// that target with BILINEAR filtering for a smooth upscale and outputs the
// rgba AS-IS so the pipeline's alpha-over blend preserves coverage: alpha 0
// leaves the full pass untouched, alpha 1 shows the mid terrain.
Texture2D<float4> MidColor : register(t0);
SamplerState MidSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_Target;
};

PSOutput main(PSInput input) {
    PSOutput output;
    const float2 uv = saturate(input.uv);
    // TANDEM sharpness fix: the mid pass renders at half-res (e.g. 480x270) and
    // bilinear-upscales, which blurs the mid terrain (the dominant "fuzzy mid" vs
    // the crisp full-res near). Recover edge definition with a cheap 5-tap unsharp
    // mask (center minus the 4-neighbor average). Outside the uber-shader, so no
    // driver/PSO risk. Alpha (mid coverage) is passed through untouched so the
    // alpha-over composite still works.
    uint texW, texH;
    MidColor.GetDimensions(texW, texH);
    const float2 texel = float2(1.0f / max(texW, 1u), 1.0f / max(texH, 1u));
    const float4 c = MidColor.Sample(MidSampler, uv);
    const float3 neighbors =
        MidColor.Sample(MidSampler, uv + float2(texel.x, 0.0f)).rgb +
        MidColor.Sample(MidSampler, uv - float2(texel.x, 0.0f)).rgb +
        MidColor.Sample(MidSampler, uv + float2(0.0f, texel.y)).rgb +
        MidColor.Sample(MidSampler, uv - float2(0.0f, texel.y)).rgb;
    const float3 sharpened = c.rgb + (c.rgb - neighbors * 0.25f) * 0.55f;
    output.color = float4(saturate(sharpened), c.a);
    return output;
}
