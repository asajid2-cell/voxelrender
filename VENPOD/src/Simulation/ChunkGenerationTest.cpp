#include "ChunkGenerationTest.h"
#include "InfiniteChunkManager.h"
#include "ChunkCoord.h"
#include "Chunk.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace VENPOD::Simulation {

// Material names for readable output
static const char* GetMaterialName(uint8_t materialID) {
    switch (materialID) {
        case 0: return "AIR";
        case 1: return "SAND";
        case 2: return "WATER";
        case 3: return "STONE";
        case 4: return "DIRT";
        case 5: return "WOOD";
        case 6: return "FIRE";
        case 7: return "LAVA";
        case 8: return "ICE";
        case 9: return "OIL";
        case 255: return "BEDROCK";
        default: return "UNKNOWN";
    }
}

bool ChunkGenerationTest::RunAllTests(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager)
{
    spdlog::info("");
    spdlog::info("CHUNK GENERATION TEST SUITE");
    spdlog::info("");

    bool allPassed = true;

    // Test 1: Single chunk generation
    spdlog::info("\n[TEST 1] Single Chunk Generation");
    if (!TestSingleChunkGeneration(device, commandQueue, heapManager)) {
        spdlog::error("FAILED: Single chunk generation test");
        allPassed = false;
    } else {
        spdlog::info("PASSED: Single chunk generation test");
    }

    // Test 2: Chunk boundaries
    spdlog::info("\n[TEST 2] Chunk Boundary Seamlessness");
    if (!TestChunkBoundaries(device, commandQueue, heapManager)) {
        spdlog::error("FAILED: Chunk boundary test");
        allPassed = false;
    } else {
        spdlog::info("PASSED: Chunk boundary test");
    }

    // Test 3: World coordinates
    spdlog::info("\n[TEST 3] World Coordinate Mapping");
    if (!TestWorldCoordinates(device, commandQueue, heapManager)) {
        spdlog::error("FAILED: World coordinate test");
        allPassed = false;
    } else {
        spdlog::info("PASSED: World coordinate test");
    }

    // Summary
    spdlog::info("\n");
    if (allPassed) {
        spdlog::info("ALL TESTS PASSED");
    } else {
        spdlog::info("SOME TESTS FAILED");
    }
    spdlog::info("\n");

    return allPassed;
}

bool ChunkGenerationTest::TestSingleChunkGeneration(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager)
{
    spdlog::info("  Initializing chunk manager...");

    // Initialize chunk manager
    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig config;
    config.worldSeed = 12345;
    config.chunksPerFrame = 1;

    auto initResult = chunkManager.Initialize(
        device.GetDevice(),
        heapManager,
        config
    );

    if (!initResult) {
        spdlog::error("  Failed to initialize chunk manager: {}", initResult.error());
        return false;
    }

    spdlog::info("  OK Chunk manager initialized");

    // Create command list for generation
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    HRESULT hr = device.GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&cmdAllocator)
    );
    if (FAILED(hr)) {
        spdlog::error("  Failed to create command allocator");
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = device.GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&cmdList)
    );
    if (FAILED(hr)) {
        spdlog::error("  Failed to create command list");
        return false;
    }

    spdlog::info("  OK Command list created");

    // Force generate chunk at origin
    ChunkCoord testCoord = {0, 0, 0};
    spdlog::info("  Generating chunk at [{},{},{}]...", testCoord.x, testCoord.y, testCoord.z);

    auto genResult = chunkManager.ForceGenerateChunk(
        device.GetDevice(),
        cmdList.Get(),
        testCoord
    );

    if (!genResult) {
        spdlog::error("  Failed to generate chunk: {}", genResult.error());
        return false;
    }

    // Close and execute command list
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);

    // Wait for GPU to finish
    commandQueue.Flush();
    spdlog::info("  OK Chunk generation complete");

    // Get generated chunk
    Chunk* chunk = chunkManager.GetChunk(testCoord);
    if (!chunk) {
        spdlog::error("  Chunk was not added to loaded chunks map!");
        return false;
    }

    if (!chunk->IsGenerated()) {
        spdlog::error("  Chunk state is not Generated!");
        return false;
    }

    spdlog::info("  OK Chunk is in Generated state");

    // Read back voxel data
    constexpr uint32_t voxelCount = 64 * 64 * 64;
    std::vector<uint32_t> voxelData(voxelCount);

    spdlog::info("  Reading back GPU data to CPU...");
    auto readbackResult = ReadbackGPUBuffer(
        device,
        commandQueue,
        chunk->GetVoxelBuffer().GetResource(),
        voxelData.data(),
        voxelCount * sizeof(uint32_t)
    );

    if (!readbackResult) {
        spdlog::error("  Failed to read back GPU data: {}", readbackResult.error());
        return false;
    }

    spdlog::info("  OK GPU data read back successfully");

    // Analyze the data
    spdlog::info("  Analyzing voxel data...");
    AnalyzeVoxelData(voxelData.data(), voxelCount);

    // Cleanup
    chunkManager.Shutdown();

    return true;
}

