#pragma once

// =============================================================================
// VENPOD Physics Dispatcher - Orchestrates compute shader execution
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include "../Graphics/RHI/DX12ComputePipeline.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "../Graphics/RHI/DescriptorHeap.h"
#include "VoxelWorld.h"
#include "ChunkManager.h"
#include "../Utils/Result.h"
#include "../Input/BrushController.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Simulation {

// Physics constants passed to compute shaders
struct PhysicsConstants {
    uint32_t gridSizeX;
    uint32_t gridSizeY;
    uint32_t gridSizeZ;
    uint32_t frameIndex;

    float deltaTime;
    float gravity;
    uint32_t simulationFlags;
    uint32_t padding;
};

// Chunk scan constants
struct ChunkScanConstants {
    uint32_t gridSizeX;
    uint32_t gridSizeY;
    uint32_t gridSizeZ;
    uint32_t frameIndex;

    uint32_t chunkCountX;
    uint32_t chunkCountY;
    uint32_t chunkCountZ;
    uint32_t chunkSize;

    uint32_t sleepThreshold;
    // PRIORITY 3: Active region offset for 4×4×4 optimization
    int32_t activeRegionOffsetX;  // Camera chunk X - 1 (start of active region)
    int32_t activeRegionOffsetY;  // Camera chunk Y - 1
    int32_t activeRegionOffsetZ;  // Camera chunk Z - 1
};

// Chunk-based physics constants
struct PhysicsChunkConstants {
    uint32_t gridSizeX;
    uint32_t gridSizeY;
    uint32_t gridSizeZ;
    uint32_t frameIndex;

    float deltaTime;
    float gravity;
    uint32_t simulationFlags;
    uint32_t chunkSize;

    uint32_t chunkCountX;
    uint32_t chunkCountY;
    uint32_t chunkCountZ;
    uint32_t padding;
};

class PhysicsDispatcher {
public:
    PhysicsDispatcher() = default;
    ~PhysicsDispatcher() = default;

    // Non-copyable
    PhysicsDispatcher(const PhysicsDispatcher&) = delete;
    PhysicsDispatcher& operator=(const PhysicsDispatcher&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        Graphics::DescriptorHeapManager& heapManager,
        const std::filesystem::path& shaderPath
    );

    void Shutdown();

    // Execute physics simulation for one frame
    void DispatchPhysics(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        float deltaTime,
        uint32_t frameIndex
    );

    // Execute physics simulation using ExecuteIndirect on active chunks only
    void DispatchPhysicsIndirect(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        ChunkManager& chunkManager,
        float deltaTime,
        uint32_t frameIndex
    );

    // Initialize voxel world with test pattern
    void DispatchInitialize(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        uint32_t seed = 0
    );

    // Apply brush painting to voxel world
    void DispatchBrush(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        const Input::BrushConstants& brushConstants
    );

    // Scan chunks to determine which are active (have moving particles)
    void DispatchChunkScan(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        ChunkManager& chunkManager,
        uint32_t frameIndex
    );

    // GPU brush raycasting (NEW - replaces CPU-side DDA)
    void DispatchBrushRaycast(
        ID3D12GraphicsCommandList* cmdList,
        VoxelWorld& world,
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDirection
    );

    // Get command signature for indirect dispatch
    ID3D12CommandSignature* GetCommandSignature() const { return m_commandSignature.Get(); }

private:
    Result<void> CreateInitializePipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateGravityPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateBrushPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateChunkScanPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreatePrepareIndirectPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateGravityChunkPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateBrushRaycastPipeline(
        ID3D12Device* device,
        Graphics::ShaderCompiler& shaderCompiler,
        const std::filesystem::path& shaderPath
    );

    Result<void> CreateCommandSignature(ID3D12Device* device);

    // Compute pipelines
    Graphics::DX12ComputePipeline m_initializePipeline;
    Graphics::DX12ComputePipeline m_gravityPipeline;
    Graphics::DX12ComputePipeline m_brushPipeline;
    Graphics::DX12ComputePipeline m_chunkScanPipeline;
    Graphics::DX12ComputePipeline m_prepareIndirectPipeline;
    Graphics::DX12ComputePipeline m_gravityChunkPipeline;
    Graphics::DX12ComputePipeline m_brushRaycastPipeline;

    // Command signature for indirect dispatch
    ComPtr<ID3D12CommandSignature> m_commandSignature;

    // Shader-visible descriptors for current frame
    Graphics::DescriptorHeapManager* m_heapManager = nullptr;
    ID3D12Device* m_device = nullptr;

    // Physics settings
    float m_gravity = 9.8f;
    uint32_t m_simulationFlags = 0;
    uint32_t m_sleepThreshold = 30;  // Frames before chunk sleeps
};

} // namespace VENPOD::Simulation
