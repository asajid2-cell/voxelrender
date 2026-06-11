#include "SparseTerrainGenerator.h"

#include "Simulation/TerrainConstants.h"
#include "Utils/BitPacking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VENPOD::Simulation {

namespace {

constexpr int32_t kMaxScenicSpawnSearchRadius = 512;
constexpr int32_t kMinScenicSpawnSampleSpacing = 16;
constexpr int32_t kMaxScenicSpawnSampleSpacing = 512;
constexpr float kProceduralHeightUpperBound =
    static_cast<float>(TERRAIN_MAX_Y);

uint32_t AddFlag(uint32_t flags, BrickResidencyFlags flag) {
    return flags | static_cast<uint32_t>(flag);
}

bool TryAddInt32(int32_t value, int32_t delta, int32_t& out) {
    const int64_t sum = static_cast<int64_t>(value) + static_cast<int64_t>(delta);
    if (sum < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        sum > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(sum);
    return true;
}

bool TryRoundToInt32(double value, int32_t& out) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double rounded = std::round(value);
    if (rounded < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    out = static_cast<int32_t>(rounded);
    return true;
}

} // namespace

float SparseTerrainGenerator::Smooth01(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float SparseTerrainGenerator::Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

uint32_t SparseTerrainGenerator::Hash3D(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed ^ 2166136261u;
    h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float SparseTerrainGenerator::ValueNoise2D(float x, float z, uint32_t seed) {
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t z0 = static_cast<int32_t>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const float sx = Smooth01(fx);
    const float sz = Smooth01(fz);

    auto sample = [seed](int32_t ix, int32_t iz) {
        return static_cast<float>(Hash3D(ix, 0, iz, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };

    const float a = Lerp(sample(x0, z0), sample(x0 + 1, z0), sx);
    const float b = Lerp(sample(x0, z0 + 1), sample(x0 + 1, z0 + 1), sx);
    return Lerp(a, b, sz) * 2.0f - 1.0f;
}

float SparseTerrainGenerator::HeightAt(int32_t worldX, int32_t worldZ) const {
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);

    const float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, m_seed + 11u);
    const float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, m_seed + 23u);
    const float ridge = 1.0f - std::abs(ridgeSource);
    const float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, m_seed + 37u);

    const float ridgeHeight = ridge * ridge;

    // VISUAL PASS iter1 (landforms): flatten the rolling base so plains read as
    // playable flats with distinct hills, not a continuous contour gradient.
    // broad 145 -> 92 (less mid-band roll), detail 8 -> 3 (less high-freq contour
    // wobble). ridgeHeight kept at 150 so hills/mountains stay distinct.
    float height = -64.0f;
    height += broad * 92.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 3.0f;

    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    const float originComfort = 1.0f - Smooth01(std::clamp((originDistance - 180.0f) / 520.0f, 0.0f, 1.0f));
    const float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - Smooth01(originDistance / 420.0f)) * 58.0f;
    height = Lerp(height, publicRegionHeight, originComfort * 0.94f);
    const float publicCapInfluence =
        1.0f - Smooth01(std::clamp((originDistance - 220.0f) / 420.0f, 0.0f, 1.0f));
    const float publicCap =
        58.0f +
        Smooth01(std::clamp(originDistance / 640.0f, 0.0f, 1.0f)) * 114.0f;
    height = Lerp(height, std::min(height, publicCap), publicCapInfluence);

