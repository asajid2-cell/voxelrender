#pragma once

// =============================================================================
// VENPOD Chunk Stress Test - Memory, threading, and crash testing
// Comprehensive testing for infinite chunk system stability
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include "../Graphics/RHI/DX12Device.h"
#include "../Graphics/RHI/DX12CommandQueue.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// Test result structure for detailed reporting
struct StressTestResult {
    std::string testName;
    bool passed;
    std::string errorMessage;
    double durationMs;
    uint32_t iterations;

    // Memory metrics
    uint64_t peakMemoryUsage;
    uint32_t descriptorsAllocated;
    uint32_t descriptorsLeaked;

    // GPU metrics
    uint32_t gpuFaults;
    bool deviceRemoved;
};

// Configuration for stress tests
struct StressTestConfig {
    // Test intensity (1=light, 2=normal, 3=heavy)
    uint32_t intensity = 2;

    // Maximum test duration per test (ms)
    uint32_t maxDurationMs = 30000;

    // Number of iterations for cycling tests
    uint32_t cycleIterations = 300;

    // Whether to abort on first failure
    bool abortOnFirstFailure = false;

    // Enable verbose logging
    bool verbose = true;
};

class ChunkStressTest {
public:
    // Run all stress tests with default config
    static bool RunAllStressTests(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager
    );

    // Run all stress tests with custom config
    static bool RunAllStressTests(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Get detailed results from last test run
    static const std::vector<StressTestResult>& GetLastResults() { return s_lastResults; }

    // =========================================================================
    // Individual Test Functions
    // =========================================================================

    // Test 1: Descriptor heap exhaustion
    // Attempts to allocate more chunks than descriptor heap can handle
    // Verifies graceful failure and proper cleanup
    static StressTestResult TestDescriptorExhaustion(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 2: Rapid chunk load/unload cycling
    // Simulates camera movement through world, loading/unloading chunks rapidly
    // Tests cache invalidation and memory management
    static StressTestResult TestRapidChunkCycling(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 3: Command allocator ring buffer stress
    // Rapidly generates chunks to test ring buffer allocator reuse
    // Verifies fence synchronization prevents allocator corruption
    static StressTestResult TestCommandAllocatorReuse(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 4: Resource state transition validation
    // Tests all possible state transitions for voxel buffers
    // Catches invalid transition sequences
    static StressTestResult TestResourceStateTransitions(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 5: GPU fence synchronization
    // Stresses fence signaling and waiting to detect race conditions
    static StressTestResult TestFenceSynchronization(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 6: Memory leak detection
    // Allocates and deallocates chunks repeatedly, monitoring descriptor counts
    static StressTestResult TestMemoryLeaks(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 7: Double-buffered cache consistency
    // Verifies cache tracking across buffer swaps
    static StressTestResult TestCacheConsistency(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // Test 8: Boundary conditions and edge cases
    // Tests negative coordinates, INT32 overflow, chunk at world edges
    static StressTestResult TestBoundaryConditions(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager,
        const StressTestConfig& config
    );

    // NOTE: Test 9 (Concurrent Operations) and Test 10 (Long Duration Stability)
    // are not yet implemented. The system is currently single-threaded for chunk
    // operations, so concurrency testing is not applicable. Long-duration testing
    // should be done via manual testing or extended playthroughs.

private:
    // =========================================================================
    // Helper Functions
    // =========================================================================

    // Check if D3D12 device was removed (GPU crash detection)
    static bool CheckDeviceRemoved(ID3D12Device* device, std::string& errorMsg);

    // Get device removed reason as string
    static std::string GetDeviceRemovedReasonString(HRESULT hr);

    // Count currently allocated descriptors (requires heap manager support)
    static uint32_t CountAllocatedDescriptors(Graphics::DescriptorHeapManager& heapManager);

    // Start timing a test
    static std::chrono::high_resolution_clock::time_point StartTimer();

    // End timing and return duration in milliseconds
    static double EndTimer(std::chrono::high_resolution_clock::time_point start);

    // Print test result to log
    static void LogTestResult(const StressTestResult& result);

    // Print summary of all test results
    static void LogTestSummary(const std::vector<StressTestResult>& results);

    // Storage for last test results
    static std::vector<StressTestResult> s_lastResults;
};

} // namespace VENPOD::Simulation
