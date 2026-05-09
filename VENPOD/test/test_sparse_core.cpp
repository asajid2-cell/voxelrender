#include "Graphics/FarVoxelOctree.h"
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
#include "Utils/BitPacking.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

using VENPOD::Graphics::ParseVoxelRenderBackend;
using VENPOD::Graphics::ToString;
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
    };

    for (const auto& c : cases) {
        Check(FloorDiv(c.world, SPARSE_BRICK_SIZE) == c.brick, "FloorDiv sparse brick case");
        Check(FloorMod(c.world, SPARSE_BRICK_SIZE) == c.local, "FloorMod sparse brick case");
    }

    BrickCoord brick = BrickCoord::FromWorldVoxel(-1, -16, -17);
    Check(brick == BrickCoord{-1, -1, -2}, "negative world voxel to brick coord");

    LocalVoxelCoord local = LocalVoxelFromWorld(-1, -16, -17);
    Check(local == LocalVoxelCoord{15, 0, 15}, "negative world voxel to local coord");

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

    Check(sizeof(SparseEditDeltaRange) == 24, "SparseEditDeltaRange GPU layout is 24 bytes");
    Check(offsetof(SparseEditDeltaRange, coord) == 0, "SparseEditDeltaRange coord offset");
    Check(offsetof(SparseEditDeltaRange, firstDelta) == 12, "SparseEditDeltaRange firstDelta offset");
    Check(offsetof(SparseEditDeltaRange, deltaCount) == 16, "SparseEditDeltaRange deltaCount offset");
    Check(offsetof(SparseEditDeltaRange, latestRevision) == 20, "SparseEditDeltaRange latestRevision offset");

    Check(sizeof(SparsePhysicsPacketResult) == 80, "SparsePhysicsPacketResult GPU layout is 80 bytes");
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

    Check(table.Remove({0, 0, 0}), "remove origin page");
    Check(!table.TryLookup({0, 0, 0}), "removed page missing");
    Check(table.TryLookup({-1, 2, -3}), "tombstone does not break probe chain");
}

void TestBrickPool() {
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

    const auto spawn = terrain.FindScenicSpawn(96, 96, 6.0f);
    Check(spawn.found, "scenic sparse spawn finds a validated spawn near origin");
    Check(spawn.groundY > SEA_LEVEL_Y + 6, "scenic sparse spawn avoids water basin starts");
    Check(spawn.eyeY > static_cast<float>(spawn.groundY) + 6.0f,
        "scenic sparse spawn places eye above player clearance");
    Check(spawn.localRelief <= 118.0f,
        "scenic sparse spawn avoids high-relief wall pockets");
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
            Check(spawn.eyeY - terrain.HeightAt(sx, sz) >= 14.0f,
                "scenic sparse spawn view cone avoids immediate wall");
        }
    }
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
}

