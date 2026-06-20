// Builds fixed-slot indirect draw args for GPU-extracted mid-mesh tiles.
// Slot N reads count/status/commit[N] and emits either a one-instance sparse
// surface draw over production faces at N*faceCapacity, or a zeroed command.

struct SparseSurfaceDrawArgs {
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};

cbuffer DrawArgsConstants : register(b0) {
    uint gSlotCount;
    uint gFaceCapacityPerSlot;
    uint gReserved0;
    uint gReserved1;
}

StructuredBuffer<uint> FaceCounts : register(t0);
StructuredBuffer<uint> FaceStatuses : register(t1);
StructuredBuffer<uint> CommitFlags : register(t2);
RWStructuredBuffer<SparseSurfaceDrawArgs> DrawArgsOut : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint slot = dispatchThreadId.x;
    if (slot >= gSlotCount) {
        return;
    }

    SparseSurfaceDrawArgs args;
    args.indexCountPerInstance = 0u;
    args.instanceCount = 0u;
    args.startIndexLocation = 0u;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0u;

    const uint count = FaceCounts[slot];
    const bool committed =
        CommitFlags[slot] != 0u &&
        FaceStatuses[slot] == 0u &&
        count != 0u &&
        count <= gFaceCapacityPerSlot;
    if (committed) {
        args.indexCountPerInstance = count * 6u;
        args.instanceCount = 1u;
        args.startIndexLocation = slot * gFaceCapacityPerSlot * 6u;
    }

    DrawArgsOut[slot] = args;
}
