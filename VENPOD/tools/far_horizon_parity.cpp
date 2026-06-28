#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr float kFarTerrainMinHeight = -332.0f;
constexpr float kFarTerrainMaxHeight = 664.0f;
constexpr float kFarSeaLevel = -48.0f;
constexpr uint32_t kDefaultSeed = 12345u;

float Saturate(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float Smooth01(float value) {
    value = Saturate(value);
    return value * value * (3.0f - 2.0f * value);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

uint32_t Hash3D(int32_t x, int32_t y, int32_t z, uint32_t seed) {
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

float ValueNoise2D(float x, float z, uint32_t seed) {
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t z0 = static_cast<int32_t>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const float sx = Smooth01(fx);
    const float sz = Smooth01(fz);
    const float s00 = static_cast<float>(Hash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s10 = static_cast<float>(Hash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s01 = static_cast<float>(Hash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    const float s11 = static_cast<float>(Hash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
    return Lerp(Lerp(s00, s10, sx), Lerp(s01, s11, sx), sz) * 2.0f - 1.0f;
}

struct TerrainSample {
    float height = 0.0f;
    float mountainMask = 0.0f;
};

TerrainSample FarTerrainHeight(float x, float z, uint32_t seed) {
    const float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, seed + 11u);
    const float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, seed + 23u);
    const float ridge = 1.0f - std::abs(ridgeSource);
    const float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, seed + 37u);
    const float ridgeHeight = ridge * ridge;

    float height = -64.0f + broad * 92.0f + ridgeHeight * 150.0f + detail * 3.0f;
    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    const float originComfort = 1.0f - Smooth01((originDistance - 180.0f) / 520.0f);
    const float publicRegionHeight =
        -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f +
        (1.0f - Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - Smooth01(originDistance / 420.0f)) * 58.0f;
    height = Lerp(height, publicRegionHeight, originComfort * 0.94f);
    const float publicCapInfluence = 1.0f - Smooth01((originDistance - 220.0f) / 420.0f);
    const float publicCap = 58.0f + Smooth01(originDistance / 640.0f) * 114.0f;
    height = Lerp(height, std::min(height, publicCap), publicCapInfluence);

    const float submergedBlend = 1.0f - Smooth01((height - (kFarSeaLevel + 28.0f)) / 86.0f);
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            (kFarSeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f +
            (1.0f - Smooth01(originDistance / 520.0f)) * 18.0f;
        height = Lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand = 1.0f - Smooth01((originDistance - 260.0f) / 980.0f);
    const float lowlandUpper = 1.0f - Smooth01((height - (kFarSeaLevel + 96.0f)) / 120.0f);
    const float lowlandFloor = Smooth01((height - (kFarSeaLevel - 40.0f)) / 64.0f);
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    const float playableShelfHeight =
        (kFarSeaLevel + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f +
        (1.0f - Smooth01(originDistance / 460.0f)) * 42.0f;
    height = Lerp(height, playableShelfHeight, playableBankBlend);

    const float publicBasinBand =
        Smooth01((originDistance - 360.0f) / 240.0f) *
        (1.0f - Smooth01((originDistance - 1700.0f) / 760.0f)) *
        Smooth01((height - (kFarSeaLevel - 38.0f)) / 56.0f) *
        (1.0f - Smooth01((height - (kFarSeaLevel + 180.0f)) / 140.0f));
    const float publicBasinFloor = (kFarSeaLevel - 12.0f) + broad * 2.0f + detail * 0.35f;
    height = Lerp(height, std::min(height, publicBasinFloor), publicBasinBand * 0.80f);

    const float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, seed + 211u);
    const float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, seed + 227u);
    const float backdropRidge = 1.0f - std::abs(backdropRidgeSource);
    const float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, seed + 271u);
    const float backdropNotch = Smooth01((backdropBreakup - 0.08f) / 0.58f);
    const float silhouetteRidge = Saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    const float backdropBand =
        Smooth01((originDistance - 1360.0f) / 700.0f) *
        (1.0f - Smooth01((originDistance - 5200.0f) / 1200.0f));
    const float northBackdrop = Smooth01((z - 1180.0f) / 900.0f);
    const float sideBackdrop = Smooth01((std::abs(x - 192.0f) - 820.0f) / 980.0f);
    const float backdropFacing = Saturate(northBackdrop + sideBackdrop * 0.58f);
    const float silhouetteContinuity = Saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
    const float backdropInfluence =
        backdropBand * backdropFacing * Smooth01(silhouetteContinuity) * (0.46f + backdropNotch * 0.54f);
    const float backdropHeight = 248.0f + backdropBand * 160.0f + silhouetteContinuity * 186.0f + backdropNoise * 26.0f;
    height = Lerp(height, std::max(height, backdropHeight), backdropInfluence * 0.70f);

    const float westCorridor = Smooth01((192.0f - x - 520.0f) / 820.0f);
    const float eastCorridor = Smooth01((x - 192.0f - 520.0f) / 820.0f);
    const float southBlend = Smooth01((360.0f - z) / 1200.0f);
    const float westNorthBlend = Smooth01((z - 360.0f) / 920.0f);
    const float routeDistanceBand =
        Smooth01((originDistance - 780.0f) / 420.0f) *
        (1.0f - Smooth01((originDistance - 4300.0f) / 1200.0f));
    const float routeCorridor = routeDistanceBand * Saturate(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend);
    const float routeRidgeNoiseA = ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, seed + 251u);
    const float routeRidgeNoiseB = ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, seed + 263u);
    const float routeBreakup = ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, seed + 281u);
    const float routeNotch = Smooth01((routeBreakup - 0.02f) / 0.60f);
    const float routeRidge = Saturate(0.26f + (1.0f - std::abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f);
    const float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
    height = Lerp(height, std::max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);

    TerrainSample sample;
    sample.height = std::clamp(height, kFarTerrainMinHeight, kFarTerrainMaxHeight);
    sample.mountainMask = Saturate((ridgeHeight * 150.0f + std::max(height - 160.0f, 0.0f)) / 300.0f);
    return sample;
}

float QuantizeTerrainTopHeight(float height, float verticalStep) {
    verticalStep = std::max(verticalStep, 1.0f);
    return std::ceil(height / verticalStep) * verticalStep;
}

float FarFallbackCellSize(float distanceFromCamera) {
    const float t = Saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) return 8.0f;
    if (t < 0.42f) return 12.0f;
    if (t < 0.68f) return 18.0f;
    return 28.0f;
}

float FarFallbackCellCenterCoord(float value, float cellSize) {
    return (std::floor(value / cellSize) + 0.5f) * cellSize;
}

float FarSpawnLandBand(float x, float z) {
    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    // Offline parity targets settled/post-startup validation frames where the
    // runtime mid-residency latch has enabled the spawn-land reshape.
    return 1.0f - Smooth01((originDistance - 200.0f) / 90000.0f);
}

float FarSpawnLandFloorBase(float x, float z, uint32_t seed) {
    const float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, seed + 11u);
    const float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, seed + 37u);
    return (kFarSeaLevel + 56.0f) + broad * 18.0f + detail * 3.0f;
}

float FarSpawnLandReshapeHeight(float x, float z, float height, float mountainMask, uint32_t seed) {
    const float band = FarSpawnLandBand(x, z);
    const float floorHeight = FarSpawnLandFloorBase(x, z, seed) + mountainMask * 80.0f;
    return Lerp(height, std::max(height, floorHeight), band);
}

