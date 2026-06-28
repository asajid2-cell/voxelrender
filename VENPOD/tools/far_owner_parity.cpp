#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr float kFarTerrainMinHeight = -332.0f;
constexpr float kFarTerrainMaxHeight = 664.0f;
constexpr float kFarSeaLevel = -48.0f;
constexpr uint32_t kDefaultSeed = 12345u;
constexpr uint32_t kMatSand = 1u;
constexpr uint32_t kMatWater = 2u;
constexpr uint32_t kMatStone = 3u;
constexpr uint32_t kMatDirt = 4u;
constexpr float kDefaultOwnerMaxDistance = 11000.0f;

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

    const float a = Lerp(s00, s10, sx);
    const float b = Lerp(s01, s11, sx);
    return Lerp(a, b, sz) * 2.0f - 1.0f;
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

    float height = -64.0f;
    height += broad * 92.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 3.0f;

    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    const float originComfort = 1.0f - Smooth01((originDistance - 180.0f) / 520.0f);
    const float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - Smooth01(originDistance / 420.0f)) * 58.0f;
    height = Lerp(height, publicRegionHeight, originComfort * 0.94f);

    const float publicCapInfluence = 1.0f - Smooth01((originDistance - 220.0f) / 420.0f);
    const float publicCap = 58.0f + Smooth01(originDistance / 640.0f) * 114.0f;
    height = Lerp(height, std::min(height, publicCap), publicCapInfluence);

    const float submergedBlend = 1.0f - Smooth01((height - (kFarSeaLevel + 28.0f)) / 86.0f);
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            (kFarSeaLevel - 8.0f) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - Smooth01(originDistance / 520.0f)) * 18.0f;
        height = Lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
    }

    const float playableBankBand = 1.0f - Smooth01((originDistance - 260.0f) / 980.0f);
    const float lowlandUpper = 1.0f - Smooth01((height - (kFarSeaLevel + 96.0f)) / 120.0f);
    const float lowlandFloor = Smooth01((height - (kFarSeaLevel - 40.0f)) / 64.0f);
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    const float playableShelfHeight =
        (kFarSeaLevel + 18.0f) +
        broad * 28.0f +
        ridgeHeight * 10.0f +
        detail * 1.5f +
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
    const float backdropRidgeSource =
        ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, seed + 227u);
    const float backdropRidge = 1.0f - std::abs(backdropRidgeSource);
    const float backdropBreakup =
        ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, seed + 271u);
    const float backdropNotch = Smooth01((backdropBreakup - 0.08f) / 0.58f);
    const float silhouetteRidge =
        Saturate(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
    const float backdropBand =
        Smooth01((originDistance - 1360.0f) / 700.0f) *
        (1.0f - Smooth01((originDistance - 5200.0f) / 1200.0f));
    const float northBackdrop = Smooth01((z - 1180.0f) / 900.0f);
    const float sideBackdrop = Smooth01((std::abs(x - 192.0f) - 820.0f) / 980.0f);
    const float backdropFacing = Saturate(northBackdrop + sideBackdrop * 0.58f);
    const float silhouetteContinuity = Saturate(silhouetteRidge + backdropBand * backdropFacing * 0.32f);
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
    const float routeRidgeNoiseA =
        ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, seed + 251u);
    const float routeRidgeNoiseB =
        ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, seed + 263u);
    const float routeBreakup =
        ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, seed + 281u);
    const float routeNotch = Smooth01((routeBreakup - 0.02f) / 0.60f);
    const float routeRidge =
        Saturate(
            0.26f +
            (1.0f - std::abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f);
    const float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = Lerp(
        height,
        std::max(height, routeBackdropHeight),
        routeCorridor * routeRidge * routeNotch * 0.68f);

    TerrainSample sample;
    sample.height = std::clamp(height, kFarTerrainMinHeight, kFarTerrainMaxHeight);
    sample.mountainMask = Saturate((ridgeHeight * 150.0f + std::max(height - 160.0f, 0.0f)) / 300.0f);
    return sample;
}

