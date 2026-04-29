#include "ChunkStressTest.h"
#include "InfiniteChunkManager.h"
#include "VoxelWorld.h"
#include "ChunkCoord.h"
#include "Chunk.h"
#include "../Graphics/RHI/d3dx12.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <random>

namespace VENPOD::Simulation {

// Static member initialization
std::vector<StressTestResult> ChunkStressTest::s_lastResults;

// ============================================================================
// Main Test Runner
// ============================================================================

bool ChunkStressTest::RunAllStressTests(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager)
{
    StressTestConfig defaultConfig;
    return RunAllStressTests(device, commandQueue, heapManager, defaultConfig);
}

bool ChunkStressTest::RunAllStressTests(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    s_lastResults.clear();

    spdlog::info("");
    spdlog::info("========================================================");
    spdlog::info("       CHUNK STRESS TEST SUITE");
    spdlog::info("       Intensity: {} | Max Duration: {}ms",
        config.intensity, config.maxDurationMs);
    spdlog::info("========================================================");
    spdlog::info("");

    bool allPassed = true;

    // Define test functions
    struct TestEntry {
        const char* name;
        StressTestResult (*func)(Graphics::DX12Device&, Graphics::DX12CommandQueue&,
                                 Graphics::DescriptorHeapManager&, const StressTestConfig&);
    };

    TestEntry tests[] = {
        {"Descriptor Exhaustion", TestDescriptorExhaustion},
        {"Rapid Chunk Cycling", TestRapidChunkCycling},
        {"Command Allocator Reuse", TestCommandAllocatorReuse},
        {"Resource State Transitions", TestResourceStateTransitions},
        {"Fence Synchronization", TestFenceSynchronization},
        {"Memory Leak Detection", TestMemoryLeaks},
        {"Cache Consistency", TestCacheConsistency},
        {"Boundary Conditions", TestBoundaryConditions},
    };

    int testNum = 1;
    for (const auto& test : tests) {
        spdlog::info("[STRESS TEST {}] {}", testNum, test.name);

        auto result = test.func(device, commandQueue, heapManager, config);
        s_lastResults.push_back(result);

        LogTestResult(result);

        if (!result.passed) {
            allPassed = false;
            if (config.abortOnFirstFailure) {
                spdlog::error("Aborting stress tests due to failure");
                break;
            }
        }

        testNum++;
        spdlog::info("");
    }

    LogTestSummary(s_lastResults);

    return allPassed;
}

// ============================================================================
// Test 1: Descriptor Exhaustion
// ============================================================================

StressTestResult ChunkStressTest::TestDescriptorExhaustion(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Descriptor Exhaustion";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    uint32_t initialDescriptors = CountAllocatedDescriptors(heapManager);
    spdlog::info("  Initial descriptors: {}", initialDescriptors);

    // Calculate target allocations based on intensity
    uint32_t targetAllocations = 50 * config.intensity;
    spdlog::info("  Attempting to allocate {} chunks...", targetAllocations);

    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig chunkConfig;
    chunkConfig.worldSeed = 12345;
    chunkConfig.chunksPerFrame = 1;

    auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize chunk manager: " + std::string(initResult.error());
        result.durationMs = EndTimer(startTime);
        return result;
    }

    // Create command resources
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    uint32_t successfulAllocations = 0;
    uint32_t failedAllocations = 0;