    const float submergedBlend =
        1.0f - Smooth01(std::clamp((height - static_cast<float>(SEA_LEVEL_Y + 28)) / 86.0f, 0.0f, 1.0f));
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            static_cast<float>(SEA_LEVEL_Y - 8) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - Smooth01(originDistance / 520.0f)) * 18.0f;
        height = Lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand =
        (1.0f - Smooth01(std::clamp((originDistance - 260.0f) / 980.0f, 0.0f, 1.0f)));
    const float lowlandUpper =
        1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(SEA_LEVEL_Y + 96)) / 120.0f,
            0.0f,
            1.0f));
    const float lowlandFloor =
        Smooth01(std::clamp(
            (height - static_cast<float>(SEA_LEVEL_Y - 40)) / 64.0f,
            0.0f,
            1.0f));
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    if (playableBankBlend > 0.0f) {
        const float playableShelfHeight =
            static_cast<float>(SEA_LEVEL_Y + 18) +
            broad * 28.0f +
            ridgeHeight * 10.0f +
            detail * 1.5f +
            (1.0f - Smooth01(std::clamp(originDistance / 460.0f, 0.0f, 1.0f))) * 42.0f;
        height = Lerp(height, playableShelfHeight, playableBankBlend);
    }
    const float publicBasinBand =
        Smooth01(std::clamp((originDistance - 360.0f) / 240.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 1700.0f) / 760.0f, 0.0f, 1.0f))) *
        Smooth01(std::clamp((height - static_cast<float>(SEA_LEVEL_Y - 38)) / 56.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((height - static_cast<float>(SEA_LEVEL_Y + 180)) / 140.0f, 0.0f, 1.0f)));
    const float publicBasinFloor =
        static_cast<float>(SEA_LEVEL_Y - 12) +
        broad * 2.0f +
        detail * 0.35f;
    if (publicBasinBand > 0.0f) {
        height = Lerp(height, std::min(height, publicBasinFloor), publicBasinBand * 0.80f);
    }
    const float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, m_seed + 211u);
    const float backdropRidgeSource =
        ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, m_seed + 227u);
    const float backdropRidge = 1.0f - std::abs(backdropRidgeSource);
    const float backdropBreakup =
        ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, m_seed + 271u);
    const float backdropNotch =
        Smooth01(std::clamp((backdropBreakup - 0.08f) / 0.58f, 0.0f, 1.0f));
    const float silhouetteRidge =
        std::clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f, 0.0f, 1.0f);
    const float backdropBand =
        Smooth01(std::clamp((originDistance - 1360.0f) / 700.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 5200.0f) / 1200.0f, 0.0f, 1.0f)));
    const float northBackdrop =
        Smooth01(std::clamp((z - 1180.0f) / 900.0f, 0.0f, 1.0f));
    const float sideBackdrop =
        Smooth01(std::clamp((std::abs(x - 192.0f) - 820.0f) / 980.0f, 0.0f, 1.0f));
    const float backdropFacing =
        std::clamp(northBackdrop + sideBackdrop * 0.58f, 0.0f, 1.0f);
    const float silhouetteContinuity =
        std::clamp(silhouetteRidge + backdropBand * backdropFacing * 0.32f, 0.0f, 1.0f);
    const float backdropInfluence =
        backdropBand *
        backdropFacing *
        Smooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    const float backdropHeight =
        248.0f +
        backdropBand * 160.0f +
        silhouetteContinuity * 186.0f +
        backdropNoise * 26.0f;
    height = Lerp(height, std::max(height, backdropHeight), backdropInfluence * 0.70f);

    const float westCorridor = Smooth01(std::clamp((192.0f - x - 520.0f) / 820.0f, 0.0f, 1.0f));
    const float eastCorridor = Smooth01(std::clamp((x - 192.0f - 520.0f) / 820.0f, 0.0f, 1.0f));
    const float southBlend = Smooth01(std::clamp((360.0f - z) / 1200.0f, 0.0f, 1.0f));
    const float westNorthBlend = Smooth01(std::clamp((z - 360.0f) / 920.0f, 0.0f, 1.0f));
    const float routeDistanceBand =
        Smooth01(std::clamp((originDistance - 780.0f) / 420.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 4300.0f) / 1200.0f, 0.0f, 1.0f)));
    const float routeCorridor = routeDistanceBand * std::clamp(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend,
        0.0f,
        1.0f);
    const float routeRidgeNoiseA =
        ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, m_seed + 251u);
    const float routeRidgeNoiseB =
        ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, m_seed + 263u);
    const float routeBreakup =
        ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, m_seed + 281u);
    const float routeNotch =
        Smooth01(std::clamp((routeBreakup - 0.02f) / 0.60f, 0.0f, 1.0f));
    const float routeRidge =
        std::clamp(
            0.26f +
            (1.0f - std::abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f,
            0.0f,
            1.0f);
    const float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = Lerp(height, std::max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    // Spawn landmass: lift low/submerged near-origin terrain onto a gently
    // rolling land floor comfortably above sea level. TANDEM widen 9300 -> 35000
    // (Codex co-design): the old /9300 band dropped to ~0.69 by 4k and faded out
    // by ~9500u, so deep basins in the mid-distance got only PARTIAL lift and
    // stayed below sea -> the "ring turns into water" archipelago of fragmented
    // banks inside the render range. /35000 keeps the band ~full (>=0.96) out to
    // ~5k and ~1.0 to 15k, pushing the partial-reshape fragment zone past the
    // ~10k render horizon so the whole visible world is a solid believable
    // continent. Must stay byte-identical to TerrainHeight.hlsli TH_HeightAt.
    const float spawnLandBand =
        1.0f - Smooth01(std::clamp((originDistance - 200.0f) / 90000.0f, 0.0f, 1.0f));
    // VISUAL PASS iter1 (coast): raise the spawn land floor and reduce its noise so
    // near-spawn ground is a solid coherent plain well above the waterline (no
    // 1-voxel banks / thin slivers). +40 -> +56 base, noise softened.
    const float spawnLandFloor =
        static_cast<float>(SEA_LEVEL_Y) + 56.0f +
        broad * 18.0f +
        ridgeHeight * 40.0f +
        detail * 3.0f;
    height = Lerp(height, std::max(height, spawnLandFloor), spawnLandBand);

    // VISUAL PASS iter1 (small consistent block steps): quantize the lowland/plains
    // band to a 3-unit terrace so flats render as clean block plateaus rather than
    // a 1-voxel contour gradient. The blend fades out as terrain rises into the
    // hill/mountain band (above SEA+150) so peaks keep their full relief.
    // VISUAL PASS iter2 (coherent shore): also fade the terrace OUT near and below
    // the waterline (off by SEA+8, full by SEA+40). The terrace staircase used to
    // run straight into the flat water plane, producing the ugly stepped/jagged
    // coast. Suppressing it in the shore band lets the smooth shelf terms above
    // form a coherent sloped beach where land meets sea.
    const float terraceStep = 3.0f;
    const float terraceUpperFade =
        1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(SEA_LEVEL_Y + 64)) / 150.0f, 0.0f, 1.0f));
    const float terraceShoreFade =
        Smooth01(std::clamp(
            (height - static_cast<float>(SEA_LEVEL_Y + 8)) / 32.0f, 0.0f, 1.0f));
    const float terraceBlend = terraceUpperFade * terraceShoreFade;
    if (terraceBlend > 0.0f) {
        const float terraced = std::floor(height / terraceStep) * terraceStep;
        height = Lerp(height, terraced, terraceBlend);
    }

    return std::clamp(height, static_cast<float>(TERRAIN_MIN_Y), static_cast<float>(TERRAIN_MAX_Y));
}