float FarFallbackCellSize(float distanceFromCamera) {
    const float t = Saturate((distanceFromCamera - 900.0f) / 6500.0f);
    if (t < 0.18f) {
        return 8.0f;
    }
    if (t < 0.42f) {
        return 12.0f;
    }
    if (t < 0.68f) {
        return 18.0f;
    }
    return 28.0f;
}

float QuantizeTerrainTopHeight(float height, float verticalStep) {
    verticalStep = std::max(verticalStep, 1.0f);
    return std::ceil(height / verticalStep) * verticalStep;
}

float FarSpawnLandFloorBase(float x, float z, uint32_t seed) {
    const float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, seed + 11u);
    const float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, seed + 37u);
    return (kFarSeaLevel + 56.0f) + broad * 18.0f + detail * 3.0f;
}

float FarSpawnLandBand(float x, float z) {
    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    return 1.0f - Smooth01((originDistance - 200.0f) / 90000.0f);
}

float FarTerrainHeightVoxelized(float x, float z, float distanceFromCamera, uint32_t seed) {
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    const float sampleX = (std::floor(x / cellSize) + 0.5f) * cellSize;
    const float sampleZ = (std::floor(z / cellSize) + 0.5f) * cellSize;
    const TerrainSample raw = FarTerrainHeight(sampleX, sampleZ, seed);
    const float floorHeight =
        FarSpawnLandFloorBase(sampleX, sampleZ, seed) + raw.mountainMask * 80.0f;
    const float reshaped =
        Lerp(raw.height, std::max(raw.height, floorHeight), FarSpawnLandBand(sampleX, sampleZ));
    return QuantizeTerrainTopHeight(reshaped, std::max(4.0f, cellSize * 0.75f));
}

struct Options {
    float actualSurfaceY = std::numeric_limits<float>::quiet_NaN();
    std::string faceCsv;
    uint32_t seed = kDefaultSeed;
    float startDistance = 256.0f;
    float endDistance = 9000.0f;
    float tolerance = 1.0f;
    float quantumTolerance = 0.0f;
    float cameraX = 192.0f;
    float cameraY = 0.0f;
    float cameraZ = 224.0f;
    float footprintStep = 4.0f;
    int minFaces = 0;
    int gridCellCount = 384;
    float gridCellSize = 28.0f;
    float ownerMaxDistance = kDefaultOwnerMaxDistance;
    bool forbidBelowSeaFaces = true;
    bool allBands = true;
    bool allowSideFaces = false;
    bool requireGridCoverage = false;
    bool hasGridOriginOverride = false;
    float gridOriginX = 0.0f;
    float gridOriginZ = 0.0f;
};

struct OwnerFace {
    int32_t worldX = 0;
    int32_t worldY = 0;
    int32_t worldZ = 0;
    uint32_t payload = 0;
};

struct FaceCsvMetadata {
    bool hasOrigin = false;
    float originX = 0.0f;
    float originZ = 0.0f;
    int faces = 0;
    float cellSize = 0.0f;
};

uint32_t PayloadDirection(uint32_t payload) {
    return (payload >> 29u) & 0x7u;
}

uint32_t PayloadWidth(uint32_t payload) {
    return ((payload >> 24u) & 0x1Fu) + 1u;
}

uint32_t PayloadHeight(uint32_t payload) {
    return ((payload >> 19u) & 0x1Fu) + 1u;
}

uint32_t PayloadMaterial(uint32_t payload) {
    return payload & 0x0007FFFFu;
}

uint32_t PackSparseSurfacePayload(uint32_t direction, uint32_t voxel, uint32_t width, uint32_t height) {
    return ((direction & 0x7u) << 29u) |
        (((width - 1u) & 0x1Fu) << 24u) |
        (((height - 1u) & 0x1Fu) << 19u) |
        (voxel & 0x0007FFFFu);
}