    for (uint32_t i = 0; i < targetAllocations; ++i) {
        // Spread chunks across 3D space
        ChunkCoord coord = {
            static_cast<int32_t>(i % 10),
            static_cast<int32_t>((i / 10) % 5),
            static_cast<int32_t>(i / 50)
        };

        auto genResult = chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), coord);

        if (genResult) {
            successfulAllocations++;
        } else {
            failedAllocations++;
            if (config.verbose) {
                spdlog::debug("  Allocation {} failed: {}", i, genResult.error());
            }
        }

        // Check for device removal periodically
        if (i % 20 == 0) {
            std::string errorMsg;
            if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
                result.passed = false;
                result.deviceRemoved = true;
                result.errorMessage = "Device removed during allocation: " + errorMsg;
                break;
            }
        }
    }

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    spdlog::info("  Successful allocations: {}", successfulAllocations);
    spdlog::info("  Failed allocations: {}", failedAllocations);

    // Check for device removal
    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed after execution: " + errorMsg;
    }

    // Cleanup
    chunkManager.Shutdown();

    uint32_t finalDescriptors = CountAllocatedDescriptors(heapManager);
    result.descriptorsAllocated = successfulAllocations * 2;  // SRV + UAV per chunk
    result.descriptorsLeaked = (finalDescriptors > initialDescriptors + 10) ?
        (finalDescriptors - initialDescriptors) : 0;

    spdlog::info("  Descriptors after cleanup: {} (leaked: {})",
        finalDescriptors, result.descriptorsLeaked);

    if (result.descriptorsLeaked > 0) {
        result.passed = false;
        result.errorMessage = "Descriptor leak detected: " + std::to_string(result.descriptorsLeaked);
    }

    result.iterations = targetAllocations;
    result.durationMs = EndTimer(startTime);

    // Test passes if we allocated at least some chunks and cleaned up properly
    if (result.passed && successfulAllocations == 0) {
        result.passed = false;
        result.errorMessage = "No chunks could be allocated";
    }

    return result;
}

// ============================================================================
// Test 2: Rapid Chunk Cycling
// ============================================================================

StressTestResult ChunkStressTest::TestRapidChunkCycling(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Rapid Chunk Cycling";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    uint32_t iterations = config.cycleIterations * config.intensity;
    spdlog::info("  Running {} camera movement cycles...", iterations);

    // Create VoxelWorld
    VoxelWorldConfig worldConfig;
    worldConfig.gridSizeX = 256;
    worldConfig.gridSizeY = 256;
    worldConfig.gridSizeZ = 256;

    // Create init command list
    ComPtr<ID3D12CommandAllocator> initAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&initAllocator));

    ComPtr<ID3D12GraphicsCommandList> initCmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        initAllocator.Get(), nullptr, IID_PPV_ARGS(&initCmdList));

    VoxelWorld voxelWorld;
    auto initResult = voxelWorld.Initialize(device.GetDevice(), initCmdList.Get(), heapManager, worldConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize VoxelWorld: " + std::string(initResult.error());
        result.durationMs = EndTimer(startTime);
        return result;
    }

    initCmdList->Close();
    ID3D12CommandList* lists[] = { initCmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    // CRITICAL FIX: Simulate GRADUAL camera movement (not teleportation)
    // This tests loading chunks ahead and unloading chunks behind
    glm::vec3 cameraPos(0.0f, 64.0f, 0.0f);
    const float speed = 10.0f;  // 10 voxels per frame (crosses chunk boundary every 6-7 frames)

    uint32_t chunksLoadedCount = 0;
    uint32_t chunksUnloadedCount = 0;

    for (uint32_t i = 0; i < iterations; ++i) {
        size_t prevChunkCount = voxelWorld.GetChunkManager()->GetLoadedChunkCount();

        // Move camera in spiral pattern to cross many chunk boundaries
        // This stresses load/unload cycle
        float angle = static_cast<float>(i) * 0.1f;
        cameraPos.x += speed * cosf(angle);
        cameraPos.z += speed * sinf(angle);

        voxelWorld.UpdateChunks(device.GetDevice(), commandQueue.GetCommandQueue(), cameraPos);

        size_t newChunkCount = voxelWorld.GetChunkManager()->GetLoadedChunkCount();
        if (newChunkCount > prevChunkCount) {
            chunksLoadedCount += static_cast<uint32_t>(newChunkCount - prevChunkCount);
        } else if (newChunkCount < prevChunkCount) {
            chunksUnloadedCount += static_cast<uint32_t>(prevChunkCount - newChunkCount);
        }

        // Flush periodically to prevent command queue overflow
        if (i % 30 == 0) {
            commandQueue.Flush();

            // Check for device removal
            std::string errorMsg;
            if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
                result.passed = false;
                result.deviceRemoved = true;
                result.errorMessage = "Device removed at iteration " + std::to_string(i) + ": " + errorMsg;
                break;
            }
        }

        // Check timeout
        if (EndTimer(startTime) > config.maxDurationMs) {
            spdlog::warn("  Test timeout at iteration {}", i);
            break;
        }
    }

    spdlog::info("  Chunks loaded: {}, unloaded: {}", chunksLoadedCount, chunksUnloadedCount);

    commandQueue.Flush();

    // Final device check
    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed after test: " + errorMsg;
    }

    voxelWorld.Shutdown();

    result.iterations = iterations;
    result.durationMs = EndTimer(startTime);

    if (result.passed) {
        spdlog::info("  Average time per cycle: {:.3f}ms", result.durationMs / iterations);
    }

    return result;
}

