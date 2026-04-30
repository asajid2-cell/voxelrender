// =============================================================================
// VENPOD Persistent Edit Apply Shader
// Replays sparse world/chunk edit overlays into the local render buffer.
// =============================================================================

cbuffer EditApplyConstants : register(b0) {
    uint editCount;
    uint padding0;
    uint padding1;
    uint padding2;
};

// x = destination linear index in the current render buffer, y = packed voxel.
StructuredBuffer<uint2> EditInput : register(t0);
RWStructuredBuffer<uint> TargetVoxelBuffer : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint editIndex = dispatchThreadId.x;
    if (editIndex >= editCount) {
        return;
    }

    uint2 edit = EditInput[editIndex];
    TargetVoxelBuffer[edit.x] = edit.y;
}
