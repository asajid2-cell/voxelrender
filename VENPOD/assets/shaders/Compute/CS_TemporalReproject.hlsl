#include "../Common/SharedTypes.hlsli"

// Stage 2b motion reprojection (v2, 2-pass depth-resolved forward-scatter). The previous frame's far
// history is scattered into the current frame by the camera delta. To stay ghost-free under arbitrary
// motion (incl. rotation that sweeps distant terrain across the screen), we resolve OVERLAPS by nearest
// distance: pass 1 InterlockedMins each current pixel's nearest reprojected distance; pass 2 writes the
// color only for the prev pixel that won. Disocclusions (newly revealed regions) get NO scatter -> the
// mask stays 1 -> they march. So reuse only ever shows the nearest real prior surface; anything
// uncertain is freshly marched. History alpha carries the far hit distance.
//
// Compile twice: REPROJECT_COLOR_PASS undefined = pass 1 (depth), defined = pass 2 (color).
ConstantBuffer<FrameConstants> frame : register(b0);
Texture2D<float4>   HistoryPrev  : register(t0);   // prev: rgb = color, a = far hit distance
RWTexture2D<uint>   ScatterDepth : register(u0);   // nearest reprojected distance (asuint), 0xFFFFFFFF = empty
#ifdef REPROJECT_COLOR_PASS
RWTexture2D<float4> HistoryCur   : register(u1);   // reuse pixels written here
RWTexture2D<uint>   MarchMask    : register(u2);   // 1 = march (default), 0 = reuse
#endif

static const float kMinReuseDist = 0.5f;
static const float kMaxReuseDist = 60000.0f;

float3 RayDirFor(float2 uv, float3 fwd, float3 right, float3 up, float fov, float aspect) {
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float tanHalf = tan(fov * 0.5f);
    return normalize(fwd + right * ndc.x * tanHalf * aspect + up * ndc.y * tanHalf);
}

// Reconstruct the prev-frame world hit for thread `tid`, reproject into the current camera, and return
// the current pixel + current radial distance. Returns false if this prev sample doesn't land a finite
// on-screen current pixel (sky/invalid/off-screen/behind) -> caller skips it (that pixel will march).
bool Reproject(uint3 tid, uint W, uint H, out int2 curPx, out float curDist) {
    curPx = int2(0, 0);
    curDist = 0.0f;
    const float4 prev = HistoryPrev.Load(int3(int2(tid.xy), 0));
    const float prevDist = prev.a;
    if (prevDist <= kMinReuseDist || prevDist >= kMaxReuseDist) {
        return false;
    }
    const float2 prevUV = (float2(tid.xy) + 0.5f) / float2(W, H);
    const float3 prevDir = RayDirFor(
        prevUV, frame.prevCameraForward.xyz, frame.prevCameraRight.xyz, frame.prevCameraUp.xyz,
        frame.prevCameraPosition.w, frame.prevCameraForward.w);
    const float3 world = frame.prevCameraPosition.xyz + prevDir * prevDist;

    const float3 rel = world - frame.cameraPosition.xyz;
    const float z = dot(rel, frame.cameraForward.xyz);
    if (z <= 1.0f) {
        return false;
    }
    const float fov = frame.cameraPosition.w;
    const float aspect = frame.cameraForward.w;
    const float tanHalf = tan(fov * 0.5f);
    const float ndcx = dot(rel, frame.cameraRight.xyz) / (z * tanHalf * aspect);
    const float ndcy = dot(rel, frame.cameraUp.xyz) / (z * tanHalf);
    if (abs(ndcx) > 1.0f || abs(ndcy) > 1.0f) {
        return false;
    }
    const float2 curUV = float2((ndcx + 1.0f) * 0.5f, (1.0f - ndcy) * 0.5f);
    curPx = int2(curUV * float2(W, H));
    if (curPx.x < 0 || curPx.y < 0 || curPx.x >= (int)W || curPx.y >= (int)H) {
        return false;
    }
    curDist = length(rel);   // radial distance from the current camera (matches history alpha units)
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint W = (uint)max(frame.viewportWidth, 1.0f);
    const uint H = (uint)max(frame.viewportHeight, 1.0f);
    if (tid.x >= W || tid.y >= H) {
        return;
    }
    int2 curPx;
    float curDist;
    if (!Reproject(tid, W, H, curPx, curDist)) {
        return;
    }
    const uint distBits = asuint(curDist);  // positive floats are monotonic as uint -> InterlockedMin works
#ifndef REPROJECT_COLOR_PASS
    // Pass 1: keep the NEAREST reprojected distance per current pixel.
    InterlockedMin(ScatterDepth[curPx], distBits);
#else
    // Pass 2: the prev sample that owns the nearest distance writes its color + marks reuse. (Ties on
    // identical distance both write near-identical content -> harmless.)
    if (distBits == ScatterDepth[curPx]) {
        const float4 prev = HistoryPrev.Load(int3(int2(tid.xy), 0));
        HistoryCur[curPx] = prev;
        MarchMask[curPx] = 0u;
    }
#endif
}
