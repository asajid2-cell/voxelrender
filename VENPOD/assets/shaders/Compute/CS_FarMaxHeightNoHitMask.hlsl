#include "../Common/SharedTypes.hlsli"

// Conservative screen-space far-height no-hit mask.
//
// Pass 0 clears per-screen-tile horizon values.
// Pass 1 projects the conservative voxelized far max-height shell into those
// tiles, storing the smallest screen-space Y touched by the shell.
// Pass 2 writes the full-resolution mask: nonzero means only the final
// RaymarchFarTerrain tail may be skipped for this pixel. The value stores
// horizonY + 1 so the pixel shader can reconstruct a horizon-relative
// atmospheric fallback instead of a flat sky cutout. Any missing or uncertain
// tile writes 0 and falls back to the existing raymarch.

ConstantBuffer<FrameConstants> frame : register(b0);
StructuredBuffer<float> FarMaxHeightCache : register(t0);
RWTexture2D<uint> FarMaxHeightNoHitMask : register(u0);
RWStructuredBuffer<uint> FarScreenHorizonY : register(u1);

cbuffer FarScreenMaskParams : register(b1) {
    uint passIndex;
    uint tileCount;
    uint tileWidth;
    uint dilationPixelsBits;
    uint projectOffset;
    uint projectSide;
    uint projectCellSizeBits;
    uint reserved0;
};

static const uint FAR_HORIZON_EMPTY = 0xffffffffu;
// Match the offline screenmask verifier's conservative predicate:
// a pixel above the projected max-height shell can skip the exact far tail.
// The generator already bakes in world-space height pad plus screen-space
// dilation; a large extra screen band made the runtime mask reject nothing.
static const uint FAR_HORIZON_RAYMARCH_BAND_PIXELS = 2u;

bool ProjectFarShellPoint(float3 worldPos, out float2 pixel) {
    const float3 rel = worldPos - frame.cameraPosition.xyz;
    const float viewZ = dot(rel, frame.cameraForward.xyz);
    if (viewZ <= 1.0f) {
        pixel = 0.0f.xx;
        return false;
    }

    const float tanHalfFov = tan(frame.cameraPosition.w * 0.5f);
    const float viewX = dot(rel, frame.cameraRight.xyz);
    const float viewY = dot(rel, frame.cameraUp.xyz);
    const float ndcX = viewX / max(viewZ * tanHalfFov * frame.cameraForward.w, 1.0e-6f);
    const float ndcY = viewY / max(viewZ * tanHalfFov, 1.0e-6f);
    if (ndcX < -1.4f || ndcX > 1.4f || ndcY < -1.4f || ndcY > 1.4f) {
        pixel = 0.0f.xx;
        return false;
    }

    pixel.x = (ndcX * 0.5f + 0.5f) * (float)frame.viewportWidth;
    pixel.y = (0.5f - ndcY * 0.5f) * (float)frame.viewportHeight;
    return true;
}

void ProjectFarHeightCell(uint2 cell) {
    const uint side = max(1u, projectSide);
    if (cell.x >= side || cell.y >= side || tileCount == 0u) {
        return;
    }

    const float leafCellSize = max(1.0f, asfloat(projectCellSizeBits));
    const float2 origin = frame.farMaxHeightCacheParams.yz;
    const uint cacheIndex = projectOffset + cell.y * side + cell.x;
    const float maxHeight = FarMaxHeightCache[cacheIndex];
    const float2 xz0 = origin + (float2)cell * leafCellSize;
    const float2 xz1 = xz0 + leafCellSize;

    float minX = 1.0e20f;
    float maxX = -1.0e20f;
    float minY = 1.0e20f;
    bool anyProjected = false;

    float2 pixel;
    if (ProjectFarShellPoint(float3(xz0.x, maxHeight, xz0.y), pixel)) {
        minX = min(minX, pixel.x);
        maxX = max(maxX, pixel.x);
        minY = min(minY, pixel.y);
        anyProjected = true;
    }
    if (ProjectFarShellPoint(float3(xz1.x, maxHeight, xz0.y), pixel)) {
        minX = min(minX, pixel.x);
        maxX = max(maxX, pixel.x);
        minY = min(minY, pixel.y);
        anyProjected = true;
    }
    if (ProjectFarShellPoint(float3(xz0.x, maxHeight, xz1.y), pixel)) {
        minX = min(minX, pixel.x);
        maxX = max(maxX, pixel.x);
        minY = min(minY, pixel.y);
        anyProjected = true;
    }
    if (ProjectFarShellPoint(float3(xz1.x, maxHeight, xz1.y), pixel)) {
        minX = min(minX, pixel.x);
        maxX = max(maxX, pixel.x);
        minY = min(minY, pixel.y);
        anyProjected = true;
    }
    if (!anyProjected) {
        return;
    }

    const float dilation = asfloat(dilationPixelsBits);
    minX -= dilation;
    maxX += dilation;
    minY -= dilation;

    const int firstTile = max(0, (int)floor(minX / (float)max(1u, tileWidth)));
    const int lastTile = min((int)tileCount - 1, (int)floor(maxX / (float)max(1u, tileWidth)));
    if (lastTile < firstTile) {
        return;
    }

    const uint horizonY = (uint)clamp(
        (int)floor(minY),
        0,
        (int)frame.viewportHeight + 1);
    for (int tile = firstTile; tile <= lastTile; ++tile) {
        InterlockedMin(FarScreenHorizonY[(uint)tile], horizonY);
    }
}

void ClassifyMaskPixel(uint2 pixel) {
    if (pixel.x >= (uint)frame.viewportWidth || pixel.y >= (uint)frame.viewportHeight) {
        return;
    }
    if (tileCount == 0u || tileWidth == 0u) {
        FarMaxHeightNoHitMask[pixel] = 0u;
        return;
    }

    const uint tile = min(tileCount - 1u, pixel.x / tileWidth);
    const uint horizonY = FarScreenHorizonY[tile];
    const bool safelyAboveHorizon =
        horizonY != FAR_HORIZON_EMPTY &&
        pixel.y + FAR_HORIZON_RAYMARCH_BAND_PIXELS < horizonY;
    FarMaxHeightNoHitMask[pixel] = safelyAboveHorizon ? (horizonY + 1u) : 0u;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (passIndex == 0u) {
        if (dtid.y == 0u && dtid.x < tileCount) {
            FarScreenHorizonY[dtid.x] = FAR_HORIZON_EMPTY;
        }
        return;
    }

    if (passIndex == 1u) {
        ProjectFarHeightCell(dtid.xy);
        return;
    }

    ClassifyMaskPixel(dtid.xy);
}
