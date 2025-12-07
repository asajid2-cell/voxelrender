// =============================================================================
// VENPOD Prepare Indirect Dispatch Args
// Takes the active chunk count and prepares indirect dispatch arguments
// =============================================================================

// Input: active chunk count
RWBuffer<uint> ActiveChunkCount : register(u0);

// Output: indirect dispatch arguments (threadGroupCountX, Y, Z)
RWBuffer<uint> IndirectArgs : register(u1);

// Single thread to prepare the args
[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint activeCount = ActiveChunkCount[0];

    // Each active chunk needs one thread group
    // We dispatch activeCount thread groups in X, 1 in Y and Z
    IndirectArgs[0] = activeCount;  // threadGroupCountX
    IndirectArgs[1] = 1;            // threadGroupCountY
    IndirectArgs[2] = 1;            // threadGroupCountZ
}