float SparseTerrainGenerator::SurfaceReliefAt(int32_t worldX, int32_t worldZ, int32_t sampleOffset) const {
    return SurfaceReliefAtWithCenter(worldX, worldZ, HeightAt(worldX, worldZ), sampleOffset);
}

float SparseTerrainGenerator::SurfaceReliefAtWithCenter(
    int32_t worldX,
    int32_t worldZ,
    float centerHeight,
    int32_t sampleOffset) const
{
    const int32_t offset = std::max(1, sampleOffset);
    int32_t xMinus = worldX;
    int32_t xPlus = worldX;
    int32_t zMinus = worldZ;
    int32_t zPlus = worldZ;
    (void)TryAddInt32(worldX, -offset, xMinus);
    (void)TryAddInt32(worldX, offset, xPlus);
    (void)TryAddInt32(worldZ, -offset, zMinus);
    (void)TryAddInt32(worldZ, offset, zPlus);

    float localMin = centerHeight;
    float localMax = centerHeight;
    const float samples[] = {
        HeightAt(xMinus, worldZ),
        HeightAt(xPlus, worldZ),
        HeightAt(worldX, zMinus),
        HeightAt(worldX, zPlus),
    };
    for (float h : samples) {
        localMin = std::min(localMin, h);
        localMax = std::max(localMax, h);
    }
    return localMax - localMin;
}

