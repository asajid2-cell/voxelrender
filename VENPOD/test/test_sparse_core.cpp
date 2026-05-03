#include "Graphics/VoxelRenderBackend.h"
#include "Simulation/SparseBrickPool.h"
#include "Simulation/SparseBrickRequestPlanner.h"
#include "Simulation/SparseClipmap.h"
#include "Simulation/SparseCollision.h"
#include "Simulation/SparseEditStore.h"
#include "Simulation/SparsePageTable.h"
#include "Simulation/SparseSurfaceCache.h"
#include "Simulation/SparseSurfaceExtractor.h"
#include "Simulation/SparseTerrainGenerator.h"
#include "Simulation/SparseVoxelTypes.h"
#include "Simulation/SparseVoxelWorld.h"
#include "Utils/BitPacking.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>

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

void TestBackendParsing() {
    Check(ParseVoxelRenderBackend("") == VoxelRenderBackend::DenseLegacy, "empty backend defaults dense");
    Check(ParseVoxelRenderBackend("dense") == VoxelRenderBackend::DenseLegacy, "dense backend parse");
    Check(ParseVoxelRenderBackend("sparse") == VoxelRenderBackend::SparseBrick, "sparse backend parse");
    Check(ParseVoxelRenderBackend("sparse-brick") == VoxelRenderBackend::SparseBrick, "sparse-brick backend parse");
    Check(std::string(ToString(VoxelRenderBackend::SparseBrick)) == "sparse-brick", "sparse backend string");
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

    Check(HashBrickCoord32({0, 0, 0}) == 1253111735u, "hash origin matches shader contract");
    Check(HashBrickCoord32({1, -2, 3}) == 2804991279u, "hash mixed signed coord matches shader contract");
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

    Check(pool.MarkDirty(a), "mark dirty should be callable");
    Check(pool.GetState(a) == BrickLifecycleState::DirtyCPU, "resident dirty transition");
    Check(pool.QueueUpload(a), "queue dirty upload");
    Check(pool.BeginUpload(a), "begin dirty upload");
    Check(pool.PublishResident(a, 124), "republish dirty resident");

    Check(pool.FreePage(a), "free page A");
    Check(!pool.IsResident(a), "page A no longer resident");
    Check(!pool.PageTable().TryLookup(a), "page table invalidated before reuse");
    Check(pool.FreePageCount() == 3, "free page count after free");

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
}

