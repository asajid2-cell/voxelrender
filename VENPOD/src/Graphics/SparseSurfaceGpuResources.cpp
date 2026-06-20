#include "SparseSurfaceGpuResources.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {

constexpr uint32_t kCullStatsUintCount = 13;

bool IsSparseSurfaceRangeTombstone(const Simulation::SparseSurfaceBrickRange& range) {
    return (range.flags & Simulation::kSparseSurfaceRangeTombstone) != 0u &&
        (range.flags & Simulation::kSparseSurfaceRangeValid) == 0u;
}

Simulation::SparseSurfaceBrickRange MakeSparseSurfaceRangeTombstone(
    const Simulation::BrickCoord& coord)
{
    Simulation::SparseSurfaceBrickRange range = {};
    range.coord = coord;
    range.flags = Simulation::kSparseSurfaceRangeTombstone;
    return range;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const uint64_t addend = alignment - 1u;
    if (value > std::numeric_limits<uint64_t>::max() - addend) {
        return std::numeric_limits<uint64_t>::max();
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool AddUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool AppendAlignedUploadRange(
    uint64_t currentEnd,
    uint64_t bytes,
    uint64_t alignment,
    uint64_t* outOffset,
    uint64_t* outEnd)
{
    if (!outOffset || !outEnd) {
        return false;
    }
    if (bytes == 0) {
        *outOffset = currentEnd;
        *outEnd = currentEnd;
        return true;
    }
    const uint64_t uploadOffset = AlignUp(currentEnd, alignment);
    uint64_t endOffset = 0;
    if (!AddUint64(uploadOffset, bytes, &endOffset)) {
        return false;
    }
    *outOffset = uploadOffset;
    *outEnd = endOffset;
    return true;
}

bool SameBytes(const void* lhs, const void* rhs, size_t byteCount) {
    return byteCount == 0 || std::memcmp(lhs, rhs, byteCount) == 0;
}

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

float ClampFinite(float value, float fallback, float minValue, float maxValue) {
    return std::clamp(FiniteOr(value, fallback), minValue, maxValue);
}

void SanitizeDirection(
    float* x,
    float* y,
    float* z,
    float fallbackX,
    float fallbackY,
    float fallbackZ)
{
    if (!x || !y || !z) {
        return;
    }
    const double dx = static_cast<double>(*x);
    const double dy = static_cast<double>(*y);
    const double dz = static_cast<double>(*z);
    const double lengthSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(*x) ||
        !std::isfinite(*y) ||
        !std::isfinite(*z) ||
        !std::isfinite(lengthSq) ||
        lengthSq <= 1.0e-12) {
        *x = fallbackX;
        *y = fallbackY;
        *z = fallbackZ;
        return;
    }
    const float invLength = static_cast<float>(1.0 / std::sqrt(lengthSq));
    *x *= invLength;
    *y *= invLength;
    *z *= invLength;
}

struct CullVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

float Dot(const CullVec3& lhs, const CullVec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

CullVec3 Sub(const CullVec3& lhs, const CullVec3& rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

CullVec3 Add(const CullVec3& lhs, const CullVec3& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

CullVec3 Mul(const CullVec3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float Length(const CullVec3& value) {
    return std::sqrt(Dot(value, value));
}

bool DirectionMaskFacesCameraCpu(uint32_t directionMask, const CullVec3& toCamera) {
    if (directionMask == 0u) {
        return true;
    }
    constexpr float epsilon = 0.25f;
    if ((directionMask & (1u << 0u)) != 0u && -toCamera.x >= -epsilon) return true;
    if ((directionMask & (1u << 1u)) != 0u &&  toCamera.x >= -epsilon) return true;
    if ((directionMask & (1u << 2u)) != 0u && -toCamera.y >= -epsilon) return true;
    if ((directionMask & (1u << 3u)) != 0u &&  toCamera.y >= -epsilon) return true;
    if ((directionMask & (1u << 4u)) != 0u && -toCamera.z >= -epsilon) return true;
    if ((directionMask & (1u << 5u)) != 0u &&  toCamera.z >= -epsilon) return true;
    return false;
}

uint32_t ClassifySparseSurfaceClusterCullCpu(
    const Simulation::SparseSurfaceClusterRecord& cluster,
    const CullVec3& camera,
    const CullVec3& forward,
    const CullVec3& right,
    const CullVec3& up,
    float fovYRadians,
    float aspectRatio,
    float maxDistance,
    float padding)
{
    constexpr uint32_t kClusterOutside = 0u;
    constexpr uint32_t kClusterIntersect = 1u;
    constexpr uint32_t kClusterInside = 2u;
    constexpr uint32_t kClusterBackface = 3u;
    if (cluster.recordCount == 0u) {
        return kClusterOutside;
    }
    const CullVec3 brickMin{
        static_cast<float>(cluster.minX),
        static_cast<float>(cluster.minY),
        static_cast<float>(cluster.minZ)};
    const CullVec3 brickMax{
        static_cast<float>(cluster.maxX),
        static_cast<float>(cluster.maxY),
        static_cast<float>(cluster.maxZ)};
    const CullVec3 center = Mul(Add(brickMin, brickMax), 0.5f);
    const CullVec3 half = Mul(Sub(brickMax, brickMin), 0.5f);
    const CullVec3 extent{
        std::max(half.x, 1.0f),
        std::max(half.y, 1.0f),
        std::max(half.z, 1.0f)};
    const float radius = Length(extent);
    const CullVec3 rel = Sub(center, camera);
    const float viewZ = Dot(rel, forward);
    const float distanceLimit = std::max(maxDistance, 1.0f) + padding + radius;
    if (Dot(rel, rel) > distanceLimit * distanceLimit) {
        return kClusterOutside;
    }
    if (viewZ < -radius || viewZ > distanceLimit) {
        return kClusterOutside;
    }
    const float tanHalfFov = std::tan(fovYRadians * 0.5f);
    const float safeAspect = std::max(aspectRatio, 0.001f);
    const float viewX = Dot(rel, right);
    const float viewY = Dot(rel, up);
    const float xLimit = std::max(viewZ, 0.0f) * tanHalfFov * safeAspect + padding + radius;
    const float yLimit = std::max(viewZ, 0.0f) * tanHalfFov + padding + radius;
    if (std::abs(viewX) > xLimit || std::abs(viewY) > yLimit) {
        return kClusterOutside;
    }
    const uint32_t directionMask = Simulation::SparseSurfaceRecordDirectionMask(cluster.flags);
    if (!DirectionMaskFacesCameraCpu(directionMask, Sub(camera, center))) {
        return kClusterBackface;
    }
    const float insideMaxDistance = std::max(maxDistance, 1.0f) + padding - radius;
    const float insideXLimit = std::max(viewZ, 0.0f) * tanHalfFov * safeAspect + padding - radius;
    const float insideYLimit = std::max(viewZ, 0.0f) * tanHalfFov + padding - radius;
    if (viewZ >= radius &&
        viewZ <= insideMaxDistance &&
        insideXLimit > 0.0f &&
        insideYLimit > 0.0f &&
        std::abs(viewX) <= insideXLimit &&
        std::abs(viewY) <= insideYLimit) {
        return kClusterInside;
    }
    return kClusterIntersect;
}

uint32_t ClassifySparseSurfaceRecordCullCpu(
    const Simulation::SparseSurfaceRecord& record,
    const CullVec3& camera,
    const CullVec3& forward,
    const CullVec3& right,
    const CullVec3& up,
    float fovYRadians,
    float aspectRatio,
    float maxDistance,
    float padding)
{
    constexpr uint32_t kStatAccepted = 0u;
    constexpr uint32_t kStatRejectInvalid = 1u;
    constexpr uint32_t kStatRejectDistance = 2u;
    constexpr uint32_t kStatRejectFrustum = 3u;
    constexpr uint32_t kStatRejectBackface = 11u;
    if ((record.flags & Simulation::kSparseSurfaceRangeValid) == 0u || record.faceCount == 0u) {
        return kStatRejectInvalid;
    }
    const CullVec3 brickMin{
        static_cast<float>(record.minX),
        static_cast<float>(record.minY),
        static_cast<float>(record.minZ)};
    const CullVec3 brickMax{
        static_cast<float>(record.maxX),
        static_cast<float>(record.maxY),
        static_cast<float>(record.maxZ)};
    const CullVec3 center = Mul(Add(brickMin, brickMax), 0.5f);
    const CullVec3 half = Mul(Sub(brickMax, brickMin), 0.5f);
    const CullVec3 extent{
        std::max(half.x, 0.5f),
        std::max(half.y, 0.5f),
        std::max(half.z, 0.5f)};
    const float radius = std::max(Length(extent), 0.5f);
    const CullVec3 rel = Sub(center, camera);
    const float viewZ = Dot(rel, forward);
    const float distanceLimit = std::max(maxDistance, 1.0f) + padding + radius;
    if (Dot(rel, rel) > distanceLimit * distanceLimit) {
        return kStatRejectDistance;
    }
    if (viewZ < -radius || viewZ > distanceLimit) {
        return kStatRejectDistance;
    }
    const float tanHalfFov = std::tan(fovYRadians * 0.5f);
    const float safeAspect = std::max(aspectRatio, 0.001f);
    const float viewX = Dot(rel, right);
    const float viewY = Dot(rel, up);
    const float xLimit = std::max(viewZ, 0.0f) * tanHalfFov * safeAspect + padding + radius;
    const float yLimit = std::max(viewZ, 0.0f) * tanHalfFov + padding + radius;
    if (std::abs(viewX) > xLimit || std::abs(viewY) > yLimit) {
        return kStatRejectFrustum;
    }
    const uint32_t directionMask = Simulation::SparseSurfaceRecordDirectionMask(record.flags);
    if (!DirectionMaskFacesCameraCpu(directionMask, Sub(camera, center))) {
        return kStatRejectBackface;
    }
    return kStatAccepted;
}

void SetSurfaceRecordBoundsFromBrickCoord(
    const Simulation::BrickCoord& coord,
    Simulation::SparseSurfaceRecord& record)
{
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;
    int32_t maxZ = 0;
    const bool minOk =
        Simulation::TryWorldVoxelFromBrickLocal(coord.x, 0, &minX) &&
        Simulation::TryWorldVoxelFromBrickLocal(coord.y, 0, &minY) &&
        Simulation::TryWorldVoxelFromBrickLocal(coord.z, 0, &minZ);
    const bool maxOk =
        Simulation::TryWorldVoxelFromBrickLocal(coord.x, Simulation::SPARSE_BRICK_SIZE - 1u, &maxX) &&
        Simulation::TryWorldVoxelFromBrickLocal(coord.y, Simulation::SPARSE_BRICK_SIZE - 1u, &maxY) &&
        Simulation::TryWorldVoxelFromBrickLocal(coord.z, Simulation::SPARSE_BRICK_SIZE - 1u, &maxZ);
    if (minOk && maxOk) {
        record.minX = minX;
        record.minY = minY;
        record.minZ = minZ;
        record.maxX = maxX;
        record.maxY = maxY;
        record.maxZ = maxZ;
        return;
    }
    record.minX = record.minY = record.minZ = 0;
    record.maxX = record.maxY = record.maxZ = 0;
}

} // namespace

SparseSurfaceGpuResources::~SparseSurfaceGpuResources() {
    Shutdown();
}

void SparseSurfaceGpuResources::RebuildSurfaceRecordLookup() {
    m_surfaceRecordIndexByCoord.clear();
    m_surfaceRecordIndexByCoord.reserve(m_surfaceRecordMirror.size());
    m_surfaceRecordClusterIndex.assign(m_surfaceRecordMirror.size(), UINT32_MAX);

    for (uint32_t recordIndex = 0;
         recordIndex < static_cast<uint32_t>(m_surfaceRecordMirror.size());
         ++recordIndex) {
        const Simulation::SparseSurfaceRecord& record = m_surfaceRecordMirror[recordIndex];
        if (record.flags == 0u || record.faceCount == 0u) {
            continue;
        }
        m_surfaceRecordIndexByCoord[record.coord] = recordIndex;
    }

    for (uint32_t clusterIndex = 0;
         clusterIndex < static_cast<uint32_t>(m_surfaceClusterMirror.size());
         ++clusterIndex) {
        const Simulation::SparseSurfaceClusterRecord& cluster =
            m_surfaceClusterMirror[clusterIndex];
        const uint64_t clusterEnd =
            static_cast<uint64_t>(cluster.firstRecord) +
            static_cast<uint64_t>(cluster.recordCount);
        const uint32_t recordEnd = static_cast<uint32_t>(
            std::min<uint64_t>(clusterEnd, m_surfaceRecordClusterIndex.size()));
        for (uint32_t recordIndex = cluster.firstRecord; recordIndex < recordEnd; ++recordIndex) {
            m_surfaceRecordClusterIndex[recordIndex] = clusterIndex;
        }
    }
}

bool SparseSurfaceGpuResources::TryGetBrickDebugInfo(
    const Simulation::BrickCoord& coord,
    SparseSurfaceGpuBrickDebugInfo* outInfo) const
{
    SparseSurfaceGpuBrickDebugInfo info;
    info.payloadResident = m_payloadResidentCoords.find(coord) != m_payloadResidentCoords.end();

    const auto payloadIt = m_payloadFaceMirrorByCoord.find(coord);
    if (payloadIt != m_payloadFaceMirrorByCoord.end()) {
        info.payloadFaceCount = static_cast<uint32_t>(payloadIt->second.size());
    }

    const auto recordIt = m_surfaceRecordIndexByCoord.find(coord);
    if (recordIt != m_surfaceRecordIndexByCoord.end() &&
        recordIt->second < m_surfaceRecordMirror.size()) {
        const Simulation::SparseSurfaceRecord& record = m_surfaceRecordMirror[recordIt->second];
        info.surfaceRecordPresent = record.flags != 0u && record.faceCount > 0u;
        info.surfaceRecordIndex = recordIt->second;
        info.surfaceRecordFirstFace = record.firstFace;
        info.surfaceRecordFaceCount = record.faceCount;
        info.surfaceRecordFlags = record.flags;
    }

    const auto slotIt = m_drawSlotByCoord.find(coord);
    if (slotIt != m_drawSlotByCoord.end()) {
        info.drawSlotPresent = true;
        info.drawSlot = slotIt->second;
    }

    if (!m_rangeMirror.empty()) {
        const uint32_t mask = static_cast<uint32_t>(m_rangeMirror.size() - 1u);
        uint32_t slot = Simulation::HashBrickCoord32(coord) & mask;
        for (uint32_t probe = 0; probe < static_cast<uint32_t>(m_rangeMirror.size()); ++probe) {
            const Simulation::SparseSurfaceBrickRange& range = m_rangeMirror[slot];
            if (range.flags == 0u) {
                break;
            }
            if (range.coord == coord) {
                info.rangePresent = true;
                info.rangeFirstFace = range.firstFace;
                info.rangeFaceCount = range.faceCount;
                info.rangeFlags = range.flags;
                break;
            }
            slot = (slot + 1u) & mask;
        }
    }

    const bool present =
        info.payloadResident ||
        info.surfaceRecordPresent ||
        info.drawSlotPresent ||
        info.rangePresent ||
        info.payloadFaceCount > 0u;
    if (outInfo) {
        *outInfo = info;
    }
    return present;
}

bool SparseSurfaceGpuResources::TryClassifyBrickGpuCull(
    const Simulation::BrickCoord& coord,
    float cameraX,
    float cameraY,
    float cameraZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    float rightX,
    float rightY,
    float rightZ,
    float upX,
    float upY,
    float upZ,
    float fovYRadians,
    float aspectRatio,
    float maxDistance,
    float padding,
    SparseSurfaceGpuCullDebugInfo* outInfo) const
{
    SparseSurfaceGpuCullDebugInfo info;
    const auto recordIt = m_surfaceRecordIndexByCoord.find(coord);
    if (recordIt == m_surfaceRecordIndexByCoord.end() ||
        recordIt->second >= m_surfaceRecordMirror.size()) {
        if (outInfo) {
            *outInfo = info;
        }
        return false;
    }

    float sanitizedForwardX = forwardX;
    float sanitizedForwardY = forwardY;
    float sanitizedForwardZ = forwardZ;
    float sanitizedRightX = rightX;
    float sanitizedRightY = rightY;
    float sanitizedRightZ = rightZ;
    float sanitizedUpX = upX;
    float sanitizedUpY = upY;
    float sanitizedUpZ = upZ;
    SanitizeDirection(
        &sanitizedForwardX,
        &sanitizedForwardY,
        &sanitizedForwardZ,
        0.0f,
        0.0f,
        1.0f);
    SanitizeDirection(
        &sanitizedRightX,
        &sanitizedRightY,
        &sanitizedRightZ,
        1.0f,
        0.0f,
        0.0f);
    SanitizeDirection(
        &sanitizedUpX,
        &sanitizedUpY,
        &sanitizedUpZ,
        0.0f,
        1.0f,
        0.0f);

    const CullVec3 camera{
        FiniteOr(cameraX, 0.0f),
        FiniteOr(cameraY, 0.0f),
        FiniteOr(cameraZ, 0.0f)};
    const CullVec3 forward{sanitizedForwardX, sanitizedForwardY, sanitizedForwardZ};
    const CullVec3 right{sanitizedRightX, sanitizedRightY, sanitizedRightZ};
    const CullVec3 up{sanitizedUpX, sanitizedUpY, sanitizedUpZ};
    const float safeFov = ClampFinite(fovYRadians, 1.0471976f, 0.05f, 3.0f);
    const float safeAspect = ClampFinite(aspectRatio, 1.7777778f, 0.1f, 10.0f);
    const float safeMaxDistance = ClampFinite(maxDistance, 16384.0f, 0.0f, 10000000.0f);
    const float safePadding = ClampFinite(padding, 0.0f, 0.0f, 4096.0f);

    info.hasRecord = true;
    info.surfaceRecordIndex = recordIt->second;
    const Simulation::SparseSurfaceRecord& record = m_surfaceRecordMirror[recordIt->second];
    const uint32_t clusterIndex =
        recordIt->second < m_surfaceRecordClusterIndex.size()
            ? m_surfaceRecordClusterIndex[recordIt->second]
            : UINT32_MAX;
    if (clusterIndex != UINT32_MAX && clusterIndex < m_surfaceClusterMirror.size()) {
        info.hasCluster = true;
        info.clusterIndex = clusterIndex;
        info.clusterClass = ClassifySparseSurfaceClusterCullCpu(
            m_surfaceClusterMirror[clusterIndex],
            camera,
            forward,
            right,
            up,
            safeFov,
            safeAspect,
            safeMaxDistance,
            safePadding);
        info.clusterRejected = info.clusterClass == 0u || info.clusterClass == 3u;
    }
    info.recordClass = ClassifySparseSurfaceRecordCullCpu(
        record,
        camera,
        forward,
        right,
        up,
        safeFov,
        safeAspect,
        safeMaxDistance,
        safePadding);
    info.recordRejected = info.recordClass != 0u;

    if (outInfo) {
        *outInfo = info;
    }
    return true;
}

Result<void> SparseSurfaceGpuResources::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    ShaderCompiler* shaderCompiler,
    const std::filesystem::path& shaderPath,
    const SparseSurfaceGpuConfig& config)
{
    if (!device) {
        return Error("SparseSurfaceGpuResources::Initialize - device is null");
    }
    if (!ValidateSparseSurfaceGpuConfigForStats(config)) {
        return Error("SparseSurfaceGpuResources::Initialize - config is outside sparse surface GPU runtime limits");
    }

    Shutdown();
    m_config = config;
    m_heapManager = &heapManager;
    m_stats = {};
    m_stats.initialized = true;
    m_stats.rangeAllocatorEnabled = config.useRangeAllocator;
    m_stats.fixedRangeTableEnabled = config.useFixedRangeTable;
    m_stats.stableDrawSlotsEnabled = config.useStableDrawSlots;
    m_stats.compactStableDrawCommandsEnabled =
        config.useStableDrawSlots && config.compactStableDrawCommands;
    m_stats.incrementalMetadataAddsEnabled = config.incrementalMetadataAdds;
    m_stats.gpuCullEnabled = config.useGpuCull;
    m_stats.maxFaces = config.maxFaces;
    m_stats.maxBrickRanges = config.maxBrickRanges;
    m_stats.maxDrawCommands = config.maxDrawCommands;
    m_stats.payloadCopyRegionBudget = config.maxPayloadCopyRegionsPerFrame;
    m_stats.payloadCopyFaceBudget = config.maxPayloadCopyFacesPerFrame;
    m_stats.surfaceRecordsPerCluster = config.surfaceRecordsPerCluster;
    m_stats.surfaceClusterMaxExtentVoxels = config.surfaceClusterMaxExtentVoxels;
    m_stats.surfaceClusterFastAcceptMaxRecords = config.surfaceClusterFastAcceptMaxRecords;
    m_stats.surfaceClusterFastAcceptMaxFaces = config.surfaceClusterFastAcceptMaxFaces;
    if (config.useRangeAllocator) {
        m_faceRangeAllocator.Initialize(config.maxFaces, config.rangeRetirementDelayFrames);
    }

    auto result = m_faceBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxFaces) * sizeof(Simulation::SparseSurfaceFace),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceFace),
        "SparseSurfaceFaceBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface face buffer: {}", result.error());
    }
    result = m_faceBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface face SRV: {}", result.error());
    }

    result = m_rangeBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxBrickRanges) * sizeof(Simulation::SparseSurfaceBrickRange),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceBrickRange),
        "SparseSurfaceRangeBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface range buffer: {}", result.error());
    }
    result = m_rangeBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface range SRV: {}", result.error());
    }

    result = m_drawArgsBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceDrawArgs),
        BufferUsage::IndirectArgument | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(Simulation::SparseSurfaceDrawArgs),
        "SparseSurfaceDrawArgsBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw args buffer: {}", result.error());
    }
    result = m_drawArgsBuffer.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw args UAV: {}", result.error());
    }
    result = m_fallbackDrawArgsBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceDrawArgs),
        BufferUsage::IndirectArgument | BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceDrawArgs),
        "SparseSurfaceFallbackDrawArgsBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface fallback draw args buffer: {}", result.error());
    }
    result = m_fallbackDrawArgsUpload.Initialize(
        device,
        std::max<uint64_t>(
            static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceDrawArgs),
            256u),
        "SparseSurfaceFallbackDrawArgsUpload");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface fallback draw args upload: {}", result.error());
    }

    result = m_surfaceRecordBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceRecord),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceRecord),
        "SparseSurfaceRecordBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface record buffer: {}", result.error());
    }
    result = m_surfaceRecordBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface record SRV: {}", result.error());
    }

    result = m_surfaceClusterBuffer.Initialize(
        device,
        static_cast<uint64_t>(config.maxDrawCommands) * sizeof(Simulation::SparseSurfaceClusterRecord),
        BufferUsage::StructuredBuffer,
        sizeof(Simulation::SparseSurfaceClusterRecord),
        "SparseSurfaceClusterBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface cluster buffer: {}", result.error());
    }
    result = m_surfaceClusterBuffer.CreateSRV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface cluster SRV: {}", result.error());
    }

    result = m_drawCountBuffer.Initialize(
        device,
        sizeof(uint32_t) * kCullStatsUintCount,
        BufferUsage::IndirectArgument | BufferUsage::StructuredBuffer | BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "SparseSurfaceCullStatsBuffer");
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw count buffer: {}", result.error());
    }
    result = m_drawCountBuffer.CreateUAV(device, heapManager);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface draw count UAV: {}", result.error());
    }

    result = CreateVertexIdStream(device);
    if (!result) {
        Shutdown();
        return Error("Failed to create sparse surface vertex-id stream: {}", result.error());
    }

    for (uint32_t i = 0; i < config.uploadRingSlots; ++i) {
        result = m_uploadRing[i].Initialize(
            device,
            config.uploadBytesPerSlot,
            "SparseSurfaceUploadRing");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface upload ring: {}", result.error());
        }
        result = m_cullConstantUploads[i].Initialize(
            device,
            256u,
            "SparseSurfaceCullConstants");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull constants: {}", result.error());
        }
        result = m_cullStatsReadback[i].Initialize(
            device,
            sizeof(uint32_t) * kCullStatsUintCount,
            BufferUsage::Readback,
            sizeof(uint32_t),
            "SparseSurfaceCullStatsReadback");
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull stats readback slot {}: {}", i, result.error());
        }
    }

    if (config.useGpuCull) {
        if (!shaderCompiler || shaderPath.empty()) {
            Shutdown();
            return Error("SparseSurfaceGpuResources::Initialize - GPU cull requested without shader compiler/path");
        }
        const std::filesystem::path csPath = shaderPath / "Compute" / "CS_SparseSurfaceCullCompact.hlsl";
        auto compileResult = shaderCompiler->CompileComputeShader(csPath, L"main", config.useGpuCull);
        if (!compileResult) {
            Shutdown();
            return Error("Failed to compile sparse surface cull shader: {}", compileResult.error());
        }
        m_surfaceCullShader = compileResult.value();
        if (!m_surfaceCullShader.IsValid()) {
            Shutdown();
            return Error("Sparse surface cull shader compilation failed: {}", m_surfaceCullShader.errors);
        }
        ComputePipelineDesc cullDesc;
        cullDesc.computeShader = m_surfaceCullShader;
        cullDesc.debugName = "SparseSurfaceCullCompact";
        cullDesc.rootParams.push_back({RootParamType::ConstantBuffer, 0, 0});
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            0,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            1,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            0,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV
        });
        cullDesc.rootParams.push_back({
            RootParamType::DescriptorTable,
            1,
            0,
            1,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV
        });
        result = m_surfaceCullPipeline.Initialize(device, cullDesc);
        if (!result) {
            Shutdown();
            return Error("Failed to create sparse surface cull pipeline: {}", result.error());
        }
        m_stats.gpuCullEnabled = true;
    }

    return {};
}