SparseTerrainGenerator::ScenicSpawn SparseTerrainGenerator::FindScenicSpawn(
    int32_t originX,
    int32_t originZ,
    float playerHeight,
    int32_t searchRadius,
    int32_t sampleSpacing) const
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int32_t kDirectionCount = 16;
    constexpr int32_t kNearViewDistance = 128;
    constexpr int32_t kFarViewDistance = 640;
    constexpr int32_t kViewStep = 32;
    constexpr float kSpawnMinGroundY = static_cast<float>(SEA_LEVEL_Y + 80);
    constexpr float kSpawnTargetGroundY = static_cast<float>(SEA_LEVEL_Y + 176);

    ScenicSpawn best;
    best.score = -std::numeric_limits<float>::infinity();

    sampleSpacing = std::clamp(
        sampleSpacing,
        kMinScenicSpawnSampleSpacing,
        kMaxScenicSpawnSampleSpacing);
    searchRadius = std::clamp(searchRadius, sampleSpacing, kMaxScenicSpawnSearchRadius);
    const float boundedPlayerHeight = std::clamp(
        std::isfinite(playerHeight) ? playerHeight : 6.0f,
        1.0f,
        64.0f);
    const float safeEyeOffset = boundedPlayerHeight + 3.0f;

    const int64_t minWorld = static_cast<int64_t>(std::numeric_limits<int32_t>::min());
    const int64_t maxWorld = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    const int64_t startZ = std::max(minWorld, static_cast<int64_t>(originZ) - searchRadius);
    const int64_t endZ = std::min(maxWorld, static_cast<int64_t>(originZ) + searchRadius);
    const int64_t startX = std::max(minWorld, static_cast<int64_t>(originX) - searchRadius);
    const int64_t endX = std::min(maxWorld, static_cast<int64_t>(originX) + searchRadius);

    for (int64_t z64 = startZ; z64 <= endZ; z64 += sampleSpacing) {
        const int32_t z = static_cast<int32_t>(z64);
        for (int64_t x64 = startX; x64 <= endX; x64 += sampleSpacing) {
            const int32_t x = static_cast<int32_t>(x64);
            const float height = HeightAt(x, z);
            const int32_t groundY = static_cast<int32_t>(std::floor(height));
            if (static_cast<float>(groundY) <= kSpawnMinGroundY || groundY >= TERRAIN_MAX_Y - 16) {
                continue;
            }

            const uint32_t groundVoxel = SampleGeneratedVoxel(x, groundY, z);
            const uint8_t groundMaterial = Utils::UnpackMaterial(groundVoxel);
            if (groundMaterial == Utils::Material::Air ||
                groundMaterial == Utils::Material::Water ||
                groundMaterial == Utils::Material::Bedrock) {
                continue;
            }

            bool hasHeadClearance = true;
            const int32_t clearanceTop =
                groundY + static_cast<int32_t>(std::ceil(boundedPlayerHeight + 8.0f));
            for (int32_t y = groundY + 1; y <= clearanceTop; ++y) {
                if (Utils::UnpackMaterial(SampleGeneratedVoxel(x, y, z)) != Utils::Material::Air) {
                    hasHeadClearance = false;
                    break;
                }
            }
            if (!hasHeadClearance) {
                continue;
            }

            float localMin = height;
            float localMax = height;
            bool localSamplesValid = true;
            for (int32_t oz = -32; oz <= 32; oz += 16) {
                for (int32_t ox = -32; ox <= 32; ox += 16) {
                    int32_t sampleX = 0;
                    int32_t sampleZ = 0;
                    if (!TryAddInt32(x, ox, sampleX) || !TryAddInt32(z, oz, sampleZ)) {
                        localSamplesValid = false;
                        break;
                    }
                    const float h = HeightAt(sampleX, sampleZ);
                    localMin = std::min(localMin, h);
                    localMax = std::max(localMax, h);
                }
                if (!localSamplesValid) {
                    break;
                }
            }
            if (!localSamplesValid) {
                continue;
            }
            const float localRelief = localMax - localMin;
            if (localRelief > 52.0f) {
                continue;
            }
            const float eyeY = static_cast<float>(groundY) + safeEyeOffset;

            float bestDirectionScore = -std::numeric_limits<float>::infinity();
            float bestYaw = 0.0f;
            float bestForwardClearance = 0.0f;
            for (int32_t dirIndex = 0; dirIndex < kDirectionCount; ++dirIndex) {
                const float yaw = (static_cast<float>(dirIndex) / static_cast<float>(kDirectionCount)) * kPi * 2.0f;

                bool nearBlocked = false;
                float openScore = 0.0f;
                float scenicScore = 0.0f;
                float directionReliefMin = height;
                float directionReliefMax = height;
                float forwardClearance = 0.0f;

                constexpr float kViewConeOffsets[] = {
                    -0.70f,
                    -0.35f,
                    0.0f,
                    0.35f,
                    0.70f,
                };
                for (float yawOffset : kViewConeOffsets) {
                    const float coneYaw = yaw + yawOffset;
                    const float dirX = std::cos(coneYaw);
                    const float dirZ = std::sin(coneYaw);
                    for (int32_t d = kViewStep; d <= kFarViewDistance; d += kViewStep) {
                        int32_t sx = 0;
                        int32_t sz = 0;
                        if (!TryRoundToInt32(
                                static_cast<double>(x) +
                                    static_cast<double>(dirX) * static_cast<double>(d),
                                sx) ||
                            !TryRoundToInt32(
                                static_cast<double>(z) +
                                    static_cast<double>(dirZ) * static_cast<double>(d),
                                sz)) {
                            nearBlocked = true;
                            break;
                        }
                        const float sampleHeight = HeightAt(sx, sz);
                        directionReliefMin = std::min(directionReliefMin, sampleHeight);
                        directionReliefMax = std::max(directionReliefMax, sampleHeight);

                        const float dropBelowEye = eyeY - sampleHeight;
                        if (d <= kNearViewDistance && dropBelowEye < 14.0f) {
                            nearBlocked = true;
                            break;
                        }
                        if (yawOffset == 0.0f && d <= kNearViewDistance) {
                            forwardClearance = static_cast<float>(d);
                        }

                        openScore += std::clamp((dropBelowEye + 24.0f) / 128.0f, 0.0f, 1.0f);
                        const float skylineBand = 1.0f -
                            std::clamp(std::abs(dropBelowEye - 48.0f) / 192.0f, 0.0f, 1.0f);
                        scenicScore += skylineBand * (d > kNearViewDistance ? 1.0f : 0.25f);
                    }
                    if (nearBlocked) {
                        break;
                    }
                }

                if (nearBlocked) {
                    continue;
                }

                const float directionRelief = directionReliefMax - directionReliefMin;
                const float directionScore =
                    openScore * 0.42f +
                    scenicScore * 0.62f +
                    std::clamp(directionRelief / 260.0f, 0.0f, 1.0f) * 8.0f;
                if (directionScore > bestDirectionScore) {
                    bestDirectionScore = directionScore;
                    bestYaw = yaw;
                    bestForwardClearance = forwardClearance;
                }
            }

            if (!std::isfinite(bestDirectionScore)) {
                continue;
            }

            const float distanceFromOrigin =
                static_cast<float>(std::sqrt(
                    (static_cast<double>(x) - static_cast<double>(originX)) *
                        (static_cast<double>(x) - static_cast<double>(originX)) +
                    (static_cast<double>(z) - static_cast<double>(originZ)) *
                        (static_cast<double>(z) - static_cast<double>(originZ))));
            const float heightScore =
                10.0f -
                std::clamp(std::abs(height - kSpawnTargetGroundY) / 148.0f, 0.0f, 1.0f) * 6.0f;
            const float reliefScore = std::clamp((52.0f - localRelief) / 52.0f, 0.0f, 1.0f) * 5.0f;
            const float slopePenalty = std::max(0.0f, localRelief - 32.0f) * 0.24f;
            const float distancePenalty = distanceFromOrigin * 0.006f;
            const float score =
                bestDirectionScore +
                heightScore +
                reliefScore -
                slopePenalty -
                distancePenalty;

            if (score > best.score) {
                best.found = true;
                best.worldX = x;
                best.worldZ = z;
                best.groundY = groundY;
                best.eyeY = eyeY;
                best.yaw = bestYaw;
                best.pitch = -0.04f;
                best.score = score;
                best.forwardClearance = bestForwardClearance;
                best.localRelief = localRelief;
            }
        }
    }

    if (best.found) {
        return best;
    }

    const int32_t fallbackGround = static_cast<int32_t>(std::floor(HeightAt(originX, originZ)));
    best.found = false;
    best.worldX = originX;
    best.worldZ = originZ;
    best.groundY = fallbackGround;
    best.eyeY = static_cast<float>(fallbackGround) + safeEyeOffset;
    best.yaw = 0.0f;
    best.pitch = -0.04f;
    best.score = 0.0f;
    return best;
}

