// VENPOD sparse surface GPU cull/compact pass.
// Converts stable per-brick surface records into a compact indirect draw list.

struct SparseSurfaceRecord {
    int3 coord;
    uint firstFace;
    uint faceCount;
    uint flags;
    uint generation;
    int3 minVoxel;
    int3 maxVoxel;
};

struct SparseSurfaceDrawArgs {
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};

struct SparseSurfaceClusterRecord {
    int3 minCoord;
    uint firstRecord;
    int3 maxCoord;
    uint recordCount;
    uint faceCount;
    uint flags;
};

cbuffer CullConstants : register(b0) {
    float4 cameraPosition; // xyz, fovY radians
    float4 cameraForward;  // xyz, aspect
    float4 cameraRight;
    float4 cameraUp;
    float4 params;         // recordCount, maxDrawCommands, maxDistance, padding
    float4 clusterParams;  // fastAcceptMaxRecords, fastAcceptMaxFaces, unused, unused
}

StructuredBuffer<SparseSurfaceRecord> SurfaceRecords : register(t0);
StructuredBuffer<SparseSurfaceClusterRecord> SurfaceClusters : register(t1);
RWStructuredBuffer<SparseSurfaceDrawArgs> DrawArgsOut : register(u0);
RWStructuredBuffer<uint> DrawStats : register(u1);

static const uint kRecordValid = 1u;
static const float kBrickSize = 16.0f;
static const float kCullRadius = 13.85640646f; // sqrt(3) * 16 / 2

static const uint kStatAccepted = 0u;
static const uint kStatRejectInvalid = 1u;
static const uint kStatRejectDistance = 2u;
static const uint kStatRejectFrustum = 3u;
static const uint kStatOverflow = 4u;
static const uint kStatCandidates = 5u;
static const uint kStatMaxDrawCommands = 6u;
static const uint kStatRejectCluster = 7u;
static const uint kStatClusterFastAccept = 8u;
static const uint kStatClusterDraws = 9u;
static const uint kStatRecordDraws = 10u;
static const uint kStatRejectBackface = 11u;

static const uint kClusterOutside = 0u;
static const uint kClusterIntersect = 1u;
static const uint kClusterInside = 2u;
static const uint kClusterBackface = 3u;
static const uint kClusterDrawInstanceFlag = 0x80000000u;
static const uint kRecordDirectionMaskShift = 8u;
static const uint kRecordDirectionMaskBits = 0x3Fu;

bool DirectionMaskFacesCamera(uint directionMask, float3 toCamera) {
    if (directionMask == 0u) {
        return true;
    }

    // Conservative normal cone test for whole-brick records. A direction bit
    // means at least one exposed face with that normal exists in the record.
    // Keep grazing cases; reject only when every recorded normal points away.
    const float epsilon = 0.25f;
    if ((directionMask & (1u << 0u)) != 0u && -toCamera.x >= -epsilon) return true; // -X
    if ((directionMask & (1u << 1u)) != 0u &&  toCamera.x >= -epsilon) return true; // +X
    if ((directionMask & (1u << 2u)) != 0u && -toCamera.y >= -epsilon) return true; // -Y
    if ((directionMask & (1u << 3u)) != 0u &&  toCamera.y >= -epsilon) return true; // +Y
    if ((directionMask & (1u << 4u)) != 0u && -toCamera.z >= -epsilon) return true; // -Z
    if ((directionMask & (1u << 5u)) != 0u &&  toCamera.z >= -epsilon) return true; // +Z
    return false;
}

uint ClassifyClusterCull(SparseSurfaceClusterRecord cluster) {
    if (cluster.recordCount == 0u) {
        return kClusterOutside;
    }

    const float3 brickMin = float3(cluster.minCoord);
    const float3 brickMax = float3(cluster.maxCoord);
    const float3 center = (brickMin + brickMax) * 0.5f;
    const float3 extent = max((brickMax - brickMin) * 0.5f, float3(1.0f, 1.0f, 1.0f));
    const float radius = length(extent);
    const float3 rel = center - cameraPosition.xyz;
    const float viewZ = dot(rel, cameraForward.xyz);
    const float maxDistance = max(params.z, 1.0f) + params.w + radius;
    if (dot(rel, rel) > maxDistance * maxDistance) {
        return kClusterOutside;
    }
    if (viewZ < -radius || viewZ > maxDistance) {
        return kClusterOutside;
    }

    const float tanHalfFov = tan(cameraPosition.w * 0.5f);
    const float aspect = max(cameraForward.w, 0.001f);
    const float viewX = dot(rel, cameraRight.xyz);
    const float viewY = dot(rel, cameraUp.xyz);
    const float xLimit = max(viewZ, 0.0f) * tanHalfFov * aspect + params.w + radius;
    const float yLimit = max(viewZ, 0.0f) * tanHalfFov + params.w + radius;
    if (abs(viewX) > xLimit || abs(viewY) > yLimit) {
        return kClusterOutside;
    }
    const uint directionMask = (cluster.flags >> kRecordDirectionMaskShift) & kRecordDirectionMaskBits;
    if (!DirectionMaskFacesCamera(directionMask, cameraPosition.xyz - center)) {
        return kClusterBackface;
    }

    const float insideMaxDistance = max(params.z, 1.0f) + params.w - radius;
    const float insideXLimit = max(viewZ, 0.0f) * tanHalfFov * aspect + params.w - radius;
    const float insideYLimit = max(viewZ, 0.0f) * tanHalfFov + params.w - radius;
    if (viewZ >= radius &&
        viewZ <= insideMaxDistance &&
        insideXLimit > 0.0f &&
        insideYLimit > 0.0f &&
        abs(viewX) <= insideXLimit &&
        abs(viewY) <= insideYLimit) {
        return kClusterInside;
    }
    return kClusterIntersect;
}