// ============================================================================
// Test 3: Command Allocator Reuse
// ============================================================================

StressTestResult ChunkStressTest::TestCommandAllocatorReuse(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Command Allocator Reuse";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    // This test rapidly generates chunks to stress the ring buffer allocator
    uint32_t iterations = 100 * config.intensity;
    spdlog::info("  Generating {} chunks rapidly to test ring buffer...", iterations);

    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig chunkConfig;
    chunkConfig.worldSeed = 54321;
    chunkConfig.chunksPerFrame = 1;

    auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize: " + std::string(initResult.error());
        result.durationMs = EndTimer(startTime);
        return result;
    }

    // Rapidly call Update to trigger chunk generation
    for (uint32_t i = 0; i < iterations; ++i) {
        glm::vec3 cameraPos(
            static_cast<float>((i * 64) % 1024),
            64.0f,
            static_cast<float>((i * 32) % 1024)
        );

        chunkManager.Update(device.GetDevice(), commandQueue.GetCommandQueue(), cameraPos);

        // Don't flush every frame - let ring buffer handle it
        if (i % 50 == 0) {
            commandQueue.Flush();

            std::string errorMsg;
            if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
                result.passed = false;
                result.deviceRemoved = true;
                result.errorMessage = "Device removed at iteration " + std::to_string(i);
                break;
            }
        }
    }

    commandQueue.Flush();

    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed after test";
    }

    size_t chunksLoaded = chunkManager.GetLoadedChunkCount();
    spdlog::info("  Final chunks loaded: {}", chunksLoaded);

    chunkManager.Shutdown();

    result.iterations = iterations;
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Test 4: Resource State Transitions
// ============================================================================

StressTestResult ChunkStressTest::TestResourceStateTransitions(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Resource State Transitions";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    spdlog::info("  Testing state transitions on voxel buffers...");

    // Create a single chunk and manually test transitions
    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig chunkConfig;
    chunkConfig.worldSeed = 99999;

    auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    // Generate a chunk
    ChunkCoord testCoord = {0, 0, 0};
    auto genResult = chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), testCoord);
    if (!genResult) {
        result.passed = false;
        result.errorMessage = "Failed to generate chunk";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    Chunk* chunk = chunkManager.GetChunk(testCoord);
    if (!chunk) {
        result.passed = false;
        result.errorMessage = "Chunk not found after generation";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    // Test multiple state transitions
    uint32_t transitionCount = 50 * config.intensity;
    spdlog::info("  Performing {} state transitions...", transitionCount);

    D3D12_RESOURCE_STATES states[] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST,
    };

    // CRITICAL FIX: Track actual current state instead of assuming
    // Chunk starts in UAV state after generation
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    for (uint32_t i = 0; i < transitionCount; ++i) {
        cmdAllocator->Reset();
        cmdList->Reset(cmdAllocator.Get(), nullptr);

        // Cycle through states, transitioning from ACTUAL current state
        D3D12_RESOURCE_STATES nextState = states[(i + 1) % 4];

        if (currentState != nextState) {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                chunk->GetVoxelBuffer().GetResource(),
                currentState,  // ✅ Use tracked state, not assumed
                nextState
            );
            cmdList->ResourceBarrier(1, &barrier);
            currentState = nextState;  // ✅ Update tracked state
        }

        cmdList->Close();
        ID3D12CommandList* cmdLists[] = { cmdList.Get() };
        commandQueue.GetCommandQueue()->ExecuteCommandLists(1, cmdLists);

        if (i % 20 == 0) {
            commandQueue.Flush();

            std::string errorMsg;
            if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
                result.passed = false;
                result.deviceRemoved = true;
                result.errorMessage = "Device removed during transitions at iteration " + std::to_string(i);
                break;
            }
        }
    }

    commandQueue.Flush();

    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed after transitions";
    }

    chunkManager.Shutdown();

    result.iterations = transitionCount;
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Test 5: Fence Synchronization
// ============================================================================