bool ChunkGenerationTest::TestChunkBoundaries(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager)
{
    spdlog::info("  Generating two adjacent chunks...");

    // Initialize chunk manager
    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig config;
    config.worldSeed = 12345;

    auto initResult = chunkManager.Initialize(
        device.GetDevice(),
        heapManager,
        config
    );

    if (!initResult) {
        spdlog::error("  Failed to initialize: {}", initResult.error());
        return false;
    }

    // Create command list
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    device.GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&cmdAllocator)
    );

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device.GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&cmdList)
    );

    // Generate chunk A at origin
    ChunkCoord chunkA = {0, 0, 0};
    chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), chunkA);

    // Generate chunk B adjacent in X direction
    ChunkCoord chunkB = {1, 0, 0};
    chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), chunkB);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    spdlog::info("  OK Both chunks generated");

    // Read back both chunks
    std::vector<uint32_t> dataA(64 * 64 * 64);
    std::vector<uint32_t> dataB(64 * 64 * 64);

    auto chunkAPtrConst = chunkManager.GetChunk(chunkA);
    auto chunkBPtrConst = chunkManager.GetChunk(chunkB);

    ReadbackGPUBuffer(device, commandQueue, chunkAPtrConst->GetVoxelBuffer().GetResource(),
                      dataA.data(), dataA.size() * sizeof(uint32_t));
    ReadbackGPUBuffer(device, commandQueue, chunkBPtrConst->GetVoxelBuffer().GetResource(),
                      dataB.data(), dataB.size() * sizeof(uint32_t));

    spdlog::info("  Checking boundary consistency...");

    // Check boundary voxels (chunk A's x=63 edge vs chunk B's x=0 edge)
    int similarMaterials = 0;
    int totalBoundaryVoxels = 64 * 64;

    for (int y = 0; y < 64; ++y) {
        for (int z = 0; z < 64; ++z) {
            // Chunk A boundary (x=63, y, z)
            uint32_t indexA = 63 + y * 64 + z * 64 * 64;
            uint8_t materialA = dataA[indexA] & 0xFF;

            // Chunk B boundary (x=0, y, z)
            uint32_t indexB = 0 + y * 64 + z * 64 * 64;
            uint8_t materialB = dataB[indexB] & 0xFF;

            // Materials should be similar at boundary (same height -> similar terrain)
            // Allow some variation due to noise
            if (materialA == materialB) {
                similarMaterials++;
            }
        }
    }

    float similarityPercent = (similarMaterials * 100.0f) / totalBoundaryVoxels;
    spdlog::info("  Boundary similarity: {:.1f}% ({}/{} matching materials)",
                 similarityPercent, similarMaterials, totalBoundaryVoxels);

    // We expect high similarity but not 100% (noise varies per voxel)
    bool passed = similarityPercent > 50.0f;  // At least 50% should match

    if (passed) {
        spdlog::info("  OK Chunks appear to connect seamlessly");
    } else {
        spdlog::warn("WARN Low boundary similarity - check world coordinate usage");
    }

    chunkManager.Shutdown();
    return passed;
}

bool ChunkGenerationTest::TestWorldCoordinates(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager)
{
    (void)device;
    (void)commandQueue;
    (void)heapManager;  // Unused in this test
    spdlog::info("  Testing world coordinate mapping...");

    // Test FromWorldPosition with various coordinates
    struct TestCase {
        int32_t worldX, worldY, worldZ;
        int32_t expectedChunkX, expectedChunkY, expectedChunkZ;
    };

    TestCase testCases[] = {
        {0, 0, 0, 0, 0, 0},           // Origin
        {63, 50, 32, 0, 0, 0},        // Still in chunk 0
        {64, 0, 0, 1, 0, 0},          // First voxel of chunk 1
        {128, 64, 128, 2, 1, 2},      // Positive coords
        {-1, 0, 0, -1, 0, 0},         // Negative (CRITICAL TEST!)
        {-64, 0, 0, -1, 0, 0},        // Negative boundary
        {-65, 0, 0, -2, 0, 0},        // Next negative chunk
    };

    bool allPassed = true;
    for (const auto& test : testCases) {
        ChunkCoord result = ChunkCoord::FromWorldPosition(
            test.worldX, test.worldY, test.worldZ, INFINITE_CHUNK_SIZE
        );

        bool matches = (result.x == test.expectedChunkX &&
                       result.y == test.expectedChunkY &&
                       result.z == test.expectedChunkZ);

        if (!matches) {
            spdlog::error("  FAILED World({},{},{}) -> Chunk[{},{},{}], expected [{},{},{}]",
                         test.worldX, test.worldY, test.worldZ,
                         result.x, result.y, result.z,
                         test.expectedChunkX, test.expectedChunkY, test.expectedChunkZ);
            allPassed = false;
        } else {
            spdlog::debug("  OK World({},{},{}) -> Chunk[{},{},{}]",
                         test.worldX, test.worldY, test.worldZ,
                         result.x, result.y, result.z);
        }
    }

    if (allPassed) {
        spdlog::info("  OK All world coordinate mappings correct");
    }

    return allPassed;
}

