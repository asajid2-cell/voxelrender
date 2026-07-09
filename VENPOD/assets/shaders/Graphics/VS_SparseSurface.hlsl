#include "../Common/SharedTypes.hlsli"
#include "../Common/BitPacking.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

struct SparseSurfaceFace {
    int worldX;
    int worldY;
    int worldZ;
    uint payload;
};

StructuredBuffer<SparseSurfaceFace> SparseSurfaceFaces : register(t0);

struct SparseSurfaceRecord {
    int3 coord;
    uint firstFace;
    uint faceCount;
    uint flags;
    uint generation;
    int3 minVoxel;
    int3 maxVoxel;
};

struct SparseSurfaceClusterRecord {
    int3 minCoord;
    uint firstRecord;
    int3 maxCoord;
    uint recordCount;
    uint faceCount;
    uint flags;
};

StructuredBuffer<SparseSurfaceRecord> SparseSurfaceRecords : register(t2);
StructuredBuffer<SparseSurfaceClusterRecord> SparseSurfaceClusters : register(t3);

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint material : MATERIAL0;
    nointerpolation uint faceDirection : TEXCOORD2;
    nointerpolation uint debugMarker : TEXCOORD3;
    nointerpolation uint debugFaceExtent : TEXCOORD4;
    float distance : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float clipDistance : SV_ClipDistance0;
};

static const float kNearPlane = 0.05f;
static const float kFarPlane = 10000.0f;
static const uint kClusterDrawInstanceFlag = 0x80000000u;
static const uint kDebugWavePixelMode = 75u;
static const uint kDebugSurfaceUploadPixelMode = 76u;
static const uint kDebugMidMeshDirtyPixelMode = 77u;
static const uint kDebugMidMeshDirtyRasterOnlyMode = 78u;
static const uint kDebugSurfaceClipXzMode = 90u;
static const uint kDebugSurfaceClipBoundary3dMode = 91u;
static const uint kDebugSurfaceClipXzProofMode = 92u;
static const uint kDebugWavePixelFlag = 0x80000000u;
static const uint kDebugWavePixelPersistSerials = 8u;
static const uint kDebugMidMeshDirtyPixelPersistSerials = 64u;
static const uint kDebugSurfaceUploadPixelPersistSerials = 24u;
static const uint kSparseSurfaceDebugPassExact = 1u;
static const uint kDebugUploadStampValidBit = 0x00008000u;
static const uint kDebugUploadStampSerialMask = 0x7Fu;
static const uint kDebugUploadStampShift = 8u;

bool UseXzSurfaceClip() {
    if (frame.debugMode == kDebugSurfaceClipBoundary3dMode) {
        return false;
    }
    return frame.nearOwnershipParams.x > 0.5f ||
        frame.debugMode == kDebugSurfaceClipXzMode ||
        frame.debugMode == kDebugSurfaceClipXzProofMode;
}

uint FaceDirection(SparseSurfaceFace face) {
    return (face.payload >> 29u) & 0x7u;
}

uint FaceVoxel(SparseSurfaceFace face) {
    return face.payload & 0x0007FFFFu;
}

uint FaceWidth(SparseSurfaceFace face) {
    return ((face.payload >> 24u) & 0x1Fu) + 1u;
}

uint FaceHeight(SparseSurfaceFace face) {
    return ((face.payload >> 19u) & 0x1Fu) + 1u;
}

float3 FaceNormal(uint direction) {
    if (direction == 0u) return float3(-1.0f, 0.0f, 0.0f);
    if (direction == 1u) return float3(1.0f, 0.0f, 0.0f);
    if (direction == 2u) return float3(0.0f, -1.0f, 0.0f);
    if (direction == 3u) return float3(0.0f, 1.0f, 0.0f);
    if (direction == 4u) return float3(0.0f, 0.0f, -1.0f);
    return float3(0.0f, 0.0f, 1.0f);
}