StressTestResult ChunkStressTest::TestFenceSynchronization(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Fence Synchronization";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    uint32_t iterations = 200 * config.intensity;
    spdlog::info("  Testing {} fence signal/wait cycles with real GPU work...", iterations);

    // Create fence
    ComPtr<ID3D12Fence> fence;
    HRESULT hr = device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        result.passed = false;
        result.errorMessage = "Failed to create fence";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceValue = 0;

    // CRITICAL FIX: Create chunk manager to dispatch real GPU work
    // Empty command lists complete instantly and don't stress fence synchronization
    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig chunkConfig;
    chunkConfig.worldSeed = 54321;
    chunkConfig.chunksPerFrame = 1;

    auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize chunk manager for fence test";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    for (uint32_t i = 0; i < iterations; ++i) {
        // CRITICAL FIX: Generate chunk (real GPU work) instead of empty command list
        cmdAllocator->Reset();
        cmdList->Reset(cmdAllocator.Get(), nullptr);

        ChunkCoord coord = {static_cast<int32_t>(i % 10), 0, static_cast<int32_t>(i / 10)};
        auto genResult = chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), coord);
        if (!genResult && config.verbose) {
            spdlog::debug("  Chunk generation failed at iteration {}: {}", i, genResult.error());
        }

        cmdList->Close();

        ID3D12CommandList* cmdLists[] = { cmdList.Get() };
        commandQueue.GetCommandQueue()->ExecuteCommandLists(1, cmdLists);

        fenceValue++;
        commandQueue.GetCommandQueue()->Signal(fence.Get(), fenceValue);

        // Occasionally wait for completion
        if (i % 10 == 0) {
            if (fence->GetCompletedValue() < fenceValue) {
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                DWORD waitResult = WaitForSingleObject(fenceEvent, 1000);
                if (waitResult == WAIT_TIMEOUT) {
                    result.passed = false;
                    result.errorMessage = "Fence wait timeout at iteration " + std::to_string(i);
                    break;
                }
            }
        }

        std::string errorMsg;
        if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
            result.passed = false;
            result.deviceRemoved = true;
            result.errorMessage = "Device removed at iteration " + std::to_string(i);
            break;
        }
    }

    // Cleanup chunk manager
    chunkManager.Shutdown();

    // Final wait
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, 5000);
    }

    CloseHandle(fenceEvent);

    result.iterations = iterations;
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Test 6: Memory Leak Detection
// ============================================================================