void SparseSurfaceGpuResources::Shutdown() {
    m_faceBuffer.Shutdown();
    m_rangeBuffer.Shutdown();
    m_drawArgsBuffer.Shutdown();
    m_fallbackDrawArgsBuffer.Shutdown();
    m_surfaceRecordBuffer.Shutdown();
    m_surfaceClusterBuffer.Shutdown();
    m_drawCountBuffer.Shutdown();
    m_vertexIdStream.Shutdown();
    m_indexStream.Shutdown();
    m_vertexIdStreamUpload.Shutdown();
    m_indexStreamUpload.Shutdown();
    m_fallbackDrawArgsUpload.Shutdown();
    m_vertexIdBufferView = {};
    m_indexBufferView = {};
    m_vertexIdCapacityFaces = 0;
    m_staticIaUploadPending = false;
    m_staticIaUploadComplete = false;
    m_staticIaUploadFence = 0;
    m_currentFrameFenceValue = 0;
    for (auto& readback : m_cullStatsReadback) {
        readback.Shutdown();
    }
    m_cullStatsReadbackPending = {};
    m_cullStatsReadbackQueuedFrames = {};
    for (auto& upload : m_uploadRing) {
        upload.Shutdown();
    }
    for (auto& upload : m_cullConstantUploads) {
        upload.Shutdown();
    }
    m_surfaceCullPipeline.Shutdown();
    m_surfaceCullShader = {};
    m_heapManager = nullptr;
    m_stats = {};
    m_faceRangeAllocator.Clear();
    m_payloadResidentCoords.clear();
    m_rangeMirror.clear();
    m_drawArgsMirror.clear();
    m_surfaceRecordMirror.clear();
    m_surfaceClusterMirror.clear();
    m_surfaceRecordIndexByCoord.clear();
    m_surfaceRecordClusterIndex.clear();
    m_drawSlotByCoord.clear();
    m_payloadFaceMirrorByCoord.clear();
    m_drawSlotOccupied.clear();
    m_freeDrawSlots.clear();
    m_uploadWriteOffset = 0;
    m_activeUploadSlot = 0;
}

Result<void> SparseSurfaceGpuResources::CreateVertexIdStream(ID3D12Device* device) {
    if (!device) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - device is null");
    }
    if (m_config.maxFaces == 0u) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - maxFaces must be > 0");
    }
    SparseSurfaceIaStreamSizing sizing;
    if (!TryBuildSparseSurfaceIaStreamSizing(m_config.maxFaces, sizing)) {
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - maxFaces is too large for indexed IA buffers");
    }

    const uint32_t vertexCount = sizing.vertexCount;
    const uint32_t indexCount = sizing.indexCount;
    const uint64_t vertexByteCount = sizing.vertexBytes;
    const uint64_t indexByteCount = sizing.indexBytes;
    auto result = m_vertexIdStream.Initialize(
        device,
        vertexByteCount,
        BufferUsage::Default,
        sizeof(uint32_t),
        "SparseSurfaceVertexIdStream");
    if (!result) {
        return result;
    }
    result = m_indexStream.Initialize(
        device,
        indexByteCount,
        BufferUsage::Default,
        sizeof(uint32_t),
        "SparseSurfaceIndexStream");
    if (!result) {
        m_vertexIdStream.Shutdown();
        return result;
    }
    result = m_vertexIdStreamUpload.Initialize(
        device,
        vertexByteCount,
        "SparseSurfaceVertexIdStreamUpload");
    if (!result) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        return result;
    }
    result = m_indexStreamUpload.Initialize(
        device,
        indexByteCount,
        "SparseSurfaceIndexStreamUpload");
    if (!result) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        return result;
    }

    uint32_t* mapped = static_cast<uint32_t*>(m_vertexIdStreamUpload.GetMappedData());
    if (!mapped) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - upload buffer is not mapped");
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        mapped[i] = i;
    }
    uint32_t* mappedIndices = static_cast<uint32_t*>(m_indexStreamUpload.GetMappedData());
    if (!mappedIndices) {
        m_vertexIdStream.Shutdown();
        m_indexStream.Shutdown();
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        return Error("SparseSurfaceGpuResources::CreateVertexIdStream - index upload buffer is not mapped");
    }
    for (uint32_t face = 0; face < m_config.maxFaces; ++face) {
        const uint32_t vertexBase = face * 4u;
        const uint32_t indexBase = face * 6u;
        mappedIndices[indexBase + 0u] = vertexBase + 0u;
        mappedIndices[indexBase + 1u] = vertexBase + 1u;
        mappedIndices[indexBase + 2u] = vertexBase + 2u;
        mappedIndices[indexBase + 3u] = vertexBase + 0u;
        mappedIndices[indexBase + 4u] = vertexBase + 2u;
        mappedIndices[indexBase + 5u] = vertexBase + 3u;
    }

    m_vertexIdCapacityFaces = m_config.maxFaces;
    m_vertexIdBufferView.BufferLocation = m_vertexIdStream.GetGPUVirtualAddress();
    m_vertexIdBufferView.SizeInBytes = static_cast<UINT>(vertexByteCount);
    m_vertexIdBufferView.StrideInBytes = sizeof(uint32_t);
    m_indexBufferView.BufferLocation = m_indexStream.GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexByteCount);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_stats.iaStreamCapacityFaces = m_vertexIdCapacityFaces;
    m_stats.iaStreamVertexCount = vertexCount;
    m_stats.iaStreamIndexCount = indexCount;
    m_stats.iaStreamVertexBytes = vertexByteCount;
    m_stats.iaStreamIndexBytes = indexByteCount;
    m_stats.iaStreamGpuLocal = true;
    m_stats.iaStreamUploadPending = true;
    m_staticIaUploadPending = true;
    m_staticIaUploadComplete = false;
    m_staticIaUploadFence = 0;
    spdlog::info(
        "Sparse surface indexed IA streams created: {} faces, GPU-local vertexIds={:.2f} MB indices={:.2f} MB",
        m_vertexIdCapacityFaces,
        static_cast<double>(vertexByteCount) / (1024.0 * 1024.0),
        static_cast<double>(indexByteCount) / (1024.0 * 1024.0));
    return {};
}

void SparseSurfaceGpuResources::BeginFrame(
    uint32_t frameIndex,
    uint64_t completedFenceValue,
    uint64_t currentFrameFenceValue)
{
    m_currentFrameFenceValue = currentFrameFenceValue;
    if (m_staticIaUploadComplete &&
        !m_staticIaUploadPending &&
        m_staticIaUploadFence != 0u &&
        completedFenceValue >= m_staticIaUploadFence) {
        m_vertexIdStreamUpload.Shutdown();
        m_indexStreamUpload.Shutdown();
        m_staticIaUploadFence = 0;
    }
    m_stats.iaStreamUploadPending = m_staticIaUploadPending;
    m_stats.iaStreamUploadRetireFence = m_staticIaUploadFence;
    if (m_config.uploadRingSlots == 0) {
        return;
    }
    m_activeUploadSlot = frameIndex % m_config.uploadRingSlots;
    m_uploadWriteOffset = 0;
    if (m_config.useRangeAllocator) {
        if (currentFrameFenceValue != 0u) {
            m_faceRangeAllocator.BeginFrame(completedFenceValue, currentFrameFenceValue);
        } else {
            m_faceRangeAllocator.BeginFrame(frameIndex);
        }
        const auto& allocatorStats = m_faceRangeAllocator.GetStats();
        m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
        m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
        m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
        m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
        m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
        m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
        m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
    }
    m_stats.stagedFacesLastFrame = 0;
    m_stats.stagedRangesLastFrame = 0;
    m_stats.stagedRangeTableCapacityLastFrame = 0;
    m_stats.stagedDrawCommandsLastFrame = 0;
    m_stats.stagedRangeCopyRegionsLastFrame = 0;
    m_stats.stagedDrawCopyRegionsLastFrame = 0;
    m_stats.skippedCleanRangeSlotsLastFrame = 0;
    m_stats.skippedCleanDrawCommandsLastFrame = 0;
    m_stats.fullRangeTableUploadLastFrame = false;
    m_stats.fullDrawArgsUploadLastFrame = false;
    m_stats.activeDrawCommandsLastFrame = 0;
    m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(m_drawSlotOccupied.size());
    m_stats.stableDrawFreeSlots = static_cast<uint32_t>(m_freeDrawSlots.size());
    m_stats.inactiveStableDrawSlotsLastFrame = 0;
    m_stats.stagedSurfaceRecordsLastFrame = 0;
    m_stats.stagedSurfaceRecordCopyRegionsLastFrame = 0;
    m_stats.skippedCleanSurfaceRecordsLastFrame = 0;
    m_stats.fullSurfaceRecordUploadLastFrame = false;
    m_stats.stagedSurfaceClustersLastFrame = 0;
    m_stats.stagedSurfaceClusterCopyRegionsLastFrame = 0;
    m_stats.skippedCleanSurfaceClustersLastFrame = 0;
    m_stats.fullSurfaceClusterUploadLastFrame = false;
    m_stats.gpuCullDispatchesLastFrame = 0;
    m_stats.gpuCullCandidateRecordsLastFrame = 0;
    m_stats.gpuCullCandidateClustersLastFrame = m_stats.uploadedSurfaceClusters;
    m_stats.gpuCullMaxDrawCommands = m_config.maxDrawCommands;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
    m_stats.stagedCandidateBricksLastFrame = 0;
    m_stats.stagedVisibleBricksLastFrame = 0;
    m_stats.stagedCulledBricksLastFrame = 0;
    m_stats.stagedFaceCopyRegionsLastFrame = 0;
    m_stats.stagedPayloadPatchBricksLastFrame = 0;
    m_stats.stagedPayloadPatchFacesLastFrame = 0;
    m_stats.stagedPayloadPatchRegionsLastFrame = 0;
    m_stats.stagedDirtyPayloadBricksLastFrame = 0;
    m_stats.skippedCleanPayloadBricksLastFrame = 0;
    m_stats.deferredPayloadBricksLastFrame = 0;
    m_stats.residentPayloadBricks = static_cast<uint32_t>(m_payloadResidentCoords.size());
    m_stats.pendingDirtyBricksLastFrame = 0;
    m_stats.pendingRemovedBricksLastFrame = 0;
    m_stats.stagedBytesLastFrame = 0;
    m_stats.uploadOverflowLastFrame = false;
}