void TestSparseVoxelWorldLifecycle() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "sparse world initialize");

    const BrickCoord coord{1, 2, -3};
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
    Check(world.GetStats().editedVoxels == 1, "world edit records one edited voxel");

    SparseBrickUploadPacket editPacket;
    Check(world.PopNextUpload(&editPacket), "pop dirty edit upload");
    Check(editPacket.coord == coord, "edit upload coord");
    Check(editPacket.generation == packet.generation, "dirty upload keeps generation before eviction");
    Check(world.CompleteUpload(editPacket), "complete dirty edit upload");
    Check(world.GetPool().IsResident(coord), "dirty upload returns to resident");
    Check(world.GetStats().surfaceCachedBricks == 1, "dirty upload keeps one cached surface brick");
    Check(world.GetStats().surfaceBricksUpdatedLastFrame >= 1,
        "dirty upload refreshes cached surface brick");

    world.BeginFrame();
    Check(world.GetStats().surfaceBricksUpdatedLastFrame == 0,
        "surface update counters reset at sparse world frame boundary");

    SparseVoxelWorld collisionOnly;
    Check(collisionOnly.Initialize({4, 16, 12345u}), "collision-only sparse world initialize");
    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    collisionOnly.SetEditedVoxel(900, 700, -900, stone);
    Check(collisionOnly.SampleCollisionStatus(900, 700, -900) == CollisionSampleStatus::KnownSolid,
        "collision samples persistent edit without render residency");
    Check(collisionOnly.GetPool().PageTable().Count() == 0,
        "collision edit did not require resident render page");
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

    GeneratedSparseBrick full;
    full.coord = BrickCoord{0, 0, 0};
    full.voxels.fill(stone);
    auto fullResult = SparseSurfaceExtractor::Extract(full);
    Check(fullResult.stats.solidVoxels == SPARSE_BRICK_VOXEL_COUNT,
        "full brick counts all solid voxels");
    Check(fullResult.stats.exposedFaces == 6u * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
        "full brick exposes only outer shell faces");
    for (uint32_t count : fullResult.stats.facesByDirection) {
        Check(count == SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE,
            "full brick exposes one face sheet per direction");
    }

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
    const uint32_t air = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Air, 0, 0, 0);
    const uint32_t stone = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Stone, 0, 0, 0);
    const uint32_t dirt = VENPOD::Utils::PackVoxel(VENPOD::Utils::Material::Dirt, 0, 0, 0);

    GeneratedSparseBrick brick;
    brick.coord = BrickCoord{2, -1, 3};
    brick.voxels.fill(air);
    brick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;

    SparseSurfaceCache cache;
    cache.BeginFrame();
    Check(cache.UpdateBrick(brick), "surface cache accepts first brick update");
    Check(cache.GetStats().cachedBricks == 1, "surface cache stores one brick");
    Check(cache.GetStats().totalFaces == 6, "surface cache tracks first brick faces");
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
    Check(snapshot.serial == cache.GetStats().serial, "surface snapshot serial matches cache");
    Check(!SparseSurfaceCache::TryLookupRangeInSnapshot(snapshot, BrickCoord{99, 99, 99}),
        "surface snapshot lookup rejects missing brick");

    GeneratedSparseBrick farBrick;
    farBrick.coord = BrickCoord{100, 0, 0};
    farBrick.voxels.fill(air);
    farBrick.voxels[LocalVoxelIndex(LocalVoxelCoord{0, 0, 0})] = stone;
    Check(cache.UpdateBrick(farBrick), "surface cache accepts distant culling brick");
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
    Check(SparseSurfaceCache::TryLookupRangeInSnapshot(culledSnapshot, brick.coord),
        "culled snapshot contains visible brick range");
    Check(!SparseSurfaceCache::TryLookupRangeInSnapshot(culledSnapshot, farBrick.coord),
        "culled snapshot omits distant brick range");

    cache.BeginFrame();
    Check(cache.RemoveBrick(brick.coord), "surface cache removes resident brick");
    Check(cache.GetStats().cachedBricks == 1, "surface cache removes one of two brick ranges");
    Check(cache.GetStats().totalFaces == 6, "surface cache keeps distant brick faces");
    Check(cache.GetStats().bricksRemovedLastFrame == 1, "surface cache remove counter increments");
    Check(cache.RemoveBrick(farBrick.coord), "surface cache removes distant culling brick");
    Check(cache.GetStats().cachedBricks == 0, "surface cache removes all brick ranges");
    Check(cache.GetStats().totalFaces == 0, "surface cache removes all face data");
    Check(!cache.RemoveBrick(brick.coord), "surface cache remove missing brick returns false");
}

void TestSparseBrushEditSemantics() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "sparse brush world initialize");

    const float airX = 900.5f;
    const float airY = 700.5f;
    const float airZ = -900.5f;
    const uint32_t painted = world.ApplyBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Stone,
        0,
        0,
        1.0f,
        77u);
    Check(painted > 0, "sparse brush paints air voxels");
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

    const uint32_t erased = world.ApplyBrushEdit(
        airX,
        airY,
        airZ,
        1.1f,
        VENPOD::Utils::Material::Air,
        1,
        0,
        1.0f,
        78u);
    Check(erased > 0, "sparse brush erases edited solid voxels");
    Check(world.SampleCollisionStatus(900, 700, -900) == CollisionSampleStatus::KnownAir,
        "sparse brush erase is immediately collision-authoritative");

    SparseVoxelWorld generatedWorld;
    Check(generatedWorld.Initialize({4, 16, 12345u}), "generated brush world initialize");
    const int32_t solidX = 96;
    const int32_t solidZ = 96;
    const int32_t solidY = static_cast<int32_t>(generatedWorld.GetTerrain().HeightAt(solidX, solidZ)) - 8;
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
        false);
    Check(rejectedPaint == 0, "sparse paint mode does not overwrite generated solid terrain");

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
        false);
    Check(replaced > 0, "sparse replace mode can overwrite generated solid terrain");
    Check(generatedWorld.GetStats().requestedBricks == 0,
        "sparse brush can update collision/edit overlay without requesting render residency");
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
}