void TestSparseCharacterController() {
    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);

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
    Check(rangeCapped.ranges.size() == 1, "edit delta range cap limits uploaded ranges");
    Check(rangeCapped.deltas.size() == rangeCapped.ranges[0].deltaCount,
        "edit delta overflow keeps range and delta arrays consistent");

    SparseEditDeltaBatch deltaCapped = BuildSparseEditDeltaBatch(deltas, 2, 16);
    Check(deltaCapped.overflow, "edit delta batch reports delta overflow");
    Check(deltaCapped.deltas.size() == 2, "edit delta cap limits uploaded deltas");
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
    constexpr uint32_t kProposalStatusHasExpectedPage = 2u;
    constexpr uint32_t kProposalStatusPageMatch = 4u;
    constexpr uint32_t kProposalStatusPageStale = 8u;
    constexpr uint32_t kProposalStatusProposal = 16u;
    constexpr uint32_t kProposalStatusEditDeltaHit = 64u;

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
    proposal.status = 16u;
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

    SparseVoxelWorld editRevisionWorld;
    Check(editRevisionWorld.Initialize({8, 32, 12345u}),
        "gpu proposal edit-revision world initialize");
    editRevisionWorld.SetEditedVoxel(0, 17, 0, sand);
    SparsePhysicsPacketResult editRevisionProposal = proposal;
    editRevisionProposal.status = kProposalStatusProposal | kProposalStatusEditDeltaHit;
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
    destinationRevisionProposal.status = kProposalStatusProposal | kProposalStatusEditDeltaHit;
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
    expectedPageProposal.materialMask = 1u;
    expectedPageProposal.status =
        kProposalStatusHasExpectedPage | kProposalStatusPageMatch | kProposalStatusProposal;
    expectedPageProposal.expectedPageIndex = residencyPacket.pageIndex;
    expectedPageProposal.expectedPageGeneration = residencyPacket.generation;
    expectedPageProposal.packedSourceLocal = 0u | (1u << 8u) | 0u;
    expectedPageProposal.packedDestinationLocal = 0u | (0u << 8u) | 0u;
    expectedPageProposal.sourceVoxel = sand;
    expectedPageProposal.destinationVoxel = air;

    SparsePhysicsPacketResult stalePageProposal = expectedPageProposal;
    stalePageProposal.status =
        kProposalStatusHasExpectedPage | kProposalStatusPageStale | kProposalStatusProposal;
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

    SparsePhysicsPacketResult boundaryProposal;
    boundaryProposal.coord = BrickCoord{0, 1, 0};
    boundaryProposal.destinationCoord = BrickCoord{0, 0, 0};
    boundaryProposal.materialMask = 1u;
    boundaryProposal.status = 16u;
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
    missingSupportProposal.status = 16u | 32u;
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

    SparseVoxelWorld lateralWorld;
    Check(lateralWorld.Initialize({8, 32, 12345u}), "gpu lateral proposal world initialize");
    const uint32_t stone = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Stone, 0, 0, 0);
    lateralWorld.SetEditedVoxel(1, 17, 1, water);
    lateralWorld.SetEditedVoxel(1, 16, 1, stone);
    lateralWorld.SetEditedVoxel(2, 17, 1, air);
    SparsePhysicsPacketResult lateralProposal;
    lateralProposal.coord = BrickCoord{0, 1, 0};
    lateralProposal.destinationCoord = BrickCoord{0, 1, 0};
    lateralProposal.materialMask = 2u;
    lateralProposal.status = 16u;
    lateralProposal.packedSourceLocal = 1u | (1u << 8u) | (1u << 16u);
    lateralProposal.packedDestinationLocal = 2u | (1u << 8u) | (1u << 16u);
    lateralProposal.sourceVoxel = water;
    lateralProposal.destinationVoxel = air;
    Check(lateralWorld.ApplyGpuPhysicsProposals({lateralProposal}, 4, false) == 1,
        "gpu lateral fluid proposal is CPU-authoritatively applied");
    Check(lateralWorld.GetEdits().TryGetVoxel(2, 17, 1, &destinationAfter) &&
          destinationAfter == water,
        "gpu lateral fluid proposal writes destination voxel");
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
    Check(emptyUploadWorld.GetStats().knownEmptyGeneratedBricks == 0,
        "editing a known empty brick invalidates the empty cache entry");
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

    SparseSurfaceFaceAllocation grown;
    Check(allocator.AllocateOrResize(BrickCoord{0, 0, 0}, 40, &grown),
        "surface range allocator grows into new range");
    Check(grown.firstFace >= b.firstFace + b.capacity && grown.capacity == 40,
        "surface range allocator moved grown allocation");
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
            SparseDelayedInvalidationDecision::Stage,
        "delayed invalidation stages reused slots while a replacement publish is pending");
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
    Check(backgroundWorld.RequestBrick(bgCenter), "background trim request center");
    Check(backgroundWorld.RequestBrick(bgSpeculative), "background trim request speculative");
    Check(backgroundWorld.RequestBrick(bgVisible), "background trim request visible");
    Check(backgroundWorld.RequestBrick(bgCollision), "background trim request collision");
    Check(backgroundWorld.RequestBrick(bgEdited), "background trim request edited");
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
}