TerrainSample FarTerrainHeightVoxelized(float x, float z, float distanceFromCamera, uint32_t seed) {
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    const float sampleX = FarFallbackCellCenterCoord(x, cellSize);
    const float sampleZ = FarFallbackCellCenterCoord(z, cellSize);
    TerrainSample sample = FarTerrainHeight(sampleX, sampleZ, seed);
    sample.height = QuantizeTerrainTopHeight(
        FarSpawnLandReshapeHeight(sampleX, sampleZ, sample.height, sample.mountainMask, seed),
        std::max(4.0f, cellSize * 0.75f));
    return sample;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Normalize(Vec3 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 1.0e-8f) {
        return {};
    }
    return {v.x / len, v.y / len, v.z / len};
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Mul(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct DiagnosticWorkStats {
    int queries = 0;
    int hits = 0;
    int misses = 0;
    int earlyRejects = 0;
    int skyBreakMisses = 0;
    int deepMisses = 0;
    int steps = 0;
    int heightEvals = 0;
};

bool DiagnosticFarTerrainWouldHit(
    Vec3 rayOrigin,
    Vec3 rayDir,
    float startDist,
    uint32_t seed,
    float& hitT,
    DiagnosticWorkStats* workStats = nullptr)
{
    if (workStats) {
        ++workStats->queries;
    }
    hitT = 1.0e20f;
    if (rayDir.y > 0.42f || rayDir.y < -0.92f) {
        if (workStats) {
            ++workStats->earlyRejects;
            ++workStats->misses;
        }
        return false;
    }

    constexpr float farMaxDist = 10400.0f;
    constexpr float farTerrainCeiling = kFarTerrainMaxHeight + 64.0f;
    float t = std::max(startDist, 160.0f);
    Vec3 previousPos = Add(rayOrigin, Mul(rayDir, t));
    if (workStats) {
        ++workStats->heightEvals;
    }
    float previousHeight = FarTerrainHeightVoxelized(previousPos.x, previousPos.z, t, seed).height;
    float previousSigned = previousPos.y - previousHeight;

    if (previousSigned <= 0.0f) {
        if (workStats) {
            ++workStats->heightEvals;
        }
        const float originHeight = FarTerrainHeightVoxelized(rayOrigin.x, rayOrigin.z, 0.0f, seed).height;
        if (rayOrigin.y > originHeight) {
            hitT = t;
            if (workStats) {
                ++workStats->hits;
            }
            return true;
        }
    }

    for (int i = 0; i < 112 && t < farMaxDist; ++i) {
        if (workStats) {
            ++workStats->steps;
        }
        const bool nearSkylineProbe =
            rayOrigin.y <= 384.0f && rayDir.y > -0.06f && rayDir.y < 0.24f && t < 3600.0f;
        const float distanceStep = nearSkylineProbe
            ? 36.0f
            : Lerp(56.0f, 220.0f, Saturate(t / farMaxDist));
        float stepSize = distanceStep;
        if (previousSigned > 0.0f && rayDir.y < -0.015f) {
            const float verticalStep = previousSigned / std::max(-rayDir.y, 0.030f);
            stepSize = std::clamp(verticalStep * 0.50f, nearSkylineProbe ? 12.0f : 24.0f, distanceStep);
        }
        t += stepSize;
        const Vec3 pos = Add(rayOrigin, Mul(rayDir, t));
        if (rayDir.y >= 0.0f && pos.y > farTerrainCeiling) {
            if (workStats) {
                ++workStats->skyBreakMisses;
                ++workStats->misses;
            }
            return false;
        }
        if (workStats) {
            ++workStats->heightEvals;
        }
        const float height = FarTerrainHeightVoxelized(pos.x, pos.z, t, seed).height;
        const float signedDistance = pos.y - height;
        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            hitT = t;
            if (workStats) {
                ++workStats->hits;
            }
            return true;
        }
        previousSigned = signedDistance;
    }
    if (workStats) {
        ++workStats->deepMisses;
        ++workStats->misses;
    }
    return false;
}

struct HorizonBin {
    float slope = -1.0e20f;
    float horizontalDistance = 1.0e20f;
};

struct SlopeTable {
    int azimuthBins = 0;
    int slopeBins = 0;
    float minSlope = 0.0f;
    float maxSlope = 0.0f;
    std::vector<float> firstHorizontalDistance;
};

struct HeightGridCache {
    float originX = 0.0f;
    float originZ = 0.0f;
    float cellSize = 32.0f;
    int cellsX = 0;
    int cellsZ = 0;
    std::vector<float> maxHeight;
    std::vector<size_t> levelOffsets;
    std::vector<int> levelCellsX;
    std::vector<int> levelCellsZ;
    std::vector<float> mipMaxHeight;
};

struct ScreenMaskCache {
    int tilesX = 0;
    int tileWidth = 8;
    std::vector<float> horizonY;
};

struct Options {
    uint32_t seed = kDefaultSeed;
    int bins = 4096;
    int slopeBins = 192;
    int pixelStep = 8;
    int width = 1920;
    int height = 1080;
    int yMin = 190;
    int yMax = 340;
    float cameraX = -197.88f;
    float cameraY = 346.0f;
    float cameraZ = 187.96f;
    float forwardX = -0.763f;
    float forwardY = -0.276f;
    float forwardZ = 0.584f;
    float fovY = 60.0f * 3.1415926535f / 180.0f;
    float aspect = 16.0f / 9.0f;
    float startDistance = 256.0f;
    float maxDistance = 10400.0f;
    float missSlopeSlack = 0.0f;
    float hitTRelTolerance = 0.35f;
    float minSlope = -1.5f;
    float maxSlope = 0.6f;
    std::string mode = "slope";
    bool conservativeOnly = false;
    float gridCellSize = 32.0f;
    float gridExtent = 10912.0f;
    float gridQueryStep = 12.0f;
    float gridHeightPad = 12.0f;
    int gridSubsamples = 3;
    bool gridExactRefine = false;
    bool gridRefineReject = false;
    float gridRefineWindow = 144.0f;
    float gridRefineStep = 12.0f;
    bool structuralCheck = false;
    int structuralSamples = 3;
    bool fallbackDiagnostic = false;
    bool hierarchicalDda = false;
    int hierarchicalDdaMaxLevel = -1;
    float hierarchicalDdaMinRayY = -2.0f;
    bool requireNetWorkGain = false;
    float diagnosticHeightEvalWork = 1.0f;
    float cacheCellWork = 0.25f;
    float refineHeightEvalWork = 1.0f;
    bool cohortDiagnostics = false;
    int screenMaskTileWidth = 8;
    float screenMaskDilationPixels = 2.0f;
    int traceX = -1;
    int traceY = -1;
};

struct StructuralCheckResult {
    int leafSamples = 0;
    int leafViolations = 0;
    float maxLeafViolation = 0.0f;
    int parentChecks = 0;
    int parentViolations = 0;
    float maxParentViolation = 0.0f;
};

struct QueryStats {
    int ddaQueries = 0;
    int64_t ddaVisitedCells = 0;
    int64_t ddaSkippedBlocks = 0;
    int64_t ddaSkippedLeafCells = 0;
    int ddaMaxMipLevelUsed = 0;
    int ddaPossibleCells = 0;
    int ddaRefineCalls = 0;
    int ddaRefineHits = 0;
    int64_t ddaRefineHeightEvals = 0;
    int ddaEnvelopeFallbacks = 0;
    int ddaOutOfGridHits = 0;
    int ddaIterationBailouts = 0;
};

bool gTraceDda = false;
float gTraceTruthT = 1.0e20f;

struct CohortStats {
    int count = 0;
    int64_t heightEvals = 0;
    int64_t steps = 0;
    double rayYSum = 0.0;
    float minRayY = std::numeric_limits<float>::infinity();
    float maxRayY = -std::numeric_limits<float>::infinity();
    double predictedTSum = 0.0;
    int predictedTSamples = 0;
    double clearanceSum = 0.0;
    int clearanceSamples = 0;
    float minClearance = std::numeric_limits<float>::infinity();
    float maxClearance = -std::numeric_limits<float>::infinity();
    int clearanceLe8 = 0;
    int clearanceLe32 = 0;
    int clearanceLe96 = 0;
    int clearanceGt96 = 0;
    std::array<int, 8> rayYBins{};
    std::array<int, 8> screenYBins{};
};

int BinClamped(float value, float minValue, float maxValue, int binCount) {
    if (binCount <= 1) {
        return 0;
    }
    const float t = Saturate((value - minValue) / std::max(maxValue - minValue, 1.0e-6f));
    return std::min(binCount - 1, static_cast<int>(std::floor(t * static_cast<float>(binCount))));
}

void RecordCohort(
    CohortStats& stats,
    const Options& opt,
    Vec3 camera,
    Vec3 rayDir,
    int screenY,
    float predictedT,
    const DiagnosticWorkStats& truthWork)
{
    ++stats.count;
    stats.heightEvals += truthWork.heightEvals;
    stats.steps += truthWork.steps;
    stats.rayYSum += rayDir.y;
    stats.minRayY = std::min(stats.minRayY, rayDir.y);
    stats.maxRayY = std::max(stats.maxRayY, rayDir.y);
    ++stats.rayYBins[static_cast<size_t>(BinClamped(rayDir.y, -0.92f, 0.42f, 8))];
    ++stats.screenYBins[static_cast<size_t>(BinClamped(
        static_cast<float>(screenY),
        static_cast<float>(opt.yMin),
        static_cast<float>(std::max(opt.yMax, opt.yMin + 1)),
        8))];
    if (std::isfinite(predictedT) && predictedT < 1.0e19f) {
        const float t = std::clamp(predictedT, std::max(opt.startDistance, 160.0f), opt.maxDistance);
        const Vec3 pos = Add(camera, Mul(rayDir, t));
        const float height = FarTerrainHeightVoxelized(pos.x, pos.z, t, opt.seed).height;
        const float clearance = pos.y - height;
        stats.predictedTSum += t;
        ++stats.predictedTSamples;
        stats.clearanceSum += clearance;
        ++stats.clearanceSamples;
        stats.minClearance = std::min(stats.minClearance, clearance);
        stats.maxClearance = std::max(stats.maxClearance, clearance);
        if (clearance <= 8.0f) {
            ++stats.clearanceLe8;
        } else if (clearance <= 32.0f) {
            ++stats.clearanceLe32;
        } else if (clearance <= 96.0f) {
            ++stats.clearanceLe96;
        } else {
            ++stats.clearanceGt96;
        }
    }
}

std::string ArrayCsv(const std::array<int, 8>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += std::to_string(values[i]);
    }
    return out;
}

void PrintCohort(const char* name, const CohortStats& stats) {
    const double avgRayY = stats.count > 0 ? stats.rayYSum / static_cast<double>(stats.count) : 0.0;
    const double avgHeightEvals = stats.count > 0
        ? static_cast<double>(stats.heightEvals) / static_cast<double>(stats.count)
        : 0.0;
    const double avgPredictedT = stats.predictedTSamples > 0
        ? stats.predictedTSum / static_cast<double>(stats.predictedTSamples)
        : 0.0;
    const double avgClearance = stats.clearanceSamples > 0
        ? stats.clearanceSum / static_cast<double>(stats.clearanceSamples)
        : 0.0;
    std::cout
        << " COHORT_" << name
        << " count=" << stats.count
        << " heightEvals=" << stats.heightEvals
        << " avgHeightEvals=" << avgHeightEvals
        << " steps=" << stats.steps
        << " avgRayY=" << avgRayY
        << " minRayY=" << (stats.count > 0 ? stats.minRayY : 0.0f)
        << " maxRayY=" << (stats.count > 0 ? stats.maxRayY : 0.0f)
        << " predictedTSamples=" << stats.predictedTSamples
        << " avgPredictedT=" << avgPredictedT
        << " clearanceSamples=" << stats.clearanceSamples
        << " avgClearance=" << avgClearance
        << " minClearance=" << (stats.clearanceSamples > 0 ? stats.minClearance : 0.0f)
        << " maxClearance=" << (stats.clearanceSamples > 0 ? stats.maxClearance : 0.0f)
        << " clearanceLe8/Le32/Le96/Gt96="
        << stats.clearanceLe8 << "/" << stats.clearanceLe32 << "/"
        << stats.clearanceLe96 << "/" << stats.clearanceGt96
        << " rayYBins=" << ArrayCsv(stats.rayYBins)
        << " screenYBins=" << ArrayCsv(stats.screenYBins);
}

bool ReadIntArg(int argc, char** argv, int& i, int& out) {
    if (i + 1 >= argc) return false;
    out = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    return true;
}

bool ReadFloatArg(int argc, char** argv, int& i, float& out) {
    if (i + 1 >= argc) return false;
    out = std::strtof(argv[++i], nullptr);
    return std::isfinite(out);
}

bool ReadUintArg(int argc, char** argv, int& i, uint32_t& out) {
    if (i + 1 >= argc) return false;
    out = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    return true;
}

