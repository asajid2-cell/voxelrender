// =============================================================================
// VENPOD Phase-B mid-voxel edit override scatter.
//
// Runs AFTER CS_GenerateMidVoxelBricks writes the PRISTINE samples for the edited
// bricks' slots, and BEFORE the raymarch reads the pool. Each thread owns one
// resolved edit override {destSlot, localIndex, voxel} -- the CPU already applied
// the tryEditedCellVoxel LOD rule (solid-wins / air-cluster threshold) per mid
// cell, so the GPU just scatters the result, overwriting that one voxel in the
// shared mid-voxel sample pool:
//
//     OutSamples[destSlot*4096 + localIndex] = voxel
//
// Branchless (no per-voxel loops, no inlining into the giant PS) so there is no
// DXC -O3 JIT cliff. A UAV barrier between the gen dispatch and this pass (emitted
// by MidVoxelGpuGenerator::ApplyEditOverrides) guarantees the pristine write lands
// first, so the override correctly REPLACES it. Air overrides (PackVoxel(Air)==0)
// carve; solid overrides paint. The raymarch recomputes neighbor surface exposure
// LIVE from the now-edited pool, so no VisualSurface flag reclassification is baked.
// =============================================================================

struct MidEditOverride {
    uint destSlot;
    uint localIndex;
    uint voxel;
    uint pad0;
};

cbuffer ApplyParams : register(b0) {
    uint4 misc;   // x = overrideCount, yzw = pad
};

StructuredBuffer<MidEditOverride> Overrides : register(t0);
RWStructuredBuffer<uint> OutSamples : register(u0);

#define SPARSE_BRICK_VOXEL_COUNT 4096u

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint idx = dtid.x;
    if (idx >= misc.x) {
        return;
    }
    const MidEditOverride ov = Overrides[idx];
    OutSamples[ov.destSlot * SPARSE_BRICK_VOXEL_COUNT + ov.localIndex] = ov.voxel;
}