bool SparseTerrainGenerator::IsDefinitelyEmptyBrick(
    const BrickCoord& coord,
    float verticalSafetyMargin) const
{
    int32_t minY = 0;
    int32_t minX = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxZ = 0;
    if (!TryWorldVoxelFromBrickLocal(coord.y, 0, &minY) ||
        !TryWorldVoxelFromBrickLocal(coord.x, 0, &minX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, 0, &minZ) ||
        !TryWorldVoxelFromBrickLocal(coord.x, SPARSE_BRICK_SIZE - 1, &maxX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, SPARSE_BRICK_SIZE - 1, &maxZ)) {
        return false;
    }
    if (minY <= SEA_LEVEL_Y || minY <= TERRAIN_MIN_Y + 2) {
        return false;
    }

    if (static_cast<float>(minY) > kProceduralHeightUpperBound + verticalSafetyMargin) {
        return true;
    }

    const int32_t midX = minX + SPARSE_BRICK_SIZE / 2;
    const int32_t midZ = minZ + SPARSE_BRICK_SIZE / 2;

    float maxHeight = static_cast<float>(TERRAIN_MIN_Y);
    const int32_t sampleX[3] = {minX, midX, maxX};
    const int32_t sampleZ[3] = {minZ, midZ, maxZ};
    for (int32_t z : sampleZ) {
        for (int32_t x : sampleX) {
            maxHeight = std::max(maxHeight, HeightAt(x, z));
        }
    }

    return static_cast<float>(minY) > maxHeight + verticalSafetyMargin;
}