bool ParseOptions(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bins") == 0) { if (!ReadIntArg(argc, argv, i, opt.bins)) return false; }
        else if (std::strcmp(argv[i], "--slope-bins") == 0) { if (!ReadIntArg(argc, argv, i, opt.slopeBins)) return false; }
        else if (std::strcmp(argv[i], "--pixel-step") == 0) { if (!ReadIntArg(argc, argv, i, opt.pixelStep)) return false; }
        else if (std::strcmp(argv[i], "--y-min") == 0) { if (!ReadIntArg(argc, argv, i, opt.yMin)) return false; }
        else if (std::strcmp(argv[i], "--y-max") == 0) { if (!ReadIntArg(argc, argv, i, opt.yMax)) return false; }
        else if (std::strcmp(argv[i], "--seed") == 0) { if (!ReadUintArg(argc, argv, i, opt.seed)) return false; }
        else if (std::strcmp(argv[i], "--camera-x") == 0) { if (!ReadFloatArg(argc, argv, i, opt.cameraX)) return false; }
        else if (std::strcmp(argv[i], "--camera-y") == 0) { if (!ReadFloatArg(argc, argv, i, opt.cameraY)) return false; }
        else if (std::strcmp(argv[i], "--camera-z") == 0) { if (!ReadFloatArg(argc, argv, i, opt.cameraZ)) return false; }
        else if (std::strcmp(argv[i], "--forward-x") == 0) { if (!ReadFloatArg(argc, argv, i, opt.forwardX)) return false; }
        else if (std::strcmp(argv[i], "--forward-y") == 0) { if (!ReadFloatArg(argc, argv, i, opt.forwardY)) return false; }
        else if (std::strcmp(argv[i], "--forward-z") == 0) { if (!ReadFloatArg(argc, argv, i, opt.forwardZ)) return false; }
        else if (std::strcmp(argv[i], "--start-distance") == 0) { if (!ReadFloatArg(argc, argv, i, opt.startDistance)) return false; }
        else if (std::strcmp(argv[i], "--hit-t-rel-tolerance") == 0) { if (!ReadFloatArg(argc, argv, i, opt.hitTRelTolerance)) return false; }
        else if (std::strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) return false;
            opt.mode = argv[++i];
        }
        else if (std::strcmp(argv[i], "--conservative-only") == 0) { opt.conservativeOnly = true; }
        else if (std::strcmp(argv[i], "--grid-cell-size") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridCellSize)) return false; }
        else if (std::strcmp(argv[i], "--grid-extent") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridExtent)) return false; }
        else if (std::strcmp(argv[i], "--grid-query-step") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridQueryStep)) return false; }
        else if (std::strcmp(argv[i], "--grid-height-pad") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridHeightPad)) return false; }
        else if (std::strcmp(argv[i], "--grid-subsamples") == 0) { if (!ReadIntArg(argc, argv, i, opt.gridSubsamples)) return false; }
        else if (std::strcmp(argv[i], "--grid-exact-refine") == 0) { opt.gridExactRefine = true; }
        else if (std::strcmp(argv[i], "--grid-refine-reject") == 0) { opt.gridRefineReject = true; }
        else if (std::strcmp(argv[i], "--grid-refine-window") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridRefineWindow)) return false; }
        else if (std::strcmp(argv[i], "--grid-refine-step") == 0) { if (!ReadFloatArg(argc, argv, i, opt.gridRefineStep)) return false; }
        else if (std::strcmp(argv[i], "--structural-check") == 0) { opt.structuralCheck = true; }
        else if (std::strcmp(argv[i], "--structural-samples") == 0) { if (!ReadIntArg(argc, argv, i, opt.structuralSamples)) return false; }
        else if (std::strcmp(argv[i], "--fallback-diagnostic") == 0) { opt.fallbackDiagnostic = true; }
        else if (std::strcmp(argv[i], "--hierarchical-dda") == 0) { opt.hierarchicalDda = true; }
        else if (std::strcmp(argv[i], "--hierarchical-dda-max-level") == 0) { if (!ReadIntArg(argc, argv, i, opt.hierarchicalDdaMaxLevel)) return false; }
        else if (std::strcmp(argv[i], "--hierarchical-dda-min-ray-y") == 0) { if (!ReadFloatArg(argc, argv, i, opt.hierarchicalDdaMinRayY)) return false; }
        else if (std::strcmp(argv[i], "--require-net-work-gain") == 0) { opt.requireNetWorkGain = true; }
        else if (std::strcmp(argv[i], "--diagnostic-height-eval-work") == 0) { if (!ReadFloatArg(argc, argv, i, opt.diagnosticHeightEvalWork)) return false; }
        else if (std::strcmp(argv[i], "--cache-cell-work") == 0) { if (!ReadFloatArg(argc, argv, i, opt.cacheCellWork)) return false; }
        else if (std::strcmp(argv[i], "--refine-height-eval-work") == 0) { if (!ReadFloatArg(argc, argv, i, opt.refineHeightEvalWork)) return false; }
        else if (std::strcmp(argv[i], "--cohort-diagnostics") == 0) { opt.cohortDiagnostics = true; }
        else if (std::strcmp(argv[i], "--screen-mask-tile-width") == 0) { if (!ReadIntArg(argc, argv, i, opt.screenMaskTileWidth)) return false; }
        else if (std::strcmp(argv[i], "--screen-mask-dilation-pixels") == 0) { if (!ReadFloatArg(argc, argv, i, opt.screenMaskDilationPixels)) return false; }
        else if (std::strcmp(argv[i], "--trace-x") == 0) { if (!ReadIntArg(argc, argv, i, opt.traceX)) return false; }
        else if (std::strcmp(argv[i], "--trace-y") == 0) { if (!ReadIntArg(argc, argv, i, opt.traceY)) return false; }
        else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return false;
        }
    }
    return opt.bins > 0 && opt.slopeBins > 1 && opt.pixelStep > 0 && opt.width > 0 &&
        opt.height > 0 && opt.yMin <= opt.yMax && opt.minSlope < opt.maxSlope &&
        (opt.mode == "slope" || opt.mode == "grid" || opt.mode == "mipdda" || opt.mode == "screenmask") &&
        opt.gridCellSize > 0.0f && opt.gridExtent > 0.0f &&
        opt.gridQueryStep > 0.0f && opt.gridSubsamples > 0 &&
        opt.gridRefineWindow >= 0.0f && opt.gridRefineStep > 0.0f &&
        opt.structuralSamples > 0 &&
        opt.screenMaskTileWidth > 0 && opt.screenMaskDilationPixels >= 0.0f &&
        opt.diagnosticHeightEvalWork >= 0.0f && opt.cacheCellWork >= 0.0f &&
        opt.refineHeightEvalWork >= 0.0f;
}

std::vector<HorizonBin> BuildHorizonTable(const Options& opt, Vec3 camera) {
    std::vector<HorizonBin> table(static_cast<size_t>(opt.bins));
    constexpr float twoPi = 6.283185307179586f;
    for (int bin = 0; bin < opt.bins; ++bin) {
        const float az = (static_cast<float>(bin) + 0.5f) * twoPi / static_cast<float>(opt.bins);
        const float dirX = std::cos(az);
        const float dirZ = std::sin(az);
        for (float s = std::max(opt.startDistance, 160.0f); s <= opt.maxDistance; s += 36.0f) {
            const TerrainSample sample = FarTerrainHeightVoxelized(
                camera.x + dirX * s,
                camera.z + dirZ * s,
                s,
                opt.seed);
            const float slope = (sample.height - camera.y) / std::max(s, 1.0f);
            if (slope > table[static_cast<size_t>(bin)].slope) {
                table[static_cast<size_t>(bin)].slope = slope;
                table[static_cast<size_t>(bin)].horizontalDistance = s;
            }
        }
    }
    return table;
}

bool HorizonClassify(const std::vector<HorizonBin>& table, const Options& opt, Vec3 rayDir, float& hitT) {
    const float horizLen = std::sqrt(rayDir.x * rayDir.x + rayDir.z * rayDir.z);
    if (horizLen <= 1.0e-6f || rayDir.y > 0.42f || rayDir.y < -0.92f) {
        hitT = 1.0e20f;
        return false;
    }
    float az = std::atan2(rayDir.z, rayDir.x);
    if (az < 0.0f) {
        az += 6.283185307179586f;
    }
    const float binFloat = az * static_cast<float>(opt.bins) / 6.283185307179586f - 0.5f;
    const int bin0 = static_cast<int>(std::floor(binFloat));
    const float frac = binFloat - static_cast<float>(bin0);
    const int i0 = (bin0 % opt.bins + opt.bins) % opt.bins;
    const int i1 = (i0 + 1) % opt.bins;
    const HorizonBin& a = table[static_cast<size_t>(i0)];
    const HorizonBin& b = table[static_cast<size_t>(i1)];
    const float slope = Lerp(a.slope, b.slope, frac);
    const float horizontalDistance = Lerp(a.horizontalDistance, b.horizontalDistance, frac);
    const float raySlope = rayDir.y / horizLen;
    if (raySlope > slope + opt.missSlopeSlack) {
        hitT = 1.0e20f;
        return false;
    }
    hitT = horizontalDistance / horizLen;
    return hitT >= opt.startDistance;
}

SlopeTable BuildSlopeTable(const Options& opt, Vec3 camera) {
    SlopeTable table;
    table.azimuthBins = opt.bins;
    table.slopeBins = opt.slopeBins;
    table.minSlope = opt.minSlope;
    table.maxSlope = opt.maxSlope;
    table.firstHorizontalDistance.assign(
        static_cast<size_t>(opt.bins * opt.slopeBins),
        1.0e20f);

    constexpr float twoPi = 6.283185307179586f;
    const float slopeStep = (opt.maxSlope - opt.minSlope) / static_cast<float>(opt.slopeBins - 1);
    for (int bin = 0; bin < opt.bins; ++bin) {
        const float az = (static_cast<float>(bin) + 0.5f) * twoPi / static_cast<float>(opt.bins);
        const float dirX = std::cos(az);
        const float dirZ = std::sin(az);
        for (float s = std::max(opt.startDistance, 160.0f); s <= opt.maxDistance; s += 36.0f) {
            const TerrainSample sample = FarTerrainHeightVoxelized(
                camera.x + dirX * s,
                camera.z + dirZ * s,
                s,
                opt.seed);
            const float terrainSlope = (sample.height - camera.y) / std::max(s, 1.0f);
            const int maxSlopeIndex =
                static_cast<int>(std::floor((terrainSlope - opt.minSlope) / slopeStep));
            const int clampedMax = std::clamp(maxSlopeIndex, -1, opt.slopeBins - 1);
            for (int si = 0; si <= clampedMax; ++si) {
                float& first =
                    table.firstHorizontalDistance[static_cast<size_t>(bin * opt.slopeBins + si)];
                if (first >= 1.0e19f) {
                    first = s;
                }
            }
        }
    }
    return table;
}