bool ExtractHeaderFloat(const std::string& line, const char* key, float& out) {
    const std::string needle = std::string(key) + "=";
    const size_t pos = line.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    const char* begin = line.c_str() + pos + needle.size();
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    if (end == begin || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool ExtractHeaderInt(const std::string& line, const char* key, int& out) {
    const std::string needle = std::string(key) + "=";
    const size_t pos = line.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    const char* begin = line.c_str() + pos + needle.size();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    if (end == begin ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

std::string FaceKey(int32_t x, int32_t y, int32_t z) {
    return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
}

bool ParseFaceLine(const std::string& line, OwnerFace& out) {
    std::string cleaned;
    cleaned.reserve(line.size());
    for (char ch : line) {
        cleaned.push_back(ch == ',' ? ' ' : ch);
    }

    std::istringstream stream(cleaned);
    uint64_t payload = 0;
    if (!(stream >> out.worldX >> out.worldY >> out.worldZ >> payload)) {
        return false;
    }
    if (payload > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out.payload = static_cast<uint32_t>(payload);
    return true;
}

bool LoadFaceCsv(
    const std::string& path,
    std::vector<OwnerFace>& faces,
    FaceCsvMetadata& metadata,
    std::string& error)
{
    std::ifstream file(path);
    if (!file) {
        error = "failed to open face CSV: " + path;
        return false;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            if (first != std::string::npos && line[first] == '#') {
                float originX = 0.0f;
                float originZ = 0.0f;
                if (ExtractHeaderFloat(line, "originX", originX) &&
                    ExtractHeaderFloat(line, "originZ", originZ)) {
                    metadata.hasOrigin = true;
                    metadata.originX = originX;
                    metadata.originZ = originZ;
                }
                (void)ExtractHeaderInt(line, "faces", metadata.faces);
                (void)ExtractHeaderFloat(line, "cellSize", metadata.cellSize);
            }
            continue;
        }
        if (line.compare(first, 6u, "worldX") == 0) {
            continue;
        }
        OwnerFace face;
        if (!ParseFaceLine(line, face)) {
            error = "invalid face CSV line " + std::to_string(lineNumber) + ": " + line;
            return false;
        }
        if (face.payload == 0u) {
            continue;
        }
        faces.push_back(face);
    }

    if (faces.empty()) {
        error = "face CSV contained no active faces: " + path;
        return false;
    }
    return true;
}

bool ReadFloatArg(int argc, char** argv, int& i, float& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = std::strtof(argv[++i], nullptr);
    return std::isfinite(out);
}

bool ReadUintArg(int argc, char** argv, int& i, uint32_t& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    return true;
}

bool ReadIntArg(int argc, char** argv, int& i, int& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--actual-surface-y") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.actualSurfaceY)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--face-csv") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options.faceCsv = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            if (!ReadUintArg(argc, argv, i, options.seed)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--start-distance") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.startDistance)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--end-distance") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.endDistance)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--tolerance") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.tolerance)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--quantum-tolerance") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.quantumTolerance)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--camera-x") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.cameraX)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--camera-y") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.cameraY)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--camera-z") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.cameraZ)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--footprint-step") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.footprintStep)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--min-faces") == 0) {
            if (!ReadIntArg(argc, argv, i, options.minFaces)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--grid-cell-count") == 0) {
            if (!ReadIntArg(argc, argv, i, options.gridCellCount)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--grid-cell-size") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.gridCellSize)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--owner-max-distance") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.ownerMaxDistance)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--grid-origin-x") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.gridOriginX)) {
                return false;
            }
            options.hasGridOriginOverride = true;
        } else if (std::strcmp(argv[i], "--grid-origin-z") == 0) {
            if (!ReadFloatArg(argc, argv, i, options.gridOriginZ)) {
                return false;
            }
            options.hasGridOriginOverride = true;
        } else if (std::strcmp(argv[i], "--allow-below-sea-faces") == 0) {
            options.forbidBelowSeaFaces = false;
        } else if (std::strcmp(argv[i], "--allow-side-faces") == 0) {
            options.allowSideFaces = true;
        } else if (std::strcmp(argv[i], "--require-grid-coverage") == 0) {
            options.requireGridCoverage = true;
        } else if (std::strcmp(argv[i], "--sample-mode") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            const std::string mode = argv[++i];
            if (mode == "all-bands") {
                options.allBands = true;
            } else if (mode == "handoff") {
                options.allBands = false;
            } else {
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return false;
        }
    }
    return (std::isfinite(options.actualSurfaceY) || !options.faceCsv.empty()) &&
        options.endDistance > options.startDistance &&
        options.tolerance >= 0.0f &&
        options.quantumTolerance >= 0.0f &&
        std::isfinite(options.cameraX) &&
        std::isfinite(options.cameraY) &&
        std::isfinite(options.cameraZ) &&
        std::isfinite(options.footprintStep) &&
        options.footprintStep > 0.0f &&
        options.minFaces >= 0 &&
        options.gridCellCount > 0 &&
        std::isfinite(options.gridCellSize) &&
        options.gridCellSize > 0.0f &&
        std::isfinite(options.ownerMaxDistance) &&
        options.ownerMaxDistance > options.startDistance;
}