bool SparseSurfaceGpuResources::StageDirtyPayloadSnapshot(
    const Simulation::SparseSurfaceGpuSnapshot& snapshot,
    SparseSurfaceUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    // Record exactly which precondition rejects a dirty stage so callers (mid-mesh)
    // can log it instead of guessing why they fall back to a full StageSnapshot.
    const char* rejectReason = nullptr;
    if (!m_stats.initialized) rejectReason = "not-initialized";
    else if (!m_config.useRangeAllocator) rejectReason = "no-range-allocator";
    else if (m_activeUploadSlot >= m_config.uploadRingSlots) rejectReason = "no-upload-slot";
    else if (snapshot.dirtyBricks.empty() && snapshot.removedBricks.empty()) rejectReason = "nothing-dirty";
    else if (m_rangeMirror.empty()) rejectReason = "range-mirror-empty(unprimed)";
    else if (m_drawArgsMirror.empty()) rejectReason = "drawargs-mirror-empty(unprimed)";
    else if (m_surfaceRecordMirror.empty()) rejectReason = "record-mirror-empty(unprimed)";
    else if (m_surfaceClusterMirror.empty()) rejectReason = "cluster-mirror-empty(unprimed)";
    else if (!m_config.useFixedRangeTable) rejectReason = "no-fixed-range-table";
    else if (!m_config.useStableDrawSlots) rejectReason = "no-stable-draw-slots";
    else if (!m_config.compactStableDrawCommands) rejectReason = "no-compact-draw";
    else if (snapshot.serial == 0u) rejectReason = "serial-zero";
    if (rejectReason != nullptr) {
        m_lastDirtyStageRejectReason = rejectReason;
        return false;
    }
    m_lastDirtyStageRejectReason = "accepted";

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    using DirtyStageClock = std::chrono::steady_clock;
    const auto dirtyStageStartTime = DirtyStageClock::now();
    const auto dirtyStageElapsedMs = [](
        DirtyStageClock::time_point begin,
        DirtyStageClock::time_point end) -> double {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };

    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> dirtyCoords;
    dirtyCoords.reserve(snapshot.dirtyBricks.size());
    for (const auto& item : snapshot.dirtyBricks) {
        if (item.serial <= snapshot.serial) {
            dirtyCoords.insert(item.coord);
        }
    }
    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> removedCoords;
    removedCoords.reserve(snapshot.removedBricks.size());
    for (const auto& item : snapshot.removedBricks) {
        if (item.serial <= snapshot.serial) {
            removedCoords.insert(item.coord);
        }
    }
    std::unordered_map<
        Simulation::BrickCoord,
        const Simulation::SparseSurfaceDrawBatch*,
        Simulation::BrickCoordHash> dirtyBatches;
    dirtyBatches.reserve(snapshot.drawBatches.size());
    for (const Simulation::SparseSurfaceDrawBatch& batch : snapshot.drawBatches) {
        dirtyBatches.emplace(batch.coord, &batch);
    }
    std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> zeroFaceDirtyCoords;
    zeroFaceDirtyCoords.reserve(snapshot.dirtyBricks.size());
    for (const auto& item : snapshot.dirtyBricks) {
        if (item.serial > snapshot.serial) {
            continue;
        }
        auto batchIt = dirtyBatches.find(item.coord);
        if (batchIt == dirtyBatches.end() ||
            !batchIt->second ||
            batchIt->second->faceCount == 0u ||
            !batchIt->second->faces) {
            zeroFaceDirtyCoords.insert(item.coord);
            removedCoords.insert(item.coord);
        }
    }
    if (dirtyCoords.empty() && removedCoords.empty()) {
        return false;
    }

    bool dirtyMetadataResizeRequired = false;
    for (const auto& item : snapshot.dirtyBricks) {
        if (item.serial > snapshot.serial ||
            removedCoords.find(item.coord) != removedCoords.end()) {
            continue;
        }
        auto batchIt = dirtyBatches.find(item.coord);
        if (batchIt == dirtyBatches.end() || !batchIt->second) {
            continue;
        }
        const Simulation::SparseSurfaceDrawBatch& batch = *batchIt->second;
        Simulation::SparseSurfaceFaceAllocation previousAllocation;
        const bool hadAllocation = m_faceRangeAllocator.TryGet(batch.coord, &previousAllocation);
        if (!hadAllocation || previousAllocation.faceCount != batch.faceCount) {
            dirtyMetadataResizeRequired = true;
            break;
        }
    }
    const uint64_t uploadWriteOffsetBeforeStage = m_uploadWriteOffset;
    const Simulation::SparseSurfaceRangeAllocator allocatorBeforeStage = m_faceRangeAllocator;
    uint64_t writeOffset = AlignUp(m_uploadWriteOffset, 4u);
    uint32_t copiedPayloadFaceCount = 0;
    uint32_t copiedPayloadBrickCount = 0;
    uint32_t patchedPayloadFaceCount = 0;
    uint32_t patchedPayloadBrickCount = 0;
    uint32_t patchedPayloadRegionCount = 0;
    uint32_t deferredPayloadBrickCount = snapshot.deferredDirtyBricks;
    const uint32_t initialDeferredPayloadBrickCount = deferredPayloadBrickCount;
    uint32_t newPayloadBrickCount = 0;
    uint32_t changedRangeSlots = 0;
    uint32_t changedDrawCommands = 0;
    uint32_t changedSurfaceRecords = 0;
    uint32_t changedSurfaceClusters = 0;
    uint32_t allocationChangedBrickCount = 0;
    uint32_t mirrorComparableBrickCount = 0;
    uint32_t cleanMirrorBrickCount = 0;
    uint32_t changedRunCount = 0;
    uint32_t changedRunFaceCount = 0;
    uint32_t fullPayloadCopyBrickCount = 0;
    uint32_t fullPayloadCopyFaceCount = 0;
    uint32_t mirrorUpdateBrickCount = 0;
    uint32_t mirrorUpdateFaceCount = 0;
    const bool stableDrawMetadataPatchSafe =
        m_drawSlotOccupied.size() == m_drawArgsMirror.size();
    const bool incrementalMetadataAddsEnabled =
        m_config.incrementalMetadataAdds &&
        stableDrawMetadataPatchSafe &&
        m_config.useFixedRangeTable &&
        m_config.useStableDrawSlots &&
        m_config.compactStableDrawCommands;
    uint32_t incrementalDrawArgsSize = static_cast<uint32_t>(m_drawArgsMirror.size());
    uint32_t incrementalDrawSlotOccupiedSize = static_cast<uint32_t>(m_drawSlotOccupied.size());
    uint32_t incrementalSurfaceRecordSize = static_cast<uint32_t>(m_surfaceRecordMirror.size());
    uint32_t incrementalSurfaceClusterSize = static_cast<uint32_t>(m_surfaceClusterMirror.size());
    bool incrementalDrawSlotStateInitialized = false;
    std::vector<uint32_t> incrementalFreeDrawSlots;

    auto ensureIncrementalDrawSlotState = [&]() {
        if (incrementalDrawSlotStateInitialized) {
            return;
        }
        incrementalFreeDrawSlots = m_freeDrawSlots;
        incrementalDrawSlotStateInitialized = true;
    };

    bool metadataMirrorsInitialized = false;
    std::vector<Simulation::SparseSurfaceBrickRange> nextRangeMirror;
    std::vector<Simulation::SparseSurfaceDrawArgs> nextDrawArgsMirror;
    std::vector<Simulation::SparseSurfaceRecord> nextSurfaceRecordMirror;
    std::vector<Simulation::SparseSurfaceClusterRecord> nextSurfaceClusterMirror;
    std::unordered_map<Simulation::BrickCoord, uint32_t, Simulation::BrickCoordHash> nextDrawSlotByCoord;
    std::vector<uint8_t> nextDrawSlotOccupied;
    std::vector<uint32_t> nextFreeDrawSlots;

    SparseSurfaceUploadTicket ticket;
    ticket.valid = true;
    ticket.ringSlot = m_activeUploadSlot;
    ticket.serial = snapshot.serial;
    ticket.hasUploadWriteOffsetRollback = true;
    ticket.uploadWriteOffsetBeforeStage = uploadWriteOffsetBeforeStage;
    ticket.hasRangeAllocatorRollback = true;
    ticket.rangeAllocatorBeforeStage = allocatorBeforeStage;
    ticket.incrementalMetadataPatches = false;

    bool allocatorStatsBatchActive = false;
    auto beginAllocatorStatsBatch = [&]() {
        if (!allocatorStatsBatchActive) {
            m_faceRangeAllocator.BeginStatsRefreshBatch();
            allocatorStatsBatchActive = true;
        }
    };
    auto endAllocatorStatsBatch = [&]() {
        if (allocatorStatsBatchActive) {
            m_faceRangeAllocator.EndStatsRefreshBatch();
            allocatorStatsBatchActive = false;
        }
    };

    auto failPayloadOnlyStage = [&]() -> bool {
        endAllocatorStatsBatch();
        m_uploadWriteOffset = uploadWriteOffsetBeforeStage;
        m_faceRangeAllocator = allocatorBeforeStage;
        return false;
    };

    auto ensureMetadataMirrors = [&]() {
        if (metadataMirrorsInitialized) {
            return;
        }
        nextRangeMirror = m_rangeMirror;
        nextDrawArgsMirror = m_drawArgsMirror;
        nextSurfaceRecordMirror = m_surfaceRecordMirror;
        nextSurfaceClusterMirror = m_surfaceClusterMirror;
        nextDrawSlotByCoord = m_drawSlotByCoord;
        nextDrawSlotOccupied = m_drawSlotOccupied;
        nextFreeDrawSlots = m_freeDrawSlots;
        metadataMirrorsInitialized = true;
    };

    auto appendBufferCopy = [&](
        const void* source,
        uint64_t byteCount,
        uint64_t destOffset,
        std::vector<SparseSurfaceBufferCopyRegion>& outRegions) -> bool
    {
        if (byteCount == 0u) {
            return true;
        }
        uint64_t uploadOffset = 0;
        uint64_t endOffset = 0;
        if (!source ||
            !AppendAlignedUploadRange(writeOffset, byteCount, 4u, &uploadOffset, &endOffset) ||
            endOffset > upload.GetSize()) {
            return false;
        }
        std::memcpy(mapped + uploadOffset, source, static_cast<size_t>(byteCount));
        outRegions.push_back({uploadOffset, destOffset, byteCount});
        writeOffset = endOffset;
        return true;
    };

    auto appendRangePatch = [&](
        uint32_t index,
        const Simulation::SparseSurfaceBrickRange& value) -> bool {
        if (!appendBufferCopy(
                &value,
                sizeof(Simulation::SparseSurfaceBrickRange),
                static_cast<uint64_t>(index) * sizeof(Simulation::SparseSurfaceBrickRange),
                ticket.rangeCopyRegions)) {
            return false;
        }
        ticket.rangeMirrorPatches.push_back({index, value});
        return true;
    };

    auto appendDrawPatch = [&](
        uint32_t index,
        const Simulation::SparseSurfaceDrawArgs& value) -> bool {
        if (!appendBufferCopy(
                &value,
                sizeof(Simulation::SparseSurfaceDrawArgs),
                static_cast<uint64_t>(index) * sizeof(Simulation::SparseSurfaceDrawArgs),
                ticket.drawArgsCopyRegions)) {
            return false;
        }
        ticket.drawArgsMirrorPatches.push_back({index, value});
        return true;
    };

    auto appendRecordPatch = [&](
        uint32_t index,
        const Simulation::SparseSurfaceRecord& value) -> bool {
        if (!appendBufferCopy(
                &value,
                sizeof(Simulation::SparseSurfaceRecord),
                static_cast<uint64_t>(index) * sizeof(Simulation::SparseSurfaceRecord),
                ticket.surfaceRecordCopyRegions)) {
            return false;
        }
        ticket.surfaceRecordMirrorPatches.push_back({index, value});
        return true;
    };

    auto appendClusterPatch = [&](
        uint32_t index,
        const Simulation::SparseSurfaceClusterRecord& value) -> bool {
        if (!appendBufferCopy(
                &value,
                sizeof(Simulation::SparseSurfaceClusterRecord),
                static_cast<uint64_t>(index) * sizeof(Simulation::SparseSurfaceClusterRecord),
                ticket.surfaceClusterCopyRegions)) {
            return false;
        }
        ticket.surfaceClusterMirrorPatches.push_back({index, value});
        return true;
    };

    auto allocateIncrementalDrawSlot = [&](
        const Simulation::BrickCoord& coord,
        uint32_t* outSlot) -> bool {
        if (!outSlot) {
            return false;
        }
        ensureIncrementalDrawSlotState();
        uint32_t slot = UINT32_MAX;
        if (!incrementalFreeDrawSlots.empty()) {
            slot = incrementalFreeDrawSlots.back();
            incrementalFreeDrawSlots.pop_back();
            if (slot >= m_config.maxDrawCommands) {
                return false;
            }
        } else {
            if (incrementalDrawSlotOccupiedSize >= m_config.maxDrawCommands) {
                return false;
            }
            slot = incrementalDrawSlotOccupiedSize++;
        }
        incrementalDrawArgsSize = std::max(incrementalDrawArgsSize, slot + 1u);
        incrementalDrawSlotOccupiedSize = std::max(incrementalDrawSlotOccupiedSize, slot + 1u);
        ticket.drawSlotAssignments.push_back({coord, slot});
        ticket.drawArgsMirrorSizeAfterPatch =
            std::max(ticket.drawArgsMirrorSizeAfterPatch, incrementalDrawArgsSize);
        ticket.drawSlotOccupiedSizeAfterPatch =
            std::max(ticket.drawSlotOccupiedSizeAfterPatch, incrementalDrawSlotOccupiedSize);
        *outSlot = slot;
        return true;
    };

    auto findRangeSlotIn = [&](
        const std::vector<Simulation::SparseSurfaceBrickRange>& rangeMirror,
        const Simulation::BrickCoord& coord,
        uint32_t* outSlot) -> bool {
        if (!outSlot ||
            rangeMirror.empty() ||
            (rangeMirror.size() & (rangeMirror.size() - 1u)) != 0u) {
            return false;
        }
        const uint32_t mask = static_cast<uint32_t>(rangeMirror.size() - 1u);
        uint32_t slot = Simulation::HashBrickCoord32(coord) & mask;
        uint32_t firstTombstone = UINT32_MAX;
        for (uint32_t probe = 0; probe < rangeMirror.size(); ++probe) {
            const Simulation::SparseSurfaceBrickRange& entry = rangeMirror[slot];
            if (entry.flags == 0u || entry.coord == coord) {
                *outSlot = entry.flags == 0u && firstTombstone != UINT32_MAX
                    ? firstTombstone
                    : slot;
                return true;
            }
            if (firstTombstone == UINT32_MAX && IsSparseSurfaceRangeTombstone(entry)) {
                firstTombstone = slot;
            }
            slot = (slot + 1u) & mask;
        }
        if (firstTombstone != UINT32_MAX) {
            *outSlot = firstTombstone;
            return true;
        }
        return false;
    };
    auto findRangeSlot = [&](const Simulation::BrickCoord& coord, uint32_t* outSlot) -> bool {
        return findRangeSlotIn(nextRangeMirror, coord, outSlot);
    };

    auto buildDrawArgs = [](const Simulation::SparseSurfaceFaceAllocation& allocation) {
        Simulation::SparseSurfaceDrawArgs args;
        args.indexCountPerInstance = allocation.faceCount * 6u;
        args.instanceCount = 1u;
        args.startIndexLocation = allocation.firstFace * 6u;
        args.baseVertexLocation = 0;
        args.startInstanceLocation = 0u;
        return args;
    };

    auto buildRecord = [&](const Simulation::BrickCoord& coord,
                           const Simulation::SparseSurfaceFaceAllocation& allocation,
                           uint32_t directionMask) {
        Simulation::SparseSurfaceRecord record;
        record.coord = coord;
        record.firstFace = allocation.firstFace;
        record.faceCount = allocation.faceCount;
        record.flags = Simulation::SparseSurfacePackRecordFlags(
            Simulation::kSparseSurfaceRangeValid,
            directionMask);
        record.generation = snapshot.serial;
        SetSurfaceRecordBoundsFromBrickCoord(coord, record);
        return record;
    };

    auto buildSingleRecordCluster = [](uint32_t recordIndex, const Simulation::SparseSurfaceRecord& record) {
        Simulation::SparseSurfaceClusterRecord cluster;
        cluster.minX = record.minX;
        cluster.minY = record.minY;
        cluster.minZ = record.minZ;
        cluster.maxX = record.maxX;
        cluster.maxY = record.maxY;
        cluster.maxZ = record.maxZ;
        cluster.firstRecord = recordIndex;
        cluster.recordCount = 1u;
        cluster.faceCount = record.faceCount;
        cluster.flags = record.flags;
        return cluster;
    };

    auto canPatchExistingAllocationMetadata = [&](
        const Simulation::BrickCoord& coord,
        uint32_t faceCount) -> bool {
        if (!stableDrawMetadataPatchSafe || faceCount == 0u) {
            return false;
        }
        Simulation::SparseSurfaceFaceAllocation previousAllocation;
        if (!m_faceRangeAllocator.TryGet(coord, &previousAllocation) ||
            previousAllocation.faceCount == faceCount) {
            return false;
        }
        uint32_t rangeSlot = 0;
        if (!findRangeSlotIn(m_rangeMirror, coord, &rangeSlot) ||
            rangeSlot >= m_rangeMirror.size() ||
            m_rangeMirror[rangeSlot].flags == 0u ||
            m_rangeMirror[rangeSlot].coord != coord) {
            return false;
        }
        auto drawSlotIt = m_drawSlotByCoord.find(coord);
        if (drawSlotIt == m_drawSlotByCoord.end() ||
            drawSlotIt->second >= m_drawArgsMirror.size()) {
            return false;
        }
        auto recordIt = m_surfaceRecordIndexByCoord.find(coord);
        if (recordIt == m_surfaceRecordIndexByCoord.end() ||
            recordIt->second >= m_surfaceRecordMirror.size()) {
            return false;
        }
        const Simulation::SparseSurfaceRecord& record =
            m_surfaceRecordMirror[recordIt->second];
        if (record.coord != coord ||
            record.faceCount == 0u ||
            record.flags == 0u ||
            recordIt->second >= m_surfaceRecordClusterIndex.size()) {
            return false;
        }
        const uint32_t clusterIndex = m_surfaceRecordClusterIndex[recordIt->second];
        return clusterIndex != UINT32_MAX &&
            clusterIndex < m_surfaceClusterMirror.size();
    };

    bool dirtyMetadataResizePatchable = true;
    if (dirtyMetadataResizeRequired) {
        for (const auto& item : snapshot.dirtyBricks) {
            if (item.serial > snapshot.serial ||
                removedCoords.find(item.coord) != removedCoords.end()) {
                continue;
            }
            auto batchIt = dirtyBatches.find(item.coord);
            if (batchIt == dirtyBatches.end() || !batchIt->second) {
                dirtyMetadataResizePatchable = false;
                break;
            }
            const Simulation::SparseSurfaceDrawBatch& batch = *batchIt->second;
            Simulation::SparseSurfaceFaceAllocation previousAllocation;
            const bool hadAllocation =
                m_faceRangeAllocator.TryGet(batch.coord, &previousAllocation);
            if (!hadAllocation) {
                if (!incrementalMetadataAddsEnabled) {
                    dirtyMetadataResizePatchable = false;
                    break;
                }
                continue;
            }
            if (previousAllocation.faceCount != batch.faceCount &&
                !canPatchExistingAllocationMetadata(batch.coord, batch.faceCount)) {
                dirtyMetadataResizePatchable = false;
                break;
            }
        }
    }
    const bool useIncrementalMetadataPatches =
        (!removedCoords.empty() ||
         (dirtyMetadataResizeRequired && dirtyMetadataResizePatchable)) &&
        (!dirtyMetadataResizeRequired || dirtyMetadataResizePatchable);
    ticket.incrementalMetadataPatches = useIncrementalMetadataPatches;

    auto stageFaceCopy = [&](
        const Simulation::SparseSurfaceFaceAllocation& allocation,
        const Simulation::SparseSurfaceFace* sourceFaces,
        uint32_t sourceFirstFace,
        uint32_t destFirstFace,
        uint32_t faceCount) -> bool
    {
        if (faceCount == 0u) {
            return true;
        }
        const uint64_t faceBytes =
            static_cast<uint64_t>(faceCount) * sizeof(Simulation::SparseSurfaceFace);
        uint64_t uploadOffset = 0;
        uint64_t endOffset = 0;
        const uint64_t sourceEnd =
            static_cast<uint64_t>(sourceFirstFace) + static_cast<uint64_t>(faceCount);
        const uint64_t destEnd =
            static_cast<uint64_t>(destFirstFace) + static_cast<uint64_t>(faceCount);
        const uint64_t allocationEnd =
            static_cast<uint64_t>(allocation.firstFace) + static_cast<uint64_t>(allocation.faceCount);
        if (!AppendAlignedUploadRange(writeOffset, faceBytes, 4u, &uploadOffset, &endOffset) ||
            endOffset > upload.GetSize() ||
            !sourceFaces ||
            sourceEnd > allocation.faceCount ||
            destFirstFace < allocation.firstFace ||
            destEnd > allocationEnd) {
            return false;
        }
        std::memcpy(
            mapped + uploadOffset,
            sourceFaces + sourceFirstFace,
            static_cast<size_t>(faceBytes));
        ticket.faceCopyRegions.push_back({
            uploadOffset,
            destFirstFace,
            faceCount
        });
        writeOffset = endOffset;
        return true;
    };

    std::unordered_set<uint32_t> clustersNeedingPatch;
    std::unordered_map<uint32_t, Simulation::SparseSurfaceRecord> incrementalRecordOverrides;
    auto getIncrementalRecord = [&](
        uint32_t recordIndex) -> const Simulation::SparseSurfaceRecord* {
        auto overrideIt = incrementalRecordOverrides.find(recordIndex);
        if (overrideIt != incrementalRecordOverrides.end()) {
            return &overrideIt->second;
        }
        if (recordIndex >= m_surfaceRecordMirror.size()) {
            return nullptr;
        }
        return &m_surfaceRecordMirror[recordIndex];
    };
    auto appendIncrementalClusterPatch = [&](uint32_t clusterIndex) -> bool {
        if (clusterIndex >= m_surfaceClusterMirror.size()) {
            return true;
        }
        Simulation::SparseSurfaceClusterRecord cluster = m_surfaceClusterMirror[clusterIndex];
        uint32_t faceCount = 0;
        uint32_t flags = 0;
        bool hasValidRecord = false;
        int32_t minX = 0;
        int32_t minY = 0;
        int32_t minZ = 0;
        int32_t maxX = 0;
        int32_t maxY = 0;
        int32_t maxZ = 0;
        const uint64_t clusterEnd =
            static_cast<uint64_t>(cluster.firstRecord) +
            static_cast<uint64_t>(cluster.recordCount);
        const uint32_t recordEnd = static_cast<uint32_t>(
            std::min<uint64_t>(clusterEnd, m_surfaceRecordMirror.size()));
        for (uint32_t recordIndex = cluster.firstRecord; recordIndex < recordEnd; ++recordIndex) {
            const Simulation::SparseSurfaceRecord* record = getIncrementalRecord(recordIndex);
            if (!record || record->faceCount == 0u || record->flags == 0u) {
                continue;
            }
            faceCount += record->faceCount;
            flags |= record->flags;
            if (!hasValidRecord) {
                minX = record->minX;
                minY = record->minY;
                minZ = record->minZ;
                maxX = record->maxX;
                maxY = record->maxY;
                maxZ = record->maxZ;
                hasValidRecord = true;
            } else {
                minX = std::min(minX, record->minX);
                minY = std::min(minY, record->minY);
                minZ = std::min(minZ, record->minZ);
                maxX = std::max(maxX, record->maxX);
                maxY = std::max(maxY, record->maxY);
                maxZ = std::max(maxZ, record->maxZ);
            }
        }
        cluster.faceCount = faceCount;
        cluster.flags = hasValidRecord ? flags : 0u;
        if (hasValidRecord) {
            cluster.minX = minX;
            cluster.minY = minY;
            cluster.minZ = minZ;
            cluster.maxX = maxX;
            cluster.maxY = maxY;
            cluster.maxZ = maxZ;
        } else {
            cluster.minX = cluster.minY = cluster.minZ = 0;
            cluster.maxX = cluster.maxY = cluster.maxZ = 0;
        }
        if (!appendClusterPatch(clusterIndex, cluster)) {
            return false;
        }
        ++changedSurfaceClusters;
        return true;
    };
    auto appendFullMirrorClusterPatch = [&](uint32_t clusterIndex) -> bool {
        if (clusterIndex >= nextSurfaceClusterMirror.size()) {
            return true;
        }
        Simulation::SparseSurfaceClusterRecord& cluster = nextSurfaceClusterMirror[clusterIndex];
        uint32_t faceCount = 0;
        uint32_t flags = 0;
        bool hasValidRecord = false;
        int32_t minX = 0;
        int32_t minY = 0;
        int32_t minZ = 0;
        int32_t maxX = 0;
        int32_t maxY = 0;
        int32_t maxZ = 0;
        const uint64_t clusterEnd =
            static_cast<uint64_t>(cluster.firstRecord) +
            static_cast<uint64_t>(cluster.recordCount);
        const uint32_t recordEnd = static_cast<uint32_t>(
            std::min<uint64_t>(clusterEnd, nextSurfaceRecordMirror.size()));
        for (uint32_t recordIndex = cluster.firstRecord; recordIndex < recordEnd; ++recordIndex) {
            const Simulation::SparseSurfaceRecord& record = nextSurfaceRecordMirror[recordIndex];
            if (record.faceCount == 0u || record.flags == 0u) {
                continue;
            }
            faceCount += record.faceCount;
            flags |= record.flags;
            if (!hasValidRecord) {
                minX = record.minX;
                minY = record.minY;
                minZ = record.minZ;
                maxX = record.maxX;
                maxY = record.maxY;
                maxZ = record.maxZ;
                hasValidRecord = true;
            } else {
                minX = std::min(minX, record.minX);
                minY = std::min(minY, record.minY);
                minZ = std::min(minZ, record.minZ);
                maxX = std::max(maxX, record.maxX);
                maxY = std::max(maxY, record.maxY);
                maxZ = std::max(maxZ, record.maxZ);
            }
        }
        cluster.faceCount = faceCount;
        cluster.flags = hasValidRecord ? flags : 0u;
        if (hasValidRecord) {
            cluster.minX = minX;
            cluster.minY = minY;
            cluster.minZ = minZ;
            cluster.maxX = maxX;
            cluster.maxY = maxY;
            cluster.maxZ = maxZ;
        } else {
            cluster.minX = cluster.minY = cluster.minZ = 0;
            cluster.maxX = cluster.maxY = cluster.maxZ = 0;
        }
        if (!appendBufferCopy(
                &cluster,
                sizeof(Simulation::SparseSurfaceClusterRecord),
                static_cast<uint64_t>(clusterIndex) * sizeof(Simulation::SparseSurfaceClusterRecord),
                ticket.surfaceClusterCopyRegions)) {
            return false;
        }
        ++changedSurfaceClusters;
        return true;
    };

    const auto dirtyStageAfterSetupTime = DirtyStageClock::now();
    beginAllocatorStatsBatch();
    if (!removedCoords.empty() && useIncrementalMetadataPatches) {
        ticket.removedBricks.reserve(removedCoords.size());
        incrementalRecordOverrides.reserve(removedCoords.size() + snapshot.dirtyBricks.size());
        for (const Simulation::BrickCoord& coord : removedCoords) {
            ticket.removedBricks.push_back(coord);
            if (zeroFaceDirtyCoords.find(coord) != zeroFaceDirtyCoords.end()) {
                ticket.uploadedPayloadBricks.push_back(coord);
            }
            m_faceRangeAllocator.Free(coord);

            uint32_t rangeSlot = 0;
            if (findRangeSlotIn(m_rangeMirror, coord, &rangeSlot) &&
                rangeSlot < m_rangeMirror.size() &&
                m_rangeMirror[rangeSlot].flags != 0u &&
                m_rangeMirror[rangeSlot].coord == coord) {
                const Simulation::SparseSurfaceBrickRange tombstoneRange =
                    MakeSparseSurfaceRangeTombstone(coord);
                if (!appendRangePatch(rangeSlot, tombstoneRange)) {
                    return failPayloadOnlyStage();
                }
                ++changedRangeSlots;
            }

            auto drawSlotIt = m_drawSlotByCoord.find(coord);
            if (drawSlotIt != m_drawSlotByCoord.end()) {
                const uint32_t drawSlot = drawSlotIt->second;
                ticket.drawSlotRetires.push_back({coord, drawSlot});
                if (stableDrawMetadataPatchSafe && drawSlot < m_drawArgsMirror.size()) {
                    const Simulation::SparseSurfaceDrawArgs emptyDraw = {};
                    if (!appendDrawPatch(drawSlot, emptyDraw)) {
                        return failPayloadOnlyStage();
                    }
                    ++changedDrawCommands;
                }
            }

            auto recordIt = m_surfaceRecordIndexByCoord.find(coord);
            if (recordIt != m_surfaceRecordIndexByCoord.end()) {
                const uint32_t recordIndex = recordIt->second;
                if (recordIndex >= m_surfaceRecordMirror.size()) {
                    return failPayloadOnlyStage();
                }
                Simulation::SparseSurfaceRecord record = m_surfaceRecordMirror[recordIndex];
                if (record.coord != coord || record.faceCount == 0u || record.flags == 0u) {
                    return failPayloadOnlyStage();
                }
                record.firstFace = 0u;
                record.faceCount = 0u;
                record.flags = 0u;
                record.generation = snapshot.serial;
                record.minX = record.minY = record.minZ = 0;
                record.maxX = record.maxY = record.maxZ = 0;
                if (!appendRecordPatch(recordIndex, record)) {
                    return failPayloadOnlyStage();
                }
                incrementalRecordOverrides[recordIndex] = record;
                ++changedSurfaceRecords;
                if (recordIndex < m_surfaceRecordClusterIndex.size()) {
                    const uint32_t clusterIndex = m_surfaceRecordClusterIndex[recordIndex];
                    if (clusterIndex != UINT32_MAX) {
                        clustersNeedingPatch.insert(clusterIndex);
                    }
                }
            }
        }

        for (uint32_t clusterIndex : clustersNeedingPatch) {
            if (!appendIncrementalClusterPatch(clusterIndex)) {
                return failPayloadOnlyStage();
            }
        }
    } else if (!removedCoords.empty()) {
        ensureMetadataMirrors();
        ticket.removedBricks.reserve(removedCoords.size());
        for (const Simulation::BrickCoord& coord : removedCoords) {
            ticket.removedBricks.push_back(coord);
            if (zeroFaceDirtyCoords.find(coord) != zeroFaceDirtyCoords.end()) {
                ticket.uploadedPayloadBricks.push_back(coord);
            }
            m_faceRangeAllocator.Free(coord);

            uint32_t rangeSlot = 0;
            if (findRangeSlot(coord, &rangeSlot) &&
                rangeSlot < nextRangeMirror.size() &&
                nextRangeMirror[rangeSlot].flags != 0u &&
                nextRangeMirror[rangeSlot].coord == coord) {
                nextRangeMirror[rangeSlot] = MakeSparseSurfaceRangeTombstone(coord);
                if (!appendBufferCopy(
                        &nextRangeMirror[rangeSlot],
                        sizeof(Simulation::SparseSurfaceBrickRange),
                        static_cast<uint64_t>(rangeSlot) * sizeof(Simulation::SparseSurfaceBrickRange),
                        ticket.rangeCopyRegions)) {
                    return failPayloadOnlyStage();
                }
                ++changedRangeSlots;
            }

            auto drawSlotIt = nextDrawSlotByCoord.find(coord);
            if (drawSlotIt != nextDrawSlotByCoord.end()) {
                const uint32_t drawSlot = drawSlotIt->second;
                nextDrawSlotByCoord.erase(drawSlotIt);
                if (drawSlot < nextDrawSlotOccupied.size() && nextDrawSlotOccupied[drawSlot] != 0u) {
                    nextDrawSlotOccupied[drawSlot] = 0u;
                    nextFreeDrawSlots.push_back(drawSlot);
                }
                if (stableDrawMetadataPatchSafe && drawSlot < nextDrawArgsMirror.size()) {
                    nextDrawArgsMirror[drawSlot] = {};
                    if (!appendBufferCopy(
                            &nextDrawArgsMirror[drawSlot],
                            sizeof(Simulation::SparseSurfaceDrawArgs),
                            static_cast<uint64_t>(drawSlot) * sizeof(Simulation::SparseSurfaceDrawArgs),
                            ticket.drawArgsCopyRegions)) {
                        return failPayloadOnlyStage();
                    }
                    ++changedDrawCommands;
                }
            }

            auto recordIt = m_surfaceRecordIndexByCoord.find(coord);
            if (recordIt != m_surfaceRecordIndexByCoord.end()) {
                const uint32_t recordIndex = recordIt->second;
                if (recordIndex >= nextSurfaceRecordMirror.size()) {
                    return failPayloadOnlyStage();
                }
                Simulation::SparseSurfaceRecord& record = nextSurfaceRecordMirror[recordIndex];
                if (record.coord != coord || record.faceCount == 0u || record.flags == 0u) {
                    return failPayloadOnlyStage();
                }
                record.firstFace = 0u;
                record.faceCount = 0u;
                record.flags = 0u;
                record.generation = snapshot.serial;
                record.minX = record.minY = record.minZ = 0;
                record.maxX = record.maxY = record.maxZ = 0;
                if (!appendBufferCopy(
                        &record,
                        sizeof(Simulation::SparseSurfaceRecord),
                        static_cast<uint64_t>(recordIndex) * sizeof(Simulation::SparseSurfaceRecord),
                        ticket.surfaceRecordCopyRegions)) {
                    return failPayloadOnlyStage();
                }
                ++changedSurfaceRecords;
                if (recordIndex < m_surfaceRecordClusterIndex.size()) {
                    const uint32_t clusterIndex = m_surfaceRecordClusterIndex[recordIndex];
                    if (clusterIndex != UINT32_MAX) {
                        clustersNeedingPatch.insert(clusterIndex);
                    }
                }
            }
        }

        for (uint32_t clusterIndex : clustersNeedingPatch) {
            if (clusterIndex >= nextSurfaceClusterMirror.size()) {
                continue;
            }
            Simulation::SparseSurfaceClusterRecord& cluster = nextSurfaceClusterMirror[clusterIndex];
            uint32_t faceCount = 0;
            uint32_t flags = 0;
            bool hasValidRecord = false;
            int32_t minX = 0;
            int32_t minY = 0;
            int32_t minZ = 0;
            int32_t maxX = 0;
            int32_t maxY = 0;
            int32_t maxZ = 0;
            const uint64_t clusterEnd =
                static_cast<uint64_t>(cluster.firstRecord) +
                static_cast<uint64_t>(cluster.recordCount);
            const uint32_t recordEnd = static_cast<uint32_t>(
                std::min<uint64_t>(clusterEnd, nextSurfaceRecordMirror.size()));
            for (uint32_t recordIndex = cluster.firstRecord; recordIndex < recordEnd; ++recordIndex) {
                const Simulation::SparseSurfaceRecord& record = nextSurfaceRecordMirror[recordIndex];
                if (record.faceCount == 0u || record.flags == 0u) {
                    continue;
                }
                faceCount += record.faceCount;
                flags |= record.flags;
                if (!hasValidRecord) {
                    minX = record.minX;
                    minY = record.minY;
                    minZ = record.minZ;
                    maxX = record.maxX;
                    maxY = record.maxY;
                    maxZ = record.maxZ;
                    hasValidRecord = true;
                } else {
                    minX = std::min(minX, record.minX);
                    minY = std::min(minY, record.minY);
                    minZ = std::min(minZ, record.minZ);
                    maxX = std::max(maxX, record.maxX);
                    maxY = std::max(maxY, record.maxY);
                    maxZ = std::max(maxZ, record.maxZ);
                }
            }
            cluster.faceCount = faceCount;
            cluster.flags = hasValidRecord ? flags : 0u;
            if (hasValidRecord) {
                cluster.minX = minX;
                cluster.minY = minY;
                cluster.minZ = minZ;
                cluster.maxX = maxX;
                cluster.maxY = maxY;
                cluster.maxZ = maxZ;
            } else {
                cluster.minX = cluster.minY = cluster.minZ = 0;
                cluster.maxX = cluster.maxY = cluster.maxZ = 0;
            }
            if (!appendBufferCopy(
                    &cluster,
                    sizeof(Simulation::SparseSurfaceClusterRecord),
                    static_cast<uint64_t>(clusterIndex) * sizeof(Simulation::SparseSurfaceClusterRecord),
                    ticket.surfaceClusterCopyRegions)) {
                return failPayloadOnlyStage();
            }
            ++changedSurfaceClusters;
        }
    }

    const auto dirtyStageAfterRemovedTime = DirtyStageClock::now();
    for (const auto& item : snapshot.dirtyBricks) {
        if (item.serial > snapshot.serial) {
            continue;
        }
        if (removedCoords.find(item.coord) != removedCoords.end()) {
            continue;
        }
        auto batchIt = dirtyBatches.find(item.coord);
        if (batchIt == dirtyBatches.end() || !batchIt->second) {
            return failPayloadOnlyStage();
        }
        const Simulation::SparseSurfaceDrawBatch& batch = *batchIt->second;
        if (batch.faceCount == 0u || !batch.faces) {
            return failPayloadOnlyStage();
        }

        Simulation::SparseSurfaceFaceAllocation previousAllocation;
        const bool hadAllocation = m_faceRangeAllocator.TryGet(batch.coord, &previousAllocation);
        const bool payloadResident = m_payloadResidentCoords.find(batch.coord) != m_payloadResidentCoords.end();
        const bool allocationChanged = !hadAllocation || previousAllocation.faceCount != batch.faceCount;
        if (allocationChanged) {
            ++allocationChangedBrickCount;
        }
        if (allocationChanged && !stableDrawMetadataPatchSafe) {
            ++deferredPayloadBrickCount;
            continue;
        }

        auto mirrorIt = m_payloadFaceMirrorByCoord.find(batch.coord);
        const bool mirrorUsable =
            mirrorIt != m_payloadFaceMirrorByCoord.end() &&
            mirrorIt->second.size() == batch.faceCount;
        const auto runs = Simulation::BuildSparseSurfaceChangedFaceRuns(
            mirrorUsable ? batch.faces : nullptr,
            mirrorUsable ? mirrorIt->second.data() : nullptr,
            batch.faceCount);
        if (mirrorUsable) {
            ++mirrorComparableBrickCount;
        }
        uint32_t runFaceCount = 0;
        for (const auto& run : runs) {
            runFaceCount += run.faceCount;
        }
        changedRunCount += static_cast<uint32_t>(runs.size());
        changedRunFaceCount += runFaceCount;
        const uint32_t remainingRegionBudget =
            m_config.maxPayloadCopyRegionsPerFrame == 0u
                ? UINT_MAX
                : m_config.maxPayloadCopyRegionsPerFrame -
                    std::min<uint32_t>(
                        m_config.maxPayloadCopyRegionsPerFrame,
                        static_cast<uint32_t>(ticket.faceCopyRegions.size()));
        const uint32_t remainingFaceBudget =
            m_config.maxPayloadCopyFacesPerFrame == 0u
                ? UINT_MAX
                : m_config.maxPayloadCopyFacesPerFrame - copiedPayloadFaceCount;
        const bool canPatch =
            mirrorUsable &&
            runs.size() <= remainingRegionBudget &&
            runFaceCount <= remainingFaceBudget;
        if (hadAllocation && payloadResident && mirrorUsable && runs.empty()) {
            ticket.uploadedPayloadBricks.push_back(batch.coord);
            ++cleanMirrorBrickCount;
            continue;
        }

        if (allocationChanged) {
            const bool regionBudgetAvailable =
                m_config.maxPayloadCopyRegionsPerFrame == 0u ||
                copiedPayloadBrickCount < m_config.maxPayloadCopyRegionsPerFrame;
            const bool faceBudgetAvailable =
                m_config.maxPayloadCopyFacesPerFrame == 0u ||
                copiedPayloadFaceCount + batch.faceCount <= m_config.maxPayloadCopyFacesPerFrame ||
                copiedPayloadBrickCount == 0u;
            if (!regionBudgetAvailable || !faceBudgetAvailable) {
                ++deferredPayloadBrickCount;
                continue;
            }
        }

        Simulation::SparseSurfaceFaceAllocation allocation = previousAllocation;
        if (allocationChanged) {
            if (!m_faceRangeAllocator.AllocateOrResize(batch.coord, batch.faceCount, &allocation)) {
                return failPayloadOnlyStage();
            }
        }

        if (hadAllocation && payloadResident && canPatch) {
            for (const auto& run : runs) {
                uint64_t destFirstFace = 0;
                if (!AddUint64(allocation.firstFace, run.firstFace, &destFirstFace) ||
                    destFirstFace > std::numeric_limits<uint32_t>::max()) {
                    return failPayloadOnlyStage();
                }
                if (!stageFaceCopy(
                        allocation,
                        batch.faces,
                        run.firstFace,
                        static_cast<uint32_t>(destFirstFace),
                        run.faceCount)) {
                    return failPayloadOnlyStage();
                }
            }
            copiedPayloadFaceCount += runFaceCount;
            ++copiedPayloadBrickCount;
            patchedPayloadFaceCount += runFaceCount;
            ++patchedPayloadBrickCount;
            patchedPayloadRegionCount += static_cast<uint32_t>(runs.size());
        } else {
            const bool regionBudgetAvailable =
                m_config.maxPayloadCopyRegionsPerFrame == 0u ||
                copiedPayloadBrickCount < m_config.maxPayloadCopyRegionsPerFrame;
            const bool faceBudgetAvailable =
                m_config.maxPayloadCopyFacesPerFrame == 0u ||
                copiedPayloadFaceCount + batch.faceCount <= m_config.maxPayloadCopyFacesPerFrame ||
                copiedPayloadBrickCount == 0u;
            if (!regionBudgetAvailable || !faceBudgetAvailable) {
                ++deferredPayloadBrickCount;
                continue;
            }
            if (!stageFaceCopy(
                    allocation,
                    batch.faces,
                    0u,
                    allocation.firstFace,
                    batch.faceCount)) {
                return failPayloadOnlyStage();
            }
            copiedPayloadFaceCount += batch.faceCount;
            ++copiedPayloadBrickCount;
            fullPayloadCopyFaceCount += batch.faceCount;
            ++fullPayloadCopyBrickCount;
        }

        if (allocationChanged) {
            const uint32_t directionMask = Simulation::BuildSparseSurfaceDirectionMask(
                batch.faces,
                batch.faceCount);
            Simulation::SparseSurfaceBrickRange range;
            range.coord = batch.coord;
            range.firstFace = allocation.firstFace;
            range.faceCount = allocation.faceCount;
            range.flags = Simulation::SparseSurfacePackRecordFlags(
                Simulation::kSparseSurfaceRangeValid,
                directionMask);

            Simulation::SparseSurfaceRecord record = buildRecord(batch.coord, allocation, directionMask);
            const bool useExistingMetadataPatch =
                ticket.incrementalMetadataPatches &&
                canPatchExistingAllocationMetadata(batch.coord, batch.faceCount);
            const bool useIncrementalNewMetadata =
                ticket.incrementalMetadataPatches &&
                incrementalMetadataAddsEnabled &&
                !useExistingMetadataPatch;
            if (useExistingMetadataPatch) {
                uint32_t rangeSlot = 0;
                if (!findRangeSlotIn(m_rangeMirror, batch.coord, &rangeSlot) ||
                    rangeSlot >= m_rangeMirror.size() ||
                    m_rangeMirror[rangeSlot].coord != batch.coord ||
                    !appendRangePatch(rangeSlot, range)) {
                    return failPayloadOnlyStage();
                }
                ++changedRangeSlots;

                auto drawSlotIt = m_drawSlotByCoord.find(batch.coord);
                if (drawSlotIt == m_drawSlotByCoord.end() ||
                    drawSlotIt->second >= m_drawArgsMirror.size()) {
                    return failPayloadOnlyStage();
                }
                const uint32_t drawSlot = drawSlotIt->second;
                if (!appendDrawPatch(drawSlot, buildDrawArgs(allocation))) {
                    return failPayloadOnlyStage();
                }
                ++changedDrawCommands;

                auto recordIt = m_surfaceRecordIndexByCoord.find(batch.coord);
                if (recordIt == m_surfaceRecordIndexByCoord.end()) {
                    return failPayloadOnlyStage();
                }
                const uint32_t recordIndex = recordIt->second;
                if (recordIndex >= m_surfaceRecordMirror.size()) {
                    return failPayloadOnlyStage();
                }
                if (!appendRecordPatch(recordIndex, record)) {
                    return failPayloadOnlyStage();
                }
                incrementalRecordOverrides[recordIndex] = record;
                ++changedSurfaceRecords;

                if (recordIndex >= m_surfaceRecordClusterIndex.size()) {
                    return failPayloadOnlyStage();
                }
                const uint32_t clusterIndex = m_surfaceRecordClusterIndex[recordIndex];
                if (clusterIndex == UINT32_MAX ||
                    !appendIncrementalClusterPatch(clusterIndex)) {
                    return failPayloadOnlyStage();
                }
            } else if (useIncrementalNewMetadata) {
                uint32_t rangeSlot = 0;
                if (!findRangeSlotIn(m_rangeMirror, batch.coord, &rangeSlot) ||
                    rangeSlot >= m_rangeMirror.size() ||
                    !appendRangePatch(rangeSlot, range)) {
                    return failPayloadOnlyStage();
                }
                ++changedRangeSlots;

                uint32_t drawSlot = UINT32_MAX;
                if (!allocateIncrementalDrawSlot(batch.coord, &drawSlot)) {
                    return failPayloadOnlyStage();
                }
                if (!appendDrawPatch(drawSlot, buildDrawArgs(allocation))) {
                    return failPayloadOnlyStage();
                }
                ++changedDrawCommands;

                if (incrementalSurfaceRecordSize >= m_config.maxDrawCommands ||
                    incrementalSurfaceClusterSize >= m_config.maxDrawCommands) {
                    return failPayloadOnlyStage();
                }
                const uint32_t recordIndex = incrementalSurfaceRecordSize++;
                const uint32_t clusterIndex = incrementalSurfaceClusterSize++;
                if (!appendRecordPatch(recordIndex, record)) {
                    return failPayloadOnlyStage();
                }
                ++changedSurfaceRecords;

                const Simulation::SparseSurfaceClusterRecord cluster =
                    buildSingleRecordCluster(recordIndex, record);
                if (!appendClusterPatch(clusterIndex, cluster)) {
                    return failPayloadOnlyStage();
                }
                ++changedSurfaceClusters;
                ticket.surfaceRecordMirrorSizeAfterPatch =
                    std::max(ticket.surfaceRecordMirrorSizeAfterPatch, incrementalSurfaceRecordSize);
                ticket.surfaceClusterMirrorSizeAfterPatch =
                    std::max(ticket.surfaceClusterMirrorSizeAfterPatch, incrementalSurfaceClusterSize);
                ++newPayloadBrickCount;
            } else {
                ensureMetadataMirrors();
                uint32_t rangeSlot = 0;
                if (!findRangeSlot(batch.coord, &rangeSlot)) {
                    return failPayloadOnlyStage();
                }
                nextRangeMirror[rangeSlot] = range;
                if (!appendBufferCopy(
                        &nextRangeMirror[rangeSlot],
                        sizeof(Simulation::SparseSurfaceBrickRange),
                        static_cast<uint64_t>(rangeSlot) * sizeof(Simulation::SparseSurfaceBrickRange),
                        ticket.rangeCopyRegions)) {
                    return failPayloadOnlyStage();
                }
                ++changedRangeSlots;

                auto slotIt = nextDrawSlotByCoord.find(batch.coord);
                uint32_t drawSlot = UINT32_MAX;
                const bool newDrawSlot = slotIt == nextDrawSlotByCoord.end();
                if (newDrawSlot) {
                    if (!nextFreeDrawSlots.empty()) {
                        drawSlot = nextFreeDrawSlots.back();
                        nextFreeDrawSlots.pop_back();
                        if (drawSlot >= nextDrawSlotOccupied.size()) {
                            return failPayloadOnlyStage();
                        }
                        nextDrawSlotByCoord.emplace(batch.coord, drawSlot);
                        nextDrawSlotOccupied[drawSlot] = 1u;
                        if (drawSlot >= nextDrawArgsMirror.size()) {
                            nextDrawArgsMirror.resize(drawSlot + 1u);
                        }
                        nextDrawArgsMirror[drawSlot] = buildDrawArgs(allocation);
                    } else {
                        if (nextDrawSlotOccupied.size() >= m_config.maxDrawCommands) {
                            return failPayloadOnlyStage();
                        }
                        drawSlot = static_cast<uint32_t>(nextDrawSlotOccupied.size());
                        nextDrawSlotByCoord.emplace(batch.coord, drawSlot);
                        nextDrawSlotOccupied.push_back(1u);
                        nextDrawArgsMirror.push_back(buildDrawArgs(allocation));
                    }
                } else {
                    drawSlot = slotIt->second;
                    if (drawSlot >= nextDrawArgsMirror.size()) {
                        return failPayloadOnlyStage();
                    }
                    nextDrawArgsMirror[drawSlot] = buildDrawArgs(allocation);
                }
                if (!appendBufferCopy(
                        &nextDrawArgsMirror[drawSlot],
                        sizeof(Simulation::SparseSurfaceDrawArgs),
                        static_cast<uint64_t>(drawSlot) * sizeof(Simulation::SparseSurfaceDrawArgs),
                        ticket.drawArgsCopyRegions)) {
                    return failPayloadOnlyStage();
                }
                ++changedDrawCommands;

                uint32_t recordIndex = UINT32_MAX;
                auto recordIt = m_surfaceRecordIndexByCoord.find(batch.coord);
                if (recordIt != m_surfaceRecordIndexByCoord.end()) {
                    recordIndex = recordIt->second;
                }
                const bool newRecord = recordIndex == UINT32_MAX;
                if (newRecord) {
                    if (nextSurfaceRecordMirror.size() >= m_config.maxDrawCommands ||
                        nextSurfaceClusterMirror.size() >= m_config.maxDrawCommands) {
                        return failPayloadOnlyStage();
                    }
                    recordIndex = static_cast<uint32_t>(nextSurfaceRecordMirror.size());
                    nextSurfaceRecordMirror.push_back(record);
                } else {
                    if (recordIndex >= nextSurfaceRecordMirror.size()) {
                        return failPayloadOnlyStage();
                    }
                    nextSurfaceRecordMirror[recordIndex] = record;
                }
                if (!appendBufferCopy(
                        &nextSurfaceRecordMirror[recordIndex],
                        sizeof(Simulation::SparseSurfaceRecord),
                        static_cast<uint64_t>(recordIndex) * sizeof(Simulation::SparseSurfaceRecord),
                        ticket.surfaceRecordCopyRegions)) {
                    return failPayloadOnlyStage();
                }
                ++changedSurfaceRecords;

                if (newRecord) {
                    const Simulation::SparseSurfaceClusterRecord cluster =
                        buildSingleRecordCluster(recordIndex, record);
                    const uint32_t clusterIndex = static_cast<uint32_t>(nextSurfaceClusterMirror.size());
                    nextSurfaceClusterMirror.push_back(cluster);
                    if (!appendBufferCopy(
                            &nextSurfaceClusterMirror[clusterIndex],
                            sizeof(Simulation::SparseSurfaceClusterRecord),
                            static_cast<uint64_t>(clusterIndex) * sizeof(Simulation::SparseSurfaceClusterRecord),
                            ticket.surfaceClusterCopyRegions)) {
                        return failPayloadOnlyStage();
                    }
                    ++changedSurfaceClusters;
                } else if (recordIndex < m_surfaceRecordClusterIndex.size()) {
                    const uint32_t clusterIndex = m_surfaceRecordClusterIndex[recordIndex];
                    if (clusterIndex != UINT32_MAX &&
                        !appendFullMirrorClusterPatch(clusterIndex)) {
                        return failPayloadOnlyStage();
                    }
                }
                if (newDrawSlot || newRecord) {
                    ++newPayloadBrickCount;
                }
            }
        }

        ticket.uploadedPayloadBricks.push_back(batch.coord);
        SparseSurfacePayloadMirrorUpdate update;
        update.coord = batch.coord;
        update.faces.assign(batch.faces, batch.faces + batch.faceCount);
        ++mirrorUpdateBrickCount;
        mirrorUpdateFaceCount += batch.faceCount;
        ticket.payloadMirrorUpdates.push_back(std::move(update));
    }

    const auto dirtyStageAfterDirtyLoopTime = DirtyStageClock::now();
    endAllocatorStatsBatch();
    if (ticket.uploadedPayloadBricks.empty() &&
        ticket.removedBricks.empty() &&
        deferredPayloadBrickCount == 0u) {
        return failPayloadOnlyStage();
    }
    if (deferredPayloadBrickCount > initialDeferredPayloadBrickCount &&
        copiedPayloadBrickCount == 0u &&
        patchedPayloadBrickCount == 0u) {
        return failPayloadOnlyStage();
    }

    m_uploadWriteOffset = writeOffset;
    ticket.deferredPayloadBricks = deferredPayloadBrickCount;

    const bool metadataChanged =
        (metadataMirrorsInitialized &&
            (newPayloadBrickCount != 0u ||
             !ticket.removedBricks.empty() ||
             changedRangeSlots != 0u ||
             changedDrawCommands != 0u ||
             changedSurfaceRecords != 0u ||
             changedSurfaceClusters != 0u)) ||
        (ticket.incrementalMetadataPatches &&
            (!ticket.rangeMirrorPatches.empty() ||
             !ticket.drawArgsMirrorPatches.empty() ||
             !ticket.surfaceRecordMirrorPatches.empty() ||
             !ticket.surfaceClusterMirrorPatches.empty() ||
             !ticket.drawSlotRetires.empty()));
    const uint32_t removedPayloadBrickCount = static_cast<uint32_t>(ticket.removedBricks.size());
    const uint32_t metadataRangeCount = metadataChanged
        ? m_stats.uploadedRanges + newPayloadBrickCount -
            std::min<uint32_t>(removedPayloadBrickCount, m_stats.uploadedRanges + newPayloadBrickCount)
        : m_stats.uploadedRanges;
    const uint32_t metadataVisibleBrickCount = metadataChanged
        ? m_stats.uploadedVisibleBricks + newPayloadBrickCount -
            std::min<uint32_t>(
                removedPayloadBrickCount,
                m_stats.uploadedVisibleBricks + newPayloadBrickCount)
        : m_stats.uploadedVisibleBricks;
    const uint32_t metadataCandidateBrickCount = metadataChanged
        ? m_stats.uploadedCandidateBricks + newPayloadBrickCount -
            std::min<uint32_t>(
                removedPayloadBrickCount,
                m_stats.uploadedCandidateBricks + newPayloadBrickCount)
        : m_stats.uploadedCandidateBricks;
    const uint32_t metadataActiveDrawCount = metadataChanged
        ? m_stats.uploadedActiveDrawCommands + newPayloadBrickCount -
            std::min<uint32_t>(
                removedPayloadBrickCount,
                m_stats.uploadedActiveDrawCommands + newPayloadBrickCount)
        : m_stats.uploadedActiveDrawCommands;
    const auto& stagedAllocatorStatsForTicket = m_faceRangeAllocator.GetStats();
    ticket.payloadOnly = !metadataChanged;
    ticket.faceCount = stagedAllocatorStatsForTicket.allocatedCapacity;
    ticket.rangeCount = metadataRangeCount;
    ticket.rangeTableCapacity = metadataChanged && !ticket.incrementalMetadataPatches
        ? static_cast<uint32_t>(nextRangeMirror.size())
        : m_stats.uploadedRangeTableCapacity;
    ticket.drawCommandCount = metadataChanged && !ticket.incrementalMetadataPatches
        ? static_cast<uint32_t>(nextDrawArgsMirror.size())
        : std::max(m_stats.uploadedDrawCommands, ticket.drawArgsMirrorSizeAfterPatch);
    ticket.activeDrawCommandCount = metadataActiveDrawCount;
    ticket.candidateBricks = metadataCandidateBrickCount;
    ticket.visibleBricks = metadataVisibleBrickCount;
    ticket.culledBricks = metadataChanged ? snapshot.culledBricks : m_stats.uploadedCulledBricks;
    if (metadataChanged && !ticket.incrementalMetadataPatches) {
        ticket.rangeMirrorAfterCopy = std::move(nextRangeMirror);
        ticket.drawArgsMirrorAfterCopy = std::move(nextDrawArgsMirror);
        ticket.surfaceRecordMirrorAfterCopy = std::move(nextSurfaceRecordMirror);
        ticket.surfaceClusterMirrorAfterCopy = std::move(nextSurfaceClusterMirror);
        ticket.drawSlotByCoordAfterCopy = std::move(nextDrawSlotByCoord);
        ticket.drawSlotOccupiedAfterCopy = std::move(nextDrawSlotOccupied);
        ticket.freeDrawSlotsAfterCopy = std::move(nextFreeDrawSlots);
    }

    m_stats.stagedFacesLastFrame = ticket.faceCount;
    m_stats.stagedRangesLastFrame = ticket.rangeCount;
    m_stats.stagedRangeTableCapacityLastFrame = ticket.rangeTableCapacity;
    m_stats.stagedDrawCommandsLastFrame = ticket.drawCommandCount;
    m_stats.activeDrawCommandsLastFrame = ticket.activeDrawCommandCount;
    m_stats.stagedSurfaceRecordsLastFrame = metadataChanged
        ? (ticket.incrementalMetadataPatches
            ? std::max(m_stats.uploadedSurfaceRecords, ticket.surfaceRecordMirrorSizeAfterPatch)
            : static_cast<uint32_t>(ticket.surfaceRecordMirrorAfterCopy.size()))
        : m_stats.uploadedSurfaceRecords;
    m_stats.stagedSurfaceClustersLastFrame = metadataChanged
        ? (ticket.incrementalMetadataPatches
            ? std::max(m_stats.uploadedSurfaceClusters, ticket.surfaceClusterMirrorSizeAfterPatch)
            : static_cast<uint32_t>(ticket.surfaceClusterMirrorAfterCopy.size()))
        : m_stats.uploadedSurfaceClusters;
    m_stats.stagedRangeCopyRegionsLastFrame = static_cast<uint32_t>(ticket.rangeCopyRegions.size());
    m_stats.stagedDrawCopyRegionsLastFrame = static_cast<uint32_t>(ticket.drawArgsCopyRegions.size());
    m_stats.stagedSurfaceRecordCopyRegionsLastFrame =
        static_cast<uint32_t>(ticket.surfaceRecordCopyRegions.size());
    m_stats.stagedSurfaceClusterCopyRegionsLastFrame =
        static_cast<uint32_t>(ticket.surfaceClusterCopyRegions.size());
    m_stats.skippedCleanRangeSlotsLastFrame =
        m_stats.uploadedRangeTableCapacity > changedRangeSlots
            ? m_stats.uploadedRangeTableCapacity - changedRangeSlots
            : 0u;
    m_stats.skippedCleanDrawCommandsLastFrame =
        m_stats.uploadedDrawCommands > changedDrawCommands
            ? m_stats.uploadedDrawCommands - changedDrawCommands
            : 0u;
    m_stats.skippedCleanSurfaceRecordsLastFrame =
        m_stats.uploadedSurfaceRecords > changedSurfaceRecords
            ? m_stats.uploadedSurfaceRecords - changedSurfaceRecords
            : 0u;
    m_stats.skippedCleanSurfaceClustersLastFrame =
        m_stats.uploadedSurfaceClusters > changedSurfaceClusters
            ? m_stats.uploadedSurfaceClusters - changedSurfaceClusters
            : 0u;
    m_stats.stagedFaceCopyRegionsLastFrame = static_cast<uint32_t>(ticket.faceCopyRegions.size());
    m_stats.stagedPayloadPatchBricksLastFrame = patchedPayloadBrickCount;
    m_stats.stagedPayloadPatchFacesLastFrame = patchedPayloadFaceCount;
    m_stats.stagedPayloadPatchRegionsLastFrame = patchedPayloadRegionCount;
    m_stats.stagedDirtyPayloadBricksLastFrame = copiedPayloadBrickCount;
    m_stats.skippedCleanPayloadBricksLastFrame =
        m_stats.uploadedRanges > copiedPayloadBrickCount
            ? m_stats.uploadedRanges - copiedPayloadBrickCount
            : 0u;
    m_stats.deferredPayloadBricksLastFrame = deferredPayloadBrickCount;
    m_stats.pendingDirtyBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
    m_stats.pendingRemovedBricksLastFrame = static_cast<uint32_t>(snapshot.removedBricks.size());
    m_stats.stagedBytesLastFrame = writeOffset - AlignUp(uploadWriteOffsetBeforeStage, 4u);
    const auto& allocatorStats = m_faceRangeAllocator.GetStats();
    m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
    m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
    m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
    m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
    m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
    m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
    m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;

    const auto dirtyStageEndTime = DirtyStageClock::now();
    const double dirtyStageTotalMs = dirtyStageElapsedMs(dirtyStageStartTime, dirtyStageEndTime);
    if (dirtyStageTotalMs >= 6.0 ||
        m_stats.stagedBytesLastFrame >= 256u * 1024u ||
        copiedPayloadFaceCount >= 8192u) {
        spdlog::info(
            "PERF_SPARSE_DIRTY_STAGE serial={} dirty={} removed={} zeroFace={} allocChanged={} "
            "copyBricks={} copyFaces={} fullCopyBricks={} fullCopyFaces={} patchBricks={} "
            "patchFaces={} patchRegions={} mirrorCmpBricks={} cleanMirrorBricks={} changedRuns={} "
            "changedRunFaces={} mirrorUpdateBricks={} mirrorUpdateFaces={} newBricks={} deferred={} "
            "rangeCopies={} drawCopies={} recordCopies={} clusterCopies={} metadataFull={} metadataIncr={} "
            "stagedMB={:.2f} setupMs={:.2f} removedMs={:.2f} dirtyLoopMs={:.2f} finalMs={:.2f} totalMs={:.2f}",
            snapshot.serial,
            static_cast<uint32_t>(snapshot.dirtyBricks.size()),
            static_cast<uint32_t>(snapshot.removedBricks.size()),
            static_cast<uint32_t>(zeroFaceDirtyCoords.size()),
            allocationChangedBrickCount,
            copiedPayloadBrickCount,
            copiedPayloadFaceCount,
            fullPayloadCopyBrickCount,
            fullPayloadCopyFaceCount,
            patchedPayloadBrickCount,
            patchedPayloadFaceCount,
            patchedPayloadRegionCount,
            mirrorComparableBrickCount,
            cleanMirrorBrickCount,
            changedRunCount,
            changedRunFaceCount,
            mirrorUpdateBrickCount,
            mirrorUpdateFaceCount,
            newPayloadBrickCount,
            deferredPayloadBrickCount,
            static_cast<uint32_t>(ticket.rangeCopyRegions.size()),
            static_cast<uint32_t>(ticket.drawArgsCopyRegions.size()),
            static_cast<uint32_t>(ticket.surfaceRecordCopyRegions.size()),
            static_cast<uint32_t>(ticket.surfaceClusterCopyRegions.size()),
            metadataMirrorsInitialized ? 1u : 0u,
            ticket.incrementalMetadataPatches ? 1u : 0u,
            static_cast<double>(m_stats.stagedBytesLastFrame) / (1024.0 * 1024.0),
            dirtyStageElapsedMs(dirtyStageStartTime, dirtyStageAfterSetupTime),
            dirtyStageElapsedMs(dirtyStageAfterSetupTime, dirtyStageAfterRemovedTime),
            dirtyStageElapsedMs(dirtyStageAfterRemovedTime, dirtyStageAfterDirtyLoopTime),
            dirtyStageElapsedMs(dirtyStageAfterDirtyLoopTime, dirtyStageEndTime),
            dirtyStageTotalMs);
    }

    if (outTicket) {
        *outTicket = std::move(ticket);
    }
    return true;
}