bool SlopeTableClassify(const SlopeTable& table, const Options& opt, Vec3 camera, Vec3 rayDir, float& hitT) {
    const float horizLen = std::sqrt(rayDir.x * rayDir.x + rayDir.z * rayDir.z);
    if (horizLen <= 1.0e-6f || rayDir.y > 0.42f || rayDir.y < -0.92f) {
        hitT = 1.0e20f;
        return false;
    }
    const float raySlope = rayDir.y / horizLen;
    if (raySlope < table.minSlope || raySlope > table.maxSlope) {
        hitT = 1.0e20f;
        return false;
    }

    float az = std::atan2(rayDir.z, rayDir.x);
    if (az < 0.0f) {
        az += 6.283185307179586f;
    }
    const int azIndex = std::clamp(
        static_cast<int>(std::floor(az * static_cast<float>(table.azimuthBins) / 6.283185307179586f)),
        0,
        table.azimuthBins - 1);
    const float slopeU = (raySlope - table.minSlope) / (table.maxSlope - table.minSlope);
    const int slopeIndex = std::clamp(
        static_cast<int>(std::floor(slopeU * static_cast<float>(table.slopeBins - 1))),
        0,
        table.slopeBins - 1);
    const float horizontalDistance =
        table.firstHorizontalDistance[static_cast<size_t>(azIndex * table.slopeBins + slopeIndex)];
    if (horizontalDistance >= 1.0e19f) {
        hitT = 1.0e20f;
        return false;
    }
    hitT = horizontalDistance / horizLen;
    const Vec3 candidate = Add(camera, Mul(rayDir, hitT));
    const float height = FarTerrainHeightVoxelized(candidate.x, candidate.z, hitT, opt.seed).height;
    if (candidate.y > height + 2.0f) {
        hitT = 1.0e20f;
        return false;
    }
    return true;
}

HeightGridCache BuildHeightGridCache(const Options& opt, Vec3 camera) {
    HeightGridCache cache;
    cache.cellSize = opt.gridCellSize;
    cache.originX = std::floor((camera.x - opt.gridExtent) / cache.cellSize) * cache.cellSize;
    cache.originZ = std::floor((camera.z - opt.gridExtent) / cache.cellSize) * cache.cellSize;
    cache.cellsX = static_cast<int>(std::ceil((opt.gridExtent * 2.0f) / cache.cellSize)) + 2;
    cache.cellsZ = static_cast<int>(std::ceil((opt.gridExtent * 2.0f) / cache.cellSize)) + 2;
    cache.maxHeight.assign(static_cast<size_t>(cache.cellsX * cache.cellsZ), kFarTerrainMinHeight);

    const int subsamples = std::max(1, opt.gridSubsamples);
    for (int z = 0; z < cache.cellsZ; ++z) {
        for (int x = 0; x < cache.cellsX; ++x) {
            float maxHeight = kFarTerrainMinHeight;
            for (int sy = 0; sy < subsamples; ++sy) {
                for (int sx = 0; sx < subsamples; ++sx) {
                    const float ux = (static_cast<float>(sx) + 0.5f) / static_cast<float>(subsamples);
                    const float uz = (static_cast<float>(sy) + 0.5f) / static_cast<float>(subsamples);
                    const float sampleX = cache.originX + (static_cast<float>(x) + ux) * cache.cellSize;
                    const float sampleZ = cache.originZ + (static_cast<float>(z) + uz) * cache.cellSize;
                    const float sampleDistance = std::sqrt(
                        (sampleX - camera.x) * (sampleX - camera.x) +
                        (sampleZ - camera.z) * (sampleZ - camera.z));
                    maxHeight = std::max(
                        maxHeight,
                        FarTerrainHeightVoxelized(sampleX, sampleZ, sampleDistance, opt.seed).height);
                }
            }
            cache.maxHeight[static_cast<size_t>(z * cache.cellsX + x)] =
                std::min(kFarTerrainMaxHeight, maxHeight + opt.gridHeightPad);
        }
    }

    cache.levelOffsets.push_back(0u);
    cache.levelCellsX.push_back(cache.cellsX);
    cache.levelCellsZ.push_back(cache.cellsZ);
    cache.mipMaxHeight = cache.maxHeight;
    int prevX = cache.cellsX;
    int prevZ = cache.cellsZ;
    size_t prevOffset = 0u;
    while (prevX > 1 || prevZ > 1) {
        const int nextX = std::max(1, (prevX + 1) / 2);
        const int nextZ = std::max(1, (prevZ + 1) / 2);
        const size_t nextOffset = cache.mipMaxHeight.size();
        cache.levelOffsets.push_back(nextOffset);
        cache.levelCellsX.push_back(nextX);
        cache.levelCellsZ.push_back(nextZ);
        cache.mipMaxHeight.resize(nextOffset + static_cast<size_t>(nextX * nextZ), kFarTerrainMinHeight);
        for (int z = 0; z < nextZ; ++z) {
            for (int x = 0; x < nextX; ++x) {
                float maxHeight = kFarTerrainMinHeight;
                for (int dz = 0; dz < 2; ++dz) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int childX = x * 2 + dx;
                        const int childZ = z * 2 + dz;
                        if (childX < prevX && childZ < prevZ) {
                            maxHeight = std::max(
                                maxHeight,
                                cache.mipMaxHeight[prevOffset + static_cast<size_t>(childZ * prevX + childX)]);
                        }
                    }
                }
                cache.mipMaxHeight[nextOffset + static_cast<size_t>(z * nextX + x)] = maxHeight;
            }
        }
        prevX = nextX;
        prevZ = nextZ;
        prevOffset = nextOffset;
    }
    return cache;
}

float HeightGridMaxHeight(const HeightGridCache& cache, float x, float z) {
    const int ix = static_cast<int>(std::floor((x - cache.originX) / cache.cellSize));
    const int iz = static_cast<int>(std::floor((z - cache.originZ) / cache.cellSize));
    if (ix < 0 || iz < 0 || ix >= cache.cellsX || iz >= cache.cellsZ) {
        return kFarTerrainMaxHeight;
    }
    return cache.maxHeight[static_cast<size_t>(iz * cache.cellsX + ix)];
}

bool ProjectToScreen(
    Vec3 point,
    Vec3 camera,
    Vec3 forward,
    Vec3 right,
    Vec3 up,
    const Options& opt,
    float& screenX,
    float& screenY)
{
    const Vec3 rel{point.x - camera.x, point.y - camera.y, point.z - camera.z};
    const float viewZ = Dot(rel, forward);
    if (viewZ <= 1.0f) {
        return false;
    }
    const float tanHalfFov = std::tan(opt.fovY * 0.5f);
    const float viewX = Dot(rel, right);
    const float viewY = Dot(rel, up);
    const float ndcX = viewX / std::max(viewZ * tanHalfFov * opt.aspect, 1.0e-6f);
    const float ndcY = viewY / std::max(viewZ * tanHalfFov, 1.0e-6f);
    if (ndcX < -1.4f || ndcX > 1.4f || ndcY < -1.4f || ndcY > 1.4f) {
        return false;
    }
    screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(opt.width);
    screenY = (0.5f - ndcY * 0.5f) * static_cast<float>(opt.height);
    return true;
}

ScreenMaskCache BuildScreenMaskCache(
    const HeightGridCache& heightGrid,
    const Options& opt,
    Vec3 camera,
    Vec3 forward,
    Vec3 right,
    Vec3 up)
{
    ScreenMaskCache mask;
    mask.tileWidth = std::max(1, opt.screenMaskTileWidth);
    mask.tilesX = std::max(1, (opt.width + mask.tileWidth - 1) / mask.tileWidth);
    mask.horizonY.assign(static_cast<size_t>(mask.tilesX), std::numeric_limits<float>::infinity());
    if (heightGrid.cellsX <= 0 || heightGrid.cellsZ <= 0) {
        return mask;
    }

    for (int z = 0; z < heightGrid.cellsZ; ++z) {
        for (int x = 0; x < heightGrid.cellsX; ++x) {
            const float maxHeight = heightGrid.maxHeight[static_cast<size_t>(z * heightGrid.cellsX + x)];
            const float x0 = heightGrid.originX + static_cast<float>(x) * heightGrid.cellSize;
            const float z0 = heightGrid.originZ + static_cast<float>(z) * heightGrid.cellSize;
            const float x1 = x0 + heightGrid.cellSize;
            const float z1 = z0 + heightGrid.cellSize;

            std::array<Vec3, 4> corners{{
                {x0, maxHeight, z0},
                {x1, maxHeight, z0},
                {x0, maxHeight, z1},
                {x1, maxHeight, z1},
            }};
            float minScreenX = std::numeric_limits<float>::infinity();
            float maxScreenX = -std::numeric_limits<float>::infinity();
            float minScreenY = std::numeric_limits<float>::infinity();
            bool anyProjected = false;
            for (const Vec3& corner : corners) {
                float sx = 0.0f;
                float sy = 0.0f;
                if (!ProjectToScreen(corner, camera, forward, right, up, opt, sx, sy)) {
                    continue;
                }
                anyProjected = true;
                minScreenX = std::min(minScreenX, sx);
                maxScreenX = std::max(maxScreenX, sx);
                minScreenY = std::min(minScreenY, sy);
            }
            if (!anyProjected) {
                continue;
            }

            minScreenX -= opt.screenMaskDilationPixels;
            maxScreenX += opt.screenMaskDilationPixels;
            minScreenY -= opt.screenMaskDilationPixels;
            const int firstTile = std::max(
                0,
                static_cast<int>(std::floor(minScreenX / static_cast<float>(mask.tileWidth))));
            const int lastTile = std::min(
                mask.tilesX - 1,
                static_cast<int>(std::floor(maxScreenX / static_cast<float>(mask.tileWidth))));
            if (lastTile < firstTile) {
                continue;
            }
            for (int tile = firstTile; tile <= lastTile; ++tile) {
                mask.horizonY[static_cast<size_t>(tile)] =
                    std::min(mask.horizonY[static_cast<size_t>(tile)], minScreenY);
            }
        }
    }
    return mask;
}