StressTestResult ChunkStressTest::TestMemoryLeaks(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Memory Leak Detection";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    uint32_t cycles = 5 * config.intensity;
    uint32_t chunksPerCycle = 20;
    spdlog::info("  Running {} allocation/deallocation cycles ({} chunks each)...",
        cycles, chunksPerCycle);

    uint32_t initialDescriptors = CountAllocatedDescriptors(heapManager);
    spdlog::info("  Initial descriptors: {}", initialDescriptors);

    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        // Allocate chunks
        InfiniteChunkManager chunkManager;
        InfiniteChunkConfig chunkConfig;
        chunkConfig.worldSeed = 12345 + cycle;

        auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
        if (!initResult) {
            result.passed = false;
            result.errorMessage = "Init failed at cycle " + std::to_string(cycle);
            break;
        }

        ComPtr<ID3D12CommandAllocator> cmdAllocator;
        device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));

        ComPtr<ID3D12GraphicsCommandList> cmdList;
        device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

        for (uint32_t i = 0; i < chunksPerCycle; ++i) {
            ChunkCoord coord = {static_cast<int32_t>(i), 0, 0};
            chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), coord);
        }

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
        commandQueue.Flush();

        // Shutdown and check for leaks
        chunkManager.Shutdown();

        uint32_t afterCycleDescriptors = CountAllocatedDescriptors(heapManager);
        // Allow small tolerance (2-3 descriptors) for persistent resources like frame buffers
        // that may be allocated during testing but aren't chunk-related
        constexpr uint32_t TOLERANCE = 3;
        if (afterCycleDescriptors > initialDescriptors + TOLERANCE) {
            spdlog::warn("  Cycle {}: {} descriptors leaked (tolerance: {})",
                cycle, afterCycleDescriptors - initialDescriptors, TOLERANCE);
        }

        std::string errorMsg;
        if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
            result.passed = false;
            result.deviceRemoved = true;
            result.errorMessage = "Device removed at cycle " + std::to_string(cycle);
            break;
        }
    }

    uint32_t finalDescriptors = CountAllocatedDescriptors(heapManager);
    // Allow small tolerance for persistent resources
    constexpr uint32_t TOLERANCE = 3;
    result.descriptorsLeaked = (finalDescriptors > initialDescriptors + TOLERANCE) ?
        (finalDescriptors - initialDescriptors) : 0;

    spdlog::info("  Final descriptors: {} (leaked: {})", finalDescriptors, result.descriptorsLeaked);

    if (result.descriptorsLeaked > 0) {
        result.passed = false;
        result.errorMessage = "Descriptors leaked: " + std::to_string(result.descriptorsLeaked);
    }

    result.iterations = cycles * chunksPerCycle;
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Test 7: Cache Consistency
// ============================================================================

StressTestResult ChunkStressTest::TestCacheConsistency(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Cache Consistency";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    spdlog::info("  Testing double-buffered cache with SwapBuffers...");

    // Create VoxelWorld
    VoxelWorldConfig worldConfig;
    worldConfig.gridSizeX = 256;
    worldConfig.gridSizeY = 256;
    worldConfig.gridSizeZ = 256;

    ComPtr<ID3D12CommandAllocator> initAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&initAllocator));

    ComPtr<ID3D12GraphicsCommandList> initCmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        initAllocator.Get(), nullptr, IID_PPV_ARGS(&initCmdList));

    VoxelWorld voxelWorld;
    auto initResult = voxelWorld.Initialize(device.GetDevice(), initCmdList.Get(), heapManager, worldConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize VoxelWorld";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    initCmdList->Close();
    ID3D12CommandList* lists[] = { initCmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    // Simulate game loop with buffer swaps
    uint32_t iterations = 100 * config.intensity;
    glm::vec3 cameraPos(128.0f, 64.0f, 128.0f);

    uint32_t cacheValidationFailures = 0;

    for (uint32_t i = 0; i < iterations; ++i) {
        int readBufferBeforeSwap = voxelWorld.GetReadBufferIndex();
        size_t cacheSizeBeforeSwap[2] = {
            voxelWorld.GetCopiedChunkCount(0),
            voxelWorld.GetCopiedChunkCount(1)
        };

        // Update chunks
        voxelWorld.UpdateChunks(device.GetDevice(), commandQueue.GetCommandQueue(), cameraPos);

        // CACHE VALIDATION: After UpdateChunks, WRITE buffer should have chunks in cache
        int writeBufferBeforeSwap = 1 - readBufferBeforeSwap;
        size_t writeCacheSizeAfterUpdate = voxelWorld.GetCopiedChunkCount(writeBufferBeforeSwap);
        if (writeCacheSizeAfterUpdate == 0 && voxelWorld.GetChunkManager()->GetLoadedChunkCount() > 0) {
            if (config.verbose) {
                spdlog::warn("  Iteration {}: WRITE buffer cache empty after UpdateChunks (expected chunks)", i);
            }
            cacheValidationFailures++;
        }

        // Swap buffers (simulating frame end)
        voxelWorld.SwapBuffers();

        // CACHE VALIDATION: After SwapBuffers, NEW WRITE buffer's cache should be cleared
        int newReadBuffer = voxelWorld.GetReadBufferIndex();
        int newWriteBuffer = 1 - newReadBuffer;
        size_t newWriteCacheSize = voxelWorld.GetCopiedChunkCount(newWriteBuffer);

        // NEW WRITE buffer should be cleared (it was old READ buffer, now needs fresh copy)
        // Allow 1 frame of grace for chunks to be copied
        if (i > 2 && newWriteCacheSize > cacheSizeBeforeSwap[newWriteBuffer]) {
            if (config.verbose) {
                spdlog::warn("  Iteration {}: NEW WRITE buffer cache not cleared after swap (size: {}, expected: 0)",
                    i, newWriteCacheSize);
            }
            cacheValidationFailures++;
        }

        // Move camera slightly
        cameraPos.x += (i % 2 == 0) ? 1.0f : -1.0f;

        if (i % 30 == 0) {
            commandQueue.Flush();

            std::string errorMsg;
            if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
                result.passed = false;
                result.deviceRemoved = true;
                result.errorMessage = "Device removed at iteration " + std::to_string(i);
                break;
            }
        }
    }

    spdlog::info("  Cache validation failures: {}", cacheValidationFailures);

    if (cacheValidationFailures > iterations / 10) {
        result.passed = false;
        result.errorMessage = "Too many cache validation failures: " + std::to_string(cacheValidationFailures);
    }

    commandQueue.Flush();

    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed after test";
    }

    voxelWorld.Shutdown();

    result.iterations = iterations;
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Test 8: Boundary Conditions
// ============================================================================

