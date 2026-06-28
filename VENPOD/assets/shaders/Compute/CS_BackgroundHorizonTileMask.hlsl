Texture2D<float4> BackgroundColor : register(t0);
RWStructuredBuffer<uint> RenderOwnershipStats : register(u0);
RWTexture2D<uint> HorizonTileMask : register(u1);
RWStructuredBuffer<uint2> HorizonTileList : register(u2);
RWStructuredBuffer<uint> HorizonTileDrawArgsWords : register(u3);

cbuffer HorizonTileMaskParams : register(b0) {
    uint fullWidth;
    uint fullHeight;
    uint tileSize;
    uint thresholdBits;
    uint y0;
    uint y1;
    uint frameIndex;
    uint selectorMode;
};

static const uint HORIZON_TILE_MASK_TILES = 49u;
static const uint HORIZON_TILE_MASK_TOTAL_TILES = 50u;
static const uint HORIZON_TILE_MASK_PIXEL_UPPER = 51u;
static const uint HORIZON_TILE_MASK_MAX_EDGE_255 = 52u;
static const uint HORIZON_TILE_MASK_FRAME = 53u;
static const uint HORIZON_TILE_MASK_BAND_TILES = 54u;
static const uint HORIZON_TILE_LIST_COUNT = 55u;
static const uint HORIZON_TILE_DRAW_INSTANCES = 56u;

float Luma(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint safeTile = max(1u, tileSize);
    const uint tilesX = (max(1u, fullWidth) + safeTile - 1u) / safeTile;
    const uint tilesY = (max(1u, fullHeight) + safeTile - 1u) / safeTile;
    if (dtid.x >= tilesX || dtid.y >= tilesY) {
        return;
    }

    uint bgWidth = 0u;
    uint bgHeight = 0u;
    BackgroundColor.GetDimensions(bgWidth, bgHeight);
    if (bgWidth == 0u || bgHeight == 0u) {
        return;
    }

    if (dtid.x == 0u && dtid.y == 0u) {
        RenderOwnershipStats[HORIZON_TILE_MASK_TOTAL_TILES] = tilesX * tilesY;
        RenderOwnershipStats[HORIZON_TILE_MASK_FRAME] = frameIndex;
        HorizonTileDrawArgsWords[0] = 6u;
        HorizonTileDrawArgsWords[2] = 0u;
        HorizonTileDrawArgsWords[3] = 0u;
    }

    const uint tileY0 = dtid.y * safeTile;
    const uint tileY1 = min(max(1u, fullHeight), tileY0 + safeTile);
    const uint bandY0 = min(y0, max(1u, fullHeight));
    const uint bandY1 = min(max(y1, bandY0), max(1u, fullHeight));
    HorizonTileMask[dtid.xy] = 0u;
    if (tileY1 <= bandY0 || tileY0 >= bandY1) {
        return;
    }
    InterlockedAdd(RenderOwnershipStats[HORIZON_TILE_MASK_BAND_TILES], 1u);

    const uint tileX0 = dtid.x * safeTile;
    const uint tileX1 = min(max(1u, fullWidth), tileX0 + safeTile);

    const uint bx0 = (tileX0 * bgWidth) / max(1u, fullWidth);
    const uint bx1 = min(bgWidth - 1u, max(bx0, ((tileX1 * bgWidth) + max(1u, fullWidth) - 1u) / max(1u, fullWidth)));
    const uint by0 = (tileY0 * bgHeight) / max(1u, fullHeight);
    const uint by1 = min(bgHeight - 1u, max(by0, ((tileY1 * bgHeight) + max(1u, fullHeight) - 1u) / max(1u, fullHeight)));

    const int sx0 = max(0, (int)bx0 - 1);
    const int sy0 = max(0, (int)by0 - 1);
    const int sx1 = min((int)bgWidth - 1, (int)bx1 + 1);
    const int sy1 = min((int)bgHeight - 1, (int)by1 + 1);

    float minLum = 1.0e9f;
    float maxLum = -1.0e9f;
    float sumLum = 0.0f;
    float sumLum2 = 0.0f;
    uint lumSamples = 0u;
    float3 minRgb = 1.0e9f.xxx;
    float3 maxRgb = -1.0e9f.xxx;
    [loop]
    for (int y = sy0; y <= sy1; ++y) {
        [loop]
        for (int x = sx0; x <= sx1; ++x) {
            const float3 c = BackgroundColor.Load(int3(x, y, 0)).rgb;
            const float l = Luma(c);
            minLum = min(minLum, l);
            maxLum = max(maxLum, l);
            sumLum += l;
            sumLum2 += l * l;
            lumSamples += 1u;
            minRgb = min(minRgb, c);
            maxRgb = max(maxRgb, c);
        }
    }

    const float lumaRange = maxLum - minLum;
    const float rgbRange = max(maxRgb.r - minRgb.r, max(maxRgb.g - minRgb.g, maxRgb.b - minRgb.b));
    const float edge = max(lumaRange, rgbRange);
    const float meanLum = sumLum / max(1.0f, (float)lumSamples);
    const float lumaVariance = max(0.0f, (sumLum2 / max(1.0f, (float)lumSamples)) - meanLum * meanLum);
    const float selectorScore = selectorMode == 1u ? (lumaVariance * edge) : edge;
    const uint edge255 = (uint)round(saturate(edge) * 255.0f);
    InterlockedMax(RenderOwnershipStats[HORIZON_TILE_MASK_MAX_EDGE_255], edge255);

    if (selectorScore >= asfloat(thresholdBits)) {
        HorizonTileMask[dtid.xy] = 1u;
        InterlockedAdd(RenderOwnershipStats[HORIZON_TILE_MASK_TILES], 1u);
        InterlockedAdd(RenderOwnershipStats[HORIZON_TILE_MASK_PIXEL_UPPER], (tileX1 - tileX0) * (tileY1 - tileY0));

        uint outIndex = 0u;
        InterlockedAdd(HorizonTileDrawArgsWords[1], 1u, outIndex);
        const uint maxTiles = tilesX * tilesY;
        if (outIndex < maxTiles) {
            HorizonTileList[outIndex] = dtid.xy;
            InterlockedAdd(RenderOwnershipStats[HORIZON_TILE_LIST_COUNT], 1u);
            InterlockedAdd(RenderOwnershipStats[HORIZON_TILE_DRAW_INSTANCES], 1u);
        }
    }
}