bool ScreenMaskClassify(
    const ScreenMaskCache& mask,
    const Options& opt,
    int pixelX,
    int pixelY,
    float& hitT,
    QueryStats* stats = nullptr)
{
    hitT = 1.0e20f;
    if (stats) {
        ++stats->ddaQueries;
        ++stats->ddaVisitedCells;
    }
    if (mask.tilesX <= 0 || mask.horizonY.empty()) {
        hitT = std::max(opt.startDistance, 160.0f);
        return true;
    }
    const int tile = std::clamp(pixelX / std::max(1, mask.tileWidth), 0, mask.tilesX - 1);
    const float horizonY = mask.horizonY[static_cast<size_t>(tile)];
    if (!std::isfinite(horizonY)) {
        hitT = std::max(opt.startDistance, 160.0f);
        return true;
    }
    if (static_cast<float>(pixelY) < horizonY) {
        return false;
    }
    hitT = std::max(opt.startDistance, 160.0f);
    return true;
}

bool ExactRefineTerrainHit(
    Vec3 camera,
    Vec3 rayDir,
    uint32_t seed,
    float startT,
    float endT,
    float step,
    float& hitT,
    int64_t* heightEvalCount = nullptr)
{
    startT = std::max(startT, 160.0f);
    endT = std::max(endT, startT);

    Vec3 previousPos = Add(camera, Mul(rayDir, startT));
    if (heightEvalCount) {
        ++(*heightEvalCount);
    }
    float previousHeight = FarTerrainHeightVoxelized(previousPos.x, previousPos.z, startT, seed).height;
    float previousSigned = previousPos.y - previousHeight;
    if (previousSigned <= 0.0f) {
        hitT = startT;
        return true;
    }

    for (float t = startT + step; t <= endT + 1.0e-3f; t += step) {
        const Vec3 pos = Add(camera, Mul(rayDir, t));
        if (heightEvalCount) {
            ++(*heightEvalCount);
        }
        const float height = FarTerrainHeightVoxelized(pos.x, pos.z, t, seed).height;
        const float signedDistance = pos.y - height;
        if (signedDistance <= 0.0f && previousSigned > 0.0f) {
            hitT = t;
            return true;
        }
        previousSigned = signedDistance;
    }
    hitT = 1.0e20f;
    return false;
}

bool HeightGridClassify(const HeightGridCache& cache, const Options& opt, Vec3 camera, Vec3 rayDir, float& hitT) {
    hitT = 1.0e20f;
    if (rayDir.y > 0.42f || rayDir.y < -0.92f) {
        return false;
    }
    if (opt.hierarchicalDda && rayDir.y < opt.hierarchicalDdaMinRayY) {
        hitT = std::max(opt.startDistance, 160.0f);
        return true;
    }

    float t = std::max(opt.startDistance, 160.0f);
    for (int i = 0; i < 2048 && t <= opt.maxDistance; ++i) {
        const Vec3 pos = Add(camera, Mul(rayDir, t));
        const float maxHeight = HeightGridMaxHeight(cache, pos.x, pos.z);
        if (pos.y <= maxHeight) {
            if (opt.gridExactRefine) {
                const float refineStart = std::max(std::max(opt.startDistance, 160.0f), t - opt.gridQueryStep);
                const float refineEnd = std::min(opt.maxDistance, t + opt.gridRefineWindow);
                if (ExactRefineTerrainHit(
                        camera,
                        rayDir,
                        opt.seed,
                        refineStart,
                        refineEnd,
                        opt.gridRefineStep,
                        hitT)) {
                    return true;
                }
                if (!opt.gridRefineReject) {
                    hitT = t;
                    return true;
                }
                t += opt.gridQueryStep;
                continue;
            }
            hitT = t;
            return true;
        }
        t += opt.gridQueryStep;
    }
    return false;
}

float HeightGridCellMaxHeight(const HeightGridCache& cache, int ix, int iz) {
    if (ix < 0 || iz < 0 || ix >= cache.cellsX || iz >= cache.cellsZ) {
        return kFarTerrainMaxHeight;
    }
    return cache.maxHeight[static_cast<size_t>(iz * cache.cellsX + ix)];
}

float HeightGridMipMaxHeight(const HeightGridCache& cache, int level, int ix, int iz) {
    if (level < 0 || level >= static_cast<int>(cache.levelOffsets.size())) {
        return kFarTerrainMaxHeight;
    }
    const int cellsX = cache.levelCellsX[static_cast<size_t>(level)];
    const int cellsZ = cache.levelCellsZ[static_cast<size_t>(level)];
    if (ix < 0 || iz < 0 || ix >= cellsX || iz >= cellsZ) {
        return kFarTerrainMaxHeight;
    }
    return cache.mipMaxHeight[
        cache.levelOffsets[static_cast<size_t>(level)] + static_cast<size_t>(iz * cellsX + ix)];
}

float BlockExitT(
    const HeightGridCache& cache,
    Vec3 camera,
    Vec3 rayDir,
    float t,
    float tEnd,
    int leafMinX,
    int leafMaxX,
    int leafMinZ,
    int leafMaxZ)
{
    constexpr float eps = 1.0e-2f;
    float exitT = tEnd;
    if (rayDir.x > eps) {
        const float boundary = cache.originX + static_cast<float>(leafMaxX) * cache.cellSize;
        const float planeT = (boundary - camera.x) / rayDir.x;
        if (planeT > t - eps) {
            exitT = std::min(exitT, std::max(planeT, t));
        }
    } else if (rayDir.x < -eps) {
        const float boundary = cache.originX + static_cast<float>(leafMinX) * cache.cellSize;
        const float planeT = (boundary - camera.x) / rayDir.x;
        if (planeT > t - eps) {
            exitT = std::min(exitT, std::max(planeT, t));
        }
    }
    if (rayDir.z > eps) {
        const float boundary = cache.originZ + static_cast<float>(leafMaxZ) * cache.cellSize;
        const float planeT = (boundary - camera.z) / rayDir.z;
        if (planeT > t - eps) {
            exitT = std::min(exitT, std::max(planeT, t));
        }
    } else if (rayDir.z < -eps) {
        const float boundary = cache.originZ + static_cast<float>(leafMinZ) * cache.cellSize;
        const float planeT = (boundary - camera.z) / rayDir.z;
        if (planeT > t - eps) {
            exitT = std::min(exitT, std::max(planeT, t));
        }
    }
    if (!std::isfinite(exitT)) {
        return tEnd;
    }
    return std::clamp(exitT, t, tEnd);
}

bool CellIntervalPossibleHit(
    Vec3 camera,
    Vec3 rayDir,
    float maxHeight,
    float intervalStart,
    float intervalEnd,
    float& possibleT)
{
    possibleT = 1.0e20f;
    if (intervalEnd < intervalStart) {
        return false;
    }

    const float yStart = camera.y + rayDir.y * intervalStart;
    const float yEnd = camera.y + rayDir.y * intervalEnd;
    const float minY = std::min(yStart, yEnd);
    if (minY > maxHeight) {
        return false;
    }
    if (yStart <= maxHeight || std::abs(rayDir.y) <= 1.0e-8f) {
        possibleT = intervalStart;
        return true;
    }
    if (rayDir.y < 0.0f) {
        const float crossingT = (maxHeight - camera.y) / rayDir.y;
        possibleT = std::clamp(crossingT, intervalStart, intervalEnd);
        return true;
    }
    possibleT = intervalStart;
    return true;
}