bool SparseTerrainGenerator::IsDefinitelyBuriedSolidBrick(
    const BrickCoord& coord,
    float verticalSafetyMargin) const
{
    int32_t minY = 0;
    int32_t maxY = 0;
    int32_t minX = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxZ = 0;
    if (!TryWorldVoxelFromBrickLocal(coord.y, 0, &minY) ||
        !TryWorldVoxelFromBrickLocal(coord.y, SPARSE_BRICK_SIZE - 1, &maxY) ||
        !TryWorldVoxelFromBrickLocal(coord.x, 0, &minX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, 0, &minZ) ||
        !TryWorldVoxelFromBrickLocal(coord.x, SPARSE_BRICK_SIZE - 1, &maxX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, SPARSE_BRICK_SIZE - 1, &maxZ)) {
        return false;
    }

    if (maxY <= TERRAIN_MIN_Y + 2) {
        return false;
    }

    const int32_t midX = minX + SPARSE_BRICK_SIZE / 2;
    const int32_t midZ = minZ + SPARSE_BRICK_SIZE / 2;
    float minHeight = static_cast<float>(TERRAIN_MAX_Y);
    const int32_t sampleX[3] = {minX, midX, maxX};
    const int32_t sampleZ[3] = {minZ, midZ, maxZ};
    for (int32_t z : sampleZ) {
        for (int32_t x : sampleX) {
            minHeight = std::min(minHeight, HeightAt(x, z));
        }
    }

    return static_cast<float>(maxY) < minHeight - verticalSafetyMargin;
}

bool SparseTerrainGenerator::MayContainExposedSurfaceBrick(
    const BrickCoord& coord,
    float verticalSafetyMargin) const
{
    int32_t minY = 0;
    int32_t maxY = 0;
    int32_t minX = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxZ = 0;
    if (!TryWorldVoxelFromBrickLocal(coord.y, 0, &minY) ||
        !TryWorldVoxelFromBrickLocal(coord.y, SPARSE_BRICK_SIZE - 1, &maxY) ||
        !TryWorldVoxelFromBrickLocal(coord.x, 0, &minX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, 0, &minZ) ||
        !TryWorldVoxelFromBrickLocal(coord.x, SPARSE_BRICK_SIZE - 1, &maxX) ||
        !TryWorldVoxelFromBrickLocal(coord.z, SPARSE_BRICK_SIZE - 1, &maxZ)) {
        return true;
    }

    const int32_t midX = minX + SPARSE_BRICK_SIZE / 2;
    const int32_t midZ = minZ + SPARSE_BRICK_SIZE / 2;
    const int32_t sampleX[3] = {minX, midX, maxX};
    const int32_t sampleZ[3] = {minZ, midZ, maxZ};
    float minHeight = static_cast<float>(TERRAIN_MAX_Y);
    float maxHeight = static_cast<float>(TERRAIN_MIN_Y);
    bool hasSubmergedColumn = false;
    for (int32_t z : sampleZ) {
        for (int32_t x : sampleX) {
            const float height = HeightAt(x, z);
            minHeight = std::min(minHeight, height);
            maxHeight = std::max(maxHeight, height);
            hasSubmergedColumn = hasSubmergedColumn || height < static_cast<float>(SEA_LEVEL_Y);
        }
    }

    const float margin = std::max(0.0f, verticalSafetyMargin);
    const float minSurfaceY = std::min(
        minHeight,
        hasSubmergedColumn ? static_cast<float>(SEA_LEVEL_Y) : minHeight);
    const float maxSurfaceY = std::max(
        maxHeight,
        hasSubmergedColumn ? static_cast<float>(SEA_LEVEL_Y) : maxHeight);

    return static_cast<float>(maxY) >= minSurfaceY - margin &&
           static_cast<float>(minY) <= maxSurfaceY + margin;
}

uint32_t SparseTerrainGenerator::SampleGeneratedVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    const float height = HeightAt(worldX, worldZ);
    const float relief = SurfaceReliefAt(worldX, worldZ, 4);
    return SampleGeneratedVoxelWithColumn(worldX, worldY, worldZ, height, relief);
}

