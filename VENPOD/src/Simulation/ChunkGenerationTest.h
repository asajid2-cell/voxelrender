#pragma once

// =============================================================================
// VENPOD Chunk Generation Test - Isolated testing for infinite chunks
// =============================================================================

#include <d3d12.h>
#include "../Graphics/RHI/DX12Device.h"
#include "../Graphics/RHI/DX12CommandQueue.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "../Utils/Result.h"

namespace VENPOD::Simulation {

// Test harness for chunk generation
class ChunkGenerationTest {
public:
    // Run all chunk generation tests
    // Returns true if all tests pass, false otherwise
    static bool RunAllTests(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager
    );

    // Test 1: Generate single chunk and verify data
    static bool TestSingleChunkGeneration(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager
    );

    // Test 2: Verify chunk boundary seamlessness
    static bool TestChunkBoundaries(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager
    );

    // Test 3: Verify world coordinate mapping
    static bool TestWorldCoordinates(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        Graphics::DescriptorHeapManager& heapManager
    );

private:
    // Helper: Read GPU buffer back to CPU
    static Result<void> ReadbackGPUBuffer(
        Graphics::DX12Device& device,
        Graphics::DX12CommandQueue& commandQueue,
        ID3D12Resource* gpuResource,
        void* destBuffer,
        uint64_t bufferSize
    );

    // Helper: Analyze voxel data statistics
    static void AnalyzeVoxelData(const uint32_t* voxelData, uint32_t voxelCount);
};

} // namespace VENPOD::Simulation
