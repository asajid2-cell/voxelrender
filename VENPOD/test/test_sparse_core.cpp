#include "Graphics/FarVoxelOctree.h"
#include "Graphics/SparseSurfaceGpuResources.h"
#include "Graphics/SparseVoxelGpuResources.h"
#include "Graphics/VoxelRenderBackend.h"
#include "Simulation/SparseBrickPool.h"
#include "Simulation/SparseBrickRequestPlanner.h"
#include "Simulation/SparseCharacterController.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseCollision.h"
#include "Simulation/SparseEditStore.h"
#include "Simulation/SparsePagePublishQueue.h"
#include "Simulation/SparsePageTable.h"
#include "Simulation/SparseRuntimeBudget.h"
#include "Simulation/SparseSurfaceCache.h"
#include "Simulation/SparseSurfaceExtractor.h"
#include "Simulation/SparseSurfaceRangeAllocator.h"
#include "Simulation/TerrainConstants.h"
#include "Simulation/SparseTerrainGenerator.h"
#include "Simulation/SparseVoxelTypes.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Simulation/VoxelWorld.h"
#include "Utils/BitPacking.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

using VENPOD::Graphics::ParseVoxelRenderBackend;
using VENPOD::Graphics::SparseSurfaceIaStreamSizing;
using VENPOD::Graphics::ToString;
using VENPOD::Graphics::TryBuildSparseSurfaceIaStreamSizing;
using VENPOD::Graphics::VoxelRenderBackend;
using namespace VENPOD::Simulation;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename T>
void WriteTestBinary(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void TestBackendParsing() {
    Check(ParseVoxelRenderBackend("") == VoxelRenderBackend::DenseLegacy, "empty backend defaults dense");
    Check(ParseVoxelRenderBackend("dense") == VoxelRenderBackend::DenseLegacy, "dense backend parse");
    Check(ParseVoxelRenderBackend("sparse") == VoxelRenderBackend::SparseBrick, "sparse backend parse");
    Check(ParseVoxelRenderBackend("sparse-brick") == VoxelRenderBackend::SparseBrick, "sparse-brick backend parse");
    Check(std::string(ToString(VoxelRenderBackend::SparseBrick)) == "sparse-brick", "sparse backend string");
}

void TestVoxelWorldRaycastReadbackLifecycle() {
    Check(!IsVoxelRaycastReadbackRetirable(InvalidVoxelRaycastReadbackFrame(), 10u),
        "dense raycast readback rejects unqueued slot");
    Check(!IsVoxelRaycastReadbackRetirable(10u, 10u),
        "dense raycast readback rejects same-frame retire");
    Check(IsVoxelRaycastReadbackRetirable(10u, 13u),
        "dense raycast readback accepts older queued frame");
    Check(IsVoxelRaycastReadbackRetirable(std::numeric_limits<uint32_t>::max() - 1u, 0u),
        "dense raycast readback handles frame wrap without using invalid sentinel");

    const uint32_t packedBits = 0xDEADBEEFu;
    float packedAsFloat = 0.0f;
    std::memcpy(&packedAsFloat, &packedBits, sizeof(packedBits));
    Check(DecodeVoxelRaycastPackedWord(packedAsFloat) == packedBits,
        "dense raycast readback decodes packed word without float aliasing");
}

void TestFarVoxelOctreeResidencyMetadata() {
    VENPOD::Graphics::FarVoxelOctreeStats empty;
    auto metadata = VENPOD::Graphics::BuildFarVoxelOctreeResidencyMetadata(empty, true);
    Check(metadata.uploadCoverageRatio == 0.0f, "far octree empty upload coverage is zero");
    Check(metadata.pageCoverageRatio == 0.0f, "far octree empty page coverage is zero");
    Check(!metadata.ready, "far octree empty stats are not ready");

    VENPOD::Graphics::FarVoxelOctreeStats partial;
    partial.nodeCount = 10;
    partial.pageCount = 4;
    partial.pageIndexCount = 8;
    partial.gpuUploadBytesUploaded = 512;
    partial.gpuUploadBytesTotal = 1024;
    metadata = VENPOD::Graphics::BuildFarVoxelOctreeResidencyMetadata(partial, true);
    Check(metadata.uploadCoverageRatio > 0.49f && metadata.uploadCoverageRatio < 0.51f,
        "far octree partial upload coverage reports ratio");
    Check(metadata.pageCoverageRatio > 0.49f && metadata.pageCoverageRatio < 0.51f,
        "far octree partial page coverage reports ratio");
    Check(!metadata.ready, "far octree partial upload is not ready");

    VENPOD::Graphics::FarVoxelOctreeStats complete = partial;
    complete.pageCount = 8;
    complete.gpuUploadBytesUploaded = 2048;
    complete.gpuUploadBytesTotal = 1024;
    metadata = VENPOD::Graphics::BuildFarVoxelOctreeResidencyMetadata(complete, true);
    Check(metadata.uploadCoverageRatio == 1.0f, "far octree upload coverage clamps to one");
    Check(metadata.pageCoverageRatio == 1.0f, "far octree page coverage reaches one");
    Check(metadata.ready, "far octree complete stats and valid buffers are ready");

    metadata = VENPOD::Graphics::BuildFarVoxelOctreeResidencyMetadata(complete, false);
    Check(!metadata.ready, "far octree complete stats without GPU buffers are not ready");

    VENPOD::Graphics::FarVoxelOctreeConfig invalidPageSize;
    invalidPageSize.pageSize = std::numeric_limits<float>::quiet_NaN();
    Check(!VENPOD::Graphics::ValidateFarVoxelOctreeConfigForBuild(invalidPageSize),
        "far octree build config rejects non-finite page size");

    VENPOD::Graphics::FarVoxelOctreeConfig hugePageOrigin;
    hugePageOrigin.pageRadius = 2;
    hugePageOrigin.pageSize = 2.0e9f;
    Check(!VENPOD::Graphics::ValidateFarVoxelOctreeConfigForBuild(hugePageOrigin),
        "far octree build config rejects page origins outside int32 range");

    VENPOD::Graphics::FarVoxelOctreeConfig excessiveDepth;
    excessiveDepth.maxDepth = 64;
    Check(!VENPOD::Graphics::ValidateFarVoxelOctreeConfigForBuild(excessiveDepth),
        "far octree build config rejects excessive max depth");
}

void TestSparseSurfaceIaStreamSizing() {
    SparseSurfaceIaStreamSizing sizing;
    Check(!TryBuildSparseSurfaceIaStreamSizing(0u, sizing),
        "surface IA sizing rejects zero faces");

    Check(TryBuildSparseSurfaceIaStreamSizing(1u, sizing),
        "surface IA sizing accepts one face");
    Check(sizing.vertexCount == 4u, "surface IA one face vertex count");
    Check(sizing.indexCount == 6u, "surface IA one face index count");
    Check(sizing.vertexBytes == 16u, "surface IA one face vertex bytes");
    Check(sizing.indexBytes == 24u, "surface IA one face index bytes");

    constexpr uint32_t maxIndexViewFaces =
        std::numeric_limits<uint32_t>::max() / (6u * static_cast<uint32_t>(sizeof(uint32_t)));
    Check(TryBuildSparseSurfaceIaStreamSizing(maxIndexViewFaces, sizing),
        "surface IA sizing accepts largest 32-bit index-view byte capacity");
    Check(sizing.indexBytes <= std::numeric_limits<uint32_t>::max(),
        "surface IA accepted index bytes fit D3D12 view size");
    Check(sizing.vertexBytes <= std::numeric_limits<uint32_t>::max(),
        "surface IA accepted vertex bytes fit D3D12 view size");

    Check(!TryBuildSparseSurfaceIaStreamSizing(maxIndexViewFaces + 1u, sizing),
        "surface IA sizing rejects index view byte overflow");
}

void TestSparseSurfaceGpuConfigValidation() {
    VENPOD::Graphics::SparseSurfaceGpuConfig config;
    Check(VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(config),
        "sparse surface GPU default config validates");

    VENPOD::Graphics::SparseSurfaceGpuConfig zeroFaces = config;
    zeroFaces.maxFaces = 0u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(zeroFaces),
        "sparse surface GPU config rejects zero face capacity");

    VENPOD::Graphics::SparseSurfaceGpuConfig iaOverflow = config;
    iaOverflow.maxFaces =
        std::numeric_limits<uint32_t>::max() / (6u * static_cast<uint32_t>(sizeof(uint32_t))) + 1u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(iaOverflow),
        "sparse surface GPU config rejects IA stream view overflow");

    VENPOD::Graphics::SparseSurfaceGpuConfig hugeFaceCapacity = config;
    hugeFaceCapacity.maxFaces = (1u << 23) + 1u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(hugeFaceCapacity),
        "sparse surface GPU config rejects excessive face capacity before allocation");

    VENPOD::Graphics::SparseSurfaceGpuConfig hugeRangeCapacity = config;
    hugeRangeCapacity.maxBrickRanges = (1u << 21);
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(hugeRangeCapacity),
        "sparse surface GPU config rejects excessive range-table capacity before allocation");

    VENPOD::Graphics::SparseSurfaceGpuConfig oversizedRangeToDrawRatio = config;
    oversizedRangeToDrawRatio.maxDrawCommands = 1024u;
    oversizedRangeToDrawRatio.maxBrickRanges = 4096u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(oversizedRangeToDrawRatio),
        "sparse surface GPU config rejects fixed range tables beyond draw-command capacity");

    VENPOD::Graphics::SparseSurfaceGpuConfig nonPowerOfTwoRanges = config;
    nonPowerOfTwoRanges.maxBrickRanges = 12345u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(nonPowerOfTwoRanges),
        "sparse surface GPU config rejects fixed non-power-of-two range table capacity");

    VENPOD::Graphics::SparseSurfaceGpuConfig tooManyUploadSlots = config;
    tooManyUploadSlots.uploadRingSlots = 4u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(tooManyUploadSlots),
        "sparse surface GPU config rejects upload ring slot overflow");

    VENPOD::Graphics::SparseSurfaceGpuConfig zeroUploadBytes = config;
    zeroUploadBytes.uploadBytesPerSlot = 0u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(zeroUploadBytes),
        "sparse surface GPU config rejects zero upload slot bytes");

    VENPOD::Graphics::SparseSurfaceGpuConfig hugeUploadBytes = config;
    hugeUploadBytes.uploadBytesPerSlot = 512u * 1024u * 1024u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(hugeUploadBytes),
        "sparse surface GPU config rejects excessive upload slot bytes before allocation");

    VENPOD::Graphics::SparseSurfaceGpuConfig tooManyDrawCommands = config;
    tooManyDrawCommands.maxDrawCommands = 65536u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(tooManyDrawCommands),
        "sparse surface GPU config rejects cull dispatch group overflow");

    VENPOD::Graphics::SparseSurfaceGpuConfig oversizedCluster = config;
    oversizedCluster.surfaceRecordsPerCluster = 65u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(oversizedCluster),
        "sparse surface GPU config rejects oversized surface clusters");

    VENPOD::Graphics::SparseSurfaceGpuConfig oversizedClusterExtent = config;
    oversizedClusterExtent.surfaceClusterMaxExtentVoxels = 4097u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(oversizedClusterExtent),
        "sparse surface GPU config rejects oversized surface-cluster extent");

    VENPOD::Graphics::SparseSurfaceGpuConfig disabledClusterExtent = config;
    disabledClusterExtent.surfaceClusterMaxExtentVoxels = 0u;
    Check(VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(disabledClusterExtent),
        "sparse surface GPU config preserves count-only cluster extent mode");

    VENPOD::Graphics::SparseSurfaceGpuConfig excessiveFastAcceptRecords = config;
    excessiveFastAcceptRecords.surfaceClusterFastAcceptMaxRecords =
        excessiveFastAcceptRecords.surfaceRecordsPerCluster + 1u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(excessiveFastAcceptRecords),
        "sparse surface GPU config rejects fast-accept record count beyond cluster size");

    VENPOD::Graphics::SparseSurfaceGpuConfig excessivePayloadCopyFaces = config;
    excessivePayloadCopyFaces.maxPayloadCopyFacesPerFrame = config.maxFaces + 1u;
    Check(!VENPOD::Graphics::ValidateSparseSurfaceGpuConfigForStats(excessivePayloadCopyFaces),
        "sparse surface GPU config rejects payload copy face budget beyond capacity");
}

void TestSparseSurfaceGpuCopyRangeValidation() {
    Check(VENPOD::Graphics::IsSparseSurfaceGpuByteRangeInBounds(8u, 16u, 24u),
        "sparse surface GPU copy range accepts exact end");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuByteRangeInBounds(9u, 16u, 24u),
        "sparse surface GPU copy range rejects end overflow");
    Check(VENPOD::Graphics::IsSparseSurfaceGpuByteRangeInBounds(24u, 0u, 24u),
        "sparse surface GPU copy range accepts zero-byte end cursor");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuByteRangeInBounds(25u, 0u, 24u),
        "sparse surface GPU copy range rejects zero-byte cursor past capacity");

    const uint64_t faceBytes = sizeof(VENPOD::Simulation::SparseSurfaceFace);
    VENPOD::Graphics::SparseSurfaceFaceCopyRegion faceRegion;
    faceRegion.uploadOffset = 16u;
    faceRegion.destFirstFace = 2u;
    faceRegion.faceCount = 3u;
    Check(VENPOD::Graphics::IsSparseSurfaceGpuFaceCopyRegionInBounds(
            faceRegion,
            16u + 3u * faceBytes,
            5u * faceBytes),
        "sparse surface GPU face copy validator accepts exact upload and destination bounds");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuFaceCopyRegionInBounds(
            faceRegion,
            16u + 3u * faceBytes - 1u,
            5u * faceBytes),
        "sparse surface GPU face copy validator rejects upload overflow");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuFaceCopyRegionInBounds(
            faceRegion,
            16u + 3u * faceBytes,
            5u * faceBytes - 1u),
        "sparse surface GPU face copy validator rejects destination overflow");

    VENPOD::Graphics::SparseSurfaceBufferCopyRegion bufferRegion;
    bufferRegion.uploadOffset = 64u;
    bufferRegion.destOffset = 128u;
    bufferRegion.byteCount = 256u;
    Check(VENPOD::Graphics::IsSparseSurfaceGpuBufferCopyRegionInBounds(
            bufferRegion,
            320u,
            384u),
        "sparse surface GPU metadata copy validator accepts exact bounds");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuBufferCopyRegionInBounds(
            bufferRegion,
            319u,
            384u),
        "sparse surface GPU metadata copy validator rejects upload overflow");
    Check(!VENPOD::Graphics::IsSparseSurfaceGpuBufferCopyRegionInBounds(
            bufferRegion,
            320u,
            383u),
        "sparse surface GPU metadata copy validator rejects destination overflow");

    Check(!VENPOD::Graphics::IsSparseSurfaceCullStatsReadbackRetirable(false, 10u, 13u),
        "sparse surface cull stats readback rejects unqueued slot");
    Check(!VENPOD::Graphics::IsSparseSurfaceCullStatsReadbackRetirable(true, 10u, 10u),
        "sparse surface cull stats readback rejects same-frame retire");
    Check(VENPOD::Graphics::IsSparseSurfaceCullStatsReadbackRetirable(true, 10u, 13u),
        "sparse surface cull stats readback accepts older queued frame");
}

void TestSparseSurfaceGpuAbiLayout() {
    Check(std::is_standard_layout_v<SparseSurfaceDrawArgs>,
        "surface draw args remain standard-layout for GPU ABI");
    Check(sizeof(SparseSurfaceDrawArgs) == 20u, "surface draw args GPU ABI size");
    Check(alignof(SparseSurfaceDrawArgs) == alignof(uint32_t),
        "surface draw args GPU ABI alignment");
    Check(offsetof(SparseSurfaceDrawArgs, indexCountPerInstance) == 0u,
        "surface draw args indexCountPerInstance offset");
    Check(offsetof(SparseSurfaceDrawArgs, instanceCount) == 4u,
        "surface draw args instanceCount offset");
    Check(offsetof(SparseSurfaceDrawArgs, startIndexLocation) == 8u,
        "surface draw args startIndexLocation offset");
    Check(offsetof(SparseSurfaceDrawArgs, baseVertexLocation) == 12u,
        "surface draw args baseVertexLocation offset");
    Check(offsetof(SparseSurfaceDrawArgs, startInstanceLocation) == 16u,
        "surface draw args startInstanceLocation offset");

    Check(std::is_standard_layout_v<SparseSurfaceRecord>,
        "surface record remains standard-layout for GPU ABI");
    Check(sizeof(SparseSurfaceRecord) == 52u, "surface record GPU ABI size");
    Check(alignof(SparseSurfaceRecord) == alignof(uint32_t),
        "surface record GPU ABI alignment");
    Check(offsetof(SparseSurfaceRecord, coord) == 0u, "surface record coord offset");
    Check(offsetof(SparseSurfaceRecord, firstFace) == 12u, "surface record firstFace offset");
    Check(offsetof(SparseSurfaceRecord, faceCount) == 16u, "surface record faceCount offset");
    Check(offsetof(SparseSurfaceRecord, flags) == 20u, "surface record flags offset");
    Check(offsetof(SparseSurfaceRecord, generation) == 24u, "surface record generation offset");
    Check(offsetof(SparseSurfaceRecord, minX) == 28u, "surface record minX offset");
    Check(offsetof(SparseSurfaceRecord, minY) == 32u, "surface record minY offset");
    Check(offsetof(SparseSurfaceRecord, minZ) == 36u, "surface record minZ offset");
    Check(offsetof(SparseSurfaceRecord, maxX) == 40u, "surface record maxX offset");
    Check(offsetof(SparseSurfaceRecord, maxY) == 44u, "surface record maxY offset");
    Check(offsetof(SparseSurfaceRecord, maxZ) == 48u, "surface record maxZ offset");

    Check(std::is_standard_layout_v<SparseSurfaceClusterRecord>,
        "surface cluster record remains standard-layout for GPU ABI");
    Check(sizeof(SparseSurfaceClusterRecord) == 40u, "surface cluster record GPU ABI size");
    Check(alignof(SparseSurfaceClusterRecord) == alignof(uint32_t),
        "surface cluster record GPU ABI alignment");
    Check(offsetof(SparseSurfaceClusterRecord, minX) == 0u, "surface cluster minX offset");
    Check(offsetof(SparseSurfaceClusterRecord, minY) == 4u, "surface cluster minY offset");
    Check(offsetof(SparseSurfaceClusterRecord, minZ) == 8u, "surface cluster minZ offset");
    Check(offsetof(SparseSurfaceClusterRecord, firstRecord) == 12u,
        "surface cluster firstRecord offset");
    Check(offsetof(SparseSurfaceClusterRecord, maxX) == 16u, "surface cluster maxX offset");
    Check(offsetof(SparseSurfaceClusterRecord, maxY) == 20u, "surface cluster maxY offset");
    Check(offsetof(SparseSurfaceClusterRecord, maxZ) == 24u, "surface cluster maxZ offset");
    Check(offsetof(SparseSurfaceClusterRecord, recordCount) == 28u,
        "surface cluster recordCount offset");
    Check(offsetof(SparseSurfaceClusterRecord, faceCount) == 32u,
        "surface cluster faceCount offset");
    Check(offsetof(SparseSurfaceClusterRecord, flags) == 36u, "surface cluster flags offset");
}

void TestSparseVoxelGpuConfigValidation() {
    VENPOD::Graphics::SparseVoxelGpuConfig config;
    Check(VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(config),
        "sparse voxel GPU default config validates");

    VENPOD::Graphics::SparseVoxelGpuConfig hugeMidTiles = config;
    hugeMidTiles.midClipmapMaxTiles = std::numeric_limits<uint32_t>::max();
    Check(!VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(hugeMidTiles),
        "sparse voxel GPU config rejects mid clipmap lookup overflow");

    VENPOD::Graphics::SparseVoxelGpuConfig hugeVoxelBricks = config;
    hugeVoxelBricks.midVoxelClipmapMaxBricks = std::numeric_limits<uint32_t>::max();
    Check(!VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(hugeVoxelBricks),
        "sparse voxel GPU config rejects mid voxel lookup overflow");

    VENPOD::Graphics::SparseVoxelGpuConfig feedbackWrap = config;
    feedbackWrap.missFeedbackMaxRecords = std::numeric_limits<uint32_t>::max();
    Check(!VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(feedbackWrap),
        "sparse voxel GPU config rejects miss feedback count wrap");

    VENPOD::Graphics::SparseVoxelGpuConfig oversizedPhysics = config;
    oversizedPhysics.maxPhysicsWorkPackets = 2049u;
    Check(!VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(oversizedPhysics),
        "sparse voxel GPU config rejects physics packet count beyond shader dispatch cap");

    VENPOD::Graphics::SparseVoxelGpuConfig oversizedEditDeltas = config;
    oversizedEditDeltas.maxEditDeltas = 8193u;
    Check(!VENPOD::Graphics::ValidateSparseVoxelGpuConfigForStats(oversizedEditDeltas),
        "sparse voxel GPU config rejects edit deltas beyond shader dispatch cap");
}

void TestSparseVoxelGpuCopyRangeValidation() {
    Check(VENPOD::Graphics::IsSparseVoxelGpuByteRangeInBounds(16u, 32u, 48u),
        "sparse voxel GPU copy range accepts exact end");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuByteRangeInBounds(17u, 32u, 48u),
        "sparse voxel GPU copy range rejects end overflow");
    Check(VENPOD::Graphics::IsSparseVoxelGpuByteRangeInBounds(48u, 0u, 48u),
        "sparse voxel GPU copy range accepts zero-byte end cursor");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuByteRangeInBounds(49u, 0u, 48u),
        "sparse voxel GPU copy range rejects zero-byte cursor past capacity");

    Check(VENPOD::Graphics::IsSparseVoxelGpuCopyRangeInBounds(64u, 128u, 256u, 320u, 384u),
        "sparse voxel GPU copy validator accepts exact upload and destination bounds");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuCopyRangeInBounds(65u, 128u, 256u, 320u, 384u),
        "sparse voxel GPU copy validator rejects upload overflow");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuCopyRangeInBounds(64u, 129u, 256u, 320u, 384u),
        "sparse voxel GPU copy validator rejects destination overflow");

    VENPOD::Graphics::SparseBrickVoxelCopyRange brickRange;
    brickRange.uploadOffset = 4u;
    brickRange.brickPoolOffset = 12u;
    brickRange.bytes = 20u;
    Check(VENPOD::Graphics::IsSparseVoxelGpuBrickCopyRangeInBounds(brickRange, 24u, 32u),
        "sparse voxel GPU brick copy validator accepts exact partial range bounds");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuBrickCopyRangeInBounds(brickRange, 23u, 32u),
        "sparse voxel GPU brick copy validator rejects partial upload overflow");
    Check(!VENPOD::Graphics::IsSparseVoxelGpuBrickCopyRangeInBounds(brickRange, 24u, 31u),
        "sparse voxel GPU brick copy validator rejects partial destination overflow");
}

void TestCoordinateConversion() {
    struct Case {
        int world;
        int brick;
        uint32_t local;
    };

    const Case cases[] = {
        {0, 0, 0},
        {1, 0, 1},
        {15, 0, 15},
        {16, 1, 0},
        {17, 1, 1},
        {-1, -1, 15},
        {-2, -1, 14},
        {-16, -1, 0},
        {-17, -2, 15},
        {1024, 64, 0},
        {-1025, -65, 15},
        {std::numeric_limits<int32_t>::min(), -134217728, 0},
        {std::numeric_limits<int32_t>::min() + 1, -134217728, 1},
        {std::numeric_limits<int32_t>::max(), 134217727, 15},
    };

    for (const auto& c : cases) {
        Check(FloorDiv(c.world, SPARSE_BRICK_SIZE) == c.brick, "FloorDiv sparse brick case");
        Check(FloorMod(c.world, SPARSE_BRICK_SIZE) == c.local, "FloorMod sparse brick case");
    }

    BrickCoord brick = BrickCoord::FromWorldVoxel(-1, -16, -17);
    Check(brick == BrickCoord{-1, -1, -2}, "negative world voxel to brick coord");

    LocalVoxelCoord local = LocalVoxelFromWorld(-1, -16, -17);
    Check(local == LocalVoxelCoord{15, 0, 15}, "negative world voxel to local coord");

    BrickCoord extremeBrick = BrickCoord::FromWorldVoxel(
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min() + 1,
        std::numeric_limits<int32_t>::max());
    Check(extremeBrick == BrickCoord{-134217728, -134217728, 134217727},
        "extreme world voxel to brick coord does not overflow");

    LocalVoxelCoord extremeLocal = LocalVoxelFromWorld(
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min() + 1,
        std::numeric_limits<int32_t>::max());
    Check(extremeLocal == LocalVoxelCoord{0, 1, 15},
        "extreme world voxel to local coord does not overflow");

    int32_t checkedWorld = 0;
    Check(TryWorldVoxelFromBrickLocal(-134217728, 0, &checkedWorld) &&
          checkedWorld == std::numeric_limits<int32_t>::min(),
        "checked brick/local conversion accepts minimum world voxel");
    Check(TryWorldVoxelFromBrickLocal(134217727, 15, &checkedWorld) &&
          checkedWorld == std::numeric_limits<int32_t>::max(),
        "checked brick/local conversion accepts maximum world voxel");
    Check(!TryWorldVoxelFromBrickLocal(std::numeric_limits<int32_t>::max(), 0, &checkedWorld),
        "checked brick/local conversion rejects positive overflow");
    Check(!TryWorldVoxelFromBrickLocal(std::numeric_limits<int32_t>::min(), 15, &checkedWorld),
        "checked brick/local conversion rejects negative overflow");

    SparseBrushVoxelBounds brushBounds;
    Check(TryBuildSparseBrushVoxelBounds(10.25f, 20.5f, -30.75f, 2.2f, 0.5f, &brushBounds) &&
          brushBounds.startX == 5 &&
          brushBounds.startY == 15 &&
          brushBounds.startZ == -36 &&
          brushBounds.endX == 17 &&
          brushBounds.endY == 27 &&
          brushBounds.endZ == -24 &&
          brushBounds.radius == 2.2f &&
          brushBounds.strength == 0.5f,
        "sparse brush voxel bounds build stable checked dispatch volume");
    Check(TryBuildSparseBrushVoxelBounds(0.0f, 0.0f, 0.0f, 2048.0f, 2.0f, &brushBounds) &&
          brushBounds.radius == SPARSE_MAX_BRUSH_RADIUS &&
          brushBounds.strength == 1.0f,
        "sparse brush voxel bounds clamp radius and strength");
    Check(!TryBuildSparseBrushVoxelBounds(
            std::numeric_limits<float>::quiet_NaN(),
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            &brushBounds),
        "sparse brush voxel bounds reject non-finite position");
    Check(!TryBuildSparseBrushVoxelBounds(
            static_cast<float>(std::numeric_limits<int32_t>::max()),
            0.0f,
            0.0f,
            4.0f,
            1.0f,
            &brushBounds),
        "sparse brush voxel bounds reject positive world-coordinate overflow boundary");

    for (uint16_t i = 0; i < SPARSE_BRICK_VOXEL_COUNT; ++i) {
        LocalVoxelCoord decoded = LocalVoxelFromIndex(i);
        Check(LocalVoxelIndex(decoded) == i, "local voxel index roundtrip");
    }

    Check(IsValidLifecycleTransition(BrickLifecycleState::Missing, BrickLifecycleState::Requested),
        "lifecycle missing to requested valid");
    Check(!IsValidLifecycleTransition(BrickLifecycleState::Missing, BrickLifecycleState::Resident),
        "lifecycle missing to resident invalid");
    Check(!IsValidLifecycleTransition(BrickLifecycleState::Evicted, BrickLifecycleState::Resident),
        "lifecycle evicted to resident invalid");
}

void TestGpuPageEntryLayout() {
    Check(sizeof(BrickCoord) == 12, "BrickCoord GPU layout is 12 bytes");
    Check(offsetof(BrickCoord, x) == 0, "BrickCoord x offset");
    Check(offsetof(BrickCoord, y) == 4, "BrickCoord y offset");
    Check(offsetof(BrickCoord, z) == 8, "BrickCoord z offset");

    Check(sizeof(BrickPageEntry) == 32, "BrickPageEntry GPU layout is 32 bytes");
    Check(offsetof(BrickPageEntry, coord) == 0, "BrickPageEntry coord offset");
    Check(offsetof(BrickPageEntry, pageIndex) == 12, "BrickPageEntry pageIndex offset");
    Check(offsetof(BrickPageEntry, generation) == 16, "BrickPageEntry generation offset");
    Check(offsetof(BrickPageEntry, flags) == 20, "BrickPageEntry flags offset");
    Check(offsetof(BrickPageEntry, occupancyWord0) == 24, "BrickPageEntry occupancyWord0 offset");
    Check(offsetof(BrickPageEntry, occupancyWord1) == 28, "BrickPageEntry occupancyWord1 offset");

    Check(sizeof(SparsePhysicsWorkPacket) == 40, "SparsePhysicsWorkPacket GPU layout is 40 bytes");
    Check(offsetof(SparsePhysicsWorkPacket, coord) == 0, "SparsePhysicsWorkPacket coord offset");
    Check(offsetof(SparsePhysicsWorkPacket, packedRegionMin) == 12,
        "SparsePhysicsWorkPacket packedRegionMin offset");
    Check(offsetof(SparsePhysicsWorkPacket, packedRegionMax) == 16,
        "SparsePhysicsWorkPacket packedRegionMax offset");
    Check(offsetof(SparsePhysicsWorkPacket, materialMask) == 20,
        "SparsePhysicsWorkPacket materialMask offset");
    Check(offsetof(SparsePhysicsWorkPacket, priority) == 24,
        "SparsePhysicsWorkPacket priority offset");
    Check(offsetof(SparsePhysicsWorkPacket, generation) == 28,
        "SparsePhysicsWorkPacket generation offset");
    Check(offsetof(SparsePhysicsWorkPacket, expectedPageIndex) == 32,
        "SparsePhysicsWorkPacket expectedPageIndex offset");
    Check(offsetof(SparsePhysicsWorkPacket, expectedPageGeneration) == 36,
        "SparsePhysicsWorkPacket expectedPageGeneration offset");

    Check(sizeof(SparseEditDelta) == 24, "SparseEditDelta GPU layout is 24 bytes");
    Check(offsetof(SparseEditDelta, coord) == 0, "SparseEditDelta coord offset");
    Check(offsetof(SparseEditDelta, packedLocal) == 12, "SparseEditDelta packedLocal offset");
    Check(offsetof(SparseEditDelta, voxel) == 16, "SparseEditDelta voxel offset");
    Check(offsetof(SparseEditDelta, revision) == 20, "SparseEditDelta revision offset");

    Check(sizeof(SparseBrushFeedbackRecord) == 16, "SparseBrushFeedbackRecord GPU layout is 16 bytes");
    Check(offsetof(SparseBrushFeedbackRecord, worldX) == 0, "SparseBrushFeedbackRecord worldX offset");
    Check(offsetof(SparseBrushFeedbackRecord, worldY) == 4, "SparseBrushFeedbackRecord worldY offset");
    Check(offsetof(SparseBrushFeedbackRecord, worldZ) == 8, "SparseBrushFeedbackRecord worldZ offset");
    Check(offsetof(SparseBrushFeedbackRecord, voxel) == 12, "SparseBrushFeedbackRecord voxel offset");
    Check(SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT == 0xFFFFFFFFu,
        "SparseBrushFeedback missing-resident sentinel matches shader contract");
    Check(IsSparseBrushFeedbackMissingResident(
              SparseBrushFeedbackRecord{0, 0, 0, SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT}),
        "SparseBrushFeedback missing-resident sentinel detected");
    Check(CanApplySparseBrushFeedbackPayload(0u, 0u, false),
        "SparseBrushFeedback complete payload can apply");
    Check(!CanApplySparseBrushFeedbackPayload(1u, 0u, false),
        "SparseBrushFeedback missing resident blocks apply");
    Check(!CanApplySparseBrushFeedbackPayload(0u, 1u, false),
        "SparseBrushFeedback missing-resident hint record blocks apply");
    Check(!CanApplySparseBrushFeedbackPayload(1u, 0u, false),
        "SparseBrushFeedback missing-resident header count blocks apply");
    Check(!CanApplySparseBrushFeedbackPayload(0u, 0u, true),
        "SparseBrushFeedback overflow blocks partial apply");
    Check(!SparseBrushFeedbackPayloadOverflowed(8u, 8u, 0u),
        "SparseBrushFeedback exact-capacity payload is not overflowed");
    Check(SparseBrushFeedbackPayloadOverflowed(9u, 8u, 0u),
        "SparseBrushFeedback record count beyond capacity is overflowed");
    Check(SparseBrushFeedbackPayloadOverflowed(8u, 8u, 1u),
        "SparseBrushFeedback shader header overflow flag blocks apply");
    Check(!HasDuplicateSparseBrushFeedbackVoxels({
              SparseBrushFeedbackRecord{1, 2, 3, 4},
              SparseBrushFeedbackRecord{1, 2, 4, 5},
              SparseBrushFeedbackRecord{1, 2, 3, SPARSE_BRUSH_FEEDBACK_MISSING_RESIDENT}}),
        "SparseBrushFeedback duplicate detector ignores unique voxels and missing-resident hints");
    Check(HasDuplicateSparseBrushFeedbackVoxels({
              SparseBrushFeedbackRecord{1, 2, 3, 4},
              SparseBrushFeedbackRecord{-1, 2, 3, 5},
              SparseBrushFeedbackRecord{1, 2, 3, 6}}),
        "SparseBrushFeedback duplicate detector rejects repeated edit voxels");

    Check(sizeof(SparseEditDeltaRange) == 24, "SparseEditDeltaRange GPU layout is 24 bytes");
    Check(offsetof(SparseEditDeltaRange, coord) == 0, "SparseEditDeltaRange coord offset");
    Check(offsetof(SparseEditDeltaRange, firstDelta) == 12, "SparseEditDeltaRange firstDelta offset");
    Check(offsetof(SparseEditDeltaRange, deltaCount) == 16, "SparseEditDeltaRange deltaCount offset");
    Check(offsetof(SparseEditDeltaRange, latestRevision) == 20, "SparseEditDeltaRange latestRevision offset");

    Check(sizeof(SparsePhysicsPacketResult) == 80, "SparsePhysicsPacketResult GPU layout is 80 bytes");
    Check(SPARSE_PHYSICS_PACKET_STATUS_CONSUMED == 1u,
        "SparsePhysicsPacketResult consumed status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE == 2u,
        "SparsePhysicsPacketResult expected-page status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH == 4u,
        "SparsePhysicsPacketResult page-match status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE == 8u,
        "SparsePhysicsPacketResult page-stale status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL == 16u,
        "SparsePhysicsPacketResult proposal status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW == 32u,
        "SparsePhysicsPacketResult missing-below status ABI");
    Check(SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT == 64u,
        "SparsePhysicsPacketResult edit-delta status ABI");
    Check(offsetof(SparsePhysicsPacketResult, coord) == 0,
        "SparsePhysicsPacketResult coord offset");
    Check(offsetof(SparsePhysicsPacketResult, packetIndex) == 12,
        "SparsePhysicsPacketResult packetIndex offset");
    Check(offsetof(SparsePhysicsPacketResult, destinationCoord) == 16,
        "SparsePhysicsPacketResult destinationCoord offset");
    Check(offsetof(SparsePhysicsPacketResult, destinationFlags) == 28,
        "SparsePhysicsPacketResult destinationFlags offset");
    Check(offsetof(SparsePhysicsPacketResult, generation) == 32,
        "SparsePhysicsPacketResult generation offset");
    Check(offsetof(SparsePhysicsPacketResult, materialMask) == 36,
        "SparsePhysicsPacketResult materialMask offset");
    Check(offsetof(SparsePhysicsPacketResult, checksum) == 40,
        "SparsePhysicsPacketResult checksum offset");
    Check(offsetof(SparsePhysicsPacketResult, status) == 44,
        "SparsePhysicsPacketResult status offset");
    Check(offsetof(SparsePhysicsPacketResult, expectedPageIndex) == 48,
        "SparsePhysicsPacketResult expectedPageIndex offset");
    Check(offsetof(SparsePhysicsPacketResult, expectedPageGeneration) == 52,
        "SparsePhysicsPacketResult expectedPageGeneration offset");
    Check(offsetof(SparsePhysicsPacketResult, packedSourceLocal) == 56,
        "SparsePhysicsPacketResult packedSourceLocal offset");
    Check(offsetof(SparsePhysicsPacketResult, packedDestinationLocal) == 60,
        "SparsePhysicsPacketResult packedDestinationLocal offset");
    Check(offsetof(SparsePhysicsPacketResult, sourceVoxel) == 64,
        "SparsePhysicsPacketResult sourceVoxel offset");
    Check(offsetof(SparsePhysicsPacketResult, destinationVoxel) == 68,
        "SparsePhysicsPacketResult destinationVoxel offset");
    Check(offsetof(SparsePhysicsPacketResult, sourceRevision) == 72,
        "SparsePhysicsPacketResult sourceRevision offset");
    Check(offsetof(SparsePhysicsPacketResult, destinationRevision) == 76,
        "SparsePhysicsPacketResult destinationRevision offset");

    Check(HashBrickCoord32({0, 0, 0}) == 1253111735u, "hash origin matches shader contract");
    Check(HashBrickCoord32({1, -2, 3}) == 2804991279u, "hash mixed signed coord matches shader contract");
    Check((VENPOD::Utils::StateFlags::VisualSurface &
              (VENPOD::Utils::StateFlags::IsStatic |
               VENPOD::Utils::StateFlags::IsIgnited |
               VENPOD::Utils::StateFlags::HasMoved |
               VENPOD::Utils::StateFlags::LifeMask)) == 0,
        "visual surface state bit does not overlap physics/life state bits");
}

void TestPageTable() {
    SparsePageTable table(16);

    Check(!table.InsertOrAssign({9, 9, 9}, 6, 0, 7), "generation zero rejected");
    Check(table.InsertOrAssign({0, 0, 0}, 3, 1, 7), "insert origin page");
    Check(table.InsertOrAssign({-1, 2, -3}, 4, 2, 9), "insert negative page");

    uint32_t page = INVALID_BRICK_PAGE;
    uint32_t flags = 0;
    uint32_t generation = 0;
    Check(table.TryLookup({0, 0, 0}, &page, &flags, &generation), "lookup origin page");
    Check(page == 3 && flags == 7, "lookup origin page data");
    Check(generation == 1, "lookup origin generation");
    uint32_t entryIndex = UINT32_MAX;
    Check(table.TryGetEntryIndex({0, 0, 0}, &entryIndex), "page table entry index lookup");
    Check(entryIndex < table.Capacity(), "page table entry index in range");
    Check(table.TryLookup({-1, 2, -3}, &page, &flags, &generation), "lookup negative page");
    Check(page == 4 && flags == 9 && generation == 2, "lookup negative page data");
    Check(table.TryLookupExactGeneration({-1, 2, -3}, 2, &page, &flags), "exact generation lookup");
    Check(!table.TryLookupExactGeneration({-1, 2, -3}, 1, &page, &flags), "stale generation rejected");

    Check(table.InsertOrAssign({0, 0, 0}, 11, 3, 13), "assign existing page");
    Check(table.TryLookup({0, 0, 0}, &page, &flags, &generation), "lookup reassigned page");
    Check(page == 11 && flags == 13 && generation == 3, "reassigned page data");

    SparsePageTable saturatedTable(16);
    for (int32_t i = 0; i < 11; ++i) {
        Check(
            saturatedTable.InsertOrAssign({i, 0, 0}, static_cast<uint32_t>(i + 1), 1u, 0u),
            "page table accepts inserts up to load threshold");
    }
    Check(!saturatedTable.InsertOrAssign({99, 0, 0}, 99u, 1u, 0u),
        "page table rejects new inserts past load threshold");
    Check(saturatedTable.InsertOrAssign({3, 0, 0}, 77u, 2u, 5u),
        "page table allows existing entry update at load threshold");
    Check(saturatedTable.TryLookup({3, 0, 0}, &page, &flags, &generation),
        "lookup saturated table updated entry");
    Check(page == 77u && flags == 5u && generation == 2u,
        "saturated table update preserves new page data");

    Check(table.Remove({0, 0, 0}), "remove origin page");
    Check(!table.TryLookup({0, 0, 0}), "removed page missing");
    Check(table.TryLookup({-1, 2, -3}), "tombstone does not break probe chain");
}

void TestBrickPool() {
    SparseBrickPool invalidPool;
    Check(!invalidPool.Initialize(0, 16),
        "brick pool rejects zero page capacity");
    Check(!invalidPool.Initialize(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()),
        "brick pool rejects page table capacity validation overflow");

    SparseBrickPool pool;
    Check(pool.Initialize(4, 16), "brick pool initialize");
    Check(pool.MaxPages() == 4, "brick pool page count");
    Check(pool.FreePageCount() == 4, "brick pool initial free pages");
    auto validation = pool.ValidateInvariants();
    Check(validation.ok && validation.activeRecords == 0 && validation.freePages == 4,
        "brick pool validates after initialize");

    const BrickCoord a{0, 0, 0};
    const BrickCoord b{-1, 4, 2};
    uint32_t pageA = pool.AllocatePage(a);
    uint32_t pageB = pool.AllocatePage(b);

    Check(pageA != INVALID_BRICK_PAGE, "allocate page A");
    Check(pageB != INVALID_BRICK_PAGE, "allocate page B");
    Check(pageA != pageB, "allocated pages unique");
    Check(pool.ResidentCount() == 2, "tracked count after allocation");
    Check(!pool.IsResident(a), "allocated page A not visible before publish");
    Check(pool.GetState(a) == BrickLifecycleState::Requested, "allocated page starts requested");
    validation = pool.ValidateInvariants();
    Check(validation.ok && validation.activeRecords == 2 && validation.pageTableEntries == 0,
        "brick pool validates requested pages before publication");

    uint32_t lookup = INVALID_BRICK_PAGE;
    Check(pool.TryGetPage(b, &lookup) && lookup == pageB, "lookup pool page");
    Check(!pool.TryGetResidentPage(b, &lookup), "unpublished page not resident");

    Check(pool.MarkGeneratingCPU(a), "mark generating");
    Check(pool.MarkGeneratedCPU(a), "mark generated");
    Check(pool.QueueUpload(a), "queue upload");
    Check(pool.BeginUpload(a), "begin upload");
    Check(pool.PublishResident(a, 123, 0x55u, 0xAAu), "publish resident");
    Check(pool.IsResident(a), "page A resident after publish");
    Check(pool.TryGetResidentPage(a, &lookup) && lookup == pageA, "resident page lookup after publish");

    BrickResidentRecord recordA;
    Check(pool.GetRecord(a, &recordA), "get record A");
    Check(recordA.state == BrickLifecycleState::Resident, "record A resident state");
    Check(recordA.generation == 1, "record A first generation");

    uint32_t pageTablePage = INVALID_BRICK_PAGE;
    uint32_t pageTableFlags = 0;
    Check(pool.PageTable().TryLookupExactGeneration(a, recordA.generation, &pageTablePage, &pageTableFlags),
        "page table exact generation after publish");
    Check(pageTablePage == pageA && pageTableFlags == 123, "page table published data");
    validation = pool.ValidateInvariants();
    Check(validation.ok && validation.pageTableEntries == 1,
        "brick pool validates resident published page table entry");
    Check(validation.missingPublishedPageTableEntries == 0,
        "brick pool validates resident records have reverse page-table mapping");

    Check(pool.MarkDirty(a), "mark dirty should be callable");
    Check(pool.GetState(a) == BrickLifecycleState::DirtyCPU, "resident dirty transition");
    validation = pool.ValidateInvariants();
    Check(validation.ok && validation.pageTableEntries == 1,
        "brick pool allows dirty CPU record to retain old published page");
    Check(validation.missingPublishedPageTableEntries == 0,
        "brick pool validates dirty records keep reverse page-table mapping");
    Check(pool.QueueUpload(a), "queue dirty upload");
    Check(pool.BeginUpload(a), "begin dirty upload");
    validation = pool.ValidateInvariants();
    Check(validation.ok && validation.pageTableEntries == 1,
        "brick pool validates in-flight reupload keeps exact-generation page mapping");
    Check(pool.PublishResident(a, 124), "republish dirty resident");

    Check(pool.FreePage(a), "free page A");
    Check(!pool.IsResident(a), "page A no longer resident");
    Check(!pool.PageTable().TryLookup(a), "page table invalidated before reuse");
    Check(pool.FreePageCount() == 3, "free page count after free");
    validation = pool.ValidateInvariants();
    Check(validation.ok && validation.pageTableEntries == 0,
        "brick pool validates after eviction invalidates page table");

    const BrickCoord c{8, 8, 8};
    uint32_t pageC = pool.AllocatePage(c);
    Check(pageC != INVALID_BRICK_PAGE, "allocate page C");
    if (pageC == pageA) {
        BrickResidentRecord recordC;
        Check(pool.GetRecord(c, &recordC), "get record C");
        Check(recordC.generation == recordA.generation + 1u, "physical page reuse increments generation");
        Check(!pool.PageTable().TryLookupExactGeneration(c, recordA.generation),
            "old generation cannot resolve reused page");
    }
}

void TestTerrainGeneration() {
    SparseTerrainGenerator terrain(12345u);
    const BrickCoord coord{0, 0, 0};
    GeneratedSparseBrick a = terrain.GenerateBrick(coord);
    GeneratedSparseBrick b = terrain.GenerateBrick(coord);

    Check(a.voxels == b.voxels, "generated brick deterministic");
    Check(a.occupancyWord0 == b.occupancyWord0 && a.occupancyWord1 == b.occupancyWord1,
        "generated occupancy deterministic");

    for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
        for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
            for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                const int32_t worldX = coord.x * SPARSE_BRICK_SIZE + x;
                const int32_t worldY = coord.y * SPARSE_BRICK_SIZE + y;
                const int32_t worldZ = coord.z * SPARSE_BRICK_SIZE + z;
                const uint32_t expected = terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
                const uint32_t actual = a.voxels[LocalVoxelIndex({x, y, z})];
                Check(actual == expected, "brick voxel equals world sample");
            }
        }
    }

    const BrickCoord sampleCoords[] = {
        {14, 4, 14},
        {12, -2, 15},
        {-1, 0, -1},
        {0, -4, 0}
    };
    for (const BrickCoord& sampleCoord : sampleCoords) {
        const GeneratedSparseBrick generated = terrain.GenerateBrick(sampleCoord);
        for (uint8_t z = 0; z < SPARSE_BRICK_SIZE; ++z) {
            for (uint8_t y = 0; y < SPARSE_BRICK_SIZE; ++y) {
                for (uint8_t x = 0; x < SPARSE_BRICK_SIZE; ++x) {
                    int32_t worldX = 0;
                    int32_t worldY = 0;
                    int32_t worldZ = 0;
                    const bool validCoord =
                        TryWorldVoxelFromBrickLocal(sampleCoord.x, x, &worldX) &&
                        TryWorldVoxelFromBrickLocal(sampleCoord.y, y, &worldY) &&
                        TryWorldVoxelFromBrickLocal(sampleCoord.z, z, &worldZ);
                    Check(validCoord, "sample terrain test coord is representable");
                    if (!validCoord) {
                        continue;
                    }
                    const uint32_t expected = terrain.SampleGeneratedVoxel(worldX, worldY, worldZ);
                    const uint32_t columnExpected = terrain.SampleGeneratedVoxelWithColumn(
                        worldX,
                        worldY,
                        worldZ,
                        terrain.HeightAt(worldX, worldZ),
                        terrain.SurfaceReliefAt(worldX, worldZ, 4));
                    const uint32_t actual = generated.voxels[LocalVoxelIndex({x, y, z})];
                    Check(columnExpected == expected, "column-cached terrain sample equals authoritative sample");
                    Check(actual == expected, "optimized generated brick equals world sample");
                }
            }
        }
    }

    const BrickCoord right{1, 0, 0};
    GeneratedSparseBrick rightBrick = terrain.GenerateBrick(right);
    const uint32_t rightEdgeSample = terrain.SampleGeneratedVoxel(16, 0, 0);
    Check(rightBrick.voxels[LocalVoxelIndex({0, 0, 0})] == rightEdgeSample,
        "adjacent brick starts at correct world coordinate");

    GeneratedSparseBrick highAir = terrain.GenerateBrick({0, 1000, 0});
    Check((highAir.flags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) != 0,
        "very high brick classified empty");
    Check(highAir.occupancyWord0 == 0 && highAir.occupancyWord1 == 0,
        "empty brick has no occupancy bits");
    Check(terrain.IsDefinitelyEmptyBrick(BrickCoord::FromWorldVoxel(0, 384, 0)),
        "terrain empty-brick fast path accepts bricks above procedural height bound");
    Check(!terrain.IsDefinitelyEmptyBrick(BrickCoord::FromWorldVoxel(0, SEA_LEVEL_Y, 0)),
        "terrain empty-brick fast path keeps low and water-adjacent bricks resident-eligible");
    Check(!terrain.IsDefinitelyEmptyBrick({std::numeric_limits<int32_t>::max(), 1000, 0}),
        "terrain empty-brick fast path rejects overflowing X coord");

    GeneratedSparseBrick overflowGenerated = terrain.GenerateBrick(
        {std::numeric_limits<int32_t>::max(), 0, 0});
    Check((overflowGenerated.flags & static_cast<uint32_t>(BrickResidencyFlags::Empty)) != 0 &&
          overflowGenerated.occupancyWord0 == 0 &&
          overflowGenerated.occupancyWord1 == 0,
        "terrain generation rejects overflowing brick coord as empty fail-closed brick");

    GeneratedSparseBrick bedrock = terrain.GenerateBrick({0, -64, 0});
    Check((bedrock.flags & static_cast<uint32_t>(BrickResidencyFlags::Solid)) != 0,
        "deep brick classified solid");
    Check(VENPOD::Utils::UnpackMaterial(bedrock.voxels[0]) == VENPOD::Utils::Material::Bedrock,
        "deep brick contains bedrock");

    const int32_t sampleX = 96;
    const int32_t sampleZ = 96;
    const int32_t terrainSurfaceY = static_cast<int32_t>(std::floor(terrain.HeightAt(sampleX, sampleZ)));
    const int32_t coarseStep = 16;
    const uint32_t surfaceVoxel = terrain.SampleGeneratedSurfaceVoxel(
        sampleX,
        terrainSurfaceY,
        sampleZ,
        coarseStep);
    Check(VENPOD::Utils::UnpackMaterial(surfaceVoxel) != VENPOD::Utils::Material::Air,
        "visual surface sample keeps exposed terrain");

    const uint32_t interiorVoxel = terrain.SampleGeneratedSurfaceVoxel(
        sampleX,
        terrainSurfaceY - coarseStep * 3,
        sampleZ,
        coarseStep);
    Check(VENPOD::Utils::UnpackMaterial(interiorVoxel) == VENPOD::Utils::Material::Air,
        "visual surface sample suppresses coarse interior terrain");

    const uint32_t regularInteriorVoxel = terrain.SampleGeneratedVoxel(
        sampleX,
        terrainSurfaceY - coarseStep * 3,
        sampleZ);
    Check(VENPOD::Utils::UnpackMaterial(regularInteriorVoxel) != VENPOD::Utils::Material::Air,
        "authoritative generated terrain still retains interior volume");

    bool foundWaterBasin = false;
    int32_t waterBasinX = 0;
    int32_t waterBasinZ = 0;
    for (int32_t z = -256; z <= 256 && !foundWaterBasin; z += 16) {
        for (int32_t x = -256; x <= 256 && !foundWaterBasin; x += 16) {
            const float basinHeight = terrain.HeightAt(x, z);
            if (basinHeight >= static_cast<float>(SEA_LEVEL_Y - 4)) {
                continue;
            }
            const int32_t basinFloorY = static_cast<int32_t>(std::floor(basinHeight));
            const uint32_t basinFloorVoxel = terrain.SampleGeneratedVoxel(x, basinFloorY, z);
            Check(VENPOD::Utils::UnpackMaterial(basinFloorVoxel) != VENPOD::Utils::Material::Sand,
                "authoritative submerged terrain floor does not masquerade as beach sand");
            const uint32_t basinFloorSurface = terrain.SampleGeneratedSurfaceVoxel(x, basinFloorY, z, 1);
            Check(VENPOD::Utils::UnpackMaterial(basinFloorSurface) == VENPOD::Utils::Material::Air,
                "visual surface sample suppresses terrain faces owned by water");
            const uint32_t basinWaterSurface = terrain.SampleGeneratedSurfaceVoxel(x, SEA_LEVEL_Y, z, 1);
            Check(VENPOD::Utils::UnpackMaterial(basinWaterSurface) == VENPOD::Utils::Material::Water,
                "visual surface sample keeps the exposed water surface over basins");
            waterBasinX = x;
            waterBasinZ = z;
            foundWaterBasin = true;
        }
    }
    Check(foundWaterBasin, "terrain test found a deterministic below-sea basin");

    bool foundShallowSubmergedColumn = false;
    for (int32_t z = -512; z <= 512 && !foundShallowSubmergedColumn; z += 4) {
        for (int32_t x = -512; x <= 512 && !foundShallowSubmergedColumn; x += 4) {
            const float shallowHeight = terrain.HeightAt(x, z);
            if (shallowHeight < static_cast<float>(SEA_LEVEL_Y - 2) ||
                shallowHeight >= static_cast<float>(SEA_LEVEL_Y)) {
                continue;
            }
            const int32_t shallowFloorY = static_cast<int32_t>(std::floor(shallowHeight));
            const uint32_t shallowFloorVoxel = terrain.SampleGeneratedVoxel(x, shallowFloorY, z);
            Check(VENPOD::Utils::UnpackMaterial(shallowFloorVoxel) != VENPOD::Utils::Material::Air,
                "shallow submerged terrain keeps its solid floor below water");
            const uint32_t shallowSeaVoxel = terrain.SampleGeneratedVoxel(x, SEA_LEVEL_Y, z);
            Check(VENPOD::Utils::UnpackMaterial(shallowSeaVoxel) == VENPOD::Utils::Material::Water,
                "shallow below-sea columns are water-owned at sea level");
            const uint32_t shallowFloorSurface = terrain.SampleGeneratedSurfaceVoxel(x, shallowFloorY, z, 1);
            Check(VENPOD::Utils::UnpackMaterial(shallowFloorSurface) == VENPOD::Utils::Material::Air,
                "visual surface suppresses shallow submerged terrain floor");
            const uint32_t shallowWaterSurface = terrain.SampleGeneratedSurfaceVoxel(x, SEA_LEVEL_Y, z, 1);
            Check(VENPOD::Utils::UnpackMaterial(shallowWaterSurface) == VENPOD::Utils::Material::Water,
                "visual surface renders water over shallow submerged terrain");
            foundShallowSubmergedColumn = true;
        }
    }
    Check(foundShallowSubmergedColumn, "terrain test found a deterministic shallow below-sea column");

    const BrickCoord buriedCoord =
        BrickCoord::FromWorldVoxel(sampleX, terrainSurfaceY - coarseStep * 6, sampleZ);
    Check(terrain.IsDefinitelyBuriedSolidBrick(buriedCoord),
        "terrain buried-solid fast path identifies render-invisible interior bricks");
    const BrickCoord surfaceCoord = BrickCoord::FromWorldVoxel(sampleX, terrainSurfaceY, sampleZ);
    Check(!terrain.IsDefinitelyBuriedSolidBrick(surfaceCoord),
        "terrain buried-solid fast path keeps surface bricks resident-eligible");
    Check(!terrain.IsDefinitelyBuriedSolidBrick(BrickCoord::FromWorldVoxel(0, 384, 0)),
        "terrain buried-solid fast path rejects air bricks");
    Check(terrain.MayContainExposedSurfaceBrick(surfaceCoord),
        "terrain surface classifier keeps exposed terrain bricks protected");
    Check(!terrain.MayContainExposedSurfaceBrick(buriedCoord),
        "terrain surface classifier demotes deeply buried interior bricks");
    Check(!terrain.MayContainExposedSurfaceBrick(BrickCoord::FromWorldVoxel(0, 384, 0)),
        "terrain surface classifier demotes high air bricks");
    Check(terrain.MayContainExposedSurfaceBrick(
            BrickCoord::FromWorldVoxel(waterBasinX, SEA_LEVEL_Y, waterBasinZ)),
        "terrain surface classifier keeps exposed water-surface bricks protected");

    SparseTerrainGenerator publicTerrain(1337u);
    Check(publicTerrain.HeightAt(192, 224) <= 112.0f,
        "public sparse basin caps the scenic spawn below overhead-slab height");
    Check(publicTerrain.HeightAt(144, 80) <= 140.0f,
        "public sparse basin caps near-origin ridges below overhead-slab height");
    Check(publicTerrain.HeightAt(128, 288) <= 80.0f,
        "public sparse basin keeps the walk-test route in a grounded midland band");

    const auto spawn = terrain.FindScenicSpawn(96, 96, 6.0f);
    Check(spawn.found, "scenic sparse spawn finds a validated spawn near origin");
    Check(spawn.groundY > SEA_LEVEL_Y + 6, "scenic sparse spawn avoids water basin starts");
    Check(spawn.eyeY > static_cast<float>(spawn.groundY) + 6.0f,
        "scenic sparse spawn places eye above player clearance");
    Check(spawn.localRelief <= 118.0f,
        "scenic sparse spawn avoids unwalkable wall pockets while preserving vertical terrain");
    const uint8_t spawnGroundMaterial =
        VENPOD::Utils::UnpackMaterial(terrain.SampleGeneratedVoxel(spawn.worldX, spawn.groundY, spawn.worldZ));
    Check(spawnGroundMaterial != VENPOD::Utils::Material::Air &&
          spawnGroundMaterial != VENPOD::Utils::Material::Water &&
          spawnGroundMaterial != VENPOD::Utils::Material::Bedrock,
        "scenic sparse spawn uses a walkable generated material");
    for (int32_t y = spawn.groundY + 1; y <= spawn.groundY + 14; ++y) {
        Check(VENPOD::Utils::UnpackMaterial(terrain.SampleGeneratedVoxel(spawn.worldX, y, spawn.worldZ)) ==
                  VENPOD::Utils::Material::Air,
            "scenic sparse spawn has head clearance");
    }
    const float coneOffsets[] = {-0.70f, -0.35f, 0.0f, 0.35f, 0.70f};
    for (float offset : coneOffsets) {
        const float spawnDirX = std::cos(spawn.yaw + offset);
        const float spawnDirZ = std::sin(spawn.yaw + offset);
        for (int32_t d = 32; d <= 128; d += 32) {
            const int32_t sx =
                static_cast<int32_t>(std::round(static_cast<float>(spawn.worldX) + spawnDirX * static_cast<float>(d)));
            const int32_t sz =
                static_cast<int32_t>(std::round(static_cast<float>(spawn.worldZ) + spawnDirZ * static_cast<float>(d)));
            const float dropBelowEye = spawn.eyeY - terrain.HeightAt(sx, sz);
            Check(dropBelowEye >= 14.0f,
                "scenic sparse spawn view cone keeps immediate terrain below the eye line");
        }
    }

    const auto extremeSpawn = terrain.FindScenicSpawn(
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max());
    Check(std::isfinite(extremeSpawn.eyeY) && std::isfinite(extremeSpawn.score),
        "scenic sparse spawn clamps malformed inputs to finite fallback data");
    Check(extremeSpawn.groundY >= TERRAIN_MIN_Y && extremeSpawn.groundY <= TERRAIN_MAX_Y,
        "scenic sparse spawn keeps extreme-origin fallback ground in terrain bounds");
}

void TestEditStoreAndCollision() {
    SparseTerrainGenerator terrain(12345u);
    SparseEditStore edits;

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

    edits.SetVoxel(-1, -1, -1, stone);
    Check(edits.EditedBrickCount() == 1, "negative edit creates one overlay");
    Check(edits.EditedVoxelCount() == 1, "negative edit creates one voxel edit");
    Check(edits.HasOverlay(BrickCoord{-1, -1, -1}), "negative edit overlay coord");
    Check(edits.PendingGpuDeltas().size() == 1, "negative edit queues one GPU delta");
    Check(edits.PendingGpuDeltas()[0].coord == BrickCoord{-1, -1, -1},
        "negative edit GPU delta brick coord");
    Check(edits.PendingGpuDeltas()[0].packedLocal == ((15u) | (15u << 8u) | (15u << 16u)),
        "negative edit GPU delta local coord");
    Check(edits.PendingGpuDeltas()[0].voxel == stone, "negative edit GPU delta voxel payload");

    uint32_t stored = 0;
    Check(edits.TryGetVoxel(-1, -1, -1, &stored) && stored == stone, "negative edit lookup");

    GeneratedSparseBrick editedBrick = terrain.GenerateBrick({-1, -1, -1});
    edits.ApplyToGeneratedBrick(editedBrick);
    Check(editedBrick.voxels[LocalVoxelIndex({15, 15, 15})] == stone,
        "edit overlay applies to generated brick local coord");

    SparseCollisionQuery collision(terrain, &edits);
    CollisionSample editedSample = collision.Sample(-1, -1, -1);
    Check(editedSample.fromEdit, "collision reports edit source");
    Check(editedSample.status == CollisionSampleStatus::KnownSolid, "collision edit solid");

    const int32_t highX = 32;
    const int32_t highY = 900;
    const int32_t highZ = 32;
    CollisionSample generatedAir = collision.Sample(highX, highY, highZ);
    Check(!generatedAir.fromEdit, "high air from generated terrain");
    Check(generatedAir.status == CollisionSampleStatus::KnownAir, "generated high sample air");

    edits.SetVoxel(highX, highY, highZ, stone);
    Check(edits.PendingGpuDeltas().size() == 2, "second edit appends GPU delta");
    edits.ClearPendingGpuDeltas(1);
    Check(edits.PendingGpuDeltas().size() == 1, "partial GPU delta clear preserves tail");
    edits.ClearPendingGpuDeltas();
    Check(edits.PendingGpuDeltas().empty(), "full GPU delta clear empties queue");
    CollisionSample editedHigh = collision.Sample(highX, highY, highZ);
    Check(editedHigh.fromEdit, "collision high edit source");
    Check(editedHigh.status == CollisionSampleStatus::KnownSolid, "edit overrides generated air");

    const int32_t groundX = 96;
    const int32_t groundZ = 96;
    const int32_t groundY = static_cast<int32_t>(terrain.HeightAt(groundX, groundZ)) - 4;
    CollisionSample generatedGround = collision.Sample(groundX, groundY, groundZ);
    Check(generatedGround.status == CollisionSampleStatus::KnownSolid,
        "collision can query generated solid without render residency");

    edits.SetVoxel(groundX, groundY, groundZ, air);
    CollisionSample carvedGround = collision.Sample(groundX, groundY, groundZ);
    Check(carvedGround.fromEdit, "collision carved edit source");
    Check(carvedGround.status == CollisionSampleStatus::KnownAir,
        "edit overlay can carve generated solid for collision");

    Check(IsSparseEditPersistencePathAllowed("review-edits.vsed"),
        "sparse edit persistence accepts .vsed paths");
    Check(IsSparseEditPersistencePathAllowed("review-edits.VSED"),
        "sparse edit persistence accepts uppercase .vsed extension");
    Check(!IsSparseEditPersistencePathAllowed("review-edits.txt"),
        "sparse edit persistence rejects non-vsed paths");
    const std::filesystem::path rejectedEditPath =
        std::filesystem::temp_directory_path() / "venpod_sparse_edit_store_reject.txt";
    {
        std::ofstream rejected(rejectedEditPath, std::ios::binary | std::ios::trunc);
        rejected << "keep";
    }
    Check(!edits.SaveToFile(rejectedEditPath),
        "sparse edit store refuses to save over non-vsed path");
    {
        std::ifstream rejected(rejectedEditPath, std::ios::binary);
        std::string preserved;
        rejected >> preserved;
        Check(preserved == "keep",
            "sparse edit store leaves rejected non-vsed file untouched");
    }
    SparseEditStore rejectedLoadEdits;
    Check(!rejectedLoadEdits.LoadFromFile(rejectedEditPath),
        "sparse edit store refuses to load non-vsed path");

    const std::filesystem::path editPath =
        std::filesystem::temp_directory_path() / "venpod_sparse_edit_store_roundtrip.vsed";
    std::filesystem::remove(editPath);
    Check(edits.SaveToFile(editPath), "sparse edit store saves binary overlay file");

    SparseEditStore loadedEdits;
    Check(loadedEdits.LoadFromFile(editPath), "sparse edit store loads binary overlay file");
    Check(loadedEdits.EditedBrickCount() == edits.EditedBrickCount(),
        "loaded sparse edit store preserves brick count");
    Check(loadedEdits.EditedVoxelCount() == edits.EditedVoxelCount(),
        "loaded sparse edit store preserves voxel count");
    Check(loadedEdits.PendingGpuDeltas().empty(),
        "loaded sparse edit store starts with no transient GPU delta backlog");
    Check(loadedEdits.TryGetVoxel(-1, -1, -1, &stored) && stored == stone,
        "loaded sparse edit store preserves negative edit");
    Check(loadedEdits.TryGetVoxel(groundX, groundY, groundZ, &stored) && stored == air,
        "loaded sparse edit store preserves carved generated terrain");

    const std::filesystem::path corruptPath =
        std::filesystem::temp_directory_path() / "venpod_sparse_edit_store_corrupt.vsed";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t badMagic = 0u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 0u;
        const uint64_t totalVoxelCount = 0u;
        WriteTestBinary(corrupt, badMagic);
        WriteTestBinary(corrupt, version);
        WriteTestBinary(corrupt, brickSize);
        WriteTestBinary(corrupt, reserved);
        WriteTestBinary(corrupt, overlayCount);
        WriteTestBinary(corrupt, totalVoxelCount);
    }
    Check(!loadedEdits.LoadFromFile(corruptPath),
        "sparse edit store rejects invalid file magic");
    Check(loadedEdits.TryGetVoxel(-1, -1, -1, &stored) && stored == stone,
        "failed sparse edit load preserves previous overlays");

    {
        std::ofstream impossibleLength(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 128'000'000ull;
        WriteTestBinary(impossibleLength, magic);
        WriteTestBinary(impossibleLength, version);
        WriteTestBinary(impossibleLength, brickSize);
        WriteTestBinary(impossibleLength, reserved);
        WriteTestBinary(impossibleLength, overlayCount);
        WriteTestBinary(impossibleLength, totalVoxelCount);
    }
    Check(!loadedEdits.LoadFromFile(corruptPath),
        "sparse edit store rejects header counts that exceed file length before allocation");
    Check(loadedEdits.TryGetVoxel(groundX, groundY, groundZ, &stored) && stored == air,
        "impossible-length load failure preserves previous carved edit");

    {
        std::ofstream duplicate(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 2u;
        const BrickCoord coord{0, 0, 0};
        const uint32_t revision = 2u;
        const uint32_t voxelCount = 2u;
        const uint16_t duplicateLocal = 0u;
        WriteTestBinary(duplicate, magic);
        WriteTestBinary(duplicate, version);
        WriteTestBinary(duplicate, brickSize);
        WriteTestBinary(duplicate, reserved);
        WriteTestBinary(duplicate, overlayCount);
        WriteTestBinary(duplicate, totalVoxelCount);
        WriteTestBinary(duplicate, coord.x);
        WriteTestBinary(duplicate, coord.y);
        WriteTestBinary(duplicate, coord.z);
        WriteTestBinary(duplicate, revision);
        WriteTestBinary(duplicate, voxelCount);
        WriteTestBinary(duplicate, duplicateLocal);
        WriteTestBinary(duplicate, stone);
        WriteTestBinary(duplicate, duplicateLocal);
        WriteTestBinary(duplicate, air);
    }
    Check(!loadedEdits.LoadFromFile(corruptPath),
        "sparse edit store rejects duplicate local entries in one overlay");
    Check(loadedEdits.TryGetVoxel(groundX, groundY, groundZ, &stored) && stored == air,
        "duplicate-entry load failure preserves previous carved edit");

    {
        std::ofstream zeroRevision(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 1u;
        const BrickCoord coord{1, 2, 3};
        const uint32_t zeroRevisionValue = 0u;
        const uint32_t voxelCount = 1u;
        const uint16_t local = 0u;
        WriteTestBinary(zeroRevision, magic);
        WriteTestBinary(zeroRevision, version);
        WriteTestBinary(zeroRevision, brickSize);
        WriteTestBinary(zeroRevision, reserved);
        WriteTestBinary(zeroRevision, overlayCount);
        WriteTestBinary(zeroRevision, totalVoxelCount);
        WriteTestBinary(zeroRevision, coord.x);
        WriteTestBinary(zeroRevision, coord.y);
        WriteTestBinary(zeroRevision, coord.z);
        WriteTestBinary(zeroRevision, zeroRevisionValue);
        WriteTestBinary(zeroRevision, voxelCount);
        WriteTestBinary(zeroRevision, local);
        WriteTestBinary(zeroRevision, stone);
    }
    Check(!loadedEdits.LoadFromFile(corruptPath),
        "sparse edit store rejects non-empty overlay with zero revision");
    Check(loadedEdits.TryGetVoxel(-1, -1, -1, &stored) && stored == stone,
        "zero-revision load failure preserves previous negative edit");

    {
        std::ofstream emptyOverlay(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 0u;
        const BrickCoord coord{4, 5, 6};
        const uint32_t revision = 1u;
        const uint32_t voxelCount = 0u;
        WriteTestBinary(emptyOverlay, magic);
        WriteTestBinary(emptyOverlay, version);
        WriteTestBinary(emptyOverlay, brickSize);
        WriteTestBinary(emptyOverlay, reserved);
        WriteTestBinary(emptyOverlay, overlayCount);
        WriteTestBinary(emptyOverlay, totalVoxelCount);
        WriteTestBinary(emptyOverlay, coord.x);
        WriteTestBinary(emptyOverlay, coord.y);
        WriteTestBinary(emptyOverlay, coord.z);
        WriteTestBinary(emptyOverlay, revision);
        WriteTestBinary(emptyOverlay, voxelCount);
    }
    Check(!loadedEdits.LoadFromFile(corruptPath),
        "sparse edit store rejects empty overlay records");
    Check(loadedEdits.TryGetVoxel(groundX, groundY, groundZ, &stored) && stored == air,
        "empty-overlay load failure preserves previous carved edit");

    {
        std::ofstream nearWrap(corruptPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 1u;
        const BrickCoord coord{7, 8, 9};
        const uint32_t revision = std::numeric_limits<uint32_t>::max() - 1u;
        const uint32_t voxelCount = 1u;
        const uint16_t local = 0u;
        WriteTestBinary(nearWrap, magic);
        WriteTestBinary(nearWrap, version);
        WriteTestBinary(nearWrap, brickSize);
        WriteTestBinary(nearWrap, reserved);
        WriteTestBinary(nearWrap, overlayCount);
        WriteTestBinary(nearWrap, totalVoxelCount);
        WriteTestBinary(nearWrap, coord.x);
        WriteTestBinary(nearWrap, coord.y);
        WriteTestBinary(nearWrap, coord.z);
        WriteTestBinary(nearWrap, revision);
        WriteTestBinary(nearWrap, voxelCount);
        WriteTestBinary(nearWrap, local);
        WriteTestBinary(nearWrap, stone);
    }
    SparseEditStore wrapEdits;
    Check(wrapEdits.LoadFromFile(corruptPath),
        "sparse edit store loads near-saturated overlay revision");
    const int32_t wrapWorldX = 7 * SPARSE_BRICK_SIZE;
    const int32_t wrapWorldY = 8 * SPARSE_BRICK_SIZE;
    const int32_t wrapWorldZ = 9 * SPARSE_BRICK_SIZE;
    wrapEdits.SetVoxel(wrapWorldX, wrapWorldY, wrapWorldZ, air);
    Check(wrapEdits.GetOverlayRevision({7, 8, 9}) == std::numeric_limits<uint32_t>::max(),
        "sparse edit store can advance to maximum revision");
    Check(wrapEdits.PendingGpuDeltas().size() == 1 &&
          wrapEdits.PendingGpuDeltas()[0].revision == std::numeric_limits<uint32_t>::max(),
        "sparse edit store queues maximum-revision GPU delta");
    wrapEdits.SetVoxel(wrapWorldX + 1, wrapWorldY, wrapWorldZ, stone);
    Check(wrapEdits.GetOverlayRevision({7, 8, 9}) == 1u,
        "sparse edit store wraps saturated revision to nonzero epoch");
    bool sawResetLocal0 = false;
    bool sawResetLocal1 = false;
    for (const SparseEditDelta& delta : wrapEdits.PendingGpuDeltas()) {
        sawResetLocal0 = sawResetLocal0 ||
            (delta.revision == 1u && delta.packedLocal == PackSparseEditLocal({0, 0, 0}));
        sawResetLocal1 = sawResetLocal1 ||
            (delta.revision == 1u && delta.packedLocal == PackSparseEditLocal({1, 0, 0}));
    }
    Check(wrapEdits.PendingGpuDeltas().size() == 2 && sawResetLocal0 && sawResetLocal1,
        "sparse edit store republishes complete overlay deltas on revision epoch reset");

    SparseVoxelWorld savedWorld;
    Check(savedWorld.Initialize({16, 64, 12345u}), "sparse world for edit persistence initializes");
    savedWorld.SetEditedVoxel(-32, 880, 17, stone);
    Check(savedWorld.SaveEditsToFile(editPath), "sparse world saves edits through runtime wrapper");

    SparseVoxelWorld loadedWorld;
    Check(loadedWorld.Initialize({16, 64, 12345u}), "sparse world edit load initializes");
    Check(loadedWorld.LoadEditsFromFile(editPath, false), "sparse world loads edits through runtime wrapper");
    Check(loadedWorld.GetEdits().EditedVoxelCount() == 1,
        "sparse world loaded edit count matches saved runtime edits");
    Check(loadedWorld.SampleCollisionStatus(-32, 880, 17) == CollisionSampleStatus::KnownSolid,
        "loaded sparse world edits are collision-authoritative immediately");
    std::filesystem::remove(editPath);
    std::filesystem::remove(corruptPath);
}

void TestSparseCollisionVolumesAndSweeps() {
    SparseTerrainGenerator terrain(12345u);
    SparseEditStore edits;
    SparseCollisionQuery collision(terrain, &edits);

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t water = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Water,
        1,
        0,
        0);
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

    const SparseCollisionAabb emptyBox{100.1f, 900.1f, 100.1f, 101.9f, 901.9f, 101.9f};
    SparseCollisionVolumeResult emptyVolume = collision.TestAabb(emptyBox);
    Check(!emptyVolume.blocked, "sparse collision AABB over high generated air is open");
    Check(emptyVolume.sampledVoxels == 8, "sparse collision AABB samples exclusive max bounds");

    edits.SetVoxel(100, 900, 100, stone);
    SparseCollisionVolumeResult solidVolume = collision.TestAabb(emptyBox);
    Check(solidVolume.blocked, "sparse collision AABB blocks on edited solid");
    Check(solidVolume.firstBlockingX == 100 && solidVolume.firstBlockingY == 900 && solidVolume.firstBlockingZ == 100,
        "sparse collision AABB records first blocking voxel");
    Check(solidVolume.solidVoxels == 1, "sparse collision AABB counts solid voxels");

    edits.SetVoxel(100, 900, 100, air);
    edits.SetVoxel(101, 900, 100, water);
    SparseCollisionVolumeResult liquidNonBlocking = collision.TestAabb(emptyBox);
    Check(!liquidNonBlocking.blocked, "sparse collision liquid is non-blocking by default");
    Check(liquidNonBlocking.hasLiquid && liquidNonBlocking.liquidVoxels == 1,
        "sparse collision AABB reports liquid overlap");
    SparseCollisionVolumeResult liquidBlocking = collision.TestAabb(emptyBox, true);
    Check(liquidBlocking.blocked, "sparse collision liquid can block when requested");

    edits.SetVoxel(101, 900, 100, air);
    edits.SetVoxel(104, 900, 100, stone);
    const SparseCollisionAabb sweepStart{100.1f, 900.1f, 100.1f, 101.9f, 901.9f, 101.9f};
    SparseCollisionSweepResult sweep = collision.SweepAabb(sweepStart, 4.0f, 0.0f, 0.0f, 8);
    Check(sweep.blocked, "sparse collision sweep blocks before edited solid");
    Check(sweep.safeFraction > 0.0f && sweep.safeFraction < 1.0f,
        "sparse collision sweep reports partial safe fraction");
    Check(sweep.hitFraction > sweep.safeFraction, "sparse collision sweep hit follows safe fraction");

    SparseCollisionSweepResult clearSweep = collision.SweepAabb(sweepStart, 0.0f, 0.0f, 4.0f, 8);
    Check(!clearSweep.blocked && clearSweep.safeFraction == 1.0f,
        "sparse collision sweep remains open when path is clear");

    edits.SetVoxel(-3, -20, -3, stone);
    const SparseCollisionAabb negativeBox{-3.8f, -20.2f, -3.8f, -2.1f, -19.1f, -2.1f};
    SparseCollisionVolumeResult negativeVolume = collision.TestAabb(negativeBox);
    Check(negativeVolume.blocked, "sparse collision AABB handles negative world coordinates");

    SparseCollisionSweepResult stationary = collision.SweepAabb(negativeBox, 0.0f, 0.0f, 0.0f, 8);
    Check(stationary.blocked && stationary.safeFraction == 0.0f,
        "sparse collision zero sweep reports initial overlap");

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    SparseCollisionVolumeResult nanVolume =
        collision.TestAabb({nan, 900.0f, 100.0f, 101.0f, 901.0f, 101.0f});
    Check(nanVolume.blocked && nanVolume.hasUnknown && nanVolume.unknownVoxels == 1,
        "sparse collision AABB rejects non-finite bounds as unknown blocked");
    SparseCollisionVolumeResult outOfRangeVolume =
        collision.TestAabb({0.0f, 0.0f, 0.0f, inf, 1.0f, 1.0f});
    Check(outOfRangeVolume.blocked && outOfRangeVolume.hasUnknown && outOfRangeVolume.sampledVoxels == 1,
        "sparse collision AABB rejects out-of-range bounds without scanning");
    SparseCollisionVolumeResult oversizedVolume =
        collision.TestAabb({0.0f, 900.0f, 0.0f, 200.0f, 1100.0f, 200.0f});
    Check(oversizedVolume.blocked && oversizedVolume.hasUnknown && oversizedVolume.sampledVoxels == 1,
        "sparse collision AABB rejects oversized queries without unbounded scans");
    SparseCollisionSweepResult invalidSweep = collision.SweepAabb(sweepStart, inf, 0.0f, 0.0f, 8);
    Check(invalidSweep.blocked && invalidSweep.safeFraction == 0.0f &&
            invalidSweep.hitVolume.hasUnknown,
        "sparse collision sweep rejects non-finite deltas as unknown blocked");
    SparseCollisionSweepResult hugeStepSweep =
        collision.SweepAabb(sweepStart, 0.0f, 0.0f, 4.0f, std::numeric_limits<uint32_t>::max());
    Check(!hugeStepSweep.blocked && hugeStepSweep.safeFraction == 1.0f,
        "sparse collision sweep clamps huge step counts without blocking an open path");

    SparseEditStore supportEdits;
    SparseCollisionQuery supportCollision(terrain, &supportEdits);
    supportEdits.SetVoxel(200, 1395, 200, stone);
    const SparseCollisionAabb supportFootprint{199.4f, 1400.0f, 199.4f, 200.6f, 1400.1f, 200.6f};
    SparseCollisionSupportResult support = supportCollision.FindSupportBelow(supportFootprint, 8.0f);
    Check(support.found, "sparse collision support query finds edited support below footprint");
    Check(support.supportY == 1395 && support.fromEdit, "sparse collision support reports edited support voxel");

    supportEdits.SetVoxel(200, 1395, 200, air);
    supportEdits.SetVoxel(200, 1396, 200, water);
    SparseCollisionSupportResult liquidNoSupport = supportCollision.FindSupportBelow(supportFootprint, 8.0f);
    Check(!liquidNoSupport.found, "sparse collision support ignores liquid by default");
    SparseCollisionSupportResult liquidSupport = supportCollision.FindSupportBelow(supportFootprint, 8.0f, true);
    Check(liquidSupport.found && liquidSupport.supportY == 1396,
        "sparse collision support can accept liquid when requested");
    SparseCollisionSupportResult invalidSupport =
        supportCollision.FindSupportBelow({nan, 1400.0f, 199.4f, 200.6f, 1400.1f, 200.6f}, 8.0f);
    Check(!invalidSupport.found && invalidSupport.sampledVoxels == 0,
        "sparse collision support rejects non-finite footprints without scanning");
    SparseCollisionSupportResult oversizedSupport =
        supportCollision.FindSupportBelow({0.0f, 1400.0f, 0.0f, 400.0f, 1400.1f, 400.0f}, 500.0f);
    Check(!oversizedSupport.found && oversizedSupport.sampledVoxels == 0,
        "sparse collision support rejects oversized footprints without unbounded scans");
}

void TestSparseCharacterController() {
    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t water = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Water,
        1,
        0,
        0);

    SparseVoxelWorld blockedWorld;
    Check(blockedWorld.Initialize({16, 64, 12345u}), "character controller blocked world initializes");
    blockedWorld.SetEditedVoxel(2, 998, 0, stone);
    SparseCharacterMoveRequest blockedRequest;
    blockedRequest.startBody = {0.0f, 1000.0f, 0.0f, 4.0f, 0.4f, 2.5f};
    blockedRequest.targetBody = {4.0f, 1000.0f, 0.0f, 4.0f, 0.4f, 2.5f};
    blockedRequest.allowStepUp = false;
    SparseCharacterMoveResult blocked = ResolveSparseCharacterHorizontalMove(blockedWorld, blockedRequest);
    Check(blocked.blocked, "sparse character controller blocks horizontal body sweep");
    Check(blocked.eyeX < blockedRequest.targetBody.eyeX,
        "sparse character controller rolls back blocked move before target");

    SparseVoxelWorld stepWorld;
    Check(stepWorld.Initialize({16, 64, 12345u}), "character controller step world initializes");
    stepWorld.SetEditedVoxel(2, 997, 0, stone);
    stepWorld.SetEditedVoxel(4, 996, 0, stone);
    SparseCharacterMoveRequest stepRequest;
    stepRequest.startBody = {0.0f, 1000.0f, 0.0f, 4.0f, 0.4f, 2.5f};
    stepRequest.targetBody = {4.0f, 1000.0f, 0.0f, 4.0f, 0.4f, 2.5f};
    stepRequest.verticalVelocity = -1.0f;
    stepRequest.allowStepUp = true;
    SparseCharacterMoveResult stepped = ResolveSparseCharacterHorizontalMove(stepWorld, stepRequest);
    Check(stepped.blocked && stepped.steppedUp, "sparse character controller steps over low ledge");
    Check(stepped.eyeX == stepRequest.targetBody.eyeX && stepped.eyeY > stepRequest.targetBody.eyeY,
        "sparse character controller places stepped body on support");

    SparseCharacterMoveRequest risingRequest = stepRequest;
    risingRequest.verticalVelocity = 5.0f;
    SparseCharacterMoveResult rising = ResolveSparseCharacterHorizontalMove(stepWorld, risingRequest);
    Check(rising.blocked && !rising.steppedUp,
        "sparse character controller does not step up while rising");

    SparseVoxelWorld verticalWorld;
    Check(verticalWorld.Initialize({16, 64, 12345u}), "character controller vertical world initializes");
    verticalWorld.SetEditedVoxel(0, 995, 0, stone);
    SparseCharacterVerticalMoveRequest fallRequest;
    fallRequest.startBody = {0.0f, 1006.0f, 0.0f, 5.0f, 0.4f, 2.5f};
    fallRequest.targetBody = {0.0f, 998.0f, 0.0f, 5.0f, 0.4f, 2.5f};
    fallRequest.verticalVelocity = -20.0f;
    SparseCharacterVerticalMoveResult landed =
        ResolveSparseCharacterVerticalMove(verticalWorld, fallRequest);
    Check(landed.blocked && landed.landed,
        "sparse character controller lands during fast downward sweep");
    Check(landed.eyeY == 1001.0f && landed.verticalVelocity == 0.0f,
        "sparse character controller clamps downward sweep to support");

    SparseVoxelWorld liquidVerticalWorld;
    Check(liquidVerticalWorld.Initialize({16, 64, 12345u}), "character controller liquid vertical world initializes");
    liquidVerticalWorld.SetEditedVoxel(0, 995, 0, water);
    SparseCharacterVerticalMoveRequest liquidFallRequest = fallRequest;
    liquidFallRequest.liquidsSupport = false;
    SparseCharacterVerticalMoveResult liquidFallIgnored =
        ResolveSparseCharacterVerticalMove(liquidVerticalWorld, liquidFallRequest);
    Check(!liquidFallIgnored.blocked && liquidFallIgnored.liquidVoxels > 0,
        "sparse character controller vertical sweep ignores liquid support by default");
    liquidFallRequest.liquidsSupport = true;
    SparseCharacterVerticalMoveResult liquidFallLanded =
        ResolveSparseCharacterVerticalMove(liquidVerticalWorld, liquidFallRequest);
    Check(liquidFallLanded.blocked && liquidFallLanded.landed &&
              liquidFallLanded.liquidVoxels > 0 && liquidFallLanded.eyeY == 1001.0f,
        "sparse character controller vertical sweep can land on temporary liquid support");

    verticalWorld.SetEditedVoxel(0, 1004, 0, stone);
    SparseCharacterVerticalMoveRequest ceilingRequest;
    ceilingRequest.startBody = {0.0f, 1000.0f, 0.0f, 5.0f, 0.4f, 2.5f};
    ceilingRequest.targetBody = {0.0f, 1008.0f, 0.0f, 5.0f, 0.4f, 2.5f};
    ceilingRequest.verticalVelocity = 20.0f;
    SparseCharacterVerticalMoveResult ceiling =
        ResolveSparseCharacterVerticalMove(verticalWorld, ceilingRequest);
    Check(ceiling.blocked && ceiling.hitCeiling,
        "sparse character controller blocks upward sweep against ceiling");
    Check(ceiling.eyeY < ceilingRequest.targetBody.eyeY && ceiling.verticalVelocity == 0.0f,
        "sparse character controller cancels upward velocity after ceiling hit");

    SparseVoxelWorld groundWorld;
    Check(groundWorld.Initialize({16, 64, 12345u}), "character controller ground world initializes");
    groundWorld.SetEditedVoxel(0, 995, 0, stone);
    SparseCharacterGroundRequest groundRequest;
    groundRequest.body = {0.0f, 1000.75f, 0.0f, 5.0f, 0.4f, 2.5f};
    groundRequest.verticalVelocity = -12.0f;
    groundRequest.maxSnapUp = 0.5f;
    groundRequest.maxSnapDown = 2.0f;
    SparseCharacterGroundResult grounded = ResolveSparseCharacterGrounding(groundWorld, groundRequest);
    Check(grounded.grounded && grounded.snapped,
        "sparse character controller snaps falling body to footprint support");
    Check(grounded.eyeY == 1001.0f && grounded.verticalVelocity == 0.0f,
        "sparse character controller places grounded eye at support plus height");

    SparseCharacterGroundRequest highRequest = groundRequest;
    highRequest.body.eyeY = 1010.0f;
    highRequest.maxSnapDown = 1.0f;
    SparseCharacterGroundResult high = ResolveSparseCharacterGrounding(groundWorld, highRequest);
    Check(!high.grounded, "sparse character controller does not snap to distant support");

    SparseCharacterGroundRequest penetrationRequest = groundRequest;
    penetrationRequest.body.eyeY = 1000.4f;
    penetrationRequest.maxSnapUp = 1.0f;
    SparseCharacterGroundResult penetration =
        ResolveSparseCharacterGrounding(groundWorld, penetrationRequest);
    Check(penetration.grounded && penetration.eyeY == 1001.0f,
        "sparse character controller resolves small upward ground penetration");

    SparseVoxelWorld liquidGroundWorld;
    Check(liquidGroundWorld.Initialize({16, 64, 12345u}), "character controller liquid ground world initializes");
    liquidGroundWorld.SetEditedVoxel(0, 995, 0, water);
    SparseCharacterGroundRequest liquidRequest = groundRequest;
    liquidRequest.liquidsSupport = false;
    SparseCharacterGroundResult liquidIgnored =
        ResolveSparseCharacterGrounding(liquidGroundWorld, liquidRequest);
    Check(!liquidIgnored.grounded,
        "sparse character controller ignores liquid support by default");
    liquidRequest.liquidsSupport = true;
    SparseCharacterGroundResult liquidGrounded =
        ResolveSparseCharacterGrounding(liquidGroundWorld, liquidRequest);
    Check(liquidGrounded.grounded && liquidGrounded.liquidVoxels > 0 && liquidGrounded.eyeY == 1001.0f,
        "sparse character controller can treat liquid as temporary walking support");

    SparseCharacterMoveRequest malformedHorizontal = blockedRequest;
    malformedHorizontal.targetBody.eyeX = std::numeric_limits<float>::quiet_NaN();
    malformedHorizontal.maxSweepSteps = std::numeric_limits<uint32_t>::max();
    SparseCharacterMoveResult malformedHorizontalResult =
        ResolveSparseCharacterHorizontalMove(blockedWorld, malformedHorizontal);
    Check(malformedHorizontalResult.blocked &&
          malformedHorizontalResult.safeFraction == 0.0f &&
          malformedHorizontalResult.eyeX == malformedHorizontal.startBody.eyeX &&
          malformedHorizontalResult.sampledVoxels == 0,
        "sparse character horizontal move rejects malformed target without collision scan");

    SparseCharacterVerticalMoveRequest malformedVertical = fallRequest;
    malformedVertical.targetBody.eyeY = std::numeric_limits<float>::infinity();
    malformedVertical.verticalVelocity = std::numeric_limits<float>::quiet_NaN();
    malformedVertical.maxSweepSteps = std::numeric_limits<uint32_t>::max();
    SparseCharacterVerticalMoveResult malformedVerticalResult =
        ResolveSparseCharacterVerticalMove(verticalWorld, malformedVertical);
    Check(malformedVerticalResult.blocked &&
          malformedVerticalResult.safeFraction == 0.0f &&
          malformedVerticalResult.eyeY == malformedVertical.startBody.eyeY &&
          malformedVerticalResult.verticalVelocity == 0.0f &&
          malformedVerticalResult.sampledVoxels == 0,
        "sparse character vertical move rejects malformed target without collision scan");

    SparseCharacterGroundRequest malformedGround = groundRequest;
    malformedGround.body.eyeY = std::numeric_limits<float>::quiet_NaN();
    malformedGround.maxSnapUp = std::numeric_limits<float>::infinity();
    malformedGround.maxSnapDown = std::numeric_limits<float>::infinity();
    SparseCharacterGroundResult malformedGroundResult =
        ResolveSparseCharacterGrounding(groundWorld, malformedGround);
    Check(!malformedGroundResult.grounded &&
          malformedGroundResult.sampledVoxels == 0 &&
          malformedGroundResult.verticalVelocity == 0.0f,
        "sparse character grounding rejects malformed body without support scan");
}

void TestSparseEditDeltaBatching() {
    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t sand = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Sand, 2, 0, 0);

    std::vector<SparseEditDelta> deltas = {
        {BrickCoord{2, 0, 0}, 1u, stone, 3u},
        {BrickCoord{-1, 4, 2}, 7u, sand, 2u},
        {BrickCoord{2, 0, 0}, 9u, sand, 5u},
        {BrickCoord{-1, 4, 2}, 3u, stone, 8u},
    };

    SparseEditDeltaBatch batch = BuildSparseEditDeltaBatch(deltas, 16, 16, 8);
    Check(!batch.overflow, "edit delta batch no overflow under generous caps");
    Check(batch.deltas.size() == deltas.size(), "edit delta batch keeps all deltas");
    Check(batch.ranges.size() == 2, "edit delta batch groups two bricks");
    Check(batch.rangeTable.size() == 8, "edit delta batch builds requested hash table");
    Check(batch.ranges[0].coord == BrickCoord{-1, 4, 2}, "edit delta batch sorts first brick coord");
    Check(batch.ranges[0].firstDelta == 0 && batch.ranges[0].deltaCount == 2,
        "edit delta batch first range covers first brick deltas");
    Check(batch.ranges[0].latestRevision == 8, "edit delta batch tracks latest revision per brick");
    Check(batch.ranges[1].coord == BrickCoord{2, 0, 0}, "edit delta batch sorts second brick coord");
    Check(batch.ranges[1].firstDelta == 2 && batch.ranges[1].deltaCount == 2,
        "edit delta batch second range covers second brick deltas");
    Check(batch.ranges[1].latestRevision == 5, "edit delta batch tracks second latest revision");

    auto lookupRange = [&](const BrickCoord& coord, uint32_t* outRange) {
        const uint32_t mask = static_cast<uint32_t>(batch.rangeTable.size() - 1u);
        const uint32_t start = HashBrickCoord32(coord) & mask;
        for (uint32_t probe = 0; probe < 64u; ++probe) {
            const uint32_t slot = (start + probe) & mask;
            const uint32_t rangeIndex = batch.rangeTable[slot];
            if (rangeIndex == 0xFFFFFFFFu) {
                return false;
            }
            if (rangeIndex < batch.ranges.size() && batch.ranges[rangeIndex].coord == coord) {
                *outRange = rangeIndex;
                return true;
            }
        }
        return false;
    };

    uint32_t rangeIndex = UINT32_MAX;
    Check(lookupRange(BrickCoord{-1, 4, 2}, &rangeIndex) && rangeIndex == 0,
        "edit delta range table resolves first brick");
    Check(lookupRange(BrickCoord{2, 0, 0}, &rangeIndex) && rangeIndex == 1,
        "edit delta range table resolves second brick");
    Check(!lookupRange(BrickCoord{99, 0, 0}, &rangeIndex),
        "edit delta range table misses unknown brick");

    SparseEditDeltaBatch rangeCapped = BuildSparseEditDeltaBatch(deltas, 16, 1);
    Check(rangeCapped.overflow, "edit delta batch reports range overflow");
    Check(rangeCapped.truncated, "edit delta batch reports truncation when range cap omits bricks");
    Check(rangeCapped.ranges.size() == 1, "edit delta range cap limits uploaded ranges");
    Check(rangeCapped.deltas.size() == rangeCapped.ranges[0].deltaCount,
        "edit delta overflow keeps range and delta arrays consistent");

    SparseEditDeltaBatch deltaCapped = BuildSparseEditDeltaBatch(deltas, 2, 16);
    Check(deltaCapped.overflow, "edit delta batch reports delta overflow");
    Check(deltaCapped.truncated, "edit delta batch reports true truncation under hard delta cap");
    Check(deltaCapped.deltas.size() == 2, "edit delta cap limits uploaded deltas");

    std::vector<SparseEditDelta> duplicateOverflowDeltas = {
        {BrickCoord{0, 0, 0}, 5u, stone, 1u},
        {BrickCoord{0, 0, 0}, 5u, sand, 9u},
        {BrickCoord{0, 0, 0}, 6u, stone, 2u},
    };
    SparseEditDeltaBatch duplicateOverflow =
        BuildSparseEditDeltaBatch(duplicateOverflowDeltas, 2, 16);
    Check(duplicateOverflow.overflow, "edit delta batch reports overflow after duplicate coalescing");
    Check(!duplicateOverflow.truncated,
        "edit delta duplicate coalescing can fully represent oversized duplicate input");
    Check(duplicateOverflow.deltas.size() == 2,
        "edit delta duplicate coalescing preserves capped upload size");
    bool sawNewestDuplicate = false;
    bool sawStaleDuplicate = false;
    for (const SparseEditDelta& delta : duplicateOverflow.deltas) {
        sawNewestDuplicate = sawNewestDuplicate ||
            (delta.packedLocal == 5u && delta.revision == 9u && delta.voxel == sand);
        sawStaleDuplicate = sawStaleDuplicate ||
            (delta.packedLocal == 5u && delta.revision == 1u);
    }
    Check(sawNewestDuplicate && !sawStaleDuplicate,
        "edit delta overflow keeps newest duplicate voxel revision");

    SparseEditDeltaBatch invalidRangeTable =
        BuildSparseEditDeltaBatch(deltas, 16, 16, 7);
    Check(invalidRangeTable.overflow && invalidRangeTable.truncated &&
          invalidRangeTable.deltas.empty() && invalidRangeTable.ranges.empty(),
        "edit delta batch treats invalid range table capacity as unrepresented input");

    SparseEditDeltaBatch tinyRangeTable =
        BuildSparseEditDeltaBatch(deltas, 16, 16, 1);
    Check(tinyRangeTable.overflow && tinyRangeTable.truncated,
        "edit delta batch treats range-table insertion failure as unrepresented input");
    Check(tinyRangeTable.ranges.size() == 2 && tinyRangeTable.rangeTable.size() == 1,
        "edit delta batch keeps CPU ranges visible when tiny range table overflows");
}

void TestSparseCollisionSupportRequests() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "support request world initialize");

    const BrickCoord emptyHigh{0, 1000, 0};
    Check(world.RequestBrickDetailed(emptyHigh) == SparseBrickRequestResult::SkippedKnownEmpty,
        "normal request keeps empty-brick fast path");

    const BrickCoord forcedEmptyHigh{0, 1001, 0};
    Check(world.RequestBrickDetailed(forcedEmptyHigh, false) == SparseBrickRequestResult::Allocated,
        "collision support request can force an empty brick resident");
    Check(world.MarkResidencyClass(forcedEmptyHigh, SparseResidencyClass::Collision),
        "forced support brick can be marked collision residency");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    world.SetEditedVoxel(20, 1400, 20, stone);
    const SparseCollisionAabb body{19.5f, 1399.5f, 19.5f, 20.5f, 1400.5f, 20.5f};
    Check(world.TestCollisionAabb(body).blocked,
        "sparse voxel world exposes authoritative AABB collision query");
    const SparseCollisionAabb support{19.5f, 1404.0f, 19.5f, 20.5f, 1404.1f, 20.5f};
    Check(world.FindCollisionSupportBelow(support, 8.0f).found,
        "sparse voxel world exposes authoritative support query");
}

void TestSparseGpuPhysicsProposalApply() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "gpu proposal world initialize");

    const uint32_t sand = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Sand, 7, 0, 0);
    const uint32_t water = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Water, 3, 0, 0);
    const uint32_t lava = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Lava, 5, 0, 0);
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    world.SetEditedVoxel(0, 17, 0, sand);
    world.SetEditedVoxel(0, 16, 0, air);

    SparsePhysicsPacketResult proposal;
    proposal.coord = BrickCoord{0, 1, 0};
    proposal.packetIndex = 0;
    proposal.destinationCoord = BrickCoord{0, 1, 0};
    proposal.generation = 1;
    proposal.materialMask = 1u;
    proposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    proposal.packedSourceLocal = 0u | (1u << 8u) | 0u;
    proposal.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    proposal.sourceVoxel = sand;
    proposal.destinationVoxel = air;

    Check(world.ApplyGpuPhysicsProposals({proposal}, 4, true) == 1,
        "gpu proposal applies one same-brick move");
    uint32_t sourceAfter = 0;
    uint32_t destinationAfter = 0;
    Check(world.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          VENPOD::Utils::UnpackMaterial(sourceAfter) == VENPOD::Utils::Material::Air,
        "gpu proposal clears source voxel");
    Check(world.GetEdits().TryGetVoxel(0, 16, 0, &destinationAfter) &&
          destinationAfter == sand,
        "gpu proposal writes destination voxel");
    Check(world.GetStats().physicsGpuAppliedMovesLastFrame == 1 &&
          world.GetStats().physicsGpuRejectedProposalsLastFrame == 0,
        "gpu proposal stats report one applied move without stale accumulation");
    Check(world.ApplyGpuPhysicsProposals({}, 4, true) == 0,
        "empty gpu proposal batch applies no moves");
    Check(world.GetStats().physicsGpuAppliedMovesLastFrame == 0 &&
          world.GetStats().physicsGpuRejectedProposalsLastFrame == 0,
        "empty gpu proposal batch resets gpu proposal stats");

    SparseVoxelWorld zeroGenerationWorld;
    Check(zeroGenerationWorld.Initialize({8, 32, 12345u}),
        "gpu proposal zero-generation world initialize");
    zeroGenerationWorld.SetEditedVoxel(0, 17, 0, sand);
    zeroGenerationWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult zeroGenerationProposal = proposal;
    zeroGenerationProposal.generation = 0u;
    Check(zeroGenerationWorld.ApplyGpuPhysicsProposals({zeroGenerationProposal}, 4, true) == 0,
        "gpu proposal rejects zero work generation");
    Check(zeroGenerationWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "zero-generation gpu proposal is counted as rejected");
    Check(zeroGenerationWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "zero-generation gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld malformedStatusWorld;
    Check(malformedStatusWorld.Initialize({8, 32, 12345u}),
        "gpu proposal malformed status world initialize");
    malformedStatusWorld.SetEditedVoxel(0, 17, 0, sand);
    malformedStatusWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult missingConsumedStatusProposal = proposal;
    missingConsumedStatusProposal.status = SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    Check(malformedStatusWorld.ApplyGpuPhysicsProposals({missingConsumedStatusProposal}, 4, true) == 0,
        "gpu proposal rejects proposal status without consumed bit");
    Check(malformedStatusWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "missing-consumed gpu proposal is counted as rejected");
    Check(malformedStatusWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "missing-consumed gpu proposal leaves source voxel unchanged");
    SparsePhysicsPacketResult unknownStatusProposal = proposal;
    unknownStatusProposal.status |= 0x80000000u;
    Check(malformedStatusWorld.ApplyGpuPhysicsProposals({unknownStatusProposal}, 4, true) == 0,
        "gpu proposal rejects unknown status bits");
    Check(malformedStatusWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "unknown-status gpu proposal is counted as rejected");
    Check(malformedStatusWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "unknown-status gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld editRevisionWorld;
    Check(editRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal edit-revision world initialize");
    editRevisionWorld.SetEditedVoxel(0, 17, 0, sand);
    SparsePhysicsPacketResult editRevisionProposal = proposal;
    editRevisionProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    editRevisionProposal.sourceRevision = 1u;
    editRevisionProposal.destinationRevision = 0u;
    editRevisionWorld.SetEditedVoxel(1, 17, 0, sand);
    Check(editRevisionWorld.ApplyGpuPhysicsProposals({editRevisionProposal}, 4, true) == 0,
        "gpu proposal rejects edit-delta result after source brick overlay revision advanced");
    Check(editRevisionWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "stale edit-delta gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld destinationRevisionWorld;
    Check(destinationRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal destination edit-revision world initialize");
    destinationRevisionWorld.SetEditedVoxel(0, 17, 0, sand);
    destinationRevisionWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult destinationRevisionProposal = proposal;
    destinationRevisionProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    destinationRevisionProposal.sourceRevision = 1u;
    destinationRevisionProposal.destinationRevision = 1u;
    destinationRevisionWorld.SetEditedVoxel(1, 16, 0, air);
    Check(destinationRevisionWorld.ApplyGpuPhysicsProposals({destinationRevisionProposal}, 4, true) == 0,
        "gpu proposal rejects edit-delta result after destination brick overlay revision advanced");
    Check(destinationRevisionWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "stale destination edit-delta gpu proposal leaves source voxel unchanged");
    Check(destinationRevisionWorld.GetEdits().TryGetVoxel(0, 16, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "stale destination edit-delta gpu proposal leaves destination voxel unchanged");

    SparseVoxelWorld crossBrickDestinationRevisionWorld;
    Check(crossBrickDestinationRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal cross-brick destination edit-revision world initialize");
    crossBrickDestinationRevisionWorld.SetEditedVoxel(15, 17, 0, sand);
    crossBrickDestinationRevisionWorld.SetEditedVoxel(16, 17, 0, air);
    SparsePhysicsPacketResult crossBrickDestinationRevisionProposal = proposal;
    crossBrickDestinationRevisionProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    crossBrickDestinationRevisionProposal.packedSourceLocal = 15u | (1u << 8u) | 0u;
    crossBrickDestinationRevisionProposal.packedDestinationLocal = 0u | (1u << 8u) | 0u;
    crossBrickDestinationRevisionProposal.destinationCoord = BrickCoord{1, 1, 0};
    crossBrickDestinationRevisionProposal.sourceRevision = 1u;
    crossBrickDestinationRevisionProposal.destinationRevision = 1u;
    crossBrickDestinationRevisionWorld.SetEditedVoxel(17, 17, 0, water);
    Check(crossBrickDestinationRevisionWorld.ApplyGpuPhysicsProposals(
              {crossBrickDestinationRevisionProposal},
              4,
              true) == 0,
        "gpu proposal rejects edit-delta result after cross-brick destination revision advanced");
    Check(crossBrickDestinationRevisionWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "cross-brick stale destination edit-delta proposal is counted as rejected");
    Check(crossBrickDestinationRevisionWorld.GetEdits().TryGetVoxel(15, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "cross-brick stale destination edit-delta proposal leaves source voxel unchanged");
    Check(crossBrickDestinationRevisionWorld.GetEdits().TryGetVoxel(16, 17, 0, &destinationAfter) &&
          destinationAfter == air,
        "cross-brick stale destination edit-delta proposal leaves destination voxel unchanged");

    SparseVoxelWorld staleDestinationVoxelWorld;
    Check(staleDestinationVoxelWorld.Initialize({8, 32, 12345u}),
        "gpu proposal stale-destination world initialize");
    staleDestinationVoxelWorld.SetEditedVoxel(0, 17, 0, sand);
    staleDestinationVoxelWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult staleDestinationVoxelProposal = proposal;
    staleDestinationVoxelProposal.destinationVoxel = water;
    Check(staleDestinationVoxelWorld.ApplyGpuPhysicsProposals({staleDestinationVoxelProposal}, 4, true) == 0,
        "gpu proposal rejects destination voxel mismatch before mutation");
    Check(staleDestinationVoxelWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "destination mismatch gpu proposal is counted as rejected");
    Check(staleDestinationVoxelWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "destination mismatch gpu proposal leaves source voxel unchanged");
    Check(staleDestinationVoxelWorld.GetEdits().TryGetVoxel(0, 16, 0, &destinationAfter) &&
          destinationAfter == air,
        "destination mismatch gpu proposal leaves destination voxel unchanged");

    SparseVoxelWorld futureRevisionWorld;
    Check(futureRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal future edit-revision world initialize");
    futureRevisionWorld.SetEditedVoxel(0, 17, 0, sand);
    futureRevisionWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult futureRevisionProposal = proposal;
    futureRevisionProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    futureRevisionProposal.sourceRevision = 99u;
    futureRevisionProposal.destinationRevision = 1u;
    Check(futureRevisionWorld.ApplyGpuPhysicsProposals({futureRevisionProposal}, 4, true) == 0,
        "gpu proposal rejects edit-delta result whose source revision does not exactly match CPU overlay");
    Check(futureRevisionWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "future edit-delta gpu proposal is counted as rejected");
    Check(futureRevisionWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "future edit-delta gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld inconsistentRevisionWorld;
    Check(inconsistentRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal inconsistent edit-revision world initialize");
    inconsistentRevisionWorld.SetEditedVoxel(0, 17, 0, sand);
    inconsistentRevisionWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult inconsistentRevisionProposal = proposal;
    inconsistentRevisionProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    inconsistentRevisionProposal.sourceRevision = 0u;
    inconsistentRevisionProposal.destinationRevision = 0u;
    Check(inconsistentRevisionWorld.ApplyGpuPhysicsProposals({inconsistentRevisionProposal}, 4, true) == 0,
        "gpu proposal rejects edit-delta status without any sampled edit revision");
    Check(inconsistentRevisionWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "inconsistent edit-delta gpu proposal is counted as rejected");
    Check(inconsistentRevisionWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "inconsistent edit-delta gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld malformedExpectedPageWorld;
    Check(malformedExpectedPageWorld.Initialize({8, 32, 12345u}),
        "gpu proposal malformed expected-page world initialize");
    malformedExpectedPageWorld.SetEditedVoxel(0, 17, 0, sand);
    malformedExpectedPageWorld.SetEditedVoxel(0, 16, 0, air);
    SparsePhysicsPacketResult malformedExpectedPageProposal = proposal;
    malformedExpectedPageProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    malformedExpectedPageProposal.expectedPageIndex = INVALID_BRICK_PAGE;
    malformedExpectedPageProposal.expectedPageGeneration = 0u;
    Check(malformedExpectedPageWorld.ApplyGpuPhysicsProposals({malformedExpectedPageProposal}, 4, true) == 0,
        "gpu proposal rejects expected-page status without expected page data");
    Check(malformedExpectedPageWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "malformed expected-page gpu proposal is counted as rejected");
    Check(malformedExpectedPageWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "malformed expected-page gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld inconsistentExpectedPageStatusWorld;
    Check(inconsistentExpectedPageStatusWorld.Initialize({8, 32, 12345u}),
        "gpu proposal inconsistent expected-page status world initialize");
    inconsistentExpectedPageStatusWorld.SetEditedVoxel(0, 17, 0, sand);
    inconsistentExpectedPageStatusWorld.SetEditedVoxel(0, 16, 0, air);
    Check(inconsistentExpectedPageStatusWorld.RequestBrick(BrickCoord{0, 1, 0}),
        "gpu proposal inconsistent expected-page status source request");
    Check(inconsistentExpectedPageStatusWorld.PumpGeneration(1) == 1,
        "gpu proposal inconsistent expected-page status source generation");
    SparseBrickUploadPacket inconsistentExpectedPagePacket;
    Check(inconsistentExpectedPageStatusWorld.PopNextUpload(&inconsistentExpectedPagePacket),
        "gpu proposal inconsistent expected-page status upload packet");
    Check(inconsistentExpectedPageStatusWorld.CompleteUpload(inconsistentExpectedPagePacket),
        "gpu proposal inconsistent expected-page status complete upload");

    SparsePhysicsPacketResult pageMatchWithoutHasProposal = proposal;
    pageMatchWithoutHasProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    pageMatchWithoutHasProposal.expectedPageIndex = inconsistentExpectedPagePacket.pageIndex;
    pageMatchWithoutHasProposal.expectedPageGeneration = inconsistentExpectedPagePacket.generation;
    Check(inconsistentExpectedPageStatusWorld.ApplyGpuPhysicsProposals(
              {pageMatchWithoutHasProposal},
              4,
              true) == 0,
        "gpu proposal rejects page-match status without has-expected-page status");
    Check(inconsistentExpectedPageStatusWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "page-match without has-expected-page gpu proposal is counted as rejected");
    Check(inconsistentExpectedPageStatusWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "page-match without has-expected-page gpu proposal leaves source voxel unchanged");

    SparsePhysicsPacketResult conflictingPageStatusProposal = proposal;
    conflictingPageStatusProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    conflictingPageStatusProposal.expectedPageIndex = inconsistentExpectedPagePacket.pageIndex;
    conflictingPageStatusProposal.expectedPageGeneration = inconsistentExpectedPagePacket.generation;
    Check(inconsistentExpectedPageStatusWorld.ApplyGpuPhysicsProposals(
              {conflictingPageStatusProposal},
              4,
              true) == 0,
        "gpu proposal rejects mutually exclusive page-match and page-stale status");
    Check(inconsistentExpectedPageStatusWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "conflicting expected-page status gpu proposal is counted as rejected");
    Check(inconsistentExpectedPageStatusWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "conflicting expected-page status gpu proposal leaves source voxel unchanged");

    SparseVoxelWorld malformedLocalWorld;
    Check(malformedLocalWorld.Initialize({8, 32, 12345u}),
        "gpu proposal malformed local-coordinate world initialize");
    malformedLocalWorld.SetEditedVoxel(255, 17, 0, sand);
    malformedLocalWorld.SetEditedVoxel(255, 16, 0, air);
    SparsePhysicsPacketResult malformedLocalProposal = proposal;
    malformedLocalProposal.packedSourceLocal = 255u | (1u << 8u) | 0u;
    malformedLocalProposal.packedDestinationLocal = 255u | (0u << 8u) | 0u;
    Check(malformedLocalWorld.ApplyGpuPhysicsProposals({malformedLocalProposal}, 4, true) == 0,
        "gpu proposal rejects local coordinates outside sparse brick bounds");
    Check(malformedLocalWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          malformedLocalWorld.GetStats().physicsCandidateBricks >= 1,
        "malformed local-coordinate gpu proposal is counted and requeued safely");
    Check(malformedLocalWorld.GetEdits().TryGetVoxel(255, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "malformed local-coordinate gpu proposal leaves outside source voxel unchanged");
    Check(malformedLocalWorld.GetEdits().TryGetVoxel(255, 16, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "malformed local-coordinate gpu proposal leaves outside destination voxel unchanged");

    SparseVoxelWorld overflowCoordWorld;
    Check(overflowCoordWorld.Initialize({8, 32, 12345u}),
        "gpu proposal overflow-coordinate world initialize");
    SparsePhysicsPacketResult overflowSourceCoordProposal = proposal;
    overflowSourceCoordProposal.coord = BrickCoord{std::numeric_limits<int32_t>::max(), 1, 0};
    Check(overflowCoordWorld.ApplyGpuPhysicsProposals({overflowSourceCoordProposal}, 4, true) == 0,
        "gpu proposal rejects source coordinate that would overflow world voxel conversion");
    Check(overflowCoordWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          overflowCoordWorld.GetStats().physicsCandidateBricks >= 1,
        "overflow source-coordinate gpu proposal is counted and requeued safely");
    SparsePhysicsPacketResult overflowDestinationCoordProposal = proposal;
    overflowDestinationCoordProposal.destinationCoord =
        BrickCoord{0, std::numeric_limits<int32_t>::min(), 0};
    Check(overflowCoordWorld.ApplyGpuPhysicsProposals({overflowDestinationCoordProposal}, 4, true) == 0,
        "gpu proposal rejects destination coordinate that would overflow world voxel conversion");
    Check(overflowCoordWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          overflowCoordWorld.GetStats().physicsCandidateBricks >= 1,
        "overflow destination-coordinate gpu proposal is counted and requeued safely");

    Check(world.ApplyGpuPhysicsProposals({proposal}, 4, true) == 0,
        "stale gpu proposal is rejected after source changed");

    SparseVoxelWorld residencyWorld;
    Check(residencyWorld.Initialize({8, 32, 12345u}), "gpu proposal residency world initialize");
    const BrickCoord residencyCoord{3, 1, -2};
    const int32_t residencySourceX = residencyCoord.x * SPARSE_BRICK_SIZE;
    const int32_t residencySourceY = residencyCoord.y * SPARSE_BRICK_SIZE + 1;
    const int32_t residencySourceZ = residencyCoord.z * SPARSE_BRICK_SIZE;
    const int32_t residencyDestX = residencyCoord.x * SPARSE_BRICK_SIZE;
    const int32_t residencyDestY = residencyCoord.y * SPARSE_BRICK_SIZE;
    const int32_t residencyDestZ = residencyCoord.z * SPARSE_BRICK_SIZE;
    residencyWorld.SetEditedVoxel(residencySourceX, residencySourceY, residencySourceZ, sand);
    residencyWorld.SetEditedVoxel(residencyDestX, residencyDestY, residencyDestZ, air);
    Check(residencyWorld.RequestBrick(residencyCoord), "gpu proposal residency source request");
    Check(residencyWorld.PumpGeneration(1) == 1, "gpu proposal residency source generation");
    SparseBrickUploadPacket residencyPacket;
    Check(residencyWorld.PopNextUpload(&residencyPacket), "gpu proposal residency source upload packet");
    Check(residencyWorld.CompleteUpload(residencyPacket), "gpu proposal residency source complete upload");

    SparsePhysicsPacketResult expectedPageProposal;
    expectedPageProposal.coord = residencyCoord;
    expectedPageProposal.destinationCoord = residencyCoord;
    expectedPageProposal.generation = 1u;
    expectedPageProposal.materialMask = 1u;
    expectedPageProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    expectedPageProposal.expectedPageIndex = residencyPacket.pageIndex;
    expectedPageProposal.expectedPageGeneration = residencyPacket.generation;
    expectedPageProposal.packedSourceLocal = 0u | (1u << 8u) | 0u;
    expectedPageProposal.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    expectedPageProposal.sourceVoxel = sand;
    expectedPageProposal.destinationVoxel = air;

    SparsePhysicsPacketResult stalePageProposal = expectedPageProposal;
    stalePageProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_STALE |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    stalePageProposal.expectedPageGeneration = residencyPacket.generation + 1u;
    Check(residencyWorld.ApplyGpuPhysicsProposals({stalePageProposal}, 4, true) == 0,
        "gpu proposal rejects stale expected page generation before voxel mutation");
    Check(residencyWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          residencyWorld.GetStats().physicsCandidateBricks >= 1,
        "stale expected-page gpu proposal is requeued for a fresh physics packet");
    Check(residencyWorld.GetEdits().TryGetVoxel(residencySourceX, residencySourceY, residencySourceZ, &sourceAfter) &&
          sourceAfter == sand,
        "stale expected-page gpu proposal leaves source voxel unchanged");
    Check(residencyWorld.ApplyGpuPhysicsProposals({expectedPageProposal}, 4, true) == 1,
        "gpu proposal applies after expected page index and generation match CPU page table");
    Check(residencyWorld.GetEdits().TryGetVoxel(residencyDestX, residencyDestY, residencyDestZ, &destinationAfter) &&
          destinationAfter == sand,
        "expected-page gpu proposal writes destination voxel after generation validation");

    SparseVoxelWorld expectedPageDestinationResidencyWorld;
    Check(expectedPageDestinationResidencyWorld.Initialize({8, 32, 12345u}),
        "gpu proposal expected-page destination residency world initialize");
    const BrickCoord expectedSourceCoord{2, 1, 0};
    const BrickCoord expectedDestinationCoord{2, 0, 0};
    const int32_t expectedSourceX = expectedSourceCoord.x * SPARSE_BRICK_SIZE + 4;
    const int32_t expectedSourceY = expectedSourceCoord.y * SPARSE_BRICK_SIZE;
    const int32_t expectedSourceZ = expectedSourceCoord.z * SPARSE_BRICK_SIZE + 4;
    const int32_t expectedDestX = expectedDestinationCoord.x * SPARSE_BRICK_SIZE + 4;
    const int32_t expectedDestY =
        expectedDestinationCoord.y * SPARSE_BRICK_SIZE + (SPARSE_BRICK_SIZE - 1);
    const int32_t expectedDestZ = expectedDestinationCoord.z * SPARSE_BRICK_SIZE + 4;
    expectedPageDestinationResidencyWorld.SetEditedVoxel(
        expectedSourceX,
        expectedSourceY,
        expectedSourceZ,
        sand);
    expectedPageDestinationResidencyWorld.SetEditedVoxel(
        expectedDestX,
        expectedDestY,
        expectedDestZ,
        air);
    Check(expectedPageDestinationResidencyWorld.RequestBrick(expectedSourceCoord),
        "gpu proposal expected-page destination residency source request");
    Check(expectedPageDestinationResidencyWorld.PumpGeneration(1) == 1,
        "gpu proposal expected-page destination residency source generation");
    SparseBrickUploadPacket expectedSourcePacket;
    Check(expectedPageDestinationResidencyWorld.PopNextUpload(&expectedSourcePacket),
        "gpu proposal expected-page destination residency source upload packet");
    Check(expectedPageDestinationResidencyWorld.CompleteUpload(expectedSourcePacket),
        "gpu proposal expected-page destination residency source complete upload");

    SparsePhysicsPacketResult expectedPageDestinationResidencyProposal = expectedPageProposal;
    expectedPageDestinationResidencyProposal.coord = expectedSourceCoord;
    expectedPageDestinationResidencyProposal.destinationCoord = expectedDestinationCoord;
    expectedPageDestinationResidencyProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_HAS_EXPECTED_PAGE |
        SPARSE_PHYSICS_PACKET_STATUS_PAGE_MATCH |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    expectedPageDestinationResidencyProposal.expectedPageIndex = expectedSourcePacket.pageIndex;
    expectedPageDestinationResidencyProposal.expectedPageGeneration = expectedSourcePacket.generation;
    expectedPageDestinationResidencyProposal.packedSourceLocal = 4u | (0u << 8u) | (4u << 16u);
    expectedPageDestinationResidencyProposal.packedDestinationLocal =
        4u | ((SPARSE_BRICK_SIZE - 1u) << 8u) | (4u << 16u);
    expectedPageDestinationResidencyProposal.sourceVoxel = sand;
    expectedPageDestinationResidencyProposal.destinationVoxel = air;
    Check(expectedPageDestinationResidencyWorld.ApplyGpuPhysicsProposals(
              {expectedPageDestinationResidencyProposal},
              4,
              true) == 1,
        "gpu proposal applies page-validated source into nonresident destination brick");
    Check(expectedPageDestinationResidencyWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 0,
        "page-validated destination residency proposal is not counted as rejected");
    Check(expectedPageDestinationResidencyWorld.GetEdits().TryGetVoxel(
              expectedSourceX,
              expectedSourceY,
              expectedSourceZ,
              &sourceAfter) &&
          VENPOD::Utils::UnpackMaterial(sourceAfter) == VENPOD::Utils::Material::Air,
        "page-validated destination residency proposal clears source voxel");
    Check(expectedPageDestinationResidencyWorld.GetEdits().TryGetVoxel(
              expectedDestX,
              expectedDestY,
              expectedDestZ,
              &destinationAfter) &&
          destinationAfter == sand,
        "page-validated destination residency proposal writes destination voxel");
    Check(expectedPageDestinationResidencyWorld.GenerationQueueSize() >= 1,
        "page-validated destination residency proposal requests missing destination render brick");
    Check(expectedPageDestinationResidencyWorld.PumpGenerationAround(1, expectedDestinationCoord) >= 1,
        "page-validated destination residency proposal generates requested destination render brick");
    SparseBrickUploadPacket expectedDestinationUpload;
    bool completedExpectedDestinationUpload = false;
    while (expectedPageDestinationResidencyWorld.PopNextUpload(&expectedDestinationUpload)) {
        Check(expectedPageDestinationResidencyWorld.CompleteUpload(expectedDestinationUpload),
            "page-validated destination residency proposal completes queued upload");
        completedExpectedDestinationUpload =
            completedExpectedDestinationUpload ||
            expectedDestinationUpload.coord == expectedDestinationCoord;
    }
    Check(completedExpectedDestinationUpload &&
          expectedPageDestinationResidencyWorld.GetPool().IsResident(expectedDestinationCoord),
        "page-validated destination residency proposal makes destination render brick resident");

    SparseVoxelWorld chainWorld;
    Check(chainWorld.Initialize({8, 32, 12345u}), "gpu chained proposal world initialize");
    chainWorld.SetEditedVoxel(0, 17, 0, sand);
    chainWorld.SetEditedVoxel(0, 16, 0, air);
    chainWorld.SetEditedVoxel(0, 15, 0, air);
    SparsePhysicsPacketResult chainA = proposal;
    chainA.packedSourceLocal = 0u | (1u << 8u) | 0u;
    chainA.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    SparsePhysicsPacketResult chainB = proposal;
    chainB.packedSourceLocal = 0u | (0u << 8u) | 0u;
    chainB.packedDestinationLocal = 0u | (15u << 8u) | 0u;
    chainB.destinationCoord = BrickCoord{0, 0, 0};
    Check(chainWorld.ApplyGpuPhysicsProposals({chainA, chainB}, 4, false) == 1,
        "gpu proposal apply rejects same-batch chained move through claimed destination voxel");
    Check(chainWorld.GetEdits().TryGetVoxel(0, 16, 0, &destinationAfter) &&
          destinationAfter == sand,
        "same-batch chained proposal leaves first destination occupied");
    Check(chainWorld.GetEdits().TryGetVoxel(0, 15, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "same-batch chained proposal does not move the voxel a second step");

    SparseVoxelWorld conflictWorld;
    Check(conflictWorld.Initialize({8, 32, 12345u}), "gpu proposal conflict world initialize");
    conflictWorld.SetEditedVoxel(3, 17, 0, sand);
    conflictWorld.SetEditedVoxel(4, 17, 0, sand);
    conflictWorld.SetEditedVoxel(3, 16, 0, air);
    SparsePhysicsPacketResult conflictA = proposal;
    conflictA.packedSourceLocal = 3u | (1u << 8u) | 0u;
    conflictA.packedDestinationLocal = 3u | (0u << 8u) | 0u;
    SparsePhysicsPacketResult conflictB = proposal;
    conflictB.packedSourceLocal = 4u | (1u << 8u) | 0u;
    conflictB.packedDestinationLocal = 3u | (0u << 8u) | 0u;
    Check(conflictWorld.ApplyGpuPhysicsProposals({conflictA, conflictB}, 4, false) == 1,
        "gpu proposal apply rejects competing same-batch writes to one destination voxel");
    Check(conflictWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          conflictWorld.GetStats().physicsCandidateBricks >= 1,
        "same-batch gpu proposal conflict requeues the rejected source voxel");
    Check(conflictWorld.GetEdits().TryGetVoxel(3, 16, 0, &destinationAfter) &&
          destinationAfter == sand,
        "same-destination proposal batch writes exactly one destination voxel");

    SparseVoxelWorld duplicateSourceWorld;
    Check(duplicateSourceWorld.Initialize({8, 32, 12345u}),
        "gpu proposal duplicate-source world initialize");
    duplicateSourceWorld.SetEditedVoxel(7, 17, 0, sand);
    duplicateSourceWorld.SetEditedVoxel(7, 16, 0, air);
    duplicateSourceWorld.SetEditedVoxel(8, 16, 0, air);
    SparsePhysicsPacketResult duplicateSourceA = proposal;
    duplicateSourceA.packedSourceLocal = 7u | (1u << 8u) | 0u;
    duplicateSourceA.packedDestinationLocal = 7u | (0u << 8u) | 0u;
    SparsePhysicsPacketResult duplicateSourceB = proposal;
    duplicateSourceB.packedSourceLocal = 7u | (1u << 8u) | 0u;
    duplicateSourceB.packedDestinationLocal = 8u | (0u << 8u) | 0u;
    Check(duplicateSourceWorld.ApplyGpuPhysicsProposals(
              {duplicateSourceA, duplicateSourceB},
              4,
              false) == 1,
        "gpu proposal apply rejects competing same-batch reads from one source voxel");
    Check(duplicateSourceWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1 &&
          duplicateSourceWorld.GetStats().physicsCandidateBricks >= 1,
        "same-source gpu proposal conflict requeues the rejected source voxel");
    Check(duplicateSourceWorld.GetEdits().TryGetVoxel(7, 16, 0, &destinationAfter) &&
          destinationAfter == sand,
        "same-source proposal batch writes the first destination voxel");
    Check(duplicateSourceWorld.GetEdits().TryGetVoxel(8, 16, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "same-source proposal batch leaves the second destination voxel unchanged");

    SparseVoxelWorld materialWorld;
    Check(materialWorld.Initialize({8, 32, 12345u}), "gpu proposal material world initialize");
    materialWorld.SetEditedVoxel(4, 17, 0, water);
    materialWorld.SetEditedVoxel(4, 16, 0, air);
    materialWorld.SetEditedVoxel(5, 17, 0, lava);
    materialWorld.SetEditedVoxel(5, 16, 0, air);
    materialWorld.SetEditedVoxel(6, 17, 0, sand);
    materialWorld.SetEditedVoxel(6, 16, 0, air);
    SparsePhysicsPacketResult waterProposal = proposal;
    waterProposal.materialMask = 2u;
    waterProposal.packedSourceLocal = 4u | (1u << 8u) | 0u;
    waterProposal.packedDestinationLocal = 4u | (0u << 8u) | 0u;
    waterProposal.sourceVoxel = water;
    SparsePhysicsPacketResult lavaProposal = proposal;
    lavaProposal.materialMask = 4u;
    lavaProposal.packedSourceLocal = 5u | (1u << 8u) | 0u;
    lavaProposal.packedDestinationLocal = 5u | (0u << 8u) | 0u;
    lavaProposal.sourceVoxel = lava;
    SparsePhysicsPacketResult rejectedSandByWaterMask = proposal;
    rejectedSandByWaterMask.materialMask = 2u;
    rejectedSandByWaterMask.packedSourceLocal = 6u | (1u << 8u) | 0u;
    rejectedSandByWaterMask.packedDestinationLocal = 6u | (0u << 8u) | 0u;
    Check(materialWorld.ApplyGpuPhysicsProposals(
              {waterProposal, lavaProposal, rejectedSandByWaterMask},
              8,
              false) == 2,
        "gpu proposal material mask applies water/lava and rejects sand under water-only mask");
    Check(materialWorld.GetEdits().TryGetVoxel(4, 16, 0, &destinationAfter) &&
          destinationAfter == water,
        "water gpu proposal writes water destination");
    Check(materialWorld.GetEdits().TryGetVoxel(5, 16, 0, &destinationAfter) &&
          destinationAfter == lava,
        "lava gpu proposal writes lava destination");
    Check(materialWorld.GetEdits().TryGetVoxel(6, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "material-masked gpu proposal leaves rejected sand source in place");

    SparseVoxelWorld mixedConflictWorld;
    Check(mixedConflictWorld.Initialize({8, 32, 12345u}),
        "gpu proposal mixed apply/reject world initialize");
    mixedConflictWorld.SetEditedVoxel(4, 17, 0, water);
    mixedConflictWorld.SetEditedVoxel(4, 16, 0, air);
    mixedConflictWorld.SetEditedVoxel(32, 17, 0, sand);
    mixedConflictWorld.SetEditedVoxel(32, 16, 0, air);
    SparsePhysicsPacketResult validMixedWaterProposal = proposal;
    validMixedWaterProposal.materialMask = 2u;
    validMixedWaterProposal.packedSourceLocal = 4u | (1u << 8u) | 0u;
    validMixedWaterProposal.packedDestinationLocal = 4u | (0u << 8u) | 0u;
    validMixedWaterProposal.sourceVoxel = water;
    SparsePhysicsPacketResult staleMixedSandProposal = proposal;
    staleMixedSandProposal.coord = BrickCoord{2, 1, 0};
    staleMixedSandProposal.destinationCoord = BrickCoord{2, 1, 0};
    staleMixedSandProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_EDIT_DELTA_HIT;
    staleMixedSandProposal.sourceRevision = 1u;
    staleMixedSandProposal.destinationRevision = 1u;
    staleMixedSandProposal.packedSourceLocal = 0u | (1u << 8u) | 0u;
    staleMixedSandProposal.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    staleMixedSandProposal.sourceVoxel = sand;
    staleMixedSandProposal.destinationVoxel = air;
    mixedConflictWorld.SetEditedVoxel(33, 16, 0, air);
    Check(mixedConflictWorld.ApplyGpuPhysicsProposals(
              {validMixedWaterProposal, staleMixedSandProposal},
              8,
              false) == 1,
        "mixed gpu proposal batch applies valid fluid and rejects stale edit-delta material");
    Check(mixedConflictWorld.GetStats().physicsGpuAppliedMovesLastFrame == 1 &&
          mixedConflictWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 1,
        "mixed gpu proposal batch records one applied move and one rejected proposal");
    Check(mixedConflictWorld.GetEdits().TryGetVoxel(4, 16, 0, &destinationAfter) &&
          destinationAfter == water,
        "mixed gpu proposal batch writes valid fluid destination");
    Check(mixedConflictWorld.GetEdits().TryGetVoxel(4, 17, 0, &sourceAfter) &&
          VENPOD::Utils::UnpackMaterial(sourceAfter) == VENPOD::Utils::Material::Air,
        "mixed gpu proposal batch clears valid fluid source");
    Check(mixedConflictWorld.GetEdits().TryGetVoxel(32, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "mixed gpu proposal batch preserves stale material source");
    Check(mixedConflictWorld.GetEdits().TryGetVoxel(32, 16, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "mixed gpu proposal batch leaves stale material destination unchanged");

    SparsePhysicsPacketResult boundaryProposal;
    boundaryProposal.coord = BrickCoord{0, 1, 0};
    boundaryProposal.destinationCoord = BrickCoord{0, 0, 0};
    boundaryProposal.generation = 1u;
    boundaryProposal.materialMask = 1u;
    boundaryProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    boundaryProposal.packedSourceLocal = 0u | (0u << 8u) | 0u;
    boundaryProposal.packedDestinationLocal = 0u | (15u << 8u) | 0u;
    boundaryProposal.sourceVoxel = sand;
    boundaryProposal.destinationVoxel = air;
    world.SetEditedVoxel(0, 16, 0, sand);
    world.SetEditedVoxel(0, 15, 0, air);
    Check(world.ApplyGpuPhysicsProposals({boundaryProposal}, 4, true) == 1,
        "gpu proposal applies cross-brick downward move");
    Check(world.GetEdits().TryGetVoxel(0, 15, 0, &destinationAfter) &&
          destinationAfter == sand,
        "cross-brick gpu proposal writes destination voxel");

    SparseVoxelWorld destinationResidencyWorld;
    Check(destinationResidencyWorld.Initialize({8, 32, 12345u}),
        "gpu proposal destination residency world initialize");
    destinationResidencyWorld.SetEditedVoxel(8, 16, 8, sand);
    destinationResidencyWorld.SetEditedVoxel(8, 15, 8, air);
    SparsePhysicsPacketResult destinationResidencyProposal = boundaryProposal;
    destinationResidencyProposal.packedSourceLocal = 8u | (0u << 8u) | (8u << 16u);
    destinationResidencyProposal.packedDestinationLocal = 8u | (15u << 8u) | (8u << 16u);
    Check(destinationResidencyWorld.ApplyGpuPhysicsProposals({destinationResidencyProposal}, 4, true) == 1,
        "gpu proposal applies when destination render brick is not resident yet");
    Check(destinationResidencyWorld.GenerationQueueSize() >= 2,
        "gpu proposal requests source and destination render bricks after cross-brick move");
    Check(destinationResidencyWorld.PumpGenerationAround(2, BrickCoord{0, 1, 0}) >= 1,
        "gpu proposal destination residency generates requested render bricks");
    uint32_t completedDestinationResidencyUploads = 0;
    SparseBrickUploadPacket destinationResidencyUpload;
    while (destinationResidencyWorld.PopNextUpload(&destinationResidencyUpload)) {
        Check(destinationResidencyWorld.CompleteUpload(destinationResidencyUpload),
            "gpu proposal destination residency completes requested upload");
        ++completedDestinationResidencyUploads;
    }
    Check(completedDestinationResidencyUploads >= 1,
        "gpu proposal destination residency produced at least one render upload");
    Check(destinationResidencyWorld.GetPool().IsResident(BrickCoord{0, 0, 0}),
        "gpu proposal destination brick becomes resident after queued generation/upload");

    SparsePhysicsPacketResult missingSupportProposal = boundaryProposal;
    missingSupportProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL |
        SPARSE_PHYSICS_PACKET_STATUS_MISSING_BELOW;
    missingSupportProposal.packedSourceLocal = 2u | (0u << 8u) | (2u << 16u);
    missingSupportProposal.packedDestinationLocal = 2u | (15u << 8u) | (2u << 16u);
    world.SetEditedVoxel(2, 16, 2, sand);
    world.SetEditedVoxel(2, 15, 2, air);
    Check(world.ApplyGpuPhysicsProposals({missingSupportProposal}, 4, true) == 1,
        "gpu proposal with missing render-support page is CPU-authoritatively validated");
    Check(world.GetEdits().TryGetVoxel(2, 15, 2, &destinationAfter) &&
          destinationAfter == sand,
        "missing-support gpu proposal writes validated destination voxel");

    SparseVoxelWorld budgetWorld;
    Check(budgetWorld.Initialize({8, 32, 12345u}), "gpu proposal budget world initialize");
    budgetWorld.SetEditedVoxel(0, 17, 0, sand);
    budgetWorld.SetEditedVoxel(0, 16, 0, air);
    budgetWorld.SetEditedVoxel(16, 17, 0, sand);
    budgetWorld.SetEditedVoxel(16, 16, 0, air);
    SparsePhysicsPacketResult proposalA = proposal;
    SparsePhysicsPacketResult proposalB = proposal;
    proposalB.coord = BrickCoord{1, 1, 0};
    proposalB.destinationCoord = BrickCoord{1, 1, 0};
    proposalB.packedSourceLocal = 0u | (1u << 8u) | 0u;
    proposalB.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    Check(budgetWorld.ApplyGpuPhysicsProposals({proposalA, proposalB}, 1, false) == 1,
        "gpu proposal apply obeys move budget");
    Check(budgetWorld.GetStats().physicsCandidateBricks >= 2,
        "unprocessed gpu proposal is requeued instead of dropped");

    SparseVoxelWorld zeroBudgetWorld;
    Check(zeroBudgetWorld.Initialize({8, 32, 12345u}), "gpu proposal zero-budget world initialize");
    zeroBudgetWorld.SetEditedVoxel(0, 17, 0, sand);
    zeroBudgetWorld.SetEditedVoxel(0, 16, 0, air);
    Check(zeroBudgetWorld.ApplyGpuPhysicsProposals({proposal}, 0, false) == 0,
        "zero-budget gpu proposal batch applies no moves");
    Check(zeroBudgetWorld.GetStats().physicsCandidateBricks >= 1 &&
          zeroBudgetWorld.GetStats().physicsGpuRejectedProposalsLastFrame == 0,
        "zero-budget gpu proposal is requeued without being rejected");
    Check(zeroBudgetWorld.GetEdits().TryGetVoxel(0, 17, 0, &sourceAfter) &&
          sourceAfter == sand,
        "zero-budget gpu proposal leaves source voxel unchanged");
    Check(zeroBudgetWorld.GetEdits().TryGetVoxel(0, 16, 0, &destinationAfter) &&
          VENPOD::Utils::UnpackMaterial(destinationAfter) == VENPOD::Utils::Material::Air,
        "zero-budget gpu proposal leaves destination voxel unchanged");

    SparseVoxelWorld lateralWorld;
    Check(lateralWorld.Initialize({8, 32, 12345u}), "gpu lateral proposal world initialize");
    const uint32_t stone = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Stone, 0, 0, 0);
    lateralWorld.SetEditedVoxel(1, 17, 1, water);
    lateralWorld.SetEditedVoxel(1, 16, 1, stone);
    lateralWorld.SetEditedVoxel(2, 17, 1, air);
    SparsePhysicsPacketResult lateralProposal;
    lateralProposal.coord = BrickCoord{0, 1, 0};
    lateralProposal.destinationCoord = BrickCoord{0, 1, 0};
    lateralProposal.generation = 1u;
    lateralProposal.materialMask = 2u;
    lateralProposal.status =
        SPARSE_PHYSICS_PACKET_STATUS_CONSUMED |
        SPARSE_PHYSICS_PACKET_STATUS_PROPOSAL;
    lateralProposal.packedSourceLocal = 1u | (1u << 8u) | (1u << 16u);
    lateralProposal.packedDestinationLocal = 2u | (1u << 8u) | (1u << 16u);
    lateralProposal.sourceVoxel = water;
    lateralProposal.destinationVoxel = air;
    Check(lateralWorld.ApplyGpuPhysicsProposals({lateralProposal}, 4, false) == 1,
        "gpu lateral fluid proposal is CPU-authoritatively applied");
    Check(lateralWorld.GetEdits().TryGetVoxel(2, 17, 1, &destinationAfter) &&
          destinationAfter == water,
        "gpu lateral fluid proposal writes destination voxel");

    SparseVoxelWorld lateralSnapshotWorld;
    Check(lateralSnapshotWorld.Initialize({8, 32, 12345u}),
        "gpu physics lateral snapshot world initialize");
    lateralSnapshotWorld.SetEditedVoxel(15, 17, 1, water);
    lateralSnapshotWorld.SetEditedVoxel(16, 17, 1, air);
    Check(lateralSnapshotWorld.StageLocalPhysicsWork(1) == 1,
        "gpu physics lateral snapshot stages only the source edge packet");
    const std::vector<SparseEditDelta> lateralSnapshot =
        lateralSnapshotWorld.BuildGpuEditDeltaSnapshotForPhysicsWork(16);
    bool sawSourceBrickDelta = false;
    bool sawPositiveXNeighborDelta = false;
    for (const SparseEditDelta& delta : lateralSnapshot) {
        if (delta.coord == BrickCoord{0, 1, 0}) {
            sawSourceBrickDelta = true;
        }
        if (delta.coord == BrickCoord{1, 1, 0}) {
            sawPositiveXNeighborDelta = true;
        }
    }
    Check(sawSourceBrickDelta,
        "gpu physics edit-delta snapshot includes the staged source brick");
    Check(sawPositiveXNeighborDelta,
        "gpu physics edit-delta snapshot includes lateral neighbor edits for edge fluid proposals");
}

void TestSparseVoxelWorldLifecycle() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "sparse world initialize");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const BrickCoord coord{1, 1000, -3};
    world.SetEditedVoxel(
        coord.x * SPARSE_BRICK_SIZE + 1,
        coord.y * SPARSE_BRICK_SIZE + 1,
        coord.z * SPARSE_BRICK_SIZE + 1,
        stone);
    world.SetEditedVoxel(
        coord.x * SPARSE_BRICK_SIZE + 8,
        coord.y * SPARSE_BRICK_SIZE + 8,
        coord.z * SPARSE_BRICK_SIZE + 8,
        stone);
    Check(world.RequestBrick(coord), "world request brick");
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::Requested,
        "requested brick starts hidden");
    Check(!world.GetPool().IsResident(coord), "requested brick is not resident");
    Check(world.GetPool().PageTable().Count() == 0, "page table stays empty before upload completion");

    Check(world.PumpGeneration(1) == 1, "pump generation produces one upload");
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::UploadQueued,
        "generated brick queues upload");
    Check(!world.GetPool().IsResident(coord), "upload queued brick is still not resident");

    SparseBrickUploadPacket packet;
    Check(world.PopNextUpload(&packet), "pop upload packet");
    Check(packet.coord == coord, "upload packet coord");
    Check(packet.pageIndex != INVALID_BRICK_PAGE, "upload packet page");
    Check(packet.generation != 0, "upload packet generation");
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::UploadingGPU,
        "popped upload transitions to uploading gpu");
    Check(!world.GetPool().IsResident(coord), "uploading brick is still hidden");

    Check(world.RequeueUploadFront(packet), "failed upload can be requeued");
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::UploadQueued,
        "requeued upload returns to upload queued state");
    Check(world.PopNextUpload(&packet), "requeued upload can be popped again");

    Check(world.CompleteUpload(packet), "complete upload publishes brick");
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::Resident,
        "completed upload is resident");
    Check(world.GetPool().IsResident(coord), "resident lookup after complete upload");
    Check(world.GetPool().PageTable().Count() == 1, "page table has resident entry after upload");
    Check(world.GetStats().surfaceExtractionQueuedBricks == 1,
        "complete upload queues budgeted surface extraction");
    Check(world.GetStats().surfaceCachedBricks == 0,
        "surface cache waits for extraction budget after upload");
    Check(world.PumpSurfaceExtraction(1) == 1, "surface extraction pump builds uploaded brick surface");
    Check(world.GetStats().surfaceCachedBricks == 1, "surface cache has uploaded resident brick");
    Check(world.GetSurfaceCache().FindFaces(coord) != nullptr,
        "surface cache owns a range for uploaded resident brick");
    Check(world.GetStats().surfaceBricksUpdatedLastFrame == 1,
        "surface cache tracks resident upload update");

    uint32_t page = INVALID_BRICK_PAGE;
    uint32_t flags = 0;
    Check(world.GetPool().PageTable().TryLookupExactGeneration(coord, packet.generation, &page, &flags),
        "page table exact generation visible after complete upload");
    Check(page == packet.pageIndex, "page table points at uploaded page");

    const int32_t worldX = coord.x * SPARSE_BRICK_SIZE + 3;
    const int32_t worldY = coord.y * SPARSE_BRICK_SIZE + 4;
    const int32_t worldZ = coord.z * SPARSE_BRICK_SIZE + 5;
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

    world.SetEditedVoxel(worldX, worldY, worldZ, air);
    Check(world.GetPool().GetState(coord) == BrickLifecycleState::UploadQueued,
        "resident edit queues dirty upload");
    Check(world.GetStats().editedBricks == 1, "world edit records one edited brick");
    Check(world.GetStats().editedVoxels == 3, "world edit records persistent modified voxels");

    SparseBrickUploadPacket editPacket;
    Check(world.PopNextUpload(&editPacket), "pop dirty edit upload");
    Check(editPacket.coord == coord, "edit upload coord");
    Check(editPacket.generation == packet.generation, "dirty upload keeps generation before eviction");
    Check(world.CompleteUpload(editPacket), "complete dirty edit upload");
    Check(world.GetPool().IsResident(coord), "dirty upload returns to resident");
    Check(world.PumpSurfaceExtraction(1) == 1, "surface extraction pump refreshes dirty upload");
    Check(world.GetStats().surfaceCachedBricks == 1, "dirty upload keeps one cached surface brick");
    Check(world.GetStats().surfaceBricksUpdatedLastFrame >= 1,
        "dirty upload refreshes cached surface brick");

    world.BeginFrame();
    Check(world.GetStats().surfaceBricksUpdatedLastFrame == 0,
        "surface update counters reset at sparse world frame boundary");

    SparseVoxelWorld emptyUploadWorld;
    Check(emptyUploadWorld.Initialize({4, 16, 12345u}), "empty-upload sparse world initialize");
    const BrickCoord highAirCoord{0, 1000, 0};
    Check(emptyUploadWorld.RequestBrickDetailed(highAirCoord) == SparseBrickRequestResult::SkippedKnownEmpty,
        "request known empty high-air brick is classified as skipped empty");
    Check(emptyUploadWorld.GetStats().emptyRequestsSkippedLastFrame == 1,
        "known empty high-air request is rejected before allocation");
    Check(emptyUploadWorld.GetStats().knownEmptyGeneratedBricks == 1,
        "known empty high-air request is cached");
    Check(emptyUploadWorld.RequestBrickDetailed(highAirCoord) == SparseBrickRequestResult::SkippedKnownEmpty,
        "cached known empty high-air request is still skipped");
    Check(emptyUploadWorld.GetStats().knownEmptyGeneratedBricks == 1,
        "known empty cache deduplicates repeated skips");
    const BrickCoord secondHighAirCoord{1, 1000, 0};
    Check(emptyUploadWorld.TrySkipKnownEmptyRequest(secondHighAirCoord),
        "known empty fast path skips high-air brick before allocation");
    Check(emptyUploadWorld.GetStats().knownEmptyGeneratedBricks == 2,
        "known empty fast path records skipped high-air brick");
    Check(!emptyUploadWorld.GetPool().IsResident(secondHighAirCoord),
        "known empty fast path does not allocate a resident page");
    Check(emptyUploadWorld.PumpGeneration(1) == 0,
        "known empty high-air request creates no generation work");
    SparseBrickUploadPacket emptyPacket;
    Check(!emptyUploadWorld.PopNextUpload(&emptyPacket),
        "known empty high-air request creates no upload");
    Check(emptyUploadWorld.GetStats().surfaceExtractionQueuedBricks == 0,
        "known empty upload skips surface extraction queue");
    Check(emptyUploadWorld.PumpSurfaceExtraction(1) == 0,
        "known empty upload has no surface extraction work");
    Check(emptyUploadWorld.GetStats().surfaceCachedBricks == 0,
        "known empty upload does not create a cached surface");
    emptyUploadWorld.SetEditedVoxel(
        highAirCoord.x * SPARSE_BRICK_SIZE,
        highAirCoord.y * SPARSE_BRICK_SIZE,
        highAirCoord.z * SPARSE_BRICK_SIZE,
        stone);
    Check(emptyUploadWorld.GetStats().knownEmptyGeneratedBricks == 1,
        "editing a known empty brick invalidates only that empty cache entry");
    Check(!emptyUploadWorld.TrySkipKnownEmptyRequest(highAirCoord),
        "known empty fast path refuses edited high-air brick");
    Check(emptyUploadWorld.RequestBrickDetailed(highAirCoord) == SparseBrickRequestResult::Allocated,
        "edited high-air brick allocates despite generated-empty cache history");

    SparseVoxelWorld collisionOnly;
    Check(collisionOnly.Initialize({4, 16, 12345u}), "collision-only sparse world initialize");
    collisionOnly.SetEditedVoxel(900, 700, -900, stone);
    Check(collisionOnly.SampleCollisionStatus(900, 700, -900) == CollisionSampleStatus::KnownSolid,
        "collision samples persistent edit without render residency");
    Check(collisionOnly.GetPool().PageTable().Count() == 0,
        "collision edit did not require resident render page");
}

void TestSparseFixedGridReadiness() {
    SparseVoxelWorld world;
    Check(world.Initialize({96, 256, 12345u}), "fixed-grid sparse world initialize");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        5,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const BrickCoord center{0, 64, 0};
    Check(world.GetRenderReadinessState(center) == SparseRenderReadinessState::Missing,
        "fixed-grid readiness reports missing before request");
    std::vector<BrickCoord> grid;
    grid.reserve(27);
    for (int32_t z = -1; z <= 1; ++z) {
        for (int32_t y = -1; y <= 1; ++y) {
            for (int32_t x = -1; x <= 1; ++x) {
                const BrickCoord coord{center.x + x, center.y + y, center.z + z};
                grid.push_back(coord);
                world.SetEditedVoxel(
                    coord.x * SPARSE_BRICK_SIZE + 8,
                    coord.y * SPARSE_BRICK_SIZE + 8,
                    coord.z * SPARSE_BRICK_SIZE + 8,
                    stone);
                Check(world.RequestBrickDetailed(coord, false) == SparseBrickRequestResult::Allocated,
                    "fixed-grid request allocates exact sparse brick");
                Check(world.TouchResidencyClass(coord, SparseResidencyClass::Visible, 1),
                    "fixed-grid marks requested brick visible");
            }
        }
    }
    for (const BrickCoord& coord : grid) {
        Check(world.GetRenderReadinessState(coord) != SparseRenderReadinessState::ReadyToRender,
            "fixed-grid target brick is not ready before generation/upload");
    }

    Check(world.PumpGenerationAround(static_cast<uint32_t>(grid.size()), center, 1) == grid.size(),
        "fixed-grid synchronous generation completes target set");
    Check(world.GetStats().generationQueuedBricks == 0,
        "fixed-grid generation queue drains before render");
    for (const BrickCoord& coord : grid) {
        Check(world.GetRenderReadinessState(coord) == SparseRenderReadinessState::UploadQueued,
            "fixed-grid readiness reports upload-queued target after generation");
    }

    SparseBrickUploadPacket packet;
    uint32_t uploaded = 0;
    while (world.PopBestUploadForClass(&packet, SparseResidencyClass::Visible, center, 1)) {
        Check(world.GetRenderReadinessState(packet.coord) == SparseRenderReadinessState::UploadingGPU,
            "fixed-grid readiness reports uploading while packet is in flight");
        Check(world.CompleteUpload(packet),
            "fixed-grid upload completion publishes resident sparse brick");
        Check(world.GetRenderReadinessState(packet.coord) == SparseRenderReadinessState::UploadingGPU,
            "fixed-grid readiness waits for GPU page-table publish after upload completion");
        Check(world.MarkGpuPageTablePublished(packet.coord, packet.pageIndex, packet.generation),
            "fixed-grid marks GPU page table entry published");
        ++uploaded;
    }
    Check(uploaded == grid.size(),
        "fixed-grid synchronous upload publishes every target brick");
    Check(world.GetStats().uploadQueuedBricks == 0,
        "fixed-grid upload queue drains before render");
    Check(world.GetStats().residentRenderableBricks == grid.size(),
        "fixed-grid target set is resident and renderable before surface extraction");
    Check(world.GetStats().surfaceExtractionQueuedBricks == grid.size(),
        "fixed-grid target surfaces are queued before render");
    for (const BrickCoord& coord : grid) {
        Check(world.GetRenderReadinessState(coord) == SparseRenderReadinessState::ResidentMissingSurface,
            "fixed-grid readiness reports resident-missing-surface before extraction");
    }

    Check(world.PumpSurfaceExtractionAround(static_cast<uint32_t>(grid.size()), center, 1) == grid.size(),
        "fixed-grid synchronous surface extraction completes target set");
    Check(world.GetStats().surfaceExtractionQueuedBricks == 0,
        "fixed-grid surface queue drains before render");
    Check(world.GetStats().surfaceCachedBricks == grid.size(),
        "fixed-grid surface cache has every target brick");
    Check(world.GetStats().residentRenderableMissingSurfaces == 0,
        "fixed-grid ready-to-render contract has no resident bricks missing surfaces");
    const SparseRenderReadinessStats readiness = world.BuildRenderReadinessStats();
    Check(readiness.readyToRender >= grid.size(),
        "fixed-grid readiness stats count target bricks as ready to render");

    for (const BrickCoord& coord : grid) {
        Check(world.GetPool().IsResident(coord),
            "fixed-grid target brick remains resident");
        Check(world.GetSurfaceCache().FindFaces(coord) != nullptr,
            "fixed-grid target brick has cached surface faces");
        Check(world.GetRenderReadinessState(coord) == SparseRenderReadinessState::ReadyToRender,
            "fixed-grid target brick reaches renderer-facing ready state");
    }
}

void TestSparseRenderDirtyRegions() {
    SparseVoxelWorld world;
    Check(world.Initialize({4, 16, 12345u}), "render-dirty sparse world initialize");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        3,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

    const int32_t x = -17;
    const int32_t y = 700;
    const int32_t z = 31;
    const BrickCoord coord = BrickCoord::FromWorldVoxel(x, y, z);

    world.SetEditedVoxel(x, y, z, stone);
    Check(world.GetStats().renderDirtyBricks == 1,
        "single sparse edit tracks one render-dirty brick");
    Check(world.GetStats().renderDirtyRegionVoxels == 1,
        "single sparse edit tracks one render-dirty voxel");
    Check(world.GetStats().renderDirtyVoxelsQueuedLastFrame == 1,
        "single sparse edit reports one queued render-dirty voxel");
    Check(world.GetStats().renderDirtyNonResidentLastFrame == 1,
        "nonresident sparse edit does not force render-page allocation");
    Check(world.GetStats().requestedBricks == 0,
        "render-dirty bookkeeping stays separate from render residency");

    Check(world.RequestBrickDetailed(coord) == SparseBrickRequestResult::Allocated,
        "render-dirty edited brick can be requested later");
    Check(world.PumpGeneration(1) == 1,
        "render-dirty edited brick generates on demand");
    SparseBrickUploadPacket packet;
    Check(world.PopNextUpload(&packet), "render-dirty edited brick queues upload");
    Check(packet.coord == coord, "render-dirty upload uses edited coord");
    Check(world.CompleteUpload(packet), "render-dirty upload completes");
    Check(world.GetStats().renderDirtyBricks == 0,
        "published render-dirty brick clears pending dirty region");

    world.BeginFrame();
    world.SetEditedVoxel(x, y, z, air);
    Check(world.GetStats().renderDirtyBricks == 1,
        "resident sparse edit re-dirties render brick");
    Check(world.GetStats().renderDirtyFullUploadsQueuedLastFrame == 1,
        "resident sparse edit queues one compatibility full-brick upload");
    Check(world.PopNextUpload(&packet), "resident render-dirty edit queues refresh upload");
    Check(packet.partialVoxelUpload,
        "resident one-voxel render dirty refresh uses partial voxel upload");
    Check(packet.voxelCount == 1,
        "resident one-voxel render dirty refresh stages one voxel");
    Check(world.CompleteUpload(packet), "resident render-dirty refresh completes");
    Check(world.GetStats().renderDirtyBricks == 0,
        "resident render-dirty refresh clears dirty region");

    SparseVoxelWorld brushWorld;
    Check(brushWorld.Initialize({8, 32, 12345u}), "brush render-dirty sparse world initialize");
    const uint32_t painted = brushWorld.ApplyBrushEdit(
        900.5f,
        700.5f,
        -900.5f,
        1.1f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        77u,
        0,
        0,
        0,
        false,
        false);
    Check(painted > 0, "brush render-dirty test paints voxels");
    Check(brushWorld.GetStats().renderDirtyBricks > 0,
        "brush edit tracks render-dirty bricks without render request");
    Check(brushWorld.GetStats().renderDirtyRegionVoxels > 0 &&
          brushWorld.GetStats().renderDirtyRegionVoxels < SPARSE_BRICK_VOXEL_COUNT,
        "brush edit keeps render-dirty region narrower than a full brick");
}

void TestSparseLocalPhysics() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 64, 12345u}), "sparse physics world initialize");

    const uint32_t sand = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Sand,
        1,
        0,
        0);
    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

    const int32_t x = 41;
    const int32_t y = 912;
    const int32_t z = -37;
    world.SetEditedVoxel(x, y, z, sand);
    Check(world.SampleCollisionStatus(x, y, z) == CollisionSampleStatus::KnownSolid,
        "sparse physics source starts solid");
    Check(world.SampleCollisionStatus(x, y - 1, z) == CollisionSampleStatus::KnownAir,
        "sparse physics destination starts air");
    Check(world.GetStats().physicsCandidateBricks == 1,
        "sparse physics queues edited brick");

    Check(world.StepLocalPhysics(1, 1, false) == 1,
        "sparse physics moves one falling voxel");
    Check(world.SampleCollisionStatus(x, y, z) == CollisionSampleStatus::KnownAir,
        "sparse physics clears source voxel");
    Check(world.SampleCollisionStatus(x, y - 1, z) == CollisionSampleStatus::KnownSolid,
        "sparse physics writes destination voxel");
    Check(world.GetStats().physicsProcessedBricksLastFrame == 1,
        "sparse physics reports processed brick");
    Check(world.GetStats().physicsMovedVoxelsLastFrame == 1,
        "sparse physics reports moved voxel");
    Check(world.GetStats().physicsHotWorkPacketsLastFrame == 1 &&
          world.GetStats().physicsWarmWorkPacketsLastFrame == 0,
        "sparse physics reports hot edit work packet priority");
    Check(world.GetStats().physicsDirtyRegionVoxelsLastFrame == 1,
        "sparse physics reports single-voxel dirty region for edit-driven work");

    SparseVoxelWorld blocked;
    Check(blocked.Initialize({8, 64, 12345u}), "blocked sparse physics world initialize");
    blocked.SetEditedVoxel(-33, 850, 65, sand);
    blocked.SetEditedVoxel(-33, 849, 65, stone);
    Check(blocked.StepLocalPhysics(4, 4, false) == 0,
        "sparse physics does not move into occupied voxel");
    Check(blocked.SampleCollisionStatus(-33, 850, 65) == CollisionSampleStatus::KnownSolid,
        "blocked sparse physics source remains solid");
    Check(blocked.SampleCollisionStatus(-33, 849, 65) == CollisionSampleStatus::KnownSolid,
        "blocked sparse physics blocker remains solid");
    Check(blocked.GetStats().physicsCandidateBricks == 0,
        "blocked sparse physics settles without hot queue spin");
    blocked.SetEditedVoxel(-33, 849, 65, air);
    Check(blocked.StepLocalPhysics(4, 1, false) == 1,
        "erasing sparse support wakes falling voxel above");
    Check(blocked.SampleCollisionStatus(-33, 850, 65) == CollisionSampleStatus::KnownAir,
        "support-wake sparse physics clears unsupported source");
    Check(blocked.SampleCollisionStatus(-33, 849, 65) == CollisionSampleStatus::KnownSolid,
        "support-wake sparse physics drops voxel onto erased support position");
    Check(blocked.GetStats().physicsDirtyRegionVoxelsLastFrame < SPARSE_BRICK_VOXEL_COUNT,
        "support-wake sparse physics stays region-local instead of scanning full bricks");

    SparseVoxelWorld priority;
    Check(priority.Initialize({8, 64, 12345u}), "priority sparse physics world initialize");
    const BrickCoord priorityCoord{4, 70, -2};
    priority.QueuePhysicsCandidate(priorityCoord);
    Check(priority.GetStats().physicsWarmCandidateBricks == 1,
        "generic sparse physics candidate starts warm");
    priority.SetEditedVoxel(
        priorityCoord.x * SPARSE_BRICK_SIZE + 2,
        priorityCoord.y * SPARSE_BRICK_SIZE + 3,
        priorityCoord.z * SPARSE_BRICK_SIZE + 4,
        sand);
    Check(priority.GetStats().physicsCandidateBricks == 1,
        "hot sparse edit upgrades existing warm candidate without duplicate candidate count");
    Check(priority.GetStats().physicsHotCandidateBricks == 1 &&
          priority.GetStats().physicsWarmCandidateBricks == 0,
        "hot sparse edit takes priority over stale warm queue entry");

    SparseVoxelWorld budget;
    Check(budget.Initialize({8, 64, 12345u}), "budget sparse physics world initialize");
    budget.SetEditedVoxel(80, 900, 80, sand);
    budget.SetEditedVoxel(112, 900, 80, sand);
    Check(budget.GetStats().physicsCandidateBricks == 2,
        "budget sparse physics setup creates two candidate bricks");
    Check(budget.StepLocalPhysics(1, 1, false) == 1,
        "sparse physics brick budget processes only one candidate");
    Check(budget.GetStats().physicsWorkPacketsLastFrame == 1,
        "sparse physics reports one consumed work packet under brick budget");
    Check(budget.GetStats().physicsDirtyRegionVoxelsLastFrame == 1,
        "sparse physics brick budget keeps dirty-region work narrow");
    Check(budget.GetStats().physicsCandidateBricks >= 1,
        "sparse physics leaves remaining candidate queued under brick budget");

    SparseVoxelWorld zeroMoveBudget;
    Check(zeroMoveBudget.Initialize({8, 64, 12345u}),
        "zero-move sparse physics world initialize");
    zeroMoveBudget.SetEditedVoxel(96, 900, 96, sand);
    Check(zeroMoveBudget.StageLocalPhysicsWork(1) == 1,
        "zero-move sparse physics stages one work packet");
    Check(zeroMoveBudget.ExecuteStagedLocalPhysics(0, false) == 0,
        "zero-move sparse physics applies no moves");
    Check(zeroMoveBudget.GetStats().physicsCandidateBricks >= 1,
        "zero-move sparse physics requeues staged work");
    Check(zeroMoveBudget.SampleCollisionStatus(96, 900, 96) == CollisionSampleStatus::KnownSolid,
        "zero-move sparse physics leaves source voxel unchanged");
    Check(zeroMoveBudget.SampleCollisionStatus(96, 899, 96) == CollisionSampleStatus::KnownAir,
        "zero-move sparse physics leaves destination voxel unchanged");

    SparseVoxelWorld boundaryRequest;
    Check(boundaryRequest.Initialize({8, 64, 12345u}),
        "boundary-request sparse physics world initialize");
    boundaryRequest.QueuePhysicsCandidate(BrickCoord{0, std::numeric_limits<int32_t>::min(), 0});
    Check(boundaryRequest.StageLocalPhysicsWork(UINT32_MAX) == 1,
        "boundary sparse physics caps oversized staging request");
    Check(boundaryRequest.GetStats().physicsSupportBricksRequestedLastFrame == 0,
        "boundary sparse physics skips overflowing support brick request");
    Check(boundaryRequest.GetStats().requestedBricks == 0,
        "boundary sparse physics does not wrap below-brick request");

    SparseVoxelWorld bottomBoundary;
    Check(bottomBoundary.Initialize({8, 64, 12345u}),
        "bottom-boundary sparse physics world initialize");
    const int32_t bottomY = std::numeric_limits<int32_t>::min();
    bottomBoundary.SetEditedVoxel(0, bottomY, 0, sand);
    Check(bottomBoundary.StageLocalPhysicsWork(UINT32_MAX) == 1,
        "bottom-boundary sparse physics stages capped work");
    Check(bottomBoundary.ExecuteStagedLocalPhysics(1, false) == 0,
        "bottom-boundary sparse physics does not move through int32 minimum");
    Check(bottomBoundary.SampleCollisionStatus(0, bottomY, 0) == CollisionSampleStatus::KnownSolid,
        "bottom-boundary sparse physics leaves source voxel in place");
    Check(bottomBoundary.GetStats().physicsSkippedVoxelsLastFrame > 0,
        "bottom-boundary sparse physics reports skipped overflowing move");
}

struct TestFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

TestFloat3 Subtract(TestFloat3 lhs, TestFloat3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

TestFloat3 Cross(TestFloat3 lhs, TestFloat3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

float Dot(TestFloat3 lhs, TestFloat3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

TestFloat3 TestFaceNormal(uint32_t direction) {
    switch (direction) {
        case static_cast<uint32_t>(SparseFaceDirection::NegX): return {-1.0f, 0.0f, 0.0f};
        case static_cast<uint32_t>(SparseFaceDirection::PosX): return { 1.0f, 0.0f, 0.0f};
        case static_cast<uint32_t>(SparseFaceDirection::NegY): return {0.0f, -1.0f, 0.0f};
        case static_cast<uint32_t>(SparseFaceDirection::PosY): return {0.0f,  1.0f, 0.0f};
        case static_cast<uint32_t>(SparseFaceDirection::NegZ): return {0.0f, 0.0f, -1.0f};
        default: return {0.0f, 0.0f, 1.0f};
    }
}

TestFloat3 TestFaceCorner(const SparseSurfaceFace& face, uint32_t corner) {
    const uint32_t direction = SparseSurfacePayloadDirection(face.payload);
    const float x0 = static_cast<float>(face.worldX);
    const float y0 = static_cast<float>(face.worldY);
    const float z0 = static_cast<float>(face.worldZ);
    const float width = static_cast<float>(SparseSurfacePayloadWidth(face.payload));
    const float height = static_cast<float>(SparseSurfacePayloadHeight(face.payload));
    const float x1 = x0 + (direction == 2u || direction == 3u || direction == 4u || direction == 5u ? width : 1.0f);
    const float y1 = y0 + (direction == 0u || direction == 1u || direction == 4u || direction == 5u ? height : 1.0f);
    const float z1 = z0 + (direction == 0u || direction == 1u ? width : direction == 2u || direction == 3u ? height : 1.0f);

    if (direction == 0u) {
        return corner == 0u ? TestFloat3{x0, y0, z1} :
            corner == 1u ? TestFloat3{x0, y1, z1} :
            corner == 2u ? TestFloat3{x0, y1, z0} :
                            TestFloat3{x0, y0, z0};
    }
    if (direction == 1u) {
        return corner == 0u ? TestFloat3{x1, y0, z0} :
            corner == 1u ? TestFloat3{x1, y1, z0} :
            corner == 2u ? TestFloat3{x1, y1, z1} :
                            TestFloat3{x1, y0, z1};
    }
    if (direction == 2u) {
        return corner == 0u ? TestFloat3{x0, y0, z0} :
            corner == 1u ? TestFloat3{x1, y0, z0} :
            corner == 2u ? TestFloat3{x1, y0, z1} :
                            TestFloat3{x0, y0, z1};
    }
    if (direction == 3u) {
        return corner == 0u ? TestFloat3{x0, y1, z1} :
            corner == 1u ? TestFloat3{x1, y1, z1} :
            corner == 2u ? TestFloat3{x1, y1, z0} :
                            TestFloat3{x0, y1, z0};
    }
    if (direction == 4u) {
        return corner == 0u ? TestFloat3{x1, y0, z0} :
            corner == 1u ? TestFloat3{x0, y0, z0} :
            corner == 2u ? TestFloat3{x0, y1, z0} :
                            TestFloat3{x1, y1, z0};
    }
    return corner == 0u ? TestFloat3{x0, y0, z1} :
        corner == 1u ? TestFloat3{x1, y0, z1} :
        corner == 2u ? TestFloat3{x1, y1, z1} :
                        TestFloat3{x0, y1, z1};
}

void TestSparseSurfaceWindingContract() {
    for (uint32_t direction = 0; direction < 6u; ++direction) {
        SparseSurfaceFace face;
        face.worldX = -3;
        face.worldY = 5;
        face.worldZ = 7;
        face.payload = PackSparseSurfacePayload(direction, 3u, 5u, 4u);

        const TestFloat3 v0 = TestFaceCorner(face, 0u);
        const TestFloat3 v1 = TestFaceCorner(face, 1u);
        const TestFloat3 v2 = TestFaceCorner(face, 2u);
        const TestFloat3 windingNormal = Cross(Subtract(v1, v0), Subtract(v2, v0));
        const TestFloat3 expectedNormal = TestFaceNormal(direction);
        Check(Dot(windingNormal, expectedNormal) > 0.0f,
            "sparse surface face corner order is outward CCW for fixed-function culling");
    }
}

void TestSparseSurfaceExtraction() {
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    const uint32_t stone = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Stone, 7, 0, 0);
    const uint32_t water = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Water, 3, 0, 0);

    GeneratedSparseBrick single;
    single.coord = BrickCoord{-2, 3, -4};
    single.voxels.fill(air);
    single.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 2, 3})] = stone;
    auto singleResult = SparseSurfaceExtractor::Extract(single);
    Check(singleResult.stats.solidVoxels == 1, "surface extractor counts single solid voxel");
    Check(singleResult.stats.exposedFaces == 6, "single isolated voxel exposes six faces");
    for (uint32_t count : singleResult.stats.facesByDirection) {
        Check(count == 1, "single isolated voxel exposes one face per direction");
    }
    Check(singleResult.faces.front().worldX == -31, "surface face preserves negative world X");
    Check(singleResult.faces.front().worldY == 50, "surface face preserves positive world Y");
    Check(singleResult.faces.front().worldZ == -61, "surface face preserves negative world Z");
    Check(SparseSurfacePayloadVoxel(singleResult.faces.front().payload) == (stone & kSparseSurfaceVoxelPayloadMask),
        "surface face packs voxel payload into compact surface record");
    Check(SparseSurfacePayloadDirection(singleResult.faces.front().payload) <=
            static_cast<uint32_t>(SparseFaceDirection::PosZ),
        "surface face packs direction into compact surface record");

    GeneratedSparseBrick liquidOnly;
    liquidOnly.coord = BrickCoord{0, 0, 0};
    liquidOnly.voxels.fill(air);
    liquidOnly.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 2, 3})] = water;
    auto liquidOnlyResult = SparseSurfaceExtractor::Extract(liquidOnly);
    Check(liquidOnlyResult.stats.solidVoxels == 0,
        "surface extractor does not count liquid as solid terrain");
    Check(liquidOnlyResult.stats.exposedFaces == 2 && liquidOnlyResult.faces.size() == 2,
        "surface extractor emits exposed liquid top and underwater underside faces");
    bool foundWaterTop = false;
    bool foundWaterUnderside = false;
    for (const SparseSurfaceFace& face : liquidOnlyResult.faces) {
        const uint32_t direction = SparseSurfacePayloadDirection(face.payload);
        foundWaterTop = foundWaterTop || direction == static_cast<uint32_t>(SparseFaceDirection::PosY);
        foundWaterUnderside = foundWaterUnderside || direction == static_cast<uint32_t>(SparseFaceDirection::NegY);
        Check(SparseSurfacePayloadVoxel(face.payload) == (water & kSparseSurfaceVoxelPayloadMask),
            "surface extractor preserves liquid material payload");
    }
    Check(foundWaterTop && foundWaterUnderside,
        "surface extractor keeps liquid surfaces visible from above and below for sparse raster");

    GeneratedSparseBrick waterOverTerrain;
    waterOverTerrain.coord = BrickCoord{0, 0, 0};
    waterOverTerrain.voxels.fill(air);
    waterOverTerrain.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 1, 1})] = stone;
    waterOverTerrain.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 2, 1})] = water;
    const auto waterOverTerrainResult = SparseSurfaceExtractor::Extract(waterOverTerrain);
    bool foundStoneFaceAgainstWater = false;
    bool foundOwnedWaterTop = false;
    for (const SparseSurfaceFace& face : waterOverTerrainResult.faces) {
        const uint32_t direction = SparseSurfacePayloadDirection(face.payload);
        const uint32_t payloadVoxel = SparseSurfacePayloadVoxel(face.payload);
        if (payloadVoxel == (stone & kSparseSurfaceVoxelPayloadMask) &&
            direction == static_cast<uint32_t>(SparseFaceDirection::PosY)) {
            foundStoneFaceAgainstWater = true;
        }
        if (payloadVoxel == (water & kSparseSurfaceVoxelPayloadMask) &&
            direction == static_cast<uint32_t>(SparseFaceDirection::PosY)) {
            foundOwnedWaterTop = true;
        }
    }
    Check(!foundStoneFaceAgainstWater,
        "surface extractor suppresses solid faces whose visible boundary is owned by water");
    Check(foundOwnedWaterTop,
        "surface extractor keeps water as the visible owner above submerged terrain");

    GeneratedSparseBrick waterBesideTerrain;
    waterBesideTerrain.coord = BrickCoord{0, 0, 0};
    waterBesideTerrain.voxels.fill(air);
    waterBesideTerrain.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 1, 1})] = stone;
    waterBesideTerrain.voxels[LocalVoxelIndex(LocalVoxelCoord{2, 1, 1})] = water;
    const auto waterBesideTerrainResult = SparseSurfaceExtractor::Extract(waterBesideTerrain);
    bool foundStoneBankAgainstWater = false;
    for (const SparseSurfaceFace& face : waterBesideTerrainResult.faces) {
        const uint32_t direction = SparseSurfacePayloadDirection(face.payload);
        const uint32_t payloadVoxel = SparseSurfacePayloadVoxel(face.payload);
        if (payloadVoxel == (stone & kSparseSurfaceVoxelPayloadMask) &&
            direction == static_cast<uint32_t>(SparseFaceDirection::PosX)) {
            foundStoneBankAgainstWater = true;
        }
    }
    Check(foundStoneBankAgainstWater,
        "surface extractor keeps solid side banks against water so shoreline terrain remains continuous");

    auto singleRegionResult = SparseSurfaceExtractor::ExtractRegion(
        single,
        SparseSurfaceLocalRegion{1, 2, 3, 1, 2, 3});
    Check(singleRegionResult.stats.solidVoxels == 1,
        "surface region extractor counts single dirty solid voxel");
    Check(singleRegionResult.stats.exposedFaces == 6,
        "surface region extractor emits same faces for isolated dirty voxel");
    auto emptyRegionResult = SparseSurfaceExtractor::ExtractRegion(
        single,
        SparseSurfaceLocalRegion{2, 2, 3, 2, 2, 3});
    Check(emptyRegionResult.stats.solidVoxels == 0 &&
          emptyRegionResult.stats.exposedFaces == 0,
        "surface region extractor skips solids outside dirty region");

    GeneratedSparseBrick full;
    full.coord = BrickCoord{0, 0, 0};
    full.voxels.fill(stone);
    auto fullResult = SparseSurfaceExtractor::Extract(full);
    Check(fullResult.stats.solidVoxels == SPARSE_BRICK_VOXEL_COUNT,
        "full brick counts all solid voxels");
    Check(fullResult.stats.exposedFaces == 6u * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "full brick exposes only outer shell unit faces");
    Check(fullResult.faces.size() == 6,
        "full brick greedy surface extraction merges each shell sheet into one quad");
    uint32_t fullMergedArea = 0;
    for (const SparseSurfaceFace& face : fullResult.faces) {
        Check(SparseSurfacePayloadWidth(face.payload) == SPARSE_BRICK_SIZE,
            "full brick merged shell quad spans full brick width");
        Check(SparseSurfacePayloadHeight(face.payload) == SPARSE_BRICK_SIZE,
            "full brick merged shell quad spans full brick height");
        fullMergedArea += SparseSurfacePayloadWidth(face.payload) * SparseSurfacePayloadHeight(face.payload);
    }
    Check(fullMergedArea == fullResult.stats.exposedFaces,
        "full brick merged quad area preserves exact exposed unit-face count");
    for (uint32_t count : fullResult.stats.facesByDirection) {
        Check(count == SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
            "full brick exposes one face sheet per direction");
    }

    GeneratedSparseBrick plate;
    plate.coord = BrickCoord{1, 0, -1};
    plate.voxels.fill(air);
    for (uint8_t z = 0; z < 2; ++z) {
        for (uint8_t x = 0; x < 4; ++x) {
            plate.voxels[LocalVoxelIndex(LocalVoxelCoord{x, 0, z})] = stone;
        }
    }
    auto plateResult = SparseSurfaceExtractor::Extract(plate);
    bool foundMergedTop = false;
    for (const SparseSurfaceFace& face : plateResult.faces) {
        if (SparseSurfacePayloadDirection(face.payload) == static_cast<uint32_t>(SparseFaceDirection::PosY) &&
            SparseSurfacePayloadWidth(face.payload) == 4u &&
            SparseSurfacePayloadHeight(face.payload) == 2u) {
            foundMergedTop = true;
            break;
        }
    }
    Check(foundMergedTop,
        "greedy surface extraction merges same-material coplanar top faces into a traversal-scale quad");

    auto posXBlocked = SparseSurfaceExtractor::Extract(
        full,
        [stone](int32_t worldX, int32_t, int32_t) {
            return worldX == SPARSE_BRICK_SIZE ? stone : 0u;
        });
    Check(posXBlocked.stats.exposedFaces == fullResult.stats.exposedFaces -
            SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "neighbor sampler suppresses cross-brick exposed faces");
    Check(posXBlocked.stats.facesByDirection[static_cast<size_t>(SparseFaceDirection::PosX)] == 0,
        "positive X sheet is fully occluded by neighbor brick");

    GeneratedSparseBrick negativeFull;
    negativeFull.coord = BrickCoord{-1, -1, -1};
    negativeFull.voxels.fill(stone);
    auto allBoundaryNeighborsSolid = SparseSurfaceExtractor::Extract(
        negativeFull,
        [stone](int32_t, int32_t, int32_t) {
            return stone;
        });
    Check(allBoundaryNeighborsSolid.stats.exposedFaces == 0,
        "neighbor sampler suppresses all exterior sheets at negative brick coordinates");
    Check(allBoundaryNeighborsSolid.faces.empty(),
        "negative-coordinate halo samples prevent fabricated air gaps at every brick boundary");

    auto negXBlocked = SparseSurfaceExtractor::Extract(
        negativeFull,
        [stone](int32_t worldX, int32_t, int32_t) {
            return worldX == -17 ? stone : 0u;
        });
    Check(negXBlocked.stats.exposedFaces == fullResult.stats.exposedFaces -
            SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "negative X deterministic neighbor sample suppresses one exterior sheet");
    Check(negXBlocked.stats.facesByDirection[static_cast<size_t>(SparseFaceDirection::NegX)] == 0,
        "negative X sheet is fully occluded by halo/world sampling");

    GeneratedSparseBrick overflowCoordBrick;
    overflowCoordBrick.coord = BrickCoord{std::numeric_limits<int32_t>::max(), 0, 0};
    overflowCoordBrick.voxels.fill(stone);
    auto overflowCoordResult = SparseSurfaceExtractor::Extract(overflowCoordBrick);
    Check(overflowCoordResult.faces.empty() &&
          overflowCoordResult.stats.solidVoxels == 0 &&
          overflowCoordResult.stats.exposedFaces == 0,
        "surface extractor rejects brick coords that would overflow world voxels");

    GeneratedSparseBrick maxBoundaryBrick;
    maxBoundaryBrick.coord = BrickCoord{134217727, 0, 0};
    maxBoundaryBrick.voxels.fill(stone);
    uint32_t wrappedNeighborCalls = 0;
    auto maxBoundaryResult = SparseSurfaceExtractor::Extract(
        maxBoundaryBrick,
        [&wrappedNeighborCalls](int32_t worldX, int32_t, int32_t) {
            if (worldX == std::numeric_limits<int32_t>::min()) {
                ++wrappedNeighborCalls;
            }
            return 0u;
        });
    Check(wrappedNeighborCalls == 0,
        "surface extractor skips overflowing neighbor samples at signed world boundary");
    Check(maxBoundaryResult.stats.exposedFaces == fullResult.stats.exposedFaces,
        "surface extractor still exposes boundary faces when neighbor sample overflows");
}

void TestSparseSurfaceCache() {
    SparseSurfaceFace previousFaces[6] = {};
    SparseSurfaceFace currentFaces[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        previousFaces[i].worldX = static_cast<int32_t>(i);
        currentFaces[i].worldX = static_cast<int32_t>(i);
    }
    currentFaces[1].payload = PackSparseSurfacePayload(0u, 2u);
    currentFaces[2].payload = PackSparseSurfacePayload(0u, 2u);
    currentFaces[5].payload = PackSparseSurfacePayload(4u, 0u);
    auto changedRuns = BuildSparseSurfaceChangedFaceRuns(currentFaces, previousFaces, 6u);
    Check(changedRuns.size() == 2, "surface face diff groups adjacent changes");
    Check(changedRuns[0].firstFace == 1 && changedRuns[0].faceCount == 2,
        "surface face diff first run covers adjacent changes");
    Check(changedRuns[1].firstFace == 5 && changedRuns[1].faceCount == 1,
        "surface face diff second run covers trailing change");
    auto noRuns = BuildSparseSurfaceChangedFaceRuns(previousFaces, previousFaces, 6u);
    Check(noRuns.empty(), "surface face diff emits no runs for identical payloads");
    auto missingMirrorRuns = BuildSparseSurfaceChangedFaceRuns(currentFaces, nullptr, 6u);
    Check(missingMirrorRuns.size() == 1 &&
        missingMirrorRuns[0].firstFace == 0 &&
        missingMirrorRuns[0].faceCount == 6,
        "surface face diff falls back to full run without mirror");

    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    const uint32_t stone = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Stone, 0, 0, 0);
    const uint32_t dirt = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Dirt, 0, 0, 0);

    SparseSurfaceCache mergedCache;
    GeneratedSparseBrick mergedFull;
    mergedFull.coord = BrickCoord{12, 0, 0};
    mergedFull.voxels.fill(stone);
    SparseTerrainGenerator::ComputeOccupancyAndFlags(mergedFull);
    Check(mergedCache.UpdateBrick(mergedFull), "surface cache accepts fully merged brick");
    Check(mergedCache.GetStats().totalFaces == 6,
        "surface cache stores merged full-brick shell as six primitives");
    Check(mergedCache.GetStats().totalUnitFaces == 6u * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "surface cache tracks merged full-brick unit-face area separately from primitive count");
    mergedFull.voxels[LocalVoxelIndex(LocalVoxelCoord{8, 15, 8})] = air;
    SparseTerrainGenerator::ComputeOccupancyAndFlags(mergedFull);
    mergedCache.BeginFrame();
    Check(mergedCache.UpdateBrickRegion(mergedFull, SparseSurfaceLocalRegion{8, 15, 8, 8, 15, 8}),
        "surface cache refreshes dirty region intersecting merged quad");
    Check(mergedCache.GetStats().bricksPartiallyUpdatedLastFrame == 0,
        "surface cache falls back to full brick refresh when dirty region intersects merged quad");
    Check(mergedCache.GetStats().totalFaces > 6,
        "surface cache full refresh preserves unedited merged-quad remainder around carved voxel");
    Check(mergedCache.GetStats().totalUnitFaces > 6u * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "surface cache unit-face metric tracks carved voxel exposure after full refresh");

    GeneratedSparseBrick brick;
    brick.coord = BrickCoord{2, -1, 3};
    brick.voxels.fill(air);
    brick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;

    SparseSurfaceCache cache;
    GeneratedSparseBrick emptyBrick;
    emptyBrick.coord = BrickCoord{44, 0, 0};
    emptyBrick.voxels.fill(air);
    SparseTerrainGenerator::ComputeOccupancyAndFlags(emptyBrick);
    Check(cache.UpdateBrick(emptyBrick), "surface cache accepts empty brick update");
    Check(cache.GetStats().cachedBricks == 0, "surface cache does not store empty new bricks");
    Check(cache.GetStats().knownBricks == 1, "surface cache records empty brick as surface-known");
    Check(cache.GetStats().knownEmptySurfaceBricks == 1,
        "surface cache distinguishes known zero-face surface bricks");
    Check(cache.GetStats().totalFaces == 0, "surface cache empty update adds no faces");
    Check(cache.GetStats().totalUnitFaces == 0, "surface cache empty update adds no unit faces");
    Check(cache.GetStats().serial == 0, "surface cache empty new brick does not dirty GPU state");
    Check(cache.GetStats().emptyFastPathBricksLastFrame == 1,
        "surface cache counts empty-brick fast-path updates");

    cache.BeginFrame();
    Check(cache.UpdateBrick(brick), "surface cache accepts first brick update");
    Check(cache.GetStats().cachedBricks == 1, "surface cache stores one brick");
    Check(cache.GetStats().knownBricks == 2, "surface cache tracks known empty plus drawable brick");
    Check(cache.GetStats().totalFaces == 6, "surface cache tracks first brick faces");
    Check(cache.GetStats().totalUnitFaces == 6, "surface cache tracks first brick unit faces");
    Check(cache.GetStats().bricksUpdatedLastFrame == 1, "surface cache update counter increments");
    const auto* faces = cache.FindFaces(brick.coord);
    Check(faces && faces->size() == 6, "surface cache returns stored faces");

    brick.voxels[LocalVoxelIndex(LocalVoxelCoord{1, 0, 0})] = dirt;
    cache.BeginFrame();
    Check(cache.UpdateBrick(brick), "surface cache replaces existing brick faces");
    Check(cache.GetStats().cachedBricks == 1, "surface cache replacement keeps one brick");
    Check(cache.GetStats().totalFaces == 10, "two adjacent voxels expose ten faces");
    Check(cache.GetStats().facesGeneratedLastUpdate == 10, "surface cache reports generated faces");

    std::vector<SparseSurfaceFace> contiguous;
    Check(cache.BuildContiguousFaceList(contiguous), "surface cache builds contiguous face list");
    Check(contiguous.size() == 10, "contiguous face list has expected count");
    SparseSurfaceGpuSnapshot snapshot;
    Check(cache.BuildGpuSnapshot(snapshot), "surface cache builds gpu snapshot");
    Check(snapshot.faces.size() == 10, "surface snapshot face count");
    Check(snapshot.rangeCount == 1, "surface snapshot range count");
    Check(snapshot.drawCommandCount == 1, "surface snapshot emits one draw command");
    Check(snapshot.drawArgs.size() == 1, "surface snapshot draw arg vector count");
    Check(snapshot.drawBatches.size() == 1, "surface snapshot draw batch vector count");
    Check(snapshot.surfaceRecords.size() == 1, "surface snapshot surface record vector count");
    Check(snapshot.drawArgs[0].indexCountPerInstance == 60, "surface draw arg emits six indices per face");
    Check(snapshot.drawArgs[0].instanceCount == 1, "surface draw arg uses one indexed surface instance");
    Check(snapshot.drawArgs[0].startIndexLocation == 0, "surface draw arg starts at first generated face index");
    Check(snapshot.drawArgs[0].baseVertexLocation == 0, "surface draw arg keeps vertex base at zero");
    Check(snapshot.drawArgs[0].startInstanceLocation == 0, "surface draw arg starts generated instances at zero");
    Check(snapshot.drawBatches[0].coord == brick.coord, "surface draw batch coord");
    Check(snapshot.drawBatches[0].firstFace == 0, "surface draw batch first face");
    Check(snapshot.drawBatches[0].faceCount == 10, "surface draw batch face count");
    Check(snapshot.surfaceRecords[0].coord == brick.coord, "surface record coord");
    Check(snapshot.surfaceRecords[0].firstFace == 0, "surface record first face");
    Check(snapshot.surfaceRecords[0].faceCount == 10, "surface record face count");
    Check(snapshot.surfaceRecords[0].flags != 0, "surface record valid flag");
    Check((snapshot.surfaceRecords[0].flags & kSparseSurfaceRangeValid) != 0,
        "surface record keeps low valid bit");
    Check(SparseSurfaceRecordDirectionMask(snapshot.surfaceRecords[0].flags) ==
            BuildSparseSurfaceDirectionMask(snapshot.faces),
        "surface record carries direction mask for GPU culling");
    Check(snapshot.surfaceRecords[0].minX == brick.coord.x * SPARSE_BRICK_SIZE &&
          snapshot.surfaceRecords[0].minY == brick.coord.y * SPARSE_BRICK_SIZE &&
          snapshot.surfaceRecords[0].minZ == brick.coord.z * SPARSE_BRICK_SIZE,
        "surface record stores tight world-space min bound");
    Check(snapshot.surfaceRecords[0].maxX <= (brick.coord.x + 1) * SPARSE_BRICK_SIZE &&
          snapshot.surfaceRecords[0].maxY <= (brick.coord.y + 1) * SPARSE_BRICK_SIZE &&
          snapshot.surfaceRecords[0].maxZ <= (brick.coord.z + 1) * SPARSE_BRICK_SIZE,
        "surface record max bound stays inside source brick");
    Check(snapshot.surfaceRecords[0].generation == snapshot.serial, "surface record generation matches snapshot serial");
    Check(snapshot.rangeTableCapacity >= 2, "surface snapshot range table has slack");
    Check(snapshot.ranges.size() == snapshot.rangeTableCapacity,
        "surface snapshot range table size matches capacity");
    SparseSurfaceBrickRange foundRange;
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(snapshot, brick.coord, &foundRange),
        "surface snapshot hashes range by brick coord");
    Check(foundRange.coord == brick.coord, "surface snapshot range coord");
    Check(foundRange.firstFace == 0, "surface snapshot range starts at zero");
    Check(foundRange.faceCount == 10, "surface snapshot range face count");
    Check(foundRange.flags != 0, "surface snapshot range is marked valid");
    Check(SparseSurfaceRecordDirectionMask(foundRange.flags) ==
            SparseSurfaceRecordDirectionMask(snapshot.surfaceRecords[0].flags),
        "surface range carries the same direction mask as the surface record");
    Check(snapshot.serial == cache.GetStats().serial, "surface snapshot serial matches cache");
    Check(snapshot.dirtyBricks.size() == 1 && snapshot.dirtyBricks[0].coord == brick.coord,
        "surface snapshot carries pending dirty brick");
    Check(cache.GetStats().pendingGpuDirtyBricks == 1,
        "surface cache tracks pending dirty brick");
    const uint32_t staleAckSerial = snapshot.serial;
    brick.voxels[LocalVoxelIndex(LocalVoxelCoord{2, 0, 0})] = stone;
    Check(cache.UpdateBrick(brick), "surface cache accepts newer dirty update before stale ack");
    cache.MarkGpuUploadComplete(staleAckSerial, std::vector<BrickCoord>{brick.coord}, {});
    Check(cache.GetStats().pendingGpuDirtyBricks == 1,
        "surface cache keeps newer dirty brick after stale upload ack");
    SparseSurfaceGpuSnapshot updatedSnapshot;
    Check(cache.BuildGpuSnapshot(updatedSnapshot), "surface cache rebuilds after newer dirty update");
    Check(updatedSnapshot.serial == cache.GetStats().serial,
        "surface snapshot serial advances after newer dirty update");
    snapshot = updatedSnapshot;
    cache.MarkGpuUploadComplete(snapshot.serial, std::vector<BrickCoord>{brick.coord}, {});
    Check(cache.GetStats().pendingGpuDirtyBricks == 0,
        "surface cache clears dirty brick after upload ack");
    Check(!SparseSurfaceCache::TryLookupRangeInSnapshot(snapshot, BrickCoord{99, 99, 99}),
        "surface snapshot lookup rejects missing brick");

    brick.voxels[LocalVoxelIndex(LocalVoxelCoord{3, 0, 0})] = dirt;
    cache.BeginFrame();
    Check(cache.UpdateBrickRegion(brick, SparseSurfaceLocalRegion{3, 0, 0, 3, 0, 0}),
        "surface cache accepts partial dirty-region update");
    Check(cache.GetStats().bricksPartiallyUpdatedLastFrame == 1,
        "surface cache reports one partial surface update");
    Check(cache.GetStats().facesRemovedByPartialUpdatesLastFrame > 0,
        "surface cache removes stale faces inside partial dirty region");
    Check(cache.GetStats().totalFaces == 18,
        "four adjacent voxels expose eighteen faces after partial update");

    GeneratedSparseBrick partialEmptyBrick;
    partialEmptyBrick.coord = BrickCoord{7, 0, -2};
    partialEmptyBrick.voxels.fill(air);
    partialEmptyBrick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;
    Check(cache.UpdateBrick(partialEmptyBrick), "surface cache accepts brick for partial zero-face test");
    partialEmptyBrick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = air;
    cache.BeginFrame();
    Check(cache.UpdateBrickRegion(partialEmptyBrick, SparseSurfaceLocalRegion{0, 0, 0, 0, 0, 0}),
        "surface cache accepts partial update that removes all faces");
    Check(cache.FindFaces(partialEmptyBrick.coord) == nullptr,
        "surface cache does not keep empty face vectors after partial removal");
    Check(cache.IsSurfaceKnown(partialEmptyBrick.coord),
        "surface cache still knows zero-face result after partial removal");
    Check(cache.GetStats().pendingGpuRemovedBricks >= 1,
        "surface cache queues GPU removal after partial update removes all faces");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, {}, std::vector<BrickCoord>{partialEmptyBrick.coord});

    GeneratedSparseBrick emptiedResident = brick;
    emptiedResident.voxels.fill(air);
    SparseTerrainGenerator::ComputeOccupancyAndFlags(emptiedResident);
    cache.BeginFrame();
    Check(cache.UpdateBrick(emptiedResident), "surface cache accepts empty replacement");
    Check(cache.FindFaces(brick.coord) == nullptr, "surface cache removes resident brick when it becomes empty");
    Check(cache.IsSurfaceKnown(brick.coord), "surface cache keeps known zero-face state for emptied brick");
    Check(cache.GetStats().cachedBricks == 0, "surface cache empty replacement frees cached brick");
    Check(cache.GetStats().pendingGpuRemovedBricks == 1,
        "surface cache empty replacement queues GPU removal");
    Check(cache.GetStats().emptyFastPathBricksLastFrame == 1,
        "surface cache removes empty resident through occupancy fast path");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, {}, std::vector<BrickCoord>{brick.coord});
    Check(cache.GetStats().pendingGpuRemovedBricks == 0,
        "surface cache clears empty replacement removal ack");
    Check(cache.UpdateBrick(brick), "surface cache re-adds brick after empty replacement");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, std::vector<BrickCoord>{brick.coord}, {});

    GeneratedSparseBrick farBrick;
    farBrick.coord = BrickCoord{100, 0, 0};
    farBrick.voxels.fill(air);
    farBrick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;
    Check(cache.UpdateBrick(farBrick), "surface cache accepts distant culling brick");
    Check(cache.GetStats().pendingGpuDirtyBricks == 1,
        "surface cache tracks dirty culled brick before culling snapshot");
    GeneratedSparseBrick orderBrick;
    orderBrick.coord = BrickCoord{-5, 3, 1};
    orderBrick.voxels.fill(air);
    orderBrick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = dirt;
    Check(cache.UpdateBrick(orderBrick), "surface cache accepts deterministic ordering brick");
    SparseSurfaceGpuSnapshot orderSnapshotA;
    SparseSurfaceGpuSnapshot orderSnapshotB;
    Check(cache.BuildGpuSnapshot(orderSnapshotA), "surface cache builds deterministic snapshot A");
    Check(cache.BuildGpuSnapshot(orderSnapshotB), "surface cache builds deterministic snapshot B");
    Check(orderSnapshotA.drawBatches.size() == orderSnapshotB.drawBatches.size(),
        "surface deterministic snapshot draw batch counts match");
    Check(orderSnapshotA.surfaceRecords.size() == orderSnapshotB.surfaceRecords.size(),
        "surface deterministic snapshot record counts match");
    Check(orderSnapshotA.brickFaceCounts.size() == orderSnapshotB.brickFaceCounts.size(),
        "surface deterministic snapshot brick-face counts match");
    for (size_t i = 1; i < orderSnapshotA.drawBatches.size(); ++i) {
        Check(
            orderSnapshotA.drawBatches[i - 1].coord.x < orderSnapshotA.drawBatches[i].coord.x ||
            (orderSnapshotA.drawBatches[i - 1].coord.x == orderSnapshotA.drawBatches[i].coord.x &&
             orderSnapshotA.drawBatches[i - 1].coord.y < orderSnapshotA.drawBatches[i].coord.y) ||
            (orderSnapshotA.drawBatches[i - 1].coord.x == orderSnapshotA.drawBatches[i].coord.x &&
             orderSnapshotA.drawBatches[i - 1].coord.y == orderSnapshotA.drawBatches[i].coord.y &&
             orderSnapshotA.drawBatches[i - 1].coord.z <= orderSnapshotA.drawBatches[i].coord.z),
            "surface deterministic snapshot draw batches are lexicographically ordered");
    }
    for (size_t i = 0; i < orderSnapshotA.drawBatches.size(); ++i) {
        Check(orderSnapshotA.drawBatches[i].coord == orderSnapshotB.drawBatches[i].coord,
            "surface deterministic snapshot draw batch order repeats");
        Check(orderSnapshotA.drawBatches[i].firstFace == orderSnapshotB.drawBatches[i].firstFace,
            "surface deterministic snapshot face offsets repeat");
        Check(orderSnapshotA.surfaceRecords[i].coord == orderSnapshotB.surfaceRecords[i].coord,
            "surface deterministic snapshot record order repeats");
        Check(orderSnapshotA.surfaceRecords[i].firstFace == orderSnapshotB.surfaceRecords[i].firstFace,
            "surface deterministic snapshot record face offsets repeat");
    }
    Check(cache.RemoveBrick(orderBrick.coord), "surface cache removes deterministic ordering brick");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, {}, std::vector<BrickCoord>{orderBrick.coord});
    SparseSurfaceVisibilityConfig visibility;
    visibility.enabled = true;
    visibility.cameraX = static_cast<float>(brick.coord.x * SPARSE_BRICK_SIZE);
    visibility.cameraY = static_cast<float>(brick.coord.y * SPARSE_BRICK_SIZE);
    visibility.cameraZ = static_cast<float>(brick.coord.z * SPARSE_BRICK_SIZE);
    visibility.forwardX = 0.0f;
    visibility.forwardY = 0.0f;
    visibility.forwardZ = 1.0f;
    visibility.rightX = 1.0f;
    visibility.rightY = 0.0f;
    visibility.rightZ = 0.0f;
    visibility.upX = 0.0f;
    visibility.upY = 1.0f;
    visibility.upZ = 0.0f;
    visibility.maxDistance = 256.0f;
    visibility.padding = 0.0f;
    SparseSurfaceGpuSnapshot culledSnapshot;
    Check(cache.BuildGpuSnapshot(culledSnapshot, &visibility), "surface cache builds culled snapshot");
    Check(culledSnapshot.candidateBricks == 2, "culled snapshot tracks candidates");
    Check(culledSnapshot.visibleBricks == 1, "culled snapshot keeps near visible brick");
    Check(culledSnapshot.culledBricks == 1, "culled snapshot culls distant brick");
    Check(culledSnapshot.drawCommandCount == 1, "culled snapshot emits draw command only for visible brick");
    Check(culledSnapshot.surfaceRecords.size() == 1, "culled snapshot emits record only for visible brick");
    Check(culledSnapshot.drawBatches.size() == 1 &&
        culledSnapshot.drawBatches[0].coord == brick.coord,
        "culled snapshot draw batch belongs to visible brick");
    Check(culledSnapshot.surfaceRecords[0].coord == brick.coord,
        "culled snapshot surface record belongs to visible brick");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(culledSnapshot, brick.coord),
        "culled snapshot contains visible brick range");
    Check(!SparseSurfaceCache::TryLookupRangeInSnapshot(culledSnapshot, farBrick.coord),
        "culled snapshot omits distant brick range");

    SparseSurfaceVisibilityConfig coveredVisibility = visibility;
    coveredVisibility.requireHorizontalNeighborCoverage = true;
    SparseSurfaceGpuSnapshot isolatedCoverageSnapshot;
    Check(cache.BuildGpuSnapshot(isolatedCoverageSnapshot, &coveredVisibility),
        "surface cache builds neighbor-coverage visibility snapshot");
    Check(isolatedCoverageSnapshot.visibleBricks == 0,
        "surface neighbor-coverage visibility rejects isolated surface islands");
    auto addKnownEmptySurfaceBrick = [&](BrickCoord coord) {
        GeneratedSparseBrick knownEmpty;
        knownEmpty.coord = coord;
        knownEmpty.voxels.fill(air);
        SparseTerrainGenerator::ComputeOccupancyAndFlags(knownEmpty);
        Check(cache.UpdateBrick(knownEmpty), "surface cache accepts known empty neighbor coverage brick");
    };
    addKnownEmptySurfaceBrick(BrickCoord{brick.coord.x - 1, brick.coord.y, brick.coord.z});
    addKnownEmptySurfaceBrick(BrickCoord{brick.coord.x + 1, brick.coord.y, brick.coord.z});
    addKnownEmptySurfaceBrick(BrickCoord{brick.coord.x, brick.coord.y, brick.coord.z - 1});
    addKnownEmptySurfaceBrick(BrickCoord{brick.coord.x, brick.coord.y, brick.coord.z + 1});
    SparseSurfaceGpuSnapshot neighborCoverageSnapshot;
    Check(cache.BuildGpuSnapshot(neighborCoverageSnapshot, &coveredVisibility),
        "surface cache builds neighbor-covered visibility snapshot");
    Check(neighborCoverageSnapshot.visibleBricks == 1,
        "surface neighbor-coverage visibility keeps brick once horizontal neighbors are known");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(neighborCoverageSnapshot, brick.coord),
        "surface neighbor-coverage snapshot publishes covered brick range");
    cache.MarkGpuUploadComplete(culledSnapshot.serial, std::vector<BrickCoord>{brick.coord}, {});
    Check(cache.GetStats().pendingGpuDirtyBricks == 1,
        "surface cache does not clear culled dirty brick without payload upload");
    cache.MarkGpuUploadComplete(culledSnapshot.serial, std::vector<BrickCoord>{farBrick.coord}, {});
    Check(cache.GetStats().pendingGpuDirtyBricks == 0,
        "surface cache clears culled dirty brick once payload upload is acknowledged");

    cache.BeginFrame();
    Check(cache.RemoveBrick(brick.coord), "surface cache removes resident brick");
    Check(cache.GetStats().cachedBricks == 1, "surface cache removes one of two brick ranges");
    Check(cache.GetStats().totalFaces == 6, "surface cache keeps distant brick faces");
    Check(cache.GetStats().bricksRemovedLastFrame == 1, "surface cache remove counter increments");
    Check(cache.GetStats().pendingGpuRemovedBricks == 1,
        "surface cache tracks pending removed brick");
    const uint32_t staleRemoveAckSerial = cache.GetStats().serial;
    Check(cache.UpdateBrick(brick), "surface cache accepts re-added brick before stale remove ack");
    cache.MarkGpuUploadComplete(staleRemoveAckSerial, {}, std::vector<BrickCoord>{brick.coord});
    Check(cache.GetStats().pendingGpuRemovedBricks == 0,
        "surface cache drops remove record when brick is re-added");
    Check(cache.GetStats().pendingGpuDirtyBricks == 1,
        "surface cache tracks re-added brick as dirty");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, std::vector<BrickCoord>{brick.coord}, {});
    Check(cache.GetStats().pendingGpuDirtyBricks == 0,
        "surface cache clears re-added brick dirty ack");
    Check(cache.RemoveBrick(brick.coord), "surface cache removes re-added brick");
    cache.MarkGpuUploadComplete(cache.GetStats().serial, {}, std::vector<BrickCoord>{brick.coord});
    Check(cache.GetStats().pendingGpuRemovedBricks == 0,
        "surface cache clears removed brick after upload ack");
    Check(cache.RemoveBrick(farBrick.coord), "surface cache removes distant culling brick");
    Check(cache.GetStats().cachedBricks == 0, "surface cache removes all brick ranges");
    Check(cache.GetStats().totalFaces == 0, "surface cache removes all face data");
    Check(!cache.RemoveBrick(brick.coord), "surface cache remove missing brick returns false");

    SparseSurfaceCache boundaryCache;
    GeneratedSparseBrick boundaryBrick;
    boundaryBrick.coord = BrickCoord{134217727, 0, 0};
    boundaryBrick.voxels.fill(stone);
    Check(boundaryCache.UpdateBrick(boundaryBrick),
        "surface cache accepts max-world boundary brick");
    SparseSurfaceGpuSnapshot boundarySnapshot;
    Check(boundaryCache.BuildGpuSnapshot(boundarySnapshot),
        "surface cache builds max-world boundary snapshot");
    Check(boundarySnapshot.surfaceRecords.size() == 1,
        "surface cache emits max-world boundary record");
    Check(boundarySnapshot.surfaceRecords[0].maxX == std::numeric_limits<int32_t>::max(),
        "surface cache clamps max-world boundary face bounds");

    SparseSurfaceCache stableVisibilityCache;
    auto makeSingleVoxelBrick = [stone](BrickCoord coord) {
        GeneratedSparseBrick generated;
        generated.coord = coord;
        generated.voxels.fill(0u);
        generated.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;
        SparseTerrainGenerator::ComputeOccupancyAndFlags(generated);
        return generated;
    };
    const GeneratedSparseBrick nearForwardBrick = makeSingleVoxelBrick(BrickCoord{0, 0, 2});
    const GeneratedSparseBrick nearBehindBrick = makeSingleVoxelBrick(BrickCoord{0, 0, -2});
    const GeneratedSparseBrick farVisibilityBrick = makeSingleVoxelBrick(BrickCoord{90, 0, 0});
    Check(stableVisibilityCache.UpdateBrick(nearForwardBrick), "stable visibility cache accepts forward brick");
    Check(stableVisibilityCache.UpdateBrick(nearBehindBrick), "stable visibility cache accepts behind brick");
    Check(stableVisibilityCache.UpdateBrick(farVisibilityBrick), "stable visibility cache accepts far brick");
    SparseSurfaceVisibilityConfig stableVisibility;
    stableVisibility.enabled = true;
    stableVisibility.cameraX = 0.0f;
    stableVisibility.cameraY = 0.0f;
    stableVisibility.cameraZ = 0.0f;
    stableVisibility.forwardX = 0.0f;
    stableVisibility.forwardY = 0.0f;
    stableVisibility.forwardZ = 1.0f;
    stableVisibility.rightX = 1.0f;
    stableVisibility.rightY = 0.0f;
    stableVisibility.rightZ = 0.0f;
    stableVisibility.upX = 0.0f;
    stableVisibility.upY = 1.0f;
    stableVisibility.upZ = 0.0f;
    stableVisibility.maxDistance = 128.0f;
    stableVisibility.padding = 0.0f;
    SparseSurfaceGpuSnapshot frustumVisibilitySnapshot;
    Check(stableVisibilityCache.BuildGpuSnapshot(frustumVisibilitySnapshot, &stableVisibility),
        "surface cache builds frustum visibility snapshot");
    Check(frustumVisibilitySnapshot.visibleBricks == 1,
        "frustum visibility culls near brick behind the camera");
    stableVisibility.useFrustum = false;
    SparseSurfaceGpuSnapshot distanceVisibilitySnapshot;
    Check(stableVisibilityCache.BuildGpuSnapshot(distanceVisibilitySnapshot, &stableVisibility),
        "surface cache builds stable distance-only visibility snapshot");
    Check(distanceVisibilitySnapshot.visibleBricks == 2,
        "stable distance-only visibility keeps near owned brick behind the camera");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(distanceVisibilitySnapshot, nearForwardBrick.coord),
        "stable distance-only snapshot contains forward near brick");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(distanceVisibilitySnapshot, nearBehindBrick.coord),
        "stable distance-only snapshot contains behind near brick");
    Check(!SparseSurfaceCache::TryLookupRangeInSnapshot(distanceVisibilitySnapshot, farVisibilityBrick.coord),
        "stable distance-only snapshot still culls distant resident brick");
    stableVisibility.useMotionLookahead = true;
    stableVisibility.lookaheadCameraX =
        static_cast<float>(farVisibilityBrick.coord.x * SPARSE_BRICK_SIZE);
    stableVisibility.lookaheadCameraY =
        static_cast<float>(farVisibilityBrick.coord.y * SPARSE_BRICK_SIZE);
    stableVisibility.lookaheadCameraZ =
        static_cast<float>(farVisibilityBrick.coord.z * SPARSE_BRICK_SIZE);
    SparseSurfaceGpuSnapshot motionVisibilitySnapshot;
    Check(stableVisibilityCache.BuildGpuSnapshot(motionVisibilitySnapshot, &stableVisibility),
        "surface cache builds motion-lookahead visibility snapshot");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(motionVisibilitySnapshot, farVisibilityBrick.coord),
        "surface motion-lookahead snapshot keeps resident brick near predicted camera");
    Check(motionVisibilitySnapshot.lookaheadVisibleBricks == 1,
        "surface motion-lookahead snapshot reports predicted-only visible brick count");
    stableVisibility.useFrustum = true;
    SparseSurfaceGpuSnapshot motionFrustumSnapshot;
    Check(stableVisibilityCache.BuildGpuSnapshot(motionFrustumSnapshot, &stableVisibility),
        "surface cache builds motion-lookahead frustum snapshot");
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(motionFrustumSnapshot, farVisibilityBrick.coord),
        "surface motion-lookahead keeps predicted brick even when outside current frustum");
    Check(motionFrustumSnapshot.lookaheadVisibleBricks == 1,
        "surface motion-lookahead frustum snapshot reports predicted-only visible brick count");

    SparseSurfaceVisibilityConfig malformedVisibility = stableVisibility;
    malformedVisibility.cameraX = std::numeric_limits<float>::quiet_NaN();
    malformedVisibility.useFrustum = true;
    malformedVisibility.useMotionLookahead = true;
    malformedVisibility.lookaheadCameraX = std::numeric_limits<float>::infinity();
    SparseSurfaceGpuSnapshot malformedVisibilitySnapshot;
    Check(stableVisibilityCache.BuildGpuSnapshot(malformedVisibilitySnapshot, &malformedVisibility),
        "surface cache builds malformed visibility snapshot");
    Check(malformedVisibilitySnapshot.visibleBricks == 3 &&
          SparseSurfaceCache::TryLookupRangeInSnapshot(malformedVisibilitySnapshot, farVisibilityBrick.coord),
        "surface visibility culling fails open for malformed camera inputs");
}

void TestSparseSurfaceClusterRecords() {
    const uint32_t allDirections =
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::NegX)) |
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::PosX)) |
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::NegY)) |
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::PosY)) |
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::NegZ)) |
        SparseSurfaceDirectionBit(static_cast<uint32_t>(SparseFaceDirection::PosZ));
    const uint32_t recordFlags = SparseSurfacePackRecordFlags(kSparseSurfaceRangeValid, allDirections);
    std::vector<SparseSurfaceRecord> records = {
        {BrickCoord{48, 0, 0}, 480u, 6u, recordFlags, 7u, 768, 0, 0, 784, 16, 16},
        {BrickCoord{0, 0, 0}, 0u, 10u, recordFlags, 7u, 0, 0, 0, 16, 16, 16},
        {BrickCoord{1, 0, 0}, 10u, 12u, recordFlags, 7u, 16, 0, 0, 32, 16, 16},
        {BrickCoord{0, 0, 1}, 22u, 8u, recordFlags, 7u, 0, 0, 16, 16, 16, 32},
        {BrickCoord{-1, 0, 0}, 30u, 14u, recordFlags, 7u, -16, 0, 0, 0, 16, 16},
        {BrickCoord{49, 0, 0}, 486u, 6u, recordFlags, 7u, 784, 0, 0, 800, 16, 16},
    };
    Check(SparseSurfaceRecordDirectionMask(records[0].flags) == allDirections,
        "surface record flags preserve direction mask for culling");

    SortSparseSurfaceRecordsForClusters(records);
    for (size_t i = 1; i < records.size(); ++i) {
        Check(
            SparseSurfaceMortonKey(records[i - 1].coord) <= SparseSurfaceMortonKey(records[i].coord),
            "surface records sort by signed morton key for spatial clusters");
    }

    Check(SparseSurfaceMortonKey({std::numeric_limits<int32_t>::max(), 0, 0}) ==
          SparseSurfaceMortonKey({-1, 0, 0}),
        "surface morton key handles positive int32 boundary without signed overflow");
    Check(SparseSurfaceMortonKey({std::numeric_limits<int32_t>::min(), 0, 0}) ==
          SparseSurfaceMortonKey({0, 0, 0}),
        "surface morton key handles negative int32 boundary without signed overflow");

    std::vector<SparseSurfaceRecord> extremeSortRecords = {
        {
            BrickCoord{std::numeric_limits<int32_t>::max(), 0, 0},
            0u,
            1u,
            recordFlags,
            17u,
            0,
            0,
            0,
            1,
            1,
            1,
        },
        {
            BrickCoord{std::numeric_limits<int32_t>::min(), 0, 0},
            1u,
            1u,
            recordFlags,
            17u,
            0,
            0,
            0,
            1,
            1,
            1,
        },
        {
            BrickCoord{0, 0, 0},
            2u,
            1u,
            recordFlags,
            17u,
            0,
            0,
            0,
            1,
            1,
            1,
        },
    };
    SortSparseSurfaceRecordsForClusters(extremeSortRecords);
    bool extremeSortOrdered = true;
    for (size_t i = 1; i < extremeSortRecords.size(); ++i) {
        const BrickCoord& prev = extremeSortRecords[i - 1].coord;
        const BrickCoord& next = extremeSortRecords[i].coord;
        const uint64_t prevKey = SparseSurfaceMortonKey(prev);
        const uint64_t nextKey = SparseSurfaceMortonKey(next);
        const bool prevCoordLess =
            prev.x < next.x ||
            (prev.x == next.x && (prev.y < next.y || (prev.y == next.y && prev.z < next.z)));
        extremeSortOrdered = extremeSortOrdered && (prevKey < nextKey || (prevKey == nextKey && prevCoordLess));
    }
    Check(extremeSortOrdered,
        "surface record sort remains deterministic for extreme int32 brick coords");

    const auto clusters = BuildSparseSurfaceClusters(records, 2u);
    Check(clusters.size() == 3, "surface cluster builder groups records by requested size");
    Check(clusters[0].firstRecord == 0 && clusters[0].recordCount == 2,
        "surface first cluster record range");
    Check(clusters[1].firstRecord == 2 && clusters[1].recordCount == 2,
        "surface second cluster record range");
    Check(clusters[2].firstRecord == 4 && clusters[2].recordCount == 2,
        "surface third cluster record range");

    for (const SparseSurfaceClusterRecord& cluster : clusters) {
        uint32_t expectedFaceCount = 0;
        uint32_t expectedDirectionMask = 0;
        for (uint32_t i = 0; i < cluster.recordCount; ++i) {
            const SparseSurfaceRecord& record = records[cluster.firstRecord + i];
            expectedFaceCount += record.faceCount;
            expectedDirectionMask |= SparseSurfaceRecordDirectionMask(record.flags);
            Check(record.minX >= cluster.minX && record.maxX <= cluster.maxX,
                "surface cluster x bounds contain member coord");
            Check(record.minY >= cluster.minY && record.maxY <= cluster.maxY,
                "surface cluster y bounds contain member coord");
            Check(record.minZ >= cluster.minZ && record.maxZ <= cluster.maxZ,
                "surface cluster z bounds contain member coord");
        }
        Check(cluster.faceCount == expectedFaceCount,
            "surface cluster caches summed face count for cluster indirect draw");
        Check(SparseSurfaceRecordDirectionMask(cluster.flags) == expectedDirectionMask,
            "surface cluster caches unioned direction mask for coarse GPU backface cull");
    }

    const auto singletonClusters = BuildSparseSurfaceClusters(records, 0u);
    Check(singletonClusters.size() == records.size(),
        "surface cluster builder clamps zero records-per-cluster to one");

    std::vector<SparseSurfaceRecord> extentRecords = {
        {BrickCoord{0, 0, 0}, 0u, 6u, recordFlags, 9u, 0, 0, 0, 16, 16, 16},
        {BrickCoord{1, 0, 0}, 6u, 6u, recordFlags, 9u, 16, 0, 0, 32, 16, 16},
        {BrickCoord{2, 0, 0}, 12u, 6u, recordFlags, 9u, 32, 0, 0, 48, 16, 16},
        {BrickCoord{40, 0, 0}, 18u, 6u, recordFlags, 9u, 640, 0, 0, 656, 16, 16},
        {BrickCoord{41, 0, 0}, 24u, 6u, recordFlags, 9u, 656, 0, 0, 672, 16, 16},
    };
    SortSparseSurfaceRecordsForClusters(extentRecords);
    const auto extentClusters = BuildSparseSurfaceClusters(extentRecords, 64u, 64u);
    Check(extentClusters.size() >= 2,
        "surface cluster builder splits spatially loose clusters even under large count budget");
    for (const SparseSurfaceClusterRecord& cluster : extentClusters) {
        const uint32_t extentX = static_cast<uint32_t>(std::max(0, cluster.maxX - cluster.minX));
        Check(cluster.recordCount == 1u || extentX <= 64u,
            "surface cluster extent limit keeps multi-record cluster bounds tight");
    }

    const auto countOnlyClusters = BuildSparseSurfaceClusters(extentRecords, 64u, 0u);
    Check(countOnlyClusters.size() == 1,
        "surface cluster extent limit is opt-in and preserves count-only behavior when disabled");

    std::vector<SparseSurfaceRecord> extremeBoundsRecords = {
        {
            BrickCoord{-134217728, 0, 0},
            0u,
            1u,
            recordFlags,
            11u,
            std::numeric_limits<int32_t>::min(),
            0,
            0,
            std::numeric_limits<int32_t>::min() + 16,
            16,
            16,
        },
        {
            BrickCoord{134217727, 0, 0},
            1u,
            1u,
            recordFlags,
            11u,
            std::numeric_limits<int32_t>::max() - 16,
            0,
            0,
            std::numeric_limits<int32_t>::max(),
            16,
            16,
        },
    };
    const auto extremeBoundsClusters = BuildSparseSurfaceClusters(extremeBoundsRecords, 64u, 64u);
    Check(extremeBoundsClusters.size() == 2,
        "surface cluster extent test uses 64-bit math for extreme int32 bounds");
    for (const SparseSurfaceClusterRecord& cluster : extremeBoundsClusters) {
        Check(cluster.recordCount == 1u,
            "surface cluster extreme bounds do not merge through wrapped extent");
    }

    std::vector<SparseSurfaceRecord> faceSaturationRecords = {
        {
            BrickCoord{0, 0, 0},
            0u,
            std::numeric_limits<uint32_t>::max() - 3u,
            recordFlags,
            13u,
            0,
            0,
            0,
            16,
            16,
            16,
        },
        {
            BrickCoord{1, 0, 0},
            1u,
            10u,
            recordFlags,
            13u,
            16,
            0,
            0,
            32,
            16,
            16,
        },
    };
    const auto faceSaturationClusters = BuildSparseSurfaceClusters(faceSaturationRecords, 2u, 0u);
    Check(faceSaturationClusters.size() == 1 &&
          faceSaturationClusters[0].faceCount == std::numeric_limits<uint32_t>::max(),
        "surface cluster face count saturates instead of wrapping");
}

void TestSparseSurfaceRangeAllocator() {
    SparseSurfaceRangeAllocator allocator;
    allocator.Initialize(128, 2);
    allocator.BeginFrame(10);

    SparseSurfaceFaceAllocation a;
    Check(allocator.AllocateOrResize(BrickCoord{0, 0, 0}, 20, &a),
        "surface range allocator allocates first brick");
    Check(a.firstFace == 0 && a.capacity == 20 && a.faceCount == 20,
        "surface range allocator first allocation layout");

    SparseSurfaceFaceAllocation b;
    Check(allocator.AllocateOrResize(BrickCoord{1, 0, 0}, 12, &b),
        "surface range allocator allocates second brick");
    Check(b.firstFace == 20 && b.capacity == 12,
        "surface range allocator second allocation follows first");

    SparseSurfaceFaceAllocation shrunk;
    Check(allocator.AllocateOrResize(BrickCoord{0, 0, 0}, 8, &shrunk),
        "surface range allocator shrinks in place");
    Check(shrunk.firstFace == a.firstFace && shrunk.capacity == a.capacity && shrunk.faceCount == 8,
        "surface range allocator preserves capacity on shrink");
    Check(shrunk.generation == a.generation + 1u,
        "surface range allocator advances generation on in-place resize");

    SparseSurfaceFaceAllocation grown;
    Check(allocator.AllocateOrResize(BrickCoord{0, 0, 0}, 40, &grown),
        "surface range allocator grows into new range");
    Check(grown.firstFace >= b.firstFace + b.capacity && grown.capacity == 40,
        "surface range allocator moved grown allocation");
    Check(grown.generation == shrunk.generation + 1u,
        "surface range allocator advances generation on moved resize");
    Check(allocator.GetStats().pendingRetiredRangeCount == 1 &&
        allocator.GetStats().pendingRetiredCapacity == a.capacity,
        "surface range allocator defers old grown allocation retirement");

    allocator.Free(BrickCoord{1, 0, 0});
    allocator.Free(BrickCoord{0, 0, 0});
    Check(allocator.GetStats().allocationCount == 0, "surface range allocator frees all allocations");
    Check(allocator.GetStats().pendingRetiredRangeCount == 3,
        "surface range allocator keeps freed ranges retired before fence horizon");
    Check(allocator.GetStats().largestFreeRange < 128,
        "surface range allocator does not immediately reuse retired ranges");
    allocator.BeginFrame(11);
    Check(allocator.GetStats().pendingRetiredRangeCount == 3,
        "surface range allocator keeps retired ranges before target frame");
    allocator.BeginFrame(12);
    Check(allocator.GetStats().pendingRetiredRangeCount == 0,
        "surface range allocator releases retired ranges at target frame");
    Check(allocator.GetStats().freeRangeCount == 1, "surface range allocator coalesces adjacent free ranges");
    Check(allocator.GetStats().largestFreeRange == 128, "surface range allocator restores full free range after retire");

    Check(!allocator.AllocateOrResize(BrickCoord{9, 0, 0}, 256, nullptr),
        "surface range allocator rejects over-capacity allocation");
    Check(allocator.GetStats().allocationFailures == 1,
        "surface range allocator tracks allocation failures");

    Check(allocator.AllocateOrResize(BrickCoord{2, 0, 0}, 16, nullptr),
        "surface range allocator allocates live release A");
    Check(allocator.AllocateOrResize(BrickCoord{3, 0, 0}, 16, nullptr),
        "surface range allocator allocates live release B");
    std::unordered_set<BrickCoord, BrickCoordHash> live;
    live.insert(BrickCoord{3, 0, 0});
    allocator.ReleaseNotIn(live);
    Check(!allocator.TryGet(BrickCoord{2, 0, 0}),
        "surface range allocator releases stale allocation");
    Check(allocator.TryGet(BrickCoord{3, 0, 0}),
        "surface range allocator keeps live allocation");
    Check(allocator.GetStats().pendingRetiredRangeCount == 1,
        "surface range allocator retires stale allocation instead of freeing immediately");

    SparseSurfaceRangeAllocator pressureAllocator;
    pressureAllocator.Initialize(32, 3);
    pressureAllocator.BeginFrame(100);
    Check(pressureAllocator.AllocateOrResize(BrickCoord{0, 0, 0}, 32, nullptr),
        "surface range allocator fills pressure heap");
    pressureAllocator.Free(BrickCoord{0, 0, 0});
    Check(!pressureAllocator.AllocateOrResize(BrickCoord{1, 0, 0}, 32, nullptr),
        "surface range allocator rejects reuse before retired range matures");
    pressureAllocator.BeginFrame(102);
    Check(!pressureAllocator.AllocateOrResize(BrickCoord{1, 0, 0}, 32, nullptr),
        "surface range allocator still rejects before retirement frame");
    pressureAllocator.BeginFrame(103);
    Check(pressureAllocator.AllocateOrResize(BrickCoord{1, 0, 0}, 32, nullptr),
        "surface range allocator reuses range after retirement frame");

    SparseSurfaceRangeAllocator fenceAllocator;
    fenceAllocator.Initialize(32, 99);
    fenceAllocator.BeginFrame(0, 10);
    Check(fenceAllocator.AllocateOrResize(BrickCoord{0, 1, 0}, 32, nullptr),
        "surface range allocator fills fence heap");
    fenceAllocator.Free(BrickCoord{0, 1, 0});
    fenceAllocator.BeginFrame(9, 11);
    Check(!fenceAllocator.AllocateOrResize(BrickCoord{1, 1, 0}, 32, nullptr),
        "surface range allocator does not reuse before completed fence");
    fenceAllocator.BeginFrame(10, 12);
    Check(fenceAllocator.AllocateOrResize(BrickCoord{1, 1, 0}, 32, nullptr),
        "surface range allocator reuses after completed fence reaches retire token");

    SparseSurfaceRangeAllocator monotonicAllocator;
    monotonicAllocator.Initialize(32, 3);
    monotonicAllocator.BeginFrame(10);
    Check(monotonicAllocator.AllocateOrResize(BrickCoord{2, 1, 0}, 32, nullptr),
        "surface range allocator fills monotonic-token heap");
    monotonicAllocator.BeginFrame(9, 9);
    monotonicAllocator.Free(BrickCoord{2, 1, 0});
    monotonicAllocator.BeginFrame(12, 12);
    Check(!monotonicAllocator.AllocateOrResize(BrickCoord{3, 1, 0}, 32, nullptr),
        "surface range allocator ignores regressed retirement tokens before reusing freed range");
    monotonicAllocator.BeginFrame(13, 13);
    Check(monotonicAllocator.AllocateOrResize(BrickCoord{3, 1, 0}, 32, nullptr),
        "surface range allocator releases monotonic-token range at original safe horizon");

    SparseSurfaceRangeAllocator saturatedTokenAllocator;
    saturatedTokenAllocator.Initialize(32, std::numeric_limits<uint32_t>::max());
    saturatedTokenAllocator.BeginFrame(std::numeric_limits<uint64_t>::max() - 2ull);
    Check(saturatedTokenAllocator.AllocateOrResize(BrickCoord{0, 2, 0}, 32, nullptr),
        "surface range allocator fills saturated-token heap");
    saturatedTokenAllocator.Free(BrickCoord{0, 2, 0});
    saturatedTokenAllocator.BeginFrame(
        std::numeric_limits<uint64_t>::max() - 1ull,
        std::numeric_limits<uint64_t>::max() - 1ull);
    Check(!saturatedTokenAllocator.AllocateOrResize(BrickCoord{1, 2, 0}, 32, nullptr),
        "surface range allocator does not release saturated retire token early");
    saturatedTokenAllocator.BeginFrame(
        std::numeric_limits<uint64_t>::max(),
        std::numeric_limits<uint64_t>::max());
    Check(saturatedTokenAllocator.AllocateOrResize(BrickCoord{1, 2, 0}, 32, nullptr),
        "surface range allocator releases range when saturated retire token completes");

    SparseSurfaceRangeAllocator signedBoundaryAllocator;
    signedBoundaryAllocator.Initialize(std::numeric_limits<uint32_t>::max(), 0);
    signedBoundaryAllocator.BeginFrame(0);
    Check(signedBoundaryAllocator.AllocateOrResize(
              BrickCoord{0, 3, 0},
              std::numeric_limits<uint32_t>::max() - 8u,
              nullptr),
        "surface range allocator accepts large boundary allocation");
    SparseSurfaceFaceAllocation boundaryAllocation;
    Check(signedBoundaryAllocator.AllocateOrResize(
              BrickCoord{1, 3, 0},
              8u,
              &boundaryAllocation),
        "surface range allocator accepts final boundary allocation");
    Check(boundaryAllocation.firstFace == std::numeric_limits<uint32_t>::max() - 8u,
        "surface range allocator final boundary allocation does not wrap first face");
    signedBoundaryAllocator.Free(BrickCoord{0, 3, 0});
    signedBoundaryAllocator.Free(BrickCoord{1, 3, 0});
    signedBoundaryAllocator.BeginFrame(0);
    Check(signedBoundaryAllocator.GetStats().freeRangeCount == 1u &&
          signedBoundaryAllocator.GetStats().largestFreeRange == std::numeric_limits<uint32_t>::max(),
        "surface range allocator coalesces uint32 boundary ranges without wrapping");
}

void TestSparsePagePublishQueue() {
    SparsePagePublishQueue queue;
    Check(queue.Empty(), "page publish queue starts empty");
    Check(
        queue.Enqueue(
            UINT32_MAX,
            BrickCoord{0, 0, 0},
            1u,
            1u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredInvalid,
        "page publish queue rejects invalid table slot");
    Check(
        queue.Enqueue(
            4u,
            BrickCoord{0, 0, 0},
            INVALID_BRICK_PAGE,
            1u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredInvalid,
        "page publish queue rejects invalid page index");
    Check(
        queue.Enqueue(
            4u,
            BrickCoord{0, 0, 0},
            INVALID_BRICK_PAGE - 1u,
            1u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredInvalid,
        "page publish queue rejects tombstone page index");
    Check(
        queue.Enqueue(
            4u,
            BrickCoord{0, 0, 0},
            1u,
            0u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredInvalid,
        "page publish queue rejects zero generation");
    SparsePendingPageTablePublish invalidRetry;
    invalidRetry.entryIndex = 4u;
    invalidRetry.coord = BrickCoord{0, 0, 0};
    invalidRetry.pageIndex = INVALID_BRICK_PAGE - 1u;
    invalidRetry.generation = 1u;
    queue.RequeueFront(invalidRetry);
    Check(queue.Empty(), "page publish queue ignores invalid retry publishes");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{1, 2, 3},
            7u,
            11u,
            4u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts first visible publish");
    Check(queue.Size() == 1, "page publish queue stores one visible publish");
    Check(queue.ContainsEntry(5u) && !queue.ContainsEntry(6u),
        "page publish queue exposes pending entry lookup for invalidation guards");

    std::vector<BrickPageEntry> cpuEntries(8);
    for (BrickPageEntry& entry : cpuEntries) {
        entry.pageIndex = INVALID_BRICK_PAGE;
    }
    SparseDelayedInvalidationInput invalidationInput{};
    invalidationInput.cpuEntries = cpuEntries.data();
    invalidationInput.cpuEntryCount = cpuEntries.size();
    invalidationInput.entryIndex = 5u;
    invalidationInput.coord = BrickCoord{1, 2, 3};
    invalidationInput.pageIndex = 7u;
    invalidationInput.generation = 11u;
    Check(
        DecideSparseDelayedInvalidation(invalidationInput) ==
            SparseDelayedInvalidationDecision::Stage,
        "delayed invalidation stages empty CPU slots");
    cpuEntries[5].coord = BrickCoord{1, 2, 3};
    cpuEntries[5].pageIndex = 7u;
    cpuEntries[5].generation = 11u;
    Check(
        DecideSparseDelayedInvalidation(invalidationInput) ==
            SparseDelayedInvalidationDecision::Stage,
        "delayed invalidation stages slots that still match the old page");
    cpuEntries[5].coord = BrickCoord{9, 8, 7};
    cpuEntries[5].pageIndex = 12u;
    cpuEntries[5].generation = 13u;
    invalidationInput.replacementPublishPending = true;
    Check(
        DecideSparseDelayedInvalidation(invalidationInput) ==
            SparseDelayedInvalidationDecision::SkipAlreadyReplaced,
        "delayed invalidation skips reused slots while a replacement publish is pending");
    invalidationInput.replacementPublishPending = false;
    Check(
        DecideSparseDelayedInvalidation(invalidationInput) ==
            SparseDelayedInvalidationDecision::SkipAlreadyReplaced,
        "delayed invalidation skips reused slots after the replacement is already published");

    Check(queue.ReadyCount(3u, 0u) == 0,
        "page publish queue withholds publishes before ready frame");
    Check(queue.ReadyCount(4u, 0u) == 1,
        "page publish queue marks publish ready at ready frame");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{9, 8, 7},
            12u,
            13u,
            5u,
            99u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Replaced,
        "page publish queue replaces stale same-slot publish with newer generation");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{9, 8, 7},
            12u,
            12u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredStale,
        "page publish queue rejects older same-page generation replacement");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{8, 8, 7},
            14u,
            12u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredStale,
        "page publish queue rejects older different-page generation replacement");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{8, 8, 7},
            14u,
            13u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::IgnoredStale,
        "page publish queue rejects ambiguous same-generation different-page replacement");
    Check(queue.Size() == 1, "page publish replacement does not duplicate table slots");
    Check(queue.ReadyCount(5u, 98u) == 0,
        "page publish queue withholds publishes before ready fence");
    const SparsePagePublishQueueStats waitingFenceStats = queue.GetStats(5u, 98u);
    Check(waitingFenceStats.total == 1 && waitingFenceStats.waitingFence == 1,
        "page publish queue reports fence-gated publishes");
    Check(queue.ReadyCount(5u, 99u) == 1,
        "page publish queue marks publish ready after completed fence");
    const SparsePagePublishQueueStats readyStats = queue.GetStats(7u, 99u);
    Check(readyStats.ready == 1 && readyStats.maxReadyFrameLag == 2,
        "page publish queue reports ready publish age");

    SparsePendingPageTablePublish publish;
    Check(queue.PopReady(5u, 99u, &publish), "page publish queue pops replaced publish");
    Check(
        publish.coord == BrickCoord{9, 8, 7} &&
        publish.pageIndex == 12u &&
        publish.generation == 13u &&
        publish.readyFrame == 5u &&
        publish.readyFenceValue == 99u,
        "page publish queue returns latest coord/page/generation/readiness for reused slot");
    Check(queue.Empty(), "page publish queue is empty after pop");

    Check(
        queue.Enqueue(
            5u,
            BrickCoord{9, 8, 7},
            12u,
            13u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts older slot reuse baseline");
    Check(
        queue.Enqueue(
            5u,
            BrickCoord{10, 8, 7},
            15u,
            14u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Replaced,
        "page publish queue replaces reused slot only with a newer generation");
    Check(queue.PopReady(0u, 0u, &publish) &&
          publish.coord == BrickCoord{10, 8, 7} &&
          publish.pageIndex == 15u &&
          publish.generation == 14u,
        "page publish queue returns newer different-page slot publish");
    Check(queue.Empty(), "page publish queue empties after newer different-page replacement pop");

    Check(
        queue.Enqueue(
            6u,
            BrickCoord{4, 4, 4},
            22u,
            7u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts retry guard baseline publish");
    SparsePendingPageTablePublish staleRetry;
    staleRetry.entryIndex = 6u;
    staleRetry.coord = BrickCoord{4, 4, 4};
    staleRetry.pageIndex = 22u;
    staleRetry.generation = 6u;
    queue.RequeueFront(staleRetry);
    Check(queue.PopReady(0u, 0u, &publish) && publish.entryIndex == 6u &&
          publish.pageIndex == 22u && publish.generation == 7u,
        "page publish queue ignores stale same-page retry publish");
    Check(queue.Empty(), "page publish queue empties after stale retry guard pop");

    Check(
        queue.Enqueue(
            7u,
            BrickCoord{7, 7, 7},
            33u,
            9u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts replacement-publish retry guard baseline");
    SparsePendingPageTablePublish staleDifferentPageRetry;
    staleDifferentPageRetry.entryIndex = 7u;
    staleDifferentPageRetry.coord = BrickCoord{6, 6, 6};
    staleDifferentPageRetry.pageIndex = 32u;
    staleDifferentPageRetry.generation = 20u;
    queue.RequeueFront(staleDifferentPageRetry);
    Check(queue.PopReady(0u, 0u, &publish) &&
          publish.entryIndex == 7u &&
          publish.coord == BrickCoord{7, 7, 7} &&
          publish.pageIndex == 33u &&
          publish.generation == 9u,
        "page publish queue does not let stale different-page retry replace pending slot publish");
    Check(queue.Empty(), "page publish queue empties after different-page retry guard pop");

    Check(
        queue.Enqueue(
            2u,
            BrickCoord{0, 0, 0},
            3u,
            4u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts background publish before edit");
    Check(
        queue.Enqueue(
            8u,
            BrickCoord{1, 0, 0},
            9u,
            10u,
            0u,
            0u,
            SparseResidencyClass::Visible) == SparsePagePublishQueueEvent::Queued,
        "page publish queue accepts second background publish");
    Check(
        queue.Enqueue(
            8u,
            BrickCoord{1, 0, 0},
            9u,
            11u,
            0u,
            0u,
            SparseResidencyClass::Edited) == SparsePagePublishQueueEvent::PromotedEdited,
        "page publish queue promotes edited publish to the front");
    Check(queue.PopReady(0u, 0u, &publish), "page publish queue pops edited publish first");
    Check(
        publish.entryIndex == 8u &&
        publish.generation == 11u &&
        publish.residencyClass == SparseResidencyClass::Edited,
        "page publish queue preserves promoted edited generation and priority");

    queue.RequeueFront(publish);
    Check(queue.PopReady(0u, 0u, &publish) && publish.entryIndex == 8u,
        "page publish queue requeues failed publish at the front");
    queue.Clear();
    Check(queue.Empty(), "page publish queue clears pending publishes");
}

void TestSparseBrushEditSemantics() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "sparse brush world initialize");

    const float airX = 900.5f;
    const float airY = 700.5f;
    const float airZ = -900.5f;
    std::vector<SparseEditDelta> previewPaintDeltas;
    const uint32_t previewPainted = world.PreviewBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        77u,
        0,
        0,
        0,
        false,
        &previewPaintDeltas);
    Check(previewPainted > 0, "sparse brush preview paints air voxels");
    Check(previewPaintDeltas.size() == previewPainted,
        "sparse brush preview returns one delta per would-be edit");
    Check(world.GetStats().brushVoxelsEditedLastStroke == 0,
        "sparse brush preview does not mutate brush stats");
    for (const SparseEditDelta& delta : previewPaintDeltas) {
        const LocalVoxelCoord local = UnpackSparseEditLocal(delta.packedLocal);
        const int32_t worldX = delta.coord.x * SPARSE_BRICK_SIZE + local.x;
        const int32_t worldY = delta.coord.y * SPARSE_BRICK_SIZE + local.y;
        const int32_t worldZ = delta.coord.z * SPARSE_BRICK_SIZE + local.z;
        uint32_t overlayVoxel = 0;
        Check(!world.GetEdits().TryGetVoxel(worldX, worldY, worldZ, &overlayVoxel),
            "sparse brush preview does not write persistent overlay voxels");
    }
    std::vector<SparseEditDelta> malformedPreviewDeltas;
    Check(world.PreviewBrushEdit(
            airX,
            airY,
            airZ,
            std::numeric_limits<float>::infinity(),
            VENPOD::Utils::Material::Stone,
            0,
            0,
            1.0f,
            77u,
            0,
            0,
            0,
            false,
            &malformedPreviewDeltas) == 0 &&
          malformedPreviewDeltas.empty(),
        "sparse brush preview rejects non-finite radius without deltas");
    Check(world.ApplyBrushEdit(
            std::numeric_limits<float>::quiet_NaN(),
            airY,
            airZ,
            1.1f,
            VENPOD::Utils::Material::Stone,
            0,
            0,
            1.0f,
            77u,
            0,
            0,
            0,
            false,
            true) == 0 &&
          world.GetStats().brushVoxelsEditedLastStroke == 0,
        "sparse brush commit rejects non-finite world position without edits");
    Check(world.ApplyBrushEdit(
            static_cast<float>(std::numeric_limits<int32_t>::max()),
            airY,
            airZ,
            4.0f,
            VENPOD::Utils::Material::Stone,
            0,
            0,
            1.0f,
            77u,
            0,
            0,
            0,
            false,
            true) == 0 &&
          world.GetStats().brushVoxelsEditedLastStroke == 0,
        "sparse brush commit fails closed near positive world-coordinate overflow boundary");

    const std::filesystem::path saturatedPreviewPath =
        std::filesystem::temp_directory_path() / "venpod_sparse_brush_saturated_preview.vsed";
    {
        std::ofstream saturatedPreview(saturatedPreviewPath, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x44455356u;
        const uint32_t version = 1u;
        const uint32_t brickSize = SPARSE_BRICK_SIZE;
        const uint32_t reserved = 0u;
        const uint64_t overlayCount = 1u;
        const uint64_t totalVoxelCount = 1u;
        const BrickCoord coord{1, 250, 1};
        const uint32_t revision = std::numeric_limits<uint32_t>::max();
        const uint32_t voxelCount = 1u;
        const uint16_t local = 0u;
        const uint32_t previewStone = VENPOD::Utils::PackVoxel(
            VENPOD::Utils::Material::Stone,
            1,
            0,
            VENPOD::Utils::StateFlags::IsStatic);
        WriteTestBinary(saturatedPreview, magic);
        WriteTestBinary(saturatedPreview, version);
        WriteTestBinary(saturatedPreview, brickSize);
        WriteTestBinary(saturatedPreview, reserved);
        WriteTestBinary(saturatedPreview, overlayCount);
        WriteTestBinary(saturatedPreview, totalVoxelCount);
        WriteTestBinary(saturatedPreview, coord.x);
        WriteTestBinary(saturatedPreview, coord.y);
        WriteTestBinary(saturatedPreview, coord.z);
        WriteTestBinary(saturatedPreview, revision);
        WriteTestBinary(saturatedPreview, voxelCount);
        WriteTestBinary(saturatedPreview, local);
        WriteTestBinary(saturatedPreview, previewStone);
    }
    SparseVoxelWorld saturatedPreviewWorld;
    Check(saturatedPreviewWorld.Initialize({8, 32, 12345u}),
        "sparse brush saturated-preview world initializes");
    Check(saturatedPreviewWorld.LoadEditsFromFile(saturatedPreviewPath, false),
        "sparse brush saturated-preview world loads max-revision overlay");
    std::vector<SparseEditDelta> saturatedPreviewDeltas;
    const uint32_t saturatedPreviewErased = saturatedPreviewWorld.PreviewBrushEdit(
        16.5f,
        4000.5f,
        16.5f,
        0.6f,
        VENPOD::Utils::Material::Air,
        1,
        0,
        1.0f,
        82u,
        0,
        0,
        0,
        false,
        &saturatedPreviewDeltas);
    Check(saturatedPreviewErased == 1u && saturatedPreviewDeltas.size() == 1u,
        "sparse brush preview can edit a saturated overlay voxel");
    Check(saturatedPreviewDeltas[0].revision == 1u,
        "sparse brush preview wraps saturated overlay revision to nonzero epoch");
    Check(saturatedPreviewWorld.GetEdits().GetOverlayRevision({1, 250, 1}) ==
              std::numeric_limits<uint32_t>::max(),
        "sparse brush preview does not mutate saturated overlay revision");
    std::filesystem::remove(saturatedPreviewPath);

    std::vector<SparseEditDelta> paintDeltas;
    const uint32_t painted = world.ApplyBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        77u,
        0,
        0,
        0,
        false,
        true,
        &paintDeltas);
    Check(painted == previewPainted,
        "sparse brush committed paint matches preview edit count");
    Check(paintDeltas.size() == previewPaintDeltas.size(),
        "sparse brush committed paint matches preview delta count");
    for (size_t i = 0; i < paintDeltas.size(); ++i) {
        Check(paintDeltas[i].coord == previewPaintDeltas[i].coord &&
              paintDeltas[i].packedLocal == previewPaintDeltas[i].packedLocal &&
              paintDeltas[i].voxel == previewPaintDeltas[i].voxel,
            "sparse brush committed paint delta matches preview delta");
    }
    Check(painted > 0, "sparse brush paints air voxels");
    Check(paintDeltas.size() == painted,
        "sparse brush returns one persistent delta per painted voxel");
    for (const SparseEditDelta& delta : paintDeltas) {
        const LocalVoxelCoord local = UnpackSparseEditLocal(delta.packedLocal);
        const int32_t worldX = delta.coord.x * SPARSE_BRICK_SIZE + local.x;
        const int32_t worldY = delta.coord.y * SPARSE_BRICK_SIZE + local.y;
        const int32_t worldZ = delta.coord.z * SPARSE_BRICK_SIZE + local.z;
        uint32_t overlayVoxel = 0;
        Check(world.GetEdits().TryGetVoxel(worldX, worldY, worldZ, &overlayVoxel),
            "sparse brush paint delta points at persistent overlay voxel");
        Check(overlayVoxel == delta.voxel,
            "sparse brush paint delta voxel matches persistent overlay value");
        Check(world.SampleCollisionStatus(worldX, worldY, worldZ) == CollisionSampleStatus::KnownSolid,
            "sparse brush paint delta is immediately collision-solid");
    }
    Check(world.GetStats().brushVoxelsEditedLastStroke == painted,
        "sparse brush records edited voxel count");
    Check(world.GetStats().brushBricksTouchedLastStroke > 0,
        "sparse brush records touched bricks");
    Check(world.SampleCollisionStatus(900, 700, -900) == CollisionSampleStatus::KnownSolid,
        "sparse brush paint is immediately collision-authoritative");
    Check(world.GetStats().requestedBricks > 0,
        "sparse brush requests render bricks for visible edits");

    Check(world.PumpGeneration(8) > 0, "sparse brush requested brick generation");
    SparseBrickUploadPacket packet;
    uint32_t uploads = 0;
    while (world.PopNextUpload(&packet)) {
        Check(world.CompleteUpload(packet), "complete sparse brush upload");
        ++uploads;
    }
    Check(uploads > 0, "sparse brush uploads touched bricks");

    std::vector<SparseEditDelta> eraseDeltas;
    std::vector<SparseEditDelta> previewEraseDeltas;
    const uint32_t previewErased = world.PreviewBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Air,
        1,
        0,
        1.0f,
        78u,
        0,
        0,
        0,
        false,
        &previewEraseDeltas);
    const uint32_t erased = world.ApplyBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Air,
        1,
        0,
        1.0f,
        78u,
        0,
        0,
        0,
        false,
        true,
        &eraseDeltas);
    Check(erased == previewErased,
        "sparse brush committed erase matches preview edit count");
    Check(eraseDeltas.size() == previewEraseDeltas.size(),
        "sparse brush committed erase matches preview delta count");
    for (size_t i = 0; i < eraseDeltas.size(); ++i) {
        Check(eraseDeltas[i].coord == previewEraseDeltas[i].coord &&
              eraseDeltas[i].packedLocal == previewEraseDeltas[i].packedLocal &&
              eraseDeltas[i].voxel == previewEraseDeltas[i].voxel,
            "sparse brush committed erase delta matches preview delta");
    }
    Check(erased > 0, "sparse brush erases edited solid voxels");
    Check(eraseDeltas.size() == erased,
        "sparse brush returns one persistent delta per erased voxel");
    for (const SparseEditDelta& delta : eraseDeltas) {
        const LocalVoxelCoord local = UnpackSparseEditLocal(delta.packedLocal);
        const int32_t worldX = delta.coord.x * SPARSE_BRICK_SIZE + local.x;
        const int32_t worldY = delta.coord.y * SPARSE_BRICK_SIZE + local.y;
        const int32_t worldZ = delta.coord.z * SPARSE_BRICK_SIZE + local.z;
        uint32_t overlayVoxel = 0;
        Check(world.GetEdits().TryGetVoxel(worldX, worldY, worldZ, &overlayVoxel),
            "sparse brush erase delta points at persistent overlay voxel");
        Check(overlayVoxel == delta.voxel,
            "sparse brush erase delta voxel matches persistent overlay value");
        Check(VENPOD::Utils::UnpackMaterial(delta.voxel) == VENPOD::Utils::Material::Air,
            "sparse brush erase delta records air material");
    }
    Check(world.SampleCollisionStatus(900, 700, -900) == CollisionSampleStatus::KnownAir,
        "sparse brush erase is immediately collision-authoritative");

    SparseVoxelWorld generatedWorld;
    Check(generatedWorld.Initialize({4, 16, 12345u}), "generated brush world initialize");
    const int32_t solidX = 96;
    const int32_t solidZ = 96;
    const int32_t solidY = static_cast<int32_t>(generatedWorld.GetTerrain().HeightAt(solidX, solidZ)) - 8;
    std::vector<SparseEditDelta> rejectedDeltas;
    std::vector<SparseEditDelta> previewRejectedDeltas;
    const uint32_t previewRejectedPaint = generatedWorld.PreviewBrushEdit(
        static_cast<float>(solidX) + 0.5f,
        static_cast<float>(solidY) + 0.5f,
        static_cast<float>(solidZ) + 0.5f,
        1.1f,
        VENPOD::Utils::Material::Glass,
        0,
        0,
        1.0f,
        79u,
        0,
        0,
        0,
        false,
        &previewRejectedDeltas);
    const uint32_t rejectedPaint = generatedWorld.ApplyBrushEdit(
        static_cast<float>(solidX) + 0.5f,
        static_cast<float>(solidY) + 0.5f,
        static_cast<float>(solidZ) + 0.5f,
        1.1f,
        VENPOD::Utils::Material::Glass,
        0,
        0,
        1.0f,
        79u,
        0,
        0,
        0,
        false,
        false,
        &rejectedDeltas);
    Check(previewRejectedPaint == 0 && previewRejectedDeltas.empty(),
        "sparse brush preview rejects fake paint-over-solid deltas");
    Check(rejectedPaint == 0, "sparse paint mode does not overwrite generated solid terrain");
    Check(rejectedDeltas.empty(),
        "sparse brush rejected paint produces no fake persistent deltas");

    std::vector<SparseEditDelta> replaceDeltas;
    std::vector<SparseEditDelta> previewReplaceDeltas;
    const uint32_t previewReplaced = generatedWorld.PreviewBrushEdit(
        static_cast<float>(solidX) + 0.5f,
        static_cast<float>(solidY) + 0.5f,
        static_cast<float>(solidZ) + 0.5f,
        1.1f,
        VENPOD::Utils::Material::Glass,
        2,
        0,
        1.0f,
        80u,
        0,
        0,
        0,
        false,
        &previewReplaceDeltas);
    const uint32_t replaced = generatedWorld.ApplyBrushEdit(
        static_cast<float>(solidX) + 0.5f,
        static_cast<float>(solidY) + 0.5f,
        static_cast<float>(solidZ) + 0.5f,
        1.1f,
        VENPOD::Utils::Material::Glass,
        2,
        0,
        1.0f,
        80u,
        0,
        0,
        0,
        false,
        false,
        &replaceDeltas);
    Check(replaced == previewReplaced && replaceDeltas.size() == previewReplaceDeltas.size(),
        "sparse brush committed replace matches preview deltas");
    Check(replaced > 0, "sparse replace mode can overwrite generated solid terrain");
    Check(replaceDeltas.size() == replaced,
        "sparse replace returns exact persistent deltas");
    Check(generatedWorld.GetStats().requestedBricks == 0,
        "sparse brush can update collision/edit overlay without requesting render residency");

    SparseVoxelWorld negativeWorld;
    Check(negativeWorld.Initialize({4, 16, 12345u}), "negative brush delta world initialize");
    std::vector<SparseEditDelta> negativePaintDeltas;
    const uint32_t negativePainted = negativeWorld.ApplyBrushEdit(
        -32.5f,
        480.5f,
        -33.5f,
        1.0f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        81u,
        0,
        0,
        0,
        false,
        false,
        &negativePaintDeltas);
    Check(negativePainted > 0, "sparse brush paints negative-coordinate air voxels");
    Check(negativePaintDeltas.size() == negativePainted,
        "negative-coordinate sparse brush returns exact delta count");
    bool sawExpectedNegativeBrick = false;
    for (const SparseEditDelta& delta : negativePaintDeltas) {
        const LocalVoxelCoord local = UnpackSparseEditLocal(delta.packedLocal);
        const int32_t worldX = delta.coord.x * SPARSE_BRICK_SIZE + local.x;
        const int32_t worldY = delta.coord.y * SPARSE_BRICK_SIZE + local.y;
        const int32_t worldZ = delta.coord.z * SPARSE_BRICK_SIZE + local.z;
        Check(BrickCoord::FromWorldVoxel(worldX, worldY, worldZ) == delta.coord,
            "negative-coordinate sparse brush delta round-trips brick coord");
        Check(LocalVoxelFromWorld(worldX, worldY, worldZ) == local,
            "negative-coordinate sparse brush delta round-trips local coord");
        sawExpectedNegativeBrick = sawExpectedNegativeBrick ||
            (delta.coord.x < 0 && delta.coord.z < 0);
    }
    Check(sawExpectedNegativeBrick,
        "negative-coordinate sparse brush emits negative brick coordinates");
}

void TestSparseRaycast() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "sparse raycast world initialize");

    SparseRaycastHit groundHit = world.Raycast(96.5f, 700.0f, 96.5f, 0.0f, -1.0f, 0.0f, 1200.0f);
    Check(groundHit.hit, "sparse raycast hits generated terrain");
    Check(groundHit.normalY == 1, "sparse raycast reports upward face normal on downward hit");
    Check(groundHit.distance > 0.0f, "sparse raycast reports positive distance");

    const uint32_t painted = world.ApplyBrushEdit(
        128.5f,
        500.5f,
        128.5f,
        1.0f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        91u,
        0,
        0,
        0,
        false,
        false);
    Check(painted > 0, "sparse raycast test paints floating edit");
    SparseRaycastHit editHit = world.Raycast(128.5f, 500.5f, 100.0f, 0.0f, 0.0f, 1.0f, 80.0f);
    Check(editHit.hit, "sparse raycast hits persistent edit overlay");
    Check(editHit.fromEdit, "sparse raycast reports edit source");
    Check(editHit.normalZ == -1, "sparse raycast reports entered face normal");

    const uint32_t erased = world.ApplyBrushEdit(
        128.5f,
        500.5f,
        128.5f,
        1.0f,
        VENPOD::Utils::Material::Air,
        1,
        0,
        1.0f,
        92u,
        0,
        0,
        0,
        false,
        false);
    Check(erased > 0, "sparse raycast test erases floating edit");
    SparseRaycastHit erasedHit = world.Raycast(128.5f, 500.5f, 100.0f, 0.0f, 0.0f, 1.0f, 80.0f);
    Check(!erasedHit.hit, "sparse raycast no longer hits erased edit in high air");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    world.SetEditedVoxel(-32, 480, -33, stone);
    SparseRaycastHit negativeEditHit = world.Raycast(-31.5f, 480.5f, -10.0f, 0.0f, 0.0f, -1.0f, 80.0f);
    Check(negativeEditHit.hit, "sparse CPU raycast hits negative-coordinate edit");
    Check(negativeEditHit.fromEdit, "negative-coordinate raycast hit reports edit source");
    Check(negativeEditHit.voxelX == -32 && negativeEditHit.voxelY == 480 && negativeEditHit.voxelZ == -33,
        "negative-coordinate raycast returns exact world voxel");
    Check(negativeEditHit.normalZ == 1, "negative-coordinate raycast normal enters from positive Z");

    Check(!world.Raycast(
            std::numeric_limits<float>::quiet_NaN(),
            500.0f,
            100.0f,
            0.0f,
            0.0f,
            1.0f,
            80.0f).hit,
        "sparse CPU raycast rejects non-finite origins");
    Check(!world.Raycast(
            128.5f,
            500.5f,
            100.0f,
            0.0f,
            std::numeric_limits<float>::infinity(),
            1.0f,
            80.0f).hit,
        "sparse CPU raycast rejects non-finite directions");
    Check(!world.Raycast(
            128.5f,
            500.5f,
            100.0f,
            0.0f,
            0.0f,
            1.0f,
            std::numeric_limits<float>::infinity()).hit,
        "sparse CPU raycast rejects non-finite max distance");
    Check(!world.Raycast(
            static_cast<float>(std::numeric_limits<int32_t>::max()),
            500.0f,
            100.0f,
            1.0f,
            0.0f,
            0.0f,
            std::numeric_limits<float>::max()).hit,
        "sparse CPU raycast fails closed at positive world-coordinate overflow boundary");
}

void TestSparseVoxelWorldEviction() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "eviction world initialize");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const BrickCoord nearCoord{0, 1000, 0};
    const BrickCoord farCoord{8, 1000, 0};
    const BrickCoord editedCoord{9, 1000, 0};
    const BrickCoord visibleCoord{10, 1000, 0};
    world.SetEditedVoxel(
        nearCoord.x * SPARSE_BRICK_SIZE,
        nearCoord.y * SPARSE_BRICK_SIZE,
        nearCoord.z * SPARSE_BRICK_SIZE,
        stone);
    world.SetEditedVoxel(
        farCoord.x * SPARSE_BRICK_SIZE,
        farCoord.y * SPARSE_BRICK_SIZE,
        farCoord.z * SPARSE_BRICK_SIZE,
        stone);
    world.SetEditedVoxel(
        editedCoord.x * SPARSE_BRICK_SIZE,
        editedCoord.y * SPARSE_BRICK_SIZE,
        editedCoord.z * SPARSE_BRICK_SIZE,
        stone);
    world.SetEditedVoxel(
        visibleCoord.x * SPARSE_BRICK_SIZE,
        visibleCoord.y * SPARSE_BRICK_SIZE,
        visibleCoord.z * SPARSE_BRICK_SIZE,
        stone);
    Check(world.RequestBrick(nearCoord), "request near brick for eviction test");
    Check(world.RequestBrick(farCoord), "request far brick for eviction test");
    Check(world.RequestBrick(editedCoord), "request edited brick for eviction test");
    Check(world.RequestBrick(visibleCoord), "request visible brick for eviction test");
    Check(world.MarkResidencyClass(visibleCoord, SparseResidencyClass::Visible),
        "mark visible brick residency class");
    Check(world.PumpGeneration(4) == 4, "generate eviction test bricks");

    SparseBrickUploadPacket packet;
    uint32_t completed = 0;
    while (world.PopNextUpload(&packet)) {
        Check(world.CompleteUpload(packet), "complete eviction test upload");
        ++completed;
    }
    Check(completed == 4, "all eviction test bricks uploaded");
    Check(world.PumpSurfaceExtraction(4) == 4, "eviction test extracts uploaded surfaces");
    Check(world.GetStats().residentBricks == 4, "eviction test starts with four residents");
    Check(world.GetStats().surfaceCachedBricks == 4, "surface cache tracks uploaded eviction bricks");

    world.SetEditedVoxel(
        editedCoord.x * SPARSE_BRICK_SIZE + 1,
        editedCoord.y * SPARSE_BRICK_SIZE,
        editedCoord.z * SPARSE_BRICK_SIZE,
        stone);
    SparseBrickUploadPacket editedPacket;
    Check(world.PopNextUpload(&editedPacket), "edited brick queues republish before eviction");
    Check(world.CompleteUpload(editedPacket), "edited brick republished before eviction");
    Check(world.PumpSurfaceExtraction(1) == 1, "eviction test extracts edited surface");

    const uint32_t evicted = world.TrimResidentBricks(nearCoord, 1, 1, 1);
    Check(evicted == 1, "trim evicts one brick per budget");
    Check(world.GetPool().IsResident(nearCoord), "near brick survives trim");
    Check(!world.GetPool().IsResident(farCoord), "far brick evicted by trim");
    Check(world.GetPool().IsResident(editedCoord), "edited brick survives trim");
    Check(world.GetPool().IsResident(visibleCoord), "visible feedback brick survives before speculative brick");
    Check(world.GetPool().PageTable().Count() == 3, "CPU page table invalidated evicted brick");
    Check(world.GetStats().evictedBricksLastFrame == 1, "eviction stats last frame");
    Check(world.GetStats().surfaceCachedBricks == 3, "surface cache removes evicted brick");
    Check(world.GetStats().surfaceBricksRemovedLastFrame == 1, "surface cache remove stats track eviction");
    Check(world.GetStats().evictionQueuedBricks == 1, "eviction queues GPU invalidation");

    SparsePageInvalidationPacket invalidation;
    Check(world.PopNextInvalidation(&invalidation), "pop eviction invalidation");
    Check(invalidation.coord == farCoord, "invalidation coord matches evicted brick");
    Check(invalidation.entryIndex < 32, "invalidation entry index in range");
    Check(invalidation.pageIndex != INVALID_BRICK_PAGE, "invalidation records old physical page");
    Check(!world.PopNextInvalidation(&invalidation), "only one invalidation queued");

    const BrickCoord reuseCoord{20, 0, 0};
    Check(world.RequestBrick(reuseCoord), "page can be requested after eviction");
    BrickResidentRecord reusedRecord;
    Check(world.GetPool().GetRecord(reuseCoord, &reusedRecord), "get reused page record");
    if (reusedRecord.pageIndex == invalidation.pageIndex) {
        Check(reusedRecord.generation == invalidation.generation + 1u,
            "reused physical page increments generation after eviction");
    }

    SparseVoxelWorld backgroundWorld;
    Check(backgroundWorld.Initialize({6, 64, 12345u}), "background trim world initialize");
    const BrickCoord bgCenter{0, 0, 0};
    const BrickCoord bgSpeculative{12, 0, 0};
    const BrickCoord bgVisible{13, 0, 0};
    const BrickCoord bgCollision{14, 0, 0};
    const BrickCoord bgEdited{15, 0, 0};
    Check(backgroundWorld.RequestBrickDetailed(bgCenter, false) == SparseBrickRequestResult::Allocated,
        "background trim request center");
    Check(backgroundWorld.RequestBrickDetailed(bgSpeculative, false) == SparseBrickRequestResult::Allocated,
        "background trim request speculative");
    Check(backgroundWorld.RequestBrickDetailed(bgVisible, false) == SparseBrickRequestResult::Allocated,
        "background trim request visible");
    Check(backgroundWorld.RequestBrickDetailed(bgCollision, false) == SparseBrickRequestResult::Allocated,
        "background trim request collision");
    Check(backgroundWorld.RequestBrickDetailed(bgEdited, false) == SparseBrickRequestResult::Allocated,
        "background trim request edited");
    Check(backgroundWorld.MarkResidencyClass(bgVisible, SparseResidencyClass::Visible),
        "background trim marks visible");
    Check(backgroundWorld.MarkResidencyClass(bgCollision, SparseResidencyClass::Collision),
        "background trim marks collision");
    Check(backgroundWorld.PumpGeneration(5) == 5, "background trim generates all pages");
    while (backgroundWorld.PopNextUpload(&packet)) {
        Check(backgroundWorld.CompleteUpload(packet), "background trim complete upload");
    }
    backgroundWorld.SetEditedVoxel(
        bgEdited.x * SPARSE_BRICK_SIZE,
        bgEdited.y * SPARSE_BRICK_SIZE,
        bgEdited.z * SPARSE_BRICK_SIZE,
        stone);
    Check(backgroundWorld.PopNextUpload(&editedPacket), "background trim edited queues upload");
    Check(backgroundWorld.CompleteUpload(editedPacket), "background trim edited upload complete");
    const uint32_t bgEvicted = backgroundWorld.TrimBackgroundResidentBricks(bgCenter, 1, 1, 2, 300);
    Check(bgEvicted == 2, "background trim evicts background pages under budget");
    Check(!backgroundWorld.GetPool().IsResident(bgSpeculative),
        "background trim evicts speculative page first");
    Check(!backgroundWorld.GetPool().IsResident(bgVisible),
        "background trim can evict visible page after speculative pages");
    Check(backgroundWorld.GetPool().IsResident(bgCollision),
        "background trim protects collision page");
    Check(backgroundWorld.GetPool().IsResident(bgEdited),
        "background trim protects edited page");

    SparseVoxelWorld queuedTrimWorld;
    Check(queuedTrimWorld.Initialize({5, 32, 12345u}), "queued trim world initialize");
    const BrickCoord queuedCenter{0, 0, 0};
    const BrickCoord queuedSpeculative{20, 0, 0};
    const BrickCoord queuedVisible{21, 0, 0};
    const BrickCoord queuedCollision{22, 0, 0};
    Check(queuedTrimWorld.RequestBrick(queuedCenter), "queued trim request center");
    Check(queuedTrimWorld.RequestBrick(queuedSpeculative), "queued trim request speculative");
    Check(queuedTrimWorld.RequestBrick(queuedVisible), "queued trim request visible");
    Check(queuedTrimWorld.RequestBrick(queuedCollision), "queued trim request collision");
    Check(queuedTrimWorld.MarkResidencyClass(queuedVisible, SparseResidencyClass::Visible),
        "queued trim marks visible");
    Check(queuedTrimWorld.MarkResidencyClass(queuedCollision, SparseResidencyClass::Collision),
        "queued trim marks collision");
    Check(queuedTrimWorld.GetStats().freePages == 1, "queued trim starts with allocated queued pages");
    const uint32_t queuedEvicted =
        queuedTrimWorld.TrimQueuedBackgroundBricks(queuedCenter, 1, 1, 2, 500);
    Check(queuedEvicted == 2, "queued trim evicts stale queued background pages");
    Check(!queuedTrimWorld.GetPool().TryGetPage(queuedSpeculative),
        "queued trim frees speculative requested page");
    Check(!queuedTrimWorld.GetPool().TryGetPage(queuedVisible),
        "queued trim can free stale visible requested page outside keep radius");
    Check(queuedTrimWorld.GetPool().TryGetPage(queuedCollision),
        "queued trim protects collision requested page");
    Check(queuedTrimWorld.GenerationQueueSize() == 2,
        "queued trim removes evicted pages from generation queue accounting");

    SparseVoxelWorld boundaryQueuedTrimWorld;
    Check(boundaryQueuedTrimWorld.Initialize({2, 32, 12345u}),
        "boundary queued trim world initialize");
    const BrickCoord boundaryQueuedCoord{
        std::numeric_limits<int32_t>::max(),
        0,
        0};
    const BrickCoord boundaryQueuedCenter{
        std::numeric_limits<int32_t>::min(),
        0,
        0};
    Check(boundaryQueuedTrimWorld.RequestBrick(boundaryQueuedCoord),
        "boundary queued trim request extreme page");
    const uint32_t boundaryQueuedEvicted =
        boundaryQueuedTrimWorld.TrimQueuedBackgroundBricks(boundaryQueuedCenter, 0, 0, 1, 700);
    Check(boundaryQueuedEvicted == 1,
        "boundary queued trim evicts extreme queued page without signed delta overflow");
    Check(!boundaryQueuedTrimWorld.GetPool().TryGetPage(boundaryQueuedCoord),
        "boundary queued trim frees extreme requested page");

    SparseVoxelWorld largeDistanceTrimWorld;
    Check(largeDistanceTrimWorld.Initialize({3, 64, 12345u}),
        "large-distance trim world initialize");
    const BrickCoord largeTrimCenter{-100000, 16, 0};
    const BrickCoord largeTrimNear{-99999, 16, 0};
    const BrickCoord largeTrimFar{100000, 16, 0};
    Check(largeDistanceTrimWorld.RequestBrickDetailed(largeTrimCenter, false) == SparseBrickRequestResult::Allocated,
        "large-distance trim request center");
    Check(largeDistanceTrimWorld.RequestBrickDetailed(largeTrimNear, false) == SparseBrickRequestResult::Allocated,
        "large-distance trim request near");
    Check(largeDistanceTrimWorld.RequestBrickDetailed(largeTrimFar, false) == SparseBrickRequestResult::Allocated,
        "large-distance trim request far");
    Check(largeDistanceTrimWorld.PumpGeneration(3) == 3,
        "large-distance trim generates pages");
    while (largeDistanceTrimWorld.PopNextUpload(&packet)) {
        Check(largeDistanceTrimWorld.CompleteUpload(packet),
            "large-distance trim complete upload");
    }
    const uint32_t largeDistanceEvicted =
        largeDistanceTrimWorld.TrimResidentBricks(largeTrimCenter, 2, 2, 1);
    Check(largeDistanceEvicted == 1,
        "large-distance trim evicts with saturating distance score");
    Check(!largeDistanceTrimWorld.GetPool().IsResident(largeTrimFar),
        "large-distance trim evicts far page");
    Check(largeDistanceTrimWorld.GetPool().IsResident(largeTrimNear),
        "large-distance trim keeps near page");
}

void TestSparsePriorityReplacement() {
    SparseVoxelWorld world;
    Check(world.Initialize({4, 16, 12345u}), "priority replacement world initialize");

    const BrickCoord center{0, 0, 0};
    const BrickCoord speculativeFar{8, 0, 0};
    const BrickCoord visibleFar{9, 0, 0};
    const BrickCoord editedFar{10, 0, 0};
    Check(world.RequestBrickDetailed(center, false) == SparseBrickRequestResult::Allocated,
        "replacement request center");
    Check(world.RequestBrickDetailed(speculativeFar, false) == SparseBrickRequestResult::Allocated,
        "replacement request speculative");
    Check(world.RequestBrickDetailed(visibleFar, false) == SparseBrickRequestResult::Allocated,
        "replacement request visible");
    Check(world.RequestBrickDetailed(editedFar, false) == SparseBrickRequestResult::Allocated,
        "replacement request edited");
    Check(world.MarkResidencyClass(center, SparseResidencyClass::Collision),
        "replacement center marked collision");
    Check(world.MarkResidencyClass(visibleFar, SparseResidencyClass::Visible),
        "replacement visible marked visible");
    Check(world.PumpGeneration(4) == 4, "replacement generate all pages");

    SparseBrickUploadPacket packet;
    while (world.PopNextUpload(&packet)) {
        Check(world.CompleteUpload(packet), "replacement complete upload");
    }

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    world.SetEditedVoxel(editedFar.x * SPARSE_BRICK_SIZE, 0, 0, stone);
    SparseBrickUploadPacket editedPacket;
    Check(world.PopNextUpload(&editedPacket), "replacement edited queues upload");
    Check(world.CompleteUpload(editedPacket), "replacement edited upload complete");

    Check(world.GetStats().freePages == 0, "replacement starts with full pool");
    const uint32_t evicted = world.EvictLowerPriorityForRequest(
        center,
        SparseResidencyClass::Visible,
        1,
        1,
        1);
    Check(evicted == 1, "visible request replacement evicts one lower-priority page");
    Check(!world.GetPool().IsResident(speculativeFar), "replacement evicts speculative far page");
    Check(world.GetPool().IsResident(visibleFar), "replacement keeps visible page");
    Check(world.GetPool().IsResident(editedFar), "replacement keeps edited page");
    Check(world.GetStats().freePages == 1, "replacement frees one page");

    SparsePageInvalidationPacket invalidation;
    Check(world.PopNextInvalidation(&invalidation), "replacement queues invalidation before reuse");
    Check(invalidation.coord == speculativeFar, "replacement invalidation targets evicted page");

    const BrickCoord newVisible{20, 0, 0};
    Check(world.RequestBrickDetailed(newVisible, false) == SparseBrickRequestResult::Allocated,
        "replacement request can reuse freed page");
    Check(world.MarkResidencyClass(newVisible, SparseResidencyClass::Visible),
        "replacement new page marked visible");

    SparseVoxelWorld visibleWorld;
    Check(visibleWorld.Initialize({3, 16, 12345u}), "same-class replacement world initialize");
    const BrickCoord visibleA{0, 0, 0};
    const BrickCoord visibleB{6, 0, 0};
    const BrickCoord visibleC{12, 0, 0};
    Check(visibleWorld.RequestBrickDetailed(visibleA, false) == SparseBrickRequestResult::Allocated,
        "same-class request visible A");
    Check(visibleWorld.RequestBrickDetailed(visibleB, false) == SparseBrickRequestResult::Allocated,
        "same-class request visible B");
    Check(visibleWorld.RequestBrickDetailed(visibleC, false) == SparseBrickRequestResult::Allocated,
        "same-class request visible C");
    Check(visibleWorld.MarkResidencyClass(visibleA, SparseResidencyClass::Visible),
        "same-class mark visible A");
    Check(visibleWorld.MarkResidencyClass(visibleB, SparseResidencyClass::Visible),
        "same-class mark visible B");
    Check(visibleWorld.MarkResidencyClass(visibleC, SparseResidencyClass::Visible),
        "same-class mark visible C");
    Check(visibleWorld.PumpGeneration(3) == 3, "same-class generate all pages");
    while (visibleWorld.PopNextUpload(&packet)) {
        Check(visibleWorld.CompleteUpload(packet), "same-class complete upload");
    }
    const uint32_t sameClassEvicted = visibleWorld.EvictLowerPriorityForRequest(
        visibleA,
        SparseResidencyClass::Visible,
        1,
        1,
        1);
    Check(sameClassEvicted == 1, "visible replacement can evict stale visible page outside hard keep");
    Check(visibleWorld.GetPool().IsResident(visibleA), "same-class hard keep protects nearby visible page");
    Check(!visibleWorld.GetPool().IsResident(visibleC), "same-class replacement evicts farthest visible page");

    SparseVoxelWorld ageWorld;
    Check(ageWorld.Initialize({4, 16, 12345u}), "age-aware replacement world initialize");
    const BrickCoord ageCenter{0, 0, 0};
    const BrickCoord oldVisible{8, 0, 0};
    const BrickCoord newVisibleSameDistance{-8, 0, 0};
    const BrickCoord collisionTouched{0, 4, 0};
    Check(ageWorld.RequestBrickDetailed(ageCenter, false) == SparseBrickRequestResult::Allocated,
        "age request center");
    Check(ageWorld.RequestBrickDetailed(oldVisible, false) == SparseBrickRequestResult::Allocated,
        "age request old visible");
    Check(ageWorld.RequestBrickDetailed(newVisibleSameDistance, false) == SparseBrickRequestResult::Allocated,
        "age request new visible");
    Check(ageWorld.RequestBrickDetailed(collisionTouched, false) == SparseBrickRequestResult::Allocated,
        "age request collision touched");
    Check(ageWorld.TouchResidencyClass(ageCenter, SparseResidencyClass::Collision, 10),
        "age center touched collision");
    Check(ageWorld.TouchResidencyClass(oldVisible, SparseResidencyClass::Visible, 20),
        "old visible touched at older frame");
    Check(ageWorld.TouchResidencyClass(newVisibleSameDistance, SparseResidencyClass::Visible, 200),
        "new visible touched at newer frame");
    Check(ageWorld.TouchResidencyClass(collisionTouched, SparseResidencyClass::Collision, 30),
        "collision page touched");
    Check(ageWorld.PumpGeneration(4) == 4, "age replacement generate all pages");
    while (ageWorld.PopNextUpload(&packet)) {
        Check(ageWorld.CompleteUpload(packet), "age replacement complete upload");
    }
    const uint32_t ageEvicted = ageWorld.EvictLowerPriorityForRequest(
        ageCenter,
        SparseResidencyClass::Visible,
        1,
        1,
        1,
        240);
    Check(ageEvicted == 1, "age-aware replacement evicts one visible page");
    Check(!ageWorld.GetPool().IsResident(oldVisible), "age-aware replacement evicts older visible page");
    Check(ageWorld.GetPool().IsResident(newVisibleSameDistance),
        "age-aware replacement preserves newer visible page at same distance");
    Check(ageWorld.GetPool().IsResident(collisionTouched),
        "visible replacement does not evict higher collision residency");

    SparseVoxelWorld largeDistanceReplacementWorld;
    Check(largeDistanceReplacementWorld.Initialize({3, 64, 12345u}),
        "large-distance replacement world initialize");
    const BrickCoord largeReplacementCenter{-100000, 16, 0};
    const BrickCoord largeReplacementSpeculative{100000, 16, 0};
    const BrickCoord largeReplacementVisible{-99999, 16, 0};
    Check(largeDistanceReplacementWorld.RequestBrickDetailed(largeReplacementCenter, false) == SparseBrickRequestResult::Allocated,
        "large-distance replacement request center");
    Check(largeDistanceReplacementWorld.RequestBrickDetailed(largeReplacementSpeculative, false) == SparseBrickRequestResult::Allocated,
        "large-distance replacement request speculative");
    Check(largeDistanceReplacementWorld.RequestBrickDetailed(largeReplacementVisible, false) == SparseBrickRequestResult::Allocated,
        "large-distance replacement request visible");
    Check(largeDistanceReplacementWorld.MarkResidencyClass(
            largeReplacementCenter,
            SparseResidencyClass::Collision),
        "large-distance replacement marks center collision");
    Check(largeDistanceReplacementWorld.MarkResidencyClass(
            largeReplacementVisible,
            SparseResidencyClass::Visible),
        "large-distance replacement marks visible");
    Check(largeDistanceReplacementWorld.PumpGeneration(3) == 3,
        "large-distance replacement generates pages");
    while (largeDistanceReplacementWorld.PopNextUpload(&packet)) {
        Check(largeDistanceReplacementWorld.CompleteUpload(packet),
            "large-distance replacement complete upload");
    }
    const uint32_t largeReplacementEvicted =
        largeDistanceReplacementWorld.EvictLowerPriorityForRequest(
            largeReplacementCenter,
            SparseResidencyClass::Visible,
            2,
            2,
            1,
            900);
    Check(largeReplacementEvicted == 1,
        "large-distance replacement evicts with saturating distance score");
    Check(!largeDistanceReplacementWorld.GetPool().IsResident(largeReplacementSpeculative),
        "large-distance replacement evicts far speculative page");
    Check(largeDistanceReplacementWorld.GetPool().IsResident(largeReplacementVisible),
        "large-distance replacement keeps protected visible page");
}

void TestSparsePriorityQueues() {
    SparseVoxelWorld statsWorld;
    Check(statsWorld.Initialize({8, 32, 12345u}), "stats deferral world initialize");
    statsWorld.SetStatsRefreshDeferred(true);
    Check(statsWorld.RequestBrick({0, -64, 0}), "stats deferral queues request");
    Check(statsWorld.GenerationQueueSize() == 1,
        "stats deferral keeps lightweight generation queue size current");
    Check(statsWorld.GetStats().generationQueuedBricks == 0,
        "stats deferral leaves diagnostic generation count stale during request burst");
    statsWorld.FlushStats();
    Check(statsWorld.GetStats().generationQueuedBricks == 1,
        "stats deferral flush publishes diagnostic generation count");
    statsWorld.SetStatsRefreshDeferred(false);

    SparseVoxelWorld queueAccountingWorld;
    Check(queueAccountingWorld.Initialize({8, 32, 12345u}), "queue accounting world initialize");
    const BrickCoord accountingSpeculative{2, -64, 0};
    const BrickCoord accountingCollision{3, -64, 0};
    Check(queueAccountingWorld.RequestBrickDetailed(accountingSpeculative, false) == SparseBrickRequestResult::Allocated,
        "queue accounting request speculative");
    Check(queueAccountingWorld.RequestBrickDetailed(accountingCollision, false) == SparseBrickRequestResult::Allocated,
        "queue accounting request collision");
    Check(queueAccountingWorld.TouchResidencyClass(
            accountingCollision,
            SparseResidencyClass::Collision,
            7),
        "queue accounting touch collision");
    Check(queueAccountingWorld.GetStats().generationQueuedSpeculativeBricks == 1,
        "queue accounting counts speculative generation queue");
    Check(queueAccountingWorld.GetStats().generationQueuedCollisionBricks == 1,
        "queue accounting counts collision generation queue after residency touch");
    Check(queueAccountingWorld.PumpGeneration(2, 7) == 2,
        "queue accounting generates both queued bricks");
    Check(queueAccountingWorld.GetStats().generationQueuedBricks == 0,
        "queue accounting clears generation queue count after pump");
    Check(queueAccountingWorld.GetStats().uploadQueuedSpeculativeBricks == 1,
        "queue accounting counts speculative upload queue");
    Check(queueAccountingWorld.GetStats().uploadQueuedCollisionBricks == 1,
        "queue accounting counts collision upload queue");
    SparseBrickUploadPacket accountingPacket;
    Check(queueAccountingWorld.PopNextUploadForClass(
            &accountingPacket,
            SparseResidencyClass::Collision,
            7),
        "queue accounting pops collision upload");
    Check(queueAccountingWorld.GetStats().uploadQueuedCollisionBricks == 0,
        "queue accounting clears consumed collision upload class");
    Check(queueAccountingWorld.GetStats().uploadQueuedSpeculativeBricks == 1,
        "queue accounting preserves remaining speculative upload class");

    SparseVoxelWorld generationWorld;
    Check(generationWorld.Initialize({8, 32, 12345u}), "priority generation world initialize");
    const BrickCoord oldSpeculative{20, 0, 0};
    const BrickCoord newVisible{21, 0, 0};
    Check(generationWorld.RequestBrick(oldSpeculative), "priority queue request speculative first");
    Check(generationWorld.TouchResidencyClass(oldSpeculative, SparseResidencyClass::Speculative, 10),
        "priority queue touch speculative");
    Check(generationWorld.RequestBrick(newVisible), "priority queue request visible second");
    Check(generationWorld.TouchResidencyClass(newVisible, SparseResidencyClass::Visible, 100),
        "priority queue touch visible");
    Check(generationWorld.PumpGeneration(1, 100) == 1,
        "priority queue generates one requested brick");
    SparseBrickUploadPacket packet;
    Check(generationWorld.PopNextUpload(&packet), "priority queue pops generated upload");
    Check(packet.coord == newVisible, "generation priority promotes visible before older speculative");

    SparseVoxelWorld valueGenerationWorld;
    Check(valueGenerationWorld.Initialize({8, 32, 12345u}), "value generation world initialize");
    const BrickCoord farGeneration{2000, -64, 0};
    const BrickCoord nearGeneration{0, -64, 0};
    Check(valueGenerationWorld.RequestBrickDetailed(farGeneration, false) == SparseBrickRequestResult::Allocated,
        "value generation request far visible");
    Check(valueGenerationWorld.TouchResidencyClass(farGeneration, SparseResidencyClass::Visible, 100),
        "value generation touch far visible newer");
    Check(valueGenerationWorld.RequestBrickDetailed(nearGeneration, false) == SparseBrickRequestResult::Allocated,
        "value generation request near visible");
    Check(valueGenerationWorld.TouchResidencyClass(nearGeneration, SparseResidencyClass::Visible, 10),
        "value generation touch near visible older");
    Check(valueGenerationWorld.PumpGenerationAround(1, BrickCoord{0, -64, 0}, 100) == 1,
        "value generation generates one focused brick");
    Check(valueGenerationWorld.PopNextUpload(&packet, 100),
        "value generation queued focused upload");
    Check(packet.coord == nearGeneration,
        "value generation chooses nearer visible brick over much newer far brick");

    SparseVoxelWorld generationUpgradeWorld;
    Check(generationUpgradeWorld.Initialize({8, 32, 12345u}), "generation class upgrade world initialize");
    const BrickCoord upgradeGeneration{1, -64, 0};
    Check(generationUpgradeWorld.RequestBrickDetailed(upgradeGeneration, false) == SparseBrickRequestResult::Allocated,
        "generation class upgrade request");
    Check(generationUpgradeWorld.TouchResidencyClass(upgradeGeneration, SparseResidencyClass::Speculative, 1),
        "generation class upgrade initially speculative");
    Check(generationUpgradeWorld.TouchResidencyClass(upgradeGeneration, SparseResidencyClass::Collision, 20),
        "generation class upgrade retouches requested brick as collision");
    Check(generationUpgradeWorld.GenerationClassQueueSize() == 1,
        "generation class upgrade removes stale class aliases before requeue");
    Check(generationUpgradeWorld.PumpGenerationAround(1, upgradeGeneration, 20) == 1,
        "generation class upgrade generates retouched collision request through class bucket");
    Check(generationUpgradeWorld.PopNextUploadForClass(&packet, SparseResidencyClass::Collision, 20),
        "generation class upgrade queues collision upload");
    Check(packet.coord == upgradeGeneration,
        "generation class upgrade produced the retouched collision brick upload");

    SparseVoxelWorld requestedEditedClassWorld;
    Check(requestedEditedClassWorld.Initialize({8, 32, 12345u}),
        "requested edited class world initialize");
    const BrickCoord requestedEdited{2, -64, 1};
    Check(requestedEditedClassWorld.RequestBrickDetailed(requestedEdited, false) ==
              SparseBrickRequestResult::Allocated,
        "requested edited class allocates brick");
    Check(requestedEditedClassWorld.MarkResidencyClass(requestedEdited, SparseResidencyClass::Edited),
        "requested edited class can promote a non-resident active record");
    Check(requestedEditedClassWorld.PumpGenerationAround(1, requestedEdited, 40) == 1,
        "requested edited class generates through edited class queue");
    Check(requestedEditedClassWorld.PopNextUploadForClass(&packet, SparseResidencyClass::Edited, 40),
        "requested edited class queues upload in edited class bucket");
    Check(packet.coord == requestedEdited,
        "requested edited class upload packet keeps promoted coordinate");
    Check(packet.residencyClass == SparseResidencyClass::Edited,
        "requested edited class upload packet carries edited residency tag");

    SparseVoxelWorld uploadWorld;
    Check(uploadWorld.Initialize({8, 32, 12345u}), "priority upload world initialize");
    const BrickCoord queuedSpeculative{30, 0, 0};
    const BrickCoord queuedVisible{31, 0, 0};
    Check(uploadWorld.RequestBrick(queuedSpeculative), "upload priority request speculative");
    Check(uploadWorld.TouchResidencyClass(queuedSpeculative, SparseResidencyClass::Speculative, 10),
        "upload priority touch speculative");
    Check(uploadWorld.PumpGeneration(1, 10) == 1, "upload priority queue speculative upload first");
    Check(uploadWorld.RequestBrick(queuedVisible), "upload priority request visible");
    Check(uploadWorld.TouchResidencyClass(queuedVisible, SparseResidencyClass::Visible, 100),
        "upload priority touch visible");
    Check(uploadWorld.PumpGeneration(1, 100) == 1, "upload priority queue visible upload second");
    Check(uploadWorld.PopNextUpload(&packet), "upload priority pops first upload");
    Check(packet.coord == queuedVisible, "upload priority promotes visible before older queued speculative");
    Check(packet.residencyClass == SparseResidencyClass::Visible,
        "upload priority packet carries visible residency tag for publish ordering");

    SparseVoxelWorld uploadClassWorld;
    Check(uploadClassWorld.Initialize({8, 32, 12345u}), "class upload world initialize");
    const BrickCoord classSpeculative{32, -64, 0};
    const BrickCoord classVisible{33, -64, 0};
    const BrickCoord classCollision{34, -64, 0};
    Check(uploadClassWorld.RequestBrickDetailed(classSpeculative, false) == SparseBrickRequestResult::Allocated,
        "class upload request speculative");
    Check(uploadClassWorld.TouchResidencyClass(classSpeculative, SparseResidencyClass::Speculative, 10),
        "class upload touch speculative");
    Check(uploadClassWorld.RequestBrickDetailed(classVisible, false) == SparseBrickRequestResult::Allocated,
        "class upload request visible");
    Check(uploadClassWorld.TouchResidencyClass(classVisible, SparseResidencyClass::Visible, 20),
        "class upload touch visible");
    Check(uploadClassWorld.RequestBrickDetailed(classCollision, false) == SparseBrickRequestResult::Allocated,
        "class upload request collision");
    Check(uploadClassWorld.TouchResidencyClass(classCollision, SparseResidencyClass::Collision, 30),
        "class upload touch collision");
    Check(uploadClassWorld.PumpGeneration(3, 30) == 3, "class upload generates all queued bricks");
    Check(uploadClassWorld.PopNextUploadForClass(&packet, SparseResidencyClass::Speculative, 30),
        "class upload can target speculative even with protected backlog");
    Check(packet.coord == classSpeculative, "class upload targeted speculative packet");
    Check(packet.residencyClass == SparseResidencyClass::Speculative,
        "class upload packet carries targeted speculative residency tag");
    Check(uploadClassWorld.PopNextUploadForClass(&packet, SparseResidencyClass::Edited, 30) == false,
        "class upload returns false for missing edited class without consuming other classes");
    Check(uploadClassWorld.PopNextUpload(&packet, 30), "class upload fallback pops remaining highest priority");
    Check(packet.coord == classCollision, "class upload fallback preserves priority after targeted pop");
    Check(packet.residencyClass == SparseResidencyClass::Collision,
        "class upload fallback packet carries collision residency tag");

    SparseVoxelWorld uploadUpgradeWorld;
    Check(uploadUpgradeWorld.Initialize({8, 32, 12345u}), "class upload upgrade world initialize");
    const BrickCoord upgradeCoord{35, -64, 0};
    Check(uploadUpgradeWorld.RequestBrickDetailed(upgradeCoord, false) == SparseBrickRequestResult::Allocated,
        "class upload upgrade request");
    Check(uploadUpgradeWorld.TouchResidencyClass(upgradeCoord, SparseResidencyClass::Speculative, 1),
        "class upload upgrade initially speculative");
    Check(uploadUpgradeWorld.PumpGeneration(1, 1) == 1,
        "class upload upgrade generates speculative upload");
    Check(uploadUpgradeWorld.TouchResidencyClass(upgradeCoord, SparseResidencyClass::Visible, 20),
        "class upload upgrade retouches queued upload as visible");
    Check(uploadUpgradeWorld.UploadClassQueueSize() == 1,
        "class upload upgrade removes stale class aliases before requeue");
    Check(uploadUpgradeWorld.PopNextUploadForClass(&packet, SparseResidencyClass::Visible, 20),
        "class upload upgrade can pop retouched visible upload from class bucket");
    Check(packet.coord == upgradeCoord,
        "class upload upgrade returns the retouched queued upload");

    SparseVoxelWorld valueUploadWorld;
    Check(valueUploadWorld.Initialize({8, 32, 12345u}), "value upload world initialize");
    const BrickCoord nearVisible{0, -64, 0};
    const BrickCoord farVisible{2000, -64, 0};
    Check(valueUploadWorld.RequestBrickDetailed(farVisible, false) == SparseBrickRequestResult::Allocated,
        "value upload request far visible");
    Check(valueUploadWorld.TouchResidencyClass(farVisible, SparseResidencyClass::Visible, 100),
        "value upload touch far visible newer");
    Check(valueUploadWorld.RequestBrickDetailed(nearVisible, false) == SparseBrickRequestResult::Allocated,
        "value upload request near visible");
    Check(valueUploadWorld.TouchResidencyClass(nearVisible, SparseResidencyClass::Visible, 10),
        "value upload touch near visible older");
    Check(valueUploadWorld.PumpGeneration(2, 100) == 2,
        "value upload generates visible candidates");
    Check(valueUploadWorld.PopBestUploadForClass(
            &packet,
            SparseResidencyClass::Visible,
            BrickCoord{0, -64, 0},
            100),
        "value upload pops best visible by focus");
    Check(packet.coord == nearVisible,
        "value upload chooses nearer visible brick over much newer far brick");

    SparseVoxelWorld repeatedValueUploadWorld;
    Check(repeatedValueUploadWorld.Initialize({8, 32, 12345u}),
        "repeated value upload world initialize");
    const BrickCoord repeatedFar{2200, -64, 0};
    const BrickCoord repeatedMid{12, -64, 0};
    const BrickCoord repeatedNear{1, -64, 0};
    Check(repeatedValueUploadWorld.RequestBrickDetailed(repeatedFar, false) == SparseBrickRequestResult::Allocated,
        "repeated value upload request far visible");
    Check(repeatedValueUploadWorld.TouchResidencyClass(repeatedFar, SparseResidencyClass::Visible, 300),
        "repeated value upload touch far visible");
    Check(repeatedValueUploadWorld.RequestBrickDetailed(repeatedMid, false) == SparseBrickRequestResult::Allocated,
        "repeated value upload request mid visible");
    Check(repeatedValueUploadWorld.TouchResidencyClass(repeatedMid, SparseResidencyClass::Visible, 300),
        "repeated value upload touch mid visible");
    Check(repeatedValueUploadWorld.RequestBrickDetailed(repeatedNear, false) == SparseBrickRequestResult::Allocated,
        "repeated value upload request near visible");
    Check(repeatedValueUploadWorld.TouchResidencyClass(repeatedNear, SparseResidencyClass::Visible, 300),
        "repeated value upload touch near visible");
    Check(repeatedValueUploadWorld.PumpGeneration(3, 300) == 3,
        "repeated value upload generates all visible candidates");
    Check(repeatedValueUploadWorld.PopBestUploadForClass(
            &packet,
            SparseResidencyClass::Visible,
            BrickCoord{0, -64, 0},
            300),
        "repeated value upload pops first focused visible");
    Check(packet.coord == repeatedNear,
        "repeated value upload first pop chooses nearest visible brick");
    Check(repeatedValueUploadWorld.PopBestUploadForClass(
            &packet,
            SparseResidencyClass::Visible,
            BrickCoord{0, -64, 0},
            300),
        "repeated value upload pops second focused visible without queue mutation");
    Check(packet.coord == repeatedMid,
        "repeated value upload preserves cached value order after first pop");

    SparseVoxelWorld surfaceWorld;
    Check(surfaceWorld.Initialize({8, 32, 12345u}), "surface extraction priority world initialize");
    const uint32_t priorityStone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        2,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    const BrickCoord surfaceSpeculative{40, 1000, 0};
    const BrickCoord surfaceVisible{41, 1000, 0};
    surfaceWorld.SetEditedVoxel(
        surfaceSpeculative.x * SPARSE_BRICK_SIZE,
        surfaceSpeculative.y * SPARSE_BRICK_SIZE,
        surfaceSpeculative.z * SPARSE_BRICK_SIZE,
        priorityStone);
    surfaceWorld.SetEditedVoxel(
        surfaceVisible.x * SPARSE_BRICK_SIZE,
        surfaceVisible.y * SPARSE_BRICK_SIZE,
        surfaceVisible.z * SPARSE_BRICK_SIZE,
        priorityStone);
    Check(surfaceWorld.RequestBrick(surfaceSpeculative), "surface priority request speculative first");
    Check(surfaceWorld.TouchResidencyClass(surfaceSpeculative, SparseResidencyClass::Speculative, 10),
        "surface priority touch speculative");
    Check(surfaceWorld.PumpGeneration(1, 10) == 1,
        "surface priority generates speculative first");
    Check(surfaceWorld.PopNextUpload(&packet), "surface priority pops speculative upload");
    Check(packet.coord == surfaceSpeculative, "surface priority speculative queues extraction first");
    Check(surfaceWorld.CompleteUpload(packet), "surface priority completes speculative upload");

    Check(surfaceWorld.RequestBrick(surfaceVisible), "surface priority request visible second");
    Check(surfaceWorld.TouchResidencyClass(surfaceVisible, SparseResidencyClass::Visible, 100),
        "surface priority touch visible");
    Check(surfaceWorld.PumpGeneration(1, 100) == 1,
        "surface priority generates visible second");
    Check(surfaceWorld.PopNextUpload(&packet), "surface priority pops visible upload");
    Check(packet.coord == surfaceVisible, "surface priority visible queues extraction second");
    Check(surfaceWorld.CompleteUpload(packet), "surface priority completes visible upload");
    Check(surfaceWorld.GetStats().surfaceExtractionQueuedBricks == 2,
        "surface priority has two queued extraction bricks");
    Check(surfaceWorld.PumpSurfaceExtraction(1, 100) == 1,
        "surface priority obeys one-brick extraction budget");
    Check(surfaceWorld.GetSurfaceCache().FindFaces(surfaceVisible) != nullptr,
        "surface priority extracts visible brick before older speculative brick");
    Check(surfaceWorld.GetSurfaceCache().FindFaces(surfaceSpeculative) == nullptr,
        "surface priority leaves speculative brick pending under one-brick budget");
    Check(surfaceWorld.PumpSurfaceExtraction(1, 101) == 1,
        "surface priority extracts remaining speculative brick next");
    Check(surfaceWorld.GetSurfaceCache().FindFaces(surfaceSpeculative) != nullptr,
        "surface priority eventually extracts speculative brick");

    SparseVoxelWorld surfaceValueWorld;
    Check(surfaceValueWorld.Initialize({8, 32, 12345u}), "surface value world initialize");
    const BrickCoord farSurface{2000, 1000, 0};
    const BrickCoord nearSurface{0, 1000, 0};
    surfaceValueWorld.SetEditedVoxel(
        farSurface.x * SPARSE_BRICK_SIZE,
        farSurface.y * SPARSE_BRICK_SIZE,
        farSurface.z * SPARSE_BRICK_SIZE,
        priorityStone);
    surfaceValueWorld.SetEditedVoxel(
        nearSurface.x * SPARSE_BRICK_SIZE,
        nearSurface.y * SPARSE_BRICK_SIZE,
        nearSurface.z * SPARSE_BRICK_SIZE,
        priorityStone);
    Check(surfaceValueWorld.RequestBrickDetailed(farSurface, false) == SparseBrickRequestResult::Allocated,
        "surface value request far visible");
    Check(surfaceValueWorld.TouchResidencyClass(farSurface, SparseResidencyClass::Visible, 100),
        "surface value touch far visible newer");
    Check(surfaceValueWorld.RequestBrickDetailed(nearSurface, false) == SparseBrickRequestResult::Allocated,
        "surface value request near visible");
    Check(surfaceValueWorld.TouchResidencyClass(nearSurface, SparseResidencyClass::Visible, 10),
        "surface value touch near visible older");
    Check(surfaceValueWorld.PumpGeneration(2, 100) == 2,
        "surface value generates candidates");
    Check(surfaceValueWorld.PopNextUpload(&packet, 100), "surface value pops first upload");
    Check(surfaceValueWorld.CompleteUpload(packet), "surface value completes first upload");
    Check(surfaceValueWorld.PopNextUpload(&packet, 100), "surface value pops second upload");
    Check(surfaceValueWorld.CompleteUpload(packet), "surface value completes second upload");
    Check(surfaceValueWorld.PumpSurfaceExtractionAround(1, BrickCoord{0, 1000, 0}, 100) == 1,
        "surface value extracts one focused brick");
    Check(surfaceValueWorld.GetSurfaceCache().FindFaces(nearSurface) != nullptr,
        "surface value extracts nearer visible surface before newer far surface");
    Check(surfaceValueWorld.GetSurfaceCache().FindFaces(farSurface) == nullptr,
        "surface value leaves far visible surface queued under one-brick budget");

    SparseVoxelWorld surfaceUpgradeWorld;
    Check(surfaceUpgradeWorld.Initialize({8, 32, 12345u}), "surface class upgrade world initialize");
    const BrickCoord surfaceUpgrade{42, 1000, 0};
    surfaceUpgradeWorld.SetEditedVoxel(
        surfaceUpgrade.x * SPARSE_BRICK_SIZE,
        surfaceUpgrade.y * SPARSE_BRICK_SIZE,
        surfaceUpgrade.z * SPARSE_BRICK_SIZE,
        priorityStone);
    Check(surfaceUpgradeWorld.RequestBrickDetailed(surfaceUpgrade, false) == SparseBrickRequestResult::Allocated,
        "surface class upgrade request");
    Check(surfaceUpgradeWorld.TouchResidencyClass(surfaceUpgrade, SparseResidencyClass::Speculative, 1),
        "surface class upgrade initially speculative");
    Check(surfaceUpgradeWorld.PumpGeneration(1, 1) == 1,
        "surface class upgrade generates upload");
    Check(surfaceUpgradeWorld.PopNextUpload(&packet, 1),
        "surface class upgrade pops upload");
    Check(surfaceUpgradeWorld.CompleteUpload(packet),
        "surface class upgrade queues pending surface");
    Check(surfaceUpgradeWorld.TouchResidencyClass(surfaceUpgrade, SparseResidencyClass::Collision, 20),
        "surface class upgrade retouches pending surface as collision");
    Check(surfaceUpgradeWorld.SurfaceClassQueueSize() == 1,
        "surface class upgrade removes stale class aliases before requeue");
    Check(surfaceUpgradeWorld.GetStats().surfaceQueuedCollisionBricks == 1,
        "surface class upgrade cached stats count collision pending surface");
    Check(surfaceUpgradeWorld.PumpSurfaceExtractionAround(1, surfaceUpgrade, 20) == 1,
        "surface class upgrade extracts retouched collision surface through class bucket");
    Check(surfaceUpgradeWorld.GetSurfaceCache().FindFaces(surfaceUpgrade) != nullptr,
        "surface class upgrade produced surface after retouch");

    SparseVoxelWorld processedClassWorld;
    Check(processedClassWorld.Initialize({4, 32, 12345u}), "processed class world initialize");
    const BrickCoord processedCollision{0, -64, 0};
    Check(processedClassWorld.RequestBrick(processedCollision), "processed class request collision");
    Check(processedClassWorld.MarkResidencyClass(processedCollision, SparseResidencyClass::Collision),
        "processed class marks collision");
    Check(processedClassWorld.PumpGeneration(1, 10) == 1,
        "processed class generates collision");
    Check(processedClassWorld.GetStats().generatedCollisionBricksLastFrame == 1,
        "processed class counts generated collision brick");
    Check(processedClassWorld.PopNextUpload(&packet), "processed class pops collision upload");
    Check(processedClassWorld.GetStats().uploadedCollisionBricksLastFrame == 1,
        "processed class counts uploaded collision brick");
    Check(processedClassWorld.CompleteUpload(packet), "processed class completes collision upload");
    Check(processedClassWorld.PumpSurfaceExtraction(1, 11) == 1,
        "processed class extracts collision surface");
    Check(processedClassWorld.GetStats().surfaceCollisionBricksExtractedLastFrame == 1,
        "processed class counts extracted collision surface");
}

void TestSparseBrickRequestPlanner() {
    SparseBrickRequestPlanner planner({
        2,
        1,
        2,
        64
    });

    const BrickCoord center{10, -3, 20};
    std::vector<SparseBrickRequest> requests = planner.Plan(center, 1, 0, 0);
    Check(!requests.empty(), "planner returns requests");
    Check(requests.size() <= 64, "planner respects max request count");

    std::unordered_set<BrickCoord, BrickCoordHash> unique;
    for (const auto& request : requests) {
        Check(unique.insert(request.coord).second, "planner emits no duplicate brick requests");
    }

    Check(requests.front().coord == center || requests.front().coord.x >= center.x,
        "planner prioritizes center/forward bricks first");

    bool hasVerticalNeighbor = false;
    bool hasForwardPrefetch = false;
    for (const auto& request : requests) {
        if (request.coord == BrickCoord{center.x, center.y + 1, center.z}) {
            hasVerticalNeighbor = true;
        }
        if (request.coord.x > center.x + 2) {
            hasForwardPrefetch = true;
        }
    }
    Check(hasVerticalNeighbor, "planner includes vertical window");
    Check(hasForwardPrefetch, "planner includes forward prefetch bricks");

    SparseBrickRequestPlanner tightPlanner({1, 0, 0, 1});
    std::vector<SparseBrickRequest> tight = tightPlanner.Plan(center, 0, 0, 0);
    Check(tight.size() == 1, "planner can hard cap to one request");
    Check(tight.front().coord == center, "single-request cap keeps center brick");

    SparseViewConeConfig cone{};
    cone.originX = 0.0f;
    cone.originY = 0.0f;
    cone.originZ = 0.0f;
    cone.forwardZ = 1.0f;
    cone.rightX = 1.0f;
    cone.upY = 1.0f;
    cone.verticalFovRadians = 1.04719755f;
    cone.aspectRatio = 1.0f;
    cone.maxDistance = 64.0f;
    cone.stepDistance = 16.0f;
    cone.rayGrid = 3;
    cone.maxRequests = 128;

    const std::vector<SparseBrickRequest> coneRequests = planner.PlanViewCone(cone);
    Check(!coneRequests.empty(), "view-cone planner returns requests");
    std::unordered_set<BrickCoord, BrickCoordHash> coneUnique;
    bool hasOrigin = false;
    bool hasForwardBrick = false;
    bool hasLateralBrick = false;
    for (const auto& request : coneRequests) {
        Check(coneUnique.insert(request.coord).second, "view-cone planner emits no duplicates");
        hasOrigin = hasOrigin || request.coord == BrickCoord{0, 0, 0};
        hasForwardBrick = hasForwardBrick || request.coord == BrickCoord{0, 0, 1};
        hasLateralBrick = hasLateralBrick || request.coord.x != 0 || request.coord.y != 0;
    }
    Check(hasOrigin, "view-cone planner includes camera brick");
    Check(hasForwardBrick, "view-cone planner includes forward visible bricks");
    Check(hasLateralBrick, "view-cone planner samples across the view frustum");

    SparseViewConeConfig obliqueCone{};
    obliqueCone.originX = 1.0f;
    obliqueCone.originY = 1.0f;
    obliqueCone.originZ = 1.0f;
    obliqueCone.forwardX = 1.0f;
    obliqueCone.forwardZ = 1.0f;
    obliqueCone.rightX = 1.0f;
    obliqueCone.upY = 1.0f;
    obliqueCone.verticalFovRadians = 0.1f;
    obliqueCone.aspectRatio = 1.0f;
    obliqueCone.maxDistance = 96.0f;
    obliqueCone.stepDistance = 64.0f;
    obliqueCone.rayGrid = 1;
    obliqueCone.maxRequests = 64;
    const std::vector<SparseBrickRequest> obliqueRequests = planner.PlanViewCone(obliqueCone);
    bool hasObliqueNear = false;
    bool hasObliqueMid = false;
    bool hasObliqueFar = false;
    for (const auto& request : obliqueRequests) {
        hasObliqueNear = hasObliqueNear || request.coord == BrickCoord{0, 0, 0};
        hasObliqueMid = hasObliqueMid || request.coord == BrickCoord{1, 0, 1};
        hasObliqueFar = hasObliqueFar || request.coord == BrickCoord{4, 0, 4};
    }
    Check(hasObliqueNear && hasObliqueMid && hasObliqueFar,
        "view-cone planner uses brick DDA so oblique visible rays do not skip crossed bricks");

    SparseViewConeConfig verticalObliqueCone = obliqueCone;
    verticalObliqueCone.forwardX = 1.0f;
    verticalObliqueCone.forwardY = 1.0f;
    verticalObliqueCone.forwardZ = 1.0f;
    const std::vector<SparseBrickRequest> verticalObliqueRequests =
        planner.PlanViewCone(verticalObliqueCone);
    bool hasVerticalOblique = false;
    for (const auto& request : verticalObliqueRequests) {
        hasVerticalOblique =
            hasVerticalOblique || request.coord == BrickCoord{2, 2, 2};
    }
    Check(hasVerticalOblique,
        "view-cone planner DDA covers vertical brick crossings for steep view rays");

    SparseViewConeConfig coveredCone = cone;
    coveredCone.maxDistance = 1.0f;
    coveredCone.rayGrid = 1;
    coveredCone.coverageRadiusXz = 1;
    coveredCone.coverageRadiusY = 1;
    coveredCone.maxRequests = 64;
    const std::vector<SparseBrickRequest> coveredConeRequests = planner.PlanViewCone(coveredCone);
    bool coveredConeHasNeighbor = false;
    bool coveredConeHasVertical = false;
    for (const auto& request : coveredConeRequests) {
        coveredConeHasNeighbor = coveredConeHasNeighbor || request.coord == BrickCoord{1, 0, 0};
        coveredConeHasVertical = coveredConeHasVertical || request.coord == BrickCoord{0, 1, 0};
    }
    Check(coveredConeHasNeighbor,
        "view-cone planner can dilate samples to cover neighboring visible bricks");
    Check(coveredConeHasVertical,
        "view-cone planner can dilate samples vertically for steep view changes");

    SparseViewConeConfig depthFirstCoveredCone = cone;
    depthFirstCoveredCone.rayGrid = 1;
    depthFirstCoveredCone.coverageRadiusXz = 1;
    depthFirstCoveredCone.coverageRadiusY = 1;
    depthFirstCoveredCone.maxRequests = 4;
    const std::vector<SparseBrickRequest> depthFirstCoveredRequests =
        planner.PlanViewCone(depthFirstCoveredCone);
    bool coveredConeKeepsDepth = false;
    for (const auto& request : depthFirstCoveredRequests) {
        coveredConeKeepsDepth = coveredConeKeepsDepth || request.coord == BrickCoord{0, 0, 1};
    }
    Check(coveredConeKeepsDepth,
        "view-cone coverage dilation preserves forward depth before filling the shell");

    cone.maxRequests = 4;
    const std::vector<SparseBrickRequest> cappedCone = planner.PlanViewCone(cone);
    Check(cappedCone.size() == 4, "view-cone planner respects max request cap");
    Check(cappedCone.front().coord == BrickCoord{0, 0, 0},
        "view-cone planner keeps center ray origin as highest-priority request");

    SparseViewConeConfig malformedCone = cone;
    malformedCone.originX = std::numeric_limits<float>::quiet_NaN();
    malformedCone.maxRequests = std::numeric_limits<uint32_t>::max();
    Check(planner.PlanViewCone(malformedCone).empty(),
        "view-cone planner rejects non-finite origin input");

    SparseViewConeConfig clampedCone = cone;
    clampedCone.verticalFovRadians = std::numeric_limits<float>::infinity();
    clampedCone.aspectRatio = -std::numeric_limits<float>::infinity();
    clampedCone.maxDistance = std::numeric_limits<float>::max();
    clampedCone.stepDistance = std::numeric_limits<float>::min();
    clampedCone.coverageRadiusXz = std::numeric_limits<uint32_t>::max();
    clampedCone.coverageRadiusY = std::numeric_limits<uint32_t>::max();
    clampedCone.maxRequests = std::numeric_limits<uint32_t>::max();
    const std::vector<SparseBrickRequest> clampedConeRequests = planner.PlanViewCone(clampedCone);
    Check(!clampedConeRequests.empty(),
        "view-cone planner clamps malformed optional view parameters");
    Check(clampedConeRequests.size() <= 4096,
        "view-cone planner caps malformed max request counts");
    std::unordered_set<BrickCoord, BrickCoordHash> clampedConeUnique;
    for (const auto& request : clampedConeRequests) {
        Check(clampedConeUnique.insert(request.coord).second,
            "view-cone planner keeps malformed-clamped requests unique");
    }

    SparseViewConeConfig boundaryCone = cone;
    boundaryCone.originX = static_cast<float>(std::numeric_limits<int32_t>::max());
    boundaryCone.originY = 0.0f;
    boundaryCone.originZ = static_cast<float>(std::numeric_limits<int32_t>::max());
    boundaryCone.forwardX = 1.0f;
    boundaryCone.forwardY = 0.0f;
    boundaryCone.forwardZ = 1.0f;
    boundaryCone.rayGrid = 1;
    boundaryCone.maxDistance = 8192.0f;
    boundaryCone.maxRequests = 32;
    Check(planner.PlanViewCone(boundaryCone).empty(),
        "view-cone planner rejects positive signed-coordinate overflow boundary origins");

    SparseHierarchicalRequestConfig hierarchy{};
    hierarchy.center = BrickCoord{0, 0, 0};
    hierarchy.cameraX = 0.0f;
    hierarchy.cameraY = 0.0f;
    hierarchy.cameraZ = 0.0f;
    hierarchy.velocityX = 48.0f;
    hierarchy.velocityZ = 0.0f;
    hierarchy.collisionBodyHeight = 6.0f;
    hierarchy.collisionBodyRadius = 0.75f;
    hierarchy.collisionStepHeight = 2.5f;
    hierarchy.collisionSupportDrop = 4.0f;
    hierarchy.forwardZ = 1.0f;
    hierarchy.rightX = 1.0f;
    hierarchy.upY = 1.0f;
    hierarchy.visibleDistance = 64.0f;
    hierarchy.speculativeDistance = 128.0f;
    hierarchy.stepDistance = 16.0f;
    hierarchy.collisionRadiusXz = 0;
    hierarchy.collisionRadiusY = 0;
    hierarchy.collisionPredictionBricks = 2;
    hierarchy.collisionMaxIntentSamples = 8;
    hierarchy.nearVisibleRadiusXz = 2;
    hierarchy.nearVisibleRadiusY = 1;
    hierarchy.maxNearVisibleRequests = 16;
    hierarchy.brushIntentValid = true;
    hierarchy.brushStartX = 96.0f;
    hierarchy.brushStartY = 0.0f;
    hierarchy.brushStartZ = 0.0f;
    hierarchy.brushEndX = 144.0f;
    hierarchy.brushEndY = 0.0f;
    hierarchy.brushEndZ = 0.0f;
    hierarchy.brushRadius = 4.0f;
    hierarchy.maxBrushCollisionRequests = 16;
    hierarchy.visibleRayGrid = 1;
    hierarchy.speculativeRayGrid = 1;
    hierarchy.maxCollisionRequests = 64;
    hierarchy.maxVisibleRequests = 16;
    hierarchy.maxSpeculativeRequests = 16;
    hierarchy.maxRequests = 64;

    const std::vector<SparseBrickRequest> hierarchical = planner.PlanHierarchical(hierarchy);
    Check(!hierarchical.empty(), "hierarchical planner returns requests");
    Check(hierarchical.front().residencyClass == SparseResidencyClass::Collision,
        "hierarchical planner prioritizes collision residency first");
    bool hasPredictedCollision = false;
    bool hasBodySupportCollision = false;
    bool hasBrushIntentCollision = false;
    bool hasVisible = false;
    bool hasLocalSideVisible = false;
    bool hasMotionVisible = false;
    bool hasSpeculative = false;
    std::unordered_set<BrickCoord, BrickCoordHash> hierarchicalUnique;
    for (const SparseBrickRequest& request : hierarchical) {
        Check(hierarchicalUnique.insert(request.coord).second,
            "hierarchical planner emits no duplicate brick requests");
        hasPredictedCollision =
            hasPredictedCollision ||
            (request.coord.x > hierarchy.center.x &&
             request.residencyClass == SparseResidencyClass::Collision &&
             request.urgent);
        hasBodySupportCollision =
            hasBodySupportCollision ||
            (request.coord.y < hierarchy.center.y &&
             request.residencyClass == SparseResidencyClass::Collision &&
             request.urgent);
        hasBrushIntentCollision =
            hasBrushIntentCollision ||
            (request.coord.x >= 6 &&
             request.residencyClass == SparseResidencyClass::Collision &&
             request.urgent);
        hasVisible = hasVisible || request.residencyClass == SparseResidencyClass::Visible;
        hasLocalSideVisible =
            hasLocalSideVisible ||
            (request.coord == BrickCoord{0, 1, 0} &&
             request.residencyClass == SparseResidencyClass::Visible &&
             request.urgent);
        hasMotionVisible =
            hasMotionVisible ||
            (request.coord.x >= 3 &&
             (request.residencyClass == SparseResidencyClass::Visible ||
              request.residencyClass == SparseResidencyClass::Collision) &&
             request.urgent);
        hasSpeculative = hasSpeculative || request.residencyClass == SparseResidencyClass::Speculative;
    }
    Check(hasPredictedCollision, "hierarchical planner includes predicted collision shell");
    Check(hasBodySupportCollision, "hierarchical planner includes body/support collision bricks");
    Check(hasBrushIntentCollision, "hierarchical planner includes active brush collision bricks");
    Check(hasVisible, "hierarchical planner includes visible requests");
    Check(hasLocalSideVisible, "hierarchical planner protects local visible shell independent of view direction");
    Check(hasMotionVisible, "hierarchical planner protects a renderable movement corridor independent of view direction");
    Check(hasSpeculative, "hierarchical planner includes speculative requests");

    SparseHierarchicalRequestConfig visibleDilationHierarchy = hierarchy;
    visibleDilationHierarchy.maxCollisionRequests = 0;
    visibleDilationHierarchy.maxNearVisibleRequests = 0;
    visibleDilationHierarchy.maxMotionVisibleRequests = 0;
    visibleDilationHierarchy.maxSpeculativeRequests = 0;
    visibleDilationHierarchy.visibleRayGrid = 1;
    visibleDilationHierarchy.maxVisibleRequests = 32;
    visibleDilationHierarchy.maxRequests = 32;
    const std::vector<SparseBrickRequest> visibleDilationRequests =
        planner.PlanHierarchical(visibleDilationHierarchy);
    bool hasDilatedVisibleNeighbor = false;
    for (const SparseBrickRequest& request : visibleDilationRequests) {
        hasDilatedVisibleNeighbor =
            hasDilatedVisibleNeighbor ||
            (request.coord == BrickCoord{1, 0, 0} &&
             request.residencyClass == SparseResidencyClass::Visible &&
             request.urgent);
    }
    Check(hasDilatedVisibleNeighbor,
        "hierarchical visible view lane dilates ray samples to reduce current-view holes");

    SparseHierarchicalRequestConfig fastHierarchy = hierarchy;
    fastHierarchy.brushIntentValid = false;
    fastHierarchy.velocityX = 320.0f;
    fastHierarchy.predictionSeconds = 0.5f;
    fastHierarchy.collisionMaxIntentSamples = 16;
    fastHierarchy.maxCollisionRequests = 64;
    fastHierarchy.maxVisibleRequests = 0;
    fastHierarchy.maxSpeculativeRequests = 0;
    fastHierarchy.maxRequests = 64;
    const std::vector<SparseBrickRequest> fastRequests = planner.PlanHierarchical(fastHierarchy);
    bool hasFastCorridorEnd = false;
    for (const SparseBrickRequest& request : fastRequests) {
        hasFastCorridorEnd =
            hasFastCorridorEnd ||
            (request.coord.x >= 8 &&
             request.residencyClass == SparseResidencyClass::Collision &&
             request.urgent);
    }
    Check(hasFastCorridorEnd, "hierarchical planner samples long fast-movement collision corridor");

    SparseHierarchicalRequestConfig sidewaysFastHierarchy = fastHierarchy;
    sidewaysFastHierarchy.velocityX = 320.0f;
    sidewaysFastHierarchy.velocityZ = 0.0f;
    sidewaysFastHierarchy.forwardX = 0.0f;
    sidewaysFastHierarchy.forwardZ = 1.0f;
    sidewaysFastHierarchy.maxCollisionRequests = 0;
    sidewaysFastHierarchy.maxVisibleRequests = 64;
    sidewaysFastHierarchy.maxSpeculativeRequests = 0;
    sidewaysFastHierarchy.maxNearVisibleRequests = 4;
    sidewaysFastHierarchy.motionVisibleMinSpeed = 64.0f;
    sidewaysFastHierarchy.maxMotionVisibleRequests = 48;
    sidewaysFastHierarchy.maxRequests = 64;
    const std::vector<SparseBrickRequest> sidewaysFastRequests =
        planner.PlanHierarchical(sidewaysFastHierarchy);
    bool hasSidewaysMotionVisible = false;
    for (const SparseBrickRequest& request : sidewaysFastRequests) {
        hasSidewaysMotionVisible =
            hasSidewaysMotionVisible ||
            (request.coord.x >= 8 &&
             request.coord.z == 0 &&
             request.residencyClass == SparseResidencyClass::Visible &&
             request.urgent);
    }
    Check(hasSidewaysMotionVisible,
        "hierarchical planner prefetches visible motion corridor when movement and view direction diverge");

    bool sidewaysMotionContinuous = true;
    for (int32_t x = 1; x <= 8; ++x) {
        bool hasX = false;
        for (const SparseBrickRequest& request : sidewaysFastRequests) {
            hasX =
                hasX ||
                (request.coord.x == x &&
                 request.coord.z == 0 &&
                 request.residencyClass == SparseResidencyClass::Visible);
        }
        sidewaysMotionContinuous = sidewaysMotionContinuous && hasX;
    }
    Check(sidewaysMotionContinuous,
        "hierarchical motion-visible lane uses DDA to avoid skipped fast-movement bricks");

    SparseHierarchicalRequestConfig recoveryHierarchy = hierarchy;
    recoveryHierarchy.brushIntentValid = false;
    recoveryHierarchy.velocityX = 0.0f;
    recoveryHierarchy.velocityZ = 0.0f;
    recoveryHierarchy.maxCollisionRequests = 0;
    recoveryHierarchy.maxNearVisibleRequests = 0;
    recoveryHierarchy.maxMotionVisibleRequests = 0;
    recoveryHierarchy.maxVisibleRequests = 0;
    recoveryHierarchy.maxSpeculativeRequests = 0;
    recoveryHierarchy.ownershipPressureLevel = 3;
    recoveryHierarchy.maxOwnershipRecoveryRequests = 24;
    recoveryHierarchy.maxRequests = 24;
    const std::vector<SparseBrickRequest> recoveryRequests =
        planner.PlanHierarchical(recoveryHierarchy);
    bool hasRecoveryVisible = false;
    bool hasRecoveryForward = false;
    bool recoveryAllUrgentVisible = !recoveryRequests.empty();
    for (const SparseBrickRequest& request : recoveryRequests) {
        hasRecoveryVisible =
            hasRecoveryVisible ||
            request.residencyClass == SparseResidencyClass::Visible;
        hasRecoveryForward =
            hasRecoveryForward ||
            (request.coord.z > recoveryHierarchy.center.z &&
             request.residencyClass == SparseResidencyClass::Visible &&
             request.urgent);
        recoveryAllUrgentVisible =
            recoveryAllUrgentVisible &&
            request.residencyClass == SparseResidencyClass::Visible &&
            request.urgent;
    }
    Check(hasRecoveryVisible,
        "hierarchical planner emits ownership-pressure recovery requests without normal visible budget");
    Check(hasRecoveryForward,
        "hierarchical planner targets ownership-pressure recovery into the current view");
    Check(recoveryAllUrgentVisible,
        "ownership-pressure recovery requests are protected visible work");

    SparseHierarchicalRequestConfig malformedHierarchy = hierarchy;
    malformedHierarchy.cameraX = std::numeric_limits<float>::infinity();
    malformedHierarchy.cameraY = std::numeric_limits<float>::quiet_NaN();
    malformedHierarchy.cameraZ = -std::numeric_limits<float>::infinity();
    malformedHierarchy.velocityX = std::numeric_limits<float>::infinity();
    malformedHierarchy.velocityY = std::numeric_limits<float>::quiet_NaN();
    malformedHierarchy.velocityZ = -std::numeric_limits<float>::infinity();
    malformedHierarchy.predictionSeconds = std::numeric_limits<float>::infinity();
    malformedHierarchy.visibleDistance = std::numeric_limits<float>::infinity();
    malformedHierarchy.speculativeDistance = std::numeric_limits<float>::quiet_NaN();
    malformedHierarchy.stepDistance = std::numeric_limits<float>::min();
    malformedHierarchy.verticalFovRadians = std::numeric_limits<float>::infinity();
    malformedHierarchy.aspectRatio = -std::numeric_limits<float>::infinity();
    malformedHierarchy.motionVisibleRadiusXz = std::numeric_limits<uint32_t>::max();
    malformedHierarchy.motionVisibleRadiusY = std::numeric_limits<uint32_t>::max();
    malformedHierarchy.maxMotionVisibleRequests = std::numeric_limits<uint32_t>::max();
    malformedHierarchy.maxVisibleRequests = std::numeric_limits<uint32_t>::max();
    malformedHierarchy.maxSpeculativeRequests = std::numeric_limits<uint32_t>::max();
    malformedHierarchy.maxRequests = 32;
    const std::vector<SparseBrickRequest> malformedHierarchyRequests =
        planner.PlanHierarchical(malformedHierarchy);
    Check(!malformedHierarchyRequests.empty(),
        "hierarchical planner sanitizes malformed camera/motion inputs");
    Check(malformedHierarchyRequests.size() <= malformedHierarchy.maxRequests,
        "hierarchical planner caps malformed request counts");
    std::unordered_set<BrickCoord, BrickCoordHash> malformedHierarchyUnique;
    for (const SparseBrickRequest& request : malformedHierarchyRequests) {
        Check(malformedHierarchyUnique.insert(request.coord).second,
            "hierarchical planner keeps malformed requests unique");
    }

    SparseHierarchicalRequestConfig boundaryHierarchy = hierarchy;
    boundaryHierarchy.center = BrickCoord{
        std::numeric_limits<int32_t>::max(),
        0,
        std::numeric_limits<int32_t>::max()};
    boundaryHierarchy.cameraX = static_cast<float>(std::numeric_limits<int32_t>::max());
    boundaryHierarchy.cameraY = 0.0f;
    boundaryHierarchy.cameraZ = static_cast<float>(std::numeric_limits<int32_t>::max());
    boundaryHierarchy.velocityX = 512.0f;
    boundaryHierarchy.velocityY = 0.0f;
    boundaryHierarchy.velocityZ = 512.0f;
    boundaryHierarchy.collisionRadiusXz = std::numeric_limits<uint32_t>::max();
    boundaryHierarchy.collisionRadiusY = std::numeric_limits<uint32_t>::max();
    boundaryHierarchy.nearVisibleRadiusXz = std::numeric_limits<uint32_t>::max();
    boundaryHierarchy.nearVisibleRadiusY = 0;
    boundaryHierarchy.maxNearVisibleRequests = 16;
    boundaryHierarchy.maxMotionVisibleRequests = 16;
    boundaryHierarchy.maxVisibleRequests = 16;
    boundaryHierarchy.maxSpeculativeRequests = 16;
    boundaryHierarchy.maxRequests = 32;
    const std::vector<SparseBrickRequest> boundaryHierarchyRequests =
        planner.PlanHierarchical(boundaryHierarchy);
    Check(boundaryHierarchyRequests.size() <= boundaryHierarchy.maxRequests,
        "hierarchical planner caps signed-boundary requests");
    const int32_t boundaryMin = std::numeric_limits<int32_t>::max() - 128;
    for (const SparseBrickRequest& request : boundaryHierarchyRequests) {
        Check(request.coord.x >= boundaryMin &&
              request.coord.z >= boundaryMin,
            "hierarchical planner does not wrap signed-boundary request coordinates");
    }

    SparseHierarchicalRequestConfig tightBrushHierarchy = fastHierarchy;
    tightBrushHierarchy.brushIntentValid = true;
    tightBrushHierarchy.brushStartX = 192.0f;
    tightBrushHierarchy.brushStartY = 0.0f;
    tightBrushHierarchy.brushStartZ = 0.0f;
    tightBrushHierarchy.brushEndX = 256.0f;
    tightBrushHierarchy.brushEndY = 0.0f;
    tightBrushHierarchy.brushEndZ = 0.0f;
    tightBrushHierarchy.brushRadius = 4.0f;
    tightBrushHierarchy.maxCollisionRequests = 16;
    tightBrushHierarchy.maxBrushCollisionRequests = 8;
    tightBrushHierarchy.reservedBrushCollisionRequests = 8;
    tightBrushHierarchy.maxRequests = 16;
    const std::vector<SparseBrickRequest> tightBrushRequests =
        planner.PlanHierarchical(tightBrushHierarchy);
    bool tightBrushHasBody = false;
    bool tightBrushHasFarBrush = false;
    for (const SparseBrickRequest& request : tightBrushRequests) {
        tightBrushHasBody =
            tightBrushHasBody ||
            (request.coord == BrickCoord{0, -1, 0} &&
             request.residencyClass == SparseResidencyClass::Collision);
        tightBrushHasFarBrush =
            tightBrushHasFarBrush ||
            (request.coord.x >= 12 &&
             request.residencyClass == SparseResidencyClass::Collision &&
             request.urgent);
    }
    Check(tightBrushHasBody, "hierarchical planner keeps body/support collision under tight brush budget");
    Check(tightBrushHasFarBrush, "hierarchical planner reserves collision residency for active brush path");

    SparseCollisionResidencyConfig brushLineCollision{};
    brushLineCollision.center = BrickCoord{0, 0, 0};
    brushLineCollision.cameraX = 0.0f;
    brushLineCollision.cameraY = 6.0f;
    brushLineCollision.cameraZ = 0.0f;
    brushLineCollision.bodyHeight = 6.0f;
    brushLineCollision.bodyRadius = 0.75f;
    brushLineCollision.stepHeight = 2.5f;
    brushLineCollision.supportDrop = 4.0f;
    brushLineCollision.brushIntentValid = true;
    brushLineCollision.brushStartX = 0.0f;
    brushLineCollision.brushStartY = 0.0f;
    brushLineCollision.brushStartZ = 0.0f;
    brushLineCollision.brushEndX = 160.0f;
    brushLineCollision.brushEndY = 0.0f;
    brushLineCollision.brushEndZ = 0.0f;
    brushLineCollision.brushRadius = 1.0f;
    brushLineCollision.maxBrushRequests = 48;
    brushLineCollision.reservedBrushRequests = 48;
    brushLineCollision.maxRequests = 64;
    const std::vector<SparseBrickRequest> brushLineRequests =
        planner.PlanCollisionResidency(brushLineCollision);
    bool brushLineContinuous = true;
    for (int32_t x = 1; x <= 8; ++x) {
        bool hasX = false;
        for (const SparseBrickRequest& request : brushLineRequests) {
            hasX = hasX || (request.coord.x == x && request.coord.y == 0 && request.coord.z == 0);
        }
        brushLineContinuous = brushLineContinuous && hasX;
    }
    Check(brushLineContinuous,
        "collision brush intent uses DDA to avoid skipped painted path residency");

    SparseCollisionResidencyConfig collisionOnly{};
    collisionOnly.center = BrickCoord{0, 0, 0};
    collisionOnly.cameraX = 0.0f;
    collisionOnly.cameraY = 0.0f;
    collisionOnly.cameraZ = 0.0f;
    collisionOnly.velocityX = 512.0f;
    collisionOnly.predictionSeconds = 0.5f;
    collisionOnly.bodyHeight = 6.0f;
    collisionOnly.bodyRadius = 0.75f;
    collisionOnly.stepHeight = 2.5f;
    collisionOnly.supportDrop = 4.0f;
    collisionOnly.brushIntentValid = true;
    collisionOnly.brushStartX = 224.0f;
    collisionOnly.brushEndX = 320.0f;
    collisionOnly.brushRadius = 4.0f;
    collisionOnly.predictionBricks = 6;
    collisionOnly.maxIntentSamples = 24;
    collisionOnly.maxBrushRequests = 8;
    collisionOnly.reservedBrushRequests = 8;
    collisionOnly.maxRequests = 16;
    const std::vector<SparseBrickRequest> collisionOnlyRequests =
        planner.PlanCollisionResidency(collisionOnly);
    bool collisionOnlyBody = false;
    bool collisionOnlyBrush = false;
    bool collisionOnlyAllClass = true;
    for (const SparseBrickRequest& request : collisionOnlyRequests) {
        collisionOnlyAllClass =
            collisionOnlyAllClass &&
            request.residencyClass == SparseResidencyClass::Collision &&
            request.urgent;
        collisionOnlyBody = collisionOnlyBody || request.coord.x <= 1;
        collisionOnlyBrush = collisionOnlyBrush || request.coord.x >= 14;
    }
    Check(collisionOnlyRequests.size() <= collisionOnly.maxRequests,
        "collision residency planner respects hard request cap");
    Check(collisionOnlyAllClass, "collision residency planner emits only urgent collision requests");
    Check(collisionOnlyBody, "collision residency planner protects immediate body/support shell");
    Check(collisionOnlyBrush, "collision residency planner protects active brush corridor independently");

    SparseCollisionResidencyConfig malformedCollision = brushLineCollision;
    malformedCollision.cameraX = std::numeric_limits<float>::infinity();
    malformedCollision.cameraY = std::numeric_limits<float>::quiet_NaN();
    malformedCollision.cameraZ = -std::numeric_limits<float>::infinity();
    malformedCollision.velocityX = std::numeric_limits<float>::infinity();
    malformedCollision.velocityY = std::numeric_limits<float>::quiet_NaN();
    malformedCollision.velocityZ = -std::numeric_limits<float>::infinity();
    malformedCollision.predictionSeconds = std::numeric_limits<float>::infinity();
    malformedCollision.bodyRadius = std::numeric_limits<float>::infinity();
    malformedCollision.bodyHeight = std::numeric_limits<float>::quiet_NaN();
    malformedCollision.stepHeight = std::numeric_limits<float>::infinity();
    malformedCollision.supportDrop = std::numeric_limits<float>::infinity();
    malformedCollision.brushStartX = std::numeric_limits<float>::quiet_NaN();
    malformedCollision.brushEndX = std::numeric_limits<float>::infinity();
    malformedCollision.brushRadius = std::numeric_limits<float>::infinity();
    malformedCollision.maxRequests = 16;
    const std::vector<SparseBrickRequest> malformedCollisionRequests =
        planner.PlanCollisionResidency(malformedCollision);
    Check(!malformedCollisionRequests.empty(),
        "collision residency planner falls back to body requests for malformed motion/brush input");
    Check(malformedCollisionRequests.size() <= malformedCollision.maxRequests,
        "collision residency planner keeps malformed input inside request cap");
    bool malformedAllNearCenter = true;
    for (const SparseBrickRequest& request : malformedCollisionRequests) {
        malformedAllNearCenter =
            malformedAllNearCenter &&
            std::abs(request.coord.x - malformedCollision.center.x) <= 2 &&
            std::abs(request.coord.z - malformedCollision.center.z) <= 2;
    }
    Check(malformedAllNearCenter,
        "collision residency planner sanitizes malformed input near the configured center");

    SparseStressRequestConfig stress{};
    stress.center = BrickCoord{4, -2, 7};
    stress.radiusXz = 4;
    stress.radiusY = 1;
    stress.maxRequests = 256;
    stress.cursor = 0;

    const std::vector<SparseBrickRequest> stressRequests = planner.PlanStressVolume(stress);
    Check(!stressRequests.empty(), "stress planner returns requests");
    Check(stressRequests.size() <= stress.maxRequests, "stress planner respects max request cap");
    std::unordered_set<BrickCoord, BrickCoordHash> stressUnique;
    bool stressHasCenter = false;
    bool stressHasVertical = false;
    bool stressHasSpeculative = false;
    for (const SparseBrickRequest& request : stressRequests) {
        Check(stressUnique.insert(request.coord).second, "stress planner emits no duplicates");
        stressHasCenter = stressHasCenter || request.coord == stress.center;
        stressHasVertical =
            stressHasVertical ||
            request.coord == BrickCoord{stress.center.x, stress.center.y + 1, stress.center.z};
        stressHasSpeculative =
            stressHasSpeculative ||
            request.residencyClass == SparseResidencyClass::Speculative;
    }
    Check(stressHasCenter, "stress planner includes collision core center");
    Check(stressHasVertical, "stress planner includes vertical stress neighbors");
    Check(stressHasSpeculative, "stress planner includes outer speculative pressure");
    Check(stressRequests.front().residencyClass == SparseResidencyClass::Collision,
        "stress planner keeps collision core highest priority");

    stress.maxRequests = 8;
    stress.cursor = 20;
    const std::vector<SparseBrickRequest> rotatedStressRequests = planner.PlanStressVolume(stress);
    Check(rotatedStressRequests.size() == 8, "stress planner supports small rotating batches");
    bool rotatedDiffers = rotatedStressRequests.size() != stressRequests.size();
    for (const SparseBrickRequest& request : rotatedStressRequests) {
        rotatedDiffers =
            rotatedDiffers ||
            stressUnique.find(request.coord) == stressUnique.end();
    }
    Check(rotatedDiffers, "stress planner cursor can sample a different pressure slice");

    SparseBrickRequestPlannerConfig extremePlannerConfig{};
    extremePlannerConfig.radiusXz = std::numeric_limits<uint32_t>::max();
    extremePlannerConfig.radiusY = std::numeric_limits<uint32_t>::max();
    extremePlannerConfig.forwardPrefetchBricks = std::numeric_limits<uint32_t>::max();
    extremePlannerConfig.maxRequests = 32;
    SparseBrickRequestPlanner extremePlanner(extremePlannerConfig);
    const std::vector<SparseBrickRequest> extremeBasicRequests = extremePlanner.Plan(
        {std::numeric_limits<int32_t>::max(), 0, std::numeric_limits<int32_t>::max()},
        1,
        0,
        1);
    Check(!extremeBasicRequests.empty() && extremeBasicRequests.size() <= extremePlannerConfig.maxRequests,
        "basic planner clamps extreme radii and request count");
    bool extremeBasicValid = true;
    std::unordered_set<BrickCoord, BrickCoordHash> extremeBasicUnique;
    for (const SparseBrickRequest& request : extremeBasicRequests) {
        extremeBasicValid =
            extremeBasicValid &&
            request.coord.x <= std::numeric_limits<int32_t>::max() &&
            request.coord.z <= std::numeric_limits<int32_t>::max() &&
            extremeBasicUnique.insert(request.coord).second;
    }
    Check(extremeBasicValid, "basic planner skips overflowing extreme-coordinate requests");

    SparseStressRequestConfig extremeStress{};
    extremeStress.center = {
        std::numeric_limits<int32_t>::max(),
        0,
        std::numeric_limits<int32_t>::min()
    };
    extremeStress.radiusXz = std::numeric_limits<uint32_t>::max();
    extremeStress.radiusY = std::numeric_limits<uint32_t>::max();
    extremeStress.maxRequests = 32;
    extremeStress.cursor = std::numeric_limits<uint32_t>::max();
    const std::vector<SparseBrickRequest> extremeStressRequests =
        planner.PlanStressVolume(extremeStress);
    Check(!extremeStressRequests.empty() && extremeStressRequests.size() <= extremeStress.maxRequests,
        "stress planner clamps extreme radii and request count");
    bool extremeStressValid = true;
    std::unordered_set<BrickCoord, BrickCoordHash> extremeStressUnique;
    for (const SparseBrickRequest& request : extremeStressRequests) {
        extremeStressValid =
            extremeStressValid &&
            request.coord.x <= std::numeric_limits<int32_t>::max() &&
            request.coord.z >= std::numeric_limits<int32_t>::min() &&
            extremeStressUnique.insert(request.coord).second;
    }
    Check(extremeStressValid, "stress planner skips overflowing extreme-coordinate requests");
}

void TestSparseRuntimeBudgetScheduler() {
    SparseFramePressureInput pressureInput{};
    pressureInput.smoothedFrameMs = 15.0f;
    pressureInput.predictedFrameMs = 18.0f;
    pressureInput.gpuFrameMs = 12.0f;
    pressureInput.gpuRaymarchMs = 10.0f;
    pressureInput.previousDebtMs = 2.0f;
    SparseFramePressure framePressure =
        SparseRuntimeBudgetScheduler::BuildFramePressure(pressureInput);
    Check(framePressure.schedulerPressureMs == 18.0f,
        "frame pressure uses max smoothed/predicted CPU pressure");
    Check(framePressure.combinedPressureMs == 18.0f,
        "frame pressure ignores lower GPU timing");
    Check(framePressure.debtMs > 2.36f && framePressure.debtMs < 2.39f,
        "frame pressure accumulates budget debt above frame target");
    Check(framePressure.budgetPressureMs > 20.36f && framePressure.budgetPressureMs < 20.40f,
        "frame pressure adds debt to scheduler pressure");

    pressureInput.smoothedFrameMs = 12.0f;
    pressureInput.predictedFrameMs = 13.0f;
    pressureInput.gpuFrameMs = 0.0f;
    pressureInput.gpuRaymarchMs = 0.0f;
    pressureInput.previousDebtMs = 4.0f;
    framePressure = SparseRuntimeBudgetScheduler::BuildFramePressure(pressureInput);
    Check(framePressure.debtMs < 4.0f && framePressure.debtMs > 3.5f,
        "frame pressure repays debt gradually under frame target");

    pressureInput.smoothedFrameMs = 12.0f;
    pressureInput.predictedFrameMs = 13.0f;
    pressureInput.gpuFrameMs = 14.0f;
    pressureInput.gpuRaymarchMs = 24.0f;
    pressureInput.previousDebtMs = 0.0f;
    framePressure = SparseRuntimeBudgetScheduler::BuildFramePressure(pressureInput);
    Check(framePressure.gpuPressureMs == 24.0f && framePressure.combinedPressureMs == 24.0f,
        "frame pressure allows GPU raymarch timing to dominate budget pressure");

    SparseFramePredictionInput predictionInput{};
    predictionInput.previousPredictedFrameMs = 16.0f;
    predictionInput.rawFrameMs = 10.0f;
    predictionInput.gpuFrameMs = 8.0f;
    predictionInput.chunkUpdateMs = 1.0f;
    predictionInput.physicsSubmitMs = 2.0f;
    predictionInput.brushSubmitMs = 3.0f;
    predictionInput.presentMs = 4.0f;
    predictionInput.historyWeight = 0.75f;
    SparseFramePrediction prediction =
        SparseRuntimeBudgetScheduler::BuildFramePrediction(predictionInput);
    Check(prediction.predictedWorkMs == 18.0f,
        "frame prediction sums GPU and CPU sidecar work when higher than raw frame time");
    Check(prediction.predictedFrameMs > 16.4f && prediction.predictedFrameMs < 16.6f,
        "frame prediction blends previous prediction toward measured work");

    predictionInput.rawFrameMs = 22.0f;
    predictionInput.gpuFrameMs = 1.0f;
    predictionInput.chunkUpdateMs = 1.0f;
    predictionInput.physicsSubmitMs = 1.0f;
    predictionInput.brushSubmitMs = 1.0f;
    predictionInput.presentMs = 1.0f;
    predictionInput.historyWeight = 0.0f;
    prediction = SparseRuntimeBudgetScheduler::BuildFramePrediction(predictionInput);
    Check(prediction.predictedWorkMs == 22.0f && prediction.predictedFrameMs == 22.0f,
        "frame prediction respects raw frame time and clamps blend weight");

    SparseOwnershipPressureInput ownershipInput{};
    ownershipInput.frameIndex = 8;
    ownershipInput.readyFrame = 12;
    ownershipInput.terrainPercent = 20;
    ownershipInput.missPercent = 50;
    ownershipInput.minTerrainPercent = 35;
    ownershipInput.maxMissPercent = 18;
    ownershipInput.maxUnsafeNearMissPercent = 6;
    ownershipInput.holdFrames = 30;
    SparseOwnershipPressure ownershipPressure =
        SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(!ownershipPressure.triggered && ownershipPressure.updatedCatchupFrames == 0,
        "ownership pressure waits until configured ready frame");
    ownershipInput.frameIndex = 12;
    ownershipInput.terrainPercent = 34;
    ownershipInput.missPercent = 19;
    ownershipPressure = SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(ownershipPressure.triggered && ownershipPressure.level == 1,
        "ownership pressure reports mild render ownership deficit");
    Check(ownershipPressure.updatedCatchupFrames == 30,
        "ownership pressure requests base catch-up hold for mild deficit");
    ownershipInput.currentCatchupFrames = 12;
    ownershipInput.terrainPercent = 8;
    ownershipInput.missPercent = 50;
    ownershipPressure = SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(ownershipPressure.triggered && ownershipPressure.level == 3,
        "ownership pressure escalates severe terrain/miss deficit");
    Check(ownershipPressure.updatedCatchupFrames == 60,
        "ownership pressure extends hold window for severe deficit");
    ownershipInput.currentCatchupFrames = 0;
    ownershipInput.terrainPercent = 80;
    ownershipInput.missPercent = 4;
    ownershipInput.unsafeNearMissPercent = 12;
    ownershipPressure = SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(ownershipPressure.triggered && ownershipPressure.level == 2 &&
          ownershipPressure.unsafeNearMissExcessPercent == 6,
        "ownership pressure treats near-owned sparse holes as protected residency pressure");
    ownershipInput.currentCatchupFrames = 9;
    ownershipInput.terrainPercent = 80;
    ownershipInput.missPercent = 4;
    ownershipInput.unsafeNearMissPercent = 0;
    ownershipPressure = SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(!ownershipPressure.triggered && ownershipPressure.active &&
          ownershipPressure.updatedCatchupFrames == 9,
        "ownership pressure preserves existing catch-up window after sample recovers");
    ownershipInput.currentCatchupFrames = 0;
    ownershipInput.terrainPercent = 92;
    ownershipInput.voxelTerrainPercent = 44;
    ownershipInput.minVoxelTerrainPercent = 68;
    ownershipInput.missPercent = 0;
    ownershipInput.unsafeNearMissPercent = 0;
    ownershipPressure = SparseRuntimeBudgetScheduler::BuildOwnershipPressure(ownershipInput);
    Check(ownershipPressure.triggered && ownershipPressure.level == 3 &&
          ownershipPressure.voxelTerrainDeficitPercent == 24,
        "ownership pressure escalates when proxy terrain hides low voxel-terrain coverage");

    SparseMissFeedbackPlanInput missPlanInput{};
    missPlanInput.enabled = true;
    missPlanInput.frameIndex = 5;
    missPlanInput.baseInterval = 4;
    missPlanInput.baseRayGrid = 5;
    missPlanInput.baseDistance = 256;
    missPlanInput.baseStride = 16;
    missPlanInput.maxRecords = 128;
    SparseMissFeedbackPlan missPlan =
        SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(!missPlan.dispatch && !missPlan.urgent,
        "miss feedback plan respects normal fixed interval when no residency pressure is present");
    missPlanInput.unsafeNearMissPercent = 1;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.urgent && missPlan.rayGrid >= 7 &&
          missPlan.distance >= 512 && missPlan.stride <= 8,
        "miss feedback plan immediately escalates sampling for unsafe near holes");
    missPlanInput.unsafeNearMissPercent = 0;
    missPlanInput.valleyAtmospherePercent = 10;
    missPlanInput.maxValleyAtmospherePercent = 8;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.urgent && missPlan.rayGrid >= 7 &&
          missPlan.distance >= 512 && missPlan.stride <= 8,
        "miss feedback plan treats excess valley atmosphere as visible residency pressure");
    missPlanInput.valleyAtmospherePercent = 18;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.rayGrid == 16 &&
          missPlan.distance >= 768 && missPlan.stride == 4,
        "miss feedback plan uses maximum sampling when valley atmosphere is far above target");
    missPlanInput.allowValleyAtmosphereFeedback = false;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(!missPlan.dispatch && !missPlan.urgent,
        "miss feedback plan does not spend valley-atmosphere feedback while generation is backlogged");
    missPlanInput.allowValleyAtmosphereFeedback = true;
    missPlanInput.valleyAtmospherePercent = 0;
    missPlanInput.unsafeNearMissPercent = 5;
    missPlanInput.ownershipPressureLevel = 3;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.rayGrid == 16 &&
          missPlan.distance >= 768 && missPlan.stride == 4,
        "miss feedback plan uses maximum safe sampling under severe ownership pressure");
    missPlanInput.unsafeNearMissPercent = 0;
    missPlanInput.ownershipPressureLevel = 0;
    missPlanInput.staleReadbackDrops = 1;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.urgent,
        "miss feedback plan immediately retries after stale feedback readback drops");
    missPlanInput.staleReadbackDrops = 0;
    missPlanInput.overflowLastRetire = true;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.urgent,
        "miss feedback plan immediately retries after feedback overflow");

    SparseRuntimeBudgetInput input{};
    input.lastRawFrameMs = 16.0f;
    input.combinedSchedulerPressureMs = 13.0f;
    input.hasQueueBacklog = true;
    input.uploadRingBytes = 8u * 1024u * 1024u;
    input.maxBrickPages = 4096u;
    SparseRuntimeBudgetDecision decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.scale > 1.0f, "runtime scheduler boosts backlog under headroom");
    Check(decision.backgroundScale == decision.scale && decision.protectedScale == decision.scale,
        "runtime scheduler keeps matching scales without protected backlog");
    Check(decision.pressureClass == SparseRuntimePressureClass::BacklogHeadroom,
        "runtime scheduler reports backlog headroom class");

    input.hasQueueBacklog = false;
    input.combinedSchedulerPressureMs = 18.0f;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.scale < 1.0f && decision.scale > 0.55f,
        "runtime scheduler reduces moderate pressure");
    Check(decision.pressureClass == SparseRuntimePressureClass::Moderate,
        "runtime scheduler reports moderate pressure class");

    input.combinedSchedulerPressureMs = 22.0f;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.scale == 0.35f, "runtime scheduler clamps severe frame pressure");
    Check(decision.pressureClass == SparseRuntimePressureClass::Severe,
        "runtime scheduler reports severe pressure class");

    input.combinedSchedulerPressureMs = 13.0f;
    input.uploadRingOverflow = true;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.scale == 0.35f, "runtime scheduler treats upload overflow as severe");

    input.uploadRingOverflow = false;
    input.combinedSchedulerPressureMs = 22.0f;
    input.urgentQueuedBricks = 12;
    input.speculativeQueuedBricks = 80;
    input.visibleQueuedBricks = 4;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.hasProtectedBacklog, "runtime scheduler detects protected sparse backlog");
    Check(decision.protectedScale > decision.scale,
        "runtime scheduler protects collision/edit work under severe pressure");
    Check(decision.backgroundScale < decision.scale,
        "runtime scheduler suppresses background work under protected severe pressure");
    Check(decision.trimSpeculativeFirst,
        "runtime scheduler flags speculative work as first trim target");

    input.combinedSchedulerPressureMs = 18.0f;
    input.physicsHotCandidateBricks = 3;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.protectedScale >= 1.15f,
        "runtime scheduler keeps hot physics/collision work moving under moderate pressure");
    input.physicsHotCandidateBricks = 0;
    input.urgentQueuedBricks = 0;
    input.visibleQueuedBricks = 6;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.hasProtectedBacklog && decision.protectedScale >= 1.15f,
        "runtime scheduler treats visible bricks as protected render continuity work");
    input.visibleQueuedBricks = 0;
    input.visibleMissPressure = true;
    input.ownershipPressureLevel = 3;
    input.combinedSchedulerPressureMs = 22.0f;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.hasProtectedBacklog && decision.protectedScale >= 1.75f,
        "runtime scheduler escalates protected scale for severe ownership pressure");
    Check(decision.backgroundScale <= 0.15f && decision.trimSpeculativeFirst,
        "runtime scheduler strongly diverts budget from background lanes under severe ownership pressure");
    input.visibleMissPressure = false;
    input.ownershipPressureLevel = 0;
    input.combinedSchedulerPressureMs = 22.0f;
    input.pagePublishReadyQueued = 5;
    input.pagePublishMaxReadyFrameLag = 5;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.hasProtectedBacklog && decision.protectedScale >= 1.55f,
        "runtime scheduler treats lagged ready page publishes as protected visibility work");
    Check(decision.backgroundScale <= 0.45f && decision.trimSpeculativeFirst,
        "runtime scheduler diverts hard-pressure budget away from speculative work for ready publishes");
    input.pagePublishReadyQueued = 0;
    input.pagePublishMaxReadyFrameLag = 0;
    input.pagePublishWaitingFence = 12;
    input.combinedSchedulerPressureMs = 13.0f;
    decision = SparseRuntimeBudgetScheduler::Evaluate(input);
    Check(decision.trimSpeculativeFirst && decision.backgroundScale <= 0.70f,
        "runtime scheduler throttles speculative work when page publishes are fence-gated");
    input.pagePublishWaitingFence = 0;

    Check(SparseRuntimeBudgetScheduler::ScaleBudget(16, 0.35f, 1u) == 6,
        "runtime scheduler scales budget with rounding");
    Check(SparseRuntimeBudgetScheduler::ScaleBudget(1, 0.01f, 1u) == 1,
        "runtime scheduler preserves nonzero minimum");
    Check(SparseRuntimeBudgetScheduler::ScaleBudget(0, 10.0f, 1u) == 0,
        "runtime scheduler keeps zero budget at zero");
    Check(SparseRuntimeBudgetScheduler::ScaleBudget(UINT32_MAX, 2.0f, 1u) == UINT32_MAX,
        "runtime scheduler saturates scaled budgets instead of wrapping");

    SparseRuntimeBudgetDecision headroom{};
    headroom.backgroundScale = 1.35f;
    headroom.protectedScale = 1.35f;
    headroom.pressureClass = SparseRuntimePressureClass::BacklogHeadroom;
    uint32_t processingBudget = SparseRuntimeBudgetScheduler::BuildProcessingBudget(
        4,
        512,
        false,
        headroom);
    Check(processingBudget > 5 && processingBudget <= 16,
        "processing scheduler adds bounded catch-up under backlog headroom");

    SparseRuntimeBudgetDecision pressure{};
    pressure.backgroundScale = 0.35f;
    pressure.protectedScale = 1.0f;
    pressure.pressureClass = SparseRuntimePressureClass::Severe;
    processingBudget = SparseRuntimeBudgetScheduler::BuildProcessingBudget(
        16,
        1024,
        false,
        pressure);
    Check(processingBudget == SparseRuntimeBudgetScheduler::ScaleBudget(16, 0.35f, 1u),
        "processing scheduler does not add background catch-up under severe pressure");
    processingBudget = SparseRuntimeBudgetScheduler::BuildProcessingBudget(
        16,
        1024,
        true,
        pressure);
    Check(processingBudget == SparseRuntimeBudgetScheduler::ScaleBudget(16, 1.0f, 1u),
        "processing scheduler applies protected scale under severe pressure");
    uint32_t editedCatchup = SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
        processingBudget,
        200,
        pressure,
        128);
    Check(editedCatchup == 32,
        "edited catch-up lane allows bounded protected burst under severe pressure");
    editedCatchup = SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
        8,
        200,
        headroom,
        128);
    Check(editedCatchup == 64,
        "edited catch-up lane expands further under backlog headroom");
    editedCatchup = SparseRuntimeBudgetScheduler::BuildEditedCatchupBudget(
        8,
        0,
        headroom,
        128);
    Check(editedCatchup == 8,
        "edited catch-up lane preserves budget without edited backlog");

    SparsePhysicsBudgetDecision physicsBudget =
        SparseRuntimeBudgetScheduler::BuildPhysicsBudgets(
            8,
            256,
            24,
            3,
            pressure);
    Check(physicsBudget.protectedBacklog,
        "physics scheduler marks hot sparse physics as protected backlog");
    Check(physicsBudget.brickBudget == 8,
        "physics scheduler keeps protected hot physics moving under severe pressure");
    Check(physicsBudget.moveBudget == 256,
        "physics scheduler keeps protected move budget under severe pressure");

    SparseRuntimeBudgetDecision backgroundPressure{};
    backgroundPressure.backgroundScale = 0.35f;
    backgroundPressure.protectedScale = 1.0f;
    backgroundPressure.pressureClass = SparseRuntimePressureClass::Severe;
    physicsBudget = SparseRuntimeBudgetScheduler::BuildPhysicsBudgets(
        8,
        256,
        24,
        0,
        backgroundPressure);
    Check(!physicsBudget.protectedBacklog,
        "physics scheduler treats warm-only sparse physics as background work");
    Check(physicsBudget.brickBudget < 8 && physicsBudget.brickBudget >= 1,
        "physics scheduler throttles warm sparse physics under pressure");
    Check(physicsBudget.moveBudget < 256 && physicsBudget.moveBudget >= 16,
        "physics scheduler throttles warm sparse physics moves under pressure");

    SparsePhysicsBudgetDecision headroomPhysicsBudget =
        SparseRuntimeBudgetScheduler::BuildPhysicsBudgets(
            8,
            256,
            96,
            6,
            headroom);
    Check(headroomPhysicsBudget.brickBudget > 8 && headroomPhysicsBudget.brickBudget <= 32,
        "physics scheduler adds bounded brick catch-up under headroom");
    Check(headroomPhysicsBudget.moveBudget > 256 && headroomPhysicsBudget.moveBudget <= 1024,
        "physics scheduler adds bounded move catch-up under headroom");

    SparseBackgroundRenderBudgetInput backgroundInput{};
    backgroundInput.combinedPressureMs = 14.0f;
    backgroundInput.gpuRaymarchMs = 5.0f;
    backgroundInput.previousRaymarchScale = 1.0f;
    backgroundInput.previousRenderQuality = 1.0f;
    backgroundInput.midHeightCoverage = 1.0f;
    backgroundInput.midVoxelCoverage = 1.0f;
    backgroundInput.farSvoReady = true;
    SparseBackgroundRenderBudgetDecision backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 0 &&
          backgroundBudget.raymarchScale > 0.99f &&
          backgroundBudget.renderQuality > 0.99f,
        "background renderer keeps full quality under frame headroom");

    backgroundInput.combinedPressureMs = 23.0f;
    backgroundInput.gpuRaymarchMs = 18.0f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 3 &&
          backgroundBudget.raymarchScale < 0.90f &&
          backgroundBudget.renderQuality < 0.90f,
        "background renderer reacts to severe GPU/background pressure");

    backgroundInput.gpuRaymarchMs = 5.0f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 0 &&
          backgroundBudget.renderQuality > 0.99f,
        "background renderer does not lower shader quality for unrelated CPU/vsync pressure");

    backgroundInput.combinedPressureMs = 14.0f;
    backgroundInput.gpuRaymarchMs = 6.2f;
    backgroundInput.previousRaymarchScale = 1.0f;
    backgroundInput.previousRenderQuality = 1.0f;
    backgroundInput.midHeightCoverage = 1.0f;
    backgroundInput.midVoxelCoverage = 1.0f;
    backgroundInput.midVoxelPixelShare = 0.34f;
    backgroundInput.farHeightPixelShare = 0.44f;
    backgroundInput.skyPixelShare = 0.10f;
    backgroundInput.farSvoReady = true;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier >= 1 &&
          backgroundBudget.renderQuality < 0.99f &&
          backgroundBudget.farFieldQuality < 1.0f,
        "background renderer uses ownership mix to downshift expensive far/mid pixel dominance");

    backgroundInput.midVoxelPixelShare = 0.04f;
    backgroundInput.farSvoPixelShare = 0.54f;
    backgroundInput.farHeightPixelShare = 0.02f;
    backgroundInput.skyPixelShare = 0.12f;
    backgroundInput.backgroundPixelShare = 1.0f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier >= 1 &&
          backgroundBudget.renderQuality <= 0.99f &&
          backgroundBudget.farFieldQuality < 1.0f,
        "background renderer reacts to far-SVO dominant GPU ray cost");

    backgroundInput.combinedPressureMs = 28.0f;
    backgroundInput.gpuRaymarchMs = 11.5f;
    backgroundInput.previousRenderQuality = 1.0f;
    backgroundInput.farSvoPixelShare = 0.84f;
    backgroundInput.backgroundPixelShare = 0.96f;
    backgroundInput.skyPixelShare = 0.13f;
    backgroundInput.farSvoReady = true;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 3 &&
          backgroundBudget.raymarchScale < 0.90f &&
          backgroundBudget.renderQuality >= 0.62f &&
          backgroundBudget.farFieldQuality >= 0.62f &&
          backgroundBudget.preserveFarFieldQuality,
        "background renderer preserves a medium far-SVO quality floor when far voxels own the view");

    backgroundInput.backgroundPixelShare = 0.20f;
    backgroundInput.midVoxelPixelShare = 0.34f;
    backgroundInput.farSvoPixelShare = 0.0f;
    backgroundInput.farHeightPixelShare = 0.44f;
    backgroundInput.combinedPressureMs = 14.0f;
    backgroundInput.gpuRaymarchMs = 6.2f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 0 &&
          backgroundBudget.renderQuality > 0.99f,
        "background renderer does not downshift when sparse surfaces already own most pixels");

    backgroundInput.backgroundPixelShare = 1.0f;
    backgroundInput.midVoxelPixelShare = 0.02f;
    backgroundInput.farHeightPixelShare = 0.06f;
    backgroundInput.skyPixelShare = 0.88f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.qualityTier == 0 &&
          backgroundBudget.renderQuality > 0.99f,
        "background renderer does not downshift mostly-sky frames from ownership mix alone");

    backgroundInput.midVoxelPixelShare = 0.0f;
    backgroundInput.farHeightPixelShare = 0.0f;
    backgroundInput.skyPixelShare = 0.0f;
    backgroundInput.midHeightCoverage = 0.0f;
    backgroundInput.midVoxelCoverage = 0.0f;
    backgroundInput.farSvoReady = false;
    backgroundInput.gpuRaymarchMs = 18.0f;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.renderQuality >= 0.74f,
        "background renderer keeps a quality floor while continuity layers warm up");

    backgroundInput.ownershipPressureLevel = 3;
    backgroundBudget =
        SparseRuntimeBudgetScheduler::BuildBackgroundRenderBudget(backgroundInput);
    Check(backgroundBudget.raymarchScale >= 0.80f &&
          backgroundBudget.renderQuality >= 0.92f &&
          backgroundBudget.farFieldQuality >= 0.94f &&
          backgroundBudget.preserveFarFieldQuality,
        "background renderer preserves terrain ownership quality during visible miss catch-up");

    SparseFarUploadBudgetInput farUploadInput{};
    farUploadInput.fullBudgetBytes = 2ull * 1024ull * 1024ull;
    farUploadInput.trickleBudgetBytes = 512ull * 1024ull;
    farUploadInput.totalBytes = 16ull * 1024ull * 1024ull;
    farUploadInput.uploadedBytes = 4ull * 1024ull * 1024ull;
    farUploadInput.combinedPressureMs = 13.0f;
    farUploadInput.predictedFrameMs = 13.5f;
    farUploadInput.lastUploadMs = 0.4f;
    farUploadInput.smoothedUploadMs = 0.5f;
    farUploadInput.targetUploadMs = 1.25f;
    farUploadInput.cheapFrame = true;
    farUploadInput.canTrickle = true;
    SparseFarUploadBudgetDecision farUploadBudget =
        SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == farUploadInput.fullBudgetBytes &&
          farUploadBudget.pressureTier == 0,
        "far upload scheduler uses full budget on cheap frames");

    farUploadInput.cheapFrame = false;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == farUploadInput.trickleBudgetBytes * 2ull &&
          farUploadBudget.pressureTier == 0,
        "far upload scheduler opportunistically expands trickle upload under clear headroom");

    SparseFarUploadBudgetInput hugeFarUploadInput{};
    hugeFarUploadInput.fullBudgetBytes = std::numeric_limits<uint64_t>::max();
    hugeFarUploadInput.trickleBudgetBytes = std::numeric_limits<uint64_t>::max() - 7ull;
    hugeFarUploadInput.totalBytes = std::numeric_limits<uint64_t>::max();
    hugeFarUploadInput.uploadedBytes = 0ull;
    hugeFarUploadInput.combinedPressureMs = 13.0f;
    hugeFarUploadInput.predictedFrameMs = 13.5f;
    hugeFarUploadInput.lastUploadMs = 0.4f;
    hugeFarUploadInput.smoothedUploadMs = 0.5f;
    hugeFarUploadInput.targetUploadMs = 1.25f;
    hugeFarUploadInput.canTrickle = true;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(hugeFarUploadInput);
    Check(farUploadBudget.budgetBytes == std::numeric_limits<uint64_t>::max() &&
          farUploadBudget.pressureTier == 0,
        "far upload scheduler saturates opportunistic trickle expansion instead of wrapping");

    farUploadInput.combinedPressureMs = 16.67f;
    farUploadInput.predictedFrameMs = 16.67f;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == farUploadInput.trickleBudgetBytes &&
          farUploadBudget.pressureTier == 1,
        "far upload scheduler uses normal trickle upload outside cheap frames");

    farUploadInput.lastUploadMs = 6.0f;
    farUploadInput.smoothedUploadMs = 4.5f;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == 128ull * 1024ull &&
          farUploadBudget.pressureTier == 3,
        "far upload scheduler clamps upload when measured upload cost is high");

    farUploadInput.readinessDeadline = true;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == 8ull * 1024ull * 1024ull &&
          farUploadBudget.pressureTier == 0,
        "far upload scheduler protects pipe-readiness deadline uploads despite prior measured upload pressure");
    farUploadInput.readinessDeadline = false;

    farUploadInput.lastUploadMs = 0.4f;
    farUploadInput.smoothedUploadMs = 0.5f;
    farUploadInput.combinedPressureMs = 23.0f;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == farUploadInput.trickleBudgetBytes &&
          farUploadBudget.pressureTier == 2,
        "far upload scheduler keeps measured-cheap far upload trickling under unrelated frame pressure");

    farUploadInput.combinedPressureMs = 13.0f;
    farUploadInput.visibleMissPressure = true;
    farUploadInput.cheapFrame = true;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.budgetBytes == farUploadInput.trickleBudgetBytes,
        "far upload scheduler does not compete with visible near-field miss catchup");

    farUploadInput.visibleMissPressure = false;
    farUploadInput.canTrickle = false;
    farUploadInput.cheapFrame = false;
    farUploadBudget = SparseRuntimeBudgetScheduler::BuildFarUploadBudget(farUploadInput);
    Check(farUploadBudget.deferred && farUploadBudget.budgetBytes == 0,
        "far upload scheduler defers when neither full upload nor trickle is allowed");

    SparseRequestBudgetDecision requestBudget =
        SparseRuntimeBudgetScheduler::BuildRequestBudgets(8, 16, 16, 32, decision);
    Check(requestBudget.speculative < 8,
        "request scheduler suppresses speculative admission under protected pressure");
    Check(requestBudget.visible >= 16 && requestBudget.collision >= 16,
        "request scheduler protects visible and collision request admission under protected pressure");
    Check(requestBudget.protectedHardTotal == requestBudget.total + requestBudget.collision,
        "request scheduler creates a protected overage lane above the general total");

    SparseRuntimeBudgetDecision neutral{};
    neutral.scale = 1.0f;
    neutral.protectedScale = 1.0f;
    neutral.backgroundScale = 1.0f;
    requestBudget = SparseRuntimeBudgetScheduler::BuildRequestBudgets(8, 16, 16, 32, neutral);
    Check(requestBudget.speculative == 8 &&
          requestBudget.visible == 16 &&
          requestBudget.collision == 16 &&
          requestBudget.total == 32,
        "request scheduler preserves configured budgets when runtime scale is neutral");
    SparseRequestBudgetDecision saturatedRequestBudget =
        SparseRuntimeBudgetScheduler::BuildRequestBudgets(UINT32_MAX - 10u, 32u, 32u, 1u, neutral);
    Check(saturatedRequestBudget.total == UINT32_MAX &&
          saturatedRequestBudget.protectedHardTotal == UINT32_MAX,
        "request scheduler saturates extreme admission totals instead of wrapping");

    SparseUploadBudgetDecision uploadBudget =
        SparseRuntimeBudgetScheduler::BuildUploadBudgets(
            8,
            20,
            12,
            3,
            2,
            neutral);
    Check(uploadBudget.edited == 2 && uploadBudget.collision == 3,
        "upload scheduler reserves edited and collision work first");
    Check(uploadBudget.visible == 3 && uploadBudget.speculative == 0,
        "upload scheduler spends remaining budget on visible before speculative");
    Check(uploadBudget.protectedTotal == 5 && uploadBudget.backgroundTotal == 3,
        "upload scheduler reports protected/background split");

    SparseRuntimeBudgetDecision trimSpec{};
    trimSpec.scale = 1.0f;
    trimSpec.protectedScale = 1.0f;
    trimSpec.backgroundScale = 0.25f;
    trimSpec.trimSpeculativeFirst = true;
    trimSpec.pressureClass = SparseRuntimePressureClass::High;
    uploadBudget =
        SparseRuntimeBudgetScheduler::BuildUploadBudgets(
            6,
            10,
            2,
            0,
            0,
            trimSpec);
    Check(uploadBudget.visible == 2 && uploadBudget.speculative == 0,
        "upload scheduler trims speculative uploads before visible uploads");

    uploadBudget =
        SparseRuntimeBudgetScheduler::BuildUploadBudgets(
            8,
            100,
            0,
            0,
            0,
            trimSpec);
    Check(uploadBudget.speculative == 2,
        "upload scheduler trickles sole speculative uploads under hard pressure");

    trimSpec.pressureClass = SparseRuntimePressureClass::BacklogHeadroom;
    trimSpec.backgroundScale = 1.35f;
    uploadBudget =
        SparseRuntimeBudgetScheduler::BuildUploadBudgets(
            8,
            100,
            0,
            0,
            0,
            trimSpec);
    Check(uploadBudget.speculative == 8,
        "upload scheduler uses available background capacity for sole speculative backlog under headroom");

    SparseFrameUploadPlanInput framePlanInput{};
    framePlanInput.uploadBytesCapacity = 1224;
    framePlanInput.pageTableResetBytes = 256;
    framePlanInput.pageTableEntryBytes = 128;
    framePlanInput.brickUploadBytes = 200;
    framePlanInput.midClipmapSnapshotBytes = 400;
    framePlanInput.pageTableResetPending = true;
    framePlanInput.midClipmapDirty = true;
    framePlanInput.protectedBacklog = true;
    framePlanInput.invalidationQueued = 2;
    framePlanInput.invalidationBudget = 2;
    framePlanInput.publishQueued = 4;
    framePlanInput.publishBudget = 4;
    framePlanInput.brickBudgets = uploadBudget;
    framePlanInput.brickBudgets.edited = 1;
    framePlanInput.brickBudgets.collision = 2;
    framePlanInput.brickBudgets.visible = 2;
    framePlanInput.brickBudgets.speculative = 2;
    framePlanInput.brickBudgets.total = 7;
    SparseFrameUploadPlan framePlan =
        SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(framePlanInput);
    Check(framePlan.allowPageTableReset, "frame upload plan reserves page-table reset first");
    Check(framePlan.invalidationBudget == 2,
        "frame upload plan reserves invalidations before brick uploads");
    Check(framePlan.brickBudgets.edited == 1 && framePlan.brickBudgets.collision >= 1,
        "frame upload plan reserves protected brick uploads before background work");
    Check(framePlan.brickBudgets.visible > 0 && framePlan.brickBudgets.speculative == 0,
        "frame upload plan preserves visible uploads and suppresses speculative under protected backlog");
    Check(!framePlan.allowMidClipmap,
        "frame upload plan defers clipmap upload when protected work exhausts byte capacity");

    framePlanInput.uploadBytesCapacity = 2600;
    framePlan = SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(framePlanInput);
    Check(framePlan.allowMidClipmap,
        "frame upload plan allows clipmap continuity upload under protected backlog when capacity exists");
    Check(framePlan.brickBudgets.speculative == 0,
        "frame upload plan still suppresses speculative brick uploads under protected backlog");

    framePlanInput = {};
    framePlanInput.uploadBytesCapacity = 1536;
    framePlanInput.pageTableEntryBytes = 128;
    framePlanInput.brickUploadBytes = 200;
    framePlanInput.midClipmapSnapshotBytes = 400;
    framePlanInput.midClipmapDirty = true;
    framePlanInput.publishQueued = 2;
    framePlanInput.publishBudget = 2;
    framePlanInput.brickBudgets.visible = 2;
    framePlanInput.brickBudgets.speculative = 1;
    framePlanInput.brickBudgets.total = 3;
    framePlan = SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(framePlanInput);
    Check(framePlan.publishBudget == 2,
        "frame upload plan reserves page-table publishes when capacity exists");
    Check(framePlan.brickBudgets.visible == 2 && framePlan.brickBudgets.speculative == 1,
        "frame upload plan allows background brick uploads without protected backlog");
    Check(framePlan.allowMidClipmap,
        "frame upload plan allows clipmap upload after protected work clears");

    framePlanInput = {};
    framePlanInput.uploadBytesCapacity = 528;
    framePlanInput.pageTableEntryBytes = 128;
    framePlanInput.brickUploadBytes = 400;
    framePlanInput.publishQueued = 2;
    framePlanInput.publishBudget = 2;
    framePlanInput.publishProtectedBacklog = true;
    framePlanInput.protectedBacklog = true;
    framePlanInput.brickBudgets.edited = 1;
    framePlanInput.brickBudgets.visible = 1;
    framePlanInput.brickBudgets.total = 2;
    framePlan = SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(framePlanInput);
    Check(framePlan.publishBudget == 2,
        "frame upload plan reserves ready page publishes before protected brick payloads");
    Check(framePlan.brickBudgets.edited == 0 && framePlan.brickBudgets.visible == 0,
        "frame upload plan prefers cheap visibility publication over another brick when bytes are tight");

    framePlanInput = {};
    framePlanInput.uploadBytesCapacity = 0;
    framePlanInput.uploadBytesAlreadyUsed = 1;
    framePlanInput.pageTableResetPending = true;
    framePlanInput.midClipmapDirty = true;
    framePlanInput.invalidationQueued = UINT32_MAX;
    framePlanInput.publishQueued = UINT32_MAX;
    framePlanInput.brickBudgets.total = UINT32_MAX;
    framePlan = SparseRuntimeBudgetScheduler::BuildFrameUploadPlan(framePlanInput);
    Check(framePlan.byteLimitedDefers == UINT32_MAX,
        "frame upload plan saturates byte-limited defer accounting instead of wrapping");
}

void TestSparseClipmapPolicy() {
    SparseClipmapPolicy policy({
        true,
        100.0f,
        900.0f,
        8.0f,
        12.0f,
        4
    });

    Check(policy.IsEnabled(), "clipmap policy enabled");
    Check(policy.Config().voxelInterestCapacityPercent == 75u,
        "clipmap policy keeps default voxel interest reserve");
    Check(policy.Config().motionLookaheadSteps == 3u &&
          policy.Config().motionLookaheadMinSpeed == 64.0f,
        "clipmap policy keeps default motion lookahead lane");
    SparseClipmapConfig unclampedInterest;
    unclampedInterest.voxelInterestCapacityPercent = 5u;
    unclampedInterest.motionLookaheadSteps = 32u;
    SparseClipmapPolicy clampedInterestPolicy(unclampedInterest);
    Check(clampedInterestPolicy.Config().voxelInterestCapacityPercent == 25u,
        "clipmap policy clamps low voxel interest reserve");
    Check(clampedInterestPolicy.Config().motionLookaheadSteps == 8u,
        "clipmap policy clamps excessive motion lookahead steps");
    SparseClipmapConfig nonFiniteConfig;
    nonFiniteConfig.startDistance = std::numeric_limits<float>::quiet_NaN();
    nonFiniteConfig.endDistance = std::numeric_limits<float>::infinity();
    nonFiniteConfig.minCellSize = -std::numeric_limits<float>::infinity();
    nonFiniteConfig.nearExitPadding = std::numeric_limits<float>::quiet_NaN();
    nonFiniteConfig.motionLookaheadMinSpeed = std::numeric_limits<float>::quiet_NaN();
    SparseClipmapPolicy nonFinitePolicy(nonFiniteConfig);
    Check(std::isfinite(nonFinitePolicy.Config().startDistance) &&
          std::isfinite(nonFinitePolicy.Config().endDistance) &&
          std::isfinite(nonFinitePolicy.Config().minCellSize) &&
          std::isfinite(nonFinitePolicy.Config().nearExitPadding) &&
          std::isfinite(nonFinitePolicy.Config().motionLookaheadMinSpeed),
        "clipmap policy sanitizes non-finite configuration values");
    Check(nonFinitePolicy.Config().endDistance > nonFinitePolicy.Config().startDistance,
        "clipmap policy keeps sanitized transition range valid");
    Check(nonFinitePolicy.CellSizeForDistance(std::numeric_limits<float>::quiet_NaN()) ==
          nonFinitePolicy.Config().minCellSize,
        "clipmap policy maps non-finite distance to minimum cell size");
    Check(!nonFinitePolicy.OwnsRaySegment(
              std::numeric_limits<float>::quiet_NaN(),
              200.0f,
              100.0f),
        "clipmap policy rejects non-finite segment ownership checks");
    Check(!nonFinitePolicy.AllowsBackgroundForMissingNearPage(
              std::numeric_limits<float>::quiet_NaN(),
              100.0f),
        "clipmap policy rejects non-finite missing-near fallback distance");
    Check(policy.TransitionStartAfterNearExit(40.0f) == 100.0f,
        "clipmap starts at configured start when near exit is close");
    Check(policy.TransitionStartAfterNearExit(200.0f) == 212.0f,
        "clipmap starts after near exit plus padding");
    Check(policy.BackgroundStartAfterNearVolumeExit(40.0f) == 100.0f,
        "background starts at configured clipmap start when near volume exits early");
    Check(policy.BackgroundStartAfterNearVolumeExit(360.0f) == 372.0f,
        "background starts after near volume exit when sparse near field is larger");
    Check(policy.FarLayerStartAfterBackground(120.0f) > policy.BackgroundStartAfterNearVolumeExit(40.0f),
        "far layer starts after the mid transition band begins");
    Check(policy.FarLayerStartAfterBackground(120.0f) < policy.Config().endDistance,
        "far layer handoff stays inside the configured clipmap range");
    Check(policy.FarLayerStartAfterBackground(940.0f) == 940.0f,
        "far layer preserves a background start already beyond the clipmap range");
    Check(policy.MissingNearPageBackgroundStart(120.0f, 360.0f) == 372.0f,
        "missing-page fallback waits for near-owned volume exit when miss is foreground");
    Check(policy.MissingNearPageBackgroundStart(500.0f, 360.0f) == 524.0f,
        "missing-page fallback also waits for missing-page padding after the miss point");
    Check(!policy.AllowsBackgroundForMissingNearPage(180.0f, 360.0f),
        "background may not fill missing sparse pages inside near-owned volume");
    Check(policy.AllowsBackgroundForMissingNearPage(384.0f, 360.0f),
        "background may fill only after near-owned volume transition");
    Check(policy.OwnsRaySegment(210.0f, 220.0f, 200.0f),
        "clipmap owns segment beyond padded near exit");
    Check(!policy.OwnsRaySegment(120.0f, 180.0f, 200.0f),
        "clipmap does not draw through near-owned segment");
    Check(!policy.OwnsRaySegment(800.0f, 1100.0f, 900.0f),
        "clipmap owns no segment when near exit is beyond the mid range");
    Check(policy.CellSizeForDistance(100.0f) == 8.0f,
        "clipmap first ring cell size");
    Check(policy.CellSizeForDistance(350.0f) == 16.0f,
        "clipmap second ring cell size");
    Check(policy.CellSizeForDistance(560.0f) == 32.0f,
        "clipmap third ring cell size");
    Check(policy.CellSizeForDistance(850.0f) == 64.0f,
        "clipmap fourth ring cell size");

    const std::vector<SparseClipmapRing> rings = policy.BuildRings();
    Check(rings.size() == 4, "clipmap builds configured rings");
    Check(rings.front().startDistance == 100.0f && rings.back().endDistance == 900.0f,
        "clipmap rings cover configured span");
    const SparseClipmapTransitionMetadata metadata = policy.BuildTransitionMetadata();
    Check(metadata.enabled, "clipmap transition metadata is marked valid when enabled");
    Check(metadata.startDistance == policy.Config().startDistance,
        "clipmap transition metadata exposes configured mid start");
    Check(metadata.endDistance == policy.Config().endDistance,
        "clipmap transition metadata exposes configured mid end");
    Check(metadata.minCellSize == policy.Config().minCellSize,
        "clipmap transition metadata exposes configured cell size");
    Check(metadata.farHandoffDistance == policy.FarLayerStartAfterBackground(policy.Config().startDistance),
        "clipmap transition metadata uses the policy far handoff");
    Check(metadata.farHandoffDistance > metadata.startDistance &&
          metadata.farHandoffDistance < metadata.endDistance,
        "clipmap transition metadata keeps far handoff inside the mid range");
    const SparseClipmapTransitionMetadata nearExitMetadata =
        policy.BuildTransitionMetadataAfterNearExit(360.0f);
    Check(nearExitMetadata.startDistance == policy.BackgroundStartAfterNearVolumeExit(360.0f),
        "ray-aware clipmap transition metadata starts after near volume exit");
    Check(nearExitMetadata.farHandoffDistance ==
          policy.FarLayerStartAfterBackground(nearExitMetadata.startDistance),
        "ray-aware clipmap transition metadata keeps far handoff after adjusted background start");
    const SparseClipmapTransitionMetadata exhaustedNearExitMetadata =
        policy.BuildTransitionMetadataAfterNearExit(900.0f);
    Check(!exhaustedNearExitMetadata.enabled &&
          exhaustedNearExitMetadata.startDistance == exhaustedNearExitMetadata.endDistance &&
          exhaustedNearExitMetadata.farHandoffDistance == exhaustedNearExitMetadata.endDistance,
        "ray-aware clipmap transition metadata disables inverted mid ranges");

    SparseClipmapPolicy disabled({false, 100.0f, 900.0f, 8.0f, 12.0f, 4});
    Check(!disabled.IsEnabled(), "disabled clipmap policy");
    Check(disabled.BuildRings().empty(), "disabled clipmap emits no rings");
    const SparseClipmapTransitionMetadata disabledMetadata = disabled.BuildTransitionMetadata();
    Check(!disabledMetadata.enabled, "disabled clipmap transition metadata is invalid");

    SparseClipmapCacheStats emptyStats;
    const SparseClipmapResidencyMetadata emptyResidency =
        BuildClipmapResidencyMetadata(emptyStats);
    Check(emptyResidency.heightCoverageRatio == 0.0f &&
          emptyResidency.voxelCoverageRatio == 0.0f,
        "clipmap residency metadata reports no coverage without interest");

    SparseClipmapCacheStats partialStats;
    partialStats.residentTiles = 9;
    partialStats.interestedTiles = 10;
    partialStats.missingInterestedTiles = 3;
    partialStats.residentVoxelBricks = 20;
    partialStats.interestedVoxelBricks = 40;
    partialStats.missingInterestedVoxelBricks = 10;
    const SparseClipmapResidencyMetadata partialResidency =
        BuildClipmapResidencyMetadata(partialStats);
    Check(partialResidency.heightCoverageRatio > 0.69f &&
          partialResidency.heightCoverageRatio < 0.71f,
        "clipmap residency metadata computes height coverage ratio");
    Check(partialResidency.voxelCoverageRatio > 0.74f &&
          partialResidency.voxelCoverageRatio < 0.76f,
        "clipmap residency metadata computes voxel coverage ratio");
    Check(partialResidency.residentHeightTiles == partialStats.residentTiles &&
          partialResidency.residentVoxelBricks == partialStats.residentVoxelBricks,
        "clipmap residency metadata preserves resident counts");
}

void TestSparseClipmapTileCache() {
    SparseClipmapConfig config;
    config.enabled = true;
    config.startDistance = 64.0f;
    config.endDistance = 512.0f;
    config.minCellSize = 16.0f;
    config.nearExitPadding = 8.0f;
    config.ringCount = 2;
    config.tileRadius = 1;
    config.tileSampleSide = 9;
    config.maxTiles = 4;
    config.seed = 12345u;

    SparseClipmapPolicy policy(config);
    SparseClipmapTileCache cache;
    Check(cache.Initialize(policy.Config()), "clipmap tile cache initializes");
    cache.UpdateInterest(0.0f, 128.0f, 0.0f, 1u, policy);
    Check(cache.GetStats().interestedTiles > 0, "clipmap tile cache tracks height interest set size");
    Check(cache.GetStats().missingInterestedTiles == cache.GetStats().interestedTiles,
        "clipmap tile cache reports all height interest missing before generation");
    Check(cache.GetStats().interestedVoxelBricks > 0, "clipmap tile cache tracks voxel interest set size");
    Check(cache.GetStats().missingInterestedVoxelBricks == cache.GetStats().interestedVoxelBricks,
        "clipmap tile cache reports all voxel interest missing before generation");
    Check(cache.GetStats().queuedTiles > 0, "clipmap tile cache queues camera interest");

    const uint32_t generated = cache.PumpGeneration(2u, 1u, policy);
    Check(generated >= 2u, "clipmap tile cache performs budgeted generation work");
    Check(cache.GetStats().generatedTilesLastFrame == 2u, "clipmap tile cache obeys height-tile generation budget");
    Check(cache.GetStats().residentTiles == 2u, "clipmap tile cache has generated residents");
    Check(cache.GetStats().missingInterestedTiles < cache.GetStats().interestedTiles,
        "clipmap height coverage improves after generation");
    Check(cache.GetStats().missingInterestedVoxelBricks < cache.GetStats().interestedVoxelBricks,
        "clipmap voxel coverage improves after generation");

    SparseClipmapGpuSnapshot snapshot;
    Check(cache.BuildGpuSnapshot(snapshot), "clipmap tile cache builds GPU snapshot");
    Check(snapshot.tileCount == 2u, "clipmap snapshot stable tile slot high-water count");
    Check(snapshot.heightDirtyStartSlot == 0u && snapshot.heightDirtySlotCount == 2u,
        "clipmap snapshot tracks initial dirty height tile slot span");
    Check(snapshot.tileSampleSide == 9u, "clipmap snapshot stores sample side");
    Check(snapshot.metadata.size() == static_cast<size_t>(config.maxTiles + 1u) * 4u,
        "clipmap snapshot metadata capacity");
    Check(!snapshot.lookup.empty(), "clipmap snapshot lookup table exists");
    Check((snapshot.lookupCapacity & (snapshot.lookupCapacity - 1u)) == 0u,
        "clipmap snapshot lookup capacity is power-of-two");
    Check(snapshot.lookup.size() == static_cast<size_t>(snapshot.lookupCapacity) * 4u,
        "clipmap snapshot lookup storage matches capacity");
    Check(snapshot.heightSamplePayloadStartSlot == snapshot.heightDirtyStartSlot,
        "clipmap dirty height payload starts at dirty slot");
    Check(snapshot.samples.size() ==
            static_cast<size_t>(snapshot.heightDirtySlotCount) *
            config.tileSampleSide *
            config.tileSampleSide,
        "clipmap dirty height sample payload is compact");
    Check(snapshot.metadata[0] == 0x56434C50u, "clipmap snapshot magic");
    Check(snapshot.metadata[2] == snapshot.tileCount, "clipmap snapshot header tile count");
    Check((snapshot.metadata[3] & 0x00FFFFFFu) == snapshot.lookupCapacity,
        "clipmap snapshot header stores lookup capacity");
    Check((snapshot.metadata[3] >> 24u) == config.ringCount,
        "clipmap snapshot header stores ring count");

    uint32_t populatedLookupEntries = 0;
    for (size_t i = 0; i + 3u < snapshot.lookup.size(); i += 4u) {
        populatedLookupEntries += snapshot.lookup[i + 3u] != 0u ? 1u : 0u;
    }
    Check(populatedLookupEntries == snapshot.tileCount,
        "clipmap lookup has one populated entry per resident stable tile slot");
    cache.ClearHeightDirtyRange();
    SparseClipmapGpuSnapshot cleanHeightSnapshot;
    Check(cache.BuildGpuSnapshot(cleanHeightSnapshot), "clipmap snapshot rebuilds after height dirty clear");
    Check(cleanHeightSnapshot.heightDirtySlotCount == 0u,
        "clipmap height dirty slot span clears after upload ack");
    Check(cleanHeightSnapshot.heightSamplePayloadStartSlot == 0u,
        "clipmap clean height payload starts at zero");
    Check(cleanHeightSnapshot.samples.size() ==
            static_cast<size_t>(config.maxTiles) *
            config.tileSampleSide *
            config.tileSampleSide,
        "clipmap clean height sample payload remains full");
    Check(snapshot.voxelBrickCount > 0, "voxel clipmap generates coarse 3D bricks");
    Check(!snapshot.voxelMetadata.empty(), "voxel clipmap metadata exists");
    Check(!snapshot.voxelLookup.empty(), "voxel clipmap lookup exists");
    Check(!snapshot.voxelSamples.empty(), "voxel clipmap samples exist");
    Check(snapshot.voxelSamplePayloadStartSlot == snapshot.voxelDirtyStartSlot,
        "voxel clipmap dirty payload starts at dirty slot");
    Check(snapshot.voxelSamples.size() ==
            static_cast<size_t>(snapshot.voxelDirtySlotCount) *
            SPARSE_BRICK_VOXEL_COUNT,
        "voxel clipmap dirty sample payload is compact");
    Check(snapshot.voxelMetadata[0] == 0x56435658u, "voxel clipmap snapshot magic");
    Check((snapshot.voxelMetadata[3] & 0x00FFFFFFu) == snapshot.voxelLookupCapacity,
        "voxel clipmap header stores lookup capacity");
    uint32_t populatedVoxelLookupEntries = 0;
    for (size_t i = 0; i + 3u < snapshot.voxelLookup.size(); i += 4u) {
        populatedVoxelLookupEntries += snapshot.voxelLookup[i + 3u] != 0u ? 1u : 0u;
    }
    Check(populatedVoxelLookupEntries == cache.GetStats().residentVoxelBricks,
        "voxel clipmap lookup has one populated entry per resident coarse brick");
    Check(populatedVoxelLookupEntries <= snapshot.voxelBrickCount,
        "voxel clipmap stable slot range covers populated lookup entries");
    uint32_t nonAirMidSamples = 0;
    uint32_t taggedSurfaceMidSamples = 0;
    uint32_t invalidTaggedAirSamples = 0;
    for (uint32_t sample : snapshot.voxelSamples) {
        const uint8_t material = VENPOD::Utils::UnpackMaterial(sample);
        const bool taggedSurface =
            (VENPOD::Utils::UnpackState(sample) & VENPOD::Utils::StateFlags::VisualSurface) != 0;
        if (material != VENPOD::Utils::Material::Air) {
            ++nonAirMidSamples;
            if (taggedSurface) {
                ++taggedSurfaceMidSamples;
            }
        } else if (taggedSurface) {
            ++invalidTaggedAirSamples;
        }
    }
    Check(nonAirMidSamples > 0, "voxel clipmap snapshot carries non-air coarse samples");
    Check(taggedSurfaceMidSamples > 0, "voxel clipmap tags renderable visual surface samples");
    Check(invalidTaggedAirSamples == 0, "voxel clipmap never tags air as a visual surface");

    auto voxelSampleAt = [](
        const SparseClipmapGpuSnapshot& gpuSnapshot,
        uint32_t slot,
        uint32_t localIndex,
        uint32_t* outSample) {
        if (!outSample || localIndex >= SPARSE_BRICK_VOXEL_COUNT) {
            return false;
        }
        uint32_t payloadSlotBase = 0;
        for (const SparseClipmapSampleRange& range : gpuSnapshot.voxelSampleRanges) {
            if (slot >= range.startSlot && slot < range.startSlot + range.slotCount) {
                const uint32_t payloadSlot = payloadSlotBase + (slot - range.startSlot);
                const size_t sampleIndex =
                    static_cast<size_t>(payloadSlot) * SPARSE_BRICK_VOXEL_COUNT + localIndex;
                if (sampleIndex >= gpuSnapshot.voxelSamples.size()) {
                    return false;
                }
                *outSample = gpuSnapshot.voxelSamples[sampleIndex];
                return true;
            }
            payloadSlotBase += range.slotCount;
        }
        return false;
    };

    uint32_t editTargetSlot = UINT32_MAX;
    uint32_t editTargetLocal = UINT32_MAX;
    int32_t editTargetOriginX = 0;
    int32_t editTargetOriginY = 0;
    int32_t editTargetOriginZ = 0;
    uint32_t editTargetCellSize = 0;
    for (uint32_t slot = 0; slot < snapshot.voxelBrickCount && editTargetSlot == UINT32_MAX; ++slot) {
        const size_t metadataBase = static_cast<size_t>(slot + 1u) * 4u;
        if (metadataBase + 3u >= snapshot.voxelMetadata.size()) {
            break;
        }
        const uint32_t cellSize = snapshot.voxelMetadata[metadataBase + 3u] >> 8u;
        if (cellSize <= 1u) {
            continue;
        }
        for (uint32_t local = 0; local < SPARSE_BRICK_VOXEL_COUNT; ++local) {
            uint32_t sample = 0;
            if (!voxelSampleAt(snapshot, slot, local, &sample)) {
                continue;
            }
            const uint8_t material = VENPOD::Utils::UnpackMaterial(sample);
            const bool taggedSurface =
                (VENPOD::Utils::UnpackState(sample) & VENPOD::Utils::StateFlags::VisualSurface) != 0;
            if (material != VENPOD::Utils::Material::Air &&
                material != VENPOD::Utils::Material::Water &&
                taggedSurface) {
                editTargetSlot = slot;
                editTargetLocal = local;
                editTargetOriginX = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 0u]);
                editTargetOriginY = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 1u]);
                editTargetOriginZ = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 2u]);
                editTargetCellSize = cellSize;
                break;
            }
        }
    }
    Check(editTargetSlot != UINT32_MAX, "voxel clipmap test finds coarse editable surface sample");
    if (editTargetSlot != UINT32_MAX) {
        const uint32_t localX = editTargetLocal % SPARSE_BRICK_SIZE;
        const uint32_t localY = (editTargetLocal / SPARSE_BRICK_SIZE) % SPARSE_BRICK_SIZE;
        const uint32_t localZ = editTargetLocal / (SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE);
        const int32_t cellMinX = editTargetOriginX + static_cast<int32_t>(localX * editTargetCellSize);
        const int32_t cellMinY = editTargetOriginY + static_cast<int32_t>(localY * editTargetCellSize);
        const int32_t cellMinZ = editTargetOriginZ + static_cast<int32_t>(localZ * editTargetCellSize);
        const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);

        SparseEditStore singleAirEdit;
        singleAirEdit.SetVoxel(cellMinX, cellMinY, cellMinZ, air);
        cache.SetEditStore(&singleAirEdit);
        Check(cache.InvalidateEditedOverlays(singleAirEdit, policy) > 0u,
            "single air edit invalidates overlapping coarse voxel brick");
        SparseClipmapGpuSnapshot singleAirSnapshot;
        Check(cache.BuildGpuSnapshot(singleAirSnapshot, false, true),
            "single air edit coarse voxel snapshot builds");
        uint32_t singleAirSample = air;
        Check(voxelSampleAt(singleAirSnapshot, editTargetSlot, editTargetLocal, &singleAirSample),
            "single air edit target sample is present in dirty voxel payload");
        Check(VENPOD::Utils::UnpackMaterial(singleAirSample) != VENPOD::Utils::Material::Air,
            "single air edit does not collapse a coarse mid-clipmap cell");

        SparseEditStore clusteredAirEdits;
        const uint32_t clusterCount = std::max<uint32_t>(8u, editTargetCellSize);
        for (uint32_t i = 0; i < clusterCount; ++i) {
            clusteredAirEdits.SetVoxel(
                cellMinX + static_cast<int32_t>(i % editTargetCellSize),
                cellMinY,
                cellMinZ + static_cast<int32_t>(i / editTargetCellSize),
                air);
        }
        cache.SetEditStore(&clusteredAirEdits);
        Check(cache.InvalidateEditedOverlays(clusteredAirEdits, policy) > 0u,
            "clustered air edits invalidate overlapping coarse voxel brick");
        SparseClipmapGpuSnapshot clusteredAirSnapshot;
        Check(cache.BuildGpuSnapshot(clusteredAirSnapshot, false, true),
            "clustered air edit coarse voxel snapshot builds");
        uint32_t clusteredAirSample = 0;
        Check(voxelSampleAt(clusteredAirSnapshot, editTargetSlot, editTargetLocal, &clusteredAirSample),
            "clustered air edit target sample is present in dirty voxel payload");
        Check(VENPOD::Utils::UnpackMaterial(clusteredAirSample) == VENPOD::Utils::Material::Air,
            "clustered air edits collapse stale coarse procedural terrain to air");
        cache.SetEditStore(nullptr);
    }

    SparseClipmapTileCache nonFiniteInterestCache;
    Check(nonFiniteInterestCache.Initialize(policy.Config()),
        "clipmap non-finite interest cache initializes");
    nonFiniteInterestCache.UpdateInterest(
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        3u,
        policy,
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity());
    Check(nonFiniteInterestCache.GetStats().interestedTiles > 0,
        "clipmap non-finite interest sanitizes height anchors");
    Check(nonFiniteInterestCache.GetStats().interestedVoxelBricks > 0,
        "clipmap non-finite interest sanitizes voxel anchors");
    Check(nonFiniteInterestCache.PumpGeneration(8u, 3u, policy) > 0,
        "clipmap non-finite interest generates sanitized requests");
    SparseClipmapGpuSnapshot nonFiniteInterestSnapshot;
    Check(nonFiniteInterestCache.BuildGpuSnapshot(nonFiniteInterestSnapshot),
        "clipmap non-finite interest snapshot builds");
    for (size_t i = 4; i + 3u < nonFiniteInterestSnapshot.metadata.size(); i += 4u) {
        if (nonFiniteInterestSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(nonFiniteInterestSnapshot.metadata[i + 0u]);
        const int32_t originZ = static_cast<int32_t>(nonFiniteInterestSnapshot.metadata[i + 1u]);
        Check(std::abs(originX) < 4096 && std::abs(originZ) < 4096,
            "clipmap non-finite height interest stays near sanitized origin");
    }
    for (size_t i = 4; i + 3u < nonFiniteInterestSnapshot.voxelMetadata.size(); i += 4u) {
        if ((nonFiniteInterestSnapshot.voxelMetadata[i + 3u] & 0xFFu) == 0u &&
            nonFiniteInterestSnapshot.voxelMetadata[i + 0u] == 0u &&
            nonFiniteInterestSnapshot.voxelMetadata[i + 1u] == 0u &&
            nonFiniteInterestSnapshot.voxelMetadata[i + 2u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(nonFiniteInterestSnapshot.voxelMetadata[i + 0u]);
        const int32_t originY = static_cast<int32_t>(nonFiniteInterestSnapshot.voxelMetadata[i + 1u]);
        const int32_t originZ = static_cast<int32_t>(nonFiniteInterestSnapshot.voxelMetadata[i + 2u]);
        Check(std::abs(originX) < 4096 && std::abs(originY) < 4096 && std::abs(originZ) < 4096,
            "clipmap non-finite voxel interest stays near sanitized origin");
    }

    SparseClipmapTileCache extremeFiniteInterestCache;
    Check(extremeFiniteInterestCache.Initialize(policy.Config()),
        "clipmap extreme finite interest cache initializes");
    extremeFiniteInterestCache.UpdateInterest(
        1.0e30f,
        -1.0e30f,
        1.0e30f,
        4u,
        policy,
        1.0e30f,
        -1.0e30f,
        1.0e30f,
        -1.0e30f,
        1.0e30f,
        -1.0e30f,
        2.0f);
    Check(extremeFiniteInterestCache.GetStats().interestedTiles > 0,
        "clipmap extreme finite interest sanitizes height anchors");
    Check(extremeFiniteInterestCache.GetStats().interestedVoxelBricks > 0,
        "clipmap extreme finite interest sanitizes voxel anchors");
    Check(extremeFiniteInterestCache.PumpGeneration(8u, 4u, policy) > 0,
        "clipmap extreme finite interest generates clamped requests");
    SparseClipmapGpuSnapshot extremeFiniteInterestSnapshot;
    Check(extremeFiniteInterestCache.BuildGpuSnapshot(extremeFiniteInterestSnapshot),
        "clipmap extreme finite interest snapshot builds");
    for (size_t i = 4; i + 3u < extremeFiniteInterestSnapshot.metadata.size(); i += 4u) {
        if (extremeFiniteInterestSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(extremeFiniteInterestSnapshot.metadata[i + 0u]);
        const int32_t originZ = static_cast<int32_t>(extremeFiniteInterestSnapshot.metadata[i + 1u]);
        Check(originX != std::numeric_limits<int32_t>::min() &&
              originX != std::numeric_limits<int32_t>::max() &&
              originZ != std::numeric_limits<int32_t>::min() &&
              originZ != std::numeric_limits<int32_t>::max(),
            "clipmap extreme finite height interest avoids saturated origins");
    }
    for (size_t i = 4; i + 3u < extremeFiniteInterestSnapshot.voxelMetadata.size(); i += 4u) {
        if ((extremeFiniteInterestSnapshot.voxelMetadata[i + 3u] & 0xFFu) == 0u &&
            extremeFiniteInterestSnapshot.voxelMetadata[i + 0u] == 0u &&
            extremeFiniteInterestSnapshot.voxelMetadata[i + 1u] == 0u &&
            extremeFiniteInterestSnapshot.voxelMetadata[i + 2u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(extremeFiniteInterestSnapshot.voxelMetadata[i + 0u]);
        const int32_t originY = static_cast<int32_t>(extremeFiniteInterestSnapshot.voxelMetadata[i + 1u]);
        const int32_t originZ = static_cast<int32_t>(extremeFiniteInterestSnapshot.voxelMetadata[i + 2u]);
        Check(originX != std::numeric_limits<int32_t>::min() &&
              originX != std::numeric_limits<int32_t>::max() &&
              originY != std::numeric_limits<int32_t>::min() &&
              originY != std::numeric_limits<int32_t>::max() &&
              originZ != std::numeric_limits<int32_t>::min() &&
              originZ != std::numeric_limits<int32_t>::max(),
            "clipmap extreme finite voxel interest avoids saturated origins");
    }

    SparseClipmapConfig tightVoxelConfig = config;
    tightVoxelConfig.ringCount = 4;
    tightVoxelConfig.maxVoxelBricks = 12;
    tightVoxelConfig.voxelBrickRadiusXz = 2;
    tightVoxelConfig.voxelBrickRadiusY = 1;
    SparseClipmapPolicy tightVoxelPolicy(tightVoxelConfig);
    SparseClipmapTileCache tightVoxelCache;
    Check(tightVoxelCache.Initialize(tightVoxelPolicy.Config()),
        "capacity-aware voxel clipmap cache initializes");
    tightVoxelCache.UpdateInterest(0.0f, 128.0f, 0.0f, 1u, tightVoxelPolicy);
    Check(tightVoxelCache.GetStats().interestedVoxelBricks < tightVoxelConfig.maxVoxelBricks,
        "voxel clipmap keeps a physical slot reserve outside active interest");
    Check(tightVoxelCache.GetStats().queuedVoxelBricks <= tightVoxelConfig.maxVoxelBricks,
        "voxel clipmap interest is capped by resident capacity");
    tightVoxelCache.PumpGeneration(64u, 1u, tightVoxelPolicy);
    Check(tightVoxelCache.GetStats().residentVoxelBricks <= tightVoxelConfig.maxVoxelBricks,
        "voxel clipmap resident count respects capacity");
    Check(tightVoxelCache.GetStats().missingInterestedVoxelBricks == 0u,
        "voxel clipmap reports full coverage once capped interest is resident");
    SparseClipmapGpuSnapshot tightVoxelSnapshot;
    Check(tightVoxelCache.BuildGpuSnapshot(tightVoxelSnapshot),
        "capacity-aware voxel clipmap snapshot builds");
    Check(tightVoxelSnapshot.voxelBrickCount <= tightVoxelConfig.maxVoxelBricks,
        "capacity-aware voxel snapshot does not exceed brick capacity");
    tightVoxelCache.UpdateInterest(0.0f, 128.0f, 0.0f, 2u, tightVoxelPolicy);
    tightVoxelCache.PumpGeneration(64u, 2u, tightVoxelPolicy);
    Check(tightVoxelCache.GetStats().generatedVoxelBricksLastFrame == 0,
        "stable voxel clipmap interest does not regenerate after warmup");
    Check(tightVoxelCache.GetStats().evictedVoxelBricksLastFrame == 0,
        "stable voxel clipmap interest does not evict after warmup");

    SparseClipmapConfig lookAheadConfig = config;
    lookAheadConfig.maxTiles = 64;
    lookAheadConfig.maxVoxelBricks = 96;
    lookAheadConfig.ringCount = 1;
    lookAheadConfig.tileRadius = 1;
    lookAheadConfig.voxelBrickRadiusXz = 2;
    lookAheadConfig.voxelBrickRadiusY = 1;
    SparseClipmapPolicy lookAheadPolicy(lookAheadConfig);
    SparseClipmapTileCache lookAheadCache;
    Check(lookAheadCache.Initialize(lookAheadPolicy.Config()),
        "clipmap lookahead cache initializes");
    lookAheadCache.UpdateInterest(
        0.0f,
        128.0f,
        0.0f,
        1u,
        lookAheadPolicy,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        160.0f,
        0.5f);
    Check(lookAheadCache.GetStats().heightInterestAnchors >= 2u,
        "clipmap records camera-forward height interest anchors");
    Check(lookAheadCache.GetStats().voxelInterestAnchors >= 2u,
        "clipmap records camera-forward voxel interest anchors");
    auto sortVoxelCoords = [](std::vector<SparseVoxelClipmapCoord>& coords) {
        std::sort(
            coords.begin(),
            coords.end(),
            [](const SparseVoxelClipmapCoord& lhs, const SparseVoxelClipmapCoord& rhs) {
                if (lhs.ring != rhs.ring) return lhs.ring < rhs.ring;
                if (lhs.x != rhs.x) return lhs.x < rhs.x;
                if (lhs.y != rhs.y) return lhs.y < rhs.y;
                return lhs.z < rhs.z;
            });
    };
    std::vector<SparseVoxelClipmapCoord> debugMissingBeforeCollection;
    lookAheadCache.CollectMissingVoxelInterest(debugMissingBeforeCollection);
    sortVoxelCoords(debugMissingBeforeCollection);
    const uint32_t debugInterestedBeforeCollection =
        lookAheadCache.GetStats().interestedVoxelBricks;
    const uint32_t debugMissingBeforeCollectionCount =
        lookAheadCache.GetStats().missingInterestedVoxelBricks;
    std::vector<SparseVoxelClipmapCoord> predictedDebugCoords;
    const uint32_t predictedDebugCount =
        lookAheadCache.CollectPredictedVisibleVoxelInterestForDebug(
            predictedDebugCoords,
            0.0f,
            128.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            2u,
            lookAheadPolicy,
            32u);
    std::vector<SparseVoxelClipmapCoord> predictedPureDebugCoords;
    const uint32_t predictedPureDebugCount =
        lookAheadCache.CollectPredictedVisibleVoxelInterestPureForDebug(
            predictedPureDebugCoords,
            0.0f,
            128.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            2u,
            lookAheadPolicy,
            32u);
    std::vector<SparseVoxelClipmapCoord> debugMissingAfterCollection;
    lookAheadCache.CollectMissingVoxelInterest(debugMissingAfterCollection);
    sortVoxelCoords(debugMissingAfterCollection);
    Check(predictedDebugCount == predictedDebugCoords.size(),
        "clipmap debug predicted collector reports returned coord count");
    Check(predictedDebugCount > 0u,
        "clipmap debug predicted collector returns candidate voxel work");
    Check(predictedPureDebugCount == predictedPureDebugCoords.size(),
        "clipmap pure predicted collector reports returned coord count");
    Check(predictedPureDebugCoords == predictedDebugCoords,
        "clipmap pure predicted collector matches stateful predicted candidate order");
    Check(lookAheadCache.GetStats().interestedVoxelBricks == debugInterestedBeforeCollection,
        "clipmap debug predicted collector preserves current voxel interest size");
    Check(lookAheadCache.GetStats().missingInterestedVoxelBricks == debugMissingBeforeCollectionCount,
        "clipmap debug predicted collector preserves current missing voxel interest count");
    Check(debugMissingAfterCollection == debugMissingBeforeCollection,
        "clipmap debug predicted collector preserves current missing voxel interest set");

    SparseClipmapConfig highAltPredictionConfig;
    highAltPredictionConfig.enabled = true;
    highAltPredictionConfig.heightClipmapEnabled = true;
    highAltPredictionConfig.voxelClipmapEnabled = true;
    highAltPredictionConfig.startDistance = 1024.0f;
    highAltPredictionConfig.endDistance = 6400.0f;
    highAltPredictionConfig.minCellSize = 12.0f;
    highAltPredictionConfig.nearExitPadding = 12.0f;
    highAltPredictionConfig.ringCount = 4u;
    highAltPredictionConfig.tileRadius = 3u;
    highAltPredictionConfig.tileSampleSide = 33u;
    highAltPredictionConfig.maxTiles = 256u;
    highAltPredictionConfig.voxelBrickRadiusXz = 8u;
    highAltPredictionConfig.voxelBrickRadiusY = 4u;
    highAltPredictionConfig.maxVoxelBricks = 12288u;
    highAltPredictionConfig.voxelInterestCapacityPercent = 75u;
    highAltPredictionConfig.motionLookaheadMinSpeed = 64.0f;
    highAltPredictionConfig.motionLookaheadSteps = 3u;
    highAltPredictionConfig.interestUpdateIntervalFrames = 1u;
    highAltPredictionConfig.backlogAwarePump = true;
    highAltPredictionConfig.seed = 12345u;
    SparseClipmapPolicy highAltPredictionPolicy(highAltPredictionConfig);
    SparseClipmapTileCache highAltPredictionCache;
    Check(highAltPredictionCache.Initialize(highAltPredictionPolicy.Config()),
        "high-alt predicted candidate cache initializes");
    highAltPredictionCache.UpdateInterest(
        -461.96f,
        436.82f,
        874.31f,
        300u,
        highAltPredictionPolicy,
        0.646f,
        -0.364f,
        -0.671f,
        0.0f,
        0.0f,
        0.0f,
        0.0f);
    highAltPredictionCache.PumpGeneration(
        highAltPredictionConfig.maxTiles,
        highAltPredictionConfig.maxVoxelBricks,
        300u,
        highAltPredictionPolicy);
    std::vector<SparseVoxelClipmapCoord> highAltStatefulPredicted;
    std::vector<SparseVoxelClipmapCoord> highAltPurePredicted;
    std::vector<SparseVoxelClipmapCoord> highAltPureResidentTouches;
    const uint32_t highAltStatefulCount =
        highAltPredictionCache.CollectPredictedVisibleVoxelInterestForDebug(
            highAltStatefulPredicted,
            -315.32f,
            435.00f,
            -486.55f,
            0.586f,
            -0.430f,
            0.687f,
            360u,
            highAltPredictionPolicy,
            4096u);
    const uint32_t highAltPureCount =
        highAltPredictionCache.CollectPredictedVisibleVoxelInterestPureForDebug(
            highAltPurePredicted,
            -315.32f,
            435.00f,
            -486.55f,
            0.586f,
            -0.430f,
            0.687f,
            360u,
            highAltPredictionPolicy,
            4096u,
            &highAltPureResidentTouches);
    if (highAltPurePredicted != highAltStatefulPredicted) {
        size_t mismatch = 0;
        while (mismatch < highAltPurePredicted.size() &&
               mismatch < highAltStatefulPredicted.size() &&
               highAltPurePredicted[mismatch] == highAltStatefulPredicted[mismatch]) {
            ++mismatch;
        }
        std::cerr << "  high-alt predicted mismatch stateful="
                  << highAltStatefulPredicted.size() << " pure="
                  << highAltPurePredicted.size() << " firstMismatch=" << mismatch << '\n';
        if (mismatch < highAltStatefulPredicted.size()) {
            const SparseVoxelClipmapCoord& coord = highAltStatefulPredicted[mismatch];
            std::cerr << "  stateful[" << mismatch << "]=("
                      << coord.ring << "," << coord.x << "," << coord.y << "," << coord.z << ")\n";
        }
        if (mismatch < highAltPurePredicted.size()) {
            const SparseVoxelClipmapCoord& coord = highAltPurePredicted[mismatch];
            std::cerr << "  pure[" << mismatch << "]=("
                      << coord.ring << "," << coord.x << "," << coord.y << "," << coord.z << ")\n";
        }
    }
    Check(highAltStatefulCount == highAltStatefulPredicted.size(),
        "high-alt stateful predicted collector reports returned coord count");
    Check(highAltPureCount == highAltPurePredicted.size(),
        "high-alt pure predicted collector reports returned coord count");
    Check(highAltPurePredicted == highAltStatefulPredicted,
        "high-alt pure predicted collector matches stateful predicted candidate order");
    Check(!highAltPureResidentTouches.empty(),
        "high-alt pure predicted collector exposes resident touch candidates");
    lookAheadCache.PumpGeneration(128u, 1u, lookAheadPolicy);
    SparseClipmapGpuSnapshot lookAheadSnapshot;
    Check(lookAheadCache.BuildGpuSnapshot(lookAheadSnapshot),
        "clipmap lookahead snapshot builds");
    bool hasForwardHeightTile = false;
    bool hasViewFanHeightTile = false;
    for (size_t i = 4; i + 3u < lookAheadSnapshot.metadata.size(); i += 4u) {
        if (lookAheadSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(lookAheadSnapshot.metadata[i + 0u]);
        const int32_t originZ = static_cast<int32_t>(lookAheadSnapshot.metadata[i + 1u]);
        hasForwardHeightTile = hasForwardHeightTile || originZ > 0;
        hasViewFanHeightTile = hasViewFanHeightTile || (std::abs(originX) > 0 && originZ > 0);
    }
    Check(hasForwardHeightTile,
        "clipmap lookahead queues height tiles ahead of the camera");
    Check(hasViewFanHeightTile,
        "clipmap lookahead queues lateral height fan tiles for visible valley walls");
    bool hasForwardVoxelBrick = false;
    bool hasViewFanVoxelBrick = false;
    for (size_t i = 4; i + 3u < lookAheadSnapshot.voxelMetadata.size(); i += 4u) {
        if ((lookAheadSnapshot.voxelMetadata[i + 3u] & 0xFFu) == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 0u] == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 1u] == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 2u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(lookAheadSnapshot.voxelMetadata[i + 0u]);
        const int32_t originZ = static_cast<int32_t>(lookAheadSnapshot.voxelMetadata[i + 2u]);
        hasForwardVoxelBrick = hasForwardVoxelBrick || originZ > 0;
        hasViewFanVoxelBrick = hasViewFanVoxelBrick || (std::abs(originX) > 0 && originZ > 0);
    }
    Check(hasForwardVoxelBrick,
        "clipmap lookahead queues voxel bricks ahead of the camera");
    Check(hasViewFanVoxelBrick,
        "clipmap lookahead queues lateral voxel fan bricks for visible valley walls");

    std::vector<SparseVoxelClipmapCoord> currentMissingBeforePrediction;
    lookAheadCache.CollectMissingVoxelInterest(currentMissingBeforePrediction);
    sortVoxelCoords(currentMissingBeforePrediction);
    const uint32_t currentInterestedBeforePrediction =
        lookAheadCache.GetStats().interestedVoxelBricks;
    const uint32_t currentMissingBeforePredictionCount =
        lookAheadCache.GetStats().missingInterestedVoxelBricks;
    (void)lookAheadCache.QueuePredictedVisibleVoxelInterest(
        0.0f,
        128.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        2u,
        lookAheadPolicy,
        32u,
        5u,
        0.0f,
        4u,
        1u);
    std::vector<SparseVoxelClipmapCoord> currentMissingAfterPrediction;
    lookAheadCache.CollectMissingVoxelInterest(currentMissingAfterPrediction);
    sortVoxelCoords(currentMissingAfterPrediction);
    Check(lookAheadCache.GetStats().interestedVoxelBricks == currentInterestedBeforePrediction,
        "clipmap predicted visible admission preserves current voxel interest size");
    Check(lookAheadCache.GetStats().missingInterestedVoxelBricks == currentMissingBeforePredictionCount,
        "clipmap predicted visible admission preserves current missing voxel interest count");
    Check(currentMissingAfterPrediction == currentMissingBeforePrediction,
        "clipmap predicted visible admission preserves current missing voxel interest set");

    SparseClipmapConfig sidewaysConfig = config;
    sidewaysConfig.maxTiles = 32;
    sidewaysConfig.maxVoxelBricks = 96;
    sidewaysConfig.ringCount = 1;
    sidewaysConfig.tileRadius = 1;
    sidewaysConfig.voxelBrickRadiusXz = 2;
    sidewaysConfig.motionLookaheadSteps = 4;
    sidewaysConfig.motionLookaheadMinSpeed = 32.0f;
    SparseClipmapPolicy sidewaysPolicy(sidewaysConfig);
    SparseClipmapTileCache sidewaysCache;
    Check(sidewaysCache.Initialize(sidewaysPolicy.Config()),
        "clipmap sideways motion cache initializes");
    sidewaysCache.UpdateInterest(
        0.0f,
        128.0f,
        0.0f,
        1u,
        sidewaysPolicy,
        0.0f,
        0.0f,
        1.0f,
        512.0f,
        0.0f,
        0.0f,
        0.75f);
    sidewaysCache.PumpGeneration(16u, 1u, sidewaysPolicy);
    SparseClipmapGpuSnapshot sidewaysSnapshot;
    Check(sidewaysCache.BuildGpuSnapshot(sidewaysSnapshot),
        "clipmap sideways motion snapshot builds");
    bool hasSidewaysHeightTile = false;
    for (size_t i = 4; i + 3u < sidewaysSnapshot.metadata.size(); i += 4u) {
        if (sidewaysSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(sidewaysSnapshot.metadata[i + 0u]);
        hasSidewaysHeightTile = hasSidewaysHeightTile || originX > 0;
    }
    Check(hasSidewaysHeightTile,
        "clipmap motion lookahead queues height tiles along velocity independent of view");
    bool hasSidewaysVoxelBrick = false;
    for (size_t i = 4; i + 3u < sidewaysSnapshot.voxelMetadata.size(); i += 4u) {
        if ((sidewaysSnapshot.voxelMetadata[i + 3u] & 0xFFu) == 0u &&
            sidewaysSnapshot.voxelMetadata[i + 0u] == 0u &&
            sidewaysSnapshot.voxelMetadata[i + 1u] == 0u &&
            sidewaysSnapshot.voxelMetadata[i + 2u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(sidewaysSnapshot.voxelMetadata[i + 0u]);
        hasSidewaysVoxelBrick = hasSidewaysVoxelBrick || originX > 0;
    }
    Check(hasSidewaysVoxelBrick,
        "clipmap motion lookahead queues voxel bricks along velocity independent of view");

    cache.UpdateInterest(4096.0f, 128.0f, 4096.0f, 80u, policy);
    cache.PumpGeneration(8u, 80u, policy);
    Check(cache.GetStats().residentTiles <= config.maxTiles, "clipmap tile cache respects max tile count");
    Check(cache.GetStats().evictedTilesLastFrame > 0, "clipmap tile cache evicts old tiles when full");

    SparseClipmapTileCache staleQueueCache;
    Check(staleQueueCache.Initialize(policy.Config()), "clipmap stale-queue test cache initializes");
    staleQueueCache.UpdateInterest(0.0f, 128.0f, 0.0f, 1u, policy);
    const uint32_t originQueued = staleQueueCache.GetStats().queuedTiles;
    staleQueueCache.UpdateInterest(8192.0f, 128.0f, 8192.0f, 2u, policy);
    const uint32_t generatedAfterMove = staleQueueCache.PumpGeneration(originQueued, 2u, policy);
    SparseClipmapGpuSnapshot staleSnapshot;
    Check(generatedAfterMove > 0, "clipmap cache still generates after camera interest moves");
    Check(staleQueueCache.BuildGpuSnapshot(staleSnapshot), "clipmap moved-interest snapshot builds");
    for (size_t i = 4; i + 3u < staleSnapshot.metadata.size(); i += 4u) {
        if (staleSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originX = static_cast<int32_t>(staleSnapshot.metadata[i + 0u]);
        const int32_t originZ = static_cast<int32_t>(staleSnapshot.metadata[i + 1u]);
        Check(originX > 1024 && originZ > 1024,
            "clipmap generation skips stale origin tiles after fast interest move");
    }
}

bool SnapshotHasVoxelCoord(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseVoxelClipmapCoord& target)
{
    for (size_t i = 0; i + 3u < snapshot.voxelLookup.size(); i += 4u) {
        const uint32_t packed = snapshot.voxelLookup[i + 3u];
        if (packed == 0u) {
            continue;
        }
        const int32_t ring = static_cast<int32_t>(packed >> 24u);
        const int32_t x = static_cast<int32_t>(snapshot.voxelLookup[i + 0u]);
        const int32_t y = static_cast<int32_t>(snapshot.voxelLookup[i + 1u]);
        const int32_t z = static_cast<int32_t>(snapshot.voxelLookup[i + 2u]);
        if (ring == target.ring && x == target.x && y == target.y && z == target.z) {
            return true;
        }
    }
    return false;
}

uint32_t ShaderStyleMidVoxelLookupProbeDistance(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseVoxelClipmapCoord& target)
{
    if (snapshot.voxelLookupCapacity == 0u ||
        (snapshot.voxelLookupCapacity & (snapshot.voxelLookupCapacity - 1u)) != 0u) {
        return UINT32_MAX;
    }
    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(target.ring)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(target.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(target.y)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(target.z)) * 16777619u;
    uint32_t slot = hash & (snapshot.voxelLookupCapacity - 1u);
    for (uint32_t probe = 0; probe < snapshot.voxelLookupCapacity; ++probe) {
        const size_t base = static_cast<size_t>(slot) * 4u;
        if (snapshot.voxelLookup[base + 3u] == 0u) {
            return UINT32_MAX;
        }
        const int32_t ring = static_cast<int32_t>(snapshot.voxelLookup[base + 3u] >> 24u);
        const int32_t x = static_cast<int32_t>(snapshot.voxelLookup[base + 0u]);
        const int32_t y = static_cast<int32_t>(snapshot.voxelLookup[base + 1u]);
        const int32_t z = static_cast<int32_t>(snapshot.voxelLookup[base + 2u]);
        if (ring == target.ring && x == target.x && y == target.y && z == target.z) {
            return probe;
        }
        slot = (slot + 1u) & (snapshot.voxelLookupCapacity - 1u);
    }
    return UINT32_MAX;
}

void DumpVoxelNeighborhoodForTest(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseVoxelClipmapCoord& target)
{
    std::cerr << "  nearby resident voxel coords for target ring="
              << target.ring << " coord=("
              << target.x << "," << target.y << "," << target.z << "):";
    uint32_t count = 0;
    for (size_t i = 0; i + 3u < snapshot.voxelLookup.size(); i += 4u) {
        const uint32_t packed = snapshot.voxelLookup[i + 3u];
        if (packed == 0u) {
            continue;
        }
        const int32_t ring = static_cast<int32_t>(packed >> 24u);
        const int32_t x = static_cast<int32_t>(snapshot.voxelLookup[i + 0u]);
        const int32_t y = static_cast<int32_t>(snapshot.voxelLookup[i + 1u]);
        const int32_t z = static_cast<int32_t>(snapshot.voxelLookup[i + 2u]);
        if (ring != target.ring ||
            std::abs(x - target.x) > 2 ||
            std::abs(y - target.y) > 2 ||
            std::abs(z - target.z) > 2) {
            continue;
        }
        std::cerr << " (" << x << "," << y << "," << z << ")";
        if (++count >= 24u) {
            break;
        }
    }
    std::cerr << '\n';
}

struct TestVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

TestVec3 operator+(const TestVec3& lhs, const TestVec3& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

TestVec3 operator*(const TestVec3& lhs, double scale) {
    return {lhs.x * scale, lhs.y * scale, lhs.z * scale};
}

double Dot(const TestVec3& lhs, const TestVec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

double Length(const TestVec3& value) {
    return std::sqrt(Dot(value, value));
}

TestVec3 Normalize(const TestVec3& value) {
    const double len = Length(value);
    if (len <= 0.000001) {
        return {0.0, 1.0, 0.0};
    }
    return {value.x / len, value.y / len, value.z / len};
}

double MidTestCellSize(uint32_t ring, const SparseClipmapConfig& config) {
    return static_cast<double>(config.minCellSize) * static_cast<double>(1u << ring);
}

bool SampleSnapshotMidVoxel(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseClipmapConfig& config,
    const TestVec3& worldPos,
    uint32_t ring,
    uint32_t& outVoxel)
{
    outVoxel = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    if (snapshot.voxelMetadata.size() < 4u ||
        snapshot.voxelLookupCapacity == 0u ||
        ring >= config.ringCount) {
        return false;
    }
    const uint32_t brickCount = snapshot.voxelMetadata[2];
    const double cellSize = MidTestCellSize(ring, config);
    const double brickWorldSize = cellSize * static_cast<double>(SPARSE_BRICK_SIZE);
    const SparseVoxelClipmapCoord coord{
        static_cast<int32_t>(ring),
        static_cast<int32_t>(std::floor(worldPos.x / brickWorldSize)),
        static_cast<int32_t>(std::floor(worldPos.y / brickWorldSize)),
        static_cast<int32_t>(std::floor(worldPos.z / brickWorldSize))
    };
    const uint32_t probeDistance = ShaderStyleMidVoxelLookupProbeDistance(snapshot, coord);
    if (probeDistance >= 8u) {
        return false;
    }

    uint32_t hash = 2166136261u;
    hash = (hash ^ static_cast<uint32_t>(coord.ring)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.x)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.y)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(coord.z)) * 16777619u;
    uint32_t slot = hash & (snapshot.voxelLookupCapacity - 1u);
    uint32_t brickIndex = UINT32_MAX;
    for (uint32_t probe = 0; probe < 8u; ++probe) {
        const size_t base = static_cast<size_t>(slot) * 4u;
        const uint32_t packed = snapshot.voxelLookup[base + 3u];
        if (packed == 0u) {
            return false;
        }
        const int32_t entryRing = static_cast<int32_t>(packed >> 24u);
        if (entryRing == coord.ring &&
            static_cast<int32_t>(snapshot.voxelLookup[base + 0u]) == coord.x &&
            static_cast<int32_t>(snapshot.voxelLookup[base + 1u]) == coord.y &&
            static_cast<int32_t>(snapshot.voxelLookup[base + 2u]) == coord.z) {
            brickIndex = (packed & 0x00FFFFFFu) - 1u;
            break;
        }
        slot = (slot + 1u) & (snapshot.voxelLookupCapacity - 1u);
    }
    if (brickIndex == UINT32_MAX || brickIndex >= brickCount) {
        return false;
    }
    const size_t metadataBase = static_cast<size_t>(brickIndex + 1u) * 4u;
    if (metadataBase + 3u >= snapshot.voxelMetadata.size()) {
        return false;
    }
    const int32_t originX = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 0u]);
    const int32_t originY = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 1u]);
    const int32_t originZ = static_cast<int32_t>(snapshot.voxelMetadata[metadataBase + 2u]);
    const double storedCellSize = static_cast<double>(snapshot.voxelMetadata[metadataBase + 3u] >> 8u);
    if (storedCellSize < 1.0) {
        return false;
    }
    const double localX = (worldPos.x - static_cast<double>(originX)) / storedCellSize;
    const double localY = (worldPos.y - static_cast<double>(originY)) / storedCellSize;
    const double localZ = (worldPos.z - static_cast<double>(originZ)) / storedCellSize;
    if (localX < 0.0 || localY < 0.0 || localZ < 0.0 ||
        localX >= static_cast<double>(SPARSE_BRICK_SIZE) ||
        localY >= static_cast<double>(SPARSE_BRICK_SIZE) ||
        localZ >= static_cast<double>(SPARSE_BRICK_SIZE)) {
        return false;
    }
    const uint32_t lx = static_cast<uint32_t>(localX);
    const uint32_t ly = static_cast<uint32_t>(localY);
    const uint32_t lz = static_cast<uint32_t>(localZ);
    const size_t sampleIndex =
        static_cast<size_t>(brickIndex) * SPARSE_BRICK_VOXEL_COUNT +
        lx + ly * SPARSE_BRICK_SIZE + lz * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
    if (sampleIndex >= snapshot.voxelSamples.size()) {
        return false;
    }
    outVoxel = snapshot.voxelSamples[sampleIndex];
    return true;
}

bool SampleSnapshotMidVoxelFallback(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseClipmapConfig& config,
    const TestVec3& worldPos,
    uint32_t preferredRing,
    uint32_t& outVoxel,
    uint32_t& outRing,
    double& outCellSize)
{
    outVoxel = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    outRing = std::min(preferredRing, config.ringCount - 1u);
    outCellSize = MidTestCellSize(outRing, config);
    if (SampleSnapshotMidVoxel(snapshot, config, worldPos, outRing, outVoxel)) {
        return true;
    }
    for (uint32_t offset = 1u; offset <= outRing; ++offset) {
        const uint32_t finerRing = outRing - offset;
        if (SampleSnapshotMidVoxel(snapshot, config, worldPos, finerRing, outVoxel)) {
            outRing = finerRing;
            outCellSize = MidTestCellSize(outRing, config);
            return true;
        }
    }
    for (uint32_t coarserRing = outRing + 1u; coarserRing < config.ringCount; ++coarserRing) {
        if (SampleSnapshotMidVoxel(snapshot, config, worldPos, coarserRing, outVoxel)) {
            outRing = coarserRing;
            outCellSize = MidTestCellSize(outRing, config);
            return true;
        }
    }
    return false;
}

bool IsSnapshotMidVoxelAirOrMissing(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseClipmapConfig& config,
    const TestVec3& worldPos,
    uint32_t ring)
{
    uint32_t voxel = 0;
    if (!SampleSnapshotMidVoxel(snapshot, config, worldPos, ring, voxel)) {
        return true;
    }
    return VENPOD::Utils::UnpackMaterial(voxel) == VENPOD::Utils::Material::Air;
}

bool IsSnapshotMidVoxelExposed(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseClipmapConfig& config,
    const TestVec3& worldPos,
    uint32_t ring,
    double cellSize,
    TestVec3& normal)
{
    normal = {0.0, 0.0, 0.0};
    const TestVec3 dx{cellSize, 0.0, 0.0};
    const TestVec3 dy{0.0, cellSize, 0.0};
    const TestVec3 dz{0.0, 0.0, cellSize};
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dx, ring)) normal.x += 1.0;
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dx * -1.0, ring)) normal.x -= 1.0;
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dy, ring)) normal.y += 1.0;
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dy * -1.0, ring)) normal.y -= 1.0;
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dz, ring)) normal.z += 1.0;
    if (IsSnapshotMidVoxelAirOrMissing(snapshot, config, worldPos + dz * -1.0, ring)) normal.z -= 1.0;
    const double normalLength = Length(normal);
    if (normalLength <= 0.001) {
        normal = {0.0, 1.0, 0.0};
        return false;
    }
    normal = normal * (1.0 / normalLength);
    return true;
}

double NextMidVoxelCellBoundaryTCpu(const TestVec3& rayOrigin, const TestVec3& rayDir, double currentT, double cellSize) {
    const TestVec3 pos = rayOrigin + rayDir * currentT;
    const TestVec3 cell{
        std::floor(pos.x / cellSize),
        std::floor(pos.y / cellSize),
        std::floor(pos.z / cellSize)
    };
    double nextT = 1.0e20;
    if (std::abs(rayDir.x) > 0.0001) {
        const double boundaryX = ((rayDir.x > 0.0) ? (cell.x + 1.0) : cell.x) * cellSize;
        const double tx = (boundaryX - rayOrigin.x) / rayDir.x;
        if (tx > currentT + 0.01) {
            nextT = std::min(nextT, tx);
        }
    }
    if (std::abs(rayDir.y) > 0.0001) {
        const double boundaryY = ((rayDir.y > 0.0) ? (cell.y + 1.0) : cell.y) * cellSize;
        const double ty = (boundaryY - rayOrigin.y) / rayDir.y;
        if (ty > currentT + 0.01) {
            nextT = std::min(nextT, ty);
        }
    }
    if (std::abs(rayDir.z) > 0.0001) {
        const double boundaryZ = ((rayDir.z > 0.0) ? (cell.z + 1.0) : cell.z) * cellSize;
        const double tz = (boundaryZ - rayOrigin.z) / rayDir.z;
        if (tz > currentT + 0.01) {
            nextT = std::min(nextT, tz);
        }
    }
    if (nextT >= 1.0e19) {
        return currentT + std::max(cellSize, 4.0);
    }
    return std::max(nextT + 0.02, currentT + 0.05);
}

bool CpuMidVoxelDdaHits(
    const SparseClipmapGpuSnapshot& snapshot,
    const SparseClipmapConfig& config,
    const TestVec3& rayOrigin,
    const TestVec3& rayDir,
    double& hitT,
    std::string& reason)
{
    double t = 1800.0;
    bool previousWasAir = false;
    for (int step = 0; step < 176 && t < config.endDistance; ++step) {
        const double normalized = std::clamp((t - config.startDistance) / (config.endDistance - config.startDistance), 0.0, 0.9999);
        const uint32_t preferredRing = std::min<uint32_t>(
            static_cast<uint32_t>(std::floor(normalized * static_cast<double>(config.ringCount))),
            config.ringCount - 1u);
        const TestVec3 pos = rayOrigin + rayDir * t;
        uint32_t voxel = 0;
        uint32_t actualRing = preferredRing;
        double actualCellSize = MidTestCellSize(actualRing, config);
        if (!SampleSnapshotMidVoxelFallback(snapshot, config, pos, preferredRing, voxel, actualRing, actualCellSize)) {
            previousWasAir = true;
            t += std::max(MidTestCellSize(preferredRing, config) * 1.55, 12.0);
            continue;
        }
        const double nextT = NextMidVoxelCellBoundaryTCpu(rayOrigin, rayDir, t, actualCellSize);
        const uint8_t material = VENPOD::Utils::UnpackMaterial(voxel);
        if (material == VENPOD::Utils::Material::Air) {
            previousWasAir = true;
            t = std::min(nextT, t + std::max(actualCellSize, 4.0));
            continue;
        }
        const bool taggedSurface =
            (VENPOD::Utils::UnpackState(voxel) & VENPOD::Utils::StateFlags::VisualSurface) != 0u;
        TestVec3 normal;
        const bool exposedSurface = IsSnapshotMidVoxelExposed(
            snapshot,
            config,
            pos,
            actualRing,
            actualCellSize,
            normal);
        const bool allowVoxelOnlyInteriorFallback = rayDir.y < 0.12 && t >= 1024.0;
        if (!exposedSurface && !taggedSurface && !previousWasAir && !allowVoxelOnlyInteriorFallback) {
            reason = "solid_sample_rejected_not_surface";
            t = std::min(nextT, t + std::max(actualCellSize, 4.0));
            continue;
        }
        if (!exposedSurface) {
            normal = Normalize({-rayDir.x, std::max(std::abs(rayDir.y), 0.35), -rayDir.z});
        }
        const double surfaceMinNormalY = previousWasAir ? -0.48 : -0.18;
        if (normal.y < surfaceMinNormalY) {
            reason = "solid_sample_rejected_normal_y_" + std::to_string(normal.y);
            t = std::min(nextT, t + std::max(actualCellSize, 4.0));
            continue;
        }
        if (taggedSurface || exposedSurface || previousWasAir || allowVoxelOnlyInteriorFallback) {
            hitT = t;
            reason = taggedSurface ? "tagged_surface" : (previousWasAir ? "ray_entry_surface" : "exposed_surface");
            return true;
        }
        t = std::min(nextT, t + std::max(actualCellSize, 4.0));
    }
    reason = "budget_or_range_exhausted";
    return false;
}

void TestSparseClipmapViewCorridorCoversFrame300Skyline() {
    SparseClipmapConfig config;
    config.enabled = true;
    config.heightClipmapEnabled = true;
    config.voxelClipmapEnabled = true;
    config.startDistance = 1024.0f;
    config.endDistance = 6400.0f;
    config.minCellSize = 12.0f;
    config.nearExitPadding = 12.0f;
    config.ringCount = 4u;
    config.tileRadius = 3u;
    config.tileSampleSide = 33u;
    config.maxTiles = 256u;
    config.voxelBrickRadiusXz = 8u;
    config.voxelBrickRadiusY = 4u;
    config.maxVoxelBricks = 12288u;
    config.voxelInterestCapacityPercent = 75u;
    config.motionLookaheadMinSpeed = 64.0f;
    config.motionLookaheadSteps = 3u;
    config.interestUpdateIntervalFrames = 1u;
    config.seed = 12345u;

    SparseClipmapPolicy policy(config);
    SparseClipmapTileCache cache;
    Check(cache.Initialize(config), "frame300 clipmap view corridor cache initializes");
    cache.UpdateInterest(
        192.5f,
        43.0f,
        256.5f,
        300u,
        policy,
        0.0f,
        -0.04f,
        0.999f,
        0.0f,
        0.0f,
        0.0f,
        0.0f);
    cache.PumpGeneration(0u, config.maxVoxelBricks, 300u, policy);

    SparseClipmapGpuSnapshot snapshot;
    Check(cache.BuildGpuSnapshot(snapshot, false, true),
        "frame300 clipmap view corridor voxel snapshot builds");

    const SparseVoxelClipmapCoord targets[] = {
        {0, -4, 1, 11},
        {0, -4, 1, 12},
        {0, -6, 1, 10},
    };
    SparseTerrainGenerator terrain(config.seed);
    for (const SparseVoxelClipmapCoord& target : targets) {
        const float brickWorldSize = config.minCellSize * static_cast<float>(SPARSE_BRICK_SIZE);
        const int32_t sampleX = static_cast<int32_t>(std::floor((static_cast<float>(target.x) + 0.5f) * brickWorldSize));
        const int32_t sampleZ = static_cast<int32_t>(std::floor((static_cast<float>(target.z) + 0.5f) * brickWorldSize));
        const float terrainY = terrain.HeightAt(sampleX, sampleZ);
        const int32_t terrainCenterY = static_cast<int32_t>(std::floor(terrainY / brickWorldSize));
        const std::string message =
            "frame300 skyline target mid voxel brick is resident in view corridor ring=" +
            std::to_string(target.ring) +
            " coord=(" +
            std::to_string(target.x) + "," +
            std::to_string(target.y) + "," +
            std::to_string(target.z) + ") terrainCenterY=" +
            std::to_string(terrainCenterY) +
            " height=" +
            std::to_string(terrainY);
        const bool hasTarget = SnapshotHasVoxelCoord(snapshot, target);
        if (!hasTarget) {
            DumpVoxelNeighborhoodForTest(snapshot, target);
        }
        Check(hasTarget, message.c_str());
        if (hasTarget) {
            const uint32_t probeDistance = ShaderStyleMidVoxelLookupProbeDistance(snapshot, target);
            const std::string probeMessage =
                "frame300 skyline target mid voxel brick is reachable by shader lookup probe ring=" +
                std::to_string(target.ring) +
                " coord=(" +
                std::to_string(target.x) + "," +
                std::to_string(target.y) + "," +
                std::to_string(target.z) + ") probe=" +
                std::to_string(probeDistance);
            Check(probeDistance < 8u, probeMessage.c_str());
        }
    }

    const TestVec3 rayOrigin{192.5, 43.0, 256.5};
    const TestVec3 rays[] = {
        Normalize({-0.395, 0.120, 0.911}),
        Normalize({-0.369, 0.114, 0.922}),
        Normalize({-0.575, 0.087, 0.814}),
    };
    for (const TestVec3& ray : rays) {
        double hitT = 0.0;
        std::string reason;
        const bool hit = CpuMidVoxelDdaHits(snapshot, config, rayOrigin, ray, hitT, reason);
        if (!hit) {
            std::cerr << "  frame300 skyline CPU mid DDA miss reason=" << reason << '\n';
        }
        Check(hit, "frame300 skyline resident mid voxel DDA hits target ray corridor");
    }
}

} // namespace

int main() {
    TestBackendParsing();
    TestVoxelWorldRaycastReadbackLifecycle();
    TestFarVoxelOctreeResidencyMetadata();
    TestSparseSurfaceIaStreamSizing();
    TestSparseSurfaceGpuConfigValidation();
    TestSparseSurfaceGpuCopyRangeValidation();
    TestSparseSurfaceGpuAbiLayout();
    TestSparseVoxelGpuConfigValidation();
    TestSparseVoxelGpuCopyRangeValidation();
    TestCoordinateConversion();
    TestGpuPageEntryLayout();
    TestPageTable();
    TestBrickPool();
    TestTerrainGeneration();
    TestSparseSurfaceExtraction();
    TestSparseSurfaceCache();
    TestSparseSurfaceClusterRecords();
    TestSparseSurfaceRangeAllocator();
    TestSparsePagePublishQueue();
    TestEditStoreAndCollision();
    TestSparseCollisionVolumesAndSweeps();
    TestSparseCharacterController();
    TestSparseEditDeltaBatching();
    TestSparseCollisionSupportRequests();
    TestSparseGpuPhysicsProposalApply();
    TestSparseVoxelWorldLifecycle();
    TestSparseFixedGridReadiness();
    TestSparseRenderDirtyRegions();
    TestSparseLocalPhysics();
    TestSparseBrushEditSemantics();
    TestSparseRaycast();
    TestSparseVoxelWorldEviction();
    TestSparsePriorityReplacement();
    TestSparsePriorityQueues();
    TestSparseBrickRequestPlanner();
    TestSparseRuntimeBudgetScheduler();
    TestSparseClipmapPolicy();
    TestSparseClipmapTileCache();
    TestSparseClipmapViewCorridorCoversFrame300Skyline();

    if (failures != 0) {
        std::cerr << failures << " sparse core test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "Sparse core tests passed\n";
    return EXIT_SUCCESS;
}
