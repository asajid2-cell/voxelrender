#include "../Common/SharedTypes.hlsli"
#include "../Common/BitPacking.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

struct SparseSurfaceFace {
    int worldX;
    int worldY;
    int worldZ;
    uint direction;
    uint voxel;
};

StructuredBuffer<SparseSurfaceFace> SparseSurfaceFaces : register(t0);

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint material : MATERIAL0;
    float distance : TEXCOORD0;
};

static const float kNearPlane = 0.05f;
static const float kFarPlane = 10000.0f;

float3 FaceNormal(uint direction) {
    if (direction == 0u) return float3(-1.0f, 0.0f, 0.0f);
    if (direction == 1u) return float3(1.0f, 0.0f, 0.0f);
    if (direction == 2u) return float3(0.0f, -1.0f, 0.0f);
    if (direction == 3u) return float3(0.0f, 1.0f, 0.0f);
    if (direction == 4u) return float3(0.0f, 0.0f, -1.0f);
    return float3(0.0f, 0.0f, 1.0f);
}

float3 FaceCorner(SparseSurfaceFace face, uint vertexInFace) {
    const uint corner = vertexInFace == 0u ? 0u :
        vertexInFace == 1u ? 1u :
        vertexInFace == 2u ? 2u :
        vertexInFace == 3u ? 0u :
        vertexInFace == 4u ? 2u : 3u;

    const float x0 = (float)face.worldX;
    const float y0 = (float)face.worldY;
    const float z0 = (float)face.worldZ;
    const float x1 = x0 + 1.0f;
    const float y1 = y0 + 1.0f;
    const float z1 = z0 + 1.0f;

    if (face.direction == 0u) {
        return corner == 0u ? float3(x0, y0, z1) :
            corner == 1u ? float3(x0, y1, z1) :
            corner == 2u ? float3(x0, y1, z0) :
                            float3(x0, y0, z0);
    }
    if (face.direction == 1u) {
        return corner == 0u ? float3(x1, y0, z0) :
            corner == 1u ? float3(x1, y1, z0) :
            corner == 2u ? float3(x1, y1, z1) :
                            float3(x1, y0, z1);
    }
    if (face.direction == 2u) {
        return corner == 0u ? float3(x0, y0, z0) :
            corner == 1u ? float3(x1, y0, z0) :
            corner == 2u ? float3(x1, y0, z1) :
                            float3(x0, y0, z1);
    }
    if (face.direction == 3u) {
        return corner == 0u ? float3(x0, y1, z1) :
            corner == 1u ? float3(x1, y1, z1) :
            corner == 2u ? float3(x1, y1, z0) :
                            float3(x0, y1, z0);
    }
    if (face.direction == 4u) {
        return corner == 0u ? float3(x1, y0, z0) :
            corner == 1u ? float3(x0, y0, z0) :
            corner == 2u ? float3(x0, y1, z0) :
                            float3(x1, y1, z0);
    }
    return corner == 0u ? float3(x0, y0, z1) :
        corner == 1u ? float3(x1, y0, z1) :
        corner == 2u ? float3(x1, y1, z1) :
                        float3(x0, y1, z1);
}

VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput output;
    const uint faceIndex = vertexId / 6u;
    const uint vertexInFace = vertexId - faceIndex * 6u;
    SparseSurfaceFace face = SparseSurfaceFaces[faceIndex];

    const float3 world = FaceCorner(face, vertexInFace);
    const float3 rel = world - frame.cameraPosition.xyz;
    const float viewX = dot(rel, frame.cameraRight.xyz);
    const float viewY = dot(rel, frame.cameraUp.xyz);
    const float viewZ = max(dot(rel, frame.cameraForward.xyz), kNearPlane);
    const float tanHalfFov = tan(frame.cameraPosition.w * 0.5f);

    const float ndcDepth = saturate((viewZ - kNearPlane) / (kFarPlane - kNearPlane));
    output.position = float4(
        viewX / max(tanHalfFov * frame.cameraForward.w, 0.001f),
        viewY / max(tanHalfFov, 0.001f),
        ndcDepth * viewZ,
        viewZ);
    output.normal = FaceNormal(face.direction);
    output.material = GetMaterial(face.voxel);
    output.distance = viewZ;
    return output;
}