float LocalVerticalQuantum(float distanceFromCamera) {
    const float cellSize = FarFallbackCellSize(distanceFromCamera);
    return std::max(4.0f, cellSize * 0.75f);
}

uint32_t FarOwnerTerrainMaterial(float x, float z, float height, uint32_t seed) {
    const TerrainSample s0 = FarTerrainHeight(x - 4.0f, z, seed);
    const TerrainSample s1 = FarTerrainHeight(x + 4.0f, z, seed);
    const TerrainSample s2 = FarTerrainHeight(x, z - 4.0f, seed);
    const TerrainSample s3 = FarTerrainHeight(x, z + 4.0f, seed);
    const float localRelief =
        std::max(
            std::max(std::abs(s0.height - height), std::abs(s1.height - height)),
            std::max(std::abs(s2.height - height), std::abs(s3.height - height)));

    if (height < kFarSeaLevel) {
        return kMatWater;
    }
    if (height < kFarSeaLevel + 48.0f && localRelief < 36.0f) {
        return kMatSand;
    }
    if (height < kFarSeaLevel + 72.0f) {
        return (height < kFarSeaLevel + 48.0f && localRelief < 36.0f) ? kMatSand : kMatDirt;
    }
    if (height < kFarSeaLevel + 128.0f) {
        return (height < kFarSeaLevel + 86.0f && localRelief < 58.0f) ? kMatSand : kMatDirt;
    }
    if (localRelief > 10.0f || height > 160.0f) {
        return kMatStone;
    }
    return kMatDirt;
}

struct ExpectedTopFace {
    int32_t worldX = 0;
    int32_t worldY = 0;
    int32_t worldZ = 0;
    uint32_t payload = 0;
};