bool HeightGridDdaClassify(
    const HeightGridCache& cache,
    const Options& opt,
    Vec3 camera,
    Vec3 rayDir,
    float& hitT,
    QueryStats* stats = nullptr)
{
    if (stats) {
        ++stats->ddaQueries;
    }
    hitT = 1.0e20f;
    if (rayDir.y > 0.42f || rayDir.y < -0.92f) {
        return false;
    }
    if (opt.hierarchicalDda && rayDir.y < opt.hierarchicalDdaMinRayY) {
        hitT = std::max(opt.startDistance, 160.0f);
        return true;
    }

    const float tStart = std::max(opt.startDistance, 160.0f);
    const float tEnd = opt.maxDistance;
    if (tEnd <= tStart) {
        return false;
    }

    if (std::abs(rayDir.x) <= 1.0e-8f && std::abs(rayDir.z) <= 1.0e-8f) {
        const Vec3 pos = Add(camera, Mul(rayDir, tStart));
        const float maxHeight = HeightGridMaxHeight(cache, pos.x, pos.z);
        float possibleT = 1.0e20f;
        if (CellIntervalPossibleHit(camera, rayDir, maxHeight, tStart, tEnd, possibleT)) {
            hitT = possibleT;
            return true;
        }
        return false;
    }

    if (opt.hierarchicalDda) {
        const auto cellFromWorld = [](float coord, float origin, float cellSize, float dir) {
            float u = (coord - origin) / cellSize;
            if (dir < 0.0f) {
                u -= 1.0e-5f;
            }
            return static_cast<int>(std::floor(u));
        };

        float t = tStart;
        const int maxIterations = (cache.cellsX + cache.cellsZ) * 16 + 64;
        int iterations = 0;
        while (t <= tEnd && iterations < maxIterations) {
            ++iterations;
            const Vec3 pos = Add(camera, Mul(rayDir, t));
            const int ix = cellFromWorld(pos.x, cache.originX, cache.cellSize, rayDir.x);
            const int iz = cellFromWorld(pos.z, cache.originZ, cache.cellSize, rayDir.z);
            if (ix < 0 || iz < 0 || ix >= cache.cellsX || iz >= cache.cellsZ) {
                hitT = t;
                if (stats) {
                    ++stats->ddaOutOfGridHits;
                }
                return true;
            }

            bool skipped = false;
            const int maxLevel = opt.hierarchicalDdaMaxLevel >= 0
                ? std::min(opt.hierarchicalDdaMaxLevel, static_cast<int>(cache.levelOffsets.size()) - 1)
                : static_cast<int>(cache.levelOffsets.size()) - 1;
            for (int level = maxLevel; level >= 0; --level) {
                const int scale = 1 << level;
                const int blockX = ix / scale;
                const int blockZ = iz / scale;
                const int leafMinX = blockX * scale;
                const int leafMinZ = blockZ * scale;
                const int leafMaxX = std::min(cache.cellsX, leafMinX + scale);
                const int leafMaxZ = std::min(cache.cellsZ, leafMinZ + scale);
                const float nextT = BlockExitT(
                    cache,
                    camera,
                    rayDir,
                    t,
                    tEnd,
                    leafMinX,
                    leafMaxX,
                    leafMinZ,
                    leafMaxZ);
                const float maxHeight = HeightGridMipMaxHeight(cache, level, blockX, blockZ);
                float possibleT = 1.0e20f;
                if (stats) {
                    ++stats->ddaVisitedCells;
                    stats->ddaMaxMipLevelUsed = std::max(stats->ddaMaxMipLevelUsed, level);
                }
                if (!CellIntervalPossibleHit(camera, rayDir, maxHeight, t, nextT, possibleT)) {
                    if (gTraceDda && gTraceTruthT >= t && gTraceTruthT <= nextT) {
                        const float y0 = camera.y + rayDir.y * t;
                        const float y1 = camera.y + rayDir.y * nextT;
                        const float x0 = camera.x + rayDir.x * t;
                        const float x1 = camera.x + rayDir.x * nextT;
                        const float z0 = camera.z + rayDir.z * t;
                        const float z1 = camera.z + rayDir.z * nextT;
                        const float xBoundary = cache.originX +
                            static_cast<float>(rayDir.x >= 0.0f ? leafMaxX : leafMinX) * cache.cellSize;
                        const float zBoundary = cache.originZ +
                            static_cast<float>(rayDir.z >= 0.0f ? leafMaxZ : leafMinZ) * cache.cellSize;
                        const float xPlaneT = std::abs(rayDir.x) > 1.0e-8f
                            ? (xBoundary - camera.x) / rayDir.x
                            : 1.0e20f;
                        const float zPlaneT = std::abs(rayDir.z) > 1.0e-8f
                            ? (zBoundary - camera.z) / rayDir.z
                            : 1.0e20f;
                        const Vec3 truthPos = Add(camera, Mul(rayDir, gTraceTruthT));
                        const int truthIx = static_cast<int>(std::floor((truthPos.x - cache.originX) / cache.cellSize));
                        const int truthIz = static_cast<int>(std::floor((truthPos.z - cache.originZ) / cache.cellSize));
                        std::cerr << "TRACE_SKIP_COVERS_TRUTH"
                            << " level=" << level
                            << " block=" << blockX << "," << blockZ
                            << " leafX=" << leafMinX << "-" << leafMaxX
                            << " leafZ=" << leafMinZ << "-" << leafMaxZ
                            << " t=" << t
                            << " nextT=" << nextT
                            << " truthT=" << gTraceTruthT
                            << " pos0=" << x0 << "," << y0 << "," << z0
                            << " pos1=" << x1 << "," << y1 << "," << z1
                            << " currentCell=" << ix << "," << iz
                            << " xBoundary=" << xBoundary
                            << " xPlaneT=" << xPlaneT
                            << " zBoundary=" << zBoundary
                            << " zPlaneT=" << zPlaneT
                            << " y0=" << y0
                            << " y1=" << y1
                            << " maxHeight=" << maxHeight
                            << " truthPos=" << truthPos.x << "," << truthPos.y << "," << truthPos.z
                            << " truthCell=" << truthIx << "," << truthIz
                            << "\n";
                    }
                    if (stats) {
                        ++stats->ddaSkippedBlocks;
                        stats->ddaSkippedLeafCells +=
                            static_cast<int64_t>(leafMaxX - leafMinX) *
                            static_cast<int64_t>(leafMaxZ - leafMinZ);
                    }
                    if (nextT >= tEnd) {
                        return false;
                    }
                    t = std::min(tEnd, nextT + 1.0e-3f);
                    skipped = true;
                    break;
                }

                if (level == 0) {
                    if (stats) {
                        ++stats->ddaPossibleCells;
                    }
                    if (opt.gridExactRefine) {
                        if (stats) {
                            ++stats->ddaRefineCalls;
                        }
                        const float refineStart = std::max(tStart, t);
                        const float refineEnd = std::min(tEnd, std::max(nextT, possibleT + opt.gridRefineWindow));
                        if (ExactRefineTerrainHit(
                                camera,
                                rayDir,
                                opt.seed,
                                refineStart,
                                refineEnd,
                                opt.gridRefineStep,
                                hitT,
                                stats ? &stats->ddaRefineHeightEvals : nullptr)) {
                            if (stats) {
                                ++stats->ddaRefineHits;
                            }
                            return true;
                        }
                        if (gTraceDda && gTraceTruthT >= refineStart && gTraceTruthT <= refineEnd) {
                            const Vec3 tracePos = Add(camera, Mul(rayDir, gTraceTruthT));
                            const float traceHeight = FarTerrainHeightVoxelized(
                                tracePos.x,
                                tracePos.z,
                                gTraceTruthT,
                                opt.seed).height;
                            std::cerr << "TRACE_REFINE_MISSED_TRUTH"
                                << " t=" << t
                                << " nextT=" << nextT
                                << " possibleT=" << possibleT
                                << " refineStart=" << refineStart
                                << " refineEnd=" << refineEnd
                                << " step=" << opt.gridRefineStep
                                << " truthT=" << gTraceTruthT
                                << " truthPos=" << tracePos.x << "," << tracePos.y << "," << tracePos.z
                                << " truthHeight=" << traceHeight
                                << " signed=" << (tracePos.y - traceHeight)
                                << "\n";
                        }
                        if (!opt.gridRefineReject) {
                            hitT = possibleT;
                            if (stats) {
                                ++stats->ddaEnvelopeFallbacks;
                            }
                            return true;
                        }
                        if (nextT >= tEnd) {
                            return false;
                        }
                        t = std::min(tEnd, nextT + 1.0e-3f);
                        skipped = true;
                        break;
                    } else {
                        hitT = possibleT;
                        return true;
                    }
                }
            }

            if (!skipped) {
                hitT = t;
                if (stats) {
                    ++stats->ddaIterationBailouts;
                }
                return true;
            }
        }
        if (iterations >= maxIterations) {
            hitT = t;
            if (stats) {
                ++stats->ddaIterationBailouts;
            }
            return true;
        }
        return false;
    }

    const auto cellXFromWorld = [&](float x) {
        return static_cast<int>(std::floor((x - cache.originX) / cache.cellSize));
    };
    const auto cellZFromWorld = [&](float z) {
        return static_cast<int>(std::floor((z - cache.originZ) / cache.cellSize));
    };

    float t = tStart;
    Vec3 pos = Add(camera, Mul(rayDir, t));
    int ix = cellXFromWorld(pos.x);
    int iz = cellZFromWorld(pos.z);
    if (ix < 0 || iz < 0 || ix >= cache.cellsX || iz >= cache.cellsZ) {
        hitT = tStart;
        if (stats) {
            ++stats->ddaOutOfGridHits;
        }
        return true;
    }

    const int stepX = rayDir.x > 0.0f ? 1 : (rayDir.x < 0.0f ? -1 : 0);
    const int stepZ = rayDir.z > 0.0f ? 1 : (rayDir.z < 0.0f ? -1 : 0);
    const float inf = std::numeric_limits<float>::infinity();
    float tMaxX = inf;
    float tMaxZ = inf;
    const float tDeltaX = stepX != 0 ? cache.cellSize / std::abs(rayDir.x) : inf;
    const float tDeltaZ = stepZ != 0 ? cache.cellSize / std::abs(rayDir.z) : inf;
    if (stepX != 0) {
        const float boundaryX = cache.originX + static_cast<float>(ix + (stepX > 0 ? 1 : 0)) * cache.cellSize;
        tMaxX = (boundaryX - camera.x) / rayDir.x;
        if (tMaxX < t) {
            tMaxX = t;
        }
    }
    if (stepZ != 0) {
        const float boundaryZ = cache.originZ + static_cast<float>(iz + (stepZ > 0 ? 1 : 0)) * cache.cellSize;
        tMaxZ = (boundaryZ - camera.z) / rayDir.z;
        if (tMaxZ < t) {
            tMaxZ = t;
        }
    }

    const int maxVisitedCells = (cache.cellsX + cache.cellsZ) * 4 + 16;
    int visitedCells = 0;
    while (t <= tEnd && visitedCells < maxVisitedCells) {
        ++visitedCells;
        if (stats) {
            ++stats->ddaVisitedCells;
        }
        if (ix < 0 || iz < 0 || ix >= cache.cellsX || iz >= cache.cellsZ) {
            hitT = t;
            if (stats) {
                ++stats->ddaOutOfGridHits;
            }
            return true;
        }
        const float nextT = std::min(tEnd, std::min(tMaxX, tMaxZ));
        const float maxHeight = HeightGridCellMaxHeight(cache, ix, iz);
        float possibleT = 1.0e20f;
        if (CellIntervalPossibleHit(camera, rayDir, maxHeight, t, nextT, possibleT)) {
            if (stats) {
                ++stats->ddaPossibleCells;
            }
            if (opt.gridExactRefine) {
                if (stats) {
                    ++stats->ddaRefineCalls;
                }
                const float refineStart = std::max(tStart, t);
                const float refineEnd = std::min(tEnd, std::max(nextT, possibleT + opt.gridRefineWindow));
                if (ExactRefineTerrainHit(
                        camera,
                        rayDir,
                        opt.seed,
                        refineStart,
                        refineEnd,
                        opt.gridRefineStep,
                        hitT,
                        stats ? &stats->ddaRefineHeightEvals : nullptr)) {
                    if (stats) {
                        ++stats->ddaRefineHits;
                    }
                    return true;
                }
                if (!opt.gridRefineReject) {
                    hitT = possibleT;
                    if (stats) {
                        ++stats->ddaEnvelopeFallbacks;
                    }
                    return true;
                }
            } else {
                hitT = possibleT;
                return true;
            }
        }

        if (nextT >= tEnd) {
            break;
        }
        const bool stepAlongX = tMaxX <= tMaxZ;
        const bool stepAlongZ = tMaxZ <= tMaxX;
        t = nextT;
        if (stepAlongX) {
            ix += stepX;
            tMaxX += tDeltaX;
        }
        if (stepAlongZ) {
            iz += stepZ;
            tMaxZ += tDeltaZ;
        }
    }
    if (visitedCells >= maxVisitedCells) {
        hitT = t;
        if (stats) {
            ++stats->ddaIterationBailouts;
        }
        return true;
    }
    return false;
}

