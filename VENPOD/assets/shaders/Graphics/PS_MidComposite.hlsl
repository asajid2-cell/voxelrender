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
    output.color = MidColor.Sample(MidSampler, saturate(input.uv));
    return output;
}