bool BuildExpectedTopFace(
    int cellX,
    int cellZ,
    const Options& options,
    float farHandoff,
    float originX,
    float originZ,
    ExpectedTopFace& out)
{
    const float baseMinX = std::floor(originX + static_cast<float>(cellX) * options.gridCellSize);
    const float baseMinZ = std::floor(originZ + static_cast<float>(cellZ) * options.gridCellSize);
    const float baseCenterX = baseMinX + options.gridCellSize * 0.5f;
    const float baseCenterZ = baseMinZ + options.gridCellSize * 0.5f;

    float dx = baseCenterX - options.cameraX;
    float dy = -options.cameraY;
    float dz = baseCenterZ - options.cameraZ;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    float cellSize = FarFallbackCellSize(distance);
    float cellMinX = std::floor(baseCenterX / cellSize) * cellSize;
    float cellMinZ = std::floor(baseCenterZ / cellSize) * cellSize;
    float cellCenterX = cellMinX + cellSize * 0.5f;
    float cellCenterZ = cellMinZ + cellSize * 0.5f;
    float topY = FarTerrainHeightVoxelized(cellCenterX, cellCenterZ, distance, options.seed);

    dx = cellCenterX - options.cameraX;
    dy = topY - options.cameraY;
    dz = cellCenterZ - options.cameraZ;
    distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    cellSize = FarFallbackCellSize(distance);
    cellMinX = std::floor(cellCenterX / cellSize) * cellSize;
    cellMinZ = std::floor(cellCenterZ / cellSize) * cellSize;
    cellCenterX = cellMinX + cellSize * 0.5f;
    cellCenterZ = cellMinZ + cellSize * 0.5f;
    topY = FarTerrainHeightVoxelized(cellCenterX, cellCenterZ, distance, options.seed);

    dx = cellCenterX - options.cameraX;
    dy = topY - options.cameraY;
    dz = cellCenterZ - options.cameraZ;
    distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance < farHandoff ||
        distance > options.ownerMaxDistance ||
        topY <= kFarSeaLevel) {
        return false;
    }

    const uint32_t faceCellSize = static_cast<uint32_t>(std::lround(cellSize));
    const uint32_t material = FarOwnerTerrainMaterial(cellCenterX, cellCenterZ, topY, options.seed);
    out.worldX = static_cast<int32_t>(std::floor(cellMinX));
    out.worldY = static_cast<int32_t>(std::floor(topY)) - 1;
    out.worldZ = static_cast<int32_t>(std::floor(cellMinZ));
    out.payload = PackSparseSurfacePayload(3u, material, faceCellSize, faceCellSize);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr
            << "usage: far_owner_parity --actual-surface-y <y> "
            << "| --face-csv <worldX,worldY,worldZ,payload> "
            << "[--seed 12345] [--start-distance 256] [--end-distance 9000] "
            << "[--tolerance 1] [--quantum-tolerance 0] "
            << "[--camera-x 192] [--camera-y 0] [--camera-z 224] "
            << "[--footprint-step 4] [--min-faces 0] [--grid-cell-count 384] "
            << "[--grid-cell-size 28] [--owner-max-distance 11000] "
            << "[--grid-origin-x X --grid-origin-z Z] [--require-grid-coverage] "
            << "[--allow-below-sea-faces] "
            << "[--allow-side-faces] "
            << "[--sample-mode all-bands|handoff]\n";
        return 2;
    }

    const float farHandoff =
        options.startDistance + (options.endDistance - options.startDistance) * 0.62f;

    if (!options.faceCsv.empty()) {
        std::vector<OwnerFace> faces;
        FaceCsvMetadata metadata;
        std::string error;
        if (!LoadFaceCsv(options.faceCsv, faces, metadata, error)) {
            std::cerr << "FAIL: " << error << "\n";
            return 2;
        }
        if (metadata.cellSize > 0.0f) {
            options.gridCellSize = metadata.cellSize;
        }
        if (metadata.faces > 0) {
            const int cells = static_cast<int>(
                std::lround(std::sqrt(static_cast<float>(metadata.faces) * 0.5f)));
            if (cells > 0) {
                options.gridCellCount = cells;
            }
        }
        if (!options.hasGridOriginOverride && metadata.hasOrigin) {
            options.gridOriginX = metadata.originX;
            options.gridOriginZ = metadata.originZ;
            options.hasGridOriginOverride = true;
        }
        if (options.requireGridCoverage && !options.hasGridOriginOverride) {
            std::cerr << "FAIL: --require-grid-coverage needs originX/originZ in CSV header or explicit grid origin args.\n";
            return 2;
        }

        int samples = 0;
        int mismatches = 0;
        int nonTopFaces = 0;
        int facesChecked = 0;
        int belowSeaSamples = 0;
        int coverageFailures = 0;
        int expectedTopFaces = 0;
        int missingTopFaces = 0;
        int materialMismatches = 0;
        int sizeMismatches = 0;
        float maxAbsDelta = 0.0f;
        float sumAbsDelta = 0.0f;

        if (static_cast<int>(faces.size()) < options.minFaces) {
            coverageFailures = 1;
        }

        std::unordered_map<std::string, uint32_t> topFacePayloadByKey;
        topFacePayloadByKey.reserve(faces.size());
        for (const OwnerFace& face : faces) {
            if (PayloadDirection(face.payload) == 3u) {
                topFacePayloadByKey[FaceKey(face.worldX, face.worldY, face.worldZ)] = face.payload;
            }
        }

        struct FaceExample {
            OwnerFace face;
            float sampleX = 0.0f;
            float sampleZ = 0.0f;
            float distance = 0.0f;
            float quantum = 0.0f;
            float expectedTopY = 0.0f;
            int expectedFaceY = 0;
            bool belowSea = false;
            float delta = 0.0f;
        };
        std::vector<FaceExample> examples;

        struct CoverageExample {
            ExpectedTopFace expected;
            uint32_t actualPayload = 0u;
            bool missing = false;
            bool materialMismatch = false;
            bool sizeMismatch = false;
        };
        std::vector<CoverageExample> coverageExamples;

        for (const OwnerFace& face : faces) {
            const uint32_t direction = PayloadDirection(face.payload);
            if (direction != 3u) {
                ++nonTopFaces;
                if (options.allowSideFaces) {
                    continue;
                }
            }

            ++facesChecked;
            const float width = static_cast<float>(PayloadWidth(face.payload));
            const float height = static_cast<float>(PayloadHeight(face.payload));
            const float sampleStep =
                std::max(1.0f, std::min(options.footprintStep, std::min(width, height)));
            const float topY = static_cast<float>(face.worldY) + 1.0f;

            for (float localZ = sampleStep * 0.5f; localZ < height + 0.001f; localZ += sampleStep) {
                for (float localX = sampleStep * 0.5f; localX < width + 0.001f; localX += sampleStep) {
                    const float sampleX =
                        static_cast<float>(face.worldX) + std::min(localX, width - 0.5f);
                    const float sampleZ =
                        static_cast<float>(face.worldZ) + std::min(localZ, height - 0.5f);
                    const float dx = sampleX - options.cameraX;
                    const float dy = topY - options.cameraY;
                    const float dz = sampleZ - options.cameraZ;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float expectedTopY =
                        FarTerrainHeightVoxelized(sampleX, sampleZ, distance, options.seed);
                    const int expectedFaceY = static_cast<int>(std::floor(expectedTopY)) - 1;
                    const float delta = static_cast<float>(face.worldY - expectedFaceY);
                    const float absDelta = std::abs(delta);
                    const float quantum = LocalVerticalQuantum(distance);
                    const float allowed = std::max(options.tolerance, options.quantumTolerance * quantum);
                    const bool belowSea = expectedTopY <= kFarSeaLevel;
                    if (belowSea) {
                        ++belowSeaSamples;
                    }

                    ++samples;
                    sumAbsDelta += absDelta;
                    maxAbsDelta = std::max(maxAbsDelta, absDelta);
                    if (absDelta > allowed ||
                        direction != 3u ||
                        (options.forbidBelowSeaFaces && belowSea)) {
                        ++mismatches;
                        if (examples.size() < 8u) {
                            examples.push_back({
                                face,
                                sampleX,
                                sampleZ,
                                distance,
                                quantum,
                                expectedTopY,
                                expectedFaceY,
                                belowSea,
                                delta});
                        }
                    }
                }
            }
        }

        if (options.requireGridCoverage) {
            const float coverageFarHandoff =
                options.startDistance + (options.endDistance - options.startDistance) * 0.62f;
            for (int z = 0; z < options.gridCellCount; ++z) {
                for (int x = 0; x < options.gridCellCount; ++x) {
                    ExpectedTopFace expected;
                    if (!BuildExpectedTopFace(
                            x,
                            z,
                            options,
                            coverageFarHandoff,
                            options.gridOriginX,
                            options.gridOriginZ,
                            expected)) {
                        continue;
                    }
                    ++expectedTopFaces;
                    const auto found = topFacePayloadByKey.find(
                        FaceKey(expected.worldX, expected.worldY, expected.worldZ));
                    if (found == topFacePayloadByKey.end()) {
                        ++missingTopFaces;
                        if (coverageExamples.size() < 8u) {
                            coverageExamples.push_back({expected, 0u, true, false, false});
                        }
                        continue;
                    }
                    const uint32_t actualPayload = found->second;
                    const bool materialMismatch =
                        PayloadMaterial(actualPayload) != PayloadMaterial(expected.payload);
                    const bool sizeMismatch =
                        PayloadWidth(actualPayload) != PayloadWidth(expected.payload) ||
                        PayloadHeight(actualPayload) != PayloadHeight(expected.payload);
                    materialMismatches += materialMismatch ? 1 : 0;
                    sizeMismatches += sizeMismatch ? 1 : 0;
                    if ((materialMismatch || sizeMismatch) && coverageExamples.size() < 8u) {
                        coverageExamples.push_back({
                            expected,
                            actualPayload,
                            false,
                            materialMismatch,
                            sizeMismatch});
                    }
                }
            }
        }

        std::cout << std::fixed << std::setprecision(2);
        std::cout
            << "FAR_OWNER_FACE_PARITY faces=" << facesChecked
            << " samples=" << samples
            << " mismatches=" << mismatches
            << " nonTopFaces=" << nonTopFaces
            << " belowSeaSamples=" << belowSeaSamples
            << " coverageFailures=" << coverageFailures
            << " requireGridCoverage=" << (options.requireGridCoverage ? 1 : 0)
            << " expectedTopFaces=" << expectedTopFaces
            << " missingTopFaces=" << missingTopFaces
            << " materialMismatches=" << materialMismatches
            << " sizeMismatches=" << sizeMismatches
            << " minFaces=" << options.minFaces
            << " tolerance=" << options.tolerance
            << " quantumTolerance=" << options.quantumTolerance
            << " footprintStep=" << options.footprintStep
            << " gridOriginX=" << options.gridOriginX
            << " gridOriginZ=" << options.gridOriginZ
            << " gridCellCount=" << options.gridCellCount
            << " gridCellSize=" << options.gridCellSize
            << " maxAbsDelta=" << maxAbsDelta
            << " meanAbsDelta=" << (samples > 0 ? sumAbsDelta / static_cast<float>(samples) : 0.0f)
            << " cameraX=" << options.cameraX
            << " cameraY=" << options.cameraY
            << " cameraZ=" << options.cameraZ
            << "\n";

        for (const FaceExample& e : examples) {
            std::cout
                << "  mismatch face=(" << e.face.worldX << "," << e.face.worldY << ","
                << e.face.worldZ << "," << e.face.payload << ")"
                << " dir=" << PayloadDirection(e.face.payload)
                << " size=" << PayloadWidth(e.face.payload) << "x" << PayloadHeight(e.face.payload)
                << " sample=(" << e.sampleX << "," << e.sampleZ << ")"
                << " dist=" << e.distance
                << " quantum=" << e.quantum
                << " expectedTopY=" << e.expectedTopY
                << " expectedFaceY=" << e.expectedFaceY
                << " belowSea=" << (e.belowSea ? 1 : 0)
                << " delta=" << e.delta
                << "\n";
        }
        for (const CoverageExample& e : coverageExamples) {
            std::cout
                << "  coverage expected=(" << e.expected.worldX << "," << e.expected.worldY << ","
                << e.expected.worldZ << "," << e.expected.payload << ")"
                << " actualPayload=" << e.actualPayload
                << " missing=" << (e.missing ? 1 : 0)
                << " materialMismatch=" << (e.materialMismatch ? 1 : 0)
                << " sizeMismatch=" << (e.sizeMismatch ? 1 : 0)
                << " expectedMaterial=" << PayloadMaterial(e.expected.payload)
                << " actualMaterial=" << PayloadMaterial(e.actualPayload)
                << " expectedSize=" << PayloadWidth(e.expected.payload) << "x" << PayloadHeight(e.expected.payload)
                << " actualSize=" << PayloadWidth(e.actualPayload) << "x" << PayloadHeight(e.actualPayload)
                << "\n";
        }

        if (mismatches != 0 ||
            coverageFailures != 0 ||
            missingTopFaces != 0 ||
            materialMismatches != 0 ||
            sizeMismatches != 0) {
            std::cerr
                << "FAIL: generated far owner faces do not match FarTerrainHeightVoxelized.\n";
            return 1;
        }

        std::cout << "PASS: generated far owner faces match FarTerrainHeightVoxelized.\n";
        return 0;
    }

    const int actualFaceY = static_cast<int>(std::floor(options.actualSurfaceY)) - 1;

    const float originX = 192.0f;
    const float originZ = 224.0f;
    std::vector<float> distances;
    if (options.allBands) {
        distances = {
            1200.0f,
            900.0f + 6500.0f * 0.18f - 12.0f,
            900.0f + 6500.0f * 0.18f + 12.0f,
            900.0f + 6500.0f * 0.42f - 12.0f,
            900.0f + 6500.0f * 0.42f + 12.0f,
            900.0f + 6500.0f * 0.68f - 12.0f,
            900.0f + 6500.0f * 0.68f + 12.0f,
            farHandoff,
            farHandoff + 384.0f,
            (farHandoff + options.endDistance) * 0.5f,
            options.endDistance - 256.0f
        };
    } else {
        distances = {
            farHandoff,
            farHandoff + 384.0f,
            (farHandoff + options.endDistance) * 0.5f,
            options.endDistance - 256.0f
        };
    }
    const std::vector<float> angles = {
        -1.04719755f, -0.78539816f, -0.52359878f, -0.26179939f,
         0.0f,
         0.26179939f, 0.52359878f, 0.78539816f, 1.04719755f
    };

    int samples = 0;
    int mismatches = 0;
    float maxAbsDelta = 0.0f;
    float sumAbsDelta = 0.0f;

    struct Example {
        float x = 0.0f;
        float z = 0.0f;
        float distance = 0.0f;
        float quantum = 0.0f;
        float expectedTopY = 0.0f;
        int expectedFaceY = 0;
        int actualFaceY = 0;
        float delta = 0.0f;
    };
    std::vector<Example> examples;

    for (float distance : distances) {
        for (float angle : angles) {
            const float x = originX + std::sin(angle) * distance;
            const float z = originZ + std::cos(angle) * distance;
            const float expectedTopY =
                FarTerrainHeightVoxelized(x, z, distance, options.seed);
            const int expectedFaceY = static_cast<int>(std::floor(expectedTopY)) - 1;
            const float delta = static_cast<float>(actualFaceY - expectedFaceY);
            const float absDelta = std::abs(delta);
            const float quantum = LocalVerticalQuantum(distance);
            const float allowed = std::max(options.tolerance, options.quantumTolerance * quantum);

            ++samples;
            sumAbsDelta += absDelta;
            maxAbsDelta = std::max(maxAbsDelta, absDelta);
            if (absDelta > allowed) {
                ++mismatches;
                if (examples.size() < 6u) {
                    examples.push_back({x, z, distance, quantum, expectedTopY, expectedFaceY, actualFaceY, delta});
                }
            }
        }
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout
        << "FAR_OWNER_PARITY samples=" << samples
        << " mismatches=" << mismatches
        << " tolerance=" << options.tolerance
        << " quantumTolerance=" << options.quantumTolerance
        << " sampleMode=" << (options.allBands ? "all-bands" : "handoff")
        << " maxAbsDelta=" << maxAbsDelta
        << " meanAbsDelta=" << (samples > 0 ? sumAbsDelta / static_cast<float>(samples) : 0.0f)
        << " actualSurfaceY=" << options.actualSurfaceY
        << " actualFaceY=" << actualFaceY
        << " farHandoff=" << farHandoff
        << "\n";

    for (const Example& e : examples) {
        std::cout
            << "  mismatch x=" << e.x
            << " z=" << e.z
            << " dist=" << e.distance
            << " quantum=" << e.quantum
            << " expectedTopY=" << e.expectedTopY
            << " expectedFaceY=" << e.expectedFaceY
            << " actualFaceY=" << e.actualFaceY
            << " delta=" << e.delta
            << "\n";
    }

    if (mismatches != 0) {
        std::cerr
            << "FAIL: far owner surface does not match FarTerrainHeightVoxelized in the sampled far bands.\n";
        return 1;
    }

    std::cout << "PASS: far owner surface matches FarTerrainHeightVoxelized within tolerance.\n";
    return 0;
}