bool SparseSurfaceGpuResources::StageSnapshot(
    const Simulation::SparseSurfaceGpuSnapshot& snapshot,
    SparseSurfaceUploadTicket* outTicket)
{
    if (outTicket) {
        *outTicket = {};
    }
    if (!m_stats.initialized || m_activeUploadSlot >= m_config.uploadRingSlots) {
        return false;
    }
    const uint64_t snapshotFaceCount =
        std::max<uint64_t>(
            static_cast<uint64_t>(snapshot.faces.size()),
            static_cast<uint64_t>(snapshot.visibleFaceCount));
    if (snapshotFaceCount > m_config.maxFaces ||
        snapshot.ranges.size() > m_config.maxBrickRanges ||
        snapshot.drawArgs.size() > m_config.maxDrawCommands ||
        snapshot.surfaceRecords.size() > m_config.maxDrawCommands) {
        m_stats.uploadOverflowLastFrame = true;
        return false;
    }

    UploadBuffer& upload = m_uploadRing[m_activeUploadSlot];
    uint8_t* mapped = static_cast<uint8_t*>(upload.GetMappedData());
    if (!mapped) {
        return false;
    }

    constexpr uint64_t kUploadAlignment = 256u;
    const uint64_t uploadWriteOffsetBeforeStage = m_uploadWriteOffset;
    if (m_config.useRangeAllocator) {
        const Simulation::SparseSurfaceRangeAllocator allocatorBeforeStage = m_faceRangeAllocator;
        auto failRangeAllocatorStage = [&](bool preserveAllocationFailure = false) -> bool {
            const uint32_t observedAllocationFailures =
                m_faceRangeAllocator.GetStats().allocationFailures;
            m_faceRangeAllocator = allocatorBeforeStage;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.uploadOverflowLastFrame = true;
            m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
            m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
            m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
            m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
            m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
            m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
            m_stats.faceRangeAllocationFailures = preserveAllocationFailure
                ? std::max(allocatorStats.allocationFailures, observedAllocationFailures)
                : allocatorStats.allocationFailures;
            return false;
        };

        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> liveCoords;
        liveCoords.reserve(snapshot.brickFaceCounts.size());
        for (const auto& item : snapshot.brickFaceCounts) {
            liveCoords.insert(item.coord);
        }
        m_faceRangeAllocator.ReleaseNotIn(liveCoords);

        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> dirtyCoords;
        dirtyCoords.reserve(snapshot.dirtyBricks.size());
        for (const auto& item : snapshot.dirtyBricks) {
            dirtyCoords.insert(item.coord);
        }

        SparseSurfaceUploadTicket ticket;
        ticket.valid = true;
        ticket.ringSlot = m_activeUploadSlot;
        ticket.serial = snapshot.serial;
        ticket.candidateBricks = snapshot.candidateBricks;
        ticket.visibleBricks = snapshot.visibleBricks;
        ticket.culledBricks = snapshot.culledBricks;
        ticket.hasUploadWriteOffsetRollback = true;
        ticket.uploadWriteOffsetBeforeStage = uploadWriteOffsetBeforeStage;
        ticket.hasRangeAllocatorRollback = true;
        ticket.rangeAllocatorBeforeStage = allocatorBeforeStage;
        ticket.removedBricks.reserve(snapshot.removedBricks.size());
        for (const auto& removed : snapshot.removedBricks) {
            if (removed.serial <= snapshot.serial) {
                ticket.removedBricks.push_back(removed.coord);
            }
        }

        std::vector<Simulation::SparseSurfaceBrickRange> remappedRanges(snapshot.ranges.size());
        const bool compactStableDrawCommands =
            m_config.useStableDrawSlots && m_config.compactStableDrawCommands;
        std::vector<Simulation::SparseSurfaceDrawArgs> remappedDrawArgs;
        std::vector<Simulation::SparseSurfaceDrawArgs> stableSlotDrawArgs;
        std::vector<Simulation::SparseSurfaceRecord> remappedSurfaceRecords;
        remappedSurfaceRecords.reserve(snapshot.drawBatches.size());
        if (!m_config.useStableDrawSlots || compactStableDrawCommands) {
            remappedDrawArgs.reserve(snapshot.drawBatches.size());
        }
        auto nextDrawSlotByCoord = m_drawSlotByCoord;
        auto nextDrawSlotOccupied = m_drawSlotOccupied;
        auto nextFreeDrawSlots = m_freeDrawSlots;
        if (m_config.useStableDrawSlots) {
            for (const Simulation::BrickCoord& coord : ticket.removedBricks) {
                auto slotIt = nextDrawSlotByCoord.find(coord);
                if (slotIt == nextDrawSlotByCoord.end()) {
                    continue;
                }
                const uint32_t slot = slotIt->second;
                nextDrawSlotByCoord.erase(slotIt);
                if (slot < nextDrawSlotOccupied.size() && nextDrawSlotOccupied[slot] != 0u) {
                    nextDrawSlotOccupied[slot] = 0u;
                    nextFreeDrawSlots.push_back(slot);
                }
            }
            if (compactStableDrawCommands) {
                stableSlotDrawArgs.resize(nextDrawSlotOccupied.size());
            } else {
                remappedDrawArgs.resize(nextDrawSlotOccupied.size());
            }
        }

        const uint64_t stageStartOffset = AlignUp(m_uploadWriteOffset, 4u);
        uint64_t writeOffset = stageStartOffset;
        uint32_t drawableFaceCount = 0;
        uint32_t copiedPayloadFaceCount = 0;
        uint32_t copiedPayloadBrickCount = 0;
        uint32_t patchedPayloadFaceCount = 0;
        uint32_t patchedPayloadBrickCount = 0;
        uint32_t patchedPayloadRegionCount = 0;
        uint32_t skippedCleanPayloadBrickCount = 0;
        uint32_t deferredPayloadBrickCount = 0;
        std::unordered_map<
            Simulation::BrickCoord,
            Simulation::SparseSurfaceFaceAllocation,
            Simulation::BrickCoordHash> visibleAllocations;
        visibleAllocations.reserve(snapshot.drawBatches.size());

        auto appendDraw = [&](const Simulation::BrickCoord& coord, const Simulation::SparseSurfaceFaceAllocation& allocation) -> bool {
            Simulation::SparseSurfaceDrawArgs args;
            args.indexCountPerInstance = allocation.faceCount * 6u;
            args.instanceCount = 1u;
            args.startIndexLocation = allocation.firstFace * 6u;
            args.baseVertexLocation = 0;
            args.startInstanceLocation = 0u;
            if (m_config.useStableDrawSlots) {
                auto slotIt = nextDrawSlotByCoord.find(coord);
                uint32_t slot = UINT32_MAX;
                if (slotIt != nextDrawSlotByCoord.end()) {
                    slot = slotIt->second;
                } else if (!nextFreeDrawSlots.empty()) {
                    slot = nextFreeDrawSlots.back();
                    nextFreeDrawSlots.pop_back();
                    nextDrawSlotByCoord.emplace(coord, slot);
                    if (slot >= nextDrawSlotOccupied.size()) {
                        return false;
                    }
                    nextDrawSlotOccupied[slot] = 1u;
                } else {
                    if (nextDrawSlotOccupied.size() >= m_config.maxDrawCommands) {
                        return false;
                    }
                    slot = static_cast<uint32_t>(nextDrawSlotOccupied.size());
                    nextDrawSlotByCoord.emplace(coord, slot);
                    nextDrawSlotOccupied.push_back(1u);
                    if (compactStableDrawCommands) {
                        stableSlotDrawArgs.resize(nextDrawSlotOccupied.size());
                    } else {
                        remappedDrawArgs.resize(nextDrawSlotOccupied.size());
                    }
                }
                if (compactStableDrawCommands) {
                    if (slot >= stableSlotDrawArgs.size()) {
                        stableSlotDrawArgs.resize(slot + 1u);
                    }
                    stableSlotDrawArgs[slot] = args;
                } else {
                    if (slot >= remappedDrawArgs.size()) {
                        remappedDrawArgs.resize(slot + 1u);
                    }
                    remappedDrawArgs[slot] = args;
                }
                return true;
            }
            remappedDrawArgs.push_back(args);
            return true;
        };

        auto batchFaceData = [&](const Simulation::SparseSurfaceDrawBatch& batch) -> const Simulation::SparseSurfaceFace* {
            if (batch.faceCount == 0u) {
                return nullptr;
            }
            if (batch.faces) {
                return batch.faces;
            }
            const uint64_t batchFaceEnd =
                static_cast<uint64_t>(batch.firstFace) + static_cast<uint64_t>(batch.faceCount);
            if (batchFaceEnd <= snapshot.faces.size()) {
                return snapshot.faces.data() + batch.firstFace;
            }
            return nullptr;
        };

        auto queuePayloadMirrorUpdate = [&](
            const Simulation::SparseSurfaceDrawBatch& batch,
            const Simulation::SparseSurfaceFace* batchFaces)
        {
            SparseSurfacePayloadMirrorUpdate update;
            update.coord = batch.coord;
            if (batch.faceCount > 0u && batchFaces) {
                update.faces.assign(batchFaces, batchFaces + batch.faceCount);
            }
            ticket.payloadMirrorUpdates.push_back(std::move(update));
        };

        auto stageFaceCopy = [&](
            const Simulation::SparseSurfaceFaceAllocation& allocation,
            const Simulation::SparseSurfaceFace* sourceFaces,
            uint32_t sourceFirstFace,
            uint32_t destFirstFace,
            uint32_t faceCount) -> bool
        {
            if (faceCount == 0u) {
                return true;
            }
            const uint64_t faceBytes =
                static_cast<uint64_t>(faceCount) * sizeof(Simulation::SparseSurfaceFace);
            uint64_t uploadOffset = 0;
            uint64_t endOffset = 0;
            const uint64_t sourceEnd =
                static_cast<uint64_t>(sourceFirstFace) + static_cast<uint64_t>(faceCount);
            const uint64_t destEnd =
                static_cast<uint64_t>(destFirstFace) + static_cast<uint64_t>(faceCount);
            const uint64_t allocationEnd =
                static_cast<uint64_t>(allocation.firstFace) + static_cast<uint64_t>(allocation.faceCount);
            if (!AppendAlignedUploadRange(writeOffset, faceBytes, 4u, &uploadOffset, &endOffset) ||
                endOffset > upload.GetSize() ||
                !sourceFaces ||
                sourceEnd > allocation.faceCount ||
                destFirstFace < allocation.firstFace ||
                destEnd > allocationEnd) {
                return false;
            }
            std::memcpy(
                mapped + uploadOffset,
                sourceFaces + sourceFirstFace,
                static_cast<size_t>(faceBytes));
            ticket.faceCopyRegions.push_back({
                uploadOffset,
                destFirstFace,
                faceCount
            });
            writeOffset = endOffset;
            return true;
        };

        for (const Simulation::SparseSurfaceDrawBatch& batch : snapshot.drawBatches) {
            const Simulation::SparseSurfaceFace* sourceFaces = batchFaceData(batch);
            Simulation::SparseSurfaceFaceAllocation previousAllocation;
            const bool hadAllocation =
                m_faceRangeAllocator.TryGet(batch.coord, &previousAllocation);
            const bool payloadResident =
                m_payloadResidentCoords.find(batch.coord) != m_payloadResidentCoords.end();
            const bool dirtyPayload = dirtyCoords.find(batch.coord) != dirtyCoords.end();
            const bool allocationChanged =
                !hadAllocation || previousAllocation.faceCount != batch.faceCount;
            const bool needsPayloadUpload =
                batch.faceCount > 0u && (!payloadResident || dirtyPayload || allocationChanged);

            const bool regionBudgetAvailable =
                m_config.maxPayloadCopyRegionsPerFrame == 0u ||
                copiedPayloadBrickCount < m_config.maxPayloadCopyRegionsPerFrame;
            const bool faceBudgetAvailable =
                m_config.maxPayloadCopyFacesPerFrame == 0u ||
                copiedPayloadFaceCount + batch.faceCount <= m_config.maxPayloadCopyFacesPerFrame ||
                copiedPayloadBrickCount == 0u;
            const bool canUploadPayload = !needsPayloadUpload || (regionBudgetAvailable && faceBudgetAvailable);

            if (needsPayloadUpload) {
                if (batch.faceCount > 0u && !sourceFaces) {
                    return failRangeAllocatorStage();
                }
                if (!canUploadPayload) {
                    if (hadAllocation && payloadResident) {
                        visibleAllocations.emplace(batch.coord, previousAllocation);
                        if (!appendDraw(batch.coord, previousAllocation)) {
                            return failRangeAllocatorStage();
                        }
                        drawableFaceCount += previousAllocation.faceCount;
                    }
                    ++deferredPayloadBrickCount;
                    continue;
                }

                Simulation::SparseSurfaceFaceAllocation allocation;
                if (!m_faceRangeAllocator.AllocateOrResize(batch.coord, batch.faceCount, &allocation)) {
                    return failRangeAllocatorStage(true);
                }

                bool uploadedPayload = false;
                if (hadAllocation &&
                    payloadResident &&
                    dirtyPayload &&
                    !allocationChanged &&
                    allocation.firstFace == previousAllocation.firstFace &&
                    allocation.faceCount == previousAllocation.faceCount) {
                    auto mirrorIt = m_payloadFaceMirrorByCoord.find(batch.coord);
                    const bool mirrorUsable =
                        mirrorIt != m_payloadFaceMirrorByCoord.end() &&
                        mirrorIt->second.size() == batch.faceCount &&
                        sourceFaces != nullptr;
                    const auto runs = Simulation::BuildSparseSurfaceChangedFaceRuns(
                        mirrorUsable ? sourceFaces : nullptr,
                        mirrorUsable ? mirrorIt->second.data() : nullptr,
                        batch.faceCount);
                    uint32_t runFaceCount = 0;
                    for (const auto& run : runs) {
                        runFaceCount += run.faceCount;
                    }
                    const uint32_t remainingRegionBudget =
                        m_config.maxPayloadCopyRegionsPerFrame == 0u
                            ? UINT_MAX
                            : m_config.maxPayloadCopyRegionsPerFrame -
                                std::min<uint32_t>(
                                    m_config.maxPayloadCopyRegionsPerFrame,
                                    static_cast<uint32_t>(ticket.faceCopyRegions.size()));
                    const uint32_t remainingFaceBudget =
                        m_config.maxPayloadCopyFacesPerFrame == 0u
                            ? UINT_MAX
                            : m_config.maxPayloadCopyFacesPerFrame - copiedPayloadFaceCount;
                    const bool canPatch =
                        !runs.empty() &&
                        runs.size() <= remainingRegionBudget &&
                        runFaceCount <= remainingFaceBudget;
                    if (runs.empty()) {
                        uploadedPayload = true;
                    } else if (canPatch) {
                        for (const auto& run : runs) {
                            uint64_t destFirstFace = 0;
                            if (!AddUint64(allocation.firstFace, run.firstFace, &destFirstFace) ||
                                destFirstFace > std::numeric_limits<uint32_t>::max()) {
                                return failRangeAllocatorStage();
                            }
                            if (!stageFaceCopy(
                                    allocation,
                                    sourceFaces,
                                    run.firstFace,
                                    static_cast<uint32_t>(destFirstFace),
                                    run.faceCount)) {
                                return failRangeAllocatorStage();
                            }
                        }
                        copiedPayloadFaceCount += runFaceCount;
                        ++copiedPayloadBrickCount;
                        patchedPayloadFaceCount += runFaceCount;
                        ++patchedPayloadBrickCount;
                        patchedPayloadRegionCount += static_cast<uint32_t>(runs.size());
                        uploadedPayload = true;
                    }
                }

                if (!uploadedPayload) {
                    if (!stageFaceCopy(
                            allocation,
                            sourceFaces,
                            0u,
                            allocation.firstFace,
                            batch.faceCount)) {
                        return failRangeAllocatorStage();
                    }
                    copiedPayloadFaceCount += batch.faceCount;
                    ++copiedPayloadBrickCount;
                }
                ticket.uploadedPayloadBricks.push_back(batch.coord);
                queuePayloadMirrorUpdate(batch, sourceFaces);
                visibleAllocations.emplace(batch.coord, allocation);
                if (!appendDraw(batch.coord, allocation)) {
                    return failRangeAllocatorStage();
                }
                drawableFaceCount += allocation.faceCount;
            } else {
                visibleAllocations.emplace(batch.coord, previousAllocation);
                if (!appendDraw(batch.coord, previousAllocation)) {
                    return failRangeAllocatorStage();
                }
                drawableFaceCount += previousAllocation.faceCount;
                ++skippedCleanPayloadBrickCount;
            }
        }

        for (size_t i = 0; i < snapshot.ranges.size(); ++i) {
            const Simulation::SparseSurfaceBrickRange& source = snapshot.ranges[i];
            if (source.flags == 0u) {
                continue;
            }
            Simulation::SparseSurfaceBrickRange range = source;
            auto allocationIt = visibleAllocations.find(source.coord);
            if (allocationIt != visibleAllocations.end()) {
                range.firstFace = allocationIt->second.firstFace;
                range.faceCount = allocationIt->second.faceCount;
            } else {
                range.firstFace = 0u;
                range.faceCount = 0u;
                if (source.faceCount == 0u && dirtyCoords.find(source.coord) != dirtyCoords.end()) {
                    ticket.uploadedPayloadBricks.push_back(source.coord);
                }
            }
            remappedRanges[i] = range;

            if (range.flags != 0u && range.faceCount > 0u) {
                Simulation::SparseSurfaceRecord record;
                record.coord = range.coord;
                record.firstFace = range.firstFace;
                record.faceCount = range.faceCount;
                record.flags = range.flags;
                record.generation = snapshot.serial;
                SetSurfaceRecordBoundsFromBrickCoord(range.coord, record);
                remappedSurfaceRecords.push_back(record);
            }
        }

        uint32_t inactiveStableDrawSlots = 0;
        if (compactStableDrawCommands) {
            remappedDrawArgs.clear();
            remappedDrawArgs.reserve(snapshot.drawBatches.size());
            for (size_t slot = 0; slot < nextDrawSlotOccupied.size(); ++slot) {
                if (nextDrawSlotOccupied[slot] == 0u) {
                    ++inactiveStableDrawSlots;
                    continue;
                }
                const Simulation::SparseSurfaceDrawArgs& args = slot < stableSlotDrawArgs.size()
                    ? stableSlotDrawArgs[slot]
                    : Simulation::SparseSurfaceDrawArgs{};
                if (args.indexCountPerInstance == 0u || args.instanceCount == 0u) {
                    ++inactiveStableDrawSlots;
                    continue;
                }
                remappedDrawArgs.push_back(args);
            }
        }

        Simulation::SortSparseSurfaceRecordsForClusters(remappedSurfaceRecords);
        std::vector<Simulation::SparseSurfaceClusterRecord> remappedSurfaceClusters =
            Simulation::BuildSparseSurfaceClusters(
                remappedSurfaceRecords,
                m_config.surfaceRecordsPerCluster,
                m_config.surfaceClusterMaxExtentVoxels);
        if (remappedSurfaceRecords.size() > m_config.maxDrawCommands ||
            remappedSurfaceClusters.size() > m_config.maxDrawCommands ||
            remappedDrawArgs.size() > m_config.maxDrawCommands) {
            return failRangeAllocatorStage();
        }

        std::vector<Simulation::SparseSurfaceBrickRange> publishedRanges;
        if (m_config.useFixedRangeTable) {
            if (static_cast<uint64_t>(snapshot.rangeCount) * 2ull >
                static_cast<uint64_t>(m_config.maxBrickRanges)) {
                return failRangeAllocatorStage();
            }
            publishedRanges.resize(m_config.maxBrickRanges);
            const uint32_t mask = m_config.maxBrickRanges - 1u;
            for (const Simulation::SparseSurfaceBrickRange& source : remappedRanges) {
                if (source.flags == 0u) {
                    continue;
                }
                uint32_t slot = Simulation::HashBrickCoord32(source.coord) & mask;
                bool inserted = false;
                for (uint32_t probe = 0; probe < m_config.maxBrickRanges; ++probe) {
                    Simulation::SparseSurfaceBrickRange& tableEntry = publishedRanges[slot];
                    if (tableEntry.flags == 0u) {
                        tableEntry = source;
                        inserted = true;
                        break;
                    }
                    slot = (slot + 1u) & mask;
                }
                if (!inserted) {
                    return failRangeAllocatorStage();
                }
            }
        } else {
            publishedRanges = std::move(remappedRanges);
        }

        const bool fullRangeUpload = m_rangeMirror.empty() && !publishedRanges.empty();
        const bool fullDrawUpload = m_drawArgsMirror.empty() && !remappedDrawArgs.empty();
        const bool fullSurfaceRecordUpload = m_surfaceRecordMirror.empty() && !remappedSurfaceRecords.empty();
        const bool fullSurfaceClusterUpload = m_surfaceClusterMirror.empty() && !remappedSurfaceClusters.empty();
        uint32_t skippedCleanRangeSlots = 0;
        uint32_t skippedCleanDrawCommands = 0;
        uint32_t skippedCleanSurfaceRecords = 0;
        uint32_t skippedCleanSurfaceClusters = 0;

        auto stageChangedBlocks = [&](
            const auto& source,
            const auto& mirror,
            bool forceFull,
            uint64_t elementSize,
            uint64_t destBaseOffset,
            uint64_t& inOutWriteOffset,
            std::vector<SparseSurfaceBufferCopyRegion>& outRegions,
            uint32_t& outSkippedCleanElements) -> bool
        {
            const uint32_t elementCount = static_cast<uint32_t>(source.size());
            uint32_t index = 0;
            while (index < elementCount) {
                bool changed = forceFull;
                if (!changed && index >= mirror.size()) {
                    changed = true;
                }
                if (!changed) {
                    changed = !SameBytes(
                        &source[index],
                        &mirror[index],
                        static_cast<size_t>(elementSize));
                }
                if (!changed) {
                    ++outSkippedCleanElements;
                    ++index;
                    continue;
                }

                const uint32_t start = index;
                ++index;
                while (index < elementCount) {
                    if (!forceFull &&
                        index < mirror.size() &&
                        SameBytes(
                            &source[index],
                            &mirror[index],
                            static_cast<size_t>(elementSize))) {
                        break;
                    }
                    ++index;
                }

                const uint32_t count = index - start;
                const uint64_t byteCount = static_cast<uint64_t>(count) * elementSize;
                uint64_t uploadOffset = 0;
                uint64_t endOffset = 0;
                if (!AppendAlignedUploadRange(inOutWriteOffset, byteCount, 4u, &uploadOffset, &endOffset) ||
                    endOffset > upload.GetSize()) {
                    return false;
                }
                std::memcpy(
                    mapped + uploadOffset,
                    source.data() + start,
                    static_cast<size_t>(byteCount));
                outRegions.push_back({
                    uploadOffset,
                    destBaseOffset + static_cast<uint64_t>(start) * elementSize,
                    byteCount
                });
                inOutWriteOffset = endOffset;
            }
            return true;
        };

        uint64_t metadataWriteOffset = AlignUp(writeOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                publishedRanges,
                m_rangeMirror,
                fullRangeUpload,
                sizeof(Simulation::SparseSurfaceBrickRange),
                0u,
                metadataWriteOffset,
                ticket.rangeCopyRegions,
                skippedCleanRangeSlots)) {
            return failRangeAllocatorStage();
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedDrawArgs,
                m_drawArgsMirror,
                fullDrawUpload,
                sizeof(Simulation::SparseSurfaceDrawArgs),
                0u,
                metadataWriteOffset,
                ticket.drawArgsCopyRegions,
                skippedCleanDrawCommands)) {
            return failRangeAllocatorStage();
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedSurfaceRecords,
                m_surfaceRecordMirror,
                fullSurfaceRecordUpload,
                sizeof(Simulation::SparseSurfaceRecord),
                0u,
                metadataWriteOffset,
                ticket.surfaceRecordCopyRegions,
                skippedCleanSurfaceRecords)) {
            return failRangeAllocatorStage();
        }
        metadataWriteOffset = AlignUp(metadataWriteOffset, kUploadAlignment);
        if (!stageChangedBlocks(
                remappedSurfaceClusters,
                m_surfaceClusterMirror,
                fullSurfaceClusterUpload,
                sizeof(Simulation::SparseSurfaceClusterRecord),
                0u,
                metadataWriteOffset,
                ticket.surfaceClusterCopyRegions,
                skippedCleanSurfaceClusters)) {
            return failRangeAllocatorStage();
        }
        const uint64_t endOffset = metadataWriteOffset;
        const uint64_t rangeBytes = 0;
        const uint64_t drawArgsBytes = 0;
        const uint64_t surfaceRecordBytes = 0;
        const uint64_t rangeOffset = 0;
        const uint64_t drawArgsOffset = 0;
        const uint64_t surfaceRecordOffset = 0;
        if (endOffset > upload.GetSize()) {
            return failRangeAllocatorStage();
        }
        ticket.rangeMirrorAfterCopy = std::move(publishedRanges);
        ticket.drawArgsMirrorAfterCopy = std::move(remappedDrawArgs);
        ticket.surfaceRecordMirrorAfterCopy = std::move(remappedSurfaceRecords);
        ticket.surfaceClusterMirrorAfterCopy = std::move(remappedSurfaceClusters);
        ticket.drawSlotByCoordAfterCopy = std::move(nextDrawSlotByCoord);
        ticket.drawSlotOccupiedAfterCopy = std::move(nextDrawSlotOccupied);
        ticket.freeDrawSlotsAfterCopy = std::move(nextFreeDrawSlots);

        m_uploadWriteOffset = endOffset;
        m_stats.stagedFacesLastFrame = drawableFaceCount;
        m_stats.stagedRangesLastFrame = snapshot.rangeCount;
        m_stats.stagedRangeTableCapacityLastFrame = static_cast<uint32_t>(ticket.rangeMirrorAfterCopy.size());
        m_stats.stagedDrawCommandsLastFrame = static_cast<uint32_t>(ticket.drawArgsMirrorAfterCopy.size());
        m_stats.activeDrawCommandsLastFrame = static_cast<uint32_t>(snapshot.drawBatches.size());
        m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(ticket.drawSlotOccupiedAfterCopy.size());
        m_stats.stableDrawFreeSlots = static_cast<uint32_t>(ticket.freeDrawSlotsAfterCopy.size());
        m_stats.inactiveStableDrawSlotsLastFrame = inactiveStableDrawSlots;
        m_stats.stagedSurfaceRecordsLastFrame = static_cast<uint32_t>(ticket.surfaceRecordMirrorAfterCopy.size());
        m_stats.stagedSurfaceClustersLastFrame = static_cast<uint32_t>(ticket.surfaceClusterMirrorAfterCopy.size());
        m_stats.stagedRangeCopyRegionsLastFrame = static_cast<uint32_t>(ticket.rangeCopyRegions.size());
        m_stats.stagedDrawCopyRegionsLastFrame = static_cast<uint32_t>(ticket.drawArgsCopyRegions.size());
        m_stats.stagedSurfaceRecordCopyRegionsLastFrame =
            static_cast<uint32_t>(ticket.surfaceRecordCopyRegions.size());
        m_stats.stagedSurfaceClusterCopyRegionsLastFrame =
            static_cast<uint32_t>(ticket.surfaceClusterCopyRegions.size());
        m_stats.skippedCleanRangeSlotsLastFrame = skippedCleanRangeSlots;
        m_stats.skippedCleanDrawCommandsLastFrame = skippedCleanDrawCommands;
        m_stats.skippedCleanSurfaceRecordsLastFrame = skippedCleanSurfaceRecords;
        m_stats.skippedCleanSurfaceClustersLastFrame = skippedCleanSurfaceClusters;
        m_stats.fullRangeTableUploadLastFrame = fullRangeUpload;
        m_stats.fullDrawArgsUploadLastFrame = fullDrawUpload;
        m_stats.fullSurfaceRecordUploadLastFrame = fullSurfaceRecordUpload;
        m_stats.fullSurfaceClusterUploadLastFrame = fullSurfaceClusterUpload;
        m_stats.stagedCandidateBricksLastFrame = snapshot.candidateBricks;
        m_stats.stagedVisibleBricksLastFrame = snapshot.visibleBricks;
        m_stats.stagedCulledBricksLastFrame = snapshot.culledBricks;
        m_stats.stagedFaceCopyRegionsLastFrame = static_cast<uint32_t>(ticket.faceCopyRegions.size());
        m_stats.stagedPayloadPatchBricksLastFrame = patchedPayloadBrickCount;
        m_stats.stagedPayloadPatchFacesLastFrame = patchedPayloadFaceCount;
        m_stats.stagedPayloadPatchRegionsLastFrame = patchedPayloadRegionCount;
        m_stats.stagedDirtyPayloadBricksLastFrame = copiedPayloadBrickCount;
        m_stats.skippedCleanPayloadBricksLastFrame = skippedCleanPayloadBrickCount;
        m_stats.deferredPayloadBricksLastFrame = deferredPayloadBrickCount;
        m_stats.pendingDirtyBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
        m_stats.pendingRemovedBricksLastFrame = static_cast<uint32_t>(snapshot.removedBricks.size());
        m_stats.stagedBytesLastFrame = endOffset - stageStartOffset;
        const auto& allocatorStats = m_faceRangeAllocator.GetStats();
        m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
        m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
        m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
        m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
        m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
        m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
        m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;

        ticket.rangeUploadOffset = rangeOffset;
        ticket.drawArgsUploadOffset = drawArgsOffset;
        ticket.surfaceRecordUploadOffset = surfaceRecordOffset;
        ticket.rangeBytes = rangeBytes;
        ticket.drawArgsBytes = drawArgsBytes;
        ticket.surfaceRecordBytes = surfaceRecordBytes;
        ticket.faceCount = drawableFaceCount;
        ticket.rangeCount = snapshot.rangeCount;
        ticket.rangeTableCapacity = static_cast<uint32_t>(ticket.rangeMirrorAfterCopy.size());
        ticket.drawCommandCount = static_cast<uint32_t>(ticket.drawArgsMirrorAfterCopy.size());
        ticket.activeDrawCommandCount = static_cast<uint32_t>(snapshot.drawBatches.size());
        ticket.deferredPayloadBricks = deferredPayloadBrickCount;

        if (outTicket) {
            *outTicket = std::move(ticket);
        }
        return true;
    }

    const uint64_t faceBytes =
        static_cast<uint64_t>(snapshot.faces.size()) * sizeof(Simulation::SparseSurfaceFace);
    const uint64_t rangeBytes =
        static_cast<uint64_t>(snapshot.ranges.size()) * sizeof(Simulation::SparseSurfaceBrickRange);
    const uint64_t drawArgsBytes =
        static_cast<uint64_t>(snapshot.drawArgs.size()) * sizeof(Simulation::SparseSurfaceDrawArgs);
    std::vector<Simulation::SparseSurfaceRecord> fallbackSurfaceRecords = snapshot.surfaceRecords;
    Simulation::SortSparseSurfaceRecordsForClusters(fallbackSurfaceRecords);
    const uint64_t surfaceRecordBytes =
        static_cast<uint64_t>(fallbackSurfaceRecords.size()) * sizeof(Simulation::SparseSurfaceRecord);
    std::vector<Simulation::SparseSurfaceClusterRecord> fallbackSurfaceClusters =
        Simulation::BuildSparseSurfaceClusters(
            fallbackSurfaceRecords,
            m_config.surfaceRecordsPerCluster,
            m_config.surfaceClusterMaxExtentVoxels);
    const uint64_t surfaceClusterBytes =
        static_cast<uint64_t>(fallbackSurfaceClusters.size()) * sizeof(Simulation::SparseSurfaceClusterRecord);
    uint64_t faceOffset = 0;
    uint64_t rangeOffset = 0;
    uint64_t drawArgsOffset = 0;
    uint64_t surfaceRecordOffset = 0;
    uint64_t surfaceClusterOffset = 0;
    uint64_t afterFaces = 0;
    uint64_t afterRanges = 0;
    uint64_t afterDrawArgs = 0;
    uint64_t afterSurfaceRecords = 0;
    uint64_t endOffset = 0;
    if (!AppendAlignedUploadRange(m_uploadWriteOffset, faceBytes, kUploadAlignment, &faceOffset, &afterFaces) ||
        !AppendAlignedUploadRange(afterFaces, rangeBytes, kUploadAlignment, &rangeOffset, &afterRanges) ||
        !AppendAlignedUploadRange(afterRanges, drawArgsBytes, kUploadAlignment, &drawArgsOffset, &afterDrawArgs) ||
        !AppendAlignedUploadRange(afterDrawArgs, surfaceRecordBytes, kUploadAlignment, &surfaceRecordOffset, &afterSurfaceRecords) ||
        !AppendAlignedUploadRange(afterSurfaceRecords, surfaceClusterBytes, kUploadAlignment, &surfaceClusterOffset, &endOffset) ||
        endOffset > upload.GetSize()) {
        m_stats.uploadOverflowLastFrame = true;
        return false;
    }

    if (faceBytes > 0) {
        std::memcpy(mapped + faceOffset, snapshot.faces.data(), static_cast<size_t>(faceBytes));
    }
    if (rangeBytes > 0) {
        std::memcpy(mapped + rangeOffset, snapshot.ranges.data(), static_cast<size_t>(rangeBytes));
    }
    if (drawArgsBytes > 0) {
        std::memcpy(mapped + drawArgsOffset, snapshot.drawArgs.data(), static_cast<size_t>(drawArgsBytes));
    }
    if (surfaceRecordBytes > 0) {
        std::memcpy(
            mapped + surfaceRecordOffset,
            fallbackSurfaceRecords.data(),
            static_cast<size_t>(surfaceRecordBytes));
    }
    if (surfaceClusterBytes > 0) {
        std::memcpy(
            mapped + surfaceClusterOffset,
            fallbackSurfaceClusters.data(),
            static_cast<size_t>(surfaceClusterBytes));
    }
    m_uploadWriteOffset = endOffset;
    m_stats.stagedFacesLastFrame = static_cast<uint32_t>(snapshot.faces.size());
    m_stats.stagedRangesLastFrame = snapshot.rangeCount;
    m_stats.stagedRangeTableCapacityLastFrame = static_cast<uint32_t>(snapshot.ranges.size());
    m_stats.stagedDrawCommandsLastFrame = snapshot.drawCommandCount;
    m_stats.stagedSurfaceRecordsLastFrame = static_cast<uint32_t>(fallbackSurfaceRecords.size());
    m_stats.stagedSurfaceClustersLastFrame = static_cast<uint32_t>(fallbackSurfaceClusters.size());
    m_stats.stagedRangeCopyRegionsLastFrame = rangeBytes > 0 ? 1u : 0u;
    m_stats.stagedDrawCopyRegionsLastFrame = drawArgsBytes > 0 ? 1u : 0u;
    m_stats.stagedSurfaceRecordCopyRegionsLastFrame = surfaceRecordBytes > 0 ? 1u : 0u;
    m_stats.stagedSurfaceClusterCopyRegionsLastFrame = surfaceClusterBytes > 0 ? 1u : 0u;
    m_stats.fullRangeTableUploadLastFrame = true;
    m_stats.fullDrawArgsUploadLastFrame = true;
    m_stats.fullSurfaceRecordUploadLastFrame = true;
    m_stats.fullSurfaceClusterUploadLastFrame = true;
    m_stats.stagedCandidateBricksLastFrame = snapshot.candidateBricks;
    m_stats.stagedVisibleBricksLastFrame = snapshot.visibleBricks;
    m_stats.stagedCulledBricksLastFrame = snapshot.culledBricks;
    m_stats.stagedBytesLastFrame = endOffset - faceOffset;
    m_stats.stagedDirtyPayloadBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
    m_stats.pendingDirtyBricksLastFrame = static_cast<uint32_t>(snapshot.dirtyBricks.size());
    m_stats.pendingRemovedBricksLastFrame = static_cast<uint32_t>(snapshot.removedBricks.size());

    if (outTicket) {
        outTicket->valid = true;
        outTicket->ringSlot = m_activeUploadSlot;
        outTicket->hasUploadWriteOffsetRollback = true;
        outTicket->uploadWriteOffsetBeforeStage = uploadWriteOffsetBeforeStage;
        outTicket->faceUploadOffset = faceOffset;
        outTicket->rangeUploadOffset = rangeOffset;
        outTicket->drawArgsUploadOffset = drawArgsOffset;
        outTicket->surfaceRecordUploadOffset = surfaceRecordOffset;
        outTicket->faceBytes = faceBytes;
        outTicket->rangeBytes = rangeBytes;
        outTicket->drawArgsBytes = drawArgsBytes;
        outTicket->surfaceRecordBytes = surfaceRecordBytes;
        if (surfaceClusterBytes > 0) {
            outTicket->surfaceClusterCopyRegions.push_back({
                surfaceClusterOffset,
                0u,
                surfaceClusterBytes
            });
        }
        outTicket->faceCount = static_cast<uint32_t>(snapshot.faces.size());
        outTicket->rangeCount = snapshot.rangeCount;
        outTicket->rangeTableCapacity = static_cast<uint32_t>(snapshot.ranges.size());
        outTicket->drawCommandCount = snapshot.drawCommandCount;
        outTicket->activeDrawCommandCount = snapshot.drawCommandCount;
        outTicket->serial = snapshot.serial;
        outTicket->candidateBricks = snapshot.candidateBricks;
        outTicket->visibleBricks = snapshot.visibleBricks;
        outTicket->culledBricks = snapshot.culledBricks;
        outTicket->rangeMirrorAfterCopy = snapshot.ranges;
        outTicket->drawArgsMirrorAfterCopy = snapshot.drawArgs;
        outTicket->surfaceRecordMirrorAfterCopy = std::move(fallbackSurfaceRecords);
        outTicket->surfaceClusterMirrorAfterCopy = std::move(fallbackSurfaceClusters);
        std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash> visibleDirtyCoords;
        visibleDirtyCoords.reserve(snapshot.dirtyBricks.size());
        for (const auto& dirty : snapshot.dirtyBricks) {
            if (dirty.serial <= snapshot.serial) {
                visibleDirtyCoords.insert(dirty.coord);
            }
        }
        outTicket->uploadedPayloadBricks.reserve(snapshot.drawBatches.size());
        for (const auto& batch : snapshot.drawBatches) {
            if (visibleDirtyCoords.find(batch.coord) != visibleDirtyCoords.end()) {
                outTicket->uploadedPayloadBricks.push_back(batch.coord);
            }
        }
        for (const auto& range : snapshot.ranges) {
            if (range.flags != 0u &&
                range.faceCount == 0u &&
                visibleDirtyCoords.find(range.coord) != visibleDirtyCoords.end()) {
                outTicket->uploadedPayloadBricks.push_back(range.coord);
            }
        }
        outTicket->removedBricks.reserve(snapshot.removedBricks.size());
        for (const auto& removed : snapshot.removedBricks) {
            if (removed.serial <= snapshot.serial) {
                outTicket->removedBricks.push_back(removed.coord);
            }
        }
    }
    return true;
}