uint32_t SparseTerrainGenerator::SampleGeneratedVoxelWithColumn(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    float height,
    float relief) const
{
    if (worldY <= TERRAIN_MIN_Y + 2) {
        const uint8_t variant = static_cast<uint8_t>(Hash3D(worldX, worldY, worldZ, m_seed) & 0xFFu);
        return Utils::PackVoxel(Utils::Material::Bedrock, variant, 0, Utils::StateFlags::IsStatic);
    }

    if (static_cast<float>(worldY) <= height) {
        const uint8_t variant = static_cast<uint8_t>(Hash3D(worldX, worldY, worldZ, m_seed) & 0xFFu);
        const float depth = height - static_cast<float>(worldY);
        const bool steepSurface = relief > 10.0f || height > 160.0f;
        uint8_t material = Utils::Material::Stone;
        const bool nearWaterlineBank =
            height >= static_cast<float>(SEA_LEVEL_Y - 2) &&
            height < static_cast<float>(SEA_LEVEL_Y + 72) &&
            worldY <= SEA_LEVEL_Y + 14 &&
            depth < 96.0f;
        const bool lowlandExposedBank =
            height >= static_cast<float>(SEA_LEVEL_Y + 18) &&
            height < static_cast<float>(SEA_LEVEL_Y + 128) &&
            worldY <= SEA_LEVEL_Y + 96 &&
            depth < 72.0f;
        const bool submergedTerrainColumn =
            height < static_cast<float>(SEA_LEVEL_Y);
        const bool dryOrIntertidalSurface =
            !submergedTerrainColumn &&
            height >= static_cast<float>(SEA_LEVEL_Y - 2) &&
            height < static_cast<float>(SEA_LEVEL_Y + 6);
        const bool lowlandShoreTop =
            height < static_cast<float>(SEA_LEVEL_Y + 72) &&
            depth < 4.0f;
        if (submergedTerrainColumn && depth < 6.0f) {
            material = Utils::Material::Dirt;
        } else if (dryOrIntertidalSurface && depth < 16.0f && relief < 14.0f) {
            material = Utils::Material::Sand;
        } else if (lowlandShoreTop) {
            material =
                (height < static_cast<float>(SEA_LEVEL_Y + 48) && relief < 36.0f)
                    ? Utils::Material::Sand
                    : Utils::Material::Dirt;
        } else if (nearWaterlineBank) {
            material =
                (height < static_cast<float>(SEA_LEVEL_Y + 72) && relief < 52.0f && depth < 96.0f)
                    ? Utils::Material::Sand
                    : Utils::Material::Dirt;
        } else if (lowlandExposedBank) {
            material =
                (height < static_cast<float>(SEA_LEVEL_Y + 86) && relief < 58.0f && depth < 42.0f)
                    ? Utils::Material::Sand
                    : Utils::Material::Dirt;
        } else if (depth < 2.0f && !steepSurface) {
            material = Utils::Material::Dirt;
        } else if (depth < 5.0f && relief < 6.0f) {
            material = Utils::Material::Dirt;
        }
        return Utils::PackVoxel(material, variant, 0, Utils::StateFlags::IsStatic);
    }

    if (worldY <= SEA_LEVEL_Y && height < static_cast<float>(SEA_LEVEL_Y)) {
        const uint8_t variant = static_cast<uint8_t>(Hash3D(worldX, worldY, worldZ, m_seed) & 0xFFu);
        return Utils::PackVoxel(Utils::Material::Water, variant, 0, 0);
    }

    return Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
}

uint32_t SparseTerrainGenerator::SampleGeneratedSurfaceVoxel(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    int32_t sampleStep) const
{
    const uint32_t voxel = SampleGeneratedVoxel(worldX, worldY, worldZ);
    const uint8_t material = Utils::UnpackMaterial(voxel);
    if (material == Utils::Material::Air) {
        return voxel;
    }

    const int32_t step = std::max(1, sampleStep);
    const int32_t directions[6][3] = {
        { 1, 0, 0 },
        { -1, 0, 0 },
        { 0, 1, 0 },
        { 0, -1, 0 },
        { 0, 0, 1 },
        { 0, 0, -1 },
    };

    // Mid/far LODs must mark true exposed low-resolution cells, not fabricate a
    // thickness shell. The backing voxel volume remains filled; this helper is
    // only the surface classifier used by render LOD metadata.
    for (const auto& direction : directions) {
        const uint32_t neighbor = SampleGeneratedVoxel(
            worldX + direction[0] * step,
            worldY + direction[1] * step,
            worldZ + direction[2] * step);
        const uint8_t neighborMaterial = Utils::UnpackMaterial(neighbor);
        if (neighborMaterial == Utils::Material::Air) {
            return voxel;
        }
        if (material == Utils::Material::Water && neighborMaterial != Utils::Material::Water) {
            return voxel;
        }
    }

    return Utils::PackVoxel(Utils::Material::Air, 0, 0, 0);
}