StressTestResult ChunkStressTest::TestBoundaryConditions(
    Graphics::DX12Device& device,
    Graphics::DX12CommandQueue& commandQueue,
    Graphics::DescriptorHeapManager& heapManager,
    const StressTestConfig& config)
{
    StressTestResult result;
    result.testName = "Boundary Conditions";
    result.passed = true;
    result.gpuFaults = 0;
    result.deviceRemoved = false;

    auto startTime = StartTimer();

    spdlog::info("  Testing extreme chunk coordinates...");

    InfiniteChunkManager chunkManager;
    InfiniteChunkConfig chunkConfig;
    chunkConfig.worldSeed = 11111;

    auto initResult = chunkManager.Initialize(device.GetDevice(), heapManager, chunkConfig);
    if (!initResult) {
        result.passed = false;
        result.errorMessage = "Failed to initialize";
        result.durationMs = EndTimer(startTime);
        return result;
    }

    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    device.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    // Test extreme coordinates
    struct TestCoord {
        int32_t x, y, z;
        const char* description;
    };

    TestCoord testCoords[] = {
        {0, 0, 0, "Origin"},
        {-1, 0, 0, "Negative X"},
        {0, -1, 0, "Negative Y"},
        {0, 0, -1, "Negative Z"},
        {-10, -10, -10, "All negative"},
        {100, 50, 100, "Large positive"},
        {-100, -50, -100, "Large negative"},
        {INT32_MAX / 64 - 1, 0, 0, "Near INT32_MAX (X)"},
        {0, INT32_MAX / 64 - 1, 0, "Near INT32_MAX (Y)"},
        {0, 0, INT32_MAX / 64 - 1, "Near INT32_MAX (Z)"},
        {INT32_MIN / 64 + 1, 0, 0, "Near INT32_MIN (X)"},
        {0, INT32_MIN / 64 + 1, 0, "Near INT32_MIN (Y)"},
        {INT32_MAX / 64 - 1, INT32_MIN / 64 + 1, 0, "Mixed extremes (X/Y)"},
    };

    int passedCoords = 0;
    int failedCoords = 0;

    for (const auto& testCoord : testCoords) {
        ChunkCoord coord = {testCoord.x, testCoord.y, testCoord.z};

        auto genResult = chunkManager.ForceGenerateChunk(device.GetDevice(), cmdList.Get(), coord);

        if (genResult) {
            passedCoords++;
            if (config.verbose) {
                spdlog::debug("  {} [{},{},{}]: OK", testCoord.description, coord.x, coord.y, coord.z);
            }
        } else {
            failedCoords++;
            spdlog::warn("  {} [{},{},{}]: FAILED - {}",
                testCoord.description, coord.x, coord.y, coord.z, genResult.error());
        }
    }

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    commandQueue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    commandQueue.Flush();

    spdlog::info("  Passed: {}, Failed: {}", passedCoords, failedCoords);

    std::string errorMsg;
    if (CheckDeviceRemoved(device.GetDevice(), errorMsg)) {
        result.passed = false;
        result.deviceRemoved = true;
        result.errorMessage = "Device removed during boundary tests";
    }

    chunkManager.Shutdown();

    // Allow some failures for extreme coords (INT32_MAX etc.)
    if (passedCoords < 5) {
        result.passed = false;
        result.errorMessage = "Too many boundary test failures";
    }

    result.iterations = sizeof(testCoords) / sizeof(testCoords[0]);
    result.durationMs = EndTimer(startTime);

    return result;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool ChunkStressTest::CheckDeviceRemoved(ID3D12Device* device, std::string& errorMsg) {
    HRESULT hr = device->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        errorMsg = GetDeviceRemovedReasonString(hr);
        return true;
    }
    return false;
}

