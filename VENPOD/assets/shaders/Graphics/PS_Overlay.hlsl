#include "../Common/SharedTypes.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

Texture1D<float4> MaterialPalette : register(t1);
SamplerState PaletteSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_Target;
};

bool IntersectOverlayBox(
    float3 localOrigin,
    float3 localDir,
    float3 boxMin,
    float3 boxMax,
    out float tHit,
    out float3 normal)
{
    float3 invDir = 1.0f / max(abs(localDir), 0.0001f) * sign(localDir);
    float3 t0 = (boxMin - localOrigin) * invDir;
    float3 t1 = (boxMax - localOrigin) * invDir;
    float3 tMin3 = min(t0, t1);
    float3 tMax3 = max(t0, t1);
    float tNear = max(max(tMin3.x, tMin3.y), tMin3.z);
    float tFar = min(min(tMax3.x, tMax3.y), tMax3.z);

    if (tFar < max(tNear, 0.0f)) {
        tHit = 0.0f;
        normal = float3(0, 1, 0);
        return false;
    }

    tHit = tNear > 0.0f ? tNear : tFar;
    float3 p = localOrigin + localDir * tHit;
    float3 dMin = abs(p - boxMin);
    float3 dMax = abs(p - boxMax);
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
    if (IntersectOverlayBox(localOrigin, localDir, boxMin, boxMax, t, normal) && t < nearestT) {
        nearestT = t;
        nearestNormal = normal;
        nearestColor = color;
    }
}

float4 RenderBlockCharacterOverlay(float3 rayOrigin, float3 rayDir) {
    if (frame.characterPosition.w < 0.5f) {
        return float4(0, 0, 0, 0);
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

    float nearestT = 1e20f;
    float3 nearestNormal = float3(0, 1, 0);
    float3 nearestColor = float3(0.2f, 0.45f, 0.95f);

    TestAvatarPart(localOrigin, localDir, float3(-0.65f, 0.00f, -0.35f), float3(-0.08f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), nearestT, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.08f, 0.00f, -0.35f), float3( 0.65f, 2.90f, 0.35f), float3(0.12f, 0.22f, 0.82f), nearestT, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-0.95f, 2.70f, -0.42f), float3( 0.95f, 5.35f, 0.42f), float3(0.18f, 0.55f, 0.95f), nearestT, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.55f, 2.45f, -0.34f), float3(-0.98f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), nearestT, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3( 0.98f, 2.45f, -0.34f), float3( 1.55f, 5.10f, 0.34f), float3(0.78f, 0.55f, 0.36f), nearestT, nearestNormal, nearestColor);
    TestAvatarPart(localOrigin, localDir, float3(-1.05f, 5.25f, -0.58f), float3( 1.05f, 7.20f, 0.58f), float3(0.86f, 0.64f, 0.42f), nearestT, nearestNormal, nearestColor);

    if (nearestT >= 1e19f) {
        return float4(0, 0, 0, 0);
    }

    float3 lightDir = normalize(float3(0.5f, 1.0f, 0.3f));
    float lighting = max(dot(nearestNormal, lightDir), 0.25f);
    return float4(nearestColor * lighting, 1.0f);
}

float4 RenderBrushPreviewOverlay(float3 rayOrigin, float3 rayDir) {
    if (frame.brushParams.z < 0.5f) {
        return float4(0, 0, 0, 0);
    }

    float3 brushCenter = frame.brushPosition.xyz;
    float brushRadius = frame.brushPosition.w;
    uint brushMaterial = (uint)frame.brushParams.x;
    uint brushShape = (uint)frame.brushParams.y;

    float3 toBrush = brushCenter - rayOrigin;
    float distToCenter = length(toBrush);
    if (brushRadius <= 0.0f || distToCenter < max(brushRadius * 3.75f, 12.0f)) {
        return float4(0, 0, 0, 0);
    }

    float3 brushDir = toBrush / max(distToCenter, 0.001f);
    if (dot(brushDir, rayDir) <= 0.0f) {
        return float4(0, 0, 0, 0);
    }

    const float angularRadius = asin(saturate(brushRadius / max(distToCenter, 0.001f)));
    if (angularRadius > 0.34f) {
        return float4(0, 0, 0, 0);
    }

    float3 oc = -toBrush;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - brushRadius * brushRadius;
    float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return float4(0, 0, 0, 0);
    }

    float t = -b - sqrt(discriminant);
    if (t < 0.0f) {
        t = -b + sqrt(discriminant);
    }
    if (t < 2.0f) {
        return float4(0, 0, 0, 0);
    }

    float u = (float(brushMaterial) + 0.5f) / 256.0f;
    float3 materialColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0).rgb;
    if (brushShape != 0u) {
        return float4(materialColor, 0.25f);
    }

    float3 hitPoint = rayOrigin + rayDir * t;
    float dist = length(hitPoint - brushCenter);
    float normalizedDist = dist / brushRadius;
    float edgeFactor = abs(normalizedDist - 0.95f) < 0.05f ? 0.6f : 0.15f;
    float3 normal = normalize(hitPoint - brushCenter);
    float fresnel = pow(1.0f - abs(dot(normal, rayDir)), 2.0f);
    float alpha = lerp(edgeFactor, 0.4f, fresnel);
    return float4(materialColor, alpha);
}

PSOutput main(PSInput input) {
    PSOutput output;
    output.color = float4(0, 0, 0, 0);

    float3 cameraPos = frame.cameraPosition.xyz;
    float3 forward = frame.cameraForward.xyz;
    float3 right = frame.cameraRight.xyz;
    float3 up = frame.cameraUp.xyz;
    float fov = frame.cameraPosition.w;
    float aspectRatio = frame.cameraForward.w;

    float2 ndc = input.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float tanHalfFov = tan(fov * 0.5f);
    float3 rayDir = normalize(forward + right * ndc.x * tanHalfFov * aspectRatio + up * ndc.y * tanHalfFov);

    float4 avatar = RenderBlockCharacterOverlay(cameraPos, rayDir);
    float4 brush = RenderBrushPreviewOverlay(cameraPos, rayDir);

    float3 color = avatar.rgb;
    float alpha = avatar.a;
    if (brush.a > 0.0f) {
        color = lerp(color, brush.rgb, brush.a);
        alpha = saturate(alpha + brush.a * (1.0f - alpha));
    }

    output.color = float4(color, alpha);
    return output;
}