float3 FaceCorner(SparseSurfaceFace face, uint corner) {
    const uint direction = FaceDirection(face);
    const float x0 = (float)face.worldX;
    const float y0 = (float)face.worldY;
    const float z0 = (float)face.worldZ;
    const float width = (float)FaceWidth(face);
    const float height = (float)FaceHeight(face);
    const float x1 = x0 + (direction == 2u || direction == 3u || direction == 4u || direction == 5u ? width : 1.0f);
    const float y1 = y0 + (direction == 0u || direction == 1u || direction == 4u || direction == 5u ? height : 1.0f);
    const float z1 =
        z0 + (direction == 0u || direction == 1u ? width :
              direction == 2u || direction == 3u ? height :
              1.0f);

    if (direction == 0u) {
        return corner == 0u ? float3(x0, y0, z1) :
            corner == 1u ? float3(x0, y1, z1) :
            corner == 2u ? float3(x0, y1, z0) :
                            float3(x0, y0, z0);
    }
    if (direction == 1u) {
        return corner == 0u ? float3(x1, y0, z0) :
            corner == 1u ? float3(x1, y1, z0) :
            corner == 2u ? float3(x1, y1, z1) :
                            float3(x1, y0, z1);
    }
    if (direction == 2u) {
        return corner == 0u ? float3(x0, y0, z0) :
            corner == 1u ? float3(x1, y0, z0) :
            corner == 2u ? float3(x1, y0, z1) :
                            float3(x0, y0, z1);
    }
    if (direction == 3u) {
        return corner == 0u ? float3(x0, y1, z1) :
            corner == 1u ? float3(x1, y1, z1) :
            corner == 2u ? float3(x1, y1, z0) :
                            float3(x0, y1, z0);
    }
    if (direction == 4u) {
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

float3 FaceCenter(SparseSurfaceFace face) {
    const uint direction = FaceDirection(face);
    const float x0 = (float)face.worldX;
    const float y0 = (float)face.worldY;
    const float z0 = (float)face.worldZ;
    const float width = (float)FaceWidth(face);
    const float height = (float)FaceHeight(face);

    if (direction == 0u) return float3(x0, y0 + height * 0.5f, z0 + width * 0.5f);
    if (direction == 1u) return float3(x0 + 1.0f, y0 + height * 0.5f, z0 + width * 0.5f);
    if (direction == 2u) return float3(x0 + width * 0.5f, y0, z0 + height * 0.5f);
    if (direction == 3u) return float3(x0 + width * 0.5f, y0 + 1.0f, z0 + height * 0.5f);
    if (direction == 4u) return float3(x0 + width * 0.5f, y0 + height * 0.5f, z0);
    return float3(x0 + width * 0.5f, y0 + height * 0.5f, z0 + 1.0f);
}

uint ResolveClusterFaceIndex(uint clusterIndex, uint localFaceIndex) {
    SparseSurfaceClusterRecord cluster = SparseSurfaceClusters[clusterIndex];
    uint faceBase = 0u;
    [loop]
    for (uint i = 0u; i < 64u; ++i) {
        if (i >= cluster.recordCount) {
            break;
        }
        SparseSurfaceRecord record = SparseSurfaceRecords[cluster.firstRecord + i];
        const uint nextBase = faceBase + record.faceCount;
        if (localFaceIndex < nextBase) {
            return record.firstFace + (localFaceIndex - faceBase);
        }
        faceBase = nextBase;
    }
    return 0u;
}

uint DebugWaveMarkerForFace(SparseSurfaceFace face, uint faceIndex) {
    const bool midMeshDebugMode =
        frame.debugMode == kDebugWavePixelMode ||
        frame.debugMode == kDebugMidMeshDirtyPixelMode ||
        frame.debugMode == kDebugMidMeshDirtyRasterOnlyMode;
    const bool exactSurfaceUploadDebugMode =
        frame.debugMode == kDebugSurfaceUploadPixelMode &&
        (uint)frame.surfaceRasterParams.w == kSparseSurfaceDebugPassExact;
    if (!midMeshDebugMode && !exactSurfaceUploadDebugMode) {
        return 0u;
    }
    const bool dirtyMidMeshRasterMode =
        frame.debugMode == kDebugMidMeshDirtyPixelMode ||
        frame.debugMode == kDebugMidMeshDirtyRasterOnlyMode;
    const uint persistSerials = dirtyMidMeshRasterMode
        ? kDebugMidMeshDirtyPixelPersistSerials
        : (exactSurfaceUploadDebugMode
            ? kDebugSurfaceUploadPixelPersistSerials
            : kDebugWavePixelPersistSerials);
    const uint recordCount = (uint)frame.surfaceParams.z;
    [loop]
    for (uint recordIndex = 0u; recordIndex < recordCount; ++recordIndex) {
        SparseSurfaceRecord record = SparseSurfaceRecords[recordIndex];
        const uint recordEnd = record.firstFace + record.faceCount;
        if (faceIndex < record.firstFace || faceIndex >= recordEnd) {
            continue;
        }
        if (!exactSurfaceUploadDebugMode && (record.flags & kDebugWavePixelFlag) == 0u) {
            return 0u;
        }
        const uint uploadedSerial = (uint)frame.surfaceParams.w;
        const uint age = uploadedSerial >= record.generation
            ? uploadedSerial - record.generation
            : 0xFFFFFFFFu;
        return age <= persistSerials ? 1u : 0u;
    }
    return 0u;
}

VSOutput main(uint faceVertex : FACEVERTEX, uint instanceId : SV_InstanceID) {
    VSOutput output;
    uint faceIndex = faceVertex / 4u;
    if ((instanceId & kClusterDrawInstanceFlag) != 0u) {
        faceIndex = ResolveClusterFaceIndex(instanceId & ~kClusterDrawInstanceFlag, faceIndex);
    }
    const uint vertexCorner = faceVertex & 3u;
    SparseSurfaceFace face = SparseSurfaceFaces[faceIndex];

    const float3 world = FaceCorner(face, vertexCorner);
    const float3 normal = FaceNormal(FaceDirection(face));
    const float3 faceCenter = FaceCenter(face);
    const float3 rel = world - frame.cameraPosition.xyz;
    const float viewX = dot(rel, frame.cameraRight.xyz);
    const float viewY = dot(rel, frame.cameraUp.xyz);
    const float viewZ = dot(rel, frame.cameraForward.xyz);
    const float tanHalfFov = tan(frame.cameraPosition.w * 0.5f);
    const float3 cameraToFace = faceCenter - frame.cameraPosition.xyz;
    const float frontFacing = dot(normal, -normalize(cameraToFace));
    const float surfaceMaxDistance = frame.nearOwnershipParams.w;
    const float surfaceMinDistance = max(frame.surfaceRasterParams.y, 0.0f);
    const uint surfaceDebugPassKind = (uint)frame.surfaceRasterParams.w;
    const float clipMetric3d = length(cameraToFace);
    const float clipMetricXz = length(cameraToFace.xz);
    const float vertexClipMetric3d = length(rel);
    const float vertexClipMetricXz = length(rel.xz);
    const bool useXzSurfaceClip = UseXzSurfaceClip();
    const float foregroundClipMetric = useXzSurfaceClip
        ? clipMetricXz
        : clipMetric3d;
    const float foregroundVertexClipMetric = useXzSurfaceClip
        ? vertexClipMetricXz
        : vertexClipMetric3d;
    const float foregroundDistanceClip =
        surfaceMaxDistance > 0.0f ? surfaceMaxDistance - foregroundClipMetric : 1.0f;
    const float foregroundNearExclusionClip =
        surfaceMinDistance > 0.0f ? foregroundVertexClipMetric - surfaceMinDistance : 1.0f;

    const float ndcDepth = (viewZ - kNearPlane) / (kFarPlane - kNearPlane);
    output.position = float4(
        viewX / max(tanHalfFov * frame.cameraForward.w, 0.001f),
        viewY / max(tanHalfFov, 0.001f),
        ndcDepth * viewZ,
        viewZ);
    output.normal = normal;
    output.faceDirection = FaceDirection(face);
    output.debugMarker = DebugWaveMarkerForFace(face, faceIndex);
    output.debugFaceExtent = max(FaceWidth(face), FaceHeight(face));
    output.material = GetMaterial(FaceVoxel(face));
    output.distance = max(viewZ, 0.0f);
    output.worldPos = world;
    output.clipDistance = min(
        min(viewZ - kNearPlane, frontFacing + 0.0001f),
        min(foregroundDistanceClip, foregroundNearExclusionClip));
    return output;
}
