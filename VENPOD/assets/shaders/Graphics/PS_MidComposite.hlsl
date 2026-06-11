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
    // TANDEM AA (replaces the earlier unsharp): the mid is RAYMARCHED coarse voxel
    // blocks (16-32u cells), so the offensive "pixelated vs real voxels" look is
    // edge ALIASING on the block stair-steps. The old 5-tap UNSHARP sharpened
    // those steps -> WORSE at native 1:1. Instead apply a cheap edge-aware FXAA-
    // style luma blend: a 5-tap box blur, applied only where local luma varies
    // (an edge), leaving flat material untouched. Outside the uber-shader, so no
    // driver/PSO risk. Alpha (mid coverage) is passed through untouched.
    uint texW, texH;
    MidColor.GetDimensions(texW, texH);
    const float2 texel = float2(1.0f / max(texW, 1u), 1.0f / max(texH, 1u));
    const float4 c = MidColor.Sample(MidSampler, uv);
    const float4 n = MidColor.Sample(MidSampler, uv - float2(0.0f, texel.y));
    const float4 s = MidColor.Sample(MidSampler, uv + float2(0.0f, texel.y));
    const float4 e = MidColor.Sample(MidSampler, uv + float2(texel.x, 0.0f));
    const float4 w = MidColor.Sample(MidSampler, uv - float2(texel.x, 0.0f));
    const float3 L = float3(0.299f, 0.587f, 0.114f);
    const float lc = dot(c.rgb, L);
    const float ln = dot(n.rgb, L), ls = dot(s.rgb, L), le = dot(e.rgb, L), lw = dot(w.rgb, L);
    const float lumaRange = max(lc, max(max(ln, ls), max(le, lw))) -
                            min(lc, min(min(ln, ls), min(le, lw)));
    const float3 aa = (n.rgb + s.rgb + e.rgb + w.rgb + c.rgb * 2.0f) / 6.0f;
    const float edge = smoothstep(0.025f, 0.10f, lumaRange);
    const float3 rgb = lerp(c.rgb, aa, edge * 0.45f);
    output.color = float4(saturate(rgb), c.a);
    return output;
}