bool SparseSurfaceGpuResources::EmitCopy(
    ID3D12GraphicsCommandList* commandList,
    const SparseSurfaceUploadTicket& ticket)
{
    auto restoreStagedStateFromTicket = [&]() {
        if (ticket.hasUploadWriteOffsetRollback &&
            ticket.ringSlot == m_activeUploadSlot) {
            m_uploadWriteOffset = ticket.uploadWriteOffsetBeforeStage;
        }
        if (ticket.hasRangeAllocatorRollback) {
            m_faceRangeAllocator = ticket.rangeAllocatorBeforeStage;
            const auto& allocatorStats = m_faceRangeAllocator.GetStats();
            m_stats.allocatedFaceRanges = allocatorStats.allocationCount;
            m_stats.allocatedFaceCapacity = allocatorStats.allocatedCapacity;
            m_stats.freeFaceRanges = allocatorStats.freeRangeCount;
            m_stats.largestFreeFaceRange = allocatorStats.largestFreeRange;
            m_stats.pendingRetiredFaceRanges = allocatorStats.pendingRetiredRangeCount;
            m_stats.pendingRetiredFaceCapacity = allocatorStats.pendingRetiredCapacity;
            m_stats.faceRangeAllocationFailures = allocatorStats.allocationFailures;
        }
    };

    if (!m_stats.initialized || !commandList || !ticket.valid) {
        restoreStagedStateFromTicket();
        return false;
    }
    if (ticket.ringSlot >= m_config.uploadRingSlots) {
        restoreStagedStateFromTicket();
        return false;
    }
    ID3D12Resource* upload = m_uploadRing[ticket.ringSlot].GetResource();
    if (!upload ||
        !m_faceBuffer.GetResource() ||
        !m_rangeBuffer.GetResource() ||
        !m_drawArgsBuffer.GetResource() ||
        !m_surfaceRecordBuffer.GetResource() ||
        !m_surfaceClusterBuffer.GetResource()) {
        restoreStagedStateFromTicket();
        return false;
    }

    const uint64_t uploadSize = m_uploadRing[ticket.ringSlot].GetSize();
    const uint64_t faceBufferSize = m_faceBuffer.GetSize();
    const uint64_t rangeBufferSize = m_rangeBuffer.GetSize();
    const uint64_t drawArgsBufferSize = m_drawArgsBuffer.GetSize();
    const uint64_t surfaceRecordBufferSize = m_surfaceRecordBuffer.GetSize();
    const uint64_t surfaceClusterBufferSize = m_surfaceClusterBuffer.GetSize();
    auto failInvalidTicket = [&](const char* label) -> bool {
        spdlog::warn("SparseSurfaceGpuResources::EmitCopy rejected out-of-bounds {}", label);
        m_stats.uploadOverflowLastFrame = true;
        restoreStagedStateFromTicket();
        return false;
    };
    if (!IsSparseSurfaceGpuByteRangeInBounds(ticket.faceUploadOffset, ticket.faceBytes, uploadSize) ||
        !IsSparseSurfaceGpuByteRangeInBounds(0u, ticket.faceBytes, faceBufferSize)) {
        return failInvalidTicket("face upload range");
    }
    if (!IsSparseSurfaceGpuByteRangeInBounds(ticket.rangeUploadOffset, ticket.rangeBytes, uploadSize) ||
        !IsSparseSurfaceGpuByteRangeInBounds(0u, ticket.rangeBytes, rangeBufferSize)) {
        return failInvalidTicket("range upload range");
    }
    if (!IsSparseSurfaceGpuByteRangeInBounds(ticket.drawArgsUploadOffset, ticket.drawArgsBytes, uploadSize) ||
        !IsSparseSurfaceGpuByteRangeInBounds(0u, ticket.drawArgsBytes, drawArgsBufferSize)) {
        return failInvalidTicket("draw upload range");
    }
    if (!IsSparseSurfaceGpuByteRangeInBounds(ticket.surfaceRecordUploadOffset, ticket.surfaceRecordBytes, uploadSize) ||
        !IsSparseSurfaceGpuByteRangeInBounds(0u, ticket.surfaceRecordBytes, surfaceRecordBufferSize)) {
        return failInvalidTicket("surface record upload range");
    }
    for (const SparseSurfaceFaceCopyRegion& region : ticket.faceCopyRegions) {
        if (!IsSparseSurfaceGpuFaceCopyRegionInBounds(region, uploadSize, faceBufferSize)) {
            return failInvalidTicket("face copy region");
        }
    }
    auto validateBufferRegions = [&](
        const std::vector<SparseSurfaceBufferCopyRegion>& regions,
        uint64_t destSize,
        const char* label) -> bool {
        for (const SparseSurfaceBufferCopyRegion& region : regions) {
            if (!IsSparseSurfaceGpuBufferCopyRegionInBounds(region, uploadSize, destSize)) {
                return failInvalidTicket(label);
            }
        }
        return true;
    };
    if (!validateBufferRegions(ticket.rangeCopyRegions, rangeBufferSize, "range copy region") ||
        !validateBufferRegions(ticket.drawArgsCopyRegions, drawArgsBufferSize, "draw copy region") ||
        !validateBufferRegions(ticket.surfaceRecordCopyRegions, surfaceRecordBufferSize, "surface record copy region") ||
        !validateBufferRegions(ticket.surfaceClusterCopyRegions, surfaceClusterBufferSize, "surface cluster copy region")) {
        return false;
    }
    if (ticket.incrementalMetadataPatches) {
        const uint32_t drawArgsMirrorPatchSize =
            std::max(static_cast<uint32_t>(m_drawArgsMirror.size()), ticket.drawArgsMirrorSizeAfterPatch);
        const uint32_t surfaceRecordMirrorPatchSize =
            std::max(static_cast<uint32_t>(m_surfaceRecordMirror.size()), ticket.surfaceRecordMirrorSizeAfterPatch);
        const uint32_t surfaceClusterMirrorPatchSize =
            std::max(static_cast<uint32_t>(m_surfaceClusterMirror.size()), ticket.surfaceClusterMirrorSizeAfterPatch);
        for (const SparseSurfaceRangeMirrorPatch& patch : ticket.rangeMirrorPatches) {
            if (patch.index >= m_rangeMirror.size()) {
                return failInvalidTicket("range mirror patch");
            }
        }
        for (const SparseSurfaceDrawArgsMirrorPatch& patch : ticket.drawArgsMirrorPatches) {
            if (patch.index >= drawArgsMirrorPatchSize ||
                patch.index >= m_config.maxDrawCommands) {
                return failInvalidTicket("draw mirror patch");
            }
        }
        for (const SparseSurfaceRecordMirrorPatch& patch : ticket.surfaceRecordMirrorPatches) {
            if (patch.index >= surfaceRecordMirrorPatchSize ||
                patch.index >= m_config.maxDrawCommands) {
                return failInvalidTicket("surface record mirror patch");
            }
        }
        for (const SparseSurfaceClusterMirrorPatch& patch : ticket.surfaceClusterMirrorPatches) {
            if (patch.index >= surfaceClusterMirrorPatchSize ||
                patch.index >= m_config.maxDrawCommands) {
                return failInvalidTicket("surface cluster mirror patch");
            }
        }
        for (const SparseSurfaceDrawSlotAssign& assign : ticket.drawSlotAssignments) {
            const uint32_t occupiedSize =
                std::max(static_cast<uint32_t>(m_drawSlotOccupied.size()), ticket.drawSlotOccupiedSizeAfterPatch);
            if (assign.slot >= occupiedSize ||
                assign.slot >= m_config.maxDrawCommands) {
                return failInvalidTicket("draw slot assignment");
            }
        }
    }

    if (m_staticIaUploadPending) {
        ID3D12Resource* vertexUpload = m_vertexIdStreamUpload.GetResource();
        ID3D12Resource* indexUpload = m_indexStreamUpload.GetResource();
        ID3D12Resource* vertexBuffer = m_vertexIdStream.GetResource();
        ID3D12Resource* indexBuffer = m_indexStream.GetResource();
        if (!vertexUpload || !indexUpload || !vertexBuffer || !indexBuffer) {
            restoreStagedStateFromTicket();
            return false;
        }
        if (!IsSparseSurfaceGpuByteRangeInBounds(0u, m_stats.iaStreamVertexBytes, m_vertexIdStreamUpload.GetSize()) ||
            !IsSparseSurfaceGpuByteRangeInBounds(0u, m_stats.iaStreamVertexBytes, m_vertexIdStream.GetSize()) ||
            !IsSparseSurfaceGpuByteRangeInBounds(0u, m_stats.iaStreamIndexBytes, m_indexStreamUpload.GetSize()) ||
            !IsSparseSurfaceGpuByteRangeInBounds(0u, m_stats.iaStreamIndexBytes, m_indexStream.GetSize())) {
            return failInvalidTicket("static IA upload range");
        }

        m_vertexIdStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            vertexBuffer,
            0,
            vertexUpload,
            0,
            m_stats.iaStreamVertexBytes);
        m_vertexIdStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        m_indexStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            indexBuffer,
            0,
            indexUpload,
            0,
            m_stats.iaStreamIndexBytes);
        m_indexStream.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDEX_BUFFER);

        m_staticIaUploadPending = false;
        m_staticIaUploadComplete = true;
        m_staticIaUploadFence = m_currentFrameFenceValue;
        m_stats.iaStreamUploadPending = false;
        m_stats.iaStreamUploadRetireFence = m_staticIaUploadFence;
    }

    if (ticket.faceBytes > 0) {
        m_faceBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_faceBuffer.GetResource(),
            0,
            upload,
            ticket.faceUploadOffset,
            ticket.faceBytes);
        m_faceBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.faceCopyRegions.empty()) {
        m_faceBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceFaceCopyRegion& region : ticket.faceCopyRegions) {
            if (region.faceCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_faceBuffer.GetResource(),
                static_cast<uint64_t>(region.destFirstFace) * sizeof(Simulation::SparseSurfaceFace),
                upload,
                region.uploadOffset,
                static_cast<uint64_t>(region.faceCount) * sizeof(Simulation::SparseSurfaceFace));
        }
        m_faceBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (ticket.rangeBytes > 0) {
        m_rangeBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_rangeBuffer.GetResource(),
            0,
            upload,
            ticket.rangeUploadOffset,
            ticket.rangeBytes);
        m_rangeBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.rangeCopyRegions.empty()) {
        m_rangeBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.rangeCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_rangeBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_rangeBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (ticket.drawArgsBytes > 0) {
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_drawArgsBuffer.GetResource(),
            0,
            upload,
            ticket.drawArgsUploadOffset,
            ticket.drawArgsBytes);
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    } else if (!ticket.drawArgsCopyRegions.empty()) {
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.drawArgsCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_drawArgsBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    if (ticket.surfaceRecordBytes > 0) {
        m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            m_surfaceRecordBuffer.GetResource(),
            0,
            upload,
            ticket.surfaceRecordUploadOffset,
            ticket.surfaceRecordBytes);
        m_surfaceRecordBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else if (!ticket.surfaceRecordCopyRegions.empty()) {
        m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.surfaceRecordCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_surfaceRecordBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_surfaceRecordBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (!ticket.surfaceClusterCopyRegions.empty()) {
        m_surfaceClusterBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const SparseSurfaceBufferCopyRegion& region : ticket.surfaceClusterCopyRegions) {
            if (region.byteCount == 0) {
                continue;
            }
            commandList->CopyBufferRegion(
                m_surfaceClusterBuffer.GetResource(),
                region.destOffset,
                upload,
                region.uploadOffset,
                region.byteCount);
        }
        m_surfaceClusterBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    for (const Simulation::BrickCoord& coord : ticket.removedBricks) {
        m_payloadResidentCoords.erase(coord);
        m_payloadFaceMirrorByCoord.erase(coord);
    }
    for (const Simulation::BrickCoord& coord : ticket.uploadedPayloadBricks) {
        m_payloadResidentCoords.insert(coord);
    }
    for (const SparseSurfacePayloadMirrorUpdate& update : ticket.payloadMirrorUpdates) {
        if (update.faces.empty()) {
            m_payloadFaceMirrorByCoord.erase(update.coord);
        } else {
            m_payloadFaceMirrorByCoord[update.coord] = update.faces;
        }
    }
    if (!ticket.payloadOnly) {
        if (ticket.incrementalMetadataPatches) {
            if (ticket.drawArgsMirrorSizeAfterPatch > m_drawArgsMirror.size()) {
                m_drawArgsMirror.resize(ticket.drawArgsMirrorSizeAfterPatch);
            }
            if (ticket.drawSlotOccupiedSizeAfterPatch > m_drawSlotOccupied.size()) {
                m_drawSlotOccupied.resize(ticket.drawSlotOccupiedSizeAfterPatch, 0u);
            }
            if (ticket.surfaceRecordMirrorSizeAfterPatch > m_surfaceRecordMirror.size()) {
                m_surfaceRecordMirror.resize(ticket.surfaceRecordMirrorSizeAfterPatch);
            }
            if (ticket.surfaceClusterMirrorSizeAfterPatch > m_surfaceClusterMirror.size()) {
                m_surfaceClusterMirror.resize(ticket.surfaceClusterMirrorSizeAfterPatch);
            }
            for (const SparseSurfaceRangeMirrorPatch& patch : ticket.rangeMirrorPatches) {
                m_rangeMirror[patch.index] = patch.value;
            }
            for (const SparseSurfaceDrawArgsMirrorPatch& patch : ticket.drawArgsMirrorPatches) {
                m_drawArgsMirror[patch.index] = patch.value;
            }
            for (const SparseSurfaceRecordMirrorPatch& patch : ticket.surfaceRecordMirrorPatches) {
                m_surfaceRecordMirror[patch.index] = patch.value;
            }
            for (const SparseSurfaceClusterMirrorPatch& patch : ticket.surfaceClusterMirrorPatches) {
                m_surfaceClusterMirror[patch.index] = patch.value;
            }
            if (m_config.useStableDrawSlots) {
                for (const SparseSurfaceDrawSlotAssign& assign : ticket.drawSlotAssignments) {
                    m_drawSlotByCoord[assign.coord] = assign.slot;
                    if (assign.slot < m_drawSlotOccupied.size()) {
                        m_drawSlotOccupied[assign.slot] = 1u;
                    }
                    auto freeIt = std::find(
                        m_freeDrawSlots.begin(),
                        m_freeDrawSlots.end(),
                        assign.slot);
                    if (freeIt != m_freeDrawSlots.end()) {
                        m_freeDrawSlots.erase(freeIt);
                    }
                }
                for (const SparseSurfaceDrawSlotRetire& retire : ticket.drawSlotRetires) {
                    m_drawSlotByCoord.erase(retire.coord);
                    if (retire.slot < m_drawSlotOccupied.size() &&
                        m_drawSlotOccupied[retire.slot] != 0u) {
                        m_drawSlotOccupied[retire.slot] = 0u;
                        m_freeDrawSlots.push_back(retire.slot);
                    }
                }
            }
            RebuildSurfaceRecordLookup();
        } else {
            m_rangeMirror = ticket.rangeMirrorAfterCopy;
            m_drawArgsMirror = ticket.drawArgsMirrorAfterCopy;
            m_surfaceRecordMirror = ticket.surfaceRecordMirrorAfterCopy;
            m_surfaceClusterMirror = ticket.surfaceClusterMirrorAfterCopy;
            RebuildSurfaceRecordLookup();
            if (m_config.useStableDrawSlots) {
                m_drawSlotByCoord = ticket.drawSlotByCoordAfterCopy;
                m_drawSlotOccupied = ticket.drawSlotOccupiedAfterCopy;
                m_freeDrawSlots = ticket.freeDrawSlotsAfterCopy;
            }
        }
    }

    if (!ticket.payloadOnly) {
        m_stats.uploadedFaces = ticket.faceCount;
        m_stats.uploadedRanges = ticket.rangeCount;
        m_stats.uploadedRangeTableCapacity = ticket.rangeTableCapacity;
        m_stats.uploadedDrawCommands = ticket.drawCommandCount;
        m_stats.uploadedActiveDrawCommands = ticket.activeDrawCommandCount;
        m_stats.uploadedSurfaceRecords = static_cast<uint32_t>(m_surfaceRecordMirror.size());
        m_stats.uploadedSurfaceClusters = static_cast<uint32_t>(m_surfaceClusterMirror.size());
    }
    m_stats.uploadedSerial = ticket.serial;
    if (!ticket.payloadOnly) {
        m_stats.uploadedCandidateBricks = ticket.candidateBricks;
        m_stats.uploadedVisibleBricks = ticket.visibleBricks;
        m_stats.uploadedCulledBricks = ticket.culledBricks;
    }
    m_stats.residentPayloadBricks = static_cast<uint32_t>(m_payloadResidentCoords.size());
    m_stats.stableDrawSlotCapacity = static_cast<uint32_t>(m_drawSlotOccupied.size());
    m_stats.stableDrawFreeSlots = static_cast<uint32_t>(m_freeDrawSlots.size());
    return true;
}

bool SparseSurfaceGpuResources::BuildFallbackDrawArgsExcluding(
    ID3D12GraphicsCommandList* commandList,
    const std::unordered_set<Simulation::BrickCoord, Simulation::BrickCoordHash>& excludedCoords,
    uint32_t* outExcludedDrawSlots,
    uint32_t* outCommandCount)
{
    if (outExcludedDrawSlots) {
        *outExcludedDrawSlots = 0u;
    }
    if (outCommandCount) {
        *outCommandCount = 0u;
    }
    if (!commandList ||
        !m_stats.initialized ||
        !m_fallbackDrawArgsBuffer.GetResource() ||
        !m_fallbackDrawArgsUpload.GetResource() ||
        m_drawArgsMirror.empty()) {
        return false;
    }

    std::vector<Simulation::SparseSurfaceDrawArgs> fallback = m_drawArgsMirror;
    uint32_t excludedSlots = 0u;
    for (const Simulation::BrickCoord& coord : excludedCoords) {
        auto it = m_drawSlotByCoord.find(coord);
        if (it == m_drawSlotByCoord.end()) {
            continue;
        }
        const uint32_t drawSlot = it->second;
        if (drawSlot < fallback.size()) {
            fallback[drawSlot] = {};
            ++excludedSlots;
        }
    }

    const uint64_t bytes =
        static_cast<uint64_t>(fallback.size()) * sizeof(Simulation::SparseSurfaceDrawArgs);
    if (bytes == 0u || bytes > m_fallbackDrawArgsUpload.GetSize()) {
        return false;
    }
    if (auto* mapped = m_fallbackDrawArgsUpload.GetMappedData()) {
        std::memcpy(mapped, fallback.data(), static_cast<size_t>(bytes));
    } else {
        return false;
    }

    m_fallbackDrawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(
        m_fallbackDrawArgsBuffer.GetResource(), 0,
        m_fallbackDrawArgsUpload.GetResource(), 0,
        bytes);
    m_fallbackDrawArgsBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    if (outExcludedDrawSlots) {
        *outExcludedDrawSlots = excludedSlots;
    }
    if (outCommandCount) {
        *outCommandCount = static_cast<uint32_t>(fallback.size());
    }
    return true;
}

bool SparseSurfaceGpuResources::CopyFixedSlotFacesIntoCompactRanges(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* fixedSlotFaceBuffer,
    uint32_t fixedSlotFaceCapacity,
    const std::vector<std::pair<uint32_t, Simulation::BrickCoord>>& slotCoords,
    uint32_t* outCopiedTiles,
    uint32_t* outCopiedFaces)
{
    if (outCopiedTiles) {
        *outCopiedTiles = 0u;
    }
    if (outCopiedFaces) {
        *outCopiedFaces = 0u;
    }
    if (!commandList ||
        !fixedSlotFaceBuffer ||
        fixedSlotFaceCapacity == 0u ||
        !m_stats.initialized ||
        !m_faceBuffer.GetResource() ||
        slotCoords.empty()) {
        return false;
    }

    uint32_t copiedTiles = 0u;
    uint32_t copiedFaces = 0u;
    m_faceBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& item : slotCoords) {
        const uint32_t fixedSlot = item.first;
        const Simulation::BrickCoord& coord = item.second;
        SparseSurfaceGpuBrickDebugInfo info;
        if (!TryGetBrickDebugInfo(coord, &info) ||
            !info.surfaceRecordPresent ||
            info.surfaceRecordFaceCount == 0u ||
            info.surfaceRecordFaceCount > fixedSlotFaceCapacity) {
            continue;
        }

        const uint64_t srcFirstFace =
            static_cast<uint64_t>(fixedSlot) * static_cast<uint64_t>(fixedSlotFaceCapacity);
        const uint64_t dstFirstFace = info.surfaceRecordFirstFace;
        const uint64_t faceCount = info.surfaceRecordFaceCount;
        const uint64_t byteCount = faceCount * sizeof(Simulation::SparseSurfaceFace);
        commandList->CopyBufferRegion(
            m_faceBuffer.GetResource(),
            dstFirstFace * sizeof(Simulation::SparseSurfaceFace),
            fixedSlotFaceBuffer,
            srcFirstFace * sizeof(Simulation::SparseSurfaceFace),
            byteCount);
        ++copiedTiles;
        copiedFaces += static_cast<uint32_t>(faceCount);
    }
    m_faceBuffer.TransitionTo(
        commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (outCopiedTiles) {
        *outCopiedTiles = copiedTiles;
    }
    if (outCopiedFaces) {
        *outCopiedFaces = copiedFaces;
    }
    return copiedTiles > 0u;
}

bool SparseSurfaceGpuResources::DispatchGpuCull(
    ID3D12GraphicsCommandList* commandList,
    float cameraX,
    float cameraY,
    float cameraZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    float rightX,
    float rightY,
    float rightZ,
    float upX,
    float upY,
    float upZ,
    float fovYRadians,
    float aspectRatio,
    float maxDistance,
    float padding)
{
    if (!m_stats.initialized ||
        !commandList ||
        !m_config.useGpuCull ||
        !m_surfaceCullPipeline.IsValid() ||
        !m_surfaceRecordBuffer.GetResource() ||
        !m_surfaceClusterBuffer.GetResource() ||
        !m_drawArgsBuffer.GetResource() ||
        !m_drawCountBuffer.GetResource() ||
        !m_surfaceRecordBuffer.GetShaderVisibleSRV().IsValid() ||
        !m_surfaceClusterBuffer.GetShaderVisibleSRV().IsValid() ||
        !m_drawArgsBuffer.GetShaderVisibleUAV().IsValid() ||
        !m_drawCountBuffer.GetShaderVisibleUAV().IsValid()) {
        return false;
    }

    const uint32_t recordCount = m_stats.uploadedSurfaceRecords;
    const uint32_t clusterCount = m_stats.uploadedSurfaceClusters;
    m_stats.gpuCullCandidateRecordsLastFrame = recordCount;
    m_stats.gpuCullCandidateClustersLastFrame = clusterCount;
    m_stats.gpuCullMaxDrawCommands = m_config.maxDrawCommands;
    if (m_activeUploadSlot >= m_cullConstantUploads.size()) {
        return false;
    }

    struct CullConstants {
        float cameraPosition[4];
        float cameraForward[4];
        float cameraRight[4];
        float cameraUp[4];
        float params[4];
        float clusterParams[4];
    } constants = {};
    float sanitizedForwardX = forwardX;
    float sanitizedForwardY = forwardY;
    float sanitizedForwardZ = forwardZ;
    float sanitizedRightX = rightX;
    float sanitizedRightY = rightY;
    float sanitizedRightZ = rightZ;
    float sanitizedUpX = upX;
    float sanitizedUpY = upY;
    float sanitizedUpZ = upZ;
    SanitizeDirection(
        &sanitizedForwardX,
        &sanitizedForwardY,
        &sanitizedForwardZ,
        0.0f,
        0.0f,
        1.0f);
    SanitizeDirection(
        &sanitizedRightX,
        &sanitizedRightY,
        &sanitizedRightZ,
        1.0f,
        0.0f,
        0.0f);
    SanitizeDirection(
        &sanitizedUpX,
        &sanitizedUpY,
        &sanitizedUpZ,
        0.0f,
        1.0f,
        0.0f);

    constants.cameraPosition[0] = FiniteOr(cameraX, 0.0f);
    constants.cameraPosition[1] = FiniteOr(cameraY, 0.0f);
    constants.cameraPosition[2] = FiniteOr(cameraZ, 0.0f);
    constants.cameraPosition[3] = ClampFinite(fovYRadians, 1.0471976f, 0.05f, 3.0f);
    constants.cameraForward[0] = sanitizedForwardX;
    constants.cameraForward[1] = sanitizedForwardY;
    constants.cameraForward[2] = sanitizedForwardZ;
    constants.cameraForward[3] = ClampFinite(aspectRatio, 1.7777778f, 0.1f, 10.0f);
    constants.cameraRight[0] = sanitizedRightX;
    constants.cameraRight[1] = sanitizedRightY;
    constants.cameraRight[2] = sanitizedRightZ;
    constants.cameraRight[3] = 0.0f;
    constants.cameraUp[0] = sanitizedUpX;
    constants.cameraUp[1] = sanitizedUpY;
    constants.cameraUp[2] = sanitizedUpZ;
    constants.cameraUp[3] = 0.0f;
    constants.params[0] = static_cast<float>(recordCount);
    constants.params[1] = static_cast<float>(m_config.maxDrawCommands);
    constants.params[2] = ClampFinite(maxDistance, 16384.0f, 0.0f, 10000000.0f);
    constants.params[3] = ClampFinite(padding, 0.0f, 0.0f, 4096.0f);
    constants.clusterParams[0] = static_cast<float>(m_config.surfaceClusterFastAcceptMaxRecords);
    constants.clusterParams[1] = static_cast<float>(m_config.surfaceClusterFastAcceptMaxFaces);
    static_assert(sizeof(CullConstants) <= 256u);
    if (void* mapped = m_cullConstantUploads[m_activeUploadSlot].GetMappedData()) {
        std::memcpy(mapped, &constants, sizeof(constants));
    }

    if (!m_heapManager) {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = { m_heapManager->GetShaderVisibleCbvSrvUavHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    m_surfaceRecordBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_surfaceClusterBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT clearValues[4] = {0u, 0u, 0u, 0u};
    commandList->ClearUnorderedAccessViewUint(
        m_drawCountBuffer.GetShaderVisibleUAV().gpu,
        m_drawCountBuffer.GetStagingUAV().cpu,
        m_drawCountBuffer.GetResource(),
        clearValues,
        0,
        nullptr);

    D3D12_RESOURCE_BARRIER clearStatsBarrier = {};
    clearStatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    clearStatsBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    clearStatsBarrier.UAV.pResource = m_drawCountBuffer.GetResource();
    commandList->ResourceBarrier(1, &clearStatsBarrier);

    if (recordCount > 0u && clusterCount > 0u) {
        m_surfaceCullPipeline.Bind(commandList);
        m_surfaceCullPipeline.SetRootConstantBufferView(
            commandList,
            0,
            m_cullConstantUploads[m_activeUploadSlot].GetGPUVirtualAddress());
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            1,
            m_surfaceRecordBuffer.GetShaderVisibleSRV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            2,
            m_surfaceClusterBuffer.GetShaderVisibleSRV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            3,
            m_drawArgsBuffer.GetShaderVisibleUAV().gpu);
        m_surfaceCullPipeline.SetRootDescriptorTable(
            commandList,
            4,
            m_drawCountBuffer.GetShaderVisibleUAV().gpu);

        const uint32_t groupCount = clusterCount;
        m_surfaceCullPipeline.Dispatch(commandList, groupCount, 1u, 1u);
        m_stats.gpuCullDispatchesLastFrame = 1u;
    }

    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[0].UAV.pResource = m_drawArgsBuffer.GetResource();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[1].UAV.pResource = m_drawCountBuffer.GetResource();
    commandList->ResourceBarrier(2, uavBarriers);

    m_drawArgsBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    return true;
}

void SparseSurfaceGpuResources::QueueGpuCullStatsReadback(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!m_stats.initialized ||
        !commandList ||
        !m_config.useGpuCull ||
        !m_drawCountBuffer.GetResource()) {
        return;
    }
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_cullStatsReadback.size());
    if (m_cullStatsReadbackPending[slot] || !m_cullStatsReadback[slot].GetResource()) {
        return;
    }

    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(
        m_cullStatsReadback[slot].GetResource(),
        0,
        m_drawCountBuffer.GetResource(),
        0,
        sizeof(uint32_t) * kCullStatsUintCount);
    m_drawCountBuffer.TransitionTo(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_cullStatsReadbackPending[slot] = true;
    m_cullStatsReadbackQueuedFrames[slot] = frameIndex;
    ++m_stats.gpuCullStatsReadbacksQueued;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
}

bool SparseSurfaceGpuResources::RetireGpuCullStatsReadback(uint32_t frameIndex)
{
    if (!m_stats.initialized || m_cullStatsReadback.empty()) {
        return false;
    }
    const uint32_t slot = frameIndex % static_cast<uint32_t>(m_cullStatsReadback.size());
    if (!m_cullStatsReadbackPending[slot] || !m_cullStatsReadback[slot].GetResource()) {
        return false;
    }
    if (!IsSparseSurfaceCullStatsReadbackRetirable(
            m_cullStatsReadbackPending[slot],
            m_cullStatsReadbackQueuedFrames[slot],
            frameIndex)) {
        return false;
    }

    const uint32_t* mapped = static_cast<const uint32_t*>(m_cullStatsReadback[slot].Map());
    if (!mapped) {
        return false;
    }
    m_stats.gpuCullAcceptedDraws = mapped[0];
    m_stats.gpuCullRejectedInvalid = mapped[1];
    m_stats.gpuCullRejectedDistance = mapped[2];
    m_stats.gpuCullRejectedFrustum = mapped[3];
    m_stats.gpuCullOverflow = mapped[4];
    m_stats.gpuCullCandidateRecordsLastFrame = mapped[5];
    m_stats.gpuCullMaxDrawCommands = mapped[6];
    m_stats.gpuCullRejectedClusters = mapped[7];
    m_stats.gpuCullFastAcceptedClusterRecords = mapped[8];
    m_stats.gpuCullAcceptedClusterDraws = mapped[9];
    m_stats.gpuCullAcceptedRecordDraws = mapped[10];
    m_stats.gpuCullRejectedBackface = mapped[11];
    m_stats.gpuCullStatsValid = true;
    m_cullStatsReadback[slot].Unmap();
    m_cullStatsReadbackPending[slot] = false;
    m_cullStatsReadbackQueuedFrames[slot] = 0u;
    ++m_stats.gpuCullStatsReadbacksRetired;
    uint32_t pendingReadbacks = 0;
    for (bool pending : m_cullStatsReadbackPending) {
        pendingReadbacks += pending ? 1u : 0u;
    }
    m_stats.gpuCullStatsReadbackPending = pendingReadbacks;
    return true;
}

} // namespace VENPOD::Graphics