uint ClassifyRecordCull(SparseSurfaceRecord record) {
    if ((record.flags & kRecordValid) == 0u || record.faceCount == 0u) {
        return kStatRejectInvalid;
    }

    const float3 brickMin = float3(record.minVoxel);
    const float3 brickMax = float3(record.maxVoxel);
    const float3 center = (brickMin + brickMax) * 0.5f;
    const float radius = max(length(max((brickMax - brickMin) * 0.5f, float3(0.5f, 0.5f, 0.5f))), 0.5f);
    const float3 rel = center - cameraPosition.xyz;
    const float viewZ = dot(rel, cameraForward.xyz);
    const float maxDistance = max(params.z, 1.0f) + params.w + radius;
    if (dot(rel, rel) > maxDistance * maxDistance) {
        return kStatRejectDistance;
    }
    if (viewZ < -radius || viewZ > maxDistance) {
        return kStatRejectDistance;
    }

    const float tanHalfFov = tan(cameraPosition.w * 0.5f);
    const float aspect = max(cameraForward.w, 0.001f);
    const float viewX = dot(rel, cameraRight.xyz);
    const float viewY = dot(rel, cameraUp.xyz);
    const float xLimit = max(viewZ, 0.0f) * tanHalfFov * aspect + params.w + radius;
    const float yLimit = max(viewZ, 0.0f) * tanHalfFov + params.w + radius;
    if (abs(viewX) > xLimit || abs(viewY) > yLimit) {
        return kStatRejectFrustum;
    }
    const uint directionMask = (record.flags >> kRecordDirectionMaskShift) & kRecordDirectionMaskBits;
    if (!DirectionMaskFacesCamera(directionMask, cameraPosition.xyz - center)) {
        return kStatRejectBackface;
    }
    return kStatAccepted;
}

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    const uint clusterIndex = groupId.x;
    const uint localIndex = groupThreadId.x;
    const uint recordCount = (uint)params.x;
    const uint maxDrawCommands = (uint)params.y;
    if (clusterIndex == 0u && localIndex == 0u) {
        DrawStats[kStatCandidates] = recordCount;
        DrawStats[kStatMaxDrawCommands] = maxDrawCommands;
    }

    SparseSurfaceClusterRecord cluster = SurfaceClusters[clusterIndex];
    const uint clusterCullClass = ClassifyClusterCull(cluster);
    if (clusterCullClass == kClusterOutside) {
        if (localIndex == 0u) {
            InterlockedAdd(DrawStats[kStatRejectCluster], cluster.recordCount);
        }
        return;
    }
    if (clusterCullClass == kClusterBackface) {
        if (localIndex == 0u) {
            InterlockedAdd(DrawStats[kStatRejectBackface], cluster.recordCount);
        }
        return;
    }

    const bool allowClusterFastAccept =
        clusterCullClass == kClusterInside &&
        cluster.recordCount <= (uint)clusterParams.x &&
        cluster.faceCount <= (uint)clusterParams.y;
    if (allowClusterFastAccept) {
        if (localIndex == 0u) {
            if (cluster.faceCount == 0u) {
                InterlockedAdd(DrawStats[kStatRejectInvalid], cluster.recordCount);
                return;
            }

            uint outIndex = 0u;
            InterlockedAdd(DrawStats[kStatAccepted], 1u, outIndex);
            if (outIndex >= maxDrawCommands) {
                InterlockedAdd(DrawStats[kStatOverflow], 1u);
                return;
            }

            SparseSurfaceDrawArgs args;
            args.indexCountPerInstance = cluster.faceCount * 6u;
            args.instanceCount = 1u;
            args.startIndexLocation = 0u;
            args.baseVertexLocation = 0;
            args.startInstanceLocation = kClusterDrawInstanceFlag | clusterIndex;
            DrawArgsOut[outIndex] = args;
            InterlockedAdd(DrawStats[kStatClusterFastAccept], cluster.recordCount);
            InterlockedAdd(DrawStats[kStatClusterDraws], 1u);
        }
        return;
    }

    if (localIndex >= cluster.recordCount) {
        return;
    }
    const uint recordIndex = cluster.firstRecord + localIndex;
    if (recordIndex >= recordCount) {
        return;
    }

    SparseSurfaceRecord record = SurfaceRecords[recordIndex];
    uint cullClass = ClassifyRecordCull(record);
    if (cullClass != kStatAccepted) {
        InterlockedAdd(DrawStats[cullClass], 1u);
        return;
    }

    uint outIndex = 0u;
    InterlockedAdd(DrawStats[kStatAccepted], 1u, outIndex);
    if (outIndex >= maxDrawCommands) {
        InterlockedAdd(DrawStats[kStatOverflow], 1u);
        return;
    }

    SparseSurfaceDrawArgs args;
    args.indexCountPerInstance = record.faceCount * 6u;
    args.instanceCount = 1u;
    args.startIndexLocation = record.firstFace * 6u;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0u;
    DrawArgsOut[outIndex] = args;
    InterlockedAdd(DrawStats[kStatRecordDraws], 1u);
}
