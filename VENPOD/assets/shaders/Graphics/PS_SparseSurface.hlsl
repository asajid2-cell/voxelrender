#include "../Common/SharedTypes.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

Texture1D<float4> MaterialPalette : register(t1);
SamplerState PaletteSampler : register(s0);
RWStructuredBuffer<uint> RenderOwnershipStats : register(u0);

static const uint RENDER_OWNER_SURFACE = 9u;
static const uint RENDER_OWNER_FRAME = 8u;
static const uint RENDER_OWNER_WATER_CONTEXT = 12u;
static const uint RENDER_OWNER_FAR_SURFACE = 21u;
static const float FAR_SEA_LEVEL = -48.0f;
static const float FAR_WATER_SURFACE_Y = FAR_SEA_LEVEL + 1.0f;

struct PSInput {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint material : MATERIAL0;
    nointerpolation uint faceDirection : TEXCOORD2;
    float distance : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

float HashVoxelCell(float3 cell) {
    const float3 p = frac(cell * float3(0.1031f, 0.11369f, 0.13787f));
    return frac((p.x + p.y + p.z) * (p.x + p.y * 19.19f + p.z * 7.31f));
}

float UnderwaterCaustic(float3 worldPos) {
    const float t = (float)(frame.frameIndex & 2047u) * 0.018f;
    const float a = sin(worldPos.x * 0.115f + worldPos.z * 0.071f + t);
    const float b = sin(worldPos.x * 0.047f - worldPos.z * 0.163f - t * 0.72f);
    const float c = sin((worldPos.x + worldPos.z + worldPos.y * 0.35f) * 0.091f + t * 1.37f);
    return saturate(0.50f + a * b * 0.24f + c * 0.14f);
}

float UnderwaterParticulate(float3 worldPos) {
    const float t = (float)(frame.frameIndex & 4095u) * 0.009f;
    const float a = sin(worldPos.x * 0.029f + worldPos.y * 0.061f + t);
    const float b = sin(worldPos.z * 0.037f - worldPos.y * 0.023f - t * 0.83f);
    return saturate(0.52f + a * 0.15f + b * 0.13f);
}

float3 SparseSurfaceMaterialVariation(float3 baseColor, uint material, float3 worldPos, float3 normal) {
    const float patch = HashVoxelCell(floor(worldPos / 9.0f));
    const float largePatch = HashVoxelCell(floor((worldPos + 31.0f) / 27.0f));
    const float slope = saturate((0.72f - normal.y) / 0.64f);
    const float highExposure = saturate((worldPos.y - 70.0f) / 190.0f);

    float3 varied = baseColor;
    if (material == MAT_STONE) {
        const float blendedPatch = lerp(patch, largePatch, 0.42f);
        const float3 coolStone = float3(0.38f, 0.41f, 0.36f);
        const float3 warmStone = float3(0.62f, 0.57f, 0.45f);
        const float3 weatheredStone = float3(0.42f, 0.50f, 0.32f);
        varied = lerp(coolStone, warmStone, blendedPatch * 0.72f + 0.14f);
        const float ledgeWeathering = (1.0f - slope) * smoothstep(0.54f, 0.92f, largePatch);
        const float lowWeathering = (1.0f - saturate((worldPos.y - FAR_SEA_LEVEL - 18.0f) / 170.0f));
        varied = lerp(varied, weatheredStone, saturate(ledgeWeathering * 0.28f + lowWeathering * 0.20f));
    } else if (material == MAT_DIRT) {
        const float3 grassDirt = float3(0.36f, 0.52f, 0.25f);
        const float3 exposedDirt = float3(0.48f, 0.42f, 0.31f);
        const float3 dryScrub = float3(0.50f, 0.49f, 0.32f);
        varied = lerp(grassDirt, exposedDirt, slope * 0.68f + highExposure * 0.22f);
        varied = lerp(varied, dryScrub, smoothstep(0.62f, 0.94f, largePatch) * 0.22f);
    } else if (material == MAT_SAND) {
        const float3 dampSand = float3(0.58f, 0.53f, 0.36f);
        const float3 drySand = float3(0.78f, 0.70f, 0.44f);
        varied = lerp(dampSand, drySand, patch * 0.72f + (1.0f - slope) * 0.18f);
    }

    const float variationStrength = material == MAT_STONE ? 0.58f : 0.44f;
    return lerp(baseColor, varied, variationStrength);
}

float3 DebugMaterialColor(uint material) {
    if (material == MAT_WATER) {
        return float3(0.05f, 0.38f, 1.0f);
    }
    if (material == MAT_SAND) {
        return float3(1.0f, 0.84f, 0.12f);
    }
    if (material == MAT_DIRT) {
        return float3(0.18f, 0.78f, 0.20f);
    }
    if (material == MAT_STONE) {
        return float3(0.55f, 0.55f, 0.55f);
    }
    if (material == MAT_AIR) {
        return float3(0.03f, 0.04f, 0.06f);
    }
    if (material == MAT_BEDROCK) {
        return float3(0.12f, 0.12f, 0.13f);
    }
    if (material == MAT_GLASS) {
        return float3(0.70f, 0.95f, 1.0f);
    }
    return float3(1.0f, 0.10f, 0.90f);
}

float3 DebugSurfaceOwnerMaterialColor(uint material) {
    const float3 materialColor = DebugMaterialColor(material);
    const float3 exactSurfaceTint = float3(0.95f, 0.95f, 0.95f);
    return saturate(materialColor * 0.86f + exactSurfaceTint * 0.14f);
}

float4 main(PSInput input) : SV_Target {
    const float surfaceDistance = distance(input.worldPos, frame.cameraPosition.xyz);
    const float exactNearDistance = max(frame.exactNearParams.x, 0.0f);
    const float protectedSurfaceDistance = max(exactNearDistance + 768.0f, 1536.0f);
    const bool aboveWaterView = frame.cameraPosition.y >= FAR_WATER_SURFACE_Y - 0.5f;
    const bool sparseWaterVoxelOccludedByPlane = false;
    float waterPlaneT = 0.0f;
    bool deterministicWaterBeforeSurface = false;
    if (input.material != MAT_WATER && aboveWaterView) {
        const float3 toSurface = input.worldPos - frame.cameraPosition.xyz;
        const float rayLength = length(toSurface);
        if (rayLength > 0.001f) {
            const float3 rayDir = toSurface / rayLength;
            if (rayDir.y < -0.0001f) {
                waterPlaneT = (FAR_WATER_SURFACE_Y - frame.cameraPosition.y) / rayDir.y;
                deterministicWaterBeforeSurface =
                    waterPlaneT >= 4.0f &&
                    waterPlaneT < rayLength - 0.25f &&
                    input.worldPos.y < FAR_WATER_SURFACE_Y + 18.0f;
            }
        }
    }
    const bool sparseSubmergedTerrainOccludedByPlane =
        input.material != MAT_WATER &&
        aboveWaterView &&
        (input.worldPos.y < FAR_WATER_SURFACE_Y - 0.05f ||
         deterministicWaterBeforeSurface);
    if (frame.farFieldGridParams.w > 0.5f) {
        InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_SURFACE], 1u);
        if (exactNearDistance > 0.0f && surfaceDistance > protectedSurfaceDistance) {
            InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_FAR_SURFACE], 1u);
        }
        RenderOwnershipStats[RENDER_OWNER_FRAME] = frame.frameIndex;
    }

    if (sparseWaterVoxelOccludedByPlane ||
        sparseSubmergedTerrainOccludedByPlane) {
        // Exact water surfaces are generated sparse geometry and should own
        // when resident. Only discard terrain that is actually below the
        // deterministic water surface; the analytic plane is the fallback for
        // missing/far water, not a replacement for exact water drawables.
        discard;
    }

    if (frame.debugMode == 50u) {
        return float4(1.0f, 0.95f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 51u) {
        const float heightDelta = input.worldPos.y - frame.cameraPosition.y;
        const float above = saturate(heightDelta / 160.0f);
        const float below = saturate(-heightDelta / 80.0f);
        const float nearEye = 1.0f - saturate(abs(heightDelta) / 32.0f);
        return float4(above, nearEye, below, 1.0f);
    }
    if (frame.debugMode == 52u) {
        const float dist = distance(input.worldPos, frame.cameraPosition.xyz);
        const float nearBand = 1.0f - saturate(dist / 128.0f);
        const float midBand = 1.0f - abs(saturate((dist - 128.0f) / 384.0f) * 2.0f - 1.0f);
        const float farBand = saturate((dist - 384.0f) / 512.0f);
        return float4(nearBand, midBand, farBand, 1.0f);
    }
    if (frame.debugMode == 54u) {
        return float4(DebugMaterialColor(input.material), 1.0f);
    }
    if (frame.debugMode == 55u) {
        // Owner debug: exact sparse raster surfaces are the bounded near-field
        // authority. They must stand apart from mid/far voxel background
        // colors so distant surface leakage is obvious in a single capture.
        if (exactNearDistance > 0.0f && surfaceDistance > protectedSurfaceDistance) {
            return float4(1.0f, 0.05f, 0.90f, 1.0f);
        }
        if (exactNearDistance > 0.0f && surfaceDistance > exactNearDistance) {
            return float4(1.0f, 0.46f, 0.05f, 1.0f);
        }
        return float4(1.0f, 0.95f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 58u) {
        return float4(1.0f, 0.95f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 59u) {
        const float nearBand = 1.0f - saturate(surfaceDistance / max(exactNearDistance, 1.0f));
        return float4(1.0f, nearBand, 0.0f, 1.0f);
    }
    if (frame.debugMode == 60u) {
        const float fog = saturate(surfaceDistance / 3000.0f) * 0.5f;
        return float4(fog, fog, fog, 1.0f);
    }
    if (frame.debugMode == 61u) {
        return input.material == MAT_WATER
            ? float4(0.02f, 0.88f, 1.0f, 1.0f)
            : float4(0.12f, 0.08f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 62u) {
        return float4(0.05f, 0.26f, 0.08f, 1.0f);
    }
    if (frame.debugMode == 63u) {
        return float4(saturate(normalize(input.normal) * 0.5f + 0.5f), 1.0f);
    }
    if (frame.debugMode == 64u) {
        if (input.faceDirection == 3u) {
            return float4(0.05f, 0.95f, 0.18f, 1.0f);
        }
        if (input.faceDirection == 2u) {
            return float4(0.95f, 0.05f, 0.95f, 1.0f);
        }
        if (input.faceDirection == 0u || input.faceDirection == 1u) {
            return float4(1.0f, 0.48f, 0.05f, 1.0f);
        }
        return float4(1.0f, 0.86f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 65u) {
        if (input.faceDirection == 3u) {
            return float4(0.05f, 0.95f, 0.18f, 1.0f);
        }
        if (input.faceDirection == 2u) {
            return float4(0.95f, 0.05f, 0.95f, 1.0f);
        }
        return float4(1.0f, 0.48f, 0.05f, 1.0f);
    }
    if (frame.debugMode == 66u) {
        const float distanceBand = saturate(surfaceDistance / 6400.0f);
        return float4(distanceBand, distanceBand, distanceBand, 1.0f);
    }
    if (frame.debugMode == 56u) {
        return float4(DebugSurfaceOwnerMaterialColor(input.material), 1.0f);
    }

    const float u = ((float)input.material + 0.5f) / 256.0f;
    const float3 n = normalize(input.normal);
    const bool sideFace =
        input.faceDirection == 0u ||
        input.faceDirection == 1u ||
        input.faceDirection == 4u ||
        input.faceDirection == 5u;
    float3 baseColor = MaterialPalette.SampleLevel(PaletteSampler, u, 0).rgb;
    const bool underwaterView = frame.cameraPosition.y < FAR_WATER_SURFACE_Y - 0.5f;
    if (input.material == MAT_SAND || input.material == MAT_DIRT || input.material == MAT_STONE) {
        baseColor = SparseSurfaceMaterialVariation(baseColor, input.material, input.worldPos, n);
    }
    if (input.material == MAT_WATER) {
        if (frame.farFieldGridParams.w > 0.5f) {
            InterlockedAdd(RenderOwnershipStats[RENDER_OWNER_WATER_CONTEXT], 1u);
        }
        const bool underwaterCamera = frame.cameraPosition.y < FAR_WATER_SURFACE_Y - 1.0f;
        if (underwaterCamera &&
            input.worldPos.y > frame.cameraPosition.y + 2.0f &&
            abs(n.y) > 0.42f) {
            const float t = (float)(frame.frameIndex & 4095u) * 0.012f;
            const float ripple =
                sin(input.worldPos.x * 0.053f + input.worldPos.z * 0.041f + t) * 0.5f +
                sin(input.worldPos.x * -0.029f + input.worldPos.z * 0.074f - t * 0.67f) * 0.5f;
            const float distanceFog = saturate((input.distance - 18.0f) / 220.0f);
            const float depthBelowSurface = saturate((input.worldPos.y - frame.cameraPosition.y) / 72.0f);
            const float2 waterCell = abs(frac(input.worldPos.xz / 10.0f) - 0.5f);
            const float edgeGrid = (1.0f - smoothstep(0.476f, 0.498f, max(waterCell.x, waterCell.y))) * 0.025f;
            float3 boundaryColor = lerp(
                float3(0.055f, 0.205f, 0.245f),
                float3(0.145f, 0.375f, 0.415f),
                0.40f + ripple * 0.10f + depthBelowSurface * 0.18f);
            boundaryColor = lerp(boundaryColor, float3(0.24f, 0.48f, 0.52f), edgeGrid);
            boundaryColor = lerp(boundaryColor, float3(0.17f, 0.34f, 0.38f), distanceFog * 0.42f);
            return float4(boundaryColor, 1.0f);
        }
        const float t = (float)(frame.frameIndex & 4095u) * 0.014f;
        const float ripple =
            sin(input.worldPos.x * 0.072f + input.worldPos.z * 0.045f + t) * 0.5f +
            sin(input.worldPos.x * -0.031f + input.worldPos.z * 0.096f - t * 0.73f) * 0.5f;
        const float2 waterCell = abs(frac(input.worldPos.xz / 12.0f) - 0.5f);
        const float edgeGrid = (1.0f - smoothstep(0.465f, 0.497f, max(waterCell.x, waterCell.y))) * 0.045f;
        float3 waterColor = lerp(float3(0.12f, 0.34f, 0.44f), float3(0.20f, 0.50f, 0.58f), 0.45f + ripple * 0.12f);
        waterColor = lerp(waterColor, float3(0.58f, 0.72f, 0.80f), edgeGrid);
        const float viewFacing = saturate(dot(normalize(frame.cameraPosition.xyz - input.worldPos), n));
        const float sheen = pow(1.0f - viewFacing, 3.0f);
        waterColor += float3(0.08f, 0.12f, 0.13f) * sheen;
        const float fog = saturate((input.distance - 900.0f) / 3500.0f);
        return float4(lerp(waterColor, float3(0.62f, 0.70f, 0.78f), fog * 0.22f), 1.0f);
    }
    if (underwaterView &&
        (input.material == MAT_SAND || input.material == MAT_DIRT || input.material == MAT_STONE)) {
        const float basinNoise = HashVoxelCell(floor(input.worldPos / 7.0f));
        const float reefNoise = HashVoxelCell(floor((input.worldPos + 19.0f) / 19.0f));
        const float slope = saturate((0.72f - n.y) / 0.58f);
        const float shelfBand = smoothstep(0.34f, 0.76f, basinNoise) * (1.0f - slope * 0.35f);
        const float rockBand = saturate(slope * 0.85f + smoothstep(0.70f, 0.93f, reefNoise) * 0.55f);
        const float3 paleSediment = float3(0.52f, 0.58f, 0.44f);
        const float3 reefStone = float3(0.34f, 0.39f, 0.35f);
        const float3 siltOlive = float3(0.40f, 0.47f, 0.35f);
        baseColor = lerp(baseColor, paleSediment, shelfBand * 0.46f);
        baseColor = lerp(baseColor, siltOlive, smoothstep(0.12f, 0.34f, basinNoise) * 0.28f);
        baseColor = lerp(baseColor, reefStone, rockBand * 0.52f);
    }
    float shorelineWetBoundary = 0.0f;
    if (input.material == MAT_DIRT) {
        const float steepFace = saturate((0.62f - n.y) / 0.55f);
        const float strata = HashVoxelCell(floor(float3(input.worldPos.x * 0.18f, input.worldPos.y * 0.55f, input.worldPos.z * 0.18f)));
        const float contour = 0.5f + 0.5f * sin(input.worldPos.y * 0.42f + strata * 6.28318f);
        const float3 bankSoil = lerp(
            float3(0.39f, 0.45f, 0.30f),
            float3(0.54f, 0.50f, 0.34f),
            strata * 0.62f + contour * 0.18f);
        const float3 vegetatedBank = float3(0.34f, 0.49f, 0.25f);
        const float sideBlend = sideFace ? 0.72f : 0.0f;
        baseColor = lerp(baseColor, bankSoil, sideBlend);
        baseColor = lerp(baseColor, vegetatedBank, sideFace ? (1.0f - steepFace) * 0.18f : 0.0f);
        baseColor = lerp(baseColor, float3(0.47f, 0.45f, 0.37f), steepFace * (sideFace ? 0.14f : 0.72f));
    }
    if (input.material == MAT_SAND && sideFace) {
        const float strata = HashVoxelCell(floor(float3(
            input.worldPos.x * 0.14f,
            input.worldPos.y * 0.48f,
            input.worldPos.z * 0.14f)));
        const float contour =
            0.5f + 0.5f * sin(input.worldPos.y * 0.36f + strata * 6.28318f);
        const float waterlineFace =
            1.0f - saturate((input.worldPos.y - FAR_SEA_LEVEL + 8.0f) / 34.0f);
        const float distanceBlend = saturate((input.distance - 96.0f) / 900.0f);
        const float3 compactSand = float3(0.55f, 0.51f, 0.35f);
        const float3 dryBankSand = float3(0.66f, 0.60f, 0.39f);
        const float3 dampBankSand = float3(0.43f, 0.45f, 0.35f);
        float3 bankSand = lerp(compactSand, dryBankSand, strata * 0.48f + contour * 0.22f);
        bankSand = lerp(bankSand, dampBankSand, waterlineFace * 0.46f);
        baseColor = lerp(baseColor, bankSand, lerp(0.62f, 0.82f, distanceBlend));
    }
    if (input.material == MAT_SAND || input.material == MAT_DIRT || input.material == MAT_STONE) {
        const float waterlineFace = 1.0f - saturate((input.worldPos.y - FAR_SEA_LEVEL + 2.0f) / 14.0f);
        const float verticalBank = saturate((0.54f - n.y) / 0.62f);
        const float wetBoundary = waterlineFace * (0.35f + verticalBank * 0.65f);
        shorelineWetBoundary = saturate(wetBoundary);
        const float3 wetSediment = input.material == MAT_SAND
            ? float3(0.40f, 0.43f, 0.34f)
            : float3(0.34f, 0.39f, 0.34f);
        baseColor = lerp(baseColor, wetSediment, saturate(wetBoundary * 0.78f));
    }

    const float3 lightDir = normalize(float3(0.45f, 0.82f, 0.34f));
    const float diffuse = saturate(dot(n, lightDir));
    // Higher-contrast lighting for terrain depth/definition (was ambient-dominated and
    // washed out). Warm directional sun + cooler sky fill, with a slope-based ambient
    // occlusion (downward/horizontal faces receive less sky light).
    const float3 sunColor = float3(1.06f, 0.99f, 0.86f);
    const float3 skyFill = float3(0.43f, 0.48f, 0.56f);
    const float skyAccess = saturate(n.y * 0.5f + 0.5f);
    float3 color = baseColor * (skyFill * (0.32f + 0.42f * skyAccess) + sunColor * diffuse * 0.80f);
    color = max(color, baseColor * 0.28f);
    const float voxelTone = HashVoxelCell(floor(input.worldPos + n * 0.01f)) - 0.5f;
    const float terrainToneStrength = input.material == MAT_STONE
        ? (sideFace ? 0.026f : 0.055f)
        : (sideFace ? 0.045f : 0.12f);
    color *= 1.0f + voxelTone * (underwaterView ? 0.014f : terrainToneStrength);

    const float3 absNormal = abs(n);
    float2 gridUv = input.worldPos.xy;
    if (absNormal.x > absNormal.y && absNormal.x > absNormal.z) {
        gridUv = input.worldPos.zy;
    } else if (absNormal.y > absNormal.z) {
        gridUv = input.worldPos.xz;
    }
    const float2 cell = abs(frac(gridUv) - 0.5f);
    const float gridLine = 1.0f - smoothstep(0.455f, 0.495f, max(cell.x, cell.y));
    const float underwaterGridStrength =
        0.0005f + (1.0f - saturate((input.distance - 12.0f) / 80.0f)) * 0.0025f;
    const float stoneGridScale = input.material == MAT_STONE ? 0.42f : 1.0f;
    const float sideSandGridScale =
        (sideFace && input.material == MAT_SAND) ? 0.42f : 1.0f;
    const float shorelineGridSuppression =
        1.0f - shorelineWetBoundary * (sideFace ? 0.82f : 0.54f);
    const float sideDistanceGridFade = sideFace
        ? lerp(0.48f, 0.18f, saturate((input.distance - 220.0f) / 620.0f))
        : 1.0f;
    const float dryGridStrength =
        (sideFace ? 0.05f : 0.14f) *
        stoneGridScale *
        sideSandGridScale *
        shorelineGridSuppression *
        sideDistanceGridFade;
    color *= lerp(1.0f, underwaterView ? 0.998f : 0.78f, gridLine * (underwaterView ? underwaterGridStrength : dryGridStrength));

    if (underwaterView) {
        const float underwaterFog = saturate((input.distance - 3.0f) / 54.0f);
        const float aboveWaterPenalty = saturate((input.worldPos.y - FAR_SEA_LEVEL + 8.0f) / 88.0f);
        const float verticalSilhouette = saturate((input.worldPos.y - frame.cameraPosition.y - 12.0f) / 120.0f);
        const float waterColumn = saturate((input.distance - 42.0f) / 220.0f);
        const float fogStrength = saturate(
            0.42f +
            underwaterFog * 0.34f +
            waterColumn * 0.30f +
            aboveWaterPenalty * 0.26f +
            verticalSilhouette * 0.18f);
        const float particulate = UnderwaterParticulate(input.worldPos);
        const float3 deepWaterTint = lerp(
            float3(0.15f, 0.36f, 0.40f),
            float3(0.12f, 0.32f, 0.36f) + particulate * float3(0.026f, 0.044f, 0.040f),
            waterColumn);
        const float3 surfaceVolumeTint =
            float3(0.22f, 0.45f, 0.49f) + particulate * float3(0.020f, 0.034f, 0.030f);
        const float3 waterTint = lerp(
            deepWaterTint,
            surfaceVolumeTint,
            saturate(aboveWaterPenalty * 0.46f + verticalSilhouette * 0.32f));
        color = lerp(color, waterTint, fogStrength);

        const float nearWaterLight = 1.0f - saturate((input.distance - 8.0f) / 190.0f);
        const float upwardFace = saturate(n.y * 0.55f + 0.45f);
        const float caustic = UnderwaterCaustic(input.worldPos);
        const float causticLines = smoothstep(0.58f, 0.93f, caustic);
        color += float3(0.040f, 0.078f, 0.066f) * causticLines * nearWaterLight * upwardFace;
        color *= 1.0f + (causticLines - 0.5f) * nearWaterLight * upwardFace * 0.026f;
    }

    const float fog = saturate((input.distance - 1500.0f) / 4500.0f);
    const float3 sky = float3(0.62f, 0.70f, 0.78f);
    color = lerp(color, sky, fog * 0.35f);
    return float4(color, 1.0f);
}