GeneratedSparseBrick SparseTerrainGenerator::GenerateBrick(const BrickCoord& coord) const {
    GeneratedSparseBrick brick;
    brick.coord = coord;
    brick.voxels.fill(Utils::PackVoxel(Utils::Material::Air, 0, 0, 0));

    int32_t worldXByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldYByLocal[SPARSE_BRICK_SIZE] = {};
    int32_t worldZByLocal[SPARSE_BRICK_SIZE] = {};
    for (uint8_t i = 0; i < SPARSE_BRICK_SIZE; ++i) {
        if (!TryWorldVoxelFromBrickLocal(coord.x, i, &worldXByLocal[i]) ||
            !TryWorldVoxelFromBrickLocal(coord.y, i, &worldYByLocal[i]) ||
            !TryWorldVoxelFromBrickLocal(coord.z, i, &worldZByLocal[i])) {
            ComputeOccupancyAndFlags(brick);
            return brick;
        }
    }

    float heightByColumn[SPARSE_BRICK_SIZE][SPARSE_BRICK_SIZE] = {};
    float reliefByColumn[SPARSE_BRICK_SIZE][SPARSE_BRICK_SIZE] = {};
    const int32_t minWorldY = worldYByLocal[0];
    const int32_t maxWorldY = worldYByLocal[SPARSE_BRICK_SIZE - 1];
    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
            const int32_t worldX = worldXByLocal[x];
            const int32_t worldZ = worldZByLocal[z];
            heightByColumn[z][x] = HeightAt(worldX, worldZ);
            if (maxWorldY > TERRAIN_MIN_Y + 2 &&
                static_cast<float>(minWorldY) <= heightByColumn[z][x]) {
                reliefByColumn[z][x] =
                    SurfaceReliefAtWithCenter(worldX, worldZ, heightByColumn[z][x], 4);
            }
        }
    }

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const int32_t worldX = worldXByLocal[x];
                const int32_t worldY = worldYByLocal[y];
                const int32_t worldZ = worldZByLocal[z];
                const float height = heightByColumn[z][x];
                const float relief = reliefByColumn[z][x];
                brick.voxels[LocalVoxelIndex({x, y, z})] =
                    SampleGeneratedVoxelWithColumn(worldX, worldY, worldZ, height, relief);
            }
        }
    }

    ComputeOccupancyAndFlags(brick);
    return brick;
}

void SparseTerrainGenerator::ComputeOccupancyAndFlags(GeneratedSparseBrick& brick) {
    bool anySolid = false;
    bool anyAir = false;
    bool anyWater = false;
    bool homogeneous = true;
    const uint8_t firstMaterial = Utils::UnpackMaterial(brick.voxels[0]);

    brick.occupancyWord0 = 0;
    brick.occupancyWord1 = 0;

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const uint32_t voxel = brick.voxels[LocalVoxelIndex({x, y, z})];
                const uint8_t material = Utils::UnpackMaterial(voxel);
                homogeneous = homogeneous && material == firstMaterial;
                if (material == Utils::Material::Air) {
                    anyAir = true;
                    continue;
                }

                anySolid = true;
                anyWater = anyWater || material == Utils::Material::Water;

                const uint32_t subX = x >> 2;
                const uint32_t subY = y >> 2;
                const uint32_t subZ = z >> 2;
                const uint32_t subIndex = subX + subY * 4u + subZ * 16u;
                if (subIndex < 32u) {
                    brick.occupancyWord0 |= 1u << subIndex;
                } else {
                    brick.occupancyWord1 |= 1u << (subIndex - 32u);
                }
            }
        }
    }

    uint32_t flags = 0;
    if (!anySolid) {
        flags = AddFlag(flags, BrickResidencyFlags::Empty);
    }
    if (anySolid && !anyAir && !anyWater) {
        flags = AddFlag(flags, BrickResidencyFlags::Solid);
    }
    if (homogeneous) {
        flags = AddFlag(flags, BrickResidencyFlags::Homogeneous);
    }
    if (anyWater) {
        flags = AddFlag(flags, BrickResidencyFlags::HasWater);
    }
    brick.flags = flags;
}

} // namespace VENPOD::Simulation