StructuralCheckResult CheckHeightGridStructure(const HeightGridCache& cache, const Options& opt) {
    StructuralCheckResult result;
    const int samples = std::max(1, opt.structuralSamples);
    for (int z = 0; z < cache.cellsZ; ++z) {
        for (int x = 0; x < cache.cellsX; ++x) {
            const float storedMax = cache.maxHeight[static_cast<size_t>(z * cache.cellsX + x)];
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const float ux = (static_cast<float>(sx) + 0.5f) / static_cast<float>(samples);
                    const float uz = (static_cast<float>(sy) + 0.5f) / static_cast<float>(samples);
                    const float sampleX = cache.originX + (static_cast<float>(x) + ux) * cache.cellSize;
                    const float sampleZ = cache.originZ + (static_cast<float>(z) + uz) * cache.cellSize;
                    const float sampleDistance = std::sqrt(
                        (sampleX - opt.cameraX) * (sampleX - opt.cameraX) +
                        (sampleZ - opt.cameraZ) * (sampleZ - opt.cameraZ));
                    const float height =
                        FarTerrainHeightVoxelized(sampleX, sampleZ, sampleDistance, opt.seed).height;
                    ++result.leafSamples;
                    if (height > storedMax + 1.0e-4f) {
                        ++result.leafViolations;
                        result.maxLeafViolation = std::max(result.maxLeafViolation, height - storedMax);
                    }
                }
            }
        }
    }

    for (size_t level = 1; level < cache.levelOffsets.size(); ++level) {
        const size_t parentOffset = cache.levelOffsets[level];
        const size_t childOffset = cache.levelOffsets[level - 1u];
        const int parentXCount = cache.levelCellsX[level];
        const int parentZCount = cache.levelCellsZ[level];
        const int childXCount = cache.levelCellsX[level - 1u];
        const int childZCount = cache.levelCellsZ[level - 1u];
        for (int z = 0; z < parentZCount; ++z) {
            for (int x = 0; x < parentXCount; ++x) {
                float childMax = kFarTerrainMinHeight;
                for (int dz = 0; dz < 2; ++dz) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int childX = x * 2 + dx;
                        const int childZ = z * 2 + dz;
                        if (childX < childXCount && childZ < childZCount) {
                            childMax = std::max(
                                childMax,
                                cache.mipMaxHeight[childOffset + static_cast<size_t>(childZ * childXCount + childX)]);
                        }
                    }
                }
                const float parent =
                    cache.mipMaxHeight[parentOffset + static_cast<size_t>(z * parentXCount + x)];
                ++result.parentChecks;
                const float violation = childMax - parent;
                if (violation > 1.0e-4f) {
                    ++result.parentViolations;
                    result.maxParentViolation = std::max(result.maxParentViolation, violation);
                }
            }
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseOptions(argc, argv, opt)) {
        std::cerr << "usage: far_horizon_parity [--bins N] [--pixel-step N] [--y-min N] [--y-max N]\n";
        return 2;
    }

    const Vec3 camera{opt.cameraX, opt.cameraY, opt.cameraZ};
    const Vec3 forward = Normalize({opt.forwardX, opt.forwardY, opt.forwardZ});
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    const Vec3 right = Normalize(Cross(forward, worldUp));
    const Vec3 up = Normalize(Cross(right, forward));
    const float tanHalfFov = std::tan(opt.fovY * 0.5f);
    const auto slopeTable = opt.mode == "slope" ? BuildSlopeTable(opt, camera) : SlopeTable{};
    const bool gridMode = opt.mode == "grid" || opt.mode == "mipdda" || opt.mode == "screenmask";
    const auto heightGrid = gridMode ? BuildHeightGridCache(opt, camera) : HeightGridCache{};
    const auto screenMask = opt.mode == "screenmask"
        ? BuildScreenMaskCache(heightGrid, opt, camera, forward, right, up)
        : ScreenMaskCache{};
    const StructuralCheckResult structural =
        (gridMode && opt.structuralCheck) ? CheckHeightGridStructure(heightGrid, opt) : StructuralCheckResult{};
    QueryStats queryStats;

    int samples = 0;
    int truthHits = 0;
    int predictedHits = 0;
    int hitMissMismatches = 0;
    int falsePositives = 0;
    int falseNegatives = 0;
    int tMismatches = 0;
    int cacheHits = 0;
    int cacheMisses = 0;
    int fallbackDiagnosticCalls = 0;
    int fallbackDiagnosticSkips = 0;
    int truthEarlyRejects = 0;
    int truthSkyBreakMisses = 0;
    int truthDeepMisses = 0;
    int fallbackSkippedEarlyRejects = 0;
    int fallbackSkippedSkyBreakMisses = 0;
    int fallbackSkippedDeepMisses = 0;
    int fallbackCalledEarlyRejects = 0;
    int fallbackCalledSkyBreakMisses = 0;
    int fallbackCalledDeepMisses = 0;
    int falsePositiveEarlyRejects = 0;
    int falsePositiveSkyBreakMisses = 0;
    int falsePositiveDeepMisses = 0;
    int64_t diagnosticHeightEvals = 0;
    int64_t diagnosticSteps = 0;
    int64_t fallbackCalledDiagnosticHeightEvals = 0;
    int64_t fallbackSkippedDiagnosticHeightEvals = 0;
    int64_t fallbackCalledDiagnosticSteps = 0;
    int64_t fallbackSkippedDiagnosticSteps = 0;
    float maxRelT = 0.0f;
    float sumRelT = 0.0f;
    int tSamples = 0;
    CohortStats calledDeepMissCohort;
    CohortStats skippedDeepMissCohort;
    CohortStats calledSkyBreakCohort;
    CohortStats skippedSkyBreakCohort;
    CohortStats calledHitCohort;
    CohortStats skippedHitCohort;

    struct Example {
        int x = 0;
        int y = 0;
        bool truth = false;
        bool predicted = false;
        float truthT = 0.0f;
        float predictedT = 0.0f;
        float relT = 0.0f;
    };
    std::vector<Example> examples;

    for (int y = opt.yMin; y <= opt.yMax; y += opt.pixelStep) {
        for (int x = 0; x < opt.width; x += opt.pixelStep) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(opt.width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(opt.height);
            const float ndcX = u * 2.0f - 1.0f;
            const float ndcY = -(v * 2.0f - 1.0f);
            const Vec3 rayDir = Normalize(Add(Add(forward, Mul(right, ndcX * tanHalfFov * opt.aspect)), Mul(up, ndcY * tanHalfFov)));

            float truthT = 1.0e20f;
            float predictedT = 1.0e20f;
            DiagnosticWorkStats truthWork;
            const bool truth = DiagnosticFarTerrainWouldHit(
                camera,
                rayDir,
                opt.startDistance,
                opt.seed,
                truthT,
                &truthWork);
            diagnosticHeightEvals += truthWork.heightEvals;
            diagnosticSteps += truthWork.steps;
            truthEarlyRejects += truthWork.earlyRejects;
            truthSkyBreakMisses += truthWork.skyBreakMisses;
            truthDeepMisses += truthWork.deepMisses;
            gTraceDda = opt.traceX == x && opt.traceY == y;
            gTraceTruthT = truthT;
            const bool cachePredicted = opt.mode == "mipdda"
                ? HeightGridDdaClassify(heightGrid, opt, camera, rayDir, predictedT, &queryStats)
                : (opt.mode == "screenmask"
                    ? ScreenMaskClassify(screenMask, opt, x, y, predictedT, &queryStats)
                    : (opt.mode == "grid"
                    ? HeightGridClassify(heightGrid, opt, camera, rayDir, predictedT)
                    : SlopeTableClassify(slopeTable, opt, camera, rayDir, predictedT)));
            cacheHits += cachePredicted ? 1 : 0;
            cacheMisses += cachePredicted ? 0 : 1;
            if (opt.cohortDiagnostics) {
                if (truth) {
                    RecordCohort(
                        cachePredicted ? calledHitCohort : skippedHitCohort,
                        opt,
                        camera,
                        rayDir,
                        y,
                        cachePredicted ? predictedT : 1.0e20f,
                        truthWork);
                } else if (truthWork.deepMisses != 0) {
                    RecordCohort(
                        cachePredicted ? calledDeepMissCohort : skippedDeepMissCohort,
                        opt,
                        camera,
                        rayDir,
                        y,
                        cachePredicted ? predictedT : 1.0e20f,
                        truthWork);
                } else if (truthWork.skyBreakMisses != 0) {
                    RecordCohort(
                        cachePredicted ? calledSkyBreakCohort : skippedSkyBreakCohort,
                        opt,
                        camera,
                        rayDir,
                        y,
                        cachePredicted ? predictedT : 1.0e20f,
                        truthWork);
                }
            }
            bool predicted = cachePredicted;
            if (opt.fallbackDiagnostic && gridMode) {
                if (cachePredicted) {
                    ++fallbackDiagnosticCalls;
                    fallbackCalledDiagnosticHeightEvals += truthWork.heightEvals;
                    fallbackCalledDiagnosticSteps += truthWork.steps;
                    fallbackCalledEarlyRejects += truthWork.earlyRejects;
                    fallbackCalledSkyBreakMisses += truthWork.skyBreakMisses;
                    fallbackCalledDeepMisses += truthWork.deepMisses;
                    predicted = truth;
                    predictedT = truthT;
                } else {
                    ++fallbackDiagnosticSkips;
                    fallbackSkippedDiagnosticHeightEvals += truthWork.heightEvals;
                    fallbackSkippedDiagnosticSteps += truthWork.steps;
                    fallbackSkippedEarlyRejects += truthWork.earlyRejects;
                    fallbackSkippedSkyBreakMisses += truthWork.skyBreakMisses;
                    fallbackSkippedDeepMisses += truthWork.deepMisses;
                    predicted = false;
                    predictedT = 1.0e20f;
                }
            }
            ++samples;
            truthHits += truth ? 1 : 0;
            predictedHits += predicted ? 1 : 0;
            if (truth != predicted) {
                ++hitMissMismatches;
                if (predicted) {
                    ++falsePositives;
                    falsePositiveEarlyRejects += truthWork.earlyRejects;
                    falsePositiveSkyBreakMisses += truthWork.skyBreakMisses;
                    falsePositiveDeepMisses += truthWork.deepMisses;
                } else {
                    ++falseNegatives;
                }
                if (examples.size() < 10u) {
                    examples.push_back({x, y, truth, predicted, truthT, predictedT, 0.0f});
                }
                continue;
            }
            if (truth) {
                const float relT = std::abs(predictedT - truthT) / std::max(truthT, 1.0f);
                maxRelT = std::max(maxRelT, relT);
                sumRelT += relT;
                ++tSamples;
                if (relT > opt.hitTRelTolerance) {
                    ++tMismatches;
                    if (examples.size() < 10u) {
                        examples.push_back({x, y, truth, predicted, truthT, predictedT, relT});
                    }
                }
            }
        }
    }

    const double netWorkSaved =
        static_cast<double>(fallbackSkippedDiagnosticHeightEvals) *
        static_cast<double>(opt.diagnosticHeightEvalWork);
    const double netWorkAdded =
        static_cast<double>(queryStats.ddaVisitedCells) * static_cast<double>(opt.cacheCellWork) +
        static_cast<double>(queryStats.ddaRefineHeightEvals) * static_cast<double>(opt.refineHeightEvalWork);
    const double netWorkGain = netWorkSaved - netWorkAdded;
    const double netWorkGainRatio = netWorkAdded > 0.0 ? netWorkSaved / netWorkAdded : 0.0;
    const double breakEvenCacheCellWork =
        queryStats.ddaVisitedCells > 0
            ? (netWorkSaved -
                  static_cast<double>(queryStats.ddaRefineHeightEvals) *
                  static_cast<double>(opt.refineHeightEvalWork)) /
                  static_cast<double>(queryStats.ddaVisitedCells)
            : 0.0;

    std::cout << std::fixed << std::setprecision(4)
        << "FAR_HORIZON_PARITY samples=" << samples
        << " mode=" << opt.mode
        << " truthHits=" << truthHits
        << " predictedHits=" << predictedHits
        << " predictedMisses=" << (samples - predictedHits)
        << " predictedMissRate=" << (samples > 0 ? static_cast<float>(samples - predictedHits) / static_cast<float>(samples) : 0.0f)
        << " trueMissCullRate=" << ((samples - truthHits) > 0 ? static_cast<float>((samples - truthHits) - falsePositives) / static_cast<float>(samples - truthHits) : 0.0f)
        << " cacheHits=" << cacheHits
        << " cacheMisses=" << cacheMisses
        << " cacheMissRate=" << (samples > 0 ? static_cast<float>(cacheMisses) / static_cast<float>(samples) : 0.0f)
        << " fallbackDiagnostic=" << (opt.fallbackDiagnostic ? 1 : 0)
        << " fallbackDiagnosticCalls=" << fallbackDiagnosticCalls
        << " fallbackDiagnosticSkips=" << fallbackDiagnosticSkips
        << " fallbackDiagnosticCallRate=" << (samples > 0 ? static_cast<float>(fallbackDiagnosticCalls) / static_cast<float>(samples) : 0.0f)
        << " truthEarlyRejects=" << truthEarlyRejects
        << " truthSkyBreakMisses=" << truthSkyBreakMisses
        << " truthDeepMisses=" << truthDeepMisses
        << " fallbackSkippedEarlyRejects=" << fallbackSkippedEarlyRejects
        << " fallbackSkippedSkyBreakMisses=" << fallbackSkippedSkyBreakMisses
        << " fallbackSkippedDeepMisses=" << fallbackSkippedDeepMisses
        << " fallbackCalledEarlyRejects=" << fallbackCalledEarlyRejects
        << " fallbackCalledSkyBreakMisses=" << fallbackCalledSkyBreakMisses
        << " fallbackCalledDeepMisses=" << fallbackCalledDeepMisses
        << " falsePositiveEarlyRejects=" << falsePositiveEarlyRejects
        << " falsePositiveSkyBreakMisses=" << falsePositiveSkyBreakMisses
        << " falsePositiveDeepMisses=" << falsePositiveDeepMisses
        << " diagnosticHeightEvals=" << diagnosticHeightEvals
        << " diagnosticAvgHeightEvals=" << (samples > 0 ? static_cast<double>(diagnosticHeightEvals) / static_cast<double>(samples) : 0.0)
        << " diagnosticSteps=" << diagnosticSteps
        << " fallbackCalledDiagnosticHeightEvals=" << fallbackCalledDiagnosticHeightEvals
        << " fallbackSkippedDiagnosticHeightEvals=" << fallbackSkippedDiagnosticHeightEvals
        << " fallbackSkippedAvgHeightEvals=" << (fallbackDiagnosticSkips > 0 ? static_cast<double>(fallbackSkippedDiagnosticHeightEvals) / static_cast<double>(fallbackDiagnosticSkips) : 0.0)
        << " fallbackCalledDiagnosticSteps=" << fallbackCalledDiagnosticSteps
        << " fallbackSkippedDiagnosticSteps=" << fallbackSkippedDiagnosticSteps
        << " ddaQueries=" << queryStats.ddaQueries
        << " ddaVisitedCells=" << queryStats.ddaVisitedCells
        << " ddaAvgVisitedCells=" << (queryStats.ddaQueries > 0 ? static_cast<float>(queryStats.ddaVisitedCells) / static_cast<float>(queryStats.ddaQueries) : 0.0f)
        << " ddaSkippedBlocks=" << queryStats.ddaSkippedBlocks
        << " ddaSkippedLeafCells=" << queryStats.ddaSkippedLeafCells
        << " ddaMaxMipLevelUsed=" << queryStats.ddaMaxMipLevelUsed
        << " ddaPossibleCells=" << queryStats.ddaPossibleCells
        << " ddaPossibleCellRate=" << (queryStats.ddaQueries > 0 ? static_cast<float>(queryStats.ddaPossibleCells) / static_cast<float>(queryStats.ddaQueries) : 0.0f)
        << " ddaRefineCalls=" << queryStats.ddaRefineCalls
        << " ddaRefineHits=" << queryStats.ddaRefineHits
        << " ddaRefineHeightEvals=" << queryStats.ddaRefineHeightEvals
        << " ddaEnvelopeFallbacks=" << queryStats.ddaEnvelopeFallbacks
        << " ddaOutOfGridHits=" << queryStats.ddaOutOfGridHits
        << " ddaIterationBailouts=" << queryStats.ddaIterationBailouts
        << " diagnosticHeightEvalWork=" << opt.diagnosticHeightEvalWork
        << " cacheCellWork=" << opt.cacheCellWork
        << " refineHeightEvalWork=" << opt.refineHeightEvalWork
        << " netWorkSaved=" << netWorkSaved
        << " netWorkAdded=" << netWorkAdded
        << " netWorkGain=" << netWorkGain
        << " netWorkGainRatio=" << netWorkGainRatio
        << " breakEvenCacheCellWork=" << breakEvenCacheCellWork
        << " requireNetWorkGain=" << (opt.requireNetWorkGain ? 1 : 0)
        << " hitMissMismatches=" << hitMissMismatches
        << " falsePositives=" << falsePositives
        << " falseNegatives=" << falseNegatives
        << " tMismatches=" << tMismatches
        << " tSamples=" << tSamples
        << " maxRelT=" << maxRelT
        << " meanRelT=" << (tSamples > 0 ? sumRelT / static_cast<float>(tSamples) : 0.0f)
        << " bins=" << opt.bins
        << " slopeBins=" << opt.slopeBins
        << " gridCellSize=" << opt.gridCellSize
        << " gridQueryStep=" << opt.gridQueryStep
        << " gridHeightPad=" << opt.gridHeightPad
        << " gridSubsamples=" << opt.gridSubsamples
        << " hierarchicalDda=" << (opt.hierarchicalDda ? 1 : 0)
        << " hierarchicalDdaMaxLevel=" << opt.hierarchicalDdaMaxLevel
        << " hierarchicalDdaMinRayY=" << opt.hierarchicalDdaMinRayY
        << " gridExactRefine=" << (opt.gridExactRefine ? 1 : 0)
        << " gridRefineReject=" << (opt.gridRefineReject ? 1 : 0)
        << " gridRefineWindow=" << opt.gridRefineWindow
        << " gridRefineStep=" << opt.gridRefineStep
        << " screenMaskTileWidth=" << opt.screenMaskTileWidth
        << " screenMaskDilationPixels=" << opt.screenMaskDilationPixels
        << " screenMaskTilesX=" << screenMask.tilesX
        << " gridCells=" << heightGrid.cellsX << "x" << heightGrid.cellsZ
        << " mipLevels=" << heightGrid.levelOffsets.size()
        << " structuralLeafSamples=" << structural.leafSamples
        << " structuralLeafViolations=" << structural.leafViolations
        << " structuralMaxLeafViolation=" << structural.maxLeafViolation
        << " structuralParentChecks=" << structural.parentChecks
        << " structuralParentViolations=" << structural.parentViolations
        << " structuralMaxParentViolation=" << structural.maxParentViolation
        << " pixelStep=" << opt.pixelStep
        << " yRange=" << opt.yMin << "-" << opt.yMax
        << " startDistance=" << opt.startDistance
        ;
    if (opt.cohortDiagnostics) {
        PrintCohort("calledDeepMiss", calledDeepMissCohort);
        PrintCohort("skippedDeepMiss", skippedDeepMissCohort);
        PrintCohort("calledSkyBreak", calledSkyBreakCohort);
        PrintCohort("skippedSkyBreak", skippedSkyBreakCohort);
        PrintCohort("calledHit", calledHitCohort);
        PrintCohort("skippedHit", skippedHitCohort);
    }
    std::cout << "\n";
    for (const Example& e : examples) {
        std::cout << "  example x=" << e.x << " y=" << e.y
            << " truth=" << (e.truth ? 1 : 0)
            << " predicted=" << (e.predicted ? 1 : 0)
            << " truthT=" << e.truthT
            << " predictedT=" << e.predictedT
            << " relT=" << e.relT
            << "\n";
    }

    if (opt.conservativeOnly && falseNegatives == 0) {
        if (opt.structuralCheck &&
            (structural.leafViolations != 0 || structural.parentViolations != 0)) {
            std::cerr << "FAIL: cache sampled ray gate passed but structural conservativeness failed.\n";
            return 1;
        }
        if (opt.requireNetWorkGain && !(netWorkGain > 0.0)) {
            std::cerr << "FAIL: cache is conservative but net work does not strictly decrease.\n";
            return 1;
        }
        std::cout << "PASS: cache is conservative for DiagnosticFarTerrainWouldHit on sampled rays.\n";
        return 0;
    }
    if (hitMissMismatches != 0 || tMismatches != 0) {
        std::cerr << "FAIL: horizon table does not match DiagnosticFarTerrainWouldHit within tolerance.\n";
        return 1;
    }
    std::cout << "PASS: horizon table matches DiagnosticFarTerrainWouldHit within tolerance.\n";
    return 0;
}