void TestSparseVoxelWorldEviction() {
    SparseVoxelWorld world;
    Check(world.Initialize({8, 32, 12345u}), "eviction world initialize");

    const BrickCoord nearCoord{0, 0, 0};
    const BrickCoord farCoord{8, 0, 0};
    const BrickCoord editedCoord{9, 0, 0};
    const BrickCoord visibleCoord{10, 0, 0};
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
    Check(world.GetStats().residentBricks == 4, "eviction test starts with four residents");
    Check(world.GetStats().surfaceCachedBricks == 4, "surface cache tracks uploaded eviction bricks");

    const uint32_t stone = VENPOD::Utils::PackVoxel(
        VENPOD::Utils::Material::Stone,
        1,
        0,
        VENPOD::Utils::StateFlags::IsStatic);
    world.SetEditedVoxel(editedCoord.x * SPARSE_BRICK_SIZE, 0, 0, stone);
    SparseBrickUploadPacket editedPacket;
    Check(world.PopNextUpload(&editedPacket), "edited brick queues republish before eviction");
    Check(world.CompleteUpload(editedPacket), "edited brick republished before eviction");

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

    cone.maxRequests = 4;
    const std::vector<SparseBrickRequest> cappedCone = planner.PlanViewCone(cone);
    Check(cappedCone.size() == 4, "view-cone planner respects max request cap");
    Check(cappedCone.front().coord == BrickCoord{0, 0, 0},
        "view-cone planner keeps center ray origin as highest-priority request");
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
    Check(policy.TransitionStartAfterNearExit(40.0f) == 100.0f,
        "clipmap starts at configured start when near exit is close");
    Check(policy.TransitionStartAfterNearExit(200.0f) == 212.0f,
        "clipmap starts after near exit plus padding");
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

    SparseClipmapPolicy disabled({false, 100.0f, 900.0f, 8.0f, 12.0f, 4});
    Check(!disabled.IsEnabled(), "disabled clipmap policy");
    Check(disabled.BuildRings().empty(), "disabled clipmap emits no rings");
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
    Check(cache.GetStats().queuedTiles > 0, "clipmap tile cache queues camera interest");

    const uint32_t generated = cache.PumpGeneration(2u, 1u, policy);
    Check(generated >= 2u, "clipmap tile cache performs budgeted generation work");
    Check(cache.GetStats().generatedTilesLastFrame == 2u, "clipmap tile cache obeys height-tile generation budget");
    Check(cache.GetStats().residentTiles == 2u, "clipmap tile cache has generated residents");

    SparseClipmapGpuSnapshot snapshot;
    Check(cache.BuildGpuSnapshot(snapshot), "clipmap tile cache builds GPU snapshot");
    Check(snapshot.tileCount == 2u, "clipmap snapshot compact tile count");
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
        "clipmap lookup has one populated entry per resident compact tile");
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
    Check(tightVoxelCache.GetStats().queuedVoxelBricks <= tightVoxelConfig.maxVoxelBricks,
        "voxel clipmap interest is capped by resident capacity");
    tightVoxelCache.PumpGeneration(64u, 1u, tightVoxelPolicy);
    Check(tightVoxelCache.GetStats().residentVoxelBricks <= tightVoxelConfig.maxVoxelBricks,
        "voxel clipmap resident count respects capacity");
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
    TestCoordinateConversion();
    TestGpuPageEntryLayout();
    TestPageTable();
    TestBrickPool();
    TestTerrainGeneration();
    TestSparseSurfaceExtraction();
    TestSparseSurfaceCache();
    TestEditStoreAndCollision();
    TestSparseVoxelWorldLifecycle();
    TestSparseBrushEditSemantics();
    TestSparseRaycast();
    TestSparseVoxelWorldEviction();
    TestSparsePriorityReplacement();
    TestSparsePriorityQueues();
    TestSparseBrickRequestPlanner();
    TestSparseClipmapPolicy();
    TestSparseClipmapTileCache();

    if (failures != 0) {
        std::cerr << failures << " sparse core test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "Sparse core tests passed\n";
    return EXIT_SUCCESS;
}