void TestSparsePriorityReplacement() {
    SparseVoxelWorld world;
    Check(world.Initialize({4, 16, 12345u}), "priority replacement world initialize");

    const BrickCoord center{0, 0, 0};
    const BrickCoord speculativeFar{8, 0, 0};
    const BrickCoord visibleFar{9, 0, 0};
    const BrickCoord editedFar{10, 0, 0};
    Check(world.RequestBrick(center), "replacement request center");
    Check(world.RequestBrick(speculativeFar), "replacement request speculative");
    Check(world.RequestBrick(visibleFar), "replacement request visible");
    Check(world.RequestBrick(editedFar), "replacement request edited");
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
    Check(world.RequestBrick(newVisible), "replacement request can reuse freed page");
    Check(world.MarkResidencyClass(newVisible, SparseResidencyClass::Visible),
        "replacement new page marked visible");

    SparseVoxelWorld visibleWorld;
    Check(visibleWorld.Initialize({3, 16, 12345u}), "same-class replacement world initialize");
    const BrickCoord visibleA{0, 0, 0};
    const BrickCoord visibleB{6, 0, 0};
    const BrickCoord visibleC{12, 0, 0};
    Check(visibleWorld.RequestBrick(visibleA), "same-class request visible A");
    Check(visibleWorld.RequestBrick(visibleB), "same-class request visible B");
    Check(visibleWorld.RequestBrick(visibleC), "same-class request visible C");
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
    Check(ageWorld.RequestBrick(ageCenter), "age request center");
    Check(ageWorld.RequestBrick(oldVisible), "age request old visible");
    Check(ageWorld.RequestBrick(newVisibleSameDistance), "age request new visible");
    Check(ageWorld.RequestBrick(collisionTouched), "age request collision touched");
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
    missPlanInput.unsafeNearMissPercent = 5;
    missPlanInput.ownershipPressureLevel = 3;
    missPlan = SparseRuntimeBudgetScheduler::BuildMissFeedbackPlan(missPlanInput);
    Check(missPlan.dispatch && missPlan.rayGrid == 8 &&
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

    backgroundInput.backgroundPixelShare = 0.20f;
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
          backgroundBudget.farFieldQuality >= 0.62f,
        "background renderer preserves terrain ownership work during visible miss catch-up");

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
    Check(snapshot.samples.size() == static_cast<size_t>(config.maxTiles) * config.tileSampleSide * config.tileSampleSide,
        "clipmap snapshot sample capacity");
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
    Check(snapshot.voxelBrickCount > 0, "voxel clipmap generates coarse 3D bricks");
    Check(!snapshot.voxelMetadata.empty(), "voxel clipmap metadata exists");
    Check(!snapshot.voxelLookup.empty(), "voxel clipmap lookup exists");
    Check(!snapshot.voxelSamples.empty(), "voxel clipmap samples exist");
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
    lookAheadCache.PumpGeneration(128u, 1u, lookAheadPolicy);
    SparseClipmapGpuSnapshot lookAheadSnapshot;
    Check(lookAheadCache.BuildGpuSnapshot(lookAheadSnapshot),
        "clipmap lookahead snapshot builds");
    bool hasForwardHeightTile = false;
    for (size_t i = 4; i + 3u < lookAheadSnapshot.metadata.size(); i += 4u) {
        if (lookAheadSnapshot.metadata[i + 3u] == 0u) {
            continue;
        }
        const int32_t originZ = static_cast<int32_t>(lookAheadSnapshot.metadata[i + 1u]);
        hasForwardHeightTile = hasForwardHeightTile || originZ > 0;
    }
    Check(hasForwardHeightTile,
        "clipmap lookahead queues height tiles ahead of the camera");
    bool hasForwardVoxelBrick = false;
    for (size_t i = 4; i + 3u < lookAheadSnapshot.voxelMetadata.size(); i += 4u) {
        if ((lookAheadSnapshot.voxelMetadata[i + 3u] & 0xFFu) == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 0u] == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 1u] == 0u &&
            lookAheadSnapshot.voxelMetadata[i + 2u] == 0u) {
            continue;
        }
        const int32_t originZ = static_cast<int32_t>(lookAheadSnapshot.voxelMetadata[i + 2u]);
        hasForwardVoxelBrick = hasForwardVoxelBrick || originZ > 0;
    }
    Check(hasForwardVoxelBrick,
        "clipmap lookahead queues voxel bricks ahead of the camera");

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

} // namespace

int main() {
    TestBackendParsing();
    TestFarVoxelOctreeResidencyMetadata();
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

    if (failures != 0) {
        std::cerr << failures << " sparse core test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "Sparse core tests passed\n";
    return EXIT_SUCCESS;
}