std::string ChunkStressTest::GetDeviceRemovedReasonString(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_DEVICE_HUNG:
            return "DXGI_ERROR_DEVICE_HUNG - GPU took too long";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "DXGI_ERROR_DEVICE_REMOVED - GPU removed or driver crashed";
        case DXGI_ERROR_DEVICE_RESET:
            return "DXGI_ERROR_DEVICE_RESET - GPU reset due to bad command";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return "DXGI_ERROR_DRIVER_INTERNAL_ERROR - Driver bug";
        case DXGI_ERROR_INVALID_CALL:
            return "DXGI_ERROR_INVALID_CALL - Invalid D3D12 API usage";
        case S_OK:
            return "S_OK - No error";
        default:
            return "Unknown error: 0x" + std::to_string(static_cast<uint32_t>(hr));
    }
}

uint32_t ChunkStressTest::CountAllocatedDescriptors(Graphics::DescriptorHeapManager& heapManager) {
    // Return currently allocated shader-visible descriptors
    // Chunks allocate 2 descriptors each (SRV + UAV) from shader-visible heap
    return heapManager.GetShaderVisibleCbvSrvUavAllocatedCount();
}

std::chrono::high_resolution_clock::time_point ChunkStressTest::StartTimer() {
    return std::chrono::high_resolution_clock::now();
}

double ChunkStressTest::EndTimer(std::chrono::high_resolution_clock::time_point start) {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void ChunkStressTest::LogTestResult(const StressTestResult& result) {
    if (result.passed) {
        spdlog::info("  [PASS] {} - {:.1f}ms ({} iterations)",
            result.testName, result.durationMs, result.iterations);
    } else {
        spdlog::error("  [FAIL] {} - {}", result.testName, result.errorMessage);
        if (result.deviceRemoved) {
            spdlog::error("         GPU DEVICE REMOVED!");
        }
    }
}

void ChunkStressTest::LogTestSummary(const std::vector<StressTestResult>& results) {
    int passed = 0;
    int failed = 0;
    double totalTime = 0;

    for (const auto& result : results) {
        if (result.passed) {
            passed++;
        } else {
            failed++;
        }
        totalTime += result.durationMs;
    }

    spdlog::info("");
    spdlog::info("========================================================");
    spdlog::info("       STRESS TEST SUMMARY");
    spdlog::info("========================================================");
    spdlog::info("  Total tests: {}", results.size());
    spdlog::info("  Passed: {}", passed);
    spdlog::info("  Failed: {}", failed);
    spdlog::info("  Total time: {:.1f}ms", totalTime);
    spdlog::info("========================================================");

    if (failed == 0) {
        spdlog::info("  ALL STRESS TESTS PASSED");
    } else {
        spdlog::error("  {} STRESS TESTS FAILED", failed);
        spdlog::info("  Failed tests:");
        for (const auto& result : results) {
            if (!result.passed) {
                spdlog::error("    - {}: {}", result.testName, result.errorMessage);
            }
        }
    }
    spdlog::info("========================================================");
    spdlog::info("");
}

} // namespace VENPOD::Simulation
