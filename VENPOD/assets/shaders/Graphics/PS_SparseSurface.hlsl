#include "../Common/SharedTypes.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

Texture1D<float4> MaterialPalette : register(t1);
SamplerState PaletteSampler : register(s0);
RWStructuredBuffer<uint> RenderOwnershipStats : register(u0);

static const uint RENDER_OWNER_SURFACE = 9u;
static const uint RENDER_OWNER_FRAME = 8u;

struct PSInput {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint material : MATERIAL0;
    float distance : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    if (frame.farFieldGridParams.w > 0.5f) {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_SURFACE], 1u);
        RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
    }

    const float u = ((float)input.material + 0.5f) / 256.0f;
    float3 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0).rgb;

    const float3 lightDir = normalize(float3(0.45f, 0.82f, 0.34f));
    const float diffuse = max(dot(normalize(input.normal), lightDir), 0.22f);
    float3 color = baseColor * diffuse;

    const float fog = saturate((input.distance - 1500.0f) / 4500.0f);
    const float3 sky = float3(0.52f, 0.67f, 0.84f);
    color = lerp(color, sky, fog * 0.55f);
    return float4(color, 1.0f);
}