Result<void> ChunkGenerationTest::ReadbackGPUBuffer(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    ID3D12Resource* gpuResource,
    void* destBuffer,
    uint64_t bufferSize)
{
    // Create readback buffer
    D3D12_HEAP_PROPERTIES readbackHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

    ComPtr<ID3D12Resource> readbackBuffer;
    HRESULT hr = device.GetDevice()->CreateCommittedResource(
        &readbackHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &readbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readbackBuffer)
    );

    if (FAILED(hr)) {
        return Error("Failed to create readback buffer");
    }

    // Create command list for copy
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    hr = device.GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&cmdAllocator)
    );
    if (FAILED(hr)) {
        return Error("Failed to create command allocator for readback");
    }

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = device.GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&cmdList)
    );
    if (FAILED(hr)) {
        return Error("Failed to create command list for readback");
    }

    // Transition GPU resource to COPY_SOURCE
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    int barrierCount = 0;

    barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
        gpuResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE
    );

    cmdList->ResourceBarrier(barrierCount, barriers);

    // Copy GPU resource to readback buffer
    cmdList->CopyResource(readbackBuffer.Get(), gpuResource);

    // Transition back to UAV
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        gpuResource,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    cmdList->ResourceBarrier(1, barriers);

    cmdList->Close();

    // Execute and wait
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    // Map and read
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, bufferSize};
    hr = readbackBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return Error("Failed to map readback buffer");
    }

    memcpy(destBuffer, mappedData, bufferSize);

    D3D12_RANGE writeRange = {0, 0};
    readbackBuffer->Unmap(0, &writeRange);

    return {};
}

void ChunkGenerationTest::AnalyzeVoxelData(const uint32_t* voxelData, uint32_t voxelCount) {
    // ===== CRITICAL: Check for magic value at index 0 (shader execution test) =====
    spdlog::info("SHADER EXECUTION TEST:");
    if (voxelData[0] == 0xDEADBEEF) {
        spdlog::info("MAGIC VALUE FOUND! Shader executed successfully!");
        spdlog::info("    voxelData[0] = 0x{:08X} (expected 0xDEADBEEF)", voxelData[0]);
    } else {
        spdlog::error("    FAILED SHADER DID NOT EXECUTE!");
        spdlog::error("    voxelData[0] = 0x{:08X} (expected 0xDEADBEEF)", voxelData[0]);
        spdlog::error("    This proves the compute shader is NOT writing to the buffer!");
    }

    // Count materials
    std::unordered_map<uint8_t, uint32_t> materialCounts;

    for (uint32_t i = 0; i < voxelCount; ++i) {
        uint8_t material = voxelData[i] & 0xFF;
        materialCounts[material]++;
    }

    // Sort by count (descending)
    std::vector<std::pair<uint8_t, uint32_t>> sortedMaterials(
        materialCounts.begin(), materialCounts.end()
    );
    std::sort(sortedMaterials.begin(), sortedMaterials.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Print distribution
    spdlog::info("  Material distribution:");
    for (const auto& [material, count] : sortedMaterials) {
        float percent = (count * 100.0f) / voxelCount;
        spdlog::info("    {:12} ({}): {:7} voxels ({:5.2f}%)",
                     GetMaterialName(material), material, count, percent);
    }

    // Validation checks
    bool hasAir = materialCounts[0] > 0;  // MAT_AIR
    bool hasSolid = materialCounts[3] > 0 || materialCounts[4] > 0 || materialCounts[1] > 0;  // STONE/DIRT/SAND
    bool hasWater = materialCounts[2] > 0;  // MAT_WATER

    spdlog::info("  Validation:");
    spdlog::info("    Air present: {}", hasAir ? "OK" : "no");
    spdlog::info("    Solid materials: {}", hasSolid ? "OK" : "no");
    spdlog::info("    Water present: {}", hasWater ? "OK" : "no");

    if (!hasAir) {
        spdlog::warn("     No air found - terrain might be solid!");
    }

    if (!hasSolid) {
        spdlog::error("    no No solid materials - chunk is empty!");
    }

    if (hasAir && hasSolid) {
        spdlog::info("    OK Chunk has realistic terrain distribution");
    }
}

} // namespace VENPOD::Simulation
